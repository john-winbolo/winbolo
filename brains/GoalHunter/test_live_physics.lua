-- =========================================================================
-- test_live_physics.lua -- standalone tests for live_physics.lua.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_live_physics.lua
-- or with any lua on PATH.
--
-- 1. Classic rules change nothing: every constant keeps its exact value
--    (and table identity), and nothing is pushed to C.
-- 2. A scaled scenario (full armour 20, taken 180%, speed 145%, full
--    shells 8) gives the hand-computed numbers.
-- 3. A missing info.rules, and LIVE_PHYSICS = false, change nothing.
-- 7. Ice Rink (speed 140, accel 35, turn 55) and Juggernaut (speed 65,
--    accel 60) scale the turn-radius caps and brake distances.
-- 8. STEER_TURN_SPEEDUP: a turn faster than the top speed raises the
--    cornering cap (turn 150, Juggernaut), capped; classic / Ice / Turbo not.
-- 9. STEER_TERRAIN_TURN: the turn entries follow the live / classic turn
--    rate of the terrain under the tank; classic terrain rates change nothing.
-- 10. ENEMY_SPEED_OWN_MODS: enemy_speed_scale() is our speed % / 100, and
--    nil at speed 100, with the knob off, or with LIVE_PHYSICS off.
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

-- C-side stubs: record every call.
local calls
local function reset_calls()
  calls = { cpf_speed = {}, cpf_cost = {}, wsim_speed = {}, wsim_rules = {}, cpf_man = {} }
