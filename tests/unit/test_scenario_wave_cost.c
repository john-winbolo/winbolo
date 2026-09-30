/*
 * What a wave costs: the runner behind a held seat, counted where it is built
 * and torn down.
 *
 * A held seat survives a wave, and so does the runner behind it.
 * serverSimUnfieldBot keeps the roster entry, the name, the team, the alliance
 * and the connection, and reaches botManagerRemoveBotKeepSeat, which parks the
 * ClientSim, the control subscription and the brain instance rather than
 * destroying them. The next spawn naming the seat goes back through
 * botManagerAddBot, which hands that runner back: a wave transition costs a
 * tank spawn and a map reload instead of a VM per seat.
 *
 * It is handed back only to a spawn naming the brain it is running and the
 * init table its VM read at its first breath; anything else takes a fresh
 * runner and says so in the log. And it never outlives the round it was built
 * in: a round start, a destroy, or the seat leaving the roster releases it.
 *
 * These cases put numbers on all of that, counted at the calls that do the
 * work rather than inferred from the roster: the fixture brain in test_stubs.c
 * records how many brains each slot has been made and how many have been
 * destroyed in all, and the ClientSim and the subscription are read off
 * sim->botMgr.bots[].
 *
 * Every case fields the way a script does — a spawn op onto the roster queue,
 * which the drain makes one entry to a tick — and unfields through the remove
 * op, which on a seat marked keepSeat reaches serverSimUnfieldBot instead of
 * emptying the seat.
 *
 * A round's held seats do not wait for a wave to get their runners: the
 * countdown builds one a frame for every seat the lobby is holding, so the
 * first wave of the round resumes like every wave after it. The countdown
 * simulates nothing, which is why the builds go there.
 *
 * run_scenario_wave_cost_refield_resumes      — one seat fielded, unfielded and
 *                                               fielded again: one brain made,
 *                                               none destroyed, and the same
 *                                               ClientSim and subscription
 *                                               throughout — carrying none of
 *                                               the previous life's prediction
 *                                               state or think telemetry
 * run_scenario_wave_cost_seats_swap_counts    — six seats swapped twice: six
 *                                               brains made, none destroyed,
 *                                               and no seat off the roster
 * run_scenario_wave_cost_other_brain_rebuilds — a refield naming another brain
 *                                               cannot have the parked runner
 * run_scenario_wave_cost_brain_case_rebuilds  — nor can one naming a file whose
 *                                               path differs from the parked
 *                                               runner's only in case
 * run_scenario_wave_cost_other_init_rebuilds  — nor can one carrying another
 *                                               init table
 * run_scenario_wave_cost_round_end_releases   — a parked runner does not cross
 *                                               a round boundary
 * run_scenario_wave_cost_all_parked_releases  — and neither does a whole wave
 *                                               of them, which is the shape no
 *                                               active bot is left to carry
 * run_scenario_wave_cost_rotation_releases    — nor does one cross the other
 *                                               round boundary, the map
 *                                               rotation a no-lobby server
 *                                               restarts on
 * run_scenario_wave_cost_seat_leaving_releases— nor does one outlive the seat
 *                                               it was parked behind
 * run_scenario_wave_cost_destroy_releases     — nor does one survive
 *                                               botManagerDestroy
 * run_scenario_wave_cost_countdown_warms_seats— the countdown leaves a parked
 *                                               runner behind every held seat
 * run_scenario_wave_cost_warm_is_one_a_frame  — one of them a frame, counted
 *                                               after each frame rather than
 *                                               at the end of the countdown
 * run_scenario_wave_cost_warmed_field_is_free — and the wave that fields one
 *                                               builds nothing
 * run_scenario_wave_cost_warmed_init_rebuilds — unless it carries an init
 *                                               table the warm had none of:
 *                                               that seat pays for a second
 *                                               brain once, and the rebuilt
 *                                               runner is the one that parks
 *                                               and matches after it
 * run_scenario_wave_cost_template_init_warms  — and a team that gives its
 *                                               seats an init table is warmed
 *                                               with it, so the first wave is
 *                                               a resume too
 * run_scenario_wave_cost_warm_skips_bad_brain — a seat whose brain will not
 *                                               load is skipped, and the rest
 *                                               are still warmed
 * run_scenario_wave_cost_failed_build_leaves_nothing
 *                                             — and a seat whose build is
 *                                               refused after it has started
 *                                               is left holding nothing: no
 *                                               ClientSim, no subscription,
 *                                               and no transport either
 * run_scenario_wave_cost_abort_countdown_releases
 *                                             — a countdown given up on takes
 *                                               the runners it had built with
 *                                               it, and the next one builds
 *                                               them again
 * run_scenario_wave_cost_all_ready_clears_skips
 *                                             — and the all-ready check that
 *                                               opens a countdown starts the
 *                                               record of refused seats empty,
 *                                               so a bit left by an earlier
 *                                               one does not cost a seat its
 *                                               warm
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, playerConnected, lobbyPlayers,
                                    * botMgr.bots[] */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath,
                                    * serverSimReturnToLobby — the round end */
#include "server_sim_scenario.h"
#include "bot_manager.h"           /* BotContext: the ClientSim and the sub,
                                    * botManagerIsBot, botManagerDestroy */
#include "client_sim_internal.h"   /* ClientSim.clientState and the err offset —
                                    * what the refield has to put back */
#include "brain_record.h"        /* the recording block a warmed seat must be told about */
#include "scenario_table.h"        /* scnTableSet — the init table a spawn carries */
#include "game_sim.h"
#include "everard_map.h"
#include "test_harness.h"

/* Slot 0 holds the human; the bots take the six seats after it, all on one
 * team, which is the Survival shape. The one-seat cases take the first of
 * them. */
#define WC_TEAM        3
#define WC_FIRST_SEAT  1
#define WC_SEATS       6

/* The whole roster once the seats are in: the human and the six held seats. */
#define WC_CONNECTED   (1 + WC_SEATS)

/* A file for the brain path to name. The fixture brain never opens it — the
 * spawn arm reads the path to refuse one that names nothing. Named per case,
 * because ctest runs the cases as concurrent processes in one directory and
 * one case's drop would take the file the others are still naming. */
static char wcBrainPath[128];
/* A second one, for the case whose refield names a brain the parked runner is
 * not running. */
static char wcOtherBrainPath[128];

/* One line of the caller's choosing in the file, so a case that writes two
 * files can tell them apart by reading one back. */
static bool wcWriteBrainFileLine(char *out, size_t outCap, const char *name,
                                 const char *line) {
    FILE *f;

    SDL_strlcpy(out, name, outCap);
    f = fopen(out, "wb");
    if (f == NULL) return false;
    fprintf(f, "%s\n", line);
    fclose(f);
    return true;
}

static bool wcWriteBrainFile(char *out, size_t outCap, const char *name) {
    return wcWriteBrainFileLine(out, outCap, name, "-- fixture");
}

/* Whether the file at `path` begins with `line`. */
static bool wcFileStartsWith(const char *path, const char *line) {
    char   buf[64];
    FILE  *f;
    size_t n;

    f = fopen(path, "rb");
    if (f == NULL) return false;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strncmp(buf, line, strlen(line)) == 0;
}

static bool wcMakeBrainFile(const char *tag) {
    char name[128];

    wcOtherBrainPath[0] = '\0';
    SDL_snprintf(name, sizeof(name),
                 "test_scenario_wave_cost_brain_%s.lua", tag);
    return wcWriteBrainFile(wcBrainPath, sizeof(wcBrainPath), name);
}

static bool wcMakeOtherBrainFile(const char *tag) {
    char name[128];

    SDL_snprintf(name, sizeof(name),
                 "test_scenario_wave_cost_other_%s.lua", tag);
    return wcWriteBrainFile(wcOtherBrainPath, sizeof(wcOtherBrainPath), name);
}

static void wcDropBrainFile(void) {
    remove(wcBrainPath);
    if (wcOtherBrainPath[0] != '\0') remove(wcOtherBrainPath);
}

/* A lobby with one ready human in slot 0 and a server configured to run bots
 * off the fixture brain file. */
static ServerSim *wcLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Human", false);
    sim->lobbyPlayers[0].ready = true;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, wcBrainPath);
    return sim;
}

/* The op naming a seat and nothing else: the name, the team and the brain all
 * come off the seat or the server, which is what a wave spawning by seat
 * number sends. */
static void wcSpawnOp(ScenarioOp *op, BYTE slot) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_ROSTER_SPAWN_BOT;
    op->u.rosterSpawnBot.slot  = slot;
    op->u.rosterSpawnBot.start = SCN_NONE;
}

static void wcRemoveOp(ScenarioOp *op, BYTE slot) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_ROSTER_REMOVE_BOT;
    op->u.rosterRemoveBot.slot = slot;
}

/* Queue one op and give the drain the tick it makes its one roster change
 * in. */
static bool wcApplyOne(ServerSim *sim, const ScenarioOp *op) {
    if (serverSimApplyScenarioOp(sim, op, NULL) != SCN_OP_QUEUED) return false;
    serverSimTick(sim);
    return true;
}

/* How many slots are in the roster, counted rather than read off a number the
 * sim keeps, so a seat that went would show here whatever that number says. */
static int wcConnectedSeats(const ServerSim *sim) {
    int n = 0;
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) n++;
    }
    return n;
}

/* The brains made for the six held seats, added up. */
static int wcSeatCreates(void) {
    int n = 0;
    int i;
    for (i = 0; i < WC_SEATS; i++) {
        n += ut_brain_stub_creates(WC_FIRST_SEAT + i);
    }
    return n;
}

/* Queue the whole wave, then give the drain one tick for each entry: it makes
 * one roster change a tick, so six spawns take six ticks. Answers false if any
 * op was refused or any seat is not on the field at the end of them. */
