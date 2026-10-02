-- =========================================================================
-- test_turtle_base.lua — standalone unit tests for the turtle base knobs
-- (TURTLE_BASE_* in constants.lua, 2026-10-02):
--   * goals.turtle_far_mult_of     — x min(2^(d/DOUBLE_TILES), MULT_CAP)
--   * goals.turtle_far_search_cap  — the ring search stops where the cap hits
--   * goals.turtle_far_ring_d      — Chebyshev ring search to influence > 0
--   * goals.turtle_far             — per-bot cache, turtle gate, no-ground x1
--   * strategy.turtle_base_run_end — ceil(FRAC x total) taken bases
-- Each knob is also checked at its PRESETS.keel value (the old behaviour).
-- No engine: cpathfinder.influence_at is replaced by a Lua table.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_turtle_base.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C   = require("constants")
local cpf = require("cpathfinder")
local G   = require("goals")
local S   = require("strategy")

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
local function near(a, b) return math.abs(a - b) < 1e-9 end

-- Save and restore the knobs so the order of the sections does not matter.
local KNOBS = { "PILL_PLACE_TURTLE", "TURTLE_BASE_RUN_END_FRAC",
                "TURTLE_BASE_DOUBLE_TILES", "TURTLE_BASE_MULT_CAP",
                "TURTLE_BASE_D_REFRESH_TICKS" }
local saved = {}
for _, k in ipairs(KNOBS) do saved[k] = C[k] end
local function defaults()
  for _, k in ipairs(KNOBS) do C[k] = saved[k] end
  C.PILL_PLACE_TURTLE = true
end
local function keel()
  defaults()
  for k, v in pairs(C.PRESETS.keel) do
    if k:match("^TURTLE_BASE_") then C[k] = v end
  end
end

print("-- defaults and keel values")
check("default TURTLE_BASE_RUN_END_FRAC = 0.25", saved.TURTLE_BASE_RUN_END_FRAC == 0.25, saved.TURTLE_BASE_RUN_END_FRAC)
check("default TURTLE_BASE_DOUBLE_TILES = 8", saved.TURTLE_BASE_DOUBLE_TILES == 8, saved.TURTLE_BASE_DOUBLE_TILES)
check("default TURTLE_BASE_MULT_CAP = 16", saved.TURTLE_BASE_MULT_CAP == 16, saved.TURTLE_BASE_MULT_CAP)
check("keel TURTLE_BASE_RUN_END_FRAC = 0", C.PRESETS.keel.TURTLE_BASE_RUN_END_FRAC == 0, C.PRESETS.keel.TURTLE_BASE_RUN_END_FRAC)
check("keel TURTLE_BASE_DOUBLE_TILES = 0", C.PRESETS.keel.TURTLE_BASE_DOUBLE_TILES == 0, C.PRESETS.keel.TURTLE_BASE_DOUBLE_TILES)
check("keel TURTLE_BASE_MULT_CAP = 1", C.PRESETS.keel.TURTLE_BASE_MULT_CAP == 1, C.PRESETS.keel.TURTLE_BASE_MULT_CAP)

print("-- multiplier values (DOUBLE_TILES 8, CAP 16)")
defaults()
check("d=0  -> x1",  near(G.turtle_far_mult_of(0), 1),  G.turtle_far_mult_of(0))
check("d=8  -> x2",  near(G.turtle_far_mult_of(8), 2),  G.turtle_far_mult_of(8))
check("d=16 -> x4",  near(G.turtle_far_mult_of(16), 4), G.turtle_far_mult_of(16))
check("d=4  -> x1.414", near(G.turtle_far_mult_of(4), math.sqrt(2)), G.turtle_far_mult_of(4))
check("d=40 -> capped at x16 (2^5 = 32 uncapped)", near(G.turtle_far_mult_of(40), 16), G.turtle_far_mult_of(40))
check("d=32 -> exactly the cap x16", near(G.turtle_far_mult_of(32), 16), G.turtle_far_mult_of(32))
check("d=nil (no ground of our own) -> x1", G.turtle_far_mult_of(nil) == 1, G.turtle_far_mult_of(nil))
check("search cap = 8 x log2(16) = 32", G.turtle_far_search_cap() == 32, G.turtle_far_search_cap())

