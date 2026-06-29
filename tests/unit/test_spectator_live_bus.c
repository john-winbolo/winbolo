/*
 * Live-lobby spectator control-bus tests.
 *
 * A spectator that connects during the lobby/countdown is a real-time
 * control-bus subscriber (serverSimRegisterSubscriber), not a delayed-ring
 * reader. These four tests pin the server-side wiring that establishes:
 *
 *   lobby_subscribe      — a subscriber registered while the sim is in lobby
 *                          gets the sync replay a live spectator's registration
 *                          triggers (CTRL_GAME_PHASE_* first, a connected
 *                          CTRL_LOBBY_SLOT, terminal CTRL_LOBBY_SYNC_COMPLETE),
 *                          and a real spectator seated in lobby is registered
 *                          (its CHANNEL_CONTROL advances from the replay).
 *   control_filter       — serverSpectatorDeliverControl is a drop-by-default
 *                          allowlist: player-targeted variants hold the
 *                          spectator's control seq; broadcast lobby control and
 *                          0xFF chat advance it.
 *   subscriber_capacity  — MAX_TANKS + MAX_SPECTATORS subscribers all register
 *                          (guards the SUBSCRIBER_SLOT_COUNT bump); past
 *                          capacity returns SUBSCRIBER_HANDLE_INVALID, never a
 *                          silent drop.
 *   roster_to_spectators — the enumerator seeds the spectator roster into every
 *                          new subscriber's sync replay, and a roster broadcast
 *                          fans CTRL_SPECTATOR_SLOT to live spectators.
 *
 * The transport-dependent cases stand up the loopback harness (lobby mode) and
 * seat real spectators over the raw-socket cookie handshake (mirroring
 * test_spectator_seed.c / test_spectator_roster_publish.c). What a spectator
 * received is read via the white-box transportUdpServerGetSpectatorControlSeq:
 * while a spectator is live the delayed seed/feed path is gated off, so
 * serverSpectatorDeliverControl is the only writer of its CHANNEL_CONTROL — a
 * per-event publish + seq-delta cleanly isolates pass (advances) from drop
 * (holds), with no wire decode and no races (the sim ticks only on this thread).
 *
 * Cases that need a distinct loopback source IP for a spectator's join
 * rate-limit bucket are skipped (logged, test passes) when the 127/8 alias
 * isn't bindable (Linux-only).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"                 /* MAX_TANKS, MAP_STR_SIZE */
#include "platform_net.h"           /* sockets, struct sockaddr_in */
#include "wire_limits.h"            /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"               /* PACKET_*, PACKET_HEADER_SIZE, BOLO_VERSION_*, JOIN_COOKIE_LEN */
#include "transport_udp.h"          /* JOIN_FLAG_SPECTATOR, MAX_SPECTATORS, WBN_JOIN_KEY_WIRE_LEN, accessors */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "server_sim.h"             /* serverSim* */
#include "client_sim.h"
#include "client_net.h"             /* clientSimGetConnectState */
#include "client_connect_state.h"   /* CLIENT_CONNECT_CONNECTED */
#include "control_event.h"          /* ControlEvent, CTRL_* */
#include "test_harness.h"
#include "loopback_harness.h"

#define LB_REPLY_DEADLINE_MS 10000
#define LB_BOUNDARY_RETRIES  4
#define LB_COOKIE_WINDOW_SEC 16
#define LB_PLAYER_CONNECT    4000

/* ── Raw-socket spectator JOIN (cookie handshake), per test_spectator_seed.c ── */

static uint64_t lbCookieWindow(void) {
    return (SDL_GetTicks() / 1000ULL) / LB_COOKIE_WINDOW_SEC;
}

