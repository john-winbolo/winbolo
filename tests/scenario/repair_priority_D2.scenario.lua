-- Scenario script for tests/repair_priority_D2.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/repair_priority_test.py arena D2.
--
-- Arena D's ground, run with cfg=BUILDER_POOL_REPAIR_LINEAR=false: the control.
-- The old formula still asks danger.lgm_path_safe_enhanced, so the same corpse
-- must be refused `path_unsafe` -- which is what makes D's silence evidence
-- about the rule and not about an arena that was never dangerous.
--
-- THE BOT IS THE ARENA'S OWN.  The knob under test is one of the driver's
-- -bot-init tokens and the gate runner fields its -bots N with no init at all,
-- so the arena spawns seat 0 itself and the GATE line asks for bots=0.  Without
-- BUILDER_POOL_REPAIR_LINEAR=false this arena is arena D and its verdict is
-- inverted.
--
-- FILLING THE SPAWN POND.  A start square has to be DEEP SEA at map load
-- (starts.c startsIsValidSquare), which leaves a one-tile hole in the corridor
-- -- and the LGM walk sim (brainPathfinderLgmTravelTicks) walks a STRAIGHT LINE
-- with a crude slide, so a hole anywhere on the line makes the target read
-- `unreachable`.  So the pond is filled back to GRASS once the tank is ashore,
-- which is what the ground would have been if the engine did not need a puddle
-- to spawn onto.
--
-- THE TRACE, IN MEMORY, AND THE MAN.  The old script wrote every change of
-- ARMOUR, OWNER and IN_TANK to repair_priority_D2_trace.log and the driver read
-- it back.  `io` is not on the scenario sandbox's base list, so the pill is
-- followed by index in locals, and the man is read from game.builder(), whose
-- `state` says whether he is aboard and whose square says where he walked.
-- This is the arena where NO LGM is expected at the corpse.

local OURS  = { { 119, 126 } }
local NEUTRALS = { { 116, 126 }, { 115, 126 }, { 115, 125 }, { 115, 127 } }
local SPAWN = { 126, 126 }
local GRASS = 7
-- Doubled from the old script's 60: on_tick's tick goes up by 2 per frame on
-- this host and by 1 on the old one.
local FILL_TICK = 120
local p0 = 0

local BOT_BRAIN = "../brains/GoalHunter_1.7/init.lua"
-- The driver's TOKENS["D2"]: arena D's two tokens, plus the legacy formula.
local TOKENS = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=ATTACK_PILL_BASE_COST=1e30;" ..
               "cfg=BUILDER_POOL_REPAIR_LINEAR=false"

local filled = false
local traced = nil               -- { { index, x, y, start_hp }, ... }
local rose_at = nil              -- tick the corpse's armour first rose
local taken = false              -- it changed hands or was carried
local man_near = 99              -- closest the builder ever got to the corpse

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
  -- LGM never becomes available.
  if not filled and tick >= FILL_TICK then
    local tk = g.tank(p0)
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message(string.format(
        "REPAIR_PRIORITY_D2 filled the spawn pond at (%d,%d) with grass at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end

  local bd = g.builder(p0)
  if bd and bd.state ~= "in_tank" and bd.state ~= "dead" then
    local d = math.abs(bd.mx - OURS[1][1]) + math.abs(bd.my - OURS[1][2])
    if d < man_near then man_near = d end
  end

  if not traced then traced = resolve(g) end
  for _, e in ipairs(traced) do
    local p = g.pill(e[1])
    if p then
      if p.in_tank or p.owner ~= p0 then taken = true end
      if not rose_at and p.armour > e[4] then rose_at = tick end
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/repair_priority_test.py arena D2, the control
-- whose entire result is a refusal: the corpse stays down and no man reaches
-- it.  Both halves are asked, because a corpse nobody rebuilt is also what a
-- man killed on the way looks like, and only the second says the walk was
-- never started.
--
-- LEFT BEHIND, because it is print2: that the refusal reads `path_unsafe` and
-- not one of the ten other gates that could also have refused this row.
--
-- GATE: ticks=4000 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if not traced or #traced < 1 then
    return false, "the arena never found its pill"
  end
  if taken then
    return false, "the corpse changed hands or was carried"
  end
  if rose_at ~= nil then
    return false, string.format("the corpse was rebuilt at t=%d: the old "
                                .. "formula sent him anyway", rose_at)
  end
  if man_near <= 1 then
    return false, string.format("the man walked to the corpse (within %d tile)",
                                man_near)
  end
  return true, string.format("corpse down, man never closer than %d tiles",
                             man_near)
end