end
reset_calls()
_G.cpf_set_terrain_speed  = function(t, v) calls.cpf_speed[#calls.cpf_speed + 1] = { t, v } end
_G.cpf_set_terrain_cost   = function(t, v) calls.cpf_cost[#calls.cpf_cost + 1] = { t, v } end
_G.wsim_set_terrain_speed = function(t, v) calls.wsim_speed[#calls.wsim_speed + 1] = { t, v } end
_G.wsim_set_rules         = function(r) calls.wsim_rules[#calls.wsim_rules + 1] = r end
_G.cpf_set_man_speed      = function(t, v) calls.cpf_man[#calls.cpf_man + 1] = { t, v } end

-- Fresh modules for each section.
local function fresh()
  for _, m in ipairs({ "constants", "live_physics", "cpathfinder", "cworldsim" }) do
    package.loaded[m] = nil
  end
  local C = require("constants")
  require("cpathfinder")
  require("cworldsim")
  local LP = require("live_physics")
  reset_calls()
  return C, LP
end

-- The rules table the engine sends under classic rules (braincore.c).
local function classic_rules()
  return {
    tank_reload_ticks = 13, tank_full_shells = 40, tank_full_mines = 40,
    tank_full_trees = 40, tank_full_armour = 40, tank_death_ticks = 255,
    tank_water_ticks = 15, mine_damage = 10, just_fired_ticks = 5,
    tank_min_move = 6, tank_accel_rate = 0.25, tank_decel_rate = 0.25,
    tank_brake_rate = 0.25, tank_autoslow_rate = 0.25,
    speed_road = 16, speed_grass = 12, speed_forest = 6, speed_river = 3,
    speed_swamp = 3, speed_crater = 3, speed_rubble = 3, speed_boat = 16,
    speed_deep_sea = 3, speed_refuel_base = 16,
    turn_road = 1, turn_grass = 1, turn_forest = 0.5, turn_river = 0.25,
    turn_swamp = 0.25, turn_crater = 0.25, turn_rubble = 0.25, turn_boat = 1,
    turn_deep_sea = 0.5, turn_refuel_base = 1,
    pill_max_armour = 15, pill_attack_ticks = 100, pill_attack_min_ticks = 6,
    base_full_armour = 90, base_full_shells = 90, base_full_mines = 90,
    shell_damage = 5, shell_life = 8, shell_speed = 32, gunsight_max = 14,
    tree_hide_distance = 768,
    pill_cooldown_ticks = 32, pill_repair_amount = 4, pill_range = 2048,
    pill_shell_damage = 1, pill_angry_divisor = 2,
    base_capture_armour = 9, base_hit_armour = 4, base_regen_ticks = 1000,
    lgm_build_ticks = 20, lgm_cost_road = 2, lgm_cost_building = 2,
    lgm_cost_pill_repair = 1, lgm_cost_boat = 20, lgm_cost_pill_new = 4,
    lgm_cost_mine = 1, lgm_gather_trees = 4, lgm_helicopter_speed = 3,
    man_speed_road = 16, man_speed_grass = 16, man_speed_forest = 8,
    man_speed_river = 0, man_speed_swamp = 4, man_speed_crater = 4,
    man_speed_rubble = 4, man_speed_boat = 16, man_speed_deep_sea = 0,
    man_speed_refuel_base = 16,
  }
end
local function classic_mods()
  return { speed = 100, accel = 100, turn = 100, reload = 100, dealt = 100, taken = 100 }
end

-- The pre-change numbers, written out by hand (not read from constants.lua,
-- so a later edit to a default cannot hide a drift here).
local OLD = {
  TANK_FULL_ARMOUR = 40, TANK_FULL_SHELLS = 40, VULN_ARMOUR_CAP = 40,
  ARMOUR_CRITICAL = 5, ARMOUR_LOW = 15, ARMOUR_MODERATE = 25,
  ATTACK_PILL_UNSAFE_HP_THRESHOLD = 13, TANK_COMBAT_FLEE_ARMOUR = 0,
  ATTACK_RUSH_MIN_ARMOUR = 5, IMMINENT_CAPTURE_MIN_ARMOUR = 8,
  TANK_COMBAT_MIN_ARMOUR = 10, BASE_STEAL_MIN_ARMOUR = 8,
  DEFEND_READY_ARMOUR = 15, ARMOUR_COMBAT = 30, ATTACK_PILL_RISKY_ARMOUR = 30,
  ATTACK_PILL_UNSAFE_ARMOUR_FLOOR = 20, ANGER_WAIT_HIGH_ARMOUR_ARM = 30,
  SQUAD_COMMANDER_MIN_ARMOUR = 30, SQUAD_REFUEL_OK_ARMOUR = 25,
  ARMOUR_PER_PILL_HP = 2, SWERVE_SKIP_ARMOUR_PER_HP = 5,
  ENGAGE_MAX_INCOMING_TICKS = 45,
  SHELLS_LOW = 19, SHELLS_COMBAT = 30, REFUEL_BASELINE_SHELLS = 25,
  DEFEND_READY_SHELLS = 20, AMMO_DEPRIVED_SHELLS = 10,
  TANK_COMBAT_MIN_SHELLS = 10, REFUEL_PER_ENEMY_TANK = 6,
  TANK_SHELL_DAMAGE = 5, TTK_TICKS_PER_HIT = 8, FIRE_HOLD_TICKS = 0,
  PILLS_MAX_HEALTH = 15, PILL_REPAIR_AMOUNT = 4, PILL_CHAIN_MIN_HP = 5,
  PPT_HEALTH_THRESHOLD = 8, TANK_FINISH_MAX_HP = 3, HEAT_PILL_MIN_HP = 6,
  HARD_TAKE_MIN_HP = 12, PILL_FIRE_RANGE = 8, ATTACK_PILL_RANGE = 9.5,
  HARDLINE_ENGAGE_RANGE = 10, PILL_ANGER_DECAY = 3000,
  BASE_FULL_ARMOUR = 90, BASE_CAPTURE_ARMOUR = 9, BASE_SHELL_DAMAGE = 5,
  BASE_FULL_SHELLS_TO_KILL = 17, BASE_STEAL_MAX_ARMOUR = 24,
  BASE_MARKUP_STALE = 1200, BASE_STEAL_OBS_STALE = 1200,
  LGM_BUILD_TIME = 20, PILL_PLACE_TREE_COST = 4, REPAIR_DEAD_MIN_TREES = 4,
  ROAD_RIVER_COST = 2, BASE_SHIELD_BUILD_COST = 2, WALL_SHIELD_BUILD_COST = 2,
  SEA_BOAT_TREES = 20, SEA_MINE_TREES = 1, SEA_PILL_TREES_TOTAL = 21,
  SEA_TREES_PER_FOREST = 4, ENEMY_LGM_RETURN_TICKS = 3000,
  NAV_TOP_SPEED = 64, NAV_CRUISE_SPEED = 48,
  SWERVE_TURN_TICKS = 35, SWERVE_DEFENSIVE_TURN_TICKS = 40,
  SQUAD_BLITZ_READY_TIMEOUT = 550,
  SHELL_SPEED = 32, TANK_COMBAT_SHELL_SPEED = 32, GUNSIGHT_MAX = 13.875,
  NAV_TURN_TOP_SPEED = 64, NAV_TURN_CRUISE_SPEED = 48,
  ORBIT_FAST_SPEED = 16, ORBIT_BRAKE_SPEED = 8, SEA_NOGO_SPEED_CAP = 16,
  BOAT_ALIGN_CRAWL = 8, KILL_LGM_TURN_DELTA = 6,
  NAV_BRAKE_MULT = 12, NAV_BRAKE_MULT_PRECISE = 24, CLIFF_NAV_LOOK_TICKS = 10,
  INRANGE_COAST_PER_SPEED = 2, NAV_BRAKE_FACTOR = 0.08,
  NAV_BRAKE_FACTOR_PRECISE = 0.04, AP_BRAKE_ZONE_FACTOR = 0.03,
  CLIFF_LOOK_MAX_WU = 1280, CLIFF_NAV_LOOK_MAX_STEPS = 6, KILL_LGM_SPEED_DELTA = 2,
  MAN_SPEED_BLESSED = 16, REPAIR_DEAD_GRASS_TICKS_PER_TILE = 16,
  BUILDER_POOL_GRASS_TICKS_PER_TILE = 16, WSIM_DWELL_TICKS_PER_TILE = 16,
  WSIM_DWELL_BUILD_TICKS = 20, PILLBOX_RANGE_WU = 2048,
  SHELL_MAX_STEPS = 64, LGM_SHELL_PREDICT_TICKS = 63,
}

local function snapshot(C, LP)
  local s = {}
  for _, e in ipairs(LP.ENTRIES) do s[e[1]] = C[e[1]] end
  return s
end

local function no_pushes(label)
  check(label .. ": no cpf speed push", #calls.cpf_speed == 0, #calls.cpf_speed)
  check(label .. ": no cpf cost push", #calls.cpf_cost == 0, #calls.cpf_cost)
  check(label .. ": no wsim speed push", #calls.wsim_speed == 0, #calls.wsim_speed)
  check(label .. ": no wsim rules push", #calls.wsim_rules == 0, #calls.wsim_rules)
  check(label .. ": no cpf man speed push", #calls.cpf_man == 0, #calls.cpf_man)
end

local function unchanged(label, C, LP, before)
  local bad = {}
  for _, e in ipairs(LP.ENTRIES) do
    local k = e[1]
    if not rawequal(C[k], before[k]) then bad[#bad + 1] = k end
  end
  check(label .. ": every constant unchanged (value and table identity)",
        #bad == 0, table.concat(bad, ","))
end

-- ---------------------------------------------------------------------------
print("1. classic rules")
do
  local C, LP = fresh()
  -- Every entry names a constant that exists.
  local missing = {}
  for _, e in ipairs(LP.ENTRIES) do
    if C[e[1]] == nil then missing[#missing + 1] = e[1] end
  end
  check("every entry names an existing constant", #missing == 0, table.concat(missing, ","))
  for k, v in pairs(OLD) do
    check("default " .. k .. " = " .. tostring(v), C[k] == v, C[k])
  end
  local before = snapshot(C, LP)
  local info = { rules = classic_rules(), mods = classic_mods(), reload_ticks = 13 }
  for _ = 1, 3 do LP.apply(info) end
  unchanged("classic", C, LP, before)
  for k, v in pairs(OLD) do
    check("classic " .. k .. " still " .. tostring(v), C[k] == v, C[k])
  end
  no_pushes("classic")

  -- A difficulty / cfg value is the base and survives classic rules.
  C.ARMOUR_LOW = 18
  C.SHELLS_LOW = 22
  LP.apply(info)
  check("cfg ARMOUR_LOW 18 kept", C.ARMOUR_LOW == 18, C.ARMOUR_LOW)
  check("cfg SHELLS_LOW 22 kept", C.SHELLS_LOW == 22, C.SHELLS_LOW)
  no_pushes("classic after cfg")
end

-- ---------------------------------------------------------------------------
print("2. scaled: full armour 20, taken 180%, speed 145%, full shells 8")
do
  local C, LP = fresh()
  local r = classic_rules()
  r.tank_full_armour = 20
  r.tank_full_shells = 8
  local m = classic_mods()
  m.taken = 180
  m.speed = 145
  local info = { rules = r, mods = m, reload_ticks = 13 }
  local x = LP.context(info)
  check("hit_taken = floor((5*180+50)/100) = 9", x.hit_taken == 9, x.hit_taken)
  LP.apply(info)
  local E = {
    TANK_FULL_ARMOUR = 20, VULN_ARMOUR_CAP = 20, TANK_FULL_SHELLS = 8,
    -- a_hits: rnd(v*9/5), capped at 20-9 = 11
    ARMOUR_CRITICAL = 9, ARMOUR_LOW = 11, ARMOUR_MODERATE = 11,
    TANK_COMBAT_MIN_ARMOUR = 11, IMMINENT_CAPTURE_MIN_ARMOUR = 11,
    TANK_COMBAT_FLEE_ARMOUR = 0, ATTACK_RUSH_MIN_ARMOUR = 9,
    -- a_full: rnd(v*20/40)
    ARMOUR_COMBAT = 15, ATTACK_PILL_RISKY_ARMOUR = 15,
    ATTACK_PILL_UNSAFE_ARMOUR_FLOOR = 10, SQUAD_REFUEL_OK_ARMOUR = 13,
    -- engage: rnd(45 * (20*5*6) / (40*9*6)) = rnd(12.5) = 13
    ENGAGE_MAX_INCOMING_TICKS = 13,
    ARMOUR_PER_PILL_HP = 2 * 9 / 5, SWERVE_SKIP_ARMOUR_PER_HP = 9,
    -- shells: fraction of 8, "low" lines capped at 7
    SHELLS_COMBAT = 6, REFUEL_BASELINE_SHELLS = 5, DEFEND_READY_SHELLS = 4,
    AMMO_DEPRIVED_SHELLS = 2, SHELLS_LOW = 7,
    -- tank kills: ceil(20/5) = 4 shells, so rnd(v*4/8)
    TANK_COMBAT_MIN_SHELLS = 5, REFUEL_PER_ENEMY_TANK = 3,
    TANK_SHELL_DAMAGE = 5,
    -- untouched groups
    PILLS_MAX_HEALTH = 15, TTK_TICKS_PER_HIT = 8, LGM_BUILD_TIME = 20,
    NAV_TOP_SPEED = 64 * 1.45, NAV_CRUISE_SPEED = 48 * 1.45,
  }
  for k, v in pairs(E) do
    check(string.format("scaled %s = %s", k, tostring(v)),
          math.abs((C[k] or -1e9) - v) < 1e-9, C[k])
  end
  check("TERRAIN_SPEED road 16 -> 23.2", math.abs(C.TERRAIN_SPEED[C.T_ROAD] - 23.2) < 1e-9,
        C.TERRAIN_SPEED[C.T_ROAD])
  check("TERRAIN_SPEED grass 12 -> 17.4", math.abs(C.TERRAIN_SPEED[C.T_GRASS] - 17.4) < 1e-9,
        C.TERRAIN_SPEED[C.T_GRASS])
  -- MAP_SPEED models enemies: rules unchanged, so the same table object.
  check("MAP_SPEED unchanged (enemy speed % unseen)", C.MAP_SPEED[C.T_ROAD] == 16, C.MAP_SPEED[C.T_ROAD])
  check("TERRAIN_COST_LAND unchanged (speed % is uniform)", C.TERRAIN_COST_LAND[C.T_GRASS] == 2,
        C.TERRAIN_COST_LAND[C.T_GRASS])
  -- C pushes: our speed table x1.45 to pathfinder and world sim, costs
  -- untouched, world-sim rules with 9 armour per hit.
  local road
  for _, c in ipairs(calls.cpf_speed) do if c[1] == C.T_ROAD then road = c[2] end end
  check("cpf road speed pushed 23.2", road and math.abs(road - 23.2) < 1e-9, road)
  road = nil
  for _, c in ipairs(calls.wsim_speed) do if c[1] == C.T_ROAD then road = c[2] end end
  check("wsim road speed pushed 23.2", road and math.abs(road - 23.2) < 1e-9, road)
  check("no cpf cost push", #calls.cpf_cost == 0, #calls.cpf_cost)
  check("wsim rules pushed once", #calls.wsim_rules == 1, #calls.wsim_rules)
  local wr = calls.wsim_rules[1] or {}
  check("wsim shell_damage 9", wr.shell_damage == 9, wr.shell_damage)
  check("wsim shoot_interval 8", wr.shoot_interval == 8, wr.shoot_interval)

  -- Same info again: nothing new is pushed.
  reset_calls()
  LP.apply(info)
  no_pushes("scaled repeat")
  check("scaled repeat keeps ARMOUR_LOW 11", C.ARMOUR_LOW == 11, C.ARMOUR_LOW)

  -- Switch off: every constant goes back, and C gets the classic values.
  C.LIVE_PHYSICS = false
  reset_calls()
  LP.apply(info)
  for k, v in pairs(OLD) do
    check("switch off restores " .. k, C[k] == v, C[k])
  end
  check("switch off restores TERRAIN_SPEED table", C.TERRAIN_SPEED[C.T_ROAD] == 16,
        C.TERRAIN_SPEED[C.T_ROAD])
  check("switch off re-pushes classic cpf speeds", #calls.cpf_speed > 0, #calls.cpf_speed)
  check("switch off re-pushes classic wsim rules", #calls.wsim_rules == 1
        and calls.wsim_rules[1].shell_damage == 5, #calls.wsim_rules)
end

-- ---------------------------------------------------------------------------
print("3. more scaled rules")
do
  local C, LP = fresh()
  local r = classic_rules()
  r.pill_max_armour = 30
  r.lgm_cost_pill_new = 0      -- PillboxTag: a free pill (0 is legal)
  r.lgm_build_ticks = 10       -- Infection
  r.base_regen_ticks = 250     -- Infection
  r.tank_reload_ticks = 13
  r.speed_forest = 12          -- forest as fast as grass
  local m = classic_mods()
  m.reload = 160               -- Infection: 13*160/100 = 20.8 -> 21
  m.dealt = 130                -- 5*130 = 6.5 -> 7 per hit
  local info = { rules = r, mods = m, reload_ticks = 21 }
  LP.apply(info)
  local E = {
    PILLS_MAX_HEALTH = 30, ATTACK_PILL_UNSAFE_HP_THRESHOLD = 26, HARD_TAKE_MIN_HP = 24,
    PILL_PLACE_TREE_COST = 0, REPAIR_DEAD_MIN_TREES = 0,
    LGM_BUILD_TIME = 10, BASE_MARKUP_STALE = 300, BASE_STEAL_OBS_STALE = 300,
    TTK_TICKS_PER_HIT = 8 * 21 / 13, TANK_SHELL_DAMAGE = 7,
    -- ceil(40/7) = 6 shells kill a tank: rnd(10*6/8) = 8
    TANK_COMBAT_MIN_SHELLS = 8,
    -- 30-HP pill needs 30 shells: rnd(19*30/15) = 38, capped at 39
    SHELLS_LOW = 38,
  }
  for k, v in pairs(E) do
    check(string.format("scaled %s = %s", k, tostring(v)),
          math.abs((C[k] or -1e9) - v) < 1e-9, C[k])
  end
  -- forest 6 -> 12: cost 3 -> 1.5 (road cost stays 1).
  check("TERRAIN_COST_LAND forest 3 -> 1.5", C.TERRAIN_COST_LAND[C.T_FOREST] == 1.5,
        C.TERRAIN_COST_LAND[C.T_FOREST])
  check("TERRAIN_COST_LAND road stays 1", C.TERRAIN_COST_LAND[C.T_ROAD] == 1,
        C.TERRAIN_COST_LAND[C.T_ROAD])
  check("MAP_SPEED forest 12 (a rule, seen by all tanks)", C.MAP_SPEED[C.T_FOREST] == 12,
        C.MAP_SPEED[C.T_FOREST])
  local forest_cost
  for _, c in ipairs(calls.cpf_cost) do if c[1] == C.T_FOREST then forest_cost = c[2] end end
  check("cpf forest cost pushed 1.5", forest_cost == 1.5, forest_cost)
  local wr = calls.wsim_rules[1] or {}
  check("wsim shoot_interval rnd(8*21/13) = 13", wr.shoot_interval == 13, wr.shoot_interval)
  -- a cfg change mid-game becomes the new base and is scaled
  C.HARD_TAKE_MIN_HP = 10
  LP.apply(info)
  check("cfg HARD_TAKE_MIN_HP 10 re-based and scaled to 20", C.HARD_TAKE_MIN_HP == 20,
        C.HARD_TAKE_MIN_HP)
end

-- ---------------------------------------------------------------------------
print("4. fallback: no info.rules / no info.mods")
do
  local C, LP = fresh()
  local before = snapshot(C, LP)
  LP.apply({})
  LP.apply({ rules = nil, mods = nil })
  LP.apply(nil)
  unchanged("no rules", C, LP, before)
  no_pushes("no rules")
  -- zero / garbage values read as classic
  local r = classic_rules()
  r.tank_full_armour = 0
  r.shell_damage = 0
  r.pill_max_armour = -3
  LP.apply({ rules = r, mods = { speed = 0, taken = 0 } })
  unchanged("zero rules", C, LP, before)
  no_pushes("zero rules")
end

-- ---------------------------------------------------------------------------
print("5. keel: LIVE_PHYSICS = false")
do
  local C, LP = fresh()
  check("PRESETS.keel.LIVE_PHYSICS == false", C.PRESETS.keel.LIVE_PHYSICS == false,
        tostring(C.PRESETS.keel.LIVE_PHYSICS))
  C.LIVE_PHYSICS = false
  local before = snapshot(C, LP)
  local r = classic_rules()
  r.tank_full_armour = 20
  local m = classic_mods()
  m.taken = 180
  LP.apply({ rules = r, mods = m })
  unchanged("keel", C, LP, before)
  no_pushes("keel")
end

-- ---------------------------------------------------------------------------
print("6. one group off")
do
  local C, LP = fresh()
  C.LIVE_PHYSICS_ARMOUR = false
  local r = classic_rules()
  r.tank_full_armour = 20
  r.tank_full_shells = 20
  LP.apply({ rules = r, mods = classic_mods() })
  check("ARMOUR group off: ARMOUR_LOW stays 15", C.ARMOUR_LOW == 15, C.ARMOUR_LOW)
  check("ARMOUR group off: TANK_FULL_ARMOUR stays 40", C.TANK_FULL_ARMOUR == 40, C.TANK_FULL_ARMOUR)
  check("SHELLS group on: TANK_FULL_SHELLS 20", C.TANK_FULL_SHELLS == 20, C.TANK_FULL_SHELLS)
end

-- ---------------------------------------------------------------------------
print("7. Ice Rink and Juggernaut: turn-radius caps and brake distances")
do
  local C, LP = fresh()
  local m = classic_mods()
  m.speed, m.accel, m.turn = 140, 35, 55
  LP.apply({ rules = classic_rules(), mods = m, reload_ticks = 13 })
  -- brake 0.25 x 35% = 0.0875 per tick: stop terms x 25/8.75, gains x 8.75/25.
  -- Longest stop: (16*140)^2 * 25 / ((16*100)^2 * 8.75) = 5.6.
  local E = {
    NAV_TOP_SPEED = 64 * 1.4, NAV_CRUISE_SPEED = 48 * 1.4,
    NAV_TURN_TOP_SPEED = 64 * 0.55, NAV_TURN_CRUISE_SPEED = 48 * 0.55,
    ORBIT_FAST_SPEED = 9, ORBIT_BRAKE_SPEED = 4, SEA_NOGO_SPEED_CAP = 9,
    BOAT_ALIGN_CRAWL = 4, KILL_LGM_TURN_DELTA = 6 * 0.55,
    NAV_BRAKE_MULT = 12 * 25 / 8.75, NAV_BRAKE_MULT_PRECISE = 24 * 25 / 8.75,
    CLIFF_NAV_LOOK_TICKS = 10 * 25 / 8.75, INRANGE_COAST_PER_SPEED = 2 * 25 / 8.75,
    NAV_BRAKE_FACTOR = 0.08 * 8.75 / 25, NAV_BRAKE_FACTOR_PRECISE = 0.04 * 8.75 / 25,
    AP_BRAKE_ZONE_FACTOR = 0.03 * 8.75 / 25,
    CLIFF_LOOK_MAX_WU = 1280 * 5.6, CLIFF_NAV_LOOK_MAX_STEPS = 34,
    KILL_LGM_SPEED_DELTA = 2 * 8.75 / 25,
    SWERVE_TURN_TICKS = 64,   -- rnd(35 / 0.55)
  }
  for k, v in pairs(E) do
    check(string.format("ice %s = %s", k, tostring(v)),
          math.abs((C[k] or -1e9) - v) < 1e-9, C[k])
  end

  local C2, LP2 = fresh()
  local m2 = classic_mods()
  m2.speed, m2.accel = 65, 60
  LP2.apply({ rules = classic_rules(), mods = m2, reload_ticks = 13 })
  local E2 = {
    NAV_TURN_TOP_SPEED = 64, NAV_TURN_CRUISE_SPEED = 48, ORBIT_FAST_SPEED = 16,
    NAV_BRAKE_MULT = 12 * 25 / 15, NAV_BRAKE_FACTOR = 0.08 * 15 / 25,
    -- longest stop 0.70 of classic: the scan caps never shrink
    CLIFF_LOOK_MAX_WU = 1280, CLIFF_NAV_LOOK_MAX_STEPS = 6,
    KILL_LGM_SPEED_DELTA = 2 * 15 / 25,
  }
  for k, v in pairs(E2) do
    check(string.format("juggernaut %s = %s", k, tostring(v)),
          math.abs((C2[k] or -1e9) - v) < 1e-9, C2[k])
  end

  -- ACCEL group off: brake terms stay classic, turn terms still scale.
  local C3, LP3 = fresh()
  C3.LIVE_PHYSICS_ACCEL = false
  LP3.apply({ rules = classic_rules(), mods = m, reload_ticks = 13 })
  check("ACCEL off: NAV_BRAKE_MULT stays 12", C3.NAV_BRAKE_MULT == 12, C3.NAV_BRAKE_MULT)
  check("ACCEL off: NAV_TURN_CRUISE_SPEED still 26.4",
        math.abs(C3.NAV_TURN_CRUISE_SPEED - 26.4) < 1e-9, C3.NAV_TURN_CRUISE_SPEED)
end

-- util holds its own C and live_physics: load it again after fresh().
local function fresh_util()
  package.loaded["util"] = nil
  return require("util")
end

-- ---------------------------------------------------------------------------
print("8. STEER_TURN_SPEEDUP: a faster turn raises the cornering cap")
do
  local function run(speed, accel, turn, knob)
    local C, LP = fresh()
    if knob ~= nil then C.STEER_TURN_SPEEDUP = knob end
    local m = classic_mods()
    m.speed, m.accel, m.turn = speed, accel, turn
    LP.apply({ rules = classic_rules(), mods = m, reload_ticks = 13 })
    local U = fresh_util()
    return C, LP, U
  end
  local near = function(a, b) return math.abs(a - b) < 1e-9 end

  -- Classic: no ratio, the cap is exactly the old math.min.
  local C, LP, U = run(100, 100, 100)
  check("classic: turn_speedup nil", LP.turn_speedup() == nil, LP.turn_speedup())
  check("classic: cruise cap 48", U.nav_turn_cap(C.NAV_CRUISE_SPEED, C.NAV_TURN_CRUISE_SPEED) == 48)
  check("classic: top cap 64", U.nav_turn_cap(C.NAV_TOP_SPEED, C.NAV_TURN_TOP_SPEED) == 64)
  check("classic: cap keeps integer type",
        math.type == nil or math.type(U.nav_turn_cap(48, 48)) == math.type(48))

  -- Turn 150 alone: ratio 1.5, cruise 48 -> min(48 x 1.25, 64) = 60; top stays 64.
  C, LP, U = run(100, 100, 150)
  check("turn150: turn_speedup 1.5", near(LP.turn_speedup(), 1.5), LP.turn_speedup())
  local cc = U.nav_turn_cap(C.NAV_CRUISE_SPEED, C.NAV_TURN_CRUISE_SPEED)
  check("turn150: cruise cap 60", near(cc, 60), cc)
  check("turn150: top cap 64", near(U.nav_turn_cap(C.NAV_TOP_SPEED, C.NAV_TURN_TOP_SPEED), 64))

  -- Juggernaut (speed 65, turn 100): ratio 100/65 = 1.538; cruise 31.2 ->
  -- 31.2 x 1.25 = 39; top 41.6 stays 41.6 (the live top speed).
  C, LP, U = run(65, 60, 100)
  check("jugg: turn_speedup 100/65", near(LP.turn_speedup(), 100 / 65), LP.turn_speedup())
  cc = U.nav_turn_cap(C.NAV_CRUISE_SPEED, C.NAV_TURN_CRUISE_SPEED)
  check("jugg: cruise cap 39", near(cc, 39), cc)
  local tc = U.nav_turn_cap(C.NAV_TOP_SPEED, C.NAV_TURN_TOP_SPEED)
  check("jugg: top cap 41.6", near(tc, 41.6), tc)

  -- Ice Rink (speed 140, turn 55) and Turbo (170, turn 150): the turn is the
  -- slower one, no speedup; the cap is the turn-radius cap as before.
  C, LP, U = run(140, 35, 55)
  check("ice: turn_speedup nil", LP.turn_speedup() == nil, LP.turn_speedup())
  check("ice: cruise cap 26.4", near(U.nav_turn_cap(C.NAV_CRUISE_SPEED, C.NAV_TURN_CRUISE_SPEED), 26.4))
  C, LP, U = run(170, 180, 150)
  check("turbo: turn_speedup nil", LP.turn_speedup() == nil, LP.turn_speedup())
  check("turbo: cruise cap 72", near(U.nav_turn_cap(C.NAV_CRUISE_SPEED, C.NAV_TURN_CRUISE_SPEED), 72))

  -- Small ratio under the cap: speed 100, turn 110 -> 48 x 1.1 = 52.8.
  C, LP, U = run(100, 100, 110)
  cc = U.nav_turn_cap(C.NAV_CRUISE_SPEED, C.NAV_TURN_CRUISE_SPEED)
  check("turn110: cruise cap 52.8", near(cc, 52.8), cc)

  -- Knob off: the old math.min (48 at turn 150).
  C, LP, U = run(100, 100, 150, false)
  check("knob off: turn_speedup nil", LP.turn_speedup() == nil, LP.turn_speedup())
  check("knob off: cruise cap 48", U.nav_turn_cap(C.NAV_CRUISE_SPEED, C.NAV_TURN_CRUISE_SPEED) == 48)

  -- LIVE_PHYSICS off (KEEL): no speedup even with the knob on.
  C, LP, U = run(100, 100, 150)
  C.LIVE_PHYSICS = false
  LP.apply({ rules = classic_rules(), mods = { speed = 100, accel = 100, turn = 150 } })
  check("LIVE_PHYSICS off: turn_speedup nil", LP.turn_speedup() == nil, LP.turn_speedup())
end

-- ---------------------------------------------------------------------------
print("9. STEER_TERRAIN_TURN: turn entries follow the terrain under the tank")
do
  local tile = 7                       -- T_GRASS
  local reads = 0
  _G.TERRAIN_MASK = 0x0F
  _G.get_terrain = function(mx, my) reads = reads + 1; return tile end
  local function run(rules, mods, tt, inboat, knob)
    local C, LP = fresh()
    if knob ~= nil then C.STEER_TERRAIN_TURN = knob end
    tile = tt
    reads = 0
    LP.apply({ rules = rules, mods = mods or classic_mods(), reload_ticks = 13,
               tankx = 100 * 256 + 128, tanky = 100 * 256 + 128, inboat = inboat })
    return C, LP
  end
  local near = function(a, b) return math.abs(a - b) < 1e-9 end

  -- Classic rules in swamp: no tile read, nothing changes.
  local C, LP = run(classic_rules(), nil, 2)
  check("classic swamp: no tile read", reads == 0, reads)
  check("classic swamp: NAV_TURN_CRUISE_SPEED 48", C.NAV_TURN_CRUISE_SPEED == 48, C.NAV_TURN_CRUISE_SPEED)
  check("classic swamp: SWERVE_TURN_TICKS unchanged", C.SWERVE_TURN_TICKS == OLD.SWERVE_TURN_TICKS, C.SWERVE_TURN_TICKS)

  -- Ice Rink in swamp, terrain rules classic: road ratio (26.4), no tile read.
  local m = classic_mods(); m.speed, m.accel, m.turn = 140, 35, 55
  C, LP = run(classic_rules(), m, 2)
  check("ice swamp: no tile read", reads == 0, reads)
  check("ice swamp: NAV_TURN_CRUISE_SPEED 26.4", near(C.NAV_TURN_CRUISE_SPEED, 26.4), C.NAV_TURN_CRUISE_SPEED)

  -- Forest turn rule 1.0 (classic 0.5): in forest the caps double.
  local r = classic_rules(); r.turn_forest = 1.0
  C, LP = run(r, nil, 5)
  check("forest x2: one tile read", reads == 1, reads)
  check("forest x2: NAV_TURN_CRUISE_SPEED 96", near(C.NAV_TURN_CRUISE_SPEED, 96), C.NAV_TURN_CRUISE_SPEED)
  check("forest x2: NAV_TURN_TOP_SPEED 128", near(C.NAV_TURN_TOP_SPEED, 128), C.NAV_TURN_TOP_SPEED)
  check("forest x2: ORBIT_FAST_SPEED 32", C.ORBIT_FAST_SPEED == 32, C.ORBIT_FAST_SPEED)
  check("forest x2: turn_speedup 2", near(LP.turn_speedup(), 2), LP.turn_speedup())
  -- ... but on grass (classic 1.0) nothing changes.
  C, LP = run(r, nil, 7)
  check("forest x2, on grass: NAV_TURN_CRUISE_SPEED 48", C.NAV_TURN_CRUISE_SPEED == 48, C.NAV_TURN_CRUISE_SPEED)
  check("forest x2, on grass: turn_speedup nil", LP.turn_speedup() == nil, LP.turn_speedup())
  -- Knob off: road rule, even in forest.
  C, LP = run(r, nil, 5, nil, false)
  check("knob off, forest: no tile read", reads == 0, reads)
  check("knob off, forest: NAV_TURN_CRUISE_SPEED 48", C.NAV_TURN_CRUISE_SPEED == 48, C.NAV_TURN_CRUISE_SPEED)

  -- Swamp halved (0.125 of classic 0.25): swamp caps halve, swerve ticks double.
  r = classic_rules(); r.turn_swamp = 0.125
  C, LP = run(r, nil, 2)
  check("swamp /2: NAV_TURN_CRUISE_SPEED 24", near(C.NAV_TURN_CRUISE_SPEED, 24), C.NAV_TURN_CRUISE_SPEED)
  check("swamp /2: SWERVE_TURN_TICKS x2", C.SWERVE_TURN_TICKS == OLD.SWERVE_TURN_TICKS * 2, C.SWERVE_TURN_TICKS)

  -- In a boat on river the boat rule counts (boat 0.5 of classic 1).
  r = classic_rules(); r.turn_boat = 0.5
  C, LP = run(r, nil, 1, true)
  check("boat /2 on river: NAV_TURN_CRUISE_SPEED 24", near(C.NAV_TURN_CRUISE_SPEED, 24), C.NAV_TURN_CRUISE_SPEED)
  C, LP = run(r, nil, 1, false)
  check("boat /2, river on foot: NAV_TURN_CRUISE_SPEED 48", C.NAV_TURN_CRUISE_SPEED == 48, C.NAV_TURN_CRUISE_SPEED)

  -- Road rule changed, tank on grass: grass keeps its own classic rate.
  r = classic_rules(); r.turn_road = 2
  C, LP = run(r, nil, 7)
  check("road x2, on grass: NAV_TURN_CRUISE_SPEED 48", C.NAV_TURN_CRUISE_SPEED == 48, C.NAV_TURN_CRUISE_SPEED)
  C, LP = run(r, nil, 4)
  check("road x2, on road: NAV_TURN_CRUISE_SPEED 96", near(C.NAV_TURN_CRUISE_SPEED, 96), C.NAV_TURN_CRUISE_SPEED)

  _G.get_terrain = nil
  _G.TERRAIN_MASK = nil
end

-- ---------------------------------------------------------------------------
print("10. ENEMY_SPEED_OWN_MODS: enemy speed caps take our speed modifier")
do
  local function run(speed, knob, live)
    local C, LP = fresh()
    if knob ~= nil then C.ENEMY_SPEED_OWN_MODS = knob end
    if live ~= nil then C.LIVE_PHYSICS = live end
    local m = classic_mods()
    m.speed = speed
    LP.apply({ rules = classic_rules(), mods = m, reload_ticks = 13 })
    return C, LP, fresh_util()
  end
  local C, LP, U = run(100)
  check("classic: enemy_speed_scale nil", U.enemy_speed_scale() == nil, U.enemy_speed_scale())
  check("classic: MAP_SPEED table untouched", C.MAP_SPEED[4] == 16, C.MAP_SPEED[4])
  C, LP, U = run(140)
  check("ice: enemy_speed_scale 1.4", math.abs(U.enemy_speed_scale() - 1.4) < 1e-12, U.enemy_speed_scale())
  check("ice: MAP_SPEED road still 16 (rules only)", C.MAP_SPEED[4] == 16, C.MAP_SPEED[4])
  C, LP, U = run(65)
  check("jugg: enemy_speed_scale 0.65", math.abs(U.enemy_speed_scale() - 0.65) < 1e-12, U.enemy_speed_scale())
  C, LP, U = run(170, false)
  check("knob off: enemy_speed_scale nil", U.enemy_speed_scale() == nil, U.enemy_speed_scale())
  C, LP, U = run(170, nil, false)
  check("LIVE_PHYSICS off: enemy_speed_scale nil", U.enemy_speed_scale() == nil, U.enemy_speed_scale())
end

-- ---------------------------------------------------------------------------
print("11. man walk, world-sim build dwell, shell life, pill range in WU")
do
  local GRASS_TICKS = { "REPAIR_DEAD_GRASS_TICKS_PER_TILE",
    "BUILDER_POOL_GRASS_TICKS_PER_TILE", "WSIM_DWELL_TICKS_PER_TILE" }
  local function scaled_rules()
    local r = classic_rules()
    r.man_speed_grass = 12
    r.man_speed_refuel_base = 24
    r.lgm_build_ticks = 30
    r.shell_life = 16
    r.pill_range = 3072
    return r
  end
  local function man_push(t)
    local out = {}
    for _, c in ipairs(calls.cpf_man) do if c[1] == t then out[#out + 1] = c[2] end end
    return out
  end
  -- All on.
  local C, LP = fresh()
  LP.apply({ rules = scaled_rules(), mods = classic_mods() })
  for _, k in ipairs(GRASS_TICKS) do
    check("grass 12: " .. k .. " 16 -> 21", C[k] == 21, C[k])
  end
  check("blessed: MAN_SPEED_BLESSED = man_speed_refuel_base 24", C.MAN_SPEED_BLESSED == 24, C.MAN_SPEED_BLESSED)
  check("build 30: WSIM_DWELL_BUILD_TICKS 30", C.WSIM_DWELL_BUILD_TICKS == 30, C.WSIM_DWELL_BUILD_TICKS)
  check("build 30: LGM_BUILD_TIME 30", C.LGM_BUILD_TIME == 30, C.LGM_BUILD_TIME)
  check("shell_life 16: SHELL_MAX_STEPS 128", C.SHELL_MAX_STEPS == 128, C.SHELL_MAX_STEPS)
  check("shell_life 16: LGM_SHELL_PREDICT_TICKS 126", C.LGM_SHELL_PREDICT_TICKS == 126, C.LGM_SHELL_PREDICT_TICKS)
  check("pill_range 3072: PILLBOX_RANGE_WU 3072", C.PILLBOX_RANGE_WU == 3072, C.PILLBOX_RANGE_WU)
  check("man push: grass 12", #man_push(C.T_GRASS) == 1 and man_push(C.T_GRASS)[1] == 12, #calls.cpf_man)
  check("man push: refbase tile 24", #man_push(C.T_REFBASE) == 1 and man_push(C.T_REFBASE)[1] == 24, #calls.cpf_man)
  check("man push: blessed 24", #man_push(-1) == 1 and man_push(-1)[1] == 24, #calls.cpf_man)
  check("man push: only the 3 changed values", #calls.cpf_man == 3, #calls.cpf_man)
  reset_calls()
  LP.apply({ rules = scaled_rules(), mods = classic_mods() })
  check("repeat: no man push", #calls.cpf_man == 0, #calls.cpf_man)
  -- Back to classic: restores the old numbers and C's classic man speeds.
  reset_calls()
  LP.apply({ rules = classic_rules(), mods = classic_mods() })
  for k, v in pairs({ MAN_SPEED_BLESSED = 16, WSIM_DWELL_BUILD_TICKS = 20, SHELL_MAX_STEPS = 64,
                      LGM_SHELL_PREDICT_TICKS = 63, PILLBOX_RANGE_WU = 2048,
                      REPAIR_DEAD_GRASS_TICKS_PER_TILE = 16 }) do
    check("back to classic: " .. k .. " " .. v, C[k] == v, C[k])
  end
  check("back to classic: grass man 16 pushed", man_push(C.T_GRASS)[1] == 16, #calls.cpf_man)
  check("back to classic: blessed 16 pushed", man_push(-1)[1] == 16, #calls.cpf_man)
  check("back to classic: 3 man pushes", #calls.cpf_man == 3, #calls.cpf_man)
  -- Grass man speed 0: the man cannot walk grass; ticks-per-tile stays.
  C, LP = fresh()
  local r = classic_rules()
  r.man_speed_grass = 0
  LP.apply({ rules = r, mods = classic_mods() })
  check("grass 0: WSIM_DWELL_TICKS_PER_TILE stays 16", C.WSIM_DWELL_TICKS_PER_TILE == 16, C.WSIM_DWELL_TICKS_PER_TILE)
  check("grass 0: C gets grass 0", man_push(C.T_GRASS)[1] == 0, #calls.cpf_man)
  -- Each knob off keeps its old numbers.
  local function off(knob)
    local C2, LP2 = fresh()
    C2[knob] = false
    LP2.apply({ rules = scaled_rules(), mods = classic_mods() })
    return C2
  end
  C = off("LIVE_PHYSICS_LGM_WALK")
  check("LGM_WALK off: MAN_SPEED_BLESSED 16", C.MAN_SPEED_BLESSED == 16, C.MAN_SPEED_BLESSED)
  check("LGM_WALK off: WSIM_DWELL_TICKS_PER_TILE 16", C.WSIM_DWELL_TICKS_PER_TILE == 16, C.WSIM_DWELL_TICKS_PER_TILE)
  check("LGM_WALK off: no man push", #calls.cpf_man == 0, #calls.cpf_man)
  check("LGM_WALK off: LGM_BUILD_TIME still live 30", C.LGM_BUILD_TIME == 30, C.LGM_BUILD_TIME)
  C = off("LIVE_PHYSICS_WSIM_BUILD")
  check("WSIM_BUILD off: WSIM_DWELL_BUILD_TICKS 20", C.WSIM_DWELL_BUILD_TICKS == 20, C.WSIM_DWELL_BUILD_TICKS)
  C = off("LIVE_PHYSICS_SHELL_LIFE")
  check("SHELL_LIFE off: SHELL_MAX_STEPS 64", C.SHELL_MAX_STEPS == 64, C.SHELL_MAX_STEPS)
  check("SHELL_LIFE off: LGM_SHELL_PREDICT_TICKS 63", C.LGM_SHELL_PREDICT_TICKS == 63, C.LGM_SHELL_PREDICT_TICKS)
  C = off("LIVE_PHYSICS_PILL_RANGE_WU")
  check("PILL_RANGE_WU off: PILLBOX_RANGE_WU 2048", C.PILLBOX_RANGE_WU == 2048, C.PILLBOX_RANGE_WU)
  C = off("LIVE_PHYSICS_LGM")
  check("LGM group off: no man push", #calls.cpf_man == 0, #calls.cpf_man)
  check("LGM group off: WSIM_DWELL_BUILD_TICKS 20", C.WSIM_DWELL_BUILD_TICKS == 20, C.WSIM_DWELL_BUILD_TICKS)
  -- PRESETS.keel turns every new knob off.
  C = fresh()
  for _, k in ipairs({ "LIVE_PHYSICS_LGM_WALK", "LIVE_PHYSICS_WSIM_BUILD",
                       "LIVE_PHYSICS_SHELL_LIFE", "LIVE_PHYSICS_PILL_RANGE_WU" }) do
    check("PRESETS.keel." .. k .. " == false", C.PRESETS.keel[k] == false, tostring(C.PRESETS.keel[k]))
    check("default " .. k .. " == true", C[k] == true, tostring(C[k]))
  end
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
