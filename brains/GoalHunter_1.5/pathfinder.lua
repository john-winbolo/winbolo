-- =========================================================================
-- GoalHunter/pathfinder.lua — utility functions (LOS, water escape)
--
-- A* pathfinding and estimate_path_cost have moved to cpathfinder (C).
-- This module retains only Lua-side helpers that aren't in the C engine.
-- =========================================================================

local C       = require("constants")
local U       = require("util")
local threat  = require("threat")
local cpf     = require("cpathfinder")

local M = {}

function M.reset()
  -- No persistent caches to clear; threat grid handles pill danger now.
end

-- Public entry: call once per tick before any pathfinding/cost queries.
-- Terrain change list is consumed by threat.update() now; nothing to do here.
function M.begin_tick(tick, world)
end

-- How dangerous is (mx, my)? Returns total pill danger cost at that square.
-- Reads from the pre-computed threat grid (O(1) per call).
function M.pill_danger(world, mx, my)
  return threat.pill_at(mx, my)
end

-- Public LOS check: total wall HP on the line between two map tiles.
-- Returns 0 when there is a clear line of sight, >0 when walls block the shot.
function M.wall_hp_between(x0, y0, x1, y1)
  local wall_hp = 0
  U.bresenham(x0, y0, x1, y1, function(cx, cy)
    if U.in_map(cx, cy) then
      local tt = U.ttype(cx, cy)
      if tt == C.T_BUILDING then
        wall_hp = wall_hp + C.WALL_HP_FULL
      elseif tt == C.T_HALFBUILD then
        wall_hp = wall_hp + C.WALL_HP_HALF
      end
    end
  end)
  return wall_hp
end

--- Test whether the shooter at (spot_wx, spot_wy) can land a clean shot
--- on the target pill from at least one of five aim points: the pill
--- tile's center and its four corners (in world units). A shot is
--- "clean" if its simulated trajectory (real shell physics, via
--- brainPathfinderSimulateShot) crosses no other pill (friendly or
--- enemy) and no wall (T_BUILDING / T_HALFBUILD).
---
--- "Any one clears" wins — even if the center is blocked by a wall, a
--- corner shot may slip past, so the spot is still considered to have
--- LOS. The shot's *origin* tile and the *target* pill's tile are
--- excluded from the obstruction check (the shooter's own square
--- never blocks, and the target pill is, of course, on its own tile).
---
--- spot_wx, spot_wy : shooter world coords (for ShotP, snap to tile center)
--- pill             : target pill table with .mx, .my
--- world            : world snapshot (uses world.pill_at)
--- shooter_type     : cpf.SHOT_TANK (default) or cpf.SHOT_PILL
--- sight_len        : tank sightLen, 0 = max
--- @return clear bool, info { aim_idx } | nil
---         clear=true with aim_idx of the first clear angle, or
---         clear=false with nil if every angle was blocked.
function M.pill_shots_clear(spot_wx, spot_wy, pill, world,
                            shooter_type, sight_len)
  if not pill or not world then return false, nil end
  local pmx, pmy = pill.mx, pill.my
  local origin_mx = spot_wx >> 8
  local origin_my = spot_wy >> 8

  -- Aim points in world units: tile center, then the 4 corners.
  -- A tile occupies [pmx<<8, pmx<<8 + 256) on each axis, so the
  -- inclusive-corner endpoints are <<8 and (<<8) + 255.
  local base_x = pmx << 8
  local base_y = pmy << 8
  local aims = {
    { base_x | 128,    base_y | 128    },  -- center
    { base_x,          base_y          },  -- top-left
    { base_x + 255,    base_y          },  -- top-right
    { base_x,          base_y + 255    },  -- bottom-left
    { base_x + 255,    base_y + 255    },  -- bottom-right
  }

  for ai = 1, #aims do
    local tiles = cpf.simulate_shot(spot_wx, spot_wy,
                                    aims[ai][1], aims[ai][2],
                                    shooter_type or cpf.SHOT_TANK,
                                    sight_len or 0)
    local blocked = false
    if tiles then
      for ti = 1, #tiles do
        local t = tiles[ti]
        -- Stop scanning once the shell has reached the target pill's
        -- tile — in-game the shell collides with the pill and never
        -- continues past it, so anything beyond is irrelevant for
        -- whether we can land this shot.
        if t.mx == pmx and t.my == pmy then break end
        if not (t.mx == origin_mx and t.my == origin_my) then
          local idx = t.my * 256 + t.mx
          if world.pill_at and world.pill_at[idx] then
            blocked = true; break
          end
          local tt = U.ttype(t.mx, t.my)
          if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
            blocked = true; break
          end
        end
      end
    end
    if not blocked then
      return true, { aim_idx = ai }
    end
  end
  return false, nil
end

-- Water escape: scan outward for the nearest dry square
function M.find_dry_land(cur_mx, cur_my)
  for r = 1, 8 do
    local best_d, best_x, best_y = math.huge, nil, nil
    for dy = -r, r do
      for dx = -r, r do
        if math.abs(dx) == r or math.abs(dy) == r then
          local cx, cy = cur_mx + dx, cur_my + dy
          if U.in_map(cx, cy) then
            local ct = U.ttype(cx, cy)
            if ct ~= C.T_RIVER and ct ~= C.T_DEEPSEA
               and ct ~= C.T_BUILDING and ct ~= C.T_HALFBUILD then
              local d = U.mdist(cur_mx, cur_my, cx, cy)
              if d < best_d then
                best_d = d; best_x = cx; best_y = cy
              end
            end
          end
        end
      end
    end
    if best_x then return best_x, best_y end
  end
  return nil, nil
end

return M
