-- GATE: ticks=3000 bots=4 script=data/mods/Virus.scenario.lua
--
-- Virus, the first one's numbers shrink as the horde grows.
--
-- With n infected seats, the first one included, each field of the first
-- one's modifiers is floor + (full - floor) x 0.5^(n - 1), rounded to a
-- whole percent. The arena works the want out from its own copy of the
-- numbers, not from the script's table:
--   n = 1: speed 145, accel 150, turn 125, reload 100, dealt 100, taken 80
--   n = 2: speed 128, accel 130, turn 115, reload 100, dealt 100, taken 88
--   n = 3: speed 119, accel 120, turn 110, reload 100, dealt 100, taken 91
--
-- All four seats are bots and the setting is the default, so a bot turns
-- first at HEAD_START. One second later the arena reads the first one's
-- mods (n = 1), then turns one survivor (n = 2), then another (n = 3),
-- reading the mods a second after each. Then it wipes the first one's
-- modifiers to the classic tank and kills the tank: when it is back, the
-- net in on_tank_spawned must have handed it the n = 3 set, not the full
-- one.
--
-- PASS: every reading is the want for its n, and so is the reading after
-- the respawn.

ARENA = { step = 0 }
ARENA.full  = { speed = 145, accel = 150, turn = 125,
                reload = 100, dealt = 100, taken = 80 }
ARENA.floor = { speed = 110, accel = 110, turn = 105,
                reload = 100, dealt = 100, taken = 95 }

ARENA.want = function(n)
  local out = {}
  for k, full in pairs(ARENA.full) do
    local fl = ARENA.floor[k]
    out[k] = math.floor(fl + (full - fl) * 0.5 ^ (n - 1) + 0.5)
  end
  return out
end

ARENA.text = function(m)
  return string.format("%s/%s/%s/%s/%s/%s", tostring(m.speed),
    tostring(m.accel), tostring(m.turn), tostring(m.reload),
    tostring(m.dealt), tostring(m.taken))
end

-- Reads the first one's mods against the want for n. True when they match.
ARENA.read = function(n, what)
  local t = zero ~= nil and game.tank(zero) or nil
  if t == nil or t.mods == nil then
    verdict(false, what .. ": no tank for zero")
    return false
  end
  local want = ARENA.want(n)
  game.log(string.format("ARENA %s n=%d mods %s want %s", what, n,
                         ARENA.text(t.mods), ARENA.text(want)))
  for k, v in pairs(want) do
    if t.mods[k] ~= v then
      verdict(false, string.format("%s n=%d: %s want %s", what, n,
                                   ARENA.text(t.mods), ARENA.text(want)))
      return false
    end
  end
  return true
end

-- Turns one more survivor, not the first one.
ARENA.turn_one = function()
  for p = 0, game.max_tanks() - 1 do
    if side[p] == SURVIVORS and in_round(p) then
      infect(p, nil)
      return p
    end
  end
  return nil
end

ARENA.tick = function()
  local A = ARENA
  A.step = A.step + 1
  if A.step == 1 then
    if zero == nil or side[zero] ~= INFECTED then
      verdict(false, "nobody turned")
      return
    end
    if not A.read(1, "first") then return end
    if A.turn_one() == nil then verdict(false, "no survivor to turn") return end
  elseif A.step == 2 then
    if #roll(INFECTED) ~= 2 then
      verdict(false, string.format("infected %d, want 2", #roll(INFECTED)))
      return
    end
    if not A.read(2, "second") then return end
    if A.turn_one() == nil then verdict(false, "no survivor to turn") return end
  elseif A.step == 3 then
    if #roll(INFECTED) ~= 3 then
      verdict(false, string.format("infected %d, want 3", #roll(INFECTED)))
      return
    end
    if not A.read(3, "third") then return end
    game.set_modifiers(zero, {})
    A.deaths = game.tank(zero).deaths
    game.kill_tank(zero)
  elseif A.step >= 4 then
    local t = game.tank(zero)
    if t == nil or t.dead or t.deaths == A.deaths then
      if A.step > 12 then
        verdict(false, "the first one never came back")
        return
      end
    else
      if A.read(3, "respawn") then
        verdict(true, "n=1,2,3 and respawn mods as wanted")
      end
      return
    end
  end
  game.timer(1, A.tick)
end

ARENA.real_start = on_start
function on_start()
  ARENA.real_start()
  game.timer(HEAD_START + 1, ARENA.tick)
end
