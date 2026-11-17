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

-- Headless debug recordings keep the pt2_* BrainTest layers (default off,
-- so a winbolods -brain-debug run had none of them to replay).

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

-- A pill's anger now: world.lua keeps the anger of the tick it last saw
-- the pill (anger_tick) and it falls 1 / PILL_ANGER_DECAY per tick since.
function M.anger_now(p, now)
  local a = p and p.anger or 0
  if a <= 0 then return 0 end
  local dt = (now or 0) - (p.anger_tick or now or 0)
  if dt > 0 then a = a - dt / (C.PILL_ANGER_DECAY or 3000) end
  return a > 0 and a or 0
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
  -- PT2_FINISH_NO_COVER_MAX_ANGER: our armour and each pill's anger now
  inp.armour = info.armour or 0
  inp.anger_of = function(p) return M.anger_now(p, now) end
  local goalA = inp.A
  local curA = goalA
  inp.banned = function(deg)
    local bans = state.banned_pill_angles and state.banned_pill_angles[curA.my * 256 + curA.mx]
    if not bans then return false end
    local exp = bans[floor((deg % 360) / 5) * 5]
    return exp ~= nil and now < exp
  end
  -- evaluate_group: another pill of the group as A. The goal's scan spots
  -- are angles around the target, so they count only for the target.
  inp.set_A = function(p) curA = p end
  local spots = goal.scan_spots
  local greens = green_set(spots)
  local boat = info.inboat and 1 or 0
  local tmx = floor((info.tankx or 0) / 256)
  local tmy = floor((info.tanky or 0) / 256)
  inp.score_fn = function(deg, mx, my, cx, cy)
    local s = (curA == goalA) and nearest_spot(spots, deg) or nil
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

-- The mode for the logs: SINGLE or HOLD (a chain), "+FINISH" (or FINISH
-- alone for a single take) when A's ray had no cover need.
local function mode_of(b)
  if not b then return "-" end
  if b.finish then return b.chain_mode and "HOLD+FINISH" or "FINISH" end
  return b.chain_mode and "HOLD" or "SINGLE"
end

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
    chain, mode_of(b),
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
    return i, V2.evaluate_group(i)
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
  -- the pill the tank shoots: the target, or another pill of its group
  -- whose chain kills the target (evaluate_group)
  local A = res.A or pill
  local out = V2.to_scan_result(res, A, standoff, function(mx, my)
    return shield.adjacent_friendly_pill(a.world, mx, my)
  end)
  out.num_pill_blockers = a.npb
  out.pill_wall_equiv = C.PPT_PILL_WALL_EQUIV or 3.0
  goal._pt2 = out.chain
  goal._pt2.tick = a.now
  goal._pt2.goal_target = { mx = pill.mx, my = pill.my }
  if A ~= pill then
    -- retarget: attack.lua takes sscan.pill as the pill for this take
    local id = inp.ids and inp.ids[A]
    if id then goal.target_id = id end
    goal.mx, goal.my = A.mx, A.my
    out.pt2_retarget = true
  end
  -- hold point: the tank waits here until every planned wall stands
  local h = b.hold
  if h then
    out.hold = { fx = h.fx, fy = h.fy, safe = h.safe }
    goal._pt2.hold = out.hold
  end
  return out
end

-- ── HOLD chains ──────────────────────────────────────────────────────────
-- The next live pill of the chain after the current target, or nil.
local function next_pill(goal, world)
  local ch = goal._pt2
  if not ch or ch.mode ~= "HOLD" or not ch.ids then return nil end
  local cur = ch.step or 1
  if ch.ids[cur] ~= goal.target_id then return nil end
  for k = cur + 1, #ch.ids do
    local id = ch.ids[k]
    local p = id and world.pills and world.pills[id]
    if p and (p.health or 0) > 0 and not p.in_tank and not p.pos_unknown then return p, id, k end
  end
  return nil
end

function M.has_next(goal, world)
  return next_pill(goal, world) ~= nil
end

-- The current chain pill is dead: aim at the next one from this same spot
-- and go back to the PPT aim path (in_range_aim -> finetune -> shoot_pill).
-- Returns true when the goal now targets the next pill.
function M.chain_next(state, goal, world, info, now)
  local B, bid, k = next_pill(goal, world)
  if not B then return false end
  local AT = require("attack")
  local idx, awx, awy = AT._clear_aim_from_world(info.tankx, info.tanky, B.mx, B.my, world, nil)
  if not idx then
    return false
  end
  goal._pt2.step = k
  -- the finish rule was for the pill just killed, not for the next one
  goal._pt2.finish = nil
  goal.target_id = bid
  goal.mx, goal.my = B.mx, B.my
  AT._set_goal_aim(goal, B.mx, B.my, idx, awx, awy)
  local sc = goal._shield_scan
  if sc then sc.pill = B; sc.pill_hp = B.health end
  goal.substate             = "in_range_aim"
  goal.aim_tick             = now
  goal._aim_locked          = nil
  goal._pre_aim_locked      = nil
  goal._finetune_taps       = 0
  goal._finetune_start      = nil
  goal._finetune_reached_tap = nil
  goal._finetune_path       = nil
  goal._finetune_on_pill    = nil
  goal._shoot_reach_checked = nil
  goal._shoot_first_steer   = nil
  -- a new pill: new shot accounting and a new no-progress clock
  goal._attack_start_tick   = nil
  goal._charge_start_hp     = nil
  goal._bullets_needed      = nil
  goal._kill_attempt        = nil
  goal._shoot_progress_hp   = nil
  goal._shoot_progress_tick = nil
  goal._on_target_in_flight = nil
  return true
end

-- ── Finish re-plan (PT2_FINISH_NO_COVER_MAX_ANGER) ───────────────────────
-- Called each think in the pre-shoot substates. The plan was made when the
-- pill did (or did not) qualify for the finish rule; anger falls with time
-- and armour or health can change, so when the answer now differs from
-- the plan's, the take plans again, once per change of the answer
-- (goal._pt2_fin_q keeps the last answer). Only with v2 driving (master on,
-- a protected take) and the knob at 0 or more. Returns true: re-plan now.
local PRE_SHOOT = { approach = true, aim = true, in_range_position = true,
                    in_range_aim_pre = true, in_range_aim = true, in_range_aim_finetune = true }
function M.finish_recheck(goal, info, pill, now, sub)
  if not C.PILLTAKE_V2_EVALUATOR or not goal._is_ppt or not PRE_SHOOT[sub] then return false end
  local m = C.PT2_FINISH_NO_COVER_MAX_ANGER or -1
  if m < 0 or not pill or (pill.health or 0) <= 0 then return false end
  -- a chain past its first pill shoots on from its spot
  if goal._pt2 and (goal._pt2.step or 1) > 1 then return false end
  local anger = M.anger_now(pill, now)
  local q = (pill.health or 0) == 1 and (info.armour or 0) >= (C.ATTACK_RUSH_MIN_ARMOUR or 5)
            and anger <= m
  local planned = (goal._pt2 and goal._pt2.finish) and true or false
  local prev = goal._pt2_fin_q
  goal._pt2_fin_q = q
  if q == planned or prev == q then return false end
  return true
end

return M
