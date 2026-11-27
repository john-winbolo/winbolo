/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          UDP Server WinBolo.net
 *Filename:      udp_server_wbn.c
 *Author:        John Morrison
 *Purpose:
 *  The server transport's WinBolo.net-facing work, split
 *  out of transport_udp_server.c.
 *    - Re-keying a connected client's WBN session, both to
 *      a single client and across every slot that was
 *      verified last round.
 *    - Resolving a web joiner's identity from its join
 *      code, and stamping the verified name, country and
 *      flags that come back onto its slot.
 *    - The reauth that follows a rekey, including the
 *      provisional name claim it settles.  Its verify runs on
 *      the WinBolo.net worker — client/verify for a native
 *      slot's player_key, client/verify_join_code for a web
 *      slot's code — so the reauth captures the slot and
 *      returns and the stamp happens on a later tick, out of
 *      udpServerApplyReauthResult.
 *    - Serving a completed round's log: the registered
 *      source the packet handler reads, the per-slot
 *      request limits, and the refusal reply.
 *********************************************************/

#include <stdio.h>   /* snprintf */
#include <string.h>  /* memset, strlen */

#include "transport_udp_internal.h"        /* packHeader, packU32,
                                            * PACKET_HEADER_SIZE */
#include "transport_udp_server_internal.h" /* udpServer, srvSendTo,
                                            * serverPreemptRename,
                                            * serverChooseUnverifiedSuffix,
                                            * WBN_JOIN_REGISTER_GRACE_TICKS */
#include "transport_udp.h"   /* RoundLogSource, UdpServerClient,
                              * WBN_JOIN_KEY_WIRE_LEN, wbnRekeyTargetSelected,
                              * wbnJoinArm, wbnJoinOnReauth,
                              * ClaimResolveAction, claimResolveDecide */
#include "wbn_key_codec.h"   /* wbnKeyEncode */
#include "netpacks.h"        /* PACKET_WBN_REKEY, PACKET_ROUND_LOG_ERR,
                              * PACKET_MAX_PLAYER_NAME */
#include "global.h"          /* BYTE, MAX_TANKS, TRUE, FALSE */
#include "players.h"         /* playersGetClientFlags, playersSetClientFlags,
                              * playersSetClientType, PLAYER_FLAG_*,
                              * PLAYER_CLIENT_HINT_MASK, PLAYER_VOICE_FLAG_MASK,
                              * CLIENT_TYPE_WEB */
#include "game_sim.h"        /* GameSim — serverSimGetGameSim(sim)->plyrs */
#include "server_sim.h"      /* ServerSim, ServerState, serverSimGetState,
                              * serverSimGetGameSim, serverSimPublishLobbySlot,
                              * serverSimSetPlayerName,
                              * serverSimSetPlayerCountry */
#include "playername_validate.h" /* playerNameCompare */
#include "server_sim_join.h" /* serverSimRejoinSignedIn */
#include "../../common/wb_log.h" /* WB_LOG_INFO, WB_LOG_WARN, WB_LOG_CAT_NET */
#include "../../winbolonet/winbolonet_core.h"   /* winbolonetIsRunning,
                                                 * winboloNetGetServerKey,
                                                 * winbolonetAddEvent,
                                                 * WINBOLONET_KEY_LEN,
                                                 * WINBOLO_NET_EVENT_PLAYER_JOIN,
                                                 * WINBOLO_NET_NO_PLAYER */
#include "../../winbolonet/winbolonet_server.h" /* winbolonetQueueVerifyClientKey,
                                                 * winbolonetApplyVerifyResult,
                                                 * winbolonetQueueVerifyJoinCode,
                                                 * winbolonetApplyVerifyJoinCodeResult,
                                                 * winboloNetIsPlayerParticipant */

/* The registered round-log source. Held outside udpServer so a transport
 * create/destroy cycle does not drop the recorder's registration, and read
 * only through here: the transport names no symbol in the recorder, which is
 * what keeps the recorder (and the WinBolo.net upload it needs) out of every
 * target that links this file. Unregistered means no recorder is installed,
 * and the server answers ROUND_LOG_ERR_DISABLED without knowing one exists. */
RoundLogSource s_roundLogSource;
bool           s_roundLogSourceSet = false;

void transportUdpServerSetRoundLogSource(const RoundLogSource *src) {
    if (src == NULL || src->serveEnabled == NULL || src->read == NULL) {
        memset(&s_roundLogSource, 0, sizeof(s_roundLogSource));
        s_roundLogSourceSet = false;
        return;
    }
    s_roundLogSource = *src;
    s_roundLogSourceSet = true;
}

/* Clear one slot's round-log request limits. A disconnect mid-transfer runs
 * this too, so a client that reconnects is not still spending the old
 * occupant's attempts. */
void udpServerResetRoundLogLimits(int idx) {
    udpServer.roundLogReqSeen[idx]     = false;
    udpServer.roundLogLastReqTick[idx] = 0;
    udpServer.roundLogServed[idx]      = 0;
}

