#!/usr/bin/env python3
"""Respawn-loadout regression test (integration layer).

Proves end-to-end, inside a real dedicated server, that a scenario bot
spawned with game.spawn_bot(..., "open") is handed the FULL open loadout on
EVERY respawn — not just on the join.

How it works:
  * tests/generate_respawn_loadout_map.py writes data/maps/RespawnLoadoutTest.map:
    a 13x13 grass island ringed by 8 neutral full-armour pillboxes at the
    fastest legal reload, deep sea beyond it, and four starts — the map's
    ONLY starts — inside the ring. Zero bases, so the TOURNAMENT loadout the
    sim runs under is 0/0/0 and a leak back to the sim rules is unmistakable.
  * data/maps/RespawnLoadoutTest.scenario.lua (the sidecar the server auto-loads
    by map name) spawns one bot in "open" mode, watches game.tank(slot) every
    tick, and on each dead->alive transition asserts shells/mines/trees/armour
    == 40. After 5 clean respawns it prints
        RESPAWN_LOADOUT_TEST PASS n=5
    and ends the round; any violation prints RESPAWN_LOADOUT_TEST FAIL ...
    with the observed numbers. game.message goes to the server console, so the
    verdict is plain stdout.
  * The bot runs tests/brains/fire_north_in_place.lua: it never moves, aims due
    north, and empties its magazine down a lane the map keeps clear of
    pillboxes. Spending the ammunition is what makes the assertion mean
    something — an inert tank reads 40 shells on respawn whether or not the
    engine refuelled it — and firing north means the trap is never damaged, so
    the result cannot depend on AI driving or shooting skill.

Usage: python respawn_loadout_test.py [--ticks N] [--build DIR] [--port N]
                                      [--exe PATH]
  --exe  run a specific dedicated-server binary (useful when the normal
         build/WinBoloDS.exe is locked by a server someone else is running
         and the build had to be linked under another name).
Exit 0 on PASS, 1 on FAIL.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
MAP = REPO / "data" / "maps" / "RespawnLoadoutTest.map"
SIDECAR = REPO / "data" / "maps" / "RespawnLoadoutTest.scenario.lua"
BRAIN = HERE / "brains" / "fire_north_in_place.lua"

# Measured cadence: the bot spawns at tick ~22 and each life costs ~608 ticks
# (the pillboxes need ~350 ticks to chew through 40 armour, then
# TANK_DEATH_WAIT is 255). Six lives is therefore ~3700 ticks; 5200 (~4.5 min
# at 20 ticks/sec) leaves ~40% headroom. The run is deterministic — seeded RNG,
# a brain that parks and fires on a fixed heading — so the cadence does not
# drift between runs. The scenario ends the round as soon as it has a verdict,
# so a passing run exits before the tick limit.
DEFAULT_TICKS = 5200
VERDICT_RE = re.compile(r"RESPAWN_LOADOUT_TEST (PASS|FAIL)\b.*")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def run(ticks, build_dir, port, exe=None):
    ds = Path(exe).resolve() if exe else find_ds(build_dir)
    if not ds or not ds.exists():
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    if not SIDECAR.exists():
        print(f"FAIL: missing scenario sidecar {SIDECAR}")
        return 1

    subprocess.run([sys.executable, str(HERE / "generate_respawn_loadout_map.py")],
                   check=True, stdout=subprocess.DEVNULL)

    cmd = [str(ds),
           "-map", str(MAP),
           "-port", str(port),
           # No lobby: the round starts immediately and the scenario ticks.
           "-nolobby",
           # The sim rules the bot's "open" override has to beat. (The sidecar
           # forces this too via scenario.game; passing it makes the intent
           # explicit and independent of the sidecar.)
           "-gametype", "tournament",
           # REQUIRED, and not for ranked play: serverSimApplyScenarioCommit
           # stamps gameScripted over the game type of any scenario map — and
           # gameTypeGetItems hands gameScripted the same full tank as gameOpen,
           # which would make an open override indistinguishable from no
           # override and the test vacuous. `!sim->ranked` is the one branch
           # that leaves the type alone, so -ranked is what keeps the sim on
           # tournament (0/0/0 here) and the test honest. The scenario's own
           # anti-degeneracy control fails loudly if this ever stops working.
           "-ranked",
           # Brains allowed; -bots 0 so the ONLY bot is the scripted one.
           "-ai", "yes", "-bots", "0",
           # spawn_bot gets no explicit brain and the sidecar sets no
           # default_brain, so it falls back to this: an inert brain.
           "-brain", str(BRAIN),
           "-seed", "42",
           "-ticks", str(ticks),
           "-limit", "-1",          # no time limit; -ticks bounds the run
           "-threads", "1",
           "-notracker", "-nowinbolonet", "-noemptyreset", "-noinput"]

    try:
        proc = subprocess.run(cmd, cwd=str(build_dir), stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True,
                              errors="ignore",
                              timeout=max(300, ticks // 10))
    except subprocess.TimeoutExpired as e:
        out = e.stdout or ""
        print("FAIL: dedicated server timed out")
        for line in (out.splitlines() if isinstance(out, str) else []):
            if "RESPAWN_LOADOUT_TEST" in line:
                print("  " + line.strip())
        return 1

    out = proc.stdout or ""
    lines = [l.strip() for l in out.splitlines() if "RESPAWN_LOADOUT_TEST" in l]
    for line in lines:
        print("  " + line)

    verdicts = [l for l in lines if VERDICT_RE.search(l)]
    if not verdicts:
        print("FAIL: the scenario never reached a verdict "
              f"(saw {len(lines)} scenario lines in {ticks} ticks)")
        return 1
    if "PASS" in verdicts[-1]:
        print("PASS: the open-mode spawn loadout survived every respawn.")
        return 0
    print("FAIL: " + verdicts[-1])
    return 1


def main():
    ticks, build, port, exe = DEFAULT_TICKS, DEFAULT_BUILD, 50043, None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]).resolve(); i += 2
        elif args[i] == "--port":
            port = int(args[i + 1]); i += 2
        elif args[i] == "--exe":
            exe = args[i + 1]; i += 2
        else:
            print(__doc__)
            return 2
    return run(ticks, build, port, exe)


if __name__ == "__main__":
    sys.exit(main())
