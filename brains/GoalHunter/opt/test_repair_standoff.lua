-- =========================================================================
-- test_repair_standoff.lua — standalone unit tests (2026-11-03) for the
-- repair_pill STANDOFF (repair_standoff.lua, C.REPAIR_STANDOFF_ENABLED):
--   * the out-of-range standoff is picked (pill range 8 + margin 1);
--   * a wall-covered square is safe (and the target pill is never cover);
--   * a heated pill ignores walls;
--   * a route that enters an unsafe square is rejected (leaving one is not);
--   * no standoff -> push (the feeder sends the builder as before);
--   * builder out -> the tank waits at the standoff (no re-pick);
--   * the feeder sends the builder only from the standoff, cap 12;
--   * KEEL (knob off): no record, no nav change, the feeder as before.
-- No engine: terrain, the shell sim, the walk sim and the route are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_repair_standoff.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local LOG = {}
package.loaded["print2"] = setmetatable({ set_tick = function() end }, {
  __call = function(_, s) LOG[#LOG + 1] = tostring(s) end })

-- The C threat grids are not loaded here: every lookup reads 0.
_G.gh_threat = _G.gh_threat or setmetatable({}, { __index = function()
  return function() return 0 end
end })
if not math.atan2 then math.atan2 = function(y, x) return math.atan(y, x) end end

-- Terrain: grass everywhere, plus the walls a test sets.
local GRASS = 7
local WALLS = {}
_G.get_terrain = function(mx, my) return WALLS[my * 256 + mx] or GRASS end

-- Walk sim: 16 ticks a square (Manhattan), or a test override.
local WALK_FN = nil
_G.cpf_lgm_travel_ticks_map = function(sx, sy, dx, dy, bx, by, maxt, stuck)
  if WALK_FN then return WALK_FN(sx, sy, dx, dy, maxt) end
  return (math.abs(sx - dx) + math.abs(sy - dy)) * 16
end
_G.BUILDMODE_PBOX = 3

local C  = require("constants")
local U  = require("util")
local RS = require("repair_standoff")

local function has_logs()
  local f = io.open("repair_standoff.lua", "r")
  if not f then return false end
  local src = f:read("*a"); f:close()
  return src:find("print2%(") ~= nil
end
local HAS_LOGS = has_logs()

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

-- ── Stubs ────────────────────────────────────────────────────────────────

RS.deps.ttype = function(mx, my) return WALLS[my * 256 + mx] or GRASS end
RS.deps.clock = function() return 0 end

-- Shell sim: the squares a line crosses, sampled every 1/16 square.
RS.deps.shot = function(ox, oy, tx, ty)
  local ax, ay, bx, by = ox / 256, oy / 256, tx / 256, ty / 256
  local n = math.max(1, math.ceil(math.max(math.abs(bx - ax), math.abs(by - ay)) * 16))
  local out, lastk = {}, nil
  for i = 0, n do
    local x = math.floor(ax + (bx - ax) * i / n)
    local y = math.floor(ay + (by - ay) * i / n)
    local k = y * 256 + x
    if k ~= lastk then out[#out + 1] = { mx = x, my = y }; lastk = k end
  end
  return out
end

-- Route: a straight line of squares from the tank to the square, unless a
-- test gives a fixed path for that square.
local TANK = { 100, 115 }
local ROUTES = {}
RS.deps.route = function(mx, my)
  local r = ROUTES[my * 256 + mx]
  if r then return r end
  local sx, sy = TANK[1], TANK[2]
  local n = math.max(math.abs(mx - sx), math.abs(my - sy))
  local p = {}
  for i = 0, n do
    p[#p + 1] = sx + math.floor((mx - sx) * i / math.max(n, 1) + 0.5)
    p[#p + 1] = sy + math.floor((my - sy) * i / math.max(n, 1) + 0.5)
  end
  return p
end

-- A world: the target pill (friendly, 14 health) and other pills.
local function make_world(pills)
  local w = { pills = {}, pill_at = {}, bases = {}, tanks = {} }
  for _, p in ipairs(pills) do
    w.pills[p.id] = p
    local k = p.my * 256 + p.mx
    w.pill_at[k] = w.pill_at[k] or {}
    table.insert(w.pill_at[k], { id = p.id, pill = p })
  end
  return w
end

local T = { id = 3, mx = 100, my = 100, owner = "friendly", health = 14 }
local function hostile(id, mx, my, anger)
  return { id = id, mx = mx, my = my, owner = "neutral", health = 15, anger = anger or 0 }
end

local function reset()
  WALLS, ROUTES, WALK_FN, LOG = {}, {}, nil, {}
  TANK = { 100, 115 }
  C.REPAIR_STANDOFF_ENABLED = true
  C.REPAIR_STANDOFF_MARGIN_TILES = 1.0
  C.REPAIR_STANDOFF_MAX_WALK_TICKS = 400
  C.REPAIR_STANDOFF_WALL_COVER = true
  C.REPAIR_STANDOFF_HEATED_ANGER = 0.34
end

local function pick(world)
  local th = RS.threats(world, T.mx, T.my)
  return RS.pick(world, T.mx, T.my, TANK[1], TANK[2], th)
end

local function log_has(pat)
  for _, l in ipairs(LOG) do if l:find(pat) then return l end end
  return nil
end

-- ── Knobs ────────────────────────────────────────────────────────────────
print("knobs")
reset()
check("default on", C.REPAIR_STANDOFF_ENABLED == true)
check("KEEL off", C.PRESETS and C.PRESETS.keel
      and C.PRESETS.keel.REPAIR_STANDOFF_ENABLED == false)
check("safe distance 8 + 1 = 9", RS.safe_dist() == 9, RS.safe_dist())
check("cap is REPAIR_DISPATCH_DIST_DANGEROUS (12)", RS.cap() == 12, RS.cap())

-- ── Out of range ─────────────────────────────────────────────────────────
print("out-of-range standoff")
do
  reset()
  -- Pill #2 four squares north of the target. The nearest square 9 or more
  -- from it is (100,105), d=5. The squares between are open: the line from
  -- #2 crosses the target pill, which is never cover.
  local w = make_world({ T, hostile(2, 100, 96) })
  local rec = pick(w)
  check("mode standoff", rec.mode == "standoff", rec.mode)
  check("pick (100,105)", rec.mx == 100 and rec.my == 105,
        tostring(rec.mx) .. "," .. tostring(rec.my))
  check("walk 5 squares x 16", rec.walk == 80, rec.walk)
  local ctx = RS.new_ctx(w, T.mx, T.my, RS.threats(w, T.mx, T.my))
  local ok, t, why = RS.square_safe(ctx, 100, 101)
  check("(100,101) unsafe: open (target pill is not cover)", not ok and why == "open", why)
  check("no pill near: mode clear", pick(make_world({ T, hostile(2, 100, 70) })).mode == "clear")
  check("allied pill is not a threat", pick(make_world({ T,
        { id = 5, mx = 100, my = 96, owner = "allied", health = 15 } })).mode == "clear")
  check("dead pill is not a threat", pick(make_world({ T,
        { id = 5, mx = 100, my = 96, owner = "hostile", health = 0 } })).mode == "clear")
  C.REPAIR_STANDOFF_MARGIN_TILES = 2.0
  rec = pick(w)
  check("margin 2: pick (100,106)", rec.mx == 100 and rec.my == 106,
        tostring(rec.mx) .. "," .. tostring(rec.my))
end

-- ── Wall cover ───────────────────────────────────────────────────────────
print("wall cover")
do
  reset()
  WALLS[98 * 256 + 100] = C.T_BUILDING
  local w = make_world({ T, hostile(2, 100, 96, 0.2) })
  local rec = pick(w)
  check("pick (100,101) behind the wall at (100,98)", rec.mode == "standoff"
        and rec.mx == 100 and rec.my == 101, tostring(rec.mx) .. "," .. tostring(rec.my))
  local ctx = RS.new_ctx(w, T.mx, T.my, RS.threats(w, T.mx, T.my))
  local ok, cu = RS.covered(ctx, ctx.threats[1], 100, 101)
  check("covered, used wall (100,98)", ok and cu and cu[1] and cu[1].mx == 100 and cu[1].my == 98,
        ok)
  WALLS[98 * 256 + 100] = C.T_HALFBUILD
  check("damaged wall counts too", pick(w).my == 101)
  WALLS[98 * 256 + 100] = nil
  -- A friendly pill as cover: by health (PILL_COVER_BY_HEALTH).
  local fp = { id = 6, mx = 100, my = 98, owner = "friendly", health = 10 }
  local w2 = make_world({ T, hostile(2, 100, 96, 0.2), fp })
  local saved = C.PILL_COVER_BY_HEALTH
  C.PILL_COVER_BY_HEALTH = true
  check("friendly pill 10 health (2 walls) is cover", pick(w2).my == 101)
  fp.health = 4
  check("friendly pill 4 health (0 walls) is not cover", pick(w2).my == 105)
  C.PILL_COVER_BY_HEALTH = saved
  -- Wall cover off: walls do not count.
  WALLS[98 * 256 + 100] = C.T_BUILDING
  C.REPAIR_STANDOFF_WALL_COVER = false
  rec = pick(w)
  check("wall cover off: pick (100,105)", rec.my == 105, rec.my)
end

-- ── Heated pill ──────────────────────────────────────────────────────────
print("heated pill")
do
  reset()
  WALLS[98 * 256 + 100] = C.T_BUILDING
  local w = make_world({ T, hostile(2, 100, 96, 0.5) })
  local rec = pick(w)
  check("anger 0.5: walls ignored, pick (100,105)", rec.mx == 100 and rec.my == 105,
        tostring(rec.mx) .. "," .. tostring(rec.my))
  local ctx = RS.new_ctx(w, T.mx, T.my, RS.threats(w, T.mx, T.my))
  local ok, _, why = RS.square_safe(ctx, 100, 101)
  check("(100,101) unsafe: heated", not ok and why == "heated", why)
  w.pills[2].anger = 0.34
  check("anger 0.34 (= limit) is not heated", pick(w).my == 101)
end

-- ── Route ────────────────────────────────────────────────────────────────
print("route")
do
  reset()
  local w = make_world({ T, hostile(2, 100, 96) })
  -- The route to (100,105) goes round by (96,104), 8.9 from pill #2.
  ROUTES[105 * 256 + 100] = { 100, 115, 98, 110, 96, 104, 100, 105 }
  local rec = pick(w)
  check("route through (96,104) rejected", not (rec.mx == 100 and rec.my == 105),
        tostring(rec.mx) .. "," .. tostring(rec.my))
  check("next square (99,105) picked", rec.mx == 99 and rec.my == 105,
        tostring(rec.mx) .. "," .. tostring(rec.my))
  check("route_fail counted", rec.counts.route == 1, rec.counts.route)
  check("first_reject names the square", rec.first_reject
        and rec.first_reject.why:find("route_enters%(96,104%)") ~= nil,
        rec.first_reject and rec.first_reject.why)
  -- Leaving the unsafe zone is not entering it: the tank starts at (100,99).
  reset()
  TANK = { 101, 99 }
  rec = pick(w)
  check("tank inside range, route out of it: ok", rec.mode == "standoff"
        and rec.counts.route == 0, rec.counts.route)
  -- No route at all.
  reset()
  RS.deps.route, ROUTES._saved = function() return nil end, RS.deps.route
  rec = pick(w)
  check("no route: push", rec.mode == "push", rec.mode)
  RS.deps.route = ROUTES._saved
end

-- ── No standoff: push ────────────────────────────────────────────────────
print("no standoff")
do
  reset()
  WALK_FN = function() return -1 end
  local w = make_world({ T, hostile(2, 100, 96) })
  local rec = pick(w)
  check("walk sim fails everywhere: push", rec.mode == "push", rec.mode)
  check("push reason given", rec.why == "check_limit" or rec.why == "no_safe_square", rec.why)
  WALK_FN = function(sx, sy, dx, dy) return (math.abs(sx - dx) + math.abs(sy - dy)) * 100 end
  rec = pick(w)
  check("walk 500 > 400: push", rec.mode == "push", rec.mode)
  C.REPAIR_STANDOFF_MAX_WALK_TICKS = 500
  rec = pick(w)
  check("max walk 500: standoff (100,105)", rec.mode == "standoff" and rec.my == 105, rec.mode)
  -- Pills all round: no safe square inside the cap.
  reset()
  local w2 = make_world({ T, hostile(2, 100, 96), hostile(4, 100, 104),
                          hostile(5, 96, 100), hostile(6, 104, 100) })
  rec = pick(w2)
  check("ringed by pills: push no_safe_square", rec.mode == "push"
        and rec.why == "no_safe_square", rec.why)
end

-- ── Per-tick driver, nav, wait while the builder is out ──────────────────
local bpool = require("builder_pool")

local function mk_state(w)
  return { tick = 1000, goal = { kind = "repair_pill", mx = 100, my = 100, target_id = 3 },
           perc = { threat_at_tank = 0 } }
end
local function mk_info(mx, my, man)
  return { tankx = mx * 256 + 128, tanky = my * 256 + 128, man_status = man or C.LGM_INTANK,
           trees = 5, inboat = false }
end

print("driver and wait")
do
  reset()
  local w = make_world({ T, hostile(2, 100, 96) })
  local s = mk_state(w)
  local rec = bpool.standoff_update(s, w, mk_info(100, 115), 1000)
  check("update: record", rec and rec.mode == "standoff" and rec.my == 105, rec and rec.mode)
  local nx, ny = RS.nav_target(s, s.goal)
  check("nav target is the standoff", nx == 100 and ny == 105, tostring(nx) .. "," .. tostring(ny))
  if HAS_LOGS then
    local l = log_has("^REPAIR_STANDOFF ")
    check("REPAIR_STANDOFF logged", l ~= nil)
    check("log has pick, walk, route, pills", l and l:find("pick=%(100,105%)")
          and l:find("walk=80/400") and l:find("route=ok") and l:find("#2%(100,96%)")
          and l:find("why_pick=new_goal"), l)
  end
  local n0 = #LOG
  bpool.standoff_update(s, w, mk_info(100, 110), 1010)
  check("no re-pick inside REFRESH_TICKS", s._repair_standoff.tick == 1000)
  -- Builder out: no re-pick, even after REFRESH_TICKS and a pill change.
  w.pills[2].anger = 0.9
  bpool.standoff_update(s, w, mk_info(100, 105, C.LGM_MOVING), 1500)
  check("builder out: same record (wait)", s._repair_standoff.tick == 1000
        and s._repair_standoff.mx == 100 and s._repair_standoff.my == 105)
  check("builder out: nav still the standoff", select(2, RS.nav_target(s, s.goal)) == 105)
  check("at_standoff on (100,105)", RS.at_standoff(s, s.goal, 100, 105))
  check("not at_standoff on (100,104)", not RS.at_standoff(s, s.goal, 100, 104))
  -- Builder back: the heated change makes a re-pick.
  bpool.standoff_update(s, w, mk_info(100, 105), 1501)
  check("builder back: re-pick on pill change", s._repair_standoff.tick == 1501)
  if HAS_LOGS then
    check("re-pick reason pills_changed", log_has("why_pick=pills_changed") ~= nil)
  end
  -- Lost cover: a used wall is gone.
  reset()
  WALLS[98 * 256 + 100] = C.T_BUILDING
  local w3 = make_world({ T, hostile(2, 100, 96, 0.2) })
  local s3 = mk_state(w3)
  bpool.standoff_update(s3, w3, mk_info(100, 115), 2000)
  check("cover pick (100,101)", s3._repair_standoff.my == 101, s3._repair_standoff.my)
  WALLS[98 * 256 + 100] = nil
  bpool.standoff_update(s3, w3, mk_info(100, 110), 2001)
  check("wall gone: re-pick to (100,105)", s3._repair_standoff.tick == 2001
        and s3._repair_standoff.my == 105, s3._repair_standoff.my)
  -- Debug mode: the viz draw runs without an error.
  local viz = require("viz")
  local is_on, drawn = viz.is_on, 0
  viz.is_on = function() return true end
  _G.overlay_text = function() drawn = drawn + 1 end
  _G.BRAIN_DEBUG_MODE = true
  s3._repair_standoff.tick = -1000
  local okd, errd = pcall(bpool.standoff_update, s3, w3, mk_info(100, 110), 2003)
  _G.BRAIN_DEBUG_MODE = nil
  viz.is_on, _G.overlay_text = is_on, nil
  if HAS_LOGS then check("debug mode: viz labels drawn", drawn >= 2, drawn) end
  check("debug mode: update + viz draw run", okd, errd)
  -- Other goal kinds: no record.
  s3.goal = { kind = "attack_pill", mx = 100, my = 96 }
  bpool.standoff_update(s3, w3, mk_info(100, 110), 2002)
  check("other goal: no record", s3._repair_standoff == nil)
end

-- ── Feeder ───────────────────────────────────────────────────────────────
print("feeder")
do
  local function feed(s, w, info)
    s._bp_seed = nil
    bpool.repair_feeder(s, w, info, s.tick)
    return s._bp_seed
  end
  reset()
  local w = make_world({ T, hostile(2, 100, 96) })
  local s = mk_state(w)
  bpool.standoff_update(s, w, mk_info(100, 115), 1000)
  check("standoff: no send from (100,101) (not the standoff)", feed(s, w, mk_info(100, 101)) == nil)
  check("standoff: no send from (100,106)", feed(s, w, mk_info(100, 106)) == nil)
  local seed = feed(s, w, mk_info(100, 105))
  check("standoff: send from (100,105)", seed and seed.src == "repair_pill"
        and seed.mx == 100 and seed.my == 100, seed and seed.src)
  check("builder out: no send", feed(s, w, mk_info(100, 105, C.LGM_MOVING)) == nil)
  -- Cap 12: a standoff at 12 sends (the old cap was 11).
  reset()
  local w12 = make_world({ T, hostile(2, 100, 91) })
  -- A far standoff record set by hand, to test the cap.
  local s12 = mk_state(w12)
  s12._repair_standoff = { pkey = 100 * 256 + 100, mode = "standoff", mx = 100, my = 112 }
  check("standoff at 12: send", feed(s12, w12, mk_info(100, 112)) ~= nil)
  s12._repair_standoff.my = 113
  check("standoff at 13: no send", feed(s12, w12, mk_info(100, 113)) == nil)
  -- Push: the old rules (tank at 1 square sends).
  reset()
  WALK_FN = function() return -1 end
  local sp = mk_state(w)
  bpool.standoff_update(sp, w, mk_info(100, 115), 1000)
  check("push record", sp._repair_standoff and sp._repair_standoff.mode == "push")
  check("push: nav to the pill (nil)", RS.nav_target(sp, sp.goal) == nil)
  WALK_FN = nil
  check("push: send from 1 square as before", feed(sp, w, mk_info(100, 101)) ~= nil)
  -- Under-fire hold is unchanged: a fresh hit on the pill holds at the standoff.
  reset()
  local sh = mk_state(w)
  bpool.standoff_update(sh, w, mk_info(100, 115), 1000)
  T.last_hit_tick = 990
  check("under-fire hold still holds at the standoff", feed(sh, w, mk_info(100, 105)) == nil)
  T.last_hit_tick = nil
  if HAS_LOGS then
    _G.BRAIN_DEBUG_MODE = true   -- REPAIR_DISPATCH_CHECK is a debug-mode line
    feed(sh, w, mk_info(100, 105))
    _G.BRAIN_DEBUG_MODE = nil
    check("REPAIR_DISPATCH_CHECK has standoff=", log_has("REPAIR_DISPATCH_CHECK.*standoff=%(100,105%)") ~= nil)
  end
end

-- ── KEEL ─────────────────────────────────────────────────────────────────
print("KEEL (knob off)")
do
  reset()
  C.REPAIR_STANDOFF_ENABLED = false
  local w = make_world({ T, hostile(2, 100, 96) })
  local s = mk_state(w)
  s._repair_standoff = { pkey = 100 * 256 + 100, mode = "standoff", mx = 100, my = 105 }
  local r = bpool.standoff_update(s, w, mk_info(100, 115), 1000)
  check("off: no record", r == nil and s._repair_standoff == nil)
  check("off: no REPAIR_STANDOFF line", log_has("^REPAIR_STANDOFF ") == nil)
  check("off: nav to the pill", RS.nav_target(s, s.goal) == nil)
  s._repair_standoff = { pkey = 100 * 256 + 100, mode = "standoff", mx = 100, my = 105 }
  check("off: a left-over record is not active", RS.active(s, s.goal) == nil)
  s._repair_standoff = nil
  s._bp_seed = nil
  bpool.repair_feeder(s, w, mk_info(100, 101), 1000)
  check("off: send from 1 square (as before)", s._bp_seed ~= nil)
  s._bp_seed = nil
  bpool.repair_feeder(s, w, mk_info(100, 111), 1000)
  check("off: send from 11 (repair leash, as before)", s._bp_seed ~= nil)
  s._bp_seed = nil
  bpool.repair_feeder(s, w, mk_info(100, 112), 1000)
  check("off: no send from 12 (calm, as before)", s._bp_seed == nil)
  if HAS_LOGS then
    _G.BRAIN_DEBUG_MODE = true
    s._bp_seed = nil
    bpool.repair_feeder(s, w, mk_info(100, 101), 1000)
    _G.BRAIN_DEBUG_MODE = nil
    check("off: REPAIR_DISPATCH_CHECK logged", log_has("^REPAIR_DISPATCH_CHECK") ~= nil)
    check("off: REPAIR_DISPATCH_CHECK has no standoff=", log_has("standoff=") == nil)
  end
end

-- ── Steering hook ────────────────────────────────────────────────────────
print("steering")
do
  local ok, steer = pcall(require, "steering")
  if not ok then
    check("steering loads", false, steer)
  else
    reset()
    local w = make_world({ T, hostile(2, 100, 96) })
    local s = mk_state(w)
    bpool.standoff_update(s, w, mk_info(100, 115), 1000)
    local nx, ny = steer.repair_standoff_nav(s, s.goal)
    check("steering nav = standoff", nx == 100 and ny == 105, tostring(nx) .. "," .. tostring(ny))
    C.REPAIR_STANDOFF_ENABLED = false
    check("steering nav off = nil", steer.repair_standoff_nav(s, s.goal) == nil)
  end
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
