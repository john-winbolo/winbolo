-- Scenario script for tests/ally_capture_guard_C.map — THE 7-SECOND EXPIRY.
--
-- Companion to tests/ally_capture_guard_test.py variant C.
--
-- Same ground as A and B, but the ally is the SCRIPTED brain
-- tests/brains/advert_capture.lua, which never moves and does nothing but put
-- `/info state goal=capture_pill mx=126 my=126` on the internal channel once a
-- second.  (No target id: that is the coordinate half of the guard's matching
-- rule, the half variant A does not exercise.)
--
-- At REMOVE_AT the script takes that bot out of the game with game.remove_bot,
-- so the adverts simply STOP.  Nothing tells our bot the ally is gone — the
-- ally_state slot keeps the last slate it received, and no GoalHunter module
-- ever deactivates a slot — so the ONLY thing that can end the block is the
-- silence expiry: BUILDER_POOL_ALLY_CAPTURE_TTL (350 brain ticks = 7 s at 50
-- ticks/s) after the last message.

local CORPSE    = 1                 -- the map's only pillbox
local OUR_SLOT  = 0
local ALLY_SLOT = 1
local GOALHUNTER = "../brains/GoalHunter_1.7/init.lua"    -- from the build dir
local TALKER     = "../tests/brains/advert_capture.lua"

-- The driver's OUR_CFG, word for word.
local OUR_CFG = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=PILL_REPOSITION_ENABLED=false"

-- The scripted talker: advertise capture_pill on the corpse TILE (no target
-- id) once a second, forever, and never move.  `switch=0` means it never
-- changes its mind — the only thing that can end the block here is silence.
-- Handed over through game.init_tokens rather than as a plain table of pairs:
-- the flatten both this brain and GoalHunter run DROPS a value of "0", so
-- `switch = "0"` written as a table key would vanish, where a token inside one
-- packed value survives.
local TALKER_INIT = "mx=126;my=126;switch=0;every=50"

-- Team 1, not the 0 this arena was ported with: team 0 is NO team on this host,
-- so two bots on it are not allies and the internal channel — the only channel
-- the advert rides — delivers nothing.
local TEAM = 1

-- Engine tick at which the talking ally is removed from the game.  Chosen well
-- after the block is established and with room afterwards for the whole TTL
-- plus the rebuild.  The hook clock on this host runs at 2 per brain think, and
-- 1200 was already an ENGINE tick in the ported file, so it stands unchanged.
local REMOVE_AT = 1200

-- The TTL in this file's own clock: 350 brain ticks, 2 engine ticks each.
local TTL_ENGINE = 700

-- How long this bot takes to rebuild the corpse when nothing is holding it
-- back, measured on this arena with a mute talker (2026-09-15, seed 42).  It is
-- the slack on the far side of the expiry: the man still has to be dispatched
-- and walk once the block lifts.
local UNBLOCKED_REBUILD = 614

local removed     = false
local rose_at     = nil   -- tick the corpse's armour first went up (the rebuild)
local scoop_at    = nil   -- tick it first went in_tank
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
  -- hands out no init: the measured bot cannot be priced off the corpse any
  -- other way.  The prelude defers these to on_start.
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

  if not removed and tick >= REMOVE_AT then
    removed = true
    g.remove_bot(ALLY_SLOT)
    g.log("ally_capture_guard_C: talker removed at tick " .. tostring(tick))
  end

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
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/ally_capture_guard_test.py _check_expiry for
-- variant C, but only the timing half that the engine can answer: the corpse
-- was still a corpse while the talker was talking, and it was rebuilt after the
-- talker fell silent — no sooner than the TTL, which is what tells the silence
-- expiry apart from the block never having held.
--
-- LEFT BEHIND: assertions 1, 2 and 3 as the driver wrote them — the
-- BP_ALLY_CAPTURE BLOCKED line, the RELEASED line and its `silent=` value, and
-- the BP_DISPATCH that follows it.  Those are print2 lines about the brain's
-- own reasoning, and `silent=` in particular has no engine-side equivalent: the
-- rebuild's timing is the closest this can get.
--
-- The window is the TTL at the bottom and the TTL plus one whole unblocked
-- rebuild at the top.  The mute-talker measurement is what sets that top: with
-- no advert at all this bot rebuilds the corpse at t=614 from a standing start
-- (2026-09-15, seed 42), so a rebuild more than 614 ticks past the expiry means
-- something other than the expiry released the man.  It is also the number to
-- look at first in a failure: a result near 614 means the advert is not
-- arriving, not that the rule broke.
--
-- GATE: ticks=2800 bots=0 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function(g)
  if scoop_at and (rose_at == nil or scoop_at < rose_at) then
    return false, string.format("the corpse went in_tank at t=%d, so nothing "
                                .. "after that measures the block", scoop_at)
  end
  if rose_at == nil then
    return false, "the corpse was never rebuilt, so the block never lifted"
  end
  if rose_at < REMOVE_AT then
    return false, string.format("rebuilt at t=%d, while the ally was still "
                                .. "advertising", rose_at)
  end
  if rose_at < REMOVE_AT + TTL_ENGINE then
    return false, string.format("rebuilt at t=%d, only %dt after the talker "
                                .. "went quiet", rose_at, rose_at - REMOVE_AT)
  end
  if rose_at > REMOVE_AT + TTL_ENGINE + UNBLOCKED_REBUILD then
    return false, string.format("rebuilt at t=%d, far past the expiry — "
                                .. "something else held the man", rose_at)
  end
  return true, string.format("block held to t=%d, %dt of silence past the "
                             .. "talker's removal", rose_at, rose_at - REMOVE_AT)
end
