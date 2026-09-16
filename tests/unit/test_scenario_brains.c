/*
 * A brain carried inside a scenario's container.
 *
 * The bot manager loads a brain by path and nothing about that changes here,
 * so the container's brain directories are written out beside the map before
 * anything asks for one, and a "package:NAME" is turned into the path of the
 * file the loader opens before it reaches the sim. The two places that turn a
 * name into a path are the lobby template the sim seats from and the spawn
 * op's own brain; the sim's two refusals stay where they are as the check that
 * neither was missed.
 *
 * The extraction directory is keyed by the MD5 of the whole map file, so these
 * cases compute that hash themselves and look where the host would have put
 * the files. The directory name is the one scenario_host.c writes; a change
 * there fails these cases rather than passing quietly.
 *
 * Each case writes its own fixture under its own scratch directory: ctest -j
 * runs cases as separate processes, so a shared fixture name is a race rather
 * than a fixture. The .map file is a synthetic BMAP built byte by byte, as in
 * test_scenario_packed_map.c, with a container written after it.
 *
 * run_scenario_brains_extracted
 *      — a packed map whose container carries a brain has it on disk beside
 *        the map after the attach, byte for byte as it went in
 * run_scenario_brains_same_md5_skips
 *      — attaching the same map again writes nothing, and a map file that
 *        changed extracts into a directory of its own
 * run_scenario_brains_template_resolved
 *      — a team whose manifest brain is "package:NAME" reaches the seat as a
 *        real path ending in init.lua, and the countdown's warm builds it
 * run_scenario_brains_unknown_name_refused
 *      — a spawn naming a brain the container does not carry is refused, and
 *        nothing was left half-extracted for it
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "everard_map.h"           /* E_MAP — the map the sims are built from */
#include "gametype.h"              /* gameOpen */
#include "server_sim.h"
#include "server_sim_internal.h"   /* seatBrain, lobbyPlayers, the console
                                    * callback, serverSimWarmOneHeldSeat */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled / BotAiType */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive, which is what
                                           * the console routes through */
#include "bot_manager.h"           /* botManagerHasRunner */
#include "common/md5.h"            /* the hash that names the directory the
                                    * brains were extracted into */
#include "scenario_host.h"
#include "scenario_package.h"
#include "test_harness.h"

/* ── What the container carries ───────────────────────────────────── */

#define SB_BRAIN "raiders"
#define SB_TEAM  3

/* The directory the host puts a map's packaged brains in, beside the map.
 * Spelled out here because a test asserting on a layout has to name it; it is
 * SCN_BRAIN_DIR_NAME in scenario_host.c. */
#define SB_DIR_NAME ".scenario-brains"

static const char kSbBrainInit[] =
    "-- the brain the container carries\n"
    "return { name = 'raiders', tick = function() end }\n";
static const char kSbBrainUtil[] =
    "-- a second file, so the extraction is a directory and not one file\n"
    "return { clamp = function(v) return v end }\n";

/* No lobby: the two cases that only look at the files on disk. */
static const char kSbManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"name\": \"Packed Brains\",\n"
    "  \"api\": 1,\n"
    "  \"bound\": true,\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

/* One unfielded team, named by the brain the container carries. */
static const char kSbTeamManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"name\": \"Packed Brains\",\n"
    "  \"api\": 1,\n"
    "  \"bound\": true,\n"
    "  \"script\": \"main.lua\",\n"
    "  \"lobby\": {\n"
    "    \"teams\": [\n"
    "      { \"id\": 3, \"bots\": 2, \"max_bots\": 2, \"fielded\": false,\n"
    "        \"brain\": \"package:" SB_BRAIN "\" }\n"
    "    ]\n"
    "  }\n"
    "}\n";

/* A script that declares no table of its own: the manifest above is what the
 * host reads, and there is nothing for the two to disagree about. */
static const char kSbSilent[] =
    "function on_setup() end\n";

/* The same, with a byte changed, so a map packed with it hashes differently
 * from one packed with the above. */
static const char kSbSilentAgain[] =
    "function on_setup() end -- again\n";

