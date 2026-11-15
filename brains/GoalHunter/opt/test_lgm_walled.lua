-- =========================================================================
-- test_lgm_walled.lua -- standalone unit tests for goals.lgm_walled_kind,
-- the check behind knob C.LGM_WALLED_KILL (2026-11-14): is our own builder
-- walled in, so that no walk can bring him home?
--   "on_wall": he stands on a BUILDING or HALFBUILDING square that is not
--              his blessed (errand) square
--   "prison":  every square he can walk to is shut in by walls only, and
--              the tank is not inside
--   nil:       anything else (open ground, a hole, water, the tank inside)
-- No engine: get_terrain reads a table, the map is grass unless set.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_lgm_walled.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local bit = require("bitcompat")
local C   = require("constants")

local TMAP = {}
_G.TERRAIN_MASK = 0x0F
_G.TERRAIN_MINE_FLAG = 0x40
_G.get_terrain = function(mx, my) return TMAP[my * 256 + mx] or C.T_GRASS end

local G = require("goals")

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

local function clear() TMAP = {} end
local function set(mx, my, tt) TMAP[my * 256 + mx] = tt end
local function ring(mx, my, tt)
  for dy = -1, 1 do
    for dx = -1, 1 do
      if dx ~= 0 or dy ~= 0 then set(mx + dx, my + dy, tt) end
    end
  end
end

-- Builder at the middle of square (mx,my); tank at (tmx,tmy).
local function kind(mx, my, tmx, tmy, disp)
  local state = { _lgm_dispatch = disp }
  local info = { man_x = mx * 256 + 128, man_y = my * 256 + 128 }
  return G.lgm_walled_kind(state, info, tmx, tmy)
end

print("on a wall square")
clear(); set(120, 120, C.T_BUILDING)
check("BUILDING under him = on_wall", kind(120, 120, 126, 126) == "on_wall",
      kind(120, 120, 126, 126))
clear(); set(120, 120, C.T_HALFBUILD)
check("HALFBUILDING under him = on_wall", kind(120, 120, 126, 126) == "on_wall",
      kind(120, 120, 126, 126))
clear(); set(120, 120, C.T_BUILDING)
check("his blessed errand square is not walled",
      kind(120, 120, 126, 126, { x = 120, y = 120 }) == nil,
      kind(120, 120, 126, 126, { x = 120, y = 120 }))

print("prison")
clear(); ring(120, 120, C.T_BUILDING)
check("ring of 8 walls = prison", kind(120, 120, 126, 126) == "prison",
      kind(120, 120, 126, 126))
clear(); ring(120, 120, C.T_HALFBUILD)
check("ring of half walls = prison", kind(120, 120, 126, 126) == "prison",
      kind(120, 120, 126, 126))
clear(); ring(120, 120, C.T_BUILDING); set(121, 120, C.T_RUBBLE)
check("ring with a rubble hole = nil", kind(120, 120, 126, 126) == nil,
      kind(120, 120, 126, 126))
clear(); ring(120, 120, C.T_BUILDING); set(121, 120, C.T_RIVER)
check("ring with a river square = nil (not walls only)",
      kind(120, 120, 126, 126) == nil, kind(120, 120, 126, 126))
clear(); ring(120, 120, C.T_BUILDING)
check("tank inside the ring = nil", kind(120, 120, 120, 120) == nil,
      kind(120, 120, 120, 120))
-- 4-way walk: the corners of a ring do not matter, a ring of the four
-- side squares is enough to shut him in.
clear()
set(121, 120, C.T_BUILDING); set(119, 120, C.T_BUILDING)
set(120, 121, C.T_BUILDING); set(120, 119, C.T_BUILDING)
check("four side walls (no corners) = prison", kind(120, 120, 126, 126) == "prison",
      kind(120, 120, 126, 126))
-- A bigger room: a 3x3 floor inside a 5x5 ring of walls.
clear()
for y = 118, 122 do
  for x = 118, 122 do
    if x == 118 or x == 122 or y == 118 or y == 122 then set(x, y, C.T_BUILDING) end
  end
end
check("3x3 room in walls = prison", kind(120, 120, 126, 126) == "prison",
      kind(120, 120, 126, 126))
check("tank in the same room = nil", kind(120, 120, 121, 121) == nil,
      kind(120, 120, 121, 121))

print("open ground")
clear()
check("open grass = nil", kind(120, 120, 126, 126) == nil, kind(120, 120, 126, 126))
clear(); set(121, 120, C.T_BUILDING)
check("one wall beside him = nil", kind(120, 120, 126, 126) == nil,
      kind(120, 120, 126, 126))
clear()
check("builder in the tank (0,0) = nil",
      G.lgm_walled_kind({}, { man_x = 0, man_y = 0 }, 126, 126) == nil)

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
