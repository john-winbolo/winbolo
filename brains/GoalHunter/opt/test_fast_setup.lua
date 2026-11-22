-- =========================================================================
-- test_fast_setup.lua -- standalone unit tests (2026-11-18) for
-- fast_setup.lua (C.PT2_FAST_SETUP, Andrew's spec, Nov 18):
--   * the knob is on by default and off in PRESETS.keel and PRESETS.mast
--   * dir16 and the unit vector match the engine's 16 directions
--   * setup_t: the setup point leaves the range of every pill that reaches
--     the run line, chained pills too; a pill off the line does not count
--   * shot_ok: a miss, a pill (our blocker) and cover on the line all fail
--   * final_angle: the middle of the clear run, from the aim corner side
--   * plan: back-projection onto the dir16 line, the facing from S rounds
--     to D, the qualify test at F and the leeway ring, and each fallback
--   * live fire test: the points 1..4 engine ticks ahead must pass too
-- The shell sim here is a straight line on the square grid, not the
-- engine's; the tests check the logic, not the engine geometry.
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_fast_setup.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local C  = require("constants")
local U  = require("util")
local FS = require("fast_setup")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end
local function near(a, b, tol) return math.abs(a - b) <= (tol or 1e-6) end

-- ── Knob ──────────────────────────────────────────────────────────────────
print("knob")
check("on by default", C.PT2_FAST_SETUP == true, C.PT2_FAST_SETUP)
check("keel off", C.PRESETS.keel.PT2_FAST_SETUP == false, C.PRESETS.keel.PT2_FAST_SETUP)
check("mast off", C.PRESETS.mast.PT2_FAST_SETUP == false, C.PRESETS.mast.PT2_FAST_SETUP)

-- ── dir16 / unit ──────────────────────────────────────────────────────────
print("dir16")
check("8 -> 0", FS.dir16(8) == 0)
check("8.01 -> 16", FS.dir16(8.01) == 16)
check("173 -> 176", FS.dir16(173) == 176)
check("249 -> 0", FS.dir16(249) == 0)
local ux, uy = FS.unit(64)
check("64 is east", near(ux, 1) and near(uy, 0, 1e-9))
ux, uy = FS.unit(0)
check("0 is north", near(ux, 0, 1e-9) and near(uy, -1))

-- ── setup_t ───────────────────────────────────────────────────────────────
print("setup_t")
do
  -- F at the origin, run line toward the east (u = north... use u = east,
  -- so S = F - t*u lies to the west).
  local fx, fy, ux1, uy1 = 0, 0, 1, 0
  local R = 1000
  -- a pill 600 wu east of F: |F - t u - P| = R at t = 400 (west exit)
  local t = FS.setup_t(fx, fy, ux1, uy1, { { 600, 0 } }, R)
  check("one pill: exit at 400", near(t, 400, 1e-6), t)
  -- a second pill whose range starts on the run line at t = 300 and
  -- ends at t = 1300: it reaches the line, so S moves to 1300
  t = FS.setup_t(fx, fy, ux1, uy1, { { 600, 0 }, { -800, 0 } }, R)
  check("chained pill: exit at 1800", near(t, 1800, 1e-6), t)
  -- a pill whose range meets the line only beyond the run does not count
  t = FS.setup_t(fx, fy, ux1, uy1, { { 600, 0 }, { -2000, 0 } }, R)
  check("pill past the run: no effect", near(t, 400, 1e-6), t)
  -- a pill that never reaches the line
  t = FS.setup_t(fx, fy, ux1, uy1, { { 0, 5000 } }, R)
  check("no pill on the line: 0", t == 0, t)
end

-- ── A grid shot sim ──────────────────────────────────────────────────────
-- Squares the ray from (x, y) at angle ang crosses, sampled every 8 wu for
-- len wu. blockers: { [sq] = "pill" | "wall" }.
local function sq(mx, my) return my * 256 + mx end
local function make_env(pmx, pmy, blockers, len)
  local function walk(x, y, ang)
    local dx, dy = FS.unit(ang)
    local out, last = {}, -1
    for s = 0, len, 8 do
      local mx, my = math.floor((x + dx * s) / 256), math.floor((y + dy * s) / 256)
      local k = sq(mx, my)
      if k ~= last then last = k; out[#out + 1] = { mx = mx, my = my } end
    end
    return out
  end
  return {
    pmx = pmx, pmy = pmy,
    sim = walk,
    obs = function(x, y, ang)
      local shots = 0
      for _, t in ipairs(walk(x, y, ang)) do
        if t.mx == pmx and t.my == pmy then return shots, nil, true end
        local b = blockers[sq(t.mx, t.my)]
        if b == "pill" then return math.huge, "pill" end
        if b == "wall" then shots = shots + 5 end
      end
      return shots, nil, false
    end,
  }
end

-- ── shot_ok ───────────────────────────────────────────────────────────────
print("shot_ok")
do
  -- pill at square (10,10); tank at (15,10) square centre, west = 192
  local env = make_env(10, 10, {}, 2048)
  local x, y = 15 * 256 + 128, 10 * 256 + 128
  check("clear shot", FS.shot_ok(env, x, y, 192) == true)
  local ok, why = FS.shot_ok(env, x, y, 0)
  check("miss", not ok and why == "miss", why)
  env = make_env(10, 10, { [sq(12, 10)] = "pill" }, 2048)
  ok, why = FS.shot_ok(env, x, y, 192)
  check("our blocker pill on the line fails", not ok and why == "pill", why)
  env = make_env(10, 10, { [sq(12, 10)] = "wall" }, 2048)
  ok, why = FS.shot_ok(env, x, y, 192)
  check("a wall on the line fails", not ok and why == "cover", why)
  env = make_env(10, 10, {}, 3 * 256)
  ok, why = FS.shot_ok(env, x, y, 192)
  check("out of range fails", not ok and why == "miss", why)
end

-- ── final_angle ───────────────────────────────────────────────────────────
print("final_angle")
do
  -- pill (10,10), tank at (15,10) centre. A blocker pill at (12,11) under
  -- the line. From the corner bearing a0 (south of the line, aimed at the
  -- blocker side) the sweep walks toward the centre bearing 192.
  local env = make_env(10, 10, { [sq(12, 11)] = "pill" }, 2048)
  local x, y = 15 * 256 + 128, 10 * 256 + 128
  local a0 = 192 - 6                  -- 6 brads south of west
  local ang, lo, hi = FS.final_angle(env, x, y, a0, 192)
  check("a lane is found", ang ~= nil, ang)
  check("lane starts clear", ang and FS.shot_ok(env, x, y, lo))
  check("lane ends clear", ang and FS.shot_ok(env, x, y, hi))
  check("angle is the middle", ang and near(ang, (lo + hi) / 2, 1e-9), ang)
  check("the step before the lane fails", ang and not FS.shot_ok(env, x, y, lo - FS.TAP))
  check("the step after the lane fails", ang and not FS.shot_ok(env, x, y, hi + FS.TAP))
  -- everything blocked: no angle
  env = make_env(10, 10, { [sq(14, 10)] = "pill", [sq(14, 11)] = "pill", [sq(14, 9)] = "pill" }, 2048)
  check("no lane: nil", FS.final_angle(env, x, y, a0, 192) == nil)
end

-- ── plan ──────────────────────────────────────────────────────────────────
print("plan")
local function base_inp(over)
  local inp = {
    fx = 16 * 256 + 128, fy = 12 * 256 + 64,
    pcx = 10 * 256 + 128, pcy = 10 * 256 + 128,
    pills = { { 10 * 256 + 128, 10 * 256 + 128 } },
    R = 2048 + 128, edge = 0.5, min_t = 2.25 * 256,
    ring_wu = 32, lee_min = 6,
    standable = function() return true end,
    drive_ok = function() return true end,
  }
  inp.a0 = U.aim_at_f(inp.fx, inp.fy, inp.pcx, inp.pcy) % 256
  for k, v in pairs(over or {}) do inp[k] = v end
  return inp
end
do
  local env = make_env(10, 10, {}, 2048)
  local inp = base_inp()
  local p, why, rec = FS.plan(env, inp)
  check("qualifies on an open field", p ~= nil, why)
  if p then
    check("D is dir16 of the angle", p.D == FS.dir16(p.ang), p.D)
    -- S lies on the line F - t*u(D)
    local ux2, uy2 = FS.unit(p.D)
    check("S on the dir16 line", near(p.sx, p.fx - p.t * ux2, 1e-6) and near(p.sy, p.fy - p.t * uy2, 1e-6))
    -- S is outside the pill's range (plus margin)
    local d = math.sqrt((p.sx - inp.pcx) ^ 2 + (p.sy - inp.pcy) ^ 2)
    check("S just outside the range", d >= inp.R - 1e-6 and d <= inp.R + 1, d)
    -- the facing from S toward AP rounds to D, and so from every point
    -- on the run
    local all = true
    for f = 0, 1, 0.125 do
      local px, py = p.sx + (p.fx - p.sx) * f, p.sy + (p.fy - p.sy) * f
      if FS.dir16(U.aim_at_f(px, py, p.apx, p.apy) % 256) ~= p.D then all = false end
    end
    check("facing AP rounds to D along the run", all)
    check("leeway counted", p.lee == 8, p.lee)
    check("phase starts at align", p.phase == "align")
    -- AP is on the ray from F at the final angle
    check("AP on the final ray", near(U.aim_at_f(p.fx, p.fy, p.apx, p.apy) % 256, p.ang, 1e-6))
  end
  -- the shot from F is blocked by our pill: the old path
  env = make_env(10, 10, { [sq(13, 11)] = "pill", [sq(13, 10)] = "pill", [sq(14, 11)] = "pill" }, 2048)
  p, why = FS.plan(env, base_inp())
  check("no lane: old path", p == nil and why == "no_lane", why)
  -- leeway: need more than the ring can give
  env = make_env(10, 10, {}, 2048)
  p, why, rec = FS.plan(env, base_inp({ lee_min = 9 }))
  check("too little leeway: old path", p == nil and why == "leeway", why)
  check("leeway logged", rec.lee == 8, rec.lee)
  -- the setup square is not standable
  p, why = FS.plan(env, base_inp({ standable = function() return false end }))
  check("setup square: old path", p == nil and why == "setup_square", why)
  p, why = FS.plan(env, base_inp({ drive_ok = function() return false end }))
  check("drive line blocked: old path", p == nil and why == "drive_blocked", why)
  -- no hostile pill on the line: the approach offset
  p = FS.plan(env, base_inp({ pills = {} }))
  check("no pill: min_t", p and near(p.t, 2.25 * 256), p and p.t)
  -- a final angle near a sector edge falls back: walk F round the pill in
  -- quarter brads until the plan's angle lands inside the edge margin
  local found, edge_ok = false, true
  for q = 0, 16 * 4 * 4 do
    local a = 176 + q * 0.25
    local inp = base_inp()
    local ux3, uy3 = FS.unit(a)
    inp.fx, inp.fy = inp.pcx - ux3 * 1500, inp.pcy - uy3 * 1500
    inp.a0 = a
    local pp, w, r = FS.plan(env, inp)
    if r.ang and r.D then
      local near_edge = 8 - math.abs(U.adiff(r.D, r.ang)) < inp.edge
      if near_edge then
        found = true
        if not (pp == nil and w == "near_boundary") then edge_ok = false end
      elseif w == "near_boundary" then
        edge_ok = false
      end
    end
  end
  check("sector edge: some angle lands in the margin", found)
  check("sector edge: old path exactly then", edge_ok)
end

-- ── live fire test (lag points) ───────────────────────────────────────────
print("live fire")
do
  -- A stub env: the shell always crosses the pill square; the obstacle
  -- check finds our blocker pill when the shot starts west of X0.
  local X0 = 15 * 256
  local env = {
    pmx = 10, pmy = 10,
    sim = function() return { { mx = 10, my = 10 } } end,
    obs = function(x, y, ang)
      if x < X0 then return math.huge, "pill at (12,10)" end
      return 0, nil, true
    end,
  }
  local orig_env = FS.world_env
  FS.world_env = function() return env end
  local goal = { mx = 10, my = 10 }
  -- stopped east of X0, facing west: fire
  local info = { tankx = X0 + 40, tanky = 10 * 256 + 128, tank_angle = 192,
                 direction = 192, speed = 0, gunrange = 14 }
  local ok = FS.live_fire_ok(info, goal, {}, nil)
  check("stopped, clear: fire", ok == true)
  -- moving west at engine speed 16: the point 3 ticks on is past X0
  info.speed = 64
  local ok2, why, k = FS.live_fire_ok(info, goal, {}, nil)
  check("lag point on the blocker line: no fire", ok2 == false and k == 3,
        tostring(ok2) .. " " .. tostring(why) .. " k=" .. tostring(k))
  -- moving east (away from X0): every lag point is clear
  info.tank_angle, info.direction = 64, 64
  check("lag points clear: fire", FS.live_fire_ok(info, goal, {}, nil) == true)
  FS.world_env = orig_env
end

-- ── join the run from here, and the move back onto the line ──────────────
print("join and side correction")
do
  -- run line through F = (0,0) along D = 64 (east): S lies to the west
  local fs = { fx = 0, fy = 0, D = 64, apx = 3000, apy = 0, range = 2048,
               pills = { { 3000, 0 } } }
  fs.ux, fs.uy = FS.unit(64)
  -- y grows south; moving east, south is to the right
  check("side: north of the line is left (+)", FS.side_off(fs, -500, -40) > 0)
  check("side: south of the line is right (-)", FS.side_off(fs, -500, 40) < 0)
  -- on the line: the bearing to AP, sector D
  local h, sec = FS.steer_heading(fs, { tankx = -1000, tanky = 5 })
  check("on the line: steer at AP", sec == 64 and math.abs(U.adiff(h, 64)) < 1, h)
  -- 40 wu north: the next sector toward the line (south of east = 80)
  h, sec = FS.steer_heading(fs, { tankx = -1000, tanky = -40 })
  check("north of the line: sector 80", sec == 80 and FS.dir16(h) == 80, tostring(h) .. " " .. tostring(sec))
  h, sec = FS.steer_heading(fs, { tankx = -1000, tanky = 40 })
  check("south of the line: sector 48", sec == 48 and FS.dir16(h) == 48, tostring(h) .. " " .. tostring(sec))
  -- the heading sits the edge margin inside the sector
  check("heading inside the sector edge", math.abs(math.abs(U.adiff(h, 64)) - (8 + C.BLITZ_SETUP_16DIR_EDGE_BRAD)) < 1e-6, h)
  -- join: outside the range, room to move back onto the line
  local ok = FS.can_join(fs, -1000, -40)
  check("join: out of range, room to fix", ok == true)
  ok = FS.can_join(fs, 1500, 0)
  check("no join: inside a pill's range", ok == false)
  ok = FS.can_join(fs, -100, -60)
  check("no join: side offset too big for the run left", ok == false)
  ok = FS.can_join(fs, 100, 0)
  check("no join: past F", ok == false)
end

-- ── fallback: plan fails after the approach went to S ─────────────────────
-- plan_from_world gives nil for a goal with no _pt2, so plan_for_goal fails.
print("fallback")
do
  local info = { tankx = 1000, tanky = 1000 }
  local g = { mx = 10, my = 10, substate = "in_range_position", _approach_start = 5,
              approach_fx = 20.5, approach_fy = 21.5, approach_mx = 20, approach_my = 21,
              _fs_appr = { fx = 12.25, fy = 13.75, mx = 12, my = 13, sfx = 20.5, sfy = 21.5 } }
  local plan = FS.plan_for_goal({}, g, info, {}, 100, nil)
  check("plan fails", plan == nil)
  check("old approach point back", g.approach_fx == 12.25 and g.approach_fy == 13.75
        and g.approach_mx == 12 and g.approach_my == 13, g.approach_fx)
  check("approach runs again", g.substate == "approach" and g._approach_start == nil, g.substate)
  check("fast setup off for this goal", g._fs_off == true and g._fs_appr == nil)
  check("off: no new plan", FS.plan_for_goal({}, g, info, {}, 101, nil) == nil)
  local ax = g.approach_fx
  FS.approach_for_goal({}, g, info, {}, 102, nil)
  check("off: approach not moved", g.approach_fx == ax)
  -- approach not at S (a replan moved it): no undo
  local g2 = { mx = 10, my = 10, substate = "in_range_position",
               approach_fx = 30.5, approach_fy = 31.5, approach_mx = 30, approach_my = 31,
               _fs_appr = { fx = 12.25, fy = 13.75, mx = 12, my = 13, sfx = 20.5, sfy = 21.5 } }
  FS.plan_for_goal({}, g2, info, {}, 100, nil)
  check("approach not S: no undo", g2.approach_fx == 30.5 and g2.substate == "in_range_position"
        and not g2._fs_off, g2.approach_fx)
  -- approach never moved to S: no undo
  local g3 = { mx = 10, my = 10, substate = "in_range_position", approach_fx = 30.5, approach_fy = 31.5 }
  FS.plan_for_goal({}, g3, info, {}, 100, nil)
  check("no S: no undo", g3.substate == "in_range_position" and not g3._fs_off)
end

-- ── shot tracker knob ─────────────────────────────────────────────────────
print("shot tracker knob")
check("own shells only on by default", C.SHOT_TRACK_OWN_SHELLS_ONLY == true)
check("keel off", C.PRESETS.keel.SHOT_TRACK_OWN_SHELLS_ONLY == false)
check("mast off", C.PRESETS.mast.SHOT_TRACK_OWN_SHELLS_ONLY == false)

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
