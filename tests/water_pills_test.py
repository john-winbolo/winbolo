#!/usr/bin/env python3
"""Dead-pills-in-water pickup bot test.

Field incident 20260831_173448_1 bot3 t=6819: two dead allied pills sat in
deep sea off the shore; the bot had just respawned in a boat beside land, and
the goal pool never offered capture_pill for them (A* from BrainTest found
them fine). Generates the water-pills arena — an island with NO shallow water
(so no boat can be built), two dead pills of the bot's own team in deep sea,
and the bot spawning at sea (in a boat) beside the island — runs one
GoalHunter bot, and asserts it uses the boat it is in to collect both pills.

PASS if both pills end the run in the tank (final-state JSON: in_tank true,
owner = player 0) within the tick budget.

Companion to tests/cliff_staircase_test.py (same harness pattern; final
state via -finaljson, no debug streams needed).

Usage: python water_pills_test.py [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import os
import sys
import json
import subprocess
from pathlib import Path

# -asap by default: ticks run back-to-back instead of one per 20 ms of wall
# clock. Same seed -> byte-identical game, just faster. --no-asap (or
# WINBOLO_ASAP=0) puts this run back on the 20 ms live-game pacing.
from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
MAP = HERE / "water_pills.map"
FINAL = HERE / "water_pills_final.json"
SNAP = HERE / "water_pills_snap.jsonl"

PILLS = [(126, 136), (128, 136)]   # must match generate_water_pills_map.py PILLS


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def run(ticks, build_dir):
    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    subprocess.run([sys.executable, str(HERE / "generate_water_pills_map.py")],
                   check=True, stdout=subprocess.DEVNULL)
    for p in (FINAL, SNAP):
        if p.exists():
            p.unlink()

    cmd = [str(ds), "-map", str(MAP), "-port", "50045", "-nolobby",
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           # yesfull: the pills and the whole (tiny) arena are known from
           # tick 0 — the test is about reaching pills in water, not fog.
           "-ai", "yesfull",
           "-limit", "20",
           "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(FINAL),
           "-snapjson", str(SNAP), "-snapinterval", "600",
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    subprocess.run(cmd, cwd=str(build_dir),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=max(240, ticks // 40))

    if not FINAL.exists():
        print("FAIL: no final-state JSON produced")
        return 1
    d = json.load(open(FINAL))
    # A pill counts as COLLECTED when it is no longer lying dead on its sea
    # tile: it is in the tank, or it has been carried off and placed
    # somewhere else (the bot goes ashore and deploys them — normal play —
    # so "in_tank at the very end" is the wrong question).
    def still_in_water(px, py):
        for pb in d.get("pillboxes", []):
            if (pb.get("tx"), pb.get("ty")) == (px, py) \
                    and not pb.get("in_tank") and (pb.get("armor") or 0) == 0:
                return True
        return False
    collected = sum(1 for (x, y) in PILLS if not still_in_water(x, y))
    # Progress trace from the snapshots: first checkpoint each pill was seen in a tank.
    picked_at = {}
    if SNAP.exists():
        for line in open(SNAP):
            s = json.loads(line)
            for pb in s.get("pillboxes", []):
                if pb.get("in_tank"):
                    # pill positions move once carried; key by index order instead
                    idx = s["pillboxes"].index(pb)
                    if idx not in picked_at:
                        picked_at[idx] = s.get("tick")
    print(f"  pills collected from the water: {collected}/{len(PILLS)}")
    for i, (x, y) in enumerate(PILLS):
        t = picked_at.get(i)
        state = "still dead in the water" if still_in_water(x, y) else "collected"
        print(f"  pill ({x},{y}): {state}" + (f" (seen in tank by tick {t})" if t is not None else ""))
    if collected >= len(PILLS):
        print("PASS: bot sailed to both dead pills in deep sea and collected them.")
        return 0
    print("FAIL")
    return 1


def main():
    ticks = 6000
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
