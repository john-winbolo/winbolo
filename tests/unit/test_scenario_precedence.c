/*
 * Which scenario a lobby plays when a map and a mod both have a claim, and
 * where the answer is applied.
 *
 * One function decides: a mod the host picked wins, otherwise the committed
 * map's own script, otherwise none. Selecting none is not "play nothing" —
 * it is the second rule, so a bound map picks its own scenario back up.
 *
 * These cases drive the real path end to end rather than setting a template
 * by hand: a scratch scenarios directory with real files in it, the host's
 * own directory lister registered on the sim, and the pick made by applying
 * CMD_LOBBY_SET_SCENARIO. What they prove is that the decision, the seating
 * and the lobby's own settings agree afterwards.
 *
 * A map is named but never written. A loose X.scenario.lua beside it is the
 * first thing the attach looks for and the only thing these cases need, so
 * the .map file itself is not made; a map path with no script beside it is
 * the plain map.
 *
 * run_scenario_precedence_mod_over_map    — a mod beats a bound map's own
 * run_scenario_precedence_none_restores_map
 *                                         — selecting none hands the bound
 *                                           map its own scenario back
 * run_scenario_precedence_plain_map_keeps_mod
 *                                         — committing a plain map leaves
 *                                           the mod playing
 * run_scenario_precedence_template_seats  — a mod that seats bots seats
 *                                           them, and they survive a return
 *                                           to the lobby
 * run_scenario_precedence_reset_reapplies — the last human leaving and
 *                                           rejoining finds the mod still
 *                                           selected and the game type
 *                                           agreeing with it
 * run_scenario_precedence_reload_reseats  — a reload after the lobby block
 *                                           changed re-seats
 * run_scenario_precedence_no_game_plays_strict
 *                                         — a round from a mod that named no
 *                                           game hands a tank the strict
 *                                           amounts, on a lobby that was open
 * run_scenario_precedence_open_game_plays_open
 *                                         — and one that named open hands it
 *                                           the open amounts
 * run_scenario_precedence_reload_refuses_bound
 *                                         — a mod edited on disk to say it is
 *                                           bound is refused at the reload,
 *                                           and what was playing keeps playing
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_SET_SCENARIO */
#include "control_event.h"         /* LobbyScenarioSource */
#include "server_sim.h"
#include "server_sim_internal.h"   /* scenarioIdentity, lobbyPlayers, the
                                    * settings snapshot the reset restores */
#include "server_sim_lifecycle.h"  /* SetLobbyEnabled, the template paths,
                                    * serverSimStartGame */
#include "server_sim_scenario.h"   /* serverSimScenarioReload */
#include "game_sim.h"             /* gameTypeGet, scenarioBaseGame */
#include "tank.h"                 /* tankGetShells — what the round paid out */
#include "scenario_host.h"
#include "everard_map.h"
#include "threads.h"
#include "test_harness.h"

/* ── The scenarios directory ──────────────────────────────────────── */

#define SP_MAX_FILES 8

static char spDir[256];
static char spFiles[SP_MAX_FILES][128];
static int  spFileCount;

/* The maps a case commits, named from its tag like its directory. ctest runs
   every case as its own process and several at once, so a name two cases
   shared would have one deleting the other's map script mid-read. */
static char spBoundMap[256];
static char spPlainMap[256];

static bool spMakeDir(const char *tag) {
    snprintf(spDir, sizeof(spDir), "wbtest_scn_prec_%s", tag);
    snprintf(spBoundMap, sizeof(spBoundMap), "wbtest_scn_prec_%s_bound.map", tag);
    snprintf(spPlainMap, sizeof(spPlainMap), "wbtest_scn_prec_%s_plain.map", tag);
    spFileCount = 0;
    /* A directory left over from a run that was killed is not a failure: the
       files below are written over whatever is in it, and the cleanup at the
       end removes what this case put there. */
    (void)SDL_RemovePath(spDir);
    return SDL_CreateDirectory(spDir);
}

/* One scenario in the directory. Remembered so the cleanup can remove it;
   written over in place when a case means to change it, which is what the
   reload case does. */