/* Send PACKET_WBN_REKEY to a single connected client carrying the current
 * server_key.  Called right after JOIN_ACCEPT so the joiner learns the
 * WBN session key without a credential ever riding the JOIN wire field,
 * and from the broadcast wrapper after each return-to-lobby rotation.
 * Silently no-ops when WBN isn't running or the server has no session
 * key yet, so non-WBN servers (and the JOIN_ACCEPT path on them) pay
 * nothing. */
void transportUdpServerSendWbnRekey(UdpServerClient *c) {
    uint8_t buf[PACKET_HEADER_SIZE + WBN_JOIN_KEY_WIRE_LEN];
    char serverKey[WINBOLONET_KEY_LEN];

    if (!winbolonetIsRunning()) return;

    winboloNetGetServerKey(serverKey);
    if (serverKey[0] == '\0') return;

    packHeader(buf, PACKET_WBN_REKEY, c->outSequence++);
    wbnKeyEncode(buf + PACKET_HEADER_SIZE, serverKey);

    /* wire-only: per-client capability refresh (no in-process audience) */
    srvSendTo(buf, sizeof(buf), &c->addr);
}

/* Defined below near the reauth handler; used here for the web lobby-return
 * path that re-stamps identity instead of sending a REKEY. */
static void udpServerApplyWebIdentity(ServerSim *sim, BYTE slot);

/* Broadcast PACKET_WBN_REKEY to every connected client that was
 * WBN-verified last round, after the server rotates its server_key
 * (post-returnToLobby).  Each client mints a fresh player_key against
 * the new key and re-auths, re-registering for the new session.
 *
 * The gate is the durable per-connection wbnWasVerified bit, NOT the
 * sim-side PLAYER_FLAG_WBN_VERIFIED nor the per-slot WBN key.  The key is
 * out: winbolonetEndSession just wiped every key, so a key-based gate
 * (winboloNetIsPlayerParticipant) would match nobody.  The flag is out
 * too: serverSimReturnToLobby clears PLAYER_FLAG_WBN_VERIFIED on every
 * slot earlier in this same tick (it means "verified for the current
 * session", and the session was just torn down), so a flag-based gate
 * would likewise match nobody and silently strand every player un-keyed
 * for the new round.  wbnWasVerified lives in the transport client struct,
 * untouched by the sim reset, so it survives as the cross-round signal.
 * Re-arm the deferred-join state for each rekeyed slot so the incoming
 * reauth fires a fresh keyed PLAYER_JOIN for the new game (or the grace
 * sweep an anonymous one if the reauth never lands). */
void transportUdpServerBroadcastWbnRekey(ServerSim *sim) {
    int i;
    if (!winbolonetIsRunning()) return;
    for (i = 0; i < MAX_TANKS; i++) {
        bool wasVerified = udpServer.clients[i].wbnWasVerified;
        if (!wbnRekeyTargetSelected(udpServer.clients[i].connected, wasVerified))
            continue;
        if (udpServer.clients[i].clientType == CLIENT_TYPE_WEB) {
            /* A web slot can't mint a fresh player_key and won't re-present its
             * single-use join_code, so its identity is re-stamped directly from
             * the cached join-code result (it survives the sim reset, like
             * wbnWasVerified) rather than via a reauth round-trip. */
            if (udpServer.clients[i].wbnWebIdentityCached &&
                udpServer.clients[i].wbnWebIsLoggedIn) {
                udpServerApplyWebIdentity(sim, (BYTE)i);
                if (serverSimGetState(sim) == serverStateLobby ||
                    serverSimGetState(sim) == serverStateCountdown) {
                    serverSimPublishLobbySlot(sim, (BYTE)i);
                }
                winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                   (BYTE)i, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
            }
            /* Still send the REKEY so the web client learns the rotated
             * server_key: it adopts the key and notifies JS (to keep the
             * shareable /join/<key> URL on the live game) but does NOT reauth,
             * so there is no wbnJoinArm here — identity was just re-stamped. */
            transportUdpServerSendWbnRekey(&udpServer.clients[i]);
            continue;
        }
        transportUdpServerSendWbnRekey(&udpServer.clients[i]);
        wbnJoinArm(&udpServer.clients[i].wbnJoin,
                   udpServer.tickCount, WBN_JOIN_REGISTER_GRACE_TICKS);
    }
}

/* Stamp a logged-in WEB slot's verified identity onto the sim + transport slot
 * from its cached join-code result.  The WBN-resolved name and country are
 * AUTHORITATIVE: a web client presents only a join_code, and the verify call
 * (unlike the native player_key route) never sends a name for the backend to
 * bind against — so the server, not the client, decides the verified display
 * name.  Without this a valid code could be paired with any spoofed name under
 * PLAYER_FLAG_WBN_VERIFIED.  serverSimSetPlayerName / serverSimSetPlayerCountry
 * publish CTRL_PLAYER_NAME / PLAYER_JOIN so the change fans out to every client.
 * Caller must hold a cached, logged-in identity. */
