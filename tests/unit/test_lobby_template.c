/*
 * The lobby template: the engine seating a scenario's teams at the points a
 * lobby is built or rebuilt.
 *
 * The template reaches the sim as data — serverSimSetScenarioLobbyTemplate
 * takes a copy — so every case here builds one by hand and never needs a
 * sidecar, a manifest or a Lua VM. That is the shape under test as much as
 * the seating is: the engine applies this without calling back out.
 *
 * Three paths apply it. A committed map seats it from scratch. A lobby
 * coming back from a round reconciles instead: what the host changed between
 * rounds stands, a team past its ceiling is cut back, and the seats a script
 * fielded during the round go back to being held. An emptied lobby resetting
 * to the operator's defaults seats it again at the end, so the next joiner
 * opens the scenario's lobby rather than a bare one.
 *
 * The unit binary has no Lua brain, so luaBrainInstanceCreate is the fixture
 * brain in test_stubs.c, as it is for the roster ops.
 *
 * run_lobby_template_map_commit_seats   — the teams, the counts, unfielded
 *                                         where the template says so
 * run_lobby_template_return_reconciles  — a trim survives, a ceiling binds,
 *                                         an emptied team stays empty
 * run_lobby_template_return_unfields    — what the script fielded is held
 *                                         again, name and team intact
 * run_lobby_template_reset_reseats      — the emptied lobby gets it back
 * run_lobby_template_preview_is_inert   — a preview changes no seat
 * run_lobby_template_plain_map_clears   — a map with no scenario keeps none
 * run_lobby_template_caps_humans_only   — max_players binds humans, not bots
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, lobbyPlayers, the template copy */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath,
                                    * and the template entry points */
#include "server_sim_scenario.h"
#include "server_sim_join.h"       /* serverSimFindFreeSlot */
#include "game_sim.h"
#include "everard_map.h"
#include "test_harness.h"

/* The two teams every case templates, unless it says otherwise. */
#define LT_HORDE  3     /* unfielded, the Survival shape */
#define LT_GUARD  4     /* fielded */

static char ltBrainPath[128];

static bool ltMakeBrainFile(const char *tag) {
    FILE *f;

    SDL_snprintf(ltBrainPath, sizeof(ltBrainPath),
                 "test_lobby_template_brain_%s.lua", tag);
    f = fopen(ltBrainPath, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

static void ltDropBrainFile(void) {
    remove(ltBrainPath);
}

/* A lobby taking part, with one human in slot 0 and a server that runs
 * bots. */
static ServerSim *ltLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Human", false);
    sim->lobbyPlayers[0].ready = true;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, ltBrainPath);
    return sim;
}

/* One unfielded team of `horde` seats and one fielded team of `guard`. */
static void ltTemplate(ScnLobbyTemplate *t, BYTE horde, BYTE hordeMax,
                       BYTE guard, BYTE guardMax) {
    memset(t, 0, sizeof(*t));
    t->numTeams = 2;
    t->teams[0].id      = LT_HORDE;
    t->teams[0].bots    = horde;
    t->teams[0].maxBots = hordeMax;
    t->teams[0].fielded = false;
    t->teams[1].id      = LT_GUARD;
    t->teams[1].bots    = guard;
    t->teams[1].maxBots = guardMax;
    t->teams[1].fielded = true;
    SDL_strlcpy(t->teams[1].brain, ltBrainPath, sizeof(t->teams[1].brain));
}

/* Seats the scenario put there, on one team. */
static int ltSeats(ServerSim *sim, BYTE team) {
    int n = 0, i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].keepSeat &&
            sim->lobbyPlayers[i].teamNumber == team) {
            n++;
        }
    }
    return n;
}

static int ltFirstSeat(ServerSim *sim, BYTE team) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].keepSeat &&
            sim->lobbyPlayers[i].teamNumber == team) {
            return i;
        }
    }
    return -1;
}

/* ── A committed map seats it ─────────────────────────────────────── */

