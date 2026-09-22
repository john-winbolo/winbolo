/*
 * A scenario names the brain its teams and its roster ops run.
 *
 * The name is the directory under the server's own brains/ — "GoalHunter_1.7"
 * — and never a path, because a scenario shared with a server knows nothing of
 * that server's layout. brainListResolve turns a name into the init.lua the
 * bot loader opens, and the three places a scenario writes a brain go through
 * it: the lobby template a team is seated from, spawn_bot and lobby_add_bot.
 * The sim's own refusals stay where they are, as the check that nothing
 * unresolved arrived.
 *
 * Each case installs a brain of its own under the working directory's brains/
 * and removes it afterwards. The names are per case on purpose: ctest runs the
 * cases as separate processes in one directory, so a shared fixture name is a
 * race rather than a fixture. The brains/ parent is left where it is for the
 * same reason — another case may be looking in it at that moment.
 *
 * The map file itself is never written. The host reads the script beside a map
 * path and nothing else, and so does the validator, so a path naming no real
 * map is enough for both; the sims here are built from the built-in map.
 *
 * run_scenario_brain_name_resolves     — a team naming an installed brain
 *                                        reaches the seat as that brain's
 *                                        init.lua, and the countdown's warm
 *                                        builds the seat a runner
 * run_scenario_brain_name_missing      — a name no brains parent holds leaves
 *                                        the seat on the server's own brain,
 *                                        says so once, and is a -validate
 *                                        problem against the team's own key
 * run_scenario_brain_name_rejects_path — a brain with a separator in it is
 *                                        refused where an op writes one, and
 *                                        reported as a path where a team does
 * run_scenario_brain_op_resolves       — lobby_add_bot and spawn_bot each
 *                                        resolve a name, so neither reaches
 *                                        the funnel's refusal for a brain it
 *                                        cannot open
 * run_scenario_brain_name_op_missing_refused
 *                                      — a spawn naming a brain no parent
 *                                        holds is refused SCN_OP_NOT_FOUND
 *                                        and seats nothing, so the raw name
 *                                        never reaches the sim as a path
 * run_scenario_brain_name_mode_falls_back
 *                                      — a team naming a mode and a brain
 *                                        this server has not got has its
 *                                        mode asked of the server's own
 *                                        brain, which is the one its seats
 *                                        take
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "gametype.h"              /* gameOpen */
#include "everard_map.h"           /* E_MAP — what the sims are built from */
#include "server_sim.h"
#include "server_sim_internal.h"   /* seatBrain, lobbyPlayers, the console
                                    * callback, serverSimWarmOneHeldSeat */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled / BotAiType,
                                    * serverSimStartGame */
#include "server_sim_scenario.h"
#include "server/sim/server_sim_shared.h" /* serverSimSetActive, which is what
                                           * the console routes through */
#include "bot_manager.h"           /* botManagerHasRunner */
#include "brain_list.h"            /* brainListResolve */
#include "scenario_host.h"
#include "scenario_validate.h"
#include "test_harness.h"

#if defined(_WIN32)
#  define BN_SEP '\\'
#else
#  define BN_SEP '/'
#endif

/* The team every case seats, so a failure message names the same team the
 * script does. */
#define BN_TEAM 3

/* ── An installed brain ───────────────────────────────────────────── */

/* brains/<name>/init.lua under the working directory, which is the first
 * parent brainListResolve looks in. */
static char bnName[64];
static char bnDir[192];
static char bnPath[256];

