-- GATE: ticks=2500 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, the full boost puts slow ground at road speed and back.
--
-- At the first tick the arena sets speed_grass to 20, over the road, and
-- notes every speed_* rule and turn_river. Seat 0 gets the prize at tick 300.
--
-- PASS, all of:
--  * 50 ticks into the boost every speed_* rule that was under speed_road is
--    at speed_road, grass is still 20, and turn_river is untouched;
--  * once BOOST_SECONDS are up, every rule is back at the value noted;
--  * at tick 700 the holder is killed, so the prize is loose; it is given to
--    seat 0 again (or seat 1 drives over it first), and 30 ticks into that
--    boost the new holder is killed: the rules are back at once.

ARENA = { step = "note", seen = {} }

ARENA.check = function(want_lift, what)
  local A = ARENA
  local road = game.rule("speed_road")
  for name, was in pairs(A.seen) do
    local now = game.rule(name)
    local want = was
    if want_lift and name ~= "turn_river" and was < road then
      want = road
    end
    if now ~= want then
      verdict(false, string.format("%s: %s is %s, wanted %s", what, name,
                                   tostring(now), tostring(want)))
      return false
    end
  end
  return true
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if A.step == "note" then
    game.set_rule("speed_grass", 20)
    for _, name in ipairs(road_lift.rules) do
      A.seen[name] = game.rule(name)
    end
    A.seen.turn_river = game.rule("turn_river")
    A.step = "give"
  elseif A.step == "give" and tick >= 300 then
    game.give_pill(0, pill)
    A.given_at = tick
    A.step = "lifted"
  elseif A.step == "lifted" and tick >= A.given_at + 50 then
    if road_lift.saved == nil then
      verdict(false, "no lift 50 ticks after the take")
      return
    end
    if not ARENA.check(true, "in the boost") then return end
    game.log(string.format("ARENA lifted t=%d river=%d grass=%d", tick,
                           game.rule("speed_river"), game.rule("speed_grass")))
    A.step = "timer"
  elseif A.step == "timer" and tick >= A.given_at + BOOST_SECONDS * 100 + 50 then
    if road_lift.saved ~= nil then
      verdict(false, "the lift outlived BOOST_SECONDS")
      return
    end
    if not ARENA.check(false, "after the boost") then return end
    game.log(string.format("ARENA restored on time t=%d river=%d", tick,
                           game.rule("speed_river")))
    A.step = "regive"
  elseif A.step == "regive" and tick >= 700 then
    if holder ~= nil then
      game.kill_tank(holder)
    end
    A.step = "retake"
  elseif A.step == "retake" and holder == nil then
    local t = game.tank(0)
    if t ~= nil and not t.dead and tick % 50 == 0 then
      game.give_pill(0, pill)
    end
  elseif A.step == "retake" and road_lift.saved ~= nil then
    -- Seat 0 was given it, or seat 1 drove over it: a take either way.
    A.given_at = tick
    A.step = "kill"
  elseif A.step == "kill" and tick >= A.given_at + 30 then
    if road_lift.saved == nil or holder == nil then
      verdict(false, "the second boost ended before the kill")
      return
    end
    if not ARENA.check(true, "in the second boost") then return end
    A.to = holder
    game.kill_tank(holder)
    A.step = "dead"
  elseif A.step == "dead" and holder ~= A.to then
    if road_lift.saved ~= nil then
      verdict(false, "the lift outlived the holder's death")
      return
    end
    if not ARENA.check(false, "after the death") then return end
    verdict(true, string.format("lifted, back on time, back at the death "
                                .. "of seat %d t=%d", A.to, tick))
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, "stuck at step " .. A.step)
  end
end
