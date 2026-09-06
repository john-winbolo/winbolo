#!/usr/bin/env python3
"""repair priority — damage first, then walking TIME, and nothing else
(GoalHunter 1.7 builder pool).

THE RULE (author, 2026-09-05, verbatim)
---------------------------------------
"We need to rebuild the score for repairing pills. it should linearly scale so
closer gets priority, but damage is the most important, up to 11 tiles. The
danger on the tile should not matter. The time to get there is what it should
count for 'distance' (close with lots of swamp in between will be slower to
repair than far but all road) -- it should easily beat any harvest trees, unless
you don't have enough trees to repair of course."

...and, the same evening: "No path safety. Send that LGM out!"

builder_pool.lua's rebuild/topup rows are therefore priced

    score = BUILDER_POOL_REPAIR_HP_W(30) x missing_hp
          - BUILDER_POOL_REPAIR_TRIP_W(0.25) x round_trip_ticks

with no base constant, no front-line clock, no threat term and no path-safety
gate, out to BUILDER_POOL_REPAIR_LEASH (11 tiles, Manhattan).  The farm row is
untouched, and so is every OTHER refusal: tree_reserve, ally_repairing,
ally_capturing, mode_owned, fire_exchange, under_fire, the reserve ETA,
out_of_leash, unreachable and MIN_SCORE all still apply.
BUILDER_POOL_REPAIR_LINEAR=false (preset=keel) puts the old formula, the old
8-tile leash and the path gate back.

SIX RUNS, FIVE CLAIMS (tests/generate_repair_priority_map.py builds the ground;
its docstring carries the geometry and the danger arithmetic).

  A  DAMAGE BEATS DISTANCE.  A 4-hp top-up two tiles away and a DEAD pill nine
     tiles away, both affordable, both live candidates on the same tick.  The
     man must walk PAST the near job (120 - ~13 = ~107) to the far one
     (450 - ~77 = ~373), then come back for it.  Asserted on the ORDER of the
     two dispatches and on the corpse's dispatch being the LONGER walk of the
     two -- "it went to the far one" only means something if it really was
     further.

  B  TIME, NOT TILES.  Two dead pills: five tiles west behind four tiles of
     swamp, eight tiles east on road.  The east one must go first, and its
     printed trip must be SHORTER in ticks while being LONGER in tiles.  That
     pair of facts on the same run is the whole of the author's swamp/road
     sentence.

  C  REPAIR BEATS FARM.  A DEAD pill two tiles east with the forest right
     beside it -- THE SAME WALK -- and the farm row pumped to a value of 99,
     the most a farm can be worth at any tree count where a repair is still
     affordable (a top-up needs trees - reserve >= 1, i.e. 5 trees, and
     15 + 12 x (12 - 5) = 99).  On equal ground the value gap decides: 429
     against 41, and the repair goes first with the farm a live, fireable
     candidate alongside.

     WHAT THIS DOES NOT CLAIM, because it is not true: that a repair beats a
     farm at ANY distance.  The two rows pay DIFFERENT trip weights -- a repair
     pays BUILDER_POOL_REPAIR_TRIP_W (0.25), a farm still pays the old
     BUILDER_POOL_TRIP_W (0.5) -- so a forest under the tank's tracks
     (99 - 0.5 x 52 = 73) beats a 4-hp top-up more than ~188 round-trip ticks
     away (120 - 0.25 x 188 = 73), about five tiles of grass.  An early version
     of this arena had the forest at two tiles and the pill at six, and the
     farm won it 73 to 59 -- correctly.

  C2 THE TREE GATE.  Same ground, plus cfg=BUILDER_POOL_TREES_REBUILD=99 --
     "this repair costs more wood than you have", which is the author's "unless
     you don't have enough trees to repair" said through the gate the pool
     actually uses.  Now the repair must be REFUSED `tree_reserve(...)` and the
     farm must be what runs.  This is the boundary of claim C, and without it C
     only says "repair beat farm once".

  D  NO PATH SAFETY.  A dead pill seven tiles west, sitting in the danger field
     of four neutral pillboxes on an island the tank cannot reach.  The man must
     be dispatched anyway and `path_unsafe` must never appear.

  D2 THE CONTROL.  Identical ground, one token different
     (cfg=BUILDER_POOL_REPAIR_LINEAR=false).  The old formula must refuse the
     same corpse `path_unsafe`.  This is what makes D's silence evidence about
     the rule rather than about an arena that was never dangerous.

WHY THE cfg= TOKENS.  These arenas have to keep the TANK out of the experiment,
because a tank that drives to the pill fixes it through the repair feeder (which
is exempt from most of the stack by construction) and measures nothing about the
pool.  capture_pill and repair_pill are therefore priced past the 1e29
"unaffordable" line, and reposition is turned off wherever an ALIVE friendly
pill is on the map.  BRAIN_INIT_ARG is 127 bytes (BotInitSlot.arg,
luabrainshandler.h) and a longer string is truncated MID-TOKEN with one
"[cfg] BAD TOKEN" line, so ARG_MAX below asserts it rather than leaving it to
be rediscovered.

Every outcome is read from print2 or from the sidecar's ENGINE-side armour
trace, never from the brain's opinion of itself.

Usage: python repair_priority_test.py [--variant A|B|C|C2|D|D2|all] [--ticks N]
                                      [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
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
import generate_repair_priority_map as G   # noqa: E402

PORTS = {"A": 50251, "B": 50252, "C": 50253, "C2": 50254, "D": 50255,
         "D2": 50256}
# ENGINE ticks.  The brain thinks once per 20 ms frame and the sim advances two
# engine ticks per frame, so these are ~half as many brain ticks.  Arena A and B
# need room for TWO complete errands (out, build, back) one after the other.
TICKS = {"A": 4000, "B": 5000, "C": 3000, "C2": 2500, "D": 1600, "D2": 1600}

ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h
# NO SEEDING.  A row the TANK's own goal hands to the man (builder_pool.seed_job)
# sorts to the FRONT of the pool regardless of score -- deliberately, it is the
# goal that owns the man -- so any arena about the pool's ORDERING has to have no
# feeder running.  Two goals feed it and both have to go:
#   repair_pill      REPAIR_BASE_COST past the 1e29 "unaffordable" line;
#   defend_pill+repair  DEFEND_REPAIR_COST is the flat 40 an ARRIVED defender
#       bids for "I am parked beside a chewed-up pill with the LGM aboard", and
#       it is the ONLY defend rung that sets goal.repair (the thing the feeder
#       keys on).  Priced out, the defender falls back to its 30-cost WATCH bid,
#       which seeds nothing and leaves defend_pill in the pool's travel class --
#       so the man is still allowed out, he is just not handed a job.
# Measured the hard way: arena A's first run dispatched the two-tile top-up at
# t=104 with `seeded_by=defend_repair` and finalize_pools showing
# `defend_pill(40)`, while the corpse -- correctly scored 382 against its 137 --
# went second.
NO_SEED = "cfg=REPAIR_BASE_COST=1e30;cfg=DEFEND_REPAIR_COST=1e30"
NO_CAPTURE = "cfg=CAPTURE_PILL_BASE_COST=1e30"
NO_REPOS = "cfg=PILL_REPOSITION_ENABLED=false"
# The farm row at its 99 ceiling, with the 40 trees the tank always starts with
# standing in for a 5-tree woodpile: discovery only offers a farm row while
# trees < TREE_OPPORTUNISTIC_MAX, and the value would otherwise be a flat 15.
FARM_ON = "cfg=TREE_OPPORTUNISTIC_MAX=41;cfg=BUILDER_POOL_VALUE_FARM=99"
TOKENS = {
    "A":  ";".join([NO_CAPTURE, NO_SEED, NO_REPOS]),
    "B":  ";".join([NO_CAPTURE, NO_SEED]),
    "C":  ";".join([NO_CAPTURE, "cfg=REPAIR_BASE_COST=1e30", FARM_ON]),
    # C2 needs NO goal priced out beyond capture: TREES_REBUILD=99 is checked
    # on a seeded row too (a seed waives the reserve, never `have >= need`).
    "C2": ";".join([NO_CAPTURE, FARM_ON, "cfg=BUILDER_POOL_TREES_REBUILD=99"]),
    # D's island is unreachable, so attack_pill would price at INF anyway -- but
    # a pill can be SHELLED from the shore, so the goal is priced out explicitly.
    # Seeding is NOT suppressed here: a seeded row still runs every gate in
    # score_row, so the path-safety question is asked either way.
    "D":  ";".join([NO_CAPTURE, "cfg=ATTACK_PILL_BASE_COST=1e30"]),
    "D2": ";".join([NO_CAPTURE, "cfg=ATTACK_PILL_BASE_COST=1e30",
                    "cfg=BUILDER_POOL_REPAIR_LINEAR=false"]),
}
# -gametype per arena, and it is "open" everywhere -- BUT NOTE that the value
# barely matters: a map with a scenario sidecar is force-switched to
# gameScripted when it is committed (server_sim_accessors.c), and gameScripted
# hands out TANK_FULL_TREES (40) exactly like Open.  `-gametype tournament`,
# which really would start the tank on zero trees, is silently ignored on every
# arena in tests/ for that reason -- measured on arena C, which still logged
# trees=40/res=4 on its first pool tick.  That is why the tree gate is tested
# with a cfg= knob and not with an empty woodpile.
GAMETYPE = {"A": "open", "B": "open", "C": "open", "C2": "open",
              "D": "open", "D2": "open"}

# ── print2 lines (builder_pool.lua) ───────────────────────────────────────
# BP_DISPATCH t=453 job=rebuild target=(121,126) score=373 (score 373 = hp_w(30)
#   x missing(15) = 450 - trip_w(0.25) x trip(308t) = 77) [linear] eta=144
#   trip=308 trees=40-4 front=12 owner=.. claim=..
DISP_RE = re.compile(
    r"BP_DISPATCH t=(\d+) job=(\S+) target=\((\d+),(\d+)\)(.*?) score=(-?[\d.]+) "
    r"\((.*?)\)(?: \[linear\])? "
    r"eta=(\S+) trip=(\S+) trees=(\d+)-(\d+) front=(-?\d+)")
LINEAR_RE = re.compile(
    r"score (-?[\d.]+) = hp_w\((\d+)\) x missing\((\d+)\) = (-?[\d.]+) - "
    r"trip_w\(([\d.]+)\) x trip\((\d+)t\) = (-?[\d.]+)")
DENY_RE = re.compile(
    r"BP_DENY t=(\d+) job=(\S+) target=\((\d+),(\d+)\) reason=(.*?) elig=(.*?) "
    r"score=(-?[\d.]+) trip=(\S+)")
POOL_RE = re.compile(
    r"BUILDER_POOL t=(\d+) owner=(\S+) elig=(.*?) cands=(\d+) ok=(\d+) ")
POOL_TREES_RE = re.compile(
    r"BUILDER_POOL t=(\d+) owner=(\S+) elig=(.*?) cands=(\d+) ok=(\d+) "
    r"trees=(\d+)/res=(\d+)")
DONE_RE = re.compile(
    r"BP_DONE t=(\d+) job=(\S+) target=\((\d+),(\d+)\) outcome=(\S+) took=(\d+)t")


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
    """The sidecar's engine-side view: [(tick, x, y, armour), ...]."""
    path = build_dir / f"repair_priority_{variant}_trace.log"
    seq = []
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 4:
                seq.append(tuple(int(p) for p in parts[:4]))
    return seq


