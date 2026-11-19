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
 * run_brain_sandbox_hostile_pack_encode
 *      a brain still cannot require string.buffer and has no
 *      brain_value_decode; brain_value_encode takes one value and gives back
 *      one string, never runs a metamethod, and refuses functions, threads,
 *      userdata, table keys, cycles, nesting too deep and output too large
 * run_brain_sandbox_hostile_pack_decode
 *      the view side's brain_value_decode gives back what was packed, and
 *      refuses a non-string, every cut-short block, unknown bytes, counts
 *      and lengths past the end, bad keys, nesting too deep and bytes left
 *      over; random byte changes give an error or plain data, never a
 *      crash, a function or userdata
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
#include "brain_value_pack.h"
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
    brainValueRegisterEncode(vm->L);   /* as luabrainshandler.c does */
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

int run_brain_sandbox_hostile_pack_encode(void) {
    char root[FILENAME_MAX];
    BshVm vm;

    UT_ASSERT(bshBrainDir(root, sizeof(root)));
    UT_ASSERT(bshVmOpen(&vm, root, true));
    BSH_RUN_OK(vm.L,
        "assert(not pcall(require, 'string.buffer'), 'require(\"string.buffer\") loaded')\n"
        "assert(package.loaded['string.buffer'] == nil, 'string.buffer is in package.loaded')\n"
        "assert(rawget(_G, 'brain_value_decode') == nil, 'a brain has brain_value_decode')\n"
        "local enc = brain_value_encode\n"
        "assert(type(enc) == 'function', 'no brain_value_encode')\n"
        /* One value in, one string out: no buffer object, no pointer. */
        "local shared = { 1, 2 }\n"
        "local cap = { v = 1, S = { 'a', 'b', x = 1.5, [false] = true }, K = { shared, shared },\n"
        "              C = { [7] = -3, [2.5] = 'h', big = 2^60, neg0 = -0.0, nan = 0/0 } }\n"
        "local n = select('#', enc(cap))\n"
        "assert(n == 1, 'brain_value_encode gave back ' .. n .. ' values')\n"
        "local s = enc(cap)\n"
        "assert(type(s) == 'string' and #s > 0, 'brain_value_encode did not give a string')\n"
        "assert(enc(cap) == enc(cap), 'two packs of one table differ')\n"
        "for _, v in ipairs({ 0, -1, 1e300, 'text', true, false }) do\n"
        "  assert(type(enc(v)) == 'string', 'a plain value did not pack')\n"
        "end\n"
        "assert(type(enc(nil)) == 'string', 'nil did not pack')\n"
        "assert(not pcall(enc), 'no argument packed')\n"
        "assert(not pcall(enc, cap, {}), 'an options table was taken')\n"
        "assert(not pcall(enc, cap, 'x'), 'a second argument was taken')\n"
        /* A metatable is not followed: no metamethod runs. */
        "local ran = false\n"
        "local mt = { __index = function() ran = true end, __len = function() ran = true; return 5 end,\n"
        "             __pairs = function() ran = true; return next end, __newindex = function() ran = true end }\n"
        "assert(type(enc(setmetatable({ 1, a = 2 }, mt))) == 'string', 'a table with a metatable did not pack')\n"
        "assert(not ran, 'brain_value_encode ran a metamethod')\n"
        /* Values that are not plain data. */
        "local function refuses(v, what)\n"
        "  local ok, err = pcall(enc, v)\n"
        "  assert(not ok, what .. ' packed')\n"
        "  assert(type(err) == 'string' and err:find('brain_value_encode', 1, true), what .. ': ' .. tostring(err))\n"
        "end\n"
        "refuses(print, 'a function')\n"
        "refuses({ f = function() end }, 'a function in a table')\n"
        "refuses({ [print] = 1 }, 'a function key')\n"
        "refuses({ [{}] = 1 }, 'a table key')\n"
        "refuses(coroutine.create(function() end), 'a thread')\n"
        "if io.stderr then refuses({ io.stderr }, 'userdata') end\n"
        "local c = {}; c.self = c\n"
        "refuses(c, 'a table that holds itself')\n"
        "local a, b = {}, {}; a.b = b; b.a = a\n"
        "refuses(a, 'two tables that hold each other')\n"
        "local arr = {}; arr[1] = arr\n"
        "refuses(arr, 'an array that holds itself')\n"
        "local deep = {}; local d = deep\n"
        "for i = 1, 70 do d.n = {}; d = d.n end\n"
        "refuses(deep, 'tables nested 70 deep')\n"
        "local ok60 = {}; d = ok60\n"
        "for i = 1, 60 do d.n = {}; d = d.n end\n"
        "assert(type(enc(ok60)) == 'string', 'tables nested 60 deep did not pack')\n"
        "refuses(string.rep('x', 17 * 1024 * 1024), 'a string over the size limit')\n"
        /* A table held twice at each level packs twice at each level: the
         * size limit stops it, not the memory. */
        "local t = { string.rep('y', 1024) }\n"
        "for i = 1, 40 do t = { t, t } end\n"
        "refuses(t, 'a shared table that packs past the size limit')\n");
    bshVmClose(&vm);
    return 0;
}

