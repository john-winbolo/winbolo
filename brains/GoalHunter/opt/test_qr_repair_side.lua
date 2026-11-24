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
--      no clean side -> wait:other_pill (no send); knobs off -> send.
--   4. outbound: the builder is out and the second pill's shots meet him ->
--      QUICK_REPAIR_ABORT why=pill_fire_predict; no second pill -> none.
--   5. presets: KEEL and MAST false, defaults true; margin 0.0.
--   6. the builder walk: a shell that only flies over him is no conflict;
--      a shell that explodes on the tank while he is still beside it is
--      one (the engine kills a builder only where a shell explodes).
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
_G.cpf_lgm_travel_ticks_map = function(fx, fy, tx, ty)
  return (math.abs(fx - tx) + math.abs(fy - ty)) * 16
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

  reset()
  st, w, inf = mk({ pill_a = true, blocker = "a" })
  qr = QR.evaluate(st, w, inf, NOW)
  check("pill A across the blocker: no clean side",
        qr and qr.side and qr.side.dir == nil and qr.side.why == "both_dirty",
        qr and qr.side and qr.side.why)
  check("no clean side, A's shots cross it: wait:other_pill (no send)",
        qr and qr.result == "wait:other_pill", qr and qr.result)
  check("the conflict is pill A's", qr and qr.fire and qr.fire.pill == 5,
        qr and qr.fire and qr.fire.pill)
  check("no side committed", st.goal._repair_swerve_dir == nil, st.goal._repair_swerve_dir)

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
  -- tank while he is still within half a tile of it.
  cf = QR.fire_conflict(w, inf, NOW, st.goal, k, rb, 100.5, 100.5,
                        NOW + 1, NOW + 400, nil, false)
  check("shell explodes on the tank beside him: builder conflict",
        cf and cf.how == "builder" and cf.pill == 3, cf and cf.how)
  check("the conflict tick is the explosion (fire + 48)",
        cf and cf.at == cf.fire + 48, cf and (tostring(cf.at) .. "/" .. tostring(cf.fire)))

  -- The same walk, fast: he is 1 tile or more from the tank when the
  -- shells land there.
  cf = QR.fire_conflict(w, inf, NOW, st.goal, k, rb, 100.5, 100.5,
                        NOW + 1, NOW + 40, nil, false)
  check("he is clear of the tank when the shells land: no conflict", cf == nil,
        cf and (cf.how .. "@" .. tostring(cf.at)))
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

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
