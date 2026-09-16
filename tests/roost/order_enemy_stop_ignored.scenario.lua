-- ROOST: an operator command from the other side is not a command.
--
-- "!stop" and "!start" are the hardest things anybody can say to a
-- GoalHunter bot. They hand the brain a paused or running state ahead of
-- goal selection: no flee, no refuel, no survival exception, no key held at
-- all while it is paused. Every other reader on the brain's chat path checks
-- the SENDER's team before it acts, and the call that parses these did not --
-- it sat outside the ally test in init.lua's inbox loop.
--
-- WHAT THIS ROUND DOES AND DOES NOT PROVE, honestly. Two layers have to fail
-- before an enemy's "!stop" can reach a bot, and only one of them was open.
-- The engine keeps an enemy's chat out of a bot's inbox already: the same
-- seat saying the same word freezes this bot when `-allybots 1` puts it on
-- our team (0 turns) and does not when `-teams 2,1` puts it on the other one
-- (137 turns), measured on this arena. So the round passed before the Lua
-- fix as well as after it, and it is here as a guard on BOTH layers rather
-- than as a reproduction of the bug -- if the engine ever starts delivering
-- enemy chat to brains, this round is what catches the Lua side going with
-- it. The Lua half has its own unit test in test_orders.lua, which does fail
-- before the fix.
--
-- THE SIGNAL IS FACING, as it is in say_stop_halts_bot: a Bolo tank handed
-- no key keeps the speed it had and stops TURNING, so a paused bot coasts in
-- a dead straight line. Position says much less, because a coasting tank
-- still moves -- measured here, a frozen bot still crosses sixteen squares
-- in ten seconds.
--
-- THE ROUND STARTS BY FREEZING THE BOT, which is what makes the middle phase
-- readable. Asking whether an enemy's "!stop" stopped a bot means reading a
-- bot that is still driving, and a driving bot turns for all sorts of
-- reasons; asking whether an enemy's "!start" STARTED one means reading a bot
-- that is standing still, and a bot standing still has one reason to begin
-- turning again. So:
--
--   1. AN ALLY STOPS IT. Seat 1 says "!stop" to everyone. Seat 0 has to stop
--      turning altogether -- the baseline, and the proof the line path works
--      in this round at all.
--   2. THE ENEMY TRIES TO START IT. Seat 2, on team 2, says "!start" to
--      everyone. Seat 0 has to stay as still as it was. Seat 1 is watched
--      over the same window as a control, so "nobody was moving" can never be
--      the reason.
--   3. AN ALLY STARTS IT. Seat 1 says the same word in the same way, and
--      seat 0 has to drive again. Without this the round proves nothing: a
--      bot that would have stayed frozen whoever spoke looks the same.
--
-- Measured on this arena: frozen 0 turns, the enemy's line 1, the ally's 41,
-- and the control 202. The threshold sits at ten, well clear of both ends --
-- exactly zero would be reading noise, since a paused bot still turns its
-- gun on something that drives into range.
--
-- Seat 2 is a prop, not an opponent. It is parked in a far corner and
-- emptied of shells every tick, so it can neither reach nor shoot the bots
-- it is talking to and the only thing that can reach them is its chat line.

scenario = {
  name        = "ROOST order_enemy_stop_ignored",
  description = "An enemy's !start does not restart a paused bot; an ally's does.",
  api         = 1,
}

local LISTENER = 0        -- team 1: the bot every line is aimed at
local ALLY     = 1        -- team 1: stops it, and starts it again at the end
local ENEMY    = 2        -- team 2: tries to start it in between
local FX, FY   = 150, 150 -- the corner the enemy is kept in

local SETUP_AT   = 150    -- stocks, builders
local STOP_AT    = 250    -- the ally's "!stop"
local FROZE_FROM = 650    -- the "it really is frozen" window, 8 s after
local FROZE_TO   = 1150   -- and 10 s wide
local ENEMY_AT   = 1200   -- the enemy's "!start"
local ENEMY_FROM = 1600   -- the "it stayed frozen" window, 8 s after
local ENEMY_TO   = 2600   -- and 20 s wide
local ALLY_AT    = 2700   -- the ally's "!start"
local ALLY_FROM  = 2800   -- the "it drives again" window, 2 s after
local ALLY_TO    = 4000   -- and 24 s wide

-- How much turning proves a bot is driving itself, and how little proves it
-- is not. The control is the ally, which never hears its own lines.
local MIN_TURNS  = 10
local STILL_MAX  = 3

