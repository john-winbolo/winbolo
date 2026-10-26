-- =========================================================================
-- Classic Tutorial — the tutorial WinBolo has always had, as a scenario.
--
-- This is the old built-in tutorial (Inbuilt Tutorial.map and the step
-- table that was in src/bolo/tutorial.c), played the same way: the player
-- drives north up the map, and each time the tank reaches one of the rows
-- below, the game stops the tank and shows that row's messages. The
-- messages are the old ones, word for word, in every language the game
-- ships, and in the wording for keyboard, controller or touch.
--
-- WHY "CLASSIC". The new Tutorial (data/maps/tutorials/Tutorial.map) is the
-- one the main menu starts. This file keeps the old one for players who know
-- it, under a name that tells the two apart.
--
-- HOW THE TEXT GETS TO THE PLAYER. Each popup is sent as "{LANG:STR_...}",
-- the name of the message in the game's language files. The player's client
-- shows that string in its own language and picks the touch or controller
-- wording itself (see "Text in the player's language" in
-- docs/SCENARIO_API.md). So this file holds no text of its own to translate.
--
-- WHAT IT DOES, STEP BY STEP (the same as the old tutorial):
--   - The intro (two messages) opens as soon as the player's tank is on
--     the map.
--   - Each later step fires when the tank is on that step's row. The steps
--     fire in order, each once.
--   - The tank is stopped when it drives north onto a step's row that it
--     has not reached before. A row the player already passed does not
--     stop it again after a respawn.
--   - Until the boat step (row 66) has fired, the tank respawns at start 1,
--     out at sea. After it, the tank respawns at start 2, on the far bank,
--     and the first such respawn shows one more message.
--
-- THE LOBBY. As the old tutorial did, the game has no hidden mines, no
-- start delay and no time limit. The lobby block fixes all three, and the
-- host cannot change them while this scenario is attached.
--
-- The scenario changes no game rule and fields no bots. A bot the host added
-- is taken off the field as the round starts, as the Tutorial does.
-- =========================================================================

scenario = {
  name = "Classic Tutorial",
  description = "The original WinBolo tutorial: drive north up the map "
    .. "and read each message as you reach it.",
  api  = 1,
  kind = "scenario",
  author  = "WinBolo",
  updated = "2026-10-26T22:13Z",
  game = "strict",

  lobby = {
    max_players = 1,
    hidden_mines = false,
    time_limit = 0,
    start_delay = 0,
  },

  callbacks = {
    on_start = "Takes off the field any bot the host added.",
    on_player_join = "Makes the player who joins the tutorial's player.",
    on_player_leave = "Forgets a player who leaves, so the next one carries on from the same step.",
    on_tick = "Shows each step's messages when your tank reaches its row, and stops the tank there the first time.",
    on_tank_spawned = "Shows the far-bank message the first time you respawn there.",
    on_choose_start = "You respawn out at sea until the boat step, then on the far bank.",
  },
}

-- ---------------------------------------------------------------------
-- The steps, in order: the row (map square y) that fires the step, and
-- the names of its messages in the language files. The first step fires
-- on any row. These are the old tutorial's rows and messages, with one
-- change: the links message (STR_TUTORIAL_START02) is the last popup, not
-- the second, so the first control the player reads comes right after the
-- welcome.
local STEPS = {
  { row = nil, msgs = { "STR_TUTORIAL_START01", "STR_TUTORIAL_START04" } },
  { row = 210, msgs = { "STR_TUTORIAL01" } },
  { row = 199, msgs = { "STR_TUTORIAL02" } },
  { row = 192, msgs = { "STR_TUTORIAL03" } },
  { row = 186, msgs = { "STR_TUTORIAL04" } },
  { row = 181, msgs = { "STR_TUTORIAL05" } },
  { row = 175, msgs = { "STR_TUTORIAL06" } },
  { row = 166, msgs = { "STR_TUTORIAL07" } },
  { row = 159, msgs = { "STR_TUTORIAL08", "STR_TUTORIAL09" } },
  { row = 142, msgs = { "STR_TUTORIAL10", "STR_TUTORIAL11" } },
  { row = 122, msgs = { "STR_TUTORIAL12" } },
  { row = 120, msgs = { "STR_TUTORIAL13", "STR_TUTORIAL14" } },
  { row = 110, msgs = { "STR_TUTORIAL15" } },
  { row = 103, msgs = { "STR_TUTORIAL16" } },
  { row =  98, msgs = { "STR_TUTORIAL17" } },
  { row =  84, msgs = { "STR_TUTORIAL18", "STR_TUTORIAL19" } },
  { row =  66, msgs = { "STR_TUTORIAL20", "STR_TUTORIAL21" } },
  { row =  47, msgs = { "STR_TUTORIAL22", "STR_TUTORIAL23",
                        "STR_TUTORIAL24" } },
  { row =  21, msgs = { "STR_TUTORIAL25", "STR_TUTORIAL_START02" } },
}

