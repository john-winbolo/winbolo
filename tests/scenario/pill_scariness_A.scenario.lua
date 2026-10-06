-- Scenario script for tests/pill_scariness_A.map (auto-loaded as
-- <map>.scenario.lua).  Variant A: NEUTRAL-pillbox strays on a healthy pill.
--
-- GATE: ticks=6200 bots=0 ai=yesfull gametype=open limit=20
--
-- MEASURED 2026-09-15 on main's GoalHunter, at the driver's own seed,
-- budget and pin.  A5 holds: the bot does go and fetch the free body rather
-- than park on watch.  A4 does not: our pill still ends the run at 3 of 15,
-- so leaving is no longer enough to keep it healthy on this ground.  The pin
-- is landing -- the same arena with the cfg token dropped finishes at 6 of 15
-- instead of 3 -- so this is the bot's behaviour and not a lost knob.  What
-- it would take to repay: find out whether the bot is coming back into the
-- neutral's range after the fetch, which needs the goal trace the driver had
-- and a scenario has not.
-- GATE: expect=fail A5 holds (the free body is collected) but A4 does not: our pill ends at 3 of 15, under the healthy line of 10
--
-- Three jobs.
--
-- 1. LOADOUT.  A map with a script boots as gameScripted, and
--    gameTypeGetItems treats gameScripted exactly like OPEN (40 shells /
--    40 mines / 40 trees) -- neither -gametype nor scenario.game reaches a
--    -bots tank.  game.spawn_bot's loadout is the one thing that does (it arms
--    sim->spawnLoadout for the slot BEFORE tankCreate runs), so this variant
--    runs with -bots 0 and spawns its own bot in "strict" mode: 0 shells, 0
--    mines, 0 trees, full armour.
--
--    That is not a detail, it is the whole arena.  With shells the bot would
--    simply shoot the neutral pillbox down and the strays would stop; with
--    trees its LGM would repair our pill back to full between hits.  An empty
--    tank can do neither, so the only question left is the one the test asks:
--    when our pill takes stray fire, does the bot park on watch or go do its
--    job?  It matches the incident too -- par2 bot3 had a live goal queue and
--    a pill it was not going to save by standing next to it.
--
-- 2. OWNERSHIP.  The map's live pill P and the dead pill belong to slot 0; the
--    neutral pillbox N stays neutral (nil owner).  Our base is re-owned and
--    then EMPTIED, so refuel_at_base has nothing to offer and does not become
--    the arena's main event.
--
-- 3. THE PILL'S HP.  This used to be written to pill_scariness_A_hp.log for
--    the python driver to read back.  There is no `io` in the scenario sandbox
--    and nothing reads a file any more, so the one question the driver asked
--    of that log -- did our pill ever fall below the healthy line -- is kept
--    in a local instead.

local OUR_PILL      = { 126, 122 }
local NEUTRAL_PILL  = { 126, 120 }
local OUR_BASE      = { 120, 132 }
local SPAWN_MODE    = "strict"
local US            = 0

-- 2026-09-06: pinned to the KEEL defend evaluator (DEFEND_ALARM_MODE off),
-- because the driver's A2 asserts on the arrived NO-BID / WATCH rungs that
-- alarm mode deletes.  This is the -bot-init string pill_scariness_test.py
-- passed, unchanged; game.init_tokens packs it into the init table spawn_bot
-- takes and the brain splits it back apart.
local CFG_TOKENS    = "cfg=DEFEND_ALARM_MODE=false"

-- generate_pill_scariness_map.py: a pillbox is 15 armour full, and the
-- defend gate's healthy line is DEFEND_WATCH_MIN_HP_FRAC (2/3) of that.
local PILLS_MAX_HEALTH = 15
local HEALTHY_HP       = 10

