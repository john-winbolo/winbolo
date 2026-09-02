-- Scenario sidecar for tests/builder_pool_B.map.  Companion to
-- tests/builder_pool_test.py variant B -- the repair_pill SPLIT.
--
-- The arena has no enemies at all, so there is nothing here but ownership and
-- a trace.  Both bases and the single worn pill go to slot 0; the pill sits 18
-- tiles from the spawn, more than twice BUILDER_POOL_LEASH, so repair_pill as
-- a TANK goal means what plan section 7 says it means -- "relocate until the
-- repair becomes leash-reachable" -- and the hand-off to the man is what the
-- test watches for.
--
-- The second base at (134,122) is not decoration: it sits 6 tiles from the
-- pill, inside PILL_FIRE_RANGE, so the pill has a job to do and the
-- reposition pool stops bidding capture_pill/reposition_shoot on it (i.e. the
-- bot shooting its own pill down to move it, which ends the experiment).

local OUR_PILL   = { 132, 126 }
local BASES      = { { 110, 126 }, { 134, 122 } }
local TRACE      = "builder_pool_B_hp.log"

local our_player = 0
local last_hp    = nil

local function own_ours(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then g.set_pill_owner(i, our_player) end
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
  return nil
end

function on_tick(g, tick)
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
