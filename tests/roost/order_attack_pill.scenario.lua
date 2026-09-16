-- ROOST: a chat order picks ONE bot, and that bot goes for the pill.
--
-- "attack 0" with no who-word is the plainest order there is: one bot takes
-- it, the rest stand off. Every bot on the team hears the same line, prices
-- its own trip on its Dijkstra slate, and the cheapest bid wins; nothing on
-- the server ranks them. So the two things worth reading are how many bots
-- answered — exactly one — and whether the one that answered went.
--
-- The speaker is a seated BOT, because a headless round has no human in it.
-- A bot's line needs a leading `!` to be read as an order at all (orders.lua
-- M.on_chat drops an unforced line from a bot), and a sender never receives
-- its own line, so seat 2 speaks and seats 0 and 1 are the only candidates.
--
-- The arena is flat: a 64-square grass island with two neutral bases well
-- away to the west, no map pills, and every tank put on a named square with
-- full stocks so no refuel pause can be mistaken for disobedience. The pill
-- the order names is added by the script, twelve squares from seat 0 and
-- twenty from seat 1.
--
-- ARRIVAL is read at eight squares, not four. attack_pill is a SHOOTING
-- goal: it drives to a standoff ring and fires from there, and in this arena
-- the ring settles at five to seven squares. Four squares would be asking
-- for a behaviour the goal does not have. Eight is still unmistakable next
-- to the twenty the holder started at.

scenario = {
  name        = "ROOST order_attack_pill",
  description = "An unaddressed chat order is taken by exactly one bot.",
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
local ARRIVE_BY = 1500    -- and for the one that answered to get there
local NEAR     = 8        -- squares: attack_pill's standoff ring

local pill_n, said_at = nil, nil
local ackers, ack_line = {}, nil
local n_ackers = 0
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
-- two paths), so a repeat of the same sender and text on the same tick is
-- the same line, not a second ack.
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
  if said_at and text:find("attack_pill #0", 1, true) and not ackers[p] then
    ackers[p] = true
    n_ackers = n_ackers + 1
    ack_line = text
    game.log(string.format("order_attack_pill: p%d acked at +%d ticks: %s",
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
    if type(pill_n) ~= "number" then
      finish("FAIL order_attack_pill: the pill could not be placed")
    end
  end

  if t == SAY_AT then
    local d0, d1 = away(0, PX, PY), away(1, PX, PY)
    -- brain pill numbers are the engine's, counted from zero; the script's
    -- are counted from one.
    local ok = game.say(SPEAKER, "!attack " .. (pill_n - 1), "all")
    if not ok then
      finish("FAIL order_attack_pill: game.say was refused")
      return
    end
    said_at = t
    game.log(string.format("order_attack_pill: ordered, p0 %d away, p1 %d away",
                           d0 or -1, d1 or -1))
  end

  if not said_at then return end
  local since = t - said_at

  if since == ACK_BY and n_ackers ~= 1 then
    finish(string.format("FAIL order_attack_pill: %d bots acked in %d ticks, wanted 1",
                         n_ackers, ACK_BY))
    return
  end

  if since >= ACK_BY and n_ackers == 1 then
    local holder = nil
    for p in pairs(ackers) do holder = p end
    local d = away(holder, PX, PY)
    if d and d <= NEAR then
      finish(string.format("PASS order_attack_pill: p%d alone acked, %d away at +%d",
                           holder, d, since))
    elseif since >= ARRIVE_BY then
      finish(string.format("FAIL order_attack_pill: p%d acked but is %d away at +%d",
                           holder, d or -1, since))
    end
  elseif since > ACK_BY and n_ackers ~= 1 then
    finish(string.format("FAIL order_attack_pill: %d bots hold the order", n_ackers))
  end
end
