/*
 * Hostile brains against the brain sandbox (src/bolo/brain_sandbox.c).
 *
 * Each case builds a brain VM the way luaBrainInstanceCreate does for a
 * production host: the standard libraries, the cpf_* pathfinder bindings
 * and the GoalHunter C bindings, then brainSandboxApply, the same code the
 * host runs. The brain's attempts then run as Lua inside that VM. The C
 * bindings' own bounds are in test_brain_binding_bounds.c.
 *
 * run_brain_sandbox_hostile_native_preloads
 *      require("ffi"), "string.buffer", "jit.util" and "jit.profile" all
 *      fail, also when the host had loaded ffi before the sandbox;
 *      package.preload and the registry's _PRELOAD are empty; jit.attach
 *      is gone and jit.off and jit.opt are kept; after
 *      brainSandboxRestrictJit only jit.on / off / status and the version
 *      fields are left, and require("jit.opt") fails
 * run_brain_sandbox_hostile_bytecode_init
 *      an init.lua that is precompiled bytecode is refused when sandboxed,
 *      and loads when not (so the file is good bytecode)
 * run_brain_sandbox_hostile_loaders_removed
 *      the base, string, package, io and os functions the sandbox removes
 *      are nil; debug keeps only traceback and getinfo, and getinfo gives
 *      out no function
 * run_brain_sandbox_hostile_astar_log_removed
 *      cpf_astar_log_enable, which opened any path for writing, is gone
 * run_brain_sandbox_hostile_file_jail
 *      io.open and loadfile refuse a path outside the brain folder, also
 *      through "..", and still work inside it; loadfile refuses bytecode
 * run_brain_sandbox_hostile_off_map_terrain
 *      gh_threat's terrain read outside the map's memory is deep sea; a
 *      read one step off a side edge reads the next row as before
 * run_brain_sandbox_jit_limits
 *      a brain VM made in the host's order (brainSandboxApply,
 *      brainSandboxTuneJit, brainSandboxRestrictJit) holds 2000 live
 *      traces and more than 4096 KB of machine code with no trace cache
 *      flush, so maxtrace and maxmcode are above LuaJIT's defaults of 1000
 *      and 4096; the same work in a VM with the defaults flushes. Passes
 *      with nothing to check on PUC-Lua or with the compiler off.
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

static BYTE bshMap[256 * 256];

/* The pieces of a production brain VM these cases need. */
typedef struct {
    lua_State       *L;
    BrainPathfinder *pf;
    const BYTE      *world;
} BshVm;

/* Open a VM with the bindings a GoalHunter brain gets. When sandbox is
 * true, brainSandboxApply runs last with root as the brain folder. */
static bool bshVmOpen(BshVm *vm, const char *root, bool sandbox) {
    memset(vm, 0, sizeof(*vm));
    memset(bshMap, 7, sizeof(bshMap));   /* grass */
    vm->L = luaL_newstate();
    if (vm->L == NULL) return false;
    /* The bindings read a BotContext pointer out of the extra space,
     * which Lua does not zero. */
    *(void **)lua_getextraspace(vm->L) = NULL;
    luaL_openlibs(vm->L);
    brainCoreRegisterConstants(vm->L);
    vm->world = bshMap;
    brainCoreRegisterGetTerrain(vm->L, &vm->world);
    vm->pf = brainPathfinderCreate();
    if (vm->pf == NULL) return false;
    brainPathfinderSetMap(vm->pf, bshMap);
    brainCoreRegisterPathfinder(vm->L, &vm->pf);
    naThreatRegister(vm->L);
    naShieldStampRegister(vm->L);
    naOptLogRegister(vm->L);
    naAttackRegister(vm->L);
    if (sandbox) brainSandboxApply(vm->L, root);
    return true;
}

static void bshVmClose(BshVm *vm) {
    if (vm->L != NULL) lua_close(vm->L);
    if (vm->pf != NULL) brainPathfinderDestroy(vm->pf);
    memset(vm, 0, sizeof(*vm));
}

/* Run a chunk of brain code. NULL on success, else the error, in a static
 * buffer. */
static const char *bshRun(lua_State *L, const char *code) {
    static char err[512];
    if (luaL_loadstring(L, code) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        SDL_strlcpy(err, e != NULL ? e : "(no message)", sizeof(err));
        lua_pop(L, 1);
        return err;
    }
    return NULL;
}

