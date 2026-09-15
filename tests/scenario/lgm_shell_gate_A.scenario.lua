-- Scenario sidecar for tests/lgm_shell_gate_A.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/lgm_shell_gate_test.py arena A.
--
-- OUR DEAD PILL three tiles west of the tank, and ONE NEUTRAL PILLBOX on an
-- island the tank cannot reach, seven tiles further west on the SAME ROW.  The
-- pill shells the tank down that row all run long, and the man's walk to the
-- corpse is that row -- so the round that lands on the hull lands next to a man
-- who has just stepped off it.  Arena A has danger.lgm_shell_gate on and must
-- refuse those ticks (`shell_will_hit`); arena B is the same ground with
-- cfg=BUILDER_POOL_SHELL_GATE=false and must never print that reason.
--
-- This sidecar hands the island pill to game.NEUTRAL: a neutral pill shoots
-- everyone, which is why the arena needs no second bot, and the .map owner byte
-- has no neutral encoding mapRead is guaranteed to keep.
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
-- THE WATCH.  Every change of ARMOUR, OWNER or IN_TANK on our pill used to go
-- to lgm_shell_gate_A_trace.log, which the driver then read, so "the repair
-- actually landed" is asked of the ENGINE and not of the brain's opinion of
-- itself -- and so is the harder question the armour alone cannot answer: that
-- the pill came back up WITHOUT anybody capturing it or picking it up first.
-- An armour rise on a pill that changed hands on the way is not a repair, it is
-- somebody else's pill; the owner and in_tank columns are what rule that out.
-- There is no io in the scenario sandbox, so the same changes are counted in
-- the locals below and the driver's repair_landed becomes this arena's own
-- verdict.
--
-- The pill is watched BY INDEX and never by coordinate, because a pill that is
-- picked up rides along with the tank that took it.

-- THE ARENA FIELDS ITS ONE TANK, so the GATE line says bots=0.  The measured
-- GoalHunter has to come in with lgm_shell_gate_test.py's pins and the runner
-- fields its -bots N seats with no init at all, so the arena does the spawn.
local OUR_BRAIN  = "../brains/GoalHunter_1.7/init.lua"
local OUR_SLOT   = 0
-- lgm_shell_gate_test.py TOKENS["A"], verbatim.
local OUR_TOKENS = "cfg=BUILDER_POOL_UNDER_FIRE_TICKS=0;cfg=PILL_REPOSITION_ENABLED=false"
local OURS  = { { 127, 125 } }
local NEUTRALS = { { 124, 126 } }
local SPAWN = { 128, 126 }
local GRASS = 7
local TOPUP_ARMOUR = 11          -- 4 down: BUILDER_POOL_TOPUP_MIN_MISSING exactly
-- DOUBLED in the port.  The tick on_tick is handed now goes up by two a frame
-- where the old host's went up by one, so both of these -- each a DURATION --
-- had to grow by the same factor to mean the same wait.
local REDAMAGE_AFTER = 32        -- engine ticks a repaired pill is left at full
local FILL_TICK = 120            -- engine ticks: the tank is ashore well before
local p0 = 0

local us_tried = false

-- THE WATCH, which is what the trace file was for: the driver's repair_landed
-- read the file to ask whether our pill ever came back to full armour, and
-- whether it stayed ours while it did.
local FULL_HEALTH = 15           -- PILLS_MAX_HEALTH
local last_sig    = nil
local seen        = 0            -- state CHANGES, i.e. rows the file would hold
local full_tick   = nil          -- tick the armour first reached full
local stolen_at   = nil          -- tick it changed hands or was picked up

-- The man's own round trips, which is the engine's side of BP_DISPATCH: the
-- shell gate is only REACHED on a tick the pool was about to send him, so a
-- run in which he never left the tank is a run in which nothing was gated.
local man_out   = false
local sorties   = 0
local man_deaths = 0
local redamage_at = {}     -- pill index -> tick it gets knocked back down
local filled = false
local traced = nil               -- { {pill_index, tile_x, tile_y}, ... }

local function same(p, t) return p.x == t[1] and p.y == t[2] end

