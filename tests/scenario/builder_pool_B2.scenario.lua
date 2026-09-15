-- Scenario sidecar for tests/builder_pool_B2.map.  Companion to
-- tests/builder_pool_test.py variant B2 -- "the same arena, with shells
-- landing on the TANK".
--
-- Identical to builder_pool_B.scenario.lua plus a scripted shooter
-- (tests/brains/shell_tank_then_flee.lua) that drives to a fixed standoff
-- inside our field and shells OUR TANK on a pace.  Aimed at the tank, not at
-- the pill, because B2 is about danger.tank_fire_age -- the builder pool's own
-- under-fire clock (armour dropped, or a hostile shell will connect with US) --
-- which is a different signal from the pill's last_hit_tick that the repair
-- hold uses.
--
-- The shooter shares OUR field rather than sitting across the moat: it has to
-- be within a tank shell's ~7.1-tile reach of wherever take_cover parks us, and
-- the moat is 12+ tiles from the cover tiles.

local OUR_PILL  = { 124, 126 }
local BASES     = { { 114, 120 }, { 126, 122 } }
-- NOT IN THIS REPOSITORY. tests/brains/ carries idle.lua and the other
-- scripted brains, but never this one, so the spawn below refuses with
-- SCN_OP_NOT_FOUND and no shell is ever fired at our tank.
local FOE_BRAIN = "../tests/brains/shell_tank_then_flee.lua"
local FOE_SLOT  = 1

local our_player  = 0
local foe_player  = nil
local spawn_tried = false
local last_hp     = nil

local function own_ours(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      g.set_pill_owner(i, our_player)
    end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      for _, bp in ipairs(BASES) do
        if b.x == bp[1] and b.y == bp[2] then
          g.set_base_owner(i, our_player)
          g.set_base_stock(i, 90, 90, 90)
        end
      end
    end
  end
end

function on_setup(g)
  own_ours(g)
  g.set_team(our_player, 0)
end

function on_choose_start(g, p)
  if p == our_player then return 1 end
  if foe_player and p == foe_player then return 2 end
  return nil
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    -- The host's own table shape, straight through the compat shim: naming the
    -- slot is what makes the call answer a seat number.
    local s, err = g.spawn_bot{ slot = FOE_SLOT, name = "Shooter",
                                brain = FOE_BRAIN, team = 1, start = 2 }
    if s == nil then
      g.message("BUILDER_POOL_B2 spawn_bot failed: " .. tostring(err))
    else
      foe_player = FOE_SLOT
      g.message("BUILDER_POOL_B2 shooter slot=" .. tostring(foe_player)
                .. " team=1")
    end
  end
  if foe_player and tick < 120 then own_ours(g) end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      last_hp = p.armour
      break
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- NOT PORTED, and this one cannot even be set up.
--
-- The arena is the scripted shooter: tests/brains/shell_tank_then_flee.lua
-- drives to a standoff and puts shells on OUR TANK on a pace, and it is the
-- only thing on the map that makes danger.tank_fire_age move. That file does
-- not exist in this repository, so spawn_bot refuses and the round is just a
-- bot repairing a pill in peace -- which is variant B, not B2.
--
-- The assertion is print2 in any case: the dispatch must be DENIED
-- `under_fire(<age>t)` on the ticks the shells are landing, and a denial
-- reason is a builder-pool log line. There is no world state that separates
-- "held back while under fire" from "went straight away": the pill comes back
-- up either way.
--
-- To repay this: bring tests/brains/shell_tank_then_flee.lua across, and then
-- a host op that exposes a bot's builder-pool verdict -- see PORT_MAP.md.
--
-- GATE: skip=tests/brains/shell_tank_then_flee.lua is not in this repo, and the driver asserts on the under_fire denial in print2
