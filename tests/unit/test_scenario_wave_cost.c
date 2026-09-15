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
 *                                               throughout
 * run_scenario_wave_cost_horde_swap_counts    — six seats swapped twice: six
 *                                               brains made, none destroyed,
 *                                               and no seat off the roster
 * run_scenario_wave_cost_other_brain_rebuilds — a refield naming another brain
 *                                               cannot have the parked runner
 * run_scenario_wave_cost_other_init_rebuilds  — nor can one carrying another
 *                                               init table
 * run_scenario_wave_cost_round_end_releases   — a parked runner does not cross
 *                                               a round boundary
 * run_scenario_wave_cost_horde_parked_releases— and neither does a whole horde
 *                                               of them, which is the shape no
 *                                               active bot is left to carry
 * run_scenario_wave_cost_destroy_releases     — nor does one survive
 *                                               botManagerDestroy
 * run_scenario_wave_cost_countdown_warms_seats— the countdown leaves a parked
 *                                               runner behind every held seat
 * run_scenario_wave_cost_warmed_field_is_free — and the wave that fields one
 *                                               builds nothing
 * run_scenario_wave_cost_warm_skips_bad_brain — a seat whose brain will not
 *                                               load is skipped, and the rest
 *                                               are still warmed
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
#include "scenario_table.h"        /* scnTableSet — the init table a spawn carries */
#include "game_sim.h"
#include "everard_map.h"
#include "test_harness.h"

/* Slot 0 holds the human; the horde takes the six seats after it, all on one
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

static bool wcWriteBrainFile(char *out, size_t outCap, const char *name) {
    FILE *f;

    SDL_strlcpy(out, name, outCap);
    f = fopen(out, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
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
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
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

/* The brains made for the horde's six seats, added up. */
static int wcHordeCreates(void) {
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
 * round. */
static void wcRunCountdown(ServerSim *sim, int ticks) {
    int i;

    sim->state = serverStateCountdown;
    sim->countdownTicks = ticks;
    sim->warmSkippedSlots = 0;
    for (i = 0; i < ticks; i++) {
        serverSimTick(sim);
    }
}

/* Seat the horde in a fresh lobby, named as the template names them. */
static bool wcSeatHorde(ServerSim *sim) {
    int i;

    for (i = 0; i < WC_SEATS; i++) {
        char name[PLAYER_NAME_LEN];
        SDL_snprintf(name, sizeof(name), "Horde%d", i + 1);
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
    if (!serverSimAddUnfieldedSeat(sim, seat, "Horde1", WC_TEAM)) {
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
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, seat, "Horde1", WC_TEAM));
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

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}

/* ── Six seats, swapped twice ─────────────────────────────────────── */

int run_scenario_wave_cost_horde_swap_counts(void) {
    ServerSim *sim;
    int i;

    UT_ASSERT(wcMakeBrainFile("horde_swap_counts"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);

    for (i = 0; i < WC_SEATS; i++) {
        char name[PLAYER_NAME_LEN];
        SDL_snprintf(name, sizeof(name), "Horde%d", i + 1);
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
    UT_ASSERT_MSG(wcHordeCreates() == 0,
                  "%d brains were made before the first wave, expected 0",
                  wcHordeCreates());

    /* The first wave. */
    UT_ASSERT_MSG(wcFieldAll(sim),
                  "the first wave did not field all %d seats", WC_SEATS);
    UT_ASSERT_MSG(wcHordeCreates() == WC_SEATS,
                  "the first wave made %d brains, expected %d",
                  wcHordeCreates(), WC_SEATS);
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
    UT_ASSERT_MSG(wcHordeCreates() == WC_SEATS,
                  "%d brains had been made by the end of the transition, "
                  "expected the %d the first wave made",
                  wcHordeCreates(), WC_SEATS);
    UT_ASSERT_MSG(wcConnectedSeats(sim) == WC_CONNECTED,
                  "the roster holds %d seats with the wave off the field, "
                  "expected %d", wcConnectedSeats(sim), WC_CONNECTED);

    /* The second wave, into the same six seats. */
    UT_ASSERT_MSG(wcFieldAll(sim),
                  "the second wave did not field all %d seats", WC_SEATS);
    UT_ASSERT_MSG(wcHordeCreates() == WC_SEATS,
                  "two waves over %d held seats made %d brains, expected %d — "
                  "the runner behind each seat is handed back to the second "
                  "wave", WC_SEATS, wcHordeCreates(), WC_SEATS);
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

int run_scenario_wave_cost_other_init_rebuilds(void) {
    ServerSim *sim;
    ScenarioOp op;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("other_init_rebuilds"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, seat, "Horde1", WC_TEAM));
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

/* A whole horde parked and nothing active — the shape a Wave Defense round
 * ends in, its last wave unfielded before the round is won. Nothing is left
 * on the field to be walked over, so a release that only ever ran for an
 * active bot never ran at all, and every one of those brains opened the next
 * round still remembering this one. */
int run_scenario_wave_cost_horde_parked_releases(void) {
    ServerSim *sim;
    int i;

    UT_ASSERT(wcMakeBrainFile("horde_parked_releases"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatHorde(sim));
    serverSimStartGame(sim);
    UT_ASSERT(sim->state == serverStateRunning);

    UT_ASSERT_MSG(wcFieldAll(sim), "the wave did not field all %d seats",
                  WC_SEATS);
    UT_ASSERT_MSG(wcUnfieldAll(sim),
                  "the wave did not come off the field");
    UT_ASSERT_MSG(wcHordeCreates() == WC_SEATS,
                  "the wave made %d brains, expected %d",
                  wcHordeCreates(), WC_SEATS);
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
                  "the pool counts %u active bots with the whole horde parked, "
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

/* ── The countdown builds what the round will need ────────────────── */

int run_scenario_wave_cost_countdown_warms_seats(void) {
    ServerSim *sim;
    int i;

    UT_ASSERT(wcMakeBrainFile("countdown_warms_seats"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatHorde(sim));
    UT_ASSERT_MSG(wcHordeCreates() == 0,
                  "%d brains were made seating the horde, expected 0",
                  wcHordeCreates());

    /* One frame per seat, and one more for the frame that starts the round. */
    wcRunCountdown(sim, WC_SEATS + 1);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the countdown did not start the round, state %d",
                  (int)sim->state);

    UT_ASSERT_MSG(wcHordeCreates() == WC_SEATS,
                  "the countdown made %d brains, expected one for each of the "
                  "%d held seats", wcHordeCreates(), WC_SEATS);
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
    UT_ASSERT(wcSeatHorde(sim));
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

int run_scenario_wave_cost_warm_skips_bad_brain(void) {
    ServerSim *sim;
    const BYTE bad = WC_FIRST_SEAT;
    int i;

    UT_ASSERT(wcMakeBrainFile("warm_skips_bad_brain"));
    ut_brain_stub_arm(true);
    sim = wcLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(wcSeatHorde(sim));

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
    UT_ASSERT_MSG(wcHordeCreates() == WC_SEATS - 1,
                  "the countdown made %d brains, expected %d — one for every "
                  "seat but the one it could not serve",
                  wcHordeCreates(), WC_SEATS - 1);

    serverSimDestroy(sim);
    wcDropBrainFile();
    return 0;
}
