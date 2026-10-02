-- GATE: ticks=6400 bots=2 script=data/mods/RuleRoulette.scenario.lua
--
-- Rule Roulette with "Multiply other mods' modifiers" at Yes, beside a
-- stand-in for another mod that writes modifiers the way Pillbox Tag does.
--
-- The gate runs one script per arena, so the other mod is played by this
-- arena: its on_tick runs first in the frame, then Rule Roulette's on_tick,
-- then the checks. That is the order the lobby gives when Rule Roulette is
-- listed ABOVE the other mod: on_tick runs up the list, bottom first.
--
-- The stand-in (ARENA.other), as Pillbox Tag does it:
--  * seat 0 is the "holder": every frame his speed modifier is set to a
--    carry percent for the square he is on (road 70, grass 85, forest 50,
--    anything else 60), whole set replaced, and only when his speed is not
--    that percent already (carry_legs);
--  * seat 1 has every modifier cleared every 20 ticks when its speed is not
--    0 (Pillbox Tag's refresh net);
--  * a killed tank has every modifier cleared (on_tank_killed).
--
-- The tank modes are forced: Turbo, Overdrive, Normal, 20 s each. The
-- builder track is off.
--
-- PASS, all of, on every frame seat 0 or seat 1 is alive:
--  * seat 0 ends the frame with base x mode, field by field, rounded:
--    base is { speed = carry percent, the rest 100 };
--  * while his carry percent and the mode stay the same, his speed stays
--    the same from one frame to the next (no flip-flop);
--  * seat 1 ends the frame with the mode's own six numbers;
--  * under Normal seat 0 has his base back (speed = carry percent);
--  * the checks saw two carry percents under Turbo or Overdrive (the arena
--    makes the squares round seat 0 road and then grass), and seat 0
--    alive again after a kill under Turbo.

ARENA = {
  carry = nil,        -- set below, by terrain code
  last  = nil,        -- { want, mode, speed } from seat 0 last frame
  wants = {},         -- carry percents checked under Turbo or Overdrive
  checks = { Turbo = 0, Overdrive = 0, Normal = 0 },
  killed_at = nil,
  respawned = false,
  checked_after_respawn = 0,
  paints = { { at = 1500, what = "road" }, { at = 2700, what = "grass" },
             { at = 3300, what = "road" } },
  painted = 0,
  failed = false,
}

-- The lobby settings this round plays: 20 s tank modes, no builder track,
-- no countdown, and multiply on.
for _, st in ipairs(scenario.settings) do
  if st.id == "interval" then st.default = 20 end
  if st.id == "builder_interval" then st.default = 0 end
  if st.id == "countdown" then st.default = 0 end
  if st.id == "multiply" then st.default = "Yes" end
end

ARENA.mod_start = on_start
ARENA.mod_tick  = on_tick
ARENA.mod_spawned = on_tank_spawned
scenario.callbacks.on_tank_killed = "The arena's stand-in mod clears a killed tank's modifiers."

function on_start()
  for _, want in ipairs({ "Turbo", "Overdrive", "Normal" }) do
    for i, m in ipairs(TANK.modes) do
      if m.name == want then TANK.queue[#TANK.queue + 1] = i end
    end
  end
  ARENA.carry = {
    [game.TERRAIN.road] = 70, [game.TERRAIN.grass] = 85,
    [game.TERRAIN.forest] = 50,
  }
  ARENA.mod_start()
end

function ARENA.pct(v)
  if v == nil or v == 0 then return 100 end
  return v
end

function ARENA.want(t)
  return ARENA.carry[game.map_tile(t.mx, t.my)] or 60
end

function ARENA.fail(why)
  if not ARENA.failed then
    ARENA.failed = true
    verdict(false, string.format("t=%d %s", game.tick(), why))
  end
end

-- The other mod's frame.
function ARENA.other(tick)
  local t = game.tank(0)
  if t ~= nil and not t.dead then
    local want = ARENA.want(t)
    if ARENA.pct(t.mods.speed) ~= want then
      game.set_modifiers(0, (want == 100) and {} or { speed = want })
    end
  end
  local t1 = game.tank(1)
  if t1 ~= nil and tick % 20 == 0 and t1.mods.speed ~= 0 then
    game.set_modifiers(1, {})
  end
end

function on_tank_killed(victim, killer, cause, scripted)
  game.set_modifiers(victim, {})
end

function on_tank_spawned(p, mx, my, respawn, scripted)
  ARENA.mod_spawned(p, mx, my, respawn, scripted)
  if p == 0 and respawn and ARENA.killed_at ~= nil then
    ARENA.respawned = true
  end
end

-- Makes the 5 by 5 squares round seat 0 `what` and puts him in the middle.
function ARENA.paint(what)
  local t = game.tank(0)
  if t == nil or t.dead then return false end
  if t.boat then game.set_boat(0, false) end
  local code = game.TERRAIN[what]
  for dy = -2, 2 do
    for dx = -2, 2 do
      game.set_tile(t.mx + dx, t.my + dy, code)
    end
  end
  game.teleport(0, t.mx, t.my)
  return true
end

function ARENA.check(tick)
  local mode = TANK.mode
  if mode == nil then return end
  local f = full_mods(mode)
  local t = game.tank(0)
  if t ~= nil and not t.dead then
    local want = ARENA.want(t)
    for _, k in ipairs(MOD_KEYS) do
      local base = (k == "speed") and want or 100
      local exp = math.floor(base * f[k] / 100 + 0.5)
      if ARENA.pct(t.mods[k]) ~= exp then
        ARENA.fail(string.format("%s seat0 %s=%d want %d (carry %d)", mode.name,
                                 k, ARENA.pct(t.mods[k]), exp, want))
        return
      end
    end
    local speed = ARENA.pct(t.mods.speed)
    local l = ARENA.last
    if l ~= nil and l.want == want and l.mode == mode.name and l.speed ~= speed then
      ARENA.fail(string.format("%s seat0 speed %d then %d at carry %d",
                               mode.name, l.speed, speed, want))
      return
    end
    ARENA.last = { want = want, mode = mode.name, speed = speed }
    ARENA.checks[mode.name] = (ARENA.checks[mode.name] or 0) + 1
    if mode.name ~= "Normal" then ARENA.wants[want] = true end
    if ARENA.respawned then
      ARENA.checked_after_respawn = ARENA.checked_after_respawn + 1
    end
  else
    ARENA.last = nil
  end
  local t1 = game.tank(1)
  if t1 ~= nil and not t1.dead then
    for _, k in ipairs(MOD_KEYS) do
      if ARENA.pct(t1.mods[k]) ~= f[k] then
        ARENA.fail(string.format("%s seat1 %s=%d want %d", mode.name, k,
                                 ARENA.pct(t1.mods[k]), f[k]))
        return
      end
    end
  end
end

function on_tick(tick)
  ARENA.other(tick)
  ARENA.mod_tick(tick)
  ARENA.check(tick)
  if ARENA.failed then return end
  -- A kill under Turbo; his respawn is checked like any other frame.
  if ARENA.killed_at == nil and tick >= 600 then
    local t = game.tank(0)
    if t ~= nil and not t.dead then
      game.kill_tank(0)
      ARENA.killed_at = tick
    end
  end
  local nxt = ARENA.paints[ARENA.painted + 1]
  if nxt ~= nil and tick >= nxt.at and ARENA.paint(nxt.what) then
    ARENA.painted = ARENA.painted + 1
  end
  if tick >= GATE_TICKS - 200 then
    local nw = 0
    for _ in pairs(ARENA.wants) do nw = nw + 1 end
    local c = ARENA.checks
    if c.Turbo < 500 or c.Overdrive < 500 or c.Normal < 500 then
      ARENA.fail(string.format("too few frames checked: Turbo %d Overdrive %d Normal %d",
                               c.Turbo, c.Overdrive, c.Normal))
    elseif nw < 2 then
      ARENA.fail("only one carry percent seen under Turbo and Overdrive")
    elseif not ARENA.respawned or ARENA.checked_after_respawn < 100 then
      ARENA.fail("seat 0 was not checked after a respawn")
    else
      verdict(true, string.format("Turbo %d Overdrive %d Normal %d frames, %d carry pcts, "
                                  .. "respawn ok", c.Turbo, c.Overdrive, c.Normal, nw))
    end
  end
end