-- After the step on this row, respawns move to the far bank.
local BOAT_ROW = 66
local START_SEA  = 1   -- out at sea, south of the first row
local START_BANK = 2   -- the far bank, north of the boat step
local RESPAWN_MSG = "STR_TUTORIAL_RESPAWN1"

-- The rows that stop the tank: every step's row.
local STOP_ROW = {}
for _, s in ipairs(STEPS) do
  if s.row ~= nil then STOP_ROW[s.row] = true end
end

-- ---------------------------------------------------------------------
-- Round state. The host builds a fresh Lua state for every round.
local S = {
  player   = nil,    -- the tutorial's player seat
  step     = 1,      -- the next step in STEPS to fire
  min_row  = 256,    -- the furthest north row the tank has reached
  prev_my  = nil,    -- the tank's row at the last tick
  far_bank = false,  -- the boat step has fired: respawn at START_BANK
  respawn_shown = false,  -- the far-bank respawn message has been shown
}

-- An arena sets CLASSIC_TUTORIAL_PLAYER to the seat that plays the player
-- (a bot seat there); in a real round it is the one human seat.
local function is_player_seat(p)
  if CLASSIC_TUTORIAL_PLAYER ~= nil then return p == CLASSIC_TUTORIAL_PLAYER end
  local s = game.lobby_slot(p)
  return s ~= nil and not s.bot
end

local function show(name)
  game.popup("{LANG:" .. name .. "}", S.player)
end

local function fire_step()
  local s = STEPS[S.step]
  for _, name in ipairs(s.msgs) do show(name) end
  if s.row == BOAT_ROW then S.far_bank = true end
  -- The last step finishes the tutorial. In a single-player game this
  -- hides the main menu's Tutorial button.
  if S.step == #STEPS then game.tutorial_done(S.player) end
  S.step = S.step + 1
end

-- ---------------------------------------------------------------------
-- Hooks.

function on_start()
  for p = 0, game.max_tanks() - 1 do
    if S.player == nil and game.lobby_slot(p) ~= nil and is_player_seat(p) then
      S.player = p
    end
  end
  -- A bot the tutorial did not ask for would drive about the map.
  for p = 0, game.max_tanks() - 1 do
    local s = game.lobby_slot(p)
    if s ~= nil and s.bot and s.fielded and p ~= S.player and
       p ~= CLASSIC_TUTORIAL_PLAYER then
      game.remove_bot(p)
    end
  end
end

function on_player_join(p, scripted)
  if scripted then return end
  if S.player == nil and is_player_seat(p) then
    S.player = p
    S.prev_my = nil
  end
end

function on_player_leave(p, scripted)
  if p ~= nil and p == S.player then
    S.player = nil
    S.prev_my = nil
  end
end

function on_choose_start(p, lobby)
  if p == nil then return nil end
  if p == S.player or (S.player == nil and is_player_seat(p)) then
    if S.far_bank then return START_BANK end
    return START_SEA
  end
  return nil
end

function on_tank_spawned(p, mx, my, respawn, scripted)
  if p ~= S.player then return end
  -- A new tank did not drive to where it is, so no row was entered.
  S.prev_my = nil
  if respawn and S.far_bank and not S.respawn_shown then
    S.respawn_shown = true
    show(RESPAWN_MSG)
  end
end

function on_tick(tick)
  if S.player == nil then return end
  local t = game.tank(S.player)
  if t == nil or t.dead then
    S.prev_my = nil
    return
  end
  local my = t.my

  -- The stop: the tank drove north onto a step's row that is further north
  -- than any row it reached before.
  if S.prev_my ~= nil and my < S.prev_my and my < S.min_row and
     STOP_ROW[my] then
    game.stop_tank(S.player)
  end
  if my < S.min_row then S.min_row = my end
  S.prev_my = my

  -- The next step, when the tank is on its row (the intro on any row).
  local s = STEPS[S.step]
  if s ~= nil and (s.row == nil or s.row == my) then
    fire_step()
  end
end
