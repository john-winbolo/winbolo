/*
 * Spectator live↔delayed cutover tests.
 *
 * A spectator that connects during the lobby is a live control-bus subscriber;
 * at game start it must cut over to the delayed ring, and only after it has
 * drained the delayed game does it flip back to the live lobby. These legs pin
 * the server-side cutover:
 *
 *   boundary    — the release gate. A live spectator is unsubscribed inside
 *                 transportUdpServerOnGameStart, which runs before any
 *                 running-state publish, so a CTRL_GAME_PHASE_RUNNING published
 *                 afterwards reaches it zero times (its control seq holds). This
 *                 is the proof the in-game delay is never undercut by the live
 *                 lobby path. Deterministic: the hook and the publish are both
 *                 driven directly on this thread with no pump in between.
 *   delayed_arm — after a real countdown→running cutover with a non-zero delay,
 *                 the spectator is no longer live and is served from the delayed
 *                 ring (its forward feed advances) rather than receiving live
 *                 state. The ring records the lobby/countdown continuously, so
 *                 head-delay first sits in retained pre-game history (the cutover
 *                 countdown holds there — see cutover_cd) and once the delayed
 *                 view reaches the game the spectator seeds and its feed advances.
 *                 Needs pumped ring time enough to clear the countdown.
 *   cutover_cd  — with a non-zero delay the cut-over spectator is held on the
 *                 "spectating begins in X" countdown until head-delay reaches
 *                 the game's first record (gameStartSeq), so it never replays the
 *                 recorded pre-game lobby; the remaining tracks toward zero and
 *                 game content seeds only once the delayed view reaches the game.
 *                 At delay 0 there is no countdown — the game seeds immediately.
 *   drain_flip  — at -specdelay 0 (which collapses the ring time without
 *                 bypassing the gate — segSpec==segHead && state==lobby is still
 *                 the real path), a spectator that drained the delayed game
 *                 flips back to live when the live game returns to the lobby.
 *                 Needs pumped ring time; the test acks the spectator's bulk
 *                 each pump (transportUdpServerTestSpectatorAckBulk) to keep the
 *                 delayed feed draining toward the boundary.
 *
 * Real spectators are seated over the raw-socket cookie handshake on distinct
 * 127/8 source IPs (the join rate-limit bucket is per source IP), mirroring
 * test_spectator_live_bus.c / test_spectator_seed.c. A leg whose alias is not
 * bindable (non-Linux) is skipped with a log, and the test passes.
 *
 * The empty-room reset path (serverSimResetGameWorld → serverStateLobby) opens
 * a lobby segment identical to the gameover→lobby path and so exercises the same
 * segSpec==segHead && state==lobby flip; it is not driven here because the
 * loopback harness always seats a human, and the last-human-leaves check ends
 * the round via gameover before the empty-reset timer can fire (see the report).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"                 /* MAX_TANKS, MAP_STR_SIZE */
#include "platform_net.h"           /* sockets, struct sockaddr_in */
#include "wire_limits.h"            /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"               /* PACKET_*, PACKET_HEADER_SIZE, BOLO_VERSION_*, JOIN_COOKIE_LEN */
#include "transport_udp.h"          /* JOIN_FLAG_SPECTATOR, MAX_SPECTATORS, accessors, OnGameStart */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "server_sim.h"             /* serverSim*, ServerState */
#include "server_sim_lifecycle.h"   /* serverSimEnterGameOver */
#include "server_lifecycle.h"       /* serverInstanceCreateSpectatorRing */
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
#define LB_RUN_ITERS         600   /* generous budget for countdown→running */
#define LB_FEED_ITERS        300   /* seed + forward-feed catch-up */
#define LB_FLIP_ITERS        600   /* gameover hold (150) + drain + flip */

/* ── Raw-socket spectator JOIN (cookie handshake), per test_spectator_live_bus.c ── */

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

static bool lbServerRunning(LoopbackHarness *h, void *u) {
    (void)u;
    return serverSimGetState(h->sim) == serverStateRunning;
}

/* The spectator's forward feed has armed (it is seeded and draining the delayed
 * game). Ack its bulk each pump so the seed completes and the feed advances. */
static bool lbSpectatorFeeding(LoopbackHarness *h, void *u) {
    uint32_t seq = 0;
    (void)h; (void)u;
    transportUdpServerTestSpectatorAckBulk(0);
    return transportUdpServerGetSpectatorFeedSeq(0, &seq, NULL) && seq > 0;
}

