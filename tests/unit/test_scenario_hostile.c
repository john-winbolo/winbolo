/*
 * Hostile scenario scripts: one case per known way out of the sandbox.
 *
 * Each case writes a script that tries the one thing, beside a map file of its
 * own, and takes it through the real attach, the round start and the tick. It
 * ends in one of two ways, read off the host's own console wording: the call
 * is refused (the host counts "<hook> raised: ...", or the funnel answers
 * SCN_OP_RATE, or the load is refused), or the script is switched off ("<name>
 * is off for the rest of the round"). Either way the case then runs the round
 * on and checks the tick counter moved and the round is still running. The
 * forged-line case is the one that ends neither way: its print is taken, and
 * what it checks is the one line that reaches the console.
 *
 * scenario_hostile_endless_loop
 *      — while true do end in a hook, stopped by the call's budget
 * scenario_hostile_memory_bomb
 *      — a hook holding more than the memory cap, refused an allocation
 * scenario_hostile_file_open
 *      — io.open, which is not there to call
 * scenario_hostile_process_call
 *      — os.execute, which is not there either
 * scenario_hostile_bytecode_chunk
 *      — a script file that is precompiled bytecode, refused at the load
 * scenario_hostile_debug_call
 *      — debug.getinfo, with no debug table
 * scenario_hostile_ffi_call
 *      — require("ffi") and ffi.C, with neither require nor ffi
 * scenario_hostile_string_bomb
 *      — string.rep asked for a gigabyte, refused by the string cap
 * scenario_hostile_pattern_bomb
 *      — a pattern that backtracks without end, stopped by the charge
 * scenario_hostile_string_metatable_rewrite
 *      — the string metatable's __index replaced; the script's own method
 *        calls raise until it is switched off, and a fresh VM is untouched
 * scenario_hostile_op_flood
 *      — game.log in a loop, refused past SCN_OPS_PER_TICK
 * scenario_hostile_message_flood
 *      — game.message in a loop, refused past SCN_MSGS_PER_TICK
 * scenario_hostile_hook_across_ticks
 *      — coroutine.yield out of a hook body, which ends the hook, and the
 *        next tick's hook with its whole budget
 * scenario_hostile_loop_behind_pcall
 *      — an endless loop inside pcall, raised again rather than caught
 * scenario_hostile_loop_behind_coroutine_resume
 *      — and inside coroutine.resume
 * scenario_hostile_print_forged_line
 *      — a print holding a newline and an escape sequence, trying to write a
 *        line that reads as the server's; it reaches the console as one line
 *        with spaces where the control bytes were
 *
 * A library added to the sandbox's whitelist, or any function that can catch
 * an error, needs a case here beside these.
 *
 * The four whose names were never created — io, os.execute, debug and ffi —
 * assert only that the attempt raises inside the script and the host counts
 * it. That the names are nil is scenario_sandbox_removed_names_are_nil's.
 *
 * Loops are sized against the calibration run_scenario_sandbox_budget_is_per_call
 * gives: a turn of "for i = 1, n do x = x + 1 end" is two to four instructions,
 * LuaJIT at the low end and 5.4 at the high. Each sized loop below shows its
 * working against SCN_BUDGET_CALL_INSTR (1,000,000) and SCN_BUDGET_TICK_INSTR
 * (2,000,000).
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.callbacks, for the console the
                                    * cases read; and scenario_defs.h through
                                    * it, for SCN_OPS_PER_TICK and
                                    * SCN_MSGS_PER_TICK */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "everard_map.h"
#include "server/sim/server_sim_shared.h" /* serverSimSetActive, which is
                                           * what the console routes
                                           * through */
#include "scenario_host.h"
#include "scenario_sandbox.h"      /* scnSandboxMemoryCapped */
#include "test_harness.h"

#include <lauxlib.h>               /* luaL_newstate, luaL_loadbufferx, for
                                    * the bytecode case */

/* ── Fixtures ─────────────────────────────────────────────────────── */

/* Ticks a case runs the round on for after the hostile part, to show it is
   still going. serverSimTick moves the counter at least once a frame. */
#define HF_MORE_TICKS 10

/* A script sits beside the map: X.map is accompanied by X.scenario.lua. The
   map file itself is never written — the host reads the script beside a map
   path and nothing else, and the sims here are built from the built-in
   map. */
static void hfScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

/* Written as bytes rather than as a string, because the bytecode case's file
   is not text and must reach the loader exactly as it stands. */
