#!/usr/bin/env python3
"""a seeded builder-pool row COMPETES, and the goal's own pill gets 20%
(GoalHunter 1.7 builder pool, 2026-09-06).

THE INCIDENT (20260905_231835_2_oilrig_2v2_repair_seed1, bot2, t=67922)
-----------------------------------------------------------------------
bot2 held defend_pill on pill #5 at (139,142) on 14/15 hp, and that goal's own
repair order SEEDED the builder pool with a one-hp top-up of it.  The seeded row
scored 30 x 1 - 0.25 x 254 = -34 -- below BUILDER_POOL_MIN_SCORE, i.e. not worth
the walk -- but it sorted FIRST by construction, and because a defend goal with
`goal.repair` was excluded from BUILDER_POOL_TRAVEL_GOALS it ALSO made the pool
read `mode_owned (suppressed/defend_pill)` for every other row.  Ten tiles the
other way, pill #13 at (123,139) sat on 10/15 and was worth ~87.  It never got
to bid:

    BP_DENY t=67922 job=topup target=(139,142) reason=under_fire(2t) ... score=-34

TWO KNOBS
---------
  BUILDER_POOL_SEEDED_COMPETES  (default true, keel false)
      The seeded row is scored by the ordinary formula, must clear MIN_SCORE,
      and is ORDERED by score like every other row.  It keeps the three waivers
      seeding is FOR -- the mode gate FOR ITSELF, the leash, the tree reserve --
      and it no longer closes the pool to its neighbours: the mode gate they see
      is the one they would have seen had the goal never seeded.

  BUILDER_POOL_GOAL_PILL_BONUS  (default 1.2, keel 1.0)
      The row whose pill IS the tank goal's target (defend_pill / repair_pill
      target_id) has its whole score MULTIPLIED by this, after the linear /
      legacy arithmetic and before the MIN_SCORE bar and the ordering.  It
      prints as ` = bp_raw{317} x goal_w{1.20}` on the end of the chip chain, so
      the boosted number is still hand-checkable from the line alone.

FOUR RUNS (tests/generate_seeded_repair_map.py builds the ground and writes the
sidecars; its docstring carries the geometry and the margins).

  A   THE INCIDENT, ON THE GROUND.  The tank holds defend_pill on the GOAL PILL
      at (127,126) on 14/15 and parks beside it, so the goal seeds a one-hp
      top-up worth 30 - 0.25 x 52 = 17.  Six tiles west the OTHER PILL sits on
      10/15, worth 30 x 5 - 0.25 x ~200 = ~100.  The man must walk PAST the
      goal's own pill to the other one, and the BP_DISPATCH line must carry the
      seeded row it beat (`seed=topup@(127,126)/defend_repair seed_score=19`).
      The pool's own verdict line must read `elig=yes` on that tick: the seed no
      longer speaks for the pool.

  A2  THE CONTROL, and NO DISPATCH IS EXPECTED IN IT.  Same ground, both knobs
      at their keel values (cfg=BUILDER_POOL_SEEDED_COMPETES=false;
      cfg=BUILDER_POOL_GOAL_PILL_BONUS=1).  Now the seeded row sorts first
      whatever it scores, fails MIN_SCORE at ~16, and the other pill's row is
      refused with the pool-wide `mode_owned (suppressed/defend_pill)` the seed
      itself caused.  The result is that NOBODY GOES: the assertion is the
      ABSENCE of a dispatch to the other pill plus the presence of the two
      refusals that explain it.  That absence is the whole point -- it is what
      makes arena A's dispatch evidence about the knob and not about an arena
      where the man would have gone anyway.

  B   THE BONUS DECIDES.  Two pills 2 tiles apart on row 126, BOTH on 4/15, and
      the tank spawns six tiles due north on the column BETWEEN them -- so the
      walk to one is the walk to the other at every tile of the drive down, and
      the defend hold tile (126,125) is a diagonal neighbour of both.  The two
      rows are equal to the tick; only goal_w{1.20} separates them.  The
      dispatch must go to the GOAL pill, and its printed chips must reproduce
      the boosted score: value - tripcost = bp_raw, bp_raw x 1.20 = bp_score.

  B2  THE CONTROL for B: cfg=BUILDER_POOL_GOAL_PILL_BONUS=1 and nothing else
      (SEEDED_COMPETES stays at its default -- arena B is about the bonus, and
      the seeded row competing is what puts the two rows side by side in the
      first place).  With the bonus gone the two rows tie exactly, the sort
      falls through to the tile key, and the OTHER pill -- lower x, lower key --
      goes FIRST.  Its line must carry no goal_w chip at all.

WHY THE cfg= TOKENS.  cfg=DEFEND_ALARM_MODE=false puts the defend evaluator
back on its keel ladder, which is the only one with an ARRIVED branch and
therefore the only one that can produce the incident's defend->repair seed (see
ALARM_OFF below; the pool code under test is the same for the repair_pill
feeder, which alarm mode does not touch).  defend_pill's flat
DEFEND_REPAIR_COST(40) arrived bid is then left alone -- it is the feeder under
test -- while REPAIR_BASE_COST goes past the 1e29 "unaffordable" line so pool 5
(repair_pill), the OTHER feeder into the same seed, never takes the goal.
BRAIN_INIT_ARG is 127 bytes (BotInitSlot.arg, luabrainshandler.h) and a longer
string is truncated MID-TOKEN, so ARG_MAX below asserts it rather than leaving
it to be rediscovered.

WHICH PILL THE GOAL LANDS ON is not assumed.  Both pills are inside
DEFEND_ARRIVE_RADIUS of the spawn, so both make the same flat arrived bid and
the tie breaks to the lower pill index -- the pill the generator writes FIRST.
Every arena therefore asserts, from the brain's own HEAT_GATE line, that the
defend REPAIR bid was made on the pill it means by "the goal pill".

AND, ON TOP OF THE ORDERING, THE THING THE ORDERING IS FOR.  A BP_DISPATCH line
is a DECISION; it is not a man on the grass and it is not a pill back on its
feet.  So every arena that expects a dispatch (A, B, B2) also asserts:

  MAN OUT -- from the brain's per-tick jsonl (logger.lua): within MAN_OUT_GRACE
     ticks of the dispatch man_status leaves LGM_INTANK and his tile closes on
     the target.

  REPAIRED, NOT CAPTURED -- from the sidecar's engine-side trace: the armour
     rose, and up to that tick the owner never changed and in_tank never went
     true (an armour rise on its own is also what a capture-and-repair looks
     like).

Usage: python seeded_repair_test.py [--variant A|A2|B|B2|all] [--ticks N]
                                    [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

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
import generate_seeded_repair_map as G   # noqa: E402

PORTS = {"A": 50320, "A2": 50321, "B": 50322, "B2": 50323}
# ENGINE ticks.  The brain thinks once per 20 ms frame and the sim advances two
# engine ticks per frame, so these are ~half as many brain ticks.  B and B2 need
# room for TWO complete errands, one after the other.
TICKS = {"A": 2500, "A2": 2500, "B": 3000, "B2": 3000}

ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h
# THE FEEDER THIS TEST DRIVES. The incident's feeder is the defend->repair
# handoff (defend_pill + goal.repair -> builder_pool.seed_job), and since
# 2026-09-06 that rung exists only on the KEEL defend path: the alarm-mode
# defend evaluator (C.DEFEND_ALARM_MODE, on by default) has no ARRIVED branch
# at all, so it never sets goal.repair and never seeds. These arenas therefore
# pin DEFEND_ALARM_MODE=false -- which is the incident's configuration, and the
# only one in which the handoff can be staged.
#
# What is under test is the POOL, not the feeder: seed_job's other caller
# (goal.kind == "repair_pill", unaffected by alarm mode) enters the identical
# code path in builder_pool.update, so the two knobs behave the same way behind
# it. Pool 5 is kept out of these arenas by geometry rather than by a token --
# every pill is inside BUILDER_POOL_REPAIR_LEASH of every tile the tank can
# stand on, so goals.lua prices repair_pill INF with `builder_can (leash 11)`.
ALARM_OFF = "cfg=DEFEND_ALARM_MODE=false"
# ...and pool 5 out of reach as well. The leash split (goals.lua prices a
# repair_pill row INF with `builder_can (leash 11)` once the man can walk it
# from where the tank stands) does NOT cover the first replan: before the
# sidecar fills the spawn pond the walk sim starts on water, reads STUCK, and
# builder_can_repair returns nil -- so repair_pill wins the goal on tick 11 and
# COMMITS, and a run that measured it went to the wrong pill for the wrong
# reason (owner=repair_pill/gather, seeded_by=repair_pill). Priced past the
# 1e29 "unaffordable" line there is no such window.
NO_POOL5 = "cfg=REPAIR_BASE_COST=1e30"
KEEL_SEED = "cfg=BUILDER_POOL_SEEDED_COMPETES=false"
# `=1`, not `=1.0`: A2 needs all four tokens and the string is 127 bytes, which
# is exactly BRAIN_INIT_ARG's limit (ARG_MAX asserts it before every run). Both
# spellings are the same Lua number.
KEEL_BONUS = "cfg=BUILDER_POOL_GOAL_PILL_BONUS=1"
TOKENS = {
    "A":  ";".join([NO_POOL5, ALARM_OFF]),
    "A2": ";".join([NO_POOL5, ALARM_OFF, KEEL_SEED, KEEL_BONUS]),
    "B":  ";".join([NO_POOL5, ALARM_OFF]),
    # NOT KEEL_SEED: with the seeded row sorting first, arena B would measure
    # the seeding rule again instead of the bonus. B is the bonus arena, and
    # its control turns off the bonus and nothing else.
    "B2": ";".join([NO_POOL5, ALARM_OFF, KEEL_BONUS]),
}
GAMETYPE = {v: "open" for v in TOKENS}

# ── print2 lines (builder_pool.lua) ───────────────────────────────────────
# BP_DISPATCH t=453 job=topup target=(118,126) score=80 (bp_score{80} = hp_w{30}
#   x missing{5} = value{150} - trip_w{0.25} x trip{278t} = tripcost{70} [legs
#   ...]) [linear] eta=129 trip=278 trees=40-2 front=12 owner=defend_pill/
#   suppressed claim=453 out=129 build=20 back=129 pred=same/no_route
#   seed=topup@(127,126)/defend_repair seed_score=20 seed_reject=below_min...
DISP_RE = re.compile(
    r"BP_DISPATCH t=(\d+) job=(\S+) target=\((\d+),(\d+)\)(.*?) score=(-?[\d.]+) "
    r"\((.*?)\)(?: \[linear\])? "
    r"eta=(\S+) trip=(\S+) trees=(\d+)-(\d+) front=(-?\d+)")
LINEAR_RE = re.compile(
    r"bp_score\{(-?[\d.]+)\} = hp_w\{(\d+)\} x missing\{(\d+)\} = value\{(-?[\d.]+)\} - "
    r"trip_w\{([\d.]+)\} x trip\{(\d+)t\} = tripcost\{(-?[\d.]+)\}")
# The goal-pill bonus, the last link of the chain and printed ONLY when the
# factor is not 1 (so its ABSENCE is the assertion in the control arenas).
GOALW_RE = re.compile(
    r"= bp_raw\{(-?[\d.]+)\} x goal_w\{([\d.]+)\}")
# The seeded row when it is NOT the row the line is about -- the shape that only
# exists because a seeded row can now lose.
SEED_TAIL_RE = re.compile(
    # seed_reject runs to the end of the line: reject strings carry spaces
    # ("below_min_score(19 < 20)"), and the tail is the last thing on the line.
    r" seed=(\S+)@\((\d+),(\d+)\)/(\S+) seed_score=(-?[\d.]+) seed_reject=(.*)$")
DENY_RE = re.compile(
    r"BP_DENY t=(\d+) job=(\S+) target=\((\d+),(\d+)\) reason=(.*?) elig=(.*?) "
    r"score=(-?[\d.]+) trip=(\S+)")
POOL_RE = re.compile(
    r"BUILDER_POOL t=(\d+) owner=(\S+) elig=(.*?) cands=(\d+) ok=(\d+) ")
# goals.lua's defend REPAIR bid: which pill the tank's goal is actually about.
HEAT_REPAIR_RE = re.compile(
    r"HEAT_GATE t=(\d+) pill@\((\d+),(\d+)\) REPAIR BID")

LGM_INTANK, LGM_DEAD, LGM_MOVING = 0, 1, 2      # constants.lua
MAN_OUT_GRACE = 40


# ── run plumbing (the shape tests/repair_priority_test.py established) ────
def find_ds(build_dir):
    for c in (build_dir / "Release" / "WinBoloDS.exe",
              build_dir / "WinBoloDS.exe", build_dir / "Release" / "WinBoloDS",
              build_dir / "WinBoloDS"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = [p for p in (build_dir / "debug_sessions").glob(f"*_{label}")
            if p.is_dir()]
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def read_trace(build_dir, variant):
    """[(sim_tick, x, y, armour, owner, in_tank), ...] from the sidecar."""
    path = build_dir / f"seeded_repair_{variant}_trace.log"
    seq = []
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 6:
                seq.append(tuple(int(p) for p in parts[:6]))
    return seq


def read_jsonl(build_dir, sess):
    """The brain's per-tick rows (logger.lua). See repair_priority_test."""
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
    """What the LGM did after a dispatch at brain tick `disp_tick`."""
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


