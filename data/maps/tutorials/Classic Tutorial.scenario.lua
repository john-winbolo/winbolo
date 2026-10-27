-- =========================================================================
-- Classic Tutorial — the tutorial WinBolo has always had, as a scenario.
--
-- This is the old built-in tutorial (Inbuilt Tutorial.map and the step
-- table that was in src/bolo/tutorial.c), played the same way: the player
-- drives north up the map, and each time the tank reaches one of the rows
-- below, the game stops the tank and shows that row's messages. The
-- messages are the old ones, in every language the game ships, and in the
-- wording for keyboard, controller or touch.
--
-- WHY "CLASSIC". The new Tutorial (data/maps/tutorials/Tutorial.map) is the
-- one the main menu starts. This file keeps the old one for players who know
-- it, under a name that tells the two apart.
--
-- THE TEXT. Every message is in this file, in TEXT[code], one table for
-- each language the game ships, laid out as the Tutorial lays out its own
-- (see "Text in the player's language" and "Text for the player's input" in
-- docs/SCENARIO_API.md). game.lang(p) picks the language and game.input(p)
-- the wording, for a message worded for touch or controller. A language with
-- no table uses the English, and a hosted server, which is never told the
-- input, sends the keyboard wording. A message longer than the 511 bytes a
-- popup may be is written as two parts, sent one after the other. The other
-- languages' tables are at the end of the file.
--
-- WHAT IT DOES, STEP BY STEP (the same as the old tutorial):
--   - The intro (three messages) opens as soon as the player's tank is on
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
  description = "The original Bolo/WinBolo tutorial from 1993/1998.",
  api  = 1,
  kind = "scenario",
  author  = "WinBolo",
  updated = "2026-10-27T13:47Z",
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
-- its messages, by their keys in TEXT. The first step fires on any row.
-- These are the old tutorial's rows and messages, in its order.
local STEPS = {
  { row = nil, msgs = { "start01", "start02", "start04" } },
  { row = 210, msgs = { "t01" } },
  { row = 199, msgs = { "t02" } },
  { row = 192, msgs = { "t03" } },
  { row = 186, msgs = { "t04" } },
  { row = 181, msgs = { "t05" } },
  { row = 175, msgs = { "t06" } },
  { row = 166, msgs = { "t07" } },
  { row = 159, msgs = { "t08", "t09" } },
  { row = 142, msgs = { "t10", "t11" } },
  { row = 122, msgs = { "t12" } },
  { row = 120, msgs = { "t13", "t14" } },
  { row = 110, msgs = { "t15" } },
  { row = 103, msgs = { "t16" } },
  { row =  98, msgs = { "t17" } },
  { row =  84, msgs = { "t18", "t19" } },
  { row =  66, msgs = { "t20", "t21" } },
  { row =  47, msgs = { "t22", "t23", "t24" } },
  { row =  21, msgs = { "t25" } },
}

-- After the step on this row, respawns move to the far bank.
local BOAT_ROW = 66
local START_SEA  = 1   -- out at sea, south of the first row
local START_BANK = 2   -- the far bank, north of the boat step
local RESPAWN_MSG = "respawn1"

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

-- ---------------------------------------------------------------------
-- Texts. TEXT[code].pop[key] is a message; the tables are at the end of the
-- file. A message worded for each input is { keyboard = , touch = ,
-- controller = }, and one the language leaves out is the keyboard's. A
-- message too long for one popup is a list of its parts. A text is picked
-- when it is sent, so a player who changes language reads the next message
-- in the new one.
local TEXT = {}

-- Message key for the tutorial's player: the parts to send, in order.
local function message(key)
  local mine = TEXT[game.lang(S.player)] or TEXT.en
  local v = mine.pop[key]
  if v == nil then v = TEXT.en.pop[key] end
  if type(v) == "table" and v.keyboard ~= nil then
    v = v[game.input(S.player)] or v.keyboard
  end
  if type(v) == "string" then return { v } end
  return v
end

local function show(key)
  for _, part in ipairs(message(key)) do game.popup(part, S.player) end
end

local function fire_step()
  local s = STEPS[S.step]
  for _, key in ipairs(s.msgs) do show(key) end
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
    game.set_speed(S.player, 0)
  end
  if my < S.min_row then S.min_row = my end
  S.prev_my = my

  -- The next step, when the tank is on its row (the intro on any row).
  local s = STEPS[S.step]
  if s ~= nil and (s.row == nil or s.row == my) then
    fire_step()
  end
end

-- ---------------------------------------------------------------------
-- The messages, one table for each language: the old tutorial's messages
-- as the game shipped them in its language files, STR_TUTORIAL01 to 25 and
-- the rest, with their touch and controller wordings.

TEXT.en = {
  pop = {
    start01 = {
      keyboard = "Welcome to the WinBolo tutorial. This island introduces\nthe basic principles of WinBolo and leads you through\nthem one at a time. Each new principle will be\ndescribed in a window like this one.\n\nAfter reading each message,\npress {DISMISS} to continue.",
      touch = "Welcome to the WinBolo tutorial. This island introduces\nthe basic principles of WinBolo and leads you through\nthem one at a time. Each new principle will be\ndescribed in a window like this one.\n\nAfter reading each message, you can proceed by\ntapping the \"OK\" button.",
    },
    start02 = "WinBolo is the only authorized clone of Stuart Cheshire's classic Macintosh network game, Bolo.\nYou can find strategy hints and other information from the official website:\n\nhttps://www.winbolo.com\n\nOr come join us on Reddit and Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = { "You are in control of a tank, which is currently on a\nboat at sea. Press {ACCEL} to make the tank (and boat) go\nforwards, and press {BRAKE} to make it slow down and\nstop. Press {LEFT} to turn left and {RIGHT} to turn right.", "Press and hold {ACCEL} until the tank is going at the\nspeed you want, then let go. The tank keeps that speed\nuntil you press {BRAKE} or it drives onto slower ground.\nEach new message stops the tank.\n\nPress {ACCEL} now to make the boat drive forwards\ntowards the land.\n(Press {DISMISS} first to dismiss the window)" },
      touch = { "You are in control of a tank, which is currently on a\nboat at sea. Use the thumbstick on the left side of the\nscreen to drive: push it forward to speed up, pull it\nback to slow down and stop, and push it to the left or\nright to turn the tank in that direction.", "Hold the thumbstick forward until the tank is going at\nthe speed you want, then let go. The tank keeps that\nspeed until you pull the thumbstick back or it drives\nonto slower ground. Each new message stops the tank.\n\nPush the thumbstick forward now to make the boat drive\nforwards towards the land.\n(Tap OK first to dismiss this window)" },
      controller = { "You are in control of a tank, which is currently on a\nboat at sea. Push the left stick forward to make the\ntank (and boat) go forwards, and pull it back to make\nit slow down and stop. Push the left stick to the left\nor right to turn the tank in that direction.", "Hold the left stick forward until the tank is going at\nthe speed you want, then let go. The tank keeps that\nspeed until you pull the stick back or it drives onto\nslower ground. Each new message stops the tank.\n\nPush the left stick forward now to drive the boat\ntowards the land.\n(Press {DISMISS} first to dismiss the window)" },
    },
    t01 = {
      keyboard = "Ahead of you is a short river leading inland. Hold\ndown the {ACCEL} key to drive your boat to the end of\nthe river.\n\nWhen you get there, keep going. The tank will\ndisembark from the boat and the boat will be left\nmoored at the end of the river.",
      touch = "Ahead of you is a short river leading inland. Push\nthe thumbstick forward to drive your boat to the end\nof the river.\n\nWhen you get there, keep going. The tank will\ndisembark from the boat and the boat will be left\nmoored at the end of the river.",
      controller = "Ahead of you is a short river leading inland. Push\nthe left stick forward to drive your boat to the end\nof the river.\n\nWhen you get there, keep going. The tank will\ndisembark from the boat and the boat will be left\nmoored at the end of the river.",
    },
    t02 = {
      keyboard = "You are now on the grass. The tank moves quite\nquickly on grass.\n\nPress {ACCEL} to drive ahead to the forest.",
      touch = "You are now on the grass. The tank moves quite\nquickly on grass.\n\nPush the thumbstick forward to drive ahead\nto the forest.",
      controller = "You are now on the grass. The tank moves quite\nquickly on grass.\n\nPush the left stick forward to drive ahead\nto the forest.",
    },
    t03 = {
      keyboard = "You are now in the forest. The tank moves more\nslowly in the forest than it does on grass.\n\nPress {ACCEL} to drive ahead to the swamp.",
      touch = "You are now in the forest. The tank moves more\nslowly in the forest than it does on grass.\n\nPush the thumbstick forward to drive ahead\nto the swamp.",
      controller = "You are now in the forest. The tank moves more\nslowly in the forest than it does on grass.\n\nPush the left stick forward to drive ahead\nto the swamp.",
    },
    t04 = {
      keyboard = "You are now in the swamp. The tank moves very\nslowly in swamp. Fortunately there is a road ahead.\n\nPress {ACCEL} to drive ahead to the road.",
      touch = "You are now in the swamp. The tank moves very\nslowly in swamp. Fortunately there is a road ahead.\n\nPush the thumbstick forward to drive ahead\nto the road.",
      controller = "You are now in the swamp. The tank moves very\nslowly in swamp. Fortunately there is a road ahead.\n\nPush the left stick forward to drive ahead\nto the road.",
    },
    t05 = {
      keyboard = "You are now on the road. The tank can move very\nquickly on road and if you hold {ACCEL} the tank will\nspeed up.\n\nIf you find yourself going too fast you can press {BRAKE}\nto slow down.",
      touch = "You are now on the road. The tank can move very\nquickly on road, and holding the thumbstick forward\nwill speed it up.\n\nIf you find yourself going too fast, pull the\nthumbstick back to slow down.",
      controller = "You are now on the road. The tank can move very\nquickly on road, and holding the left stick forward\nwill speed it up.\n\nIf you find yourself going too fast, pull the left\nstick back to slow down.",
    },
    t06 = {
      keyboard = "There are some buildings ahead. Buildings are solid\nobstacles that you cannot drive through, so you will\nhave to find your way through the maze to the other\nside.\n\nTo make the tank turn left press {LEFT}\nTo make the tank turn right press {RIGHT}",
      touch = "There are some buildings ahead. Buildings are solid\nobstacles that you cannot drive through, so you will\nhave to find your way through the maze to the other\nside.\n\nPush the thumbstick to the left or right to turn the\ntank in that direction.",
      controller = "There are some buildings ahead. Buildings are solid\nobstacles that you cannot drive through, so you will\nhave to find your way through the maze to the other\nside.\n\nPush the left stick to the left or right to turn the\ntank in that direction.",
    },
    t07 = "These round objects with the red guns poking out are\nautomatic pillboxes. They will shoot at any tank\nthat comes within range.\n\nFortunately the buildings provide protection from the\nshots, so the pillboxes will not be able to hit you\nunless you wait so long that they manage to\ncompletely shoot their way through the buildings.",
    t08 = "Forest also provides protection from pillboxes, but in\na different way. As well as slowing down the tank a\nlot, driving through forest also limits visibility.\n\nWhen you are inside forest you can still clearly see\neverything outside the forest, but pillboxes (and\nother players) cannot see your tank unless you get\nvery close to them.",
    t09 = "Line up your tank with the middle of the strip of\nforest and drive due North. If you go straight\nand don't stray too close to the edges of the forest,\nthe pillboxes will not see you and they will not shoot.\n\nDon't drive outside the forest or you'll get blown to pieces.",
    t10 = {
      keyboard = "There is one other simple way to escape pillboxes -\nspeed.\n\nMake sure you are correctly lined up with the centre\nof the road, and hold {ACCEL} to go full speed past the\npillboxes.",
      touch = "There is one other simple way to escape pillboxes -\nspeed.\n\nMake sure you are correctly lined up with the centre\nof the road, and hold the thumbstick forward to go\nfull speed past the pillboxes.",
      controller = "There is one other simple way to escape pillboxes -\nspeed.\n\nMake sure you are correctly lined up with the centre\nof the road, and hold the left stick forward to go\nfull speed past the pillboxes.",
    },
    t11 = "When they notice you they will start shooting, but if\nyou keep going straight ahead and don't lose your\nnerve you will be out of range before the shots can\nhit you.\n\nDon't run off the road into the marsh or you'll be a\nsitting duck for the pillboxes, and you won't be\nable to move quickly enough to escape.",
    t12 = "Ahead of you is a refuelling base. Refuelling bases provide\nshells for you to shoot, mines for you to lay secret traps\nwith, and they also repair your tank's armour back to full\nstrength if it has been damaged.\n\nStop on the refuelling base and load up your tank\nwith shells.",
    t13 = "You are on the refuelling base. Your tank will now\nload up with shells and mines, as the indicators on\nthe right will show. Ahead of you, beyond the\nbuildings, is a minefield.",
    t14 = {
      keyboard = "If you drive your tank into the minefield it will be\ndestroyed. Fortunately you can clear a minefield by\nshooting. Press {FIRE} to shoot.\n\nShoot a hole through the building, and then land a\nshell in the middle of the minefield to detonate it.\nWhen you blow up the mines they will make a big\ncrater in the ground, which you can drive through,\nalthough it is as slow as driving through swamp.",
      touch = "If you drive your tank into the minefield it will be\ndestroyed. Fortunately you can clear a minefield by\nshooting. Tap the fire button to shoot.\n\nShoot a hole through the building, and then land a\nshell in the middle of the minefield to detonate it.\nWhen you blow up the mines they will make a big\ncrater in the ground, which you can drive through,\nalthough it is as slow as driving through swamp.",
      controller = "If you drive your tank into the minefield it will be\ndestroyed. Fortunately you can clear a minefield by\nshooting. Press {FIRE} to shoot.\n\nShoot a hole through the building, and then land a\nshell in the middle of the minefield to detonate it.\nWhen you blow up the mines they will make a big\ncrater in the ground, which you can drive through,\nalthough it is as slow as driving through swamp.",
    },
    t15 = "Ahead is another minefield. Do the same trick again:\nDrive onto the refuelling base, shoot through the\nwall, and land a shell in the middle of the minefield to\ndetonate it.\n\nThis time though, the minefield is adjacent to a large\nriver, and water will flood into the craters. You can\ndrive the tank through the water like you can through\ncraters and marsh.",
    t16 = {
      keyboard = "WinBolo automatically scrolls the window to show you\nthe area around your tank but you can use the\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\nkeys to manually override the view WinBolo chooses.\n\nPress {SCROLL_UP} now to get a better view of the\nriver ahead of you.",
      touch = "WinBolo automatically scrolls the window to show you\nthe area around your tank, but you can drag the map\nwith a finger to manually override the view.\n\nDrag the map down now to bring the river ahead\nof you into view.",
      controller = "WinBolo automatically scrolls the window to show you\nthe area around your tank, but you can push the right\nstick to manually override the view WinBolo chooses.\n\nPush the right stick up now to get a better view of\nthe river ahead of you.",
    },
    t17 = "As well as being very slow, driving through water has\nanother disadvantage: It will also deplete your tank's\nsupplies of shells and mines.\n\nWhen you get to the other side your tank will still be\nin working condition, but you will have lost all\noffensive capability.",
    t18 = {
      keyboard = "This time we are going to be more subtle about\ncrossing the river. Instead of just driving through the\nwater, we are going to build a bridge.\n\nSelect tree farming mode by clicking on the forest\nicon (top left of the window) and click on the forest.\nYour builder will run out of the tank and harvest one\ntree for building materials.",
      touch = "This time we are going to be more subtle about\ncrossing the river. Instead of just driving through the\nwater, we are going to build a bridge.\n\nSelect tree farming mode by tapping the forest icon\n(top left of the window) and tap on the forest. Your\nbuilder will run out of the tank and harvest one tree\nfor building materials.",
      controller = "This time we are going to be more subtle about\ncrossing the river. Instead of just driving through the\nwater, we are going to build a bridge.\n\nPull {BUILD_MODE} to switch from scrolling into build\nmode - the right stick now moves a build cursor instead\nof the view. Press {BUILD_TOOL} to select tree-farming\nmode (top left of the window), aim the cursor at a tree\nand press {BUILD_PLACE}. Your builder will run out of\nthe tank and harvest one tree for building materials.",
    },
    t19 = {
      keyboard = "After farming some trees, select the road/bridge tool\n(the second button from the top) and click the mouse\non the river at the end of the road.\n\nYour builder will run out of the tank and build one\nsegment of bridge. Click again to complete the bridge\nand then drive across to the other side.",
      touch = "After farming some trees, select the road/bridge tool\n(the second button from the top) and tap on the\nriver at the end of the road.\n\nYour builder will run out of the tank and build one\nsegment of bridge. Tap again to complete the bridge\nand then drive across to the other side.",
      controller = "After farming some trees, press {BUILD_TOOL} to select\nthe road/bridge tool (the second button from the top),\naim the build cursor at the river at the end of the\nroad and press {BUILD_PLACE}.\n\nYour builder will run out of the tank and build one\nsegment of bridge. Place again to complete the bridge,\nthen pull {BUILD_MODE} to leave build mode and drive\nacross to the other side.",
    },
    t20 = "This river is wider than the last, and at the centre it is\ntoo deep for you to build a bridge. If you try to just\ndrive through the tank will sink to the bottom and be\ndestroyed.",
    t21 = {
      keyboard = "To cross this river you will have to build a boat. Boats\ntake five trees, so click on the five trees to farm them.\nThen select the wall/boat tool (the third button from\nthe top) and click on the water to build a boat.\n\nIf you click on the land you will build a wall instead\nand you will have to farm some more trees to build a\nboat. Drive onto the boat and cross the river to the\nother side.",
      touch = "To cross this river you will have to build a boat. Boats\ntake five trees, so tap on the five trees to farm them.\nThen select the wall/boat tool (the third button from\nthe top) and tap on the water to build a boat.\n\nIf you tap on the land you will build a wall instead\nand you will have to farm some more trees to build a\nboat. Drive onto the boat and cross the river to the\nother side.",
      controller = "To cross this river you will have to build a boat. Boats\ntake five trees, so aim at five trees and press\n{BUILD_PLACE} on each to farm them. Then press\n{BUILD_TOOL} to select the wall/boat tool (the third\nbutton from the top) and place it on the water.\n\nIf you place on land you will build a wall instead, and\nyou will have to farm more trees to build a boat. Drive\nonto the boat and cross the river to the other side.",
    },
    t22 = "There is a pillbox ahead, and some refuelling bases to\nprovide you with ammunition. Collect shells from the\nrefuelling base and attack the pillbox.\n\nIt will shoot back, but if your tank gets hit you can easily\nreturn to the refuelling base to get your armour repaired\nback to full strength again.",
    t23 = "With patience and caution you should be able to destroy this\npillbox. Use the skills you have learned to make it easier:\n\nHide in the forest so that the pillbox can't see you,\nfarm trees, build roads to make your tank move\nfaster, and build buildings to provide cover against\npillbox shots.",
    t24 = "After you manage to defeat this pillbox, your final\nchallenge of the tutorial is to take out the two\npillboxes guarding the exit, build a boat, and escape.\n\nTake special care to watch your tank's armour level -\nif your tank is destroyed you'll be given a new tank\nnearby so you can try again.",
    t25 = "Congratulations. You have completed the WinBolo\ntutorial.\n\nYou're ready to play. Join a game online, host one\nfor your friends, or practise against bots.",
    respawn1 = "Your tank was destroyed - but you don't have to start\nthe tutorial over. You've been given a fresh tank just\nacross the water, where you came ashore. Get back into\nthe fight and finish your escape.",
  },
}

TEXT["cs"] = {
  pop = {
    start01 = {
      keyboard = "Vítej ve výukovém kurzu WinBolo. Tento ostrov\ntě seznámí se základními principy WinBolo a provede\ntě jimi postupně. Každý nový princip bude popsán\nv okně jako je toto.\n\nPo přečtení každé zprávy\nstiskni {DISMISS} pro pokračování.",
      touch = "Vítej ve výukovém kurzu WinBolo. Tento ostrov\ntě seznámí se základními principy WinBolo a provede\ntě jimi postupně. Každý nový princip bude popsán\nv okně jako je toto.\n\nPo přečtení každé zprávy můžeš pokračovat\nklepnutím na tlačítko „OK\".",
    },
    start02 = "WinBolo je jedinou autorizovanou kopií klasické macintoshové síťové hry Bolo Stuarta Cheshira.\nStrategické tipy a další informace najdeš na oficiálních webových stránkách:\n\nhttps://www.winbolo.com\n\nNebo se k nám přidej na Redditu a Discordu\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Ovládáš tank, který je právě na lodi na moři.\nStiskni {ACCEL} pro pohyb tanku (a lodě) dopředu\na {BRAKE} pro zpomalení a zastavení.\n\nPřidrž klávesy, dokud tank nedosáhne požadované\nrychlosti, a pak je pusť. Stiskni teď {ACCEL} pro\npohyb lodě směrem k pevnině.\n(Nejdřív stiskni {DISMISS} pro zavření okna)",
      touch = "Ovládáš tank, který je právě na lodi na moři.\nPoužij páčku na levé straně obrazovky pro řízení:\npotlač ji dopředu pro zrychlení, potáhni zpět\npro zpomalení a zastavení.\n\nČím dál páčku posuneš, tím rychleji tank jede.\nPosuň páčku dopředu pro pohyb lodě směrem\nk pevnině.\n(Nejdřív klepni na OK pro zavření okna)",
      controller = "Ovládáš tank, který je právě na lodi na moři.\nPosuň levou páčku dopředu pro pohyb tanku\n(a lodě) dopředu, a potáhni zpět pro zpomalení\na zastavení.\n\nČím dál levou páčku posuneš, tím rychleji tank\njede. Posuň levou páčku dopředu pro pohyb lodě\nsmě­rem k pevnině.\n(Nejdřív stiskni {DISMISS} pro zavření okna)",
    },
    t01 = {
      keyboard = "Před tebou je krátká řeka vedoucí do vnitrozemí.\nPřidrž klávesu {ACCEL} a dojed lodí na konec řeky.\n\nAž tam dojedeš, drž {ACCEL} dál. Tank vystoupí z lodě\na loď zůstane uvázána na konci řeky.",
      touch = "Před tebou je krátká řeka vedoucí do vnitrozemí.\nPosuň páčku dopředu a dojed lodí na konec řeky.\n\nAž tam dojedeš, drž páčku posunutou dopředu.\nTank vystoupí z lodě a loď zůstane uvázána\nna konci řeky.",
      controller = "Před tebou je krátká řeka vedoucí do vnitrozemí.\nPosuň levou páčku dopředu a dojed lodí na konec řeky.\n\nAž tam dojedeš, drž páčku posunutou dopředu.\nTank vystoupí z lodě a loď zůstane uvázána\nna konci řeky.",
    },
    t02 = {
      keyboard = "Jsi teď na trávě. Na trávě se tank pohybuje\ndocela rychle.\n\nPokračuj v mačkání {ACCEL} a jeď dál do lesa.",
      touch = "Jsi teď na trávě. Na trávě se tank pohybuje\ndocela rychle.\n\nDrž páčku posunutou dopředu a jeď dál do lesa.",
      controller = "Jsi teď na trávě. Na trávě se tank pohybuje\ndocela rychle.\n\nDrž levou páčku posunutou dopředu a jeď dál do lesa.",
    },
    t03 = {
      keyboard = "Jsi teď v lese. V lese se tank pohybuje pomaleji\nnež na trávě.\n\nPokračuj v mačkání {ACCEL} a jeď dál do bažiny.",
      touch = "Jsi teď v lese. V lese se tank pohybuje pomaleji\nnež na trávě.\n\nDrž páčku posunutou dopředu a jeď dál do bažiny.",
      controller = "Jsi teď v lese. V lese se tank pohybuje pomaleji\nnež na trávě.\n\nDrž levou páčku posunutou dopředu a jeď dál do bažiny.",
    },
    t04 = {
      keyboard = "Jsi teď v bažině. V bažině se tank pohybuje velmi\npomalu. Naštěstí je před tebou silnice.\n\nPokračuj v mačkání {ACCEL} a jeď dál na silnici.",
      touch = "Jsi teď v bažině. V bažině se tank pohybuje\nvery pomalu. Naštěstí je před tebou silnice.\n\nDrž páčku posunutou dopředu a jeď dál na silnici.",
      controller = "Jsi teď v bažině. V bažině se tank pohybuje\nvery pomalu. Naštěstí je před tebou silnice.\n\nDrž levou páčku posunutou dopředu a jeď dál na silnici.",
    },
    t05 = {
      keyboard = "Jsi teď na silnici. Tank se může po silnici pohybovat\nvelmí rychle a po stisknutí {ACCEL} zrychlí.\n\nPokud jedeš příliš rychle, můžeš stisknout {BRAKE}",
      touch = "Jsi teď na silnici. Tank se může pohybovat po\nsilnici velmi rychle a posunutí páčky naplno\ndopředu ho zrychlí.\n\nPokud jedeš příliš rychle, potáhni páčku\nzpět pro zpomalení.",
      controller = "Jsi teď na silnici. Tank se může pohybovat po\nsilnici velmi rychle a posunutí levé páčky naplno\ndopředu ho zrychlí.\n\nPokud jedeš příliš rychle, potáhni levou páčku\nzpět pro zpomalení.",
    },
    t06 = {
      keyboard = "Vpředu jsou budovy. Budovy jsou pevné překážky,\nkterými nejde projet, takže si musíš najít cestu\nbludištěm na druhou stranu.\n\nPro otočení tanku vlevo stiskni {LEFT}\nPro otočení tanku vpravo stiskni {RIGHT}",
      touch = "Vpředu jsou budovy. Budovy jsou pevné překážky,\nkterými nejde projet, takže si musíš najít cestu\nbludištěm na druhou stranu.\n\nPosuň páčku vlevo nebo vpravo pro otočení\ntanku tím směrem.",
      controller = "Vpředu jsou budovy. Budovy jsou pevné překážky,\nkterými nejde projet, takže si musíš najít cestu\nbludištěm na druhou stranu.\n\nPosuň levou páčku vlevo nebo vpravo pro otočení\ntanku tím směrem.",
    },
    t07 = "Tyto kulaté objekty s červenými děly jsou automatické\nbunkry. Budou střílet na každý tank, který se dostane\ndo jejich dosahu.\n\nNaštěstí budovy poskytují ochranu před výstřely,\ntakže tě bunkry nezasáhnou, pokud nebudeš čekat\ntak dlouho, že se jim podaří úplně prostřílet\nbudovy.",
    t08 = "Les také poskytuje ochranu před bunkry, ale jiným\nzpůsobem. Kromě výrazného zpomalení tanku jízda\nlesem také omezuje viditelnost.\n\nKdyž jsi uvnitř lesa, stále jasně vidíš vše vně,\nale bunkry (a ostatní hráči) tvůj tank nevidí,\npokud se k nim nepřiblížíš dostatečně blízko.",
    t09 = "Nastav tank doprostřed pruhu lesa a jeď přímo na\nsever. Pokud pojedeš rovně a nepřiblížíš se příliš\nk okrajům lesa, bunkry tě neuvidí a nebudou střílet.\n\nNejezdi mimo les, nebo tě roztrhají na kusy.",
    t10 = {
      keyboard = "Je tu ještě jeden jednoduchý způsob, jak uniknout\nbunkrům — rychlost.\n\nUjisti se, že jsi správně zarovnán se středem\nsilnice, a drž {ACCEL} pro plnou rychlost kolem\nbunkrů.",
      touch = "Je tu ještě jeden jednoduchý způsob, jak uniknout\nbunkrům — rychlost.\n\nUjisti se, že jsi správně zarovnán se středem\nsilnice, a posuň páčku naplno dopředu pro\nplnou rychlost kolem bunkrů.",
      controller = "Je tu ještě jeden jednoduchý způsob, jak uniknout\nbunkrům — rychlost.\n\nUjisti se, že jsi správně zarovnán se středem\nsilnice, a posuň levou páčku naplno dopředu pro\nplnou rychlost kolem bunkrů.",
    },
    t11 = "Až si tě všimnou, začnou střílet, ale pokud\npůjdeš stále rovně a neztratíš nervy, budeš mimo\ndosah dřív, než tě výstřely zasáhnou.\n\nNejezdi ze silnice do bažiny, nebo budeš jako terč\npro bunkry a nebudeš se moci pohybovat dost rychle\nna útěk.",
    t12 = "Před tebou je zásobovací základna. Zásobovací\nzákladny ti dodají granáty ke střelbě, miny k kladení\ntajných pastí a opraví pancíř tvého tanku na plnou\nsilupokud byl poškozen.\n\nZastav na zásobovací základně a naplň tank granáty.",
    t13 = "Jsi na zásobovací základně. Tvůj tank se teď\nnaloží granáty a minami, jak ukáží ukazatele vpravo.\nPřed tebou, za budovami, je minové pole.",
    t14 = {
      keyboard = "Pokud vjedete tankem do minového pole, bude\nzničen. Naštěstí minové pole lze vyčistit střelbou.\nStiskni {FIRE} pro střelbu.\n\nProstřílej díru v budově a pak umísti granát\ndo středu minového pole, aby se detonoval.\nKdyž miny vybuchnou, v zemi vznikne velký kráter,\nkterým lze projet, i když je to stejně pomalé\njako jízda bažinou.",
      touch = "Pokud vjedete tankem do minového pole, bude\nzničen. Naštěstí minové pole lze vyčistit střelbou.\nKlepni na tlačítko střelby pro střelbu.\n\nProstřílej díru v budově a pak umísti granát\ndo středu minového pole, aby se detonoval.\nKdyž miny vybuchnou, v zemi vznikne velký kráter,\nkterým lze projet, i když je to stejně pomalé\njako jízda bažinou.",
      controller = "Pokud vjedete tankem do minového pole, bude\nzničen. Naštěstí minové pole lze vyčistit střelbou.\nStiskni {FIRE} pro střelbu.\n\nProstřílej díru v budově a pak umísti granát\ndo středu minového pole, aby se detonoval.\nKdyž miny vybuchnou, v zemi vznikne velký kráter,\nkterým lze projet, i když je to stejně pomalé\njako jízda bažinou.",
    },
    t15 = "Vpředu je další minové pole. Použij stejný trik:\nNajeď na zásobovací základnu, prostřílej zeď\na umísti granát do středu minového pole.\n\nTentokrát je ale minové pole u velké řeky\na voda se rozlije do kráterů. Tankem můžeš\njet vodou stejně jako krátery a bažinou.",
    t16 = {
      keyboard = "WinBolo automaticky posouvá okno, aby ti ukázal\nokolí tanku, ale můžeš použít klávesy\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\npro ruční ovládání pohledu WinBolo.\n\nStiskni teď {SCROLL_UP} pro lepší pohled na\nřeku před tebou.",
      touch = "WinBolo automaticky posouvá okno, aby ti ukázal\nokolí tanku, ale mapu můžeš přetáhnout prstem\npro ruční ovládání pohledu.\n\nPřetáhni mapu dolů pro zobrazení řeky\npřed tebou.",
      controller = "WinBolo automaticky posouvá okno, aby ti ukázal\nokolí tanku, ale pravou páčkou můžeš pohled\nručně přepisovat.\n\nPosuň pravou páčku nahoru pro lepší pohled na\nřeku před tebou.",
    },
    t17 = "Kromě toho, že je velmi pomalá, má jízda vodou\nještě jednu nevýhodu: vyčerpá také zásoby\ngranátů a min tanku.\n\nAž se dostaneš na druhou stranu, tank bude stále\nfunkční, ale ztratíš veškerou útočnou schopnost.",
    t18 = {
      keyboard = "Tentokrát překročíme řeku chytřeji. Místo\npřímé jízdy vodou postavíme most.\n\nVyber režim těžby stromů kliknutím na ikonu lesa\n(vlevo nahoře v okně) a klikni na les. Tvůj\nstavitel vyběhne z tanku a pokácí jeden strom\nna stavební materiál.",
      touch = "Tentokrát překročíme řeku chytřeji. Místo\npřímé jízdy vodou postavíme most.\n\nVyber režim těžby stromů klepnutím na ikonu lesa\n(vlevo nahoře v okně) a klepni na les. Tvůj\nstavitel vyběhne z tanku a pokácí jeden strom\nna stavební materiál.",
      controller = "Tentokrát překročíme řeku chytřeji. Místo\npřímé jízdy vodou postavíme most.\n\nPotáhni {BUILD_MODE} pro přepnutí do režimu\nstavby — pravá páčka nyní pohybuje kurzorem\nstavby místo pohledu. Stiskni {BUILD_TOOL} pro\nvýběr režimu těžby stromů (vlevo nahoře v okně),\nnamiř kurzor na strom a stiskni {BUILD_PLACE}.\nTvůj stavitel vyběhne z tanku a pokácí jeden strom.",
    },
    t19 = {
      keyboard = "Po pokácení stromů vyber nástroj silnice/most\n(druhé tlačítko shora) a klikni myší na řeku\nna konci silnice.\n\nTvůj stavitel vyběhne z tanku a postaví jeden\núsek mostu. Klikni znovu pro dokončení mostu\na pak přejeď na druhou stranu.",
      touch = "Po pokácení stromů vyber nástroj silnice/most\n(druhé tlačítko shora) a klepni na řeku\nna konci silnice.\n\nTvůj stavitel vyběhne z tanku a postaví jeden\núsek mostu. Klepni znovu pro dokončení mostu\na pak přejeď na druhou stranu.",
      controller = "Po pokácení stromů stiskni {BUILD_TOOL} pro výběr\nnástroje silnice/most (druhé tlačítko shora),\nnamiř kurzor stavby na řeku na konci silnice\na stiskni {BUILD_PLACE}.\n\nTvůj stavitel vyběhne z tanku a postaví jeden\núsek mostu. Umísti znovu pro dokončení mostu,\npak potáhni {BUILD_MODE} pro opuštění režimu\nstavby a přejeď na druhou stranu.",
    },
    t20 = "Tato řeka je širší než ta předchozí a uprostřed\nje příliš hluboká na stavbu mostu. Pokud se\npokusíš prostě projet, tank se potopí ke dnu\na bude zničen.",
    t21 = {
      keyboard = "Pro překročení této řeky budeš muset postavit loď.\nLodě vyžadují pět stromů, takže klikni na pět stromů\naby se pokácely. Pak vyber nástroj zeď/loď\n(třetí tlačítko shora) a klikni na vodu pro stavbu lodě.\n\nPokud klikneš na pevninu, postavíš zeď\na budeš muset pokácet více stromů pro stavbu lodě.\nNajeď na loď a přepleť na druhou stranu.",
      touch = "Pro překročení této řeky budeš muset postavit loď.\nLodě vyžadují pět stromů, takže klepni na pět\nstromů aby se pokácely. Pak vyber nástroj zeď/loď\n(třetí tlačítko shora) a klepni na vodu pro stavbu lodě.\n\nPokud klepneš na pevninu, postavíš zeď\na budeš muset pokácet více stromů pro stavbu lodě.\nNajeď na loď a přepleť na druhou stranu.",
      controller = "Pro překročení této řeky budeš muset postavit loď.\nLodě vyžadují pět stromů, takže namiř na pět\nstromů a stiskni {BUILD_PLACE} na každém pro\npokácení. Pak stiskni {BUILD_TOOL} pro výběr\nnástroje zeď/loď (třetí tlačítko shora) a umísti\nna vodu.\n\nPokud umístíš na pevninu, postavíš zeď a budeš\nmuset pokácet více stromů pro stavbu lodě.\nNajeď na loď a přepleť na druhou stranu.",
    },
    t22 = "Před tebou je bunkr a několik zásobovacích základen,\nkteré ti dodají munici. Vezmi si granáty ze zásobovací\nzákladny a zaútoč na bunkr.\n\nBunkr bude oplácet palbou, ale pokud tvůj tank bude\nzasažen, můžeš se snadno vrátit na zásobovací\nzákladnu a opravit pancíř na plnou sílu.",
    t23 = "S trpělivostí a opatrností by ses měl zvládnout\nzničit tento bunkr. Využij dovednosti, které ses\nnaučil, aby to bylo snazší:\n\nSkryj se v lese, aby tě bunkr neviděl, kácí stromy,\nstav silnice aby se tvůj tank pohyboval rychleji\na stav budovy jako kryt před střelami bunkru.",
    t24 = "Po zničení tohoto bunkru je tvou závěrečnou\nvýzvou kurzu zlikvidovat dva bunkry střežící\nvýchod, postavit loď a uniknout.\n\nDávej zvláštní pozor na úroveň pancíře tanku —\npokud bude tank zničen, dostaneš nový tank\nv blízkosti, abys mohl zkusit štěstí znovu.",
    t25 = "Gratulace. Dokončil jsi výukový kurz WinBolo.\n\nJsi připraven hrát. Připoj se ke hře online, založ\nhru pro své přátele, nebo si zahraj proti botům.",
    respawn1 = "Tvůj tank byl zničen — ale nemusíš začínat kurz\nznovu. Dostals nový tank hned přes vodu,\nkde jsi vystoupil na břeh. Vrať se do boje\na dokonči svůj útěk.",
  },
}

