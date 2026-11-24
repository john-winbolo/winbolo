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
local V2     = require("pt2_eval")   -- PT2_EVALUATOR: v2 or old
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
-- any other point 10000 + the spot's adjusted score (so greens always win),
-- except on a finish: then every point costs its travel only.
-- The builder's own trip ticks (pt2_start.trip_ticks): -1 when he cannot
-- get there (E4-4: no start), nil when there is no answer. v2 (inp.lgm_ticks, E4-5)
-- and the hold check (start_now_check) use the same walk.
local function lgm_ticks(ox, oy, mx, my, bmx, bmy)
  local t = cpf.lgm_travel_ticks_map(ox, oy, mx, my, bmx or mx, bmy or my,
    C.WALL_SHIELD_LGM_MAX_TICKS, C.WALL_SHIELD_LGM_STUCK_TICKS)
  if not t then return nil end
  if t < 0 then return -1 end
  return t
end

function M.build_inp(state, goal, info, world, pill, no_builder, npb, radius, now)
  -- P2: the tier row's evaluator (pt2_eval.use), kept in the inp
  V2.use(state)
  local inp = V2.knob_inp(C, {})
  inp._pt2_ev = V2.pick()
  -- debug profile (C.PT2_PROF_LOG): the world_inp part of build_inp
  local tw = BRAIN_DEBUG_MODE and C.PT2_PROF_LOG and clock_us()
  V2.world_inp(world, C, U, PC, cpf, inp, no_builder)
  if tw then M.prof_world_us = clock_us() - tw end
  inp.A = nil
  for _, p in ipairs(inp.pills) do
    if p == pill or (p.mx == pill.mx and p.my == pill.my and not p.in_tank) then inp.A = p; break end
  end
  if not inp.A then return nil end
  inp.radius = radius
  inp.num_pill_blockers = npb
  -- PT2_FAVOR_BLOCKER_PLACE: the pills we carry pick the blocker pill tiers
  inp.carried_pills = info.carried_pills or 0
  -- PT2_FINISH_NO_COVER_MAX_ANGER: our armour and each pill's anger now
  inp.armour = info.armour or 0
  -- PT2_LIVE_SHELL: the shadow's shells walk the live rules
  if C.PT2_LIVE_SHELL then V2.live_shell(inp.env, info.rules) end
  -- PT2_TANKSHOT_BLOCKERS: boats and the tanks we see stop our own shell
  if C.PT2_TANKSHOT_BLOCKERS then V2.tank_blockers(inp.env, info) end
  inp.anger_of = function(p) return M.anger_now(p, now) end
  -- PT2_START_WHILE_BUILDING: the pill reload ladder on the live rules
  if inp.start_while_building then
    local x = require("live_physics").context(info, {})
    inp.pill_attack_ticks, inp.pill_attack_min_ticks = x.pill_attack_ticks, x.pill_attack_min_ticks
    -- PT2_START_ANGER_RISE: the rest of the reload rules, our reload
    inp.pill_angry_divisor, inp.pill_cooldown_ticks = x.pill_angry_divisor, x.pill_cooldown_ticks
    inp.tank_reload = x.reload
    -- E4-5: the builder's own trips, as the hold check counts them
    inp.lgm_ticks = lgm_ticks
  end
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
  -- A finish (PT2_FINISH_NO_COVER_MAX_ANGER: A's ray needs no cover) is
  -- a charge with a swerve: v1's green filter is a blocker-era ranking, so
  -- it must not push the finish to a far spot. A finish point costs its
  -- travel only. pen_n / pen_skip_n count the points for the log.
  -- The travel of a finish point also leaves out A's own danger: the
  -- Dijkstra cost along the same path less A's stamped danger on it
  -- (threat.pill_contrib[A], the C subtract lookup goals.lua uses for a
  -- pill it will kill on the way). The danger of every other hostile pill
  -- stays in. noA_n counts the points priced so, for the log.
  inp.pen_n, inp.pen_skip_n, inp.noA_n = 0, 0, 0
  local th = package.loaded["threat"]
  local contrib_A, contrib_of = nil, nil
  -- nearest_spot per angle (the scan spots do not change) and finish_ok
  -- once per A: asked again when A changes, so a later group part (a
  -- later tick) reads the pill's health and anger again
  local spot_of, fin_A, fin_v = {}, nil, false
  inp.score_fn = function(deg, mx, my, cx, cy)
    local s = nil
    if curA == goalA then
      s = spot_of[deg]
      if s == nil then s = nearest_spot(spots, deg) or false; spot_of[deg] = s end
      s = s or nil
    end
    if fin_A ~= curA then fin_A, fin_v = curA, V2.finish_ok(inp, curA) or false end
    local fin = fin_v
    if s and not greens[s] then
      if fin then
        inp.pen_skip_n = inp.pen_skip_n + 1
      else
        inp.pen_n = inp.pen_n + 1
        return 10000 + (s._adj_score or 1000)
      end
    end
    local dij
    if fin then
      if contrib_of ~= curA then
        contrib_of = curA
        contrib_A = th and th.pill_contrib and th.pill_contrib[curA.my * 256 + curA.mx] or nil
      end
      if contrib_A then
        dij = cpf.dijkstra_lookup_subtract_by_kind(cpf.KIND_NORMAL, mx, my, boat, contrib_A)
        inp.noA_n = inp.noA_n + 1
      end
    end
    if not dij then dij = cpf.dijkstra_lookup_by_kind(cpf.KIND_NORMAL, mx, my, boat) end
    if dij >= 1e29 then dij = cpf.estimate_cost(tmx, tmy, mx, my, boat) end
    return dij + ((s and s._hd_cost) or 0)
  end
  -- PT2_CHAIN_CLAIM: chain pills an ally holds against us (pt2_claim.keep)
  if C.PT2_CHAIN_CLAIM then
    inp.chain_skip = require("pt2_claim").chain_skip(state, inp, now, (info and info.player_number) or -1)
  end
  return inp
end

-- v1's winner: point, aim, blockers and the cover they give.
-- approach_skip.shield_check reads the take's plan with it too: a v2 plan
-- (to_scan_result) marks each planned square pt2_kind "pill" or "wall".
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
      if bl.pt2_kind then sh = (bl.pt2_kind == "pill") and 15 or (C.SHIELD_WALL_COVER_SHOTS or 5) end
      if left > 0 then left = left - 1 end
      extra[bl.my * 256 + bl.mx] = sh
      cover = cover + sh
    end
  end
  return { cx = b.cx, cy = b.cy, deg = b.deg, aim = b.best_aim_idx, act = act, pot = pot,
           extra = extra, cover = cover }
end

M.v1_view = v1_view

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
  -- v1's point priced by v2's score (log only; read the counts first, the
  -- call adds to them)
  local pen_n, pen_skip_n, noA_n = inp.pen_n or 0, inp.pen_skip_n or 0, inp.noA_n or 0
  local v1_score = v1 and v1.deg and inp.score_fn
    and inp.score_fn(v1.deg, floor(v1.cx), floor(v1.cy), v1.cx, v1.cy) or nil
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
    .. " | v2: so=%s deg=%s R=%s aim=%s aimnb=%s bpt=%s wfar=%s carried=%d chain=%s mode=%s kills=%s need=%s have=%s safe=%s lee=%s plan=%s"
    .. " | diff=%s d_wu=%s why=%s drive=%s cost_us=%d evals=%d green_pen=%d green_pen_skip=%d travel_noA=%d noA_pts=%d v1_score=%s v2_score=%s",
    now, pill.mx, pill.my, pill.health or 0,
    v1 and string.format("(%.2f,%.2f)", v1.cx, v1.cy) or "-",
    v1 and string.format("%.1f", v1.deg or 0) or "-",
    v1 and tostring(v1.aim) or "-",
    v1 and sq_list(v1.act) or "-", v1 and sq_list(v1.pot) or "-",
    v1 and tostring(v1.cover) or "-",
    b and string.format("(%.2f,%.2f)", b.cx, b.cy) or "-",
    b and string.format("%.1f", b.deg) or "-",
    b and string.format("%.2f", b.radius or 0) or "-",
    b and tostring(b.aim_idx) or "-",
    b and b.aimnb ~= nil and (b.aimnb and "1" or "0") or "-",
    b and b.bp_tier ~= nil and tostring(b.bp_tier) or "-",
    b and b.wall_far ~= nil and tostring(b.wall_far) or "-", inp.carried_pills or 0,
    chain, mode_of(b),
    b and tostring(b.kills) or "-", b and tostring(b.need) or "-", b and tostring(b.have) or "-",
    tostring(v2ok and true or false), b and tostring(b.leeway) or "-", plan,
    diff, d_wu and string.format("%.0f", d_wu) or "-", why,
    drive and "v2" or "v1", cost_us, res and res.n_eval or 0, pen_n, pen_skip_n,
    noA_n > 0 and 1 or 0, noA_n,
    v1_score and string.format("%.1f", v1_score) or "-", b and string.format("%.1f", b.score or 0) or "-")
  if do_print then print2(line) end
  return line