def check_man_out(step, rows, disp_tick, target):
    """MAN OUT: the man physically left the tank and walked AT the pill."""
    if not rows:
        print(f"FAIL ({step}): no per-tick jsonl for this run, so 'the man came "
              f"out' cannot be checked (expected build/player0_<stamp>.jsonl).")
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
    if w["closest"] is None or w["closest"] >= (w["start_d"] or 0):
        print(f"FAIL ({step}): the man left at t={w['out_t']} from "
              f"{w['out_tile']} ({w['start_d']} tiles off {target}) and never "
              f"got closer than {w['closest']} -- he went OUT, but not at this "
              f"pill.")
        return 1
    if w["arrive_t"] is None:
        print(f"FAIL ({step}): the man left at t={w['out_t']} and closed from "
              f"{w['start_d']} tiles to {w['closest']} at t={w['closest_t']}, "
              f"but never stood on {target} or beside it"
              + (f" -- he died at t={w['died_t']}." if w["died_t"] else
                 f" -- back in the tank at t={w['home_t']}."
                 if w["home_t"] else " -- still out when the run ended."))
        return 1
    print(f"  {step} OK: MAN OUT -- dispatch t={disp_tick}, man_status left the "
          f"tank at t={w['out_t']} from {w['out_tile']} ({w['start_d']} tiles "
          f"off {target}), arrived t={w['arrive_t']}")
    return 0


