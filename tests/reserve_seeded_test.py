#!/usr/bin/env python3
"""The tree reserve must not starve the tank's own jobs (GoalHunter 1.7).

Field incident 20260903_105448 bot2 t=34774.  Four pills aboard, 21 trees in
the tank, a damaged friendly pill beside it, and the pool refused the job
every tick: `BP_DENY ... reason=tree_reserve(41,21,1)`.  Reserve 41 was
base 4 + carried pills 16 + a retired sea plan 21, and the carried-pill
component was PILL_PLACE_TREE_COST per pill, uncapped -- so even with the sea
plan gone it read 20 against 21 trees and still refused a ONE-tree repair.

WHAT IS UNDER TEST (builder_pool.tree_reserve, builder_pool.update's
seed_ctx, builder.set_mode's seek_trees rung)
    1. the carried-pill component is ONE placement's worth
       (PILL_PLACE_TREE_COST), not 4 x carried;
    2. a SEEDED job -- one the tank's own goal handed to the pool -- is
       charged NO reserve at all: it asks for exactly its own tree cost.  The
       reserve is for opportunistic side-quests; a seeded job is the tank's
       own plan, so holding wood back from it reserves the job's wood against
       the job;
    3. the seek_trees gather stops at one placement's worth too, instead of
       4 x carried.

VARIANT `seeded` (rules 1 and 2)
    Nine healthy friendly pills are handed to the tank at sim tick 200, next
    to a worn friendly pill that needs three trees.  At nine carried the OLD
    reserve was 4 + 36 = 40 against the open loadout's 40 trees -- exactly
    nothing left -- and a seeded repair was charged the same 40.  Nine rather
    than four because the scenario API cannot set a tank's wood, so the arena
    moves the other side of the inequality; see the generator's header.
    CHECKS
      1. the `pills` chip on the BUILDER_POOL line is PILL_PLACE_TREE_COST
         whenever a pill is aboard and 0 when none is -- never 4 x carried;
      2. the repair goes out (BP_DISPATCH of a topup/rebuild on the worn
         pill) while the tank is carrying all nine;
      3. no BP_DENY on that pill ever names tree_reserve;
      4. and the pill really comes back up (the final JSON has it at full
         armour), so the dispatch was a repair and not just a row.

VARIANT `gather` (rule 3)
    A strict-loadout tank (0 trees) carrying four pills, on a map whose ENTIRE
    wood supply is two forest tiles = 8 trees.  The old gather asked for
    4 x 4 = 16, more wood than the map contains, so it could never finish and
    the pill could never be dropped.
    CHECKS
      5. every seek_trees tick declares goal need_trees == PILL_PLACE_TREE_COST
         (the `goal` chip of the reserve is b.need_trees);
      6. a pill is actually placed -- the final JSON has a pill standing on a
         tile that had none at the start.

Usage: python reserve_seeded_test.py [--variant seeded|gather|ALL]
                                     [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):        # pragma: no cover - old Pythons
    pass

from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
sys.path.insert(0, str(HERE))
from generate_reserve_seeded_map import (     # noqa: E402
    WORN_PILL, WORN_HP, SPARE_PILLS, GATHER_SPARES, FOREST_TILES, GIVE_TICK,
    PILL_PLACE_TREE_COST, PILLS_MAX_HEALTH, TANK_FULL_TREES, LGM_GATHER_TREE,
    VARIANTS, old_reserve, new_reserve, topup_trees_needed)

PORTS = {"seeded": 50144, "gather": 50145}
DEFAULT_TICKS = {"seeded": 4000, "gather": 6000}

POOL_RE = re.compile(
    r"BUILDER_POOL t=(\d+) owner=(\S+) elig=.*? cands=(\d+) ok=(\d+) "
    r"trees=(\d+)/res=(\d+) \(base (\d+) \+ pills (\d+) \+ goal (\d+) "
    r"\+ sea (\d+)\)")
DISPATCH_RE = re.compile(
    r"BP_DISPATCH t=(\d+) job=(\S+) target=\((\d+),(\d+)\)")
DENY_RE = re.compile(
    r"BP_DENY t=(\d+) job=(\S+) target=\((\d+),(\d+)\) reason=(\S+)")
CARRY_RE = re.compile(r"ENGINE_DUMP t=(\d+) .*? tr=(\d+) carry=(\d+)")
DUMP_RE = re.compile(r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\)")
LUA_ERR_RE = re.compile(r"(?:stack traceback|attempt to (?:index|call|compare"
                        r"|perform|concatenate))")


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


def play(variant, ticks, build_dir, out):
    label = f"reserve_{variant}"
    mapfile = HERE / f"reserve_{variant}.map"
    final = HERE / f"reserve_{variant}_final.json"
    stderr = HERE / f"reserve_{variant}_stderr.txt"
    scripted = variant == "gather"      # spawns its own tank, for the loadout

    ds = find_ds(build_dir)
    if not ds:
        out.append(f"FAIL: WinBoloDS not found under {build_dir}")
        return None, None
    subprocess.run([sys.executable,
                    str(HERE / "generate_reserve_seeded_map.py"),
                    "--variant", variant], check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                out.append(f"FAIL: {p.name} is locked -- a previous WinBoloDS "
                           f"run is still going.")
                return None, None

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label,
               WINBOLO_BRAIN_TIER="10")
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORTS[variant]),
           "-nolobby", "-gametype", "open",
           "-bots", "0" if scripted else "1", "-brain", str(BRAIN),
           # yesfull: these arenas are tiny and the test is about what the
           # reserve charges, not about finding anything.
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 4))

    sess = newest_session(build_dir, label)
    if not sess:
        out.append("FAIL: no debug session produced (is the cwd on a drive "
                   "with >50 GB free? -brain-debug records nothing otherwise)")
        return None, None
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        out.append(f"FAIL: brain crashed -- see {crashes[0]}")
        out.append(Path(crashes[0]).read_text(errors="ignore")[:1500])
        return None, None
    # A scripted spawn lands in whatever slot the server had free, so take the
    # biggest log rather than assuming bot0.
    logs = sorted(sess.glob("print2_bot*.log"), key=lambda p: p.stat().st_size)
    if not logs:
        out.append(f"FAIL: no print2_bot*.log under {sess}")
        return None, None
    out.append(f"  session: {sess.name} ({logs[-1].name})")
    fd = json.load(open(final)) if final.exists() else None
    return logs[-1].read_text(encoding="utf-8", errors="replace"), fd


def sanity(text, out):
    if LUA_ERR_RE.search(text):
        bad = next(ln for ln in text.splitlines() if LUA_ERR_RE.search(ln))
        out.append(f"FAIL: Lua error in the print2 log: {bad.strip()}")
        return False
    if not DUMP_RE.search(text):
        out.append("FAIL: no ENGINE_DUMP lines -- the bot never thought")
        return False
    return True


def check_seeded(text, fd, out):
    if not sanity(text, out):
        return 1
    pool = [(int(m.group(1)), int(m.group(5)), int(m.group(6)), int(m.group(7)),
             int(m.group(8)), int(m.group(9)), int(m.group(10)))
            for m in POOL_RE.finditer(text)]   # tick trees res base pills goal sea
    carry = {int(m.group(1)): (int(m.group(2)), int(m.group(3)))
             for m in CARRY_RE.finditer(text)}   # tick -> (trees, carried)
    if not pool:
        out.append("FAIL: no BUILDER_POOL verdict lines at all")
        return 1
    n_carried = len(SPARE_PILLS)
    loaded = [t for t, (_, c) in sorted(carry.items()) if c >= n_carried]
    if not loaded:
        best = max((c for _, c in carry.values()), default=0)
        out.append(f"FAIL: the tank never carried {n_carried} pills (best "
                   f"{best}) -- give_pill did not land. Grep the stderr for "
                   f"RESERVE_ARENA.")
        return 1
    t_loaded = loaded[0]
    out.append(f"  {n_carried} pill(s) aboard from brain tick {t_loaded} "
               f"(handed over at sim tick {GIVE_TICK})")

    # -- 1. the `pills` chip is one placement's worth, never 4 x carried -----
    bad = []
    for (t, trees, res, base, pills, goal, sea) in pool:
        c = carry.get(t, (None, None))[1]
        if c is None:
            continue
        want = PILL_PLACE_TREE_COST if c > 0 else 0
        if pills != want:
            bad.append((t, c, pills, want))
    if bad:
        t, c, got, want = bad[0]
        out.append(f"FAIL (1): t={t} carrying {c} pill(s) the reserve's "
                   f"`pills` chip read {got}, expected {want} "
                   f"(PILL_PLACE_TREE_COST when anything is aboard, 0 when "
                   f"nothing is). 4 x carried would be {c * PILL_PLACE_TREE_COST}.")
        return 1
    loaded_lines = [p for p in pool if carry.get(p[0], (0, 0))[1] >= n_carried]
    out.append(f"  1 OK: the `pills` chip is {PILL_PLACE_TREE_COST} on all "
               f"{len(loaded_lines)} verdict tick(s) with {n_carried} aboard "
               f"(4 x carried would be {n_carried * PILL_PLACE_TREE_COST}); "
               f"reserve total {sorted({p[2] for p in loaded_lines})} against "
               f"{sorted({p[1] for p in loaded_lines})} trees")

    # -- 2. the repair goes out while the tank is full of pills -------------
    worn = (str(WORN_PILL[0]), str(WORN_PILL[1]))
    fixes = [d for d in DISPATCH_RE.findall(text) if (d[2], d[3]) == worn]
    late = [d for d in fixes if int(d[0]) >= t_loaded]
    if not late:
        out.append(f"FAIL (2): the worn pill {WORN_PILL} ({WORN_HP}/"
                   f"{PILLS_MAX_HEALTH}, {topup_trees_needed()} tree(s) to "
                   f"fix) was never dispatched while the tank was carrying "
                   f"{n_carried}. "
                   + (f"Earlier dispatches: {fixes}." if fixes else
                      "It was never dispatched at all.")
                   + f" Old reserve at {n_carried} carried: "
                   f"{old_reserve(n_carried)} against {TANK_FULL_TREES} trees.")
        return 1
    out.append(f"  2 OK: BP_DISPATCH t={late[0][0]} job={late[0][1]} "
               f"target={WORN_PILL} with {n_carried} pill(s) aboard -- the old "
               f"reserve ({old_reserve(n_carried)}) left "
               f"{TANK_FULL_TREES - old_reserve(n_carried)} trees for a "
               f"{topup_trees_needed()}-tree job; the new one "
               f"({new_reserve(n_carried)}) leaves "
               f"{TANK_FULL_TREES - new_reserve(n_carried)}")

    # -- 3. and it was never refused for the reserve ------------------------
    refused = [d for d in DENY_RE.findall(text)
               if (d[2], d[3]) == worn and d[4].startswith("tree_reserve")]
    if refused:
        out.append(f"FAIL (3): BP_DENY t={refused[0][0]} target={WORN_PILL} "
                   f"reason={refused[0][4]} -- the tank's own job was refused "
                   f"for wood it is holding back from itself. "
                   f"{len(refused)} such line(s).")
        return 1
    other = sorted({d[4].split("(")[0] for d in DENY_RE.findall(text)
                    if (d[2], d[3]) == worn})
    out.append(f"  3 OK: no BP_DENY on {WORN_PILL} ever named tree_reserve"
               + (f" (other reasons seen: {', '.join(other)})" if other else ""))

    # -- 4. the pill came back up -------------------------------------------
    hp = None
    for pb in (fd or {}).get("pillboxes", []):
        if (pb.get("tx"), pb.get("ty")) == WORN_PILL:
            hp = pb.get("armor")
    if hp is None:
        out.append(f"FAIL (4): the final JSON has no pill on {WORN_PILL}")
        return 1
    if hp <= WORN_HP:
        out.append(f"FAIL (4): the worn pill ended at armour {hp}, no better "
                   f"than the {WORN_HP} it started with -- the dispatch never "
                   f"turned into a repair.")
        return 1
    out.append(f"  4 OK: the worn pill ended at armour {hp}/{PILLS_MAX_HEALTH} "
               f"(started {WORN_HP})")
    out.append("PASS (seeded): nine pills aboard no longer freeze the tank's "
               "own repair -- the carried reserve is one placement's worth and "
               "a seeded job is charged none of it.")
    return 0


def check_gather(text, fd, out):
    if not sanity(text, out):
        return 1
    pool = [(int(m.group(1)), m.group(2), int(m.group(5)), int(m.group(9)))
            for m in POOL_RE.finditer(text)]     # tick owner trees goalchip
    seek = [p for p in pool if p[1].startswith("place_pill_strategic/gather")
            or "/seek_trees" in p[1]]
    wood = len(FOREST_TILES) * LGM_GATHER_TREE
    if not seek:
        owners = sorted({p[1] for p in pool})[:8]
        out.append(f"FAIL (5): the tank never entered seek_trees. It has 0 "
                   f"trees and {len(GATHER_SPARES)} pill(s), so the redirect "
                   f"should fire. Owners seen: {owners}")
        return 1
    bad = [p for p in seek if p[3] != PILL_PLACE_TREE_COST]
    if bad:
        out.append(f"FAIL (5): t={bad[0][0]} owner={bad[0][1]} declared goal "
                   f"need_trees {bad[0][3]}, expected {PILL_PLACE_TREE_COST}. "
                   f"4 x carried would be "
                   f"{len(GATHER_SPARES) * PILL_PLACE_TREE_COST}, which is "
                   f"more than the {wood} trees this whole map contains.")
        return 1
    out.append(f"  5 OK: goal need_trees is {PILL_PLACE_TREE_COST} on all "
               f"{len(seek)} seek_trees/gather verdict tick(s) "
               f"(4 x {len(GATHER_SPARES)} carried = "
               f"{len(GATHER_SPARES) * PILL_PLACE_TREE_COST} > the {wood} "
               f"trees the map holds)")

    # -- 6. a pill was actually placed --------------------------------------
    started = set(GATHER_SPARES)
    placed = [(pb.get("tx"), pb.get("ty")) for pb in (fd or {}).get("pillboxes", [])
              if not pb.get("in_tank") and (pb.get("tx"), pb.get("ty")) not in started]
    if not placed:
        where = [(pb.get("tx"), pb.get("ty"), pb.get("in_tank"))
                 for pb in (fd or {}).get("pillboxes", [])]
        out.append(f"FAIL (6): no pill was ever dropped on a new tile -- the "
                   f"gather never funded a placement. Final pill positions: "
                   f"{where}")
        return 1
    trees = [int(m.group(2)) for m in CARRY_RE.finditer(text)]
    out.append(f"  6 OK: {len(placed)} pill(s) placed on new ground: {placed} "
               f"(trees peaked at {max(trees) if trees else '?'} of the "
               f"{wood} the map holds)")
    out.append("PASS (gather): the seek-trees gather stops at one placement's "
               "worth, so a tank carrying four pills on a two-tree map still "
               "gets one of them into the ground.")
    return 0


def run_one(variant, ticks, build_dir):
    out = []
    text, fd = play(variant, ticks, build_dir, out)
    if text is None:
        return 1, out
    rc = (check_seeded if variant == "seeded" else check_gather)(text, fd, out)
    return rc, out


def main():
    args = sys.argv[1:]
    take_asap_flag(args)
    variant, ticks, build = "ALL", None, DEFAULT_BUILD
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            variant = args[i + 1]; i += 2
        elif args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        else:
            i += 1
    print(pacing_line(""))
    print("=== tree reserve vs the tank's own jobs")
    rc = 0
    for v in (list(VARIANTS) if variant == "ALL" else [variant]):
        r, lines = run_one(v, ticks or DEFAULT_TICKS[v], build)
        print(f"-- variant {v} " + "-" * 46)
        for line in lines:
            print(line)
        rc |= r
    return rc


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.TimeoutExpired:
        print("FAIL: run timed out")
        sys.exit(1)