/* One spawn, on the round's first tick, naming a brain the container does not
 * carry. The refusal comes back as a nil and the result's own name rather than
 * as an error, so the script reads it and says which. */
static const char kSbSpawnMissing[] =
    "local done = false\n"
    "function on_tick(t)\n"
    "  if done then return end\n"
    "  done = true\n"
    "  local slot, why = game.spawn_bot{ brain = 'package:NotThere', team = 1 }\n"
    "  if slot then print('brains:spawned') else\n"
    "    print('brains:refused ' .. tostring(why))\n"
    "  end\n"
    "end\n";

/* ── Fixtures ─────────────────────────────────────────────────────── */

/* A map with entities and two runs in it, built byte by byte so the walk that
 * measures it steps over a datalen rather than meeting the terminator straight
 * away. The entity and run data is filler: nothing decodes it. */
static size_t sbMapBytes(uint8_t *out) {
    size_t n = 0;

    memcpy(out + n, "BMAPBOLO", 8);
    n += 8;
    out[n++] = 1;   /* version */
    out[n++] = 1;   /* one pill */
    out[n++] = 2;   /* two bases */
    out[n++] = 1;   /* one start */
    memset(out + n, 0x11, 5);
    n += 5;
    memset(out + n, 0x22, 12);
    n += 12;
    memset(out + n, 0x33, 3);
    n += 3;

    out[n++] = 10;  /* four header bytes and six of data */
    out[n++] = 3;
    out[n++] = 0;
    out[n++] = 20;
    memset(out + n, 0xAB, 6);
    n += 6;

    out[n++] = 9;   /* four header bytes and five of data */
    out[n++] = 4;
    out[n++] = 0;
    out[n++] = 20;
    memset(out + n, 0xCD, 5);
    n += 5;

    out[n++] = 4;
    out[n++] = 255;
    out[n++] = 255;
    out[n++] = 255;
    return n;
}

/* One map file with a container after it: the manifest, the script and, when
 * asked for, a brain of two files. --pack does not write brains yet, so the
 * fixture builds its own container in memory. */
static bool sbWritePacked(const char *path, const char *manifest,
                          const char *lua, bool withBrain) {
    uint8_t         map[128];
    uint8_t        *container    = NULL;
    size_t          containerLen = 0;
    size_t          mapLen;
    ScnPackageEntry entries[4];
    int             count = 2;
    char            err[256];
    FILE           *f;
    bool            ok;

    mapLen = sbMapBytes(map);

    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)manifest;
    entries[0].len   = strlen(manifest);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)lua;
    entries[1].len   = strlen(lua);
    if (withBrain) {
        entries[2].name    = "brains/" SB_BRAIN "/init.lua";
        entries[2].bytes   = (const uint8_t *)kSbBrainInit;
        entries[2].len     = sizeof(kSbBrainInit) - 1;
        entries[2].deflate = true;
        entries[3].name    = "brains/" SB_BRAIN "/util.lua";
        entries[3].bytes   = (const uint8_t *)kSbBrainUtil;
        entries[3].len     = sizeof(kSbBrainUtil) - 1;
        entries[3].deflate = true;
        count = 4;
    }
    if (!scnPackageWrite(entries, count, &container, &containerLen, err,
                         sizeof(err))) {
        return false;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        free(container);
        return false;
    }
    ok = fwrite(map, 1, mapLen, f) == mapLen &&
         fwrite(container, 1, containerLen, f) == containerLen;
    fclose(f);
    free(container);
    return ok;
}

/* .../X.map is accompanied by .../X.scenario.lua. */
static void sbLoosePath(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);

    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

/* Where the host will have put this map's brains: the fixed directory beside
 * the map, with the MD5 of the whole map file under it. Computed the same way
 * the host computes it and from the same bytes, so a case that looked in the
 * wrong place would be looking in the wrong place for the host too. */
