-- ROOST: `last` names the bot that took the sender's previous order.
--
-- The who-word `last` stands for whoever took the line you gave before it.
-- It is the shortcut for "them again": you do not have to remember or retype
-- the name. Every bot works the set out on its own, from two things it
-- already has — the order id it saw that sender give, and the claims the
-- takers broadcast — so no new traffic carries it and all the bots agree.
--
-- The round proves the set is exactly ONE bot, not the team. Seat 0 is
-- ordered by a prefix of its name, so seat 0 alone holds an order. Then
-- `!last cancel` goes out, and the only bot that may answer it is seat 0.
--
-- `Released` is the proof, and it is the same proof order_cancel uses: the
-- only code in orders.lua that says that word is release_held, which is also
-- the only code that empties the order slot. A bot cannot say it without
-- having let the order go.
--
-- The round FAILS if a second bot says it, because that is `last` reading as
-- `all`. It also fails if nobody says it, because that is `last` finding
-- nothing to point at.
--
-- The cancel is a plain `!last cancel` with no name in it: a name would make
-- the line an ordinary `cancel <bot>` and prove nothing about `last`.
--
-- Seat 2 speaks, and a sender never receives its own line, so seat 2 has no
-- history of its own order. That is harmless here: the "no previous order"
-- answer is only ever said by the SPEAKING bot, which is the lowest bot
-- player number on the team, and that is seat 0.

scenario = {
  name        = "ROOST order_last",
  description = "`last cancel` releases the bot that took the last order, and only it.",
  api         = 1,
}

local SPEAKER = 2
local PX, PY  = 128, 128
local POS = { [0] = { 108, 124 },   -- 20 squares out: the bot we name
              [1] = { 116, 128 },   -- 12 squares: the one the auction would pick
              [2] = { 108, 132 } }

local SETUP_AT = 150
local SAY_AT   = 250
local ACK_BY   = 400              -- ticks allowed for the named bot to ack
local WAIT_FOR = 600             -- ticks allowed after the cancel

local pill_n, said_at, cancel_at, prefix = nil, nil, nil, nil
local ackers, n_ackers = {}, 0
local releasers, n_releasers = {}, 0
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

local function who(t)
  local out = {}
  for p in pairs(t) do out[#out + 1] = "p" .. p end
  table.sort(out)
  return table.concat(out, " ")
end

-- The shortest prefix of seat 0's name, three letters at the least, that no
-- other seated bot's name begins with.  Name matching is meant to take one,
-- and order_by_name proves the same thing about the ordering line.
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
  if not cancel_at then
    if text:find("attack_pill #0", 1, true) and not ackers[p] then
      ackers[p] = true
      n_ackers = n_ackers + 1
      game.log(string.format("order_last: p%d acked at +%d: %s",
                             p, game.tick() - said_at, text))
    end
  elseif text:lower():find("released", 1, true) and not releasers[p] then
    releasers[p] = true
    n_releasers = n_releasers + 1
    game.log(string.format("order_last: p%d released at +%d: %s",
                           p, game.tick() - cancel_at, text))
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
      finish("FAIL order_last: the pill could not be placed")
    end
  end

  if t == SAY_AT then
    prefix = unique_prefix()
    if not prefix then
      finish("FAIL order_last: no prefix of p0's name is its own")
      return
    end
    if not game.say(SPEAKER, "!" .. prefix .. " attack " .. (pill_n - 1), "all") then
      finish("FAIL order_last: game.say was refused")
      return
    end
    said_at = t
    game.log(string.format("order_last: ordered '%s' (p0 is %s)",
                           prefix, game.lobby_slot(0).name))
  end

  -- The named bot has to be holding the order before `last` can mean it.
  if said_at and not cancel_at and (t - said_at) >= ACK_BY then
    if n_ackers ~= 1 or not ackers[0] then
      finish(string.format("FAIL order_last: the named order was answered by %s, want p0 alone",
                           n_ackers == 0 and "nobody" or who(ackers)))
      return
    end
    if not game.say(SPEAKER, "!last cancel", "all") then
      finish("FAIL order_last: the cancel was refused")
      return
    end
    cancel_at = t
    game.log("order_last: p0 holds it; said '!last cancel'")
  end

  if cancel_at and (t - cancel_at) >= WAIT_FOR then
    if n_releasers == 1 and releasers[0] then
      finish("PASS order_last: '!last cancel' released p0, and p0 alone")
    elseif n_releasers == 0 then
      finish("FAIL order_last: nobody answered '!last cancel'")
    else
      finish(string.format("FAIL order_last: '!last cancel' released %s, want p0 alone",
                           who(releasers)))
    end
  end
end