local froze_turns, enemy_turns, ally_turns, control_turns = 0, 0, 0, 0
local last_froze, last_enemy, last_ally, last_control = nil, nil, nil, nil
local said_stop, said_enemy, said_ally = false, false, false
local judged_froze, judged_enemy = false, false
local done = false

local function finish(text)
  if done then return end
  done = true
  -- game.log is the only call whose text reaches the server's console.
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

-- One sample of a seat's facing. A dead tank is skipped rather than counted:
-- a respawn moves the tank and the facing with it, and that is not a turn the
-- bot chose to make.
local function sample(p, last, n)
  local tk = game.tank(p)
  if not tk or tk.dead then return last, n end
  if last and tk.dir ~= last then n = n + 1 end
  return tk.dir, n
end

local function say(p, line, who)
  local ok, code, detail = game.say(p, line, "all")
  if not ok then
    finish(string.format("FAIL order_enemy_stop_ignored: %s's %s refused %s (%s)",
                         who, line, tostring(code), tostring(detail)))
    return false
  end
  return true
end

function on_start()
  for p = 0, 2 do
    local s = game.lobby_slot(p)
    if not s then
      finish("FAIL order_enemy_stop_ignored: the round needs three seated bots")
      return
    end
    game.log(string.format("order_enemy_stop_ignored: p%d %s on team %s",
                           p, tostring(s.name), tostring(s.team)))
  end
end

function on_tick(t)
  if done then return end

  if t == SETUP_AT then
    for p = 0, 2 do
      game.builder_recall(p)
      game.set_stocks(p, { shells = 40, armour = 40, trees = 20 })
    end
  end

  -- The enemy is a chat seat and nothing else: kept out of reach and out of
  -- ammunition, so nothing it does but talk can touch the other two.
  if t > SETUP_AT then
    game.teleport(ENEMY, FX, FY)
    game.set_stocks(ENEMY, { shells = 0 })
  end

  -- ── 1. AN ALLY STOPS IT ───────────────────────────────────────────────
  if not said_stop and t >= STOP_AT then
    said_stop = true
    if not say(ALLY, "!stop", "the ally") then return end
  end

  if t >= FROZE_FROM and t < FROZE_TO then
    last_froze, froze_turns = sample(LISTENER, last_froze, froze_turns)
  end

  if not judged_froze and t >= FROZE_TO then
    judged_froze = true
    if froze_turns > STILL_MAX then
      finish(string.format(
        "FAIL order_enemy_stop_ignored: the ally's !stop left %d turns, so the "
        .. "round has no baseline", froze_turns))
      return
    end
    game.log(string.format("order_enemy_stop_ignored: the ally's !stop froze it, "
                           .. "%d turns", froze_turns))
  end

  -- ── 2. THE ENEMY TRIES TO START IT ────────────────────────────────────
  if not said_enemy and t >= ENEMY_AT then
    said_enemy = true
    if not say(ENEMY, "!start", "the enemy") then return end
  end

  if t >= ENEMY_FROM and t < ENEMY_TO then
    last_enemy, enemy_turns = sample(LISTENER, last_enemy, enemy_turns)
    last_control, control_turns = sample(ALLY, last_control, control_turns)
  end

  if not judged_enemy and t >= ENEMY_TO then
    judged_enemy = true
    if control_turns < MIN_TURNS then
      finish(string.format(
        "FAIL order_enemy_stop_ignored: the control turned %d times, under %d",
        control_turns, MIN_TURNS))
      return
    end
    if enemy_turns > STILL_MAX then
      finish(string.format(
        "FAIL order_enemy_stop_ignored: the enemy's !start woke it, %d turns "
        .. "(control %d)", enemy_turns, control_turns))
      return
    end
    game.log(string.format("order_enemy_stop_ignored: ignored the enemy, "
                           .. "%d turns, control %d", enemy_turns, control_turns))
  end

  -- ── 3. AN ALLY STARTS IT ──────────────────────────────────────────────
  if not said_ally and t >= ALLY_AT then
    said_ally = true
    if not say(ALLY, "!start", "the ally") then return end
  end

  if t >= ALLY_FROM and t < ALLY_TO then
    last_ally, ally_turns = sample(LISTENER, last_ally, ally_turns)
  end

  if t >= ALLY_TO then
    if ally_turns < MIN_TURNS then
      finish(string.format(
        "FAIL order_enemy_stop_ignored: the ally's !start left it frozen too, "
        .. "%d turns, so the round proves nothing", ally_turns))
    else
      finish(string.format(
        "PASS order_enemy_stop_ignored: frozen %d, enemy %d (control %d), ally %d",
        froze_turns, enemy_turns, control_turns, ally_turns))
    end
  end
end
