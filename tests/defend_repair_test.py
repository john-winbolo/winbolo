#!/usr/bin/env python3
"""defend_pill -> repair handoff test (GoalHunter 1.7).

Field incident 20260902_000405_1_16v17 bot2 t=22561: our pill #14 was being
shelled at ~5 hits/s one tile from an enemy base, the bot was parked on its own
base with 40 armour, 23 trees, the LGM aboard -- and it switched from
defend_pill to take_cover on a tile two steps away.  Two separate holes:

  * take_cover's CALM branch bid on a margin made entirely of "this tile has
    one more of our pillboxes overhead", with zero avoided fire, and the
    influence pass halved it for being near home while DOUBLING the defend bid
    for the pill being in enemy influence;
  * even when defend did win, the ARRIVED ladder had no rung for "the shelling
    stopped and I can FIX this" -- `taking_damage` blocks heat for 400 ticks,
    which drops the bid to WATCH, so the defender babysits a 9/15 pill it is
    carrying the wood to repair.

The arena (tests/generate_defend_repair_map.py + defend_repair.scenario.lua)
reproduces the shape at test scale: our pill, our bot parked six tiles from it,
and a SCRIPTED enemy tank across an uncrossable moat that puts eight paced
shells into the pill and then drives away for good.

PASS requires all five:
  1. while the pill is under attack, the bot's goal is defend_pill at some
     point and NEVER take_cover;
  2. no repair is dispatched while shells are still landing -- every
     REPAIR_DISPATCH_FIRED must carry hit_age >= REPAIR_QUIET_TICKS (the
     builder's own reading of the pill's last_hit_tick, so one clock);
  3. a repair IS dispatched once the pill goes quiet, within
     REPAIR_QUIET_TICKS + SLACK of the last hit;
  4. the ARRIVED repair bid itself fired (the new defend rung), i.e. a
     HEAT_GATE "REPAIR BID" line exists, and its cost multiplies out of its own
     chips (DEFEND_REPAIR_COST x the ENEMY_IN_RANGE multiplier -- x1.5 while a
     hostile tank is still visible at the pill, which is a PRICE, not a gate:
     the dispatch still has to happen);
  5. the LGM survived and the repair landed: the engine-side HP trace shows
     the pill's armour going back UP after the shelling stopped.

The builder's under-fire LGM interlock (BUILDER_HOLD_UNDER_FIRE) is REPORTED
but not required.  It can only log while a repair goal is already live and a
shell lands, and this arena's siege is short -- our own pill kills the scripted
shooter after four or five shells -- so the hold does not fire reliably here.
Assertion 2 covers the same guarantee from the other side (no dispatch ever
happens with a fresh hit on the pill), and the interlock's own log line is
exercised by the DH-Oil Rig smoke run.

Usage: python defend_repair_test.py [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
MAP = HERE / "defend_repair.map"
FINAL = HERE / "defend_repair_final.json"
STDERR = HERE / "defend_repair_stderr.txt"
LABEL = "defend_repair_test"

sys.path.insert(0, str(HERE))
from generate_defend_repair_map import (        # noqa: E402
    OUR_PILL, PILLS_MAX_HEALTH)

# constants.lua
REPAIR_QUIET_TICKS = 75
SLACK = 120          # brain ticks: one replan cycle plus the drive-in

# init.lua TICK_COST t=123 ms=1.0 tgt=... tier=10 replan=true goal=defend_pill sub=nil
TICK_RE = re.compile(r"TICK_COST t=(\d+) .*? goal=(\S+)")
# perception.lua PILL_HIT_SRC t=123 pill@(126,126) hp=13 hit_t=123 src=tank ...
HIT_RE = re.compile(r"PILL_HIT_SRC t=(\d+) pill@\((\d+),(\d+)\) hp=(\d+)")
# builder.lua REPAIR_DISPATCH_FIRED t=123 pill=(126,126) action=... eta=44
#   goal=defend_pill hit_age=190
FIRED_RE = re.compile(
    r"REPAIR_DISPATCH_FIRED t=(\d+) pill=\((\d+),(\d+)\) action=\S+ eta=(-?\d+) "
    r"goal=(\S+) hit_age=(\S+)")
# builder.lua BUILDER_HOLD_UNDER_FIRE t=123 pill=(126,126) hit_age=12 quiet=75 ...
HOLD_RE = re.compile(
    r"BUILDER_HOLD_UNDER_FIRE t=(\d+) pill=\((\d+),(\d+)\) hit_age=(-?\d+)")
# goals.lua HEAT_GATE t=123 pill@(126,126) REPAIR BID 60 (40 x 1.50 ENEMY_IN_RANGE)
#   hp=9/15 quiet=88 (>= 75) lgm=in_tank trees=22 live_enemy=true drive_to=...
REPBID_RE = re.compile(
    r"HEAT_GATE t=(\d+) pill@\((\d+),(\d+)\) REPAIR BID ([\d.]+) "
    r"\(([\d.]+) x ([\d.]+)([^)]*)\) hp=(\d+)/(\d+) quiet=(\S+).*?"
    r"drive_to=(\S+)")


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
    subprocess.run([sys.executable, str(HERE / "generate_defend_repair_map.py")],
                   check=True, stdout=subprocess.DEVNULL)
    hp_trace = build_dir / "defend_repair_hp.log"
    for p in (FINAL, STDERR, hp_trace):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked -- a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return 1

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=LABEL)
    cmd = [str(ds), "-map", str(MAP), "-port", "50053", "-nolobby",
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           # yesfull: the arena is tiny and the test is about WHEN the bot
           # repairs, not about discovering the map.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(FINAL),
           "-nowinbolonet", "-quiet", "-threads", "1"]
    with open(STDERR, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 10))

    sess = newest_session(build_dir)
    if not sess:
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return 1
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed -- see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:2000])
        return 1
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return 1
    text = log.read_text(errors="ignore")

    hits = [(int(t), int(hp)) for (t, x, y, hp) in HIT_RE.findall(text)
            if (int(x), int(y)) == OUR_PILL]
    goals = [(int(t), g) for (t, g) in TICK_RE.findall(text)]
    fired = FIRED_RE.findall(text)
    holds = HOLD_RE.findall(text)
    repbids = REPBID_RE.findall(text)

    hp_seq = []
    if hp_trace.exists():
        for line in hp_trace.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            a, b = line.split()[:2]
            hp_seq.append((int(a), int(b)))

    print(f"  session: {sess.name}")
    print(f"  hits on our pill {OUR_PILL}: {len(hits)}"
          + (f" (brain ticks {hits[0][0]}..{hits[-1][0]}, hp {hits[0][1]} -> "
             f"{hits[-1][1]})" if hits else ""))
    print(f"  HP trace (sim ticks): {len(hp_seq)} change(s)"
          + (f", lowest {min(h for _, h in hp_seq)}/{PILLS_MAX_HEALTH}"
             if hp_seq else ""))
    print(f"  repair dispatches: {len(fired)}; under-fire holds: {len(holds)}; "
          f"ARRIVED repair bids: {len(repbids)}")

    # ── 0. the arena worked at all ────────────────────────────────────
    if len(hits) < 3:
        print("FAIL: the scripted shooter never really shelled our pill "
              f"({len(hits)} attributed hit(s)). Check the standoff distance "
              "against the tank shell's ~7.1-tile reach, and that the sidecar "
              "spawned it (grep the stderr for DEFEND_REPAIR).")
        tail = [ln for ln in text.splitlines() if "PILL_HIT_SRC" in ln][:5]
        for ln in tail:
            print("   " + ln.strip())
        return 1
    first_hit, last_hit = hits[0][0], hits[-1][0]

    # ── 1. defend, never take_cover, while it is being shot ───────────
    window = [(t, g) for (t, g) in goals
              if first_hit <= t <= last_hit + REPAIR_QUIET_TICKS]
    kinds = sorted({g for _, g in window})
    print(f"  goals inside the attack window [{first_hit}..{last_hit}"
          f"+{REPAIR_QUIET_TICKS}]: {', '.join(kinds) or '(none logged)'}")
    if not window:
        print("FAIL: no TICK_COST lines inside the attack window -- cannot say "
              "what the bot was doing.")
        return 1
    cover = [(t, g) for (t, g) in window if g == "take_cover"]
    if cover:
        print(f"FAIL (1): the bot took cover while its pill was under fire, at "
              f"t={cover[0][0]} ({len(cover)} tick(s)). This is the incident.")
        return 1
    if not any(g == "defend_pill" for _, g in window):
        print("FAIL (1): the bot never chose defend_pill while its own pill was "
              f"being shelled. It was doing: {', '.join(kinds)}")
        return 1
    n_def = sum(1 for _, g in window if g == "defend_pill")
    print(f"  1 OK: defend_pill for {n_def}/{len(window)} ticks of the attack "
          f"window, take_cover never")

    # ── 2. no dispatch while the shells are still landing ─────────────
    early = []
    for (t, px, py, eta, goal, hit_age) in fired:
        if hit_age == "-":
            continue
        if int(hit_age) < REPAIR_QUIET_TICKS:
            early.append((t, goal, hit_age))
    if early:
        print(f"FAIL (2): {len(early)} repair dispatch(es) with the pill still "
              f"under fire, e.g. t={early[0][0]} goal={early[0][1]} "
              f"hit_age={early[0][2]} < REPAIR_QUIET_TICKS {REPAIR_QUIET_TICKS}")
        return 1
    print(f"  2 OK: every one of the {len(fired)} dispatch(es) waited for the "
          f"pill to go quiet")
    if holds:
        print(f"     (the LGM interlock also logged {len(holds)} hold(s), first "
              f"t={holds[0][0]} hit_age={holds[0][3]})")

    # ── 3. ...but it did not dawdle either ────────────────────────────
    prompt = [f for f in fired
              if f[5] != "-" and int(f[5]) <= REPAIR_QUIET_TICKS + SLACK]
    if not fired:
        print("FAIL (3): no repair was ever dispatched. The pill went quiet and "
              "the bot kept watching it.")
        checks = [ln for ln in text.splitlines()
                  if "REPAIR_DISPATCH_CHECK" in ln][-5:]
        for ln in checks:
            print("   " + ln.strip())
        return 1
    if not prompt:
        ages = [f[5] for f in fired]
        print(f"FAIL (3): a repair was dispatched, but never within "
              f"REPAIR_QUIET_TICKS+{SLACK} of a hit. hit_age at dispatch: {ages}")
        return 1
    p0 = prompt[0]
    print(f"  3 OK: repaired at t={p0[0]} (goal={p0[4]}, hit_age={p0[5]}, "
          f"eta={p0[3]}) -- within {REPAIR_QUIET_TICKS}+{SLACK} of the last hit")

    # ── 4. the new defend rung is what offered it ─────────────────────
    if not repbids:
        print("FAIL (4): the ARRIVED repair bid never fired. The defender was "
              "beside a damaged pill with the LGM aboard and wood in hand and "
              "still had no rung between watch and heat.")
        gates = [ln for ln in text.splitlines()
                 if "HEAT_GATE" in ln and "WATCH" in ln][:5]
        for ln in gates:
            print("   " + ln.strip())
        return 1
    # ...and the number on the row is reproducible from its own chips:
    # cost == DEFEND_REPAIR_COST x the ENEMY_IN_RANGE multiplier.
    bad_bid = [r for r in repbids
               if abs(float(r[3]) - float(r[4]) * float(r[5])) > 0.51]
    if bad_bid:
        b = bad_bid[0]
        print(f"FAIL (4): a REPAIR BID row does not multiply out -- t={b[0]} "
              f"cost={b[3]} but base {b[4]} x {b[5]} = "
              f"{float(b[4]) * float(b[5]):.1f}")
        return 1
    r0 = repbids[0]
    mults = sorted({r[5] for r in repbids})
    print(f"  4 OK: {len(repbids)} ARRIVED repair bid(s), first t={r0[0]} "
          f"cost={r0[3]} (= {r0[4]} x {r0[5]}{r0[6]}) hp={r0[7]}/{r0[8]} "
          f"quiet={r0[9]} drive_to={r0[10]}; multipliers seen {mults}")

    # ── 5. the LGM lived and the pill actually came back up ───────────
    rises = [(t, hp) for i, (t, hp) in enumerate(hp_seq)
             if i > 0 and hp > hp_seq[i - 1][1]]
    if not rises:
        print("FAIL (5): the pill's armour never went back up. Either the LGM "
              "was killed on the way or the dispatch never completed. HP trace: "
              + ", ".join(f"{t}:{hp}" for t, hp in hp_seq[:16]))
        return 1
    print(f"  5 OK: pill armour rose {len(rises)} time(s), first at sim t="
          f"{rises[0][0]} to {rises[0][1]}/{PILLS_MAX_HEALTH} -- the LGM walked "
          f"out and back")

    print("PASS: defend held the pill under fire, the LGM stayed aboard while "
          "shells were landing, and the repair went out as soon as it was quiet.")
    return 0


def main():
    ticks = 3000
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