end

-- Called with v1's sscan; returns the sscan to use. See the file header.
-- a = { state, goal, info, world, pill, sscan, no_builder, npb, radius, now }
-- Does v2 run for this take? false: after_scan hands v1's sscan back as is.
local function v2_wanted(goal)
  local master = C.PILLTAKE_V2_EVALUATOR
  local log = BRAIN_DEBUG_MODE and C.PT2_SIDE_BY_SIDE_LOG and _G._PRINT2_ENABLED
  if not master and not log then return false end
  if master and not goal._is_ppt and not log then return false end
  return true
end

-- PT2_V2_FIRST: does v2 run before v1's shield.scan on this take? Only
-- when v2 can drive (master on, a protected take) and nothing needs v1's
-- answer next to v2's (the PT2_CMP log, a pt2_* BrainTest layer). Then
-- attack.lua runs the scan only when v2 hands back no plan of its own
-- (M.v1_needed).
function M.v2_first(goal)
  if not (C.PT2_V2_FIRST and C.PILLTAKE_V2_EVALUATOR and goal._is_ppt) then return false end
  return true
end

-- PT2_V2_FIRST: v2's answer is not a plan of its own (no safe point, an
-- error, a give-up, no input): v1's shield.scan must run.
function M.v1_needed(sscan)
  return not (sscan and sscan.pt2)
end

-- The part of after_scan after the evaluate: inp/res from build_inp and
-- evaluate_group, cost their time in us. The split run (PT2_AFTER_SCAN_SPLIT)
-- ends here too, so both give the same answer for the same inp and res.
local function finish(a, inp, res, cost)
  local master = C.PILLTAKE_V2_EVALUATOR
  local log = BRAIN_DEBUG_MODE and C.PT2_SIDE_BY_SIDE_LOG and _G._PRINT2_ENABLED
  local goal, pill = a.goal, a.pill
  local viz_on = BRAIN_DEBUG_MODE and PV.any_on()
  if not inp or not res then return a.sscan end
  -- PT2_DOUBLE_TAKE_DISCOUNT: pt2_dbl's first shadow run uses this as its cost
  if C.PT2_DOUBLE_TAKE_DISCOUNT and a.state then a.state._pt2_v2_last_us = cost end
  -- before cmp_line: its v1 price adds to the counts
  local noA_n = inp.noA_n or 0
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
  -- PT2_START_WHILE_BUILDING: v2's answer for this point (start_check);
  -- the hold point asks again with the live builder (M.start_now_check).
  -- The background walls (M.bg_wall) start empty on each new plan, but
  -- E5-12: a plan made after the hold (old bg.go) keeps the old missing
  -- walls (below).
  local old_bg = goal._pt2_bg
  goal._pt2_bg = nil
  if b.start_now ~= nil then
    goal._pt2.start = { ok = b.start_now, why = b.start_why, pills = b.start_pills,
                        exposure = b.exposure, shells = b.shells_through, chain = b.start_chain,
                        cx = b.cx, cy = b.cy }
  end
  -- PT2_EXTRA_COVER_WALLS: v2's extra walls for a safe point (after the
  -- rank). The builder builds them while the take goes on, only when he
  -- is free (builder.set_mode, M.bg_wall); the tank never waits for them.
  if b.extra_cover and #b.extra_cover > 0 then
    local walls = {}
    for i, w in ipairs(b.extra_cover) do walls[i] = { mx = w.mx, my = w.my } end
    local plan = {}
    for i, q in ipairs(b.plan or {}) do plan[i] = { mx = q.mx, my = q.my } end
    goal._pt2_bg = { walls = walls, plan = plan }
    goal._pt2.extra_cover = walls
  end
  -- E5-12: a new plan during a take (the old hold was over: bg.go) keeps
  -- the old background walls that do not stand yet. The new plan's own
  -- squares are left out: its hold builds them. PT2_FINISH_IN_TAKE counts
  -- how often this happens.
  if old_bg and old_bg.go then
    goal._pt2_finish_in_take = (goal._pt2_finish_in_take or 0) + 1
    local on_plan = {}
    for _, q in ipairs(b.plan or {}) do on_plan[q.my * 256 + q.mx] = true end
    local bg = goal._pt2_bg
    local have = {}
    for _, w in ipairs(bg and bg.walls or {}) do have[w.my * 256 + w.mx] = true end
    local kept = {}
    for _, w in ipairs(old_bg.walls or {}) do
      local k = w.my * 256 + w.mx
      if not on_plan[k] and not have[k]
         and PC.slot_cover_shots(a.world, w.mx, w.my, U.ttype(w.mx, w.my)) <= 0 then
        kept[#kept + 1] = { mx = w.mx, my = w.my }
        have[k] = true
      end
    end
    if #kept > 0 then
      if not bg then
        local plan = {}
        for i, q in ipairs(b.plan or {}) do plan[i] = { mx = q.mx, my = q.my } end
        bg = { walls = {}, plan = plan }
        goal._pt2_bg = bg
      end
      for _, w in ipairs(kept) do bg.walls[#bg.walls + 1] = w end
    end
  end
  -- hold point: the tank waits here until every planned wall stands
  local h = b.hold
  if h then
    out.hold = { fx = h.fx, fy = h.fy, safe = h.safe }
    goal._pt2.hold = out.hold
  end
  return out
end

-- ── PT2_START_WHILE_BUILDING (E4): the hold point check ─────────────────
-- attack.lua's v2 wall hold calls this while planned walls are missing.
-- v2 said the point may start (goal._pt2.start.ok); this asks again with
-- what the bot sees now: the builder's place (on the ground, else the
-- tank), the missing walls in the hold's build order, the builder's own
-- trip ticks (cpathfinder.lgm_travel_ticks_map, as LAST_WALL_EARLY), each
-- start pill's anger now and the cover on its ray that stands now (v2's
-- cover_now plus the planned walls of that ray already up). On a yes the
-- missing walls go to goal._pt2_bg: the builder builds them while the take
-- goes on (M.bg_wall). Asked at most every START_CHECK_TICKS.
M.START_CHECK_TICKS = 10
local PS = require("pt2_start")


