/*
 * The lobby template: the engine seating a scenario's teams at the points a
 * lobby is built or rebuilt.
 *
 * The template reaches the sim as data — serverSimSetScenarioLobbyTemplate
 * takes a copy — so every case here builds one by hand and never needs a
 * script, a manifest or a Lua VM. That is the shape under test as much as
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
 * run_lobby_template_cap_seats_human_above_bots
 *                                       — bots in the lowest slots take no
 *                                         human's place
 * run_lobby_template_cap_refuses_extra_human
 *                                       — the cap still binds once that many
 *                                         people are in
 * run_lobby_template_cap_never_binds_bots
 *                                       — and never binds a bot
 * run_lobby_template_cancel_keeps_trim  — a cancelled preview gives the
 *                                         host's trim back
 * run_lobby_template_cancel_keeps_empty_team
 *                                       — and gives an emptied team back
 *                                         empty
 * run_lobby_template_cancel_chain_rolls_back
 *                                       — one cancel over two previews goes
 *                                         back to before the first
 * run_lobby_template_commit_keeps_new_lobby
 *                                       — a commit keeps the previewed map's
 *                                         lobby and restores nothing
 * run_lobby_template_cancel_restores_path_inmem
 *                                       — a cancel after an uploaded-map
 *                                         preview gives the map's file back
 * run_lobby_template_cancel_restores_path_random
 *                                       — and after a generated-map preview
 * run_lobby_template_seat_carries_init  — a team's init table reaches the
 *                                         seats it makes, and a seat leaving
 *                                         takes it with it
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
#include "bot_manager.h"           /* BotContext.initTable — what a fielded
                                    * seat's bot was built with */
#include "mapgen.h"                /* MapGenConfig, mapGenDefaultConfig — the
                                    * generated-map preview */
#include "game_sim.h"
#include "everard_map.h"
#include "test_harness.h"

/* The two teams every case templates, unless it says otherwise. */
#define LT_HORDE  3     /* unfielded, the Survival shape */
#define LT_GUARD  4     /* fielded */

/* The human cap the three cases at the foot of this file work against. */
#define LT_CAP    4

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

/* A lobby with nobody in it, for the cases that write the roster by hand.
 * Those never seat a real bot, so there is no brain for this one to name. */
static ServerSim *ltBareSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* Seats written straight onto the roster. A bot seat is spelled the way an
 * unfielded one is — connected, marked a bot, and nothing behind it in the
 * bot manager — because that is the seat that used to take a human's place. */
static void ltFillSeats(ServerSim *sim, BYTE n, bool bots) {
    BYTE i;
    for (i = 0; i < n; i++) {
        sim->playerConnected[i]    = TRUE;
        sim->lobbyPlayers[i].isBot = bots;
    }
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

/* ── The cap is a headcount, not a ceiling on the slot ────────────── */

/* A horde seats before anybody joins, so it holds the lowest slots. Those
 * seats are bots and the cap is on people, so the first human to arrive is
 * owed a place above them — under the operator's cap and under a scenario's
 * alike. */
int run_lobby_template_cap_seats_human_above_bots(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              seat;

    sim = ltBareSim();
    UT_ASSERT(sim != NULL);
    ltFillSeats(sim, LT_CAP, true);

    sim->maxPlayers = LT_CAP;
    seat = serverSimFindFreeSlot(sim, false);
    UT_ASSERT_MSG(seat == LT_CAP,
                  "under an operator cap of %d with %d bots below it a human "
                  "seated at %d, expected %d",
                  LT_CAP, LT_CAP, seat, LT_CAP);

    /* The same question of a scenario's own cap, which is where this shows
       up: a lobby seats its horde before anyone has joined. */
    sim->maxPlayers = 0;
    memset(&t, 0, sizeof(t));
    t.maxPlayers = LT_CAP;
    serverSimSetScenarioLobbyTemplate(sim, &t);

    seat = serverSimFindFreeSlot(sim, false);
    UT_ASSERT_MSG(seat == LT_CAP,
                  "under a scenario cap of %d with %d bots below it a human "
                  "seated at %d, expected %d",
                  LT_CAP, LT_CAP, seat, LT_CAP);

    serverSimDestroy(sim);
    return 0;
}

/* The other half of the same rule: once that many people are in, the cap
 * binds, wherever they are sitting. */
int run_lobby_template_cap_refuses_extra_human(void) {
    ServerSim *sim = ltBareSim();

    UT_ASSERT(sim != NULL);
    ltFillSeats(sim, LT_CAP, false);
    sim->maxPlayers = LT_CAP;

    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) < 0,
                  "a %dth person seated under a cap of %d",
                  LT_CAP + 1, LT_CAP);

    serverSimDestroy(sim);
    return 0;
}

