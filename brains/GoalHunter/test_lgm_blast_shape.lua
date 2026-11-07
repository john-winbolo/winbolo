-- =========================================================================
-- test_lgm_blast_shape.lua -- standalone tests for the builder blast shape
-- model in util.lua (U.lgm_open_blast_catches and friends).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_lgm_blast_shape.lua
-- or with any lua on PATH.
--
-- 1. No rule sent, or lgm_blast_shape 0: the half-square circle, radius 128.
-- 2. lgm_blast_shape 1: the Mac Bolo box, |dx| < 128 and |dy| < 128.
-- 3. lgm_blast_shape 2: the circle of radius 144.
-- 4. LGM_BLAST_SHAPE_LIVE = false (KEEL): always the half-square circle.
-- 5. The perp form and the reach follow the same shape.
-- Every shape is checked at 127/128/129 and 143/144/145 straight across and
-- at 100 and 120 on the diagonal, the same points as the C unit test
-- tests/unit/test_lgm_blast_shape.c.
-- =========================================================================

package.path = "./?.lua;" .. package.path

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

local C = require("constants")
local U = require("util")

-- { dx, dy, { half, box, area } }
local ROWS = {
  { 127,   0, { true,  true,  true  } },
  { 128,   0, { true,  false, true  } },
  { 129,   0, { false, false, true  } },
  { 143,   0, { false, false, true  } },
  { 144,   0, { false, false, true  } },
  { 145,   0, { false, false, false } },
  { 100, 100, { false, true,  true  } },
  { 120, 120, { false, true,  false } },
}

local function check_shape(label, want_shape)
  for _, row in ipairs(ROWS) do
    for _, s in ipairs({ { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } }) do
      local dx, dy = row[1] * s[1], row[2] * s[2]
      local got = U.lgm_open_blast_catches(dx, dy)
      check(string.format("%s at (%d,%d)", label, dx, dy),
            got == row[3][want_shape + 1], got)
      if row[2] == 0 then
        got = U.lgm_open_blast_catches(dy, dx)
        check(string.format("%s at (%d,%d)", label, dy, dx),
              got == row[3][want_shape + 1], got)
      end
    end
  end
end

print("1. no rule / shape 0: half-square circle")
U.lgm_blast_shape_update({})
check("no rules table reads 0", U.lgm_blast_shape() == 0, U.lgm_blast_shape())
check_shape("no rule", 0)
U.lgm_blast_shape_update({ rules = { lgm_blast_shape = 0 } })
check_shape("shape 0", 0)
check("shape 0 reach 128", U.lgm_open_blast_reach() == 128, U.lgm_open_blast_reach())

print("2. shape 1: Mac Bolo box")
U.lgm_blast_shape_update({ rules = { lgm_blast_shape = 1 } })
check("reads 1", U.lgm_blast_shape() == 1, U.lgm_blast_shape())
check_shape("shape 1", 1)
check("shape 1 reach 128", U.lgm_open_blast_reach() == 128, U.lgm_open_blast_reach())

print("3. shape 2: Mac Bolo equivalent area circle")
U.lgm_blast_shape_update({ rules = { lgm_blast_shape = 2 } })
check("reads 2", U.lgm_blast_shape() == 2, U.lgm_blast_shape())
check_shape("shape 2", 2)
check("shape 2 reach 144", U.lgm_open_blast_reach() == 144, U.lgm_open_blast_reach())

print("4. KEEL: LGM_BLAST_SHAPE_LIVE = false")
C.LGM_BLAST_SHAPE_LIVE = false
U.lgm_blast_shape_update({ rules = { lgm_blast_shape = 1 } })
check("keel reads 0 under shape 1", U.lgm_blast_shape() == 0, U.lgm_blast_shape())
check_shape("keel under shape 1", 0)
C.LGM_BLAST_SHAPE_LIVE = true
U.lgm_blast_shape_update({ rules = { lgm_blast_shape = 7 } })
check("an unknown value reads 0", U.lgm_blast_shape() == 0, U.lgm_blast_shape())

print("5. perp form")
-- A shell flying east (hx 1, hy 0): the side offset is along y.
U.lgm_blast_shape_update({ rules = { lgm_blast_shape = 0 } })
check("shape 0 perp 128 kills", U.lgm_open_blast_catches_perp(128, 1, 0) == true)
check("shape 0 perp 129 spares", U.lgm_open_blast_catches_perp(129, 1, 0) == false)
U.lgm_blast_shape_update({ rules = { lgm_blast_shape = 1 } })
check("shape 1 perp 127 kills", U.lgm_open_blast_catches_perp(127, 1, 0) == true)
check("shape 1 perp 128 spares", U.lgm_open_blast_catches_perp(128, 1, 0) == false)
-- A shell flying on the diagonal: perp 160 is about 113 on each axis.
local d = math.sqrt(0.5)
check("shape 1 diagonal perp 160 kills",
      U.lgm_open_blast_catches_perp(160, d, d) == true)
check("shape 1 diagonal perp 182 spares",
      U.lgm_open_blast_catches_perp(182, d, d) == false)
U.lgm_blast_shape_update({ rules = { lgm_blast_shape = 2 } })
check("shape 2 perp 144 kills", U.lgm_open_blast_catches_perp(144, 1, 0) == true)
check("shape 2 perp 145 spares", U.lgm_open_blast_catches_perp(145, 1, 0) == false)

U.lgm_blast_shape_update({})
print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
