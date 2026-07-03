-- =========================================================================
-- demine.lua — automatic mine-clearing interrupt.
--
-- When a KNOWN mine (TERRAIN_MINE_FLAG set on the brain map) sits within
-- crosshair range of the tank and the current goal is interruptible, PUSH a
-- synthetic kill_mine goal on top of it: the real goal object is stashed
-- UNTOUCHED (substate, timers, scan results, all context) on
-- state._demine_saved. Steering then stops the tank and drives the
-- crosshair — heading AND gunsight length — RIGHT ONTO the mine tile: a
-- shell only detonates a mine when it ENDS on the mined square (shells.c
-- calls minesExpAddItem at both shell-death paths — collision and
-- range-expiry — never mid-flight), exactly like landing a shot on an LGM.
-- The moment the mine flag clears, POP: state.goal = the saved object and
-- the old goal resumes where it left off.
--
-- Interruptible = every goal EXCEPT combat/urgency kinds (kill_lgm,
-- attack_tank, flee/escape/rescue/wait) and any attack_pill substate other
-- than the get-into-position phases (plan_position / approach). The
-- selector holds a pushed kill_mine against replans (init.lua arbitration
-- chain) with flee_to_base as the only preempt.
--
-- Target choice: mines behind the tank cost more, proportionally cheaper
-- the closer the mine's bearing is to the current heading:
--   cost = dist_wu * (1 + DEMINE_BEHIND_MULT * (|heading - bearing| / 128))
-- =========================================================================
local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local print2 = require("print2")

local M = {}

-- Goal kinds that must never be interrupted for a mine.
local DENY_KINDS = {
  kill_lgm = true, attack_tank = true, kill_mine = true,
  escape_water = true, flee_pill = true, flee_to_base = true,
  rescue_lgm = true, wait_for_lgm = true,
  pill_place = true, place_pill_strategic = true,
}
-- attack_pill: only the get-into-position phases may be interrupted.
local PILL_OK_SUB = { plan_position = true, approach = true }

local function eligible(state, info)
  if C.DEMINE_ENABLE == false then return false end
  if state.command_goal then return false end
  if info.inboat then return false end
  if (info.shells or 0) < (C.DEMINE_MIN_SHELLS or 3) then return false end
  local g = state.goal
  if not g or DENY_KINDS[g.kind] then return false end
  if g.kind == "attack_pill" and not PILL_OK_SUB[g.substate or ""] then
    return false
  end
  return true
end

-- Best mine in crosshair range, or nil. Raw-terrain box scan around the
-- tank; runs on the DEMINE_SCAN_PERIOD cadence so the ~800 get_terrain
-- reads amortize to noise.
local function find_target(state, world, info)
  local now = state.tick or 0
  local tmx, tmy = info.tankx >> 8, info.tanky >> 8
  local R      = math.floor((C.GUNSIGHT_MAX or 14) / 2)   -- crosshair reach, tiles
  local min_wu = C.DEMINE_MIN_DIST_WU or 512              -- don't blast our own feet
  local max_wu = R * 256
  local cd     = state._demine_cooldown
  local lgm_out = info.man_status ~= C.LGM_INTANK and info.man_x and info.man_y
  local best_mx, best_my, best_cost
  for dy = -R, R do
    for dx = -R, R do
      if dx ~= 0 or dy ~= 0 then
        local mx2, my2 = tmx + dx, tmy + dy
        -- OUR influence only: clearing lanes in friendly territory is worth
        -- shells; a mine in contested/enemy ground is theirs to live with
        -- (and shelling it advertises our position for nothing).
        if U.in_map(mx2, my2)
           and (U.traw(mx2, my2) & TERRAIN_MINE_FLAG) ~= 0
           and (cpf.influence_at(mx2, my2) or 0) > 0 then
          local k = my2 * 256 + mx2
          if not (cd and cd[k] and cd[k] > now) then
            local wx = (mx2 << 8) | 128
            local wy = (my2 << 8) | 128
            local ddx, ddy = wx - info.tankx, wy - info.tanky
            local dist = math.sqrt(ddx * ddx + ddy * ddy)
            -- In reach, not under our feet, and never with our own LGM
            -- near the blast (mine explosions kill LGMs within a tile).
            if dist >= min_wu and dist <= max_wu
               and not (lgm_out
                        and U.wdist(info.man_x, info.man_y, wx, wy) < 512) then
              local bearing = U.aim_at(info.tankx, info.tanky, wx, wy)
              local adiff   = math.abs(U.adiff(info.direction, bearing))
              local cost    = dist * (1.0 + (C.DEMINE_BEHIND_MULT or 2.0)
                                            * (adiff / 128.0))
              if not best_cost or cost < best_cost then
                best_cost, best_mx, best_my = cost, mx2, my2
              end
            end
          end
        end
      end
    end
  end
  if not best_mx then return nil end
  -- Only push for a mine we can actually land a shell on from here.
  -- (Lazy require: steering requires other modules but never demine,
  -- so this cannot cycle.)
  local steer = require("steering")
  local wx = (best_mx << 8) | 128
  local wy = (best_my << 8) | 128
  if not steer.shot_path_clear(info, world, wx, wy, best_mx, best_my) then
    -- Blocked line: cool the tile down so the scan doesn't re-pick it
    -- every pass; a later scan from elsewhere may see it clear.
    state._demine_cooldown = state._demine_cooldown or {}
    state._demine_cooldown[best_my * 256 + best_mx] = now + 150
    return nil
  end
  return best_mx, best_my, best_cost
end

-- Per-tick update. Call AFTER goal selection (init.lua think loop):
-- pops a finished/stale kill_mine back to the saved goal, or pushes a
-- fresh one over an interruptible goal when a shootable mine exists.
function M.update(state, world, info)
  local now = state.tick or 0
  local g = state.goal

  -- ── Active kill_mine: pop when cleared / stale / dry ──────────────
  if g and g.kind == "kill_mine" then
    local cleared = (U.traw(g.mx, g.my) & TERRAIN_MINE_FLAG) == 0
    local timeout = (now - (g._push_tick or now)) > (C.DEMINE_MAX_TICKS or 150)
    local dry     = (info.shells or 0) <= 0
    if cleared or timeout or dry then
      if not cleared then
        -- Give up on this tile for a while (blocked geometry / out of
        -- shells) so we don't immediately re-push the same mine.
        state._demine_cooldown = state._demine_cooldown or {}
        state._demine_cooldown[g.my * 256 + g.mx] = now + 500
      end
      local saved = state._demine_saved
      state._demine_saved = nil
      state.goal = saved or { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
      state.pf.status = "idle"   -- resume with a fresh path step
    end
    return
  end

  -- ── No interrupt active: consider a push ───────────────────────────
  if not eligible(state, info) then return end
  if (now - (state._demine_scan_tick or -1e9)) < (C.DEMINE_SCAN_PERIOD or 5) then
    return
  end
  state._demine_scan_tick = now
  local mx, my, cost = find_target(state, world, info)
  if not mx then return end
  state._demine_saved = state.goal
  state.goal = { kind = "kill_mine", mx = mx, my = my,
                 wx = (mx << 8) | 128, wy = (my << 8) | 128,
                 _push_tick = now }
end

return M
