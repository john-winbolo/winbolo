/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * A copy of a server's script, saved to the player's own Mods directory by
 * scenarioHostSaveLocalScript.
 *
 * A .lua and a .scenario land byte for byte through a temporary file that is
 * gone afterwards, are found by scenarioHostLocalScriptPath, and the .lua is
 * listed straight away. A name the directory already holds, in the same case
 * or another, is refused and the file there is left alone; the chooser asks
 * scenarioHostLocalScriptPath the same question before it sends the fetch at
 * all, so a name refused here is one the lobby never asks the server for. A
 * name that is not a bare .lua or .scenario file name writes nothing, inside
 * the directory or out of it. A Mods directory that is not there yet is made.
 *
 * WB_MOD_DIR_USER points the directory at this case's scratch path, and is
 * set only for the length of each call so a failed case leaves the
 * environment as it found it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "server_sim.h"
#include "scenario_host.h"
#include "test_harness.h"

#define SSL_MAX 16

static char sslModDir[512];
static char sslWorkshopDir[512];

static void sslSetEnv(const char *key, const char *val) {
#ifdef _WIN32
    _putenv_s(key, val != NULL ? val : "");
#else
    if (val != NULL) setenv(key, val, 1);
    else unsetenv(key);
#endif
}

/* The Workshop directory beside Mods, named so the real one is never read
   and left uncreated, so it holds nothing and the scratch path holds only
   what each case makes. */
static const char *sslWorkshop(void) {
    if (!utScratchPath(sslWorkshopDir, sizeof(sslWorkshopDir), "Workshop")) {
        snprintf(sslWorkshopDir, sizeof(sslWorkshopDir), "%s-Workshop",
                 sslModDir);
    }
    return sslWorkshopDir;
}

static ScenarioLocalSaveResult sslSave(const char *file, const void *bytes,
                                       size_t len) {
    ScenarioLocalSaveResult r;

    sslSetEnv("WB_MOD_DIR_USER", sslModDir);
    sslSetEnv("WB_MOD_DIR_WORKSHOP", sslWorkshop());
    r = scenarioHostSaveLocalScript(file, (const uint8_t *)bytes, len);
    sslSetEnv("WB_MOD_DIR_USER", NULL);
    sslSetEnv("WB_MOD_DIR_WORKSHOP", NULL);
    return r;
}

static bool sslPath(const char *file, char *out, size_t outLen) {
    bool ok;

    sslSetEnv("WB_MOD_DIR_USER", sslModDir);
    sslSetEnv("WB_MOD_DIR_WORKSHOP", sslWorkshop());
    ok = scenarioHostLocalScriptPath(file, out, outLen);
    sslSetEnv("WB_MOD_DIR_USER", NULL);
    sslSetEnv("WB_MOD_DIR_WORKSHOP", NULL);
    return ok;
}

static int sslList(ServerScenarioEntry *out, int max) {
    int n;

    sslSetEnv("WB_MOD_DIR_USER", sslModDir);
    sslSetEnv("WB_MOD_DIR_WORKSHOP", sslWorkshop());
    n = scenarioHostListLocalScripts(out, max);
    sslSetEnv("WB_MOD_DIR_USER", NULL);
    sslSetEnv("WB_MOD_DIR_WORKSHOP", NULL);
    return n;
}

/* Whether the file at dir/file holds exactly want. */
static bool sslFileIs(const char *dir, const char *file, const void *want,
                      size_t wantLen) {
    char   path[768];
    void  *got;
    size_t gotLen = 0;
    bool   same;

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    got = SDL_LoadFile(path, &gotLen);
    if (got == NULL) return false;
    same = gotLen == wantLen &&
           (wantLen == 0 || memcmp(got, want, wantLen) == 0);
    SDL_free(got);
    return same;
}

/* How many entries dir holds, and how many of them start with a dot. */
static int sslCount(const char *dir, int *dotted) {
    char **names;
    int    count = 0;
    int    i;

    if (dotted != NULL) *dotted = 0;
    names = SDL_GlobDirectory(dir, "*", 0, &count);
    if (names == NULL) return 0;
    for (i = 0; i < count; i++) {
        if (dotted != NULL && names[i] != NULL && names[i][0] == '.') {
            (*dotted)++;
        }
    }
    SDL_free(names);
    return count;
}

static const char kSslMod[] =
    "scenario = {\n"
    "  name = \"Copied\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n";

/* A .lua and a .scenario land whole, leave no temporary file, are found by
   name, and the .lua is listed on the very next listing even though one was
   read just before it landed. */