static bool wcFieldAll(ServerSim *sim) {
    ScenarioOp op;
    int i;

    for (i = 0; i < WC_SEATS; i++) {
        wcSpawnOp(&op, (BYTE)(WC_FIRST_SEAT + i));
        if (serverSimApplyScenarioOp(sim, &op, NULL) != SCN_OP_QUEUED) {
            return false;
        }
    }
    for (i = 0; i < WC_SEATS; i++) {
        serverSimTick(sim);
    }
    for (i = 0; i < WC_SEATS; i++) {
        if (!sim->lobbyPlayers[WC_FIRST_SEAT + i].fielded) return false;
    }
    return true;
}

/* The same for the other half of the transition. */
static bool wcUnfieldAll(ServerSim *sim) {
    ScenarioOp op;
    int i;

    for (i = 0; i < WC_SEATS; i++) {
        wcRemoveOp(&op, (BYTE)(WC_FIRST_SEAT + i));
        if (serverSimApplyScenarioOp(sim, &op, NULL) != SCN_OP_QUEUED) {
            return false;
        }
    }
    for (i = 0; i < WC_SEATS; i++) {
        serverSimTick(sim);
    }
    for (i = 0; i < WC_SEATS; i++) {
        if (sim->lobbyPlayers[WC_FIRST_SEAT + i].fielded) return false;
    }
    return true;
}

/* The countdown a lobby runs before a round, driven a frame at a time: the
 * state and the tick count the all-ready check sets, and then the frames
 * themselves. Each one builds one held seat's runner; the last starts the
 * round.
 *
 * The record of seats the warm refused is not touched here. A fresh sim is
 * zeroed at create, and clearing it at every countdown is the production
 * all-ready check's job — run_scenario_wave_cost_all_ready_clears_skips is
 * what holds it to that, and a helper doing it as well would hide it. */
static void wcRunCountdown(ServerSim *sim, int ticks) {
    int i;

    sim->state = serverStateCountdown;
    sim->countdownTicks = ticks;
    for (i = 0; i < ticks; i++) {
        serverSimTick(sim);
    }
}

/* The same countdown stopped part way through, for the cases about a round
 * that never starts: ticks is the count the all-ready check set and frames is
 * how many of them are run, so with frames below ticks the last one warms a
 * seat like the rest and the server is still counting down at the end. Leaves
 * the refused-seat record alone, as wcRunCountdown does and for the same
 * reason. */
static void wcRunCountdownFrames(ServerSim *sim, int ticks, int frames) {
    int i;

    sim->state = serverStateCountdown;
    sim->countdownTicks = ticks;
    for (i = 0; i < frames; i++) {
        serverSimTick(sim);
    }
}

/* Two tables the same, pair for pair and in the order they were stored —
 * which is the test bot_manager makes when it decides whether a parked runner
 * can serve a spawn. */
static bool wcSameTable(const ScnTable *a, const ScnTable *b) {
    int i;

    if (a->count != b->count) return false;
    for (i = 0; i < (int)a->count && i < SCN_TABLE_MAX; i++) {
        if (strcmp(a->kv[i].key, b->kv[i].key) != 0) return false;
        if (strcmp(a->kv[i].value, b->kv[i].value) != 0) return false;
    }
    return true;
}

/* Seat the wave's seats from a lobby template, which is the only thing that
 * carries a team's init table: wcSeatTeam below goes straight to
 * serverSimAddUnfieldedSeat, and a seat made that way has no team behind it
 * to take a brain or a table from. One unfielded team of WC_SEATS, so the
 * seats land in the slots from WC_FIRST_SEAT up, above the human in slot 0. */
static bool wcSeatFromTemplate(ServerSim *sim, const ScnTable *init) {
    ScnLobbyTemplate t;
    int              i;

    memset(&t, 0, sizeof(t));
    t.numTeams         = 1;
    t.teams[0].id      = WC_TEAM;
    t.teams[0].bots    = WC_SEATS;
    t.teams[0].maxBots = WC_SEATS;
    t.teams[0].fielded = false;
    SDL_strlcpy(t.teams[0].brain, wcBrainPath, sizeof(t.teams[0].brain));
    if (init != NULL) t.teams[0].init = *init;

    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);

    /* i is 0-based over the seats; the slot is WC_FIRST_SEAT + i. */
    for (i = 0; i < WC_SEATS; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        if (!sim->playerConnected[seat]) return false;
        if (!sim->lobbyPlayers[seat].keepSeat) return false;
        if (sim->lobbyPlayers[seat].fielded) return false;
    }
    return true;
}

/* Seat the team's seats in a fresh lobby, named as the template names them. */
static bool wcSeatTeam(ServerSim *sim) {
    int i;

    for (i = 0; i < WC_SEATS; i++) {
        char name[PLAYER_NAME_LEN];
        SDL_snprintf(name, sizeof(name), "Raider%d", i + 1);
        if (!serverSimAddUnfieldedSeat(sim, (BYTE)(WC_FIRST_SEAT + i),
                                       name, WC_TEAM)) {
            return false;
        }
    }
    return true;
}

/* A running round holding one seat, with that seat fielded and then taken off
 * the field again — so it comes back with its runner parked. */
static ServerSim *wcParkedSeatSim(BYTE seat) {
    ServerSim *sim = wcLobbySim();
    ScenarioOp op;

    if (sim == NULL) return NULL;
    if (!serverSimAddUnfieldedSeat(sim, seat, "Raider1", WC_TEAM)) {
        serverSimDestroy(sim);
        return NULL;
    }
    serverSimStartGame(sim);

    wcSpawnOp(&op, seat);
    if (!wcApplyOne(sim, &op) || !sim->lobbyPlayers[seat].fielded) {
        serverSimDestroy(sim);
        return NULL;
    }
    wcRemoveOp(&op, seat);
    if (!wcApplyOne(sim, &op) || sim->lobbyPlayers[seat].fielded) {
        serverSimDestroy(sim);
        return NULL;
    }
    return sim;
}

/* ── One seat, fielded twice ──────────────────────────────────────── */

int run_scenario_wave_cost_refield_resumes(void) {
    ServerSim *sim;
    ScenarioOp op;
    const BYTE seat = WC_FIRST_SEAT;
    ClientSim *parkedCs;
    SubscriberHandle parkedSub;

    UT_ASSERT(wcMakeBrainFile("refield_resumes"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, seat, "Raider1", WC_TEAM));
    serverSimStartGame(sim);
    UT_ASSERT(sim->state == serverStateRunning);

    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 0,
                  "the held seat has had %d brains made for it before anything "
                  "fielded it, expected 0", ut_brain_stub_creates(seat));

    /* Field it. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the spawn did not field the seat");

    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "fielding the seat made %d brains for it, expected 1",
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "fielding the seat destroyed %d brains, expected 0",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs != NULL,
                  "the fielded seat has no ClientSim behind it");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub !=
                      SUBSCRIBER_HANDLE_INVALID,
                  "the fielded seat holds no control subscription");

    /* The two the refield has to get back, by value. */
    parkedCs  = sim->botMgr.bots[seat].cs;
    parkedSub = sim->botMgr.bots[seat].controlSub;

    /* What a life of playing leaves on the ClientSim, planted by hand: this
       binary delivers no snapshot to a bot, so nothing here would set the
       predicted-tank latch, fill the input history or move the render-only
       error offset on its own. The tick is a long way past anything the
       refielded life will produce, which is the point — the replay loop
       walks from the server's lastProcessedInput + 1 up to newestInput, and
       the unfield zeroed the server's end of that. The think telemetry is
       planted the same way, for the same reason. */
    sim->botMgr.bots[seat].cs->clientState.hasPredictedTank = TRUE;
    sim->botMgr.bots[seat].cs->clientState.newestInput      = 3001;
    sim->botMgr.bots[seat].cs->errX     = 12.5f;
    sim->botMgr.bots[seat].cs->errY     = -7.25f;
    sim->botMgr.bots[seat].cs->errAngle = 3.5f;
    sim->botMgr.bots[seat].overrunCount = 9;
    sim->botMgr.bots[seat].thinkCount   = 40;
    sim->botMgr.bots[seat].maxThinkMs   = 22.0;

    /* Off the field again: the seat stays, and so does everything behind it. */
    wcRemoveOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(!sim->lobbyPlayers[seat].fielded,
                  "the removal left the seat on the field");

    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "taking the seat off the field destroyed %d brains, "
                  "expected 0 — the runner is parked, not torn down",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the unfielded seat does not read as parked");
    UT_ASSERT_MSG(!botManagerIsBot(sim, seat),
                  "a parked seat must not read as an active bot");
    UT_ASSERT_MSG(serverSimGetNumBots(sim) == 0,
                  "the bot pool counts %u bots with the seat off the field, "
                  "expected 0", (unsigned)serverSimGetNumBots(sim));
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == parkedCs,
                  "the parked seat is holding a different ClientSim");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub == parkedSub,
                  "the parked seat is holding a different control subscription");

    /* And the seat itself is exactly where it was. */
    UT_ASSERT_MSG(sim->playerConnected[seat],
                  "the unfield emptied the seat");
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].keepSeat,
                  "the unfield stopped the seat being the scenario's");

    /* Field it a second time: nothing made and nothing destroyed, and the
       same three things behind the seat as the first wave had. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the second spawn did not field the seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the seat has had %d brains made for it over a field, an "
                  "unfield and a refield, expected 1 — the refield resumes the "
                  "parked runner", ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "%d brains were destroyed over the three, expected 0",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == parkedCs,
                  "the refielded seat is running on a different ClientSim");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub == parkedSub,
                  "the refielded seat registered another control subscription");
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "the refielded seat still reads as parked");
    UT_ASSERT_MSG(botManagerIsBot(sim, seat),
                  "the refielded seat is not an active bot");
    UT_ASSERT_MSG(sim->sim.tanks[seat] != NULL,
                  "the refield built no tank for the seat");

    /* And that tank is being predicted from nothing, on the same ClientSim
       the asserts above just established this is. A latch still set here
       sends the first snapshot into the reconcile branch, which would replay
       the previous life's inputs — every tick from the server's zeroed
       lastProcessedInput + 1 up to newestInput — onto a tank that never
       drove them. */
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].cs->clientState.hasPredictedTank,
                  "the refielded seat still has the predicted-tank latch set "
                  "from the life that ended at the park");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs->clientState.initialized,
                  "the refielded seat's client state does not read as "
                  "initialized");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs->clientState.newestInput == 0,
                  "the refielded seat's input history newest tick is %u, "
                  "expected 0 — the previous life's inputs are still there to "
                  "replay",
                  (unsigned)sim->botMgr.bots[seat].cs->clientState.newestInput);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs->errX == 0.0f &&
                      sim->botMgr.bots[seat].cs->errY == 0.0f &&
                      sim->botMgr.bots[seat].cs->errAngle == 0.0f,
                  "the refielded seat carries the previous life's render error "
                  "offset (%f, %f, angle %f), expected all zero",
                  (double)sim->botMgr.bots[seat].cs->errX,
                  (double)sim->botMgr.bots[seat].cs->errY,
                  (double)sim->botMgr.bots[seat].cs->errAngle);

    /* The think telemetry is one window, so the resume starts the next one
       rather than adding this life's thinking to the last one's. */
    UT_ASSERT_MSG(sim->botMgr.bots[seat].overrunCount == 0,
                  "the refielded seat counts %u overruns, expected 0",
                  (unsigned)sim->botMgr.bots[seat].overrunCount);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].thinkCount == 0,
                  "the refielded seat counts %u thinks, expected 0",
                  (unsigned)sim->botMgr.bots[seat].thinkCount);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].maxThinkMs == 0.0,
                  "the refielded seat holds a %f ms think peak, expected 0",
                  sim->botMgr.bots[seat].maxThinkMs);

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* ── Six seats, swapped twice ─────────────────────────────────────── */

