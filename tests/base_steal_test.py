#!/usr/bin/env python3
"""Armour-aware attack_base pricing + IMMINENT BASE STEAL test.

Field incident 20260901_160325_1_par2 bot3, t=23491.  Four hostile bases stood
within six tiles of the tank -- it had 35 armour and 28 shells -- and the bot
never bid on any of them as a steal.  attack_base surfaced ONE candidate at cost
169: the path plus a FLAT 80 markup that took no notice of how much armour was
left on the base.  It lost every pool.

Two changes came out of that, and this test covers both.

  1. ARMOUR-AWARE PRICING.  ATTACK_BASE_EXTRA_COST is scaled by the WORK LEFT:
     shells-still-needed / 17, where 17 = ceil((90 - 9) / 5) is what a full base
     costs in shells.  A base one shell from falling pays 80 x 1/17 ~= 4.7; an
     untouched one still pays 80.  A stale armour reading decays the discount
     back to the flat price, so old information can never make a healthy base
     look free.

  2. IMMINENT BASE STEAL.  A hostile base with a FRESH armour reading at or
     below BASE_STEAL_MAX_ARMOUR (24, i.e. three shells from capturable), inside
     BASE_STEAL_RANGE (6) tiles, with shells to spare, with no live
     hostile/neutral pillbox covering the base tile or our straight approach to
     it, on a healthy tank that is not afloat, is snapped to BASE_STEAL_COST
     (10) in the attack_base slot.  One shell later the base is at armour 0 and
     the existing IMMINENT capture takes the hand-off.

Two variants (tests/generate_base_steal_map.py), each one game-minute:

  A  Two hostile bases at armour 12 -- one shell from falling -- three tiles
     either side of the spawn, plus a full-armour base twelve tiles south.
     PASS: the brain reports STEAL on both near bases, both are OURS at the end
     of the minute, the far base is rejected out_of_range and still quotes the
     full 80 markup, and the bot does not die.

  B  A's arena with a NEUTRAL pillbox on an unreachable island seven tiles east
     of the EAST base, covering that base and the approach to it.
     PASS: the east base's row reads REJECT covered, the WEST base is still
     stolen and flipped, and the bot does not die.  A alone cannot prove the
     coverage guard did anything -- B is the control, and the pillbox is the
     only thing that moved.

Companion to tests/capture_cluster_test.py (same print2-log harness pattern).

Usage: python base_steal_test.py [--variant A|B|ALL] [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"

sys.path.insert(0, str(HERE))
from generate_base_steal_map import (            # noqa: E402
    WEST_BASE, EAST_BASE, FAR_BASE, GUARD, SPAWN, NEAR_ARMOUR,
    BASE_FULL_ARMOUR, FULL_SHELLS_TO_KILL, ATTACK_BASE_EXTRA_COST,
    MARKUP_STALE, STEAL_RANGE, markup, shells_to_kill, steal_bases,
    covered_base, guards)

PORTS = {"A": 50061, "B": 50062}

# goals.lua refresh_base_steal, one line per eval cycle:
#   BASE_STEAL t=310 tank=(126,126) arm=40 sh=40 pick=base#1 | base#1@(122,126)
#     arm=12(age=4) n=1/17 markup=4.7 d=4 STEAL; base#2@(130,126) ... REJECT covered(pill#1)
STEAL_LINE_RE = re.compile(r"BASE_STEAL t=(\d+) tank=\((\d+),(\d+)\) "
                           r"arm=(\d+) sh=(\d+) pick=(\S+)(?: veto=(\S+))? \| (.*)")
CAND_RE = re.compile(
    r"base#(\d+)@\((\d+),(\d+)\) arm=(\S+)\(age=(\S+)\) n=(\S+)/(\d+) "
    r"markup=([\d.]+) d=(\d+) (STEAL|REJECT [a-z_]+(?:\(pill#\d+\))?)")

# Whole-tank vetoes: refresh_base_steal stamps these on EVERY row at once, so
# they say nothing about the base itself.  The tank spawns afloat (a start
# square has to be deep sea) so the first handful of rows are always `inboat`.
TANK_VETOES = ("REJECT inboat", "REJECT tank_armour")


def first_real(rows):
    """The first row that is about the BASE, not about the tank."""
    for r in rows:
        if r["verdict"] not in TANK_VETOES:
            return r
    return None
# init.lua:  GOAL_CHANGE Goal: attack_base #2 (130,126)
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)(?: #(-?\d+))? \((\d+),(\d+)\)")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def parse_cands(text):
    """Every BASE_STEAL line -> {tile: [row, ...]} keyed by base tile."""
    by_tile = {}
    for m in STEAL_LINE_RE.finditer(text):
        for c in CAND_RE.finditer(m.group(8)):
            tile = (int(c.group(2)), int(c.group(3)))
            by_tile.setdefault(tile, []).append({
                "t": int(m.group(1)), "id": int(c.group(1)),
                "armour": c.group(4), "age": c.group(5), "need": c.group(6),
                "nfull": int(c.group(7)), "markup": float(c.group(8)),
                "dist": int(c.group(9)), "verdict": c.group(10),
            })
    return by_tile


def run_one(variant, ticks, build_dir):
    label = f"base_steal_{variant}"
    mapfile = HERE / f"base_steal_{variant}.map"
    final = HERE / f"base_steal_{variant}_final.json"
    stderr = HERE / f"base_steal_{variant}_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    subprocess.run([sys.executable, str(HERE / "generate_base_steal_map.py"),
                    variant], check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return 1

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORTS[variant]),
           "-nolobby", "-gametype", "open", "-bots", "1",
           "-brain", str(BRAIN),
           # yesfull: the whole arena is known from tick 0 - the test is about
           # how the bases are PRICED, not about finding them.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"]
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
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return 1
    text = log.read_text(errors="ignore")
    print(f"  session: {sess.name}")

    by_tile = parse_cands(text)
    if not by_tile:
        print("FAIL: the brain never emitted a BASE_STEAL line. Either "
              "refresh_base_steal never ran, or it saw no hostile base at all "
              "(the sidecar sets base owner to a phantom slot in on_setup - "
              "check the arena log for BASE_STEAL_ARENA lines).")
        return 1
    for tile, rows in sorted(by_tile.items()):
        last = rows[-1]
        print(f"  base {tile}: {len(rows)} row(s), last t={last['t']} "
              f"arm={last['armour']}(age={last['age']}) "
              f"n={last['need']}/{last['nfull']} markup={last['markup']:.1f} "
              f"d={last['dist']} {last['verdict']}")

    # -- 1. Armour-aware pricing.  Check EVERY row against the formula from its
    #       own reported armour and reading age, so the whole chain - work-left
    #       fraction and staleness decay together - has to reproduce the number
    #       the brain printed, not just one hand-picked sample.
    checked = 0
    for tile, rows in sorted(by_tile.items()):
        for r in rows:
            if r["armour"] == "?" or r["need"] == "?":
                # No usable reading (never observed, or the EVENT_BASE_UPDATE
                # armour-0 artefact): full price, no discount.
                want = markup(0, None)
            else:
                age = None if r["age"] == "never" else int(r["age"])
                want = markup(int(r["armour"]), age)
            if abs(r["markup"] - want) > 0.15:
                print(f"FAIL: base {tile} at t={r['t']} quoted markup "
                      f"{r['markup']:.1f}; armour {r['armour']} with a "
                      f"{r['age']}-tick-old reading should charge "
                      f"{ATTACK_BASE_EXTRA_COST} x (n/{FULL_SHELLS_TO_KILL} "
                      f"blended toward 1 over {MARKUP_STALE} ticks) = "
                      f"{want:.1f}.")
                return 1
            checked += 1

    # The near bases start one shell from falling and must be quoted as such;
    # the far base starts untouched and must still pay the full flat markup.
    fresh_near = markup(NEAR_ARMOUR, 0)
    fresh_far = markup(BASE_FULL_ARMOUR, 0)
    for tile in (WEST_BASE, EAST_BASE):
        rows = by_tile.get(tile)
        if not rows:
            print(f"FAIL: no candidate row for the near base {tile}")
            return 1
        # A reading is always at least a tick old by the time it is priced, so
        # allow the decay a little room: markup(NEAR_ARMOUR, 20) is the price of
        # a 20-tick-old one-shell reading.
        best = min(r["markup"] for r in rows)
        lo, hi = fresh_near - 0.05, markup(NEAR_ARMOUR, 20)
        if not (lo <= best <= hi):
            print(f"FAIL: near base {tile} never quoted a fresh one-shell "
                  f"markup. Cheapest row was {best:.1f}; expected between "
                  f"{lo:.1f} ({ATTACK_BASE_EXTRA_COST} x "
                  f"{shells_to_kill(NEAR_ARMOUR)}/{FULL_SHELLS_TO_KILL} with no "
                  f"decay) and {hi:.1f} (the same reading 20 ticks old).")
            return 1
    far_rows = by_tile.get(FAR_BASE)
    far = first_real(far_rows or [])
    if not far:
        print(f"FAIL: no non-veto candidate row for the far base {FAR_BASE}")
        return 1
    if abs(far["markup"] - fresh_far) > 0.15 or far["need"] != str(FULL_SHELLS_TO_KILL):
        print(f"FAIL: the FULL-armour base {FAR_BASE} opened at markup "
              f"{far['markup']:.1f} (n={far['need']}), expected the "
              f"undiscounted {fresh_far:.1f} over "
              f"{FULL_SHELLS_TO_KILL} shells. A healthy base must still pay "
              "full price.")
        return 1
    if not far["verdict"].startswith("REJECT out_of_range"):
        print(f"FAIL: the far base {FAR_BASE} ({far['dist']} tiles away, past "
              f"BASE_STEAL_RANGE) was not rejected out_of_range: "
              f"{far['verdict']}")
        return 1
    print(f"  1 OK: {checked} row(s) reproduce the markup formula exactly; near "
          f"bases bottom out at {fresh_near:.1f} (80 x 1/17), the untouched "
          f"base opens at {fresh_far:.1f} and is out_of_range")

    # -- 2. Which bases armed the steal, and which the guards locked out. -----
    #
    # A base only stays in this list while it is hostile AND standing: the
    # moment its armour falls to 0 it belongs to capture_base, so it drops out.
    # That means a base can be taken WITHOUT ever showing a STEAL row - the
    # armour-aware markup alone (80 x 1/17 ~= 4.7) is often enough to win the
    # pool while the tank is still afloat on its spawn.  So STEAL is required
    # where the arena guarantees a window for it (variant A, both near bases),
    # and elsewhere the requirement is that the machinery armed at all and that
    # a GUARDED base never did.
    want_steal = set(steal_bases(variant))
    cov = covered_base(variant)
    armed = {t for t, rows in by_tile.items()
             if any(r["verdict"] == "STEAL" for r in rows)}
    if not armed:
        print("FAIL: no base ever armed the steal. Every row was a reject - "
              "see the verdicts above for which gate stopped it.")
        return 1
    if variant == "A":
        missing = [t for t in sorted(want_steal) if t not in armed]
        if missing:
            print(f"FAIL (A): {missing} never armed the steal. Verdicts seen: "
                  + str({t: sorted({r['verdict'] for r in by_tile.get(t, [])})
                         for t in missing}))
            return 1
    # A row only says anything about the coverage guard when the base was a
    # live steal candidate by DISTANCE and the tank itself was not vetoed --
    # otherwise an earlier gate (inboat, out_of_range) answers first.
    def in_range_rows(tile):
        return [r for r in by_tile.get(tile, [])
                if r["dist"] <= STEAL_RANGE and r["verdict"] not in TANK_VETOES]

    if cov:
        rows = in_range_rows(cov)
        if not rows:
            print(f"FAIL (B): the guarded base {cov} was never even a candidate "
                  f"by distance (no row with d <= {STEAL_RANGE} off the boat), "
                  "so the coverage guard was never asked the question. The "
                  "arena is not exercising what it claims to.")
            return 1
        bad = [r for r in rows if not r["verdict"].startswith("REJECT covered")]
        if bad:
            print(f"FAIL (B): the guarded base {cov} was in steal range on "
                  f"{len(rows)} row(s) but {len(bad)} of them did not read "
                  f"`REJECT covered`: {sorted({r['verdict'] for r in bad})}. "
                  f"The pillbox at {GUARD} has the range and a clear line onto "
                  f"the base and onto the approach from {SPAWN}.")
            return 1
        if cov in armed:
            print(f"FAIL (B): base {cov} armed the steal at least once despite "
                  "the pillbox covering it.")
            return 1
        print(f"  2 OK: steal armed at {sorted(armed)}; {cov} was in range on "
              f"{len(rows)} row(s) and every one reads REJECT covered")
    else:
        # Variant A's mirror: with nothing guarding them, every in-range row on
        # a near base must actually arm.
        for tile in sorted(want_steal):
            rows = in_range_rows(tile)
            bad = [r for r in rows if r["verdict"] != "STEAL"]
            if bad:
                print(f"FAIL (A): near base {tile} was in steal range on "
                      f"{len(rows)} row(s) but {len(bad)} did not arm: "
                      f"{sorted({r['verdict'] for r in bad})}")
                return 1
        print(f"  2 OK: steal armed at {sorted(armed)}, on every in-range row")

    # -- 3. Did the bot actually take them, and survive doing it. ------------
    goals = GOAL_RE.findall(text)
    kinds = sorted({g[0] for g in goals})
    if "attack_base" not in kinds:
        print(f"FAIL: the bot never committed to attack_base at all. "
              f"Goals seen: {kinds}")
        return 1
    if not final.exists():
        print("FAIL: no final JSON")
        return 1
    f = json.load(open(final))
    owners = {(b.get("tx"), b.get("ty")): (b.get("owner"), b.get("armor"))
              for b in f.get("bases", [])}
    print(f"  final bases: {owners}")
    deaths = sum(t.get("deaths", 0) for t in f.get("tanks", []))
    print(f"  goals: {kinds}; {deaths} tank death(s)")
    if deaths:
        print("FAIL: the bot died - a snap-priced base steal must not be a "
              "suicide run.")
        return 1
    not_ours = [t for t in sorted(want_steal) if owners.get(t, (None,))[0] != 0]
    if not_ours:
        print(f"FAIL: {not_ours} still not ours after the minute. A base one "
              f"shell from falling, {abs(SPAWN[0] - not_ours[0][0]) + abs(SPAWN[1] - not_ours[0][1])} "
              "tiles away, priced at the steal floor, should have been taken. "
              f"Owners: {owners}")
        return 1
    if cov and owners.get(FAR_BASE, (None,))[0] == 0:
        # Not a failure by itself, just worth seeing in the output.
        print("  note: the far base was taken too")

    if variant == "A":
        print("PASS (A): both armour-12 bases priced at the steal floor and "
              "were flipped inside the minute, while the full-armour base kept "
              "its 80 markup and stayed out of range.")
    else:
        print(f"PASS (B): the pillbox at {GUARD} locked the steal out of "
              f"{cov} (REJECT covered) while {WEST_BASE} was still stolen and "
              "flipped, with no tank death.")
    return 0


def main():
    ticks = 6000
    build = DEFAULT_BUILD
    variants = ["A", "B"]
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        elif args[i] == "--variant":
            v = args[i + 1].upper()
            variants = ["A", "B"] if v == "ALL" else [v]
            i += 2
        else:
            print(f"unknown arg {args[i]}"); return 2
    rc = 0
    for v in variants:
        print(f"=== variant {v} ({ticks} ticks) ===")
        rc |= run_one(v, ticks, build)
    return rc


if __name__ == '__main__':
    sys.exit(main())
