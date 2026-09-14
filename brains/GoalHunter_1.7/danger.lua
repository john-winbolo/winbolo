local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/danger.lua — shell trajectory prediction + LGM dispatch safety
-- =========================================================================
--
-- Two danger sources are combined in danger_at():
--
--   1. Shell trajectories (Layer 1 — fast decaying)
--      For each visible hostile OBJECT_SHOT, ray-cast forward in 32-WU steps
--      up to 8 map-tile range (64 steps × 32 WU).  Every map cell the shell
--      passes through (and the wall it hits) is marked in shell_map with an
--      expiry of tick + DANGER_DECAY_TICKS_SHELL.  Casting stops at solid
--      terrain (T_BUILDING / T_HALFBUILD).
--
--      NOTE: we always trace the full remaining range; we can't know how far
--      through its life a shell already is.  This is intentionally pessimistic
--      — false positives are cheap, false negatives are fatal for the LGM.
--
--   2. Pill proximity (Layer 2 — persistent)
--      We cannot recover a pillbox's fire *direction* from the brain API
--      (obj.direction encodes health for OBJECT_PILLBOX, not bearing), so
--      omnidirectional pill_danger from pathfinder.lua is used instead.
--      This covers all cells within PILL_RANGE_MAP of any hostile/neutral pill.
--
-- lgm_path_safe() walks the straight-line tank→destination path and returns
-- false if any cell's danger_at value exceeds the caller's threshold.  Three
-- priority thresholds are defined in constants.lua:
--   LGM_DANGER_LOW  (0)  — any danger aborts (normal road / farm)
--   LGM_DANGER_MED  (20) — mild pill danger OK (repair pill)
--   LGM_DANGER_HIGH (80) — only heavy fire aborts (emergency wall / refuel)
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local threat = require("threat")
local print2 = require("print2")
-- ally_state is a leaf module (no requires of its own), so pulling it in here
-- is safe. perception.lua must NOT be required — it requires danger, so that
-- direction is a load cycle; the score functions take `info`/`world`/`state`
-- as parameters instead.
local ally_state = require("ally_state")

local M = {}

-- Sample fractions along a straight LGM path: three interior samples for a
-- path longer than 4 tiles, one midpoint otherwise. Module scope because they
-- are constants -- lgm_path_safe built a fresh table on every call.
local FRACS_LONG  = { 0.25, 0.5, 0.75 }
local FRACS_SHORT = { 0.5 }

-- M.shell_map[mkey] = expires_tick
-- Cells on predicted hostile-shell trajectories this tick.
-- Lives on M (not a closure-private local) so the state serializer can
-- include it in snapshots and replays see the same shell predictions.
M.shell_map = {}

function M.reset()
  -- Mutate in place so any cached references stay valid.
  for k in pairs(M.shell_map) do M.shell_map[k] = nil end
end

-- -------------------------------------------------------------------------
-- Internal: trace all visible hostile shell trajectories
-- -------------------------------------------------------------------------
local function predict_shells(info, tick)
  local tank_mx = bit.rshift(info.tankx, 8)
  local tank_my = bit.rshift(info.tanky, 8)

  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_SHOT and (bit.band(ob.info, OBJECT_HOSTILE)) ~= 0
       and math.abs((bit.rshift(ob.x, 8)) - tank_mx) + math.abs((bit.rshift(ob.y, 8)) - tank_my) <= 10 then
      -- Use floats so we accumulate sub-tile fractions accurately
      local wx = ob.x + 0.0
      local wy = ob.y + 0.0
      -- Per-step displacement: bsin/bcos return [-128,128], divide by 128 to
      -- get a unit vector, then multiply by SHELL_SPEED (32 WU per step).
      local step_x =  U.bsin(ob.direction) * C.SHELL_SPEED / 128
      local step_y = -U.bcos(ob.direction) * C.SHELL_SPEED / 128

      for _ = 1, C.SHELL_MAX_STEPS do
        wx = wx + step_x
        wy = wy + step_y

        -- Out of map bounds?
        if wx < 0x100 or wx > 0xFEFF or wy < 0x100 or wy > 0xFEFF then
          break
        end

        local mx = bit.rshift(math.floor(wx), 8)
        local my = bit.rshift(math.floor(wy), 8)

        -- Mark this cell as dangerous
        local k = U.mkey(mx, my)
        M.shell_map[k] = tick + C.DANGER_DECAY_TICKS_SHELL

        -- Stop at solid terrain (shell impacts here)
        local tt = bit.band(U.traw(mx, my), TERRAIN_MASK)
        if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
          break
        end
      end
    end
  end
end

-- -------------------------------------------------------------------------
-- Internal: expire old shell-map entries (called every 10 ticks)
-- -------------------------------------------------------------------------
local function purge_shell_map(tick)
  for k, exp in pairs(M.shell_map) do
    if tick >= exp then
      M.shell_map[k] = nil
    end
  end
end

-- -------------------------------------------------------------------------
-- Public: combined danger value at a map cell
-- Returns 0..200+  (DANGER_SHELL_IMPACT + pill_danger)
-- -------------------------------------------------------------------------
function M.danger_at(mx, my, tick, world)
  local k = U.mkey(mx, my)
  local shell_val = (M.shell_map[k] and tick < M.shell_map[k])
                    and C.DANGER_SHELL_IMPACT or 0
  local threat_val = threat.at(mx, my)
  return shell_val + threat_val
end

