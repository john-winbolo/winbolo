-- GoalHunter/pill_table.lua
-- Far-left HUD: every friendly pillbox as a row, colored by reposition
-- category (back / front / aggressive / in-use). Categories use the SAME
-- front-line definition as the "3" influence overlay: a tile is "front" when
-- it sits near an influence sign-flip boundary (see brainPathfinderFindFrontLine).
-- Debug viz only — all drawing goes through viz.* which lua_strip removes
-- from opt/, so this is a no-op in production/splash builds.
local PP  = require("pill_portfolio")
local cpf = require("cpathfinder")

local M = {}

local AGGRO_DEEP = -50  -- influence deeper than this = overextended

local COLORS = {
  back    = {  90, 220, 120 },  -- green:  holding friendly territory
  front   = { 255, 225,  70 },  -- yellow: on/near the front line
  aggro   = { 255,  95,  55 },  -- red:    in enemy influence
  utility = {  80, 215, 205 },  -- teal:   blocker in an active pill take
  intank  = { 120, 200, 255 },  -- cyan:   currently carried in a tank
  down    = { 130, 130, 130 },  -- gray:   dead on the ground
}

-- Category for a friendly pill (delegates to the shared portfolio model).
-- Returns (category, influence).
function M.classify(p)
  return PP.classify(p.mx, p.my, p._in_use)
end

-- What category of pill the followed bot is about to build, if it's holding
-- one and has a strategic-placement target. Returns (category, mx, my) or nil.
local function building_intent(state, info)
  if not state or not state.goal then return nil end
  local g = state.goal
  if g.kind ~= "place_pill_strategic" or not g.mx then return nil end
  local cat = PP.classify(g.mx, g.my)
  return cat, g.mx, g.my
end

function M.draw(viz, world, state, info)
  if not viz or not viz.is_on or not viz.is_on("pill_portfolio") then return end
  if not viz.hud_text or not world or not world.pills then return end

  -- Classify placed pills, tally categories, and gather in-tank / down pills.
  -- An in-tank pill can be built into ANY role, so it doesn't count toward the
  -- current portfolio — instead we assign it the role it SHOULD fill.
  local counts = { back = 0, front = 0, aggro = 0, utility = 0 }
  local rows = {}
  local intank_n = 0
  for id, p in pairs(world.pills) do
    if p.in_tank then
      -- A pill carried in a tank is ALWAYS utility (own = friendly, ally = allied).
      if p.owner == "friendly" or p.owner == "allied" then
        counts.utility = counts.utility + 1
        intank_n = intank_n + 1
        local carrier = p.carrier or p.owner_player
        rows[#rows + 1] = { id = id, label = string.format("[tank #%s] util", tostring(carrier or "?")), key = "utility" }
      end
    elseif p.owner == "friendly" and (p.health or 0) > 0 then
      local cat = PP.role_of(p, state and state.tick)   -- cached 60s role
      local inf = cpf.influence_at(p.mx, p.my)
      if counts[cat] ~= nil then counts[cat] = counts[cat] + 1 end
      rows[#rows + 1] = { id = id, label = cat, key = cat, inf = inf }
    elseif p.owner == "friendly" then
      rows[#rows + 1] = { id = id, label = "[down]", key = "down" }
    end
  end

  -- Targets over the full pool (back/front/aggro/util incl. carried).
  local placed_total = counts.back + counts.front + counts.aggro
  local targets = PP.targets(placed_total + counts.utility)
  local need = {
    back  = targets.back  - counts.back,
    front = targets.front - counts.front,
    aggro = targets.aggro - counts.aggro,
  }
  table.sort(rows, function(a, b) return a.id < b.id end)

  -- Left-MIDDLE anchor.
  local x, y, dy = 8, 330, 14

  y = y + dy

  -- Ratio line: have/target per category; missing (deficit) shown as (-N).
  do
    local bd = (need.back  > 0) and string.format("(-%d)", need.back)  or ""
    local fd = (need.front > 0) and string.format("(-%d)", need.front) or ""
    local ad = (need.aggro > 0) and string.format("(-%d)", need.aggro) or ""
    y = y + dy
  end

  for _, r in ipairs(rows) do
    local c = COLORS[r.key] or { 200, 200, 200 }
    local deep = (r.key == "aggro" and r.inf and r.inf < AGGRO_DEEP) and " !deep" or ""
    y = y + dy
  end

  -- "Building" intent: when the followed bot holds a pill and has chosen a
  -- placement tile, show which category that tile is (the pill "type").
  local bcat, bmx, bmy = building_intent(state, info)
  if bcat then
    local c = COLORS[bcat] or { 255, 255, 255 }
    local carry = (info and (info.carried_pills or 0) > 0) and "holding" or "planning"
    y = y + dy
  end
end

return M
