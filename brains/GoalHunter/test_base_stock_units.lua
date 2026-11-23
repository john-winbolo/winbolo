-- =========================================================================
-- test_base_stock_units.lua — standalone unit tests (2026-11-03) for the
-- unit of world.bases[id].obs_armour:
--   * EVENT_BASE_STOCK data is [baseIndex, armour, shells, mines] with RAW
--     base armour (0..90, server_sim_tick.c copies item.armour as it is)
--   * info.base.armour is raw armour / 5, rounded down (bases.c
--     basesGetBrainBaseItem), so a raw 65 reads 13
--   * every writer must store RAW armour, so both paths give the same
--     obs_armour and the same refuel verdict for the same base
--   * the refuel low-stock rules count armour in steps of five against
--     REFUEL_MIN_STOCK (5 steps = 25 raw); shells stay raw
--   * EVENT_BASE_UPDATE data is [baseIndex, owner] only, so it must not
--     write obs_armour / obs_shells / obs_mines / obs_tick
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_base_stock_units.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

_G.EVENT_BASE_UPDATE = _G.EVENT_BASE_UPDATE or 11
_G.EVENT_BASE_STOCK  = _G.EVENT_BASE_STOCK  or 19
_G.OBJECT_REFBASE    = _G.OBJECT_REFBASE    or 3
_G.OBJECT_PILLBOX    = _G.OBJECT_PILLBOX    or 2

local W = require("world")
local U = require("util")
local C = require("constants")

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

-- A world with one friendly base, #7 at (40,50), never stocked.
local function new_world()
  local b = { mx = 40, my = 50, owner = "friendly", health = 13,
              last_health = 13, last_seen = 0 }
  return { pills = {}, bases = { [7] = b }, base_at = {}, pill_at = {} }
end

local function stock_event(world, tick, idx, arm, sh, mi)
  local state = { tick = tick }
  local info = { events = {
    { type = EVENT_BASE_STOCK, data = { idx, arm, sh, mi } },
  } }
  W.process_events(world, info, state)
end

local function owner_event(world, tick, idx, owner)
  local state = { tick = tick }
  local info = { playernum = 0, events = {
    { type = EVENT_BASE_UPDATE, data = { idx, owner } },
  } }
  W.process_events(world, info, state)
end

-- The per-tick "closest base" item, as braincore.c hands it to Lua.
local function info_base(world, tick, idx, arm_div5, sh, mi)
  local info = { objects = {},
                 base = { idnum = idx, x = 40, y = 50,
                          armour = arm_div5, shells = sh, mines = mi } }
  W.update(world, info, tick)
end

-- ── EVENT_BASE_STOCK stores raw armour ───────────────────────────────────
do
  local w = new_world()
  stock_event(w, 100, 7, 65, 30, 10)
  local b = w.bases[7]
  check("BASE_STOCK: obs_armour is the raw 65", b.obs_armour == 65, b.obs_armour)
  check("BASE_STOCK: obs_shells is the raw 30", b.obs_shells == 30, b.obs_shells)
  check("BASE_STOCK: obs_mines is the raw 10", b.obs_mines == 10, b.obs_mines)
  check("BASE_STOCK: obs_tick is stamped", b.obs_tick == 100, b.obs_tick)
end

-- ── info.base (raw / 5) is stored back as raw ────────────────────────────
do
  local w = new_world()
  info_base(w, 100, 7, 13, 30, 10)
  local b = w.bases[7]
  check("info.base 13: obs_armour is raw 65, not 13", b.obs_armour == 65, b.obs_armour)
  check("info.base: obs_shells stays raw 30", b.obs_shells == 30, b.obs_shells)
end

-- ── an exact raw value from the event survives the /5 reading ────────────
do
  local w = new_world()
  stock_event(w, 100, 7, 64, 30, 10)
  info_base(w, 101, 7, 12, 30, 10)        -- 64 / 5 = 12
  check("raw 64 then info.base 12: obs_armour stays the exact 64",
        w.bases[7].obs_armour == 64, w.bases[7].obs_armour)
  info_base(w, 102, 7, 11, 30, 10)        -- the base gave armour away
  check("then info.base 11: obs_armour moves to 55",
        w.bases[7].obs_armour == 55, w.bases[7].obs_armour)
end

-- ── both writers give the same refuel verdict ────────────────────────────
-- Raw 20 armour is 4 steps, below REFUEL_MIN_STOCK (5): a tank that needs
-- armour only must skip the base (low_stock), whichever path wrote it.
do
  local w1 = new_world()
  stock_event(w1, 100, 7, 20, 40, 0)
  local w2 = new_world()
  info_base(w2, 100, 7, 4, 40, 0)
  local v1 = U.refuel_base_verdict(w1.bases[7], true, false, 110)
  local v2 = U.refuel_base_verdict(w2.bases[7], true, false, 110)
  check("raw 20 from BASE_STOCK is low_stock for an armour need", v1 == "low_stock", v1)
  check("info.base 4 is low_stock for an armour need", v2 == "low_stock", v2)
  check("both paths store the same obs_armour",
        w1.bases[7].obs_armour == w2.bases[7].obs_armour,
        tostring(w1.bases[7].obs_armour) .. " vs " .. tostring(w2.bases[7].obs_armour))