print("-- cap")
C.TURTLE_BASE_MULT_CAP = 4
check("CAP 4: d=40 -> x4", near(G.turtle_far_mult_of(40), 4), G.turtle_far_mult_of(40))
check("CAP 4: d=8 -> x2 (under the cap)", near(G.turtle_far_mult_of(8), 2), G.turtle_far_mult_of(8))
check("CAP 4: search cap = 16", G.turtle_far_search_cap() == 16, G.turtle_far_search_cap())
C.TURTLE_BASE_MULT_CAP = 1e6
check("huge CAP: search cap held at 64", G.turtle_far_search_cap() == 64, G.turtle_far_search_cap())

print("-- keel no-op")
keel()
for _, d in ipairs({ 0, 8, 16, 40 }) do
  check(string.format("keel: d=%d -> x1", d), G.turtle_far_mult_of(d) == 1, G.turtle_far_mult_of(d))
end
check("keel: search cap 0 (no search at all)", G.turtle_far_search_cap() == 0, G.turtle_far_search_cap())
defaults()
C.TURTLE_BASE_MULT_CAP = 1
check("CAP 1 alone: d=40 -> x1", G.turtle_far_mult_of(40) == 1, G.turtle_far_mult_of(40))

print("-- ring search (Chebyshev)")
defaults()
local grid = {}
local function inf_at(x, y) return grid[y * 256 + x] or 0 end
local function set(x, y, v) grid[y * 256 + x] = v end
set(100, 100, 50)
check("on a positive tile -> 0", G.turtle_far_ring_d(100, 100, 32, inf_at) == 0, G.turtle_far_ring_d(100, 100, 32, inf_at))
check("diagonal 5 away -> 5 (Chebyshev, not Manhattan 10)", G.turtle_far_ring_d(105, 105, 32, inf_at) == 5, G.turtle_far_ring_d(105, 105, 32, inf_at))
check("straight 8 away -> 8", G.turtle_far_ring_d(100, 108, 32, inf_at) == 8, G.turtle_far_ring_d(100, 108, 32, inf_at))
check("40 away, search cap 32 -> 32 (saturates)", G.turtle_far_ring_d(140, 100, 32, inf_at) == 32, G.turtle_far_ring_d(140, 100, 32, inf_at))
set(100, 100, -20)
check("negative tile is not ours -> cap", G.turtle_far_ring_d(100, 102, 32, inf_at) == 32, G.turtle_far_ring_d(100, 102, 32, inf_at))
grid = {}
set(3, 0, 10)
check("map corner (0,0): off-map cells skipped, 3 -> 3", G.turtle_far_ring_d(0, 0, 32, inf_at) == 3, G.turtle_far_ring_d(0, 0, 32, inf_at))
set(250, 255, 10)
check("map corner (255,255): 5 -> 5", G.turtle_far_ring_d(255, 255, 32, inf_at) == 5, G.turtle_far_ring_d(255, 255, 32, inf_at))
grid = {}
set(110, 100, 10)
check("upper bound 6 stops the search early -> 6", G.turtle_far_ring_d(100, 100, 32, inf_at, 6) == 6, G.turtle_far_ring_d(100, 100, 32, inf_at, 6))
check("a hit inside the bound still wins -> 10 < bound 12", G.turtle_far_ring_d(100, 100, 32, inf_at, 12) == 10, G.turtle_far_ring_d(100, 100, 32, inf_at, 12))