def linear_terms(terms):
    """(missing, value, trip, cost) off a linear BP_DISPATCH breakdown, or None.

    Also re-derives the arithmetic: the standing rule for these lines is that
    every factor is ON the line, so a reader can hand-check the score without
    opening constants.lua.  If the products do not close, the line is lying and
    the arenas below are reading a number nobody can reproduce."""
    m = LINEAR_RE.match(terms)
    if not m:
        return None
    score, hp_w, missing, value, trip_w, trip, cost = (
        float(m.group(1)), float(m.group(2)), int(m.group(3)),
        float(m.group(4)), float(m.group(5)), int(m.group(6)),
        float(m.group(7)))
    if abs(hp_w * missing - value) > 0.51:
        return None
    if abs(trip_w * trip - cost) > 0.51:
        return None
    if abs(score - (value - cost)) > 0.51:
        return None
    return missing, value, trip, cost


def run_sim(variant, ticks, build_dir):
    """Runs the arena; returns (session_dir, our_log_text) or (None, msg)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"
    subprocess.run([sys.executable,
                    str(HERE / "generate_repair_priority_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"repairprio_{variant}"
    final = HERE / f"repair_priority_{variant}_final.json"
    stderr = HERE / f"repair_priority_{variant}_stderr.txt"
    trace = build_dir / f"repair_priority_{variant}_trace.log"
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
    cmd = [str(ds), "-map", str(HERE / f"repair_priority_{variant}.map"),
           "-port", str(PORTS[variant]), "-nolobby", "-gametype", GAMETYPE[variant],
           "-bots", "1", "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN}[{tokens}]",
           # yesfull: three-row corridors, and the experiment is about WHICH
           # errand the man is spent on, not about finding the pills.
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
    """Ticks on which the pool had more than one row to choose between.

    Every arena here is a COMPARISON, so "it picked X" is worth nothing unless
    Y was on the table at the same moment."""
    return {int(t): int(c) for (t, _o, _e, c, _ok) in POOL_RE.findall(text)
            if int(c) >= 2}


def dumps(text, needle, n=6, tail=False):
    lines = [l.strip() for l in text.splitlines() if needle in l]
    for ln in (lines[-n:] if tail else lines[:n]):
        print("   " + ln)


# ── arena A: damage beats distance ───────────────────────────────────────
def check_A(sess, text, build_dir):
    disp = DISP_RE.findall(text)
    multi = multi_candidate_ticks(text)
    corpse, topup = G.A_CORPSE, G.A_TOPUP

    if not multi:
        print("FAIL (0): the pool never had two candidates on one tick, so "
              "nothing was ever compared. Both pills should be rows from the "
              "first ticks -- check the sidecar owned them (grep the stderr).")
        dumps(text, "BUILDER_POOL")
        return 1
    print(f"  0 OK: {len(multi)} tick(s) with 2+ candidates on the table")

    if not disp:
        print("FAIL (1): the man was never dispatched at all.")
        dumps(text, "BP_DENY", tail=True)
        return 1
    first = disp[0]
    if (int(first[2]), int(first[3])) != corpse:
        print(f"FAIL (1): the FIRST dispatch went to ({first[2]},{first[3]}) "
              f"job={first[1]}, not to the corpse at {corpse}. Damage did not "
              f"beat distance -- the man took the easy two-tile job first.")
        dumps(text, "BP_DISPATCH")
        return 1
    if int(first[0]) not in multi:
        print(f"FAIL (1): the corpse was dispatched at t={first[0]}, but the "
              f"pool line for that tick does not show 2+ candidates -- the "
              f"top-up was not on the table, so nothing was outranked.")
        return 1
    t_corpse = linear_terms(first[6])
    if not t_corpse:
        print(f"FAIL (1): the corpse's dispatch line is not a linear breakdown "
              f"that adds up: '{first[6]}'. Either BUILDER_POOL_REPAIR_LINEAR "
              f"is off, or M.score_terms changed without this test.")
        return 1
    print(f"  1 OK: first dispatch t={first[0]} job={first[1]} target={corpse} "
          f"-- missing {t_corpse[0]} x 30 = {t_corpse[1]:.0f}, trip "
          f"{t_corpse[2]}t costs {t_corpse[3]:.0f}, score {first[5]}; "
          f"{multi[int(first[0])]} candidates on that tick")

    later = [d for d in disp if (int(d[2]), int(d[3])) == topup]
    if not later:
        print(f"FAIL (2): the near top-up at {topup} was never done. The man "
              f"walked past it and never came back -- {len(disp)} dispatch(es) "
              f"total. (If the run simply ran out of ticks, raise TICKS['A'].)")
        dumps(text, "BP_DENY", tail=True)
        return 1
    t_topup = linear_terms(later[0][6])
    if not t_topup:
        print(f"FAIL (2): the top-up's breakdown does not add up: "
              f"'{later[0][6]}'")
        return 1
    print(f"  2 OK: the top-up followed at t={later[0][0]} -- missing "
          f"{t_topup[0]} x 30 = {t_topup[1]:.0f}, trip {t_topup[2]}t costs "
          f"{t_topup[3]:.0f}, score {later[0][5]}")

    # 3. ...and the far job really WAS the longer walk. Without this the order
    #    could just be two nearby jobs done in an arbitrary order.
    if t_corpse[2] <= t_topup[2]:
        print(f"FAIL (3): the corpse's trip ({t_corpse[2]}t) was not longer "
              f"than the top-up's ({t_topup[2]}t) -- the tank must have "
              f"wandered, so 'damage beat distance' is not what was measured.")
        return 1
    print(f"  3 OK: the corpse was the LONGER walk ({t_corpse[2]}t vs "
          f"{t_topup[2]}t) and still went first -- 30 x {t_corpse[0]} beat "
          f"30 x {t_topup[0]} by more than 0.25 x the extra "
          f"{t_corpse[2] - t_topup[2]}t")

    trace = read_trace(build_dir, "A")
    rose = [(t, x, y, a) for (t, x, y, a) in trace if (x, y) == corpse and a > 0]
    if not rose:
        print("FAIL (4): the engine never saw the corpse's armour come off 0. "
              "Trace: " + ", ".join(f"{t}:({x},{y})={a}"
                                    for t, x, y, a in trace[:12]))
        dumps(text, "BP_ABORT")
        return 1
    print(f"  4 OK: engine says the corpse came back up at sim t={rose[0][0]} "
          f"to {rose[0][3]}/{G.PILLS_MAX_HEALTH}")
    print("PASS (A): the man walked past a three-tile errand to rebuild a "
          "nine-tile corpse, then came back for the errand.")
    return 0


# ── arena B: time, not tiles ─────────────────────────────────────────────
def check_B(sess, text, build_dir):
    disp = DISP_RE.findall(text)
    multi = multi_candidate_ticks(text)
    road, swamp = G.B_ROAD_CORPSE, G.B_SWAMP_CORPSE
    d_road = G.mdist(G.B_SPAWN, road)
    d_swamp = G.mdist(G.B_SPAWN, swamp)

    if not multi:
        print("FAIL (0): the pool never had both corpses on one tick.")
        dumps(text, "BUILDER_POOL")
        return 1
    print(f"  0 OK: {len(multi)} tick(s) with 2+ candidates; from the spawn the "
          f"road corpse is {d_road} tiles away and the swamp corpse {d_swamp}")

    if not disp:
        print("FAIL (1): the man was never dispatched at all.")
        dumps(text, "BP_DENY", tail=True)
        return 1
    first = disp[0]
    if (int(first[2]), int(first[3])) != road:
        print(f"FAIL (1): the FIRST dispatch went to ({first[2]},{first[3]}), "
              f"not to the ROAD corpse at {road}. The pool ranked by tiles, not "
              f"by walking time.")
        dumps(text, "BP_DISPATCH")
        return 1
    if int(first[0]) not in multi:
        print(f"FAIL (1): dispatched at t={first[0]} with fewer than two "
              f"candidates -- the swamp corpse was not on the table.")
        return 1
    t_road = linear_terms(first[6])
    if not t_road:
        print(f"FAIL (1): the road corpse's breakdown does not add up: "
              f"'{first[6]}'")
        return 1
    print(f"  1 OK: first dispatch t={first[0]} to the ROAD corpse {road} -- "
          f"trip {t_road[2]}t, score {first[5]}")

    later = [d for d in disp if (int(d[2]), int(d[3])) == swamp]
    if not later:
        print(f"FAIL (2): the swamp corpse at {swamp} was never dispatched, so "
              f"its trip was never printed and the comparison is missing its "
              f"other half. (Most likely the tank drifted east and the row went "
              f"out_of_leash -- check the BP_DENY tail below -- or the run "
              f"needs more ticks.)")
        dumps(text, "BP_DENY", tail=True)
        return 1
    t_swamp = linear_terms(later[0][6])
    if not t_swamp:
        print(f"FAIL (2): the swamp corpse's breakdown does not add up: "
              f"'{later[0][6]}'")
        return 1
    print(f"  2 OK: the swamp corpse followed at t={later[0][0]} -- trip "
          f"{t_swamp[2]}t, score {later[0][5]}")

    # 3. THE CLAIM, both halves on one line: further in tiles, cheaper in ticks.
    if not (d_road > d_swamp and t_road[2] < t_swamp[2]):
        print(f"FAIL (3): the arena did not reproduce 'far but all road beats "
              f"close through swamp'. road: {d_road} tiles / {t_road[2]}t; "
              f"swamp: {d_swamp} tiles / {t_swamp[2]}t. The swamp band is "
              f"x{G.B_SWAMP_X[0]}..{G.B_SWAMP_X[1]} -- did the man find a way "
              f"around it?")
        return 1
    print(f"  3 OK: road corpse {d_road} tiles / {t_road[2]}t beat swamp corpse "
          f"{d_swamp} tiles / {t_swamp[2]}t -- {d_road - d_swamp} tiles "
          f"FURTHER and {t_swamp[2] - t_road[2]}t CHEAPER, and the trip term is "
          f"charged on the ticks")

    trace = read_trace(build_dir, "B")
    fixed = {(x, y) for (_t, x, y, a) in trace if a > 0}
    if road not in fixed:
        print("FAIL (4): the engine never saw the road corpse come off 0. "
              "Trace: " + ", ".join(f"{t}:({x},{y})={a}"
                                    for t, x, y, a in trace[:12]))
        return 1
    print(f"  4 OK: engine says {sorted(fixed)} came back up")
    print("PASS (B): the pool priced the walk in ticks, not tiles -- eight "
          "tiles of road beat five tiles of swamp.")
    return 0


# -- arena C: repair beats farm -------------------------------------------
def check_C(sess, text, build_dir):
    disp = DISP_RE.findall(text)
    multi = multi_candidate_ticks(text)
    corpse = G.C_CORPSE

    if not multi:
        print("FAIL (0): the pool never had two candidates on one tick -- the "
              "farm row and the rebuild have to be on the table together. If "
              "cands is always 1 the farm row is missing: discovery only offers "
              "one while trees < TREE_OPPORTUNISTIC_MAX, which is why the token "
              "raises it above the 40 the tank starts with.")
        dumps(text, "BUILDER_POOL")
        return 1
    print(f"  0 OK: {len(multi)} tick(s) with 2+ candidates")

    if not disp:
        print("FAIL (1): nothing was ever dispatched.")
        dumps(text, "BP_DENY", tail=True)
        return 1
    first = disp[0]
    if first[1] != "rebuild" or (int(first[2]), int(first[3])) != corpse:
        print(f"FAIL (1): the FIRST dispatch was job={first[1]} at "
              f"({first[2]},{first[3]}), not the rebuild at {corpse}. A repair "
              f"the bot can afford has to outrank the woodpile.")
        dumps(text, "BP_DISPATCH")
        return 1
    if int(first[0]) not in multi:
        print(f"FAIL (1): the rebuild went at t={first[0]} on a tick with fewer "
              f"than two candidates -- the farm row was not competing.")
        return 1
    t_rep = linear_terms(first[6])
    if not t_rep:
        print(f"FAIL (1): the rebuild's breakdown does not add up: '{first[6]}'")
        return 1
    print(f"  1 OK: first dispatch t={first[0]} job=rebuild target={corpse} -- "
          f"missing {t_rep[0]} x 30 = {t_rep[1]:.0f}, trip {t_rep[2]}t costs "
          f"{t_rep[3]:.0f}, score {first[5]}, with the farm row live alongside")

    # 2. ...and the farm row was genuinely FIREABLE, not merely present. A row
    #    that could never clear MIN_SCORE is not competition.
    farms = [d for d in disp if d[1] == "farm"]
    if not farms:
        print("FAIL (2): the farm row never fired even after the repair was "
              "done, so it may never have been able to. This arena needs the "
              "farm to be a real alternative that LOST, not a dead row. "
              "(cfg=BUILDER_POOL_VALUE_FARM=99 should put a two-tile forest at "
              f"99 - 0.5 x 116 = 41, well over MIN_SCORE {G.MIN_SCORE}.)")
        dumps(text, "BP_DENY", tail=True)
        return 1
    if int(farms[0][0]) <= int(first[0]):
        print(f"FAIL (2): a farm dispatch at t={farms[0][0]} came before the "
              f"repair at t={first[0]}.")
        return 1
    print(f"  2 OK: the farm row is fireable -- it went at t={farms[0][0]} "
          f"target=({farms[0][2]},{farms[0][3]}) score={farms[0][5]}, AFTER the "
          f"repair")

    trace = read_trace(build_dir, "C")
    rose = [(t, a) for (t, x, y, a) in trace if (x, y) == corpse and a > 0]
    if not rose:
        print("FAIL (3): the engine never saw the corpse's armour come off 0. "
              "Trace: " + ", ".join(f"{t}:{a}" for t, _x, _y, a in trace[:12]))
        dumps(text, "BP_ABORT")
        return 1
    print(f"  3 OK: engine says the corpse came back up at sim t={rose[0][0]} "
          f"to {rose[0][3] if False else rose[0][1]}/{G.PILLS_MAX_HEALTH}")
    print("PASS (C): a rebuild the bot could afford outranked the richest farm "
          "row a five-tree woodpile can produce, standing on the same ground.")
    return 0


# -- arena C2: the tree gate ----------------------------------------------
def check_C2(sess, text, build_dir):
    """cfg=BUILDER_POOL_TREES_REBUILD=99 -- "this repair costs more wood than
    you have", which is the author's "unless you don't have enough trees to
    repair" said through the gate the pool actually uses.

    Chosen over raising the RESERVE because it also survives SEEDING: a seeded
    row waives the whole reserve (seed_ctx.reserve = 0) but still has to satisfy
    `have >= need`, so this arena needs no goal priced out to keep the feeder
    from walking straight past the gate it is testing."""
    disp = DISP_RE.findall(text)
    deny = DENY_RE.findall(text)
    corpse = G.C_CORPSE

    refused = [d for d in deny if (int(d[2]), int(d[3])) == corpse
               and d[4].startswith("tree_reserve(")]
    if not refused:
        reasons = sorted({d[4] for d in deny})
        print("FAIL (1): the rebuild was never refused tree_reserve(...). "
              "Reasons seen: " + ", ".join(reasons)[:300])
        dumps(text, "BP_DENY")
        return 1
    print(f"  1 OK: BP_DENY t={refused[0][0]} target={corpse} "
          f"reason={refused[0][4]}")

    went = [d for d in disp if (int(d[2]), int(d[3])) == corpse]
    if went:
        print(f"FAIL (2): the man was dispatched to the corpse anyway at "
              f"t={went[0][0]} -- the tree gate did not hold. Dispatch line "
              f"seed field: '{went[0][4].strip()}'")
        return 1
    print(f"  2 OK: no dispatch to {corpse} in {len(disp)} dispatch(es)")

    farms = [d for d in disp if d[1] == "farm"]
    if not farms:
        print("FAIL (3): with the repair refused for lack of wood, the farm row "
              "should be what runs -- and nothing did. That is the 'unless you "
              "don't have enough trees' half of the rule.")
        dumps(text, "BP_DENY", tail=True)
        return 1
    print(f"  3 OK: the farm went instead -- t={farms[0][0]} "
          f"target=({farms[0][2]},{farms[0][3]}) score={farms[0][5]}")
    print("PASS (C2): no wood to spare, so the repair is refused by the tree "
          "gate and the man goes for wood -- the boundary of arena C's claim.")
    return 0


# ── arena D / D2: no path safety, and the control ────────────────────────
def _path_denies(text, corpse):
    return [d for d in DENY_RE.findall(text)
            if (int(d[2]), int(d[3])) == corpse and d[4].startswith("path_unsafe")]


def check_D(sess, text, build_dir):
    disp = DISP_RE.findall(text)
    corpse = G.D_CORPSE
    pool = POOL_RE.findall(text)

    if not pool:
        print("FAIL (0): no BUILDER_POOL verdicts at all.")
        return 1
    with_cands = [t for (t, _o, _e, c, _ok) in pool if int(c) > 0]
    if not with_cands:
        print(f"FAIL (0): the corpse at {corpse} was never a candidate row. "
              "Check the sidecar owned it (slot 0) and left the four island "
              "pills NEUTRAL.")
        dumps(text, "BUILDER_POOL")
        return 1
    print(f"  0 OK: {len(with_cands)} tick(s) with a live candidate")

    went = [d for d in disp if (int(d[2]), int(d[3])) == corpse]
    if not went:
        print(f"FAIL (1): the man was never dispatched to the corpse at "
              f"{corpse}. 'Send that LGM out' did not happen. Denials:")
        dumps(text, "BP_DENY", tail=True)
        return 1
    print(f"  1 OK: dispatched at t={went[0][0]} job={went[0][1]} "
          f"score={went[0][5]}")

    unsafe = _path_denies(text, corpse)
    if unsafe:
        print(f"FAIL (2): a path_unsafe refusal survived on a repair row -- "
              f"t={unsafe[0][0]} reason={unsafe[0][4]}. The gate is supposed to "
              f"be skipped entirely for rebuild/topup under the linear formula.")
        return 1
    print("  2 OK: not one path_unsafe refusal on the corpse in the whole run")

    # 3. The row shows no path term and no danger term at all -- the breakdown
    #    is two products, which is the whole of the formula.
    t = linear_terms(went[0][6])
    if not t:
        print(f"FAIL (3): the dispatch breakdown is not the two-term linear "
              f"shape: '{went[0][6]}'")
        return 1
    if "danger" in went[0][6] or "path" in went[0][6]:
        print(f"FAIL (3): the breakdown still names a danger/path term: "
              f"'{went[0][6]}'")
        return 1
    print(f"  3 OK: the row is damage and time only -- 30 x {t[0]} = {t[1]:.0f} "
          f"minus 0.25 x {t[2]}t = {t[3]:.0f}, score {went[0][5]}")
    print("PASS (D): the man was sent through a neutral pillbox's danger field "
          "because a repair row no longer asks about it.")
    return 0


def check_D2(sess, text, build_dir):
    corpse = G.D_CORPSE
    unsafe = _path_denies(text, corpse)
    disp = [d for d in DISP_RE.findall(text) if (int(d[2]), int(d[3])) == corpse]

    if not unsafe:
        reasons = sorted({d[4] for d in DENY_RE.findall(text)})
        print("FAIL (1): with cfg=BUILDER_POOL_REPAIR_LINEAR=false the OLD "
              "formula runs and lgm_path_safe_enhanced should refuse this "
              "corpse -- and it never did. That means arena D's silence proves "
              "nothing, because the ground was never dangerous enough. The "
              "generator asserts the danger arithmetic (the field at the "
              "corpse must exceed BUILDER_POOL_PATH_DANGER, 20); if that still "
              "holds, look at whether the tank drove close enough to make the "
              "walk short (dist <= 1 skips the line sampling). Reasons seen: "
              + ", ".join(reasons)[:300])
        dumps(text, "BP_DENY", tail=True)
        return 1
    print(f"  1 OK: BP_DENY t={unsafe[0][0]} target={corpse} "
          f"reason={unsafe[0][4]} -- the old formula refuses the same walk")

    if disp:
        # Not a failure on its own: the tank can shoot the island pills down
        # from the shore and make the corpse safe. What matters is that the
        # refusal happened FIRST.
        if int(disp[0][0]) < int(unsafe[0][0]):
            print(f"FAIL (2): a dispatch at t={disp[0][0]} came BEFORE the "
                  f"path_unsafe refusal at t={unsafe[0][0]} -- the control "
                  f"never actually held the man back.")
            return 1
        print(f"  2 OK: the refusal at t={unsafe[0][0]} came before the "
              f"eventual dispatch at t={disp[0][0]} (the danger can lift; the "
              f"gate firing at all is the point)")
    else:
        print("  2 OK: the man never went at all under the old formula")
    print("PASS (D2): the control reproduces the path_unsafe refusal, so arena "
          "D measured the rule change and not a quiet arena.")
    return 0


CHECKS = {"A": check_A, "B": check_B, "C": check_C, "C2": check_C2,
          "D": check_D, "D2": check_D2}


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
