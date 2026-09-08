#!/usr/bin/env python3
"""Capture-target LGM PRIORITY test (GoalHunter constants.lua
CAPTURE_LGM_PRIORITY).

WHAT IS BEING MEASURED
----------------------
A bot on capture_pill drives at a dead pillbox to grab it.  An ENEMY BUILDER
working beside that corpse is rebuilding it out from under us: the moment his
repair lands the free pill is a live enemy pillbox and the capture is gone.

There are two answers to that in the brain and they are INDEPENDENT knobs:

  CAPTURE_LGM_HUNT      the SWEEP.  The goal never changes; while it is
                        capture_pill the turret is blended onto a man within
                        CAPTURE_LGM_HUNT_RADIUS (2) tiles of the target pill.
                        Tested by tests/capture_lgm_hunt_test.py.

  CAPTURE_LGM_PRIORITY  THIS ONE.  Steering is untouched; instead the man's
                        kill_lgm pool row (pool 13) is multiplied by
                        CAPTURE_LGM_PRIORITY_MAX_COST (4) whenever he stands
                        within CAPTURE_LGM_PRIORITY_RADIUS (8) tiles,
                        CHEBYSHEV, of the CAPTURE TARGET PILL.  kill_lgm
                        outbids capture_pill, the bot goes and shoots him, and
                        then the pool runs normally -- capture_pill may or may
                        not win back, priced exactly as it always was.

THE MEMORY, AND WHY IT IS THE INTERESTING PART.  The discount is only visible
while the capture flow has a target, so a naive implementation flip-flops: the
tick kill_lgm wins the pool the target is gone, the discount vanishes, kill_lgm
jumps back to full price and capture_pill wins straight back -- forever, at one
goal change per tick.  goals.lua therefore REMEMBERS the target pill
(state.cap_prio): stamped from the goal while the goal is capture_pill, HELD
while the goal is kill_lgm, dropped on any other goal or as soon as the tile
stops being capturable.  This test bounds the capture_pill <-> kill_lgm switch
rate to catch a regression there, and reports the count either way.

WHAT THE DEFAULT MULTIPLIER CAN AND CANNOT WIN.  Worth knowing before reading a
result: capture_pill for a dead pill within IMMINENT_CAPTURE_PATH_COST (30) is
floored at IMMINENT_CAPTURE_FLOOR = 5 whatever the distance, and kill_lgm's
cheapest branch is TANK_COMBAT_LOS_BASE_COST(5) + dist x
TANK_COMBAT_LOS_COST_PER_TILE(3).  Times CAPTURE_LGM_PRIORITY_MULT 0.333 that
is under 5 only while the man is within 3 tiles of the TANK; the standoff
branch starts at KILL_LGM_BASE_COST 20, i.e. 6.7 discounted, and can never beat
an imminent capture at all.  So the option bites when the builder is right on
the corpse we are arriving at -- which is the case it was asked for -- and is
priced out when he is further off.  The arena is built to sit exactly on that
edge; the generator asserts it.

THREE ARENAS (tests/generate_capture_lgm_priority_map.py -- read its docstring;
it is the hunt arena with the corpse moved three tiles west of the builder's
errand tiles, so the man is INSIDE the priority box and OUTSIDE the sweep box)

  P1  THE MEASUREMENT.  Sweep pinned off, priority on (its default).
      PASS: the discount applies (CAPTURE_LGM_PRIO lines, whose cost=A->B is
      checked to be A*mult); the man named is really inside the radius; the
      bot actually SWITCHES from capture_pill to kill_lgm on a tick the
      discount is live (that is the option doing its job, as opposed to
      kill_lgm winning on its own merits); the capture still happens (goal
      returns to capture_pill, and corpses get taken); and the
      capture_pill<->kill_lgm switch rate stays under the bound.

  P0  THE CONTROL.  cfg=CAPTURE_LGM_PRIORITY=false.
      PASS: not one CAPTURE_LGM_PRIO line, and the bot still gets its
      captures.

  PK  THE BASELINE.  preset=keel (which carries CAPTURE_LGM_PRIORITY=false and
      CAPTURE_LGM_HUNT=false among everything else).
      PASS: same as P0.

WHY P0 PINS ONLY THE ONE KNOB.  BotInitSlot.arg is 127 bytes
(luabrainshandler.h), and "both masters off" plus the three arena pins is 152.
It does not need both: the sweep is INERT in this arena by construction -- its
LGM trigger needs a man within 2 tiles of the corpse and the errand tiles are
6, and its armour trigger needs the corpse's armour to go UP, which the
scripted enemy never does (he paves a road and chops a tree; he never repairs
the pill).  Every variant asserts ZERO CAPTURE_LGM_HUNT lines, which is the
proof rather than the assumption, so P1 vs P0 differ in exactly one live
behaviour.

Usage: python capture_lgm_priority_test.py [--variant P0|P1|PK|ALL]
                                           [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
import os
import re
import subprocess
import sys
from pathlib import Path

# -asap by default: ticks run back-to-back instead of one per 20 ms of wall
# clock. Same seed -> byte-identical game, just faster. --no-asap (or
# WINBOLO_ASAP=0) puts this run back on the 20 ms live-game pacing.
from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = (REPO / "brains" / "GoalHunter_1.7" / "init.lua").as_posix()
ENEMY = (REPO / "tests" / "brains" / "farm_beside.lua").as_posix()

sys.path.insert(0, str(HERE))
from generate_capture_lgm_priority_map import (      # noqa: E402
    CORPSE, PRIO_RADIUS, PRIO_CAP, nbots)

VARIANTS = ("P0", "P1", "PK")
# Deliberately clear of 31000-31999 and of the hunt suite's 50570-50572.
PORTS = {"P0": 50580, "P1": 50581, "PK": 50582}
# ENGINE ticks.  The brain thinks once per frame and the sim advances two
# engine ticks per frame, so these are half as many brain ticks.  The arena
# refills the corpse fourteen times and the builder is only out for part of
# each of his round trips, so the run has to be long.
TICKS = {"P0": 24000, "P1": 24000, "PK": 24000}

ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h

# ── the -bot-init tokens ─────────────────────────────────────────────────
# The three pins are the hunt suite's, for the same reasons:
#   TANK_COMBAT_ENABLED=false     the scripted enemy is a visible hostile tank
#     that never fires; without this our bot takes attack_tank and chases it
#     and the capture stops happening.
#   BUILDER_POOL_ENABLED=false    keeps OUR man in the tank, so rescue_lgm /
#     wait_for_lgm noise does not land on top of the measurement.
#   STRATEGIC_PLACE_ENABLED=false the bot keeps what it grabs instead of
#     planting it; a live friendly pillbox beside the corpse would shoot the
#     builder and there would be nothing left to measure.
COMMON = ("cfg=TANK_COMBAT_ENABLED=false"
          ";cfg=BUILDER_POOL_ENABLED=false"
          ";cfg=STRATEGIC_PLACE_ENABLED=false")
TOKENS = {
    # Sweep pinned off explicitly even though it is inert here, so P1 cannot
    # be blamed on it.
    "P1": "cfg=CAPTURE_LGM_HUNT=false;cfg=CAPTURE_LGM_PRIORITY=true;" + COMMON,
    "P0": "cfg=CAPTURE_LGM_PRIORITY=false;" + COMMON,
    "PK": "preset=keel;" + COMMON,
}

# goals.lua refresh_kill_lgm, on the discount's start/stop plus a heartbeat
# every CAPTURE_LGM_PRIORITY_LOG_TICKS:
#   CAPTURE_LGM_PRIO t=310 pill=(122,126) lgm=(128,126) d=6 mult=0.333
#     cost=63->21
PRIO_RE = re.compile(
    r"CAPTURE_LGM_PRIO t=(\d+) pill=\((\d+),(\d+)\) lgm=\((-?\d+),(-?\d+)\) "
    r"d=(\d+) cap=([\d.]+) cost=(-?[\d.]+)->(-?[\d.]+)")
PRIO_OFF_RE = re.compile(r"CAPTURE_LGM_PRIO t=(\d+) verdict=off")
# The sweep's own lines -- must not appear at all in this arena.
HUNT_ANY_RE = re.compile(r"CAPTURE_LGM_HUNT t=\d+")
# init.lua per-tick engine dump -- the goal the brain was actually on.
TICKGOAL_RE = re.compile(r"TICK_COST t=(\d+) .*? goal=(\w+)")
# The sidecar's own trace (engine truth), in SIM ticks.
SPENT_RE = re.compile(r"^# spent (\d+) n=(\d+)", re.M)

# Switch-rate bound.  The brain thinks once per two engine ticks, so a 24000
# tick run is 12000 brain ticks; the constants call 100 brain ticks "~2 s @
# 50 Hz", which puts a minute at 3000 brain ticks.  A healthy run alternates
# once per approach -- the arena offers fifteen corpses, so a handful of
# switches per minute is normal and anything near the tick rate is the
# flip-flop this bound exists to catch.
BRAIN_TICKS_PER_MIN = 3000
MAX_SWITCHES_PER_MIN = 40


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def run_one(variant, ticks, build_dir):
    """Run the arena.  Returns (session, bot0_log_text, trace_text) or
    (None, error_string, None)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}", None
    subprocess.run([sys.executable,
                    str(HERE / "generate_capture_lgm_priority_map.py"),
                    variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"clp_{variant}"
    stderr = HERE / f"capture_lgm_priority_{variant}_stderr.txt"
    trace = build_dir / f"capture_lgm_priority_{variant}_trace.log"
    for p in (stderr, trace):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                return None, (f"{p.name} is locked -- a previous WinBoloDS run "
                              "is still going. Wait for it to exit, then "
                              "retry."), None

    tok = TOKENS[variant]
    if len(tok) > ARG_MAX:
        return None, (f"the -bot-init token string is {len(tok)} bytes, over "
                      f"the {ARG_MAX}-byte BRAIN_INIT_ARG limit -- it would be "
                      f"truncated mid-token and the last cfg= silently "
                      f"ignored:\n  {tok}"), None
    n = nbots(variant)
    init = f"0={BRAIN}[{tok}],1={ENEMY}[]"
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"capture_lgm_priority_{variant}.map"),
           "-port", str(PORTS[variant]), "-nolobby",
           "-gametype", "open",
           "-bots", str(n), "-brain", BRAIN,
           "-bot-init", init,
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-allow-unsafe-brains",
           "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(900, ticks // 2))

    sess = newest_session(build_dir, label)
    if not sess:
        return None, ("no debug session produced (is the cwd on a drive with "
                      ">50 GB free? -brain-debug silently records nothing "
                      "otherwise)"), None
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        return None, ("brain crashed -- " + str(crashes[0]) + "\n"
                      + Path(crashes[0]).read_text(errors="ignore")[:2000]), None
    log = sess / "print2_bot0.log"
    if not log.exists():
        return None, f"no print2_bot0.log under {sess}", None
    tr = trace.read_text(errors="ignore") if trace.exists() else ""
    return sess, log.read_text(errors="ignore"), tr


def parse_prio(text):
    """Every discount line as a dict, in order."""
    rows = []
    for m in PRIO_RE.finditer(text):
        rows.append(dict(
            t=int(m.group(1)), pill=(int(m.group(2)), int(m.group(3))),
            lgm=(int(m.group(4)), int(m.group(5))), d=int(m.group(6)),
            cap=float(m.group(7)), raw=float(m.group(8)),
            disc=float(m.group(9))))
    return rows


def goal_at(text):
    """tick -> goal kind, from the per-tick TICK_COST line."""
    return {int(m.group(1)): m.group(2) for m in TICKGOAL_RE.finditer(text)}


def episodes(text, rows):
    """The discount's ON spans, as (first_tick, last_tick, ticks).

    The line is rate-limited to CAPTURE_LGM_PRIORITY_LOG_TICKS, so counting
    LINES understates a discount that applied once and simply held.  An
    episode runs from the first line after an `off` (or the start) to the
    `verdict=off` that closes it."""
    marks = sorted([(r["t"], "on") for r in rows]
                   + [(int(m.group(1)), "off")
                      for m in PRIO_OFF_RE.finditer(text)])
    out, start, last = [], None, None
    for t, kind in marks:
        if kind == "on":
            if start is None:
                start = t
            last = t
        elif start is not None:
            out.append((start, t, t - start))
            start, last = None, None
    if start is not None:
        out.append((start, last, last - start))
    return out


def goal_switches(goals):
    """(total capture_pill<->kill_lgm switches, span in brain ticks).

    Counted on the CHANGE, walking the TICK_COST goal series in tick order:
    a switch is any adjacent pair of DIFFERENT goals both of which are
    capture_pill or kill_lgm."""
    if not goals:
        return 0, 0
    ts = sorted(goals)
    pair = ("capture_pill", "kill_lgm")
    n, prev = 0, None
    for t in ts:
        g = goals[t]
        if prev is not None and g != prev and g in pair and prev in pair:
            n += 1
        prev = g
    return n, ts[-1] - ts[0]


def report(variant, rows, goals, eps, trace):
    caps = len(SPENT_RE.findall(trace))
    print(f"  discount lines: {len(rows)}")
    if rows:
        d = sorted({r["d"] for r in rows})
        print(f"    lgm cheb distance to the target pill: {d}")
        print(f"    cap: {sorted({r['cap'] for r in rows})}")
        cut = [r["raw"] - r["disc"] for r in rows]
        print(f"    cost cut: min={min(cut):.0f} max={max(cut):.0f} "
              f"mean={sum(cut) / len(cut):.0f}")
        tot = sum(e[2] for e in eps)
        print(f"    episodes: {len(eps)}, {tot} brain ticks discounted, "
              f"longest {max(e[2] for e in eps) if eps else 0}")
    kinds = {}
    for g in goals.values():
        kinds[g] = kinds.get(g, 0) + 1
    top = dict(sorted(kinds.items(), key=lambda kv: -kv[1])[:8])
    print(f"  goal ticks (top 8): {top}")
    sw, span = goal_switches(goals)
    rate = (sw * BRAIN_TICKS_PER_MIN / span) if span else 0.0
    print(f"  capture_pill<->kill_lgm switches: {sw} over {span} brain ticks "
          f"= {rate:.1f}/min (bound {MAX_SWITCHES_PER_MIN}/min)")
    print(f"  captures (engine trace): {caps}")
    return caps, sw, rate


def check_sweep_silent(text):
    """The sweep must never fire in this arena, in ANY variant -- that is what
    makes the priority option the only live difference between P1 and P0."""
    n = len(HUNT_ANY_RE.findall(text))
    if n:
        return [f"{n} CAPTURE_LGM_HUNT line(s) appeared. The arena is built so "
                f"the sweep cannot trigger (errand tiles are Chebyshev 6 from "
                f"the corpse, radius 2; the enemy never repairs the pill), so "
                f"P1 vs P0 would no longer isolate CAPTURE_LGM_PRIORITY."]
    return []


def check_P1(rows, goals, text, trace, eps, caps, sw, rate):
    errs = check_sweep_silent(text)
    if not rows:
        errs.append("the discount never applied: no CAPTURE_LGM_PRIO line at "
                    "all. Either the bot never took capture_pill (see the goal "
                    "histogram above), or the builder never came within "
                    f"CAPTURE_LGM_PRIORITY_RADIUS {PRIO_RADIUS} of the corpse "
                    f"{CORPSE} (check [farm_beside] lines in print2_bot1.log).")
        return errs
    # 1. THE TARGET IS THE CORPSE and the man really is inside the radius.
    bad_pill = [r["t"] for r in rows if r["pill"] != CORPSE]
    if bad_pill:
        errs.append(f"the discount named a pill other than the corpse "
                    f"{CORPSE} on ticks {bad_pill[:6]}")
    far = [(r["t"], r["d"]) for r in rows if r["d"] > PRIO_RADIUS]
    if far:
        errs.append(f"a man outside CAPTURE_LGM_PRIORITY_RADIUS "
                    f"{PRIO_RADIUS} was discounted: {far[:6]}")
    # 2. THE ARITHMETIC IS THE KNOB'S.  cost=A->B must be min(A, cap), and
    #    cap must be the constant -- this is the "hand-computable" contract the
    #    pool-grid chip carries.
    bad_cap = [(r["t"], r["cap"]) for r in rows
               if abs(r["cap"] - PRIO_CAP) > 1e-6]
    if bad_cap:
        errs.append(f"cap was not CAPTURE_LGM_PRIORITY_MAX_COST {PRIO_CAP}: "
                    f"{bad_cap[:6]}")
    bad_math = [(r["t"], r["raw"], r["disc"]) for r in rows
                if abs(r["disc"] - min(r["raw"], r["cap"])) > 1.0]
    if bad_math:
        errs.append(f"cost=A->B is not min(A, cap) on {len(bad_math)} line(s) "
                    f"(t, A, B): {bad_math[:6]}")
    # 3. THE DISCOUNT ACTUALLY WON THE GOAL.  That is the whole point: it is
    #    meant to be big enough to displace capture_pill.  "kill_lgm was the
    #    goal at some point" is too weak -- kill_lgm wins on its own merits in
    #    the control too -- so what is asserted is the SWITCH: a tick where the
    #    goal went capture_pill -> kill_lgm while the discount was live.
    on = set()
    for a, b, _ in eps:
        for t in range(a, b + 1):
            on.add(t)
    picked = [t for t, g in goals.items() if g == "kill_lgm" and t in on]
    ts = sorted(goals)
    takeovers = [t for i, t in enumerate(ts)
                 if i and goals[t] == "kill_lgm"
                 and goals[ts[i - 1]] == "capture_pill" and t in on]
    print(f"  kill_lgm brain ticks while the discount was live: {len(picked)}")
    print(f"  capture_pill -> kill_lgm takeovers while the discount was live: "
          f"{len(takeovers)}" + (f" (first at t={takeovers[0]})"
                                 if takeovers else ""))
    if not takeovers:
        errs.append("the discount never took the goal: not one capture_pill "
                    "-> kill_lgm switch happened on a tick it was live. "
                    "CAPTURE_LGM_PRIORITY_MAX_COST was not enough to displace "
                    "capture_pill (see the note at the top of this file about "
                    "IMMINENT_CAPTURE_FLOOR), so nothing about the option is "
                    "being exercised.")
    # 4. THE CAPTURE STILL HAPPENS.  "Continue regular execution": the bot has
    #    to come back to capture_pill, or take the corpse outright.
    back = [t for t, g in goals.items() if g == "capture_pill"
            and picked and t > min(picked)]
    if not back and caps < 1:
        errs.append("after the first kill_lgm the bot never returned to "
                    "capture_pill and never took a corpse -- the priority "
                    "option is supposed to interrupt the capture, not end it.")
    # 5. NO FLIP-FLOP STORM.
    if rate > MAX_SWITCHES_PER_MIN:
        errs.append(f"capture_pill<->kill_lgm switched {rate:.1f} times a "
                    f"minute (bound {MAX_SWITCHES_PER_MIN}) -- that is the "
                    "flip-flop the remembered capture target (state.cap_prio) "
                    "exists to prevent.")
    if caps < 1:
        errs.append(f"the bot took {caps} corpse(s); with the arena refilling "
                    "the corpse fifteen times it should manage at least one.")
    return errs


def check_off(variant, rows, goals, text, trace, eps, caps, sw, rate):
    errs = check_sweep_silent(text)
    if rows or PRIO_OFF_RE.search(text):
        errs.append(f"{variant} still produced {len(rows)} CAPTURE_LGM_PRIO "
                    "line(s) -- the master switch does not gate everything it "
                    "claims to.")
    if caps < 1:
        errs.append(f"the control took {caps} corpse(s) -- the arena is "
                    "broken, not the knob.")
    return errs


def main():
    args = sys.argv[1:]
    take_asap_flag(args)
    variant, ticks, build_dir = "ALL", None, DEFAULT_BUILD
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            variant = args[i + 1].upper(); i += 2
        elif args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build_dir = Path(args[i + 1]); i += 2
        else:
            print(f"unknown arg {args[i]}"); return 1
    todo = list(VARIANTS) if variant == "ALL" else [variant]
    for v in todo:
        if v not in VARIANTS:
            print(f"unknown variant {v}"); return 1

    print(pacing_line())
    rc = 0
    for v in todo:
        n = ticks or TICKS[v]
        print(f"\n=== arena {v} ({n} engine ticks, tokens: {TOKENS[v]}) ===")
        sess, text, trace = run_one(v, n, build_dir)
        if sess is None:
            print(f"FAIL ({v}): {text}")
            rc = 1
            continue
        print(f"  session: {sess.name}")
        rows = parse_prio(text)
        goals = goal_at(text)
        eps = episodes(text, rows)
        caps, sw, rate = report(v, rows, goals, eps, trace)
        if v == "P1":
            errs = check_P1(rows, goals, text, trace, eps, caps, sw, rate)
        else:
            errs = check_off(v, rows, goals, text, trace, eps, caps, sw, rate)
        if errs:
            for e in errs:
                print(f"FAIL ({v}): {e}")
            rc = 1
        else:
            print(f"  {v} OK")
    print("\nPASS" if rc == 0 else "\nFAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
