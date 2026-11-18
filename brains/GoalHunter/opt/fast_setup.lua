-- =========================================================================
-- fast_setup.lua -- pill take v2 fast setup (C.PT2_FAST_SETUP, Andrew's
-- spec, Nov 18)
--
-- The old PPT take stops on the standoff, then turns to a pre-aim point,
-- to the aim corner, and taps one brad at a time until the shell sim hits
-- the pill. On Oil Rig that took 128 ticks in range before the first shot,
-- and the blocker pill died before the second twin.
--
-- Fast setup does the aiming before the tank is in range:
--
--   1. Predict the gun angle the finetune ends on at the standoff F: start
--      at the aim corner's bearing and step toward the pill centre in
--      0.125 brad steps (one /8 engine turn step, the finetune's tap), until
--      the shot test passes. Keep stepping while it passes; the angle is the
--      middle of that clear run. AP is the point on that ray at the pill.
--   2. D = dir16 of the angle. Every point S = F - t*u(D) behind F has a
--      bearing to AP that also rounds to D, so a tank that faces AP from S
--      moves along D straight through F (attack.lua blitz_setup_16dir has
--      the same geometry; its edge margin C.BLITZ_SETUP_16DIR_EDGE_BRAD).
--   3. t: the setup point is just outside the range of every hostile pill
--      whose range (pill range + C.PT2_RANGE_MARGIN_WU, the v2 rule) reaches
--      the run line. No such pill: C.ATTACK_APPROACH_OFFSET.
--   4. Qualify: the shot test passes at F facing AP, and at no fewer than
--      C.PT2_LEEWAY_MIN of the 8 v2 leeway points (32 wu around F) facing
--      AP. Else the old path, unchanged.
--
-- The approach (attack.lua, first approach tick): when the plan qualifies
-- then, the approach drives to S instead of the old approach point.
--
-- The run (substate in_range_position, goal._fs; planned again here):
--   align  join from where the tank stands when it is outside every hostile
--          pill's range (SH.PILL_RANGE, no v2 margin: the margin absorbs
--          the approach's 64 wu arrival slop) and far enough behind F to
--          move back onto the line before F. Else the old in_range_position
--          creep to S (12 wu, speed 4).
--   turn   brake and turn to the steer heading (1 brad, its dir16 sector).
--   run    full speed; each tick brake when cpf.predict_stop from the live
--          state reaches F along D, else speed up. Steer heading: the
--          bearing to AP when the tank is within 12 wu of the line; else
--          the edge of the next sector toward the line, so the tank moves
--          back onto it (hold > 3, tap > TAP while no shot passes).
--   stop   braked, speed <= 4 and F reached (or passed): the shot test
--          passes from here: shoot_pill (as the finetune's success); else
--          the old in_range_position path.
--
-- Fire: on any turn / run tick when the shot test passes from the live
-- position and facing, and from the points 1..4 engine ticks ahead along
-- the moving direction (the brain thinks every other engine tick and its
-- keys land one brain tick late, so the shell leaves up to 4 engine ticks
-- of travel later). No turn key on this tick or the one before, so the gun
-- angle at launch is the one tested. The shot test: the engine shell sim
-- crosses the pill square, and attack.lua's shot_path_obstacle_count finds
-- no live pill (math.huge), building, half building or forest before it.
-- Our blocker pill on the line fails the test, so we never hit it.
-- =========================================================================

local bit    = require('bitcompat')
local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local print2 = require("print2")

local M = {}

local floor, sqrt, abs, sin, cos, ceil = math.floor, math.sqrt, math.abs, math.sin, math.cos, math.ceil

-- One /8 engine turn step in brads: the finetune's smallest tap.
M.TAP = 0.125
-- Lag window in engine ticks (see the header).
M.LAG_TICKS = 4
-- The in_range_position tolerances (attack.lua DIST_TOL / SPEED_TOL).
local DIST_TOL, SPEED_TOL = 12, 4
-- attack.lua IN_RANGE_GIVE_UP_TICKS: no setup run lasts longer.
local GIVE_UP_TICKS = 500

local function dir16(a)
  a = a % 256
  if a <= 8 or a > 248 then return 0 end
  return ceil((a - 8) / 16) * 16
end
M.dir16 = dir16

local function unit(a)
  local r = a * C.TWO_PI / 256
  return sin(r), -cos(r)            -- engine: 0 = north, 64 = east
end
M.unit = unit

local function bearing(x, y, tx, ty)
  return U.aim_at_f(x, y, tx, ty) % 256
end

-- ── The shot test ─────────────────────────────────────────────────────────
-- env: { pmx, pmy, sim(x, y, ang) -> square list, obs(x, y, ang) -> shots,
-- why, reached }. x, y are whole world units. Returns ok, why.
function M.shot_ok(env, x, y, ang)
  x, y = floor(x + 0.5), floor(y + 0.5)
  local path = env.sim(x, y, ang)
  local on_pill = false
  if path then
    for _, t in ipairs(path) do
      if t.mx == env.pmx and t.my == env.pmy then on_pill = true; break end
    end
  end
  if not on_pill then return false, "miss" end
  local shots, why, reached = env.obs(x, y, ang)
  if shots == math.huge then return false, why or "pill" end
  if (shots or 0) > 0 then return false, "cover" end
  if reached == false then return false, "miss" end
  return true
end

-- Step 1: the angle the finetune ends on at (fx, fy). a0: the aim corner's
-- bearing; ac: the pill centre's bearing. Returns ang, lo, hi or nil.
function M.final_angle(env, fx, fy, a0, ac)
  local d = U.adiff(a0, ac)
  local sgn = d >= 0 and 1 or -1
  local n = ceil(abs(d) / M.TAP)
  local lo
  for k = 0, n do
    local a = (a0 + sgn * k * M.TAP) % 256
    if M.shot_ok(env, fx, fy, a) then lo = k; break end
  end
  if not lo then return nil end
  -- keep stepping while the shot passes (the lane can go past the centre;
  -- 256 steps = 32 brads stops a runaway)
  local hi = lo
  while hi - lo < 256 and M.shot_ok(env, fx, fy, (a0 + sgn * (hi + 1) * M.TAP) % 256) do
    hi = hi + 1
  end
  local a_lo = (a0 + sgn * lo * M.TAP) % 256
  local a_hi = (a0 + sgn * hi * M.TAP) % 256
  return (a0 + sgn * (lo + hi) * 0.5 * M.TAP) % 256, a_lo, a_hi
end

-- Step 3: how far behind F (wu) the setup must be. pills: { {x, y}, ... }
-- centres (wu); R: range (wu). A pill counts when its range reaches the run
-- line F - t*u for t in [0, t_now]; t grows to its far exit. Returns t.
function M.setup_t(fx, fy, ux, uy, pills, R)
  local t = 0
  local R2 = R * R
  local changed = true
  local used = {}
  while changed do
    changed = false
    for i, p in ipairs(pills) do
      if not used[i] then
        -- |F - t*u - P|^2 = R^2: t^2 - 2 t (w.u) + |w|^2 - R^2 = 0, w = F - P
        local wx, wy = fx - p[1], fy - p[2]
        local wu = wx * ux + wy * uy
        local c = wx * wx + wy * wy - R2
        local disc = wu * wu - c
        if disc >= 0 then
          local r = sqrt(disc)
          local t_in, t_out = wu - r, wu + r
          -- the circle meets the part of the line already in the run
          if t_out > 0 and t_in <= t then
            used[i] = true
            if t_out > t then t = t_out; changed = true end
          end
        end
      end
    end
  end
  return t
end

-- The 8 leeway neighbours (pilltake_v2.lua NB): unit directions.
local NB = {}
for k = 0, 7 do
  local a = k * math.pi / 4
  NB[k + 1] = { cos(a), sin(a) }
end

-- The plan. inp:
--   fx, fy        standoff F (wu)
--   pcx, pcy      target pill centre (wu)
--   a0            bearing F -> the aim corner
--   pills         hostile pill centres { {x, y}, ... } (wu)
--   R             pill range for the setup (wu)
--   edge          dir16 sector edge margin (brads)
--   min_t         run length when no pill reaches the line (wu)
--   ring_wu       leeway ring radius; lee_min: leeway points needed
--   standable(mx, my), drive_ok(sx, sy, fx, fy) (wu)
-- Returns plan or nil, why, rec.
function M.plan(env, inp)
  local rec = {}
  local fx, fy = inp.fx, inp.fy
  local ac = bearing(fx, fy, inp.pcx, inp.pcy)
  local ang, lo, hi = M.final_angle(env, fx, fy, inp.a0, ac)
  rec.a0, rec.ac = inp.a0, ac
  if not ang then return nil, "no_lane", rec end
  rec.ang, rec.lo, rec.hi = ang, lo, hi
  local dpc = sqrt((inp.pcx - fx) ^ 2 + (inp.pcy - fy) ^ 2)
  local aux, auy = unit(ang)
  local apx, apy = fx + aux * dpc, fy + auy * dpc
  rec.apx, rec.apy = apx, apy
  local D = dir16(ang)
  rec.D = D
  if 8 - abs(U.adiff(D, ang)) < inp.edge then return nil, "near_boundary", rec end
  local ux, uy = unit(D)
  local t = M.setup_t(fx, fy, ux, uy, inp.pills, inp.R)
  if t <= 0 then t = inp.min_t end
  rec.t = t
  local sx, sy = fx - t * ux, fy - t * uy
  rec.sx, rec.sy = sx, sy
  local sb = bearing(sx, sy, apx, apy)
  if dir16(sb) ~= D or 8 - abs(U.adiff(D, sb)) < inp.edge then
    return nil, "setup_dir_mismatch", rec
  end
  if not inp.standable(floor(sx / 256), floor(sy / 256)) then return nil, "setup_square", rec end
  if not inp.drive_ok(sx, sy, fx, fy) then return nil, "drive_blocked", rec end
  -- Qualify: the shot from F, then the leeway ring.
  local ok, why = M.shot_ok(env, fx, fy, ang)
  if not ok then return nil, "shot_at_F:" .. tostring(why), rec end
  local lee = 0
  for k = 1, 8 do
    local qx, qy = fx + NB[k][1] * inp.ring_wu, fy + NB[k][2] * inp.ring_wu
    if M.shot_ok(env, qx, qy, bearing(qx, qy, apx, apy)) then lee = lee + 1 end
  end
  rec.lee = lee
  if lee < inp.lee_min then return nil, "leeway", rec end
  return {
    fx = fx, fy = fy, ang = ang, apx = apx, apy = apy, D = D, ux = ux, uy = uy,
    sx = sx, sy = sy, t = t, lee = lee, phase = "align",
  }, nil, rec
end

-- ── Adapters to the bot's world ───────────────────────────────────────────
-- The shot-test env for goal's pill. obs_fn: attack.lua's
-- shot_path_obstacle_count. gunrange: the sight length the shot uses.
function M.world_env(info, goal, world, obs_fn, gunrange)
  local probe = setmetatable({}, { __index = info })
  return {
    pmx = goal.mx, pmy = goal.my,
    sim = function(x, y, ang)
      return cpf.simulate_shot_angle(x, y, ang, cpf.SHOT_TANK, gunrange)
    end,
    obs = function(x, y, ang)
      probe.tankx, probe.tanky, probe.tank_angle = x, y, ang
      return obs_fn(probe, goal, world, nil, nil, nil, nil, true)
    end,
  }
end

local function hostile_pills(world)
  local PC = require("pill_cover")
  local out = {}
  for _, p in pairs(world.pills or {}) do
    if p and (p.health or 0) > 0 and not p.in_tank and not p.pos_unknown and p.mx
       and not PC.is_friendly_owner(p.owner) then
      out[#out + 1] = { p.mx * 256 + 128, p.my * 256 + 128 }
    end
  end
  return out
end

-- The same checks the blitz setup line gets (attack.lua blitz_setup_16dir):
-- no building, half building, pillbox or hostile base on the drive line.
local function drive_ok_fn(world)
  return function(sx, sy, fx, fy)
    local smx, smy = floor(fx / 256), floor(fy / 256)
    local blocked = false
    U.line_walk(sx / 256, sy / 256, fx / 256, fy / 256, function(lx, ly)
      if lx == smx and ly == smy then return end
      local wt = U.ttype(lx, ly)
      if wt == C.T_BUILDING or wt == C.T_HALFBUILD or wt == C.T_PILLBOX then
        blocked = true; return true
      end
      local be = world and world.base_at and world.base_at[ly * 256 + lx]
      if be and be.base and be.base.owner == "hostile" then
        blocked = true; return true
      end
    end)
    return not blocked
  end
end

local function standable_fn(world)
  local PC = require("pill_cover")
  return function(mx, my)
    if not U.in_map(mx, my) then return false end
    local tt = U.ttype(mx, my)
    if U.is_water(tt) or ((C.TERRAIN_COST_LAND or {})[tt] or 9999) >= 9999 then return false end
    local p = PC.pill_on_square(world, mx, my)
    if p and (p.health or 0) > 0 then return false end
    return true
  end
end

-- The plan for goal's standoff from the bot's world. stage: "approach" or
-- "in_range" (the log only). Returns plan or nil.
local function plan_from_world(stage, goal, info, world, now, obs_fn)
  if not (goal._pt2 and goal._is_ppt and goal.standoff_fx and goal.mx) then return nil end
  local fx = floor(goal.standoff_fx * 256 + 0.5)
  local fy = floor(goal.standoff_fy * 256 + 0.5)
  local axw = goal.aim_wx or floor((goal.aim_mx or (goal.mx + 0.5)) * 256 + 0.5)
  local ayw = goal.aim_wy or floor((goal.aim_my or (goal.my + 0.5)) * 256 + 0.5)
  local env = M.world_env(info, goal, world, obs_fn, C.GUNSIGHT_MAX)
  local V2 = require("pilltake_v2")
  local SH = require("shadow")
  local inp = {
    fx = fx, fy = fy,
    pcx = goal.mx * 256 + 128, pcy = goal.my * 256 + 128,
    a0 = bearing(fx, fy, axw, ayw),
    pills = hostile_pills(world),
    R = SH.PILL_RANGE + (C.PT2_RANGE_MARGIN_WU or 128),
    edge = C.BLITZ_SETUP_16DIR_EDGE_BRAD or 0.5,
    min_t = (C.ATTACK_APPROACH_OFFSET or 2.25) * 256,
    ring_wu = V2.knob_inp(C, {}).leeway_ring_wu,
    lee_min = C.PT2_LEEWAY_MIN or 6,
    standable = standable_fn(world),
    drive_ok = drive_ok_fn(world),
  }
  local plan, why, rec = M.plan(env, inp)
  if plan then
    plan.pills = inp.pills
    plan.range = SH.PILL_RANGE
  end
  return plan
end

-- Called on the first tick of a v2 take's approach (attack.lua). When the
-- plan qualifies, the approach drives to S instead of the old approach
-- point, so the tank comes in on the run line and faces along it: no
-- turn-around at the standoff. The plan itself is made again at the
-- aim -> in_range_position step (plan_for_goal), from the state then.
function M.approach_for_goal(state, goal, info, world, now, obs_fn)
  if (goal._pt2 and goal._pt2.finish) or not goal._shield_scan then return end
  local plan = plan_from_world("approach", goal, info, world, now, obs_fn)
  if not plan then return end
  goal.approach_fx = plan.sx / 256
  goal.approach_fy = plan.sy / 256
  goal.approach_mx = U.mclamp(floor(goal.approach_fx))
  goal.approach_my = U.mclamp(floor(goal.approach_fy))
end

-- Called at the aim -> in_range_position step of a v2 take (attack.lua).
-- Returns the plan for goal._fs, or nil (the old path).
function M.plan_for_goal(state, goal, info, world, now, obs_fn)
  local plan = plan_from_world("in_range", goal, info, world, now, obs_fn)
  if not plan then return nil end
  plan.t0 = now
  goal._in_range_dist_tol = DIST_TOL
  return plan
end

-- The shot test from the live state: the tank now and 1..LAG_TICKS engine
-- ticks ahead along its moving direction.
local function live_fire_ok(info, goal, world, obs_fn)
  local env = M.world_env(info, goal, world, obs_fn, info.gunrange or 14)
  local ang = info.tank_angle or info.direction
  local ok, why = M.shot_ok(env, info.tankx, info.tanky, ang)
  if not ok then return false, why, 0 end
  local espeed = (info.speed or 0) / 4      -- info.speed is engine speed x4
  if espeed > 0 then
    local mx, my = unit(dir16(ang))
    for k = 1, M.LAG_TICKS do
      local ok2, why2 = M.shot_ok(env, info.tankx + mx * espeed * k, info.tanky + my * espeed * k, ang)
      if not ok2 then return false, why2, k end
    end
  end
  return true, nil, 0
end

M.live_fire_ok = live_fire_ok

local function to_shoot_pill(goal, info, now)
  local angle_f = info.tank_angle or info.direction
  local far_t = 16.0
  local rad = angle_f * (3.14159265358979323846 / 128.0)
  goal.aim_mx = info.tankx / 256.0 + math.sin(rad) * far_t
  goal.aim_my = info.tanky / 256.0 - math.cos(rad) * far_t
  goal.substate          = "shoot_pill"
  goal._shoot_armour     = info.armour
  goal._shoot_shells     = info.shells
  goal._shoot_hits_total = 0
  goal._shoot_start_tick = now
  goal._shoot_reach_checked = nil
  goal._shoot_first_steer = true
end

-- Side offset (wu) of the tank from the run line through F along D.
-- Positive: to the left of the moving direction.
local function side_off(fs, x, y)
  return (x - fs.fx) * fs.uy - (y - fs.fy) * fs.ux
end

-- The heading the tank steers to. On the line: the bearing to AP. Off it
-- (side offset > DIST_TOL): the edge of the next dir16 sector toward the
-- line (C.BLITZ_SETUP_16DIR_EDGE_BRAD inside it), so the tank moves back
-- onto the line while it runs. The move back holds (fs.fix_sign) until
-- the tank reaches the line, so it does not wobble at the tolerance.
-- Returns heading, its dir16, side offset.
local function steer_heading(fs, info)
  local off = side_off(fs, info.tankx, info.tanky)
  if fs.fix_sign and off * fs.fix_sign <= 0 then fs.fix_sign = nil end
  if not fs.fix_sign and abs(off) > DIST_TOL then fs.fix_sign = off > 0 and 1 or -1 end
  if not fs.fix_sign then
    local h = bearing(info.tankx, info.tanky, fs.apx, fs.apy)
    return h, fs.D, off
  end
  local edge = C.BLITZ_SETUP_16DIR_EDGE_BRAD or 0.5
  for _, s in ipairs({ 16, -16 }) do
    local vx, vy = unit(fs.D + s)
    if (vx * fs.uy - vy * fs.ux) * off < 0 then
      local h = (fs.D + (s > 0 and (8 + edge) or -(8 + edge))) % 256
      return h, dir16(h), off
    end
  end
  return bearing(info.tankx, info.tanky, fs.apx, fs.apy), fs.D, off
end
M.side_off, M.steer_heading = side_off, steer_heading

-- Join the run from where the tank stands (no drive back to S): the tank is
-- outside every hostile pill's range (SH.PILL_RANGE; the plan's extra
-- C.PT2_RANGE_MARGIN_WU is for the approach's slop) and far enough behind F
-- to move back onto the line before F (one sector off D: 1 / tan(22.5 deg)
-- wu of run per wu of side offset).
function M.can_join(fs, x, y)
  local along = (fs.fx - x) * fs.ux + (fs.fy - y) * fs.uy
  if along <= 0 then return false, along end
  local R2 = (fs.range or 0) ^ 2
  for _, p in ipairs(fs.pills or {}) do
    if (x - p[1]) ^ 2 + (y - p[2]) ^ 2 <= R2 then return false, along end
  end
  local fix = abs(side_off(fs, x, y)) / math.tan(math.pi / 8)
  return fix <= along, along
end

-- attack.lua's in_range_position with goal._fs set. Returns true when it
-- handled the tick; false: goal._fs is gone, the old code runs this tick.
function M.attack_tick(state, goal, info, world, now, obs_fn)
  local fs = goal._fs
  local function drop(why)
    goal._fs = nil
    return false
  end
  if now - (fs.t0 or now) > GIVE_UP_TICKS then return drop("timeout") end
  if fs.phase == "align" then
    local d = U.wdist(info.tankx, info.tanky, fs.sx, fs.sy)
    local join, along = M.can_join(fs, info.tankx, info.tanky)
    if join and (info.speed or 0) <= SPEED_TOL then
      fs.phase = "turn"
    elseif d <= DIST_TOL and (info.speed or 0) <= SPEED_TOL then
      fs.phase = "turn"
    end
    return true
  end
  local ok, why, k = live_fire_ok(info, goal, world, obs_fn)
  fs.fire_ok = ok
  local ang = info.tank_angle or info.direction
  local brg, hsec, off = steer_heading(fs, info)
  local corr = U.adiff(ang, brg)
  -- distance left to F along D (negative: past F)
  local rem = (fs.fx - info.tankx) * fs.ux + (fs.fy - info.tanky) * fs.uy
  if fs.phase == "turn" then
    if abs(corr) <= 1 and dir16(ang) == hsec and (info.speed or 0) <= SPEED_TOL then
      fs.phase = "run"
      fs.run_t0 = now
    end
    return true
  end
  -- run
  -- stopped (attack.lua's speed <= 4) on F, or past it (no reverse)
  if fs.braked and (info.speed or 0) <= SPEED_TOL and rem <= DIST_TOL then
    local err = U.wdist(info.tankx, info.tanky, floor(fs.fx + 0.5), floor(fs.fy + 0.5))
    local env = M.world_env(info, goal, world, obs_fn, info.gunrange or 14)
    local here_ok, here_why = M.shot_ok(env, info.tankx, info.tanky, ang)
    goal._fs = nil
    if here_ok then
      to_shoot_pill(goal, info, now)
      return true
    end
    return false
  end
  return true
end

-- steering.lua's in_range_position with goal._fs set, phases turn and run
-- (align uses the old creep, aimed at S). Returns keys, taps, fire or nil.
function M.steer(state, world, info, goal)
  local fs = goal._fs
  if not fs or fs.phase == "align" then return nil end
  local keys, taps = 0, 0
  if info.gunrange < C.GUNSIGHT_MAX then keys = bit.bor(keys, KEY_MORERANGE) end
  local ang = info.tank_angle or info.direction
  local brg, hsec = steer_heading(fs, info)
  local corr = U.adiff(ang, brg)
  -- Fire: the test passed this tick and no turn key last tick.
  local fire = false
  if fs.fire_ok and not fs.turned_prev and info.shells > C.SHELL_RESERVE
     and not U.fire_hold_block(state, C.FIRE_HOLD_TICKS) then
    local p = goal.target_id and world.pills and world.pills[goal.target_id]
    local hp = p and (p.health or 0) or 0
    if (goal._on_target_in_flight or 0) < hp then fire = true end
  end
  local turned = false
  if fs.phase == "turn" then
    if (info.speed or 0) > 0 then keys = bit.bor(keys, KEY_SLOWER) end
    -- a passing shot test holds the gun still, so the next tick can fire
    if not fs.fire_ok then
      if     corr >  6 then keys = bit.bor(keys, KEY_TURNRIGHT); turned = true
      elseif corr < -6 then keys = bit.bor(keys, KEY_TURNLEFT); turned = true
      elseif corr >  1 then taps = bit.bor(taps, KEY_TURNRIGHT); turned = true
      elseif corr < -1 then taps = bit.bor(taps, KEY_TURNLEFT); turned = true
      -- the facing must round to the sector as well (an edge): one more tap
      elseif dir16(ang) ~= hsec then
        taps = bit.bor(taps, corr >= 0 and KEY_TURNRIGHT or KEY_TURNLEFT); turned = true
      end
    end
  else
    local rem = (fs.fx - info.tankx) * fs.ux + (fs.fy - info.tanky) * fs.uy
    -- Brake from the first tick the stop predicted from the live state
    -- reaches F. Stopped short of F (speed <= 4): drive on.
    local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
    local tcap = (C.TERRAIN_SPEED and C.TERRAIN_SPEED[U.ttype(tmx, tmy)]) or 16
    local espeed = (info.speed or 0) / 4
    local psx, psy = cpf.predict_stop(info.tankx, info.tanky, ang, espeed, tcap)
    -- the stop's distance along D (the tank may be one sector off D
    -- while it moves back onto the line)
    local stop_dist = (psx - info.tankx) * fs.ux + (psy - info.tanky) * fs.uy
    if stop_dist >= rem then fs.braking = true; fs.braked = true
    elseif fs.braking and (info.speed or 0) <= SPEED_TOL and rem > DIST_TOL then
      fs.braking = false
    end
    keys = bit.bor(keys, fs.braking and KEY_SLOWER or KEY_FASTER)
    -- no shot yet: hold > 3, else tap (one TAP each) toward AP, as fine
    -- as the finetune, so a narrow lane is met before the stop
    if not fs.fire_ok then
      if     corr >  3 then keys = bit.bor(keys, KEY_TURNRIGHT); turned = true
      elseif corr < -3 then keys = bit.bor(keys, KEY_TURNLEFT); turned = true
      elseif corr >  M.TAP then taps = bit.bor(taps, KEY_TURNRIGHT); turned = true
      elseif corr < -M.TAP then taps = bit.bor(taps, KEY_TURNLEFT); turned = true
      elseif dir16(ang) ~= hsec then
        taps = bit.bor(taps, corr >= 0 and KEY_TURNRIGHT or KEY_TURNLEFT); turned = true
      end
    end
  end
  fs.turned_prev = turned
  if fire then
    local now = state.tick or 0
    if not goal._attack_start_tick then
      -- shoot_pill's shot accounting, started at the first shot
      local p = goal.target_id and world.pills and world.pills[goal.target_id]
      local hp = p and (p.health or 0) or 0
      goal._attack_start_tick = now
      goal._charge_start_hp   = hp
      goal._bullets_needed    = math.min(10, hp)
      goal._kill_attempt      = true
    end
    fs.shots = (fs.shots or 0) + 1
    local rem = (fs.fx - info.tankx) * fs.ux + (fs.fy - info.tanky) * fs.uy
  end
  return keys, taps, fire
end

return M