static bool spWriteMod(const char *name, const char *text) {
    char  path[512];
    FILE *f;
    int   i;
    bool  seen = false;

    for (i = 0; i < spFileCount; i++) {
        if (strcmp(spFiles[i], name) == 0) seen = true;
    }
    if (!seen) {
        if (spFileCount >= SP_MAX_FILES) return false;
        snprintf(spFiles[spFileCount], sizeof(spFiles[0]), "%s", name);
        spFileCount++;
    }
    snprintf(path, sizeof(path), "%s/%s", spDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void spDropDir(void) {
    char path[512];
    int  i;

    for (i = 0; i < spFileCount; i++) {
        snprintf(path, sizeof(path), "%s/%s", spDir, spFiles[i]);
        remove(path);
    }
    spFileCount = 0;
    SDL_RemovePath(spDir);
}

/* ── The map's own script ─────────────────────────────────────────── */

static void spScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

static bool spPutMapScript(const char *mapPath, const char *text) {
    char  path[512];
    FILE *f;

    spScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void spDropMapScript(const char *mapPath) {
    char path[512];
    spScriptFor(mapPath, path, sizeof(path));
    remove(path);
}

/* ── The scenarios these cases pick between ───────────────────────── */

/* A mod that changes a rule and nothing else: no game type, no teams. What
 * it proves is the manifest with no game in it — the round plays strict
 * tournament, because an author who wants an open round names one. */
static const char kSpRulesOnly[] =
    "scenario = {\n"
    "  name = \"Fast Reload\",\n"
    "  description = \"Shells come back quicker\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "}\n";

/* The same mod with the game it plays named. Beside the one above it is what
 * tells the two answers apart: a declared "open" is an open round, and only a
 * mod that declared nothing falls to strict. */
static const char kSpRulesOnlyOpen[] =
    "scenario = {\n"
    "  name = \"Fast Reload Open\",\n"
    "  description = \"Shells come back quicker\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  game = \"open\",\n"
    "}\n";

/* A mod with a lobby of its own: three held seats on team 3, and a game type
 * it does declare, so the lobby moves onto the scenario's. */
static const char kSpWaves[] =
    "scenario = {\n"
    "  name = \"Waves\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  game = \"tournament\",\n"
    "  lobby = {\n"
    "    teams = { { id = 3, bots = 3, max_bots = 3, fielded = false } },\n"
    "  },\n"
    "}\n";

/* And the same mod edited on disk to claim it belongs to a map. A mod plays
 * over whichever map is committed, so this is a table the attach refuses and
 * the reload has to refuse too. */
static const char kSpWavesBound[] =
    "scenario = {\n"
    "  name = \"Waves\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "  lobby = {\n"
    "    teams = { { id = 3, bots = 3, max_bots = 3, fielded = false } },\n"
    "  },\n"
    "}\n";

/* The same mod after a host edited its lobby block: five seats, not three. */
static const char kSpWavesWider[] =
    "scenario = {\n"
    "  name = \"Waves\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  game = \"tournament\",\n"
    "  lobby = {\n"
    "    teams = { { id = 3, bots = 5, max_bots = 5, fielded = false } },\n"
    "  },\n"
    "}\n";

/* A map that carries its own scenario: two held seats on team 2. */
static const char kSpBound[] =
    "scenario = {\n"
    "  name = \"Survival\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  lobby = {\n"
    "    teams = { { id = 2, bots = 2, max_bots = 2, fielded = false } },\n"
    "  },\n"
    "}\n";

/* ── The sim ──────────────────────────────────────────────────────── */

/* The scenario the decision attached, kept where the callback can reach it.
   One per case, which is one per process at a time: the cases run in
   sequence. */
static ScenarioHost *spSlot;

/* What the operator configured, captured the way serverSimApplyInstanceConfig
 * captures it at startup. serverSimResetLobbyToDefaults restores from this,
 * so without it the reset leaves the live settings alone and the case that
 * pins the game type would prove nothing. Called before any scenario is
 * attached, so what it holds is the operator's own type. */
static void spSnapshotDefaults(ServerSim *sim) {
    BYTE i;

    sim->originalLobbySettings.valid               = true;
    sim->originalLobbySettings.gameType            = gameTypeGet(&sim->sim.game);
    sim->originalLobbySettings.hiddenMines         = sim->sim.hiddenMines ? true : false;
    sim->originalLobbySettings.botAiType           = sim->botAiType;
    sim->originalLobbySettings.aiPolicy            = sim->aiPolicy;
    sim->originalLobbySettings.timeLimit           = sim->timeLimit;
    sim->originalLobbySettings.timeMinutes         = sim->timeMinutes;
    sim->originalLobbySettings.gameLength          = sim->gameLength;
    sim->originalLobbySettings.openHost            = sim->openHost;
    sim->originalLobbySettings.autoLockOnGameStart = sim->autoLockOnGameStart;
    sim->originalLobbySettings.ranked              = sim->ranked;
    sim->originalLobbySettings.serverLocks         = sim->serverLocks;
    for (i = 0; i < VIEW_CATEGORY_COUNT; i++) {
        sim->originalLobbySettings.viewPolicy[i]    = sim->viewPolicy[i];
        sim->originalLobbySettings.viewDecaySecs[i] = sim->viewDecaySecs[i];
    }
    sim->originalLobbySettings.classicMode    = sim->classicMode;
    sim->originalLobbySettings.alliesInTrees  = sim->alliesInTrees;
    sim->originalLobbySettings.overviewWindow = sim->overviewWindow;
    sim->originalLobbySettings.lineOfSight    = sim->lineOfSight;
    sim->originalLobbySettings.smartPingsOff  = sim->smartPingsOff;
}

/* A lobby with the host in slot 0, the scratch directory as its scenarios
 * directory, the host's real lister on it so a pick validates against what
 * is actually in there, and the decision registered. */
static ServerSim *spSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetBotAiType(sim, aiFull);
    spSnapshotDefaults(sim);

    /* The reload path seats only from the lobby, so every case starts in
       one. A fresh sim is already there; this is what says so. */
    serverSimSetState(sim, serverStateLobby);

    spSlot = NULL;
    serverSimSetScenarioDir(sim, spDir);
    scenarioHostRegisterScenarioLister(sim);
    scenarioHostFollowMap(sim, &spSlot);
    return sim;
}

static void spDestroy(ServerSim *sim) {
    /* The host holds registrations on the sim and a subscriber slot in it,
       so it goes before the sim does. */
    scenarioHostDetach(spSlot);
    spSlot = NULL;
    serverSimDestroy(sim);
}

/* A map commit, which is what the sim does after loading the new map. The
 * path goes on the sim the way the real load puts it there: a pick made
 * later reads it back to weigh the mod against, so a fixture that left it
 * empty would find no committed map to decide about. */
static void spCommit(ServerSim *sim, const char *mapPath) {
    SDL_strlcpy(sim->mapFilePath, mapPath, sizeof(sim->mapFilePath));
    serverSimScenarioOnMapChanged(sim, mapPath);
    serverSimScenarioApplyLobbyRules(sim);
}

/* The host picking a scenario, through the command the lobby sends. "" is
 * the pick that selects none. */
static CmdResult spSelect(ServerSim *sim, const char *file) {
    ClientCommand cmd;
    CmdResult     r;
    size_t        n = strlen(file);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_SET_SCENARIO;
    cmd.cmdSeq = 1;
    cmd.u.lobbySetScenario.relPathLen = (uint8_t)n;
    if (n > 0) memcpy(cmd.u.lobbySetScenario.relPath, file, n);
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Seats the scenario put on one team. */
static int spSeats(const ServerSim *sim, BYTE team) {
    int n = 0, i;

    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].keepSeat &&
            sim->lobbyPlayers[i].teamNumber == team) {
            n++;
        }
    }
    return n;
}