-- -------------------------------------------------------------------------
-- Public: check whether the straight-line path tank→(dest_mx, dest_my)
-- has maximum danger <= threshold at every cell.
-- Returns true  = safe to dispatch LGM
--         false = do not dispatch (danger exceeds threshold somewhere)
-- -------------------------------------------------------------------------
function M.lgm_path_safe(info, dest_mx, dest_my, threshold, tick, world)
  local tx = bit.rshift(info.tankx, 8)
  local ty = bit.rshift(info.tanky, 8)
  local dx = math.abs(dest_mx - tx)
  local dy = math.abs(dest_my - ty)
  local steps = math.max(dx, dy)

  -- Check destination only when tank is already there
  if steps == 0 then
    return M.danger_at(dest_mx, dest_my, tick, world) <= threshold
  end

  for s = 0, steps do
    local t  = s / steps
    local cx = math.floor(tx + (dest_mx - tx) * t + 0.5)
    local cy = math.floor(ty + (dest_my - ty) * t + 0.5)
    if M.danger_at(cx, cy, tick, world) > threshold then
      return false
    end
  end
  return true
end

-- -------------------------------------------------------------------------
-- Enhanced LGM path safety: samples midpoints and handles wall detours.
-- The straight-line check above can miss dangers on the actual LGM path
-- (which routes around walls) or flag tiles the LGM never traverses.
-- This version samples at quarter-points and checks perpendicular offsets
-- when midpoints hit impassable terrain.
-- -------------------------------------------------------------------------
-- excluded_pill_mx/my: optional. When set, the pill at (mx,my) has its
-- own per-tile contribution subtracted from danger_at before the
-- threshold check. Use when dispatching the LGM for a build that's IN
-- this pill's danger footprint while we're committed to killing it
-- (mirrors self_dr in goals.lua: the target shouldn't scare us off
-- its own approach corridor).
local function danger_at_excl(mx, my, tick, world, excl_pcontrib)
  local d = M.danger_at(mx, my, tick, world)
  if excl_pcontrib then
    local contrib = excl_pcontrib[my * 256 + mx]
    if contrib then d = d - contrib end
    if d < 0 then d = 0 end
  end
  return d
end

function M.lgm_path_safe_enhanced(info, dest_mx, dest_my, threshold, tick, world,
                                   excluded_pill_mx, excluded_pill_my)
  local excl_pcontrib = nil
  if excluded_pill_mx and excluded_pill_my then
    excl_pcontrib = threat.pill_contrib[excluded_pill_my * 256 + excluded_pill_mx]
  end

  -- Quick check: destination
  if danger_at_excl(dest_mx, dest_my, tick, world, excl_pcontrib) > threshold then
    return false
  end

  local tx = bit.rshift(info.tankx, 8)
  local ty = bit.rshift(info.tanky, 8)
  local ddx = dest_mx - tx
  local ddy = dest_my - ty
  local dist = math.abs(ddx) + math.abs(ddy)

  if dist <= 1 then
    return danger_at_excl(tx, ty, tick, world, excl_pcontrib) <= threshold
  end

  -- Sample at fractions along the straight line. The two tables are constants
  -- (see FRACS_LONG / FRACS_SHORT at module scope); building them here meant
  -- one table per call, and this is called for every LGM path test.
  local fracs = dist > 4 and FRACS_LONG or FRACS_SHORT
  for _, frac in ipairs(fracs) do
    local sx = math.floor(tx + ddx * frac + 0.5)
    local sy = math.floor(ty + ddy * frac + 0.5)
    local stt = U.ttype(sx, sy)

    if stt == C.T_BUILDING or stt == C.T_HALFBUILD then
      -- LGM goes around walls: check perpendicular offsets
      local len = math.sqrt(ddx * ddx + ddy * ddy)
      local px = len > 0 and math.floor(-ddy / len + 0.5) or 0
      local py = len > 0 and math.floor(ddx / len + 0.5) or 0
      local ok1 = danger_at_excl(sx + px, sy + py, tick, world, excl_pcontrib) <= threshold
      local ok2 = danger_at_excl(sx - px, sy - py, tick, world, excl_pcontrib) <= threshold
      if not (ok1 or ok2) then return false end
    else
      if danger_at_excl(sx, sy, tick, world, excl_pcontrib) > threshold then
        return false
      end
    end
  end

  return true
end

