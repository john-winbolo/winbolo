#!/usr/bin/env python3
"""Seek-trees test.

Generates the seek-trees map (dead pills piled on spawn, forest only in the far
north), runs one GoalHunter bot, and asserts that when it runs out of wood while
still carrying pills it explicitly TRAVELS to the distant forest and harvests —
rather than deadlocking on a build it can't pay for.

PASS if all of:
  * seek_trees goal fired (it recognized it must go get wood),
  * it reached the far forest (northmost position while carrying pills got up to
    the forest band), and
  * it harvested (trees dropped below the place cost while carrying, then climbed
    back to >= the cost afterward).

Usage: python seek_trees_test.py [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import os
import re
import sys
import glob
import subprocess
from pathlib import Path

import generate_seek_trees_map as gen

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.6" / "init.lua"
MAP = HERE / "seek_trees.map"
LABEL = "_seek_trees_test"
PLACE_COST = 4                    # PILL_PLACE_TREE_COST
FOREST_Y1 = gen.FOREST_BOX[3]     # south edge of the forest band


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
    subprocess.run([sys.executable, str(HERE / "generate_seek_trees_map.py")],
                   check=True, stdout=subprocess.DEVNULL)

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=LABEL)
    cmd = [str(ds), "-map", str(MAP), "-port", "50042", "-nolobby",
           # Strict Tournament: players spawn with NO trees, so the moment the bot
           # scoops a pill it's carry>=1 with tr=0 — exactly the out-of-wood state
           # that must drive it to travel for trees.
           "-gametype", "strict", "-bots", "1", "-brain", str(BRAIN),
           # Strict games default to ~1 min (~3000 ticks); raise the limit well
           # past our tick budget so -ticks is what actually bounds the run and the
           # bot has time to capture -> run dry -> travel to the far forest -> harvest.
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-nowinbolonet", "-quiet", "-threads", "1"]
    subprocess.run(cmd, cwd=str(build_dir), env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=max(240, ticks // 40))

    sess = newest_session(build_dir)
    if not sess:
        print("FAIL: no debug session produced")
        return 1
    if list(sess.glob("brain_crash_*.log")):
        print(f"FAIL: brain crashed — see {sess}")
        return 1
    log = sess / "print2_bot0.log"
    if not log.exists():
        print("FAIL: no bot0 log")
        return 1
    text = log.read_text(errors="ignore")

    sought = "seek_trees@" in text

    northmost_carrying = 999
    went_low = False           # trees < cost while carrying a pill
    harvested_after_low = False
    for m in re.finditer(
            r"ENGINE_DUMP .*?self=\((\d+),(\d+)\).*?\btr=(\d+)\b.*?\bcarry=(\d+)\b", text):
        sy = int(m.group(2)); tr = int(m.group(3)); carry = int(m.group(4))
        if carry >= 1:
            northmost_carrying = min(northmost_carrying, sy)
            if tr < PLACE_COST:
                went_low = True
        if went_low and tr >= PLACE_COST:
            harvested_after_low = True

    # The LGM harvests from a few tiles away (it walks to the forest), so the tank
    # sits ~LGM-deploy-distance short of the trees — "reached" means within that.
    reached_forest = northmost_carrying <= FOREST_Y1 + 10

    print(f"  seek_trees goal fired:        {sought}")
    print(f"  reached the far forest:       {reached_forest}  (northmost carrying y={northmost_carrying}, forest y<= {FOREST_Y1})")
    print(f"  ran out of wood then harvested: {harvested_after_low}  (went_low={went_low})")
    if sought and reached_forest and harvested_after_low:
        print("PASS: bot ran dry on wood, drove to the distant forest, and harvested to keep building.")
        return 0
    print("FAIL")
    return 1


def main():
    ticks = 8000
    build = DEFAULT_BUILD
    args = sys.argv[1:]
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
