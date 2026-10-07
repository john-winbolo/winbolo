-- GATE: ticks=40000 bots=3 script=data/maps/Joust.scenario.lua
--
-- Joust, a drowning is credited to the tank that sank it in the kill the
-- engine reports, not only on Joust's own scoreboard.
--
-- In Joust a shell knocks a tank off its boat and the tank drowns. The
-- engine names the victim as its own killer for a drowning, and each
-- client counts its HUD kills from the engine's kill event, so before
-- kill_credit the HUD never counted a drowning while Joust's scoreboard
-- did. Joust now answers kill_credit, and the engine puts the tank that
-- sank the victim in the killer's place.
--
-- Three bots fight on Joust.map as it ships. At every kill Joust scores,
-- the seat whose own kills went up must be the killer on_tank_killed was
-- told, which is the killer in the kill event the HUD counts from.
--
-- PASS: three drownings credited to another tank, and every kill Joust
-- scored named the same tank in the engine's kill. FAIL: a kill Joust
-- scored for a tank the engine did not name, or no three credited
-- drownings before the tick limit.

ARENA = { drowned = 0, scored = 0, done = false }

local arena_real_killed = on_tank_killed
function on_tank_killed(victim, killer, cause, scripted)
  local before = {}
  for p = 0, 15 do before[p] = own_kills[p] or 0 end
  arena_real_killed(victim, killer, cause, scripted)
  if ARENA.done then return end
  for p = 0, 15 do
    if (own_kills[p] or 0) ~= before[p] then
      ARENA.scored = ARENA.scored + 1
      if p ~= killer then
        ARENA.done = true
        verdict(false, string.format("Joust scored seat %d, engine killer %s (%s)",
                                     p, tostring(killer), cause))
        return
      end
    end
  end
  if cause == "deep_sea" and killer ~= victim and killer ~= game.NEUTRAL then
    ARENA.drowned = ARENA.drowned + 1
    game.log(string.format("ARENA drowning %d: %d sank %d at %d",
                           ARENA.drowned, killer, victim, game.tick()))
    if ARENA.drowned >= 3 then
      ARENA.done = true
      verdict(true, string.format("%d drownings credited, %d scored kills agree",
                                  ARENA.drowned, ARENA.scored))
    end
  end
end

function on_tick(tick)
  if not ARENA.done and tick >= GATE_TICKS - 100 then
    ARENA.done = true
    verdict(false, string.format("only %d credited drownings (%d scored)",
                                 ARENA.drowned, ARENA.scored))
  end
end
