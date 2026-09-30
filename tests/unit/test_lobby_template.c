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
 * run_lobby_template_seat_carries_mode  — a team's mode and difficulty reach
 *                                         every seat it makes, held or
 *                                         fielded, and each asks to be
 *                                         published
 * run_lobby_template_mode_unknown_key_kept
 *                                       — a key the brain does not list
 *                                         costs the key, not the seat
 * run_lobby_template_no_mode_leaves_config
 *                                       — naming neither leaves both as the
 *                                         lobby had them
 * run_lobby_template_add_bot_takes_template
 *                                       — Add Bot on a team the template
 *                                         configures takes the template's
 *                                         mode and that team's own last
 *                                         hand-picked level, and a plain
 *                                         team keeps the ordinary rule
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
#define LT_RAIDER 3     /* unfielded, the Survival shape */
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

/* A fixture brain that SHIPS A modes.txt, for the cases that name a mode and
 * a difficulty on a team.
 *
 * It has to be a directory and not the bare file above: brain_list.c reads a
 * brain's modes.txt by the name of the DIRECTORY holding init.lua, under
 * brains/ or Brains/ of the working directory. The two modes and their three
 * levels are written out here rather than borrowed from a shipped brain, so
 * a case says what it expects and an edit to GoalHunter's own modes.txt
 * cannot move it. */
static char ltModesDir[160];
static char ltModesBrain[224];

static bool ltMakeModesBrainWith(const char *tag, const char *header);

static bool ltMakeModesBrain(const char *tag) {
    return ltMakeModesBrainWith(tag, "");
}

/* The same fixture with `header` written above the first section, where a
 * manifest's file-level lines (open_default) go. */
static bool ltMakeModesBrainWith(const char *tag, const char *header) {
    char  path[288];
    FILE *f;

    SDL_snprintf(ltModesDir, sizeof(ltModesDir), "brains/ut_lt_%s", tag);
    SDL_CreateDirectory("brains");
    if (!SDL_CreateDirectory(ltModesDir)) return false;

    SDL_snprintf(ltModesBrain, sizeof(ltModesBrain), "%s/init.lua",
                 ltModesDir);
    f = fopen(ltModesBrain, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);

    SDL_snprintf(path, sizeof(path), "%s/modes.txt", ltModesDir);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(header, f);
    fputs("[default]\n"
          "label = Default\n"
          "levels = easy:Easy:1, medium:Medium:2, hard:Hard:3\n"
          "default = hard\n"
          "\n"
          "[survival]\n"
          "label = Survival\n"
          "levels = easy:Easy:1, medium:Medium:2, hard:Hard:3\n"
          "default = hard\n", f);
    fclose(f);
    return true;
}

