-- Scenario sidecar for tests/builder_pool_D.map.  Companion to
-- tests/builder_pool_test.py variant D -- the RESERVATION test.
--
-- Same shape as variant A (a scripted idler owns the pill we take, so
-- attack_pill has a "hostile" target to bid on), but the geometry is the
-- experiment: our worn pill sits near the FAR standoff rather than near our
-- spawn.  While the tank is still crossing the map the round trip to it does
-- not fit inside b.reserve_eta -- the walls are due when the tank arrives, and
-- arriving is soon relative to a 25-tile walk -- so the row must read
-- `reserve(<eta> < trip <n>)`.  Once the tank is at the standoff the trip is
-- short and the reservation is satisfied, and it must dispatch.

local OUR_PILL  = { 127, 124 }
local FOE_PILL  = { 140, 126 }
local BASES     = { { 110, 130 }, { 124, 119 } }
local FOE_BRAIN = "../tests/brains/idle.lua"
local TRACE     = "builder_pool_D_hp.log"

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
      g.message("BUILDER_POOL_D spawn_bot failed: " .. tostring(err))
    else
      foe_player = s
      g.set_team(s, 1)
      g.message("BUILDER_POOL_D idler slot=" .. tostring(s) .. " team=1")
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
