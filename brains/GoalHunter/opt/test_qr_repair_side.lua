-- =========================================================================
-- test_qr_repair_side.lua — standalone unit tests (2026-11-24) for
-- C.SWERVE_AVOID_REPAIR_SIDE (Rule 1) and C.QUICK_REPAIR_WAIT_ALL_PILLS
-- (Rule 2), quick_repair.lua "The repair-side rules":
--   1. side choice: the blocker on the left fan -> right chosen, over the
--      cover pick; right blocked by deep water -> the old (cover) pick;
--      knob off -> the cover pick.
--   2. committed side: the side committed at the send is used at swerve
--      start even when the terrain there would block it; the lock ends with
--      the job.
--   3. send: a clean side -> send and commit side R; a second live pill
--      (not the take target) whose shots cross the blocker on both sides ->
--      no clean side -> wait:other_pill (no send) when a shot meets the
--      blocker inside the stay window (arrival .. build end), send when the
--      shots cross only while he walks; knobs off -> send.
--   4. outbound: the builder is out and the second pill's shots meet him ->
--      QUICK_REPAIR_ABORT why=pill_fire_predict; no second pill -> none.
--   5. presets: KEEL and MAST false, defaults true; margin 0.0.
--   6. the builder walk: a shell that only flies over him is no conflict;
--      since the stay window a shell that explodes on the tank while he is
--      still beside it is no conflict here either (lgm_certain_death and
--      tank_clear test his walk).
--   7. send then outbound: a send at t, the same world at t+1 with the
--      builder just out -> no QUICK_REPAIR_ABORT (not swerving, and with
--      the running swerve on the clean side); a running swerve on the dirty
--      side whose shot crosses the blocker while he walks -> send, nothing
--      committed.
-- No engine: terrain and the walk sim are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   <luajit> test_qr_repair_side.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_qr_repair_side.lua
-- (the print2 lines are stripped there, so the log checks are skipped).
-- =========================================================================

package.path = "./?.lua;" .. package.path