static bool bnInstallBrain(const char *tag) {
    FILE *f;

    snprintf(bnName, sizeof(bnName), "BrainNameTest_%s", tag);
    snprintf(bnDir, sizeof(bnDir), "brains%c%s", BN_SEP, bnName);
    snprintf(bnPath, sizeof(bnPath), "%s%cinit.lua", bnDir, BN_SEP);
    /* SDL_CreateDirectory makes the missing parent and is a no-op on a
       directory already there, which brains/ may well be. */
    if (!SDL_CreateDirectory(bnDir)) {
        return false;
    }
    f = fopen(bnPath, "wb");
    if (f == NULL) {
        return false;
    }
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

/* A modes.txt in the brain installed above, for the one case that names a
 * mode. brain_list.c reads it by the name of the directory holding init.lua,
 * so it belongs beside that file and not anywhere else. The two modes and
 * their three levels are written out here rather than borrowed from a
 * shipped brain, so the case says what it expects and an edit to
 * GoalHunter's own modes.txt cannot move it. */
static char bnModes[256];

static bool bnInstallModes(void) {
    FILE *f;

    snprintf(bnModes, sizeof(bnModes), "%s%cmodes.txt", bnDir, BN_SEP);
    f = fopen(bnModes, "wb");
    if (f == NULL) {
        return false;
    }
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

/* The brain's file and its own directory, and not the brains/ parent: a case
 * running beside this one may be looking in that. The modes.txt goes first
 * where one was installed, because a directory still holding it cannot be
 * removed. */
static void bnRemoveBrain(void) {
    if (bnModes[0] != '\0') {
        remove(bnModes);
        bnModes[0] = '\0';
    }
    if (bnPath[0] != '\0') {
        remove(bnPath);
    }
    if (bnDir[0] != '\0') {
        SDL_RemovePath(bnDir);
    }
    bnName[0] = '\0';
    bnDir[0]  = '\0';
    bnPath[0] = '\0';
}

static bool bnIsFile(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* ── The script beside the map ────────────────────────────────────── */

/* .../X.map is accompanied by .../X.scenario.lua. */
static void bnScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);

    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

static bool bnPut(const char *mapPath, const char *lua) {
    char  path[512];
    FILE *f;

    bnScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static void bnDrop(const char *mapPath) {
    char path[512];

    bnScriptFor(mapPath, path, sizeof(path));
    remove(path);
}

/* One team, holding its seats, naming this brain. */
static void bnTeamScript(char *out, size_t outLen, const char *brain) {
    snprintf(out, outLen,
             "scenario = {\n"
             "  name = \"Named\", api = 1,\n"
             "  lobby = {\n"
             "    teams = {\n"
             "      { id = %d, bots = 1, max_bots = 2, fielded = false,\n"
             "        brain = \"%s\" },\n"
             "    },\n"
             "  },\n"
             "}\n", BN_TEAM, brain);
}

/* The same, with the mode the team's bots are to play in and the level
 * inside it. Both go on the team beside the brain, which is where a file
 * writes them. */
static void bnTeamScriptMode(char *out, size_t outLen, const char *brain,
                             const char *mode, const char *level) {
    snprintf(out, outLen,
             "scenario = {\n"
             "  name = \"Named\", api = 1,\n"
             "  lobby = {\n"
             "    teams = {\n"
             "      { id = %d, bots = 1, max_bots = 2, fielded = false,\n"
             "        brain = \"%s\", mode = \"%s\", difficulty = \"%s\" },\n"
             "    },\n"
             "  },\n"
             "}\n", BN_TEAM, brain, mode, level);
}

/* ── The sims ─────────────────────────────────────────────────────── */

/* A lobby taking part, with one ready human in slot 0 and a server that runs
 * bots. serverSimSetActive because serverSimConsoleMessage writes through the
 * active sim's callback and falls back to stdout when there is none; creating
 * a sim does not make it the active one. */
static ServerSim *bnLobbySim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);

    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Human", false);
    sim->lobbyPlayers[0].ready = true;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetActive(sim);
    return sim;
}

/* A sim with no lobby, ready to be attached to and then started. */
static ServerSim *bnRoundSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);

    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetActive(sim);
    return sim;
}

