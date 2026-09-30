-- ROOST: "attack closest" means the pill closest to WHOEVER SAID IT.
--
-- Every other target word names a number. `closest` names a position, and the
-- position it names is the SENDER's, not the bot's — which is the whole point
-- of the word: a person types it about the pillbox in front of THEM.
--
-- The arena is built so the two readings give different answers. Two pills
-- are placed: pill #0 beside the speaker, out to the west, and pill #1 beside
-- the two bots that hear the line, in the middle of the island. A bot
-- resolving `closest` against its own tank picks #1, which is four squares
-- away; resolving it against the sender picks #0, which is twelve. So the
-- ack — which carries the pill number — says outright which rule ran.
--
-- Only the acks are read. Each bot resolves `closest` on its own, against the
-- pills IT knows, so two bots can open two different orders and each ack its
-- own. Every ack is recorded, keyed by seat, and any ack that names a pill
-- other than the far one fails the test at once. At ACK_BY the test passes if
-- at least one bot acked and every ack named the far pill, and fails if
-- nobody acked.
--
-- Both pills have to be KNOWN before the line is said: a bot that knows no
-- pill answers "no pill to take known", and one that has not seen the speaker
-- answers "can't see you". Either line fails the test. The seats are laid out
-- so each pill is inside somebody's view at setup, and the order waits three
-- hundred ticks for the known-world digest to go round the team.
--
-- Seat 2 speaks and never hears its own line, so seats 0 and 1 are the two
-- bots that can ack.

scenario = {
  name        = "ROOST order_attack_closest",
  description = "`closest` is measured from the sender, not from the bot.",
  api         = 1,
}

local SPEAKER = 2
local FX, FY  = 120, 128      -- pill #0: beside the speaker, far from the bots
local NX, NY  = 132, 128      -- pill #1: beside the bots
local POS = { [0] = { 132, 124 },   -- 4 from the near pill, 12 from the far
              [1] = { 132, 132 },   -- 4 and 12
              [2] = { 118, 128 } }  -- the speaker, 2 from the far pill

local SETUP_AT  = 150
local SAY_AT    = 450         -- the digest has 300 ticks to share both pills
local ACK_BY    = 700

local far_n, near_n, said_at = nil, nil, nil
local acks, n_acks = {}, 0     -- seat -> the pill number its ack named
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

function on_chat(p, text, scripted)
  if scripted or done or text:sub(1, 1) == "/" then return end
  if not fresh(p, text) or not said_at then return end
  -- The ack carries the pill the bot decided on, which is the answer.
  -- THE ACK NAMES THE PILL, so a wrong one is wrong at once.
  local n = text:match("attack_pill #(%d+)")
  if n then
    n = tonumber(n)
    if not acks[p] then n_acks = n_acks + 1 end
    acks[p] = n
    game.log(string.format("order_attack_closest: p%d acked at +%d: %s",
                           p, game.tick() - said_at, text))
    if n ~= (far_n - 1) then
      finish(string.format("FAIL order_attack_closest: p%d took pill #%d, wanted #%d",
                           p, n, far_n - 1))
    end
  elseif text:find("no pill to take known", 1, true)
      or text:find("can't see you", 1, true)
      or text:find("not known yet", 1, true) then
    finish("FAIL order_attack_closest: " .. text)
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
    far_n  = game.add_pill(FX, FY, game.NEUTRAL, 15)
    near_n = game.add_pill(NX, NY, game.NEUTRAL, 15)
    if type(far_n) ~= "number" or type(near_n) ~= "number" then
      finish("FAIL order_attack_closest: a pill could not be placed")
    end
  end

  if t == SAY_AT then
    if not game.say(SPEAKER, "!attack closest", "all") then
      finish("FAIL order_attack_closest: game.say was refused")
      return
    end
    said_at = t
    game.log(string.format("order_attack_closest: ordered; far pill #%d, near #%d",
                           far_n - 1, near_n - 1))
  end

  if not said_at then return end
  local since = t - said_at

  -- A wrong ack has already failed in on_chat, so every ack here named the
  -- far pill.
  if since == ACK_BY then
    if n_acks == 0 then
      finish(string.format("FAIL order_attack_closest: nobody answered in %d ticks",
                           ACK_BY))
    else
      local who = {}
      for p = 0, 2 do
        if acks[p] then who[#who + 1] = "p" .. p end
      end
      finish(string.format("PASS order_attack_closest: %s took pill #%d, no other pill acked in %d ticks",
                           table.concat(who, " and "), far_n - 1, ACK_BY))
    end
  end
end
