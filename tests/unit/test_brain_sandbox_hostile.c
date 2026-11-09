/*
 * Hostile brains: one case per hole the security review on PR #652 found
 * in the brain sandbox and in the C bindings a brain can call.
 *
 * Each case builds a brain VM the way luaBrainInstanceCreate does for a
 * production host: the standard libraries, the cpf_* pathfinder bindings
 * and the GoalHunter C bindings, then brainSandboxApply (the same code
 * luabrainshandler.c runs). The brain's attempts then run as Lua inside
 * that VM, or, where the hole is in what C reads, as the C call the
 * binding makes.
 *
 * brain_sandbox_hostile_native_preloads
 *      -- require("ffi"), "string.buffer", "jit.util" and "jit.profile"
 *         all fail; the registry's _PRELOAD holds none of them; table.new
 *         still loads; jit.attach is gone and jit.off is kept
 * brain_sandbox_hostile_bytecode_init
 *      -- an init.lua that is precompiled bytecode is refused when
 *         sandboxed, and loads when not (so the file is good bytecode)
 * brain_sandbox_hostile_loaders_removed
 *      -- loadstring, string.dump, load and dofile are nil
 * brain_sandbox_hostile_astar_log_jail
 *      -- cpf_astar_log_enable cannot write outside the brain folder,
 *         and can still write inside it
 * brain_sandbox_hostile_bad_binding_args
 *      -- negative tail steps, off-map shells_at, junk stamp tables,
 *         an oversized PILL_RANGE_MAP, an off-map next-step start and an
 *         off-map terrain read are refused or clamped
 * brain_sandbox_hostile_forged_blob
 *      -- a cpf_deserialize blob with a forged open count leaves the
 *         slate's open count at 0
 * brain_sandbox_hostile_pooled_info
 *      -- a brain that gives the pooled info tables metatables, or
 *         replaces them with other values, does not break the next push
 * brain_sandbox_hostile_shield_file
 *      -- a shield stamp file whose path claims more tiles than it holds
 *         is refused at the load
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "test_harness.h"
#include "brain.h"
#include "braincore.h"
#include "brain_pathfinder.h"
#include "brain_sandbox.h"
#include "gh_threat.h"
#include "gh_attack.h"
#include "gh_shield_stamp.h"
#include "gh_opt_log.h"

/* ---- helpers ------------------------------------------------------- */

static SDL_EnumerationResult bsh_delete_entry(void *ud, const char *dir,
                                              const char *fname) {
    char full[FILENAME_MAX];
    (void)ud;
    SDL_snprintf(full, sizeof(full), "%s/%s", dir, fname);
    SDL_RemovePath(full);
    return SDL_ENUM_CONTINUE;
}

static void bsh_rmdir(const char *dir) {
    SDL_EnumerateDirectory(dir, bsh_delete_entry, NULL);
    SDL_RemovePath(dir);
}

static bool bsh_exists(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info);
}