/* The seat this team is holding, or -1 when the template seated none. */
static int bnHeldSeat(ServerSim *sim, BYTE team) {
    int i;

    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].keepSeat &&
            sim->lobbyPlayers[i].teamNumber == team) {
            return i;
        }
    }
    return -1;
}

/* ── What the console was told ────────────────────────────────────── */

/* Both the host's own lines and whatever a script printed: the scenario
 * sandbox routes print to the server console. Only consoleMessage is
 * replaced, never the ctx beside it, which the sim's other callbacks read. */
static void (*bnConsolePrev)(void *ctx, char *msg) = NULL;
static char bnSaid[8192];

static void bnConsoleCb(void *ctx, char *msg) {
    size_t have;
    size_t room;
    size_t n;

    if (bnConsolePrev != NULL) {
        bnConsolePrev(ctx, msg);
    }
    if (msg == NULL) {
        return;
    }
    have = strlen(bnSaid);
    room = sizeof(bnSaid) - 1 - have;
    n    = strlen(msg);
    if (n > room) {
        n = room;
    }
    memcpy(bnSaid + have, msg, n);
    bnSaid[have + n] = '\0';
}

static void bnWatchConsole(ServerSim *sim) {
    bnSaid[0]     = '\0';
    bnConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = bnConsoleCb;
}

static void bnUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = bnConsolePrev;
    bnConsolePrev = NULL;
}

/* ── What the validator reported ──────────────────────────────────── */

static const ScnValidateIssue *bnFind(const ScnValidateResult *r,
                                      const char *key) {
    uint16_t i;

    for (i = 0; i < r->count; i++) {
        if (strcmp(r->issues[i].key, key) == 0) {
            return &r->issues[i];
        }
    }
    return NULL;
}

/* Every issue on one line, so a failure says what was reported rather than
 * only what was missing. */
static void bnList(const ScnValidateResult *r, char *out, size_t outLen) {
    uint16_t i;
    size_t   at = 0;

    out[0] = '\0';
    for (i = 0; i < r->count && at + 1 < outLen; i++) {
        at += (size_t)snprintf(out + at, outLen - at, "[%s %s] ",
                               r->issues[i].key, r->issues[i].message);
    }
}

/* ── 1. A name reaches the seat as a path ─────────────────────────── */

int run_scenario_brain_name_resolves(void) {
    static const char *const kMap = "scnbrain_resolves.map";
    char          lua[512];
    char          want[512];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           seat;

    UT_ASSERT(bnInstallBrain("resolves"));
    snprintf(want, sizeof(want), "%s", bnPath);
    bnTeamScript(lua, sizeof(lua), bnName);
    UT_ASSERT(bnPut(kMap, lua));

    /* The fixture brain in test_stubs.c stands in for a Lua brain, so the
       warm below builds a runner and the case is about the resolution and
       nothing else. */
    ut_brain_stub_arm(true);
    sim = bnLobbySim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    /* What a committed map runs once the map is in. */
    serverSimScenarioSeatLobby(sim);

    seat = bnHeldSeat(sim, BN_TEAM);
    UT_ASSERT_MSG(seat >= 0, "the template seated nothing on team %d",
                  BN_TEAM);
    UT_ASSERT_MSG(strcmp(sim->seatBrain[seat], want) == 0,
                  "seat %d holds '%s', expected '%s' — the name did not become "
                  "the path of the brain it names", seat,
                  sim->seatBrain[seat], want);
    UT_ASSERT_MSG(bnIsFile(sim->seatBrain[seat]),
                  "seat %d names '%s', which is no file", seat,
                  sim->seatBrain[seat]);

    /* And the countdown's warm builds that seat's runner rather than skipping
       it, which is what resolving the name this early is for. */
    UT_ASSERT_MSG(serverSimWarmOneHeldSeat(sim),
                  "the warm built nothing for a seat whose brain resolves");
    UT_ASSERT_MSG(botManagerHasRunner(sim, (BYTE)seat),
                  "seat %d has no runner after the warm", seat);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    ut_brain_stub_arm(false);
    bnDrop(kMap);
    bnRemoveBrain();
    return 0;
}

