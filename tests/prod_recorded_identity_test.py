#!/usr/bin/env python3
"""prod-vs-recorded identity — the gate's cheap copy of tests/identity_check.py.

THE RULE (memory: reference_prod_vs_recorded_identity)
-------------------------------------------------------
The production brain (brains/<name>/opt/, loaded when there is no -brain-debug)
and the recorded brain (brains/<name>/*.lua, loaded with -brain-debug) must play
BYTE-IDENTICAL games for the same seed under the determinism flags.  If they do
not, every recording, every BrainTest session and every bench is showing a bot
that is not the one that plays.

This file is the PIGEON gate's copy: one seed, short, under 90 s.  It is not the
whole check — tests/identity_check.py carries the full docs, the failure
forensics and the tunable setup, and is what you run at 20000 or 60000 ticks
when this one goes red or after a brain change you want to clear properly:

    C:\\Python310\\python.exe tests/identity_check.py --ticks 60000 --seed 1

WHY seed 4242 AND 6000 TICKS
----------------------------
The gate has a time budget, so the seed is chosen for how FAST it exposes a
divergence rather than for coverage.  On the DH-Oil Rig 2v2 the two bugs found
on 2026-09-06 showed up at:

    bug B (init.lua's stop_predict_live overlay reading terrain through the
           change-DETECTOR U.ttype instead of U.ttype_peek)
        seed 4242 -> engine tick 2579   <- this gate, 6000 ticks = 2.3x margin
        seed 1    -> engine tick 31127

    bug A (viz.lua's pill_best_spots_back / pill_best_spots_aggro missing
           `default_on = false`, which turned on place_pill_strategic's
           `viz_only` candidate scan in recorded games only)
        seed 1    -> engine tick 17701

Verified: this exact gate configuration goes RED on the pre-fix brain, at row
2580.  What it does NOT promise is that every future regression of this class
surfaces inside 6000 ticks -- bug A on seed 1 needed 17701, and a gate that
ran that long would blow the budget three times over.  The gate is a smoke
alarm; after any brain change worth clearing properly, run the deep check:

    C:\\Python310\\python.exe tests/identity_check.py --ticks 60000 --seed 1
    C:\\Python310\\python.exe tests/identity_check.py --ticks 60000 --seed 4242

DO NOT make this cheaper with -bd-noviz.  That sets _BT_VIZ_COLLECT="off", so
viz.is_on() answers false for every layer — which is precisely what hides this
class of bug.

STATUS 2026-09-06 — GREEN HERE DOES NOT MEAN IDENTITY IS CLEAN
--------------------------------------------------------------
Three causes were found and fixed on 2026-09-06 (viz default_on, the terrain
detector, the pool-breakdown report mutating pool state).  They moved the first
divergence a long way but did NOT finish the job:

    seed 4242 @60000 : 2579  -> 33971 -> CLEAN  (md5 d2cdc59b, 60001 rows)
    seed 2    @20000 : CLEAN
    seed 5    @9000  : CLEAN
    seed 1    @60000 : 17701 -> 31127 -> 36139   still broken
    seed 3    @20000 : 17479 -> 19795            still broken

Production play did not move at all: the seed-1 and seed-4242 production md5s
are the same before and after all three fixes (e80c9472 / d2cdc59b).  Every fix
was to code the production brain never runs, so what changed is the RECORDED
brain, onto production's game.

So this gate passes while a known break survives past its window.  Treat a
green here as "no fast regression", not as "identity holds".  The open repro
for whoever picks it up:

    C:\\Python310\\python.exe tests/identity_check.py --ticks 20000 --seed 3

and the method that found all three (it is quick once you have a seed that
breaks early): shadow `local BRAIN_DEBUG_MODE = false` at the top of a module
in a scratch copy of the brain and bisect over modules; if shadowing EVERY
module still differs, the cause is host-side, and `-bd-nopool` / `-bd-nojsonl`
/ `-bd-noprint2` say which recorder subsystem it is.

SEPARATE ISSUE, also seen 2026-09-06: under heavy CPU load (three
identity_check runs at once) the recorded run is not always reproducible --
this gate went red at row 3417 on seed 4242 under load and green, twice, run
alone.  Run identity checks ONE AT A TIME, or you will chase ghosts.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKER = os.path.join(ROOT, "tests", "identity_check.py")
BRAIN_DIR = os.path.join(ROOT, "brains", "GoalHunter")
LUA_STRIP = os.path.join(ROOT, "build", "Release", "lua_strip.exe")

SEED = 4242
TICKS = 6000
PORT = 28600
BUDGET_S = 90

# Visualizers that do not merely DRAW: switching them on makes the brain take a
# different code path (the "lua version" of a scan, an extra candidate sweep).
# Each MUST declare `default_on = false` in viz.lua, or an unattended
# -braindebug host -- which pushes no _BT_VIZ_* toggles, so viz.is_on() falls
# back to the declared default and a missing default reads as ON -- records a
# game the production brain never played.  Add to this list whenever you add an
# overlay that forces a path; it is the static half of this gate.
PATH_FORCING_VIZ = [
    "pill_best_spots_back",     # goals.lua eval_place_pill_strategic viz_only
    "pill_best_spots_aggro",    # ditto
    "shield_scan_candidates",   # attack_shield.lua _want_full_scan_viz
    "shield_blocker_union",     # ditto
    "attack_scan_spots_all_pills",  # goals.lua force_detailed
]

# Functions that look like reads and are really DETECTORS: they mutate brain
# state (util.lua's terrain_prev + changes.terrain) as a side effect, and
# changes.terrain drives threat.lua's pill-danger recompute.  Debug-only code --
# anything lua_strip deletes from opt/ -- must never call one, or the recorded
# brain primes tiles the production brain never touches.  Use U.ttype_peek().
DETECTOR_CALLS = ["U.ttype(", "U.traw(", "util.ttype(", "util.traw("]


def check_viz_defaults():
    """Every path-forcing overlay declares default_on = false."""
    src = open(os.path.join(BRAIN_DIR, "viz.lua"), encoding="utf-8").read()
    bad = []
    for vid in PATH_FORCING_VIZ:
        m = re.search(r'^\s{2}' + re.escape(vid) + r'\s*=\s*\{', src, re.M)
        if not m:
            bad.append("%s: no entry in viz.lua M.IDS" % vid)
            continue
        nxt = re.search(r'^\s{2}[A-Za-z_][A-Za-z0-9_]*\s*=\s*\{', src[m.end():], re.M)
        body = src[m.start(): m.end() + (nxt.start() if nxt else 4000)]
        if not re.search(r'default_on\s*=\s*false', body):
            bad.append("%s: forces a code path but does not declare "
                       "default_on = false (reads ON in unattended -braindebug)"
                       % vid)
    return bad


def _norm(path):
    return [l.rstrip("\r\n") for l in
            open(path, encoding="utf-8", errors="replace")]


def check_strip_invariants():
    """Strip the brain into a temp dir with the shipping flags, then check two
    things the runtime half of this gate is too short to guarantee:

      a) opt/ IS that strip.  A root .lua edited without regenerating opt/ is
         the most ordinary way to make the two brains differ, and it needs no
         cleverness at all to happen.
      b) No line the strip REMOVES calls a terrain DETECTOR.  Removed lines are
         exactly the debug-only ones; U.ttype/U.traw mutate terrain_prev and
         changes.terrain, so a call from there gives the recorded brain state
         the production brain never had (2026-09-06 bug B).
    """
    if not os.path.exists(LUA_STRIP):
        return ["build/Release/lua_strip.exe missing -- cannot run the strip "
                "invariants (build it, or run the deep check by hand)"]
    tmp = tempfile.mkdtemp(prefix="identity_strip_")
    try:
        rc = subprocess.call(
            [LUA_STRIP, "--strip", "print2", "--strip", "viz.",
             "--strip", "overlay_", "--strip-block", "if BRAIN_DEBUG_MODE",
             "--exclude", "los_stamp_cache.lua",
             "--exclude", "shield_stamp_cache.lua", tmp]
            + sorted(glob.glob(os.path.join(BRAIN_DIR, "*.lua"))),
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        if rc != 0:
            return ["lua_strip failed (rc=%s)" % rc]
        bad = []
        for src_path in sorted(glob.glob(os.path.join(BRAIN_DIR, "*.lua"))):
            name = os.path.basename(src_path)
            out_path = os.path.join(tmp, name)
            if not os.path.exists(out_path):
                continue              # excluded from the strip (stamp caches)
            fresh = _norm(out_path)
            # (a) opt/ up to date.  Compared line-by-line after newline
            # normalisation: lua_strip writes CRLF, opt/ is committed LF.
            opt_path = os.path.join(BRAIN_DIR, "opt", name)
            if not os.path.exists(opt_path):
                bad.append("opt/%s missing -- regenerate opt/" % name)
            elif _norm(opt_path) != fresh:
                bad.append("opt/%s is stale: it is not the current strip of "
                           "%s -- regenerate opt/ (brains/GoalHunter/"
                           "strip.sh) before committing" % (name, name))
            # (b) no detector call on a line the strip removes.
            kept = set(fresh)
            for i, line in enumerate(_norm(src_path), 1):
                if line in kept:
                    continue          # survives the strip: production code
                if any(c in line for c in DETECTOR_CALLS):
                    bad.append("%s:%d debug-only line calls a terrain detector "
                               "-- use U.ttype_peek(): %s"
                               % (name, i, line.strip()[:90]))
        return bad
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    static = check_viz_defaults() + check_strip_invariants()
    if static:
        print("prod-vs-recorded identity: STATIC CHECKS FAILED")
        for s in static:
            print("  - %s" % s)
        print()
        print("These are the shapes that broke identity on 2026-09-06.")
        print("Fix them before the run below is worth anything.")
        return 1
    print("static checks OK: opt/ is the current strip, %d path-forcing viz "
          "declare default_on = false, no terrain-detector calls in "
          "debug-only code" % len(PATH_FORCING_VIZ))

    argv = [sys.executable, CHECKER,
            "--ticks", str(TICKS), "--seed", str(SEED),
            "--port", str(PORT), "--label", "gate_identity"]
    print("prod-vs-recorded identity: seed %d, %d ticks (budget %ds)"
          % (SEED, TICKS, BUDGET_S))
    t0 = time.time()
    rc = subprocess.call(argv, cwd=ROOT)
    dt = time.time() - t0

    if rc != 0:
        print()
        print("FAIL: the opt/ brain and the root brain played DIFFERENT games.")
        print("      Re-run with the full checker for the forensics:")
        print("        C:\\Python310\\python.exe tests/identity_check.py "
              "--ticks 20000 --seed %d --keep" % SEED)
        print("      Usual causes, in the order they have actually happened:")
        print("        1. a viz id that forces a code path and forgot "
              "`default_on = false` in viz.lua")
        print("        2. lua_strip eating an `if BRAIN_DEBUG_MODE ... else "
              "<decision> end` whole")
        print("        3. debug-only work with side effects the decision reads")
        print("        4. opt/ simply not regenerated after a root .lua edit")
        return 1

    print()
    print("PASS: production and recorded games are byte-identical (%.0fs)" % dt)
    if dt > BUDGET_S:
        print("NOTE: took %.0fs, over the %ds gate budget — lower TICKS if this"
              " keeps happening." % (dt, BUDGET_S))
    return 0


if __name__ == "__main__":
    sys.exit(main())
