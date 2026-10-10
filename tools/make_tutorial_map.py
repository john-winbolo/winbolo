#!/usr/bin/env python3
"""
Build data/maps/Tutorial.map, the map the Tutorial scenario plays on, and
rewrite the layout block in data/maps/Tutorial.scenario.lua so the script
and the map always name the same squares.

    C:\\Python310\\python.exe tools/make_tutorial_map.py

The map is seven stations stacked up a column, Station 1 at the south
(bottom) and Station 7 at the north (top), with a deep-water trench down
the west side. Each station's checkpoint start sits in its own stretch of
the trench: forest dams cut the trench into one pool per station, so a tank
that respawns on its boat cannot sail into another station.

Coordinates are written so the terrain's bounding box is centred on square
126 both ways. mapRead (bolo_map.c mapCenter) moves a map whose box is not
centred there; this one it leaves alone, so the squares below are the
squares the game uses. The script's own writes must stay on squares 21 to
235; everything here is inside 24..228.

Map file format (BMAPBOLO version 1): see bolo_map.c mapRead. Pills are
x, y, owner, armour, speed; bases x, y, owner, armour, shells, mines; starts
x, y, dir (0 = east, 4 = north, counted anticlockwise in sixteenths).
Terrain is a list of runs, each one row segment of squares that are not
deep sea, nibble-packed. A row may hold several runs, which is how deep sea
in the middle of a row is written: it is simply the gap between two runs.
"""

import math
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP_OUT = os.path.join(ROOT, "data", "maps", "Tutorial.map")
SCRIPT = os.path.join(ROOT, "data", "maps", "Tutorial.scenario.lua")

# Terrain nibbles (src/bolo/public/global.h). Deep sea is None here: it is
# every square no run covers.
BUILDING, RIVER, SWAMP, CRATER, ROAD, FOREST, RUBBLE, GRASS = range(8)
HALFBUILDING, BOAT = 8, 9
SEA = None

NEUTRAL = 255
PILL_FULL = 15     # pill_max_armour
PILL_SPEED = 100   # pill_attack_ticks: the calm rate, about 2 s a shot
BASE_FULL = 90     # base armour, shells and mines when full

# The frame: a wall round the whole map. Its corners fix the bounding box at
# x 96..156 (centre 126) and y 24..228 (centre 126).
X0, X1 = 96, 156
Y0, Y1 = 24, 228
TRENCH = (97, 100)          # deep-water trench, west side
LAND_X0, LAND_X1 = 101, 155

# Station rows, north edge first in each pair. Station 1 is at the south.
STATIONS = {
    1: (205, 227),
    2: (182, 201),
    3: (161, 178),
    4: (130, 157),
    5: (95, 126),
    6: (72, 91),
    7: (25, 71),
}
# Forest bands between stations (rows), each with a road gap in the middle.
BANDS = [(202, 204), (179, 181), (158, 160), (127, 129), (92, 94)]
GAP = (126, 128)            # the road gap and the main path's columns

grid = [[SEA] * 256 for _ in range(256)]
pills = []     # (name, x, y, owner, armour, speed)
bases = []     # (name, x, y, owner, armour, shells, mines)
starts = []    # (name, x, y, dir)
points = {}    # name -> (x, y)
regions = {}   # name -> (x, y, w, h)


def fill(x0, y0, x1, y1, t):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            grid[y][x] = t


def put(x, y, t):
    grid[y][x] = t


def region(name, x0, y0, x1, y1):
    regions[name] = (x0, y0, x1 - x0 + 1, y1 - y0 + 1)


def pill(name, x, y, owner=NEUTRAL, armour=PILL_FULL):
    pills.append((name, x, y, owner, armour, PILL_SPEED))


def base(name, x, y, owner=NEUTRAL, stock=BASE_FULL):
    bases.append((name, x, y, owner, BASE_FULL, stock, stock))


def start(name, x, y, d):
    assert grid[y][x] is SEA, "start %s is not on deep sea" % name
    starts.append((name, x, y, d))


def plain_pad(name, x, y):
    """A plain reset pad: a 2x2 road square in a ring of rubble. The region
    is the road square."""
    fill(x - 1, y - 1, x + 2, y + 2, RUBBLE)
    fill(x, y, x + 1, y + 1, ROAD)
    region(name, x, y, x + 1, y + 1)


# 3x5 letters for the walled RESET pads.
FONT = {
    "R": ["##.", "#.#", "##.", "#.#", "#.#"],
    "E": ["###", "#..", "##.", "#..", "###"],
    "S": [".##", "#..", ".#.", "..#", "##."],
    "T": ["###", ".#.", ".#.", ".#.", ".#."],
}


