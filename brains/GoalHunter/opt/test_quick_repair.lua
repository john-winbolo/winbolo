-- =========================================================================
-- test_quick_repair.lua — standalone unit tests (2026-11-04) for QUICK
-- REPAIR of a take blocker (quick_repair.lua; knobs C.QUICK_REPAIR_ON_TAKE,
-- C.QUICK_REPAIR_PREDICT_SHOTS, C.QUICK_REPAIR_MARGIN_TICKS):
--   * the timing rule: wait while T_repair < T_safe + margin (early), send
--     when the window is open, wait on certain death, wait until a hostile
--     shell that lands on the blocker after T_kill has landed;
--   * T_kill from our shells in flight (the hp-th landing) and from the
--     predicted shots (one reload apart plus the flight), only in the firing
--     run and only with the predict knob on;
--   * the builder pool: with the knob on and "send" the blocker's row is
--     dispatched; with the knob off the blocker is refused (take_blocker)
--     like before;
--   * the KEEL preset turns both behaviour knobs off.
-- No engine: terrain, the walk sim, the shell death check and the reload
-- source are stubs or plain fields.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_quick_repair.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_quick_repair.lua
-- (the print2 lines are stripped there, so the log checks are skipped).
-- =========================================================================

package.path = "./?.lua;" .. package.path

