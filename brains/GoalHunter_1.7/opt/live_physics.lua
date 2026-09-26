-- =========================================================================
-- GoalHunter/live_physics.lua -- read the live game rules instead of the
-- classic numbers the constants were tuned on.
-- =========================================================================
--
-- Many constants in constants.lua encode a classic physics number: full
-- armour 40, 5 armour per shell hit, reload 13 ticks, pill armour 15, road
-- speed 16, and so on. A scenario can change any of these (rules) and a
-- tank's own modifiers (speed/accel/turn/reload/dealt/taken percent). The
-- engine now pushes both into info.rules and info.mods.
--
-- apply(info) runs once per think, before anything else reads C. For every
-- entry in ENTRIES it rewrites C[KEY] from the value the constant had before
-- this module touched it (its "base"), scaled by the live rule. Every scale
-- function returns the base value itself when the rules it reads are
-- classic, so under classic rules no constant changes and nothing is pushed
-- to C: behaviour is bit-for-bit the old brain.
--
-- A value someone else writes into C (a cfg= token, a difficulty bundle, a
-- test) becomes the new base the next time apply runs.
--
-- Knobs (constants.lua): LIVE_PHYSICS is the master switch (PRESETS.keel =
-- false). Each group below has its own LIVE_PHYSICS_<GROUP> switch.
-- With a switch off, the group's constants go back to their base values.
--
-- Missing or zero rules fall back to the classic value, so a host that does
-- not send info.rules gets the classic brain.
-- =========================================================================

local C = require("constants")

local M = {}

local floor = math.floor
local ceil  = math.ceil

-- The engine's classic rules (src/bolo/sim_rules.c simRulesClassic).
local CLASSIC = {
  tank_full_armour = 40, tank_full_shells = 40, tank_reload_ticks = 13,
  shell_damage = 5, shell_speed = 32, gunsight_max = 14,
  tree_hide_distance = 768,
  speed_road = 16, speed_grass = 12, speed_forest = 6, speed_river = 3,
  speed_swamp = 3, speed_crater = 3, speed_rubble = 3, speed_boat = 16,
  speed_deep_sea = 3, speed_refuel_base = 16,
  turn_road = 1, turn_swamp = 0.25,
  pill_max_armour = 15, pill_attack_ticks = 100, pill_attack_min_ticks = 6,
  pill_cooldown_ticks = 32, pill_repair_amount = 4, pill_range = 2048,
  pill_shell_damage = 1,
  base_full_armour = 90, base_capture_armour = 9, base_regen_ticks = 1000,
  lgm_build_ticks = 20, lgm_cost_road = 2, lgm_cost_building = 2,
  lgm_cost_boat = 20, lgm_cost_pill_new = 4, lgm_cost_mine = 1,
  lgm_gather_trees = 4, lgm_helicopter_speed = 3,
  man_speed_road = 16, man_speed_grass = 16, man_speed_forest = 8,
  man_speed_river = 0, man_speed_swamp = 4, man_speed_crater = 4,
  man_speed_rubble = 4, man_speed_boat = 16, man_speed_deep_sea = 0,
  man_speed_refuel_base = 16,
}
M.CLASSIC = CLASSIC

-- Rules where 0 is a legal live value (a free job, a man who cannot walk
-- a terrain). For every other rule 0 means "not sent" and reads as classic.
local ZERO_OK = {
  lgm_cost_road = true, lgm_cost_building = true, lgm_cost_boat = true,
  lgm_cost_pill_new = true, lgm_cost_mine = true,
  man_speed_road = true, man_speed_grass = true, man_speed_forest = true,
  man_speed_river = true, man_speed_swamp = true, man_speed_crater = true,
  man_speed_rubble = true, man_speed_boat = true, man_speed_deep_sea = true,
  man_speed_refuel_base = true,
  speed_road = true, speed_grass = true, speed_forest = true,
  speed_river = true, speed_swamp = true, speed_crater = true,
  speed_rubble = true, speed_boat = true, speed_deep_sea = true,
  speed_refuel_base = true,
}

