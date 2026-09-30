-- Scenario script for tests/repair_priority_A.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/repair_priority_test.py arena A.
--
-- A 4-hp top-up THREE tiles from the tank and a DEAD pill NINE tiles away, both
-- ours, both on grass.  The linear repair score is 30 x missing - 0.25 x trip,
-- so the corpse (450 - ~68) has to beat the near top-up (120 - ~30) and the man
-- has to walk PAST the easy job to do the important one.  This script's jobs
-- are the bot, ownership, the pond fill and the trace.
--
-- THE BOT IS THE ARENA'S OWN.  The driver pinned four knobs on it with
-- -bot-init and the gate runner fields its -bots N with no init at all, so the
-- arena spawns seat 0 itself and the GATE line asks for bots=0.  Without the
-- pins this measures something else: capture_pill sends the TANK to the corpse
-- and it pockets it, and a goal that seeds the pool puts its own row at the
-- front whatever it scores.
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
-- and IN_TANK to repair_priority_A_trace.log and the python driver read it
-- back.  `io` is not on the scenario sandbox's base list, so the two questions
-- the driver asked of that file are answered from locals instead: did each
-- pill's armour rise, and had anybody taken the pill -- changed its owner or
-- carried it -- before it did.  An armour rise on a pill that changed hands is
-- not our repair, it is somebody else's pill.
--
-- The pills are read BY INDEX, resolved once from their tiles: a pill that is
-- picked up rides along with the carrier, so a coordinate match would stop
-- finding it at exactly the moment the check exists to rule that out.

local OURS  = { { 121, 126 }, { 128, 125 } }
local NEUTRALS = {  }
local SPAWN = { 130, 126 }
local GRASS = 7
-- Doubled from the old script's 60: on_tick's tick goes up by 2 per frame on
-- this host and by 1 on the old one, so every duration in this file is twice
-- the number it used to be and means the same wall time.
local FILL_TICK = 120
local p0 = 0

local BOT_BRAIN = "../brains/GoalHunter_1.7/init.lua"
-- The driver's TOKENS["A"], verbatim: capture priced out so the tank leaves the
-- corpse alone, both seeding feeders priced out so the pool's own ordering is
-- what runs, and reposition off so the bot does not shoot its own pill down.
local TOKENS = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=REPAIR_BASE_COST=1e30;" ..
               "cfg=DEFEND_REPAIR_COST=1e30;cfg=PILL_REPOSITION_ENABLED=false"

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
  -- Queued by the compat prelude and flushed on the round's first tick: a
  -- roster op is refused inside on_setup on this host.
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
  -- run, which is exactly what arena A did on the first attempt.
  if not filled and tick >= FILL_TICK then
    local tk = g.tank(p0)
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message(string.format(
        "REPAIR_PRIORITY_A filled the spawn pond at (%d,%d) with grass at t=%d",
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
-- PORTED (2026-09-15) from tests/repair_priority_test.py arena A, the
-- REPAIRED-NOT-CAPTURED half: both our pills come back up, and neither changed
-- owner or rode in a tank on the way, so the rise is our man's work and not
-- somebody else's pill handed back.  The rise is also the proof the man went:
-- nothing else on this map heals a pillbox.
--
-- LEFT BEHIND, because it is print2 and the brain's per-tick jsonl: the claim
-- the arena is named for -- that the two BP_DISPATCH lines come in the order
-- corpse-then-top-up and that the corpse's printed trip is the longer walk --
-- and the man's tile closing on each target.  A scenario cannot see either.
--
-- 6000, not the driver's 4000: room for two complete errands out and back,
-- the corpse being nine tiles off.  Measured, both pills are back up well
-- inside it.
--
-- GATE: ticks=6000 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if not traced or #traced < 2 then
    return false, "the arena never found its two pills"
  end
  local up, stolen = 0, 0
  for _, e in ipairs(traced) do
    if rose_at[e[1]] then up = up + 1 end
    if taken[e[1]] then stolen = stolen + 1 end
  end
  if stolen > 0 then
    return false, string.format("%d pill(s) changed hands or were carried", stolen)
  end
  if up < 2 then
    return false, string.format("only %d of 2 pills repaired", up)
  end
  return true, "both pills repaired, neither taken"
end
