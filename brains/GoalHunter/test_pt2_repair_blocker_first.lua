-- =========================================================================
-- test_pt2_repair_blocker_first.lua -- standalone unit tests (2026-11-24)
-- for C.PT2_REPAIR_BLOCKER_FIRST. Owner: "repairing the damaged blocker
-- first sounds good".
--
-- Before a pill take fires its first shot, a built friendly or ally pill on
-- a blocker tile of the take, with health below the pill maximum, is
-- repaired to full first (quick_repair.lua pre_shoot / pick_first_blocker /
-- first_hold; attack.lua aim holds on first_hold). Layout: the gap_w arena
-- of dt_demo_nov24: target pill A (124,118), neutral, 15 health; the
-- blocker an ally pill at (121,118) with 9 of 15 health (2 trees); the tank
-- at its approach point (115,118), 9 squares from A (out of pill range).
-- The walk is Manhattan x 16 ticks: 6 squares = 96 ticks.
--
--   1. knobs: on by default, off in MAST.
--   2. pre_shoot: approach and aim only, before the firing run, no blitz.
--   3. pick_first_blocker: our take's own tiles only, damaged only.
--   4. evaluate in aim: send with no kill tick; early on a hostile shell;
--      wood, man, death; the old rule (knob off) waits for a kill.
--   5. first_hold: holds on send / early / death / man, skips on wood,
--      a dead builder, a pool reject (an ally's repair claim); a full
--      blocker never holds; done when the repair lands.
--   6. the builder pool sends the builder to the ally blocker in aim.
--   7. source checks: attack.lua aim calls first_hold before the anger
--      gate and the charge.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build-xc/_deps/luajit_src-src/src/luajit.exe test_pt2_repair_blocker_first.lua
-- Against the stripped copies: cd opt && <luajit> ../test_pt2_repair_blocker_first.lua
-- (print2 is stripped there, so the log checks are skipped).
-- =========================================================================

package.path = "./?.lua;" .. package.path

local LOG = {}
package.loaded["print2"] = setmetatable({ set_tick = function() end },
  { __call = function(_, s) LOG[#LOG + 1] = tostring(s) end })
_G.print2 = _G.print2 or function() end

if math.atan2 then
  local _a1, _a2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _a1(y) else return _a2(y, x) end end
end

_G.OBJECT_TANK, _G.OBJECT_SHOT = 0, 1
_G.BUILDMODE_FARM, _G.BUILDMODE_PBOX = 1, 3

local C      = require("constants")
local DEFAULT_ON = C.PT2_REPAIR_BLOCKER_FIRST
local U      = require("util")
local cpf    = require("cpathfinder")
local danger = require("danger")
local threat = require("threat")
local PP     = require("pill_portfolio")
local AS     = require("ally_state")
local BP     = require("builder_pool")
local QR     = require("quick_repair")
local A      = require("attack")

local HAS_LOGS
do
  local f = io.open("quick_repair.lua", "r")
  local src = f and f:read("*a") or ""
  if f then f:close() end
  HAS_LOGS = src:find("print2(", 1, true) ~= nil
end

-- ── Engine stand-ins ─────────────────────────────────────────────────────
U.ttype = function() return C.T_GRASS end
U.ttype_peek = U.ttype
_G.cpf_lgm_travel_ticks_map = function(fx, fy, tx, ty)
  return (math.abs(fx - tx) + math.abs(fy - ty)) * 16
end
local SHELL = {}
danger.update_fire_clock      = function() end
danger.tank_fire_age          = function() return nil end
danger.lgm_shell_gate         = function(_, _, mx, my) return SHELL[my * 256 + mx] end
danger.lgm_certain_death      = function(_, _, mx, my) return SHELL[my * 256 + mx] end
danger.log_certain_death      = function() end
danger.lgm_path_safe_enhanced = function() return true end
threat.at          = function() return 0 end
PP.on_front_line   = function() return false end
AS.init()

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

-- ── Fixture ──────────────────────────────────────────────────────────────
local NOW = 2000
local TX, TY = 124, 118      -- target pill A
local BX, BY = 121, 118      -- the take's blocker (ally pill)
local function world_of(pills)
  local w = { pills = pills, pill_at = {}, bases = {}, base_at = {} }
  for id, p in pairs(pills) do
    local k = p.my * 256 + p.mx
    w.pill_at[k] = w.pill_at[k] or {}
    w.pill_at[k][#w.pill_at[k] + 1] = { pill = p, id = id }
  end
  return w
end
local function mk(o)
  o = o or {}
  local pills = {
    [1] = { mx = TX, my = TY, health = 15, owner = "neutral" },
    [7] = { mx = BX, my = BY, health = o.hp or 9, owner = o.owner or "allied", _in_use = true },
  }
  if o.extra then for k, v in pairs(o.extra) do pills[k] = v end end
  local world = world_of(pills)
  local aim = { blockers = { { kind = "friendly_pill", mx = BX, my = BY } } }
  local state = {
    player_number = 1, tick = NOW,
    builder = { mode = "suppressed" },
    goal = { kind = "attack_pill", mx = TX, my = TY, target_id = 1,
             substate = o.substate or "aim", _aim_locked = true,
             _attack_start_tick = o.started, _blitz = o.blitz,
             _shield_scan = { best = { best_aim_idx = 1, aims = { aim } } } },
  }
  local info = { tankx = 115 * 256 + 128, tanky = 118 * 256 + 128,
                 trees = o.trees or 10, man_status = o.man or C.LGM_INTANK,
                 inboat = o.inboat or false, armour = 40, carried_pills = 0,
                 player_number = 1, reload = false, reload_ticks = 13,
                 objects = o.objects or {} }
  return state, world, info
end
local function reset(on)
  for k in pairs(SHELL) do SHELL[k] = nil end
  for i = #LOG, 1, -1 do LOG[i] = nil end
  C.PT2_REPAIR_BLOCKER_FIRST = on
  C.QUICK_REPAIR_ON_TAKE = true
  C.QUICK_REPAIR_MARGIN_TICKS = 2
end
-- A hostile shell going south down column `mx`, `up` squares above row BY.
local function shell_south(mx, up, life)
  return { type = OBJECT_SHOT, x = mx * 256 + 128, y = (BY - up) * 256 + 128,
           angle = 128, life = life or 40, owner = 255, info = 2 }
end

-- ── 1. knobs ─────────────────────────────────────────────────────────────
print("1. knobs")
check("PT2_REPAIR_BLOCKER_FIRST on by default", DEFAULT_ON == true, DEFAULT_ON)
check("PT2_REPAIR_BLOCKER_FIRST off in MAST",
      C.PRESETS.mast.PT2_REPAIR_BLOCKER_FIRST == false, C.PRESETS.mast.PT2_REPAIR_BLOCKER_FIRST)

-- ── 2. pre_shoot ─────────────────────────────────────────────────────────
print("2. pre_shoot")
do
  reset(true)
  local st = mk()
  check("aim, no shot yet: pre-shoot", QR.pre_shoot(st.goal) == true)
  st = mk({ substate = "approach" })
  check("approach: pre-shoot", QR.pre_shoot(st.goal) == true)
  st = mk({ substate = "build_walls" })
  check("build_walls: not pre-shoot (the builder is on the walls)", QR.pre_shoot(st.goal) == false)
  st = mk({ substate = "in_range_position" })
  check("in_range_position: not pre-shoot", QR.pre_shoot(st.goal) == false)
  st = mk({ substate = "aim", started = NOW - 40 })
  check("aim after the firing run started (re-aim): not pre-shoot", QR.pre_shoot(st.goal) == false)
  st = mk({ blitz = true })
  check("blitz: not pre-shoot", QR.pre_shoot(st.goal) == false)
  st = mk(); st.goal.kind = "capture_pill"
  check("other goal: not pre-shoot", QR.pre_shoot(st.goal) == false)
  reset(false)
  st = mk()
  check("knob off: not pre-shoot", QR.pre_shoot(st.goal) == false)
end

-- ── 3. pick_first_blocker ────────────────────────────────────────────────
print("3. pick_first_blocker")
do
  reset(true)
  -- A damaged _in_use pill an ALLY declared (not on our tiles) at (118,121)
  -- with 3 health: the old pick_blocker would take it; ours does not.
  local st, w, inf = mk({ extra = { [9] = { mx = 118, my = 121, health = 3,
                                            owner = "friendly", _in_use = true } } })
  local p, id = QR.pick_first_blocker(st, w, inf)
  check("the take's own ally blocker (121,118)", id == 7 and p.mx == BX, id)
  local _, oid = QR.pick_blocker(w, inf)
  check("(old pick_blocker takes the lower-health ally-declared pill)", oid == 9, oid)
  st, w, inf = mk({ hp = 15 })
  check("full health: none", QR.pick_first_blocker(st, w, inf) == nil)
  st, w, inf = mk({ owner = "friendly", hp = 6 })
  p, id = QR.pick_first_blocker(st, w, inf)
  check("our own pill, 6 health: picked", id == 7, id)
  st, w, inf = mk({ owner = "hostile", hp = 6 })
  check("an enemy pill on the tile: none", QR.pick_first_blocker(st, w, inf) == nil)
end

-- ── 4. evaluate in aim ───────────────────────────────────────────────────
print("4. evaluate")
do
  reset(true)
  local st, w, inf = mk()
  local qr = QR.evaluate(st, w, inf, NOW)
  check("send before the first shot", qr and qr.result == "send", qr and qr.result)
  check("record marked first", qr and qr.first == true)
  check("no kill tick (pre_shoot)", qr and qr.t_kill == nil and qr.kill.why == "pre_shoot",
        qr and tostring(qr.kill.why))
  check("T_safe = now", qr and qr.t_safe == NOW, qr and qr.t_safe)
  check("walk 6 squares = 96, T_repair = now + 97", qr and qr.walk == 96 and qr.t_repair == NOW + 97,
        qr and tostring(qr.walk))
  check("trees: 9 of 15 needs ceil(6/4) = 2", qr and qr.trees_need == 2, qr and qr.trees_need)
  check("no swerve side committed", st.goal._repair_swerve_dir == nil)

  -- A hostile shell 6 squares up column 121, life 72, from the square
  -- centre: it reaches row 118 after (6*256 - 128) / 32 = 44 ticks.
  -- T_repair now + 97 >= now + 44 + 2: send.
  reset(true)
  st, w, inf = mk({ objects = { shell_south(BX, 6, 72) } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("shell lands before the builder: send", qr and qr.result == "send", qr and qr.result)
  -- Closer walk: the tank at (120,118), 1 square: T_repair = now + 17 < 44 + 2.
  st, w, inf = mk({ objects = { shell_south(BX, 6, 72) } })
  inf.tankx = 120 * 256 + 128
  qr = QR.evaluate(st, w, inf, NOW)
  check("shell lands after the builder: wait:early", qr and qr.result == "wait:early", qr and qr.result)
  check("opens in (now+44+2) - (now+17) = 29", qr and qr.open_in == 29, qr and qr.open_in)

  reset(true)
  st, w, inf = mk({ trees = 1 })
  qr = QR.evaluate(st, w, inf, NOW)
  check("1 tree for 2: wait:wood", qr and qr.result == "wait:wood", qr and qr.result)
  st, w, inf = mk({ man = C.LGM_MOVING })
  qr = QR.evaluate(st, w, inf, NOW)
  check("builder out: wait:man", qr and qr.result == "wait:man", qr and qr.result)
  st, w, inf = mk()
  SHELL[BY * 256 + BX] = { t = 30, src = "t2", how = "tank" }
  qr = QR.evaluate(st, w, inf, NOW)
  check("certain death: wait:death", qr and qr.result == "wait:death", qr and qr.result)

  -- Knob off: the old quick repair. No firing run, so no kill tick: wait.
  reset(false)
  st, w, inf = mk()
  qr = QR.evaluate(st, w, inf, NOW)
  check("knob off: wait:nokill (old rule waits for the kill)",
        qr and qr.result == "wait:nokill" and not qr.first, qr and qr.result)
  -- Knob on, after the first shot (engage): the old rule too.
  reset(true)
  st, w, inf = mk({ substate = "engage", started = NOW - 30 })
  qr = QR.evaluate(st, w, inf, NOW)
  check("engage: the old rule (not first)", qr and not qr.first, qr and tostring(qr.first))
  -- Both quick repair knobs off: nothing.
  reset(false); C.QUICK_REPAIR_ON_TAKE = false
  st, w, inf = mk()
  check("both knobs off: nil", QR.evaluate(st, w, inf, NOW) == nil)
end

-- ── 5. first_hold ────────────────────────────────────────────────────────
print("5. first_hold")
do
  local function hold_after(o, now)
    local st, w, inf = mk(o)
    QR.evaluate(st, w, inf, now or NOW)
    return QR.first_hold(st, w, inf, (now or NOW) + 1, st.goal), st, w, inf
  end
  reset(true)
  local h, st, w, inf = hold_after()
  check("send: hold", h == true)
  check("record kept", st._pt2_brf and st._pt2_brf.mx == BX and st._pt2_brf.id == 7)
  if HAS_LOGS then
    check("log: hold:send line", LOG[#LOG] and LOG[#LOG]:find(
      "PT2_BLOCKER_REPAIR t=2001 pill=#7 (121,118) hp=9/15 -> hold:send", 1, true) ~= nil, LOG[#LOG])
  end
  local n = #LOG
  QR.first_hold(st, w, inf, NOW + 1, st.goal)
  check("same answer: no new line", #LOG == n, #LOG)

  -- The builder walks out (man moving), then the repair lands.
  inf.man_status = C.LGM_MOVING
  st._bp_job = { type = "topup", mx = BX, my = BY, quick = true }
  QR.evaluate(st, w, inf, NOW + 2)
  check("builder out on it: hold", QR.first_hold(st, w, inf, NOW + 3, st.goal) == true)
  if HAS_LOGS then
    check("log: hold:man line", LOG[#LOG]:find("-> hold:man", 1, true) ~= nil, LOG[#LOG])
  end
  w.pills[7].health = 15
  QR.evaluate(st, w, inf, NOW + 98)
  check("repaired to full: no hold", QR.first_hold(st, w, inf, NOW + 99, st.goal) == false)
  check("record cleared", st._pt2_brf == nil)
  if HAS_LOGS then
    check("log: done line", LOG[#LOG]:find("hp=15/15 -> done (held 98t", 1, true) ~= nil, LOG[#LOG])
  end

  -- dt_demo d1 beside_n, Nov 24: at aim (t=315) the builder went out on a
  -- wall_shield BUILD at (122,116), not on the blocker's repair. wait:man
  -- held the aim to t=482. Out on another trip: no hold.
  reset(true)
  st, w, inf = mk({ man = C.LGM_MOVING })
  QR.evaluate(st, w, inf, NOW)
  check("out, no pool job (wall trip): no hold",
        QR.first_hold(st, w, inf, NOW + 1, st.goal) == false)
  if HAS_LOGS then
    check("log: skip:man_busy", LOG[#LOG]:find("-> skip:man_busy", 1, true) ~= nil, LOG[#LOG])
  end
  reset(true)
  st, w, inf = mk({ man = C.LGM_MOVING })
  st._bp_job = { type = "topup", mx = 122, my = 116 }
  QR.evaluate(st, w, inf, NOW)
  check("out on a job at another square: no hold",
        QR.first_hold(st, w, inf, NOW + 1, st.goal) == false)

  reset(true)
  h = hold_after({ trees = 1 })
  check("no wood: no hold (take goes on)", h == false)
  if HAS_LOGS then
    check("log: skip:wood", LOG[#LOG] and LOG[#LOG]:find("-> skip:wood", 1, true) ~= nil, LOG[#LOG])
  end
  reset(true)
  h = hold_after({ man = C.LGM_DEAD })
  check("dead builder: no hold", h == false)
  reset(true)
  h = hold_after({ man = C.LGM_MOVING, inboat = true })
  check("in a boat: no hold", h == false)
  reset(true)
  SHELL[BY * 256 + BX] = { t = 30, src = "t2", how = "tank" }
  h = hold_after()
  check("certain death (a shell in the air): hold", h == true)
  reset(true)
  h = hold_after({ hp = 15 })
  check("full blocker: no hold, no line", h == false and #LOG == 0, #LOG)

  -- An ally claims the repair: the pool refuses our quick row; the take goes on.
  reset(true)
  st, w, inf = mk()
  QR.evaluate(st, w, inf, NOW)
  st._builder_pool = { tick = NOW, rows = { { quick = true, mx = BX, my = BY,
                       reject = "ally_repairing (p2)" } } }
  check("ally repair claim: no hold", QR.first_hold(st, w, inf, NOW + 1, st.goal) == false)
  if HAS_LOGS then
    check("log: skip:pool(ally_repairing ...)",
          LOG[#LOG]:find("-> skip:pool(ally_repairing (p2))", 1, true) ~= nil, LOG[#LOG])
  end
  -- A shell refusal of the row is "not this tick": still hold.
  st._builder_pool.rows[1].shell_hit = { src = "t2", t = 5 }
  st._builder_pool.rows[1].reject = "shell_will_hit (shell from t2 at +5t, tank)"
  check("shell refusal: hold", QR.first_hold(st, w, inf, NOW + 1, st.goal) == true)

  -- After the first shot the hold is off and the record cleared.
  reset(true)
  st, w, inf = mk({ substate = "engage", started = NOW - 30 })
  st._pt2_brf = { mx = BX, my = BY, id = 7, t0 = NOW - 50 }
  check("engage: no hold", QR.first_hold(st, w, inf, NOW, st.goal) == false and st._pt2_brf == nil)
  reset(false)
  h = hold_after()
  check("knob off: no hold", h == false)
end

-- ── 6. the builder pool ──────────────────────────────────────────────────
print("6. builder pool")
do
  reset(true)
  local st, w, inf = mk()
  local bp = BP.update(st, w, inf, NOW)
  local cmd = BP.rung(st, w, inf, NOW)
  local r
  for _, row in ipairs(bp and bp.rows or {}) do
    if row.mx == BX and row.my == BY then r = row end
  end
  check("the ally blocker row is quick", r and r.quick == true, r and tostring(r.reject))
  check("the row is not refused", r and not r.reject, r and tostring(r.reject))
  check("builder sent to the blocker", cmd and cmd.x == BX and cmd.y == BY,
        cmd and string.format("%d,%d", cmd.x, cmd.y))
  check("the job is quick", st._bp_job and st._bp_job.quick == true)
  -- Knob off: the blocker is refused while alive (take_blocker), as before.
  reset(false)
  st, w, inf = mk()
  bp = BP.update(st, w, inf, NOW)
  cmd = BP.rung(st, w, inf, NOW)
  check("knob off: no send", not (cmd and cmd.x == BX and cmd.y == BY),
        cmd and string.format("%d,%d", cmd.x, cmd.y))
end

-- ── 7. source checks ─────────────────────────────────────────────────────
print("7. source")
do
  local f = io.open("attack.lua", "r")
  local src = f and f:read("*a") or ""
  if f then f:close() end
  local a = src:find('if goal.substate == "aim" then', 1, true)
  local h = a and src:find('require("quick_repair").first_hold(state, world, info, now, goal)', a, true)
  local g = a and src:find("Anger cool-down gate", a, true)
  local c = a and src:find('goal.substate = "charge"', a, true)
  check("aim calls first_hold", h ~= nil)
  check("before the anger gate and the charge", h and g and c and h < g and h < c)
end

-- ── 8. Nov 24 arena fixes (source checks) ────────────────────────────────
print("8. arena fixes")
do
  local function read(fn)
    local f = io.open(fn, "r"); local t = f and f:read("*a") or ""
    if f then f:close() end
    return t
  end
  -- goals.lua: the reposition read path skips a pill our take marked.
  local g = read("goals.lua")
  local r = g:find("Read path (every replan)", 1, true)
  local m = r and g:find("and not lp._in_use and not lp._take_pend", r, true)
  check("reposition read path skips _in_use / _take_pend", m ~= nil)
  -- init.lua: our approval on a blocker of our own take is dropped.
  local i = read("init.lua")
  check("own-blocker approval drop",
        i:find("dropped (blocker of our own take plan)", 1, true) ~= nil)
  -- pt2_attack.lua: bg_trip_ok judges the guns at the tank now.
  local p = read("pt2_attack.lua")
  local b = p:find("function M.bg_trip_ok", 1, true)
  local e = b and p:find("\nend", b, true)
  local t = b and p:find("tx = (info.tankx or 0) / 256.0", b, true)
  check("bg_trip_ok passes the tank point", t and e and t < e)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