#define BSH_RUN_OK(L, code)                                                 \
    do {                                                                    \
        const char *bshErr_ = bshRun((L), (code));                          \
        UT_ASSERT_MSG(bshErr_ == NULL, "brain code failed: %s", bshErr_);   \
    } while (0)

static bool bshWrite(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    bool ok;
    if (f == NULL) return false;
    ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static bool bshExists(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info);
}

/* <scratch>/brain, made, as a brain's own folder. */
static bool bshBrainDir(char *out, size_t outSz) {
    if (!utScratchPath(out, outSz, "brain")) return false;
    return SDL_CreateDirectory(out);
}

/* Bytecode for "return 42", made in a VM with no sandbox, into path. */
static bool bshWriteBytecode(const char *path) {
    lua_State *plain = luaL_newstate();
    size_t len = 0;
    const char *b;
    bool ok = false;
    if (plain == NULL) return false;
    luaL_openlibs(plain);
    if (luaL_dostring(plain, "return string.dump(function() return 42 end)") ==
        LUA_OK) {
        b = lua_tolstring(plain, -1, &len);
        ok = b != NULL && len > 4 && b[0] == 0x1b && bshWrite(path, b, len);
    }
    lua_close(plain);
    return ok;
}

/* ---- cases --------------------------------------------------------- */

int run_brain_sandbox_hostile_native_preloads(void) {
    char root[FILENAME_MAX];
    BshVm vm;

    UT_ASSERT(bshBrainDir(root, sizeof(root)));

    /* A host that loaded ffi itself before the sandbox leaves it in
     * package.loaded, where require hands it out with no searcher. */
    UT_ASSERT(bshVmOpen(&vm, root, false));
    (void)bshRun(vm.L, "pcall(require, 'ffi')");
    brainSandboxApply(vm.L, root);

    BSH_RUN_OK(vm.L,
        "for _, name in ipairs({'ffi', 'string.buffer', 'jit.util', 'jit.profile'}) do\n"
        "  local ok = pcall(require, name)\n"
        "  assert(not ok, 'require(\"' .. name .. '\") loaded')\n"
        "  assert(package.loaded[name] == nil, name .. ' is in package.loaded')\n"
        "end\n"
        "assert(next(package.preload) == nil, 'package.preload is not empty')\n"
        "assert(rawget(_G, 'ffi') == nil, 'an ffi global')\n"
        "if jit then\n"
        "  assert(jit.attach == nil, 'jit.attach is still there')\n"
        "  assert(type(jit.off) == 'function', 'jit.off was removed')\n"
        "  assert(type(jit.opt) == 'table', 'jit.opt was removed')\n"
        "end\n");

    /* The host makes its own jit.opt calls after the sandbox, then cuts the
     * table down to what a brain may use. */
    brainSandboxRestrictJit(vm.L);
    BSH_RUN_OK(vm.L,
        "if jit then\n"
        "  for _, name in ipairs({'opt', 'flush', 'attach', 'util', 'profile'}) do\n"
        "    assert(jit[name] == nil, 'jit.' .. name .. ' is still there')\n"
        "  end\n"
        "  for _, name in ipairs({'on', 'off', 'status'}) do\n"
        "    assert(type(jit[name]) == 'function', 'jit.' .. name .. ' was removed')\n"
        "  end\n"
        "  assert(type(jit.version) == 'string', 'jit.version was removed')\n"
        "  assert(require('jit') == jit, 'require(\"jit\") is another table')\n"
        "  assert(not pcall(require, 'jit.opt'), 'require(\"jit.opt\") loaded')\n"
        "  jit.off() jit.on()\n"
        "end\n");

    /* The registry's own copy, which the brain cannot reach but which must
     * not keep a loader the brain could get back some other way. */
    lua_getfield(vm.L, LUA_REGISTRYINDEX, "_PRELOAD");
    if (lua_istable(vm.L, -1)) {
        lua_pushnil(vm.L);
        UT_ASSERT_MSG(lua_next(vm.L, -2) == 0,
                      "the registry's _PRELOAD is not empty");
    }
    lua_pop(vm.L, 1);

    bshVmClose(&vm);
    return 0;
}

