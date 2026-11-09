/*
 * The guards on the Bot Host inside a client (bot_host_guard.c, the noCode
 * manifest flag and install check of brain_sync.c, and the limited capacity
 * question of brain_capacity_lua.c).
 *
 *   1. bot_host_official_no_files: a manifest with noCode keeps the flag
 *      through the wire, and an unknown flag bit is refused; a pack request
 *      that asks for no file and takes every file from the install puts the
 *      brain in place, and one that asks for any file does not; a host's
 *      install with a different brain does not match, so it offers 0 slots.
 *   2. bot_host_token_allow_list: cfg= and other tokens off the list are
 *      refused; the stock difficulty, mode and preset tokens pass, in an
 *      init arg and in an init table.
 *   3. bot_host_unculled_not_player: a bot that sees the whole map never
 *      goes to a player's host; it may go to a standalone host; an
 *      official-only host needs the brain in place.
 *   4. bot_host_rec_upload_asked: a recording upload is taken only for a
 *      block the server asked that host for.
 *   5. bot_host_salt_limit: a connection gets few same-computer salts, and
 *      not two inside the gap.
 *   6. brain_capacity_throwing_index: a brain whose "brain" table throws
 *      from __index, or whose capacity() loops, gives the default and leaves
 *      the stack and the caller's hook as they were.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "bot_host_guard.h"
#include "brain_capacity.h"
#include "brain_pack.h"
#include "brain_sync.h"
#include "scenario_table.h"
#include "test_harness.h"

static bool sec_write(const char *dir, const char *rel, const char *text) {
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s/%s", dir, rel);
    return brainSyncWriteFile(path, text, strlen(text));
}

static bool sec_fill_brain(const char *dir, const char *initText) {
    return sec_write(dir, "init.lua", initText) &&
           sec_write(dir, "lib/util.lua", "return { n = 1 }\n");
}

/* ================================================================
 * 1. Official mode: no files, and the install must match
 * ================================================================ */
int run_bot_host_official_no_files(void) {
    BrainSyncManifest m, d;
    char root[1024], serverDir[1024], installDir[1024];
    uint8_t bits[BRAIN_PACK_BITMAP_BYTES];
    uint8_t *blob;
    size_t len = 0;

    UT_ASSERT(utScratchPath(root, sizeof(root), "secinstall"));
    SDL_snprintf(installDir, sizeof(installDir), "%s/TestBrain", root);
    UT_ASSERT(utScratchPath(serverDir, sizeof(serverDir), "secserver"));
    UT_ASSERT(sec_fill_brain(serverDir, "print('server')\n"));
    UT_ASSERT(sec_fill_brain(installDir, "print('server')\n"));

    memset(&m, 0, sizeof(m));
    m.haveSha = true;
    m.noCode = true;
    UT_ASSERT(brainSyncCollectBrain(&m.brains[0], "TestBrain", serverDir,
                                    false, NULL));
    m.brainCount = 1;
    brainSyncComputeVersions(&m);

    /* The flag keeps through the wire, and is not part of the version. */
    blob = brainSyncManifestEncodeV2(&m, &len);
    UT_ASSERT(blob != NULL && len > 4);
    UT_ASSERT(brainSyncManifestDecode(blob, len, &d));
    UT_ASSERT(d.noCode);
    UT_ASSERT(!d.debugMode);
    UT_ASSERT(d.version == m.version);
    brainSyncFree(&d);
    /* An unknown flag bit is refused. */
    blob[3] |= 0x04u;
    UT_ASSERT(!brainSyncManifestDecode(blob, len, &d));
    free(blob);

    /* The same brain in the install: in place, nothing to send. */
    UT_ASSERT(brainSyncInstallMatches(root, &m.brains[0]));
    memset(bits, 0, sizeof(bits));
    UT_ASSERT(botHostPackReqAllInstall(bits, sizeof(bits),
                                       (uint16_t)m.brains[0].fileCount,
                                       (uint16_t)m.brains[0].fileCount));
    /* A request for any file, or a file from somewhere else, is not. */
    UT_ASSERT(!botHostPackReqAllInstall(bits, sizeof(bits),
                                        (uint16_t)m.brains[0].fileCount,
                                        (uint16_t)(m.brains[0].fileCount - 1)));
    brainPackBitSet(bits, 0);
    UT_ASSERT(!botHostPackReqAllInstall(bits, sizeof(bits),
                                        (uint16_t)m.brains[0].fileCount,
                                        (uint16_t)m.brains[0].fileCount));
    UT_ASSERT(!botHostPackReqAllInstall(NULL, 0, 0, 0));

    /* A different brain on the server: the install does not match. The
     * host then takes no file and offers 0 slots (bothost_main.c
     * syncInstallOnlyManifest and syncOffersNone). */
    UT_ASSERT(sec_write(serverDir, "init.lua", "print('changed')\n"));
    brainSyncFree(&m);
    memset(&m, 0, sizeof(m));
    m.haveSha = true;
    m.noCode = true;
    UT_ASSERT(brainSyncCollectBrain(&m.brains[0], "TestBrain", serverDir,
                                    false, NULL));
    m.brainCount = 1;
    brainSyncComputeVersions(&m);
    UT_ASSERT(!brainSyncInstallMatches(root, &m.brains[0]));
    /* No install folder at all: no match. */
    UT_ASSERT(!brainSyncInstallMatches(NULL, &m.brains[0]));
    brainSyncFree(&m);
    return 0;
}