TEXT["de"] = {
  pop = {
    start01 = {
      keyboard = "Willkommen zum WinBolo-Tutorial. Diese Insel führt in die\nGrundprinzipien von WinBolo ein und leitet Sie nacheinander\ndurch sie hindurch. Jedes neue Prinzip wird in einem Fenster\nwie diesem beschrieben.\n\nNach jeder Nachricht\n{DISMISS} drücken, um fortzufahren.",
      touch = "Willkommen zum WinBolo-Tutorial. Diese Insel führt in die\nGrundprinzipien von WinBolo ein und leitet Sie nacheinander\ndurch sie hindurch. Jedes neue Prinzip wird in einem Fenster\nwie diesem beschrieben.\n\nNach dem Lesen jeder Nachricht können Sie fortfahren, indem\nSie auf \"OK\" tippen.",
    },
    start02 = "WinBolo ist der einzige autorisierte Klon von Stuart Cheshires klassischem Macintosh-Netzwerkspiel Bolo.\nStrategietipps und weitere Informationen finden Sie auf der offiziellen Website:\n\nhttps://www.winbolo.com\n\nOder kommen Sie zu uns auf Reddit und Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Sie steuern einen Panzer, der sich derzeit auf einem Boot\nim Meer befindet. Drücken Sie {ACCEL}, um den Panzer (und das\nBoot) vorwärts fahren zu lassen, und drücken Sie {BRAKE},\num ihn zu verlangsamen und zu stoppen.\n\nDrücken und halten Sie die Tasten, bis der Panzer die\ngewünschte Geschwindigkeit erreicht hat, dann lassen Sie\nlos. Drücken Sie jetzt {ACCEL}, um das Boot vorwärts zum\nLand fahren zu lassen.\n(Drücken Sie zuerst {DISMISS}, um das Fenster zu schließen)",
      touch = "Sie steuern einen Panzer, der sich derzeit auf einem Boot\nim Meer befindet. Verwenden Sie den Joystick auf der linken\nBildschirmseite zum Fahren: nach vorn drücken zum\nBeschleunigen, zurück ziehen zum Bremsen und Anhalten.\n\nJe weiter Sie den Joystick drücken, desto schneller fährt der\nPanzer. Drücken Sie den Joystick jetzt nach vorn, um das Boot\nvorwärts zum Land fahren zu lassen.\n(Tippen Sie zuerst OK, um dieses Fenster zu schließen)",
      controller = "Sie steuern einen Panzer, der sich derzeit auf einem Boot\nim Meer befindet. Drücken Sie den linken Stick nach vorn,\num den Panzer (und das Boot) vorwärts fahren zu lassen,\nund ziehen Sie ihn zurück, um zu verlangsamen und zu stoppen.\n\nJe weiter Sie den linken Stick drücken, desto schneller fährt\nder Panzer. Drücken Sie den linken Stick jetzt nach vorn, um\ndas Boot vorwärts zum Land fahren zu lassen.\n(Drücken Sie zuerst {DISMISS}, um das Fenster zu schließen)",
    },
    t01 = {
      keyboard = "Vor Ihnen liegt ein kurzer Fluss, der ins Landesinnere führt.\nHalten Sie {ACCEL} gedrückt, um Ihr Boot bis zum Ende\ndes Flusses zu fahren.\n\nWenn Sie dort ankommen, halten Sie {ACCEL} weiter gedrückt.\nDer Panzer wird das Boot verlassen, und das Boot bleibt\nam Ende des Flusses vertäut zurück.",
      touch = "Vor Ihnen liegt ein kurzer Fluss, der ins Landesinnere führt.\nDrücken Sie den Joystick nach vorn, um Ihr Boot bis zum Ende\ndes Flusses zu fahren.\n\nWenn Sie dort ankommen, halten Sie den Joystick weiter\nvorwärts gedrückt. Der Panzer wird das Boot verlassen, und das\nBoot bleibt am Ende des Flusses vertäut zurück.",
      controller = "Vor Ihnen liegt ein kurzer Fluss, der ins Landesinnere führt.\nDrücken Sie den linken Stick nach vorn, um Ihr Boot bis zum\nEnde des Flusses zu fahren.\n\nWenn Sie dort ankommen, halten Sie den Stick weiter vorwärts\ngedrückt. Der Panzer wird das Boot verlassen, und das Boot\nbleibt am Ende des Flusses vertäut zurück.",
    },
    t02 = {
      keyboard = "Sie befinden sich nun auf Gras. Der Panzer bewegt sich\nauf Gras recht schnell.\n\nDrücken Sie weiter {ACCEL}, um zum Wald vorzurücken.",
      touch = "Sie befinden sich nun auf Gras. Der Panzer bewegt sich\nauf Gras recht schnell.\n\nHalten Sie den Joystick nach vorn gedrückt, um zum Wald\nvorzurücken.",
      controller = "Sie befinden sich nun auf Gras. Der Panzer bewegt sich\nauf Gras recht schnell.\n\nHalten Sie den linken Stick nach vorn gedrückt, um zum\nWald vorzurücken.",
    },
    t03 = {
      keyboard = "Sie befinden sich nun im Wald. Der Panzer bewegt sich\nim Wald langsamer als auf Gras.\n\nDrücken Sie weiter {ACCEL}, um zum Sumpf vorzurücken.",
      touch = "Sie befinden sich nun im Wald. Der Panzer bewegt sich\nim Wald langsamer als auf Gras.\n\nHalten Sie den Joystick nach vorn gedrückt, um zum Sumpf\nvorzurücken.",
      controller = "Sie befinden sich nun im Wald. Der Panzer bewegt sich\nim Wald langsamer als auf Gras.\n\nHalten Sie den linken Stick nach vorn gedrückt, um zum\nSumpf vorzurücken.",
    },
    t04 = {
      keyboard = "Sie befinden sich nun im Sumpf. Der Panzer bewegt sich\nim Sumpf sehr langsam. Glücklicherweise gibt es vor Ihnen\neine Straße.\n\nDrücken Sie weiter {ACCEL}, um zur Straße vorzurücken.",
      touch = "Sie befinden sich nun im Sumpf. Der Panzer bewegt sich\nim Sumpf sehr langsam. Glücklicherweise gibt es vor Ihnen\neine Straße.\n\nHalten Sie den Joystick nach vorn gedrückt, um zur Straße\nvorzurücken.",
      controller = "Sie befinden sich nun im Sumpf. Der Panzer bewegt sich\nim Sumpf sehr langsam. Glücklicherweise gibt es vor Ihnen\neine Straße.\n\nHalten Sie den linken Stick nach vorn gedrückt, um zur\nStraße vorzurücken.",
    },
    t05 = {
      keyboard = "Sie befinden sich nun auf der Straße. Der Panzer kann\nsich auf Straßen sehr schnell bewegen, und wenn Sie {ACCEL}\ndrücken, wird der Panzer beschleunigen.\n\nWenn Sie zu schnell werden, können Sie {BRAKE} drücken",
      touch = "Sie befinden sich nun auf der Straße. Der Panzer kann\nsich auf Straßen sehr schnell bewegen, und wenn Sie den\nJoystick ganz nach vorn drücken, wird er beschleunigen.\n\nWenn Sie zu schnell werden, ziehen Sie den Joystick zurück,\num zu bremsen.",
      controller = "Sie befinden sich nun auf der Straße. Der Panzer kann\nsich auf Straßen sehr schnell bewegen, und wenn Sie den\nlinken Stick ganz nach vorn drücken, wird er beschleunigen.\n\nWenn Sie zu schnell werden, ziehen Sie den linken Stick\nzurück, um zu bremsen.",
    },
    t06 = {
      keyboard = "Vor Ihnen befinden sich einige Gebäude. Gebäude sind solide\nHindernisse, durch die Sie nicht fahren können, also müssen\nSie Ihren Weg durch das Labyrinth zur anderen Seite finden.\n\nUm den Panzer nach links zu drehen, drücken Sie {LEFT}\nUm den Panzer nach rechts zu drehen, drücken Sie {RIGHT}",
      touch = "Vor Ihnen befinden sich einige Gebäude. Gebäude sind solide\nHindernisse, durch die Sie nicht fahren können, also müssen\nSie Ihren Weg durch das Labyrinth zur anderen Seite finden.\n\nDrücken Sie den Joystick nach links oder rechts, um den\nPanzer in diese Richtung zu drehen.",
      controller = "Vor Ihnen befinden sich einige Gebäude. Gebäude sind solide\nHindernisse, durch die Sie nicht fahren können, also müssen\nSie Ihren Weg durch das Labyrinth zur anderen Seite finden.\n\nDrücken Sie den linken Stick nach links oder rechts, um den\nPanzer in diese Richtung zu drehen.",
    },
    t07 = "Diese runden Objekte mit den hervorstehenden roten Geschützen\nsind automatische Bunker. Sie schießen auf jeden Panzer,\nder in ihre Reichweite kommt.\n\nGlücklicherweise bieten die Gebäude Schutz vor den Schüssen,\nsodass die Bunker Sie nicht treffen können, es sei denn,\nSie warten so lange, dass sie sich vollständig durch die\nGebäude hindurchschießen können.",
    t08 = "Auch der Wald bietet Schutz vor Bunkern, allerdings auf\neine andere Art. Neben dem starken Verlangsamen des Panzers\nbeschränkt das Fahren durch den Wald auch die Sicht.\n\nWenn Sie sich im Wald befinden, können Sie weiterhin alles\naußerhalb des Waldes deutlich sehen, aber Bunker (und andere\nSpieler) können Ihren Panzer nicht sehen, es sei denn,\nSie kommen ihnen sehr nahe.",
    t09 = "Richten Sie Ihren Panzer in der Mitte des Waldstreifens aus\nund fahren Sie genau nach Norden. Wenn Sie geradeaus fahren\nund nicht zu nahe an die Waldränder kommen, werden die Bunker\nSie nicht sehen und nicht schießen.\n\nFahren Sie nicht aus dem Wald heraus, sonst werden Sie in Stücke\ngeschossen.",
    t10 = {
      keyboard = "Es gibt noch eine andere einfache Möglichkeit, Bunkern zu\nentkommen - Geschwindigkeit.\n\nStellen Sie sicher, dass Sie korrekt in der Mitte der Straße\nausgerichtet sind, und halten Sie {ACCEL} gedrückt, um mit\nvoller Geschwindigkeit an den Bunkern vorbeizufahren.",
      touch = "Es gibt noch eine andere einfache Möglichkeit, Bunkern zu\nentkommen - Geschwindigkeit.\n\nStellen Sie sicher, dass Sie korrekt in der Mitte der Straße\nausgerichtet sind, und drücken Sie den Joystick voll nach vorn,\num mit voller Geschwindigkeit an den Bunkern vorbeizufahren.",
      controller = "Es gibt noch eine andere einfache Möglichkeit, Bunkern zu\nentkommen — Geschwindigkeit.\n\nStellen Sie sicher, dass Sie korrekt in der Mitte der Straße\nausgerichtet sind, und drücken Sie den linken Stick voll\nnach vorn, um mit voller Geschwindigkeit vorbeizufahren.",
    },
    t11 = "Wenn sie Sie bemerken, werden sie zu schießen beginnen, aber\nwenn Sie weiter geradeaus fahren und nicht die Nerven\nverlieren, sind Sie außer Reichweite, bevor die Schüsse\nSie treffen können.\n\nFahren Sie nicht von der Straße in den Sumpf, sonst sind Sie\nein leichtes Ziel für die Bunker, und Sie können sich nicht\nschnell genug bewegen, um zu entkommen.",
    t12 = "Vor Ihnen befindet sich eine Nachschubbasis. Nachschubbasen\nliefern Granaten zum Schießen, Minen zum Legen geheimer Fallen\nund reparieren auch die Panzerung Ihres Panzers wieder auf\nvolle Stärke, falls sie beschädigt wurde.\n\nHalten Sie auf der Nachschubbasis an und laden Sie Ihren\nPanzer mit Granaten auf.",
    t13 = "Sie sind auf der Nachschubbasis. Ihr Panzer wird sich nun\nmit Granaten und Minen aufladen, wie die Anzeigen rechts\nzeigen werden. Vor Ihnen, hinter den Gebäuden, befindet sich\nein Minenfeld.",
    t14 = {
      keyboard = "Wenn Sie mit Ihrem Panzer in das Minenfeld fahren, wird er\nzerstört. Glücklicherweise können Sie ein Minenfeld durch\nSchießen räumen. Drücken Sie {FIRE}, um zu schießen.\n\nSchießen Sie ein Loch durch das Gebäude und landen Sie dann\neine Granate in der Mitte des Minenfelds, um es zu zünden.\nWenn Sie die Minen sprengen, entsteht ein großer Krater im\nBoden, durch den Sie fahren können, obwohl es so langsam ist\nwie das Fahren durch Sumpf.",
      touch = "Wenn Sie mit Ihrem Panzer in das Minenfeld fahren, wird er\nzerstört. Glücklicherweise können Sie ein Minenfeld durch\nSchießen räumen. Tippen Sie auf den Feuer-Button, um zu schießen.\n\nSchießen Sie ein Loch durch das Gebäude und landen Sie dann\neine Granate in der Mitte des Minenfelds, um es zu zünden.\nWenn Sie die Minen sprengen, entsteht ein großer Krater im\nBoden, durch den Sie fahren können, obwohl es so langsam ist\nwie das Fahren durch Sumpf.",
      controller = "Wenn Sie mit Ihrem Panzer in das Minenfeld fahren, wird er\nzerstört. Glücklicherweise können Sie ein Minenfeld durch\nSchießen räumen. Drücken Sie {FIRE}, um zu schießen.\n\nSchießen Sie ein Loch durch das Gebäude und landen Sie dann\neine Granate in der Mitte des Minenfelds, um es zu zünden.\nWenn Sie die Minen sprengen, entsteht ein großer Krater im\nBoden, durch den Sie fahren können, obwohl es so langsam ist\nwie das Fahren durch Sumpf.",
    },
    t15 = "Vor Ihnen liegt ein weiteres Minenfeld. Wenden Sie denselben\nTrick erneut an: Fahren Sie auf die Nachschubbasis, schießen\nSie durch die Mauer und landen Sie eine Granate in der Mitte\ndes Minenfelds, um es zu zünden.\n\nDiesmal grenzt das Minenfeld jedoch an einen großen Fluss, und\nWasser wird in die Krater fließen. Sie können den Panzer durch\ndas Wasser fahren, genau wie durch Krater und Sumpf.",
    t16 = {
      keyboard = "WinBolo scrollt das Fenster automatisch, um Ihnen den Bereich\num Ihren Panzer zu zeigen, aber Sie können die Tasten\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\nverwenden, um die von WinBolo gewählte Ansicht manuell zu\nüberschreiben.\n\nDrücken Sie jetzt {SCROLL_UP}, um eine bessere Sicht auf den\nFluss vor Ihnen zu erhalten.",
      touch = "WinBolo scrollt das Fenster automatisch, um Ihnen den Bereich\num Ihren Panzer zu zeigen, aber Sie können die Karte mit dem\nFinger ziehen, um die Ansicht manuell zu ändern.\n\nZiehen Sie die Karte jetzt nach unten, um den Fluss vor Ihnen\nins Sichtfeld zu bringen.",
      controller = "WinBolo scrollt das Fenster automatisch, um Ihnen den Bereich\num Ihren Panzer zu zeigen, aber Sie können den rechten Stick\nverwenden, um die Ansicht manuell zu überschreiben.\n\nDrücken Sie den rechten Stick jetzt nach oben, um eine bessere\nSicht auf den Fluss vor Ihnen zu erhalten.",
    },
    t17 = "Neben der Langsamkeit hat das Fahren durch Wasser einen\nweiteren Nachteil: Es verbraucht auch die Granaten- und\nMinenvorräte Ihres Panzers.\n\nWenn Sie auf der anderen Seite ankommen, ist Ihr Panzer noch\nfunktionsfähig, aber Sie haben jegliche Offensivfähigkeit\nverloren.",
    t18 = {
      keyboard = "Diesmal werden wir die Flussüberquerung etwas raffinierter\nangehen. Anstatt einfach durch das Wasser zu fahren,\nwerden wir eine Brücke bauen.\n\nWählen Sie den Baum-Modus, indem Sie auf das Wald-Symbol\n(oben links im Fenster) klicken, und klicken Sie dann auf\nden Wald. Ihr Bauarbeiter wird aus dem Panzer steigen und\neinen Baum als Baumaterial ernten.",
      touch = "Diesmal werden wir die Flussüberquerung etwas raffinierter\nangehen. Anstatt einfach durch das Wasser zu fahren,\nwerden wir eine Brücke bauen.\n\nWählen Sie den Baum-Modus, indem Sie auf das Wald-Symbol\n(oben links im Fenster) tippen, und tippen Sie dann auf den\nWald. Ihr Bauarbeiter wird aus dem Panzer steigen und einen\nBaum als Baumaterial ernten.",
      controller = "Diesmal werden wir die Flussüberquerung etwas raffinierter\nangehen. Anstatt durch das Wasser zu fahren, bauen wir eine\nBrücke.\n\nZiehen Sie {BUILD_MODE}, um in den Baumodus zu wechseln —\nder rechte Stick bewegt jetzt einen Baucursor. Drücken Sie\n{BUILD_TOOL} für den Baumfäll-Modus (oben links im Fenster),\nrichten Sie den Cursor auf einen Baum und drücken Sie\n{BUILD_PLACE}. Ihr Bauarbeiter erntet einen Baum.",
    },
    t19 = {
      keyboard = "Nachdem Sie einige Bäume gefällt haben, wählen Sie das\nStraßen-/Brückenwerkzeug (zweite Schaltfläche von oben) und\nklicken Sie mit der Maus auf den Fluss am Ende der Straße.\n\nIhr Bauarbeiter wird aus dem Panzer steigen und ein Stück der\nBrücke bauen. Klicken Sie erneut, um die Brücke fertigzustellen,\nund fahren Sie dann auf die andere Seite.",
      touch = "Nachdem Sie einige Bäume gefällt haben, wählen Sie das\nStraßen-/Brückenwerkzeug (zweite Schaltfläche von oben) und\ntippen Sie auf den Fluss am Ende der Straße.\n\nIhr Bauarbeiter wird aus dem Panzer steigen und ein Stück der\nBrücke bauen. Tippen Sie erneut, um die Brücke fertigzustellen,\nund fahren Sie dann auf die andere Seite.",
      controller = "Nach dem Fällen einiger Bäume drücken Sie {BUILD_TOOL} für das\nStraßen-/Brückenwerkzeug (zweite Schaltfläche von oben),\nrichten Sie den Baucursor auf den Fluss am Ende der Straße\nund drücken Sie {BUILD_PLACE}.\n\nIhr Bauarbeiter wird ein Stück der Brücke bauen. Erneut\nplatzieren, um die Brücke fertigzustellen, dann {BUILD_MODE}\nziehen, um den Baumodus zu verlassen und hinüberzufahren.",
    },
    t20 = "Dieser Fluss ist breiter als der letzte, und in der Mitte ist\ner zu tief, um eine Brücke zu bauen. Wenn Sie versuchen,\neinfach hindurchzufahren, sinkt der Panzer auf den Grund und\nwird zerstört.",
    t21 = {
      keyboard = "Um diesen Fluss zu überqueren, müssen Sie ein Boot bauen.\nBoote benötigen fünf Bäume, also klicken Sie auf die fünf\nBäume, um sie zu fällen. Wählen Sie dann das Mauer-/Boot-\nWerkzeug (dritte Schaltfläche von oben) und klicken Sie auf\ndas Wasser, um ein Boot zu bauen.\n\nWenn Sie auf das Land klicken, bauen Sie stattdessen eine\nMauer und müssen weitere Bäume fällen, um ein Boot zu bauen.\nFahren Sie auf das Boot und überqueren Sie den Fluss zur\nanderen Seite.",
      touch = "Um diesen Fluss zu überqueren, müssen Sie ein Boot bauen.\nBoote benötigen fünf Bäume, also tippen Sie auf die fünf\nBäume, um sie zu fällen. Wählen Sie dann das Mauer-/Boot-\nWerkzeug (dritte Schaltfläche von oben) und tippen Sie auf\ndas Wasser, um ein Boot zu bauen.\n\nWenn Sie auf das Land tippen, bauen Sie stattdessen eine\nMauer und müssen weitere Bäume fällen, um ein Boot zu bauen.\nFahren Sie auf das Boot und überqueren Sie den Fluss zur\nanderen Seite.",
      controller = "Um diesen Fluss zu überqueren, müssen Sie ein Boot bauen.\nBoote benötigen fünf Bäume — richten Sie den Cursor auf fünf\nBäume und drücken Sie jeweils {BUILD_PLACE}. Dann drücken Sie\n{BUILD_TOOL} für das Mauer-/Bootwerkzeug (dritte Schaltfläche\nvon oben) und platzieren Sie es auf dem Wasser.\n\nAuf dem Land platzieren baut stattdessen eine Mauer, und Sie\nmüssen weitere Bäume fällen. Fahren Sie auf das Boot und\nüberqueren Sie den Fluss zur anderen Seite.",
    },
    t22 = "Vor Ihnen befindet sich ein Bunker und einige Nachschubbasen,\ndie Sie mit Munition versorgen. Sammeln Sie Granaten von der\nNachschubbasis und greifen Sie den Bunker an.\n\nEr wird zurückschießen, aber wenn Ihr Panzer getroffen wird,\nkönnen Sie leicht zur Nachschubbasis zurückkehren, um die\nPanzerung wieder auf volle Stärke zu reparieren.",
    t23 = "Mit Geduld und Vorsicht sollten Sie diesen Bunker zerstören\nkönnen. Verwenden Sie Ihre erlernten Fähigkeiten, um es\nleichter zu machen:\n\nVerstecken Sie sich im Wald, damit der Bunker Sie nicht sehen\nkann, fällen Sie Bäume, bauen Sie Straßen, damit Ihr Panzer\nsich schneller bewegt, und bauen Sie Gebäude, um Schutz vor\nBunkerschüssen zu bieten.",
    t24 = "Nachdem Sie es geschafft haben, diesen Bunker zu besiegen, ist\nIhre letzte Tutorial-Herausforderung, die zwei Bunker, die den\nAusgang bewachen, auszuschalten, ein Boot zu bauen und zu\nentkommen.\n\nAchten Sie besonders auf den Panzerungsstand Ihres Panzers —\nwenn Ihr Panzer zerstört wird, erhalten Sie einen neuen Panzer\nin der Nähe, damit Sie es erneut versuchen können.",
    t25 = "Glückwunsch. Sie haben das WinBolo-Tutorial abgeschlossen.\n\nSie sind bereit zu spielen. Treten Sie einem Online-Spiel bei,\nrichten Sie eines für Ihre Freunde ein oder üben Sie gegen Bots.",
    respawn1 = "Ihr Panzer wurde zerstört — aber Sie müssen das Tutorial\nnicht von vorne beginnen. Sie erhalten einen neuen Panzer\ndirekt über dem Wasser, wo Sie an Land gegangen sind.\nKehren Sie in den Kampf zurück und vollenden Sie Ihre Flucht.",
  },
}

TEXT["es"] = {
  pop = {
    start01 = {
      keyboard = "Bienvenido al tutorial de WinBolo. Esta isla introduce\nlos principios básicos de WinBolo y te lleva a través\nde ellos uno a uno. Cada nuevo principio se describirá\nen una ventana como esta.\n\nTras leer cada mensaje,\npulsa {DISMISS} para continuar.",
      touch = "Bienvenido al tutorial de WinBolo. Esta isla introduce\nlos principios básicos de WinBolo y te lleva a través\nde ellos uno a uno. Cada nuevo principio se describirá\nen una ventana como esta.\n\nDespués de leer cada mensaje, puedes continuar\ntocando el botón \"Aceptar\".",
    },
    start02 = "WinBolo es el único clon autorizado del clásico juego de red de Macintosh de Stuart Cheshire, Bolo.\nPuedes encontrar consejos de estrategia y otra información en el sitio web oficial:\n\nhttps://www.winbolo.com\n\nO únete a nosotros en Reddit y Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Estás al mando de un tanque, que actualmente está en\nuna barca en el mar. Pulsa {ACCEL} para que el tanque\n(y la barca) avance, y pulsa {BRAKE} para que frene\ny se detenga.\n\nMantén pulsadas las teclas hasta que el tanque vaya a\nla velocidad que quieres, luego suéltalas. Pulsa {ACCEL}\nahora para que la barca avance hacia tierra.\n(Pulsa antes {DISMISS} para cerrar la ventana)",
      touch = "Estás al mando de un tanque, que actualmente está\nen una barca en el mar. Usa el stick del lado izquierdo\nde la pantalla para conducir: empújalo hacia delante\npara acelerar, tira de él hacia atrás para frenar y parar.\n\nCuanto más empujes el stick, más rápido va el tanque.\nEmpuja el stick hacia delante ahora para que la barca\navance hacia tierra.\n(Toca antes Aceptar para cerrar esta ventana)",
      controller = "Estás al mando de un tanque, que actualmente está\nen una barca en el mar. Empuja el stick izquierdo\nhacia delante para que el tanque (y la barca) avance,\ny tíralo hacia atrás para que frene y se detenga.\n\nCuanto más empujes el stick izquierdo, más rápido va\nel tanque. Empuja el stick izquierdo hacia delante\nahora para que la barca avance hacia tierra.\n(Pulsa antes {DISMISS} para cerrar la ventana)",
    },
    t01 = {
      keyboard = "Delante de ti hay un río corto que entra hacia tierra.\nMantén pulsada la tecla {ACCEL} para conducir tu barca\nhasta el final del río.\n\nCuando llegues, sigue manteniendo {ACCEL}. El tanque\ndesembarcará de la barca y la barca quedará amarrada\nal final del río.",
      touch = "Delante de ti hay un río corto que entra hacia tierra.\nEmpuja el stick hacia delante para conducir tu barca\nhasta el final del río.\n\nCuando llegues, sigue manteniendo el stick empujado\nhacia delante. El tanque desembarcará de la barca y\nla barca quedará amarrada al final del río.",
      controller = "Delante de ti hay un río corto que entra hacia tierra.\nEmpuja el stick izquierdo hacia delante para conducir\ntu barca hasta el final del río.\n\nCuando llegues, sigue manteniendo el stick empujado\nhacia delante. El tanque desembarcará de la barca y\nla barca quedará amarrada al final del río.",
    },
    t02 = {
      keyboard = "Ahora estás en la hierba. El tanque se mueve bastante\nrápido sobre hierba.\n\nSigue pulsando {ACCEL} para avanzar hasta el bosque.",
      touch = "Ahora estás en la hierba. El tanque se mueve bastante\nrápido sobre hierba.\n\nSigue manteniendo el stick empujado hacia delante\npara avanzar hasta el bosque.",
      controller = "Ahora estás en la hierba. El tanque se mueve bastante\nrápido sobre hierba.\n\nSigue manteniendo el stick izquierdo empujado hacia\ndelante para avanzar hasta el bosque.",
    },
    t03 = {
      keyboard = "Ahora estás en el bosque. El tanque se mueve más\ndespacio en el bosque que sobre hierba.\n\nSigue pulsando {ACCEL} para avanzar hasta el pantano.",
      touch = "Ahora estás en el bosque. El tanque se mueve más\ndespacio en el bosque que sobre hierba.\n\nSigue manteniendo el stick empujado hacia delante\npara avanzar hasta el pantano.",
      controller = "Ahora estás en el bosque. El tanque se mueve más\ndespacio en el bosque que sobre hierba.\n\nSigue manteniendo el stick izquierdo empujado hacia\ndelante para avanzar hasta el pantano.",
    },
    t04 = {
      keyboard = "Ahora estás en el pantano. El tanque se mueve muy\ndespacio en pantano. Por suerte hay una carretera\ndelante.\n\nSigue pulsando {ACCEL} para avanzar hasta la carretera.",
      touch = "Ahora estás en el pantano. El tanque se mueve muy\ndespacio en pantano. Por suerte hay una carretera\ndelante.\n\nSigue manteniendo el stick empujado hacia delante\npara avanzar hasta la carretera.",
      controller = "Ahora estás en el pantano. El tanque se mueve muy\ndespacio en pantano. Por suerte hay una carretera\ndelante.\n\nSigue manteniendo el stick izquierdo empujado hacia\ndelante para avanzar hasta la carretera.",
    },
    t05 = {
      keyboard = "Ahora estás en la carretera. El tanque puede moverse\nmuy rápido por carretera y, si pulsas {ACCEL}, el tanque\nacelerará.\n\nSi notas que vas demasiado rápido puedes pulsar {BRAKE}",
      touch = "Ahora estás en la carretera. El tanque puede moverse\nmuy rápido por carretera y empujar el stick del todo\nhacia delante lo acelerará.\n\nSi notas que vas demasiado rápido, tira del stick hacia\natrás para frenar.",
      controller = "Ahora estás en la carretera. El tanque puede moverse\nmuy rápido por carretera y empujar el stick izquierdo\ndel todo hacia delante lo acelerará.\n\nSi notas que vas demasiado rápido, tira del stick\nizquierdo hacia atrás para frenar.",
    },
    t06 = {
      keyboard = "Hay algunos edificios delante. Los edificios son obstáculos\nsólidos por los que no puedes pasar, así que tendrás que\nencontrar el camino a través del laberinto hasta el otro\nlado.\n\nPara que el tanque gire a la izquierda, pulsa {LEFT}\nPara que el tanque gire a la derecha, pulsa {RIGHT}",
      touch = "Hay algunos edificios delante. Los edificios son\nobstáculos sólidos por los que no puedes pasar, así\nque tendrás que encontrar el camino a través del\nlaberinto hasta el otro lado.\n\nEmpuja el stick a la izquierda o a la derecha para\ngirar el tanque en esa dirección.",
      controller = "Hay algunos edificios delante. Los edificios son\nobstáculos sólidos por los que no puedes pasar, así\nque tendrás que encontrar el camino a través del\nlaberinto hasta el otro lado.\n\nEmpuja el stick izquierdo a la izquierda o a la derecha\npara girar el tanque en esa dirección.",
    },
    t07 = "Estos objetos redondos con cañones rojos asomando son\nbúnkeres automáticos. Dispararán a cualquier tanque\nque entre en su alcance.\n\nPor suerte los edificios protegen de los disparos, así\nque los búnkeres no podrán darte salvo que esperes\ntanto que consigan abrirse paso a tiros a través de los\nedificios.",
    t08 = "El bosque también protege de los búnkeres, pero de\nmodo distinto. Además de ralentizar mucho al tanque,\nconducir por el bosque también limita la visibilidad.\n\nCuando estás dentro del bosque sigues viendo con\nclaridad todo lo que hay fuera, pero los búnkeres (y\notros jugadores) no pueden ver tu tanque a menos que\nse acerquen mucho.",
    t09 = "Alinea tu tanque con el centro de la franja de bosque\ny conduce hacia el norte. Si vas recto y no te acercas\ndemasiado a los bordes del bosque, los búnkeres no\nte verán y no dispararán.\n\nNo salgas del bosque o te volarán en pedazos.",
    t10 = {
      keyboard = "Hay otra forma sencilla de escapar de los búnkeres:\nla velocidad.\n\nAsegúrate de estar bien alineado con el centro de la\ncarretera y mantén pulsado {ACCEL} para pasar a toda\nvelocidad junto a los búnkeres.",
      touch = "Hay otra forma sencilla de escapar de los búnkeres:\nla velocidad.\n\nAsegúrate de estar bien alineado con el centro de la\ncarretera y empuja el stick a fondo hacia delante para\npasar a toda velocidad junto a los búnkeres.",
      controller = "Hay otra forma sencilla de escapar de los búnkeres:\nla velocidad.\n\nAsegúrate de estar bien alineado con el centro de la\ncarretera y empuja el stick izquierdo a fondo hacia\ndelante para pasar a toda velocidad junto a los búnkeres.",
    },
    t11 = "Cuando te detecten empezarán a disparar, pero si sigues\nrecto y no pierdes los nervios estarás fuera de alcance\nantes de que los disparos puedan alcanzarte.\n\nNo te salgas de la carretera al pantano o serás un\nblanco fácil para los búnkeres y no podrás moverte\nlo bastante rápido para escapar.",
    t12 = "Delante de ti hay una base de reabastecimiento. Las\nbases de reabastecimiento te dan obuses para disparar,\nminas para tender trampas secretas y también reparan\nel blindaje de tu tanque hasta su máximo si está dañado.\n\nDetente sobre la base de reabastecimiento y carga tu\ntanque de obuses.",
    t13 = "Estás sobre la base de reabastecimiento. Tu tanque se\ncargará ahora de obuses y minas, como mostrarán los\nindicadores de la derecha. Delante de ti, más allá de\nlos edificios, hay un campo de minas.",
    t14 = {
      keyboard = "Si conduces tu tanque al campo de minas, será destruido.\nPor suerte puedes despejar un campo de minas\ndisparando. Pulsa {FIRE} para disparar.\n\nDispara un agujero a través del edificio y luego mete\nun obús en el centro del campo de minas para detonarlo.\nCuando hagas estallar las minas formarán un gran\ncráter en el suelo, por el que puedes conducir, aunque\nes tan lento como conducir por pantano.",
      touch = "Si conduces tu tanque al campo de minas, será destruido.\nPor suerte puedes despejar un campo de minas\ndisparando. Toca el botón de disparo para disparar.\n\nDispara un agujero a través del edificio y luego mete\nun obús en el centro del campo de minas para detonarlo.\nCuando hagas estallar las minas formarán un gran\ncráter en el suelo, por el que puedes conducir, aunque\nes tan lento como conducir por pantano.",
      controller = "Si conduces tu tanque al campo de minas, será destruido.\nPor suerte puedes despejar un campo de minas\ndisparando. Pulsa {FIRE} para disparar.\n\nDispara un agujero a través del edificio y luego mete\nun obús en el centro del campo de minas para detonarlo.\nCuando hagas estallar las minas formarán un gran\ncráter en el suelo, por el que puedes conducir, aunque\nes tan lento como conducir por pantano.",
    },
    t15 = "Delante hay otro campo de minas. Repite el mismo truco:\nentra en la base de reabastecimiento, dispara a través\ndel muro y mete un obús en el centro del campo de minas\npara detonarlo.\n\nEsta vez, sin embargo, el campo de minas está junto a un\ngran río, y el agua inundará los cráteres. Puedes conducir\nel tanque por el agua igual que por cráteres y pantano.",
    t16 = {
      keyboard = "WinBolo desplaza la ventana automáticamente para\nmostrarte el área alrededor de tu tanque, pero puedes\nusar las teclas {SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\npara anular manualmente la vista que elige WinBolo.\n\nPulsa {SCROLL_UP} ahora para ver mejor el río que\ntienes delante.",
      touch = "WinBolo desplaza la ventana automáticamente para\nmostrarte el área alrededor de tu tanque, pero puedes\narrastrar el mapa con el dedo para anular manualmente\nla vista.\n\nArrastra el mapa hacia abajo ahora para ver el río\nque tienes delante.",
      controller = "WinBolo desplaza la ventana automáticamente para\nmostrarte el área alrededor de tu tanque, pero puedes\nempujar el stick derecho para anular manualmente\nla vista que elige WinBolo.\n\nEmpuja el stick derecho hacia arriba ahora para ver\nmejor el río que tienes delante.",
    },
    t17 = "Además de ser muy lento, conducir por el agua tiene\notro inconveniente: también agota las reservas de\nobuses y minas de tu tanque.\n\nCuando llegues al otro lado, tu tanque seguirá\nfuncionando, pero habrás perdido toda capacidad\nofensiva.",
    t18 = {
      keyboard = "Esta vez vamos a ser más sutiles al cruzar el río. En\nlugar de simplemente conducir por el agua, vamos a\nconstruir un puente.\n\nSelecciona el modo de talar árboles haciendo clic en el\nicono del bosque (arriba a la izquierda de la ventana)\ny haz clic en el bosque. Tu constructor saldrá del\ntanque y cosechará un árbol como material de construcción.",
      touch = "Esta vez vamos a ser más sutiles al cruzar el río. En\nlugar de simplemente conducir por el agua, vamos a\nconstruir un puente.\n\nSelecciona el modo de talar árboles tocando el icono\ndel bosque (arriba a la izquierda de la ventana) y\ntoca el bosque. Tu constructor saldrá del tanque y\ncosechará un árbol como material de construcción.",
      controller = "Esta vez vamos a ser más sutiles al cruzar el río. En\nlugar de simplemente conducir por el agua, vamos a\nconstruir un puente.\n\nTira de {BUILD_MODE} para pasar del desplazamiento\nal modo construcción — el stick derecho mueve ahora\nun cursor de construcción. Pulsa {BUILD_TOOL} para\nseleccionar el modo de talar árboles (arriba a la\nizquierda de la ventana), apunta el cursor a un árbol\ny pulsa {BUILD_PLACE}. Tu constructor talará un árbol.",
    },
    t19 = {
      keyboard = "Tras talar algunos árboles, selecciona la herramienta de\ncarretera/puente (el segundo botón desde arriba) y haz\nclic en el río al final de la carretera.\n\nTu constructor saldrá del tanque y construirá un\ntramo de puente. Haz clic de nuevo para completar el\npuente y luego cruza al otro lado.",
      touch = "Tras talar algunos árboles, selecciona la herramienta\nde carretera/puente (el segundo botón desde arriba)\ny toca el río al final de la carretera.\n\nTu constructor saldrá del tanque y construirá un\ntramo de puente. Toca de nuevo para completar el\npuente y luego cruza al otro lado.",
      controller = "Tras talar algunos árboles, pulsa {BUILD_TOOL} para\nseleccionar la herramienta de carretera/puente (el\nsegundo botón desde arriba), apunta el cursor de\nconstrucción al río al final de la carretera y pulsa\n{BUILD_PLACE}.\n\nTu constructor construirá un tramo de puente. Coloca\nde nuevo para completarlo, luego tira de {BUILD_MODE}\npara salir del modo construcción y cruza al otro lado.",
    },
    t20 = "Este río es más ancho que el anterior y, en el centro,\nes demasiado profundo para construir un puente. Si\nintentas conducir por él, el tanque se hundirá hasta\nel fondo y será destruido.",
    t21 = {
      keyboard = "Para cruzar este río tendrás que construir una barca.\nLas barcas requieren cinco árboles, así que haz clic\nen los cinco árboles para talarlos. Luego selecciona\nla herramienta de muro/barca (el tercer botón desde\narriba) y haz clic en el agua para construir una barca.\n\nSi haces clic en tierra construirás un muro y tendrás\nque talar más árboles para construir una barca.\nConduce sobre la barca y cruza el río al otro lado.",
      touch = "Para cruzar este río tendrás que construir una barca.\nLas barcas requieren cinco árboles, así que toca los\ncinco árboles para talarlos. Luego selecciona la\nherramienta de muro/barca (el tercer botón desde\narriba) y toca el agua para construir una barca.\n\nSi tocas tierra construirás un muro y tendrás que\ntalar más árboles para construir una barca. Conduce\nsobre la barca y cruza el río al otro lado.",
      controller = "Para cruzar este río tendrás que construir una barca.\nLas barcas requieren cinco árboles — apunta a cinco\nárboles y pulsa {BUILD_PLACE} en cada uno. Luego pulsa\n{BUILD_TOOL} para seleccionar la herramienta de\nmuro/barca (el tercer botón desde arriba) y colócala\nen el agua.\n\nSi la colocas en tierra construirás un muro y tendrás\nque talar más árboles. Conduce sobre la barca y\ncruza el río al otro lado.",
    },
    t22 = "Hay un búnker delante y algunas bases de reabastecimiento\npara proporcionarte munición. Recoge obuses de la base\nde reabastecimiento y ataca el búnker.\n\nDevolverá el fuego, pero si tu tanque recibe daños\npuedes volver fácilmente a la base de reabastecimiento\npara reparar el blindaje hasta el máximo otra vez.",
    t23 = "Con paciencia y precaución deberías poder destruir\neste búnker. Usa las habilidades que has aprendido\npara facilitarlo:\n\nEscóndete en el bosque para que el búnker no te vea,\ntala árboles, construye carreteras para que tu tanque\nse mueva más rápido y construye edificios para\nprotegerte de los disparos del búnker.",
    t24 = "Tras conseguir vencer a este búnker, tu reto final del\ntutorial es eliminar los dos búnkeres que vigilan la\nsalida, construir una barca y escapar.\n\nVigila con especial cuidado el nivel de blindaje de tu\ntanque: si te lo destruyen te darán un tanque nuevo\ncerca para que puedas intentarlo de nuevo.",
    t25 = "Enhorabuena. Has completado el tutorial de WinBolo.\n\nYa estás listo para jugar. Únete a una partida\nen línea, organiza una para tus amigos, o practica\ncontra los bots.",
    respawn1 = "Tu tanque fue destruido, pero no tienes que empezar\nel tutorial de nuevo. Te han dado un tanque nuevo justo\nal otro lado del agua, donde desembarcaste. Vuelve al\ncombate y termina tu huida.",
  },
}

