-- ROOST: "<name> retreat" reaches one bot, and it answers take_cover.
--
-- `retreat` is the only verb with no target: it means cancel whatever you
-- hold and go somewhere safe, and the brain runs it as take_cover with the
-- margin waived. Addressed by name it skips the auction, so the bot named is
-- the bot that answers.
--
-- The ack a bot says carries the REAL goal name out of goals.lua rather than
-- the word that was typed — "Encore! take_cover #" for this line, the empty
-- number being a goal with nothing to number. That is what the round reads,
-- because it is the one place an outside observer can see which goal the
-- verb turned into.
--
-- The name is taken as a prefix, the same way order_by_name takes it, and
-- seat 2 speaks so seats 0 and 1 are the ones who hear it.
--
-- There is no pill and no order before this one. A bot that is already busy
-- answers "Busy (<reason>)" instead, and busy has its own reasons — a man
-- out of the tank, a capture in its last phase — that a round cannot set up
-- from the outside. So this round asks the simpler question: does a named
-- retreat land at all.

scenario = {
  name        = "ROOST order_retreat_busy",
  description = "`<name> retreat` is answered by that bot with take_cover.",
  api         = 1,
}

local SPEAKER = 2
local POS = { [0] = { 116, 128 }, [1] = { 108, 124 }, [2] = { 108, 132 } }

local SETUP_AT = 150
local SAY_AT   = 250
local ACK_BY   = 600

local said_at, prefix = nil, nil
local ackers, n_ackers = {}, 0
local line = nil
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
  if text:find("take_cover", 1, true) and not ackers[p] then
    ackers[p] = true
    n_ackers = n_ackers + 1
    line = text
    game.log(string.format("order_retreat_busy: p%d acked at +%d: %s",
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
  end

  if t == SAY_AT then
    prefix = unique_prefix()
    if not prefix then
      finish("FAIL order_retreat_busy: no prefix of p0's name is its own")
      return
    end
    if not game.say(SPEAKER, "!" .. prefix .. " retreat", "all") then
      finish("FAIL order_retreat_busy: game.say was refused")
      return
    end
    said_at = t
    game.log(string.format("order_retreat_busy: ordered '%s retreat' (p0 is %s)",
                           prefix, game.lobby_slot(0).name))
  end

  if said_at and (t - said_at) >= ACK_BY then
    if n_ackers == 1 and ackers[0] then
      finish(string.format("PASS order_retreat_busy: p0 alone answered: %s", line))
    elseif n_ackers == 0 then
      finish(string.format("FAIL order_retreat_busy: '%s retreat' was answered by nobody",
                           prefix))
    else
      local who = {}
      for p in pairs(ackers) do who[#who + 1] = "p" .. p end
      finish(string.format("FAIL order_retreat_busy: answered by %s",
                           table.concat(who, " ")))
    end
  end
end