/* ================================================================
 * 2. Init token allow-list
 * ================================================================ */
int run_bot_host_token_allow_list(void) {
    ScnTable t;

    /* Off the list. */
    UT_ASSERT(!botHostInitTokenAllowed("cfg=EXPAND_WATER_STEP=-5"));
    UT_ASSERT(!botHostInitTokenAllowed("cfg=FOCUS_SPIKE_STDERR_LOG=1"));
    UT_ASSERT(!botHostInitArgAllowed("difficulty=hard;cfg=X=1"));
    UT_ASSERT(!botHostInitArgAllowed("difficulty=hard,cfg=X=1"));
    UT_ASSERT(!botHostInitTokenAllowed("difficulty=../../x"));
    UT_ASSERT(!botHostInitTokenAllowed("difficulty=a_name_that_is_far_too_long_for_it"));
    UT_ASSERT(!botHostInitTokenAllowed("portfolio=1/2"));
    UT_ASSERT(!botHostInitTokenAllowed("portfolio=1/2/3/101"));
    UT_ASSERT(!botHostInitTokenAllowed("blitz=17"));
    UT_ASSERT(!botHostInitTokenAllowed("refuel=-1"));
    UT_ASSERT(!botHostInitTokenAllowed("refuel=11"));
    UT_ASSERT(!botHostInitTokenAllowed("peace=16"));
    UT_ASSERT(!botHostInitTokenAllowed("unknownword"));
    UT_ASSERT(!botHostInitTokenAllowed("script=evil"));

    /* On the list. */
    UT_ASSERT(botHostInitTokenAllowed(""));
    UT_ASSERT(botHostInitTokenAllowed("difficulty=hard"));
    UT_ASSERT(botHostInitTokenAllowed(" mode=turtle "));
    UT_ASSERT(botHostInitTokenAllowed("preset=keel"));
    UT_ASSERT(botHostInitTokenAllowed("portfolio=40/30/20/10"));
    UT_ASSERT(botHostInitTokenAllowed("blitz=default"));
    UT_ASSERT(botHostInitTokenAllowed("blitz=2/4"));
    UT_ASSERT(botHostInitTokenAllowed("blitzsuiciders=3"));
    UT_ASSERT(botHostInitTokenAllowed("refuel=2.5"));
    UT_ASSERT(botHostInitTokenAllowed("peace=1/3/7"));
    UT_ASSERT(botHostInitTokenAllowed("survivor"));
    UT_ASSERT(botHostInitTokenAllowed("noblitz"));
    UT_ASSERT(botHostInitArgAllowed(NULL));
    UT_ASSERT(botHostInitArgAllowed("difficulty=hard;mode=turtle,noblitz"));

    /* The same list for an init table. */
    scnTableClear(&t);
    UT_ASSERT(scnTableSet(&t, "difficulty", "medium"));
    UT_ASSERT(scnTableSet(&t, "survivor", "true"));
    UT_ASSERT(scnTableSet(&t, "horde", "false"));
    UT_ASSERT(botHostInitTableAllowed(&t));
    UT_ASSERT(botHostInitTableAllowed(NULL));
    UT_ASSERT(scnTableSet(&t, "cfg", "EXPAND_WATER_STEP=-5"));
    UT_ASSERT(!botHostInitTableAllowed(&t));
    scnTableClear(&t);
    UT_ASSERT(scnTableSet(&t, "survivor", "x"));
    UT_ASSERT(!botHostInitTableAllowed(&t));
    scnTableClear(&t);
    UT_ASSERT(scnTableSet(&t, "difficulty", "hard;cfg=X=1"));
    UT_ASSERT(!botHostInitTableAllowed(&t));
    return 0;
}

