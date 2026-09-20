-- ROOST: "nearby attack 0" reaches the bot beside the pill and nobody else.
--
-- `nearby` is the who-word that draws a circle rather than holding an
-- auction: every bot within ORDER_NEARBY_TILES of the TARGET takes the
-- order, and a bot outside that circle answers nothing and carries on with
-- what it was doing. The circle is measured to the target, never to the
-- speaker, so every brain works out the same answer on its own.
--
-- The seats are laid out so the circle can only hold one of them. Seat 0 is
-- six squares from the pill, well inside the ten-square rule; seats 1 and 2
-- are twenty-four squares west of it, and seat 2 is the speaker, so seat 1
-- is the one bot that HEARS the order and has to decline it.
--
-- Two things are read. One bot acked, and it was seat 0 — named by the seat
-- the line came from, which is the same number `game.lobby_slot` knows it
-- by. And seat 1 is still more than fifteen squares out well after the
-- order, so declining meant staying put rather than going anyway.
--
-- Seat 2 is not held to that: it never heard the order (a sender does not
-- receive its own line), so where it drives says nothing about `nearby`.
--
-- A second, DEAD pillbox is put four squares from seat 1 on purpose. A bot
-- with nothing at all to do drifts towards the only pill on the map, and
-- that drift would read as disobedience it never committed; a free pillbox
-- on its doorstep gives it its own work and leaves the reading clean.

scenario = {
  name        = "ROOST order_nearby",
  description = "`nearby` reaches only the bots beside the target.",
  api         = 1,
}

local SPEAKER = 2
local PX, PY  = 128, 128
local DX, DY  = 100, 120  -- a dead pill: seat 1's own business
local POS = { [0] = { 122, 128 },   -- 6 squares from the ordered pill
              [1] = { 104, 120 },   -- 24, beside the dead pill, and hears it
              [2] = { 104, 136 } }  -- 24, the speaker

local SETUP_AT = 150
local SAY_AT   = 250
local ACK_BY   = 600
local HOLD_BY  = 1200     -- how long the far bot has to stay away
local FAR      = 15       -- squares it must still be beyond

local pill_n, said_at = nil, nil
local ackers, n_ackers = {}, 0
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
  if not fresh(p, text) or not said_at then return end
  if text:find("attack_pill #0", 1, true) and not ackers[p] then
    ackers[p] = true
    n_ackers = n_ackers + 1
    game.log(string.format("order_nearby: p%d acked at +%d: %s",
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
    pill_n = game.add_pill(PX, PY, game.NEUTRAL, 15)
    local dead = game.add_pill(DX, DY, game.NEUTRAL, 0)
    if type(pill_n) ~= "number" or type(dead) ~= "number" then
      finish("FAIL order_nearby: the pills could not be placed")
    end
  end

  if t == SAY_AT then
    if not game.say(SPEAKER, "!nearby attack " .. (pill_n - 1), "all") then
      finish("FAIL order_nearby: game.say was refused")
      return
    end
    said_at = t
    game.log(string.format("order_nearby: ordered, p0 %d away, p1 %d away",
                           away(0, PX, PY) or -1, away(1, PX, PY) or -1))
  end

  if not said_at then return end
  local since = t - said_at

  if since == ACK_BY then
    if n_ackers ~= 1 then
      finish(string.format("FAIL order_nearby: %d bots acked in %d ticks, wanted 1",
                           n_ackers, ACK_BY))
      return
    end
    if not ackers[0] then
      local who = "nobody"
      for p in pairs(ackers) do who = "p" .. p end
      finish("FAIL order_nearby: the order went to " .. who .. ", not to p0")
      return
    end
  end

  if since >= HOLD_BY then
    local d0, d1 = away(0, PX, PY), away(1, PX, PY)
    if n_ackers ~= 1 or not ackers[0] then
      finish(string.format("FAIL order_nearby: %d acks by +%d", n_ackers, since))
    elseif d1 and d1 > FAR then
      finish(string.format("PASS order_nearby: p0 alone acked and is %d away, p1 still %d",
                           d0 or -1, d1))
    else
      finish(string.format("FAIL order_nearby: p1 closed to %d, inside %d, at +%d",
                           d1 or -1, FAR, since))
    end
  end
end