static void udpServerApplyWebIdentity(ServerSim *sim, BYTE slot) {
    uint8_t flags = udpServer.clients[slot].clientHints & PLAYER_CLIENT_HINT_MASK;
    flags |= PLAYER_FLAG_WBN_VERIFIED;  /* WEB joiners carry no Steam/supporter */
    /* Carry the mic bits across the rebuild. The client sends them once when
     * they change, so anything dropped here is gone for the rest of the
     * connection rather than re-reported on the next tick. */
    flags |= (uint8_t)(playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs,
                                             slot) & PLAYER_VOICE_FLAG_MASK);
    playersSetClientFlags(&serverSimGetGameSim(sim)->plyrs, slot, flags);
    udpServer.clients[slot].wbnWasVerified = true;
    playersSetClientType(&serverSimGetGameSim(sim)->plyrs, slot,
                         udpServer.clients[slot].clientType);

    if (udpServer.clients[slot].wbnWebName[0] != '\0') {
        serverSimSetPlayerName(sim, slot, udpServer.clients[slot].wbnWebName);
        snprintf(udpServer.clients[slot].playerName, PACKET_MAX_PLAYER_NAME,
                 "%s", udpServer.clients[slot].wbnWebName);
        udpServer.clients[slot].nameStickySuffix = false;
    }
    if (udpServer.clients[slot].wbnWebCountry[0] != '\0') {
        serverSimSetPlayerCountry(sim, slot,
                                  udpServer.clients[slot].wbnWebCountry);
        udpServer.clients[slot].countryCode[0] =
            udpServer.clients[slot].wbnWebCountry[0];
        udpServer.clients[slot].countryCode[1] =
            udpServer.clients[slot].wbnWebCountry[1];
        udpServer.clients[slot].countryCode[2] = '\0';
    }
}

/* Everything about the slot that re-authenticated, captured when the reauth
 * arrived so the stamp below can run a round trip later without re-reading
 * state that may have moved on.  connId is what says the slot still belongs
 * to the same player when the result lands. */
typedef struct {
    BYTE     slot;
    uint64_t connId;
    char     token[WBN_JOIN_KEY_WIRE_LEN];
    char     verifyName[PACKET_MAX_PLAYER_NAME];
    bool     isWeb;
    bool     isPendingClaim;
} WbnReauthCtx;

/* Re-authentications waiting on their client/verify job, one entry per slot.
 * The context lives here rather than on the job because nothing on the
 * WinBolo.net worker may name a slot, a connection or the sim.
 *
 * One entry per slot is the worst case: the rekey broadcast makes every
 * connected slot re-auth at once, and a slot that re-auths again while its
 * first verify is still out is refused until that verify answers or its
 * hold lapses (transportUdpServerHandleWbnReauth).  So the table cannot
 * fill, and at most MAX_TANKS verifies are ever on the worker's queue for
 * re-auths: the token is the client's to send, and without the refusal one
 * connected client could queue a verify per datagram behind everything
 * else the worker has to post.  A reauth that lands after the hold lapsed
 * replaces the entry, and the lapsed job's result then finds no entry and
 * is dropped.
 *
 * Transport create, destroy and the fuzz init each clear the whole table.
 * Job ids are unique for the life of the process and never reused, so an
 * entry left over from an earlier server cannot match a later result, but
 * its hold names a tick number the next server's counter (restarted at 0)
 * will not reach for a long time, which would refuse that slot's re-auths. */
static struct {
    uint32_t     jobId;          /* 0 when the entry is free */
    uint32_t     holdUntilTick;  /* the grace sweep defers to this entry until here */
    WbnReauthCtx ctx;
} s_reauthPending[MAX_TANKS];

/* The pending entry a completed verify belongs to, or -1 when it was
 * superseded, already applied, or never ours. */
static int udpServerFindReauthPending(uint32_t jobId) {
    int i;
    if (jobId == 0) return -1;
    for (i = 0; i < MAX_TANKS; i++) {
        if (s_reauthPending[i].jobId == jobId) return i;
    }
    return -1;
}

bool udpServerReauthVerifyOutstanding(BYTE slot, uint32_t nowTick) {
    if (slot >= MAX_TANKS) return false;
    if (s_reauthPending[slot].jobId == 0) return false;
    /* Wrap-safe compare, as wbnJoinOnTick does it: lapsed once nowTick has
     * reached the deadline. */
    if ((int32_t)(nowTick - s_reauthPending[slot].holdUntilTick) >= 0) {
        return false;
    }
    return true;
}

void udpServerClearReauthPending(BYTE slot) {
    if (slot >= MAX_TANKS) return;
    s_reauthPending[slot].jobId = 0;
}

/* The WinBolo.net checks of rejoins into a locked game, before a seat is
 * given (udpServerLockedSignInStep). An entry is a joiner (address and
 * name) and the player_key it sent. The verify runs on the WinBolo.net
 * worker; the joiner's next JOIN reads the result. The table has MAX_TANKS
 * entries, and a source IP (for IPv6, a source /64) has at most one check
 * that runs, so one address cannot fill it. An entry is freed when its JOIN reads the end of
 * the check, or reused once WBN_REAUTH_HOLD_TICKS have gone. Cleared with
 * the re-authentication table. */