def repair_landed(step, trace, target, owner=0):
    """REPAIRED, NOT CAPTURED: the armour rose and nobody took the pill."""
    rows = [r for r in trace if (r[1], r[2]) == target]
    if not rows:
        print(f"FAIL ({step}): the sidecar traced no pill at {target} -- its "
              f"OURS list and the generator's geometry have drifted apart.")
        return 1
    a0 = rows[0][3]
    rise = next((r for r in rows if r[3] > a0), None)
    if rise is None:
        print(f"FAIL ({step}): the engine never saw {target}'s armour rise "
              f"above the {a0} it started at. Trace: "
              + ", ".join(f"{t}:a={a},own={o},tank={k}"
                          for t, _x, _y, a, o, k in rows[:12]))
        return 1
    before = [r for r in rows if r[0] <= rise[0]]
    stolen = [r for r in before if r[4] != owner]
    if stolen:
        print(f"FAIL ({step}): {target} changed hands at sim t={stolen[0][0]} "
              f"before its armour rose at sim t={rise[0]}.")
        return 1
    boarded = [r for r in before if r[5]]
    if boarded:
        print(f"FAIL ({step}): {target} was IN A TANK at sim t={boarded[0][0]} "
              f"before the armour rose -- that is a capture, not a repair.")
        return 1
    print(f"  {step} OK: REPAIRED, NOT CAPTURED -- engine says {target} went "
          f"{a0} -> {rise[3]}/{G.PILLS_MAX_HEALTH} armour at sim t={rise[0]}, "
          f"owner stayed {owner} and in_tank never went true")
    return 0