int run_script_save_local_ok(void) {
    static const uint8_t pkg[] = { 'P', 'K', 0x03, 0x04, 0x00, 0xff, 0x7f };
    ServerScenarioEntry  list[SSL_MAX];
    char                 path[1024];
    int                  dotted;
    int                  n;
    int                  i;
    bool                 listed = false;

    UT_ASSERT(utScratchPath(sslModDir, sizeof(sslModDir), "Mods"));
    UT_ASSERT(SDL_CreateDirectory(sslModDir));
    n = sslList(list, SSL_MAX);
    UT_ASSERT_MSG(n == 0, "%d rows in an empty directory", n);

    UT_ASSERT(sslSave("copied.lua", kSslMod, strlen(kSslMod)) ==
              SCENARIO_LOCAL_SAVE_OK);
    UT_ASSERT(sslSave("pack.scenario", pkg, sizeof(pkg)) ==
              SCENARIO_LOCAL_SAVE_OK);

    UT_ASSERT(sslFileIs(sslModDir, "copied.lua", kSslMod, strlen(kSslMod)));
    UT_ASSERT(sslFileIs(sslModDir, "pack.scenario", pkg, sizeof(pkg)));
    n = sslCount(sslModDir, &dotted);
    UT_ASSERT_MSG(n == 2, "%d entries, wanted the two files", n);
    UT_ASSERT_MSG(dotted == 0, "%d temporary files left behind", dotted);

    UT_ASSERT(sslPath("copied.lua", path, sizeof(path)));
    UT_ASSERT(sslPath("pack.scenario", path, sizeof(path)));

    n = sslList(list, SSL_MAX);
    for (i = 0; i < n; i++) {
        if (strcmp(list[i].file, "copied.lua") == 0) listed = true;
    }
    UT_ASSERT_MSG(listed, "copied.lua not in the %d listed rows", n);

    /* A file of no bytes is still a file. */
    UT_ASSERT(sslSave("empty.lua", NULL, 0) == SCENARIO_LOCAL_SAVE_OK);
    UT_ASSERT(sslFileIs(sslModDir, "empty.lua", "", 0));
    /* No bytes with a length is not. */
    UT_ASSERT(sslSave("nothing.lua", NULL, 4) == SCENARIO_LOCAL_SAVE_WRITE);
    UT_ASSERT(!sslPath("nothing.lua", path, sizeof(path)));
    return 0;
}

/* A name already there, in the same case or another, is refused and the
   file there keeps its bytes. */
int run_script_save_local_exists(void) {
    static const char original[] = "-- the player's own\n";
    static const char other[]    = "-- the server's copy\n";
    char              path[768];
    FILE             *f;
    int               n;

    UT_ASSERT(utScratchPath(sslModDir, sizeof(sslModDir), "Mods"));
    UT_ASSERT(SDL_CreateDirectory(sslModDir));
    snprintf(path, sizeof(path), "%s/Taken.lua", sslModDir);
    f = fopen(path, "wb");
    UT_ASSERT(f != NULL);
    UT_ASSERT(fwrite(original, 1, strlen(original), f) == strlen(original));
    UT_ASSERT(fclose(f) == 0);

    UT_ASSERT(sslSave("Taken.lua", other, strlen(other)) ==
              SCENARIO_LOCAL_SAVE_EXISTS);
    UT_ASSERT(sslSave("taken.lua", other, strlen(other)) ==
              SCENARIO_LOCAL_SAVE_EXISTS);
    UT_ASSERT(sslSave("TAKEN.LUA", other, strlen(other)) ==
              SCENARIO_LOCAL_SAVE_EXISTS);

    UT_ASSERT(sslFileIs(sslModDir, "Taken.lua", original, strlen(original)));
    n = sslCount(sslModDir, NULL);
    UT_ASSERT_MSG(n == 1, "%d entries, wanted the one file", n);
    return 0;
}

/* Every name that is not a bare .lua or .scenario file name is refused, and
   nothing lands in the directory or in the one above it. */
int run_script_save_local_bad_name(void) {
    static const char bytes[] = "-- anything\n";
    char              parent[512];
    char              longName[SSL_MAX * 16];
    const char       *names[] = {
        "../x.lua", "a/b.lua", "a\\b.lua", ".hidden.lua", "x.txt", "",
        "CON.lua", "con.lua", "LPT1.scenario", "CON.x.lua", "a\x01" "b.lua",
        "c:d.lua", ".lua", NULL,
    };
    size_t            i;
    int               n;

    UT_ASSERT(utScratchPath(parent, sizeof(parent), NULL));
    UT_ASSERT(utScratchPath(sslModDir, sizeof(sslModDir), "Mods"));
    UT_ASSERT(SDL_CreateDirectory(sslModDir));

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        ScenarioLocalSaveResult r = sslSave(names[i], bytes, strlen(bytes));

        UT_ASSERT_MSG(r == SCENARIO_LOCAL_SAVE_BAD_NAME,
                      "name %u answered %d", (unsigned)i, (int)r);
    }

    /* One byte over what a listing row holds, ending in .lua. */
    memset(longName, 'a', sizeof(longName));
    SDL_strlcpy(longName + 124, ".lua", sizeof(longName) - 124);
    UT_ASSERT(strlen(longName) == 128);
    UT_ASSERT(sslSave(longName, bytes, strlen(bytes)) ==
              SCENARIO_LOCAL_SAVE_BAD_NAME);

    n = sslCount(sslModDir, NULL);
    UT_ASSERT_MSG(n == 0, "%d entries in Mods, wanted none", n);
    n = sslCount(parent, NULL);
    UT_ASSERT_MSG(n == 1, "%d entries beside Mods, wanted Mods alone", n);
    return 0;
}

/* A Mods directory that is not there yet is made, and the copy lands in it. */
int run_script_save_local_creates_dir(void) {
    char         path[1024];
    SDL_PathInfo info;

    UT_ASSERT(utScratchPath(sslModDir, sizeof(sslModDir), "Mods"));
    UT_ASSERT(!SDL_GetPathInfo(sslModDir, &info));

    UT_ASSERT(sslSave("fresh.lua", kSslMod, strlen(kSslMod)) ==
              SCENARIO_LOCAL_SAVE_OK);
    UT_ASSERT(SDL_GetPathInfo(sslModDir, &info) &&
              info.type == SDL_PATHTYPE_DIRECTORY);
    UT_ASSERT(sslFileIs(sslModDir, "fresh.lua", kSslMod, strlen(kSslMod)));
    UT_ASSERT(sslPath("fresh.lua", path, sizeof(path)));
    return 0;
}
