-- Scenario sidecar for tests/stranded_lgm_B.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/stranded_lgm_test.py arena B (the KEEL control -- identical
-- ground and identical sidecar, two cfg= tokens apart).
--
-- WHAT THIS SIDECAR IS FOR: it strands the man, once, at a moment the ENGINE
-- has already told us he is on the far side of the gate -- and it does that
-- with the only tool the scenario API has for the job, set_tile.  There is no
-- LGM teleport, so his whereabouts are inferred from something the engine
-- publishes and the brain cannot fake: OUR PILL'S ARMOUR.  The pool's only
-- errand in this arena is a top-up of the pill at (129,123), four armour down;
-- when game.pill() reports it at full the repair has landed, which means the
-- man walked to that tile and did the work there.  He is, at that instant, two
-- tiles east of the gate.
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
-- THE BOT IS THE ARENA'S OWN.  PILL_REPOSITION_ENABLED=false is the driver's
-- own -bot-init token and the gate runner fields its -bots N with no init, so
-- the arena spawns seat 0 itself and the GATE line asks for bots=0.  Without
-- it the bot decides its own damaged pill is badly placed, shoots it down and
-- pockets the corpse -- and the arena's only errand vanishes with it.
--
-- FILLING THE SPAWN POND.  A start square has to be DEEP SEA at map load
-- (starts.c startsIsValidSquare).  Here the hole is worse than usual: the west
-- room is FOREST so that pillbox.c's tree-hiding stops the neutral ever firing
-- at a parked tank, and a puddle in the middle of it is a tile with no tree
-- cover.  So the pond is filled back to FOREST once the tank is ashore.
--
-- WHAT THE OLD TRACE FILE ASKED, AND WHERE IT IS ASKED NOW.  The old sidecar
-- wrote the pill's armour, owner and in_tank to stranded_lgm_B_trace.log and
-- the python driver read it back, and read the man and the tank out of the
-- brain's per-tick jsonl.  `io` is not on the scenario sandbox's base list and
-- the jsonl is not the scenario's to read, so all of it comes from the world:
-- the pill by index, the man from game.builder().state, and the tank's
-- stationary spells from game.tank().mx/my.

local OURS    = { { 129, 123 } }
local NEUTRALS = { { 126, 130 } }
local SPAWN   = { 124, 124 }
local GATE_X  = 128
local GATE_Y  = { 122, 123, 124 }
local FOREST  = 5
local RIVER   = 1
-- Both doubled from the old sidecar's 60 and 16: on_tick's tick goes up by 2
-- per frame on this host and by 1 on the old one, so a duration written here
-- means half the wall time it used to unless it is doubled.
local FILL_TICK = 120
local REDAMAGE_AFTER = 32        -- engine ticks a repaired pill is left at full
local TOPUP_ARMOUR = 11          -- 4 down: BUILDER_POOL_TOPUP_MIN_MISSING exactly
-- The driver's own freeze line, in ENGINE ticks, which is what game.tick()
-- counts. It reads against the Everard figure (87510) directly. The measured
-- runs: arena B never moved again, arena A's worst spell was 1866.
local STUCK_ENGINE_TICKS = 3000
local p0 = 0

local BOT_BRAIN = "../brains/GoalHunter_1.7/init.lua"
-- The driver's TOKENS["B"]: the same reposition pin, plus the two knobs of the
-- fix put back to their keel values.  That is the whole difference between this
-- arena and arena A.
local TOKENS = "cfg=PILL_REPOSITION_ENABLED=false;" ..
               "cfg=RESCUE_LGM_SUPPRESS_BY_FIRE_AGE=false;" ..
               "cfg=ATTACK_PP_HOLD_SKIP_STRANDED=false"

local filled = false
local flooded_at = nil
local retry_at = nil
local retries = 0
local traced = nil               -- { { index, x, y }, ... }
local man_home_at = nil          -- first tick the man was aboard after the flood
local man_died = false
local still_since = nil          -- tick the tank last changed tile
local last_tile = nil
local worst_still = 0            -- longest stationary spell since the flood

local function same(p, t) return p.x == t[1] and p.y == t[2] end

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
  -- Queued by the compat prelude and flushed on the round's first tick: a
  -- roster op is refused inside on_setup on this host.
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
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
      if not flooded_at and p.armour >= 15 and not p.in_tank and p.owner == p0 then
        local tk = g.tank(p0)
        if tk and not tk.dead and tk.mx < GATE_X then
          flooded_at = tick
          for _, gy in ipairs(GATE_Y) do
            g.set_tile(GATE_X, gy, RIVER)
          end
          g.log(string.format(
            "STRANDED_LGM_B flooded x=%d at t=%d, tank at (%d,%d), retries=%d",
            GATE_X, tick, tk.mx, tk.my, retries))
        elseif tk and not tk.dead then
          -- The tank was on or east of the gate at the one instant we knew
          -- where the man was.  Knock the pill back down after a short wait
          -- so the pool sends him out again and we get another instant.
          retry_at = retry_at or (tick + REDAMAGE_AFTER)
          if tick >= retry_at then
            g.set_pill_armour(e[1], TOPUP_ARMOUR)
            retry_at = nil
            retries = retries + 1
          end
        end
      end
    end
  end

  -- THE TWO THINGS THE DRIVER READ OUT OF THE BRAIN'S JSONL, read here off the
  -- world: whether the man ever got back aboard, and how long the tank sat on
  -- one tile.  Both only count after the stranding, because before it there is
  -- nothing to be stuck about.
  if flooded_at then
    local bd = g.builder(p0)
    if bd then
      if bd.state == "dead" then man_died = true end
      if bd.state == "in_tank" and not man_home_at then man_home_at = tick end
    end
    local tk = g.tank(p0)
    if tk and not tk.dead then
      local tile = tk.mx * 256 + tk.my
      if tile ~= last_tile then
        last_tile = tile
        still_since = tick
      elseif still_since then
        local spell = tick - still_since
        if spell > worst_still then worst_still = spell end
      end
    else
      last_tile = nil
      still_since = nil
    end
  end
end

-- ── the verdict ─────────────────────────────────────────
-- PORTED (2026-09-15) from tests/stranded_lgm_test.py arena B, the control:
-- with the two knobs back at their keel values the same stranding produces the
-- Everard freeze.  The world says both halves of it -- the man never comes
-- back aboard, and the tank stops moving for longer than the driver's own
-- freeze line -- and that is what makes arena A's rescue evidence about the
-- knobs rather than about an arena nobody could have got stuck in.
--
-- LEFT BEHIND, because it is print2: that the gate reads mode=static,
-- under_fire=true, suppress=true, that rescue_lgm is never picked at all, and
-- that attack_pill prints PP_HOLD_LGM with a held= count that keeps growing.
-- This says the tank froze; only those lines say which of the three waits it
-- froze in.
--
-- GATE: ticks=8200 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if not flooded_at then
    return false, string.format("the gate never flooded (%d retr(ies))", retries)
  end
  if man_home_at ~= nil then
    return false, string.format("the man came home at t=%d: no freeze",
                                man_home_at)
  end
  if worst_still < STUCK_ENGINE_TICKS then
    return false, string.format("tank kept moving: worst spell %d ticks",
                                worst_still)
  end
  return true, string.format("froze: flood t=%d, %d ticks on one tile",
                             flooded_at, worst_still)
end
