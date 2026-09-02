#!/usr/bin/env python3
"""
Generate the base-steal arena (companion to tests/base_steal_test.py).

Field incident 20260901_160325_1_par2 bot3, t=23491.  Four hostile bases stood
within six tiles of the tank; the bot had 35 armour and 28 shells and never bid
on any of them as a steal.  attack_base surfaced ONE candidate at cost 169 --
path plus a FLAT 80 markup that took no notice of how much armour was left --
and lost every pool.

Two things came out of that incident and this arena tests both:

  1. ARMOUR-AWARE PRICING.  ATTACK_BASE_EXTRA_COST is now scaled by the work
     left: shells-still-needed / 17, where 17 = ceil((90 - 9)/5) is what a FULL
     base costs in shells.  A base three shells from falling pays 80 x 3/17
     ~= 14; an untouched one still pays 80.

  2. IMMINENT BASE STEAL.  A hostile base with a fresh armour reading at or
     below BASE_STEAL_MAX_ARMOUR (24 -- three shells from capturable), within
     BASE_STEAL_RANGE (6) tiles, with the ammo to finish it, no pillbox covering
     the base tile or the straight approach, a healthy tank and no boat, is
     snapped to BASE_STEAL_COST (10) in the attack_base slot.  One shell, and
     the existing armour-0 IMMINENT capture takes the hand-off.

ENGINE FACTS THIS ARENA RESTS ON
  bases.h    BASE_FULL_ARMOUR 90, MIN_ARMOUR_CAPTURE 9, BASE_TICKS_BETWEEN_REFUEL 1000
  global.h   DAMAGE 5 (armour a shell takes off a base)
  bases.c ~1649  basesGetItems FOGS hostile base armour in the per-tick object
             scan: an enemy base reports direction 1 ("alive") or 0
             ("capturable"), never a number.  The real armour reaches a BOT
             through EVENT_BASE_STOCK, which server_sim.c ~3667 culls to
             neutral/allied bases for humans but sends in full to bots.
             EVENT_BASE_STOCK fires only when a base's armour/shells/mines
             CHANGE, which is why the sidecar re-asserts stock periodically --
             otherwise an untouched base goes quiet and the bot's reading ages
             out.  (The sidecar never HEALS: it clamps armour down, never up.)
  starts.c   a start square must be DEEP SEA, and a start within
             START_BASE_RANGE (9) of a HOSTILE base is skipped -- which is why
             the bases are written NEUTRAL in the map file and only turned
             hostile in on_setup, which runs after the tanks are placed.

Two variants:

  A  Two hostile bases at armour 12 -- ONE shell from capturable -- three tiles
     west and three tiles east of the spawn, and a third at FULL armour 90
     twelve tiles south.  Nothing else on the map.  Both near bases must be
     stolen and flipped inside the minute, and the far base's row must still
     quote the full markup.

  B  A's arena with a NEUTRAL pillbox added seven tiles east of the EAST base, on
     its own one-tile island in a deep-sea moat so the bot can neither drive to
     it nor shoot it down.  It has the range and a clear line onto that base
     AND onto the straight approach to it, so the steal's coverage guard must
     reject the east base -- while the west base, thirteen tiles away from the
     pillbox, is still stolen.  A alone cannot prove the guard did anything: B
     is the control, and the pillbox is the only thing that moved.

Neutral rather than an enemy player's pillbox for the guard, for the same
reasons as the capture-cluster arena: a neutral pillbox shoots every tank in
range, it needs no second player, and the brain's sea_threat_pills (which the
steal's coverage guard reuses) counts hostile and neutral identically.

IMPORTANT: mapRead RECENTERS off-center maps -- the terrain bounding-box
midpoint is shifted to (126,126), but only when BOTH axes need it.  The field
is a rectangle symmetric about x=126 and y=126, so the recenter is a no-op and
in-game coordinates match this file.  main() asserts it.  (The deep-sea moat is
a hole INSIDE the field and does not move the bounding box.)

Usage:
    python3 tests/generate_base_steal_map.py [A|B] [output_path]
    Default: variant A -> tests/base_steal_A.map
Writes <output>.map and the matching <output>.scenario.lua sidecar.
"""

