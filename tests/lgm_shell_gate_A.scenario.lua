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
-- TRACE.  Every change of ARMOUR, OWNER or IN_TANK on the pills the arena is
-- about is written to lgm_shell_gate_A_trace.log in SIM ticks, so "the repair
-- actually landed" is asked of the ENGINE and not of the brain's opinion of
-- itself -- and so is the harder question the armour alone cannot answer: that
-- the pill came back up WITHOUT anybody capturing it or picking it up first.
-- An armour rise on a pill that changed hands on the way is not a repair, it is
-- somebody else's pill; the owner/in_tank columns are what rules that out.
--
-- Rows are `tick x y armour owner in_tank`, one per CHANGE.  x/y are the pill's
-- ORIGINAL tile -- its identity in this file -- not its live position, because a
-- pill that is picked up rides along with the tank that took it.

local TRACE = "lgm_shell_gate_A_trace.log"
local OURS  = { { 127, 125 } }
local NEUTRALS = { { 124, 126 } }
local SPAWN = { 128, 126 }
local GRASS = 7
local TOPUP_ARMOUR = 11          -- 4 down: BUILDER_POOL_TOPUP_MIN_MISSING exactly
local REDAMAGE_AFTER = 16        -- engine ticks a repaired pill is left at full
local FILL_TICK = 60             -- engine ticks: the tank is ashore well before
local p0 = 0

local last = {}
local redamage_at = {}     -- pill index -> tick it gets knocked back down
local filled = false
local traced = nil               -- { {pill_index, tile_x, tile_y}, ... }

local function same(p, t) return p.x == t[1] and p.y == t[2] end

-- Resolve OURS (tile coords) to PILL INDICES, once.  Everything after this
-- reads the pill BY INDEX and never by coordinate: a pill that is picked up
-- moves with the carrier, so a coordinate match would quietly stop finding it
-- at exactly the moment the trace exists to prove nobody took it.  The pillbox
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

function on_setup(g)
  own_everything(g)
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# tick x y armour owner in_tank\n") f:close() end
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
      if last[e[1]] ~= sig then
        local f = io.open(TRACE, "a")
        if f then
          f:write(string.format("%d %d %d %d %d %d\n",
                                tick, e[2], e[3], p.armour, p.owner, in_tank))
          f:close()
        end
        last[e[1]] = sig
      end
    end
  end
end
