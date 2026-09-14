/*
 * Spectator roster publish test — server transport broadcasts CTRL_SPECTATOR_SLOT
 * to player clients, driven end-to-end through the real client apply path.
 *
 * Like test_spectator_join.c this stands up the loopback harness (a real wire
 * PLAYER client, h.cs) and drives spectator JOINs from a raw UDP socket the
 * test owns, each completing the address-proof cookie round-trip. The player's
 * client-side spectator roster is read back via clientSimGetSpectatorSlot.
 *
 * Cases (raw spectators bind distinct 127/8 source IPs for their own join
 * rate-limit bucket; Linux-only aliases, skipped with a log otherwise):
 *   join:     a spectator joins while a player is connected → the player's
 *             roster shows that slot connected with the spectator's name.
 *   leave:    the spectator goes idle and times out → the player's roster shows
 *             that slot connected == false.
 *   catch-up: a spectator is seated first, then a fresh player joins → the new
 *             player receives the current roster and sees the slot connected.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAP_STR_SIZE */
#include "platform_net.h"      /* sockets, struct sockaddr_in */
#include "wire_limits.h"       /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"          /* PACKET_* types, PACKET_HEADER_SIZE, BOLO_VERSION_*, JOIN_COOKIE_LEN */
#include "transport_udp.h"     /* WBN_JOIN_KEY_WIRE_LEN, JOIN_FLAG_SPECTATOR, MAX_SPECTATORS, count getter */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "server_sim.h"        /* serverSimSetMaxSpectators, serverSimGetNumPlayers */
#include "client_sim.h"        /* ClientSpectatorSlot, clientSim* */
#include "client_net.h"        /* clientSimConnectUdp, clientSimNetTick, clientSimDisconnect */
#include "client_connect_state.h" /* CLIENT_CONNECT_CONNECTED */
#include "test_harness.h"
#include "loopback_harness.h"

#define SRP_REPLY_DEADLINE_MS 10000
#define SRP_PLAYER_CONNECT    4000
#define SRP_SEE_SLOT          2000
#define SRP_TIMEOUT_PUMPS     1400
#define SRP_CATCHUP_PUMPS     4000
#define SRP_COOKIE_WINDOW_SEC 16
#define SRP_BOUNDARY_RETRIES  4

static uint64_t srpCookieWindow(void) {
    return (SDL_GetTicks() / 1000ULL) / SRP_COOKIE_WINDOW_SEC;
}

/* Build a JOIN_REQUEST: version triple, name, flags byte, trailing cookie. */
static int srpBuildJoin(uint8_t *buf, const char *name, uint8_t flags,
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

static SOCKET srpOpenSocketOnIp(const char *ip) {
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

/* Send one JOIN and pump until an accept/reject/challenge arrives. */
static void srpDriveOnce(LoopbackHarness *h, SOCKET sock,
                         const uint8_t *join, int joinLen,
                         const struct sockaddr_in *server,
                         bool *gotAccept, bool *gotChallenge, bool *gotReject,
                         uint8_t *outCookie) {
    Uint64 deadline;
    *gotAccept = false; *gotChallenge = false; *gotReject = false;
    sendto(sock, (const char *)join, joinLen, 0,
           (const struct sockaddr *)server, sizeof(*server));
    deadline = SDL_GetTicks() + SRP_REPLY_DEADLINE_MS;
    while (SDL_GetTicks() < deadline) {
        uint8_t in[1024];
        int n;
        bool drained = false;
        loopbackHarnessPump(h);
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in), server)) > 0) {
            uint8_t type = getPacketType(in, n);
            if (type == PACKET_JOIN_ACCEPT && n > PACKET_HEADER_SIZE) {
                *gotAccept = true;
            } else if (type == PACKET_JOIN_REJECT) {
                *gotReject = true;
            } else if (type == PACKET_JOIN_CHALLENGE &&
                       n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                *gotChallenge = true;
                if (outCookie != NULL) {
                    memcpy(outCookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
                }
            }
            drained = true;
        }
        if (*gotAccept || *gotReject || *gotChallenge) return;
        if (!drained) SDL_Delay(1);
    }
}

/* Full spectator handshake (cookieless → challenge → echo). Returns true if a
 * JOIN_ACCEPT arrived. */
static bool srpSpectatorJoins(LoopbackHarness *h, SOCKET s,
                              const struct sockaddr_in *server,
                              const char *name) {
    uint8_t joinBuf[1024];
    uint8_t cookie[JOIN_COOKIE_LEN];
    bool gotAccept, gotChallenge, gotReject;
    int attempt;

    for (attempt = 0; attempt < SRP_BOUNDARY_RETRIES; attempt++) {
        uint64_t w0 = srpCookieWindow();
        int joinLen = srpBuildJoin(joinBuf, name, JOIN_FLAG_SPECTATOR, NULL);
        srpDriveOnce(h, s, joinBuf, joinLen, server,
                     &gotAccept, &gotChallenge, &gotReject, cookie);
        if (gotAccept) return true;
        if (gotReject) return false;
        if (!gotChallenge) return false;

        joinLen = srpBuildJoin(joinBuf, name, JOIN_FLAG_SPECTATOR, cookie);
        srpDriveOnce(h, s, joinBuf, joinLen, server,
                     &gotAccept, &gotChallenge, &gotReject, NULL);
        if (gotAccept) return true;
        if (gotReject) return false;
        if (srpCookieWindow() - w0 >= 2) continue;   /* cookie aged out — retry */
        return false;
    }
    return false;
}

