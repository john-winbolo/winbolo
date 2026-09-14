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
    return t == SCN_OP_TANK_SET_MODIFIERS ||  /* test_tank_modifiers.c */
           t == SCN_OP_TANK_SET_STOCKS ||     /* test_scenario_tank_arms.c */
           t == SCN_OP_TANK_KILL ||
           t == SCN_OP_TANK_TELEPORT ||
           t == SCN_OP_TANK_SET_BOAT ||
           t == SCN_OP_TANK_GIVE_PILL ||
           t == SCN_OP_TANK_DROP_PILL ||
           t == SCN_OP_LGM_DISPATCH ||        /* test_scenario_builder_arms.c */
           t == SCN_OP_LGM_RECALL ||
           t == SCN_OP_LGM_KILL ||
           t == SCN_OP_LGM_PARACHUTE ||
           t == SCN_OP_LGM_SET_CARRIED ||
           t == SCN_OP_PILL_SET_OWNER ||     /* test_scenario_pill_base_arms.c */
           t == SCN_OP_PILL_SET_ARMOUR ||
           t == SCN_OP_PILL_SET_SPEED ||
           t == SCN_OP_PILL_MOVE ||
           t == SCN_OP_BASE_SET_OWNER ||
           t == SCN_OP_BASE_SET_STOCK ||
           t == SCN_OP_ENTITY_ADD_PILL ||    /* test_scenario_entity_arms.c */
           t == SCN_OP_ENTITY_REMOVE_PILL ||
           t == SCN_OP_ENTITY_ADD_BASE ||
           t == SCN_OP_ENTITY_REMOVE_BASE ||
           t == SCN_OP_ENTITY_ADD_START ||
           t == SCN_OP_ENTITY_REMOVE_START ||
           t == SCN_OP_MAP_SET_TILE ||       /* test_scenario_map_arms.c */
           t == SCN_OP_MAP_FILL_RECT ||
           t == SCN_OP_MAP_PLACE_MINE ||
           t == SCN_OP_MAP_REMOVE_MINE ||
           t == SCN_OP_ROSTER_SPAWN_BOT ||   /* test_scenario_roster_arms.c */
           t == SCN_OP_ROSTER_REMOVE_BOT ||
           t == SCN_OP_ROSTER_SET_TEAM ||
           t == SCN_OP_LOBBY_ADD_BOT ||
           t == SCN_OP_LOBBY_REMOVE_BOT ||
           t == SCN_OP_LOBBY_SET_TEAM ||
           t == SCN_OP_MSG_ALL ||            /* test_scenario_comms_arms.c */
           t == SCN_OP_MSG_TEAM ||
           t == SCN_OP_MSG_PLAYER ||
           t == SCN_OP_SOUND ||
           t == SCN_OP_LOG ||
           t == SCN_OP_END_ROUND ||          /* test_scenario_flow_arms.c */
           t == SCN_OP_SET_GAME_TIME ||
           t == SCN_OP_SET_RULE;             /* test_scenario_rule_arms.c */
}

/* An op with no arm answers UNSUPPORTED, and an op with one does not. The
 * count check catches a member added to the enum without a line in
 * kAllOpTypes, which would otherwise leave it untested. */
int run_scenario_op_every_type_unsupported(void) {
    int i;

    UT_ASSERT_MSG(NUM_OP_TYPES == (int)SCN_OP_SET_RULE + 1,
                  "kAllOpTypes covers %d types but the enum declares %d",
                  NUM_OP_TYPES, (int)SCN_OP_SET_RULE + 1);

    for (i = 0; i < NUM_OP_TYPES; i++) {
        ServerSim *sim = makeLobbySim();
        ScenarioOp op;
        ScnOpOut out;
        ScnOpResult r;
        UT_ASSERT(sim != NULL);

        memset(&op, 0, sizeof(op));
        memset(&out, 0, sizeof(out));
        op.type = kAllOpTypes[i];
        r = serverSimApplyScenarioOp(sim, &op, &out);
        if (opArmHasLanded(op.type)) {
            /* The sim is in the lobby, so most landed arms refuse on the
               state or the slot — what none of them may do is claim to have
               no arm. A zeroed payload names slot 0, which this sim fills
               with a ready human, so an accepting arm writes: the lobby team
               arm moves that slot, and the comms arms publish a line and
               write a console message. Each op therefore gets a sim of its
               own, built and dropped inside the loop, so what one arm leaves
               behind is not what the next one is asked against. */
            UT_ASSERT_MSG(r != SCN_OP_UNSUPPORTED,
                          "op type %d has an arm but still answers "
                          "SCN_OP_UNSUPPORTED", (int)op.type);
            serverSimDestroy(sim);
            continue;
        }
        UT_ASSERT_MSG(r == SCN_OP_UNSUPPORTED,
                      "op type %d returned %d, expected SCN_OP_UNSUPPORTED — "
                      "an arm has landed without a line in opArmHasLanded",
                      (int)op.type, (int)r);
        serverSimDestroy(sim);
    }

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

    /* What the second call proves is that the prelude stopped refusing, not
       what the op then went on to answer. SCN_OP_IN_POLICY is the prelude's
       alone, so asking only that it is gone keeps this case true whatever
       state the op's own arm is in — including having no arm at all. */
    serverSimScenarioPolicyLeave(sim);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) != SCN_OP_IN_POLICY,
                  "leaving the policy call must let ops through again");

    serverSimDestroy(sim);
    return 0;
}

