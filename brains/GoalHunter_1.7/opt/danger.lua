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

local M = {}

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

  -- Sample at fractions along the straight line
  local fracs = dist > 4 and {0.25, 0.5, 0.75} or {0.5}
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

-- -------------------------------------------------------------------------
-- Public: call once per tick (after world.update, before build decisions)
-- -------------------------------------------------------------------------
function M.update(info, tick)
  predict_shells(info, tick)
  if tick % 10 == 0 then
    purge_shell_map(tick)
  end
end

return M