local function wall_up(world, mx, my)
  return PC.slot_cover_shots(world, mx, my, U.ttype(mx, my)) > 0
end

-- PT2_START_ANGER_RISE at the hold point: v2's chain (start_check) with
-- the live pills, anger and walls. A planned wall that stands now covers
-- from tick 0; a missing one from its finish tick (the order of `walls`,
-- each later trip from the tank). A dead chain pill neither fires nor is
-- hit.
function M.rise_now(chain, world, info, now, walls, ox, oy, exp, x)
  local ts = PS.wall_ticks(ox, oy, walls, lgm_ticks, nil,
    bit.rshift(info.tankx or 0, 8), bit.rshift(info.tanky or 0, 8)) or {}
  local wt = {}
  for i, w in ipairs(walls) do wt[w.my * 256 + w.mx] = ts[i] end
  local entries = {}
  for k, c in ipairs(chain) do
    local p = PC.pill_on_square(world, c.mx, c.my)
    local e = { to = c.to, cover_now = c.cover_now, planned = c.planned, count = c.count }
    if p and (p.health or 0) > 0 then
      e.speed = PS.pill_interval(M.anger_now(p, now), C.PILL_ANGER_BUMP,
        x.pill_attack_ticks, x.pill_attack_min_ticks, x.pill_angry_divisor)
    else
      e.speed, e.off = x.pill_attack_ticks or 100, true
    end
    entries[k] = e
  end
  local A = chain[1]
  local o = { attack_ticks = x.pill_attack_ticks, min_ticks = x.pill_attack_min_ticks,
              divisor = x.pill_angry_divisor, cooldown = x.pill_cooldown_ticks,
              tank_reload = x.reload,
              flight = A and PS.flight_ticks(info.tankx or 0, info.tanky or 0, A.mx, A.my, x.shell_speed) or 0 }
  return (PS.rise_chain(entries, exp, function(mx, my)
    local t = wt[my * 256 + mx]
    if t then return t end
    if wall_up(world, mx, my) then return 0 end
    return nil
  end, o))
end

