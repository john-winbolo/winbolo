-- =========================================================================
-- test_spike.lua — standalone unit tests (2026-11-01) for spike.lua:
--   * spike.check — the base lead rule, the pill rule, the pair readiness
--     rule, each knob on and off, and role "single" with the pair rule off
--   * spike.team_pills — deployed + own carried + ally carried
--   * the pair lifecycle — propose, first landing, second due, the window,
--     a failed pair (care on the first pill, the area cooldown), a done pair
--   * spike.second_ok / los_clear — cover distance, LOS, forest, tank reach
--   * spike care — care_at (knob, time, tile still aggro), tank_mult,
--     the spkd advert both ways, a new friendly pill seen on an aggro tile
--   * builder_pool — a spike pill tops up at 1 hp missing and reaches
--     SPIKE_REPAIR_LEASH; score_row adds SPIKE_REPAIR_BONUS and score_terms
--     shows it; eligibility keeps the man for the second trip
--   * eval_place_pill_strategic — the hold row; role "first" on the goal; the
--     second pill: tiles near the first only, no double ebase, cost
--     SPIKE_PAIR_SECOND_COST, the util hold and surplus checks do not apply
-- No engine: influence is a stub field, every tile is placeable.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_spike.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local threat = require("threat")
local PP     = require("pill_portfolio")
local danger = require("danger")
local squad  = require("squad")
local ally_state = require("ally_state")
local G      = require("goals")
local BP     = require("builder_pool")
local SP     = require("spike")

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
local function near(a, b) return a ~= nil and b ~= nil and math.abs(a - b) < 1e-6 end

-- ── Engine stand-ins ─────────────────────────────────────────────────────
-- Influence: x < 150 is enemy ground (-30), x >= 150 is ours (+20).
cpf.influence_at   = function(x, y) return (x < 150) and -30 or 20 end
cpf.smart_cost_dij_only = function() return 10 end
U.is_placeable     = function() return true end
U.los_coverage     = function() return 0 end
local tiles = {}   -- tile type overrides, key y*256+x; default grass (4)
U.ttype            = function(x, y) return tiles[y * 256 + x] or 4 end
threat.at          = function() return 0 end
threat.coverage_at = function() return 0 end
threat.pill_at     = function() return 0 end
danger.should_panic_build = function() return false, 0, "test" end
danger.tank_fire_age = function() return nil, nil end
squad.blitz_shot_lines = function() return nil end
_G.cpf_dijkstra_lookup_by_kind = function() return 20 end   -- smart_cost
_G.cpf_estimate_tank_travel_ticks = function() return 50 end
_G.KIND_NORMAL = 0
ally_state.init()

C.BLITZ_NO_BUILD_ACTIVE    = false
C.PLACE_PILL_MAN_PATH_SAFE = false
C.PLACE_PILL_BEHIND_ONLY   = false
C.STRATEGIC_PLACE_MIN_SCORE       = -1e9
C.STRATEGIC_PLACE_MIN_SCORE_FLOOR = -1e9
C.PILL_PLACE_AGGRO_DEEP = true
C.PILL_PLACE_TURTLE     = false
local WIN_DEFAULT = C.AGGRO_DEEP_WIN_SHARES
C.AGGRO_DEEP_WIN_SHARES = false   -- section 6b turns it on; the rest test the shares as set
local DEF_T = { PP.TARGET_BACK, PP.TARGET_FRONT, PP.TARGET_AGGRO, PP.TARGET_UTIL }

local function base(mx, my, owner) return { mx = mx, my = my, owner = owner } end
local function pill(mx, my, owner, hp, in_tank)
  return { mx = mx, my = my, owner = owner, health = hp or 15, in_tank = in_tank or nil }
end
local function info_at(mx, my, carried, trees)
  return { tankx = mx * 256 + 128, tanky = my * 256 + 128,
           carried_pills = carried or 2, man_status = C.LGM_INTANK, inboat = false,
           armour = 40, shells = 40, trees = trees or 40, mines = 0, player_number = 0 }
