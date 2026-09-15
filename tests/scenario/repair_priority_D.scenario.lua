-- Scenario script for tests/repair_priority_D.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/repair_priority_test.py arena D.
--
-- A dead friendly pill seven tiles west, and FOUR NEUTRAL PILLBOXES on an
-- island the tank cannot reach, whose combined danger field covers the corpse
-- and the whole of the man's walk.  Under the linear formula there is no
-- path-safety gate on a repair row at all, so the man must be dispatched
-- anyway.  This script hands the island pills to game.NEUTRAL (a neutral pill
-- shoots everyone and is stamped into threat.lua's field exactly like a hostile
-- one, which is why the arena needs no second bot).
--
-- THE BOT IS THE ARENA'S OWN.  The driver pinned two knobs on it with
-- -bot-init and the gate runner fields its -bots N with no init at all, so the
-- arena spawns seat 0 itself and the GATE line asks for bots=0.  attack_pill is
-- priced out because a pill can be SHELLED from the shore even when the island
-- cannot be driven to, and seeding is deliberately left alone -- a seeded row
-- still runs every gate in score_row, so the path-safety question is asked
-- either way.
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
-- ARMOUR, OWNER and IN_TANK to repair_priority_D_trace.log and the driver read
-- it back, and read the man out of the brain's own per-tick jsonl.  `io` is not
-- on the scenario sandbox's base list and the jsonl is not the scenario's to
-- read, so both are answered from the world instead: the pill by index, and the
-- man from game.builder(), whose `state` says whether he is aboard and whose
-- square says where he walked.

local OURS  = { { 119, 126 } }
local NEUTRALS = { { 116, 126 }, { 115, 126 }, { 115, 125 }, { 115, 127 } }
local SPAWN = { 126, 126 }
local GRASS = 7
-- Doubled from the old script's 60: on_tick's tick goes up by 2 per frame on
-- this host and by 1 on the old one.
local FILL_TICK = 120
local p0 = 0

local BOT_BRAIN = "../brains/GoalHunter_1.7/init.lua"
-- The driver's TOKENS["D"].
local TOKENS = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=ATTACK_PILL_BASE_COST=1e30"

local filled = false
local traced = nil               -- { { index, x, y, start_hp }, ... }
local rose_at = nil              -- tick the corpse's armour first rose
local taken = false              -- it changed hands or was carried
local man_out_at = nil           -- tick the builder first left the tank
local man_near = 99              -- closest he ever got to the corpse, in tiles
local man_died = false

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
        "REPAIR_PRIORITY_D filled the spawn pond at (%d,%d) with grass at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end

  local bd = g.builder(p0)
  if bd then
    if bd.state == "dead" then
      man_died = true
    elseif bd.state ~= "in_tank" then
      if not man_out_at then man_out_at = tick end
      local d = math.abs(bd.mx - OURS[1][1]) + math.abs(bd.my - OURS[1][2])
      if d < man_near then man_near = d end
    end
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
-- PORTED (2026-09-15) from tests/repair_priority_test.py arena D, which is the
-- one arena allowed to end two ways and has to say which.  The man being sent
-- is the whole claim -- "no path safety, send that LGM out" -- so the FAIL the
-- driver names is the man never leaving, and that is what this asks.  Either
-- ending is a pass: PASS-D-REPAIRED is the man out and the corpse back up
-- without changing hands; PASS-D-MAN-LOST is the man out and killed on the
-- walk with the corpse still down.
--
-- LEFT BEHIND, because it is print2: that `path_unsafe` never appears in the
-- pool's refusals.  This says the man went; it does not say the gate was never
-- consulted.  Arena D2 is the control that gives that silence its meaning.
--
-- 4000, over the driver's 1600: the island pills can kill the tank on the way
-- and it has to come back and try again.  Measured, the man is out at t=394
-- and the corpse is up at t=596.
--
-- GATE: ticks=4000 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if not traced or #traced < 1 then
    return false, "the arena never found its pill"
  end
  if taken then
    return false, "the corpse changed hands or was carried"
  end
  if man_out_at == nil then
    return false, "the man never left the tank: the walk was refused"
  end
  if rose_at then
    return true, string.format("REPAIRED: man out t=%d, corpse up t=%d",
                               man_out_at, rose_at)
  end
  if man_died then
    return true, string.format("MAN LOST: out t=%d, closest %d tiles",
                               man_out_at, man_near)
  end
  return true, string.format("man out t=%d, closest %d tiles, corpse still down",
                             man_out_at, man_near)
end
