#!/usr/bin/env python3
"""Sea-pill harvest test — capture_pill's DEEP-SEA branch (GoalHunter 1.7).

Field incident 20260901_000042_1_loss_b6 bot2 t=22735: three dead pills sat in
the first two deep-sea columns off the east shore of DH-Oil Rig and the pool
rejected all three every 50 ticks with `reject=deepsea_no_boat`.  Nothing in
the brain knew how to get afloat.

capture_pill now prices an ENTRANCE into the pills' own body of water and runs
a substate chain in front of the pickup:

  entrance_plan -> (refuel_mines) -> (seek_trees) -> approach_F
                -> lay_mine -> detonate -> build_boat -> board -> pickup

with an entrance LADDER: an existing boat, else an existing river (build the
boat straight into it, 20 trees, no mine), else a mined shore tile cardinally
adjacent to that water (21 trees + 1 mine + shells).

Variants (see tests/generate_sea_pills_map.py for the arenas):
  A  full loadout, no hostiles      -> mine/crater/river/boat, 3 pills taken
  B  hostile pill with a clear line -> REJECT pills_covered_by_pill#N, no mine
  C  same pill behind a breakwater  -> executes like A
  D  0 mines, base stocks mines     -> refuel_mines leg first, then A
  E  0 trees                        -> harvest leg first; the mine NEVER goes
                                       down before 21 trees + 1 mine are held
  F  no forest in our territory     -> REJECT no_trees_in_territory, no mine
  G  existing river in the shore    -> no mine at all; boat built into the
                                       river; 3 pills taken

Usage: python sea_pills_test.py [--variant A|B|C|D|E|F|G|ALL] [--ticks N]
                                [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

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
from generate_sea_pills_map import (            # noqa: E402
    SEA_PILLS, ISOLATED_POND, RIVER_TILE, FIELD_X1, SEA_PILL_TREES_TOTAL,
    SEA_TREES_PER_FOREST,
    gametype_for, VARIANTS)

# Engine terrain values (src/bolo/public/global.h). A mined tile is its base
# terrain + MINE_SUBTRACT(8), so 10..15 are the mined forms of SWAMP..GRASS.
T_BUILDING, T_RIVER, T_CRATER, T_ROAD, T_FOREST, T_GRASS = 0, 1, 3, 4, 5, 7
T_BOAT = 9
MINE_START, MINE_END, MINE_SUBTRACT = 10, 15, 8
TNAME = {0: "BUILDING", 1: "RIVER", 2: "SWAMP", 3: "CRATER", 4: "ROAD",
         5: "FOREST", 6: "RUBBLE", 7: "GRASS", 8: "HALFBUILDING", 9: "BOAT",
         255: "DEEP_SEA"}


def tname(v):
    if MINE_START <= v <= MINE_END:
        return TNAME.get(v - MINE_SUBTRACT, str(v - MINE_SUBTRACT)) + "+MINE"
    return TNAME.get(v, str(v))


SEA_PLAN_RE = re.compile(
    r"SEA_PLAN t=(\d+) cluster=(\d+) n=(\d+) entrance=(\S+) comp=(\d+) "
    r"S=\((\d+),(\d+)\) F=(\S+) mine=(\S+) boat=(\S+) "
    r"trees=(\d+)/(\d+) short=(\d+) need_tiles=(\d+) forest_ok=(\d+) "
    r"mines=(\d+)/(\d+) refuel=(\S+) travel=(\S+) legs=(\S+)/(\S+) "
    r"boat_path=(\S+) cost=(\S+)")
SEA_SUB_RE = re.compile(r"SEA_SUB t=(\d+) from=(\S+) to=(\S+) reason=(.*)")
SEA_REJECT_RE = re.compile(
    r"SEA_REJECT t=(\d+) cluster=(\d+) n=(\d+) pills=\[([^\]]*)\] reason=(\S+) \((.*)\)")
SEA_ABORT_RE = re.compile(r"SEA_ABORT t=(\d+) reason=(\S+)")
SEA_SHOT_RE = re.compile(r"SEA_SHOT t=(\d+) #(\d+) at S=\((\d+),(\d+)\)")
SEA_DISPATCH_RE = re.compile(r"SEA_DISPATCH t=(\d+) action=(\S+) S=\((\d+),(\d+)\)")
TREES_RE = re.compile(r"ENGINE_DUMP t=\d+ .*? tr=(\d+)")
CAPTURE_CAND_RE = re.compile(
    r"CAPTURE_CAND t=(\d+) id=(\S+) @\((\d+),(\d+)\).*?reject=(\S+)")
LAY_MINE_TREES_RE = re.compile(r"trees (\d+)/(\d+) and mines (\d+)/(\d+)")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    root = build_dir / "debug_sessions"
    if not root.is_dir():
        return None
    cands = [d for d in root.iterdir() if d.is_dir() and label in d.name]
    return max(cands, key=lambda d: d.stat().st_mtime) if cands else None


def read_trace(path):
    """{(x,y): [(tick, value), ...]} from the sidecar's terrain log."""
    out = {}
    if not path.exists():
        return out
    for line in path.read_text(errors="ignore").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split()
        if len(parts) != 5:
            continue
        tick, x, y, old, new = (int(p) for p in parts)
        out.setdefault((x, y), []).append((tick, new))
    return out