/* The spectator drained the delayed game and flipped back to the live lobby.
 * Keep acking its bulk so the feed reaches the game→lobby boundary. */
static bool lbSpectatorWentLive(LoopbackHarness *h, void *u) {
    (void)h; (void)u;
    transportUdpServerTestSpectatorAckBulk(0);
    return transportUdpServerGetSpectatorLive(0);
}

/* The live game has returned to the lobby. Ack the spectator's bulk each pump so
 * its delayed feed keeps draining through the game-over hold. */
static bool lbStateLobby(LoopbackHarness *h, void *u) {
    (void)u;
    transportUdpServerTestSpectatorAckBulk(0);
    return serverSimGetState(h->sim) == serverStateLobby;
}

/* Watch for the delayed→live flip while recording how far the feed drained
 * before it: each pump acks the bulk; while still delayed it remembers the
 * spectator's feed position (the teardown zeroes it once live, so the last value
 * seen while delayed is the position the flip fired at); it stops on the flip. */
typedef struct {
    uint32_t lastFeed;
} TailFlipWatch;

static bool lbTailFlipWatch(LoopbackHarness *h, void *u) {
    TailFlipWatch *w = (TailFlipWatch *)u;
    uint32_t seq = 0;
    (void)h;
    transportUdpServerTestSpectatorAckBulk(0);
    if (transportUdpServerGetSpectatorLive(0)) {
        return true;   /* flipped */
    }
    if (transportUdpServerGetSpectatorFeedSeq(0, &seq, NULL)) {
        w->lastFeed = seq;
    }
    return false;
}

/* The cutover countdown is armed: the spectator is delayed, has seeded NO game
 * record yet (its feed seq holds at 0), and is in countdown. Captures the
 * remaining into *user so the caller can watch it track toward zero. */
static bool lbSpecCountingDown(LoopbackHarness *h, void *u) {
    uint32_t *rem = (uint32_t *)u;
    uint32_t seq = 0;
    (void)h;
    if (transportUdpServerGetSpectatorLive(0)) return false;
    if (transportUdpServerGetSpectatorFeedSeq(0, &seq, NULL) && seq != 0) {
        return false;
    }
    return transportUdpServerGetSpectatorCountdown(0, rem);
}

/* ── Leg 1: anti-cheat boundary (the release gate, deterministic) ──────────── */

static int lbLegBoundary(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET spec;
    ControlEvent evt;
    uint32_t seqAfterSeat;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_PLAYER_CONNECT,
                                           lbPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    serverSimSetMaxSpectators(h.sim, 4);
    lbServerAddr(&serverAddr, h.port);
    spec = lbOpenSocketOnIp("127.0.0.60");
    if (spec == INVALID_SOCKET) {
        SDL_Log("  cutover boundary: skipping — 127.0.0.60 not bindable");
        loopbackHarnessStop(&h);
        return 0;
    }
    UT_ASSERT_MSG(lbSpectatorJoins(&h, spec, &serverAddr, "SpecCut"),
                  "spectator JOIN drew no accept");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                  "accepted spectator must be counted once");
    /* A lobby spectator is a live control-bus subscriber: its sync replay
     * advanced its control channel. */
    UT_ASSERT_MSG(transportUdpServerGetSpectatorLive(0),
                  "lobby spectator must be live");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) > 0,
                  "lobby spectator's control channel never advanced");

    /* Capture the seq, then drive the cutover hook directly (no pump in
     * between, so the only possible writer of the spectator's control channel
     * across the next step is the publish below). */
    seqAfterSeat = transportUdpServerGetSpectatorControlSeq(0);
    transportUdpServerOnGameStart(h.sim);
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorLive(0),
                  "spectator must be cut to delayed (unsubscribed) at game start");

    /* The running-phase publish must reach the now-delayed spectator zero
     * times — the anti-cheat boundary. Its control seq must hold. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_RUNNING;
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) == seqAfterSeat,
                  "CTRL_GAME_PHASE_RUNNING leaked to a former-live spectator "
                  "(control seq %u -> %u)", seqAfterSeat,
                  transportUdpServerGetSpectatorControlSeq(0));

    closesocket(spec);
    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 2: the delayed path arms after a real cutover ─────────────────────── */

