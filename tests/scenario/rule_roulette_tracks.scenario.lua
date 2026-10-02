-- Rule Roulette's two tracks on different clocks, a builder skip, and a
-- tank rule mode and a builder rule mode in force together when the round
-- ends.
--
-- The runner writes data/mods/RuleRoulette.scenario.lua in front of this
-- text (the include on the GATE line), so the mod's locals -- TANK,
-- BUILDER, TRACKS, command, build_panel, panel_bottom, refused -- are in
-- scope. The queues are filled before the mod's on_start takes the first
-- modes:
--
--   tank, every 20 s:    Turbo, Overdrive, Hovercraft
--   builder, every 30 s: Cleanup Crew, Hustle
--
-- 25.5 s in, the arena types "!roulette next builder" through the mod's own
-- command function, so Hustle starts at 26 s, not 30, and the tank clock
-- does not move. At 50.5 s Hovercraft (tank rules) and Hustle (builder
-- rules) are both in force, and the arena ends the round.
--
-- The round opens on some host values, not the classic ones, so a mode
-- that put back the classic number rather than the one it found shows:
-- speed_road 15, man_speed_grass 12 and lgm_cost_road 3.
--
-- What is checked:
--   1. Every frame, each rule either track can touch holds the value the
--      two modes in force should give it, worked out here from the values
--      the round opened on.
--   2. The tank modes start 20 s apart and the builder modes 26 s apart
--      (the skip), each to within 2 ticks.
--   3. Every frame the panel the mod would draw ends inside the 128 units
--      and no panel update was refused.
--   4. After the mod's on_end every rule holds the value the round opened
--      on, and neither track holds rules to put back.
--
-- GATE: ticks=8000 bots=0 ai=yesfull gametype=open include=data/mods/RuleRoulette.scenario.lua

local roulette = scenario
local mod_start, mod_end, mod_spawned = on_start, on_end, on_tank_spawned
on_chat = nil   -- a human's !roulette; the tail must not wrap it

local arena_settings = {}
for i, st in ipairs(roulette.settings) do
  local c = {}
  for k, v in pairs(st) do c[k] = v end
  if c.id == "interval" then c.default = 20 end
  if c.id == "builder_interval" then c.default = 30 end
  if c.id == "preview" then c.default = 3 end
  arena_settings[i] = c
end

scenario = {
  name        = GATE_NAME,
  description = "Rule Roulette's tank and builder tracks on their own clocks, restored together.",
  game        = GATE_GAMETYPE,
  api         = 1,
  needs_bots  = true,
  rules = {
    base_regen_ticks = roulette.rules.base_regen_ticks,
    speed_road       = 15,
    man_speed_grass  = 12,
    lgm_cost_road    = 3,
  },
  settings = arena_settings,
}

local TANK_ORDER    = { "Turbo", "Overdrive", "Hovercraft" }
local BUILDER_ORDER = { "Cleanup Crew", "Hustle" }

local TANK_RULES = { "speed_road", "speed_river", "speed_swamp", "speed_crater",
  "speed_rubble", "speed_boat", "speed_deep_sea", "speed_refuel_base",
  "water_loss_shells", "water_loss_mines", "tank_deep_sea_safe" }
local MAN_SPEEDS = { "man_speed_road", "man_speed_grass", "man_speed_forest",
  "man_speed_river", "man_speed_swamp", "man_speed_crater", "man_speed_rubble",
  "man_speed_boat", "man_speed_deep_sea", "man_speed_refuel_base" }