typedef enum {
    LOCKED_ENTRY_FREE = 0,
    LOCKED_ENTRY_WAITING,
    LOCKED_ENTRY_OK,
    LOCKED_ENTRY_FAILED
} LockedEntryState;

static struct {
    LockedEntryState   state;
    uint32_t           jobId;
    uint32_t           untilTick;  /* the check lapses at this tick */
    BoloAddr           addr;
    char               name[PACKET_MAX_PLAYER_NAME];
    char               key[WBN_JOIN_KEY_WIRE_LEN];
    bool               hasSteam;
    bool               isSupporter;
} s_lockedSignIn[MAX_TANKS];

/* Test seam: transportUdpServerSetLockedRejoinWbnForTest. With it on, the
 * check runs as if WinBolo.net runs: a fake server key, and job ids that
 * only the result seam answers. */
static bool     s_lockedSignInForTest = false;
static uint32_t s_lockedSignInTestJobId = 0;
#define LOCKED_SIGN_IN_TEST_SERVER_KEY "test-locked-sign-in-server-key"

void udpServerClearAllReauthPending(void) {
    memset(s_reauthPending, 0, sizeof(s_reauthPending));
    memset(s_lockedSignIn, 0, sizeof(s_lockedSignIn));
}

void transportUdpServerSetLockedRejoinWbnForTest(bool on) {
    s_lockedSignInForTest = on;
}

bool udpServerLockedSignInAvailable(void) {
    return winbolonetIsRunning() || s_lockedSignInForTest;
}

bool udpServerLockedSignInServerKey(uint8_t *out) {
    char serverKey[WINBOLONET_KEY_LEN];
    serverKey[0] = '\0';
    if (winbolonetIsRunning()) {
        winboloNetGetServerKey(serverKey);
    } else if (s_lockedSignInForTest) {
        snprintf(serverKey, sizeof(serverKey), "%s",
                 LOCKED_SIGN_IN_TEST_SERVER_KEY);
    }
    if (serverKey[0] == '\0') return false;
    wbnKeyEncode(out, serverKey);
    return true;
}

static bool lockedSignInLapsed(int i, uint32_t nowTick) {
    /* Wrap-safe compare, as wbnJoinOnTick does it. */
    return (int32_t)(nowTick - s_lockedSignIn[i].untilTick) >= 0;
}

LockedSignInStep udpServerLockedSignInStep(const BoloAddr *addr,
                                           const char *name,
                                           const char *joinKey,
                                           uint32_t nowTick,
                                           char *keyOut,
                                           bool *hasSteamOut,
                                           bool *isSupporterOut) {
    int i;
    int found = -1;
    int freeIdx = -1;
    uint32_t jobId;
    uint8_t prefix[BOLO_ADDR_KEY_LEN];
    uint8_t entryPrefix[BOLO_ADDR_KEY_LEN];

    for (i = 0; i < MAX_TANKS; i++) {
        if (s_lockedSignIn[i].state == LOCKED_ENTRY_FREE) continue;
        if (boloAddrEqual(&s_lockedSignIn[i].addr, addr) &&
            playerNameCompare(s_lockedSignIn[i].name, name) == 0) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        LockedEntryState state = s_lockedSignIn[found].state;
        bool sameKey = joinKey[0] == '\0' ||
                       strcmp(s_lockedSignIn[found].key, joinKey) == 0;
        if (sameKey && state == LOCKED_ENTRY_WAITING &&
            !lockedSignInLapsed(found, nowTick)) {
            return LOCKED_SIGN_IN_WAIT;
        }
        /* The check ends here in every other case: a check that lapsed,
         * a JOIN with another key, or a result to give. */
        s_lockedSignIn[found].state = LOCKED_ENTRY_FREE;
        if (lockedSignInLapsed(found, nowTick) || joinKey[0] == '\0' ||
            !sameKey || state != LOCKED_ENTRY_OK) {
            return LOCKED_SIGN_IN_REFUSED;
        }
        snprintf(keyOut, WINBOLONET_KEY_LEN, "%s", s_lockedSignIn[found].key);
        *hasSteamOut = s_lockedSignIn[found].hasSteam;
        *isSupporterOut = s_lockedSignIn[found].isSupporter;
        return LOCKED_SIGN_IN_OK;
    }
    if (joinKey[0] == '\0') return LOCKED_SIGN_IN_ASK;

    /* A new check. One source IP, or IPv6 /64, has at most one check that
     * runs. */
    boloAddrPrefixKey(addr, prefix);
    for (i = 0; i < MAX_TANKS; i++) {
        if (s_lockedSignIn[i].state == LOCKED_ENTRY_FREE ||
            lockedSignInLapsed(i, nowTick)) {
            if (freeIdx < 0) freeIdx = i;
            continue;
        }
        if (s_lockedSignIn[i].state != LOCKED_ENTRY_WAITING) continue;
        boloAddrPrefixKey(&s_lockedSignIn[i].addr, entryPrefix);
        if (memcmp(entryPrefix, prefix, sizeof(prefix)) == 0) {
            return LOCKED_SIGN_IN_REFUSED;
        }
    }
    if (freeIdx < 0) return LOCKED_SIGN_IN_REFUSED;
    if (winbolonetIsRunning()) {
        jobId = winbolonetQueueVerifyClientKey(joinKey, name);
    } else if (s_lockedSignInForTest) {
        jobId = ++s_lockedSignInTestJobId;
    } else {
        jobId = 0;
    }
    if (jobId == 0) return LOCKED_SIGN_IN_REFUSED;
    memset(&s_lockedSignIn[freeIdx], 0, sizeof(s_lockedSignIn[freeIdx]));
    s_lockedSignIn[freeIdx].state = LOCKED_ENTRY_WAITING;
    s_lockedSignIn[freeIdx].jobId = jobId;
    s_lockedSignIn[freeIdx].untilTick = nowTick + WBN_REAUTH_HOLD_TICKS;
    s_lockedSignIn[freeIdx].addr = *addr;
    snprintf(s_lockedSignIn[freeIdx].name, sizeof(s_lockedSignIn[freeIdx].name),
             "%s", name);
    snprintf(s_lockedSignIn[freeIdx].key, sizeof(s_lockedSignIn[freeIdx].key),
             "%s", joinKey);
    return LOCKED_SIGN_IN_WAIT;
}