-- -------------------------------------------------------------------------
-- Public: closest-point-of-approach scan of all visible DANGEROUS shells
-- (hostile or neutral — both can damage us) against a point (px, py in WU —
-- typically our own tank).
--
-- For each such OBJECT_SHOT we compute the minimum distance its forward
-- trajectory comes to (px, py) over its remaining flight (clamped to
-- SHELL_MAX_STEPS, same pessimistic full-range assumption as predict_shells —
-- we can't know how far through its life a shell already is). A shell whose
-- closest approach is within `radius` WU counts as "incoming near" — this also
-- covers any shell currently sitting inside the radius (its t*=0 sample is its
-- present position).
--
-- Returns:
--   threatened (bool)  — at least one dangerous shell passes within radius
--   detail     (table) — { {sx,sy, cx,cy, dist, threat}, ... } for the viz.
--                        sx/sy = shell pos, cx/cy = closest-approach point.
--
-- Cheap: a handful of shells, O(1) math each. radius defaults to
-- SWERVE_SHELL_NEAR_WU.
-- -------------------------------------------------------------------------
-- Returns (will_hit_any, detail). For each hostile/neutral shell we compute its
-- closest approach to the tank IN THE TANK'S MOVING FRAME (relative velocity =
-- shell_vel − tank_vel), so a shell we're successfully dodging reads as a MISS.
--   * threat (red) = that relative closest approach lands within SWERVE_HIT_RADIUS_WU
--     of us → it WILL hit given how we're moving. The marker (cx,cy) is the
--     closest-approach point drawn around our CURRENT position, so it sits ON the
--     tank exactly when the shot connects. "no red marker on the tank" = dodged.
--   * radius is just the scan/awareness ring (viz cull); the hit test is tighter.
-- All velocities are WU per SIM STEP (t in sim steps, matching SHELL_SPEED).
function M.shells_incoming_near(info, px, py, radius)
  radius = radius or C.SWERVE_SHELL_NEAR_WU or 400
  local r2 = radius * radius
  local hit_r2 = (C.SWERVE_HIT_RADIUS_WU or 160); hit_r2 = hit_r2 * hit_r2
  local will_hit = false
  local detail = {}
  if not info.objects then return false, detail end
  -- Our OWN velocity from instantaneous heading + speed (a direct engine field —
  -- NO laggy position-delta sampling): the tank advances `engine_speed` WU/tick
  -- (utilCalcDistance), engine_speed = info.speed/4, /2 again for per-sim-step →
  -- info.speed/8. Same angle convention as the shell (bsin_f, -bcos_f: 0=N,64=E).
  local tstep = (info.speed or 0) / 8
  local tvx =  U.bsin_f(info.direction or 0) * tstep
  local tvy = -U.bcos_f(info.direction or 0) * tstep
  for _, ob in ipairs(info.objects) do
    -- Any shell that can damage us counts: hostile AND neutral both hurt our tank
    -- (a neutral pillbox fires on everyone). Our own / friendly shells are safe.
    if ob.type == OBJECT_SHOT
       and ((bit.band(ob.info, OBJECT_HOSTILE)) ~= 0 or (bit.band(ob.info, OBJECT_NEUTRAL)) ~= 0) then
      -- RELATIVE velocity (shell − tank), WU per sim step.
      local vx =  U.bsin_f(ob.direction) * C.SHELL_SPEED - tvx
      local vy = -U.bcos_f(ob.direction) * C.SHELL_SPEED - tvy
      local r0x = ob.x - px
      local r0y = ob.y - py
      local vv  = vx * vx + vy * vy
      -- t* = projection of -r0 onto the RELATIVE velocity, clamped to remaining flight.
      local t = 0
      if vv > 0 then
        t = -(r0x * vx + r0y * vy) / vv
        if t < 0 then t = 0 elseif t > C.SHELL_MAX_STEPS then t = C.SHELL_MAX_STEPS end
      end
      -- Closest-approach OFFSET from us in our moving frame; |offset| = miss dist.
      -- Marker = our pos + offset → lands ON the tank when the shell will hit.
      local offx = r0x + vx * t
      local offy = r0y + vy * t
      local d2 = offx * offx + offy * offy
      if d2 <= r2 then   -- within the scan ring → show it + test for a hit
        local is_hit = d2 <= hit_r2
        if is_hit then will_hit = true end
        detail[#detail + 1] = {
          sx = ob.x, sy = ob.y, cx = px + offx, cy = py + offy,
          dist = math.sqrt(d2), threat = is_hit,
        }
      end
    end
  end
  return will_hit, detail
end

-- =========================================================================
-- lgm_shell_gate — the HARD STOP on sending the man into a shell already in
-- the air (2026-09-06, author's rule, verbatim):
--
--   "if we know we can predict shells for at most 63 ticks, let's do that,
--    and if any shell will kill our builder (predict the builder for 63 ticks
--    also) then we should HARD STOP sending it out right then. shells and
--    builder are very predictable so this is worth doing. I realize a tank can
--    impact reality quicker than 63 ticks but it's a good start."
--
-- WHY 63, AND IN WHAT UNIT. shells.c shellLifeTicks is
-- `1 + 8 x range_tiles - 6` ENGINE ticks; a pillbox fires at
-- PILLBOX_FIRE_DISTANCE 8.5 tiles, so 1 + 68 - 6 = 63 is the longest a shell
-- can possibly live, and a tank's own shot lives 51. EVERY tick count in this
-- function is an ENGINE tick -- the shell moves SHELL_SPEED (32 WU) per engine
-- tick and the LGM walk sim counts engine ticks too. The brain thinks every
-- SECOND engine tick, so `now` (a brain tick) never appears in here.
--
-- WHAT ACTUALLY KILLS THE MAN (src/bolo/lgm.c lgmDeathCheckAtPosition:1264,
-- reached from shells.c only when a shell EXPLODES -- lines 368 and 452, the
-- collision path and the end-of-life path). Given the explosion at (wx,wy):
--
--   solid = the explosion tile is BUILDING / HALFBUILDING / has a pillbox /
--           has a base;
--   not solid -> the man dies if euclidean dist(man, explosion) <=
--           MAP_SQUARE_MIDDLE (128 WU, half a tile);
--   solid    -> he dies only if his TILE is the explosion tile (he can stand
--           on a base or on the pill he is working on; he can never stand in
--           a wall).
--
-- A SHELL FLYING THROUGH THE MAN DOES NOT TOUCH HIM. That single fact is the
-- shape of this whole function: the question is not "does the shell cross his
-- path" (it usually does, and answering that would refuse every dispatch on a
-- shelled lane forever) -- it is "does the shell END where he is standing".
-- So each shell is flown forward and its ENDING is what gets tested, and a
-- shell has exactly three ways to end (shells.c shellsUpdate/shellsCalcCollision):
--
--   1. it reaches a solid tile -- a wall, or a LIVE pillbox. The engine
--      recentres the explosion on that tile, so only a man standing ON that
--      tile dies. Stop tracking.
--   2. it reaches OUR TANK's hull (within TANK_HIT_RADIUS, tank.h:102 = 112 WU)
--      and we did not fire it -- a shell never hits its own owner. It explodes
--      where it is, over open ground, so the 128-WU rule applies. This is the
--      case that matters most: a round aimed at the tank, landing on the hull,
--      with the man who has just stepped off it standing 60 WU away. Stop.
--   3. it runs out of life -- ob.life ticks from now, exactly -- and explodes
--      where it happens to be. 128-WU rule.
--
-- The tank is assumed to hold its ground for the window, which is the same
-- assumption the man's walk already makes (it starts from where the tank is
-- now). A tank that drives off makes the gate pessimistic, never blind.
--
-- Bases are deliberately NOT treated as stoppers even though the engine can
-- explode a shell on one (basesCanHit depends on the shell's owner, which
-- would have to be resolved against team state here). Flying on THROUGH a base
-- tile can only ever add a refusal, never miss one, so the error is on the
-- safe side.
--
-- WHAT THE BRAIN CAN SEE OF A SHELL (brain_data.c, bot branch):
--   ob.x, ob.y     world position this tick
--   ob.angle       the EXACT 8-bit bradian heading (ob.direction is the old
--                  16-compass-point snap, +-11.25 deg, and is NOT used here)
--   ob.life        remaining flight in engine ticks
--   ob.owner       firing player number, 0xFF for a pillbox
--   ob.info        HOSTILE / NEUTRAL bits; 0 = friendly
-- Friendly shells are propagated too: a shell is a shell, and our own round
-- landing on the man kills him exactly like anyone else's.
--
-- WHERE THE MAN WILL BE: cpf_lgm_walk_path, which is the SAME simulation
-- lgm_trip prices the trip with (brain_pathfinder.c lgmTravelTicksCore), asked
-- for its first LGM_SHELL_PREDICT_TICKS positions. He starts at the tank's
-- TILE CENTRE, like every other lgm_trip call, and leaves on dispatch+1 -- the
-- one-tick offset is at most 16 WU of walk against a 128 WU kill radius, so
-- tick t of the walk is read as tick t of the shell. The window covers the
-- WHOLE errand where the errand fits in it -- out, the LGM_BUILD_TIME stand on
-- the target, and the walk home -- see man_at.
--
-- WHAT IT STILL CANNOT SEE, and the author already said so: "a tank can impact
-- reality quicker than 63 ticks but it's a good start." Only shells ALREADY IN
-- THE AIR are propagated. A pillbox that reloads while the man is out fires a
-- round this gate was never shown, and the tank is assumed to hold its ground.
-- The gate is a filter on what is knowable now, not a guarantee.
-- =========================================================================