static const char *spName(const ServerSim *sim) {
    return sim->scenarioIdentity.name;
}

static const char *spFile(const ServerSim *sim) {
    return sim->scenarioIdentity.fileName;
}

static LobbyScenarioSource spSource(const ServerSim *sim) {
    return sim->scenarioIdentity.source;
}

/* ── A mod beats a bound map's own scenario ───────────────────────── */

int run_scenario_precedence_mod_over_map(void) {
    ServerSim *sim;

    UT_ASSERT(spMakeDir("mod_over_map"));
    UT_ASSERT(spWriteMod("fastreload.lua", kSpRulesOnly));
    UT_ASSERT(spPutMapScript(spBoundMap, kSpBound));
    sim = spSim();
    UT_ASSERT(sim != NULL);

    /* The bound map on its own plays its own scenario. */
    spCommit(sim, spBoundMap);
    UT_ASSERT_MSG(spSource(sim) == lobbyScenarioMap,
                  "a bound map alone read source %d, wanted map (%d)",
                  (int)spSource(sim), (int)lobbyScenarioMap);
    UT_ASSERT_MSG(strcmp(spName(sim), "Survival") == 0,
                  "the bound map's scenario read back as \"%s\"", spName(sim));

    /* And the mod takes its place: the host picked it, so the host decided. */
    UT_ASSERT_MSG(spSelect(sim, "fastreload.lua") == CMD_OK,
                  "the mod was refused");
    UT_ASSERT_MSG(spSource(sim) == lobbyScenarioMod,
                  "with a mod picked the source read %d, wanted mod (%d)",
                  (int)spSource(sim), (int)lobbyScenarioMod);
    UT_ASSERT_MSG(strcmp(spName(sim), "Fast Reload") == 0,
                  "the mod's name read back as \"%s\"", spName(sim));
    UT_ASSERT_MSG(strcmp(spFile(sim), "fastreload.lua") == 0,
                  "the file name read back as \"%s\", wanted the mod's",
                  spFile(sim));
    /* The map's own seats went with it: the scenario playing is the mod's,
       and a mod that asks for no teams asks for none. */
    UT_ASSERT_MSG(spSeats(sim, 2) == 0,
                  "the bound map's %d seats outlived its scenario",
                  spSeats(sim, 2));
    /* A manifest with no game in it declares no base game, so the template
       carries 0. The lobby still moves to gameScripted — that is the rules
       being in force — and 0 is what gameTypeResolve reads as strict
       tournament when the round is played. */
    UT_ASSERT_MSG(sim->sim.scenarioBaseGame == (gameType)0,
                  "a manifest naming no game set the base game to %d",
                  (int)sim->sim.scenarioBaseGame);

    spDestroy(sim);
    spDropMapScript(spBoundMap);
    spDropDir();
    return 0;
}

