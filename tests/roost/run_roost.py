#!/usr/bin/env python
"""Run the ROOST scenario tests and print a PASS/FAIL table.

A ROOST test is a map and a scenario script beside it, both named after the
test.  The script drives the round and ends it with a verdict; this runner
starts a dedicated server on the map, waits for the round to finish, reads
the verdict off the server's console output, and reports.

    C:\\Python310\\python.exe tests/roost/run_roost.py
    C:\\Python310\\python.exe tests/roost/run_roost.py say_stop_halts_bot
    C:\\Python310\\python.exe tests/roost/run_roost.py --exe build/WinBoloDS.exe

Exit code is 0 when every test passed and 1 otherwise.  See README.md in
this directory for the convention the scripts follow.
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# The line a scenario writes with game.log to say how it went.  game.log is
# the only call whose text reaches the server's console: the round's own end
# text goes to the lobby, which a headless run has nobody in.
VERDICT_PREFIX = "ROOST VERDICT "

# What every test's server is given unless the test overrides it with an
# .args file.  Two allied GoalHunter bots, because the smallest interesting
# question about a bot is what one of them does that the other does not.
#
#   -nolobby   a lobby never starts a round with no human in it to ready up,
#              so a headless round has to skip the lobby altogether.
#   -ai yes    without it the server refuses to seat a brain at all.
#   -asap      ticks back to back; the simulation is unchanged.
#   -seed      one seed, so a test that passes passes again.
DEFAULT_ARGS = [
    "-gametype", "tournament",
    "-ai", "yes",
    "-nolobby",
    "-notracker",
    "-nowinbolonet",
    "-dontsendlog",
    "-noinput",
    "-bots", "2",
    "-allybots", "1",
    "-threads", "4",
    "-brain", "brains/GoalHunter_1.7/init.lua",
    "-seed", "42",
    "-asap",
]

# A round that has not said anything by here is a round that is stuck.  The
# server is given a tick ceiling too, so this only fires when the server
# itself has hung.
DEFAULT_TICKS = 20000
DEFAULT_TIMEOUT = 240


def discover(names):
    """Every test in this directory, or the ones named, in name order."""
    found = sorted(p.stem[: -len(".scenario")]
                   for p in HERE.glob("*.scenario.lua"))
    if not names:
        return found
    missing = [n for n in names if n not in found]
    if missing:
        raise SystemExit("no such ROOST test: " + ", ".join(missing))
    return [n for n in found if n in names]


def args_for(name):
    """The server arguments for one test: its own .args file, or the default.

    An .args file is one line of flags, split on whitespace, replacing the
    defaults wholesale rather than adding to them — a test that needs four
    bots on two teams needs to say so once, not fight the default.
    """
    override = HERE / (name + ".args")
    if override.exists():
        args = override.read_text(encoding="utf-8").split()
    else:
        args = list(DEFAULT_ARGS)
    return with_bot_chat_on(args)


def with_bot_chat_on(args):
    """Start every bot with "bot chat" ON.

    A game starts with the bots quiet (BOT_CHAT_DEFAULT is false in
    constants.lua), but most of these tests read the spoken acks to see that
    an order was taken.  So every bot on the test's -brain gets
    cfg=BOT_CHAT_DEFAULT=true through -bot-init.  A test that names its own
    -bot-init is left alone.
    """
    if "-bot-init" in args or "-brain" not in args:
        return args
    i = args.index("-brain")
    if i + 1 >= len(args):
        return args
    brain = args[i + 1]
    return args + ["-bot-init", "0-15=%s[cfg=BOT_CHAT_DEFAULT=true]" % brain]


def run_one(name, exe, port, ticks, timeout, keep_output):
    """Run one test and answer (verdict, detail, seconds)."""
    mapfile = HERE / (name + ".map")
    if not mapfile.exists():
        return "FAIL", "no map beside the script: %s" % mapfile.name, 0.0

    cmd = [str(exe), "-map", str(mapfile)]
    cmd += args_for(name)
    cmd += ["-port", str(port), "-ticks", str(ticks)]

    started = time.time()
    try:
        # The server is run from the repository root: -brain and the rest of
        # the default arguments are paths relative to it.
        done = subprocess.run(cmd, cwd=str(REPO), timeout=timeout,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = done.stdout.decode("utf-8", "replace")
        crashed = done.returncode != 0
    except subprocess.TimeoutExpired as e:
        output = (e.stdout or b"").decode("utf-8", "replace")
        crashed = False
        elapsed = time.time() - started
        if keep_output:
            (HERE / (name + ".out")).write_text(output, encoding="utf-8")
        return "FAIL", "the round did not finish inside %ds" % timeout, elapsed

    elapsed = time.time() - started
    if keep_output:
        (HERE / (name + ".out")).write_text(output, encoding="utf-8")

    verdicts = [line.strip()[len(VERDICT_PREFIX):].strip()
                for line in output.splitlines()
                if line.strip().startswith(VERDICT_PREFIX)]
    if not verdicts:
        tail = "\n".join(output.splitlines()[-8:])
        return "FAIL", "the round said no verdict; last lines:\n" + tail, elapsed
    # The first verdict is the one that decided the round; a script that
    # writes a second one after ending is writing into a round that is over.
    verdict = verdicts[0]
    if crashed:
        return "FAIL", "the server exited badly after: " + verdict, elapsed
    if verdict.startswith("PASS"):
        return "PASS", verdict, elapsed
    return "FAIL", verdict, elapsed


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tests", nargs="*",
                    help="test names to run; every test when none are named")
    ap.add_argument("--exe", default=None,
                    help="the dedicated server to run "
                         "(default: build-own/WinBoloDS.exe, then build/)")
    ap.add_argument("--port", type=int, default=29500,
                    help="UDP port for the first test; one more for each "
                         "after it (default 29500)")
    ap.add_argument("--ticks", type=int, default=DEFAULT_TICKS,
                    help="tick ceiling handed to the server (default %d)"
                         % DEFAULT_TICKS)
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
                    help="seconds one test may take (default %d)"
                         % DEFAULT_TIMEOUT)
    ap.add_argument("--keep-output", action="store_true",
                    help="write each test's console output to <name>.out")
    opts = ap.parse_args()

    if opts.exe:
        exe = Path(opts.exe)
        if not exe.is_absolute():
            exe = (REPO / exe).resolve()
    else:
        for candidate in ("build-own/WinBoloDS.exe", "build/WinBoloDS.exe"):
            exe = REPO / candidate
            if exe.exists():
                break
    if not exe.exists():
        raise SystemExit("no dedicated server at %s; name one with --exe" % exe)

    names = discover(opts.tests)
    if not names:
        raise SystemExit("no ROOST tests in %s" % HERE)

    width = max(len(n) for n in names)
    failures = 0
    print("ROOST: %d test(s) with %s" % (len(names), exe))
    for i, name in enumerate(names):
        sys.stdout.write("  %-*s  ... " % (width, name))
        sys.stdout.flush()
        verdict, detail, secs = run_one(name, exe, opts.port + i, opts.ticks,
                                        opts.timeout, opts.keep_output)
        print("%-4s  %5.1fs" % (verdict, secs))
        if verdict != "PASS":
            failures += 1
            for line in detail.splitlines():
                print("      " + line)
        elif os.environ.get("ROOST_VERBOSE"):
            print("      " + detail)

    print("ROOST: %d passed, %d failed" % (len(names) - failures, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
