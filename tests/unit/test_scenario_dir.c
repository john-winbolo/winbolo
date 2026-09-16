/*
 * The scenarios a server offers on their own, independently of any map.
 *
 * scnDirList reads a directory two ways. A .scenario is a WBSC container held
 * on its own, and its manifest.json is read straight out of it — scnPackageOpen
 * takes a buffer starting at the magic, which a package file does, so no Lua
 * runs to list one. A loose .lua is read through the validator's stub VM: the
 * chunk's top level runs once against a game table that answers nothing, and
 * the scenario table it declares is the manifest. Anything else is skipped.
 *
 * The list then travels as PACKET_LOBBY_SCENARIO_LIST_RSP, chunked the way the
 * map list is. The last case holds the production encoder's bytes against a
 * committed golden array and feeds the same bytes back through the production
 * accumulator, so a symmetric change to both halves still fails.
 *
 * Each case builds its own directory under a per-case name and removes it
 * afterwards: ctest runs the cases as separate processes in one directory, so
 * a shared fixture name is a race rather than a fixture.
 *
 * run_scenario_dir_lists_package       — a .scenario written by scnPackageWrite
 *                                        is listed with its manifest's name,
 *                                        description, cap and bot count
 * run_scenario_dir_lists_loose_script  — a loose .lua declaring a scenario
 *                                        table is listed with the same fields,
 *                                        derived through the stub VM
 * run_scenario_dir_skips_junk          — another extension, and a .lua that is
 *                                        no scenario, are both left out and the
 *                                        rest of the list is unaffected
 * run_scenario_dir_skips_subdirectory  — a .lua one directory down is not in
 *                                        the list, and no entry's file name
 *                                        carries a separator
 * run_scenario_dir_entry_roundtrip     — a list encoded into the RSP shape
 *                                        matches committed golden bytes and
 *                                        decodes back to the same entries,
 *                                        including one whose description fills
 *                                        its length byte
 *
 * Reads the ClientSim struct directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"        /* the scenario list accumulator */
#include "netpacks.h"                   /* PACKET_LOBBY_SCENARIO_LIST_RSP */
#include "transport_udp.h"              /* udpClientHandleLobbyScenarioListRsp */
#include "transport_udp_internal.h"     /* PACKET_HEADER_SIZE, UDP_MAX_PAYLOAD */
#include "transport_udp_server_internal.h" /* udpServerPackScenarioListChunk */
#include "scenario_defs.h"              /* ScnDirEntry */
#include "scenario_dir.h"
#include "scenario_package.h"
#include "test_harness.h"

/* ── The directory ────────────────────────────────────────────────── */

static char sdDir[256];

/* Everything in path, and then path itself. One level down as well, because a
 * case builds a subdirectory in here. */
static void sdRemoveTree(const char *path) {
    char **names;
    int    count = 0;
    int    i;

    names = SDL_GlobDirectory(path, "*", 0, &count);
    if (names != NULL) {
        for (i = 0; i < count; i++) {
            char child[512];

            if (names[i] == NULL || names[i][0] == '\0') continue;
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
            if (remove(child) != 0) {
                /* A directory rather than a file: empty it and take it. */
                char **inner;
                int    innerCount = 0;
                int    j;

                inner = SDL_GlobDirectory(child, "*", 0, &innerCount);
                if (inner != NULL) {
                    for (j = 0; j < innerCount; j++) {
                        char grandchild[640];

                        if (inner[j] == NULL || inner[j][0] == '\0') continue;
                        snprintf(grandchild, sizeof(grandchild), "%s/%s", child,
                                 inner[j]);
                        remove(grandchild);
                    }
                    SDL_free(inner);
                }
                SDL_RemovePath(child);
            }
        }
        SDL_free(names);
    }
    SDL_RemovePath(path);
}

static bool sdMakeDir(const char *tag) {
    snprintf(sdDir, sizeof(sdDir), "wbtest_scenario_dir_%s", tag);
    /* Emptied rather than merely removed. A run that failed part way through
       never reaches its own cleanup, and SDL_RemovePath will not take a
       directory that still holds files — so without this a case that counts
       what it listed counts the previous run's leftovers too. */
    sdRemoveTree(sdDir);
    return SDL_CreateDirectory(sdDir);
}