static void ltDropModesBrain(void) {
    char path[288];

    SDL_snprintf(path, sizeof(path), "%s/modes.txt", ltModesDir);
    remove(path);
    remove(ltModesBrain);
    SDL_RemovePath(ltModesDir);
    /* And brains/ itself, which ltMakeModesBrain creates when it is not
       already there. SDL_RemovePath removes a file or an EMPTY directory and
       fails on anything else, so this takes back the one this case made and
       leaves a working directory that has brains of its own — a build
       directory, where brains/ is a CMake output — untouched. */
    SDL_RemovePath("brains");
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

/* One unfielded team of `raider` seats and one fielded team of `guard`. */
static void ltTemplate(ScnLobbyTemplate *t, BYTE raider, BYTE raiderMax,
                       BYTE guard, BYTE guardMax) {
    memset(t, 0, sizeof(*t));
    t->numTeams = 2;
    t->teams[0].id      = LT_RAIDER;
    t->teams[0].bots    = raider;
    t->teams[0].maxBots = raiderMax;
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

    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 4,
                  "the unfielded team has %d seats, expected 4",
                  ltSeats(sim, LT_RAIDER));
    UT_ASSERT_MSG(ltSeats(sim, LT_GUARD) == 2,
                  "the fielded team has %d seats, expected 2",
                  ltSeats(sim, LT_GUARD));

    /* The human is untouched and is not one of the scenario's. */
    UT_ASSERT(sim->playerConnected[0]);
    UT_ASSERT_MSG(!sim->lobbyPlayers[0].keepSeat,
                  "the human's seat was taken for the template");

    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || !sim->lobbyPlayers[i].keepSeat) continue;
        if (sim->lobbyPlayers[i].teamNumber == LT_RAIDER) {
            UT_ASSERT_MSG(!sim->lobbyPlayers[i].fielded,
                          "raider seat %d is on the field", i);
            UT_ASSERT_MSG(sim->sim.tanks[i] == NULL,
                          "raider seat %d has a tank", i);
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
    UT_ASSERT_MSG(sim->teams[LT_RAIDER].in_use,
                  "the raiders' team is not in use");
    UT_ASSERT_MSG(sim->teams[LT_GUARD].in_use,
                  "the guard's team is not in use");

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── A template that changed takes the lobby's own bots off ──────── */

int run_lobby_template_map_commit_drops_prior_bots(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              i, bots = 0;
    int              hostBot;

    UT_ASSERT(ltMakeBrainFile("map_commit_drops"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    /* The lobby as a single-player game opens it: the human at 0 and one
       seeded enemy at 1, on a team the script knows nothing about. */
    {
        ServerSimBotConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.brainPath  = ltBrainPath;
        cfg.brainName  = "Bot 1";
        cfg.ai         = aiFull;
        cfg.gameType   = gameOpen;
        cfg.teamNumber = 2;
        UT_ASSERT(serverSimAddBot(sim, 1, &cfg));
    }
    UT_ASSERT(serverSimIsBot(sim, 1));

    /* Then a scripted map is chosen. */
    ltTemplate(&t, 4, 4, 2, 2);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioOnMapChanged(sim, "");

    /* The seeded bot is gone, the human stays, and the roster is the
       human plus the template's six and nothing else. */
    UT_ASSERT(sim->playerConnected[0]);
    UT_ASSERT_MSG(!sim->lobbyPlayers[0].keepSeat,
                  "the human's seat was taken for the template");
    for (i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsBot(sim, (BYTE)i)) continue;
        bots++;
        UT_ASSERT_MSG(sim->lobbyPlayers[i].keepSeat,
                      "bot in slot %d is not one of the template's", i);
        UT_ASSERT_MSG(sim->lobbyPlayers[i].teamNumber == LT_RAIDER ||
                      sim->lobbyPlayers[i].teamNumber == LT_GUARD,
                      "bot in slot %d is on team %d, not the template's",
                      i, (int)sim->lobbyPlayers[i].teamNumber);
    }
    UT_ASSERT_MSG(bots == 6, "%d bots in the lobby, expected the template's 6",
                  bots);
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 7, "roster %u, expected 7",
                  (unsigned)serverSimGetNumPlayers(sim));

    /* The host adds one of their own, on a team the template says nothing
       about. */
    hostBot = serverSimFindFreeSlot(sim, true);
    UT_ASSERT(hostBot >= 0);
    {
        ServerSimBotConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.brainPath  = ltBrainPath;
        cfg.brainName  = "Bot 2";
        cfg.ai         = aiFull;
        cfg.gameType   = gameOpen;
        cfg.teamNumber = 2;
        UT_ASSERT(serverSimAddBot(sim, (BYTE)hostBot, &cfg));
    }
    UT_ASSERT(serverSimIsBot(sim, (BYTE)hostBot));

    /* The selection is decided again and reaches the same template, which is
       what a mods change does. The host's bot stays and the seats are the
       template's same six. */
    serverSimScenarioOnMapChanged(sim, "");

    UT_ASSERT_MSG(serverSimIsBot(sim, (BYTE)hostBot),
                  "the host's own bot was taken off by an unchanged template");
    UT_ASSERT_MSG(!sim->lobbyPlayers[hostBot].keepSeat,
                  "the host's own bot was counted as one of the template's");
    UT_ASSERT_MSG(sim->lobbyPlayers[hostBot].teamNumber == 2,
                  "the host's own bot is on team %d, not the 2 it was added to",
                  (int)sim->lobbyPlayers[hostBot].teamNumber);
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 4,
                  "seated %d of the raiders, expected the template's 4",
                  ltSeats(sim, LT_RAIDER));
    UT_ASSERT_MSG(ltSeats(sim, LT_GUARD) == 2,
                  "seated %d of the guard, expected the template's 2",
                  ltSeats(sim, LT_GUARD));
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 8, "roster %u, expected 8",
                  (unsigned)serverSimGetNumPlayers(sim));

    /* A different template, and it goes with the rest: the lobby the script
       lays out is not the one the bot was added to. */
    ltTemplate(&t, 2, 2, 1, 1);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioOnMapChanged(sim, "");

    UT_ASSERT_MSG(!(serverSimIsBot(sim, (BYTE)hostBot) &&
                    !sim->lobbyPlayers[hostBot].keepSeat),
                  "the host's own bot survived a template change");
    bots = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsBot(sim, (BYTE)i)) continue;
        bots++;
        UT_ASSERT_MSG(sim->lobbyPlayers[i].keepSeat,
                      "bot in slot %d is not one of the template's", i);
    }
    UT_ASSERT_MSG(bots == 3, "%d bots in the lobby, expected the template's 3",
                  bots);
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 4, "roster %u, expected 4",
                  (unsigned)serverSimGetNumPlayers(sim));

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
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 10,
                  "seeded %d of the raiders, expected 10",
                  ltSeats(sim, LT_RAIDER));

    /* The host trims the raiders to six, which is the whole point of seating
       them where a host can see them. */
    while (ltSeats(sim, LT_RAIDER) > 6) {
        slot = ltFirstSeat(sim, LT_RAIDER);
        UT_ASSERT(slot >= 0);
        serverSimRemoveBot(sim, (BYTE)slot);
    }
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 6);

    serverSimScenarioReconcileLobby(sim);
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 6,
                  "the host's trim to six came back as %d — bots is how many "
                  "the engine seeds, not a floor it re-imposes",
                  ltSeats(sim, LT_RAIDER));

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
    while (ltSeats(sim, LT_RAIDER) > 0) {
        slot = ltFirstSeat(sim, LT_RAIDER);
        UT_ASSERT(slot >= 0);
        serverSimRemoveBot(sim, (BYTE)slot);
    }
    serverSimScenarioReconcileLobby(sim);
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 0,
                  "an emptied team came back with %d seats",
                  ltSeats(sim, LT_RAIDER));

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

    slot = ltFirstSeat(sim, LT_RAIDER);
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
        cfg.teamNumber = LT_RAIDER;
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
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == LT_RAIDER,
                  "the seat came back on team %u, expected %u",
                  (unsigned)sim->lobbyPlayers[slot].teamNumber,
                  (unsigned)LT_RAIDER);
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
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 3);

    /* The last human leaves, which is what reaches the reset. */
    serverSimRemovePlayer(sim, 0);

    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0,
                  "the lobby should be humanless");
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 3,
                  "the next joiner opens a lobby with %d of the raiders, "
                  "expected the template's three",
                  ltSeats(sim, LT_RAIDER));

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
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 3);

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
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 3);
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
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 4);
    UT_ASSERT(ltSeats(sim, LT_GUARD) == 1);

    /* A plain map is committed: whoever owns the scenario detaches and
       clears the template, and the seating that follows finds none. */
    serverSimSetScenarioLobbyTemplate(sim, NULL);
    serverSimScenarioSeatLobby(sim);

    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 0,
                  "%d of the previous scenario's raiders survived a plain map",
                  ltSeats(sim, LT_RAIDER));
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

    /* Two humans, and a raider team far larger than that. */
    memset(&t, 0, sizeof(t));
    t.maxPlayers = 2;
    t.numTeams   = 1;
    t.teams[0].id      = LT_RAIDER;
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
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 8,
                  "the raiders seated %d of eight under a human cap of two",
                  ltSeats(sim, LT_RAIDER));
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim, false) < 0,
                  "the human cap stopped binding once bots seated");

    serverSimDestroy(sim);
    ltDropBrainFile();
    return 0;
}

