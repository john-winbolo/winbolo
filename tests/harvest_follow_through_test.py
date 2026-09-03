#!/usr/bin/env python3
"""Harvest FOLLOW-THROUGH test (GoalHunter 1.7).

"If you've harvested the spot to build a pill, follow through and build it."

Field incident 20260902_030233_1_16v17_20min, bot2:

    t=36981  HARVEST_SET tile=(109,123) score=311.3 origin=strategic -- the
             chosen pill spot is FOREST, so the engine rewrites the pill request
             into a tree harvest and the builder walks out to chop it.
    t=37011  the placement row DISAPPEARS from the pool: eval_place_pill_strategic
             is only "actionable" with the man IN the tank.  refuel_at_base 22
             tiles away wins the empty seat, and the tank drives off.
    t=37445  the man is back with the wood, the tile is re-scored FROM WHERE THE
             TANK NOW IS (the strategic centre, the spike base and the nearest
             hostile pill are all measured from the tank), 311.3 -> 181.7, and
             the paid-for harvest is thrown away: HARVEST_DROP worse_than_margin.

Two fixes, and this test covers both:

  * pool 8 keeps bidding while a harvest trip is live -- a FOLLOW_THROUGH row at
    the flat PLACE_FOLLOW_THROUGH_COST (25), whose whole job is to hold the tank
    within the builder's dispatch range of the trip tile;
  * the resume re-score is asked from the DISPATCH position, so the margin
    measures the tile changing rather than the tank moving.

The arena (tests/generate_harvest_follow_through_map.py +
harvest_follow_through.scenario.lua) is our half of a map where every placeable
tile is FOREST, so whichever tile the placement scan picks becomes a harvest
trip -- the test reads the tile off HARVEST_SET rather than predicting it.  The
opponent is an idle bot on an islet behind a full-width deep-sea channel: it
owns the northern base (a base must belong to a player before the brain stamps
hostile influence, and without a front line the portfolio has no most-needed
role to place for) and can do nothing else.

PASS requires all five:
  1. a HARVEST trip is set on a FOREST tile, origin=strategic;
  2. while the trip is live the tank never gets further from that tile than it
     was when the builder left (and no further than the hold radius + 3);
  3. the FOLLOW_THROUGH row is in the pool during the trip, priced at
     PLACE_FOLLOW_THROUGH_COST and pointing at the trip tile;
  4. the trip RESUMES (HARVEST_RESUME) -- no HARVEST_DROP for that tile;
  5. the pill actually lands there: PLACE_TRIP_END reason=placed, and the final
     JSON shows our pill on the tile, out of the tank.

Usage: python harvest_follow_through_test.py [--ticks N] [--build DIR]
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
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
MAP = HERE / "harvest_follow_through.map"
FINAL = HERE / "harvest_follow_through_final.json"
STDERR = HERE / "harvest_follow_through_stderr.txt"
LABEL = "harvest_ft_test"

sys.path.insert(0, str(HERE))
import generate_harvest_follow_through_map as gen        # noqa: E402

# constants.lua
FOLLOW_THROUGH_COST = 25       # M.PLACE_FOLLOW_THROUGH_COST
HOLD_DIST = 5                  # M.PLACE_FOLLOW_THROUGH_HOLD_DIST (= REPAIR_DISPATCH_DIST_BASE)
SLACK = 3                      # the brief's "dispatch range + 3"

# init.lua HARVEST_SET t=641 tile=(126,123) score=314.5 origin=strategic carried=2 trees=40
SET_RE = re.compile(
    r"HARVEST_SET t=(\d+) tile=\((\d+),(\d+)\) score=(\S+) origin=(\S+) "
    r"carried=(\d+) trees=(\d+)")
# init.lua HARVEST_RESUME t=721 tile=(126,123) score 314.5 -> 314.5 [travel-free ...] -> dispatch
RESUME_RE = re.compile(
    r"HARVEST_RESUME t=(\d+) tile=\((\d+),(\d+)\) score (\S+) -> (\S+)"
    r"(?: \[([^\]]*)\])?")
# init.lua HARVEST_DROP t=37445 tile=(109,123) reason=worse_than_margin ...
DROP_RE = re.compile(r"HARVEST_DROP t=(\d+) tile=\((\d+),(\d+)\) reason=(\S+)")
# init.lua PLACE_TRIP_END t=778 tile=(126,123) reason=placed after 57t
END_RE = re.compile(
    r"PLACE_TRIP_END t=(\d+) tile=\((\d+),(\d+)\) reason=(\S+) after (\d+)t")
# init.lua ENGINE_DUMP t=11 self=(117,121) dir=...
DUMP_RE = re.compile(r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\)")
# goals.lua   [1] place_pill_strategic@125,123 total=25.0 ... desc=FOLLOW_THROUGH
#   harvest@(126,123) score_at_dispatch=314 lgm=out cost{25} hold=(125,123)[stand_fast] d=1/5 ...
FT_ROW_RE = re.compile(
    r"\[(\d+)\] (\S+)@(\d+),(\d+) total=([\d.]+).*?"
    r"desc=FOLLOW_THROUGH harvest@\((\d+),(\d+)\) score_at_dispatch=(\S+) "
    r"lgm=out cost\{([\d.]+)\} hold=\((\d+),(\d+)\)\[(\w+)\] d=(\d+)/(\d+)")


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{LABEL}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def run(ticks, build_dir):
    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    subprocess.run([sys.executable,
                    str(HERE / "generate_harvest_follow_through_map.py")],
                   check=True, stdout=subprocess.DEVNULL)
    for p in (FINAL, STDERR):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked -- a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return 1

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=LABEL)
    cmd = [str(ds), "-map", str(MAP), "-port", "50061", "-nolobby",
           # open: the bot starts with wood aboard.  Out of wood it would take
           # the seek-trees redirect first, and this test is about the trip
           # AFTER the spot is chosen, not about getting to that point.
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           # yesfull: the whole arena is known from tick 0 -- including the
           # hostile base whose influence draws the front line the placement
           # scan needs.  Nothing here is a fog-of-war experiment.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(FINAL),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(STDERR, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 10))

    sess = newest_session(build_dir)
    if not sess:
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return 1
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed -- see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:2000])
        return 1
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return 1
    text = log.read_text(errors="ignore")
    terrain = gen.make_map()

    sets = SET_RE.findall(text)
    print(f"  session: {sess.name}")
    print(f"  HARVEST_SET: {len(sets)}; HARVEST_RESUME: "
          f"{len(RESUME_RE.findall(text))}; HARVEST_DROP: "
          f"{len(DROP_RE.findall(text))}")

    # ── 1. a strategic harvest trip on a forest tile ──────────────────
    strategic = [s for s in sets if s[4] == "strategic"]
    if not strategic:
        print("FAIL (1): no strategic HARVEST_SET. The bot never chose a forest "
              "tile for its pill, so there was no harvest to follow through on.")
        for ln in [l for l in text.splitlines()
                   if "PLACE_NO_SPOT" in l or "PLACE_HOLD" in l][:5]:
            print("   " + ln.strip())
        return 1
    s0 = strategic[0]
    t_set, tile = int(s0[0]), (int(s0[1]), int(s0[2]))
    if terrain[tile[1]][tile[0]] != gen.FOREST:
        print(f"FAIL (1): HARVEST_SET tile {tile} is not forest in the generated "
              f"map (terrain {terrain[tile[1]][tile[0]]}) -- the arena drifted.")
        return 1
    print(f"  1 OK: harvest trip set at t={t_set} on forest tile {tile} "
          f"(score {s0[3]}, carried {s0[5]}, trees {s0[6]})")

    # The window: from the dispatch to whichever ends the trip first.
    ends = [int(r[0]) for r in RESUME_RE.findall(text)
            if (int(r[1]), int(r[2])) == tile and int(r[0]) > t_set]
    ends += [int(d[0]) for d in DROP_RE.findall(text)
             if (int(d[1]), int(d[2])) == tile and int(d[0]) > t_set]
    if not ends:
        print(f"FAIL: the trip on {tile} never ended in the log -- no "
              f"HARVEST_RESUME and no HARVEST_DROP after t={t_set}.")
        return 1
    t_end = min(ends)

    # ── 2. the tank held station while the man was out ────────────────
    track = [(int(t), int(x), int(y)) for (t, x, y) in DUMP_RE.findall(text)
             if t_set <= int(t) <= t_end]
    if not track:
        print("FAIL (2): no ENGINE_DUMP lines inside the trip window -- cannot "
              "say where the tank went.")
        return 1
    d_at_set = mdist((track[0][1], track[0][2]), tile)
    worst = max(track, key=lambda s: mdist((s[1], s[2]), tile))
    d_worst = mdist((worst[1], worst[2]), tile)
    budget = max(d_at_set, HOLD_DIST) + SLACK
    print(f"  tank track over the trip [{t_set}..{t_end}]: {len(track)} sample(s), "
          f"{d_at_set} tiles from {tile} at dispatch, worst {d_worst} at "
          f"t={worst[0]} ({worst[1]},{worst[2]}), budget {budget}")
    if d_worst > budget:
        print(f"FAIL (2): the tank wandered {d_worst} tiles from the tile its "
              f"builder was chopping (budget {budget} = max(dispatch distance "
              f"{d_at_set}, hold {HOLD_DIST}) + {SLACK}). This is the incident: "
              f"something outbid the follow-through and took the tank away.")
        return 1
    # ...and it stayed on the BUILD, not merely near the tile. This is the half
    # of the incident that shows up even on a small arena: without the
    # follow-through row pool 8 goes silent the moment the man steps out, and
    # the bot spends the walk on whatever inherits the seat (explore here, a
    # 22-tile refuel in the field session). Distance alone would not catch it --
    # forest is slow, so an explore goal barely moves the tank in 75 ticks.
    goals = [(int(t), g) for (t, g) in
             re.findall(r"TICK_COST t=(\d+) .*? goal=(\S+)", text)
             if t_set <= int(t) <= t_end]
    on_build = [g for _, g in goals if g == "place_pill_strategic"]
    others = sorted({g for _, g in goals if g != "place_pill_strategic"})
    if goals and len(on_build) * 2 < len(goals):
        print(f"FAIL (2): the bot spent {len(goals) - len(on_build)}/{len(goals)} "
              f"ticks of its own harvest on something else ({', '.join(others)}). "
              f"The placement row went missing while the builder was out.")
        return 1
    print(f"  2 OK: the tank stayed within {d_worst} tiles of the trip tile for "
          f"the whole {t_end - t_set}-tick walk, on place_pill_strategic for "
          f"{len(on_build)}/{len(goals)} ticks"
          + (f" (also saw: {', '.join(others)})" if others else ""))

    # ── 3. ...because the FOLLOW_THROUGH row was in the pool ──────────
    rows = [m for m in FT_ROW_RE.finditer(text)]
    mine = [m for m in rows
            if (int(m.group(6)), int(m.group(7))) == tile]
    if not mine:
        print("FAIL (3): no FOLLOW_THROUGH row in FINAL_SCORES for this trip. "
              "Pool 8 went silent while the builder was out -- the placement "
              "row is gated on the man being IN the tank.")
        return 1
    bad = [m for m in mine if abs(float(m.group(9)) - FOLLOW_THROUGH_COST) > 0.01]
    if bad:
        print(f"FAIL (3): a FOLLOW_THROUGH row is not priced at "
              f"PLACE_FOLLOW_THROUGH_COST {FOLLOW_THROUGH_COST}: "
              f"cost{{{bad[0].group(9)}}}")
        return 1
    far = [m for m in mine if int(m.group(13)) > int(m.group(14))
           and m.group(12) == "stand_fast"]
    if far:
        print(f"FAIL (3): a stand_fast hold was offered from outside the hold "
              f"radius: d={far[0].group(13)}/{far[0].group(14)}")
        return 1
    holds = sorted({m.group(12) for m in mine})
    print(f"  3 OK: {len(mine)} FOLLOW_THROUGH row(s) at cost{{{FOLLOW_THROUGH_COST}}} "
          f"pointing at {tile}, hold source(s) {holds}; e.g.\n"
          f"     {mine[0].group(0)[:150]}")

    # ── 4. the trip resumed rather than being thrown away ─────────────
    dropped = [d for d in DROP_RE.findall(text)
               if (int(d[1]), int(d[2])) == tile and int(d[0]) == t_end]
    if dropped:
        print(f"FAIL (4): the harvest was thrown away -- HARVEST_DROP t="
              f"{dropped[0][0]} reason={dropped[0][3]}. The bot chopped the "
              f"trees and then refused to build on the tile it chopped.")
        for ln in [l for l in text.splitlines() if "HARVEST_DROP" in l][:3]:
            print("   " + ln.strip())
        return 1
    res = [r for r in RESUME_RE.findall(text)
           if (int(r[1]), int(r[2])) == tile and int(r[0]) == t_end]
    if not res:
        print("FAIL (4): the trip ended without a HARVEST_RESUME.")
        return 1
    r0 = res[0]
    print(f"  4 OK: HARVEST_RESUME t={r0[0]} score {r0[3]} -> {r0[4]}"
          + (f" [{r0[5]}]" if r0[5] else ""))

    # ── 5. and the pill actually landed on the tile ───────────────────
    placed = [e for e in END_RE.findall(text)
              if (int(e[1]), int(e[2])) == tile and e[3] == "placed"]
    if not placed:
        print("FAIL (5): no PLACE_TRIP_END reason=placed for the tile -- the "
              "resume dispatch never completed.")
        for ln in [l for l in text.splitlines() if "PLACE_TRIP_END" in l][:5]:
            print("   " + ln.strip())
        return 1
    if not FINAL.exists():
        print(f"FAIL (5): no {FINAL.name}")
        return 1
    import json
    final = json.loads(FINAL.read_text())
    on_tile = [p for p in final.get("pillboxes", [])
               if (p.get("tx"), p.get("ty")) == tile]
    if not on_tile or on_tile[0].get("in_tank") or on_tile[0].get("owner") != 0:
        print(f"FAIL (5): the final JSON has no fielded pill of ours on {tile}: "
              f"{on_tile or final.get('pillboxes')}")
        return 1
    print(f"  5 OK: PLACE_TRIP_END t={placed[0][0]} reason=placed after "
          f"{placed[0][4]}t; final JSON has our pill on {tile} "
          f"(armour {on_tile[0].get('armor')}, in_tank "
          f"{on_tile[0].get('in_tank')})")

    print("PASS: the bot harvested the tile it chose, held station while the "
          "builder was out, and built the pill it paid for.")
    return 0


def main():
    ticks = 2500
    build = DEFAULT_BUILD
    args = sys.argv[1:]
    take_asap_flag(args)      # consumes --asap / --no-asap
    print(pacing_line(""))
    i = 0
    while i < len(args):
        if args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        else:
            i += 1
    try:
        sys.exit(run(ticks, build))
    except subprocess.TimeoutExpired:
        print("FAIL: run timed out")
        sys.exit(1)


if __name__ == "__main__":
    main()
