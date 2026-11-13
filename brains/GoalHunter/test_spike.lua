-- =========================================================================
-- test_spike.lua — standalone unit tests (2026-11-01) for spike.lua:
--   * spike.check — the base lead rule, the pill rule, the pair readiness
--     rule, each knob on and off, and role "single" with the pair rule off
--   * spike.team_pills — deployed + own carried + ally carried
--   * the pair lifecycle — propose, first landing, second due, the window,
--     a failed pair (care on the first pill, the area cooldown), a done pair
--   * spike.second_ok / los_clear — cover distance, LOS, forest, tank reach
--   * spike care — care_at (knob, time, tile still aggro), tank_mult,
--     defend_mult and the defend_pill discount (time, held clamp, KEEL),
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

-- These tests cover the per-bot rules of the first builds ("count");
-- the deny plan and its team vote are in test_focus_vote.lua.
C.FOCUS_SPIKE_RULE = "count"

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
C.FOCUS_SPIKE_ENABLED = true
C.PILL_PLACE_TURTLE     = false
local WIN_DEFAULT = C.FOCUS_SPIKE_WIN_SHARES
C.FOCUS_SPIKE_WIN_SHARES = false   -- section 6b turns it on; the rest test the shares as set
local U2A_DEFAULT = C.FOCUS_SPIKE_UTIL_TO_AGGRO
local NAE_DEFAULT = C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY
C.FOCUS_SPIKE_UTIL_TO_AGGRO = false  -- section 6d turns it on
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
C.FOCUS_SPIKE_BASE_RULE, C.FOCUS_SPIKE_PILL_RULE = true, true
C.FOCUS_SPIKE_PAIR = "solo"
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
  check("FOCUS_SPIKE_RULES key names the base rule", st._spike.log_key == "none:base_lead",
        st._spike.log_key)
end
C.FOCUS_SPIKE_BASE_RULE = false
check("base rule off: 3 vs 1 -> first", (chk(3, 1, cnt8, 2, 8) or {}).role == "first")
C.FOCUS_SPIKE_BASE_RULE = true
local cnt7 = { back = 1, front = 4, aggro = 0, utility = 0 }   -- 5 + 2 = 7
check("7 team pills < 8 -> no", chk(4, 1, cnt7, 2, 8) == false)
local cntf = { back = 2, front = 3, aggro = 1, utility = 0 }   -- front 3 < 4
check("front 3 < target 4 -> no", chk(4, 1, cntf, 2, 8) == false)
do
  local _, st = chk(4, 1, cntf, 2, 8)
  check("FOCUS_SPIKE_RULES key names the front rule", st._spike.log_key == "none:front")
end
C.FOCUS_SPIKE_PILL_RULE = false
check("pill rule off: 7 pills -> first", (chk(4, 1, cnt7, 2, 8) or {}).role == "first")
check("pill rule off: front short -> first", (chk(4, 1, cntf, 2, 8) or {}).role == "first")
C.FOCUS_SPIKE_PILL_RULE = true
check("1 carried: no second pill -> no", chk(4, 1, { back = 1, front = 4, aggro = 2 }, 1, 8) == false)
check("2 carried, 7 trees: no wood for two -> no", chk(4, 1, cnt8, 2, 7) == false)
C.FOCUS_SPIKE_PAIR = "off"
check("pair off, 1 carried: single", (chk(4, 1, { back = 1, front = 4, aggro = 2 }, 1, 0) or {}).role == "single")
C.FOCUS_SPIKE_PAIR = "team"
check("pair team acts as solo", (chk(4, 1, cnt8, 2, 8) or {}).role == "first")
C.FOCUS_SPIKE_PAIR = "solo"
C.FOCUS_SPIKE_ENABLED = false
check("deep knob off: false", chk(4, 1, cnt8, 2, 8) == false)
C.FOCUS_SPIKE_ENABLED = true
check("focus_spike_ctx(world, false) = nil", G.focus_spike_ctx({ bases = bases(1, 1) }, false) == nil)
check("focus_spike_ctx(world) = role single", G.focus_spike_ctx({ bases = bases(1, 1) }).role == "single")

