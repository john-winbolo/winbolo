-- ROOST: an order ends the MOMENT its job is done, not when the clock runs
-- out.
--
-- A bot is told to sweep a dead pill six squares away. It goes, it picks the
-- pill up — and that is the end of the order. It used to keep the slot for
-- the whole 60 s focus, so it sat on a pill it had already swept with nothing
-- left to do there (Andrew, Sep 14). orders.lua now checks the pill every
-- tick and clears the slot on the tick the pill is ours or is in somebody's
-- tank.
--
-- HOW A ROUND CAN SEE A SLOT CLEAR. Brain state is not readable from a
-- sidecar, so the round watches what reaches the outside:
--
--   * THE LINE. Ending the order says the line it has always said,
--     "capture_pill #N done". Only the release path prints it, so the line
--     IS the slot clearing. That is what this round gates on, and it has to
--     arrive within CLEAR_BY ticks of the pill becoming ours.
--   * THE DRIVING. A held order pins a bot to its target, so a bot that is
--     FAR squares off the pill within MOVE_BY ticks of taking it is a bot
--     with nothing holding it there. That is what ends the round, and the
--     distance goes in the verdict. It is corroboration, not the gate: a bot
--     with a pill in its cargo picks its own next goal, and one fair answer
--     to "where do I put this" is "right here".
--
-- The arena and the seating come from order_attack_pill: a flat grass island,
-- seat 2 speaks (a sender never hears its own line), and the order is
-- ADDRESSED BY NAME so there is no auction to lose — seat 0 is the only bot
-- that can answer. Seats 1 and 2 are parked twenty squares out so neither
-- wanders in and takes the pill first.

scenario = {
  name        = "ROOST order_sweep_clears",
  description = "A swept pill ends the order that asked for it, at once.",
  api         = 1,
}

local SPEAKER = 2          -- says the line, and never hears it
local PX, PY  = 128, 128   -- the dead pill
local POS = { [0] = { 122, 128 },   -- 6 squares from the pill
              [1] = { 108, 118 },   -- 20
              [2] = { 108, 138 } }  -- 20, the speaker

local SETUP_AT   = 150
local SAY_AT     = 250
local ACK_BY     = 600     -- ticks after the order for somebody to answer
local CAPTURE_BY = 6000    -- and for the holder to get the pill
local CLEAR_BY   = 60      -- ticks from the capture to the order ending
local MOVE_BY    = 400     -- how long the driving is watched afterwards
local FAR        = 4       -- squares off the pill that say the bot is free

local pill_n, brain_n = nil, nil
local said_at, holder = nil, nil
local n_ackers = 0
local ackers = {}
local captured_at, done_at = nil, nil
local done_line = nil
local furthest = 0
local seen = {}
local done = false

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

-- Chebyshev, which is how far a tank has to drive when it can go diagonally.
local function away(p, x, y)
  local tk = game.tank(p)
  if not tk then return nil end
  local dx, dy = math.abs(tk.mx - x), math.abs(tk.my - y)
  return (dx > dy) and dx or dy
end

-- Every chat line reaches the scenario TWICE (the server fans one line down
-- two paths), so a repeat of the same sender and text on the same tick is the
-- same line, not a second one.
local function fresh(p, text)
  local key = p .. "|" .. text .. "|" .. game.tick()
  if seen[key] then return false end
  seen[key] = true
  return true
end

