-- Scenario script for tests/repair_priority_C.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/repair_priority_test.py arena C.
--
-- A DEAD pill two tiles east with the forest right beside it, and a farm row
-- pinned to 99 -- the most a farm can be worth at any tree count where a repair
-- is still affordable.  Same walk for both, so only the values differ: 450
-- against 99.  The bot, ownership, the pond fill and the trace.
--
-- THE BOT IS THE ARENA'S OWN.  The farm ceiling is two of the driver's own
-- -bot-init tokens and the gate runner fields its -bots N with no init at all,
-- so the arena spawns seat 0 itself and the GATE line asks for bots=0.  Without
-- TREE_OPPORTUNISTIC_MAX raised there is no farm row to beat and the arena
-- measures nothing.
--
-- FILLING THE SPAWN POND.  A start square has to be DEEP SEA at map load
-- (starts.c startsIsValidSquare), which leaves a one-tile hole in the corridor
-- -- and the LGM walk sim (brainPathfinderLgmTravelTicks) walks a STRAIGHT LINE
-- with a crude slide, so a hole anywhere on the line makes the target read
-- `unreachable`.  That is not a subtlety: it decided two runs of these arenas
-- before it was found.  Arena C's corpse was unreachable for its first ~140
-- ticks -- the pond sat between the tank and the pill -- and the farm row won by
-- default while the only competitor had no score at all.  So the pond is filled
-- back to GRASS once the tank is ashore, which is what the ground would have
-- been if the engine did not need a puddle to spawn onto.
--
-- THE TRACE, IN MEMORY.  The old script wrote every change of ARMOUR, OWNER
-- and IN_TANK to repair_priority_C_trace.log and the python driver read it
-- back.  `io` is not on the scenario sandbox's base list, so the two questions
-- the driver asked of that file are answered from locals instead: did the
-- pill's armour rise, and had anybody taken it -- changed its owner or carried
-- it -- before it did.
--
-- The pill is read BY INDEX, resolved once from its tile: a pill that is picked
-- up rides along with the carrier, so a coordinate match would stop finding it
-- at exactly the moment the check exists to rule that out.

local OURS  = { { 129, 126 } }
local NEUTRALS = {  }
local SPAWN = { 127, 126 }
local GRASS = 7
-- Doubled from the old script's 60: on_tick's tick goes up by 2 per frame on
-- this host and by 1 on the old one.
local FILL_TICK = 120
local p0 = 0

local BOT_BRAIN = "../brains/GoalHunter_1.7/init.lua"
-- The driver's TOKENS["C"]: capture and repair_pill priced out so the TANK
-- stays out of the experiment, and the farm row at its 99 ceiling.
local TOKENS = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=REPAIR_BASE_COST=1e30;" ..
               "cfg=TREE_OPPORTUNISTIC_MAX=41;cfg=BUILDER_POOL_VALUE_FARM=99"

local filled = false
local traced = nil               -- { { index, x, y, start_hp }, ... }
local rose_at = {}               -- pill index -> tick its armour first rose
local taken = {}                 -- pill index -> it changed hands or was carried

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
    if p then
      local is_neutral = false
      for _, n in ipairs(NEUTRALS) do
        if same(p, n) then is_neutral = true end
      end
      if is_neutral then
        g.set_pill_owner(i, g.NEUTRAL)
      else
        g.set_pill_owner(i, p0)
      end
    end
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
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
end

-- THE CORPSE IS NAILED TO ITS SQUARE.  The driver kept the TANK out of the
-- experiment with a knob -- capture_pill priced past the unaffordable line, so
-- the bot never routes to a dead pill.  Driving over one is not a decision
-- though: tank.c pockets any capturable pillbox the tracks pass, goal or no
-- goal, and a corpse riding in the tank is an errand the LGM can no longer be
-- measured on.  Measured here: arena C's tank picked its corpse up at t=1780
-- with every token in place.  So the engine is refused the pickup as well.
-- A tank pockets a dead pillbox by driving over it, goal or no goal:
-- tank.c takes any capturable pill the tracks pass, so pricing capture out
-- of the goal pool with a token only stops the bot ROUTING to a corpse.
-- Refusing the capture outright is what keeps this arena's errand alive.
-- The kind is the whole test, so the number is not read; where it is, the
-- host counts it the way game.pill does, from one.
function can_capture(g, kind, n, p)
  if kind == "pill" then return false end
  return nil
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_tick(g, tick)
  -- ...but only once the tank is ASHORE and off its boat. Filling the tile
  -- while the tank is still sitting on it leaves the boat state stuck and the
  -- LGM never becomes available -- the pool then reads `no_man` for the whole
  -- run.
  if not filled and tick >= FILL_TICK then
    local tk = g.tank(p0)
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message(string.format(
        "REPAIR_PRIORITY_C filled the spawn pond at (%d,%d) with grass at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end
  if not traced then traced = resolve(g) end
  for _, e in ipairs(traced) do
    local p = g.pill(e[1])
    if p then
      if p.in_tank or p.owner ~= p0 then taken[e[1]] = true end
      if not rose_at[e[1]] and p.armour > e[4] then rose_at[e[1]] = tick end
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/repair_priority_test.py arena C, the
-- REPAIRED-NOT-CAPTURED half: the corpse comes back up, and it did so without
-- anybody changing its owner or carrying it.  With the farm row pinned at its
-- ceiling on the same walk, a rebuilt corpse is the repair beating the farm.
--
-- LEFT BEHIND, because it is print2: the two scores side by side (429 against
-- 41) and the farm being a live, fireable candidate alongside rather than an
-- absent one.  A scenario sees the outcome, not the competition.
--
-- 6000, not the driver's 3000: one errand, with room for the tank to be killed
-- and to come back.
--
-- GATE: ticks=6000 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if not traced or #traced < 1 then
    return false, "the arena never found its pill"
  end
  local e = traced[1]
  if taken[e[1]] then
    return false, "the corpse changed hands or was carried"
  end
  if rose_at[e[1]] == nil then
    return false, "the corpse (129,126) was never rebuilt"
  end
  return true, string.format("corpse rebuilt at t=%d, never taken", rose_at[e[1]])
end