import struct
import sys
from pathlib import Path

GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)
MAP_SIZE = 256
NEUTRAL = 0xFF

# -- Geometry (the test runner imports these for its assertions) -------------
SPAWN = (126, 126)          # one-tile deep-sea pond (starts.c startsIsValidSquare)
FIELD = (110, 142, 110, 142)  # x0, x1, y0, y1 -- symmetric about (126,126)
WEST_BASE = (123, 126)      # 3 tiles from the spawn -- close enough that the
EAST_BASE = (129, 126)      # OTHER near base is still inside BASE_STEAL_RANGE
                            # while the tank is standing on this one, so variant
                            # B's coverage guard gets asked its question on more
                            # than one replan
FAR_BASE = (126, 138)       # 12 tiles from the spawn -- outside BASE_STEAL_RANGE
GUARD = (136, 126)          # variant B only: 7 tiles east of EAST_BASE
ENEMY_SLOT = 1              # phantom owner: playersIsAllie says "not my ally",
                            # so the brain reads these bases as hostile without
                            # a second tank driving around the arena

# -- Brain/engine constants this arena is designed against -------------------
BASE_FULL_ARMOUR = 90       # bases.h
MIN_ARMOUR_CAPTURE = 9      # bases.h -- at or below this a base is capturable
SHELL_DAMAGE = 5            # global.h DAMAGE
FULL_SHELLS_TO_KILL = 17    # ceil((90 - 9) / 5)
STEAL_MAX_ARMOUR = 24       # constants.lua BASE_STEAL_MAX_ARMOUR
MARKUP_STALE = 500          # constants.lua BASE_MARKUP_STALE
STEAL_RANGE = 6             # constants.lua BASE_STEAL_RANGE
STEAL_COST = 10             # constants.lua BASE_STEAL_COST
ATTACK_BASE_EXTRA_COST = 80  # constants.lua
PILLBOX_RANGE = 8           # tiles a pill can actually SHOOT
NEAR_ARMOUR = 12            # one shell (12 - 5 = 7 <= 9) from capturable
STOCK_PERIOD = 150          # sidecar re-assert cadence, in ticks


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def shells_to_kill(armour):
    over = armour - MIN_ARMOUR_CAPTURE
    return 0 if over <= 0 else -(-over // SHELL_DAMAGE)


def markup(armour, age=0):
    """What the brain's armour-aware attack_base markup charges (goals.lua
    base_markup).  The fraction of ATTACK_BASE_EXTRA_COST charged is the work
    left, shells-still-needed / 17, blended linearly back toward the full flat
    price as the armour reading ages out over MARKUP_STALE ticks:

        frac  = n / 17
        decay = min(1, age / MARKUP_STALE)
        eff   = frac + (1 - frac) * decay

    `age` None means "never observed": full price, no discount at all."""
    if age is None:
        return float(ATTACK_BASE_EXTRA_COST)
    frac = min(1.0, shells_to_kill(armour) / FULL_SHELLS_TO_KILL)
    decay = min(1.0, age / MARKUP_STALE) if MARKUP_STALE > 0 else 1.0
    return ATTACK_BASE_EXTRA_COST * min(1.0, frac + (1.0 - frac) * decay)


def near_bases():
    return [WEST_BASE, EAST_BASE]


def guards(variant):
    return [GUARD] if variant == "B" else []


def covered_base(variant):
    """The base variant B's pillbox is expected to lock out of the steal."""
    return EAST_BASE if variant == "B" else None


def steal_bases(variant):
    """The bases the bot is expected to steal in this variant."""
    return [WEST_BASE] if variant == "B" else [WEST_BASE, EAST_BASE]


def island(cx, cy):
    """The deep-sea ring that isolates the guard pillbox on its own tile."""
    return [(x, y) for y in range(cy - 1, cy + 2) for x in range(cx - 1, cx + 2)
            if (x, y) != (cx, cy)]


def approach_tiles(a, b):
    """The brain's base_approach_tiles: the straight tank->base line."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = max(abs(dx), abs(dy))
    if n < 1:
        return [a]
    return [(a[0] + int(dx * i / n + 0.5), a[1] + int(dy * i / n + 0.5))
            for i in range(n + 1)]


def make_map(variant="A"):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    fx0, fx1, fy0, fy1 = FIELD
    for yy in range(fy0, fy1 + 1):
        for xx in range(fx0, fx1 + 1):
            t[yy][xx] = GRASS
    for (gx, gy) in guards(variant):
        for (mx, my) in island(gx, gy):
            t[my][mx] = DEEP_SEA
    t[SPAWN[1]][SPAWN[0]] = DEEP_SEA
    return t


def encode_map_runs(terrain):
    """Nibble-run encode; one run per CONTIGUOUS non-deep span (follows
    mapProcessRun: length nibble 0-7 = that many +1 literal nibbles; 8-15 =
    run of len-6 copies of the next nibble)."""
    MAP_RUN_SAME = 6
    runs = bytearray()
    for y in range(MAP_SIZE):
        x = 0
        while x < MAP_SIZE:
            if terrain[y][x] is DEEP_SEA:
                x += 1
                continue
            startx = x
            while x < MAP_SIZE and terrain[y][x] is not DEEP_SEA:
                x += 1
            endx = x
            data = bytearray()
            i = startx
            while i < endx:
                run_start = i
                while (i < endx and terrain[y][i] == terrain[y][run_start]
                       and (i - run_start) < 9):
                    i += 1
                run_len = i - run_start
                tile = terrain[y][run_start]
                if run_len >= 2:
                    data.append(((run_len + MAP_RUN_SAME) << 4) | (tile & 0x0F))
                else:
                    data.append((0 << 4) | (tile & 0x0F))
            runs.append(4 + len(data))
            runs.append(y)
            runs.append(startx)
            runs.append(endx)
            runs.extend(data)
    runs.extend(b'\x04\xFF\xFF\xFF')
    return bytes(runs)


SIDECAR = '''\
-- Scenario sidecar for tests/base_steal_{V}.map -- GENERATED by
-- tests/generate_base_steal_map.py, do not edit by hand.
--
-- Four jobs.
--
-- 1. MAKE THE BASES HOSTILE, AFTER the tanks are placed.  starts.c skips any
--    start square within START_BASE_RANGE (9) of a hostile base, and both near
--    bases are three tiles from the spawn -- so the map file writes them NEUTRAL
--    and this flips them in on_setup, which runs once per round with the tanks
--    already placed and no client having seen the world yet.
--
--    The owner is slot {SLOT}, which nobody occupies.  playersIsAllie asks the
--    OCCUPIED side of the pair (our bot, slot 0), gets "not in my alliance",
--    and the brain reads the base as hostile -- with no enemy tank driving
--    around the arena to muddy the test.
--
-- 2. HOLD THE NEAR BASES ONE SHELL FROM FALLING.  Armour {NEAR_ARMOUR}: one
--    shell (DAMAGE 5) puts them at 7, at or below MIN_ARMOUR_CAPTURE (9), which
--    is capturable.  The far base stays at BASE_FULL_ARMOUR so its row must
--    still quote the full markup.
--
-- 3. FILL THE SPAWN POND once the tank has driven off it.  A start square has
--    to be DEEP SEA (starts.c startsIsValidSquare), so the arena has a one-tile
--    pond in the middle of it and the tank spawns afloat.  Once every base is
--    taken the bot has nothing left to do but explore a 33x33 field, and
--    wandering into that pond without a boat is an instant drowning -- noise
--    this test does not want to measure.  Filling it in with grass removes the
--    only water on the map.
--
-- 4. KEEP THE BOT'S ARMOUR READING ALIVE.  A bot only learns a hostile base's
--    armour from EVENT_BASE_STOCK, and the server only emits that when a base's
--    armour/shells/mines actually CHANGE.  An untouched base would go quiet and
--    the reading would age out, so every {PERIOD} ticks this re-asserts stock
--    with a mines value that alternates 90/89 -- a real change, so a real event.
--    It NEVER heals: armour is clamped DOWN to the target and left alone once
--    the bot has shot it.

local NEAR   = {{ {{ {WX}, {WY} }}, {{ {EX}, {EY} }} }}
local FAR    = {{ {FX}, {FY} }}
local NEAR_ARMOUR = {NEAR_ARMOUR}
local FAR_ARMOUR  = {FAR_ARMOUR}
local ENEMY  = {SLOT}
local PERIOD = {PERIOD}
local SPAWN  = {{ {SPX}, {SPY} }}
local GRASS  = 7

local flip = 0
local pond_filled = false

local function is_near(b)
  for _, n in ipairs(NEAR) do
    if b.x == n[1] and b.y == n[2] then return true end
  end
  return false
end

-- Re-assert stock on every base.  `flip` alternates the mines value so the
-- write is always a CHANGE and therefore always emits EVENT_BASE_STOCK.
local function assert_stock(g)
  flip = 1 - flip
  local mines = 90 - flip
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      local target = is_near(b) and NEAR_ARMOUR or FAR_ARMOUR
      -- Clamp DOWN only: undo the +1-per-1000-ticks regen, never heal a base
      -- the bot has already shot.
      local arm = b.armour
      if arm > target then arm = target end
      g.set_base_stock(i, arm, 0, mines)
    end
  end
end

function on_setup(g)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      g.set_base_owner(i, ENEMY)
      local target = is_near(b) and NEAR_ARMOUR or FAR_ARMOUR
      g.set_base_stock(i, target, 0, 90)
      g.message(string.format("BASE_STEAL_ARENA base#%d (%d,%d) owner=%d armour=%d",
                              i, b.x, b.y, ENEMY, target))
    end
  end
end

function on_tick(g, tick)
  if tick > 0 and tick % PERIOD == 0 then assert_stock(g) end
  -- Fill the spawn pond as soon as the tank is clear of it (see note 3).
  if not pond_filled and tick > 100 then
    local t = g.tank(0)
    if t and not (t.mx == SPAWN[1] and t.my == SPAWN[2]) and not t.boat then
      pond_filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message("BASE_STEAL_ARENA spawn pond filled at tick " .. tostring(tick))
    end
  end
end
'''


def write_sidecar(path, variant):
    text = SIDECAR.format(
        V=variant, SLOT=ENEMY_SLOT, NEAR_ARMOUR=NEAR_ARMOUR,
        FAR_ARMOUR=BASE_FULL_ARMOUR, PERIOD=STOCK_PERIOD,
        SPX=SPAWN[0], SPY=SPAWN[1],
        WX=WEST_BASE[0], WY=WEST_BASE[1],
        EX=EAST_BASE[0], EY=EAST_BASE[1],
        FX=FAR_BASE[0], FY=FAR_BASE[1])
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def main():
    args = list(sys.argv[1:])
    variant = "A"
    if args and args[0].upper() in ("A", "B"):
        variant = args.pop(0).upper()
    output = args[0] if args else str(
        Path(__file__).parent / f"base_steal_{variant}.map")
    terrain = make_map(variant)

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    assert terrain[SPAWN[1]][SPAWN[0]] is DEEP_SEA, (
        "the start square must be deep sea (starts.c startsIsValidSquare)")

    # The near bases have to be inside the steal's reach, the far one outside.
    for nb in near_bases():
        assert mdist(SPAWN, nb) <= STEAL_RANGE, (
            f"{nb} is {mdist(SPAWN, nb)} tiles from the spawn, past "
            f"BASE_STEAL_RANGE ({STEAL_RANGE})")
    assert mdist(SPAWN, FAR_BASE) > STEAL_RANGE, (
        f"the far base must be OUTSIDE BASE_STEAL_RANGE ({STEAL_RANGE}); it is "
        f"{mdist(SPAWN, FAR_BASE)} tiles away")
    # ...and the near ones one shell from falling, the far one untouched.
    assert shells_to_kill(NEAR_ARMOUR) == 1, (
        f"armour {NEAR_ARMOUR} is {shells_to_kill(NEAR_ARMOUR)} shells from "
        "capturable, not 1")
    assert NEAR_ARMOUR <= STEAL_MAX_ARMOUR, (
        f"armour {NEAR_ARMOUR} > BASE_STEAL_MAX_ARMOUR ({STEAL_MAX_ARMOUR})")
    assert NEAR_ARMOUR > MIN_ARMOUR_CAPTURE, (
        f"armour {NEAR_ARMOUR} is already capturable -- the base would land in "
        "capture_base (pool 3), not attack_base (pool 7), and there would be "
        "nothing for the steal to do")
    assert shells_to_kill(BASE_FULL_ARMOUR) == FULL_SHELLS_TO_KILL

    for g in guards(variant):
        cov = covered_base(variant)
        # The guard must reach the base it is meant to lock out...
        assert edist(g, cov) <= PILLBOX_RANGE, (
            f"guard {g} is {edist(g, cov):.1f} tiles from {cov}, past "
            f"PILLBOX_RANGE ({PILLBOX_RANGE})")
        # ...and at least one tile of the straight approach to it.
        line = approach_tiles(SPAWN, cov)
        reach = [t for t in line if edist(g, t) <= PILLBOX_RANGE]
        assert reach, f"guard {g} covers no tile of the approach {line}"
        # ...but NOT the base we still expect to be stolen, nor the spawn.
        for keep in steal_bases(variant) + [SPAWN]:
            assert edist(g, keep) > PILLBOX_RANGE + 1, (
                f"guard {g} is {edist(g, keep):.1f} tiles from {keep} -- it "
                "would lock out the base this variant still expects to be "
                "stolen (or shoot the tank on its spawn)")
        assert terrain[g[1]][g[0]] is not DEEP_SEA, "guard tile needs land"

    # Pill records: x, y, owner, armour, speed.  armour 15 = alive,
    # speed 100 = PILLBOX_ATTACK_NORMAL.
    pills = [(g[0], g[1], NEUTRAL, 15, 100) for g in guards(variant)]
    # Base records: x, y, owner, armour, shells, mines.  NEUTRAL here on
    # purpose -- the sidecar turns them hostile after the tank is placed.
    bases = [(b[0], b[1], NEUTRAL, BASE_FULL_ARMOUR, 0, 90)
             for b in (WEST_BASE, EAST_BASE, FAR_BASE)]
    starts = [(SPAWN[0], SPAWN[1], 2)]   # dir 2 = east

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

    sidecar = str(Path(output).with_suffix('')) + ".scenario.lua"
    write_sidecar(sidecar, variant)

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  and {sidecar}")
    print(f"  variant {variant}: near bases {near_bases()} at armour "
          f"{NEAR_ARMOUR} ({shells_to_kill(NEAR_ARMOUR)} shell from capturable, "
          f"markup {markup(NEAR_ARMOUR):.1f}); far base {FAR_BASE} at "
          f"{BASE_FULL_ARMOUR} ({FULL_SHELLS_TO_KILL} shells, markup "
          f"{markup(BASE_FULL_ARMOUR):.1f})")
    print(f"  expect steal at {steal_bases(variant)}; "
          f"guard {guards(variant) or 'NONE'}"
          + (f" locks out {covered_base(variant)}" if guards(variant) else ""))


if __name__ == '__main__':
    main()
