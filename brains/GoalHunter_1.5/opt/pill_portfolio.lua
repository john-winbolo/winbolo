local function __idiv(a,b) return math.floor(a/b) end
-- GoalHunter/pill_portfolio.lua
-- Shared friendly-pill positioning model: classify a pill/tile into
-- back / front / aggressive (+ in-use), and the 35/45/20 target portfolio.
-- Single source of truth for both the pill-table visualizer (pill_table.lua)
-- and strategic placement (goals.lua eval_place_pill_strategic).
--
-- Roles (M.classify):
--   front  - within FRONT_NEAR_RADIUS of the front line (near_front).
--   aggro  - own tile in enemy influence (< 0), OR surrounded by it (>=
--            AGGRO_NEG_NEIGHBORS of the 8 adjacent tiles negative).
--   back   - positive influence AND outside the front range (and not surrounded).
--   utility- a pill in a tank or reserved as an in-use blocker.
-- See PILL_REPOSITION_PLAN.md.
local cpf = require("cpathfinder")
local C   = require("constants")

local M = {}

-- "front" = within FRONT_NEAR_RADIUS tiles (euclidean) of a front "3" tile.
-- Narrowed to 2 (2026-06-16): only pills hugging the contested line count as
-- front. History: flat 3-tile chebyshev box -> pill SHOOT RANGE (8, so any pill
-- that could fire at the line) -> 2, since shoot-range-wide pulled in pills well
-- behind the line and inflated the "front" bucket.
local FRONT_NEAR_RADIUS = C.FRONT_NEAR_RADIUS or 2

-- Portfolio targets (share of friendly pills). util = blockers + carried pills
-- (R1): an enforced 15% reserve that flexes to defense. back rolls forward into
-- front as the line advances; front and aggro are never repositioned.
M.TARGET_BACK  = 0.20
M.TARGET_FRONT = 0.45
M.TARGET_AGGRO = 0.20
M.TARGET_UTIL  = 0.15

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
  local r  = FRONT_NEAR_RADIUS
  local r2 = r * r
  for dy = -r, r do
    for dx = -r, r do
      if dx * dx + dy * dy <= r2 and M.on_front_line(mx + dx, my + dy) then
        return true
      end
    end
  end
  return false
end

-- Precise classification of a pill/tile (front-line aware). Returns
-- (category, influence). `in_use` flags a pill reserved for a pill take.
function M.classify(mx, my, in_use)
  local inf = cpf.influence_at(mx, my)
  if in_use then return "utility", inf end
  -- front: on/near the front line (the FRONT_NEAR_RADIUS box scan).
  if M.near_front(mx, my) then return "front", inf end
  -- aggro: own tile in enemy influence, OR surrounded by it (>= AGGRO_NEG_NEIGHBORS
  -- of the 8 adjacent tiles negative — catches a positive tile boxed in by enemy).
  if inf < 0 then return "aggro", inf end
  local neg = 0
  for dy = -1, 1 do
    for dx = -1, 1 do
      if not (dx == 0 and dy == 0)
         and (cpf.influence_at(mx + dx, my + dy) or 0) < 0 then
        neg = neg + 1
      end
    end
  end
  if neg >= (C.AGGRO_NEG_NEIGHBORS or 5) then return "aggro", inf end
  -- back: positive influence AND outside the front range AND not surrounded.
  if inf > 0 then return "back", inf end
  -- inf == 0, away from the line, not surrounded: neutral → treat as front.
  return "front", inf
end