static int lbBuildJoin(uint8_t *buf, const char *name, uint8_t flags,
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

static SOCKET lbOpenSocketOnIp(const char *ip) {
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

static void lbDriveOnce(LoopbackHarness *h, SOCKET sock,
                        const uint8_t *join, int joinLen,
                        const struct sockaddr_in *server,
                        bool *gotAccept, bool *gotChallenge, bool *gotReject,
                        uint8_t *outCookie) {
    Uint64 deadline;
    *gotAccept = false; *gotChallenge = false; *gotReject = false;
    sendto(sock, (const char *)join, joinLen, 0,
           (const struct sockaddr *)server, sizeof(*server));
    deadline = SDL_GetTicks() + LB_REPLY_DEADLINE_MS;
    while (SDL_GetTicks() < deadline) {
        uint8_t in[1024];
        int n;
        bool drained = false;
        loopbackHarnessPump(h);
        while ((n = (int)recvfrom(sock, (char *)in, sizeof(in), 0, NULL, NULL)) > 0) {
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

/* Full spectator handshake (cookieless -> challenge -> echo). True on ACCEPT. */
static bool lbSpectatorJoins(LoopbackHarness *h, SOCKET s,
                             const struct sockaddr_in *server, const char *name) {
    uint8_t joinBuf[1024];
    uint8_t cookie[JOIN_COOKIE_LEN];
    bool gotAccept, gotChallenge, gotReject;
    int attempt;

    for (attempt = 0; attempt < LB_BOUNDARY_RETRIES; attempt++) {
        uint64_t w0 = lbCookieWindow();
        int joinLen = lbBuildJoin(joinBuf, name, JOIN_FLAG_SPECTATOR, NULL);
        lbDriveOnce(h, s, joinBuf, joinLen, server,
                    &gotAccept, &gotChallenge, &gotReject, cookie);
        if (gotAccept) return true;
        if (gotReject) return false;
        if (!gotChallenge) return false;

        joinLen = lbBuildJoin(joinBuf, name, JOIN_FLAG_SPECTATOR, cookie);
        lbDriveOnce(h, s, joinBuf, joinLen, server,
                    &gotAccept, &gotChallenge, &gotReject, NULL);
        if (gotAccept) return true;
        if (gotReject) return false;
        if (lbCookieWindow() - w0 >= 2) continue;   /* cookie aged out — retry */
        return false;
    }
    return false;
}

static void lbServerAddr(struct sockaddr_in *out, unsigned short port) {
    memset(out, 0, sizeof(*out));
    out->sin_family      = AF_INET;
    out->sin_addr.s_addr = inet_addr("127.0.0.1");
    out->sin_port        = htons(port);
}

static bool lbPlayerConnected(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* ── Sync-replay capture sink ──────────────────────────────────────────────── */

typedef struct {
    int              count;
    ControlEventType types[128];
    bool             sawConnectedLobbySlot;
    bool             sawConnectedSpectatorSlot;
} CapBuf;

static void lbCaptureDeliver(void *ctx, const struct ControlEvent *evt) {
    CapBuf *c = (CapBuf *)ctx;
    if (c->count < (int)(sizeof(c->types) / sizeof(c->types[0]))) {
        c->types[c->count] = evt->type;
    }
    if (evt->type == CTRL_LOBBY_SLOT && evt->u.lobbySlot.slot.connected) {
        c->sawConnectedLobbySlot = true;
    }
    if (evt->type == CTRL_SPECTATOR_SLOT && evt->u.spectatorSlot.slot.connected) {
        c->sawConnectedSpectatorSlot = true;
    }
    c->count++;
}

/* ── Test 1: lobby subscribe ───────────────────────────────────────────────── */

int run_spectator_lobby_subscribe(void) {
    LoopbackHarness h;
    CapBuf cap;
    SubscriberHandle sub;
    int last;
    struct sockaddr_in serverAddr;
    SOCKET spec;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_PLAYER_CONNECT,
                                           lbPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    /* A subscriber registered while the sim is in lobby gets the same sync
     * replay a live-lobby spectator's registration triggers. */
    memset(&cap, 0, sizeof(cap));
    sub = serverSimRegisterSubscriber(h.sim, lbCaptureDeliver, &cap);
    UT_ASSERT_MSG(sub != SUBSCRIBER_HANDLE_INVALID, "subscriber register failed");
    UT_ASSERT_MSG(cap.count >= 3 && cap.count <= 128,
                  "sync replay delivered %d events (out of range)", cap.count);
    UT_ASSERT_MSG(cap.types[0] == CTRL_GAME_PHASE_LOBBY,
                  "first sync event must be CTRL_GAME_PHASE_LOBBY, got %d",
                  (int)cap.types[0]);
    UT_ASSERT_MSG(cap.sawConnectedLobbySlot,
                  "sync replay carried no connected CTRL_LOBBY_SLOT");
    last = cap.count - 1;
    UT_ASSERT_MSG(cap.types[last] == CTRL_LOBBY_SYNC_COMPLETE,
                  "sync replay must end with CTRL_LOBBY_SYNC_COMPLETE, got %d",
                  (int)cap.types[last]);
    serverSimUnregisterSubscriber(h.sim, sub);

    /* A real spectator seated in lobby is registered on the bus: its
     * CHANNEL_CONTROL advances as the registration sync-replay is delivered. */
    serverSimSetMaxSpectators(h.sim, 4);
    lbServerAddr(&serverAddr, h.port);
    spec = lbOpenSocketOnIp("127.0.0.30");
    if (spec == INVALID_SOCKET) {
        SDL_Log("  lobby subscribe: skipping real-spectator leg — 127.0.0.30 not bindable");
    } else {
        UT_ASSERT_MSG(lbSpectatorJoins(&h, spec, &serverAddr, "SpecLobby"),
                      "spectator JOIN drew no accept");
        UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                      "accepted spectator must be counted once");
        UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) > 0,
                      "lobby spectator's control channel never advanced "
                      "(not registered / sync replay never reached it)");
        closesocket(spec);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* ── Test 2: control allowlist ─────────────────────────────────────────────── */

int run_spectator_control_filter(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET spec;
    ControlEvent evt;
    uint32_t before;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_PLAYER_CONNECT,
                                           lbPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    serverSimSetMaxSpectators(h.sim, 4);
    lbServerAddr(&serverAddr, h.port);
    spec = lbOpenSocketOnIp("127.0.0.40");
    if (spec == INVALID_SOCKET) {
        SDL_Log("  control filter: skipping — 127.0.0.40 not bindable");
        loopbackHarnessStop(&h);
        return 0;
    }
    UT_ASSERT_MSG(lbSpectatorJoins(&h, spec, &serverAddr, "SpecFilter"),
                  "spectator JOIN drew no accept");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                  "accepted spectator must be counted once");
    /* serverAcceptSpectator fills the first free slot from 0, so the lone
     * spectator is slot 0. No pump from here on — serverSimPublishControl
     * delivers synchronously, so each seq delta is solely this publish. */

    /* DROP set — player-targeted variants the allowlist rejects before encode,
     * so the discriminating field is all that need be set. Seq must hold. */
#define LB_EXPECT_DROP(label)                                                   \
    do {                                                                        \
        before = transportUdpServerGetSpectatorControlSeq(0);                   \
        serverSimPublishControl(h.sim, &evt);                                   \
        UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) == before,    \
                      "%s leaked to spectator (control seq advanced)", label);  \
    } while (0)

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_REQUEST;
    evt.u.allianceRequest.fromPlayer = 1;
    evt.u.allianceRequest.toPlayer   = 2;
    LB_EXPECT_DROP("CTRL_ALLIANCE_REQUEST");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 1;
    evt.u.chat.destPlayer = 0;        /* unicast */
    evt.u.chat.bodyLen    = 0;
    LB_EXPECT_DROP("CTRL_CHAT unicast");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 1;
    evt.u.chat.destPlayer = 0x81;     /* team-addressed */
    evt.u.chat.bodyLen    = 0;
    LB_EXPECT_DROP("CTRL_CHAT team");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_COMMAND_REJECTED;
    evt.u.commandRejected.origSlot = 3;
    LB_EXPECT_DROP("CTRL_COMMAND_REJECTED");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SHELL_DEATH;
    evt.u.shellDeath.owner = 3;
    LB_EXPECT_DROP("CTRL_SHELL_DEATH");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BALANCE_FAILED;
    evt.u.balanceFailed.reasonCode = 1;
    LB_EXPECT_DROP("CTRL_BALANCE_FAILED");

#undef LB_EXPECT_DROP

    /* PASS set — broadcast lobby control + 0xFF chat. Seq must advance by one. */
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(h.sim, &evt);
    before = transportUdpServerGetSpectatorControlSeq(0);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) == before + 1,
                  "broadcast CTRL_LOBBY_SETTINGS did not reach spectator "
                  "(seq %u -> %u)", before,
                  transportUdpServerGetSpectatorControlSeq(0));

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 0;
    evt.u.chat.destPlayer = 0xFF;     /* broadcast */
    evt.u.chat.bodyLen    = 0;
    before = transportUdpServerGetSpectatorControlSeq(0);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) == before + 1,
                  "broadcast CTRL_CHAT (0xFF) did not reach spectator "
                  "(seq %u -> %u)", before,
                  transportUdpServerGetSpectatorControlSeq(0));

    closesocket(spec);
    loopbackHarnessStop(&h);
    return 0;
}

