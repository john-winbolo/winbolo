-- Scenario script for tests/capture_lgm_priority_P1.map (auto-loaded as <map>.scenario.lua).
-- Companion to tests/capture_lgm_priority_test.py arena P1.
--
-- A bot on capture_pill drives at a dead pillbox to grab it.  The arena keeps
-- one -- and only one -- dead pill on the corpse tile: grabbing it is a single
-- tick of driving over it, so every corpse buys exactly ONE approach, and a
-- hidden spare is popped back onto the tile once the bot has driven clear.
--
-- PORTED (2026-09-15) from the old scenario host.  Four things changed and
-- each of them is marked where it happens: the trace file is gone (there is no
-- io in the sandbox, so the one question the driver asked of it -- how many
-- corpses were taken -- is counted in a local); the arena fields BOTH tanks
-- itself so it can hand the measured bot the driver's cfg pins; every DURATION
-- is doubled because this host's hook clock counts 100 a second where the old
-- one counted 50; and the live corpse is taken off the map before a spare is
-- put back, because this host refuses two pillboxes on one square.

-- The driver's -bot-init tokens, verbatim.  WHY EACH PIN IS THERE, and all
-- three are about keeping the bot ON the errand being measured:
--   TANK_COMBAT_ENABLED=false     the scripted enemy is a visible hostile tank
--     that never fires; without this our bot takes attack_tank and chases it
--     and the capture stops happening.
--   BUILDER_POOL_ENABLED=false    keeps our own man in the tank, so rescue_lgm
--     and wait_for_lgm noise does not land on top of the measurement.
--   STRATEGIC_PLACE_ENABLED=false measured on the old host: with it on the bot
--     planted every pill it captured, once one tile from the corpse, and a
--     live pillbox in the shell's lane is a hard LOS block.  With it off the
--     bot simply keeps what it grabs -- and, on this host, the refill is not
--     then refused for the square already holding a pillbox.
local TOKENS  = "cfg=CAPTURE_LGM_PRIORITY=true;cfg=TANK_COMBAT_ENABLED=false;cfg=BUILDER_POOL_ENABLED=false;cfg=STRATEGIC_PLACE_ENABLED=false"
local OUR_BRAIN = "../brains/GoalHunter/init.lua"
local FOE_BRAIN = "../tests/brains/farm_beside.lua"

local P0, P1  = 0, 1
local NBOTS   = 2
local PULSE   = 0         -- H2: armour an invisible "repair" reaches
local PMODE   = false      -- H2: an armour rise is the POINT, not a spent corpse
local HOLD    = 0          -- H2: ticks the pulse is held (doubled)
local PERIOD  = 0        -- H2: ticks between pulses (doubled)
local AWAY    = 6          -- tiles p0 must be clear before a spare pops
local MAXWAIT = 800       -- ...or this many ticks, whichever first (doubled)
local CORPSE  = { 122, 131 }
local ROADT   = { 128, 126 }
local FARMT   = { 128, 125 }
local SPAWN0  = { 110, 126 }
local SPAWN1  = { 128, 116 }
local SPARES  = { { 108, 118 }, { 108, 122 }, { 108, 126 }, { 108, 130 }, { 108, 134 }, { 144, 118 }, { 144, 122 }, { 144, 126 }, { 144, 130 }, { 144, 134 }, { 112, 118 }, { 112, 134 }, { 140, 118 }, { 140, 134 } }
local GRASS, FOREST = 7, 5
local FILL_TICK = 120      -- doubled

local idx_live   = nil            -- pill index currently playing the corpse
local spare_idx  = {}             -- resolved indices of the hidden spares
local next_spare = 1
local taken_at   = nil            -- tick the live corpse went into a tank
local grabs      = 0              -- what the driver counted `# spent` rows for
local refills    = 0
local filled0, filled1 = false, false
local pulse_at, pulse_off = nil, nil
local pulses     = 0
local spawn_tried = false
local resolved   = false

local function same(p, t) return p.x == t[1] and p.y == t[2] end

