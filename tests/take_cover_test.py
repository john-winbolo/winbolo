#!/usr/bin/env python3
"""take_cover goal test (GoalHunter pool 14).

Field incident 20260901_000042_1_loss_b6 bot3 t=16625: full tank, one tile
from a forest, parked inside an ANGRY hostile pill's fire for 130+ ticks with
its own pill being shot from 15 HP down to 1 — because no goal in the brain
was allowed to say "stand somewhere less lethal".  take_cover is that goal.

The arena (tests/generate_take_cover_map.py) puts the bot inside the crossfire
of three unreachable NEUTRAL pillboxes with the only safe ground to the south,
under one of its own pillboxes, and gives it nothing else worth doing.

PASS requires all four:
  1. take_cover became the bot's goal (GOAL_CHANGE trace) within the budget;
  2. it drove to a tile out of every neutral pill's danger disk (which is what
     threat.pill_at < TAKE_COVER_BAD_GROUND_PILL_AT means on this map — the
     pills are the only source of pill danger here);
  3. the TAKE_COVER scan line's own arithmetic closes: best - here == margin
     to within 0.1, on every line it printed;
  4. no brain crash.

Companion to tests/blocked_aim_test.py (same print2-log harness pattern).

Usage: python take_cover_test.py [--ticks N] [--build DIR]
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
MAP = HERE / "take_cover.map"
FINAL = HERE / "take_cover_final.json"
SNAP = HERE / "take_cover_snap.jsonl"
STDERR = HERE / "take_cover_stderr.txt"
LABEL = "take_cover_test"

sys.path.insert(0, str(HERE))
from generate_take_cover_map import (          # noqa: E402
    NEUTRAL_PILLS, SPAWN, EXPECT_PICK, BAD_GROUND, pill_at)

# TAKE_COVER t=123 at=(126,125) pick=(126,131) trig=bad_ground cost=51
#   here=-1.4 best=8.0 margin=9.4 travel=12 here_pill_at=50 best_pill_at=0
#   n=41 rej=13
TC_RE = re.compile(
    r"TAKE_COVER t=(\d+) at=\((\d+),(\d+)\) pick=\((\d+),(\d+)\) trig=(\S+) "
    r"cost=(\S+) here=(-?[\d.]+) best=(-?[\d.]+) margin=(-?[\d.]+) "
    r"travel=(-?[\d.]+) here_pill_at=(-?[\d.]+) best_pill_at=(-?[\d.]+) "
    r"n=(\d+) rej=(\d+)")
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)(?: #(-?\d+))? \((\d+),(\d+)\)")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{LABEL}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def out_of_pill_danger(mx, my):
    """True when the modelled threat.pill_at for this tile is below
    TAKE_COVER_BAD_GROUND_PILL_AT — the same line the brain calls bad ground.
    Modelled rather than read back so a position sampled from the snapshot
    JSON (which carries no brain state) can still be judged; the brain's own
    number is checked separately from the here_pill_at field it prints."""
    return pill_at(mx, my) < BAD_GROUND


def run(ticks, build_dir):
    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    subprocess.run([sys.executable, str(HERE / "generate_take_cover_map.py")],
                   check=True, stdout=subprocess.DEVNULL)
    for p in (FINAL, SNAP, STDERR):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked — a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return 1

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=LABEL)
    cmd = [str(ds), "-map", str(MAP), "-port", "50047", "-nolobby",
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           # yesfull: the whole (tiny) arena is known from tick 0 — the test is
           # about choosing where to stand, not about fog.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(FINAL),
           "-snapjson", str(SNAP), "-snapinterval", "100",
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(STDERR, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(240, ticks // 40))

    sess = newest_session(build_dir)
    if not sess:
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return 1
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed — see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:1500])
        return 1
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return 1
    text = log.read_text(errors="ignore")

    # ── 1. take_cover was actually chosen ──────────────────────────────
    goals = GOAL_RE.findall(text)
    kinds = [g[0] for g in goals]
    cover_goals = [g for g in goals if g[0] == "take_cover"]
    print(f"  goal changes: {len(goals)} ({', '.join(dict.fromkeys(kinds))})")
    if not cover_goals:
        print("FAIL: the bot never chose take_cover.")
        for line in text.splitlines():
            if "TAKE_COVER" in line:
                print("   " + line.strip())
                break
        return 1
    print(f"  take_cover chosen {len(cover_goals)}x, first at "
          f"({cover_goals[0][2]},{cover_goals[0][3]})")

    # ── 2. the scan lines' own arithmetic closes ───────────────────────
    rows = TC_RE.findall(text)
    if not rows:
        print("FAIL: no TAKE_COVER scan lines in the log")
        return 1
    bad = []
    for r in rows:
        here, best, margin = float(r[7]), float(r[8]), float(r[9])
        if abs((best - here) - margin) > 0.1:
            bad.append(r)
    print(f"  TAKE_COVER scan lines: {len(rows)}, "
          f"margin arithmetic consistent on {len(rows) - len(bad)}")
    print(f"  first scan: at=({rows[0][1]},{rows[0][2]}) "
          f"pick=({rows[0][3]},{rows[0][4]}) trig={rows[0][5]} cost={rows[0][6]} "
          f"here={rows[0][7]} best={rows[0][8]} margin={rows[0][9]} "
          f"here_pill_at={rows[0][11]} best_pill_at={rows[0][12]} "
          f"n={rows[0][13]} rej={rows[0][14]}")
    if bad:
        print(f"FAIL: {len(bad)} TAKE_COVER line(s) where best - here != margin, "
              f"e.g. {bad[0]}")
        return 1

    # The brain's own threat.pill_at at the spawn must agree with the model the
    # arena was designed against — otherwise the bad_ground trigger this test
    # thinks it is exercising is not the one that fired.
    spawn_rows = [r for r in rows if (int(r[1]), int(r[2])) == SPAWN]
    if spawn_rows:
        got, want = float(spawn_rows[0][11]), pill_at(*SPAWN)
        print(f"  brain threat.pill_at{SPAWN} = {got:.0f} "
              f"(arena model says {want:.0f}, bad ground is >= {BAD_GROUND})")
    triggers = sorted({r[5] for r in rows})
    print(f"  triggers seen: {', '.join(triggers)}")
    if not any(t.startswith("bad_ground") for t in triggers):
        print("FAIL: bad_ground never triggered — the arena is not putting the "
              "tank on ground the brain considers dangerous")
        return 1

    # ...and at least one scan taken FROM a low-danger tile, i.e. the brain
    # itself measured threat.pill_at < BAD_GROUND where it ended up standing.
    settled = [r for r in rows if float(r[11]) < BAD_GROUND]
    if not settled:
        print("FAIL: every TAKE_COVER scan was taken from bad ground — the bot "
              "never actually reached a tile with threat.pill_at < "
              f"{BAD_GROUND}")
        return 1
    print(f"  first scan from safe ground: t={settled[0][0]} "
          f"at=({settled[0][1]},{settled[0][2]}) "
          f"here_pill_at={settled[0][11]}")

    # ── 3. the bot actually got out of the pill danger ─────────────────
    tiles = []
    if SNAP.exists():
        for line in SNAP.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                s = json.loads(line)
            except ValueError:
                continue
            for t in s.get("tanks", []):
                if t.get("player") == 0:
                    tiles.append((s.get("tick"), t.get("tx"), t.get("ty")))
    if FINAL.exists():
        f = json.load(open(FINAL))
        for t in f.get("tanks", []):
            if t.get("player") == 0:
                tiles.append((f.get("tick"), t.get("tx"), t.get("ty")))
    safe = [(tk, x, y) for (tk, x, y) in tiles
            if x is not None and out_of_pill_danger(x, y)]
    print(f"  spawn {SPAWN} (in the crossfire); expected cover tile "
          f"{EXPECT_PICK}; sampled {len(tiles)} tank positions, "
          f"{len(safe)} out of pill danger")
    if not safe:
        print("FAIL: the bot never reached a tile outside the neutral pills' "
              "danger disk. Positions seen: "
              + ", ".join(f"({x},{y})@{tk}" for tk, x, y in tiles[:12]))
        return 1
    print(f"  first safe tile: ({safe[0][1]},{safe[0][2]}) at tick {safe[0][0]}")

    print("PASS: take_cover fired on bad ground, its printed margin reconciles, "
          "and the bot moved out of the pillbox crossfire.")
    return 0


def main():
    ticks = 1500
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