local idx_our  = nil      -- our pill, by slot: a pill picked up moves, a slot does not
local idx_dead = nil      -- the free body the bot is meant to go and fetch
local dead_at  = nil      -- where that body started, so a re-deploy still counts
local min_hp   = PILLS_MAX_HEALTH
local collected = false

local function own_everything(g, owner)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      if p.x == NEUTRAL_PILL[1] and p.y == NEUTRAL_PILL[2] then
        g.set_pill_owner(i, nil)          -- nil = neutral: shoots everyone
      else
        g.set_pill_owner(i, owner)
      end
    end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
      g.set_base_owner(i, owner)
      -- Emptied AFTER the owner change: set_base_owner drains stock on a
      -- non-neutral -> non-neutral change anyway, but say it outright so the
      -- intent survives an engine-rule change.
      g.set_base_stock(i, 0, 0, 0)
    end
  end
end

function on_setup(g)
  own_everything(g, US)
  -- The three pillboxes are named once, by slot.  The dead one is whichever
  -- is neither ours nor the neutral, so the arena does not have to carry the
  -- generator's coordinate for it.
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      if p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
        idx_our = i
      elseif not (p.x == NEUTRAL_PILL[1] and p.y == NEUTRAL_PILL[2]) then
        idx_dead = i
        dead_at = { p.x, p.y }
      end
    end
  end
end

-- The bot is spawned here rather than fielded by the runner, for the loadout
-- (see job 1) and because a runner-fielded bot gets no init table and the cfg
-- pin rides in that table.  on_setup's roster ops are queued by the prelude
-- and flushed at the top of on_start, so the seat comes up on the first tick.
function on_start(g)
  local s, err = g.spawn_bot{ slot = US, name = "StrayWatch",
                              loadout = SPAWN_MODE,
                              init = g.init_tokens(CFG_TOKENS) }
  if s == nil then
    g.log("PILL_SCARINESS spawn_bot failed: " .. tostring(err))
  else
    own_everything(g, US)
  end
end

function on_tick(g, tick)
  if idx_our then
    local p = g.pill(idx_our)
    if p and p.armour < min_hp then min_hp = p.armour end
  end
  if idx_dead and not collected then
    local p = g.pill(idx_dead)
    -- Collected means in a tank, or already put down somewhere else: the
    -- driver counted a re-deploy as collected too.
    if p == nil or p.in_tank or p.x ~= dead_at[1] or p.y ~= dead_at[2] then
      collected = true
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/pill_scariness_test.py check_A, the two
-- clauses of it that are world state:
--   A4  our pill really did stay above the healthy line, which is what a bot
--       that parks beside it to watch would not have allowed -- standing there
--       keeps it inside the neutral pillbox's range;
--   A5  and the bot went and did its job instead, so the free body is gone
--       from the tile it started on.
--
-- LEFT BEHIND: A1, A2 and A3, which are all lines in the brain's own print2
-- log -- PILL_HIT_SRC's src=npill attribution off the shell back-ray, the
-- HEAT_GATE NO-BID row carrying dmgsrc=npill{x0.25}, and the absence of any
-- WATCH bid on a healthy pill.  A scenario cannot see the brain's scoring at
-- all, so the attribution itself does not move; what moves is its consequence.
VERDICT_CHECK = function(g)
  if idx_our == nil then
    return false, "our pill was never found on the map"
  end
  if min_hp >= PILLS_MAX_HEALTH then
    return false, "our pill was never hit at all, so nothing was measured"
  end
  if min_hp < HEALTHY_HP then
    return false, string.format("our pill fell to %d of %d (healthy %d), collected=%s",
                                min_hp, PILLS_MAX_HEALTH, HEALTHY_HP,
                                tostring(collected))
  end
  if not collected then
    return false, string.format("pill held at %d/%d but the free body was left lying",
                                min_hp, PILLS_MAX_HEALTH)
  end
  return true, string.format("pill stayed at %d/%d and the free body was collected",
                             min_hp, PILLS_MAX_HEALTH)
end