def word_walls(text, x, y):
    """The squares of `text` in walls, top-left at (x, y), one blank column
    between letters. Returns the list of wall squares."""
    out = []
    for i, ch in enumerate(text):
        for dy, row in enumerate(FONT[ch]):
            for dx, c in enumerate(row):
                if c == "#":
                    out.append((x + i * 4 + dx, y + dy))
    return out


def walled_reset(name, x, y):
    """RESET spelled in walls with its top-left at (x, y), on a road strip,
    and a 3x2 road pad centred under it. Returns the wall squares, which the
    script rebuilds on each reset."""
    fill(x - 1, y - 1, x + 19, y + 5, ROAD)
    walls = word_walls("RESET", x, y)
    for (wx, wy) in walls:
        put(wx, wy, BUILDING)
    px = x + 8
    fill(px - 1, y + 6, px + 3, y + 9, RUBBLE)
    fill(px, y + 7, px + 2, y + 8, ROAD)
    region(name, px, y + 7, px + 2, y + 8)
    return walls


# --------------------------------------------------------------------------
# Frame, trench, bands
# --------------------------------------------------------------------------
fill(X0, Y0, X1, Y1, GRASS)
fill(X0, Y0, X1, Y0, BUILDING)
fill(X0, Y1, X1, Y1, BUILDING)
fill(X0, Y0, X0, Y1, BUILDING)
fill(X1, Y0, X1, Y1, BUILDING)
fill(TRENCH[0], Y0 + 1, TRENCH[1], Y1 - 1, SEA)
for (a, b) in BANDS:
    fill(TRENCH[0], a, LAND_X1, b, FOREST)
    fill(GAP[0], a, GAP[1], b, ROAD)

for n, (a, b) in STATIONS.items():
    region("s%d" % n, TRENCH[0], a, LAND_X1, b)

# --------------------------------------------------------------------------
# Station 1: terrain. Grass, a road up the middle, a patch of each other
# terrain beside it. The trench to the west is the deep water.
# --------------------------------------------------------------------------
a, b = STATIONS[1]
fill(GAP[0], a, GAP[1], b, ROAD)
fill(LAND_X0, 222, GAP[0] - 1, 224, ROAD)        # from the trench to the path
fill(106, 207, 114, 211, FOREST)
fill(106, 214, 114, 218, SWAMP)
fill(140, 207, 148, 211, RUBBLE)
fill(140, 214, 148, 218, CRATER)
fill(140, 221, 148, 225, RIVER)
start("cp1", 98, 223, 0)
plain_pad("pad1", 117, 209)
points["s1_shore"] = (102, 223)

# --------------------------------------------------------------------------
# Station 2: bases. 2A your base, 2B a neutral base, 2C an enemy base.
# --------------------------------------------------------------------------
a, b = STATIONS[2]
fill(GAP[0], a, GAP[1], b, ROAD)
fill(LAND_X0, 198, GAP[0] - 1, 199, ROAD)
fill(118, 194, 124, 197, ROAD)
base("b2a", 121, 195)                     # given to the player at the start
fill(134, 188, 140, 191, ROAD)
base("b2b", 137, 189)                     # neutral
fill(116, 184, 124, 187, ROAD)
base("b2c", 120, 185)                     # given to the enemy at the start
region("b2a_area", 118, 194, 124, 197)
region("b2b_area", 134, 188, 140, 191)
region("b2c_area", 116, 184, 124, 187)
start("cp2", 98, 199, 0)
plain_pad("pad2", 146, 197)

# --------------------------------------------------------------------------
# Station 3: building. A grove to harvest, open grass to build on, and a
# target wall to shoot down.
# --------------------------------------------------------------------------
a, b = STATIONS[3]
fill(GAP[0], a, GAP[1], b, ROAD)
fill(LAND_X0, 175, GAP[0] - 1, 176, ROAD)
fill(104, 163, 118, 172, FOREST)
GROVE3 = (104, 163, 118, 172)
region("grove3", *GROVE3)
fill(136, 166, 146, 172, GRASS)
WALL3 = (148, 168)
put(WALL3[0], WALL3[1], BUILDING)
fill(147, 170, 149, 170, ROAD)            # a step to stand on, below it
points["wall3"] = WALL3
start("cp3", 98, 176, 0)
plain_pad("pad3", 146, 175)