int run_scenario_wave_cost_seats_swap_counts(void) {
    ServerSim *sim;
    int i;

    UT_ASSERT(wcMakeBrainFile("seats_swap_counts"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);

    for (i = 0; i < WC_SEATS; i++) {
        char name[PLAYER_NAME_LEN];
        SDL_snprintf(name, sizeof(name), "Raider%d", i + 1);
        UT_ASSERT_MSG(serverSimAddUnfieldedSeat(sim, (BYTE)(WC_FIRST_SEAT + i),
                                                name, WC_TEAM),
                      "seat %d of the %d would not be seated",
                      i + 1, WC_SEATS);
    }
    serverSimStartGame(sim);
    UT_ASSERT(sim->state == serverStateRunning);

    UT_ASSERT_MSG(wcConnectedSeats(sim) == WC_CONNECTED,
                  "the roster holds %d seats before the first wave, expected "
                  "%d — one human and %d held seats",
                  wcConnectedSeats(sim), WC_CONNECTED, WC_SEATS);
    UT_ASSERT_MSG(wcSeatCreates() == 0,
                  "%d brains were made before the first wave, expected 0",
                  wcSeatCreates());

    /* The first wave. */
    UT_ASSERT_MSG(wcFieldAll(sim),
                  "the first wave did not field all %d seats", WC_SEATS);
    UT_ASSERT_MSG(wcSeatCreates() == WC_SEATS,
                  "the first wave made %d brains, expected %d",
                  wcSeatCreates(), WC_SEATS);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "the first wave destroyed %d brains, expected 0",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(wcConnectedSeats(sim) == WC_CONNECTED,
                  "the roster holds %d seats with the first wave fielded, "
                  "expected %d", wcConnectedSeats(sim), WC_CONNECTED);

    /* The transition between the two waves. */
    UT_ASSERT_MSG(wcUnfieldAll(sim),
                  "the wave transition did not take all %d seats off the "
                  "field", WC_SEATS);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "taking the wave off the field destroyed %d brains, "
                  "expected 0 — every runner is parked for the next wave",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(wcSeatCreates() == WC_SEATS,
                  "%d brains had been made by the end of the transition, "
                  "expected the %d the first wave made",
                  wcSeatCreates(), WC_SEATS);
    UT_ASSERT_MSG(wcConnectedSeats(sim) == WC_CONNECTED,
                  "the roster holds %d seats with the wave off the field, "
                  "expected %d", wcConnectedSeats(sim), WC_CONNECTED);

    /* The second wave, into the same six seats. */
    UT_ASSERT_MSG(wcFieldAll(sim),
                  "the second wave did not field all %d seats", WC_SEATS);
    UT_ASSERT_MSG(wcSeatCreates() == WC_SEATS,
                  "two waves over %d held seats made %d brains, expected %d — "
                  "the runner behind each seat is handed back to the second "
                  "wave", WC_SEATS, wcSeatCreates(), WC_SEATS);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "%d brains were destroyed over the two waves, expected 0",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(wcConnectedSeats(sim) == WC_CONNECTED,
                  "the roster holds %d seats with the second wave fielded, "
                  "expected %d", wcConnectedSeats(sim), WC_CONNECTED);

    /* The count above would hide one seat lost and another gained, so each of
       the six is asked for by name as well. */
    for (i = 0; i < WC_SEATS; i++) {
        UT_ASSERT_MSG(sim->playerConnected[WC_FIRST_SEAT + i],
                      "seat %d left the roster over the two waves; all %d "
                      "should still be in it", WC_FIRST_SEAT + i, WC_SEATS);
    }

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* ── A refield the parked runner cannot serve ─────────────────────── */

int run_scenario_wave_cost_other_brain_rebuilds(void) {
    ServerSim *sim;
    ScenarioOp op;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("other_brain_rebuilds"));
    UT_ASSERT(wcMakeOtherBrainFile("other_brain_rebuilds"));
    ut_brain_stub_arm(true);
    sim = wcParkedSeatSim(seat);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the seat should have come back with its runner parked");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "%d brains were made fielding the seat once, expected 1",
                  ut_brain_stub_creates(seat));

    /* The refield names the other brain. The parked runner is running the
       first one, so it goes and a new one is built. */
    wcSpawnOp(&op, seat);
    SDL_strlcpy(op.u.rosterSpawnBot.brain, wcOtherBrainPath,
                sizeof(op.u.rosterSpawnBot.brain));
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the refield did not field the seat");

    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 2,
                  "the seat has had %d brains made for it, expected 2 — a "
                  "refield on another brain cannot take the parked runner",
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "%d brains were destroyed, expected 1 — the parked runner "
                  "is released to make room for the new one",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "the refielded seat still reads as parked");
    UT_ASSERT_MSG(SDL_strcasecmp(sim->botMgr.bots[seat].brainPath,
                                 wcOtherBrainPath) == 0,
                  "the refielded seat is running '%s', expected '%s'",
                  sim->botMgr.bots[seat].brainPath, wcOtherBrainPath);
    UT_ASSERT_MSG(sim->playerConnected[seat],
                  "the rebuild emptied the seat");

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* The same refield onto a second brain file, except that the second file's
 * name differs from the first's only in case. The path names a file, and on
 * the server's platforms those are two files, so the parked runner is running
 * a script the refield did not ask for and cannot serve it.
 *
 * Both files are written for real, because the spawn arm refuses a path that
 * names no file. That assumes a case-sensitive filesystem: on one that folds
 * case the second write would land in the first file and there would be one
 * script here rather than two, so the case reads the first file back and
 * returns without asserting anything when it finds the second write's line in
 * it. The build directory this runs in is on Linux ext4, where the two names
 * stay apart. */
int run_scenario_wave_cost_brain_case_rebuilds(void) {
    ServerSim *sim;
    ScenarioOp op;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("case_rebuilds"));
    /* Written straight into wcOtherBrainPath, which wcMakeBrainFile has just
       cleared, so wcDropBrainFile takes this one too. */
    UT_ASSERT(wcWriteBrainFileLine(wcOtherBrainPath, sizeof(wcOtherBrainPath),
                                   "test_scenario_wave_cost_BRAIN_"
                                   "case_rebuilds.lua",
                                   "-- second fixture"));
    if (!wcFileStartsWith(wcBrainPath, "-- fixture")) {
        /* One file under both names: nothing here to tell apart. */
        wcDropBrainFile();
        return 0;
    }

    UT_ASSERT_MSG(SDL_strcasecmp(wcBrainPath, wcOtherBrainPath) == 0,
                  "'%s' and '%s' must differ only in case, or this case is "
                  "not exercising the exact compare",
                  wcBrainPath, wcOtherBrainPath);
    UT_ASSERT_MSG(strcmp(wcBrainPath, wcOtherBrainPath) != 0,
                  "the two paths are the same string, so there is no case "
                  "difference to compare");

    ut_brain_stub_arm(true);
    sim = wcParkedSeatSim(seat);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the seat should have come back with its runner parked");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "%d brains were made fielding the seat once, expected 1",
                  ut_brain_stub_creates(seat));

    /* The refield names the second file. */
    wcSpawnOp(&op, seat);
    SDL_strlcpy(op.u.rosterSpawnBot.brain, wcOtherBrainPath,
                sizeof(op.u.rosterSpawnBot.brain));
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the refield did not field the seat");

    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 2,
                  "the seat has had %d brains made for it, expected 2 — a "
                  "refield naming a path that differs only in case names "
                  "another file, which the parked runner is not running",
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "%d brains were destroyed, expected 1 — the parked runner "
                  "is released to make room for the new one",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "the refielded seat still reads as parked");
    UT_ASSERT_MSG(strcmp(sim->botMgr.bots[seat].brainPath,
                         wcOtherBrainPath) == 0,
                  "the refielded seat is running '%s', expected '%s'",
                  sim->botMgr.bots[seat].brainPath, wcOtherBrainPath);
    UT_ASSERT_MSG(sim->playerConnected[seat],
                  "the rebuild emptied the seat");

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

