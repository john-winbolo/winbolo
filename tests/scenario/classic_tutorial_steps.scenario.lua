-- GATE: ticks=3000 bots=1 script=data/maps/tutorials/Classic Tutorial.scenario.lua
--
-- Classic Tutorial: every step's popups are sent, in the old tutorial's
-- order, each on the tick the tank reaches that step's row, and the tank is
-- stopped on each step's row.
--
-- The arena puts a stand-in over game.tank for the tutorial's player, so it
-- can move the tank one row north a tick from row 214 to row 15, with no
-- terrain in the way. game.popup and game.stop_tank are wrapped to note
-- what the script sends and on which row; the real calls still go out.
--
-- PASS:
--   - the intro's three popups are sent first, on the first tick;
--   - each later step's popups are sent on the tick the tank is on its row,
--     in the order of the old step table, and nothing else is sent;
--   - every popup is a whole "{LANG:NAME}" reference and the server takes it;
--   - game.stop_tank is called once on each step's row, and on no other row.

CLASSIC_TUTORIAL_PLAYER = 0
ARENA = { my = 214, pops = {}, stops = {}, bad = {}, done = false }

-- The old tutorial's steps (src/bolo/tutorial.c), row then message names.
ARENA.want = {
  { nil, "STR_TUTORIAL_START01", "STR_TUTORIAL_START02", "STR_TUTORIAL_START04" },
  { 210, "STR_TUTORIAL01" }, { 199, "STR_TUTORIAL02" }, { 192, "STR_TUTORIAL03" },
  { 186, "STR_TUTORIAL04" }, { 181, "STR_TUTORIAL05" }, { 175, "STR_TUTORIAL06" },
  { 166, "STR_TUTORIAL07" }, { 159, "STR_TUTORIAL08", "STR_TUTORIAL09" },
  { 142, "STR_TUTORIAL10", "STR_TUTORIAL11" }, { 122, "STR_TUTORIAL12" },
  { 120, "STR_TUTORIAL13", "STR_TUTORIAL14" }, { 110, "STR_TUTORIAL15" },
  { 103, "STR_TUTORIAL16" }, { 98, "STR_TUTORIAL17" },
  { 84, "STR_TUTORIAL18", "STR_TUTORIAL19" }, { 66, "STR_TUTORIAL20", "STR_TUTORIAL21" },
  { 47, "STR_TUTORIAL22", "STR_TUTORIAL23", "STR_TUTORIAL24" }, { 21, "STR_TUTORIAL25" },
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

local real_popup = game.popup
game.popup = function(text, p, ...)
  local ok = real_popup(text, p, ...)
  local name = string.match(text, "^{LANG:([A-Z0-9_]+)}$")
  if name == nil then
    ARENA.bad[#ARENA.bad + 1] = "popup is not a reference: " .. text
  elseif not ok then
    ARENA.bad[#ARENA.bad + 1] = "popup refused: " .. name
  end
  if p ~= 0 then ARENA.bad[#ARENA.bad + 1] = "popup sent to " .. tostring(p) end
  ARENA.pops[#ARENA.pops + 1] = { name = name or text, my = ARENA.my }
  return ok
end

local real_stop = game.stop_tank
game.stop_tank = function(p)
  ARENA.stops[#ARENA.stops + 1] = ARENA.my
  return real_stop(p)
end

local function check()
  local A = ARENA
  local i = 0
  for n, step in ipairs(A.want) do
    for k = 2, #step do
      i = i + 1
      local got = A.pops[i]
      if got == nil then
        return false, "missing " .. step[k]
      end
      if got.name ~= step[k] then
        return false, string.format("popup %d is %s, want %s", i, got.name, step[k])
      end
      if n == 1 and got.my ~= 214 then
        return false, "the intro was sent on row " .. got.my
      end
      if step[1] ~= nil and got.my ~= step[1] then
        return false, string.format("%s was sent on row %d, want %d",
                                    step[k], got.my, step[1])
      end
    end
  end
  if #A.pops ~= i then
    return false, string.format("%d popups, want %d", #A.pops, i)
  end
  if #A.stops ~= #A.want - 1 then
    return false, string.format("%d stops, want %d", #A.stops, #A.want - 1)
  end
  for n = 2, #A.want do
    if A.stops[n - 1] ~= A.want[n][1] then
      return false, string.format("stop %d on row %s, want %d", n - 1,
                                  tostring(A.stops[n - 1]), A.want[n][1])
    end
  end
  if #A.bad > 0 then return false, A.bad[1] end
  return true, string.format("%d popups and %d stops on the right rows",
                             #A.pops, #A.stops)
end

local arena_real_tick = on_tick
function on_tick(tick)
  local A = ARENA
  if A.done then return end
  if real_tank(0) == nil then return end
  arena_real_tick(tick)
  if A.my <= 15 then
    A.done = true
    verdict(check())
    return
  end
  -- The first tick sends the intro on the start row; after that the tank
  -- goes one row north a tick.
  A.my = A.my - 1
end