int run_lobby_template_map_commit_seats(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              i;
    int              unfielded = 0;

    UT_ASSERT(ltMakeBrainFile("map_commit_seats"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 4, 4, 2, 2);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    /* What a map commit runs once the map is in and the starts are
       reconciled. */
    serverSimScenarioSeatLobby(sim);

    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 4,
                  "the unfielded team has %d seats, expected 4",
                  ltSeats(sim, LT_HORDE));
    UT_ASSERT_MSG(ltSeats(sim, LT_GUARD) == 2,
                  "the fielded team has %d seats, expected 2",
                  ltSeats(sim, LT_GUARD));

    /* The human is untouched and is not one of the scenario's. */
    UT_ASSERT(sim->playerConnected[0]);
    UT_ASSERT_MSG(!sim->lobbyPlayers[0].keepSeat,
                  "the human's seat was taken for the template");

    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || !sim->lobbyPlayers[i].keepSeat) continue;
        if (sim->lobbyPlayers[i].teamNumber == LT_HORDE) {
            UT_ASSERT_MSG(!sim->lobbyPlayers[i].fielded,
                          "horde seat %d is on the field", i);
            UT_ASSERT_MSG(sim->sim.tanks[i] == NULL,
                          "horde seat %d has a tank", i);
            unfielded++;
        } else {
            UT_ASSERT_MSG(sim->lobbyPlayers[i].fielded,
                          "guard seat %d is not on the field", i);
        }
        UT_ASSERT_MSG(serverSimIsBot(sim, (BYTE)i),
                      "seat %d does not read as a bot", i);
    }
    UT_ASSERT(unfielded == 4);

    /* Both numbers: six seats in the roster with the human, and only the
       human and the two guards on the field. */
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 7, "roster %u, expected 7",
                  (unsigned)serverSimGetNumPlayers(sim));
    UT_ASSERT_MSG(serverSimGetNumFielded(sim) == 3, "fielded %u, expected 3",
                  (unsigned)serverSimGetNumFielded(sim));

    /* The template's teams are in use, so the lobby lists them. */
    UT_ASSERT_MSG(sim->teams[LT_HORDE].in_use,
                  "the horde's team is not in use");
    UT_ASSERT_MSG(sim->teams[LT_GUARD].in_use,
                  "the guard's team is not in use");

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── A returning lobby reconciles ─────────────────────────────────── */

int run_lobby_template_return_reconciles(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              slot;

    UT_ASSERT(ltMakeBrainFile("return_reconciles"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    /* Ten seeded, ten the most a host may leave. */
    ltTemplate(&t, 10, 10, 1, 4);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 10,
                  "seeded %d of the horde, expected 10",
                  ltSeats(sim, LT_HORDE));

    /* The host trims the horde to six, which is the whole point of seating
       them where a host can see them. */
    while (ltSeats(sim, LT_HORDE) > 6) {
        slot = ltFirstSeat(sim, LT_HORDE);
        UT_ASSERT(slot >= 0);
        serverSimRemoveBot(sim, (BYTE)slot);
    }
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 6);

    serverSimScenarioReconcileLobby(sim);
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 6,
                  "the host's trim to six came back as %d — bots is how many "
                  "the engine seeds, not a floor it re-imposes",
                  ltSeats(sim, LT_HORDE));

    /* The guard's ceiling is four and the host has gone past it. */
    while (ltSeats(sim, LT_GUARD) < 6) {
        int free = serverSimFindFreeSlot(sim, true);
        UT_ASSERT(free >= 0);
        UT_ASSERT(serverSimAddUnfieldedSeat(sim, (BYTE)free, "Extra",
                                            LT_GUARD));
    }
    serverSimScenarioReconcileLobby(sim);
    UT_ASSERT_MSG(ltSeats(sim, LT_GUARD) == 4,
                  "the guard sits at %d with a ceiling of four",
                  ltSeats(sim, LT_GUARD));

    /* A team the host emptied altogether stays empty: the same edit as the
       trim to six, only further. */
    while (ltSeats(sim, LT_HORDE) > 0) {
        slot = ltFirstSeat(sim, LT_HORDE);
        UT_ASSERT(slot >= 0);
        serverSimRemoveBot(sim, (BYTE)slot);
    }
    serverSimScenarioReconcileLobby(sim);
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 0,
                  "an emptied team came back with %d seats",
                  ltSeats(sim, LT_HORDE));

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── A returning lobby holds the seats again ──────────────────────── */

int run_lobby_template_return_unfields(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              slot;
    char             before[PLAYER_NAME_LEN];
    char             after[PLAYER_NAME_LEN];

    UT_ASSERT(ltMakeBrainFile("return_unfields"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 2, 2, 0, 0);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);

    slot = ltFirstSeat(sim, LT_HORDE);
    UT_ASSERT(slot >= 0);
    playersGetPlayerName(&sim->sim.plyrs, (BYTE)slot, before, sizeof(before),
                         TRUE);

    /* The round runs and a wave fields the seat, which is what a spawn
       naming it does. */
    serverSimStartGame(sim);
    {
        ServerSimBotConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.brainPath  = ltBrainPath;
        cfg.brainName  = before;
        cfg.ai         = aiFull;
        cfg.gameType   = gameOpen;
        cfg.teamNumber = LT_HORDE;
        UT_ASSERT(serverSimAddBot(sim, (BYTE)slot, &cfg));
    }
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].fielded,
                  "the seat did not field");

    serverSimReturnToLobby(sim);

    UT_ASSERT_MSG(sim->playerConnected[slot],
                  "the seat the script fielded was emptied on return");
    UT_ASSERT_MSG(!sim->lobbyPlayers[slot].fielded,
                  "the seat the script fielded is still on the field");
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].keepSeat,
                  "the seat stopped being the scenario's");
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == LT_HORDE,
                  "the seat came back on team %u, expected %u",
                  (unsigned)sim->lobbyPlayers[slot].teamNumber,
                  (unsigned)LT_HORDE);
    playersGetPlayerName(&sim->sim.plyrs, (BYTE)slot, after, sizeof(after),
                         TRUE);
    UT_ASSERT_MSG(strcmp(before, after) == 0,
                  "the seat came back called '%s', was '%s'", after, before);
    UT_ASSERT_MSG(sim->sim.tanks[slot] == NULL,
                  "the held seat still has a tank");

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── An emptied lobby gets it back ────────────────────────────────── */