int run_scenario_wave_cost_other_init_rebuilds(void) {
    ServerSim *sim;
    ScenarioOp op;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("other_init_rebuilds"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, seat, "Raider1", WC_TEAM));
    serverSimStartGame(sim);
    UT_ASSERT(sim->state == serverStateRunning);

    /* Fielded with one configuration. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(scnTableSet(&op.u.rosterSpawnBot.init, "role", "guard"));
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the spawn did not field the seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "%d brains were made fielding the seat once, expected 1",
                  ut_brain_stub_creates(seat));

    /* Off the field, and back on carrying another one. The init table is read
       at the VM's first breath, so the parked runner cannot serve it. */
    wcRemoveOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the seat should have come back with its runner parked");

    wcSpawnOp(&op, seat);
    UT_ASSERT(scnTableSet(&op.u.rosterSpawnBot.init, "role", "raider"));
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the refield did not field the seat");

    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 2,
                  "the seat has had %d brains made for it, expected 2 — a "
                  "refield carrying another init table cannot take the parked "
                  "runner", ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "%d brains were destroyed, expected 1 — the parked runner "
                  "is released to make room for the new one",
                  ut_brain_stub_destroys());
    {
        const ScnTable *made = ut_brain_stub_init(seat);
        UT_ASSERT_MSG(made != NULL && made->count == 1 &&
                          strcmp(made->kv[0].value, "raider") == 0,
                      "the rebuilt brain was not handed the refield's own "
                      "init table");
    }

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* ── A parked runner outlives neither the round nor the pool ──────── */

int run_scenario_wave_cost_round_end_releases(void) {
    ServerSim *sim;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("round_end_releases"));
    ut_brain_stub_arm(true);
    sim = wcParkedSeatSim(seat);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the seat should have come back with its runner parked");

    /* The round ending. A parked brain keeps its state table, so the round it
       remembers is this one — it must not open the next. */
    serverSimReturnToLobby(sim);

    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "the round end left the runner parked");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == NULL,
                  "the round end left the parked ClientSim behind");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub ==
                      SUBSCRIBER_HANDLE_INVALID,
                  "the round end left the parked control subscription "
                  "registered");
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "%d brains were destroyed at the round end, expected the "
                  "1 that was parked", ut_brain_stub_destroys());
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the round end made a brain for a seat that is not on the "
                  "field: %d made in all, expected 1",
                  ut_brain_stub_creates(seat));

    /* The seat itself is untouched: it goes back to the lobby held, the way
       it was seated. */
    UT_ASSERT_MSG(sim->playerConnected[seat],
                  "the round end emptied the seat");
    UT_ASSERT_MSG(!sim->lobbyPlayers[seat].fielded,
                  "the round end put the held seat on the field");
    UT_ASSERT_MSG(serverSimGetNumBots(sim) == 0,
                  "the bot pool counts %u bots, expected 0",
                  (unsigned)serverSimGetNumBots(sim));

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

int run_scenario_wave_cost_destroy_releases(void) {
    ServerSim *sim;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("destroy_releases"));
    ut_brain_stub_arm(true);
    sim = wcParkedSeatSim(seat);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the seat should have come back with its runner parked");

    /* The pool going away takes the parked runner with it — the loop over
       active bots would walk straight past it. */
    botManagerDestroy(sim);

    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "the destroy left the runner parked");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == NULL,
                  "the destroy left the parked ClientSim behind");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub ==
                      SUBSCRIBER_HANDLE_INVALID,
                  "the destroy left the parked control subscription "
                  "registered");
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "%d brains were destroyed, expected the 1 that was parked",
                  ut_brain_stub_destroys());

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* A whole wave parked and nothing active — the shape a Wave Defense round
 * ends in, its last wave unfielded before the round is won. Nothing is left
 * on the field to be walked over, so a release that only ever ran for an
 * active bot never ran at all, and every one of those brains opened the next
 * round still remembering this one. */
int run_scenario_wave_cost_all_parked_releases(void) {
    ServerSim *sim;
    int i;

    UT_ASSERT(wcMakeBrainFile("all_parked_releases"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));
    serverSimStartGame(sim);
    UT_ASSERT(sim->state == serverStateRunning);

    UT_ASSERT_MSG(wcFieldAll(sim), "the wave did not field all %d seats",
                  WC_SEATS);
    UT_ASSERT_MSG(wcUnfieldAll(sim),
                  "the wave did not come off the field");
    UT_ASSERT_MSG(wcSeatCreates() == WC_SEATS,
                  "the wave made %d brains, expected %d",
                  wcSeatCreates(), WC_SEATS);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "%d brains were destroyed taking the wave off the field, "
                  "expected 0", ut_brain_stub_destroys());
    for (i = 0; i < WC_SEATS; i++) {
        UT_ASSERT_MSG(sim->botMgr.bots[WC_FIRST_SEAT + i].parked,
                      "seat %d came off the field without parking its runner",
                      WC_FIRST_SEAT + i);
    }
    /* The condition the round-start release could not see. */
    UT_ASSERT_MSG(serverSimGetNumBots(sim) == 0,
                  "the pool counts %u active bots with every seat parked, "
                  "expected 0", (unsigned)serverSimGetNumBots(sim));

    serverSimReturnToLobby(sim);

    UT_ASSERT_MSG(ut_brain_stub_destroys() == WC_SEATS,
                  "the round end destroyed %d brains, expected the %d that "
                  "were parked", ut_brain_stub_destroys(), WC_SEATS);
    for (i = 0; i < WC_SEATS; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                      "seat %d carried its parked runner across the boundary",
                      (int)seat);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == NULL,
                      "seat %d kept its ClientSim across the boundary",
                      (int)seat);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub ==
                          SUBSCRIBER_HANDLE_INVALID,
                      "seat %d kept its control subscription across the "
                      "boundary", (int)seat);
        UT_ASSERT_MSG(sim->playerConnected[seat],
                      "the round end emptied seat %d", (int)seat);
    }

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* The other round boundary. A no-lobby server run with map rotation does not
 * go back to a lobby between rounds: serverSimMapRotateRound picks the next
 * map and starts the next round where the gameOver->lobby path would have
 * waited, so it is the end of one round as much as serverSimReturnToLobby is,
 * and the parked runners built in the round being left have to go the same
 * way. Left behind, each brain would open the next round still holding the
 * state table it filled in this one.
 *
 * The rotation picks its map out of a map directory, and this sim has none —
 * mapDirFiles is NULL, so the pick is skipped and the round restarts on the
 * map already loaded. That is the whole of what the pick contributes here; the
 * release runs before it either way. */
int run_scenario_wave_cost_rotation_releases(void) {
    ServerSim *sim;
    int i;

    UT_ASSERT(wcMakeBrainFile("rotation_releases"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    /* What makes this the kind of server that reaches the rotation at all. */
    serverSimSetMapRotate(sim, true);
    UT_ASSERT(wcSeatTeam(sim));
    serverSimStartGame(sim);
    UT_ASSERT(sim->state == serverStateRunning);

    /* A wave fielded and taken off again, so every seat comes into the
       rotation holding a parked runner. */
    UT_ASSERT_MSG(wcFieldAll(sim), "the wave did not field all %d seats",
                  WC_SEATS);
    UT_ASSERT_MSG(wcUnfieldAll(sim), "the wave did not come off the field");
    UT_ASSERT_MSG(wcSeatCreates() == WC_SEATS,
                  "the wave made %d brains, expected %d",
                  wcSeatCreates(), WC_SEATS);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "%d brains were destroyed taking the wave off the field, "
                  "expected 0", ut_brain_stub_destroys());
    /* i is 0-based over the seats; the slot is WC_FIRST_SEAT + i. */
    for (i = 0; i < WC_SEATS; i++) {
        UT_ASSERT_MSG(sim->botMgr.bots[WC_FIRST_SEAT + i].parked,
                      "seat %d came off the field without parking its runner",
                      WC_FIRST_SEAT + i);
    }

    serverSimMapRotateRound(sim);

    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the rotation left the server in state %d, expected the "
                  "next round running", (int)sim->state);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == WC_SEATS,
                  "the rotation destroyed %d brains, expected the %d that were "
                  "parked", ut_brain_stub_destroys(), WC_SEATS);
    UT_ASSERT_MSG(wcSeatCreates() == WC_SEATS,
                  "%d brains had been made by the end of the rotation, "
                  "expected the %d the wave made", wcSeatCreates(), WC_SEATS);
    for (i = 0; i < WC_SEATS; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(!botManagerHasRunner(sim, seat),
                      "seat %d carried its runner across the rotation",
                      (int)seat);
        UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                      "seat %d still reads as parked after the rotation",
                      (int)seat);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == NULL,
                      "seat %d kept its ClientSim through the rotation",
                      (int)seat);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub ==
                          SUBSCRIBER_HANDLE_INVALID,
                      "seat %d kept its control subscription through the "
                      "rotation", (int)seat);
    }

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* The seat itself leaving, which is not a round boundary at all: a host taking
 * a held seat out of the lobby, or a script removing the seat rather than
 * taking its bot off the field. The pool holds no active bot for such a seat —
 * it is off the field — so the loop over active bots walks straight past it,
 * and the runner it is still holding has to be released on the way out. */