-- team_pills
do
  local w = { pills = { pill(1, 1, "allied", 15, true), pill(2, 2, "friendly", 15, true),
                        pill(3, 3, "hostile", 15, true) }, bases = {} }
  local t, dep, own, ally = SP.team_pills(w, { carried_pills = 3 }, { back = 2, front = 1, aggro = 1, utility = 2 })
  check("team pills = 4 deployed + 3 own + 1 ally = 8", t == 8 and dep == 4 and own == 3 and ally == 1,
        string.format("%d %d %d %d", t, dep, own, ally))
  -- a placed pill in use as a blocker counts as utility; it still counts
  t, dep, own, ally = SP.team_pills(w, { carried_pills = 3 }, { back = 2, front = 1, aggro = 1, utility = 3 })
  check("team pills count a placed utility pill: 5 deployed + 3 own + 1 ally = 9",
        t == 9 and dep == 5 and own == 3 and ally == 1, string.format("%d %d %d %d", t, dep, own, ally))
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
  C.FOCUS_SPIKE_PAIR = "off"
  local st = fresh_state(1000)
  SP.on_dispatch(st, { _spike_role = "single" }, 103, 104, 95, 104, 1000)
  check("single: no pair", st._spike.pair == nil)
  SP.update(st, world_with({ pill(103, 104, "friendly") }), info_at(95, 104, 1), 1080)
  check("single: tagged on landing", st._spike.care[104 * 256 + 103]
        and st._spike.care[104 * 256 + 103].t == 1080)
  check("single: hold row nil (pair off)", SP.hold_row(st, info_at(95, 104), 95, 104) == nil)
  C.FOCUS_SPIKE_PAIR = "solo"
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
  -- New friendly pill seen on a focused spike tile.
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
  C.FOCUS_SPIKE_PAIR = "off"
  ok, why = BP.eligibility(st, w, inf, 3000)
  check("eligibility: pair off -> no wait", why ~= "spike_pair_second", why)
  C.FOCUS_SPIKE_PAIR = "solo"
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
  C.FOCUS_SPIKE_PILL_RULE = false
  local st = fresh_state(1000)
  local r = eval_pps(st, wspike(), info_at(60, 104, 5, 40), 60, 104, false, 40, nil)
  check("rules met, 5 carried: deep goal with role first",
        r and r.goal._focus_spike and r.goal._spike_role == "first",
        r and tostring(r.goal._spike_role))
  local st2 = fresh_state(1000)
  local r2 = eval_pps(st2, wspike(), info_at(60, 104, 1, 40), 60, 104, false, 40, nil)
  check("1 carried: no focused spike (normal placement)",
        r2 == nil or (r2.goal._focus_spike == nil and r2.goal._spike_role == nil),
        r2 and tostring(r2.goal._focus_spike))
  C.FOCUS_SPIKE_PILL_RULE = true
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
  check("second: role second, deep", r and r.goal._spike_role == "second" and r.goal._focus_spike)
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
  -- The same tile scored as a role-single focused spike pays ebase for both
  -- near bases (60 + 60).
  C.FOCUS_SPIKE_PAIR = "off"
  C.FOCUS_SPIKE_BASE_RULE, C.FOCUS_SPIKE_PILL_RULE = false, false
  local s_single = G.score_place_tile(fresh_state(), wspike(), info_at(103, 104, 5), 103, 104, -5, 103, 104)
  check("single deep at (103,104) scores (ebase counts all bases)", s_single ~= nil)
  C.FOCUS_SPIKE_PAIR = "solo"
  C.FOCUS_SPIKE_BASE_RULE, C.FOCUS_SPIKE_PILL_RULE = true, true
end

-- ── 6b. win shares (FOCUS_SPIKE_WIN_SHARES) ───────────────────────────────
print("win shares")
do
  C.FOCUS_SPIKE_WIN_SHARES = true
  C.FOCUS_SPIKE_BASE_RULE, C.FOCUS_SPIKE_PILL_RULE = true, true
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
  C.FOCUS_SPIKE_BASE_RULE = false
  SP.update(st, { pills = wpills(4, 2), bases = bases(3, 1) }, inf, 5004)
  check("base rule off: 3 vs 1 -> win", PP.win_on == true)
  C.FOCUS_SPIKE_BASE_RULE = true
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
  C.FOCUS_SPIKE_WIN_SHARES = false
  SP.update(st, w, inf, 5009)
  check("WIN_SHARES off -> normal", PP.win_on == false and PP.targets_source == "init_arg")
  C.FOCUS_SPIKE_WIN_SHARES = true
  SP.update(st, w, inf, 5010)
  C.FOCUS_SPIKE_ENABLED, C.SPIKE_REPAIR_PRIORITY = false, false
  SP.update(st, w, inf, 5011)
  check("deep knob off -> normal", PP.win_on == false and PP.targets_source == "init_arg")
  C.FOCUS_SPIKE_ENABLED, C.SPIKE_REPAIR_PRIORITY = true, true
  C.FOCUS_SPIKE_WIN_SHARES = false
  PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "default")
end

-- The every-tick rule check reads the pill roles but must not write them:
-- writing moved the role re-evaluation times and changed play while the
-- rules failed (round 1 bench: WIN_SHARES on and off gave different games).
do
  C.FOCUS_SPIKE_WIN_SHARES = true
  local ps = { pill(150, 11, "friendly"), pill(200, 11, "friendly") }
  ps[2].role, ps[2].role_tick = "aggro", 0          -- stale at t=9000
  local w = { pills = ps, bases = bases(1, 4) }      -- base lead fails
  SP.update(fresh_state(9000), w, info_at(200, 30, 0), 9000)
  check("update_win leaves an unset role unset", ps[1].role == nil and ps[1].role_tick == nil,
        tostring(ps[1].role))
  check("update_win leaves a stale role as it was", ps[2].role == "aggro" and ps[2].role_tick == 0,
        tostring(ps[2].role) .. "@" .. tostring(ps[2].role_tick))
  local c1 = PP.counts(w, 9000, true)
  check("peek counts give the same roles as counts", ps[1].role == nil
        and (function() local c2 = PP.counts(w, 9000); return c1.front == c2.front
             and c1.back == c2.back and c1.aggro == c2.aggro end)())
  check("counts (no peek) still writes the cache", ps[1].role ~= nil and ps[2].role_tick == 9000)
  C.FOCUS_SPIKE_WIN_SHARES = false
  PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "default")
end