/* A question asked from inside another leaves the funnel shut until the
 * outer one has returned too: the state is a depth, not a flag, so the inner
 * leave cannot open a door the outer caller is still standing in. */
int run_scenario_op_refused_in_nested_policy(void) {
    ServerSim *sim = makeLobbySim();
    ScenarioOp op;
    UT_ASSERT(sim != NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_KILL;

    serverSimScenarioPolicyEnter(sim);
    serverSimScenarioPolicyEnter(sim);
    serverSimScenarioPolicyLeave(sim);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_IN_POLICY,
                  "the inner leave opened the funnel while the outer policy "
                  "call was still running");

    serverSimScenarioPolicyLeave(sim);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) != SCN_OP_IN_POLICY,
                  "the outer leave must let ops through again");

    serverSimDestroy(sim);
    return 0;
}

/* A start rebuilds the roster, the tanks and the state, so nothing may
 * be written until it finishes.
 *
 * The op is one the sim would otherwise carry out — a square well inside
 * the map, terrain a map holds, and an arm that asks nothing of the round
 * state — so the two calls answer differently and the difference is the
 * whole of what the flag does. Refusing on the flag and refusing on the
 * op's own contract are both SCN_OP_WRONG_STATE, so an op the sim would
 * turn down anyway would leave this case unable to tell them apart. */
int run_scenario_op_refused_during_start(void) {
    ServerSim *sim = makeLobbySim();
    ScenarioOp op;
    UT_ASSERT(sim != NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_SET_TILE;
    op.u.mapSetTile.x = 100;
    op.u.mapSetTile.y = 100;
    op.u.mapSetTile.terrain = GRASS;

    sim->startInProgress = true;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_WRONG_STATE,
                  "an op issued during a start must be refused");

    sim->startInProgress = false;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "clearing the flag must let the op through");

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

/* ── The setup window ──────────────────────────────────────────── */

/* A square well inside Everard Island, and two carried rules with room to
 * move: tank_reload_ticks sits in a 0..255 row and tank_death_ticks in a
 * 0..65535 one, and neither is half of a pair. */
#define SW_TILE_X     100
#define SW_TILE_Y     100
#define SW_RELOAD_SET 20
#define SW_DEATH_SET  600

/* What the round-start callback found while it ran, and what the funnel
 * answered the ops it issued from there. One struct for every case below;
 * each callback fills the fields its case reads and leaves the rest. */
typedef struct {
    ServerSim  *sim;
    int         calls;
    void       *ctxSeen;
    bool        sawStartInProgress;
    bool        sawWindow;
    ScnOpResult setRule;
    ScnOpResult mapTile;
    ScnOpResult rosterSpawnBot;
    ScnOpResult rosterRemoveBot;
    ScnOpResult rosterSetTeam;
    ScnOpResult lobbyAddBot;
    ScnOpResult lobbyRemoveBot;
    ScnOpResult lobbySetTeam;
    ScnOpResult ruleReload;
    ScnOpResult ruleDeath;
} SetupWatch;

static ScnOpResult swSetRule(ServerSim *sim, uint16_t rule, double value) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_SET_RULE;
    op.u.setRule.rule  = rule;
    op.u.setRule.value = value;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

