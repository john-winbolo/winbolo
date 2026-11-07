-- =========================================================================
-- test_lgm_blast_shape.lua -- standalone tests for the builder blast shape
-- model in util.lua (U.lgm_open_blast_catches and friends).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_lgm_blast_shape.lua
-- or with any lua on PATH.
--
-- The rules reach the model through live_physics.apply (group BLAST), as
-- they do in Brain.think.
-- 1. No rule sent, or lgm_blast_shape 0: the half-square circle, radius 128.
-- 2. lgm_blast_shape 1: the Mac Bolo box, |dx| < 128 and |dy| < 128.
-- 3. lgm_blast_shape 2: the circle of radius 144.
-- 4. LIVE_PHYSICS = false (KEEL) or LIVE_PHYSICS_BLAST = false: always the
--    half-square circle.
-- 5. The perp form and the reach follow the same shape.
-- 6. danger.lgm_shell_gate, the builder pool's shell check, follows the
--    shape: one shell that ends its life next to the man, on grass.
-- 7. pill_range under a shape's reach stops every shape there, in the
--    point test, the perp form, the reach and the shell check.
-- Every shape is checked at 127/128/129 and 143/144/145 straight across and
-- at 100 and 120 on the diagonal, the same points as the C unit test
-- tests/unit/test_lgm_blast_shape.c.
-- =========================================================================

package.path = "./?.lua;" .. package.path

-- print2 stand-in, installed before any module loads (danger.lua needs it).
package.loaded["print2"] = setmetatable({ set_tick = function() end },
  { __call = function() end })

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

-- Send these rules (nil: no rules table), as one think would.
local function rules(r)
  U.live_physics.apply(r and { rules = r } or {})
end

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
rules(nil)
check("no rules table reads 0", U.lgm_blast_shape() == 0, U.lgm_blast_shape())
check_shape("no rule", 0)
rules({ lgm_blast_shape = 0 })
check_shape("shape 0", 0)
check("shape 0 reach 128", U.lgm_open_blast_reach() == 128, U.lgm_open_blast_reach())
check("classic pill_range limit 2048", U.lgm_open_blast_limit() == 2048,
      U.lgm_open_blast_limit())

print("2. shape 1: Mac Bolo box")
rules({ lgm_blast_shape = 1 })
check("reads 1", U.lgm_blast_shape() == 1, U.lgm_blast_shape())
check_shape("shape 1", 1)
check("shape 1 reach 128", U.lgm_open_blast_reach() == 128, U.lgm_open_blast_reach())

print("3. shape 2: Mac Bolo equivalent area circle")
rules({ lgm_blast_shape = 2 })
check("reads 2", U.lgm_blast_shape() == 2, U.lgm_blast_shape())
check_shape("shape 2", 2)
check("shape 2 reach 144", U.lgm_open_blast_reach() == 144, U.lgm_open_blast_reach())

print("4. KEEL: LIVE_PHYSICS = false, and LIVE_PHYSICS_BLAST = false")
C.LIVE_PHYSICS = false
rules({ lgm_blast_shape = 1, pill_range = 100 })
check("keel reads 0 under shape 1", U.lgm_blast_shape() == 0, U.lgm_blast_shape())
check("keel keeps the 2048 limit", U.lgm_open_blast_limit() == 2048,
      U.lgm_open_blast_limit())
check_shape("keel under shape 1", 0)
C.LIVE_PHYSICS = true
C.LIVE_PHYSICS_BLAST = false
rules({ lgm_blast_shape = 1, pill_range = 100 })
check("BLAST off reads 0 under shape 1", U.lgm_blast_shape() == 0, U.lgm_blast_shape())
check("BLAST off keeps the 2048 limit", U.lgm_open_blast_limit() == 2048,
      U.lgm_open_blast_limit())
check_shape("BLAST off under shape 1", 0)
C.LIVE_PHYSICS_BLAST = true
rules({ lgm_blast_shape = 7 })
check("an unknown value reads 0", U.lgm_blast_shape() == 0, U.lgm_blast_shape())

print("5. perp form")
-- A shell flying east (hx 1, hy 0): the side offset is along y.
rules({ lgm_blast_shape = 0 })
check("shape 0 perp 128 kills", U.lgm_open_blast_catches_perp(128, 1, 0) == true)
check("shape 0 perp 129 spares", U.lgm_open_blast_catches_perp(129, 1, 0) == false)
rules({ lgm_blast_shape = 1 })
check("shape 1 perp 127 kills", U.lgm_open_blast_catches_perp(127, 1, 0) == true)
check("shape 1 perp 128 spares", U.lgm_open_blast_catches_perp(128, 1, 0) == false)
-- A shell flying on the diagonal: perp 160 is about 113 on each axis.
local d = math.sqrt(0.5)
check("shape 1 diagonal perp 160 kills",
      U.lgm_open_blast_catches_perp(160, d, d) == true)
