-- =========================================================================
-- test_turtle_base.lua — standalone unit tests for the turtle base knobs
-- (TURTLE_BASE_* in constants.lua, 2026-10-02):
--   * goals.turtle_far_mult_of     — x min(2^(d/DOUBLE_TILES), MULT_CAP)
--   * goals.turtle_far_search_cap  — the ring search stops where the cap hits
--   * goals.turtle_far_ring_d      — Chebyshev ring search to influence > 0
--   * goals.turtle_far             — per-bot cache (C own-dist path and the
--                                    budgeted Lua fallback), turtle gate,
--                                    no-ground x1, cancelled-ground fallback
--   * goals.turtle_repick          — full evals pick on the multiplied cost
--   * goals.influence_tail_on      — TURTLE_NO_INFLUENCE_TAIL gate
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
                "TURTLE_BASE_D_REFRESH_TICKS", "TURTLE_BASE_D_READS_PER_TICK",
                "TURTLE_NO_INFLUENCE_TAIL", "EXPAND_ENABLED" }
local saved = {}
for _, k in ipairs(KNOBS) do saved[k] = C[k] end
local function defaults()
  for _, k in ipairs(KNOBS) do C[k] = saved[k] end
  C.PILL_PLACE_TURTLE = true
end
local function keel()
  defaults()
  for k, v in pairs(C.PRESETS.keel) do
    if k:match("^TURTLE_") then C[k] = v end
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
check("lower bound 8 starts at ring 8 and still finds 10", G.turtle_far_ring_d(100, 100, 32, inf_at, nil, 8) == 10, G.turtle_far_ring_d(100, 100, 32, inf_at, nil, 8))
local _, nr = G.turtle_far_ring_hit(0, 0, 3, inf_at)
check("ring 3 at the map corner reads only its 7 on-map tiles", nr == 7, nr)

print("-- turtle_far (cache, gates) -- Lua fallback path (no C own-dist helper)")
defaults()
local real_inf = cpf.influence_at
local real_build, real_odat = cpf.build_own_dist, cpf.own_dist_at
cpf.build_own_dist = function() return nil end   -- an older exe
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
-- Run one tick: one turtle_far call per base, as the evals do. Returns the
-- influence reads that tick.
local function tick_all(st, w)
  local c0 = calls
  for id = 1, 3 do if w.bases[id] then G.turtle_far(st, w, id, w.bases[id]) end end
  return calls - c0
end
local st = { tick = 1000 }
C.TURTLE_BASE_D_READS_PER_TICK = 300
local r1 = tick_all(st, world)
check("budget 300: first tick reads <= 300 + centres", r1 <= 301, r1)
local d3p, _, s3p = G.turtle_far(st, world, 3, world.bases[3])
check("base 3 before its search: provisional centre distance 40 -> cap 32, tagged centre",
      d3p == 32 and s3p == "centre", tostring(d3p) .. " " .. tostring(s3p))
local maxr, ticks = r1, 1
while st._turtle_far.jobs do
  st.tick = st.tick + 1; ticks = ticks + 1
  local r = tick_all(st, world); if r > maxr then maxr = r end
end
check("the pass spreads over more than one tick", ticks > 1, ticks)
check("no tick reads more than the budget + one ring (8 x 32)", maxr <= 300 + 8 * 32, maxr)
local d1, m1, s1 = G.turtle_far(st, world, 1, world.bases[1])
check("our own base: d=0, x1, not a centre value", d1 == 0 and m1 == 1 and s1 == nil, tostring(d1) .. " x" .. tostring(m1))
local d2, m2 = G.turtle_far(st, world, 2, world.bases[2])
check("10 east, disc edge at 52: d=8, x2", d2 == 8 and near(m2, 2), tostring(d2) .. " x" .. tostring(m2))
local d3, m3, s3 = G.turtle_far(st, world, 3, world.bases[3])
check("40 east (38 to the disc): d=32 search cap, x16", d3 == 32 and near(m3, 16) and s3 == nil, tostring(d3) .. " x" .. tostring(m3))
-- Tail off (turtle default): reach = 12, so base 3 (40 from the only centre)
-- is searched from ring 28, not ring 1.
check("reach bound with the tail off: 12", G.turtle_far_reach() == 12, G.turtle_far_reach())
C.TURTLE_BASE_D_READS_PER_TICK = nil
local c0 = calls
local pass_tick = st._turtle_far.pass_tick
st.tick = pass_tick + 49
tick_all(st, world)
check("pass done: no influence reads until 50 ticks after it started", calls == c0, calls - c0)
-- Our ground grows next to base #2: the cache still answers until the next pass.
set(58, 50, 100)
local d2b = G.turtle_far(st, world, 2, world.bases[2])
check("tick +49: still the cached d=8", d2b == 8, d2b)
st.tick = pass_tick + 50
local d2c = G.turtle_far(st, world, 2, world.bases[2])
check("tick +50: the new pass gives d=2 (base 2 fits in the first slice)", d2c == 2, d2c)
grid[50 * 256 + 58] = nil
-- Finding 3: we own base #1 but enemy influence cancels it (no tile > 0).
grid = {}
local stc = { tick = 2000 }
while true do
  tick_all(stc, world)
  if not stc._turtle_far.jobs then break end
  stc.tick = stc.tick + 1
