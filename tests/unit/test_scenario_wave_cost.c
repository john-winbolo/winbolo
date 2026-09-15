/*
 * What a wave costs: the runner behind a held seat, counted where it is built
 * and torn down.
 *
 * A held seat survives a wave. serverSimUnfieldBot keeps the roster entry, the
 * name, the team, the alliance and the connection, so there is no leave to
 * announce and no client to resync. The runner behind the seat does not
 * survive it: the same call reaches botManagerRemoveBotKeepSeat and then
 * botTearDownRunner, which destroys the brain instance, unregisters the
 * control subscription and destroys the ClientSim. The next spawn naming the
 * seat goes back through botManagerAddBot and builds all three again.
 *
 * These two cases put a number on that, counted at the calls that do the work
 * rather than inferred from the roster: the fixture brain in test_stubs.c
 * records how many brains each slot has been made and how many have been
 * destroyed in all, and the ClientSim and the subscription are read off
 * sim->botMgr.bots[].
 *
 * Both cases field the way a script does — a spawn op onto the roster queue,
 * which the drain makes one entry to a tick — and unfield through the remove
 * op, which on a seat marked keepSeat reaches serverSimUnfieldBot instead of
 * emptying the seat.
 *
 * run_scenario_wave_cost_refield_rebuilds  — one seat fielded, unfielded and
 *                                            fielded again: two brains made,
 *                                            one destroyed, and the seat
 *                                            itself untouched throughout
 * run_scenario_wave_cost_horde_swap_counts — six seats swapped twice: twelve
 *                                            brains made, six destroyed, and
 *                                            no seat off the roster
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
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath */
#include "server_sim_scenario.h"
#include "bot_manager.h"           /* BotContext: the ClientSim and the sub */
#include "game_sim.h"
#include "everard_map.h"
#include "test_harness.h"

/* Slot 0 holds the human; the horde takes the six seats after it, all on one
 * team, which is the Survival shape. The one-seat case takes the first of
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

static bool wcMakeBrainFile(const char *tag) {
    FILE *f;

    SDL_snprintf(wcBrainPath, sizeof(wcBrainPath),
                 "test_scenario_wave_cost_brain_%s.lua", tag);
    f = fopen(wcBrainPath, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

static void wcDropBrainFile(void) {
    remove(wcBrainPath);
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

/* ── One seat, fielded twice ──────────────────────────────────────── */

int run_scenario_wave_cost_refield_rebuilds(void) {
    ServerSim *sim;
    ScenarioOp op;
    const BYTE seat = WC_FIRST_SEAT;

    UT_ASSERT(wcMakeBrainFile("refield_rebuilds"));
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
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
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

    /* Off the field again: the seat stays, everything behind it goes. */
    wcRemoveOp(&op, seat);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    UT_ASSERT_MSG(!sim->lobbyPlayers[seat].fielded,
                  "the removal left the seat on the field");

    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 1,
                  "the seat has had %d brains made for it after one field and "
                  "one unfield, expected 1", ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "taking the seat off the field destroyed %d brains, "
                  "expected 1", ut_brain_stub_destroys());
    UT_ASSERT_MSG(sim->botMgr.bots[seat].cs == NULL,
                  "the unfielded seat still holds a ClientSim");
    UT_ASSERT_MSG(sim->botMgr.bots[seat].controlSub ==
                      SUBSCRIBER_HANDLE_INVALID,
                  "the unfielded seat still holds a control subscription");

    /* And the seat itself is exactly where it was. */
    UT_ASSERT_MSG(sim->playerConnected[seat],
                  "the unfield emptied the seat");
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].keepSeat,
                  "the unfield stopped the seat being the scenario's");

    /* Field it a second time: another brain made, and nothing destroyed for
       it, so what the refield costs is a second build and not a swap. */
    wcSpawnOp(&op, seat);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->lobbyPlayers[seat].fielded,
                  "the second spawn did not field the seat");
    UT_ASSERT_MSG(ut_brain_stub_creates(seat) == 2,
                  "the seat has had %d brains made for it over a field, an "
                  "unfield and a refield, expected 2",
                  ut_brain_stub_creates(seat));
    UT_ASSERT_MSG(ut_brain_stub_destroys() == 1,
                  "%d brains were destroyed over the three, expected 1",
                  ut_brain_stub_destroys());

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
    UT_ASSERT_MSG(ut_brain_stub_destroys() == WC_SEATS,
                  "taking the wave off the field destroyed %d brains, "
                  "expected %d", ut_brain_stub_destroys(), WC_SEATS);
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
    UT_ASSERT_MSG(wcHordeCreates() == 2 * WC_SEATS,
                  "two waves over %d held seats made %d brains, expected %d — "
                  "the runner behind each seat is built again for the second "
                  "wave", WC_SEATS, wcHordeCreates(), 2 * WC_SEATS);
    UT_ASSERT_MSG(ut_brain_stub_destroys() == WC_SEATS,
                  "%d brains were destroyed over the two waves, expected %d",
                  ut_brain_stub_destroys(), WC_SEATS);
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