/* ================================================================
 * 3. Unculled bots never on a player's host
 * ================================================================ */
int run_bot_host_unculled_not_player(void) {
    BotHostSeatFit f;

    memset(&f, 0, sizeof(f));
    f.brainInPlace = true;
    f.tokensAllowed = true;

    /* A player's host. */
    f.playerHost = true;
    f.culled = false;
    f.officialOnly = false;
    UT_ASSERT(!botHostSeatMayRunOn(&f));
    f.officialOnly = true;
    UT_ASSERT(!botHostSeatMayRunOn(&f));
    f.culled = true;
    UT_ASSERT(botHostSeatMayRunOn(&f));
    /* Official only: the brain must be in place, the tokens on the list. */
    f.brainInPlace = false;
    UT_ASSERT(!botHostSeatMayRunOn(&f));
    f.brainInPlace = true;
    f.tokensAllowed = false;
    UT_ASSERT(!botHostSeatMayRunOn(&f));
    /* Custom code (the player said yes): the tokens are not checked. */
    f.officialOnly = false;
    UT_ASSERT(botHostSeatMayRunOn(&f));

    /* A standalone host may run an unculled bot and any token. */
    f.playerHost = false;
    f.culled = false;
    f.tokensAllowed = false;
    f.officialOnly = false;
    f.brainInPlace = false;
    UT_ASSERT(botHostSeatMayRunOn(&f));
    /* In official mode it still needs the brain in place. */
    f.officialOnly = true;
    UT_ASSERT(!botHostSeatMayRunOn(&f));
    f.brainInPlace = true;
    UT_ASSERT(botHostSeatMayRunOn(&f));
    UT_ASSERT(!botHostSeatMayRunOn(NULL));
    return 0;
}

/* ================================================================
 * 4. Uploads only for an asked block
 * ================================================================ */
int run_bot_host_rec_upload_asked(void) {
    static BotHostRecAsked a;
    char block[32];
    int i;

    botHostRecAskedInit(&a);
    UT_ASSERT(!botHostRecAskedHas(&a, "blk001", "hostA", true));
    botHostRecAskedNote(&a, "blk001", "hostA", true);
    UT_ASSERT(botHostRecAskedHas(&a, "blk001", "hostA", true));
    /* Another block, another host, or the other kind of host: refused. */
    UT_ASSERT(!botHostRecAskedHas(&a, "blk002", "hostA", true));
    UT_ASSERT(!botHostRecAskedHas(&a, "blk001", "hostB", true));
    UT_ASSERT(!botHostRecAskedHas(&a, "blk001", "hostA", false));
    UT_ASSERT(!botHostRecAskedHas(&a, NULL, "hostA", true));

    /* Old entries make room for new ones; the newest are kept. */
    for (i = 0; i < BOT_HOST_REC_ASKED_MAX + 10; i++) {
        SDL_snprintf(block, sizeof(block), "b%04d", i);
        botHostRecAskedNote(&a, block, "hostC", false);
    }
    UT_ASSERT(botHostRecAskedHas(&a, block, "hostC", false));
    UT_ASSERT(!botHostRecAskedHas(&a, "b0000", "hostC", false));
    UT_ASSERT(!botHostRecAskedHas(&a, "blk001", "hostA", true));
    return 0;
}

/* ================================================================
 * 5. Same-computer salt limit
 * ================================================================ */