TEXT["fr"] = {
  pop = {
    start01 = {
      keyboard = "Bienvenue dans le tutoriel WinBolo. Cette île présente\nles principes de base de WinBolo et vous guide à travers\nceux-ci un par un. Chaque nouveau principe sera décrit\ndans une fenêtre comme celle-ci.\n\nAprès chaque message,\nappuyez sur {DISMISS} pour continuer.",
      touch = "Bienvenue dans le tutoriel WinBolo. Cette île présente\nles principes de base de WinBolo et vous guide à travers\nceux-ci un par un. Chaque nouveau principe sera décrit\ndans une fenêtre comme celle-ci.\n\nAprès avoir lu chaque message, vous pouvez continuer\nen tapant sur le bouton « OK ».",
    },
    start02 = "WinBolo est le seul clone autorisé du jeu en réseau classique de Stuart Cheshire pour Macintosh, Bolo.\nVous trouverez des conseils de stratégie et d'autres informations sur le site officiel :\n\nhttps://www.winbolo.com\n\nOu rejoignez-nous sur Reddit et Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Vous contrôlez un tank, qui est actuellement sur un\nbateau en mer. Appuyez sur {ACCEL} pour faire avancer\nle tank (et le bateau), et appuyez sur {BRAKE} pour le\nralentir et l'arrêter.\n\nAppuyez et maintenez les touches jusqu'à ce que le tank\naille à la vitesse souhaitée, puis relâchez-les. Appuyez\nsur {ACCEL} maintenant pour faire avancer le bateau\nvers la terre.\n(Appuyez d'abord sur {DISMISS} pour fermer la fenêtre)",
      touch = "Vous contrôlez un tank, qui est actuellement sur un\nbateau en mer. Utilisez le joystick sur le côté gauche\nde l'écran pour conduire : poussez-le vers l'avant pour\naccélérer, tirez-le en arrière pour ralentir et arrêter.\n\nPlus vous poussez le joystick, plus le tank va vite.\nPoussez le joystick vers l'avant maintenant pour faire\navancer le bateau vers la terre.\n(Tapez d'abord sur OK pour fermer cette fenêtre)",
      controller = "Vous contrôlez un tank, qui est actuellement sur un\nbateau en mer. Poussez le stick gauche vers l'avant pour\nfaire avancer le tank (et le bateau), et tirez-le en arrière\npour le ralentir et l'arrêter.\n\nPlus vous poussez le stick gauche, plus le tank va vite.\nPoussez le stick gauche vers l'avant maintenant pour faire\navancer le bateau vers la terre.\n(Appuyez d'abord sur {DISMISS} pour fermer la fenêtre)",
    },
    t01 = {
      keyboard = "Devant vous se trouve une courte rivière menant\nvers l'intérieur des terres. Maintenez la touche {ACCEL}\npour conduire votre bateau jusqu'au bout de la rivière.\n\nUne fois arrivé, continuez à maintenir {ACCEL}. Le tank\ndébarquera du bateau et celui-ci restera amarré au\nbout de la rivière.",
      touch = "Devant vous se trouve une courte rivière menant vers\nl'intérieur des terres. Poussez le joystick vers l'avant\npour conduire votre bateau jusqu'au bout de la rivière.\n\nUne fois arrivé, continuez à pousser le joystick vers\nl'avant. Le tank débarquera du bateau et celui-ci restera\namarré au bout de la rivière.",
      controller = "Devant vous se trouve une courte rivière menant vers\nl'intérieur des terres. Poussez le stick gauche vers\nl'avant pour conduire votre bateau jusqu'au bout.\n\nUne fois arrivé, continuez à pousser le stick vers l'avant.\nLe tank débarquera du bateau et celui-ci restera\namarré au bout de la rivière.",
    },
    t02 = {
      keyboard = "Vous êtes maintenant sur l'herbe. Le tank avance\nassez rapidement sur l'herbe.\n\nContinuez à appuyer sur {ACCEL} pour avancer jusqu'à la forêt.",
      touch = "Vous êtes maintenant sur l'herbe. Le tank avance\nassez rapidement sur l'herbe.\n\nContinuez à pousser le joystick vers l'avant pour\navancer jusqu'à la forêt.",
      controller = "Vous êtes maintenant sur l'herbe. Le tank avance\nassez rapidement sur l'herbe.\n\nContinuez à pousser le stick gauche vers l'avant pour\navancer jusqu'à la forêt.",
    },
    t03 = {
      keyboard = "Vous êtes maintenant dans la forêt. Le tank avance\nplus lentement dans la forêt que sur l'herbe.\n\nContinuez à appuyer sur {ACCEL} pour avancer jusqu'au marais.",
      touch = "Vous êtes maintenant dans la forêt. Le tank avance\nplus lentement dans la forêt que sur l'herbe.\n\nContinuez à pousser le joystick vers l'avant pour\navancer jusqu'au marais.",
      controller = "Vous êtes maintenant dans la forêt. Le tank avance\nplus lentement dans la forêt que sur l'herbe.\n\nContinuez à pousser le stick gauche vers l'avant pour\navancer jusqu'au marais.",
    },
    t04 = {
      keyboard = "Vous êtes maintenant dans le marais. Le tank avance\ntrès lentement dans le marais. Heureusement il y a une\nroute devant.\n\nContinuez à appuyer sur {ACCEL} pour avancer jusqu'à la route.",
      touch = "Vous êtes maintenant dans le marais. Le tank avance\ntrès lentement dans le marais. Heureusement il y a une\nroute devant.\n\nContinuez à pousser le joystick vers l'avant pour\navancer jusqu'à la route.",
      controller = "Vous êtes maintenant dans le marais. Le tank avance\ntrès lentement dans le marais. Heureusement il y a une\nroute devant.\n\nContinuez à pousser le stick gauche vers l'avant pour\navancer jusqu'à la route.",
    },
    t05 = {
      keyboard = "Vous êtes maintenant sur la route. Le tank peut\navancer très rapidement sur la route et si vous appuyez\nsur {ACCEL} le tank accélérera.\n\nSi vous allez trop vite, vous pouvez appuyer sur {BRAKE}",
      touch = "Vous êtes maintenant sur la route. Le tank peut\navancer très rapidement sur la route et pousser le\njoystick à fond vers l'avant l'accélérera.\n\nSi vous allez trop vite, tirez le joystick en arrière\npour ralentir.",
      controller = "Vous êtes maintenant sur la route. Le tank peut\navancer très rapidement sur la route et pousser le\nstick gauche à fond vers l'avant l'accélérera.\n\nSi vous allez trop vite, tirez le stick gauche en arrière\npour ralentir.",
    },
    t06 = {
      keyboard = "Il y a des bâtiments devant. Les bâtiments sont des\nobstacles solides que vous ne pouvez pas traverser, donc\nvous devrez trouver votre chemin à travers le labyrinthe\njusqu'à l'autre côté.\n\nPour faire tourner le tank à gauche, appuyez sur {LEFT}\nPour faire tourner le tank à droite, appuyez sur {RIGHT}",
      touch = "Il y a des bâtiments devant. Les bâtiments sont des\nobstacles solides que vous ne pouvez pas traverser, donc\nvous devrez trouver votre chemin à travers le labyrinthe\njusqu'à l'autre côté.\n\nPoussez le joystick à gauche ou à droite pour faire\ntourner le tank dans cette direction.",
      controller = "Il y a des bâtiments devant. Les bâtiments sont des\nobstacles solides que vous ne pouvez pas traverser, donc\nvous devrez trouver votre chemin à travers le labyrinthe\njusqu'à l'autre côté.\n\nPoussez le stick gauche à gauche ou à droite pour faire\ntourner le tank dans cette direction.",
    },
    t07 = "Ces objets ronds avec des canons rouges qui dépassent\nsont des pillboxes automatiques. Ils tireront sur tout\ntank qui entre dans leur portée.\n\nHeureusement, les bâtiments protègent des tirs, donc les\npillboxes ne pourront pas vous toucher à moins que vous\nattendiez si longtemps qu'ils parviennent à se frayer un\nchemin à travers les bâtiments.",
    t08 = "La forêt protège également des pillboxes, mais d'une\nmanière différente. En plus de beaucoup ralentir le\ntank, traverser la forêt limite aussi la visibilité.\n\nQuand vous êtes dans la forêt, vous voyez encore\nclairement tout ce qui est à l'extérieur, mais les\npillboxes (et les autres joueurs) ne peuvent pas voir\nvotre tank à moins de s'en approcher de très près.",
    t09 = "Alignez votre tank avec le milieu de la bande de forêt\net conduisez plein nord. Si vous allez tout droit\net ne vous écartez pas trop des bords de la forêt,\nles pillboxes ne vous verront pas et ne tireront pas.\n\nNe sortez pas de la forêt sinon vous serez réduit en pièces.",
    t10 = {
      keyboard = "Il y a un autre moyen simple d'échapper aux pillboxes —\nla vitesse.\n\nAssurez-vous d'être bien aligné avec le centre de la\nroute, et maintenez {ACCEL} pour passer à pleine vitesse\ndevant les pillboxes.",
      touch = "Il y a un autre moyen simple d'échapper aux pillboxes —\nla vitesse.\n\nAssurez-vous d'être bien aligné avec le centre de la\nroute, et poussez le joystick à fond vers l'avant pour\npasser à pleine vitesse devant les pillboxes.",
      controller = "Il y a un autre moyen simple d'échapper aux pillboxes —\nla vitesse.\n\nAssurez-vous d'être bien aligné avec le centre de la\nroute, et poussez le stick gauche à fond vers l'avant\npour passer à pleine vitesse devant les pillboxes.",
    },
    t11 = "Quand ils vous remarqueront, ils commenceront à tirer,\nmais si vous continuez tout droit et ne perdez pas\nvotre sang-froid, vous serez hors de portée avant que\nles tirs ne vous atteignent.\n\nNe quittez pas la route pour aller dans le marais sinon\nvous serez une cible facile pour les pillboxes, et vous\nne pourrez pas bouger assez vite pour vous échapper.",
    t12 = "Devant vous se trouve une base de ravitaillement. Les\nbases de ravitaillement fournissent des obus à tirer, des\nmines pour poser des pièges secrets, et elles réparent\naussi le blindage de votre tank à pleine puissance s'il\na été endommagé.\n\nArrêtez-vous sur la base de ravitaillement et chargez\nvotre tank avec des obus.",
    t13 = "Vous êtes sur la base de ravitaillement. Votre tank va\nmaintenant se charger d'obus et de mines, comme\nl'indiquent les indicateurs à droite. Devant vous, au-delà\ndes bâtiments, se trouve un champ de mines.",
    t14 = {
      keyboard = "Si vous conduisez votre tank dans le champ de mines, il\nsera détruit. Heureusement vous pouvez nettoyer un\nchamp de mines en tirant. Appuyez sur {FIRE} pour tirer.\n\nTirez un trou à travers le bâtiment, puis posez un obus\nau milieu du champ de mines pour le faire exploser.\nQuand vous faites exploser les mines, elles formeront un\ngrand cratère dans le sol, que vous pouvez traverser,\nmême si c'est aussi lent que de traverser un marais.",
      touch = "Si vous conduisez votre tank dans le champ de mines, il\nsera détruit. Heureusement vous pouvez nettoyer un\nchamp de mines en tirant. Tapez sur le bouton de tir.\n\nTirez un trou à travers le bâtiment, puis posez un obus\nau milieu du champ de mines pour le faire exploser.\nQuand vous faites exploser les mines, elles formeront un\ngrand cratère dans le sol, que vous pouvez traverser,\nmême si c'est aussi lent que de traverser un marais.",
      controller = "Si vous conduisez votre tank dans le champ de mines, il\nsera détruit. Heureusement vous pouvez nettoyer un\nchamp de mines en tirant. Appuyez sur {FIRE} pour tirer.\n\nTirez un trou à travers le bâtiment, puis posez un obus\nau milieu du champ de mines pour le faire exploser.\nQuand vous faites exploser les mines, elles formeront un\ngrand cratère dans le sol, que vous pouvez traverser,\nmême si c'est aussi lent que de traverser un marais.",
    },
    t15 = "Devant se trouve un autre champ de mines. Refaites\nle même tour : roulez sur la base de ravitaillement,\ntirez à travers le mur, et posez un obus au milieu du\nchamp de mines pour le faire exploser.\n\nCette fois cependant, le champ de mines est adjacent à\nune grande rivière, et l'eau s'écoulera dans les cratères.\nVous pouvez conduire le tank à travers l'eau comme à\ntravers les cratères et le marais.",
    t16 = {
      keyboard = "WinBolo fait défiler automatiquement la fenêtre pour\nvous montrer la zone autour de votre tank, mais vous\npouvez utiliser les touches\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\npour modifier manuellement la vue choisie par WinBolo.\n\nAppuyez sur {SCROLL_UP} maintenant pour avoir une\nmeilleure vue de la rivière devant vous.",
      touch = "WinBolo fait défiler automatiquement la fenêtre pour\nvous montrer la zone autour de votre tank, mais vous\npouvez faire glisser la carte avec un doigt pour\nmodifier manuellement la vue.\n\nFaites glisser la carte vers le bas maintenant pour\nfaire apparaître la rivière devant vous.",
      controller = "WinBolo fait défiler automatiquement la fenêtre pour\nvous montrer la zone autour de votre tank, mais vous\npouvez pousser le stick droit pour modifier manuellement\nla vue choisie par WinBolo.\n\nPoussez le stick droit vers le haut maintenant pour avoir\nune meilleure vue de la rivière devant vous.",
    },
    t17 = "En plus d'être très lent, conduire dans l'eau a un autre\ninconvénient : cela épuise aussi les réserves d'obus et\nde mines de votre tank.\n\nQuand vous arriverez de l'autre côté, votre tank sera\ntoujours en état de marche, mais vous aurez perdu\ntoute capacité offensive.",
    t18 = {
      keyboard = "Cette fois nous allons être plus subtils pour traverser\nla rivière. Au lieu de simplement conduire dans l'eau,\nnous allons construire un pont.\n\nSélectionnez le mode récolte d'arbres en cliquant sur\nl'icône forêt (en haut à gauche de la fenêtre) et cliquez\nsur la forêt. Votre constructeur sortira du tank et\nrécoltera un arbre comme matériau de construction.",
      touch = "Cette fois nous allons être plus subtils pour traverser\nla rivière. Au lieu de simplement conduire dans l'eau,\nnous allons construire un pont.\n\nSélectionnez le mode récolte d'arbres en tapotant l'icône\nforêt (en haut à gauche de la fenêtre) et tapez sur la\nforêt. Votre constructeur sortira du tank et récoltera\nun arbre comme matériau de construction.",
      controller = "Cette fois nous allons être plus subtils pour traverser\nla rivière. Au lieu de conduire dans l'eau, nous allons\nconstruire un pont.\n\nTirez {BUILD_MODE} pour passer en mode construction —\nle stick droit déplace maintenant un curseur de\nconstruction. Appuyez sur {BUILD_TOOL} pour le mode\nrécolte d'arbres (en haut à gauche), visez un arbre avec\nle curseur et appuyez sur {BUILD_PLACE}. Votre\nconstructeur récoltera un arbre.",
    },
    t19 = {
      keyboard = "Après avoir récolté quelques arbres, sélectionnez l'outil\nroute/pont (le deuxième bouton à partir du haut) et\ncliquez sur la rivière au bout de la route.\n\nVotre constructeur sortira du tank et bâtira un\nsegment de pont. Cliquez à nouveau pour terminer le\npont puis traversez vers l'autre côté.",
      touch = "Après avoir récolté quelques arbres, sélectionnez l'outil\nroute/pont (le deuxième bouton à partir du haut) et\ntapez sur la rivière au bout de la route.\n\nVotre constructeur sortira du tank et bâtira un\nsegment de pont. Tapez à nouveau pour terminer le\npont puis traversez vers l'autre côté.",
      controller = "Après avoir récolté des arbres, appuyez sur {BUILD_TOOL}\npour sélectionner l'outil route/pont (deuxième bouton en\nhaut), visez la rivière au bout de la route avec le curseur\net appuyez sur {BUILD_PLACE}.\n\nVotre constructeur bâtira un segment de pont. Placez à\nnouveau pour terminer le pont, puis tirez {BUILD_MODE}\npour quitter le mode construction et traversez vers l'autre côté.",
    },
    t20 = "Cette rivière est plus large que la précédente, et au\ncentre elle est trop profonde pour que vous puissiez\nconstruire un pont. Si vous essayez juste de traverser,\nle tank coulera au fond et sera détruit.",
    t21 = {
      keyboard = "Pour traverser cette rivière, vous devrez construire un\nbateau. Les bateaux nécessitent cinq arbres, alors cliquez\nsur les cinq arbres pour les récolter. Sélectionnez ensuite\nl'outil mur/bateau (le troisième bouton à partir du haut)\net cliquez sur l'eau pour construire un bateau.\n\nSi vous cliquez sur la terre vous construirez un mur à la\nplace et il faudra récolter d'autres arbres pour construire\nun bateau. Conduisez sur le bateau et traversez la rivière\njusqu'à l'autre côté.",
      touch = "Pour traverser cette rivière, vous devrez construire un\nbateau. Les bateaux nécessitent cinq arbres, alors tapez\nsur les cinq arbres pour les récolter. Sélectionnez ensuite\nl'outil mur/bateau (le troisième bouton à partir du haut)\net tapez sur l'eau pour construire un bateau.\n\nSi vous tapez sur la terre vous construirez un mur à la\nplace et il faudra récolter d'autres arbres pour construire\nun bateau. Conduisez sur le bateau et traversez la rivière\njusqu'à l'autre côté.",
      controller = "Pour traverser cette rivière, vous devrez construire un\nbateau. Les bateaux nécessitent cinq arbres — visez cinq\narbres et appuyez sur {BUILD_PLACE} pour chacun. Puis\nappuyez sur {BUILD_TOOL} pour l'outil mur/bateau\n(troisième bouton en haut) et placez-le sur l'eau.\n\nSur la terre cela construirait un mur — récoltez d'autres\narbres si cela arrive. Conduisez sur le bateau et\ntraversez la rivière jusqu'à l'autre côté.",
    },
    t22 = "Il y a un pillbox devant, et quelques bases de\nravitaillement pour vous fournir des munitions.\nRécupérez des obus à la base de ravitaillement et\nattaquez le pillbox.\n\nIl ripostera, mais si votre tank est touché, vous pouvez\nfacilement retourner à la base de ravitaillement pour\nréparer votre blindage à pleine puissance.",
    t23 = "Avec patience et prudence, vous devriez pouvoir détruire\nce pillbox. Utilisez les compétences que vous avez apprises\npour vous faciliter la tâche :\n\nCachez-vous dans la forêt pour que le pillbox ne puisse\npas vous voir, récoltez des arbres, construisez des routes\npour faire avancer votre tank plus vite, et construisez\ndes bâtiments pour vous protéger des tirs du pillbox.",
    t24 = "Après avoir réussi à vaincre ce pillbox, votre dernier\ndéfi du tutoriel est d'éliminer les deux pillboxes qui\ngardent la sortie, de construire un bateau et de\nvous échapper.\n\nFaites bien attention au niveau de blindage de votre\ntank — si votre tank est détruit, vous recevrez un nouveau\ntank à proximité pour pouvoir réessayer.",
    t25 = "Félicitations. Vous avez terminé le tutoriel WinBolo.\n\nVous êtes prêt à jouer. Rejoignez une partie en\nligne, hébergez-en une pour vos amis, ou\nentraînez-vous contre des bots.",
    respawn1 = "Votre tank a été détruit — mais vous n'avez pas à\nrecommencer le tutoriel depuis le début. Un nouveau tank\nvous a été donné juste de l'autre côté de l'eau, là où vous\nêtes débarqué. Retournez au combat et terminez votre fuite.",
  },
}

TEXT["it"] = {
  pop = {
    start01 = {
      keyboard = "Benvenuto nel tutorial di WinBolo. Questa isola introduce\ni principi di base di WinBolo e ti guida attraverso\ndi essi uno alla volta. Ogni nuovo principio verrà\ndescritto in una finestra come questa.\n\nDopo ogni messaggio,\npremi {DISMISS} per continuare.",
      touch = "Benvenuto nel tutorial di WinBolo. Questa isola introduce\ni principi di base di WinBolo e ti guida attraverso\ndi essi uno alla volta. Ogni nuovo principio verrà\ndescritto in una finestra come questa.\n\nDopo aver letto ogni messaggio, puoi procedere\ntoccando il pulsante \"OK\".",
    },
    start02 = "WinBolo è l'unico clone autorizzato del classico gioco di rete per Macintosh di Stuart Cheshire, Bolo.\nPuoi trovare suggerimenti strategici e altre informazioni sul sito ufficiale:\n\nhttps://www.winbolo.com\n\nOppure vieni a trovarci su Reddit e Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Sei al controllo di un tank, che si trova attualmente su una\nbarca in mare. Premi {ACCEL} per far avanzare il tank (e la barca)\ne premi {BRAKE} per rallentare e fermarti.\n\nPremi e tieni premuti i tasti finché il tank non\nraggiunge la velocità che desideri, poi lascia andare.\nPremi {ACCEL} ora per far avanzare la barca verso\nla terraferma.\n(Premi prima {DISMISS} per chiudere la finestra)",
      touch = "Sei al controllo di un tank, che si trova attualmente su una\nbarca in mare. Usa il thumbstick sul lato sinistro dello\nschermo per guidare: spingi in avanti per accelerare, tira\nindietro per rallentare e fermarti.\n\nPiù spingi il thumbstick, più il tank va veloce.\nSpingi il thumbstick in avanti ora per far avanzare\nla barca verso la terraferma.\n(Tocca OK per chiudere questa finestra)",
      controller = "Sei al controllo di un tank, che si trova attualmente\nsu una barca in mare. Spingi lo stick sinistro in avanti\nper far avanzare il tank (e la barca) e tiralo indietro\nper rallentare e fermarti.\n\nPiù spingi lo stick sinistro, più il tank va veloce.\nSpingi lo stick sinistro in avanti ora per guidare\nla barca verso la terraferma.\n(Premi prima {DISMISS} per chiudere la finestra)",
    },
    t01 = {
      keyboard = "Davanti a te c'è un breve fiume che porta nell'entroterra. Tieni\npremuto il tasto {ACCEL} per guidare la barca fino alla fine\ndel fiume.\n\nQuando arrivi, continua a tenere premuto {ACCEL}. Il tank\nsbarcherà dalla barca, che resterà ormeggiata alla fine del fiume.",
      touch = "Davanti a te c'è un breve fiume che porta nell'entroterra. Spingi\nil thumbstick in avanti per guidare la barca fino alla fine\ndel fiume.\n\nQuando arrivi, continua a spingere il thumbstick\nin avanti. Il tank sbarcherà dalla barca e la\nbarca resterà ormeggiata alla fine del fiume.",
      controller = "Davanti a te c'è un breve fiume che porta nell'entroterra.\nSpingi lo stick sinistro in avanti per guidare la barca\nfino alla fine del fiume.\n\nQuando arrivi, continua a spingere lo stick in avanti.\nIl tank sbarcherà dalla barca e la barca resterà\normeggiata alla fine del fiume.",
    },
    t02 = {
      keyboard = "Sei ora sull'erba. Il tank si muove abbastanza\nvelocemente sull'erba.\n\nContinua a premere {ACCEL} per avanzare verso la foresta.",
      touch = "Sei ora sull'erba. Il tank si muove abbastanza\nvelocemente sull'erba.\n\nContinua a spingere il thumbstick in avanti per avanzare\nverso la foresta.",
      controller = "Sei ora sull'erba. Il tank si muove abbastanza\nvelocemente sull'erba.\n\nContinua a spingere lo stick sinistro in avanti\nper avanzare verso la foresta.",
    },
    t03 = {
      keyboard = "Sei ora nella foresta. Il tank si muove più\nlentamente nella foresta rispetto all'erba.\n\nContinua a premere {ACCEL} per avanzare verso la palude.",
      touch = "Sei ora nella foresta. Il tank si muove più\nlentamente nella foresta rispetto all'erba.\n\nContinua a spingere il thumbstick in avanti per avanzare\nverso la palude.",
      controller = "Sei ora nella foresta. Il tank si muove più\nlentamente nella foresta rispetto all'erba.\n\nContinua a spingere lo stick sinistro in avanti\nper avanzare verso la palude.",
    },
    t04 = {
      keyboard = "Sei ora nella palude. Il tank si muove molto\nlentamente nella palude. Per fortuna c'è una strada davanti.\n\nContinua a premere {ACCEL} per avanzare verso la strada.",
      touch = "Sei ora nella palude. Il tank si muove molto\nlentamente nella palude. Per fortuna c'è una strada davanti.\n\nContinua a spingere il thumbstick in avanti per avanzare\nverso la strada.",
      controller = "Sei ora nella palude. Il tank si muove molto\nlentamente nella palude. Per fortuna c'è una strada davanti.\n\nContinua a spingere lo stick sinistro in avanti\nper avanzare verso la strada.",
    },
    t05 = {
      keyboard = "Sei ora sulla strada. Il tank può muoversi molto\nvelocemente su strada e premendo {ACCEL} il tank\naccelerà.\n\nSe vai troppo veloce puoi premere {BRAKE}",
      touch = "Sei ora sulla strada. Il tank può muoversi molto\nvelocemente su strada e spingere il thumbstick tutto\nin avanti lo farà accelerare.\n\nSe vai troppo veloce, tira il thumbstick\nindietro per rallentare.",
      controller = "Sei ora sulla strada. Il tank può muoversi molto\nvelocemente su strada e spingere lo stick sinistro\ntutto in avanti lo farà accelerare.\n\nSe vai troppo veloce, tira lo stick sinistro\nindietro per rallentare.",
    },
    t06 = {
      keyboard = "Ci sono alcuni edifici davanti. Gli edifici sono ostacoli\nsolidi attraverso cui non puoi guidare, quindi dovrai\nfaeti strada nel labirinto fino all'altro lato.\n\nPer far girare il tank a sinistra premi {LEFT}\nPer far girare il tank a destra premi {RIGHT}",
      touch = "Ci sono alcuni edifici davanti. Gli edifici sono ostacoli\nsolidi attraverso cui non puoi guidare, quindi dovrai\nfarti strada nel labirinto fino all'altro lato.\n\nSpingi il thumbstick a sinistra o a destra per girare\nil tank in quella direzione.",
      controller = "Ci sono alcuni edifici davanti. Gli edifici sono ostacoli\nsolidi attraverso cui non puoi guidare, quindi dovrai\nfarti strada nel labirinto fino all'altro lato.\n\nSpingi lo stick sinistro a sinistra o a destra\nper girare il tank in quella direzione.",
    },
    t07 = "Questi oggetti rotondi con i cannoni rossi che sporgono sono\nbunker automatici. Sparano a qualsiasi tank\nche entra nel loro raggio d'azione.\n\nPer fortuna gli edifici offrono protezione dai\ncolpi, quindi i bunker non riusciranno a colpirti\na meno che tu non aspetti così a lungo che riescano a\nspararsi completamente attraverso gli edifici.",
    t08 = "La foresta offre protezione dai bunker, ma in\nmodo diverso. Oltre a rallentare molto il tank,\nguidare nella foresta limita anche la visibilità.\n\nQuando sei nella foresta puoi ancora vedere chiaramente\ntutto ciò che è fuori dalla foresta, ma i bunker (e\ngli altri giocatori) non possono vedere il tuo tank a meno che\nnon ti avvicini molto a loro.",
    t09 = "Allinea il tank con il centro della striscia di\nforesta e guida verso nord. Se vai dritto\ne non ti avvicini troppo ai bordi della foresta,\ni bunker non ti vedranno e non spareranno.\n\nNon uscire dalla foresta o sarai fatto a pezzi.",
    t10 = {
      keyboard = "C'è un altro modo semplice per sfuggire ai bunker:\nla velocità.\n\nAssicurati di essere allineato correttamente con il centro\ndella strada e tieni premuto {ACCEL} per passare a tutta\nvelocità davanti ai bunker.",
      touch = "C'è un altro modo semplice per sfuggire ai bunker:\nla velocità.\n\nAssicurati di essere allineato correttamente con il centro\ndella strada e spingi il thumbstick completamente in avanti per\npassare a tutta velocità davanti ai bunker.",
      controller = "C'è un altro modo semplice per sfuggire ai bunker:\nla velocità.\n\nAssicurati di essere allineato correttamente con il centro\ndella strada e spingi lo stick sinistro completamente\nin avanti per passare a tutta velocità davanti ai bunker.",
    },
    t11 = "Quando ti notano inizieranno a sparare, ma se\ncontinui dritto e non perdi la calma, sarai fuori\ndal loro raggio prima che i colpi possano colpirti.\n\nNon correre fuori strada nella palude o sarai un\nbersaglio facile per i bunker, e non riuscirai a\nmuoverti abbastanza velocemente per scappare.",
    t12 = "Davanti a te c'è una base di rifornimento. Le basi di rifornimento forniscono\nproiettili per sparare, mine per tendere trappole segrete\ne riparano la corazza del tuo tank fino alla resistenza piena\nse è stata danneggiata.\n\nFermati sulla base di rifornimento e carica il tuo tank\ncon proiettili.",
    t13 = "Sei sulla base di rifornimento. Il tuo tank ora\ncaricherà proiettili e mine, come indicato dai\ncontatori sulla destra. Davanti a te, oltre gli\nedifici, c'è un campo minato.",
    t14 = {
      keyboard = "Se guidi il tuo tank nel campo minato verrà\ndistrutto. Per fortuna puoi bonificare un campo minato\nsparando. Premi {FIRE} per sparare.\n\nSpara un varco attraverso l'edificio, poi fai cadere un\nproiettile al centro del campo minato per farlo\nesplodere. Quando fai saltare le mine si creerà un grande\ncratere nel terreno, attraverso cui puoi guidare,\nsebbene sia lento come guidare nella palude.",
      touch = "Se guidi il tuo tank nel campo minato verrà\ndistrutto. Per fortuna puoi bonificare un campo minato\nsparando. Tocca il pulsante di fuoco per sparare.\n\nSpara un varco attraverso l'edificio, poi fai cadere un\nproiettile al centro del campo minato per farlo\nesplodere. Quando fai saltare le mine si creerà un grande\ncratere nel terreno, attraverso cui puoi guidare,\nsebbene sia lento come guidare nella palude.",
      controller = "Se guidi il tuo tank nel campo minato verrà\ndistrutto. Per fortuna puoi bonificare un campo minato\nsparando. Premi {FIRE} per sparare.\n\nSpara un varco attraverso l'edificio, poi fai cadere un\nproiettile al centro del campo minato per farlo\nesplodere. Quando fai saltare le mine si creerà un grande\ncratere nel terreno, attraverso cui puoi guidare,\nsebbene sia lento come guidare nella palude.",
    },
    t15 = "Davanti c'è un altro campo minato. Fai lo stesso trucco:\nPortati sulla base di rifornimento, spara attraverso il\nmuro e fai cadere un proiettile al centro del campo minato per\nfarlo esplodere.\n\nQuesta volta però il campo minato è adiacente a un grande\nfiume e l'acqua inonderà i crateri. Puoi\nguidare il tank attraverso l'acqua come attraverso\ni crateri e la palude.",
    t16 = {
      keyboard = "WinBolo scorre automaticamente la finestra per mostrarti\nl'area intorno al tuo tank, ma puoi usare i tasti\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\nper modificare manualmente la vista scelta da WinBolo.\n\nPremi {SCROLL_UP} ora per avere una visuale migliore del\nfiume davanti a te.",
      touch = "WinBolo scorre automaticamente la finestra per mostrarti\nl'area intorno al tuo tank, ma puoi trascinare la mappa\ncon un dito per modificare manualmente la vista.\n\nTrascia la mappa verso il basso ora per portare il fiume\ndavanti a te nella visuale.",
      controller = "WinBolo scorre automaticamente la finestra per mostrarti\nl'area intorno al tuo tank, ma puoi spingere lo stick\ndestro per modificare manualmente la vista.\n\nSpingi lo stick destro verso l'alto ora per avere\nuna visuale migliore del fiume davanti a te.",
    },
    t17 = "Oltre a essere molto lenta, guidare nell'acqua ha\nun altro svantaggio: depaupera anche le\nscorte di proiettili e mine del tuo tank.\n\nQuando arrivi dall'altra parte il tuo tank sarà ancora\nfunzionante, ma avrai perso tutta la capacità offensiva.",
    t18 = {
      keyboard = "Questa volta saremo più furbi nel\nattraversare il fiume. Invece di guadarlo direttamente,\nconstruiremo un ponte.\n\nSeleziona la modalità raccolta alberi facendo clic sull'icona\ndella foresta (in alto a sinistra della finestra) e fai clic sulla foresta.\nIl tuo costruttore uscirà dal tank e raccoglierà un\nalbero per i materiali da costruzione.",
      touch = "Questa volta saremo più furbi nel\nattraversare il fiume. Invece di guadarlo direttamente,\nconstruiremo un ponte.\n\nSeleziona la modalità raccolta alberi toccando l'icona\ndella foresta (in alto a sinistra della finestra) e tocca la foresta.\nIl tuo costruttore uscirà dal tank e raccoglierà un\nalbero per i materiali da costruzione.",
      controller = "Questa volta saremo più furbi nell'attraversare il fiume.\nInvece di guadarlo direttamente, costruiremo un ponte.\n\nTieni premuto {BUILD_MODE} per passare alla modalità\ncursore di costruzione - lo stick destro muove ora un\ncursore invece della visuale. Premi {BUILD_TOOL} per\nselezionare la raccolta alberi (in alto a sinistra),\npunta il cursore su un albero e premi {BUILD_PLACE}.\nIl costruttore raccoglierà un albero per i materiali.",
    },
    t19 = {
      keyboard = "Dopo aver raccolto qualche albero, seleziona lo strumento\nstrada/ponte (il secondo pulsante dall'alto) e fai clic col mouse\nsul fiume alla fine della strada.\n\nIl tuo costruttore uscirà dal tank e costruirà un\nsegmento di ponte. Fai clic di nuovo per completare il ponte\ne poi attraversa fino all'altro lato.",
      touch = "Dopo aver raccolto qualche albero, seleziona lo strumento\nstrada/ponte (il secondo pulsante dall'alto) e tocca il\nfiume alla fine della strada.\n\nIl tuo costruttore uscirà dal tank e costruirà un\nsegmento di ponte. Tocca di nuovo per completare il ponte\ne poi attraversa fino all'altro lato.",
      controller = "Dopo aver raccolto qualche albero, premi {BUILD_TOOL}\nper selezionare lo strumento strada/ponte (il secondo\npulsante dall'alto), punta il cursore sul fiume alla\nfine della strada e premi {BUILD_PLACE}.\n\nIl tuo costruttore costruirà un segmento di ponte.\nPositiona di nuovo per completare il ponte, poi tieni\npremuto {BUILD_MODE} per uscire dalla modalità costruzione\ne attraversa fino all'altro lato.",
    },
    t20 = "Questo fiume è più largo dell'ultimo, e al centro è\ntroppo profondo per costruire un ponte. Se provi a\nguadarlo il tank affonderà sul fondo e verrà\ndistrutto.",
    t21 = {
      keyboard = "Per attraversare questo fiume dovrai costruire una barca. Le barche\nrichiedono cinque alberi, quindi fai clic sui cinque alberi per raccoglierli.\nPoi seleziona lo strumento muro/barca (il terzo pulsante dall'alto)\ne fai clic sull'acqua per costruire una barca.\n\nSe fai clic sulla terra costruirai un muro\ne dovrai raccogliere altri alberi per costruire la barca.\nSali sulla barca e attraversa il fiume fino all'altro lato.",
      touch = "Per attraversare questo fiume dovrai costruire una barca. Le barche\nrichiedono cinque alberi, quindi tocca i cinque alberi per raccoglierli.\nPoi seleziona lo strumento muro/barca (il terzo pulsante dall'alto)\ne tocca l'acqua per costruire una barca.\n\nSe tocchi la terra costruirai un muro\ne dovrai raccogliere altri alberi per costruire la barca.\nSali sulla barca e attraversa il fiume fino all'altro lato.",
      controller = "Per attraversare questo fiume dovrai costruire una barca.\nLe barche richiedono cinque alberi: punta su cinque\nalberi e premi {BUILD_PLACE} su ognuno. Poi premi\n{BUILD_TOOL} per selezionare lo strumento muro/barca\n(il terzo pulsante dall'alto) e posizionalo sull'acqua.\n\nSe posizioni sulla terra costruirai un muro e\ndovrai raccogliere altri alberi. Sali sulla barca\ne attraversa il fiume fino all'altro lato.",
    },
    t22 = "C'è un bunker davanti e alcune basi di rifornimento\nper fornirti munizioni. Raccogli proiettili dalla\nbase di rifornimento e attacca il bunker.\n\nSpareranno a loro volta, ma se il tuo tank viene colpito puoi facilmente\ntornare alla base di rifornimento per riparare la corazza\nfino alla piena resistenza.",
    t23 = "Con pazienza e cautela dovresti riuscire a distruggere questo\nbunker. Usa le abilità che hai imparato per renderlo più facile:\n\nNasconditi nella foresta in modo che il bunker non ti veda,\nraccogli alberi, costruisci strade per far muovere più\nvelocemente il tank e costruisci edifici per proteggerti dai\ncolpi del bunker.",
    t24 = "Dopo aver sconfitto questo bunker, la tua sfida finale\ndel tutorial è eliminare i due bunker che sorvegliano l'uscita,\ncostruire una barca e fuggire.\n\nFai molta attenzione al livello di corazza del tuo tank:\nse il tuo tank viene distrutto, ti verrà assegnato un\nnuovo tank nelle vicinanze così puoi riprovare.",
    t25 = "Congratulazioni. Hai completato il tutorial di WinBolo.\n\nSei pronto a giocare. Unisciti a una partita online,\nospitane una per i tuoi amici o esercitati contro i bot.",
    respawn1 = "Il tuo tank è stato distrutto, ma non devi ricominciare\ndall'inizio il tutorial. Ti è stato assegnato un nuovo\ntank appena oltre l'acqua, dove sei sbarcato. Torna\nin azione e completa la tua fuga.",
  },
}