/* One file in the directory. The cleanup takes whatever is in there rather
 * than a list of what was put there, so nothing is recorded here. */
static bool sdWrite(const char *name, const void *bytes, size_t len) {
    char  path[512];
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), "%s/%s", sdDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    ok = (len == 0) || (fwrite(bytes, 1, len, f) == len);
    fclose(f);
    return ok;
}

static bool sdWriteText(const char *name, const char *text) {
    return sdWrite(name, text, strlen(text));
}

static void sdCleanup(void) {
    sdRemoveTree(sdDir);
}

/* The entry with this file name, or NULL when the list does not hold one. */
static const ScnDirEntry *sdFind(const ScnDirEntry *list, int count,
                                 const char *file) {
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(list[i].file, file) == 0) return &list[i];
    }
    return NULL;
}

/* Every file name in the list on one line, so a failure says what was listed
 * rather than only what was missing. */
static void sdNames(const ScnDirEntry *list, int count, char *out,
                    size_t outLen) {
    int    i;
    size_t at = 0;

    out[0] = '\0';
    for (i = 0; i < count && at + 1 < outLen; i++) {
        at += (size_t)snprintf(out + at, outLen - at, "[%s] ", list[i].file);
    }
}

/* ── The two kinds of file ────────────────────────────────────────── */

/* A manifest naming two teams, so the bot count is a sum and not one team's
 * number. 4 + 2 = 6 seats, a cap of 6 humans, and not bound to any map. */
static const char kSdManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"api\": 1,\n"
    "  \"name\": \"Packed Raid\",\n"
    "  \"description\": \"Raiders from the north\",\n"
    "  \"bound\": false,\n"
    "  \"lobby\": {\n"
    "    \"max_players\": 6,\n"
    "    \"teams\": [\n"
    "      { \"id\": 2, \"bots\": 4 },\n"
    "      { \"id\": 3, \"bots\": 2 }\n"
    "    ]\n"
    "  },\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

static const char kSdPackedScript[] =
    "-- the script the container carries\n"
    "function on_setup() end\n";

/* A loose script declaring its own table: 2 + 3 = 5 seats and a cap of 4. */
static const char kSdLooseScript[] =
    "scenario = {\n"
    "  name = \"Loose Hold\",\n"
    "  description = \"Hold the keep\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  lobby = {\n"
    "    max_players = 4,\n"
    "    teams = {\n"
    "      { id = 1, bots = 2 },\n"
    "      { id = 2, bots = 3 },\n"
    "    },\n"
    "  },\n"
    "}\n";

/* Lua that runs and declares no scenario table, so it is no scenario. */
static const char kSdPlainScript[] =
    "local helper = {}\n"
    "function helper.add(a, b) return a + b end\n"
    "return helper\n";

/* One .scenario file, written the way -pack writes a container. */
static bool sdWritePackage(const char *name, const char *manifest,
                           const char *script) {
    ScnPackageEntry entries[2];
    uint8_t        *bytes = NULL;
    size_t          len   = 0;
    char            err[256];
    bool            ok;

    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)manifest;
    entries[0].len   = strlen(manifest);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)script;
    entries[1].len   = strlen(script);

    err[0] = '\0';
    if (!scnPackageWrite(entries, 2, &bytes, &len, err, sizeof(err))) {
        return false;
    }
    ok = sdWrite(name, bytes, len);
    free(bytes);
    return ok;
}

/* ── 1. A .scenario package ───────────────────────────────────────── */

