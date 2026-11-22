-- =========================================================================
-- test_haul_keep_lgm_kill.lua -- standalone unit tests (2026-11-22) for
-- C.HAUL_KEEP_LGM_KILL in goals.haul_flee_eval: while kill_lgm has an
-- enemy builder target close by with a clear line of sight, the haul fires
-- only for a shell really coming at the tank or for low armour. The
-- pill-cover stamp and the enemy-tank range do not fire it then.
-- Each case below has both of those triggers on (a pill covers the tank's
-- square, an enemy tank 5 squares away) and our builder dead, so the old
-- code fires the haul every time.
--   1. knobs: on by default, off in KEEL and MAST; the two distances.
--   2. 1 pill aboard: builder 5 squares away = no haul; 7 squares = haul.
--   3. 2 pills aboard: builder 3 squares away = no haul; 5 squares = haul.
--   4. a shell coming at the tank: haul (builder 2.2 squares away).
--   5. low armour: haul.
--   6. a wall between tank and builder: haul.
--   7. no kill_lgm target: haul.
--   8. knob off: haul (the old way).
-- No engine: a hand-made grass map, stubbed threat grid.
--
-- Run from this directory with the LuaJIT built beside the game:
--   <luajit> test_haul_keep_lgm_kill.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_haul_keep_lgm_kill.lua
-- Against older sources (to show the test fails there):
--   <luajit> test_haul_keep_lgm_kill.lua src=<dir with the old .lua files>
-- =========================================================================

local SRC
for _, a in ipairs(arg or {}) do
  local v = a:match("^src=(.+)$")
  if v then SRC = v end
end
package.path = (SRC and (SRC .. "/?.lua;") or "") .. "./?.lua;" .. package.path

if math.atan2 then
  local _a1, _a2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _a1(y) else return _a2(y, x) end end
end
package.loaded["print2"] = setmetatable({ set_tick = function() end }, { __call = function() end })
_G.print2 = _G.print2 or function() end
_G.OBJECT_TANK, _G.OBJECT_SHOT = 0, 1
_G.OBJECT_HOSTILE = _G.OBJECT_HOSTILE or 1
_G.OBJECT_NEUTRAL = _G.OBJECT_NEUTRAL or 4

local C      = require("constants")
local U      = require("util")
local threat = require("threat")
local G      = require("goals")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

-- ── Map and stubs ────────────────────────────────────────────────────────
local walls = {}
U.ttype = function(mx, my) return walls[my * 256 + mx] or C.T_GRASS end
U.ttype_peek = U.ttype
threat.pill_at = function() return 3 end   -- a pill covers every square

local TX, TY = 100 * 256 + 128, 100 * 256 + 128   -- the tank, square centre

-- state + info: our builder dead, an enemy tank 5 squares away (inside
-- FLEE_HAUL_TANK_RANGE), kill_lgm's target `d` squares east of the tank.
local function setup(carry, d, opts)
  opts = opts or {}
  local state = { tick = 42194, perc = { nearest_hostile_tank = { dist = 5 } } }
  if d then
    local wx, wy = TX + math.floor(d * 256), TY
    local lgm = { mx = bit.rshift(wx, 8), my = bit.rshift(wy, 8), wx = wx, wy = wy, idnum = 3 }
    state.pool_cache = { [13] = { goal = { kind = "kill_lgm", mx = lgm.mx, my = lgm.my,
                                           target_id = 3, _lgm_track = lgm } } }
  end
  local info = { tankx = TX, tanky = TY, direction = 0, speed = 0,
                 armour = opts.armour or 40, shells = 40, carried_pills = carry,
                 man_status = C.LGM_DEAD, objects = opts.objects or {} }
  return state, info
end

local function haul(carry, d, opts)
  local state, info = setup(carry, d, opts)
  local h = G.haul_flee_eval(state, info, 100, 100, { pills = {} })
  return h.triggered, h
end