def run_one(variant, ticks, build_dir, port):
    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    subprocess.run([sys.executable, str(HERE / "generate_sea_pills_map.py"),
                    "--variant", variant],
                   check=True, stdout=subprocess.DEVNULL)
    mapfile = HERE / f"sea_pills_{variant}.map"
    final = HERE / f"sea_pills_{variant}_final.json"
    snap = HERE / f"sea_pills_{variant}_snap.jsonl"
    stderr = HERE / f"sea_pills_{variant}_stderr.txt"
    trace = build_dir / f"sea_pills_terrain_{variant}.log"
    label = f"sea_pills_{variant}"
    for p in (final, snap, stderr, trace):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked — a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return 1

    # A map with a sidecar boots as gameScripted, which hands a -bots tank the
    # OPEN loadout (40/40/40) whatever -gametype says. The variants that need an
    # empty tank therefore run with -bots 0 and let the sidecar spawn its own
    # bot with an explicit loadout mode (game.spawn_bot arms sim->spawnLoadout
    # before tankCreate). -noemptyreset keeps the round alive until it lands.
    scripted_spawn = gametype_for(variant) != "open"
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(mapfile), "-port", str(port), "-nolobby",
           "-gametype", gametype_for(variant),
           "-bots", "0" if scripted_spawn else "1",
           "-brain", str(BRAIN),
           # yesfull: the whole (tiny) arena is known from tick 0 — the test is
           # about getting afloat, not about fog.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-snapjson", str(snap), "-snapinterval", str(SNAP_INTERVAL),
           "-nowinbolonet", "-quiet", "-threads", "1"]
    if scripted_spawn:
        cmd.append("-noemptyreset")
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 20))

    sess = newest_session(build_dir, label)
    if not sess:
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return 1
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed — see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:2000])
        return 1
    # The scripted-spawn variants land in whatever slot spawn_bot picked (it
    # fills from the TOP down), so find the log rather than assuming bot0.
    logs = sorted(sess.glob("print2_bot*.log"), key=lambda p2: p2.stat().st_size)
    if not logs:
        print(f"FAIL: no print2_bot*.log under {sess}")
        return 1
    log = logs[-1]
    print(f"  brain log: {log.name}")
    text = log.read_text(errors="ignore")

    plans = SEA_PLAN_RE.findall(text)
    subs = SEA_SUB_RE.findall(text)
    rejects = SEA_REJECT_RE.findall(text)
    aborts = SEA_ABORT_RE.findall(text)
    shots = SEA_SHOT_RE.findall(text)
    dispatches = SEA_DISPATCH_RE.findall(text)
    tr = read_trace(trace)

    print(f"  SEA_PLAN lines: {len(plans)}   SEA_SUB: {len(subs)}   "
          f"SEA_REJECT: {len(rejects)}   SEA_ABORT: {len(aborts)}   "
          f"SEA_SHOT: {len(shots)}   SEA_DISPATCH: {len(dispatches)}")
    if plans:
        p = plans[0]
        print(f"  first plan: t={p[0]} cluster={p[1]} n={p[2]} entrance={p[3]} "
              f"comp={p[4]} S=({p[5]},{p[6]}) F={p[7]} mine={p[8]} boat={p[9]} "
              f"trees={p[10]}/{p[11]} short={p[12]} need_tiles={p[13]} "
              f"forest_ok={p[14]} mines={p[15]}/{p[16]} refuel={p[17]} "
              f"travel={p[18]} legs={p[19]}/{p[20]} boat_path={p[21]} cost={p[22]}")
    for r in dict.fromkeys((r[4], r[5]) for r in rejects):
        print(f"  reject: {r[0]} — {r[1][:150]}")
    for s in subs:
        print(f"  SEA_SUB t={s[0]} {s[1]} -> {s[2]}  ({s[3][:90]})")
    for a in aborts:
        print(f"  SEA_ABORT t={a[0]} reason={a[1]}")

    # ── Terrain trace at the entrance ──────────────────────────────────
    ent_S = (int(plans[0][5]), int(plans[0][6])) if plans else None
    if ent_S and ent_S in tr:
        seq = [(t, tname(v)) for (t, v) in tr[ent_S]]
        print(f"  terrain at S{ent_S}: " +
              " -> ".join(f"{n}@{t}" for (t, n) in seq))
    any_mine_laid = any(MINE_START <= v <= MINE_END
                        for hist in tr.values() for (_, v) in hist)
    boats = sorted({xy for xy, hist in tr.items()
                    if any(v == T_BOAT for (_, v) in hist)})
    print(f"  mined tile ever seen: {any_mine_laid}    boats appeared at: {boats}")

    # -- Pills collected, and WHEN --------------------------------------
    def collected_in(snapshot):
        n = 0
        for (x, y) in SEA_PILLS:
            lying = False
            for pb in snapshot.get("pillboxes", []):
                if (pb.get("tx"), pb.get("ty")) == (x, y)                         and not pb.get("in_tank") and (pb.get("armor") or 0) == 0:
                    lying = True
            if not lying:
                n += 1
        return n

    collected = 0
    if final.exists():
        collected = collected_in(json.load(open(final)))
    # First snapshot at which the WHOLE cluster was out of the water. Snapshots
    # land every SNAP_INTERVAL sim ticks, so this is the completion tick to that
    # resolution; the brain tick is half the sim tick.
    done_tick = None
    if snap.exists():
        for line in snap.read_text(errors="ignore").splitlines():
            if not line.strip():
                continue
            sn = json.loads(line)
            if collected_in(sn) >= len(SEA_PILLS):
                done_tick = sn.get("tick")
                break
    print(f"  sea pills collected: {collected}/{len(SEA_PILLS)}"
          + (f"   COMPLETED by sim tick {done_tick} (brain tick ~{done_tick // 2})"
             if done_tick is not None else ""))

    # -- What the whole cluster COST ------------------------------------
    mine_dispatches = [d2 for d2 in dispatches if d2[1] == "MINE"]
    boat_dispatches = [d2 for d2 in dispatches if d2[1].startswith("BUILD")]
    trees = [int(m.group(1)) for m in TREES_RE.finditer(text)]
    trees_spent = 0
    for a, b in zip(trees, trees[1:]):
        if b < a:
            trees_spent += a - b
    print(f"  cluster cost: {len(mine_dispatches)} mine dispatch(es), "
          f"{len(boat_dispatches)} boat build(s), {trees_spent} trees spent")

    ok, why = check(variant, plans, subs, rejects, aborts, dispatches,
                    tr, any_mine_laid, boats, collected,
                    done_tick, mine_dispatches, boat_dispatches, trees_spent)
    if ok:
        print(f"PASS ({variant}): {why}")
        return 0
    print(f"FAIL ({variant}): {why}")
    return 1


