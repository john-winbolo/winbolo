#!/usr/bin/env python3
"""C / Lua parity for attack.evaluate_pill_difficulty.

goals.lua's pool-6 pill evaluation runs the C fast path
(gh_attack.evaluate_pill_difficulty) in BOTH the production and the recorded
brain -- since 8673ca7c there is no mode in which the Lua sweep quietly
supplies the decision instead.  So the C port has to answer the same question
the Lua sweep answers, and this test is what holds it to that.

WHAT IS ASSERTED (decision parity)
  For every evaluated pill, the C path must
    * find a spot whenever the Lua sweep finds one (and find none when it
      finds none), and
    * pick the SAME spot -- identical mx, my and approach degree.
  A C "no spot" where Lua has one is the failure that matters: pool 6 scores
  the pill INF, logs `SYNC_P6 ... skipped=no_spot`, and attack_pill never
  starts.  That is what this arena set caught.

  The score is compared separately.  C sums the ellipse in its own order, so
  a result within PARITY_SCORE_TOL (relative, see attack.lua's
  M._parity_check) of the Lua score is reported as a SCOREDRIFT rather than a
  mismatch -- it cannot change which spot wins, because the spot is already
  asserted equal.  Measured today the drift count is ZERO on all three
  arenas: every score matches to the last bit.  DRIFT_BUDGET below is the
  documented tolerance; raise it only with a run that shows the drift is
  order-of-summation and never flips a decision.

HOW IT WORKS
  attack.lua carries the comparison itself.  With BRAIN_DEBUG_MODE on and the
  "attackparity" token in -bot-init's [arg], every C evaluation is repeated
  through the Lua sweep and both answers are printed as one line:

    ATTACK_PARITY pill=(x,y) id=.. C=(score,mx,my,deg) LUA=(..) MATCH

  The harness is inside `if BRAIN_DEBUG_MODE`, so lua_strip removes it from
  opt/ entirely and a production game never pays for it -- and never runs it
  even if the token is passed.  That also means this test MUST run with
  -brain-debug (opt/ has no harness to switch on).

ARENAS
  blocked_aim   -- a neutral pill ringed by our OWN pills.  The pre-fix C read
                   grass as impassable and returned no spot at all here.
  builder_pool D -- walls at the standoff; the attack_pill-with-walls case.
  DH-Oil Rig    -- a real 2v2 for 3000 ticks; ~560 evaluations across four
                   bots, which is where the volume comes from.

BASELINE (pre-fix C, same harness, DH-Oil Rig): 410 MISMATCH out of 511.
AFTER:                                          0 out of 559.

Usage: python attack_eval_parity_test.py [--build DIR] [--arena NAME]
Exit 0 on PASS, 1 on FAIL.
"""

import os
import re
import sys
import glob
import subprocess
from pathlib import Path

from asap import asap_args, pacing_line  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"

# A run with fewer than this many comparisons proves nothing -- it would go
# green if the harness silently stopped firing.
MIN_LINES = {"blocked_aim": 1, "builder_pool_D": 1, "oilrig": 200}

# Documented tolerance: same spot, score off by rounding.  Zero today.
DRIFT_BUDGET = 0

PARITY_RE = re.compile(r"ATTACK_PARITY .*?(MATCH|MISMATCH|SCOREDRIFT\([^)]*\)|LUA_ERROR=.*)$")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def bot_init(spec):
    """-bot-init value with the parity token appended to every brain path.

    NOTE the path is passed as given: `brains/...` resolved from the build
    directory hits build/Brains (the copy the build step makes), not the repo
    tree, so every path here is absolute.
    """
    return spec