static bool hfPut(const char *mapPath, const void *bytes, size_t len) {
    char  path[512];
    FILE *f;

    hfScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    if (len > 0 && fwrite(bytes, 1, len, f) != len) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool hfPutText(const char *mapPath, const char *lua) {
    return hfPut(mapPath, lua, strlen(lua));
}

static void hfDrop(const char *mapPath) {
    char path[512];
    hfScriptFor(mapPath, path, sizeof(path));
    remove(path);
}

static ServerSim *hfSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    /* serverSimConsoleMessage writes through the active sim's callback, and
       creating a sim does not make it the active one. */
    serverSimSetActive(sim);
    return sim;
}

/* ── The console ──────────────────────────────────────────────────── */

/* Only consoleMessage is replaced, never the ctx beside it, which the sim's
   other callbacks read. */
static char hfLines[8192];
static size_t hfLinesLen;
static void (*hfConsolePrev)(void *ctx, char *msg) = NULL;

static void hfConsoleCb(void *ctx, char *msg) {
    size_t n;
    size_t room;

    if (hfConsolePrev != NULL) {
        hfConsolePrev(ctx, msg);
    }
    /* Two bytes short of the end is as far as a line can start: one for the
       newline and one for the terminator. */
    if (msg == NULL || hfLinesLen + 2 > sizeof(hfLines)) {
        return;
    }
    n    = strlen(msg);
    room = sizeof(hfLines) - 2 - hfLinesLen;
    if (n > room) {
        n = room;
    }
    memcpy(hfLines + hfLinesLen, msg, n);
    hfLinesLen += n;
    hfLines[hfLinesLen++] = '\n';
    hfLines[hfLinesLen]   = '\0';
}

static void hfWatchConsole(ServerSim *sim) {
    hfLines[0]    = '\0';
    hfLinesLen    = 0;
    hfConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = hfConsoleCb;
}

static void hfUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = hfConsolePrev;
    hfConsolePrev = NULL;
}

/* How many times a line appears in what the console caught. A needle that
   carries its own newline matches a whole line's end. */
static int hfCount(const char *needle) {
    const char *p = hfLines;
    int         n = 0;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

/* Whether needle is somewhere in the n bytes at s. */
static bool hfHas(const char *s, size_t n, const char *needle) {
    size_t k = strlen(needle);
    size_t i;

    if (k > n) {
        return false;
    }
    for (i = 0; i + k <= n; i++) {
        if (memcmp(s + i, needle, k) == 0) {
            return true;
        }
    }
    return false;
}

/* How many console lines hold all of a, b and c. b and c may be NULL. The
   host writes one line per error, so this is what says an error line named
   what the case expects rather than two different lines each holding half. */
static int hfLinesWith(const char *a, const char *b, const char *c) {
    const char *p = hfLines;
    int         n = 0;

    while (*p != '\0') {
        const char *e   = strchr(p, '\n');
        size_t      len = (e != NULL) ? (size_t)(e - p) : strlen(p);

        if (hfHas(p, len, a) && (b == NULL || hfHas(p, len, b)) &&
            (c == NULL || hfHas(p, len, c))) {
            n++;
        }
        if (e == NULL) {
            break;
        }
        p = e + 1;
    }
    return n;
}

/* Attach the script beside map, start the round and run it for ticks
   frames, watching the console throughout. The sim and host are handed back
   for the case to read and put away. */
static int hfRun(const char *map, const char *lua, int ticks,
                 ServerSim **simOut, ScenarioHost **hOut) {
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(hfPutText(map, lua));
    sim = hfSim();
    UT_ASSERT(sim != NULL);

    hfWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, map, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused at the attach: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < ticks; i++) {
        serverSimTick(sim);
    }
    hfUnwatchConsole(sim);
    *simOut = sim;
    *hOut   = h;
    return 0;
}

/* The round runs on: HF_MORE_TICKS more frames, the tick counter moved by at
   least that many, and the state still running. */
static int hfKeepsTicking(ServerSim *sim) {
    uint32_t before = serverSimGetTick(sim);
    int      i;

    for (i = 0; i < HF_MORE_TICKS; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(serverSimGetTick(sim) >= before + (uint32_t)HF_MORE_TICKS,
                  "the tick counter went from %u to %u over %d frames, so the "
                  "server stopped ticking", (unsigned)before,
                  (unsigned)serverSimGetTick(sim), HF_MORE_TICKS);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running",
                  (int)serverSimGetState(sim));
    return 0;
}

static void hfFinish(ServerSim *sim, ScenarioHost *h, const char *map) {
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    hfDrop(map);
}