int run_brain_sandbox_hostile_bytecode_init(void) {
    char root[FILENAME_MAX];
    char path[FILENAME_MAX];
    char srcPath[FILENAME_MAX];
    lua_State *plain;
    BshVm vm;

    UT_ASSERT(bshBrainDir(root, sizeof(root)));
    SDL_snprintf(path, sizeof(path), "%s/init.lua", root);
    SDL_snprintf(srcPath, sizeof(srcPath), "%s/source.lua", root);
    UT_ASSERT(bshWriteBytecode(path));
    UT_ASSERT(bshWrite(srcPath, "return 42\n", 10));

    /* Control: with no sandbox the same file loads, so it is good bytecode
     * and the refusal below is the mode, not a broken file. */
    plain = luaL_newstate();
    UT_ASSERT(plain != NULL);
    luaL_openlibs(plain);
    UT_ASSERT_MSG(brainSandboxLoadInitFile(plain, path, false) == LUA_OK,
                  "the unsandboxed load of the bytecode failed: %s",
                  lua_tostring(plain, -1));
    lua_close(plain);

    UT_ASSERT(bshVmOpen(&vm, root, true));
    UT_ASSERT_MSG(brainSandboxLoadInitFile(vm.L, path, true) != LUA_OK,
                  "the sandboxed load took a bytecode init.lua");
    lua_pop(vm.L, 1);
    UT_ASSERT_MSG(brainSandboxLoadInitFile(vm.L, srcPath, true) == LUA_OK,
                  "the sandboxed load refused a source file: %s",
                  lua_tostring(vm.L, -1));
    UT_ASSERT(lua_pcall(vm.L, 0, 1, 0) == LUA_OK);
    UT_ASSERT(lua_tointeger(vm.L, -1) == 42);
    lua_pop(vm.L, 1);

    bshVmClose(&vm);
    return 0;
}

int run_brain_sandbox_hostile_loaders_removed(void) {
    char root[FILENAME_MAX];
    BshVm vm;

    UT_ASSERT(bshBrainDir(root, sizeof(root)));
    UT_ASSERT(bshVmOpen(&vm, root, true));
    BSH_RUN_OK(vm.L,
        "for _, name in ipairs({'load', 'loadstring', 'dofile', 'getfenv',\n"
        "                       'setfenv', 'newproxy'}) do\n"
        "  assert(rawget(_G, name) == nil, name .. ' is there')\n"
        "end\n"
        "assert(string.dump == nil, 'string.dump is there')\n"
        "assert(('x').dump == nil, 'the string method dump is there')\n"
        "assert(package.loadlib == nil, 'package.loadlib is there')\n"
        "assert(package.cpath == nil, 'package.cpath is there')\n"
        "assert(package.searchpath == nil, 'package.searchpath is there')\n"
        "for _, name in ipairs({'popen', 'lines', 'input', 'output', 'tmpfile'}) do\n"
        "  assert(io[name] == nil, 'io.' .. name .. ' is there')\n"
        "end\n"
        "for _, name in ipairs({'execute', 'exit', 'rename', 'getenv',\n"
        "                       'tmpname', 'setlocale'}) do\n"
        "  assert(os[name] == nil, 'os.' .. name .. ' is there')\n"
        "end\n"
        "for k in pairs(debug) do\n"
        "  assert(k == 'traceback' or k == 'getinfo', 'debug.' .. k .. ' is there')\n"
        "end\n"
        "local i = debug.getinfo(1, 'fSl')\n"
        "assert(i.func == nil, 'debug.getinfo gave out a function')\n"
        "assert(type(i.currentline) == 'number', 'debug.getinfo named no frame')\n");
    bshVmClose(&vm);
    return 0;
}

int run_brain_sandbox_hostile_astar_log_removed(void) {
    char root[FILENAME_MAX];
    BshVm vm;

    UT_ASSERT(bshBrainDir(root, sizeof(root)));
    /* Control: the binding is there before the sandbox. */
    UT_ASSERT(bshVmOpen(&vm, root, false));
    lua_getglobal(vm.L, "cpf_astar_log_enable");
    UT_ASSERT_MSG(lua_isfunction(vm.L, -1),
                  "cpf_astar_log_enable was not registered, so this case "
                  "shows nothing");
    lua_pop(vm.L, 1);
    brainSandboxApply(vm.L, root);
    BSH_RUN_OK(vm.L,
        "assert(cpf_astar_log_enable == nil, 'cpf_astar_log_enable is there')\n");
    bshVmClose(&vm);
    return 0;
}