static bool sbRootOf(const char *mapPath, char *out, size_t outLen) {
    uint8_t     digest[16];
    char        hex[33];
    uint8_t    *file  = NULL;
    long        size;
    size_t      got;
    const char *slash = NULL;
    const char *p;
    FILE       *f     = fopen(mapPath, "rb");

    if (f == NULL) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    size = ftell(f);
    if (size <= 0) { fclose(f); return false; }
    rewind(f);
    file = (uint8_t *)malloc((size_t)size);
    if (file == NULL) { fclose(f); return false; }
    got = fread(file, 1, (size_t)size, f);
    fclose(f);

    md5Compute(file, got, digest);
    md5ToHex(digest, hex);
    free(file);

    for (p = mapPath; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (slash == NULL) {
        snprintf(out, outLen, "%s/%s", SB_DIR_NAME, hex);
    } else {
        snprintf(out, outLen, "%.*s/%s/%s", (int)(slash - mapPath), mapPath,
                 SB_DIR_NAME, hex);
    }
    return true;
}

/* Whether a file holds exactly these bytes. */
static bool sbFileIs(const char *path, const char *expect, size_t expectLen) {
    char  *buf;
    size_t got;
    bool   same;
    FILE  *f = fopen(path, "rb");

    if (f == NULL) return false;
    buf = (char *)malloc(expectLen + 1);
    if (buf == NULL) { fclose(f); return false; }
    got  = fread(buf, 1, expectLen + 1, f);
    fclose(f);
    same = (got == expectLen) && (memcmp(buf, expect, expectLen) == 0);
    free(buf);
    return same;
}

static bool sbWriteText(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");

    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static bool sbIsFile(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

static bool sbExists(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info);
}

/* A sim in a lobby, with one human in slot 0 and a server that runs bots. */
static ServerSim *sbLobbySim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);

    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Human", false);
    sim->lobbyPlayers[0].ready = true;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetActive(sim);
    return sim;
}

/* A sim with no lobby, ready to be attached to and then started. */
static ServerSim *sbRoundSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);

    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimSetBotAiType(sim, aiFull);
    /* serverSimConsoleMessage writes through the active sim's callback and
       falls back to stdout when there is none, and creating a sim does not
       make it the active one. */
    serverSimSetActive(sim);
    return sim;
}

/* ── What the console was told ────────────────────────────────────── */

static void (*sbConsolePrev)(void *ctx, char *msg) = NULL;
static char sbSaid[8192];

static void sbConsoleCb(void *ctx, char *msg) {
    size_t have;
    size_t room;
    size_t n;

    if (sbConsolePrev != NULL) {
        sbConsolePrev(ctx, msg);
    }
    if (msg == NULL) {
        return;
    }
    have = strlen(sbSaid);
    room = sizeof(sbSaid) - 1 - have;
    n    = strlen(msg);
    if (n > room) {
        n = room;
    }
    memcpy(sbSaid + have, msg, n);
    sbSaid[have + n] = '\0';
}

/* Only consoleMessage is replaced, never the ctx beside it, which the sim's
   other callbacks read. */
static void sbWatchConsole(ServerSim *sim) {
    sbSaid[0]     = '\0';
    sbConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = sbConsoleCb;
}

static void sbUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = sbConsolePrev;
    sbConsolePrev = NULL;
}

/* ── 1. The brains land beside the map ────────────────────────────── */

int run_scenario_brains_extracted(void) {
    char          mapPath[1024];
    char          loose[1024];
    char          root[1024];
    char          path[1200];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "brains_extracted.map"));
    sbLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);                  /* nothing beside the map but the map */
    UT_ASSERT_MSG(sbWritePacked(mapPath, kSbManifest, kSbSilent, true),
                  "the packed map fixture could not be written");
    UT_ASSERT(sbRootOf(mapPath, root, sizeof(root)));

    sim = sbRoundSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the packed map was refused: %s", err);

    /* Both of the brain's files, under the brain's own name, byte for byte as
       they went into the container. */
    snprintf(path, sizeof(path), "%s/%s/init.lua", root, SB_BRAIN);
    UT_ASSERT_MSG(sbIsFile(path), "%s is not there after the attach", path);
    UT_ASSERT_MSG(sbFileIs(path, kSbBrainInit, sizeof(kSbBrainInit) - 1),
                  "%s came out with different bytes", path);
    snprintf(path, sizeof(path), "%s/%s/util.lua", root, SB_BRAIN);
    UT_ASSERT_MSG(sbFileIs(path, kSbBrainUtil, sizeof(kSbBrainUtil) - 1),
                  "%s came out with different bytes", path);

    /* And only the brains: the manifest and the script stay inside the
       container, where the host reads them from. */
    snprintf(path, sizeof(path), "%s/main.lua", root);
    UT_ASSERT_MSG(!sbExists(path), "%s was written out as well", path);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    remove(mapPath);
    return 0;
}