int run_brain_sandbox_hostile_pack_decode(void) {
    /* The view side's VM: plain libraries plus brain_value_decode, as
     * pool_render_host.c opens it, and the encoder to build the input. */
    lua_State *L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);
    brainValueRegisterEncode(L);
    brainValueRegisterDecode(L);
    BSH_RUN_OK(L,
        "local enc, dec = brain_value_encode, brain_value_decode\n"
        "local function same(a, b)\n"
        "  if type(a) ~= type(b) then return false end\n"
        "  if type(a) ~= 'table' then return a == b or (a ~= a and b ~= b) end\n"
        "  for k, v in pairs(a) do if not same(v, b[k]) then return false end end\n"
        "  for k in pairs(b) do if a[k] == nil then return false end end\n"
        "  return true\n"
        "end\n"
        "local cap = { v = 1, S = { 'a', 'b\\0c', x = 1.5, [false] = true, [3] = 'c', [10] = 'gap' },\n"
        "              K = { threat = { [256 * 4 + 9] = 0.25 }, low = false },\n"
        "              C = { [-7] = -3, [2.5] = 'h', big = 2^60, neg = -123456789, tiny = 1e-300,\n"
        "                    inf = 1/0, ninf = -1/0, nan = 0/0 } }\n"
        "local got = dec(enc(cap))\n"
        "assert(same(cap, got), 'the round trip changed the value')\n"
        "assert(getmetatable(got) == nil, 'a decoded table has a metatable')\n"
        "assert(1/dec(enc(-0.0)) < 0, '-0 lost its sign')\n"
        "assert(dec(enc(nil)) == nil and select('#', dec(enc(nil))) == 1, 'nil did not round trip')\n"
        "for _, v in ipairs({ 0, -1, 2^53, -2^53, 2^53 + 2, 0.1, 'x', true, false }) do\n"
        "  assert(dec(enc(v)) == v, 'a plain value did not round trip')\n"
        "end\n"
        /* Malformed input: an error each time, never a crash. */
        "local function refuses(s, what)\n"
        "  local ok, err = pcall(dec, s)\n"
        "  assert(not ok, what .. ' decoded')\n"
        "  assert(type(err) == 'string' and err:find('brain_value_decode', 1, true), what .. ': ' .. tostring(err))\n"
        "end\n"
        "local B = string.char\n"
        "local function nest(n) return string.rep(B(7, 1, 0, 0, 0), n) .. B(7, 0, 0, 0, 0, 8) .. string.rep(B(8), n) end\n"
        "assert(not pcall(dec, 5), 'a number decoded')\n"
        "assert(not pcall(dec, {}), 'a table decoded')\n"
        "assert(not pcall(dec), 'no argument decoded')\n"
        "assert(not pcall(dec, enc(1), 'x'), 'a second argument was taken')\n"
        "refuses('', 'an empty block')\n"
        "refuses(B(0), 'type byte 0')\n"
        "refuses(B(9), 'type byte 9')\n"
        "refuses(B(255), 'type byte 255')\n"
        "refuses(B(2, 2), 'bytes left over')\n"
        "refuses(B(4) .. string.rep(B(255), 11), 'a number that never ends')\n"
        "refuses(B(4) .. string.rep(B(255), 9) .. B(127), 'a number over 64 bits')\n"
        "refuses(B(5, 0, 0, 0), 'a cut-short double')\n"
        "refuses(B(6, 128, 128, 128, 128, 16) .. 'abc', 'a string length past the end')\n"
        "refuses(B(7, 255, 255, 255, 255, 8), 'a table count past the end')\n"
        "refuses(B(7, 0, 0, 0, 0), 'a table with no end mark')\n"
        "refuses(B(7, 1, 0, 0, 0, 1, 8), 'a nil array item')\n"
        "refuses(B(7, 0, 0, 0, 0, 1, 2, 8), 'a nil key')\n"
        "refuses(B(7, 0, 0, 0, 0, 2, 1, 8), 'a nil value')\n"
        "refuses(B(7, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 248, 127, 2, 8), 'a NaN key')\n"
        "refuses(B(7, 0, 0, 0, 0, 7, 0, 0, 0, 0, 8, 2, 8), 'a table key')\n"
        "refuses(nest(70), 'tables nested 70 deep')\n"
        "assert(type(dec(nest(60))) == 'table', 'tables nested 60 deep did not decode')\n"
        "local good = enc(cap)\n"
        "for i = 0, #good - 1 do refuses(good:sub(1, i), 'a block cut to ' .. i .. ' bytes') end\n"
        /* Random byte changes: an error or plain data, nothing else. */
        "local function plain(v, depth)\n"
        "  local t = type(v)\n"
        "  if t == 'table' then\n"
        "    assert(depth < 70 and getmetatable(v) == nil, 'a decoded table is not plain')\n"
        "    for k, x in pairs(v) do plain(k, depth + 1); plain(x, depth + 1) end\n"
        "  else\n"
        "    assert(t == 'nil' or t == 'boolean' or t == 'number' or t == 'string', 'decoded a ' .. t)\n"
        "  end\n"
        "end\n"
        "local unpk = table.unpack or unpack\n"
        "math.randomseed(653)\n"
        "local errs = 0\n"
        "for n = 1, 3000 do\n"
        "  local bytes = { good:byte(1, -1) }\n"
        "  for m = 1, math.random(1, 4) do bytes[math.random(1, #bytes)] = math.random(0, 255) end\n"
        "  local ok, v = pcall(dec, B(unpk(bytes)))\n"
        "  if ok then plain(v, 0) else errs = errs + 1 end\n"
        "end\n"
        "assert(errs > 0, 'no changed block was refused')\n");
    lua_close(L);
    return 0;
}