TEXT["ja"] = {
  pop = {
    start01 = {
      keyboard = "WinBolo チュートリアルへようこそ。この島では WinBolo の\n基本原則を紹介し、一つずつ案内します。各新しい原則は\nこのようなウィンドウで説明されます。\n\n各メッセージを読んだ後、\n{DISMISS} を押して続行してください。",
      touch = "WinBolo チュートリアルへようこそ。この島では WinBolo の\n基本原則を紹介し、一つずつ案内します。各新しい原則は\nこのようなウィンドウで説明されます。\n\n各メッセージを読んだ後、\n「OK」ボタンをタップして続行できます。",
    },
    start02 = "WinBolo は Stuart Cheshire の classic Macintosh ネットワークゲーム Bolo の唯一の公認クローンです。\n公式ウェブサイトで戦略ヒントやその他の情報が見つかります：\n\nhttps://www.winbolo.com\n\nReddit や Discord にも参加してください。\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "あなたは海上のボートに乗ったタンクを操縦しています。{ACCEL} を押すと\nタンク（とボート）が前進し、{BRAKE} を押すと\n減速・停止します。\n\nタンクが望む速度になるまでキーを押し続け、\n達したら離してください。今すぐ {ACCEL} を押して\nボートを陸に向けて進めてください。\n（ウィンドウを閉じるにはまず {DISMISS} を押してください）",
      touch = "あなたは海上のボートに乗ったタンクを操縦しています。画面左側の\nスティックで操縦してください：前に押すと加速し、\n引くと減速・停止します。\n\nスティックをより多く押すほどタンクが速くなります。\n今すぐスティックを前に押して\nボートを陸に向けて進めてください。\n（このウィンドウを閉じるにはまず OK をタップ）",
      controller = "あなたは海上のボートに乗ったタンクを操縦しています。\n左スティックを前に押すとタンク（とボート）が前進し、\n引くと減速・停止します。\n\n左スティックをより多く押すほどタンクが速くなります。\n今すぐ左スティックを前に押して\nボートを陸に向けて進めてください。\n（ウィンドウを閉じるにはまず {DISMISS} を押してください）",
    },
    t01 = {
      keyboard = "前方に内陸へ続く短い川があります。{ACCEL} キーを押し続けて\nボートを川の終点まで進めてください。\n\n到着したら、{ACCEL} を押し続けてください。タンクがボートから\n上陸し、ボートは川の終点に係留されます。",
      touch = "前方に内陸へ続く短い川があります。\nスティックを前に押してボートを川の終点まで\n進めてください。\n\n到着したら、スティックを前に押し続けてください。\nタンクがボートから上陸し、ボートは川の終点に係留されます。",
      controller = "前方に内陸へ続く短い川があります。左スティックを\n前に押してボートを川の終点まで進めてください。\n\n到着したら、スティックを前に押し続けてください。\nタンクがボートから上陸し、ボートは川の終点に\n係留されます。",
    },
    t02 = {
      keyboard = "草地に上陸しました。草地ではタンクはかなり\n速く動けます。\n\n{ACCEL} を押し続けて森に向かって進んでください。",
      touch = "草地に上陸しました。草地ではタンクは\nかなり速く動けます。\n\nスティックを前に押し続けて\n森に向かって進んでください。",
      controller = "草地に上陸しました。草地ではタンクは\nかなり速く動けます。\n\n左スティックを前に押し続けて\n森に向かって進んでください。",
    },
    t03 = {
      keyboard = "森の中に入りました。森の中では草地よりも\nタンクの動きが遅くなります。\n\n{ACCEL} を押し続けて沼地に向かって進んでください。",
      touch = "森の中に入りました。森の中では草地より\nタンクの動きが遅くなります。\n\nスティックを前に押し続けて\n沼地に向かって進んでください。",
      controller = "森の中に入りました。森の中では草地より\nタンクの動きが遅くなります。\n\n左スティックを前に押し続けて\n沼地に向かって進んでください。",
    },
    t04 = {
      keyboard = "沼地に入りました。沼地ではタンクの動きは非常に\n遅くなります。幸い、前方に道路があります。\n\n{ACCEL} を押し続けて道路に向かって進んでください。",
      touch = "沼地に入りました。沼地ではタンクの動きは\n非常に遅くなります。幸い、前方に道路があります。\n\nスティックを前に押し続けて\n道路に向かって進んでください。",
      controller = "沼地に入りました。沼地ではタンクの動きは\n非常に遅くなります。幸い、前方に道路があります。\n\n左スティックを前に押し続けて\n道路に向かって進んでください。",
    },
    t05 = {
      keyboard = "道路に出ました。道路ではタンクは非常に\n速く動け、{ACCEL} を押すとさらに加速します。\n\n速くなりすぎた場合は {BRAKE} を押してください。",
      touch = "道路に出ました。道路ではタンクは非常に速く動け、\nスティックを全前方に押すと加速します。\n\n速くなりすぎた場合は、\nスティックを引いて減速してください。",
      controller = "道路に出ました。道路ではタンクは非常に速く動け、\n左スティックを全前方に押すと加速します。\n\n速くなりすぎた場合は、\n左スティックを引いて減速してください。",
    },
    t06 = {
      keyboard = "前方にいくつかの建物があります。建物は固い\n障害物で、通り抜けることはできません。迷路を\n抜けて反対側へ進んでください。\n\nタンクを左に曲げるには {LEFT} を押してください。\nタンクを右に曲げるには {RIGHT} を押してください。",
      touch = "前方にいくつかの建物があります。建物は固い\n障害物で、通り抜けることはできません。迷路を\n抜けて反対側へ進んでください。\n\nスティックを左右に押してタンクを\nその方向に向けてください。",
      controller = "前方にいくつかの建物があります。建物は固い\n障害物で、通り抜けることはできません。迷路を\n抜けて反対側へ進んでください。\n\n左スティックを左右に押してタンクを\nその方向に向けてください。",
    },
    t07 = "赤い砲塔が突き出た丸い物体は\n自動トーチカです。射程内に入ったタンクに\n向かって射撃してきます。\n\n幸い、建物は射撃から身を守ってくれます。そのため、\nトーチカが建物を完全に撃ち抜くほど\n長時間待機しない限り、当たることはありません。",
    t08 = "森もトーチカから身を守ってくれますが、その方法は\n異なります。タンクの速度を大幅に低下させるだけでなく、\n森を走行すると視界も制限されます。\n\n森の中にいる場合、外の様子は見えますが、トーチカ（や\n他のプレイヤー）はかなり近づかない限り、\nあなたのタンクを見つけることができません。",
    t09 = "タンクを森の帯の中央に向けて真北に進んでください。\nまっすぐ進んで森の端に近づきすぎなければ、\nトーチカはあなたを見つけられず、射撃してきません。\n\n森の外に出ると吹き飛ばされます。",
    t10 = {
      keyboard = "トーチカから逃れるもう一つのシンプルな方法は\nスピードです。\n\n道路の中央に正しく位置を合わせ、{ACCEL} を押して\n全速でトーチカの前を通り抜けてください。",
      touch = "トーチカから逃れるもう一つのシンプルな方法は\nスピードです。\n\n道路の中央に正しく位置を合わせ、\nスティックを完全に前に押して\nトーチカの前を全速で通り抜けてください。",
      controller = "トーチカから逃れるもう一つのシンプルな方法は\nスピードです。\n\n道路の中央に正しく位置を合わせ、\n左スティックを完全に前に押して\nトーチカの前を全速で通り抜けてください。",
    },
    t11 = "気づかれると射撃が始まりますが、まっすぐ進み続け\n焦らなければ、砲弾が当たる前に射程外に出られます。\n\n道路を外れて沼地に入ると格好の標的になり、\n素早く逃げることができなくなります。",
    t12 = "前方に補給基地があります。補給基地では\n射撃用の砲弾、罠を仕掛けるための地雷が補給でき、\nまた損傷したタンクの装甲を完全に修復できます。\n\n補給基地に停車してタンクに砲弾を補充してください。",
    t13 = "補給基地の上にいます。タンクに砲弾と地雷が\n補充されます。右側のインジケーターで確認できます。\n前方の建物の向こうに地雷原があります。",
    t14 = {
      keyboard = "地雷原にタンクを走らせると破壊されます。\n幸い、射撃で地雷原を安全にできます。{FIRE} を押して射撃してください。\n\n建物に穴を開けてから、地雷原の中央に砲弾を着弾させて\n爆破してください。地雷を爆発させると大きなクレーターができ、\n走行できますが、沼地と同じくらい遅くなります。",
      touch = "地雷原にタンクを走らせると破壊されます。\n幸い、射撃で地雷原を安全にできます。射撃ボタンをタップして射撃してください。\n\n建物に穴を開けてから、地雷原の中央に砲弾を着弾させて\n爆破してください。地雷を爆発させると大きなクレーターができ、\n走行できますが、沼地と同じくらい遅くなります。",
      controller = "地雷原にタンクを走らせると破壊されます。\n幸い、射撃で地雷原を安全にできます。{FIRE} を押して射撃してください。\n\n建物に穴を開けてから、地雷原の中央に砲弾を着弾させて\n爆破してください。地雷を爆発させると大きなクレーターができ、\n走行できますが、沼地と同じくらい遅くなります。",
    },
    t15 = "さらに地雷原があります。同じ要領で:\n補給基地に移動し、壁を撃ち抜き、地雷原の中央に\n砲弾を当てて爆破させてください。\n\n今回は地雷原が大きな川に隣接しており、\n水がクレーターに流れ込みます。クレーターや沼地と同様に\n水の中も走行できます。",
    t16 = {
      keyboard = "WinBolo はタンクの周囲を自動的に表示しますが、\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\nキーを使って手動で視点を変更できます。\n\n前方の川をよく見るために今すぐ {SCROLL_UP} を押してください。",
      touch = "WinBolo はタンクの周囲を自動的に表示しますが、\n指でマップをドラッグして手動で視点を変更できます。\n\n前方の川を視野に入れるために\n今すぐマップを下にドラッグしてください。",
      controller = "WinBolo はタンクの周囲を自動的に表示しますが、\n右スティックを押して手動で視点を変更できます。\n\n前方の川をよく見るために\n今すぐ右スティックを上に押してください。",
    },
    t17 = "水中の走行は非常に遅いだけでなく、\nもう一つの問題があります：タンクの砲弾と地雷の\n備蓄も減少します。\n\n反対側に到達してもタンクは動作しますが、\n攻撃能力はすべて失われます。",
    t18 = {
      keyboard = "今回は川を渡る別の方法を使います。\n水の中を直接走行する代わりに、橋を建設します。\n\nウィンドウ左上の森アイコンをクリックして木の伐採モードを選択し、\n森をクリックしてください。ビルダーがタンクから走り出て\n建設資材として木を 1 本収穫します。",
      touch = "今回は川を渡る別の方法を使います。\n水の中を直接走行する代わりに、橋を建設します。\n\nウィンドウ左上の森アイコンをタップして木の伐採モードを選択し、\n森をタップしてください。ビルダーがタンクから走り出て\n建設資材として木を 1 本収穫します。",
      controller = "今回は川を渡る別の方法を使います。\n水の中を直接走行する代わりに、橋を建設します。\n\n{BUILD_MODE} を引いてスクロールから建設モードへ切り替えます\n- 右スティックが視点の代わりに建設カーソルを動かします。\n{BUILD_TOOL} を押して木の伐採モードを選択し（左上）、\nカーソルを木に合わせて {BUILD_PLACE} を押してください。\nビルダーが木を 1 本収穫します。",
    },
    t19 = {
      keyboard = "木を伐採したら、道路/橋ツール\n（上から 2 番目のボタン）を選択し、\n道路の終端で川の上をクリックしてください。\n\nビルダーが走り出て橋のセグメントを 1 つ建設します。\nもう一度クリックして橋を完成させ、反対側へ走行してください。",
      touch = "木を伐採したら、道路/橋ツール\n（上から 2 番目のボタン）を選択し、\n道路の終端で川の上をタップしてください。\n\nビルダーが走り出て橋のセグメントを 1 つ建設します。\nもう一度タップして橋を完成させ、反対側へ走行してください。",
      controller = "木を伐採したら、{BUILD_TOOL} を押して道路/橋ツール\n（上から 2 番目のボタン）を選択し、建設カーソルを\n道路の終端の川に合わせて {BUILD_PLACE} を押してください。\n\nビルダーが橋のセグメントを 1 つ建設します。\nもう一度配置して橋を完成させ、{BUILD_MODE} を引いて\n建設モードを終了し、反対側へ走行してください。",
    },
    t20 = "この川は前の川より広く、中央は深すぎて橋を\n建設できません。タンクで直接渡ろうとすると\n川底に沈んで破壊されます。",
    t21 = {
      keyboard = { "この川を渡るにはボートを作る必要があります。ボートには\n5 本の木が必要です。5 本の木をクリックして伐採してください。\n次に壁/ボートツール（上から 3 番目のボタン）を選択し、\n水の上をクリックしてボートを作ってください。", "陸をクリックすると壁が作られてしまうので、\nボートを作るためにさらに木を伐採する必要があります。\nボートに乗って川を渡ってください。" },
      touch = "この川を渡るにはボートを作る必要があります。ボートには\n5 本の木が必要です。5 本の木をタップして伐採してください。\n次に壁/ボートツール（上から 3 番目のボタン）を選択し、\n水の上をタップしてボートを作ってください。\n\n陸をタップすると壁が作られてしまうので、\nボートを作るためにさらに木を伐採する必要があります。\nボートに乗って川を渡ってください。",
      controller = { "この川を渡るにはボートを作る必要があります。ボートには\n5 本の木が必要です。5 本の木に照準を合わせて\n各木に {BUILD_PLACE} を押して伐採してください。次に\n{BUILD_TOOL} を押して壁/ボートツール（上から 3 番目の\nボタン）を選択し、水の上に配置してください。", "陸に配置すると壁が作られるので、ボートを作るには\nさらに木を伐採する必要があります。\nボートに乗って川を渡ってください。" },
    },
    t22 = "前方にトーチカがあり、弾薬を提供する補給基地もあります。\n補給基地から砲弾を補充してトーチカを攻撃してください。\n\n反撃してきますが、タンクが被弾しても\n補給基地に戻れば装甲を完全に修復できます。",
    t23 = "忍耐と慎重さでこのトーチカを破壊できるはずです。\n以下のスキルを使って楽にしてください：\n\n森に隠れてトーチカから見えないようにし、\n木を伐採して道路を建設してタンクを速く動かし、\n建物を建ててトーチカの射撃から身を守ってください。",
    t24 = "このトーチカを倒したら、チュートリアルの最終課題は\n出口を守る 2 つのトーチカを排除し、ボートを作って\n脱出することです。\n\nタンクの装甲レベルに十分注意してください -\nタンクが破壊されても、近くに新しいタンクが\n与えられるのでもう一度試せます。",
    t25 = "おめでとうございます。WinBolo の\nチュートリアルを完了しました。\n\nプレイの準備ができました。オンラインで対戦するか、\n友達のためにゲームを主催するか、\nボット相手に練習してみましょう。",
    respawn1 = "タンクが破壊されましたが、チュートリアルをやり直す\n必要はありません。上陸した場所の近く、水の向こうに\n新しいタンクが用意されています。\n戻って脱出を完了させてください。",
  },
}

TEXT["ko"] = {
  pop = {
    start01 = {
      keyboard = "WinBolo 튜토리얼에 오신 것을 환영해요. 이 섬은\nWinBolo의 기본 원리를 소개하고\n하나씩 안내해 줘요. 각각의 새 원리는\n이것과 같은 창에 설명될 거예요.\n\n각 메시지를 읽은 후,\n{DISMISS} 키를 눌러 진행하세요.",
      touch = "WinBolo 튜토리얼에 오신 것을 환영해요. 이 섬은\nWinBolo의 기본 원리를 소개하고\n하나씩 안내해 줘요. 각각의 새 원리는\n이것과 같은 창에 설명될 거예요.\n\n각 메시지를 읽은 후 \"확인\" 버튼을\n탭해 진행할 수 있어요.",
    },
    start02 = "WinBolo는 Stuart Cheshire의 클래식 Macintosh 네트워크 게임 Bolo의 유일한 공인 클론이에요.\n공식 웹사이트에서 전략 힌트와 다른 정보를 찾을 수 있어요:\n\nhttps://www.winbolo.com\n\nReddit과 Discord에서 함께해요\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "현재 바다 위 보트에 탱크를 조종 중이에요. {ACCEL} 키를 눌러\n탱크(와 보트)를 앞으로 전진시키고, {BRAKE} 키를 눌러\n속도를 줄이고 정지시키세요.\n\n탱크가 원하는 속도에 도달할 때까지 키를 길게 누른 후\n놓으세요. 지금 {ACCEL} 키를 눌러\n보트를 육지 쪽으로 전진시키세요.\n({DISMISS} 키를 먼저 눌러 창을 닫으세요)",
      touch = "현재 바다 위 보트에 탱크를 조종 중이에요. 화면 왼쪽의\n썸스틱으로 조종하세요: 앞으로 밀면 가속되고\n뒤로 당기면 속도가 줄어 정지해요.\n\n썸스틱을 더 세게 밀수록 탱크가\n빠르게 움직여요. 지금 썸스틱을 앞으로 밀어\n보트를 육지 쪽으로 전진시키세요.\n(이 창을 닫으려면 먼저 확인을 탭하세요)",
      controller = "현재 바다 위 보트에 탱크를 조종 중이에요.\n왼쪽 스틱을 앞으로 밀어 탱크(와 보트)를\n전진시키고, 뒤로 당겨 속도를 줄이고 정지시키세요.\n\n왼쪽 스틱을 더 세게 밀수록 탱크가\n빠르게 움직여요. 지금 왼쪽 스틱을 앞으로 밀어\n보트를 육지 쪽으로 전진시키세요.\n({DISMISS}를 먼저 눌러 창을 닫으세요)",
    },
    t01 = {
      keyboard = "앞에 육지로 이어지는 짧은 강이 있어요. {ACCEL} 키를\n길게 눌러 보트를 강 끝까지 몰아가세요.\n\n도착하면 {ACCEL} 키를 계속 누르고 있으세요. 탱크가\n보트에서 내리고 보트는 강 끝에 정박해요.",
      touch = "앞에 육지로 이어지는 짧은 강이 있어요.\n썸스틱을 앞으로 밀어 보트를 강\n끝까지 몰아가세요.\n\n도착하면 썸스틱을 계속 앞으로 밀고 있으세요.\n탱크가 보트에서 내리고 보트는 강 끝에\n정박해요.",
      controller = "앞에 육지로 이어지는 짧은 강이 있어요.\n왼쪽 스틱을 앞으로 밀어 보트를 강\n끝까지 몰아가세요.\n\n도착하면 스틱을 계속 앞으로 밀고 있으세요.\n탱크가 보트에서 내리고 보트는 강 끝에\n정박해요.",
    },
    t02 = {
      keyboard = "이제 풀밭 위에 있어요. 탱크는 풀밭에서\n꽤 빠르게 움직여요.\n\n{ACCEL} 키를 계속 눌러 숲으로 이동하세요.",
      touch = "이제 풀밭 위에 있어요. 탱크는 풀밭에서\n꽤 빠르게 움직여요.\n\n썸스틱을 앞으로 계속 밀어 숲으로\n이동하세요.",
      controller = "이제 풀밭 위에 있어요. 탱크는 풀밭에서\n꽤 빠르게 움직여요.\n\n왼쪽 스틱을 앞으로 계속 밀어 숲으로\n이동하세요.",
    },
    t03 = {
      keyboard = "이제 숲 안에 있어요. 탱크는 숲에서\n풀밭보다 더 느리게 움직여요.\n\n{ACCEL} 키를 계속 눌러 늪으로 이동하세요.",
      touch = "이제 숲 안에 있어요. 탱크는 숲에서\n풀밭보다 더 느리게 움직여요.\n\n썸스틱을 앞으로 계속 밀어 늪으로\n이동하세요.",
      controller = "이제 숲 안에 있어요. 탱크는 숲에서\n풀밭보다 더 느리게 움직여요.\n\n왼쪽 스틱을 앞으로 계속 밀어 늪으로\n이동하세요.",
    },
    t04 = {
      keyboard = "이제 늪 안에 있어요. 탱크는 늪에서 매우\n느리게 움직여요. 다행히 앞에 도로가 있어요.\n\n{ACCEL} 키를 계속 눌러 도로로 이동하세요.",
      touch = "이제 늪 안에 있어요. 탱크는 늪에서\n매우 느리게 움직여요. 다행히 앞에 도로가 있어요.\n\n썸스틱을 앞으로 계속 밀어 도로로\n이동하세요.",
      controller = "이제 늪 안에 있어요. 탱크는 늪에서\n매우 느리게 움직여요. 다행히 앞에 도로가 있어요.\n\n왼쪽 스틱을 앞으로 계속 밀어 도로로\n이동하세요.",
    },
    t05 = {
      keyboard = "이제 도로 위에 있어요. 탱크는 도로에서 매우\n빠르게 움직이며 {ACCEL} 키를 누르면 가속돼요.\n\n너무 빠르다 싶으면 {BRAKE} 키를 누르세요",
      touch = "이제 도로 위에 있어요. 탱크는 도로에서\n매우 빠르게 움직이며 썸스틱을 끝까지\n밀면 가속돼요.\n\n너무 빠르다 싶으면 썸스틱을\n뒤로 당겨 속도를 줄이세요.",
      controller = "이제 도로 위에 있어요. 탱크는 도로에서\n매우 빠르게 움직이며 왼쪽 스틱을 끝까지\n밀면 가속돼요.\n\n너무 빠르다 싶으면 왼쪽 스틱을\n뒤로 당겨 속도를 줄이세요.",
    },
    t06 = {
      keyboard = "앞에 건물들이 있어요. 건물은 뚫고 지나갈 수 없는\n장애물이에요. 미로를 찾아 반대편으로\n빠져나가야 해요.\n\n탱크를 왼쪽으로 돌리려면 {LEFT} 키를 누르세요\n탱크를 오른쪽으로 돌리려면 {RIGHT} 키를 누르세요",
      touch = "앞에 건물들이 있어요. 건물은 뚫고 지나갈 수 없는\n장애물이에요. 미로를 찾아 반대편으로\n빠져나가야 해요.\n\n썸스틱을 왼쪽 또는 오른쪽으로 밀어\n해당 방향으로 탱크를 돌리세요.",
      controller = "앞에 건물들이 있어요. 건물은 뚫고 지나갈 수 없는\n장애물이에요. 미로를 찾아 반대편으로\n빠져나가야 해요.\n\n왼쪽 스틱을 왼쪽 또는 오른쪽으로 밀어\n해당 방향으로 탱크를 돌리세요.",
    },
    t07 = "빨간 총이 돌출된 이 둥근 물체들은\n자동 토치카예요. 사거리 내에 들어오는 탱크에\n발사해요.\n\n다행히 건물이 총알을 막아줘요. 건물을 완전히\n뚫을 만큼 오래 기다리지 않으면\n토치카는 여러분을 맞출 수 없어요.",
    t08 = "숲도 토치카로부터 보호해 주지만,\n방식이 달라요. 탱크를 많이 느리게 할 뿐만 아니라,\n숲을 통과하면 시야도 제한돼요.\n\n숲 안에 있으면 숲 밖은 선명하게 보이지만,\n토치카(와 다른 플레이어)는 매우 가까이 오지 않으면\n여러분의 탱크를 볼 수 없어요.",
    t09 = "탱크를 숲 중앙과 일직선으로 맞추고\n정북 방향으로 직진하세요. 직선으로 가서\n숲 가장자리에 너무 가까이 가지 않으면,\n토치카는 여러분을 발견하지 못하고 발사하지 않아요.\n\n숲 밖으로 나가면 박살날 거예요.",
    t10 = {
      keyboard = "토치카에서 벗어나는 또 다른 간단한 방법이 있어요 -\n바로 속도예요.\n\n도로 중앙과 정확히 일직선으로 맞추고,\n{ACCEL} 키를 눌러 전속력으로 토치카를 지나치세요.",
      touch = "토치카에서 벗어나는 또 다른 간단한 방법이 있어요 -\n바로 속도예요.\n\n도로 중앙과 정확히 일직선으로 맞추고,\n썸스틱을 끝까지 밀어 전속력으로\n토치카를 지나치세요.",
      controller = "토치카에서 벗어나는 또 다른 간단한 방법이 있어요 -\n바로 속도예요.\n\n도로 중앙과 정확히 일직선으로 맞추고,\n왼쪽 스틱을 끝까지 밀어 전속력으로\n토치카를 지나치세요.",
    },
    t11 = "발견되면 발사하기 시작하지만,\n직진을 유지하고 당황하지 않으면\n총알이 맞기 전에 사거리를 벗어날 거예요.\n\n습지 쪽 도로를 벗어나지 마세요. 그러면\n토치카의 좋은 표적이 되고,\n빠져나갈 만큼 빠르게 움직일 수 없어요.",
    t12 = "앞에 보급기지가 있어요. 보급기지는 발사할 포탄과\n비밀 함정을 위한 지뢰를 제공하고,\n손상된 탱크의 장갑을 완전히 수리해 줘요.\n\n보급기지 위에 정차하고 탱크에 포탄을\n가득 채우세요.",
    t13 = "지금 보급기지 위에 있어요. 탱크에 포탄과\n지뢰가 가득 채워질 거예요. 오른쪽 표시기에서\n확인할 수 있어요. 앞 건물 너머에\n지뢰밭이 있어요.",
    t14 = {
      keyboard = "탱크를 지뢰밭으로 몰면 파괴돼요. 다행히\n발사로 지뢰밭을 제거할 수 있어요. {FIRE} 키를 눌러 발사하세요.\n\n건물에 구멍을 뚫은 다음 지뢰밭 한가운데에\n포탄을 떨어뜨려 폭파하세요.\n지뢰가 터지면 큰 분화구가 생기고,\n늪처럼 느리지만 통과할 수 있어요.",
      touch = "탱크를 지뢰밭으로 몰면 파괴돼요. 다행히\n발사로 지뢰밭을 제거할 수 있어요. 발사 버튼을 탭해 발사하세요.\n\n건물에 구멍을 뚫은 다음 지뢰밭 한가운데에\n포탄을 떨어뜨려 폭파하세요.\n지뢰가 터지면 큰 분화구가 생기고,\n늪처럼 느리지만 통과할 수 있어요.",
      controller = "탱크를 지뢰밭으로 몰면 파괴돼요. 다행히\n발사로 지뢰밭을 제거할 수 있어요. {FIRE} 키를 눌러 발사하세요.\n\n건물에 구멍을 뚫은 다음 지뢰밭 한가운데에\n포탄을 떨어뜨려 폭파하세요.\n지뢰가 터지면 큰 분화구가 생기고,\n늪처럼 느리지만 통과할 수 있어요.",
    },
    t15 = "앞에 또 다른 지뢰밭이 있어요. 같은 방법을 반복하세요:\n보급기지로 이동해 벽에 구멍을 뚫고\n지뢰밭 한가운데에 포탄을 떨어뜨려 폭파하세요.\n\n이번에는 지뢰밭이 큰 강 옆에 있어서\n분화구에 물이 차요. 분화구와 늪처럼\n물속을 탱크로 통과할 수 있어요.",
    t16 = {
      keyboard = "WinBolo는 탱크 주변 지역을 자동으로 스크롤하지만\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\n키로 보기를 수동으로 변경할 수 있어요.\n\n지금 {SCROLL_UP} 키를 눌러 앞에 있는\n강을 더 잘 보세요.",
      touch = "WinBolo는 탱크 주변 지역을 자동으로 스크롤하지만\n손가락으로 맵을 드래그해 보기를\n수동으로 변경할 수 있어요.\n\n지금 맵을 아래로 드래그해 앞에 있는\n강을 화면에 가져오세요.",
      controller = "WinBolo는 탱크 주변 지역을 자동으로 스크롤하지만\n오른쪽 스틱을 밀어 보기를 수동으로 변경할 수 있어요.\n\n지금 오른쪽 스틱을 위로 밀어 앞에 있는\n강을 더 잘 보세요.",
    },
    t17 = "물속을 통과하면 매우 느릴 뿐만 아니라\n또 다른 단점이 있어요: 탱크의 포탄과\n지뢰 보급량이 줄어들어요.\n\n반대편에 도착해도 탱크는 작동하지만\n모든 공격 능력을 잃게 돼요.",
    t18 = {
      keyboard = "이번에는 강을 건너는 더 영리한 방법을 써볼 거예요.\n그냥 물속을 통과하는 대신 다리를 건설할 거예요.\n\n숲 아이콘 (윈도우 왼쪽 상단)을 클릭해\n나무 벌채 모드를 선택하고 숲을 클릭하세요.\n건설자가 탱크에서 뛰어나와 건설 재료로 쓸\n나무 하나를 수확해요.",
      touch = "이번에는 강을 건너는 더 영리한 방법을 써볼 거예요.\n그냥 물속을 통과하는 대신 다리를 건설할 거예요.\n\n숲 아이콘 (윈도우 왼쪽 상단)을 탭해\n나무 벌채 모드를 선택하고 숲을 탭하세요.\n건설자가 탱크에서 뛰어나와 건설 재료로 쓸\n나무 하나를 수확해요.",
      controller = "이번에는 강을 건너는 더 영리한 방법을 써볼 거예요.\n그냥 물속을 통과하는 대신 다리를 건설할 거예요.\n\n{BUILD_MODE}를 당겨 스크롤에서 건설 모드로 전환하세요\n- 오른쪽 스틱이 화면 대신 건설 커서를 움직여요.\n{BUILD_TOOL}을 눌러 나무 벌채 모드를 선택하고 (왼쪽 상단),\n커서를 나무에 맞춰 {BUILD_PLACE}를 누르세요.\n건설자가 탱크에서 뛰어나와 나무 하나를 수확해요.",
    },
    t19 = {
      keyboard = "나무를 수확한 후 도로/다리 도구\n(위에서 두 번째 버튼)를 선택하고 도로 끝\n강 위를 마우스로 클릭하세요.\n\n건설자가 뛰어나와 다리를 한 구간 건설해요.\n다시 클릭해 다리를 완성하고\n반대편으로 건너가세요.",
      touch = "나무를 수확한 후 도로/다리 도구\n(위에서 두 번째 버튼)를 선택하고 도로 끝\n강 위를 탭하세요.\n\n건설자가 뛰어나와 다리를 한 구간 건설해요.\n다시 탭해 다리를 완성하고\n반대편으로 건너가세요.",
      controller = "나무를 수확한 후 {BUILD_TOOL}을 눌러 도로/다리 도구\n(위에서 두 번째 버튼)를 선택하고, 건설 커서를 도로 끝\n강 위에 맞춰 {BUILD_PLACE}를 누르세요.\n\n건설자가 뛰어나와 다리를 한 구간 건설해요.\n다시 배치해 다리를 완성하고, {BUILD_MODE}를 당겨\n건설 모드를 종료한 후 반대편으로 건너가세요.",
    },
    t20 = "이 강은 이전 강보다 넓고 중앙은 너무 깊어서\n다리를 건설할 수 없어요. 그냥 통과하려고 하면\n탱크가 가라앉아 파괴될 거예요.",
    t21 = {
      keyboard = "이 강을 건너려면 보트를 건조해야 해요. 보트에는\n나무 다섯 그루가 필요해요. 나무 다섯 그루를 클릭해\n수확하세요. 그런 다음 벽/보트 도구 (위에서 세 번째\n버튼)를 선택하고 물 위를 클릭해 보트를 건조하세요.\n\n땅을 클릭하면 보트 대신 벽이 생겨요.\n보트를 만들려면 나무를 더 수확해야 해요.\n보트에 탑승해 강을 건너세요.",
      touch = "이 강을 건너려면 보트를 건조해야 해요. 보트에는\n나무 다섯 그루가 필요해요. 나무 다섯 그루를 탭해\n수확하세요. 그런 다음 벽/보트 도구 (위에서 세 번째\n버튼)를 선택하고 물 위를 탭해 보트를 건조하세요.\n\n땅을 탭하면 보트 대신 벽이 생겨요.\n보트를 만들려면 나무를 더 수확해야 해요.\n보트에 탑승해 강을 건너세요.",
      controller = "이 강을 건너려면 보트를 건조해야 해요. 보트에는\n나무 다섯 그루가 필요해요. 다섯 그루에 조준해\n각 나무에 {BUILD_PLACE}를 눌러 수확하세요. 그런 다음\n{BUILD_TOOL}을 눌러 벽/보트 도구 (위에서 세 번째\n버튼)를 선택하고 물 위에 배치하세요.\n\n땅에 배치하면 보트 대신 벽이 생겨요.\n보트를 만들려면 나무를 더 수확해야 해요.\n보트에 탑승해 강을 건너세요.",
    },
    t22 = "앞에 토치카가 있고, 탄약을 보급할 보급기지가\n몇 곳 있어요. 보급기지에서 포탄을 모은 후\n토치카를 공격하세요.\n\n반격하겠지만, 탱크가 맞으면 쉽게\n보급기지로 돌아가 장갑을 완전히 수리할 수 있어요.",
    t23 = "인내심과 신중함으로 이 토치카를 파괴할 수 있어요.\n배운 기술을 활용해 더 쉽게 만드세요:\n\n숲에 숨어 토치카가 보지 못하게 하고,\n나무를 수확해 도로를 건설해 탱크를 더 빠르게 하고,\n토치카 포탄을 막을 건물을 지으세요.",
    t24 = "이 토치카를 물리친 후, 튜토리얼의 마지막 과제는\n출구를 지키는 두 토치카를 제거하고\n보트를 건조해 탈출하는 거예요.\n\n탱크의 장갑 수준을 잘 확인하세요 -\n탱크가 파괴되면 근처에 새 탱크가 주어지니\n다시 시도할 수 있어요.",
    t25 = "축하해요. WinBolo 튜토리얼을 완료했어요.\n\n이제 플레이할 준비가 됐어요. 온라인 게임에\n참가하거나, 친구들을 위해 게임을 주최하거나,\n봇을 상대로 연습해 보세요.",
    respawn1 = "탱크가 파괴됐지만 튜토리얼을 처음부터\n다시 시작할 필요는 없어요. 상륙한 지점 바로\n건너편 물 너머에 새 탱크가 주어졌어요.\n다시 싸우러 가서 탈출을 완료하세요.",
  },
}

