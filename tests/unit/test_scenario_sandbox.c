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
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
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
#include "test_harness.h"

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
