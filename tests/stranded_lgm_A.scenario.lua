-- Scenario sidecar for tests/stranded_lgm_A.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/stranded_lgm_test.py arena A.
--
-- WHAT THIS SIDECAR IS FOR: it strands the man, once, at a moment the ENGINE
-- has already told us he is on the far side of the gate -- and it does that
-- with the only tool the scenario API has for the job, set_tile.  There is no
-- LGM teleport and the sidecar cannot read the man's position at all, so his
-- whereabouts are inferred from something the engine publishes and the brain
-- cannot fake: OUR PILL'S ARMOUR.  The pool's only errand in this arena is a
-- top-up of the pill at (129,123), four armour down; when game.pill() reports
-- it at full the repair has landed, which means the man walked to that tile
-- and did the work there.  He is, at that instant, two tiles east of the gate.
--
-- THE GATE (128, y=122..124) then goes from FOREST to RIVER, and that is the
-- whole stranding:
--   * bolo_map.h MAP_MANSPEED_TRIVER is 0 -- the man cannot enter river at all,
--     and lgm.c lgmReturn walks a straight line at the tank with a per-axis
--     slide and no re-route, so a full-height column of it stops him for good;
--   * MAP_SPEED_TRIVER is 3 -- the TANK can cross, slowly, paying the water
--     drain.  That asymmetry is deliberate: it leaves the RESCUE possible, so
--     the test can assert the man is actually fetched rather than just that
--     the tank stopped sitting still.
--
-- TWO GUARDS ON THE FLOOD, both of which have to hold on the same tick:
--   * the tank's tile is WEST of the gate column (game.tank().mx), so we never
--     drop a river under the tank.  The tank can never be EAST of it: the only
--     open tile over there holds a LIVE pillbox, which no tank can drive onto;
--   * the pill is at full armour and still OURS and not in anyone's tank -- an
--     armour rise on a pill that changed hands is not our repair.
--
-- AND A RETRY, because the two do not have to coincide first time. The very
-- first measured run had the tank parked ON the gate column at the instant the
-- repair landed (it holds in plan_position wherever it happens to be, which is
-- the freeze this whole arena is about), so the flood was refused -- correctly.
-- When that happens the sidecar knocks the pill straight back down to
-- TOPUP_ARMOUR, the pool sends the man out again, and the next completion is
-- another chance. So the arena keeps asking until the answer is yes instead of
-- betting the run on one coincidence. Once the column is river the pill is
-- left alone for good.
--
-- FILLING THE SPAWN POND.  A start square has to be DEEP SEA at map load
-- (starts.c startsIsValidSquare).  Here the hole is worse than usual: the west
-- room is FOREST so that pillbox.c's tree-hiding stops the neutral ever firing
-- at a parked tank, and a puddle in the middle of it is a tile with no tree
-- cover.  So the pond is filled back to FOREST once the tank is ashore.
--
-- TRACE.  Every change of ARMOUR, OWNER or IN_TANK on our pill, plus the flood
-- itself, is written to stranded_lgm_A_trace.log in SIM ticks, so "the man got
-- across and the ground closed behind him" is asked of the ENGINE and never of
-- the brain's opinion of itself.
--
-- Rows are `tick x y armour owner in_tank`, one per CHANGE, plus one
-- `# flood <tick>` comment line.  x/y are the pill's ORIGINAL tile -- its
-- identity in this file -- not its live position, because a pill that is
-- picked up rides along with the tank that took it.

local TRACE   = "stranded_lgm_A_trace.log"
local OURS    = { { 129, 123 } }
local NEUTRALS = { { 126, 130 } }
local SPAWN   = { 124, 124 }
local GATE_X  = 128
local GATE_Y  = { 122, 123, 124 }
local FOREST  = 5
local RIVER   = 1
local FILL_TICK = 60             -- engine ticks: the tank is ashore well before
local TOPUP_ARMOUR = 11          -- 4 down: BUILDER_POOL_TOPUP_MIN_MISSING exactly
local REDAMAGE_AFTER = 16        -- engine ticks a repaired pill is left at full
local p0 = 0

local last = {}
local filled = false
local flooded = false
local retry_at = nil
local retries = 0
local traced = nil               -- { {pill_index, tile_x, tile_y}, ... }

local function same(p, t) return p.x == t[1] and p.y == t[2] end

local function note(line)
  local f = io.open(TRACE, "a")
  if f then f:write(line .. "\n") f:close() end
end

-- Resolve OURS (tile coords) to PILL INDICES, once.  Everything after this
-- reads the pill BY INDEX and never by coordinate: a pill that is picked up
-- moves with the carrier, so a coordinate match would quietly stop finding it
-- at exactly the moment the trace exists to prove nobody took it.
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
  -- ...but only once the tank is ASHORE and off its boat.  Filling the tile
  -- while the tank is still sitting on it leaves the boat state stuck and the
  -- LGM never becomes available.
  if not filled and tick >= FILL_TICK then
    local tk = g.tank(p0)
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], FOREST)
      note(string.format("# pondfill %d", tick))
      g.message(string.format(
        "STRANDED_LGM_A filled the spawn pond at (%d,%d) with forest at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end

  if not traced then traced = resolve(g) end
  for _, e in ipairs(traced) do
    local p = g.pill(e[1])
    if p then
      -- THE FLOOD.  Full armour on a pill that is still ours and not in a tank
      -- means the man is standing on (129,123), east of the gate.  The tank
      -- has to be west of it on the same tick, or we would be dropping river
      -- under it.  One shot: once the column is river it stays river.
      if not flooded and p.armour >= 15 and not p.in_tank and p.owner == p0 then
        local tk = g.tank(p0)
        if tk and not tk.dead and tk.mx < GATE_X then
          flooded = true
          for _, gy in ipairs(GATE_Y) do
            g.set_tile(GATE_X, gy, RIVER)
          end
          note(string.format("# flood %d tank=(%d,%d)", tick, tk.mx, tk.my))
          g.message(string.format(
            "STRANDED_LGM_A flooded the gate column x=%d at t=%d "
            .. "(tank at (%d,%d), pill full) -- the man is cut off",
            GATE_X, tick, tk.mx, tk.my))
        elseif tk and not tk.dead then
          -- The tank was on or east of the gate at the one instant we knew
          -- where the man was.  Knock the pill back down after a short wait
          -- so the pool sends him out again and we get another instant.
          retry_at = retry_at or (tick + REDAMAGE_AFTER)
          if tick >= retry_at then
            g.set_pill_armour(e[1], TOPUP_ARMOUR)
            retry_at = nil
            retries = retries + 1
            note(string.format("# retry %d n=%d tank=(%d,%d)",
                               tick, retries, tk.mx, tk.my))
          end
        end
      end
      local in_tank = p.in_tank and 1 or 0
      local sig = p.armour .. "/" .. p.owner .. "/" .. in_tank
      if last[e[1]] ~= sig then
        note(string.format("%d %d %d %d %d %d",
                           tick, e[2], e[3], p.armour, p.owner, in_tank))
        last[e[1]] = sig
      end
    end
  end
end
