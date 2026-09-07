#!/usr/bin/env python3
"""farm sectors, and the LGM's walk HOME (GoalHunter 1.7 builder pool).

TWO CHANGES, FOUR RUNS, TWO CLAIMS AND TWO CONTROLS
---------------------------------------------------

1. BUILDER_POOL_FARM_SECTORS (default 4).  Farm discovery used to offer exactly
   ONE row -- the nearest forest anywhere inside BUILDER_POOL_LEASH.  The
   engine's LGM does not pathfind: brain_pathfinder.c lgmTravelTicksCore
   replays a straight line with a crude slide and reports stuck.  So "nearest"
   is regularly a forest the man cannot reach at all, and a clear forest a tile
   further out in another direction was never a candidate.  Discovery now
   splits the leash square into four 90-degree wedges (N/E/S/W, boundaries on
   the 45-degree diagonals, the diagonal itself going to the vertical wedge)
   and offers the nearest forest in EACH.  They compete on the existing farm
   score -- no new term.  FARM_SECTORS=1 restores the single row.

2. BUILDER_POOL_RETURN_PREDICT (default true).  M.lgm_trip charged 2 x outbound
   for the round trip: the walk home mirrored the walk out, as if the tank had
   waited on the spot.  It now marches the tank along ITS OWN COMMITTED ROUTE
   (state.pf.path_chain, at the engine's per-terrain speed caps) for
   out + LGM_BUILD_TIME brain ticks and walks the man back to the tile it lands
   on, so trip = out + build + back.  RETURN_PREDICT=false is the old number.

  A   THE WALL.  A tank with nothing to bid on, a forest TWO tiles west behind
      a full-height moat of DEEP SEA, and a clear forest THREE tiles east.  The
      near one is nearest and unreachable (the man cannot cross water and the
      tank cannot shoot it away -- a BUILDING wall was tried first and the bot
      simply shot a hole in it); the far one is in a different wedge.  The man
      must be dispatched EAST, and the pool must have had 2+ candidates on the
      tick it decided -- "it picked the open one" means nothing unless the
      walled one was on the table at the same moment.

      The tank still WANDERS (it explores; pricing EXPLORE_BASE_COST out does
      not stop it, because explore is the pool's fallback rather than a scored
      candidate), so "the walled forest is the nearest" is only true for a
      stretch of the run.  The sidecar logs every tile the tank stands on and
      both A checks are made strictly inside the WINDOW where it really was.

  A2  THE CONTROL (cfg=BUILDER_POOL_FARM_SECTORS=1).  IT IS EXPECTED NOT TO
      DISPATCH.  With one wedge the only farm row is the nearest forest -- the
      walled one -- so the pool has nothing it can send the man to.  The
      assertion is that NO farm BP_DISPATCH happens anywhere in that window,
      that the pool DID offer the walled row (a BP_DENY naming it, so the
      silence is a refusal and not an empty pool), and that the open forest was
      never offered at all.  The absence IS the result; this arena therefore
      has neither of the "the man came out" checks the others carry, and its
      PASS line states that rather than letting the silence look like an
      oversight.

  B   THE DRIVE.  A tank driving east along a road to take a NEUTRAL BASE
      twenty tiles away -- which is what gives it the ROUTE the prediction
      walks.  (A base and not a pill: capture_pill puts the builder in
      "gather" mode and the pool refuses every row under it with
      `mode_owned (gather)`; capture_base is "opportunistic".)  The
      sidecar plants two forests when it reaches a chosen column, one three
      tiles behind and one three tiles ahead, both one row south: SAME outbound
      leg, so only the walk home can separate them.  The man must go to the
      AHEAD one, its back{} must be shorter than the behind row's would be, the
      predicted tile must lie ON the printed route, and trip = out + build +
      back must close from the chips alone.

  B2  THE CONTROL (cfg=BUILDER_POOL_RETURN_PREDICT=false).  Same ground, same
      plant.  back must equal out, pred must read `same`, predsrc must read
      `off`, and the dispatch must go to the BEHIND forest -- the two rows now
      score identically and order_rows breaks the tie on the lower tile key,
      which is the western one.  That is the wrong answer, and printing it is
      what makes B's answer evidence about the change rather than about the
      arena.

EVERY NUMBER IS READ OFF THE PRINTED CHIPS.  The standing rule for these lines
is that every factor in the score is on the line, so a reader can hand-check it
without opening constants.lua.  This file therefore re-derives, for every
dispatch it asserts on:
    value - trip_w x trip            == bp_score
    out + build + back               == trip
and refuses a line whose arithmetic does not close, because a line that does
not close is a line nobody can check.

Usage: python farm_sectors_test.py [--variant A|A2|B|B2|all] [--ticks N]
                                   [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"

sys.path.insert(0, str(HERE))
import generate_farm_sectors_map as G   # noqa: E402

PORTS = {"A": 50281, "A2": 50282, "B": 50283, "B2": 50284}
# ENGINE ticks. The brain thinks once per 20 ms frame and the sim advances two
# engine ticks per frame, so these are ~half as many brain ticks. Arena A has
# to wait out its PLANT_TICK before anything can happen at all.
TICKS = {"A": 2000, "A2": 2000, "B": 3000, "B2": 3000}

ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h

# Discovery offers no farm row at or above TREE_OPPORTUNISTIC_MAX, and a
# scripted map hands out 40 trees whatever -gametype says; and at 40 trees the
# urgency term is 0, so the row is worth a flat VALUE_FARM (15) and can never
# clear MIN_SCORE. 99 is what a 5-tree woodpile would produce
# (15 + 12 x (12 - 5)) -- the same stand-in repair_priority_test.py uses.
FARM_ON = "cfg=TREE_OPPORTUNISTIC_MAX=41;cfg=BUILDER_POOL_VALUE_FARM=99"
# A and A2 differ by ONE token, and so do B and B2: that is what makes each
# pair a control rather than two separate experiments.
#
# There is deliberately NO "stop the tank" token. cfg=EXPLORE_BASE_COST=1e30
# was tried and does nothing -- explore is the goal pool's FALLBACK, not a
# scored candidate, so pricing it out leaves it winning anyway (measured: the
# tank still crawled four tiles in the first 800 ticks). The arenas handle the
# drift instead: the sidecar logs every tile the tank stands on, and the
# arena-A checks only make their claim inside the WINDOW during which the
# walled forest really was the nearer of the two.
TOKENS = {
    "A":  FARM_ON,
    "A2": ";".join([FARM_ON, "cfg=BUILDER_POOL_FARM_SECTORS=1"]),
    "B":  FARM_ON,
    "B2": ";".join([FARM_ON, "cfg=BUILDER_POOL_RETURN_PREDICT=false"]),
}
GAMETYPE = {v: "open" for v in TOKENS}

# ── print2 lines (builder_pool.lua) ───────────────────────────────────────
# BP_DISPATCH t=453 job=farm target=(129,127) score=52 (bp_score{52} = bp_base{99}
#   + urg{0} + front{0} = value{99} - trip_w{0.50} x trip{94t} = tripcost{47}
#   - danger_w{1.50} x bp_danger{0} = dangercost{0} [legs out{51} + build{20}
#   + back{23} pred{130,126} predsrc{route} wedge{E}]) eta=51 trip=94 trees=40-0
#   front=12 owner=.. claim=.. out=51 build=20 back=23 pred=(130,126)/route wedge=E
DISP_RE = re.compile(
    r"BP_DISPATCH t=(\d+) job=(\S+) target=\((\d+),(\d+)\).*? score=(-?[\d.]+) "
    r"\((.*?)\)(?: \[linear\])? "
    r"eta=(\S+) trip=(\S+) trees=(\d+)-(\d+) front=(-?\d+)")
LEGS_RE = re.compile(
    r"out=(\S+) build=(\S+) back=(\S+) pred=(\S+)(?: wedge=(\S+))?")
# The farm row's chip line, in full: every factor in the score, in order.
FARM_TERMS_RE = re.compile(
    r"bp_score\{(-?[\d.]+)\} = bp_base\{(-?[\d.]+)\} \+ urg\{(-?[\d.]+)\} \+ "
    r"front\{(-?[\d.]+)\} = value\{(-?[\d.]+)\} - trip_w\{([\d.]+)\} x "
    r"trip\{(\d+)t\} = tripcost\{(-?[\d.]+)\} - danger_w\{([\d.]+)\} x "
    r"bp_danger\{(-?[\d.]+)\} = dangercost\{(-?[\d.]+)\}")
LEG_CHIPS_RE = re.compile(
    r"\[legs out\{(\S+?)\} \+ build\{(\S+?)\} \+ back\{(\S+?)\} "
    r"pred\{([^}]*)\} predsrc\{(\S+?)\}(?: wedge\{(\S+?)\})?\]")
DENY_RE = re.compile(
    r"BP_DENY t=(\d+) job=(\S+) target=\((\d+),(\d+)\) reason=(.*?) elig=(.*?) "
    r"score=(-?[\d.]+) trip=(\S+)")
POOL_RE = re.compile(
    r"BUILDER_POOL t=(\d+) owner=(\S+) elig=(.*?) cands=(\d+) ok=(\d+) ")
PRED_RE = re.compile(
    r"BP_PRED t=(\d+) job=(\S+) target=\((\d+),(\d+)\) src=(\S+) "
    r"horizon=(\S+) pred=(\S+) wp=(\d+)/(\d+) route=(\S+)")
WP_RE = re.compile(r"\((\d+),(\d+)\)@(-?[\d.]+)")

LGM_INTANK, LGM_DEAD, LGM_MOVING = 0, 1, 2      # constants.lua 223-225
MAN_OUT_GRACE = 40


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def read_trace(build_dir, variant):
    """The sidecar's engine-side view: TANK and PLANT rows, in SIM ticks.

      TANK  tick mx my                        every tile the tank stood on
      PLANT tick mx my <geometry...>          what was written, and where
    """
    path = build_dir / f"farm_sectors_{variant}_trace.log"
    tank, plant = [], None
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if p[0] == "TANK" and len(p) >= 4:
                tank.append(tuple(int(v) for v in p[1:4]))
            elif p[0] == "PLANT":
                plant = tuple(int(v) for v in p[1:])
    return tank, plant


def read_jsonl(build_dir, sess):
    """The brain's per-tick rows (logger.lua log_tick), as dicts.

    NOT under the debug session directory: _G.DEBUG_SESSION_DIR is set by
    BrainTest and nothing else, so headless WinBoloDS writes
    <cwd>/player0_<stamp>.jsonl straight into build/. The session directory is
    <the same stamp>_<n>_<label>, so the two are paired on the stamp.
    Fields used: t (brain tick -- the SAME clock as BP_DISPATCH's t=),
    lgm (info.man_status), lx/ly (man world position; tile = >>8)."""
    stamp = "_".join(sess.name.split("_")[:2])
    cand = build_dir / f"player0_{stamp}.jsonl"
    if not cand.exists():
        floor = os.path.getmtime(sess) - 5
        pool = [p for p in build_dir.glob("player0_*.jsonl")
                if os.path.getmtime(p) >= floor]
        if not pool:
            return []
        cand = max(pool, key=os.path.getmtime)
    rows = []
    for line in cand.read_text(errors="ignore").splitlines():
        if not line.startswith('{"type":"tick"'):
            continue
        try:
            rows.append(json.loads(line))
        except ValueError:
            pass
    return rows


def man_walk(rows, disp_tick, target):
    """What the LGM did after a BP_DISPATCH at brain tick `disp_tick`, or None
    if he never left the tank within MAN_OUT_GRACE ticks of it."""
    out_t = None
    for r in rows:
        if disp_tick <= r["t"] <= disp_tick + MAN_OUT_GRACE \
           and r["lgm"] == LGM_MOVING:
            out_t = r["t"]
            break
    if out_t is None:
        return None
    w = {"out_t": out_t, "out_tile": None, "start_d": None, "arrive_t": None,
         "closest": None, "closest_t": None, "died_t": None, "home_t": None}
    for r in rows:
        if r["t"] < out_t:
            continue
        if r["lgm"] == LGM_DEAD:
            w["died_t"] = r["t"]
            break
        if r["lgm"] == LGM_INTANK:
            w["home_t"] = r["t"]
            break
        tile = (r["lx"] >> 8, r["ly"] >> 8)
        d = abs(tile[0] - target[0]) + abs(tile[1] - target[1])
        if w["out_tile"] is None:
            w["out_tile"], w["start_d"] = tile, d
        if w["closest"] is None or d < w["closest"]:
            w["closest"], w["closest_t"] = d, r["t"]
        if d <= 1 and w["arrive_t"] is None:
            w["arrive_t"] = r["t"]
    return w


def check_man_out(step, rows, disp_tick, target, require_arrival=True):
    """MAN OUT: the man physically left the tank and walked AT the forest.

    A BP_DISPATCH line is the pool's decision, not an event in the world, so
    every arena that claims a dispatch also has to show the man leaving."""
    if not rows:
        print(f"FAIL ({step}): no per-tick jsonl for this run, so 'the man came "
              f"out' cannot be checked. Expected build/player0_<stamp>.jsonl "
              f"next to the session directory (see read_jsonl).")
        return 1
    w = man_walk(rows, disp_tick, target)
    if w is None:
        near = [(r["t"], r["lgm"]) for r in rows
                if disp_tick <= r["t"] <= disp_tick + MAN_OUT_GRACE]
        print(f"FAIL ({step}): BP_DISPATCH printed at t={disp_tick}, but "
              f"man_status never left LGM_INTANK in the next {MAN_OUT_GRACE} "
              f"brain ticks -- the pool decided and NO MAN CAME OUT. "
              f"(t,lgm) around it: {near[:12]}")
        return 1
    if w["closest"] is None:
        print(f"FAIL ({step}): the man left at t={w['out_t']} but the jsonl "
              f"logged no position for him while he was out.")
        return 1
    if w["closest"] >= w["start_d"]:
        print(f"FAIL ({step}): the man left at t={w['out_t']} from "
              f"{w['out_tile']} ({w['start_d']} tiles off {target}) and never "
              f"got closer than {w['closest']} -- he went OUT, but not at this "
              f"forest.")
        return 1
    if require_arrival and w["arrive_t"] is None:
        print(f"FAIL ({step}): the man left at t={w['out_t']} and closed from "
              f"{w['start_d']} tiles to {w['closest']} at t={w['closest_t']}, "
              f"but never stood on {target} or beside it"
              + (f" -- he died at t={w['died_t']}." if w["died_t"] else
                 f" -- back in the tank at t={w['home_t']}."
                 if w["home_t"] else " -- still out when the run ended."))
        return 1
    tail = (f", arrived t={w['arrive_t']}" if w["arrive_t"] is not None
            else f", closest {w['closest']} tile(s) at t={w['closest_t']}")
    print(f"  {step} OK: MAN OUT -- dispatch t={disp_tick}, man_status left the "
          f"tank at t={w['out_t']} from {w['out_tile']} ({w['start_d']} tiles "
          f"off {target}){tail}")
    return 0


def farm_terms(terms):
    """(score, base, urg, front, value, trip_w, trip, tripcost) off a farm
    BP_DISPATCH breakdown, or None -- and only if the arithmetic CLOSES.

    The standing rule for these lines is that every factor is on the line, so a
    reader can hand-check the score without opening constants.lua. If the
    products do not close, the line is lying and everything below is reading a
    number nobody can reproduce."""
    m = FARM_TERMS_RE.search(terms)
    if not m:
        return None
    (score, base, urg, front, value, trip_w, trip, tripcost,
     danger_w, danger, dangercost) = (
        float(m.group(1)), float(m.group(2)), float(m.group(3)),
        float(m.group(4)), float(m.group(5)), float(m.group(6)),
        int(m.group(7)), float(m.group(8)), float(m.group(9)),
        float(m.group(10)), float(m.group(11)))
    # TOLERANCES. Every chip is printed with %.0f, so each one carries up to
    # half a point of rounding and a sum of three of them can be a point and a
    # half off the sum of the unrounded values it was built from -- measured
    # here as trip_w 0.50 x trip 93 = 46.5 printing as tripcost{46} beside a
    # score of 99 - 46.5 = 52.5 printing as bp_score{52}. The rule these lines
    # obey is that a READER can reproduce the number from the line, not that
    # the printed decimals are exact; so each check allows half a point per
    # rounded value it involves, and nothing more. trip_w itself is printed to
    # two places, hence the extra 0.005 per trip tick.
    if abs(base + urg + front - value) > 2.01:            # 4 rounded values
        return None
    if abs(trip_w * trip - tripcost) > 0.51 + 0.005 * trip:
        return None
    if abs(danger_w * danger - dangercost) > 1.01:
        return None
    if abs(score - (value - tripcost - dangercost)) > 2.01:
        return None
    return score, base, urg, front, value, trip_w, trip, tripcost


def leg_chips(terms):
    """(out, build, back, pred, predsrc, wedge) off the [legs ...] chips, or
    None. `out` / `build` / `back` come back as ints; pred is the raw string
    ("same" or "x,y")."""
    m = LEG_CHIPS_RE.search(terms)
    if not m:
        return None
    def num(s):
        try:
            return int(s)
        except ValueError:
            return None
    return (num(m.group(1)), num(m.group(2)), num(m.group(3)),
            m.group(4), m.group(5), m.group(6))


def run_sim(variant, ticks, build_dir):
    """Runs the arena; returns (session_dir, our_log_text) or (None, msg)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"
    subprocess.run([sys.executable,
                    str(HERE / "generate_farm_sectors_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"farmsect_{variant}"
    final = HERE / f"farm_sectors_{variant}_final.json"
    stderr = HERE / f"farm_sectors_{variant}_stderr.txt"
    trace = build_dir / f"farm_sectors_{variant}_trace.log"
    for p in (final, stderr, trace):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                return None, (f"{p.name} is locked -- a previous WinBoloDS run "
                              f"is still going. Wait for it to exit, then retry.")

    tokens = TOKENS[variant]
    if len(tokens) > ARG_MAX:
        return None, (f"the -bot-init token string is {len(tokens)} bytes, over "
                      f"the {ARG_MAX}-byte BRAIN_INIT_ARG limit -- it would be "
                      f"truncated mid-token and the last cfg= silently ignored:"
                      f"\n  {tokens}")
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"farm_sectors_{variant}.map"),
           "-port", str(PORTS[variant]), "-nolobby",
           "-gametype", GAMETYPE[variant],
           "-bots", "1", "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN}[{tokens}]",
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(600, ticks // 4))

    sess = newest_session(build_dir, label)
    if not sess:
        return None, ("no debug session produced (is the cwd on a drive with "
                      ">50 GB free? -brain-debug silently records nothing "
                      "otherwise)")
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        return None, ("brain crashed -- " + str(crashes[0]) + "\n"
                      + Path(crashes[0]).read_text(errors="ignore")[:2000])
    ours = sess / "print2_bot0.log"
    if not ours.exists():
        return None, f"no print2_bot0.log under {sess}"
    return sess, ours.read_text(errors="ignore")


def lua_errors(text):
    return [ln for ln in text.splitlines()
            if "attempt to " in ln or "stack traceback" in ln
            or (".lua:" in ln and "Error" in ln)][:5]


def multi_candidate_ticks(text):
    """Ticks on which the pool had more than one row to choose between."""
    return {int(t): int(c) for (t, _o, _e, c, _ok) in POOL_RE.findall(text)
            if int(c) >= 2}


def dumps(text, needle, n=6, tail=False):
    lines = [l.strip() for l in text.splitlines() if needle in l]
    for ln in (lines[-n:] if tail else lines[:n]):
        print("   " + ln)


def farm_dispatches(text):
    return [d for d in DISP_RE.findall(text) if d[1] == "farm"]


# ── the walled-nearest window ────────────────────────────────────────────
# Everything arenas A and A2 claim is a claim about where the tank was STANDING
# when the pool decided: "the nearest forest was the walled one". A bot with
# nothing to do still explores (see TOKENS), so that is true for a stretch of
# the run and not for all of it. The sidecar logs every tile the tank stands
# on, in SIM ticks; this turns that trail into the set of BRAIN ticks during
# which the walled forest was strictly nearer than the open one AND the open
# one was inside the leash -- which is exactly the condition under which
# FARM_SECTORS=1 must be offering the walled row and only the walled row.
#
# print2's t= is the BRAIN tick and the sidecar's is the SERVER tick; the brain
# thinks once per 20 ms frame and the sim runs two server ticks per frame, so
# sim = 2 x brain (repair_priority_test.py says the same).
MIN_WINDOW_BRAIN_TICKS = 60     # a window shorter than this is not evidence


def tank_at(trail, sim_tick):
    """Where the tank stood at a SIM tick, from the sidecar's TANK trail."""
    here = None
    for (t, x, y) in trail:
        if t <= sim_tick:
            here = (x, y)
        else:
            break
    return here


def walled_window(trail, plant_tick, blocked, opened, last_sim):
    """(first_brain, last_brain, n_brain_ticks) of the stretch after the plant
    during which the WALLED forest was strictly the nearer of the two and both
    were inside BUILDER_POOL_LEASH -- i.e. every tick on which FARM_SECTORS=1
    has to be offering the walled row and nothing else.

    The FIRST contiguous stretch only: once the tank has wandered out of
    position the arena is over, and a later stretch is a different arena.
    Returns None if there is no such stretch at all."""
    first = last = None
    for sim in range(plant_tick, last_sim + 1, 2):
        p = tank_at(trail, sim)
        if p is None:
            continue
        db = abs(p[0] - blocked[0]) + abs(p[1] - blocked[1])
        do = abs(p[0] - opened[0]) + abs(p[1] - opened[1])
        if db < do and do <= G.LEASH and db <= G.LEASH:
            if first is None:
                first = sim // 2
            last = sim // 2
        elif first is not None:
            break
    if first is None:
        return None
    return first, last, last - first + 1


def _plant_and_window(build_dir, variant, text):
    """Shared arena-A setup: the plant, the trail and the window. Returns
    (blocked, opened, plant, win) or None after printing the failure."""
    trail, plant = read_trace(build_dir, variant)
    if not plant:
        print("FAIL (0): the sidecar never planted the arena. It waits for the "
              "tank to be ashore, off the spawn pond, and standing where the "
              "geometry fits inside the field. TANK trail: "
              + ", ".join("t%d:(%d,%d)" % r for r in trail[:12]))
        return None
    p_tick, p_mx, p_my, wall_x, bl_x, bl_y, op_x, op_y = plant
    blocked, opened = (bl_x, bl_y), (op_x, op_y)
    last_sim = trail[-1][0] if trail else p_tick
    ticks = [int(m[0]) for m in POOL_RE.findall(text)]
    if ticks:
        last_sim = max(last_sim, max(ticks) * 2)
    print("  0 OK: sidecar planted at sim t=%d with the tank on (%d,%d) -- "
          "full-height wall at x=%d, WALLED forest %s (%d tiles), OPEN forest "
          "%s (%d tiles)"
          % (p_tick, p_mx, p_my, wall_x, blocked,
             abs(bl_x - p_mx) + abs(bl_y - p_my), opened,
             abs(op_x - p_mx) + abs(op_y - p_my)))
    win = walled_window(trail, p_tick, blocked, opened, last_sim)
    if win is None:
        print("FAIL (0b): the walled forest was never the nearer of the two "
              "after the plant, so neither arena has anything to claim. TANK "
              "trail: " + ", ".join("t%d:(%d,%d)" % r for r in trail[:16]))
        return None
    if win[2] < MIN_WINDOW_BRAIN_TICKS:
        print("FAIL (0b): the walled forest was the nearer of the two for only "
              "%d brain ticks (t=%d..%d), under the %d this arena needs to be "
              "evidence -- the tank wandered out of position too fast."
              % (win[2], win[0], win[1], MIN_WINDOW_BRAIN_TICKS))
        return None
    print("  0b OK: the WALLED forest was the nearer of the two for brain "
          "ticks %d..%d (%d ticks) -- that is the window every claim below is "
          "made inside" % win)
    return blocked, opened, plant, win


# ── arena A: the wall ────────────────────────────────────────────────────
def check_A(sess, text, build_dir):
    got = _plant_and_window(build_dir, "A", text)
    if got is None:
        return 1
    blocked, opened, plant, win = got
    w0, w1, _wn = win

    multi = multi_candidate_ticks(text)
    disp = [d for d in farm_dispatches(text) if w0 <= int(d[0]) <= w1]
    if not disp:
        alld = farm_dispatches(text)
        print("FAIL (1): no farm dispatch inside the window (brain t=%d..%d). "
              "With four wedges the OPEN forest %s should be a row of its own "
              "and should fire. %d farm dispatch(es) in the whole run%s"
              % (w0, w1, opened, len(alld),
                 (", first at t=%s -> (%s,%s)"
                  % (alld[0][0], alld[0][2], alld[0][3])) if alld else ""))
        dumps(text, "BP_DENY", tail=6)
        dumps(text, "BUILDER_POOL", tail=3)
        return 1
    first = disp[0]
    got_tile = (int(first[2]), int(first[3]))
    if got_tile != opened:
        print("FAIL (1): the first farm dispatch inside the window went to %s, "
              "not to the OPEN forest at %s. If it went to %s the walled walk "
              "is not actually blocked." % (got_tile, opened, blocked))
        dumps(text, "BP_DISPATCH")
        return 1
    if int(first[0]) not in multi:
        print("FAIL (1b): the open forest was dispatched at t=%s, but the "
              "BUILDER_POOL line for that tick does not show 2+ candidates -- "
              "the walled row was not on the table, so nothing was outranked "
              "and the wedges proved nothing." % first[0])
        dumps(text, "BUILDER_POOL")
        return 1
    t = farm_terms(first[5])
    if not t:
        print("FAIL (1c): the dispatch line's term breakdown does not add up, "
              "so its score cannot be hand-checked: '%s'" % first[5])
        return 1
    legs = leg_chips(first[5])
    if not legs:
        print("FAIL (1d): no [legs out{} + build{} + back{} pred{} predsrc{}] "
              "chips on the dispatch line: '%s'" % first[5])
        return 1
    out_t, build_t, back_t, pred, predsrc, wedge = legs
    trip = t[6]
    if out_t + build_t + back_t != trip:
        print("FAIL (1e): the legs do not close -- out %d + build %d + back %d "
              "= %d, but the line prints trip %d."
              % (out_t, build_t, back_t, out_t + build_t + back_t, trip))
        return 1
    if wedge != "E":
        print("FAIL (1f): the open forest is east of the tank, so its row "
              "should carry wedge{E}; it carries wedge{%s}." % wedge)
        return 1
    print("  1 OK: first farm dispatch t=%s -> the OPEN forest %s with %d "
          "candidates on that tick; value %.0f - %.2f x trip %d = score %.0f; "
          "legs out %d + build %d + back %d = %d; wedge{%s} pred{%s} "
          "predsrc{%s}"
          % (first[0], opened, multi[int(first[0])], t[4], t[5], trip, t[0],
             out_t, build_t, back_t, trip, wedge, pred, predsrc))
    if predsrc != "same" or back_t != out_t:
        print("  1g NOTE: this arena expects the prediction to fall back (a "
              "tank on explore has no committed route), but the line says "
              "predsrc{%s} back=%d out=%d." % (predsrc, back_t, out_t))

    denied = [d for d in DENY_RE.findall(text)
              if (int(d[2]), int(d[3])) == blocked]
    if denied:
        print("  2 OK: the WALLED forest %s was scored and refused -- first "
              "BP_DENY at t=%s reason=%s" % (blocked, denied[0][0], denied[0][4]))
    else:
        print("  2 OK: the WALLED forest %s never reached the top of the pool, "
              "so it never printed a BP_DENY of its own (BP_DENY only prints "
              "the row that WOULD have won); the 2+ candidate count above is "
              "what shows it was scored alongside" % (blocked,))

    rows = read_jsonl(build_dir, sess)
    if check_man_out("3", rows, int(first[0]), opened):
        return 1
    print("PASS-A: four wedges made the OPEN forest %s a candidate of its own; "
          "on a tick when the WALLED forest %s was the nearer of the two and "
          "on the table, the pool scored them together and sent the man east "
          "to the reachable one." % (opened, blocked))
    return 0


# ── arena A2: the control, expected NOT to dispatch ──────────────────────
def check_A2(sess, text, build_dir):
    got = _plant_and_window(build_dir, "A2", text)
    if got is None:
        return 1
    blocked, opened, plant, win = got
    w0, w1, wn = win

    inside = [d for d in farm_dispatches(text) if w0 <= int(d[0]) <= w1]
    if inside:
        d = inside[0]
        print("FAIL (1): THIS CONTROL IS EXPECTED NOT TO DISPATCH, but a farm "
              "job went out at t=%s to (%s,%s) inside the window (brain "
              "t=%d..%d). With BUILDER_POOL_FARM_SECTORS=1 the only farm row "
              "is the NEAREST forest, which is the walled one at %s, and the "
              "man cannot walk to it."
              % (d[0], d[2], d[3], w0, w1, blocked))
        dumps(text, "BP_DISPATCH")
        return 1

    # ...and the reason has to be the walled tile, not "there were no rows".
    win_denies = [d for d in DENY_RE.findall(text) if w0 <= int(d[0]) <= w1]
    walled = [d for d in win_denies if (int(d[2]), int(d[3])) == blocked]
    open_rows = [d for d in win_denies if (int(d[2]), int(d[3])) == opened]
    if open_rows:
        print("FAIL (2): with one wedge the OPEN forest %s should not be a row "
              "at all while the walled one is nearer, but BP_DENY named it at "
              "t=%s." % (opened, open_rows[0][0]))
        dumps(text, "BP_DENY", tail=6)
        return 1
    if not walled:
        print("FAIL (2b): the walled forest %s never appeared on a BP_DENY "
              "inside the window, so there is no evidence the pool was "
              "offering it at all -- the control has to show the row it could "
              "not use, not just an empty pool." % (blocked,))
        dumps(text, "BP_DENY", tail=8)
        dumps(text, "BUILDER_POOL", tail=3)
        return 1
    reasons = sorted(set(d[4] for d in walled))
    print("  1 OK: inside the window the only farm row offered was the WALLED "
          "forest %s -- first BP_DENY at t=%s, reason(s) %s"
          % (blocked, walled[0][0], reasons))
    later = [d for d in farm_dispatches(text) if int(d[0]) > w1]
    tail = ""
    if later:
        tail = (" (after the window, once the tank had wandered and %s became "
                "the nearest forest instead, it did dispatch at t=%s to "
                "(%s,%s) -- which is the single-wedge rule working exactly as "
                "described)" % (opened, later[0][0], later[0][2], later[0][3]))
    print("PASS-A2 (EXPECTED NO DISPATCH): with "
          "cfg=BUILDER_POOL_FARM_SECTORS=1 the pool offered only the nearest "
          "forest -- the walled one at %s -- and for all %d brain ticks it was "
          "nearest, the man stayed in the tank. The clear forest at %s, which "
          "arena A sends him to on the same ground, was never a candidate.%s "
          "THE ABSENCE OF A DISPATCH IS THIS ARENA'S RESULT, so it "
          "deliberately has no 'the man came out' assertion."
          % (blocked, wn, opened, tail))
    return 0


# ── arena B: the drive ───────────────────────────────────────────────────
def _B_common(sess, text, build_dir, variant):
    tank, plant = read_trace(build_dir, variant)
    if not plant:
        print("FAIL (0): the sidecar never planted the forests -- the tank "
              f"never reached column {G.B_PLANT_X} ashore and off its boat. "
              "TANK trail: "
              + ", ".join(f"t{t}:({x},{y})" for t, x, y in tank[:16]))
        return None
    p_tick, p_mx, p_my, bx, by, ax, ay = plant
    behind, ahead = (bx, by), (ax, ay)
    print(f"  0 OK: sidecar planted at sim t={p_tick} with the tank driving "
          f"through ({p_mx},{p_my}) -- BEHIND forest {behind}, AHEAD forest "
          f"{ahead}, both {abs(bx - p_mx) + abs(by - p_my)} tiles away")
    disp = farm_dispatches(text)
    if not disp:
        print("FAIL (1): no farm dispatch at all after the plant.")
        dumps(text, "BP_DENY", tail=8)
        dumps(text, "BUILDER_POOL", tail=4)
        return None
    first = disp[0]
    t = farm_terms(first[5])
    if not t:
        print(f"FAIL (1c): the dispatch line's term breakdown does not add up: "
              f"'{first[5]}'")
        return None
    legs = leg_chips(first[5])
    if not legs:
        print(f"FAIL (1d): no [legs ...] chips on the dispatch line: "
              f"'{first[5]}'")
        return None
    out_t, build_t, back_t, pred, predsrc, wedge = legs
    trip = t[6]
    if out_t + build_t + back_t != trip:
        print(f"FAIL (1e): the legs do not close -- out {out_t} + build "
              f"{build_t} + back {back_t} = {out_t + build_t + back_t}, but "
              f"the line prints trip {trip}.")
        return None
    return (behind, ahead, first, t, legs, trip)


def check_B(sess, text, build_dir):
    got = _B_common(sess, text, build_dir, "B")
    if got is None:
        return 1
    behind, ahead, first, t, legs, trip = got
    out_t, build_t, back_t, pred, predsrc, wedge = legs
    tgt = (int(first[2]), int(first[3]))
    if tgt != ahead:
        print(f"FAIL (2): the man was sent to {tgt}, not to the AHEAD forest "
              f"{ahead}. Both forests are the same distance out, so the only "
              f"thing that can separate them is the walk HOME -- and the tank "
              f"is driving toward {ahead}.")
        dumps(text, "BP_DISPATCH")
        return 1
    if wedge != "E":
        print(f"FAIL (2b): the ahead forest is east of the tank, so its row "
              f"should carry wedge{{E}}; it carries wedge{{{wedge}}}.")
        return 1
    if predsrc != "route":
        print(f"FAIL (3): predsrc{{{predsrc}}} -- the prediction did not walk "
              f"the tank's route. A driving tank on a capture_pill goal has a "
              f"committed state.pf.path_chain; `same` means it fell back "
              f"(pred{{{pred}}}) and the return leg is just the outbound leg "
              f"mirrored, which is the behaviour this arena is testing AGAINST.")
        dumps(text, "BP_PRED", tail=4)
        return 1
    if back_t >= out_t:
        print(f"FAIL (4): back {back_t} is not shorter than out {out_t}. The "
              f"tank drives TOWARD this forest while the man walks to it, so "
              f"his walk home has to be the shorter leg.")
        return 1

    preds = PRED_RE.findall(text)
    mine = [p for p in preds if int(p[0]) == int(first[0])]
    if not mine:
        print(f"FAIL (5): no BP_PRED line at t={first[0]} -- the route the "
              f"return leg was priced against was never printed, so pred{{}} "
              f"cannot be checked against anything.")
        dumps(text, "BP_PRED", tail=4)
        return 1
    pr = mine[0]
    wps = [(int(a), int(b), float(c)) for a, b, c in WP_RE.findall(pr[9])]
    px, py = (int(v) for v in pred.split(","))
    if (px, py) not in [(w[0], w[1]) for w in wps]:
        print(f"FAIL (5b): the predicted tile ({px},{py}) is NOT on the route "
              f"BP_PRED printed: {pr[9][:200]}")
        return 1
    idx = [i for i, w in enumerate(wps) if (w[0], w[1]) == (px, py)][0]
    horizon = out_t + build_t
    if wps[idx][2] > horizon + 0.51:
        print(f"FAIL (5c): the predicted tile ({px},{py}) is {wps[idx][2]:.0f} "
              f"ticks along the route, past the horizon of out {out_t} + build "
              f"{build_t} = {horizon}.")
        return 1
    if idx + 1 < len(wps) and wps[idx + 1][2] <= horizon + 0.51:
        print(f"FAIL (5d): the route reaches ({wps[idx+1][0]},{wps[idx+1][1]}) "
              f"at {wps[idx+1][2]:.0f} ticks, still inside the horizon "
              f"{horizon} -- the prediction stopped short.")
        return 1
    print(f"  2 OK: the man went to the AHEAD forest {ahead}, wedge{{E}}, "
          f"score {t[0]:.0f} = value {t[4]:.0f} - {t[5]:.2f} x trip {trip}")
    print(f"  3 OK: predsrc{{route}} -- pred{{{pred}}} is waypoint {idx + 1} of "
          f"{len(wps)} on the printed route, {wps[idx][2]:.0f} ticks along it, "
          f"inside the horizon out {out_t} + build {build_t} = {horizon}"
          + (f" (the next waypoint is {wps[idx+1][2]:.0f} ticks, past it)"
             if idx + 1 < len(wps) else " (route ends there)"))
    print(f"  4 OK: back {back_t} < out {out_t} -- the tank closed the gap "
          f"while the man walked; trip = {out_t} + {build_t} + {back_t} = "
          f"{trip}")

    rows = read_jsonl(build_dir, sess)
    # No arrival requirement: the tank keeps driving east and the man is chasing
    # a moving home, so "he set off at it and closed" is the honest claim.
    if check_man_out("5", rows, int(first[0]), ahead, require_arrival=False):
        return 1
    print(f"PASS-B: with two forests the same distance out, the return-leg "
          f"prediction walked the tank's own route and sent the man to the one "
          f"AHEAD ({ahead}) rather than the one BEHIND ({behind}).")
    return 0


def check_B2(sess, text, build_dir):
    got = _B_common(sess, text, build_dir, "B2")
    if got is None:
        return 1
    behind, ahead, first, t, legs, trip = got
    out_t, build_t, back_t, pred, predsrc, wedge = legs
    tgt = (int(first[2]), int(first[3]))
    if predsrc != "off":
        print(f"FAIL (2): predsrc{{{predsrc}}} -- with "
              f"cfg=BUILDER_POOL_RETURN_PREDICT=false the chip must read "
              f"`off`. The token did not land.")
        return 1
    if pred != "same":
        print(f"FAIL (3): pred{{{pred}}} -- with the prediction off the walk "
              f"home is measured back to the tank's CURRENT tile and the chip "
              f"must read `same`.")
        return 1
    if back_t != out_t:
        print(f"FAIL (4): back {back_t} != out {out_t}. With the prediction "
              f"off the return leg is the outbound leg mirrored, which is the "
              f"whole definition of the old number.")
        return 1
    if trip != 2 * out_t + build_t:
        print(f"FAIL (4b): trip {trip} != 2 x out {out_t} + build {build_t} = "
              f"{2 * out_t + build_t} -- the old formula did not come back.")
        return 1
    if tgt != behind:
        print(f"FAIL (5): the man was sent to {tgt}. With the prediction off "
              f"the two rows score IDENTICALLY (same outbound leg, same "
              f"mirrored return), and order_rows breaks the tie on the lower "
              f"tile key -- which is the BEHIND forest {behind}. Getting "
              f"{ahead} here would mean the two rows were not actually equal "
              f"and arena B's result is about the arena, not the change.")
        dumps(text, "BP_DISPATCH")
        return 1
    print(f"  2 OK: predsrc{{off}} pred{{same}} -- the prediction is disabled")
    print(f"  3 OK: back {back_t} == out {out_t}, trip = 2 x {out_t} + "
          f"{build_t} = {trip}, the pre-2026-09-06 number exactly")
    print(f"  4 OK: score {t[0]:.0f} = value {t[4]:.0f} - {t[5]:.2f} x trip "
          f"{trip}, and the tie went to the lower tile key")
    rows = read_jsonl(build_dir, sess)
    if check_man_out("5", rows, int(first[0]), behind, require_arrival=False):
        return 1
    print(f"PASS-B2 (THE CONTROL, AND IT GOES THE WRONG WAY ON PURPOSE): with "
          f"cfg=BUILDER_POOL_RETURN_PREDICT=false the two equidistant forests "
          f"score the same and the man is sent BEHIND the tank, to {behind} -- "
          f"away from where it is driving. That is the bug arena B shows fixed.")
    return 0


CHECKS = {"A": check_A, "A2": check_A2, "B": check_B, "B2": check_B2}


def run_variant(v, ticks, build_dir):
    print(f"\n=== farm_sectors arena {v} "
          f"({'wall / four wedges' if v == 'A' else ''}"
          f"{'wall / ONE wedge -- expected NO dispatch' if v == 'A2' else ''}"
          f"{'drive / return-leg prediction' if v == 'B' else ''}"
          f"{'drive / prediction OFF -- expected the WRONG forest' if v == 'B2' else ''}"
          f") ===")
    print(f"    tokens: {TOKENS[v]}  ({len(TOKENS[v])} bytes)")
    sess, text = run_sim(v, ticks, build_dir)
    if sess is None:
        print(f"FAIL ({v}): {text}")
        return 1
    errs = lua_errors(text)
    if errs:
        print(f"FAIL ({v}): the brain logged Lua errors:")
        for e in errs:
            print("   " + e)
        return 1
    print(f"    session: {sess.name}")
    return CHECKS[v](sess, text, build_dir)


def main():
    argv = sys.argv[1:]
    take_asap_flag(argv)          # strips --asap/--no-asap from argv in place
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="all",
                    choices=["A", "A2", "B", "B2", "all"])
    ap.add_argument("--ticks", type=int, default=None)
    ap.add_argument("--build", default=str(DEFAULT_BUILD))
    args = ap.parse_args(argv)
    build_dir = Path(args.build)
    print(pacing_line(""))

    variants = ["A", "A2", "B", "B2"] if args.variant == "all" else [args.variant]
    rc = 0
    failed = []
    for v in variants:
        r = run_variant(v, args.ticks or TICKS[v], build_dir)
        rc |= r
        if r:
            failed.append(v)
    print()
    if rc:
        print(f"FARM SECTORS: FAIL ({', '.join(failed)})")
    else:
        print("FARM SECTORS: PASS (" + ", ".join(variants) + ")")
    return rc


if __name__ == "__main__":
    sys.exit(main())
