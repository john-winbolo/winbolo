-- GATE: ticks=3000 bots=1 script=data/maps/tutorials/Classic Tutorial.scenario.lua
--
-- Classic Tutorial: a row stops the tank only the first time the tank drives
-- north onto it, as the old tutorial's server did (tutorialMinRow). After a
-- respawn the tank drives back up through rows it has passed, and those do
-- not stop it or show their messages again. A tank that drives south onto a
-- row is not stopped either.
--
-- The arena puts a stand-in over game.tank for the tutorial's player and
-- moves it one row a tick along this path:
--   1. north from row 214 to row 150 (the steps on rows 210 to 159 fire);
--   2. a respawn: on_tank_spawned is called for a respawn on row 213;
--   3. north from row 213 to row 121 (rows 142 and 122 are new);
--   4. south to row 123, then north again to row 119 (row 122 is passed,
--      row 120 is new);
--   5. south onto row 120.
--
-- PASS:
--   - stop_tank is called on rows 210, 199, 192, 186, 181, 175, 166, 159,
--     142, 122 and 120, in that order, and on no other row;
--   - every step's popups are sent once: the intro, then the messages of the
--     steps on rows 210 to 120, and no far-bank message (the boat step has
--     not fired).

CLASSIC_TUTORIAL_PLAYER = 0
ARENA = { i = 1, path = {}, pops = {}, stops = {}, done = false }

do
  local P = ARENA.path
  for r = 214, 150, -1 do P[#P + 1] = r end
  P[#P + 1] = "spawn"
  for r = 213, 121, -1 do P[#P + 1] = r end
  P[#P + 1] = 122
  P[#P + 1] = 123
  for r = 122, 119, -1 do P[#P + 1] = r end
  P[#P + 1] = 120
end
ARENA.my = 214

ARENA.want_stops = { 210, 199, 192, 186, 181, 175, 166, 159, 142, 122, 120 }
ARENA.want_pops = {
  "start01", "start02", "start04",
  "t01", "t02", "t03", "t04",
  "t05", "t06", "t07", "t08",
  "t09", "t10", "t11", "t12",
  "t13", "t14",
}

local real_tank = game.tank
game.tank = function(p)
  local t = real_tank(p)
  if p == 0 and t ~= nil then
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
  if m == nil or m.part == 1 then
    ARENA.pops[#ARENA.pops + 1] = m and m.key or text
  end
  return real_popup(text, p, ...)
end

local real_stop = game.stop_tank
game.stop_tank = function(p)
  ARENA.stops[#ARENA.stops + 1] = ARENA.my
  return real_stop(p)
end

local function same(got, want, what)
  for k = 1, math.max(#got, #want) do
    if got[k] ~= want[k] then
      return false, string.format("%s %d is %s, want %s", what, k,
                                  tostring(got[k]), tostring(want[k]))
    end
  end
  return true
end

local arena_real_tick = on_tick
function on_tick(tick)
  local A = ARENA
  if A.done then return end
  if real_tank(0) == nil then return end
  local at = A.path[A.i]
  if at == nil then
    A.done = true
    local ok, why = same(A.stops, A.want_stops, "stop")
    if ok then ok, why = same(A.pops, A.want_pops, "popup") end
    verdict(ok, ok and string.format("%d stops and %d popups as the old "
                                     .. "tutorial", #A.stops, #A.pops) or why)
    return
  end
  if at == "spawn" then
    A.my = 213
    on_tank_spawned(0, 126, 213, true, false)
  else
    A.my = at
  end
  A.i = A.i + 1
  arena_real_tick(tick)
end
