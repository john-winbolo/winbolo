-- =========================================================================
-- test_lgm_stranded.lua -- standalone unit tests (2026-11-21) for
-- lgm_bless.lua (C.LGM_STRANDED_BLESS_DISPATCH, C.LGM_STRANDED_RECHECK,
-- Andrew: "Yes"):
--   * the knobs are on by default and off in PRESETS.keel and PRESETS.mast
--   * blessed_order: the engine's blessX/blessY rule per order kind
--   * a builder on his own job square (the pill he just put down) is not
--     stranded; with the knob off he is (the old false alarm)
--   * a builder on a pill that is not his job square is stranded
--   * the stranded flag clears once the walk works again (recheck on), and
--     stays with the recheck off; it is checked only every 50 ticks
-- The walk sim here is a stub with the engine's first-step rule: speed 0 on
-- a solid square unless it is the blessed square. No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_lgm_stranded.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path
_G.print2 = _G.print2 or function() end
_G.BUILDMODE_FARM, _G.BUILDMODE_ROAD, _G.BUILDMODE_BUILD = 1, 2, 3
_G.BUILDMODE_PBOX, _G.BUILDMODE_MINE = 4, 5

local C  = require("constants")
local LB = require("lgm_bless")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

print("knobs")
check("bless on by default", C.LGM_STRANDED_BLESS_DISPATCH == true)
check("recheck on by default", C.LGM_STRANDED_RECHECK == true)
check("keel off", C.PRESETS.keel.LGM_STRANDED_BLESS_DISPATCH == false and C.PRESETS.keel.LGM_STRANDED_RECHECK == false)
check("mast off", C.PRESETS.mast.LGM_STRANDED_BLESS_DISPATCH == false and C.PRESETS.mast.LGM_STRANDED_RECHECK == false)

print("blessed_order (lgm.c lgmNewPrimaryRequest)")
local GRASS = 4
check("pill on grass: bless", LB.blessed_order(BUILDMODE_PBOX, GRASS, false) == true)
check("pill repair (pill there): bless", LB.blessed_order(BUILDMODE_PBOX, GRASS, true) == true)
check("pill on forest, no pill: tree order, no bless", LB.blessed_order(BUILDMODE_PBOX, C.T_FOREST, false) == false)
check("pill on forest with pill: bless", LB.blessed_order(BUILDMODE_PBOX, C.T_FOREST, true) == true)
check("wall on grass: bless", LB.blessed_order(BUILDMODE_BUILD, GRASS, false) == true)
check("wall on river (boat): bless", LB.blessed_order(BUILDMODE_BUILD, C.T_RIVER, false) == true)
check("wall on forest: tree order, no bless", LB.blessed_order(BUILDMODE_BUILD, C.T_FOREST, false) == false)
check("road on river: bless", LB.blessed_order(BUILDMODE_ROAD, C.T_RIVER, false) == true)
check("road on grass: no bless", LB.blessed_order(BUILDMODE_ROAD, GRASS, false) == false)
check("farm: no bless", LB.blessed_order(BUILDMODE_FARM, C.T_FOREST, false) == false)
check("mine: no bless", LB.blessed_order(BUILDMODE_MINE, GRASS, false) == false)

-- Walk stub: the man at square (124,118); SOLID[sq] = true is a pill square.
local SOLID = {}
local calls = {}
_G.cpf_lgm_travel_ticks = function(sx, sy, dx, dy, bx, by, maxt, stuck)
  local mx, my = math.floor(sx / 256), math.floor(sy / 256)
  calls[#calls + 1] = { bx = bx, by = by }
  if SOLID[my * 256 + mx] and not (bx == mx and by == my) then return -1 end
  return 120
end
local MX, MY = 124, 118
local function mkstate(dispatch)
  return { builder = {}, _lgm_dispatch = dispatch }
end
local info = { man_x = MX * 256 + 128, man_y = MY * 256 + 128,
               tankx = 118 * 256 + 128, tanky = 123 * 256 + 128 }
local function step(st, now, ibd)
  LB.update_stranded(st, info, MX, MY, 118, 123, 8, now, ibd or false)
end

print("own job square")
SOLID = { [MY * 256 + MX] = true }
local st = mkstate({ x = MX, y = MY, bless = true, pbox = true })
step(st, 100)
check("own pill square: not stranded", not st.lgm_stranded, st.lgm_stranded)
check("bless passed = job square", calls[#calls].bx == MX and calls[#calls].by == MY)
check("factors carry bless", st._lgm_stranded_factors.bless_mx == MX)
C.LGM_STRANDED_BLESS_DISPATCH = false
st = mkstate({ x = MX, y = MY, bless = true, pbox = true })
step(st, 100)
check("knob off: old false alarm (stranded)", st.lgm_stranded == true)
C.LGM_STRANDED_BLESS_DISPATCH = true

print("someone else's pill")
st = mkstate({ x = 130, y = 118, bless = true, pbox = true })   -- his job is elsewhere
step(st, 100)
check("pill not his job square: stranded", st.lgm_stranded == true)
check("no bless passed", calls[#calls].bx == 0 and calls[#calls].by == 0)
st = mkstate(nil)
step(st, 100)
check("no dispatch record: stranded", st.lgm_stranded == true)
st = mkstate({ x = MX, y = MY, bless = false })                 -- tree order
step(st, 100)
check("own square but an order the engine does not bless: stranded", st.lgm_stranded == true)

print("recheck")
st = mkstate({ x = 130, y = 118, bless = true })
step(st, 100)
check("stranded at t=100", st.lgm_stranded == true)
SOLID = {}                                   -- the pill under him is gone
step(st, 120)
check("not re-run before 50 ticks", st.lgm_stranded == true)
step(st, 150)
check("cleared at t=150 when the walk works", st.lgm_stranded == nil, st.lgm_stranded)
check("recheck noted in factors", st._lgm_stranded_factors.recheck_tick == 150
      and st._lgm_stranded_factors.recheck_ticks == 120)
SOLID = { [MY * 256 + MX] = true }
st = mkstate(nil); step(st, 100)
step(st, 150)
check("still stuck: flag stays", st.lgm_stranded == true)
st.lgm_walled = "on_pill"; SOLID = {}
step(st, 200)
check("walled: no recheck clear", st.lgm_stranded == true)
st.lgm_walled = nil
C.LGM_STRANDED_RECHECK = false
step(st, 250)
check("recheck off: flag stays (old way)", st.lgm_stranded == true)
C.LGM_STRANDED_RECHECK = true

print("build dispatch carve-out kept")
SOLID = { [MY * 256 + MX] = true }
st = mkstate(nil)
step(st, 100, true)
check("in_build_dispatch: no first check", not st.lgm_stranded and st._lgm_stranded_factors.build_suppress == true)

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
