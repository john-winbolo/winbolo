-- Scenario sidecar for tests/refuel_topoff_A.map.
--
-- Five jobs.
--
-- 0. THE BOT IS THE ARENA'S OWN, AND IT COMES IN EMPTY.  The three knobs this
--    arena is about are the driver's own -bot-init tokens and the gate runner
--    fields its -bots N with no init, so the arena spawns seat 0 itself and the
--    GATE line asks for bots=0.  REFUEL_TOPOFF_CANDIDATE is OFF by default
--    since 2026-09-06 and SHELLS_LOW moved to 19, so without the pins this
--    arena measures the flag's absence: on main's brain, with no tokens, the
--    tank drank to 30, gave up the emptied base and never went back.
--
--    The EMPTY tank is a loadout policy, not a token.  The driver got it from
--    -gametype tournament plus -ranked, because serverSimApplyScenarioCommit
--    otherwise stamps gameScripted over the game type of any map with a
--    sidecar and gameScripted hands out a full tank.  The gate runner passes no
--    -ranked, so the same thing is asked for directly: spawn_loadout answers
--    "tournament" for seat 0, which outlives a respawn the way the spawn's own
--    loadout field would not.  A tank that starts on 40 shells has drained the
--    base before it has moved and there is no phase 1 at all.
--
-- 1. PIN THE STARTS.  on_choose_start fires for every placement, so player p
--    always gets start p+1: the bot at (126,118) beside the base.  Without it
--    startsGetStartTournament is free to hand the bot any start with an own
--    base within 9 tiles.
--
-- 2. PHASE 1 -- stop the drink at exactly 30 shells.  The moment it reaches 30
--    the base's shells go to 0 and are HELD there until the restock: the engine
--    regenerates base stock by itself (bases.c basesUpdateStock), so seeding the
--    base with 30 and walking away is not enough -- the tank just keeps drinking
--    to 40.  Held at 0, the base can supply nothing the tank needs (its armour
--    is already full), so the brain blocks it as base_useless and the tank
--    leaves with 30 shells: above SHELLS_LOW (20), below the shell target (40).
--
-- 3. PHASE 2 -- 1200 ticks after the drain the base is refilled to 90/90 and
--    re-asserted every 300 ticks, with the ARMOUR value alternating 90/89
--    because the server only emits EVENT_BASE_STOCK when a base's stock
--    actually CHANGES and the bot's observation has to stay fresh.  Both
--    numbers are DOUBLE the old sidecar's: on_tick's tick goes up by 2 per
--    frame on this host and by 1 on the old one, so every duration here means
--    half the wall time it used to unless it is doubled.  The re-assert is on a
--    next-due counter rather than `tick % PERIOD == 0`, because game.tick()
--    holds one parity for a whole round and an even period lands on half the
--    multiples or on none of them.  Mines are held at 0 throughout: the tank
--    must never pick any up, or refuel_shape's exponential mine-hoard surcharge
--    joins the cost and the multiplier stops being hand-computable.
--
-- 4. FILL THE SPAWN PONDS once every tank is ashore.  A start square has to be
--    DEEP SEA, so the arena digs one-tile ponds in the grass; a tank that
--    later drives over one without a boat drowns instantly.

local FULL     = 90
local P1SH     = 30
local SHELL_TARGET = 40          -- the dynamic full target, pinned by the token
local OWNER    = 0
local TEAM     = 1               -- team 0 is NO team on this host, not team zero
local ALLIED   = {  }            -- player slots put on one team (empty in A)
local DRY      = 1200
local PERIOD   = 300
local PONDS    = { { 126, 118 }, { 96, 112 } }
local GRASS    = 7
local POND_TICK = 400

local BOT_BRAIN = "../brains/GoalHunter_1.7/init.lua"
-- The driver's CFG_ON, verbatim.  REFUEL_BASELINE_SHELLS pins state.shell_target
-- at the ordinary 40 in an arena with nothing to shoot at; SHELLS_LOW is pinned
-- at the 20 this arena's numbers were derived for.
local TOKENS = "cfg=REFUEL_BASELINE_SHELLS=40;cfg=SHELLS_LOW=20;" ..
               "cfg=REFUEL_TOPOFF_CANDIDATE=true"