end
local function fresh_state(tick) return { tick = tick or 1000, perc = { enemy_tanks = {} } } end
-- n friendly and m hostile bases (far apart, nowhere near the test tiles
-- unless placed there by the caller).
local function bases(nf, nh)
  local b = {}
  for i = 1, nf do b[#b + 1] = base(200 + i * 2, 20, "friendly") end
  for i = 1, nh do b[#b + 1] = base(10 + i * 2, 240, "hostile") end
  return b
end

-- ── 1. spike.check: the rules ───────────────────────────────────────────
print("spike.check")
PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "test")   -- default: front target 4 of 8
C.AGGRO_DEEP_BASE_GATE, C.AGGRO_DEEP_PILL_GATE = true, true
C.AGGRO_DEEP_PAIR = "solo"
local cnt8 = { back = 1, front = 4, aggro = 1, utility = 0 }   -- 6 deployed
local function chk(nf, nh, counts, carried, trees, st)
  local w = { pills = {}, bases = bases(nf, nh) }
  st = st or fresh_state()
  return SP.check(st, w, info_at(60, 104, carried, trees), counts, false), st
end
-- 6 deployed + 2 carried = 8 team pills; targets(8).front = 4 (have 4).
check("4 vs 1 bases, 8 pills, front 4/4, 2 carried: first",
      (chk(4, 1, cnt8, 2, 8) or {}).role == "first")
check("3 vs 1 bases: lead 2 < 3 -> no", chk(3, 1, cnt8, 2, 8) == false)
check("6 vs 4 bases: 6 >= 6 but lead 2 -> no", chk(6, 4, cnt8, 2, 8) == false)
check("5 vs 2 bases: 5 >= 3 and lead 3 -> yes", (chk(5, 2, cnt8, 2, 8) or {}).role == "first")
check("4 vs 3 bases: 4 < 4.5 -> no", chk(7, 5, cnt8, 2, 8) == false)
check("3 vs 0 bases: lead 3 -> yes", (chk(3, 0, cnt8, 2, 8) or {}).role == "first")
check("2 vs 0 bases: lead 2 -> no", chk(2, 0, cnt8, 2, 8) == false)
do
  local _, st = chk(3, 1, cnt8, 2, 8)
  check("AGGRO_DEEP_GATE key names the base rule", st._spike.log_key == "none:base_lead",
        st._spike.log_key)
end
C.AGGRO_DEEP_BASE_GATE = false
check("base rule off: 3 vs 1 -> first", (chk(3, 1, cnt8, 2, 8) or {}).role == "first")
C.AGGRO_DEEP_BASE_GATE = true
local cnt7 = { back = 1, front = 4, aggro = 0, utility = 0 }   -- 5 + 2 = 7
check("7 team pills < 8 -> no", chk(4, 1, cnt7, 2, 8) == false)
local cntf = { back = 2, front = 3, aggro = 1, utility = 0 }   -- front 3 < 4
check("front 3 < target 4 -> no", chk(4, 1, cntf, 2, 8) == false)
do
  local _, st = chk(4, 1, cntf, 2, 8)
  check("AGGRO_DEEP_GATE key names the front rule", st._spike.log_key == "none:front")
end
C.AGGRO_DEEP_PILL_GATE = false
check("pill rule off: 7 pills -> first", (chk(4, 1, cnt7, 2, 8) or {}).role == "first")
check("pill rule off: front short -> first", (chk(4, 1, cntf, 2, 8) or {}).role == "first")
C.AGGRO_DEEP_PILL_GATE = true
check("1 carried: no second pill -> no", chk(4, 1, { back = 1, front = 4, aggro = 2 }, 1, 8) == false)
check("2 carried, 7 trees: no wood for two -> no", chk(4, 1, cnt8, 2, 7) == false)
C.AGGRO_DEEP_PAIR = "off"
check("pair off, 1 carried: single", (chk(4, 1, { back = 1, front = 4, aggro = 2 }, 1, 0) or {}).role == "single")
C.AGGRO_DEEP_PAIR = "team"
check("pair team acts as solo", (chk(4, 1, cnt8, 2, 8) or {}).role == "first")
C.AGGRO_DEEP_PAIR = "solo"
C.PILL_PLACE_AGGRO_DEEP = false
check("deep knob off: false", chk(4, 1, cnt8, 2, 8) == false)
C.PILL_PLACE_AGGRO_DEEP = true
check("aggro_deep_ctx(world, false) = nil", G.aggro_deep_ctx({ bases = bases(1, 1) }, false) == nil)
check("aggro_deep_ctx(world) = role single", G.aggro_deep_ctx({ bases = bases(1, 1) }).role == "single")

-- team_pills
do
  local w = { pills = { pill(1, 1, "allied", 15, true), pill(2, 2, "friendly", 15, true),
                        pill(3, 3, "hostile", 15, true) }, bases = {} }
  local t, dep, own, ally = SP.team_pills(w, { carried_pills = 3 }, { back = 2, front = 1, aggro = 1, utility = 2 })
  check("team pills = 4 deployed + 3 own + 1 ally = 8", t == 8 and dep == 4 and own == 3 and ally == 1,
        string.format("%d %d %d %d", t, dep, own, ally))
end

-- ── 2. pair lifecycle ────────────────────────────────────────────────────
print("pair lifecycle")
PP.set_targets(0, 0, 1, 0, "test")         -- every pill is wanted as aggro
local function world_with(pills) return { pills = pills or {}, bases = bases(4, 1) } end
do
  local st = fresh_state(1000)
  local inf = info_at(95, 104, 2, 8)
  SP.on_dispatch(st, { _spike_role = "first", _spike_carried = 2 }, 103, 104, 95, 104, 1000)
  local P = st._spike.pair
  check("propose: pair first_out", P and P.phase == "first_out" and P.tmx == 95)
  inf.man_status = C.LGM_MOVING; inf.carried_pills = 1
  st.tick = 1010; SP.update(st, world_with(), inf, 1010)
  check("man out: holding", SP.holding(st, inf))
  local h = SP.hold_row(st, inf, 95, 104)
  check("hold row: follow_through at the dispatch tile, cost 25",
        h and h.goal.follow_through and h.goal.spike_hold and h.goal.mx == 95
        and h.cost == C.PLACE_FOLLOW_THROUGH_COST)
  st.tick = 1100; SP.update(st, world_with({ pill(103, 104, "friendly", 15) }), inf, 1100)
  check("first landed at t=1100", P.t_first == 1100, P.t_first)
  inf.man_status = C.LGM_INTANK
  st.tick = 1150; SP.update(st, world_with({ pill(103, 104, "friendly", 15) }), inf, 1150)
  check("man back: second_wait", P.phase == "second_wait")
  check("builder pool waits for the second trip", SP.second_pending(st))
  check("first spike tagged (own, t=1100)", st._spike.care[104 * 256 + 103]
        and st._spike.care[104 * 256 + 103].t == 1100)
  local c2 = SP.check(st, world_with(), inf, { back = 0, front = 0, aggro = 0 }, false)
  check("check in the window: role second (rules skipped)", c2 and c2.role == "second"
        and c2.fmx == 103 and c2.fmy == 104)
  st.tick = 1401; SP.update(st, world_with({ pill(103, 104, "friendly", 15) }), inf, 1401)
  check("window passed: pair ends", st._spike.pair == nil)
  check("failed pair: first pill care refreshed to t=1401",
        st._spike.care[104 * 256 + 103].t == 1401)
  check("failed pair: cooldown on the area", SP.in_cool(st, 110, 104, 1500)
        and not SP.in_cool(st, 103 + 17, 104, 1500) and not SP.in_cool(st, 103, 104, 1401 + 1500))
  local ad = { role = "first", state = st, now = 1500 }
  check("deep_ok: cooled tile refused for pair roles", not SP.deep_ok(ad, 105, 104))
  check("deep_ok: role single ignores the cooldown", SP.deep_ok({ role = "single" }, 105, 104))
  tiles[50 * 256 + 50] = C.T_FOREST
  check("deep_ok: forest refused for pair roles", not SP.deep_ok({ role = "first", now = 0 }, 50, 50))
  tiles[50 * 256 + 50] = nil
end
do
  -- Done path.
  local st = fresh_state(1000)
  local inf = info_at(95, 104, 2, 8)
  SP.on_dispatch(st, { _spike_role = "first" }, 103, 104, 95, 104, 1000)
  inf.man_status = C.LGM_MOVING
  SP.update(st, world_with({ pill(103, 104, "friendly") }), inf, 1050)
  inf.man_status = C.LGM_INTANK
  SP.update(st, world_with({ pill(103, 104, "friendly") }), inf, 1100)
  SP.on_dispatch(st, { _spike_role = "second" }, 108, 104, 95, 104, 1110)
  check("go: second_out", st._spike.pair.phase == "second_out")
  inf.man_status = C.LGM_MOVING
  SP.update(st, world_with({ pill(103, 104, "friendly") }), inf, 1120)
  check("second trip out: holding", SP.holding(st, inf))
  SP.update(st, world_with({ pill(103, 104, "friendly"), pill(108, 104, "friendly") }), inf, 1200)
  check("done: pair cleared", st._spike.pair == nil)
  check("done: both tagged", st._spike.care[104 * 256 + 103] and st._spike.care[104 * 256 + 108])
  check("done: no cooldown", #st._spike.cool == 0)
end
do
  -- First trip: man dies -> abort, no care, cooldown.
  local st = fresh_state(1000)
  local inf = info_at(95, 104, 2, 8)
  SP.on_dispatch(st, { _spike_role = "first" }, 103, 104, 95, 104, 1000)
  inf.man_status = C.LGM_DEAD
  SP.update(st, world_with(), inf, 1050)
  check("lgm lost: pair ends, no care, cooldown", st._spike.pair == nil
        and st._spike.care[104 * 256 + 103] == nil and #st._spike.cool == 1)
end
do
  -- second_wait: tank lost its pills -> no_pill.
  local st = fresh_state(1000)
  local inf = info_at(95, 104, 2, 8)
  SP.on_dispatch(st, { _spike_role = "first" }, 103, 104, 95, 104, 1000)
  inf.man_status = C.LGM_MOVING
  SP.update(st, world_with({ pill(103, 104, "friendly") }), inf, 1050)
  inf.man_status = C.LGM_INTANK; inf.carried_pills = 0
  SP.update(st, world_with({ pill(103, 104, "friendly") }), inf, 1100)
  SP.update(st, world_with({ pill(103, 104, "friendly") }), inf, 1101)
  check("no pill left: pair ends", st._spike.pair == nil)
end
do
  -- Pair off: the single spike is still tagged by its watch.
  C.AGGRO_DEEP_PAIR = "off"
  local st = fresh_state(1000)
  SP.on_dispatch(st, { _spike_role = "single" }, 103, 104, 95, 104, 1000)
  check("single: no pair", st._spike.pair == nil)
  SP.update(st, world_with({ pill(103, 104, "friendly") }), info_at(95, 104, 1), 1080)
  check("single: tagged on landing", st._spike.care[104 * 256 + 103]
        and st._spike.care[104 * 256 + 103].t == 1080)
  check("single: hold row nil (pair off)", SP.hold_row(st, info_at(95, 104), 95, 104) == nil)
  C.AGGRO_DEEP_PAIR = "solo"
end

-- ── 3. second tile rules ─────────────────────────────────────────────────
print("spike.second_ok")
do
  local ad = { role = "second", fmx = 100, fmy = 100, tmx = 95, tmy = 100 }
  check("8 tiles straight: ok", SP.second_ok(ad, 108, 100))
  check("9 tiles: too far", not SP.second_ok(ad, 109, 100))
  check("diagonal 6,6 = 8.49 > 8: too far", not SP.second_ok(ad, 106, 106))
  check("diagonal 5,5 = 7.07: ok", SP.second_ok(ad, 105, 105))
  check("the first tile itself: no", not SP.second_ok(ad, 100, 100))
  tiles[100 * 256 + 103] = C.T_BUILDING
  check("building between: no LOS", not SP.second_ok(ad, 106, 100))
  check("los_clear false", not SP.los_clear(100, 100, 106, 100))
  tiles[100 * 256 + 103] = C.T_HALFBUILD
  check("half building between: no LOS", not SP.second_ok(ad, 106, 100))
  tiles[100 * 256 + 103] = nil
  check("clear again", SP.second_ok(ad, 106, 100))
  tiles[100 * 256 + 106] = C.T_FOREST
  check("forest tile: no", not SP.second_ok(ad, 106, 100))
  tiles[100 * 256 + 106] = nil
  local far = { role = "second", fmx = 100, fmy = 100, tmx = 80, tmy = 100 }
  check("beyond PLACE_DISPATCH_MAX_DIST from the tank: no", not SP.second_ok(far, 103, 100)
        and SP.second_ok(far, 95, 100))
end

-- ── 4. spike care ────────────────────────────────────────────────────────
print("spike care")
do
  local st = fresh_state(2000)
  st._spike = nil
  SP.on_dispatch(st, { _spike_role = "single" }, 103, 104, 95, 104, 2000)
  st._spike.care[104 * 256 + 103] = { mx = 103, my = 104, t = 2000, src = "own" }
  C.SPIKE_REPAIR_PRIORITY = true
  check("care_at: tagged aggro tile", SP.care_at(st, 103, 104, 2100) ~= nil)
  check("care_at: expired", SP.care_at(st, 103, 104, 2000 + C.SPIKE_CARE_TICKS + 1) == nil)
  check("tank_mult = 1 - 0.5", near(SP.tank_mult(st, 103, 104, 2100), 1 - C.SPIKE_REPAIR_TANK_DISCOUNT))
  st._spike.care[104 * 256 + 160] = { mx = 160, my = 104, t = 2000, src = "own" }
  check("care_at: tile no longer aggro -> nil", SP.care_at(st, 160, 104, 2100) == nil)
  st._spike.care[104 * 256 + 160] = nil
  check("advert: own spike", SP.advert(st, 2100) == string.format("%02X%02X%04X", 103, 104, 2000))
  C.SPIKE_REPAIR_PRIORITY = false
  check("knob off: care_at nil", SP.care_at(st, 103, 104, 2100) == nil)
  check("knob off: tank_mult nil", SP.tank_mult(st, 103, 104, 2100) == nil)
  check("knob off: advert nil (key not sent)", SP.advert(st, 2100) == nil)
  C.SPIKE_REPAIR_PRIORITY = true
  -- Ally advert.
  ally_state.merge_info(3, 2100, { spkd = string.format("%02X%02X%04X", 90, 90, 2050) })
  local st2 = fresh_state(2100)
  SP.update(st2, { pills = {}, bases = {} }, info_at(60, 60), 2100)
  local e = st2._spike.care[90 * 256 + 90]
  check("ally advert: tagged, landing t=2050", e and e.src == "ally" and e.t == 2050,
        e and (e.src .. " " .. e.t))
  check("advert: ally tags not re-advertised", SP.advert(st2, 2100) == "-")
  -- New friendly pill seen on a deep aggro tile.
  local wseen = { pills = {}, bases = { base(100, 100, "hostile") } }
  SP.update(st2, wseen, info_at(60, 60), 2101)
  wseen.pills[1] = pill(104, 100, "friendly")
  wseen.pills[2] = pill(40, 40, "friendly")     -- aggro but far from any hostile base
  SP.update(st2, wseen, info_at(60, 60), 2102)
  check("seen: new pill near a hostile base tagged", st2._spike.care[100 * 256 + 104]
        and st2._spike.care[100 * 256 + 104].src == "seen")
  check("seen: new pill far from hostile bases not tagged", st2._spike.care[40 * 256 + 40] == nil)
end

-- ── 5. builder pool ──────────────────────────────────────────────────────
print("builder_pool")
do
  C.BUILDER_POOL_REPAIR_LINEAR = true
  local st = fresh_state(3000)
  st._spike = { pair = nil, watch = {}, care = {}, cool = {} }
  local w = { pills = { [7] = pill(102, 104, "friendly", 14) }, bases = {} }
  local inf = info_at(90, 104, 0, 40)        -- 12 tiles from the pill
  local rows = BP.discover(st, w, inf)
  check("no care: 1 hp missing -> no row", #rows == 0, #rows)
  st._spike.care[104 * 256 + 102] = { mx = 102, my = 104, t = 2900, src = "own" }
  rows = BP.discover(st, w, inf)
  local r = rows[1]
  check("care: topup row at 1 hp missing", r and r.type == "topup" and r.missing == 1)
  check("care: leash 15, 12 tiles is in reach", r and r.leash == 15 and not r.out_of_leash,
        r and r.leash)
  check("care: row carries the spike tag", r and r.spike == true)
  local real_trip, real_fd = BP.lgm_trip, BP.front_distance
  BP.lgm_trip = function() return 50, 100, nil end
  BP.front_distance = function() return 5 end
  local ctx = { ok = true, reserve = 0 }
  BP.score_row(st, w, inf, 3000, r, ctx)
  -- 30 x 1 - 0.25 x 100 = 5; + 200 = 205.
  check("score = 30x1 - 0.25x100 + 200 = 205", near(r.score, 205), r.score)
  check("pre_spike 5", near(r.spike_pre, 5), r.spike_pre)
  check("accepted (not below the floor)", r.reject == nil, r.reject)
  local terms = BP.score_terms(r)
  check("score_terms shows pre_spike{5} + spike_care{200}",
        terms:find("= pre_spike{5} + spike_care{200}", 1, true) ~= nil, terms)
  check("score_terms keeps tripcost{} before the spike chips",
        terms:find("tripcost{", 1, true) < terms:find("pre_spike{", 1, true))
  local form = BP.row_formula(r)
  check("row_formula has the spike_care segment", form:find("spike_care:SPIKE_REPAIR_BONUS", 1, true) ~= nil)
  C.SPIKE_REPAIR_PRIORITY = false
  rows = BP.discover(st, w, inf)
  check("knob off: no row again", #rows == 0)
  local w4 = { pills = { [7] = pill(102, 104, "friendly", 11) }, bases = {} }
  rows = BP.discover(st, w4, inf)
  check("knob off: 4 missing at 12 tiles -> out_of_leash (11)", rows[1] and rows[1].out_of_leash
        and rows[1].spike == nil)
  BP.score_row(st, w4, inf, 3000, rows[1], ctx)
  check("knob off: no bonus", rows[1].spike_bonus == nil and near(rows[1].score, 30 * 4 - 25))
  C.SPIKE_REPAIR_PRIORITY = true
  BP.lgm_trip, BP.front_distance = real_trip, real_fd
  -- Eligibility: the second trip keeps the man.
  st.builder = { mode = "none" }
  st._spike.pair = { phase = "second_wait", mx = 1, my = 1, t_first = 2990 }
  local ok, why = BP.eligibility(st, w, inf, 3000)
  check("eligibility: spike_pair_second", ok == false and why == "spike_pair_second", why)
  C.AGGRO_DEEP_PAIR = "off"
  ok, why = BP.eligibility(st, w, inf, 3000)
  check("eligibility: pair off -> no wait", why ~= "spike_pair_second", why)
  C.AGGRO_DEEP_PAIR = "solo"
end

-- ── 6. eval_place_pill_strategic ─────────────────────────────────────────
print("eval_place_pill_strategic")
local eval_pps
do
  local i = 1
  while true do
    local n, v = debug.getupvalue(G.score_place_tile, i)
    if not n then break end
    if n == "eval_place_pill_strategic" then eval_pps = v end
    i = i + 1
  end
end
check("eval reachable", eval_pps ~= nil)
-- Hostile bases (100,100), (106,100) near the first spike (103,104); (130,130)
-- far. Four friendly bases far east: 4 vs 3 fails the ratio, so add more.
local function wspike(extra_pills)
  local b = { base(130, 130, "hostile"), base(106, 100, "hostile"), base(100, 100, "hostile") }
  for i = 1, 8 do b[#b + 1] = base(200 + i * 3, 30, "friendly") end
  return { pills = extra_pills or {}, bases = b }
end
do
  -- Role first from the full rules (pill rule off: no deployed pills here).
  C.AGGRO_DEEP_PILL_GATE = false
  local st = fresh_state(1000)
  local r = eval_pps(st, wspike(), info_at(60, 104, 5, 40), 60, 104, false, 40, nil)
  check("rules met, 5 carried: deep goal with role first",
        r and r.goal._aggro_deep and r.goal._spike_role == "first",
        r and tostring(r.goal._spike_role))
  local st2 = fresh_state(1000)
  local r2 = eval_pps(st2, wspike(), info_at(60, 104, 1, 40), 60, 104, false, 40, nil)
  check("1 carried: no deep spike (normal placement)",
        r2 == nil or (r2.goal._aggro_deep == nil and r2.goal._spike_role == nil),
        r2 and tostring(r2.goal._aggro_deep))
  C.AGGRO_DEEP_PILL_GATE = true
end
do
  -- Hold row while the first trip is out.
  local st = fresh_state(1000)
  st._spike = { pair = { phase = "first_out", mx = 103, my = 104, tmx = 92, tmy = 104, t_disp = 990 },
                watch = {}, care = {}, cool = {} }
  local inf = info_at(95, 104, 1, 40); inf.man_status = C.LGM_MOVING
  local r = eval_pps(st, wspike(), inf, 95, 104, false, 40, nil)
  check("first trip out: eval returns the hold at (92,104)",
        r and r.goal.spike_hold and r.goal.mx == 92 and r.goal.my == 104)
end
local function eval_second(debug_on, targets_full)
  local st = fresh_state(1000)
  st._spike = { pair = { phase = "second_wait", mx = 103, my = 104, tmx = 95, tmy = 104,
                         t_disp = 900, t_first = 950 },
                watch = {}, care = {}, cool = {} }
  if targets_full then PP.set_targets(1, 0, 0, 0, "test") end
  _G.BRAIN_DEBUG_MODE = debug_on or nil
  local r = eval_pps(st, wspike({ pill(103, 104, "friendly") }), info_at(95, 104, 1, 40),
                     95, 104, false, 40, nil)
  _G.BRAIN_DEBUG_MODE = nil
  PP.set_targets(0, 0, 1, 0, "test")
  return r, st
end
do
  local r = eval_second()
  check("second: a goal with 1 carried and no util surplus", r ~= nil)
  check("second: role second, deep", r and r.goal._spike_role == "second" and r.goal._aggro_deep)
  check("second: within 8 of the first, LOS",
        r and U.edist(r.goal.mx, r.goal.my, 103, 104) <= 8
        and not (r.goal.mx == 103 and r.goal.my == 104),
        r and string.format("(%d,%d)", r.goal.mx, r.goal.my))
  check("second: cost = SPIKE_PAIR_SECOND_COST", r and r.cost == C.SPIKE_PAIR_SECOND_COST, r and r.cost)
  local rf = eval_second(false, true)
  check("second: aggro target full still places (no surplus check)", rf ~= nil
        and rf.goal._spike_role == "second")
  -- No second tile: everything blocked -> pair ends.
  local real = U.is_placeable
  U.is_placeable = function() return false end
  local rn, stn = eval_second()
  U.is_placeable = real
  check("no second tile: no goal, pair ended", rn == nil and stn._spike.pair == nil
        and #stn._spike.cool == 1 and stn._spike.care[104 * 256 + 103] ~= nil)
end
local function has_breakdown()
  local fh = io.open("goals.lua", "r")
  if not fh then return false end
  local src = fh:read("*a"); fh:close()
  return src:find("ebase{%.0f} + depth{%.0f}", 1, true) ~= nil
end
if not has_breakdown() then
  print("  skip (stripped goals.lua: no debug breakdown)")
else
  local r = eval_second(true)
  local win
  for _, c in ipairs(r and r.cands or {}) do
    if c.mx == r.goal.mx and c.my == r.goal.my then win = c.formula end
  end
  check("second: winner formula", win ~= nil)
  if win then
    print("    winner: " .. win:sub(1, 300))
    -- Every candidate tile is within 8 of (103,104), so (100,100) and
    -- (106,100) would pay ebase; the first spike covers both, (130,130) is
    -- out of reach: ebase 0 on every candidate.
    local all0 = true
    for _, c in ipairs(r.cands) do
      if not c.formula:find("ebase{0}", 1, true) then all0 = false end
    end
    check("second: ebase{0} on every candidate (no double credit)", all0)
    check("second: names the role and 1 of 3 bases counted",
          win:find("spike role second", 1, true) and win:find("(1 counted of 3 known)", 1, true))
    check("second: cost chip spike_pair_second{1}", win:find("spike_pair_second{1}", 1, true) ~= nil)
    local total = tonumber(win:match("^score{(%-?[%d%.]+)}"))
    local sum = 0
    local body = win:match("^score{[^}]*} = (.-)%s%s") or win
    for v in body:gmatch("{(%-?[%d%.]+)}") do sum = sum + tonumber(v) end
    check("second: terms add up to the score", total and math.abs(sum - total) <= 1,
          string.format("sum %.1f score %s", sum, tostring(total)))
  end
  -- The same tile scored as a role-single deep spike pays ebase for both
  -- near bases (60 + 60).
  C.AGGRO_DEEP_PAIR = "off"
  C.AGGRO_DEEP_BASE_GATE, C.AGGRO_DEEP_PILL_GATE = false, false
  local s_single = G.score_place_tile(fresh_state(), wspike(), info_at(103, 104, 5), 103, 104, -5, 103, 104)
  check("single deep at (103,104) scores (ebase counts all bases)", s_single ~= nil)
  C.AGGRO_DEEP_PAIR = "solo"
  C.AGGRO_DEEP_BASE_GATE, C.AGGRO_DEEP_PILL_GATE = true, true
end

-- ── 6b. win shares (AGGRO_DEEP_WIN_SHARES) ───────────────────────────────
print("win shares")
do
  C.AGGRO_DEEP_WIN_SHARES = true
  C.AGGRO_DEEP_BASE_GATE, C.AGGRO_DEEP_PILL_GATE = true, true
  PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "default")
  -- 4 front pills on the line (x=150: +20 beside -30 at x=149), 2 back
  -- (x=200), 2 carried: 8 team pills, normal front target 4.
  local function wpills(nfront, nback)
    local ps = {}
    for i = 1, nfront do ps[#ps + 1] = pill(150, 10 + i, "friendly") end
    for i = 1, nback do ps[#ps + 1] = pill(200, 10 + i, "friendly") end
    return ps
  end
  local st = fresh_state(5000)
  local inf = info_at(200, 30, 2)
  local w = { pills = wpills(4, 2), bases = bases(4, 1) }
  check("starts on normal shares", PP.win_on == false and PP.targets_source == "default")
  SP.update(st, w, inf, 5000)
  check("rules pass -> win shares on", PP.win_on == true, PP.targets_label())
  check("win: back 0.10 front 0.40 aggro 0.35 util 0.15",
        near(PP.TARGET_BACK, 0.10) and near(PP.TARGET_FRONT, 0.40)
        and near(PP.TARGET_AGGRO, 0.35) and near(PP.TARGET_UTIL, 0.15))
  check("label shows win", PP.targets_label() == "tgt 10/40/35/15 (win)", PP.targets_label())
  check("targets(20) under win: aggro 7", PP.targets(20).aggro == 7, PP.targets(20).aggro)
  check("normal_targets(20) under win: aggro 4", PP.normal_targets(20).aggro == 4,
        PP.normal_targets(20).aggro)
  -- The pill rule reads the normal front target: 12 team pills, front 5.
  -- Win targets(12).front = 5 (would pass), normal = 6 (fails).
  check("targets(12).front: win 5, normal 6",
        PP.targets(12).front == 5 and PP.normal_targets(12).front == 6)
  local why = SP.rules({ pills = {}, bases = bases(4, 1) }, info_at(200, 30, 2),
                       { back = 3, front = 5, aggro = 2, utility = 0 })
  check("rules under win use the normal front target -> front", why == "front", why)
  SP.update(st, w, inf, 5001)
  check("still passing: stays on", PP.win_on == true)
  -- Base lead lost: back to normal.
  SP.update(st, { pills = wpills(4, 2), bases = bases(3, 1) }, inf, 5002)
  check("base lead lost -> normal shares", PP.win_on == false and PP.targets_source == "default"
        and near(PP.TARGET_AGGRO, DEF_T[3]), PP.targets_label())
  -- Front below the normal target: stays normal.
  SP.update(st, { pills = wpills(3, 3), bases = bases(4, 1) }, inf, 5003)
  check("front 3 < 4 -> normal", PP.win_on == false)
  -- A rule with its knob off counts as a pass.
  C.AGGRO_DEEP_BASE_GATE = false
  SP.update(st, { pills = wpills(4, 2), bases = bases(3, 1) }, inf, 5004)
  check("base rule off: 3 vs 1 -> win", PP.win_on == true)
  C.AGGRO_DEEP_BASE_GATE = true
  SP.update(st, w, inf, 5005)
  -- portfolio= token: its shares are the normal ones.
  PP.set_targets(0.30, 0.40, 0.20, 0.10, "init_arg")
  check("a new portfolio= set ends win", PP.win_on == false and PP.targets_source == "init_arg")
  SP.update(st, w, inf, 5006)
  check("win on over portfolio=", PP.win_on == true and PP.targets_source == "win")
  SP.update(st, { pills = wpills(4, 2), bases = bases(1, 1) }, inf, 5007)
  check("off -> the portfolio= shares come back",
        PP.targets_source == "init_arg" and near(PP.TARGET_BACK, 0.30)
        and near(PP.TARGET_FRONT, 0.40) and near(PP.TARGET_AGGRO, 0.20)
        and near(PP.TARGET_UTIL, 0.10), PP.targets_label())
  -- Knob off while on: back to normal.
  SP.update(st, w, inf, 5008)
  C.AGGRO_DEEP_WIN_SHARES = false
  SP.update(st, w, inf, 5009)
  check("WIN_SHARES off -> normal", PP.win_on == false and PP.targets_source == "init_arg")
  C.AGGRO_DEEP_WIN_SHARES = true
  SP.update(st, w, inf, 5010)
  C.PILL_PLACE_AGGRO_DEEP, C.SPIKE_REPAIR_PRIORITY = false, false
  SP.update(st, w, inf, 5011)
  check("deep knob off -> normal", PP.win_on == false and PP.targets_source == "init_arg")
  C.PILL_PLACE_AGGRO_DEEP, C.SPIKE_REPAIR_PRIORITY = true, true
  C.AGGRO_DEEP_WIN_SHARES = false
  PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "default")
end

-- ── 7. keel ──────────────────────────────────────────────────────────────
print("PRESETS.keel")
check("default: AGGRO_DEEP_WIN_SHARES = true", WIN_DEFAULT == true)
check("win shares sum to 1", near(C.AGGRO_DEEP_WIN_BACK + C.AGGRO_DEEP_WIN_FRONT
      + C.AGGRO_DEEP_WIN_AGGRO + C.AGGRO_DEEP_WIN_UTIL, 1))
check("keel: SPIKE_REPAIR_PRIORITY = false", C.PRESETS.keel.SPIKE_REPAIR_PRIORITY == false)
check("keel: PILL_PLACE_AGGRO_DEEP = false", C.PRESETS.keel.PILL_PLACE_AGGRO_DEEP == false)
check("defaults: rules on", true == (require("constants").AGGRO_DEEP_BASE_RATIO == 1.5))

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