-- Resolve OURS (tile coords) to PILL INDICES, once.  Everything after this
-- reads the pill BY INDEX and never by coordinate: a pill that is picked up
-- moves with the carrier, so a coordinate match would quietly stop finding it
-- at exactly the moment the watch exists to prove nobody took it.  The pillbox
-- array is stable (a carried pill keeps its slot, with inTank set), so the
-- index is good for the whole run.
local function resolve(g)
  local out = {}
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      for _, t in ipairs(OURS) do
        if same(p, t) then out[#out + 1] = { i, t[1], t[2] } end
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

-- The roster ops are refused in on_setup on this host and our own tank is one
-- of them now, so the spawn is in on_tick.  Team 0 is NO team rather than team
-- zero, which is what leaves the neutral pillbox hostile to us; it is asked
-- for on the spawn itself instead of with a set_team that has no seat to name.
function on_setup(g)
  own_everything(g)
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

-- The man's deaths, which arena B asserts and arena A only reports. The driver
-- counted them off the brain's own per-tick jsonl; the engine says it here.
function on_lgm_died(g, p)
  if p == p0 then man_deaths = man_deaths + 1 end
end

function on_tick(g, tick)
  -- Our own tank, which the runner no longer fields. A spawn with no slot
  -- answers true rather than a seat, so the seat is named.
  if not us_tried and tick >= 2 then
    us_tried = true
    local u, err = g.spawn_bot{ slot = OUR_SLOT, name = "Digger",
                                brain = OUR_BRAIN, team = 0, start = 1,
                                init = g.init_tokens(OUR_TOKENS) }
    if u == nil then
      g.message("LGM_SHELL_GATE_A spawn_bot(us) failed: " .. tostring(err))
    end
    own_everything(g)
  end

  -- The man's round trips, counted as edges so one errand counts once.
  local b = g.builder(p0)
  local out = b ~= nil and (b.state == "going" or b.state == "returning")
  if out and not man_out then sorties = sorties + 1 end
  man_out = out

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
        "LGM_SHELL_GATE_A filled the spawn pond at (%d,%d) with grass at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end
  if not traced then traced = resolve(g) end
  for _, e in ipairs(traced) do
    local p = g.pill(e[1])
    if p then
      -- RE-DAMAGE THE PILL, so the pool keeps making dispatch decisions.
      --
      -- The gate is only REACHED on a tick where a row has passed every other
      -- test and the pool is about to send the man -- which, left alone, is
      -- ONCE PER ERRAND. With a round landing every 50 engine ticks and a
      -- ~15-tick refusal window, one errand is a coin toss, and an arena whose
      -- verdict is a coin toss measures luck rather than the rule. So every
      -- time the man finishes the repair the pill is knocked straight back
      -- down to TOPUP_ARMOUR, and the run becomes dozens of decisions instead
      -- of one.
      --
      -- It is a harness for the NUMBER of decisions and nothing else: the gate
      -- sees exactly the shells the engine has in the air, either way, and
      -- both arenas get the same treatment.
      --
      -- DAMAGED AND ALIVE, never a corpse: a corpse gets driven over and
      -- pocketed (an earlier draft lost its only errand to exactly that, with
      -- `in_tank 1` in this trace at sim t=2010), and a top-up costs ONE tree
      -- against a rebuild's four -- and the tank's 40 trees are the hard cap
      -- on how many errands a run can hold.
      if p.armour >= 15 and not p.in_tank then
        redamage_at[e[1]] = redamage_at[e[1]] or (tick + REDAMAGE_AFTER)
        if tick >= redamage_at[e[1]] then
          g.set_pill_armour(e[1], TOPUP_ARMOUR)
          redamage_at[e[1]] = nil
        end
      else
        redamage_at[e[1]] = nil
      end
      local in_tank = p.in_tank and 1 or 0
      local sig = p.armour .. "/" .. p.owner .. "/" .. in_tank
      if last_sig ~= sig then
        last_sig = sig
        seen = seen + 1
        if p.armour >= FULL_HEALTH and full_tick == nil then
          full_tick = tick
        end
        if (p.owner ~= p0 or in_tank == 1) and stolen_at == nil then
          stolen_at = tick
        end
      end
    end
  end
end

-- ── the verdict ───────────────────────────────────────────
-- PORTED (2026-09-15) from tests/lgm_shell_gate_test.py check_A, step 4
-- (repair_landed) and the engine's side of step 1: the man really did leave
-- the tank, and our pill really did come back to full armour while staying
-- ours and out of any tank. Those errands are the only thing the gate ever
-- gets to gate, so a run without them says nothing either way.
--
-- LEFT BEHIND, and it is the headline of the arena: steps 2 and 3, both
-- print2 out of builder_pool.lua and invisible to a scenario -- at least one
-- `BP_DENY ... reason=shell_will_hit` naming whose round it is, how many
-- engine ticks until it lands and which of the three endings it is, every
-- offset inside the gate's own 1..63 window, and then the SAME row dispatched
-- again within a few ticks. "Not this tick", never "not this job", is exactly
-- the half that cannot move. So a PASS here says the errands ran; it does not
-- say the gate refused one.
--
-- GATE: ticks=8200 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if seen == 0 then
    return false, "no pill watched: OURS and the map's geometry have drifted"
  end
  if sorties == 0 then
    return false, "the man never left the tank, so nothing was ever gated"
  end
  if stolen_at then
    return false, string.format("the pill changed hands at t=%d -- the arena "
                                .. "lost its errand", stolen_at)
  end
  if full_tick == nil then
    return false, string.format("the pill never reached 15 armour over %d "
                                .. "change(s), so no errand completed", seen)
  end
  return true, string.format("%d sortie(s), pill back to 15 by t=%d and "
                             .. "still ours; man killed %d time(s)",
                             sorties, full_tick, man_deaths)
end