local flip = 0
local next_flip = nil
local allied_done = (#ALLIED == 0)
local ponds_filled = false
local drained = false
local drain_tick = -1
local restocked = false
local base_tile = nil            -- the pad the tank drinks at
local left_pad = false           -- it was off the pad while the base was dry
local best_after = 0             -- most shells seen after the restock

local function set_all(g, armour, shells)
  for i = 1, g.num_bases() do g.set_base_stock(i, armour, shells, 0) end
end

function on_choose_start(g, p)
  local n = p + 1
  if n >= 1 and n <= g.num_starts() then return n end
  return nil
end

-- The empty tank, and it has to outlive a respawn: a spawn's own `loadout`
-- field is spent on the tank the spawn builds and the next life is fuelled the
-- ordinary way, which here would be a full one.
function spawn_loadout(g, p)
  if p == OWNER then return "tournament" end
  return nil
end

function on_setup(g)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      g.set_base_owner(i, OWNER)
      g.set_base_stock(i, FULL, FULL, 0)
      if base_tile == nil then base_tile = { b.x, b.y } end
    end
  end
  g.spawn_bot{ slot = OWNER, name = "Bot", team = TEAM, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
  g.message(string.format(
    "REFUEL_TOPOFF_ARENA pills=%d starts=%d phase1_shells=%d dry_ticks=%d",
    g.num_pills(), g.num_starts(), P1SH, DRY))
end

function on_tick(g, tick)
  -- ONLY the listed slots are allied, and NOT from on_setup: the bots are
  -- still joining there and g.tank(2) is nil, so the ally silently stayed on
  -- its own team and variant B priced exactly like variant A.  Retried from
  -- on_tick until every listed slot has a tank.
  if not allied_done then
    local all = true
    for _, p in ipairs(ALLIED) do
      if g.tank(p) then g.set_team(p, TEAM) else all = false end
    end
    if all then allied_done = true end
  end

  local t = g.tank(OWNER)
  if not drained then
    if t and (t.shells or 0) >= P1SH then
      drained = true
      drain_tick = tick
      set_all(g, FULL, 0)
      g.log(string.format("REFUEL_TOPOFF_A DRAINED t=%d shells=%d",
                          tick, t.shells or -1))
    end
  elseif not restocked then
    if t and base_tile and (t.mx ~= base_tile[1] or t.my ~= base_tile[2]) then
      left_pad = true
    end
    if tick >= drain_tick + DRY then
      restocked = true
      set_all(g, FULL, FULL)
      g.log("REFUEL_TOPOFF_A RESTOCK t=" .. tostring(tick))
    else
      -- Beat the engine's own stock regeneration back down.  Only when it has
      -- actually crept up, so the log is not one base-stock event per tick.
      for i = 1, g.num_bases() do
        local b = g.base(i)
        if b and (b.shells or 0) > 0 then g.set_base_stock(i, FULL, 0, 0) end
      end
    end
  else
    if next_flip == nil then next_flip = tick + PERIOD end
    if tick >= next_flip then
      next_flip = tick + PERIOD
      flip = 1 - flip
      set_all(g, FULL - flip, FULL)
    end
    if t and (t.shells or 0) > best_after then best_after = t.shells end
  end

  if not ponds_filled and tick > POND_TICK then
    local afloat = false
    for p = 0, g.max_tanks() - 1 do
      local tk = g.tank(p)
      if tk and tk.boat then afloat = true end
    end
    if not afloat then
      ponds_filled = true
      for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/refuel_topoff_test.py run A, the part of it
-- the world states: phase 1 really happened (the tank drank to 30 and left the
-- emptied pad), and after the restock it went BACK and filled to the 40-shell
-- target.  That is checks 2 and 5 read off the tank instead of off the log.
--
-- LEFT BEHIND, because every one of them is a REFUEL_SHAPE row in print2:
-- check 3, that each row's printed fill is the min of its own two ratios and
-- its multiplier is 1 + fill^2 x 7 x scarcity; check 4, that a row tagged
-- topoff=on really is between the low line and the target; the ramp climbing
-- with the fill; and check 6, why the tank finally left the pad.  A scenario
-- sees the tank, not the price it put on the trip.
--
-- ALSO LEFT BEHIND, and it has no arena at all: run A2, the same ground with
-- cfg=REFUEL_TOPOFF_CANDIDATE=false, which is what makes this run evidence
-- about the flag.  There is no refuel_topoff_A2.map, so the control would need
-- a new arena file rather than a verdict.
--
-- GATE: ticks=8000 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if not drained then
    return false, "the tank never reached 30 shells: no phase 1"
  end
  if not left_pad then
    return false, "the tank never left the pad while the base was dry"
  end
  if not restocked then
    return false, string.format("the base was never restocked (dry from t=%d)",
                                drain_tick)
  end
  if best_after < SHELL_TARGET then
    return false, string.format("no top-off: shells reached only %d of %d",
                                best_after, SHELL_TARGET)
  end
  return true, string.format("topped off to %d after the restock", best_after)
end
