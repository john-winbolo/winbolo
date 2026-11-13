#!/usr/bin/env python3
"""Brain sandbox test.

Runs real brains in a real brain VM, through WinBoloHeadless, which keeps the
sandbox on. The unit tests cannot: they stub luaBrainInstanceCreate.

  * A probe brain records whether each thing the sandbox takes away is gone. It
    only asks: it calls nothing native and starts nothing.
  * A brain whose init.lua starts like bytecode must be refused as bytecode, not
    read as bytecode. Its contents are not valid bytecode.
  * GoalHunter must still load and play without a crash, from opt/ as a release
    runs it.

Usage: python brain_sandbox_test.py [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
MAP = REPO / "data" / "maps" / "Everard Island.map"
# A brain that does not load is a warning: the host plays on without it.
EXIT_BRAIN_REFUSED = 0

PROBE = r"""
local results = {}
local function say(name, ok)
  results[#results + 1] = name .. "=" .. (ok and "ok" or "BAD")
end

-- Each of these must be gone.
say("no_ffi", not pcall(require, "ffi"))
say("no_jit_util", not pcall(require, "jit.util"))
say("no_jit_profile", not pcall(require, "jit.profile"))
say("no_loadstring", loadstring == nil)
say("no_load", load == nil)
say("no_dofile", dofile == nil)
say("no_string_dump", string.dump == nil)
say("no_getfenv", getfenv == nil)
say("no_setfenv", setfenv == nil)
say("no_newproxy", newproxy == nil)
say("no_loadlib", package.loadlib == nil)
say("no_searchpath", package.searchpath == nil)
say("no_astar_log", cpf_astar_log_enable == nil)
say("no_popen", io.popen == nil)
say("no_execute", os.execute == nil)
say("no_getenv", os.getenv == nil)
for k in pairs(debug) do
  say("debug_" .. tostring(k), k == "traceback" or k == "getinfo")
end
local preloaded = 0
for _ in pairs(package.preload) do preloaded = preloaded + 1 end
say("preload_empty", preloaded == 0)
say("no_func_from_getinfo", debug.getinfo(1, "f").func == nil
                            and debug.getinfo(1).func == nil
                            and debug.getinfo(print).func == nil)
-- The levels still mean what they mean in plain Lua.
local here = debug.getinfo(1, "Sl")
say("getinfo_level_1_is_caller", here.source:find("init.lua", 1, true) ~= nil
                                 and here.currentline > 0)
local function callee() return debug.getinfo(2, "Sl").currentline end
local line_before = debug.getinfo(1, "l").currentline
say("getinfo_level_2_is_callers_caller", callee() == line_before + 1)
say("getinfo_function_form", debug.getinfo(callee, "S").what == "Lua")
say("getinfo_needs_a_level", not pcall(debug.getinfo))
say("getinfo_negative_level", debug.getinfo(-1) == nil)
do
  local co = coroutine.create(function()
    local inner = debug.getinfo(coroutine.running(), 1, "S")
    coroutine.yield(inner and inner.source or "")
  end)
  local _, source = coroutine.resume(co)
  say("getinfo_this_thread_form", type(source) == "string"
                                  and source:find("init.lua", 1, true) ~= nil)
  local other = debug.getinfo(co, 0, "S")
  say("getinfo_other_thread_form", other ~= nil and other.func == nil)
end

-- The jail: a file beside the brain's folder cannot be opened.
local outside = io.open("@OUTSIDE@", "w")
say("no_write_outside", outside == nil)
if outside then outside:close() end

-- A stamp file that claims more tiles than a path holds is refused. The bytes
-- are the header (magic, version, 1 angle, 1 nudge, step, 3 floats), then one
-- entry of 6 paths, each with n_tiles at its byte 9.
local path = string.rep("\0", 9) .. string.char(255) .. string.rep("\0", 30)
local stamp = "SHLD" .. string.char(2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
              .. string.rep("\0", 16) .. string.rep(path, 6)
local stamp_path = "@BRAIN@/bad_stamp.bin"
local f = io.open(stamp_path, "wb")
say("write_inside", f ~= nil)
if f then
  f:write(stamp)
  f:close()
  say("bad_stamp_refused", gh_shield and gh_shield.load(stamp_path) == false)
end

-- What brains do need is still here.
say("has_traceback", type(debug.traceback) == "function")
say("has_getinfo", type(debug.getinfo) == "function")
say("has_require", type(require) == "function")
say("has_loadfile", type(loadfile) == "function")
if jit then
  say("has_jit_off", type(jit.off) == "function")
  say("has_jit_on", type(jit.on) == "function")
  say("has_jit_status", type(jit.status) == "function")
  say("no_jit_opt", jit.opt == nil and not pcall(require, "jit.opt"))
  say("no_jit_flush", jit.flush == nil)
  say("no_jit_attach", jit.attach == nil)
  say("has_bit", pcall(require, "bit"))
end

local out = io.open("@BRAIN@/results.txt", "w")
out:write(table.concat(results, "\n"), "\n")
out:close()

return { think = function() return {} end }
"""


# A brain whose table raises on __newindex and whose _G raises on every miss. It
# runs on the server's own bot path, where the host writes to the brain's table
# every tick. It counts its thinks, so a server that dies or never thinks reads
# as FAIL.
HOSTILE_METATABLE = r"""
local open, rawset, setmetatable, error = io.open, rawset, setmetatable, error
local thinks = 0
local brain_table = setmetatable({
  think = function()
    thinks = thinks + 1
    local out = open("@BRAIN@/thinks.txt", "w")
    out:write(tostring(thinks))
    out:close()
    return {}
  end,
}, {
  __newindex = function() error("brain __newindex") end,
})
local function hit() error("global metamethod") end
setmetatable(_G, { __index = hit, __newindex = hit })
return brain_table
"""


def find_headless(build_dir):
    for candidate in (build_dir / "WinBoloHeadless.exe",
                      build_dir / "WinBoloHeadless",
                      build_dir / "Release" / "WinBoloHeadless.exe"):
        if candidate.exists():
            return candidate
    return None


def run_brain(headless, brain, ticks, cwd):
    env = dict(os.environ)
    env["WINBOLO_LOG"] = "lua=info"
    done = subprocess.run(
        [str(headless), "-nocrashreporting", "--fast", "--map", str(MAP),
         "--brain", str(brain), "--ticks", str(ticks)],
        cwd=str(cwd), env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=300)
    return done.returncode, done.stdout.decode("utf-8", "replace")


def check_probe(headless, tmp):
    failures = []
    brain_dir = tmp / "Probe"
    brain_dir.mkdir()
    outside = tmp / "outside.txt"
    source = PROBE.replace("@OUTSIDE@", outside.as_posix())
    source = source.replace("@BRAIN@", brain_dir.as_posix())
    (brain_dir / "init.lua").write_text(source, encoding="utf-8")
    code, log = run_brain(headless, brain_dir / "init.lua", 2, tmp)
    if code != 0:
        failures.append("the probe's host exited with %d; its log:\n%s" % (code, log[-1500:]))
    results = brain_dir / "results.txt"
    if not results.exists():
        return failures + ["the probe wrote no results; its log:\n" + log[-1500:]]
    for line in results.read_text(encoding="utf-8").splitlines():
        print("  " + line)
        if line.endswith("=BAD"):
            failures.append(line)
    if outside.exists():
        failures.append("a file outside the brain's folder was written")
    return failures


def check_bytecode_refused(headless, tmp):
    brain_dir = tmp / "Bytecode"
    brain_dir.mkdir()
    # What LuaJIT and PUC-Lua bytecode starts with, and nothing a VM could run.
    (brain_dir / "init.lua").write_bytes(b"\x1bLJ\x02\x00\x00\x00\x00")
    code, log = run_brain(headless, brain_dir / "init.lua", 2, tmp)
    if code != EXIT_BRAIN_REFUSED:
        return ["the host of a bytecode init.lua exited with %d; its log:\n%s"
                % (code, log[-1500:])]
    wanted = ("wrong mode", "mode is 't'")
    if any(word in log for word in wanted):
        print("  bytecode init.lua refused by mode")
        return []
    return ["a bytecode init.lua was not refused by mode; its log:\n" + log[-1500:]]


def check_hostile_metatable(build_dir, tmp):
    ds = None
    for candidate in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
                      build_dir / "Release" / "WinBoloDS.exe"):
        if candidate.exists():
            ds = candidate
            break
    if ds is None:
        return ["WinBoloDS not found under %s" % build_dir]
    brain_dir = tmp / "Hostile"
    brain_dir.mkdir()
    source = HOSTILE_METATABLE.replace("@BRAIN@", brain_dir.as_posix())
    (brain_dir / "init.lua").write_text(source, encoding="utf-8")
    done = subprocess.run(
        [str(ds), "-nocrashreporting", "-map", str(MAP), "-port", "50077",
         "-nolobby", "-gametype", "open", "-ai", "yes", "-limit", "20",
         "-bots", "1", "-brain", str(brain_dir / "init.lua"),
         "-ticks", "200", "-seed", "42", "-nowinbolonet", "-quiet",
         "-threads", "1", "-asap"],
        cwd=str(tmp), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=300)
    log = done.stdout.decode("utf-8", "replace")
    if done.returncode != 0:
        return ["the server exited with %d; its log:\n%s" % (done.returncode, log[-1500:])]
    thinks = brain_dir / "thinks.txt"
    if not thinks.exists():
        return ["the hostile brain never thought; the server's log:\n" + log[-1500:]]
    count = int(thinks.read_text(encoding="utf-8"))
    if count < 3:
        return ["the hostile brain thought only %d times" % count]
    print("  server survived, and the brain thought %d times" % count)
    return []


def check_goalhunter(headless, tmp, ticks):
    failures = []
    # Outside debug mode the loader takes opt/init.lua when it exists.
    brain = REPO / "brains" / "GoalHunter" / "init.lua"
    code, log = run_brain(headless, brain, ticks, tmp)
    if code != 0:
        failures.append("GoalHunter's host exited with %d; its log:\n%s" % (code, log[-1500:]))
    for marker in ("BRAIN_CRASH", "failed to load", "error running",
                   "did not return a table"):
        if marker in log:
            start = log.find(marker)
            failures.append("GoalHunter: %s\n%s" % (marker, log[start:start + 800]))
    if not failures:
        print("  GoalHunter ran %d ticks with no brain error" % ticks)
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ticks", type=int, default=3000,
                        help="ticks GoalHunter plays")
    parser.add_argument("--build", type=Path, default=REPO / "build")
    args = parser.parse_args()
    # The hosts are started from another folder, so a relative path would
    # no longer name them.
    args.build = args.build.resolve()

    headless = find_headless(args.build)
    if headless is None:
        print("FAIL: WinBoloHeadless not found under %s" % args.build)
        return 1

    failures = []
    tmp = Path(tempfile.mkdtemp(prefix="wb_brain_sandbox_"))
    try:
        print("Probe brain:")
        failures += check_probe(headless, tmp)
        print("Bytecode init.lua:")
        failures += check_bytecode_refused(headless, tmp)
        print("Hostile metatables on a server:")
        failures += check_hostile_metatable(args.build, tmp)
        print("GoalHunter:")
        failures += check_goalhunter(headless, tmp, args.ticks)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("FAIL:")
        for failure in failures:
            print("  " + failure)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
