-- GoalHunter/pill_portfolio.lua
-- Shared friendly-pill positioning model: classify a pill/tile into
-- back / front / aggressive (+ in-use), and the 35/45/20 target portfolio.
-- Single source of truth for both the pill-table visualizer (pill_table.lua)
-- and strategic placement (goals.lua eval_place_pill_strategic).
--
-- "front" uses the same definition as the "3" influence overlay: a tile sits
-- near an influence sign-flip boundary (brainPathfinderFindFrontLine). "back"
-- is solidly friendly influence (> BACK_INFLUENCE_MIN); "aggressive" is enemy
-- influence (< 0).  See PILL_REPOSITION_PLAN.md.
local cpf = require("cpathfinder")
local C   = require("constants")

local M = {}

local FRONT_NEAR_RADIUS = 3   -- tiles: "front" if within this of a sign-flip

-- Portfolio targets (share of friendly pills, excluding in-use).
M.TARGET_BACK  = 0.25
M.TARGET_FRONT = 0.50
M.TARGET_AGGRO = 0.25

-- A tile is on the front line if it has influence AND an orthogonal neighbor
-- of opposite sign (mirrors brainPathfinderFindFrontLine / the "3" overlay).
function M.on_front_line(tx, ty)
  if tx < 1 or tx > 254 or ty < 1 or ty > 254 then return false end
  local v = cpf.influence_at(tx, ty)
  if v == 0 then return false end
  local n = cpf.influence_at(tx, ty - 1)
  local s = cpf.influence_at(tx, ty + 1)
  local w = cpf.influence_at(tx - 1, ty)
  local e = cpf.influence_at(tx + 1, ty)
  if v > 0 then return n < 0 or s < 0 or w < 0 or e < 0 end
  return n > 0 or s > 0 or w > 0 or e > 0
end

function M.near_front(mx, my)
  for dy = -FRONT_NEAR_RADIUS, FRONT_NEAR_RADIUS do
    for dx = -FRONT_NEAR_RADIUS, FRONT_NEAR_RADIUS do
      if M.on_front_line(mx + dx, my + dy) then return true end
    end
  end
  return false
end

-- Precise classification of a pill/tile (front-line aware). Returns
-- (category, influence). `in_use` flags a pill reserved for a pill take.
function M.classify(mx, my, in_use)
  local inf = cpf.influence_at(mx, my)
  if in_use then return "utility", inf end
  if M.near_front(mx, my) then return "front", inf end
  if inf < 0 then return "aggro", inf end
  if inf > C.STRATEGIC_PLACE_BACK_INFLUENCE_MIN then return "back", inf end
  return "front", inf
end

-- Fast, value-only category (no front-line scan) for hot paths like the
-- placement candidate sweep. Approximates M.classify at the boundary.
function M.category_by_influence(inf)
  if inf < 0 then return "aggro" end
  if inf > C.STRATEGIC_PLACE_BACK_INFLUENCE_MIN then return "back" end
  return "front"
end

-- Cached role for an EXISTING pill, re-evaluated every PILL_ROLE_REEVAL_TICKS
-- (~60s). Influence shifts over time — an aggressive pill can become a front
-- pill as the line moves — but classifying live every tick both costs the
-- near_front scan and makes categories flicker. Cache on the pill; refresh on
-- a stagger. `tick` drives the refresh; pass the current sim tick.
function M.role_of(pill, tick)
  if not pill then return "front" end
  -- A pill currently serving as a blocker in an active pill take is "utility"
  -- (overrides its back/front/aggro role) until the take ends. Driven live by
  -- the team blocker broadcast (pill._in_use), so it reverts automatically.
  if pill._in_use then return "utility" end
  tick = tick or 0
  local stale = (not pill.role) or (not pill.role_tick)
    or (tick - pill.role_tick) >= (C.PILL_ROLE_REEVAL_TICKS or 3000)
  if stale then
    pill.role      = (M.classify(pill.mx, pill.my, false))
    pill.role_tick = tick
  end
  return pill.role
end

-- Current friendly-pill counts per category, using the cached 60s role.
function M.counts(world, tick)
  local c = { back = 0, front = 0, aggro = 0, utility = 0 }
  if not world or not world.pills then return c end
  for _, p in pairs(world.pills) do
    if p.owner == "friendly" and (p.health or 0) > 0 then
      local cat = M.role_of(p, tick)
      c[cat] = (c[cat] or 0) + 1
    end
  end
  return c
end

-- Target counts for a given total (excludes in-use). Guarantees >=1 back.
function M.targets(total)
  if total <= 0 then return { back = 0, front = 0, aggro = 0 } end
  local back  = math.max(1, math.floor(total * M.TARGET_BACK + 0.5))
  local front = math.floor(total * M.TARGET_FRONT + 0.5)
  local aggro = total - back - front
  if aggro < 0 then aggro = 0; front = math.max(0, total - back) end
  return { back = back, front = front, aggro = aggro }
end

return M
