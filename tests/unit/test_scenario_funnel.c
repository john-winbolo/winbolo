/*
 * The scenario write door and the two re-entrancy rules behind it.
 *
 * serverSimApplyScenarioOp is the only way a scenario changes the
 * world. Its prelude refuses an op issued from inside a policy
 * callback and an op issued while a game start is running; everything
 * past the prelude is an arm, and an op whose arm has not been written
 * answers SCN_OP_UNSUPPORTED rather than quietly doing nothing. These
 * tests hold the prelude, the registrations beside it, the per-tick
 * callback the host drains from, and the start-in-progress flag that
 * keeps the all-ready detector out of a start already under way.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "server_sim_scenario.h"
#include "control_event.h"
#include "everard_map.h"
#include "test_harness.h"

/* Every member of ScenarioOpType, in the order the enum declares them.
 * Listed out rather than walked as a range so a member added without a
 * line here shows up as a count mismatch below. */
static const ScenarioOpType kAllOpTypes[] = {
    SCN_OP_NONE,
    SCN_OP_TANK_SET_STOCKS,
    SCN_OP_TANK_KILL,
    SCN_OP_TANK_TELEPORT,
    SCN_OP_TANK_SET_BOAT,
    SCN_OP_TANK_GIVE_PILL,
    SCN_OP_TANK_DROP_PILL,
    SCN_OP_TANK_SET_MODIFIERS,
    SCN_OP_LGM_DISPATCH,
    SCN_OP_LGM_RECALL,
    SCN_OP_LGM_KILL,
    SCN_OP_LGM_PARACHUTE,
    SCN_OP_LGM_SET_CARRIED,
    SCN_OP_PILL_SET_OWNER,
    SCN_OP_PILL_SET_ARMOUR,
    SCN_OP_PILL_SET_SPEED,
    SCN_OP_PILL_MOVE,
    SCN_OP_BASE_SET_OWNER,
    SCN_OP_BASE_SET_STOCK,
    SCN_OP_ENTITY_ADD_PILL,
    SCN_OP_ENTITY_REMOVE_PILL,
    SCN_OP_ENTITY_ADD_BASE,
    SCN_OP_ENTITY_REMOVE_BASE,
    SCN_OP_ENTITY_ADD_START,
    SCN_OP_ENTITY_REMOVE_START,
    SCN_OP_MAP_SET_TILE,
    SCN_OP_MAP_FILL_RECT,
    SCN_OP_MAP_PLACE_MINE,
    SCN_OP_MAP_REMOVE_MINE,
    SCN_OP_ROSTER_SPAWN_BOT,
    SCN_OP_ROSTER_REMOVE_BOT,
    SCN_OP_ROSTER_SET_TEAM,
    SCN_OP_LOBBY_ADD_BOT,
    SCN_OP_LOBBY_REMOVE_BOT,
    SCN_OP_LOBBY_SET_TEAM,
    SCN_OP_BOT_HINT,
    SCN_OP_MSG_ALL,
    SCN_OP_MSG_TEAM,
    SCN_OP_MSG_PLAYER,
    SCN_OP_SOUND,
    SCN_OP_LOG,
    SCN_OP_PANEL,
    SCN_OP_SCORE,
    SCN_OP_ANNOUNCE,
    SCN_OP_MARKER,
    SCN_OP_END_ROUND,
    SCN_OP_SET_GAME_TIME,
    SCN_OP_SET_RULE
};
#define NUM_OP_TYPES ((int)(sizeof(kAllOpTypes) / sizeof(kAllOpTypes[0])))

static ServerSim *makeLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Ready", false);
    /* One human, ready: everything the all-ready detector asks for. */
    sim->lobbyPlayers[0].ready = TRUE;
    return sim;
}

/* ── The funnel ────────────────────────────────────────────────── */

/* A type value that is not a member of the enum is refused like any
 * other op instead of falling off the end of the switch. */
int run_scenario_op_unknown_type_unsupported(void) {
    ServerSim *sim = makeLobbySim();
    ScenarioOp op;
    UT_ASSERT(sim != NULL);

    memset(&op, 0, sizeof(op));
    op.type = (ScenarioOpType)(SCN_OP_SET_RULE + 100);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_UNSUPPORTED,
                  "an out-of-range op type must be refused");

    serverSimDestroy(sim);
    return 0;
}

/* The ops whose arms are written. Each one is covered by its own tests; this
 * list is what keeps the sweep below honest as they land one at a time. */
static bool opArmHasLanded(ScenarioOpType t) {
    return t == SCN_OP_TANK_SET_MODIFIERS;   /* test_tank_modifiers.c */
}

/* An op with no arm answers UNSUPPORTED, and an op with one does not. The
 * count check catches a member added to the enum without a line in
 * kAllOpTypes, which would otherwise leave it untested. */
