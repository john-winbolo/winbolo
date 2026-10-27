-- GATE: ticks=3000 bots=1 script=data/maps/tutorials/Classic Tutorial.scenario.lua
--
-- Classic Tutorial: every step's popups are sent, in the old tutorial's
-- order, each on the tick the tank reaches that step's row, and the tank is
-- stopped on each step's row.
--
-- The arena puts a stand-in over game.tank for the tutorial's player, so it
-- can move the tank one row north a tick from row 214 to row 15, with no
-- terrain in the way. game.popup and game.set_speed are wrapped to note
-- what the script sends and on which row; the real calls still go out.
--
-- PASS:
--   - the intro's three popups are sent first, on the first tick;
--   - each later step's popups are sent on the tick the tank is on its row,
--     in the order of the old step table, and nothing else is sent;
--   - every popup is one of the script's English messages, or a part of one
--     right after the part before it, and the server takes it;
--   - game.set_speed(p, 0) is called once on each step's row, and on no other row;
--   - game.tutorial_done is called once, for seat 0, on the last step's row
--     after its popup, and the server takes it.

CLASSIC_TUTORIAL_PLAYER = 0
ARENA = { my = 214, pops = {}, stops = {}, bad = {}, dones = {},
          done = false }

-- The old tutorial's steps (src/bolo/tutorial.c), row then message keys.
ARENA.want = {
  { nil, "start01", "start02", "start04" },
  { 210, "t01" }, { 199, "t02" }, { 192, "t03" },
  { 186, "t04" }, { 181, "t05" }, { 175, "t06" },
  { 166, "t07" }, { 159, "t08", "t09" },
  { 142, "t10", "t11" }, { 122, "t12" },
  { 120, "t13", "t14" }, { 110, "t15" },
  { 103, "t16" }, { 98, "t17" },
  { 84, "t18", "t19" }, { 66, "t20", "t21" },
  { 47, "t22", "t23", "t24" }, { 21, "t25" },
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
-- which of its parts it is, and every input whose wording it is (two inputs
-- may share one text, as t14's keyboard and controller wordings do).
ARENA_TEXT = {}
for key, v in pairs(TEXT.en.pop) do
  local wordings = v
  if type(v) ~= "table" or v.keyboard == nil then wordings = { keyboard = v } end
  for input, w in pairs(wordings) do
    local parts = w
    if type(w) == "string" then parts = { w } end
    for i, part in ipairs(parts) do
      local m = ARENA_TEXT[part]
      if m == nil then
        m = { key = key, part = i, parts = #parts, inputs = {} }
        ARENA_TEXT[part] = m
      end
      m.inputs[input] = true
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
  if p ~= 0 then ARENA.bad[#ARENA.bad + 1] = "popup sent to " .. tostring(p) end
  local last = ARENA.pops[#ARENA.pops]
  if m ~= nil and m.part > 1 then
    if last == nil or last.name ~= m.key then
      ARENA.bad[#ARENA.bad + 1] = "part " .. m.part .. " of " .. m.key
        .. " without the part before it"
    end
  else
    ARENA.pops[#ARENA.pops + 1] = { name = m and m.key or text, my = ARENA.my }
  end
  return ok
end

local real_set_speed = game.set_speed
game.set_speed = function(p, s)
  if s == 0 then ARENA.stops[#ARENA.stops + 1] = ARENA.my end
  return real_set_speed(p, s)
end

ARENA.real_done = game.tutorial_done
game.tutorial_done = function(p)
  local ok, code = ARENA.real_done(p)
  ARENA.dones[#ARENA.dones + 1] = { p = p, ok = ok, code = code,
                                    my = ARENA.my, after = #ARENA.pops }
  return ok, code
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
  local d = A.dones[1]
  if #A.dones ~= 1 or d.p ~= 0 or d.ok ~= true or d.my ~= 21 or
     d.after ~= #A.pops then
    return false, string.format("tutorial_done calls=%d p=%s ok=%s code=%s "
      .. "row=%s after %s of %d popups", #A.dones, tostring(d and d.p),
      tostring(d and d.ok), tostring(d and d.code), tostring(d and d.my),
      tostring(d and d.after), #A.pops)
  end
  return true, string.format("%d popups and %d stops on the right rows; "
                             .. "tutorial_done on row 21", #A.pops, #A.stops)
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