/* Writes the end of a check that runs. */
static void lockedSignInFinish(int i, bool ok, bool hasSteam,
                               bool isSupporter) {
    s_lockedSignIn[i].state = ok ? LOCKED_ENTRY_OK : LOCKED_ENTRY_FAILED;
    s_lockedSignIn[i].hasSteam = ok && hasSteam;
    s_lockedSignIn[i].isSupporter = ok && isSupporter;
    WB_LOG_INFO(WB_LOG_CAT_NET,
                "[WBN] locked-game rejoin check for '%s': %s",
                s_lockedSignIn[i].name, ok ? "accepted" : "refused");
}

bool udpServerLockedSignInResult(uint32_t jobId, int status,
                                 const char *response) {
    int i;
    char errorMsg[512];
    bool hasSteam = FALSE;
    bool isSupporter = FALSE;
    bool ok;

    if (jobId == 0) return false;
    for (i = 0; i < MAX_TANKS; i++) {
        if (s_lockedSignIn[i].state == LOCKED_ENTRY_WAITING &&
            s_lockedSignIn[i].jobId == jobId) {
            break;
        }
    }
    if (i == MAX_TANKS) return false;
    /* The joiner has no slot yet, so the reply is only read
     * (playerNum MAX_TANKS). The seat stores the key. */
    errorMsg[0] = '\0';
    ok = winbolonetApplyVerifyResult(status, response, s_lockedSignIn[i].key,
                                     (BYTE)MAX_TANKS, errorMsg, &hasSteam,
                                     &isSupporter);
    lockedSignInFinish(i, ok, hasSteam, isSupporter);
    return true;
}

/* Test seam: the result WinBolo.net gives the check that runs under name.
 * WinBolo.net verifies the key under the name the joiner asked for, so it
 * accepts only when keyOwner is that name. NULL is a failed verify. */
bool transportUdpServerLockedSignInResultForTest(const char *name,
                                                 const char *keyOwner) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (s_lockedSignIn[i].state == LOCKED_ENTRY_WAITING &&
            playerNameCompare(s_lockedSignIn[i].name, name) == 0) {
            lockedSignInFinish(i,
                               keyOwner != NULL &&
                                   playerNameCompare(keyOwner, name) == 0,
                               false, false);
            return true;
        }
    }
    return false;
}

