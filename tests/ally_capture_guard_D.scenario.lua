-- Scenario sidecar for tests/ally_capture_guard_D.map — THE MOVE-ON.
--
-- Companion to tests/ally_capture_guard_test.py variant D.
--
-- Same ground and the same scripted ally as variant C, except that it is never
-- removed: at think SWITCH_AT it simply starts advertising a DIFFERENT goal
-- (`goal=explore`) and keeps talking on the same one-second cadence.
--
-- What must happen: the block ends on the tick that new slate lands, not
-- 350 ticks later.  The ally is as fresh as it ever was — `silent=` on the
-- RELEASED line is ~0 — so the only thing that can have lifted the block is the
-- rule that the LATEST advert no longer names the pill.  That is the half of
-- the author's rule that the TTL cannot express: "make it clear that block if
-- they move on to a different goal".

local PILL_CORPSE = 1
local ALLY_BRAIN  = "../tests/brains/advert_capture.lua"
local TRACE       = "ally_capture_guard_D_trace.log"

-- The scripted talker again, but this one CHANGES ITS MIND: for its first 600
-- thinks it advertises capture_pill on the corpse tile, and from think 600 on it
-- advertises `goal=explore` instead — on the same one-second cadence, so the
-- slot never goes stale and only the move-on rule can lift the block.
local ALLY_INIT = "mx=126;my=126;switch=600;every=50"

local p0 = 0
local ally = nil
local spawn_tried = false
local last = nil

-- Owners only.  Armour is set ONCE, at setup: re-asserting it per tick would
-- quietly undo the very rebuild variant B has to observe.
local function own_everything(g)
  for i = 1, g.num_pills() do
    g.set_pill_owner(i, p0)
  end
  for i = 1, g.num_bases() do
    g.set_base_owner(i, p0)
    g.set_base_stock(i, 90, 90, 90)
  end
end

function on_setup(g)
  own_everything(g)
  -- Say the armour again even though the map header carries it, so the arena
  -- cannot silently start with a live "corpse" if a future map loader ever
  -- normalises a 0 in the header.
  g.set_pill_armour(PILL_CORPSE, 0)
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# tick armour in_tank owner\n") f:close() end
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  if ally and p == ally then return 2 end
  return nil
end

local function trace(g, tick)
  local p = g.pill(PILL_CORPSE)
  if not p then return end
  local key = string.format("%d %d %s", p.armour, p.in_tank and 1 or 0,
                            tostring(p.owner))
  if key ~= last then
    last = key
    local f = io.open(TRACE, "a")
    if f then f:write(string.format("%d %s\n", tick, key)) f:close() end
  end
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local s, err = g.spawn_bot("Ally", ALLY_BRAIN, 0, nil, ALLY_INIT)
    if s == nil then
      g.message("ALLY_CAPTURE_D spawn_bot failed: " .. tostring(err))
    else
      ally = s
      g.set_team(s, 0)
      g.message("ALLY_CAPTURE_D ally slot=" .. tostring(s) .. " team=0")
      -- Into the trace as well as the newswire: a brain's own print() does not
      -- reach the headless server's stdout, so the trace file is the only
      -- channel the test can read to confirm the ally exists at all.
      local f = io.open(TRACE, "a")
      if f then
        f:write(string.format("# ally slot=%d tick=%d brain=%s\n",
                              s, tick, ALLY_BRAIN))
        f:close()
      end
      -- The pills stay slot 0's, so the corpse reads "friendly" to us and
      -- "allied" to the ally. Both are legal rebuild candidates (the engine's
      -- pillsRepairPos has no ownership test) and both are legal PICKUPS, which
      -- is exactly the collision the guard has to resolve.
      own_everything(g)
    end
  end
  trace(g, tick)
end