TEXT["nl"] = {
  pop = {
    start01 = {
      keyboard = "Welkom bij de WinBolo-tutorial. Dit eiland introduceert\nde basisprincipes van WinBolo en leidt je er stap voor\nstap doorheen. Elk nieuw principe wordt beschreven\nin een venster zoals dit.\n\nNa elk bericht:\ndruk op {DISMISS} om door te gaan.",
      touch = "Welkom bij de WinBolo-tutorial. Dit eiland introduceert\nde basisprincipes van WinBolo en leidt je er stap voor\nstap doorheen. Elk nieuw principe wordt beschreven\nin een venster zoals dit.\n\nNa het lezen van elk bericht kun je verdergaan door\nop de knop \"OK\" te tikken.",
    },
    start02 = "WinBolo is de enige geautoriseerde kloon van Stuart Cheshire's klassieke Macintosh-netwerkspel, Bolo.\nJe kunt strategietips en andere informatie vinden op de officiële website:\n\nhttps://www.winbolo.com\n\nOf kom je bij ons voegen op Reddit en Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Je bestuurt een tank, die zich momenteel op een\nboot op zee bevindt. Druk op {ACCEL} om de tank (en boot) vooruit\nte laten gaan en druk op {BRAKE} om te vertragen en\nte stoppen.\n\nHoud de toetsen ingedrukt totdat de tank op de\ngewenste snelheid is, en laat dan los. Druk nu op\n{ACCEL} om de boot vooruit te laten rijden naar het land.\n(Druk eerst op {DISMISS} om het venster te sluiten)",
      touch = "Je bestuurt een tank, die zich momenteel op een\nboot op zee bevindt. Gebruik de joystick aan de linkerkant van\nhet scherm om te rijden: duw hem naar voren om te versnellen,\ntrek hem naar achteren om te vertragen en te stoppen.\n\nHoe verder je de joystick induwt, hoe sneller de\ntank gaat. Duw de joystick nu naar voren om de\nboot vooruit te laten rijden naar het land.\n(Tik eerst op OK om dit venster te sluiten)",
      controller = "Je bestuurt een tank, die zich momenteel op een\nboot op zee bevindt. Duw de linker stick naar voren om\nde tank (en boot) vooruit te laten gaan en trek hem\nnaar achteren om te vertragen en te stoppen.\n\nHoe verder je de linker stick induwt, hoe sneller\nde tank gaat. Duw de linker stick nu naar voren\nom de boot naar het land te rijden.\n(Druk eerst op {DISMISS} om het venster te sluiten)",
    },
    t01 = {
      keyboard = "Voor je ligt een korte rivier die landinwaarts gaat. Houd\nde {ACCEL}-toets ingedrukt om je boot naar het einde van de\nrivier te rijden.\n\nAls je er bent, blijf dan {ACCEL} ingedrukt houden. De tank\nstapt uit de boot en de boot blijft afgemeerd aan\nhet einde van de rivier.",
      touch = "Voor je ligt een korte rivier die landinwaarts gaat. Duw\nde joystick naar voren om je boot naar het einde\nvan de rivier te rijden.\n\nAls je er bent, houd de joystick dan\nnaar voren gedrukt. De tank stapt uit de boot en de\nboot blijft afgemeerd aan het einde van de rivier.",
      controller = "Voor je ligt een korte rivier die landinwaarts gaat. Duw\nde linker stick naar voren om je boot naar het einde\nvan de rivier te rijden.\n\nAls je er bent, houd de stick dan naar voren gedrukt.\nDe tank stapt uit de boot en de boot blijft\nafgemeerd aan het einde van de rivier.",
    },
    t02 = {
      keyboard = "Je staat nu op het gras. De tank beweegt vrij\nsnel op gras.\n\nBlijf {ACCEL} indrukken om verder te gaan naar het bos.",
      touch = "Je staat nu op het gras. De tank beweegt vrij\nsnel op gras.\n\nHoud de joystick naar voren om verder te gaan\nnaar het bos.",
      controller = "Je staat nu op het gras. De tank beweegt vrij\nsnel op gras.\n\nHoud de linker stick naar voren om verder\nte gaan naar het bos.",
    },
    t03 = {
      keyboard = "Je bent nu in het bos. De tank beweegt langzamer\nin het bos dan op gras.\n\nBlijf {ACCEL} indrukken om verder te gaan naar het moeras.",
      touch = "Je bent nu in het bos. De tank beweegt langzamer\nin het bos dan op gras.\n\nHoud de joystick naar voren om verder te gaan\nnaar het moeras.",
      controller = "Je bent nu in het bos. De tank beweegt langzamer\nin het bos dan op gras.\n\nHoud de linker stick naar voren om verder\nte gaan naar het moeras.",
    },
    t04 = {
      keyboard = "Je bent nu in het moeras. De tank beweegt heel\nlangzaam in moeras. Gelukkig is er een weg voor je.\n\nBlijf {ACCEL} indrukken om verder te gaan naar de weg.",
      touch = "Je bent nu in het moeras. De tank beweegt heel\nlangzaam in moeras. Gelukkig is er een weg voor je.\n\nHoud de joystick naar voren om verder te gaan\nnaar de weg.",
      controller = "Je bent nu in het moeras. De tank beweegt heel\nlangzaam in moeras. Gelukkig is er een weg voor je.\n\nHoud de linker stick naar voren om verder\nte gaan naar de weg.",
    },
    t05 = {
      keyboard = "Je bent nu op de weg. De tank kan heel snel rijden\nop de weg en als je {ACCEL} indrukt, versnelt de tank.\n\nAls je te snel gaat, kun je op {BRAKE} drukken",
      touch = "Je bent nu op de weg. De tank kan heel snel rijden\nop de weg en de joystick helemaal naar voren duwen\nversnelt hem.\n\nAls je te snel gaat, trek de\njoystick naar achteren om te vertragen.",
      controller = "Je bent nu op de weg. De tank kan heel snel rijden\nop de weg en de linker stick helemaal naar voren\nduwen versnelt hem.\n\nAls je te snel gaat, trek de linker stick\nnaar achteren om te vertragen.",
    },
    t06 = {
      keyboard = "Er zijn wat gebouwen voor je. Gebouwen zijn vaste\nbelemmeringen waar je niet doorheen kunt rijden, dus\nmoe je je een weg door het doolhof vinden naar de\nandere kant.\n\nDruk op {LEFT} om de tank links te laten draaien\nDruk op {RIGHT} om de tank rechts te laten draaien",
      touch = "Er zijn wat gebouwen voor je. Gebouwen zijn vaste\nbelemmeringen waar je niet doorheen kunt rijden, dus\nmoe je je een weg door het doolhof vinden naar de\nandere kant.\n\nDuw de joystick naar links of rechts om de\ntank in die richting te draaien.",
      controller = "Er zijn wat gebouwen voor je. Gebouwen zijn vaste\nbelemmeringen waar je niet doorheen kunt rijden, dus\nmoe je je een weg door het doolhof vinden naar de\nandere kant.\n\nDuw de linker stick naar links of rechts om de\ntank in die richting te draaien.",
    },
    t07 = "Die ronde objecten met de rode kanonnen die uitsteken zijn\nautomatische bunkers. Ze schieten op elke tank\ndie binnen bereik komt.\n\nGelukkig bieden de gebouwen bescherming tegen de\nschoten, dus de bunkers kunnen je niet raken\ntenzij je zo lang wacht dat ze zich een weg door\nde gebouwen schieten.",
    t08 = "Bos biedt ook bescherming tegen bunkers, maar op\neen andere manier. Naast het sterk vertragen van de tank,\nbeperkt rijden door bos ook het zicht.\n\nAls je in het bos bent, kun je nog steeds duidelijk\nalles buiten het bos zien, maar bunkers (en\nandere spelers) kunnen je tank niet zien tenzij je\nerg dichtbij komt.",
    t09 = "Lijn je tank uit met het midden van de strook\nbos en rij recht naar het noorden. Als je recht rijdt\nen niet te dicht bij de randen van het bos komt,\nzullen de bunkers je niet zien en niet schieten.\n\nRijd niet buiten het bos of je wordt aan stukken geschoten.",
    t10 = {
      keyboard = "Er is nog één eenvoudige manier om aan bunkers te ontsnappen -\nsnelheid.\n\nZorg dat je correct op het midden van de weg staat\nen houd {ACCEL} ingedrukt om op volle snelheid langs de\nbunkers te rijden.",
      touch = "Er is nog één eenvoudige manier om aan bunkers te ontsnappen -\nsnelheid.\n\nZorg dat je correct op het midden van de weg staat\nen duw de joystick helemaal naar voren om op\nvolle snelheid langs de bunkers te rijden.",
      controller = "Er is nog één eenvoudige manier om aan bunkers te ontsnappen -\nsnelheid.\n\nZorg dat je correct op het midden van de weg staat\nen duw de linker stick helemaal naar voren om op\nvolle snelheid langs de bunkers te rijden.",
    },
    t11 = "Als ze je opmerken, beginnen ze te schieten, maar als\nje recht vooruit blijft rijden en je zenuwen niet verliest,\nben je buiten bereik voordat de schoten je kunnen\nraken.\n\nRijd niet van de weg af het moeras in, anders ben je\neen makkelijk doelwit voor de bunkers en kun je niet\nsnel genoeg bewegen om te ontsnappen.",
    t12 = "Voor je ligt een bevoorradingsbasis. Bevoorradingsbases leveren\ngranaten om te schieten, mijnen om geheime valstrikken mee te\nleggen, en ze repareren ook het pantser van je tank terug\nnaar volledige sterkte als het beschadigd is.\n\nStop op de bevoorradingsbasis en laad je tank\nmet granaten.",
    t13 = "Je staat op de bevoorradingsbasis. Je tank wordt nu\ngeladen met granaten en mijnen, zoals de indicatoren\nrechts aangeven. Voor je, achter de\ngebouwen, ligt een mijnenveld.",
    t14 = {
      keyboard = "Als je je tank het mijnenveld inrijdt, wordt hij\nvernietigd. Gelukkig kun je een mijnenveld opruimen door\nte schieten. Druk op {FIRE} om te schieten.\n\nSchiet een gat door het gebouw en laat dan een\ngranaat in het midden van het mijnenveld landen om het tot\nontploffing te brengen. Als je de mijnen opblaast, ontstaat\neen grote krater in de grond, waar je doorheen\nkunt rijden, hoewel het net zo langzaam gaat als door moeras.",
      touch = "Als je je tank het mijnenveld inrijdt, wordt hij\nvernietigd. Gelukkig kun je een mijnenveld opruimen door\nte schieten. Tik op de vuurknop om te schieten.\n\nSchiet een gat door het gebouw en laat dan een\ngranaat in het midden van het mijnenveld landen om het tot\nontploffing te brengen. Als je de mijnen opblaast, ontstaat\neen grote krater in de grond, waar je doorheen\nkunt rijden, hoewel het net zo langzaam gaat als door moeras.",
      controller = "Als je je tank het mijnenveld inrijdt, wordt hij\nvernietigd. Gelukkig kun je een mijnenveld opruimen door\nte schieten. Druk op {FIRE} om te schieten.\n\nSchiet een gat door het gebouw en laat dan een\ngranaat in het midden van het mijnenveld landen om het tot\nontploffing te brengen. Als je de mijnen opblaast, ontstaat\neen grote krater in de grond, waar je doorheen\nkunt rijden, hoewel het net zo langzaam gaat als door moeras.",
    },
    t15 = "Voor je ligt een ander mijnenveld. Doe dezelfde truc:\nRij naar de bevoorradingsbasis, schiet door de\nmuur en laat een granaat in het midden van het mijnenveld\nlanden om het tot ontploffing te brengen.\n\nDeze keer grenst het mijnenveld aan een grote\nrivier en water zal de kraters instromen. Je kunt\nde tank door het water rijden, net als door\nkraters en moeras.",
    t16 = {
      keyboard = "WinBolo scrollt het venster automatisch om het\ngebied rond je tank te tonen, maar je kunt de\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\ntoetsen gebruiken om de weergave handmatig te sturen.\n\nDruk nu op {SCROLL_UP} voor een beter uitzicht op de\nrivier voor je.",
      touch = "WinBolo scrollt het venster automatisch om het\ngebied rond je tank te tonen, maar je kunt de kaart\nmet een vinger slepen om de weergave handmatig te sturen.\n\nSleep de kaart nu naar beneden om de rivier\nvoor je in beeld te brengen.",
      controller = "WinBolo scrollt het venster automatisch om het\ngebied rond je tank te tonen, maar je kunt de\nrechter stick gebruiken om de weergave handmatig te sturen.\n\nDuw de rechter stick nu omhoog voor een beter\nuitzicht op de rivier voor je.",
    },
    t17 = "Naast het erg langzame rijden heeft rijden door water\nnog een nadeel: het tast ook de voorraad granaten\nen mijnen van je tank aan.\n\nAls je aan de andere kant aankomt, zal je tank nog\nwerken, maar je bent al je aanvalscapaciteit kwijtgeraakt.",
    t18 = {
      keyboard = "Deze keer gaan we subtieler te werk bij het\noversteken van de rivier. In plaats van gewoon door\nhet water te rijden, gaan we een brug bouwen.\n\nSelecteer de boommodus door op het bospictogram\n(linksboven in het venster) te klikken en klik op het bos.\nJe bouwer rent uit de tank en oogst één\nboom voor bouwmaterialen.",
      touch = "Deze keer gaan we subtieler te werk bij het\noversteken van de rivier. In plaats van gewoon door\nhet water te rijden, gaan we een brug bouwen.\n\nSelecteer de boommodus door op het bospictogram\n(linksboven in het venster) te tikken en tik op het bos.\nJe bouwer rent uit de tank en oogst één\nboom voor bouwmaterialen.",
      controller = "Deze keer gaan we subtieler te werk bij het\noversteken van de rivier. In plaats van gewoon door\nhet water te rijden, gaan we een brug bouwen.\n\nTrek {BUILD_MODE} om van scrollen naar bouwmodus\nte schakelen - de rechter stick beweegt nu een bouwcursor\nin plaats van het beeld. Druk op {BUILD_TOOL} om de\nboomkapmodus te selecteren (linksboven), richt de cursor\nop een boom en druk op {BUILD_PLACE}. Je bouwer\noogst één boom voor bouwmaterialen.",
    },
    t19 = {
      keyboard = "Na het oogsten van bomen, selecteer het weg-/bruggereedschap\n(de tweede knop van boven) en klik met de muis\nop de rivier aan het einde van de weg.\n\nJe bouwer rent uit de tank en bouwt één\nbrugdeel. Klik nogmaals om de brug te voltooien\nen rij dan naar de andere kant.",
      touch = "Na het oogsten van bomen, selecteer het weg-/bruggereedschap\n(de tweede knop van boven) en tik op de\nrivier aan het einde van de weg.\n\nJe bouwer rent uit de tank en bouwt één\nbrugdeel. Tik nogmaals om de brug te voltooien\nen rij dan naar de andere kant.",
      controller = "Na het oogsten van bomen, druk op {BUILD_TOOL} om\nhet weg-/bruggereedschap te selecteren (de tweede knop\nvan boven), richt de bouwcursor op de rivier aan het\neinde van de weg en druk op {BUILD_PLACE}.\n\nJe bouwer bouwt één brugdeel. Plaats nogmaals om\nde brug te voltooien, trek dan {BUILD_MODE} om de\nbouwmodus te verlaten en rij naar de andere kant.",
    },
    t20 = "Deze rivier is breder dan de vorige en in het midden is\nze te diep om een brug te bouwen. Als je probeert\ndoor te rijden, zinkt de tank naar de bodem\nen wordt vernietigd.",
    t21 = {
      keyboard = "Om deze rivier over te steken, moet je een boot bouwen. Boten\nkosten vijf bomen, dus klik op de vijf bomen om ze te oogsten.\nSelecteer dan het muur-/bootgereedschap (de derde knop van\nboven) en klik op het water om een boot te bouwen.\n\nAls je op het land klikt, bouw je een muur\nen moet je nog meer bomen oogsten om een boot\nte bouwen. Rij op de boot en steek de rivier over.",
      touch = "Om deze rivier over te steken, moet je een boot bouwen. Boten\nkosten vijf bomen, dus tik op de vijf bomen om ze te oogsten.\nSelecteer dan het muur-/bootgereedschap (de derde knop van\nboven) en tik op het water om een boot te bouwen.\n\nAls je op het land tikt, bouw je een muur\nen moet je nog meer bomen oogsten om een boot\nte bouwen. Rij op de boot en steek de rivier over.",
      controller = "Om deze rivier over te steken, moet je een boot bouwen.\nBoten kosten vijf bomen: richt op vijf bomen en druk\nop {BUILD_PLACE} bij elke boom. Druk dan op {BUILD_TOOL}\nom het muur-/bootgereedschap te selecteren (de derde knop\nvan boven) en plaats het op het water.\n\nAls je op het land plaatst, bouw je een muur en\nmoet je meer bomen oogsten. Rij op de boot\nen steek de rivier over.",
    },
    t22 = "Er is een bunker voor je en wat bevoorradingsbases voor\nmunitie. Verzamel granaten bij de bevoorradingsbasis en\nval de bunker aan.\n\nHij schiet terug, maar als je tank geraakt wordt, kun je\ngemakkelijk teruggaan naar de bevoorradingsbasis om je pantser\nterug naar volle sterkte te repareren.",
    t23 = "Met geduld en voorzichtigheid moet je deze bunker kunnen\nvernietigen. Gebruik de vaardigheden die je hebt geleerd\nom het makkelijker te maken:\n\nVerschuil je in het bos zodat de bunker je niet kan zien,\noogst bomen, bouw wegen om je tank sneller te laten rijden\nen bouw gebouwen om dekking te bieden tegen\nbunkerschoten.",
    t24 = "Nadat je deze bunker hebt verslagen, is je einduitdaging\nvan de tutorial om de twee bunkers bij de uitgang\nte vernietigen, een boot te bouwen en te ontsnappen.\n\nLet goed op het pantserniveau van je tank -\nals je tank vernietigd wordt, krijg je een nieuwe tank\nin de buurt zodat je het opnieuw kunt proberen.",
    t25 = "Gefeliciteerd. Je hebt de WinBolo-tutorial\nvoltooid.\n\nJe bent klaar om te spelen. Doe mee aan een online spel,\nhost er een voor je vrienden, of oefen tegen bots.",
    respawn1 = "Je tank is vernietigd - maar je hoeft niet opnieuw\nte beginnen met de tutorial. Je hebt een nieuwe tank\ngekregen aan de overkant van het water, waar je aan\nland ging. Ga terug en voltooi je ontsnapping.",
  },
}

TEXT["pl"] = {
  pop = {
    start01 = {
      keyboard = "Witaj w samouczku WinBolo. Ta wyspa wprowadza\npodstawowe zasady WinBolo i prowadzi cię przez\nnie po kolei. Każda nowa zasada zostanie opisana\nw oknie podobnym do tego.\n\nPo przeczytaniu każdej wiadomości\nnaciśnij {DISMISS}, by kontynuować.",
      touch = "Witaj w samouczku WinBolo. Ta wyspa wprowadza\npodstawowe zasady WinBolo i prowadzi cię przez\nnie po kolei. Każda nowa zasada zostanie opisana\nw oknie podobnym do tego.\n\nPo przeczytaniu każdej wiadomości możesz przejść\ndalej, stukając przycisk \"OK\".",
    },
    start02 = "WinBolo to jedyny autoryzowany klon klasycznej macintoshowej gry sieciowej Bolo autorstwa Stuarta Cheshire'a.\nWskazówki strategiczne i inne informacje znajdziesz na oficjalnej stronie:\n\nhttps://www.winbolo.com\n\nLub dołącz do nas na Reddicie i Discordzie\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Sterujesz czołgiem, który aktualnie znajduje się na\nłodzi na morzu. Naciśnij {ACCEL}, aby czołg (i łódź)\nruszył naprzód, a {BRAKE}, aby zwolnił i się\nzatrzymał.\n\nPrzytrzymaj klawisze, aż czołg osiągnie żądaną\nprędkość, a potem puść. Naciśnij teraz {ACCEL}, aby\nłódź popłynęła w stronę lądu.\n(Naciśnij najpierw {DISMISS}, aby zamknąć to okno)",
      touch = "Sterujesz czołgiem, który aktualnie znajduje się na\nłodzi na morzu. Użyj drążka po lewej stronie ekranu,\naby kierować: przesuń go w przód, by przyspieszyć,\npociągnij w tył, by zwolnić i się zatrzymać.\n\nIm dalej przesuniesz drążek, tym szybciej jedzie\nczołg. Przesuń teraz drążek w przód, aby łódź\npopłynęła w stronę lądu.\n(Stuknij najpierw OK, aby zamknąć to okno)",
      controller = "Sterujesz czołgiem, który aktualnie znajduje się na\nłodzi na morzu. Przesuń lewy drążek do przodu,\naby czołg (i łódź) ruszył naprzód, i pociągnij go\nz powrotem, aby zwolnił i się zatrzymał.\n\nIm dalej przesuniesz lewy drążek, tym szybciej\njedzie czołg. Przesuń teraz lewy drążek do przodu,\naby łódź popłynęła w stronę lądu.\n(Naciśnij najpierw {DISMISS}, aby zamknąć to okno)",
    },
    t01 = {
      keyboard = "Przed tobą jest krótka rzeka prowadząca w głąb lądu.\nPrzytrzymaj klawisz {ACCEL}, aby pokierować łodzią\ndo końca rzeki.\n\nGdy tam dotrzesz, nie puszczaj {ACCEL}. Czołg zejdzie\nz łodzi, a łódź pozostanie zacumowana na końcu rzeki.",
      touch = "Przed tobą jest krótka rzeka prowadząca w głąb lądu.\nPrzesuń drążek do przodu, aby pokierować łodzią\ndo końca rzeki.\n\nGdy tam dotrzesz, trzymaj drążek przesunięty\ndo przodu. Czołg zejdzie z łodzi, a łódź pozostanie\nzacumowana na końcu rzeki.",
      controller = "Przed tobą jest krótka rzeka prowadząca w głąb lądu.\nPrzesuń lewy drążek do przodu, aby pokierować\nłodzią do końca rzeki.\n\nGdy tam dotrzesz, trzymaj drążek przesunięty do\nprzodu. Czołg zejdzie z łodzi, a łódź pozostanie\nzacumowana na końcu rzeki.",
    },
    t02 = {
      keyboard = "Jesteś teraz na trawie. Czołg porusza się po trawie\ndość szybko.\n\nNaciskaj {ACCEL}, aby przejechać do lasu.",
      touch = "Jesteś teraz na trawie. Czołg porusza się po trawie\ndość szybko.\n\nTrzymaj drążek przesunięty do przodu, aby\nprzejechać do lasu.",
      controller = "Jesteś teraz na trawie. Czołg porusza się po trawie\ndość szybko.\n\nTrzymaj lewy drążek przesunięty do przodu, aby\nprzejechać do lasu.",
    },
    t03 = {
      keyboard = "Jesteś teraz w lesie. Czołg porusza się w lesie\nwolniej niż na trawie.\n\nNaciskaj {ACCEL}, aby przejechać do bagna.",
      touch = "Jesteś teraz w lesie. Czołg porusza się w lesie\nwolniej niż na trawie.\n\nTrzymaj drążek przesunięty do przodu, aby\nprzejechać do bagna.",
      controller = "Jesteś teraz w lesie. Czołg porusza się w lesie\nwolniej niż na trawie.\n\nTrzymaj lewy drążek przesunięty do przodu, aby\nprzejechać do bagna.",
    },
    t04 = {
      keyboard = "Jesteś teraz na bagnie. Czołg porusza się po bagnie\nbardzo wolno. Na szczęście przed tobą jest droga.\n\nNaciskaj {ACCEL}, aby przejechać do drogi.",
      touch = "Jesteś teraz na bagnie. Czołg porusza się po bagnie\nbardzo wolno. Na szczęście przed tobą jest droga.\n\nTrzymaj drążek przesunięty do przodu, aby\nprzejechać do drogi.",
      controller = "Jesteś teraz na bagnie. Czołg porusza się po bagnie\nbardzo wolno. Na szczęście przed tobą jest droga.\n\nTrzymaj lewy drążek przesunięty do przodu, aby\nprzejechać do drogi.",
    },
    t05 = {
      keyboard = "Jesteś teraz na drodze. Czołg może poruszać się\npo drodze bardzo szybko, a po naciśnięciu {ACCEL}\nczołg przyspieszy.\n\nJeśli stwierdzisz, że jedziesz zbyt szybko, możesz\nnacisnąć {BRAKE}",
      touch = "Jesteś teraz na drodze. Czołg może poruszać się\npo drodze bardzo szybko, a przesunięcie drążka\ndo oporu w przód go przyspieszy.\n\nJeśli stwierdzisz, że jedziesz zbyt szybko, pociągnij\ndrążek z powrotem, aby zwolnić.",
      controller = "Jesteś teraz na drodze. Czołg może poruszać się\npo drodze bardzo szybko, a przesunięcie lewego\ndrążka do oporu w przód go przyspieszy.\n\nJeśli stwierdzisz, że jedziesz zbyt szybko, pociągnij\nlewy drążek z powrotem, aby zwolnić.",
    },
    t06 = {
      keyboard = "Przed tobą stoją budynki. Budynki to solidne\nprzeszkody, których nie da się przejechać, więc\nbędziesz musiał znaleźć drogę przez labirynt na\ndrugą stronę.\n\nAby skręcić czołgiem w lewo, naciśnij {LEFT}\nAby skręcić czołgiem w prawo, naciśnij {RIGHT}",
      touch = "Przed tobą stoją budynki. Budynki to solidne\nprzeszkody, których nie da się przejechać, więc\nbędziesz musiał znaleźć drogę przez labirynt na\ndrugą stronę.\n\nPrzesuń drążek w lewo lub w prawo, aby skręcić\nczołgiem w tym kierunku.",
      controller = "Przed tobą stoją budynki. Budynki to solidne\nprzeszkody, których nie da się przejechać, więc\nbędziesz musiał znaleźć drogę przez labirynt na\ndrugą stronę.\n\nPrzesuń lewy drążek w lewo lub w prawo, aby skręcić\nczołgiem w tym kierunku.",
    },
    t07 = "Te okrągłe obiekty z wystającymi czerwonymi działami\nto automatyczne bunkry. Strzelają do każdego czołgu,\nktóry znajdzie się w ich zasięgu.\n\nNa szczęście budynki dają osłonę przed pociskami,\nwięc bunkry nie trafią cię, chyba że będziesz czekać\ntak długo, że zdążą całkowicie przestrzelić budynki.",
    t08 = "Las również daje osłonę przed bunkrami, ale w inny\nsposób. Oprócz mocnego spowolnienia czołgu,\njazda przez las również ogranicza widoczność.\n\nGdy jesteś w lesie, nadal wyraźnie widzisz wszystko\npoza lasem, ale bunkry (i inni gracze) nie widzą\ntwojego czołgu, dopóki nie znajdziesz się bardzo blisko.",
    t09 = "Ustaw czołg na środku pasa lasu i jedź na północ.\nJeśli pojedziesz prosto i nie zboczysz zbyt blisko\nbrzegów lasu, bunkry cię nie zobaczą i nie strzelą.\n\nNie wyjeżdżaj poza las, bo zostaniesz rozerwany na strzępy.",
    t10 = {
      keyboard = "Jest jeszcze jeden prosty sposób na ucieczkę przed\nbunkrami — szybkość.\n\nUpewnij się, że jesteś dobrze ustawiony na środku\ndrogi, i przytrzymaj {ACCEL}, aby pełną prędkością\nprzemknąć obok bunkrów.",
      touch = "Jest jeszcze jeden prosty sposób na ucieczkę przed\nbunkrami — szybkość.\n\nUpewnij się, że jesteś dobrze ustawiony na środku\ndrogi, i przesuń drążek do oporu w przód, aby pełną\nprędkością przemknąć obok bunkrów.",
      controller = "Jest jeszcze jeden prosty sposób na ucieczkę przed\nbunkrami — szybkość.\n\nUpewnij się, że jesteś dobrze ustawiony na środku\ndrogi, i przesuń lewy drążek do oporu w przód, aby\npełną prędkością przemknąć obok bunkrów.",
    },
    t11 = "Gdy cię zauważą, zaczną strzelać, ale jeśli pojedziesz\nprosto i nie stracisz zimnej krwi, znajdziesz się poza\nzasięgiem, zanim pociski cię trafią.\n\nNie zjeżdżaj z drogi w bagno, bo będziesz jak kaczka\nna strzelnicy dla bunkrów i nie będziesz w stanie\nporuszać się dość szybko, aby uciec.",
    t12 = "Przed tobą jest baza zaopatrzenia. Bazy zaopatrzenia\ndostarczają pocisków do strzelania, min do zakładania\ntajnych pułapek, a także naprawiają pancerz czołgu\ndo pełnej wytrzymałości, jeśli został uszkodzony.\n\nZatrzymaj się na bazie zaopatrzenia i załaduj czołg\npociskami.",
    t13 = "Jesteś na bazie zaopatrzenia. Twój czołg załaduje\nteraz pociski i miny, jak pokażą wskaźniki po prawej.\nPrzed tobą, za budynkami, znajduje się pole minowe.",
    t14 = {
      keyboard = "Jeśli wjedziesz czołgiem w pole minowe, zostanie\nzniszczony. Na szczęście pole minowe można\noczyścić strzelaniem. Naciśnij {FIRE}, aby strzelić.\n\nZrób dziurę w budynku, a następnie umieść pocisk\nna środku pola minowego, aby je zdetonować.\nGdy wysadzisz miny, zrobią duży krater w ziemi,\nprzez który możesz przejechać, choć jest to równie\npowolne jak jazda przez bagno.",
      touch = "Jeśli wjedziesz czołgiem w pole minowe, zostanie\nzniszczony. Na szczęście pole minowe można\noczyścić strzelaniem. Stuknij przycisk strzału, aby strzelić.\n\nZrób dziurę w budynku, a następnie umieść pocisk\nna środku pola minowego, aby je zdetonować.\nGdy wysadzisz miny, zrobią duży krater w ziemi,\nprzez który możesz przejechać, choć jest to równie\npowolne jak jazda przez bagno.",
      controller = "Jeśli wjedziesz czołgiem w pole minowe, zostanie\nzniszczony. Na szczęście pole minowe można\noczyścić strzelaniem. Naciśnij {FIRE}, aby strzelić.\n\nZrób dziurę w budynku, a następnie umieść pocisk\nna środku pola minowego, aby je zdetonować.\nGdy wysadzisz miny, zrobią duży krater w ziemi,\nprzez który możesz przejechać, choć jest to równie\npowolne jak jazda przez bagno.",
    },
    t15 = "Przed tobą kolejne pole minowe. Powtórz tę samą\nsztuczkę: wjedź na bazę zaopatrzenia, przestrzel\nścianę i umieść pocisk na środku pola minowego,\naby je zdetonować.\n\nTym razem jednak pole minowe sąsiaduje z dużą\nrzeką, a woda zaleje kratery. Czołgiem możesz\njechać przez wodę tak samo jak przez kratery i bagno.",
    t16 = {
      keyboard = "WinBolo automatycznie przewija okno, aby pokazać ci\nobszar wokół czołgu, ale możesz użyć klawiszy\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT},\naby ręcznie nadpisać widok wybrany przez WinBolo.\n\nNaciśnij teraz {SCROLL_UP}, aby lepiej zobaczyć\nrzekę przed tobą.",
      touch = "WinBolo automatycznie przewija okno, aby pokazać ci\nobszar wokół czołgu, ale możesz przeciągnąć mapę\npalcem, aby ręcznie nadpisać widok.\n\nPrzeciągnij mapę w dół teraz, aby zobaczyć\nrzekę przed tobą.",
      controller = "WinBolo automatycznie przewija okno, aby pokazać ci\nobszar wokół czołgu, ale możesz pchnąć prawym\ndrążkiem, aby ręcznie nadpisać wybrany przez WinBolo widok.\n\nPchnij teraz prawym drążkiem w górę, aby lepiej\nzobaczyć rzekę przed tobą.",
    },
    t17 = "Oprócz tego, że jest bardzo wolna, jazda przez wodę\nma jeszcze jedną wadę: zużywa również zapasy\npocisków i min czołgu.\n\nGdy dotrzesz na drugą stronę, twój czołg nadal\nbędzie sprawny, ale stracisz całą zdolność\nbojową.",
    t18 = {
      keyboard = "Tym razem przekroczymy rzekę bardziej subtelnie.\nZamiast po prostu przejeżdżać przez wodę,\nzbudujemy most.\n\nWybierz tryb sadzenia drzew, klikając ikonę lasu\n(lewy górny róg okna), i kliknij na lesie. Twój\nbudowniczy wybiegnie z czołgu i ścinie jedno\ndrzewo na materiały budowlane.",
      touch = "Tym razem przekroczymy rzekę bardziej subtelnie.\nZamiast po prostu przejeżdżać przez wodę,\nzbudujemy most.\n\nWybierz tryb sadzenia drzew, stukając ikonę lasu\n(lewy górny róg okna), i stuknij na lesie. Twój\nbudowniczy wybiegnie z czołgu i ścinie jedno\ndrzewo na materiały budowlane.",
      controller = "Tym razem przekroczymy rzekę bardziej subtelnie.\nZamiast po prostu przejeżdżać przez wodę,\nzbudujemy most.\n\nPociągnij {BUILD_MODE}, aby przełączyć się z przewijania\nna tryb budowania — prawy drążek steruje teraz\nkursorem budowania. Naciśnij {BUILD_TOOL}, aby wybrać\ntryb ścinania drzew (lewy górny róg okna), wyceluj\nkursor w drzewo i naciśnij {BUILD_PLACE}. Twój\nbudowniczy wybiegnie z czołgu i ścinie jedno drzewo.",
    },
    t19 = {
      keyboard = "Po ścięciu kilku drzew wybierz narzędzie droga/most\n(drugi przycisk od góry) i kliknij myszą na rzece\nna końcu drogi.\n\nTwój budowniczy wybiegnie z czołgu i zbuduje jeden\nodcinek mostu. Kliknij ponownie, aby dokończyć most,\na potem przejedź na drugą stronę.",
      touch = "Po ścięciu kilku drzew wybierz narzędzie droga/most\n(drugi przycisk od góry) i stuknij na rzece\nna końcu drogi.\n\nTwój budowniczy wybiegnie z czołgu i zbuduje jeden\nodcinek mostu. Stuknij ponownie, aby dokończyć most,\na potem przejedź na drugą stronę.",
      controller = "Po ścięciu kilku drzew naciśnij {BUILD_TOOL}, aby\nwybrać narzędzie droga/most (drugi przycisk od góry),\nwyceluj kursor budowania w rzekę na końcu drogi\ni naciśnij {BUILD_PLACE}.\n\nTwój budowniczy wybiegnie z czołgu i zbuduje jeden\nodcinek mostu. Umieść ponownie, aby ukończyć most,\nnastępnie pociągnij {BUILD_MODE}, aby wyjść z trybu\nbudowania, i przejedź na drugą stronę.",
    },
    t20 = "Ta rzeka jest szersza niż poprzednia, a w środku jest\nzbyt głęboka, abyś mógł zbudować most. Jeśli\nspróbujesz po prostu przejechać, czołg utonie\ni zostanie zniszczony.",
    t21 = {
      keyboard = "Aby przekroczyć tę rzekę, musisz zbudować łódź.\nŁodzie wymagają pięciu drzew, więc kliknij na pięciu\ndrzewach, aby je ściąć. Następnie wybierz narzędzie\nściana/łódź (trzeci przycisk od góry) i kliknij na\nwodzie, aby zbudować łódź.\n\nJeśli klikniesz na lądzie, zbudujesz ścianę zamiast\nłodzi i będziesz musiał ściąć więcej drzew, aby\nzbudować łódź. Wjedź na łódź i przepłyń na drugą\nstronę.",
      touch = "Aby przekroczyć tę rzekę, musisz zbudować łódź.\nŁodzie wymagają pięciu drzew, więc stuknij na pięciu\ndrzewach, aby je ściąć. Następnie wybierz narzędzie\nściana/łódź (trzeci przycisk od góry) i stuknij na\nwodzie, aby zbudować łódź.\n\nJeśli stukniesz na lądzie, zbudujesz ścianę zamiast\nłodzi i będziesz musiał ściąć więcej drzew, aby\nzbudować łódź. Wjedź na łódź i przepłyń na drugą\nstronę.",
      controller = "Aby przekroczyć tę rzekę, musisz zbudować łódź.\nŁodzie wymagają pięciu drzew, więc wyceluj w pięć\ndrzew i naciśnij {BUILD_PLACE} na każdym, aby je ściąć.\nNastępnie naciśnij {BUILD_TOOL}, aby wybrać narzędzie\nściana/łódź (trzeci przycisk od góry) i umieść je na wodzie.\n\nJeśli umieścisz na lądzie, zbudujesz ścianę i będziesz\nmusiał ściąć więcej drzew. Wjedź na łódź i przepłyń\nna drugą stronę.",
    },
    t22 = "Przed tobą jest bunkier i kilka baz zaopatrzenia,\nktóre dostarczą ci amunicji. Zbierz pociski z bazy\nzaopatrzenia i zaatakuj bunkier.\n\nBunkier odpowie ogniem, ale jeśli twój czołg zostanie\ntrafiony, możesz łatwo wrócić do bazy zaopatrzenia,\naby naprawić pancerz do pełnej wytrzymałości.",
    t23 = "Z cierpliwością i ostrożnością powinieneś dać radę\nzniszczyć ten bunkier. Wykorzystaj umiejętności,\nktórych się nauczyłeś, aby było łatwiej:\n\nUkryj się w lesie, aby bunkier cię nie widział, ścinaj\ndrzewa, buduj drogi, aby twój czołg poruszał się\nszybciej, i buduj budynki, które dadzą osłonę przed\npociskami z bunkra.",
    t24 = "Po pokonaniu tego bunkra ostatnim wyzwaniem\nsamouczka jest zniszczenie dwóch bunkrów\nstrzegących wyjścia, zbudowanie łodzi i ucieczka.\n\nZwróć szczególną uwagę na poziom pancerza czołgu —\njeśli czołg zostanie zniszczony, dostaniesz nowy czołg\nw pobliżu, żebyś mógł spróbować jeszcze raz.",
    t25 = "Gratulacje. Ukończyłeś samouczek WinBolo.\n\nJesteś gotowy do gry. Dołącz do gry online, załóż\nją dla znajomych albo poćwicz z botami.",
    respawn1 = "Twój czołg został zniszczony — ale nie musisz\nzaczynać samouczka od nowa. Dostałeś świeży czołg\ntuż za wodą, przy miejscu lądowania. Wróć do walki\ni dokończ ucieczkę.",
  },
}