/* Test seam: true while a check under name waits for its result. */
bool transportUdpServerLockedSignInPendingForTest(const char *name) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (s_lockedSignIn[i].state == LOCKED_ENTRY_WAITING &&
            playerNameCompare(s_lockedSignIn[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Test seam: makes the check under name lapse at this tick. */
void transportUdpServerExpireLockedSignInForTest(const char *name) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (s_lockedSignIn[i].state != LOCKED_ENTRY_FREE &&
            playerNameCompare(s_lockedSignIn[i].name, name) == 0) {
            s_lockedSignIn[i].untilTick = udpServer.tickCount;
        }
    }
}

void transportUdpServerExpireReauthHoldForTest(BYTE slot) {
    if (slot >= MAX_TANKS) return;
    s_reauthPending[slot].holdUntilTick = udpServer.tickCount;
}

/* Apply one re-authentication's outcome to its slot: the flags, the deferred
 * PLAYER_JOIN, the lobby publish and the provisional-claim resolve.  Runs on
 * the tick thread in every case — from the completion handler for a native
 * slot, and straight from the reauth for a web slot, whose join_code verify
 * is read-only and cached for the connection.
 *
 * wasParticipant is the slot's keyed state read immediately before the key
 * was written, not when the re-auth was queued: a session rotation clears
 * every key, so a value read at queue time can say "already a participant"
 * for a slot that is registering into a new session, which reads a genuine
 * absent-to-present transition as an idempotent rekey resend and swallows
 * the PLAYER_JOIN. */
static void udpServerStampReauth(ServerSim *sim, const WbnReauthCtx *rc,
                                 bool wasParticipant, bool verifyOk,
                                 bool hasSteam, bool wbnIsSupporter,
                                 const char *errorMsg) {
    BYTE slot = rc->slot;

    if (verifyOk) {
        if (rc->isWeb) {
            /* Web slot: stamp the WBN-authoritative identity (name, country,
             * flags) from the cached join-code result. */
            udpServerApplyWebIdentity(sim, slot);
        } else {
            /* Re-merge using the clientHints captured at JOIN_REQUEST (the
             * client doesn't re-send them on REAUTH; we re-verify against
             * WBN, not the network). */
            uint8_t storedHints = udpServer.clients[slot].clientHints;
            uint8_t flags = storedHints & PLAYER_CLIENT_HINT_MASK;
            flags |= PLAYER_FLAG_WBN_VERIFIED;
            if (hasSteam) flags |= PLAYER_FLAG_WBN_STEAM_LINKED;
            if (wbnIsSupporter) flags |= PLAYER_FLAG_SUPPORTER;
            /* Carry the mic bits across the rebuild. The client sends them
             * once when they change, so anything dropped here is gone for
             * the rest of the connection rather than re-reported on the
             * next tick. */
            flags |= (uint8_t)(playersGetClientFlags(
                                   &serverSimGetGameSim(sim)->plyrs, slot)
                               & PLAYER_VOICE_FLAG_MASK);
            playersSetClientFlags(&serverSimGetGameSim(sim)->plyrs, slot, flags);
            /* Keep the durable rekey-gate bit in step with the session flag. */
            udpServer.clients[slot].wbnWasVerified = true;
            playersSetClientType (&serverSimGetGameSim(sim)->plyrs, slot,
                                  udpServer.clients[slot].clientType);
        }
        WB_LOG_INFO(WB_LOG_CAT_NET,
                    "[WBN] Player %d re-authenticated (steam=%d)",
                    (int)slot, hasSteam ? 1 : 0);
        /* Fire the deferred PLAYER_JOIN now that the slot is keyed — in
         * every phase, not just running.  The edge guard emits exactly
         * once per session: a fresh join or a post-rotation re-register
         * (key was absent) emits; an idempotent rekey resend (already a
         * participant) does not.  This also satisfies the anonymous
         * fallback armed at join, so the grace sweep won't fire too. */
        if (wbnJoinOnReauth(&udpServer.clients[slot].wbnJoin,
                            wasParticipant)) {
            winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                               slot, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
        }
        if (serverSimGetState(sim) == serverStateLobby ||
            serverSimGetState(sim) == serverStateCountdown) {
            serverSimPublishLobbySlot(sim, slot);
        }

        /* Resolve a pending provisional claim: verify ran under the desired
         * bare name above, so attribution is correct; now reconcile the local
         * display.  Three outcomes by who holds the bare name now. */
        if (rc->isPendingClaim) {
            const char *desired = udpServer.clients[slot].claimDesiredName;
            int s;
            int holder = -1;
            for (s = 0; s < MAX_TANKS; s++) {
                if (s == (int)slot) continue;
                if (!udpServer.clients[s].connected) continue;
                if (playerNameCompare(udpServer.clients[s].playerName,
                                      desired) == 0) {
                    holder = s;
                    break;
                }
            }

            bool holderIsVerified =
                holder >= 0 &&
                (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs,
                                       (BYTE)holder)
                 & PLAYER_FLAG_WBN_VERIFIED) != 0;

            switch (claimResolveDecide(holder >= 0, holderIsVerified)) {
            case CLAIM_RESOLVE_PROMOTE_FREE:
                /* Bare name free — the squatter left during grace.  Promote
                 * straight to the bare name. */
                serverSimSetPlayerName(sim, slot, desired);
                serverSimPublishLobbySlot(sim, slot);
                snprintf(udpServer.clients[slot].playerName,
                         PACKET_MAX_PLAYER_NAME, "%s", desired);
                udpServer.clients[slot].nameStickySuffix = false;
                break;
            case CLAIM_RESOLVE_PREEMPT_SQUATTER: {
                /* Unverified squatter still holds the bare name.  Rename it
                 * off first (it must vacate before the joiner claims), then
                 * promote this slot.  If the squatter's suffix pool is
                 * exhausted, keep this slot on its temp name. */
                char squatterName[PACKET_MAX_PLAYER_NAME];
                if (serverChooseUnverifiedSuffix(
                        udpServer.clients[holder].playerName, holder,
                        squatterName, sizeof(squatterName))) {
                    serverPreemptRename(sim, holder, squatterName,
                                        desired,
                                        udpServer.clients[slot].countryCode);
                    /* Plain set, NOT serverPreemptRename — that would emit a
                     * spurious "renamed by verified player" naming the joiner
                     * as its own victim. */
                    serverSimSetPlayerName(sim, slot, desired);
                    serverSimPublishLobbySlot(sim, slot);
                    snprintf(udpServer.clients[slot].playerName,
                             PACKET_MAX_PLAYER_NAME, "%s", desired);
                    udpServer.clients[slot].nameStickySuffix = false;
                }
                /* else: squatter pool exhausted — stay on the temp name. */
                break;
            }
            case CLAIM_RESOLVE_KEEP_TEMP:
                /* A verified slot won the bare name (a second reclaimer won
                 * the race); keep this slot on its temp name permanently.  WBN
                 * attribution is already correct since verify ran under the
                 * bare name; only the local display stays suffixed. */
                break;
            }

            /* Clear the claim in every outcome.  desired aliases the buffer,
             * so this clear must come after all uses of desired. */
            udpServer.clients[slot].claimPending = false;
            udpServer.clients[slot].claimDesiredName[0] = '\0';
        }

        /* The seat's name is now the one WinBolo.net confirmed. A rejoiner
         * under the name of a signed-in leaver gets that player's team,
         * pillboxes and bases back here, not at the join. */
        serverSimRejoinSignedIn(sim, slot);
    } else if (rc->isWeb && udpServer.clients[slot].wbnWebIdentityCached) {
        /* A web slot whose code verified but resolved to a guest (not logged
         * in) returns false here — that is the expected anonymous case, not a
         * failure, so log it at info and don't emit a scary warning. */
        WB_LOG_INFO(WB_LOG_CAT_NET,
                    "[WBN] Player %d web slot resolved as guest (anonymous)",
                    (int)slot);
    } else {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[WBN] Player %d re-auth failed: %s", (int)slot, errorMsg);
    }
}