static bool bsh_write(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

/* The pieces of a production brain VM these cases need. */
typedef struct {
    lua_State       *L;
    BrainPathfinder *pf;
    const BYTE      *world;
} BshVm;

static BYTE s_map[65536];

static void bsh_fill_map(BYTE terrain) {
    memset(s_map, terrain, sizeof(s_map));
}

static bool bsh_vm_open(BshVm *vm, const char *root) {
    memset(vm, 0, sizeof(*vm));
    vm->L = luaL_newstate();
    if (vm->L == NULL) return false;
    luaL_openlibs(vm->L);
    brainCoreRegisterConstants(vm->L);
    vm->world = s_map;
    brainCoreRegisterGetTerrain(vm->L, &vm->world);
    vm->pf = brainPathfinderCreate();
    if (vm->pf == NULL) return false;
    brainPathfinderSetMap(vm->pf, s_map);
    brainCoreRegisterPathfinder(vm->L, &vm->pf);
    naThreatRegister(vm->L);
    naShieldStampRegister(vm->L);
    naOptLogRegister(vm->L);
    naAttackRegister(vm->L);
    brainSandboxApply(vm->L, root);
    return true;
}

static void bsh_vm_close(BshVm *vm) {
    if (vm->L) lua_close(vm->L);
    if (vm->pf) brainPathfinderDestroy(vm->pf);
    memset(vm, 0, sizeof(*vm));
}

/* Run a chunk of brain code. Returns NULL on success, or the error. The
 * returned string is a static copy. */
static const char *bsh_run(lua_State *L, const char *code) {
    static char err[512];
    if (luaL_loadstring(L, code) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        SDL_strlcpy(err, e ? e : "(no message)", sizeof(err));
        lua_pop(L, 1);
        return err;
    }
    return NULL;
}

#define BSH_RUN_OK(L, code)                                              \
    do {                                                                 \
        const char *bsh_e_ = bsh_run((L), (code));                       \
        UT_ASSERT_MSG(bsh_e_ == NULL, "brain code failed: %s", bsh_e_);  \
    } while (0)

/* ---- cases --------------------------------------------------------- */

int run_brain_sandbox_hostile_native_preloads(void) {
    static const char *ROOT = "test_bsh_preloads";
    BshVm vm;
    bsh_rmdir(ROOT);
    UT_ASSERT(SDL_CreateDirectory(ROOT));
    bsh_fill_map(7);
    UT_ASSERT(bsh_vm_open(&vm, ROOT));

    BSH_RUN_OK(vm.L,
        "for _, name in ipairs({'ffi', 'string.buffer', 'jit.util', 'jit.profile'}) do\n"
        "  local ok, mod = pcall(require, name)\n"
        "  assert(not ok, 'require(\"' .. name .. '\") loaded')\n"
        "  assert(package.preload[name] == nil, name .. ' still in package.preload')\n"
        "  assert(package.loaded[name] == nil, name .. ' in package.loaded')\n"
        "end\n"
        "assert(rawget(_G, 'ffi') == nil, 'ffi global')\n"
        "if jit then\n"
        "  assert(jit.attach == nil, 'jit.attach is still there')\n"
        "  assert(type(jit.off) == 'function', 'jit.off was removed')\n"
        "  assert(type(jit.opt) == 'table', 'jit.opt was removed')\n"
        "  local ok, tnew = pcall(require, 'table.new')\n"
        "  assert(ok and type(tnew) == 'function', 'table.new no longer loads')\n"
        "end\n");

    /* The registry's own copy, which the brain cannot reach but which
     * must not keep a loader the brain could get back some other way. */
    lua_getfield(vm.L, LUA_REGISTRYINDEX, "_PRELOAD");
    if (lua_istable(vm.L, -1)) {
        lua_getfield(vm.L, -1, "ffi");
        UT_ASSERT_MSG(lua_isnil(vm.L, -1), "registry _PRELOAD still has ffi");
        lua_pop(vm.L, 1);
        lua_getfield(vm.L, -1, "string.buffer");
        UT_ASSERT_MSG(lua_isnil(vm.L, -1),
                      "registry _PRELOAD still has string.buffer");
        lua_pop(vm.L, 1);
    }
    lua_pop(vm.L, 1);

    bsh_vm_close(&vm);
    bsh_rmdir(ROOT);
    return 0;
}

int run_brain_sandbox_hostile_bytecode_init(void) {
    static const char *ROOT = "test_bsh_bytecode";
    char path[FILENAME_MAX];
    char srcpath[FILENAME_MAX];
    size_t blen = 0;
    const char *b;
    BshVm vm;
    lua_State *plain;

    bsh_rmdir(ROOT);
    UT_ASSERT(SDL_CreateDirectory(ROOT));
    SDL_snprintf(path, sizeof(path), "%s/init.lua", ROOT);
    SDL_snprintf(srcpath, sizeof(srcpath), "%s/source.lua", ROOT);

    /* Real bytecode, made in a VM with no sandbox. */
    plain = luaL_newstate();
    UT_ASSERT(plain != NULL);
    luaL_openlibs(plain);
    UT_ASSERT(luaL_dostring(plain,
        "return string.dump(function() return 42 end)") == LUA_OK);
    b = lua_tolstring(plain, -1, &blen);
    UT_ASSERT(b != NULL && blen > 4);
    UT_ASSERT_MSG(b[0] == 0x1b, "string.dump did not give bytecode");
    UT_ASSERT(bsh_write(path, b, blen));
    lua_pop(plain, 1);
    UT_ASSERT(bsh_write(srcpath, "return 42\n", 10));

    /* Control: with no sandbox the same file loads, so it is good
     * bytecode and the refusal below is the mode, not a broken file. */
    UT_ASSERT_MSG(brainSandboxLoadInitFile(plain, path, false) == LUA_OK,
                  "unsandboxed load of the bytecode failed: %s",
                  lua_tostring(plain, -1));
    lua_close(plain);

    bsh_fill_map(7);
    UT_ASSERT(bsh_vm_open(&vm, ROOT));
    UT_ASSERT_MSG(brainSandboxLoadInitFile(vm.L, path, true) != LUA_OK,
                  "sandboxed load took a bytecode init.lua");
    lua_pop(vm.L, 1);
    UT_ASSERT_MSG(brainSandboxLoadInitFile(vm.L, srcpath, true) == LUA_OK,
                  "sandboxed load refused a source file: %s",
                  lua_tostring(vm.L, -1));
    UT_ASSERT(lua_pcall(vm.L, 0, 1, 0) == LUA_OK);
    UT_ASSERT(lua_tointeger(vm.L, -1) == 42);
    lua_pop(vm.L, 1);

    bsh_vm_close(&vm);
    bsh_rmdir(ROOT);
    return 0;
}

int run_brain_sandbox_hostile_loaders_removed(void) {
    static const char *ROOT = "test_bsh_loaders";
    BshVm vm;
    bsh_rmdir(ROOT);
    UT_ASSERT(SDL_CreateDirectory(ROOT));
    bsh_fill_map(7);
    UT_ASSERT(bsh_vm_open(&vm, ROOT));
    BSH_RUN_OK(vm.L,
        "assert(loadstring == nil, 'loadstring is there')\n"
        "assert(string.dump == nil, 'string.dump is there')\n"
        "assert(load == nil, 'load is there')\n"
        "assert(dofile == nil, 'dofile is there')\n"
        "assert(('x').dump == nil, 'string method dump is there')\n");
    bsh_vm_close(&vm);
    bsh_rmdir(ROOT);
    return 0;
}

int run_brain_sandbox_hostile_astar_log_jail(void) {
    static const char *ROOT    = "test_bsh_astar";
    static const char *OUTSIDE = "test_bsh_astar_outside.log";
    char code[1024];
    char inside[FILENAME_MAX];
    BshVm vm;

    bsh_rmdir(ROOT);
    SDL_RemovePath(OUTSIDE);
    UT_ASSERT(SDL_CreateDirectory(ROOT));
    bsh_fill_map(7);
    UT_ASSERT(bsh_vm_open(&vm, ROOT));

    SDL_snprintf(code, sizeof(code),
        "local r, e = cpf_astar_log_enable('%s')\n"
        "assert(r == nil and tostring(e):find('blocked'), 'outside path taken')\n"
        "r, e = cpf_astar_log_enable('%s/../%s')\n"
        "assert(r == nil and tostring(e):find('blocked'), 'dot-dot path taken')\n"
        "r, e = cpf_astar_log_enable({})\n"
        "assert(r == nil, 'a table was taken as a path')\n",
        OUTSIDE, ROOT, OUTSIDE);
    BSH_RUN_OK(vm.L, code);
    UT_ASSERT_MSG(!bsh_exists(OUTSIDE), "a log was written outside the jail");

    SDL_snprintf(inside, sizeof(inside), "%s/astar.log", ROOT);
    SDL_snprintf(code, sizeof(code),
        "cpf_astar_log_enable('%s')\n", inside);
    BSH_RUN_OK(vm.L, code);
    UT_ASSERT_MSG(bsh_exists(inside), "the log inside the brain folder was not made");
    BSH_RUN_OK(vm.L, "cpf_astar_log_enable()\n");   /* nil closes it */

    bsh_vm_close(&vm);
    bsh_rmdir(ROOT);
    return 0;
}

int run_brain_sandbox_hostile_bad_binding_args(void) {
    static const char *ROOT = "test_bsh_args";
    BshVm vm;
    int nx = -1, ny = -1;

    bsh_rmdir(ROOT);
    UT_ASSERT(SDL_CreateDirectory(ROOT));
    bsh_fill_map(1);   /* river everywhere, so the water step is used */
    UT_ASSERT(bsh_vm_open(&vm, ROOT));

    /* Seeds for the tail, so tail_pass walks with the bad steps. */
    for (int i = 0; i < 64; i++) {
        vm.pf->influence_grid[(100 + i) * 256 + 100] = 1000;
    }

    BSH_RUN_OK(vm.L,
        /* Negative steps: refused at the binding. */
        "assert(not pcall(cpf_rebuild_influence_tail, 1, 8, 10, -5, 2), 'negative neutral_step taken')\n"
        "assert(not pcall(cpf_rebuild_influence_tail, 1, 8, 10, 3, -100000), 'negative water_step taken')\n"
        /* Valid steps still run. */
        "cpf_rebuild_influence_tail(1, 8, 10, 3, 2)\n"
        /* Off-map tiles: nil, not a read past the arrays. */
        "assert(cpf_astar_shells_at(-1, 99999) == nil, 'astar_shells_at off-map')\n"
        "assert(cpf_astar_shells_at(256, 0) == nil, 'astar_shells_at x=256')\n"
        "assert(cpf_dijkstra_shells_at(0, 300, -2) == nil, 'dijkstra_shells_at off-map')\n"
        /* Stamp tables of the wrong type: skipped, no error, no crash. */
        "gh_attack.init_stamps({[0] = 5, [5] = {spot_dx = 1, aims = 7},\n"
        "                       [10] = {aims = {'x', 3}}},\n"
        "                      {[0] = 'abcdefgh', [5] = 9, [10] = {}})\n"
        /* PILL_RANGE_MAP far past MAX_DISK_R. */
        "gh_threat.configure({PILL_RANGE_MAP = 100000})\n"
        "gh_threat.apply_occlusion_all({}, {}, {}, 0, {100}, {100}, 1)\n"
        "gh_threat.configure({PILL_RANGE_MAP = -7})\n"
        "gh_threat.apply_occlusion_all({}, {}, {}, 0, {100}, {100}, 1)\n");

    /* The C side of the tail rebuild clamps too, for callers that are
     * not the binding. */
    brainPathfinderRebuildInfluenceTail(vm.pf, 1, 8, 10, -5, -100000, 0, 1);
    brainPathfinderRebuildInfluenceTail(vm.pf, 1, 8, 10, 3, 2147483000, 0, 1);

    /* Terrain read off the map: deep sea, not memory before the map.
     * One step off a side edge still reads the next row, as before. */
    UT_ASSERT(bsh_run(vm.L, "gh_threat.configure({T_DEEPSEA = 10})") == NULL);
    UT_ASSERT_MSG(naThreatRawTT(vm.L, -3, -3) == 10,
                  "off-map terrain read was not deep sea");
    UT_ASSERT_MSG(naThreatRawTT(vm.L, 300, 255) == 10,
                  "off-map terrain read past the end was not deep sea");
    s_map[5 * 256 - 1] = 5;
    UT_ASSERT_MSG(naThreatRawTT(vm.L, -1, 5) == 5,
                  "the in-array read one step off the side changed");

    /* Next step from a start off the map, with a slate that has data,
     * so the fallback that read g_cost at the start is reached. */
    bsh_fill_map(7);
    brainPathfinderSetMap(vm.pf, s_map);
    brainPathfinderDijkstraStart(vm.pf, 0, 1, 100, 100, 0, 40, 0, 0, 40,
                                 0.0f, 0, 1.0f, 0, 1);
    brainPathfinderDijkstraStep(vm.pf, 0, 1, 1000000);
    UT_ASSERT(brainPathfinderDijkstraNextStep(vm.pf, 0, -100000, 5, 110, 110,
                                              NULL, 0, 1.0e6f, -1,
                                              &nx, &ny) == 0);
    UT_ASSERT(brainPathfinderDijkstraNextStep(vm.pf, 0, 50, 99999, 110, 110,
                                              NULL, 0, 1.0e6f, -1,
                                              &nx, &ny) == 0);

    bsh_vm_close(&vm);
    bsh_rmdir(ROOT);
    return 0;
}

static void bsh_put_le32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t bsh_get_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int run_brain_sandbox_hostile_forged_blob(void) {
    BrainPathfinder *empty, *live;
    unsigned char *blob;
    size_t size = 0, off;
    uint32_t clen, nslates;

    bsh_fill_map(7);
    /* A blob from a pathfinder with no slate data: every slate is
     * written with hasData = 0, and its header is then forged. */
    empty = brainPathfinderCreate();
    UT_ASSERT(empty != NULL);
    brainPathfinderSetMap(empty, s_map);
    blob = brainPathfinderSerialize(empty, &size);
    UT_ASSERT(blob != NULL && size > 300);

    /* header 12, 17 config floats, 3 terrain tables of 16 floats */
    off = 12 + 17 * 4 + 3 * (sizeof(empty->terrain_cost_table));
    UT_ASSERT(off + 4 <= size);
    clen = bsh_get_le32(blob + off);
    off += 4 + clen;
    UT_ASSERT(off + 4 + 16 * 4 <= size);
    nslates = bsh_get_le32(blob + off);
    UT_ASSERT_MSG(nslates >= 1 && nslates <= DIJKSTRA_NUM_SLATES,
                  "blob layout not as expected (slates=%u)", nslates);
    off += 4;
    bsh_put_le32(blob + off + 1 * 4, 0);          /* done = 0 */
    bsh_put_le32(blob + off + 15 * 4, 1000000);   /* open_count */

    /* The victim has a live slate 0, so a heap is allocated there. */
    live = brainPathfinderCreate();
    UT_ASSERT(live != NULL);
    brainPathfinderSetMap(live, s_map);
    brainPathfinderDijkstraStart(live, 0, 1, 100, 100, 0, 40, 0, 0, 40,
                                 0.0f, 0, 1.0f, 0, 1);
    (void)brainPathfinderDeserialize(live, blob, size);
    UT_ASSERT_MSG(live->dij_slates[0].open_count == 0,
                  "forged open count was taken: %d",
                  (int)live->dij_slates[0].open_count);
    /* And the step that would have popped past the heap returns. */
    brainPathfinderDijkstraStep(live, 0, 2, 1000);

    free(blob);
    brainPathfinderDestroy(empty);
    brainPathfinderDestroy(live);
    return 0;
}

static int bsh_push_info(lua_State *L) {
    const BrainInfo *info = (const BrainInfo *)lua_touserdata(L, 1);
    lua_settop(L, 0);
    brainCorePushInfo(L, info);
    return 1;
}

/* PushInfo runs outside any pcall in the hosts; here it runs inside one
 * so a failure is a test failure and not the end of the test run. */
static int bsh_push_protected(lua_State *L, const BrainInfo *info) {
    lua_pushcfunction(L, bsh_push_info);
    lua_pushlightuserdata(L, (void *)info);
    return lua_pcall(L, 1, 1, 0);
}

int run_brain_sandbox_hostile_pooled_info(void) {
    static BrainInfo info;
    static ObjectInfo objs[2];
    static GameEvent evs[2];
    lua_State *L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);

    memset(&info, 0, sizeof(info));
    memset(objs, 0, sizeof(objs));
    memset(evs, 0, sizeof(evs));
    info.num_objects = 2;
    info.objects = objs;
    info.num_events = 2;
    info.events = evs;

    UT_ASSERT(bsh_push_protected(L, &info) == LUA_OK);
    lua_setglobal(L, "info");

    /* The elements: a metatable on an object and on an event, and an
     * event's data array swapped for a number. */
    BSH_RUN_OK(L,
        "local hostile_mt = {__newindex = function() error('hostile metatable') end,\n"
        "              __index = function() error('hostile metatable') end}\n"
        "setmetatable(info.objects[1], hostile_mt)\n"
        "setmetatable(info.events[1], hostile_mt)\n"
        "info.events[2].data = 7\n");
    UT_ASSERT_MSG(bsh_push_protected(L, &info) == LUA_OK,
                  "push after element tampering failed: %s",
                  lua_tostring(L, -1));
    lua_setglobal(L, "info");
    BSH_RUN_OK(L,
        "assert(getmetatable(info.objects[1]) == nil)\n"
        "assert(getmetatable(info.events[1]) == nil)\n"
        "assert(type(info.events[2].data) == 'table')\n");

    /* The pooled tables themselves: replaced with other values, and the
     * info table given a metatable. */
    BSH_RUN_OK(L,
        "local hostile_mt = {__newindex = function() error('hostile metatable') end,\n"
        "              __index = function() error('hostile metatable') end}\n"
        "info.objects = 5\n"
        "info.player_names = 'abcdefgh'\n"
        "info.events = true\n"
        "setmetatable(info.rules, hostile_mt)\n"
        "setmetatable(info, hostile_mt)\n");
    UT_ASSERT_MSG(bsh_push_protected(L, &info) == LUA_OK,
                  "push after pool tampering failed: %s",
                  lua_tostring(L, -1));
    lua_setglobal(L, "info");
    BSH_RUN_OK(L,
        "assert(getmetatable(info) == nil, 'info kept its metatable')\n"
        "assert(type(info.objects) == 'table' and #info.objects == 2)\n"
        "assert(type(info.player_names) == 'table')\n"
        "assert(type(info.events) == 'table' and #info.events == 2)\n"
        "assert(getmetatable(info.rules) == nil)\n");

    lua_close(L);
    return 0;
}