TEXT["pt-br"] = {
  pop = {
    start01 = {
      keyboard = "Bem-vindo ao tutorial do WinBolo. Esta ilha apresenta\nos princípios básicos do WinBolo e te conduz por\neles um de cada vez. Cada novo princípio será\ndescrito numa janela como esta.\n\nDepois de cada mensagem,\npressione {DISMISS} para continuar.",
      touch = "Bem-vindo ao tutorial do WinBolo. Esta ilha apresenta\nos princípios básicos do WinBolo e te conduz por\neles um de cada vez. Cada novo princípio será\ndescrito numa janela como esta.\n\nDepois de ler cada mensagem, você pode prosseguir\ntocando no botão \"OK\".",
    },
    start02 = "O WinBolo é o único clone autorizado do clássico jogo de rede para Macintosh de Stuart Cheshire, o Bolo.\nVocê pode encontrar dicas de estratégia e outras informações no site oficial:\n\nhttps://www.winbolo.com\n\nOu venha se juntar a nós no Reddit e no Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Você está no comando de um tanque, que atualmente\nestá em um barco no mar. Pressione {ACCEL} para fazer\no tanque (e o barco) avançar, e pressione {BRAKE}\npara desacelerar e parar.\n\nMantenha as teclas pressionadas até que o tanque\nesteja na velocidade que você quer, depois solte.\nPressione {ACCEL} agora para fazer o barco avançar\nem direção à terra.\n(Pressione {DISMISS} antes para fechar a janela)",
      touch = "Você está no comando de um tanque, que atualmente\nestá em um barco no mar. Use o thumbstick do lado\nesquerdo da tela para conduzir: empurre-o para a\nfrente para acelerar, puxe-o para trás para desacelerar e parar.\n\nQuanto mais você empurrar o thumbstick, mais rápido\no tanque vai. Empurre o thumbstick para a frente\nagora para fazer o barco avançar em direção à terra.\n(Toque OK antes para fechar esta janela)",
      controller = "Você está no comando de um tanque, que atualmente\nestá em um barco no mar. Empurre o analógico esquerdo\npara a frente para fazer o tanque (e o barco) avançar\ne puxe-o para trás para desacelerar e parar.\n\nQuanto mais você empurrar o analógico esquerdo, mais\nrápido o tanque vai. Empurre o analógico esquerdo para\na frente agora para conduzir o barco em direção à terra.\n(Pressione {DISMISS} antes para fechar a janela)",
    },
    t01 = {
      keyboard = "À sua frente há um pequeno rio que segue para o\ninterior. Mantenha pressionada a tecla {ACCEL} para\npilotar seu barco até o final do rio.\n\nQuando chegar lá, continue pressionando {ACCEL}. O\ntanque desembarcará do barco e o barco ficará atracado\nno final do rio.",
      touch = "À sua frente há um pequeno rio que segue para o\ninterior. Empurre o thumbstick para a frente para\npilotar seu barco até o final do rio.\n\nQuando chegar lá, mantenha o thumbstick empurrado\npara a frente. O tanque desembarcará do barco e o\nbarco ficará atracado no final do rio.",
      controller = "À sua frente há um pequeno rio que segue para o\ninterior. Empurre o analógico esquerdo para a frente\npara pilotar seu barco até o final do rio.\n\nQuando chegar lá, mantenha o analógico empurrado\npara a frente. O tanque desembarcará do barco e o\nbarco ficará atracado no final do rio.",
    },
    t02 = {
      keyboard = "Você agora está na grama. O tanque se move bastante\nrápido sobre a grama.\n\nContinue pressionando {ACCEL} para avançar até a floresta.",
      touch = "Você agora está na grama. O tanque se move bastante\nrápido sobre a grama.\n\nMantenha o thumbstick empurrado para a frente para\navançar até a floresta.",
      controller = "Você agora está na grama. O tanque se move bastante\nrápido sobre a grama.\n\nMantenha o analógico esquerdo empurrado para a\nfrente para avançar até a floresta.",
    },
    t03 = {
      keyboard = "Você agora está na floresta. O tanque se move mais\ndevagar na floresta do que sobre a grama.\n\nContinue pressionando {ACCEL} para avançar até o pântano.",
      touch = "Você agora está na floresta. O tanque se move mais\ndevagar na floresta do que sobre a grama.\n\nMantenha o thumbstick empurrado para a frente para\navançar até o pântano.",
      controller = "Você agora está na floresta. O tanque se move mais\ndevagar na floresta do que sobre a grama.\n\nMantenha o analógico esquerdo empurrado para a\nfrente para avançar até o pântano.",
    },
    t04 = {
      keyboard = "Você agora está no pântano. O tanque se move muito\ndevagar no pântano. Felizmente há uma estrada à frente.\n\nContinue pressionando {ACCEL} para avançar até a estrada.",
      touch = "Você agora está no pântano. O tanque se move muito\ndevagar no pântano. Felizmente há uma estrada à frente.\n\nMantenha o thumbstick empurrado para a frente para\navançar até a estrada.",
      controller = "Você agora está no pântano. O tanque se move muito\ndevagar no pântano. Felizmente há uma estrada à frente.\n\nMantenha o analógico esquerdo empurrado para a\nfrente para avançar até a estrada.",
    },
    t05 = {
      keyboard = "Você agora está na estrada. O tanque pode se mover\nmuito rápido na estrada e, se você pressionar {ACCEL},\no tanque acelerará.\n\nSe achar que está indo rápido demais, pode pressionar {BRAKE}",
      touch = "Você agora está na estrada. O tanque pode se mover\nmuito rápido na estrada e empurrar o thumbstick\ntodo para a frente o acelerará.\n\nSe achar que está indo rápido demais, puxe o\nthumbstick para trás para desacelerar.",
      controller = "Você agora está na estrada. O tanque pode se mover\nmuito rápido na estrada e empurrar o analógico\nesquerdo todo para a frente o acelerará.\n\nSe achar que está indo rápido demais, puxe o\nanalógico esquerdo para trás para desacelerar.",
    },
    t06 = {
      keyboard = "Há alguns prédios à frente. Prédios são obstáculos\nsólidos pelos quais você não pode passar, então você\nterá que encontrar o caminho pelo labirinto até o\noutro lado.\n\nPara fazer o tanque virar à esquerda, pressione {LEFT}\nPara fazer o tanque virar à direita, pressione {RIGHT}",
      touch = "Há alguns prédios à frente. Prédios são obstáculos\nsólidos pelos quais você não pode passar, então você\nterá que encontrar o caminho pelo labirinto até o\noutro lado.\n\nEmpurre o thumbstick para a esquerda ou direita para\nvirar o tanque nessa direção.",
      controller = "Há alguns prédios à frente. Prédios são obstáculos\nsólidos pelos quais você não pode passar, então você\nterá que encontrar o caminho pelo labirinto até o\noutro lado.\n\nEmpurre o analógico esquerdo para a esquerda ou\ndireita para virar o tanque nessa direção.",
    },
    t07 = "Estes objetos redondos com canhões vermelhos\nsaindo são bunkers automáticos. Eles atirarão em\nqualquer tanque que entrar em seu alcance.\n\nFelizmente, os prédios protegem dos disparos, então\nos bunkers não conseguirão acertar você, a menos\nque espere tanto tempo que eles consigam abrir caminho\na tiros pelos prédios.",
    t08 = "A floresta também protege dos bunkers, mas de\numa forma diferente. Além de desacelerar muito o\ntanque, dirigir pela floresta também limita a visibilidade.\n\nQuando você está dentro da floresta, ainda pode ver\nclaramente tudo que está fora dela, mas os bunkers (e\noutros jogadores) não podem ver seu tanque a menos\nque cheguem muito perto.",
    t09 = "Alinhe seu tanque com o meio da faixa de floresta e\ndirija para o norte. Se for em linha reta e não chegar\nperto demais das bordas da floresta, os bunkers não\nvão te ver e não atirarão.\n\nNão saia da floresta ou você será explodido em pedaços.",
    t10 = {
      keyboard = "Há outra forma simples de escapar dos bunkers:\nvelocidade.\n\nCertifique-se de estar bem alinhado com o centro da\nestrada e mantenha {ACCEL} pressionado para passar\nem alta velocidade pelos bunkers.",
      touch = "Há outra forma simples de escapar dos bunkers:\nvelocidade.\n\nCertifique-se de estar bem alinhado com o centro da\nestrada e empurre o thumbstick totalmente para a\nfrente para passar em alta velocidade pelos bunkers.",
      controller = "Há outra forma simples de escapar dos bunkers:\nvelocidade.\n\nCertifique-se de estar bem alinhado com o centro\nda estrada e empurre o analógico esquerdo totalmente\npara a frente para passar em alta velocidade pelos bunkers.",
    },
    t11 = "Quando eles te notarem, começarão a atirar, mas se\nvocê continuar em linha reta e não perder a calma,\nestará fora de alcance antes que os tiros possam te atingir.\n\nNão saia da estrada para o pântano ou você será um\nalvo fácil para os bunkers e não conseguirá se mover\nrápido o suficiente para escapar.",
    t12 = "À sua frente há uma base de reabastecimento. Bases de\nreabastecimento fornecem projéteis para você atirar,\nminas para você armar armadilhas secretas, e também\nrestauram a blindagem do tanque ao máximo se ele\nestiver danificado.\n\nPare sobre a base de reabastecimento e carregue seu\ntanque com projéteis.",
    t13 = "Você está sobre a base de reabastecimento. Seu tanque\nagora será carregado com projéteis e minas, como\nmostrarão os indicadores à direita. À sua frente, além\ndos prédios, há um campo minado.",
    t14 = {
      keyboard = "Se você dirigir seu tanque para o campo minado, ele\nserá destruído. Felizmente, você pode limpar um\ncampo minado atirando. Pressione {FIRE} para atirar.\n\nAbra um buraco no prédio com tiros e depois acerte\num projétil no meio do campo minado para detoná-lo.\nQuando você explodir as minas, elas formarão uma\ngrande cratera no chão, pela qual você pode dirigir,\nembora seja tão lento quanto dirigir pelo pântano.",
      touch = "Se você dirigir seu tanque para o campo minado, ele\nserá destruído. Felizmente, você pode limpar um\ncampo minado atirando. Toque no botão de tiro para atirar.\n\nAbra um buraco no prédio com tiros e depois acerte\num projétil no meio do campo minado para detoná-lo.\nQuando você explodir as minas, elas formarão uma\ngrande cratera no chão, pela qual você pode dirigir,\nembora seja tão lento quanto dirigir pelo pântano.",
      controller = "Se você dirigir seu tanque para o campo minado, ele\nserá destruído. Felizmente, você pode limpar um\ncampo minado atirando. Pressione {FIRE} para atirar.\n\nAbra um buraco no prédio com tiros e depois acerte\num projétil no meio do campo minado para detoná-lo.\nQuando você explodir as minas, elas formarão uma\ngrande cratera no chão, pela qual você pode dirigir,\nembora seja tão lento quanto dirigir pelo pântano.",
    },
    t15 = "À frente há outro campo minado. Faça o mesmo truque\nde novo: dirija até a base de reabastecimento, atire\natravés do muro e acerte um projétil no meio do\ncampo minado para detoná-lo.\n\nDesta vez, porém, o campo minado fica ao lado de um\ngrande rio, e a água inundará as crateras. Você pode\ndirigir o tanque pela água como faz pelas crateras\ne pelo pântano.",
    t16 = {
      keyboard = "O WinBolo desloca a janela automaticamente para\nmostrar a área ao redor do seu tanque, mas você pode\nusar as teclas\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\npara substituir manualmente a visão escolhida pelo WinBolo.\n\nPressione {SCROLL_UP} agora para ter uma visão melhor\ndo rio à sua frente.",
      touch = "O WinBolo desloca a janela automaticamente para\nmostrar a área ao redor do seu tanque, mas você pode\narrastar o mapa com o dedo para substituir manualmente\na visão.\n\nArraste o mapa para baixo agora para ver o rio\nà sua frente.",
      controller = "O WinBolo desloca a janela automaticamente para\nmostrar a área ao redor do seu tanque, mas você pode\nempurrar o analógico direito para substituir\nmanualmente a visão escolhida pelo WinBolo.\n\nEmpurre o analógico direito para cima agora para ter\numa visão melhor do rio à sua frente.",
    },
    t17 = "Além de ser muito lento, dirigir pela água tem outra\ndesvantagem: também esgota os suprimentos de\nprojéteis e minas do seu tanque.\n\nQuando chegar do outro lado, seu tanque ainda estará\nem condições de operar, mas você terá perdido toda\na capacidade ofensiva.",
    t18 = {
      keyboard = "Desta vez vamos ser mais sutis ao atravessar o rio.\nEm vez de simplesmente dirigir pela água, vamos\nconstruir uma ponte.\n\nSelecione o modo de cultivo de árvores clicando no\nícone da floresta (canto superior esquerdo da janela)\ne clique na floresta. Seu construtor sairá do tanque\ne colherá uma árvore como material de construção.",
      touch = "Desta vez vamos ser mais sutis ao atravessar o rio.\nEm vez de simplesmente dirigir pela água, vamos\nconstruir uma ponte.\n\nSelecione o modo de cultivo de árvores tocando no\nícone da floresta (canto superior esquerdo da janela)\ne toque na floresta. Seu construtor sairá do tanque\ne colherá uma árvore como material de construção.",
      controller = "Desta vez vamos ser mais sutis ao atravessar o rio.\nEm vez de simplesmente dirigir pela água, vamos\nconstruir uma ponte.\n\nPuxe {BUILD_MODE} para alternar da rolagem para o\nmodo de construção — o analógico direito move agora\num cursor de construção. Pressione {BUILD_TOOL} para\nselecionar o modo de cultivo de árvores (canto superior\nesquerdo), aponte o cursor para uma árvore e pressione\n{BUILD_PLACE}. Seu construtor sairá e colherá uma árvore.",
    },
    t19 = {
      keyboard = "Depois de cultivar algumas árvores, selecione a\nferramenta de estrada/ponte (o segundo botão a\npartir do topo) e clique com o mouse no rio, no fim da estrada.\n\nSeu construtor sairá do tanque e construirá um\ntrecho de ponte. Clique novamente para completar a\nponte e depois atravesse para o outro lado.",
      touch = "Depois de cultivar algumas árvores, selecione a\nferramenta de estrada/ponte (o segundo botão a\npartir do topo) e toque no rio, no fim da estrada.\n\nSeu construtor sairá do tanque e construirá um\ntrecho de ponte. Toque novamente para completar a\nponte e depois atravesse para o outro lado.",
      controller = "Depois de cultivar algumas árvores, pressione {BUILD_TOOL}\npara selecionar a ferramenta de estrada/ponte (o segundo\nbotão a partir do topo), aponte o cursor de construção\npara o rio no fim da estrada e pressione {BUILD_PLACE}.\n\nSeu construtor sairá e construirá um trecho de ponte.\nColoque novamente para completar a ponte, então puxe\n{BUILD_MODE} para sair do modo de construção e\natravesse para o outro lado.",
    },
    t20 = "Este rio é mais largo que o anterior e, no centro,\nfica fundo demais para você construir uma ponte. Se\ntentar simplesmente dirigir, o tanque afundará até o\nfundo e será destruído.",
    t21 = {
      keyboard = "Para atravessar este rio, você terá que construir um\nbarco. Barcos exigem cinco árvores, então clique nas\ncinco árvores para colhê-las. Em seguida, selecione a\nferramenta de muro/barco (o terceiro botão a partir\ndo topo) e clique na água para construir um barco.\n\nSe clicar em terra, você construirá um muro e terá\nque cultivar mais árvores para construir um barco.\nEntre no barco e atravesse o rio até o outro lado.",
      touch = "Para atravessar este rio, você terá que construir um\nbarco. Barcos exigem cinco árvores, então toque nas\ncinco árvores para colhê-las. Em seguida, selecione a\nferramenta de muro/barco (o terceiro botão a partir\ndo topo) e toque na água para construir um barco.\n\nSe tocar em terra, você construirá um muro e terá\nque cultivar mais árvores para construir um barco.\nEntre no barco e atravesse o rio até o outro lado.",
      controller = "Para atravessar este rio, você terá que construir um\nbarco. Barcos exigem cinco árvores, então aponte para\ncinco árvores e pressione {BUILD_PLACE} em cada uma\npara colhê-las. Em seguida, pressione {BUILD_TOOL} para\nselecionar a ferramenta de muro/barco (o terceiro botão)\ne coloque na água.\n\nSe colocar em terra, você construirá um muro e terá\nque cultivar mais árvores. Entre no barco e atravesse\no rio até o outro lado.",
    },
    t22 = "Há um bunker à frente e algumas bases de\nreabastecimento para fornecer munição. Pegue\nprojéteis na base de reabastecimento e ataque o bunker.\n\nEle revidará, mas se seu tanque for atingido, você\npode voltar facilmente à base de reabastecimento\npara consertar a blindagem ao máximo de novo.",
    t23 = "Com paciência e cautela, você deve conseguir destruir\neste bunker. Use as habilidades que aprendeu para\nfacilitar:\n\nEsconda-se na floresta para que o bunker não te veja,\ncultive árvores, construa estradas para o tanque\nse mover mais rápido e construa prédios para se\nproteger dos disparos do bunker.",
    t24 = "Depois de conseguir derrotar este bunker, seu desafio\nfinal do tutorial é eliminar os dois bunkers que\nguardam a saída, construir um barco e escapar.\n\nFique especialmente atento ao nível de blindagem do\ntanque: se ele for destruído, você receberá um tanque\nnovo nas proximidades para tentar de novo.",
    t25 = "Parabéns. Você completou o tutorial do WinBolo.\n\nVocê está pronto para jogar. Entre em uma partida\nonline, hospede uma para seus amigos ou pratique\ncontra bots.",
    respawn1 = "Seu tanque foi destruído — mas você não precisa\nrecomeçar o tutorial. Você recebeu um tanque novo\ndo outro lado da água, onde desembarcou. Volte à\nluta e termine sua fuga.",
  },
}

TEXT["ru"] = {
  pop = {
    start01 = {
      keyboard = "Добро пожаловать в обучение WinBolo. Этот остров\nзнакомит с базовыми принципами WinBolo и проводит\nвас через них по одному. Каждый новый принцип\nбудет описан в окне, подобном этому.\n\nПрочитав сообщение,\nнажмите {DISMISS} для продолжения.",
      touch = "Добро пожаловать в обучение WinBolo. Этот остров\nзнакомит с базовыми принципами WinBolo и проводит\nвас через них по одному. Каждый новый принцип\nбудет описан в окне, подобном этому.\n\nПрочитав сообщение, вы можете продолжить,\nкоснувшись кнопки «OK».",
    },
    start02 = "WinBolo — единственный авторизованный клон классической сетевой игры Bolo для Macintosh от Stuart Cheshire.\nСоветы по стратегии и другую информацию можно найти на официальном сайте:\n\nhttps://www.winbolo.com\n\nИли присоединяйтесь к нам в Reddit и Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = { "Вы управляете танком, который сейчас находится\nна лодке в море. Нажмите {ACCEL}, чтобы танк (и лодка)\nдвигались вперёд, и {BRAKE}, чтобы замедлиться\nи остановиться.", "Удерживайте клавиши, пока танк не наберёт нужную\nскорость, затем отпускайте. Нажмите {ACCEL},\nчтобы лодка поплыла к берегу.\n(Сначала нажмите {DISMISS}, чтобы закрыть окно)" },
      touch = { "Вы управляете танком, который сейчас находится\nна лодке в море. Используйте стик в левой части\nэкрана для управления: толкните вперёд, чтобы\nразогнаться, потяните назад, чтобы замедлиться\nи остановиться.", "Чем сильнее вы толкаете стик, тем быстрее едет\nтанк. Толкните стик вперёд, чтобы лодка поплыла\nк берегу.\n(Сначала коснитесь OK, чтобы закрыть окно)" },
      controller = { "Вы управляете танком, который сейчас находится\nна лодке в море. Толкните левый стик вперёд, чтобы\nтанк (и лодка) двигались вперёд, и потяните назад,\nчтобы замедлиться и остановиться.", "Чем сильнее вы толкаете левый стик, тем быстрее\nедет танк. Толкните левый стик вперёд, чтобы\nлодка поплыла к берегу.\n(Сначала нажмите {DISMISS}, чтобы закрыть окно)" },
    },
    t01 = {
      keyboard = "Перед вами короткая река, ведущая вглубь острова.\nУдерживайте клавишу {ACCEL}, чтобы провести лодку\nдо конца реки.\n\nКогда доедете, продолжайте удерживать {ACCEL}. Танк\nсойдёт с лодки, а лодка останется пришвартованной\nв конце реки.",
      touch = "Перед вами короткая река, ведущая вглубь острова.\nТолкните стик вперёд, чтобы провести лодку\nдо конца реки.\n\nКогда доедете, продолжайте удерживать стик\nвперёд. Танк сойдёт с лодки, а лодка останется\nпришвартованной в конце реки.",
      controller = "Перед вами короткая река, ведущая вглубь острова.\nТолкните левый стик вперёд, чтобы провести лодку\nдо конца реки.\n\nКогда доедете, продолжайте удерживать стик вперёд.\nТанк сойдёт с лодки, а лодка останется\nпришвартованной в конце реки.",
    },
    t02 = {
      keyboard = "Вы на траве. Танк движется по траве\nдовольно быстро.\n\nПродолжайте нажимать {ACCEL}, чтобы продвинуться к лесу.",
      touch = "Вы на траве. Танк движется по траве\nдовольно быстро.\n\nПродолжайте удерживать стик вперёд, чтобы\nпродвинуться к лесу.",
      controller = "Вы на траве. Танк движется по траве\nдовольно быстро.\n\nПродолжайте удерживать левый стик вперёд, чтобы\nпродвинуться к лесу.",
    },
    t03 = {
      keyboard = "Вы в лесу. Танк движется по лесу\nмедленнее, чем по траве.\n\nПродолжайте нажимать {ACCEL}, чтобы продвинуться к болоту.",
      touch = "Вы в лесу. Танк движется по лесу\nмедленнее, чем по траве.\n\nПродолжайте удерживать стик вперёд, чтобы\nпродвинуться к болоту.",
      controller = "Вы в лесу. Танк движется по лесу\nмедленнее, чем по траве.\n\nПродолжайте удерживать левый стик вперёд, чтобы\nпродвинуться к болоту.",
    },
    t04 = {
      keyboard = "Вы в болоте. Танк движется по болоту\nочень медленно. К счастью, впереди есть дорога.\n\nПродолжайте нажимать {ACCEL}, чтобы продвинуться к дороге.",
      touch = "Вы в болоте. Танк движется по болоту\nочень медленно. К счастью, впереди есть дорога.\n\nПродолжайте удерживать стик вперёд, чтобы\nпродвинуться к дороге.",
      controller = "Вы в болоте. Танк движется по болоту\nочень медленно. К счастью, впереди есть дорога.\n\nПродолжайте удерживать левый стик вперёд, чтобы\nпродвинуться к дороге.",
    },
    t05 = {
      keyboard = "Вы на дороге. По дороге танк может двигаться\nочень быстро, и если нажать {ACCEL}, танк ускорится.\n\nЕсли едете слишком быстро, можно нажать {BRAKE}",
      touch = "Вы на дороге. По дороге танк может двигаться\nочень быстро, и если толкнуть стик до упора\nвперёд, он ускорится.\n\nЕсли едете слишком быстро, потяните стик\nназад, чтобы замедлиться.",
      controller = "Вы на дороге. По дороге танк может двигаться\nочень быстро, и если толкнуть левый стик до\nупора вперёд, он ускорится.\n\nЕсли едете слишком быстро, потяните левый стик\nназад, чтобы замедлиться.",
    },
    t06 = {
      keyboard = "Впереди здания. Здания — это сплошные\nпрепятствия, через которые нельзя проехать,\nпоэтому вам придётся искать путь через лабиринт\nна другую сторону.\n\nЧтобы повернуть танк налево, нажмите {LEFT}\nЧтобы повернуть танк направо, нажмите {RIGHT}",
      touch = "Впереди здания. Здания — это сплошные\nпрепятствия, через которые нельзя проехать,\nпоэтому вам придётся искать путь через лабиринт\nна другую сторону.\n\nТолкните стик влево или вправо, чтобы повернуть\nтанк в эту сторону.",
      controller = "Впереди здания. Здания — это сплошные\nпрепятствия, через которые нельзя проехать,\nпоэтому вам придётся искать путь через лабиринт\nна другую сторону.\n\nТолкните левый стик влево или вправо, чтобы\nповернуть танк в эту сторону.",
    },
    t07 = { "Эти круглые объекты с торчащими красными пушками —\nавтоматические бункеры. Они стреляют по любому танку,\nпопавшему в зону поражения.", "К счастью, здания обеспечивают защиту от выстрелов,\nпоэтому бункеры не смогут попасть в вас, пока вы\nне задержитесь так долго, что они полностью\nпробьют здания насквозь." },
    t08 = "Лес тоже защищает от бункеров, но иначе. Помимо\nсильного замедления танка, движение через лес\nограничивает видимость.\n\nНаходясь в лесу, вы по-прежнему чётко видите\nвсё снаружи, но бункеры (и другие игроки)\nне видят ваш танк, если вы не подойдёте к ним\nочень близко.",
    t09 = "Выровняйте танк с серединой полосы леса\nи двигайтесь строго на север. Если ехать прямо\nи не отклоняться слишком близко к краям леса,\nбункеры вас не заметят и стрелять не будут.\n\nНе выезжайте за пределы леса, иначе вас разнесёт.",
    t10 = {
      keyboard = "Есть ещё один простой способ уйти от бункеров —\nскорость.\n\nУбедитесь, что вы выровнены с центром дороги,\nи удерживайте {ACCEL} на полной скорости, чтобы\nпромчаться мимо бункеров.",
      touch = "Есть ещё один простой способ уйти от бункеров —\nскорость.\n\nУбедитесь, что вы выровнены с центром дороги,\nи толкните стик до упора вперёд, чтобы\nпромчаться на полной скорости мимо бункеров.",
      controller = "Есть ещё один простой способ уйти от бункеров —\nскорость.\n\nУбедитесь, что вы выровнены с центром дороги,\nи толкните левый стик до упора вперёд, чтобы\nпромчаться на полной скорости мимо бункеров.",
    },
    t11 = { "Заметив вас, они начнут стрелять, но если ехать\nстрого вперёд и не терять самообладания, вы\nвыйдете из зоны поражения раньше, чем выстрелы\nдостигнут цели.", "Не сворачивайте с дороги в болото, иначе станете\nлёгкой мишенью для бункеров и не сможете\nдвигаться достаточно быстро, чтобы уйти." },
    t12 = "Перед вами база снабжения. Базы снабжения дают\nснаряды для стрельбы, мины для скрытых ловушек,\nа также восстанавливают броню вашего танка\nдо полной прочности, если он повреждён.\n\nОстановитесь на базе снабжения и пополните запас\nснарядов танка.",
    t13 = "Вы на базе снабжения. Ваш танк сейчас пополнит\nзапас снарядов и мин — это покажут индикаторы\nсправа. Впереди, за зданиями, находится\nминное поле.",
    t14 = {
      keyboard = { "Если въехать танком в минное поле, его уничтожит.\nК счастью, мины можно разминировать выстрелами.\nНажмите {FIRE}, чтобы выстрелить.", "Прострелите дыру в здании, а затем попадите\nснарядом в середину минного поля, чтобы взорвать его.\nПри подрыве мин образуется большая воронка\nв земле, через которую можно проехать, хотя\nи так же медленно, как через болото." },
      touch = { "Если въехать танком в минное поле, его уничтожит.\nК счастью, мины можно разминировать выстрелами.\nКоснитесь кнопки выстрела, чтобы стрелять.", "Прострелите дыру в здании, а затем попадите\nснарядом в середину минного поля, чтобы взорвать его.\nПри подрыве мин образуется большая воронка\nв земле, через которую можно проехать, хотя\nи так же медленно, как через болото." },
      controller = { "Если въехать танком в минное поле, его уничтожит.\nК счастью, мины можно разминировать выстрелами.\nНажмите {FIRE}, чтобы выстрелить.", "Прострелите дыру в здании, а затем попадите\nснарядом в середину минного поля, чтобы взорвать его.\nПри подрыве мин образуется большая воронка\nв земле, через которую можно проехать, хотя\nи так же медленно, как через болото." },
    },
    t15 = { "Впереди ещё одно минное поле. Сделайте тот же трюк:\nвъезжайте на базу снабжения, прострелите стену\nи попадите снарядом в середину минного поля,\nчтобы взорвать его.", "Но на этот раз минное поле граничит с большой\nрекой, и вода затопит воронки. По воде танк\nможет ехать так же, как по воронкам и болоту." },
    t16 = {
      keyboard = "WinBolo автоматически прокручивает окно, чтобы\nпоказывать область вокруг танка, но вы можете\nиспользовать клавиши\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT},\nчтобы вручную переопределить вид, выбранный WinBolo.\n\nНажмите {SCROLL_UP}, чтобы получше рассмотреть\nреку впереди.",
      touch = "WinBolo автоматически прокручивает окно, чтобы\nпоказывать область вокруг танка, но вы можете\nперетащить карту пальцем, чтобы вручную\nпереопределить вид.\n\nПеретащите карту вниз, чтобы увидеть\nреку впереди.",
      controller = "WinBolo автоматически прокручивает окно, чтобы\nпоказывать область вокруг танка, но вы можете\nтолкнуть правый стик, чтобы вручную переопределить\nвид, выбранный WinBolo.\n\nТолкните правый стик вверх, чтобы получше\nрассмотреть реку впереди.",
    },
    t17 = "Помимо очень медленного движения, езда по воде\nимеет ещё один недостаток: она также истощает\nзапасы снарядов и мин танка.\n\nКогда доберётесь до другого берега, ваш танк\nостанется на ходу, но потеряет всю огневую мощь.",
    t18 = {
      keyboard = "На этот раз мы поступим хитрее при пересечении реки.\nВместо того чтобы ехать по воде, построим мост.\n\nВыберите режим рубки деревьев, щёлкнув по значку\nлеса (вверху слева окна), и щёлкните по лесу.\nВаш строитель выбежит из танка и срубит одно\nдерево для строительных материалов.",
      touch = "На этот раз мы поступим хитрее при пересечении реки.\nВместо того чтобы ехать по воде, построим мост.\n\nВыберите режим рубки деревьев, коснувшись значка\nлеса (вверху слева окна), и коснитесь леса. Ваш\nстроитель выбежит из танка и срубит одно дерево\nдля строительных материалов.",
      controller = { "На этот раз мы поступим хитрее при пересечении реки.\nВместо того чтобы ехать по воде, построим мост.\n\nПотяните {BUILD_MODE}, чтобы переключиться из\nрежима прокрутки в режим строительства — правый\nстик теперь управляет курсором строительства.", "Нажмите {BUILD_TOOL}, чтобы выбрать режим рубки\nдеревьев (вверху слева), наведите курсор на дерево\nи нажмите {BUILD_PLACE}. Строитель выбежит и срубит\nодно дерево для строительных материалов." },
    },
    t19 = {
      keyboard = "После заготовки деревьев выберите инструмент\nдорога/мост (вторая кнопка сверху) и щёлкните\nмышью по реке в конце дороги.\n\nВаш строитель выбежит из танка и построит\nодин сегмент моста. Щёлкните ещё раз, чтобы\nдостроить мост, и переезжайте на другой берег.",
      touch = "После заготовки деревьев выберите инструмент\nдорога/мост (вторая кнопка сверху) и коснитесь\nреки в конце дороги.\n\nВаш строитель выбежит из танка и построит\nодин сегмент моста. Коснитесь ещё раз, чтобы\nдостроить мост, и переезжайте на другой берег.",
      controller = { "После заготовки деревьев нажмите {BUILD_TOOL}, чтобы\nвыбрать инструмент дорога/мост (вторая кнопка\nсверху), наведите курсор на реку в конце дороги\nи нажмите {BUILD_PLACE}.", "Ваш строитель выбежит и построит один сегмент\nмоста. Разместите ещё раз, чтобы достроить мост,\nзатем потяните {BUILD_MODE}, чтобы выйти из режима\nстроительства, и переезжайте на другой берег." },
    },
    t20 = "Эта река шире предыдущей, и в её центре слишком\nглубоко, чтобы построить мост. Если просто въехать\nв воду, танк опустится на дно и будет уничтожен.",
    t21 = {
      keyboard = { "Чтобы пересечь эту реку, придётся построить лодку.\nЛодки требуют пять деревьев, поэтому щёлкните по\nпяти деревьям, чтобы их срубить.\nЗатем выберите инструмент стена/лодка (третья\nкнопка сверху) и щёлкните по воде, чтобы построить лодку.", "Если щёлкнуть по суше, вы построите стену,\nи придётся срубить ещё деревьев, чтобы построить\nлодку. Въезжайте на лодку и переплывайте на другой берег." },
      touch = { "Чтобы пересечь эту реку, придётся построить лодку.\nЛодки требуют пять деревьев, поэтому коснитесь\nпяти деревьев, чтобы их срубить.\nЗатем выберите инструмент стена/лодка (третья\nкнопка сверху) и коснитесь воды, чтобы построить лодку.", "Если коснуться суши, вы построите стену,\nи придётся срубить ещё деревьев, чтобы построить\nлодку. Въезжайте на лодку и переплывайте на другой берег." },
      controller = { "Чтобы пересечь эту реку, придётся построить лодку.\nЛодки требуют пять деревьев, поэтому наведите\nкурсор на пять деревьев и нажмите {BUILD_PLACE}\nна каждом. Затем нажмите {BUILD_TOOL}, чтобы выбрать\nинструмент стена/лодка (третья кнопка сверху)\nи разместите его на воде.", "Если разместить на суше, получится стена и придётся\nсрубить ещё деревьев. Въезжайте на лодку\nи переплывайте на другой берег." },
    },
    t22 = "Впереди бункер и несколько баз снабжения,\nкоторые обеспечат вас боеприпасами. Получите\nснаряды на базе снабжения и атакуйте бункер.\n\nОн будет отстреливаться, но если ваш танк\nполучит повреждения, можно легко вернуться\nна базу снабжения и восстановить броню\nдо полной прочности.",
    t23 = "С терпением и осторожностью вы сможете уничтожить\nэтот бункер. Используйте полученные навыки,\nчтобы упростить задачу:\n\nПрячьтесь в лесу, чтобы бункер вас не видел,\nрубите деревья, стройте дороги, чтобы танк двигался\nбыстрее, и стройте здания для укрытия от выстрелов\nбункера.",
    t24 = "После того как вы уничтожите этот бункер,\nваше последнее задание в обучении — снять\nдва бункера, охраняющих выход, построить лодку\nи покинуть остров.\n\nОсобенно следите за уровнем брони танка —\nесли танк уничтожат, вы получите новый танк\nнеподалёку и сможете попробовать снова.",
    t25 = "Поздравляем. Вы прошли обучение WinBolo.\n\nВы готовы играть. Присоединяйтесь к игре\nонлайн, создайте свою для друзей или\nпотренируйтесь против ботов.",
    respawn1 = "Ваш танк уничтожен — но начинать обучение заново\nне нужно. Вам выдан новый танк сразу за водой,\nтам, где вы высадились на берег. Вернитесь в бой\nи завершите побег.",
  },
}

