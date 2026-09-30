-- ROOST: a dead pill removed before a bot picks it up is left alone.
--
-- A dead pill is picked up by a builder. The bot drives near it and sends
-- the builder out. If the pill is removed while the bot is on its way, the
-- bot has to drop the plan: there is nothing on the square to pick up.
--
-- A dead neutral pill sits at (128,128). Seat 2 is parked ten squares from
-- it, so the team knows it, and says "!attack 0", which on a dead pill is a
-- pick-up (capture_pill). Seats 0 and 1 hear it from twenty-two and
-- twenty-six squares out. The pill is removed on the tick the ack is heard.
--
-- Two things are read:
--   * the holder says "order lapsed" inside LAPSE_BY ticks;
--   * in the WATCH ticks after the removal no builder is sent to the
--     removed pill's square, and no tank drives onto it.

scenario = {
  name        = "ROOST removed_dead_pill_pickup",
  description = "A dead pill removed before pick-up is dropped from every plan.",
  api         = 1,
}

local SPEAKER = 2
local PX, PY  = 128, 128
local POS = { [0] = { 150, 128 },
              [1] = { 154, 120 },
              [2] = { 138, 128 } }

local SETUP_AT = 150
local SAY_AT   = 400
local ACK_BY   = 600
local LAPSE_BY = 400
local WATCH    = 1500

local pill_n, said_at, gone_at = nil, nil, nil
local holder, lapsed_at = nil, nil
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
  game.log(string.format("removed_dead_pill_pickup: t=%d p%d says: %s", game.tick(), p, text))
  if said_at and not holder and text:find("capture_pill #0", 1, true) then
    holder = p
    game.remove_pill(pill_n)
    gone_at = game.tick()
    game.log(string.format("removed_dead_pill_pickup: p%d acked; pill removed at t=%d, holder %d away",
                           p, gone_at, away(p, PX, PY) or -1))
  end
  if gone_at and p == holder and not lapsed_at and text:find("order lapsed", 1, true) then
    lapsed_at = game.tick()
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
    pill_n = game.add_pill(PX, PY, game.NEUTRAL, 0)
    if type(pill_n) ~= "number" then
      finish("FAIL removed_dead_pill_pickup: the pill could not be placed")
    end
    return
  end

  if t == SAY_AT then
    if not game.say(SPEAKER, "!attack " .. (pill_n - 1), "all") then
      finish("FAIL removed_dead_pill_pickup: game.say was refused")
      return
    end
    said_at = t
  end

  if said_at and not gone_at and t - said_at >= ACK_BY then
    finish("FAIL removed_dead_pill_pickup: nobody took the order")
    return
  end
  if not gone_at then return end

  local since = t - gone_at
  if game.pill(pill_n) ~= nil then
    finish("FAIL removed_dead_pill_pickup: game.pill still answers after the removal")
    return
  end
  for q = 0, 2 do
    if away(q, PX, PY) == 0 then
      finish(string.format("FAIL removed_dead_pill_pickup: p%d drove onto the square at +%d", q, since))
      return
    end
    local b = game.builder(q)
    if b and b.state ~= "in_tank" and b.mx == PX and b.my == PY then
      finish(string.format("FAIL removed_dead_pill_pickup: p%d builder on the square at +%d", q, since))
      return
    end
  end
  if since >= LAPSE_BY and not lapsed_at then
    finish(string.format("FAIL removed_dead_pill_pickup: p%d never said order lapsed; %d away at +%d",
                         holder, away(holder, PX, PY) or -1, since))
    return
  end
  if since >= WATCH then
    finish(string.format("PASS removed_dead_pill_pickup: p%d lapsed at +%d, square left alone %d ticks",
                         holder, (lapsed_at or t) - gone_at, WATCH))
  end
end