def run_sim(variant, ticks, build_dir):
    """Runs the arena; returns (session_dir, our_log_text) or (None, msg)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"
    subprocess.run([sys.executable,
                    str(HERE / "generate_seeded_repair_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"seededrepair_{variant}"
    final = HERE / f"seeded_repair_{variant}_final.json"
    stderr = HERE / f"seeded_repair_{variant}_stderr.txt"
    trace = build_dir / f"seeded_repair_{variant}_trace.log"
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
    cmd = [str(ds), "-map", str(HERE / f"seeded_repair_{variant}.map"),
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


def dumps(text, needle, n=6, tail=False):
    lines = [l.strip() for l in text.splitlines() if needle in l]
    for ln in (lines[-n:] if tail else lines[:n]):
        print("   " + ln)


def dispatch_lines(text):
    """One dict per BP_DISPATCH: tick, job, target, score, terms, and the whole
    line (the seed tail and the goal_w chip are read off it)."""
    out = []
    for ln in text.splitlines():
        if "BP_DISPATCH" not in ln:
            continue
        m = DISP_RE.search(ln)
        if not m:
            continue
        out.append({"t": int(m.group(1)), "job": m.group(2),
                    "target": (int(m.group(3)), int(m.group(4))),
                    "mid": m.group(5), "score": float(m.group(6)),
                    "terms": m.group(7), "line": ln.strip()})
    return out


def linear_terms(terms):
    """(score, missing, value, trip, tripcost, raw, goal_w) off a linear chip
    chain, or None if the arithmetic does not CLOSE.

    The standing rule for these lines is that every factor is ON them, so a
    reader can hand-check the score without opening constants.lua.  Since
    2026-09-06 that includes the goal-pill bonus: when the chain ends
    `= bp_raw{317} x goal_w{1.20}` the bp_score at its head is the PRODUCT, and
    value - tripcost is bp_raw."""
    m = LINEAR_RE.search(terms)
    if not m:
        return None
    score, hp_w, missing, value, trip_w, trip, cost = (
        float(m.group(1)), float(m.group(2)), int(m.group(3)),
        float(m.group(4)), float(m.group(5)), int(m.group(6)),
        float(m.group(7)))
    if abs(hp_w * missing - value) > 0.51:
        return None
    if abs(trip_w * trip - cost) > 0.51 + 0.005 * trip:
        return None
    g = GOALW_RE.search(terms)
    if g:
        raw, gw = float(g.group(1)), float(g.group(2))
        if abs(raw - (value - cost)) > 1.01:
            return None
        if abs(score - raw * gw) > 1.01:
            return None
    else:
        raw, gw = value - cost, 1.0
        if abs(score - raw) > 1.01:
            return None
    return score, missing, value, trip, cost, raw, gw


def goal_pill_from_log(text):
    """The pill the tank's defend goal made its REPAIR bid on, or None."""
    hits = HEAT_REPAIR_RE.findall(text)
    if not hits:
        return None
    return (int(hits[0][1]), int(hits[0][2]))


