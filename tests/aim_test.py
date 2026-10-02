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

The metric is SHOT HIT RATE = hits / shells fired:
  * shells fired  = decrements in the shooter's per-tick shell count
                    (ENGINE_DUMP 'sh=' in print2_bot0.log); shells drop only on
                    OUR fires and there are no bases to refuel, so it's exact.
  * hits          = total armour the victim lost (sum of per-tick drops in
                    victim_log.csv) / DAMAGE-per-shell(5); robust to the victim's
                    slow auto-repair and to two shells landing in one tick.

This rewards fire discipline together with lead accuracy: a shooter that only
fires when the shot will connect scores high, while one that sprays out-of-range
or poorly-led shells tanks its percentage even if it eventually deals damage.

  PASS  if >= AIM_MIN_SHOTS shells were fired AND hit rate >= AIM_MIN_HIT_PCT.
  FAIL  otherwise (poor lead, firing out of range, never engaged, brain crashed).

Usage:
    python aim_test.py [--ticks N] [--build DIR]
Exit code 0 on PASS, 1 on FAIL.
"""

import os
import sys
import glob
import subprocess
from pathlib import Path

# -asap by default: ticks run back-to-back instead of one per 20 ms of wall
# clock. Same seed -> byte-identical game, just faster. --no-asap (or
# WINBOLO_ASAP=0) puts this run back on the 20 ms live-game pacing.
from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
# Absolute (forward-slash) paths — the runner sets cwd=build_dir, so relative
# brain paths would resolve under build/ and silently fall back to the default.
SHOOTER_BRAIN = (REPO / "brains/GoalHunter/init.lua").as_posix()
VICTIM_BRAIN  = (REPO / "tests/brains/drive_east.lua").as_posix()
MAP = HERE / "aim_arena.map"

START_ARMOUR = 40    # TANK_FULL_ARMOUR
DAMAGE_PER_HIT = 5   # global.h: #define DAMAGE 5 (armour removed per shell hit)
# Pass metric: of the shells the shooter actually LAUNCHED, at least this fraction
# must connect with the enemy tank. This rewards fire discipline + lead accuracy
# together — a shooter that only fires when the shot will land scores high, while
# one that sprays out-of-range or poorly-led shells tanks its percentage even if
# it eventually deals damage. Requires a minimum sample so 1-for-1 can't pass.
AIM_MIN_HIT_PCT = 80.0
AIM_MIN_SHOTS   = 8


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


def read_victim_hits(build_dir, sess):
    """Return (hits, damage, death_ticks, path) from victim_log.csv. HITS is the
    total armour the shooter removed (sum of per-tick armour DROPS) / DAMAGE per
    shell — robust to the victim's slow auto-repair (a rise is never a hit) and to
    two shells landing in one tick (a -10 tick counts as 2). death_ticks are the
    ticks the victim's armour reached 0 (it dies and respawns in 'open'); shells
    already in flight at those moments can't hit and are excluded from the rate.
    The victim writes to DEBUG_SESSION_DIR/victim_log.csv if set, else CWD."""
    for cand in ((sess / "victim_log.csv") if sess else None,
                 build_dir / "victim_log.csv"):
        if cand and cand.exists():
            path = cand
            break
    else:
        return None, None, None, None
    prev, hit_ticks, death_ticks = None, [], []
    for line in path.read_text(errors="ignore").splitlines():
        cols = line.split(",")
        if len(cols) >= 9 and cols[0].isdigit() and cols[8].lstrip("-").isdigit():
            t = int(cols[0]); arm = int(cols[8])
            # A per-tick DROP within the normal armour band is one (or more)
            # landed shells; a rise (auto-repair / respawn) is never a hit.
            if prev is not None and arm < prev and prev <= START_ARMOUR:
                for _ in range((prev - arm) // DAMAGE_PER_HIT):
                    hit_ticks.append(t)
            if prev is not None and prev > 0 and arm <= 0:
                death_ticks.append(t)          # armour hit 0 -> the victim died
            prev = arm
    return hit_ticks, death_ticks, path


def read_shooter_launches(sess):
    """Return the list of (launch_tick, flight_ticks) for every shell the shooter
    LAUNCHED. A launch is a tick where ENGINE_DUMP 'sh=' (shell count) decrements
    — info.shells drops by one only on OUR fire, and there are no bases on the aim
    map to refuel, so this is an exact fire count. flight_ticks (how long the
    shell is in the air) comes from the same-tick TANK_ENGAGE 'flight=' so we can
    tell which shells were still airborne when the victim died."""
    import re
    if not sess:
        return None
    log = sess / "print2_bot0.log"
    if not log.exists():
        return None
    text = log.read_text(errors="ignore")
    flight = {}
    for m in re.finditer(r"TANK_ENGAGE t=(\d+).*?flight=([0-9.]+)", text):
        flight[int(m.group(1))] = float(m.group(2))
    launches, prev = [], None
    for m in re.finditer(r"ENGINE_DUMP t=(\d+)[^\n]*?\bsh=(\d+)", text):
        t = int(m.group(1)); sh = int(m.group(2))
        if prev is not None and sh < prev:
            fire_t = t - 1                     # the KEY_SHOOT was the tick before sh dropped
            fl = flight.get(fire_t) or flight.get(t) or 30.0
            for _ in range(prev - sh):         # >1 drop in a tick = that many shells
                launches.append((fire_t, fl))
        prev = sh
    return launches


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
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    subprocess.run(cmd, cwd=str(build_dir), env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=max(240, ticks // 10))

    sess = newest_session(build_dir, label)
    if sess and list(sess.glob("brain_crash_*.log")):
        print(f"FAIL: brain crashed — see {sess}")
        return 1

    hit_ticks, death_ticks, path = read_victim_hits(build_dir, sess)
    if hit_ticks is None:
        print("FAIL: no victim_log.csv produced (victim didn't run?)")
        return 1
    launches = read_shooter_launches(sess)
    if launches is None:
        print("FAIL: no shooter print2_bot0.log (couldn't count shells fired)")
        return 1

    # Measure only the FIRST life: the victim starts at full armour, the shooter
    # whittles it to 0, and at that death the engagement is over. Each armour DROP
    # is a landed hit at a KNOWN tick, so match every hit to the launch that
    # caused it (nearest fire+flight -> hit tick). A launch is then:
    #   * a HIT   — it was matched to an armour drop;
    #   * a MISS  — unmatched but it landed before the death (had its chance);
    #   * IN-AIR  — unmatched and still airborne when the victim died (arrival
    #               past the death) — excluded, it never had a chance to hit.
    # Anchoring hits to real drop ticks (not an estimated arrival) means the
    # killing shell is never wrongly dropped, so the rate can't exceed 100%.
    death = death_ticks[0] if death_ticks else float("inf")
    life1_hits = [t for t in hit_ticks if t <= death]
    launches = sorted(launches)
    matched = [False] * len(launches)
    for H in life1_hits:
        best, bestd = None, 1e9
        for i, (ft, fl) in enumerate(launches):
            if matched[i] or ft >= H:
                continue
            d = abs((ft + fl) - H)
            if d < bestd:
                bestd, best = d, i
        if best is not None:
            matched[best] = True
    hits = sum(matched)
    misses = sum(1 for i, (ft, fl) in enumerate(launches)
                 if not matched[i] and ft < death and (ft + fl) <= death)
    in_air = sum(1 for i, (ft, fl) in enumerate(launches)
                 if not matched[i] and ft < death and (ft + fl) > death)
    fired = hits + misses
    pct = (100.0 * hits / fired) if fired else 0.0

    print(f"  scope: " + (f"first life (victim died at t={death})" if death_ticks
                          else "full run (victim never died)"))
    print(f"  shells that could land: {fired}   hits: {hits}   hit rate: {pct:.0f}%")
    print(f"  (excluded {in_air} shell(s) still in the air when the victim died)")
    print(f"  log: {path}")
    if fired < AIM_MIN_SHOTS:
        print(f"FAIL: only {fired} landable shells (< {AIM_MIN_SHOTS}) — shooter never really engaged.")
        return 1
    if pct >= AIM_MIN_HIT_PCT:
        print(f"PASS: {pct:.0f}% of landable shells hit the target (>= {AIM_MIN_HIT_PCT:.0f}%).")
        return 0
    print(f"FAIL: {pct:.0f}% hit rate (< {AIM_MIN_HIT_PCT:.0f}%) — {misses} of {fired} "
          f"landable shells missed (poor lead or firing out of range).")
    return 1


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
    sys.exit(run(ticks, build))


if __name__ == "__main__":
    main()
