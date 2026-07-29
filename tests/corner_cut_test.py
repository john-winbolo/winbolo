#!/usr/bin/env python3
"""Pathfinder corner-cut bot test.

Generates the corner-cut arena (the Gothic Industrial geometry from field
incident 20260713_233008_1 bot1 t=11467: a rubble pocket plus a halfbuilding
cluster between the bot's spawn at (112,141) and the neutral base at
(108,144)), runs one GoalHunter bot, and asserts it crosses the pocket
instead of planning a diagonal past the solid halfbuilding corner and
grinding against it at full throttle.

PASS if all of:
  * no wall grind — the tank never sits on the same tile at high commanded
    speed for GRIND_TICKS consecutive engine dumps (the incident signature:
    ~110 ticks at spd>=40 pushing into the halfbuilding corner), and
  * it actually reaches the base across the pocket (within 1 tile), proving
    the route is completed rather than merely avoided.

Companion to the headless unit tests (WinBoloUnitTests --test
pf_dijkstra_no_solid_corner_cut etc.), which pin the corner rule at the
pathfinder level; this test pins the end-to-end driving behavior.

Usage: python corner_cut_test.py [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import os
import re
import sys
import glob
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.6" / "init.lua"
MAP = HERE / "corner_cut.map"
LABEL = "_corner_cut_test"

BASE_X, BASE_Y = 122, 128      # must match generate_corner_cut_map.py BASE
GRIND_SPD = 40                 # commanded speed while not moving = grinding
GRIND_TICKS = 50               # incident showed ~110 ticks; half that = fail


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
    subprocess.run([sys.executable, str(HERE / "generate_corner_cut_map.py")],
                   check=True, stdout=subprocess.DEVNULL)

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=LABEL)
    cmd = [str(ds), "-map", str(MAP), "-port", "50043", "-nolobby",
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           # yesfull so the whole (tiny) arena is known terrain from tick 0 —
           # the test is about pathing over KNOWN rubble/halfbuildings, not
           # fog-of-war discovery.
           "-ai", "yesfull",
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

    # ── Wall-grind detector ──
    # Consecutive engine dumps on the SAME map tile with commanded speed
    # >= GRIND_SPD. Legitimate driving crosses a tile in well under 50
    # ticks at that speed; only pushing into something solid holds
    # (tile, high-speed) that long. The pre-fix incident held (112,141)
    # at spd 48-64 for ~110 ticks.
    worst_grind = 0
    worst_tile = None
    cur_tile = None
    cur_run = 0
    reached_base = False
    for m in re.finditer(
            r"ENGINE_DUMP .*?self=\((\d+),(\d+)\).*?\bspd=(\d+)\b", text):
        sx, sy, spd = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if abs(sx - BASE_X) <= 1 and abs(sy - BASE_Y) <= 1:
            reached_base = True
        if spd >= GRIND_SPD:
            if (sx, sy) == cur_tile:
                cur_run += 1
            else:
                cur_tile = (sx, sy)
                cur_run = 1
            if cur_run > worst_grind:
                worst_grind = cur_run
                worst_tile = cur_tile
        else:
            cur_tile = None
            cur_run = 0

    no_grind = worst_grind < GRIND_TICKS

    print(f"  no wall grind:   {no_grind}  "
          f"(worst {worst_grind} ticks at spd>={GRIND_SPD}"
          f"{' on ' + str(worst_tile) if worst_tile else ''})")
    print(f"  reached base:    {reached_base}  (within 1 tile of ({BASE_X},{BASE_Y}))")
    if no_grind and reached_base:
        print("PASS: bot crossed the rubble pocket to the base without grinding a solid corner.")
        return 0
    print("FAIL")
    return 1


def main():
    ticks = 4000
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