end

-- Raw 25 armour is 5 steps, at REFUEL_MIN_STOCK: the base is used, no markup.
do
  local w = new_world()
  stock_event(w, 100, 7, 25, 40, 0)
  local why, mult = U.refuel_base_verdict(w.bases[7], true, false, 110)
  check("raw 25 is not skipped for an armour need", why == nil, why)
  check("raw 25 has no low-stock markup", mult == 1.0, mult)
end

-- Raw 24 is 4 steps: not skipped when shells are needed too and the base
-- has them, but marked up once for the armour.
do
  local w = new_world()
  stock_event(w, 100, 7, 24, 40, 0)
  local why, mult, d = U.refuel_base_verdict(w.bases[7], true, true, 110)
  check("raw 24 with shells: not skipped", why == nil, why)
  check("raw 24 with shells: x REFUEL_LOW_ARMOUR_MULT",
        mult and math.abs(mult - C.REFUEL_LOW_ARMOUR_MULT) < 1e-9, mult)
  check("raw 24: the panel detail shows 4 steps and raw 24",
        d and d.obs_arm == 4 and d.obs_arm_raw == 24,
        d and (tostring(d.obs_arm) .. "/" .. tostring(d.obs_arm_raw)))
end

-- Raw 4 is 0 steps: a base holding only that is empty for an armour need,
-- the same as info.base.armour 0.
do
  local w = new_world()
  stock_event(w, 100, 7, 4, 0, 0)
  local why = U.refuel_base_verdict(w.bases[7], true, false, 110)
  check("raw 4 armour, 0 shells: empty for an armour need", why == "empty", why)
end

-- Shells are raw on both paths: 4 shells is low, 5 is not.
do
  local w = new_world()
  stock_event(w, 100, 7, 90, 4, 0)
  check("4 shells is low_stock for a shells need",
        U.refuel_base_verdict(w.bases[7], false, true, 110) == "low_stock",
        U.refuel_base_verdict(w.bases[7], false, true, 110))
  stock_event(w, 101, 7, 90, 5, 0)
  check("5 shells is used for a shells need",
        U.refuel_base_verdict(w.bases[7], false, true, 110) == nil,
        U.refuel_base_verdict(w.bases[7], false, true, 110))
end

-- ── EVENT_BASE_UPDATE carries no stock ───────────────────────────────────
do
  local w = new_world()
  stock_event(w, 100, 7, 64, 30, 10)
  owner_event(w, 200, 7, 0)
  local b = w.bases[7]
  check("BASE_UPDATE: obs_armour kept at 64", b.obs_armour == 64, b.obs_armour)
  check("BASE_UPDATE: obs_shells kept at 30", b.obs_shells == 30, b.obs_shells)
  check("BASE_UPDATE: obs_mines kept at 10", b.obs_mines == 10, b.obs_mines)
  check("BASE_UPDATE: obs_tick kept at 100", b.obs_tick == 100, b.obs_tick)
  check("BASE_UPDATE: the base is not read as empty",
        U.refuel_base_verdict(b, true, true, 210) == nil,
        U.refuel_base_verdict(b, true, true, 210))
end
do
  local w = new_world()
  owner_event(w, 200, 9, 0)                -- a base we have no record of
  local b = w.bases[9]
  check("BASE_UPDATE new base: a record is made", b ~= nil, b)
  check("BASE_UPDATE new base: no stock observation",
        b and b.obs_tick == nil and b.obs_armour == nil and b.obs_shells == nil,
        b and tostring(b.obs_tick))
end

-- ── the helpers ──────────────────────────────────────────────────────────
do
  local steps = U.base_armour_steps
  local back  = U.base_armour_raw_from_info
  check("helpers exist", steps ~= nil and back ~= nil, steps)
  if steps and back then
    check("steps(90) = 18", steps(90) == 18, steps(90))
    check("steps(24) = 4", steps(24) == 4, steps(24))
    check("steps(25) = 5", steps(25) == 5, steps(25))
    check("steps(nil) = 0", steps(nil) == 0, steps(nil))
    check("raw_from_info(18) = 90", back(18) == 90, back(18))
    check("raw_from_info(12, 64) keeps 64", back(12, 64) == 64, back(12, 64))
    check("raw_from_info(12, 70) = 60", back(12, 70) == 60, back(12, 70))
  end
end

print(string.format("test_base_stock_units: %d passed, %d failed", pass, fail))