TEXT["sv"] = {
  pop = {
    start01 = {
      keyboard = "Välkommen till WinBolo-tutorialen. Den här ön presenterar\ngrundprinciperna i WinBolo och leder dig igenom\ndem ett i taget. Varje ny princip beskrivs\ni ett fönster som det här.\n\nEfter varje meddelande:\ntryck {DISMISS} för att fortsätta.",
      touch = "Välkommen till WinBolo-tutorialen. Den här ön presenterar\ngrundprinciperna i WinBolo och leder dig igenom\ndem ett i taget. Varje ny princip beskrivs\ni ett fönster som det här.\n\nEfter att ha läst varje meddelande kan du fortsätta genom\natt trycka på \"OK\"-knappen.",
    },
    start02 = "WinBolo är den enda auktoriserade klonen av Stuart Cheshires klassiska Macintosh-nätverksspel Bolo.\nDu hittar strategitips och annan information på den officiella webbplatsen:\n\nhttps://www.winbolo.com\n\nEller kom och häng med oss på Reddit och Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Du styr en tank som för närvarande befinner sig på en\nbåt till havs. Tryck {ACCEL} för att tanken (och båten) ska\nåka framåt och tryck {BRAKE} för att sakta ned och stanna.\n\nTryck och håll ned tangenterna tills tanken håller\nönskad hastighet, släpp sedan. Tryck {ACCEL} nu för\natt köra båten framåt mot land.\n(Tryck {DISMISS} först för att stänga fönstret)",
      touch = "Du styr en tank som för närvarande befinner sig på en\nbåt till havs. Använd tumspaken på vänster sida av\nskärmen för att köra: tryck framåt för att accelerera, dra\nbakåt för att sakta ned och stanna.\n\nJu hårdare du trycker tumspaken, desto snabbare går\ntanken. Tryck tumspaken framåt nu för att köra\nbåten mot land.\n(Tryck OK först för att stänga det här fönstret)",
      controller = "Du styr en tank som för närvarande befinner sig på en\nbåt till havs. Tryck vänster spak framåt för att\ntanken (och båten) ska åka framåt, och dra den bakåt\nför att sakta ned och stanna.\n\nJu hårdare du trycker vänster spak, desto snabbare\ngår tanken. Tryck vänster spak framåt nu för att\nköra båten mot land.\n(Tryck {DISMISS} först för att stänga fönstret)",
    },
    t01 = {
      keyboard = "Framför dig finns en kort flod som leder inåt land. Håll\nnedtryckt tangenten {ACCEL} för att köra din båt till\nflodens slut.\n\nNär du kommer dit, fortsätt hålla {ACCEL} nedtryckt. Tanken\nstiger av båten och båten lämnas förtöjd vid flodens slut.",
      touch = "Framför dig finns en kort flod som leder inåt land. Tryck\ntumspaken framåt för att köra din båt till\nflodens slut.\n\nNär du kommer dit, fortsätt hålla tumspaken\nframåttryckt. Tanken stiger av båten och\nbåten lämnas förtöjd vid flodens slut.",
      controller = "Framför dig finns en kort flod som leder inåt land.\nTryck vänster spak framåt för att köra din båt\ntill flodens slut.\n\nNär du kommer dit, fortsätt hålla spaken framåttryckt.\nTanken stiger av båten och båten lämnas förtöjd\nvid flodens slut.",
    },
    t02 = {
      keyboard = "Du befinner dig nu på gräset. Tanken rör sig ganska\nsnabbt på gräs.\n\nFortätt trycka på {ACCEL} för att köra framåt mot skogen.",
      touch = "Du befinner dig nu på gräset. Tanken rör sig ganska\nsnabbt på gräs.\n\nHåll tumspaken tryckt framåt för att köra framåt\nmot skogen.",
      controller = "Du befinner dig nu på gräset. Tanken rör sig ganska\nsnabbt på gräs.\n\nHåll vänster spak tryckt framåt för att köra framåt\nmot skogen.",
    },
    t03 = {
      keyboard = "Du befinner dig nu i skogen. Tanken rör sig långsammare\ni skogen än på gräs.\n\nFortätt trycka på {ACCEL} för att köra framåt mot träsket.",
      touch = "Du befinner dig nu i skogen. Tanken rör sig långsammare\ni skogen än på gräs.\n\nHåll tumspaken tryckt framåt för att köra framåt\nmot träsket.",
      controller = "Du befinner dig nu i skogen. Tanken rör sig långsammare\ni skogen än på gräs.\n\nHåll vänster spak tryckt framåt för att köra framåt\nmot träsket.",
    },
    t04 = {
      keyboard = "Du befinner dig nu i träsket. Tanken rör sig mycket\nlångsamt i träsk. Som tur är finns det en väg framåt.\n\nFortätt trycka på {ACCEL} för att köra framåt till vägen.",
      touch = "Du befinner dig nu i träsket. Tanken rör sig mycket\nlångsamt i träsk. Som tur är finns det en väg framåt.\n\nHåll tumspaken tryckt framåt för att köra framåt\ntill vägen.",
      controller = "Du befinner dig nu i träsket. Tanken rör sig mycket\nlångsamt i träsk. Som tur är finns det en väg framåt.\n\nHåll vänster spak tryckt framåt för att köra framåt\ntill vägen.",
    },
    t05 = {
      keyboard = "Du befinner dig nu på vägen. Tanken kan röra sig mycket\nsnabbt på väg och om du trycker {ACCEL} ökar tanken farten.\n\nOm du tycker att du går för fort kan du trycka {BRAKE}",
      touch = "Du befinner dig nu på vägen. Tanken kan röra sig mycket\nsnabbt på väg och om du trycker tumspaken hela\nvägen framåt ökar farten.\n\nOm du tycker att du kör för fort, dra\ntumspaken bakåt för att sakta ned.",
      controller = "Du befinner dig nu på vägen. Tanken kan röra sig\nmycket snabbt på väg och om du trycker vänster\nspak hela vägen framåt ökar farten.\n\nOm du tycker att du kör för fort, dra vänster\nspak bakåt för att sakta ned.",
    },
    t06 = {
      keyboard = "Det finns några byggnader framåt. Byggnader är solida\nhinder som du inte kan köra igenom, så du måste\nhitta din väg genom labyrinten till den andra sidan.\n\nFör att svänga tanken vänster tryck {LEFT}\nFör att svänga tanken höger tryck {RIGHT}",
      touch = "Det finns några byggnader framåt. Byggnader är solida\nhinder som du inte kan köra igenom, så du måste\nhitta din väg genom labyrinten till den andra sidan.\n\nTryck tumspaken åt vänster eller höger för att svänga\ntanken i den riktningen.",
      controller = "Det finns några byggnader framåt. Byggnader är solida\nhinder som du inte kan köra igenom, så du måste\nhitta din väg genom labyrinten till den andra sidan.\n\nTryck vänster spak åt vänster eller höger för att\nsvänga tanken i den riktningen.",
    },
    t07 = "Dessa runda föremål med röda kanoner som sticker ut är\nautomatiska bunkrar. De skjuter mot varje tank\ninom räckvidd.\n\nLyckligtvis ger byggnaderna skydd mot\nskotten, så bunkrarna kan inte träffa dig\nutan att du väntar så länge att de hinner\nskjuta sig igenom byggnaderna.",
    t08 = "Skogen ger också skydd mot bunkrar, men på ett\nannat sätt. Förutom att bromsa tanken en\nhel del begränsar körning i skogen även sikten.\n\nNär du befinner dig i skogen kan du fortfarande\ntydligt se allt utanför skogen, men bunkrar (och\nandra spelare) kan inte se din tank om de inte\nkommer mycket nära.",
    t09 = "Rikta in din tank mot mitten av skogsremsan\noch kör rakt norrut. Om du kör rakt\noch inte avviker för nära skogskanterna\nkommer bunkrarna inte att se dig och de skjuter inte.\n\nKör inte utanför skogen, annars blir du sprängd.",
    t10 = {
      keyboard = "Det finns ett annat enkelt sätt att undkomma bunkrar –\nfart.\n\nSe till att du är rätt inriktad mot mitten\nav vägen och håll {ACCEL} för att köra i full fart förbi\nbunkrarna.",
      touch = "Det finns ett annat enkelt sätt att undkomma bunkrar –\nfart.\n\nSe till att du är rätt inriktad mot mitten av vägen\noch tryck tumspaken helt framåt för att\nköra i full fart förbi bunkrarna.",
      controller = "Det finns ett annat enkelt sätt att undkomma bunkrar –\nfart.\n\nSe till att du är rätt inriktad mot mitten av vägen\noch tryck vänster spak helt framåt för att\nköra i full fart förbi bunkrarna.",
    },
    t11 = "När de märker dig börjar de skjuta, men om\ndu fortsätter rakt framåt och inte tappar\nnerven är du utom räckvidd innan skotten kan\ntreäffa dig.\n\nKör inte av vägen ut i träsket, annars blir du\nett stillastående mål för bunkrarna och du kan inte\nröra dig snabbt nog för att fly.",
    t12 = "Framför dig finns en försörjningsbas. Försörjningsbaser ger\ndig granater att skjuta, minor att lägga ut hemliga\nfällor med, och de reparerar också tankens pansar\ntill fullt om det har skadats.\n\nStanna på försörjningsbasen och ladda upp tanken\nmed granater.",
    t13 = "Du befinner dig på försörjningsbasen. Din tank\nladdar nu upp med granater och minor, vilket\nindikatorerna till höger visar. Framför dig,\nbortom byggnaderna, finns ett minfält.",
    t14 = {
      keyboard = "Om du kör in i minfältet förstörs tanken. Som tur är kan\ndu röja ett minfält genom att skjuta. Tryck {FIRE} för att skjuta.\n\nSkjut ett hål genom byggnaden och landa sedan ett\nskott i mitten av minfältet för att detonera det.\nNär du spränger minorna skapar de en stor\nkrater i marken som du kan köra igenom,\nom än lika långsamt som i träsk.",
      touch = "Om du kör in i minfältet förstörs tanken. Som tur är kan\ndu röja ett minfält genom att skjuta. Tryck på eldknappen för att skjuta.\n\nSkjut ett hål genom byggnaden och landa sedan ett\nskott i mitten av minfältet för att detonera det.\nNär du spränger minorna skapar de en stor\nkrater i marken som du kan köra igenom,\nom än lika långsamt som i träsk.",
      controller = "Om du kör in i minfältet förstörs tanken. Som tur är\nkan du röja ett minfält genom att skjuta.\nTryck {FIRE} för att skjuta.\n\nSkjut ett hål genom byggnaden och landa sedan ett\nskott i mitten av minfältet för att detonera det.\nNär du spränger minorna skapar de en stor krater\ni marken som du kan köra igenom,\nom än lika långsamt som i träsk.",
    },
    t15 = "Framför finns ytterligare ett minfält. Gör samma sak igen:\nKör till försörjningsbasen, skjut igenom\nmuren och landa ett skott i mitten av minfältet\nför att detonera det.\n\nDen här gången är minfältet dock intill en stor\nflod och vatten flödar in i kratrarna. Du kan\nköra tanken genom vattnet precis som genom\nkratrar och träsk.",
    t16 = {
      keyboard = "WinBolo rullar automatiskt fönstret för att visa\nomgivningen kring din tank, men du kan använda\ntangenterna {SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\nför att manuellt styra vad WinBolo visar.\n\nTryck {SCROLL_UP} nu för att få en bättre vy av\nfloden framför dig.",
      touch = "WinBolo rullar automatiskt fönstret för att visa\nomgivningen kring din tank, men du kan dra kartan\nmed ett finger för att manuellt styra vyn.\n\nDra kartan nedåt nu för att ta in floden\nframför dig i bild.",
      controller = "WinBolo rullar automatiskt fönstret för att visa\nomgivningen kring din tank, men du kan trycka höger\nspak för att manuellt styra vad WinBolo visar.\n\nTryck höger spak uppåt nu för att få en bättre vy\nav floden framför dig.",
    },
    t17 = "Förutom att vara mycket långsamt har körning i vatten\nytterligare en nackdel: Det tömmer också tankens\nförråd av granater och minor.\n\nNär du kommer till den andra sidan är tanken\nfortfarande funktionsduglig, men du har förlorat\nall offensiv förmåga.",
    t18 = {
      keyboard = "Den här gången ska vi vara mer listiga vid\nflodkorsningen. I stället för att köra rakt igenom\nvattnet ska vi bygga en bro.\n\nVälj trädfarmningsläge genom att klicka på skogsikonen\n(uppe till vänster i fönstret) och klicka på skogen.\nDin byggare springer ut ur tanken och skördar ett\nträd som byggmaterial.",
      touch = "Den här gången ska vi vara mer listiga vid\nflodkorsningen. I stället för att köra rakt igenom\nvattnet ska vi bygga en bro.\n\nVälj trädfarmningsläge genom att trycka på skogsikonen\n(uppe till vänster i fönstret) och tryck på skogen. Din\nbyggare springer ut ur tanken och skördar ett träd\nsom byggmaterial.",
      controller = "Den här gången ska vi vara mer listiga vid\nflodkorsningen. I stället för att köra rakt igenom\nvattnet ska vi bygga en bro.\n\nHåll in {BUILD_MODE} för att byta från rullning till\nbyggläge – höger spak styr nu en byggmarkör i stället\nför vyn. Tryck {BUILD_TOOL} för att välja trädfarmningsläge\n(uppe till vänster), rikta markören mot ett träd\noch tryck {BUILD_PLACE}. Din byggare springer ut\noch skördar ett träd som byggmaterial.",
    },
    t19 = {
      keyboard = "Efter att ha samlat lite trä, välj väg/bro-verktyget\n(den andra knappen uppifrån) och klicka med musen\npå floden vid vägens slut.\n\nDin byggare springer ut ur tanken och bygger ett\nbrosegment. Klicka igen för att slutföra bron\noch kör sedan till den andra sidan.",
      touch = "Efter att ha samlat lite trä, välj väg/bro-verktyget\n(den andra knappen uppifrån) och tryck på\nfloden vid vägens slut.\n\nDin byggare springer ut ur tanken och bygger ett\nbrosegment. Tryck igen för att slutföra bron\noch kör sedan till den andra sidan.",
      controller = "Efter att ha samlat lite trä, tryck {BUILD_TOOL} för att\nvälja väg/bro-verktyget (den andra knappen uppifrån),\nrikta byggmarkören mot floden vid vägens slut\noch tryck {BUILD_PLACE}.\n\nDin byggare springer ut och bygger ett brosegment.\nPlacera igen för att slutföra bron, håll sedan in\n{BUILD_MODE} för att lämna byggläget och kör till\nden andra sidan.",
    },
    t20 = "Den här floden är bredare än den förra och i mitten är\nden för djup för att bygga en bro. Om du försöker köra\nrakt igenom sjunker tanken till botten och förstörs.",
    t21 = {
      keyboard = "För att korsa den här floden måste du bygga en båt. Båtar\nkräver fem träd, så klicka på de fem träden för att skörda dem.\nVälj sedan mur/båt-verktyget (den tredje knappen uppifrån)\noch klicka på vattnet för att bygga en båt.\n\nOm du klickar på land bygger du en mur i stället\noch du måste skörda fler träd för att bygga en båt.\nKör upp på båten och kör till den andra sidan.",
      touch = "För att korsa den här floden måste du bygga en båt. Båtar\nkräver fem träd, så tryck på de fem träden för att skörda dem.\nVälj sedan mur/båt-verktyget (den tredje knappen uppifrån)\noch tryck på vattnet för att bygga en båt.\n\nOm du trycker på land bygger du en mur i stället\noch du måste skörda fler träd för att bygga en båt.\nKör upp på båten och kör till den andra sidan.",
      controller = "För att korsa den här floden måste du bygga en båt.\nBåtar kräver fem träd, så rikta mot fem träd och\ntryck {BUILD_PLACE} på vart och ett. Tryck sedan\n{BUILD_TOOL} för att välja mur/båt-verktyget (den tredje\nknappen uppifrån) och placera det på vattnet.\n\nOm du placerar på land bygger du en mur i stället\noch måste skörda fler träd. Kör upp på båten\noch kör till den andra sidan.",
    },
    t22 = "Det finns en bunker framåt och några försörjningsbaser som\nger dig ammunition. Hämta granater från\nförsörjningsbasen och attackera bunkern.\n\nDen skjuter tillbaka, men om din tank träffas kan du enkelt\nåtervända till försörjningsbasen för att reparera pansaret\ntill fullt styrka igen.",
    t23 = "Med tålamod och försiktighet bör du kunna förstöra den\nhär bunkern. Använd de färdigheter du lärt dig för att göra\ndet lättare:\n\nGöm dig i skogen så att bunkern inte kan se dig,\nskörda träd, bygg vägar för att tanken ska röra sig\nsnabbare och bygg byggnader för att ge skydd mot\nbunkereld.",
    t24 = "Efter att du lyckats besegra den här bunkern är din sista\nutmaning i tutorialen att ta ut de två\nbunkrarna som vaktar utgången, bygga en båt och fly.\n\nVar noga med att hålla koll på tankens pansarnivå –\nom din tank förstörs får du en ny tank i närheten\nså att du kan försöka igen.",
    t25 = "Grattis! Du har genomfört WinBolo-tutorialen.\n\nNu är du redo att spela. Gå med i ett spel online,\nskapa ett åt dina vänner eller öva mot bottar.",
    respawn1 = "Din tank förstördes – men du behöver inte börja om\ntutorialen. Du har fått en ny tank alldeles på andra\nsidan vattnet, där du gick i land. Återvänd till striden\noch slutför din flykt.",
  },
}

TEXT["tr"] = {
  pop = {
    start01 = {
      keyboard = "WinBolo eğitimine hoş geldiniz. Bu ada WinBolo'nun\ntemel ilkelerini tanıtıyor ve bunları teker teker\nöğretiyor. Her yeni ilke bu pencere gibi\nbir pencerede açıklanacak.\n\nHer mesajı okuduktan sonra\ndevam etmek için {DISMISS}'a basın.",
      touch = "WinBolo eğitimine hoş geldiniz. Bu ada WinBolo'nun\ntemel ilkelerini tanıtıyor ve bunları teker teker\nöğretiyor. Her yeni ilke bu pencere gibi\nbir pencerede açıklanacak.\n\nHer mesajı okuduktan sonra \"Tamam\" düğmesine\ndokunarak devam edebilirsiniz.",
    },
    start02 = "WinBolo, Stuart Cheshire'in klasik Macintosh ağ oyunu Bolo'nun tek yetkili klonudur.\nStrateji ipuçlarını ve diğer bilgileri resmi web sitesinde bulabilirsiniz:\n\nhttps://www.winbolo.com\n\nReddit ve Discord'da bize katılabilirsiniz\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "Şu anda denizde bir tekne üzerinde bulunan bir tankı\nkontrol ediyorsunuz. Tankı (ve tekneyi) ileri götürmek\niçin {ACCEL}'a, yavaşlatmak ve durdurmak için\n{BRAKE}'e basın.\n\nTuşları tank istediğiniz hıza ulaşana kadar basılı\ntutun, sonra bırakın. Tekneyi karaya doğru sürmek\niçin şimdi {ACCEL}'a basın.\n(Pencereyi kapatmak için önce {DISMISS}'a basın)",
      touch = "Şu anda denizde bir tekne üzerinde bulunan bir tankı\nkontrol ediyorsunuz. Ekranın sol tarafındaki joystick\nile sürün: ileri itmek hızlandırır, geri çekmek\nyavaşlatır ve durdurur.\n\nJoystick'i ne kadar ileri iterseniz tank o kadar hızlanır.\nTekneyi karaya doğru sürmek için şimdi joystick'i\nileri itin.\n(Bu pencereyi kapatmak için önce Tamam'a dokunun)",
      controller = "Şu anda denizde bir tekne üzerinde bulunan bir tankı\nkontrol ediyorsunuz. Tankı (ve tekneyi) ileri götürmek\niçin sol çubuğu ileri itin, yavaşlatmak ve durdurmak\niçin geri çekin.\n\nSol çubuğu ne kadar ileri iterseniz tank o kadar\nhızlanır. Tekneyi karaya doğru sürmek için şimdi\nsol çubuğu ileri itin.\n(Pencereyi kapatmak için önce {DISMISS}'a basın)",
    },
    t01 = {
      keyboard = "Önünüzde kıyıya uzanan kısa bir nehir var. Teknenizi\nnehrin sonuna kadar sürmek için {ACCEL} tuşunu basılı\ntutun.\n\nOraya ulaştığınızda {ACCEL}'ı basmaya devam edin. Tank\ntekneyi terk edecek ve tekne nehrin sonunda\nbağlı kalacak.",
      touch = "Önünüzde kıyıya uzanan kısa bir nehir var. Tekneyi\nnehrin sonuna kadar sürmek için joystick'i ileri itin.\n\nOraya ulaştığınızda joystick'i ileri itmeye\ndevam edin. Tank tekneyi terk edecek ve tekne\nnehrin sonunda bağlı kalacak.",
      controller = "Önünüzde kıyıya uzanan kısa bir nehir var. Tekneyi\nnehrin sonuna kadar sürmek için sol çubuğu\nileri itin.\n\nOraya ulaştığınızda çubuğu ileri itmeye devam edin.\nTank tekneyi terk edecek ve tekne nehrin\nsonunda bağlı kalacak.",
    },
    t02 = {
      keyboard = "Artık çimendeysiniz. Tank çimende oldukça\nhızlı hareket eder.\n\nOrmana doğru ilerlemek için {ACCEL}'a basmaya devam edin.",
      touch = "Artık çimendeysiniz. Tank çimende oldukça\nhızlı hareket eder.\n\nOrmana doğru ilerlemek için joystick'i ileri itmeye devam edin.",
      controller = "Artık çimendeysiniz. Tank çimende oldukça\nhızlı hareket eder.\n\nOrmana doğru ilerlemek için sol çubuğu ileri itmeye\ndevam edin.",
    },
    t03 = {
      keyboard = "Artık ormandayısınız. Tank ormanda çimenden\ndaha yavaş hareket eder.\n\nBataklığa doğru ilerlemek için {ACCEL}'a basmaya devam edin.",
      touch = "Artık ormandayısınız. Tank ormanda çimenden\ndaha yavaş hareket eder.\n\nBataklığa doğru ilerlemek için joystick'i ileri itmeye devam edin.",
      controller = "Artık ormandayısınız. Tank ormanda çimenden\ndaha yavaş hareket eder.\n\nBataklığa doğru ilerlemek için sol çubuğu ileri itmeye\ndevam edin.",
    },
    t04 = {
      keyboard = "Artık bataklıktasınız. Tank bataklıkta çok\nyavaş hareket eder. Neyse ki ileride bir yol var.\n\nYola doğru ilerlemek için {ACCEL}'a basmaya devam edin.",
      touch = "Artık bataklıktasınız. Tank bataklıkta çok\nyavaş hareket eder. Neyse ki ileride bir yol var.\n\nYola doğru ilerlemek için joystick'i ileri itmeye devam edin.",
      controller = "Artık bataklıktasınız. Tank bataklıkta çok\nyavaş hareket eder. Neyse ki ileride bir yol var.\n\nYola doğru ilerlemek için sol çubuğu ileri itmeye\ndevam edin.",
    },
    t05 = {
      keyboard = "Artık yoldasınız. Tank yolda çok hızlı\nhareket edebilir ve {ACCEL}'a basarsanız hız kazanır.\n\nÇok hızlandığınızı fark ederseniz {BRAKE}'e basabilirsiniz",
      touch = "Artık yoldasınız. Tank yolda çok hızlı\nhareket edebilir; joystick'i sonuna kadar ileri itmek\nhızı artırır.\n\nÇok hızlandığınızı fark ederseniz\nyavaşlamak için joystick'i geri çekin.",
      controller = "Artık yoldasınız. Tank yolda çok hızlı\nhareket edebilir; sol çubuğu sonuna kadar ileri itmek\nhızı artırır.\n\nÇok hızlandığınızı fark ederseniz yavaşlamak için\nsol çubuğu geri çekin.",
    },
    t06 = {
      keyboard = "İleride bazı binalar var. Binalar sürüş yapamayacağınız\nkatı engellerdir; labirentten geçerek\nkarşı tarafa geçmeniz gerekecek.\n\nTankı sola döndürmek için {LEFT}'e basın\nTankı sağa döndürmek için {RIGHT}'a basın",
      touch = "İleride bazı binalar var. Binalar sürüş yapamayacağınız\nkatı engellerdir; labirentten geçerek\nkarşı tarafa geçmeniz gerekecek.\n\nTankı o yöne döndürmek için joystick'i sola\nveya sağa itin.",
      controller = "İleride bazı binalar var. Binalar sürüş yapamayacağınız\nkatı engellerdir; labirentten geçerek\nkarşı tarafa geçmeniz gerekecek.\n\nTankı o yöne döndürmek için sol çubuğu sola\nveya sağa itin.",
    },
    t07 = "Kırmızı silahları dışarı uzanan bu yuvarlak nesneler\notomatik bunkerlardır. Menzillerine giren herhangi\nbir tanka ateş ederler.\n\nNeyse ki binalar atışlardan koruma sağlar; bu yüzden\nbunkerlar çok uzun beklemediğiniz sürece sizi\nvuramaz.",
    t08 = "Orman da bunkerlardan koruma sağlar, ancak farklı\nbir şekilde. Tankı önemli ölçüde yavaşlatmanın yanı\nsıra ormandan geçmek görüş mesafesini de kısıtlar.\n\nOrman içindeyken dışarısını net görebilirsiniz, ancak\nbunkerlar (ve diğer oyuncular) çok yaklaşmadıkça\ntankınızı göremez.",
    t09 = "Tankınızı orman şeridinin ortasıyla hizalayıp tam\nKuzeye doğru sürün. Düzgün gidip ormanın kenarlarına\nyaklaşmazsanız bunkerlar sizi görmez ve ateş etmez.\n\nOrmanın dışına çıkmayın, yoksa paramparça olursunuz.",
    t10 = {
      keyboard = "Bunkerlardan kaçmanın başka bir yolu daha var:\nhız.\n\nYolun merkezine düzgünce hizalandığınızdan emin olun\nve bunkerların yanından tam hızla geçmek için\n{ACCEL}'ı basılı tutun.",
      touch = "Bunkerlardan kaçmanın başka bir yolu daha var:\nhız.\n\nYolun merkezine düzgünce hizalandığınızdan emin olun\nve bunkerların yanından tam hızla geçmek için\njoystick'i sonuna kadar ileri itin.",
      controller = "Bunkerlardan kaçmanın başka bir yolu daha var:\nhız.\n\nYolun merkezine düzgünce hizalandığınızdan emin olun\nve bunkerların yanından tam hızla geçmek için sol\nçubuğu sonuna kadar ileri itin.",
    },
    t11 = "Sizi fark ettiklerinde ateş etmeye başlayacaklar; ancak\ndüz gitmeye devam edip cesaretinizi yitirmezseniz\natışlar size ulaşmadan menzil dışına çıkarsınız.\n\nYoldan çıkıp bataklığa gitmeyin, yoksa bunkerlar için\nkolay hedef olursunuz ve kaçacak kadar hızlanamassınız.",
    t12 = "Önünüzde bir ikmal üssü var. İkmal üsleri ateş etmeniz\niçin mermiler, gizli tuzaklar kurmak için mayınlar sağlar\nve hasar görmüş tankınızın zırhını da tamamen onarır.\n\nİkmal üssüne durun ve tankınızı\nmermiyle doldurun.",
    t13 = "İkmal üssündesiniz. Tankınız şimdi sağdaki\ngöstergelerden görebileceğiniz gibi mermiler ve mayınlarla\ndolacak. Önünüzde, binaların ötesinde\nbir mayın tarlası var.",
    t14 = {
      keyboard = "Tankınızı mayın tarlasına sürerseniz tahrip\nolur. Neyse ki ateş ederek mayın tarlasını temizleyebilirsiniz. Ateş etmek için {FIRE}'e basın.\n\nBinada delik açın, ardından mayın tarlasının ortasına\nbir mermi düşürerek patlatın.\nMayınları patlatınca yerde büyük bir krater oluşur;\nbataklıkta sürmek kadar yavaş da olsa tankla\ngeçebilirsiniz.",
      touch = "Tankınızı mayın tarlasına sürerseniz tahrip\nolur. Neyse ki ateş ederek mayın tarlasını temizleyebilirsiniz. Ateş etmek için ateş düğmesine dokunun.\n\nBinada delik açın, ardından mayın tarlasının ortasına\nbir mermi düşürerek patlatın.\nMayınları patlatınca yerde büyük bir krater oluşur;\nbataklıkta sürmek kadar yavaş da olsa tankla\ngeçebilirsiniz.",
      controller = "Tankınızı mayın tarlasına sürerseniz tahrip\nolur. Neyse ki ateş ederek mayın tarlasını temizleyebilirsiniz. Ateş etmek için {FIRE}'e basın.\n\nBinada delik açın, ardından mayın tarlasının ortasına\nbir mermi düşürerek patlatın.\nMayınları patlatınca yerde büyük bir krater oluşur;\nbataklıkta sürmek kadar yavaş da olsa tankla\ngeçebilirsiniz.",
    },
    t15 = "İleride başka bir mayın tarlası var. Aynı taktiği\nuygulayın: İkmal üssüne sürün, duvardan geçerek\nateş edin ve mayın tarlasının ortasına mermi\ndüşürerek patlatın.\n\nAncak bu sefer mayın tarlası geniş bir nehrin yanında\nve su kraterlere dolacak. Krater ve bataklıktan\ngeçer gibi tankla su içinden de geçebilirsiniz.",
    t16 = {
      keyboard = "WinBolo pencereyi tankınızın çevresini göstermek için\notomatik kaydırır, ancak görünümü kendiniz\nayarlamak için\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\ntuşlarını kullanabilirsiniz.\n\nÖnünüzdeki nehri daha iyi görmek için şimdi {SCROLL_UP}'a basın.",
      touch = "WinBolo pencereyi tankınızın çevresini göstermek için\notomatik kaydırır, ancak görünümü kendiniz ayarlamak\niçin haritayı parmağınızla sürükleyebilirsiniz.\n\nÖnünüzdeki nehri görünüme almak için\nharitayı aşağı sürükleyin.",
      controller = "WinBolo pencereyi tankınızın çevresini göstermek için\notomatik kaydırır, ancak görünümü kendiniz\nayarlamak için sağ çubuğu kullanabilirsiniz.\n\nÖnünüzdeki nehri daha iyi görmek için şimdi sağ\nçubuğu yukarı itin.",
    },
    t17 = "Suda sürmenin çok yavaş olmasının yanı sıra\nbaşka bir dezavantajı daha var: Tank mermi ve\nmayın stoklarınızı da tüketir.\n\nKarşı tarafa geçtiğinizde tankınız çalışır durumda\nolacak ama tüm saldırı kapasitesini yitirmiş olacaksınız.",
    t18 = {
      keyboard = "Bu sefer nehri geçmekte daha ince davranacağız.\nSudan direkt geçmek yerine köprü inşa edeceğiz.\n\nOrman simgesine tıklayarak (pencerenin sol üstü) ağaç\nçiftçiliği modunu seçin ve ormana tıklayın.\nİnşaatçınız tanktan çıkıp bir ağaç toplayacak.",
      touch = "Bu sefer nehri geçmekte daha ince davranacağız.\nSudan direkt geçmek yerine köprü inşa edeceğiz.\n\nOrman simgesine dokunarak (pencerenin sol üstü)\nağaç çiftçiliği modunu seçin ve ormana dokunun.\nİnşaatçınız tanktan çıkıp bir ağaç toplayacak.",
      controller = "Bu sefer nehri geçmekte daha ince davranacağız.\nSudan direkt geçmek yerine köprü inşa edeceğiz.\n\n{BUILD_MODE}'a çekerek kaydırma modundan inşa moduna\ngeçin — sağ çubuk artık görünüm yerine inşa imlecini\ntaşır. Ağaç çiftçiliği modunu seçmek için {BUILD_TOOL}'a basın\n(pencerenin sol üstü), imleci bir ağaca yöneltin\nve {BUILD_PLACE}'e basın. İnşaatçınız tanktan çıkıp\nbir ağaç toplayacak.",
    },
    t19 = {
      keyboard = "Birkaç ağaç topladıktan sonra yol/köprü aracını\n(üstten ikinci düğme) seçin ve yolun sonundaki\nnehre tıklayın.\n\nİnşaatçınız tanktan çıkıp bir köprü bölümü inşa edecek.\nKöprüyü tamamlamak için tekrar tıklayın ve\nkarşı tarafa geçin.",
      touch = "Birkaç ağaç topladıktan sonra yol/köprü aracını\n(üstten ikinci düğme) seçin ve yolun sonundaki\nnehre dokunun.\n\nİnşaatçınız tanktan çıkıp bir köprü bölümü inşa edecek.\nKöprüyü tamamlamak için tekrar dokunun ve\nkarşı tarafa geçin.",
      controller = "Birkaç ağaç topladıktan sonra yol/köprü aracını seçmek\niçin {BUILD_TOOL}'a basın (üstten ikinci düğme), inşa\nimlecini yolun sonundaki nehre yöneltin ve {BUILD_PLACE}'e basın.\n\nİnşaatçınız tanktan çıkıp bir köprü bölümü inşa edecek.\nKöprüyü tamamlamak için tekrar yerleştirin, ardından inşa\nmodundan çıkmak için {BUILD_MODE}'a çekin ve karşıya geçin.",
    },
    t20 = "Bu nehir sonuncusundan daha geniş ve ortasında\nköprü inşa etmek için çok derin. Doğrudan geçmeye\nçalışırsanız tank dibe batacak ve tahrip olacak.",
    t21 = {
      keyboard = "Bu nehri geçmek için tekne yapmanız gerekecek. Tekneler\nbeş ağaç gerektirir; beş ağaca tıklayarak toplayın.\nArdından duvar/tekne aracını (üstten üçüncü düğme)\nseçin ve tekne yapmak için suya tıklayın.\n\nKaraya tıklarsanız tekne yerine duvar yaparsınız\nve tekne için daha fazla ağaç toplamanız gerekir.\nTekneye binip nehrin karşı tarafına geçin.",
      touch = "Bu nehri geçmek için tekne yapmanız gerekecek. Tekneler\nbeş ağaç gerektirir; beş ağaca dokunarak toplayın.\nArdından duvar/tekne aracını (üstten üçüncü düğme)\nseçin ve tekne yapmak için suya dokunun.\n\nKaraya dokunursanız tekne yerine duvar yaparsınız\nve tekne için daha fazla ağaç toplamanız gerekir.\nTekneye binip nehrin karşı tarafına geçin.",
      controller = "Bu nehri geçmek için tekne yapmanız gerekecek. Tekneler\nbeş ağaç gerektirir; beş ağaca nişan alıp her birine\n{BUILD_PLACE}'e basarak toplayın. Ardından duvar/tekne\naracını seçmek için {BUILD_TOOL}'a basın (üstten üçüncü\ndüğme) ve suya yerleştirin.\n\nKaraya yerleştirirseniz tekne yerine duvar yaparsınız;\ntekne için daha fazla ağaç toplamanız gerekir. Tekneye\nbinip nehrin karşı tarafına geçin.",
    },
    t22 = "İleride bir bunker ve cephane sağlayan ikmal üsleri var.\nİkmal üssünden mermileri toplayın ve bunkera saldırın.\n\nBunker ateş edecek, ama tankınız vurulursa ikmal üssüne\nkolayca dönüp zırhınızı tekrar tam kapasiteye\ntamirletebilirsiniz.",
    t23 = "Sabır ve dikkatle bu bunkeri yok edebilirsiniz.\nİşi kolaylaştırmak için öğrendiklerinizi kullanın:\n\nBunker sizi göremesin diye ormana saklanın,\nağaç toplayın, tankın daha hızlı hareket etmesi için\nyollar inşa edin ve bunker atışlarına karşı siper\nolması için binalar yapın.",
    t24 = "Bu bunkeri yenmeyi başardıktan sonra eğitimin\nson görevi: çıkışı koruyan iki bunkeri temizlemek,\nbir tekne inşa etmek ve kaçmak.\n\nTankınızın zırh seviyesine özellikle dikkat edin;\ntankınız tahrip olursa yakınınıza yeni bir tank\nverilecek ve tekrar deneyebileceksiniz.",
    t25 = "Tebrikler. WinBolo\neğitimini tamamladınız.\n\nArtık oynamaya hazırsınız. Çevrimiçi bir oyuna\nkatılın, arkadaşlarınız için bir oyun kurun ya da\nbotlara karşı pratik yapın.",
    respawn1 = "Tankınız tahrip oldu — ama eğitime baştan başlamanıza\ngerek yok. Karaya çıktığınız yerin hemen karşısında\nyeni bir tank verildi. Savaşa geri dönün ve kaçışı\ntamamlayın.",
  },
}