void udpServerApplyReauthResult(ServerSim *sim, uint32_t id, int status,
                                const char *response) {
    WbnReauthCtx rc;
    char errorMsg[512];
    bool hasSteam = FALSE;
    bool wbnIsSupporter = FALSE;
    bool wasParticipant;
    bool verifyOk;
    int  idx;

    if (sim == NULL) return;
    /* A check of a rejoin into a locked game has no slot yet. */
    if (udpServerLockedSignInResult(id, status, response)) return;
    idx = udpServerFindReauthPending(id);
    if (idx < 0) return;
    rc = s_reauthPending[idx].ctx;
    s_reauthPending[idx].jobId = 0;

    /* Before anything else: the slot may have been freed and handed to
     * somebody else while the verify was out, and stamping the previous
     * occupant's identity onto the new player is what this check exists to
     * prevent. */
    if (!udpServer.clients[rc.slot].connected ||
        udpServer.clients[rc.slot].connId != rc.connId) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[WBN] re-auth result for slot %d dropped: verified "
                    "connection %llu, slot now holds %llu",
                    (int)rc.slot,
                    (unsigned long long)rc.connId,
                    (unsigned long long)udpServer.clients[rc.slot].connId);
        return;
    }

    /* Read the slot's keyed state now, a few lines ahead of the key write
     * below, rather than carrying a copy from when the verify was queued.
     * A session rotation between the two clears every key, so a stale copy
     * would report a fresh registration as a rekey resend. */
    wasParticipant = winboloNetIsPlayerParticipant(rc.slot);

    errorMsg[0] = '\0';
    if (rc.isWeb) {
        /* The join_code route resolves an identity rather than confirming a
         * key.  What comes back is the slot's, so it is cached here, on the
         * tick, the way the key write below is. */
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        char countryBuf[3];
        bool isLoggedIn = FALSE;
        int  userId = -1;
        nameBuf[0]    = '\0';
        countryBuf[0] = '\0';
        if (winbolonetApplyVerifyJoinCodeResult(status, response, nameBuf,
                                                &isLoggedIn, countryBuf,
                                                &userId, errorMsg)) {
            udpServer.clients[rc.slot].wbnWebIdentityCached = true;
            udpServer.clients[rc.slot].wbnWebIsLoggedIn = isLoggedIn;
            snprintf(udpServer.clients[rc.slot].wbnWebName,
                     PACKET_MAX_PLAYER_NAME, "%s", nameBuf);
            udpServer.clients[rc.slot].wbnWebCountry[0] = countryBuf[0];
            udpServer.clients[rc.slot].wbnWebCountry[1] = countryBuf[1];
            udpServer.clients[rc.slot].wbnWebCountry[2] = '\0';
            udpServer.clients[rc.slot].wbnWebUserId = userId;
            /* A code that resolves to a guest verified fine; it just names
             * nobody, which the stamp reports as the anonymous case. */
            verifyOk = isLoggedIn;
        } else {
            /* Invalid, expired or wrong-server code: leave the slot uncached
             * so a later reauth with a still-valid code can still succeed. */
            verifyOk = FALSE;
        }
    } else {
        /* The key write winboloNetVerifyClientKey used to do on the calling
         * thread happens in here, on the tick. */
        verifyOk = winbolonetApplyVerifyResult(status, response, rc.token,
                                               rc.slot, errorMsg, &hasSteam,
                                               &wbnIsSupporter);
    }
    udpServerStampReauth(sim, &rc, wasParticipant, verifyOk, hasSteam,
                         wbnIsSupporter, errorMsg);
}

