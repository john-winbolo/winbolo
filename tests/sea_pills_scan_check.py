#!/usr/bin/env python3
"""Replay the SEA-PILL HARVEST plan against the recorded DH-Oil Rig incident.

Default target:

    build/debug_sessions/20260901_000042_1_loss_b6/print2_bot2.log, t=22736

    Three dead pills lay in the first two deep-sea columns off the east shore
    (game coords #7 friendly (146,121), #11 hostile (146,120), #1 hostile
    (147,120)).  Every 50 ticks the pool printed
    `CAPTURE_CAND ... hp=0 ... reject=deepsea_no_boat` for all three, for the
    last 5000 ticks of the game.  Bot2 stood at (131,136) with armour 25,
    25 shells, 14 trees, **0 mines**, LGM aboard.  Hostile pill #10 sits at
    (141,114); hostile base #13 at (143,115); an enemy tank was at (137,115).

This script answers, offline, what capture_pill's deep-sea branch would have
said there — above all whether pill #10 has a LINE onto the cluster, which is
what decides between a priced plan and `REJECT pills_covered_by_pill#10`.

Fidelity:
  * terrain   — read from data/maps/"DH-Oil Rig.map" and recentred exactly as
                bolo_map.c does, then given mapRead's ROAD-under-base and
                ROAD-under-pill-on-impassable fixes.  Shared with
                tests/take_cover_scan_check.py.
                CAVEAT: this is the terrain at TICK 0.  22 736 ticks of
                harvested forest, craters and built road are not in it — and
                on this shore the forest strip is exactly what blocks pill
                #10, so a felled tree could change the verdict.  The script
                prints the strip so the assumption is visible.
  * shell walk— a faithful port of brain_pathfinder.c simulate_shot_walk
                (shellSpawnPos with SHELL_START_ADD, utilCalcDistanceHP's
                8-bit fixed-point stepping, shellLifeTicks) plus shells.c's
                collision rule: the shell dies on the first tile that is
                impassable with onBoat=FALSE — BUILDING, HALFBUILDING, FOREST
                or BOAT (bolo_map.c mapIsPassable) — or on the first LIVE pill
                or base.  Water is passable: shells fly over river and sea.
                LIMITATION: it cannot call the real cpf.simulate_shot; there
                is no way to drive the C brain from a script.  The port is
                bit-comparable apart from the double-precision sin/cos, where
                the engine uses the same libm call, so any divergence would be
                a sub-brad rounding difference at grazing angles.
  * travel    — APPROXIMATE: a plain 8-neighbour Dijkstra over
                brain_pathfinder.c's terrain cost table (no live danger
                slate), borrowed from take_cover_scan_check.  Treat the S
                SCORES as indicative; the LINE-OF-FIRE verdicts are exact.

Usage:
    python tests/sea_pills_scan_check.py
    python tests/sea_pills_scan_check.py --session <dir> --tick N --bot 2 \
                                         --map "data/maps/DH-Oil Rig.map"
"""

import argparse
import heapq
import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

from take_cover_scan_check import (              # noqa: E402
    read_map, travel_costs, TERRAIN_NAME, TERRAIN_COST, MAP_W,
    T_BUILDING, T_RIVER, T_SWAMP, T_CRATER, T_ROAD, T_FOREST, T_RUBBLE,
    T_GRASS, T_HALFBUILD, T_BOAT, T_DEEPSEA, T_REFBASE, T_PILLBOX,
    build_pill_grid, pill_at)

# ── engine physics constants (src/bolo) ──────────────────────────────────
SHELL_SPEED = 32          # shells.h
SHELL_START_ADD = 6       # shells.h
SHELL_LIFE = 8            # shells.h
GUNSIGHT_MAX = 14         # a tank's longest sight
PILLBOX_FIRE_DISTANCE = 8.5   # pillbox.h
BRADIANS_MAX = 256.0
BRADIANS_EAST = 64
DEGREES_MAX = 360.0
RADIANS_MAX = 2.0 * 3.14159   # global.h — NOT math.pi, deliberately
TANK_SHIFT_MAPSIZE = 8