static int lbLegDelayedArm(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET spec;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 2u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_PLAYER_CONNECT,
                                           lbPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    /* Open viewer slots, set a non-zero delay, and register the ring (mirrors
     * the production order — the delay must be set before the ring is sized). */
    serverSimSetMaxSpectators(h.sim, 4);
    serverSimSetSpecDelayTicks(h.sim, 100);
    serverInstanceCreateSpectatorRing(h.sim);

    lbServerAddr(&serverAddr, h.port);
    spec = lbOpenSocketOnIp("127.0.0.61");
    if (spec == INVALID_SOCKET) {
        SDL_Log("  cutover delayed-arm: skipping — 127.0.0.61 not bindable");
        loopbackHarnessStop(&h);
        return 0;
    }
    UT_ASSERT_MSG(lbSpectatorJoins(&h, spec, &serverAddr, "SpecDelay"),
                  "spectator JOIN drew no accept");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorLive(0),
                  "lobby spectator must be live before game start");

    /* Drive a real countdown→running, which cuts the spectator over. */
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "trigger game start failed (client has no slot)");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_RUN_ITERS,
                                           lbServerRunning, NULL) > 0,
                  "server never reached running");
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorLive(0),
                  "spectator must be delayed after the live→delayed cutover");

    /* The ring records the lobby/countdown continuously, so head - delay first
     * sits in retained pre-game history; the cutover countdown holds there (see
     * cutover_cd) until the delayed view reaches the game, then the spectator
     * seeds. Ack its bulk so the seed completes and the forward feed advances,
     * proving it is served from the delayed ring, not live state. It stays
     * delayed for the whole running game (the flip gate needs a lobby/countdown
     * state). LB_FEED_ITERS covers the countdown window (delay=100) plus seed. */
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_FEED_ITERS,
                                           lbSpectatorFeeding, NULL) > 0,
                  "delayed spectator never began its forward feed after cutover");
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorLive(0),
                  "spectator must stay delayed while the game runs");

    closesocket(spec);
    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 3: drain → flip at running→lobby (gameover→lobby, delay 0) ────────── */

static int lbLegDrainFlip(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET spec;
    uint32_t seqBeforeFlip;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 3u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_PLAYER_CONNECT,
                                           lbPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    /* delay 0 reads the ring at its head, collapsing the ring time the flip
     * needs while still exercising the real segSpec==segHead && state==lobby
     * gate. */
    serverSimSetMaxSpectators(h.sim, 4);
    serverSimSetSpecDelayTicks(h.sim, 0);
    serverInstanceCreateSpectatorRing(h.sim);

    lbServerAddr(&serverAddr, h.port);
    spec = lbOpenSocketOnIp("127.0.0.62");
    if (spec == INVALID_SOCKET) {
        SDL_Log("  cutover drain-flip: skipping — 127.0.0.62 not bindable");
        loopbackHarnessStop(&h);
        return 0;
    }
    UT_ASSERT_MSG(lbSpectatorJoins(&h, spec, &serverAddr, "SpecDrain"),
                  "spectator JOIN drew no accept");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorLive(0),
                  "lobby spectator must be live before game start");

    /* Cut over to delayed at game start. */
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "trigger game start failed (client has no slot)");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_RUN_ITERS,
                                           lbServerRunning, NULL) > 0,
                  "server never reached running");
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorLive(0),
                  "spectator must be delayed after the live→delayed cutover");

    /* Seed and start draining the delayed game (acks bulk each pump). */
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_FEED_ITERS,
                                           lbSpectatorFeeding, NULL) > 0,
                  "delayed spectator never began its forward feed");

    seqBeforeFlip = transportUdpServerGetSpectatorControlSeq(0);

    /* End the game. The state machine holds in gameover, then returns to the
     * lobby (serverSimResetGameWorld → serverStateLobby), opening the lobby
     * segment the spectator's read head drains into. */
    serverSimEnterGameOver(h.sim);
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_FLIP_ITERS,
                                           lbSpectatorWentLive, NULL) > 0,
                  "spectator never flipped back to live after draining the "
                  "delayed game into the lobby");

    /* Re-registering on the bus replays the current live lobby in one step, so
     * the control channel jumps. */
    UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) > seqBeforeFlip,
                  "flip back to live did not deliver a fresh lobby sync "
                  "(control seq held at %u)", seqBeforeFlip);
    UT_ASSERT_MSG(serverSimGetState(h.sim) == serverStateLobby,
                  "live state must be lobby when the spectator flips");

    closesocket(spec);
    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 4: tail is drained before the flip (gameover→lobby, delay > 0) ─────── */

