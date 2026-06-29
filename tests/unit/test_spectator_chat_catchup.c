/*
 * Lobby-chat catch-up buffer — the server-side current-session backlog a
 * spectator replays on its delayed->live drain-flip.
 *
 * While a spectator watches the delayed game, the live lobby keeps accumulating
 * chat the spectator is off-bus for; its sync replay on the flip carries roster
 * but not chat, so without a buffer that chat is lost. serverSim keeps a capped
 * current-session buffer of broadcast player chat (CTRL_CHAT, destPlayer 0xFF)
 * and spectator chat (CTRL_SPECTATOR_CHAT) captured while in lobby/countdown,
 * cleared at game start, and re-delivered by serverSimReplayLobbyChat at the
 * drain-flip only. This pins the buffer's semantics directly on the sim:
 *
 *   - a fresh subscriber's sync replay carries NO backlog (fresh joins get
 *     nothing); the explicit replay re-delivers the captured lines in order,
 *     and team/unicast chat is never captured;
 *   - game start clears the buffer, so a returning spectator sees this
 *     session's lobby chat, not the previous game's;
 *   - the buffer caps and drops the oldest.
 *
 * The end-to-end flip (replay reaching a real spectator client's lobby log) is
 * exercised in test_loopback_spectator_lobby.c's lobby->game->lobby cycle.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"             /* serverSim*, ServerState */
#include "server_sim_lifecycle.h"   /* serverSimStartGame */
#include "control_event.h"
#include "wire_limits.h"            /* PACKET_MAX_CHAT_MESSAGE */
#include "threads.h"                /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

/* Mirrors LOBBY_CHAT_BUFFER_MAX in server_sim_internal.h (not visible here). */
#define CATCHUP_CAP 200

/* Records the broadcast-chat / spectator-chat events a deliver callback sees,
 * in order, so a sync replay (must be empty of chat) and an explicit
 * serverSimReplayLobbyChat (the backlog) can each be inspected. Sized to hold a
 * full capped buffer with room to spare. */
typedef struct {
    int     count;
    uint8_t types[CATCHUP_CAP + 8];
    char    bodies[CATCHUP_CAP + 8][PACKET_MAX_CHAT_MESSAGE + 1];
} ChatCollector;

static void ccDeliver(void *ctx, const struct ControlEvent *evt) {
    ChatCollector *c = (ChatCollector *)ctx;
    const void *body;
    uint16_t n;

    if (evt->type == CTRL_CHAT) {
        body = evt->u.chat.body;
        n    = evt->u.chat.bodyLen;
    } else if (evt->type == CTRL_SPECTATOR_CHAT) {
        body = evt->u.spectatorChat.body;
        n    = evt->u.spectatorChat.bodyLen;
    } else {
        return;   /* phase / settings / roster sync events are not the subject */
    }
    if (c->count >= (int)(sizeof(c->types) / sizeof(c->types[0]))) return;
    if (n > PACKET_MAX_CHAT_MESSAGE) n = PACKET_MAX_CHAT_MESSAGE;
    c->types[c->count] = (uint8_t)evt->type;
    if (n > 0) memcpy(c->bodies[c->count], body, n);
    c->bodies[c->count][n] = '\0';
    c->count++;
}

/* ── Leg 1: capture broadcast + spectator chat; fresh subscriber gets none ── */