# ── constants.lua mirrors (the sea-pill block) ───────────────────────────
SEA_PILL_CLUSTER_RADIUS = 3
SEA_PILL_ENTRANCE_MAX_DIST = 12
SEA_PILL_PILL_SAFE_RANGE = 9
SEA_PILL_ENEMY_TANK_NEAR = 10
SEA_PILL_FIRE_DIST = 2
SEA_PILL_BOAT_STEP_COST = 12
SEA_PILL_DANGER_W = 2.0
SEA_PILL_COST_MULT = 0.3
SEA_PILL_COST_FLOOR = 5
SEA_PILL_TREES_TOTAL = 21
SEA_BOAT_TREES = 20
SEA_TREES_PER_FOREST = 4
SEA_TREES_RADIUS = 12
SEA_COMPONENT_BOX = 16
SEA_COMPONENT_MIN_WATER = 3
BAD_GROUND = 30.0

SEA_MINABLE = {T_GRASS, T_ROAD, T_FOREST, T_SWAMP, T_RUBBLE, T_CRATER}
SEA_STANDABLE = SEA_MINABLE
SHOT_STOPPERS = {T_BUILDING, T_HALFBUILD, T_FOREST, T_BOAT}
CARD = ((1, 0), (-1, 0), (0, 1), (0, -1))
D8 = ((0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1))


def round_half_up(v):
    return math.floor(v + 0.5) if v >= 0 else math.ceil(v - 0.5)


def calc_distance(angle, speed):
    """utilCalcDistance — integer per-tick offset."""
    a = angle - BRADIANS_EAST
    if a < 0:
        a += BRADIANS_MAX
    rad = ((DEGREES_MAX / BRADIANS_MAX) * a / DEGREES_MAX) * RADIANS_MAX
    return round_half_up(speed * math.cos(rad)), round_half_up(speed * math.sin(rad))


def calc_distance_hp(angle, speed):
    """utilCalcDistanceHP — 8-bit fixed-point per-tick step."""
    a = angle - BRADIANS_EAST
    if a < 0:
        a += BRADIANS_MAX
    rad = ((DEGREES_MAX / BRADIANS_MAX) * a / DEGREES_MAX) * RADIANS_MAX
    return (round_half_up(speed * math.cos(rad) * 256.0),
            round_half_up(speed * math.sin(rad) * 256.0))


def shell_angle_from_target(ox, oy, tx, ty):
    """shellAngleFromTarget, then simulate_shot's lroundf to an integer brad."""
    dx, dy = tx - ox, ty - oy
    if dx == 0 and dy == 0:
        return 0
    return round_half_up(math.atan2(dx, -dy) * 128.0 / math.pi) % 256


def shot_tiles(ox, oy, angle, shooter_pill, sight_len=0):
    """brain_pathfinder.c simulate_shot_walk — the tiles a shell crosses."""
    if shooter_pill:
        length = PILLBOX_FIRE_DISTANCE
    else:
        length = (sight_len if sight_len > 0 else GUNSIGHT_MAX) / 2.0
    ticks = 1 + int(SHELL_LIFE * length) - SHELL_START_ADD
    ticks = max(0, ticks)
    x_add, y_add = calc_distance(angle, SHELL_SPEED)
    x = ox + SHELL_START_ADD * x_add
    y = oy + SHELL_START_ADD * y_add
    x_step, y_step = calc_distance_hp(angle, SHELL_SPEED)
    x_acc = y_acc = 0
    out = []
    last = (-1, -1)
    for wx, wy in ((ox, oy), (x, y)):
        mx, my = (wx & 0xFFFF) >> TANK_SHIFT_MAPSIZE, (wy & 0xFFFF) >> TANK_SHIFT_MAPSIZE
        if (mx, my) != last:
            out.append((mx, my))
            last = (mx, my)
    for _ in range(ticks):
        x_acc += x_step
        y_acc += y_step
        x_move = x_acc >> 8
        y_move = y_acc >> 8
        x_acc -= x_move << 8
        y_acc -= y_move << 8
        x += x_move
        y += y_move
        mx, my = (x & 0xFFFF) >> TANK_SHIFT_MAPSIZE, (y & 0xFFFF) >> TANK_SHIFT_MAPSIZE
        if (mx, my) != last:
            out.append((mx, my))
            last = (mx, my)
    return out


def m2w(m):
    return (m << 8) | 0x80


