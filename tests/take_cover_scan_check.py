#!/usr/bin/env python3
"""Replay the take_cover scan against a RECORDED session, offline.

Purpose: put the numbers the new pool-14 scan would produce side by side with
a real incident, so the constants can be judged before trusting them in a live
game — and so the in-brain TAKE_COVER row can later be diffed against a model
that was written independently of it.

Default target is the incident take_cover was built for:

    build/debug_sessions/20260901_000042_1_loss_b6/print2_bot3.log, t=16625

    Full tank (armour 40, 29 shells), carrying 1 pill, LGM DEAD, standing at
    (143,118) — inside hostile pill #10's fire at (141,114), with our own
    pill #13 at (142,116) being shot from 15 HP down to 1.  defend_pill made
    no bid (heat blocked by "taking_damage"), attack_tank was gated by
    pill_crossfire, repair needed the LGM, and the haul flee's base picker
    rejected every base as "empty" because a full tank needs nothing.  The
    pool winner was seek_trees@(144,118) at cost 34, one tile from a forest
    inside the angry pill's range, and the bot sat there for 130+ ticks.

What it models, and how faithfully:

  * terrain   — read from the .map, RECENTERED exactly as bolo_map.c does
                (mapRecenter shifts the terrain bbox midpoint to (126,126),
                but only when BOTH axes need it), then given mapRead's two
                post-load fixes (ROAD under bases, ROAD under pills on
                impassable ground).  DH-Oil Rig needs no shift — its base
                table already matches the log's KWDIAG line exactly, which is
                what the startup cross-check verifies.
                CAVEAT: this is the terrain at TICK 0.  Sixteen thousand ticks
                of harvested forest, craters and built road are not in it.
                The script detects the biggest single effect (tree cover,
                which divides a pill's danger by ten) by building the grid
                both ways and keeping whichever reproduces the tank tile's
                logged threat.pill_at.
  * pill_at   — gh_threat.c stamp_pill + apply_occlusion_all reimplemented:
                base = PILL_DANGER_BASE + PILL_DANGER_ANGER*anger, HP curve,
                radial falloff, tree-hide, per-tile terrain factor, then the
                per-pill LOS reduction (walls .20, trees .10, own pills .40,
                capped .80).  NO crossfire multiplier —
                C.CROSSFIRE_MULTIPLIER_ENABLED is false.
  * cover/odds— danger.lua's cover_weight / odds_weight ladders, exactly.
  * travel    — APPROXIMATE, and the one knob worth playing with.  A plain
                8-neighbour Dijkstra over brain_pathfinder.c's terrain cost
                table plus danger * --travel-danger.  The real smart_cost
                reads a live Dijkstra slate with wall shooting, boat handling
                and shell/mine drain in it, so treat travel — and therefore
                the PICK among near-equal tiles — as indicative.  The SAFETY
                columns are exact.  A "best shelter ignoring travel" list is
                printed after the table so the travel model cannot hide what
                the scan would have liked.

Usage:
    python tests/take_cover_scan_check.py
    python tests/take_cover_scan_check.py --session <dir> --tick N --bot 3 \
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

# ── constants.lua / brain_pathfinder.c mirrors ───────────────────────────
PILL_RANGE_MAP = 10
PILL_DANGER_BASE = 8.0
PILL_DANGER_ANGER = 200.0
PILL_DANGER_EDGE_FALLOFF = 0.5
PILLS_MAX_HEALTH = 15
HP_DAMAGE_FLOOR = 0.60
HP_DAMAGE_SCALE = 0.40
HP_DAMAGE_EXP = 0.6
TREE_FULL_HIDE_MULT = 0.1
TREE_PARTIAL_HIDE_MULT = 0.7
FOREST_TERRAIN_MULT = 1.5
HAZARD_NEIGHBOR_MULT = 1.5
MIN_TREEHIDE_DIST_MAP = 3

T_BUILDING, T_RIVER, T_SWAMP, T_CRATER, T_ROAD = 0, 1, 2, 3, 4
T_FOREST, T_RUBBLE, T_GRASS, T_HALFBUILD, T_BOAT = 5, 6, 7, 8, 9
T_DEEPSEA, T_REFBASE, T_PILLBOX = 10, 11, 12
TERRAIN_NAME = {0: "wall", 1: "river", 2: "swamp", 3: "crater", 4: "road",
                5: "forest", 6: "rubble", 7: "grass", 8: "halfwall",
                9: "boat", 10: "deepsea", 11: "base", 12: "pill"}
# threat.lua terrain factor = 16 / TERRAIN_SPEED[tile]
TERRAIN_SPEED = {T_ROAD: 16, T_REFBASE: 16, T_BOAT: 16, T_GRASS: 12,
                 T_FOREST: 6, T_RUBBLE: 3, T_SWAMP: 3, T_CRATER: 3,
                 T_RIVER: 3, T_DEEPSEA: 3, T_PILLBOX: 16,
                 T_BUILDING: 0, T_HALFBUILD: 0}
HAZARD_TERRAIN = {T_DEEPSEA, T_RIVER}
# brain_pathfinder.c terrain_cost_table (foot)
TERRAIN_COST = {T_BUILDING: 9999.0, T_RIVER: 8.0, T_SWAMP: 8.0, T_CRATER: 8.0,
                T_ROAD: 1.0, T_FOREST: 3.0, T_RUBBLE: 8.0, T_GRASS: 2.0,
                T_HALFBUILD: 9999.0, T_BOAT: 2.0, T_DEEPSEA: 9999.0,
                T_REFBASE: 1.0, T_PILLBOX: 2.0}
DIJKSTRA_DANGER_SCALE = 0.1     # C.DIJKSTRA_DANGER_SCALE-ish; travel is a model

# danger.lua ladders
IMD_COVER_W_NEAR, IMD_COVER_W_MID, IMD_COVER_W_FAR = 1.00, 0.75, 0.25
IMD_COVER_HEATED_MULT, HEATED_ANGER = 3, 0.6
IMD_COVER_MAX, IMD_COVER_PER_UNIT = 30, 10
IMD_ODDS_W_NEAR, IMD_ODDS_W_FAR = 0.75, 0.25
IMD_ODDS_NEAR_TILES, IMD_ODDS_FAR_TILES = 8, 15

# constants.lua "take_cover (2026-09-01)"
W_COVER, W_EXPO, W_ENEMY, W_ALLY, W_TRAVEL = 8.0, 0.15, 20.0, 4.0, 0.6
MIN_MARGIN, BASE_COST, K = 8.0, 60.0, 1.0
HAUL_FLOOR = 10.0
BAD_GROUND = 30.0
PILL_NEIGHBOUR_RANGE, BASE_TILE_RANGE = 15, 15

MAP_W = 256


def cover_weight(d):
    if d <= 4:
        return IMD_COVER_W_NEAR
    if d <= 8:
        return IMD_COVER_W_MID
    if d <= 9:
        return IMD_COVER_W_FAR
    return 0.0


def odds_weight(d):
    if d <= IMD_ODDS_NEAR_TILES:
        return IMD_ODDS_W_NEAR
    if d <= IMD_ODDS_FAR_TILES:
        return IMD_ODDS_W_FAR
    return 0.0


# ── .map reader (bolo_map.c mapProcessRun + mapRecenter) ─────────────────
MAP_RUN_DIFF, MAP_RUN_SAME = 8, 6


def read_map(path):
    """Returns (terrain[x][y], pills, bases, starts) in GAME coordinates."""
    b = Path(path).read_bytes()
    assert b[:8] == b"BMAPBOLO", f"{path} is not a Bolo map"
    p = 8
    _ver, n_pill, n_base, n_start = b[p], b[p + 1], b[p + 2], b[p + 3]
    p += 4
    pills, bases, starts = [], [], []
    for _ in range(n_pill):
        pills.append(list(b[p:p + 5])); p += 5
    for _ in range(n_base):
        bases.append(list(b[p:p + 6])); p += 6
    for _ in range(n_start):
        starts.append(list(b[p:p + 3])); p += 3

    t = [[T_DEEPSEA] * MAP_W for _ in range(MAP_W)]   # t[x][y], like mapItem
    while p + 4 <= len(b):
        ln, y, sx, ex = b[p], b[p + 1], b[p + 2], b[p + 3]
        if (ln, y, sx, ex) == (4, 255, 255, 255):
            break
        data = b[p + 4:p + ln]
        p += ln
        _decode_run(t, data, y, sx)

    _recenter(t, pills, bases, starts)
    # mapRead's two post-load terrain fixes (bolo_map.c, right after
    # mapCenter): ROAD under every base, and ROAD under any pill sitting on
    # impassable terrain. Without these a base tile reads as whatever the run
    # encoded and the travel model prices it wrong.
    for b in bases:
        t[b[0]][b[1]] = T_ROAD
    for q in pills:
        if t[q[0]][q[1]] in (T_RIVER, T_DEEPSEA, T_BUILDING, T_HALFBUILD):
            t[q[0]][q[1]] = T_ROAD
    return t, pills, bases, starts


def _decode_run(t, data, y, start_x):
    """Nibble state machine, mirroring mapProcessRun."""
    nibbles = []
    for byte in data:
        nibbles.append(byte >> 4)
        nibbles.append(byte & 0x0F)
    i, x = 0, start_x
    while i < len(nibbles):
        ln = nibbles[i]; i += 1
        if ln < MAP_RUN_DIFF:            # `diff`: ln+1 literal nibbles
            for _ in range(ln + 1):
                if i >= len(nibbles):
                    return
                if x < MAP_W:
                    t[x][y] = nibbles[i]
                i += 1; x += 1
        else:                            # `same`: ln-MAP_RUN_SAME copies
            if i >= len(nibbles):
                return
            tile = nibbles[i]; i += 1
            for _ in range(ln - MAP_RUN_SAME):
                if x < MAP_W:
                    t[x][y] = tile
                x += 1


def _recenter(t, pills, bases, starts):
    """mapRecenter: shift the written-tile bbox midpoint to (126,126), but
    ONLY when both axes need it (`if (addX != 0 && addY != 0)`)."""
    left, right, top, bottom = MAP_W, -1, MAP_W, -1
    for x in range(MAP_W):
        col = t[x]
        for y in range(MAP_W):
            if col[y] != T_DEEPSEA:
                left = min(left, x); right = max(right, x)
                top = min(top, y); bottom = max(bottom, y)
    if right < left or bottom < top:
        return 0, 0
    add_x = 127 - (left + right) // 2 - 1
    add_y = 127 - (top + bottom) // 2 - 1
    if add_x == 0 or add_y == 0:
        return 0, 0
    new = [[T_DEEPSEA] * MAP_W for _ in range(MAP_W)]
    for x in range(left, right + 1):
        for y in range(top, bottom + 1):
            new[(x + add_x) & 0xFF][(y + add_y) & 0xFF] = t[x][y]
    for x in range(MAP_W):
        t[x][:] = new[x]
    for rec in pills + bases + starts:
        rec[0] = (rec[0] + add_x) & 0xFF
        rec[1] = (rec[1] + add_y) & 0xFF
    return add_x, add_y


# ── session log parsing ──────────────────────────────────────────────────
DUMP_RE = re.compile(
    r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\).*?arm=(\d+) sh=(\d+) mn=(\d+) "
    r"tr=(\d+) carry=(\d+) man=(\d+) boat=(\w+).*?\| OBJ=(.*?) \| EVT=")
OBJ_RE = re.compile(r"ty(\d+)#(\d+)@\((\d+),(\d+)\)info=0x([0-9a-fA-F]+)")
HEAT_RE = re.compile(r"HEAT_GATE t=(\d+) pill@\((\d+),(\d+)\).*?hp=(\d+) "
                     r"anger=([\d.]+)")
EXPO_RE = re.compile(r"expo\{[-+\d.]+\}\(pill_at=([\d.]+)\)")

OBJ_TANK, OBJ_PILLBOX, OBJ_BASE = 0, 2, 3
INFO_HOSTILE = 0x1


def parse_tick(log_path, tick):
    """Pull the engine state at `tick` out of a print2 log."""
    text = Path(log_path).read_text(errors="ignore")
    dump = None
    for m in DUMP_RE.finditer(text):
        if int(m.group(1)) == tick:
            dump = m
            break
    if dump is None:
        raise SystemExit(f"no ENGINE_DUMP for t={tick} in {log_path}")
    state = {
        "tick": tick,
        "tank": (int(dump.group(2)), int(dump.group(3))),
        "armour": int(dump.group(4)), "shells": int(dump.group(5)),
        "mines": int(dump.group(6)), "trees": int(dump.group(7)),
        "carry": int(dump.group(8)), "man": int(dump.group(9)),
        "boat": dump.group(10) == "true",
        "pills": [], "tanks": [], "bases": [],
    }
    for o in OBJ_RE.finditer(dump.group(11)):
        ty, idn = int(o.group(1)), int(o.group(2))
        x, y, info = int(o.group(3)), int(o.group(4)), int(o.group(5), 16)
        rec = {"id": idn, "mx": x, "my": y,
               "hostile": bool(info & INFO_HOSTILE)}
        if ty == OBJ_PILLBOX:
            state["pills"].append(rec)
        elif ty == OBJ_TANK:
            state["tanks"].append(rec)
        elif ty == OBJ_BASE:
            state["bases"].append(rec)

    # Per-pill hp/anger, from the HEAT_GATE lines the same tick emitted (only
    # our own pills get one, but they are the ones that drive the cover term).
    heat = {}
    for m in HEAT_RE.finditer(text):
        if int(m.group(1)) == tick:
            heat[(int(m.group(2)), int(m.group(3)))] = (int(m.group(4)),
                                                        float(m.group(5)))
    state["heat"] = heat
    # The brain's own threat.pill_at at the tank tile, from the SCORES line —
    # the one number this model can be graded against.
    logged = None
    idx = text.find(f"SCORES t={tick} ")
    if idx >= 0:
        m = EXPO_RE.search(text, idx, idx + 2000)
        if m:
            logged = float(m.group(1))
    state["logged_pill_at"] = logged
    return state


# ── threat.pill_at model (gh_threat.c) ───────────────────────────────────
def terrain_factor(t, x, y):
    tt = t[x][y]
    spd = TERRAIN_SPEED.get(tt, 3) or 3
    m = 16.0 / spd
    if tt == T_FOREST:
        m = FOREST_TERRAIN_MULT
    for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
        if 0 <= nx < MAP_W and 0 <= ny < MAP_W and t[nx][ny] in HAZARD_TERRAIN:
            m *= HAZARD_NEIGHBOR_MULT
            break
    return m


def in_trees(t, x, y):
    if t[x][y] != T_FOREST:
        return False
    for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
        if not (0 <= nx < MAP_W and 0 <= ny < MAP_W) or t[nx][ny] != T_FOREST:
            return False
    return True


def bresenham(x0, y0, x1, y1):
    """Intermediate tiles only (neither endpoint) — U.bresenham's convention."""
    pts = []
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    x, y = x0, y0
    while True:
        if (x, y) not in ((x0, y0), (x1, y1)):
            pts.append((x, y))
        if x == x1 and y == y1:
            return pts
        e2 = 2 * err
        if e2 > -dy:
            err -= dy; x += sx
        if e2 < dx:
            err += dx; y += sy