-- Reused across calls: the walk path comes back as a flat pair list
-- (x1,y1,x2,y2,...) written into this table by the C sim, so a gate that runs
-- every tick a dispatch is possible allocates nothing.
local WALK = {}

-- Where the man is at engine tick `t` of the errand, from the walk WALK holds.
--
-- THE WHOLE ERRAND, not just the walk out. A short trip fits inside the
-- window twice over -- one tile of grass is 16 ticks each way and the build is
-- LGM_BUILD_TIME (20), so an errand next door is 52 of the 63 ticks -- and
-- modelling him as STANDING on the target for the rest of the window is simply
-- wrong about where he will be. It is also the half that gets him killed: the
-- first arena run of this feature lost the man on his way HOME, to a round the
-- gate had not been asked about because its model had him parked at the pill.
--
--   t in [1, nw]                  outbound, WALK[t]
--   t in (nw, nw + dwell]         standing on the target, WALK[nw]
--   t after that                  the same steps in reverse, back to the tank
--   nw == 0 (adjacent target, or a first tile he cannot enter)  the tank tile
local function man_at(nw, dwell, t, home_x, home_y)
  if nw < 1 then return home_x, home_y end
  local i
  if t <= nw then
    i = t
  elseif t <= nw + dwell then
    i = nw
  else
    i = nw - (t - nw - dwell)
    if i < 1 then return home_x, home_y end     -- home, and staying there
  end
  return WALK[i * 2 - 1], WALK[i * 2]
end

-- Shell class name for the reason string / the deny key. There is no shell
-- IDENTITY in the brain API (idnum carries angle+owner, not a serial), so the
-- best a reason line can name is who fired it.
local function shell_src(ob)
  local owner = ob.owner or 0
  if owner == 255 then return "pill" end
  return "p" .. owner
end

-- Is (mx,my) a tile a shell explodes ON, in the engine's sense? Two of
-- shellsCalcCollision's three terrain-ish stoppers:
--   * impassable terrain -- BUILDING / HALFBUILDING (mapIsPassable);
--   * a LIVE pillbox -- pillsIsPillHit requires armour > 0 and inTank == FALSE,
--     so a CORPSE does not stop a shell (which is exactly the tile the man is
--     usually walking to) and neither does one riding in somebody's tank.
-- Bases are left out on purpose -- see the header. Owner does not enter into
-- the pill test: pillsIsPillHit stops a shell on ANY live pill, including its
-- own owner's.
local function shell_stops_on(world, mx, my)
  local tt = U.ttype(mx, my)
  if tt == C.T_BUILDING or tt == C.T_HALFBUILD then return true end
  local lst = world and world.pill_at and world.pill_at[my * 256 + mx]
  if lst then
    for i = 1, #lst do
      local p = lst[i].pill
      if p and (p.health or 0) > 0 and not p.in_tank then return true end
    end
  end
  return false
end