static int lbLegTailDrainFlip(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET spec;
    const uint32_t delay = 30;   /* records of tail to drain before the flip */
    uint32_t feedAtLobby = 0;
    TailFlipWatch w;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 4u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_PLAYER_CONNECT,
                                           lbPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    serverSimSetMaxSpectators(h.sim, 4);
    serverSimSetSpecDelayTicks(h.sim, delay);
    serverInstanceCreateSpectatorRing(h.sim);

    lbServerAddr(&serverAddr, h.port);
    spec = lbOpenSocketOnIp("127.0.0.63");
    if (spec == INVALID_SOCKET) {
        SDL_Log("  cutover tail-drain: skipping — 127.0.0.63 not bindable");
        loopbackHarnessStop(&h);
        return 0;
    }
    UT_ASSERT_MSG(lbSpectatorJoins(&h, spec, &serverAddr, "SpecTail"),
                  "spectator JOIN drew no accept");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorLive(0),
                  "lobby spectator must be live before game start");

    /* Cut over to delayed at game start, then seed and track at head - delay. */
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "trigger game start failed (client has no slot)");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_RUN_ITERS,
                                           lbServerRunning, NULL) > 0,
                  "server never reached running");
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorLive(0),
                  "spectator must be delayed after the live→delayed cutover");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_FEED_ITERS,
                                           lbSpectatorFeeding, NULL) > 0,
                  "delayed spectator never began its forward feed");

    /* End the game and pump through the game-over hold to the lobby. Game-over
     * ticks are not recorded, so the last running record X is frozen as the bar
     * (lastRunningSeq) while the spectator rests at X - delay. */
    serverSimEnterGameOver(h.sim);
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_FLIP_ITERS,
                                           lbStateLobby, NULL) > 0,
                  "live game never returned to the lobby");

    /* At the lobby transition the spectator has NOT yet drained the gameover
     * tail (records (X - delay, X]) — those are only replayed once lobby
     * recording advances the ring head past them — so it must still be delayed.
     * The pre-fix gate flipped here, at X - delay, skipping the ending; this
     * assertion is what guards against that regression. */
    UT_ASSERT_MSG(transportUdpServerGetSpectatorFeedSeq(0, &feedAtLobby, NULL),
                  "spectator feed position unavailable at the lobby transition");
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorLive(0),
                  "spectator must NOT flip at the lobby transition — it still "
                  "has the gameover tail to drain");

    /* Now drain the tail: the spectator stays delayed for ~delay more records,
     * then flips once it has replayed through the last running record. */
    w.lastFeed = feedAtLobby;
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_FLIP_ITERS,
                                           lbTailFlipWatch, &w) > 0,
                  "spectator never flipped back to live after draining the tail");

    /* The feed advanced through the tail (~delay records) before the flip — the
     * proof the ending was watched, not skipped. */
    UT_ASSERT_MSG(w.lastFeed >= feedAtLobby + (delay - 3),
                  "spectator flipped before draining the gameover tail "
                  "(feed advanced %u, expected ~%u)",
                  w.lastFeed - feedAtLobby, delay);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorLive(0),
                  "spectator must be live after draining the tail");
    UT_ASSERT_MSG(serverSimGetState(h.sim) == serverStateLobby,
                  "live state must be lobby when the spectator flips");

    closesocket(spec);
    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 5: cutover "spectating begins in X" countdown (delay>0 and delay=0) ── */

