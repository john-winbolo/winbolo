-- Scenario sidecar for tests/builder_pool_A.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/builder_pool_test.py variant A --
-- the "stolen blocker" shape.
--
-- Three jobs.
--
-- 1. AN OPPONENT THAT OWNS THE TARGET.  attack_pill only bids on pills whose
--    owner reads "hostile", so the take needs a real enemy player.  A scripted
--    IDLER (tests/brains/idle.lua) in the far corner is the cheapest way to get
--    one: it never moves, never shoots and never comes near the experiment, so
--    the only thing it contributes is ownership of the pill at (134,126).
--
-- 2. OWNERSHIP.  Our worn pill at (124,121) and our base go to slot 0 (the
--    -bots tank); the target pill goes to the idler.  on_choose_start pins each
--    tank to its own pond so they cannot swap ends.
--
-- 3. TRACE.  Write our worn pill's armour to builder_pool_A_hp.log on every
--    CHANGE, in SIM ticks.  The test's "the repair actually landed" assertion
--    is checked against the ENGINE, not against the brain's account of itself.

local OUR_PILL  = { 124, 121 }
local FOE_PILL  = { 134, 126 }
local BASES     = { { 110, 126 }, { 121, 118 } }
local FOE_BRAIN = "../tests/brains/idle.lua"
local TRACE     = "builder_pool_A_hp.log"

local our_player  = 0
local foe_player  = nil
local spawn_tried = false
local last_hp     = nil

local function own_everything(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      if p.x == FOE_PILL[1] and p.y == FOE_PILL[2] then
        if foe_player then g.set_pill_owner(i, foe_player) end
      else
        g.set_pill_owner(i, our_player)
      end
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
  own_everything(g)
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
    local s, err = g.spawn_bot("Idler", FOE_BRAIN, 1, nil)
    if s == nil then
      g.message("BUILDER_POOL_A spawn_bot failed: " .. tostring(err))
    else
      foe_player = s
      g.set_team(s, 1)
      g.message("BUILDER_POOL_A idler slot=" .. tostring(s) .. " team=1")
      own_everything(g)
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
