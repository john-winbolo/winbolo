#!/usr/bin/env python3
"""
Build data/maps/Tutorial.map, the map the Tutorial scenario plays on, and
rewrite the layout block in data/maps/Tutorial.scenario.lua so the script
and the map always name the same squares.

    C:\\Python310\\python.exe tools/make_tutorial_map.py

The map is seven stations stacked up a column, Station 1 at the south
(bottom) and Station 7 at the north (top), joined by one road up the
middle. The main road stays clear the whole way up. Each station's
checkpoint start is the centre of a 3x3 pool of deep water (pool() below),
walled on the west, east and south, facing north. One road square leaves
the middle of the pool's north edge; the tank that respawns there on its
boat drives out north and the road merges into the main road. Station 1's
pool sits at the main road's south end, in line with it; Stations 2 to 6
have theirs just west of the main road at the station's south edge
(dock()). No boat can reach another station. Station 7 is an island; its
pool opens onto the island's road.

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
LAND_X0, LAND_X1 = 97, 155

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


arrows = {}  # RESET pen name -> the road squares of its arrow


def region(name, x0, y0, x1, y1):
    regions[name] = (x0, y0, x1 - x0 + 1, y1 - y0 + 1)


def pill(name, x, y, owner=NEUTRAL, armour=PILL_FULL):
    pills.append((name, x, y, owner, armour, PILL_SPEED))


def base(name, x, y, owner=NEUTRAL, stock=BASE_FULL):
    bases.append((name, x, y, owner, BASE_FULL, stock, stock))


def start(name, x, y, d):
    assert grid[y][x] is SEA, "start %s is not on deep sea" % name
    starts.append((name, x, y, d))


def pool(name, px, py):
    """A checkpoint start: a 3x3 pool of deep water centred on (px, py), the
    start square. Walls close it on the west, east and south, one square out.
    On the north the pool's middle column opens onto one road square at
    (px, py - 2), with a wall on each side of it, so that road square is the
    only way in or out. The start faces north, out along that road.

    No drowning: the tank respawns on a boat. It leaves the boat on the pool
    square it drives off from, and the one way out is through (px, py - 1),
    so the boat always sits on the square a tank drives back in on. A tank
    that comes back boards it and does not drown."""
    fill(px - 2, py - 1, px + 2, py + 2, BUILDING)
    fill(px - 1, py - 1, px + 1, py + 1, SEA)
    put(px - 1, py - 2, BUILDING)
    put(px + 1, py - 2, BUILDING)
    put(px, py - 2, ROAD)
    start(name, px, py, 4)


def dock(name, south):
    """A station's checkpoint, west of the main road at the station's south
    edge: a pool() whose centre is three squares west of the road's west
    column, its south wall on the station's south row `south`. The exit road
    runs north two squares from the pool, then east along that row into the
    main road. Footprint: x GAP[0]-5 .. GAP[0]-1, rows south-5 .. south."""
    g0 = GAP[0]
    px, py = g0 - 3, south - 2
    pool(name, px, py)
    fill(px, py - 3, g0 - 1, py - 3, ROAD)      # north, then east to the road


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


def walled_reset(name, x, y, road_x, arrow_dx=5):
    """A RESET pad: the word RESET in walls with its top-left at (x, y), on a
    road strip so it reads; below it a small walled pen with a 3x2 road pad
    inside and a two-square gap in its east wall; a road from the gap east
    to the main road at column `road_x`; and an arrow of road squares under that
    road pointing along it at the gap. The region is the pad. Returns every
    wall square (letters and pen), which the script rebuilds on each
    reset; the arrow's squares go to the layout as arrow_<name>."""
    fill(x - 1, y - 1, x + 19, y + 5, ROAD)
    walls = word_walls("RESET", x, y)
    px = x + 8
    pen = []
    for wy in range(y + 6, y + 10):
        for wx in range(px - 1, px + 4):
            edge = wy in (y + 6, y + 9) or wx in (px - 1, px + 3)
            gap = wx == px + 3 and wy in (y + 7, y + 8)
            if edge and not gap:
                pen.append((wx, wy))
    fill(px, y + 7, road_x - 1, y + 8, ROAD)     # pad, gap and road east
    region(name, px, y + 7, px + 2, y + 8)
    walls = walls + pen
    for (wx, wy) in walls:
        put(wx, wy, BUILDING)
    ax = px + arrow_dx
    arrow = [(ax, y + 11), (ax + 1, y + 10), (ax + 1, y + 12),
             (ax + 2, y + 11), (ax + 3, y + 11)]
    for (wx, wy) in arrow:
        put(wx, wy, ROAD)
    arrows[name] = arrow
    return walls