int run_scenario_wave_cost_seat_leaving_releases(void) {
    ServerSim *sim;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("seat_leaving_releases"));
    ut_brain_stub_arm(true);
    sim = wcParkedSeatSim(seat);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the seat should have come back with its runner parked");
    UT_ASSERT_MSG(!botManagerIsBot(sim, seat),
                  "a parked seat must not read as an active bot, or the "
                  "removal would take the active arm instead");
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "%d brains were destroyed before the removal, expected 0",
                  ut_brain_stub_destroys());

    serverSimRemoveBot(sim, seat);

    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "the seat leaving destroyed %d brains, expected the 1 that "
                  "was parked behind it", ut_brain_stub_destroys());
    UT_ASSERT_MSG(!sim->playerConnected[seat],
                  "the removal left seat %d in the roster", (int)seat);
    UT_ASSERT_MSG(!botManagerHasRunner(sim, seat),
                  "seat %d still holds a runner after leaving", (int)seat);
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "seat %d still reads as parked after leaving", (int)seat);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == NULL,
                  "seat %d kept its ClientSim on the way out", (int)seat);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub ==
                      SUBSCRIBER_HANDLE_INVALID,
                  "seat %d kept its control subscription on the way out",
                  (int)seat);

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* ── The countdown builds what the round will need ────────────────── */

int run_scenario_wave_cost_countdown_warms_seats(void) {
    ServerSim *sim;
    int i;

    UT_ASSERT(wcMakeBrainFile("countdown_warms_seats"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));
    UT_ASSERT_MSG(wcSeatCreates() == 0,
                  "%d brains were made seating the held seats, expected 0",
                  wcSeatCreates());

    /* One frame per seat, and one more for the frame that starts the round. */
    wcRunCountdown(sim, WC_SEATS + 1);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the countdown did not start the round, state %d",
                  (int)sim->state);

    UT_ASSERT_MSG(wcSeatCreates() == WC_SEATS,
                  "the countdown made %d brains, expected one for each of the "
                  "%d held seats", wcSeatCreates(), WC_SEATS);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "the countdown destroyed %d brains, expected 0",
                  ut_brain_stub_destroys());

    for (i = 0; i < WC_SEATS; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                      "seat %d had %d brains made for it, expected 1",
                      (int)seat, ut_brain_stub_creates(seat));
        UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                      "seat %d has no parked runner behind it", (int)seat);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].cs != NULL,
                      "seat %d was warmed without a ClientSim", (int)seat);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub !=
                          SUBSCRIBER_HANDLE_INVALID,
                      "seat %d was warmed without a control subscription",
                      (int)seat);
        /* The roster is untouched: a warm fields nobody. */
        UT_ASSERT_MSG(!sim->lobbyPlayers[seat].fielded,
                      "the warm put seat %d on the field", (int)seat);
        UT_ASSERT_MSG(sim->sim.tanks[seat] == NULL,
                      "the warm built a tank for seat %d", (int)seat);
    }
    UT_ASSERT_MSG(serverSimGetNumBots(sim) == 0,
                  "the pool counts %u active bots after the warm, expected 0",
                  (unsigned)serverSimGetNumBots(sim));

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* The same countdown, counted after every frame instead of once at the end.
 * run_scenario_wave_cost_countdown_warms_seats reads the counts only when the
 * round has started, so a countdown that built all six seats in its first
 * frame would satisfy it, and one build a frame is the point of the pass: the
 * server timer reads the clock once a callback and then runs every tick the
 * clock says is owed back to back, so a frame that builds two runners stalls
 * for twice as long as one that builds one. */
int run_scenario_wave_cost_warm_is_one_a_frame(void) {
    ServerSim *sim;
    int frame;

    UT_ASSERT(wcMakeBrainFile("warm_is_one_a_frame"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));

    /* A countdown one frame longer than there are seats, with none of its
       frames run yet, so every frame below is a warming frame and none of them
       is the one that starts the round. */
    wcRunCountdownFrames(sim, WC_SEATS + 1, 0);
    UT_ASSERT_MSG(sim->state == serverStateCountdown,
                  "the server is in state %d before the first countdown "
                  "frame, expected it to be counting down", (int)sim->state);
    UT_ASSERT_MSG(wcSeatCreates() == 0,
                  "%d brains were made before the first countdown frame, "
                  "expected 0", wcSeatCreates());

    for (frame = 1; frame <= WC_SEATS; frame++) {
        int i;

        serverSimTick(sim);

        UT_ASSERT_MSG(sim->state == serverStateCountdown,
                      "the server is in state %d after %d countdown frames, "
                      "expected it to still be counting down",
                      (int)sim->state, frame);
        UT_ASSERT_MSG(wcSeatCreates() == frame,
                      "%d countdown frames have made %d brains, expected %d — "
                      "one a frame", frame, wcSeatCreates(), frame);
        UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                      "%d brains had been destroyed by frame %d, expected 0",
                      ut_brain_stub_destroys(), frame);

        /* Which seats, and not only how many. The pass walks the roster in
           slot order, so after frame k the first k seats from WC_FIRST_SEAT
           hold a runner and the rest hold none. i is 0-based over the seats;
           the slot is WC_FIRST_SEAT + i. */
        for (i = 0; i < WC_SEATS; i++) {
            const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
            if (i < frame) {
                UT_ASSERT_MSG(botManagerHasRunner(sim, seat),
                              "seat %d holds no runner after %d frames",
                              (int)seat, frame);
                UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                              "seat %d holds a runner that does not read as "
                              "parked after %d frames", (int)seat, frame);
                UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                              "seat %d has had %d brains made for it by frame "
                              "%d, expected 1", (int)seat,
                              ut_brain_stub_creates(seat), frame);
            } else {
                UT_ASSERT_MSG(!botManagerHasRunner(sim, seat),
                              "seat %d holds a runner after %d frames, which "
                              "reach only as far as seat %d", (int)seat, frame,
                              WC_FIRST_SEAT + frame - 1);
                UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 0,
                              "seat %d has had %d brains made for it by frame "
                              "%d, expected 0", (int)seat,
                              ut_brain_stub_creates(seat), frame);
            }
        }
    }

    /* And the frame after the last seat is the one that starts the round,
       which builds nothing of its own. */
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the countdown did not start the round, state %d",
                  (int)sim->state);
    UT_ASSERT_MSG(wcSeatCreates() == WC_SEATS,
                  "the whole countdown made %d brains, expected one for each "
                  "of the %d held seats", wcSeatCreates(), WC_SEATS);

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

int run_scenario_wave_cost_warmed_field_is_free(void) {
    ServerSim *sim;
    ScenarioOp op;
    const BYTE seat = WC_FIRST_SEAT;
    ClientSim *warmCs;
    SubscriberHandle warmSub;

    UT_ASSERT(wcMakeBrainFile("warmed_field_is_free"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));
    wcRunCountdown(sim, WC_SEATS + 1);
    UT_ASSERT(sim->state == serverStateRunning);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the countdown left seat %d without a runner", (int)seat);

    warmCs  = sim->botMgr.bots[seat].cs;
    warmSub = sim->botMgr.bots[seat].controlSub;

    /* The first wave of the round. Nothing to build: the runner is here. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the wave did not field the warmed seat");

    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "fielding a warmed seat made %d brains for it in all, "
                  "expected the 1 the warm made",
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "fielding a warmed seat destroyed %d brains, expected 0",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == warmCs,
                  "the fielded seat is running on a different ClientSim than "
                  "the warm built");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub == warmSub,
                  "the fielded seat registered another control subscription");
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "the fielded seat still reads as parked");
    UT_ASSERT_MSG(botManagerIsBot(sim, seat),
                  "the fielded seat is not an active bot");
    UT_ASSERT_MSG(sim->sim.tanks[seat] != NULL,
                  "the fielding built no tank for the seat");

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* And the warmed seat is told where the round's debug files go.
 *
 * A -brain-debug run opens a recording block on the round's first running
 * tick and publishes DEBUG_SESSION_DIR into every bot as it opens. Both
 * halves of that missed a seat the COUNTDOWN had warmed:
 *
 *   * the publish goes through botManagerSetLuaGlobalString, which used to
 *     refuse any seat that was not `active`. A warmed runner is parked, so
 *     every held seat was silently skipped.
 *   * and the mid-round hand-off that catches a bot born after the block
 *     opened sat past the early return a resume takes, so the fielding did
 *     not make up for it either.
 *
 * A lobby-hosted round therefore recorded a block with a .btr and a perf log
 * in it and not one print2_bot*.log, because every bot that took the field
 * in it came back on a warmed runner. Both halves are held here: the write
 * lands in the parked VM, and the resume publishes the dir on its own.
 *
 * The fixture brain is given a real lua_State for this case — the cheap stub
 * has none, and a global published into nothing cannot be read back. */
int run_scenario_wave_cost_warmed_seat_takes_session_dir(void) {
    ServerSim  *sim;
    ScenarioOp  op;
    const BYTE  seat = WC_FIRST_SEAT;
    const char *dir  = "debug_sessions/ut_warmed_session_dir";
    char       *got;
    bool        wrote;

    UT_ASSERT(wcMakeBrainFile("warmed_seat_takes_session_dir"));
    ut_brain_stub_arm(true);
    ut_brain_stub_lua(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));
    wcRunCountdown(sim, WC_SEATS + 1);
    UT_ASSERT(sim->state == serverStateRunning);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the countdown left seat %d without a runner", (int)seat);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].brain.L != NULL,
                  "the fixture brain for seat %d has no Lua state, so this "
                  "case cannot see what is written into it", (int)seat);

    /* A recording block, as the lifecycle opens one. */
    brainRecordSetEnabled(true);
    brainRecordSetSessionDir(dir);

    /* (a) THE PUBLISH REACHES A PARKED RUNNER. This is the call
           serverLifecycleOpenBraindbgBlock makes for every bot seat as the
           block opens, and a held seat is a bot seat. Written under a name
           of this case's own so it cannot be confused with the hand-off
           below, which writes the real one. */
    wrote = serverSimBotSetLuaGlobalString(sim, seat, "WB_UT_BLOCK_OPEN",
                                           "yes");
    UT_ASSERT_MSG(wrote,
                  "the block-open publish was refused for seat %d, which is "
                  "parked on a warmed runner", (int)seat);

    /* The wave fields it, which is a resume. Nothing has published
       DEBUG_SESSION_DIR to this seat: the resume has to do it. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the wave did not field the warmed seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the fielding made %d brain(s) for seat %d in all; this "
                  "case is only about a RESUME", ut_brain_stub_creates(seat),
                  (int)seat);

    /* (a), read back out of the VM the resume carried over. */
    got = serverSimBotEvalLuaString(sim, seat,
                                    "return WB_UT_BLOCK_OPEN or ''");
    UT_ASSERT_MSG(got != NULL && strcmp(got, "yes") == 0,
                  "the block-open publish did not land in seat %d's brain: "
                  "it reads '%s'", (int)seat, got != NULL ? got : "(nothing)");
    free(got);

    /* (b) AND THE RESUME HANDED OVER THE SESSION DIR ITSELF. */
    got = serverSimBotEvalLuaString(sim, seat,
                                    "return DEBUG_SESSION_DIR or ''");
    UT_ASSERT_MSG(got != NULL && strcmp(got, dir) == 0,
                  "seat %d came back on its warmed runner with "
                  "DEBUG_SESSION_DIR '%s', expected '%s': its print2 log "
                  "would land outside the recording block",
                  (int)seat, got != NULL ? got : "(nothing)", dir);
    free(got);

    brainRecordSetSessionDir("");
    brainRecordSetEnabled(false);
    ut_brain_stub_lua(false);
    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* The warm builds a held seat's runner with the seat's own init table, and
 * these seats were made by hand with no team behind them, so that table is
 * empty. A wave whose spawn names one of those seats WITH an init table cannot
 * take that runner: the table is read at the VM's first breath. So the seat
 * costs a second brain, once, and the runner the rebuild leaves behind is the
 * one the rest of the round parks and resumes.
 *
 * This is one of the two shapes now. A team that gives its seats an `init` is
 * warmed with it and its first wave is a resume, which is what
 * run_scenario_wave_cost_template_init_warms holds; this case is the other —
 * a seat whose team gave it no table, met by a spawn that carries one. That is
 * a spawn carrying configuration its team did not, not a script varying a
 * seat's table, and bot_manager says so at INFO rather than WARN. This binary
 * captures no wb_log output, so the case counts the brains and reads the
 * context; which level the line went out at is not asserted here. */