/* ── Selecting none hands the map its own back ────────────────────── */

int run_scenario_precedence_none_restores_map(void) {
    ServerSim *sim;

    UT_ASSERT(spMakeDir("none_restores_map"));
    UT_ASSERT(spWriteMod("fastreload.lua", kSpRulesOnly));
    UT_ASSERT(spPutMapScript(spBoundMap, kSpBound));
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, spBoundMap);
    UT_ASSERT(spSelect(sim, "fastreload.lua") == CMD_OK);
    UT_ASSERT(spSource(sim) == lobbyScenarioMod);

    /* Selecting none is not "play nothing": it is the second rule, so the
       bound map's own scenario is what plays. */
    UT_ASSERT_MSG(spSelect(sim, "") == CMD_OK, "selecting none was refused");
    UT_ASSERT_MSG(spSource(sim) == lobbyScenarioMap,
                  "after selecting none the source read %d, wanted map (%d)",
                  (int)spSource(sim), (int)lobbyScenarioMap);
    UT_ASSERT_MSG(strcmp(spName(sim), "Survival") == 0,
                  "the map's own scenario came back as \"%s\"", spName(sim));
    UT_ASSERT_MSG(spSeats(sim, 2) == 2,
                  "the map's own scenario seated %d, wanted its two",
                  spSeats(sim, 2));

    spDestroy(sim);
    spDropMapScript(spBoundMap);
    spDropDir();
    return 0;
}

/* ── A plain map leaves the mod playing ───────────────────────────── */