# --------------------------------------------------------------------------
# Station 4: pillboxes. 4A a dead neutral pill; 4B/4C one live neutral pill
# with forest over the west half of its range circle and grass over the
# east half, and a friendly base just outside the circle.
# --------------------------------------------------------------------------
a, b = STATIONS[4]
fill(GAP[0], a, GAP[1], b, ROAD)
fill(LAND_X0, 154, GAP[0] - 1, 155, ROAD)
pill("p4a", 112, 148, NEUTRAL, 0)          # dead
fill(110, 146, 114, 150, ROAD)
region("p4a_area", 108, 144, 116, 152)
P4 = (141, 141)
pill("p4", P4[0], P4[1])
RANGE = 8
for y in range(P4[1] - RANGE, P4[1] + RANGE + 1):
    for x in range(P4[0] - RANGE, P4[0] + RANGE + 1):
        d = math.hypot(x - P4[0], y - P4[1])
        if d <= RANGE + 0.5 and x <= P4[0] - 3:
            grid[y][x] = FOREST
region("p4_forest", P4[0] - RANGE, P4[1] - RANGE, P4[0] - 3, P4[1] + RANGE)
region("p4_open", P4[0] + 1, P4[1] - RANGE, P4[0] + RANGE, P4[1] + RANGE)
points["p4"] = P4
fill(147, 146, 151, 150, ROAD)
base("b4c", 149, 148)                      # given to the player at the start
start("cp4", 98, 155, 0)
plain_pad("pad4", 116, 136)

# --------------------------------------------------------------------------
# Station 5: the pill take. 5A (east) is an island in a moat where a demo bot
# takes a pill; the player watches from the west shore, out of range of
# both pills. 5B (west) is the same layout for the player.
#
# The layout, both times: target pill T; a friendly pill one square east
# and four south of it (in T's line of fire at a tank parked on the patch);
# a 3x3 road patch six to eight squares south of T, inside T's range.
# --------------------------------------------------------------------------
a, b = STATIONS[5]
fill(GAP[0], a, GAP[1], b, ROAD)
fill(LAND_X0, 123, GAP[0] - 1, 124, ROAD)


def take_layout(prefix, tx, ty, target_owner, friend_owner):
    pill(prefix + "_target", tx, ty, target_owner)
    pill(prefix + "_friend", tx + 1, ty + 4, friend_owner)
    fill(tx - 1, ty + 6, tx + 1, ty + 8, ROAD)
    region(prefix + "_patch", tx - 1, ty + 6, tx + 1, ty + 8)
    points[prefix + "_target"] = (tx, ty)
    points[prefix + "_friend"] = (tx + 1, ty + 4)


# 5A island: moat x 135..155, y 96..120; island x 140..153, y 99..117.
fill(135, 96, LAND_X1, 120, SEA)
fill(140, 99, 153, 117, GRASS)
take_layout("t5a", 147, 101, NEUTRAL, NEUTRAL)
points["t5a_home"] = (147, 114)
fill(146, 113, 148, 115, ROAD)
put(152, 116, SEA)                          # the demo bot's start pocket
start("bot5a", 152, 116, 4)
region("watch5a", 129, 98, 134, 118)
region("island5a", 140, 99, 153, 117)

# 5B: same layout to the west, and a walled RESET outside T's range.
take_layout("t5b", 112, 100, NEUTRAL, NEUTRAL)
walls5b = walled_reset("reset5b", 103, 112)
start("cp5", 98, 123, 0)
region("s5b", TRENCH[0], 95, 125, 126)

# --------------------------------------------------------------------------
# Station 6: kill the man. A bot's tank is parked in the east; its builder
# harvests the grove on a loop. A gate in the north-west carries the player
# across the moat to Station 7.
# --------------------------------------------------------------------------
a, b = STATIONS[6]
fill(GAP[0], a, GAP[1], b, ROAD)
fill(LAND_X0, 88, GAP[0] - 1, 89, ROAD)
fill(134, 76, 146, 87, FOREST)
region("grove6", 134, 76, 146, 87)
fill(149, 79, 153, 83, ROAD)
points["bot6_park"] = (151, 81)
put(154, 81, SEA)
start("bot6", 154, 81, 8)
fill(104, 73, 110, 76, ROAD)
region("gate6", 105, 73, 109, 75)
start("cp6", 98, 89, 0)
plain_pad("pad6", 116, 84)