/* ── One hostile call ─────────────────────────────────────────────── */

/* The shape most of the cases share. on_tick's first call prints "<tag>
 * begun", does the hostile thing and would print "<tag> finished" after it;
 * its second call runs later, then prints "<tag> still here". later is Lua
 * run at the start of that second call, "" for most. Every case sets calls
 * rather than a flag, so a hook the host skipped does not shift the lines. */
#define HF_ONCE_LUA(name, tag, hostile, later)                              \
    "scenario = { name = \"" name "\", api = 1 }\n"                         \
    "local calls = 0\n"                                                     \
    "function on_tick()\n"                                                  \
    "  calls = calls + 1\n"                                                 \
    "  if calls == 1 then\n"                                                \
    "    print(\"" tag " begun\")\n"                                        \
    "    " hostile "\n"                                                     \
    "    print(\"" tag " finished\")\n"                                     \
    "  elseif calls == 2 then\n"                                            \
    "    " later "\n"                                                       \
    "    print(\"" tag " still here\")\n"                                   \
    "  end\n"                                                               \
    "end\n"

/* Frames the one-call cases run: the hostile call, the one after it, and two
   to spare. */
#define HF_ONCE_TICKS 4

/* The call was refused: it began and never finished, the host counted exactly
 * one error, on on_tick, on one line holding need1 and need2 (need2 may be
 * NULL), and the next call answered after it. */
