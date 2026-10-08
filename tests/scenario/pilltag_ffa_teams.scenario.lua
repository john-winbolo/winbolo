-- GATE: ticks=1500 bots=3 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, Free For All: every seat on a team of its own, whoever holds
-- the prize. This is how a Free For All round played before Everyone vs It
-- came in, and it must not change.
--
-- The arena picks Free For All (TEAMS.vs_it = false, as the "Teams" setting
-- would) before the round starts.
-- Step 1, tick 200: nobody holds the prize. No two seats are allied, and no
-- two seats share a team.
-- Step 2, tick 300: seat 0 gets the prize. Five ticks later the same holds,
-- every seat is on the team it had in step 1, and can_ally refuses every
-- pair.
--
-- PASS: both steps hold.

TEAMS.vs_it = false
ARENA = { step = 0, team = {} }

function ARENA.check(name, keep)
  local bad = {}
  for a = 0, 2 do
    local slot = game.lobby_slot(a)
    local t = slot and slot.team
    game.log(string.format("ARENA %s: seat %d team %s", name, a, tostring(t)))
    if t == nil or t == 0 then
      bad[#bad + 1] = string.format("seat %d team %s", a, tostring(t))
    end
    if keep and ARENA.team[a] ~= t then
      bad[#bad + 1] = string.format("seat %d moved %s->%s", a,
                                    tostring(ARENA.team[a]), tostring(t))
    end
    ARENA.team[a] = t
    for b = a + 1, 2 do
      local tb = game.lobby_slot(b) and game.lobby_slot(b).team
      if game.allied(a, b) == true then
        bad[#bad + 1] = string.format("allied(%d,%d)", a, b)
      end
      if t == tb then
        bad[#bad + 1] = string.format("seats %d,%d share team %s", a, b, tostring(t))
      end
      if keep and (can_ally(a, b) or can_ally(b, a)) then
        bad[#bad + 1] = string.format("can_ally(%d,%d)", a, b)
      end
    end
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
  if tick >= GATE_TICKS - 100 then
    verdict(false, "stuck at step " .. A.step)
  end
  if A.step == 0 and tick >= 200 then
    if TEAMS.vs_it_on() or teams_on() then
      verdict(false, "the round is not a Free For All")
      return
    end
    local pb = game.pill(pill)
    game.teleport(0, pb.x, pb.y, 64)
    game.teleport(1, pb.x - 12, pb.y, 64)
    game.teleport(2, pb.x + 12, pb.y, 64)
    A.check("start", false)
    A.step = 1
  elseif A.step == 1 and tick >= 300 then
    game.give_pill(0, pill)
    A.step, A.at = 2, tick
  elseif A.step == 2 and tick >= A.at + 5 then
    A.check("seat 0 holds", true)
    verdict(holder == 0, "own teams, no alliances, before and after the take "
                         .. "(holder " .. tostring(holder) .. ")")
  end
end
