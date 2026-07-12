#!/usr/bin/env python3
"""Lead-targeting (aim) test.

Generates the aim arena (generate_aim_map.py: a grass field, NO bases/pills, with
a single-tile deep-sea spawn pocket per tank so combat happens on land) and runs
TWO bots:
  * player 0 = GoalHunter (the shooter), spawned ~7 tiles south of the lane;
  * player 1 = tests/brains/drive_east.lua (the victim), patrolling east<->west
    across the shooter's front at full speed, never shooting.

The shooter must LEAD the moving target — aim ahead so the victim drives into the
shell. Directly exercises the tank-combat lead fix (and, on a LuaJIT build, the
two-arg math.atan/atan2 heading fix, without which the shooter can't aim at all).

The victim logs its armour every tick. On land (no boat to shoot out from under
it -> no drowning) and under the shooter's continuous fire (which outpaces the
tank's slow tree auto-repair), the minimum armour reached is a clean read of the
damage the lead-aim landed — identical on PUC-Lua and LuaJIT for a working aim.

  PASS  if the shooter deals >= AIM_MIN_DAMAGE armour to the crossing victim.
  FAIL  otherwise (never led the target / never engaged / brain crashed).

Usage:
    python aim_test.py [--ticks N] [--build DIR]
Exit code 0 on PASS, 1 on FAIL.
"""

import os
import sys
import glob
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
# Absolute (forward-slash) paths — the runner sets cwd=build_dir, so relative
# brain paths would resolve under build/ and silently fall back to the default.
SHOOTER_BRAIN = (REPO / "brains/GoalHunter_1.6/init.lua").as_posix()
VICTIM_BRAIN  = (REPO / "tests/brains/drive_east.lua").as_posix()
MAP = HERE / "aim_arena.map"

START_ARMOUR = 40   # TANK_FULL_ARMOUR
# Damage (armour points) the shooter must land on the crossing victim to prove it
# leads the target. Broken aim lands 0; a working lead-aim knocks off ~20-25 in a
# crossing. 10 sits comfortably between, so the test is a stable regression guard.
AIM_MIN_DAMAGE = 10


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
    return f"_aim_test_{build_tag(build_dir)}"


def find_ds(build_dir):
    for cand in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
                 build_dir / "Release" / "WinBoloDS.exe"):
        if cand.exists():
            return cand
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def read_victim_stats(build_dir, sess):
    """Return (max_damage, deaths, path) from victim_log.csv: max_damage is the
    biggest armour drop below full the shooter achieved; deaths is the '# end
    deaths=N' trailer (extra info). The victim writes to DEBUG_SESSION_DIR/
    victim_log.csv if set, else CWD (set to build_dir)."""
    import re
    for cand in ((sess / "victim_log.csv") if sess else None,
                 build_dir / "victim_log.csv"):
        if cand and cand.exists():
            path = cand
            break
    else:
        return None, None, None
    min_arm, deaths = START_ARMOUR, 0
    for line in path.read_text(errors="ignore").splitlines():
        m = re.search(r"deaths=(\d+)", line)
        if m:
            deaths = int(m.group(1))
        cols = line.split(",")
        if len(cols) >= 9 and cols[8].isdigit():
            min_arm = min(min_arm, int(cols[8]))
    return (START_ARMOUR - min_arm), deaths, path


def run(ticks, build_dir):
    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    subprocess.run([sys.executable, str(HERE / "generate_aim_map.py")],
                   check=True, stdout=subprocess.DEVNULL)
    # Clear a stale victim log so we never read a previous run's hits.
    for stale in (build_dir / "victim_log.csv",):
        if stale.exists():
            stale.unlink()

    label = session_label(build_dir)
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(MAP), "-port", "50043", "-nolobby",
           # 'open' so -teams makes the two bots enemies (Tournament overrides
           # teams). Tanks carry trees and slowly auto-repair, but the shooter's
           # continuous fire during a crossing outpaces it, so the minimum armour
           # reached is a clean read of the damage the lead-aim actually landed.
           "-gametype", "open", "-bots", "2",
           "-brain", SHOOTER_BRAIN,
           "-bot-init", f"0={SHOOTER_BRAIN},1={VICTIM_BRAIN}",
           "-teams", "2",                      # opposing teams so the shooter engages
           "-brain-debug", "-allow-unsafe-brains", "-seed", "42",
           "-ticks", str(ticks),
           "-nowinbolonet", "-quiet", "-threads", "1"]
    subprocess.run(cmd, cwd=str(build_dir), env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=max(240, ticks // 10))

    sess = newest_session(build_dir, label)
    if sess and list(sess.glob("brain_crash_*.log")):
        print(f"FAIL: brain crashed — see {sess}")
        return 1

    damage, deaths, path = read_victim_stats(build_dir, sess)
    if damage is None:
        print("FAIL: no victim_log.csv produced (victim didn't run?)")
        return 1

    print(f"  damage landed on crossing victim: {damage} armour   (deaths: {deaths})")
    print(f"  log: {path}")
    if damage >= AIM_MIN_DAMAGE:
        print(f"PASS: shooter led the moving target and dealt {damage} damage (>= {AIM_MIN_DAMAGE}).")
        return 0
    print(f"FAIL: only {damage} damage (< {AIM_MIN_DAMAGE}) — lead-aim missed the crossing target.")
    return 1


def main():
    ticks = 2200
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