/* ── The cap is a headcount, not a ceiling on the slot ────────────── */

/* The raiders seat before anybody joins, so they hold the lowest slots. Those
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
       up: a lobby seats its raiders before anyone has joined. */
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
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 6);

    /* The host trims the six raider seats to three. */
    ltTrimTo(sim, LT_RAIDER, 3);
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 3);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "the lobby started itself, so there is no preview to make");

    UT_ASSERT(ltPreview(sim, "Preview"));
    UT_ASSERT_MSG(serverSimHasPreviewMap(sim),
                  "the map change left no preview to cancel");

    UT_ASSERT(serverSimRevertPreview(sim));
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 3,
                  "the trim to three came back as %d after a cancel",
                  ltSeats(sim, LT_RAIDER));
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
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 4);

    ltTrimTo(sim, LT_RAIDER, 0);
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 0);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    UT_ASSERT(ltPreview(sim, "Preview"));
    UT_ASSERT(serverSimRevertPreview(sim));

    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 0,
                  "an emptied team came back with %d seats after a cancel",
                  ltSeats(sim, LT_RAIDER));
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
    ltTrimTo(sim, LT_RAIDER, 2);
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 2);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    UT_ASSERT(ltPreview(sim, "Preview one"));
    /* The first preview seated the five again; the host trims to four while
       looking at it, so four is the count in between. */
    ltTrimTo(sim, LT_RAIDER, 4);
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 4);

    UT_ASSERT(ltPreview(sim, "Preview two"));
    UT_ASSERT(serverSimRevertPreview(sim));

    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 2,
                  "one cancel over two previews gave back %d seats, expected "
                  "the two the host had before the first",
                  ltSeats(sim, LT_RAIDER));

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
    ltTrimTo(sim, LT_RAIDER, 1);
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 1);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    UT_ASSERT(ltPreview(sim, "Preview"));
    serverSimCommitPreview(sim);
    UT_ASSERT_MSG(!serverSimHasPreviewMap(sim),
                  "the commit left a preview pending");
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 4,
                  "the committed map's lobby holds %d seats, expected the "
                  "template's four",
                  ltSeats(sim, LT_RAIDER));

    /* And the old map's count is not waiting to be applied to a later
       cancel: previewing again and backing out returns the four that are
       there now. */
    UT_ASSERT(ltPreview(sim, "Preview again"));
    UT_ASSERT(serverSimRevertPreview(sim));
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 4,
                  "a cancel after a commit gave back %d seats, expected 4",
                  ltSeats(sim, LT_RAIDER));

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
    ltTrimTo(sim, LT_RAIDER, 2);
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
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 2);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    UT_ASSERT(ltPreview(sim, "Uploaded"));
    UT_ASSERT(serverSimRevertPreview(sim));

    UT_ASSERT_MSG(strcmp(sim->mapFilePath, LT_MAP_PATH) == 0,
                  "the cancel left the map file as '%s', expected '%s'",
                  sim->mapFilePath, LT_MAP_PATH);
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 2,
                  "the trim to two came back as %d",
                  ltSeats(sim, LT_RAIDER));

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
    UT_ASSERT(ltSeats(sim, LT_RAIDER) == 2);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);

    cfg = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
    cfg.seed = 20260914u;
    UT_ASSERT(serverSimReloadRandomMap(sim, &cfg));
    UT_ASSERT(serverSimHasPreviewMap(sim));

    UT_ASSERT(serverSimRevertPreview(sim));
    UT_ASSERT_MSG(strcmp(sim->mapFilePath, LT_MAP_PATH) == 0,
                  "the cancel left the map file as '%s', expected '%s'",
                  sim->mapFilePath, LT_MAP_PATH);
    UT_ASSERT_MSG(ltSeats(sim, LT_RAIDER) == 2,
                  "the trim to two came back as %d",
                  ltSeats(sim, LT_RAIDER));

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

    held = ltFirstSeat(sim, LT_RAIDER);
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