local LOG = {}
package.loaded["print2"] = setmetatable({ set_tick = function() end },
  { __call = function(_, s) LOG[#LOG + 1] = tostring(s) end })

if not math.atan2 then math.atan2 = function(y, x) return math.atan(y, x) end end

_G.OBJECT_TANK, _G.OBJECT_SHOT = 0, 1
_G.BUILDMODE_FARM, _G.BUILDMODE_PBOX = 1, 3

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local danger = require("danger")
local threat = require("threat")
local PP     = require("pill_portfolio")
local AS     = require("ally_state")
local QR     = require("quick_repair")
local A      = require("attack")

local DEF_SIDE, DEF_WAIT = C.SWERVE_AVOID_REPAIR_SIDE, C.QUICK_REPAIR_WAIT_ALL_PILLS

local HAS_LOGS
do
  local f = io.open("quick_repair.lua", "r")
  local src = f and f:read("*a") or ""
  if f then f:close() end
  HAS_LOGS = src:find("print2(", 1, true) ~= nil
end

-- Terrain: grass, or deep sea on rows above DEEP_ABOVE (north) when set.
local DEEP_ABOVE
U.ttype = function(mx, my)
  if DEEP_ABOVE and my < DEEP_ABOVE then return C.T_DEEPSEA end
  return C.T_GRASS
end
U.ttype_peek = U.ttype
cpf.SHOT_TANK = cpf.SHOT_TANK or 0
-- The walk stub: WALK_PER_TILE ticks a tile, Manhattan.
local WALK_PER_TILE = 16
_G.cpf_lgm_travel_ticks_map = function(fx, fy, tx, ty)
  return (math.abs(fx - tx) + math.abs(fy - ty)) * WALK_PER_TILE
end
danger.update_fire_clock      = function() end
danger.tank_fire_age          = function() return nil end
danger.lgm_shell_gate         = function() return nil end
danger.lgm_certain_death      = function() return nil end
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
local function M_fire(qr)
  local f = qr and qr.fire
  return f and string.format(" #%s %s@%d", tostring(f.pill), f.how, f.at) or ""
end
local function logged(pat)
  for i = 1, #LOG do if LOG[i]:find(pat) then return LOG[i] end end
  return nil
end

-- ── Fixture ──────────────────────────────────────────────────────────────
-- Tank (player 1) at (100,100). Take target: the neutral pill at (106,100),
-- hp 2, 6 tiles east; our shells land on it at NOW+10 and NOW+20 (T_kill =
-- NOW+20). Side L (dir 1, attack.lua's LEFT sample) is south (+y), side R
-- is north.
--   blocker "south": our dead blocker at (101,102), south of the
--     target's line: only the target's L fan crosses it.
--   blocker "a": our dead blocker at (102,102), right on the line from a
--     second hostile pill A at (105,104) to the tank: A's shots cross it
--     whichever way the tank swerves.
local NOW = 1000
local function pill(mx, my, hp, owner, in_use)
  return { mx = mx, my = my, health = hp, owner = owner or "friendly",
           _in_use = in_use or nil, anger = 0, anger_tick = 0 }
end
local function mk(opts)
  opts = opts or {}
  local bmx, bmy = 101, 102
  if opts.blocker == "a" then bmx, bmy = 102, 102 end
  if opts.bmx then bmx, bmy = opts.bmx, opts.bmy end
  local pills = {
    [3] = pill(106, 100, 2, "neutral"),
    [8] = pill(bmx, bmy, 0, "friendly", true),
  }
  if opts.pill_a then pills[5] = pill(105, 104, 15, "hostile") end
  local world = { pills = pills, pill_at = {}, bases = {}, base_at = {} }
  for id, p in pairs(pills) do
    world.pill_at[p.my * 256 + p.mx] = { { pill = p, id = id } }
  end
  local state = {
    player_number = 1, tick = NOW,
    builder = { mode = "suppressed" },
    goal = { kind = "attack_pill", mx = 106, my = 100, target_id = 3,
             substate = opts.substate or "engage", _attack_start_tick = NOW - 50,
             _on_target_land = { NOW + 10, NOW + 20 },
             _on_target_land_tick = NOW },
  }
  local info = { tankx = 100 * 256 + 128, tanky = 100 * 256 + 128,
                 trees = 10, man_status = C.LGM_INTANK, inboat = false,
                 armour = 40, carried_pills = 0, player_number = 1,
                 shells = 40, direction = 64, speed = 0,
                 reload = false, reload_ticks = 13, objects = {} }
  return state, world, info
end
local function reset()
  C.QUICK_REPAIR_ON_TAKE = true
  C.QUICK_REPAIR_PREDICT_SHOTS = true
  C.QUICK_REPAIR_MARGIN_TICKS = 2
  C.QUICK_REPAIR_SKIP_OWN_FIRE = true
  C.QUICK_REPAIR_TANK_CLEAR_TICKS = 16
  C.SWERVE_AVOID_REPAIR_SIDE = true
  C.QUICK_REPAIR_WAIT_ALL_PILLS = true
  DEEP_ABOVE = nil
  WALK_PER_TILE = 16
  for i = #LOG, 1, -1 do LOG[i] = nil end
end
-- enter_swerve with the cover pick cached as `cover` (1 = L).
local function swerve(state, world, info, cover)
  local goal = state.goal
  goal._best_swerve_dir = cover
  goal.substate = "charge"
  _G.BRAIN_DEBUG_MODE = true
  A.enter_swerve(goal, world, state, info, 106, 100, "defensive")
  _G.BRAIN_DEBUG_MODE = nil
  return goal._swerve_dir
end

-- ── 1. side choice ───────────────────────────────────────────────────────
print("1. side choice")
do
  reset()
  local st, w, inf = mk()
  local rb = QR.repair_blocker(st, w, inf)
  check("the dead blocker is the repair blocker", rb and rb.mx == 101 and rb.my == 102,
        rb and (rb.mx .. "," .. rb.my))
  local rec = QR.side_eval(w, inf, rb, 106, 100, nil)
  check("L dirty, R clean", rec.side[1].dirty and not rec.side[-1].dirty,
        tostring(rec.side[1].dirty) .. "/" .. tostring(rec.side[-1].dirty))
  check("fan about 18 degrees", rec.side[1].deg > 15 and rec.side[1].deg < 22, rec.side[1].deg)
  check("R chosen (clean_side)", rec.dir == -1 and rec.why == "clean_side", rec.why)

  local d = swerve(st, w, inf, 1)
  check("enter_swerve: cover pick L, blocker on L -> R", d == -1, d)
  check("SWERVE_REPAIR_SIDE logged chosen=R why=clean_side", not HAS_LOGS
        or (logged("SWERVE_REPAIR_SIDE") or ""):find("left=dirty right=clean chosen=R why=clean_side"),
        logged("SWERVE_REPAIR_SIDE"))

  reset()
  DEEP_ABOVE = 100
  st, w, inf = mk()
  local sc = QR.side_scores(w, 106, 100, 100, 100, false)
  check("scorer: deep water north -> right_hazard, left none",
        sc.right_hazard == true and sc.left_hazard == false,
        tostring(sc.right_hazard) .. "/" .. tostring(sc.left_hazard))
  d = swerve(st, w, inf, 1)
  check("right blocked by deep water -> old cover pick L", d == 1, d)
  check("logged right=blocked why=cover:clean_side_blocked", not HAS_LOGS
        or (logged("SWERVE_REPAIR_SIDE") or ""):find("right=blocked chosen=L why=cover:clean_side_blocked"),
        logged("SWERVE_REPAIR_SIDE"))

  reset()
  C.SWERVE_AVOID_REPAIR_SIDE = false
  st, w, inf = mk()
  d = swerve(st, w, inf, 1)
  check("knob off: cover pick L", d == 1, d)

  reset()
  st, w, inf = mk()
  w.pills[8].health = C.PILLS_MAX_HEALTH or 15     -- blocker whole: no rule
  d = swerve(st, w, inf, 1)
  check("no damaged blocker: cover pick L", d == 1, d)
end

-- ── 2. committed side ────────────────────────────────────────────────────
print("2. committed side")
do
  reset()
  DEEP_ABOVE = 100       -- R (north) blocked at swerve time, cover pick L
  local st, w, inf = mk()
  st.goal._repair_swerve_dir = { dir = -1, mx = 101, my = 102, tick = NOW - 30,
                                 left = "dirty", right = "clean" }
  st._bp_job = { type = "rebuild", quick = true, mx = 101, my = 102,
                 phase = "outbound", dispatch_tick = NOW - 30, arrive_tick = NOW + 20 }
  local d = swerve(st, w, inf, 1)
  check("job running: committed R used though terrain blocks R", d == -1, d)
  check("logged why=committed", not HAS_LOGS
        or (logged("SWERVE_REPAIR_SIDE") or ""):find("chosen=R why=committed"),
        logged("SWERVE_REPAIR_SIDE"))

  reset()
  DEEP_ABOVE = 100
  st, w, inf = mk()
  st.goal._repair_swerve_dir = { dir = -1, mx = 101, my = 102, tick = NOW - 30 }
  d = swerve(st, w, inf, 1)
  check("job ended: lock gone, R blocked -> cover pick L", d == 1, d)
  check("lock cleared", st.goal._repair_swerve_dir == nil, st.goal._repair_swerve_dir)
end

-- ── 3. the send ──────────────────────────────────────────────────────────
print("3. send")
do
  reset()
  local st, w, inf = mk()
  _G.BRAIN_DEBUG_MODE = true
  local qr = QR.evaluate(st, w, inf, NOW)
  _G.BRAIN_DEBUG_MODE = nil
  check("clean side R: send", qr and qr.result == "send", qr and qr.result)
  local c = st.goal._repair_swerve_dir
  check("side R committed at the send", c and c.dir == -1 and c.mx == 101 and c.my == 102 and c.tick == NOW,
        c and c.dir)
  check("QUICK_REPAIR line has side=R", not HAS_LOGS
        or (logged("QUICK_REPAIR t=") or ""):find("side=R%(clean_side"),
        logged("QUICK_REPAIR t="))
  -- The swerve on the send tick (and while the job runs) takes it even when
  -- the cover pick is L.
  local d = swerve(st, w, inf, 1)
  check("swerve on the send tick: committed R", d == -1, d)

  -- Pill A (reload 100) fires at NOW+1 and NOW+101; its shots meet the
  -- blocker 29 ticks later (NOW+30, NOW+130). The stay window (2026-11-24)
  -- counts only the shots that meet the blocker while he is on it.
  reset()
  WALK_PER_TILE = 30                -- walk 120: stay NOW+121 .. NOW+141
  st, w, inf = mk({ pill_a = true, blocker = "a" })
  _G.BRAIN_DEBUG_MODE = true
  qr = QR.evaluate(st, w, inf, NOW)
  _G.BRAIN_DEBUG_MODE = nil
  check("pill A across the blocker: no clean side",
        qr and qr.side and qr.side.dir == nil and qr.side.why == "both_dirty",
        qr and qr.side and qr.side.why)
  check("no clean side, A's shot meets it in the stay: wait:other_pill (no send)",
        qr and qr.result == "wait:other_pill", qr and qr.result)
  check("the conflict is pill A's", qr and qr.fire and qr.fire.pill == 5,
        qr and qr.fire and qr.fire.pill)
  -- From one tick before T_repair: he arrives on now + walk in most runs.
  check("the conflict tick is inside the stay window",
        qr and qr.fire and qr.window and qr.fire.at >= qr.window[1] - 1
        and qr.fire.at <= qr.window[2],
        qr and qr.fire and (tostring(qr.fire.at) .. " in "
                            .. tostring(qr.window and qr.window[1]) .. ".."
                            .. tostring(qr.window and qr.window[2])))
  check("window = arrive .. arrive + LGM_BUILD_TIME",
        qr and qr.window and qr.window[1] == NOW + 121
        and qr.window[2] == NOW + 121 + (C.LGM_BUILD_TIME or 20),
        qr and qr.window and (qr.window[1] .. ".." .. qr.window[2]))
  check("QUICK_REPAIR line logs window=1121..1141", not HAS_LOGS
        or (logged("QUICK_REPAIR t=") or ""):find("window=1121%.%.1141"),
        logged("QUICK_REPAIR t="))
  check("no side committed", st.goal._repair_swerve_dir == nil, st.goal._repair_swerve_dir)

  -- The same pill with walk 64 (stay NOW+65 .. NOW+85): A's shot crosses
  -- the dead blocker at NOW+30 while he walks; the next one fires after he
  -- is done. None meets him on the square: send.
  reset()
  st, w, inf = mk({ pill_a = true, blocker = "a" })
  qr = QR.evaluate(st, w, inf, NOW)
  check("A's shots cross only while he walks or after: send",
        qr and qr.result == "send", qr and (qr.result .. M_fire(qr)))

  reset()
  C.QUICK_REPAIR_WAIT_ALL_PILLS = false
  st, w, inf = mk({ pill_a = true, blocker = "a" })
  qr = QR.evaluate(st, w, inf, NOW)
  check("Rule 2 off: send (old rule)", qr and qr.result == "send", qr and qr.result)

  reset()
  C.SWERVE_AVOID_REPAIR_SIDE = false
  C.QUICK_REPAIR_WAIT_ALL_PILLS = false
  st, w, inf = mk()
  qr = QR.evaluate(st, w, inf, NOW)
  check("both off: send, no commit", qr and qr.result == "send"
        and st.goal._repair_swerve_dir == nil, qr and qr.result)
end

-- ── 4. outbound ──────────────────────────────────────────────────────────
print("4. outbound")
do
  local function out(opts)
    local st, w, inf = mk(opts)
    st._bp_job = { type = "rebuild", quick = true, mx = 102, my = 102,
                   phase = "outbound", dispatch_tick = NOW - 10, arrive_tick = NOW + 24 }
    inf.man_status = C.LGM_MOVING
    inf.man_x, inf.man_y = 101 * 256 + 128, 101 * 256 + 128
    local qr = QR.evaluate(st, w, inf, NOW)
    return qr, st, w, inf
  end
  reset()
  local qr, st, w, inf = out({ pill_a = true, blocker = "a" })
  check("pill A's shots meet him: outbound conflict", qr and qr.outbound ~= nil,
        qr and qr.outbound)
  check("the job is marked", st._bp_job._qr_abort ~= nil, st._bp_job._qr_abort)
  check("QUICK_REPAIR_ABORT why=pill_fire_predict logged", not HAS_LOGS
        or logged("QUICK_REPAIR_ABORT t=%d+ pill=%(102,102%) why=pill_fire_predict"),
        LOG[#LOG])
  local n = #LOG
  QR.evaluate(st, w, inf, NOW + 1)
  local again = 0
  for i = n + 1, #LOG do if LOG[i]:find("QUICK_REPAIR_ABORT") then again = again + 1 end end
  check("logged once per job", again == 0, again)

  reset()
  qr, st = out({ blocker = "a" })
  check("no second pill: no outbound conflict", qr and qr.outbound == nil,
        qr and qr.outbound and qr.outbound.pill)
  check("no abort logged", not logged("QUICK_REPAIR_ABORT"), logged("QUICK_REPAIR_ABORT"))

  reset()
  C.QUICK_REPAIR_WAIT_ALL_PILLS = false
  qr, st = out({ pill_a = true, blocker = "a" })
  check("Rule 2 off: no outbound check", qr and qr.outbound == nil, qr and qr.outbound)
end

-- ── 6. the builder walk ─────────────────────────────────────────────────
print("6. builder walk")
do
  -- The take target at (106,100) shoots the tank at (100,100) along the
  -- row y = 100.5; its shells explode on the tank, 6 tiles from the pill.
  -- The blocker squares below are off that row, so no shell hits them.
  reset()
  local st, w, inf = mk({ bmx = 103, bmy = 102 })
  local rb = { mx = 103, my = 102 }
  local k = { t = NOW + 300 }
  -- He stands on the row at (103.5, 100.5) and creeps south to the
  -- blocker: the shells fly right over him for many ticks, but explode on
  -- the tank 3 tiles away.
  local cf = QR.fire_conflict(w, inf, NOW, st.goal, k, rb, 103.5, 100.5,
                              NOW, NOW + 200, nil, false)
  check("shells fly over his walk, explode on the tank: no conflict", cf == nil,
        cf and (cf.how .. "@" .. tostring(cf.at)))

  st, w, inf = mk({ bmx = 100, bmy = 103 })
  rb = { mx = 100, my = 103 }
  -- He leaves the tank and creeps south: the next shell explodes on the
  -- tank while he is still within half a tile of it. Since the stay window
  -- (2026-11-24) his walk is not tested here: danger.lgm_certain_death and
  -- tank_clear test it.
  cf = QR.fire_conflict(w, inf, NOW, st.goal, k, rb, 100.5, 100.5,
                        NOW + 1, NOW + 400, nil, false)
  check("shell explodes on the tank beside him on his walk: no conflict here",
        cf == nil, cf and cf.how)

  -- The same walk, fast: he is 1 tile or more from the tank when the
  -- shells land there.
  cf = QR.fire_conflict(w, inf, NOW, st.goal, k, rb, 100.5, 100.5,
                        NOW + 1, NOW + 40, nil, false)
  check("he is clear of the tank when the shells land: no conflict", cf == nil,
        cf and (cf.how .. "@" .. tostring(cf.at)))
end

-- ── 7. send, then the same world one tick later ─────────────────────────
print("7. send then outbound")
do
  -- Send at NOW, then at NOW+1 the builder pool's job is out and the man
  -- stands on the tank; nothing else changes.
  local function send_then_out(substate, swerve_dir)
    reset()
    local st, w, inf = mk()
    if substate then st.goal.substate = substate end
    st.goal._swerve_dir = swerve_dir
    _G.BRAIN_DEBUG_MODE = true
    local qr = QR.evaluate(st, w, inf, NOW)
    _G.BRAIN_DEBUG_MODE = nil
    if not (qr and qr.result == "send") then return qr, nil, st end
    st._bp_job = { type = "rebuild", quick = true, mx = 101, my = 102,
                   phase = "outbound", dispatch_tick = NOW,
                   arrive_tick = qr.t_repair }
    st.tick = NOW + 1
    inf.man_status = C.LGM_MOVING
    inf.man_x, inf.man_y = inf.tankx, inf.tanky
    _G.BRAIN_DEBUG_MODE = true
    local qr2 = QR.evaluate(st, w, inf, NOW + 1)
    _G.BRAIN_DEBUG_MODE = nil
    return qr, qr2, st
  end
  local qr, qr2, st = send_then_out(nil, nil)
  check("engage: send at t", qr and qr.result == "send", qr and qr.result)
  check("engage: t+1 outbound, no conflict", qr2 and qr2.outbound == nil,
        qr2 and qr2.outbound and (qr2.outbound.how .. "@" .. tostring(qr2.outbound.at)))
  check("engage: no abort", st._bp_job and st._bp_job._qr_abort == nil
        and not logged("QUICK_REPAIR_ABORT"), logged("QUICK_REPAIR_ABORT"))
  local c = st.goal._repair_swerve_dir
  check("engage: the commit keeps the send's swerve start", c and c.ox == 100.5 and c.oy == 100.5,
        c and c.ox)

  qr, qr2, st = send_then_out("swerve", -1)
  check("running swerve on clean R: send", qr and qr.result == "send", qr and qr.result)
  c = st.goal._repair_swerve_dir
  check("running swerve on clean R: R committed", c and c.dir == -1 and c.why == "running_clean",
        c and c.why)
  check("running swerve: t+1 outbound, no conflict", qr2 and qr2.outbound == nil,
        qr2 and qr2.outbound and (qr2.outbound.how .. "@" .. tostring(qr2.outbound.at)))
  check("running swerve: no abort", not logged("QUICK_REPAIR_ABORT"), logged("QUICK_REPAIR_ABORT"))
  check("running swerve: SWERVE_REPAIR_SIDE why=committed at the send", not HAS_LOGS
        or (logged("SWERVE_REPAIR_SIDE") or ""):find("chosen=R why=committed"),
        logged("SWERVE_REPAIR_SIDE"))

  -- The dirty side's shot meets the dead blocker at NOW+44, while he walks
  -- (stay NOW+49 .. NOW+69): the shell flies over it, so it does not count.
  qr, qr2, st = send_then_out("swerve", 1)
  check("running swerve on dirty L: send (its shot crosses while he walks)",
        qr and qr.result == "send", qr and (qr.result .. M_fire(qr)))
  check("running swerve on dirty L: nothing committed", st.goal._repair_swerve_dir == nil,
        st.goal._repair_swerve_dir)
end

-- ── 5. presets ───────────────────────────────────────────────────────────
print("5. presets")
do
  local keel = C.PRESETS.keel or {}
  local mast = C.PRESETS.mast or {}
  check("defaults true", DEF_SIDE == true and DEF_WAIT == true)
  check("keel: SWERVE_AVOID_REPAIR_SIDE false", keel.SWERVE_AVOID_REPAIR_SIDE == false)
  check("keel: QUICK_REPAIR_WAIT_ALL_PILLS false", keel.QUICK_REPAIR_WAIT_ALL_PILLS == false)
  check("mast: SWERVE_AVOID_REPAIR_SIDE false", mast.SWERVE_AVOID_REPAIR_SIDE == false)
  check("mast: QUICK_REPAIR_WAIT_ALL_PILLS false", mast.QUICK_REPAIR_WAIT_ALL_PILLS == false)
  check("margin 0.0 (Andrew Nov 24)", C.REPAIR_FIRE_LANE_MARGIN_TILES == 0, C.REPAIR_FIRE_LANE_MARGIN_TILES)
end

-- -- 8. run_c15 beside_n t=785: a target whose reload phase is unknown --
-- The real numbers: the take target (126,118) at the engine's lowest
-- reload (6), no shell of it in the air; the dead blocker (124,118); the
-- tank at (119.58,116.52). T_kill 858 (first 796 + 13 + flight 49), the
-- builder on the blocker 869..889. Its real shots were 788, 794 .. 854 and
-- the one at 854 killed him at 868: the check must wait.
print("8. run_c15 beside_n: unknown reload phase")
do
  reset()
  local T = 785
  local LPm = require("live_physics")
  local tp = pill(126, 118, 6, "neutral")
  -- Our hits on it, every 13 ticks (EVENT_PILL_UPDATE armour 15 -> 6).
  tp._eng_hits = { 677, 690, 703, 716, 729, 742, 755, 768, 781 }
  local bp = pill(124, 118, 0, "friendly", true)
  local w = { pills = { [1] = tp, [2] = bp }, pill_at = {}, bases = {}, base_at = {} }
  for id, p in pairs(w.pills) do w.pill_at[p.my * 256 + p.mx] = { { pill = p, id = id } } end
  local goal = { kind = "attack_pill", mx = 126, my = 118, target_id = 1, substate = "engage" }
  local inf = { tankx = math.floor(119.58 * 256), tanky = math.floor(116.52 * 256),
                trees = 36, man_status = C.LGM_INTANK, inboat = false, armour = 40,
                carried_pills = 0, player_number = 0, shells = 27, direction = 72,
                speed = 0, reload = false, reload_ticks = 13, objects = {} }
  local x = LPm.context(inf, {}) or {}
  local _, rl, last = QR.pill_next_fire(tp, inf, T, x)
  check("the target is at reload 6 with no shot seen", rl == 6 and last == nil,
        tostring(rl) .. "/" .. tostring(last))
  -- The real game: the shot heard at 782 gives the phase 788, 794 .. 854.
  tp._heard_fire = 782
  local nx, _, l2 = QR.pill_next_fire(tp, inf, T, x)
  check("heard at 782: last 782, next 788", l2 == 782 and nx == 788,
        tostring(l2) .. "/" .. tostring(nx))
  local fl = QR.fire_ticks(tp, inf, T, x, 104, nil, 858)
  check("heard at 782: fires 788 .. 854", fl[1] == 788 and fl[#fl] == 854,
        tostring(fl[1]) .. ".." .. tostring(fl[#fl]))
  -- Four of our shells in the air (they hit at 794 .. 833), two still to
  -- fire (796 and 809, 49 ticks of flight).
  local k0 = { t = 858, src = "predict", first = 796, reload = 13, remaining = 2,
               flight = 49, land = { 794, 807, 820, 833 }, n_inflight = 4 }
  local cf0 = QR.fire_conflict(w, inf, T, goal, k0, { mx = 124, my = 118 }, 119.5, 116.5,
                               T + 1, 869, nil, false)
  -- Its shell is on the blocker 861 .. 868 (6 steps out at 854, then one
  -- a tick); he arrives at 868, one tick before T_repair: wait.
  check("heard at 782: the shot at 854 is on the blocker when he arrives (868): wait",
        cf0 ~= nil and cf0.target and cf0.fire == 854 and cf0.at == 868,
        cf0 and (tostring(cf0.how) .. " fire=" .. tostring(cf0.fire) .. " at=" .. tostring(cf0.at)) or "nil")
  tp._heard_fire = nil
  local k = { t = 858, src = "predict", first = 796, reload = 13, remaining = 2,
              flight = 49, land = { 794, 807, 820, 833 }, n_inflight = 4 }
  local rb = { mx = 124, my = 118 }
  local cf = QR.fire_conflict(w, inf, T, goal, k, rb, 119.5, 116.5, T + 1, 869, nil, false)
  check("a shot late in the unknown phase meets the blocker in the stay: wait",
        cf ~= nil and cf.target and cf.at >= 868 and cf.fire < 858,
        cf and (tostring(cf.how) .. " fire=" .. tostring(cf.fire) .. " at=" .. tostring(cf.at)) or "nil")
  check("helper exists", type(QR.any_phase) == "function")
  -- The shell's ticks on the squares, from the real dump: the target
  -- (126,118) fires west; 6 steps out at the fire tick, one a tick.
  local a, b = QR.shell_on_square(126.5, 118.5, -1, 0, 124, 118, 0, 8, QR.shell_lead(inf))
  check("shell on the blocker (124,118) fire+7 .. fire+14", a == 7 and b == 14,
        tostring(a) .. ".." .. tostring(b))
  a, b = QR.shell_on_square(126.5, 118.5, -1, 0, 125, 118, 0, 8, QR.shell_lead(inf))
  check("shell on (125,118) fire+0 .. fire+6", a == 0 and b == 6,
        tostring(a) .. ".." .. tostring(b))
  -- The line to the tank at 785 crosses only a corner of the blocker.
  local ddx, ddy = 119.58 - 126.5, 116.52 - 118.5
  local dl = math.sqrt(ddx * ddx + ddy * ddy)
  a, b = QR.shell_on_square(126.5, 118.5, ddx / dl, ddy / dl, 124, 118, 0, 8, QR.shell_lead(inf))
  check("line to the tank at 785: on the blocker 2 ticks only", a and b - a <= 2,
        tostring(a) .. ".." .. tostring(b))
  if type(QR.any_phase) == "function" then
    local f = QR.any_phase(tp, inf, T, x, 120, 858, { 786, 792, 852 })
    check("any_phase: every tick from the first shot to the kill",
          #f == 858 - 786 and f[1] == 786 and f[#f] == 857, #f .. " " .. tostring(f[#f]))
    local calm = pill(126, 118, 6, "neutral")
    local g = QR.any_phase(calm, inf, T, x, 120, 858, { 786, 852 })
    check("any_phase: a calm pill keeps its own ticks", #g == 2, #g)
  end
end

-- -- 9. run_c17 beside_e t=623: a shot east along the blocker row --------
-- The real numbers: the take target (125,118) fires east every 6 ticks
-- (heard 620; its shots 626 .. 692, then it dies at 697); the blocker
-- (127,118) next door but one; the tank at (132.42,116.52). The send at
-- 623 had T_repair 707; he arrived at 706, and the shot at 692 was on the
-- blocker 699 .. 706: it hit the rebuilt pill at 706 and killed him.
print("9. run_c17 beside_e: a shot east along the blocker row")
do
  reset()
  local T = 623
  local LPm = require("live_physics")
  local tp = pill(125, 118, 6, "neutral")
  -- Our hits on it, every 13 ticks (EVENT_PILL_UPDATE armour 14 -> 6).
  tp._eng_hits = { 515, 528, 541, 554, 567, 580, 593, 606, 619 }
  tp._heard_fire = 620
  local bp = pill(127, 118, 1, "friendly", true)
  local w = { pills = { [1] = tp, [2] = bp }, pill_at = {}, bases = {}, base_at = {} }
  for id, p in pairs(w.pills) do w.pill_at[p.my * 256 + p.mx] = { { pill = p, id = id } } end
  local goal = { kind = "attack_pill", mx = 125, my = 118, target_id = 1, substate = "shoot_pill" }
  local inf = { tankx = math.floor(132.4211 * 256), tanky = math.floor(116.5154 * 256),
                trees = 36, man_status = C.LGM_INTANK, inboat = false, armour = 40,
                carried_pills = 0, player_number = 0, shells = 27, direction = 183,
                speed = 0, reload = false, reload_ticks = 13, objects = {} }
  local x = LPm.context(inf, {}) or {}
  local fl = QR.fire_ticks(tp, inf, T, x, 104, nil, 696)
  check("c17: fires 626 .. 692", fl[1] == 626 and fl[#fl] == 692,
        tostring(fl[1]) .. ".." .. tostring(fl[#fl]))
  -- The shell east from (125,118): on (127,118) fire+7 .. fire+14, the
  -- same as the c15 shell west (a point on a square edge is on the square
  -- nearer the pill: the real shells are never exactly on the axis).
  local a, b = QR.shell_on_square(125.5, 118.5, 1, 0, 127, 118, 0, 8, QR.shell_lead(inf))
  check("c17: shell east on the blocker (127,118) fire+7 .. fire+14", a == 7 and b == 14,
        tostring(a) .. ".." .. tostring(b))
  a, b = QR.shell_on_square(125.5, 118.5, 1, 0, 126, 118, 0, 8, QR.shell_lead(inf))
  check("c17: shell east on (126,118) fire+0 .. fire+6", a == 0 and b == 6,
        tostring(a) .. ".." .. tostring(b))
  local k = { t = 696, src = "predict", first = 634, reload = 13, remaining = 2,
              flight = 49, land = { 632, 645, 658, 671 }, n_inflight = 4 }
  local cf = QR.fire_conflict(w, inf, T, goal, k, { mx = 127, my = 118 }, 132.5, 116.5,
                              T + 1, 707, nil, false)
  check("c17 at 623: the shot at 692 is on the blocker when he arrives (706): wait",
        cf ~= nil and cf.target and cf.fire == 692 and cf.at == 706,
        cf and (tostring(cf.how) .. " fire=" .. tostring(cf.fire) .. " at=" .. tostring(cf.at)) or "nil")
  -- One tick later he arrives at 707: the shell has left; the pill is dead
  -- before its next shot (698). Send.
  cf = QR.fire_conflict(w, inf, T + 1, goal, k, { mx = 127, my = 118 }, 132.5, 116.5,
                        T + 2, 708, nil, false)
  check("c17 at 624: arrival 707, after the last shell: no conflict", cf == nil,
        cf and (tostring(cf.how) .. " fire=" .. tostring(cf.fire) .. " at=" .. tostring(cf.at)) or "nil")
end

print("10. a5 noc beside_e: the target's shot on its kill tick")
do
  -- dt_demo_nov24 a5 noc beside_e, t=449 (the send): target #0 (125,118)
  -- fires every 6 ticks (fire events 455, 461 .. 515, 521); T_kill was
  -- predicted 520 (first 458, reload 13, remaining 2, flight 49). Pill #1
  -- (126,118) died at t=449. Blocker (127,118) hp 1; tank (132.42,116.52);
  -- both swerve sides dirty, no side committed, no swerve running.
  -- T_repair 533 (window from 532). The real end: the pill died at 521
  -- and fired at 521; the tank swerved to (132,118); that shell was on
  -- (127,118) at 528..531 and exploded on the rebuilt pill at 532 with
  -- the builder on it.
  local T = 449
  local tp = pill(125, 118, 6, "neutral")
  local p1 = pill(126, 118, 0, "hostile")
  local bp = pill(127, 118, 1, "friendly", true)
  local w = { pills = { [0] = tp, [1] = p1, [2] = bp }, pill_at = {}, bases = {}, base_at = {} }
  for id, p in pairs(w.pills) do w.pill_at[p.my * 256 + p.mx] = { { pill = p, id = id } } end
  local goal = { kind = "attack_pill", mx = 125, my = 118, target_id = 0, substate = "shoot_pill" }
  local inf = { tankx = math.floor(132.4211 * 256), tanky = math.floor(116.5154 * 256),
                trees = 40, man_status = C.LGM_INTANK, inboat = false, armour = 40,
                carried_pills = 0, player_number = 0, shells = 27, direction = 185,
                speed = 4, reload = false, reload_ticks = 13, objects = {} }
  local k = { t = 520, src = "predict", first = 458, reload = 13, remaining = 2,
              flight = 49, land = {}, n_inflight = 0 }
  local pnf, ap = QR.pill_next_fire, QR.any_phase
  QR.pill_next_fire = function(p, _, now)
    if p == tp then return 455, 6, 449 end
    return now + 1000, 100, nil
  end
  QR.any_phase = function(_, _, _, _, _, _, fires) return fires end
  -- fire_ticks itself (no extra hits): 455 .. 515 before T_kill 520; with
  -- the kill tick 521 (slop 1) and the death-tick shot, 521 too.
  C.QUICK_REPAIR_DEATH_TICK_SHOT = true
  local f1 = QR.fire_ticks(tp, inf, T, {}, 104, nil, 521)
  C.QUICK_REPAIR_DEATH_TICK_SHOT = false
  local f0 = QR.fire_ticks(tp, inf, T, {}, 104, nil, 521)
  check("a5 beside_e: fire_ticks to 521 with the death-tick shot", f1[#f1] == 521 and f1[1] == 455,
        tostring(f1[1]) .. ".." .. tostring(f1[#f1]))
  check("a5 beside_e: fire_ticks to 515 without it (old rule)", f0[#f0] == 515,
        tostring(f0[#f0]))
  -- The ladder with our hits (own_hits) is not under test here: the fire
  -- list is the one the engine showed, cut at the death tick as fire_ticks
  -- cuts it.
  local ft = QR.fire_ticks
  QR.fire_ticks = function(p, _, _, _, _, _, death)
    local out = {}
    if p ~= tp then return out end
    for f = 455, 521, 6 do
      if not death or f < death or (C.QUICK_REPAIR_DEATH_TICK_SHOT and f == death) then
        out[#out + 1] = f
      end
    end
    return out
  end
  local rb = { mx = 127, my = 118 }
  local function run(dts, slop)
    reset()
    C.QUICK_REPAIR_DEATH_TICK_SHOT, C.QUICK_REPAIR_KILL_SLOP_TICKS = dts, slop
    return QR.fire_conflict(w, inf, T, goal, k, rb, 132.5, 116.5, T + 1, 533, nil, false)
  end
  local function txt(cf)
    return cf and (tostring(cf.pill) .. " " .. tostring(cf.how) .. " fire=" .. tostring(cf.fire)
                   .. " at=" .. tostring(cf.at)) or "nil"
  end
  local c_old = run(false, 0)
  local c_def = run(true, 0)
  local c_s1  = run(true, 1)
  local c_s1o = run(false, 1)
  local c_s2  = run(true, 2)
  QR.pill_next_fire, QR.any_phase, QR.fire_ticks = pnf, ap, ft
  C.QUICK_REPAIR_DEATH_TICK_SHOT = true
  C.QUICK_REPAIR_KILL_SLOP_TICKS = 0
  check("a5 beside_e: old rule: no conflict (the send at 449)", c_old == nil, txt(c_old))
  check("a5 beside_e: slop 0 (default): the 521 shot is past T_kill 520: no conflict",
        c_def == nil, txt(c_def))
  check("a5 beside_e: slop 1 + the death-tick shot: the 521 shot on the blocker at 532: wait",
        c_s1 ~= nil and c_s1.pill == 0 and c_s1.fire == 521 and c_s1.at == 532, txt(c_s1))
  check("a5 beside_e: slop 1 without the death-tick shot: 521 is cut", c_s1o == nil, txt(c_s1o))
  check("a5 beside_e: slop 2: wait", c_s2 ~= nil and c_s2.fire == 521, txt(c_s2))
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
