-- GATE: ticks=3000 bots=1 script=data/maps/tutorials/Classic Tutorial.scenario.lua
--
-- Classic Tutorial: a message with a touch or a controller wording is sent
-- under that wording's name for a player on that input.
--
-- The arena drives the tank north as classic_tutorial_steps does, and puts
-- a stand-in over game.input for the tutorial's player: "touch" from the
-- start row down to row 160, "controller" north of it. game.popup is
-- wrapped to note the name of each popup the script sends.
--
-- PASS:
--   - each popup is sent under the name for the input of the moment: the
--     _TOUCH or _CTRL name where the message has one, and the keyboard
--     name where it has none (STR_TUTORIAL_START01 has no controller
--     wording, and most messages have neither);
--   - the server takes every popup;
--   - the touch half covers STR_TUTORIAL_START01, STR_TUTORIAL_START04 and
--     STR_TUTORIAL01 to 06, and the controller half STR_TUTORIAL10, 14, 16,
--     18, 19 and 21.

CLASSIC_TUTORIAL_PLAYER = 0
ARENA = { my = 214, pops = {}, bad = {}, done = false }

-- The messages the arena expects with an input wording, written out here
-- rather than read from the script.
ARENA.touch = {
  STR_TUTORIAL_START01 = true, STR_TUTORIAL_START04 = true,
  STR_TUTORIAL01 = true, STR_TUTORIAL02 = true, STR_TUTORIAL03 = true,
  STR_TUTORIAL04 = true, STR_TUTORIAL05 = true, STR_TUTORIAL06 = true,
  STR_TUTORIAL10 = true, STR_TUTORIAL14 = true, STR_TUTORIAL16 = true,
  STR_TUTORIAL18 = true, STR_TUTORIAL19 = true, STR_TUTORIAL21 = true,
}
ARENA.ctrl = {
  STR_TUTORIAL_START04 = true,
  STR_TUTORIAL01 = true, STR_TUTORIAL02 = true, STR_TUTORIAL03 = true,
  STR_TUTORIAL04 = true, STR_TUTORIAL05 = true, STR_TUTORIAL06 = true,
  STR_TUTORIAL10 = true, STR_TUTORIAL14 = true, STR_TUTORIAL16 = true,
  STR_TUTORIAL18 = true, STR_TUTORIAL19 = true, STR_TUTORIAL21 = true,
}

-- Every message the tutorial sends, in order (as in classic_tutorial_steps).
ARENA.want = {
  "STR_TUTORIAL_START01", "STR_TUTORIAL_START04",
  "STR_TUTORIAL01", "STR_TUTORIAL02", "STR_TUTORIAL03", "STR_TUTORIAL04",
  "STR_TUTORIAL05", "STR_TUTORIAL06", "STR_TUTORIAL07", "STR_TUTORIAL08",
  "STR_TUTORIAL09", "STR_TUTORIAL10", "STR_TUTORIAL11", "STR_TUTORIAL12",
  "STR_TUTORIAL13", "STR_TUTORIAL14", "STR_TUTORIAL15", "STR_TUTORIAL16",
  "STR_TUTORIAL17", "STR_TUTORIAL18", "STR_TUTORIAL19", "STR_TUTORIAL20",
  "STR_TUTORIAL21", "STR_TUTORIAL22", "STR_TUTORIAL23", "STR_TUTORIAL24",
  "STR_TUTORIAL25", "STR_TUTORIAL_START02",
}

local function input_now()
  if ARENA.my >= 160 then return "touch" end
  return "controller"
end

local real_tank = game.tank
game.tank = function(p)
  local t = real_tank(p)
  if p == 0 and t ~= nil then
    t.my = ARENA.my
    t.dead = false
  end
  return t
end

local real_input = game.input
game.input = function(p)
  if p == 0 then return input_now() end
  return real_input(p)
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
  ARENA.pops[#ARENA.pops + 1] = { name = name or text, input = input_now() }
  return ok
end

local function check()
  local A = ARENA
  local touched, ctrled = 0, 0
  if #A.bad > 0 then return false, A.bad[1] end
  if #A.pops ~= #A.want then
    return false, string.format("%d popups, want %d", #A.pops, #A.want)
  end
  for i, base in ipairs(A.want) do
    local got = A.pops[i]
    local want = base
    if got.input == "touch" and A.touch[base] then
      want = base .. "_TOUCH"
      touched = touched + 1
    elseif got.input == "controller" and A.ctrl[base] then
      want = base .. "_CTRL"
      ctrled = ctrled + 1
    end
    if got.name ~= want then
      return false, string.format("popup %d on %s is %s, want %s", i,
                                  got.input, got.name, want)
    end
  end
  if touched ~= 8 or ctrled ~= 6 then
    return false, string.format("%d touch and %d controller wordings, "
                                .. "want 8 and 6", touched, ctrled)
  end
  return true, string.format("%d popups; %d touch and %d controller wordings",
                             #A.pops, touched, ctrled)
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
  A.my = A.my - 1
end