/* ── 2. The same map does not extract twice ───────────────────────── */

int run_scenario_brains_same_md5_skips(void) {
    static const char kMark[] = "-- this file was edited after extraction\n";

    char          mapPath[1024];
    char          loose[1024];
    char          first[1024];
    char          second[1024];
    char          path[1200];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "brains_md5.map"));
    sbLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);
    UT_ASSERT_MSG(sbWritePacked(mapPath, kSbManifest, kSbSilent, true),
                  "the packed map fixture could not be written");
    UT_ASSERT(sbRootOf(mapPath, first, sizeof(first)));

    sim = sbRoundSim();
    UT_ASSERT(sim != NULL);
    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the packed map was refused: %s", err);
    scenarioHostDetach(h);

    /* An extracted file, changed on disk. A second attach that re-extracted
       would put the container's bytes back over it. */
    snprintf(path, sizeof(path), "%s/%s/init.lua", first, SB_BRAIN);
    UT_ASSERT_MSG(sbIsFile(path), "%s is not there after the first attach",
                  path);
    UT_ASSERT(sbWriteText(path, kMark));

    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the second attach was refused: %s", err);
    scenarioHostDetach(h);
    UT_ASSERT_MSG(sbFileIs(path, kMark, sizeof(kMark) - 1),
                  "%s was written again: the same map extracted twice", path);

    /* A map file that changed is a different hash and so a different
       directory, and that one is extracted. */
    UT_ASSERT_MSG(sbWritePacked(mapPath, kSbManifest, kSbSilentAgain, true),
                  "the second packed map fixture could not be written");
    UT_ASSERT(sbRootOf(mapPath, second, sizeof(second)));
    UT_ASSERT_MSG(strcmp(first, second) != 0,
                  "a changed map file hashed to the same directory: %s",
                  first);

    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the changed map was refused: %s", err);
    scenarioHostDetach(h);

    snprintf(path, sizeof(path), "%s/%s/init.lua", second, SB_BRAIN);
    UT_ASSERT_MSG(sbFileIs(path, kSbBrainInit, sizeof(kSbBrainInit) - 1),
                  "%s does not hold the container's brain", path);

    /* And the directory the old map file had is left where it is. */
    snprintf(path, sizeof(path), "%s/%s/init.lua", first, SB_BRAIN);
    UT_ASSERT_MSG(sbFileIs(path, kMark, sizeof(kMark) - 1),
                  "the old extraction directory was touched");

    serverSimDestroy(sim);
    remove(mapPath);
    return 0;
}

/* ── 3. A team's package brain reaches the seat as a path ─────────── */

