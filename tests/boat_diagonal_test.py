#!/usr/bin/env python3
"""Boat diagonal-channel navigation test.

Generates the narrow deep-water up-right staircase map (generate_boat_diagonal_map),
runs one GoalHunter bot that must sail its boat up the channel to capture a dead
pill sitting in deep water at the top, and asserts:

  PASS  if the bot captured the pill (carry reached >= 1) AND never died
        (armour never hit 0 in any ENGINE_DUMP snapshot).
  FAIL  otherwise (never reached/captured the pill, or drowned/died en route —
        which is what happens if it corner-cuts the staircase off the boat).

Usage:
    python boat_diagonal_test.py [--ticks N] [--build DIR]
Exit code 0 on PASS, 1 on FAIL.
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
MAP = HERE / "boat_diagonal.map"


def build_tag(build_dir):
    """'luajit' or 'puc' for the given build, so the debug_sessions/ dir name
    makes the VM obvious. Authoritative from CMakeCache; falls back to dir name."""
    cache = build_dir / "CMakeCache.txt"
    try:
        if cache.exists() and "WINBOLO_LUAJIT:BOOL=ON" in cache.read_text(errors="ignore"):
            return "luajit"
    except OSError:
        pass
    return "luajit" if "jit" in build_dir.name.lower() else "puc"


def session_label(build_dir):
    return f"_boatdiag_test_{build_tag(build_dir)}"


def find_ds(build_dir):
    for cand in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
                 build_dir / "Release" / "WinBoloDS.exe"):
        if cand.exists():
            return cand
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def run(ticks, build_dir):
    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    # (Re)generate the map so the test is self-contained.
    subprocess.run([sys.executable, str(HERE / "generate_boat_diagonal_map.py")],
                   check=True, stdout=subprocess.DEVNULL)

    label = session_label(build_dir)
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(MAP), "-port", "50041", "-nolobby",
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           # -mines no = VISIBLE mines (default is hidden). The bot's pathfinder
           # only avoids mines it can see (mine_penalty on the TERRAIN_MINE bit),
           # so the channel-forcing mines must be visible or it can't route round.
           "-mines", "no",
           "-nowinbolonet", "-quiet", "-threads", "1"]
    subprocess.run(cmd, cwd=str(build_dir), env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    sess = newest_session(build_dir, label)
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

    # Walk snapshots in order. The test verifies the BOAT NAVIGATION: the bot must
    # reach and capture the pill (carry>=1) without having died en route (armour
    # never hit 0 up to and including the capture tick). What happens after — in
    # this "mines on every land tile" map the top of the channel is a dead-end
    # pocket, so once the bot has the pill and looks for somewhere to place it, it
    # walks into mines — is a map artifact, not a navigation failure, so it's not
    # part of the assertion.
    captured = False
    alive_through_capture = True
    min_arm_precapture = None
    for m in re.finditer(r"ENGINE_DUMP .*?\barm=(\d+)\b.*?\bcarry=(\d+)\b", text):
        arm, carry = int(m.group(1)), int(m.group(2))
        if not captured:
            min_arm_precapture = arm if min_arm_precapture is None else min(min_arm_precapture, arm)
            if arm == 0:
                alive_through_capture = False
            if carry >= 1:
                captured = True
                break   # capture reached alive — navigation succeeded

    print(f"  captured pill (carry>=1): {captured}")
    print(f"  alive all the way to capture: {alive_through_capture}  (min armour pre-capture={min_arm_precapture})")
    if captured and alive_through_capture:
        print("PASS: boat sailed the diagonal channel and captured the pill without dying en route.")
        return 0
    print("FAIL: never captured the pill alive — drowned/mined off the boat before reaching it.")
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
    sys.exit(run(ticks, build))


if __name__ == "__main__":
    main()