static ScnOpResult swMapTile(ServerSim *sim) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_SET_TILE;
    op.u.mapSetTile.x       = SW_TILE_X;
    op.u.mapSetTile.y       = SW_TILE_Y;
    op.u.mapSetTile.terrain = GRASS;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* Records where the callback ran: inside the start, with the window open. */
static void swCount(void *ctx) {
    SetupWatch *w = (SetupWatch *)ctx;
    w->calls++;
    w->ctxSeen = ctx;
    w->sawStartInProgress = w->sim->startInProgress;
    w->sawWindow = w->sim->scenarioSetupWindow;
}

/* Two ops from different groups, neither of them a roster change. */
static void swIssueOps(void *ctx) {
    SetupWatch *w = (SetupWatch *)ctx;
    swCount(ctx);
    w->setRule = swSetRule(w->sim, SCN_RULE_tank_reload_ticks,
                           (double)SW_RELOAD_SET);
    w->mapTile = swMapTile(w->sim);
}

/* Each of the six roster ops, named one at a time. The list lives here in
 * full rather than being walked, so it is written twice — once in the
 * funnel and once here — and a type dropped from the funnel's predicate
 * fails a case that still names it. */
static void swIssueRosterOps(void *ctx) {
    SetupWatch *w = (SetupWatch *)ctx;
    ScenarioOp op;
    swCount(ctx);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_SPAWN_BOT;
    op.u.rosterSpawnBot.slot  = SCN_NONE;
    op.u.rosterSpawnBot.team  = 1;
    op.u.rosterSpawnBot.start = SCN_NONE;
    w->rosterSpawnBot = serverSimApplyScenarioOp(w->sim, &op, NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_REMOVE_BOT;
    op.u.rosterRemoveBot.slot = 0;
    w->rosterRemoveBot = serverSimApplyScenarioOp(w->sim, &op, NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_SET_TEAM;
    op.u.rosterSetTeam.slot = 0;
    op.u.rosterSetTeam.team = 1;
    w->rosterSetTeam = serverSimApplyScenarioOp(w->sim, &op, NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LOBBY_ADD_BOT;
    op.u.lobbyAddBot.slot = SCN_NONE;
    op.u.lobbyAddBot.team = 1;
    w->lobbyAddBot = serverSimApplyScenarioOp(w->sim, &op, NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LOBBY_REMOVE_BOT;
    op.u.lobbyRemoveBot.slot = 0;
    w->lobbyRemoveBot = serverSimApplyScenarioOp(w->sim, &op, NULL);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_LOBBY_SET_TEAM;
    op.u.lobbySetTeam.slot = 0;
    op.u.lobbySetTeam.team = 1;
    w->lobbySetTeam = serverSimApplyScenarioOp(w->sim, &op, NULL);
}

/* Two carried rules, which the handler would each publish for on its own. */
static void swSetTwoRules(void *ctx) {
    SetupWatch *w = (SetupWatch *)ctx;
    swCount(ctx);
    w->ruleReload = swSetRule(w->sim, SCN_RULE_tank_reload_ticks,
                              (double)SW_RELOAD_SET);
    w->ruleDeath  = swSetRule(w->sim, SCN_RULE_tank_death_ticks,
                              (double)SW_DEATH_SET);
}

/* Both starts invoke the callback once, with the ctx it was registered
 * with, and a registration cleared with NULL invokes nothing. */
int run_scenario_round_start_called_both_starts(void) {
    ServerSim *inPlace = makeLobbySim();
    ServerSim *cleared;
    ServerSim *fullReset;
    SetupWatch w;
    SetupWatch wCleared;
    SetupWatch wFull;

    UT_ASSERT(inPlace != NULL);
    memset(&w, 0, sizeof(w));
    w.sim = inPlace;
    serverSimSetScenarioRoundStart(inPlace, swCount, &w);
    serverSimStartGameInPlace(inPlace);
    UT_ASSERT_MSG(w.calls == 1,
                  "serverSimStartGameInPlace invoked the round-start callback "
                  "%d times, expected 1", w.calls);
    UT_ASSERT_MSG(w.ctxSeen == &w,
                  "the callback must be handed the ctx it was registered with");
    UT_ASSERT_MSG(serverSimGetState(inPlace) == serverStateRunning,
                  "the start did not finish");
    serverSimDestroy(inPlace);

    cleared = makeLobbySim();
    UT_ASSERT(cleared != NULL);
    memset(&wCleared, 0, sizeof(wCleared));
    wCleared.sim = cleared;
    serverSimSetScenarioRoundStart(cleared, swCount, &wCleared);
    serverSimSetScenarioRoundStart(cleared, NULL, NULL);
    serverSimStartGameInPlace(cleared);
    UT_ASSERT_MSG(wCleared.calls == 0,
                  "a cleared round-start callback was invoked %d times",
                  wCleared.calls);
    serverSimDestroy(cleared);

    fullReset = makeLobbySim();
    UT_ASSERT(fullReset != NULL);
    memset(&wFull, 0, sizeof(wFull));
    wFull.sim = fullReset;
    serverSimSetScenarioRoundStart(fullReset, swCount, &wFull);
    serverSimStartGame(fullReset);
    UT_ASSERT_MSG(wFull.calls == 1,
                  "serverSimStartGame invoked the round-start callback %d "
                  "times, expected 1", wFull.calls);
    UT_ASSERT_MSG(serverSimGetState(fullReset) == serverStateRunning,
                  "the start did not finish");
    serverSimDestroy(fullReset);
    return 0;
}

/* One start's worth of the window: the callback runs inside the start, the
 * funnel takes non-roster ops from it, and the window is shut again by the
 * time the start returns. Both start functions go through this, so a window
 * opened by only one of them fails rather than passing on the other's
 * strength. */
static int swCheckAdmits(ServerSim *sim, bool inPlace, const char *which) {
    SetupWatch w;
    memset(&w, 0, sizeof(w));
    w.sim = sim;
    serverSimSetScenarioRoundStart(sim, swIssueOps, &w);

    if (inPlace) {
        serverSimStartGameInPlace(sim);
    } else {
        serverSimStartGame(sim);
    }

    UT_ASSERT_MSG(w.calls == 1, "%s invoked the callback %d times, expected 1",
                  which, w.calls);
    UT_ASSERT_MSG(w.sawStartInProgress,
                  "%s: startInProgress was not set inside the callback, so "
                  "the ops below were not issued during a start", which);
    UT_ASSERT_MSG(w.sawWindow,
                  "%s: the setup window was not open inside the callback",
                  which);
    UT_ASSERT_MSG(w.setRule == SCN_OP_OK,
                  "%s: SCN_OP_SET_RULE answered %d inside the window, "
                  "expected SCN_OP_OK", which, (int)w.setRule);
    UT_ASSERT_MSG(w.mapTile == SCN_OP_OK,
                  "%s: SCN_OP_MAP_SET_TILE answered %d inside the window, "
                  "expected SCN_OP_OK", which, (int)w.mapTile);
    UT_ASSERT_MSG(!sim->scenarioSetupWindow,
                  "%s left the setup window open", which);

    /* And the window was what admitted them. With the start flag set again
     * and the window shut, the same op is refused. */
    sim->startInProgress = true;
    UT_ASSERT_MSG(swMapTile(sim) == SCN_OP_WRONG_STATE,
                  "%s: the op still went through once the window was shut",
                  which);
    sim->startInProgress = false;
    return 0;
}

int run_scenario_setup_window_admits_ops(void) {
    ServerSim *inPlace = makeLobbySim();
    ServerSim *fullReset;
    int rc;

    UT_ASSERT(inPlace != NULL);
    rc = swCheckAdmits(inPlace, true, "serverSimStartGameInPlace");
    if (rc != 0) return rc;
    serverSimDestroy(inPlace);

    fullReset = makeLobbySim();
    UT_ASSERT(fullReset != NULL);
    rc = swCheckAdmits(fullReset, false, "serverSimStartGame");
    if (rc != 0) return rc;
    serverSimDestroy(fullReset);
    return 0;
}

/* The six the window does not admit. A roster edit from inside a start
 * re-enters the all-ready detector, which is what the start-in-progress
 * guard exists for, so these keep refusing while everything else goes
 * through. */
int run_scenario_setup_window_roster_refused(void) {
    ServerSim *sim = makeLobbySim();
    ScenarioOp op;
    SetupWatch w;

    UT_ASSERT(sim != NULL);
    memset(&w, 0, sizeof(w));
    w.sim = sim;
    serverSimSetScenarioRoundStart(sim, swIssueRosterOps, &w);
    serverSimStartGameInPlace(sim);

    UT_ASSERT_MSG(w.calls == 1, "the callback ran %d times, expected 1",
                  w.calls);
    UT_ASSERT_MSG(w.rosterSpawnBot == SCN_OP_WRONG_STATE,
                  "SCN_OP_ROSTER_SPAWN_BOT answered %d inside the window, "
                  "expected SCN_OP_WRONG_STATE", (int)w.rosterSpawnBot);
    UT_ASSERT_MSG(w.rosterRemoveBot == SCN_OP_WRONG_STATE,
                  "SCN_OP_ROSTER_REMOVE_BOT answered %d inside the window, "
                  "expected SCN_OP_WRONG_STATE", (int)w.rosterRemoveBot);
    UT_ASSERT_MSG(w.rosterSetTeam == SCN_OP_WRONG_STATE,
                  "SCN_OP_ROSTER_SET_TEAM answered %d inside the window, "
                  "expected SCN_OP_WRONG_STATE", (int)w.rosterSetTeam);
    UT_ASSERT_MSG(w.lobbyAddBot == SCN_OP_WRONG_STATE,
                  "SCN_OP_LOBBY_ADD_BOT answered %d inside the window, "
                  "expected SCN_OP_WRONG_STATE", (int)w.lobbyAddBot);
    UT_ASSERT_MSG(w.lobbyRemoveBot == SCN_OP_WRONG_STATE,
                  "SCN_OP_LOBBY_REMOVE_BOT answered %d inside the window, "
                  "expected SCN_OP_WRONG_STATE", (int)w.lobbyRemoveBot);
    UT_ASSERT_MSG(w.lobbySetTeam == SCN_OP_WRONG_STATE,
                  "SCN_OP_LOBBY_SET_TEAM answered %d inside the window, "
                  "expected SCN_OP_WRONG_STATE", (int)w.lobbySetTeam);

    /* The same team change goes through once the start has finished, so the
     * refusal above is the window's answer and not the round's. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_SET_TEAM;
    op.u.rosterSetTeam.slot = 0;
    op.u.rosterSetTeam.team = 1;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "SCN_OP_ROSTER_SET_TEAM was refused after the start too, so "
                  "the refusal inside the window proves nothing");

    serverSimDestroy(sim);
    return 0;
}

/* Outside the window the guard is what it always was: a start in progress
 * refuses every op there is, whatever group it belongs to. */
int run_scenario_setup_window_shut_refuses_all(void) {
    ServerSim *sim = makeLobbySim();
    ScenarioOp op;
    int i;

    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(!sim->scenarioSetupWindow,
                  "a fresh sim must start with the window shut");

    sim->startInProgress = true;
    for (i = 0; i < NUM_OP_TYPES; i++) {
        memset(&op, 0, sizeof(op));
        op.type = kAllOpTypes[i];
        UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL)
                          == SCN_OP_WRONG_STATE,
                      "op type %d was not refused during a start with the "
                      "window shut", (int)kAllOpTypes[i]);
    }
    sim->startInProgress = false;

    serverSimDestroy(sim);
    return 0;
}

/* A subscriber that reads the window flag at every publish a start makes. */
typedef struct {
    ServerSim *sim;
    bool       armed;
    int        publishes;
    bool       sawWindowOpen;
} WindowWatch;

static void windowWatchDeliver(void *ctx, const struct ControlEvent *evt) {
    WindowWatch *w = (WindowWatch *)ctx;
    (void)evt;
    if (!w->armed) return;
    w->publishes++;
    if (w->sim->scenarioSetupWindow) {
        w->sawWindowOpen = true;
    }
}

/* A sim with no callback registered never has the window open: not at any
 * publish the start makes, and not once the start has returned. */
static int swCheckNoWindow(ServerSim *sim, bool inPlace, const char *which) {
    WindowWatch w;
    SubscriberHandle h;

    memset(&w, 0, sizeof(w));
    w.sim = sim;
    h = serverSimRegisterSubscriber(sim, windowWatchDeliver, &w);
    UT_ASSERT_MSG(h != SUBSCRIBER_HANDLE_INVALID, "%s: no subscriber slot",
                  which);
    w.armed = true;

    UT_ASSERT_MSG(!sim->scenarioSetupWindow,
                  "%s: the window was open before the start", which);
    if (inPlace) {
        serverSimStartGameInPlace(sim);
    } else {
        serverSimStartGame(sim);
    }

    UT_ASSERT_MSG(w.publishes > 0,
                  "%s published nothing, so the window was never looked at",
                  which);
    UT_ASSERT_MSG(!w.sawWindowOpen,
                  "%s: the window was open at a publish although no round-"
                  "start callback is registered", which);
    UT_ASSERT_MSG(!sim->scenarioSetupWindow,
                  "%s left the window open with no callback registered",
                  which);

    /* And the guard still refuses on the flag alone, which it would not do
     * if the start had left the window open. */
    sim->startInProgress = true;
    UT_ASSERT_MSG(swMapTile(sim) == SCN_OP_WRONG_STATE,
                  "%s: an op went through during a start, so the window is "
                  "open on a sim that has no callback", which);
    sim->startInProgress = false;

    serverSimUnregisterSubscriber(sim, h);
    return 0;
}

int run_scenario_setup_window_no_callback(void) {
    ServerSim *inPlace = makeLobbySim();
    ServerSim *fullReset;
    int rc;

    UT_ASSERT(inPlace != NULL);
    rc = swCheckNoWindow(inPlace, true, "serverSimStartGameInPlace");
    if (rc != 0) return rc;
    serverSimDestroy(inPlace);

    fullReset = makeLobbySim();
    UT_ASSERT(fullReset != NULL);
    rc = swCheckNoWindow(fullReset, false, "serverSimStartGame");
    if (rc != 0) return rc;
    serverSimDestroy(fullReset);
    return 0;
}

/* ── The publish hold ──────────────────────────────────────────── */

/* Counts CTRL_SIM_RULES arriving at a subscriber. Counted where they are
 * delivered rather than off a queue, so nothing else can empty them
 * between the start and the count. */
typedef struct {
    bool armed;
    int  rules;
} RulesCount;

static void rulesCountDeliver(void *ctx, const struct ControlEvent *evt) {
    RulesCount *c = (RulesCount *)ctx;
    if (!c->armed) return;
    if (evt->type == CTRL_SIM_RULES) {
        c->rules++;
    }
}

/* Two carried rules set from inside the window cost one CTRL_SIM_RULES
 * across the whole start — the one the start publishes itself, carrying
 * both — and a rule set outside the window publishes as it always has. */
int run_scenario_setup_window_holds_publish(void) {
    ServerSim *sim = makeLobbySim();
    SetupWatch w;
    RulesCount c;
    SubscriberHandle h;

    UT_ASSERT(sim != NULL);
    memset(&w, 0, sizeof(w));
    memset(&c, 0, sizeof(c));
    w.sim = sim;

    /* Registered before the counting starts: the registration replays the
     * sim's current state to the new subscriber, and that replay is not a
     * publish this case is counting. */
    h = serverSimRegisterSubscriber(sim, rulesCountDeliver, &c);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    c.armed = true;

    serverSimSetScenarioRoundStart(sim, swSetTwoRules, &w);
    serverSimStartGameInPlace(sim);

    UT_ASSERT_MSG(w.calls == 1, "the callback ran %d times, expected 1",
                  w.calls);
    UT_ASSERT_MSG(w.ruleReload == SCN_OP_OK,
                  "tank_reload_ticks answered %d inside the window",
                  (int)w.ruleReload);
    UT_ASSERT_MSG(w.ruleDeath == SCN_OP_OK,
                  "tank_death_ticks answered %d inside the window",
                  (int)w.ruleDeath);
    UT_ASSERT_MSG(sim->sim.rules.tank_reload_ticks == SW_RELOAD_SET,
                  "the table holds %ld for tank_reload_ticks, expected %d — "
                  "the set never happened, so the count below proves nothing",
                  (long)sim->sim.rules.tank_reload_ticks, SW_RELOAD_SET);
    UT_ASSERT_MSG(sim->sim.rules.tank_death_ticks == SW_DEATH_SET,
                  "the table holds %ld for tank_death_ticks, expected %d",
                  (long)sim->sim.rules.tank_death_ticks, SW_DEATH_SET);
    UT_ASSERT_MSG(c.rules == 1,
                  "the start delivered %d CTRL_SIM_RULES events, expected the "
                  "1 it publishes itself", c.rules);

    /* The same handler, with the window shut, still states its change. */
    UT_ASSERT_MSG(swSetRule(sim, SCN_RULE_tank_reload_ticks,
                            (double)(SW_RELOAD_SET + 1)) == SCN_OP_OK,
                  "the handler refused a value inside tank_reload_ticks' row");
    UT_ASSERT_MSG(c.rules == 2,
                  "a set-rule outside the window brought the count to %d, "
                  "expected 2 — it published nothing", c.rules);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}