/* ── 2. A name this server does not have ──────────────────────────── */

int run_scenario_brain_name_missing(void) {
    static const char *const kMap = "scnbrain_missing.map";
    char              lua[512];
    char              missing[96];
    char              seen[1024];
    ServerSim        *sim;
    ScenarioHost     *h;
    ScnValidateResult r;
    char              err[512];
    int               seat;

    /* A brain is installed and the team names a different one, so the case
       says the name was looked up and not found rather than that there was
       nowhere to look. */
    UT_ASSERT(bnInstallBrain("missing"));
    snprintf(missing, sizeof(missing), "%s_NotInstalled", bnName);
    bnTeamScript(lua, sizeof(lua), missing);
    UT_ASSERT(bnPut(kMap, lua));

    ut_brain_stub_arm(true);
    sim = bnLobbySim();
    UT_ASSERT(sim != NULL);
    /* The server's own brain, which is what the seat falls back to. */
    serverSimSetBotBrainPath(sim, bnPath);
    bnWatchConsole(sim);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimScenarioSeatLobby(sim);

    seat = bnHeldSeat(sim, BN_TEAM);
    UT_ASSERT_MSG(seat >= 0,
                  "the template seated nothing on team %d; a brain this "
                  "server does not have must not cost the team its seats",
                  BN_TEAM);
    UT_ASSERT_MSG(sim->seatBrain[seat][0] == '\0',
                  "seat %d holds '%s'; a name that does not resolve must "
                  "leave the seat on the server's own brain", seat,
                  sim->seatBrain[seat]);

    /* Said once, naming the team and the brain, so an operator watching the
       console knows which team lost which brain. */
    UT_ASSERT_MSG(strstr(bnSaid, missing) != NULL,
                  "the console never named the brain that is missing: %s",
                  bnSaid);
    UT_ASSERT_MSG(strstr(bnSaid, "does not have") != NULL,
                  "the console did not say the server has no such brain: %s",
                  bnSaid);

    /* The seat runs the server's own brain, which is what the warm builds. */
    UT_ASSERT_MSG(serverSimWarmOneHeldSeat(sim),
                  "the warm built nothing for a seat on the server's own "
                  "brain");
    UT_ASSERT_MSG(botManagerHasRunner(sim, (BYTE)seat),
                  "seat %d has no runner after the warm", seat);

    bnUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    ut_brain_stub_arm(false);

    /* And the same thing said before a round is ever started, against the
       team's own key. */
    sim = bnRoundSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "a team naming a brain this server has not got was passed "
                  "without a word");
    bnList(&r, seen, sizeof(seen));
    UT_ASSERT_MSG(bnFind(&r, "lobby.teams[1].brain") != NULL,
                  "no problem against the team's brain: %s", seen);
    UT_ASSERT_MSG(strstr(bnFind(&r, "lobby.teams[1].brain")->message,
                         missing) != NULL,
                  "the problem does not name the brain: %s", seen);

    serverSimDestroy(sim);
    bnDrop(kMap);
    bnRemoveBrain();
    return 0;
}

/* ── 3. A path where a name belongs ───────────────────────────────── */