-- ── 1. knobs ─────────────────────────────────────────────────────────────
print("1. knobs")
do
  local D = dofile((SRC and (SRC .. "/") or "") .. "constants.lua")
  check("HAUL_KEEP_LGM_KILL default true", D.HAUL_KEEP_LGM_KILL == true, D.HAUL_KEEP_LGM_KILL)
  check("keel false", D.PRESETS.keel.HAUL_KEEP_LGM_KILL == false)
  check("mast false", D.PRESETS.mast.HAUL_KEEP_LGM_KILL == false)
  check("HAUL_KEEP_LGM_KILL_TILES_ONE 6 (Andrew)", D.HAUL_KEEP_LGM_KILL_TILES_ONE == 6,
        D.HAUL_KEEP_LGM_KILL_TILES_ONE)
  check("HAUL_KEEP_LGM_KILL_TILES_MANY 4 (Andrew)", D.HAUL_KEEP_LGM_KILL_TILES_MANY == 4,
        D.HAUL_KEEP_LGM_KILL_TILES_MANY)
end
C.HAUL_KEEP_LGM_KILL = true
C.HAUL_KEEP_LGM_KILL_TILES_ONE  = C.HAUL_KEEP_LGM_KILL_TILES_ONE or 6
C.HAUL_KEEP_LGM_KILL_TILES_MANY = C.HAUL_KEEP_LGM_KILL_TILES_MANY or 4

if not G.haul_flee_eval then
  check("goals.haul_flee_eval exists", false, "nil")
  print(string.format("\n%d passed, %d failed", pass, fail))
  os.exit(1)
end

-- the old triggers are on in this set-up
do
  local _, h = haul(1, 5)
  check("set-up: haul level 1, pill stamp and tank range both on",
        h.level == 1.0 and (h.pill_at or 0) > 0 and (h.tank_dist or 99) <= (h.range or 0),
        string.format("level=%s pill_at=%s tank=%s range=%s", tostring(h.level),
          tostring(h.pill_at), tostring(h.tank_dist), tostring(h.range)))
end

-- ── 2. 1 pill aboard ─────────────────────────────────────────────────────
print("2. 1 pill aboard (6 squares)")
do
  local t, h = haul(1, 5)
  check("builder 5 squares away: no haul (kill the builder)", t == false, t)
  check("  lgm_keep set", h.lgm_keep == true, h.lgm_keep)
  t = haul(1, 7)
  check("builder 7 squares away: haul", t == true, t)
end

-- ── 3. 2 pills aboard ────────────────────────────────────────────────────
print("3. 2 pills aboard (4 squares)")
do
  local t = haul(2, 3)
  check("builder 3 squares away: no haul (kill the builder)", t == false, t)
  t = haul(2, 5)
  check("builder 5 squares away: haul", t == true, t)
end

-- ── 4. a shell coming at the tank ────────────────────────────────────────
-- A hostile shell 2 squares north of the tank, flying south (dir 128).
print("4. shell coming at the tank")
do
  local shell = { type = OBJECT_SHOT, info = OBJECT_HOSTILE, x = TX, y = TY - 2 * 256,
                  direction = 128, owner = 5 }
  local t, h = haul(1, 2.2, { objects = { shell } })
  check("builder 2.2 squares away, shell coming: haul", t == true, t)
  check("  source tank#5", h.shell_src == "tank#5", h.shell_src)
  -- the same shell flying away from the tank: no haul
  shell = { type = OBJECT_SHOT, info = OBJECT_HOSTILE, x = TX, y = TY - 2 * 256,
            direction = 0, owner = 5 }
  t = haul(1, 2.2, { objects = { shell } })
  check("shell flying away: no haul", t == false, t)
end

-- ── 5. low armour ────────────────────────────────────────────────────────
print("5. low armour")
do
  local t = haul(1, 2.2, { armour = C.ARMOUR_LOW })
  check("armour at ARMOUR_LOW: haul", t == true, t)
end

-- ── 6. a wall between tank and builder ───────────────────────────────────
print("6. wall between")
do
  walls[100 * 256 + 102] = C.T_BUILDING
  local t = haul(1, 3)
  walls[100 * 256 + 102] = nil
  check("wall between, builder 3 squares away: haul", t == true, t)
end

-- ── 7. no kill_lgm target ────────────────────────────────────────────────
print("7. no kill_lgm target")
do
  local t = haul(1, nil)
  check("no target: haul", t == true, t)
end

-- ── 8. knob off ──────────────────────────────────────────────────────────
print("8. knob off")
do
  C.HAUL_KEEP_LGM_KILL = false
  local t = haul(1, 2.2)
  C.HAUL_KEEP_LGM_KILL = true
  check("knob off, builder 2.2 squares away: haul (the old way)", t == true, t)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