def check_goal_pill(step, text, expect):
    """The arena assumes the defend goal landed on a particular pill (the two
    arrived bids are equal, so the tie breaks to the lower pill index). Assert
    it instead of assuming it."""
    got = goal_pill_from_log(text)
    if got is None:
        print(f"FAIL ({step}): the brain never printed a defend REPAIR BID at "
              f"all, so no goal ever seeded the pool and this arena measured "
              f"nothing. (HEAT_GATE lines below.)")
        dumps(text, "HEAT_GATE")
        return 1
    if got != expect:
        print(f"FAIL ({step}): the defend REPAIR bid was made on {got}, not on "
              f"the goal pill {expect} this arena is built around -- the two "
              f"arrived bids are both DEFEND_REPAIR_COST(40) and the tie broke "
              f"the other way. Swap the pill order in the generator.")
        return 1
    print(f"  {step} OK: the tank's defend goal is the pill at {got} "
          f"(HEAT_GATE REPAIR BID), so that is the pill the pool sees as "
          f"seeded and as the goal's own")
    return 0


# ── arena A: the seeded row competes, and loses ──────────────────────────
def check_A(sess, text, build_dir):
    goal, other = G.A_GOAL_PILL, G.A_OTHER_PILL
    rc = check_goal_pill(0, text, goal)
    if rc:
        return rc

    disp = dispatch_lines(text)
    if not disp:
        print("FAIL (1): the pool never dispatched at all. With the seeded row "
              "competing, the other pill's ~80 should have won a tick as soon "
              "as the man was available.")
        dumps(text, "BP_DENY", tail=True)
        return 1
    first = disp[0]
    if first["target"] != other:
        print(f"FAIL (1): the FIRST dispatch went to {first['target']}, not to "
              f"the other pill at {other}. The seeded one-hp top-up of the "
              f"goal pill is worth ~17 x 1.2 = ~20 against ~80 -- with "
              f"BUILDER_POOL_SEEDED_COMPETES on it must not win by being "
              f"seeded.\n   {first['line']}")
        return 1
    print(f"  1 OK: the first dispatch went to the other pill {other} at "
          f"t={first['t']} (score {first['score']:.0f}), NOT to the goal pill "
          f"the tank was parked on")

    # 2. The line has to SAY that a seeded row lost, and what it scored -- that
    #    is the whole mechanism, and it is not visible from the target alone.
    st = SEED_TAIL_RE.search(first["line"])
    if not st:
        print(f"FAIL (2): the dispatch line carries no `seed=...@(x,y)/src "
              f"seed_score=..` tail, so it does not say that the goal's own "
              f"seeded row was on the table and lost:\n   {first['line']}")
        return 1
    seed_tile = (int(st.group(2)), int(st.group(3)))
    seed_score = float(st.group(5))
    if seed_tile != goal:
        print(f"FAIL (2): the seeded row on the dispatch line is at "
              f"{seed_tile}, not the goal pill {goal}.")
        return 1
    if seed_score >= first["score"]:
        print(f"FAIL (2): the seeded row scored {seed_score:.0f}, at or above "
              f"the winner's {first['score']:.0f} -- then it did not lose on "
              f"SCORE and this arena proves nothing about the ordering.")
        return 1
    print(f"  2 OK: the seeded row {seed_tile} (src={st.group(4)}) was on the "
          f"table and lost on score: {seed_score:.0f} against "
          f"{first['score']:.0f}, reject={st.group(6)}")

    # 3. The pool-wide gate. Under the old rule the feeder goal made the whole
    #    pool `mode_owned (suppressed/defend_pill)`; the fix is that the OTHER
    #    rows see the gate they would have seen with no seed at all.
    verdicts = {int(t): e for (t, _o, e, _c, _ok) in POOL_RE.findall(text)}
    v = verdicts.get(first["t"])
    if v is None:
        print(f"FAIL (3): no BUILDER_POOL verdict line on the dispatch tick "
              f"t={first['t']}.")
        return 1
    if not v.startswith("yes"):
        print(f"FAIL (3): the pool's verdict on the dispatch tick was "
              f"`elig={v}` -- a dispatch through a closed gate is a different "
              f"bug, not this fix.")
        return 1
    owned = sum(1 for e in verdicts.values() if "mode_owned" in e)
    print(f"  3 OK: the pool's verdict on the dispatch tick is `elig=yes`; "
          f"{owned} of {len(verdicts)} verdict lines in the whole run read "
          f"mode_owned (the seed no longer speaks for the pool)")

    # 4. The winner's arithmetic, including the absence of a bonus on it: the
    #    other pill is not the goal's pill, so it must carry no goal_w chip.
    t = linear_terms(first["terms"])
    if not t:
        print(f"FAIL (4): the winner's breakdown does not add up: "
              f"'{first['terms']}'")
        return 1
    score, missing, value, trip, cost, raw, gw = t
    if gw != 1.0:
        print(f"FAIL (4): the other pill's row carries goal_w{{{gw}}} -- it is "
              f"not the tank goal's target and must not be boosted.")
        return 1
    if missing != G.PILLS_MAX_HEALTH - G.A_OTHER_HP:
        print(f"FAIL (4): the winner is missing {missing} hp, not the "
              f"{G.PILLS_MAX_HEALTH - G.A_OTHER_HP} the generator placed -- "
              f"the arena and the run have drifted apart.")
        return 1
    print(f"  4 OK: hand-checkable and unboosted -- 30 x missing({missing}) = "
          f"{value:.0f} - 0.25 x trip({trip}t) = {cost:.0f} -> "
          f"score {score:.0f}, no goal_w chip")

    rows = read_jsonl(build_dir, sess)
    rc |= check_man_out(5, rows, first["t"], other)
    rc |= repair_landed(6, read_trace(build_dir, "A"), other)
    if rc:
        return rc
    print("PASS (A): a seeded row now COMPETES -- the goal's own one-hp top-up "
          "did not sort first, did not close the pool, and the man walked past "
          "it to the five-hp repair six tiles away and fixed it.")
    return 0


