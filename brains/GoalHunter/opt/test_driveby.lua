-- =========================================================================
-- test_driveby.lua — standalone unit tests (2026-11-22) for the drive-by
-- shots at an enemy tank (steering.lua "Opportunistic shooting while
-- navigating" and init.lua's opportunistic tank shot).
--
-- DRIVEBY_REQUIRE_REACH: steering.shot_path_clear walks the shell at the
-- given sight, and with require_reach refuses a shell that bursts short of
-- the target. hostile_hit_reaches lets a hit on a hostile tank on the way
-- count as reaching it. lgm_kill_lag_near (2026-11-22): at sight 2-4 the
-- drive-by shells burst at 288-544 wu, short of a tank 500-640 wu away.
--
-- DRIVEBY_YIELD_LGM_KILL: kill_lgm.man_in_fire_range says when the kill-LGM
-- fire check has a hostile builder in range; the drive-by shots then hold
-- fire.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_driveby.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local bit = require('bitcompat')
local C   = require("constants")

_G.TERRAIN_MASK = 0x0F
_G.TERRAIN_MINE_FLAG = 0x40
_G.get_terrain = function(mx, my) return C.T_GRASS end

local U   = require("util")
local cpf = require("cpathfinder")
local KL  = require("kill_lgm")

-- Shell walk stub: every tile the centre line crosses, out to the burst of
-- the given sight (kill_lgm.shell_travel; 0 = the longest sight), or to the
-- first tank in `tanks` within 192 wu of the shell (a tank hit entry, here
-- always before the tank's own square).
local last_sight = nil
local function walk(ox, oy, tx, ty, sight, tanks)
  last_sight = sight
  local sl = (sight and sight > 0) and sight or 14
  local reach = KL.shell_travel(sl, KL.all_on())
  local dx, dy = tx - ox, ty - oy
  local d = math.sqrt(dx * dx + dy * dy)
  local out, seen = {}, {}
  if d == 0 then return out end
  for st = 0, reach, 16 do
    local x, y = ox + dx / d * st, oy + dy / d * st
    for _, t in ipairs(tanks or {}) do
      if math.abs(t.wx - x) < 192 and math.abs(t.wy - y) < 192 then
        out[#out + 1] = { mx = math.floor(x / 256), my = math.floor(y / 256),
                          hit_type = 1, hit_id = t.player_num }
        return out
      end
    end
    local mx, my = math.floor(x / 256), math.floor(y / 256)
    local k = my * 256 + mx
    if not seen[k] then seen[k] = true; out[#out + 1] = { mx = mx, my = my } end
  end
  return out
end
cpf.simulate_shot = function(ox, oy, tx, ty, _, sight) return walk(ox, oy, tx, ty, sight) end
cpf.simulate_shot_with_tanks = function(ox, oy, tx, ty, _, sight, tanks)
  return walk(ox, oy, tx, ty, sight, tanks)
end

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

local steer = require("steering")
local W = { pill_at = {}, base_at = {}, pills = {}, bases = {} }

-- Our tank at the centre of (20,20); the enemy tank at the centre of
-- (22,20), 512 wu east.
local OX, OY = 20 * 256 + 128, 20 * 256 + 128
local EMX, EMY = 22, 20
local EWX, EWY = U.m2w(EMX), U.m2w(EMY)     -- 512 wu east

print("\n-- 1. shot_path_clear: sight and require_reach (no tanks in view)")
do
  local info = { tankx = OX, tanky = OY, objects = {}, player_number = 0, gunrange = 2 }
  check("1a old call (longest sight, no reach): clear",
        steer.shot_path_clear(info, W, EWX, EWY, EMX, EMY) == true)
  check("1b old call walks at sight 0", last_sight == 0, last_sight)
  check("1c sight 2 (burst 288 wu), no require_reach: clear (nothing stops it)",
        steer.shot_path_clear(info, W, EWX, EWY, EMX, EMY, nil, nil, nil, nil, nil, 2) == true)
  check("1d sight 2 walked at sight 2", last_sight == 2, last_sight)
  check("1e sight 2 + require_reach: refused (bursts short of the tank)",
        steer.shot_path_clear(info, W, EWX, EWY, EMX, EMY, nil, nil, nil, true, nil, 2) == false)
  check("1f sight 4 (544 wu) + require_reach: reaches (22,20)",
        steer.shot_path_clear(info, W, EWX, EWY, EMX, EMY, nil, nil, nil, true, nil, 4) == true)
  check("1g sight 0 + require_reach: reaches",
        steer.shot_path_clear(info, W, EWX, EWY, EMX, EMY, nil, nil, nil, true, nil, 0) == true)
end

print("\n-- 2. shot_path_clear: a hostile tank hit on the way")
do
  local info = { tankx = OX, tanky = OY, player_number = 0, gunrange = 4,
                 objects = { { type = 0, x = EWX, y = EWY, idnum = 1, info = OBJECT_HOSTILE or 1 } } }
  _G.OBJECT_TANK = _G.OBJECT_TANK or 0
  _G.OBJECT_HOSTILE = _G.OBJECT_HOSTILE or 1
  info.objects[1].info = _G.OBJECT_HOSTILE
  check("2a require_reach, the shell meets the tank before its square: refused without hostile_hit_reaches",
        steer.shot_path_clear(info, W, EWX, EWY, EMX, EMY, nil, nil, nil, true, nil, 4) == false)
  check("2b with hostile_hit_reaches: reaches",
        steer.shot_path_clear(info, W, EWX, EWY, EMX, EMY, nil, nil, nil, true, nil, 4, true) == true)
  info.gunrange = 2
  check("2c sight 2 bursts before the tank, hostile_hit_reaches: refused",
        steer.shot_path_clear(info, W, EWX, EWY, EMX, EMY, nil, nil, nil, true, nil, 2, true) == false)
  -- An allied tank on the line still blocks.
  info.objects[1].info = 0
  check("2d an allied tank hit still blocks with hostile_hit_reaches",
        steer.shot_path_clear(info, W, EWX, EWY, EMX, EMY, nil, nil, nil, true, nil, 4, true) == false)
end

print("\n-- 3. the knob")
check("3a DRIVEBY_REQUIRE_REACH default on", C.DRIVEBY_REQUIRE_REACH == true)
check("3b KEEL off", C.PRESETS.keel.DRIVEBY_REQUIRE_REACH == false)
check("3c MAST off", C.PRESETS.mast.DRIVEBY_REQUIRE_REACH == false)
check("3d DRIVEBY_YIELD_LGM_KILL default on", C.DRIVEBY_YIELD_LGM_KILL == true)
check("3e KEEL off", C.PRESETS.keel.DRIVEBY_YIELD_LGM_KILL == false)
check("3f MAST off", C.PRESETS.mast.DRIVEBY_YIELD_LGM_KILL == false)

print("\n-- 4. kill_lgm.man_in_fire_range")
do
  local R = C.KILL_LGM_SHOOT_RANGE * 256
  local function st(lgms, peace)
    return { tick = 100, perc = { enemy_lgms = lgms }, peace = peace }
  end
  local info = { tankx = OX, tanky = OY, shells = 40, inboat = false }
  local man_in = { wx = OX + R - 1, wy = OY, idnum = 1 }
  local man_out = { wx = OX + R + 64, wy = OY, idnum = 1 }
  check("4a no builders seen: false", KL.man_in_fire_range(st({}), info) == false)
  check("4b no perception: false", KL.man_in_fire_range({}, info) == false)
  check("4c a builder inside KILL_LGM_SHOOT_RANGE: true",
        KL.man_in_fire_range(st({ man_in }), info) == true)
  check("4d a builder outside it: false", KL.man_in_fire_range(st({ man_out }), info) == false)
  check("4e Euclidean: a builder at (R*0.8, R*0.8) is out",
        KL.man_in_fire_range(st({ { wx = OX + R * 0.8, wy = OY + R * 0.8, idnum = 1 } }), info) == false)
  info.inboat = true
  check("4f in a boat: false", KL.man_in_fire_range(st({ man_in }), info) == false)
  info.inboat = false
  info.shells = C.SHELL_RESERVE
  check("4g shells at SHELL_RESERVE: false", KL.man_in_fire_range(st({ man_in }), info) == false)
  info.shells = 40
  local saved = C.PEACE_ENABLED
  C.PEACE_ENABLED = true
  check("4h spared by peace: false",
        KL.man_in_fire_range(st({ man_in }, { [1] = true }), info) == false)
  check("4i another man not spared: true",
        KL.man_in_fire_range(st({ man_in, { wx = OX, wy = OY + 256, idnum = 2 } }, { [1] = true }), info) == true)
  C.PEACE_ENABLED = saved
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