local WATCH = { "speed_grass", "speed_forest", "lgm_cost_road", "lgm_helicopter_speed" }
for _, r in ipairs(TANK_RULES) do WATCH[#WATCH + 1] = r end
for _, r in ipairs(MAN_SPEEDS) do WATCH[#WATCH + 1] = r end

local ME       = 0
local start    = nil     -- rule name -> its value as the round opened
local t0       = nil     -- tick of on_start
local skipped  = false
local ended    = false
local bad      = nil
local starts   = { tank = {}, builder = {} }   -- tick each mode started
local counted  = { tank = 0, builder = 0 }
local both_in  = false   -- Hovercraft and Hustle checked together

function on_setup(g)
  g.spawn_bot{ slot = ME, name = "Idle", brain = "idle" }
end

local function force(tr, order)
  for _, want in ipairs(order) do
    for i, m in ipairs(tr.modes) do
      if m.name == want then tr.queue[#tr.queue + 1] = i end
    end
  end
end

function on_start(g)
  start = {}
  for _, r in ipairs(WATCH) do start[r] = g.rule(r) end
  t0 = g.tick()
  force(TANK, TANK_ORDER)
  force(BUILDER, BUILDER_ORDER)
  mod_start()
end

function on_tank_spawned(g, p, ...)
  return mod_spawned(p, ...)
end

-- pct percent, to the nearest whole number; 0 stays 0, above 0 stays 1+.
local function pct_of(v, pct)
  if v <= 0 then return v end
  return math.max(1, math.floor(v * pct / 100 + 0.5))
end

-- What each watched rule should hold under the two modes in force.
local function expected()
  local want = {}
  for _, r in ipairs(WATCH) do want[r] = start[r] end
  local tm = TANK.mode and TANK.mode.name
  local bm = BUILDER.mode and BUILDER.mode.name
  if tm == "Hovercraft" then
    for _, r in ipairs(TANK_RULES) do want[r] = start.speed_grass end
    want.water_loss_shells, want.water_loss_mines, want.tank_deep_sea_safe = 0, 0, 1
  end
  if bm == "Cleanup Crew" then
    want.lgm_cost_road = 0
  elseif bm == "Hustle" then
    for _, r in ipairs(MAN_SPEEDS) do want[r] = math.min(63, pct_of(start[r], 125)) end
  end
  return want
end

local function check(g)
  local want = expected()
  for _, r in ipairs(WATCH) do
    if g.rule(r) ~= want[r] then
      return string.format("%s/%s: %s is %s, want %s",
                           TANK.mode and TANK.mode.name or "-",
                           BUILDER.mode and BUILDER.mode.name or "-",
                           r, tostring(g.rule(r)), tostring(want[r]))
    end
  end
  local bottom = panel_bottom(build_panel())
  if bottom > 128 then return "panel ends at " .. bottom end
  if refused ~= 0 then return refused .. " panel updates refused" end
  return nil
end

local function gaps(list)
  local out = {}
  for i = 2, #list do out[#out + 1] = list[i] - list[i - 1] end
  return out
end

local function near(a, b) return a ~= nil and math.abs(a - b) <= 2 end

function on_tick(g, tick)
  if t0 == nil or ended then return end
  for _, tr in ipairs(TRACKS) do
    if tr.changes ~= counted[tr.id] then
      counted[tr.id] = tr.changes
      local list = starts[tr.id]
      list[#list + 1] = tr.next_at - tr.mode_len * 100
    end
  end
  if bad == nil then
    bad = check(g)
    if bad ~= nil then verdict_fail(bad); return end
  end
  if not skipped and tick >= t0 + 2550 then
    skipped = true
    command(ME, "next builder")
  end
  if tick >= t0 + 5050 then
    ended = true
    if TANK.mode.name ~= "Hovercraft" or BUILDER.mode.name ~= "Hustle" then
      verdict_fail("in force at the end: " .. TANK.mode.name .. " / " .. BUILDER.mode.name)
      return
    end
    both_in = g.rule("speed_road") == start.speed_grass
              and g.rule("man_speed_grass") == pct_of(start.man_speed_grass, 125)
    g.end_round("arena over")
  end
end

function on_end(g)
  mod_end()
  if bad ~= nil then verdict_fail(bad); return end
  for _, r in ipairs(WATCH) do
    if g.rule(r) ~= start[r] then
      verdict_fail(string.format("after on_end %s is %s, want %s", r,
                                 tostring(g.rule(r)), tostring(start[r])))
      return
    end
  end
  if TANK.saved ~= nil or BUILDER.saved ~= nil then
    verdict_fail("a track still holds rules to put back")
    return
  end
  if not both_in then
    verdict_fail("Hovercraft and Hustle were not both in force at the end")
    return
  end
  local tg, bg = gaps(starts.tank), gaps(starts.builder)
  local ok = #starts.tank == 3 and #starts.builder == 2
             and near(tg[1], 2000) and near(tg[2], 2000) and near(bg[1], 2600)
             and near(starts.tank[1], starts.builder[1])
  verdict(ok, string.format("tank gaps %s, builder gaps %s; every rule back; " ..
                            "Hovercraft + Hustle together",
                            table.concat(tg, "/"), table.concat(bg, "/")))
end

VERDICT_CHECK = function(g)
  if bad ~= nil then return false, bad end
  return false, "the round never ended"
end