/* And a bot is not a person, so a roster full of people has seats for it. */
int run_lobby_template_cap_never_binds_bots(void) {
    ServerSim *sim = ltBareSim();
    int        seat;

    UT_ASSERT(sim != NULL);
    ltFillSeats(sim, LT_CAP, false);
    sim->maxPlayers = LT_CAP;

    seat = serverSimFindFreeSlot(sim, true);
    UT_ASSERT_MSG(seat == LT_CAP,
                  "a bot seated at %d with %d people in and a cap of %d, "
                  "expected %d", seat, LT_CAP, LT_CAP, LT_CAP);

    serverSimDestroy(sim);
    return 0;
}

/* ── A cancelled preview gives the host their lobby back ──────────── */

/* Keep the lobby from starting itself. A scenario seat is ready the moment it
 * is seated, so with the human ready too the next roster change meets an
 * all-ready lobby and starts the round — and a map change is refused outside
 * the lobby, which is where these cases do their previewing. */
static void ltHoldInLobby(ServerSim *sim) {
    sim->lobbyPlayers[0].ready = false;
}

/* Take a team down to n the way a host does, one seat at a time. */
static void ltTrimTo(ServerSim *sim, BYTE team, int n) {
    while (ltSeats(sim, team) > n) {
        int slot = ltFirstSeat(sim, team);
        if (slot < 0) return;
        serverSimRemoveBot(sim, (BYTE)slot);
    }
}

/* Show the host another map. A real preview is a map change like any other,
 * which is why it seats the template again. */
static bool ltPreview(ServerSim *sim, const char *name) {
    BYTE emap[6000] = E_MAP;
    return serverSimReloadCompressedInMemory(sim, emap, 5097, name);
}

int run_lobby_template_cancel_keeps_trim(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;

    UT_ASSERT(ltMakeBrainFile("cancel_keeps_trim"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 6, 6, 1, 1);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 6);

    /* The host trims the six raider seats to three. */
    ltTrimTo(sim, LT_HORDE, 3);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 3);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "the lobby started itself, so there is no preview to make");

    UT_ASSERT(ltPreview(sim, "Preview"));
    UT_ASSERT_MSG(serverSimHasPreviewMap(sim),
                  "the map change left no preview to cancel");

    UT_ASSERT(serverSimRevertPreview(sim));
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 3,
                  "the trim to three came back as %d after a cancel",
                  ltSeats(sim, LT_HORDE));
    UT_ASSERT_MSG(ltSeats(sim, LT_GUARD) == 1,
                  "the untouched team came back at %d, expected 1",
                  ltSeats(sim, LT_GUARD));
    UT_ASSERT_MSG(!serverSimHasPreviewMap(sim),
                  "the cancel left a preview pending");

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* A team emptied on purpose comes back empty, not re-seated — the same edit
 * as a trim, only further. */
int run_lobby_template_cancel_keeps_empty_team(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;

    UT_ASSERT(ltMakeBrainFile("cancel_keeps_empty_team"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 4, 4, 1, 1);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 4);

    ltTrimTo(sim, LT_HORDE, 0);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 0);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    UT_ASSERT(ltPreview(sim, "Preview"));
    UT_ASSERT(serverSimRevertPreview(sim));

    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 0,
                  "an emptied team came back with %d seats after a cancel",
                  ltSeats(sim, LT_HORDE));
    UT_ASSERT_MSG(ltSeats(sim, LT_GUARD) == 1,
                  "the untouched team came back at %d, expected 1",
                  ltSeats(sim, LT_GUARD));

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* One cancel rolls a chain of previews all the way back to where the host
 * started, not to whatever the lobby looked like between two of them. */