-- Cached role for an EXISTING pill, re-evaluated every PILL_ROLE_REEVAL_TICKS
-- (~60s). Influence shifts over time — an aggressive pill can become a front
-- pill as the line moves — but classifying live every tick both costs the
-- near_front scan and makes categories flicker. Cache on the pill; refresh on
-- a stagger. `tick` drives the refresh; pass the current sim tick.
-- force_fresh: ignore the 60s cache and reclassify NOW. Used before committing
-- an irreversible decision off a pill's role (e.g. repositioning a "back" pill)
-- so a stale cache — or a front line that has since advanced over the pill —
-- can't trigger a move the rest of the team (who reclassified more recently)
-- disagrees with. Roles are per-bot and never shared, so the only defence
-- against acting on a stale role is to refresh it at the decision point.
function M.role_of(pill, tick, force_fresh)
  if not pill then return "front" end
  -- A pill carried IN A TANK is always utility (a mobile reserve).
  if pill.in_tank then return "utility" end
  -- A pill currently serving as a blocker in an active pill take is "utility"
  -- (overrides its back/front/aggro role) until the take ends. Driven live by
  -- the team blocker broadcast (pill._in_use), so it reverts automatically.
  if pill._in_use then return "utility" end
  tick = tick or 0
  local stale = force_fresh or (not pill.role) or (not pill.role_tick)
    or (tick - pill.role_tick) >= (C.PILL_ROLE_REEVAL_TICKS or 3000)
  if stale then
    pill.role      = (M.classify(pill.mx, pill.my, false))
    pill.role_tick = tick
  end
  return pill.role
end

-- Current friendly-pill counts per category, using the cached 60s role.
-- In-tank pills (own = "friendly", ally = "allied") always count as utility;
-- deployed friendly pills count by their cached role.
function M.counts(world, tick)
  local c = { back = 0, front = 0, aggro = 0, utility = 0 }
  if world and world.pills then
    for _, p in pairs(world.pills) do
      if p.in_tank then
        if p.owner == "friendly" or p.owner == "allied" then
          c.utility = c.utility + 1
        end
      elseif p.owner == "friendly" and (p.health or 0) > 0 then
        local cat = M.role_of(p, tick)
        c[cat] = (c[cat] or 0) + 1
      end
    end
  end
  return c
end

-- Priority order for filling the portfolio when pills are scarce: util first,
-- then front, then aggro, then back. The first pill a team gets should be a
-- carried utility blocker, not a back defender; back is the LAST role to fill.
M.FILL_PRIORITY = { "utility", "front", "aggro", "back" }

-- Target counts for a given total. Each category gets the FLOOR of its share
-- (R1: util 15 / front 45 / aggro 20 / back 20), then the leftover rounding
-- slots are handed out in M.FILL_PRIORITY order — so for small totals util/
-- front/aggro fill before any back pill. (No forced >=1 back anymore: back is
-- the lowest priority; the base-guardian bonus handles must-cover bases.)
function M.targets(total)
  if total <= 0 then return { back = 0, front = 0, aggro = 0, utility = 0 } end
  local t = {
    utility = math.floor(total * M.TARGET_UTIL),
    front   = math.floor(total * M.TARGET_FRONT),
    aggro   = math.floor(total * M.TARGET_AGGRO),
    back    = math.floor(total * M.TARGET_BACK),
  }
  local assigned = t.utility + t.front + t.aggro + t.back
  local i = 0
  while assigned < total do
    local cat = M.FILL_PRIORITY[(i % #M.FILL_PRIORITY) + 1]
    t[cat] = t[cat] + 1
    assigned = assigned + 1
    i = i + 1
  end
  return { back = t.back, front = t.front, aggro = t.aggro, utility = t.utility }
end

-- ── Visualizers (R1 / brainstorm batch) ───────────────────────────────────
local _ROLE_COL = {
  back    = { 120, 160, 255 },
  front   = { 120, 255, 160 },
  aggro   = { 255, 140, 120 },
  utility = { 230, 210, 120 },
}

-- pill_roles: tint every friendly pill by its cached role + a letter (b/f/a/u).
function M.draw_roles(viz, world, tick)
  if not viz.is_on("pill_roles") or not viz.rect or not world or not world.pills then return end
  for _, p in pairs(world.pills) do
    if p.owner == "friendly" and (p.health or 0) > 0 then
      local role = M.role_of(p, tick)
      local c = _ROLE_COL[role] or { 200, 200, 200 }
      if viz.text then
      end
    end
  end
end

-- front_band: the front "3" tiles (bright dots) plus the ~9-tile euclidean band
-- that defines the "front" category (faint rings, sparse to limit overdraw).
function M.draw_front_band(viz)
  if not viz.is_on("front_band") or not viz.circle then return end
  local fpts = cpf.find_front_line()
  local np = fpts and (__idiv(#fpts, 2)) or 0
  for i = 1, np do
    local mx, my = fpts[2 * i - 1], fpts[2 * i]
    if (i % 5) == 0 then
    end
  end
end

return M
