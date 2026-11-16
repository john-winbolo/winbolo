-- =========================================================================
-- pt2_attack.lua -- pill take v2 inside attack.lua's plan (2026-11-16)
--
-- One call, after_scan(), from the deferred shield scan in plan_position
-- (attack.lua), on the tick v1's shield.scan has its answer:
--
--   * master PILLTAKE_V2_EVALUATOR off, debug run, PT2_SIDE_BY_SIDE_LOG on:
--     v2 runs on the same inputs and logs one PT2_CMP line. v1's result is
--     handed back unchanged; nothing in goal, state or world is written
--     (except goal._pt2_viz while a pt2_* BrainTest layer is on, which only
--     the drawing reads).
--   * master on, the take is a protected one (goal._is_ppt) and v2 found a
--     SAFE point: v2's result replaces v1's, in the shield.scan shape, so the
--     standoff adoption, build_walls and the charge read it as they read
--     v1's. goal._pt2 keeps the chain. v2 found nothing safe: v1 stays.
--
-- Its own module so attack.lua gains no new local or upvalue.
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local PC     = require("pill_cover")
local V2     = require("pilltake_v2")
local shield = require("attack_shield")
local print2 = require("print2")
local PV     = require("pt2_viz")

local M = {}

local floor, abs, sqrt = math.floor, math.abs, math.sqrt
local clock_us = clock_us or function() return floor(os.clock() * 1e6) end