-- ── 6d. utility to aggro (FOCUS_SPIKE_UTIL_TO_AGGRO) ──────────────────────
print("utility to aggro")
do
  -- The targets_for code before the share-0 leftover skip, to show that
  -- every share set with no 0 share gives the same targets as before.
  local function old_targets(total, b, f, a, u)
    if total <= 0 then return { back = 0, front = 0, aggro = 0, utility = 0 } end
    local t = { utility = math.floor(total * u), front = math.floor(total * f),
                aggro = math.floor(total * a), back = math.floor(total * b) }
    local assigned, i = t.utility + t.front + t.aggro + t.back, 0
    while assigned < total do
      local cat = PP.FILL_PRIORITY[(i % #PP.FILL_PRIORITY) + 1]
      t[cat] = t[cat] + 1; assigned = assigned + 1; i = i + 1
    end
    return t
  end
  local function same_t(x, y)
    return x.back == y.back and x.front == y.front and x.aggro == y.aggro
       and x.utility == y.utility
  end
  local function tstr(t) return string.format("%d/%d/%d/%d", t.back, t.front, t.aggro, t.utility) end
  -- Every share set in use today: the defaults, the win shares, and
  -- portfolio= values with no 0 share (no preset sets portfolio=).
  local sets = {
    { "default", DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4] },
    { "win", C.FOCUS_SPIKE_WIN_BACK, C.FOCUS_SPIKE_WIN_FRONT, C.FOCUS_SPIKE_WIN_AGGRO, C.FOCUS_SPIKE_WIN_UTIL },
    { "30/40/20/10", 0.30, 0.40, 0.20, 0.10 },
    { "25/25/25/25", 0.25, 0.25, 0.25, 0.25 },
    { "10/30/50/10", 0.10, 0.30, 0.50, 0.10 },
  }
  local all_same = true
  for _, sset in ipairs(sets) do
    PP.set_targets(sset[2], sset[3], sset[4], sset[5], "test")
    for n = 0, 60 do
      if not same_t(PP.targets(n), old_targets(n, sset[2], sset[3], sset[4], sset[5])) then
        all_same = false
        check("targets unchanged " .. sset[1] .. " n=" .. n, false, tstr(PP.targets(n)))
      end
    end
  end
  check("no 0 share: targets(0..60) same as before for default, win and 3 portfolio= sets", all_same)

  -- Leftover skip: a role with share exactly 0 gets no leftover slot.
  PP.set_targets(0.25, 0.25, 0.50, 0, "test")
  check("share 0: targets(3) util 0 (old code gave util 1)",
        tstr(PP.targets(3)) == "0/1/2/0" and tstr(old_targets(3, 0.25, 0.25, 0.50, 0)) == "0/1/1/1",
        tstr(PP.targets(3)))
  check("share 0: util 0 for every total 1..60", (function()
    for n = 1, 60 do if PP.targets(n).utility ~= 0 then return false end end
    return true end)())
  PP.set_targets(0, 0, 0, 0, "test")
  check("all shares 0: the old fill (no endless loop)", tstr(PP.targets(3)) == "0/1/1/1",
        tstr(PP.targets(3)))

  -- The switch with win shares on and off: targets for 8..16 pills.
  local want_win = {
    [8] = "0/4/4/0", [9] = "0/4/5/0", [10] = "1/4/5/0", [11] = "1/5/5/0", [12] = "1/5/6/0",
    [13] = "1/6/6/0", [14] = "1/6/7/0", [15] = "1/7/7/0", [16] = "1/7/8/0" }
  local want_nrm = {
    [8] = "1/4/3/0", [9] = "1/5/3/0", [10] = "2/5/3/0", [11] = "2/5/4/0", [12] = "2/6/4/0",
    [13] = "2/6/5/0", [14] = "2/7/5/0", [15] = "3/7/5/0", [16] = "3/8/5/0" }
  C.FOCUS_SPIKE_BASE_RULE, C.FOCUS_SPIKE_PILL_RULE = true, true
  C.FOCUS_SPIKE_UTIL_TO_AGGRO = true
  local function wpills(nfront, nback)
    local ps = {}
    for i = 1, nfront do ps[#ps + 1] = pill(150, 10 + i, "friendly") end
    for i = 1, nback do ps[#ps + 1] = pill(200, 10 + i, "friendly") end
    return ps
  end
  local st = fresh_state(6000)
  local inf = info_at(200, 30, 2)
  local w = { pills = wpills(4, 2), bases = bases(4, 1) }
  for _, win in ipairs({ true, false }) do
    C.FOCUS_SPIKE_WIN_SHARES = win
    PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "default")
    local tag = win and "win on" or "win off"
    SP.update(st, { pills = wpills(4, 2), bases = bases(1, 1) }, inf, 6000)
    check(tag .. ": rules fail -> switch off", PP.u2a_on == false and near(PP.TARGET_UTIL, DEF_T[4]))
    SP.update(st, w, inf, 6001)
    check(tag .. ": rules pass -> switch on", PP.u2a_on == true and PP.win_on == win, PP.targets_label())
    if win then
      check("win on: shares 10/40/50/0", near(PP.TARGET_BACK, 0.10) and near(PP.TARGET_FRONT, 0.40)
            and PP.TARGET_AGGRO == 0.5 and PP.TARGET_UTIL == 0, PP.targets_label())
      check("win on: label", PP.targets_label() == "tgt 10/40/50/0 (win+u2a)", PP.targets_label())
    else
      check("win off: shares 20/45/35/0", near(PP.TARGET_BACK, 0.20) and near(PP.TARGET_FRONT, 0.45)
            and PP.TARGET_AGGRO == 0.35 and PP.TARGET_UTIL == 0, PP.targets_label())
      check("win off: label", PP.targets_label() == "tgt 20/45/35/0 (default+u2a)", PP.targets_label())
    end
    local want = win and want_win or want_nrm
    for n = 8, 16 do
      check(tag .. ": targets(" .. n .. ") = " .. want[n], tstr(PP.targets(n)) == want[n], tstr(PP.targets(n)))
    end
    -- The hold reserve (goals.lua util_reserve = PP.targets(pf_all).utility).
    check(tag .. ": utility reserve 0 for 1..60 pills", (function()
      for n = 1, 60 do if PP.targets(n).utility ~= 0 then return false end end
      return true end)())
    -- The pill rule reads the normal targets: the switch does not turn itself off.
    check(tag .. ": normal_targets(8) still the normal targets",
          same_t(PP.normal_targets(8), old_targets(8, DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4])),
          tstr(PP.normal_targets(8)))
    SP.update(st, w, inf, 6002)
    check(tag .. ": still passing -> stays on", PP.u2a_on == true)
    -- Switch-back: the rules fail, the normal shares come back.
    SP.update(st, { pills = wpills(4, 2), bases = bases(3, 1) }, inf, 6003)
    check(tag .. ": base lead lost -> switch off, normal shares back",
          PP.u2a_on == false and PP.win_on == false and PP.targets_source == "default"
          and near(PP.TARGET_AGGRO, DEF_T[3]) and near(PP.TARGET_UTIL, DEF_T[4]), PP.targets_label())
    local back_ok = true
    for n = 8, 16 do
      if not same_t(PP.targets(n), old_targets(n, DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4])) then
        back_ok = false
      end
    end
    check(tag .. ": after switch-back targets(8..16) are the normal ones", back_ok)
  end
  -- Knob off: the rules pass but the shares do not change.
  C.FOCUS_SPIKE_WIN_SHARES, C.FOCUS_SPIKE_UTIL_TO_AGGRO = false, false
  PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "default")
  SP.update(st, w, inf, 6010)
  check("knob off: rules pass, shares unchanged", PP.u2a_on == false and PP.win_on == false
        and PP.targets_source == "default" and near(PP.TARGET_UTIL, DEF_T[4]))
  -- Knob turned off while on: back to normal.
  C.FOCUS_SPIKE_UTIL_TO_AGGRO = true
  SP.update(st, w, inf, 6011)
  check("knob on -> switch on", PP.u2a_on == true)
  C.FOCUS_SPIKE_UTIL_TO_AGGRO = false
  SP.update(st, w, inf, 6012)
  check("knob turned off -> switch off", PP.u2a_on == false and near(PP.TARGET_UTIL, DEF_T[4]))
  -- A new portfolio= set ends it; the portfolio= utility share moves to aggro.
  C.FOCUS_SPIKE_UTIL_TO_AGGRO = true
  SP.update(st, w, inf, 6013)
  PP.set_targets(0.30, 0.40, 0.20, 0.10, "init_arg")
  check("portfolio= set ends the switch", PP.u2a_on == false and near(PP.TARGET_UTIL, 0.10))
  SP.update(st, w, inf, 6014)
  check("portfolio= 30/40/20/10 -> 30/40/30/0", PP.u2a_on == true and near(PP.TARGET_AGGRO, 0.30)
        and PP.TARGET_UTIL == 0 and PP.targets_label() == "tgt 30/40/30/0 (init_arg+u2a)",
        PP.targets_label())
  SP.update(st, { pills = wpills(4, 2), bases = bases(1, 1) }, inf, 6015)
  check("off -> the portfolio= shares come back", PP.u2a_on == false
        and PP.targets_label() == "tgt 30/40/20/10 (init_arg)", PP.targets_label())
  -- The every-tick check uses the peek reads: no pill role written.
  PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "default")
  local ps = wpills(4, 2)
  SP.update(fresh_state(6020), { pills = ps, bases = bases(4, 1) }, inf, 6020)
  check("switch check writes no pill role", (function()
    for _, p in ipairs(ps) do if p.role ~= nil or p.role_tick ~= nil then return false end end
    return true end)() and PP.u2a_on == true)

  -- The hold rule (goals.lua PLACE_HOLD_UTIL) with one carried pill and one
  -- utility pill (in this tank). Normal shares: reserve 1 -> hold. Switch
  -- on: reserve 0 -> no hold.
  PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "default")
  local function hold_cat()
    local hs = fresh_state(7000)
    C.FOCUS_SPIKE_ENABLED = false
    eval_pps(hs, { pills = { pill(60, 104, "friendly", 15, true) }, bases = bases(4, 1) },
             info_at(60, 104, 1, 40), 60, 104, false, 40, nil)
    C.FOCUS_SPIKE_ENABLED = true
    return hs._place_need_cat
  end
  check("normal shares, 1 carried, utility 1 = reserve 1: hold", hold_cat() == "util_reserve",
        tostring(hold_cat()))
  PP.set_u2a(true)
  check("switch on: reserve 0, no utility hold", hold_cat() ~= "util_reserve", tostring(hold_cat()))
  PP.set_u2a(false)
  C.FOCUS_SPIKE_UTIL_TO_AGGRO = false
  C.FOCUS_SPIKE_WIN_SHARES = false
  PP.set_targets(DEF_T[1], DEF_T[2], DEF_T[3], DEF_T[4], "default")