int run_scenario_wave_cost_warmed_init_rebuilds(void) {
    ServerSim *sim;
    ScenarioOp op;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("warmed_init_rebuilds"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));
    wcRunCountdown(sim, WC_SEATS + 1);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the countdown did not start the round, state %d",
                  (int)sim->state);

    /* What the warm left behind: a parked runner that was built ahead of the
       round and holds no configuration of its own. */
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the countdown left seat %d without a runner", (int)seat);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].warmed,
                  "seat %d's runner does not read as warmed", (int)seat);
    UT_ASSERT_MSG(sim->seatInit[seat].count == 0,
                  "seat %d was seated with an init table of %u entries, "
                  "expected none — it has no team behind it", (int)seat,
                  (unsigned)sim->seatInit[seat].count);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].initTable.count == 0,
                  "the warm gave seat %d an init table of %u entries, expected "
                  "none", (int)seat,
                  (unsigned)sim->botMgr.bots[seat].initTable.count);
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the warm made %d brains for seat %d, expected 1",
                  ut_brain_stub_creates(seat), (int)seat);

    /* The wave fields that one seat, carrying a configuration. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(scnTableSet(&op.u.rosterSpawnBot.init, "role", "guard"));
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the spawn did not field the seat");

    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 2,
                  "the seat has had %d brains made for it, expected 2 — the "
                  "warm's and the one the spawn's init table costs",
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "%d brains were destroyed, expected 1 — the warmed runner is "
                  "released to make room for the new one",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].warmed,
                  "the rebuilt runner still reads as warmed");
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "the fielded seat still reads as parked");
    UT_ASSERT_MSG(botManagerIsBot(sim, seat),
                  "the fielded seat is not an active bot");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].initTable.count == 1,
                  "the fielded seat holds an init table of %u entries, "
                  "expected the 1 the spawn carried",
                  (unsigned)sim->botMgr.bots[seat].initTable.count);

    /* And from here the rebuilt runner behaves like any other: off the field
       it parks carrying the spawn's table, and a wave handing it the same one
       takes it back rather than paying a third time. */
    wcRemoveOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the removal did not park the rebuilt runner");

    wcSpawnOp(&op, seat);
    UT_ASSERT(scnTableSet(&op.u.rosterSpawnBot.init, "role", "guard"));
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the refield did not field the seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 2,
                  "the seat has had %d brains made for it over the warm, the "
                  "rebuild and a refield on the same init table, expected 2",
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "%d brains were destroyed over the three, expected the 1 the "
                  "rebuild released", ut_brain_stub_destroys());

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* The other shape: a team that gives its seats an `init` table. The template
 * carries it, the seating writes it onto the seat, and the countdown warms the
 * seat's runner with it — so the first wave of the round is a resume like every
 * wave after it, which is what the warm exists to buy.
 *
 * Three spawns, in order: one naming the seat and carrying nothing, which
 * takes the seat's table; one carrying the same table by value; and one
 * carrying a different one, which is the only build. The seats are seated from
 * a template rather than by hand, because serverSimAddUnfieldedSeat takes no
 * team and so no table.
 *
 * The brain counts are per slot, so the other five seats the countdown warms
 * do not show in ut_brain_stub_creates(seat); the destroy count is the whole
 * sim's, and nothing else here releases a runner while the round runs. */
int run_scenario_wave_cost_template_init_warms(void) {
    ServerSim *sim;
    ScenarioOp op;
    ScnTable   want;
    ScnTable   other;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("template_init_warms"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);

    scnTableClear(&want);
    UT_ASSERT(scnTableSet(&want, "role", "raider"));
    UT_ASSERT(scnTableSet(&want, "tier", "3"));
    UT_ASSERT_MSG(wcSeatFromTemplate(sim, &want),
                  "the template did not seat %d held seats from slot %d",
                  WC_SEATS, (int)WC_FIRST_SEAT);

    /* On the seat before anything is built, which is where the warm reads
       it. */
    UT_ASSERT_MSG(wcSameTable(&sim->seatInit[seat], &want),
                  "seat %d carries %u init pairs, expected the team's %u",
                  (int)seat, (unsigned)sim->seatInit[seat].count,
                  (unsigned)want.count);
    UT_ASSERT_MSG(wcSeatCreates() == 0,
                  "%d brains were made seating the template, expected 0",
                  wcSeatCreates());

    wcRunCountdown(sim, WC_SEATS + 1);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the countdown did not start the round, state %d",
                  (int)sim->state);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the countdown left seat %d without a runner", (int)seat);
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the warm made %d brains for seat %d, expected 1",
                  ut_brain_stub_creates(seat), (int)seat);
    UT_ASSERT_MSG(wcSameTable(&sim->botMgr.bots[seat].initTable, &want),
                  "the warm built seat %d's runner with %u init pairs, "
                  "expected the team's %u", (int)seat,
                  (unsigned)sim->botMgr.bots[seat].initTable.count,
                  (unsigned)want.count);

    /* The first wave, naming the seat and carrying no table: it gets the
       seat's, which is what the runner was built with, so this is a resume. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the first wave did not field the seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the first wave brought seat %d to %d brains, expected the "
                  "1 the warm made — a seat warmed with its team's table is a "
                  "resume", (int)seat, ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "the first wave destroyed %d brains, expected 0",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "the fielded seat still reads as parked");
    UT_ASSERT_MSG(botManagerIsBot(sim, seat),
                  "the fielded seat is not an active bot");
    UT_ASSERT_MSG(wcSameTable(&sim->botMgr.bots[seat].initTable, &want),
                  "the fielded bot holds %u init pairs, expected the team's "
                  "%u", (unsigned)sim->botMgr.bots[seat].initTable.count,
                  (unsigned)want.count);

    /* Off the field, and back on with the same table written out by value —
       what a script that restates its team's table on every spawn sends. */
    wcRemoveOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the removal did not park the seat's runner");

    wcSpawnOp(&op, seat);
    op.u.rosterSpawnBot.init = want;
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the second wave did not field the seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "a spawn restating the team's table brought seat %d to %d "
                  "brains, expected 1", (int)seat,
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "a spawn restating the team's table destroyed %d brains, "
                  "expected 0", ut_brain_stub_destroys());

    /* And back on with a different table, which the parked runner cannot
       serve: the script has handed the one seat two tables. */
    wcRemoveOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the second removal did not park the seat's runner");

    scnTableClear(&other);
    UT_ASSERT(scnTableSet(&other, "role", "guard"));
    wcSpawnOp(&op, seat);
    op.u.rosterSpawnBot.init = other;
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the third wave did not field the seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 2,
                  "a spawn carrying another table brought seat %d to %d "
                  "brains, expected 2", (int)seat,
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "%d brains were destroyed, expected 1 — the parked runner is "
                  "released to make room for the new one",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(wcSameTable(&sim->botMgr.bots[seat].initTable, &other),
                  "the rebuilt runner holds %u init pairs, expected the "
                  "spawn's %u",
                  (unsigned)sim->botMgr.bots[seat].initTable.count,
                  (unsigned)other.count);

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

