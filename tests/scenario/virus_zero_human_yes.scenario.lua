-- GATE: ticks=2000 bots=4 script=data/mods/Virus.scenario.lua
--
-- Virus, the lobby setting "First infected is a person" set to Yes.
--
-- Seats 0 and 1 play people: the arena makes game.lobby_slot read them as
-- no bot; seats 2 and 3 read as bots. game.setting("zero_human") answers
-- "Yes", the way the lobby's choice does.
--
-- "Yes" draws from the people, whatever the number of bots.
--
-- PASS: the first to turn (zero) is a person and is on the infected side,
-- and 200 more calls of pick_zero all answer a person.

ARENA = { want = "Yes", person = true, draws = 200,
          people = { [0] = true, [1] = true } }
-- The seats in ARENA.people are people and every other seat a bot,
-- whatever the server says.
ARENA.real_slot = game.lobby_slot
game.lobby_slot = function(p)
  local real = ARENA.real_slot(p)
  if real == nil then
    if game.tank(p) == nil then
      return nil
    end
    real = { connected = true, fielded = true, name = "P" .. p }
  end
  local s = {}
  for k, v in pairs(real) do
    s[k] = v
  end
  s.bot = not ARENA.people[p]
  return s
end

-- The lobby's answer for the setting under test.
ARENA.real_setting = game.setting
game.setting = function(id)
  if id == "zero_human" then
    return ARENA.want
  end
  return ARENA.real_setting(id)
end

ARENA.check = function()
  if zero == nil then
    verdict(false, "nobody turned")
    return
  end
  if side[zero] ~= INFECTED then
    verdict(false, string.format("zero p%d is not infected", zero))
    return
  end
  if ARENA.person ~= nil and (ARENA.people[zero] == true) ~= ARENA.person then
    verdict(false, string.format("%s: zero is p%d", ARENA.want, zero))
    return
  end
  -- pick_zero draws from the survivors, and zero is infected now. Put zero
  -- back on the survivors for the draws, then on the horde again.
  side[zero] = SURVIVORS
  local seen, people, bots = {}, 0, 0
  for _ = 1, ARENA.draws do
    local p = pick_zero()
    local bad = p == nil or game.tank(p) == nil or side[p] ~= SURVIVORS or
                (ARENA.person ~= nil and (ARENA.people[p] == true) ~= ARENA.person)
    if bad then
      side[zero] = INFECTED
      verdict(false, string.format("%s: pick_zero gave %s", ARENA.want,
                                   tostring(p)))
      return
    end
    seen[p] = true
    if ARENA.people[p] then people = people + 1 else bots = bots + 1 end
  end
  side[zero] = INFECTED
  if ARENA.person == nil and (people == 0 or bots == 0) then
    verdict(false, string.format("%s: %d draws, person %d, bots %d",
                                 ARENA.want, ARENA.draws, people, bots))
    return
  end
  local list = {}
  for p = 0, game.max_tanks() - 1 do
    if seen[p] then list[#list + 1] = "p" .. p end
  end
  verdict(true, string.format("%s: zero p%d, draws %s, person %d bots %d",
                              ARENA.want, zero, table.concat(list, ","),
                              people, bots))
end

ARENA.real_start = on_start
function on_start()
  ARENA.real_start()
  game.timer(HEAD_START + 1, ARENA.check)
end