int run_lobby_template_cancel_chain_rolls_back(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;

    UT_ASSERT(ltMakeBrainFile("cancel_chain_rolls_back"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 5, 5, 1, 1);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);
    ltTrimTo(sim, LT_HORDE, 2);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 2);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    UT_ASSERT(ltPreview(sim, "Preview one"));
    /* The first preview seated the five again; the host trims to four while
       looking at it, so four is the count in between. */
    ltTrimTo(sim, LT_HORDE, 4);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 4);

    UT_ASSERT(ltPreview(sim, "Preview two"));
    UT_ASSERT(serverSimRevertPreview(sim));

    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 2,
                  "one cancel over two previews gave back %d seats, expected "
                  "the two the host had before the first",
                  ltSeats(sim, LT_HORDE));

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* Committing keeps the previewed map, so its lobby stands and the counts the
 * old map had are gone for good. */
int run_lobby_template_commit_keeps_new_lobby(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;

    UT_ASSERT(ltMakeBrainFile("commit_keeps_new_lobby"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 4, 4, 1, 1);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);
    ltTrimTo(sim, LT_HORDE, 1);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 1);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    UT_ASSERT(ltPreview(sim, "Preview"));
    serverSimCommitPreview(sim);
    UT_ASSERT_MSG(!serverSimHasPreviewMap(sim),
                  "the commit left a preview pending");
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 4,
                  "the committed map's lobby holds %d seats, expected the "
                  "template's four",
                  ltSeats(sim, LT_HORDE));

    /* And the old map's count is not waiting to be applied to a later
       cancel: previewing again and backing out returns the four that are
       there now. */
    UT_ASSERT(ltPreview(sim, "Preview again"));
    UT_ASSERT(serverSimRevertPreview(sim));
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 4,
                  "a cancel after a commit gave back %d seats, expected 4",
                  ltSeats(sim, LT_HORDE));

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── A cancel gives the map's own file back too ───────────────────── */

/* The live map came from a file, which is where a scenario is found beside
 * it. These sims are built from bytes, so name the file the way a file-map
 * commit would have left it. */
#define LT_MAP_PATH "data/maps/Wave Defense.map"

/* Put the sim where a host is when they go looking at another map: the live
 * map came from a file, the template is seated, and one team is trimmed to
 * two. That is the state a cancel has to bring back. */
static void ltSeatOnFileMap(ServerSim *sim, ScnLobbyTemplate *t) {
    SDL_strlcpy(sim->mapFilePath, LT_MAP_PATH, sizeof(sim->mapFilePath));
    ltTemplate(t, 4, 4, 1, 1);
    serverSimSetScenarioLobbyTemplate(sim, t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);
    ltTrimTo(sim, LT_HORDE, 2);
}

/* An uploaded map previews from bytes, which leaves nothing to look beside
 * while it stands. Cancelling has to put the file back, or the map-changed
 * callback looks for a script beside nothing, drops the scenario, and the
 * seats have no template to be restored against. */
