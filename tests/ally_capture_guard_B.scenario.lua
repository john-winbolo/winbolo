-- Scenario sidecar for tests/ally_capture_guard_B.map — THE CONTROL.
--
-- Companion to tests/ally_capture_guard_test.py variant B.
--
-- BYTE-IDENTICAL ground to variant A: same corpse, same spawns, same bases.
-- Two things differ, and both are there to isolate the ONE variable under test:
-- our bot carries `cfg=BUILDER_POOL_ALLY_CAPTURE_GUARD=false`, and the ally is
-- the scripted talker rather than a real 1.7 (see the ALLY_INIT note below).
--
-- What must happen with the guard off: a live capture advert is sitting in our
-- ally_state slot the whole time and our LGM walks out and rebuilds the corpse
-- regardless — the engine-side armour trace rises off 0.  That is the behaviour
-- this fix removes, and having it here is what stops variant A passing for some
-- unrelated reason (the pool never having a row at all, the man never being
-- spendable, the corpse never being reachable).

local PILL_CORPSE = 1
local ALLY_BRAIN  = "../tests/brains/advert_capture.lua"
local TRACE       = "ally_capture_guard_B_trace.log"

-- The SCRIPTED talker (tests/brains/advert_capture.lua), not variant A's real
-- 1.7 ally, and on purpose: it advertises `goal=capture_pill mx=126 my=126`
-- once a second forever and NEVER MOVES.  A control must isolate one variable,
-- and with a driving ally this arena is a footrace between its six tiles and
-- our man's three -- which is a coin flip that says nothing about the guard.
-- With the talker the advert is live the entire run, so "our man rebuilt it
-- anyway" can only mean the guard was off.
local ALLY_INIT = "mx=126;my=126;switch=0;every=50"

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
      g.message("ALLY_CAPTURE_B spawn_bot failed: " .. tostring(err))
    else
      ally = s
      g.set_team(s, 0)
      g.message("ALLY_CAPTURE_B ally slot=" .. tostring(s) .. " team=0")
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
