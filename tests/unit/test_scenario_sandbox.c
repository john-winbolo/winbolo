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

/* ── 1. What the whitelist took, and what it kept ─────────────────── */

/* The script builds a list of everything that is wrong and raises with it,
 * so the attach fails and the operator line names the leak. An attach that
 * succeeds is the whole assertion: nothing was there that should not be, and
 * everything that should be there answered a call.
 *
 * math.randomseed is on the absent list and math.random on the present one,
 * which is the pair that says the host drew the round's seed before the seal
 * rather than losing the seeding with the function. */
int run_scenario_sandbox_removed_names_are_nil(void) {
    static const char *const kMap = "scnsand_names.map";
    static const char *const kLua =
        "local leaks = {}\n"
        "local gone = { \"io\", \"package\", \"require\", \"debug\", \"ffi\",\n"
        "               \"jit\", \"bit\", \"load\", \"loadstring\", \"dofile\",\n"
        "               \"loadfile\", \"module\", \"newproxy\" }\n"
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
        "local kept = { \"pcall\", \"pairs\", \"tostring\", \"setmetatable\" }\n"
        "for _, n in ipairs(kept) do\n"
        "  if type(_G[n]) ~= \"function\" then\n"
        "    leaks[#leaks + 1] = \"no \" .. n\n"
        "  end\n"
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
