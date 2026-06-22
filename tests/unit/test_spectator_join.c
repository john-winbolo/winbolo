/*
 * Spectator JOIN handshake test (Phase 2b-i).
 *
 * A JOIN_REQUEST carrying JOIN_FLAG_SPECTATOR registers a tankless viewer in
 * the transport's spectators[] array instead of allocating a tank slot. Like
 * test_cookie_handshake.c this stands up the loopback harness and drives
 * hand-built JOINs from raw UDP sockets the test owns, each completing the
 * address-proof cookie round-trip before the spectator branch runs.
 *
 * Cases (each on a distinct loopback source IP so it gets its own join
 * rate-limit bucket; Linux-only 127/8 aliases, skipped with a log otherwise):
 *   (a) Disabled: maxSpectators == 0 → a flagged JOIN is refused, spectator
 *       count stays 0.
 *   (b) Accepted + no tank slot: maxSpectators > 0 → a flagged JOIN draws a
 *       JOIN_ACCEPT whose slot byte is the 0xFF viewer sentinel; the spectator
 *       count is 1 and the sim's player count is unchanged from its
 *       (harness-client-only) baseline.
 *   (c) Cap: maxSpectators == 1 with one spectator already connected → a
 *       second, distinct-address flagged JOIN is refused; count stays 1.
 *
 * Subscriber isolation (a spectator is never registered on the control bus)
 * is verified structurally in review, not by a runtime assertion here.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAP_STR_SIZE */
#include "platform_net.h"      /* sockets, struct sockaddr_in */
#include "wire_limits.h"       /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"          /* PACKET_* types, PACKET_HEADER_SIZE, BOLO_VERSION_*, JOIN_COOKIE_LEN */
#include "transport_udp.h"     /* WBN_JOIN_KEY_WIRE_LEN, JOIN_FLAG_SPECTATOR, transportUdpServerGetSpectatorCount */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "server_sim.h"        /* serverSimSetMaxSpectators, serverSimGetNumPlayers */
#include "test_harness.h"
#include "loopback_harness.h"

#define SJ_REPLY_DEADLINE_MS 10000
#define SJ_NOACCEPT_PUMPS    400
#define SJ_BOUNDARY_RETRIES  4

#define SJ_COOKIE_WINDOW_SEC 16
static uint64_t sjCookieWindow(void) {
    return (SDL_GetTicks() / 1000ULL) / SJ_COOKIE_WINDOW_SEC;
}

/* Build a JOIN_REQUEST with the server's version triple, a valid name, the
 * given flags byte, and a trailing cookie (given bytes, or zeros if NULL). */
static int sjBuildJoin(uint8_t *buf, const char *name, uint8_t flags,
                       const uint8_t *cookieOrNull) {
    int pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_JOIN_REQUEST, 0);

    memset(buf + pos, 0, PACKET_MAX_PLAYER_NAME);
    strncpy((char *)(buf + pos), name, PACKET_MAX_PLAYER_NAME - 1);
    pos += PACKET_MAX_PLAYER_NAME;

    memset(buf + pos, 0, MAP_STR_SIZE);   /* empty password */
    pos += MAP_STR_SIZE;

    buf[pos++] = BOLO_VERSION_MAJOR;
    buf[pos++] = BOLO_VERSION_MINOR;
    buf[pos++] = BOLO_VERSION_REVISION;

    memset(buf + pos, 0, WBN_JOIN_KEY_WIRE_LEN);  /* anonymous join */
    pos += WBN_JOIN_KEY_WIRE_LEN;

    buf[pos++] = flags;
    buf[pos++] = 0;   /* clientType */
    buf[pos++] = 0;   /* clientHints */
    buf[pos++] = 0;   /* fallbackCountry[0] */
    buf[pos++] = 0;   /* fallbackCountry[1] */

    if (cookieOrNull != NULL) {
        memcpy(buf + pos, cookieOrNull, JOIN_COOKIE_LEN);
    } else {
        memset(buf + pos, 0, JOIN_COOKIE_LEN);
    }
    pos += JOIN_COOKIE_LEN;
    return pos;
}