int run_scenario_brain_name_rejects_path(void) {
    static const char *const kMap   = "scnbrain_path.map";
    static const char *const kOpMap = "scnbrain_path_op.map";
    char              lua[1024];
    char              asPath[512];
    char              escaped[640];
    char              seen[1024];
    ServerSim        *sim;
    ScenarioHost     *h;
    ScnValidateResult r;
    char              err[512];
    const ScnValidateIssue *issue;
    size_t            i;
    size_t            at = 0;
    int               seat;

    UT_ASSERT(bnInstallBrain("rejects_path"));
    /* The path of the brain that IS installed: the value names a file that is
       really there, so what is refused is its shape and not its content. */
    snprintf(asPath, sizeof(asPath), "%s", bnPath);
    /* A Windows path carries backslashes, which a Lua string would read as
       escapes. */
    for (i = 0; asPath[i] != '\0' && at + 2 < sizeof(escaped); i++) {
        if (asPath[i] == '\\') {
            escaped[at++] = '\\';
        }
        escaped[at++] = asPath[i];
    }
    escaped[at] = '\0';

    bnTeamScript(lua, sizeof(lua), escaped);
    UT_ASSERT(bnPut(kMap, lua));

    ut_brain_stub_arm(true);
    sim = bnLobbySim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimScenarioSeatLobby(sim);
    seat = bnHeldSeat(sim, BN_TEAM);
    UT_ASSERT_MSG(seat >= 0, "the template seated nothing on team %d",
                  BN_TEAM);
    UT_ASSERT_MSG(sim->seatBrain[seat][0] == '\0',
                  "seat %d holds '%s'; a path is not a name and must not "
                  "reach the seat", seat, sim->seatBrain[seat]);

    scenarioHostDetach(h);
    serverSimDestroy(sim);

    /* The validator says which of the two it is, so an author who wrote the
       old form is told what to write instead. */
    sim = bnRoundSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "a team whose brain is a path was passed without a word");
    bnList(&r, seen, sizeof(seen));
    issue = bnFind(&r, "lobby.teams[1].brain");
    UT_ASSERT_MSG(issue != NULL, "no problem against the team's brain: %s",
                  seen);
    UT_ASSERT_MSG(strstr(issue->message, "is a path") != NULL,
                  "the problem does not say the value is a path: %s",
                  issue->message);
    serverSimDestroy(sim);

    /* And a roster op writing one is refused rather than run. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Ops\", api = 1 }\n"
             "local done = false\n"
             "function on_tick(t)\n"
             "  if done then return end\n"
             "  done = true\n"
             "  local slot, why, detail = game.spawn_bot{ brain = \"%s\",\n"
             "                                            team = 1 }\n"
             "  if slot then print(\"brain:spawned\")\n"
             "  else print(\"brain:refused \" .. tostring(why) .. \" \" ..\n"
             "             tostring(detail)) end\n"
             "end\n", escaped);
    UT_ASSERT(bnPut(kOpMap, lua));

    sim = bnRoundSim();
    UT_ASSERT(sim != NULL);
    serverSimSetBotBrainPath(sim, bnPath);
    bnWatchConsole(sim);

    h = scenarioHostAttach(sim, kOpMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the op script was refused: %s", err);
    serverSimStartGame(sim);
    /* Three, not one: the script spawns on the first running tick, and a
       spawn that had been accepted would be queued and taken up on the tick
       after. */
    serverSimTick(sim);
    serverSimTick(sim);
    serverSimTick(sim);

    UT_ASSERT_MSG(strstr(bnSaid, "brain:refused") != NULL,
                  "a spawn whose brain is a path was not refused; the console "
                  "heard: %s", bnSaid);
    UT_ASSERT_MSG(strstr(bnSaid, "is a path") != NULL,
                  "the refusal did not say the value is a path: %s", bnSaid);

    bnUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    ut_brain_stub_arm(false);
    bnDrop(kMap);
    bnDrop(kOpMap);
    bnRemoveBrain();
    return 0;
}

/* ── 4. Both ops resolve a name ───────────────────────────────────── */

/* A brain the funnel cannot open is refused SCN_OP_NOT_FOUND, so an op the
 * funnel accepts is an op whose name became a path on the way. The two are
 * each run in the state they belong to: lobby_add_bot from the chunk's own top
 * level, which runs at the attach while the sim is still a lobby, and
 * spawn_bot from the first running tick of a round. */
