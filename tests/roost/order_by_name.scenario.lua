-- ROOST: a bot's name in front of the verb addresses that bot alone.
--
-- A who-word can be a name instead of `all` or `nearby`, and then there is
-- no auction at all: the bot whose name it is takes the order whatever it
-- costs, and every other bot on the team stays silent. This is the form a
-- player uses when the cheapest bot is not the one they want.
--
-- The line is built from a PREFIX of the name rather than the whole of it,
-- because name matching is meant to take one: the round works out the
-- shortest prefix of seat 0's name that no other bot's name shares, three
-- letters at the least, and orders that. So the round proves the addressing
-- and the prefix matching in the same line.
--
-- Seat 0 is the far bot here, twenty squares out, and seat 1 is the near one
-- at twelve. That is on purpose: with no name in the line the auction would
-- give the job to seat 1 (order_attack_pill is that round), so an ack from
-- seat 0 and silence from seat 1 can only be the name doing the work.
--
-- Seat 2 speaks, and a sender never receives its own line.

scenario = {
  name        = "ROOST order_by_name",
  description = "A bot name, given as a prefix, addresses one bot alone.",
  api         = 1,
}

local SPEAKER = 2
local PX, PY  = 128, 128
local POS = { [0] = { 108, 124 },   -- 20 squares out: the auction's loser
              [1] = { 116, 128 },   -- 12: the auction's winner, told nothing
              [2] = { 108, 132 } }

local SETUP_AT = 150
local SAY_AT   = 250
local ACK_BY   = 600

local pill_n, said_at, prefix = nil, nil, nil
local ackers, n_ackers = {}, 0
local seen = {}
local done = false

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

local function fresh(p, text)
  local key = p .. "|" .. text .. "|" .. game.tick()
  if seen[key] then return false end
  seen[key] = true
  return true
end

-- The shortest prefix of seat 0's name, three letters at the least, that no
-- other seated bot's name begins with.
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
  if scripted or done or text:sub(1, 1) == "/" then return end
  if not fresh(p, text) or not said_at then return end
  if text:find("attack_pill #0", 1, true) and not ackers[p] then
    ackers[p] = true
    n_ackers = n_ackers + 1
    game.log(string.format("order_by_name: p%d acked at +%d: %s",
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
      finish("FAIL order_by_name: the pill could not be placed")
    end
  end

  if t == SAY_AT then
    prefix = unique_prefix()
    if not prefix then
      finish("FAIL order_by_name: no prefix of p0's name is its own")
      return
    end
    if not game.say(SPEAKER, "!" .. prefix .. " attack " .. (pill_n - 1), "all") then
      finish("FAIL order_by_name: game.say was refused")
      return
    end
    said_at = t
    game.log(string.format("order_by_name: ordered '%s' (p0 is %s)",
                           prefix, game.lobby_slot(0).name))
  end

  if said_at and (t - said_at) >= ACK_BY then
    if n_ackers == 1 and ackers[0] then
      finish(string.format("PASS order_by_name: '%s' was answered by p0 alone", prefix))
    elseif n_ackers == 0 then
      finish(string.format("FAIL order_by_name: '%s' was answered by nobody", prefix))
    else
      local who = {}
      for p in pairs(ackers) do who[#who + 1] = "p" .. p end
      finish(string.format("FAIL order_by_name: '%s' was answered by %s",
                           prefix, table.concat(who, " ")))
    end
  end
end
