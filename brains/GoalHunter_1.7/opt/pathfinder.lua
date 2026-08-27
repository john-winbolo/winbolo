local bit = require('bitcompat')
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
  local origin_mx = bit.rshift(spot_wx, 8)
  local origin_my = bit.rshift(spot_wy, 8)

  -- Aim points in world units: tile center, then the 4 corners.
  -- A tile occupies [pmx<<8, pmx<<8 + 256) on each axis, so the
  -- inclusive-corner endpoints are <<8 and (<<8) + 255.
  local base_x = bit.lshift(pmx, 8)
  local base_y = bit.lshift(pmy, 8)
  local aims = {
    { bit.bor(base_x, 128),    bit.bor(base_y, 128)    },  -- center
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

-- Solid-for-driving test for the escape beeline: walls and pill tiles
-- stop a tank dead. T_PILLBOX is conservative (a dead pill is passable)
-- but a live one is another permanent pin, so route around both.
-- T_DEEPSEA blocks too: escape steering beelines at full throttle with
-- the cliff brake exempted, so a line crossing deep sea is a drowning,
-- not a pin. Safe because beeline_clear excludes endpoints — a tank
-- standing IN water never blocks its own escape line, and river
-- intermediates stay passable (that's how you drive out of a channel).
local function escape_solid(tx, ty)
  if not U.in_map(tx, ty) then return true end
  local tt = U.ttype(tx, ty)
  return tt == C.T_BUILDING or tt == C.T_HALFBUILD or tt == C.T_PILLBOX
         or tt == C.T_DEEPSEA
end

-- True when the straight tile-line from (x0,y0) to (x1,y1) crosses no
-- solid tile. Endpoints excluded (the tank stands on x0,y0; the caller
-- already vetted the destination). Diagonal steps are blocked when
-- EITHER orthogonal neighbour is solid — the on-foot rule from the C
-- pathfinder (brain_pathfinder.c corner-cut check): a full-tile tank
-- cannot squeeze diagonally past ANY solid corner. (Requiring BOTH
-- corners solid is the boat rule; using it here let the beeline clip
-- corners at channel bends and re-create the very pin this fixes.)
local function beeline_clear(x0, y0, x1, y1)
  local dx, dy = math.abs(x1 - x0), math.abs(y1 - y0)
  local sx = x0 < x1 and 1 or -1
  local sy = y0 < y1 and 1 or -1
  local err = dx - dy
  local cx, cy = x0, y0
  while cx ~= x1 or cy ~= y1 do
    local e2 = 2 * err
    local step_x = e2 > -dy
    local step_y = e2 < dx
    if step_x and step_y
       and (escape_solid(cx + sx, cy) or escape_solid(cx, cy + sy)) then
      return false
    end
    if step_x then err = err - dy; cx = cx + sx end
    if step_y then err = err + dx; cy = cy + sy end
    if (cx ~= x1 or cy ~= y1) and escape_solid(cx, cy) then return false end
  end
  return true
end

-- Water escape: scan outward for the nearest dry square the tank can
-- actually DRIVE to. escape_water steering beelines at the result with
-- no A*, so a candidate only counts when the straight line is clear of
-- walls/pills (beeline_clear) — otherwise the tank pins against the
-- wall and idles until something knocks it down (observed 5-8 minute
-- stalls on river-maze maps like Wild Bleeding Chickens). Candidates
-- the stuck handler recently blocked (state.blocked, the same store the
-- goal pickers consult) are skipped, so a failed escape target rotates
-- to the next-best tile instead of being re-picked every tick forever.
-- state/now are optional: omitted -> no blocked filtering (old shape).
function M.find_dry_land(cur_mx, cur_my, state, now)
  local blocked = state and state.blocked or nil
  -- Nearest dry tile ignoring line-of-drive, kept as a fallback when
  -- nothing within 8 rings is beeline-reachable: pushing toward it can
  -- still slide the tank along walls, and each 3-second stuck cycle
  -- blocks it so the next call rotates to a different target.
  local any_d, any_x, any_y = math.huge, nil, nil
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
              local bk = cy * C.MAP_W + cx
              if not (blocked and blocked[bk] and now and now < blocked[bk]) then
                local d = U.mdist(cur_mx, cur_my, cx, cy)
                if d < any_d then
                  any_d = d; any_x = cx; any_y = cy
                end
                if d < best_d and ct ~= C.T_PILLBOX
                   and beeline_clear(cur_mx, cur_my, cx, cy) then
                  best_d = d; best_x = cx; best_y = cy
                end
              end
            end
          end
        end
      end
    end
    if best_x then return best_x, best_y end
  end
  return any_x, any_y
end

return M