/* ── Test 3: subscriber capacity ───────────────────────────────────────────── */

static void lbNoopDeliver(void *ctx, const struct ControlEvent *evt) {
    (void)ctx;
    (void)evt;
}

int run_spectator_subscriber_capacity(void) {
    ServerSim *sim = ut_make_running_sim("Cap");
    const int cap = MAX_TANKS + 1 + MAX_SPECTATORS;   /* SUBSCRIBER_SLOT_COUNT */
    SubscriberHandle handles[MAX_TANKS + 1 + MAX_SPECTATORS];
    int n = 0, i;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    for (i = 0; i < cap; i++) {
        SubscriberHandle hh = serverSimRegisterSubscriber(sim, lbNoopDeliver, NULL);
        if (hh == SUBSCRIBER_HANDLE_INVALID) break;
        handles[n++] = hh;
    }
    UT_ASSERT_MSG(n >= MAX_TANKS + MAX_SPECTATORS,
                  "only %d subscribers registered; expected >= %d "
                  "(SUBSCRIBER_SLOT_COUNT bump missing?)",
                  n, MAX_TANKS + MAX_SPECTATORS);
    /* Past capacity returns INVALID, not a silent drop. */
    UT_ASSERT_MSG(serverSimRegisterSubscriber(sim, lbNoopDeliver, NULL)
                  == SUBSCRIBER_HANDLE_INVALID,
                  "registration past capacity must return SUBSCRIBER_HANDLE_INVALID");

    for (i = 0; i < n; i++) {
        serverSimUnregisterSubscriber(sim, handles[i]);
    }
    serverSimDestroy(sim);
    return 0;
}