int run_bot_host_salt_limit(void) {
    BotHostSaltLimit l;
    uint32_t t = 5000;
    int i, took = 0;

    botHostSaltLimitInit(&l);
    UT_ASSERT(botHostSaltLimitTake(&l, t));
    /* Too soon after the last. */
    UT_ASSERT(!botHostSaltLimitTake(&l, t + 10));
    UT_ASSERT(!botHostSaltLimitTake(&l, t + BOT_HOST_SALT_MIN_GAP_MS - 1));
    took = 1;
    for (i = 0; i < BOT_HOST_SALT_MAX_PER_CONN * 2; i++) {
        t += BOT_HOST_SALT_MIN_GAP_MS;
        if (botHostSaltLimitTake(&l, t)) took++;
    }
    UT_ASSERT_MSG(took == BOT_HOST_SALT_MAX_PER_CONN,
                  "took %d salts", took);
    /* A new connection starts again. */
    botHostSaltLimitInit(&l);
    UT_ASSERT(botHostSaltLimitTake(&l, t));
    UT_ASSERT(!botHostSaltLimitTake(NULL, t));
    return 0;
}

/* ================================================================
 * 6. capacity() in a protected call with a time limit
 * ================================================================ */
static int s_otherHookRuns;
static void sec_other_hook(lua_State *L, lua_Debug *ar) {
    (void)L; (void)ar;
    s_otherHookRuns++;
}

/* Runs src, keeps its table as "brain", and asks for the answer. */
static int sec_capacity(const char *src, const char *name, unsigned budgetMs,
                        SDL_AtomicInt *abortFlag, bool *hookKept) {
    lua_State *L = luaL_newstate();
    int value = -100;

    if (L == NULL) return value;
    luaL_openlibs(L);
#ifdef WINBOLO_LUAJIT
    (void)luaL_dostring(L, "if jit and jit.off then jit.off() end\n");
#endif
    if (luaL_dostring(L, src) == 0) {
        lua_setglobal(L, "brain");
        lua_sethook(L, sec_other_hook, LUA_MASKCOUNT, 100000);
        value = brainCapacityResolveLuaEx(L, NULL, name, budgetMs, abortFlag);
        if (lua_gettop(L) != 0) value = -200;
        if (hookKept != NULL) {
            *hookKept = lua_gethook(L) == sec_other_hook &&
                        lua_gethookmask(L) == LUA_MASKCOUNT &&
                        lua_gethookcount(L) == 100000;
        }
    }
    lua_close(L);
    return value;
}

int run_brain_capacity_throwing_index(void) {
    BrainMachineInfo m;
    SDL_AtomicInt stop;
    int def;
    bool kept = false;
    Uint64 t0;

    brainCapacityClear();
    brainCapacitySetMachineFromTest("Ut CPU", 6, 6, 30000.0, 150000.0);
    brainCapacityGetMachine(&m);
    def = brainCapacityDefault(&m);

    /* An __index that throws: no crash, nothing kept, the stack as it was. */
    UT_ASSERT(sec_capacity(
        "return setmetatable({}, { __index = function() error('boom') end })",
        "secthrow", 500, NULL, &kept) == BRAIN_CAPACITY_UNKNOWN);
    UT_ASSERT(kept);

    /* An __index that never returns: stopped by the time limit. */
    t0 = SDL_GetTicks();
    UT_ASSERT(sec_capacity(
        "return setmetatable({}, { __index = function() while true do end end })",
        "secloopidx", 300, NULL, &kept) == BRAIN_CAPACITY_UNKNOWN);
    UT_ASSERT(kept);
    UT_ASSERT_MSG(SDL_GetTicks() - t0 < 5000, "took %u ms",
                  (unsigned)(SDL_GetTicks() - t0));

    /* A capacity() that never returns: stopped; the default. */
    t0 = SDL_GetTicks();
    UT_ASSERT(sec_capacity(
        "return { version = '1', capacity = function() while true do end end }",
        "secloopcap", 300, NULL, &kept) == def);
    UT_ASSERT(kept);
    UT_ASSERT(SDL_GetTicks() - t0 < 5000);

    /* The abort flag stops it at once, with no time limit. */
    SDL_SetAtomicInt(&stop, 1);
    t0 = SDL_GetTicks();
    UT_ASSERT(sec_capacity(
        "return { version = '1', capacity = function() while true do end end }",
        "secabort", 0, &stop, &kept) == def);
    UT_ASSERT(kept);
    UT_ASSERT(SDL_GetTicks() - t0 < 5000);

    /* A good brain still answers. */
    UT_ASSERT(sec_capacity(
        "return { version = 2, capacity = function(m) return 3 end }",
        "secgood", 500, NULL, &kept) == 3);
    UT_ASSERT(kept);
    brainCapacityClear();
    return 0;
}
