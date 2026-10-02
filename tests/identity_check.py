#!/usr/bin/env python3
"""Production vs recorded identity: the opt/ brain and the root brain must play
the SAME game.

THE RULE
--------
A GoalHunter brain ships twice.  `brains/<name>/*.lua` is the source the
recorder runs (winbolods -brain-debug, BrainTest) and `brains/<name>/opt/*.lua`
is the lua_strip'd copy every production host loads.  Those two must produce a
BYTE-IDENTICAL game for the same seed, or every bench, every recording and
every "watch it in BrainTest" session is measuring a bot that is not the one
that plays.  With the determinism flags on --

    -brain-no-budget-kill -brain-lua-seed <n> -asap

-- one seed is one canonical game, so the check is a plain file compare of two

BUILD REQUIREMENT
-----------------
Run this against a build configured with

    cmake -DWINBOLO_LUAJIT_DETERMINISTIC=ON ...

Without it LuaJIT seeds its string hash from OS entropy at every VM creation,
pairs() order over hash tables changes run to run, and the brain's /info
broadcast and eval scan orders change with it -- so two runs of the SAME brain
on the SAME seed diverge and this check reports a difference that is not there.
The flag is OFF by default because a fixed hash seed weakens a shipped build
(see the comment on the option in CMakeLists.txt); it is a measurement setting,
not a product one.

`-snapjson -snapinterval 1` streams: run the setup once plain (opt/, no
recorder) and once with -brain-debug (root files, recorder on) and diff.

WHAT BREAKS IT (all four have happened)
---------------------------------------
  1. lua_strip removing a whole `if BRAIN_DEBUG_MODE ... else <decision> end`
     block -- the strip takes the block WHOLE, else-branch included, so the
     production brain lost the decision code in the else.
  2. A debug-only pre-probe that filled a cache the decision then read, on a
     different tick schedule from the production path.
  3. A visualizer that FORCES A DIFFERENT CODE PATH (the "lua version" of a
     scan, or an extra candidate sweep) and does not declare
     `default_on = false`.  An unattended -braindebug host pushes no viz
     toggles, so viz.lua falls back to the declared default -- and a toggle
     that forgot to declare one reads as ON.  2026-09-06 bug A:
     pill_best_spots_back / pill_best_spots_aggro turned on the
     place_pill_strategic `viz_only` full candidate scan in every recorded
     game and in no production game.  Seed 1 diverged at engine tick 17701.
  4. Debug-only code calling a function that LOOKS like a read and is really a
     detector.  2026-09-06 bug B: init.lua's stop_predict_live overlay read the
     tank tile's terrain with U.ttype() for a speed cap.  U.ttype primes
     util.lua's terrain_prev and pushes changed tiles into changes.terrain,
     which threat.lua's check_terrain_dirty turns into a pill-danger recompute
     -- so ONE debug-only terrain read moved threat.pill_at under the tank
     (6.92 -> 6.15), which moved imdanger, which moved the steering.  Seed 4242
     diverged at engine tick 2579.  The fix is U.ttype_peek(), the
     side-effect-free read; debug and viz-gated code must use it.

USAGE
-----
    C:\\Python310\\python.exe tests/identity_check.py                  # seed 1, 20000 ticks
    C:\\Python310\\python.exe tests/identity_check.py --ticks 60000 --seed 4242
    C:\\Python310\\python.exe tests/identity_check.py --keep           # keep the artifacts

Exit 0 = the two games are byte-identical.  Exit 1 = they are not, and the
first differing snapshot row is printed for both sides with the per-tank delta
called out, which is normally enough to name the bot and the tick to go and
read in the recorded run's print2 log.

NOTE ON -bd-* FLAGS: do NOT add -bd-noviz to make this cheaper.  It sets
_BT_VIZ_COLLECT="off", which makes viz.is_on() answer false for everything --
masking exactly the class of bug (3) above.
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join("build", "WinBoloDS.exe")
DEFAULT_MAP = "data/maps/DH-Oil Rig.map"
DEFAULT_BRAIN = "brains/GoalHunter/init.lua"


def build_argv(exe, mapfile, brain, bot_init, bots, teams, threads, port, seed,
               ticks, snap, debug):
    argv = [
        exe,
        "-map", mapfile,
        "-gametype", "tournament", "-ai", "yes", "-nolobby", "-quitonwin",
        "-notracker", "-nowinbolonet", "-dontsendlog", "-noinput",
        "-bots", str(bots), "-threads", str(threads), "-teams", teams,
        "-port", str(port), "-seed", str(seed), "-ticks", str(ticks),
        "-brain", brain,
        # Determinism flags. -brain-no-budget-kill also hands out the 1000 ms
        # slow-mo budget, which pins the capacity tier at 10 in both runs, so
        # -brain-tier is not needed on top of it.
        "-brain-no-budget-kill", "-brain-lua-seed", "42", "-asap", "-quiet",
        "-snapjson", snap, "-snapinterval", "1",
    ]
    if bot_init:
        argv += ["-bot-init", bot_init]
    if debug:
        argv += ["-brain-debug"]
    return argv


def first_diff(path_a, path_b):
    """Return (line_no, line_a, line_b) of the first differing line, 1-based."""
    with open(path_a, encoding="utf-8", errors="replace") as fa, \
         open(path_b, encoding="utf-8", errors="replace") as fb:
        n = 0
        while True:
            n += 1
            la = fa.readline()
            lb = fb.readline()
            if not la and not lb:
                return None
            if la != lb:
                return n, la.rstrip("\n"), lb.rstrip("\n")


def describe_row(la, lb):
    """Human-readable per-tank delta between two snapshot rows."""
    try:
        a, b = json.loads(la), json.loads(lb)
    except Exception:
        return ["  (rows are not both valid JSON; raw compare only)"]
    out = ["  engine tick %s" % a.get("tick")]
    for ta, tb in zip(a.get("tanks", []), b.get("tanks", [])):
        if ta != tb:
            keys = [k for k in ta if ta.get(k) != tb.get(k)]
            out.append("  tank %s (%s): %s" % (
                ta.get("player"), ta.get("name"),
                ", ".join("%s %s -> %s" % (k, ta.get(k), tb.get(k)) for k in keys)))
    for key in ("pillboxes", "bases"):
        if a.get(key) != b.get(key):
            for i, (x, y) in enumerate(zip(a.get(key, []), b.get(key, []))):
                if x != y:
                    out.append("  %s[%d]: %s -> %s" % (key, i, x, y))
    if len(out) == 1:
        out.append("  (rows differ outside tanks/pillboxes/bases)")
    return out


def md5_of(path):
    if not os.path.exists(path):
        return "(missing)"
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def rows_of(path):
    if not os.path.exists(path):
        return 0
    with open(path, encoding="utf-8", errors="replace") as f:
        return sum(1 for _ in f)


def scan_log(path, bots):
    """Return (n_started, errors) from a WINBOLO_LOG=sim=info run log."""
    started, errors = 0, []
    if not os.path.exists(path):
        return 0, ["(no log written)"]
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "started with brain" in line:
                started += 1
            if "ERROR" in line:
                errors.append(line.rstrip())
    return started, errors


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ticks", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--map", default=DEFAULT_MAP)
    ap.add_argument("--brain", default=DEFAULT_BRAIN)
    ap.add_argument("--bot-init", default=None,
                    help="default: side A on the pre-2026-09-05 repair formula, "
                         "side B on the current defaults (exercises both)")
    ap.add_argument("--bots", type=int, default=4)
    ap.add_argument("--teams", default="2,2")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--port", type=int, default=28400)
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--label", default="identchk")
    ap.add_argument("--workdir", default=os.path.join("build", "identity_check"))
    ap.add_argument("--keep", action="store_true",
                    help="keep the snapshot streams and the debug_sessions dir")
    args = ap.parse_args()

    bot_init = args.bot_init
    if bot_init is None:
        bot_init = ("0-1=%s[cfg=BUILDER_POOL_REPAIR_LINEAR=false],2-3=%s"
                    % (args.brain, args.brain))

    work = os.path.join(ROOT, args.workdir)
    os.makedirs(work, exist_ok=True)
    snaps, logs, procs = {}, {}, {}

    env = dict(os.environ)
    env["WINBOLO_LOG"] = "sim=info"

    t0 = time.time()
    for i, mode in enumerate(("plain", "debug")):
        # Label goes in the filename: two identity_check runs in parallel (a
        # gate run and a hand-run deep check, say) must not share a stream.
        snaps[mode] = os.path.join(work, "%s_%s_s%d.json"
                                   % (args.label, mode, args.seed))
        logs[mode] = snaps[mode] + ".log"
        for p in (snaps[mode], logs[mode]):
            if os.path.exists(p):
                try:
                    os.remove(p)
                except OSError as e:
                    print("cannot clear %s: %s" % (p, e))
                    return 1
        e = dict(env)
        if mode == "debug":
            e["WINBOLO_BRAINDBG_LABEL"] = args.label
        argv = build_argv(args.exe, args.map, args.brain, bot_init, args.bots,
                          args.teams, args.threads, args.port + i * 2, args.seed,
                          args.ticks, snaps[mode], mode == "debug")
        procs[mode] = subprocess.Popen(
            argv, cwd=ROOT, env=e,
            stdout=open(logs[mode], "w", encoding="utf-8", errors="replace"),
            stderr=subprocess.STDOUT)
    rcs = {m: p.wait() for m, p in procs.items()}
    dt = time.time() - t0

    failures = []
    want_lines = args.ticks + 1
    for mode in ("plain", "debug"):
        if rcs[mode] != 0:
            failures.append("%s run exited rc=%s" % (mode, rcs[mode]))
        n = sum(1 for _ in open(snaps[mode], encoding="utf-8", errors="replace")) \
            if os.path.exists(snaps[mode]) else 0
        if n != want_lines:
            failures.append("%s snapshot has %d rows, want %d (game ended early?)"
                            % (mode, n, want_lines))
        started, errors = scan_log(logs[mode], args.bots)
        if started != args.bots:
            failures.append("%s started %d bot(s), want %d" % (mode, started, args.bots))
        if errors:
            failures.append("%s log has %d ERROR line(s): %s"
                            % (mode, len(errors), errors[0]))

    print("identity_check: seed %d, %d ticks, %s (%.0fs)"
          % (args.seed, args.ticks, args.brain, dt))
    for mode in ("plain", "debug"):
        print("  %-5s md5=%s rows=%s  %s"
              % (mode, md5_of(snaps[mode]), rows_of(snaps[mode]), snaps[mode]))

    diff = None
    if os.path.exists(snaps["plain"]) and os.path.exists(snaps["debug"]):
        diff = first_diff(snaps["plain"], snaps["debug"])

    if diff is not None:
        n, la, lb = diff
        failures.append("games diverge at snapshot row %d (engine tick %d)"
                        % (n, n - 1))
        print()
        print("FIRST DIFFERING ROW  (row %d, engine tick %d)" % (n, n - 1))
        for line in describe_row(la, lb):
            print(line)
        print()
        print("  production : %s" % la[:400])
        print("  recorded   : %s" % lb[:400])

    if not args.keep:
        for d in glob.glob(os.path.join(ROOT, "debug_sessions", "*_" + args.label)):
            shutil.rmtree(d, ignore_errors=True)
        for d in glob.glob(os.path.join(ROOT, "build", "debug_sessions",
                                        "*_" + args.label)):
            shutil.rmtree(d, ignore_errors=True)
        if not failures:
            for mode in ("plain", "debug"):
                for p in (snaps[mode], logs[mode]):
                    if os.path.exists(p):
                        try:
                            os.remove(p)
                        except OSError:
                            pass

    print()
    if failures:
        print("IDENTITY BROKEN:")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("IDENTITY OK: production and recorded games are byte-identical "
          "(%d rows each)" % want_lines)
    return 0


if __name__ == "__main__":
    sys.exit(main())
