-- GATE: ticks=4000 bots=1 script=data/maps/tutorials/Classic Tutorial.scenario.lua
--
-- Classic Tutorial on a real tank: the first step fires and the tank is
-- stopped when the real tank reaches row 210, with no stand-in for
-- game.tank.
--
-- Player 0 plays the tutorial's player, held still at sea near start 1,
-- south of row 210. After the intro the arena looks along row 210 for a
-- square game.teleport takes and puts the tank there, so the tank's row goes
-- north onto row 210 from one tick to the next.
--
-- PASS:
--   - the intro's two popups come first;
--   - STR_TUTORIAL01 is sent while the real tank is on row 210;
--   - game.stop_tank is called on row 210 and the server takes it.

CLASSIC_TUTORIAL_PLAYER = 0
ARENA = { phase = 0, x = nil, pops = {}, stop_ok = nil, stop_row = nil,
          wait = 0, done = false }

local real_popup = game.popup
game.popup = function(text, p, ...)
  local t = game.tank(0)
  ARENA.pops[#ARENA.pops + 1] = { text = text, my = t and t.my }
  return real_popup(text, p, ...)
end

local real_stop = game.stop_tank
game.stop_tank = function(p)
  local ok, code = real_stop(p)
  local t = game.tank(0)
  ARENA.stop_ok, ARENA.stop_row = ok, t and t.my
  return ok, code
end

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local arena_real_tick = on_tick
function on_tick(tick)
  local A = ARENA
  if A.done then return end
  arena_real_tick(tick)
  local t = game.tank(0)
  if t == nil or t.dead then return end
  if A.wait > 0 then A.wait = A.wait - 1; return end

  if A.phase == 0 and #A.pops >= 2 then
    -- The tank is still south of row 210, at sea near start 1.
    if t.my <= 210 then
      A.done = true
      return verdict(false, "the tank is on row " .. t.my .. " before the move")
    end
    for x = 100, 156 do
      if game.teleport(0, x, 210, 64) then
        A.x = x
        break
      end
    end
    if A.x == nil then
      A.done = true
      return verdict(false, "no square on row 210 takes a tank")
    end
    A.phase, A.wait = 2, 5
  elseif A.phase == 2 then
    A.done = true
    local p = A.pops
    if #p < 3 then return verdict(false, #p .. " popups") end
    if p[1].text ~= "{LANG:STR_TUTORIAL_START01}" or
       p[2].text ~= "{LANG:STR_TUTORIAL_START04}" then
      return verdict(false, "the intro is not first")
    end
    if p[3].text ~= "{LANG:STR_TUTORIAL01}" or p[3].my ~= 210 then
      return verdict(false, string.format("popup 3 is %s on row %s",
                                          p[3].text, tostring(p[3].my)))
    end
    if A.stop_row ~= 210 or not A.stop_ok then
      return verdict(false, string.format("stop on row %s, answered %s",
                                          tostring(A.stop_row), tostring(A.stop_ok)))
    end
    verdict(true, "step 1 fired on row 210 and the tank was stopped")
  end
end