def build_pill_grid(t, hostiles, friendlies, tree_hide=True):
    """threat.pill_at for every tile in range of a hostile/neutral pill.
    hostiles/friendlies: [{mx,my,anger,health}]. Mirrors stamp_pill then
    apply_occlusion_all (per-pill factor applied to the SHARED grid, which is
    what the C does)."""
    grid = {}
    fset = {(p["mx"], p["my"]) for p in friendlies}
    for p in hostiles:
        base = PILL_DANGER_BASE + PILL_DANGER_ANGER * p.get("anger", 0.0)
        hp = p.get("health", PILLS_MAX_HEALTH)
        if hp >= PILLS_MAX_HEALTH:
            base *= HP_DAMAGE_FLOOR + HP_DAMAGE_SCALE
        elif hp <= 0:
            base *= HP_DAMAGE_FLOOR
        else:
            base *= HP_DAMAGE_FLOOR + HP_DAMAGE_SCALE * (
                (hp / PILLS_MAX_HEALTH) ** HP_DAMAGE_EXP)
        for dy in range(-PILL_RANGE_MAP, PILL_RANGE_MAP + 1):
            for dx in range(-PILL_RANGE_MAP, PILL_RANGE_MAP + 1):
                d2 = dx * dx + dy * dy
                if d2 > PILL_RANGE_MAP ** 2:
                    continue
                nx, ny = p["mx"] + dx, p["my"] + dy
                if not (0 <= nx < MAP_W and 0 <= ny < MAP_W):
                    continue
                d = math.sqrt(d2)
                pen = base * (1.0 - PILL_DANGER_EDGE_FALLOFF * (d / PILL_RANGE_MAP))
                if tree_hide and in_trees(t, nx, ny):
                    pen *= (TREE_FULL_HIDE_MULT if d >= MIN_TREEHIDE_DIST_MAP
                            else TREE_PARTIAL_HIDE_MULT)
                pen *= terrain_factor(t, nx, ny)
                if pen > 0:
                    grid[(nx, ny)] = grid.get((nx, ny), 0.0) + pen
    # Occlusion pass, one pill at a time, over the accumulated grid.
    for p in hostiles:
        for dy in range(-PILL_RANGE_MAP, PILL_RANGE_MAP + 1):
            for dx in range(-PILL_RANGE_MAP, PILL_RANGE_MAP + 1):
                d2 = dx * dx + dy * dy
                if d2 > PILL_RANGE_MAP ** 2 or d2 < 4:
                    continue
                nx, ny = p["mx"] + dx, p["my"] + dy
                cur = grid.get((nx, ny), 0.0)
                if cur <= 0:
                    continue
                walls = trees = fpills = 0
                for (bx, by) in bresenham(p["mx"], p["my"], nx, ny):
                    tt = t[bx][by]
                    if tt in (T_BUILDING, T_HALFBUILD):
                        walls += 1
                    elif tt == T_FOREST:
                        trees += 1
                    if (bx, by) in fset:
                        fpills += 1
                red = min(0.80, walls * 0.20 + trees * 0.10 + fpills * 0.40)
                if red > 0:
                    grid[(nx, ny)] = cur * (1.0 - red)
    return grid