int run_scenario_brain_op_resolves(void) {
    static const char *const kAddMap   = "scnbrain_op_add.map";
    static const char *const kSpawnMap = "scnbrain_op_spawn.map";
    char          lua[1024];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;
    int           bots;

    UT_ASSERT(bnInstallBrain("op_resolves"));
    ut_brain_stub_arm(true);

    /* lobby_add_bot, fielded, so the add goes through the funnel's brain
       check rather than holding a seat that loads nothing. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Ops\", api = 1 }\n"
             "local slot, why, detail =\n"
             "  game.lobby_add_bot{ brain = \"%s\", team = 2 }\n"
             "if slot then print(\"brain:added\")\n"
             "else print(\"brain:refused \" .. tostring(why) .. \" \" ..\n"
             "           tostring(detail)) end\n", bnName);
    UT_ASSERT(bnPut(kAddMap, lua));

    sim = bnLobbySim();
    UT_ASSERT(sim != NULL);
    bnWatchConsole(sim);

    h = scenarioHostAttach(sim, kAddMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the add script was refused: %s", err);

    UT_ASSERT_MSG(strstr(bnSaid, "brain:added") != NULL,
                  "lobby_add_bot did not take a brain by name; the console "
                  "heard: %s", bnSaid);
    UT_ASSERT_MSG(strstr(bnSaid, "SCN_OP_NOT_FOUND") == NULL,
                  "lobby_add_bot reached the funnel's refusal for a brain it "
                  "cannot open: %s", bnSaid);
    bots = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) {
            bots++;
        }
    }
    UT_ASSERT_MSG(bots == 1, "%d bots in the lobby, expected 1", bots);

    bnUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);

    /* spawn_bot, on the first running tick of a round. */
    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Ops\", api = 1 }\n"
             "local done = false\n"
             "function on_tick(t)\n"
             "  if done then return end\n"
             "  done = true\n"
             "  local slot, why, detail =\n"
             "    game.spawn_bot{ brain = \"%s\", team = 2 }\n"
             "  if slot then print(\"brain:spawned\")\n"
             "  else print(\"brain:refused \" .. tostring(why) .. \" \" ..\n"
             "             tostring(detail)) end\n"
             "end\n", bnName);
    UT_ASSERT(bnPut(kSpawnMap, lua));

    sim = bnRoundSim();
    UT_ASSERT(sim != NULL);
    bnWatchConsole(sim);

    h = scenarioHostAttach(sim, kSpawnMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the spawn script was refused: %s", err);
    serverSimStartGame(sim);
    serverSimTick(sim);
    serverSimTick(sim);
    serverSimTick(sim);

    UT_ASSERT_MSG(strstr(bnSaid, "brain:spawned") != NULL,
                  "spawn_bot did not take a brain by name; the console heard: "
                  "%s", bnSaid);
    UT_ASSERT_MSG(strstr(bnSaid, "SCN_OP_NOT_FOUND") == NULL,
                  "spawn_bot reached the funnel's refusal for a brain it "
                  "cannot open: %s", bnSaid);
    bots = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) {
            bots++;
        }
    }
    UT_ASSERT_MSG(bots == 1, "%d bots in the round, expected 1", bots);

    bnUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    ut_brain_stub_arm(false);
    bnDrop(kAddMap);
    bnDrop(kSpawnMap);
    bnRemoveBrain();
    return 0;
}

/* ── 5. A spawn naming a brain this server has not got ────────────── */

/* A name the brains parents do not hold used to be left as the script wrote
 * it and handed on, where the sim reads it as a path relative to wherever the
 * server was started: spawn_bot{ brain = "init.lua" } opened ./init.lua. The
 * name is refused at the row now, so the raw value never reaches the sim.
 *
 * No brain is installed here at all, and the name the script writes is one
 * nothing could resolve. The server's own brain is set, so a bot that did
 * land would have had one to run — the roster being empty afterwards is the
 * refusal and not a missing fixture. */
