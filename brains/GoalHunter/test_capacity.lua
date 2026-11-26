-- =========================================================================
-- test_capacity.lua — standalone unit tests for Brain.capacity
-- (capacity.lua, knobs C.CAPACITY_K and C.CAPACITY_REF_1T). No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_capacity.lua
-- or with any lua on PATH. The same file is in opt/, run the same way there.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")
local cap = require("capacity").capacity

local failures = 0
local function check(name, got, want)
  if got == want then
    print("PASS  " .. name)
  else
    failures = failures + 1
    print("FAIL  " .. name .. ": got " .. tostring(got) .. ", want " .. tostring(want))
  end
end

local function machine(physical, speed1t)
  return {
    cpu_name = "test", logical_cores = physical * 2, physical_cores = physical,
    speed_1t = speed1t, speed_all = speed1t * physical, scaling = physical,
    worker_threads = physical, on_battery = false, version = "test",
  }
end

check("knob CAPACITY_K is 1.5", C.CAPACITY_K, 1.5)
check("knob CAPACITY_REF_1T is 25000", C.CAPACITY_REF_1T, 25000)

-- The three bench machines (1-thread scores from the 2 s test).
check("Ryzen 5 5600: 6 x 31000 / 25000 x 1.5 = 11.16 -> 11", cap(machine(6, 31000)), 11)
check("Ryzen 5 5600: 6 x 33500 / 25000 x 1.5 = 12.06 -> 12", cap(machine(6, 33500)), 12)
check("i5-6600: 4 x 25000 / 25000 x 1.5 = 6", cap(machine(4, 25000)), 6)
check("i5-3317U: 2 x 16000 / 25000 x 1.5 = 1.92 -> 1", cap(machine(2, 16000)), 1)

-- Clamps.
check("very slow core clamps up to 1", cap(machine(1, 1000)), 1)
check("very fast machine clamps down to 16", cap(machine(64, 50000)), 16)

-- No answer (the engine then uses its default).
check("no speed test result gives nil", cap(machine(4, 0)), nil)
check("no physical cores gives nil", cap(machine(0, 25000)), nil)
check("no machine table gives nil", cap(nil, nil), nil)
check("measured is not needed", cap(machine(4, 25000), nil), 6)

-- CAPACITY_K scales the answer.
local k = C.CAPACITY_K
C.CAPACITY_K = 0.5
check("CAPACITY_K 0.5 gives 8 x 0.5 = 4", cap(machine(8, 25000)), 4)
C.CAPACITY_K = k

if failures > 0 then
  print(failures .. " FAILED")
  os.exit(1)
end
print("all passed")