def shot_reaches(t, live_pill_at, base_at, omx, omy, tmx, tmy, shooter_pill=True):
    """goals.lua sea_shot_reaches: does a shell from (omx,omy) reach (tmx,tmy)?
    Returns (reached, stop_tile, stop_reason)."""
    ox, oy = m2w(omx), m2w(omy)
    ang = shell_angle_from_target(ox, oy, m2w(tmx), m2w(tmy))
    for (mx, my) in shot_tiles(ox, oy, ang, shooter_pill):
        if (mx, my) == (tmx, tmy):
            return True, (mx, my), "reached"
        if (mx, my) == (omx, omy):
            continue
        tt = t[mx][my]
        if tt in SHOT_STOPPERS:
            return False, (mx, my), TERRAIN_NAME.get(tt, str(tt))
        if (mx, my) in live_pill_at:
            return False, (mx, my), "live pill #%d" % live_pill_at[(mx, my)]
        if (mx, my) in base_at:
            return False, (mx, my), "base #%d" % base_at[(mx, my)]
    return False, None, "out of range"


# ── session parsing ──────────────────────────────────────────────────────
KWDIAG_RE = re.compile(r"KWDIAG t=(\d+) pn=(\d+) B\[([^\]]*)\] P\[([^\]]*)\]")
KW_ITEM_RE = re.compile(r"(\d+)([hfn])\((\d+),(\d+)\)")
CAND_RE = re.compile(
    r"CAPTURE_CAND t=(\d+) id=(\d+) @\((\d+),(\d+)\) hp=(\d+) owner=(\w+).*?reject=(\S+)")
DUMP_RE = re.compile(
    r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\).*?arm=(\d+) sh=(\d+) mn=(\d+) tr=(\d+)")


def parse_session(log_path, tick):
    text = Path(log_path).read_text(errors="ignore")
    # KWDIAG is emitted every 8 ticks; take the first at or after `tick`.
    kw = None
    for m in KWDIAG_RE.finditer(text):
        if int(m.group(1)) >= tick:
            kw = m
            break
    if kw is None:
        raise SystemExit(f"no KWDIAG at/after t={tick} in {log_path}")
    bases, live_pills = [], []
    for it in KW_ITEM_RE.finditer(kw.group(3)):
        bases.append({"id": int(it.group(1)), "owner": it.group(2),
                      "mx": int(it.group(3)), "my": int(it.group(4))})
    for it in KW_ITEM_RE.finditer(kw.group(4)):
        live_pills.append({"id": int(it.group(1)), "owner": it.group(2),
                           "mx": int(it.group(3)), "my": int(it.group(4))})
    # Dead pills come from the pool's own CAPTURE_CAND rows (KWDIAG lists only
    # live ones). Take the LAST batch at or before `tick`.
    dead = {}
    for m in CAND_RE.finditer(text):
        if int(m.group(1)) > tick:
            break
        if int(m.group(5)) == 0:
            dead[int(m.group(2))] = {"id": int(m.group(2)),
                                     "mx": int(m.group(3)), "my": int(m.group(4)),
                                     "owner": m.group(6), "reject": m.group(7)}
    tank = None
    for m in DUMP_RE.finditer(text):
        if int(m.group(1)) >= tick:
            tank = {"mx": int(m.group(2)), "my": int(m.group(3)),
                    "armour": int(m.group(4)), "shells": int(m.group(5)),
                    "mines": int(m.group(6)), "trees": int(m.group(7))}
            break
    return {"kw_tick": int(kw.group(1)), "bases": bases,
            "live_pills": live_pills, "dead_pills": dead, "tank": tank}