void transportUdpServerHandleWbnReauth(ServerSim *sim, BYTE slot,
                                       const char *token) {
    WbnReauthCtx rc;
    uint32_t jobId;

    if (!winbolonetIsRunning() || token == NULL || token[0] == '\0') {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[WBN] re-auth for slot %d ignored: running=%d tokenLen=%d",
                    (int)slot, winbolonetIsRunning() ? 1 : 0,
                    token ? (int)strlen(token) : -1);
        return;
    }

    memset(&rc, 0, sizeof(rc));
    rc.slot   = slot;
    rc.connId = udpServer.clients[slot].connId;
    snprintf(rc.token, sizeof(rc.token), "%s", token);
    rc.isWeb = (udpServer.clients[slot].clientType == CLIENT_TYPE_WEB);
    /* Web slots take their verified name from WBN, not the wire, so the native
     * provisional-claim dance (temp -unverified[-N] names, squatter preemption)
     * does not apply to them. */
    rc.isPendingClaim = !rc.isWeb && udpServer.clients[slot].claimPending;
    /* For a pending provisional claim the slot's display name is the temp
     * -unverified[-N] handed out at join; WBN must verify and attribute
     * under the real account display name, which is the stored desired bare
     * name.  Non-claim reauths verify under the slot's own name as before. */
    snprintf(rc.verifyName, sizeof(rc.verifyName), "%s",
             rc.isPendingClaim ? udpServer.clients[slot].claimDesiredName
                               : udpServer.clients[slot].playerName);

    /* A WEB slot's identity is resolved once per connection and cached: the
     * join_code is single-use and the server_key rotates between rounds, so a
     * re-verify would fail.  A later reauth re-stamps from the cache and never
     * reaches the network at all. */
    if (rc.isWeb && udpServer.clients[slot].wbnWebIdentityCached) {
        /* The slot's keyed state as it is now. Nothing has been queued for
         * this path, so now is also immediately before the stamp. */
        udpServerStampReauth(sim, &rc, winboloNetIsPlayerParticipant(slot),
                             udpServer.clients[slot].wbnWebIsLoggedIn,
                             /*hasSteam*/ FALSE, /*wbnIsSupporter*/ FALSE, "");
        return;
    }

    /* Both verifies are an HTTPS round trip, so they go to the WinBolo.net
     * worker and the outcome comes back through udpServerApplyReauthResult on
     * a later tick.  A web slot presents a join_code and takes the read-only
     * verify_join_code route; a native one presents a minted player_key.
     * Nothing below this line waits for either. */
    if (udpServerReauthVerifyOutstanding(slot, udpServer.tickCount)) {
        /* One verify per slot at a time. The client re-sends on the next
         * rekey, and a rekey's register result is drained behind the verify
         * this slot already has out, so a legitimate re-auth never needs to
         * queue behind one. Without this a client could queue a verify per
         * datagram, and the worker posts them one at a time with up to 35s
         * each. */
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[WBN] re-auth for slot %d ignored: verify job %u is "
                    "still outstanding",
                    (int)slot, (unsigned)s_reauthPending[slot].jobId);
        return;
    }
    jobId = rc.isWeb ? winbolonetQueueVerifyJoinCode(rc.token)
                     : winbolonetQueueVerifyClientKey(rc.token, rc.verifyName);
    if (jobId == 0) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
                    "[WBN] re-auth for slot %d not queued: %s was not taken",
                    (int)slot,
                    rc.isWeb ? "client/verify_join_code" : "client/verify");
        return;
    }
    /* One entry per slot. A reauth arriving after the hold lapsed replaces
     * the entry, and the lapsed job's result is dropped when it lands. */
    s_reauthPending[slot].jobId         = jobId;
    s_reauthPending[slot].holdUntilTick = udpServer.tickCount +
                                          WBN_REAUTH_HOLD_TICKS;
    s_reauthPending[slot].ctx           = rc;
}

/* Turn away a PACKET_ROUND_LOG_REQ. Every gate that refuses one sends this,
 * echoing the request's reqSeq: a silent refusal is indistinguishable from a
 * lost request, and the client would sit waiting for bytes that never come.
 * Wire: [header 8] [reqSeq 4 BE] [code 1]. */
void serverSendRoundLogErr(uint32_t reqSeq, uint8_t code,
                           const BoloAddr *toAddr) {
    uint8_t err[PACKET_HEADER_SIZE + 5];
    packHeader(err, PACKET_ROUND_LOG_ERR, 0);
    packU32(err + PACKET_HEADER_SIZE, reqSeq);
    err[PACKET_HEADER_SIZE + 4] = code;
    srvSendTo(err, (int)sizeof(err), toAddr);
}
