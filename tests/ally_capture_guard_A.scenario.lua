-- Scenario sidecar for tests/ally_capture_guard_A.map — THE BLOCK.
--
-- Companion to tests/ally_capture_guard_test.py variant A.
--
-- One dead friendly pill at (126,126).  Our bot (slot 0) is parked three tiles
-- west of it with a full woodpile and the LGM aboard, and is priced out of
-- taking the corpse itself (see the cfg= tokens in the test).  A SECOND real
-- GoalHunter 1.7 spawns six tiles east with nothing else on the map to do, so
-- it goes for the corpse and advertises `goal=capture_pill target=<pill id>` on
-- the internal channel — which is the ID half of the guard's matching rule
-- (variants C and D cover the mx/my half).
--
-- What must happen: our builder pool sees the corpse as a `rebuild` row, reads
-- the ally's advert, and refuses — because four trees would make the pill a
-- live friendly one, which cannot be driven over, so the ally's trip and the
-- kill that produced the corpse would both be thrown away.
--
-- The trace this writes is the ENGINE's answer, not the brain's: tick, armour,
-- in_tank, owner for the corpse.  Armour rising means somebody rebuilt it (a
-- failure here); in_tank going 1 means somebody scooped it (the pass).

local PILL_CORPSE = 1
local ALLY_BRAIN  = "../brains/GoalHunter_1.7/init.lua"
local TRACE       = "ally_capture_guard_A_trace.log"

-- The ally runs stock 1.7: the guard under test lives in OUR bot, and an ally
-- with default constants is the realistic thing to be blocked by.
local ALLY_INIT = nil

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
      g.message("ALLY_CAPTURE_A spawn_bot failed: " .. tostring(err))
    else
      ally = s
      g.set_team(s, 0)
      g.message("ALLY_CAPTURE_A ally slot=" .. tostring(s) .. " team=0")
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