int run_scenario_precedence_plain_map_keeps_mod(void) {
    ServerSim *sim;

    UT_ASSERT(spMakeDir("plain_map_keeps_mod"));
    UT_ASSERT(spWriteMod("waves.lua", kSpWaves));
    UT_ASSERT(spPutMapScript(spBoundMap, kSpBound));
    /* No script beside this one, which is what makes it a plain map. */
    spDropMapScript(spPlainMap);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, spBoundMap);
    UT_ASSERT(spSelect(sim, "waves.lua") == CMD_OK);
    UT_ASSERT(spSource(sim) == lobbyScenarioMod);
    UT_ASSERT(spSeats(sim, 3) == 3);

    /* Committing a plain map over it changes the map and not the pick. */
    spCommit(sim, spPlainMap);
    UT_ASSERT_MSG(spSource(sim) == lobbyScenarioMod,
                  "a plain map left the source at %d, wanted mod (%d)",
                  (int)spSource(sim), (int)lobbyScenarioMod);
    UT_ASSERT_MSG(strcmp(spName(sim), "Waves") == 0,
                  "the mod read back as \"%s\" over a plain map", spName(sim));
    UT_ASSERT_MSG(spSeats(sim, 3) == 3,
                  "the mod holds %d seats over a plain map, wanted its three",
                  spSeats(sim, 3));

    /* And selecting none over a plain map is the third rule: nothing plays. */
    UT_ASSERT(spSelect(sim, "") == CMD_OK);
    UT_ASSERT_MSG(spSource(sim) == lobbyScenarioNone,
                  "none over a plain map read source %d, wanted none (%d)",
                  (int)spSource(sim), (int)lobbyScenarioNone);
    UT_ASSERT_MSG(spSeats(sim, 3) == 0,
                  "%d seats outlived the mod that asked for them",
                  spSeats(sim, 3));

    spDestroy(sim);
    spDropMapScript(spBoundMap);
    spDropDir();
    return 0;
}

/* ── A mod's template seats, and the seats survive a round ────────── */

int run_scenario_precedence_template_seats(void) {
    ServerSim *sim;

    UT_ASSERT(spMakeDir("template_seats"));
    UT_ASSERT(spWriteMod("waves.lua", kSpWaves));
    spDropMapScript(spPlainMap);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    /* A plain map, so every seat here is the mod's doing. */
    spCommit(sim, spPlainMap);
    UT_ASSERT(spSelect(sim, "waves.lua") == CMD_OK);
    UT_ASSERT_MSG(spSeats(sim, 3) == 3,
                  "the mod seated %d, wanted its three", spSeats(sim, 3));
    /* This one does name a game type, so the lobby moves onto it. */
    UT_ASSERT_MSG(sim->sim.scenarioBaseGame == gameTournament,
                  "the mod's game type read %d, wanted tournament (%d)",
                  (int)sim->sim.scenarioBaseGame, (int)gameTournament);
    UT_ASSERT_MSG(gameTypeGet(&sim->sim.game) == gameScripted,
                  "a lobby with a mod attached is on game type %d, wanted "
                  "scripted (%d)", (int)gameTypeGet(&sim->sim.game),
                  (int)gameScripted);

    /* A round ends and the lobby comes back, which reconciles rather than
       seating from scratch. The mod is still attached, so its ceiling is
       what binds and its seats are still there. */
    serverSimScenarioReconcileLobby(sim);
    UT_ASSERT_MSG(spSeats(sim, 3) == 3,
                  "%d of the mod's seats came back from the round, wanted "
                  "three", spSeats(sim, 3));
    UT_ASSERT_MSG(spSource(sim) == lobbyScenarioMod,
                  "the mod stopped playing across a return to the lobby");

    spDestroy(sim);
    spDropDir();
    return 0;
}

/* ── The lobby emptying and filling again ─────────────────────────── */

/* A pinning case, not a fix. serverSimResetLobbyToDefaults already seats the
 * template again and re-applies the scenario's lobby rules at its end — both
 * calls landed with the lobby scenario work, and without them the restore of
 * the operator's settings a few lines above would leave the lobby on the
 * operator's game type with a scenario still attached and disagreeing.
 *
 * What this case is for is stopping that pair of calls being dropped by a
 * later change to the reset: it passes today, and it is the thing that would
 * notice. The mod half is the new part — nothing in the reset clears the
 * selection, so the scenario the host picked is still the one playing when
 * the next player arrives. */
