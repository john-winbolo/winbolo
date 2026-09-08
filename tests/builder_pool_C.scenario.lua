-- Scenario sidecar for tests/builder_pool_C.map.  Companion to
-- tests/builder_pool_test.py variant C -- "two allies, one pill".
--
-- Spawns a SECOND GoalHunter 1.7 on team 0 and pins the two bots to opposite
-- ponds, equidistant from the single worn pill in the middle.  Both see it,
-- both can reach it with the man, and both would repair it -- which is exactly
-- the race BUILDER_POOL_PLAN section 6 is about.  The claim rides /info extra
-- as `bpj`; the earlier claim tick wins and a same-tick tie breaks to the lower
-- player number, so the outcome is deterministic and one of the two must log
-- ally_repairing.
--
-- Both bots get their own full-stock base so neither goes hunting for fuel and
-- wanders out of the leash mid-experiment.

local OUR_PILL = { 126, 126 }
local BASES    = { { 114, 126 }, { 138, 126 }, { 126, 120 } }
local ALLY_BRAIN = "../brains/GoalHunter_1.7/init.lua"
local TRACE    = "builder_pool_C_hp.log"

local p0 = 0
local ally = nil
local spawn_tried = false
local last_hp = nil

local function own_ours(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      g.set_pill_owner(i, p0)
    end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      for _, bp in ipairs(BASES) do
        if b.x == bp[1] and b.y == bp[2] then
          g.set_base_owner(i, p0)
          g.set_base_stock(i, 90, 90, 90)
        end
      end
    end
  end
end

function on_setup(g)
  own_ours(g)
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# tick hp\n") f:close() end
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  if ally and p == ally then return 2 end
  return nil
end

function on_tick(g, tick)
  -- 50 ticks after bot 0, not 2: the two starts are the same distance from
  -- the pill, and two pools that become eligible on the SAME tick both send
  -- their man -- a claim only reaches the ally on the next tick and there is
  -- no recall. Andrew (2026-09-08) chose to live with that race rather than
  -- delay every dispatch by a tick, so the arena keeps the claim's one-tick
  -- head start by offsetting the ally's whole timeline instead.
  if not spawn_tried and tick >= 50 then
    spawn_tried = true
    -- The 5th argument is this bot's BRAIN_INIT_ARG.  cfg=DEFEND_ALARM_MODE=
    -- false keeps the ALLY on the keel defend evaluator, whose ARRIVED
    -- WATCH/REPAIR rungs are what park it at the pill and give the bpj claim a
    -- loser to stand down.  Alarm mode has no arrived ladder and no enemy on
    -- this map, so without the pin the ally wanders off and the arena stops
    -- being about the claim.  builder_pool_test.py pins slot 0 the same way and
    -- carries the full reasoning (see KEEL_DEFEND there).
    -- The 6th argument pins the ally to start 2 (the east pond).  The
    -- on_choose_start hook below cannot do it: it fires INSIDE spawn_bot,
    -- before `ally` is known, so it used to answer nil for the ally and the
    -- engine picked -- which, before the 2026-09-08 start rewrite, dropped
    -- the ally on start 1 next to bot 0 and made the arena asymmetric by
    -- accident (the claim then had a tick to win).
    local s, err = g.spawn_bot("Ally", ALLY_BRAIN, 0, nil,
                               "cfg=DEFEND_ALARM_MODE=false", 2)
    if s == nil then
      g.message("BUILDER_POOL_C spawn_bot failed: " .. tostring(err))
    else
      ally = s
      g.set_team(s, 0)
      g.message("BUILDER_POOL_C ally slot=" .. tostring(s) .. " team=0")
      -- The pill stays slot 0's, so it reads "friendly" to p0 and "allied" to
      -- the ally. Both discover it: world.lua's owner classification is what
      -- the pool's discovery filters on, and only p0's copy is "friendly", so
      -- the ally reaching for it at all is itself the thing being de-conflicted.
      own_ours(g)
    end
  end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      if last_hp ~= p.armour then
        local f = io.open(TRACE, "a")
        if f then f:write(string.format("%d %d\n", tick, p.armour)) f:close() end
        last_hp = p.armour
      end
      break
    end
  end
end
