#!/usr/bin/env python3
"""
Generate the farm-sector / return-leg arenas: maps AND their scenario sidecars
(companion to tests/farm_sectors_test.py).

TWO CHANGES UNDER TEST (builder_pool.lua, 2026-09-06)
----------------------------------------------------

1. BUILDER_POOL_FARM_SECTORS.  Farm discovery used to offer exactly ONE row:
   the nearest forest anywhere in the (2 x BUILDER_POOL_LEASH + 1)^2 square.
   The engine's LGM does not pathfind -- brain_pathfinder.c lgmTravelTicksCore
   replays a STRAIGHT LINE with a crude slide and reports stuck -- so "nearest"
   is regularly a forest the man cannot get to at all, and a clear forest one
   tile further out was never a candidate.  Discovery now splits the square
   into four 90-degree wedges (N/E/S/W, boundaries on the 45-degree diagonals,
   the diagonal itself going to the vertical wedge) and offers the nearest
   forest in EACH.  FARM_SECTORS=1 is the old single row.

2. BUILDER_POOL_RETURN_PREDICT.  M.lgm_trip used to charge `2 x outbound` --
   the walk home mirrored the walk out, which is only true if the tank waits on
   the spot.  It now marches the tank forward along ITS OWN COMMITTED ROUTE
   (state.pf.path_chain, at the engine's per-terrain speed caps) for
   out + LGM_BUILD_TIME brain ticks and walks the man back to the tile it lands
   on, so trip = out + build + back.  RETURN_PREDICT=false is the old number.

THE ARENAS
----------

  A   THE WALL.  A tank with nothing to do, a forest TWO tiles west behind a
      three-tile BUILDING wall, and a clear forest THREE tiles east.  The near
      one is nearest and unreachable; the far one is in a different wedge.
      Default (4 wedges): both are rows, the walled one rejects `unreachable`
      and the man is dispatched EAST.
  A2  THE CONTROL, cfg=BUILDER_POOL_FARM_SECTORS=1.  Now only the NEAREST
      forest in the whole square is a row -- the walled one -- so the pool has
      nothing it can send the man to and NO FARM DISPATCH HAPPENS.  The absence
      is the entire result, and the test says so rather than letting it look
      like an oversight.

      THE TANK WANDERS, AND THE ARENA IS BUILT AROUND THAT rather than
      pretending otherwise.  "The walled forest is the nearest" is a claim
      about the tank's TILE, and a bot with nothing to do still explores --
      measured: pricing EXPLORE_BASE_COST past the unaffordable line does not
      stop it, because explore is the pool's fallback and not a scored
      candidate, and the tank crawled four tiles west in the first 800 ticks.
      So:
        * the arena has NO pills and NO bases, so nothing but explore is ever
          bid and the tank crawls rather than drives;
        * the sidecar plants the whole geometry RELATIVE to wherever the tank
          is when it plants, so the arena is exact at that moment;
        * the wall spans the FULL field height, which pens the tank east of it
          for the rest of the run -- so the walled forest can never stop being
          unreachable;
        * and the sidecar logs the tank's tile on every change, so the test
          computes the WINDOW during which the walled forest really was the
          nearer of the two and makes its claims only inside it.  A2's "no
          dispatch" is asserted over that window and the window's length is
          printed, so a short one cannot pass unnoticed.

      A tank on explore has no committed route either (measured: predsrc{same},
      pred=same/no_route), so the return-leg prediction falls back to the
      tank's own tile on every row here.  That is fine and deliberate: arena A
      is about the wedges and nothing else, and its arithmetic is identical
      under both settings of RETURN_PREDICT.  Arena B is where the route is.

  B   THE DRIVE.  A long straight ROAD with a NEUTRAL BASE twenty tiles east:
      the bot bids capture_base and drives east at road top speed to take it,
      which is what gives it the committed ROUTE the prediction walks.  It has
      to be a base and not a pill: capture_pill puts the builder in "gather"
      mode and the pool then refuses every row with `mode_owned (gather)`
      (measured), while capture_base is "opportunistic" and leaves the pool
      running the whole way there.  When it reaches a chosen column the sidecar plants TWO forests,
      one three tiles BEHIND and one three tiles AHEAD, at the SAME distance
      from the tank and in different wedges (W and E).  Equal outbound legs, so
      the OLD score cannot tell them apart -- only the return leg can.
        with prediction: four tiles of road fit inside the horizon, so the man
          walks ~1.4 tiles home from the AHEAD forest and ~7.1 from the BEHIND
          one.  The ahead row wins; the behind row drops under
          BUILDER_POOL_MIN_SCORE and is refused.  The route itself is printed
          on a BP_PRED line and the test checks the predicted tile is ON it.
        B2 THE CONTROL, cfg=BUILDER_POOL_RETURN_PREDICT=false: back == out on
          both rows, they score IDENTICALLY, and the tie breaks by tile key --
          which hands it to the BEHIND forest (the lower x).  The man is sent
          the wrong way, which is the bug the change is for.

      WHY THE FORESTS ARE PLANTED AND NOT BAKED IN.  "Two forests the same
      distance away" is only true at one instant for a moving tank, and nothing
      in the brain can be asked to score at a chosen instant.  game.set_tile
      from the sidecar puts them both down on the tick the tank enters a named
      column, which makes the instant the arena's to choose.

      WHY THEY SIT ONE ROW SOUTH of the road.  A forest ON the drive row is a
      forest the tank drives over and the on-path harvest chops, which ends the
      comparison; one row off it is out of the way and still symmetric, because
      both forests are the same offset from whatever row the tank is on.

THE ARITHMETIC (BUILDER_POOL_LEASH 8, MIN_SCORE 20, TRIP_W 0.5, LGM_BUILD_TIME
20, road/grass MAN speed 16 wu/tick = 16 ticks a tile, road TANK speed cap 16
wu/tick, VALUE_FARM pushed to 99 by the same cfg= the repair-priority arenas
use -- see below):

  A   open forest 3 tiles: out 48, back 48 (no goal -> no route -> the
      prediction falls back to the tank's own tile and the return leg is the
      outbound leg mirrored, identical under both flags), trip 116, cost 58,
      score 99 - 58 = 41, comfortably over MIN_SCORE.
      walled forest 2 tiles: the walk sim never leaves the tank's tile and
      returns -1 after BUILDER_POOL_LGM_STUCK_TICKS -> `unreachable`.

  B   both forests 3 tiles east/west and one row south: euclid 3.16 tiles,
      out ~51 either way, so the control scores both 99 - 0.5 x (51+20+51) = 38
      and the tie goes to the lower tile key -- the BEHIND forest.
      With prediction, horizon = 51 + 20 = 71 ticks and a road tile costs
      256/16 = 16 ticks, so four tiles of route fit (64 <= 71 < 80):
        ahead   back ~23  -> trip 94  -> score 99 - 47 = 52
        behind  back ~113 -> trip 184 -> score 99 - 92 = 7  (< MIN_SCORE)

WHY THE cfg= TOKENS (the same two traps the repair-priority arenas hit):
  * a scenario sidecar force-switches the map to gameScripted, which hands out
    TANK_FULL_TREES (40) whatever -gametype says, and discovery offers no farm
    row at all at or above TREE_OPPORTUNISTIC_MAX (20).  So the gate is moved
    with cfg=TREE_OPPORTUNISTIC_MAX=41 rather than with an empty woodpile.
  * at 40 trees the farm urgency term is zero and the row is worth a flat
    BUILDER_POOL_VALUE_FARM (15), which cannot clear MIN_SCORE at any distance.
    cfg=BUILDER_POOL_VALUE_FARM=99 stands in for the 5-tree woodpile that
    produces 15 + 12 x (12 - 5) = 99 in a real game -- the same stand-in
    tests/generate_repair_priority_map.py uses, and for the same reason.

MAP FACTS THAT BITE (the list every arena generator here keeps):
  * mapRead recenters the terrain bounding-box midpoint to (126,126), so every
    arena asserts its own midpoint is already there and nothing shifts.
  * a start square must be DEEP SEA (starts.c startsIsValidSquare) -- the
    one-tile ponds -- and the pond is a hole in the straight-line walk sim, so
    the sidecar fills it back in once the tank is ashore and OFF it (filling it
    under the tank leaves the boat state stuck and the LGM never comes free).
  * mapRead puts ROAD under every map-file pill.
  * -gametype open starts a tank with TANK_FULL_TREES (40).

Usage:
    python3 tests/generate_farm_sectors_map.py [variant]
    variant: A | A2 | B | B2   (default: all of them)
Writes tests/farm_sectors_<V>.map and tests/farm_sectors_<V>.scenario.lua.
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

BUILDING = 0
ROAD = 4
FOREST = 5
GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256

# constants.lua, mirrored so the test file reads them from one place.
LEASH = 8                  # BUILDER_POOL_LEASH (the farm row's reach)
MIN_SCORE = 20             # BUILDER_POOL_MIN_SCORE
TRIP_W = 0.5               # BUILDER_POOL_TRIP_W
LGM_BUILD_TIME = 20
MAN_TICKS_PER_TILE = 16    # BUILDER_POOL_GRASS_TICKS_PER_TILE / lgm_man_speed
TANK_ROAD_SPEED = 16       # C.MAP_SPEED[T_ROAD] = MAP_SPEED_TROAD, wu/brain tick
VALUE_FARM_CFG = 99        # what the arenas pump BUILDER_POOL_VALUE_FARM to
PREDICT_MAX_TICKS = 400    # BUILDER_POOL_RETURN_PREDICT_MAX_TICKS

# ── Arena A / A2: the wall ───────────────────────────────────────────────
# Nothing is baked in but the ground: the sidecar plants the wall and both
# forests relative to wherever the tank comes to rest (see the docstring).
A_FIELD = (120, 132, 124, 128)     # x0, x1, y0, y1 -- midpoint (126,126)
A_SPAWN = (126, 126)
A_WALL_DX = -1                     # BUILDING column at tank_mx - 1, spanning
                                   # tank_my-1 .. tank_my+1 so no straight line
                                   # from the tank can slip past it
A_BLOCKED_DX = -2                  # forest behind the wall: NEAREST, and the
                                   # only row FARM_SECTORS=1 ever offers
A_OPEN_DX = 3                      # clear forest, further out, other wedge
A_FILL_TICK = 60                   # engine ticks: the tank is ashore by then
A_PLANT_TICK = 400                 # ...and has stopped moving by then

# ── Arena B / B2: the drive ──────────────────────────────────────────────
B_FIELD = (112, 140, 125, 127)     # midpoint (126,126)
B_SPAWN = (116, 126)               # pond at the west end; tank faces east
B_TARGET = (139, 126)              # NEUTRAL base: the reason to drive east,
                                   # and a goal whose builder mode leaves the
                                   # pool eligible (see build())
B_PLANT_X = 126                    # the sidecar plants when tank_mx hits this
B_PLANT_DY = 1                     # both forests one row south of the drive
                                   # row, so the tank never drives OVER one
B_PLANT_DX = 3                     # ...and this far behind and ahead
B_FILL_TICK = 40

VARIANTS = ("A", "A2", "B", "B2")
# A2 is arena A's ground, B2 is arena B's; only the -bot-init tokens differ.
# Each still gets its OWN map and sidecar because the sidecar is found by map
# name and each writes its own trace.
BASE_OF = {"A": "A", "A2": "A", "B": "B", "B2": "B"}


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def fill(t, box, tile=GRASS):
    x0, x1, y0, y1 = box
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            t[yy][xx] = tile


def pond(t, sq):
    t[sq[1]][sq[0]] = DEEP_SEA


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def euclid(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def build(variant):
    """Returns terrain, pills, bases, starts for the variant."""
    t = blank()
    base = BASE_OF[variant]
    if base == "A":
        fill(t, A_FIELD, GRASS)
        pond(t, A_SPAWN)
        pills = []
        bases = []
        starts = [(A_SPAWN[0], A_SPAWN[1], 4)]         # facing east
    else:  # B
        fill(t, B_FIELD, ROAD)
        pond(t, B_SPAWN)
        # A NEUTRAL BASE, and it has to be a BASE and not a pill. The tank
        # needs a goal that makes it DRIVE twenty tiles -- that is what gives
        # the return-leg prediction a committed route to walk -- but it also
        # needs the builder pool to stay ELIGIBLE while it drives, and those
        # two demands only meet on a handful of goals. builder.lua's
        # GOAL_TO_MODE puts capture_pill (and repair_pill) in "gather", a
        # goal-tied WORKING mode, and the pool refuses every row under it with
        # `mode_owned (gather)` -- measured: the first version of this arena
        # drove east behind a dead pill and the pool denied all 419 ticks of
        # it. capture_base is "opportunistic", an idle-ish mode, so the pool
        # runs normally the whole way there.
        # A base is also not a builder-pool row of any kind, so it cannot
        # compete with the two farm rows.
        pills = []
        bases = [(B_TARGET[0], B_TARGET[1], 0, 90, 90, 90)]
        starts = [(B_SPAWN[0], B_SPAWN[1], 4)]         # facing east
    return t, pills, bases, starts


def check(variant, terrain, pills, bases, starts):
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    mid = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    assert mid == (126, 126), (
        f"variant {variant}: terrain midpoint is {mid}, not (126,126) -- "
        f"mapRead would shift every coordinate in this file")
    for (sx, sy, _d) in starts:
        assert terrain[sy][sx] is DEEP_SEA, (
            f"variant {variant}: start ({sx},{sy}) must be deep sea "
            f"(starts.c startsIsValidSquare)")
    for (px, py, _o, _a, _s) in pills:
        assert terrain[py][px] is not DEEP_SEA, (
            f"variant {variant}: pill ({px},{py}) is in the sea")

    base = BASE_OF[variant]
    if base == "A":
        # The geometry is planted at run time, so what is asserted here is that
        # it FITS wherever the tank can be, and that it says what the arena
        # claims it says.
        assert abs(A_BLOCKED_DX) < abs(A_OPEN_DX), (
            "arena A: the WALLED forest must be the NEARER one -- it is the "
            "row FARM_SECTORS=1 offers, and the control's whole claim is that "
            "the pool then has nothing it can send the man to")
        assert A_BLOCKED_DX < 0 < A_OPEN_DX, (
            "arena A: the two forests must be on OPPOSITE sides of the tank, "
            "or they land in the same wedge and the change does nothing here")
        assert abs(A_OPEN_DX) <= LEASH, (
            f"arena A: the open forest is {abs(A_OPEN_DX)} tiles out, past "
            f"BUILDER_POOL_LEASH ({LEASH}) -- it would not be a row at all")
        assert A_BLOCKED_DX < A_WALL_DX < 0, (
            "arena A: the wall has to sit BETWEEN the tank and the walled "
            "forest or nothing is blocked")
        assert abs(A_BLOCKED_DX) > 1, (
            "arena A: a forest 1 tile away skips the walk sim entirely "
            "(M.lgm_trip's adjacent shortcut) and could never read unreachable")
        out = abs(A_OPEN_DX) * MAN_TICKS_PER_TILE
        trip = 2 * out + LGM_BUILD_TIME
        score = VALUE_FARM_CFG - TRIP_W * trip
        assert score >= MIN_SCORE + 10, (
            f"arena A: the open forest scores {score:.0f} against MIN_SCORE "
            f"{MIN_SCORE} -- too close to the line to be evidence; move it in")
        x0, x1, y0, y1 = A_FIELD
        assert (x1 - x0) >= (abs(A_BLOCKED_DX) + A_OPEN_DX + 4), (
            "arena A: the field is too narrow to hold the wall and both "
            "forests wherever the tank comes ashore")
        assert (y1 - y0) >= 2, (
            "arena A: the field needs three rows for the wall column")
    else:
        behind = (B_PLANT_X - B_PLANT_DX, 126 + B_PLANT_DY)
        ahead = (B_PLANT_X + B_PLANT_DX, 126 + B_PLANT_DY)
        tank = (B_PLANT_X, 126)
        assert behind[0] < B_PLANT_X < ahead[0], (
            "arena B: one forest behind and one ahead, or there is no "
            "comparison")
        assert mdist(tank, behind) == mdist(tank, ahead), (
            f"arena B: the two forests are {mdist(tank, behind)} and "
            f"{mdist(tank, ahead)} tiles from the tank -- the OUTBOUND legs "
            f"have to be equal or the old score can already tell them apart "
            f"and the arena is not about the return leg at all")
        assert mdist(tank, ahead) <= LEASH, (
            "arena B: the forests are outside BUILDER_POOL_LEASH")
        assert B_PLANT_DY != 0, (
            "arena B: a forest on the drive row is one the tank drives over "
            "and the on-path harvest chops")
        for f in (behind, ahead):
            assert B_FIELD[2] <= f[1] <= B_FIELD[3], (
                f"arena B: forest {f} is off the corridor")
            assert B_FIELD[0] <= f[0] <= B_FIELD[1], (
                f"arena B: forest {f} is off the corridor")
        out = round(euclid(tank, ahead) * MAN_TICKS_PER_TILE)
        keel_trip = 2 * out + LGM_BUILD_TIME
        keel_score = VALUE_FARM_CFG - TRIP_W * keel_trip
        assert keel_score >= MIN_SCORE + 10, (
            f"arena B: under the OLD number both rows score {keel_score:.0f} "
            f"against MIN_SCORE {MIN_SCORE} -- the control needs its dispatch "
            f"to actually happen, so bring the forests in")
        # The tie the control has to break, and which way. order_rows sorts on
        # score, then type rank, then tile key (my*256+mx): equal scores hand
        # it to the LOWER key, which is the western (behind) forest.
        assert (behind[1] * 256 + behind[0]) < (ahead[1] * 256 + ahead[0]), (
            "arena B: the control's tie-break has to land on the BEHIND "
            "forest -- that is the wrong answer the change exists to fix")
        # The prediction: walk the ROUTE (the road) at MAP_SPEED[T_ROAD].
        horizon = min(out + LGM_BUILD_TIME, PREDICT_MAX_TICKS)
        per_tile = 256.0 / TANK_ROAD_SPEED
        tiles = int(horizon // per_tile)
        pred = (B_PLANT_X + tiles, 126)
        assert pred[0] <= B_FIELD[1], (
            f"arena B: the predicted tile {pred} runs off the east end of the "
            f"corridor -- lengthen it or the prediction falls back")
        back_a = round(euclid(ahead, pred) * MAN_TICKS_PER_TILE)
        back_b = round(euclid(behind, pred) * MAN_TICKS_PER_TILE)
        assert back_a < back_b, (
            f"arena B: predicted back legs are {back_a} (ahead) and {back_b} "
            f"(behind) -- the ahead one has to be the shorter walk home or "
            f"there is nothing for the prediction to find")
        sa = VALUE_FARM_CFG - TRIP_W * (out + LGM_BUILD_TIME + back_a)
        sb = VALUE_FARM_CFG - TRIP_W * (out + LGM_BUILD_TIME + back_b)
        assert sa >= MIN_SCORE and sb < MIN_SCORE, (
            f"arena B: with prediction the ahead row scores {sa:.0f} and the "
            f"behind row {sb:.0f}; the arena wants the ahead one fireable and "
            f"the behind one refused below MIN_SCORE ({MIN_SCORE})")
        assert B_TARGET[0] - B_PLANT_X >= 8, (
            "arena B: the capture target is too close to the plant column -- "
            "the tank has to still be DRIVING (and so have a route) when the "
            "forests appear")
        assert B_TARGET[0] - B_PLANT_X > tiles, (
            f"arena B: the prediction reaches {tiles} tiles past the plant "
            f"column but the drive only has {B_TARGET[0] - B_PLANT_X} left in "
            f"it -- the route would END before the horizon and the predicted "
            f"tile would be the base, not a point on the road")


# ── Scenario sidecars ────────────────────────────────────────────────────
# Written next to the map as <map>.scenario.lua, which is how the server finds
# them. Generated rather than hand-kept because every coordinate in them is a
# coordinate above, and the two drifting apart is the failure mode this whole
# file exists to prevent.

SIDECAR_A = '''-- Scenario sidecar for tests/farm_sectors_{V}.map (auto-loaded as
-- <map>.scenario.lua). Companion to tests/farm_sectors_test.py arena {V}.
-- GENERATED by tests/generate_farm_sectors_map.py -- edit that, not this.
--
-- ARENA A: a tank with nothing to do, a forest behind a wall, and a clear
-- forest further away on the other side. The whole geometry is planted HERE,
-- relative to the tile the tank comes to rest on, because "the walled forest
-- is the nearest one" is a claim about the tank's position and baking it into
-- the map file would make it a claim about where the tank happened to stop.
--
-- The tank stops because it has nothing to do: no pills, no bases, and
-- EXPLORE_BASE_COST priced past the unaffordable line by the -bot-init tokens.
-- TANK lines in the trace record every tile it stands on, so the test can
-- prove it stayed put rather than assuming it.
--
-- FILLING THE SPAWN POND. A start square has to be DEEP SEA at map load
-- (starts.c startsIsValidSquare), and the LGM walk sim walks a STRAIGHT LINE
-- with a crude slide, so a hole anywhere on the line makes the target read
-- `unreachable`. The pond is filled back to grass once the tank is ashore and
-- OFF it -- filling it under the tank leaves the boat state stuck and the LGM
-- never becomes available at all.

local TRACE = "farm_sectors_{V}_trace.log"
local SPAWN = {{ {SPAWN_X}, {SPAWN_Y} }}
local FIELD = {{ {FX0}, {FX1}, {FY0}, {FY1} }}   -- x0, x1, y0, y1
local WALL_DX, BLOCKED_DX, OPEN_DX = {WALL_DX}, {BLOCKED_DX}, {OPEN_DX}
local FILL_TICK, PLANT_TICK = {FILL_TICK}, {PLANT_TICK}
local MOAT, FOREST, GRASS = 0xFF, 5, 7   -- 0xFF = DEEP SEA (scenario.c l_set_tile)
local p0 = 0

local filled = false
local planted = false
local last_tile = nil

local function log(fmt, ...)
  local f = io.open(TRACE, "a")
  if f then f:write(string.format(fmt, ...) .. "\\n") f:close() end
end

function on_setup(g)
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# TANK tick mx my | PLANT tick mx my wallx blx bly opx opy\\n") f:close() end
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_tick(g, tick)
  local tk = g.tank(p0)
  if not tk then return end
  if not tk.dead then
    local key = tk.mx .. "," .. tk.my
    if last_tile ~= key then
      last_tile = key
      log("TANK %d %d %d", tick, tk.mx, tk.my)
    end
  end

  if not filled and tick >= FILL_TICK then
    if not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message(string.format(
        "FARM_SECTORS_{V} filled the spawn pond at (%d,%d) at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end

  -- Plant only once the tank has settled AND the geometry fits inside the
  -- field from where it is standing. If it never fits the test says so; it
  -- must never plant a half-arena and let the run look like a real result.
  if filled and not planted and tick >= PLANT_TICK and not tk.dead
     and not tk.boat then
    local mx, my = tk.mx, tk.my
    local ok = (mx + BLOCKED_DX >= FIELD[1]) and (mx + OPEN_DX <= FIELD[2])
    if ok then
      planted = true
      -- A MOAT OF DEEP SEA, not a wall of buildings, and it spans the WHOLE
      -- field height. Two measured failures are behind both halves of that:
      --   * a three-tile BUILDING wall beside the tank was walked around --
      --     the walk sim runs a straight line from wherever the tank IS, and a
      --     two-row drift clears the end of a short wall (arena A dispatched
      --     to the "walled" forest at t=526 on the first attempt);
      --   * a FULL-HEIGHT building wall was SHOT DOWN. The bot prices walls
      --     into its own paths (wall_shoot_cost) and cleared the column in
      --     ~270 sim ticks, then drove through the gap.
      -- Deep sea is the one barrier the man can never cross
      -- (brain_pathfinder.c lgm_man_speed[10] = 0) and the tank can never
      -- remove -- the same moat tests/generate_repair_priority_map.py uses to
      -- keep its neutral island unreachable.
      for wy = FIELD[3], FIELD[4] do
        g.set_tile(mx + WALL_DX, wy, MOAT)
      end
      g.set_tile(mx + BLOCKED_DX, my, FOREST)
      g.set_tile(mx + OPEN_DX, my, FOREST)
      log("PLANT %d %d %d %d %d %d %d %d", tick, mx, my,
          mx + WALL_DX, mx + BLOCKED_DX, my, mx + OPEN_DX, my)
      g.message(string.format(
        "FARM_SECTORS_{V} planted: wall x=%d, walled forest (%d,%d),"
        .. " open forest (%d,%d), tank (%d,%d) at t=%d",
        mx + WALL_DX, mx + BLOCKED_DX, my, mx + OPEN_DX, my, mx, my, tick))
    end
  end
end
'''

SIDECAR_B = '''-- Scenario sidecar for tests/farm_sectors_{V}.map (auto-loaded as
-- <map>.scenario.lua). Companion to tests/farm_sectors_test.py arena {V}.
-- GENERATED by tests/generate_farm_sectors_map.py -- edit that, not this.
--
-- ARENA B: a tank DRIVING east along a road to scoop a dead pill twenty tiles
-- away -- which is what gives it the committed route the return-leg prediction
-- walks. The moment it enters column PLANT_X the sidecar plants two forests,
-- one PLANT_DX tiles behind and one PLANT_DX ahead, both PLANT_DY rows south
-- of the drive row. Equal outbound legs by construction; only the walk HOME
-- can tell them apart.
--
-- They are planted rather than baked in because "the same distance away" is
-- true for exactly one tick of a moving tank, and game.set_tile is the only
-- way to make that tick the arena's to choose.

local TRACE = "farm_sectors_{V}_trace.log"
local SPAWN = {{ {SPAWN_X}, {SPAWN_Y} }}
local TARGET = {{ {TGT_X}, {TGT_Y} }}
local PLANT_X, PLANT_DX, PLANT_DY = {PLANT_X}, {PLANT_DX}, {PLANT_DY}
local FILL_TICK = {FILL_TICK}
local FOREST, GRASS = 5, 7
local p0 = 0

local filled = false
local planted = false
local last_tile = nil

local function log(fmt, ...)
  local f = io.open(TRACE, "a")
  if f then f:write(string.format(fmt, ...) .. "\\n") f:close() end
end

local function same(p, t) return p.x == t[1] and p.y == t[2] end

function on_setup(g)
  -- The drive target is a NEUTRAL base: capture_base is what makes the tank
  -- cross the map, and (unlike capture_pill, which sets builder mode
  -- "gather") it leaves the builder pool eligible the whole way.
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and same(b, TARGET) then g.set_base_owner(i, g.NEUTRAL) end
  end
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# TANK tick mx my | PLANT tick mx my bx by ax ay\\n") f:close() end
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_tick(g, tick)
  local tk = g.tank(p0)
  if not tk then return end
  if not tk.dead then
    local key = tk.mx .. "," .. tk.my
    if last_tile ~= key then
      last_tile = key
      log("TANK %d %d %d", tick, tk.mx, tk.my)
    end
  end

  if not filled and tick >= FILL_TICK then
    if not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
    end
  end

  if filled and not planted and not tk.dead and not tk.boat
     and tk.mx >= PLANT_X then
    planted = true
    local my = tk.my + PLANT_DY
    local bx, ax = PLANT_X - PLANT_DX, PLANT_X + PLANT_DX
    g.set_tile(bx, my, FOREST)
    g.set_tile(ax, my, FOREST)
    log("PLANT %d %d %d %d %d %d %d", tick, tk.mx, tk.my, bx, my, ax, my)
    g.message(string.format(
      "FARM_SECTORS_{V} planted: behind (%d,%d), ahead (%d,%d),"
      .. " tank (%d,%d) at t=%d", bx, my, ax, my, tk.mx, tk.my, tick))
  end
end
'''


def write_sidecar(variant, path):
    base = BASE_OF[variant]
    if base == "A":
        txt = SIDECAR_A.format(
            V=variant, SPAWN_X=A_SPAWN[0], SPAWN_Y=A_SPAWN[1],
            FX0=A_FIELD[0], FX1=A_FIELD[1], FY0=A_FIELD[2], FY1=A_FIELD[3],
            WALL_DX=A_WALL_DX, BLOCKED_DX=A_BLOCKED_DX, OPEN_DX=A_OPEN_DX,
            FILL_TICK=A_FILL_TICK, PLANT_TICK=A_PLANT_TICK)
    else:
        txt = SIDECAR_B.format(
            V=variant, SPAWN_X=B_SPAWN[0], SPAWN_Y=B_SPAWN[1],
            TGT_X=B_TARGET[0], TGT_Y=B_TARGET[1],
            PLANT_X=B_PLANT_X, PLANT_DX=B_PLANT_DX, PLANT_DY=B_PLANT_DY,
            FILL_TICK=B_FILL_TICK)
    Path(path).write_text(txt, encoding="ascii", newline="\n")


def write(variant, output):
    terrain, pills, bases, starts = build(variant)
    check(variant, terrain, pills, bases, starts)

    with open(output, 'wb') as f:
        f.write(b'BMAPBOLO')
        f.write(struct.pack('B', 1))
        f.write(struct.pack('B', len(pills)))
        f.write(struct.pack('B', len(bases)))
        f.write(struct.pack('B', len(starts)))
        for x, y, owner, armour, speed in pills:
            f.write(struct.pack('BBBBB', x, y, owner, armour, speed))
        for x, y, owner, armour, shells, mines in bases:
            f.write(struct.pack('BBBBBB', x, y, owner, armour, shells, mines))
        for x, y, dr in starts:
            f.write(struct.pack('BBB', x, y, dr))
        f.write(encode_map_runs(terrain))
    side = str(Path(output).with_suffix('')) + ".scenario.lua"
    if output.endswith(".map"):
        side = output[:-4] + ".scenario.lua"
    write_sidecar(variant, side)
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes) + "
          f"{Path(side).name} [variant {variant}: {len(pills)} pill(s), "
          f"{len(bases)} base(s), {len(starts)} start(s)]")


def main():
    args = sys.argv[1:]
    variants = [args[0]] if args and args[0] in VARIANTS else list(VARIANTS)
    here = Path(__file__).parent
    for v in variants:
        out = (args[1] if len(args) > 1 and len(variants) == 1
               else str(here / f"farm_sectors_{v}.map"))
        write(v, out)


if __name__ == '__main__':
    main()