def pill_at(grid, x, y):
    return grid.get((x, y), 0.0)


# ── travel model ─────────────────────────────────────────────────────────
def travel_costs(t, grid, sx, sy, limit=24, danger_w=DIJKSTRA_DANGER_SCALE):
    """8-neighbour Dijkstra over brain_pathfinder.c's terrain costs plus a
    danger term. APPROXIMATE — the real smart_cost reads a live slate."""
    dist = {(sx, sy): 0.0}
    pq = [(0.0, sx, sy)]
    while pq:
        c, x, y = heapq.heappop(pq)
        if c > dist.get((x, y), 1e18):
            continue
        if abs(x - sx) > limit or abs(y - sy) > limit:
            continue
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                nx, ny = x + dx, y + dy
                if not (0 <= nx < MAP_W and 0 <= ny < MAP_W):
                    continue
                tc = TERRAIN_COST.get(t[nx][ny], 9999.0)
                if tc >= 9999.0:
                    continue
                step = tc * (math.sqrt(2) if dx and dy else 1.0)
                step += pill_at(grid, nx, ny) * danger_w
                nc = c + step
                if nc < dist.get((nx, ny), 1e18):
                    dist[(nx, ny)] = nc
                    heapq.heappush(pq, (nc, nx, ny))
    return dist