# --------------------------------------------------------------------------
# Station 7: the final round, a small Chew Toy on an island in a moat. The
# player holds three quarters (SW, NW, SE); one bot holds NE.
# --------------------------------------------------------------------------
a, b = STATIONS[7]
fill(TRENCH[0], a, LAND_X1, b, SEA)
IX0, IY0, IX1, IY1 = 107, 27, 149, 67
CX, CY = 128, 47
fill(IX0, IY0, IX1, IY1, FOREST)
fill(IX0 + 3, IY0 + 3, IX1 - 3, IY1 - 3, ROAD)
# Round the corners off, the way Chew Toy's forest disc is round.
for y in range(IY0, IY1 + 1):
    for x in range(IX0, IX1 + 1):
        dx = max(0, abs(x - CX) - 14)
        dy = max(0, abs(y - CY) - 13)
        if dx * dx + dy * dy > 49:
            grid[y][x] = SEA
# The grass cross through the middle, and walls on the axes in the forest.
fill(CX, IY0 + 3, CX, IY1 - 3, GRASS)
fill(IX0 + 3, CY, IX1 - 3, CY, GRASS)
for x in list(range(IX0, IX0 + 3)) + list(range(IX1 - 2, IX1 + 1)):
    if grid[CY][x] is not SEA:
        grid[CY][x] = BUILDING
for y in list(range(IY0, IY0 + 3)):
    if grid[y][CX] is not SEA:
        grid[y][CX] = BUILDING
# Short diagonal wall bits in each quarter, as Chew Toy has.
for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
    for k in range(3):
        put(CX + sx * (5 + k), CY + sy * (3 + k), BUILDING)
# Grass edging inside the forest, as Chew Toy has.
for y in range(IY0 + 3, IY1 - 2):
    for x in range(IX0 + 3, IX1 - 2):
        if grid[y][x] == ROAD:
            for ddx, ddy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if grid[y + ddy][x + ddx] == FOREST:
                    grid[y][x] = GRASS
                    break

# Bot quarter (NE).
fill(141, 31, 143, 33, FOREST)
put(142, 32, SEA)
start("bot7", 142, 32, 10)
pill("p7_ne1", CX + 6, CY - 8)
pill("p7_ne2", CX + 8, CY - 6)
base("b7_ne1", CX + 4, CY - 12)
base("b7_ne2", CX + 12, CY - 4)
# Player quarters: NW, SE live pills; SW two dead pills by the start.
pill("p7_nw1", CX - 6, CY - 8)
pill("p7_nw2", CX - 8, CY - 6)
base("b7_nw1", CX - 4, CY - 12)
base("b7_nw2", CX - 12, CY - 4)
pill("p7_se1", CX + 6, CY + 8)
pill("p7_se2", CX + 8, CY + 6)
base("b7_se1", CX + 4, CY + 12)
base("b7_se2", CX + 12, CY + 4)
# SW: the start pocket faces east, two dead pills just in front of it, two
# bases, and the walled RESET along the south.
fill(111, 51, 113, 54, FOREST)
fill(112, 52, 112, 53, SEA)
start("cp7", 112, 52, 0)
pill("p7_sw1", 116, 51, NEUTRAL, 0)
pill("p7_sw2", 116, 54, NEUTRAL, 0)
base("b7_sw1", CX - 4, CY + 8)
base("b7_sw2", CX - 10, CY + 4)
walls7 = walled_reset("reset7", 109, 56)
points["s7_arrive"] = (114, 52)
region("quarter_ne", CX + 1, IY0, IX1, CY - 1)
region("island7", IX0, IY0, IX1, IY1)

# The dams between the trench pools sit in the bands. Station 6's pool is cut
# off from Station 7's moat by one more dam on Station 6's top row, so a tank
# that respawns at Station 6 cannot sail round to the island.
fill(TRENCH[0], 72, TRENCH[1], 72, FOREST)

# --------------------------------------------------------------------------
# Checks and output
# --------------------------------------------------------------------------
def bbox():
    xs = [x for y in range(256) for x in range(256) if grid[y][x] is not SEA]
    ys = [y for y in range(256) for x in range(256) if grid[y][x] is not SEA]
    return min(xs), max(xs), min(ys), max(ys)


L, R, T, B = bbox()
assert (L + R) // 2 == 126 and (T + B) // 2 == 126, (L, R, T, B)
assert len(pills) <= 16 and len(bases) <= 16 and len(starts) <= 16
for (_, x, y, *_r) in pills + bases:
    assert L <= x <= R and T <= y <= B
for (n, x, y, _d) in starts:
    assert grid[y][x] is SEA, n
    assert L <= x <= R and T <= y <= B
for (n, x, y, *_r) in pills:
    assert grid[y][x] is not SEA, n
    # mapRead puts road under a map-file pill; say so here as well.
    grid[y][x] = ROAD