end

-- ── 6e. neutral as enemy (FOCUS_SPIKE_NEUTRAL_AS_ENEMY) ───────────────────
print("neutral as enemy")
do
  local saved = C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY
  C.FOCUS_SPIKE_BASE_RULE = true
  -- world.bases is keyed by base id (world.lua). Mixed owners: 2 friendly,
  -- 1 allied, 2 hostile, 2 neutral, 1 record with no owner; the map has
  -- ids 0..9, so ids 8 and 9 are on the map with no record (not seen yet).
  local function mixed()
    local b = {}
    b[0] = base(200, 20, "friendly"); b[1] = base(202, 20, "friendly")
    b[2] = base(204, 20, "allied")
    b[3] = base(10, 240, "hostile");  b[4] = base(12, 240, "hostile")
    b[5] = base(14, 240, "neutral");  b[6] = base(16, 240, "neutral")
    b[7] = { mx = 18, my = 240 }      -- a record with no owner field
    return { bases = b, pills = {}, _bases_on_map = 0x3FF }
  end
  C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY = true
  local o, t, h, n, u = SP.base_counts(mixed())
  check("knob on: ours 3, theirs 2 + 2 + 3 = 7", o == 3 and t == 7 and h == 2 and n == 2 and u == 3,
        string.format("%d %d %d %d %d", o, t, h, n, u))
  C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY = false
  o, t, h, n, u = SP.base_counts(mixed())
  check("knob off: ours 3, theirs = hostile 2 (old count)", o == 3 and t == 2,
        string.format("%d %d", o, t))
  -- No map mask (an older engine): only records count.
  C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY = true
  local w = mixed(); w._bases_on_map = nil
  o, t, h, n, u = SP.base_counts(w)
  check("no map mask: unknown = the record with no owner only", u == 1 and t == 5,
        string.format("%d %d", u, t))
  -- A base seen later moves from unknown to its owner.
  w = mixed(); w.bases[8] = base(20, 240, "neutral")
  o, t, h, n, u = SP.base_counts(w)
  check("seen base 8 (neutral): unknown 2, neutral 3, theirs 7", u == 2 and n == 3 and t == 7)

  -- The strict rule (ratio 1.5, lead 3).
  local function nb(nf, nh, nn)
    local b = bases(nf, nh)
    for i = 1, nn do b[#b + 1] = base(60 + i * 2, 240, "neutral") end
    return { bases = b, pills = {} }
  end
  local cnt = { back = 1, front = 4, aggro = 1, utility = 0 }
  for _, on in ipairs({ true, false }) do
    C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY = on
    local tag = on and "knob on" or "knob off"
    local why = SP.rules(nb(10, 6, 0), info_at(200, 30, 2), cnt)
    check(tag .. ": 10 vs 6 hostile, no neutral -> pass", why == "ok", why)
  end
  C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY = true
  local why = SP.rules(nb(8, 5, 3), info_at(200, 30, 2), cnt)
  check("knob on: 8 vs 5 hostile + 3 neutral = 8 -> base_lead", why == "base_lead", why)
  C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY = false
  why = SP.rules(nb(8, 5, 3), info_at(200, 30, 2), cnt)
  check("knob off: 8 vs 5 hostile (neutral not counted) -> pass", why == "ok", why)
  -- The check log line shows the parts (a rules table carries them).
  C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY = true
  local _, r = SP.rules(nb(8, 5, 3), info_at(200, 30, 2), cnt)
  check("rules table: hostile 5, neutral 3, unknown 0", r.b_host == 5 and r.b_neut == 3 and r.b_unk == 0)
  C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY = saved
end

-- ── 7. keel ──────────────────────────────────────────────────────────────
print("PRESETS.keel")
check("default: FOCUS_SPIKE_WIN_SHARES = true", WIN_DEFAULT == true)
do
  local sp, raw = C.PRESETS.keel_focus, C.PRESETS.keel_focus_raw
  local nk, same = 0, true
  for k, v in pairs(C.PRESETS.keel) do
    nk = nk + 1
    if k ~= "FOCUS_SPIKE_ENABLED" and k ~= "SPIKE_REPAIR_PRIORITY"
       and k ~= "FOCUS_SPIKE_UTIL_TO_AGGRO" and k ~= "FOCUS_SPIKE_NEUTRAL_AS_ENEMY"
       and k ~= "REPAIR_FRIENDLY_FIRE_REJECT_TICKS" and k ~= "FOCUS_SPIKE_DEFEND_DISCOUNT"
       and k ~= "FOCUS_SPIKE_NEUTRAL_PILL_COVER" and k ~= "FOCUS_SPIKE_ADAPTIVE_SPOT"
       and (sp[k] ~= v or raw[k] ~= v) then
      same = false
    end
  end
  check("keel_focus / _raw carry every other keel value", nk > 10 and same, nk)
  check("keel_focus / _raw: friendly fire 50", sp.REPAIR_FRIENDLY_FIRE_REJECT_TICKS == 50
        and raw.REPAIR_FRIENDLY_FIRE_REJECT_TICKS == 50)
  check("preset keel_focus: every rule on", sp.FOCUS_SPIKE_ENABLED == true
        and sp.FOCUS_SPIKE_BASE_RULE == true and sp.FOCUS_SPIKE_PILL_RULE == true
        and sp.FOCUS_SPIKE_PAIR == "solo" and sp.SPIKE_REPAIR_PRIORITY == true
        and sp.FOCUS_SPIKE_WIN_SHARES == true)
  check("preset keel_focus_raw: deep on, the rest off", raw.FOCUS_SPIKE_ENABLED == true
        and raw.FOCUS_SPIKE_BASE_RULE == false and raw.FOCUS_SPIKE_PILL_RULE == false
        and raw.FOCUS_SPIKE_PAIR == "off" and raw.SPIKE_REPAIR_PRIORITY == false
        and raw.FOCUS_SPIKE_WIN_SHARES == false)
  local lo = C.PRESETS.keel_focus_loose
  local lsame = true
  for k, v in pairs(sp) do
    if k ~= "FOCUS_SPIKE_MIN_PILLS" and k ~= "FOCUS_SPIKE_BASE_LEAD"
       and k ~= "FOCUS_SPIKE_BASE_RATIO" and k ~= "FOCUS_SPIKE_RULE"
       and lo[k] ~= v then lsame = false end
  end
  check("keel_focus_loose = keel_focus plus the three rule values", lsame)
  check("keel_focus and _loose are cluster; _raw keeps the count rules",
        sp.FOCUS_SPIKE_RULE == "cluster" and lo.FOCUS_SPIKE_RULE == "cluster"
        and raw.FOCUS_SPIKE_RULE == "count", tostring(sp.FOCUS_SPIKE_RULE))
  check("preset keel_focus_loose: pills 6, lead 2, ratio 1.25", lo.FOCUS_SPIKE_MIN_PILLS == 6
        and lo.FOCUS_SPIKE_BASE_LEAD == 2 and lo.FOCUS_SPIKE_BASE_RATIO == 1.25)
  check("defaults stay: pills 8, lead 3, ratio 1.5", C.FOCUS_SPIKE_MIN_PILLS == 8
        and C.FOCUS_SPIKE_BASE_LEAD == 3 and C.FOCUS_SPIKE_BASE_RATIO == 1.5)
  for _, t in ipairs({ sp, raw, lo }) do
    for k, v in pairs(t) do
      if type(C[k]) ~= type(v) then check("preset knob " .. k .. " type", false, type(v)) end
    end
  end
end
check("win shares sum to 1", near(C.FOCUS_SPIKE_WIN_BACK + C.FOCUS_SPIKE_WIN_FRONT
      + C.FOCUS_SPIKE_WIN_AGGRO + C.FOCUS_SPIKE_WIN_UTIL, 1))
check("keel: SPIKE_REPAIR_PRIORITY = false", C.PRESETS.keel.SPIKE_REPAIR_PRIORITY == false)
check("keel: FOCUS_SPIKE_ENABLED = false", C.PRESETS.keel.FOCUS_SPIKE_ENABLED == false)
check("default: FOCUS_SPIKE_UTIL_TO_AGGRO = true", U2A_DEFAULT == true)
check("keel: FOCUS_SPIKE_UTIL_TO_AGGRO = false", C.PRESETS.keel.FOCUS_SPIKE_UTIL_TO_AGGRO == false)
check("keel_focus and keel_focus_loose: FOCUS_SPIKE_UTIL_TO_AGGRO = true",
      C.PRESETS.keel_focus.FOCUS_SPIKE_UTIL_TO_AGGRO == true
      and C.PRESETS.keel_focus_loose.FOCUS_SPIKE_UTIL_TO_AGGRO == true)
check("keel_focus_raw (placement only): FOCUS_SPIKE_UTIL_TO_AGGRO = false",
      C.PRESETS.keel_focus_raw.FOCUS_SPIKE_UTIL_TO_AGGRO == false)
check("default: FOCUS_SPIKE_NEUTRAL_AS_ENEMY = true", NAE_DEFAULT == true)
check("keel: FOCUS_SPIKE_NEUTRAL_AS_ENEMY = false", C.PRESETS.keel.FOCUS_SPIKE_NEUTRAL_AS_ENEMY == false)
check("keel_focus and keel_focus_loose: FOCUS_SPIKE_NEUTRAL_AS_ENEMY = true",
      C.PRESETS.keel_focus.FOCUS_SPIKE_NEUTRAL_AS_ENEMY == true
      and C.PRESETS.keel_focus_loose.FOCUS_SPIKE_NEUTRAL_AS_ENEMY == true)
check("keel_focus_raw: FOCUS_SPIKE_NEUTRAL_AS_ENEMY = false",
      C.PRESETS.keel_focus_raw.FOCUS_SPIKE_NEUTRAL_AS_ENEMY == false)
check("defaults: rules on", true == (require("constants").FOCUS_SPIKE_BASE_RATIO == 1.5))

-- ── Spike defend discount (FOCUS_SPIKE_DEFEND_DISCOUNT) ─────────────────
print("spike defend discount")
do
  _G.OBJECT_TANK    = _G.OBJECT_TANK or 1
  _G.OBJECT_HOSTILE = _G.OBJECT_HOSTILE or 0x80
  local old_d, old_t = C.FOCUS_SPIKE_DEFEND_DISCOUNT, C.FOCUS_SPIKE_DEFEND_TICKS
  local function state_with(src, t)
    local s = { tick = t, perc = { enemy_tanks = {} },
                _spike = { pair = nil, watch = {}, care = {}, cool = {} } }
    if src then s._spike.care[104 * 256 + 103] = { mx = 103, my = 104, t = 1000, src = src } end
    return s
  end
  local function info_far(objects)
    return { player_number = 0, allies = 1 + 2, num_players = 4, shells = 40, armour = 40,
             mines = 0, trees = 0, man_status = C.LGM_INTANK, objects = objects or {},
             tankx = 200 * 256, tanky = 200 * 256 }
  end
  local function pill(sight_t)
    return { mx = 103, my = 104, health = 15, attack_damage = 0, owner = "friendly",
             _enemy_near_tick = sight_t }
  end
  local function score(s, inf, now, sight_t)
    local c, bd = G._defend_pill_score(s, { pills = {}, bases = {} }, inf, pill(sight_t), 50, now, 200, 200)
    return c, bd
  end
  C.FOCUS_SPIKE_DEFEND_DISCOUNT, C.FOCUS_SPIKE_DEFEND_TICKS = 0.30, 6000
  -- defend_mult: plan pills only, inside 6000 ticks of landing.
  check("defend_mult: plan pill inside 6000 ticks = 0.70",
        near(SP.defend_mult(state_with("plan"), 103, 104, 1000 + 6000), 0.70))
  check("defend_mult: after 6000 ticks = nil", SP.defend_mult(state_with("plan"), 103, 104, 1000 + 6001) == nil)
  check("defend_mult: a pill not from a plan (own) = nil", SP.defend_mult(state_with("own"), 103, 104, 1100) == nil)
  check("defend_mult: an ally advert pill = nil", SP.defend_mult(state_with("ally"), 103, 104, 1100) == nil)
  check("defend_mult: no tag = nil", SP.defend_mult(state_with(nil), 103, 104, 1100) == nil)
  -- The defend_pill cost: x 0.70 inside the time, unchanged after.
  local c0, b0 = score(state_with(nil, 2000), info_far(), 2000)
  local c1, b1 = score(state_with("plan", 2000), info_far(), 2000)
  local c2, b2 = score(state_with("plan", 7001), info_far(), 7001)
  local function base(c, b) return c - (b.tb or 0) end
  check("defend cost: plan pill x 0.70 (chip value)", b1.spike_defend and near(b1.spike_defend, 0.70)
        and math.abs(base(c1, b1) - 0.70 * base(c0, b0)) < 1e-6, c0 .. " -> " .. c1)
  check("defend cost: after 6000 ticks no discount", b2.spike_defend == nil and near(base(c2, b2), base(c0, b0)),
        c2)
  local co, bo = score(state_with("own", 2000), info_far(), 2000)
  check("defend cost: a pill not from a plan gets no discount", bo.spike_defend == nil and near(co, c0), co)
  -- Held: an enemy seen near the pill 10 ticks ago (sight tier, under 500),
  -- one ally tank at the pill, no foes now. The discount comes first, the
  -- well-defended clamp still raises the cost to DEFEND_WELL_DEFENDED_COST.
  local cs, bs = score(state_with("plan", 2000), info_far(), 2000, 1990)
  check("sight plan pill, not held: discounted, under 500", bs.spike_defend ~= nil
        and base(cs, bs) < C.DEFEND_WELL_DEFENDED_COST, cs)
  local ally = { { type = OBJECT_TANK, info = 0, x = 104 * 256, y = 104 * 256 } }
  local ch, bh = score(state_with("plan", 2000), info_far(ally), 2000, 1990)
  check("held plan pill: still clamps to 500", bh.welldef ~= nil and bh.spike_defend ~= nil
        and near(base(ch, bh), C.DEFEND_WELL_DEFENDED_COST), ch)
  -- KEEL: discount 0, no change.
  C.FOCUS_SPIKE_DEFEND_DISCOUNT = C.PRESETS.keel.FOCUS_SPIKE_DEFEND_DISCOUNT
  check("KEEL: discount 0 (off)", C.PRESETS.keel.FOCUS_SPIKE_DEFEND_DISCOUNT == 0)
  check("KEEL: defend_mult nil on a plan pill", SP.defend_mult(state_with("plan"), 103, 104, 1100) == nil)
  local ck, bk = score(state_with("plan", 2000), info_far(), 2000)
  check("KEEL: defend cost unchanged", bk.spike_defend == nil and near(ck, c0), ck)
  check("presets: keel_focus, _loose, _bench 0.30 / 6000; default 0",
        C.PRESETS.keel_focus.FOCUS_SPIKE_DEFEND_DISCOUNT == 0.30
        and C.PRESETS.keel_focus_loose.FOCUS_SPIKE_DEFEND_DISCOUNT == 0.30
        and C.PRESETS.keel_focus_bench.FOCUS_SPIKE_DEFEND_DISCOUNT == 0.30
        and C.PRESETS.keel_focus.FOCUS_SPIKE_DEFEND_TICKS == 6000
        and C.PRESETS.keel_focus_bench.FOCUS_SPIKE_DEFEND_TICKS == 6000
        and old_d == 0)
  C.FOCUS_SPIKE_DEFEND_DISCOUNT, C.FOCUS_SPIKE_DEFEND_TICKS = old_d, old_t
end

-- â”€â”€ Euclid builder leash (BUILDER_LEASH_EUCLID, 2026-11-02) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
print("builder leash euclid")
do
  C.BUILDER_POOL_REPAIR_LINEAR = true
  local saved_eu = C.BUILDER_LEASH_EUCLID
  local real_trip = BP.lgm_trip
  BP.lgm_trip = function() return 50, 100, nil end
  C.BUILDER_LEASH_EUCLID = true
  local st = fresh_state(3000)
  st._spike = { pair = nil, watch = {}, care = {}, cool = {} }
  -- run11 session 2: dead pill #2 at (120,114), tank at (126,109).
  local w = { pills = { [2] = pill(120, 114, "friendly", 0) }, bases = {} }
  local rows = BP.discover(st, w, info_at(126, 109, 0, 40))
  local r = rows[1]
  check("(126,109)->(120,114): rebuild row, leash 11, in reach",
        r and r.type == "rebuild" and r.leash == 11 and not r.out_of_leash, r and r.dist)
  check("(126,109)->(120,114): dist is straight line 7.8",
        r and math.abs(r.dist - math.sqrt(61)) < 1e-9, r and r.dist)
  check("builder_can_repair (126,109)->(120,114) in reach",
        BP.builder_can_repair(st, w, info_at(126, 109, 0, 40), w.pills[2]) ~= nil)
  -- (126,108): Euclid 8.5 (in), Manhattan 12 (out).
  rows = BP.discover(st, w, info_at(126, 108, 0, 40))
  check("(126,108): euclid 8.5 is in reach", rows[1] and not rows[1].out_of_leash, rows[1] and rows[1].dist)
  check("builder_can_repair (126,108) in reach",
        BP.builder_can_repair(st, w, info_at(126, 108, 0, 40), w.pills[2]) ~= nil)
  -- Knob off: the old Manhattan measure.
  C.BUILDER_LEASH_EUCLID = false
  rows = BP.discover(st, w, info_at(126, 108, 0, 40))
  check("knob off: (126,108) Manhattan 12 -> out_of_leash",
        rows[1] and rows[1].out_of_leash and rows[1].dist == 12, rows[1] and rows[1].dist)
  check("knob off: builder_can_repair (126,108) refuses",
        BP.builder_can_repair(st, w, info_at(126, 108, 0, 40), w.pills[2]) == nil)
  C.BUILDER_LEASH_EUCLID = true
  -- Panel text: d with one decimal. (126,103) to (120,114) is 12.5 straight line.
  rows = BP.discover(st, w, info_at(126, 103, 0, 40))
  local ro = rows[1]
  check("(126,103): euclid 12.5 > 11 -> out_of_leash", ro and ro.out_of_leash, ro and ro.dist)
  if ro then
    ro.trip = 100
    BP.gate_row(st, w, info_at(126, 103, 0, 40), 3000, ro, { ok = true, reserve = 0 })
  end
  check("reject text 'out_of_leash (12.5 > 11)'",
        ro and ro.reject == "out_of_leash (12.5 > 11)", ro and ro.reject)
  C.BUILDER_LEASH_EUCLID = saved_eu
  BP.lgm_trip = real_trip
end

-- â”€â”€ Euclid long send (PLACE_DISPATCH_MAX_DIST 15, BUILDER_LEASH_EUCLID) â”€â”€
print("long send euclid")
do
  local saved_eu = C.BUILDER_LEASH_EUCLID
  local FV = require("focus_vote")
  C.BUILDER_LEASH_EUCLID = true
  check("leash_dist 10 across, 8 down = 12.8",
        math.abs(U.leash_dist(92, 94, 102, 102) - math.sqrt(164)) < 1e-9)
  -- Tank (92,94), first spike (100,100), second tile (102,102): 12.8 from the
  -- tank (Manhattan 18), 2.8 from the first spike.
  local diag = { role = "second", fmx = 100, fmy = 100, tmx = 92, tmy = 94 }
  check("second_ok: 10 across, 8 down = 12.8 <= 15 is in reach", SP.second_ok(diag, 102, 102))
  check("second_ok: 11 across, 11 down = 15.6 > 15 is out",
        not SP.second_ok({ role = "second", fmx = 100, fmy = 100, tmx = 92, tmy = 92 }, 103, 103))
  check("leash_row_w(15, 9) = 12 (circle)", U.leash_row_w(15, 9) == 12, U.leash_row_w(15, 9))
  check("leash_row_w(15, 15) = 0", U.leash_row_w(15, 15) == 0)
  local x, y = FV.stand_on_line(120, 116, { mx = 100, my = 100 })
  check("stand_on_line: straight-line 15 from the spot", x == 112 and y == 109
        and U.edist(x, y, 100, 100) <= 15 + 1e-9, string.format("(%s,%s)", x, y))
  C.BUILDER_LEASH_EUCLID = false
  check("knob off: leash_dist is Manhattan 18", U.leash_dist(92, 94, 102, 102) == 18)
  check("knob off: second_ok 10 across, 8 down (Manhattan 18) is out",
        not SP.second_ok(diag, 102, 102))
  check("knob off: leash_row_w(15, 9) = 6 (diamond)", U.leash_row_w(15, 9) == 6)
  x, y = FV.stand_on_line(120, 116, { mx = 100, my = 100 })
  check("knob off: stand_on_line Manhattan 15 from the spot", x == 108 and y == 107,
        string.format("(%s,%s)", x, y))
  C.BUILDER_LEASH_EUCLID = saved_eu
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