def run_arena(name, build_dir):
    """Runs one arena and returns (lines, error_message)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"

    label = f"_parity_{name}"
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    common = ["-brain-debug", "-brain-no-budget-kill", "-brain-lua-seed", "42",
              "-nowinbolonet", "-quiet"] + asap_args()
    arg = f"{BRAIN}[attackparity]"

    if name == "blocked_aim":
        subprocess.run([sys.executable, str(HERE / "generate_blocked_aim_map.py")],
                       check=True, stdout=subprocess.DEVNULL)
        cmd = [str(ds), "-map", str(HERE / "blocked_aim.map"),
               "-port", "50216", "-nolobby", "-gametype", "open",
               "-bots", "1", "-brain", str(BRAIN),
               "-bot-init", f"0={arg}",
               "-ai", "yesfull", "-limit", "20",
               "-seed", "42", "-ticks", "2500", "-threads", "1"] + common
        timeout = 600
    elif name == "builder_pool_D":
        subprocess.run([sys.executable, str(HERE / "generate_builder_pool_map.py"), "D"],
                       check=True, stdout=subprocess.DEVNULL)
        cmd = [str(ds), "-map", str(HERE / "builder_pool_D.map"),
               "-port", "50217", "-nolobby", "-gametype", "open",
               "-bots", "1", "-brain", str(BRAIN),
               "-bot-init", f"0={arg}",
               "-ai", "yesfull", "-limit", "20",
               "-seed", "42", "-ticks", "6000", "-threads", "1"] + common
        timeout = 900
    elif name == "oilrig":
        cmd = [str(ds), "-map", str(REPO / "data" / "maps" / "DH-Oil Rig.map"),
               "-gametype", "tournament", "-ai", "yes", "-nolobby",
               "-notracker", "-dontsendlog", "-noinput",
               "-bots", "4", "-threads", "4", "-brain", str(BRAIN),
               "-bot-init", f"0-3={arg}", "-teams", "2,2",
               "-port", "50218", "-seed", "4242", "-ticks", "3000"] + common
        timeout = 1200
    else:
        return None, f"unknown arena {name}"

    subprocess.run(cmd, cwd=str(build_dir), env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                   timeout=timeout)

    sess = newest_session(build_dir, label)
    if not sess:
        return None, ("no debug session produced (is the cwd on a drive with "
                      ">50 GB free? -brain-debug records nothing otherwise)")
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        return None, "brain crashed -- " + str(crashes[0])

    lines = []
    for p in sorted(sess.glob("print2_bot*.log")):
        for ln in p.read_text(errors="ignore").splitlines():
            m = PARITY_RE.search(ln.strip())
            if m:
                lines.append((m.group(1), ln.strip()))
    if not lines:
        return None, (f"no ATTACK_PARITY lines under {sess} -- the harness did "
                      f"not run.  It needs -brain-debug (opt/ has it stripped) "
                      f"AND the 'attackparity' token in -bot-init's [arg].")
    return lines, None


def main(argv):
    build_dir = DEFAULT_BUILD
    only = None
    i = 0
    while i < len(argv):
        if argv[i] == "--build" and i + 1 < len(argv):
            build_dir = Path(argv[i + 1]).resolve(); i += 2
        elif argv[i] == "--arena" and i + 1 < len(argv):
            only = argv[i + 1]; i += 2
        else:
            i += 1

    print(pacing_line())
    arenas = [a for a in ("blocked_aim", "builder_pool_D", "oilrig")
              if only is None or a == only]
    failures = []
    for name in arenas:
        print(f"=== {name} ===")
        lines, err = run_arena(name, build_dir)
        if err:
            print(f"  FAIL: {err}")
            failures.append(f"{name}: {err}")
            continue
        n = len(lines)
        mism = [ln for v, ln in lines if v == "MISMATCH"]
        errs = [ln for v, ln in lines if v.startswith("LUA_ERROR")]
        drift = [ln for v, ln in lines if v.startswith("SCOREDRIFT")]
        print(f"  comparisons: {n}   MATCH: {n - len(mism) - len(errs) - len(drift)}"
              f"   MISMATCH: {len(mism)}   SCOREDRIFT: {len(drift)}"
              f"   LUA_ERROR: {len(errs)}")
        need = MIN_LINES.get(name, 1)
        if n < need:
            print(f"  FAIL: only {n} comparison(s), expected at least {need} "
                  f"-- the arena stopped evaluating pills, so a green result "
                  f"would prove nothing")
            failures.append(f"{name}: too few comparisons ({n} < {need})")
        for ln in mism[:5]:
            print(f"    {ln}")
        for ln in errs[:3]:
            print(f"    {ln}")
        if mism:
            failures.append(f"{name}: {len(mism)} MISMATCH")
        if errs:
            failures.append(f"{name}: {len(errs)} LUA_ERROR")
        if len(drift) > DRIFT_BUDGET:
            for ln in drift[:5]:
                print(f"    {ln}")
            failures.append(f"{name}: {len(drift)} SCOREDRIFT "
                            f"(budget {DRIFT_BUDGET})")

    if failures:
        print("FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("PASS: the C evaluator and the Lua sweep pick the same spot, with "
          "the same score, on every evaluation in every arena.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
