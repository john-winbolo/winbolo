-- GATE: ticks=2000 bots=3 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, Everyone vs It (the default "Teams" setting): the teams follow
-- the prize.
--
-- Step 1, tick 200: nobody holds the prize, so every seat is a hunter, all
-- three on the hunters' team and allied to each other.
-- Step 2, tick 300: seat 0 gets the prize. Five ticks later seat 0 is on the
-- holder's team alone, allied to nobody, and seats 1 and 2 are still allies.
-- can_ally refuses 0 with 1 and 1 with 0, and allows 1 with 2.
-- Step 3, tick 400: seat 0's tank is killed with the prize aboard, so the
-- prize drops dead and nobody holds it. Five ticks later all three are
-- allies again.
-- Step 4, tick 500: seat 1 gets the prize. Five ticks later seat 1 is alone.
-- Step 5, tick 600: seat 1's prize is put down two squares from his tank
-- (game.drop_pill, which puts a pillbox down dead, the way a sinking tank
-- drops it). Nobody holds it, so five ticks later all three are allies.
-- Step 6, tick 700: seat 2 gets the prize. Five ticks later seat 2 is alone,
-- seats 0 and 1 are allies, and can_ally refuses 1 with 2 and allows 0 with
-- 1.
--
-- The tanks are kept far apart and at full armour, so no bot gets the prize
-- by driving over it between the steps.
--
-- PASS: every check at every step holds.

ARENA = { step = 0 }

function ARENA.allies(a, b)
  return game.allied(a, b) == true
end

-- The check of one step: want[i] is { a, b, allied? }. Logs every pair.
function ARENA.check(name, want, alone)
  local bad = {}
  for _, w in ipairs(want) do
    local got = ARENA.allies(w[1], w[2])
    game.log(string.format("ARENA %s: allied(%d,%d)=%s want %s", name, w[1],
                           w[2], tostring(got), tostring(w[3])))
    if got ~= w[3] then
      bad[#bad + 1] = string.format("allied(%d,%d)=%s", w[1], w[2], tostring(got))
    end
  end
  for p = 0, 2 do
    local slot = game.lobby_slot(p)
    local want_team = (p == alone) and TEAMS.IT or TEAMS.HUNTERS
    game.log(string.format("ARENA %s: seat %d team %s want %d", name, p,
                           tostring(slot and slot.team), want_team))
    if slot == nil or slot.team ~= want_team then
      bad[#bad + 1] = string.format("seat %d team %s", p, tostring(slot and slot.team))
    end
  end
  if holder ~= alone then
    bad[#bad + 1] = "holder " .. tostring(holder)
  end
  if #bad > 0 then
    verdict(false, name .. ": " .. table.concat(bad, ", "))
  end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if tick % 25 == 0 then
    local pb = game.pill(pill)
    for p = 0, 2 do
      local t = game.tank(p)
      if t ~= nil and not t.dead and pb ~= nil then
        game.set_stocks(p, { armour = game.rule("tank_full_armour") })
      end
    end
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, "stuck at step " .. A.step)
  end
  if A.step == 0 and tick >= 200 then
    if not TEAMS.vs_it_on() then
      verdict(false, "the default Teams setting is not Everyone vs It")
      return
    end
    local pb = game.pill(pill)
    game.teleport(0, pb.x, pb.y, 64)
    game.teleport(1, pb.x - 12, pb.y, 64)
    game.teleport(2, pb.x + 12, pb.y, 64)
    A.check("start", { { 0, 1, true }, { 0, 2, true }, { 1, 2, true } }, nil)
    A.step = 1
  elseif A.step == 1 and tick >= 300 then
    game.give_pill(0, pill)
    A.step, A.at = 2, tick
  elseif A.step == 2 and tick >= A.at + 5 then
    A.check("seat 0 holds", { { 0, 1, false }, { 0, 2, false }, { 1, 2, true } }, 0)
    local c01, c10, c12 = can_ally(0, 1), can_ally(1, 0), can_ally(1, 2)
    game.log(string.format("ARENA can_ally(0,1)=%s can_ally(1,0)=%s can_ally(1,2)=%s",
                           tostring(c01), tostring(c10), tostring(c12)))
    if c01 or c10 or not c12 then
      verdict(false, "can_ally let the holder ally, or refused two hunters")
      return
    end
    A.step = 3
  elseif A.step == 3 and tick >= 400 then
    game.kill_tank(0)
    A.step, A.at = 4, tick
  elseif A.step == 4 and tick >= A.at + 5 then
    A.check("holder died", { { 0, 1, true }, { 0, 2, true }, { 1, 2, true } }, nil)
    A.step = 5
  elseif A.step == 5 and tick >= 500 then
    local pb = game.pill(pill)
    game.teleport(1, pb.x, pb.y, 64)
    game.give_pill(1, pill)
    A.step, A.at = 6, tick
  elseif A.step == 6 and tick >= A.at + 5 then
    A.check("seat 1 holds", { { 0, 1, false }, { 1, 2, false }, { 0, 2, true } }, 1)
    A.step = 7
  elseif A.step == 7 and tick >= 600 then
    local t = game.tank(1)
    local x, y = standable_near(t.mx + 2, t.my)
    A.drop = game.drop_pill(1, pill, x, y)
    A.step, A.at = 8, tick
  elseif A.step == 8 and tick >= A.at + 5 then
    local pb = game.pill(pill)
    game.log(string.format("ARENA drop %s: prize armour %s in_tank %s holder %s",
                           tostring(A.drop), tostring(pb.armour),
                           tostring(pb.in_tank), tostring(holder)))
    A.check("seat 1 dropped it", { { 0, 1, true }, { 0, 2, true }, { 1, 2, true } }, nil)
    A.step = 9
  elseif A.step == 9 and tick >= 700 then
    local pb = game.pill(pill)
    game.teleport(2, pb.x, pb.y, 64)
    game.teleport(1, pb.x - 12, pb.y, 64)
    game.give_pill(2, pill)
    A.step, A.at = 10, tick
  elseif A.step == 10 and tick >= A.at + 5 then
    A.check("seat 2 took it", { { 0, 2, false }, { 1, 2, false }, { 0, 1, true } }, 2)
    if can_ally(1, 2) or can_ally(2, 0) or not can_ally(0, 1) then
      verdict(false, "can_ally after the take")
      return
    end
    verdict(true, "all hunters with nobody holding; holder alone after "
                  .. "each take; can_ally refuses the holder")
  end
end
