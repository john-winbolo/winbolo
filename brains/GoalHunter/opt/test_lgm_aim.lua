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

local OLD = (arg and arg[1] == "old")
local C = require("constants")
local LA = require("lgm_aim")
local KL = require("kill_lgm")

local KNOBS = { "LGM_AIM_KEYS_IN_FLIGHT", "LGM_AIM_DELAY_EST", "LGM_AIM_LAUNCH_POSE",
                "LGM_AIM_TURN_RULE", "LGM_AIM_SEARCH_MOVE", "LGM_AIM_SIGHT_TAP",
                "LGM_TELEPORT_REPEAT_VIEW", "LGM_AIM_LEAD_CONVERGE" }
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
           shots = {} }
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
    e.shots[#e.shots + 1] = { x = e.x + math.sin(a) * 128 * e.gun,
                              y = e.y - math.cos(a) * 128 * e.gun, gun = e.gun }
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
      return math.abs(e2.y - 128 * e2.gun + d)
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
  local real_ft = KL.flight_ticks(KL.sightlen_for(
    math.sqrt((ax - tx) ^ 2 + (ay - ty) ^ 2)))
  local mx, my = lgm.wx + 16 * real_ft, lgm.wy
  local off = math.sqrt((ax - mx) ^ 2 + (ay - my) ^ 2)
  check("the aim point is where he is when a shell to it bursts (<= 64 wu)",
        off <= 64, string.format("lead %.0f wu, flight %d, %.0f wu off", ax - lgm.wx, real_ft, off))
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