int run_brain_sandbox_hostile_shield_file(void) {
    static const char *ROOT = "test_bsh_shield";
    unsigned char file[32 + 240];
    char path[FILENAME_MAX];
    char code[512];
    BshVm vm;

    bsh_rmdir(ROOT);
    UT_ASSERT(SDL_CreateDirectory(ROOT));
    memset(file, 0, sizeof(file));
    bsh_put_le32(file + 0, 0x444C4853u);   /* "SHLD" */
    bsh_put_le32(file + 4, 2u);            /* version */
    bsh_put_le32(file + 8, 1u);            /* n_angles */
    bsh_put_le32(file + 12, 1u);           /* n_nudges */
    /* entry 0, aim 0: n_tiles at byte 9 of the 40-byte path */
    file[32 + 9] = 200;
    SDL_snprintf(path, sizeof(path), "%s/forged.bin", ROOT);
    UT_ASSERT(bsh_write(path, file, sizeof(file)));

    bsh_fill_map(7);
    UT_ASSERT(bsh_vm_open(&vm, ROOT));
    SDL_snprintf(code, sizeof(code),
        "assert(gh_shield.load('%s') == false, 'forged file loaded')\n"
        "assert(gh_shield.get_flat(0, 0, 1) == nil, 'get_flat read the forged path')\n",
        path);
    BSH_RUN_OK(vm.L, code);

    bsh_vm_close(&vm);
    bsh_rmdir(ROOT);
    return 0;
}