static int hfCheckOnce(const char *tag, const char *need1, const char *need2) {
    char        want[96];
    const char *raised;
    const char *here;

    snprintf(want, sizeof(want), "%s begun\n", tag);
    UT_ASSERT_MSG(hfCount(want) == 1,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", hfLines);
    snprintf(want, sizeof(want), "%s finished\n", tag);
    UT_ASSERT_MSG(hfCount(want) == 0,
                  "the hostile call ran to its end. The console holds:\n%s",
                  hfLines);
    UT_ASSERT_MSG(hfCount(" raised: ") == 1,
                  "%d calls were counted as errors, expected the hostile one. "
                  "The console holds:\n%s", hfCount(" raised: "), hfLines);
    UT_ASSERT_MSG(hfLinesWith("scenario: on_tick raised: ", need1, need2) == 1,
                  "no error line from on_tick names \"%s\"%s%s. The console "
                  "holds:\n%s", need1, need2 != NULL ? " and " : "",
                  need2 != NULL ? need2 : "", hfLines);
    raised = strstr(hfLines, "scenario: on_tick raised: ");
    snprintf(want, sizeof(want), "%s still here\n", tag);
    here = strstr(hfLines, want);
    UT_ASSERT_MSG(here != NULL && here > raised,
                  "no later call answered after the error, so the script's "
                  "state did not survive it. The console holds:\n%s",
                  hfLines);
    return 0;
}

/* Attach, run and check one of the one-call cases, then run the round on and
   put everything away. */
static int hfOnce(const char *map, const char *lua, const char *tag,
                  const char *need1, const char *need2) {
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;

    UT_ASSERT(hfRun(map, lua, HF_ONCE_TICKS, &sim, &h) == 0);
    UT_ASSERT(hfCheckOnce(tag, need1, need2) == 0);
    UT_ASSERT(hfKeepsTicking(sim) == 0);
    hfFinish(sim, h, map);
    return 0;
}

/* ── 1. An endless loop ───────────────────────────────────────────── */

/* Unbounded on purpose: the budget is the only thing that ends it. A loop
 * with an empty body is one or two instructions a turn, so the call's
 * million is reached within a few milliseconds on either VM. A budget that
 * has stopped working hangs this case until CTest's timeout says so. */
int run_scenario_hostile_endless_loop(void) {
    return hfOnce("scnhostile_endless_loop.map",
                  HF_ONCE_LUA("Endless", "endless", "while true do end", ""),
                  "endless", "instruction budget", NULL);
}

/* ── 2. A memory bomb ─────────────────────────────────────────────── */

/* 8192 strings of 16 KiB, every one kept: 128 MiB against a cap of 32, so
 * the refusal lands around the two thousandth. Each value ends in its own
 * index because LuaJIT interns identical content into one allocation however
 * long it is, and a table of the same bytes would hold one string.
 *
 * Instructions: a turn is a rep, a concat and a table store, about ten, so
 * 8192 turns are about 82,000, a twelfth of the call's budget. What stops
 * this is the memory, not the count.
 *
 * The second call collects before anything else. LuaJIT does no emergency
 * collection before it raises, so the table the first call abandoned is
 * still held until something collects it, and the survival line would be
 * refused memory for the same reason the bomb was. */
int run_scenario_hostile_memory_bomb(void) {
    static const char *const kMap = "scnhostile_memory_bomb.map";
    static const char *const kLua =
        HF_ONCE_LUA("Glutton", "glutton",
                    "local t = {} "
                    "for i = 1, 8192 do "
                    "t[i] = string.rep(\"x\", 16384) .. i end",
                    "collectgarbage(\"collect\")");
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;

    UT_ASSERT(hfRun(kMap, kLua, HF_ONCE_TICKS, &sim, &h) == 0);
    if (scnSandboxMemoryCapped()) {
        UT_ASSERT(hfCheckOnce("glutton", "not enough memory", NULL) == 0);
    } else {
        /* A build whose Lua takes no allocator counts nothing, so there is
           no refusal to see; what is left to check is that the hook ran. */
        UT_ASSERT_MSG(hfCount("glutton finished\n") == 1,
                      "this build's Lua takes no allocator, so the hook "
                      "should have run to its end. The console holds:\n%s",
                      hfLines);
    }
    UT_ASSERT(hfKeepsTicking(sim) == 0);
    hfFinish(sim, h, kMap);
    return 0;
}

/* ── 3. A file open ───────────────────────────────────────────────── */

/* There is no io table, so the index raises before anything is opened. The
 * two VMs word it differently — "attempt to index a nil value (global 'io')"
 * and "attempt to index global 'io' (a nil value)" — and both carry the two
 * pieces matched here. */
int run_scenario_hostile_file_open(void) {
    return hfOnce("scnhostile_file_open.map",
                  HF_ONCE_LUA("Opener", "opener",
                              "local f = io.open(\"x\", \"w\")", ""),
                  "opener", "attempt to index", "'io'");
}

/* ── 4. A process call ────────────────────────────────────────────── */

/* os is there and os.execute is not, so the call raises. "attempt to call a
 * nil value (field 'execute')" on 5.4, "attempt to call field 'execute' (a
 * nil value)" on LuaJIT. */
int run_scenario_hostile_process_call(void) {
    return hfOnce("scnhostile_process_call.map",
                  HF_ONCE_LUA("Runner", "runner", "os.execute(\"true\")", ""),
                  "runner", "attempt to call", "'execute'");
}

/* ── 5. A precompiled chunk ───────────────────────────────────────── */

/* A dump writer that fills a fixed buffer, for the bytecode case. */
typedef struct {
    char   bytes[4096];
    size_t len;
    bool   full;
} HfDump;

static int hfDumpWriter(lua_State *L, const void *p, size_t sz, void *ud) {
    HfDump *d = (HfDump *)ud;

    (void)L;
    if (sz > sizeof(d->bytes) - d->len) {
        d->full = true;
        return 1;
    }
    memcpy(d->bytes + d->len, p, sz);
    d->len += sz;
    return 0;
}

/* A real chunk, compiled and dumped by a bare state of the same VM the host
 * runs, so the only thing wrong with it is that it is bytecode. The loader
 * takes text only, and the load is refused before anything in the chunk
 * runs: the attach answers NULL with the VM's own words in the operator
 * line — "attempt to load a binary chunk (mode is 't')" on 5.4, "attempt to
 * load chunk with wrong mode" on LuaJIT — and the round the sim goes on to
 * play has no script in it. */
int run_scenario_hostile_bytecode_chunk(void) {
    static const char *const kMap = "scnhostile_bytecode_chunk.map";
    static const char *const kSrc =
        "print(\"compiled chunk ran\")\n"
        "scenario = { name = \"Compiled\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    lua_State    *C;
    HfDump        dump;
    char          err[512];
    int           rc;

    C = luaL_newstate();
    UT_ASSERT(C != NULL);
    dump.len  = 0;
    dump.full = false;
    rc = luaL_loadbufferx(C, kSrc, strlen(kSrc), "=compiled", "t");
    if (rc == 0) {
#ifdef WINBOLO_LUAJIT
        rc = lua_dump(C, hfDumpWriter, &dump);
#else
        rc = lua_dump(C, hfDumpWriter, &dump, 0);
#endif
    }
    lua_close(C);
    UT_ASSERT_MSG(rc == 0 && !dump.full && dump.len > 0,
                  "the source did not compile and dump (rc %d, %u bytes)",
                  rc, (unsigned)dump.len);

    UT_ASSERT(hfPut(kMap, dump.bytes, dump.len));
    sim = hfSim();
    UT_ASSERT(sim != NULL);

    hfWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    hfUnwatchConsole(sim);

    UT_ASSERT_MSG(h == NULL, "a precompiled script file was attached");
    UT_ASSERT_MSG(strncmp(err, "scenario: ", 10) == 0 &&
                  strstr(err, "attempt to load") != NULL,
                  "the refusal did not come from the load: %s", err);
    UT_ASSERT_MSG(hfCount("compiled chunk ran") == 0,
                  "the chunk's top level ran. The console holds:\n%s",
                  hfLines);

    serverSimStartGame(sim);
    UT_ASSERT(hfKeepsTicking(sim) == 0);
    serverSimDestroy(sim);
    hfDrop(kMap);
    return 0;
}

/* ── 6. A debug call ──────────────────────────────────────────────── */

/* No debug table: "attempt to index a nil value (global 'debug')" on 5.4,
 * "attempt to index global 'debug' (a nil value)" on LuaJIT. */
int run_scenario_hostile_debug_call(void) {
    return hfOnce("scnhostile_debug_call.map",
                  HF_ONCE_LUA("Debugger", "debugger", "debug.getinfo(1)", ""),
                  "debugger", "attempt to index", "'debug'");
}

/* ── 7. An ffi call ───────────────────────────────────────────────── */

/* The two ways a LuaJIT script reaches ffi: through require, and through a
 * global had one been left. Neither is there, on either VM, so each raises
 * in its own call — the first on calling require, the second on indexing
 * ffi — and a third call answers. */
int run_scenario_hostile_ffi_call(void) {
    static const char *const kMap = "scnhostile_ffi_call.map";
    static const char *const kLua =
        "scenario = { name = \"Foreign\", api = 1 }\n"
        "local calls = 0\n"
        "function on_tick()\n"
        "  calls = calls + 1\n"
        "  if calls == 1 then\n"
        "    print(\"foreign begun\")\n"
        "    local f = require(\"ffi\")\n"
        "    print(\"foreign required\")\n"
        "  elseif calls == 2 then\n"
        "    local c = ffi.C\n"
        "    print(\"foreign indexed\")\n"
        "  elseif calls == 3 then\n"
        "    print(\"foreign still here\")\n"
        "  end\n"
        "end\n";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    const char   *here;

    UT_ASSERT(hfRun(kMap, kLua, HF_ONCE_TICKS + 1, &sim, &h) == 0);

    UT_ASSERT_MSG(hfCount("foreign begun\n") == 1,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", hfLines);
    UT_ASSERT_MSG(hfCount("foreign required") == 0 &&
                  hfCount("foreign indexed") == 0,
                  "a call reached ffi. The console holds:\n%s", hfLines);
    UT_ASSERT_MSG(hfCount(" raised: ") == 2,
                  "%d calls were counted as errors, expected the two that "
                  "reached for ffi. The console holds:\n%s",
                  hfCount(" raised: "), hfLines);
    UT_ASSERT_MSG(hfLinesWith("scenario: on_tick raised: ", "attempt to call",
                              "'require'") == 1,
                  "no error line says require could not be called. The "
                  "console holds:\n%s", hfLines);
    UT_ASSERT_MSG(hfLinesWith("scenario: on_tick raised: ", "attempt to index",
                              "'ffi'") == 1,
                  "no error line says ffi could not be indexed. The console "
                  "holds:\n%s", hfLines);
    here = strstr(hfLines, "foreign still here\n");
    UT_ASSERT_MSG(here != NULL, "no later call answered. The console "
                  "holds:\n%s", hfLines);
    UT_ASSERT(hfKeepsTicking(sim) == 0);
    hfFinish(sim, h, kMap);
    return 0;
}

/* ── 8. A string bomb ─────────────────────────────────────────────── */

/* A gigabyte from one string.rep. The count hook cannot see inside a C
 * function, so this is the string cap's: refused before anything is built,
 * in the sandbox's own words, which give the length it would have come to. */
int run_scenario_hostile_string_bomb(void) {
    return hfOnce("scnhostile_string_bomb.map",
                  HF_ONCE_LUA("Stringer", "stringer",
                              "local s = string.rep(\"x\", 2^30)", ""),
                  "stringer",
                  "string.rep would build a string of 1073741824 bytes",
                  NULL);
}

/* ── 9. A pattern bomb ────────────────────────────────────────────── */

/* ".-.-.-b" over sixty thousand a's is on the order of n⁴ matcher steps. The
 * subject is under SCN_STRING_MAX, so the cap has nothing to say; the steps
 * are charged to the call's budget and it is stopped there. */
int run_scenario_hostile_pattern_bomb(void) {
    return hfOnce("scnhostile_pattern_bomb.map",
                  HF_ONCE_LUA("Backtracker", "backtracker",
                              "string.find(string.rep(\"a\", 60000), "
                              "\".-.-.-b\")", ""),
                  "backtracker", "instruction budget", NULL);
}

/* ── 10. A string metatable rewrite ───────────────────────────────── */

/* Nothing guards the string metatable, so a script can replace its __index,
 * and every method call on a string in that VM then goes to the script's own
 * function. What this case shows is that the rewrite stays in the VM that
 * made it.
 *
 * on_start makes the rewrite and on_tick calls a method every frame, so each
 * on_tick raises. The count reaches SCN_ERROR_LIMIT and the script is
 * switched off, the round going on without it. Then a second sim, with a
 * plain script of its own, calls the same method in a later hook and gets
 * the ordinary answer: the rewrite went with the first VM. */
int run_scenario_hostile_string_metatable_rewrite(void) {
    static const char *const kMap      = "scnhostile_string_metatable.map";
    static const char *const kMapAfter = "scnhostile_string_metatable_after.map";
    static const char *const kLua =
        "scenario = { name = \"Rewriter\", api = 1 }\n"
        "function on_start()\n"
        "  getmetatable(\"\").__index = function() error(\"hijacked\") end\n"
        "  print(\"rewriter done\")\n"
        "end\n"
        "function on_tick()\n"
        "  local s = (\"x\"):rep(2)\n"
        "  print(\"rewriter method answered \" .. s)\n"
        "end\n";
    static const char *const kLuaAfter =
        "scenario = { name = \"After\", api = 1 }\n"
        "local calls = 0\n"
        "function on_tick()\n"
        "  calls = calls + 1\n"
        "  if calls == 2 then\n"
        "    print(\"after method \" .. (\"x\"):rep(2))\n"
        "  end\n"
        "end\n";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          off[128];

    UT_ASSERT(hfRun(kMap, kLua, SCN_ERROR_LIMIT + 5, &sim, &h) == 0);

    UT_ASSERT_MSG(hfCount("rewriter done\n") == 1,
                  "on_start never made the rewrite, so nothing here was "
                  "tested. The console holds:\n%s", hfLines);
    UT_ASSERT_MSG(hfLinesWith("scenario: on_tick raised: ", "hijacked",
                              NULL) == SCN_ERROR_LIMIT,
                  "%d method calls raised through the rewritten __index, "
                  "expected %d before the script was switched off. The "
                  "console holds:\n%s",
                  hfLinesWith("scenario: on_tick raised: ", "hijacked", NULL),
                  SCN_ERROR_LIMIT, hfLines);
    UT_ASSERT_MSG(hfCount(" raised: ") == SCN_ERROR_LIMIT,
                  "%d calls were counted as errors, expected %d. The console "
                  "holds:\n%s", hfCount(" raised: "), SCN_ERROR_LIMIT,
                  hfLines);
    snprintf(off, sizeof(off),
             "scenario: Rewriter is off for the rest of the round: %d errors "
             "in a row.\n", SCN_ERROR_LIMIT);
    UT_ASSERT_MSG(hfCount(off) == 1,
                  "the script was not switched off. The console holds:\n%s",
                  hfLines);
    UT_ASSERT(hfKeepsTicking(sim) == 0);
    hfFinish(sim, h, kMap);

    /* A fresh sim and a fresh VM: the method answers as it always does. */
    UT_ASSERT(hfRun(kMapAfter, kLuaAfter, HF_ONCE_TICKS, &sim, &h) == 0);
    UT_ASSERT_MSG(hfCount("after method xx\n") == 1,
                  "a method call in a later sim's hook did not answer \"xx\", "
                  "so the rewrite reached past the VM that made it. The "
                  "console holds:\n%s", hfLines);
    UT_ASSERT_MSG(hfCount(" raised: ") == 0,
                  "the plain script raised. The console holds:\n%s", hfLines);
    UT_ASSERT(hfKeepsTicking(sim) == 0);
    hfFinish(sim, h, kMapAfter);
    return 0;
}

/* ── 11 and 12. Floods ────────────────────────────────────────────── */

/* How many ops a flood sends from one call: four times the tick's op
 * allowance, so the refusals outnumber what applies whichever limit it is.
 *
 * Instructions: a turn reads game and the row, loads the text, calls, tests
 * the answer and adds one — about ten VM instructions on either VM, since the
 * C side of the row is not counted by the hook. 1024 turns are then about
 * 10,000 instructions, a hundredth of the call's budget, so the loop always
 * finishes and it is the funnel that says no. A loop of the plan's hundred
 * thousand turns would be a million instructions at that rate, and the
 * budget rather than the funnel would stop it. */
#define HF_FLOOD (SCN_OPS_PER_TICK * 4)

/* on_tick's first call sends HF_FLOOD of one op and counts what the funnel
 * answered, as the script sees it: true for applied, SCN_OP_RATE for refused
 * at the tick's allowance, anything else as other. Its second call, in the
 * next frame, sends one more and prints what that one answered. */
static int hfFlood(const char *map, const char *tag, const char *call,
                   int applied) {
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          lua[1536];
    char          want[128];

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Flood\", api = 1 }\n"
             "local calls = 0\n"
             "function on_tick()\n"
             "  calls = calls + 1\n"
             "  if calls == 1 then\n"
             "    local applied, rate, other = 0, 0, 0\n"
             "    for i = 1, %d do\n"
             "      local ok, why = %s(\"%s op\")\n"
             "      if ok then applied = applied + 1\n"
             "      elseif why == \"SCN_OP_RATE\" then rate = rate + 1\n"
             "      else other = other + 1 end\n"
             "    end\n"
             "    print(\"%s applied \" .. applied .. \" rate \" .. rate ..\n"
             "          \" other \" .. other)\n"
             "  elseif calls == 2 then\n"
             "    print(\"%s next \" .. tostring((%s(\"%s next\"))))\n"
             "  end\n"
             "end\n", HF_FLOOD, call, tag, tag, tag, call, tag);

    UT_ASSERT(hfRun(map, lua, HF_ONCE_TICKS, &sim, &h) == 0);

    snprintf(want, sizeof(want), "%s applied %d rate %d other 0\n", tag,
             applied, HF_FLOOD - applied);
    UT_ASSERT_MSG(hfCount(want) == 1,
                  "expected the line \"%.*s\": %d applied and the rest "
                  "refused SCN_OP_RATE. The console holds:\n%s",
                  (int)strlen(want) - 1, want, applied, hfLines);
    snprintf(want, sizeof(want), "%s next true\n", tag);
    UT_ASSERT_MSG(hfCount(want) == 1,
                  "the frame after the flood did not hand the allowance back. "
                  "The console holds:\n%s", hfLines);
    UT_ASSERT_MSG(hfCount(" raised: ") == 0,
                  "a refusal was counted as an error: the funnel answers a "
                  "script, it does not raise into it. The console holds:\n%s",
                  hfLines);
    UT_ASSERT(hfKeepsTicking(sim) == 0);
    hfFinish(sim, h, map);
    return 0;
}

/* game.log is an op and not a message, so SCN_OPS_PER_TICK is its only
 * limit. Each one applied writes its text to the console, so the console
 * holds exactly that many of them as well. */
int run_scenario_hostile_op_flood(void) {
    UT_ASSERT(hfFlood("scnhostile_op_flood.map", "oflood", "game.log",
                      SCN_OPS_PER_TICK) == 0);
    /* hfFlood has put the sim away, and with it nothing that reads the
       console; the lines it caught are still in hfLines. */
    UT_ASSERT_MSG(hfCount("oflood op\n") == SCN_OPS_PER_TICK,
                  "%d logged lines reached the console, expected %d. The "
                  "console holds:\n%s", hfCount("oflood op\n"),
                  SCN_OPS_PER_TICK, hfLines);
    return 0;
}

/* game.message reaches the players, so SCN_MSGS_PER_TICK is its limit. */
int run_scenario_hostile_message_flood(void) {
    return hfFlood("scnhostile_message_flood.map", "mflood", "game.message",
                   SCN_MSGS_PER_TICK);
}

/* ── 13. A hook that tries to run across ticks ────────────────────── */

/* A coroutine.yield straight out of a hook body, not inside a coroutine the
 * script made. The host calls a hook with lua_pcall and resumes nothing, so
 * the yield raises there and the hook ends: "attempt to yield across C-call
 * boundary" on LuaJIT, "attempt to yield from outside a coroutine" on 5.4,
 * which calls hooks on its main thread. Both begin "attempt to yield".
 *
 * The next frame's call then spins 125,000 turns: 250,000 to 500,000
 * instructions, a quarter to a half of the call's budget and inside the
 * tick's, on either VM. It finishes only if that call had its whole budget. */
int run_scenario_hostile_hook_across_ticks(void) {
    return hfOnce("scnhostile_hook_across_ticks.map",
                  HF_ONCE_LUA("Yielder", "yielder", "coroutine.yield()",
                              "local x = 0 "
                              "for i = 1, 125000 do x = x + 1 end"),
                  "yielder", "attempt to yield", NULL);
}

/* ── 14 and 15. An endless loop behind a catcher ──────────────────── */

/* The loop of case 1 inside pcall, and inside a coroutine resumed from the
 * hook. The budget's error sets a latch the sandbox's pcall and resume read,
 * so each raises it again rather than answering false: the "caught" line is
 * never printed, and the hook is the call the host counts. */
int run_scenario_hostile_loop_behind_pcall(void) {
    return hfOnce("scnhostile_loop_behind_pcall.map",
                  HF_ONCE_LUA("Catcher", "pcatcher",
                              "local ok = pcall(function() "
                              "while true do end end) "
                              "print(\"pcatcher caught \" .. tostring(ok))",
                              ""),
                  "pcatcher", "instruction budget", NULL);
}

int run_scenario_hostile_loop_behind_coroutine_resume(void) {
    return hfOnce("scnhostile_loop_behind_resume.map",
                  HF_ONCE_LUA("Resumer", "rcatcher",
                              "local ok = coroutine.resume("
                              "coroutine.create(function() "
                              "while true do end end)) "
                              "print(\"rcatcher caught \" .. tostring(ok))",
                              ""),
                  "rcatcher", "instruction budget", NULL);
}

/* ── 16. A print that forges a line ───────────────────────────────── */

/* One print whose text holds a newline, a carriage return and an escape
 * sequence, trying to write a second line that reads as the server's own and
 * to colour the operator's terminal. print turns each control byte into a
 * space, so the whole of it reaches the console as one line.
 *
 * The console here joins what it is handed with newlines, one per
 * serverSimConsoleMessage, so a line of it never spans two messages: the line
 * holding both "forge start" and "forged line" is one message holding both.
 * Had the newline gone through, the two would sit on separate lines. */
int run_scenario_hostile_print_forged_line(void) {
    static const char *const kMap = "scnhostile_print_forged_line.map";
    static const char *const kLua =
        "scenario = { name = \"Forger\", api = 1 }\n"
        "local calls = 0\n"
        "function on_tick()\n"
        "  calls = calls + 1\n"
        "  if calls == 1 then\n"
        "    print(\"forge start\\nscenario: forged line\\r\\27[31mred\\tend\")\n"
        "  end\n"
        "end\n";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    const char   *at;
    const char   *start;
    const char   *e;
    size_t        len;
    size_t        i;

    UT_ASSERT(hfRun(kMap, kLua, HF_ONCE_TICKS, &sim, &h) == 0);

    UT_ASSERT_MSG(hfCount("forge start") == 1,
                  "%d console lines hold \"forge start\", expected the one "
                  "print. The console holds:\n%s", hfCount("forge start"),
                  hfLines);
    UT_ASSERT_MSG(hfLinesWith("forge start", "forged line", "red") == 1 &&
                  hfLinesWith("forge start", "end", NULL) == 1,
                  "the printed text did not reach the console as one line. "
                  "The console holds:\n%s", hfLines);
    UT_ASSERT_MSG(strncmp(hfLines, "scenario: forged line", 21) != 0 &&
                  hfCount("\nscenario: forged line") == 0,
                  "a console line begins with the forged text. The console "
                  "holds:\n%s", hfLines);

    /* The line itself, from its start to the newline the capture put after
       it. */
    at    = strstr(hfLines, "forge start");
    start = at;
    while (start > hfLines && start[-1] != '\n') {
        start--;
    }
    e   = strchr(at, '\n');
    len = (e != NULL) ? (size_t)(e - start) : strlen(start);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        UT_ASSERT_MSG((c >= 0x20 || c == '\t') && c != 0x7f,
                      "byte %u of the printed line is 0x%02x, a control "
                      "character that reached the console. The line is:\n%.*s",
                      (unsigned)i, (unsigned)c, (int)len, start);
    }
    UT_ASSERT_MSG(hfHas(start, len, "red\tend"),
                  "the tab between \"red\" and \"end\" did not survive. The "
                  "line is:\n%.*s", (int)len, start);

    UT_ASSERT(hfKeepsTicking(sim) == 0);
    hfFinish(sim, h, kMap);
    return 0;
}