-- The trees the planned squares of a take need: a v2 plan square marked
-- pt2_kind "pill" (to_scan_result) is a carried pill drop, which costs
-- PILL_PLACE_TREE_COST (the engine's LGM_COST_PILLNEW), not a wall.
-- A v1 square (no pt2_kind) counts as a wall, as before.
function M.plan_trees(pots)
  local n = 0
  for _, p in ipairs(pots or {}) do
    if p.pt2_kind == "pill" then n = n + (C.PILL_PLACE_TREE_COST or 4)
    else n = n + (C.WALL_SHIELD_BUILD_COST or 2) end
  end
  return n
end

-- PT2_START_BUILDER_SAFE: does a predicted pill shell kill the builder on
-- his remaining wall trips (out, build, home; pt2_start.trip_timeline, the
-- same trips as exposure_ticks) if the take starts now? The shell model is
-- quick_repair's (Rule 2): every live non-friendly pill whose range reaches
-- the tank's point (the start record's cx, cy, else the tank now) fires at
-- it, the first shell at its next fire tick (quick_repair.pill_next_fire),
-- then one per reload at its anger now (pt2_start.pill_interval). Each shell
-- explodes where the engine ends it (quick_repair.shell_end): on the first
-- square that stops it, a planned wall included from the tick the builder
-- reaches it (lgmDoWork puts the wall up on arrival), else on the tank or at
-- the end of its range. It kills him if he is there then
-- (quick_repair.blast_catches_man: on that square for a square hit, else
-- in the blast area). Returns nil (safe) or
-- { id, t (ticks from now), sq_mx, sq_my, ex, ey, lx, ly }.
-- opts (all optional):
--   legs, home: the builder's timeline (pt2_start.trip_timeline's form)
--     in place of the trips to `walls` from (ox, oy);
--   tx, ty: the tank's point (tiles) in place of the start record's;
--   fire = true: the tank shoots the target pill (goal.mx, goal.my) from
--     the next tick. Each tank shell lands after its flight
--     (pt2_start.flight_ticks) and one tank reload (x.reload) after the
--     one before; each landed shell is one more hit on that pill, so its
--     reload is pt2_start.pill_interval at its anger now plus one
--     PILL_ANGER_BUMP per landed hit (the same ladder as rise_shells).
--     The pill fires when the ticks since its last shot reach the reload
--     of that tick.
-- PT2_START_BUILDER_SAFE: does a hostile pill shell already in flight
-- (info.objects, owner 255) kill the builder on his trip (legs, home: ticks
-- from now)? run_c4: the hold said "safe -> go" on the tick the pill fired,
-- because only the pill's NEXT shots were looked at; the shell it had just
-- fired killed him 12 to 22 ticks later. Each shell flies SHELL_SPEED a tick
-- along its angle for the rest of its life (shells_on_blocker's flight).
-- It stops on a square that stops shells (danger.shell_stops_on, or a trip
-- wall up by then: up_at) or on our tank (TANK_HIT_RADIUS_WU), else it
-- bursts at the end of its life. Returns builder_trip_hit's hit or nil.
function M.inflight_hit(world, info, legs, home, up_at)
  local QR = require("quick_repair")
  local danger = require("danger")
  local N = C.LGM_SHELL_PREDICT_TICKS or 63
  local spd = (C.SHELL_SPEED or 32) / 256
  local tx, ty = (info.tankx or 0) / 256, (info.tanky or 0) / 256
  local tr = (C.TANK_HIT_RADIUS_WU or 112) / 256
  for _, ob in ipairs(info.objects or {}) do
    if ob.type == (OBJECT_SHOT or 1) and (ob.owner or 255) == 255 and (ob.info or 0) ~= 0
       and ob.x and ob.y then
      local life = ob.life
      local moves = (type(life) == "number" and life >= 0 and life <= N) and life or N
      local vx = U.bsin_f(ob.angle or 0) * spd
      local vy = -U.bcos_f(ob.angle or 0) * spd
      local sx, sy = ob.x / 256, ob.y / 256
      local smx0, smy0 = floor(sx), floor(sy)
      local e
      for k = 1, moves do
        if k > home then break end
        sx, sy = sx + vx, sy + vy
        local mx, my = floor(sx), floor(sy)
        local t = up_at[my * 256 + mx]
        -- The square the shell is in now is skipped for what stops shells
        -- now (it is flying out of it), but not for a trip wall that goes
        -- up later (run_c6 diag_s: the wall went up 3 ticks after the
        -- check with the shell still in its square, and killed him).
        local start = mx == smx0 and my == smy0
        if (not start and danger.shell_stops_on(world, mx, my))
           or (t ~= nil and k >= t and (not start or t >= 1)) then
          e = { t = k, ex = mx + 0.5, ey = my + 0.5, sq_mx = mx, sq_my = my, how = "square" }
          break
        end
        local dx, dy = sx - tx, sy - ty
        if dx * dx + dy * dy <= tr * tr then
          e = { t = k, ex = sx, ey = sy, how = "tank" }
          break
        end
        if k == moves then e = { t = k, ex = sx, ey = sy, how = "range" } end
      end
      if e and e.t <= home then
        local lx, ly = PS.man_at(legs, e.t)
        if lx and QR.blast_catches_man(e, lx, ly) then
          return { id = "shell", t = e.t, sq_mx = e.sq_mx, sq_my = e.sq_my,
                   ex = e.ex, ey = e.ey, lx = lx, ly = ly, how = e.how }
        end
      end
    end
  end
  return nil
end

function M.builder_trip_hit(goal, world, info, now, walls, ox, oy, x, opts)
  opts = opts or {}
  local QR = require("quick_repair")
  local tmx, tmy = bit.rshift(info.tankx or 0, 8), bit.rshift(info.tanky or 0, 8)
  local legs, home = opts.legs, opts.home
  if not legs then
    legs, home = PS.trip_timeline(ox, oy, walls, lgm_ticks, nil, tmx, tmy)
  end
  if not legs or not home or home <= 0 then return nil end
  local st = goal._pt2 and goal._pt2.start
  local tx, ty = (info.tankx or 0) / 256.0, (info.tanky or 0) / 256.0
  if opts.tx and opts.ty then tx, ty = opts.tx, opts.ty
  elseif st and st.cx and st.cy then tx, ty = st.cx, st.cy end
  local range = (x.pill_range or 2048) / 256
  local bump = C.PILL_ANGER_BUMP
  -- the tank's shells on the target pill (opts.fire)
  local hit_flight, hit_reload
  if opts.fire and goal.mx and goal.my then
    hit_flight = PS.flight_ticks(info.tankx or 0, info.tanky or 0, goal.mx, goal.my,
                                 x.shell_speed) or 0
    hit_reload = x.reload or 13
    if hit_reload < 1 then hit_reload = 1 end
  end
  -- the tick each missing wall goes up (arrival)
  local up_at = {}
  for _, L in ipairs(legs) do
    if L.kind == "build" then
      up_at[floor(L.y0) * 256 + floor(L.x0)] = L.t0
    end
  end
  local fl = M.inflight_hit(world, info, legs, home, up_at)
  if fl then return fl end
  local guns = QR.guns_at(world, tx, ty, range)
  for gi = 1, #guns do
    local g = guns[gi]
    local nxt, _, last = QR.pill_next_fire(g.p, info, now, x)
    local a0 = M.anger_now(g.p, now)
    local shot_at = hit_flight and g.p.mx == goal.mx and g.p.my == goal.my
    -- this pill's reload at tick t (ticks from now)
    local function iv_at(t)
      local a = a0
      if shot_at and t >= 1 + hit_flight then
        a = a + bump * (floor((t - 1 - hit_flight) / hit_reload) + 1)
      end
      local iv = PS.pill_interval(a, bump, x.pill_attack_ticks,
        x.pill_attack_min_ticks, x.pill_angry_divisor)
      if not iv or iv < 1 then iv = 1 end
      return iv
    end
    -- its first fire tick at or after `from` (absolute), its last at `prev`
    local function next_fire(from, prev)
      if not prev then return from end
      local t = from
      while t - now <= home and t - prev < iv_at(t - now) do t = t + 1 end
      return t
    end
    -- run_c6 (pillbox.c justSeen): when no shell of this pill was seen
    -- since it first saw our tank, its reload started from 0 on that tick
    -- and it fires once a reload from then. Its last fire is the latest
    -- such tick at or before now (the sight tick itself before the first).
    local since = g.p._tank_seen_since
    if since and since <= now and (not last or since > last) then
      local iv0 = iv_at(0)
      last = since + floor((now - since) / iv0) * iv0
      nxt = nil
    end
    local f = next_fire(now + 1, last)
    if nxt and nxt < f then f = nxt end
    while f - now <= home do
      local f0 = f - now
      local e = QR.shell_end(world, g, tx, ty, range, function(mx, my, k)
        local t = up_at[my * 256 + mx]
        return t ~= nil and f0 + k >= t
      end)
      local at = f0 + e.t
      if at <= home then
        local lx, ly = PS.man_at(legs, at)
        if lx and QR.blast_catches_man(e, lx, ly) then
          return { id = g.id, t = at, sq_mx = e.sq_mx, sq_my = e.sq_my,
                   ex = e.ex, ey = e.ey, lx = lx, ly = ly, how = e.how }
        end
      end
      f = next_fire(f + 1, f)
    end
  end
  return nil
end

-- PT2_START_BUILDER_SAFE once the hold is over (run_c3_beside_e): the
-- builder's current take trip. When he is out (LGM_MOVING) on a take wall
-- (state._wall_shield_dispatch: a background wall, a wall_shield wall or a
-- planned wall), the rest of his trip from where he is now: out to the
-- wall, build, home (the wall is not up yet); build, home (he is on it);
-- or home (the wall stands and he is off it). A build under way counts as
-- a whole LGM_BUILD_TIME. Returns legs, home (pt2_start.trip_timeline's
-- form) or nil.
function M.current_trip(state, goal, world, info, now)
  if info.man_status ~= C.LGM_MOVING then return nil end
  local d = state and state._wall_shield_dispatch
  if not d or not d.wx or goal.kind ~= "attack_pill" then return nil end
  local mx, my = bit.rshift(info.man_x or 0, 8), bit.rshift(info.man_y or 0, 8)
  local tmx, tmy = bit.rshift(info.tankx or 0, 8), bit.rshift(info.tanky or 0, 8)
  local up = wall_up(world, d.wx, d.wy)
  local on = mx == d.wx and my == d.wy
  -- The estimate below counts a whole LGM_BUILD_TIME from now while he is
  -- on the wall, and walks him home in a straight line at the lgm_ticks
  -- pace. run_c4 beside_n: the walk home from the wall (122,116) to the
  -- tank at (120,113) runs into our own new pill at (121,115), so the
  -- engine walk is not that line. So when the brain tracks him
  -- (danger.own_builder_timeline: his real arrival, the rest of his stay,
  -- the engine's own walk home) that is his trip, up to
  -- BUILDER_POOL_LGM_STUCK_TICKS ahead. The wall goes up the first tick he
  -- is inside the engine's goal box (LGM_MIN_GOAL .. LGM_MAX_GOAL, 16 WU
  -- on each axis from the square centre) while it is not up yet; entering
  -- the square is not enough (run_c6 diag_s).
  if now then
    local cap = C.BUILDER_POOL_LGM_STUCK_TICKS or 150
    local tl = require("danger").own_builder_timeline(state, info, now, cap)
    if tl then
      local legs, home, built = {}, cap, up
      for t = 0, cap - 1 do
        local a, b = tl[t], tl[t + 1]
        if not a then home = t; break end
        if not built and math.abs(a[1] - (d.wx + 0.5)) * 256 <= 16
           and math.abs(a[2] - (d.wy + 0.5)) * 256 <= 16 then
          built = true
          legs[#legs + 1] = { t0 = t, t1 = t, x0 = d.wx + 0.5, y0 = d.wy + 0.5,
                              x1 = d.wx + 0.5, y1 = d.wy + 0.5, kind = "build", i = 1 }
        end
        b = b or a
        legs[#legs + 1] = { t0 = t, t1 = t + 1, x0 = a[1], y0 = a[2],
                            x1 = b[1], y1 = b[2], kind = "back", i = 1 }
        if not tl[t + 1] then home = t + 1; break end
      end
      return legs, home
    end
  end
  if not up and not on then
    return PS.trip_timeline(mx, my, { { mx = d.wx, my = d.wy } }, lgm_ticks, nil, tmx, tmy)
  end
  local lx, ly = (info.man_x or 0) / 256.0, (info.man_y or 0) / 256.0
  local legs, t = {}, 0
  if on then
    legs[1] = { t0 = 0, t1 = PS.LGM_BUILD_TIME, x0 = d.wx + 0.5, y0 = d.wy + 0.5,
                x1 = d.wx + 0.5, y1 = d.wy + 0.5, kind = "build", i = 1 }
    t = PS.LGM_BUILD_TIME
    lx, ly = d.wx + 0.5, d.wy + 0.5
  end
  local back = PS.trip_ticks(mx, my, tmx, tmy, lgm_ticks, mx, my)
  if not back then return nil end
  legs[#legs + 1] = { t0 = t, t1 = t + back, x0 = lx, y0 = ly,
                      x1 = tmx + 0.5, y1 = tmy + 0.5, kind = "back", i = 1 }
  return legs, t + back
end

-- Log a builder-safety answer once per change. what: "fire" (the fire
-- hold), "bg_wall=(x,y)" (a background wall trip) or "shield_wall=(x,y)"
-- (a take wall trip). act: the unsafe answer's action ("wait" if nil).
local function bsafe_log(goal, now, what, hit, act)
  local key = ""
  if hit then
    key = string.format("%s:%s:%s", tostring(hit.id), tostring(hit.sq_mx or floor(hit.ex)),
                        tostring(hit.sq_my or floor(hit.ey)))
  end
  local tag = what .. "|" .. key
  local field = what == "fire" and "_pt2_bsafe_fire"
    or (what:sub(1, 12) == "shield_wall=" and "_pt2_bsafe_shield" or "_pt2_bsafe_bg")
  local was = goal[field]
  if was == tag then return end
  goal[field] = tag
  if hit then
  elseif was and was:sub(-1) ~= "|" then
  end
end

-- PT2_START_BUILDER_SAFE: hold the take's fire? True when the builder is
-- out on a take wall (M.current_trip) and a pill shell kills him on the rest
-- of his trip with the tank firing at the target pill (builder_trip_hit,
-- fire = true). The caller stays in its pre-fire substate and asks again
-- next tick.
function M.builder_fire_hold(state, goal, world, info, now)
  if not C.PT2_START_BUILDER_SAFE then return false end
  local legs, home = M.current_trip(state, goal, world, info, now)
  local hit
  if legs then
    local x = require("live_physics").context(info, {})
    hit = M.builder_trip_hit(goal, world, info, now, {}, 0, 0, x,
      { legs = legs, home = home, fire = true,
        tx = (info.tankx or 0) / 256.0, ty = (info.tanky or 0) / 256.0 })
  end
  bsafe_log(goal, now, "fire", hit)
  return hit ~= nil
end

-- PT2_START_BUILDER_SAFE: may the builder go out to background wall w
-- (M.bg_wall) now? The take fires during his trip, or soon, so the trip
-- (tank square to w, build, home) is judged with the tank firing at the
-- target pill. False = keep him in this tick. The tank never waits for a
-- background wall, so the take goes on without it.
function M.bg_trip_ok(state, goal, world, info, now, w)
  if not C.PT2_START_BUILDER_SAFE then return true end
  local x = require("live_physics").context(info, {})
  local tmx, tmy = bit.rshift(info.tankx or 0, 8), bit.rshift(info.tanky or 0, 8)
  local hit = M.builder_trip_hit(goal, world, info, now, { w }, tmx, tmy, x, { fire = true })
  bsafe_log(goal, now, string.format("bg_wall=(%d,%d)", w.mx, w.my), hit)
  return hit == nil
end

-- PT2_START_BUILDER_SAFE (Nov 24, Andrew's option a): may the builder go
-- out from the tank to the take's own wall w (a v2 take's wall_shield wall)
-- now? His trip (tank square to w, build, home) is judged as a background
-- wall's: pill shells in flight, and every pill in range of the tank firing
-- at it by itself (no tank fire needed); the tank's own fire counts only
-- when the take may start while he builds (the start check passed). Unsafe:
-- he stays in and w is dropped from the take's wall list (not waited for);
-- the take goes on with its other blockers. A dropped wall stays dropped
-- for that wall list; a new plan (a new list) checks it again.
function M.shield_wall_ok(state, goal, world, info, now, w)
  if not C.PT2_START_BUILDER_SAFE then return true end
  if M.shield_wall_skipped(goal, w.mx, w.my) then return false end
  local x = require("live_physics").context(info, {})
  local tmx, tmy = bit.rshift(info.tankx or 0, 8), bit.rshift(info.tanky or 0, 8)
  local st = goal._pt2 and goal._pt2.start
  local hit = M.builder_trip_hit(goal, world, info, now, { { mx = w.mx, my = w.my } }, tmx, tmy, x,
    { fire = (st and st.ok) and true or false,
      tx = (info.tankx or 0) / 256.0, ty = (info.tanky or 0) / 256.0 })
  if not hit then return true end
  local sk = goal._pt2_shield_skip
  if not sk or sk.list ~= goal._wall_build_list then
    sk = { list = goal._wall_build_list, sq = {} }
    goal._pt2_shield_skip = sk
  end
  sk.sq[w.my * 256 + w.mx] = true
  bsafe_log(goal, now, string.format("shield_wall=(%d,%d)", w.mx, w.my), hit, "skip")
  return false
end

-- Was the take wall (mx, my) dropped from this wall list (shield_wall_ok)?
function M.shield_wall_skipped(goal, mx, my)
  local sk = goal._pt2_shield_skip
  return (sk and sk.list == goal._wall_build_list and sk.sq[my * 256 + mx]) and true or false
end

function M.start_now_check(goal, world, info, now, list)
  if C.PT2_START_WHILE_BUILDING == false then return false end
  local st = goal._pt2 and goal._pt2.start
  if not st or not st.ok then return false end
  if goal._pt2_start_tick and now - goal._pt2_start_tick < M.START_CHECK_TICKS then return false end
  goal._pt2_start_tick = now
  local walls = {}
  for _, p in ipairs(list) do
    if not wall_up(world, p.mx, p.my) then
      -- A planned pill drop is placed by the hold only: a background
      -- wall never places a carried pill (E5-10), so the take waits for it.
      if p.pt2_kind == "pill" then
        return false
      end
      walls[#walls + 1] = { mx = p.mx, my = p.my }
    end
  end
  if #walls == 0 then return false end
  -- E4-6: a dead builder builds no wall: no start
  if info.man_status == C.LGM_DEAD then
    return false
  end
  local ox, oy
  if info.man_status ~= C.LGM_INTANK and info.man_x and info.man_y then
    ox, oy = bit.rshift(info.man_x, 8), bit.rshift(info.man_y, 8)
  else
    ox, oy = bit.rshift(info.tankx or 0, 8), bit.rshift(info.tanky or 0, 8)
  end
  -- E4-1: each later wall's order starts at the tank
  local exp = PS.exposure_ticks(ox, oy, walls, lgm_ticks, nil,
    bit.rshift(info.tankx or 0, 8), bit.rshift(info.tanky or 0, 8))
  if not exp then
    return false
  end
  local x = require("live_physics").context(info, {})
  local shells = 0
  if st.chain and PS.rise_on() then
    shells = M.rise_now(st.chain, world, info, now, walls, ox, oy, exp, x)
  else
  for _, sp in ipairs(st.pills or {}) do
    local p = PC.pill_on_square(world, sp.mx, sp.my)
    if p and (p.health or 0) > 0 then
      local cover = sp.cover_now or 0
      for _, q in ipairs(sp.planned or {}) do
        if wall_up(world, q.mx, q.my) then cover = cover + (q.shots or 0) end
      end
      local iv = PS.pill_interval(M.anger_now(p, now), C.PILL_ANGER_BUMP,
                                  x.pill_attack_ticks, x.pill_attack_min_ticks)
      shells = shells + PS.shells_through(exp, iv, cover)
    end
  end
  end
  local armour = info.armour or 0
  local ok = PS.can_start(armour, shells, C.ATTACK_RUSH_MIN_ARMOUR or 5)
  -- PT2_START_BUILDER_SAFE: the armour holds, but would a pill shell kill
  -- the builder on his remaining wall trips?
  if ok and C.PT2_START_BUILDER_SAFE then
    local hit = M.builder_trip_hit(goal, world, info, now, walls, ox, oy, x)
    if hit then
      local hx = hit.sq_mx or floor(hit.ex)
      local hy = hit.sq_my or floor(hit.ey)
      return false
    end
  end
  if not ok then return false end
  local bg = goal._pt2_bg or { walls = {} }
  local have = {}
  for _, w in ipairs(bg.walls) do have[w.my * 256 + w.mx] = true end
  local merged = {}
  for _, w in ipairs(walls) do merged[#merged + 1] = w end
  for _, w in ipairs(bg.walls) do
    local seen = false
    for _, m in ipairs(walls) do if m.mx == w.mx and m.my == w.my then seen = true end end
    if not seen then merged[#merged + 1] = w end
  end
  bg.walls = merged
  bg.go = true
  goal._pt2_bg = bg
  return true
end

-- The next background wall for the builder (builder.set_mode), or nil.
-- Walls that stand leave the list. E5-9: nothing until the hold is over
-- (bg.go: start_now_check said yes, or every planned wall, bg.plan,
-- stands), and only in the take substates after the hold (an allow
-- list), so the hold's own wall queue keeps the builder.
local BG_TAKE = { aim = true, blitz_wait = true, in_range_position = true,
                  charge = true, engage = true, post_engage = true,
                  kill_hardline = true }
M.BG_TAKE = BG_TAKE
function M.bg_wall(goal, world)
  local bg = goal and goal._pt2_bg
  if not bg or not BG_TAKE[goal.substate or ""] then return nil end
  if not bg.go then
    for _, q in ipairs(bg.plan or {}) do
      if not wall_up(world, q.mx, q.my) then return nil end
    end
    bg.go = true
  end
  while bg.walls[1] do
    local w = bg.walls[1]
    if not wall_up(world, w.mx, w.my) then return w end
    table.remove(bg.walls, 1)
  end
  return nil
end

function M.after_scan(a)
  if not v2_wanted(a.goal) then return a.sscan end
  local t0 = clock_us()
  -- A v2 bug must never cost the take: v1's result stands. A budget kill
  -- is passed on, as attack.lua does for shield.scan.
  local viz_on = BRAIN_DEBUG_MODE and PV.any_on()
  local ok, inp, res = pcall(function()
    local i = M.build_inp(a.state, a.goal, a.info, a.world, a.pill, a.no_builder, a.npb, a.radius, a.now)
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
  return finish(a, inp, res, clock_us() - t0)
end

-- ── SHIELD_SCAN_PENDING_SURVIVES_V2: v2 in one go, across a budget kill ──
-- With PT2_AFTER_SCAN_SPLIT off, attack.lua runs v2 in one go on the plan
-- tick, after v1's shield.scan. Before, attack.lua cleared
-- goal._shield_scan_pending before that run and set goal._shield_scan only
-- after it, so a budget kill inside v2 left neither: the next tick drove in
-- with no plan (20261121_002624, bot1 t=9247 and t=9685: a safe double take
-- found, no walls built). With the knob on:
--   * attack.lua keeps the flag until goal._shield_scan is set
--     (M.keeps_pending), as the split run does;
--   * the next tick takes v1's answer from v1's own cache (same scan key,
--     SHIELD_SCAN_CACHE_TICKS) and runs v2 again with a fresh budget;
--   * a marker written before the run counts the kills on this scan key;
--     after SHIELD_SCAN_BLACKLIST_TRIES kills v2 is given up for the key
--     and v1's answer stands (as an evaluator error does). The marker goes
--     when a run ends, when the key changes, or after
--     SHIELD_SCAN_BLACKLIST_TICKS.
-- Off: after_scan as before.
function M.keeps_pending(split_pa)
  return (split_pa or C.SHIELD_SCAN_PENDING_SURVIVES_V2) and true or false
end

function M.one_go(a, key)
  if not C.SHIELD_SCAN_PENDING_SURVIVES_V2 then return M.after_scan(a) end
  if not v2_wanted(a.goal) then return a.sscan end
  local st = a.state or {}
  local now = a.now or 0
  local att = st._pt2_v2_attempt
  if att and (att.key ~= key or now - (att.tick or 0) > (C.SHIELD_SCAN_BLACKLIST_TICKS or 250)) then
    att = nil
    st._pt2_v2_attempt = nil
  end
  if att and (att.count or 0) >= (C.SHIELD_SCAN_BLACKLIST_TRIES or 2) then
    if C.PILLTAKE_V2_EVALUATOR then a.goal._pt2 = nil end
    M.last_run = { parts = 1, work = 0, kills = att.count, ticks = 0, giveup = "kills" }
    return a.sscan
  end
  if att then
    att.count, att.tick = (att.count or 0) + 1, now
  else
    st._pt2_v2_attempt = { key = key, count = 1, tick = now }
  end
  -- a budget kill unwinds from here with the marker set
  local out = M.after_scan(a)
  st._pt2_v2_attempt = nil
  return out
end

-- ── PT2_AFTER_SCAN_SPLIT: v2 after the shield scan, in parts over ticks ──
-- The plan tick (attack.lua's deferred shield scan) runs only v1's
-- shield.scan. v2 runs on the next brain ticks, in parts: part 0 is
-- build_inp, part 1 is v2 on the target, each later part is v2 with one
-- more pill of the group as A (pilltake_v2_old.group_step). The parts are
-- spread over at most PT2_AFTER_SCAN_MAX_TICKS work ticks: each work tick
-- runs its share of the parts left, and more while this think so far plus
-- one part's estimate stays under the tick target (pt2_dbl's safety net).
-- The last work tick runs all parts left. finish() then makes the same
-- answer as after_scan in one go.
-- PT2_SPLIT_ONE_PART_PER_TICK: each work tick runs one heavy part (part 1
-- and on) at most, part 0 with part 1; no work-tick maximum then.
--
-- Budget kills, as attack.lua does for v1's shield.scan:
--   * attack.lua keeps goal._shield_scan_pending until goal._shield_scan is
--     set, so the take stays in its plan, as before v1's answer.
--   * goal._pt2_split keeps the v1 result and the parts done. A part marks
--     itself (sp.in_part) before it starts and clears the mark when it
--     ends; a kill leaves the mark, and the next tick puts the part's
--     inputs back and runs that part again. The parts done stay done.
--   * the same part killed SHIELD_SCAN_BLACKLIST_TRIES times: give up v2 for
--     this plan, keep v1's sscan (PT2_SPLIT_GIVEUP), as an evaluator error
--     does today. So does a run dropped SHIELD_SCAN_BLACKLIST_TRIES times
--     in a row.
--   * finish() (the step after the last part) is marked the same way.
-- A killed tick is not a work tick. Each part and finish() can be killed
-- once and run again (the second kill gives up), so a run that ends with
-- v2's answer takes at most MAX_TICKS + parts + 1 ticks after the plan tick
-- (parts = 2 + the group pills in pill range; 5 + 2 + n). A run that gives
-- up ends on the tick after the second kill.
--
-- The run is dropped (and starts again from v1's scan) when it goes stale:
-- the v1 scan key changes (target square or health, standoff, angle, scan
-- radius, LGM dead or alive, our armour, carried pills), the target id
-- changes, the target's owner changes, a pill of v2's input list changes
-- (health, owner, square, carried), or it is older than
-- SHIELD_SCAN_CACHE_TICKS (v1's result would be old in v1's own cache).

local function snap_pills(inp)
  local sn = {}
  for i, p in ipairs(inp.pills) do
    sn[i] = { p, p.health or 0, p.owner, p.mx, p.my, p.in_tank and true or false }
  end
  return sn
end

local function snap_changed(sn)
  for _, r in ipairs(sn) do
    local p = r[1]
    if (p.health or 0) ~= r[2] or p.owner ~= r[3] or p.mx ~= r[4] or p.my ~= r[5]
       or (p.in_tank and true or false) ~= r[6] then
      return true
    end
  end
  return false
end

-- The last run's outcome, for the tests (the log lines are debug only):
-- { parts, work, kills, ticks, giveup = why or nil }, and the last drop's why.
M.last_run, M.last_drop = nil, nil

-- The open run for this plan, or nil (none, or dropped as stale).
function M.split_get(goal, pill, key, now)
  local sp = goal._pt2_split
  if not sp then return nil end
  local why
  if sp.key ~= key then why = "key"
  elseif goal.target_id ~= sp.target_id then why = "target"
  elseif pill.owner ~= sp.owner then why = "owner"
  elseif now - sp.t0 > (C.SHIELD_SCAN_CACHE_TICKS or 25) then why = "age"
  elseif sp.snap and snap_changed(sp.snap) then why = "pill"
  end
  if not why then return sp end
  goal._pt2_split = nil
  goal._pt2_split_drops = (goal._pt2_split_drops or 0) + 1
  M.last_drop = why
  return nil
end

-- Give up v2 for this plan: v1's sscan, as after_scan's evaluator error.
local function giveup(goal, pill, sscan, now, why, sp)
  goal._pt2_split = nil
  goal._pt2_split_drops = nil
  if C.PILLTAKE_V2_EVALUATOR then goal._pt2 = nil end
  M.last_run = { parts = sp and sp.n or 0, work = sp and sp.work or 0, kills = sp and sp.n_kill or 0,
                 ticks = sp and now - sp.t0 or 0, giveup = why }
  return sscan
end

local function may_part(state, est_ms)
  local br = _G.brain
  local tgt = br and br.targetMs or 0
  if tgt <= 0 then return true end
  local t0 = state and state._think_t0_us
  if not t0 then return false end
  local s = state._pt2_dbl
  local rest = (C.PT2_DBL_PART_LEAVE_REST and s and s.rest_ms) or 0
  return (clock_us() - t0) / 1000 + est_ms + rest < tgt
end

-- PT2_V2_SPLIT (P1): a heavy part (one v2 evaluate, group_step) runs in a
-- coroutine over up to PT2_V2_SPLIT_TICKS work ticks: pilltake_v2.slicer
-- yields when the tick's budget of points is used, and the next work tick
-- resumes it. The answer is the same as the part in one call. A budget
-- kill inside a slice kills the coroutine: the part's mark stays, and the
-- next tick puts its inputs back and runs it again from its start
-- (restore), as before. The drop rules (split_get) are the same.
-- Only a job of an evaluator with a slicer (pilltake_v2; the old one has
-- none) is sliced.
local function v2_split_on(g)
  return C.PT2_V2_SPLIT and coroutine ~= nil and g and g.ev and g.ev.slicer and true or false
end

-- Run part sp.n. Returns true when the run is done; (false, nil, true)
-- when a sliced part yielded (it goes on next tick). A budget kill is
-- passed on with sp.in_part still set.
local function run_part(sp, a)
  local k = sp.n
  local ok, r
  sp.in_part = k
  local t0 = clock_us()
  if k >= 1 and (sp.co or v2_split_on(sp.g)) then
    local inp, g = sp.inp, sp.g
    if not sp.co then
      sp.save = { g.i, inp.pen_n, inp.pen_skip_n, inp.noA_n }
      sp.co = coroutine.create(function() return V2.group_step(g) end)
      local sl = g.ev.slicer(inp, C.PT2_V2_SPLIT_TICKS or 5, coroutine.yield)
      sp.slicer = sl
      inp.v2_slice = sl.fn
    end
    ok, r = coroutine.resume(sp.co)
    local us = clock_us() - t0
    sp.us = sp.us + us
    -- the cost estimate is the cost of one tick's slice
    local st = a and a.state
    if ok and st then
      local al = C.CAPACITY_EWMA_ALPHA or 0.2
      st._pt2_split_est_ms = st._pt2_split_est_ms and (st._pt2_split_est_ms * (1 - al) + (us / 1000) * al) or us / 1000
    end
    if ok and coroutine.status(sp.co) ~= "dead" then
      sp.in_part = nil
      return false, nil, true
    end
    sp.co, sp.slicer, inp.v2_slice = nil, nil, nil
    if not ok then
      local msg = tostring(r)
      if msg:find("tick_budget_exceeded", 1, true) then error(r, 0) end
      sp.in_part = nil
      print("[pt2] evaluator error, v1 kept: " .. msg)
      return nil, msg
    end
    sp.in_part = nil
    sp.n = k + 1
    return r
  end
  if k == 0 then
    ok, r = pcall(function()
      local i = M.build_inp(a.state, a.goal, a.info, a.world, a.pill, a.no_builder, a.npb, a.radius, a.now)
      if i and BRAIN_DEBUG_MODE and PV.any_on() then i.keep_all = true end
      if i then sp.g, sp.snap = V2.group_begin(i), snap_pills(i) end
      sp.inp, sp.a = i or false, a
      return not i
    end)
  else
    local inp, g = sp.inp, sp.g
    sp.save = { g.i, inp.pen_n, inp.pen_skip_n, inp.noA_n }
    ok, r = pcall(V2.group_step, g)
  end
  local us = clock_us() - t0
  if not ok then
    local msg = tostring(r)
    if msg:find("tick_budget_exceeded", 1, true) then error(r, 0) end
    sp.in_part = nil
    print("[pt2] evaluator error, v1 kept: " .. msg)
    return nil, msg
  end
  sp.in_part = nil
  sp.n = k + 1
  sp.us = sp.us + us
  local st = a.state
  if st then
    local al = C.CAPACITY_EWMA_ALPHA or 0.2
    st._pt2_split_est_ms = st._pt2_split_est_ms and (st._pt2_split_est_ms * (1 - al) + (us / 1000) * al) or us / 1000
  end
  return r
end

-- A part that started and did not end (a budget kill): put back what the
-- part changed before its evaluate came back.
local function restore(sp)
  local inp, g, sv = sp.inp, sp.g, sp.save
  -- PT2_V2_SPLIT: the killed coroutine is not resumed; the part runs again
  sp.co, sp.slicer = nil, nil
  if inp then inp.v2_slice = nil end
  if not (inp and g and sv) then return end
  g.i, inp.pen_n, inp.pen_skip_n, inp.noA_n = sv[1], sv[2], sv[3], sv[4]
  inp.A = g.goalA
  if inp.set_A then inp.set_A(g.goalA) end
end

-- One tick of the split run. sp: M.split_get's answer. a: after_scan's
-- arguments, a.sscan v1's result. Returns (sscan, true) when the plan is
-- ready, (nil, false) while the run goes on (the take waits in its plan).
function M.split_step(sp, a, key)
  local goal, now = a.goal, a.now
  if not sp then
    if not v2_wanted(goal) then return a.sscan, true end
    local tries = C.SHIELD_SCAN_BLACKLIST_TRIES or 2
    if (goal._pt2_split_drops or 0) >= tries then
      return giveup(goal, a.pill, a.sscan, now, "drops", nil), true
    end
    -- the plan tick: v1 only (with PT2_V2_FIRST, nothing: v1_later, v1's
    -- scan runs only if v2 has no plan of its own)
    goal._pt2_split = { key = key, t0 = now, target_id = goal.target_id, owner = a.pill.owner,
                        sscan = a.sscan, v1_later = a.v1_later or nil, n = 0, ms = {}, us = 0, kills = {}, n_kill = 0, work = 0 }
    return nil, false
  end
  local tries = C.SHIELD_SCAN_BLACKLIST_TRIES or 2
  if sp.in_part then
    local k = sp.in_part
    sp.in_part = nil
    sp.kills[k] = (sp.kills[k] or 0) + 1
    sp.n_kill, sp.last_kill = sp.n_kill + 1, k
    if k ~= "fin" then restore(sp) end
    if sp.kills[k] >= tries then
      return giveup(goal, a.pill, sp.sscan, now, "kills", sp), true
    end
  end
  -- the answer is made: a kill after it (in attack.lua, before the plan is
  -- set) gets the same answer again
  if sp.out then return sp.out, true end
  if not sp.done then
    local maxt = C.PT2_AFTER_SCAN_MAX_TICKS or 5
    local left = maxt - sp.work
    local need = 1
    if left <= 1 then
      need = math.huge
    elseif sp.g and sp.g.list then
      need = math.ceil(V2.group_parts_left(sp.g) / left)
    end
    local st = a.state
    local one = C.PT2_SPLIT_ONE_PART_PER_TICK
    local ran, heavy, done, err = 0, 0, false, nil
    repeat
      if one then
        if heavy >= 1 then break end
      elseif ran >= need and not may_part(st, (st and st._pt2_split_est_ms) or 0) then
        break
      end
      if sp.n >= 1 then heavy = heavy + 1 end
      local yielded
      done, err, yielded = run_part(sp, (sp.n == 0) and a or sp.a)
      if err then return giveup(goal, a.pill, sp.sscan, now, "error", sp), true end
      ran = ran + 1
      -- PT2_V2_SPLIT: a sliced part used this tick's budget
      if yielded then break end
    until done
    sp.work = sp.work + 1
    if not done then return nil, false end
    sp.done = true
  end
  -- finish is marked as a part too ("fin"): killed TRIES times, v1 stays
  sp.in_part = "fin"
  local out = finish(sp.a, sp.inp or nil, sp.g and sp.g.res, sp.us)
  sp.in_part = nil
  sp.out = out
  M.last_run = { parts = sp.n, work = sp.work, kills = sp.n_kill, ticks = now - sp.t0 }
  goal._pt2_split_drops = nil
  return out, true
end

-- attack.lua: the plan is set (goal._shield_scan), the run is over.
function M.split_done(goal)
  goal._pt2_split = nil
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