def _ordered_terrain(tr, xy):
    """The values seen at a tile, in tick order."""
    return [v for (_, v) in tr.get(xy, [])]


def _flood_sequence_ok(vals):
    """The mine -> crater -> river -> boat progression, judged on FIRST
    appearance. Later oscillation is normal and not a failure: a tank that
    drives off a boat turns the tile back into river (it takes the boat with
    it), and a second trip builds another one. A step may also go unobserved
    (the tick sampling can miss CRATER when the flood lands between samples),
    but the firsts that ARE observed must be in order."""
    rank = {}
    for v in range(MINE_START, MINE_END + 1):
        rank[v] = 1
    rank[T_CRATER] = 2
    rank[T_RIVER] = 3
    rank[T_BOAT] = 4
    firsts = {}
    for i, v in enumerate(vals):
        r = rank.get(v)
        if r is not None and r not in firsts:
            firsts[r] = i
    order = [firsts[r] for r in sorted(firsts)]
    return all(a < b for a, b in zip(order, order[1:])), firsts


def check(variant, plans, subs, rejects, aborts, dispatches,
          tr, any_mine_laid, boats, collected,
          done_tick, mine_dispatches, boat_dispatches, trees_spent):
    sub_targets = [s[2] for s in subs]
    reject_reasons = [r[4] for r in rejects]

    if variant in ("A", "C", "D", "E", "G"):
        if not plans:
            return False, "no SEA_PLAN line — the deep-sea branch never priced the cluster"
        entrance = plans[0][3]
        S = (int(plans[0][5]), int(plans[0][6]))
        want_entrance = "existing_river" if variant == "G" else "mine_crater"
        if entrance != want_entrance:
            return False, f"entrance was {entrance}, expected {want_entrance}"
        # Connectivity: the entrance must belong to the pills' water, never the
        # land-locked pond.
        for pond in ISOLATED_POND:
            if abs(S[0] - pond[0]) + abs(S[1] - pond[1]) <= 1:
                return False, (f"entrance S={S} is cardinally adjacent to the "
                               f"land-locked pond {pond} — the connectivity gate failed")
        if variant == "G":
            if S != RIVER_TILE:
                return False, f"expected the existing river tile {RIVER_TILE}, got S={S}"
            # The FIRST trip must use the river: no mine before the tank is
            # afloat. Later trips legitimately fall back to a crater — boarding
            # CONSUMES the boat (the tank takes it), which turns the tile back
            # to river and then the river tile itself is gone once that boat
            # sails away, so a second load has no free entrance left.
            first_board = next((i for i, s2 in enumerate(sub_targets)
                                if s2 == "board"), None)
            if first_board is None:
                return False, "never boarded"
            if "lay_mine" in sub_targets[:first_board]:
                return False, ("entered lay_mine before boarding even though an "
                               "existing river was available")
            if RIVER_TILE not in boats:
                return False, f"no boat was ever built on the river tile {RIVER_TILE}"
        else:
            if S[0] != FIELD_X1:
                return False, (f"entrance S={S} is not on the shore column x={FIELD_X1}")
            if not any_mine_laid:
                return False, "no mine was ever laid"
            ok, firsts = _flood_sequence_ok(_ordered_terrain(tr, S))
            if not ok:
                return False, (f"terrain at S={S} did not progress "
                               f"mine -> crater -> river -> boat "
                               f"(first-seen indices by stage {firsts})")
            vals = _ordered_terrain(tr, S)
            if T_RIVER not in vals:
                return False, f"S={S} never flooded to RIVER"
            if T_BOAT not in vals:
                return False, f"S={S} never became a BOAT"
        if S not in boats:
            return False, f"no boat was ever built at the entrance {S}"
        if variant == "G" and "seek_trees" not in sub_targets:
            return False, "the seek_trees leg never ran (trees started at 0)"
        if variant == "D" and "refuel_mines" not in sub_targets:
            return False, "the refuel_mines leg never ran (mines started at 0)"
        if variant in ("D", "E") and "seek_trees" not in sub_targets:
            return False, "the seek_trees leg never ran (trees started at 0)"
        if variant == "E":
            lm = [s for s in subs if s[2] == "lay_mine"]
            if not lm:
                return False, "never reached lay_mine"
            for s in lm:
                m = LAY_MINE_TREES_RE.search(s[3])
                if not m:
                    return False, f"lay_mine transition did not report its resources: {s[3]}"
                have, need, mhave, mneed = (int(g) for g in m.groups())
                if have < need or have < SEA_PILL_TREES_TOTAL:
                    return False, (f"the mine went down with trees {have}/{need} "
                                   f"(need >= {SEA_PILL_TREES_TOTAL})")
                if mhave < mneed:
                    return False, f"the mine went down with mines {mhave}/{mneed}"
            order_ok = sub_targets.index("seek_trees") < sub_targets.index("lay_mine")
            if not order_ok:
                return False, "lay_mine came before the harvest leg"
        if collected < len(SEA_PILLS):
            return False, (f"only {collected}/{len(SEA_PILLS)} sea pills were "
                           f"collected within one game minute")
        if done_tick is None:
            return False, "the cluster was never all out of the water in a snapshot"
        # ONE boat trip takes the whole cluster: one mine, one boat, and the
        # trees for them. More than that means the bot went ashore mid-trip and
        # paid for a second entrance — the thing the collect phase exists to
        # prevent, and what turned "three free pills" into 3 mines and 60 trees.
        if len(mine_dispatches) > 1:
            return False, (f"{len(mine_dispatches)} mines laid for one cluster "
                           f"(expected exactly 1)")
        if len(boat_dispatches) > 1:
            return False, (f"{len(boat_dispatches)} boats built for one cluster "
                           f"(expected exactly 1)")
        budget = SEA_PILL_TREES_TOTAL + SEA_TREES_PER_FOREST
        if trees_spent > budget:
            return False, (f"{trees_spent} trees spent on one cluster "
                           f"(expected <= {budget} = 21 + one harvest overshoot)")
        return True, (f"entrance={entrance} S={S}, boat built, "
                      f"{collected}/{len(SEA_PILLS)} pills taken by brain tick "
                      f"~{done_tick // 2} "
                      f"({len(mine_dispatches)} mine, {len(boat_dispatches)} boat, "
                      f"{trees_spent} trees)"
                      + (", refuel leg ran" if variant == "D" else "")
                      + (", harvest leg ran with the mine held back until "
                         "21 trees were in hand" if variant == "E" else ""))

    if variant == "B":
        covered = [r for r in reject_reasons if r.startswith("pills_covered_by_pill#")]
        if not covered:
            return False, ("no pills_covered_by_pill reject — the line-of-fire "
                           f"gate did not fire (rejects seen: {sorted(set(reject_reasons))})")
        if any_mine_laid:
            return False, "a mine was laid despite the cluster being covered"
        if "lay_mine" in sub_targets:
            return False, "entered lay_mine despite the cluster being covered"
        if boats:
            return False, f"a boat was built at {boats} despite the cluster being covered"
        return True, f"rejected with {covered[0]}, no mine, no boat"

    if variant == "F":
        if "no_trees_in_territory" not in reject_reasons:
            return False, ("expected REJECT no_trees_in_territory; got "
                           f"{sorted(set(reject_reasons))}")
        if any_mine_laid:
            return False, "a mine was laid even though the wood is not obtainable"
        if "lay_mine" in sub_targets:
            return False, "entered lay_mine even though the wood is not obtainable"
        if dispatches:
            return False, f"the LGM was dispatched ({dispatches[0]}) with no fundable plan"
        return True, "rejected no_trees_in_territory, no mine, no LGM trip"

    return False, f"unknown variant {variant}"


