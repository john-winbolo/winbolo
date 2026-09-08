#!/usr/bin/env python3
"""Capture-pill LGM hunt test (GoalHunter constants.lua CAPTURE_LGM_HUNT).

WHAT IS BEING MEASURED
----------------------
A bot on capture_pill drives at a dead pillbox to grab it.  An ENEMY BUILDER
standing on or beside that corpse is rebuilding it out from under us: the
moment his repair lands, the free pill is a live enemy pillbox and the capture
is gone.  KEEL drives straight past him.  Two separate things stopped it doing
anything about him, and the change fixes both:

  * the TURN keys belong to navigation, so the turret is never brought onto
    the man;
  * the gunsight driver that lets init.lua's opportunistic LGM shot actually
    open is gated on goal == kill_lgm, so under capture_pill the crosshair
    sits wherever the last goal left it and the shot's impact-point gate
    (explosion within 64 wu of the lead point) essentially never opens.

The change makes capture_pill builder-aware without touching the goal:

  * a hostile LGM within CAPTURE_LGM_HUNT_RADIUS tiles (Chebyshev) OF THE
    TARGET PILL -- measured from the pill, not the tank -- puts the bot in the
    hunt.  So does the target pill's ARMOUR GOING UP, which is that same
    repair seen from outside when the man is hidden in trees;
  * the TURN keys go to the kill_lgm aim solution while it points within
    CAPTURE_LGM_HUNT_TOL_BRADS of the navigation heading (widened to
    ..._TOL_NEAR_BRADS inside ..._NEAR_TILES of the pill), and back to
    navigation the moment it does not;
  * the THROTTLE is never touched.  steering.lua writes `turn_corr` and never
    `correction`, and every throttle branch reads `correction`, so "we never
    brake for the builder" is structural.  The decision line carries the
    throttle branch name (thr=) so the runs can show it too;
  * the goal stays capture_pill: no substate, no target_id, no cost change.

THREE ARENAS (tests/generate_capture_lgm_hunt_map.py -- read its docstring for
the geometry and for why the enemy is a scripted brain and parks where it does)

  H1  THE HUNT.  Enemy builder parked beside the corpse, forever sent to a
      tile Chebyshev 2 from it.
      PASS: the hunt engages repeatedly with verdict=turn; the aim is only
      ever taken when it is inside the tolerance the line itself reports; the
      bot FIRES at the man at least once from capture_pill (verdict=fire,
      gate=shooting); the goal is capture_pill on every hunt tick; and no hunt
      tick is ever driven by the kill_lgm halt branch.

  H0  THE CONTROL.  Identical arena and identical enemy, p0 run with
      cfg=CAPTURE_LGM_HUNT=false -- the ONE knob, so nothing else can explain
      the difference.
      PASS: not one hunt line, and the bot still gets its captures.

  H2  THE ARMOUR TRIGGER.  One tank, nobody to see.  The sidecar pulses the
      corpse's armour 0 -> 2 -> 0, a repair landing with the builder invisible.
      PASS: the hunt engages on src=armour, only inside a pulse's window, and
      holds for a real span rather than a tick or two.

      This arena is the one that found the trigger's first shape to be
      unreachable.  Watching the armour from inside the hunt could never work:
      the goal pool drops capture_pill on the VERY TICK the corpse comes back
      to life (brain t=107 read armour=2 with the goal already `none`), so a
      sampler gated on goal == capture_pill never sees the rise.  The trigger
      now reads world.lua's repair tell (p._repair_tick), which is stamped
      every tick whatever the goal is -- and the window then means the right
      thing: still hot when the pill is back at 0 and capture_pill wins the
      pool again, which is the moment the bot arrives on a tile somebody's man
      was working on seconds ago.

WHY "AT LEAST ONCE" AND NOT "EVERY PASS".  The tank does not stop -- that is
the point of the feature -- so the crosshair has to arrive on a moving lead
point while the range closes, and the gunsight only steps in half-tiles.  A
connect per pass is not the promise; being able to connect at all, from
capture_pill, is.

Usage: python capture_lgm_hunt_test.py [--variant H0|H1|H2|ALL] [--ticks N]
                                       [--build DIR]
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
from generate_capture_lgm_hunt_map import (          # noqa: E402
    CORPSE, ROAD_TILE, FARM_TILE, P1_PARK, SPAWN0, HUNT_RADIUS,
    HUNT_NEAR_TILES, HUNT_TOL_BRADS, HUNT_TOL_NEAR_BRADS, nbots, cheb)

VARIANTS = ("H0", "H1", "H2")
PORTS = {"H0": 50570, "H1": 50571, "H2": 50572}
# ENGINE ticks.  The brain thinks once per frame and the sim advances two
# engine ticks per frame, so these are half as many brain ticks.  H1 needs the
# length: each capture is one approach, the arena refills the corpse fourteen
# times, and the enemy's man is only standing in the hunt box for part of each
# of his round trips.
TICKS = {"H0": 24000, "H1": 24000, "H2": 12000}

ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h

# ── the -bot-init tokens ─────────────────────────────────────────────────
# WHY EACH PIN IS THERE.  All three are about keeping the bot ON the errand
# that is being measured; none of them touches the hunt.
#   TANK_COMBAT_ENABLED=false     the scripted enemy is a visible hostile tank
#     that never fires.  Without this our bot takes attack_tank and chases it,
#     and the capture -- the only thing this test is about -- stops happening.
#   BUILDER_POOL_ENABLED=false    keeps our own man in the tank.  His errands
#     are irrelevant here and a stranded man of our own adds rescue_lgm and
#     wait_for_lgm noise on top of the measurement.
#   STRATEGIC_PLACE_ENABLED=false measured: with it on, the bot planted every
#     pill it captured -- once at (126,127), ONE TILE from the corpse -- and a
#     live friendly pillbox in the shell's lane is a hard LOS block for the
#     kill-LGM shot, so the fire half could never open.  With it off the bot
#     simply keeps what it grabs.
COMMON = ("cfg=TANK_COMBAT_ENABLED=false"
          ";cfg=BUILDER_POOL_ENABLED=false"
          ";cfg=STRATEGIC_PLACE_ENABLED=false")
# The control differs by ONE knob.  preset=keel would also do it (the master
# switch is in keel), but it drags in every other keel value and then a null
# result could be blamed on any of them.
CONTROL = "cfg=CAPTURE_LGM_HUNT=false;" + COMMON

TOKENS = {"H0": CONTROL, "H1": COMMON, "H2": COMMON}

# init.lua, once per change of VERDICT or of the kill-LGM fire gate:
#   CAPTURE_LGM_HUNT t=310 pill=(126,126) src=lgm lgm=(128,125) pd=2 d=6.9
#     nav=68 aim=51 err=-17 tol=26 uturn=0 gun=14 spd=48 slower=0 thr=plow_through
#     steer=turn gate=shooting verdict=fire
# steer= is what the BLEND did with the turn keys; verdict= is the loudest
# thing that happened.  They are different questions: the kill-LGM block fires
# at any man in range whatever we are steering, so steer=nav with verdict=fire
# is a real and correct combination (seen at H1 t=2153, aim 119 bradians off
# the nav heading with a U-turn latched -- the turn stayed navigation's and the
# shot still went out).  The tolerance assertion below is about steer=, never
# verdict=.
HUNT_RE = re.compile(
    r"CAPTURE_LGM_HUNT t=(\d+) pill=\((\d+),(\d+)\) src=(\w+) "
    r"lgm=\((\S+),(\S+)\) pd=(\S+) d=([\d.]+) nav=(\S+) aim=(-?\d+) "
    r"err=(\S+) tol=(\d+) uturn=(\d) gun=(\d+) spd=(-?\d+) slower=(\d) "
    r"thr=(\S+) steer=(\w+) gate=(\S+) verdict=(\w+)")
OFF_RE = re.compile(r"CAPTURE_LGM_HUNT t=(\d+) verdict=off")
# init.lua: GOAL_CHANGE Goal: capture_pill #0 (126,126)
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)(?: #(-?\d+))? \((\d+),(\d+)\)")
# init.lua per-tick engine dump -- the goal the brain was actually on.
TICKGOAL_RE = re.compile(r"TICK_COST t=(\d+) .*? goal=(\w+)")
# The sidecar's own trace (engine truth), in SIM ticks.
PULSE_RE = re.compile(r"^# pulse (\d+) n=(\d+) armour=(\d+)", re.M)
PULSE_END_RE = re.compile(r"^# pulse_end (\d+) armour=(\d+) (\w+)", re.M)
SPENT_RE = re.compile(r"^# spent (\d+) n=(\d+)", re.M)


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
                    str(HERE / "generate_capture_lgm_hunt_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"clh_{variant}"
    stderr = HERE / f"capture_lgm_hunt_{variant}_stderr.txt"
    trace = build_dir / f"capture_lgm_hunt_{variant}_trace.log"
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
    init = f"0={BRAIN}[{tok}]"
    if n > 1:
        # The enemy is a scripted brain, not a GoalHunter -- see
        # tests/brains/farm_beside.lua for why.
        init += f",1={ENEMY}[]"
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"capture_lgm_hunt_{variant}.map"),
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


def parse_hunt(text):
    """Every hunt decision line as a dict, in order."""
    rows = []
    for m in HUNT_RE.finditer(text):
        rows.append(dict(
            t=int(m.group(1)), pill=(int(m.group(2)), int(m.group(3))),
            src=m.group(4), lgm=(m.group(5), m.group(6)), pd=m.group(7),
            d=float(m.group(8)), nav=m.group(9), aim=int(m.group(10)),
            err=m.group(11), tol=int(m.group(12)), uturn=int(m.group(13)),
            gun=int(m.group(14)), spd=int(m.group(15)),
            slower=int(m.group(16)), thr=m.group(17), steer=m.group(18),
            gate=m.group(19), verdict=m.group(20)))
    return rows


def goal_at(text):
    """tick -> goal kind, from the per-tick TICK_COST line."""
    return {int(m.group(1)): m.group(2) for m in TICKGOAL_RE.finditer(text)}


def episodes(text, rows):
    """The hunt's ON spans, as (first_tick, last_tick, ticks).

    The decision line is rate-limited to changes of verdict/gate, so counting
    LINES badly understates a hunt that engaged once and simply held -- which
    is exactly what the armour trigger does.  An episode runs from the first
    line after an `off` (or the start) to the `verdict=off` that closes it."""
    marks = sorted([(r["t"], "on") for r in rows]
                   + [(int(m.group(1)), "off") for m in OFF_RE.finditer(text)])
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


def check_common(variant, rows, goals):
    """Assertions that hold for ANY arena that produced hunt lines."""
    errs = []
    # 1. THE GOAL NEVER MOVED.  This is the promise that the blend is steering
    #    only: capture_pill with no substate churn.
    bad_goal = [(r["t"], goals.get(r["t"])) for r in rows
                if goals.get(r["t"]) not in (None, "capture_pill")]
    if bad_goal:
        errs.append(f"the goal was not capture_pill on {len(bad_goal)} hunt "
                    f"tick(s) -- the hunt must never change the goal: "
                    f"{bad_goal[:6]}")
    # 2. THE TARGET IS THE GOAL'S PILL and the man really is inside the box.
    bad_pill = [r["t"] for r in rows if r["pill"] != CORPSE]
    if bad_pill:
        errs.append(f"the hunt named a pill other than the corpse {CORPSE} on "
                    f"ticks {bad_pill[:6]}")
    bad_pd = [(r["t"], r["pd"]) for r in rows
              if r["src"] == "lgm" and int(r["pd"]) > HUNT_RADIUS]
    if bad_pd:
        errs.append(f"a man outside CAPTURE_LGM_HUNT_RADIUS {HUNT_RADIUS} "
                    f"triggered the hunt: {bad_pd[:6]}")
    # 3. THE TOLERANCE IS THE ONE THE KNOBS SAY, and the aim is only ever
    #    taken inside it.
    for r in rows:
        want = HUNT_TOL_NEAR_BRADS if r["tol"] == HUNT_TOL_NEAR_BRADS \
            else HUNT_TOL_BRADS
        if r["tol"] not in (HUNT_TOL_BRADS, HUNT_TOL_NEAR_BRADS):
            errs.append(f"t={r['t']}: tol={r['tol']} is neither "
                        f"CAPTURE_LGM_HUNT_TOL_BRADS ({HUNT_TOL_BRADS}) nor "
                        f"..._TOL_NEAR_BRADS ({HUNT_TOL_NEAR_BRADS})")
            break
    outside = [(r["t"], r["err"], r["tol"]) for r in rows
               if r["steer"] == "turn" and r["err"] != "nil"
               and abs(int(r["err"])) > r["tol"]]
    if outside:
        errs.append(f"the aim took the turn keys while OUTSIDE its tolerance "
                    f"on {len(outside)} tick(s) -- (t, err, tol): "
                    f"{outside[:6]}")
    # ...and a latched U-turn must always give the turn back to navigation.
    latched = [r["t"] for r in rows if r["steer"] == "turn" and r["uturn"]]
    if latched:
        errs.append(f"the aim took the turn keys on {len(latched)} tick(s) "
                    f"with a U-turn latched: {latched[:6]}")
    # 4. THE THROTTLE WAS NEVER THE HUNT'S.  Every tick has to be driven by one
    #    of navigation's own branches; kill_lgm_halt is the brake the kill_lgm
    #    GOAL applies when it stops to shoot, and the hunt must never reach it.
    halted = [(r["t"], r["thr"]) for r in rows if r["thr"] == "kill_lgm_halt"]
    if halted:
        errs.append(f"the tank was on the kill_lgm halt branch during the hunt "
                    f"-- the capture must never brake for the builder: "
                    f"{halted[:6]}")
    return errs


def report_rows(rows, goals, eps):
    verdicts = {}
    steers = {}
    gates = {}
    thrs = {}
    for r in rows:
        verdicts[r["verdict"]] = verdicts.get(r["verdict"], 0) + 1
        steers[r["steer"]] = steers.get(r["steer"], 0) + 1
        gates[r["gate"]] = gates.get(r["gate"], 0) + 1
        thrs[r["thr"]] = thrs.get(r["thr"], 0) + 1
    print(f"  hunt lines: {len(rows)}")
    print(f"    steering: {dict(sorted(steers.items()))}")
    print(f"    verdicts: {dict(sorted(verdicts.items()))}")
    print(f"    fire gate: {dict(sorted(gates.items()))}")
    print(f"    throttle branch: {dict(sorted(thrs.items()))}")
    if rows:
        print(f"    src: {dict(sorted({r['src']: sum(1 for q in rows if q['src'] == r['src']) for r in rows}.items()))}")
        spd = [r["spd"] for r in rows]
        print(f"    tank speed on hunt ticks: min={min(spd)} max={max(spd)} "
              f"mean={sum(spd) / len(spd):.0f}")
        tot = sum(e[2] for e in eps)
        print(f"    episodes: {len(eps)}, {tot} brain ticks hunting, "
              f"longest {max(e[2] for e in eps) if eps else 0}")


def check_H1(rows, goals, text, trace, eps):
    errs = check_common("H1", rows, goals)
    turns = [r for r in rows if r["steer"] == "turn"]
    fires = [r for r in rows if r["verdict"] == "fire"]
    lgm_rows = [r for r in rows if r["src"] == "lgm"]
    if not lgm_rows:
        errs.append("the hunt never saw a hostile LGM at all. Either the "
                    "scripted enemy never sent its man (check "
                    f"[farm_beside] lines / man_status in print2_bot1.log) or "
                    f"it stopped outside the {HUNT_RADIUS}-tile box around "
                    f"{CORPSE}.")
    if len(turns) < 5:
        errs.append(f"the aim took the turn keys only {len(turns)} time(s); "
                    "expected at least 5 engagements over the run.")
    if not fires:
        errs.append("the bot never fired at the builder from capture_pill "
                    "(no verdict=fire / gate=shooting). The gate histogram "
                    "above says why it stopped short: out_of_range = never got "
                    "within KILL_LGM_SHOOT_RANGE; off_aim = the hull never "
                    "came round far enough; gunrange_off = the crosshair never "
                    "landed within 64 wu of the lead point; ready_busy = "
                    "something else had already claimed KEY_SHOOT that tick "
                    "(check the enemy tank is off the firing line).")
    for r in fires:
        if r["gate"] != "shooting":
            errs.append(f"t={r['t']}: verdict=fire with gate={r['gate']} -- a "
                        "fire verdict must come from the kill-LGM block's own "
                        "'shooting' status.")
    caps = len(SPENT_RE.findall(trace))
    print(f"  captures (engine trace): {caps}")
    if caps < 3:
        errs.append(f"the bot only took {caps} corpse(s) -- the arena did not "
                    "produce enough approaches to measure anything.")
    return errs


def check_H0(rows, goals, text, trace, eps):
    errs = []
    if rows or OFF_RE.search(text):
        errs.append(f"cfg=CAPTURE_LGM_HUNT=false still produced "
                    f"{len(rows)} hunt line(s) -- the master switch does not "
                    "gate everything it claims to.")
    caps = len(SPENT_RE.findall(trace))
    print(f"  captures (engine trace): {caps}")
    if caps < 3:
        errs.append(f"the control only took {caps} corpse(s) -- the arena is "
                    "broken, not the knob.")
    return errs


def check_H2(rows, goals, text, trace, eps):
    errs = check_common("H2", rows, goals)
    pulses = [int(m.group(1)) for m in PULSE_RE.finditer(trace)]
    ends = PULSE_END_RE.findall(trace)
    print(f"  armour pulses (engine trace): {len(pulses)}"
          + (f", first at sim t={pulses[0]}" if pulses else ""))
    shot_down = [e for e in ends if e[2] == "shot_down"]
    print(f"  pulses the bot shelled back to 0 itself: {len(shot_down)}"
          f" of {len(ends)} completed")
    if not pulses:
        errs.append("the sidecar never pulsed the corpse's armour -- nothing "
                    "was offered to the trigger.")
    arm = [r for r in rows if r["src"] == "armour"]
    if not arm:
        errs.append("the armour trigger never fired. With no LGM visible the "
                    "hunt must engage on the pill's armour rising alone "
                    "(CAPTURE_LGM_HUNT_ARMOUR_TRIGGER).")
    held = sum(e[2] for e in eps)
    if held < 200:
        errs.append(f"the armour-triggered hunt only held for {held} brain "
                    "tick(s) across the run; the pulses are dense enough that "
                    "CAPTURE_LGM_HUNT_ARMOUR_TICKS should keep it hot far "
                    "longer than that.")
    lgm = [r for r in rows if r["src"] == "lgm"]
    if lgm:
        errs.append(f"H2 has no enemy at all, yet {len(lgm)} hunt line(s) "
                    "claim src=lgm -- the LGM scan is seeing something it "
                    "should not.")
    # Every armour-triggered tick must sit inside the window a real pulse
    # opened: ARMOUR_TICKS brain ticks after a rise. The trace is in SIM ticks
    # and the brain thinks once per two of them.
    if pulses and arm:
        from generate_capture_lgm_hunt_map import HUNT_ARMOUR_TICKS
        brain_pulses = [p // 2 for p in pulses]
        stray = [r["t"] for r in arm
                 # The brain reads the new armour a couple of ticks after the
                 # sim writes it (measured: sim 210 -> brain 107, i.e. 105+2),
                 # so allow a little slack on top of the window.
                 if not any(0 <= r["t"] - bp <= HUNT_ARMOUR_TICKS + 10
                            for bp in brain_pulses)]
        if stray:
            errs.append(f"{len(stray)} armour-triggered hunt tick(s) fall "
                        f"outside every pulse's {HUNT_ARMOUR_TICKS}-tick "
                        f"window: {stray[:6]} (pulses at brain t="
                        f"{brain_pulses[:6]})")
    return errs


CHECKS = {"H0": check_H0, "H1": check_H1, "H2": check_H2}


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
        print(f"\n=== arena {v} ({n} engine ticks) ===")
        sess, text, trace = run_one(v, n, build_dir)
        if sess is None:
            print(f"FAIL ({v}): {text}")
            rc = 1
            continue
        print(f"  session: {sess.name}")
        rows = parse_hunt(text)
        goals = goal_at(text)
        eps = episodes(text, rows)
        report_rows(rows, goals, eps)
        errs = CHECKS[v](rows, goals, text, trace, eps)
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