int run_scenario_dir_lists_package(void) {
    ScnDirEntry        list[8];
    const ScnDirEntry *e;
    char               seen[512];
    int                n;

    UT_ASSERT(sdMakeDir("package"));
    UT_ASSERT_MSG(sdWritePackage("raid.scenario", kSdManifest,
                                 kSdPackedScript),
                  "the package fixture could not be written");

    n = scnDirList(sdDir, list, 8);
    sdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));
    UT_ASSERT_MSG(n == 1, "%d entries listed, expected 1: %s", n, seen);

    e = sdFind(list, n, "raid.scenario");
    UT_ASSERT_MSG(e != NULL, "the package is not in the list: %s", seen);
    UT_ASSERT_MSG(strcmp(e->name, "Packed Raid") == 0,
                  "name read as '%s'", e->name);
    UT_ASSERT_MSG(strcmp(e->description, "Raiders from the north") == 0,
                  "description read as '%s'", e->description);
    UT_ASSERT_MSG(e->maxPlayers == 6, "max_players read as %u",
                  (unsigned)e->maxPlayers);
    /* The sum over the teams, which is what says the count is not one team's
       own number. */
    UT_ASSERT_MSG(e->bots == 6, "bots read as %u, expected 4 + 2",
                  (unsigned)e->bots);
    UT_ASSERT_MSG(!e->bound, "a manifest saying bound false read as bound");

    sdCleanup();
    return 0;
}

/* ── 2. A loose script ────────────────────────────────────────────── */

int run_scenario_dir_lists_loose_script(void) {
    ScnDirEntry        list[8];
    const ScnDirEntry *e;
    char               seen[512];
    int                n;

    UT_ASSERT(sdMakeDir("loose"));
    UT_ASSERT(sdWriteText("hold.lua", kSdLooseScript));

    n = scnDirList(sdDir, list, 8);
    sdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));
    UT_ASSERT_MSG(n == 1, "%d entries listed, expected 1: %s", n, seen);

    e = sdFind(list, n, "hold.lua");
    UT_ASSERT_MSG(e != NULL, "the loose script is not in the list: %s", seen);
    UT_ASSERT_MSG(strcmp(e->name, "Loose Hold") == 0, "name read as '%s'",
                  e->name);
    UT_ASSERT_MSG(strcmp(e->description, "Hold the keep") == 0,
                  "description read as '%s'", e->description);
    UT_ASSERT_MSG(e->maxPlayers == 4, "max_players read as %u",
                  (unsigned)e->maxPlayers);
    UT_ASSERT_MSG(e->bots == 5, "bots read as %u, expected 2 + 3",
                  (unsigned)e->bots);
    UT_ASSERT_MSG(!e->bound, "a table saying bound false read as bound");

    sdCleanup();
    return 0;
}

/* ── 3. What is not a scenario ────────────────────────────────────── */

int run_scenario_dir_skips_junk(void) {
    ScnDirEntry list[8];
    char        seen[512];
    int         n;

    UT_ASSERT(sdMakeDir("junk"));
    /* Two that belong in the list and two that do not. */
    UT_ASSERT(sdWriteText("hold.lua", kSdLooseScript));
    UT_ASSERT(sdWritePackage("raid.scenario", kSdManifest, kSdPackedScript));
    UT_ASSERT(sdWriteText("notes.txt", "not a scenario, and not Lua either\n"));
    UT_ASSERT(sdWriteText("helper.lua", kSdPlainScript));

    n = scnDirList(sdDir, list, 8);
    sdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));
    UT_ASSERT_MSG(n == 2, "%d entries listed, expected 2: %s", n, seen);
    UT_ASSERT_MSG(sdFind(list, n, "hold.lua") != NULL,
                  "the loose script fell out of the list: %s", seen);
    UT_ASSERT_MSG(sdFind(list, n, "raid.scenario") != NULL,
                  "the package fell out of the list: %s", seen);
    UT_ASSERT_MSG(sdFind(list, n, "notes.txt") == NULL,
                  "a .txt was listed as a scenario: %s", seen);
    UT_ASSERT_MSG(sdFind(list, n, "helper.lua") == NULL,
                  "Lua that declares no scenario table was listed: %s", seen);

    /* And the two that were listed still carry what their files said, so the
       skipped ones cost the others nothing. */
    UT_ASSERT(strcmp(sdFind(list, n, "hold.lua")->name, "Loose Hold") == 0);
    UT_ASSERT(strcmp(sdFind(list, n, "raid.scenario")->name,
                     "Packed Raid") == 0);

    /* File-name order: "hold.lua" before "raid.scenario". */
    UT_ASSERT_MSG(strcmp(list[0].file, "hold.lua") == 0,
                  "the list is not in file-name order: %s", seen);

    sdCleanup();
    return 0;
}