/* ── The mode and the difficulty a team names ─────────────────────────
 *
 * A team may name the brain mode its bots play in and the level inside it,
 * by the keys the brain's own modes.txt lists. The seating resolves them to
 * the two indices a lobby seat carries and writes them into the seat's
 * config BEFORE anything builds a brain with it — which is where
 * botManagerStageInitArg reads the pair it turns into the brain's mode= and
 * difficulty= tokens.
 *
 * The fixture brain's modes are default (mode 0) and survival (mode 1), each
 * with easy / medium / hard (levels 0 / 1 / 2) and hard as the default.
 *
 * The queued bot-config event is asserted with them. It is the whole of the
 * lobby-row half: nothing else ever publishes a scripted seat's config, and
 * a client that is told nothing goes on showing the zero its own table was
 * created with, which reads as Easy. */

/* One team of held seats and one of fielded ones, both on the fixture brain
 * that ships modes, with the mode and the level this case is about. */
static void ltModesTemplate(ScnLobbyTemplate *t,
                            const char *mode, const char *level) {
    memset(t, 0, sizeof(*t));
    t->numTeams = 2;
    t->teams[0].id      = LT_RAIDER;
    t->teams[0].bots    = 2;
    t->teams[0].maxBots = 2;
    t->teams[0].fielded = false;
    t->teams[1].id      = LT_GUARD;
    t->teams[1].bots    = 1;
    t->teams[1].maxBots = 1;
    t->teams[1].fielded = true;
    SDL_strlcpy(t->teams[0].brain, ltModesBrain, sizeof(t->teams[0].brain));
    SDL_strlcpy(t->teams[1].brain, ltModesBrain, sizeof(t->teams[1].brain));
    if (mode != NULL) {
        SDL_strlcpy(t->teams[0].mode, mode, sizeof(t->teams[0].mode));
        SDL_strlcpy(t->teams[1].mode, mode, sizeof(t->teams[1].mode));
    }
    if (level != NULL) {
        SDL_strlcpy(t->teams[0].difficulty, level,
                    sizeof(t->teams[0].difficulty));
        SDL_strlcpy(t->teams[1].difficulty, level,
                    sizeof(t->teams[1].difficulty));
    }
}

