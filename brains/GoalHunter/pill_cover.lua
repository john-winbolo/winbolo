-- =========================================================================
-- GoalHunter/pill_cover.lua — how much shield cover a pill on a square gives
--
-- One rule for the protected-take build loop (attack.lua build_walls), the
-- builder (builder.lua wall_shield) and the shield planner
-- (attack_shield.lua, and gh_shield_stamp.c naShieldPillCoverWalls, which
-- uses the same formula):
--
--   walls = floor(health / 5)      15 -> 3, 10-14 -> 2, 5-9 -> 1, 1-4 -> 0
--
-- Cover is counted in C.SHIELD_WALL_COVER_SHOTS (5) shots per wall, the
-- classic shots to break a wall, so a friendly pill with health h is worth
-- 5 * floor(h / 5) shots of cover. It stays 5 whatever building_life is, to
-- match gh_shield_stamp.c (WALL_SHOTS 5.0). "Friendly" is the brain
-- API's owner string "friendly" (our own pill or an ally's, see world.lua
-- classify_owner) or "allied". Enemy ("hostile") and "neutral" pills give
-- no cover.
-- =========================================================================

local C = require("constants")

local M = {}

M.HEALTH_PER_WALL = 5

-- Walls of cover a friendly pill with this health gives.
function M.walls_for_health(h)
  h = tonumber(h) or 0
  if h <= 0 then return 0 end
  return math.floor(h / M.HEALTH_PER_WALL)
end

function M.is_friendly_owner(owner)
  return owner == "friendly" or owner == "allied"
end

-- Walls of cover pill p gives us: 0 for no pill, a pill in a tank, an enemy
-- or neutral pill, or a friendly pill under 5 health.
function M.pill_walls(p)
  if p and not p.in_tank and M.is_friendly_owner(p.owner) then
    return M.walls_for_health(p.health)
  end
  return 0
end

-- The deployed pill on square (mx,my), any owner, any health (a dead pill
-- still stands on its square and no wall can go there), or nil. Checks the
-- world.pill_at entry against the live world.pills table, as
-- shot_path_obstacle_count does: an entry can point at an old copy of a pill
-- that has since been picked up.
function M.pill_on_square(world, mx, my)
  local plist = world and world.pill_at and world.pill_at[my * 256 + mx]
  if not plist then return nil end
  local pills = world.pills
  for _, e in ipairs(plist) do
    local p = (e.id and pills and pills[e.id]) or e.pill
    if p and not p.in_tank and (p.mx == nil or (p.mx == mx and p.my == my)) then
      return p
    end
  end
  return nil
end

-- Walls of cover from the pill on square (mx,my) (0 when there is none).
function M.square_walls(world, mx, my)
  return M.pill_walls(M.pill_on_square(world, mx, my))
end

-- ── Protected-take build loop (attack.lua build_walls) ────────────────────
-- tt is the slot's terrain (U.ttype). The brain's terrain map does not show
-- pills (a pill square reads as the ground under it), so each test also
-- looks for a pill on the square.

-- Is the build loop done with this slot? A wall, a half-built wall, or ANY
-- pill on the square: no wall can go on a pill, so the loop never waits on
-- that slot and never asks the builder for a wall there.
function M.slot_done(world, mx, my, tt)
  if tt == C.T_BUILDING or tt == C.T_HALFBUILD or tt == C.T_PILLBOX then
    return true
  end
  return M.pill_on_square(world, mx, my) ~= nil
end

-- Cover on this slot in shots: a friendly pill 5 * floor(health / 5), a wall
-- or half-built wall SHIELD_WALL_COVER_SHOTS (5), anything else 0.
function M.slot_cover_shots(world, mx, my, tt)
  local w = M.square_walls(world, mx, my)
  local ws = C.SHIELD_WALL_COVER_SHOTS or 5
  if w > 0 then return ws * w end
  if tt == C.T_BUILDING or tt == C.T_HALFBUILD then return ws end
  return 0
end

-- Does this slot hold a finished blocker: a full wall, or a friendly pill
-- worth at least one wall? (A half-built wall does not count here.)
function M.slot_blocker(world, mx, my, tt)
  if tt == C.T_BUILDING or tt == C.T_PILLBOX then return true end
  return M.square_walls(world, mx, my) > 0
end

return M
