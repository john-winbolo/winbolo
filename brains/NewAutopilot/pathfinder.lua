-- =========================================================================
-- NewAutopilot/pathfinder.lua — utility functions (LOS, water escape)
--
-- A* pathfinding and estimate_path_cost have moved to cpathfinder (C).
-- This module retains only Lua-side helpers that aren't in the C engine.
-- =========================================================================

local C       = require("constants")
local U       = require("util")
local threat  = require("threat")

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
