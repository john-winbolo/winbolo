/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Join
 *Filename:      udp_server_join.c
 *Author:        John Morrison
 *Purpose:
 *  The server transport's join handshake and admission
 *  control, split out of transport_udp_server.c.
 *    - The address-proof cookie exchange a joiner must
 *      echo before any slot is allocated.
 *    - The per-source-IP join rate limit that bounds the
 *      unproven half of that exchange.
 *    - Connection-id allocation, and the slot lookups
 *      keyed on connection id and on source address.
 *    - Name-collision resolution between verified and
 *      unverified joiners: preempt, provisional admit,
 *      and the -unverified suffix pool.
 *    - The JOIN_REQUEST handler itself.
 *********************************************************/

#include <stdio.h>   /* fprintf, snprintf, stderr */
#include <stdlib.h>  /* getenv, strtoll */
#include <string.h>  /* memcpy, memset, strncpy, strcmp, strlen */

#include "transport_udp_internal.h"        /* packHeader, packU32, unpackU32,
                                            * PACKET_HEADER_SIZE, SDL_GetTicks */
#include "transport_udp_server_internal.h" /* udpServer, srvSendTo, JoinRateEntry,
                                            * WBN_JOIN_REGISTER_GRACE_TICKS, and the
                                            * statics transport_udp_server.c still owns */
#include "transport_udp.h"   /* WbnJoinState, UdpServerClient, JoinCollisionVerdict,
                              * ClaimResolveAction, JOIN_FLAG_*, WBN_JOIN_KEY_WIRE_LEN */
#include "netpacks.h"        /* JOIN_COOKIE_LEN, PACKET_JOIN_*, PACKET_MAX_PLAYER_NAME */
#include "global.h"          /* BYTE, MAX_TANKS, MAP_STR_SIZE, PLAYER_NAME_LEN */
#include "players.h"         /* playersGetClientFlags/AccountFlags/CountryCode,
                              * PLAYER_FLAG_*, PLAYER_CLIENT_HINT_MASK, CLIENT_TYPE_* */
#include "messages.h"        /* globalMessage */
#include "game_sim.h"        /* GameSim, and via lang.h the STR_ and NETERR_ message
                              * ids plus MessageArgs */
#include "server_sim.h"      /* ServerSim, ServerState, serverSimGetState and the
                              * rest of the sim accessors the handler reads */
#include "server_sim_internal.h" /* serverSimSetShadowCulled,
                                  * serverSimShadowSeedRoundStart,
                                  * serverSimGetCompressedMapFor — sim co-owner */
#include "server_sim_join.h" /* serverSimFindFreeSlot, addPlayerInternal,
                              * setClientTypeFlagsInternal, fillAndPublishPlayerJoin,
                              * serverSimPromoteHostOnJoin */
#include "server_sim_lifecycle.h" /* lobbyAutoUnreadyOnChange */
#include "control_event.h"   /* ControlEvent, CTRL_BALANCE_PROPOSAL */
#include "channel_mux.h"     /* channelMuxInit */
#include "bulk_transfer.h"   /* bulkSenderInit, bulkReceiverInit */
#include "playername_validate.h" /* playerNameValidate, playerNameCompare,
                                  * playerNameMakeUnverifiedSuffix */
#include "../geolookup.h"    /* geoLookupCountry */
#include "../../common/md5.h" /* Md5Ctx, md5Init/Update/Final/Compute — hmacMd5 */
#include "../../common/wb_log.h"      /* WB_LOG_* */
#include "../../common/mp_diag_log.h" /* mpDiagLog */
#include "../sim/server_sim_shared.h"  /* serverSimAnnounce */
#include "../../winbolonet/winbolonet_core.h"   /* winbolonetIsRunning, winbolonetAddEvent,
                                                 * WINBOLONET_KEY_LEN */
#include "../../winbolonet/winbolonet_server.h" /* winboloNetVerifyClientKey,
                                                 * winboloNetVerifySpectatorKey */

/* OS cryptographic RNG, used to seed the address-proof cookie secret. Kept
 * below the headers above: bcrypt.h uses the Windows base types without
 * including them itself, so it has to follow the WinSock2.h/windows.h that
 * platform_net.h pulls in through transport_udp_internal.h. */
#if defined(_WIN32)
#  include <bcrypt.h>
#elif defined(__linux__)
#  include <sys/random.h>  /* getrandom */
#  include <fcntl.h>       /* open, O_RDONLY (/dev/urandom fallback) */
#  include <unistd.h>      /* read, close */
#else
#  include <stdlib.h>      /* arc4random_buf (macOS/BSD) */
#endif

/* ── Deferred WBN PLAYER_JOIN pure core (declared in transport_udp.h) ─
 * Value-only sequencing so the join/reauth/grace/disconnect logic is
 * unit-testable without sockets or the WBN HTTP layer. */
void wbnJoinArm(WbnJoinState *s, uint32_t nowTick, uint32_t graceTicks) {
    s->pending = true;
    s->deadlineTick = nowTick + graceTicks;
}

bool wbnJoinOnReauth(WbnJoinState *s, bool wasParticipant) {
    s->pending = false;
    return !wasParticipant;
}

bool wbnJoinOnTick(WbnJoinState *s, uint32_t nowTick) {
    /* Wrap-safe compare: nowTick - deadlineTick >= 0 once reached. */
    if (s->pending && (int32_t)(nowTick - s->deadlineTick) >= 0) {
        s->pending = false;
        return true;
    }
    return false;
}

void wbnJoinClear(WbnJoinState *s) {
    s->pending = false;
}

bool wbnRekeyTargetSelected(bool connected, bool wbnWasVerified) {
    return connected && wbnWasVerified;
}

JoinCollisionVerdict joinCollisionDecide(bool incomingWillAuth,
                                         bool existingIsVerified) {
    if (existingIsVerified) return JOIN_COLLISION_REJECT_VERIFIED;
    if (!incomingWillAuth)  return JOIN_COLLISION_REJECT_IN_USE;
    return JOIN_COLLISION_ADMIT_PROVISIONAL;
}

ClaimResolveAction claimResolveDecide(bool bareNameHeld, bool holderIsVerified) {
    if (!bareNameHeld)    return CLAIM_RESOLVE_PROMOTE_FREE;
    if (holderIsVerified) return CLAIM_RESOLVE_KEEP_TEMP;
    return CLAIM_RESOLVE_PREEMPT_SQUATTER;
}

/* Per-source-IP JOIN rate limit. Keys on the source IP alone (a spoofer can
 * walk source ports), tracking a small LRU of recent sources. Scope: this
 * contains port-walking from a *single* source IP; it does NOT rate-limit a
 * flood from random/spoofed source IPs, which gets a fresh bucket per packet
 * and keeps evicting the LRU. That's acceptable — the address-proof cookie
 * prevents slot exhaustion regardless of flood shape, and the challenge it
 * draws is smaller than the JOIN (de-amplifying). */
#define JOIN_RL_BURST       5     /* token-bucket capacity per source IP */
#define JOIN_RL_REFILL_MS   2000  /* +1 token every 2 s */

/* Find client slot by address. Returns player index or -1. */
int serverFindClient(const struct sockaddr_in *addr) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected &&
            udpServer.clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            udpServer.clients[i].addr.sin_port == addr->sin_port) {
            return i;
        }
    }
    return -1;
}

/* Token-bucket join rate limit keyed on source IP (port ignored: a spoofer
 * walks ports). Returns true and consumes a token if allowed; false if the
 * source is over-rate. LRU-evicts the least-recently-seen source on overflow.
 * A real join never originates from 0.0.0.0, so reusing srcAddr==0 as the
 * empty-entry sentinel can't collide with a legitimate source.
 *
 * This throttles port-walking from one source IP; it does not throttle a
 * random-source-IP flood (each packet lands in a fresh bucket and evicts the
 * LRU). The cookie gate is what actually prevents slot exhaustion, so that
 * residual is acceptable. */