-- M.lgm_shell_gate(world, info, dest_mx, dest_my)
--   -> nil                 nothing in the air can reach him
--   -> hit (table)         REFUSE. Fields, all of which the reason line and
--                          the overlay are built from:
--        t       ENGINE ticks from now at which it lands
--        sx, sy  the shell's world position then (the impact point)
--        lx, ly  the man's predicted world position then
--        src     "p3" (a player) / "pill" (a NEUTRAL pillbox)
--        how     which of the three endings: "wall" (a wall or live pill),
--                "tank" (our hull), "expiry" (out of life), or the
--                should-not-happen "unknown_life"
--        solid   true when it was the same-tile rule that fired ("wall")
--        walk_n  how many walk steps were simulated
function M.lgm_shell_gate(world, info, dest_mx, dest_my)
  if not C.BUILDER_POOL_SHELL_GATE then return nil end
  local objs = info.objects
  if not objs then return nil end

  -- Cheapest possible bail: no shell anywhere in view, no walk sim at all.
  -- ipairs, not `#objs`: the object array is a PERSISTENT table the host
  -- overwrites in place and nils the tail of, so its length operator has no
  -- guaranteed answer -- ipairs stops at the first nil, which is the count.
  local any = false
  for _, ob in ipairs(objs) do
    if ob.type == OBJECT_SHOT then any = true; break end
  end
  if not any then return nil end

  local N = C.LGM_SHELL_PREDICT_TICKS or 63
  local KILL = C.LGM_SHELL_KILL_RADIUS_WU or 128
  local KILL2 = KILL * KILL
  local TANK_HIT2 = (C.TANK_HIT_RADIUS_WU or 112) ^ 2
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)

  -- The man's walk, same sim and same blessed destination as M.lgm_trip.
  local nw = cpf_lgm_walk_path(tmx, tmy, dest_mx, dest_my, dest_mx, dest_my,
                               N, C.BUILDER_POOL_LGM_STUCK_TICKS or 150, WALK)
  local dwell = C.LGM_BUILD_TIME or 20
  -- No step at all (adjacent target, blocked first tile): he is still a man
  -- standing on the tank's tile, which is a position worth testing.
  local home_x = tmx * 256 + 128
  local home_y = tmy * 256 + 128

  for _, ob in ipairs(objs) do
    if ob.type == OBJECT_SHOT then
      -- The shell's own clock. `life` is the engine's `length` -- the exact
      -- number of ticks it has left -- so the expiry is a KNOWN tick, not a
      -- range of guesses. A shell we somehow have no life for is treated as
      -- able to expire on ANY tick of the window instead: pessimistic, never
      -- blind. (It should not happen; a snapshot shell always has length >= 1.)
      local life = ob.life or 0
      local known_life = life > 0
      -- The tick it explodes of its own accord, or nil when that is past the
      -- horizon (it cannot be, at N = 63: shellLifeTicks caps there. Written
      -- so that lowering LGM_SHELL_PREDICT_TICKS cannot invent an expiry at
      -- the edge of the window for a shell that is still flying).
      local expires_at = (known_life and life <= N) and life or nil
      if not known_life or life > N then life = N end
      local ours = (ob.owner == info.player_number)
      local vx =  U.bsin_f(ob.angle or 0) * C.SHELL_SPEED
      local vy = -U.bcos_f(ob.angle or 0) * C.SHELL_SPEED
      local sx, sy = ob.x + 0.0, ob.y + 0.0
      for t = 1, life do
        sx = sx + vx
        sy = sy + vy
        if sx < 0x100 or sx > 0xFEFF or sy < 0x100 or sy > 0xFEFF then break end
        local smx = bit.rshift(math.floor(sx), 8)
        local smy = bit.rshift(math.floor(sy), 8)
        local lx, ly = man_at(nw, dwell, t, home_x, home_y)
        -- (1) a wall or a live pill: the engine recentres the explosion on
        -- that tile, so only a man standing ON it dies.
        if shell_stops_on(world, smx, smy) then
          if bit.rshift(lx, 8) == smx and bit.rshift(ly, 8) == smy then
            return { t = t, sx = smx * 256 + 128, sy = smy * 256 + 128,
                     lx = lx, ly = ly, src = shell_src(ob), how = "wall",
                     solid = true, walk_n = nw }
          end
          break
        end
        local dx = sx - lx
        local dy = sy - ly
        local near_man = (dx * dx + dy * dy) <= KILL2
        -- (2) our own hull. A shell never hits the tank that fired it, so our
        -- own rounds fly straight through.
        if not ours then
          local hx = sx - info.tankx
          local hy = sy - info.tanky
          if hx * hx + hy * hy < TANK_HIT2 then
            if near_man then
              return { t = t, sx = sx, sy = sy, lx = lx, ly = ly,
                       src = shell_src(ob), how = "tank", solid = false,
                       walk_n = nw }
            end
            break
          end
        end
        -- (3) end of life, over open ground.
        if near_man and (not known_life or t == expires_at) then
          return { t = t, sx = sx, sy = sy, lx = lx, ly = ly,
                   src = shell_src(ob), how = known_life and "expiry" or "unknown_life",
                   solid = false, walk_n = nw }
        end
      end
    end
  end
  return nil
end

-- Snapshot of the walk the last gate call simulated, for the overlay. Copied
-- ONLY when the gate actually refuses (rare), so the common path stays free.
function M.lgm_shell_gate_walk(hit)
  local out = {}
  local n = hit and hit.walk_n or 0
  for i = 1, n * 2 do out[i] = WALK[i] end
  return out, n
end

-- -------------------------------------------------------------------------
-- Public: call once per tick (after world.update, before build decisions)
-- -------------------------------------------------------------------------
function M.update(info, tick)
  predict_shells(info, tick)
  if tick % 10 == 0 then
    purge_shell_map(tick)
  end
end