int run_scenario_wave_cost_warm_skips_bad_brain(void) {
    ServerSim *sim;
    const BYTE bad = WC_FIRST_SEAT;
    int i;

    UT_ASSERT(wcMakeBrainFile("warm_skips_bad_brain"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));

    /* One seat written to run a brain that is not on disk. The others name
       none of their own and fall back to the server's, which is. */
    SDL_strlcpy(sim->seatBrain[bad], "test_scenario_wave_cost_no_such.lua",
                sizeof(sim->seatBrain[bad]));

    wcRunCountdown(sim, WC_SEATS + 1);
    UT_ASSERT(sim->state == serverStateRunning);

    UT_ASSERT_MSG(ut_brain_stub_creates(bad) == 0,
                  "the seat naming a brain that will not load had %d made for "
                  "it, expected 0", ut_brain_stub_creates(bad));
    UT_ASSERT_MSG(!sim->botMgr.bots[bad].parked,
                  "the skipped seat has a parked runner");
    UT_ASSERT_MSG(sim->botMgr.bots[bad].cs == NULL,
                  "the skipped seat has a ClientSim");
    UT_ASSERT_MSG(sim->playerConnected[bad],
                  "the skipped seat left the roster");

    /* And the seats behind it were warmed all the same. */
    for (i = 1; i < WC_SEATS; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                      "seat %d had %d brains made for it, expected 1 — one "
                      "seat the warm cannot serve must not cost the rest "
                      "theirs", (int)seat, ut_brain_stub_creates(seat));
        UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                      "seat %d has no parked runner behind it", (int)seat);
    }
    UT_ASSERT_MSG(wcSeatCreates() == WC_SEATS - 1,
                  "the countdown made %d brains, expected %d — one for every "
                  "seat but the one it could not serve",
                  wcSeatCreates(), WC_SEATS - 1);

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* The other way a warm fails. The seat above named a brain that is not on
 * disk, so the warm refused it before it built anything; here the path
 * resolves and the build itself is refused, part way through, with the
 * ClientSim and its transport already made. Whatever a half-built runner is
 * taken back, the seat has to read afterwards as one that has never been
 * built: no ClientSim, no subscription, and no transport — a transport left
 * behind here names a context the ClientSim teardown has already freed.
 *
 * The fixture brain is put back to refusing for the countdown frames, so every
 * build reaches the brain create and comes back false. */
int run_scenario_wave_cost_failed_build_leaves_nothing(void) {
    ServerSim *sim;
    const BYTE seat  = WC_FIRST_SEAT;
    /* Frames run, and so seats tried: the warm builds one seat a frame and a
       frame whose build fails stops there, so three frames reach the first
       three seats. Fewer than WC_SEATS, so there are seats left over for the
       frame that builds again at the end. */
    const int  tried = 3;
    uint16_t   expectSkipped;
    int        skipped = 0;
    int        i;

    UT_ASSERT(wcMakeBrainFile("failed_build_leaves_nothing"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));

    ut_brain_stub_arm(false);
    wcRunCountdownFrames(sim, WC_SEATS + 1, tried);
    UT_ASSERT_MSG(sim->state == serverStateCountdown,
                  "the server is in state %d after %d countdown frames, "
                  "expected it to still be counting down", (int)sim->state,
                  tried);

    /* The first seat tried, read field by field. */
    UT_ASSERT_MSG(!botManagerHasRunner(sim, seat),
                  "seat %d holds a runner after a build that failed",
                  (int)seat);
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "seat %d reads as parked after a build that failed",
                  (int)seat);
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].active,
                  "seat %d reads as an active bot after a build that failed",
                  (int)seat);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == NULL,
                  "seat %d kept its ClientSim through a build that failed",
                  (int)seat);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub ==
                      SUBSCRIBER_HANDLE_INVALID,
                  "seat %d kept its control subscription through a build that "
                  "failed", (int)seat);
    /* The ClientSim owned the transport by the time the build was refused, so
       its teardown freed the context; a pointer left here names memory that
       has gone. */
    UT_ASSERT_MSG(sim->botMgr.bots[seat].transport.ctx == NULL,
                  "seat %d kept a transport context through a build that "
                  "failed, and the ClientSim teardown has freed it",
                  (int)seat);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].transport.recordInput == NULL &&
                      sim->botMgr.bots[seat].transport.sendInput == NULL &&
                      sim->botMgr.bots[seat].transport.tick == NULL &&
                      sim->botMgr.bots[seat].transport.getSnapshot == NULL &&
                      sim->botMgr.bots[seat].transport.drainSnapshots == NULL,
                  "seat %d kept part of its transport through a build that "
                  "failed", (int)seat);

    /* The fixture brain counts the brains it has made, and one it refused is
       not one of them, so the count stands at nothing made for the seat. What
       says the build was reached at all is the skipped bit below: the warm
       only sets it after the seat's path resolved to a file and the build came
       back false. */
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 0,
                  "the fixture brain reports %d made for seat %d, expected 0 "
                  "— every create is being refused",
                  ut_brain_stub_creates(seat), (int)seat);
    UT_ASSERT_MSG(!ut_brain_stub_made(seat),
                  "seat %d reads as having had a brain made for it",
                  (int)seat);

    /* The seat is the lobby's, exactly as it was seated: a build that fails
       costs the seat its runner and nothing else. */
    UT_ASSERT_MSG(sim->playerConnected[seat],
                  "the failed build emptied seat %d", (int)seat);
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].keepSeat,
                  "the failed build stopped seat %d being the scenario's",
                  (int)seat);
    UT_ASSERT_MSG(!sim->lobbyPlayers[seat].fielded,
                  "the failed build put seat %d on the field", (int)seat);

    /* The bit index is the player slot, so the seats the frames reached are
       the `tried` bits from WC_FIRST_SEAT up. */
    expectSkipped = (uint16_t)(((1u << tried) - 1u) << WC_FIRST_SEAT);
    UT_ASSERT_MSG(sim->warmSkippedSlots == expectSkipped,
                  "the countdown marked slots 0x%04x, expected 0x%04x — the "
                  "%d seats from slot %d that its %d frames tried",
                  (unsigned)sim->warmSkippedSlots, (unsigned)expectSkipped,
                  tried, (int)WC_FIRST_SEAT, tried);
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->warmSkippedSlots & (uint16_t)(1u << i)) skipped++;
    }
    UT_ASSERT_MSG(skipped == tried,
                  "%d slots are marked skipped, expected %d — one a frame over "
                  "%d frames", skipped, tried, tried);
    for (i = tried; i < WC_SEATS; i++) {
        const BYTE later = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(!botManagerHasRunner(sim, later),
                      "seat %d holds a runner after %d frames that never "
                      "reached it", (int)later, tried);
    }

    /* Builds succeed again from here. A marked seat is not tried a second
       time while the countdown runs, and the frame goes to the next seat that
       has not been marked. The arm resets the counts, so the one below is this
       frame's. */
    ut_brain_stub_arm(true);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->state == serverStateCountdown,
                  "the server is in state %d after one more countdown frame, "
                  "expected it to still be counting down", (int)sim->state);
    for (i = 0; i < tried; i++) {
        const BYTE marked = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(!botManagerHasRunner(sim, marked),
                      "seat %d was built for a second time by a later frame; "
                      "the countdown marked it once", (int)marked);
    }
    {
        const BYTE next = (BYTE)(WC_FIRST_SEAT + tried);
        UT_ASSERT_MSG(botManagerHasRunner(sim, next),
                      "the frame after the marked seats built nothing for "
                      "seat %d", (int)next);
        UT_ASSERT_MSG(sim->botMgr.bots[next].parked,
                      "seat %d holds a runner that does not read as parked",
                      (int)next);
        UT_ASSERT_MSG(ut_brain_stub_creates(next) == 1,
                      "seat %d had %d brains made for it by that frame, "
                      "expected 1", (int)next, ut_brain_stub_creates(next));
    }

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* A countdown given up on — someone un-readies, someone drops, or the console
 * says stop — goes back to the lobby through serverSimAbortCountdown, and the
 * runners the frames before it had built were built for the round it was
 * leading to. Left parked they would wait out a lobby that can be edited for
 * as long as it lasts, and a later round would field brains that read the
 * lobby as it stood here. */