/* Client-side roster scan: index of the slot showing `name` connected, or -1. */
static int srpRosterConnected(struct ClientSim *cs, const char *name) {
    int i;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        const ClientSpectatorSlot *sl = clientSimGetSpectatorSlot(cs, (uint8_t)i);
        if (sl != NULL && sl->connected && strcmp(sl->playerName, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool pred_player_connected(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_sees_spec(LoopbackHarness *h, void *u) {
    return srpRosterConnected(h->cs, (const char *)u) >= 0;
}

static bool pred_spec_gone(LoopbackHarness *h, void *u) {
    int idx = *(const int *)u;
    const ClientSpectatorSlot *sl = clientSimGetSpectatorSlot(h->cs, (uint8_t)idx);
    return sl != NULL && !sl->connected;
}

int run_spectator_roster_publish(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    int joinIdx;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    /* Open viewer slots so the spectator-accept path admits the raw JOINs. */
    serverSimSetMaxSpectators(h.sim, 4);

    /* Let the harness player settle into CONNECTED so it is subscribed to the
     * control bus and applies inbound CTRL_SPECTATOR_SLOT events. */
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, SRP_PLAYER_CONNECT,
                                           pred_player_connected, NULL) > 0,
                  "harness player never reached CONNECTED");

    /* ---- join: a spectator joins → player's roster shows it connected ---- */
    {
        SOCKET s = srpOpenSocketOnIp("127.0.0.2");
        if (s == INVALID_SOCKET) {
            SDL_Log("  spectator roster: skipping join/leave — 127.0.0.2 not bindable");
            joinIdx = -1;
        } else {
            UT_ASSERT_MSG(srpSpectatorJoins(&h, s, &serverAddr, "SpecJoin"),
                          "spectator JOIN drew no accept");
            joinIdx = loopbackHarnessPumpUntil(&h, SRP_SEE_SLOT,
                                               pred_sees_spec, (void *)"SpecJoin");
            UT_ASSERT_MSG(joinIdx > 0,
                          "player never saw the joined spectator on its roster");
            joinIdx = srpRosterConnected(h.cs, "SpecJoin");
            UT_ASSERT_MSG(joinIdx >= 0, "joined spectator slot vanished");
            {
                const ClientSpectatorSlot *sl =
                    clientSimGetSpectatorSlot(h.cs, (uint8_t)joinIdx);
                UT_ASSERT_MSG(sl != NULL && sl->connected,
                              "spectator slot must read connected");
                UT_ASSERT_MSG(strcmp(sl->playerName, "SpecJoin") == 0,
                              "spectator slot name mismatch: '%s'", sl->playerName);
            }

            /* ---- leave: idle spectator times out → slot connected == false ---- */
            closesocket(s);   /* stop all spectator traffic */
            UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, SRP_TIMEOUT_PUMPS,
                                                   pred_spec_gone, &joinIdx) > 0,
                          "player never saw the spectator leave its roster");
            {
                const ClientSpectatorSlot *sl =
                    clientSimGetSpectatorSlot(h.cs, (uint8_t)joinIdx);
                UT_ASSERT_MSG(sl != NULL && !sl->connected,
                              "spectator slot must read disconnected after timeout");
            }
        }
    }

    /* ---- catch-up: seat a spectator, then a fresh player gets the roster ---- */
    {
        SOCKET s = srpOpenSocketOnIp("127.0.0.3");
        if (s == INVALID_SOCKET) {
            SDL_Log("  spectator roster: skipping catch-up — 127.0.0.3 not bindable");
        } else {
            ClientSim *cs2;
            int it, seen = -1;

            UT_ASSERT_MSG(srpSpectatorJoins(&h, s, &serverAddr, "SpecCatch"),
                          "catch-up spectator JOIN drew no accept");

            cs2 = clientSimAlloc();
            UT_ASSERT_MSG(cs2 != NULL, "second client alloc failed");
            clientSimCreate(cs2);
            UT_ASSERT_MSG(clientSimConnectUdp(cs2, "127.0.0.1", h.port, "Player2",
                                              /*fallbackCountry*/ "", /*password*/ "",
                                              /*wbnApiToken*/ NULL, /*wbnServerKey*/ NULL,
                                              /*wantRejoin*/ false, /*trackerAddr*/ "",
                                              /*trackerPort*/ 0, /*spectator*/ false),
                          "second player connect failed");

            /* Pump both endpoints: the new player completes its join (catch-up
             * fires at accept) and applies the spectator roster row. */
            for (it = 0; it < SRP_CATCHUP_PUMPS; it++) {
                clientSimNetTick(cs2);
                loopbackHarnessPump(&h);
                seen = srpRosterConnected(cs2, "SpecCatch");
                if (seen >= 0) break;
            }
            UT_ASSERT_MSG(seen >= 0,
                          "late-joining player never received the spectator roster");

            clientSimDisconnect(cs2);
            clientSimDestroy(cs2);
            closesocket(s);
        }
    }

    loopbackHarnessStop(&h);
    return 0;
}