def encode_segment(cells):
    """Nibbles for one run of terrain codes (no deep sea)."""
    out = []
    i = 0
    n = len(cells)
    while i < n:
        j = i
        while j < n and cells[j] == cells[i] and j - i < 9:
            j += 1
        if j - i >= 2:
            out += [(j - i) + 6, cells[i]]
            i = j
            continue
        # A literal group: up to 8 squares, stopping before a repeat of 3.
        k = i
        lit = []
        while k < n and len(lit) < 8:
            if k + 2 < n and cells[k] == cells[k + 1] == cells[k + 2]:
                break
            lit.append(cells[k])
            k += 1
        if not lit:
            lit = [cells[i]]
            k = i + 1
        out += [len(lit) - 1] + lit
        i = k
    return out


def encode_runs():
    data = bytearray()
    for y in range(256):
        x = 0
        while x < 256:
            if grid[y][x] is SEA:
                x += 1
                continue
            sx = x
            while x < 256 and grid[y][x] is not SEA:
                x += 1
            # A run's length byte holds the whole record, so keep a segment
            # short enough; 120 squares is far below the 255-byte limit.
            seg_x = sx
            while seg_x < x:
                ex = min(x, seg_x + 120)
                nib = encode_segment(grid[y][seg_x:ex])
                if len(nib) % 2:
                    nib.append(0)
                body = bytes((nib[i] << 4) | nib[i + 1]
                             for i in range(0, len(nib), 2))
                assert len(body) + 4 <= 255
                data += bytes((len(body) + 4, y, seg_x, ex)) + body
                seg_x = ex
    data += bytes((4, 0xFF, 0xFF, 0xFF))
    return bytes(data)


def write_map():
    out = bytearray(b"BMAPBOLO")
    out += bytes((1, len(pills), len(bases), len(starts)))
    for (_, x, y, o, a, s) in pills:
        out += struct.pack("BBBBB", x, y, o, a, s)
    for (_, x, y, o, a, s, m) in bases:
        out += struct.pack("BBBBBB", x, y, o, a, s, m)
    for (_, x, y, d) in starts:
        out += struct.pack("BBB", x, y, d)
    out += encode_runs()
    with open(MAP_OUT, "wb") as f:
        f.write(out)


def lua_layout():
    lines = ["-- BEGIN LAYOUT (written by tools/make_tutorial_map.py; do not edit)",
             "local LAYOUT = {"]
    lines.append("  pill = {")
    for i, (n, x, y, o, a, s) in enumerate(pills):
        lines.append("    %s = { n = %d, x = %d, y = %d, armour = %d }," % (n, i + 1, x, y, a))
    lines.append("  },")
    lines.append("  base = {")
    for i, (n, x, y, *_r) in enumerate(bases):
        lines.append("    %s = { n = %d, x = %d, y = %d }," % (n, i + 1, x, y))
    lines.append("  },")
    lines.append("  start = {")
    for i, (n, x, y, d) in enumerate(starts):
        lines.append("    %s = { n = %d, x = %d, y = %d }," % (n, i + 1, x, y))
    lines.append("  },")
    lines.append("  point = {")
    for n in sorted(points):
        lines.append("    %s = { %d, %d }," % (n, points[n][0], points[n][1]))
    lines.append("  },")
    lines.append("  region = {")
    for n in sorted(regions):
        lines.append("    %s = { x = %d, y = %d, w = %d, h = %d }," % ((n,) + regions[n]))
    lines.append("  },")
    for nm, ws in (("walls5b", walls5b), ("walls7", walls7)):
        lines.append("  %s = { %s }," % (nm, ", ".join("{%d,%d}" % w for w in ws)))
    gx0, gy0, gx1, gy1 = GROVE3
    lines.append("  grove3 = { %d, %d, %d, %d }," % (gx0, gy0, gx1, gy1))
    lines.append("}")
    lines.append("-- END LAYOUT")
    return "\n".join(lines)


def update_script():
    if not os.path.exists(SCRIPT):
        print("no script yet; layout block:\n" + lua_layout())
        return
    with open(SCRIPT, encoding="utf-8", newline="") as f:
        s = f.read()
    pat = re.compile(r"-- BEGIN LAYOUT.*?-- END LAYOUT", re.S)
    if not pat.search(s):
        sys.exit("the script has no LAYOUT block to rewrite")
    s2 = pat.sub(lambda _m: lua_layout(), s, count=1)
    if s2 != s:
        with open(SCRIPT, "w", encoding="utf-8", newline="") as f:
            f.write(s2)
        print("layout block rewritten in", SCRIPT)


if __name__ == "__main__":
    write_map()
    print("wrote", MAP_OUT, "bbox", bbox(), "pills", len(pills),
          "bases", len(bases), "starts", len(starts))
    update_script()