TEXT["uk"] = {
  pop = {
    start01 = {
      keyboard = "Ласкаво просимо до навчання WinBolo. Цей острів знайомить\nвас з основними принципами WinBolo і веде вас\nкрізь них по одному. Кожен новий принцип буде\nописано у вікні, подібному до цього.\n\nПрочитавши кожне повідомлення,\nнатисніть {DISMISS} для продовження.",
      touch = "Ласкаво просимо до навчання WinBolo. Цей острів знайомить\nвас з основними принципами WinBolo і веде вас\nкрізь них по одному. Кожен новий принцип буде\nописано у вікні, подібному до цього.\n\nПрочитавши кожне повідомлення, ви можете продовжити,\nторкнувшись кнопки «OK».",
    },
    start02 = "WinBolo — єдиний авторизований клон класичної мережевої гри Stuart Cheshire для Macintosh — Bolo.\nСтратегічні підказки та іншу інформацію можна знайти на офіційному сайті:\n\nhttps://www.winbolo.com\n\nАбо приєднуйтесь до нас на Reddit та Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = { "Ви керуєте танком, який зараз знаходиться на\nчовні в морі. Натисніть {ACCEL}, щоб рухати танк (і човен) вперед,\nі натисніть {BRAKE}, щоб сповільнитись і\nзупинитись.", "Натисніть і утримуйте клавіші, доки танк не рухатиметься\nз потрібною швидкістю, а потім відпустіть. Натисніть {ACCEL},\nщоб рушити човен вперед до суші.\n(Спочатку натисніть {DISMISS}, щоб закрити це вікно)" },
      touch = { "Ви керуєте танком, який зараз знаходиться на\nчовні в морі. Використовуйте джойстик з лівого боку\nекрана для керування: штовхніть його вперед для прискорення, потягніть\nназад для сповільнення і зупинки.", "Чим далі ви штовхаєте джойстик, тим швидше\nрухається танк. Штовхніть джойстик вперед, щоб рушити\nчовен вперед до суші.\n(Спочатку торкніться OK, щоб закрити це вікно)" },
      controller = { "Ви керуєте танком, який зараз знаходиться на\nчовні в морі. Штовхніть лівий стік вперед, щоб\nрухати танк (і човен) вперед, і потягніть назад, щоб\nсповільнитись і зупинитись.", "Чим далі ви штовхаєте лівий стік, тим швидше\nрухається танк. Штовхніть лівий стік вперед, щоб\nрушити човен до суші.\n(Спочатку натисніть {DISMISS}, щоб закрити це вікно)" },
    },
    t01 = {
      keyboard = "Попереду невелика річка, що веде вглиб суші. Утримуйте\nклавішу {ACCEL}, щоб керувати човном до кінця\nрічки.\n\nКоли доберетесь туди, продовжуйте утримувати {ACCEL}. Танк\nзійде з човна, а човен залишиться\nпришвартованим наприкінці річки.",
      touch = "Попереду невелика річка, що веде вглиб суші. Штовхніть\nджойстик вперед, щоб керувати човном до кінця\nрічки.\n\nКоли доберетесь туди, тримайте джойстик\nштовхнутим вперед. Танк зійде з човна, а\nчовен залишиться пришвартованим наприкінці річки.",
      controller = "Попереду невелика річка, що веде вглиб суші. Штовхніть\nлівий стік вперед, щоб керувати човном до кінця\nрічки.\n\nКоли доберетесь туди, тримайте стік штовхнутим\nвперед. Танк зійде з човна, а човен залишиться\nпришвартованим наприкінці річки.",
    },
    t02 = {
      keyboard = "Тепер ви на траві. Танк рухається досить\nшвидко по траві.\n\nПродовжуйте натискати {ACCEL}, щоб рухатись вперед до лісу.",
      touch = "Тепер ви на траві. Танк рухається досить\nшвидко по траві.\n\nТримайте джойстик штовхнутим вперед, щоб рухатись\nдо лісу.",
      controller = "Тепер ви на траві. Танк рухається досить\nшвидко по траві.\n\nТримайте лівий стік штовхнутим вперед, щоб рухатись\nдо лісу.",
    },
    t03 = {
      keyboard = "Тепер ви у лісі. Танк рухається повільніше\nу лісі, ніж по траві.\n\nПродовжуйте натискати {ACCEL}, щоб рухатись вперед до болота.",
      touch = "Тепер ви у лісі. Танк рухається повільніше\nу лісі, ніж по траві.\n\nТримайте джойстик штовхнутим вперед, щоб рухатись\nдо болота.",
      controller = "Тепер ви у лісі. Танк рухається повільніше\nу лісі, ніж по траві.\n\nТримайте лівий стік штовхнутим вперед, щоб рухатись\nдо болота.",
    },
    t04 = {
      keyboard = "Тепер ви на болоті. Танк рухається дуже\nповільно на болоті. На щастя, попереду є дорога.\n\nПродовжуйте натискати {ACCEL}, щоб рухатись вперед до дороги.",
      touch = "Тепер ви на болоті. Танк рухається дуже\nповільно на болоті. На щастя, попереду є дорога.\n\nТримайте джойстик штовхнутим вперед, щоб рухатись\nдо дороги.",
      controller = "Тепер ви на болоті. Танк рухається дуже\nповільно на болоті. На щастя, попереду є дорога.\n\nТримайте лівий стік штовхнутим вперед, щоб рухатись\nдо дороги.",
    },
    t05 = {
      keyboard = "Тепер ви на дорозі. Танк може рухатись дуже\nшвидко по дорозі, і якщо ви натиснете {ACCEL}, танк\nприскориться.\n\nЯкщо ви рухаєтесь надто швидко, ви можете натиснути {BRAKE}",
      touch = "Тепер ви на дорозі. Танк може рухатись дуже\nшвидко по дорозі, і якщо штовхнути джойстик\nповністю вперед, він прискориться.\n\nЯкщо ви рухаєтесь надто швидко, потягніть\nджойстик назад, щоб сповільнитись.",
      controller = "Тепер ви на дорозі. Танк може рухатись дуже\nшвидко по дорозі, і якщо штовхнути лівий стік\nповністю вперед, він прискориться.\n\nЯкщо ви рухаєтесь надто швидко, потягніть лівий\nстік назад, щоб сповільнитись.",
    },
    t06 = {
      keyboard = "Попереду кілька будівель. Будівлі — суцільні\nперешкоди, крізь які не можна проїхати, тому вам\nдоведеться знайти шлях через лабіринт до іншого\nбоку.\n\nЩоб повернути танк ліворуч, натисніть {LEFT}\nЩоб повернути танк праворуч, натисніть {RIGHT}",
      touch = "Попереду кілька будівель. Будівлі — суцільні\nперешкоди, крізь які не можна проїхати, тому вам\nдоведеться знайти шлях через лабіринт до іншого\nбоку.\n\nШтовхніть джойстик ліворуч або праворуч, щоб повернути\nтанк у відповідному напрямку.",
      controller = "Попереду кілька будівель. Будівлі — суцільні\nперешкоди, крізь які не можна проїхати, тому вам\nдоведеться знайти шлях через лабіринт до іншого\nбоку.\n\nШтовхніть лівий стік ліворуч або праворуч, щоб\nповернути танк у відповідному напрямку.",
    },
    t07 = { "Ці круглі об'єкти з червоними гарматами, що стирчать — це\nавтоматичні бункери. Вони стрілятимуть у будь-який танк,\nщо потрапить у зону досяжності.", "На щастя, будівлі захищають від\nпострілів, тому бункери не зможуть влучити у вас,\nякщо ви не чекаєте так довго, що вони встигнуть\nповністю прострілити крізь будівлі." },
    t08 = { "Ліс також захищає від бункерів, але\nіншим чином. Крім того, що він сильно сповільнює танк,\nпроїзд через ліс також обмежує видимість.", "Коли ви всередині лісу, ви ще можете чітко бачити\nвсе за межами лісу, але бункери (і\nінші гравці) не бачать ваш танк, якщо ви не\nпідійдете дуже близько до них." },
    t09 = "Вирівняйте ваш танк по центру смуги\nлісу і їдьте строго на північ. Якщо ви їдете прямо\nі не підходите надто близько до країв лісу,\nбункери не побачать вас і не стрілятимуть.\n\nНе виїжджайте з лісу, бо вас рознесе на шматки.",
    t10 = {
      keyboard = "Є ще один простий спосіб уникнути бункерів —\nшвидкість.\n\nПереконайтесь, що ви правильно вирівняні по центру\nдороги, і утримуйте {ACCEL}, щоб проїхати повз бункери\nна повній швидкості.",
      touch = "Є ще один простий спосіб уникнути бункерів —\nшвидкість.\n\nПереконайтесь, що ви правильно вирівняні по центру\nдороги, і штовхніть джойстик повністю вперед,\nщоб проїхати повз бункери на повній швидкості.",
      controller = "Є ще один простий спосіб уникнути бункерів —\nшвидкість.\n\nПереконайтесь, що ви правильно вирівняні по центру\nдороги, і штовхніть лівий стік повністю вперед,\nщоб проїхати повз бункери на повній швидкості.",
    },
    t11 = { "Коли вони помітять вас, вони почнуть стріляти, але якщо\nви продовжуватимете їхати прямо і не втратите\nнерви, ви виїдете з зони досяжності раніше, ніж постріли\nзможуть влучити в вас.", "Не з'їжджайте з дороги на болото, бо ви станете\nлегкою мішенню для бункерів, і ви не зможете\nрухатись достатньо швидко, щоб утекти." },
    t12 = "Попереду база поповнення. Бази поповнення надають\nснаряди для стрільби, міни для встановлення таємних пасток,\nі вони також відновлюють броню вашого танка до повної\nміцності, якщо вона була пошкоджена.\n\nЗупиніться на базі поповнення і завантажте ваш танк\nснарядами.",
    t13 = "Ви на базі поповнення. Ваш танк тепер\nзавантажиться снарядами і мінами — індикатори\nправоруч покажуть це. Попереду, за\nбудівлями, знаходиться мінне поле.",
    t14 = {
      keyboard = { "Якщо ви заїдете вашим танком у мінне поле, його буде\nзнищено. На щастя, ви можете очистити мінне поле,\nстріляючи. Натисніть {FIRE}, щоб вистрілити.", "Прострілте отвір у будівлі, а потім влучте\nснарядом у центр мінного поля, щоб підірвати його.\nКоли ви підриваєте міни, вони залишать великий\nкратер у землі, крізь який можна проїхати,\nхоча це так само повільно, як і болото." },
      touch = { "Якщо ви заїдете вашим танком у мінне поле, його буде\nзнищено. На щастя, ви можете очистити мінне поле,\nстріляючи. Торкніться кнопки вогню, щоб вистрілити.", "Прострілте отвір у будівлі, а потім влучте\nснарядом у центр мінного поля, щоб підірвати його.\nКоли ви підриваєте міни, вони залишать великий\nкратер у землі, крізь який можна проїхати,\nхоча це так само повільно, як і болото." },
      controller = { "Якщо ви заїдете вашим танком у мінне поле, його буде\nзнищено. На щастя, ви можете очистити мінне поле,\nстріляючи. Натисніть {FIRE}, щоб вистрілити.", "Прострілте отвір у будівлі, а потім влучте\nснарядом у центр мінного поля, щоб підірвати його.\nКоли ви підриваєте міни, вони залишать великий\nкратер у землі, крізь який можна проїхати,\nхоча це так само повільно, як і болото." },
    },
    t15 = { "Попереду ще одне мінне поле. Зробіть те саме:\nПроїдьте до бази поповнення, прострілте\nстіну і влучте снарядом у центр мінного поля, щоб\nпідірвати його.", "Цього разу мінне поле знаходиться поруч із великою\nрічкою, і вода затопить кратери. Ви можете\nпроїхати танком крізь воду, як і крізь\nкратери і болото." },
    t16 = {
      keyboard = "WinBolo автоматично прокручує вікно, щоб показати\nвам область навколо вашого танка, але ви можете використовувати\nклавіші {SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT},\nщоб вручну змінити вид.\n\nНатисніть {SCROLL_UP}, щоб краще побачити\nрічку попереду.",
      touch = "WinBolo автоматично прокручує вікно, щоб показати вам\nобласть навколо вашого танка, але ви можете перетягнути карту\nпальцем, щоб вручну змінити вид.\n\nПеретягніть карту вниз, щоб побачити річку\nпопереду.",
      controller = "WinBolo автоматично прокручує вікно, щоб показати\nвам область навколо вашого танка, але ви можете\nштовхнути правий стік, щоб вручну змінити вид.\n\nШтовхніть правий стік вгору, щоб краще побачити\nрічку попереду.",
    },
    t17 = "Крім того, що їзда крізь воду дуже повільна, вона має\nще один недолік: вона також витратить\nзапаси снарядів і мін вашого танка.\n\nКоли ви дістанетесь іншого берегу, ваш танк ще буде\nпрацездатним, але ви втратите всю\nнаступальну можливість.",
    t18 = {
      keyboard = { "Цього разу ми підійдемо до переправи через річку\nтонше. Замість того, щоб просто проїхати крізь\nводу, ми побудуємо міст.", "Виберіть режим збору дерев, натиснувши на значок лісу\n(у верхньому лівому кутку вікна) і натисніть на ліс.\nВаш будівельник вибіжить з танка і зрубає одне\nдерево для будівельних матеріалів." },
      touch = { "Цього разу ми підійдемо до переправи через річку\nтонше. Замість того, щоб просто проїхати крізь\nводу, ми побудуємо міст.", "Виберіть режим збору дерев, торкнувшись значка лісу\n(у верхньому лівому кутку вікна) і торкніться лісу. Ваш\nбудівельник вибіжить з танка і зрубає одне дерево\nдля будівельних матеріалів." },
      controller = { "Цього разу ми підійдемо до переправи через річку\nтонше. Замість того, щоб просто проїхати крізь\nводу, ми побудуємо міст.\n\nПотягніть {BUILD_MODE}, щоб перейти зі скролінгу в режим\nбудівництва — правий стік тепер переміщує курсор будівництва.", "Натисніть {BUILD_TOOL}, щоб вибрати режим збору дерев\n(у верхньому лівому кутку), наведіть курсор на дерево\nі натисніть {BUILD_PLACE}. Ваш будівельник вийде з\nтанка і зрубає одне дерево для будівництва." },
    },
    t19 = {
      keyboard = "Після збору дерев виберіть інструмент «дорога/міст»\n(другу кнопку зверху) і натисніть мишею\nна річку в кінці дороги.\n\nВаш будівельник вибіжить з танка і побудує одну\nсекцію мосту. Натисніть ще раз, щоб завершити міст,\nа потім переїдьте на інший берег.",
      touch = "Після збору дерев виберіть інструмент «дорога/міст»\n(другу кнопку зверху) і торкніться\nрічки в кінці дороги.\n\nВаш будівельник вибіжить з танка і побудує одну\nсекцію мосту. Торкніться ще раз, щоб завершити міст,\nа потім переїдьте на інший берег.",
      controller = { "Після збору дерев натисніть {BUILD_TOOL}, щоб вибрати\nінструмент «дорога/міст» (другу кнопку зверху),\nнаведіть курсор будівництва на річку в кінці дороги\nі натисніть {BUILD_PLACE}.", "Ваш будівельник побудує одну секцію мосту. Розмістіть\nще раз, щоб завершити міст, потім потягніть {BUILD_MODE},\nщоб вийти з режиму будівництва, і переїдьте." },
    },
    t20 = "Ця річка ширша за попередню, і в центрі вона\nнадто глибока для будівництва мосту. Якщо ви спробуєте\nпросто проїхати, танк потоне і буде\nзнищено.",
    t21 = {
      keyboard = { "Щоб перетнути цю річку, вам доведеться збудувати човен. Човни\nпотребують п'ять дерев, тому натисніть на п'ять дерев, щоб зрубати їх.\nПотім виберіть інструмент «стіна/човен» (третю кнопку зверху)\nі натисніть на воду, щоб збудувати човен.", "Якщо ви натиснете на сушу, ви збудуєте стіну\nі вам доведеться зрубати ще дерев, щоб збудувати\nчовен. Заїдьте на човен і переправтесь на інший берег." },
      touch = { "Щоб перетнути цю річку, вам доведеться збудувати човен. Човни\nпотребують п'ять дерев, тому торкніться п'яти дерев, щоб зрубати їх.\nПотім виберіть інструмент «стіна/човен» (третю кнопку зверху)\nі торкніться води, щоб збудувати човен.", "Якщо ви торкнетесь суші, ви збудуєте стіну\nі вам доведеться зрубати ще дерев, щоб збудувати\nчовен. Заїдьте на човен і переправтесь на інший берег." },
      controller = { "Щоб перетнути цю річку, вам доведеться збудувати\nчовен. Човни потребують п'ять дерев; наведіть на\nп'ять дерев і натисніть {BUILD_PLACE} на кожному.\nПотім натисніть {BUILD_TOOL}, щоб вибрати інструмент\n«стіна/човен» (третю кнопку зверху), і розмістіть на воді.", "Якщо розмістите на суші — збудуєте стіну і доведеться\nзрубати ще дерев. Заїдьте на човен і переправтесь." },
    },
    t22 = { "Попереду бункер і кілька баз поповнення,\nщо забезпечать вас боєприпасами. Зберіть снаряди з\nбази поповнення і атакуйте бункер.", "Він стрілятиме у відповідь, але якщо ваш танк отримає\nпопадання, ви легко можете повернутись до бази поповнення,\nщоб відновити броню до повної міцності." },
    t23 = "З терпінням і обережністю ви зможете знищити цей\nбункер. Використовуйте набуті навички, щоб полегшити завдання:\n\nПереховуйтесь у лісі, щоб бункер вас не бачив,\nрубайте дерева, будуйте дороги, щоб танк рухався\nшвидше, і зводьте будівлі як укриття від\nпострілів бункера.",
    t24 = { "Після того як знищите цей бункер, вашим фінальним\nвипробуванням у навчанні буде знешкодити два\nбункери, що охороняють вихід, збудувати човен і втекти.", "Особливо стежте за рівнем броні вашого танка —\nякщо ваш танк буде знищено, вам нададуть новий\nтанк поблизу, щоб ви могли спробувати ще раз." },
    t25 = "Вітаємо! Ви завершили навчання WinBolo.\n\nВи готові грати. Приєднайтеся до гри онлайн,\nстворіть гру для друзів або потренуйтеся проти\nботів.",
    respawn1 = "Ваш танк знищено — але вам не потрібно починати\nнавчання спочатку. Вам надано новий танк прямо\nпоблизу місця висадки. Повертайтесь у бій і\nзавершіть втечу.",
  },
}

TEXT["zh-cn"] = {
  pop = {
    start01 = {
      keyboard = "欢迎来到 WinBolo 教程。这座岛屿将逐一介绍 WinBolo 的基本原理。\n每个新原理都会在如此窗口中描述。\n\n阅读每条消息后，\n按 {DISMISS} 继续。",
      touch = "欢迎来到 WinBolo 教程。这座岛屿将逐一介绍 WinBolo 的基本原理。\n每个新原理都会在如此窗口中描述。\n\n阅读每条消息后，可通过\n点击\"确定\"按钮继续。",
    },
    start02 = "WinBolo 是 Stuart Cheshire 经典 Macintosh 网络游戏 Bolo 的唯一授权克隆版。\n您可以在官方网站找到策略提示和其他信息：\n\nhttps://www.winbolo.com\n\n或者加入我们的 Reddit 和 Discord 社区\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "您正在控制一辆坦克，目前在海上的船中。\n按 {ACCEL} 使坦克（和船）前进，按 {BRAKE} 减速停止。\n\n按住按键，直到坦克达到所需速度后再松手。\n现在按 {ACCEL} 使船向陆地前进。\n（先按 {DISMISS} 关闭窗口）",
      touch = "您正在控制一辆坦克，目前在海上的船中。\n使用屏幕左侧的摇杆驾驶：向前推加速，向后拉减速停止。\n\n摇杆推得越远，坦克速度越快。\n现在向前推摇杆使船向陆地前进。\n（先点击确定关闭此窗口）",
      controller = "您正在控制一辆坦克，目前在海上的船中。\n向前推左摇杆使坦克（和船）前进，\n向后拉使其减速并停止。\n\n左摇杆推得越远，坦克速度越快。\n现在向前推左摇杆，驾驶船只驶向陆地。\n（先按 {DISMISS} 关闭窗口）",
    },
    t01 = {
      keyboard = "前方有一条通向内陆的短河。按住 {ACCEL} 键将船驶向河道尽头。\n\n到达后继续按住 {ACCEL}。坦克将从船上离舰，\n船将停靠在河道尽头。",
      touch = "前方有一条通向内陆的短河。\n将摇杆向前推使船驶向河道尽头。\n\n到达后继续保持摇杆前推。\n坦克将从船上离舰，船将停靠在河道尽头。",
      controller = "前方有一条通向内陆的短河。将左摇杆向前推，\n驾驶船只驶向河道尽头。\n\n到达后继续保持左摇杆前推。\n坦克将从船上离舰，船将停靠在河道尽头。",
    },
    t02 = {
      keyboard = "您现在在草地上。坦克在草地上行驶相当快。\n\n继续按 {ACCEL} 向前驶向森林。",
      touch = "您现在在草地上。坦克在草地上行驶相当快。\n\n继续向前推摇杆，向前驶向森林。",
      controller = "您现在在草地上。坦克在草地上行驶相当快。\n\n继续向前推左摇杆，向前驶向森林。",
    },
    t03 = {
      keyboard = "您现在在森林中。坦克在森林中比在草地上移动得更慢。\n\n继续按 {ACCEL} 向前驶向沼泽。",
      touch = "您现在在森林中。坦克在森林中比在草地上移动得更慢。\n\n继续向前推摇杆，向前驶向沼泽。",
      controller = "您现在在森林中。坦克在森林中比在草地上移动得更慢。\n\n继续向前推左摇杆，向前驶向沼泽。",
    },
    t04 = {
      keyboard = "您现在在沼泽中。坦克在沼泽中移动非常缓慢。幸运的是前方有道路。\n\n继续按 {ACCEL} 向前驶向道路。",
      touch = "您现在在沼泽中。坦克在沼泽中移动非常缓慢。幸运的是前方有道路。\n\n继续向前推摇杆，向前驶向道路。",
      controller = "您现在在沼泽中。坦克在沼泽中移动非常缓慢。\n幸运的是前方有道路。\n\n继续向前推左摇杆，向前驶向道路。",
    },
    t05 = {
      keyboard = "您现在在道路上。坦克在道路上行驶非常快，\n按 {ACCEL} 可以加速。\n\n如果速度过快，可以按 {BRAKE} 减速",
      touch = "您现在在道路上。坦克在道路上行驶非常快，\n将摇杆完全向前推可以加速。\n\n如果速度过快，将摇杆向后拉减速。",
      controller = "您现在在道路上。坦克在道路上行驶非常快，\n将左摇杆完全向前推可以加速。\n\n如果速度过快，将左摇杆向后拉减速。",
    },
    t06 = {
      keyboard = "前方有一些建筑。建筑是您无法穿越的固体障碍，\n必须找到穿越迷宫的路。\n\n按 {LEFT} 使坦克向左转\n按 {RIGHT} 使坦克向右转",
      touch = "前方有一些建筑。建筑是您无法穿越的固体障碍，\n必须找到穿越迷宫的路。\n\n将摇杆向左或向右推，使坦克转向该方向。",
      controller = "前方有一些建筑。建筑是您无法穿越的固体障碍，\n必须找到穿越迷宫的路。\n\n将左摇杆向左或向右推，使坦克转向该方向。",
    },
    t07 = "这些带红色炮管探出的圆形物体是自动碉堡。\n它们会向进入射程的坦克开火。\n\n幸运的是，建筑可以阻挡炮弹，\n因此除非您等太久让碉堡把建筑打穿，\n否则碉堡无法击中您。",
    t08 = "森林同样可以保护您免受碉堡攻击，但方式不同。\n除了大幅降低坦克速度外，穿越森林还会限制视野。\n\n在森林内部，您仍能清楚看到森林外的一切，\n但除非您非常靠近，否则碉堡（及其他玩家）\n无法看到您的坦克。",
    t09 = "将坦克对准林带中央，向正北行驶。\n如果走直线且不偏向森林边缘，\n碉堡就看不到您，也不会开火。\n\n不要开出森林，否则会被炸成碎片。",
    t10 = {
      keyboard = "还有一种简单逃脱碉堡的方法——速度。\n\n确保与道路中央对齐，按住 {ACCEL} 全速冲过碉堡。",
      touch = "还有一种简单逃脱碉堡的方法——速度。\n\n确保与道路中央对齐，将摇杆完全向前推，\n全速冲过碉堡。",
      controller = "还有一种简单逃脱碉堡的方法——速度。\n\n确保与道路中央对齐，将左摇杆完全向前推，\n全速冲过碉堡。",
    },
    t11 = "当碉堡发现您时会开始射击，\n但只要保持直线前进不慌乱，\n炮弹击中前您就已脱离射程。\n\n不要冲出道路进入沼泽，\n否则会成为碉堡的靶子，无法快速逃脱。",
    t12 = "前方是一个补给基地。补给基地提供炮弹、地雷，\n并在坦克装甲受损时将其修复至满值。\n\n停在补给基地上，为坦克补充炮弹。",
    t13 = "您现在在补给基地上。坦克正在装载炮弹和地雷，\n右侧指示器会显示进度。前方建筑外是一片雷区。",
    t14 = {
      keyboard = "驾驶坦克进入雷区会被摧毁。\n幸运的是可以通过射击清除雷区。按 {FIRE} 射击。\n\n射穿建筑，然后在雷区中央落一发炮弹引爆地雷。\n炸毁地雷后会留下大弹坑，可以通过，\n但速度与穿越沼泽一样缓慢。",
      touch = "驾驶坦克进入雷区会被摧毁。\n幸运的是可以通过射击清除雷区。\n点击射击按钮进行射击。\n\n射穿建筑，然后在雷区中央落一发炮弹引爆地雷。\n炸毁地雷后会留下大弹坑，可以通过，\n但速度与穿越沼泽一样缓慢。",
      controller = "驾驶坦克进入雷区会被摧毁。\n幸运的是可以通过射击清除雷区。按 {FIRE} 射击。\n\n射穿建筑，然后在雷区中央落一发炮弹引爆地雷。\n炸毁地雷后会留下大弹坑，可以通过，\n但速度与穿越沼泽一样缓慢。",
    },
    t15 = "前方是另一片雷区。重复同样的技巧：\n开到补给基地，射穿墙，在雷区中央落一发炮弹引爆。\n\n但这次雷区紧邻一条大河，水会涌入弹坑。\n您可以像通过弹坑和沼泽一样驾车穿越水域。",
    t16 = {
      keyboard = "WinBolo 会自动滚动窗口显示坦克周围区域，\n但您也可以使用 {SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\n键手动覆盖视角。\n\n按 {SCROLL_UP} 获得前方河流的更好视角。",
      touch = "WinBolo 会自动滚动窗口显示坦克周围区域，\n但您可以用手指拖动地图手动覆盖视角。\n\n向下拖动地图，将前方的河流带入视野。",
      controller = "WinBolo 会自动滚动窗口显示坦克周围区域，\n但您可以推动右摇杆手动覆盖视角。\n\n现在向上推右摇杆，获得前方河流的更好视角。",
    },
    t17 = "穿越水域不仅速度极慢，\n还有另一个缺点：会消耗坦克的炮弹和地雷储备。\n\n到达对岸时坦克仍可运行，\n但会失去所有进攻能力。",
    t18 = {
      keyboard = "这次我们用更巧妙的方式过河。\n不是直接驾车穿越水域，而是建造一座桥。\n\n点击窗口左上角的森林图标选择伐木模式，\n然后点击森林。建造者会从坦克中跑出采集一棵树作为建材。",
      touch = "这次我们用更巧妙的方式过河。\n不是直接驾车穿越水域，而是建造一座桥。\n\n点击窗口左上角的森林图标选择伐木模式，\n然后点击森林。\n建造者会从坦克中跑出采集一棵树作为建材。",
      controller = "这次我们用更巧妙的方式过河。\n不是直接驾车穿越水域，而是建造一座桥。\n\n拉住 {BUILD_MODE} 从滚动模式切换到建造模式\n——右摇杆现在移动建造光标而非视角。\n按 {BUILD_TOOL} 选择伐木模式（窗口左上角），\n将光标对准一棵树，然后按 {BUILD_PLACE}。\n建造者会跑出来采集一棵树作为建材。",
    },
    t19 = {
      keyboard = "采集一些树木后，选择道路/桥梁工具\n（从顶部数第二个按钮），在道路末端的河流上点击鼠标。\n\n建造者会跑出来建造一段桥梁。\n再次点击完成桥梁，然后驾车驶向对岸。",
      touch = "采集一些树木后，选择道路/桥梁工具\n（从顶部数第二个按钮），在道路末端的河流上点击。\n\n建造者会跑出来建造一段桥梁。\n再次点击完成桥梁，然后驾车驶向对岸。",
      controller = "采集一些树木后，按 {BUILD_TOOL} 选择道路/桥梁工具\n（从顶部数第二个按钮），将建造光标对准道路末端的\n河流，然后按 {BUILD_PLACE}。\n\n建造者会建造一段桥梁。再次放置完成桥梁，\n然后拉住 {BUILD_MODE} 退出建造模式，驾车过桥。",
    },
    t20 = "这条河比上一条更宽，中央水太深无法建桥。\n如果强行驾驶，坦克会沉入河底被摧毁。",
    t21 = {
      keyboard = "要渡过这条河，需要建造一艘船。\n船需要五棵树，先点击五棵树进行采集。\n然后选择墙/船工具（从顶部数第三个按钮），\n在水面上点击建造一艘船。\n\n如果点击陆地会建造墙壁，\n需要再采集一些树才能建船。\n驾车上船，渡过对岸。",
      touch = "要渡过这条河，需要建造一艘船。\n船需要五棵树，先点击五棵树进行采集。\n然后选择墙/船工具（从顶部数第三个按钮），\n在水面上点击建造一艘船。\n\n如果点击陆地会建造墙壁，\n需要再采集一些树才能建船。\n驾车上船，渡过对岸。",
      controller = "要渡过这条河，需要建造一艘船。船需要五棵树，\n瞄准五棵树，每棵按 {BUILD_PLACE} 采集。然后按\n{BUILD_TOOL} 选择墙/船工具（从顶部数第三个按钮），\n在水面上放置。\n\n如果放置在陆地会建造墙壁，需要再采集树木。\n驾车上船，渡过对岸。",
    },
    t22 = "前方有一个碉堡，旁边有几个补给基地提供弹药。\n从补给基地补充炮弹并攻击碉堡。\n\n碉堡会还击，但如果坦克被击中，\n可以轻松回到补给基地修复装甲至满值。",
    t23 = "只要有耐心和谨慎，应该能摧毁这个碉堡。\n运用已学技巧让这变得更容易：\n\n躲在森林中让碉堡看不到您，\n采集树木，修建道路加快坦克速度，\n建造建筑遮挡碉堡炮弹。",
    t24 = "击败这个碉堡后，教程的最后挑战是\n消灭守卫出口的两个碉堡，建造一艘船，并成功逃脱。\n\n特别注意坦克的装甲等级——\n如果坦克被摧毁，您将在附近获得一辆新坦克，\n可以再次尝试。",
    t25 = "恭喜！您已完成 WinBolo 教程。\n\n您已经可以开始游戏了。加入线上对战，\n为朋友主持一局，或与机器人练习。",
    respawn1 = "您的坦克已被摧毁——但无需从头开始教程。\n已在您登陆处附近为您提供了一辆新坦克。\n返回战斗，完成逃脱任务。",
  },
}

TEXT["zh-tw"] = {
  pop = {
    start01 = {
      keyboard = "歡迎來到 WinBolo 教學。這座小島介紹\nWinBolo 的基本原則，並逐一引導您\n了解它們。每個新原則將在\n像這樣的視窗中說明。\n\n閱讀每條訊息後，\n按 {DISMISS} 繼續。",
      touch = "歡迎來到 WinBolo 教學。這座小島介紹\nWinBolo 的基本原則，並逐一引導您\n了解它們。每個新原則將在\n像這樣的視窗中說明。\n\n閱讀每條訊息後，您可以透過\n點擊「確定」按鈕來繼續。",
    },
    start02 = "WinBolo 是 Stuart Cheshire 經典 Macintosh 網路遊戲 Bolo 的唯一授權重製版。\n您可以在官方網站找到策略提示和其他資訊：\n\nhttps://www.winbolo.com\n\n歡迎加入我們的 Reddit 和 Discord\n\nhttps://www.reddit.com/r/winbolo\n\nhttps://discord.gg/znGR3VMaqd",
    start04 = {
      keyboard = "您正在控制一輛坦克，目前在\n海上的船隻上。按 {ACCEL} 使坦克（和船隻）\n向前行駛，按 {BRAKE} 減速並停止。\n\n按住按鍵直到坦克達到您想要的速度\n再放開。現在按 {ACCEL} 讓\n船隻向前駛向陸地。\n（先按 {DISMISS} 關閉視窗）",
      touch = "您正在控制一輛坦克，目前在\n海上的船隻上。使用畫面左側的搖桿\n來駕駛：向前推動加速，\n向後拉動減速並停止。\n\n搖桿推動越遠，坦克速度越快。\n現在向前推動搖桿，讓\n船隻向前駛向陸地。\n（先點擊確定關閉此視窗）",
      controller = "您正在控制一輛坦克，目前在\n海上的船隻上。向前推左搖桿使坦克（和船隻）\n向前行駛，向後拉減速並停止。\n\n左搖桿推動越遠，坦克速度越快。\n現在向前推左搖桿，讓\n船隻向前駛向陸地。\n（先按 {DISMISS} 關閉視窗）",
    },
    t01 = {
      keyboard = "前方有一條通往內陸的短河。按住 {ACCEL} 鍵\n將您的船駛到河的盡頭。\n\n到達後，繼續按住 {ACCEL}。坦克將從船上\n下船，船將停泊在河的盡頭。",
      touch = "前方有一條通往內陸的短河。\n向前推動搖桿將您的船駛到\n河的盡頭。\n\n到達後，繼續向前推動搖桿。\n坦克將從船上下船，\n船將停泊在河的盡頭。",
      controller = "前方有一條通往內陸的短河。\n向前推左搖桿，將船駛向河的盡頭。\n\n到達後繼續保持左搖桿前推。\n坦克將從船上下船，船將停泊在河的盡頭。",
    },
    t02 = {
      keyboard = "您現在在草地上。坦克在草地上移動\n速度相當快。\n\n繼續按 {ACCEL} 前進到森林。",
      touch = "您現在在草地上。坦克在草地上\n移動速度相當快。\n\n繼續向前推動搖桿前進到森林。",
      controller = "您現在在草地上。坦克在草地上移動\n速度相當快。\n\n繼續向前推左搖桿，前進到森林。",
    },
    t03 = {
      keyboard = "您現在在森林中。坦克在森林中的移動速度\n比在草地上慢。\n\n繼續按 {ACCEL} 前進到沼澤。",
      touch = "您現在在森林中。坦克在森林中的移動速度\n比在草地上慢。\n\n繼續向前推動搖桿前進到沼澤。",
      controller = "您現在在森林中。坦克在森林中的移動速度\n比在草地上慢。\n\n繼續向前推左搖桿，前進到沼澤。",
    },
    t04 = {
      keyboard = "您現在在沼澤中。坦克在沼澤中移動\n非常緩慢。幸好前方有一條道路。\n\n繼續按 {ACCEL} 前進到道路。",
      touch = "您現在在沼澤中。坦克在沼澤中移動\n非常緩慢。幸好前方有一條道路。\n\n繼續向前推動搖桿前進到道路。",
      controller = "您現在在沼澤中。坦克在沼澤中移動\n非常緩慢。幸好前方有一條道路。\n\n繼續向前推左搖桿，前進到道路。",
    },
    t05 = {
      keyboard = "您現在在道路上。坦克在道路上可以移動\n非常快，按 {ACCEL} 可以加速。\n\n如果速度過快，可以按 {BRAKE} 減速。",
      touch = "您現在在道路上。坦克在道路上可以移動\n非常快，將搖桿完全向前推動\n可以加速。\n\n如果速度過快，向後拉動\n搖桿以減速。",
      controller = "您現在在道路上。坦克在道路上可以移動\n非常快，將左搖桿完全向前推可以加速。\n\n如果速度過快，向後拉左搖桿以減速。",
    },
    t06 = {
      keyboard = "前方有一些建築。建築是不可穿越的\n固體障礙物，您必須找到穿越迷宮的\n路線到達另一側。\n\n按 {LEFT} 使坦克左轉\n按 {RIGHT} 使坦克右轉",
      touch = "前方有一些建築。建築是不可穿越的\n固體障礙物，您必須找到穿越迷宮的\n路線到達另一側。\n\n向左或向右推動搖桿\n使坦克轉向該方向。",
      controller = "前方有一些建築。建築是不可穿越的\n固體障礙物，您必須找到穿越迷宮的\n路線到達另一側。\n\n向左或向右推左搖桿，使坦克轉向該方向。",
    },
    t07 = "這些圓形物體紅色砲管突出的是\n自動碉堡。它們會射擊任何進入射程內的坦克。\n\n幸好建築物可以提供保護，\n碉堡將無法擊中您，\n除非您等待太久使它們成功\n完全射穿建築物。",
    t08 = "森林也提供碉堡防護，但方式不同。\n除了大幅減慢坦克速度外，\n穿越森林也會限制視野。\n\n當您在森林內時，仍然可以清楚看到\n森林外的一切，但碉堡（和其他玩家）\n無法看到您的坦克，除非非常靠近。",
    t09 = "將坦克對準林帶中央，\n向正北方行駛。若您筆直前進，\n不要太靠近森林邊緣，\n碉堡將看不見您，也不會射擊。\n\n不要駛出森林，否則您會被炸得粉碎。",
    t10 = {
      keyboard = "還有一種簡單的逃脫碉堡方式 —\n速度。\n\n確保您正確對準道路中央，\n按住 {ACCEL} 以全速通過碉堡。",
      touch = "還有一種簡單的逃脫碉堡方式 —\n速度。\n\n確保您正確對準道路中央，\n將搖桿完全向前推動以\n全速通過碉堡。",
      controller = "還有一種簡單的逃脫碉堡方式 —\n速度。\n\n確保您正確對準道路中央，\n將左搖桿完全向前推以全速通過碉堡。",
    },
    t11 = "當它們發現您時會開始射擊，\n但如果您繼續直行且不失去勇氣，\n砲彈打中您之前您就已離開射程。\n\n不要跑出道路進入沼澤，否則您將成為\n碉堡的固定靶，無法快速移動以逃脫。",
    t12 = "前方有一個補給基地。補給基地提供\n砲彈供您射擊，地雷供您佈置秘密陷阱，\n並在坦克裝甲受損時將其修復至滿格。\n\n停在補給基地上為您的坦克補充砲彈。",
    t13 = "您在補給基地上。您的坦克現在將\n補充砲彈和地雷，如右側指示器所示。\n前方，建築物後方，是一片地雷區。",
    t14 = {
      keyboard = "如果將坦克開進地雷區，坦克將被\n摧毀。幸好您可以透過射擊來清除地雷區。\n按 {FIRE} 射擊。\n\n射穿建築物，然後在地雷區中央\n落下一枚砲彈使其引爆。\n炸掉地雷後會在地面留下大彈坑，\n您可以駛過彈坑，雖然速度和穿越沼澤一樣慢。",
      touch = "如果將坦克開進地雷區，坦克將被\n摧毀。幸好您可以透過射擊來清除地雷區。\n點擊射擊按鈕進行射擊。\n\n射穿建築物，然後在地雷區中央\n落下一枚砲彈使其引爆。\n炸掉地雷後會在地面留下大彈坑，\n您可以駛過彈坑，雖然速度和穿越沼澤一樣慢。",
      controller = "如果將坦克開進地雷區，坦克將被\n摧毀。幸好您可以透過射擊來清除地雷區。\n按 {FIRE} 射擊。\n\n射穿建築物，然後在地雷區中央\n落下一枚砲彈使其引爆。\n炸掉地雷後會在地面留下大彈坑，\n您可以駛過彈坑，雖然速度和穿越沼澤一樣慢。",
    },
    t15 = "前方還有一個地雷區。再用同樣的方法：\n駛上補給基地，射穿牆壁，\n在地雷區中央落下一枚砲彈使其引爆。\n\n不過這次地雷區緊鄰一條大河，\n水會淹入彈坑。您可以像穿越\n彈坑和沼澤一樣駕駛坦克穿越水中。",
    t16 = {
      keyboard = "WinBolo 會自動捲動視窗以顯示\n您坦克周圍的區域，但您可以使用\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\n鍵手動覆蓋 WinBolo 選擇的視角。\n\n現在按 {SCROLL_UP} 以更好地查看\n前方的河流。",
      touch = "WinBolo 會自動捲動視窗以顯示\n您坦克周圍的區域，但您可以用手指\n拖動地圖來手動覆蓋視角。\n\n現在向下拖動地圖，將前方的河流\n帶入視野。",
      controller = "WinBolo 會自動捲動視窗以顯示\n您坦克周圍的區域，但您可以推動右搖桿\n手動覆蓋視角。\n\n現在向上推右搖桿，以更好地查看前方的河流。",
    },
    t17 = "除了速度非常慢之外，穿越水中還有\n另一個缺點：會耗盡坦克的砲彈和地雷存量。\n\n到達另一側後，坦克仍可正常運作，\n但您將失去所有攻擊能力。",
    t18 = {
      keyboard = "這次我們要用更巧妙的方式\n渡河。不是直接穿越水中，\n而是建造一座橋。\n\n點擊左上角的森林圖示選擇樹木採伐模式，\n然後點擊森林。您的建造者將從坦克\n跑出來採集一棵樹作為建材。",
      touch = "這次我們要用更巧妙的方式\n渡河。不是直接穿越水中，\n而是建造一座橋。\n\n點擊左上角的森林圖示選擇樹木採伐模式，\n然後點擊森林。您的建造者將從坦克\n跑出來採集一棵樹作為建材。",
      controller = "這次我們要用更巧妙的方式渡河。\n不是直接穿越水中，而是建造一座橋。\n\n拉住 {BUILD_MODE} 從捲動模式切換至建造模式\n——右搖桿現在移動建造游標而非視角。\n按 {BUILD_TOOL} 選擇伐木模式（視窗左上角），\n將游標對準一棵樹，然後按 {BUILD_PLACE}。\n建造者會從坦克跑出來採集一棵樹作為建材。",
    },
    t19 = {
      keyboard = "採集一些樹木後，選擇道路/橋梁工具\n（從頂部算第二個按鈕），然後點擊\n道路末端的河流。\n\n您的建造者將從坦克跑出來建造\n一段橋梁。再點擊一次完成橋梁，\n然後駛過橋到另一側。",
      touch = "採集一些樹木後，選擇道路/橋梁工具\n（從頂部算第二個按鈕），然後點擊\n道路末端的河流。\n\n您的建造者將從坦克跑出來建造\n一段橋梁。再點擊一次完成橋梁，\n然後駛過橋到另一側。",
      controller = "採集一些樹木後，按 {BUILD_TOOL} 選擇道路/橋梁工具\n（從頂部算第二個按鈕），將建造游標對準\n道路末端的河流，然後按 {BUILD_PLACE}。\n\n建造者會建造一段橋梁。再次放置完成橋梁，\n然後拉住 {BUILD_MODE} 退出建造模式，駛過橋到另一側。",
    },
    t20 = "這條河比上一條更寬，在中央\n太深，無法建造橋梁。如果您嘗試\n直接駛過，坦克會沉入河底被摧毀。",
    t21 = {
      keyboard = "要渡過這條河，您需要建造一艘船。船\n需要五棵樹，所以點擊五棵樹來採集它們。\n然後選擇牆壁/船隻工具（從頂部算第三個按鈕），\n點擊水面來建造船隻。\n\n如果點擊陸地，您將建造牆壁，\n需要再採集樹木才能建造船隻。\n駛上船隻並渡河到另一側。",
      touch = "要渡過這條河，您需要建造一艘船。船\n需要五棵樹，所以點擊五棵樹來採集它們。\n然後選擇牆壁/船隻工具（從頂部算第三個按鈕），\n點擊水面來建造船隻。\n\n如果點擊陸地，您將建造牆壁，\n需要再採集樹木才能建造船隻。\n駛上船隻並渡河到另一側。",
      controller = "要渡過這條河，您需要建造一艘船。船\n需要五棵樹，瞄準五棵樹，每棵按 {BUILD_PLACE} 採集。\n然後按 {BUILD_TOOL} 選擇牆壁/船隻工具\n（從頂部算第三個按鈕），放置在水面上。\n\n如果放置在陸地會建造牆壁，需要再採集樹木。\n駛上船隻並渡河到另一側。",
    },
    t22 = "前方有一個碉堡，還有幾個補給基地\n為您提供彈藥。從補給基地補充砲彈，\n然後攻擊碉堡。\n\n它會反擊，但如果您的坦克被擊中，\n您可以輕鬆返回補給基地修復裝甲至滿格。",
    t23 = "憑藉耐心和謹慎，您應該能夠摧毀這個\n碉堡。使用您學到的技能讓任務更容易：\n\n躲在森林中使碉堡看不見您，\n採集樹木，建造道路讓坦克移動更快，\n建造建築物作為碉堡砲彈的掩護。",
    t24 = "成功摧毀這個碉堡後，您的最終\n教學挑戰是消滅守衛出口的兩個\n碉堡，建造船隻並逃脫。\n\n特別注意您坦克的裝甲等級 —\n如果坦克被摧毀，您將在附近獲得一輛新坦克，\n讓您可以再次嘗試。",
    t25 = "恭喜！您已完成 WinBolo\n教學。\n\n您已經準備好開始遊戲了。加入線上對戰，\n為朋友主持一場遊戲，或與電腦對手練習。",
    respawn1 = "您的坦克已被摧毀——但不需要從頭開始教學。\n已在您登陸處附近為您提供了一輛新坦克。\n返回戰鬥，完成逃脫任務。",
  },
}
