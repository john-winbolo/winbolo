-- =========================================================================
-- test_lgm_aim.lua -- unit tests for lgm_aim.lua (the kill-LGM aim with keys
-- in flight) and the LGM_TELEPORT_REPEAT_VIEW velocity check in kill_lgm.
--
-- A small fake engine plays the server side, from the engine facts listed
-- at the top of lgm_aim.lua: two packets a think (keys tick, game tick); a
-- HELD key in both, a TAP only in the first; one sight step per packet
-- (2..14); fire at the start of the game tick from the present pose;
-- tankTurn twice a think with the 1/8 ramp on the first 6 held calls; the
-- move speed / 4 wu on the game tick. The keys a think sends reach this
-- engine D - 1 thinks later and show in the next snapshot (D = the delay).
-- The fake engine does not model speed changes, the first-press
-- tankVisibleTurn nudge, or terrain: speed stays as set, turn rule 1.
--
--   luajit test_lgm_aim.lua        the fixes (every LGM_AIM_* knob on)
--   luajit test_lgm_aim.lua old    every LGM_AIM_* knob and
--                                  LGM_TELEPORT_REPEAT_VIEW off = the old
--                                  aim; the fix checks FAIL here.
--   luajit test_lgm_aim.lua lgm_aim=<file>
--                                  load lgm_aim from <file> (for example the
--                                  parent commit's): test 11 checks a fix
--                                  with no knob (the delay measure), so
--                                  this is how it is seen to fail.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build-own-deps/luajit_src/src/luajit.exe test_lgm_aim.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path
local bit = require("bitcompat")

_G.KEY_SHOOT, _G.KEY_FASTER, _G.KEY_SLOWER = 0x01, 0x10, 0x20
_G.KEY_TURNLEFT, _G.KEY_TURNRIGHT = 0x04, 0x08
_G.KEY_MORERANGE, _G.KEY_LESSRANGE = 0x40, 0x80
TERRAIN_MASK = 0xFF
function get_terrain() return 7 end

local OLD = false
for _, a in ipairs(arg or {}) do
  if a == "old" then OLD = true end
  local la = a:match("^lgm_aim=(.+)$")
  if la then package.preload["lgm_aim"] = function() return dofile(la) end end
end
local C = require("constants")
local LA = require("lgm_aim")
local KL = require("kill_lgm")

local KNOBS = { "LGM_AIM_KEYS_IN_FLIGHT", "LGM_AIM_DELAY_EST", "LGM_AIM_LAUNCH_POSE",
                "LGM_AIM_TURN_RULE", "LGM_AIM_SEARCH_MOVE", "LGM_AIM_SIGHT_TAP",
                "LGM_TELEPORT_REPEAT_VIEW", "LGM_AIM_LEAD_CONVERGE",
                "LGM_SHELL_MODEL_RULES", "LGM_FIRE_LOS_STRICT", "LGM_LEAD_STOP_RETURN",
                "LGM_AIM_DELAY_CONFIRM", "LGM_AIM_DELAY_BEST_RATE" }
-- t[k] = false turns one knob off for a test that looks at another fix
-- alone. In "old" mode every knob is off whatever t says.
local function cfg(t)
  t = t or {}
  for _, k in ipairs(KNOBS) do C[k] = (not OLD) and (t[k] ~= false) end
  C.LGM_AIM_DELAY_DEFAULT = t.D0 or 1
  C.TEST_KEY_DELAY_THINKS = 0
end

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print(string.format("  ok   %s   (%s)", name, tostring(got)))
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

local band, bor = bit.band, bit.bor
local TWO_PI_256 = math.pi * 2 / 256
local function adiff(a, b) local d = (b - a) % 256; if d >= 128 then d = d - 256 end; return d end

-- ── fake engine ──────────────────────────────────────────────────────────
local function new_engine(o)
  return { x = o.x or 0, y = o.y or 0, ang = o.ang or 0, gun = o.gun or 14,
           speed = o.speed or 0, D = o.D or 1, q = {}, fl = 0, fr = 0, rate = 2,
           shots = {}, real = o.real }
end
-- The engine's shell (tank.c 683-689, shells.c, sim_rules.c classic table):
-- it starts start_add steps of speed ahead, moves L = max(0, 1 +
-- floor(life * sight / 2) - start_add) steps, and bursts. Returns the burst
-- distance and L.
local function engine_travel(sl, r)
  r = r or {}
  local speed, life, add = r.shell_speed or 32, r.shell_life or 8, r.shell_start_add or 5
  local L = 1 + math.floor(life * sl / 2) - add
  if L < 0 then L = 0 end
  return speed * (add + L), L
end
local function eng_gun(e, k)
  if band(k, KEY_MORERANGE) ~= 0 then e.gun = e.gun + 1
  elseif band(k, KEY_LESSRANGE) ~= 0 then e.gun = e.gun - 1 end
  if e.gun < 2 then e.gun = 2 elseif e.gun > 14 then e.gun = 14 end
end
local function eng_turn(e, k)
  local half = e.rate / 2
  if band(k, KEY_TURNLEFT) ~= 0 then
    local a = half
    if e.fl < 6 then e.fl = e.fl + 1; a = a / 8 end
    e.ang = (e.ang - a) % 256
  else e.fl = 0 end
  if band(k, KEY_TURNRIGHT) ~= 0 then
    local a = half
    if e.fr < 6 then e.fr = e.fr + 1; a = a / 8 end
    e.ang = (e.ang + a) % 256
  else e.fr = 0 end
end
local function eng_step(e, keys, taps)
  local p1 = bor(keys, taps)
  eng_gun(e, p1); eng_turn(e, p1)
  eng_gun(e, keys)
  if band(p1, KEY_SHOOT) ~= 0 then
    local a = e.ang * TWO_PI_256
    -- e.real: the engine's burst (tests 10+); else the old 128 * sight.
    local tr = e.real and engine_travel(e.gun) or 128 * e.gun
    e.shots[#e.shots + 1] = { x = e.x + math.sin(a) * tr,
                              y = e.y - math.cos(a) * tr, gun = e.gun }
  end
  eng_turn(e, keys)
  local a = e.ang * TWO_PI_256
  e.x = e.x + math.sin(a) * e.speed / 4
  e.y = e.y - math.cos(a) * e.speed / 4
end
local function snapshot(e)
  return { tankx = e.x, tanky = e.y, tank_angle = e.ang, direction = math.floor(e.ang),
           gunrange = e.gun, speed = e.speed, rules = { gunsight_max = 14 } }
end
local function eng_send(e, keys, taps)
  e.q[#e.q + 1] = { keys, taps }
  if #e.q >= e.D then
    local k = table.remove(e.q, 1)
    eng_step(e, k[1], k[2])
  end
end

-- ── one brain think, as init.lua does it ─────────────────────────────────
-- 1. fire check (snapshot burst, or lgm_aim.launch_impact with the launch
--    pose knob); 2. the crosshair search; 3. freeze_fire_keys + note_sent.
local function think(state, e, now, ax, ay, fire)
  local info = snapshot(e)
  local keys, taps = 0, 0
  local ex, ey
  if C.LGM_AIM_LAUNCH_POSE then
    ex, ey = LA.launch_impact(state, info, now, keys, taps, 2)
  else
    local a = info.direction * TWO_PI_256
    ex = info.tankx + math.sin(a) * 128 * info.gunrange
    ey = info.tanky - math.cos(a) * 128 * info.gunrange
  end
  local off = math.sqrt((ex - ax) ^ 2 + (ey - ay) ^ 2)
  if fire and not state._fired and off <= C.KILL_LGM_FIRE_WU then
    taps = bor(taps, KEY_SHOOT)
    state._fired = now
    if C.LGM_AIM_LAUNCH_POSE then LA.note_fire(state, now, keys, taps) end
  end
  local hk, tk = LA.search(state, info, now, ax, ay, 2, C.KILL_LGM_TURN_DELTA,
                           C.KILL_LGM_SPEED_DELTA, false)
  local CLEAR = bor(bor(bor(KEY_TURNLEFT, KEY_TURNRIGHT), bor(KEY_FASTER, KEY_SLOWER)),
                    bor(KEY_MORERANGE, KEY_LESSRANGE))
  keys = bor(band(keys, bit.bnot(CLEAR)), hk)
  if tk ~= 0 then taps = bor(band(taps, bit.bnot(CLEAR)), tk) end
  keys, taps = LA.freeze_fire_keys(state, keys, taps, now)
  keys, taps = LA.note_sent(state, info, keys, taps, now)
  eng_send(e, keys, taps)
  return info
end

print(OLD and "MODE: old (every LGM_AIM_* knob off)" or "MODE: fixes (every LGM_AIM_* knob on)")

-- ── 0. with every knob off, lgm_aim.search is the old 27-move loop ───────
-- (init.lua keeps its own copy of the old loop for that case; this proves
-- the test's "old" mode below really is the old search.)
print("0. knobs off: search = the old loop")
do
  local saved = OLD; OLD = true; cfg(); OLD = saved
  local function old_loop(info, lx, ly)
    local TD, SD = C.KILL_LGM_TURN_DELTA, C.KILL_LGM_SPEED_DELTA
    local T = { { 0, 0 }, { KEY_TURNLEFT, -TD }, { KEY_TURNRIGHT, TD } }
    local S = { { 0, 0 }, { KEY_FASTER, SD }, { KEY_SLOWER, -SD } }
    local G = { { 0, 0 }, { KEY_MORERANGE, 2 }, { KEY_LESSRANGE, -2 } }
    local best, bk = math.huge, 0
    for _, t in ipairs(T) do
      local nd = (info.direction + t[2]) % 256
      local r = nd * TWO_PI_256
      for _, s in ipairs(S) do
        local ns = math.max(0, math.min(128, info.speed + s[2]))
        local px, py = info.tankx + math.sin(r) * ns, info.tanky - math.cos(r) * ns
        for _, g in ipairs(G) do
          local ng = math.max(2, math.min(14, info.gunrange + g[2]))
          local cx, cy = px + math.sin(r) * ng / 2 * 256, py - math.cos(r) * ng / 2 * 256
          local sc = math.sqrt((cx - lx) ^ 2 + (cy - ly) ^ 2)
          if sc < best then best, bk = sc, bor(bor(t[1], s[1]), g[1]) end
        end
      end
    end
    return bk, best
  end
  local seed, same, n = 12345, 0, 0
  local function rnd(m) seed = (seed * 1103515245 + 12345) % 2147483648; return seed % m end
  for _ = 1, 300 do
    local info = { tankx = 20000 + rnd(500), tanky = 20000 + rnd(500), direction = rnd(256),
                   gunrange = 2 + rnd(13), speed = rnd(129), rules = { gunsight_max = 14 } }
    info.tank_angle = info.direction + 0.4
    local lx, ly = 20000 + rnd(4000) - 1500, 20000 + rnd(4000) - 1500
    local k1, s1 = old_loop(info, lx, ly)
    local k2, t2, s2 = LA.search({}, info, 100, lx, ly, 2, C.KILL_LGM_TURN_DELTA,
                                 C.KILL_LGM_SPEED_DELTA, false)
    n = n + 1
    if k1 == k2 and t2 == 0 and math.abs(s1 - s2) < 1e-6 then same = same + 1 end
  end
  check("300 random poses: same keys and score as the old loop", same == n, same .. "/" .. n)
end
cfg()

-- ── 1. keys in flight: the sight does not swing ──────────────────────────
-- Recorded case: delay 3, the sight swung 14 -> 2 -> 12 -> 2 -> 10 -> 2 round
-- a target that needed a nearly fixed sight. Here: nose on the target,
-- target at sight 6 (768 wu), sight starts at 14, delay 3, no fire.
print("1. keys in flight (LGM_AIM_KEYS_IN_FLIGHT), delay 3")
do
  cfg({ LGM_AIM_DELAY_EST = false, D0 = 3 })
  local e = new_engine({ D = 3, gun = 14 })
  local st, guns, minimum, turns = {}, {}, 99, 0
  local last_dir = 0
  for t = 1, 40 do
    think(st, e, t, 0, -768, false)
    guns[#guns + 1] = e.gun
    if t > 1 then
      local d = guns[t] - guns[t - 1]
      if d ~= 0 then
        local sgn = d > 0 and 1 or -1
        if last_dir ~= 0 and sgn ~= last_dir then turns = turns + 1 end
        last_dir = sgn
      end
    end
    if e.gun < minimum then minimum = e.gun end
  end
  local trace = table.concat(guns, " ", 1, 20)
  check("sight never goes below the target sight 6", minimum >= 6,
        "min " .. minimum .. "; sight by think: " .. trace)
  check("sight does not reverse (no triangle wave)", turns == 0, turns .. " reversals")
  check("sight ends on 6", e.gun == 6, e.gun)
end

-- ── 2. the delay estimate ────────────────────────────────────────────────
print("2. delay measured online (LGM_AIM_DELAY_EST)")
for _, Dreal in ipairs({ 3, 1, 5 }) do
  cfg({ D0 = 1 })
  local e = new_engine({ D = Dreal, gun = 8 })
  local st = {}
  -- The target walks across the sight so keys keep changing.
  for t = 1, 120 do
    local ty = -128 * (6 + 4 * math.sin(t / 7))
    local tx = 300 * math.sin(t / 11)
    think(st, e, t, tx, ty, false)
  end
  check(string.format("real delay %d: the planner's delay is %d", Dreal, Dreal),
        LA.delay(st) == Dreal, LA.delay(st))
end

-- ── 3. fire check at the launch pose ─────────────────────────────────────
print("3. fire check at the launch pose (LGM_AIM_LAUNCH_POSE)")
do
  -- a) direct: the snapshot sight is 6 (on target) but two -2 steps are in
  --    flight; the shell would leave with sight 2.
  cfg({ D0 = 3, LGM_AIM_DELAY_EST = false })
  local st = {}
  local info = { tankx = 0, tanky = 0, tank_angle = 0, direction = 0, gunrange = 6,
                 speed = 0, rules = { gunsight_max = 14 } }
  LA.note_sent(st, info, KEY_LESSRANGE, 0, 8)
  LA.note_sent(st, info, KEY_LESSRANGE, 0, 9)
  local ex, ey
  if C.LGM_AIM_LAUNCH_POSE then ex, ey = LA.launch_impact(st, info, 10, 0, 0, 2)
  else ex, ey = 0, -128 * info.gunrange end
  local off = math.sqrt(ex * ex + (ey + 768) ^ 2)
  check("a) sight 6 seen, 2 steps down in flight: the check sees the 512 wu miss",
        off > C.KILL_LGM_FIRE_WU, string.format("burst %.0f wu off", off))
  -- b) the freeze keeps the fire think's sight / turn keys as scored.
  LA.note_fire(st, 10, 0, KEY_SHOOT)
  local k2, t2 = LA.freeze_fire_keys(st, KEY_MORERANGE + KEY_TURNLEFT, KEY_SHOOT, 10)
  check("b) fire think: sight / turn keys put back as scored (none)",
        band(k2, KEY_MORERANGE + KEY_TURNLEFT) == 0, string.format("keys 0x%x", k2))
  -- c) closed loop, the search alone not fixed (keys in flight off): every
  --    shell must land where the check said.
  cfg({ D0 = 3, LGM_AIM_DELAY_EST = false, LGM_AIM_KEYS_IN_FLIGHT = false })
  local e = new_engine({ D = 3, gun = 14 })
  st = {}
  for t = 1, 40 do think(st, e, t, 0, -768, true) end
  local s = e.shots[1]
  local miss = s and math.sqrt(s.x ^ 2 + (s.y + 768) ^ 2) or -1
  check("c) delay 3, search not fixed: the shell lands within 128 wu",
        s and miss <= C.KILL_LGM_FIRE_WU,
        s and string.format("left with sight %d, %.0f wu off", s.gun, miss) or "no shot")
end

-- ── 4. teleport check: a repeated snapshot keeps the velocity ────────────
print("4. teleport check (LGM_TELEPORT_REPEAT_VIEW)")
do
  cfg()
  local st, lgm = {}, { idnum = 7, wx = 1000, wy = 2000 }
  local vx
  -- 12 thinks of +16 wu, then the same place twice, then +48 (the 3 walks).
  local x, now = 1000, 0
  for _ = 1, 12 do now = now + 1; x = x + 16; lgm.wx = x; vx = KL.update_velocity(st, lgm, now) end
  now = now + 1; vx = KL.update_velocity(st, lgm, now)
  now = now + 1; vx = KL.update_velocity(st, lgm, now)
  now = now + 1; x = x + 48; lgm.wx = x; vx = KL.update_velocity(st, lgm, now)
  local h = st._enemy_lgm_history[7]
  check("same place for 2 thinks, then +48: velocity kept (about 16 wu a think)",
        vx > 8, string.format("vx=%.1f status=%s", vx, tostring(h.lock_status)))
  -- A real jump still resets.
  now = now + 1; x = x + 1600; lgm.wx = x; vx = KL.update_velocity(st, lgm, now)
  check("a 1600 wu jump still resets", vx == 0, string.format("vx=%.1f", vx))
end

-- ── 5. the real turn rate ────────────────────────────────────────────────
-- Delay 1 (a bot in the server), sight already right (6), the nose 21 brads
-- left of the target. The old search turns 6 brads a think in its head, so
-- it stops when the error is under 3 brads; the engine turns 2.
print("5. real turn rate (LGM_AIM_TURN_RULE), delay 1")
do
  cfg()
  local e = new_engine({ D = 1, gun = 6, ang = 256 - 21 })
  local st = {}
  for t = 1, 40 do think(st, e, t, 0, -768, false) end
  local err = math.abs(adiff(e.ang, 0))
  check("nose ends within one turn call (1 brad) of the target",
        err <= 1, string.format("%.2f brads = %.0f wu at 768 wu", err, err * TWO_PI_256 * 768))
end

-- ── 6. the search moves the tank speed / 4 wu a think ────────────────────
-- Delay 1, nose on, speed 128 (the engine moves 32 wu a think). For each
-- target distance the move chosen must be the one whose REAL crosshair next
-- think is nearest the target.
print("6. search move speed / 4 (LGM_AIM_SEARCH_MOVE), delay 1")
do
  cfg()
  local wrong, n, ex = 0, 0, ""
  for d = 400, 1800, 8 do
    local e = new_engine({ D = 1, gun = 8, speed = 128 })
    local info = snapshot(e)
    local hk, tk = LA.search({}, info, 1, 0, -d, 2, C.KILL_LGM_TURN_DELTA,
                             C.KILL_LGM_SPEED_DELTA, false)
    local best = math.huge
    local function real(k, t)
      local e2 = new_engine({ D = 1, gun = 8, speed = 128 })
      eng_step(e2, k, t)
      -- The crosshair = the burst the brain models (KL.shell_travel: the
      -- engine's with LGM_SHELL_MODEL_RULES, else 128 * sight).
      return math.abs(e2.y - KL.shell_travel(e2.gun) + d)
    end
    for _, g in ipairs({ { 0, 0 }, { KEY_MORERANGE, 0 }, { KEY_LESSRANGE, 0 },
                         { 0, KEY_MORERANGE }, { 0, KEY_LESSRANGE } }) do
      if not (g[2] ~= 0 and not C.LGM_AIM_SIGHT_TAP) then
        best = math.min(best, real(g[1], g[2]))
      end
    end
    local gk = band(hk, KEY_MORERANGE + KEY_LESSRANGE)
    local got = real(gk, band(tk, KEY_MORERANGE + KEY_LESSRANGE))
    n = n + 1
    if got > best + 1e-6 then
      wrong = wrong + 1
      if ex == "" then ex = string.format(" (first: %d wu, %.0f vs best %.0f)", d, got, best) end
    end
  end
  check("every pick has the nearest real crosshair", wrong == 0, wrong .. "/" .. n .. " wrong" .. ex)
end

-- ── 7. sight taps reach an odd sight ─────────────────────────────────────
print("7. sight taps (LGM_AIM_SIGHT_TAP), delay 1")
do
  cfg()
  local e = new_engine({ D = 1, gun = 14 })
  local st = {}
  for t = 1, 30 do think(st, e, t, 0, -128 * 7, false) end
  local off = math.abs(e.y - 128 * e.gun + 128 * 7)
  check("target at sight 7: the crosshair ends within half a step (64 wu)",
        off <= 64, string.format("sight %d, %.0f wu off", e.gun, off))
end

-- ── 8. the recorded case, all fixes: delay 3, nose off, sight 14 ─────────
-- One closed loop with every fix: delay 3 (the estimate starts at 1 and must
-- find it), the nose 20 brads off, sight 14, target at 4.5 tiles walking
-- across at 16 wu a think. The first shell must land within 128 wu of him.
print("8. recorded case, delay 3, nose 20 off, man walking 16 wu a think")
do
  cfg()
  local e = new_engine({ D = 3, gun = 14, ang = 20, speed = 24 })
  local st = {}
  local mx0, my0 = -600, -1100
  local function man(t) return mx0 + 16 * t, my0 end
  local shot_t
  for t = 1, 150 do
    -- Aim point: where he will be when the shell bursts (flight time
    -- 4 x sight - 5 brain ticks, kill_lgm.flight_ticks) - the lead the
    -- brain already has; the burst is judged against the real man.
    local info = snapshot(e)
    local ft = 4 * info.gunrange - 5
    local ax, ay = man(t + LA.delay(st) + ft)
    local before = #e.shots
    think(st, e, t, ax, ay, true)
    if #e.shots > before and not shot_t then shot_t = t end
  end
  local s = e.shots[1]
  local miss = -1
  if s then
    -- The shell left on the engine step run during think shot_t; it bursts
    -- 4 x sight - 5 brain ticks later.
    local mx, my = man(shot_t + (4 * s.gun - 5))
    miss = math.sqrt((s.x - mx) ^ 2 + (s.y - my) ^ 2)
  end
  check("first shell lands within 128 wu of the man", s and miss <= 128,
        s and string.format("sight %d, %.0f wu off, delay est %d", s.gun, miss, LA.delay(st)) or "no shot")
end

-- ── 9. the lead point: flight time to the lead point, not to the man ─────
-- tests/scenario/lgm_kill_lag walk 1 (seed 42, brain tick 162): our tank at
-- (32364, 33101), the man at (31232, 32112) walking east at 16 wu a think.
-- He is 5.9 squares away now, but his lead point is only 4.2 squares away,
-- so the shell's real flight is 27 ticks, not 43. A shell that bursts on the
-- aim point bursts flight(aim) ticks after it leaves; the man must be there.
print("9. lead point (LGM_AIM_LEAD_CONVERGE)")
do
  cfg()
  local tx, ty = 32364, 33101
  local lgm = { wx = 31232, wy = 32112, v_ema_x = 16.0, v_ema_y = 0.0 }
  local ax, ay = KL.predict_aim(tx, ty, lgm, nil)
  local real_ft = KL.flight_ticks(KL.sight_for(
    math.sqrt((ax - tx) ^ 2 + (ay - ty) ^ 2)))
  local mx, my = lgm.wx + 16 * real_ft, lgm.wy
  local off = math.sqrt((ax - mx) ^ 2 + (ay - my) ^ 2)
  check("the aim point is where he is when a shell to it bursts (<= 64 wu)",
        off <= 64, string.format("lead %.0f wu, flight %d, %.0f wu off", ax - lgm.wx, real_ft, off))
end

-- ── 10. the shell model from the rules (LGM_SHELL_MODEL_RULES) ────────────
-- Play test 20261121_231141: 13 of 13 full-life hunt shells burst 31-39 wu
-- past the brain's burst. The model must match the engine's shell for the
-- classic rules and for other rules, within 16 wu, at every sight.
print("10. shell model from the rules (LGM_SHELL_MODEL_RULES)")
do
  cfg()
  local function run(r, name)
    KL.set_rules(r)
    local worst, worst_sl, bad_t = 0, 0, 0
    local hi = (r and r.gunsight_max) or 14
    for sl = 2, hi do
      local tr, L = engine_travel(sl, r)
      local e = math.abs(KL.shell_travel(sl) - tr)
      if e > worst then worst, worst_sl = e, sl end
      -- The man that counts is where he is one tick before the burst
      -- (shells.c SHELL_LGM_REWIND_LESS): L - 1 ticks after the fire tick.
      local want = L - 1; if want < 1 then want = 1 end
      if KL.flight_ticks(sl) ~= want then bad_t = bad_t + 1 end
    end
    check(name .. ": burst within 16 wu of the engine at every sight",
          worst <= 16, string.format("worst %.0f wu at sight %d", worst, worst_sl))
    check(name .. ": flight time = engine L - 1 at every sight", bad_t == 0,
          bad_t .. " sights wrong")
  end
  run(nil, "classic rules")
  run({ shell_speed = 40, shell_life = 10, shell_start_add = 4, gunsight_max = 12,
        lgm_build_ticks = 30 }, "rules speed 40 life 10 start_add 4 max 12")
  KL.set_rules(nil)
  -- Closed loop on the engine's shell: delay 1, nose on, the man still at
  -- 900 wu. The shell the fire check passes must burst within 16 wu of the
  -- model burst the check scored.
  local e = new_engine({ D = 1, gun = 14, real = true })
  local st, scored = {}, nil
  for t = 1, 40 do
    local info = snapshot(e)
    local before = #e.shots
    think(st, e, t, 0, -900, true)
    if #e.shots > before then
      local bx, by
      if C.LGM_AIM_LAUNCH_POSE then
        bx, by = LA.launch_impact(st, info, t, 0, KEY_SHOOT, 2)
      else
        bx, by = 0, -KL.shell_travel(info.gunrange)
      end
      scored = { bx = bx, by = by }
      break
    end
  end
  local s = e.shots[1]
  local err = (s and scored) and math.sqrt((s.x - scored.bx) ^ 2 + (s.y - scored.by) ^ 2) or -1
  check("closed loop: the real burst is within 16 wu of the model burst",
        s and err <= 16, s and string.format(
          "sight %d, real burst %.0f wu from the model, %.0f wu from the man",
          s.gun, err, math.sqrt(s.x ^ 2 + (s.y + 900) ^ 2)) or "no shot")
end

-- ── 11. the delay measure does not run away (no knob: LGM_AIM_DELAY_EST) ──
-- Play test 20261121_231141_1, bot 1, tick 2037: a bot in the server
-- (delay 1), idle at sight 14, sent one key and then none. 32 thinks later
-- the d = 1 sample left the window first; D went 1 -> 2 on "1 error in 1",
-- then 2 -> 3, and on up to 14. The fix has no knob, so this test runs the
-- measure on in both modes; run it with lgm_aim=<the parent's file> to see
-- the old measure fail.
print("11. delay measure: no runaway from one key (delay 1, idle, sight 14)")
do
  cfg()
  C.LGM_AIM_DELAY_EST = true
  local e = new_engine({ D = 1, gun = 14 })
  local st, top, first_bad = {}, 1, nil
  for t = 1, 120 do
    local info = snapshot(e)
    local k = (t == 3) and KEY_TURNLEFT or 0
    local keys, taps = LA.note_sent(st, info, k, 0, t)
    eng_send(e, keys, taps)
    local D = LA.delay(st)
    if D > top then top = D end
    if D ~= 1 and not first_bad then first_bad = t end
  end
  check("D stays 1 over 120 thinks", top == 1,
        string.format("highest D %d%s", top, first_bad and (", first off 1 at think " .. first_bad) or ""))
  -- Busy keys after an idle stretch: the measure still finds a real delay 3
  -- (the minimum sample count does not stop it).
  cfg()
  C.LGM_AIM_DELAY_EST = true
  e = new_engine({ D = 3, gun = 8 })
  st = {}
  for t = 1, 40 do
    local info = snapshot(e)
    local keys, taps = LA.note_sent(st, info, 0, 0, t)
    eng_send(e, keys, taps)
  end
  for t = 41, 160 do
    local ty = -128 * (6 + 4 * math.sin(t / 7))
    think(st, e, t, 300 * math.sin(t / 11), ty, false)
  end
  check("then a real delay 3 is still found", LA.delay(st) == 3, LA.delay(st))
  cfg()
end

-- ── 12. stop-and-return lead (LGM_LEAD_STOP_RETURN) ──────────────────────
-- A builder on a job: he walks out east at 16 wu a tick, stops, works for
-- lgm_build_ticks (20) ticks, and walks back west the way he came (lgm.c:
-- waitTime = lgm_build_ticks on the arrival tick, first step back after
-- it). Our tank stands 1100 wu north of his stop spot. At every tick of the
-- stop and of the walk back, the man at (this tick + the flight time to
-- the aim point) must be within 64 wu of the aim point.
-- Play test: bot 1 ticks 8386-8426 missed by 400-700 wu in these phases.
print("12. stop-and-return lead (LGM_LEAD_STOP_RETURN)")
do
  cfg()
  local X0, Y0, WALK, WORK = 20000, 30000, 60, 20
  local A = WALK                       -- arrival tick (his last step out)
  local function man_at(t)
    if t <= A then return X0 + 16 * t, Y0 end
    if t <= A + WORK then return X0 + 16 * A, Y0 end
    local back = t - (A + WORK)
    if back > A then back = A end
    return X0 + 16 * A - 16 * back, Y0
  end
  local TX, TY = X0 + 16 * A, Y0 - 1100
  local st = {}
  local lgm = { idnum = 5, wx = X0, wy = Y0 }
  KL.update_velocity(st, lgm, 0)
  local worst = { stop = 0, back = 0 }
  local worst_t = { stop = 0, back = 0 }
  local n = { stop = 0, back = 0 }
  local hit = { stop = 0, back = 0 }
  for t = 1, A + WORK + 24 do
    lgm.wx, lgm.wy = man_at(t)
    local vx, vy = KL.update_velocity(st, lgm, t)
    lgm.v_ema_x, lgm.v_ema_y = vx, vy
    local h = st._enemy_lgm_history[5]
    local ax, ay = KL.predict_aim(TX, TY, lgm, h)
    local T = KL.flight_ticks(KL.sight_for(math.sqrt((ax - TX) ^ 2 + (ay - TY) ^ 2)))
    local rx, ry = man_at(t + T)
    local miss = math.sqrt((ax - rx) ^ 2 + (ay - ry) ^ 2)
    local ph = (t > A and t <= A + WORK) and "stop" or (t > A + WORK and "back" or nil)
    if ph then
      n[ph] = n[ph] + 1
      if miss <= 64 then hit[ph] = hit[ph] + 1 end
      if miss > worst[ph] then worst[ph], worst_t[ph] = miss, t end
    end
  end
  check("stop phase: the aim is on him at the burst (<= 64 wu) every tick",
        hit.stop == n.stop, string.format("%d/%d; worst %.0f wu at tick %d",
          hit.stop, n.stop, worst.stop, worst_t.stop))
  check("walk back: the aim is on him at the burst (<= 64 wu) every tick",
        hit.back == n.back, string.format("%d/%d; worst %.0f wu at tick %d",
          hit.back, n.back, worst.back, worst_t.back))
end

-- 13. The fire check's line of sight (kill_lgm.fire_line_blocked and
-- los_cache_ok, LGM_FIRE_LOS_STRICT). Play test 20261121_231141_1, bot 2,
-- tick 4888: the man stood on the live pill he built on his work tile
-- (140,115); the shell, aimed at him one tile on (141,115), hit the pill.
-- The tank is due west on row 115. The old walk stopped at the man's tile
-- before it tested for a blocker, so the shot read as clear.
print("13. fire line of sight (LGM_FIRE_LOS_STRICT)")
do
  cfg()
  local ROW = 115
  local function line(mx0, mx1)
    local t = {}
    for mx = mx0, mx1 do t[#t + 1] = { mx = mx, my = ROW } end
    return t
  end
  local blockers = {}
  local function stops(mx, my) return blockers[my * 256 + mx] == true end
  local function q(man_mx, aim_mx, extra)
    local r = { tank_wx = 130 * 256 + 128, tank_wy = ROW * 256 + 128,
                aim_wx = aim_mx * 256 + 128, aim_wy = ROW * 256 + 128,
                origin_mx = 130, origin_my = ROW, man_mx = man_mx, man_my = ROW,
                aim_mx = aim_mx, aim_my = ROW }
    for k, v in pairs(extra or {}) do r[k] = v end
    return r
  end
  local function blocked(x0, x1, man_mx, aim_mx, extra)
    return KL.fire_line_blocked(line(x0, x1), q(man_mx, aim_mx, extra), stops)
  end
  blockers = { [ROW * 256 + 140] = true }      -- the live pill on his work tile
  local b = blocked(130, 142, 140, 141)
  check("pill on his tile, aim one tile on: blocked", b == true, tostring(b))
  b = blocked(130, 142, 140, 140)
  check("pill on his tile, aim on his tile: clear (the blast there kills him)",
        b == false, tostring(b))
  blockers = { [ROW * 256 + 143] = true }      -- a wall two tiles past the aim
  b = blocked(130, 144, 140, 141)
  check("wall two tiles past the aim tile: clear", b == false, tostring(b))
  blockers = { [ROW * 256 + 136] = true }      -- a pill on the way
  b = blocked(130, 142, 140, 141)
  check("pill short of the man: blocked", b == true, tostring(b))
  blockers = { [ROW * 256 + 141] = true }      -- the destination pill (LGM_DEST_AIM)
  b = blocked(130, 143, 140, 142, { dest_mx = 141, dest_my = ROW })
  check("aim past the destination pill's tile: it is the target, clear",
        b == false, tostring(b))
  -- The kept answer: the check at about tick 4880 saw him on tile 139 with
  -- the aim on 140. Two ticks on, he is on 140 and the aim on 141.
  local ce = { tick = 4880, blocked = false, mmx = 139, mmy = ROW, amx = 140, amy = ROW }
  local ok = KL.los_cache_ok(ce, 4882, 10, 140, ROW, 141, ROW)
  check("kept answer not used once the man's tile or the aim tile change",
        ok == false, tostring(ok))
  ok = KL.los_cache_ok(ce, 4882, 10, 139, ROW, 140, ROW)
  check("kept answer used while both stay the same", ok == true, tostring(ok))
end

-- 14. Stop-and-return lead, overdue stand (LGM_LEAD_STOP_RETURN, Andrew
-- 2026-11-22: "if they don't start moving when they should, then the
-- target should jump back to its current position"). The man walks out
-- for WALK ticks and then stands 3 x the work time (stuck, or a job the
-- model does not know). From the first tick he should have walked back
-- and has not, the aim must be on him where he stands, in his tile.
print("14. stop-and-return lead, overdue stand (LGM_LEAD_STOP_RETURN)")
do
  cfg()
  local X0, Y0, WALK = 20000, 30000, 60
  local WORK = 20                      -- lgm_build_ticks (classic rules)
  local STAND = 3 * WORK               -- he stands this long
  local A = WALK
  local function man_at(t)
    if t <= A then return X0 + 16 * t, Y0 end
    return X0 + 16 * A, Y0
  end
  local TX, TY = X0 + 16 * A, Y0 - 1100
  local st = {}
  local lgm = { idnum = 6, wx = X0, wy = Y0 }
  KL.update_velocity(st, lgm, 0)
  local n, on, worst, worst_t = 0, 0, 0, 0
  for t = 1, A + STAND do
    lgm.wx, lgm.wy = man_at(t)
    local vx, vy = KL.update_velocity(st, lgm, t)
    lgm.v_ema_x, lgm.v_ema_y = vx, vy
    local h = st._enemy_lgm_history[6]
    local ax, ay = KL.predict_aim(TX, TY, lgm, h)
    -- He is seen standing from tick A + 1, so he should take his first
    -- step back at tick A + WORK + 1. From then on he is overdue.
    if t >= A + WORK + 1 then
      n = n + 1
      local miss = math.sqrt((ax - lgm.wx) ^ 2 + (ay - lgm.wy) ^ 2)
      local same_tile = bit.rshift(math.floor(ax), 8) == bit.rshift(math.floor(lgm.wx), 8)
                        and bit.rshift(math.floor(ay), 8) == bit.rshift(math.floor(lgm.wy), 8)
      if same_tile then on = on + 1 end
      if miss > worst then worst, worst_t = miss, t end
    end
  end
  check("overdue stand: the aim is in his tile every tick",
        on == n, string.format("%d/%d; worst %.0f wu from him at tick %d", on, n, worst, worst_t))
end

-- ── 15. delay confirm (LGM_AIM_DELAY_CONFIRM) ────────────────────────────
-- The fire check may fire only once D has LGM_AIM_DELAY_WINDOW / 2 samples
-- in the window; a change of D makes it wait again for the new D.
print("15. delay confirm (LGM_AIM_DELAY_CONFIRM)")
if OLD then
  cfg()
  check("knob off: always confirmed", LA.delay_confirmed({}) == true, LA.delay_confirmed({}))
else
  for _, Dreal in ipairs({ 1, 3 }) do
    cfg({ D0 = 1 })
    local e = new_engine({ D = Dreal, gun = 8 })
    local st = {}
    local first_conf, changes, lastD = nil, 0, LA.delay(st)
    local conf_on_wrong = false
    check(string.format("real delay %d: not confirmed before any sample", Dreal),
          LA.delay_confirmed(st) == false, LA.delay_confirmed(st))
    for t = 1, 160 do
      local ty = -128 * (6 + 4 * math.sin(t / 7))
      think(st, e, t, 300 * math.sin(t / 11), ty, false)
      local D = LA.delay(st)
      if D ~= lastD then changes = changes + 1; lastD = D end
      if LA.delay_confirmed(st) then
        if not first_conf then first_conf = t end
        if D ~= Dreal then conf_on_wrong = true end
      end
    end
    check(string.format("real delay %d: confirmed in the end on D %d", Dreal, Dreal),
          LA.delay_confirmed(st) and LA.delay(st) == Dreal,
          string.format("D %d, first confirm at think %s, %d change(s)", LA.delay(st),
                        tostring(first_conf), changes))
    -- C.LGM_AIM_DELAY_BEST_RATE: D is confirmed only with the lowest error
    -- rate. Without it, with a real delay 3, D = 1 got its 16 samples first
    -- (think 9, 7 errors) and was confirmed until D moved on.
    check(string.format("real delay %d: never confirmed on a wrong D", Dreal),
          not conf_on_wrong, conf_on_wrong)
  end
  -- lgm_kill_lag_near, 3-tick, every seed, t=122: the window held
  -- 1:4/15 2:2/15 3:0/15 4:2/16 5:3/16 ... 14:12/16 (errors / samples).
  -- D = 3 has no error but one sample short of 16; D = 4 has 16.
  local NN = { 15, 15, 15, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16 }
  local EE = { 4, 2, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }
  local function t122(best_rate)
    cfg({ D0 = 1 })
    C.LGM_AIM_DELAY_BEST_RATE = best_rate
    local st = {}
    local s = LA.st_of(st)
    for j = 1, 16 do
      local err, inf = {}, {}
      for d = 1, 14 do
        inf[d] = (j <= NN[d]) and 1 or 0
        err[d] = (j <= EE[d]) and 1 or 0
      end
      local t = 106 + j
      s.recs[t % (C.LGM_AIM_DELAY_MAX + C.LGM_AIM_DELAY_WINDOW + 2)] = { tick = t, g1 = 0, g2 = 0,
        t1 = 0, t2 = 0, err = err, inf = inf }
    end
    LA._choose(s, 122)
    return st, s
  end
  local st, s = t122(false)
  check("t=122 case, best rate off: D = 4 wins and is confirmed (the old fault)",
        LA.delay(st) == 4 and LA.delay_confirmed(st), string.format("D %d confirmed %s",
        LA.delay(st), tostring(LA.delay_confirmed(st))))
  st, s = t122(true)
  check("t=122 case, best rate on: D stays 1, not confirmed (D = 3 at 0/15 blocks)",
        LA.delay(st) == 1 and not LA.delay_confirmed(st), string.format("D %d confirmed %s",
        LA.delay(st), tostring(LA.delay_confirmed(st))))
  -- t=123: one more sample, for D = 3 only, with no error.
  local err, inf = {}, {}
  for d = 1, 14 do err[d], inf[d] = 0, (d == 3) and 1 or 0 end
  s.recs[123 % (C.LGM_AIM_DELAY_MAX + C.LGM_AIM_DELAY_WINDOW + 2)] = { tick = 123, g1 = 0, g2 = 0,
    t1 = 0, t2 = 0, err = err, inf = inf }
  LA._choose(s, 123)
  check("t=123: D = 3 has 16 samples, wins and is confirmed",
        LA.delay(st) == 3 and LA.delay_confirmed(st), string.format("D %d confirmed %s",
        LA.delay(st), tostring(LA.delay_confirmed(st))))
  cfg()
  -- The flag and the two ways round it.
  cfg({ D0 = 1 })
  local st = {}
  local s = LA.st_of(st)
  s.confirmed = true
  check("confirmed with the flag set", LA.delay_confirmed(st) == true)
  s.confirmed = false
  check("not confirmed with the flag clear", LA.delay_confirmed(st) == false)
  C.LGM_AIM_DELAY_CONFIRM = false
  check("knob off: confirmed whatever the flag", LA.delay_confirmed(st) == true)
  C.LGM_AIM_DELAY_CONFIRM = true
  C.LGM_AIM_DELAY_EST = false
  check("LGM_AIM_DELAY_EST off (fixed D): confirmed", LA.delay_confirmed(st) == true)
  cfg()
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