int run_scenario_op_every_type_unsupported(void) {
    ServerSim *sim = makeLobbySim();
    int i;
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(NUM_OP_TYPES == (int)SCN_OP_SET_RULE + 1,
                  "kAllOpTypes covers %d types but the enum declares %d",
                  NUM_OP_TYPES, (int)SCN_OP_SET_RULE + 1);

    for (i = 0; i < NUM_OP_TYPES; i++) {
        ScenarioOp op;
        ScnOpOut out;
        ScnOpResult r;
        memset(&op, 0, sizeof(op));
        memset(&out, 0, sizeof(out));
        op.type = kAllOpTypes[i];
        r = serverSimApplyScenarioOp(sim, &op, &out);
        if (opArmHasLanded(op.type)) {
            /* The sim is in the lobby, so a landed arm refuses on the state
               or the slot — what it must not do is claim it has no arm. */
            UT_ASSERT_MSG(r != SCN_OP_UNSUPPORTED,
                          "op type %d has an arm but still answers "
                          "SCN_OP_UNSUPPORTED", (int)op.type);
            continue;
        }
        UT_ASSERT_MSG(r == SCN_OP_UNSUPPORTED,
                      "op type %d returned %d, expected SCN_OP_UNSUPPORTED — "
                      "an arm has landed without a line in opArmHasLanded",
                      (int)op.type, (int)r);
    }

    serverSimDestroy(sim);
    return 0;
}

/* The sim stores the vtable pointer and neither copies nor owns it, so
 * the last registration is what it holds and NULL clears it. */
int run_scenario_policy_register_replace_clear(void) {
    ServerSim *sim = makeLobbySim();
    ScenarioPolicy first, second;
    UT_ASSERT(sim != NULL);

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));

    UT_ASSERT_MSG(sim->scenarioPolicy == NULL,
                  "a fresh sim must start with no policy");

    serverSimSetScenarioPolicy(sim, &first);
    UT_ASSERT(sim->scenarioPolicy == &first);

    serverSimSetScenarioPolicy(sim, &second);
    UT_ASSERT_MSG(sim->scenarioPolicy == &second,
                  "a second registration must replace the first");

    serverSimSetScenarioPolicy(sim, NULL);
    UT_ASSERT_MSG(sim->scenarioPolicy == NULL,
                  "NULL must clear the registration");

    serverSimDestroy(sim);
    return 0;
}

/* A policy callback answers a question the engine asked mid-operation.
 * It may not write back through the funnel while it does. */
int run_scenario_op_refused_in_policy(void) {
    ServerSim *sim = makeLobbySim();
    ScenarioOp op;
    UT_ASSERT(sim != NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_KILL;

    serverSimScenarioPolicyEnter(sim);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_IN_POLICY,
                  "an op issued from inside a policy call must be refused");

    serverSimScenarioPolicyLeave(sim);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_UNSUPPORTED,
                  "leaving the policy call must let ops through again");

    serverSimDestroy(sim);
    return 0;
}

/* A start rebuilds the roster, the tanks and the state, so nothing may
 * be written until it finishes. */
int run_scenario_op_refused_during_start(void) {
    ServerSim *sim = makeLobbySim();
    ScenarioOp op;
    UT_ASSERT(sim != NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_SET_TILE;

    sim->startInProgress = true;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE,
                  "an op issued during a start must be refused");

    sim->startInProgress = false;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_UNSUPPORTED);

    serverSimDestroy(sim);
    return 0;
}

/* ── The per-tick callback ─────────────────────────────────────── */

static int s_tickCalls;
static void *s_tickCtxSeen;

static void countingTick(void *ctx) {
    s_tickCalls++;
    s_tickCtxSeen = ctx;
}

/* The host drains its queued events from this, so it has to run in the
 * lobby as well as in a running game — lobby hooks are hooks too. */
int run_scenario_tick_called_both_branches(void) {
    int marker = 0;
    ServerSim *running = ut_make_running_sim("Tester");
    ServerSim *lobby;
    UT_ASSERT(running != NULL);

    /* No registration: the sim ticks and calls nothing. */
    s_tickCalls = 0;
    s_tickCtxSeen = NULL;
    serverSimTick(running);
    UT_ASSERT_MSG(s_tickCalls == 0,
                  "an unregistered tick must not be invoked");

    serverSimSetScenarioTick(running, countingTick, &marker);
    s_tickCalls = 0;
    serverSimTick(running);
    UT_ASSERT_MSG(s_tickCalls == 1,
                  "running branch called the tick %d times, expected 1",
                  s_tickCalls);
    UT_ASSERT_MSG(s_tickCtxSeen == &marker,
                  "the tick must be handed the ctx it was registered with");
    serverSimTick(running);
    UT_ASSERT_MSG(s_tickCalls == 2, "one call per frame, got %d", s_tickCalls);
    serverSimDestroy(running);

    lobby = makeLobbySim();
    UT_ASSERT(lobby != NULL);
    UT_ASSERT(serverSimGetState(lobby) == serverStateLobby);
    serverSimSetScenarioTick(lobby, countingTick, &marker);
    s_tickCalls = 0;
    serverSimTick(lobby);
    UT_ASSERT_MSG(s_tickCalls == 1,
                  "lobby branch called the tick %d times, expected 1",
                  s_tickCalls);
    serverSimDestroy(lobby);
    return 0;
}