/* ── 4. A file one directory down ─────────────────────────────────── */

/* SDL's match-everything walk descends into subdirectories and hands back what
 * it finds there as "sub/x.lua". ScnDirEntry.file is a name in the scenarios
 * directory and never a path, so such a file is not one this list offers. */
int run_scenario_dir_skips_subdirectory(void) {
    ScnDirEntry list[8];
    char        seen[512];
    char        subDir[512];
    char        subFile[640];
    FILE       *f;
    int         n;
    int         i;

    UT_ASSERT(sdMakeDir("subdir"));
    UT_ASSERT(sdWriteText("hold.lua", kSdLooseScript));

    snprintf(subDir, sizeof(subDir), "%s/sub", sdDir);
    UT_ASSERT_MSG(SDL_CreateDirectory(subDir),
                  "the subdirectory fixture could not be made: %s",
                  SDL_GetError());
    snprintf(subFile, sizeof(subFile), "%s/buried.lua", subDir);
    f = fopen(subFile, "wb");
    UT_ASSERT_MSG(f != NULL, "the buried script could not be written");
    fputs(kSdLooseScript, f);
    fclose(f);

    n = scnDirList(sdDir, list, 8);
    sdNames(list, (n > 0) ? n : 0, seen, sizeof(seen));
    UT_ASSERT_MSG(n == 1,
                  "%d entries listed, expected the one file in the directory "
                  "itself: %s", n, seen);
    UT_ASSERT_MSG(sdFind(list, n, "hold.lua") != NULL,
                  "the loose script fell out of the list: %s", seen);
    UT_ASSERT_MSG(sdFind(list, n, "buried.lua") == NULL,
                  "a script one directory down was listed: %s", seen);

    /* And not under a path either, which is the shape it would arrive in. */
    for (i = 0; i < n; i++) {
        UT_ASSERT_MSG(strchr(list[i].file, '/') == NULL &&
                      strchr(list[i].file, '\\') == NULL,
                      "entry %d is \"%s\", which is a path and not a name in "
                      "the directory", i, list[i].file);
    }

    sdCleanup();
    return 0;
}

/* ── 5. The second read of a directory nothing moved ──────────────── */

}

/* ── 6. The chunk, byte for byte and back ─────────────────────────── */

/* The two entries the golden bytes below describe. */
#define SD_E0_FILE "alpha.scenario"
#define SD_E0_NAME "Alpha"
#define SD_E0_DESC "First"
#define SD_E1_FILE "beta.lua"
#define SD_E1_NAME "Beta"
#define SD_E1_DESC "Second"

/* What udpServerPackScenarioListChunk must write after the 8-byte packet
 * header for those two, in wire order:
 *   [final][count] then per entry
 *   [fileLen][file][nameLen][name][descLen][desc][maxPlayers][bots][bound]
 *
 * Committed rather than computed: a round trip passes even when both halves
 * change together, and these bytes are what catches a wire change nobody
 * meant. The packet header is another layer's and is not pinned here. */
static const uint8_t kSdGolden[] = {
    0x01, 0x02,                             /* final = 1, count = 2      */
    0x0E, 'a','l','p','h','a','.','s','c','e','n','a','r','i','o',
    0x05, 'A','l','p','h','a',
    0x05, 'F','i','r','s','t',
    0x08, 0x03, 0x00,                       /* maxPlayers, bots, bound   */
    0x08, 'b','e','t','a','.','l','u','a',
    0x04, 'B','e','t','a',
    0x06, 'S','e','c','o','n','d',
    0x00, 0x00, 0x01                        /* maxPlayers, bots, bound   */
};