int run_scenario_brain_name_op_missing_refused(void) {
    static const char *const kOpMap = "wbtest_brain_op_missing.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           bots;
    int           i;

    /* A brain for the server itself, so the only thing missing is the one the
       script names. */
    UT_ASSERT(bnInstallBrain("op_missing"));
    ut_brain_stub_arm(true);

    UT_ASSERT(bnPut(kOpMap,
        "scenario = { name = \"Ops\", api = 1 }\n"
        "local done = false\n"
        "function on_tick(t)\n"
        "  if done then return end\n"
        "  done = true\n"
        "  local slot, why, detail = game.spawn_bot{ brain = \"init.lua\",\n"
        "                                            team = 1 }\n"
        "  if slot then print(\"brain:spawned\")\n"
        "  else print(\"brain:refused \" .. tostring(why) .. \" \" ..\n"
        "             tostring(detail)) end\n"
        "end\n"));

    sim = bnRoundSim();
    UT_ASSERT(sim != NULL);
    serverSimSetBotBrainPath(sim, bnPath);
    bnWatchConsole(sim);

    h = scenarioHostAttach(sim, kOpMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the op script was refused: %s", err);
    serverSimStartGame(sim);
    /* Three, as the path case runs: the spawn is made on the first running
       tick, and one that had been accepted would be queued and taken up on
       the tick after. */
    serverSimTick(sim);
    serverSimTick(sim);
    serverSimTick(sim);

    UT_ASSERT_MSG(strstr(bnSaid, "brain:refused") != NULL,
                  "a spawn naming a brain this server has not got was not "
                  "refused; the console heard: %s", bnSaid);
    UT_ASSERT_MSG(strstr(bnSaid, "SCN_OP_NOT_FOUND") != NULL,
                  "the refusal was not SCN_OP_NOT_FOUND: %s", bnSaid);
    UT_ASSERT_MSG(strstr(bnSaid, "names no brain") != NULL,
                  "the refusal did not say the name resolves to nothing: %s",
                  bnSaid);

    /* And nothing was seated, which is the half the refusal is for: a bot
       built on the raw name would have run whatever ./init.lua happened to
       be. */
    bots = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) {
            bots++;
        }
    }
    UT_ASSERT_MSG(bots == 0,
                  "%d bots in the round after a refused spawn, expected none",
                  bots);

    bnUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    ut_brain_stub_arm(false);
    bnDrop(kOpMap);
    bnRemoveBrain();
    return 0;
}

/* ── 6. A mode on a team whose brain does not resolve ─────────────── */

/* The mode and the level a team names are asked of the brain that will run
   its bots. Where the name the team gave resolves to nothing, that brain is
   the server's own: a name this server has not got costs the team neither
   its seats nor its keys, and the seats run the server's brain. So the
   validator asks the server's brain rather than leaving the question out.

   Leaving it out was the alternative, and it is the wrong one. It would
   pass a template whose keys the seating then refuses — the seating asks
   the same question of the same brain through the same call — and a
   -validate that passes what the round will not take is worth nothing.

   Two halves against one installed brain and one name it does not hold: a
   mode the server's brain lists, which draws no problem of its own, and one
   it does not, which draws a problem naming that brain. */
