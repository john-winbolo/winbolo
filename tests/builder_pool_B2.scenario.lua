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
local FOE_BRAIN = "../tests/brains/shell_tank_then_flee.lua"
local TRACE     = "builder_pool_B2_hp.log"

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
  local f = io.open(TRACE, "w")
  if f then f:write("# tick hp\n") f:close() end
end

function on_choose_start(g, p)
  if p == our_player then return 1 end
  if foe_player and p == foe_player then return 2 end
  return nil
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local s, err = g.spawn_bot("Shooter", FOE_BRAIN, 1, nil)
    if s == nil then
      g.message("BUILDER_POOL_B2 spawn_bot failed: " .. tostring(err))
    else
      foe_player = s
      g.set_team(s, 1)
      g.message("BUILDER_POOL_B2 shooter slot=" .. tostring(s) .. " team=1")
      own_ours(g)
    end
  end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      if last_hp ~= p.armour then
        local f = io.open(TRACE, "a")
        if f then f:write(string.format("%d %d\n", tick, p.armour)) f:close() end
        last_hp = p.armour
      end
      break
    end
  end
end
