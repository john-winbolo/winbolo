/*
 * The library a scenario state is opened with.
 *
 * A scenario script travels inside a map file from the packaging work
 * onwards, so what it can reach is what these cases are about: the names the
 * whitelist takes away, the ones it keeps, the chunk it refuses to load at
 * all, and where a script's print ends up.
 *
 * Each case drives the real attach, because scnNewVm is what the sandbox
 * lands in and the attach is the shortest path to it. A script that finds
 * something wrong raises from its own top level, which fails the attach and
 * carries the reason out in the operator line — so a failure here names the
 * leak rather than saying only that the attach failed.
 *
 * run_scenario_sandbox_removed_names_are_nil
 *      — every name the whitelist takes is nil, and the ones it keeps answer
 * run_scenario_sandbox_bytecode_chunk_refused
 *      — a chunk that starts with the binary signature is refused by Lua
 * run_scenario_sandbox_collectgarbage_stop_refused
 *      — collectgarbage("stop") raises and the other options still work
 * run_scenario_sandbox_print_reaches_the_console
 *      — a script's print arrives on the server console
 * run_scenario_sandbox_memory_cap_refuses
 *      — a hook that holds more than the cap is refused, and the round
 *        goes on ticking
 * run_scenario_sandbox_state_survives_a_refusal
 *      — and the same round's VM still answers afterwards
 * run_scenario_sandbox_instruction_budget_cuts_a_loop
 *      — a hook that spins past the budget is stopped, and the round goes
 *        on ticking
 * run_scenario_sandbox_budget_is_per_call
 *      — sixteen long but legal calls each get the whole budget
 * run_scenario_sandbox_state_survives_the_budget
 *      — and a later hook still runs after one was cut off
 * run_scenario_sandbox_budget_survives_a_nested_call
 *      — a hook that triggers a policy is still counted afterwards
 * run_scenario_sandbox_budget_survives_a_pcall
 *      — a hook that catches the budget's error with pcall has it raised
 *        again rather than carrying on
 * run_scenario_sandbox_budget_survives_a_coroutine
 *      — and the same through coroutine.resume
 * run_scenario_sandbox_os_date_refuses_a_bad_format
 *      — os.date takes the portable conversion characters and raises on the
 *        rest, with the formats a script really writes still answering
 * run_scenario_sandbox_print_bounded_in_one_call
 *      — a hook printing past the per-call bound has the rest dropped, with
 *        one line saying so, and the round goes on
 * run_scenario_sandbox_print_allowance_returns
 *      — a hook printing modestly is never cut, over enough ticks that a
 *        count climbing across the round would show
 * run_scenario_sandbox_string_cap_on_results
 *      — rep, format and concat build a string of exactly the cap and
 *        refuse one a byte longer, rep through a method call as well
 * run_scenario_sandbox_string_cap_on_subjects
 *      — find, match, gmatch and gsub search a subject of exactly the cap
 *        and refuse one a byte longer
 * run_scenario_sandbox_tick_budget_cuts_a_drain
 *      — timers that together pass the tick's total stop at the one that
 *        went over, and the rest run on a later tick
 * run_scenario_sandbox_tick_budget_returns
 *      — the tick after one that ran out has the whole total again
 * run_scenario_sandbox_tick_budget_survives_a_pcall
 *      — pcall and coroutine.resume raise the tick's error again
 * run_scenario_sandbox_tick_budget_spares_on_end
 *      — on_end runs in the tick that ran out, on its own budget
 * run_scenario_sandbox_tick_budget_switches_off
 *      — a script that runs out every tick is switched off at the limit
 * run_scenario_sandbox_pattern_bomb_stopped
 *      — a pattern that backtracks without end over a subject under the cap
 *        is stopped by the instruction budget, and the round goes on
 * run_scenario_sandbox_pattern_bomb_behind_pcall
 *      — and pcall and coroutine.resume raise that error again
 * run_scenario_sandbox_pattern_results
 *      — find, match, gmatch and gsub answer as Lua 5.4 does on either VM
 * run_scenario_sandbox_pattern_charge_counts
 *      — pattern work past the budget in one call is stopped, and the same
 *        work spread over several ticks finishes
 * run_scenario_sandbox_interpreted_cost
 *      — the same on_tick timed on a state with the count hook, on one
 *        without, and on LuaJIT on one with the compiler on, printed as one
 *        bench: line; nothing is asserted about speed
 * run_scenario_sandbox_tick_stats_recorded
 *      — the instructions and time a scenario's tick cost reach the sim for
 *        the info to read, and a tick that ran out is counted as a trip
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "control_event.h"         /* ControlEvent, for the lobby case */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.callbacks, for the console
                                    * the print case reads */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "everard_map.h"
#include "server/sim/server_sim_shared.h" /* serverSimSetActive, which is
                                           * what the console routes
                                           * through */
#include "scenario_host.h"
#include "scenario_sandbox.h"      /* scnSandboxMemoryCapped */
#include "scenario_validate.h"     /* scnNewVm, for the bench case */
#include "test_harness.h"

#include <lauxlib.h>               /* luaL_newstate, luaL_loadbufferx */
#include <lualib.h>                /* luaopen_jit, for the bench case */

/* ── Fixtures ─────────────────────────────────────────────────────── */

/* A script sits beside the map: X.map is accompanied by X.scenario.lua. The
   map file itself is never written — the host reads the script beside a map
   path and nothing else, and the sims here are built from the built-in
   map. */
static void sbScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

/* Written as bytes rather than as a string, because one case's file is not
   text and must reach the loader exactly as it stands. */
static bool sbPut(const char *mapPath, const void *bytes, size_t len) {
    char  path[512];
    FILE *f;

    sbScriptFor(mapPath, path, sizeof(path));
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

static bool sbPutText(const char *mapPath, const char *lua) {
    return sbPut(mapPath, lua, strlen(lua));
}

static void sbDrop(const char *mapPath) {
    char path[512];
    sbScriptFor(mapPath, path, sizeof(path));
    remove(path);
}

static ServerSim *sbSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    /* serverSimConsoleMessage writes through the active sim's callback and
       falls back to stdout when there is none, and creating a sim does not
       make it the active one — the round start and the tick do. A line a
       script prints from its own top level arrives during the attach, before
       either of those has run, so the case says which sim it means here. */
    serverSimSetActive(sim);
    return sim;
}

/* ── The console, for the print case ──────────────────────────────── */

/* Only consoleMessage is replaced, never the ctx beside it, which the sim's
   other callbacks read. */
static char sbLines[8192];
static size_t sbLinesLen;
static void (*sbConsolePrev)(void *ctx, char *msg) = NULL;

static void sbConsoleCb(void *ctx, char *msg) {
    size_t n;
    size_t room;

    if (sbConsolePrev != NULL) {
        sbConsolePrev(ctx, msg);
    }
    /* Two bytes short of the end is as far as a line can start: one for the
       newline and one for the terminator. */
    if (msg == NULL || sbLinesLen + 2 > sizeof(sbLines)) {
        return;
    }
    n    = strlen(msg);
    room = sizeof(sbLines) - 2 - sbLinesLen;
    if (n > room) {
        n = room;
    }
    memcpy(sbLines + sbLinesLen, msg, n);
    sbLinesLen += n;
    sbLines[sbLinesLen++] = '\n';
    sbLines[sbLinesLen]   = '\0';
}

static void sbWatchConsole(ServerSim *sim) {
    sbLines[0]    = '\0';
    sbLinesLen    = 0;
    sbConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = sbConsoleCb;
}

static void sbUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = sbConsolePrev;
    sbConsolePrev = NULL;
}

/* How many times a line appears in what the console caught. The cases below
   pass a needle carrying its own newline, so "said 6\n" is not found inside
   "said 64". */
