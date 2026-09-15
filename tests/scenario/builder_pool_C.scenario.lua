-- Scenario sidecar for tests/builder_pool_C.map.  Companion to
-- tests/builder_pool_test.py variant C -- "two allies, one pill".
--
-- Two GoalHunter 1.7 bots pinned to opposite ponds, equidistant from the single
-- worn pill in the middle.  Both see it, both can reach it with the man, and
-- both would repair it -- which is exactly the race BUILDER_POOL_PLAN section 6
-- is about.  The claim rides /info extra as `bpj`; the earlier claim tick wins
-- and a same-tick tie breaks to the lower player number, so the outcome is
-- deterministic and one of the two must stand down.
--
-- Both bots get their own full-stock base so neither goes hunting for fuel and
-- wanders out of the leash mid-experiment.

local OUR_PILL   = { 126, 126 }
local BASES      = { { 114, 126 }, { 138, 126 }, { 126, 120 } }
local OUR_SLOT   = 0
local ALLY_SLOT  = 1
local GOALHUNTER = "../brains/GoalHunter_1.7/init.lua"    -- from the build dir

-- The driver's KEEL_DEFEND, on BOTH bots.  It keeps them on the keel defend
-- evaluator, whose ARRIVED WATCH/REPAIR rungs are what park a bot at the pill
-- and give the bpj claim a loser to stand down.  Alarm mode has no arrived
-- ladder and no enemy on this map, so without the pin a bot wanders off and the
-- arena stops being about the claim.  The driver pinned slot 0 on the command
-- line and the ally through spawn_bot; here both come from spawn_bot.
local KEEL_DEFEND = "cfg=DEFEND_ALARM_MODE=false"

-- Team 1, not the 0 this arena was ported with: team 0 is NO team on this host,
-- so the two bots are not allies and the `bpj` claim -- which rides the
-- ally-only internal channel -- is never delivered.  That is the whole arena.
local TEAM = 1

-- The ally's whole timeline is offset rather than started level: the two starts
-- are the same distance from the pill, and two pools that become eligible on
-- the SAME tick both send their man, because a claim only reaches the ally on
-- the next tick and there is no recall.  Andrew (2026-09-08) chose to live with
-- that race rather than delay every dispatch by a tick.  100, not the ported
-- 50: the hook clock on this host runs at 2 ticks per brain think, so a
-- duration written for the old host means half as long here.
local ALLY_AT = 100

local pill_n      = nil
local start_hp    = nil
local repaired_at = nil   -- tick the pill's armour first rose
local both_out_at = nil   -- tick both men were first at the pill together
local reached     = { [OUR_SLOT] = nil, [ALLY_SLOT] = nil }
local last_hp     = nil
local spawned     = false
local ally_tank   = nil   -- tick the ally first had a tank on the map

local function index_pill(g)
  if pill_n then return end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then pill_n = i end
  end
end

local function own_ours(g)
  index_pill(g)
  if pill_n then g.set_pill_owner(pill_n, OUR_SLOT) end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      for _, bp in ipairs(BASES) do
        if b.x == bp[1] and b.y == bp[2] then
          g.set_base_owner(i, OUR_SLOT)
          g.set_base_stock(i, 90, 90, 90)
        end
      end
    end
  end
end

function on_setup(g)
  own_ours(g)
  -- Our bot is fielded by the arena rather than by the runner's -bots, which
  -- hands out no init and would leave it on alarm mode.  The prelude defers the
  -- spawn to on_start.  `start` is named here rather than left to
  -- on_choose_start because that hook fires INSIDE spawn_bot, before this file
  -- could know which seat it is for -- which is how an earlier version of this
  -- arena silently put both bots on the same pond.
  g.spawn_bot{ slot = OUR_SLOT, name = "Ours", brain = GOALHUNTER,
               team = TEAM, start = 1, init = g.init_tokens(KEEL_DEFEND) }
end

-- Only for a respawn: the first lives come in on the starts named above.
function on_choose_start(g, p)
  if p == OUR_SLOT  then return 1 end
  if p == ALLY_SLOT then return 2 end
  return nil