-- Resolve tiles to pill INDICES once.  Everything after this reads the pill BY
-- INDEX: a pill that is picked up moves with its carrier, so a coordinate
-- match would stop finding it exactly when the count exists to prove somebody
-- took it.  Resolved ONCE, in on_setup, and never again: hide_pill on this
-- host is a real remove_pill, so a second sweep would find the spares gone.
local function resolve(g)
  spare_idx = {}
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      if same(p, CORPSE) then
        idx_live = i
      else
        for s = 1, #SPARES do
          if same(p, SPARES[s]) then spare_idx[#spare_idx + 1] = i end
        end
      end
    end
  end
  return idx_live ~= nil
end

function on_setup(g)
  resolved = resolve(g)
  -- The corpse belongs to p0 and is DEAD: a free pill lying on the ground,
  -- which is exactly what capture_pill is for.
  if idx_live then
    g.set_pill_owner(idx_live, P0)
    g.set_pill_armour(idx_live, 0)
  end
  -- The spares are refills, not pillboxes: off the map until they are needed.
  -- NEUTRAL FIRST, AND THAT IS NOT COSMETIC.  On the old host hide_pill left
  -- the pill owned and in a tank, so a hidden spare still owned by p0 read to
  -- the brain as a pillbox p0 was CARRYING: five of them gave it five phantom
  -- pillboxes and it spent the whole run on place_pill_strategic.  Ownership
  -- goes back to p0 in the refill.
  for i = 1, #spare_idx do
    g.set_pill_owner(spare_idx[i], g.NEUTRAL)
    g.hide_pill(spare_idx[i])
  end
  for i = 1, g.num_bases() do
    g.set_base_stock(i, 90, 90, 90)
  end
end

-- Both spawns name their start, so this is only the fallback a respawn takes.
function on_choose_start(g, p)
  if p == P0 then return 1 end
  if p == P1 and NBOTS > 1 then return 2 end
  return nil
end

function on_tick(g, tick)
  -- THE ARENA FIELDS ITS OWN TANKS.  The runner's -bots N seats come in with
  -- no init, and the measured bot cannot be measured without the pins above,
  -- so the GATE line asks for bots=0 and both seats are taken here.
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local us, uerr = g.spawn_bot{ slot = P0, name = "Hunter",
                                  brain = OUR_BRAIN, team = 0, start = 1,
                                  init = g.init_tokens(TOKENS) }
    if us == nil then
      g.message("CAPTURE_LGM spawn_bot(us) failed: " .. tostring(uerr))
    end
    if NBOTS > 1 then
      local s, err = g.spawn_bot{ slot = P1, name = "Builder",
                                  brain = FOE_BRAIN, team = 1, start = 2 }
      if s == nil then
        g.message("CAPTURE_LGM spawn_bot(foe) failed: " .. tostring(err))
      end
    end
  end

  -- ALLIANCE.  botManagerSetTeams walks the bots' OWN ClientSim alliance
  -- tables, so a team set before a bot exists reaches the server roster and
  -- nothing else.  Re-applied for a while so it lands whenever the slots come
  -- up.  Team 0 is NO team, which is what makes the two tanks hostile.
  if NBOTS > 1 and tick <= 240 then
    g.set_team(P0, 0)
    g.set_team(P1, 1)
  end

  -- Fill the spawn ponds once each tank is ASHORE and off its boat.  Filling
  -- the tile while the tank still sits on it leaves the boat state stuck and
  -- the LGM never becomes available.
  if tick >= FILL_TICK then
    if not filled0 then
      local tk = g.tank(P0)
      if tk and not tk.boat and not tk.dead
         and (tk.mx ~= SPAWN0[1] or tk.my ~= SPAWN0[2]) then
        filled0 = true
        g.set_tile(SPAWN0[1], SPAWN0[2], GRASS)
      end
    end
    if NBOTS > 1 and not filled1 then
      local tk = g.tank(P1)
      if tk and not tk.boat and not tk.dead
         and (tk.mx ~= SPAWN1[1] or tk.my ~= SPAWN1[2]) then
        filled1 = true
        g.set_tile(SPAWN1[1], SPAWN1[2], GRASS)
      end
    end
  end

  -- KEEP THE ERRANDS ALIVE.  A paved road is not a road job any more and a
  -- chopped tree is gone for good, so without this the enemy's man stops being
  -- sent halfway through the measurement.  Re-stamping both tiles every tick
  -- costs nothing and also repairs a stray shell's damage.
  g.set_tile(ROADT[1], ROADT[2], GRASS)
  g.set_tile(FARMT[1], FARMT[2], FOREST)

  if not resolved then return end

  -- ── ONE PILL ON THE MAP, ALWAYS THE CORPSE ──────────────────────────
  -- Anything lying on the ground that is not the live corpse gets taken off
  -- the map.  That is almost always a pill p0 captured and then PLACED, and a
  -- LIVE pill next to the target is the worst possible scenery here --
  -- init.lua's kill-LGM LOS check treats any live pillbox in the shell's lane
  -- as a block, so the fire half of the feature could never open.
  for i = 1, g.num_pills() do
    if i ~= idx_live then
      local p = g.pill(i)
      if p and not p.in_tank
         and not (p.x == CORPSE[1] and p.y == CORPSE[2]) then
        g.set_pill_owner(i, g.NEUTRAL)
        g.hide_pill(i)
      end
    end
  end

  -- ── THE REFILLING CORPSE ────────────────────────────────────────────
  -- The corpse is SPENT the moment it stops being "dead, on the ground, on the
  -- corpse tile" -- taken into a tank, or replanted elsewhere.  Once it is
  -- spent, wait until p0 has driven AWAY tiles clear (or MAXWAIT ticks,
  -- whichever comes first, so a bot that parks on the tile cannot wedge the
  -- arena) and pop the next spare back onto the corpse tile.  show_pill(n,x,y)
  -- lands it DEAD and on the ground, which is what a capture wants.
  -- PMODE (arena H2) is the exception to "armour > 0 means somebody rebuilt
  -- it, so this corpse is finished": there the pulse IS the measurement, and
  -- treating it as spent refilled a SECOND pill onto the same tile two ticks
  -- into every pulse.
  local live = idx_live and g.pill(idx_live)
  local spent = (live == nil) or live.in_tank
                or (not PMODE and live.armour > 0)
                or live.x ~= CORPSE[1] or live.y ~= CORPSE[2]
  if spent then
    if not taken_at then
      taken_at = tick
      grabs = grabs + 1
    end
    local t0 = g.tank(P0)
    local away = 99
    if t0 and not t0.dead then
      local dx = t0.mx - CORPSE[1]; if dx < 0 then dx = -dx end
      local dy = t0.my - CORPSE[2]; if dy < 0 then dy = -dy end
      away = (dx > dy) and dx or dy
    end
    if next_spare <= #spare_idx
       and (away >= AWAY or (tick - taken_at) >= MAXWAIT) then
      -- TWO PILLBOXES MAY NOT SHARE A SQUARE on this host, and the old one
      -- allowed it.  A corpse that was spent by being REBUILT is still lying
      -- on the corpse tile, so it has to come off the map before the spare
      -- goes on -- otherwise show_pill is refused and the arena runs out of
      -- approaches without ever saying why.
      if live and not live.in_tank
         and live.x == CORPSE[1] and live.y == CORPSE[2] then
        g.set_pill_owner(idx_live, g.NEUTRAL)
        g.hide_pill(idx_live)
      end
      local n = spare_idx[next_spare]
      next_spare = next_spare + 1
      if g.show_pill(n, CORPSE[1], CORPSE[2]) then
        g.set_pill_owner(n, P0)
        g.set_pill_armour(n, 0)
        idx_live = n
        taken_at = nil
        refills = refills + 1
      end
    end
  else
    taken_at = nil
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/capture_lgm_priority_test.py arena P1: the
-- one assertion it made of the ENGINE rather than of print2 -- the bot took
-- at least 1 corpse, counted off the script's own `# spent` rows.  In
-- the driver that check guarded the rest ("the arena did not produce enough
-- approaches to measure anything"); here it is all that is left.
--
-- LEFT BEHIND, because a scenario cannot read the brain's debug output:
-- that the discount applied at all (CAPTURE_LGM_PRIO lines), that cost=A->B
-- was min(A, CAPTURE_LGM_PRIORITY_MAX_COST) and the man named really inside
-- CAPTURE_LGM_PRIORITY_RADIUS, that the discount actually TOOK the goal (a
-- capture_pill -> kill_lgm switch on a tick it was live, which is the whole
-- assertion), that the capture_pill<->kill_lgm switch rate stayed under 40 a
-- minute, and that the sweep stayed silent so P1 against P0 isolates one knob.
-- So a PASS here says the bot still gets its captures and says nothing about
-- the priority option.
--
-- GATE: ticks=24200 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if grabs >= 1 then
    return true, string.format("%d corpse(s) taken, %d refill(s)",
                               grabs, refills)
  end
  return false, string.format("only %d corpse(s) taken of 1, %d refill(s)",
                              grabs, refills)
end
