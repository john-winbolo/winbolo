-- Scenario sidecar for tests/ally_capture_guard_C.map — THE 7-SECOND EXPIRY.
--
-- Companion to tests/ally_capture_guard_test.py variant C.
--
-- Same ground as A and B, but the ally is the SCRIPTED brain
-- tests/brains/advert_capture.lua, which never moves and does nothing but put
-- `/info state goal=capture_pill mx=126 my=126` on the internal channel once a
-- second.  (No target id: that is the coordinate half of the guard's matching
-- rule, the half variant A does not exercise.)
--
-- At REMOVE_AT the sidecar takes that bot out of the game with game.remove_bot,
-- so the adverts simply STOP.  Nothing tells our bot the ally is gone — the
-- ally_state slot keeps the last slate it received, and no GoalHunter module
-- ever deactivates a slot — so the ONLY thing that can end the block is the
-- silence expiry: BUILDER_POOL_ALLY_CAPTURE_TTL (350 brain ticks = 7 s at
-- 50 ticks/s) after the last message.
--
-- What must happen: BP_ALLY_CAPTURE ... BLOCKED, then a long quiet stretch, then
-- BP_ALLY_CAPTURE ... RELEASED with `silent=` at or just past the TTL, and only
-- then the rebuild.  The release line prints both `silent=` and the ex-blocker's
-- current goal, so the test can tell this case apart from variant D's.

local PILL_CORPSE = 1
local ALLY_BRAIN  = "../tests/brains/advert_capture.lua"
local TRACE       = "ally_capture_guard_C_trace.log"

-- The scripted talker: advertise capture_pill on the corpse TILE (no target
-- id) once a second, forever, and never move. `switch=0` means it never
-- changes its mind — the only thing that can end the block here is silence.
local ALLY_INIT = "mx=126;my=126;switch=0;every=50"

-- Engine tick at which the talking ally is removed from the game. Chosen well
-- after the block is established (our pool starts scoring the corpse within the
-- first ~50 brain ticks) and with room afterwards for the whole 350-tick TTL
-- plus the rebuild. Engine ticks run at 2 per brain think.
local REMOVE_AT = 1200
local removed = false
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
      g.message("ALLY_CAPTURE_C spawn_bot failed: " .. tostring(err))
    else
      ally = s
      g.set_team(s, 0)
      g.message("ALLY_CAPTURE_C ally slot=" .. tostring(s) .. " team=0")
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
  if not removed and ally and tick >= REMOVE_AT then
    removed = true
    g.remove_bot(ally)
    g.message("ALLY_CAPTURE_C removed ally slot=" .. tostring(ally)
              .. " at tick=" .. tostring(tick))
  end
  trace(g, tick)
end