print("-- turtle_far (cache, gates)")
defaults()
local real_inf = cpf.influence_at
grid = {}
local calls = 0
cpf.influence_at = function(x, y) calls = calls + 1; return grid[y * 256 + x] or 0 end
-- Our base #1 at (50,50) stamps a disc of radius 2; base #2 neutral 10 tiles
-- east, base #3 hostile 40 tiles east.
for dx = -2, 2 do for dy = -2, 2 do set(50 + dx, 50 + dy, 100) end end
local world = { bases = {
  [1] = { mx = 50, my = 50, owner = "friendly" },
  [2] = { mx = 60, my = 50, owner = "neutral" },
  [3] = { mx = 90, my = 50, owner = "hostile" },
}, pills = {} }
local st = { tick = 1000 }
local d1, m1 = G.turtle_far(st, world, 1, world.bases[1])
check("our own base: d=0, x1", d1 == 0 and m1 == 1, tostring(d1) .. " x" .. tostring(m1))
local d2, m2 = G.turtle_far(st, world, 2, world.bases[2])
check("10 east, disc edge at 52: d=8, x2", d2 == 8 and near(m2, 2), tostring(d2) .. " x" .. tostring(m2))
local d3, m3 = G.turtle_far(st, world, 3, world.bases[3])
check("40 east (38 to the disc): d=32 search cap, x16", d3 == 32 and near(m3, 16), tostring(d3) .. " x" .. tostring(m3))
local c0 = calls
G.turtle_far(st, world, 2, world.bases[2])
st.tick = 1049
G.turtle_far(st, world, 3, world.bases[3])
check("cached: no influence reads within 50 ticks", calls == c0, calls - c0)
-- Our ground grows next to base #2: the cache still answers until it expires.
set(58, 50, 100)
st.tick = 1049
local d2b = G.turtle_far(st, world, 2, world.bases[2])
check("tick 1049: still the cached d=8", d2b == 8, d2b)
st.tick = 1050
local d2c = G.turtle_far(st, world, 2, world.bases[2])
check("tick 1050: recomputed d=2", d2c == 2, d2c)
-- No ground at all: no friendly base, no placed live friendly pill.
local world0 = { bases = {
  [2] = { mx = 60, my = 50, owner = "neutral" },
  [3] = { mx = 90, my = 50, owner = "hostile" },
}, pills = { [1] = { mx = 70, my = 70, owner = "friendly", health = 15, in_tank = true } } }
local st0 = { tick = 10 }
local dn, mn = G.turtle_far(st0, world0, 3, world0.bases[3])
check("no ground (only a pill riding in the tank): d=nil, x1", dn == nil and mn == 1, tostring(dn) .. " x" .. tostring(mn))
-- Turtle off: x1 and no search, whatever the knobs say.
C.PILL_PLACE_TURTLE = false
calls = 0
local doff, moff = G.turtle_far({ tick = 5 }, world, 3, world.bases[3])
check("turtle off: d=nil, x1, no influence reads", doff == nil and moff == 1 and calls == 0, tostring(doff) .. " x" .. tostring(moff) .. " reads=" .. calls)
-- Keel: x1 and no search.
keel()
calls = 0
local dk, mk = G.turtle_far({ tick = 5 }, world, 3, world.bases[3])
check("keel: d=nil, x1, no influence reads", dk == nil and mk == 1 and calls == 0, tostring(dk) .. " x" .. tostring(mk) .. " reads=" .. calls)
cpf.influence_at = real_inf

print("-- breakdown chip")
defaults()
local chip, det = G.turtle_far_text(8, 2)
check("chip names d and the multiplier", chip == " x turtle_far{d=8,x2.00}", chip)
check("detail names both knobs", det:find("TURTLE_BASE_DOUBLE_TILES", 1, true) ~= nil and det:find("TURTLE_BASE_MULT_CAP", 1, true) ~= nil, det)
local chip2 = G.turtle_far_text(32, 16)
check("chip at the search cap reads d>=32", chip2 == " x turtle_far{d=>=32,x16.00}", chip2)
check("no term -> empty chip", G.turtle_far_text(nil, 1) == "", G.turtle_far_text(nil, 1))

