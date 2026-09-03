#!/usr/bin/env python3
"""Dead-pill cluster discount + hostile-territory guard test (GoalHunter
capture_pill, Parts 2 and 3).

Field incident 20260901_160325_1_par2 bot3, t=17930-18600.  SIX dead pills sat
in one heap at (114-117,130-131) with no reject on any of them, and
capture_pill#2 still priced 234 -- every member was quoted the full ~11-tile
trip as if it were the only pill out there.  A heap is ONE errand: the tank is
already there for the first pill and the rest are a few tiles of walking.  So
each member now pays its share, and a heap sitting under enemy guns pays it
back (goals.lua capture_cluster_refresh):

    cost / min(n, CAPTURE_CLUSTER_DIVISOR_MAX)   floored at CAPTURE_CLUSTER_MIN_COST
         x (1 + CAPTURE_CLUSTER_GUARD_MULT * k)  capped at CAPTURE_CLUSTER_GUARD_MAX

k counts the live hostile/neutral pillboxes that can actually put a shell on a
cluster tile -- range AND a clear line of fire, the same test the sea harvest
uses, so a pillbox walled off from the heap does not count.

Three variants (tests/generate_capture_cluster_map.py), each one game-minute:

  A  Heap of five, 11 tiles east, unguarded, nothing else to do.
     PASS: the brain reports the cluster as n=5 /5X with guard 0, capture_pill
     wins, and all five pills are off their tiles by the end of the minute.

  B  Heap of five 40 tiles east, TWO neutral pillboxes covering it from
     unreachable islands, and a lone SAFE dead pill five tiles west.
     PASS: the brain reports guard 2 (x2.50), and the first capture the bot
     commits to is the SAFE pill -- the discount did not walk it into the
     fortress -- with no tank death.

  C  B's arena with the two guards deleted, and nothing else changed.
     PASS: guard 0, and now the first capture the bot commits to is a HEAP
     pill.  B and C together are the actual proof: the same bot, the same
     distances, the same discount, and the two pillboxes are the only thing
     that moved the decision.

Companion to tests/take_cover_test.py (same print2-log harness pattern).

Usage: python capture_cluster_test.py [--variant A|B|C|ALL] [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
import json
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

sys.path.insert(0, str(HERE))
from generate_capture_cluster_map import (       # noqa: E402
    cluster, guards, SAFE_PILL, SPAWN, DIVISOR_MAX, guard_mult)

PORTS = {"A": 50053, "B": 50054, "C": 50055}

# goals.lua capture_cluster_refresh:
#   CAPTURE_CLUSTER t=11 cluster#1 n=5 div=5 guards=2[6,7] guard_m=2.50
#     tiles=(165,126) (166,126) ...
CLUSTER_RE = re.compile(
    r"CAPTURE_CLUSTER t=(\d+) cluster#(\d+) n=(\d+) div=(\d+) "
    r"guards=(\d+)\[([^\]]*)\] guard_m=([\d.]+) tiles=(.*)")
# init.lua:  GOAL_CHANGE Goal: capture_pill #5 (121,126)
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)(?: #(-?\d+))? \((\d+),(\d+)\)")


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
    label = f"capture_cluster_{variant}"
    mapfile = HERE / f"capture_cluster_{variant}.map"
    final = HERE / f"capture_cluster_{variant}_final.json"
    stderr = HERE / f"capture_cluster_{variant}_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    subprocess.run([sys.executable, str(HERE / "generate_capture_cluster_map.py"),
                    variant], check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return 1

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORTS[variant]),
           "-nolobby", "-gametype", "open", "-bots", "1",
           "-brain", str(BRAIN),
           # yesfull: the whole arena is known from tick 0 - the test is about
           # how the heap is PRICED, not about finding it.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 10))

    sess = newest_session(build_dir, label)
    if not sess:
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return 1
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed - see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:1500])
        return 1
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return 1
    text = log.read_text(errors="ignore")
    print(f"  session: {sess.name}")

    heap = cluster(variant)
    want_k = len(guards(variant))

    # ── 1. The brain built the heap as ONE cluster, with the guard count
    #       this arena was designed to produce. ────────────────────────────
    rows = CLUSTER_RE.findall(text)
    if not rows:
        print("FAIL: the brain never reported a dead-pill cluster at all. "
              "Either build_pill_clusters split the heap (check the tiles are "
              "within CAPTURE_CLUSTER_RADIUS) or capture_cluster_refresh never "
              "ran (the pills must be DEAD and not deep-sea).")
        return 1
    r = rows[0]
    n, div, k, gids, gm = int(r[2]), int(r[3]), int(r[4]), r[5], float(r[6])
    tiles = set(re.findall(r"\((\d+),(\d+)\)", r[7]))
    tiles = {(int(a), int(b)) for a, b in tiles}
    print(f"  cluster: n={n} div={div} guards={k}[{gids}] guard_m={gm:.2f} "
          f"tiles={sorted(tiles)}")
    if n != len(heap) or tiles != set(heap):
        print(f"FAIL: the heap did not cluster as one. Expected n={len(heap)} "
              f"over {sorted(heap)}, got n={n} over {sorted(tiles)}.")
        return 1
    if div != min(len(heap), DIVISOR_MAX):
        print(f"FAIL: divisor {div}, expected min({len(heap)}, {DIVISOR_MAX})")
        return 1
    if k != want_k:
        print(f"FAIL: {k} covering pill(s), expected {want_k}. The guard scan "
              "reuses the sea harvest's coverage test (range + heat margin AND "
              "a clear line of fire) - check the arena's guard geometry.")
        return 1
    want_gm = guard_mult(want_k)
    if abs(gm - want_gm) > 0.01:
        print(f"FAIL: guard multiplier {gm:.2f}, expected "
              f"1 + 0.75 x {want_k} capped at 4.0 = {want_gm:.2f}")
        return 1
    print(f"  1 OK: cluster{{{n}}} /{div}X guard{{{k} pills, x{gm:.2f}}} "
          f"({len(rows)} refresh line(s))")

    # ── 2. What the bot actually committed to. ─────────────────────────────
    goals = GOAL_RE.findall(text)
    captures = [(int(g[2]), int(g[3])) for g in goals if g[0] == "capture_pill"]
    if not captures:
        print("FAIL: the bot never chose capture_pill at all. "
              f"Goals seen: {sorted({g[0] for g in goals})}")
        return 1
    first = captures[0]
    print(f"  first capture target: {first} "
          f"({len(captures)} capture_pill goal changes)")

    if variant == "A":
        if first not in heap:
            print(f"FAIL (A): the first capture was {first}, not a heap pill "
                  f"{sorted(heap)} - the discount did not make the heap the "
                  "obvious errand.")
            return 1
    elif variant == "B":
        if first != SAFE_PILL:
            print(f"FAIL (B): the first capture was {first}, not the safe pill "
                  f"{SAFE_PILL}. The guard surcharge (x{gm:.2f} on {k} covering "
                  "pill(s)) did not keep the discounted heap from winning while "
                  "a safe alternative was on the table.")
            return 1
    else:  # C - the control
        if first not in heap:
            print(f"FAIL (C): with the guards deleted the first capture was "
                  f"{first}, not a heap pill {sorted(heap)}. If C does not "
                  "flip, B proves nothing about the guard term - the safe pill "
                  "was simply cheaper all along.")
            return 1
    print(f"  2 OK: first commitment is "
          f"{'a heap pill' if first in heap else 'the safe pill'}, as required")

    # ── 3. Final state: what was collected, and did we survive it. ────────
    if not final.exists():
        print("FAIL: no final JSON")
        return 1
    f = json.load(open(final))
    still_dead = []
    for tile in heap:
        for p in f.get("pillboxes", []):
            if (p.get("tx"), p.get("ty")) == tile and p.get("armor") == 0 \
                    and not p.get("in_tank"):
                still_dead.append(tile)
    deaths = sum(t.get("deaths", 0) for t in f.get("tanks", []))
    print(f"  final: {len(heap) - len(still_dead)}/{len(heap)} heap pills off "
          f"their tiles, {deaths} tank death(s)")
    if deaths:
        print(f"FAIL: the bot died {deaths} time(s) - a discounted capture must "
              "not be a suicide run.")
        return 1
    if variant == "A" and still_dead:
        print(f"FAIL (A): {len(still_dead)} heap pill(s) still lying dead at "
              f"{still_dead} after the minute. The discount is supposed to make "
              "the whole heap one cheap errand.")
        return 1

    if variant == "A":
        print("PASS (A): the five-pill heap clustered, priced at a fifth of the "
              "trip with no guard surcharge, and the bot swept all five.")
    elif variant == "B":
        print(f"PASS (B): the heap clustered and picked up guard{{{k} pills, "
              f"x{gm:.2f}}}, and the bot took the safe pill first instead of "
              "driving into the guns.")
    else:
        print("PASS (C): the same arena with the guards removed flips back to "
              "the heap - the guard surcharge is what moved variant B.")
    return 0


def main():
    ticks = 6000
    build = DEFAULT_BUILD
    variants = ["A", "B", "C"]
    args = sys.argv[1:]
    take_asap_flag(args)      # consumes --asap / --no-asap
    print(pacing_line(""))
    i = 0
    while i < len(args):
        if args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        elif args[i] == "--variant":
            v = args[i + 1].upper(); i += 2
            variants = ["A", "B", "C"] if v == "ALL" else [v]
        else:
            i += 1
    rc = 0
    for v in variants:
        print(f"-- variant {v} " + "-" * 46)
        try:
            r = run_one(v, ticks, build)
        except subprocess.TimeoutExpired:
            print(f"FAIL ({v}): run timed out")
            r = 1
        rc = rc or r
    sys.exit(rc)


if __name__ == "__main__":
    main()