static void sdFill(ScnDirEntry *e, const char *file, const char *name,
                   const char *desc, uint8_t maxPlayers, uint8_t bots,
                   bool bound) {
    memset(e, 0, sizeof(*e));
    SDL_strlcpy(e->file, file, sizeof(e->file));
    SDL_strlcpy(e->name, name, sizeof(e->name));
    SDL_strlcpy(e->description, desc, sizeof(e->description));
    e->maxPlayers = maxPlayers;
    e->bots       = bots;
    e->bound      = bound;
}

static ClientSim *sdFreshClientSim(void) {
    ClientSim *cs = clientSimAlloc();

    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    return cs;
}

int run_scenario_dir_entry_roundtrip(void) {
    ScnDirEntry entries[3];
    uint8_t     buf[UDP_MAX_PAYLOAD];
    char        longDesc[SCN_DIR_DESC_LEN];
    ClientSim  *cs;
    int         len;
    int         next = 0;
    int         i;

    sdFill(&entries[0], SD_E0_FILE, SD_E0_NAME, SD_E0_DESC, 8, 3, false);
    sdFill(&entries[1], SD_E1_FILE, SD_E1_NAME, SD_E1_DESC, 0, 0, true);

    /* ── The golden bytes ─────────────────────────────────────────── */
    memset(buf, 0, sizeof(buf));
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), entries, 2, 0,
                                         &next);
    UT_ASSERT_MSG(len == PACKET_HEADER_SIZE + (int)sizeof(kSdGolden),
                  "the chunk is %d bytes, expected %d", len,
                  PACKET_HEADER_SIZE + (int)sizeof(kSdGolden));
    UT_ASSERT_MSG(next == 2, "the encoder stopped at entry %d, expected 2",
                  next);
    for (i = 0; i < (int)sizeof(kSdGolden); i++) {
        UT_ASSERT_MSG(buf[PACKET_HEADER_SIZE + i] == kSdGolden[i],
                      "byte %d of the payload is 0x%02X, expected 0x%02X",
                      i, (unsigned)buf[PACKET_HEADER_SIZE + i],
                      (unsigned)kSdGolden[i]);
    }

    /* ── And back through the client's accumulator ────────────────── */
    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    cs->lobbyScenarioListInFlight = true;
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);

    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 2,
                  "the two-entry chunk read as %d entries",
                  cs->lobbyScenarioListCount);
    UT_ASSERT_MSG(cs->lobbyScenarioListReady,
                  "a final chunk did not make the list ready");
    UT_ASSERT_MSG(!cs->lobbyScenarioListInFlight,
                  "a final chunk left the request in flight");
    UT_ASSERT(strcmp(cs->lobbyScenarioListFiles[0], SD_E0_FILE) == 0);
    UT_ASSERT(strcmp(cs->lobbyScenarioListNames[0], SD_E0_NAME) == 0);
    UT_ASSERT(strcmp(cs->lobbyScenarioListDescs[0], SD_E0_DESC) == 0);
    UT_ASSERT_MSG(cs->lobbyScenarioListMaxPlayers[0] == 8,
                  "entry 0's max_players read as %u",
                  (unsigned)cs->lobbyScenarioListMaxPlayers[0]);
    UT_ASSERT_MSG(cs->lobbyScenarioListBots[0] == 3,
                  "entry 0's bots read as %u",
                  (unsigned)cs->lobbyScenarioListBots[0]);
    UT_ASSERT_MSG(!cs->lobbyScenarioListBound[0],
                  "entry 0 read as bound");
    UT_ASSERT(strcmp(cs->lobbyScenarioListFiles[1], SD_E1_FILE) == 0);
    UT_ASSERT(strcmp(cs->lobbyScenarioListNames[1], SD_E1_NAME) == 0);
    UT_ASSERT(strcmp(cs->lobbyScenarioListDescs[1], SD_E1_DESC) == 0);
    UT_ASSERT_MSG(cs->lobbyScenarioListBound[1],
                  "entry 1 read as unbound, and the fixture says bound");
    /* Read back through the public accessors too, which is how a chooser will
       see it. */
    UT_ASSERT(strcmp(clientSimGetLobbyScenarioListFile(cs, 0),
                     SD_E0_FILE) == 0);
    UT_ASSERT(clientSimGetLobbyScenarioListBots(cs, 0) == 3);
    UT_ASSERT(clientSimGetLobbyScenarioListCount(cs) == 2);
    UT_ASSERT(clientSimGetLobbyScenarioListReady(cs));
    /* An index off the end answers rather than reading past the list. */
    UT_ASSERT(clientSimGetLobbyScenarioListFile(cs, 2)[0] == '\0');
    UT_ASSERT(clientSimGetLobbyScenarioListFile(cs, -1)[0] == '\0');
    clientSimDestroy(cs);

    /* ── A description filling its length byte ────────────────────── */
    /* SCN_DIR_DESC_LEN is 256, so the longest description a scenario can
       hold is 255 bytes — exactly what one length byte carries. This is that
       boundary: the whole of it has to survive the trip. The encoder's cut to
       255 is the defence if that constant ever grows past what the byte can
       say, and it cannot be reached from here. */
    memset(longDesc, 'x', sizeof(longDesc) - 1);
    longDesc[sizeof(longDesc) - 1] = '\0';
    UT_ASSERT_MSG(strlen(longDesc) == 255,
                  "the fixture description is %d bytes, expected 255",
                  (int)strlen(longDesc));
    sdFill(&entries[2], "wide.lua", "Wide", longDesc, 16, 255, false);

    memset(buf, 0, sizeof(buf));
    next = 0;
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), &entries[2], 1,
                                         0, &next);
    UT_ASSERT(next == 1);
    UT_ASSERT_MSG(buf[PACKET_HEADER_SIZE + 1] == 1,
                  "the one-entry chunk says count %u",
                  (unsigned)buf[PACKET_HEADER_SIZE + 1]);

    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 1,
                  "the wide entry read as %d entries",
                  cs->lobbyScenarioListCount);
    UT_ASSERT_MSG(strlen(cs->lobbyScenarioListDescs[0]) == 255,
                  "the 255-byte description arrived as %d bytes",
                  (int)strlen(cs->lobbyScenarioListDescs[0]));
    UT_ASSERT(strcmp(cs->lobbyScenarioListDescs[0], longDesc) == 0);
    UT_ASSERT(cs->lobbyScenarioListBots[0] == 255);
    clientSimDestroy(cs);

    /* ── An empty list is one chunk that says so ──────────────────── */
    memset(buf, 0, sizeof(buf));
    next = -1;
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), entries, 0, 0,
                                         &next);
    UT_ASSERT_MSG(len == PACKET_HEADER_SIZE + 2,
                  "an empty chunk is %d bytes, expected %d", len,
                  PACKET_HEADER_SIZE + 2);
    UT_ASSERT_MSG(buf[PACKET_HEADER_SIZE] == 1,
                  "an empty chunk did not set final");
    UT_ASSERT_MSG(buf[PACKET_HEADER_SIZE + 1] == 0,
                  "an empty chunk says count %u",
                  (unsigned)buf[PACKET_HEADER_SIZE + 1]);
    UT_ASSERT(next == 0);

    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    cs->lobbyScenarioListInFlight = true;
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT(cs->lobbyScenarioListCount == 0);
    UT_ASSERT_MSG(cs->lobbyScenarioListReady,
                  "an empty list never became ready");
    UT_ASSERT(!cs->lobbyScenarioListInFlight);
    clientSimDestroy(cs);

    /* ── A chunk cut short keeps the entry out ────────────────────── */
    memset(buf, 0, sizeof(buf));
    next = 0;
    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), entries, 2, 0,
                                         &next);
    cs = sdFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len - 1);
    UT_ASSERT_MSG(cs->lobbyScenarioListCount == 1,
                  "a chunk one byte short read as %d entries, expected the "
                  "first alone", cs->lobbyScenarioListCount);
    clientSimDestroy(cs);
    return 0;
}
