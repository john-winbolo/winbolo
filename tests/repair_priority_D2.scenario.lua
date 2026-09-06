-- Scenario sidecar for tests/repair_priority_D2.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/repair_priority_test.py arena D2.
--
-- Arena D's ground, run with cfg=BUILDER_POOL_REPAIR_LINEAR=false: the control.
-- The old formula still asks danger.lgm_path_safe_enhanced, so the same corpse
-- must be refused `path_unsafe` -- which is what makes D's silence evidence
-- about the rule and not about an arena that was never dangerous.
--
-- FILLING THE SPAWN POND.  A start square has to be DEEP SEA at map load
-- (starts.c startsIsValidSquare), which leaves a one-tile hole in the corridor
-- -- and the LGM walk sim (brainPathfinderLgmTravelTicks) walks a STRAIGHT LINE
-- with a crude slide, so a hole anywhere on the line makes the target read
-- `unreachable`.  That is not a subtlety: it decided two runs of these arenas
-- before it was found.  Arena C's corpse was unreachable for its first ~140
-- ticks -- the pond sat between the tank and the pill -- and the farm row won by
-- default while the only competitor had no score at all.  So the pond is filled
-- back to GRASS once the tank is ashore, which is what the ground would have
-- been if the engine did not need a puddle to spawn onto.
--
-- TRACE.  Every armour CHANGE on the pills the arena is about is written to
-- repair_priority_D2_trace.log in SIM ticks, so "the repair actually landed" is
-- asked of the ENGINE and not of the brain's opinion of itself.

local TRACE = "repair_priority_D2_trace.log"
local OURS  = { { 119, 126 } }
local NEUTRALS = { { 116, 126 }, { 115, 126 }, { 115, 125 }, { 115, 127 } }
local SPAWN = { 126, 126 }
local GRASS = 7
local FILL_TICK = 60             -- engine ticks: the tank is ashore well before
local p0 = 0

local last = {}
local filled = false

local function same(p, t) return p.x == t[1] and p.y == t[2] end

local function own_everything(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      local is_neutral = false
      for _, n in ipairs(NEUTRALS) do
        if same(p, n) then is_neutral = true end
      end
      if is_neutral then
        g.set_pill_owner(i, g.NEUTRAL)
      else
        g.set_pill_owner(i, p0)
      end
    end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      g.set_base_owner(i, p0)
      g.set_base_stock(i, 90, 90, 90)
    end
  end
end

function on_setup(g)
  own_everything(g)
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# tick x y armour\n") f:close() end
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_tick(g, tick)
  -- ...but only once the tank is ASHORE and off its boat. Filling the tile
  -- while the tank is still sitting on it leaves the boat state stuck and the
  -- LGM never becomes available -- the pool then reads `no_man` for the whole
  -- run, which is exactly what arena A did on the first attempt.
  if not filled and tick >= FILL_TICK then
    local tk = g.tank(p0)
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message(string.format(
        "REPAIR_PRIORITY_D2 filled the spawn pond at (%d,%d) with grass at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      for _, t in ipairs(OURS) do
        if same(p, t) then
          local key = p.x * 256 + p.y
          if last[key] ~= p.armour then
            local f = io.open(TRACE, "a")
            if f then
              f:write(string.format("%d %d %d %d\n", tick, p.x, p.y, p.armour))
              f:close()
            end
            last[key] = p.armour
          end
        end
      end
    end
  end
end