# --------------------------------------------------------------------------
# Frame, bands
# --------------------------------------------------------------------------
fill(X0, Y0, X1, Y1, GRASS)
fill(X0, Y0, X1, Y0, BUILDING)
fill(X0, Y1, X1, Y1, BUILDING)
fill(X0, Y0, X0, Y1, BUILDING)
fill(X1, Y0, X1, Y1, BUILDING)
for (a, b) in BANDS:
    fill(LAND_X0, a, LAND_X1, b, FOREST)
    fill(GAP[0], a, GAP[1], b, ROAD)

for n, (a, b) in STATIONS.items():
    region("s%d" % n, LAND_X0, a, LAND_X1, b)

# --------------------------------------------------------------------------
# Station 1: terrain. Grass, a road up the middle, and a 3x3 patch of each
# other terrain one square beside it. The checkpoint pool is the deep water,
# at the south end of the road.
# --------------------------------------------------------------------------
a, b = STATIONS[1]
fill(GAP[0], a, GAP[1], b, ROAD)
W1, E1 = (GAP[0] - 4, GAP[0] - 2), (GAP[1] + 2, GAP[1] + 4)
fill(W1[0], 216, W1[1], 218, FOREST)
fill(E1[0], 216, E1[1], 218, SWAMP)
fill(W1[0], 211, W1[1], 213, RUBBLE)
fill(E1[0], 211, E1[1], 213, CRATER)
fill(W1[0], 206, W1[1], 208, RIVER)
# The checkpoint pool at the main road's south end, in line with it: the
# pool's walls take rows 224..228 (228 is the frame), so the main road stops
# at row 223 and the pool's one road square (MID, 224) runs straight into it.
pool("cp1", (GAP[0] + GAP[1]) // 2, b - 1)

# --------------------------------------------------------------------------
# Station 2: bases. 2A your base, 2B a neutral base, 2C an enemy base.
# --------------------------------------------------------------------------
a, b = STATIONS[2]
fill(GAP[0], a, GAP[1], b, ROAD)
# Each base on a road patch that touches the main road, in the order the
# station asks for them going north: 2A west, 2B east, 2C west.
fill(121, 193, 125, 195, ROAD)
base("b2a", 123, 194)                     # given to the player at the start
fill(129, 188, 133, 190, ROAD)
base("b2b", 131, 189)                     # neutral
fill(121, 184, 125, 186, ROAD)
base("b2c", 123, 185)                     # given to the enemy at the start
region("b2a_area", 121, 193, 125, 195)
region("b2b_area", 129, 188, 133, 190)
region("b2c_area", 121, 184, 125, 186)
dock("cp2", b)

# --------------------------------------------------------------------------
# Station 3: building. A grove to harvest, open grass to build on, and a
# target wall to shoot down.
# --------------------------------------------------------------------------
a, b = STATIONS[3]
fill(GAP[0], a, GAP[1], b, ROAD)
# The grove one square west of the road; the target wall four squares east
# of it, with open grass round it to build on.
GROVE3 = (118, 164, 124, 172)
fill(*GROVE3, FOREST)
region("grove3", *GROVE3)
WALL3 = (133, 168)
put(WALL3[0], WALL3[1], BUILDING)
fill(132, 170, 134, 170, ROAD)            # a step to stand on, below it
points["wall3"] = WALL3
# A friendly base on the road's centre column at the north end, to refill
# shells after the wall.
base("b3", (GAP[0] + GAP[1]) // 2, a + 2)  # given to the player at the start
dock("cp3", b)

# --------------------------------------------------------------------------
# Station 4: pillboxes. 4A a dead neutral pill; 4B/4C one live neutral pill
# in a full ring of forest, with a clearing round the pill itself; and a
# friendly base on the main road at the station's north end.
# --------------------------------------------------------------------------
a, b = STATIONS[4]
fill(GAP[0], a, GAP[1], b, ROAD)
# The dead pill on a road patch touching the main road, west.
pill("p4a", 122, 149, NEUTRAL, 0)          # dead
fill(120, 147, 125, 151, ROAD)
region("p4a_area", 119, 146, 125, 152)
# The live pill east. Forest covers every square within RANGE + 1.5 of it
# except a 5x5 clearing round the pill, so every way in is through forest:
# a tank wholly in the ring and 3 or more squares out (the classic
# tree_hide_distance) is hidden; in the clearing it is seen. The ring's west
# edge touches the road's east edge, and the road (x <= GAP[1]) is ten
# squares from the pill, out of its range.
P4 = (138, 141)
pill("p4", P4[0], P4[1])
RANGE = 8
for y in range(P4[1] - RANGE - 2, P4[1] + RANGE + 3):
    for x in range(P4[0] - RANGE - 2, P4[0] + RANGE + 3):
        d = math.hypot(x - P4[0], y - P4[1])
        near = max(abs(x - P4[0]), abs(y - P4[1]))
        if d <= RANGE + 1.5 and near >= 3 and x > GAP[1]:
            grid[y][x] = FOREST
region("p4_clearing", P4[0] - 2, P4[1] - 2, P4[0] + 2, P4[1] + 2)
points["p4"] = P4
# The base on the road's centre column at the north end, so the tank drives
# over it on the way on; 14 squares from the pill, out of its range.
base("b4c", (GAP[0] + GAP[1]) // 2, a + 2)   # given to the player at the start
dock("cp4", b)

# --------------------------------------------------------------------------
# Station 5: the pill take. 5A (east) is a walled island beside the road
# where a demo bot takes a pill; the player watches from a short road spur,
# out of range of it. 5B (west) is the same target for the player.
#
# The take, both times: the target pill T, and a blocker pill built on a
# square orthogonally next to it. The blocker soaks up T's shells while the
# tank shoots T.
# --------------------------------------------------------------------------
a, b = STATIONS[5]
fill(GAP[0], a, GAP[1], b, ROAD)


def take_layout(prefix, tx, ty, park=None):
    """T at (tx, ty), its blocker on the square south of it. `park`, when
    given, is the (dx, dy) of the tank's parking square from T."""
    pill(prefix + "_target", tx, ty, NEUTRAL)
    pill(prefix + "_blocker", tx, ty + 1, NEUTRAL, 0)
    points[prefix + "_target"] = (tx, ty)
    points[prefix + "_blocker"] = (tx, ty + 1)
    if park is not None:
        px, py = tx + park[0], ty + park[1]
        put(px, py, ROAD)
        region(prefix + "_park", px, py, px, py)
        points[prefix + "_park"] = (px, py)


# 5A: an island of grass, x 135..140, y 104..117, closed in by a wall ring,
# a one-square deep-water moat, and a second wall ring outside the moat:
#
#   x 132 and 143, y 101 and 120: the outer walls
#   x 133 and 142, y 102 and 119: the moat
#   x 134 and 141, y 103 and 118: the inner walls
#
# The moat is what keeps the demo bot on its island. With walls alone the
# bot's route finder counts a wall as a few shots' work, so every pillbox
# and square off the island was in reach: the bot shot its way out after
# the dead pills in Stations 4 and 7 and to drop its blocker in 5B, and
# never took T. Deep water without a boat is no route at all, so with the
# moat nothing off the island is a goal. The inner walls keep the bot off
# the moat's edge, so it cannot drown; the outer walls keep the player's
# tank off it.
#
# The player watches from the end of a short road spur off the main road,
# the green square at WATCH5A, beside the outer west wall. T is six squares
# east and six north of it (8.5 squares), out of T's range of 8; the main
# road is 9 or more squares from T. From the spur the take is on screen.
#
# The demo bot carries its blocker and builds it itself each time round, on
# a square orthogonally next to the target (GoalHunter's
# BLOCKER_ORTHOGONAL_ONLY, turned on for this bot only); the map puts the
# pill on the square south of T, dead, for the script to hand it. See
# docs/TUTORIAL_DECISIONS.md, "The blocker take".
#
# A start must be on deep water, so the bot's start is a one-square pocket
# in the island's south-east corner, walled on the south, east and west and
# open to the north, 12 squares from T and well away from where the bot
# fights. It does not touch the moat.
WATCH5A = (131, 111)
fill(GAP[1] + 1, WATCH5A[1], WATCH5A[0], WATCH5A[1], ROAD)
points["watch5a"] = WATCH5A
region("watch5a", GAP[1] + 1, WATCH5A[1], WATCH5A[0], WATCH5A[1])
fill(132, 101, 143, 120, BUILDING)
fill(133, 102, 142, 119, SEA)
fill(134, 103, 141, 118, BUILDING)
fill(135, 104, 140, 117, GRASS)
take_layout("t5a", 137, 105)
points["t5a_home"] = (136, 115)
fill(135, 114, 137, 116, ROAD)
put(139, 117, BUILDING)                     # the pocket's west wall
put(140, 117, SEA)                          # the demo bot's start pocket
start("bot5a", 140, 117, 4)
region("island5a", 135, 104, 140, 117)

# 5B: the target nine squares from the road, so a tank driving up the road
# is out of its range. The player is handed its blocker pill in the tank and
# builds it on the square south of the target. The parking square is a road
# square seven squares straight south of the target, the farthest a tank
# shell still reaches it (7.125 squares from the tank's centre; the target's
# near edge is at 6.5). A road spur along that row joins it to the main road.
# The target's shots at the parked tank fly straight down the column and hit
# the blocker. The tank's shots fly the same column the other way; the
# script lets them pass the player's own blocker (can_hit), which a real game
# would not. See docs/TUTORIAL_DECISIONS.md, "5B parking square".
take_layout("t5b", 117, 101, park=(0, 7))
fill(118, 108, GAP[0] - 1, 108, ROAD)
# The arrow sits two squares further west than walled_reset's default, at
# x 117..120, so it clears the checkpoint pool's west wall at x 121.
walls5b = walled_reset("reset5b", 106, 112, GAP[0], arrow_dx=3)
# A friendly base on the road's centre column at the north end.
base("b5", (GAP[0] + GAP[1]) // 2, a + 2)  # given to the player at the start
dock("cp5", b)
# x up to 122 and rows 95..122 only: the checkpoint pool (122..124 x
# 123..125) and its exit road (x 123..125, rows 121..122) lie outside, so a
# respawn there does not count as driving into 5B.
region("s5b", LAND_X0, 95, 122, 122)

# --------------------------------------------------------------------------
# Station 6: kill the man. A bot's tank is parked just east of the road; its
# man walks west along one row to a single tree west of the road, crossing
# the road and a short span of craters, and back, over and over. A friendly
# base on the road a few squares south of the craters.
# --------------------------------------------------------------------------
a, b = STATIONS[6]
fill(GAP[0], a, GAP[1], b, ROAD)
ROW6 = 80
PARK6 = (GAP[1] + 3, ROW6)                  # 131: the parked tank
TREE6 = (GAP[0] - 11, ROW6)                 # 115: the one tree
# 121..124: the aiming spot, 2 to 5 squares west of the road's west edge, so
# a crosshair at its longest range (7) from anywhere on the road reaches it.
# The man walks the row both ways, so he crosses the same craters going out
# to the tree and coming back.
CRATERS6 = (GAP[0] - 5, GAP[0] - 2)
fill(TREE6[0] + 1, ROW6, PARK6[0], ROW6, ROAD)
fill(CRATERS6[0], ROW6, CRATERS6[1], ROW6, CRATER)
put(TREE6[0], TREE6[1], FOREST)
points["bot6_park"] = PARK6
points["tree6"] = TREE6
region("craters6", CRATERS6[0], ROW6, CRATERS6[1], ROW6)
region("path6", TREE6[0] + 1, ROW6, PARK6[0], ROW6)
put(PARK6[0] + 3, ROW6, SEA)                # the bot's start pocket
start("bot6", PARK6[0] + 3, ROW6, 8)
base("b6", (GAP[0] + GAP[1]) // 2, ROW6 + 4)  # given to the player
dock("cp6", b)

# --------------------------------------------------------------------------
# Station 7: the final round, a small Chew Toy island. The main road runs
# straight in from the south. A wall round the island keeps the bot and its
# man in; the road is the one gap in it, and the script sends the bot back
# if it drives out through it. The player holds three quarters (SW, NW,
# SE), each with two pills and two bases; the bot holds NE, two bases and
# no pills.
# --------------------------------------------------------------------------
a, b = STATIONS[7]
fill(LAND_X0, a, LAND_X1, b, GRASS)
IX0, IY0, IX1, IY1 = 107, 27, 149, 67
CX, CY = 128, 47
fill(IX0, IY0, IX1, IY1, FOREST)
fill(IX0 + 3, IY0 + 3, IX1 - 3, IY1 - 3, ROAD)
# Round the corners off, the way the Chew Toy forest disc is round.
outside = set()
for y in range(IY0, IY1 + 1):
    for x in range(IX0, IX1 + 1):
        dx = max(0, abs(x - CX) - 14)
        dy = max(0, abs(y - CY) - 13)
        if dx * dx + dy * dy > 49:
            grid[y][x] = GRASS
            outside.add((x, y))
# The grass cross through the middle, and walls on the axes in the forest.
fill(CX, IY0 + 3, CX, IY1 - 3, GRASS)
fill(IX0 + 3, CY, IX1 - 3, CY, GRASS)
for x in list(range(IX0, IX0 + 3)) + list(range(IX1 - 2, IX1 + 1)):
    if (x, CY) not in outside:
        grid[CY][x] = BUILDING
for y in list(range(IY0, IY0 + 3)):
    if (CX, y) not in outside:
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
# The wall: every square outside the island that touches it.
inside = set((x, y) for y in range(IY0, IY1 + 1) for x in range(IX0, IX1 + 1)
             if (x, y) not in outside)
for y in range(IY0 - 1, IY1 + 2):
    for x in range(IX0 - 1, IX1 + 2):
        if (x, y) in inside:
            continue
        if any((x + ddx, y + ddy) in inside
               for ddx in (-1, 0, 1) for ddy in (-1, 0, 1)):
            grid[y][x] = BUILDING
# The road in: from the station's south edge north through the wall and the
# forest rim to the island's road.
fill(GAP[0], IY1 - 3, GAP[1], b, ROAD)
region("island7", IX0, IY0, IX1, IY1)
points["s7_arrive"] = ((GAP[0] + GAP[1]) // 2, IY1 - 4)

# Bot quarter (NE): its start pocket, a pool in a forest clump, and two
# bases. No pills: a pill there would be one more thing the bot defends,
# and the lesson is taking bases with the player's pills.
fill(141, 31, 143, 33, FOREST)
put(142, 32, SEA)
start("bot7", 142, 32, 10)
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
# SW: the checkpoint pool, two bases, and the walled RESET along the south.
# The pool's exit road square (112, 49) opens onto the island's road. The
# two dead pills the player starts the round with lie on the centre of the
# road in, one behind the other, between the island's wall and
# the station's south edge: driving straight in picks up both. They are
# inside Station 7 and outside the island, which the bot never leaves.
pool("cp7", 112, 51)
MID = (GAP[0] + GAP[1]) // 2
pill("p7_sw1", MID, b - 1, NEUTRAL, 0)
pill("p7_sw2", MID, b - 3, NEUTRAL, 0)
base("b7_sw1", CX - 4, CY + 4)
base("b7_sw2", CX - 10, CY + 4)
walls7 = walled_reset("reset7", 108, 55, GAP[0])
region("quarter_ne", CX + 1, IY0, IX1, CY - 1)

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
    for nm in sorted(arrows):
        lines.append("  arrow_%s = { %s }," % (nm, ", ".join("{%d,%d}" % w for w in arrows[nm])))
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
