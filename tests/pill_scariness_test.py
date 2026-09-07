#!/usr/bin/env python3
"""Damage-source scariness test (GoalHunter defend_pill, Part 1).

Field incident 20260901_160325_1_par2 bot3, t=17930-18600.  Six free dead
pills sat in a heap for 700 ticks while the bot cycled defend-watch /
take_cover / capture.  What kept preempting the capture was our pill #4 at
(123,131) reading as "taking_damage": its ARRIVED-watch bid (30, weighted 93)
took the tick 50 ticks after the capture finally won.  #4 was at 13/15 HP and
the damage was STRAY FIRE from NEUTRAL pill #8 shooting at our tank.

Pillboxes only ever fire at TANKS (pillbox.c pillsUpdate), so a shell that
lands on a pill is always a miss aimed at somebody else.  The brain now
attributes every hp drop on a team pill to a source class (perception.lua
shell_source_class + its attribution pass) and scales the defend siege tier by
it -- tank x1.00 > enemy pill x0.50 > neutral pill x0.25 -- and the
ARRIVED-watch bid additionally requires TANK fire, or a pill already below
DEFEND_WATCH_MIN_HP_FRAC (2/3) of full.

Two variants, both inside one game-minute (6000 ticks):

  A  A neutral pillbox on an unreachable island shells our tank; our own pill
     sits directly in the line and eats every stray.  The bot's tank has an
     EMPTY loadout (the sidecar spawns it "strict"), so it can neither shoot
     the pillbox down nor repair the pill -- the only question left is what it
     does about the damage.  PASS requires all of:
       A1  the hits attribute as src=npill, by the shell back-ray;
       A2  the defend gate refuses the watch on that source, printing the
           0.25 factor (dmgsrc=npill{x0.25});
       A3  NO watch bid on that pill, ever, while it is healthy;
       A4  the pill really does stay >= 2/3 HP (10 of 15);
       A5  the bot goes and does its job -- the dead pill is collected.

  B  The control.  No pillbox on the map except ours, and a HOSTILE bot tank
     six tiles away.  Any damage can then only be tank fire, and defend must
     behave exactly as it did before the change.  PASS requires:
       B1  the hits attribute as src=tank;
       B2  the ARRIVED watch bid fires on the damage path (block
           `taking_damage`) while the pill is still healthy -- which the gate
           only allows for tank fire.

Companion to tests/take_cover_test.py (same print2-log harness pattern).

Usage: python pill_scariness_test.py [--variant A|B|ALL] [--ticks N] [--build DIR]
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

sys.path.insert(0, str(HERE))
from generate_pill_scariness_map import (        # noqa: E402
    OUR_PILL, NEUTRAL_PILL, DEAD_PILL, SPAWN, HEALTHY_HP, PILLS_MAX_HEALTH)

PORTS = {"A": 50051, "B": 50052}

# perception.lua:
#   PILL_HIT_SRC t=109 pill@(126,122) hp=14 hit_t=109 src=npill via=shell by=0
HIT_SRC_RE = re.compile(
    r"PILL_HIT_SRC t=(\d+) pill@\((\d+),(\d+)\) hp=(\d+) hit_t=(\d+) "
    r"src=(\w+) via=(\w+) by=(\S+)")
# goals.lua, the two ARRIVED-branch gate prints:
#   HEAT_GATE t=125 pill@(126,122) NO-BID (taking_damage, no-watch npill strays,
#     hp 14/15: hit_age=16 live_enemy=false dmgsrc=npill{x0.25} hp=14/15) ...
NO_BID_RE = re.compile(
    r"HEAT_GATE t=(\d+) pill@\((\d+),(\d+)\) NO-BID \((\w+), (.*?): "
    r"hit_age=(\S+) live_enemy=(\w+) dmgsrc=(\S+) hp=(\d+)/(\d+)\)")
#   HEAT_GATE t=703 pill@(126,122) WATCH 30 (taking_damage) dmgsrc=tank{x1.00} at=...
WATCH_RE = re.compile(
    r"HEAT_GATE t=(\d+) pill@\((\d+),(\d+)\) WATCH (\S+) \((\w+)\) "
    r"dmgsrc=(\S+) at=\((\d+),(\d+)\) src=(\S+) hp=(\d+)")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def our_brain_log(sess, variant):
    """The log of OUR bot.

    A runs with -bots 0 and the sidecar spawns the tank, so the slot (and the
    file name) is whatever the server handed out -- there is exactly one log.
    B runs with -bots 1 plus a spawned FOE, so there are two; ours is slot 0,
    the -bots tank the sidecar pinned to start 1."""
    logs = sorted(sess.glob("print2_bot*.log"))
    if not logs:
        return None
    if variant == "B":
        for lg in logs:
            if lg.name == "print2_bot0.log":
                return lg
    return logs[0]


def run_one(variant, ticks, build_dir):
    label = f"pill_scariness_{variant}"
    mapfile = HERE / f"pill_scariness_{variant}.map"
    final = HERE / f"pill_scariness_{variant}_final.json"
    snap = HERE / f"pill_scariness_{variant}_snap.jsonl"
    stderr = HERE / f"pill_scariness_{variant}_stderr.txt"
    hp_trace = build_dir / f"pill_scariness_{variant}_hp.log"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    subprocess.run([sys.executable, str(HERE / "generate_pill_scariness_map.py"),
                    variant], check=True, stdout=subprocess.DEVNULL)
    for p in (final, snap, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return 1

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORTS[variant]),
           "-nolobby", "-gametype", "open",
           # A: the sidecar spawns its own EMPTY tank (see the file's note on
           # why the loadout is the arena).  B: the -bots tank is ours and the
           # sidecar adds the hostile one.
           "-bots", "0" if variant == "A" else "1",
           "-brain", str(BRAIN),
           # 2026-09-06: DEFEND_ALARM_MODE (Andrew's defend_pill redesign)
           # deletes the arrived NO-BID / WATCH rungs that A2/B2 assert on, so
           # this test guards the KEEL defend evaluator by pinning it (same
           # policy as defend_repair_test / builder_pool_test C). Variant A's
           # bot is spawned by the sidecar, which carries the same token.
           "-bot-init", f"0={BRAIN}[cfg=DEFEND_ALARM_MODE=false]",
           # yesfull: the whole (tiny) arena is known from tick 0 - the test is
           # about who is shooting our pill, not about fog.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-snapjson", str(snap), "-snapinterval", "200",
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 10))

    sess = newest_session(build_dir, label)
    if not sess:
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return 1
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed - see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:1500])
        return 1
    log = our_brain_log(sess, variant)
    if not log:
        print(f"FAIL: no print2_bot*.log under {sess}")
        return 1
    text = log.read_text(errors="ignore")
    print(f"  session: {sess.name}   brain log: {log.name}")

    px, py = OUR_PILL
    hits = [h for h in HIT_SRC_RE.findall(text)
            if (int(h[1]), int(h[2])) == (px, py)]
    nobids = [n for n in NO_BID_RE.findall(text)
              if (int(n[1]), int(n[2])) == (px, py)]
    watches = [w for w in WATCH_RE.findall(text)
               if (int(w[1]), int(w[2])) == (px, py)]
    srcs = {}
    for h in hits:
        srcs[h[5]] = srcs.get(h[5], 0) + 1
    print(f"  our pill {OUR_PILL}: {len(hits)} attributed hit(s) "
          f"{srcs or '{}'}; {len(nobids)} gate NO-BID, {len(watches)} WATCH bid(s)")
    if hits:
        h = hits[0]
        print(f"  first attribution: t={h[0]} hp={h[3]} src={h[5]} via={h[6]} by={h[7]}")

    # HP trace, written by the sidecar in SIM ticks (the brain's own ticks run
    # at half that rate, so the two clocks are deliberately never compared).
    hp_seq = []
    if hp_trace.exists():
        for line in hp_trace.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            hp_seq.append((int(parts[0]), int(parts[1])))
    min_hp = min([hp for _, hp in hp_seq], default=PILLS_MAX_HEALTH)
    print(f"  HP trace: {len(hp_seq)} change(s), lowest {min_hp}/{PILLS_MAX_HEALTH} "
          f"(healthy line {HEALTHY_HP})")

    if variant == "A":
        return check_A(hits, nobids, watches, min_hp, final)
    return check_B(hits, nobids, watches)


def check_A(hits, nobids, watches, min_hp, final):
    # A1 - the shell back-ray found the neutral pillbox.
    npill_hits = [h for h in hits if h[5] == "npill"]
    if not npill_hits:
        print("FAIL (A1): no hit on our pill was attributed to a NEUTRAL pillbox. "
              f"Saw: {[h[5] for h in hits] or 'no hits at all'}. Either the "
              "pillbox never fired (check the spawn is inside PILLBOX_RANGE) or "
              "shell_source_class did not find it on the back-ray.")
        return 1
    by_shell = [h for h in npill_hits if h[6] == "shell"]
    print(f"  A1 OK: {len(npill_hits)} npill hit(s), {len(by_shell)} of them "
          f"from an observed shell's back-ray")

    # A2 - the gate refused the watch ON THE SOURCE, and showed the factor.
    denied = [n for n in nobids
              if "no-watch" in n[4] and n[7].startswith("npill")]
    if not denied:
        print("FAIL (A2): the defend gate never printed a source-denied no-watch "
              "for our pill. NO-BID reasons seen: "
              f"{sorted({(n[3], n[4][:24], n[7]) for n in nobids})}")
        return 1
    d = denied[0]
    if "{x0.25}" not in d[7]:
        print(f"FAIL (A2): the no-watch row does not carry the 0.25 npill factor: "
              f"dmgsrc={d[7]}")
        return 1
    print(f"  A2 OK: t={d[0]} NO-BID ({d[3]}, {d[4]}) dmgsrc={d[7]} "
          f"hp={d[8]}/{d[9]} live_enemy={d[6]}")

    # A3 - and no watch bid on a healthy pill, ever.  (HP is read off the
    # WATCH line itself, so this needs no tick-base conversion.)
    healthy_watches = [w for w in watches if int(w[9]) >= HEALTHY_HP]
    if healthy_watches:
        w = healthy_watches[0]
        print(f"FAIL (A3): the bot still bid a watch on a healthy pill - "
              f"t={w[0]} WATCH {w[3]} ({w[4]}) dmgsrc={w[5]} hp={w[9]}. "
              "That is the par2 behaviour this change removes.")
        return 1
    print(f"  A3 OK: 0 watch bids while the pill was healthy "
          f"({len(watches)} at any HP)")

    # A4 - and the pill really did stay healthy, which is the point: a bot
    # that parks next to it keeps it inside the pillbox's range.
    if min_hp < HEALTHY_HP:
        print(f"FAIL (A4): our pill fell to {min_hp}/{PILLS_MAX_HEALTH}, below the "
              f"{HEALTHY_HP} healthy line - the bot stayed in the neutral pill's "
              "range instead of getting on with its work.")
        return 1
    print(f"  A4 OK: pill never fell below {min_hp}/{PILLS_MAX_HEALTH}")

    # A5 - the "other work" actually happened.
    if not final.exists():
        print("FAIL (A5): no final JSON to check the dead pill against")
        return 1
    f = json.load(open(final))
    dead = [p for p in f.get("pillboxes", [])
            if (p.get("tx"), p.get("ty")) == DEAD_PILL]
    taken = bool(dead and dead[0].get("in_tank"))
    # A pill can also be picked up and re-deployed somewhere else, in which
    # case it is no longer on its old tile at all - equally "collected".
    moved = not dead
    if not (taken or moved):
        print(f"FAIL (A5): the dead pill {DEAD_PILL} was never collected "
              f"(final: {dead[0] if dead else None}). The bot had nothing else "
              "to do and still did not go get it.")
        return 1
    print(f"  A5 OK: dead pill {DEAD_PILL} collected "
          f"({'in tank' if taken else 're-deployed elsewhere'})")

    print("PASS (A): neutral-pillbox strays on a healthy pill attribute as "
          "npill x0.25, get no watch bid, and the bot went and fetched the "
          "free pill instead.")
    return 0


def check_B(hits, nobids, watches):
    # B1 - with no pillbox on the map, every route must answer "tank".
    tank_hits = [h for h in hits if h[5] == "tank"]
    if not tank_hits:
        print("FAIL (B1): our pill was never damaged by the hostile tank "
              f"(attributions seen: {[h[5] for h in hits] or 'none'}). The foe "
              "never engaged - check the sidecar's spawn_bot and on_choose_start.")
        return 1
    vias = {}
    for h in tank_hits:
        vias[h[6]] = vias.get(h[6], 0) + 1
    print(f"  B1 OK: {len(tank_hits)} hit(s) attributed to TANK fire, by {vias}")
    bad = [h for h in hits if h[5] != "tank"]
    if bad:
        print(f"FAIL (B1): {len(bad)} hit(s) blamed on a pillbox on a map with "
              f"no hostile pillbox at all, e.g. src={bad[0][5]} via={bad[0][6]}")
        return 1

    # B2 - and defend still bids the watch off the damage path, which the gate
    # allows only for tank fire while the pill is healthy.
    dmg_watches = [w for w in watches
                   if w[4] == "taking_damage" and int(w[9]) >= HEALTHY_HP]
    if not dmg_watches:
        print("FAIL (B2): no ARRIVED watch bid fired on the damage path for a "
              "healthy pill under TANK fire - the change has switched defend "
              f"off where it must not. Watch lines seen: "
              f"{[(w[0], w[4], w[5], w[9]) for w in watches[:6]] or 'none'}")
        return 1
    w = dmg_watches[0]
    print(f"  B2 OK: t={w[0]} WATCH {w[3]} ({w[4]}) dmgsrc={w[5]} hp={w[9]} "
          f"hold=({w[6]},{w[7]}) [{w[8]}] - {len(dmg_watches)} such line(s)")

    print("PASS (B): tank fire on the same pill still attributes as src=tank "
          "and still gets the watch bid, exactly as before the change.")
    return 0


def main():
    ticks = 6000
    build = DEFAULT_BUILD
    variants = ["A", "B"]
    args = sys.argv[1:]
    take_asap_flag(args)      # consumes --asap / --no-asap
    print(pacing_line(""))
    i = 0
    while i < len(args):
        if args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        elif args[i] == "--variant":
            v = args[i + 1].upper(); i += 2
            variants = ["A", "B"] if v == "ALL" else [v]
        else:
            i += 1
    rc = 0
    for v in variants:
        print(f"-- variant {v} " + "-" * 46)
        try:
            r = run_one(v, ticks, build)
        except subprocess.TimeoutExpired:
            print(f"FAIL ({v}): run timed out")
            r = 1
        rc = rc or r
    sys.exit(rc)


if __name__ == "__main__":
    main()
