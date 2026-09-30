-- ROOST: "bot chat off" stops the bots TALKING, not the bots WORKING.
--
-- The latch silences the spoken goal confirmations — the ack, the group line,
-- "Still on it.", "holding 10s". Nothing else changes: the order is still
-- heard, still auctioned, still taken, and the bot still drives. So the round
-- is two halves on one map, and the second half only means something because
-- the first one ran:
--
--   A. chat ON.  "attack 0" -> a bot acks "attack_pill #0". This is the
--      control: it proves the seats, the pill and the channel all work, so
--      silence in half B is the latch rather than a starved message slot.
--   B. chat OFF. "attack 1" -> NOT ONE line comes back, and a bot is
--      nevertheless at pill #1's standoff ring by the end.
--
-- Half B orders a DIFFERENT pill on purpose. Re-ordering the same one would
-- take the repeat-ack path, which answers "Still on it." — a different line
-- with a different reason to be silent, and a weaker reading.
--
-- Between the halves the round says "cancel all", so the bot that holds pill
-- #0 is free to bid on pill #1 rather than answering "Busy".
--
-- Seat 2 speaks and never hears its own lines. Arrival is eight squares,
-- attack_pill's standoff ring.

scenario = {
  name        = "ROOST order_bot_chat_off",
  description = "`bot chat off` silences the acks and nothing else.",
  api         = 1,
}

local SPEAKER = 2
local AX, AY  = 128, 120      -- pill #0, half A
local BX, BY  = 128, 136      -- pill #1, half B
local POS = { [0] = { 120, 128 }, [1] = { 116, 128 }, [2] = { 108, 132 } }

local SETUP_AT = 150
local SAY_A    = 450          -- the digest has 300 ticks to share both pills
local ACK_A_BY = 700          -- half A: somebody must ack by here
local OFF_AT   = 1250         -- cancel + "bot chat off"
local SAY_B    = 1450
local ARRIVE_B = 2900         -- inside the 3000-tick order focus of half B
local NEAR     = 8

local pill_a, pill_b = nil, nil
local said_a, said_b = nil, nil
local acked_a = false
local latched = false
local quiet_break = nil       -- the line that broke the silence in half B
local seen = {}
local done = false

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

local function away(p, x, y)
  local tk = game.tank(p)
  if not tk then return nil end
  local dx, dy = math.abs(tk.mx - x), math.abs(tk.my - y)
  return (dx > dy) and dx or dy
end

local function fresh(p, text)
  local key = p .. "|" .. text .. "|" .. game.tick()
  if seen[key] then return false end
  seen[key] = true
  return true
end

function on_chat(p, text, scripted)
  if scripted or done or text:sub(1, 1) == "/" then return end
  if not fresh(p, text) then return end

  if said_a and not said_b then
    if text:find("attack_pill #0", 1, true) then
      acked_a = true
      game.log(string.format("order_bot_chat_off: A p%d acked at +%d: %s",
                             p, game.tick() - said_a, text))
    elseif text:find("Bot chat off", 1, true) then
      latched = true
      game.log("order_bot_chat_off: the latch was confirmed: " .. text)
    end
    return
  end

  -- HALF B: the bots are meant to be silent. Anything at all is the failure,
  -- which is what makes half A's control necessary.
  if said_b and not quiet_break then
    quiet_break = text
    game.log(string.format("order_bot_chat_off: B p%d spoke at +%d: %s",
                           p, game.tick() - said_b, text))
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
    pill_a = game.add_pill(AX, AY, game.NEUTRAL, 15)
    pill_b = game.add_pill(BX, BY, game.NEUTRAL, 15)
    if type(pill_a) ~= "number" or type(pill_b) ~= "number" then
      finish("FAIL order_bot_chat_off: a pill could not be placed")
    end
  end

  if t == SAY_A then
    if not game.say(SPEAKER, "!attack " .. (pill_a - 1), "all") then
      finish("FAIL order_bot_chat_off: game.say was refused")
      return
    end
    said_a = t
  end

  -- THE CONTROL. No ack here and the round proves nothing about half B.
  if said_a and t == SAY_A + ACK_A_BY and not acked_a then
    finish(string.format("FAIL order_bot_chat_off: no ack with chat ON in %d ticks",
                         ACK_A_BY))
    return
  end

  if t == OFF_AT then
    game.say(SPEAKER, "!cancel all", "all")
  end
  if t == OFF_AT + 50 then
    if not game.say(SPEAKER, "!bot chat off", "all") then
      finish("FAIL order_bot_chat_off: game.say was refused")
      return
    end
  end

  if t == SAY_B then
    if not game.say(SPEAKER, "!attack " .. (pill_b - 1), "all") then
      finish("FAIL order_bot_chat_off: game.say was refused")
      return
    end
    said_b = t
    game.log(string.format("order_bot_chat_off: B ordered pill #%d, latch seen=%s",
                           pill_b - 1, tostring(latched)))
  end

  if not said_b then return end
  local since = t - said_b

  if quiet_break then
    finish(string.format("FAIL order_bot_chat_off: a bot spoke with chat off: %s",
                         quiet_break:sub(1, 40)))
    return
  end

  local d0, d1 = away(0, BX, BY), away(1, BX, BY)
  local near = ((d0 and d0 <= NEAR) or (d1 and d1 <= NEAR))
  if near then
    finish(string.format("PASS order_bot_chat_off: silent, p0 %d p1 %d away at +%d",
                         d0 or -1, d1 or -1, since))
  elseif since >= ARRIVE_B then
    finish(string.format("FAIL order_bot_chat_off: silent but p0 %d p1 %d away at +%d",
                         d0 or -1, d1 or -1, since))
  end
end