-- -------------------------------------------------------------------------
-- Sustained "the TANK is in a firefight" clock (builder pool, 2026-09-02).
--
-- perc.under_fire is a SINGLE-TICK test -- "the danger field at our tile is
-- non-zero" -- and it is the wrong question for deciding whether to let the
-- LGM out. It is true through the quiet minutes of a standoff with a calm pill
-- in range, and it is false in the gap between two shells that are both aimed
-- at us. The man's risky moments are the two ENDS of his trip (splash at
-- departure, splash at return), so what matters is whether anything has
-- actually connected or is genuinely inbound RECENTLY.
--
-- Two pieces of evidence, both of which the brain already keeps:
--   * armour dropped since last tick (init.lua's state.took_damage_this_tick),
--   * a hostile/neutral shell whose closest approach IN OUR MOVING FRAME lands
--     inside SWERVE_HIT_RADIUS_WU -- i.e. shells_incoming_near's own hit test,
--     the same one the swerve uses to decide a shell WILL connect. A shell we
--     are successfully dodging reads as a miss and does not restart the clock.
--
-- update_fire_clock stamps state._tank_fire_tick on either; tank_fire_age
-- reports how long ago that was (nil = never, this game/life). Deterministic:
-- game ticks only, no wall clock.
-- -------------------------------------------------------------------------
function M.update_fire_clock(state, info, tick)
  if state._tank_fire_clock_tick == tick then return end
  state._tank_fire_clock_tick = tick
  local why = nil
  if state.took_damage_this_tick then
    why = "armour"
  else
    local will_hit = M.shells_incoming_near(info, info.tankx, info.tanky,
                                            C.SWERVE_SHELL_NEAR_WU)
    if will_hit then why = "shell" end
  end
  if why then
    state._tank_fire_tick = tick
    state._tank_fire_why  = why
  end
end

-- Ticks since the last hit / inbound shell, plus which of the two it was.
-- nil when nothing has ever hit us (a fresh spawn, a quiet game).
function M.tank_fire_age(state, tick)
  local t = state._tank_fire_tick
  if not t then return nil, nil end
  return tick - t, state._tank_fire_why
end

-- =========================================================================
-- vulnerability / imdanger — the two build scores
-- =========================================================================
-- Both are 0-100, clamped, and HIGHER IS BETTER. 50 is neutral. They exist so
-- the decision "should I dump a pill right now" reads two separate questions
-- instead of one armour threshold:
--
--   vulnerability — what I stand to lose (armour, cargo). Intrinsic.
--   imdanger      — what is arriving at me (cover, exposure, shells, odds).
--                   Environmental; says nothing about our own state.
--
-- Both return the score AND a terms table, because every printed breakdown
-- has to let the final number be recomputed by hand. All distances Euclidean,
-- in map tiles; boundary values belong to the closer, higher-weighted band.
-- =========================================================================

local function clamp01_100(v)
  if v < 0 then return 0 elseif v > 100 then return 100 end
  return v
end

-- Carry term: 0 pills is the safest we get (nothing to lose), 1 pill is
-- neutral, and it falls linearly to the floor at VULN_CARRY_SATURATE.
local function carry_term(n)
  if n <= 0 then return C.VULN_CARRY_EMPTY end
  local span = C.VULN_CARRY_SATURATE - 1
  if span <= 0 then return C.VULN_CARRY_FULL end
  local t = (n - 1) / span
  if t > 1 then t = 1 end
  return C.VULN_CARRY_FULL * t
end

-- M.vulnerability(info) -> score, terms
function M.vulnerability(info)
  local armour = info.armour or 0
  if armour > C.VULN_ARMOUR_CAP then armour = C.VULN_ARMOUR_CAP end
  local t_armour = (armour / C.VULN_ARMOUR_CAP) * C.VULN_ARMOUR_SPAN
                   - (C.VULN_ARMOUR_SPAN / 2)
  local pills    = info.carried_pills or 0
  local t_carry  = carry_term(pills)
  local score    = clamp01_100(50 + t_armour + t_carry)
  return score, { armour = t_armour, carry = t_carry,
                  armour_raw = info.armour or 0, pills = pills }
end

-- Distance weight for the cover term. A pillbox's real reach is 8 tiles
-- (PILLBOX_RANGE 2048 WU / 256). The 8..9 band is PILL_RANGE_MAP's deliberate
-- one-tile margin, kept as a small credit for a pill that nearly covers us.
local function cover_weight(d)
  if d <= 4 then return C.IMD_COVER_W_NEAR end
  if d <= 8 then return C.IMD_COVER_W_MID end
  if d <= 9 then return C.IMD_COVER_W_FAR end
  return 0
end

local function odds_weight(d)
  if d <= C.IMD_ODDS_NEAR_TILES then return C.IMD_ODDS_W_NEAR, true end
  if d <= C.IMD_ODDS_FAR_TILES  then return C.IMD_ODDS_W_FAR,  false end
  return 0, false
end

-- Published so goals.lua's take_cover scorer prices a CANDIDATE TILE with the
-- exact same distance bands imdanger prices the tank's own tile with. Two
-- copies of these ladders would drift and the panel numbers would stop
-- reconciling with the SCORES line.
M.cover_weight = cover_weight
M.odds_weight  = odds_weight

-- Is some ally already engaging enemy tank `id`? Read from the /info state
-- slate. Two guards matter: gate on goal == "attack_tank" FIRST, because
-- `target` is a PILL id when the ally's goal is attack_pill (an ally attacking
-- pill #3 would otherwise suppress enemy tank player 3), and only trust a
-- slate fresh enough to still describe reality.
local function enemy_is_ally_engaged(now, id)
  if id == nil then return false, nil, nil end
  for pn, slot in ally_state.iter_active(now, C.SCORE_ALLY_MAX_AGE) do
    local inf = slot.info
    if inf and inf.goal == "attack_tank" then
      local tgt = tonumber(inf.target)
      if tgt ~= nil and tgt == id then
        -- Age reported so a suppression traced in the log can be judged: a
        -- slate is only as good as how recently the ally sent it.
        return true, pn, now - (slot.last_tick or now)
      end
    end
  end
  return false, nil, nil
end

-- M.imdanger(info, world, state) -> score, terms
--
-- `state` supplies only state.tick (for slate freshness). `world` supplies
-- world.pills for the cover term. Neither is required as a module.
function M.imdanger(info, world, state)
  local now = (state and state.tick) or 0
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)

  -- cover (0 .. +IMD_COVER_MAX): our own pillboxes protecting us. They shoot
  -- at whatever is attacking us, so being inside one's range is real cover.
  -- Friendly AND allied, deployed only. A heated pill counts triple because
  -- the engine halves its reload per hit.
  local units, n_cover, n_heated = 0, 0, 0
  local cover_pills = nil   -- viz: every contributing pill with its units
  for _, p in pairs((world and world.pills) or {}) do
    if (p.owner == "friendly" or p.owner == "allied")
       and not p.in_tank and (p.health or 0) > 0 then
      local d = U.edist(tmx, tmy, p.mx, p.my)
      local w = cover_weight(d)
      if w > 0 then
        local hot = (p.anger or 0) >= C.HEATED_ANGER
        local u   = w * (hot and C.IMD_COVER_HEATED_MULT or 1)
        units   = units + u
        n_cover = n_cover + 1
        if hot then n_heated = n_heated + 1 end
        if BRAIN_DEBUG_MODE then
          cover_pills = cover_pills or {}
          cover_pills[#cover_pills + 1] = { mx = p.mx, my = p.my, d = d, u = u, hot = hot }
        end
      end
    end
  end
  local t_cover = units * C.IMD_COVER_PER_UNIT
  if t_cover > C.IMD_COVER_MAX then t_cover = C.IMD_COVER_MAX end

  -- exposure (0 .. IMD_EXPOSURE_MAX): a hostile pill has our tile in range, so
  -- this is bad ground to linger on. threat.pill_at, NOT threat.at or
  -- danger_at -- those fold in tanks and shell stamps, counted in their own
  -- terms below. Note this only really registers once a pill is ANGRY (anger
  -- rises on damage taken, never from firing at us), which is correct rather
  -- than a gap: a calm pill's threat to us IS its shells, and the shells term
  -- already counts those. This term means "ground covered by a pill that is
  -- firing fast".
  local pill_at  = threat.pill_at(tmx, tmy) or 0
  local expo_frac = pill_at / C.IMD_EXPOSURE_DIV
  if expo_frac > 1 then expo_frac = 1 end
  local t_expo = C.IMD_EXPOSURE_MAX * expo_frac

  -- shells (0 / ONE / MANY): hostile shots arriving near us.
  local _, sdetail = M.shells_incoming_near(info, info.tankx, info.tanky,
                                            C.IMD_SHELLS_RADIUS_WU)
  local n_shells = 0
  for i = 1, #sdetail do
    if (sdetail[i].dist or 1e9) <= C.IMD_SHELLS_RADIUS_WU then
      n_shells = n_shells + 1
    end
  end
  local t_shells = 0
  if n_shells >= 2 then t_shells = C.IMD_SHELLS_MANY
  elseif n_shells == 1 then t_shells = C.IMD_SHELLS_ONE end

  -- odds (0 .. IMD_ODDS_MAX): are we outnumbered by tanks right now. Our own
  -- tank is NOT in info.objects, so nothing is added for self -- this measures
  -- who is around me, not headcount. (perc.allied_tank_count does `+ 1 = us`
  -- and cannot be reused here.)
  local near_net, far_net = 0, 0
  local n_enemy, n_ally, n_skipped = 0, 0, 0
  local skipped, counted = nil, nil
  if info.objects then
    for _, ob in ipairs(info.objects) do
      if ob.type == OBJECT_TANK then
        local omx, omy = bit.rshift(ob.x, 8), bit.rshift(ob.y, 8)
        local d = U.edist(tmx, tmy, omx, omy)
        local w, is_near = odds_weight(d)
        if w > 0 then
          if (bit.band(ob.info, OBJECT_HOSTILE)) ~= 0 then
            local engaged, by, age = enemy_is_ally_engaged(now, ob.idnum)
            if engaged then
              n_skipped = n_skipped + 1
              if BRAIN_DEBUG_MODE then
                skipped = skipped or {}
                skipped[#skipped + 1] = { id = ob.idnum, d = d, by = by, age = age,
                                          mx = omx, my = omy }
              end
            else
              n_enemy = n_enemy + 1
              if is_near then near_net = near_net + w else far_net = far_net + w end
              if BRAIN_DEBUG_MODE then
                counted = counted or {}
                counted[#counted + 1] = { id = ob.idnum, d = d, w = w,
                                          near = is_near, foe = true,
                                          mx = omx, my = omy }
              end
            end
          else
            n_ally = n_ally + 1
            if is_near then near_net = near_net - w else far_net = far_net - w end
            if BRAIN_DEBUG_MODE then
              counted = counted or {}
              counted[#counted + 1] = { id = ob.idnum, d = d, w = w,
                                        near = is_near, foe = false,
                                        mx = omx, my = omy }
            end
          end
        end
      end
    end
  end
  local t_near = -near_net * C.IMD_ODDS_SCALE
  if t_near > 0 then t_near = 0 elseif t_near < C.IMD_ODDS_MAX then t_near = C.IMD_ODDS_MAX end
  local t_far = -far_net * C.IMD_ODDS_SCALE
  if t_far > 0 then t_far = 0 elseif t_far < C.IMD_ODDS_FAR_MAX then t_far = C.IMD_ODDS_FAR_MAX end
  local t_odds = t_near + t_far
  if t_odds > 0 then t_odds = 0 elseif t_odds < C.IMD_ODDS_MAX then t_odds = C.IMD_ODDS_MAX end

  local score = clamp01_100(50 + t_cover + t_expo + t_shells + t_odds)
  return score, {
    cover = t_cover, exposure = t_expo, shells = t_shells, odds = t_odds,
    cover_units = units, n_cover = n_cover, n_heated = n_heated,
    pill_at = pill_at, n_shells = n_shells,
    near = t_near, far = t_far, near_net = near_net, far_net = far_net,
    n_enemy = n_enemy, n_ally = n_ally, n_skipped = n_skipped,
    skipped = skipped, counted = counted, cover_pills = cover_pills,
  }
end

-- =========================================================================
-- The panic build trigger
-- =========================================================================
-- Dump a pill into the ground NOW, because we are about to lose what we are
-- carrying. Replaces a bare armour threshold: armour alone said nothing about
-- whether anything was actually threatening us, which is how a bot at armour
-- 10 with no enemy in sight dumped four pills in 200 ticks.
--
--   threshold = min(35, 50 - vulnerability * 0.8)
--   panic     = carrying and builder aboard and not in a boat
--               and vulnerability <= 50 and imdanger <= threshold
--
-- The sliding threshold is the whole idea: the more we stand to lose, the less
-- arriving danger it takes to justify banking it. The cap at 35 is
-- load-bearing -- anything reading 40 or above can never panic at any armour,
-- which permanently excludes quiet ground (50) and distant-only outnumbering
-- (40). Without it a nearly-dead bot would panic on an empty field.
function M.panic_threshold(vuln)
  local t = 50 - vuln * 0.8
  if t > 35 then t = 35 end
  return t
end

-- M.should_panic_build(state, info) -> bool, threshold, why
--
-- NO HYSTERESIS, deliberately. If conditions improve the bot should stop
-- panicking. A panic build is a single tick -- pick_goal, set_mode and decide
-- all run in the same brain tick, so it fires, picks a spot at ring 1-5 and
-- dispatches before the tick ends -- after which man_status is no longer
-- LGM_INTANK and it cannot re-fire until the builder is home. That bounds it
-- at one pill per builder round trip whatever the scores do meanwhile, which
-- is what hysteresis would have been protecting, so there is nothing left for
-- it to buy.
function M.should_panic_build(state, info)
  local carrying = (info.carried_pills or 0) >= 1
  -- Carrying is NOT implied by the scores and has to be stated: at armour 0
  -- with no pills, vulnerability is 50 + (-25) + 25 = exactly 50, which passes
  -- the inclusive gate -- so under fire an empty tank would panic with nothing
  -- to place, electing a cost-1 goal that dead-ends at no dispatch. Carrying
  -- nothing RAISES vulnerability, because having nothing to lose is safer.
  if not carrying then return false, nil, "not_carrying" end
  if info.man_status ~= C.LGM_INTANK then return false, nil, "lgm_out" end
  if info.inboat then return false, nil, "inboat" end

  return M.panic_scores(state)
end

-- M.panic_scores(state) -> bool, threshold, why
--
-- The SCORE half of should_panic_build, with the builder/carry/boat gates
-- left out. should_panic_build calls it after its own gates, so the two can
-- never disagree about the numbers; take_cover calls it directly to ask "do
-- the scores say panic?" for the case where the answer is yes but the panic
-- REACTION (a build) is impossible -- lgm_out / not_carrying / inboat. That
-- combination is exactly incident A: panic true, no builder, nothing happened.
function M.panic_scores(state)
  local v = state.vuln
  local i = state.imdanger
  if v == nil or i == nil then return false, nil, "no_scores" end
  if v > 50 then return false, nil, "vuln_ok" end

  local thresh = M.panic_threshold(v)
  if i > thresh then return false, thresh, "imdanger_ok" end
  return true, thresh, "panic"
end

-- M.scores(info, world, state) -> vuln, imd, vterms, iterms
-- Computes both and logs the full breakdown. Kept as one call so the log line
-- is emitted once per tick with both halves, rather than twice out of order.
function M.scores(info, world, state)
  local v, vt = M.vulnerability(info)
  local i, it = M.imdanger(info, world, state)
  print2(string.format(
    "SCORES t=%d vuln=%.1f = 50 + arm{%+.1f}(%d/%d) + carry{%+.1f}(%d pills)",
    (state and state.tick) or 0, v, vt.armour, vt.armour_raw,
    C.VULN_ARMOUR_CAP, vt.carry, vt.pills))
  -- near_net/far_net are printed alongside the clamped contributions so odds is
  -- derivable, not merely checkable: near = clamp(-35,0, -near_net * SCALE).
  print2(string.format(
    "       imdanger=%.1f = 50 + cover{%+.1f}(%.2fu %dp %dhot) + expo{%+.1f}(pill_at=%.0f)"
    .. " + shells{%+.1f}(%d near) + odds{%+.1f}(near{%+.1f}=net %.2f, far{%+.1f}=net %.2f;"
    .. " %de %da %dskip)",
    i, it.cover, it.cover_units, it.n_cover, it.n_heated, it.exposure, it.pill_at,
    it.shells, it.n_shells, it.odds, it.near, it.near_net, it.far, it.far_net,
    it.n_enemy, it.n_ally, it.n_skipped))
  if it.counted then
    for _, t in ipairs(it.counted) do
      print2(string.format("       ODDS %-5s #%d @%.1ft ring=%s w=%.2f",
                           t.foe and "enemy" or "ally", t.id or -1, t.d or -1,
                           t.near and "near" or "far", t.w or 0))
    end
  end
  if it.skipped then
    for _, s in ipairs(it.skipped) do
      print2(string.format(
        "       ODDS skip enemy #%d @%.1ft — engaged by ally #%d (slate age %dt)",
        s.id or -1, s.d or -1, s.by or -1, s.age or -1))
    end
  end
  return v, i, vt, it
end

return M