int run_scenario_wave_cost_abort_countdown_releases(void) {
    ServerSim *sim;
    const int warmed = 3;
    int i;

    UT_ASSERT(wcMakeBrainFile("abort_countdown_releases"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));

    /* Three frames of a countdown long enough that none of them is the frame
       that starts the round: three of the six seats warmed. */
    wcRunCountdownFrames(sim, WC_SEATS + 1, warmed);
    UT_ASSERT_MSG(sim->state == serverStateCountdown,
                  "the server is in state %d after %d countdown frames, "
                  "expected it to still be counting down — there is nothing to "
                  "abandon otherwise", (int)sim->state, warmed);
    UT_ASSERT_MSG(wcSeatCreates() == warmed,
                  "%d countdown frames made %d brains, expected %d",
                  warmed, wcSeatCreates(), warmed);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "the countdown destroyed %d brains, expected 0",
                  ut_brain_stub_destroys());
    for (i = 0; i < warmed; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(botManagerHasRunner(sim, seat),
                      "seat %d holds no runner after the frame that warms it",
                      (int)seat);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                      "seat %d holds a runner that does not read as parked",
                      (int)seat);
    }
    for (i = warmed; i < WC_SEATS; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(!botManagerHasRunner(sim, seat),
                      "seat %d was warmed by a countdown that only ran %d "
                      "frames", (int)seat, warmed);
    }

    serverSimAbortCountdown(sim);

    UT_ASSERT_MSG(sim->state == serverStateLobby,
                  "the abandoned countdown left the server in state %d, "
                  "expected the lobby", (int)sim->state);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == warmed,
                  "the abandoned countdown destroyed %d brains, expected the "
                  "%d it had built", ut_brain_stub_destroys(), warmed);
    UT_ASSERT_MSG(wcSeatCreates() == warmed,
                  "%d brains had been made by the end of the abort, expected "
                  "the %d the countdown made", wcSeatCreates(), warmed);
    for (i = 0; i < WC_SEATS; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(!botManagerHasRunner(sim, seat),
                      "seat %d still holds a runner after the countdown was "
                      "abandoned", (int)seat);
        UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                      "seat %d still reads as parked after the countdown was "
                      "abandoned", (int)seat);
        /* The seats themselves are the lobby's, not the round's: the abort
           releases what was built for the round and leaves the roster. */
        UT_ASSERT_MSG(!sim->lobbyPlayers[seat].fielded,
                      "the abort put seat %d on the field", (int)seat);
        UT_ASSERT_MSG(sim->lobbyPlayers[seat].keepSeat,
                      "the abort stopped seat %d being the scenario's",
                      (int)seat);
    }
    /* The three the countdown had got to, in detail. The seats behind them
       were never touched, and a slot that has held nothing still reads its
       control subscription as the zero it was allocated with rather than the
       invalid handle a release writes. */
    for (i = 0; i < warmed; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == NULL,
                      "seat %d kept its ClientSim through the abort",
                      (int)seat);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub ==
                          SUBSCRIBER_HANDLE_INVALID,
                      "seat %d kept its control subscription through the "
                      "abort", (int)seat);
    }
    UT_ASSERT_MSG(wcConnectedSeats(sim) == WC_CONNECTED,
                  "the roster holds %d seats after the abort, expected %d — "
                  "one human and %d held seats",
                  wcConnectedSeats(sim), WC_CONNECTED, WC_SEATS);

    /* The countdown that does run. Every seat is warmed from nothing, so the
       three the abandoned one had built cost a second brain each — which is
       what the release bought: those three read the lobby as it stands now. */
    wcRunCountdown(sim, WC_SEATS + 1);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the second countdown did not start the round, state %d",
                  (int)sim->state);
    for (i = 0; i < WC_SEATS; i++) {
        const BYTE seat = (BYTE)(WC_FIRST_SEAT + i);
        const int expect = (i < warmed) ? 2 : 1;
        UT_ASSERT_MSG(ut_brain_stub_creates(seat) == expect,
                      "seat %d has had %d brains made for it over the two "
                      "countdowns, expected %d", (int)seat,
                      ut_brain_stub_creates(seat), expect);
        UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                      "seat %d has no parked runner behind it after the second "
                      "countdown", (int)seat);
    }

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* Where the record of refused seats is cleared. serverSimWarmOneHeldSeat marks
 * a seat it could not serve so the rest of the countdown does not try it
 * again, and the one place that record goes back to empty is the all-ready
 * check that opens a countdown. Left uncleared, a bit set in one countdown
 * would cost that seat its warm in every countdown after it, and the round
 * would field the seat by building its runner mid-play.
 *
 * Reaching a second countdown takes a first round. A fresh sim holds the world
 * the create loaded and this binary has no wire clients, so the first all-
 * ready check starts the round in place with no countdown at all; the return
 * to lobby resets the world, which is what makes every later round take the
 * countdown. So the first round here is driven by the all-ready check itself,
 * and the stale bit is planted in the lobby it comes back to. */
int run_scenario_wave_cost_all_ready_clears_skips(void) {
    ServerSim *sim;
    const BYTE seat = WC_FIRST_SEAT;
    int i;

    UT_ASSERT(wcMakeBrainFile("all_ready_clears_skips"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatTeam(sim));

    /* The first round. The human in slot 0 is ready and the seats read ready
       as they are seated, so the check has everything it needs. */
    UT_ASSERT(sim->state == serverStateLobby);
    serverSimLobbyCheckAllReady(sim);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the first all-ready check left the server in state %d, "
                  "expected it to start the round in place", (int)sim->state);
    UT_ASSERT_MSG(wcSeatCreates() == 0,
                  "the round that started without a countdown made %d brains, "
                  "expected 0 — there was no countdown to warm in",
                  wcSeatCreates());

    /* Back to the lobby, which resets the world and so sends the next round
       through the countdown. */
    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(sim->state == serverStateLobby,
                  "the round end left the server in state %d, expected the "
                  "lobby", (int)sim->state);
    for (i = 0; i < WC_SEATS; i++) {
        UT_ASSERT_MSG(sim->playerConnected[WC_FIRST_SEAT + i],
                      "the round end emptied seat %d", WC_FIRST_SEAT + i);
        UT_ASSERT_MSG(sim->lobbyPlayers[WC_FIRST_SEAT + i].ready,
                      "seat %d does not read as ready back in the lobby",
                      WC_FIRST_SEAT + i);
    }

    /* A bit left over from a countdown that has been and gone, on the first
       seat. The index is the player slot, so this is slot WC_FIRST_SEAT. */
    sim->warmSkippedSlots = (uint16_t)(1u << WC_FIRST_SEAT);
    /* The human readies up again — the round end cleared every human's ready
       flag, and that is what drives the check. */
    sim->lobbyPlayers[0].ready = true;

    serverSimLobbyCheckAllReady(sim);

    UT_ASSERT_MSG(sim->state == serverStateCountdown,
                  "the second all-ready check left the server in state %d, "
                  "expected a countdown — the world is no longer the one the "
                  "create loaded, so this round cannot start in place",
                  (int)sim->state);
    UT_ASSERT_MSG(sim->warmSkippedSlots == 0,
                  "the countdown opened with slots 0x%04x marked as refused, "
                  "expected none — the record is one countdown's",
                  (unsigned)sim->warmSkippedSlots);

    /* And the seat the stale bit named is warmed by the first frame, which is
       what the clear buys. */
    serverSimTick(sim);
    UT_ASSERT_MSG(botManagerHasRunner(sim, seat),
                  "the first countdown frame built nothing for seat %d",
                  (int)seat);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "seat %d holds a runner that does not read as parked",
                  (int)seat);
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "seat %d had %d brains made for it by the first countdown "
                  "frame, expected 1", (int)seat, ut_brain_stub_creates(seat));

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* ── A seat told new orders keeps its runner ──────────────────────── */

/* The shape Survival's waves have: a team gives its held seats an init table,
 * the countdown warms a runner per seat with it, and each wave fields a seat
 * with a spawn that carries NO table of its own — which matches the seat's and
 * resumes — then hands that bot the wave's own values through game.bot_init.
 *
 * The question this case asks is what the NEXT wave costs. bot_init replaces
 * the live init table, and the live table used to be what a park was keyed on,
 * so the next wave's spawn was measured against wave 1's orders rather than
 * against what the VM was built reading: it missed, every attacker paid for a
 * fresh brain, and the ten runners the countdown bought were thrown away one a
 * wave. The park is keyed on builtInit now, which nothing but a build writes.
 *
 * Three brains would say the bug is back: the warm's, wave 1's and wave 2's.
 * One says every fielding after the warm was a resume. */
int run_scenario_wave_cost_bot_init_keeps_park(void) {
    ServerSim *sim;
    ScenarioOp op;
    ScnTable   team;
    ScnTable   wave;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("bot_init_keeps_park"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);

    /* A team that gives its seats a table, as the horde's does. */
    memset(&team, 0, sizeof(team));
    UT_ASSERT(scnTableSet(&team, "blitz", "2/4"));
    UT_ASSERT(wcSeatFromTemplate(sim, &team));
    wcRunCountdown(sim, WC_SEATS + 1);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the countdown did not start the round, state %d",
                  (int)sim->state);
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the countdown left seat %d without a runner", (int)seat);
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the warm made %d brains for seat %d, expected 1",
                  ut_brain_stub_creates(seat), (int)seat);

    /* Wave 1 fields it with no table of its own, which is the team's. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the first wave's spawn did not field the seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the seat has had %d brains made for it, expected 1 — a "
                  "spawn carrying no table of its own is the seat's own table "
                  "and resumes the warm", ut_brain_stub_creates(seat));

    /* And is then told the wave's own orders. */
    memset(&wave, 0, sizeof(wave));
    UT_ASSERT(scnTableSet(&wave, "blitz", "3/4"));
    UT_ASSERT(scnTableSet(&wave, "noblitz", "1"));
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_BOT_INIT;
    op.u.rosterBotInit.slot = seat;
    op.u.rosterBotInit.init = wave;
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->botMgr.bots[seat].initTable.count == 2,
                  "the bot's live table holds %u entries after bot_init, "
                  "expected the wave's 2",
                  (unsigned)sim->botMgr.bots[seat].initTable.count);

    /* The wave ends: the seat comes off the field and its runner parks. */
    wcRemoveOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(!sim->lobbyPlayers[seat].fielded,
                  "the removal left the seat on the field");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].parked,
                  "the removal did not park seat %d's runner", (int)seat);

    /* Wave 2, the same spawn. It must resume the runner wave 1 parked,
       whatever wave 1 told that bot afterwards. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(wcApplyOne(sim, &op));
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the second wave's spawn did not field the seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the seat has had %d brains made for it, expected 1 — the "
                  "orders wave 1 gave it must not cost wave 2 a new one",
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 0,
                  "%d brains were destroyed, expected 0",
                  ut_brain_stub_destroys());
    UT_ASSERT_MSG(!sim->botMgr.bots[seat].parked,
                  "the refielded seat still reads as parked");

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}
