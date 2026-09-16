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
#include "server_sim_lifecycle.h"  /* SetLobbyEnabled, the template paths */
#include "server_sim_scenario.h"   /* serverSimScenarioReload */
#include "game_sim.h"             /* gameTypeGet, scenarioBaseGame */
#include "scenario_host.h"
#include "everard_map.h"
#include "threads.h"
#include "test_harness.h"

/* ── The scenarios directory ──────────────────────────────────────── */

#define SP_MAX_FILES 8

static char spDir[256];
static char spFiles[SP_MAX_FILES][128];
static int  spFileCount;

static bool spMakeDir(const char *tag) {
    snprintf(spDir, sizeof(spDir), "wbtest_scn_prec_%s", tag);
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
 * it proves is the manifest with no game in it — the lobby keeps whatever
 * type it was on rather than being moved to open. */
static const char kSpRulesOnly[] =
    "scenario = {\n"
    "  name = \"Fast Reload\",\n"
    "  description = \"Shells come back quicker\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
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

#define SP_BOUND_MAP "wbtest_scn_prec_bound.map"
#define SP_PLAIN_MAP "wbtest_scn_prec_plain.map"

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
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
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
    UT_ASSERT(spPutMapScript(SP_BOUND_MAP, kSpBound));
    sim = spSim();
    UT_ASSERT(sim != NULL);

    /* The bound map on its own plays its own scenario. */
    spCommit(sim, SP_BOUND_MAP);
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
    /* A manifest with no game in it leaves the lobby its own type. The
       scenario still puts the round on gameScripted — that is the rules
       being in force — and what a mod naming none does not do is decide the
       game underneath it. */
    UT_ASSERT_MSG(sim->sim.scenarioBaseGame == (gameType)0,
                  "a manifest naming no game set the base game to %d",
                  (int)sim->sim.scenarioBaseGame);

    spDestroy(sim);
    spDropMapScript(SP_BOUND_MAP);
    spDropDir();
    return 0;
}

/* ── Selecting none hands the map its own back ────────────────────── */

int run_scenario_precedence_none_restores_map(void) {
    ServerSim *sim;

    UT_ASSERT(spMakeDir("none_restores_map"));
    UT_ASSERT(spWriteMod("fastreload.lua", kSpRulesOnly));
    UT_ASSERT(spPutMapScript(SP_BOUND_MAP, kSpBound));
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, SP_BOUND_MAP);
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
    spDropMapScript(SP_BOUND_MAP);
    spDropDir();
    return 0;
}

/* ── A plain map leaves the mod playing ───────────────────────────── */

int run_scenario_precedence_plain_map_keeps_mod(void) {
    ServerSim *sim;

    UT_ASSERT(spMakeDir("plain_map_keeps_mod"));
    UT_ASSERT(spWriteMod("waves.lua", kSpWaves));
    UT_ASSERT(spPutMapScript(SP_BOUND_MAP, kSpBound));
    /* No script beside this one, which is what makes it a plain map. */
    spDropMapScript(SP_PLAIN_MAP);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, SP_BOUND_MAP);
    UT_ASSERT(spSelect(sim, "waves.lua") == CMD_OK);
    UT_ASSERT(spSource(sim) == lobbyScenarioMod);
    UT_ASSERT(spSeats(sim, 3) == 3);

    /* Committing a plain map over it changes the map and not the pick. */
    spCommit(sim, SP_PLAIN_MAP);
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
    spDropMapScript(SP_BOUND_MAP);
    spDropDir();
    return 0;
}

/* ── A mod's template seats, and the seats survive a round ────────── */

int run_scenario_precedence_template_seats(void) {
    ServerSim *sim;

    UT_ASSERT(spMakeDir("template_seats"));
    UT_ASSERT(spWriteMod("waves.lua", kSpWaves));
    spDropMapScript(SP_PLAIN_MAP);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    /* A plain map, so every seat here is the mod's doing. */
    spCommit(sim, SP_PLAIN_MAP);
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
    spDropMapScript(SP_PLAIN_MAP);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, SP_PLAIN_MAP);
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
    spDropMapScript(SP_PLAIN_MAP);
    sim = spSim();
    UT_ASSERT(sim != NULL);

    spCommit(sim, SP_PLAIN_MAP);
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