# ── arena A2: the control, where nobody is expected to go ────────────────
def check_A2(sess, text, build_dir):
    goal, other = G.A_GOAL_PILL, G.A_OTHER_PILL
    rc = check_goal_pill(0, text, goal)
    if rc:
        return rc

    # THE WINDOW. The claim is about the ticks in which the GOAL PILL is the
    # seeded row -- that is the incident's situation. Nothing keeps the tank's
    # own goal there for ever: with nobody repairing anything (which is the
    # control's whole point) the defend pool eventually re-targets the other
    # pill, and from that tick on the other pill IS the seeded row and going to
    # it is the keel rule working, not breaking. So the window ends at the first
    # line on which the other pill carries seeded_by=.
    moved_t = None
    for ln in text.splitlines():
        if (f"target=({other[0]},{other[1]})" in ln and "seeded_by=" in ln):
            m = re.search(r" t=(\d+)", ln)
            if m:
                moved_t = int(m.group(1))
                break
    horizon = (f"the {moved_t} brain ticks before the tank's own goal moved to "
               f"{other} (it seeds that pill from t={moved_t} on)"
               if moved_t is not None else "the whole run")

    def in_window(t):
        return moved_t is None or t < moved_t

    # 1. THE CLAIM: nobody goes. Stated as the expected outcome, not as an
    #    absence somebody forgot to assert.
    disp = dispatch_lines(text)
    early = [d for d in disp if in_window(d["t"])]
    if early:
        d0 = early[0]
        print(f"FAIL (1): with both knobs at their keel values the pool must "
              f"send NOBODY while the goal pill {goal} is the seeded row -- it "
              f"sorts first whatever it scores and the pool-wide mode gate and "
              f"reserve_eta are closed behind it -- but it dispatched to "
              f"{d0['target']} at t={d0['t']}:\n   {d0['line']}")
        return 1
    print(f"  1 OK: NO DISPATCH, as expected -- 0 BP_DISPATCH lines in "
          f"{horizon} (this is the control; the absence IS the result)")

    # 2. ...and the two refusals that explain it, both on the same deny line:
    #    the seeded row on top (seeded_by=), failing MIN_SCORE, and the
    #    pool-wide mode_owned in elig= that is holding the other row down.
    deny = [(int(t), j, int(x), int(y), r, e, float(s), tr)
            for (t, j, x, y, r, e, s, tr) in DENY_RE.findall(text)]
    if not deny:
        print("FAIL (2): the pool never printed a BP_DENY, so nothing says WHY "
              "nobody went -- an arena with no rows would look the same.")
        dumps(text, "BUILDER_POOL", tail=True)
        return 1
    seeded_deny = []
    for ln in text.splitlines():
        if ("BP_DENY" in ln and "seeded_by=" in ln
                and f"target=({goal[0]},{goal[1]})" in ln):
            m = re.search(r" t=(\d+)", ln)
            if m and in_window(int(m.group(1))):
                seeded_deny.append(ln.strip())
    if not seeded_deny:
        print("FAIL (2): no BP_DENY names the SEEDED row on the goal pill as "
              "the row that would have won -- under keel it sorts first "
              "whatever it scores, so it should be the one being denied.")
        dumps(text, "BP_DENY", tail=True)
        return 1
    if "below_min_score" not in seeded_deny[0]:
        print(f"FAIL (2): the seeded row was denied for something other than "
              f"MIN_SCORE, so the arena is not measuring the seeded-first rule:"
              f"\n   {seeded_deny[0]}")
        return 1
    print(f"  2 OK: the seeded row is what the pool denies, on MIN_SCORE -- "
          f"and the SAME line carries the pool-wide verdict that is holding "
          f"the other row down:\n   {seeded_deny[0]}")

    owned = [e for (t, _j, _x, _y, _r, e, _s, _tr) in deny
             if "mode_owned" in e and in_window(t)]
    if not owned:
        print("FAIL (3): no BP_DENY line in the window carries a pool-wide "
              "`elig=mode_owned (...)`, so the other pill was held back by "
              "something else and the control is not reproducing the incident. "
              "elig values seen: "
              + ", ".join(sorted({e for (_t, _j, _x, _y, _r, e, _s, _tr)
                                  in deny}))[:300])
        return 1
    print(f"  3 OK: {len(owned)} deny line(s) in the window carry the "
          f"pool-wide `elig={owned[0]}` the feeder goal caused -- which is "
          f"what kept the other pill's row from ever competing")

    rows = read_jsonl(build_dir, sess)
    outs = [r for r in rows if r["lgm"] == LGM_MOVING]
    if outs:
        near = min(abs((r["lx"] >> 8) - other[0]) + abs((r["ly"] >> 8) - other[1])
                   for r in outs)
        print(f"  4 --: the jsonl shows the man out on {len(outs)} tick(s) "
              f"(nearest {near} tile(s) to {other}) -- not a pool errand")
    else:
        print("  4 --: the jsonl shows man_status never left LGM_INTANK")
    print(f"PASS (A2): the control reproduces the incident -- the seeded row "
          f"sorts first, fails MIN_SCORE at ~17, and the pool-wide mode_owned "
          f"it causes keeps the five-hp repair off the table. NO LGM IS "
          f"EXPECTED IN THIS ARENA while the goal pill is the seeded one: the "
          f"refusal is the whole result, and it is what makes arena A's "
          f"dispatch evidence about the knob"
          + (f". (Later, at t={moved_t}, the tank's own defend goal moves to "
             f"{other} -- with nobody repairing anything it eventually "
             f"re-targets -- and from there the man may go, because by then "
             f"that pill IS the seeded row.)" if moved_t is not None else "."))
    return 0


