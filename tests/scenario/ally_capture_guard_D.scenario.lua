-- Scenario script for tests/ally_capture_guard_D.map — THE MOVE-ON.
--
-- Companion to tests/ally_capture_guard_test.py variant D.
--
-- Same ground and the same scripted ally as variant C, except that it is never
-- removed: at think SWITCH_AT it simply starts advertising a DIFFERENT goal
-- (`goal=explore`) and keeps talking on the same one-second cadence.
--
-- What must happen: the block ends on the tick that new slate lands, not a TTL
-- later.  The ally is as fresh as it ever was, so the only thing that can have
-- lifted the block is the rule that the LATEST advert no longer names the pill.
-- That is the half of the author's rule the TTL cannot express: "make it clear
-- that block if they move on to a different goal".

local CORPSE    = 1                 -- the map's only pillbox
local OUR_SLOT  = 0
local ALLY_SLOT = 1
local GOALHUNTER = "../brains/GoalHunter_1.7/init.lua"    -- from the build dir
local TALKER     = "../tests/brains/advert_capture.lua"

-- The driver's OUR_CFG, word for word.
local OUR_CFG = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=PILL_REPOSITION_ENABLED=false"

-- The scripted talker again, but this one CHANGES ITS MIND: for its first 600
-- thinks it advertises capture_pill on the corpse tile, and from think 600 on
-- it advertises `goal=explore` instead — on the same one-second cadence, so the
-- slot never goes stale and only the move-on rule can lift the block.
-- `switch` counts the talker's own thinks, which the hook clock's doubling does
-- not touch, so it stands unchanged.
-- Handed over through game.init_tokens rather than as a plain table of pairs:
-- the flatten both this brain and GoalHunter run DROPS a value of "0", so
-- `switch = "0"` written as a table key would vanish, where a token inside one
-- packed value survives.
local SWITCH_THINK = 600
local TALKER_INIT  = "mx=126;my=126;switch=600;every=50"

-- The same moment on the hook clock, which runs at 2 ticks per brain think.
local SWITCH_AT = SWITCH_THINK * 2

-- BUILDER_POOL_ALLY_CAPTURE_TTL, 350 brain ticks, on the same clock.  Nothing
-- here waits for it — it is the upper bound that tells a move-on release apart
-- from a silence one.
local TTL_ENGINE = 700

-- Team 1, not the 0 this arena was ported with: team 0 is NO team on this host,
-- so two bots on it are not allies and the internal channel — the only channel
-- the advert rides — delivers nothing.
local TEAM = 1

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
-- variant D, but in the engine's own terms: the corpse survived every tick the
-- talker was advertising the capture, and was rebuilt after the talker moved
-- on.  The driver's distinguishing number is `silent=` on the RELEASE line,
-- which is ~0 for a move-on and ~TTL for an expiry.  The engine cannot see that
-- line, but it does not have to: the talker NEVER STOPS TALKING in this arena,
-- so there is no silence for the TTL to count and the move-on is the only rule
-- that can lift the block at all.  The window below says the same thing as a
-- number — a release a whole TTL after the switch would mean the clock, not the
-- latest advert, did the work.
--
-- LEFT BEHIND: assertions 1, 2 and 3 — the BLOCKED line, the RELEASED line with
-- its `silent=` value, and the BP_DISPATCH that follows it.  All print2.
--
-- The mute-talker measurement, for anyone reading a failure here: with no
-- advert at all this bot rebuilds the corpse at t=614 (2026-09-15, seed 42).  A
-- number near 614 means the advert is not arriving, not that the rule broke.
--
-- GATE: ticks=2600 bots=0 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function(g)
  if scoop_at and (rose_at == nil or scoop_at < rose_at) then
    return false, string.format("the corpse went in_tank at t=%d, so nothing "
                                .. "after that measures the block", scoop_at)
  end
  if rose_at == nil then
    return false, "the corpse was never rebuilt, so the block never lifted"
  end
  if rose_at < SWITCH_AT then
    return false, string.format("rebuilt at t=%d, while the ally was still "
                                .. "advertising the capture", rose_at)
  end
  if rose_at > SWITCH_AT + TTL_ENGINE then
    return false, string.format("rebuilt at t=%d, a whole TTL after the "
                                .. "move-on — the clock lifted it, not the "
                                .. "advert", rose_at)
  end
  return true, string.format("block held to the talker's move-on, rebuilt at "
                             .. "t=%d (%dt after it)", rose_at,
                             rose_at - SWITCH_AT)
end