int run_brain_sandbox_hostile_file_jail(void) {
    char root[FILENAME_MAX];
    char outside[FILENAME_MAX];
    char inside[FILENAME_MAX];
    char code[FILENAME_MAX * 4 + 512];
    char outsideCode[FILENAME_MAX];
    char bytecode[FILENAME_MAX];
    BshVm vm;
    size_t i;

    UT_ASSERT(bshBrainDir(root, sizeof(root)));
    UT_ASSERT(utScratchPath(outside, sizeof(outside), "outside.lua"));
    SDL_snprintf(inside, sizeof(inside), "%s/inside.lua", root);
    SDL_snprintf(bytecode, sizeof(bytecode), "%s/bytecode.lua", root);
    UT_ASSERT(bshWrite(outside, "return 1\n", 9));
    UT_ASSERT(bshWriteBytecode(bytecode));
    /* Lua strings below: backslashes in a Windows path become slashes. */
    SDL_strlcpy(outsideCode, outside, sizeof(outsideCode));
    for (i = 0; outsideCode[i] != '\0'; i++) {
        if (outsideCode[i] == '\\') outsideCode[i] = '/';
    }
    for (i = 0; root[i] != '\0'; i++) {
        if (root[i] == '\\') root[i] = '/';
    }
    for (i = 0; inside[i] != '\0'; i++) {
        if (inside[i] == '\\') inside[i] = '/';
    }
    for (i = 0; bytecode[i] != '\0'; i++) {
        if (bytecode[i] == '\\') bytecode[i] = '/';
    }

    UT_ASSERT(bshVmOpen(&vm, root, true));
    SDL_snprintf(code, sizeof(code),
        "local f, e = io.open('%s', 'w')\n"
        "assert(f == nil, 'io.open wrote outside the brain folder')\n"
        "f = io.open('%s/../outside.lua', 'w')\n"
        "assert(f == nil, 'io.open took a path out through ..')\n"
        "assert(loadfile('%s') == nil, 'loadfile read outside the brain folder')\n"
        "assert(loadfile('%s') == nil, 'loadfile took bytecode')\n"
        "f = assert(io.open('%s', 'w'))\n"
        "f:write('return 7\\n')\n"
        "f:close()\n"
        "assert(assert(loadfile('%s'))() == 7, 'loadfile inside the folder')\n",
        outsideCode, root, outsideCode, bytecode, inside, inside);
    BSH_RUN_OK(vm.L, code);
    bshVmClose(&vm);

    /* The outside file was not changed. */
    {
        size_t len = 0;
        char *got = (char *)SDL_LoadFile(outside, &len);
        UT_ASSERT(got != NULL);
        UT_ASSERT_MSG(len == 9 && memcmp(got, "return 1\n", 9) == 0,
                      "the file outside the brain folder changed");
        SDL_free(got);
    }
    UT_ASSERT(bshExists(inside));
    return 0;
}

int run_brain_sandbox_hostile_off_map_terrain(void) {
    char root[FILENAME_MAX];
    BshVm vm;

    UT_ASSERT(bshBrainDir(root, sizeof(root)));
    UT_ASSERT(bshVmOpen(&vm, root, true));
    BSH_RUN_OK(vm.L, "gh_threat.configure({})\n");   /* T_DEEPSEA = 10 */

    UT_ASSERT_MSG(naThreatRawTT(vm.L, -3, -3) == 10,
                  "a terrain read before the map was not deep sea");
    UT_ASSERT_MSG(naThreatRawTT(vm.L, 300, 255) == 10,
                  "a terrain read past the map was not deep sea");
    /* One step off the left edge of row 5 is the last tile of row 4,
     * inside the array: read as it always was. */
    bshMap[5 * 256 - 1] = 5;
    UT_ASSERT_MSG(naThreatRawTT(vm.L, -1, 5) == 5,
                  "the in-array read one step off the side changed");
    bshVmClose(&vm);
    return 0;
}

/* ---- trace compiler limits ---------------------------------------- */

/* The work: 2000 functions, each with one hot loop of 48 statements, all
 * kept alive (a trace is freed with its function). Each loop compiles to
 * one trace of about 3 KB, so about 2000 traces and 6 MB of machine code,
 * over both of LuaJIT's default limits (1000 traces, 4096 KB). The tools
 * come in as the chunk's argument, taken before the sandbox removed them
 * from the brain's view. Returns flushes, live traces, machine code KB. */