int run_lobby_template_cancel_restores_path_inmem(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;

    UT_ASSERT(ltMakeBrainFile("cancel_restores_path_inmem"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltSeatOnFileMap(sim, &t);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 2);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    UT_ASSERT(ltPreview(sim, "Uploaded"));
    UT_ASSERT(serverSimRevertPreview(sim));

    UT_ASSERT_MSG(strcmp(sim->mapFilePath, LT_MAP_PATH) == 0,
                  "the cancel left the map file as '%s', expected '%s'",
                  sim->mapFilePath, LT_MAP_PATH);
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 2,
                  "the trim to two came back as %d",
                  ltSeats(sim, LT_HORDE));

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* A generated map is the other one with no file of its own. It never clears
 * the path while it stands, so here the cancel is what loses it. */
int run_lobby_template_cancel_restores_path_random(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    MapGenConfig     cfg;

    UT_ASSERT(ltMakeBrainFile("cancel_restores_path_random"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltSeatOnFileMap(sim, &t);
    UT_ASSERT(ltSeats(sim, LT_HORDE) == 2);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    cfg = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
    cfg.seed = 20260914u;
    UT_ASSERT(serverSimReloadRandomMap(sim, &cfg));
    UT_ASSERT(serverSimHasPreviewMap(sim));

    UT_ASSERT(serverSimRevertPreview(sim));
    UT_ASSERT_MSG(strcmp(sim->mapFilePath, LT_MAP_PATH) == 0,
                  "the cancel left the map file as '%s', expected '%s'",
                  sim->mapFilePath, LT_MAP_PATH);
    UT_ASSERT_MSG(ltSeats(sim, LT_HORDE) == 2,
                  "the trim to two came back as %d",
                  ltSeats(sim, LT_HORDE));

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── The init table the template's seats carry ────────────────────── */

/* A team's `init` is what its bots are built with, and the seating is where
 * it reaches a seat. A held seat keeps it on the sim, which is where the
 * countdown's warm and a spawn carrying no table of its own both read it; a
 * fielded team's bot was built with it and holds it on its own context. And a
 * seat leaving the roster takes its copy with it, so the slot's next occupant
 * does not inherit a team it was never on.
 *
 * ltTemplate's teams are 0-based over the template's own array: teams[0] is
 * the unfielded team and teams[1] the fielded one. */
int run_lobby_template_seat_carries_init(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              held;
    int              fielded;
    const char      *v;

    UT_ASSERT(ltMakeBrainFile("seat_carries_init"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltTemplate(&t, 2, 2, 1, 1);
    UT_ASSERT(scnTableSet(&t.teams[0].init, "role", "raider"));
    UT_ASSERT(scnTableSet(&t.teams[0].init, "tier", "3"));
    UT_ASSERT(scnTableSet(&t.teams[1].init, "role", "guard"));
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);

    held = ltFirstSeat(sim, LT_HORDE);
    UT_ASSERT_MSG(held >= 0, "the unfielded team seated nothing");
    fielded = ltFirstSeat(sim, LT_GUARD);
    UT_ASSERT_MSG(fielded >= 0, "the fielded team seated nothing");

    /* The held seat, which has no bot behind it yet. */
    UT_ASSERT_MSG(sim->seatInit[held].count == 2,
                  "held seat %d carries %u init pairs, expected the team's 2",
                  held, (unsigned)sim->seatInit[held].count);
    v = scnTableGet(&sim->seatInit[held], "role");
    UT_ASSERT_MSG(v != NULL && strcmp(v, "raider") == 0,
                  "held seat %d has role '%s', expected 'raider'", held,
                  (v != NULL) ? v : "(absent)");
    v = scnTableGet(&sim->seatInit[held], "tier");
    UT_ASSERT_MSG(v != NULL && strcmp(v, "3") == 0,
                  "held seat %d has tier '%s', expected '3'", held,
                  (v != NULL) ? v : "(absent)");
    UT_ASSERT_MSG(!sim->lobbyPlayers[held].fielded,
                  "the unfielded team's seat %d is on the field", held);

    /* The fielded seat, whose bot was built while the seating ran. */
    UT_ASSERT_MSG(sim->seatInit[fielded].count == 1,
                  "fielded seat %d carries %u init pairs, expected the team's "
                  "1", fielded, (unsigned)sim->seatInit[fielded].count);
    UT_ASSERT_MSG(serverSimIsBot(sim, (BYTE)fielded),
                  "seat %d does not read as a bot", fielded);
    UT_ASSERT_MSG(sim->botMgr.bots[fielded].initTable.count == 1,
                  "the bot in seat %d was built with %u init pairs, expected "
                  "the team's 1", fielded,
                  (unsigned)sim->botMgr.bots[fielded].initTable.count);
    v = scnTableGet(&sim->botMgr.bots[fielded].initTable, "role");
    UT_ASSERT_MSG(v != NULL && strcmp(v, "guard") == 0,
                  "the bot in seat %d was built with role '%s', expected "
                  "'guard'", fielded, (v != NULL) ? v : "(absent)");

    /* A seat leaving hands the slot back empty. */
    serverSimRemoveBot(sim, (BYTE)held);
    UT_ASSERT_MSG(!sim->playerConnected[held],
                  "the removal left seat %d in the roster", held);
    UT_ASSERT_MSG(sim->seatInit[held].count == 0,
                  "seat %d left the roster still carrying %u init pairs",
                  held, (unsigned)sim->seatInit[held].count);

    serverSimRemoveBot(sim, (BYTE)fielded);
    UT_ASSERT_MSG(sim->seatInit[fielded].count == 0,
                  "fielded seat %d left the roster still carrying %u init "
                  "pairs", fielded, (unsigned)sim->seatInit[fielded].count);

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}