static int sbCount(const char *needle) {
    const char *p = sbLines;
    int         n = 0;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

/* ── 1. What the whitelist took, and what it kept ─────────────────── */

/* The script builds a list of everything that is wrong and raises with it,
 * so the attach fails and the operator line names the leak. An attach that
 * succeeds is the whole assertion: nothing was there that should not be, and
 * everything that should be there answered a call.
 *
 * math.randomseed is on the absent list and math.random on the present one,
 * which is the pair that says the host drew the round's seed before the seal
 * rather than losing the seeding with the function.
 *
 * The present lists name every global the whitelist keeps rather than a few
 * of them, because the whitelist is a keep list: a name dropped from it is a
 * global deleted, and a case that only watched the deletions would let that
 * through. unpack and rawlen are the one pair asked for together — 5.1 has
 * the first and 5.2 the second, and either answers for both. */
int run_scenario_sandbox_removed_names_are_nil(void) {
    static const char *const kMap = "scnsand_names.map";
    static const char *const kLua =
        "local leaks = {}\n"
        "local gone = { \"io\", \"package\", \"require\", \"debug\", \"ffi\",\n"
        "               \"jit\", \"bit\", \"load\", \"loadstring\", \"dofile\",\n"
        "               \"loadfile\", \"module\", \"newproxy\", \"getfenv\",\n"
        "               \"setfenv\", \"gcinfo\" }\n"
        "for _, n in ipairs(gone) do\n"
        "  if _G[n] ~= nil then leaks[#leaks + 1] = n end\n"
        "end\n"
        "if string.dump ~= nil then leaks[#leaks + 1] = \"string.dump\" end\n"
        "if math.randomseed ~= nil then\n"
        "  leaks[#leaks + 1] = \"math.randomseed\"\n"
        "end\n"
        "local osGone = { \"execute\", \"exit\", \"getenv\", \"remove\",\n"
        "                 \"rename\", \"setlocale\", \"tmpname\" }\n"
        "for _, n in ipairs(osGone) do\n"
        "  if os[n] ~= nil then leaks[#leaks + 1] = \"os.\" .. n end\n"
        "end\n"
        "local kept = { \"assert\", \"collectgarbage\", \"error\",\n"
        "               \"getmetatable\", \"ipairs\", \"next\", \"pairs\",\n"
        "               \"pcall\", \"print\", \"rawequal\", \"rawget\",\n"
        "               \"rawset\", \"select\", \"setmetatable\",\n"
        "               \"tonumber\", \"tostring\", \"type\", \"xpcall\" }\n"
        "for _, n in ipairs(kept) do\n"
        "  if type(_G[n]) ~= \"function\" then\n"
        "    leaks[#leaks + 1] = \"no \" .. n\n"
        "  end\n"
        "end\n"
        "local keptTables = { \"_G\", \"coroutine\", \"string\", \"table\",\n"
        "                     \"math\", \"os\" }\n"
        "for _, n in ipairs(keptTables) do\n"
        "  if type(_G[n]) ~= \"table\" then leaks[#leaks + 1] = \"no \" .. n end\n"
        "end\n"
        "if type(_VERSION) ~= \"string\" then\n"
        "  leaks[#leaks + 1] = \"no _VERSION\"\n"
        "end\n"
        "if unpack == nil and rawlen == nil then\n"
        "  leaks[#leaks + 1] = \"no unpack and no rawlen\"\n"
        "end\n"
        "local calls = {\n"
        "  [\"string.format\"] = function() return string.format(\"%d\", 1) end,\n"
        "  [\"table.concat\"] = function()\n"
        "    return table.concat({ \"a\", \"b\" }, \",\")\n"
        "  end,\n"
        "  [\"math.random\"] = function() return math.random(1, 2) end,\n"
        "  [\"os.time\"] = function() return os.time() end,\n"
        "  [\"os.clock\"] = function() return os.clock() end,\n"
        "  [\"os.date\"] = function() return os.date(\"%Y\") end,\n"
        "  [\"os.difftime\"] = function()\n"
        "    return os.difftime(os.time(), os.time())\n"
        "  end,\n"
        "  [\"coroutine.create\"] = function()\n"
        "    return coroutine.create(function() end)\n"
        "  end,\n"
        "}\n"
        "for n, f in pairs(calls) do\n"
        "  local ok, e = pcall(f)\n"
        "  if not ok then\n"
        "    leaks[#leaks + 1] = \"no \" .. n .. \" (\" .. tostring(e) .. \")\"\n"
        "  end\n"
        "end\n"
        "if #leaks > 0 then\n"
        "  error(\"sandbox: \" .. table.concat(leaks, \", \"))\n"
        "end\n"
        "scenario = { name = \"Sandbox\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL,
                  "the script raised, which is what it does when the library "
                  "is not the one the whitelist opens: %s", err);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 2. A precompiled chunk ───────────────────────────────────────── */

/* Bytes beginning with Lua's binary-chunk signature. The loader is asked for
 * text only, so it refuses these before it reads any of them — which is what
 * a script arriving inside a map file needs, since bytecode is a stream the
 * VM trusts and does not check.
 *
 * The message is Lua's own and the two builds word it differently, so the
 * assertion is that the refusal came from the load rather than what it
 * says. */
int run_scenario_sandbox_bytecode_chunk_refused(void) {
    static const char *const kMap = "scnsand_bytecode.map";
    static const char        kBytes[] =
        "\x1b" "Lua not really a compiled chunk\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(sbPut(kMap, kBytes, sizeof(kBytes) - 1));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);

    UT_ASSERT_MSG(h == NULL,
                  "a chunk beginning with the binary signature was attached");
    UT_ASSERT_MSG(err[0] != '\0', "the refusal produced no operator line");
    UT_ASSERT_MSG(strstr(err, "load") != NULL,
                  "the refusal did not come from the load: %s", err);
    return 0;
}

/* ── 3. collectgarbage ────────────────────────────────────────────── */

/* "stop" raises, because a script that could stop the collector could grow
 * the state without bound while every allocation it made stayed legitimate.
 * "count" answers a number, which is what says the rest of the function is
 * still the one Lua provides rather than a refusal wearing its name. */
int run_scenario_sandbox_collectgarbage_stop_refused(void) {
    static const char *const kMap = "scnsand_gc.map";
    static const char *const kLua =
        "local ok = pcall(collectgarbage, \"stop\")\n"
        "if ok then error(\"collectgarbage('stop') was allowed\") end\n"
        "local n = collectgarbage(\"count\")\n"
        "if type(n) ~= \"number\" then\n"
        "  error(\"collectgarbage('count') answered a \" .. type(n))\n"
        "end\n"
        "scenario = { name = \"Collector\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script raised: %s", err);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 4. print ─────────────────────────────────────────────────────── */

/* Stock print writes to the host's stdout, which a dedicated server's
 * operator is not necessarily reading, so the sandbox sends it to the server
 * console instead. The watcher is installed before the attach, because the
 * line is printed from the chunk's own top level. */
int run_scenario_sandbox_print_reaches_the_console(void) {
    static const char *const kMap  = "scnsand_print.map";
    static const char *const kLine = "sandbox print reached the console";
    static const char *const kLua =
        "print(\"sandbox print reached the console\")\n"
        "scenario = { name = \"Printer\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    UT_ASSERT_MSG(strstr(sbLines, kLine) != NULL,
                  "the console caught '%s', which does not hold the line the "
                  "script printed", sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 5. The memory a state may hold ───────────────────────────────── */

/* The hook asks for 8192 strings of 16 KiB and keeps every one of them in a
 * table, so nothing it has asked for can be collected: 128 MiB against a cap
 * of 32, with the refusal landing around the two thousandth. The loop is
 * bounded rather than endless so that a build counting nothing ends it
 * instead of running the machine out of memory — which is what the second
 * half of the assertion below is for.
 *
 * Each stored string has to differ from the last, which is what the index on
 * the end is for. LuaJIT interns every string it is handed, at any length:
 * lj_str_new hashes the content, walks the chain and returns the existing
 * object on a match, so a table filled with the same bytes over and over
 * holds one allocation and costs nothing but its own slots. A fixture that
 * wants a state to really hold what it asked for has to make each value its
 * own.
 *
 * 16 KiB rather than something larger because a later string.rep cap is
 * coming at 65,536 bytes, and a fixture sitting on that boundary would one
 * day fail for a reason that has nothing to do with the memory it is about.
 *
 * The hook prints on the way in and on the way out. Reaching the cap shows
 * as the first line without the second, beside the host's own line naming
 * both the hook and the memory: the refusal is an ordinary Lua error caught
 * by the lua_pcall every hook is called through, and the round goes on
 * ticking afterwards, which is the whole point of counting rather than
 * letting the process take it. */
int run_scenario_sandbox_memory_cap_refuses(void) {
    static const char *const kMap = "scnsand_memcap.map";
    static const char *const kLua =
        "scenario = { name = \"Glutton\", api = 1 }\n"
        "function on_start()\n"
        "  print(\"glutton begun\")\n"
        "  local t = {}\n"
        "  for i = 1, 8192 do\n"
        "    t[i] = string.rep(\"x\", 16384) .. i\n"
        "  end\n"
        "  print(\"glutton finished\")\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    serverSimTick(sim);              /* the first running tick: on_start */
    for (i = 0; i < 5; i++) {
        serverSimTick(sim);          /* and the round after it */
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(strstr(sbLines, "glutton begun") != NULL,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    if (scnSandboxMemoryCapped()) {
        UT_ASSERT_MSG(strstr(sbLines, "glutton finished") == NULL,
                      "the hook held four times SCN_VM_MEMORY_MAX and ran to "
                      "its end. The console holds:\n%s", sbLines);
        UT_ASSERT_MSG(strstr(sbLines, "on_start raised") != NULL,
                      "the hook stopped without the host counting an error "
                      "against it, so the refusal was not a Lua error. The "
                      "console holds:\n%s", sbLines);
        /* Which error it was, not merely that there was one. Both VMs word
           an allocation failure this way, so a hook that stopped for some
           other reason does not pass for the cap doing its work. */
        UT_ASSERT_MSG(strstr(sbLines, "not enough memory") != NULL,
                      "the hook raised for some reason other than the memory "
                      "it was asking for. The console holds:\n%s", sbLines);
    } else {
        UT_ASSERT_MSG(strstr(sbLines, "glutton finished") != NULL,
                      "this build's Lua takes no allocator, so nothing is "
                      "counted and the hook should have run to its end. The "
                      "console holds:\n%s", sbLines);
    }
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running "
                  "after the refusal", (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 6. And the state lives through it ────────────────────────────── */

/* The refusal is a Lua error and not a wrecked state, so the same round's VM
 * goes on answering. The same allocating on_start, and an on_tick that says
 * so from the tick after the one the refusal happened on — the tick after,
 * because on_start runs ahead of on_tick within a tick and a line from the
 * same one would not have come after anything.
 *
 * The collection is what makes the recovery the case's own rather than the
 * collector's timing: the table on_start abandoned is unreachable the moment
 * it raised, and collecting hands its memory back before the line is
 * printed.
 *
 * The 8192 strings of 16 KiB and the index on the end of each are the case
 * above's, and for its reasons: 128 MiB against a cap of 32 so the refusal
 * is certain, and content that differs per iteration because strings that
 * are all the same bytes are interned into one allocation however long they
 * are. */
int run_scenario_sandbox_state_survives_a_refusal(void) {
    static const char *const kMap = "scnsand_survive.map";
    static const char *const kLua =
        "scenario = { name = \"Survivor\", api = 1 }\n"
        "local ticks = 0\n"
        "function on_start()\n"
        "  local t = {}\n"
        "  for i = 1, 8192 do\n"
        "    t[i] = string.rep(\"x\", 16384) .. i\n"
        "  end\n"
        "  print(\"survivor filled\")\n"
        "end\n"
        "function on_tick()\n"
        "  ticks = ticks + 1\n"
        "  if ticks == 2 then\n"
        "    collectgarbage(\"collect\")\n"
        "    print(\"survivor still here\")\n"
        "  end\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < 6; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);

    if (scnSandboxMemoryCapped()) {
        UT_ASSERT_MSG(strstr(sbLines, "survivor filled") == NULL,
                      "the hook ran to its end, so there was no refusal for "
                      "the state to live through. The console holds:\n%s",
                      sbLines);
    } else {
        UT_ASSERT_MSG(strstr(sbLines, "survivor filled") != NULL,
                      "this build's Lua takes no allocator, so nothing is "
                      "counted and the hook should have run to its end. The "
                      "console holds:\n%s", sbLines);
    }
    UT_ASSERT_MSG(strstr(sbLines, "survivor still here") != NULL,
                  "no later hook answered, so the state did not survive the "
                  "refusal. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 7. The instructions one call may spend ───────────────────────── */

/* A loop of twenty million turns, which is somewhere between forty and sixty
 * million VM instructions — a numeric for with a one-line body costs a
 * handful of them either way, and the two VMs do not emit the same handful.
 * Against a budget of a million that is forty times over, so the cut-off
 * lands around the three hundred thousandth turn whichever VM this is.
 *
 * Bounded rather than endless on purpose: a budget that has stopped working
 * has to end this case with a failure, not hold it until CTest kills it a
 * minute later. Twenty million turns interpreted is under a second, so the
 * broken case is slow rather than fatal.
 *
 * The hook prints on the way in and on the way out. Being cut off shows as
 * the first line without the second, beside the host's own line naming both
 * the hook it was in and what stopped it. */
int run_scenario_sandbox_instruction_budget_cuts_a_loop(void) {
    static const char *const kMap = "scnsand_budget.map";
    static const char *const kLua =
        "scenario = { name = \"Spinner\", api = 1 }\n"
        "function on_start()\n"
        "  print(\"spinner begun\")\n"
        "  local x = 0\n"
        "  for i = 1, 20000000 do x = x + 1 end\n"
        "  print(\"spinner finished\")\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    serverSimTick(sim);              /* the first running tick: on_start */
    for (i = 0; i < 5; i++) {
        serverSimTick(sim);          /* and the round after it */
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(strstr(sbLines, "spinner begun") != NULL,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "spinner finished") == NULL,
                  "the hook spun forty times the budget and ran to its end. "
                  "The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "on_start raised") != NULL,
                  "the hook stopped without the host counting an error "
                  "against it, so what stopped it was not a Lua error. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "instruction budget") != NULL,
                  "the hook raised for some reason other than the budget. "
                  "The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running "
                  "after the call was cut off", (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 8. And each call gets the whole of it ────────────────────────── */

/* Fifty thousand turns is between a hundred and two hundred thousand
 * instructions, a tenth to a fifth of the budget — a long call by any
 * scenario's standard and a legal one by a wide margin, on either VM.
 *
 * Sixteen of them, because what this case is about is the count going back
 * to zero at every call rather than climbing across a round. Sixteen legal
 * calls add up to between one and a half and three million, so a count that
 * accumulated would be cut off somewhere around the sixth and the sixteenth
 * line would never be printed. The script numbers its own lines and only
 * numbers one after the loop it belongs to has finished, so the last number
 * arriving is every call before it having finished too. */
int run_scenario_sandbox_budget_is_per_call(void) {
    static const char *const kMap = "scnsand_percall.map";
    static const char *const kLua =
        "scenario = { name = \"Steady\", api = 1 }\n"
        "local done = 0\n"
        "function on_tick()\n"
        "  local x = 0\n"
        "  for i = 1, 50000 do x = x + 1 end\n"
        "  done = done + 1\n"
        "  print(\"steady call \" .. done .. \" finished\")\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < 18; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(strstr(sbLines, "steady call 2 finished") != NULL,
                  "two successive calls did not both finish, so a call well "
                  "inside the budget is being cut off. The console holds:"
                  "\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "steady call 16 finished") != NULL,
                  "sixteen legal calls did not all finish, so the count is "
                  "climbing across the round rather than starting again at "
                  "each call. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "raised") == NULL,
                  "a call inside the budget was counted as an error. The "
                  "console holds:\n%s", sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 9. And the state lives through being cut off ─────────────────── */

/* The same spinning on_start, and an on_tick that answers from the tick
 * after the one it was cut off on — the tick after, because on_start runs
 * ahead of on_tick within a tick and a line from the same one would not have
 * come after anything.
 *
 * Nothing is collected here, unlike the memory cap's survival case: a call
 * that ran out of instructions was holding nothing, so what this shows is
 * only that an error raised from the hook leaves a state that still runs. */
int run_scenario_sandbox_state_survives_the_budget(void) {
    static const char *const kMap = "scnsand_budget_live.map";
    static const char *const kLua =
        "scenario = { name = \"Spinner\", api = 1 }\n"
        "local ticks = 0\n"
        "function on_start()\n"
        "  local x = 0\n"
        "  for i = 1, 20000000 do x = x + 1 end\n"
        "  print(\"spinner finished\")\n"
        "end\n"
        "function on_tick()\n"
        "  ticks = ticks + 1\n"
        "  if ticks == 2 then print(\"spinner still here\") end\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < 6; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(strstr(sbLines, "spinner finished") == NULL,
                  "the hook ran to its end, so there was nothing for the "
                  "state to live through. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "spinner still here") != NULL,
                  "no later hook answered, so the state did not survive being "
                  "cut off. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 10. A call made from inside another ──────────────────────────── */

/* Calls into script code nest, and the count has to survive it. Handing a
 * base over from a hook is the shortest way there: the op sets the owner
 * with migrate false, which raises the base-owner-changed callback, which
 * asks the announce policy, which is script code arriving on the same thread
 * while the hook that issued the op is still on the stack. The VM lock lets
 * it through for exactly that reason.
 *
 * So the hook hands a base to nobody, and then spins. What it is watching
 * for is the inner call putting the outer one's count back rather than
 * leaving the state unarmed: an arm and disarm that only set and cleared
 * would leave the rest of this hook uncounted and the loop would run to its
 * end.
 *
 * The announce line is asserted as well as the two the hook writes. Without
 * it the case would still pass if the op were refused and no policy ran at
 * all — it would be the plain runaway case over again, proving nothing about
 * nesting. */
int run_scenario_sandbox_budget_survives_a_nested_call(void) {
    static const char *const kMap = "scnsand_nested.map";
    static const char *const kLua =
        "scenario = { name = \"Nested\", api = 1 }\n"
        "local spun = false\n"
        "function announce(kind, subject, actor)\n"
        "  print(\"nested announce \" .. kind)\n"
        "  return true\n"
        "end\n"
        "function on_tick()\n"
        "  if spun then return end\n"
        "  spun = true\n"
        "  print(\"nested begun\")\n"
        "  game.set_base_owner(1, game.NEUTRAL)\n"
        "  local x = 0\n"
        "  for i = 1, 20000000 do x = x + 1 end\n"
        "  print(\"nested finished\")\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < 6; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(strstr(sbLines, "nested begun") != NULL,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "nested announce") != NULL,
                  "the op raised no policy, so no call was made from inside "
                  "another and this case tested nothing it claims to. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "nested finished") == NULL,
                  "the hook ran to its end after the policy it triggered "
                  "returned, so the inner call left the outer one uncounted. "
                  "The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "on_tick raised") != NULL,
                  "the hook stopped without the host counting an error "
                  "against it. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "instruction budget") != NULL,
                  "the hook raised for some reason other than the budget. "
                  "The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 11. A hook that catches what stopped it ──────────────────────── */

/* pcall is in the library a script is given, so the error the budget raises
 * lands wherever a script chooses to put one. What this case is about is that
 * putting one there buys nothing: the call was stopped, the latch says so,
 * and pcall raises the error again rather than answering with it.
 *
 * The hook spins inside a pcall, says what it caught, and then spins again.
 * Neither line after the pcall may be printed — the first would say the error
 * had been handed back as a value, and the second that the rest of the hook
 * ran on a budget that was already spent.
 *
 * Both loops are bounded for the reason the earlier budget cases give: a
 * latch that has stopped working has to end this case with a failure rather
 * than hold it until CTest kills it. Twenty million turns is forty times the
 * budget, so either loop alone is enough to be stopped by it. */
int run_scenario_sandbox_budget_survives_a_pcall(void) {
    static const char *const kMap = "scnsand_pcall.map";
    static const char *const kLua =
        "scenario = { name = \"Catcher\", api = 1 }\n"
        "function on_start()\n"
        "  print(\"catcher begun\")\n"
        "  local ok = pcall(function()\n"
        "    local x = 0\n"
        "    for i = 1, 20000000 do x = x + 1 end\n"
        "  end)\n"
        "  print(\"catcher caught \" .. tostring(ok))\n"
        "  local y = 0\n"
        "  for i = 1, 20000000 do y = y + 1 end\n"
        "  print(\"catcher finished\")\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    serverSimTick(sim);              /* the first running tick: on_start */
    for (i = 0; i < 5; i++) {
        serverSimTick(sim);          /* and the round after it */
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(strstr(sbLines, "catcher begun") != NULL,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "catcher caught") == NULL,
                  "pcall answered with the error the budget raised rather "
                  "than raising it again, so a script can catch being "
                  "stopped. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "catcher finished") == NULL,
                  "the hook went on spinning after it caught being stopped. "
                  "The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "on_start raised") != NULL,
                  "the hook returned without the host counting an error "
                  "against it, so a call that ran past its budget cost the "
                  "script nothing. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "instruction budget") != NULL,
                  "the hook raised for some reason other than the budget. "
                  "The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running "
                  "after the call was cut off", (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 12. And the same through a coroutine ─────────────────────────── */

/* coroutine.resume is the other way script code is handed a failure rather
 * than having it raised through it, and what is stopped there is a thread of
 * the script's own making: the hook's error arrives at the resume as an
 * ordinary false, from a call the hook itself never made.
 *
 * So the hook resumes a coroutine that spins, says what the resume answered,
 * and spins again. As in the case above, neither line after it may be
 * printed.
 *
 * coroutine.wrap has no case of its own. The function it answers with raises
 * into its caller rather than answering with a failure, so it reaches
 * whatever caught it — the pcall of the case above, or the host's own
 * lua_pcall where nothing did. */
int run_scenario_sandbox_budget_survives_a_coroutine(void) {
    static const char *const kMap = "scnsand_coro.map";
    static const char *const kLua =
        "scenario = { name = \"Threader\", api = 1 }\n"
        "function on_start()\n"
        "  print(\"threader begun\")\n"
        "  local co = coroutine.create(function()\n"
        "    local x = 0\n"
        "    for i = 1, 20000000 do x = x + 1 end\n"
        "  end)\n"
        "  local ok = coroutine.resume(co)\n"
        "  print(\"threader resumed \" .. tostring(ok))\n"
        "  local y = 0\n"
        "  for i = 1, 20000000 do y = y + 1 end\n"
        "  print(\"threader finished\")\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    serverSimTick(sim);              /* the first running tick: on_start */
    for (i = 0; i < 5; i++) {
        serverSimTick(sim);          /* and the round after it */
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(strstr(sbLines, "threader begun") != NULL,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "threader resumed") == NULL,
                  "coroutine.resume answered with the error the budget raised "
                  "rather than raising it again, or the coroutine spun to its "
                  "end uncounted. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "threader finished") == NULL,
                  "the hook went on spinning after the coroutine it resumed "
                  "was stopped. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "on_start raised") != NULL,
                  "the hook returned without the host counting an error "
                  "against it, so a call that ran past its budget cost the "
                  "script nothing. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "instruction budget") != NULL,
                  "the hook raised for some reason other than the budget. "
                  "The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running "
                  "after the call was cut off", (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 13. The format os.date is given ──────────────────────────────── */

/* A format reaches the host's own strftime, and the C libraries this server is
 * built against do not agree on what an invalid conversion character is worth:
 * glibc copies one through, the MSVC CRT calls the invalid-parameter handler,
 * which by default ends the process. So the sandbox reads the format first.
 *
 * Both directions are in the one case on purpose. A check that refused
 * everything would be no better than what it replaced, so the formats a script
 * really writes are asked for as well — the two that ask for a table among
 * them, which never reach strftime at all, and a second argument, which says
 * that what follows the format still arrives.
 *
 * Every call is made through the script's own pcall, so an attach that
 * succeeds is also the assertion that this refusal is an ordinary error: the
 * budget's is raised again by the pcall wrapper, and this one is not. */
int run_scenario_sandbox_os_date_refuses_a_bad_format(void) {
    static const char *const kMap = "scnsand_date.map";
    static const char *const kLua =
        "local leaks = {}\n"
        "local function good(what, kind, ...)\n"
        "  local ok, v = pcall(os.date, ...)\n"
        "  if not ok then\n"
        "    leaks[#leaks + 1] = what .. \" was refused: \" .. tostring(v)\n"
        "  elseif type(v) ~= kind then\n"
        "    leaks[#leaks + 1] = what .. \" answered a \" .. type(v)\n"
        "  end\n"
        "end\n"
        "local function bad(what, fmt, names)\n"
        "  local ok, e = pcall(os.date, fmt)\n"
        "  if ok then\n"
        "    leaks[#leaks + 1] = what .. \" was allowed\"\n"
        "  elseif not string.find(tostring(e), names, 1, true) then\n"
        "    leaks[#leaks + 1] = what .. \" was refused without naming \" ..\n"
        "                        names .. \": \" .. tostring(e)\n"
        "  end\n"
        "end\n"
        "good(\"os.date()\", \"string\")\n"
        "good(\"os.date(nil)\", \"string\", nil)\n"
        "good(\"%Y-%m-%d\", \"string\", \"%Y-%m-%d\")\n"
        "good(\"!%H:%M:%S\", \"string\", \"!%H:%M:%S\")\n"
        "good(\"*t\", \"table\", \"*t\")\n"
        "good(\"!*t\", \"table\", \"!*t\")\n"
        "good(\"a literal percent\", \"string\", \"100%% sure\")\n"
        "local y = os.date(\"!%Y\", 0)\n"
        "if y ~= \"1970\" then\n"
        "  leaks[#leaks + 1] = \"!%Y at time 0 answered \" .. tostring(y)\n"
        "end\n"
        "bad(\"%Q\", \"%Q\", \"%Q\")\n"
        "bad(\"%Ec\", \"%Ec\", \"%E\")\n"
        "bad(\"%#c\", \"%#c\", \"%#\")\n"
        "bad(\"a trailing percent\", \"the time is 20%\",\n"
        "    \"no conversion character\")\n"
        "bad(\"a 300-byte format\", string.rep(\"%Y\", 150), \"300\")\n"
        "if #leaks > 0 then\n"
        "  error(\"os.date: \" .. table.concat(leaks, \"; \"))\n"
        "end\n"
        "scenario = { name = \"Dater\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script raised: %s", err);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 14. The lines one call may print ─────────────────────────────── */

/* print goes to the server console, and a console line reaches the operator's
 * message log where one is configured — a file opened, written and closed for
 * each line. Two hundred thousand prints fit inside one call's instruction
 * budget, so the lines are counted as well.
 *
 * The hook asks for four times the bound, numbering each line, so what came
 * out is read off the numbers rather than off a total: the bound's own line is
 * there, the one after it is not, and the notice saying where the rest went is
 * there exactly once however many were dropped.
 *
 * The second tick's line is what says the round went on and the next call got
 * its allowance back. The tick after, rather than the same one — on_start and
 * on_tick run in the same tick, and the flood has spent that tick's allowance
 * as well as its own call's. */
int run_scenario_sandbox_print_bounded_in_one_call(void) {
    static const char *const kMap = "scnsand_printcap.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          lua[512];
    char          want[64];
    char          err[512];
    int           i;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Flood\", api = 1 }\n"
             "local ticks = 0\n"
             "function on_start()\n"
             "  for i = 1, %d do print(\"flood \" .. i) end\n"
             "end\n"
             "function on_tick()\n"
             "  ticks = ticks + 1\n"
             "  if ticks == 2 then print(\"flood still here\") end\n"
             "end\n", SCN_PRINT_PER_CALL * 4);
    UT_ASSERT(sbPutText(kMap, lua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < 6; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(sbCount("flood 1\n") == 1,
                  "the hook never printed, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    snprintf(want, sizeof(want), "flood %d\n", SCN_PRINT_PER_CALL);
    UT_ASSERT_MSG(sbCount(want) == 1,
                  "the call printed fewer than the %d lines it is allowed. "
                  "The console holds:\n%s", SCN_PRINT_PER_CALL, sbLines);
    snprintf(want, sizeof(want), "flood %d\n", SCN_PRINT_PER_CALL + 1);
    UT_ASSERT_MSG(sbCount(want) == 0,
                  "the call printed past the %d lines it is allowed, so the "
                  "bound is not being applied. The console holds:\n%s",
                  SCN_PRINT_PER_CALL, sbLines);
    UT_ASSERT_MSG(sbCount("one call may print") == 1,
                  "the lines dropped were said to be dropped %d times, "
                  "expected once: an operator is told where the output went "
                  "and then left alone. The console holds:\n%s",
                  sbCount("one call may print"), sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "flood still here") != NULL,
                  "no later hook printed, so the round did not go on — or the "
                  "allowance never came back. The console holds:\n%s",
                  sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running "
                  "after the lines were dropped",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 15. And the allowance comes back ─────────────────────────────── */

/* The bounds are per call and per tick and not for the life of the round. A
 * hook printing an eighth of the per-call bound is modest by either count, and
 * doing it every tick for long enough to put out four times the bound in total
 * is what a counter that climbed instead of resetting would fail: the lines
 * past the first bound's worth would be dropped, the notice would be said, and
 * the last number the script reached would never arrive.
 *
 * The script numbers its lines across the whole round rather than within a
 * tick, so the last number is every call before it having printed in full. */
int run_scenario_sandbox_print_allowance_returns(void) {
    static const char *const kMap = "scnsand_printback.map";
    const int     perTick = SCN_PRINT_PER_CALL / 8;
    const int     wanted  = SCN_PRINT_PER_CALL * 4;
    ServerSim    *sim;
    ScenarioHost *h;
    char          lua[512];
    char          want[64];
    char          err[512];
    int           i;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Steady\", api = 1 }\n"
             "local said = 0\n"
             "function on_tick()\n"
             "  for i = 1, %d do\n"
             "    said = said + 1\n"
             "    print(\"said \" .. said)\n"
             "  end\n"
             "end\n", perTick);
    UT_ASSERT(sbPutText(kMap, lua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    /* Enough ticks for the wanted number with room to spare, so the case does
       not turn on exactly which tick on_tick first ran in. */
    for (i = 0; i < (wanted / perTick) + 8; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(sbCount("said 1\n") == 1,
                  "the hook never printed, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    snprintf(want, sizeof(want), "said %d\n", wanted);
    UT_ASSERT_MSG(sbCount(want) == 1,
                  "%d lines printed %d at a time did not all arrive, so an "
                  "allowance is climbing across the round rather than coming "
                  "back at each call and each tick. The console holds:\n%s",
                  wanted, perTick, sbLines);
    UT_ASSERT_MSG(sbCount("may print") == 0,
                  "a hook well inside both bounds had lines dropped. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "raised") == NULL,
                  "a call inside both bounds was counted as an error. The "
                  "console holds:\n%s", sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── The string cap, for the two cases below ──────────────────────── */

/* Whether the console line that begins with prefix also carries needle
   before it ends. A refusal is one line, so this is what says the error
   named the function rather than some other line having done so. */
static bool sbLineHas(const char *prefix, const char *needle) {
    const char *p = strstr(sbLines, prefix);
    const char *end;
    const char *hit;

    if (p == NULL) {
        return false;
    }
    end = strchr(p, '\n');
    hit = strstr(p, needle);
    return hit != NULL && (end == NULL || hit < end);
}

/* What one refusal is expected to say: the line's label, the function its
   error names, and whether the length it gives is the cap plus one. */
typedef struct {
    const char *label;
    const char *fn;
    bool        byOne;
} SbRefusal;

/* The attach, the round and the ticks both cases share. The script's lines
   are left in sbLines; the round is checked still running before this
   hands the sim back. */
static int sbRunCap(const char *map, const char *lua, ServerSim **simOut,
                    ScenarioHost **hOut) {
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(map, lua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, map, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    serverSimTick(sim);              /* the first running tick: on_start */
    for (i = 0; i < 5; i++) {
        serverSimTick(sim);          /* and the round after it */
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running "
                  "after the refusals", (int)serverSimGetState(sim));
    *simOut = sim;
    *hOut   = h;
    return 0;
}

/* Every refusal is there once, names its function, and — where the case
   built it to be one byte over — gives that length. The two counts are what
   say nothing else was refused and nothing at the cap was: every line the
   script prints begins with one of the two. */
static int sbCheckCap(const char *const *oks, size_t nOk, const char *okTail,
                      const SbRefusal *nos, size_t nNo) {
    char   want[96];
    char   over[32];
    size_t i;

    UT_ASSERT_MSG(sbCount("cap ok ") == (int)nOk,
                  "%d strings at the cap were built, expected %d. The "
                  "console holds:\n%s", sbCount("cap ok "), (int)nOk,
                  sbLines);
    UT_ASSERT_MSG(sbCount("cap no ") == (int)nNo,
                  "%d strings were refused, expected %d. The console "
                  "holds:\n%s", sbCount("cap no "), (int)nNo, sbLines);
    for (i = 0; i < nOk; i++) {
        snprintf(want, sizeof(want), "cap ok %s%s\n", oks[i], okTail);
        UT_ASSERT_MSG(sbCount(want) == 1,
                      "no line \"%s\": a string of exactly %u bytes was "
                      "refused or came out another length. The console "
                      "holds:\n%s", oks[i], (unsigned)SCN_STRING_MAX,
                      sbLines);
    }
    snprintf(over, sizeof(over), " %u bytes", (unsigned)SCN_STRING_MAX + 1u);
    for (i = 0; i < nNo; i++) {
        snprintf(want, sizeof(want), "cap no %s: ", nos[i].label);
        UT_ASSERT_MSG(sbCount(want) == 1,
                      "\"%s\" was not refused. The console holds:\n%s",
                      nos[i].label, sbLines);
        UT_ASSERT_MSG(sbLineHas(want, nos[i].fn),
                      "the refusal of \"%s\" does not name %s. The console "
                      "holds:\n%s", nos[i].label, nos[i].fn, sbLines);
        if (nos[i].byOne) {
            UT_ASSERT_MSG(sbLineHas(want, over),
                          "the refusal of \"%s\" does not give the length "
                          "%u. The console holds:\n%s", nos[i].label,
                          (unsigned)SCN_STRING_MAX + 1u, sbLines);
        }
    }
    UT_ASSERT_MSG(strstr(sbLines, "raised") == NULL,
                  "a refusal the script caught was counted as an error "
                  "against it. The console holds:\n%s", sbLines);
    return 0;
}

/* ── 16. The strings a C function may build ───────────────────────── */

/* rep, format and concat, each asked for a string of exactly the cap and for
 * one a byte longer, every call inside a pcall so a refusal is a line rather
 * than the end of the hook. rep is asked through a method call as well, which
 * reaches it through the string metatable's __index rather than through the
 * global, and with a separator, which the length has to count: the separated
 * case with many copies would be far under the cap without it.
 *
 * format is refused both ways it can be: a result over the cap built from
 * arguments under it, and an argument already over it. concat is asked once
 * more with a number among its elements, which counts by the length of its
 * text. The one string over the cap the script starts from is built with ..,
 * which the cap does not cover.
 *
 * rep is refused a count far past the cap with an empty string, which comes
 * to no bytes but is a copy loop in C all the same. concat is asked for two
 * elements that only __index supplies: 5.4 reads through it and is refused
 * by the cap, LuaJIT reads raw and refuses the empty slot itself, and either
 * way it is one refusal line. */
int run_scenario_sandbox_string_cap_on_results(void) {
    static const char *const kMap = "scnsand_strres.map";
    static const char *const kOk[] = {
        "rep at", "method at", "sep at", "format at", "concat at"
    };
    static const SbRefusal kNo[] = {
        { "rep over",           "string.rep",   true  },
        { "method over",        "string.rep",   true  },
        { "sep over",           "string.rep",   true  },
        { "sep many over",      "string.rep",   false },
        { "format over",        "string.format", true },
        { "format arg",         "string.format", true },
        { "concat over",        "table.concat", true  },
        { "concat number over", "table.concat", true  },
        { "rep empty many",     "string.rep",   false },
#ifdef WINBOLO_LUAJIT
        /* LuaJIT's concat reads raw, so it never sees what __index would
           answer and refuses the empty slot in its own words. */
        { "concat index over",  "invalid value", false },
#else
        { "concat index over",  "table.concat", false },
#endif
    };
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          lua[4096];

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Long results\", api = 1 }\n"
             "local MAX = %u\n"
             "local function try(label, f)\n"
             "  local ok, r = pcall(f)\n"
             "  if ok then print(\"cap ok \" .. label .. \" \" .. #r)\n"
             "  else print(\"cap no \" .. label .. \": \" .. tostring(r)) end\n"
             "end\n"
             "function on_start()\n"
             "  local q   = math.floor(MAX / 4)\n"
             "  local big = string.rep(\"x\", MAX) .. \"y\"\n"
             "  try(\"rep at\", function() return string.rep(\"x\", MAX) end)\n"
             "  try(\"rep over\", function()\n"
             "    return string.rep(\"x\", MAX + 1) end)\n"
             "  try(\"method at\", function() return (\"x\"):rep(MAX) end)\n"
             "  try(\"method over\", function()\n"
             "    return (\"x\"):rep(MAX + 1) end)\n"
             "  try(\"sep at\", function() return string.rep(\n"
             "    string.rep(\"x\", q), 2, string.rep(\"-\", MAX - 2 * q)) end)\n"
             "  try(\"sep over\", function() return string.rep(\n"
             "    string.rep(\"x\", q), 2, string.rep(\"-\", MAX - 2 * q + 1))\n"
             "    end)\n"
             "  try(\"sep many over\", function()\n"
             "    return string.rep(\"x\", math.floor(MAX / 2) + 1, \",\") end)\n"
             "  try(\"format at\", function()\n"
             "    return string.format(\"%%s\", string.rep(\"x\", MAX)) end)\n"
             "  try(\"format over\", function()\n"
             "    return string.format(\"%%s!\", string.rep(\"x\", MAX)) end)\n"
             "  try(\"format arg\", function()\n"
             "    return string.format(\"%%d\", 1, big) end)\n"
             "  try(\"concat at\", function() return table.concat(\n"
             "    { string.rep(\"x\", q), string.rep(\"y\", MAX - q - 1) },\n"
             "    \",\") end)\n"
             "  try(\"concat over\", function() return table.concat(\n"
             "    { string.rep(\"x\", q), string.rep(\"y\", MAX - q) }, \",\")\n"
             "    end)\n"
             "  try(\"concat number over\", function() return table.concat(\n"
             "    { string.rep(\"x\", MAX - 3), 1234 }) end)\n"
             "  try(\"rep empty many\", function()\n"
             "    return string.rep(\"\", 1e18) end)\n"
             "  try(\"concat index over\", function()\n"
             "    local t = setmetatable({}, { __index = function()\n"
             "      return string.rep(\"x\", MAX) end })\n"
             "    return table.concat(t, \"\", 1, 2) end)\n"
             "end\n", (unsigned)SCN_STRING_MAX);

    UT_ASSERT(sbRunCap(kMap, lua, &sim, &h) == 0);
    UT_ASSERT_MSG(sbCount("cap ok rep at") == 1,
                  "the hook never printed, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    {
        char okTail[32];

        snprintf(okTail, sizeof(okTail), " %u", (unsigned)SCN_STRING_MAX);
        UT_ASSERT(sbCheckCap(kOk, sizeof(kOk) / sizeof(kOk[0]), okTail,
                             kNo, sizeof(kNo) / sizeof(kNo[0])) == 0);
    }

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 17. The strings a pattern may run over ───────────────────────── */

/* find, match, gmatch and gsub, each given a subject of exactly the cap and
 * one a byte longer. The pattern is a byte the subject does not hold, so an
 * accepted call reads every byte of it and answers nothing. gsub is asked
 * through a method call too. The subject over the cap is built with .., which
 * the cap does not cover, so the script has one to pass. */
int run_scenario_sandbox_string_cap_on_subjects(void) {
    static const char *const kMap = "scnsand_strsub.map";
    static const char *const kOk[] = {
        "find at", "match at", "gmatch at", "gsub at", "method at"
    };
    static const SbRefusal kNo[] = {
        { "find over",   "string.find",   true },
        { "match over",  "string.match",  true },
        { "gmatch over", "string.gmatch", true },
        { "gsub over",   "string.gsub",   true },
        { "method over", "string.gsub",   true },
    };
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          lua[4096];

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Long subjects\", api = 1 }\n"
             "local MAX = %u\n"
             "local function try(label, f)\n"
             "  local ok, r = pcall(f)\n"
             "  if ok then print(\"cap ok \" .. label)\n"
             "  else print(\"cap no \" .. label .. \": \" .. tostring(r)) end\n"
             "end\n"
             "function on_start()\n"
             "  local at   = string.rep(\"x\", MAX)\n"
             "  local over = at .. \"x\"\n"
             "  local function each(s) for w in string.gmatch(s, \"y\") do\n"
             "    end end\n"
             "  try(\"find at\", function() return string.find(at, \"y\") end)\n"
             "  try(\"find over\", function()\n"
             "    return string.find(over, \"y\") end)\n"
             "  try(\"match at\", function()\n"
             "    return string.match(at, \"y\") end)\n"
             "  try(\"match over\", function()\n"
             "    return string.match(over, \"y\") end)\n"
             "  try(\"gmatch at\", function() each(at) end)\n"
             "  try(\"gmatch over\", function() each(over) end)\n"
             "  try(\"gsub at\", function()\n"
             "    return string.gsub(at, \"y\", \"z\") end)\n"
             "  try(\"gsub over\", function()\n"
             "    return string.gsub(over, \"y\", \"z\") end)\n"
             "  try(\"method at\", function() return at:gsub(\"y\", \"z\") end)\n"
             "  try(\"method over\", function()\n"
             "    return over:gsub(\"y\", \"z\") end)\n"
             "end\n", (unsigned)SCN_STRING_MAX);

    UT_ASSERT(sbRunCap(kMap, lua, &sim, &h) == 0);
    UT_ASSERT_MSG(sbCount("cap ok find at") == 1,
                  "the hook never printed, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT(sbCheckCap(kOk, sizeof(kOk) / sizeof(kOk[0]), "",
                         kNo, sizeof(kNo) / sizeof(kNo[0])) == 0);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── The tick's total, for the cases below ────────────────────────── */

/* The loop the per-call cases are calibrated with: fifty thousand turns cost
 * between a hundred and two hundred thousand instructions depending on the
 * VM, so a turn is two to four.
 *
 * Two hundred thousand turns is then 400k to 800k instructions: under the
 * per-call budget of a million on either VM, so no timer below is stopped for
 * its own sake. Eight of them are 3.2M to 6.4M together, over the tick's two
 * million on either VM. Two of them are at most 1.6M, under it, so the first
 * two always finish; five of them are at least 2.0M, so the trip lands in the
 * sixth at the latest and the seventh and eighth are always left over. */
#define SB_TICK_TURNS  200000
#define SB_TICK_TIMERS 8

#define SB_SPIN_LUA                                                         \
    "local function spin(n)\n"                                              \
    "  local x = 0\n"                                                       \
    "  for i = 1, n do x = x + 1 end\n"                                     \
    "  return x\n"                                                          \
    "end\n"

/* Attach, start the round and run it for ticks ticks, watching the console
   throughout. The sim and host are handed back for the case to read and put
   away. */
static int sbRunTicks(const char *map, const char *lua, int ticks,
                      ServerSim **simOut, ScenarioHost **hOut) {
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    int           i;

    UT_ASSERT(sbPutText(map, lua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, map, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < ticks; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);
    *simOut = sim;
    *hOut   = h;
    return 0;
}

/* A drain of count timers, each printing "<tag> begun k" and "<tag> finished
 * k", read back off the console against the one line the trip wrote.
 *
 * Which timer the trip lands in depends on the VM, so this finds it rather
 * than naming it: the one timer that began and never finished. Every timer
 * before it finished ahead of the trip's line, so it was the tick's; every
 * timer after it began behind that line, so it waited for a later tick rather
 * than running in this one or being lost, and it finished there. */
static int sbCheckTrip(const char *tag, int count, int *tripped) {
    const char *raised = strstr(sbLines, "a timer raised");
    char        want[64];
    const char *b;
    const char *f;
    int         n = 0;
    int         k;

    UT_ASSERT_MSG(raised != NULL,
                  "no timer was stopped, so the tick's calls never passed "
                  "the total. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(sbCount(" raised: ") == 1,
                  "%d calls were counted as errors, expected the one that "
                  "went over. The console holds:\n%s", sbCount(" raised: "),
                  sbLines);
    UT_ASSERT_MSG(sbLineHas("a timer raised", "one tick may spend"),
                  "the timer was stopped for something other than the tick's "
                  "total. The console holds:\n%s", sbLines);
    for (k = 1; k <= count; k++) {
        snprintf(want, sizeof(want), "%s begun %d\n", tag, k);
        b = strstr(sbLines, want);
        snprintf(want, sizeof(want), "%s finished %d\n", tag, k);
        f = strstr(sbLines, want);
        UT_ASSERT_MSG(b != NULL,
                      "timer %d never began, so it was lost rather than left "
                      "for a later tick. The console holds:\n%s", k, sbLines);
        if (f == NULL) {
            UT_ASSERT_MSG(n == 0,
                          "timers %d and %d both began without finishing; "
                          "only the one that went over should. The console "
                          "holds:\n%s", n, k, sbLines);
            n = k;
        }
    }
    UT_ASSERT_MSG(n >= 2 && n < count,
                  "timer %d was the one stopped, expected one after the "
                  "first and before the last. The console holds:\n%s", n,
                  sbLines);
    for (k = 1; k <= count; k++) {
        snprintf(want, sizeof(want), "%s begun %d\n", tag, k);
        b = strstr(sbLines, want);
        snprintf(want, sizeof(want), "%s finished %d\n", tag, k);
        f = strstr(sbLines, want);
        if (k < n) {
            UT_ASSERT_MSG(f < raised,
                          "timer %d, ahead of the one stopped, did not finish "
                          "in the tick. The console holds:\n%s", k, sbLines);
        } else if (k == n) {
            UT_ASSERT_MSG(b < raised,
                          "the timer stopped began after its own error. The "
                          "console holds:\n%s", sbLines);
        } else {
            UT_ASSERT_MSG(b > raised,
                          "timer %d began in the tick that ran out, after the "
                          "one that went over. The console holds:\n%s", k,
                          sbLines);
        }
    }
    *tripped = n;
    return 0;
}

/* ── 18. The tick's calls share one total ─────────────────────────── */

/* Eight timers due on the same tick, each well inside its own budget and
 * together over the tick's, on either VM — see SB_TICK_TURNS. Each spins only
 * on the tick on_start ran in, so the ones left over run light on the next
 * tick and finish there: that is the timers not being lost, and the drain the
 * trip cut short going on where it stopped. */
int run_scenario_sandbox_tick_budget_cuts_a_drain(void) {
    static const char *const kMap = "scnsand_tickdrain.map";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          lua[1024];
    int           tripped = 0;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Drain\", api = 1 }\n"
             SB_SPIN_LUA
             "local heavy = -1\n"
             "function on_start()\n"
             "  heavy = game.tick()\n"
             "  for k = 1, %d do\n"
             "    game.timer(0, function()\n"
             "      print(\"drain begun \" .. k)\n"
             "      if game.tick() == heavy then spin(%d) end\n"
             "      print(\"drain finished \" .. k)\n"
             "    end)\n"
             "  end\n"
             "end\n", SB_TICK_TIMERS, SB_TICK_TURNS);

    UT_ASSERT(sbRunTicks(kMap, lua, 6, &sim, &h) == 0);
    UT_ASSERT(sbCheckTrip("drain", SB_TICK_TIMERS, &tripped) == 0);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 19. And the next tick has all of it again ────────────────────── */

/* The tick after one that ran out makes two calls of SB_TICK_TURNS each: a
 * timer set to come due one tick on, and on_tick. Together they are 0.8M to
 * 1.6M, inside the total on either VM, so both finish only if the count went
 * back to zero and the latch was cleared: a count carried over would stop the
 * first of them within its first steps, and a latch left set would skip both.
 *
 * The delay is a tick and a half in seconds, which the timer rounds down to
 * one, so it cannot come due on the tick that ran out. */
int run_scenario_sandbox_tick_budget_returns(void) {
    static const char *const kMap = "scnsand_tickback.map";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          lua[1536];
    const char   *raised;
    const char   *timerLine;
    const char   *tickLine;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Back\", api = 1 }\n"
             SB_SPIN_LUA
             "local heavy = -1\n"
             "local back  = false\n"
             "function on_start()\n"
             "  heavy = game.tick()\n"
             "  for k = 1, %d do\n"
             "    game.timer(0, function()\n"
             "      if game.tick() == heavy then spin(%d) end\n"
             "    end)\n"
             "  end\n"
             "  game.timer(1.5 / %d, function()\n"
             "    spin(%d)\n"
             "    print(\"back timer finished\")\n"
             "  end)\n"
             "end\n"
             "function on_tick()\n"
             "  if game.tick() == heavy or back then return end\n"
             "  back = true\n"
             "  spin(%d)\n"
             "  print(\"back tick finished\")\n"
             "end\n", SB_TICK_TIMERS, SB_TICK_TURNS, GAME_NUMTOTALTICKS_SEC,
             SB_TICK_TURNS, SB_TICK_TURNS);

    UT_ASSERT(sbRunTicks(kMap, lua, 6, &sim, &h) == 0);

    raised    = strstr(sbLines, "a timer raised");
    timerLine = strstr(sbLines, "back timer finished");
    tickLine  = strstr(sbLines, "back tick finished");
    UT_ASSERT_MSG(raised != NULL &&
                  sbLineHas("a timer raised", "one tick may spend"),
                  "the first tick never ran out, so there was nothing to come "
                  "back from. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(timerLine != NULL && timerLine > raised,
                  "the timer due the tick after did not finish, so that tick "
                  "did not have its whole total. The console holds:\n%s",
                  sbLines);
    UT_ASSERT_MSG(tickLine != NULL && tickLine > raised,
                  "on_tick the tick after did not finish, so that tick did "
                  "not have its whole total. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(sbCount(" raised: ") == 1,
                  "%d calls were counted as errors, expected only the one "
                  "that went over. The console holds:\n%s",
                  sbCount(" raised: "), sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 20. A catcher does not give the tick its total back ──────────── */

/* The drain of case 18, with each timer's spin inside a catcher. The timer
 * that goes over is stopped inside the catcher, and the catcher raises the
 * error again rather than answering false, so the timer is stopped outright
 * and the rest of the tick is skipped as before. A catcher that kept the
 * error would print "caught N false" and let the tick run on.
 *
 * Run twice, once through pcall and once through coroutine.resume, because
 * which timer the trip lands in depends on the VM and one run could not be
 * sure of stopping both. Each run has its own map. */
static int sbTickCatch(const char *map, const char *tag, const char *open,
                       const char *close) {
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          lua[1536];
    int           tripped = 0;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Catch\", api = 1 }\n"
             SB_SPIN_LUA
             "local heavy = -1\n"
             "function on_start()\n"
             "  heavy = game.tick()\n"
             "  for k = 1, %d do\n"
             "    game.timer(0, function()\n"
             "      print(\"%s begun \" .. k)\n"
             "      local ok = %sfunction()\n"
             "        if game.tick() == heavy then spin(%d) end\n"
             "      end%s\n"
             "      print(\"%s caught \" .. k .. \" \" .. tostring(ok))\n"
             "      print(\"%s finished \" .. k)\n"
             "    end)\n"
             "  end\n"
             "end\n", SB_TICK_TIMERS, tag, open, SB_TICK_TURNS, close, tag,
             tag);

    UT_ASSERT(sbRunTicks(map, lua, 6, &sim, &h) == 0);
    UT_ASSERT(sbCheckTrip(tag, SB_TICK_TIMERS, &tripped) == 0);
    UT_ASSERT_MSG(sbCount(" false\n") == 0,
                  "a catcher answered false, so it kept the error the tick's "
                  "total raised rather than raising it again. The console "
                  "holds:\n%s", sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(map);
    return 0;
}

int run_scenario_sandbox_tick_budget_survives_a_pcall(void) {
    UT_ASSERT(sbTickCatch("scnsand_tickpcall.map", "pcalled", "pcall(",
                          ")") == 0);
    UT_ASSERT(sbTickCatch("scnsand_tickcoro.map", "resumed",
                          "coroutine.resume(coroutine.create(", "))") == 0);
    return 0;
}

/* ── 21. on_end is not one of the calls a spent tick skips ────────── */

/* The first timer ends the round and the ones after it spend the tick's
 * total, so the round's end and the trip land in the same tick. on_end is
 * owed in that tick, and it runs after the window has closed: it says which
 * tick it ran in, spends SB_TICK_TURNS of its own, and finishes. Were it
 * counted against the spent total it would be skipped, and were it counted
 * against what was left it would be stopped part way. */
int run_scenario_sandbox_tick_budget_spares_on_end(void) {
    static const char *const kMap = "scnsand_tickend.map";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          lua[1536];
    const char   *raised;
    const char   *ended;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Ender\", api = 1 }\n"
             SB_SPIN_LUA
             "local heavy = -1\n"
             "function on_start()\n"
             "  heavy = game.tick()\n"
             "  game.timer(0, function()\n"
             "    game.end_round(\"Ended from a timer\")\n"
             "    print(\"ender asked\")\n"
             "  end)\n"
             "  for k = 1, %d do\n"
             "    game.timer(0, function()\n"
             "      if game.tick() == heavy then spin(%d) end\n"
             "    end)\n"
             "  end\n"
             "end\n"
             "function on_end()\n"
             "  if game.tick() == heavy then print(\"on_end same tick\")\n"
             "  else print(\"on_end later tick\") end\n"
             "  spin(%d)\n"
             "  print(\"on_end finished\")\n"
             "end\n", SB_TICK_TIMERS, SB_TICK_TURNS, SB_TICK_TURNS);

    UT_ASSERT(sbRunTicks(kMap, lua, 6, &sim, &h) == 0);

    raised = strstr(sbLines, "a timer raised");
    ended  = strstr(sbLines, "on_end same tick");
    UT_ASSERT_MSG(strstr(sbLines, "ender asked") != NULL,
                  "the timer that ends the round did not run to its end, so "
                  "the round was not ended from the drain. The console "
                  "holds:\n%s", sbLines);
    UT_ASSERT_MSG(raised != NULL &&
                  sbLineHas("a timer raised", "one tick may spend"),
                  "the tick that ended the round did not run out, so on_end "
                  "was never at risk. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(ended != NULL && ended > raised,
                  "on_end did not run in the tick that ran out. The console "
                  "holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "on_end later tick") == NULL,
                  "on_end ran in a later tick than the round ended in. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "on_end finished") != NULL,
                  "on_end was stopped, so it was counted against the tick's "
                  "spent total rather than its own budget. The console "
                  "holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "on_end raised") == NULL,
                  "on_end raised. The console holds:\n%s", sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 22. And a script that always runs out is switched off ────────── */

/* Eight timers that set themselves again before they spin, so every tick has
 * eight due and every tick runs out — the one stopped has already set its
 * next. The calls ahead of the trip return and would clear the count; a tick
 * that runs out undoes that, so the count climbs by one a tick and the
 * script is switched off on the SCN_ERROR_LIMIT-th tick and not before.
 *
 * The console is read after every tick, which is what says which tick the
 * line arrived on. Once the script is off no timer runs, so the lines that
 * say a timer was stopped number exactly the limit. */
int run_scenario_sandbox_tick_budget_switches_off(void) {
    static const char *const kMap = "scnsand_tickoff.map";
    static const char *const kOff = "is off for the rest of the round";
    ServerSim    *sim;
    ScenarioHost *h;
    char          lua[1024];
    char          err[512];
    int           offAt = 0;
    int           i;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Runaway\", api = 1 }\n"
             SB_SPIN_LUA
             "local function body()\n"
             "  game.timer(0, body)\n"
             "  spin(%d)\n"
             "end\n"
             "function on_start()\n"
             "  for k = 1, %d do game.timer(0, body) end\n"
             "end\n", SB_TICK_TURNS, SB_TICK_TIMERS);

    UT_ASSERT(sbPutText(kMap, lua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 1; i <= SCN_ERROR_LIMIT + 5; i++) {
        serverSimTick(sim);
        if (offAt == 0 && strstr(sbLines, kOff) != NULL) {
            offAt = i;
        }
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(offAt != 0,
                  "the script ran out every tick for %d ticks and was never "
                  "switched off. The console holds:\n%s",
                  SCN_ERROR_LIMIT + 5, sbLines);
    UT_ASSERT_MSG(offAt == SCN_ERROR_LIMIT,
                  "the script was switched off on tick %d, expected tick %d. "
                  "The console holds:\n%s", offAt, SCN_ERROR_LIMIT, sbLines);
    UT_ASSERT_MSG(sbCount("a timer raised") == SCN_ERROR_LIMIT,
                  "%d timers were stopped, expected one a tick for %d ticks "
                  "and none after. The console holds:\n%s",
                  sbCount("a timer raised"), SCN_ERROR_LIMIT, sbLines);
    UT_ASSERT_MSG(sbCount("a timer raised") ==
                  sbCount("one tick may spend"),
                  "a timer was stopped for something other than the tick's "
                  "total. The console holds:\n%s", sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 23. A pattern that backtracks without end ────────────────────── */

/* ".-.-.-b" over sixty thousand a's tries every way of splitting the subject
 * in three before giving up at each start, which is on the order of n⁴ steps:
 * no VM's own matcher would come back from it inside the life of the test. The
 * subject is under SCN_STRING_MAX, so the cap has nothing to say about it —
 * what stops it is the matcher's steps charged to the call's budget.
 *
 * on_tick runs the bomb once, on the first tick it is called, and the case
 * runs a handful of ticks, so a charge that has stopped working hangs here
 * rather than failing: CTest's timeout is what reports that. */
#define SB_BOMB_LUA                                                         \
    "string.find(string.rep(\"a\", 60000), \".-.-.-b\")"

int run_scenario_sandbox_pattern_bomb_stopped(void) {
    static const char *const kMap = "scnsand_patbomb.map";
    static const char *const kLua =
        "scenario = { name = \"Bomb\", api = 1 }\n"
        "local fired = false\n"
        "function on_tick()\n"
        "  if fired then return end\n"
        "  fired = true\n"
        "  print(\"bomb begun\")\n"
        "  local r = " SB_BOMB_LUA "\n"
        "  print(\"bomb finished \" .. tostring(r))\n"
        "end\n";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;

    UT_ASSERT(sbRunTicks(kMap, kLua, 4, &sim, &h) == 0);

    UT_ASSERT_MSG(strstr(sbLines, "bomb begun") != NULL,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "bomb finished") == NULL,
                  "the pattern ran to its end, so the call was never "
                  "stopped. The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(sbLineHas("on_tick raised", "instruction budget"),
                  "the hook was not stopped by the instruction budget. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(sbCount(" raised: ") == 1,
                  "%d calls were counted as errors, expected the one that "
                  "ran the pattern. The console holds:\n%s",
                  sbCount(" raised: "), sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running "
                  "after the call was cut off", (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 24. And the same bomb behind a catcher ───────────────────────── */

/* The bomb inside pcall, and inside a coroutine resumed from the hook. The
 * charge raises the budget's own error and sets its latch, so the catcher
 * raises it again rather than answering false: "caught" is never printed and
 * the hook is the call the host counts the error against. Each run has its
 * own map. */
static int sbPatternCatch(const char *map, const char *tag, const char *open,
                          const char *close) {
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          lua[1024];
    char          want[64];

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Bomb catcher\", api = 1 }\n"
             "local fired = false\n"
             "function on_tick()\n"
             "  if fired then return end\n"
             "  fired = true\n"
             "  print(\"%s begun\")\n"
             "  local ok = %sfunction()\n"
             "    return " SB_BOMB_LUA "\n"
             "  end%s\n"
             "  print(\"%s caught \" .. tostring(ok))\n"
             "end\n", tag, open, close, tag);

    UT_ASSERT(sbRunTicks(map, lua, 4, &sim, &h) == 0);

    snprintf(want, sizeof(want), "%s begun", tag);
    UT_ASSERT_MSG(strstr(sbLines, want) != NULL,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    snprintf(want, sizeof(want), "%s caught", tag);
    UT_ASSERT_MSG(strstr(sbLines, want) == NULL,
                  "the catcher answered rather than raising the budget's "
                  "error again, so a script can keep being stopped. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(sbLineHas("on_tick raised", "instruction budget"),
                  "the hook was not stopped by the instruction budget. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(sbCount(" raised: ") == 1,
                  "%d calls were counted as errors, expected the one that "
                  "ran the pattern. The console holds:\n%s",
                  sbCount(" raised: "), sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(map);
    return 0;
}

int run_scenario_sandbox_pattern_bomb_behind_pcall(void) {
    UT_ASSERT(sbPatternCatch("scnsand_patpcall.map", "pbomb", "pcall(",
                             ")") == 0);
    UT_ASSERT(sbPatternCatch("scnsand_patcoro.map", "cbomb",
                             "coroutine.resume(coroutine.create(", "))") == 0);
    return 0;
}

/* ── 25. What the four answer ─────────────────────────────────────── */

/* Every answer below was worked out by hand from the Lua 5.4 manual, not taken
 * from a run, and each is what 5.4 says on either VM. A few are where LuaJIT's
 * own matcher answers differently, and are here for that reason: %g, gmatch's
 * third argument, the empty match right after another in gmatch and gsub, a %
 * in a replacement that is not one of the escapes, and an init with a
 * fraction.
 *
 * Each line is "pat <label> = " and then everything the call returned joined
 * with |, so a nil in the middle still shows. The script prints forty-three
 * lines from one hook, under the sixty-four one call and one tick may print. */
int run_scenario_sandbox_pattern_results(void) {
    static const char *const kMap = "scnsand_patres.map";
    static const char *const kLua =
        "scenario = { name = \"Patterns\", api = 1 }\n"
        "local function show(...)\n"
        "  local t = {}\n"
        "  for i = 1, select(\"#\", ...) do\n"
        "    t[i] = tostring((select(i, ...)))\n"
        "  end\n"
        "  return table.concat(t, \"|\")\n"
        "end\n"
        "local function pat(label, ...)\n"
        "  print(\"pat \" .. label .. \" = \" .. show(...))\n"
        "end\n"
        "local function each(it)\n"
        "  local t = {}\n"
        "  for a, b in it do\n"
        "    t[#t + 1] = \"[\" .. a .. (b and (\":\" .. b) or \"\") .. \"]\"\n"
        "  end\n"
        "  return table.concat(t)\n"
        "end\n"
        "function on_start()\n"
        "  pat(\"plain\", string.find(\"hello world\", \"o w\"))\n"
        "  pat(\"init\", string.find(\"hello world\", \"o\", 6))\n"
        "  pat(\"flag\", string.find(\"a.b.c\", \".\", 3, true))\n"
        "  pat(\"negative\", string.find(\"abcabc\", \"b\", -3))\n"
        "  pat(\"past end\", string.find(\"abc\", \"b\", 10))\n"
        "  pat(\"empty at end\", string.find(\"abc\", \"\", 4))\n"
        "  pat(\"anchor\", string.find(\"hello\", \"^h\"))\n"
        "  pat(\"anchor miss\", string.find(\"hello\", \"^e\"))\n"
        "  pat(\"dollar\", string.find(\"hello\", \"lo$\"))\n"
        "  pat(\"alpha\", string.match(\"abc123 \", \"%a+\"))\n"
        "  pat(\"digit\", string.match(\"abc123\", \"%d+\"))\n"
        "  pat(\"space\", string.gsub(\"a \\n b\", \"%s\", \"\"))\n"
        "  pat(\"alnum\", string.match(\"--ab12--\", \"%w+\"))\n"
        "  pat(\"punct\", string.gsub(\"a,b.c!\", \"%p\", \"\"))\n"
        "  pat(\"graph\", string.gsub(\"a b c\", \"%g\", \"x\"))\n"
        "  pat(\"not graph\", string.gsub(\"a b\", \"%G\", \"_\"))\n"
        "  pat(\"set\", string.match(\"xyz123abc\", \"[a-c]+\"))\n"
        "  pat(\"negated set\", string.match(\"abc123\", \"[^%a]+\"))\n"
        "  pat(\"escaped bracket\", string.match(\"a]b\", \"[%]]\"))\n"
        "  pat(\"lazy\", string.match(\"<a><b>\", \"<(.-)>\"))\n"
        "  pat(\"greedy\", string.match(\"<a><b>\", \"<(.*)>\"))\n"
        "  pat(\"plus\", string.find(\"aaab\", \"a+\"))\n"
        "  pat(\"optional\", string.gsub(\"color colour\", \"colou?r\", \"X\"))\n"
        "  pat(\"captures\", string.find(\"hello world\", \"(o)(r)\"))\n"
        "  pat(\"positions\", string.match(\"hello\", \"()ll()\"))\n"
        "  pat(\"balanced\", string.match(\"f(a(b)c)d\", \"%b()\"))\n"
        "  pat(\"frontier\", string.gsub(\"hello world\", \"%f[%w]%w+\", \"X\"))\n"
        "  pat(\"frontier empty\", string.find(\"abc 123\", \"%f[%d]\"))\n"
        "  pat(\"backref\", string.find(\"xabcabcy\", \"(abc)%1\"))\n"
        "  pat(\"gmatch captures\",\n"
        "      each(string.gmatch(\"a=1, b=2\", \"(%w+)=(%w+)\")))\n"
        "  pat(\"gmatch init\", each(string.gmatch(\"one two three\", \"%a+\", 5)))\n"
        "  pat(\"gmatch empty\", each(string.gmatch(\"abc\", \"%a*\")))\n"
        "  pat(\"gsub capture\", string.gsub(\"hello world\", \"(o)\", \"[%1]\"))\n"
        "  pat(\"gsub whole\", string.gsub(\"abc\", \"%w\", \"%0%0\"))\n"
        "  pat(\"gsub percent\", string.gsub(\"50\", \"%d+\", \"%0%%\"))\n"
        "  pat(\"gsub table\", string.gsub(\"$name is $age\", \"%$(%w+)\",\n"
        "      { name = \"Bob\" }))\n"
        "  pat(\"gsub function\", string.gsub(\"abc\", \"%w\", function(c)\n"
        "      if c == \"b\" then return \"B\" end end))\n"
        "  pat(\"gsub limit\", string.gsub(\"aaaa\", \"a\", \"b\", 2))\n"
        "  pat(\"gsub anchor\", string.gsub(\"aaa\", \"^a\", \"b\"))\n"
        "  pat(\"gsub empty\", string.gsub(\"abc\", \"%w*\", \"-\"))\n"
        "  pat(\"bad replacement\", pcall(string.gsub, \"abc\", \"b\", \"%x\"))\n"
        "  pat(\"method\", (\"key=val\"):match(\"(%w+)=(%w+)\"))\n"
        "  local ok, e = pcall(string.find, \"abc\", \"b\", 1.5)\n"
        "  pat(\"fraction\", ok, string.find(tostring(e),\n"
        "      \"no integer representation\", 1, true) ~= nil)\n"
        "end\n";
    static const char *const kWant[] = {
        "pat plain = 5|7\n",
        "pat init = 8|8\n",
        "pat flag = 4|4\n",
        "pat negative = 5|5\n",
        "pat past end = nil\n",
        "pat empty at end = 4|3\n",
        "pat anchor = 1|1\n",
        "pat anchor miss = nil\n",
        "pat dollar = 4|5\n",
        "pat alpha = abc\n",
        "pat digit = 123\n",
        "pat space = ab|3\n",
        "pat alnum = ab12\n",
        "pat punct = abc|3\n",
        "pat graph = x x x|3\n",
        "pat not graph = a_b|1\n",
        "pat set = abc\n",
        "pat negated set = 123\n",
        "pat escaped bracket = ]\n",
        "pat lazy = a\n",
        "pat greedy = a><b\n",
        "pat plus = 1|3\n",
        "pat optional = X X|2\n",
        "pat captures = 8|9|o|r\n",
        "pat positions = 3|5\n",
        "pat balanced = (a(b)c)\n",
        "pat frontier = X X|2\n",
        "pat frontier empty = 5|4\n",
        "pat backref = 2|7|abc\n",
        "pat gmatch captures = [a:1][b:2]\n",
        "pat gmatch init = [two][three]\n",
        "pat gmatch empty = [abc]\n",
        "pat gsub capture = hell[o] w[o]rld|2\n",
        "pat gsub whole = aabbcc|3\n",
        "pat gsub percent = 50%|1\n",
        "pat gsub table = Bob is $age|2\n",
        "pat gsub function = aBc|3\n",
        "pat gsub limit = bbaa|2\n",
        "pat gsub anchor = baa|1\n",
        "pat gsub empty = -|1\n",
        "pat bad replacement = false|invalid use of '%' in replacement "
        "string\n",
        "pat method = key|val\n",
        "pat fraction = false|true\n",
    };
    const size_t  nWant = sizeof(kWant) / sizeof(kWant[0]);
    ServerSim    *sim   = NULL;
    ScenarioHost *h     = NULL;
    size_t        i;

    UT_ASSERT(sbRunCap(kMap, kLua, &sim, &h) == 0);

    UT_ASSERT_MSG(sbCount("pat ") == (int)nWant,
                  "%d answers were printed, expected %d. The console "
                  "holds:\n%s", sbCount("pat "), (int)nWant, sbLines);
    for (i = 0; i < nWant; i++) {
        UT_ASSERT_MSG(sbCount(kWant[i]) == 1,
                      "no line \"%.*s\". The console holds:\n%s",
                      (int)strlen(kWant[i]) - 1, kWant[i], sbLines);
    }
    UT_ASSERT_MSG(strstr(sbLines, "raised") == NULL,
                  "a pattern call raised where 5.4 answers. The console "
                  "holds:\n%s", sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 26. The charge is what bounds pattern work ───────────────────── */

/* One find of "[b]" over sixty thousand a's is a bounded piece of work: at
 * each of the 60,001 places it tries, one pass through match, one item of the
 * set read by classend and one by the compare — three steps, two at the end
 * of the subject where nothing is compared — so 180,002 steps, charged as that
 * many instructions.
 *
 * Twenty of them in one call is 3.6 million, past the call's million by more
 * than three times, so that call is stopped part way through its sixth. The
 * same twenty two to a call is 360,004 a call, well inside the call's budget
 * and the tick's, and all ten calls finish. Nothing but the charge could stop
 * the first: the subject is under the cap and the Lua around the finds is a
 * few hundred instructions. */
int run_scenario_sandbox_pattern_charge_counts(void) {
    static const char *const kMap = "scnsand_patcharge.map";
    static const char *const kLua =
        "scenario = { name = \"Charge\", api = 1 }\n"
        "local subject = string.rep(\"a\", 60000)\n"
        "local heavy = false\n"
        "local done = 0\n"
        "function on_tick()\n"
        "  if not heavy then\n"
        "    heavy = true\n"
        "    print(\"charge heavy begun\")\n"
        "    for i = 1, 20 do string.find(subject, \"[b]\") end\n"
        "    print(\"charge heavy finished\")\n"
        "    return\n"
        "  end\n"
        "  if done < 20 then\n"
        "    string.find(subject, \"[b]\")\n"
        "    string.find(subject, \"[b]\")\n"
        "    done = done + 2\n"
        "    if done == 20 then print(\"charge split finished\") end\n"
        "  end\n"
        "end\n";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    const char   *raised;
    const char   *split;

    UT_ASSERT(sbRunTicks(kMap, kLua, 18, &sim, &h) == 0);

    raised = strstr(sbLines, "on_tick raised");
    split  = strstr(sbLines, "charge split finished");
    UT_ASSERT_MSG(strstr(sbLines, "charge heavy begun") != NULL,
                  "the hook never ran, so nothing here was tested. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "charge heavy finished") == NULL,
                  "twenty finds in one call ran to their end, so the "
                  "matcher's steps are not being charged. The console "
                  "holds:\n%s", sbLines);
    UT_ASSERT_MSG(raised != NULL &&
                  sbLineHas("on_tick raised", "instruction budget"),
                  "the heavy call was not stopped by the instruction budget. "
                  "The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(sbCount(" raised: ") == 1,
                  "%d calls were counted as errors, expected only the heavy "
                  "one. The console holds:\n%s", sbCount(" raised: "),
                  sbLines);
    UT_ASSERT_MSG(split != NULL && split > raised,
                  "the same work spread over ten calls did not finish, so "
                  "something other than the call's budget is bounding it. "
                  "The console holds:\n%s", sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to still be running",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

/* ── 27. What running interpreted costs ───────────────────────────── */

/* The calls made before timing starts, so no state is paying for its first
 * touches of the chunk: the tables' first growth and the caches they warm.
 *
 * On the compiled state it is also what gets the code compiled before the
 * clock starts. LuaJIT's hot counters start at hotloop (56) times two and
 * lose two for each turn of a loop and one for each call, so on_tick's loop
 * of sixteen is hot on the fourth call and on_tick itself on about the
 * hundred-and-twelfth. The branch in the loop wants ten taken exits for a
 * side trace, which the first few calls give it. Two hundred calls are past
 * all of that with room to spare. */
#define SB_BENCH_WARM  200
/* The calls timed on each state. Around four hundred instructions a call, so
   the two interpreted states together run about four million, well under a
   second on a Debug build; the compiled one adds a fraction of that. */
#define SB_BENCH_CALLS 5000

/* A roster of sixteen tanks and an on_tick that walks it as a script's would:
 * the squared distance of each to a fixed point, with the tick folded in so
 * no call can be answered from the one before, a write back into the entry,
 * a comparison, and a summary table written at the end. Around four hundred
 * instructions a call, far inside the per-call budget. It returns a number
 * that depends on every entry and on the tick, which is what the states are
 * compared on. */
static const char kSbBenchLua[] =
    "local roster = {}\n"
    "for i = 1, 16 do\n"
    "  roster[i] = { x = i * 7 % 256, y = i * 13 % 256, armour = i % 9,\n"
    "                team = i % 4 }\n"
    "end\n"
    "local summary = { near = 0, far = 0, total = 0 }\n"
    "local cx, cy = 128, 96\n"
    "function on_tick(tick)\n"
    "  local near, total = 0, 0\n"
    "  for i = 1, #roster do\n"
    "    local e = roster[i]\n"
    "    local dx, dy = e.x - cx, e.y - cy\n"
    "    local d2 = dx * dx + dy * dy + e.armour * tick % 7\n"
    "    e.d2 = d2\n"
    "    if d2 < 4096 then near = near + 1 end\n"
    "    total = total + d2\n"
    "  end\n"
    "  summary.near = near\n"
    "  summary.far = #roster - near\n"
    "  summary.total = total\n"
    "  return total + near\n"
    "end\n";

/* The chunk loaded as text and run, leaving on_tick a global. */
static int sbBenchLoad(lua_State *L, const char *which) {
    int rc = luaL_loadbufferx(L, kSbBenchLua, sizeof(kSbBenchLua) - 1,
                              "=bench", "t");
    if (rc == 0) {
        rc = lua_pcall(L, 0, 0, 0);
    }
    UT_ASSERT_MSG(rc == 0, "the %s state refused the chunk: %s", which,
                  lua_tostring(L, -1));
    return 0;
}

/* One call of on_tick(tick), made as the host makes one: through lua_pcall,
   and armed and disarmed around it where the state is the counted one, so
   the arm, the disarm and every step the hook takes are in the figure. */
static int sbBenchCall(lua_State *L, bool armed, int tick, const char *which,
                       lua_Number *out) {
    ScnSandboxCall saved;
    int            rc;

    lua_getglobal(L, "on_tick");
    lua_pushinteger(L, tick);
    if (armed) {
        scnSandboxArmCall(L, &saved);
    }
    rc = lua_pcall(L, 1, 1, 0);
    if (armed) {
        scnSandboxDisarmCall(L, &saved);
    }
    UT_ASSERT_MSG(rc == 0, "on_tick raised on the %s state at tick %d: %s",
                  which, tick, lua_tostring(L, -1));
    *out = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return 0;
}

/* Warm-up, then SB_BENCH_CALLS timed calls, with microseconds per call and
   what the last one answered handed back. Every state is given the same
   ticks, so the last answers match only if they all ran the same code. */
static int sbBenchRun(lua_State *L, bool armed, const char *which,
                      double *usOut, lua_Number *lastOut) {
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 started;
    int    i;

    for (i = 1; i <= SB_BENCH_WARM; i++) {
        UT_ASSERT(sbBenchCall(L, armed, i, which, lastOut) == 0);
    }
    started = SDL_GetPerformanceCounter();
    for (i = 1; i <= SB_BENCH_CALLS; i++) {
        UT_ASSERT(sbBenchCall(L, armed, SB_BENCH_WARM + i, which,
                              lastOut) == 0);
    }
    *usOut = (double)(SDL_GetPerformanceCounter() - started) * 1000000.0 /
             (double)freq / (double)SB_BENCH_CALLS;
    return 0;
}

/* A state opened as the unhooked one is, with the host's library and the
   same seal, and not carrying the counting struct. */
static lua_State *sbBenchPlainState(void) {
    lua_State *L = luaL_newstate();

    if (L != NULL) {
        scnSandboxOpenLibs(L);
        scnSandboxSealRandom(L);
    }
    return L;
}

#ifdef WINBOLO_LUAJIT
/* LuaJIT's compiler switched on, the one thing luaopen_jit does that the
 * other states lack, and the jit table taken away again so the script sees
 * the same globals as on the other two. The opener is called as
 * scnSandboxOpenOne calls one: pushed, handed its name, and left to set the
 * global itself, as a 5.1 opener does.
 *
 * jit.status() is read before the global goes. A LuaJIT built without its
 * compiler answers false there, and a figure from that build would be an
 * interpreted one printed as compiled, so the case fails instead. */
static int sbBenchOpenCompiler(lua_State *L) {
    bool on;

    lua_pushcfunction(L, luaopen_jit);
    lua_pushstring(L, LUA_JITLIBNAME);
    lua_call(L, 1, 0);

    lua_getglobal(L, LUA_JITLIBNAME);
    UT_ASSERT_MSG(lua_istable(L, -1), "luaopen_jit left no jit table");
    lua_getfield(L, -1, "status");
    UT_ASSERT_MSG(lua_isfunction(L, -1), "the jit table has no status");
    lua_call(L, 0, 1);
    on = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    UT_ASSERT_MSG(on, "jit.status() says the compiler is off, so this "
                      "build has none and the compiled figure would be an "
                      "interpreted one");

    lua_pushnil(L);
    lua_setglobal(L, LUA_JITLIBNAME);
    return 0;
}
#endif

/* The same on_tick on each of these states:
 *
 * hooked — scnNewVm, which is what the host boots: counted allocator, count
 *     hook, the whitelist and the wrappers, and armed around every call as
 *     the host arms one.
 * unhooked — luaL_newstate opened through the same scnSandboxOpenLibs and
 *     sealed the same way, so it has the same library, keep lists and
 *     wrappers, and differs only in having no hook and Lua's own allocator.
 *     Without -allow-unsafe-scripts scnNewVm always sets the hook, so the
 *     host has no unhooked sandboxed state to compare with; this case builds
 *     one.
 * compiled — LuaJIT only: the unhooked state with LuaJIT's compiler switched
 *     on. It is the only one of the three the compiler runs on.
 *
 * A scenario state runs interpreted on LuaJIT because jit is never opened,
 * not because of the hook: LuaJIT turns its compiler on in luaopen_jit, and
 * scnSandboxOpenLibs opens no jit, so the unhooked state is interpreted too.
 * hooked against unhooked is then the hook's own cost on top of the
 * interpreter, on either VM, and hooked against compiled is what a scenario
 * pays compared with the same code compiled.
 *
 * The line printed is the result. Nothing is asserted about speed: timings on
 * a shared machine are not a pass or a fail. What is asserted is that every
 * state ran every call and gave the same last answer. */
int run_scenario_sandbox_interpreted_cost(void) {
    lua_State  *hooked;
    lua_State  *plain;
    double      hookedUs   = 0.0;
    double      plainUs    = 0.0;
    lua_Number  hookedLast = 0;
    lua_Number  plainLast  = 0;
#ifdef WINBOLO_LUAJIT
    lua_State  *compiled;
    double      compiledUs   = 0.0;
    lua_Number  compiledLast = 0;
#endif

    hooked = scnNewVm();
    UT_ASSERT(hooked != NULL);
    plain = sbBenchPlainState();
    UT_ASSERT(plain != NULL);
#ifdef WINBOLO_LUAJIT
    compiled = sbBenchPlainState();
    UT_ASSERT(compiled != NULL);
    UT_ASSERT(sbBenchOpenCompiler(compiled) == 0);
#endif

    UT_ASSERT(sbBenchLoad(hooked, "hooked") == 0);
    UT_ASSERT(sbBenchLoad(plain, "unhooked") == 0);
    UT_ASSERT(sbBenchRun(hooked, true, "hooked", &hookedUs,
                         &hookedLast) == 0);
    UT_ASSERT(sbBenchRun(plain, false, "unhooked", &plainUs,
                         &plainLast) == 0);
    UT_ASSERT_MSG(hookedLast == plainLast,
                  "the hooked and unhooked states answered %.17g and %.17g "
                  "on the last call, so they did not run the same code",
                  (double)hookedLast, (double)plainLast);

#ifdef WINBOLO_LUAJIT
    UT_ASSERT(sbBenchLoad(compiled, "compiled") == 0);
    UT_ASSERT(sbBenchRun(compiled, false, "compiled", &compiledUs,
                         &compiledLast) == 0);
    UT_ASSERT_MSG(compiledLast == hookedLast,
                  "the compiled state answered %.17g and the other two "
                  "%.17g on the last call, so they did not run the same "
                  "code", (double)compiledLast, (double)hookedLast);

    printf("bench: scenario on_tick hooked=%.2fus unhooked=%.2fus "
           "compiled=%.2fus hook_ratio=%.2f interp_ratio=%.2f vm=luajit "
           "calls=%d\n", hookedUs, plainUs, compiledUs,
           plainUs > 0.0 ? hookedUs / plainUs : 0.0,
           compiledUs > 0.0 ? hookedUs / compiledUs : 0.0, SB_BENCH_CALLS);
#else
    printf("bench: scenario on_tick hooked=%.2fus unhooked=%.2fus "
           "hook_ratio=%.2f vm=lua54 calls=%d\n", hookedUs, plainUs,
           plainUs > 0.0 ? hookedUs / plainUs : 0.0, SB_BENCH_CALLS);
#endif
    fflush(stdout);

    scnCloseVm(hooked);
    /* lua_close rather than scnCloseVm for the others: they were not made by
       scnSandboxNewState, so there is no counting struct beside them for
       scnSandboxCloseState to find and free. */
    lua_close(plain);
#ifdef WINBOLO_LUAJIT
    lua_close(compiled);
#endif
    return 0;
}

/* ── 28. What the info reads of a scenario's tick ─────────────────── */

/* A hundred and fifty thousand turns is 300k to 600k instructions on either
 * VM, by the calibration of case 8, and it is the only call each tick makes,
 * so the worst tick recorded has to land in that range. The upper bound is
 * 650k rather than 600k because the count moves in whole hook steps and
 * whatever the host's own calls into the state add to it.
 *
 * Then the drain of case 18 on a map of its own, which runs out of the tick's
 * total on the tick on_start runs in: that tick is recorded as a trip. */
#define SB_STATS_TURNS 150000

int run_scenario_sandbox_tick_stats_recorded(void) {
    static const char *const kMap     = "scnsand_tickstats.map";
    static const char *const kTripMap = "scnsand_tickstats_trip.map";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    char          lua[1024];

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Measured\", api = 1 }\n"
             SB_SPIN_LUA
             "function on_tick()\n"
             "  spin(%d)\n"
             "end\n", SB_STATS_TURNS);

    UT_ASSERT(sbRunTicks(kMap, lua, 5, &sim, &h) == 0);
    UT_ASSERT_MSG(sim->scenarioTickStats.ticks > 0,
                  "no scenario tick was recorded in five ticks of a round");
    UT_ASSERT_MSG(sim->scenarioTickStats.peakInstr >= 300000u &&
                  sim->scenarioTickStats.peakInstr <= 650000u,
                  "the worst tick was charged %u instructions, expected "
                  "300000 to 650000 for one call of %d turns",
                  (unsigned)sim->scenarioTickStats.peakInstr, SB_STATS_TURNS);
    UT_ASSERT_MSG(sim->scenarioTickStats.budget == SCN_BUDGET_TICK_INSTR,
                  "the budget recorded is %u, expected the tick's total of %u",
                  (unsigned)sim->scenarioTickStats.budget,
                  (unsigned)SCN_BUDGET_TICK_INSTR);
    UT_ASSERT_MSG(sim->scenarioTickStats.trips == 0,
                  "%u ticks were recorded as running out, expected none. The "
                  "console holds:\n%s",
                  (unsigned)sim->scenarioTickStats.trips, sbLines);
    UT_ASSERT_MSG(sim->scenarioTickStats.peakMs > 0.0,
                  "the worst tick took %.3fms, expected some time at all",
                  sim->scenarioTickStats.peakMs);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    sim = NULL;
    h   = NULL;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Tripped\", api = 1 }\n"
             SB_SPIN_LUA
             "local heavy = -1\n"
             "function on_start()\n"
             "  heavy = game.tick()\n"
             "  for k = 1, %d do\n"
             "    game.timer(0, function()\n"
             "      if game.tick() == heavy then spin(%d) end\n"
             "    end)\n"
             "  end\n"
             "end\n", SB_TICK_TIMERS, SB_TICK_TURNS);

    UT_ASSERT(sbRunTicks(kTripMap, lua, 6, &sim, &h) == 0);
    UT_ASSERT_MSG(sim->scenarioTickStats.trips >= 1,
                  "no tick was recorded as running out, though the drain "
                  "passes the tick's total. The console holds:\n%s", sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kTripMap);
    return 0;
}

/* ── -allow-unsafe-scripts ────────────────────────────────────────── */

/* Each case below turns the switch on, runs its body, and turns it off again
 * whichever way the body returned: the switch is one answer for the process,
 * and a case that failed with it still on would hand every case after it a
 * plain state. The bodies are separate functions so that an assertion's early
 * return lands back here rather than skipping the reset. */
static int sbUnsafeRun(int (*body)(void)) {
    int rc;

    scenarioHostSetUnsafeScripts(true);
    rc = body();
    scenarioHostSetUnsafeScripts(false);
    return rc;
}

/* The names the switch is meant to open, each printed as present or absent
 * from on_start. ffi is asked through require, because LuaJIT's full open
 * leaves it in package.preload rather than on a global; with require gone, as
 * it is in the sandbox, ffi is absent too. */
static const char kSbUnsafeLibLua[] =
    "scenario = { name = \"Open\", api = 1 }\n"
    "local function say(n, v)\n"
    "  print(\"lib \" .. n .. \" \" .. (v and \"present\" or \"absent\"))\n"
    "end\n"
    "function on_start()\n"
    "  say(\"io.open\", io ~= nil and io.open ~= nil)\n"
    "  say(\"os.execute\", os ~= nil and os.execute ~= nil)\n"
    "  say(\"require\", require ~= nil)\n"
    "  say(\"debug.getinfo\", debug ~= nil and debug.getinfo ~= nil)\n"
    "  say(\"load\", load ~= nil)\n"
    "  say(\"string.dump\", string.dump ~= nil)\n"
    "  say(\"math.randomseed\", math.randomseed ~= nil)\n"
    "  say(\"jit\", jit ~= nil)\n"
    "  say(\"ffi\", require ~= nil and (pcall(require, \"ffi\")))\n"
    "end\n";

static const char *const kSbUnsafeLibNames[] = {
    "io.open", "os.execute", "require", "debug.getinfo", "load",
    "string.dump", "math.randomseed",
#ifdef WINBOLO_LUAJIT
    "jit", "ffi",
#endif
};

/* One attach of the script above on its own map, run into its first tick,
 * with every name asserted to have come back as want says. */
static int sbUnsafeLibReport(const char *map, const char *want) {
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];
    char          line[96];
    size_t        n;
    int           i;

    UT_ASSERT(sbPutText(map, kSbUnsafeLibLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, map, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < 3; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);

    for (n = 0; n < sizeof(kSbUnsafeLibNames) / sizeof(kSbUnsafeLibNames[0]);
         n++) {
        snprintf(line, sizeof(line), "lib %s %s\n", kSbUnsafeLibNames[n],
                 want);
        UT_ASSERT_MSG(sbCount(line) == 1,
                      "expected '%s' to be %s. The console holds:\n%s",
                      kSbUnsafeLibNames[n], want, sbLines);
    }

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(map);
    return 0;
}

/* The same script twice: once with the switch on, where every name is there,
 * and once with it off on a second map, where every one is gone. The second
 * half is what says the first is the switch's doing rather than a library the
 * host opens anyway. */
static int sbUnsafeOpensFullLibrary(void) {
    UT_ASSERT(sbUnsafeLibReport("scnsand_unsafe_lib.map", "present") == 0);
    scenarioHostSetUnsafeScripts(false);
    UT_ASSERT(sbUnsafeLibReport("scnsand_unsafe_lib_off.map", "absent") == 0);
    return 0;
}

int run_scenario_sandbox_unsafe_opens_full_library(void) {
    return sbUnsafeRun(sbUnsafeOpensFullLibrary);
}

/* Everything the sandbox bounds, done in one call with the switch on.
 *
 * Five million turns is between ten and fifteen million instructions by the
 * per-call case's reckoning, ten times the call's budget and five times the
 * tick's, and a small fraction of a second whether it runs interpreted or
 * compiled. The string is one byte past the cap string.rep refuses at. The
 * getinfo is what says string.find is the VM's own: the sandbox's pattern
 * functions are a C closure over two upvalues, and the VM's has none. And
 * twice as many lines as one call may print, every one of which has to
 * arrive. */
static int sbUnsafeLiftsTheBudgets(void) {
    static const char *const kMap = "scnsand_unsafe_budget.map";
    const int     lines = SCN_PRINT_PER_CALL * 2;
    ServerSim    *sim;
    ScenarioHost *h;
    char          lua[1024];
    char          want[64];
    char          err[512];
    int           i;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Unbounded\", api = 1 }\n"
             "function on_start()\n"
             "  local x = 0\n"
             "  for i = 1, 5000000 do x = x + 1 end\n"
             "  print(\"unsafe loop \" .. x)\n"
             "  print(\"unsafe rep \" .. #string.rep(\"x\", %u))\n"
             "  print(\"unsafe find nups \" ..\n"
             "        debug.getinfo(string.find, \"u\").nups)\n"
             "  for i = 1, %d do print(\"unsafe line \" .. i) end\n"
             "end\n", (unsigned)SCN_STRING_MAX + 1u, lines);
    UT_ASSERT(sbPutText(kMap, lua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < 3; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(sbCount("unsafe loop 5000000\n") == 1,
                  "the loop did not run to its end, so a budget still "
                  "applies. The console holds:\n%s", sbLines);
    snprintf(want, sizeof(want), "unsafe rep %u\n",
             (unsigned)SCN_STRING_MAX + 1u);
    UT_ASSERT_MSG(sbCount(want) == 1,
                  "string.rep did not build a string past the cap. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(sbCount("unsafe find nups 0\n") == 1,
                  "string.find is not the VM's own. The console holds:\n%s",
                  sbLines);
    for (i = 1; i <= lines; i++) {
        snprintf(want, sizeof(want), "unsafe line %d\n", i);
        UT_ASSERT_MSG(sbCount(want) == 1,
                      "line %d of the %d one call printed did not arrive, so "
                      "the print limit still applies. The console holds:\n%s",
                      i, lines, sbLines);
    }
    UT_ASSERT_MSG(strstr(sbLines, "raised") == NULL,
                  "a call was counted as an error. The console holds:\n%s",
                  sbLines);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is in state %d, expected it to be running",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

int run_scenario_sandbox_unsafe_lifts_the_budgets(void) {
    return sbUnsafeRun(sbUnsafeLiftsTheBudgets);
}

/* A chunk dumped into a buffer, for the script file that is bytecode. */
typedef struct {
    char   bytes[4096];
    size_t len;
    bool   full;
} SbDump;

static int sbDumpWriter(lua_State *L, const void *p, size_t sz, void *ud) {
    SbDump *d = (SbDump *)ud;

    (void)L;
    if (sz > sizeof(d->bytes) - d->len) {
        d->full = true;
        return 1;
    }
    memcpy(d->bytes + d->len, p, sz);
    d->len += sz;
    return 0;
}

/* Two ways a precompiled chunk arrives. A script that dumps a function and
 * loads the bytes back, which the sandbox answers by having neither
 * string.dump nor load; and a script file that is itself bytecode, compiled
 * here by the same VM the host runs, which the sandbox's loader refuses as
 * run_scenario_sandbox_bytecode_chunk_refused shows. Both are taken with the
 * switch on. */
static int sbUnsafeLoadsBytecode(void) {
    static const char *const kMap     = "scnsand_unsafe_dump.map";
    static const char *const kMapComp = "scnsand_unsafe_compiled.map";
    static const char *const kLua =
        "scenario = { name = \"Dumper\", api = 1 }\n"
        "function on_start()\n"
        "  print(\"unsafe dumped \" ..\n"
        "        load(string.dump(function() return 7 end))())\n"
        "end\n";
    static const char *const kCompiledSrc =
        "print(\"unsafe precompiled ran\")\n"
        "scenario = { name = \"Compiled\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    lua_State    *C;
    SbDump        dump;
    char          err[512];
    int           rc;
    int           i;

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < 3; i++) {
        serverSimTick(sim);
    }
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(sbCount("unsafe dumped 7\n") == 1,
                  "the dumped function did not load back and answer 7. The "
                  "console holds:\n%s", sbLines);
    UT_ASSERT_MSG(strstr(sbLines, "raised") == NULL,
                  "a call was counted as an error. The console holds:\n%s",
                  sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);

    /* The file: the source compiled and dumped by a bare state of the same
       VM, written beside the second map as it stands. */
    C = luaL_newstate();
    UT_ASSERT(C != NULL);
    dump.len  = 0;
    dump.full = false;
    rc = luaL_loadbufferx(C, kCompiledSrc, strlen(kCompiledSrc),
                          "=compiled", "t");
    if (rc == 0) {
#ifdef WINBOLO_LUAJIT
        rc = lua_dump(C, sbDumpWriter, &dump);
#else
        rc = lua_dump(C, sbDumpWriter, &dump, 0);
#endif
    }
    lua_close(C);
    UT_ASSERT_MSG(rc == 0 && !dump.full && dump.len > 0,
                  "the source did not compile and dump (rc %d, %u bytes)",
                  rc, (unsigned)dump.len);

    UT_ASSERT(sbPut(kMapComp, dump.bytes, dump.len));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    sbWatchConsole(sim);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMapComp, err, sizeof(err));
    sbUnwatchConsole(sim);

    UT_ASSERT_MSG(h != NULL,
                  "a precompiled script file was refused with the switch "
                  "on: %s", err);
    UT_ASSERT_MSG(sbCount("unsafe precompiled ran\n") == 1,
                  "the precompiled chunk attached without running its top "
                  "level. The console holds:\n%s", sbLines);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMapComp);
    return 0;
}

int run_scenario_sandbox_unsafe_loads_bytecode(void) {
    return sbUnsafeRun(sbUnsafeLoadsBytecode);
}

/* What the lobby settings say about the switch, filled by the sim the way
 * every publish is. The sim cannot ask the host, so this is the attach's
 * word carried through serverSimSetScenarioIdentity and nothing else.
 *
 * On with the switch on; off again after a detach, since a lobby with no
 * script says nothing about scripts; and off on a second attach of the same
 * script with the switch off, which is what says the first answer was the
 * switch's and not something every attach sets. */
static int sbUnsafeReachesTheLobby(void) {
    static const char *const kMap = "scnsand_unsafe_lobby.map";
    static const char *const kLua =
        "scenario = { name = \"Lobby\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    ControlEvent  evt;
    char          err[512];

    UT_ASSERT(sbPutText(kMap, kLua));
    sim = sbSim();
    UT_ASSERT(sim != NULL);

    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT_MSG(evt.u.lobbySettings.scenarioSource != lobbyScenarioNone,
                  "setup: the attach left the lobby with no scenario");
    UT_ASSERT_MSG(evt.u.lobbySettings.scenarioUnsafe,
                  "the switch was on and the lobby settings say the server "
                  "sandboxes its scripts");

    scenarioHostDetach(h);
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT_MSG(!evt.u.lobbySettings.scenarioUnsafe,
                  "the detach left the lobby saying the server runs scripts "
                  "without the sandbox");

    scenarioHostSetUnsafeScripts(false);
    err[0] = '\0';
    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the second attach was refused: %s", err);
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT_MSG(evt.u.lobbySettings.scenarioSource != lobbyScenarioNone,
                  "setup: the second attach left the lobby with no scenario");
    UT_ASSERT_MSG(!evt.u.lobbySettings.scenarioUnsafe,
                  "the switch was off and the lobby settings say the server "
                  "runs scripts without the sandbox");

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sbDrop(kMap);
    return 0;
}

int run_scenario_sandbox_unsafe_reaches_the_lobby(void) {
    return sbUnsafeRun(sbUnsafeReachesTheLobby);
}