static const char *const bshJitWork =
    "local probe = ...\n"
    "local flushes = 0\n"
    "probe.attach(function(what) if what == 'flush' then flushes = flushes + 1 end end, 'trace')\n"
    "local keep = {}\n"
    "for k = 1, 2000 do\n"
    "  local parts = { 'local x, y = 0, 1 for i = 1, 200 do ' }\n"
    "  for j = 1, 24 do\n"
    "    parts[#parts + 1] = ('x = x + i * %d - y y = y + x %% %d '):format(k * 13 + j, j + 2)\n"
    "  end\n"
    "  parts[#parts + 1] = 'end return x'\n"
    "  local f = assert(probe.loadstring(table.concat(parts)))\n"
    "  f()\n"
    "  keep[k] = f\n"
    "end\n"
    "probe.attach(nil)\n"
    "local traces, bytes = 0, 0\n"
    "for tr = 1, 4000 do\n"
    "  if probe.util.traceinfo(tr) then\n"
    "    traces = traces + 1\n"
    "    local mc = probe.util.tracemc(tr)\n"
    "    if mc then bytes = bytes + #mc end\n"
    "  end\n"
    "end\n"
    "return flushes, traces, bytes / 1024\n";

/* Open a plain VM; when host is true, bring it to the state a sandboxed
 * production host leaves it in. Runs bshJitWork. 1 when it ran, 0 on an
 * error, -1 when the VM has no working trace compiler (PUC-Lua, or LuaJIT
 * built without it). */
static int bshJitRun(const char *root, bool host, int *flushes, int *traces,
                     double *kb) {
    BshVm vm;
    int ok = 0;
    *flushes = *traces = 0;
    *kb = 0.0;
    if (!bshVmOpen(&vm, root, false)) return 0;
    if (luaL_dostring(vm.L,
            "if not (jit and jit.status and jit.status()) then return nil end\n"
            "return { attach = jit.attach, util = require('jit.util'), loadstring = loadstring }\n")
            != LUA_OK) {
        fprintf(stderr, "  jit probe failed: %s\n", lua_tostring(vm.L, -1));
        bshVmClose(&vm);
        return 0;
    }
    if (!lua_istable(vm.L, -1)) {
        bshVmClose(&vm);
        return -1;
    }
    if (host) {
        brainSandboxApply(vm.L, root);
        brainSandboxTuneJit(vm.L);
        brainSandboxRestrictJit(vm.L);
    }
    if (luaL_loadstring(vm.L, bshJitWork) == LUA_OK) {
        lua_insert(vm.L, -2);            /* chunk below the probe */
        if (lua_pcall(vm.L, 1, 3, 0) == LUA_OK) {
            *flushes = (int)lua_tointeger(vm.L, -3);
            *traces = (int)lua_tointeger(vm.L, -2);
            *kb = (double)lua_tonumber(vm.L, -1);
            ok = 1;
        } else {
            fprintf(stderr, "  jit work failed: %s\n", lua_tostring(vm.L, -1));
        }
    }
    bshVmClose(&vm);
    return ok;
}

int run_brain_sandbox_jit_limits(void) {
    char root[FILENAME_MAX];
    int flushes, traces, ran;
    double kb;

    UT_ASSERT(bshBrainDir(root, sizeof(root)));

    ran = bshJitRun(root, true, &flushes, &traces, &kb);
    if (ran < 0) {
        printf("  no LuaJIT trace compiler; nothing to check\n");
        return 0;
    }
    UT_ASSERT(ran == 1);
    printf("  host VM: %d flushes, %d traces, %.0f KB machine code\n",
           flushes, traces, kb);
    UT_ASSERT_MSG(flushes == 0, "the host VM flushed its traces %d times",
                  flushes);
    UT_ASSERT_MSG(traces > 1000, "only %d traces, so maxtrace is not above 1000",
                  traces);
    UT_ASSERT_MSG(kb > 4096.0,
                  "only %.0f KB of machine code, so maxmcode is not tested",
                  kb);

    /* The same work with LuaJIT's defaults: the work is big enough to
     * matter. */
    UT_ASSERT(bshJitRun(root, false, &flushes, &traces, &kb) == 1);
    printf("  default VM: %d flushes, %d traces, %.0f KB machine code\n",
           flushes, traces, kb);
    UT_ASSERT_MSG(flushes > 0, "the default VM did not flush, so the work is "
                  "too small to test the limits");
    return 0;
}
