#!/usr/bin/env python3
"""
Generate the tree-reserve arenas (companion to tests/reserve_seeded_test.py).

Field incident 20260903_105448 bot2 t=34774.  Four pills aboard, 21 trees in
the tank, three damaged friendly pills within arm's reach, and every one of
them refused: `BP_DENY ... reason=tree_reserve(41,21,1)`.  Reserve 41 was
base 4 + carried pills 16 + a retired sea plan 21.  Even with the sea plan
gone it would have read 20 against 21 trees and still refused a ONE-tree
repair, because the carried-pill component was PILL_PLACE_TREE_COST per pill,
uncapped.

Two variants, two things to prove.

  seeded    THE RESERVE MUST NOT STARVE THE TANK'S OWN JOBS.
            An empty grass field, our base, and a WORN friendly pill two tiles
            east of the spawn.  Nine healthy friendly pills sit in a block
            beside the base; at sim tick GIVE_TICK the sidecar loads all nine
            into the tank.

            Why NINE.  A map with a scenario sidecar boots as gameScripted,
            which gets the OPEN loadout -- 40 trees, and the scenario API has
            no lever to set a tank's wood.  So the arena moves the other side
            of the inequality instead: at nine carried pills the OLD rule
            reserved 4 + 9 x 4 = 40 trees against the 40 in the tank, leaving
            exactly 0 for a job that needs 3, and a seeded repair (the tank's
            own defend->repair handoff) was charged that same 40.  Nine is the
            smallest count that does it, and "enough carried pills freeze
            every job" is precisely what `uncapped` meant.  Under the new rule
            the carried component is ONE placement's worth whatever is aboard,
            and a seeded job is charged nothing at all.

  gather    THE SEEK-TREES GATHER STOPS AT ONE PLACEMENT'S WORTH.
            The same field with FOREST_TILES forest tiles as its entire wood
            supply, four healthy pills parked beside the base, and a tank
            spawned through
            game.spawn_bot in STRICT mode -- 0 shells, 0 mines, 0 TREES (the
            one seam that reaches a tank's loadout; a sidecar map ignores
            -gametype).  It is handed four pills, so with no wood at all
            goals.lua's seek-trees redirect fires and builder.set_mode puts the
            builder in `gather` with b.need_trees.

            The forest holds FOREST_TILES x LGM_GATHER_TREE = 8 trees, total,
            for the whole game.  The OLD need_trees was 4 x carried = 16, i.e.
            more wood than the map contains: the gather could never finish and
            the pill could never be dropped.  The NEW one is 4, which the first
            forest tile pays for.  b.need_trees is also the `goal` term of the
            reserve, so it is readable straight off the BUILDER_POOL line.

MAP FACTS THAT BITE (the same ones every arena in this suite has met)
  * a start square must be DEEP SEA (starts.c startsIsValidSquare), hence the
    one-tile spawn pond;
  * mapRead recenters a map whose terrain bounding box is off-centre, so the
    field is symmetric about (126,126) and main() asserts it;
  * mapRead puts ROAD under every map-file pillbox;
  * EVERY friendly pill needs a friendly base within PILL_FIRE_RANGE (8) or
    the reposition pool decides it is badly placed and bids to shoot it DOWN.
    `gather`'s four spares are parked beside its base for the same reason.

Usage:
    python3 tests/generate_reserve_seeded_map.py [--variant seeded|gather|ALL]
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

FOREST = 5
GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256

# ── Geometry (the test runner imports these) ─────────────────────────────
FIELD = (110, 142, 110, 142)        # x0, x1, y0, y1 -- midpoint (126,126)
SPAWN = (126, 126)                  # one-tile pond at the centre
OUR_BASE = (124, 124)               # full stock: a full tank never bids refuel
WORN_PILL = (128, 126)              # 2 east of the spawn, 6 from the base
WORN_HP = 6                         # 9 missing -> ceil(9/4) = 3 trees
# Nine healthy friendly pills in a block beside the base, every one of them
# inside PILL_FIRE_RANGE of it so the reposition pool leaves them alone.  They
# exist only to be handed to the tank at GIVE_TICK.
SPARE_PILLS = [(x, y) for y in (121, 122, 123) for x in (121, 122, 123)]
GIVE_TICK = 200                     # SIM ticks (brain tick 100)

# ── gather variant ───────────────────────────────────────────────────────
# The tank spawns with NO shells (strict), so it will want a base early; four
# tiles west of the spawn keeps that errand short and out of the experiment.
GATHER_BASE = (122, 126)
GATHER_SPARES = [(120, 124), (120, 125), (120, 127), (120, 128)]
FOREST_TILES = [(130, 126), (131, 126)]     # the map's ENTIRE wood supply
GATHER_CARRIED = len(GATHER_SPARES)

# ── Brain/engine constants these arenas are designed against ─────────────
PILLS_MAX_HEALTH = 15
PILL_PLACE_TREE_COST = 4            # constants.lua (LGM_COST_PILLNEW)
PILL_REPAIR_AMOUNT = 4              # constants.lua: armour per tree
LGM_GATHER_TREE = 4                 # trees a single forest tile yields
TREE_RESERVE = 4                    # constants.lua: the standing base float
TANK_FULL_TREES = 40                # gametype.h: the open/scripted loadout
PILL_FIRE_RANGE = 8                 # constants.lua
VARIANTS = ("seeded", "gather")


def topup_trees_needed():
    """builder_pool.discover's trees_need for WORN_PILL."""
    missing = PILLS_MAX_HEALTH - WORN_HP
    return max(1, -(-missing // PILL_REPAIR_AMOUNT))


def old_reserve(carried):
    """What tree_reserve charged an errand BEFORE 2026-09-03: the standing
    float plus PILL_PLACE_TREE_COST for every pill in the tank, uncapped."""
    return TREE_RESERVE + carried * PILL_PLACE_TREE_COST


def new_reserve(carried):
    """...and after: one placement's worth, whatever is aboard."""
    return TREE_RESERVE + (PILL_PLACE_TREE_COST if carried else 0)


def make_map(variant):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y0, y1 = FIELD
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            t[y][x] = GRASS
    if variant == "gather":
        for (x, y) in FOREST_TILES:
            t[y][x] = FOREST
    t[SPAWN[1]][SPAWN[0]] = DEEP_SEA          # the start pond
    return t


def pills_for(variant):
    """(x, y, owner, armour, speed).  All healthy except `seeded`'s worn one,
    so nothing here is ever a capture or attack candidate."""
    if variant == "gather":
        return [(x, y, 0, PILLS_MAX_HEALTH, 50) for (x, y) in GATHER_SPARES]
    out = [(WORN_PILL[0], WORN_PILL[1], 0, WORN_HP, 50)]
    out += [(x, y, 0, PILLS_MAX_HEALTH, 50) for (x, y) in SPARE_PILLS]
    return out


def bases_for(variant):
    b = GATHER_BASE if variant == "gather" else OUR_BASE
    return [(b[0], b[1], 0, 90, 90, 90)]


SIDECAR = r'''
-- Scenario sidecar for tests/reserve_{V}.map -- GENERATED by
-- tests/generate_reserve_seeded_map.py, do not edit by hand.
--
-- 1. LOADOUT.  A map with a sidecar boots as gameScripted, and
--    gameTypeGetItems treats gameScripted exactly like OPEN (40 shells / 40
--    mines / 40 trees) -- neither -gametype nor scenario.game reaches a -bots
--    tank.  game.spawn_bot's mode argument is the one thing that does
--    (scenario.c arms sim->spawnLoadout for the slot before tankCreate runs),
--    so the `gather` variant runs with -bots 0 and spawns its own tank in
--    STRICT mode to get a tank with no wood at all.  `seeded` wants the full
--    40 trees, so it just uses the -bots tank.
--
-- 2. Own everything, so no pill on the map is a capture or attack candidate.
--
-- 3. At tick {GIVE_TICK}, hand {NGIVE} pill(s) to the tank.  That is the whole
--    experiment: what the tank is CARRYING is what the old reserve priced
--    against every job it wanted to do.

local OUR_SLOT   = {SLOT}     -- -1 until spawn_bot answers, in `gather`
local SPAWN_MODE = {MODE}     -- nil = use the -bots tank as it spawned
local GIVE_IDS   = {GIVEIDS}  -- pill numbers to load into the tank
local N_GIVE     = {NGIVE}    -- how many to make up in `gather` (no map pills)
local GIVE_TICK  = {GIVE_TICK}
local slot = OUR_SLOT
local spawn_tried = false
local given = false

local function own_everything(g, owner)
  for i = 1, g.num_pills() do g.set_pill_owner(i, owner) end
  for i = 1, g.num_bases() do g.set_base_owner(i, owner) end
end

function on_setup(g)
  own_everything(g, OUR_SLOT >= 0 and OUR_SLOT or 0)
  g.message(string.format("RESERVE_ARENA pills=%d bases=%d give=%d@%d mode=%s",
                          g.num_pills(), g.num_bases(), N_GIVE, GIVE_TICK,
                          tostring(SPAWN_MODE)))
end

function on_tick(g, tick)
  if SPAWN_MODE and not spawn_tried and tick >= 2 then
    spawn_tried = true
    local s, err = g.spawn_bot("Reserve", nil, 0, SPAWN_MODE)
    if s == nil then
      g.message("RESERVE_ARENA spawn_bot failed: " .. tostring(err))
    else
      slot = s
      g.message("RESERVE_ARENA spawned slot=" .. tostring(s)
                .. " mode=" .. SPAWN_MODE)
      own_everything(g, s)
    end
  end
  if not given and slot >= 0 and tick >= GIVE_TICK then
    given = true
    local n = 0
    for _, i in ipairs(GIVE_IDS) do
      local ok, err = g.give_pill(slot, i)
      if ok then n = n + 1
      else g.message("RESERVE_ARENA give_pill#" .. i .. " failed: "
                     .. tostring(err)) end
    end
    g.message("RESERVE_ARENA gave " .. n .. " pill(s) to slot " .. slot
              .. " at tick " .. tick)
  end
end
'''


def write_sidecar(variant, path):
    pills = pills_for(variant)
    if variant == "seeded":
        give_ids = [i + 1 for i, p in enumerate(pills)
                    if (p[0], p[1]) in SPARE_PILLS]
        mode, slot = "nil", "0"
    else:
        # gather spawns its own tank (strict loadout), and every pill on its
        # map is one of the four it is handed.
        give_ids = [i + 1 for i, p in enumerate(pills)]
        mode, slot = '"strict"', "-1"
    text = (SIDECAR
            .replace("{V}", variant)
            .replace("{SLOT}", slot)
            .replace("{MODE}", mode)
            .replace("{GIVEIDS}", "{ " + ", ".join(str(i) for i in give_ids) + " }")
            .replace("{NGIVE}", str(len(give_ids)))
            .replace("{GIVE_TICK}", str(GIVE_TICK)))
    Path(path).write_text(text.lstrip("\n"), encoding="utf-8", newline="\n")


def build(variant, output):
    terrain = make_map(variant)
    pills = pills_for(variant)
    bases = bases_for(variant)
    starts = [(SPAWN[0], SPAWN[1], 4)]

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    assert terrain[SPAWN[1]][SPAWN[0]] is DEEP_SEA, (
        "the start must be deep sea (starts.c startsIsValidSquare)")

    if variant == "seeded":
        # Every friendly pill needs a base inside PILL_FIRE_RANGE or the
        # reposition pool bids to shoot it down instead of leaving it alone.
        for (x, y) in [WORN_PILL] + SPARE_PILLS:
            d = abs(x - OUR_BASE[0]) + abs(y - OUR_BASE[1])
            assert d <= PILL_FIRE_RANGE, (
                f"pill ({x},{y}) is {d} tiles from the base, outside "
                f"PILL_FIRE_RANGE {PILL_FIRE_RANGE}")
        # The arena only means something if the OLD reserve really did leave
        # the tank nothing and the NEW one leaves it plenty.
        carried = len(SPARE_PILLS)
        need = topup_trees_needed()
        assert TANK_FULL_TREES - old_reserve(carried) < need, (
            f"with {carried} pills aboard the old reserve was "
            f"{old_reserve(carried)} against {TANK_FULL_TREES} trees, which "
            f"still leaves {TANK_FULL_TREES - old_reserve(carried)} for a "
            f"{need}-tree job -- the arena would prove nothing")
        assert TANK_FULL_TREES - new_reserve(carried) >= need
        assert PILLS_MAX_HEALTH - WORN_HP >= PILL_REPAIR_AMOUNT, (
            "the worn pill is not damaged enough for the pool to offer a "
            "topup row at all (BUILDER_POOL_TOPUP_MIN_MISSING)")
    else:
        wood = len(FOREST_TILES) * LGM_GATHER_TREE
        assert wood < GATHER_CARRIED * PILL_PLACE_TREE_COST, (
            f"the map holds {wood} trees but the OLD need_trees was "
            f"{GATHER_CARRIED * PILL_PLACE_TREE_COST}; the forest has to be "
            f"too small to ever satisfy it, or the variant proves nothing")
        assert wood >= PILL_PLACE_TREE_COST, (
            "the forest has to be able to pay for ONE placement")
        for (x, y) in FOREST_TILES:
            assert terrain[y][x] == FOREST
        for (x, y) in GATHER_SPARES:
            d = abs(x - GATHER_BASE[0]) + abs(y - GATHER_BASE[1])
            assert d <= PILL_FIRE_RANGE, (
                f"pill ({x},{y}) is {d} tiles from the base, outside "
                f"PILL_FIRE_RANGE {PILL_FIRE_RANGE}")
        assert GATHER_BASE not in FOREST_TILES

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


def main_one(variant):
    here = Path(__file__).parent
    out = here / f"reserve_{variant}.map"
    build(variant, out)
    write_sidecar(variant, here / f"reserve_{variant}.scenario.lua")
    print(f"Wrote {out} ({out.stat().st_size} bytes) variant={variant}")
    if variant == "seeded":
        c = len(SPARE_PILLS)
        print(f"  worn pill {WORN_PILL} armour {WORN_HP}/{PILLS_MAX_HEALTH} "
              f"({topup_trees_needed()} tree(s) to fix), base {OUR_BASE}")
        print(f"  {c} spare pill(s) handed over at sim tick {GIVE_TICK}: old "
              f"reserve {old_reserve(c)} vs {TANK_FULL_TREES} trees leaves "
              f"{TANK_FULL_TREES - old_reserve(c)}; new reserve "
              f"{new_reserve(c)} leaves {TANK_FULL_TREES - new_reserve(c)}")
    else:
        print(f"  base {GATHER_BASE}, spares {GATHER_SPARES}")
        print(f"  forest {FOREST_TILES} = "
              f"{len(FOREST_TILES) * LGM_GATHER_TREE} trees for the whole "
              f"game; {GATHER_CARRIED} pill(s) handed over at sim tick "
              f"{GIVE_TICK}; old need_trees "
              f"{GATHER_CARRIED * PILL_PLACE_TREE_COST}, new "
              f"{PILL_PLACE_TREE_COST}")


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