-- v1's green set (plan_position steps 2 and 3): clear-shot spots with an
-- adjusted score under 10, or else the best clear-shot spot and all within
-- 25% of it.
local function green_set(spots)
  local greens, los = {}, {}
  for _, s in ipairs(spots or {}) do
    if s.has_los then
      los[#los + 1] = s
      if s._adj_score and s._adj_score < 10 then greens[s] = true end
    end
  end
  if next(greens) == nil and #los > 0 then
    table.sort(los, function(a, b) return (a._adj_score or math.huge) < (b._adj_score or math.huge) end)
    local th = (los[1]._adj_score or math.huge) * 1.25
    for _, s in ipairs(los) do
      if (s._adj_score or math.huge) <= th then greens[s] = true else break end
    end
  end
  return greens
end

-- The nearest spot of v1's ring by angle.
local function nearest_spot(spots, deg)
  local best, bd = nil, nil
  for _, s in ipairs(spots or {}) do
    if s.deg then
      local d = abs(((s.deg - deg) + 540) % 360 - 180)
      if not bd or d < bd then best, bd = s, d end
    end
  end
  return best
end

-- inp for v2 from the bot's own view. The score is v1's: a point next to a
-- green spot costs its Dijkstra travel (plus the human-decoy cost, as v1),
-- any other point 10000 + the spot's adjusted score (so greens always win).
function M.build_inp(state, goal, info, world, pill, no_builder, npb, radius, now)
  local inp = V2.knob_inp(C, {})
  V2.world_inp(world, C, U, PC, cpf, inp, no_builder)
  inp.A = nil
  for _, p in ipairs(inp.pills) do
    if p == pill or (p.mx == pill.mx and p.my == pill.my and not p.in_tank) then inp.A = p; break end
  end
  if not inp.A then return nil end
  inp.radius = radius
  inp.num_pill_blockers = npb
  local bans = state.banned_pill_angles and state.banned_pill_angles[pill.my * 256 + pill.mx]
  if bans then
    inp.banned = function(deg)
      local exp = bans[floor((deg % 360) / 5) * 5]
      return exp ~= nil and now < exp
    end
  end
  local spots = goal.scan_spots
  local greens = green_set(spots)
  local boat = info.inboat and 1 or 0
  local tmx = floor((info.tankx or 0) / 256)
  local tmy = floor((info.tanky or 0) / 256)
  inp.score_fn = function(deg, mx, my, cx, cy)
    local s = nearest_spot(spots, deg)
    if s and not greens[s] then return 10000 + (s._adj_score or 1000) end
    local dij = cpf.dijkstra_lookup_by_kind(cpf.KIND_NORMAL, mx, my, boat)
    if dij >= 1e29 then dij = cpf.estimate_cost(tmx, tmy, mx, my, boat) end
    return dij + ((s and s._hd_cost) or 0)
  end
  return inp
end

-- v1's winner: point, aim, blockers and the cover they give.
local function v1_view(world, sscan, npb)
  local b = sscan and sscan.best
  if not b or (b.score or 0) <= 0 then return nil end
  local aim = b.best_aim_idx and b.aims and b.aims[b.best_aim_idx]
  local act, pot, extra, cover = {}, {}, {}, 0
  if aim then
    for _, bl in ipairs(aim.blockers or {}) do
      act[#act + 1] = bl
      if bl.kind == "friendly_pill" then
        cover = cover + PC.pill_walls(PC.pill_on_square(world, bl.mx, bl.my)) * (C.SHIELD_WALL_COVER_SHOTS or 5)
      else
        cover = cover + (C.SHIELD_WALL_COVER_SHOTS or 5)
      end
    end
    local left = npb
    for _, bl in ipairs(aim.potential_blockers or {}) do
      pot[#pot + 1] = bl
      local sh = (left > 0) and 15 or (C.SHIELD_WALL_COVER_SHOTS or 5)
      if left > 0 then left = left - 1 end
      extra[bl.my * 256 + bl.mx] = sh
      cover = cover + sh
    end
  end
  return { cx = b.cx, cy = b.cy, deg = b.deg, aim = b.best_aim_idx, act = act, pot = pot,
           extra = extra, cover = cover }
end

local function sq_list(list)
  local p = {}
  for _, s in ipairs(list or {}) do p[#p + 1] = string.format("(%d,%d)", s.mx, s.my) end
  return #p > 0 and table.concat(p, " ") or "-"
end

-- Agreement test for the log (tolerance: proposal, awaiting Andrew's value).
M.CMP_TOL_WU = 64

local function cmp_line(now, pill, inp, res, v1, cost_us, drive, do_print)
  local b = res and res.best
  local v2ok = b and b.safe
  local diff, d_wu = "same", nil
  if v1 and v2ok then
    local dx, dy = (v1.cx - b.cx) * 256, (v1.cy - b.cy) * 256
    d_wu = sqrt(dx * dx + dy * dy)
    if b.kills >= 2 then diff = "chain"
    elseif d_wu > M.CMP_TOL_WU then diff = "so" end
  elseif v1 and not v2ok then diff = "none_v2"
  elseif v2ok and not v1 then diff = "none_v1"
  elseif not v1 and not v2ok then diff = "none_both" end
  local why = "-"
  if v1 then
    why = V2.judge(inp, floor(v1.cx * 256), floor(v1.cy * 256), v1.extra, v1.aim) or "-"
  end
  local chain, plan = "-", "-"
  if b then
    local c = {}
    for _, p in ipairs(b.chain) do c[#c + 1] = V2.pill_name(inp, p) end
    chain = table.concat(c, "-")
    local pl = {}
    for _, s in ipairs(b.plan or {}) do pl[#pl + 1] = string.format("(%d,%d)%s", s.mx, s.my, s.kind == "pill" and "P" or "W") end
    if #pl > 0 then plan = table.concat(pl, " ") end
  end
  local line = string.format(
    "PT2_CMP t=%d pill=(%d,%d) hp=%d | v1: so=%s deg=%s aim=%s act=%s pot=%s cover=%s"
    .. " | v2: so=%s deg=%s aim=%s chain=%s mode=%s kills=%s need=%s have=%s safe=%s lee=%s plan=%s"
    .. " | diff=%s d_wu=%s why=%s drive=%s cost_us=%d evals=%d",
    now, pill.mx, pill.my, pill.health or 0,
    v1 and string.format("(%.2f,%.2f)", v1.cx, v1.cy) or "-",
    v1 and string.format("%.1f", v1.deg or 0) or "-",
    v1 and tostring(v1.aim) or "-",
    v1 and sq_list(v1.act) or "-", v1 and sq_list(v1.pot) or "-",
    v1 and tostring(v1.cover) or "-",
    b and string.format("(%.2f,%.2f)", b.cx, b.cy) or "-",
    b and string.format("%.1f", b.deg) or "-",
    b and tostring(b.aim_idx) or "-",
    chain, b and (b.chain_mode and "HOLD" or "SINGLE") or "-",
    b and tostring(b.kills) or "-", b and tostring(b.need) or "-", b and tostring(b.have) or "-",
    tostring(v2ok and true or false), b and tostring(b.leeway) or "-", plan,
    diff, d_wu and string.format("%.0f", d_wu) or "-", why,
    drive and "v2" or "v1", cost_us, res and res.n_eval or 0)
  if do_print then print2(line) end
  return line
end

-- Called with v1's sscan; returns the sscan to use. See the file header.
-- a = { state, goal, info, world, pill, sscan, no_builder, npb, radius, now }
function M.after_scan(a)
  local master = C.PILLTAKE_V2_EVALUATOR
  local log = BRAIN_DEBUG_MODE and C.PT2_SIDE_BY_SIDE_LOG and _G._PRINT2_ENABLED
  if not master and not log then return a.sscan end
  local goal, pill = a.goal, a.pill
  if master and not goal._is_ppt and not log then return a.sscan end
  local t0 = clock_us()
  -- A v2 bug must never cost the take: v1's result stands. A budget kill
  -- is passed on, as attack.lua does for shield.scan.
  local viz_on = BRAIN_DEBUG_MODE and PV.any_on()
  local ok, inp, res = pcall(function()
    local i = M.build_inp(a.state, goal, a.info, a.world, pill, a.no_builder, a.npb, a.radius, a.now)
    if not i then return nil, nil end
    if viz_on then i.keep_all = true end
    return i, V2.evaluate(i)
  end)
  if not ok then
    local msg = tostring(inp)
    if msg:find("tick_budget_exceeded", 1, true) then error(inp, 0) end
    print("[pt2] evaluator error, v1 kept: " .. msg)
    return a.sscan
  end
  if not inp or not res then return a.sscan end
  local cost = clock_us() - t0
  local drive = master and goal._is_ppt and res.best and res.best.safe
  local v1 = (log or viz_on) and v1_view(a.world, a.sscan, a.npb) or nil
  local line = (log or viz_on) and cmp_line(a.now, pill, inp, res, v1, cost, drive, log) or nil
  -- BrainTest layers: goal._pt2_viz is read only by pt2_viz.draw.
  if viz_on then PV.capture(goal, inp, res, v1, line, a.now) end
  if not drive then
    if master then goal._pt2 = nil end
    return a.sscan
  end
  local b = res.best
  local standoff = (a.sscan and a.sscan.standoff)
    or { cx = goal.standoff_fx, cy = goal.standoff_fy, mx = goal.standoff_mx, my = goal.standoff_my }
  local out = V2.to_scan_result(res, pill, standoff, function(mx, my)
    return shield.adjacent_friendly_pill(a.world, mx, my)
  end)
  out.num_pill_blockers = a.npb
  out.pill_wall_equiv = C.PPT_PILL_WALL_EQUIV or 3.0
  goal._pt2 = out.chain
  goal._pt2.tick = a.now
  print2(string.format("PT2_DRIVE t=%d pill=(%d,%d) so=(%.2f,%.2f) deg=%.1f aim=%d kills=%d build=%d have=%d need=%d",
    a.now, pill.mx, pill.my, b.cx, b.cy, b.deg, b.aim_idx, b.kills, #b.plan, b.have, b.need))
  return out
end

return M
