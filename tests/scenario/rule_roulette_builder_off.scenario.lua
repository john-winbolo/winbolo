-- Rule Roulette with the builder track off, then turned on and off again
-- from chat.
--
-- The runner writes data/mods/RuleRoulette.scenario.lua in front of this
-- text (the include on the GATE line), so the mod's locals -- TANK,
-- BUILDER, command, build_panel, panel_bottom, refused -- are in scope. The
-- lobby's builder interval is 0, so only the tank track runs, every 20 s:
-- Overdrive, Rust Bucket, Turbo. The builder queue is given Hustle, which
-- is only taken if the track comes on.
--
-- What is checked:
--   1. Up to 45.5 s no builder mode starts, no builder rule moves, and the
--      panel the mod would draw has "Builder: off" for its builder half.
--      With Overdrive (four number rows) and preview 3 it ends at 96: the
--      tank half to 80, the line at 81 and the "Builder: off" band 83..96.
--   2. The tank modes start 20 s apart.
--   3. At 45.5 s "!roulette builder 30" (through the mod's command
--      function) starts Hustle on the next second: the builder rules are
--      scaled and the panel's builder half shows Hustle.
--   4. At 50.5 s "!roulette builder 0" stops the track at once: Hustle's
--      rules are back and nothing is held to put back.
--   5. No panel update was refused, and every panel ends inside 128.
--
-- GATE: ticks=7000 bots=0 ai=yesfull gametype=open include=data/mods/RuleRoulette.scenario.lua

local roulette = scenario
local mod_start, mod_end, mod_spawned = on_start, on_end, on_tank_spawned
on_chat = nil   -- a human's !roulette; the tail must not wrap it

local arena_settings = {}
for i, st in ipairs(roulette.settings) do
  local c = {}
  for k, v in pairs(st) do c[k] = v end
  if c.id == "interval" then c.default = 20 end
  if c.id == "builder_interval" then c.default = 0 end
  if c.id == "preview" then c.default = 3 end
  arena_settings[i] = c
end

scenario = {
  name        = GATE_NAME,
  description = "Rule Roulette with the builder track off, then on and off from chat.",
  game        = GATE_GAMETYPE,
  api         = 1,
  needs_bots  = true,
  rules = {
    base_regen_ticks = roulette.rules.base_regen_ticks,
  },
  settings = arena_settings,
}

local BUILDER_RULES = { "lgm_cost_road", "lgm_helicopter_speed", "man_speed_road",
  "man_speed_grass", "man_speed_forest", "man_speed_river", "man_speed_swamp",
  "man_speed_crater", "man_speed_rubble", "man_speed_boat", "man_speed_deep_sea",
  "man_speed_refuel_base" }

local ME        = 0
local t0        = nil
local start     = nil     -- builder rule -> value as the round opened
local phase     = "off"   -- "off", "on" (asked), "stopped"
local bad       = nil
local starts    = {}      -- tick each tank mode started
local counted   = 0
local full_one  = 0       -- Overdrive frames with the builder-off panel at 96
local on_ok     = false   -- Hustle seen in force and on the panel
local stop_ok   = nil

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
  for _, r in ipairs(BUILDER_RULES) do start[r] = g.rule(r) end
  t0 = g.tick()
  force(TANK, { "Overdrive", "Rust Bucket", "Turbo" })
  force(BUILDER, { "Hustle" })
  mod_start()
end

function on_tank_spawned(g, p, ...)
  return mod_spawned(p, ...)
end

function on_end(g)
  mod_end()
end

local function rules_at_start(g)
  for _, r in ipairs(BUILDER_RULES) do
    if g.rule(r) ~= start[r] then
      return string.format("%s is %s, want %s", r, tostring(g.rule(r)), tostring(start[r]))
    end
  end
  return nil
end

local function has_text(list, s)
  for _, it in ipairs(list) do
    if it[1] == "text" and it[7] == s then return true end
  end
  return false
end

local function check(g)
  local list = build_panel()
  local bottom = panel_bottom(list)
  if bottom > 128 then return "panel ends at " .. bottom end
  if refused ~= 0 then return refused .. " panel updates refused" end
  if phase ~= "on" then
    if BUILDER.mode ~= nil then return "builder mode " .. BUILDER.mode.name .. " in force" end
    local why = rules_at_start(g)
    if why ~= nil then return "builder off: " .. why end
    if not has_text(list, "Builder: off") then return "builder off but no \"Builder: off\"" end
    if TANK.mode.name == "Overdrive" then
      if bottom ~= 96 then return "builder-off Overdrive panel ends at " .. bottom end
      full_one = full_one + 1
    end
  elseif BUILDER.mode ~= nil then
    if BUILDER.mode.name ~= "Hustle" then return "builder came on with " .. BUILDER.mode.name end
    if g.rule("man_speed_road") ~= math.floor(start.man_speed_road * 1.25 + 0.5) then
      return "Hustle: man_speed_road " .. tostring(g.rule("man_speed_road"))
    end
    if not has_text(list, "Builder: Hustle") then return "Hustle on but not on the panel" end
    on_ok = true
  end
  return nil
end

local function finish()
  if bad ~= nil then return false, bad end
  local gaps = {}
  for i = 2, #starts do gaps[#gaps + 1] = starts[i] - starts[i - 1] end
  if #starts < 3 then return false, #starts .. " tank modes started" end
  for _, d in ipairs(gaps) do
    if math.abs(d - 2000) > 2 then return false, "tank gaps " .. table.concat(gaps, "/") end
  end
  if full_one == 0 then return false, "the builder-off Overdrive panel never checked" end
  if not on_ok then return false, "Hustle never seen after builder 30" end
  if stop_ok ~= true then return false, tostring(stop_ok or "builder 0 never typed") end
  if BUILDER.changes ~= 1 then return false, BUILDER.changes .. " builder modes" end
  return true, string.format("tank gaps %s; builder off, on (Hustle), off with rules back",
                             table.concat(gaps, "/"))
end

function on_tick(g, tick)
  if t0 == nil then return end
  if TANK.changes ~= counted then
    counted = TANK.changes
    starts[#starts + 1] = TANK.next_at - TANK.mode_len * 100
  end
  if bad == nil then
    bad = check(g)
    if bad ~= nil then verdict_fail(bad); return end
  end
  if phase == "off" and tick >= t0 + 4550 then
    phase = "on"
    command(ME, "builder 30")
  elseif phase == "on" and tick >= t0 + 5050 then
    phase = "stopped"
    command(ME, "builder 0")
    local why = rules_at_start(g)
    if BUILDER.mode ~= nil then
      stop_ok = "builder still in force after builder 0"
    elseif BUILDER.saved ~= nil then
      stop_ok = "builder still holds rules to put back"
    elseif why ~= nil then
      stop_ok = "after builder 0: " .. why
    else
      stop_ok = true
    end
  elseif phase == "stopped" and tick >= t0 + 5550 then
    local ok, why = finish()
    verdict(ok, why)
  end
end

VERDICT_CHECK = function(g)
  return finish()
end
