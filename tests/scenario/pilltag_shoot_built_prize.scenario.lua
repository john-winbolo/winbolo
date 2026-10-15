-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a hunter shoots a built prize while it stands, and its guard does not.
--
-- Seat 0 gets the prize at tick 300. Until he builds, the arena holds his
-- armour at 15 (at or below FORT_ARMOUR_AT, so he builds a fort, not a hop)
-- and keeps seat 1 7 squares behind him. When the fort stands, the script
-- tells seat 1 to attack it ({ verb = "attack", pill = <prize> }, the order
-- "attack pill" gives), in place of its chase order. While walled.update has
-- given it a ring square with a line to the prize, it is first sent there
-- once with a goto, and after the goto's short hold its own choice plays
-- on (walled.LINE_THEN_ATTACK, off: on, it is told to attack again there).
--
-- When the fort goes up, seat 1 is put 5 squares behind the guard, so he is
-- close enough that the guard does not take it straight back.
--
-- PASS: once the prize is built, seat 1 hits it while it stands (alive, on
-- the map), or kills its guard. Whatever the guard is doing with it counts:
-- a hit after he has started to take it back (plan.kind "take") is still a
-- hunter's hit on the built prize. Decided when the guard's built prize is
-- over (taken back, shot down, or its guard dead).
-- A guard's own hit on his prize is logged, not counted: the guard role
-- keeps its gun for the hunters, and a shot at a hunter can catch it.

ARENA = { given = false, hits = {} }
scenario.callbacks.pill_damage_scale = "Arena: counts the hits on the fort."

function pill_damage_scale(attacker, n, cause, by_pill)
  local A = ARENA
  if n == pill and A.built_at ~= nil and plan ~= nil and plan.built then
    local key = tostring(attacker) .. ":" .. plan.kind
    if attacker == 1 then
      A.hunter_hits = (A.hunter_hits or 0) + 1
    end
    A.hits[key] = (A.hits[key] or 0) + 1
    -- An ask hook may not write, so the line is logged from on_tick.
    A.lines = A.lines or {}
    A.lines[#A.lines + 1] = string.format("ARENA hit by %s on %s %s t=%d armour=%d",
      tostring(attacker), plan.kind, tostring(cause), game.tick(), game.pill(pill).armour)
  end
  return 100
end

-- A kill of the guard by the hunter while the fort stands ends the fort.
local arena_real_killed = on_tank_killed
function on_tank_killed(victim, killer, cause, scripted)
  local A = ARENA
  if victim == 0 and killer == 1 and plan ~= nil and plan.built and
     plan.kind == "fort" then
    A.guard_killed = game.tick()
  end
  arena_real_killed(victim, killer, cause, scripted)
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if not A.given then
    if tick >= 300 then
      local pb = game.pill(pill)
      game.teleport(0, pb.x, pb.y, 64)
      game.teleport(1, pb.x - 7, pb.y, 64)
      game.give_pill(0, pill)
      A.given = true
    end
    return
  end
  for _, line in ipairs(A.lines or {}) do game.log(line) end
  A.lines = nil
  local me = game.tank(0)
  if plan == nil and A.built_at == nil and tick % 100 == 0 and me ~= nil
     and not me.dead then
    game.set_stocks(0, { armour = 15 })
    local fx, fy = facing(me.dir)
    local k = 7 / math.max(math.abs(fx), math.abs(fy))
    game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
  end
  if plan ~= nil and plan.built and A.built_at == nil then
    A.built_at = tick
    game.log(string.format("ARENA %s up at %d,%d t=%d", plan.kind, plan.x,
                           plan.y, tick))
    if plan.kind ~= "fort" then
      verdict(false, "built a " .. plan.kind .. " with armour 15")
    end
  end
  if plan ~= nil and not plan.built and A.moved == nil and me ~= nil then
    A.moved = true
    local fx, fy = facing(me.dir)
    local k = 5 / math.max(math.abs(fx), math.abs(fy))
    game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
  end
  if ((A.hunter_hits or 0) > 0 or A.guard_killed ~= nil) and
     A.built_at ~= nil and (plan == nil or plan.kind ~= "fort") then
    -- Decided once the fort is over: taken back, shot down, or dead with
    -- its guard.
    verdict(true, string.format("hunter hit the built prize %d time(s) "
                                .. "(%d while guarded), guard %d, "
                                .. "hunter killed the guard: %s",
                                A.hunter_hits or 0, A.hits["1:fort"] or 0,
                                A.hits["0:fort"] or 0,
                                tostring(A.guard_killed ~= nil)))
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, A.built_at == nil and "no fort" or
                   "the hunter never hit the built prize or killed its guard")
  end
end