int run_lobby_template_seat_carries_mode(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              held;
    int              fielded;

    UT_ASSERT(ltMakeBrainFile("seat_carries_mode"));
    UT_ASSERT(ltMakeModesBrain("seat_carries_mode"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltModesTemplate(&t, "survival", "medium");
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);

    held = ltFirstSeat(sim, LT_RAIDER);
    UT_ASSERT_MSG(held >= 0, "the held team seated nothing");
    fielded = ltFirstSeat(sim, LT_GUARD);
    UT_ASSERT_MSG(fielded >= 0, "the fielded team seated nothing");

    /* Both halves of the pair, on a seat that holds no bot yet. */
    UT_ASSERT_MSG(sim->botConfigs[held].mode == 1,
                  "held seat %d is in mode %u, expected survival (1)",
                  held, (unsigned)sim->botConfigs[held].mode);
    UT_ASSERT_MSG(sim->botConfigs[held].difficulty == 1,
                  "held seat %d is at level %u, expected medium (1)",
                  held, (unsigned)sim->botConfigs[held].difficulty);

    /* And on one whose bot was built while the seating ran. */
    UT_ASSERT_MSG(sim->botConfigs[fielded].mode == 1,
                  "fielded seat %d is in mode %u, expected survival (1)",
                  fielded, (unsigned)sim->botConfigs[fielded].mode);
    UT_ASSERT_MSG(sim->botConfigs[fielded].difficulty == 1,
                  "fielded seat %d is at level %u, expected medium (1)",
                  fielded, (unsigned)sim->botConfigs[fielded].difficulty);

    /* Every seat the template made asks to be published. */
    UT_ASSERT_MSG((sim->botConfigPublishPending & (1u << held)) != 0,
                  "held seat %d queued no bot-config event", held);
    UT_ASSERT_MSG((sim->botConfigPublishPending & (1u << fielded)) != 0,
                  "fielded seat %d queued no bot-config event", fielded);

    serverSimDestroy(sim);
    ltDropModesBrain();
    ltDropBrainFile();
    return 0;
}

/* A mode key the brain does not list costs the key and not the seat: the
 * seating leaves the config as the lobby had it and says so in the log, and
 * -validate is where the author is told. The same for a level key. */
int run_lobby_template_mode_unknown_key_kept(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              held;

    UT_ASSERT(ltMakeBrainFile("mode_unknown"));
    UT_ASSERT(ltMakeModesBrain("mode_unknown"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltModesTemplate(&t, "nosuchmode", "medium");
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);

    held = ltFirstSeat(sim, LT_RAIDER);
    UT_ASSERT_MSG(held >= 0, "the held team seated nothing");
    UT_ASSERT_MSG(sim->botConfigs[held].mode == 0,
                  "a mode nothing lists moved seat %d to mode %u",
                  held, (unsigned)sim->botConfigs[held].mode);
    UT_ASSERT_MSG(sim->botConfigs[held].difficulty == BOT_DIFFICULTY_HARD,
                  "a mode nothing lists moved seat %d to level %u, expected "
                  "the lobby's Hard", held,
                  (unsigned)sim->botConfigs[held].difficulty);

    serverSimDestroy(sim);
    ltDropModesBrain();
    ltDropBrainFile();

    /* And the level key on its own, against a mode that does exist. */
    UT_ASSERT(ltMakeBrainFile("level_unknown"));
    UT_ASSERT(ltMakeModesBrain("level_unknown"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltModesTemplate(&t, "survival", "nosuchlevel");
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);

    held = ltFirstSeat(sim, LT_RAIDER);
    UT_ASSERT_MSG(held >= 0, "the held team seated nothing");
    UT_ASSERT_MSG(sim->botConfigs[held].mode == 0,
                  "a level nothing lists still moved seat %d to mode %u",
                  held, (unsigned)sim->botConfigs[held].mode);
    UT_ASSERT_MSG(sim->botConfigs[held].difficulty == BOT_DIFFICULTY_HARD,
                  "a level nothing lists moved seat %d to level %u",
                  held, (unsigned)sim->botConfigs[held].difficulty);

    serverSimDestroy(sim);
    ltDropModesBrain();
    ltDropBrainFile();
    return 0;
}

/* A template that names neither leaves both exactly as the lobby had them —
 * which is every scenario written before the two fields existed. The event
 * is queued all the same, because a seat nobody publishes for is the defect
 * the two fields were added next to. */
int run_lobby_template_no_mode_leaves_config(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              held;
    int              fielded;

    UT_ASSERT(ltMakeBrainFile("no_mode"));
    UT_ASSERT(ltMakeModesBrain("no_mode"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);

    ltModesTemplate(&t, NULL, NULL);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);

    held = ltFirstSeat(sim, LT_RAIDER);
    UT_ASSERT_MSG(held >= 0, "the held team seated nothing");
    fielded = ltFirstSeat(sim, LT_GUARD);
    UT_ASSERT_MSG(fielded >= 0, "the fielded team seated nothing");

    UT_ASSERT_MSG(sim->botConfigs[held].mode == 0,
                  "seat %d moved to mode %u with no mode named",
                  held, (unsigned)sim->botConfigs[held].mode);
    UT_ASSERT_MSG(sim->botConfigs[held].difficulty == BOT_DIFFICULTY_HARD,
                  "seat %d moved to level %u with no difficulty named",
                  held, (unsigned)sim->botConfigs[held].difficulty);
    UT_ASSERT_MSG(sim->botConfigs[fielded].mode == 0,
                  "fielded seat %d moved to mode %u with no mode named",
                  fielded, (unsigned)sim->botConfigs[fielded].mode);
    UT_ASSERT_MSG(sim->botConfigs[fielded].difficulty == BOT_DIFFICULTY_HARD,
                  "fielded seat %d moved to level %u with no difficulty "
                  "named", fielded,
                  (unsigned)sim->botConfigs[fielded].difficulty);

    UT_ASSERT_MSG((sim->botConfigPublishPending & (1u << held)) != 0,
                  "held seat %d queued no bot-config event", held);
    UT_ASSERT_MSG((sim->botConfigPublishPending & (1u << fielded)) != 0,
                  "fielded seat %d queued no bot-config event", fielded);

    serverSimDestroy(sim);
    ltDropModesBrain();
    ltDropBrainFile();
    return 0;
}

/* A seat the template makes starts from a NEW seat's config, whatever the
 * slot's previous bot had: botConfigs is not cleared when a bot leaves, so a
 * seat that read it would inherit a removed bot's mode. And the base is the
 * one Add Bot resolves for the team, so on an Open game a seeded seat starts
 * in the brain's open_default mode as an added bot does. */
int run_lobby_template_seat_new_seat_base(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              held;
    int              fielded;
    int              i;

    /* 1. Stale configs on every slot; the fixture has no open_default. */
    UT_ASSERT(ltMakeBrainFile("new_seat_base"));
    UT_ASSERT(ltMakeModesBrain("new_seat_base"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);
    for (i = 0; i < MAX_TANKS; i++) {
        sim->botConfigs[i].mode       = 1;
        sim->botConfigs[i].difficulty = 0;
    }

    ltModesTemplate(&t, NULL, NULL);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);

    held = ltFirstSeat(sim, LT_RAIDER);
    UT_ASSERT_MSG(held >= 0, "the held team seated nothing");
    fielded = ltFirstSeat(sim, LT_GUARD);
    UT_ASSERT_MSG(fielded >= 0, "the fielded team seated nothing");
    UT_ASSERT_MSG(sim->botConfigs[held].mode == 0 &&
                  sim->botConfigs[held].difficulty == BOT_DIFFICULTY_HARD,
                  "held seat %d kept the slot's stale config %u/%u",
                  held, (unsigned)sim->botConfigs[held].mode,
                  (unsigned)sim->botConfigs[held].difficulty);
    UT_ASSERT_MSG(sim->botConfigs[fielded].mode == 0 &&
                  sim->botConfigs[fielded].difficulty == BOT_DIFFICULTY_HARD,
                  "fielded seat %d kept the slot's stale config %u/%u",
                  fielded, (unsigned)sim->botConfigs[fielded].mode,
                  (unsigned)sim->botConfigs[fielded].difficulty);

    serverSimDestroy(sim);
    ltDropModesBrain();
    ltDropBrainFile();

    /* 2. A brain whose open_default is its second mode, on an Open game:
          the seeded seats start there, at the lobby's Hard. */
    UT_ASSERT(ltMakeBrainFile("new_seat_open"));
    UT_ASSERT(ltMakeModesBrainWith("new_seat_open",
                                   "open_default = survival\n\n"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimGetGameType(sim) == gameOpen);

    ltModesTemplate(&t, NULL, NULL);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);

    held = ltFirstSeat(sim, LT_RAIDER);
    UT_ASSERT_MSG(held >= 0, "the held team seated nothing");
    fielded = ltFirstSeat(sim, LT_GUARD);
    UT_ASSERT_MSG(fielded >= 0, "the fielded team seated nothing");
    UT_ASSERT_MSG(sim->botConfigs[held].mode == 1 &&
                  sim->botConfigs[held].difficulty == BOT_DIFFICULTY_HARD,
                  "held seat %d on an Open game is %u/%u, expected the "
                  "open_default mode (1) at Hard", held,
                  (unsigned)sim->botConfigs[held].mode,
                  (unsigned)sim->botConfigs[held].difficulty);
    UT_ASSERT_MSG(sim->botConfigs[fielded].mode == 1 &&
                  sim->botConfigs[fielded].difficulty == BOT_DIFFICULTY_HARD,
                  "fielded seat %d on an Open game is %u/%u, expected the "
                  "open_default mode (1) at Hard", fielded,
                  (unsigned)sim->botConfigs[fielded].mode,
                  (unsigned)sim->botConfigs[fielded].difficulty);

    serverSimDestroy(sim);
    ltDropModesBrain();
    ltDropBrainFile();
    return 0;
}

/* ── Add Bot on a team the template configures ─────────────────────────
 *
 * The owner's rule: "when someone hits Add Bot on horde, it needs to go
 * through the scenario somehow to know to put them to Survival and whatever
 * the latest difficulty was that was manually set by a human, or default to
 * Hard."
 *
 * So a seat Add Bot makes on a templated team takes the template's MODE, and
 * for the level takes the last difficulty a person set on a seat of THAT
 * team, else the template's own, else the mode's default, else Hard. A team
 * the template says nothing about keeps the ordinary lobby rule, where the
 * last pair a person picked by hand sets the mode as well as the level.
 *
 * Both roads in resolve through serverSimApplyNewBotDefaults, so this is
 * what the host's Add Bot over the wire and single player's own both do.
 */

/* One templated team and one plain one, both on the modes fixture brain. */
static void ltAddBotTemplate(ScnLobbyTemplate *t,
                             const char *mode, const char *level) {
    memset(t, 0, sizeof(*t));
    t->numTeams = 2;
    /* The templated team — Survival's horde. No seats of its own: this case
       is about the seats Add Bot makes, not the ones the seating makes. */
    t->teams[0].id      = LT_GUARD;
    t->teams[0].bots    = 0;
    t->teams[0].maxBots = 8;
    t->teams[0].fielded = true;
    SDL_strlcpy(t->teams[0].brain, ltModesBrain, sizeof(t->teams[0].brain));
    if (mode != NULL) {
        SDL_strlcpy(t->teams[0].mode, mode, sizeof(t->teams[0].mode));
    }
    if (level != NULL) {
        SDL_strlcpy(t->teams[0].difficulty, level,
                    sizeof(t->teams[0].difficulty));
    }
    /* The plain team — Survival's defenders. Listed, and naming neither key,
       which is the same as not being listed at all as far as this rule is
       concerned. */
    t->teams[1].id      = LT_RAIDER;
    t->teams[1].bots    = 0;
    t->teams[1].maxBots = 8;
    t->teams[1].fielded = true;
    SDL_strlcpy(t->teams[1].brain, ltModesBrain, sizeof(t->teams[1].brain));
}

/* The Add Bot button, server side: a bot in the first free seat, on the team
   asked for, then the mode and difficulty every new lobby bot gets. The same
   two calls, in the same order, as the CMD_LOBBY_ADD_BOT arm of
   server_command_dispatch.c. Returns the seat. */
static int ltAddBot(ServerSim *sim, BYTE team) {
    int slot = serverSimFindFreeSlot(sim, true);
    if (slot < 0) return -1;
    if (!serverSimCreateBot(sim, (BYTE)slot, serverSimGetBotBrainPath(sim),
                            "Added", (aiType)serverSimGetBotAiType(sim),
                            gameOpen, sim->sim.hiddenMines, team, NULL)) {
        return -1;
    }
    serverSimApplyNewBotDefaults(sim, (BYTE)slot, (int)team,
                                 serverSimGetBotBrainPath(sim), TRUE);
    return slot;
}

/* A person choosing a mode and a difficulty off the lobby's dropdown. The
   same two calls, in the same order, as the CMD_LOBBY_BOT_CONFIG arm of
   server_command_dispatch.c — the write, then the remember, and the remember
   only because the pair really changed. */
static void ltHostPicks(ServerSim *sim, int slot, uint8_t mode, uint8_t lvl) {
    serverSimSetBotConfig(sim, (BYTE)slot, mode, lvl, 0, NULL);
    serverSimRememberManualBotPick(sim, (BYTE)slot);
}

int run_lobby_template_add_bot_takes_template(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              horde1;
    int              horde2;
    int              horde3;
    int              plain1;
    int              plain2;

    UT_ASSERT(ltMakeBrainFile("add_bot_template"));
    UT_ASSERT(ltMakeModesBrain("add_bot_template"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);
    /* The brain Add Bot runs. ltLobbySim names the bare fixture file, which
       ships no modes.txt and so has no mode to resolve against. */
    serverSimSetBotBrainPath(sim, ltModesBrain);

    ltAddBotTemplate(&t, "survival", "hard");
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);

    /* (a) Nobody has picked anything: the template's own pair. */
    horde1 = ltAddBot(sim, LT_GUARD);
    UT_ASSERT_MSG(horde1 >= 0, "Add Bot on the templated team was refused");
    UT_ASSERT_MSG(sim->botConfigs[horde1].mode == 1,
                  "the added horde bot in seat %d is in mode %u, expected "
                  "survival (1)", horde1,
                  (unsigned)sim->botConfigs[horde1].mode);
    UT_ASSERT_MSG(sim->botConfigs[horde1].difficulty == BOT_DIFFICULTY_HARD,
                  "the added horde bot in seat %d is at level %u, expected "
                  "hard (%d)", horde1,
                  (unsigned)sim->botConfigs[horde1].difficulty,
                  BOT_DIFFICULTY_HARD);

    /* And the plain team is untouched by any of it: the lobby's own default,
       which is mode 0 at Hard. */
    plain1 = ltAddBot(sim, LT_RAIDER);
    UT_ASSERT_MSG(plain1 >= 0, "Add Bot on the plain team was refused");
    UT_ASSERT_MSG(sim->botConfigs[plain1].mode == 0,
                  "the added plain bot in seat %d is in mode %u, expected the "
                  "lobby's own (0)", plain1,
                  (unsigned)sim->botConfigs[plain1].mode);
    UT_ASSERT_MSG(sim->botConfigs[plain1].difficulty == BOT_DIFFICULTY_HARD,
                  "the added plain bot in seat %d is at level %u, expected "
                  "hard (%d)", plain1,
                  (unsigned)sim->botConfigs[plain1].difficulty,
                  BOT_DIFFICULTY_HARD);

    /* (b) A person turns one HORDE seat down to easy. The next Add Bot on
           that team is survival at easy: the mode is still the template's,
           and the level is the one a person chose. */
    ltHostPicks(sim, horde1, 1 /* survival */, BOT_DIFFICULTY_EASY);
    horde2 = ltAddBot(sim, LT_GUARD);
    UT_ASSERT_MSG(horde2 >= 0, "the second Add Bot on the horde was refused");
    UT_ASSERT_MSG(sim->botConfigs[horde2].mode == 1,
                  "the second horde bot, seat %d, is in mode %u, expected "
                  "survival (1)", horde2,
                  (unsigned)sim->botConfigs[horde2].mode);
    UT_ASSERT_MSG(sim->botConfigs[horde2].difficulty == BOT_DIFFICULTY_EASY,
                  "the second horde bot, seat %d, is at level %u, expected "
                  "easy (%d)", horde2,
                  (unsigned)sim->botConfigs[horde2].difficulty,
                  BOT_DIFFICULTY_EASY);

    /* (c) A person now picks a pair on the PLAIN team — mode 0 at medium.
           The plain team's next bot takes both, which is the rule that team
           has always followed. */
    ltHostPicks(sim, plain1, 0 /* default */, BOT_DIFFICULTY_MEDIUM);
    plain2 = ltAddBot(sim, LT_RAIDER);
    UT_ASSERT_MSG(plain2 >= 0,
                  "the second Add Bot on the plain team was refused");
    UT_ASSERT_MSG(sim->botConfigs[plain2].mode == 0,
                  "the second plain bot, seat %d, is in mode %u, expected the "
                  "host's own (0)", plain2,
                  (unsigned)sim->botConfigs[plain2].mode);
    UT_ASSERT_MSG(sim->botConfigs[plain2].difficulty == BOT_DIFFICULTY_MEDIUM,
                  "the second plain bot, seat %d, is at level %u, expected "
                  "medium (%d)", plain2,
                  (unsigned)sim->botConfigs[plain2].difficulty,
                  BOT_DIFFICULTY_MEDIUM);

    /* And the horde has not heard a word of it. This is the half a single
       remembered pair gets wrong: a difficulty chosen for the defenders
       would walk onto the horde, and mode 0 with it. */
    horde3 = ltAddBot(sim, LT_GUARD);
    UT_ASSERT_MSG(horde3 >= 0, "the third Add Bot on the horde was refused");
    UT_ASSERT_MSG(sim->botConfigs[horde3].mode == 1,
                  "a pick made on another team moved the horde bot in seat %d "
                  "to mode %u", horde3,
                  (unsigned)sim->botConfigs[horde3].mode);
    UT_ASSERT_MSG(sim->botConfigs[horde3].difficulty == BOT_DIFFICULTY_EASY,
                  "a pick made on another team moved the horde bot in seat %d "
                  "to level %u, expected easy (%d)", horde3,
                  (unsigned)sim->botConfigs[horde3].difficulty,
                  BOT_DIFFICULTY_EASY);

    serverSimDestroy(sim);
    ltDropModesBrain();
    ltDropBrainFile();

    /* (d) A team naming a mode and NO difficulty lands on that mode's own
           default level, which for this fixture is hard — the "or default to
           Hard" half of the rule, arrived at without anything saying hard. */
    UT_ASSERT(ltMakeBrainFile("add_bot_mode_only"));
    UT_ASSERT(ltMakeModesBrain("add_bot_mode_only"));
    ut_brain_stub_arm(true);
    sim = ltLobbySim();
    UT_ASSERT(sim != NULL);
    serverSimSetBotBrainPath(sim, ltModesBrain);

    ltAddBotTemplate(&t, "survival", NULL);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    ltHoldInLobby(sim);

    horde1 = ltAddBot(sim, LT_GUARD);
    UT_ASSERT_MSG(horde1 >= 0, "Add Bot on the mode-only team was refused");
    UT_ASSERT_MSG(sim->botConfigs[horde1].mode == 1,
                  "seat %d is in mode %u, expected survival (1)", horde1,
                  (unsigned)sim->botConfigs[horde1].mode);
    UT_ASSERT_MSG(sim->botConfigs[horde1].difficulty == BOT_DIFFICULTY_HARD,
                  "seat %d is at level %u, expected survival's own default, "
                  "hard (%d)", horde1,
                  (unsigned)sim->botConfigs[horde1].difficulty,
                  BOT_DIFFICULTY_HARD);

    serverSimDestroy(sim);
    ltDropModesBrain();
    ltDropBrainFile();
    return 0;
}