# ── the scan itself (goals.lua eval_take_cover / M.find_cover_tile) ──────
def standable(t, x, y, boat):
    tt = t[x][y]
    if tt == T_DEEPSEA:
        return "deepsea"
    if tt in (T_BUILDING, T_HALFBUILD):
        return "wall"
    if tt == T_RIVER and not boat:
        return "river"
    if tt == T_PILLBOX:
        return "pillbox"
    return None


def safety(grid, x, y, own_pills, enemies, allies, tank):
    units, ncov, nhot = 0.0, 0, 0
    for p in own_pills:
        w = cover_weight(math.dist((x, y), (p["mx"], p["my"])))
        if w > 0:
            hot = p.get("anger", 0.0) >= HEATED_ANGER
            units += w * (IMD_COVER_HEATED_MULT if hot else 1)
            ncov += 1
            nhot += 1 if hot else 0
    units = min(units, IMD_COVER_MAX / IMD_COVER_PER_UNIT)
    expo = pill_at(grid, x, y)
    en, ne = 0.0, 0
    closer = False
    for e in enemies:
        d = math.dist((x, y), (e["mx"], e["my"]))
        w = odds_weight(d)
        if w > 0:
            en += w; ne += 1
        if d < math.dist(tank, (e["mx"], e["my"])):
            closer = True
    al, na = 0.0, 0
    for a in allies:
        w = odds_weight(math.dist((x, y), (a["mx"], a["my"])))
        if w > 0:
            al += w; na += 1
    s = W_COVER * units - W_EXPO * expo - W_ENEMY * en + W_ALLY * al
    return {"mx": x, "my": y, "cover": units, "cover_n": ncov,
            "cover_hot": nhot, "expo": expo, "enemy": en, "n_enemy": ne,
            "ally": al, "n_ally": na, "closer": closer, "safety": s}