static int lbLegCutoverCountdown(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET spec;
    const uint32_t delay = 100;
    uint32_t rem1 = 0, rem2 = 0, seq = 0;
    int k;

    /* ── delay > 0: a cut-over spectator counts down to the game, then seeds ── */
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 5u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_PLAYER_CONNECT,
                                           lbPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    serverSimSetMaxSpectators(h.sim, 4);
    serverSimSetSpecDelayTicks(h.sim, delay);
    serverInstanceCreateSpectatorRing(h.sim);

    lbServerAddr(&serverAddr, h.port);
    spec = lbOpenSocketOnIp("127.0.0.64");
    if (spec == INVALID_SOCKET) {
        SDL_Log("  cutover countdown: skipping — 127.0.0.64 not bindable");
        loopbackHarnessStop(&h);
        return 0;
    }
    UT_ASSERT_MSG(lbSpectatorJoins(&h, spec, &serverAddr, "SpecCD"),
                  "spectator JOIN drew no accept");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorLive(0),
                  "lobby spectator must be live before game start");

    /* Record a stretch of pre-game lobby so head - delay lands inside it at the
     * cutover (this is exactly the recorded-lobby segment the spectator must NOT
     * be made to replay), then drive a real countdown→running. */
    for (k = 0; k < (int)delay + 20; k++) loopbackHarnessPump(&h);
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "trigger game start failed (client has no slot)");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_RUN_ITERS,
                                           lbServerRunning, NULL) > 0,
                  "server never reached running");
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorLive(0),
                  "spectator must be delayed after the live→delayed cutover");

    /* It arms the cutover countdown rather than seeding the delayed pre-game
     * lobby: in countdown, remaining > 0, and zero game content seeded. */
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_FEED_ITERS,
                                           lbSpecCountingDown, &rem1) > 0,
                  "cutover spectator never armed the 'begins in X' countdown");
    UT_ASSERT_MSG(rem1 > 0, "cutover countdown remaining must be > 0");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorFeedSeq(0, &seq, NULL) && seq == 0,
                  "cutover spectator must seed no game content while counting down");

    /* Remaining tracks toward zero as the live head advances — no bulk acks are
     * needed, the countdown rides the spectator's own control channel. */
    for (k = 0; k < 30; k++) loopbackHarnessPump(&h);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCountdown(0, &rem2),
                  "cutover spectator left the countdown before reaching the game");
    UT_ASSERT_MSG(rem2 < rem1,
                  "cutover countdown did not track toward zero (%u -> %u)",
                  rem1, rem2);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorFeedSeq(0, &seq, NULL) && seq == 0,
                  "cutover spectator seeded game content before the countdown ended");

    /* Once head - delay reaches gameStartSeq the countdown ends and the delayed
     * game seeds. Ack the bulk each pump so the seed completes and the feed
     * advances; the budget covers the rest of the delay window plus the seed. */
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_RUN_ITERS,
                                           lbSpectatorFeeding, NULL) > 0,
                  "cutover spectator never seeded the delayed game after the "
                  "countdown");
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorCountdown(0, &rem2),
                  "countdown must clear once the delayed game seeds");

    closesocket(spec);
    loopbackHarnessStop(&h);

    /* ── delay == 0: head - delay == head >= gameStartSeq, so no countdown — the
     * game seeds immediately at cutover (must not regress). ── */
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 6u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_PLAYER_CONNECT,
                                           lbPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    serverSimSetMaxSpectators(h.sim, 4);
    serverSimSetSpecDelayTicks(h.sim, 0);
    serverInstanceCreateSpectatorRing(h.sim);

    lbServerAddr(&serverAddr, h.port);
    spec = lbOpenSocketOnIp("127.0.0.65");
    if (spec == INVALID_SOCKET) {
        SDL_Log("  cutover countdown (delay 0): skipping — 127.0.0.65 not bindable");
        loopbackHarnessStop(&h);
        return 0;
    }
    UT_ASSERT_MSG(lbSpectatorJoins(&h, spec, &serverAddr, "SpecCD0"),
                  "spectator JOIN drew no accept");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorLive(0),
                  "lobby spectator must be live before game start");

    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "trigger game start failed (client has no slot)");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_RUN_ITERS,
                                           lbServerRunning, NULL) > 0,
                  "server never reached running");
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorLive(0),
                  "spectator must be delayed after the live→delayed cutover");

    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, LB_FEED_ITERS,
                                           lbSpectatorFeeding, NULL) > 0,
                  "delay=0 spectator never seeded the game immediately");
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorCountdown(0, &rem1),
                  "delay=0 spectator must not enter a cutover countdown");

    closesocket(spec);
    loopbackHarnessStop(&h);
    return 0;
}

int run_spectator_lobby_cutover(void) {
    int rc;
    if ((rc = lbLegBoundary())        != 0) return rc;
    if ((rc = lbLegDelayedArm())      != 0) return rc;
    if ((rc = lbLegDrainFlip())       != 0) return rc;
    if ((rc = lbLegTailDrainFlip())   != 0) return rc;
    if ((rc = lbLegCutoverCountdown()) != 0) return rc;
    return 0;
}