# ── the plan model (goals.lua sea_plan_cluster) ──────────────────────────
def water_component(t, seeds):
    seen = set(seeds)
    q = list(seeds)
    x0 = min(s[0] for s in seeds) - SEA_COMPONENT_BOX
    x1 = max(s[0] for s in seeds) + SEA_COMPONENT_BOX
    y0 = min(s[1] for s in seeds) - SEA_COMPONENT_BOX
    y1 = max(s[1] for s in seeds) + SEA_COMPONENT_BOX
    i = 0
    while i < len(q):
        cx, cy = q[i]; i += 1
        for dx, dy in CARD:
            nx, ny = cx + dx, cy + dy
            if not (x0 <= nx <= x1 and y0 <= ny <= y1):
                continue
            if (nx, ny) in seen:
                continue
            if t[nx][ny] in (T_DEEPSEA, T_RIVER, T_BOAT):
                seen.add((nx, ny))
                q.append((nx, ny))
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", default=str(
        REPO / "build" / "debug_sessions" / "20260901_000042_1_loss_b6"))
    ap.add_argument("--bot", type=int, default=2)
    ap.add_argument("--tick", type=int, default=22736)
    ap.add_argument("--map", default=str(REPO / "data" / "maps" / "DH-Oil Rig.map"))
    args = ap.parse_args()

    log = Path(args.session) / f"print2_bot{args.bot}.log"
    if not log.exists():
        raise SystemExit(f"no {log}")
    st = parse_session(log, args.tick)
    t, mpills, mbases, _ = read_map(args.map)

    print(f"session : {args.session}")
    print(f"bot     : {args.bot}   tick {args.tick} (KWDIAG t={st['kw_tick']})")
    if st["tank"]:
        k = st["tank"]
        print(f"tank    : ({k['mx']},{k['my']}) armour={k['armour']} "
              f"shells={k['shells']} mines={k['mines']} trees={k['trees']}")
    # Cross-check the recentred map against the log's base table.
    log_bases = {(b["mx"], b["my"]) for b in st["bases"]}
    map_bases = {(b[0], b[1]) for b in mbases}
    print(f"map     : {args.map}")
    print(f"          bases from the .map {'MATCH' if log_bases == map_bases else 'DIFFER FROM'} "
          f"the log's KWDIAG table ({len(map_bases)} vs {len(log_bases)})")
    if log_bases != map_bases:
        print(f"          only in map: {sorted(map_bases - log_bases)}")
        print(f"          only in log: {sorted(log_bases - map_bases)}")

    # ── the cluster ────────────────────────────────────────────────────
    dead = sorted(st["dead_pills"].values(), key=lambda p: p["id"])
    # perception.lua's rule: a dead pill counts as afloat when at least 3 of its
    # 4 cardinal neighbours are deep sea. Another dead pill of the same raft
    # counts as water too (the pill overlay hides the terrain under it).
    dead_tiles = {(q["mx"], q["my"]) for q in dead}
    sea = []
    for p in dead:
        n = sum(1 for dx, dy in CARD
                if t[p["mx"] + dx][p["my"] + dy] == T_DEEPSEA
                or (p["mx"] + dx, p["my"] + dy) in dead_tiles)
        if n >= 3:
            sea.append(p)
    print()
    print(f"dead pills in the log at t<={args.tick}: "
          + ", ".join(f"#{p['id']}@({p['mx']},{p['my']}) {p['owner']} "
                      f"reject={p['reject']}" for p in dead))
    if not sea:
        print("none of them sits in open deep sea by the tick-0 terrain — "
              "nothing for the harvest branch to price")
        return 0
    clusters = []
    for p in sea:
        home = None
        for c in clusters:
            if any(abs(p["mx"] - q["mx"]) + abs(p["my"] - q["my"])
                   <= SEA_PILL_CLUSTER_RADIUS for q in c):
                home = c
                break
        if home is None:
            clusters.append([p])
        else:
            home.append(p)
    print(f"clusters: {len(clusters)}")
    for i, c in enumerate(clusters, 1):
        print(f"  C{i}: n={len(c)} " +
              " ".join(f"#{p['id']}({p['mx']},{p['my']})" for p in c))

    live_pill_at = {(p["mx"], p["my"]): p["id"] for p in st["live_pills"]}
    base_at = {(b["mx"], b["my"]): b["id"] for b in st["bases"]}
    hostiles = [p for p in st["live_pills"] if p["owner"] in ("h", "n")]

    for ci, cl in enumerate(clusters, 1):
        tiles = [(p["mx"], p["my"]) for p in cl]
        print()
        print(f"── cluster C{ci} " + "-" * 50)
        # 1. line of fire onto the pills
        # EUCLIDEAN, like the brain (U.edist): a pillbox's reach is a circle.
        threats = [p for p in hostiles
                   if any(math.hypot(p["mx"] - x, p["my"] - y) <= SEA_PILL_PILL_SAFE_RANGE
                          for (x, y) in tiles)]
        print(f"hostile/neutral pills within {SEA_PILL_PILL_SAFE_RANGE} tiles: "
              + (", ".join(f"#{p['id']}@({p['mx']},{p['my']})" for p in threats) or "none"))
        covered_by = None
        for p in threats:
            for (x, y) in tiles:
                ok, stop, why = shot_reaches(t, live_pill_at, base_at,
                                             p["mx"], p["my"], x, y)
                print(f"  pill #{p['id']}({p['mx']},{p['my']}) -> ({x},{y}): "
                      + ("REACHES" if ok else f"blocked at {stop} by {why}"))
                if ok and covered_by is None:
                    covered_by = p["id"]
        if covered_by is not None:
            print(f"VERDICT: REJECT pills_covered_by_pill#{covered_by} "
                  f"— one shell sinks the boat, so the whole cluster is off")
            continue

        # 2. entrance candidates
        comp = water_component(t, tiles)
        print(f"water component containing the cluster: {len(comp)} tiles "
              f"(BFS box +-{SEA_COMPONENT_BOX})")
        if len(comp) < len(tiles) + SEA_COMPONENT_MIN_WATER:
            print("VERDICT: REJECT pills_landlocked — a one-tile puddle is not "
                  "navigable water; there is nothing for a boat to sail")
            continue
        tank = st["tank"] or {"mx": 128, "my": 128}
        grid = build_pill_grid(t,
                               [{"mx": p["mx"], "my": p["my"], "hp": 15, "anger": 0.0}
                                for p in hostiles],
                               [], tree_hide=True)
        dist = travel_costs(t, grid, tank["mx"], tank["my"], limit=40)
        x0 = min(x for x, _ in tiles) - SEA_PILL_ENTRANCE_MAX_DIST
        x1 = max(x for x, _ in tiles) + SEA_PILL_ENTRANCE_MAX_DIST
        y0 = min(y for _, y in tiles) - SEA_PILL_ENTRANCE_MAX_DIST
        y1 = max(y for _, y in tiles) + SEA_PILL_ENTRANCE_MAX_DIST
        cands = []
        for my in range(max(0, y0), min(255, y1) + 1):
            for mx in range(max(0, x0), min(255, x1) + 1):
                cdist = min(abs(mx - x) + abs(my - y) for (x, y) in tiles)
                if not (0 < cdist <= SEA_PILL_ENTRANCE_MAX_DIST):
                    continue
                tt = t[mx][my]
                kind = ("existing_boat" if tt == T_BOAT else
                        "existing_river" if tt == T_RIVER else
                        "mine_crater" if tt in SEA_MINABLE else None)
                if kind is None:
                    continue
                if kind == "mine_crater":
                    wat = next(((mx + dx, my + dy) for dx, dy in CARD
                                if (mx + dx, my + dy) in comp), None)
                else:
                    wat = (mx, my) if (mx, my) in comp else None
                if wat is None:
                    continue
                hot = pill_at(grid, mx, my)
                rej = None
                if hot >= BAD_GROUND:
                    rej = f"hot({hot:.0f})"
                F = None
                if rej is None and kind == "mine_crater":
                    for dx, dy in D8:
                        fx, fy = mx + dx * SEA_PILL_FIRE_DIST, my + dy * SEA_PILL_FIRE_DIST
                        if not (0 <= fx < MAP_W and 0 <= fy < MAP_W):
                            continue
                        if t[fx][fy] not in SEA_STANDABLE:
                            continue
                        mid = (mx + dx, my + dy)
                        if t[mid[0]][mid[1]] in (T_BUILDING, T_HALFBUILD):
                            continue
                        key = pill_at(grid, fx, fy) - (100 if t[fx][fy] == T_FOREST else 0)
                        if F is None or key < F[2] or (key == F[2] and
                                                       (fy * 256 + fx) < (F[1] * 256 + F[0])):
                            F = (fx, fy, key)
                    if F is None:
                        rej = "no_F"
                if rej is None:
                    ok, stop, why = (False, None, None)
                    for p in threats:
                        ok, stop, why = shot_reaches(t, live_pill_at, base_at,
                                                     p["mx"], p["my"], mx, my)
                        if ok:
                            rej = f"lof(pill#{p['id']})"
                            break
                travel = dist.get((mx, my))
                if rej is None and travel is None:
                    rej = "unreachable"
                score = None
                if rej is None:
                    score = travel + cdist * SEA_PILL_BOAT_STEP_COST + hot * SEA_PILL_DANGER_W
                cands.append((score, kind, mx, my, F, travel, hot, cdist, rej))
        ok_cands = [c for c in cands if c[8] is None]
        print(f"entrance candidates: {len(cands)} scanned, {len(ok_cands)} viable")
        for rung in ("existing_boat", "existing_river", "mine_crater"):
            rc = sorted([c for c in ok_cands if c[1] == rung])
            if not rc:
                continue
            print(f"  rung {rung}: {len(rc)} viable, best 6:")
            for c in rc[:6]:
                print(f"    S=({c[2]},{c[3]}) score={c[0]:.0f} travel={c[5]:.0f} "
                      f"cdist={c[7]} pill_at={c[6]:.0f} "
                      f"F={(c[4][0], c[4][1]) if c[4] else '-'} "
                      f"terrain={TERRAIN_NAME.get(t[c[2]][c[3]])}")
            break
        rejected = {}
        for c in cands:
            if c[8]:
                rejected[c[8].split("(")[0]] = rejected.get(c[8].split("(")[0], 0) + 1
        if rejected:
            print("  rejected candidates by reason: "
                  + ", ".join(f"{k}x{v}" for k, v in sorted(rejected.items())))
        if not ok_cands:
            print("VERDICT: REJECT no_entrance / no_connected_entrance")
            continue

        best = None
        for rung in ("existing_boat", "existing_river", "mine_crater"):
            rc = sorted([c for c in ok_cands if c[1] == rung])
            if rc:
                best = rc[0]
                break
        score, kind, sx, sy, F, travel, hot, cdist, _ = best

        # 3. resources
        trees = (st["tank"] or {}).get("trees", 0)
        mines = (st["tank"] or {}).get("mines", 0)
        needs_mine = kind == "mine_crater"
        trees_need = SEA_BOAT_TREES + (1 if needs_mine else 0)
        short = max(0, trees_need - trees)
        need_tiles = -(-short // SEA_TREES_PER_FOREST)
        forest_ok = sum(1 for yy in range(max(0, sy - SEA_TREES_RADIUS),
                                          min(255, sy + SEA_TREES_RADIUS) + 1)
                        for xx in range(max(0, sx - SEA_TREES_RADIUS),
                                        min(255, sx + SEA_TREES_RADIUS) + 1)
                        if t[xx][yy] == T_FOREST and pill_at(grid, xx, yy) < BAD_GROUND)
        mine_bases = [b for b in st["bases"] if b["owner"] == "f"]
        leg_trees = short * 6
        leg_mines = 0.0
        refuel = None
        if needs_mine and mines < 1:
            best_b = None
            for b in mine_bases:
                c1 = dist.get((b["mx"], b["my"]))
                if c1 is None:
                    continue
                back = (abs(b["mx"] - sx) + abs(b["my"] - sy)) * 16
                det = max(0.0, c1 + back - travel)
                if best_b is None or det < best_b[0]:
                    best_b = (det, b["id"])
            if best_b is None:
                print("VERDICT: REJECT no_mines_anywhere (mines=0, no reachable "
                      "friendly base priced)")
                continue
            leg_mines, refuel = best_b
        boat = cdist * SEA_PILL_BOAT_STEP_COST
        gross = travel + leg_mines + leg_trees + boat
        cost = max(SEA_PILL_COST_FLOOR, gross * SEA_PILL_COST_MULT / len(cl))
        print()
        print(f"PLAN  entrance={kind} S=({sx},{sy}) F={(F[0], F[1]) if F else '-'} "
              f"water_component={len(comp)}")
        print(f"      travel{{{travel:.0f}}} + refuel_leg{{{leg_mines:.0f}}}"
              f"{f' via base #{refuel}' if refuel is not None else ''}"
              f" + tree_leg{{{leg_trees:.0f}}} + boat_path{{{boat:.0f}}}"
              f" = {gross:.0f}")

        print(f"      x {SEA_PILL_COST_MULT} / n{{{len(cl)}}} = {cost:.1f} "
              f"(floor {SEA_PILL_COST_FLOOR})")
        print(f"      trees {trees}/{trees_need} short {short} need_tiles "
              f"{need_tiles} forest_ok {forest_ok} within {SEA_TREES_RADIUS}t "
              f"| mines {mines}/{1 if needs_mine else 0}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