/* ── Test 4: roster to spectators ──────────────────────────────────────────── */

int run_spectator_roster_to_spectators(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET specA, specB;
    CapBuf cap;
    SubscriberHandle sub;
    uint32_t seqA;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_PLAYER_CONNECT,
                                           lbPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    serverSimSetMaxSpectators(h.sim, 4);
    lbServerAddr(&serverAddr, h.port);

    specA = lbOpenSocketOnIp("127.0.0.50");
    if (specA == INVALID_SOCKET) {
        SDL_Log("  roster to spectators: skipping — 127.0.0.50 not bindable");
        loopbackHarnessStop(&h);
        return 0;
    }
    UT_ASSERT_MSG(lbSpectatorJoins(&h, specA, &serverAddr, "SpecA"),
                  "spectator A JOIN drew no accept");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                  "spectator A must be counted once");

    /* (a) Enumerator: a new subscriber's sync replay now carries A's roster
     * row (a connected CTRL_SPECTATOR_SLOT) via serverEnumSpectatorRoster. */
    memset(&cap, 0, sizeof(cap));
    sub = serverSimRegisterSubscriber(h.sim, lbCaptureDeliver, &cap);
    UT_ASSERT_MSG(sub != SUBSCRIBER_HANDLE_INVALID, "subscriber register failed");
    UT_ASSERT_MSG(cap.sawConnectedSpectatorSlot,
                  "sync replay carried no connected CTRL_SPECTATOR_SLOT "
                  "(enumerator not seeding the roster)");
    serverSimUnregisterSubscriber(h.sim, sub);

    /* (b) Fan: a roster broadcast reaches live spectators. Seating spectator B
     * runs serverBroadcastSpectatorSlot(B), which fans B's row to live
     * spectators including A — so A's control channel advances. */
    seqA = transportUdpServerGetSpectatorControlSeq(0);   /* A is slot 0 */
    specB = lbOpenSocketOnIp("127.0.0.51");
    if (specB == INVALID_SOCKET) {
        SDL_Log("  roster to spectators: skipping fan leg — 127.0.0.51 not bindable");
    } else {
        UT_ASSERT_MSG(lbSpectatorJoins(&h, specB, &serverAddr, "SpecB"),
                      "spectator B JOIN drew no accept");
        UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 2,
                      "spectator B must be counted (2 total)");
        UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) > seqA,
                      "spectator B's roster row never fanned to live spectator A "
                      "(seq held at %u)", seqA);
        closesocket(specB);
    }

    closesocket(specA);
    loopbackHarnessStop(&h);
    return 0;
}