int run_scenario_precedence_reset_reapplies(void) {
    ServerSim *sim;

    UT_ASSERT(spMakeDir("reset_reapplies"));
    UT_ASSERT(spWriteMod("waves.lua", kSpWaves));
    spDropMapScript(spPlainMap);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, spPlainMap);
    UT_ASSERT(spSelect(sim, "waves.lua") == CMD_OK);
    UT_ASSERT(spSeats(sim, 3) == 3);
    UT_ASSERT_MSG(sim->originalLobbySettings.gameType == gameOpen,
                  "setup: the operator's saved type is %d, wanted open (%d)",
                  (int)sim->originalLobbySettings.gameType, (int)gameOpen);

    /* The last human leaves, which is what reaches the reset. */
    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0,
                  "the lobby should be humanless");

    /* The pick survived it. */
    UT_ASSERT_MSG(strcmp(serverSimGetSelectedScenario(sim), "waves.lua") == 0,
                  "the reset left \"%s\" selected",
                  serverSimGetSelectedScenario(sim));
    UT_ASSERT_MSG(spSource(sim) == lobbyScenarioMod,
                  "the reset left the source at %d, wanted mod (%d)",
                  (int)spSource(sim), (int)lobbyScenarioMod);

    /* The template was applied again, so the next joiner opens the mod's
       lobby rather than a bare one. */
    UT_ASSERT_MSG(spSeats(sim, 3) == 3,
                  "the next joiner opens a lobby with %d of the mod's seats, "
                  "wanted three", spSeats(sim, 3));

    /* And the game type agrees with the scenario rather than with the
       operator's saved one, which the restore put back a moment earlier. */
    UT_ASSERT_MSG(gameTypeGet(&sim->sim.game) == gameScripted,
                  "after the reset the lobby is on game type %d, wanted "
                  "scripted (%d) — the operator's saved type is %d",
                  (int)gameTypeGet(&sim->sim.game), (int)gameScripted,
                  (int)sim->originalLobbySettings.gameType);

    spDestroy(sim);
    spDropDir();
    return 0;
}

/* ── A reload after the lobby block changed ───────────────────────── */

int run_scenario_precedence_reload_reseats(void) {
    ServerSim *sim;
    char       err[512];

    UT_ASSERT(spMakeDir("reload_reseats"));
    UT_ASSERT(spWriteMod("waves.lua", kSpWaves));
    spDropMapScript(spPlainMap);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, spPlainMap);
    UT_ASSERT(spSelect(sim, "waves.lua") == CMD_OK);
    UT_ASSERT_MSG(spSeats(sim, 3) == 3,
                  "the mod seated %d, wanted its three", spSeats(sim, 3));

    /* The host edits the lobby block and asks for a reload. Until the
       template was re-handed, a reload changed the rules the next round runs
       and left the lobby exactly as it was. */
    UT_ASSERT(spWriteMod("waves.lua", kSpWavesWider));
    err[0] = '\0';
    UT_ASSERT_MSG(serverSimScenarioReload(sim, err, sizeof(err)),
                  "the reload was refused: %s", err);
    UT_ASSERT_MSG(spSeats(sim, 3) == 5,
                  "after the reload the lobby holds %d of the mod's seats, "
                  "wanted the five it now asks for", spSeats(sim, 3));
    /* Still the mod, and still named as one: a reload re-hands the identity
       as well as the seats. */
    UT_ASSERT_MSG(spSource(sim) == lobbyScenarioMod,
                  "the reload left the source at %d, wanted mod (%d)",
                  (int)spSource(sim), (int)lobbyScenarioMod);
    UT_ASSERT_MSG(strcmp(spFile(sim), "waves.lua") == 0,
                  "the reload left the file name as \"%s\"", spFile(sim));

    spDestroy(sim);
    spDropDir();
    return 0;
}

/* ── What a round from a mod that named no game is played by ──────── */

/* The two cases below go past the lobby and start the round, because the
 * template alone does not say what is played: it carries the declared game,
 * and 0 for a mod that declared none. gameTypeResolve is what turns that 0
 * into a game, and the tank the host takes the field in is where the answer
 * is spent. Shells tell the two apart — strict tournament hands a tank none
 * and open hands it the rule's full load.
 *
 * Both start from a lobby on gameOpen, so a mod that was simply leaving the
 * lobby's own type alone would give the open amount either way. */
static bool spStartAndReadShells(ServerSim *sim, BYTE *shells) {
    serverSimStartGame(sim);
    if (sim->sim.tanks[0] == NULL) {
        return false;
    }
    *shells = tankGetShells(&sim->sim.tanks[0]);
    return true;
}