static bool serverJoinRateLimitAllow(const struct sockaddr_in *fromAddr) {
    uint32_t key = fromAddr->sin_addr.s_addr;
    uint64_t now = SDL_GetTicks();
    JoinRateEntry *match = NULL;
    JoinRateEntry *empty = NULL;
    JoinRateEntry *oldest = NULL;
    JoinRateEntry *e;
    int i;

    for (i = 0; i < JOIN_RL_MAX_SOURCES; i++) {
        JoinRateEntry *cur = &udpServer.joinRate[i];
        if (cur->srcAddr == key) { match = cur; break; }
        if (cur->srcAddr == 0) {
            if (empty == NULL) empty = cur;
        } else if (oldest == NULL || cur->lastMs < oldest->lastMs) {
            oldest = cur;
        }
    }

    if (match != NULL) {
        e = match;
        /* Refill whole tokens for the elapsed time, capped at the burst, and
         * advance lastMs by the consumed whole windows so the sub-window
         * remainder still counts toward the next refill. */
        if (e->tokens < JOIN_RL_BURST) {
            uint64_t elapsed = now - e->lastMs;
            uint64_t refill  = elapsed / JOIN_RL_REFILL_MS;
            if (refill > 0) {
                if (refill > JOIN_RL_BURST - e->tokens) {
                    refill = JOIN_RL_BURST - e->tokens;
                }
                e->tokens += (uint32_t)refill;
                e->lastMs += refill * JOIN_RL_REFILL_MS;
            }
        }
    } else {
        /* Fresh source: take an empty slot, else evict the least-recently
         * seen one. A new bucket starts full. */
        e = (empty != NULL) ? empty : oldest;
        e->srcAddr = key;
        e->tokens  = JOIN_RL_BURST;
        e->lastMs  = now;
    }

    if (e->tokens == 0) {
        return false;
    }
    e->tokens--;
    return true;
}

/* ── Address-proof retry cookie (anti-spoof) ─────────────────────────────
 * Before a JOIN is allowed to allocate a slot the joiner must echo a cookie
 * the server can recompute for its source address.  The cookie is an HMAC of
 * (address ‖ port ‖ time-window) under a per-process secret, so it is
 * stateless on the server: a blind/IP-spoofed JOIN can't produce one without
 * receiving the challenge at the real address.  The cookie is server-only —
 * the client never computes it, it just stores and echoes the opaque bytes. */
#define COOKIE_WINDOW_SEC 16

/* Fill buf with n bytes from the OS cryptographic RNG. Returns true on
 * success. Deliberately NOT the project's randombytes() (a no-op tweetnacl
 * linkage stub) — the cookie secret must come from a real CSPRNG. */
static bool serverFillRandomBytes(uint8_t *buf, size_t n) {
#if defined(_WIN32)
    /* STATUS_SUCCESS == 0. */
    return BCryptGenRandom(NULL, buf, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
    {
        ssize_t got = getrandom(buf, n, 0);
        if (got == (ssize_t)n) {
            return true;
        }
        /* Old kernel / ENOSYS, or a short read: fall back to /dev/urandom. */
        {
            int fd = open("/dev/urandom", O_RDONLY);
            size_t off = 0;
            if (fd < 0) {
                return false;
            }
            while (off < n) {
                ssize_t r = read(fd, buf + off, n - off);
                if (r <= 0) {
                    close(fd);
                    return false;
                }
                off += (size_t)r;
            }
            close(fd);
            return true;
        }
    }
#else
    arc4random_buf(buf, n);
    return true;
#endif
}

/* Per-process secret keying the cookies. Filled once on first use from the
 * OS CSPRNG so it is unpredictable to a remote attacker: even an attacker who
 * observes a valid (addr,port,window,cookie) tuple from an honest handshake
 * can't recover the secret or forge cookies for a spoofed address. Never
 * leaves the process.
 *
 * Fails closed: if no RNG path succeeds, the secret is left unseeded and this
 * returns NULL so callers refuse to issue or accept cookies, rather than
 * keying them off a guessable value (a predictable secret would defeat the
 * whole anti-spoof feature). A later call retries the RNG. */
static const uint8_t *serverCookieSecret(void) {
    static uint8_t secret[32];
    static bool seeded = false;
    if (!seeded) {
        if (serverFillRandomBytes(secret, sizeof(secret))) {
            seeded = true;
        } else {
            WB_LOG_ERROR(WB_LOG_CAT_NET,
                "cookie secret: no OS CSPRNG available; refusing to key "
                "address-proof cookies from a predictable source");
            return NULL;
        }
    }
    return secret;
}

/* Standard HMAC (RFC 2104) over MD5: 64-byte block, ipad/opad. The cookie is
 * an address proof, not a confidentiality primitive — MD5's break doesn't help
 * an attacker forge one without the secret, and it avoids a new crypto dep. */
static void hmacMd5(const uint8_t *key, size_t keyLen,
                    const uint8_t *msg, size_t msgLen, uint8_t out[16]) {
    uint8_t k0[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[16];
    Md5Ctx ctx;
    size_t i;

    /* Block-pad the key. A key longer than the block would be hashed first;
     * our secret is a fixed 32 bytes so that arm is effectively unused. */
    memset(k0, 0, sizeof(k0));
    if (keyLen > sizeof(k0)) {
        md5Compute(key, keyLen, k0);
    } else {
        memcpy(k0, key, keyLen);
    }
    for (i = 0; i < sizeof(k0); i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36);
        opad[i] = (uint8_t)(k0[i] ^ 0x5c);
    }
    md5Init(&ctx);
    md5Update(&ctx, ipad, sizeof(ipad));
    md5Update(&ctx, msg, msgLen);
    md5Final(inner, &ctx);

    md5Init(&ctx);
    md5Update(&ctx, opad, sizeof(opad));
    md5Update(&ctx, inner, sizeof(inner));
    md5Final(out, &ctx);
}

/* The current cookie time-window.
 * Test-only clock seam: WB_COOKIE_WINDOW_OFFSET shifts the window counter so a
 * test can simulate cookie expiry without waiting real time. Default 0; unset
 * in production. Security-neutral — an attacker can't set a server-side env var
 * remotely and holds no secret, so a shifted window only affects the server's
 * own consistent issue/accept (worst case a self-inflicted reject). Read per
 * call (not cached) so a test can advance the clock mid-run. */
uint64_t serverCookieCurrentWindow(void) {
    uint64_t w = (SDL_GetTicks() / 1000ULL) / COOKIE_WINDOW_SEC;
    const char *off = getenv("WB_COOKIE_WINDOW_OFFSET");
    if (off != NULL) w += (uint64_t)strtoll(off, NULL, 10);
    return w;
}

/* HMAC-MD5 of (sin_addr ‖ sin_port ‖ window) under the per-process secret.
 * Byte order is irrelevant for security — the secret never leaves the process,
 * so only self-consistency between issue and accept matters. Returns false
 * (out untouched) when the CSPRNG secret is unavailable, so the caller fails
 * closed instead of computing a cookie under a missing key. */
bool serverCookieCompute(const struct sockaddr_in *addr, uint64_t window,
                         uint8_t out[JOIN_COOKIE_LEN]) {
    const uint8_t *secret = serverCookieSecret();
    uint8_t msg[4 + 2 + 8];
    if (secret == NULL) {
        return false;
    }
    memcpy(msg + 0, &addr->sin_addr.s_addr, 4);
    memcpy(msg + 4, &addr->sin_port, 2);
    memcpy(msg + 6, &window, 8);
    hmacMd5(secret, 32, msg, sizeof(msg), out);
    return true;
}

/* Accept a JOIN cookie for the current window or the one before it (so a
 * cookie issued near a boundary still validates). Constant-time compare:
 * OR-accumulate the byte diffs so a partial match can't be timed byte by byte.
 * A NULL cookie (none echoed) never validates. */
static bool serverCookieAccept(const struct sockaddr_in *addr,
                               const uint8_t *cookieOrNull) {
    uint64_t w;
    uint8_t expect[JOIN_COOKIE_LEN];
    int diff;
    int i;

    if (cookieOrNull == NULL) return false;
    w = serverCookieCurrentWindow();

    /* No secret (CSPRNG unavailable) → fail closed: nothing validates. */
    if (!serverCookieCompute(addr, w, expect)) return false;
    diff = 0;
    for (i = 0; i < JOIN_COOKIE_LEN; i++) diff |= expect[i] ^ cookieOrNull[i];
    if (diff == 0) return true;

    if (!serverCookieCompute(addr, w - 1, expect)) return false;
    diff = 0;
    for (i = 0; i < JOIN_COOKIE_LEN; i++) diff |= expect[i] ^ cookieOrNull[i];
    return diff == 0;
}

/* Issue a fresh challenge: [header PACKET_JOIN_CHALLENGE][cookie for window w].
 * Smaller than a JOIN_REQUEST, so it can't amplify a spoofed source. */
static void serverSendJoinChallenge(const struct sockaddr_in *addr) {
    uint8_t buf[PACKET_HEADER_SIZE + JOIN_COOKIE_LEN];
    packHeader(buf, PACKET_JOIN_CHALLENGE, 0);
    /* Fail closed: with no secret we can't issue a valid challenge, so send
     * nothing rather than a cookie keyed off a missing/guessable secret. */
    if (!serverCookieCompute(addr, serverCookieCurrentWindow(),
                             buf + PACKET_HEADER_SIZE)) {
        return;
    }
    srvSendTo(buf, sizeof(buf), addr);
}

/* connId rides the wire as two 32-bit halves through the existing packU32
 * helpers — low half first, then high. Client send/read must agree with these. */
void packConnId(uint8_t *buf, uint64_t connId) {
    packU32(buf,     (uint32_t)(connId & 0xffffffffULL));
    packU32(buf + 4, (uint32_t)(connId >> 32));
}
uint64_t unpackConnId(const uint8_t *buf) {
    uint32_t lo = unpackU32(buf);
    uint32_t hi = unpackU32(buf + 4);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Generate a per-session connection id. SplitMix64 over its own state, seeded
 * once from the high-resolution performance counter — deliberately independent
 * of the process-global bolo_rand stream so a join never perturbs the
 * deterministic sim sequence (baseline replays depend on that stream). Never
 * returns 0, which the wire reserves for "no connId". */
uint64_t serverNextConnId(void) {
    static uint64_t state;
    static bool seeded = false;
    uint64_t z;
    if (!seeded) {
        state = (uint64_t)SDL_GetPerformanceCounter();
        state ^= 0x9e3779b97f4a7c15ULL * (uint64_t)SDL_GetTicks();
        seeded = true;
    }
    state += 0x9e3779b97f4a7c15ULL;
    z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    return z ? z : 0x9e3779b97f4a7c15ULL;
}

/* Resolve the slot owning an inbound INPUT by its connection id (see header). */
int transportUdpServerFindByConnId(const UdpServerClient *clients,
                                   uint64_t connId,
                                   const struct sockaddr_in *fromAddr,
                                   bool *outRehome) {
    int i;
    if (outRehome) *outRehome = false;
    if (connId == 0 || clients == NULL) return -1;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!clients[i].connected || clients[i].connId != connId) {
            continue;
        }
        if (outRehome &&
            (clients[i].addr.sin_addr.s_addr != fromAddr->sin_addr.s_addr ||
             clients[i].addr.sin_port != fromAddr->sin_port)) {
            *outRehome = true;
        }
        return i;
    }
    return -1;
}