int run_scenario_brains_template_resolved(void) {
    char          mapPath[1024];
    char          loose[1024];
    char          root[1024];
    char          want[1200];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           seat = -1;
    int           i;

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "brains_template.map"));
    sbLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);
    UT_ASSERT_MSG(sbWritePacked(mapPath, kSbTeamManifest, kSbSilent, true),
                  "the packed map fixture could not be written");
    UT_ASSERT(sbRootOf(mapPath, root, sizeof(root)));
    snprintf(want, sizeof(want), "%s/%s/init.lua", root, SB_BRAIN);

    ut_brain_stub_arm(true);
    sim = sbLobbySim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the packed map was refused: %s", err);
    /* What a committed map runs once the map is in. */
    serverSimScenarioSeatLobby(sim);

    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].keepSeat &&
            sim->lobbyPlayers[i].teamNumber == SB_TEAM) {
            seat = i;
            break;
        }
    }
    UT_ASSERT_MSG(seat >= 0, "the template seated nothing on team %d",
                  SB_TEAM);
    UT_ASSERT_MSG(!sim->lobbyPlayers[seat].fielded,
                  "seat %d is on the field; the team asks to be held", seat);

    /* The two things warmSeatBrainPath asks of a seat's brain before it
       builds a runner for it: that it is not a name the sim cannot open, and
       that it is a file on disk. */
    UT_ASSERT_MSG(strncmp(sim->seatBrain[seat], "package:", 8) != 0,
                  "seat %d still holds '%s': the name never became a path",
                  seat, sim->seatBrain[seat]);
    UT_ASSERT_MSG(strcmp(sim->seatBrain[seat], want) == 0,
                  "seat %d holds '%s', expected '%s'", seat,
                  sim->seatBrain[seat], want);
    UT_ASSERT_MSG(sbIsFile(sim->seatBrain[seat]),
                  "seat %d names '%s', which is no file", seat,
                  sim->seatBrain[seat]);

    /* And the countdown's warm builds that seat's runner rather than skipping
       it, which is the whole point of resolving the name this early. */
    UT_ASSERT_MSG(serverSimWarmOneHeldSeat(sim),
                  "the warm built nothing for a seat whose brain resolves");
    UT_ASSERT_MSG(botManagerHasRunner(sim, (BYTE)seat),
                  "seat %d has no runner after the warm", seat);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    ut_brain_stub_arm(false);
    remove(mapPath);
    return 0;
}

/* ── 4. A name the container does not carry ───────────────────────── */

int run_scenario_brains_unknown_name_refused(void) {
    char          mapPath[1024];
    char          loose[1024];
    char          root[1024];
    char          path[1200];
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(utScratchPath(mapPath, sizeof(mapPath), "brains_unknown.map"));
    sbLoosePath(mapPath, loose, sizeof(loose));
    remove(loose);
    UT_ASSERT_MSG(sbWritePacked(mapPath, kSbManifest, kSbSpawnMissing, true),
                  "the packed map fixture could not be written");
    UT_ASSERT(sbRootOf(mapPath, root, sizeof(root)));

    /* The fixture brain is set to succeed, so a spawn that got as far as
       building a bot would, and the case is about the resolution and nothing
       else. */
    ut_brain_stub_arm(true);
    sim = sbRoundSim();
    UT_ASSERT(sim != NULL);
    sbWatchConsole(sim);

    h = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the packed map was refused: %s", err);
    serverSimStartGame(sim);
    /* Three, not one: the script spawns on the first running tick, and a
       spawn that had been accepted would be queued and taken up on the tick
       after, where the roster walk below would see it. */
    serverSimTick(sim);
    serverSimTick(sim);
    serverSimTick(sim);

    UT_ASSERT_MSG(strstr(sbSaid, "brains:refused") != NULL,
                  "the spawn was not refused; the console heard: %s", sbSaid);
    UT_ASSERT_MSG(strstr(sbSaid, "SCN_OP_NOT_FOUND") != NULL,
                  "the refusal was not that the brain could not be found: %s",
                  sbSaid);

    /* No bot went into the roster on the way to that refusal. */
    for (i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(!serverSimIsBot(sim, (BYTE)i),
                      "seat %d holds a bot after a refused spawn", i);
    }

    /* The brain the container does carry is there, and nothing was made for
       the one it does not. */
    snprintf(path, sizeof(path), "%s/%s/init.lua", root, SB_BRAIN);
    UT_ASSERT_MSG(sbIsFile(path), "%s is not there", path);
    snprintf(path, sizeof(path), "%s/NotThere", root);
    UT_ASSERT_MSG(!sbExists(path), "%s was left behind", path);

    sbUnwatchConsole(sim);
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    ut_brain_stub_arm(false);
    remove(mapPath);
    return 0;
}