int run_scenario_precedence_no_game_plays_strict(void) {
    ServerSim *sim;
    BYTE       shells = 0;

    UT_ASSERT(spMakeDir("no_game_plays_strict"));
    UT_ASSERT(spWriteMod("fastreload.lua", kSpRulesOnly));
    spDropMapScript(spPlainMap);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, spPlainMap);
    UT_ASSERT_MSG(gameTypeGet(&sim->sim.game) == gameOpen,
                  "setup: the lobby is on game type %d, wanted open (%d)",
                  (int)gameTypeGet(&sim->sim.game), (int)gameOpen);
    UT_ASSERT_MSG(spSelect(sim, "fastreload.lua") == CMD_OK,
                  "the mod was refused");
    UT_ASSERT_MSG(sim->sim.scenarioBaseGame == (gameType)0,
                  "a mod naming no game declared base game %d",
                  (int)sim->sim.scenarioBaseGame);

    UT_ASSERT_MSG(spStartAndReadShells(sim, &shells),
                  "the round started with no tank in slot 0");
    UT_ASSERT_MSG(shells == 0,
                  "a round from a mod that named no game handed the host %u "
                  "shells; strict tournament hands out none",
                  (unsigned)shells);

    spDestroy(sim);
    spDropDir();
    return 0;
}

int run_scenario_precedence_open_game_plays_open(void) {
    ServerSim *sim;
    BYTE       shells = 0;

    UT_ASSERT(spMakeDir("open_game_plays_open"));
    UT_ASSERT(spWriteMod("fastreloadopen.lua", kSpRulesOnlyOpen));
    spDropMapScript(spPlainMap);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, spPlainMap);
    UT_ASSERT_MSG(spSelect(sim, "fastreloadopen.lua") == CMD_OK,
                  "the mod was refused");
    UT_ASSERT_MSG(sim->sim.scenarioBaseGame == gameOpen,
                  "a mod naming open declared base game %d, wanted open (%d)",
                  (int)sim->sim.scenarioBaseGame, (int)gameOpen);

    UT_ASSERT_MSG(spStartAndReadShells(sim, &shells),
                  "the round started with no tank in slot 0");
    UT_ASSERT_MSG(shells == (BYTE)sim->sim.rules.tank_full_shells,
                  "a round from a mod that named open handed the host %u "
                  "shells, wanted the open game's %ld",
                  (unsigned)shells, (long)sim->sim.rules.tank_full_shells);

    spDestroy(sim);
    spDropDir();
    return 0;
}

/* ── A mod edited on disk to say it is bound ──────────────────────── */

/* A mod is played over whichever map is committed, so a table that says it is
 * bound was written for a map of its own and the attach turns it away. The
 * reload checked the manifest and the api and not that, so editing the file
 * on disk and asking for a reload seated a bound scenario over any map.
 *
 * The pick is made while the file is still a mod, so what is being held to
 * the refusal is the reload rather than the pick. */
int run_scenario_precedence_reload_refuses_bound(void) {
    ServerSim *sim;
    char       err[512];

    UT_ASSERT(spMakeDir("reload_refuses_bound"));
    UT_ASSERT(spWriteMod("waves.lua", kSpWaves));
    spDropMapScript(spPlainMap);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, spPlainMap);
    UT_ASSERT(spSelect(sim, "waves.lua") == CMD_OK);
    UT_ASSERT_MSG(spSeats(sim, 3) == 3,
                  "the mod seated %d, wanted its three", spSeats(sim, 3));

    /* The same file, edited to claim it belongs to a map. */
    UT_ASSERT(spWriteMod("waves.lua", kSpWavesBound));
    err[0] = '\0';
    UT_ASSERT_MSG(!serverSimScenarioReload(sim, err, sizeof(err)),
                  "a mod edited to say it is bound reloaded");
    UT_ASSERT_MSG(strstr(err, "bound") != NULL,
                  "the refusal does not say the scenario is bound: %s", err);

    /* And what was playing is still playing: a reload that will not go
       through changes nothing, so the lobby keeps the seats it had. */
    UT_ASSERT_MSG(spSource(sim) == lobbyScenarioMod,
                  "the refused reload left the source at %d, wanted mod (%d)",
                  (int)spSource(sim), (int)lobbyScenarioMod);
    UT_ASSERT_MSG(spSeats(sim, 3) == 3,
                  "the refused reload left %d seats, wanted the three the mod "
                  "had", spSeats(sim, 3));

    spDestroy(sim);
    spDropDir();
    return 0;
}