# ── arena B: the 20% bonus decides ───────────────────────────────────────
def _check_B(sess, text, build_dir, variant, expect_first, expect_bonus):
    goal, other = G.B_GOAL_PILL, G.B_OTHER_PILL
    rc = check_goal_pill(0, text, goal)
    if rc:
        return rc

    disp = dispatch_lines(text)
    if not disp:
        print("FAIL (1): the pool never dispatched at all -- both rows are "
              "worth ~317 and MIN_SCORE is 20.")
        dumps(text, "BP_DENY", tail=True)
        return 1
    first = disp[0]
    if first["target"] != expect_first:
        why = ("the goal pill's row is multiplied by "
               f"BUILDER_POOL_GOAL_PILL_BONUS({G.GOAL_PILL_BONUS}) and the two "
               "rows are otherwise equal"
               if expect_bonus else
               "with the bonus at 1.0 the two rows tie exactly and the sort "
               "falls through to the tile key, which is lower on the other pill")
        print(f"FAIL (1): the FIRST dispatch went to {first['target']}, not to "
              f"{expect_first} -- {why}.\n   {first['line']}")
        return 1
    print(f"  1 OK: the first dispatch went to {first['target']} at "
          f"t={first['t']} (score {first['score']:.0f})")

    t = linear_terms(first["terms"])
    if not t:
        print(f"FAIL (2): the winner's breakdown does not add up: "
              f"'{first['terms']}'")
        return 1
    score, missing, value, trip, cost, raw, gw = t
    if expect_bonus:
        if gw == 1.0:
            print(f"FAIL (2): the goal pill's row carries no goal_w chip, so "
                  f"the score on it is not the boosted one and nothing on the "
                  f"line explains why it won:\n   {first['terms']}")
            return 1
        if abs(gw - G.GOAL_PILL_BONUS) > 1e-9:
            print(f"FAIL (2): goal_w{{{gw}}} is not "
                  f"BUILDER_POOL_GOAL_PILL_BONUS({G.GOAL_PILL_BONUS}).")
            return 1
        print(f"  2 OK: hand-checkable WITH the bonus -- 30 x missing"
              f"({missing}) = {value:.0f} - 0.25 x trip({trip}t) = {cost:.0f} "
              f"-> bp_raw {raw:.0f} x goal_w {gw} = score {score:.0f}")
    else:
        if gw != 1.0 or "goal_w{" in first["terms"]:
            print(f"FAIL (2): with BUILDER_POOL_GOAL_PILL_BONUS=1.0 no row may "
                  f"print a goal_w chip at all:\n   {first['terms']}")
            return 1
        if "goal_w{" in text:
            bad = next(l.strip() for l in text.splitlines() if "goal_w{" in l)
            print(f"FAIL (2): a goal_w chip appears somewhere in the control's "
                  f"log:\n   {bad}")
            return 1
        print(f"  2 OK: hand-checkable with NO bonus -- 30 x missing"
              f"({missing}) = {value:.0f} - 0.25 x trip({trip}t) = {cost:.0f} "
              f"-> score {score:.0f}, and no goal_w chip anywhere in the run")

    # 3. The other pill is the SECOND errand, when there is one: the arena is
    #    an ORDER, not a preference, and both jobs are worth doing.
    later = [d for d in disp[1:] if d["target"] != first["target"]]
    if later:
        print(f"  3 OK: the other pill {later[0]['target']} was dispatched "
              f"afterwards at t={later[0]['t']} (score "
              f"{later[0]['score']:.0f}) -- an ORDER, not an exclusion")
    else:
        print("  3 --: only one errand fitted in the run; the order claim "
              "rests on the first dispatch alone")

    rows = read_jsonl(build_dir, sess)
    rc |= check_man_out(4, rows, first["t"], first["target"])
    rc |= repair_landed(5, read_trace(build_dir, variant), first["target"])
    if rc:
        return rc
    if expect_bonus:
        print("PASS (B): with the two rows equal to the tick, the tank goal's "
              "own pill won on goal_w{1.20} alone, and the line it won on says "
              "so in numbers a reader can re-multiply.")
    else:
        print("PASS (B2): with the bonus at 1.0 the same two rows tie and the "
              "tile key decides -- the OTHER pill goes first, and no goal_w "
              "chip is printed anywhere. That is what makes arena B's result "
              "about the bonus.")
    return 0