local LOG = {}
package.loaded["print2"] = setmetatable({ set_tick = function() end },
  { __call = function(_, s) LOG[#LOG + 1] = tostring(s) end })

if not math.atan2 then math.atan2 = function(y, x) return math.atan(y, x) end end

-- brain.h values.
_G.OBJECT_TANK, _G.OBJECT_SHOT = 0, 1
_G.BUILDMODE_FARM, _G.BUILDMODE_PBOX = 1, 3

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local threat = require("threat")
local PP     = require("pill_portfolio")
local AS     = require("ally_state")
local BP     = require("builder_pool")
local QR     = require("quick_repair")

local HAS_LOGS
do
  local f = io.open("quick_repair.lua", "r")
  local src = f and f:read("*a") or ""
  if f then f:close() end
  HAS_LOGS = src:find("print2(", 1, true) ~= nil
end

-- ── Engine stand-ins ─────────────────────────────────────────────────────
U.ttype = function() return C.T_GRASS end
_G.cpf_lgm_travel_ticks_map = function(fx, fy, tx, ty)
  return (math.abs(fx - tx) + math.abs(fy - ty)) * 16
end
local SHELL = {}                        -- [my*256+mx] = certain-death hit
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
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end

-- ── Fixture ──────────────────────────────────────────────────────────────
-- Tank at (100,100). Target: the enemy pill at (110,100). Blocker: our pill
-- at (BX,100), _in_use. The walk is Manhattan x 16 ticks.
local NOW = 1000
local function pill(mx, my, hp, owner, in_use)
  return { mx = mx, my = my, health = hp, owner = owner or "friendly",
           _in_use = in_use or nil }
end
local function world_of(pills)
  local w = { pills = pills, pill_at = {}, bases = {}, base_at = {} }
  for id, p in pairs(pills) do
    w.pill_at[p.my * 256 + p.mx] = { { pill = p, id = id } }
  end
  return w
end
local function mk(opts)
  opts = opts or {}
  local bx = opts.bx or 104
  local pills = {
    [3] = pill(110, 100, opts.target_hp or 2, "hostile"),
    [8] = pill(bx, 100, opts.blocker_hp or 0, "friendly", true),
  }
  if opts.extra then for k, v in pairs(opts.extra) do pills[k] = v end end
  local world = world_of(pills)
  local state = {
    player_number = 1, tick = NOW,
    builder = { mode = "suppressed" },
    goal = { kind = "attack_pill", mx = 110, my = 100, target_id = 3,
             substate = opts.substate or "engage",
             _attack_start_tick = NOW - 50,
             _on_target_land = opts.land,
             _on_target_land_tick = opts.land_tick or NOW },
  }
  local info = { tankx = 100 * 256 + 128, tanky = 100 * 256 + 128,
                 trees = opts.trees or 10, man_status = C.LGM_INTANK,
                 inboat = false, armour = 40, carried_pills = 0,
                 player_number = 1, reload = opts.reloading or false,
                 reload_ticks = opts.reload_ticks or 13,
                 objects = opts.objects or {} }
  return state, world, info
end
local function reset(on, predict)
  for k in pairs(SHELL) do SHELL[k] = nil end
  for i = #LOG, 1, -1 do LOG[i] = nil end
  C.QUICK_REPAIR_ON_TAKE = on
  C.QUICK_REPAIR_PREDICT_SHOTS = predict
  C.QUICK_REPAIR_MARGIN_TICKS = 2
end
-- A hostile shell going south down column `mx`, `up` tiles above row 100.
local function shell_south(mx, up, life, inf)
  return { type = OBJECT_SHOT, x = mx * 256 + 128, y = (100 - up) * 256 + 128,
           angle = 128, life = life or 40, owner = 255, info = inf or 2 }
end
local function run_pool(state, world, info)
  state.tick = NOW
  local bp = BP.update(state, world, info, NOW)
  local cmd = BP.rung(state, world, info, NOW)
  return bp, cmd
end
local function row_at(bp, mx, my)
  for _, r in ipairs(bp and bp.rows or {}) do
    if r.mx == mx and r.my == my then return r end
  end
  return nil
end

-- ── 1. Helpers ───────────────────────────────────────────────────────────
print("1. helpers")
do
  check("ticks_to_tile: on the tile now -> 0",
        QR.ticks_to_tile(100 * 256 + 10, 100 * 256 + 10, 32, 0, 100, 100, 72) == 0)
  local k = QR.ticks_to_tile(100 * 256 + 128, 100 * 256 + 128, 32, 0, 102, 100, 72)
  check("ticks_to_tile: two tiles east at 32/tick -> 12", k == 12, k)
  check("ticks_to_tile: never there -> nil",
        QR.ticks_to_tile(100 * 256, 100 * 256, 0, 32, 120, 100, 72) == nil)
  local s = { cur_fx = 0, cur_fy = 0, matched = true,
              closest_fx = 108 * 256 + 128, closest_fy = 100 * 256 + 128,
              step_x = 32, step_y = 0 }
  local t = QR.shot_land_tick(s, 110, 100, NOW)
  check("shot_land_tick: starts from the matched engine shell", t == NOW + 12, t)
  check("flight_ticks: 10 tiles -> ceil((2560-128-160)/32) = 71",
        QR.flight_ticks({ tankx = 100 * 256 + 128, tanky = 100 * 256 + 128 }, 110, 100) == 71)
  check("trees_need: dead -> rebuild trees",
        QR.trees_need({ health = 0 }) == (C.BUILDER_POOL_TREES_REBUILD or 4))
  check("trees_need: hp 9 of 15 -> 2",
        QR.trees_need({ health = 9 }) == 2, QR.trees_need({ health = 9 }))
  check("reload_ticks: the engine's figure",
        QR.reload_ticks({ reload_ticks = 9 }) == 9, QR.reload_ticks({ reload_ticks = 9 }))
  local w = world_of({ [1] = pill(102, 100, 9, "friendly", true),
                       [2] = pill(106, 100, 3, "friendly", true),
                       [4] = pill(101, 100, 3, "friendly", nil),
                       [5] = pill(99, 100, 15, "friendly", true) })
  local p, id = QR.pick_blocker(w, { tankx = 100 * 256 + 128, tanky = 100 * 256 + 128 })
  check("pick_blocker: lowest hp _in_use pill", id == 2 and p.mx == 106, id)
end

-- ── 2. The timing rule ───────────────────────────────────────────────────
print("2. timing rule")
do
  -- Early: hp 2, shells land at +70 and +80: T_kill +80. Walk 4 tiles = 64,
  -- T_repair = +65 < +82.
  reset(true, true)
  local st, w, inf = mk({ land = { NOW + 80, NOW + 70 } })
  local qr = QR.evaluate(st, w, inf, NOW)
  check("early: wait:early", qr and qr.result == "wait:early", qr and qr.result)
  check("early: T_kill is the 2nd landing (+80)", qr and qr.t_kill == NOW + 80, qr and qr.t_kill)
  check("early: T_repair = now + 1 + 64", qr and qr.t_repair == NOW + 65, qr and qr.t_repair)
  check("early: opens in 82 - 65 = 17", qr and qr.open_in == 17, qr and qr.open_in)
  check("early: record kept in state._quick_repair", st._quick_repair == qr)

  -- Window open: shells land at +10 and +20: T_kill +20, +65 >= +22.
  reset(true, true)
  st, w, inf = mk({ land = { NOW + 10, NOW + 20 } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("open: send", qr and qr.result == "send", qr and qr.result)
  check("open: src inflight", qr and qr.kill_src == "inflight", qr and qr.kill_src)

  -- Exactly on the margin: T_repair == T_safe + margin sends.
  reset(true, true)
  st, w, inf = mk({ land = { NOW + 10, NOW + 63 } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("margin: T_repair == T_safe + 2 -> send", qr and qr.result == "send", qr and qr.result)
  reset(true, true)
  st, w, inf = mk({ land = { NOW + 10, NOW + 64 } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("margin: one tick short -> wait:early", qr and qr.result == "wait:early", qr and qr.result)

  -- Certain death on the errand.
  reset(true, true)
  st, w, inf = mk({ land = { NOW + 10, NOW + 20 } })
  SHELL[100 * 256 + 104] = { t = 30, src = "p3", how = "tank" }
  qr = QR.evaluate(st, w, inf, NOW)
  check("death: wait:death", qr and qr.result == "wait:death", qr and qr.result)
  check("death: the hit is kept", qr and qr.death and qr.death.src == "p3")

  -- Not enough wood for a rebuild.
  reset(true, true)
  st, w, inf = mk({ land = { NOW + 10, NOW + 20 }, trees = 1 })
  qr = QR.evaluate(st, w, inf, NOW)
  check("wood: wait:wood", qr and qr.result == "wait:wood", qr and qr.result)

  -- Builder already out.
  reset(true, true)
  st, w, inf = mk({ land = { NOW + 10, NOW + 20 } })
  inf.man_status = C.LGM_INTANK + 1
  qr = QR.evaluate(st, w, inf, NOW)
  check("man out: wait:man", qr and qr.result == "wait:man", qr and qr.result)

  -- A stale landing list (not from this tick) is not used.
  reset(true, false)
  st, w, inf = mk({ land = { NOW + 10, NOW + 20 }, land_tick = NOW - 1 })
  qr = QR.evaluate(st, w, inf, NOW)
  check("stale landing list: wait:nokill", qr and qr.result == "wait:nokill", qr and qr.result)

  -- No damaged blocker: nothing to think about.
  reset(true, true)
  st, w, inf = mk({ land = { NOW + 10, NOW + 20 }, blocker_hp = 15 })
  check("full blocker: nil", QR.evaluate(st, w, inf, NOW) == nil)

  -- Not an attack_pill take: nil.
  reset(true, true)
  st, w, inf = mk({ land = { NOW + 10, NOW + 20 } })
  st.goal.kind = "defend_pill"
  check("other goal: nil", QR.evaluate(st, w, inf, NOW) == nil)
end

-- ── 3. A hostile shell that lands on the blocker after T_kill ────────────
print("3. hostile shell after T_kill")
do
  -- Blocker next door at (101,100): walk 16, T_repair +17. T_kill +5. A
  -- neutral pill shell comes south down column 101 from 4 tiles up: it
  -- reaches row 100 after (25600 - 24704) / 32 = 28 ticks. T_safe +28.
  reset(true, true)
  local st, w, inf = mk({ bx = 101, land = { NOW + 3, NOW + 5 },
                          objects = { shell_south(101, 4, 40) } })
  local qr = QR.evaluate(st, w, inf, NOW)
  check("hostile: one hit on the blocker", qr and #qr.hits == 1, qr and #qr.hits)
  check("hostile: lands at +28", qr and qr.hits[1] and qr.hits[1].t == 28,
        qr and qr.hits[1] and qr.hits[1].t)
  check("hostile: T_safe = the hit, not T_kill", qr and qr.t_safe == NOW + 28, qr and qr.t_safe)
  check("hostile: wait:early", qr and qr.result == "wait:early", qr and qr.result)
  check("hostile: opens in 30 - 17 = 13", qr and qr.open_in == 13, qr and qr.open_in)

  -- Its life ends before the blocker: no hit, send.
  reset(true, true)
  st, w, inf = mk({ bx = 101, land = { NOW + 3, NOW + 5 },
                    objects = { shell_south(101, 4, 20) } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("short life: no hit, send", qr and #qr.hits == 0 and qr.result == "send",
        qr and (#qr.hits .. "/" .. qr.result))

  -- Our own shell (info 0) is not counted.
  reset(true, true)
  st, w, inf = mk({ bx = 101, land = { NOW + 3, NOW + 5 },
                    objects = { shell_south(101, 4, 40, 0) } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("friendly shell: ignored, send", qr and #qr.hits == 0 and qr.result == "send",
        qr and qr.result)

  -- Another live pill in its way stops it.
  reset(true, true)
  st, w, inf = mk({ bx = 101, land = { NOW + 3, NOW + 5 },
                    objects = { shell_south(101, 4, 40) },
                    extra = { [9] = pill(101, 98, 15, "friendly") } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("stopped by a live pill: send", qr and #qr.hits == 0 and qr.result == "send",
        qr and qr.result)

  -- The shell has landed (gone from the list): the window opens.
  reset(true, true)
  st, w, inf = mk({ bx = 101, land = { NOW + 3, NOW + 5 } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("after it lands: send", qr and qr.result == "send", qr and qr.result)
end

-- ── 4. Predicted shots ───────────────────────────────────────────────────
print("4. predicted shots")
do
  -- hp 3, one in flight at +4: two to fire. Gun ready: first at +1, the
  -- next at +14 (reload 13), flight 71: T_kill = +85.
  reset(true, true)
  local st, w, inf = mk({ target_hp = 3, land = { NOW + 4 } })
  local qr = QR.evaluate(st, w, inf, NOW)
  local k = qr and qr.kill
  check("predict: src predict", qr and qr.kill_src == "predict", qr and qr.kill_src)
  check("predict: first +1, reload 13, 2 to fire, flight 71",
        k and k.first == NOW + 1 and k.reload == 13 and k.remaining == 2 and k.flight == 71,
        k and string.format("%s/%s/%s/%s", tostring(k.first), tostring(k.reload),
                            tostring(k.remaining), tostring(k.flight)))
  check("predict: T_kill = +14 + 71 = +85", qr and qr.t_kill == NOW + 85, qr and qr.t_kill)
  check("predict: wait:early (+65 < +87)", qr and qr.result == "wait:early", qr and qr.result)

  -- Gun reloading: the first shot waits one full reload.
  reset(true, true)
  st, w, inf = mk({ target_hp = 3, land = { NOW + 4 }, reloading = true })
  qr = QR.evaluate(st, w, inf, NOW)
  check("predict reloading: T_kill = +13 + 13 + 71 = +97",
        qr and qr.t_kill == NOW + 97, qr and qr.t_kill)

  -- The reload figure comes from the engine (info.reload_ticks).
  reset(true, true)
  st, w, inf = mk({ target_hp = 3, land = { NOW + 4 }, reload_ticks = 6 })
  qr = QR.evaluate(st, w, inf, NOW)
  check("predict reload 6: T_kill = +7 + 71 = +78", qr and qr.t_kill == NOW + 78, qr and qr.t_kill)

  -- Close target, few shots: the predicted kill opens the window.
  reset(true, true)
  st, w, inf = mk({ target_hp = 1, land = {} })
  st.goal.mx, st.goal.my = 103, 100
  w = world_of({ [3] = pill(103, 100, 1, "hostile"),
                 [8] = pill(100, 104, 0, "friendly", true) })
  qr = QR.evaluate(st, w, inf, NOW)
  check("predict near: T_kill = +1 + flight 15", qr and qr.t_kill == NOW + 16, qr and qr.t_kill)
  check("predict near: send", qr and qr.result == "send", qr and qr.result)

  -- Predict knob off: in-flight shells short of hp -> nokill.
  reset(true, false)
  st, w, inf = mk({ target_hp = 3, land = { NOW + 4 } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("predict off: wait:nokill (inflight_short)",
        qr and qr.result == "wait:nokill" and qr.kill.why == "inflight_short",
        qr and (qr.result .. "/" .. tostring(qr.kill.why)))

  -- Not in the firing run: no prediction.
  reset(true, true)
  st, w, inf = mk({ target_hp = 3, land = { NOW + 4 }, substate = "approach" })
  qr = QR.evaluate(st, w, inf, NOW)
  check("not firing: wait:nokill (not_firing)",
        qr and qr.result == "wait:nokill" and qr.kill.why == "not_firing",
        qr and (qr.result .. "/" .. tostring(qr.kill.why)))

  -- In flight already enough: no prediction needed.
  reset(true, true)
  st, w, inf = mk({ target_hp = 1, land = { NOW + 9 } })
  qr = QR.evaluate(st, w, inf, NOW)
  check("enough in flight: src inflight", qr and qr.kill_src == "inflight" and qr.t_kill == NOW + 9,
        qr and qr.kill_src)
end

-- ── 5. The builder pool ──────────────────────────────────────────────────
print("5. builder pool")
do
  -- Alive blocker (hp 5) in its window: the pool sends the builder to it.
  reset(true, true)
  local st, w, inf = mk({ blocker_hp = 5, land = { NOW + 10, NOW + 20 } })
  local bp, cmd = run_pool(st, w, inf)
  local r = row_at(bp, 104, 100)
  check("pool on: the blocker row is quick", r and r.quick == true, r and tostring(r.reject))
  check("pool on: the row is not refused", r and not r.reject, r and tostring(r.reject))
  check("pool on: builder sent to the blocker", cmd and cmd.x == 104 and cmd.y == 100,
        cmd and string.format("%d,%d", cmd.x, cmd.y))
  check("pool on: the job is marked quick", st._bp_job and st._bp_job.quick == true)

  -- Same tick, but early: the blocker is refused as before.
  reset(true, true)
  st, w, inf = mk({ blocker_hp = 5, land = { NOW + 70, NOW + 80 } })
  bp, cmd = run_pool(st, w, inf)
  r = row_at(bp, 104, 100)
  check("pool early: no send to the blocker", not (cmd and cmd.x == 104 and cmd.y == 100),
        cmd and string.format("%d,%d", cmd.x, cmd.y))
  check("pool early: row refused take_blocker", r == nil or tostring(r.reject):find("take_blocker", 1, true) ~= nil,
        r and tostring(r.reject))

  -- Certain death on the open window: no send.
  reset(true, true)
  st, w, inf = mk({ blocker_hp = 5, land = { NOW + 10, NOW + 20 } })
  SHELL[100 * 256 + 104] = { t = 30, src = "p3", how = "tank" }
  bp, cmd = run_pool(st, w, inf)
  check("pool death: no send to the blocker", not (cmd and cmd.x == 104 and cmd.y == 100),
        cmd and string.format("%d,%d", cmd.x, cmd.y))

  -- Knob off: the old refusal.
  reset(false, true)
  st, w, inf = mk({ blocker_hp = 5, land = { NOW + 10, NOW + 20 } })
  bp, cmd = run_pool(st, w, inf)
  r = row_at(bp, 104, 100)
  check("knob off: no quick record", st._quick_repair == nil)
  check("knob off: no send to the blocker", not (cmd and cmd.x == 104 and cmd.y == 100),
        cmd and string.format("%d,%d", cmd.x, cmd.y))
  check("knob off: row refused take_blocker", r == nil or tostring(r.reject):find("take_blocker", 1, true) ~= nil,
        r and tostring(r.reject))
  check("knob off: row not quick", r == nil or not r.quick)
end

-- ── 6. The log line ──────────────────────────────────────────────────────
if HAS_LOGS then
  print("6. log")
  _G.BRAIN_DEBUG_MODE = true
  reset(true, true)
  local st, w, inf = mk({ land = { NOW + 80, NOW + 70 } })
  QR.evaluate(st, w, inf, NOW)
  local n1 = #LOG
  local line = LOG[1] or ""
  check("log: one QUICK_REPAIR line", n1 == 1 and line:find("^QUICK_REPAIR t=1000 pill=%(104,100%)") ~= nil,
        line)
  check("log: names T_kill, T_safe and the result",
        line:find("T_kill=1080 src=inflight T_safe=1080 walk=64 dwell=0 -> wait:early", 1, true) ~= nil,
        line)
  QR.evaluate(st, w, inf, NOW)
  check("log: same result, no new line", #LOG == n1, #LOG)
  st.goal._on_target_land = { NOW + 10, NOW + 20 }
  QR.evaluate(st, w, inf, NOW)
  check("log: result changes, new line", #LOG == n1 + 1 and LOG[#LOG]:find("-> send", 1, true) ~= nil,
        LOG[#LOG])
  _G.BRAIN_DEBUG_MODE = nil
end

-- ── 7. KEEL preset ───────────────────────────────────────────────────────
print("7. KEEL preset")
do
  local K = C.PRESETS.keel
  check("keel: QUICK_REPAIR_ON_TAKE = false", K.QUICK_REPAIR_ON_TAKE == false)
  check("keel: QUICK_REPAIR_PREDICT_SHOTS = false", K.QUICK_REPAIR_PREDICT_SHOTS == false)
  check("keel: margin not changed", K.QUICK_REPAIR_MARGIN_TICKS == nil)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
