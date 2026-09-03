#!/usr/bin/env python3
"""
Generate the placement-pin arenas (companion to tests/place_pin_range_test.py).

Field incident 20260903_105448 bot2 t=34774.  The carry pressure had done its
job: the pool-8 placement row priced 1.0 with four pills aboard and the carry
discount maxed at -300.  goal_selection then overwrote it with 2163 because "a
NORMAL place_pill_strategic must never out-rank an attack_tank", and the
cheapest attack_tank row cost 2162 -- for an enemy FIFTEEN tiles away on the
far side of water this tank could not cross.  Nothing was placed for the rest
of the game and the bot died carrying all four pills.

The brain change these arenas exercise (author, 2026-09-03): the pin applies
only while a hostile tank is within C.PLACE_PIN_ENEMY_RANGE (7 tiles = gun
range) of our tank.  Outside it the placement competes on its own cost.  The
verdict is stamped on the place row's desc, so FINAL_SCORES says which way it
went and why:  ` pin{atk 2162.0+1, enemy 5t<=7}` / ` nopin{enemy 15t>7, atk 2162.0}`.

ONE CANVAS, TWO VARIANTS.  Everything is written between x 110..142 and
y 110..142 as plain grass, symmetric about (126,126), so mapRead's recenter is
a no-op and file coordinates are game coordinates.  Our corner is the east
side:

    (138,126)  our start: a one-tile deep-sea pond (starts.c requires water)
    (136,130)  our base, full stock
    (134,132) (135,132) (136,132)
               three healthy friendly pills, all inside PILL_FIRE_RANGE (8) of
               the base so the reposition pool leaves them alone.  The sidecar
               hands all three to the tank at GIVE_TICK; three aboard is what
               drives the carry pressure that the pin used to cancel.

The opponent is one tank running tests/brains/patrol_ns.lua, which drives
south off its start pond and then paces y=116..133 for ever (Y0/Y1 are
hardcoded in that brain -- keep them in step).  It never fires; it only moves.
Where its lane runs is the ONLY difference between the variants:

  far     lane x=126, ten tiles west of our base.  This is the incident's
          shape: a hostile tank that is VISIBLE, real and priced, and still
          much too far away to shoot us.  Ten and not twenty: an enemy the
          tank cannot SEE is not in the object list at all, so perception has
          no nearest_hostile_tank and attack_tank has nothing to price -- the
          pin question is never asked and the arena measures nothing.  Under
          the old rule this tank's attack_tank row pinned every placement;
          under the new one the row is left alone and the pills go into the
          ground.

  near    lane x=133, three tiles west of our pills and inside gun range of
          everywhere our tank works.  While it is within PLACE_PIN_ENEMY_RANGE
          a casual placement must NOT win the goal.

THE GROUND IS OPEN ON PURPOSE.  An enemy behind an uncrossable moat was tried
first and proved useless twice over: attack_tank prices an unreachable tank at
INF so its row never reaches the pool, and at twenty-plus tiles the tank is
not in the object list at all so there is no nearest_hostile_tank either.  The
incident's enemy was across water but still both seen and priced (2162), which
is the case this has to reproduce.

WHY THE OPEN LOADOUT.  Placing a pill costs PILL_PLACE_TREE_COST wood and a
tournament tank starts with none, so these arenas run open (40 shells / 40
mines / 40 trees).  patrol_ns has no fire routine, so the shells only ever go
one way.

Usage:
    python3 tests/generate_place_pin_map.py [--variant far|near|ALL]
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256

# ── Geometry (the test runner imports these) ─────────────────────────────
BODY_X = (110, 142)
BODY_Y = (110, 142)

OUR_SPAWN = (138, 126)
OUR_BASE = (136, 130)
SPARE_PILLS = [(134, 132), (135, 132), (136, 132)]
# The patrol tank's start pond sits NORTH of PATROL_Y0 so, once it has driven
# south off it, its lane never crosses it again (a tank without a boat drowns
# on deep sea).
FOE_SPAWN = {"far": (126, 112), "near": (133, 112)}
PATROL_Y0 = 116                 # must match tests/brains/patrol_ns.lua
PATROL_Y1 = 133
GIVE_TICK = 200                 # SIM ticks (brain tick 100)

PILLS_MAX_HEALTH = 15
PILL_FIRE_RANGE = 8             # constants.lua
PLACE_PIN_ENEMY_RANGE = 7       # constants.lua -- the gate under test
VARIANTS = ("far", "near")


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def ponds(variant):
    return [OUR_SPAWN, FOE_SPAWN[variant]]


def make_map(variant):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    for y in range(BODY_Y[0], BODY_Y[1] + 1):
        for x in range(BODY_X[0], BODY_X[1] + 1):
            t[y][x] = GRASS
    for (x, y) in ponds(variant):
        t[y][x] = DEEP_SEA
    return t


SIDECAR = r'''
-- Scenario sidecar for tests/place_pin_{V}.map -- GENERATED by
-- tests/generate_place_pin_map.py, do not edit by hand.
--
-- Four jobs.
--
-- 1. OWN the base and all three pills, and put our tank on its own team so the
--    scripted opponents are hostile to it.
--
-- 2. PIN THE STARTS.  on_choose_start fires for every placement (spawn AND
--    respawn), so each tank always comes back on the pond this arena designed
--    for it -- start 1 ours, start 2 the patrol.
--
-- 3. HAND OVER THE PILLS at tick {GIVE_TICK}.  Three aboard is what makes the
--    carry pressure real: place_pill_strategic's cost collapses, and whether
--    it is allowed to WIN with that cost is the whole question.
--
-- 4. FILL THE SPAWN PONDS once nobody is afloat.  A start square has to be
--    deep sea, and a tank that later drives over one without a boat drowns --
--    noise this test does not want.

local OUR_PLAYER = 0
local PILL_IDS   = {PILLIDS}
local PONDS      = {PONDS}
local GIVE_TICK  = {GIVE_TICK}
local GRASS      = 7
local given = false
local ponds_filled = false

function on_setup(g)
  for i = 1, g.num_pills() do g.set_pill_owner(i, OUR_PLAYER) end
  for i = 1, g.num_bases() do g.set_base_owner(i, OUR_PLAYER) end
  g.set_team(OUR_PLAYER, 0)
  g.message(string.format("PLACE_PIN_ARENA pills=%d give=%d",
                          g.num_pills(), GIVE_TICK))
end

function on_choose_start(g, p)
  if p == 0 then return 1 end      -- ours
  if p == 1 then return 2 end      -- the patrol
  return nil
end

function on_tick(g, tick)
  if not given and tick >= GIVE_TICK then
    given = true
    local n = 0
    for _, i in ipairs(PILL_IDS) do
      local ok, err = g.give_pill(OUR_PLAYER, i)
      if ok then n = n + 1
      else g.message("PLACE_PIN_ARENA give_pill#" .. i .. " failed: "
                     .. tostring(err)) end
    end
    g.message("PLACE_PIN_ARENA gave " .. n .. " pill(s) at tick " .. tick)
  end
  if not ponds_filled and tick > 200 then
    local afloat = false
    for p = 0, 1 do
      local t = g.tank(p)
      if t and t.boat then afloat = true end
    end
    if not afloat then
      ponds_filled = true
      for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
      g.message("PLACE_PIN_ARENA spawn ponds filled at tick " .. tostring(tick))
    end
  end
end
'''


def write_sidecar(variant, path):
    text = (SIDECAR
            .replace("{V}", variant)
            .replace("{PILLIDS}",
                     "{ " + ", ".join(str(i + 1) for i in range(len(SPARE_PILLS)))
                     + " }")
            .replace("{PONDS}",
                     "{ " + ", ".join("{ %d, %d }" % p for p in ponds(variant))
                     + " }")
            .replace("{GIVE_TICK}", str(GIVE_TICK)))
    Path(path).write_text(text.lstrip("\n"), encoding="utf-8", newline="\n")


def build(variant, output):
    terrain = make_map(variant)
    foe = FOE_SPAWN[variant]
    pills = [(x, y, 0, PILLS_MAX_HEALTH, 50) for (x, y) in SPARE_PILLS]
    bases = [(OUR_BASE[0], OUR_BASE[1], 0, 90, 90, 90)]
    starts = [(OUR_SPAWN[0], OUR_SPAWN[1], 6),      # 1: ours, facing west
              (foe[0], foe[1], 8)]                  # 2: the patrol, facing south

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    for p in ponds(variant):
        assert terrain[p[1]][p[0]] is DEEP_SEA, (
            f"start {p} must be deep sea (starts.c startsIsValidSquare)")

    # The lane has to be land the whole way, and must never re-cross the pond
    # the patrol tank left (a tank without a boat drowns on deep sea).
    for y in range(PATROL_Y0, PATROL_Y1 + 1):
        assert terrain[y][foe[0]] is not DEEP_SEA, (foe[0], y)
    assert not (PATROL_Y0 <= foe[1] <= PATROL_Y1), foe
    # ...and it must not run over our base, or the patrol tank captures it in
    # passing and the arena turns into a base fight.
    assert foe[0] != OUR_BASE[0] or not (PATROL_Y0 <= OUR_BASE[1] <= PATROL_Y1)

    # Every friendly pill needs a base inside PILL_FIRE_RANGE or the
    # reposition pool bids to shoot it down instead of leaving it alone.
    for p in SPARE_PILLS:
        d = mdist(p, OUR_BASE)
        assert d <= PILL_FIRE_RANGE, (p, d)

    # How close the lane ever brings the patrol to our base -- the number the
    # whole variant turns on.
    closest = min(mdist((foe[0], y), OUR_BASE)
                  for y in range(PATROL_Y0, PATROL_Y1 + 1))
    if variant == "far":
        assert PLACE_PIN_ENEMY_RANGE + 2 <= closest <= 12, (
            f"the `far` lane's closest approach to our base is {closest} "
            f"tiles. It has to clear PLACE_PIN_ENEMY_RANGE "
            f"{PLACE_PIN_ENEMY_RANGE} with room to spare AND stay inside the "
            f"tank's sight, or there is no visible enemy to not-pin against")
    else:
        assert closest <= PLACE_PIN_ENEMY_RANGE, (
            f"the `near` lane never comes within PLACE_PIN_ENEMY_RANGE "
            f"{PLACE_PIN_ENEMY_RANGE} of our base ({closest} tiles) -- the pin "
            f"would never be asked to fire")

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
        for x, y, d in starts:
            f.write(struct.pack('BBB', x, y, d))
        f.write(encode_map_runs(terrain))
    return closest


def main_one(variant):
    here = Path(__file__).parent
    out = here / f"place_pin_{variant}.map"
    closest = build(variant, out)
    write_sidecar(variant, here / f"place_pin_{variant}.scenario.lua")
    print(f"Wrote {out} ({out.stat().st_size} bytes) variant={variant}")
    print(f"  our spawn {OUR_SPAWN}, base {OUR_BASE}, pills {SPARE_PILLS} "
          f"handed over at sim tick {GIVE_TICK}")
    print(f"  patrol start {FOE_SPAWN[variant]}, lane x={FOE_SPAWN[variant][0]} "
          f"y={PATROL_Y0}..{PATROL_Y1}; closest it ever gets to our base is "
          f"{closest} tiles (PLACE_PIN_ENEMY_RANGE {PLACE_PIN_ENEMY_RANGE})")


def main():
    args = sys.argv[1:]
    variant = "ALL"
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            variant = args[i + 1]; i += 2
        else:
            i += 1
    for v in (VARIANTS if variant == "ALL" else (variant,)):
        assert v in VARIANTS, f"unknown variant {v}"
        main_one(v)


if __name__ == '__main__':
    main()