check("shape 1 diagonal perp 182 spares",
      U.lgm_open_blast_catches_perp(182, d, d) == false)
rules({ lgm_blast_shape = 2 })
check("shape 2 perp 144 kills", U.lgm_open_blast_catches_perp(144, 1, 0) == true)
check("shape 2 perp 145 spares", U.lgm_open_blast_catches_perp(145, 1, 0) == false)

print("6. danger.lgm_shell_gate")
-- The man stays on the tank's square (the walk stand-in takes no step), and
-- one of our own shells (so our hull does not stop it) flies north and ends
-- its life on its first tick, dx, dy from him, over grass.
local danger = require("danger")
_G.OBJECT_SHOT = _G.OBJECT_SHOT or 1
_G.cpf_lgm_walk_path = function() return 0 end
U.ttype = function() return C.T_GRASS end
local TX, TY = 100 * 256 + 128, 100 * 256 + 128
local function gate_kills(dx, dy)
  local vy = -U.bcos_f(0) * C.SHELL_SPEED
  local info = {
    tankx = TX, tanky = TY, player_number = 0,
    objects = { { type = OBJECT_SHOT, owner = 0, angle = 0, life = 1,
                  x = TX + dx, y = TY + dy - vy } },
  }
  local hit = danger.lgm_shell_gate({}, info, 103, 100)
  return hit ~= nil and hit.how == "expiry"
end
-- { dx, dy, { half, box, area } }
local GATE_ROWS = {
  { 127,   0, { true,  true,  true  } },
  { 130,   0, { false, false, true  } },
  { 100, 100, { false, true,  true  } },
  { 110, 110, { false, true,  false } },
  { 150,   0, { false, false, false } },
}
local function check_gate(label, want_shape)
  for _, row in ipairs(GATE_ROWS) do
    local got = gate_kills(row[1], row[2])
    check(string.format("gate %s at (%d,%d)", label, row[1], row[2]),
          got == row[3][want_shape + 1], got)
  end
end
rules(nil)
check_gate("no rule", 0)
for shape = 0, 2 do
  rules({ lgm_blast_shape = shape })
  check_gate("shape " .. shape, shape)
end
C.LIVE_PHYSICS = false
rules({ lgm_blast_shape = 1 })
check_gate("keel under shape 1", 0)
C.LIVE_PHYSICS = true

print("7. pill_range under the reach")
-- pill_range 100: every shape is the circle of radius 100. (100,0) and
-- (70,70) (99.0 off) die; (101,0) and (71,71) (100.4 off) live.
for shape = 0, 2 do
  rules({ lgm_blast_shape = shape, pill_range = 100 })
  local l = "shape " .. shape .. " pill_range 100"
  check(l .. " limit 100", U.lgm_open_blast_limit() == 100, U.lgm_open_blast_limit())
  check(l .. " (100,0) kills", U.lgm_open_blast_catches(100, 0) == true)
  check(l .. " (0,-100) kills", U.lgm_open_blast_catches(0, -100) == true)
  check(l .. " (101,0) spares", U.lgm_open_blast_catches(101, 0) == false)
  check(l .. " (70,70) kills", U.lgm_open_blast_catches(70, 70) == true)
  check(l .. " (71,71) spares", U.lgm_open_blast_catches(71, -71) == false)
  check(l .. " perp 100 kills", U.lgm_open_blast_catches_perp(100, 1, 0) == true)
  check(l .. " perp 101 spares", U.lgm_open_blast_catches_perp(101, 1, 0) == false)
  check(l .. " diagonal perp 101 spares",
        U.lgm_open_blast_catches_perp(101, d, d) == false)
  check(l .. " reach 100", U.lgm_open_blast_reach() == 100, U.lgm_open_blast_reach())
  check(l .. " gate (100,0) kills", gate_kills(100, 0) == true)
  check(l .. " gate (110,0) spares", gate_kills(110, 0) == false)
end
-- pill_range 160: the half-square circle and the 144 circle are whole; the
-- box loses its corners past 160.
rules({ lgm_blast_shape = 1, pill_range = 160 })
check("box pill_range 160 (110,110) kills (155.6 off)",
      U.lgm_open_blast_catches(110, 110) == true)
check("box pill_range 160 (120,120) spares (169.7 off)",
      U.lgm_open_blast_catches(120, 120) == false)
check("box pill_range 160 reach 128", U.lgm_open_blast_reach() == 128,
      U.lgm_open_blast_reach())
rules({ lgm_blast_shape = 2, pill_range = 160 })
check_shape("shape 2 pill_range 160", 2)
rules({ lgm_blast_shape = 0, pill_range = 160 })
check_shape("shape 0 pill_range 160", 0)

rules(nil)
print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