end

-- Is this seat's man out of the tank and standing on or beside the pill? The
-- engine-side stand-in for the driver's BP_DISPATCH line: the pool is the only
-- thing that puts a man on the ground, and on this pocket of a map there is
-- nothing else out there for him to do.
local function man_at_pill(g, slot, px, py)
  local b = g.builder(slot)
  if b == nil or b.state == "in_tank" or b.state == "dead" then return false end
  local dx = b.mx - px
  local dy = b.my - py
  if dx < 0 then dx = -dx end
  if dy < 0 then dy = -dy end
  return (dx + dy) <= 2
end

function on_tick(g, tick)
  -- The pill and the bases are handed over again for the first few ticks: an
  -- owner written in on_setup names a seat the roster does not hold yet.
  if tick <= 20 then own_ours(g) end

  if not spawned and tick >= ALLY_AT then
    spawned = true
    g.spawn_bot{ slot = ALLY_SLOT, name = "Ally", brain = GOALHUNTER,
                 team = TEAM, start = 2, init = g.init_tokens(KEEL_DEFEND) }
    -- The pill stays slot 0's, so it reads "friendly" to us and "allied" to the
    -- ally.  Both discover it: world.lua's owner classification is what the
    -- pool's discovery filters on, and only our copy is "friendly", so the ally
    -- reaching for it at all is itself the thing being de-conflicted.
    own_ours(g)
  end

  -- "Only one man went" would also be true of an arena with only one bot in it,
  -- which is the driver's assertion 0. The ally's tank is the engine's answer
  -- to it.
  if spawned and ally_tank == nil and g.tank(ALLY_SLOT) ~= nil then
    ally_tank = tick
  end

  if pill_n == nil then return end
  local p = g.pill(pill_n)
  if p == nil or p.in_tank then return end

  if start_hp == nil then start_hp = p.armour end
  if last_hp ~= nil and p.armour > last_hp and repaired_at == nil then
    repaired_at = tick
  end
  last_hp = p.armour

  local ours = man_at_pill(g, OUR_SLOT, p.x, p.y)
  local ally = spawned and man_at_pill(g, ALLY_SLOT, p.x, p.y)
  if ours and reached[OUR_SLOT]  == nil then reached[OUR_SLOT]  = tick end
  if ally and reached[ALLY_SLOT] == nil then reached[ALLY_SLOT] = tick end
  if ours and ally and both_out_at == nil then both_out_at = tick end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/builder_pool_test.py check_C assertion 1: one
-- pill, two willing bots, and never two men out for it at once.  The driver
-- counted BP_DISPATCH lines and called it a failure when two of them, from
-- different bots, fell within 200 brain ticks of each other.  A scenario cannot
-- see a dispatch, so this watches the men themselves and asks the stricter
-- question the engine can answer: were both ever at the pill on the same tick.
--
-- LEFT BEHIND: assertion 2 -- that the loser logged BP_DENY with reason
-- `ally_repairing`, which is what says the claim and not something else stopped
-- it.  That is a print2 line about the brain's own reasoning.  It is the half
-- that names the mechanism, and without it this arena cannot tell a loser that
-- stood down from an ally that never wanted the pill at all; what is left
-- proves only that two willing bots produced one trip.  Assertion 0 survives as
-- the ally-tank check below.
--
-- GATE: ticks=5200 bots=0 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function(g)
  if pill_n == nil then
    return false, "no pill at the arena's own square"
  end
  if ally_tank == nil then
    return false, "the ally never took a tank, so there was no second bidder"
  end
  if both_out_at then
    return false, string.format("both men were at the pill at t=%d",
                                both_out_at)
  end
  if reached[OUR_SLOT] == nil and reached[ALLY_SLOT] == nil then
    return false, "neither bot ever sent its man to the pill"
  end
  if repaired_at == nil then
    return false, "a man went but the pill was never repaired"
  end
  local who = (reached[OUR_SLOT] ~= nil) and OUR_SLOT or ALLY_SLOT
  return true, string.format("only p%d spent its man; repaired at t=%d",
                             who, repaired_at)
end