print("-- base-run end threshold")
defaults()
local function need_at(total, neutral)
  local taken, need = S.turtle_base_run_end(total, neutral)
  return taken, need
end
local t3, n3 = need_at(16, 13)
check("16 bases, 3 taken: need 4, not yet", n3 == 4 and t3 == 3 and not (t3 >= n3), tostring(t3) .. "/" .. tostring(n3))
local t4, n4 = need_at(16, 12)
check("16 bases, 4 taken: need 4, ends", n4 == 4 and t4 == 4 and t4 >= n4, tostring(t4) .. "/" .. tostring(n4))
local _, n10 = need_at(10, 10)
check("10 bases: need ceil(2.5) = 3", n10 == 3, n10)
local _, n1 = need_at(1, 1)
check("1 base: need 1", n1 == 1, n1)
local _, n0 = need_at(0, 0)
check("0 bases (cold start): off", n0 == nil, n0)
C.TURTLE_BASE_RUN_END_FRAC = 0.3
local _, n30 = need_at(10, 10)
check("FRAC 0.3 x 10 = 3 exactly (float 3.0000000000000004): need 3", n30 == 3, n30)
keel()
local _, nk = need_at(16, 12)
check("keel FRAC 0: off", nk == nil, nk)
defaults()
C.PILL_PLACE_TURTLE = false
local _, noff = need_at(16, 12)
check("turtle off: off", noff == nil, noff)

-- Phase chain: drive strategy.update far enough to see the opening end.
print("-- strategy.update: the opening ends at 4 of 16 taken")
defaults()
local real_scdo = cpf.smart_cost_dij_only
cpf.smart_cost_dij_only = function() return 10 end
local function perc(taken)
  return { friendly_pill_count = 0, hostile_pill_count = 0,
           neutral_pill_count = 4, dead_neutral_pill_count = 0,
           friendly_base_count = taken, hostile_base_count = 0,
           neutral_base_count = 16 - taken }
end
local function run(taken, ticks, st2)
  for _ = 1, ticks do
    st2.tick = st2.tick + 1
    st2.perc = perc(taken)
    local ok, err = pcall(S.update, st2, { bases = {}, pills = {}, tanks = {} },
          { tankx = 0, tanky = 0, armour = 40, shells = 40, mines = 0, trees = 0 })
    -- update runs on past the phase code; only an error before the phase
    -- is decided matters, and st2.phase_reason shows the phase code ran.
    if not ok and not st2.phase_reason then
      check("strategy.update reached the phase code", false, err)
      return
    end
  end
end
local stA = { tick = 600, birth_tick = 0, phase = "opening" }
run(3, 150, stA)
check("3 of 16 taken at t=750: still opening", stA.phase == "opening", stA.phase)
run(4, 99, stA)
check("4 of 16 taken, 99 ticks: still opening (hysteresis)", stA.phase == "opening", stA.phase)
run(4, 2, stA)
check("4 of 16 taken, past the hysteresis: middle", stA.phase == "middle", tostring(stA.phase) .. " / " .. tostring(stA.phase_reason))
local stB = { tick = 0, birth_tick = 0, phase = "opening" }
run(4, 450, stB)
check("4 of 16 taken at t=450: still opening (500-tick minimum)", stB.phase == "opening", stB.phase)
keel()
local stK = { tick = 600, birth_tick = 0, phase = "opening" }
run(4, 300, stK)
check("keel: 4 of 16 taken at t=900 stays opening", stK.phase == "opening", stK.phase)
cpf.smart_cost_dij_only = real_scdo

for _, k in ipairs(KNOBS) do C[k] = saved[k] end
print(string.format("\n  %d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
