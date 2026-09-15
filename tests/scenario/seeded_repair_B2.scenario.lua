-- Scenario script for tests/seeded_repair_B2.map (auto-loaded as
-- <map>.scenario.lua).
--
-- Arena B2: the CONTROL for arena B, same ground, with the goal-pill bonus at
-- 1.0 and nothing else changed.  The two rows now tie exactly, the sort falls
-- through to the tile key, and the OTHER pill at (125,126) -- lower x, lower
-- key -- has to be the one that comes back up first.
--
-- It does four things and nothing else:
--   THE BOT.  The knob under test is pinned by the driver's -bot-init and the
--     gate runner fields its -bots N with no init, so the arena spawns seat 0
--     itself and the GATE line asks for bots=0.  DEFEND_ALARM_MODE=false puts
--     the defend evaluator back on the only ladder with an ARRIVED branch,
--     which is the only one that seeds the pool at all, and REPAIR_BASE_COST
--     past the unaffordable line keeps the OTHER feeder out.
--   OWNERSHIP.  Both pills and the base are player 0's; the .map's owner byte
--     is not something to rely on for a scripted game.
--   THE POND.  A start square has to be DEEP SEA at map load (starts.c
--     startsIsValidSquare), which leaves a one-tile hole in the field -- and
--     the LGM walk sim (brainPathfinderLgmTravelTicks) walks a STRAIGHT LINE,
--     so a hole anywhere on it makes the target read `unreachable`.  The pond
--     is filled back to grass once the tank is ashore and OFF the tile (filling
--     it under the tank leaves the boat state stuck and the LGM never becomes
--     available at all).
--   THE TRACE, IN MEMORY.  The old script wrote every change of ARMOUR, OWNER
--     and IN_TANK to seeded_repair_B2_trace.log and the python driver read it
--     back.  `io` is not on the scenario sandbox's base list, so the questions
--     the driver asked of that file are answered from locals: which pill's
--     armour rose first, and whether anybody took a pill -- changed its owner
--     or carried it -- on the way.  The pills are read BY INDEX, resolved once
--     from their tiles, because a pill that is picked up rides with the carrier.

local GOAL_PILL  = { 127, 126 }
local OTHER_PILL = { 125, 126 }
local OURS  = { GOAL_PILL, OTHER_PILL }
local SPAWN = { 126, 120 }
local GRASS = 7
-- Doubled from the old script's 60: on_tick's tick goes up by 2 per frame on
-- this host and by 1 on the old one, so a duration written here means half the
-- wall time it used to unless it is doubled.
local FILL_TICK = 120
local p0 = 0

local BOT_BRAIN = "../brains/GoalHunter_1.7/init.lua"
-- The driver's TOKENS["B2"]: arena B's two, plus the bonus at its keel value.
-- NOT the keel seeding rule -- the seeded row competing is what puts the two
-- rows side by side in the first place.
local TOKENS = "cfg=REPAIR_BASE_COST=1e30;cfg=DEFEND_ALARM_MODE=false;" ..
               "cfg=BUILDER_POOL_GOAL_PILL_BONUS=1"
-- ONE TOKEN THE DRIVER DID NOT NEED.  cfg=PILL_REPOSITION_ENABLED=false is not
-- in the driver's TOKENS for this family; it is in stranded_lgm's, for exactly
-- the reason it is here.  Measured on this ground: the bot decides its own
-- damaged pill is badly placed, SHOOTS IT DOWN to move it and drives over the
-- corpse (pill (127,126) at hp 0 and in a tank by t=1552).  The arena's whole
-- errand goes with it.  Nothing in this test is about reposition and it is off
-- in all four arenas, so the pair and its control still differ only in the
-- knobs under test.
local TOKENS_ALL = TOKENS .. ";cfg=PILL_REPOSITION_ENABLED=false"

local filled = false
local traced = nil               -- { { index, x, y, start_hp }, ... }
local rose_at = {}               -- pill index -> tick its armour first rose
local taken = {}                 -- pill index -> it changed hands or was carried
-- Somebody shot it below the armour it started on.  A pill that went down and
-- came back up "rose", and would be read as a repair the arena never staged.
local sank = {}

local function same(p, t) return p.x == t[1] and p.y == t[2] end

local function resolve(g)
  local out = {}
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      for _, t in ipairs(OURS) do
        if same(p, t) then out[#out + 1] = { i, t[1], t[2], p.armour } end
      end
    end
  end
  return out
end

local function own_everything(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then g.set_pill_owner(i, p0) end
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
  -- Queued by the compat prelude and flushed on the round's first tick: a
  -- roster op is refused inside on_setup on this host.
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS_ALL) }
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_tick(g, tick)
  if not filled and tick >= FILL_TICK then
    local tk = g.tank(p0)
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message(string.format(
        "SEEDED_REPAIR_B2 filled the spawn pond at (%d,%d) with grass at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end
  if not traced then traced = resolve(g) end
  for _, e in ipairs(traced) do
    local p = g.pill(e[1])
    if p then
      if p.in_tank or p.owner ~= p0 then taken[e[1]] = true end
      if p.armour < e[4] then sank[e[1]] = true end
      if not rose_at[e[1]] and p.armour > e[4] then rose_at[e[1]] = tick end
    end
  end
end

local function rise(x)
  for _, e in ipairs(traced or {}) do
    if e[2] == x then
      return rose_at[e[1]], (taken[e[1]] or sank[e[1]]) and true or false
    end
  end
  return nil, nil
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/seeded_repair_test.py arena B2, the control
-- that gives arena B's ordering its meaning: with the bonus gone the tie
-- breaks on the tile key and the lower-x pill goes first.
--
-- LEFT BEHIND, because it is print2: that the winning line carries no goal_w
-- chip at all, which is the direct reading of the bonus being switched off.
--
-- GATE: ticks=8000 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if not traced or #traced < 2 then
    return false, "the arena never found its two pills"
  end
  local goal, goal_taken = rise(GOAL_PILL[1])
  local other, other_taken = rise(OTHER_PILL[1])
  if goal_taken or other_taken then
    return false, "a pill was taken or shot down"
  end
  if other == nil then
    return false, "the pill at (125,126) was never repaired"
  end
  if goal ~= nil and goal < other then
    return false, string.format("the goal pill went first: t=%d before t=%d",
                                goal, other)
  end
  return true, string.format("tie broke on the tile key: (125,126) at t=%d",
                             other)
end