/* Pack a langid + arg list into buf at *pos.  Used by the localized
 * server→client packets (PACKET_JOIN_REJECT and the fromPlayer=0xFF
 * variant of PACKET_CHAT_BROADCAST).  Args are written verbatim as
 * UTF-8 byte strings, each preceded by a length byte (0..PLAYER_NAME_LEN-1).
 * Returns false (and leaves *pos undefined) if argCount > 4 or any arg
 * exceeds the per-arg byte cap. */
bool packLocalizedPayload(uint8_t *buf, int *pos, int bufSize,
                          langid id, int argCount,
                          const char *const args[]) {
    int i;
    if (argCount < 0 || argCount > 4) return false;
    if (*pos + 3 > bufSize) return false;
    /* langid: 2 bytes big-endian */
    buf[(*pos)++] = (uint8_t)((id >> 8) & 0xFF);
    buf[(*pos)++] = (uint8_t)(id & 0xFF);
    buf[(*pos)++] = (uint8_t)argCount;
    for (i = 0; i < argCount; i++) {
        const char *a = (args && args[i]) ? args[i] : "";
        size_t aLen = strlen(a);
        if (aLen > PLAYER_NAME_LEN - 1) return false;
        if (*pos + 1 + (int)aLen > bufSize) return false;
        buf[(*pos)++] = (uint8_t)aLen;
        if (aLen > 0) {
            memcpy(buf + *pos, a, aLen);
            *pos += (int)aLen;
        }
    }
    return true;
}

/* Send a localized join reject to a specific address.  Wire format is
 * documented at PACKET_JOIN_REJECT in netpacks.h. */
void serverSendJoinReject(const struct sockaddr_in *addr, langid id,
                          int argCount, const char *const args[]) {
    /* Worst case: 8 hdr + 2 langid + 1 argCount + 4*(1 + 64) = 271. */
    uint8_t buf[PACKET_HEADER_SIZE + 3 + 4 * (1 + PLAYER_NAME_LEN - 1)];
    int pos = PACKET_HEADER_SIZE;
    {
        struct in_addr ia = addr->sin_addr;
        WB_LOG_INFO(WB_LOG_CAT_NET,
            "join reject: dest=%s:%u langid=%u argc=%d",
            inet_ntoa(ia),
            (unsigned)ntohs(addr->sin_port),
            (unsigned)id, argCount);
    }
    packHeader(buf, PACKET_JOIN_REJECT, 0);
    if (!packLocalizedPayload(buf, &pos, sizeof(buf), id, argCount, args)) {
        WB_LOG_ERROR(WB_LOG_CAT_NET,
            "serverSendJoinReject: pack failed id=%u argc=%d",
            (unsigned)id, argCount);
        fprintf(stderr,
                "[UDP SERVER] serverSendJoinReject: pack failed id=%u argc=%d\n",
                (unsigned)id, argCount);
        return;
    }
    /* wire-only: per-client handshake (response to a single client's request) */
    srvSendTo(buf, pos, addr);
}

/* Build and send the join accept packet with the slot, current server
 * tick, and compressed map size. */
void serverSendJoinAccept(int slot, ServerSim *sim,
                          const struct sockaddr_in *addr) {
    uint8_t acceptBuf[PACKET_HEADER_SIZE + 9 + 8];
    int pos;

    packHeader(acceptBuf, PACKET_JOIN_ACCEPT,
               udpServer.clients[slot].outSequence++);
    pos = PACKET_HEADER_SIZE;
    acceptBuf[pos++] = (uint8_t)slot;
    packU32(acceptBuf + pos, serverSimGetTick(sim));
    pos += 4;
    packU32(acceptBuf + pos, udpServer.compressedMapSize);
    pos += 4;
    /* connId the client echoes on its INPUT packets for NAT-rebind re-homing. */
    packConnId(acceptBuf + pos, udpServer.clients[slot].connId);
    pos += 8;

    /* wire-only: per-client handshake (response to a single client's request) */
    srvSendTo(acceptBuf, pos, addr);
}

/* Phase 5 verified-priority preempt — rename `victimSlot` to `chosenName`
 * and broadcast the change to all connected clients.  Sets the
 * sticky-suffix flag so the slot keeps its renamed form for the rest of
 * the session.  `incomingName` and `incomingCountry` describe the
 * verified joiner that triggered the rename; both are used for the
 * newswire announce. */
