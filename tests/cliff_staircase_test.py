#!/usr/bin/env python3
"""Cliff-staircase drowning bot test.

Generates the cliff-staircase arena (the DH-Oil Rig geometry from field
incident 20260831_113629_1 bot2 t=6081: a diagonal road staircase along a
deep-sea edge, where the pre-fix tank corner-cut into the deep tile
(142,112) and drowned), runs one GoalHunter bot, and asserts it makes the
whole trip.

PASS if all of:
  * no drowning — the tank's tile is never one of the deep-sea cut
    corners, and there is no long gap in the ENGINE_DUMP tick sequence
    (a dead brain stops ticking; deep-sea death is the only killer on
    this map). Merely being on a boat is NOT a death signal — the tank
    legitimately sails in, and may re-board its parked boat later. And
  * it reaches the base (within 1 tile) — which sits past a RIVER ford,
    so this also fails if a cliff-guard fix overshoots and makes the bot
    refuse legitimate passable water. Only deep sea kills; rivers must
    still be driven through.

Companion to tests/corner_cut_test.py (same harness pattern).

Usage: python cliff_staircase_test.py [--ticks N] [--build DIR]
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
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
MAP = HERE / "cliff_staircase.map"
LABEL = "_cliff_staircase_test"

BASE_X, BASE_Y = 119, 124      # must match generate_cliff_staircase_map.py BASE
CUT_CORNERS = {(128, 126), (127, 125), (126, 124)}   # ditto CUT_CORNERS
DEATH_GAP_TICKS = 150          # ENGINE_DUMP tick gap this long = the tank died


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
    subprocess.run([sys.executable, str(HERE / "generate_cliff_staircase_map.py")],
                   check=True, stdout=subprocess.DEVNULL)

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=LABEL)
    cmd = [str(ds), "-map", str(MAP), "-port", "50044", "-nolobby",
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           # yesfull so the staircase and the deep corners are known
           # terrain from tick 0 — the test is about steering along a
           # KNOWN deep-sea edge, not fog-of-war discovery.
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

    # ── Drown detector ──
    # The tank's tile landing on a deep-sea cut corner is the drowning
    # itself; a long ENGINE_DUMP tick gap is the same event seen from
    # the other side (a dead tank's brain stops ticking). Boat state is
    # NOT used: the tank sails in on a boat and may legitimately
    # re-board the parked boat later.
    landed = False
    drowned_at = None
    last_tick = None
    gap_at = None
    reached_base = False
    for m in re.finditer(
            r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\).*?\bboat=(true|false)\b",
            text):
        t, sx, sy = int(m.group(1)), int(m.group(2)), int(m.group(3))
        boat = m.group(4) == "true"
        if last_tick is not None and t - last_tick >= DEATH_GAP_TICKS \
                and gap_at is None:
            gap_at = (last_tick, t)
        last_tick = t
        if not landed and not boat:
            landed = True
        if (sx, sy) in CUT_CORNERS and drowned_at is None:
            drowned_at = (t, sx, sy)
        if abs(sx - BASE_X) <= 1 and abs(sy - BASE_Y) <= 1:
            reached_base = True

    no_drown = landed and drowned_at is None and gap_at is None

    print(f"  landed:          {landed}")
    print(f"  no drowning:     {drowned_at is None and gap_at is None}  "
          + (f"(in deep corner {drowned_at[1:]} at t={drowned_at[0]})"
             if drowned_at is not None
             else f"(tick gap {gap_at})" if gap_at is not None else ""))
    print(f"  reached base:    {reached_base}  "
          f"(within 1 tile of ({BASE_X},{BASE_Y}) — past the river ford)")
    if no_drown and reached_base:
        print("PASS: bot drove the deep-sea staircase without drowning "
              "and still forded the river to the base.")
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