int run_scenario_brain_name_mode_falls_back(void) {
    static const char *const kMap = "scnbrain_modefallback.map";
    char              lua[640];
    char              missing[96];
    char              seen[1024];
    ServerSim        *sim;
    ScnValidateResult r;

    UT_ASSERT(bnInstallBrain("modefallback"));
    UT_ASSERT(bnInstallModes());
    snprintf(missing, sizeof(missing), "%s_NotInstalled", bnName);

    /* A mode the server's own brain lists. The team's brain is still
       reported, because the server has not got it, and the mode is not,
       because the brain the seats will take knows it. */
    bnTeamScriptMode(lua, sizeof(lua), missing, "survival", "hard");
    UT_ASSERT(bnPut(kMap, lua));

    sim = bnRoundSim();
    UT_ASSERT(sim != NULL);
    serverSimSetBotBrainPath(sim, bnPath);
    UT_ASSERT_MSG(!scenarioValidateMap(sim, kMap, &r),
                  "a team naming a brain this server has not got was "
                  "passed without a word");
    bnList(&r, seen, sizeof(seen));
    UT_ASSERT_MSG(bnFind(&r, "lobby.teams[1].brain") != NULL,
                  "no problem against the team's brain: %s", seen);
    UT_ASSERT_MSG(bnFind(&r, "lobby.teams[1].mode") == NULL,
                  "a mode the server's own brain lists was reported as a "
                  "problem; the check has to fall back to that brain, not "
                  "to nothing: %s", seen);
    UT_ASSERT_MSG(bnFind(&r, "lobby.teams[1].difficulty") == NULL,
                  "a level that brain lists was reported as a problem: %s",
                  seen);
    serverSimDestroy(sim);

    /* And a mode it does not list, which is reported against the team's own
       key and names the brain the question was asked of. */
    bnTeamScriptMode(lua, sizeof(lua), missing, "nosuchmode", "hard");
    UT_ASSERT(bnPut(kMap, lua));

    sim = bnRoundSim();
    UT_ASSERT(sim != NULL);
    serverSimSetBotBrainPath(sim, bnPath);
    UT_ASSERT(!scenarioValidateMap(sim, kMap, &r));
    bnList(&r, seen, sizeof(seen));
    UT_ASSERT_MSG(bnFind(&r, "lobby.teams[1].mode") != NULL,
                  "a mode no brain in this test lists went unreported, so "
                  "the check was skipped rather than asked of the "
                  "server's brain: %s", seen);
    UT_ASSERT_MSG(strstr(bnFind(&r, "lobby.teams[1].mode")->message,
                         bnName) != NULL,
                  "the problem does not name the brain it asked: %s", seen);
    serverSimDestroy(sim);

    bnDrop(kMap);
    bnRemoveBrain();
    return 0;
}

/* ── 7. A mode on a team with no brain, asked with no sim ─────────── */

/* The editor checks a script with no sim, which is the one case where the
   fallback above has nothing to fall back to: the team named no brain of its
   own and there is no server to give one. Those seats will run whatever brain
   the server is configured with, so the mode and the level are the server's
   question, and WinBoloDS -validate is where it gets asked.

   Asked of no brain at all it resolved to no manifest, and the editor drew
   "brain '' declares no modes" — the empty quotes being the tell — against a
   file WinBoloDS takes. No brain is now no question.

   The total is deliberately not asserted on. A template this small may raise
   something else, and this case is about the absence of that one message
   rather than about silence. */
int run_scenario_brain_name_mode_no_brain(void) {
    static const char *const kName = "scnbrain_nobrain.scenario.lua";
    char              lua[640];
    char              seen[1024];
    ScnValidateResult r;
    uint16_t          i;

    /* The same team the other scripts here use, with the mode and the level
       on it and the brain left off entirely. */
    snprintf(lua, sizeof(lua),
             "scenario = {\n"
             "  name = \"Named\", api = 1,\n"
             "  lobby = {\n"
             "    teams = {\n"
             "      { id = %d, bots = 1, max_bots = 2, fielded = false,\n"
             "        mode = \"survival\", difficulty = \"hard\" },\n"
             "    },\n"
             "  },\n"
             "}\n", BN_TEAM);

    scenarioValidateSource(NULL, lua, strlen(lua), kName, NULL, &r);
    bnList(&r, seen, sizeof(seen));
    for (i = 0; i < r.count; i++) {
        UT_ASSERT_MSG(strstr(r.issues[i].message, "declares no modes") == NULL,
                      "a team that named no brain was checked against one "
                      "anyway, with no sim to give the server's: %s", seen);
    }
    return 0;
}