def run_scan(t, st, args):
    tank = st["tank"]
    own_pills = [p for p in st["pills"] if not p["hostile"]]
    hostiles = [p for p in st["pills"] if p["hostile"]]
    for p in own_pills + hostiles:
        hp, anger = st["heat"].get((p["mx"], p["my"]), (None, None))
        p["health"] = hp if hp is not None else PILLS_MAX_HEALTH
        if anger is not None:
            p["anger"] = anger
        else:
            p["anger"] = args.hostile_anger if p["hostile"] else 0.0
    enemies = [o for o in st["tanks"] if o["hostile"]]
    allies = [o for o in st["tanks"] if not o["hostile"]]

    # The .map is the terrain at tick 0.  By t=16625 the bots have harvested
    # forest, cratered ground and paved roads, and the single biggest effect
    # on threat.pill_at is tree cover (a full-hide forest tile divides the
    # pill's danger by ten).  So build the grid BOTH ways and keep whichever
    # reproduces the tank tile's logged threat.pill_at — that is the one
    # number the log gives us to calibrate against.
    grid_h = build_pill_grid(t, hostiles, own_pills, tree_hide=True)
    grid_n = build_pill_grid(t, hostiles, own_pills, tree_hide=False)
    logged0 = st["logged_pill_at"]
    tree_note = "tick-0 forest kept"
    grid = grid_h
    if logged0 is not None:
        if abs(pill_at(grid_n, *tank) - logged0) < abs(pill_at(grid_h, *tank) - logged0):
            grid = grid_n
            tree_note = ("tree-hide DISABLED — the tick-0 map's forest around "
                         "the tank had been harvested by this tick")
    dist = travel_costs(t, grid, *tank, danger_w=args.travel_danger)

    print(f"session tick {st['tick']}  tank=({tank[0]},{tank[1]}) "
          f"terrain={TERRAIN_NAME.get(t[tank[0]][tank[1]], '?')} "
          f"arm={st['armour']} sh={st['shells']} carry={st['carry']} "
          f"man={st['man']} boat={st['boat']}")
    print(f"  hostile/neutral pills: "
          + ", ".join(f"#{p['id']}@({p['mx']},{p['my']}) anger={p['anger']:.2f}"
                      f" hp={p['health']}" for p in hostiles))
    print(f"  team pills: "
          + ", ".join(f"#{p['id']}@({p['mx']},{p['my']}) anger={p['anger']:.2f}"
                      f" hp={p['health']}" for p in own_pills))
    print(f"  enemy tanks: "
          + (", ".join(f"#{e['id']}@({e['mx']},{e['my']})" for e in enemies)
             or "none"))
    modelled = pill_at(grid, *tank)
    logged = st["logged_pill_at"]
    print(f"  threat.pill_at at the tank: modelled {modelled:.0f}"
          + (f", brain logged {logged:.0f}" if logged is not None else "")
          + f"   (bad ground is >= {BAD_GROUND:.0f})")
    print(f"  terrain model: {tree_note}"
          f"  [tree-hide on -> {pill_at(grid_h, *tank):.0f},"
          f" off -> {pill_at(grid_n, *tank):.0f}]")
    if logged is not None and modelled > 0 and abs(modelled - logged) > 0.25 * logged:
        print("  WARNING: the model and the brain disagree by more than 25% on"
              " the tank tile. The .map is tick-0 terrain; craters, harvested"
              " forest and built road since then are not in it, so read the"
              " table below as shape, not as exact numbers.")

    here = safety(grid, tank[0], tank[1], own_pills, enemies, allies, tank)

    cands, seen = [], set()

    def consider(x, y, src):
        x = max(0, min(255, x)); y = max(0, min(255, y))
        if (x, y) == tank or (x, y) in seen:
            return
        seen.add((x, y))
        why = standable(t, x, y, st["boat"])
        if why:
            cands.append({"mx": x, "my": y, "src": src, "reject": why})
            return
        trav = dist.get((x, y))
        if trav is None:
            cands.append({"mx": x, "my": y, "src": src, "reject": "unreachable"})
            return
        c = safety(grid, x, y, own_pills, enemies, allies, tank)
        c["src"] = src
        c["travel"] = trav
        c["adj"] = c["safety"] - W_TRAVEL * trav
        c["reject"] = "toward_enemy" if c["closer"] else None
        cands.append(c)

    for r in (3, 6, 9, 12):
        for i in range(8):
            ang = i * math.pi / 4
            consider(tank[0] + math.floor(r * math.sin(ang) + 0.5),
                     tank[1] - math.floor(r * math.cos(ang) + 0.5), f"ring{r}")
    for p in sorted(own_pills, key=lambda q: q["id"]):
        if max(abs(p["mx"] - tank[0]), abs(p["my"] - tank[1])) <= PILL_NEIGHBOUR_RANGE:
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx or dy:
                        consider(p["mx"] + dx, p["my"] + dy, f"pill{p['id']}")
    for b in sorted(st["bases"], key=lambda q: q["id"]):
        if not b["hostile"] and \
                max(abs(b["mx"] - tank[0]), abs(b["my"] - tank[1])) <= BASE_TILE_RANGE:
            consider(b["mx"], b["my"], f"base{b['id']}")

    live = [c for c in cands if not c["reject"]]
    live.sort(key=lambda c: (-c["adj"], c["travel"], c["my"] * 256 + c["mx"]))
    best = live[0] if live else None

    print()
    print("  tile      src      terrain  cover  expo  enemy  ally   safety "
          "travel    adj  note")
    def row(c, note=""):
        if c.get("reject"):
            print(f"  ({c['mx']:3d},{c['my']:3d}) {c['src']:<8} "
                  f"{TERRAIN_NAME.get(t[c['mx']][c['my']], '?'):<8} "
                  f"{'':>38}  REJECT {c['reject']}")
            return
        print(f"  ({c['mx']:3d},{c['my']:3d}) {c.get('src', 'HERE'):<8} "
              f"{TERRAIN_NAME.get(t[c['mx']][c['my']], '?'):<8} "
              f"{c['cover']:5.2f} {c['expo']:5.0f} {c['enemy']:6.2f} "
              f"{c['ally']:5.2f} {c['safety']:7.1f} "
              f"{c.get('travel', 0):6.1f} {c.get('adj', 0):6.1f}  {note}")
    row(here, "HERE (the margin baseline)")
    for c in live:
        row(c, "<== PICK" if c is best else "")
    for c in cands:
        if c.get("reject"):
            row(c)

    print()
    by_safety = sorted(live, key=lambda c: -c["safety"])[:5]
    print("  best shelter IGNORING travel (what the scan would pick if getting"
          " there were free):")
    for c in by_safety:
        print(f"    ({c['mx']:3d},{c['my']:3d}) {c['src']:<8} safety "
              f"{c['safety']:6.1f}  expo {c['expo']:5.0f}  travel "
              f"{c['travel']:6.1f}")

    print()
    if best is None:
        print("  REJECT unreachable — no standable, non-toward-enemy tile")
        return
    margin = best["safety"] - here["safety"]
    bad_ground = here["expo"] >= BAD_GROUND
    haul = st["carry"] >= 1 and st["man"] in (1, 2) and here["expo"] > 0
    if haul:
        trig, cost = "haul", HAUL_FLOOR
        detail = (f"carry={st['carry']} man={st['man']} (LGM dead/out) and "
                  f"threat.pill_at({tank[0]},{tank[1]})={here['expo']:.0f} > 0")
    elif bad_ground:
        trig, cost = "bad_ground", max(1.0, BASE_COST - K * margin)
        detail = f"pill_at {here['expo']:.0f} >= {BAD_GROUND:.0f}"
    elif margin >= MIN_MARGIN:
        trig, cost = "calm", max(1.0, BASE_COST - K * margin)
        detail = f"margin {margin:.1f} >= MIN_MARGIN {MIN_MARGIN:.0f}"
    else:
        trig, cost, detail = "none", None, (
            f"margin {margin:.1f} < MIN_MARGIN {MIN_MARGIN:.0f}, no trigger")
    print(f"  pick ({best['mx']},{best['my']}) via {best['src']}  "
          f"margin = best {best['safety']:.1f} - here {here['safety']:.1f} "
          f"= {margin:.1f}")
    print(f"  trigger {trig}: {detail}")
    print(f"  pool-14 cost = "
          + (f"{cost:.0f}" if cost is not None else "REJECT no_safer_tile"))
    print(f"  (for reference, the goal that actually won this replan was "
          f"seek_trees at cost 34)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", default=str(
        REPO / "build" / "debug_sessions" / "20260901_000042_1_loss_b6"))
    ap.add_argument("--bot", type=int, default=3)
    ap.add_argument("--tick", type=int, default=16625)
    ap.add_argument("--map", default=str(REPO / "data" / "maps" / "DH-Oil Rig.map"))
    ap.add_argument("--travel-danger", type=float, default=0.1,
                    help="weight on threat.pill_at per tile in the TRAVEL "
                         "model (not the safety score). Higher = the scan "
                         "refuses to cross danger to reach shelter")
    ap.add_argument("--hostile-anger", type=float, default=1.0,
                    help="anger assumed for hostile pills the log does not "
                         "report (HEAT_GATE only reports our own); the "
                         "incident's pill #10 was at 1.00")
    args = ap.parse_args()

    log = Path(args.session) / f"print2_bot{args.bot}.log"
    if not log.exists():
        raise SystemExit(f"no such log: {log}")
    t, mpills, mbases, _ = read_map(args.map)
    st = parse_tick(log, args.tick)

    # Cross-check on BASES, not pills: bases never move, while pills are
    # picked up and replanted all game (only the handful still sitting on
    # their original pedestals would ever match). If the base positions the
    # engine reported are not in the map's base table, we have the wrong map
    # or a bad recenter, and every coordinate below is meaningless.
    mb = {(b[0], b[1]) for b in mbases}
    ok = sum(1 for b in st["bases"] if (b["mx"], b["my"]) in mb)
    print(f"map {Path(args.map).name}: {len(mpills)} pills, {len(mbases)} bases;"
          f" {ok}/{len(st['bases'])} visible bases match a map-file base"
          f" position")
    if st["bases"] and ok < len(st["bases"]):
        print("  WARNING: base positions do not line up — wrong map, or the "
              "recenter model is off. Numbers below are not trustworthy.")
    print()
    run_scan(t, st, args)


if __name__ == "__main__":
    main()
