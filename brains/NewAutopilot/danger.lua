-- =========================================================================
-- NewAutopilot/danger.lua — shell trajectory prediction + LGM dispatch safety
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

-- shell_map[mkey] = expires_tick
-- Cells on predicted hostile-shell trajectories this tick.
local shell_map = {}

function M.reset()
  shell_map = {}
end

-- -------------------------------------------------------------------------
-- Internal: trace all visible hostile shell trajectories
-- -------------------------------------------------------------------------
local function predict_shells(info, tick)
  local tank_mx = info.tankx >> 8
  local tank_my = info.tanky >> 8

  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_SHOT and (ob.info & OBJECT_HOSTILE) ~= 0
       and math.abs((ob.x >> 8) - tank_mx) + math.abs((ob.y >> 8) - tank_my) <= 10 then
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

        local mx = math.floor(wx) >> 8
        local my = math.floor(wy) >> 8

        -- Mark this cell as dangerous
        local k = U.mkey(mx, my)
        shell_map[k] = tick + C.DANGER_DECAY_TICKS_SHELL

        -- Stop at solid terrain (shell impacts here)
        local tt = U.traw(mx, my) & TERRAIN_MASK
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
  for k, exp in pairs(shell_map) do
    if tick >= exp then
      shell_map[k] = nil
    end
  end
end

-- -------------------------------------------------------------------------
-- Public: combined danger value at a map cell
-- Returns 0..200+  (DANGER_SHELL_IMPACT + pill_danger)
-- -------------------------------------------------------------------------
function M.danger_at(mx, my, tick, world)
  local k = U.mkey(mx, my)
  local shell_val = (shell_map[k] and tick < shell_map[k])
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
  local tx = info.tankx >> 8
  local ty = info.tanky >> 8
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
-- Public: call once per tick (after world.update, before build decisions)
-- -------------------------------------------------------------------------
function M.update(info, tick)
  predict_shells(info, tick)
  if tick % 10 == 0 then
    purge_shell_map(tick)
  end
end

return M
