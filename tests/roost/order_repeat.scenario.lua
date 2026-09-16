-- ROOST: the SAME order said twice gets a short confirmation, not a second
-- order.
--
-- A human who says "!attack 0" again while a bot is already on the way used
-- to hear nothing at all: the repeat only refreshed the 60 s focus, quietly.
-- Now the bot holding the order answers "Still on it. attack_pill #0", and
-- no other bot takes the job, because the auction counts the bots that
-- already hold it and finds no slot left to fill.
--
-- So this round reads three things off the chat: the first order is taken by
-- exactly ONE bot, the repeat draws a "Still on it." line within a second,
-- and the count of real acks is still one afterwards. The confirmation line
-- carries the goal and the pill number too, so an ack is counted only when
-- it is NOT a "Still on it." line.
--
-- The arena, the seats and the speaker are order_attack_pill's: a flat grass
-- island, three seated bots, a pill the script adds twelve squares from seat
-- 0, and seat 2 speaking (a bot's line needs a leading "!" to be read as an
-- order, and a sender never hears its own line).

scenario = {
  name        = "ROOST order_repeat",
  description = "The same order said again gets one 'Still on it.' back.",
  api         = 1,
}

local SPEAKER = 2         -- says the line, and never hears it
local PX, PY  = 128, 128  -- the pill
local POS = { [0] = { 116, 128 },   -- 12 squares from the pill
              [1] = { 108, 124 },   -- 20
              [2] = { 108, 132 } }  -- 20, the speaker

local SETUP_AT  = 150
local SAY_AT    = 250
local ACK_BY    = 600     -- ticks after the order for somebody to answer
local REPEAT_IN = 300     -- ticks after that ack before the line is repeated
local STILL_BY  = 60      -- and for the confirmation to come back

local line, ack_txt, said_at = nil, nil, nil
local ackers, n_ackers = {}, 0
local ack_at, repeat_at = nil, nil
local seen = {}
local done = false

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

-- Every chat line reaches the scenario TWICE (the server fans one line down
-- two paths), so a repeat of the same sender and text on the same tick is
-- the same line, not a second one.
local function fresh(p, text)
  local key = p .. "|" .. text .. "|" .. game.tick()
  if seen[key] then return false end
  seen[key] = true
  return true
end

function on_chat(p, text, scripted)
  -- scripted lines are the script's own; "/" lines are the brains' internal
  -- channel, which a human never sees either.
  if scripted or done or text:sub(1, 1) == "/" then return end
  if not fresh(p, text) then return end
  if not said_at then return end

  -- The confirmation names the goal as well, so it is tested FIRST: an ack
  -- is any other line that names the goal.
  if text:sub(1, 12) == "Still on it." then
    if not repeat_at then
      finish("FAIL order_repeat: a confirmation arrived before the repeat")
      return
    end
    local held = 0
    for _ in pairs(ackers) do held = held + 1 end
    if held ~= 1 then
      finish(string.format("FAIL order_repeat: %d bots acked, wanted 1", held))
      return
    end
    finish(string.format("PASS order_repeat: p%d said it +%d after the repeat",
                         p, game.tick() - repeat_at))
    return
  end

  if ack_txt and text:find(ack_txt, 1, true) and not ackers[p] then
    ackers[p] = true
    n_ackers = n_ackers + 1
    ack_at = ack_at or game.tick()
    game.log(string.format("order_repeat: p%d acked at +%d ticks: %s",
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
    local pill_n = game.add_pill(PX, PY, game.NEUTRAL, 15)
    if type(pill_n) ~= "number" then
      finish("FAIL order_repeat: the pill could not be placed")
      return
    end
    -- brain pill numbers are the engine's, counted from zero; the script's
    -- are counted from one.
    line    = "!attack " .. (pill_n - 1)
    ack_txt = "attack_pill #" .. (pill_n - 1)
  end

  if t == SAY_AT then
    if not game.say(SPEAKER, line, "all") then
      finish("FAIL order_repeat: game.say was refused")
      return
    end
    said_at = t
    game.log("order_repeat: ordered " .. line)
  end

  if not said_at then return end

  -- Nobody answered the first order: there is nothing to repeat.
  if not ack_at and (t - said_at) >= ACK_BY then
    finish(string.format("FAIL order_repeat: %d bots acked in %d ticks, wanted 1",
                         n_ackers, ACK_BY))
    return
  end
  if not ack_at then return end

  if n_ackers > 1 then
    finish(string.format("FAIL order_repeat: %d bots took the order", n_ackers))
    return
  end

  if not repeat_at and (t - ack_at) >= REPEAT_IN then
    if not game.say(SPEAKER, line, "all") then
      finish("FAIL order_repeat: the repeat was refused")
      return
    end
    repeat_at = t
    game.log("order_repeat: said again at +" .. (t - ack_at))
    return
  end

  if repeat_at and (t - repeat_at) >= STILL_BY then
    finish(string.format("FAIL order_repeat: no confirmation in %d ticks", STILL_BY))
  end
end