static SOCKET sjOpenSocketOnIp(const char *ip) {
    struct sockaddr_in addr;
    unsigned long nonblock = 1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port        = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    ioctlsocket(s, FIONBIO, &nonblock);
    return s;
}

/* Send one JOIN and pump. Captures whether an ACCEPT and/or CHALLENGE arrived,
 * the accept's slot byte (when accepted), and (on a challenge) its cookie. When
 * wantNoAccept, pumps extra ticks after a challenge to confirm none follows. */
static void sjDriveOnce(LoopbackHarness *h, SOCKET sock,
                        const uint8_t *join, int joinLen,
                        const struct sockaddr_in *server,
                        bool wantNoAccept,
                        bool *gotAccept, bool *gotChallenge,
                        uint8_t *acceptSlot, uint8_t *outCookie) {
    Uint64 deadline;
    int extra = 0;
    *gotAccept    = false;
    *gotChallenge = false;
    sendto(sock, (const char *)join, joinLen, 0,
           (const struct sockaddr *)server, sizeof(*server));
    deadline = SDL_GetTicks() + SJ_REPLY_DEADLINE_MS;
    while (SDL_GetTicks() < deadline) {
        uint8_t in[1024];
        int n;
        bool drained = false;
        loopbackHarnessPump(h);
        while ((n = (int)recvfrom(sock, (char *)in, sizeof(in), 0, NULL, NULL)) > 0) {
            uint8_t type = getPacketType(in, n);
            if (type == PACKET_JOIN_ACCEPT && n > PACKET_HEADER_SIZE) {
                *gotAccept = true;
                if (acceptSlot != NULL) *acceptSlot = in[PACKET_HEADER_SIZE];
            } else if (type == PACKET_JOIN_CHALLENGE &&
                       n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                *gotChallenge = true;
                if (outCookie != NULL) {
                    memcpy(outCookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
                }
            }
            drained = true;
        }
        if (*gotAccept) return;
        if (*gotChallenge) {
            if (!wantNoAccept) return;
            if (++extra >= SJ_NOACCEPT_PUMPS) return;
        }
        if (!drained) SDL_Delay(1);
    }
}

/* Drive a full spectator handshake (cookieless → challenge → echo). Returns
 * true and fills *acceptSlot if a JOIN_ACCEPT arrived; false if it was refused
 * (challenge but no accept). Retries only when a cookie ages out of its window
 * mid-flight under host starvation. */
static bool sjHandshakeAccepts(LoopbackHarness *h, SOCKET s,
                               const struct sockaddr_in *server,
                               const char *name, bool expectAccept,
                               uint8_t *acceptSlot) {
    uint8_t joinBuf[1024];
    uint8_t cookie[JOIN_COOKIE_LEN];
    bool gotAccept, gotChallenge;
    int attempt;

    for (attempt = 0; attempt < SJ_BOUNDARY_RETRIES; attempt++) {
        uint64_t w0 = sjCookieWindow();
        int joinLen = sjBuildJoin(joinBuf, name, JOIN_FLAG_SPECTATOR, NULL);
        sjDriveOnce(h, s, joinBuf, joinLen, server, /*wantNoAccept*/ false,
                    &gotAccept, &gotChallenge, NULL, cookie);
        if (gotAccept) return true;          /* accepted with no cookie?? */
        if (!gotChallenge) return false;     /* no challenge — treat as refused */

        joinLen = sjBuildJoin(joinBuf, name, JOIN_FLAG_SPECTATOR, cookie);
        sjDriveOnce(h, s, joinBuf, joinLen, server,
                    /*wantNoAccept*/ !expectAccept,
                    &gotAccept, &gotChallenge, acceptSlot, NULL);
        if (gotAccept) return true;
        if (sjCookieWindow() - w0 >= 2) continue; /* cookie aged out — retry */
        return false;                              /* genuinely refused */
    }
    return false;
}

/* The harness's own wire client has taken its tank slot once the server
 * counts a player. (clientSimGetMyPlayerNum can't be used as the signal: it
 * defaults to 0 before the join completes, indistinguishable from slot 0.) */
static bool sjHarnessPlayerSeated(LoopbackHarness *h, void *user) {
    (void)user;
    return serverSimGetNumPlayers(h->sim) >= 1;
}

int run_spectator_join(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    BYTE baselinePlayers;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    /* Let the harness's own wire client settle into its tank slot so the
     * player-count baseline is stable; the spectator must not change it. */
    loopbackHarnessPumpUntil(&h, 4000, sjHarnessPlayerSeated, NULL);
    baselinePlayers = serverSimGetNumPlayers(h.sim);

    /* ---- (a) disabled: maxSpectators == 0 refuses, count stays 0 ---- */
    {
        SOCKET s = sjOpenSocketOnIp("127.0.0.2");
        if (s == INVALID_SOCKET) {
            SDL_Log("  spectator join: skipping (a) — 127.0.0.2 not bindable");
        } else {
            uint8_t slotByte = 0;
            bool accepted;
            serverSimSetMaxSpectators(h.sim, 0);
            accepted = sjHandshakeAccepts(&h, s, &serverAddr, "SpecA",
                                          /*expectAccept*/ false, &slotByte);
            closesocket(s);
            if (accepted) {
                loopbackHarnessStop(&h);
                UT_FAIL("spectator JOIN accepted while maxSpectators==0");
            }
            UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 0,
                          "disabled spectating must register no spectator");
        }
    }

    /* ---- (b) accepted: slot==0xFF, count 1, no tank slot consumed ---- */
    {
        SOCKET s = sjOpenSocketOnIp("127.0.0.3");
        if (s == INVALID_SOCKET) {
            SDL_Log("  spectator join: skipping (b) — 127.0.0.3 not bindable");
        } else {
            uint8_t slotByte = 0;
            bool accepted;
            serverSimSetMaxSpectators(h.sim, 4);
            accepted = sjHandshakeAccepts(&h, s, &serverAddr, "SpecB",
                                          /*expectAccept*/ true, &slotByte);
            closesocket(s);
            if (!accepted) {
                loopbackHarnessStop(&h);
                UT_FAIL("spectator JOIN drew no accept with maxSpectators==4");
            }
            UT_ASSERT_MSG(slotByte == 0xFF,
                          "spectator accept must carry the 0xFF no-slot sentinel");
            UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                          "accepted spectator must be counted once");
            UT_ASSERT_MSG(serverSimGetNumPlayers(h.sim) == baselinePlayers,
                          "spectator must not consume a tank slot");
        }
    }

    /* ---- (c) cap: with one spectator up and maxSpectators==1, refuse a
     *           second distinct-address spectator; count stays 1 ---- */
    {
        SOCKET s = sjOpenSocketOnIp("127.0.0.4");
        if (s == INVALID_SOCKET) {
            SDL_Log("  spectator join: skipping (c) — 127.0.0.4 not bindable");
        } else {
            uint8_t slotByte = 0;
            bool accepted;
            /* Only meaningful if (b) actually seated a spectator. */
            if (transportUdpServerGetSpectatorCount() == 1) {
                serverSimSetMaxSpectators(h.sim, 1);
                accepted = sjHandshakeAccepts(&h, s, &serverAddr, "SpecC",
                                              /*expectAccept*/ false, &slotByte);
                closesocket(s);
                if (accepted) {
                    loopbackHarnessStop(&h);
                    UT_FAIL("spectator JOIN accepted past the maxSpectators cap");
                }
                UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                              "cap reached → spectator count must stay 1");
            } else {
                closesocket(s);
                SDL_Log("  spectator join: skipping (c) — (b) seated no spectator");
            }
        }
    }

    loopbackHarnessStop(&h);
    return 0;
}