# ONE GAME MINUTE for every variant. -ticks counts SIM ticks and the brain runs
# at half that rate, so 6000 sim ticks == 3000 brain ticks == 60 s of play. The
# arena is sized so a full mine -> crater -> river -> boat -> three-pickup cycle
# fits inside it with room to spare; if a variant needs more than a minute here,
# that is a bug in the plan, not a budget to raise.
DEFAULT_TICKS = {v: 6000 for v in VARIANTS}
SNAP_INTERVAL = 100     # completion-tick resolution
PORTS = {"A": 50061, "B": 50062, "C": 50063, "D": 50064,
         "E": 50065, "F": 50066, "G": 50067}


def main():
    variant = "ALL"
    ticks = None
    build = DEFAULT_BUILD
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            variant = args[i + 1].upper(); i += 2
        elif args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        else:
            i += 1
    todo = list(VARIANTS) if variant == "ALL" else [variant]
    rc = 0
    for v in todo:
        print(f"-- variant {v} ({gametype_for(v)}) " + "-" * 40)
        try:
            r = run_one(v, ticks or DEFAULT_TICKS[v], build, PORTS[v])
        except subprocess.TimeoutExpired:
            print(f"FAIL ({v}): run timed out")
            r = 1
        rc |= r
    sys.exit(rc)


if __name__ == "__main__":
    main()