end
local dc1, _, sc1 = G.turtle_far(stc, world, 1, world.bases[1])
local dc2, mc2, sc2 = G.turtle_far(stc, world, 2, world.bases[2])
check("cancelled ground: our base reads d=0(centre), not the cap", dc1 == 0 and sc1 == "centre", tostring(dc1) .. " " .. tostring(sc1))
check("cancelled ground: base 10 away reads d=10(centre), x2.83", dc2 == 10 and sc2 == "centre" and near(mc2, 2 ^ (10 / 8)), tostring(dc2) .. " " .. tostring(sc2))
local dcc = G.turtle_far(stc, world, 3, world.bases[3])
check("cancelled ground: base 40 away still the cap 32", dcc == 32, dcc)
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

print("-- turtle_far -- C own-dist path (helper stubbed with a brute-force Lua transform)")
defaults()
local own = {}
local builds = 0
cpf.build_own_dist = function(cap)
  builds = builds + 1
  local src = {}
  for k, v in pairs(grid) do if v > 0 then src[#src + 1] = k end end
  own = { cap = cap, src = src }
  return #src
end
cpf.own_dist_at = function(x, y)
  local best = own.cap
  for _, k in ipairs(own.src) do
    local sx, sy = k % 256, math.floor(k / 256)
    local dd = math.max(math.abs(sx - x), math.abs(sy - y))
    if dd < best then best = dd end
  end
  return best
end
grid = {}
for dx = -2, 2 do for dy = -2, 2 do set(50 + dx, 50 + dy, 100) end end
calls = 0
local stC = { tick = 3000 }
local reads = tick_all(stC, world)
check("C path: one build per pass", builds == 1, builds)
check("C path: only the centre reads (1 friendly centre), no ring search", reads == 1, reads)
check("C path: pass finishes in the same tick", stC._turtle_far.jobs == nil, tostring(stC._turtle_far.jobs))
local e1, _, es1 = G.turtle_far(stC, world, 1, world.bases[1])
local e2 = G.turtle_far(stC, world, 2, world.bases[2])
local e3 = G.turtle_far(stC, world, 3, world.bases[3])
check("C path: d = 0 / 8 / 32 as the ring search", e1 == 0 and es1 == nil and e2 == 8 and e3 == 32,
      tostring(e1) .. "/" .. tostring(e2) .. "/" .. tostring(e3))
stC.tick = 3049; tick_all(stC, world)
check("C path: no rebuild inside 50 ticks", builds == 1, builds)
stC.tick = 3050; tick_all(stC, world)
check("C path: rebuild at 50 ticks", builds == 2, builds)
grid = {}
stC.tick = 3100; tick_all(stC, world)
local f2, _, fs2 = G.turtle_far(stC, world, 2, world.bases[2])
check("C path, cancelled ground: d=10(centre)", f2 == 10 and fs2 == "centre", tostring(f2) .. " " .. tostring(fs2))
cpf.influence_at = real_inf
cpf.build_own_dist, cpf.own_dist_at = real_build, real_odat

print("-- influence tail gate (TURTLE_NO_INFLUENCE_TAIL)")
defaults()
check("default TURTLE_NO_INFLUENCE_TAIL = true", saved.TURTLE_NO_INFLUENCE_TAIL == true, saved.TURTLE_NO_INFLUENCE_TAIL)
check("keel TURTLE_NO_INFLUENCE_TAIL = false", C.PRESETS.keel.TURTLE_NO_INFLUENCE_TAIL == false, C.PRESETS.keel.TURTLE_NO_INFLUENCE_TAIL)
check("turtle bot: no tail", G.influence_tail_on() == false, G.influence_tail_on())
check("turtle bot: reach = max disc radius 12 (no EXPAND_RADIUS term)", G.turtle_far_reach() == 12, G.turtle_far_reach())
C.PILL_PLACE_TURTLE = false
check("not a turtle bot: tail on", G.influence_tail_on() == true, G.influence_tail_on())
C.PILL_PLACE_TURTLE = true
C.TURTLE_NO_INFLUENCE_TAIL = false
check("turtle bot, knob false: tail on", G.influence_tail_on() == true, G.influence_tail_on())
check("tail on: reach = 12 + EXPAND_RADIUS 10 = 22", G.turtle_far_reach() == 22, G.turtle_far_reach())
local ex = C.EXPAND_ENABLED
C.EXPAND_ENABLED = false
check("EXPAND_ENABLED false: tail off whatever the knob", G.influence_tail_on() == false, G.influence_tail_on())
C.EXPAND_ENABLED = ex
keel()
C.PILL_PLACE_TURTLE = true
check("keel knob on a turtle bot: tail on (as before)", G.influence_tail_on() == true, G.influence_tail_on())

print("-- full-eval re-pick on the multiplied cost (turtle_repick)")
defaults()
do
  -- Review scenario: base A 2 tiles from our ground, path 120; base B 32
  -- tiles away, path 100. nearest_where picks B (100); x16 makes it 1600.
  local wr = { bases = {
    [1] = { mx = 60, my = 60, owner = "friendly" },
    [4] = { mx = 64, my = 60, owner = "neutral", health = 0 },
    [5] = { mx = 94, my = 60, owner = "neutral", health = 0 },
  }, pills = {} }
  grid = {}
  for dx = -2, 2 do for dy = -2, 2 do set(60 + dx, 60 + dy, 100) end end
  local saved_inf = cpf.influence_at
  local sb = cpf.build_own_dist
  cpf.influence_at = function(x, y) return grid[y * 256 + x] or 0 end
  cpf.build_own_dist = function() return nil end
  local sr = { tick = 100 }
  while true do
    for _, id in ipairs({ 1, 4, 5 }) do G.turtle_far(sr, wr, id, wr.bases[id]) end
    if not sr._turtle_far.jobs then break end
    sr.tick = sr.tick + 1
  end
  local cands = { { id = 5, cost = 100 }, { id = 4, cost = 120 }, { id = 9, cost = -1 } }
  local function plain(_, c, m) return c * m, c * m end
  local pick = G.turtle_repick(sr, wr, wr.bases, cands, plain)
  local dA, mA = G.turtle_far(sr, wr, 4, wr.bases[4])
  check("A is d=2 (x1.19), B is at the cap (x16)", dA == 2 and near(mA, 2 ^ 0.25), tostring(dA))
  check("re-pick takes A (120 x 1.19 = 143) over B (100 x 16 = 1600)", pick and pick.id == 4, pick and pick.id)
  -- A floor after the multiplier counts: B floored to 8 beats A at 143.
  local function floored(b, c, m)
    local u = c * m
    if b == wr.bases[5] then return math.min(u, 8), u end
    return u, u
  end
  local pick2 = G.turtle_repick(sr, wr, wr.bases, cands, floored)
  check("a floor (IMMINENT / close-out) on B wins: B", pick2 and pick2.id == 5, pick2 and pick2.id)
  -- Equal multipliers: the pick nearest_where made (first lowest in order).
  local cands2 = { { id = 4, cost = 50 }, { id = 4, cost = 50 } }
  check("ties keep list order", G.turtle_repick(sr, wr, wr.bases, cands2, plain) == cands2[1], "")
  keel()
  check("keel: no re-pick (nil, the nearest_where pick stands)", G.turtle_repick(sr, wr, wr.bases, cands, plain) == nil, "")
  defaults()
  C.PILL_PLACE_TURTLE = false
  check("turtle off: no re-pick", G.turtle_repick(sr, wr, wr.bases, cands, plain) == nil, "")
  cpf.influence_at = saved_inf
  cpf.build_own_dist = sb
end

print("-- breakdown chip")
defaults()
local chip, det = G.turtle_far_text(8, 2)
check("chip names d and the multiplier", chip == " x turtle_far{d=8,x2.00}", chip)
check("detail names both knobs", det:find("TURTLE_BASE_DOUBLE_TILES", 1, true) ~= nil and det:find("TURTLE_BASE_MULT_CAP", 1, true) ~= nil, det)
local chip2 = G.turtle_far_text(32, 16)
check("chip at the search cap reads d>=32", chip2 == " x turtle_far{d=>=32,x16.00}", chip2)
local chip3, det3 = G.turtle_far_text(10, 2 ^ (10 / 8), "centre")
check("centre-distance d is tagged (centre)", chip3 == " x turtle_far{d=10(centre),x2.38}", chip3)
check("centre detail says it is the distance to a friendly centre", det3:find("nearest friendly base/pill centre", 1, true) ~= nil, det3)
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
-- The tick after: turtle_end is no longer what holds it (the chain alone
-- says opening, 12 neutral bases); the phase ratchet must keep middle.
run(4, 2, stA)
check("two more ticks after the turtle end: still middle", stA.phase == "middle", tostring(stA.phase) .. " / " .. tostring(stA.phase_reason))
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