static int legCaptureReplayFreshNone(void) {
    LoopbackHarness h;
    ChatCollector sync, replay;
    SubscriberHandle sub;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "ChatCap", /*seed*/ 31u),
                  "spectator lobby harness start failed");

    threadsWaitForMutex();
    serverSimReceiveChat(h.sim, 0, 0xFF, "alpha", 5);   /* broadcast — captured */
    serverSimReceiveSpectatorChat(h.sim, 0, "beta", 4); /* spectator — captured */
    serverSimReceiveChat(h.sim, 0, 1,    "uni",  3);    /* unicast — NOT captured */
    serverSimReceiveChat(h.sim, 0, 0x81, "team", 4);    /* team   — NOT captured */

    /* A fresh subscriber's sync replay must carry no chat backlog: the buffer
     * is replayed only at the drain-flip, never on register/accept. */
    memset(&sync, 0, sizeof(sync));
    sub = serverSimRegisterSubscriber(h.sim, ccDeliver, &sync);
    threadsReleaseMutex();

    UT_ASSERT_MSG(sub != SUBSCRIBER_HANDLE_INVALID, "subscriber register failed");
    UT_ASSERT_MSG(sync.count == 0,
                  "a fresh subscriber must get no lobby-chat backlog (saw %d)",
                  sync.count);

    /* The drain-flip replay re-delivers exactly the captured broadcast +
     * spectator lines, oldest first; team/unicast were never captured. */
    memset(&replay, 0, sizeof(replay));
    threadsWaitForMutex();
    serverSimReplayLobbyChat(h.sim, ccDeliver, &replay);
    serverSimUnregisterSubscriber(h.sim, sub);
    threadsReleaseMutex();

    UT_ASSERT_MSG(replay.count == 2,
                  "replay must deliver exactly the 2 captured lobby lines (saw %d)",
                  replay.count);
    UT_ASSERT_MSG(replay.types[0] == CTRL_CHAT &&
                  strcmp(replay.bodies[0], "alpha") == 0,
                  "first replayed line must be the broadcast 'alpha'");
    UT_ASSERT_MSG(replay.types[1] == CTRL_SPECTATOR_CHAT &&
                  strcmp(replay.bodies[1], "beta") == 0,
                  "second replayed line must be the spectator 'beta'");

    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 2: game start clears the buffer; in-game chat is not captured ────── */

static int legClearAtGameStart(void) {
    LoopbackHarness h;
    ChatCollector replay;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "ChatClr", /*seed*/ 32u),
                  "spectator lobby harness start failed");

    threadsWaitForMutex();
    serverSimReceiveChat(h.sim, 0, 0xFF, "old", 3);     /* previous-session lobby */
    serverSimStartGame(h.sim);                          /* clears the buffer */
    serverSimReceiveChat(h.sim, 0, 0xFF, "ingame", 6);  /* running — NOT captured */
    serverSimSetState(h.sim, serverStateLobby);         /* post-game lobby */
    serverSimReceiveChat(h.sim, 0, 0xFF, "new", 3);     /* captured */
    memset(&replay, 0, sizeof(replay));
    serverSimReplayLobbyChat(h.sim, ccDeliver, &replay);
    threadsReleaseMutex();

    UT_ASSERT_MSG(replay.count == 1,
                  "after a game start only the post-game lobby line survives "
                  "(saw %d)", replay.count);
    UT_ASSERT_MSG(replay.types[0] == CTRL_CHAT &&
                  strcmp(replay.bodies[0], "new") == 0,
                  "previous-session chat must be cleared at game start; only "
                  "'new' remains (got '%s')", replay.bodies[0]);

    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 3: the cap holds — the 201st message drops the oldest ───────────── */

static int legCapDropsOldest(void) {
    LoopbackHarness h;
    ChatCollector replay;
    char body[16];
    char want[16];
    int i, n;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "ChatCap2", /*seed*/ 33u),
                  "spectator lobby harness start failed");

    threadsWaitForMutex();
    for (i = 0; i <= CATCHUP_CAP; i++) {   /* 201 lines: m0 .. m200 */
        n = snprintf(body, sizeof(body), "m%d", i);
        serverSimReceiveChat(h.sim, 0, 0xFF, body, (size_t)n);
    }
    memset(&replay, 0, sizeof(replay));
    serverSimReplayLobbyChat(h.sim, ccDeliver, &replay);
    threadsReleaseMutex();

    UT_ASSERT_MSG(replay.count == CATCHUP_CAP,
                  "buffer must cap at %d lines (saw %d)", CATCHUP_CAP, replay.count);
    UT_ASSERT_MSG(strcmp(replay.bodies[0], "m1") == 0,
                  "the oldest line (m0) must be dropped; first is now 'm1' "
                  "(got '%s')", replay.bodies[0]);
    snprintf(want, sizeof(want), "m%d", CATCHUP_CAP);
    UT_ASSERT_MSG(strcmp(replay.bodies[CATCHUP_CAP - 1], want) == 0,
                  "the newest line (%s) must be retained (got '%s')",
                  want, replay.bodies[CATCHUP_CAP - 1]);

    loopbackHarnessStop(&h);
    return 0;
}

int run_spectator_chat_catchup(void) {
    int rc;
    if ((rc = legCaptureReplayFreshNone()) != 0) return rc;
    if ((rc = legClearAtGameStart())       != 0) return rc;
    if ((rc = legCapDropsOldest())         != 0) return rc;
    return 0;
}
