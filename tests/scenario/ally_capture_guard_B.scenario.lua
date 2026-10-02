-- Scenario script for tests/ally_capture_guard_B.map — THE CONTROL.
--
-- Companion to tests/ally_capture_guard_test.py variant B.
--
-- Same ground as variant A, and one token different: our bot carries
-- `cfg=BUILDER_POOL_ALLY_CAPTURE_GUARD=false`.  With the guard off our LGM walks
-- out and rebuilds the corpse even though a teammate is coming for it — the
-- engine-side armour trace rises off 0.  That is the behaviour the guard
-- removes, and having it here is what stops variant A passing for some
-- unrelated reason (the pool never having a row at all, the man never being
-- spendable, the corpse never being reachable).
--
-- The ally is the SCRIPTED talker, not variant A's real 1.7, and on purpose: it
-- advertises `goal=capture_pill mx=126 my=126` once a second forever and NEVER
-- MOVES.  A control must isolate one variable, and with a driving ally this
-- arena is a footrace between its six tiles and our man's seven — which was
-- measured on this host (2026-09-15, seed 42): the ally scoops the corpse at
-- t=414 and our man has not finished the rebuild, so a real-1.7 version of this
-- arena says nothing about the guard at all.  With the talker the advert is
-- live the entire run, so "our man rebuilt it anyway" can only mean the guard
-- was off.

local CORPSE    = 1                 -- the map's only pillbox
local OUR_SLOT  = 0
local ALLY_SLOT = 1
local GOALHUNTER = "../brains/GoalHunter/init.lua"    -- from the build dir
local TALKER     = "../tests/brains/advert_capture.lua"

-- The driver's OUR_CFG plus its GUARD_OFF, word for word.
local OUR_CFG = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=PILL_REPOSITION_ENABLED=false;"
                .. "cfg=BUILDER_POOL_ALLY_CAPTURE_GUARD=false"

-- What the talker needs to say anything at all.  Handed over through
-- game.init_tokens rather than as a plain table of pairs: the flatten both this
-- brain and GoalHunter run DROPS a value of "0", so `switch = "0"` written as a
-- table key would vanish, where a token inside one packed value survives.
local TALKER_INIT = "mx=126;my=126;switch=0;every=50"

-- Team 1, not the 0 this arena was ported with: team 0 is NO team on this host,
-- so two bots on it are not allies and the internal channel — the only channel
-- the advert rides — delivers nothing.
local TEAM = 1

local rose_at     = nil   -- tick the corpse's armour first went up (the rebuild)
local scoop_at    = nil   -- tick it first went in_tank
local man_at      = nil   -- tick our own LGM first stood on the corpse's square
local last_armour = nil

local function own_everything(g)
  for i = 1, g.num_pills() do
    g.set_pill_owner(i, OUR_SLOT)
  end
  for i = 1, g.num_bases() do
    g.set_base_owner(i, OUR_SLOT)
    g.set_base_stock(i, 90, 90, 90)
  end
end

function on_setup(g)
  own_everything(g)
  -- Say the armour again even though the map header carries it, so the arena
  -- cannot silently start with a live "corpse" if a future map loader ever
  -- normalises a 0 in the header.
  g.set_pill_armour(CORPSE, 0)
  -- Both bots are fielded by the arena rather than by the runner's -bots, which
  -- hands out no init: without one this bot would run with the guard ON and the
  -- control would measure nothing.  The prelude defers these to on_start.
  g.spawn_bot{ slot = OUR_SLOT, name = "Ours", brain = GOALHUNTER,
               team = TEAM, start = 1, init = g.init_tokens(OUR_CFG) }
  g.spawn_bot{ slot = ALLY_SLOT, name = "Ally", brain = TALKER,
               team = TEAM, start = 2, init = g.init_tokens(TALKER_INIT) }
end

-- Only for a respawn: the first lives come in on the starts named above.
function on_choose_start(g, p)
  if p == OUR_SLOT  then return 1 end
  if p == ALLY_SLOT then return 2 end
  return nil
end

function on_tick(g, tick)
  -- The pill and the bases are handed over again for the first few ticks: an
  -- owner written in on_setup names a seat the roster does not hold yet.
  if tick <= 20 then own_everything(g) end

  local p = g.pill(CORPSE)
  if p == nil then return end

  if p.in_tank then
    if scoop_at == nil then scoop_at = tick end
    return                      -- a carried pill has no armour worth watching
  end
  if last_armour ~= nil and p.armour > last_armour and rose_at == nil then
    rose_at = tick
  end
  last_armour = p.armour

  if man_at == nil then
    local b = g.builder(OUR_SLOT)
    if b and b.state ~= "in_tank" and b.state ~= "dead"
       and b.mx == p.x and b.my == p.y then
      man_at = tick
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/ally_capture_guard_test.py check_B: assertion
-- 2 (the man WAS sent to the corpse) and assertion 3 (the armour rose off 0, so
-- the rebuild landed), both of which the driver read out of this script's own
-- engine trace.
--
-- LEFT BEHIND: assertions 0, 1 and 1b — the pool ran at all, the bot logged the
-- `[cfg]` override, and a talker was on the map to be ignored.  All three are
-- print2 lines about the brain's own reasoning, which no scenario can see.
--
-- GATE: ticks=2200 bots=0 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function(g)
  if rose_at == nil then
    if scoop_at then
      return false, string.format("the ally scooped it at t=%d before our man "
                                  .. "could rebuild", scoop_at)
    end
    return false, "the corpse was never rebuilt with the guard off"
  end
  if man_at == nil then
    return false, string.format("armour rose at t=%d but our man was never "
                                .. "seen on the square", rose_at)
  end
  return true, string.format("guard off: our man reached the corpse at t=%d "
                             .. "and rebuilt it at t=%d", man_at, rose_at)
end