void serverPreemptRename(ServerSim *sim, int victimSlot,
                         const char *chosenName,
                         const char *incomingName,
                         const char *incomingCountry) {
    GameSim *gs = serverSimGetGameSim(sim);
    char originalName[PACKET_MAX_PLAYER_NAME];

    strncpy(originalName, udpServer.clients[victimSlot].playerName,
            PACKET_MAX_PLAYER_NAME - 1);
    originalName[PACKET_MAX_PLAYER_NAME - 1] = '\0';

    /* Update the per-slot transport-side name. */
    snprintf(udpServer.clients[victimSlot].playerName,
             PACKET_MAX_PLAYER_NAME, "%s", chosenName);
    udpServer.clients[victimSlot].nameStickySuffix = true;

    /* Update the gameSim player record and publish the name change —
     * same path PACKET_NAME_CHANGE uses. */
    serverSimSetPlayerName(sim, (BYTE)victimSlot, chosenName);

    /* Publish a single-slot lobby update so other surfaces (lobby
     * table, players panel) refresh. */
    serverSimPublishLobbySlot(sim, (BYTE)victimSlot);

    /* Post a newswire announcement. The server's messageAdd callback
     * drops newswire messages today (see server_sim.c:serverSimCbMessageAdd),
     * but we mirror the existing emit pattern for consistency and so a
     * future server-side listener picks it up automatically.  Clients
     * generate their own MESSAGE_CHANGENAME announce when they apply
     * PACKET_NAME_CHANGE, so the visible chat update on each client comes
     * from that path; the verified-specific phrasing is informational
     * for now (Phase 11 wires it to chat). */
    {
        MessageArgs args;
        memset(&args, 0, sizeof(args));
        strncpy(args.playerName, originalName, PLAYER_NAME_LEN - 1);
        strncpy(args.otherName, incomingName, PLAYER_NAME_LEN - 1);
        args.playerFlags = playersGetAccountFlags(&gs->plyrs,
                                                  (BYTE)victimSlot);
        playersGetCountryCode(&gs->plyrs, (BYTE)victimSlot,
                              args.playerCountry);
        /* Incoming joiner has a slot allocated but isn't in the players
         * struct yet; pull country from the caller-supplied lookup and
         * mark the WBN flag manually since we only get here when they're
         * verified. */
        args.otherFlags = (uint8_t)PLAYER_FLAG_WBN_VERIFIED;
        if (incomingCountry) {
            args.otherCountry[0] = incomingCountry[0];
            args.otherCountry[1] = incomingCountry[1];
            args.otherCountry[2] = '\0';
        }
        gs->callbacks.messageAdd(gs->callbacks.ctx,
                                 globalMessage, MESSAGE_NEWSWIRE,
                                 STR_NAME_RENAMED_BY_VERIFIED, &args);
    }

    /* Phase 5.1: surface the announcement to clients via the server-message
     * broadcast path (the messageAdd callback above drops on the server). */
    {
        const char *renameArgs[2];
        renameArgs[0] = originalName;
        renameArgs[1] = incomingName;
        serverSendServerMessage(sim, STR_NAME_RENAMED_BY_VERIFIED, 2, renameArgs);
    }

    {
        char consoleMsg[256];
        snprintf(consoleMsg, sizeof(consoleMsg),
                 "Player '%s' renamed to '%s' (verified player '%s' joined)",
                 originalName, chosenName, incomingName);
        serverSimConsoleMessage(consoleMsg);
    }
}

/* Choose a unique "<baseName>-unverified[-N]" name, skipping index 1
 * (the bare "-unverified" form IS the "1").  The candidate must not
 * collide with any connected slot other than excludeSlot.  Returns true
 * and writes the chosen name into out (capacity outLen) on success;
 * returns false when the suffix pool (indices 0, 2..99) is exhausted. */
bool serverChooseUnverifiedSuffix(const char *baseName,
                                  int excludeSlot,
                                  char *out, size_t outLen) {
    int suffixIdx;
    /* Try indices 0, 2, 3, ..., 99 (1 is reserved — the bare
     * "-unverified" form IS the "1"). */
    for (suffixIdx = 0; suffixIdx <= 99; suffixIdx++) {
        if (suffixIdx == 1) continue;
        char candidate[PACKET_MAX_PLAYER_NAME];
        if (!playerNameMakeUnverifiedSuffix(baseName, suffixIdx,
                                            candidate,
                                            sizeof(candidate))) {
            continue;
        }

        /* Candidate must be unique against ALL other connected
         * slots (not just the excluded slot). */
        bool clash = false;
        int k;
        for (k = 0; k < MAX_TANKS; k++) {
            if (!udpServer.clients[k].connected) continue;
            if (k == excludeSlot) continue;
            if (playerNameCompare(udpServer.clients[k].playerName,
                                  candidate) == 0) {
                clash = true;
                break;
            }
        }
        if (clash) continue;

        strncpy(out, candidate, outLen - 1);
        out[outLen - 1] = '\0';
        return true;
    }
    return false;
}

/* Handle a join request from a new client */
void serverHandleJoinRequest(const uint8_t *buf, int len,
                             const struct sockaddr_in *fromAddr,
                             ServerSim *sim) {
    int pos = PACKET_HEADER_SIZE;
    char name[PACKET_MAX_PLAYER_NAME];
    char pass[MAP_STR_SIZE];
    char wbnJoinKey[WBN_JOIN_KEY_WIRE_LEN];
    int slot;

    WB_LOG_DEBUG(WB_LOG_CAT_NET,
        "join request from %s:%u len=%d",
        inet_ntoa(fromAddr->sin_addr),
        (unsigned)ntohs(fromAddr->sin_port), len);
#ifndef WB_FUZZ
    fprintf(stderr, "[UDP SERVER] Join request received, len=%d\n", len);
#endif
    /* Full JOIN_REQUEST payload after header: name + pass + 3 version bytes
     * + WBN token + 1 flags byte + 2 client-identity bytes (clientType,
     * clientHints).  No backward-compat path — older clients are rejected. */
    int joinReqMin = pos + PACKET_MAX_PLAYER_NAME + MAP_STR_SIZE + 3
                     + WBN_JOIN_KEY_WIRE_LEN + 1 + 2;
    if (len < joinReqMin) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "join request malformed: need=%d got=%d from=%s:%u",
            joinReqMin, len,
            inet_ntoa(fromAddr->sin_addr),
            (unsigned)ntohs(fromAddr->sin_port));
#ifndef WB_FUZZ
        fprintf(stderr, "[UDP SERVER] Join request malformed (need %d, got %d)\n",
                joinReqMin, len);
