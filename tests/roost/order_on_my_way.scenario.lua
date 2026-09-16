-- ROOST: a bot that takes an order puts an ON MY WAY marker on the target.
--
-- The chat ack says who took the order. The marker says WHERE, on the map,
-- to everyone on the team at once, and it is the thing a person can see
-- without reading a line of text. It is not the "bot pings" setting: that
-- one covers the ATTACK markers a bot places for goals it chose itself. An
-- order was given by a person, so the answer to it always goes out.
--
-- What is read here: after "!attack 0" one bot acks, and an EVENT_PING of
-- kind ON_MY_WAY from the SAME seat lands within sixty ticks of that ack,
-- pointing at the pill the order named.
--
-- WITHIN, NOT AFTER. The marker and the ack leave on the same think, and the
-- two travel by different roads: the marker is a command the server applies
-- and buffers as an event, the ack is a chat line that waits its turn in the
-- brain's outgoing queue. So the marker usually arrives a tick or two AHEAD
-- of the words. The test keeps the last marker each seat placed and reads it
-- once the ack is in, which is why the order the two arrive in does not
-- matter.
--
-- The arena is the one order_attack_pill uses: a flat island, a pill twelve
-- squares from seat 0 and twenty from seat 1, and seat 2 speaking the line
-- because a headless round has no human in it and a sender never hears its
-- own line.

scenario = {
  name        = "ROOST order_on_my_way",
  description = "Taking an order puts an ON MY WAY marker on the target.",
  api         = 1,
}

local SPEAKER = 2         -- says the line, and never hears it
local PX, PY  = 128, 128  -- the pill
local POS = { [0] = { 116, 128 },   -- 12 squares from the pill
              [1] = { 108, 124 },   -- 20
              [2] = { 108, 132 } }  -- 20, the speaker

local SETUP_AT = 150
local SAY_AT   = 250
local ACK_BY   = 600      -- ticks after the order for somebody to answer
local PING_BY  = 60       -- ticks after that ack for the marker to arrive
local NEAR     = 2        -- squares the marker may sit from the pill

local KIND_ON_MY_WAY = 4

local pill_n, said_at = nil, nil
local acker, acked_at = nil, nil
local omw = {}            -- omw[seat] = the last ON MY WAY marker it placed
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
-- the same line, not a second ack.
local function fresh(p, text)
  local key = p .. "|" .. text .. "|" .. game.tick()
  if seen[key] then return false end
  seen[key] = true
  return true
end

function on_chat(p, text, scripted)
  if scripted or done or text:sub(1, 1) == "/" then return end
  if not fresh(p, text) then return end
  if said_at and not acker and text:find("attack_pill #0", 1, true) then
    acker, acked_at = p, game.tick()
    game.log(string.format("order_on_my_way: p%d acked at +%d ticks: %s",
                           p, acked_at - said_at, text))
  end
end

-- The read-only ping hook. Every smart ping on the map arrives here, a bot's
-- as much as a person's, because a bot places its ping through the same
-- command arm from its own seat.
function on_ping(p, kind, mx, my, scripted)
  if done or scripted then return end
  game.log(string.format("order_on_my_way: ping from p%d kind=%d at (%d,%d) t=%d",
                         p, kind, mx, my, game.tick()))
  if kind ~= KIND_ON_MY_WAY then return end
  omw[p] = { mx = mx, my = my, t = game.tick() }
end

-- The marker this seat placed, if it landed close enough in time to the ack
-- to be the answer to that order rather than something older.
local function marker_for(p)
  local m = omw[p]
  if not m or not acked_at then return nil end
  local gap = m.t - acked_at
  if gap < 0 then gap = -gap end
  if gap > PING_BY then return nil end
  return m
end

function on_tick(t)
  if done then return end

  if t == SETUP_AT then
    for p = 0, 2 do
      game.builder_recall(p)
      game.teleport(p, POS[p][1], POS[p][2])
      game.set_stocks(p, { shells = 40, armour = 40, trees = 20 })
    end
    pill_n = game.add_pill(PX, PY, game.NEUTRAL, 15)
    if type(pill_n) ~= "number" then
      finish("FAIL order_on_my_way: the pill could not be placed")
    end
  end

  if t == SAY_AT then
    -- brain pill numbers are the engine's, counted from zero; the script's
    -- are counted from one.
    if not game.say(SPEAKER, "!attack " .. (pill_n - 1), "all") then
      finish("FAIL order_on_my_way: game.say was refused")
      return
    end
    said_at = t
  end

  if not said_at then return end

  if not acker and (t - said_at) >= ACK_BY then
    finish(string.format("FAIL order_on_my_way: nobody acked in %d ticks", ACK_BY))
    return
  end

  if acker and (t - acked_at) >= PING_BY then
    local m = marker_for(acker)
    if not m then
      finish(string.format(
        "FAIL order_on_my_way: p%d acked but placed no ON MY WAY marker within %d ticks",
        acker, PING_BY))
      return
    end
    local dx, dy = math.abs(m.mx - PX), math.abs(m.my - PY)
    local d = (dx > dy) and dx or dy
    if d > NEAR then
      finish(string.format(
        "FAIL order_on_my_way: p%d marked (%d,%d), %d squares off the pill",
        acker, m.mx, m.my, d))
      return
    end
    finish(string.format(
      "PASS order_on_my_way: p%d marked (%d,%d) at t=%d, ack at t=%d",
      acker, m.mx, m.my, m.t, acked_at))
  end
end
