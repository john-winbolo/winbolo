-- GATE: ticks=12000 bots=1 script=data/maps/tutorials/Classic Tutorial.scenario.lua
--
-- Classic Tutorial: where the tank respawns, and the far-bank message.
--
-- As in the old tutorial, the tank respawns at start 1 (out at sea) until
-- the boat step (row 66) has fired, and at start 2 (the far bank) after it.
-- The first respawn at start 2 shows respawn1, once.
--
-- Player 0 plays the tutorial's player, held still. The arena kills the
-- real tank with game.kill_tank and reads where on_tank_spawned puts the
-- new one. To reach the boat step it puts a stand-in over game.tank for a
-- while and moves the tank one row north a tick from row 214 to row 60, then
-- takes the stand-in away.
--   1. Kill the tank before the boat step: it respawns near start 1, and no
--      far-bank message is sent.
--   2. Drive the stand-in past row 66.
--   3. Kill the tank: it respawns near start 2, and respawn1 is
--      sent.
--   4. Kill it again: it respawns near start 2, and no second message.
--
-- PASS: all of the above.

CLASSIC_TUTORIAL_PLAYER = 0
ARENA = { phase = 0, fake = false, my = 214, respawns = {}, msg = 0,
          wait = 0, done = false }

local real_tank = game.tank
game.tank = function(p)
  local t = real_tank(p)
  if ARENA.fake and p == 0 and t ~= nil then
    t.my = ARENA.my
    t.dead = false
  end
  return t
end

-- The message a popup is, read from the script's English table: its key,
-- the input it is worded for, and which of its parts it is.
ARENA_TEXT = {}
for key, v in pairs(TEXT.en.pop) do
  local wordings = v
  if type(v) ~= "table" or v.keyboard == nil then wordings = { keyboard = v } end
  for input, w in pairs(wordings) do
    local parts = w
    if type(w) == "string" then parts = { w } end
    for i, part in ipairs(parts) do
      ARENA_TEXT[part] = { key = key, input = input, part = i, parts = #parts }
    end
  end
end

local real_popup = game.popup
game.popup = function(text, p, ...)
  local m = ARENA_TEXT[text]
  if m ~= nil and m.key == "respawn1" and m.part == 1 then
    ARENA.msg = ARENA.msg + 1
  end
  return real_popup(text, p, ...)
end

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p ~= 0 then return end
  game.set_modifiers(0, { speed = 1, turn = 1 })
  if respawn then
    ARENA.respawns[#ARENA.respawns + 1] = { mx = mx, my = my, msg = ARENA.msg }
  end
end

-- True when (mx, my) is within the start scatter of start n.
local function near_start(r, n)
  local s = game.start(n)
  return s ~= nil and math.abs(r.mx - s.x) <= 8 and math.abs(r.my - s.y) <= 8
end

local function fail(why)
  ARENA.done = true
  verdict(false, why)
end

local arena_real_tick = on_tick
function on_tick(tick)
  local A = ARENA
  if A.done then return end
  arena_real_tick(tick)
  local t = real_tank(0)
  if t == nil then return end
  if A.wait > 0 then A.wait = A.wait - 1; return end

  if A.phase == 0 then
    if not t.dead and S.step > 1 then   -- the intro is out
      game.kill_tank(0)
      A.phase = 1
    end
  elseif A.phase == 1 then
    if #A.respawns >= 1 then
      local r = A.respawns[1]
      if not near_start(r, 1) then
        return fail(string.format("first respawn at %d,%d, not start 1", r.mx, r.my))
      end
      if A.msg ~= 0 then return fail("far-bank message before the boat step") end
      A.fake, A.my, A.phase = true, 214, 2
    end
  elseif A.phase == 2 then
    A.my = A.my - 1
    if A.my <= 60 then
      if not S.far_bank then return fail("the boat step did not fire") end
      A.fake, A.phase, A.wait = false, 3, 5
    end
  elseif A.phase == 3 then
    if not t.dead then
      game.kill_tank(0)
      A.phase = 4
    end
  elseif A.phase == 4 then
    if #A.respawns >= 2 then
      local r = A.respawns[2]
      if not near_start(r, 2) then
        return fail(string.format("second respawn at %d,%d, not start 2", r.mx, r.my))
      end
      if A.msg ~= 1 then
        return fail(string.format("%d far-bank messages after the first far-bank respawn", A.msg))
      end
      A.phase, A.wait = 5, 5
    end
  elseif A.phase == 5 then
    if not t.dead then
      game.kill_tank(0)
      A.phase = 6
    end
  elseif A.phase == 6 then
    if #A.respawns >= 3 then
      local r = A.respawns[3]
      if not near_start(r, 2) then
        return fail(string.format("third respawn at %d,%d, not start 2", r.mx, r.my))
      end
      if A.msg ~= 1 then
        return fail(string.format("%d far-bank messages after two far-bank respawns", A.msg))
      end
      A.done = true
      verdict(true, "start 1, then start 2 with one far-bank message")
    end
  end
end