#endif
        return; /* Malformed */
    }

    /* Already connected from this address? */
    if (serverFindClient(fromAddr) >= 0) {
        /* Resend accept in case they missed it */
        slot = serverFindClient(fromAddr);
        WB_LOG_DEBUG(WB_LOG_CAT_NET,
            "join from already-connected slot=%d, resending accept", slot);
        serverSendJoinAccept(slot, sim, fromAddr);
        /* Resend the rekey alongside the accept — recovers the rare case
         * where the original JOIN_ACCEPT was delivered but the trailing
         * REKEY wasn't, which would otherwise leave the client without
         * a wbnServerKey until the next return-to-lobby rotation. */
        transportUdpServerSendWbnRekey(&udpServer.clients[slot]);
        /* No map re-poke needed: an incomplete download is still armed/in-flight
         * on CHANNEL_BULK and the channel retransmits its own unacked segments.
         * A download this client can no longer complete (it missed the stream's
         * head, or the transfer already finished into a wiped receiver) is
         * recovered by its PACKET_MAP_DL_READY re-ask, not here. */
        return;
    }

    memcpy(name, buf + pos, PACKET_MAX_PLAYER_NAME);
    name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    pos += PACKET_MAX_PLAYER_NAME;

    /* Validate the requested name before any other check.  The validator
     * returns the NFC-normalized, stripped, codepoint-safe-truncated form
     * which then becomes the canonical name we accept and store. */
    {
        char validated[PACKET_MAX_PLAYER_NAME];
        PlayerNameValidationError nameErr = PLAYER_NAME_OK;
        if (!playerNameValidate(name, validated, sizeof(validated), &nameErr)) {
            char consoleMsg[160];
            snprintf(consoleMsg, sizeof(consoleMsg),
                     "Join rejected for '%s': Invalid player name (code %d)",
                     name, (int)nameErr);
            serverSimConsoleMessage(consoleMsg);
            serverSendJoinReject(fromAddr, STR_REJECT_INVALID_PLAYER_NAME, 0, NULL);
            return;
        }
        strncpy(name, validated, PACKET_MAX_PLAYER_NAME - 1);
        name[PACKET_MAX_PLAYER_NAME - 1] = '\0';
    }

    memcpy(pass, buf + pos, MAP_STR_SIZE);
    pass[MAP_STR_SIZE - 1] = '\0';
    pos += MAP_STR_SIZE;

    /* Version gate: require an exact protocol-version triple match against
     * the version this server was compiled with.  A self-built or stale
     * client sending a different triple is rejected here — before the
     * password check — so it gets the version error rather than a
     * misleading password failure.  The three bytes keep their fixed wire
     * offset so even an old client's JOIN stays parseable for rejection. */
    {
        uint8_t cliMajor = buf[pos];
        uint8_t cliMinor = buf[pos + 1];
        uint8_t cliRev   = buf[pos + 2];
        pos += 3;
        if (cliMajor != BOLO_VERSION_MAJOR ||
            cliMinor != BOLO_VERSION_MINOR ||
            cliRev   != BOLO_VERSION_REVISION) {
            char serverVer[16];
            char clientVer[16];
            char consoleMsg[160];
            const char *args[4];
            snprintf(serverVer, sizeof(serverVer), "%u.%u.%u",
                     (unsigned)BOLO_VERSION_MAJOR,
                     (unsigned)BOLO_VERSION_MINOR,
                     (unsigned)BOLO_VERSION_REVISION);
            snprintf(clientVer, sizeof(clientVer), "%u.%u.%u",
                     (unsigned)cliMajor, (unsigned)cliMinor, (unsigned)cliRev);
            snprintf(consoleMsg, sizeof(consoleMsg),
                     "Join rejected for '%s': Version mismatch "
                     "(server %s, client %s)", name, serverVer, clientVer);
            serverSimConsoleMessage(consoleMsg);
            /* The 1389 string renders {string1}=server, {string2}=client.
             * The client decode fills string1/string2 from arg slots 2/3
             * (slots 0/1 are the player/other name, unused here), so pass
             * the two versions in slots 2 and 3 with empty leading args. */
            args[0] = "";
            args[1] = "";
            args[2] = serverVer;
            args[3] = clientVer;
            serverSendJoinReject(fromAddr, STR_REJECT_VERSION_MISMATCH, 4, args);
            return;
        }
    }

    /* Read WBN token if present (backwards compatible — older clients won't send it) */
    memset(wbnJoinKey, 0, WBN_JOIN_KEY_WIRE_LEN);
    if (len >= pos + WBN_JOIN_KEY_WIRE_LEN) {
        memcpy(wbnJoinKey, buf + pos, WBN_JOIN_KEY_WIRE_LEN);
        wbnJoinKey[WBN_JOIN_KEY_WIRE_LEN - 1] = '\0';
        pos += WBN_JOIN_KEY_WIRE_LEN;
    }

    /* Read flags byte if present (backwards compatible — older clients default to 0) */
    bool wantRejoin = false;
    bool incomingWillAuth = false;
    bool isSpectator = false;
    if (len > pos) {
        wantRejoin       = (buf[pos] & JOIN_FLAG_WANT_REJOIN) != 0;
        incomingWillAuth = (buf[pos] & JOIN_FLAG_WILL_AUTHENTICATE) != 0;
        isSpectator      = (buf[pos] & JOIN_FLAG_SPECTATOR) != 0;
        pos++;
    }

    /* Read clientType + clientHints (length already gated above). */
    uint8_t clientType  = buf[pos++];
    uint8_t clientHints = buf[pos++];
    if (clientType >= CLIENT_TYPE_COUNT) clientType = CLIENT_TYPE_UNKNOWN;
    /* Drop reserved/server-only bits — clients are never trusted to set
     * WBN_VERIFIED or WBN_STEAM_LINKED. */
    clientHints &= PLAYER_CLIENT_HINT_MASK;

    /* Optional trailing fallbackCountry (2 bytes). Old clients won't
     * send it — leave empty in that case. Used below as the GeoIP-failed
     * fallback so loopback and private-LAN joiners can supply their own
     * cached country code without the server reading its own WBN cache. */
    char wireFallbackCountry[3];
    wireFallbackCountry[0] = '\0';
    wireFallbackCountry[1] = '\0';
    wireFallbackCountry[2] = '\0';
    if (len >= pos + 2) {
        wireFallbackCountry[0] = (char)buf[pos++];
        wireFallbackCountry[1] = (char)buf[pos++];
    }

    /* Optional trailing address-proof cookie. Old/initial JOINs omit it
     * (NULL) and draw a challenge below; a returning JOIN echoes the 16
     * bytes from a prior PACKET_JOIN_CHALLENGE. */
    const uint8_t *joinCookie = NULL;
    if (len >= pos + JOIN_COOKIE_LEN) {
        joinCookie = buf + pos;
        pos += JOIN_COOKIE_LEN;
    }

    /* Address proof, gated before the password / game-lock / full checks: a
     * slot is allocated only after the joiner echoes a valid retry cookie, and
     * the password and game-locked rejects below run only for a proven address
     * so they can't be reflected to a spoofed source. The version reject
     * earlier is intentionally left ahead of this gate — an old, cookie-
     * incapable client must get a clean version reject rather than a silent
     * timeout (that residual reflection is de-amplifying — the version reject
     * precedes this gate, so it is not itself rate-limited).
     *
     * The cookie — not the rate limiter — is what prevents slot exhaustion, so
     * an unproven JOIN (no/stale cookie) is the only thing the per-source-IP
     * rate limit guards: it bounds the cheap challenge/reflection path. A
     * proven (valid-cookie) JOIN is never rate-limited, so many legitimate
     * clients behind one NAT or public IP still join promptly. Drop an
     * over-rate unproven JOIN silently — a reply would reflect to a
     * possibly-spoofed source — otherwise issue a fresh challenge (smaller
     * than the JOIN, so it can't amplify). */
    if (!serverCookieAccept(fromAddr, joinCookie)) {
        if (!serverJoinRateLimitAllow(fromAddr)) {
            WB_LOG_DEBUG(WB_LOG_CAT_NET,
                "join rate-limited from %s:%u",
                inet_ntoa(fromAddr->sin_addr),
                (unsigned)ntohs(fromAddr->sin_port));
            return;
        }
        serverSendJoinChallenge(fromAddr);
        return;
    }

    /* Check password */
    {
        const char *expected = serverSimGetPassword(sim);
        if (expected[0] != '\0' && strcmp(pass, expected) != 0) {
            char consoleMsg[128];
            snprintf(consoleMsg, sizeof(consoleMsg),
                     "Join rejected for '%s': Incorrect password", name);
            serverSimConsoleMessage(consoleMsg);
            serverSendJoinReject(fromAddr, STR_REJECT_INCORRECT_PASSWORD, 0, NULL);
            return;
        }
    }

    /* Join as viewer: no tank slot, no sim player, no control-bus
     * subscription.  All shared pre-checks above (cookie, version, name,
     * password) have run; the player game-lock below does not gate
     * spectating — a locked or running game stays watchable. */
    if (isSpectator) {
        /* Attribute-only WBN check: verify the spectator_key when one was sent
         * and WBN is running. A failure (or WBN down / empty key) never blocks
         * spectating — the viewer is admitted anonymously with no badge. */
        char spectatorKey[WINBOLONET_KEY_LEN];
        uint8_t wbnFlags = 0;
        spectatorKey[0] = '\0';
        /* The guard below cannot currently be true, for the same reason the
         * player verify's cannot: a spectator_key is minted against the server
         * key, and a viewer is given an empty one — gamefront.c passes "" for
         * both the API token and the server key on the spectator connect, and
         * the mint in transport_udp_client.c requires a non-empty server key.
         * So wbnJoinKey rides the spectator JOIN empty and every viewer is
         * admitted anonymously.  Before any handshake change lets a spectator
         * arrive with a key, this call has to become a job on the WinBolo.net
         * worker the way the reauth's verify is: it is a synchronous HTTPS
         * round trip and this is the tick thread. */
        if (winbolonetIsRunning() && wbnJoinKey[0] != '\0') {
            char errorMsg[512];
            bool loggedIn = false;
            errorMsg[0] = '\0';
            if (winboloNetVerifySpectatorKey(wbnJoinKey, name, errorMsg, &loggedIn)) {
                strncpy(spectatorKey, wbnJoinKey, WINBOLONET_KEY_LEN - 1);
                spectatorKey[WINBOLONET_KEY_LEN - 1] = '\0';
                if (loggedIn) wbnFlags |= PLAYER_FLAG_WBN_VERIFIED;
                fprintf(stderr,
                        "[UDP SERVER] Spectator '%s' verified with WinBolo.net "
                        "(logged_in=%d)\n", name, (int)loggedIn);
            } else {
                fprintf(stderr,
                        "[UDP SERVER] Spectator '%s' WBN verify failed: %s. "
                        "Joining anonymously.\n",
                        name, errorMsg[0] ? errorMsg : "(no detail)");
            }
        }
        /* Resolve the viewer's country the same way the player path does
         * (:incomingCountry): GeoIP first, then the client-supplied
         * fallbackCountry, empty when neither resolves. */
        char specCountry[3];
        {
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &fromAddr->sin_addr, ipStr, sizeof(ipStr));
            if (!geoLookupCountry(ipStr, specCountry)) {
                specCountry[0] = wireFallbackCountry[0];
                specCountry[1] = wireFallbackCountry[1];
            }
            specCountry[2] = '\0';
        }
        serverAcceptSpectator(sim, fromAddr, name, clientType, clientHints,
                              spectatorKey, wbnFlags, specCountry);
        return;
    }

    /* Check game lock: server admin command OR host toggled
     * "Allow New Players: Now" off (mirrored as !sim->allowNewPlayers). */
    if (udpServer.gameLocked || !serverSimIsAcceptingJoins(sim)) {
        char consoleMsg[128];
        snprintf(consoleMsg, sizeof(consoleMsg),
                 "Join rejected for '%s': Game is locked", name);
        serverSimConsoleMessage(consoleMsg);
        serverSendJoinReject(fromAddr, STR_REJECT_GAME_LOCKED, 0, NULL);
        return;
    }

    /* Find free slot for the incoming player.  Slot allocation moved
     * ahead of the duplicate check (Phase 5) so the WBN-verification
     * step below has a slot to bind its player_key to, and the collision
     * policy has the slot available before applying any preempt. */
    slot = serverSimFindFreeSlot(sim, false);
    if (slot < 0) {
        serverSimConsoleMessage("Join rejected: Server full");
        serverSendJoinReject(fromAddr, STR_REJECT_SERVER_FULL, 0, NULL);
        return;
    }

    /* Clear any stale provisional-claim state before the duplicate-search
     * loop (which sets it for a will-auth provisional admit).  A slot freed
     * by the map-serialize failure path below leaves connected=false without
     * routing through serverDisconnectClient, so the claim would otherwise
     * persist and mis-route the next reuser's reauth. */
    udpServer.clients[slot].claimPending = false;
    udpServer.clients[slot].claimDesiredName[0] = '\0';

    /* Country resolution for the incoming player.  Done early so the
     * preempt path can include it in the rename newswire. Uniform
     * across loopback / private-LAN / public-WAN joiners: GeoIP first,
     * then the client-supplied fallbackCountry if GeoIP can't resolve
     * the address. The host self-join over loopback supplies its own
     * cached WBN country via clientSimConnectUdp; private-LAN joiners
     * supply whatever they cached. Empty stays empty for non-WBN
     * old clients. */
    char incomingCountry[3];
    {
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &fromAddr->sin_addr, ipStr, sizeof(ipStr));
        if (!geoLookupCountry(ipStr, incomingCountry)) {
            incomingCountry[0] = wireFallbackCountry[0];
            incomingCountry[1] = wireFallbackCountry[1];
        }
        incomingCountry[2] = '\0';
    }

    /* Verify WBN token if provided. Verification failure is no longer
     * fatal — the client simply joins as a non-WBN player and forfeits
     * WBN-mediated features (Balance from WBN, ranked credit, ladder
     * placement). This keeps a game playable when winbolo.net is
     * unreachable or returns transient errors, instead of locking
     * everyone out of the lobby. The collision policy below already
     * treats !incomingIsWBN as the lower-priority class. */
    bool incomingIsWBN = false;
    bool wbnHasSteam = false;
    bool wbnIsSupporter = false;
    /* The guard below cannot currently be true: wbnJoinKey rides the JOIN
     * empty, because minting a player_key needs the server key and a client
     * is given an empty one at every clientSimConnectUdp call site — it
     * learns the key from the PACKET_WBN_REKEY that follows JOIN_ACCEPT.
     * Before any handshake change lets a key ride the JOIN, this call has to
     * become a job on the WinBolo.net worker, the way the reauth's verify is:
     * it is a synchronous HTTPS round trip and this is the tick thread.
     *
     * Such a change also has to reckon with incomingWillAuth, which comes from
     * JOIN_FLAG_WILL_AUTHENTICATE and is set from the client's wbnApiToken
     * alone — so a key and that flag can disagree.  A key arriving with the
     * flag clear breaks two things.  The join announcement below publishes an
     * immediate anonymous PLAYER_JOIN, and the verify result then publishes a
     * second keyed one on an arm that was never raised; the hold that keeps
     * those two apart covers only the grace sweep, and this announcement does
     * not go through it.  And the joiner's collision drops from an immediate
     * preempt to JOIN_COLLISION_REJECT_IN_USE, which turns it away instead of
     * seating it. */
    if (wbnJoinKey[0] != '\0' && winbolonetIsRunning()) {
        char errorMsg[512];
        errorMsg[0] = '\0';
        if (winboloNetVerifyClientKey(wbnJoinKey, name, (BYTE)slot, errorMsg,
                                      &wbnHasSteam, &wbnIsSupporter)) {
            fprintf(stderr, "[UDP SERVER] Player '%s' verified with WinBolo.net\n", name);
            incomingIsWBN = true;
        } else {
            /* Degrade to non-WBN join instead of rejecting outright.
             * The original "WinBolo.net verification failed: <reason>"
             * message is extended with "Proceeding without WBN.net
             * features" so the host's console explains both halves
             * (what broke, and that the lobby keeps running anyway). */
            char failMsg[256];
            snprintf(failMsg, sizeof(failMsg),
                     "WinBolo.net verification failed: %s. Proceeding "
                     "without WBN.net features.", errorMsg);
            fprintf(stderr, "[UDP SERVER] %s (player='%s')\n", failMsg, name);
            serverSimConsoleMessage(failMsg);
            incomingIsWBN = false;
        }
    }

    /* Verified-priority collision policy (Phase 5).  Replaces the older
     * single-rule "Name already in use" check.  Loop terminates on the
     * first match (existing duplicate-loop semantics). */
    {
        int i;
        for (i = 0; i < MAX_TANKS; i++) {
            if (!udpServer.clients[i].connected) continue;
            if (i == slot) continue; /* slot is unconnected; defensive */
            if (playerNameCompare(udpServer.clients[i].playerName, name) != 0)
                continue;

            bool existingIsWBN =
                (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, (BYTE)i)
                 & PLAYER_FLAG_WBN_VERIFIED) != 0;

            /* The joiner is always the unverified class here.  A client
             * does not hold the server key at join: clientSimConnectUdp is
             * given an empty one at every call site, and the key a player_key
             * is minted against only arrives afterwards, on the
             * PACKET_WBN_REKEY the server sends once the join is accepted.
             * So wbnJoinKey rides the JOIN empty, the verify above never runs
             * and incomingIsWBN is never true.  A joiner's WinBolo.net
             * identity is established later, by the reauth that follows the
             * rekey, and the provisional claim recorded below is what carries
             * the name it asked for across to that point.
             *
             * Consult the pure verdict core. */
            JoinCollisionVerdict verdict =
                joinCollisionDecide(incomingWillAuth, existingIsWBN);

            if (verdict == JOIN_COLLISION_REJECT_VERIFIED) {
                /* Unverified joiner can't take a verified player's name. */
                char consoleMsg[160];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "Join rejected for '%s': name taken by verified player",
                         name);
                serverSimConsoleMessage(consoleMsg);
                serverSendJoinReject(fromAddr, STR_NAME_TAKEN_BY_VERIFIED, 0, NULL);
                return;
            }
            if (verdict == JOIN_COLLISION_REJECT_IN_USE) {
                /* Both unverified (incl. Steam, bot): existing behavior. */
                char consoleMsg[128];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "Join rejected for '%s': Name already in use", name);
                serverSimConsoleMessage(consoleMsg);
                serverSendJoinReject(fromAddr, STR_DLGSETNAME_INUSE_ERR, 0, NULL);
                return;
            }

            /* JOIN_COLLISION_ADMIT_PROVISIONAL — a will-authenticate joiner
             * whose desired bare name is held by an unverified squatter.
             * Seat it under a temporary -unverified[-N] name keyed off the
             * squatter slot and record the pending claim so reauth can
             * promote it to the bare name. */
            char tempName[PACKET_MAX_PLAYER_NAME];
            if (!serverChooseUnverifiedSuffix(udpServer.clients[i].playerName, i,
                                              tempName, sizeof(tempName))) {
                /* Suffix pool exhausted — reject as at the join-time preempt.
                 * This joiner is !incomingIsWBN, so there is no WBN-recorded
                 * join to roll back. */
                char consoleMsg[200];
                snprintf(consoleMsg, sizeof(consoleMsg),
                         "WARNING: -unverified suffix pool exhausted for "
                         "'%s'; rejecting provisional joiner", name);
                serverSimConsoleMessage(consoleMsg);
                serverSendJoinReject(fromAddr,
                                     STR_REJECT_NAME_POOL_EXHAUSTED, 0, NULL);
                return;
            }

            /* Record the pending claim on the joiner's slot (not the
             * squatter's): the desired bare name is the validated incoming
             * name, resolved at reauth. */
            udpServer.clients[slot].claimPending = true;
            snprintf(udpServer.clients[slot].claimDesiredName,
                     sizeof(udpServer.clients[slot].claimDesiredName), "%s", name);

            /* Seat the provisional joiner under the temp name; the bare name is
             * recorded above and claimed at reauth.  Overwriting the local name
             * makes all downstream seating use the temp name. */
            snprintf(name, sizeof(name), "%s", tempName);
            break; /* terminate the duplicate-search loop on first match */
        }
    }

    /* Accept the player */
    udpServer.clients[slot].connected = true;
    /* This transport now owns the slot, so it owns that slot's copy of the
     * terrain too: the drain below sends it only the changes inside its
     * viewports and writes the copy as it sends. Cleared again by
     * serverDisconnectClient (and by the reject path just below). */
    serverSimSetShadowCulled(sim, (BYTE)slot, true);
    udpServer.clients[slot].connId = serverNextConnId();
    udpServer.clients[slot].nameStickySuffix = false;
    udpServer.clients[slot].addr = *fromAddr;
    udpServer.clients[slot].playerNum = (uint8_t)slot;
    snprintf(udpServer.clients[slot].playerName,
             PACKET_MAX_PLAYER_NAME, "%s", name);
    udpServer.clients[slot].lastReceivedTick = udpServer.tickCount;
    udpServer.clients[slot].outSequence = 1;
    udpServer.clients[slot].inboundCmdSeq = 0;
    udpServer.clients[slot].lastPingTime = udpServer.tickCount;
    udpServer.clients[slot].pingMs = 0;
    udpServer.clients[slot].wantRejoin = wantRejoin;

    /* Persist the early GeoIP lookup result. */
    udpServer.clients[slot].countryCode[0] = incomingCountry[0];
    udpServer.clients[slot].countryCode[1] = incomingCountry[1];
    udpServer.clients[slot].countryCode[2] = '\0';
    udpServer.clients[slot].clientType  = clientType;
    udpServer.clients[slot].clientHints = clientHints;

    /* Initialize the reliable map event queue for this client */
    udpServer.mapEventQueues[slot].nextSeq = 1;
    udpServer.mapEventQueues[slot].ackedSeq = 1;
    memset(udpServer.mapEventQueues[slot].buffer, 0, sizeof(udpServer.mapEventQueues[slot].buffer));
    /* The drop count belongs to this connection, so a client taking over the
     * slot does not inherit the previous occupant's drops. */
    udpServer.mapEventQueueDrops[slot] = 0;
    udpServer.mapChannelStalled[slot] = false;
    udpServer.mapGen[slot] = 0;

    /* Bring up this slot's parallel channel mux alongside the queues. */
    channelMuxInit(&udpServer.channelMux[slot]);
    udpServer.channelFramesRx[slot] = 0;
    bulkSenderInit(&udpServer.bulkSend[slot]);
    bulkReceiverInit(&udpServer.bulkRecvUp[slot]);
    udpServerResetRoundLogLimits(slot);
    udpServerResetMapReaskLimit(slot);

    /* Merge client-supplied hints with server-determined WBN trust into a
     * single clientFlags byte, then run the four-step join sequence so a
     * single CTRL_PLAYER_JOIN fans out with name, country, clientType,
     * and clientFlags all populated. */
    {
        uint8_t flags = clientHints & PLAYER_CLIENT_HINT_MASK;
        if (incomingIsWBN)                  flags |= PLAYER_FLAG_WBN_VERIFIED;
        if (incomingIsWBN && wbnHasSteam)   flags |= PLAYER_FLAG_WBN_STEAM_LINKED;
        if (incomingIsWBN && wbnIsSupporter) flags |= PLAYER_FLAG_SUPPORTER;
        /* Durable cross-round signal for the rekey-rotation gate: set it
         * definitively here (true for a WBN joiner, false otherwise) so a
         * non-WBN client reusing a slot can't inherit a stale true. */
        udpServer.clients[slot].wbnWasVerified = incomingIsWBN;
        udpServer.clients[slot].wbnWebIdentityCached = false;
        udpServer.clients[slot].wbnWebIsLoggedIn = false;
        udpServer.clients[slot].wbnWebName[0] = '\0';
        udpServer.clients[slot].wbnWebCountry[0] = '\0';
        udpServer.clients[slot].wbnWebUserId = -1;
        addPlayerInternal(sim, (BYTE)slot,
                          udpServer.clients[slot].playerName,
                          udpServer.clients[slot].countryCode,
                          udpServer.clients[slot].wantRejoin);
        setClientTypeFlagsInternal(sim, (BYTE)slot, clientType, flags);
        fillAndPublishPlayerJoin(sim, (BYTE)slot);
        /* Before the subscriber registers below, so the replay this client
         * gets already names the slot holding the host role. */
        serverSimPromoteHostOnJoin(sim, (BYTE)slot);
    }
    WB_LOG_INFO(WB_LOG_CAT_NET,
                "join accept: slot=%d clientType=%u clientHints=0x%02x",
                slot, (unsigned)clientType, (unsigned)clientHints);

    /* Joining a game already in progress downloads the map the round started
     * on, not the map as it stands now, so arriving (or leaving and rejoining)
     * shows nothing about what has happened since. The add above seeded this
     * slot's copy from the live map — correct for an in-process client, which
     * takes every change — so restart it from the round-start terrain here and
     * the blob below carries that. The joiner is paid the differences by the
     * catch-up sweep as its viewports cover the ground. A lobby or countdown
     * joiner keeps the current map: nothing has changed it yet, and there is no
     * running sweep to settle a difference with. */
    if (serverSimGetState(sim) == serverStateRunning) {
        serverSimShadowSeedRoundStart(sim, (BYTE)slot);
    }

    /* Compress this slot's copy of the map for the joining player.
     * Done after addPlayerInternal so rejoin ownership is included — the
     * copy the blob is taken from is the one the slot's snapshot checksum is
     * taken over, so the two always describe the same tiles. */
    {
        int mapLen = serverSimGetCompressedMapFor(sim, (BYTE)slot,
                                                  udpServer.compressedMap,
                                                  (int)sizeof(udpServer.compressedMap));
        if (mapLen <= 0) {
            serverSimRemovePlayer(sim, (BYTE)slot);
            udpServer.clients[slot].connected = false;
            serverSimSetShadowCulled(sim, (BYTE)slot, false);
            serverSendJoinReject(fromAddr, NETERR_MAPSERIALIZE, 0, NULL);
            return;
        }
        udpServer.compressedMapSize = (uint32_t)mapLen;
    }

    /* Send accept with game settings and map size */
    {
        char consoleMsg[128];
        snprintf(consoleMsg, sizeof(consoleMsg),
                 "New Player '%s' accepted into game.", name);
        serverSimConsoleMessage(consoleMsg);
    }
    fprintf(stderr, "[UDP SERVER] Player '%s' assigned slot %d, mapSize=%u\n",
            name, slot, udpServer.compressedMapSize);
    serverSendJoinAccept(slot, sim, fromAddr);
    /* Hand the joiner the current WBN server_key so it can mint a
     * player_key and re-auth via the lobby-snapshot path.  Gated inside
     * the send function — no-op on non-WBN servers. */
    transportUdpServerSendWbnRekey(&udpServer.clients[slot]);

    /* Subscribe this client to the server's control-event bus so
     * future events can be encoded and unicast to it via the codec
     * table.  Register's sync-replay walks the lobby settings, every
     * connected slot, and every player-join and feeds them through
     * the codec encoder to this client's socket — replacing the old
     * composite PACKET_LOBBY_STATE handshake.
     *
     * The replay burst is queued onto CHANNEL_CONTROL by udpClientDeliverControl
     * and carried by the channel's own framing — the snapshot trailer during
     * running, a standalone PACKET_CHANNEL otherwise — so no explicit post-burst
     * flush is needed.  controlSyncInProgress is still bracketed here for the
     * dormant queue machinery; the channel ignores it. */
    mpDiagLog("[srv] SYNC START slot=%d phase=%d (about to register subscriber + replay)",
              slot, (int)serverSimGetState(sim));
    udpServer.controlSyncInProgress[slot] = true;
    udpServer.clients[slot].controlSub =
        serverSimRegisterSubscriber(sim, udpClientDeliverControl,
                                    &udpServer.clients[slot]);
    udpServer.controlSyncInProgress[slot] = false;
    mpDiagLog("[srv] SYNC END slot=%d phase=%d (replay queued onto CHANNEL_CONTROL)",
              slot, (int)serverSimGetState(sim));
    /* Flush the coalesced replay burst now when outside running (mirrors the
     * per-event eager flush the sync guard suppressed); during running the
     * next snapshot trailer carries it. */
    if (serverSimGetState(sim) != serverStateRunning) {
        transportUdpServerFlushChannel(slot);
    }

    /* Announce the join to WBN.  If the slot's key already rode the JOIN
     * field and verified inline (incomingIsWBN), the player is already a
     * participant — register keyed right now.  Otherwise, when WBN is
     * running, a will-authenticate joiner defers: the rekey we sent above
     * prompts a reauth that fills the key and fires a keyed join
     * (transportUdpServerHandleWbnReauth); if no reauth lands within the
     * grace window the per-tick sweep in transportUdpServerCheckTimeouts
     * fires an anonymous one.  A not-signed-in joiner owes no reauth, so
     * its anonymous join fires now.  On a non-WBN server there is nothing
     * to defer (and winbolonetAddEvent is a no-op anyway). */
    wbnJoinClear(&udpServer.clients[slot].wbnJoin);
    if (incomingIsWBN) {
        winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                           (BYTE)slot, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
    } else if (winbolonetIsRunning()) {
        if (incomingWillAuth) {
            /* Signed-in joiner: a reauth is coming.  Arm the anonymous
             * fallback so a never-landing reauth still announces the join. */
            wbnJoinArm(&udpServer.clients[slot].wbnJoin,
                       udpServer.tickCount, WBN_JOIN_REGISTER_GRACE_TICKS);
        } else {
            /* Not signed in: no reauth will ever land, so there is nothing
             * to wait for — announce the anonymous join now. */
            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                               (BYTE)slot, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
        }
    }

    if (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown) {
        /* Publish a lobby-slot update for the new player so existing
         * clients pick up the joiner's team/ready/ping fields (the
         * CTRL_PLAYER_JOIN already fanned out from
         * fillAndPublishPlayerJoin carries name/country/clientType, but
         * not the lobby-slot extras). */
        serverSimPublishLobbySlot(sim, (BYTE)slot);
        /* Dismiss any pending balance proposal — player composition changed */
        if (serverSimGetBalanceProposal(sim)->pending) {
            ControlEvent evt;
            serverSimClearBalanceProposal(sim);
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_BALANCE_PROPOSAL;
            serverSimPublishControl(sim, &evt);
        }
    }
    /* No-lobby and mid-game-join PACKET_GAME_START is emitted by the
     * subscriber's sync replay: CTRL_GAME_PHASE_RUNNING flows through
     * the codec encoder to this client's socket as part of
     * serverSimRegisterSubscriber above. */

    /* Initialize map download. The blob is not streamed here: it is armed and
     * begins on CHANNEL_BULK once the channel is idle, carried by the per-tick
     * frame in transportUdpServerSend. No anti-reflection round-trip gates it —
     * the join cookie (serverCookieAccept) already proved this address can
     * receive a reply, so a spoofed JOIN never reaches a slot to be exploited. */
    serverInitMapDownload(slot);

    /* The sync-replay just enqueued a CTRL_PLAYER_JOIN for every in-use
     * player into this client's controlEventQueue, so the JOIN-time
     * roster is covered by the reliable bus path. */

    /* Surface the join in everyone's lobby chat and unready any humans
     * who were ready. The chat line rides CTRL_SERVER_TEXT, which the
     * bus fans to both in-process subscribers and UDP clients via the
     * codec — same path serverSendServerEnglishBroadcast already uses
     * for lock-toggle / ping-enforcement announcements. The unready
     * call is a no-op outside lobby/countdown (no human is ready in
     * running state), so it stays unconditional. */
    /* Newswire-worthy, so the scenario is asked first; a silenced window
     * keeps the join off every lobby chat panel. */
    if (serverSimAnnounce(sim, ANNOUNCE_KIND_JOINED, (BYTE)slot, (BYTE)slot)) {
        char chatMsg[32 + PACKET_MAX_PLAYER_NAME];
        snprintf(chatMsg, sizeof(chatMsg), "%s has joined.",
                 udpServer.clients[slot].playerName);
        serverSendServerEnglishBroadcast(sim, chatMsg);
    }
    lobbyAutoUnreadyOnChange(sim);
}