-- Terrain type -> the speed / man-speed rule that governs it.
local T = C
local SPEED_RULE = {
  [T.T_ROAD] = "speed_road", [T.T_GRASS] = "speed_grass",
  [T.T_FOREST] = "speed_forest", [T.T_RIVER] = "speed_river",
  [T.T_SWAMP] = "speed_swamp", [T.T_CRATER] = "speed_crater",
  [T.T_RUBBLE] = "speed_rubble", [T.T_BOAT] = "speed_boat",
  [T.T_DEEPSEA] = "speed_deep_sea", [T.T_REFBASE] = "speed_refuel_base",
  [T.T_PILLBOX] = "speed_road",   -- brain tables treat a dead pill as road-fast
  [T.T_UNKNOWN] = "speed_grass",  -- unseen tiles assume grass
}
local MAN_RULE = {
  [T.T_ROAD] = "man_speed_road", [T.T_GRASS] = "man_speed_grass",
  [T.T_FOREST] = "man_speed_forest", [T.T_RIVER] = "man_speed_river",
  [T.T_SWAMP] = "man_speed_swamp", [T.T_CRATER] = "man_speed_crater",
  [T.T_RUBBLE] = "man_speed_rubble", [T.T_BOAT] = "man_speed_boat",
  [T.T_DEEPSEA] = "man_speed_deep_sea",
  [T.T_REFBASE] = "man_speed_refuel_base",
}
-- Fixed iteration order for every per-terrain loop (never pairs()).
local TERRAINS = {}
for tt = 0, 13 do TERRAINS[#TERRAINS + 1] = tt end

-- -------------------------------------------------------------------------
-- The live context: every number the scale functions read.
-- -------------------------------------------------------------------------

local function rule_of(r, name)
  local v = r and r[name]
  if type(v) ~= "number" or v ~= v then return CLASSIC[name] end
  if v < 0 or (v == 0 and not ZERO_OK[name]) then return CLASSIC[name] end
  return v
end

local function pct_of(m, name)
  local v = m and m[name]
  if type(v) ~= "number" or v <= 0 then return 100 end
  return v
end

-- The engine's rounding: tankDamageAmount = (base*dealt*taken*scale + 5e5)
-- / 1e6 with integer division. Against an unknown other side (their dealt
-- or taken) the classic 100 is assumed.
local function engine_hit(dmg, pct)
  return floor((dmg * pct + 50) / 100)
end

--- Build the context from one BrainInfo. Pure: no C writes. `out` is
--- reused when given, so a think allocates nothing here.
function M.context(info, out)
  local r = info and info.rules
  local m = info and info.mods
  local x = out or {}
  for k in pairs(CLASSIC) do x[k] = rule_of(r, k) end
  x.speed_pct  = pct_of(m, "speed")
  x.turn_pct   = pct_of(m, "turn")
  x.reload_pct = pct_of(m, "reload")
  x.dealt_pct  = pct_of(m, "dealt")
  x.taken_pct  = pct_of(m, "taken")
  -- Armour one enemy or pill shell takes off us, and one of our shells
  -- takes off a classic tank. Never below 1 for scaling.
  x.hit_taken = math.max(1, engine_hit(x.shell_damage, x.taken_pct))
  x.hit_dealt = math.max(1, engine_hit(x.shell_damage, x.dealt_pct))
  -- Our reload in engine ticks: the engine's own figure when sent.
  local rt = info and info.reload_ticks
  if type(rt) == "number" and rt > 0 then
    x.reload = rt
  else
    x.reload = floor((x.tank_reload_ticks * x.reload_pct + 50) / 100)
    if x.reload < 1 then x.reload = 1 end
  end
  -- Shells to kill a full-armour tank / a full pill.
  x.tank_kill_shells = ceil(x.tank_full_armour / x.hit_dealt)
  x.pill_kill_shells = ceil(x.pill_max_armour / x.pill_shell_damage)
  return x
end

-- -------------------------------------------------------------------------
-- Scale helpers. Each returns `b` unchanged when num == den (classic), so
-- the constant keeps its exact value and type.
-- -------------------------------------------------------------------------

local function rnd(v) return floor(v + 0.5) end

local function scale_int(b, num, den)
  if num == den or type(b) ~= "number" then return b end
  return rnd(b * num / den)
end

local function scale_f(b, num, den)
  if num == den or type(b) ~= "number" then return b end
  return b * num / den
end

-- Armour in HITS: b armour = b/5 classic hits. Rescaled to the live armour
-- per hit taken. Capped one hit below full so a "low" line can never sit at
-- or above full armour (the bot would never leave the base). A value at or
-- above classic full (40) means "never"; it scales with full instead.
local function a_hits(b, x)
  if type(b) ~= "number" then return b end
  if x.hit_taken == 5 and x.tank_full_armour == 40 then return b end
  if b <= 0 then return b end
  if b >= 40 then return scale_int(b, x.tank_full_armour, 40) end
  local v = rnd(b * x.hit_taken / 5)
  local cap = x.tank_full_armour - x.hit_taken
  if cap < 1 then cap = 1 end
  if v > cap then v = cap end
  return v
end

-- Armour as a FRACTION of full armour (readiness lines).
local function a_full(b, x)
  if x.tank_full_armour == 40 then return b end
  if type(b) ~= "number" then return b end
  local v = scale_int(b, x.tank_full_armour, 40)
  if b <= 40 and v > x.tank_full_armour then v = x.tank_full_armour end
  return v
end

-- Shells as a fraction of full shells. `low` lines cap one below full.
local function s_cap(v, b, x, low)
  if b > 40 then return v end
  local cap = low and (x.tank_full_shells - 1) or x.tank_full_shells
  if cap < 0 then cap = 0 end
  if v > cap then v = cap end
  return v
end
local function s_full(b, x, low)
  if x.tank_full_shells == 40 then return b end
  if type(b) ~= "number" then return b end
  return s_cap(scale_int(b, x.tank_full_shells, 40), b, x, low)
end
-- Shells sized in tank kills (8 classic shells kill a full tank).
local function s_tank(b, x, low)
  if x.tank_kill_shells == 8 and x.tank_full_shells == 40 then return b end
  if type(b) ~= "number" then return b end
  return s_cap(scale_int(b, x.tank_kill_shells, 8), b, x, low)
end
-- Shells sized in pill kills (15 classic shells kill a full pill).
local function s_pill(b, x, low)
  if x.pill_kill_shells == 15 and x.tank_full_shells == 40 then return b end
  if type(b) ~= "number" then return b end
  return s_cap(scale_int(b, x.pill_kill_shells, 15), b, x, low)
end

-- Tank top-speed factor on a terrain: live rule x own speed %.
local function speed_factor(x, rule)
  return x[rule], CLASSIC[rule], x.speed_pct
end
local function scale_speed(b, x, rule, with_pct)
  local live, classic, pct = speed_factor(x, rule)
  if not with_pct then pct = 100 end
  if live == classic and pct == 100 then return b end
  if type(b) ~= "number" then return b end
  return b * live * pct / (classic * 100)
end

-- Turn-time factor: ticks scale with 1 / (turn rule x turn %).
local function scale_turn(b, x, rule)
  local live, classic = x[rule], CLASSIC[rule]
  if live == classic and x.turn_pct == 100 then return b end
  if type(b) ~= "number" then return b end
  return rnd(b * classic * 100 / (live * x.turn_pct))
end

-- -------------------------------------------------------------------------
-- Per-terrain tables. A new table is built only when the rules differ from
-- classic; otherwise the original table object comes back.
-- -------------------------------------------------------------------------

local function tbl_classic_speed(x, with_pct)
  if with_pct and x.speed_pct ~= 100 then return false end
  for _, rule in pairs(SPEED_RULE) do
    if x[rule] ~= CLASSIC[rule] then return false end
  end
  return true
end

local function scale_speed_table(b, x, with_pct)
  if type(b) ~= "table" or tbl_classic_speed(x, with_pct) then return b end
  local out = {}
  for _, tt in ipairs(TERRAINS) do
    local v = b[tt]
    if v ~= nil then
      local rule = SPEED_RULE[tt]
      out[tt] = rule and scale_speed(v, x, rule, with_pct) or v
    end
  end
  return out
end

local function man_table(b, x)
  if type(b) ~= "table" then return b end
  local same = true
  for _, rule in pairs(MAN_RULE) do
    if x[rule] ~= CLASSIC[rule] then same = false end
  end
  if same then return b end
  local out = {}
  for _, tt in ipairs(TERRAINS) do
    local v = b[tt]
    if v ~= nil then
      local rule = MAN_RULE[tt]
      -- Same units as the engine: the live rule replaces the value.
      out[tt] = rule and x[rule] or v
    end
  end
  return out
end

-- Route cost ~ 1/speed. Keep the tuned bias: cost x (classic/live speed),
-- normalised so road stays at its tuned cost. Impassable (9999) and
-- dynamic (< 0) entries stay as they are; a terrain whose live speed is 0
-- becomes impassable.
local function cost_entry(v, x, rule)
  if type(v) ~= "number" or v <= 0 or v >= 9999 then return v end
  local live, classic = x[rule], CLASSIC[rule]
  local rl, rc = x.speed_road, CLASSIC.speed_road
  if live == classic and rl == rc then return v end
  if live <= 0 then return 9999 end
  if rl <= 0 then rl = rc end
  return v * (classic / live) * (rl / rc)
end

local function scale_cost_table(b, x)
  if type(b) ~= "table" or tbl_classic_speed(x, false) then return b end
  local out = {}
  for _, tt in ipairs(TERRAINS) do
    local v = b[tt]
    if v ~= nil then
      local rule = SPEED_RULE[tt]
      out[tt] = rule and cost_entry(v, x, rule) or v
    end
  end
  return out
end

local function road_cost_table(b, x)
  if type(b) ~= "table" or x.lgm_cost_road == CLASSIC.lgm_cost_road then return b end
  local out = {}
  for _, tt in ipairs(TERRAINS) do
    if b[tt] ~= nil then out[tt] = x.lgm_cost_road end
  end
  return out
end

-- -------------------------------------------------------------------------
-- ENTRIES: { KEY, GROUP, fn(base, x) }. Order is fixed.
-- -------------------------------------------------------------------------

local function direct(rule)
  return function(b, x)
    if x[rule] == CLASSIC[rule] then return b end
    return x[rule]
  end
end
local function ratio_int(rule)
  return function(b, x) return scale_int(b, x[rule], CLASSIC[rule]) end
end
local function ratio_f(rule)
  return function(b, x) return scale_f(b, x[rule], CLASSIC[rule]) end
end

local ENTRIES = {
  -- Full stocks (refuel targets, "base useless" tests, vulnerability).
  { "TANK_FULL_ARMOUR", "ARMOUR", direct("tank_full_armour") },
  { "TANK_FULL_SHELLS", "SHELLS", direct("tank_full_shells") },
  { "VULN_ARMOUR_CAP",  "ARMOUR", direct("tank_full_armour") },

  -- Danger lines: armour counted in hits taken.
  { "ARMOUR_CRITICAL",              "ARMOUR", a_hits },
  { "ARMOUR_LOW",                   "ARMOUR", a_hits },
  { "ARMOUR_MODERATE",              "ARMOUR", a_hits },
  { "ATTACK_PILL_UNSAFE_HP_THRESHOLD", "PILL",
    function(b, x) return scale_int(b, x.pill_max_armour, 15) end },
  { "TANK_COMBAT_FLEE_ARMOUR",      "ARMOUR", a_hits },
  { "ATTACK_RUSH_MIN_ARMOUR",       "ARMOUR", a_hits },
  { "IMMINENT_CAPTURE_MIN_ARMOUR",  "ARMOUR", a_hits },
  { "TANK_COMBAT_MIN_ARMOUR",       "ARMOUR", a_hits },
  { "BASE_STEAL_MIN_ARMOUR",        "ARMOUR", a_hits },
  { "DEFEND_READY_ARMOUR",          "ARMOUR", a_hits },

  -- Readiness lines: armour as a fraction of full.
  { "ARMOUR_COMBAT",                   "ARMOUR", a_full },
  { "ATTACK_PILL_RISKY_ARMOUR",        "ARMOUR", a_full },
  { "ATTACK_PILL_UNSAFE_ARMOUR_FLOOR", "ARMOUR", a_full },
  { "ANGER_WAIT_HIGH_ARMOUR_ARM",      "ARMOUR", a_full },
  { "SQUAD_COMMANDER_MIN_ARMOUR",      "ARMOUR", a_full },
  { "SQUAD_REFUEL_OK_ARMOUR",          "ARMOUR", a_full },

  -- Armour lost per pill HP we shoot off (one shell per pill_shell_damage).
  { "ARMOUR_PER_PILL_HP", "ARMOUR",
    function(b, x) return scale_f(b, x.hit_taken, 5 * x.pill_shell_damage) end },
  { "SWERVE_SKIP_ARMOUR_PER_HP", "ARMOUR",
    function(b, x) return scale_f(b, x.hit_taken, 5 * x.pill_shell_damage) end },
  -- Ticks under pill fire before fleeing: survivable hits x pill fire rate.
  { "ENGAGE_MAX_INCOMING_TICKS", "ARMOUR",
    function(b, x)
      local num = x.tank_full_armour * 5 * x.pill_attack_min_ticks
      local den = 40 * x.hit_taken * 6
      return scale_int(b, num, den)
    end },

  -- Shells.
  { "SHELLS_LOW",             "SHELLS", function(b, x) return s_pill(b, x, true) end },
  { "SHELLS_COMBAT",          "SHELLS", function(b, x) return s_full(b, x, false) end },
  { "REFUEL_BASELINE_SHELLS", "SHELLS", function(b, x) return s_full(b, x, false) end },
  { "DEFEND_READY_SHELLS",    "SHELLS", function(b, x) return s_full(b, x, false) end },
  { "AMMO_DEPRIVED_SHELLS",   "SHELLS", function(b, x) return s_full(b, x, true) end },
  { "TANK_COMBAT_MIN_SHELLS", "SHELLS", function(b, x) return s_tank(b, x, true) end },
  { "TANK_COMBAT_FLEE_SHELLS","SHELLS", function(b, x) return s_tank(b, x, true) end },
  { "REFUEL_PER_ENEMY_TANK",  "SHELLS", function(b, x) return s_tank(b, x, false) end },
  { "TANK_SHELL_DAMAGE",      "SHELLS",
    function(b, x) if x.hit_dealt == 5 then return b end return x.hit_dealt end },

  -- Reload.
  { "TTK_TICKS_PER_HIT", "RELOAD",
    function(b, x) return scale_f(b, x.reload, 13 * x.pill_shell_damage) end },
  { "FIRE_HOLD_TICKS", "RELOAD",
    function(b, x) return scale_int(b, x.reload, 13) end },

  -- Pills.
  { "PILLS_MAX_HEALTH",     "PILL", direct("pill_max_armour") },
  { "PILL_REPAIR_AMOUNT",   "PILL", direct("pill_repair_amount") },
  { "PILL_CHAIN_MIN_HP",    "PILL", function(b, x) return scale_int(b, x.pill_max_armour, 15) end },
  { "PPT_HEALTH_THRESHOLD", "PILL", function(b, x) return scale_int(b, x.pill_max_armour, 15) end },
  { "TANK_FINISH_MAX_HP",   "PILL", function(b, x) return scale_int(b, x.pill_max_armour, 15) end },
  { "HEAT_PILL_MIN_HP",     "PILL", function(b, x) return scale_int(b, x.pill_max_armour, 15) end },
  { "HARD_TAKE_MIN_HP",     "PILL", function(b, x) return scale_int(b, x.pill_max_armour, 15) end },
  { "PILL_FIRE_RANGE",      "PILL", ratio_f("pill_range") },
  { "ATTACK_PILL_RANGE",    "PILL", ratio_f("pill_range") },
  { "HARDLINE_ENGAGE_RANGE","PILL", ratio_f("pill_range") },
  -- Anger decay ~ (attack - min) x cooldown ticks (94 x 32 classic).
  { "PILL_ANGER_DECAY", "PILL",
    function(b, x)
      return scale_f(b, (x.pill_attack_ticks - x.pill_attack_min_ticks) * x.pill_cooldown_ticks,
                     (100 - 6) * 32)
    end },

  -- Bases.
  { "BASE_FULL_ARMOUR",    "BASE", direct("base_full_armour") },
  { "BASE_CAPTURE_ARMOUR", "BASE", direct("base_capture_armour") },
  { "BASE_SHELL_DAMAGE",   "BASE", direct("shell_damage") },
  { "BASE_FULL_SHELLS_TO_KILL", "BASE",
    function(b, x)
      if x.base_full_armour == 90 and x.base_capture_armour == 9 and x.shell_damage == 5 then
        return b
      end
      return math.max(1, ceil((x.base_full_armour - x.base_capture_armour) / x.shell_damage))
    end },
  -- 24 = capture 9 + three shells of 5.
  { "BASE_STEAL_MAX_ARMOUR", "BASE",
    function(b, x)
      if x.base_capture_armour == 9 and x.shell_damage == 5 then return b end
      return x.base_capture_armour + rnd((b - 9) * x.shell_damage / 5)
    end },
  { "BASE_MARKUP_STALE",    "BASE", ratio_int("base_regen_ticks") },
  { "BASE_STEAL_OBS_STALE", "BASE", ratio_int("base_regen_ticks") },

  -- Builder.
  { "LGM_BUILD_TIME",         "LGM", ratio_int("lgm_build_ticks") },
  { "PILL_PLACE_TREE_COST",   "LGM", direct("lgm_cost_pill_new") },
  { "REPAIR_DEAD_MIN_TREES",  "LGM", ratio_int("lgm_cost_pill_new") },
  { "ROAD_RIVER_COST",        "LGM", direct("lgm_cost_road") },
  { "ROAD_BUILD_TERRAIN",     "LGM", road_cost_table },
  { "BASE_SHIELD_BUILD_COST", "LGM", direct("lgm_cost_building") },
  { "WALL_SHIELD_BUILD_COST", "LGM", direct("lgm_cost_building") },
  { "SEA_BOAT_TREES",         "LGM", direct("lgm_cost_boat") },
  { "SEA_MINE_TREES",         "LGM", direct("lgm_cost_mine") },
  { "SEA_PILL_TREES_TOTAL",   "LGM",
    function(b, x)
      if x.lgm_cost_boat == 20 and x.lgm_cost_mine == 1 then return b end
      return x.lgm_cost_boat + x.lgm_cost_mine
    end },
  { "SEA_TREES_PER_FOREST",   "LGM", direct("lgm_gather_trees") },
  { "MAN_SPEED",              "LGM", man_table },
  -- A dead builder flies back at helicopter speed (3 classic).
  { "ENEMY_LGM_RETURN_TICKS", "LGM",
    function(b, x) return scale_int(b, CLASSIC.lgm_helicopter_speed, x.lgm_helicopter_speed) end },

  -- Tank speed. TERRAIN_SPEED is our own top speed (rules x own speed %).
  -- MAP_SPEED models ENEMY tanks, whose modifiers we cannot see: rules only.
  { "TERRAIN_SPEED",     "SPEED", function(b, x) return scale_speed_table(b, x, true) end },
  { "MAP_SPEED",         "SPEED", function(b, x) return scale_speed_table(b, x, false) end },
  { "TERRAIN_COST_LAND", "SPEED", scale_cost_table },
  { "NAV_TOP_SPEED",     "SPEED", function(b, x) return scale_speed(b, x, "speed_road", true) end },
  { "NAV_CRUISE_SPEED",  "SPEED", function(b, x) return scale_speed(b, x, "speed_road", true) end },

  -- Turn times.
  { "SWERVE_TURN_TICKS",           "TURN", function(b, x) return scale_turn(b, x, "turn_road") end },
  { "SWERVE_DEFENSIVE_TURN_TICKS", "TURN", function(b, x) return scale_turn(b, x, "turn_road") end },
  { "SQUAD_BLITZ_READY_TIMEOUT",   "TURN", function(b, x) return scale_turn(b, x, "turn_swamp") end },

  -- Shell flight and gunsight.
  { "SHELL_SPEED",             "SHELL", ratio_f("shell_speed") },
  { "TANK_COMBAT_SHELL_SPEED", "SHELL", ratio_f("shell_speed") },
  { "GUNSIGHT_MAX", "SHELL",
    function(b, x)
      if x.gunsight_max == 14 or type(b) ~= "number" then return b end
      return b + (x.gunsight_max - 14)
    end },
}
M.ENTRIES = ENTRIES

-- -------------------------------------------------------------------------
-- State: base values and what this module last wrote.
-- -------------------------------------------------------------------------

local base    = {}   -- KEY -> the value before live scaling
local written = {}   -- KEY -> what apply last wrote (nil = never)

-- What the C pathfinder / world sim last received from here. nil = the
-- configure() defaults, which apply never re-sends while they stay right.
local pushed_cpf_speed = {}
local pushed_cpf_cost  = {}
local pushed_wsim_speed = {}
local pushed_wsim_rules = nil

--- Forget everything (Brain.open, after cpf/wsim.configure()).
function M.reset()
  for k in pairs(base) do base[k] = nil end
  for k in pairs(written) do written[k] = nil end
  for k in pairs(pushed_cpf_speed) do pushed_cpf_speed[k] = nil end
  for k in pairs(pushed_cpf_cost) do pushed_cpf_cost[k] = nil end
  for k in pairs(pushed_wsim_speed) do pushed_wsim_speed[k] = nil end
  pushed_wsim_rules = nil
end

local GROUP_KEY = {}
for _, g in ipairs({ "ARMOUR", "SHELLS", "RELOAD", "PILL", "BASE", "LGM",
                     "SPEED", "TURN", "SHELL" }) do
  GROUP_KEY[g] = "LIVE_PHYSICS_" .. g
end

local function group_on(group)
  return C[GROUP_KEY[group]] ~= false
end

-- Push one C table entry only when it differs from what the C side holds.
local function push_table(defaults, x, scale, pushed, setter)
  if type(defaults) ~= "table" or type(setter) ~= "function" then return end
  for _, tt in ipairs(TERRAINS) do
    local d = defaults[tt]
    if d ~= nil then
      local rule = SPEED_RULE[tt]
      local v = d
      if rule and x then v = scale(d, x, rule) end
      local have = pushed[tt]
      if have == nil then have = d end
      if v ~= have then
        setter(tt, v)
        pushed[tt] = v
      end
    end
  end
end

local function speed_with_pct(d, x, rule) return scale_speed(d, x, rule, true) end

local WSIM_CLASSIC = {
  shell_damage = 5, pill_range = 2048, pill_attack_ticks = 100,
  pill_attack_min_ticks = 6, pill_cooldown_ticks = 32, pill_hit_damage = 1,
  shoot_interval = 8, forest_range = 768,
}
local WSIM_KEYS = { "shell_damage", "pill_range", "pill_attack_ticks",
  "pill_attack_min_ticks", "pill_cooldown_ticks", "pill_hit_damage",
  "shoot_interval", "forest_range" }

local function push_c(x)
  -- x == nil restores the classic defaults (switch turned off).
  local cpf = package.loaded["cpathfinder"]
  local speed_x = (x and group_on("SPEED")) and x or nil
  if cpf and cpf.DEFAULTS then
    push_table(cpf.DEFAULTS.terrain_speed, speed_x, speed_with_pct,
               pushed_cpf_speed, rawget(_G, "cpf_set_terrain_speed"))
    push_table(cpf.DEFAULTS.terrain_cost, speed_x, cost_entry,
               pushed_cpf_cost, rawget(_G, "cpf_set_terrain_cost"))
  end
  local ws = package.loaded["cworldsim"]
  if ws and ws.DEFAULTS then
    push_table(ws.DEFAULTS.terrain_speed, speed_x, speed_with_pct,
               pushed_wsim_speed, rawget(_G, "wsim_set_terrain_speed"))
  end
  local set_rules = rawget(_G, "wsim_set_rules")
  if type(set_rules) == "function" then
    local want = WSIM_CLASSIC
    if x and group_on("PILL") then
      want = {
        shell_damage = x.hit_taken, pill_range = x.pill_range,
        pill_attack_ticks = x.pill_attack_ticks,
        pill_attack_min_ticks = x.pill_attack_min_ticks,
        pill_cooldown_ticks = x.pill_cooldown_ticks,
        pill_hit_damage = x.pill_shell_damage,
        shoot_interval = math.max(1, rnd(8 * x.reload / 13)),
        forest_range = x.tree_hide_distance,
      }
    end
    local have = pushed_wsim_rules or WSIM_CLASSIC
    local differ = false
    for _, k in ipairs(WSIM_KEYS) do
      if want[k] ~= have[k] then differ = true end
    end
    if differ then
      set_rules(want)
      pushed_wsim_rules = want
    end
  end
end

local X = {}   -- the reused context

--- Rewrite the physics constants in C from this think's info.
function M.apply(info)
  local on = C.LIVE_PHYSICS == true
  if not on and next(written) == nil then return end
  local x = on and M.context(info, X) or nil
  for i = 1, #ENTRIES do
    local e = ENTRIES[i]
    local key = e[1]
    local cur = C[key]
    if cur ~= nil then
      local w = written[key]
      -- Someone else wrote this constant since our last write: that is the
      -- new base.
      if w == nil or cur ~= w then base[key] = cur end
      local b = base[key]
      local v = b
      if x and group_on(e[2]) then v = e[3](b, x) end
      if v ~= cur then C[key] = v end
      if not on then
        written[key] = nil
      else
        written[key] = v
      end
    end
  end
  push_c(x)
end

return M