/* ── The start-in-progress flag ────────────────────────────────── */

/* A subscriber watching the publishes a start makes. The window that
 * matters runs from the first publish inside serverSimStartGameInPlace
 * (the alliance reset) to the state flip near its end: the state still
 * reads lobby and every player is still marked ready, which is exactly
 * what the all-ready detector acts on. */
typedef struct {
    ServerSim *sim;
    bool armed;          /* set just before the start, so the registration's
                          * own state replay is not mistaken for it */
    bool reenter;        /* call the all-ready detector from the callback */
    bool sawPublish;
    bool firstWasLobby;
    bool firstSawFlag;
    bool reentered;
    int  runningPhases;  /* CTRL_GAME_PHASE_RUNNING publishes seen */
} StartWatch;

static void startWatchDeliver(void *ctx, const struct ControlEvent *evt) {
    StartWatch *w = (StartWatch *)ctx;
    if (!w->armed) return;
    if (evt->type == CTRL_GAME_PHASE_RUNNING) {
        w->runningPhases++;
    }
    if (w->sawPublish) return;
    w->sawPublish = true;
    w->firstWasLobby = (w->sim->state == serverStateLobby);
    w->firstSawFlag = w->sim->startInProgress;
    if (w->reenter && w->firstWasLobby) {
        w->reentered = true;
        serverSimLobbyCheckAllReady(w->sim);
    }
}

/* The flag covers the window: at the start's first publish the state
 * still reads lobby and startInProgress is already set. */
int run_scenario_start_flag_set_during_start(void) {
    ServerSim *sim = makeLobbySim();
    StartWatch w;
    SubscriberHandle h;
    UT_ASSERT(sim != NULL);

    memset(&w, 0, sizeof(w));
    w.sim = sim;
    h = serverSimRegisterSubscriber(sim, startWatchDeliver, &w);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    w.armed = true;
    serverSimLobbyCheckAllReady(sim);

    UT_ASSERT_MSG(w.sawPublish, "the start published nothing to watch");
    UT_ASSERT_MSG(w.firstWasLobby,
                  "the start's first publish should still read lobby");
    UT_ASSERT_MSG(w.firstSawFlag,
                  "startInProgress must already be set at the first publish");
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* The detector, re-entered from inside that window, returns at its
 * first line — so the start under way is the only one that runs and
 * exactly one game comes out of it. */
int run_scenario_start_guard_blocks_reentry(void) {
    ServerSim *sim = makeLobbySim();
    StartWatch w;
    SubscriberHandle h;
    UT_ASSERT(sim != NULL);

    memset(&w, 0, sizeof(w));
    w.sim = sim;
    w.reenter = true;
    h = serverSimRegisterSubscriber(sim, startWatchDeliver, &w);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    w.armed = true;
    serverSimLobbyCheckAllReady(sim);

    UT_ASSERT_MSG(w.reentered,
                  "the callback never reached the detector, so nothing was held");
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the start should have finished normally");
    UT_ASSERT_MSG(w.runningPhases == 1,
                  "%d running-phase publishes: a second game started on top "
                  "of the first", w.runningPhases);
    UT_ASSERT_MSG(!sim->startInProgress,
                  "the flag must not survive the start it covered");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* Both start functions clear the flag on the way out. A start that
 * leaves it set wedges the lobby: the detector returns at its first
 * line forever and no later ready-up can begin a round. */
int run_scenario_start_flag_cleared_after_start(void) {
    ServerSim *inPlace = makeLobbySim();
    ServerSim *fullReset;
    UT_ASSERT(inPlace != NULL);

    serverSimLobbyCheckAllReady(inPlace);
    UT_ASSERT(serverSimGetState(inPlace) == serverStateRunning);
    UT_ASSERT_MSG(!inPlace->startInProgress,
                  "serverSimStartGameInPlace left the flag set");

    /* And the detector still acts once the flag is clear: back in the
     * lobby with the same ready player and the same untouched world, a
     * ready-up starts a round as it did before. */
    inPlace->state = serverStateLobby;
    inPlace->lobbyPlayers[0].ready = TRUE;
    inPlace->worldPreLoaded = TRUE;
    serverSimLobbyCheckAllReady(inPlace);
    UT_ASSERT_MSG(serverSimGetState(inPlace) == serverStateRunning,
                  "an ordinary all-ready check stopped working");
    serverSimDestroy(inPlace);

    fullReset = makeLobbySim();
    UT_ASSERT(fullReset != NULL);
    serverSimStartGame(fullReset);
    UT_ASSERT(serverSimGetState(fullReset) == serverStateRunning);
    UT_ASSERT_MSG(!fullReset->startInProgress,
                  "serverSimStartGame left the flag set");
    serverSimDestroy(fullReset);
    return 0;
}