def check_B(sess, text, build_dir):
    return _check_B(sess, text, build_dir, "B", G.B_GOAL_PILL, True)


def check_B2(sess, text, build_dir):
    return _check_B(sess, text, build_dir, "B2", G.B_OTHER_PILL, False)


CHECKS = {"A": check_A, "A2": check_A2, "B": check_B, "B2": check_B2}


def run(variant, ticks, build_dir):
    print(f"=== arena {variant} ({ticks} engine ticks, tokens: "
          f"{TOKENS[variant]}) ===")
    sess, text = run_sim(variant, ticks, build_dir)
    if sess is None:
        print(f"FAIL: {text}")
        return 1
    print(f"  session: {sess.name}")
    errs = lua_errors(text)
    if errs:
        print("FAIL: Lua errors in our bot's log:")
        for e in errs:
            print("   " + e.strip())
        return 1
    return CHECKS[variant](sess, text, build_dir)


def main():
    variant, ticks, build = "all", None, DEFAULT_BUILD
    args = sys.argv[1:]
    take_asap_flag(args)
    print(pacing_line(""))
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            variant = args[i + 1]; i += 2
        elif args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        else:
            i += 1
    variants = list(CHECKS) if variant == "all" else variant.split(",")
    unknown = [v for v in variants if v not in CHECKS]
    if unknown:
        print(f"unknown arena(s): {', '.join(unknown)} "
              f"(have: {', '.join(CHECKS)})")
        sys.exit(2)
    rc = 0
    for v in variants:
        try:
            rc |= run(v, ticks or TICKS[v], build)
        except subprocess.TimeoutExpired:
            print(f"FAIL ({v}): run timed out")
            rc = 1
    sys.exit(rc)


if __name__ == "__main__":
    main()
