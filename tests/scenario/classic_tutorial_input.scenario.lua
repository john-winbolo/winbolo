-- GATE: ticks=3000 bots=1 script=data/maps/tutorials/Classic Tutorial.scenario.lua
--
-- Classic Tutorial: a message with a touch or a controller wording is sent
-- in that wording to a player on that input.
--
-- The arena drives the tank north as classic_tutorial_steps does, and puts
-- a stand-in over game.input for the tutorial's player: "touch" from the
-- start row down to row 160, "controller" north of it. game.popup is
-- wrapped to note which message, and which wording of it, each popup is.
--
-- PASS:
--   - each message is sent in the wording for the input of the moment: the
--     touch or controller wording where the message has one, and the
--     keyboard wording where it has none (start01 has no controller
--     wording, and most messages have neither);
--   - the server takes every popup;
--   - the touch half covers start01, start04 and t01 to t06, and the
--     controller half t10, t14, t16, t18, t19 and t21.

CLASSIC_TUTORIAL_PLAYER = 0
ARENA = { my = 214, pops = {}, bad = {}, done = false }

-- The messages the arena expects with an input wording, written out here
-- rather than read from the script.
ARENA.touch = {
  start01 = true, start04 = true,
  t01 = true, t02 = true, t03 = true,
  t04 = true, t05 = true, t06 = true,
  t10 = true, t14 = true, t16 = true,
  t18 = true, t19 = true, t21 = true,
}
ARENA.ctrl = {
  start04 = true,
  t01 = true, t02 = true, t03 = true,
  t04 = true, t05 = true, t06 = true,
  t10 = true, t14 = true, t16 = true,
  t18 = true, t19 = true, t21 = true,
}

-- Every message the tutorial sends, in order (as in classic_tutorial_steps).
ARENA.want = {
  "start01", "start02", "start04",
  "t01", "t02", "t03", "t04",
  "t05", "t06", "t07", "t08",
  "t09", "t10", "t11", "t12",
  "t13", "t14", "t15", "t16",
  "t17", "t18", "t19", "t20",
  "t21", "t22", "t23", "t24",
  "t25",
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
  local ok = real_popup(text, p, ...)
  local m = ARENA_TEXT[text]
  if m == nil then
    ARENA.bad[#ARENA.bad + 1] = "popup is not a message: " .. string.sub(text, 1, 40)
  elseif not ok then
    ARENA.bad[#ARENA.bad + 1] = "popup refused: " .. m.key
  end
  if m == nil or m.part == 1 then
    ARENA.pops[#ARENA.pops + 1] = { name = m and m.key or text,
                                    wording = m and m.input,
                                    input = input_now() }
  end
  return ok
end

local function check()
  local A = ARENA
  local touched, ctrled = 0, 0
  if #A.bad > 0 then return false, A.bad[1] end
  if #A.pops ~= #A.want then
    return false, string.format("%d popups, want %d", #A.pops, #A.want)
  end
  for i, key in ipairs(A.want) do
    local got = A.pops[i]
    local want = "keyboard"
    if got.input == "touch" and A.touch[key] then
      want = "touch"
      touched = touched + 1
    elseif got.input == "controller" and A.ctrl[key] then
      want = "controller"
      ctrled = ctrled + 1
    end
    if got.name ~= key or got.wording ~= want then
      return false, string.format("popup %d on %s is %s in the %s wording, "
        .. "want %s in the %s wording", i, got.input, tostring(got.name),
        tostring(got.wording), key, want)
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