-- The name the bot is called, shortened until no other seat shares the head
-- of it. The same walk order_retreat_busy does.
local function unique_prefix()
  local mine = game.lobby_slot(0)
  if not mine or not mine.name then return nil end
  local me = mine.name:lower()
  local others = {}
  for p = 1, game.max_tanks() - 1 do
    local s = game.lobby_slot(p)
    if s and s.name then others[#others + 1] = s.name:lower() end
  end
  for k = 3, #me do
    local head = me:sub(1, k)
    local clash = false
    for i = 1, #others do
      if others[i]:sub(1, k) == head then clash = true end
    end
    if not clash then return head end
  end
  return nil
end

function on_chat(p, text, scripted)
  -- scripted lines are the script's own; "/" lines are the brains' internal
  -- channel, which a human never sees either.
  if scripted or done or text:sub(1, 1) == "/" then return end
  if not fresh(p, text) or not said_at or not brain_n then return end

  local ack  = "capture_pill #" .. brain_n
  local fini = ack .. " done"

  if text:find(fini, 1, true) then
    if p == holder and not done_at then
      done_at   = game.tick()
      done_line = text
      game.log(string.format("order_sweep_clears: p%d ended the order at +%d: %s",
                             p, done_at - (captured_at or done_at), text))
    end
    return
  end
  if text:find(ack, 1, true) and not ackers[p] then
    ackers[p] = true
    n_ackers  = n_ackers + 1
    holder    = p
    game.log(string.format("order_sweep_clears: p%d acked at +%d: %s",
                           p, game.tick() - said_at, text))
  end
end

function on_tick(t)
  if done then return end

  if t == SETUP_AT then
    for p = 0, 2 do
      game.builder_recall(p)
      game.teleport(p, POS[p][1], POS[p][2])
      game.set_stocks(p, { shells = 40, armour = 40, trees = 20 })
    end
    -- Nobody's, and dead: armour 0 is a pill that can only be picked up.
    pill_n = game.add_pill(PX, PY, game.NEUTRAL, 0)
    if type(pill_n) ~= "number" then
      finish("FAIL order_sweep_clears: the pill could not be placed")
      return
    end
    -- brain pill numbers are the engine's, counted from zero; the script's
    -- are counted from one.
    brain_n = pill_n - 1
  end

  if t == SAY_AT then
    local prefix = unique_prefix()
    if not prefix then
      finish("FAIL order_sweep_clears: no prefix of p0's name is its own")
      return
    end
    if not game.say(SPEAKER, "!" .. prefix .. " sweep " .. brain_n, "all") then
      finish("FAIL order_sweep_clears: game.say was refused")
      return
    end
    said_at = t
    game.log(string.format("order_sweep_clears: ordered '%s sweep %d', p0 is %s, %d away",
                           prefix, brain_n, game.lobby_slot(0).name,
                           away(0, PX, PY) or -1))
  end

  if not said_at then return end
  local since = t - said_at

  if since == ACK_BY and n_ackers ~= 1 then
    finish(string.format("FAIL order_sweep_clears: %d bots acked in %d ticks, wanted 1",
                         n_ackers, ACK_BY))
    return
  end
  if holder ~= nil and holder ~= 0 then
    finish(string.format("FAIL order_sweep_clears: p%d answered, wanted p0", holder))
    return
  end
  if holder == nil then
    if since >= ACK_BY then
      finish("FAIL order_sweep_clears: nobody answered the order")
    end
    return
  end

  -- THE CAPTURE. A pill in a tank still holds its slot, so both the owner and
  -- the in_tank flag are read: either one means the holder has it.
  if not captured_at then
    local p = game.pill(pill_n)
    if p and p.owner == holder then
      captured_at = t
      game.log(string.format("order_sweep_clears: p%d has the pill at +%d (in_tank=%s)",
                             holder, since, tostring(p.in_tank)))
    elseif since >= CAPTURE_BY then
      finish(string.format("FAIL order_sweep_clears: p%d never took the pill in %d ticks (%d away)",
                           holder, CAPTURE_BY, away(holder, PX, PY) or -1))
    end
    return
  end

  local after = t - captured_at
  local d = away(holder, PX, PY)
  if d and d > furthest then furthest = d end

  if not done_at then
    if after > CLEAR_BY then
      finish(string.format("FAIL order_sweep_clears: p%d still held the order %d ticks after the capture",
                           holder, after))
    end
    return
  end

  -- The verdict is in as soon as the driving has been seen, and at MOVE_BY
  -- whatever the driving did. Ending early matters: this round finishes a
  -- long way inside the round's own life, and a team that owns every pill and
  -- base has WON — which is where a swept pill, placed, leads.
  if furthest >= FAR or after >= MOVE_BY then
    -- game.log and game.end_round both refuse a line of 128 bytes or more,
    -- and a refusal is a Lua error, so the verdict has to stay short.
    finish(string.format("PASS order_sweep_clears: p%d let go %d ticks after the capture, %d squares off by +%d",
                         holder, done_at - captured_at, furthest, after))
  end
end
