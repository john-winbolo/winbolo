-- Rule Roulette: a rule mode in force when the round ends puts its rules
-- back.
--
-- The runner writes data/mods/RuleRoulette.scenario.lua in front of this
-- text (the include on the GATE line), so the mod's locals are in scope:
-- the queue is given Cleanup Crew first, before the mod's on_start takes a
-- mode from it. The round starts with lgm_cost_road at 3, a host's value
-- and not the classic 2. Five seconds in, with lgm_cost_road at 0, the
-- arena ends the round; after the mod's on_end, lgm_cost_road must be 3
-- again and the mod must hold no rules to put back.
--
-- GATE: ticks=3000 bots=0 ai=yesfull gametype=open include=data/mods/RuleRoulette.scenario.lua

local roulette = scenario
local mod_start, mod_end, mod_spawned = on_start, on_end, on_tank_spawned
on_chat = nil   -- a human's !roulette; the tail must not wrap it

scenario = {
  name        = GATE_NAME,
  description = "Rule Roulette puts Cleanup Crew's road cost back at the round's end.",
  game        = GATE_GAMETYPE,
  api         = 1,
  needs_bots  = true,
  rules = {
    base_regen_ticks = roulette.rules.base_regen_ticks,
    lgm_cost_road    = 3,
  },
  settings = roulette.settings,
}

local ME        = 0
local end_at    = 500       -- game.tick() the arena ends the round on
local during    = nil       -- lgm_cost_road just before the end
local ended     = false

function on_setup(g)
  g.spawn_bot{ slot = ME, name = "Idle", brain = "idle" }
end

function on_start(g)
  for i, m in ipairs(MODES) do
    if m.name == "Cleanup Crew" then queue[#queue + 1] = i end
  end
  mod_start()
end

function on_tank_spawned(g, p, ...)
  return mod_spawned(p, ...)
end

function on_tick(g, tick)
  if not ended and tick >= end_at then
    ended  = true
    during = g.rule("lgm_cost_road")
    if mode == nil or mode.name ~= "Cleanup Crew" then
      verdict_fail("mode in force is " .. (mode and mode.name or "none"))
      return
    end
    g.end_round("arena over")
  end
end

function on_end(g)
  mod_end()
  local after = g.rule("lgm_cost_road")
  verdict(during == 0 and after == 3 and saved == nil,
          string.format("lgm_cost_road %s in the mode, %s after on_end, saved %s",
                        tostring(during), tostring(after), tostring(saved)))
end