int run_lobby_template_reset_reseats(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;

    UT_ASSERT(ltMakeBrainFile("reset_reseats"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 3, 3, 0, 0);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 3);

    /* The last human leaves, which is what reaches the reset. */
    serverSimRemovePlayer(sim, 0);

    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0,
                  "the lobby should be humanless");
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 3,
                  "the next joiner opens a lobby with %d of the horde, "
                  "expected the template's three",
                  ltSeats(sim, LT_HORDE));

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── A preview changes nothing ────────────────────────────────────── */

int run_lobby_template_preview_is_inert(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    LobbyPlayer      before[MAX_TANKS];
    bool             connBefore[MAX_TANKS];
    uint8_t         *bytes = NULL;
    size_t           len   = 0;

    UT_ASSERT(ltMakeBrainFile("preview_is_inert"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 3, 3, 1, 1);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 3);

    memcpy(before, sim->lobbyPlayers, sizeof(before));
    memcpy(connBefore, sim->playerConnected, sizeof(connBefore));

    /* The one thing a preview request asks of the sim: read a map file so
       its bytes can be streamed to whoever asked. Everything else the
       request does is transport. Whether the file is there does not matter
       here — what matters is that a host curious about a scenario map is not
       charged their teams for looking. */
    (void)serverSimReadMapFile(sim, "Everard Island.map", &bytes, &len);
    if (bytes != NULL) free(bytes);

    UT_ASSERT_MSG(memcmp(before, sim->lobbyPlayers, sizeof(before)) == 0,
                  "a preview changed the roster");
    UT_ASSERT_MSG(memcmp(connBefore, sim->playerConnected,
                         sizeof(connBefore)) == 0,
                  "a preview changed who is connected");
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 3);
    UT_ASSERT(ltSeats(sim, LT_GUARD) == 1);

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── A map with no scenario keeps no seats ────────────────────────── */

int run_lobby_template_plain_map_clears(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;

    UT_ASSERT(ltMakeBrainFile("plain_map_clears"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 4, 4, 1, 1);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 4);
    UT_ASSERT(ltSeats(sim, LT_GUARD) == 1);

    /* A plain map is committed: whoever owns the scenario detaches and
       clears the template, and the seating that follows finds none. */
    serverSimSetScenarioLobbyTemplate(sim, NULL);
    serverSimScenarioSeatLobby(sim);

    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 0,
                  "%d of the previous scenario's horde survived a plain map",
                  ltSeats(sim, LT_HORDE));
    UT_ASSERT_MSG(ltSeats(sim, LT_GUARD) == 0,
                  "%d of the previous scenario's guard survived a plain map",
                  ltSeats(sim, LT_GUARD));
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 1,
                  "the roster holds %u, expected the human alone",
                  (unsigned)serverSimGetNumPlayers(sim));
    UT_ASSERT_MSG(sim->playerConnected[0],
                  "the human went with the scenario's seats");

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── The human cap binds humans ───────────────────────────────────── */

int run_lobby_template_caps_humans_only(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              seat;

    UT_ASSERT(ltMakeBrainFile("caps_humans_only"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    /* Two humans, and a horde far larger than that. */
    memset(&t, 0, sizeof(t));
    t.maxPlayers = 2;
    t.numTeams   = 1;
    t.teams[0].id      = LT_HORDE;
    t.teams[0].bots    = 8;
    t.teams[0].maxBots = 8;
    t.teams[0].fielded = false;
    serverSimSetScenarioLobbyTemplate(sim, &t);

    /* Slot 0 holds a human, so one human seat is left. */
    seat = serverSimFindFreeSlot(sim, false);
    UT_ASSERT_MSG(seat == 1, "the second human seat is %d, expected 1", seat);
    serverSimAddPlayer(sim, (BYTE)seat, "Human2", false);
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) < 0,
                  "a third human seated under a cap of two");

    /* Bots seat above it all the same. */
    serverSimScenarioSeatLobby(sim);
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 8,
                  "the horde seated %d of eight under a human cap of two",
                  ltSeats(sim, LT_HORDE));
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) < 0,
                  "the human cap stopped binding once bots seated");

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}
