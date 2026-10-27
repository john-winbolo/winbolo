-- =========================================================================
-- Tutorial — a scripted single-player round on Tutorial.map.
--
-- Seven stations up one road, south to north. Each station shows a short
-- popup when the player first drives into it, a checklist on the scenario
-- panel, one map marker on the goal it is on, and a white status line that
-- says what to do next:
--
--   1  Driving      terrain patches beside the road and their speeds;
--                   done after three of the five
--   2  Bases        2A your base (stop and refill), 2B a neutral base (take
--                   it), 2C an enemy base (shoot it empty, take it); then
--                   the game types
--   3  Building     harvest, road, wall, mine, shoot down a wall
--   4  Pillboxes    4A a dead neutral pill to pick up and build,
--                   4B a damaged pill inside a ring of forest: hide in it,
--                   4C kill it and pick it up
--   5  Taking a pill  5A a friendly bot takes a pill on a loop while you
--                   watch, 5B the same take for you, with a walled RESET pen
--   6  The man      a parked bot's man walks to one tree and back; shoot
--                   him as he crosses the craters
--   7  Final round  a small walled Chew Toy island against one Easy bot
--
-- CHECKPOINTS. The furthest station the player has driven into is the
-- checkpoint: on_choose_start puts every respawn there, and spawn_loadout
-- gives the tank that station's loadout. Each checkpoint is the centre of
-- a walled 3x3 deep-water pool: the tank respawns on its boat facing north
-- and drives out on a road from the pool's north edge that merges into the
-- main road. Station 1's pool is at the main road's south end; Stations 2
-- to 6 have theirs just west of the main road.
--
-- RE-ARMING. Nothing has to be reset by hand: once a second the station
-- the player is in puts back whatever a goal still needs while that goal
-- is not ticked (a pill, a base, a tree, a wall). Stations 5 and 7 also
-- have a walled RESET pen that puts the whole station back.
--
-- BOTS. Three held seats: one on team 1 for the Station 5A demo bot, which
-- is the player's ally, and two on team 2 for the Station 6 bot and the
-- Station 7 bot. The seats are picked out as the round starts, so they can
-- own things (the 2C base, the 5B pill) before they are fielded; each bot
-- is fielded the first time the player drives into its station. Where the
-- lobby held no seats (a host trimmed them), a free seat is used instead.
--
-- THE PLAYER is the first human seat. A test may name another seat in the
-- global TUTORIAL_PLAYER before the round starts (tests/scenario/tutorial_*).
--
-- The map, and the LAYOUT block below, are written by
-- tools/make_tutorial_map.py. Change the map there, not here.
--
-- The API it is written against: docs/SCENARIO_API.md.
-- =========================================================================

-- BEGIN LAYOUT (written by tools/make_tutorial_map.py; do not edit)
local LAYOUT = {
  pill = {
    p4a = { n = 1, x = 122, y = 149, armour = 0 },
    p4 = { n = 2, x = 138, y = 141, armour = 15 },
    t5a_target = { n = 3, x = 137, y = 105, armour = 15 },
    t5a_blocker = { n = 4, x = 137, y = 106, armour = 0 },
    t5b_target = { n = 5, x = 117, y = 101, armour = 15 },
    t5b_blocker = { n = 6, x = 117, y = 102, armour = 0 },
    p7_nw1 = { n = 7, x = 122, y = 39, armour = 15 },
    p7_nw2 = { n = 8, x = 120, y = 41, armour = 15 },
    p7_se1 = { n = 9, x = 134, y = 55, armour = 15 },
    p7_se2 = { n = 10, x = 136, y = 53, armour = 15 },
    p7_sw1 = { n = 11, x = 127, y = 70, armour = 0 },
    p7_sw2 = { n = 12, x = 127, y = 68, armour = 0 },
  },
  base = {
    b2a = { n = 1, x = 123, y = 194 },
    b2b = { n = 2, x = 131, y = 189 },
    b2c = { n = 3, x = 123, y = 185 },
    b3 = { n = 4, x = 127, y = 163 },
    b4c = { n = 5, x = 127, y = 132 },
    b5 = { n = 6, x = 127, y = 110 },
    b6 = { n = 7, x = 127, y = 84 },
    b7_ne1 = { n = 8, x = 132, y = 35 },
    b7_ne2 = { n = 9, x = 140, y = 43 },
    b7_nw1 = { n = 10, x = 124, y = 35 },
    b7_nw2 = { n = 11, x = 116, y = 43 },
    b7_se1 = { n = 12, x = 132, y = 59 },
    b7_se2 = { n = 13, x = 140, y = 51 },
    b7_sw1 = { n = 14, x = 124, y = 51 },
    b7_sw2 = { n = 15, x = 118, y = 51 },
  },
  start = {
    cp1 = { n = 1, x = 127, y = 226 },
    cp2 = { n = 2, x = 123, y = 199 },
    cp3 = { n = 3, x = 123, y = 176 },
    cp4 = { n = 4, x = 123, y = 155 },
    bot5a = { n = 5, x = 140, y = 117 },
    cp5 = { n = 6, x = 123, y = 124 },
    bot6 = { n = 7, x = 134, y = 80 },
    cp6 = { n = 8, x = 123, y = 89 },
    bot7 = { n = 9, x = 142, y = 32 },
    cp7 = { n = 10, x = 112, y = 51 },
  },
  point = {
    bot6_park = { 131, 80 },
    p4 = { 138, 141 },
    s7_arrive = { 127, 63 },
    t5a_blocker = { 137, 106 },
    t5a_home = { 136, 115 },
    t5a_target = { 137, 105 },
    t5b_blocker = { 117, 102 },
    t5b_park = { 121, 107 },
    t5b_target = { 117, 101 },
    tree6 = { 115, 80 },
    wall3 = { 133, 168 },
    watch5a = { 131, 111 },
  },
  region = {
    b2a_area = { x = 121, y = 193, w = 5, h = 3 },
    b2b_area = { x = 129, y = 188, w = 5, h = 3 },
    b2c_area = { x = 121, y = 184, w = 5, h = 3 },
    craters6 = { x = 121, y = 80, w = 4, h = 1 },
    grove3 = { x = 118, y = 164, w = 7, h = 9 },
    island5a = { x = 135, y = 104, w = 6, h = 14 },
    island7 = { x = 107, y = 27, w = 43, h = 41 },
    p4_clearing = { x = 136, y = 139, w = 5, h = 5 },
    p4a_area = { x = 119, y = 146, w = 7, h = 7 },
    path6 = { x = 116, y = 80, w = 16, h = 1 },
    quarter_ne = { x = 129, y = 27, w = 21, h = 20 },
    reset5b = { x = 114, y = 119, w = 3, h = 2 },
    reset7 = { x = 116, y = 62, w = 3, h = 2 },
    s1 = { x = 97, y = 205, w = 59, h = 23 },
    s2 = { x = 97, y = 182, w = 59, h = 20 },
    s3 = { x = 97, y = 161, w = 59, h = 18 },
    s4 = { x = 97, y = 130, w = 59, h = 28 },
    s5 = { x = 97, y = 95, w = 59, h = 32 },
    s5b = { x = 97, y = 95, w = 26, h = 28 },
    s6 = { x = 97, y = 72, w = 59, h = 20 },
    s7 = { x = 97, y = 25, w = 59, h = 47 },
    t5b_park = { x = 121, y = 107, w = 1, h = 1 },
    watch5a = { x = 129, y = 111, w = 3, h = 1 },
  },
  walls5a = { {132,101}, {133,101}, {134,101}, {135,101}, {136,101}, {137,101}, {138,101}, {139,101}, {140,101}, {141,101}, {142,101}, {143,101}, {132,102}, {143,102}, {132,103}, {134,103}, {135,103}, {136,103}, {137,103}, {138,103}, {139,103}, {140,103}, {141,103}, {143,103}, {132,104}, {134,104}, {141,104}, {143,104}, {132,105}, {134,105}, {141,105}, {143,105}, {132,106}, {134,106}, {141,106}, {143,106}, {132,107}, {134,107}, {141,107}, {143,107}, {132,108}, {134,108}, {141,108}, {143,108}, {132,109}, {134,109}, {141,109}, {143,109}, {132,110}, {134,110}, {141,110}, {143,110}, {132,111}, {134,111}, {141,111}, {143,111}, {132,112}, {134,112}, {141,112}, {143,112}, {132,113}, {134,113}, {141,113}, {143,113}, {132,114}, {134,114}, {141,114}, {143,114}, {132,115}, {134,115}, {141,115}, {143,115}, {132,116}, {134,116}, {141,116}, {143,116}, {132,117}, {134,117}, {141,117}, {143,117}, {132,118}, {134,118}, {135,118}, {136,118}, {137,118}, {138,118}, {139,118}, {140,118}, {141,118}, {143,118}, {132,119}, {143,119}, {132,120}, {133,120}, {134,120}, {135,120}, {136,120}, {137,120}, {138,120}, {139,120}, {140,120}, {141,120}, {142,120}, {143,120} },
  moat5a = { {133,102}, {134,102}, {135,102}, {136,102}, {137,102}, {138,102}, {139,102}, {140,102}, {141,102}, {142,102}, {133,103}, {142,103}, {133,104}, {142,104}, {133,105}, {142,105}, {133,106}, {142,106}, {133,107}, {142,107}, {133,108}, {142,108}, {133,109}, {142,109}, {133,110}, {142,110}, {133,111}, {142,111}, {133,112}, {142,112}, {133,113}, {142,113}, {133,114}, {142,114}, {133,115}, {142,115}, {133,116}, {142,116}, {133,117}, {142,117}, {133,118}, {142,118}, {133,119}, {134,119}, {135,119}, {136,119}, {137,119}, {138,119}, {139,119}, {140,119}, {141,119}, {142,119} },
  walls5b = { {106,112}, {107,112}, {106,113}, {108,113}, {106,114}, {107,114}, {106,115}, {108,115}, {106,116}, {108,116}, {110,112}, {111,112}, {112,112}, {110,113}, {110,114}, {111,114}, {110,115}, {110,116}, {111,116}, {112,116}, {115,112}, {116,112}, {114,113}, {115,114}, {116,115}, {114,116}, {115,116}, {118,112}, {119,112}, {120,112}, {118,113}, {118,114}, {119,114}, {118,115}, {118,116}, {119,116}, {120,116}, {122,112}, {123,112}, {124,112}, {123,113}, {123,114}, {123,115}, {123,116}, {113,118}, {114,118}, {115,118}, {116,118}, {117,118}, {113,119}, {113,120}, {113,121}, {114,121}, {115,121}, {116,121}, {117,121} },
  walls7 = { {108,55}, {109,55}, {108,56}, {110,56}, {108,57}, {109,57}, {108,58}, {110,58}, {108,59}, {110,59}, {112,55}, {113,55}, {114,55}, {112,56}, {112,57}, {113,57}, {112,58}, {112,59}, {113,59}, {114,59}, {117,55}, {118,55}, {116,56}, {117,57}, {118,58}, {116,59}, {117,59}, {120,55}, {121,55}, {122,55}, {120,56}, {120,57}, {121,57}, {120,58}, {120,59}, {121,59}, {122,59}, {124,55}, {125,55}, {126,55}, {125,56}, {125,57}, {125,58}, {125,59}, {115,61}, {116,61}, {117,61}, {118,61}, {119,61}, {115,62}, {115,63}, {115,64}, {116,64}, {117,64}, {118,64}, {119,64} },
  arrow_reset5b = { {117,123}, {118,122}, {118,124}, {119,123}, {120,123} },
  arrow_reset7 = { {121,66}, {122,65}, {122,67}, {123,66}, {124,66} },
  grove3 = { 118, 164, 124, 172 },
}
-- END LAYOUT

scenario = {
  name = "Tutorial",
  description = "Learn WinBolo one step at a time: driving, bases, "
    .. "building, pillboxes, taking a pillbox and the builder, then a "
    .. "small game against an Easy bot.",
  api  = 1,
  kind = "scenario",
  author  = "WinBolo",
  updated = "2026-10-27T09:21Z",
  game = "open",
  -- The demo bots and the final-round bot are held seats this file fields
  -- with game.spawn_bot, so the lobby has to allow bots.
  needs_bots = true,

  lobby = {
    max_players = 1,
    teams = {
      -- The Station 5A demo bot: the player's ally.
      { id = 1, bots = 1, max_bots = 1, fielded = false,
        brain = "GoalHunter", mode = "default", difficulty = "hard" },
      -- The Station 6 bot and the Station 7 bot.
      { id = 2, bots = 2, max_bots = 2, fielded = false,
        brain = "GoalHunter", mode = "default", difficulty = "hard" },
    },
  },

  regions = LAYOUT.region,

  callbacks = {
    on_start = "Hands out the bases and pillboxes and picks the bots' seats.",
    on_player_join = "Sets the tutorial up for a player who joins after the round has started.",
    on_player_leave = "Forgets a player who leaves, so the next one to join carries on from the same station.",
    on_tick = "Ticks off goals, draws the panel, markers and status line, puts back what a goal needs, and runs the bots.",
    on_enter_region = "Shows each station's popup, fields its bot, moves your checkpoint, and runs the RESET pens.",
    on_tank_spawned = "Parks the Station 6 bot, arms each bot, and hands you your 5B pillbox.",
    on_tank_hit = "Notices when the Station 4 pillbox shoots you while you hide.",
    on_built = "Ticks off harvesting, roads and walls in Station 3.",
    on_mine_laid = "Ticks off laying a mine in Station 3.",
    on_lgm_died = "Counts the men you kill in Station 6 and lands each new man near his tank.",
    on_pill_picked_up = "Ticks off pillbox pick-ups and starts the Station 5 takes again.",
    on_pill_placed = "Ticks off building a pillbox, moves a Station 5 blocker to its square, and sends a stray pillbox home.",
    on_pill_killed = "Rebuilds your Station 5B blocking pillbox 2 seconds after it dies.",
    on_base_captured = "Ticks off taking bases, and finishes the tutorial when you own every base in Station 7.",
    can_build = "Keeps each bot's builder inside its own station, and stops boats in Station 5.",
    can_capture = "The 5A bot takes only its own two pillboxes; the Station 7 bot leaves your two dead ones.",
    can_hit = "Tank shells pass between you and the demo bots; yours pass your 5B pillbox; the 5A pillboxes' pass you.",
    can_target = "The two Station 5A pillboxes never aim or fire at you; they shoot only at the demo bot.",
    can_die = "The parked Station 6 tank cannot be destroyed.",
    announce = "No newswire line when a base or a pillbox changes hands.",
    on_choose_start = "You respawn at the furthest station you have reached.",
    spawn_loadout = "You respawn with the tank that station starts you with.",
    allow_base_win = "Owning every base does not end the round; Station 7 has its own win.",
  },
}

-- ---------------------------------------------------------------------
-- Round state. The host builds a fresh Lua state for every round, so none
-- of this needs a reset between rounds.
local S = {
  player   = nil,     -- the tutorial's player seat; nil again once he leaves
  adopted  = false,   -- a player has been set up once: the round is handed out
  -- The pillboxes and bases the player owned when last noted (note_owned), by
  -- number. The server gives a leaving player's away before on_player_leave
  -- runs, so this is what the next player gets back.
  owned    = { pills = {}, bases = {} },
  reached  = 1,       -- the furthest station reached: the checkpoint
  at       = 1,       -- the station the player last drove into
  entered  = {},      -- station -> true once driven into
  done     = { {}, {}, {}, {}, {}, {}, {} },  -- checklist, by station
  -- Goals passed over, by station: not ticked and still there to do, but
  -- the station no longer waits for them (Station 4's hide, when the
  -- pillbox is killed first).
  passed   = { {}, {}, {}, {}, {}, {}, {} },
  popped   = {},      -- popup id -> true once shown
  queue    = {},      -- popups waiting for their delay: { id, due }
  says     = {},      -- announcements waiting for a popup: { key, secs, args }
  carrier  = {},      -- pill n -> seat carrying it
  -- The bots' seats, picked as the round starts; nil where none was free.
  bot5a = nil, bot6 = nil, bot7 = nil,
  free_seat = {},     -- seat -> true where a role took an empty seat
  fielded  = {},      -- role -> true once spawn_bot was asked
  frames   = 0,       -- on_tick calls (50 a second)
  sec      = 0,       -- whole seconds since the start
  last_pos = nil,     -- world position at the last poll
  moving   = false,
  on_2a    = false,   -- Station 2: the tank has been on the 2A base
  near5a_at = nil,    -- Station 5A: frame the tank first came near watch5a
  hide     = 0,       -- Station 4: seconds hidden in the forest in range
  men      = 0,       -- Station 6: men killed
  gun_remind = false, -- Station 6: the gun-range reminder is up
  t5a_reset_at = 0,   -- Station 5A: the second of the last reset
  t5b_restore = false,  -- Station 5B: a restore of the target is waiting
  b5b_dead_at = nil,  -- Station 5B: the second the player's blocker died
  out5     = 0,       -- polls in a row the player's tank was outside Station 5
  rings5a_again = false,  -- Station 5A: a ring rebuild is not finished
  sw       = {},      -- Station 7: dead pillboxes picked up
  won      = false,
  panel_key = nil,
  status   = nil,     -- the status line last sent
  marker_key = nil,
  markers  = 0,       -- how many marker ids are in use
}

-- ---------------------------------------------------------------------
-- Small helpers.
local function rule(name) return game.rule(name) end

local P = LAYOUT.pill
local B = LAYOUT.base
local ST = LAYOUT.start
local PT = LAYOUT.point

-- Shells to take a base from `armour` down to where it can be taken.
local function base_shots(armour)
  local over = armour - rule("base_capture_armour")
  if over <= 0 then return 0 end
  return math.ceil(over / rule("shell_damage"))
end

local function full_stock()
  return { shells = rule("tank_full_shells"), mines = rule("tank_full_mines"),
           armour = rule("tank_full_armour"), trees = rule("tank_full_trees") }
end

-- ---------------------------------------------------------------------
-- Texts.
--
-- Every word the player reads, in TEXT[code], one table for each language
-- the game ships (data/lang/<code>.txt). game.lang(p) names the player's
-- language; a language with no table, or a key a table leaves out, uses
-- the English. The other languages' tables are at the end of the file.
--
-- A text is picked when it is sent, never kept: a popup when it is shown,
-- an announcement when it goes out (say), the status line and the panel
-- at each poll. So a player who changes language mid-game reads the next
-- line in the new one.
--
-- An entry with a number in it is a string.format pattern ("%d"), or for
-- English, which writes "1 shot" and "2 shots", a function given the same
-- numbers. The other languages write the number after a colon, so no
-- plural is needed.
--
-- A popup may be a table of strings when it is longer than the 511 bytes
-- a popup may be: they go out one after the other.
--
-- A text worded differently for each input is a table of { keyboard = ,
-- touch = , controller = }, picked by game.input(p); one it leaves out is
-- the keyboard's. Each of the three may itself be a popup's parts. A
-- status line is 128 bytes at most, so it holds only the one wording.
--
-- The words for single keys and buttons are tokens the client fills in
-- for the player's own input: {FIRE}, {GUN_UP} and the rest.
-- docs/SCENARIO_API.md lists them.
local TEXT = {}

TEXT.en = {
  pop = {
    welcome = "Welcome to WinBolo! Bolo is a multiplayer tank game Stuart "
      .. "Cheshire first wrote in 1987, and WinBolo carries it on today. You "
      .. "drive a tank round an island of pillboxes, gun towers that shoot at "
      .. "enemy tanks, and bases, which refill your shells, mines and armour. "
      .. "A small man rides in your tank and gets out to cut trees and build "
      .. "for you. You play in teams, and the team that holds the pillboxes "
      .. "and bases holds the island.",
    s1 = {
      keyboard = "{ACCEL} forward, {BRAKE} slow down, {LEFT} and {RIGHT} turn.\n\nThe "
        .. "patches beside the road are forest, swamp, rubble, craters and a "
        .. "shallow river. Each slows you down by a different amount. Try "
        .. "driving on a few of them.\n\nDeep water sinks tanks. If you die "
        .. "anywhere here, you come back at your last checkpoint.",
      touch = "Drive with the thumbstick on the left of the screen: push it the "
        .. "way you want to go. The further you push it, the faster you "
        .. "go.\n\nThe patches beside the road are forest, swamp, rubble, "
        .. "craters and a shallow river. Each slows you down by a different "
        .. "amount. Try driving on a few of them.\n\nDeep water sinks tanks. If "
        .. "you die anywhere here, you come back at your last checkpoint.",
    },
    s2a = "Bases refill your shells, mines and armour, and your tank is low.\n\n"
      .. "Drive onto your base at the green marker. Then STOP on the base and "
      .. "wait while it refills.",
    s2stay = "This is your base. Stay on it and do not move: your shells, "
      .. "mines and armour flow in a little at a time.",
    s2b = "Neutral bases don't have red or green and can be claimed by "
      .. "driving over them.",
    -- %d: the shells a full base takes.
    s2c = "Enemy bases must be shot empty first. A full one takes %d shots: "
      .. "aim at it and fire with {FIRE}. Then drive over it to make it "
      .. "yours.",
    s2d = "What your tank spawns with depends on game type.\n\n"
      .. "Open: tanks always respawn fully loaded.\n\n"
      .. "Tournament: the more bases are taken, the fewer shells a tank "
      .. "respawns with; once all bases are taken, tanks respawn without "
      .. "shells.\n\n"
      .. "Strict tournament: tanks never respawn with shells.\n\n"
      .. "Tournament modes have a clear win condition: Once one team owns all "
      .. "the bases, they win, because the enemy team can't get any ammo.",
    s3 = {
      keyboard = "Your man harvests trees, then uses them to build roads, walls, "
        .. "mines and pillboxes. Click a build button, or press its key: "
        .. "{QUICK_TREE} trees, {QUICK_ROAD} road, {QUICK_WALL} wall, "
        .. "{QUICK_MINE} mine. Then click on the map to send your man "
        .. "there.\n\nTip: with the road, wall or pillbox tool picked, click a "
        .. "tree and your man harvests it. You do not have to switch to the "
        .. "tree tool each time.",
      touch = "Your man harvests trees, then uses them to build roads, walls, "
        .. "mines and pillboxes. Tap a build button: trees, road, wall or mine. "
        .. "Then tap the map to send your man there.\n\nTip: with the road, "
        .. "wall or pillbox tool picked, tap a tree and your man harvests it. "
        .. "You do not have to switch to the tree tool each time.",
      controller = "Your man harvests trees, then uses them to build roads, walls, "
        .. "mines and pillboxes. Pull {BUILD_MODE} for the build cursor and aim "
        .. "it with the right stick. Press {BUILD_TOOL} to pick a tool: trees, "
        .. "road, wall or mine. Then press {BUILD_PLACE} to send your man to "
        .. "the cursor.\n\nTip: with the road, wall or pillbox tool picked, put "
        .. "the build cursor on a tree and press {BUILD_PLACE}, and your man "
        .. "harvests it. You do not have to switch to the tree tool each time.",
    },
    s4a = {
      keyboard = "Games start with neutral pillboxes, and a full one takes 15 shots "
        .. "to kill.\n\nFirst, drive over the dead pillbox to put it in your "
        .. "tank. Then press the pillbox build button or its shortcut key "
        .. "{QUICK_PILL}. Then click on the map to send your man out to build a "
        .. "pillbox loyal to you at that location.",
      touch = "Games start with neutral pillboxes, and a full one takes 15 shots "
        .. "to kill.\n\nFirst, drive over the dead pillbox to put it in your "
        .. "tank. Then tap the pillbox build button, and tap the map to send "
        .. "your man out to build a pillbox loyal to you at that location.",
      controller = "Games start with neutral pillboxes, and a full one takes 15 shots "
        .. "to kill.\n\nFirst, drive over the dead pillbox to put it in your "
        .. "tank. Then press {BUILD_TOOL} until the pillbox tool is picked, put "
        .. "the build cursor where you want it and press {BUILD_PLACE}. Your "
        .. "man goes out and builds a pillbox loyal to you there.",
    },
    s4b = "East of the road, a damaged pillbox sits in a ring of forest. It "
      .. "still fires, and a pillbox you hit gets angry and fires faster. The "
      .. "red markers show its range.\n\n"
      .. "Forests hide tanks from both other tanks and pillboxes. Drive into "
      .. "the forest inside the markers and sit still. Firing reveals your "
      .. "position.",
    s4c = "This pillbox is already badly damaged: two more shots kill it.\n\n"
      .. "Your tank is full. Kill it, then drive over it to pick it up.",
    s5a = "Stop on the green square ahead. Watch the friendly bot on the "
      .. "walled island take a pillbox. It builds its own pillbox right next "
      .. "to the enemy one and "
      .. "parks behind it, so the enemy's shots hit its pillbox, not its tank. "
      .. "Then it shoots the enemy pillbox dead and drives over it.",
    s5b = "Your turn: you carry a pillbox. Build your pillbox at the marked "
      .. "green square, right next to theirs. Then stop at the road tile "
      .. "highlighted and aim at the enemy pillbox: fire until it dies. Your "
      .. "pillbox takes its shots, and is rebuilt if it dies. Drive into the "
      .. "RESET pen to start over.\n\n"
      .. "Start with full armour. Note that it's normal to take some tank "
      .. "damage after the enemy pillbox has shot through the blocker "
      .. "pillbox.",
    s6 = "Kill an enemy man and his tank cannot build until a new man "
      .. "arrives. You cannot run a man over: only shells, mines and "
      .. "explosions kill him.\n\n"
      .. "A shell explodes at your crosshair and kills a man within half a "
      .. "square. Press {GUN_DOWN} to bring the crosshair in and {GUN_UP} to "
      .. "move it out. "
      .. "For this tutorial, a bot has been programmed to send the man out "
      .. "across the craters. Put your crosshair there and keep firing as he "
      .. "crosses.",
    s6gun = "Got him! Now press {GUN_UP} until your crosshair is back at its "
      .. "longest range, ready for the next station.",
    s7 = "The final round, against an Easy bot. You hold three corners of "
      .. "the island and the bot holds the north-east. Take its land with "
      .. "your pillboxes, then steal its bases as you did in Station 2. You "
      .. "win when you own every base. Drive into the RESET pen to start "
      .. "over.\n\n"
      .. "First, drive over the two dead pillboxes on the road in to put them "
      .. "in your tank. Build your pillboxes near the border between your "
      .. "land and theirs to take control.",
    win = "Well done: you own every base, and the tutorial is complete.\n\n"
      .. "Drive about as long as you like. Leave from the menu when you are "
      .. "ready, then try a game against Easy bots.",
  },

  -- The panel's heading for each station.
  title = { "Station 1: Driving", "Station 2: Bases", "Station 3: Building",
            "Station 4: Pillboxes", "Station 5: Taking a pill",
            "Station 6: The man", "Station 7: Final round" },

  -- The checklist labels, by goal key. A label is short enough for one
  -- panel line in the small font.
  list = {
    forest = "Forest", swamp = "Swamp", rubble = "Rubble",
    crater = "Craters", river = "Shallow river",
    a_refill = "Refill at your base", b_take = "Take the neutral base",
    c_shoot = "Shoot enemy base empty", c_take = "Drive over it",
    harvest = "Harvest a tree", road = "Build a road", wall = "Build a wall",
    mine = "Lay a mine", shoot = "Shoot down the wall",
    a_pick = "Pick up the dead pill", a_build = "Build it",
    b_in = "Enter its range in forest", b_hide = "Sit hidden in range",
    c_kill = "Kill the pillbox", c_pick = "Pick the pillbox up",
    a_watch = "Watch the bot take one", b_build = "Build on the green square",
    b_park = "Park on the road square", b_kill = "Kill the enemy pill",
    b_pick = "Pick it up",
    kill = "Shoot the man",
    dead = "Pick up both dead pills", build = "Pill in enemy land",
    base = "Steal an enemy base", all = "Take every enemy base",
  },

  -- The status line for each goal: what to do next. A line too wide for
  -- the game view at a 1280-wide window (about 390 of the client's layout
  -- units, with the longest controller button words in) is split in two
  -- with "\n"; the client draws each row centred (docs/SCENARIO_API.md,
  -- "Line breaks").
  step = {
    a_refill = "Drive onto the green-marked base, then STOP\non it and wait "
      .. "while it refills.",
    b_take = "Drive onto the neutral base to make it yours.",
    -- The shots the enemy base still needs.
    c_shoot = function(left)
      return string.format("Shoot the red-marked base with {FIRE}:\n%d "
        .. "shot%s left.", left, left == 1 and "" or "s")
    end,
    c_take = "The base is empty. Drive onto it to make it yours.",
    harvest = {
      keyboard = "Click the tree button or press {QUICK_TREE},\nthen click a forest "
        .. "square in the marked grove.",
      touch = "Tap the tree button, then tap a forest\nsquare in the marked grove.",
      controller = "Pick the tree tool with {BUILD_TOOL}, put the build\ncursor on the "
        .. "marked grove and press {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Click the road button or press {QUICK_ROAD},\nthen click a grass "
        .. "square.",
      touch = "Tap the road button, then tap a grass square.",
      controller = "Pick the road tool with {BUILD_TOOL}, put the build\ncursor on "
        .. "grass and press {BUILD_PLACE}.",
    },
    wall = {
      keyboard = "Build a wall: Click on the wall button\nor press {QUICK_WALL}, then "
        .. "click a grass square.",
      touch = "Build a wall: tap the wall button,\nthen tap a grass square.",
      controller = "Build a wall: pick the wall tool with {BUILD_TOOL},\nput the build "
        .. "cursor on grass and press {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Click the mine button or press {QUICK_MINE},\nthen click a square "
        .. "to lay a mine.",
      touch = "Tap the mine button, then tap\na square to lay a mine.",
      controller = "Pick the mine tool with {BUILD_TOOL}, put the build\ncursor on a "
        .. "square and press {BUILD_PLACE}.",
    },
    shoot = "Shoot the marked wall with {FIRE}\nuntil it falls.",
    a_pick = "Drive over the dead pillbox to put it in your tank.",
    a_build = {
      keyboard = "Press the pillbox button or {QUICK_PILL},\nthen click on the map to "
        .. "build it.",
      touch = "Tap the pillbox button,\nthen tap the map to build it.",
      controller = "Pick the pillbox tool with {BUILD_TOOL}, put the\nbuild cursor down "
        .. "and press {BUILD_PLACE} to build it.",
    },
    b_in = "Drive into the forest inside the red markers.",
    -- %d: whole seconds hidden so far.
    b_hide = "Sit still deep in the forest, in range.\nHidden: %d of 5 "
      .. "seconds.",
    c_kill = "Shoot the pillbox twice with {FIRE}.",
    c_pick = "Drive over the dead pillbox to pick it up.",
    a_watch = "Stop on the green square ahead\nand watch the bot "
      .. "take the pillbox.",
    b_build = {
      keyboard = "Press the pillbox button or {QUICK_PILL},\nthen click the green "
        .. "square.",
      touch = "Tap the pillbox button,\nthen tap the green square.",
      controller = "Pick the pillbox tool with {BUILD_TOOL}, put the build\ncursor on "
        .. "the green square and press {BUILD_PLACE}.",
    },
    b_park = "Stop at the road tile highlighted\nand aim at the enemy "
      .. "pillbox.",
    b_kill = "Shoot the enemy pillbox with {FIRE}\nuntil it dies.",
    b_pick = "Drive over the dead pillbox to pick it up.",
    kill = "Put your crosshair on the craters, then\nfire as the man "
      .. "crosses them.",
    dead = "Drive over both dead pillboxes on the road in.",
    build = "Build your pillboxes near the border, in the\nbot's land "
      .. "(north-east), to take control.",
    base = "Shoot an enemy base empty with {FIRE},\nthen drive onto it.",
    -- %d: the Station 7 bases the player owns.
    all = "Take every enemy base: %d of 8 are yours.",
  },

  -- Status lines that belong to no one goal.
  line = {
    gun = "Press {GUN_UP} until your crosshair\nis at its longest range.",
    shells = "Low on shells? Stop on the base\nby the road to refill.",
    done = "Station done. Follow the road north.",
    win = "Tutorial complete. Leave from the\nmenu when you are ready.",
  },

  -- The panel's yellow line under the checklist.
  info = {
    on = "On %s: speed %d",            -- terrain word, its speed rule
    on_plain = "On %s",                -- terrain word with no speed rule
    enemy_base = "Enemy base: %d shots left",
    wall_down = "Target wall: down",
    wall_left = function(left)
      return string.format("Target wall: %d shot%s left", left,
                           left == 1 and "" or "s")
    end,
    pill_armour = "Pillbox armour: %d/%d",
    men = "Men killed: %d",
    bases = "Bases yours: %d/8",
    needed = "%d of %d needed",        -- Station 1's terrains
  },

  -- The panel's bottom line.
  hint = {
    stop = "STOP on the base: wait",
    full = "Full",
    refill = "Refilling: wait here",
    low = "Low on shells: refill",
    reset = "RESET pen: start over",
  },

  -- What the panel calls the square the tank is on, by the script's own
  -- terrain words (game.TERRAIN's names, "_" written as a space; the half
  -- building is "wall"). Only for showing: the script's logic compares the
  -- English words.
  terrain = {
    ["deep sea"] = "deep sea", building = "building", river = "river",
    swamp = "swamp", crater = "crater", road = "road", forest = "forest",
    rubble = "rubble", grass = "grass", wall = "wall", boat = "boat",
  },

  -- The white lines at the top of the view.
  say = {
    done = "Station %d done. Follow the road north.",
    done4 = "Station %d done. Follow the road north.\nShoot trees to "
      .. "clear the trees quickly.",
    reset = "Station %d reset",
    back = "The enemy pillbox is back.\nTake it again, or go north.",
    moved = "Moved to the green square.",
  },
}

-- The text tables in the language player p reads, or the English.
local function lang_text(p)
  if p == nil then return TEXT.en end
  return TEXT[game.lang(p)] or TEXT.en
end

-- An entry's wording for the input player p uses, for an entry worded for
-- each input; any other entry as it is.
local function for_input(v, p)
  if type(v) ~= "table" or v.keyboard == nil then return v end
  if p == nil then return v.keyboard end
  return v[game.input(p)] or v.keyboard
end

-- One entry of TEXT for the tutorial's player: section, then key. Numbers
-- after the key fill its pattern. A key the player's language leaves out
-- is the English.
local function tr(section, key, ...)
  local mine = lang_text(S.player)[section]
  local v = mine and mine[key]
  if v == nil then v = TEXT.en[section][key] end
  v = for_input(v, S.player)
  if type(v) == "function" then return v(...) end
  if select("#", ...) == 0 then return v end
  -- A popup written in parts: each part takes the numbers it uses.
  if type(v) == "table" then
    local parts = {}
    for i, part in ipairs(v) do parts[i] = string.format(part, ...) end
    return parts
  end
  return string.format(v, ...)
end

-- The English names the arenas and the logic read: the popups by id, and
-- each station's checklist as { key, label } in its order.
local POP = TEXT.en.pop

local LIST_KEYS = {
  { "forest", "swamp", "rubble", "crater", "river" },
  { "a_refill", "b_take", "c_shoot", "c_take" },
  { "harvest", "road", "wall", "mine", "shoot" },
  { "a_pick", "a_build", "b_in", "b_hide", "c_kill", "c_pick" },
  { "a_watch", "b_build", "b_park", "b_kill", "b_pick" },
  { "kill" },
  { "dead", "build", "base", "all" },
}
local LIST = {}
for n, keys in ipairs(LIST_KEYS) do
  LIST[n] = {}
  for i, k in ipairs(keys) do LIST[n][i] = { k, TEXT.en.list[k] } end
end

-- Station 1 is done after this many of its five terrains.
local TERRAINS_NEEDED = 3

-- Station 5A: a tank that has come within WATCH_NEAR squares (the larger of
-- the x and y distances) of the watch spot ticks "Watch the bot take one"
-- WATCH_NEAR_FRAMES on_tick calls (10 seconds) later, on the spot or not.
local WATCH_NEAR = 5
local WATCH_NEAR_FRAMES = 500

local PILLS7 = { "p7_nw1", "p7_nw2", "p7_se1", "p7_se2", "p7_sw1", "p7_sw2" }
local BASES7 = { "b7_ne1", "b7_ne2", "b7_nw1", "b7_nw2", "b7_se1", "b7_se2",
                 "b7_sw1", "b7_sw2" }
-- The friendly bases by the road where the player shoots a lot.
local SUPPLY = { "b2a", "b3", "b4c", "b5", "b6" }

-- The numbers a status line's pattern takes, by goal key.
local STEP_ARGS = {
  c_shoot = function()
    local b = game.base(B.b2c.n)
    return b and base_shots(b.armour) or 0
  end,
  b_hide = function() return math.floor(S.hide) end,
  all = function()
    local mine = 0
    for _, k in ipairs(BASES7) do
      local b = game.base(B[k].n)
      if b ~= nil and b.owner == S.player then mine = mine + 1 end
    end
    return mine
  end,
}

-- The station a pill belongs to: one the player carries out of it, or puts
-- down outside it, is sent back.
local PILL_HOME = { p4a = 4, p4 = 4, t5a_target = 5, t5a_blocker = 5,
                    t5b_target = 5, t5b_blocker = 5 }
for _, k in ipairs(PILLS7) do PILL_HOME[k] = 7 end

local PILL_NAME = {}
for k, v in pairs(P) do PILL_NAME[v.n] = k end

-- Terrain codes to the words the panel shows. A mined square reads as the
-- terrain under the mine.
local TERRAIN_WORD = {}
for word, code in pairs(game.TERRAIN) do
  local plain = string.match(word, "^mine_(.+)$") or word
  TERRAIN_WORD[code] = (string.gsub(plain, "_", " "))
end
TERRAIN_WORD[game.TERRAIN.half_building] = "wall"

local function terrain_at(x, y)
  return TERRAIN_WORD[game.map_tile(x, y) or -1]
end

-- What a tank gets on first driving into a station, and respawns with
-- there. Station 2 starts low until its base refill is done, so there is
-- something to refill; Stations 1 to 3 carry no trees, so Station 3's
-- harvest is needed.
local function station_loadout(n)
  local t = full_stock()
  if n <= 3 then t.trees = 0 end
  if n == 2 and not S.done[2].a_refill then
    t.shells, t.mines, t.armour = 5, 0, 15
  end
  return t
end

local function region_has(name, x, y)
  return game.in_region(name, x, y)
end

local function station_of(x, y)
  for n = 1, 7 do
    if region_has("s" .. n, x, y) then return n end
  end
  return nil
end

local function player_tank()
  if S.player == nil then return nil end
  local t = game.tank(S.player)
  if t == nil or t.dead then return nil end
  return t
end

local function is_player_seat(p)
  if TUTORIAL_PLAYER ~= nil then return p == TUTORIAL_PLAYER end
  local s = game.lobby_slot(p)
  return s ~= nil and not s.bot
end

local function is_demo_bot(p)
  return p ~= nil and (p == S.bot5a or p == S.bot6)
end

local function owner_or_neutral(p)
  if p == nil then return game.NEUTRAL end
  return p
end

local function live_tank(p)
  if p == nil then return nil end
  local t = game.tank(p)
  if t == nil or t.dead then return nil end
  return t
end

-- ---------------------------------------------------------------------
-- Putting things back.

-- Put pill `name` on its own square with an owner and armour, taking it out
-- of whichever tank carries it first.
local function pill_home(name, owner, armour)
  local L = P[name]
  local n = L.n
  local pb = game.pill(n)
  if pb == nil then return false end
  if pb.in_tank then
    local ok = false
    local c = S.carrier[n]
    if c ~= nil then ok = game.drop_pill(c, n, L.x, L.y) end
    if not ok then
      for q = 0, game.max_tanks() - 1 do
        local t = game.tank(q)
        if t ~= nil and (t.pills or 0) > 0 and game.drop_pill(q, n, L.x, L.y) then
          ok = true
          break
        end
      end
    end
    if not ok then return false end
  elseif pb.x ~= L.x or pb.y ~= L.y then
    game.move_pill(n, L.x, L.y)
  end
  S.carrier[n] = nil
  game.set_pill_owner(n, owner_or_neutral(owner))
  game.set_pill_armour(n, armour)
  return true
end

local function base_home(name, owner)
  local n = B[name].n
  game.set_base_owner(n, owner_or_neutral(owner))
  game.set_base_stock(n, rule("base_full_armour"), rule("base_full_shells"),
                      rule("base_full_mines"))
end

local function rebuild_walls(list)
  for _, w in ipairs(list) do
    game.set_tile(w[1], w[2], game.TERRAIN.building)
  end
end

local function rebuild_road(list)
  for _, w in ipairs(list) do
    game.set_tile(w[1], w[2], game.TERRAIN.road)
  end
end

-- The 4B/4C pillbox's armour: two shots kill it, so the player is shot at
-- while killing it in 4C.
local P4_ARMOUR = 2

-- The owner and armour each pill starts with.
local function pill_start(name)
  if name == "p4a" or name == "p7_sw1" or name == "p7_sw2" or
     name == "t5a_blocker" or name == "t5b_blocker" then
    return nil, 0
  elseif name == "p4" then
    -- Damaged from the start: two shots kill it in 4C. It is never
    -- repaired (rearm).
    return nil, P4_ARMOUR
  elseif name == "t5a_target" then
    return nil, rule("pill_max_armour")
  elseif name == "t5b_target" then
    return S.bot6, rule("pill_max_armour")
  end
  return S.player, rule("pill_max_armour")
end

local function reset_pill(name)
  local owner, armour = pill_start(name)
  return pill_home(name, owner, armour)
end

local function base_start(name)
  if name == "b2b" then return nil end
  if name == "b2c" then return S.bot6 end
  if name == "b7_ne1" or name == "b7_ne2" then return S.bot7 end
  return S.player
end

local function reset_base(name) base_home(name, base_start(name)) end

-- Put pill n into tank p, unless it is there already.
local function hand_pill(p, n)
  if p == nil or live_tank(p) == nil then return false end
  local pb = game.pill(n)
  if pb ~= nil and pb.in_tank and S.carrier[n] == p then return true end
  if game.give_pill(p, n) then
    S.carrier[n] = p
    return true
  end
  return false
end

-- Station 5B: until the player has built it, the blocker pillbox is in the
-- player's tank. Called the moment 5B becomes the goal, on every entry to
-- Station 5 and on every spawn there, as well as by the once-a-second
-- re-arm. A blocker already carried, or already built, is left alone.
local function hand_5b()
  if S.player == nil or S.done[5].b_build then return end
  local n = P.t5b_blocker.n
  local bl = game.pill(n)
  if bl == nil then return end
  if bl.in_tank and S.carrier[n] == S.player then return end
  hand_pill(S.player, n)
end

-- ---------------------------------------------------------------------
-- The checklist.

local function station_done(n)
  if n == 1 then
    local c = 0
    for _, item in ipairs(LIST[1]) do
      if S.done[1][item[1]] then c = c + 1 end
    end
    return c >= TERRAINS_NEEDED
  end
  for _, item in ipairs(LIST[n]) do
    if not S.done[n][item[1]] and not S.passed[n][item[1]] then
      return false
    end
  end
  return true
end

-- The first goal of station n that is neither ticked nor passed over, or
-- nil.
local function current_goal(n)
  for _, item in ipairs(LIST[n]) do
    if not S.done[n][item[1]] and not S.passed[n][item[1]] then
      return item[1]
    end
  end
  return nil
end

-- ---------------------------------------------------------------------
-- Popups. A popup that follows something the player did waits about 3
-- seconds (POPUP_DELAY on_tick calls), so the player sees what happened
-- first; it shows at once if the player has already moved on to the next
-- goal or station.

local POPUP_DELAY = 150   -- on_tick runs 50 times a second

-- The numbers a popup's pattern takes, by id.
local POP_ARGS = {
  s2c = function() return base_shots(rule("base_full_armour")) end,
}

local function show(id)
  if S.popped[id] or S.player == nil then return end
  S.popped[id] = true
  local args = POP_ARGS[id]
  local text
  if args ~= nil then text = tr("pop", id, args()) else text = tr("pop", id) end
  -- A popup too long for one is written as two, sent one after the other.
  if type(text) == "table" then
    for _, part in ipairs(text) do game.popup(part, S.player) end
  else
    game.popup(text, S.player)
  end
  if id == "s4c" then
    local t = player_tank()
    if t ~= nil then game.set_stocks(S.player, full_stock()) end
  end
end

-- White text after its popup, so the popup is read first. While a popup
-- is waiting in S.queue, an announcement waits in
-- S.says and the status line is blank (draw_status); both go out once the
-- queue is empty, so they follow the popup. In the same tick as the popup
-- is fine: the client draws no white text while a popup is open
-- (clientSimScnAnnounceHold) and starts the line's time when it closes.
local function release_says()
  if #S.queue > 0 or #S.says == 0 then return end
  local says = S.says
  S.says = {}
  -- "top": every white line sits at the top edge of the view, under the
  -- status line.
  for _, a in ipairs(says) do
    S.say_until = math.max(S.say_until or 0, S.frames + a.secs * 50)
    local text
    if a.num ~= nil then text = tr("say", a.key, a.num)
    else text = tr("say", a.key) end
    game.announce(text, a.secs, S.player, "top")
  end
end

-- A white line: TEXT's "say" entry key, up for secs seconds, with the
-- number its pattern takes (num, or nil). One that has to wait for a popup
-- keeps its key and number, not its text, so it is worded in the language the
-- player reads when it goes out.
local function say(key, secs, num)
  if S.player == nil then return end
  if #S.queue > 0 then
    S.says[#S.says + 1] = { key = key, secs = secs, num = num }
    return
  end
  -- S.say_until: the frame the last white line comes down. The status
  -- line's "Station done" waits for it, so the two never say it together.
  S.say_until = math.max(S.say_until or 0, S.frames + secs * 50)
  local text
  if num ~= nil then text = tr("say", key, num) else text = tr("say", key) end
  game.announce(text, secs, S.player, "top")
end

local function popup_later(id)
  if S.popped[id] then return end
  for _, q in ipairs(S.queue) do
    if q.id == id then return end
  end
  S.queue[#S.queue + 1] = { id = id, due = S.frames + POPUP_DELAY }
end

local function flush_popups(all)
  if #S.queue == 0 then return end
  local keep = {}
  for _, q in ipairs(S.queue) do
    if all or S.frames >= q.due then show(q.id) else keep[#keep + 1] = q end
  end
  S.queue = keep
  release_says()
end

-- The popup station n needs next, for what the player has done there.
local function next_popup(n)
  local d = S.done[n]
  if n == 1 then return "s1"
  elseif n == 2 then
    if d.c_take then return "s2d" end
    if d.b_take then return "s2c" end
    if d.a_refill then return "s2b" end
    return "s2a"
  elseif n == 3 then return "s3"
  elseif n == 4 then
    -- s4c says how to kill the pillbox: not for one already dead.
    if d.c_kill then return nil end
    if d.b_hide then return "s4c" end
    if d.a_build then return "s4b" end
    return "s4a"
  elseif n == 5 then
    if d.a_watch then return "s5b" end
    return "s5a"
  elseif n == 6 then return "s6"
  elseif n == 7 then
    if d.all then return "win" end
    return "s7"
  end
  return nil
end

-- ---------------------------------------------------------------------
-- The panel, the markers and the status line.

local function on_friendly_base(t)
  for name, L in pairs(B) do
    if t.mx == L.x and t.my == L.y then
      local b = game.base(L.n)
      if b ~= nil and b.owner == S.player then return b end
    end
  end
  return nil
end

local function station_info(n, t)
  if n == 1 then
    if t == nil then return nil end
    local word = terrain_at(t.mx, t.my)
    if t.boat then word = "boat" end
    if word == nil then return nil end
    local speeds = { grass = "speed_grass", road = "speed_road",
                     forest = "speed_forest", river = "speed_river",
                     swamp = "speed_swamp", crater = "speed_crater",
                     rubble = "speed_rubble", boat = "speed_boat" }
    local r = speeds[word]
    local shown = tr("terrain", word) or word
    if r == nil then return tr("info", "on_plain", shown) end
    return tr("info", "on", shown, rule(r))
  elseif n == 2 then
    local b = game.base(B.b2c.n)
    -- Only once the neutral base is taken: until then the enemy base is
    -- not the goal yet.
    if b ~= nil and b.owner ~= S.player and S.done[2].b_take and
       not S.done[2].c_shoot then
      return tr("info", "enemy_base", base_shots(b.armour))
    end
  elseif n == 3 then
    local w = PT.wall3
    local left = game.wall_shots(w[1], w[2])
    if left == nil or left <= 0 then return tr("info", "wall_down") end
    return tr("info", "wall_left", left)
  elseif n == 4 then
    local pb = game.pill(P.p4.n)
    if pb ~= nil and not pb.in_tank then
      return tr("info", "pill_armour", pb.armour, rule("pill_max_armour"))
    end
  elseif n == 6 then
    return tr("info", "men", S.men)
  elseif n == 7 then
    local mine = 0
    for _, k in ipairs(BASES7) do
      local b = game.base(B[k].n)
      if b ~= nil and b.owner == S.player then mine = mine + 1 end
    end
    return tr("info", "bases", mine)
  end
  return nil
end

local function tank_full(t)
  return t.shells >= rule("tank_full_shells") and
         t.armour >= rule("tank_full_armour") and
         t.mines >= rule("tank_full_mines")
end

local function panel_hint(n, t)
  if t ~= nil then
    local b = on_friendly_base(t)
    if b ~= nil then
      if S.moving then return tr("hint", "stop"), "yellow" end
      if tank_full(t) then return tr("hint", "full"), "green" end
      return tr("hint", "refill"), "yellow"
    end
    if t.shells < 5 and n >= 2 and n <= 6 then
      return tr("hint", "low"), "yellow"
    end
  end
  if n == 5 or n == 7 then return tr("hint", "reset"), "grey" end
  return nil
end

local function draw_panel(force)
  if S.player == nil then return end
  local n = S.at
  local t = player_tank()
  local sdone = station_done(n)
  local list = {
    { "rect", 0, 0, 128, 14, sdone and "green" or "grey_dark", true },
    { "text", 64, 2, "white", "normal", "centre", tr("title", n) },
  }
  -- The key holds every word drawn, so a change of language redraws it.
  local key = { n, sdone and "D" or "-", tr("title", n) }
  local y = 18
  for _, item in ipairs(LIST[n]) do
    local done = S.done[n][item[1]] == true
    local label = tr("list", item[1])
    local colour = done and "green" or (sdone and "grey" or "white")
    list[#list + 1] = { "rect", 4, y + 1, 6, 6, done and "green" or "grey", done }
    list[#list + 1] = { "text", 13, y, colour, "small", "left", label }
    key[#key + 1] = label .. (done and "1" or "0")
    y = y + 10
  end
  if n == 1 then
    local c = 0
    for _, item in ipairs(LIST[1]) do
      if S.done[1][item[1]] then c = c + 1 end
    end
    local line = tr("info", "needed", math.min(c, TERRAINS_NEEDED),
                    TERRAINS_NEEDED)
    list[#list + 1] = { "text", 64, y, sdone and "green" or "white", "small",
                        "centre", line }
    key[#key + 1] = line
  end
  local info = station_info(n, t)
  if info ~= nil then
    list[#list + 1] = { "text", 64, 106, "yellow", "small", "centre", info }
    key[#key + 1] = info
  end
  local hint, colour = panel_hint(n, t)
  if hint ~= nil then
    list[#list + 1] = { "text", 64, 117, colour, "small", "centre", hint }
    key[#key + 1] = hint
  end
  local k = table.concat(key, "|")
  if not force and k == S.panel_key then return end
  if game.panel(0, list, S.player) then S.panel_key = k end
end

-- The Station 4 pillbox's range as 16 marks round it, worked out on first
-- use: the rules are not there while the file loads.
local RING = nil
local function range_ring()
  if RING ~= nil then return RING end
  RING = {}
  local cx, cy = PT.p4[1], PT.p4[2]
  local r = rule("pill_range") / 256
  for i = 0, 15 do
    local a = i * math.pi / 8
    RING[#RING + 1] = { math.floor(cx + r * math.cos(a) + 0.5),
                        math.floor(cy + r * math.sin(a) + 0.5), "red" }
  end
  return RING
end

-- The marker for the goal the player is on: one at a time, gone the moment
-- the goal ticks. Station 4B shows the pillbox's range as a ring. The RESET
-- pens carry no marker: the pen, its walls and its arrow show it.
local function goal_markers(n)
  local g = current_goal(n)
  local m = {}
  if n == 2 then
    if g == "a_refill" then m = { { B.b2a.x, B.b2a.y, "green" } }
    elseif g == "b_take" then m = { { B.b2b.x, B.b2b.y, "grey" } }
    elseif g == "c_shoot" or g == "c_take" then
      m = { { B.b2c.x, B.b2c.y, "red" } }
    end
  elseif n == 3 then
    if g == "harvest" then
      local gr = LAYOUT.grove3
      m = { { math.floor((gr[1] + gr[3]) / 2), math.floor((gr[2] + gr[4]) / 2),
              "green" } }
    elseif g == "shoot" then
      m = { { PT.wall3[1], PT.wall3[2], "green" } }
    end
  elseif n == 4 then
    if g == "a_pick" then m = { { P.p4a.x, P.p4a.y, "yellow" } }
    elseif g == "b_in" or g == "b_hide" then
      m = {}
      for i, v in ipairs(range_ring()) do m[i] = v end
    elseif g == "c_kill" or g == "c_pick" then
      m = { { P.p4.x, P.p4.y, "red" } }
    end
  elseif n == 5 then
    if g == "a_watch" then
      m = { { PT.watch5a[1], PT.watch5a[2], "green" },
            { P.t5a_target.x, P.t5a_target.y, "yellow" } }
    elseif g == "b_build" then
      m = { { PT.t5b_blocker[1], PT.t5b_blocker[2], "green" } }
    elseif g == "b_park" then m = { { PT.t5b_park[1], PT.t5b_park[2], "green" } }
    elseif g == "b_kill" or g == "b_pick" then
      m = { { P.t5b_target.x, P.t5b_target.y, "red" } }
    end
  elseif n == 6 then
    if g == "kill" then
      local c = LAYOUT.region.craters6
      m = { { c.x, c.y, "yellow" } }
    end
  elseif n == 7 then
    if g == "dead" then
      for _, k in ipairs({ "p7_sw1", "p7_sw2" }) do
        if not S.sw[k] then
          m = { { P[k].x, P[k].y, "yellow" } }
          break
        end
      end
    end
  end
  return m
end

local function draw_markers()
  if S.player == nil then return end
  local m = goal_markers(S.at)
  local parts = {}
  for _, v in ipairs(m) do parts[#parts + 1] = v[1] .. "," .. v[2] .. v[3] end
  local k = table.concat(parts, ";")
  if k == S.marker_key then return end
  S.marker_key = k
  for i, v in ipairs(m) do game.marker(i - 1, v[1], v[2], v[3], S.player) end
  for id = #m, S.markers - 1 do game.clear_marker(id, S.player) end
  S.markers = #m
end

local function status_text(t)
  if S.won then return tr("line", "win") end
  if S.gun_remind then return tr("line", "gun") end
  local n = S.at
  if t ~= nil and t.shells < 5 and n >= 2 and n <= 6 and
     on_friendly_base(t) == nil and not (n == 2 and not S.done[2].a_refill)
     then
    return tr("line", "shells")
  end
  -- "Station done" only once the white "Station N done" line is down.
  local done_line = (S.frames < (S.say_until or 0)) and "" or
                    tr("line", "done")
  if n == 1 then
    if station_done(1) then return done_line end
    return ""
  end
  local g = current_goal(n)
  if g == nil then
    if n == 7 then return tr("line", "win") end
    return done_line
  end
  local args = STEP_ARGS[g]
  if args ~= nil then return tr("step", g, args()) or "" end
  return tr("step", g) or ""
end

local STATUS_GAP_FRAMES = 50   -- 1 s at 50 frames a second

local function draw_status()
  if S.player == nil then return end
  local text = status_text(player_tank())
  -- Its popup first: blank while a popup waits to be shown.
  if #S.queue > 0 then text = "" end
  if text == S.status then return end
  -- One line straight to another: blank for STATUS_GAP_FRAMES first, so
  -- the player sees the line change: one line straight into another was
  -- missed in play-testing.
  if S.status_gap_until ~= nil and S.frames < S.status_gap_until then return end
  -- Not when only a number in the line changed ("5 shots left" to "4 shots
  -- left"): that is the same line counting, and it stays up.
  if text ~= "" and S.status ~= nil and S.status ~= "" and
     (text:gsub("%d+", "#")) ~= (S.status:gsub("%d+", "#")) then
    S.status = ""
    S.status_gap_until = S.frames + STATUS_GAP_FRAMES
    game.status("", nil, S.player)
    return
  end
  S.status = text
  game.status(text, nil, S.player)
end

local function redraw(force)
  draw_panel(force)
  draw_markers()
  draw_status()
end

-- ---------------------------------------------------------------------
-- Ticking goals off.

local function mark(n, key)
  if S.done[n][key] then return end
  local was_done = station_done(n)
  S.done[n][key] = true
  game.sound("lobby_ready")
  if n == 5 and key == "a_watch" then hand_5b() end
  -- A popup still waiting is shown now: the player has moved on.
  flush_popups(true)
  -- The next popup is queued before the "done" line, so the line waits for
  -- it (say).
  if S.at == n then
    local id = next_popup(n)
    if id ~= nil then popup_later(id) end
  end
  if not was_done and station_done(n) and n < 7 then
    say(n == 4 and "done4" or "done", 4, n)
  end
  redraw(false)
end

-- ---------------------------------------------------------------------
-- The bots. Their seats are picked as the round starts, so that the 2C
-- base and the 5B pillbox can belong to the Station 6 bot's seat before
-- the bot is on the field; each bot is fielded the first time the player
-- drives into its station. A held seat that is not fielded has no tank.

local ROLE = {
  bot5a = { start = "bot5a", team = 1, name = "Demo" },
  bot6 = { start = "bot6", team = 2, name = "Builder" },
  bot7 = { start = "bot7", team = 2, name = "Final", difficulty = "easy" },
}

local function reserve_seats()
  local held = { {}, {} }
  local free = {}
  for p = 0, game.max_tanks() - 1 do
    if p ~= S.player and p ~= TUTORIAL_PLAYER then
      local s = game.lobby_slot(p)
      if s == nil then
        free[#free + 1] = p
      elseif s.bot and not s.fielded and (s.team == 1 or s.team == 2) then
        local l = held[s.team]
        l[#l + 1] = p
      end
    end
  end
  local function pick(role, list)
    local p = table.remove(list, 1)
    if p == nil then
      p = table.remove(free, 1)
      if p ~= nil then S.free_seat[p] = true end
    end
    if p == nil then game.log("Tutorial: no seat for the " .. role .. " bot") end
    S[role] = p
  end
  pick("bot5a", held[1])
  pick("bot6", held[2])
  pick("bot7", held[2])
end

local function bot_init(role)
  if role == "bot5a" then
    -- The 5A demo bot builds its blocker only on a square orthogonally next
    -- to the target, the take the player is taught in 5B. It is the
    -- player's ally, so its ATTACK markers (bot pings) would reach the
    -- player; they are off. (Its spoken goal lines, BOT_CHAT_DEFAULT, are
    -- off by default.) It never drops its pillbox anywhere else
    -- (STRATEGIC_PLACE_ENABLED off): a pillbox dropped on the island away
    -- from the target leaves the bot with no blocker, and the target kills
    -- it. A value is at most 63 bytes, so the second goes under cfg2; its
    -- own token, "cfg2=0", means nothing to the brain.
    -- The player watching from the green square is not a decoy
    -- (HUMAN_DECOY_AWARE off): with it on, a human standing near the target
    -- counts as the bot's cover and the bot builds no blocker of its own
    -- (HUMAN_DECOY_NO_BLOCKERS), which is the very thing the demo shows.
    return { cfg = "BLOCKER_ORTHOGONAL_ONLY=true;cfg=BOT_PINGS_DEFAULT=false",
             cfg2 = "0;cfg=STRATEGIC_PLACE_ENABLED=false",
             cfg3 = "0;cfg=HUMAN_DECOY_AWARE=false" }
  elseif role == "bot6" then
    -- The Station 6 bot never fights: its man is the target.
    -- It spares every seat but its own and the Station 7 bot's, not only
    -- the player's: a player who leaves may come back on another seat, and
    -- a list that names them all needs no new token (game.bot_init) when
    -- that happens. The 5A demo bot's seat is on the list too, so the list
    -- does not depend on where that bot sits (move_bot_seat can move it);
    -- that bot is held on its island, 24 squares from the parked Station 6
    -- tank, so the two never meet. 14 seats of 16 come to 33 bytes, inside
    -- the 63 a value may have.
    local seats = {}
    for p = 0, game.max_tanks() - 1 do
      if p ~= S.bot6 and p ~= S.bot7 then
        seats[#seats + 1] = tostring(p)
      end
    end
    return { peace = table.concat(seats, "/") }
  elseif role == "bot7" then
    -- The Station 7 bot shoots at the player's man and usually misses.
    -- LGM_MISS_WU / LGM_HIT_PCT: one aim in ten is true; the rest burst one
    -- square off him (behind him when he walks), so he sees the shots land
    -- around him. The man can die: about one shot in ten at him kills him
    -- (tests/scenario/tutorial_bot7_man). CAPTURE_LGM_HUNT stays off (as
    -- Easy has it): that hunt fires on its own line, which the miss knobs
    -- do not reach. Several cfg values share one init value, "A=1;cfg=B=2",
    -- at most 63 bytes.
    -- STAY_AREA (cfg2) keeps every goal, build order and route of the bot
    -- on its island: "x1:y1:x2:y2", inclusive squares, from island7.
    -- The leash in on_tick stays as a backstop.
    local r = LAYOUT.region.island7
    return { cfg = "LGM_MISS_WU=256;cfg=LGM_HIT_PCT=10"
                   .. ";cfg=CAPTURE_LGM_HUNT=false",
             cfg2 = string.format("0;cfg=STAY_AREA=%d:%d:%d:%d",
                                  r.x, r.y, r.x + r.w - 1, r.y + r.h - 1) }
  end
  return nil
end

local function field(role)
  if S.fielded[role] then return end
  S.fielded[role] = true
  local p = S[role]
  if p == nil then return end
  local r = ROLE[role]
  local t = { slot = p, start = ST[r.start].n, loadout = "open",
              init = bot_init(role) }
  if r.difficulty ~= nil then
    t.mode, t.difficulty = "default", r.difficulty
  end
  if S.free_seat[p] then
    t.team, t.brain, t.name = r.team, "GoalHunter", r.name
  end
  local q, code = game.spawn_bot(t)
  if type(q) ~= "number" then
    if code == "SCN_OP_ALREADY" then
      -- The seat is still on the field: a remove_bot (unfield_bot5a) lands
      -- a frame or more after it is asked for. Try again on the next poll.
      S.fielded[role] = nil
      return
    end
    game.log("Tutorial: spawn of the " .. role .. " bot refused: "
             .. tostring(code))
  end
end

-- The 5A demo bot is on the field only while the player's tank is in
-- Station 5. Leaving the station either way, or coming back to life outside
-- it, takes the bot off: its two pillboxes go home first, so nothing it
-- carries is dropped where it stood. The next poll inside Station 5 fields it
-- again.
local function unfield_bot5a()
  if not S.fielded.bot5a then return end
  S.fielded.bot5a = nil
  reset_pill("t5a_target")
  reset_pill("t5a_blocker")
  if S.bot5a ~= nil then
    local ok, code = game.remove_bot(S.bot5a)
    if not ok then
      game.log("Tutorial: removing the 5A bot refused: " .. tostring(code))
    end
  end
end

local function park_bot6()
  local at = PT.bot6_park
  game.teleport(S.bot6, at[1], at[2], 128)
  -- 1 percent, not 0: a modifier of 0 is read as the classic 100. Even 1
  -- percent creeps, so on_tick also puts the tank back on the square's
  -- centre whenever it has moved.
  game.set_modifiers(S.bot6, { speed = 1, accel = 1, turn = 1 })
end

local function hold_bot6()
  local t = live_tank(S.bot6)
  if t == nil then return end
  local at = PT.bot6_park
  if t.wx ~= at[1] * 256 + 128 or t.wy ~= at[2] * 256 + 128 then
    game.teleport(S.bot6, at[1], at[2])
  end
end

local function home_bot5a()
  local h = PT.t5a_home
  if live_tank(S.bot5a) ~= nil then
    game.teleport(S.bot5a, h[1], h[2], 64)
    game.set_stocks(S.bot5a, full_stock())
  end
end

-- Whether a tank, a man out of his tank, or a pillbox on the map stands on
-- square (x, y): a square the 5A ring rebuild must leave alone.
local function square_taken(x, y)
  for _, p in ipairs({ S.player, S.bot5a }) do
    local t = live_tank(p)
    if t ~= nil and t.mx == x and t.my == y then return true end
    local b = p ~= nil and game.builder(p) or nil
    if b ~= nil and b.state ~= "in_tank" and b.state ~= "dead" and
       b.mx == x and b.my == y then
      return true
    end
  end
  for _, L in pairs(P) do
    local pb = game.pill(L.n)
    if pb ~= nil and not pb.in_tank and pb.x == x and pb.y == y then
      return true
    end
  end
  return false
end

-- Put the 5A island's two wall rings and its moat back to the map's
-- squares (LAYOUT.walls5a, LAYOUT.moat5a, written by the map tool): a wall
-- shot down or damaged is a full wall again, and a moat square the man
-- filled is deep water again. A square with a tank, a man or a pillbox on
-- it is left alone.
--
-- 104 wall squares and 52 moat squares, 156 at most, and only a square that
-- changed is written. A frame allows 256 writes, and that count is shared
-- with everything else this file and any mod write in the frame. A write
-- refused for the count (SCN_OP_RATE) stops the pass and sets
-- S.rings5a_again; on_tick runs the pass again on the next frame, and the
-- squares already put back are skipped, so it goes on from where it stopped.
local function rebuild_5a_rings()
  local wall, sea = game.TERRAIN.building, game.TERRAIN.deep_sea
  S.rings5a_again = false
  for _, w in ipairs(LAYOUT.walls5a) do
    local x, y = w[1], w[2]
    local left = game.wall_shots(x, y)
    if (game.map_tile(x, y) ~= wall or
        (left ~= nil and left < rule("building_life") + 1)) and
       not square_taken(x, y) then
      local ok, code = game.set_tile(x, y, wall)
      if not ok and code == "SCN_OP_RATE" then
        S.rings5a_again = true
        return
      end
    end
  end
  for _, w in ipairs(LAYOUT.moat5a) do
    local x, y = w[1], w[2]
    if game.map_tile(x, y) ~= sea and not square_taken(x, y) then
      local ok, code = game.set_tile(x, y, sea)
      if not ok and code == "SCN_OP_RATE" then
        S.rings5a_again = true
        return
      end
    end
  end
end

-- Start the 5A demo take again: the target back, the blocker back in the
-- bot's tank, the island's walls and moat back, and a bot with a fresh goal.
-- The bot is taken off and fielded again: its brain keeps its
-- attack goal across a teleport, so a bot sent home in the middle of a take
-- went on from the step it had reached, past building its blocker, and shot
-- the target with the blocker still in its tank. A bot taken off drops its
-- goal, the way a death does. unfield_bot5a puts both pillboxes home; the
-- poll fields the bot again on the next frame while the player is in
-- Station 5, and on_tank_spawned hands it the blocker.
local function reset_5a()
  rebuild_5a_rings()
  if S.fielded.bot5a then
    unfield_bot5a()
  else
    reset_pill("t5a_target")
    reset_pill("t5a_blocker")
  end
  S.t5a_reset_at = S.sec
end

-- The Station 6 man: whenever he is in the tank, put the one tree back and
-- send him to it. The row he walks is road, with a short span of craters
-- beside the main road for the player to aim at.
local function rearm_path6()
  local r = LAYOUT.region.path6
  local c = LAYOUT.region.craters6
  for x = r.x, r.x + r.w - 1 do
    local want = game.TERRAIN.road
    if x >= c.x and x < c.x + c.w then want = game.TERRAIN.crater end
    if game.map_tile(x, r.y) ~= want then game.set_tile(x, r.y, want) end
  end
end

local function bot6_loop()
  rearm_path6()
  local t = live_tank(S.bot6)
  if t == nil then return end
  local b = game.builder(S.bot6)
  if b == nil or b.state ~= "in_tank" then return end
  local tr = PT.tree6
  if game.map_tile(tr[1], tr[2]) ~= game.TERRAIN.forest then
    game.set_tile(tr[1], tr[2], game.TERRAIN.forest)
  end
  game.set_stocks(S.bot6, { trees = 0 })
  game.builder_order(S.bot6, "trees", tr[1], tr[2])
end

-- ---------------------------------------------------------------------
-- Re-arming: once a second, the station the player is in gets back what a
-- goal still needs while that goal is not ticked.

local function supply_bases()
  for _, k in ipairs(SUPPLY) do
    local b = game.base(B[k].n)
    if b ~= nil and (b.owner ~= S.player or b.shells < rule("base_full_shells")
                     or b.armour < rule("base_full_armour")) then
      base_home(k, S.player)
    end
  end
end

-- Station 5B: is the player's built blocker dead or gone from its square?
local function blocker_5b_gone()
  local L = P.t5b_blocker
  local bl = game.pill(L.n)
  if bl == nil then return false end
  local gone = bl.in_tank or bl.armour == 0 or bl.x ~= L.x or bl.y ~= L.y
  return gone and S.carrier[L.n] ~= S.player
end

-- Station 5B: rebuild the player's blocker on its square at full armour.
-- Nothing is said to the player; the log line is for the arena tests. Only
-- while it is built and the take is not done.
local function rebuild_5b()
  S.b5b_dead_at = nil
  local d = S.done[5]
  if not d.b_build or d.b_pick or not blocker_5b_gone() then return end
  if pill_home("t5b_blocker", S.player, rule("pill_max_armour")) then
    game.log("Tutorial: the 5B blocker was automatically rebuilt")
  end
end

local function rearm(n)
  local d = S.done[n]
  if n == 2 then
    local b = game.base(B.b2b.n)
    if not d.b_take and b ~= nil and b.owner ~= game.NEUTRAL then
      reset_base("b2b")
    end
    b = game.base(B.b2c.n)
    if not d.c_take and b ~= nil and S.bot6 ~= nil and b.owner ~= S.bot6 then
      reset_base("b2c")
    end
  elseif n == 3 then
    local w = PT.wall3
    local tile = game.map_tile(w[1], w[2])
    if not d.shoot and tile ~= game.TERRAIN.building and
       tile ~= game.TERRAIN.half_building then
      game.set_tile(w[1], w[2], game.TERRAIN.building)
    end
    local g = LAYOUT.grove3
    local trees = 0
    for y = g[2], g[4] do
      for x = g[1], g[3] do
        if game.map_tile(x, y) == game.TERRAIN.forest then trees = trees + 1 end
      end
    end
    if trees < 20 then game.fill_rect(g[1], g[2], g[3], g[4], game.TERRAIN.forest) end
  elseif n == 4 then
    local pa = game.pill(P.p4a.n)
    if not d.a_build and pa ~= nil then
      local carried = pa.in_tank and S.carrier[P.p4a.n] == S.player
      if d.a_pick and not carried then
        -- Lost on the way (the tank died): pick it up again.
        S.done[4].a_pick = nil
        reset_pill("p4a")
      elseif not d.a_pick and (pa.in_tank or pa.x ~= P.p4a.x or
                               pa.y ~= P.p4a.y or pa.armour ~= 0) then
        reset_pill("p4a")
      end
    end
    local pb = game.pill(P.p4.n)
    if not d.c_kill and pb ~= nil then
      -- Never repaired: every hit stays, hidden first or not. Only a
      -- pillbox that left its square is put back.
      if pb.in_tank or pb.x ~= P.p4.x or pb.y ~= P.p4.y then
        reset_pill("p4")
      end
    elseif d.c_kill and not d.c_pick and pb ~= nil and pb.in_tank and
           S.carrier[P.p4.n] ~= S.player then
      reset_pill("p4")
      game.set_pill_armour(P.p4.n, 0)
    end
  elseif n == 5 then
    local tg = game.pill(P.t5b_target.n)
    if not d.b_pick and not S.t5b_restore and tg ~= nil then
      local L = P.t5b_target
      if tg.in_tank or tg.x ~= L.x or tg.y ~= L.y then reset_pill("t5b_target") end
    end
    local bl = game.pill(P.t5b_blocker.n)
    if bl ~= nil and not d.b_build then
      -- Not built yet: the player carries it.
      hand_5b()
    elseif bl ~= nil and not d.b_pick then
      -- Built: it is rebuilt 2 seconds after it dies. on_pill_killed starts
      -- the 2 seconds on the tick it dies; this catches any other way it
      -- goes (picked up by another tank), 2 to 3 seconds after.
      if blocker_5b_gone() and not S.b5b_timer then
        S.b5b_dead_at = S.b5b_dead_at or S.sec
        if S.sec - S.b5b_dead_at >= 3 then rebuild_5b() end
      else
        S.b5b_dead_at = nil
      end
    end
  end
end

-- ---------------------------------------------------------------------
-- Station resets: the RESET pens in Stations 5 and 7.

local function reset_station(n)
  S.done[n] = {}
  S.passed[n] = {}
  if n == 5 then
    reset_pill("t5b_target")
    reset_pill("t5b_blocker")
    hand_5b()
    rebuild_walls(LAYOUT.walls5b)
    rebuild_road(LAYOUT.arrow_reset5b)
    S.b5b_dead_at = nil
    -- Watching the bot is not undone: it keeps taking pills either way.
    S.done[5].a_watch = S.popped.s5b and true or nil
  elseif n == 7 then
    for _, k in ipairs(PILLS7) do reset_pill(k) end
    for _, k in ipairs(BASES7) do reset_base(k) end
    rebuild_walls(LAYOUT.walls7)
    rebuild_road(LAYOUT.arrow_reset7)
    -- The two dead pillboxes of the road in go straight back into the
    -- player's tank: the pen is far past the road, and driving back for
    -- them again is no part of starting over. Each one handed counts as
    -- picked up, so the first goal stays ticked.
    S.sw = {}
    for _, k in ipairs({ "p7_sw1", "p7_sw2" }) do
      if hand_pill(S.player, P[k].n) then S.sw[k] = true end
    end
    if S.sw.p7_sw1 and S.sw.p7_sw2 then S.done[7].dead = true end
    if live_tank(S.bot7) ~= nil then
      game.teleport_to_start(S.bot7, ST.bot7.n)
      game.set_stocks(S.bot7, full_stock())
    end
    S.won = false
  end
  if player_tank() ~= nil then
    game.set_stocks(S.player, station_loadout(n))
  end
  say("reset", 2, n)
  redraw(true)
end

-- ---------------------------------------------------------------------
-- Setting the round up for the player.

local function hand_out()
  for _, k in ipairs({ "b2a", "b2b", "b2c", "b3", "b4c", "b5", "b6" }) do
    reset_base(k)
  end
  for _, k in ipairs({ "p4a", "p4", "t5a_target", "t5a_blocker", "t5b_target",
                       "t5b_blocker" }) do
    reset_pill(k)
  end
  for _, k in ipairs(PILLS7) do reset_pill(k) end
  for _, k in ipairs(BASES7) do reset_base(k) end
end

local function enter_station(n)
  if n > S.reached then S.reached = n end
  if n ~= S.at then
    S.at = n
    flush_popups(true)
  end
  if not S.entered[n] then
    S.entered[n] = true
    -- A full tank on the first visit (Station 2 starts low; Stations 1 to
    -- 3 carry no trees).
    if player_tank() ~= nil then
      game.set_stocks(S.player, station_loadout(n))
    end
    if n == 6 then
      field("bot6")
    elseif n == 7 then
      field("bot7")
    end
  end
  -- Every entry to Station 5: the 5B blocker in the tank, if not built.
  -- (The 5A demo bot is fielded by the poll, which sees the tank inside.)
  if n == 5 then hand_5b() end
  -- A pill the player carries out of its station goes home.
  for name, home in pairs(PILL_HOME) do
    if home ~= n then
      local pn = P[name].n
      local pb = game.pill(pn)
      if pb ~= nil and pb.in_tank and S.carrier[pn] == S.player then
        reset_pill(name)
      end
    end
  end
  local id = next_popup(n)
  if id ~= nil then show(id) end
  redraw(true)
end

-- Note what the player owns, once a second and each time he takes a base or
-- picks up a pillbox (see S.owned). The pillboxes of the stations' own
-- set-up are left out, so give_back never hands them to the next player:
-- the two 5A demo pillboxes always, and the 5B target until the player has
-- picked it up. The pillboxes a goal has the player take or build (4A, 4B,
-- the 5B blocker, Station 7's) are noted.
-- Nothing is noted once his seat is empty: by then the server has passed on
-- everything he owned. on_player_leave runs after that too, so a note taken
-- there would be an empty one; the last good one is kept.
local function note_owned()
  if S.player == nil or game.lobby_slot(S.player) == nil then return end
  local o = { pills = {}, bases = {} }
  for name, L in pairs(P) do
    local setup = name == "t5a_target" or name == "t5a_blocker" or
                  (name == "t5b_target" and not S.done[5].b_pick)
    local pb = game.pill(L.n)
    if not setup and pb ~= nil and pb.owner == S.player then
      o.pills[#o.pills + 1] = L.n
    end
  end
  for _, L in pairs(B) do
    local b = game.base(L.n)
    if b ~= nil and b.owner == S.player then o.bases[#o.bases + 1] = L.n end
  end
  table.sort(o.pills)
  table.sort(o.bases)
  S.owned = o
end

-- Give the new player what the last one owned. The server passed each of
-- those to an ally on the field (only the 5A demo bot can be one) or to
-- nobody; one that anybody else holds now is left alone.
local function give_back(p)
  local function lost(owner)
    return owner == game.NEUTRAL or (S.bot5a ~= nil and owner == S.bot5a)
  end
  for _, n in ipairs(S.owned.pills) do
    local pb = game.pill(n)
    if pb ~= nil and lost(pb.owner) then game.set_pill_owner(n, p) end
  end
  for _, n in ipairs(S.owned.bases) do
    local b = game.base(n)
    if b ~= nil and lost(b.owner) then game.set_base_owner(n, p) end
  end
end

-- An empty seat put by for a bot that is not on the field can be taken by a
-- person who joins later. The bot then gets another empty seat, if there is
-- one. The Station 6 bot's peace list leaves out its own seat and the
-- Station 7 bot's (bot_init), so when one of those two moves and the Station
-- 6 bot is in the round, it is handed the list again: without that it would
-- treat the person now on the old seat as an enemy.
local function move_bot_seat(p)
  local peace_moved = false
  for _, role in ipairs({ "bot5a", "bot6", "bot7" }) do
    if S[role] == p then
      if role ~= "bot5a" then peace_moved = true end
      S[role] = nil
      S.free_seat[p] = nil
      for q = 0, game.max_tanks() - 1 do
        if q ~= p and q ~= S.bot5a and q ~= S.bot6 and q ~= S.bot7 and
           game.lobby_slot(q) == nil then
          S[role] = q
          S.free_seat[q] = true
          break
        end
      end
      if S[role] == nil then
        game.log("Tutorial: no seat left for the " .. role .. " bot")
      end
    end
  end
  if peace_moved and S.fielded.bot6 and S.bot6 ~= nil then
    local ok, code = game.bot_init(S.bot6, bot_init("bot6"))
    if not ok then
      game.log("Tutorial: new peace list for the Station 6 bot refused: "
               .. tostring(code))
    end
  end
end

-- Set the tutorial up for seat p. The first player gets the round handed
-- out. A player who joins after the last one left (on_player_leave) takes
-- the round as it stands: the stations reached and the goals ticked are
-- kept, because the map was not put back, and he gets what the last player
-- owned. The popups start again, so he is shown the one for where he is.
local function adopt_player(p)
  if S.player ~= nil then return end
  if S.adopted then move_bot_seat(p) end
  S.player = p
  local s = game.lobby_slot(p)
  if TUTORIAL_PLAYER == nil and s ~= nil and s.team == 2 then
    game.set_team(p, 1)
  end
  if not S.adopted then
    S.adopted = true
    reserve_seats()
    hand_out()
  else
    give_back(p)
  end
  -- A seat with no tank yet spawns at the checkpoint (on_choose_start).
  local t = game.tank(p)
  local n = S.reached
  if t ~= nil then n = station_of(t.mx, t.my) or S.reached end
  S.at = n
  if n == 1 then show("welcome") end
  enter_station(n)
end

-- ---------------------------------------------------------------------
-- Hooks.

function on_start()
  for p = 0, game.max_tanks() - 1 do
    local s = game.lobby_slot(p)
    if s ~= nil and S.player == nil and is_player_seat(p) then
      adopt_player(p)
    end
  end
  -- A bot the tutorial did not ask for would play in the stations.
  for p = 0, game.max_tanks() - 1 do
    local s = game.lobby_slot(p)
    if s ~= nil and s.bot and s.fielded and p ~= S.player and p ~= S.bot5a
       and p ~= S.bot6 and p ~= S.bot7 and p ~= TUTORIAL_PLAYER then
      game.remove_bot(p)
    end
  end
end

function on_player_join(p, scripted)
  if scripted then return end
  if S.player == nil and is_player_seat(p) then adopt_player(p) end
end

-- The player left. His seat is forgotten, so the next person to join is set
-- up as the player (adopt_player), and everything kept for his screen and
-- his tank goes: the popups shown and waiting, the white lines waiting, the
-- panel, markers and status line last sent, the pillboxes he carried. The
-- stations and goals stay as they are. The 5A demo bot comes off the field
-- until a player is in Station 5 again. The seat is empty by now, so nothing
-- is sent to it.
function on_player_leave(p, scripted)
  if p == nil or p ~= S.player then return end
  S.player = nil
  for n, c in pairs(S.carrier) do
    if c == p then S.carrier[n] = nil end
  end
  S.popped, S.queue, S.says = {}, {}, {}
  S.say_until, S.status_gap_until = nil, nil
  S.panel_key, S.status, S.marker_key, S.markers = nil, nil, nil, 0
  S.last_pos, S.moving, S.hide, S.gun_remind = nil, false, 0, false
  S.b5b_dead_at, S.out5 = nil, 0
  unfield_bot5a()
end

function on_tank_spawned(p, mx, my, respawn, scripted)
  if p == S.bot6 and S.fielded.bot6 then
    park_bot6()
  elseif p == S.bot5a and S.fielded.bot5a then
    game.set_stocks(p, full_stock())
    hand_pill(p, P.t5a_blocker.n)
    rebuild_5a_rings()
    S.t5a_reset_at = S.sec
  elseif p == S.bot7 and S.fielded.bot7 then
    if not respawn then
      reset_base("b7_ne1"); reset_base("b7_ne2")
    end
  elseif p == S.player then
    S.last_pos = nil
    S.hide = 0
    -- A tank that has just spawned carries nothing. The pillboxes its last
    -- tank carried were dropped where it died, with no callback, so the
    -- carrier notes are cleared here; a stale one kept the 5B blocker from
    -- being handed over or rebuilt.
    for n, c in pairs(S.carrier) do
      if c == p then S.carrier[n] = nil end
    end
    if station_of(mx, my) == 5 then
      -- Every spawn in Station 5 has the blocker in the tank, built before
      -- or not, unless it stands alive on its own square.
      local L = P.t5b_blocker
      local bl = game.pill(L.n)
      if bl ~= nil and (bl.in_tank or bl.armour == 0 or bl.x ~= L.x or
                        bl.y ~= L.y) then
        hand_pill(p, L.n)
      end
    end
  end
end

function on_enter_region(p, name)
  if p ~= S.player then return end
  local n = string.match(name, "^s(%d)$")
  if n ~= nil then
    enter_station(tonumber(n))
    return
  end
  if name == "reset5b" then
    reset_station(5)
  elseif name == "reset7" then
    reset_station(7)
  elseif name == "s5b" then
    flush_popups(true)
    show("s5b")
  end
end

function on_tank_hit(victim, attacker, cause, amount, pill)
  if victim == S.player and pill == P.p4.n then S.hide = 0 end
end

function on_built(p, action, x, y, scripted)
  if p ~= S.player or not region_has("s3", x, y) then return end
  if action == "trees" then
    mark(3, "harvest")
  elseif action == "road" then
    mark(3, "road")
  elseif action == "building" then
    mark(3, "wall")
  end
end

function on_mine_laid(p, mx, my, scripted)
  if p == S.player and region_has("s3", mx, my) then mark(3, "mine") end
end

function on_lgm_died(p, killer, mx, my, scripted)
  -- Every new man lands two squares from his tank, not on a start across
  -- the map.
  game.builder_parachute(p, nil, nil, 2)
  if p == S.bot6 and S.player ~= nil and killer == S.player then
    S.men = S.men + 1
    mark(6, "kill")
    local t = player_tank()
    if t ~= nil and t.sight ~= nil and t.sight < rule("gunsight_max") then
      S.gun_remind = true
      popup_later("s6gun")
    end
    redraw(false)
  end
end

function on_pill_picked_up(n, p, scripted)
  S.carrier[n] = p
  if scripted then return end
  local name = PILL_NAME[n]
  if p == S.bot5a then
    if name == "t5a_target" then
      -- The demo take is done: put the island back and go again. It counts
      -- as watched only when the player's tank is on the watching spur,
      -- where the whole island is on screen.
      local t = player_tank()
      if t ~= nil and S.at == 5 and region_has("watch5a", t.mx, t.my) then
        mark(5, "a_watch")
      end
      reset_5a()
    end
    return
  end
  if p ~= S.player then return end
  if name == "p4a" then
    mark(4, "a_pick")
  elseif name == "p4" then
    mark(4, "c_pick")
  elseif name == "t5b_target" then
    mark(5, "b_pick")
    if not S.t5b_restore then
      S.t5b_restore = true
      -- 5 seconds, not 3: picking it up usually finishes Station 5, and the
      -- "Station 5 done" line is up for 4 seconds. This line comes after
      -- it, not on top of it.
      game.timer(5, function()
        S.t5b_restore = false
        if reset_pill("t5b_target") then
          say("back", 4)
        end
      end)
    end
  elseif name == "p7_sw1" or name == "p7_sw2" then
    -- Each counts once picked up, so building the first before picking up
    -- the second still ticks the item.
    S.sw[name] = true
    if S.sw.p7_sw1 and S.sw.p7_sw2 then mark(7, "dead") end
    redraw(false)
  end
  -- Noted now, not at the next whole second: he may leave before it.
  note_owned()
end

function on_pill_placed(n, p, armour, scripted)
  S.carrier[n] = nil
  if scripted then return end
  local name = PILL_NAME[n]
  local pb = game.pill(n)
  if name ~= nil and pb ~= nil then
    local home = PILL_HOME[name]
    if home ~= nil and station_of(pb.x, pb.y) ~= home then
      reset_pill(name)
      return
    end
    -- The player's 5B blocker belongs on the square beside the target that
    -- the parking square is worked out for.
    if name == "t5b_blocker" then
      local L = P[name]
      if pb.x ~= L.x or pb.y ~= L.y then
        game.move_pill(n, L.x, L.y)
        if p == S.player then
          say("moved", 3)
        end
      end
      if armour == 0 then game.set_pill_armour(n, rule("pill_max_armour")) end
    end
  end
  if p ~= S.player or pb == nil then return end
  if name == "p4a" then mark(4, "a_build") end
  if name == "t5b_blocker" then mark(5, "b_build") end
  if region_has("quarter_ne", pb.x, pb.y) then mark(7, "build") end
end

function on_pill_killed(n, by, scripted)
  if n ~= P.t5b_blocker.n or S.player == nil then return end
  local d = S.done[5]
  if not d.b_build or d.b_pick then return end
  S.b5b_dead_at = nil
  S.b5b_timer = true
  game.timer(2, function()
    S.b5b_timer = false
    rebuild_5b()
  end)
end

local function check_win()
  if S.won or S.player == nil then return end
  for _, k in ipairs(BASES7) do
    local b = game.base(B[k].n)
    if b == nil or b.owner ~= S.player then return end
  end
  -- The round does not end: the player reads the popup and leaves from the
  -- menu. Ending it would put the round-end screen over the popup.
  S.won = true
  mark(7, "all")
  redraw(false)
  -- In a single-player game this hides the main menu's Tutorial button.
  game.tutorial_done(S.player)
end

function on_base_captured(n, old, new, scripted)
  if scripted or new ~= S.player then return end
  if n == B.b2b.n then
    mark(2, "b_take")
  elseif n == B.b2c.n then
    mark(2, "c_shoot")
    mark(2, "c_take")
  elseif S.bot7 ~= nil and old == S.bot7 then
    mark(7, "base")
  end
  check_win()
  note_owned()     -- as in on_pill_picked_up
end

-- ---------------------------------------------------------------------
-- Policies.

function can_build(p, action, x, y, n)
  if p == nil then return nil end
  if p == S.bot6 then
    return action == "trees" and x == PT.tree6[1] and y == PT.tree6[2]
  elseif p == S.bot5a then
    return action ~= "boat" and region_has("island5a", x, y)
  elseif p == S.bot7 then
    return action ~= "boat" and region_has("island7", x, y)
  elseif p == S.player and action == "boat" and region_has("s5", x, y) then
    return false
  end
  return nil
end

-- The 5A bot is the player's ally: without this it would pick up the
-- player's dead pillboxes and take the 5B pillbox. The Station 7 bot may
-- drive at the two dead pillboxes on the road in (its route finder does not
-- know this rule), but it cannot take them; the player drives over them on
-- the way in, long before the bot gets there.
function can_capture(kind, n, p)
  if p == nil then return nil end
  if p == S.bot7 then
    -- The two dead pillboxes on the Station 7 road in are the player's.
    if kind == "pill" and (n == P.p7_sw1.n or n == P.p7_sw2.n) then
      return false
    end
    return nil
  end
  if p ~= S.bot5a then return nil end
  return kind == "pill" and (n == P.t5a_target.n or n == P.t5a_blocker.n)
end

-- No newswire line for a base or a pillbox changing hands. The script hands
-- the player a dozen bases and pillboxes as the round starts, and gives a
-- station's bases and pillboxes back whenever it puts a station right, and
-- each of those would read as a capture. The player can see what they take
-- and on_base_captured still ticks the goals off.
function announce(kind, subject, actor)
  if kind == "base_captured" or kind == "pill_captured" then return false end
  return nil
end

-- Tank shells pass between the player and the two demo bots (shells hit
-- allied tanks too, so the allied 5A bot is in the list). Two Station 5
-- rules more: the player's shells pass the player's own 5B blocker, and the
-- 5A pillboxes' shells pass the player. Every other pillbox shoots as it
-- always does, so the Station 6 bot's pillbox in Station 5B is a real enemy
-- pillbox. The target's shells take their full armour off the player's 5B
-- blocker: there is no damage rule, so the blocker wears down and dies as
-- in a real game, and the announced rebuild is the only thing that puts
-- armour back. (A rule that let one target shell in five count made the
-- blocker look as if it repaired itself without a word, and was taken out.)

function can_hit(attacker, kind, n, pill)
  if S.player == nil then return nil end
  if kind == "pill" then
    -- 5B: the player's shells pass the player's own blocker, so a tank
    -- parked straight behind it can hit the target. A real game has no such
    -- rule. From the parking square (121,107) the target's centre is 7.21
    -- squares off and a shell flies 7.125, so a shot at the centre ends in
    -- the target's square; but the line clips the blocker's square
    -- (117,102) by about 0.3 of a square, and without this the blocker
    -- would take the player's shots. The firing client still draws the
    -- shell stopping at the blocker; the server lets it through.
    if pill == nil and attacker == S.player and n == P.t5b_blocker.n then
      return false
    end
    return nil
  end
  if kind ~= "tank" then return nil end
  if pill ~= nil then
    -- 5A: the demo's pillboxes never hurt the player, who watches from
    -- just outside their range and may wander closer.
    if n == S.player and (pill == P.t5a_target.n or pill == P.t5a_blocker.n) then
      return false
    end
    return nil
  end
  if (attacker == S.player and is_demo_bot(n)) or
     (n == S.player and is_demo_bot(attacker)) then
    return false
  end
  return nil
end

-- 5A: the demo's two pillboxes never pick the player as their target, so
-- they do not turn to him, fire at him or wait for him; their fire is for
-- the demo bot alone. can_hit above stays as the second guard, for a shell
-- aimed at the bot that reaches the player. The 5B target is not in the
-- list: there the player is meant to be shot at.

function can_target(pill, p)
  if p == S.player and S.player ~= nil and
     (pill == P.t5a_target.n or pill == P.t5a_blocker.n) then
    return false
  end
  return nil
end

function can_die(kind, n, killer, cause, pill)
  if kind == "tank" and n == S.bot6 and S.bot6 ~= nil then return false end
  return nil
end

function on_choose_start(p)
  if p == nil then return nil end
  if p == S.bot5a then return ST.bot5a.n end
  if p == S.bot6 then return ST.bot6.n end
  if p == S.bot7 then return ST.bot7.n end
  if p == S.player or (S.player == nil and is_player_seat(p)) then
    return ST["cp" .. S.reached].n
  end
  return nil
end

function spawn_loadout(p)
  if p == S.player or (S.player == nil and is_player_seat(p)) then
    return station_loadout(S.reached)
  end
  return "open"
end

function allow_base_win()
  return false
end

-- ---------------------------------------------------------------------
-- The poll: five times a second.

local TERRAIN_ITEM = { forest = "forest", swamp = "swamp", rubble = "rubble",
                       crater = "crater", river = "river" }

local function hidden_in_trees(t, cx, cy)
  local function forest(x, y)
    return game.map_tile(x, y) == game.TERRAIN.forest
  end
  -- The engine's test: the tank's square, its neighbours toward the side
  -- of the square it sits in, and the diagonal between them, all forest;
  -- and 3 squares or more from the one looking along either axis.
  local sx = (t.wx % 256) >= 128 and 1 or -1
  local sy = (t.wy % 256) >= 128 and 1 or -1
  if not (forest(t.mx, t.my) and forest(t.mx + sx, t.my) and
          forest(t.mx, t.my + sy) and forest(t.mx + sx, t.my + sy)) then
    return false
  end
  local far = rule("tree_hide_distance")
  return math.abs(t.wx - cx) >= far or math.abs(t.wy - cy) >= far
end

local function poll()
  local t = player_tank()
  if t == nil then
    draw_status()
    return
  end
  local n = S.at
  -- The 5A demo bot: on the field only while the tank is in Station 5. A
  -- second outside (5 polls) takes it off, so a tank on the line does not
  -- field and remove it over and over.
  if station_of(t.mx, t.my) == 5 then
    S.out5 = 0
    if not S.fielded.bot5a then field("bot5a") end
  elseif S.fielded.bot5a then
    S.out5 = S.out5 + 1
    if S.out5 >= 5 then unfield_bot5a() end
  end
  if S.last_pos ~= nil then
    S.moving = t.wx ~= S.last_pos[1] or t.wy ~= S.last_pos[2]
  end
  S.last_pos = { t.wx, t.wy }
  if S.gun_remind and t.sight ~= nil and t.sight >= rule("gunsight_max") then
    S.gun_remind = false
  end
  if n == 1 then
    local word = terrain_at(t.mx, t.my)
    local item = TERRAIN_ITEM[word or ""]
    if item ~= nil and not t.boat then mark(1, item) end
  elseif n == 2 then
    -- Arriving on the base says to stay there. 2A is told, not enforced:
    -- it ticks when the tank is full on the base, when the tank drives off
    -- the base, or when any later Station 2 goal ticks, so it never holds
    -- anything up. It still has to tick: the status line and the marker
    -- name the first goal not ticked, so a goal that never ticked would
    -- keep "STOP and wait" up while the player took 2B.
    if not S.done[2].a_refill then
      local d = S.done[2]
      if t.mx == B.b2a.x and t.my == B.b2a.y then
        show("s2stay")
        S.on_2a = true
        if tank_full(t) then mark(2, "a_refill") end
      elseif S.on_2a or d.b_take or d.c_shoot or d.c_take then
        mark(2, "a_refill")
      end
    end
    local b = game.base(B.b2c.n)
    if b ~= nil and b.owner ~= S.player and
       b.armour <= rule("base_capture_armour") then
      mark(2, "c_shoot")
    end
  elseif n == 3 then
    local w = PT.wall3
    local left = game.wall_shots(w[1], w[2])
    if left ~= nil and left <= 0 then mark(3, "shoot") end
  elseif n == 4 then
    local pb = game.pill(P.p4.n)
    if pb ~= nil and not pb.in_tank and pb.armour <= 0 then
      -- Dead. Killed before the hide too: the hide goals are passed over,
      -- not ticked, and the station goes on to the pick-up: the player can
      -- still come back and do them.
      if not S.done[4].b_in then S.passed[4].b_in = true end
      if not S.done[4].b_hide then S.passed[4].b_hide = true end
      mark(4, "c_kill")
    end
    if not S.done[4].b_hide then
      -- The hide is measured from the pillbox's own square, which is where
      -- it stands while it is alive, so a player who killed it first can
      -- still come back and do the hide.
      local cx = P.p4.x * 256 + 128
      local cy = P.p4.y * 256 + 128
      local d = math.sqrt((t.wx - cx) ^ 2 + (t.wy - cy) ^ 2)
      local inr = d <= rule("pill_range")
      if inr and terrain_at(t.mx, t.my) == "forest" then mark(4, "b_in") end
      if inr and hidden_in_trees(t, cx, cy) then
        S.hide = S.hide + 0.2
        if S.hide >= 5 and S.done[4].b_in then mark(4, "b_hide") end
      else
        S.hide = 0
      end
    end
  elseif n == 5 then
    -- 5A is told, not forced: the take seen from the watch spot ticks it
    -- (on_pill_picked_up), and so does WATCH_NEAR_FRAMES after the tank
    -- first came within WATCH_NEAR squares of the spot, wherever it is
    -- then. The timer is not reset by leaving.
    if not S.done[5].a_watch then
      local w = PT.watch5a
      if math.max(math.abs(t.mx - w[1]), math.abs(t.my - w[2])) <=
         WATCH_NEAR then
        S.near5a_at = S.near5a_at or S.frames
      end
      if S.near5a_at ~= nil and S.frames - S.near5a_at >= WATCH_NEAR_FRAMES
         then
        mark(5, "a_watch")
      end
    end
    if t.mx == PT.t5b_park[1] and t.my == PT.t5b_park[2] then
      mark(5, "b_park")
    end
    local pb = game.pill(P.t5b_target.n)
    if pb ~= nil and not pb.in_tank and pb.armour == 0 then
      mark(5, "b_kill")
    end
  elseif n == 7 then
    check_win()
  end
  redraw(false)
end

-- Once a second: the bots' loops and the re-arming.
local function every_second()
  S.sec = S.sec + 1
  if S.player ~= nil then
    supply_bases()
    rearm(S.at)
    note_owned()
  end
  if S.fielded.bot6 then bot6_loop() end
  -- 5A: the bot stays on its island, and a take that stalls starts again.
  -- The bot builds its blocker itself, on a square orthogonally next to the
  -- target (BLOCKER_ORTHOGONAL_ONLY, set in bot_init).
  local t5 = live_tank(S.bot5a)
  if t5 ~= nil then
    if not region_has("island5a", t5.mx, t5.my) then home_bot5a() end
    if S.sec - S.t5a_reset_at >= 150 then reset_5a() end
    -- The island has no base. GoalHunter will not drive at a pillbox with
    -- under 30 armour while it carries one, so a bot worn down by the
    -- target's shells would sit until the 150-second reset, and the target
    -- kills it in the end. Its stocks are topped up instead.
    if (t5.armour or 0) < 30 or (t5.shells or 0) < 10 then
      game.set_stocks(S.bot5a, full_stock())
    end
  end
end

function on_tick(tick)
  S.frames = S.frames + 1
  -- A 5A ring rebuild the frame's write count cut short goes on first.
  if S.rings5a_again then rebuild_5a_rings() end
  if S.fielded.bot6 then hold_bot6() end
  -- 7: the bot stays on its island. Checked every frame: the island has no
  -- moat or gate, and a bot on the road is out within a few frames.
  if S.fielded.bot7 then
    local t7 = live_tank(S.bot7)
    if t7 ~= nil and not region_has("island7", t7.mx, t7.my) then
      game.teleport_to_start(S.bot7, ST.bot7.n)
    end
  end
  flush_popups(false)
  if S.frames % 10 == 0 then poll() end
  if S.frames % 50 == 0 then every_second() end
end

-- ===== Translations =====
-- Each language's text, in the shape of TEXT.en. A key a language
-- leaves out is the English (tr). Plain words with no plural forms
-- ("Shots left: 3") are used where English has a plural function.

-- Czech
TEXT["cs"] = {
  pop = {
    s1 = {
      keyboard = "Stiskni {ACCEL} pro jízdu vpřed, {BRAKE} pro zpomalení, {LEFT} a {RIGHT} pro zatáčení.\n\nPolíčka vedle silnice jsou les, bažina, trosky, krátery a mělká řeka. Každé tě zpomalí jinak. Zkus po několika z nich projet.\n\nHluboká voda tanky potopí. Když tu kdekoli zemřeš, vrátíš se na poslední kontrolní bod.",
      touch = "Řiď páčkou na levé straně obrazovky: posuň ji směrem, kterým chceš jet. Čím dál ji posuneš, tím rychleji jedeš.\n\nPolíčka vedle silnice jsou les, bažina, trosky, krátery a mělká řeka. Každé tě zpomalí jinak. Zkus po několika z nich projet.\n\nHluboká voda tanky potopí. Když tu kdekoli zemřeš, vrátíš se na poslední kontrolní bod.",
    },
    s2a = "Základny doplňují granáty, miny a pancíř, a tvůj tank má zásob málo.\n\nNajeď na svou základnu u zelené značky. Pak na základně ZASTAV a počkej, než se doplní.",
    s2b = "Neutrální základny nemají červenou ani zelenou barvu. Získáš je tím, že přes ně přejedeš.",
    s2c = "Nepřátelské základny musíš nejdřív střelbou vyprázdnit. Plná vydrží %d výstřelů: zamiř na ni a stiskni {FIRE} pro střelbu. Pak přes ni přejeď a bude tvoje.",
    s2d = "To, s čím se tvůj tank objeví, závisí na typu hry.\n\nOtevřená: tanky se vždy objeví plně vybavené.\n\nTurnaj: čím více základen je obsazeno, tím méně granátů tank dostane. Když jsou obsazeny všechny základny, tanky se objevují bez granátů.\n\nStriktní turnaj: tanky se nikdy neobjevují s granáty.\n\nTurnajové režimy mají jasnou podmínku vítězství: jakmile jeden tým vlastní všechny základny, vyhrává, protože nepřátelský tým nemůže získat žádnou munici.",
    s2stay = "Toto je tvoje základna. Zůstaň na ní a nehýbej se: granáty, miny a pancíř se doplňují postupně.",
    s3 = {
      keyboard = "Tvůj muž kácí stromy a pak z nich staví silnice, zdi, miny a bunkry. Klikni na tlačítko stavby, nebo stiskni jeho klávesu: {QUICK_TREE} stromy, {QUICK_ROAD} silnice, {QUICK_WALL} zeď, {QUICK_MINE} mina. Pak klikni na mapu a pošli tam svého muže.\n\nTip: když máš vybraný nástroj silnice, zdi nebo bunkru, klikni na strom a tvůj muž ho pokácí. Nemusíš pokaždé přepínat na nástroj stromů.",
      touch = "Tvůj muž kácí stromy a pak z nich staví silnice, zdi, miny a bunkry. Klepni na tlačítko stavby: stromy, silnice, zeď nebo mina. Pak klepni na mapu a pošli tam svého muže.\n\nTip: když máš vybraný nástroj silnice, zdi nebo bunkru, klepni na strom a tvůj muž ho pokácí. Nemusíš pokaždé přepínat na nástroj stromů.",
      controller = "Tvůj muž kácí stromy a pak z nich staví silnice, zdi, miny a bunkry. Stiskni {BUILD_MODE} pro kurzor stavby a namiř ho pravou páčkou. Stiskni {BUILD_TOOL} pro výběr nástroje: stromy, silnice, zeď nebo mina. Pak stiskni {BUILD_PLACE} a pošli muže ke kurzoru.\n\nTip: když máš vybraný nástroj silnice, zdi nebo bunkru, dej kurzor stavby na strom a stiskni {BUILD_PLACE}. Tvůj muž ho pokácí. Nemusíš pokaždé přepínat na nástroj stromů.",
    },
    s4a = {
      keyboard = "Hry začínají s neutrálními bunkry a plný bunkr zničí až 15 výstřelů.\n\nNejdřív přejeď přes zničený bunkr, abys ho naložil do tanku. Pak stiskni tlačítko stavby bunkru nebo jeho klávesu {QUICK_PILL}. Pak klikni na mapu a pošli muže, aby na tom místě postavil bunkr věrný tobě.",
      touch = "Hry začínají s neutrálními bunkry a plný bunkr zničí až 15 výstřelů.\n\nNejdřív přejeď přes zničený bunkr, abys ho naložil do tanku. Pak klepni na tlačítko stavby bunkru a klepni na mapu. Tvůj muž vyrazí a postaví na tom místě bunkr věrný tobě.",
      controller = "Hry začínají s neutrálními bunkry a plný bunkr zničí až 15 výstřelů.\n\nNejdřív přejeď přes zničený bunkr, abys ho naložil do tanku. Pak tiskni {BUILD_TOOL}, dokud nevybereš nástroj bunkru, dej kurzor stavby, kam chceš, a stiskni {BUILD_PLACE}. Tvůj muž vyrazí a postaví tam bunkr věrný tobě.",
    },
    s4b = "Východně od silnice stojí v kruhu lesa poškozený bunkr. Pořád střílí, a bunkr, který zasáhneš, se rozzlobí a střílí rychleji. Červené značky ukazují jeho dosah.\n\nLesy skrývají tanky před ostatními tanky i před bunkry. Vjeď do lesa uvnitř značek a stůj klidně. Střelba prozradí tvou polohu.",
    s4c = "Tento bunkr je už těžce poškozený: zničí ho dva další výstřely.\n\nTvůj tank je plný. Znič ho a pak přes něj přejeď, abys ho sebral.",
    s5a = "Zastav na zeleném čtverci před sebou. Sleduj, jak přátelský bot na ostrově obehnaném zdí obsadí bunkr. Postaví vlastní bunkr hned vedle nepřátelského a zaparkuje za ním, takže nepřátelské střely zasahují jeho bunkr, ne jeho tank. Pak nepřátelský bunkr zničí a přejede přes něj.",
    s5b = "Jsi na řadě: vezeš bunkr. Postav svůj bunkr na označeném zeleném čtverci, hned vedle jejich. Pak zastav na zvýrazněném políčku silnice a zamiř na nepřátelský bunkr: střílej, dokud nepadne. Tvůj bunkr schytá jeho střely, a když padne, je postaven znovu. Pro nový začátek vjeď do ohrady RESET.\n\nZačni s plným pancířem. Je normální, že tank utrpí nějaké poškození poté, co nepřátelský bunkr prostřílí blokující bunkr.",
    s6 = "Zabij nepřátelského muže a jeho tank nemůže stavět, dokud nedorazí nový muž. Muže nemůžeš přejet: zabijí ho jen granáty, miny a výbuchy.\n\nGranát vybuchne v místě zaměřovače a zabije muže do vzdálenosti půl políčka. Stiskni {GUN_DOWN} pro přiblížení zaměřovače a {GUN_UP} pro jeho oddálení. V tomto výukovém kurzu je bot naprogramován tak, aby poslal muže přes krátery. Umísti tam zaměřovač a střílej, když je muž přechází.",
    s6gun = "Máš ho! Teď tiskni {GUN_UP}, dokud zaměřovač nebude zase na největším dosahu, připravený na další stanoviště.",
    s7 = "Poslední kolo, proti lehkému botovi. Držíš tři rohy ostrova a bot drží severovýchod. Zaber jeho území svými bunkry a pak mu ukradni základny, jako na Stanovišti 2. Vyhraješ, až budeš vlastnit všechny základny. Pro nový začátek vjeď do ohrady RESET.\n\nNejdřív přejeď přes dva zničené bunkry na příjezdové silnici, abys je naložil do tanku. Postav své bunkry blízko hranice mezi tvým a jeho územím a převezmi kontrolu.",
    welcome = "Vítej ve WinBolo! Bolo je hra s tanky pro více hráčů, kterou Stuart Cheshire poprvé napsal v roce 1987, a WinBolo v ní dnes pokračuje. Jezdíš tankem po ostrově plném bunkrů, tedy dělových věží, které střílejí na nepřátelské tanky, a základen, které doplňují granáty, miny a pancíř. V tvém tanku jede malý muž, který vystupuje, aby pro tebe kácel stromy a stavěl. Hraje se v týmech, a tým, který drží bunkry a základny, ovládá ostrov.",
    win = "Výborně: vlastníš všechny základny a výukový kurz je dokončen.\n\nJezdi, jak dlouho chceš. Až budeš připraven, odejdi přes menu a pak zkus hru proti lehkým botům.",
  },
  title = {
    [1] = "Stanoviště 1: Jízda",
    [2] = "Stanoviště 2: Základny",
    [3] = "Stanoviště 3: Stavění",
    [4] = "Stanoviště 4: Bunkry",
    [5] = "Stanoviště 5: Zisk bunkru",
    [6] = "Stanoviště 6: Muž",
    [7] = "Stanoviště 7: Finále",
  },
  list = {
    a_build = "Postav ho",
    a_pick = "Seber zničený bunkr",
    a_refill = "Doplň na své základně",
    a_watch = "Sleduj bota v akci",
    all = "Zaber všechny základny",
    b_build = "Stav na zelený čtverec",
    b_hide = "Skryj se v dosahu",
    b_in = "Vjeď do lesa v dosahu",
    b_kill = "Znič nepřátelský bunkr",
    b_park = "Zastav na políčku silnice",
    b_pick = "Seber ho",
    b_take = "Obsaď neutrální základnu",
    base = "Ukradni cizí základnu",
    build = "Bunkr v cizím území",
    c_kill = "Znič bunkr",
    c_pick = "Seber bunkr",
    c_shoot = "Vystřílej cizí základnu",
    c_take = "Přejeď přes ni",
    crater = "Krátery",
    dead = "Seber oba zničené bunkry",
    forest = "Les",
    harvest = "Pokácej strom",
    kill = "Zastřel muže",
    mine = "Polož minu",
    river = "Mělká řeka",
    road = "Postav silnici",
    rubble = "Trosky",
    shoot = "Sestřel zeď",
    swamp = "Bažina",
    wall = "Postav zeď",
  },
  step = {
    wall = {
      keyboard = "Postav zeď: klikni na tlačítko zdi\nnebo stiskni {QUICK_WALL}, pak klikni na trávu.",
      touch = "Postav zeď: klepni na tlačítko zdi,\npak klepni na políčko trávy.",
      controller = "Postav zeď: stiskni {BUILD_TOOL} pro nástroj zdi,\ndej kurzor na trávu a stiskni {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Klikni na tlačítko silnice nebo stiskni\n{QUICK_ROAD}, pak klikni na políčko trávy.",
      touch = "Klepni na tlačítko silnice, pak klepni na políčko trávy.",
      controller = "Stiskni {BUILD_TOOL} pro nástroj silnice, dej kurzor\nna trávu a stiskni {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Klikni na tlačítko miny nebo stiskni {QUICK_MINE},\npak klikni na políčko a polož minu.",
      touch = "Klepni na tlačítko miny, pak klepni\nna políčko a polož minu.",
      controller = "Stiskni {BUILD_TOOL} pro nástroj miny, dej kurzor\nna políčko a stiskni {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Klikni na tlačítko stromů nebo stiskni {QUICK_TREE},\npak klikni na lesní políčko v označeném háji.",
      touch = "Klepni na tlačítko stromů, pak klepni\nna lesní políčko v označeném háji.",
      controller = "Stiskni {BUILD_TOOL} pro nástroj stromů, dej kurzor\nna označený háj a stiskni {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Stiskni tlačítko bunkru nebo {QUICK_PILL},\npak klikni na mapu a postav ho.",
      touch = "Klepni na tlačítko bunkru,\npak klepni na mapu a postav ho.",
      controller = "Stiskni {BUILD_TOOL} pro nástroj bunkru, umísti\nkurzor a stiskni {BUILD_PLACE} pro stavbu.",
    },
    a_pick = "Přejeď přes zničený bunkr a nalož ho do tanku.",
    a_refill = "Najeď na základnu se zelenou značkou, pak na ní\nZASTAV a počkej, než se doplní.",
    a_watch = "Zastav na zeleném čtverci před sebou\na sleduj, jak bot obsadí bunkr.",
    all = "Obsaď všechny cizí základny: tvých je %d z 8.",
    b_build = {
      keyboard = "Stiskni tlačítko bunkru nebo {QUICK_PILL},\npak klikni na zelený čtverec.",
      touch = "Klepni na tlačítko bunkru,\npak klepni na zelený čtverec.",
      controller = "Stiskni {BUILD_TOOL} pro nástroj bunkru, dej kurzor\nna zelený čtverec a stiskni {BUILD_PLACE}.",
    },
    b_hide = "Stůj klidně hluboko v lese, v dosahu.\nSkrytý: %d z 5 sekund.",
    b_in = "Vjeď do lesa uvnitř červených značek.",
    b_kill = "Tiskni {FIRE} a střílej do nepřátelského\nbunkru, dokud nepadne.",
    b_park = "Zastav na zvýrazněném políčku silnice\na zamiř na nepřátelský bunkr.",
    b_pick = "Přejeď přes zničený bunkr a seber ho.",
    b_take = "Najeď na neutrální základnu a bude tvoje.",
    base = "Tiskni {FIRE} a vystřílej cizí základnu,\npak na ni najeď.",
    build = "Postav bunkry blízko hranice, v území bota\n(severovýchod), a převezmi kontrolu.",
    c_kill = "Stiskni {FIRE} a zasáhni bunkr dvakrát.",
    c_pick = "Přejeď přes zničený bunkr a seber ho.",
    c_shoot = "Tiskni {FIRE} a střílej do červeně\noznačené základny. Zbývá výstřelů: %d.",
    c_take = "Základna je prázdná. Najeď na ni a bude tvoje.",
    dead = "Přejeď přes oba zničené bunkry\nna příjezdové silnici.",
    kill = "Umísti zaměřovač na krátery a pak\nstřílej, když je muž přechází.",
    shoot = "Tiskni {FIRE} a střílej do označené zdi,\ndokud nepadne.",
  },
  line = {
    done = "Stanoviště splněno. Jeď po silnici na sever.",
    gun = "Tiskni {GUN_UP}, dokud zaměřovač\nnebude na největším dosahu.",
    shells = "Málo granátů? Zastav na základně\nu silnice a doplň je.",
    win = "Výukový kurz dokončen. Až budeš\npřipraven, odejdi přes menu.",
  },
  info = {
    bases = "Tvé základny: %d/8",
    enemy_base = "Cizí základna: zbývá %d",
    men = "Zabití muži: %d",
    needed = "%d z %d potřebných",
    on = "%s: rychlost %d",
    on_plain = "%s",
    pill_armour = "Pancíř bunkru: %d/%d",
    wall_down = "Cílová zeď: zničena",
    wall_left = "Cílová zeď: zbývá %d",
  },
  hint = {
    full = "Plno",
    low = "Málo granátů: doplň",
    refill = "Doplňuje se: čekej",
    reset = "Ohrada RESET: znovu",
    stop = "ZASTAV na základně: čekej",
  },
  terrain = {
    boat = "Loď",
    building = "Budova",
    crater = "Kráter",
    ["deep sea"] = "Širé moře",
    forest = "Les",
    grass = "Tráva",
    river = "Řeka",
    road = "Silnice",
    rubble = "Trosky",
    swamp = "Bažina",
    wall = "Zeď",
  },
  say = {
    back = "Nepřátelský bunkr je zpět.\nObsaď ho znovu, nebo jeď na sever.",
    done = "Stanoviště %d splněno. Jeď po silnici na sever.",
    done4 = "Stanoviště %d splněno. Jeď po silnici na sever.\nStřílej do stromů, rychle je odstraníš.",
    moved = "Přesunuto na zelený čtverec.",
    reset = "Stanoviště %d začíná znovu",
  },
}

-- German
TEXT["de"] = {
  pop = {
    s1 = {
      keyboard = "Drücken Sie {ACCEL}, um vorwärts zu fahren, {BRAKE}, um zu bremsen, und {LEFT} oder {RIGHT}, um zu lenken.\n\nDie Flächen neben der Straße sind Wald, Sumpf, Trümmer, Krater und ein seichter Fluss. Jede bremst Sie unterschiedlich stark. Fahren Sie über einige davon.\n\nTiefes Wasser versenkt Panzer. Wenn Sie hier sterben, kehren Sie zu Ihrem letzten Kontrollpunkt zurück.",
      touch = "Fahren Sie mit dem Joystick links auf dem Bildschirm: Schieben Sie ihn in die gewünschte Richtung. Je weiter Sie ihn schieben, desto schneller fahren Sie.\n\nDie Flächen neben der Straße sind Wald, Sumpf, Trümmer, Krater und ein seichter Fluss. Jede bremst Sie unterschiedlich stark. Fahren Sie über einige davon.\n\nTiefes Wasser versenkt Panzer. Wenn Sie hier sterben, kehren Sie zu Ihrem letzten Kontrollpunkt zurück.",
    },
    s2a = "Basen füllen Ihre Granaten, Minen und Panzerung auf, und Ihr Panzer hat nur noch wenig davon.\n\nFahren Sie an der grünen Markierung auf Ihre Basis. Dann STOPP: Bleiben Sie auf der Basis stehen und warten Sie, bis alles aufgefüllt ist.",
    s2b = "Neutrale Basen sind weder rot noch grün. Sie gehören Ihnen, wenn Sie über sie fahren.",
    s2c = "Feindliche Basen müssen Sie erst leer schießen. Eine volle Basis braucht %d Schüsse: Zielen Sie darauf und drücken Sie {FIRE}, um zu schießen. Fahren Sie dann darüber, um sie zu übernehmen.",
    s2d = "Womit Ihr Panzer startet, hängt vom Spieltyp ab.\n\nOffen: Panzer starten immer voll beladen.\n\nTurnier: Je mehr Basen erobert sind, desto weniger Granaten hat ein neuer Panzer. Sind alle Basen erobert, starten Panzer ohne Granaten.\n\nStriktes Turnier: Panzer starten nie mit Granaten.\n\nTurniermodi haben ein klares Siegziel: Sobald ein Team alle Basen besitzt, gewinnt es, denn das gegnerische Team bekommt keine Munition mehr.",
    s2stay = "Das ist Ihre Basis. Bleiben Sie darauf stehen und fahren Sie nicht weiter: Granaten, Minen und Panzerung kommen nach und nach hinzu.",
    s3 = {
      keyboard = "Ihr Mann fällt Bäume und baut damit Straßen, Mauern, Minen und Bunker. Klicken Sie auf einen Bauknopf oder drücken Sie seine Taste: {QUICK_TREE} Bäume, {QUICK_ROAD} Straße, {QUICK_WALL} Mauer, {QUICK_MINE} Mine. Klicken Sie dann auf die Karte, um Ihren Mann dorthin zu schicken.\n\nTipp: Ist das Straßen-, Mauer- oder Bunkerwerkzeug gewählt, klicken Sie auf einen Baum: Ihr Mann fällt ihn. Ein Wechsel zum Baumwerkzeug ist nicht nötig.",
      touch = "Ihr Mann fällt Bäume und baut damit Straßen, Mauern, Minen und Bunker. Tippen Sie auf einen Bauknopf: Bäume, Straße, Mauer oder Mine. Tippen Sie dann auf die Karte, um Ihren Mann dorthin zu schicken.\n\nTipp: Ist das Straßen-, Mauer- oder Bunkerwerkzeug gewählt, tippen Sie auf einen Baum: Ihr Mann fällt ihn. Ein Wechsel zum Baumwerkzeug ist nicht nötig.",
      controller = "Ihr Mann fällt Bäume und baut damit Straßen, Mauern, Minen und Bunker. Ziehen Sie {BUILD_MODE} für den Baucursor und zielen Sie mit dem rechten Stick. Drücken Sie {BUILD_TOOL} für ein Werkzeug: Bäume, Straße, Mauer oder Mine. Drücken Sie dann {BUILD_PLACE}, um Ihren Mann zum Cursor zu schicken.\n\nTipp: Ist das Straßen-, Mauer- oder Bunkerwerkzeug gewählt, zielen Sie auf einen Baum und drücken Sie {BUILD_PLACE}: Ihr Mann fällt ihn. Ein Wechsel zum Baumwerkzeug ist nicht nötig.",
    },
    s4a = {
      keyboard = "Spiele beginnen mit neutralen Bunkern. Ein voller Bunker braucht 15 Treffer, bis er zerstört ist.\n\nFahren Sie zuerst über den zerstörten Bunker, um ihn in Ihren Panzer zu laden. Drücken Sie dann den Bunker-Bauknopf oder seine Taste {QUICK_PILL}. Klicken Sie dann auf die Karte. Ihr Mann geht los und baut dort einen Bunker, der Ihnen treu ist.",
      touch = "Spiele beginnen mit neutralen Bunkern. Ein voller Bunker braucht 15 Treffer, bis er zerstört ist.\n\nFahren Sie zuerst über den zerstörten Bunker, um ihn in Ihren Panzer zu laden. Tippen Sie dann auf den Bunker-Bauknopf und tippen Sie auf die Karte. Ihr Mann geht los und baut dort einen Bunker, der Ihnen treu ist.",
      controller = "Spiele beginnen mit neutralen Bunkern. Ein voller Bunker braucht 15 Treffer, bis er zerstört ist.\n\nFahren Sie zuerst über den zerstörten Bunker, um ihn in Ihren Panzer zu laden. Drücken Sie dann {BUILD_TOOL}, bis das Bunkerwerkzeug gewählt ist, setzen Sie den Baucursor an die gewünschte Stelle und drücken Sie {BUILD_PLACE}. Ihr Mann geht los und baut dort einen Bunker, der Ihnen treu ist.",
    },
    s4b = "Östlich der Straße steht ein beschädigter Bunker in einem Ring aus Wald. Er schießt noch, und ein getroffener Bunker wird wütend und schießt schneller. Die roten Markierungen zeigen seine Reichweite.\n\nWald verbirgt Panzer vor anderen Panzern und vor Bunkern. Fahren Sie in den Wald innerhalb der Markierungen und bleiben Sie still stehen. Schießen verrät Ihre Position.",
    s4c = "Dieser Bunker ist schon schwer beschädigt: Zwei weitere Treffer zerstören ihn.\n\nIhr Panzer ist voll. Zerstören Sie den Bunker und fahren Sie dann darüber, um ihn aufzunehmen.",
    s5a = "Halten Sie auf dem grünen Feld vor Ihnen an. Sehen Sie zu, wie der befreundete Bot auf der ummauerten Insel einen Bunker erobert. Er baut seinen eigenen Bunker direkt neben den feindlichen und parkt dahinter. So treffen die feindlichen Schüsse seinen Bunker, nicht seinen Panzer. Dann zerstört er den feindlichen Bunker und fährt darüber.",
    s5b = {
      "Jetzt Sie: Sie tragen einen Bunker. Bauen Sie ihn auf dem markierten grünen Feld, direkt neben dem feindlichen. Halten Sie dann auf dem hervorgehobenen Straßenfeld an und zielen Sie auf den feindlichen Bunker: Schießen Sie, bis er zerstört ist. Ihr Bunker fängt seine Schüsse ab und wird neu gebaut, wenn er zerstört wird. Fahren Sie in das RESET-Gehege, um neu zu beginnen.",
      "Beginnen Sie mit voller Panzerung. Hinweis: Es ist normal, dass Ihr Panzer etwas Schaden nimmt, nachdem der feindliche Bunker Ihren Schutzbunker zerschossen hat.",
    },
    s6 = {
      "Töten Sie den Mann eines Gegners, dann kann sein Panzer nicht bauen, bis ein neuer Mann ankommt. Sie können einen Mann nicht überfahren: Nur Granaten, Minen und Explosionen töten ihn.",
      "Eine Granate explodiert an Ihrem Fadenkreuz und tötet einen Mann im Umkreis von einem halben Feld. Drücken Sie {GUN_DOWN}, um das Fadenkreuz heranzuholen, und drücken Sie {GUN_UP}, um es weiter wegzuschieben. In diesem Tutorial schickt ein Bot seinen Mann über die Krater. Setzen Sie Ihr Fadenkreuz dorthin und schießen Sie weiter, während er sie überquert.",
    },
    s6gun = "Getroffen! Drücken Sie jetzt {GUN_UP}, bis Ihr Fadenkreuz wieder auf größter Reichweite steht. Dann sind Sie bereit für die nächste Station.",
    s7 = {
      "Die letzte Runde, gegen einen einfachen Bot. Sie halten drei Ecken der Insel, der Bot hält den Nordosten. Erobern Sie sein Land mit Ihren Bunkern und stehlen Sie dann seine Basen wie in Station 2. Sie gewinnen, wenn Ihnen alle Basen gehören. Fahren Sie in das RESET-Gehege, um neu zu beginnen.",
      "Fahren Sie zuerst über die zwei zerstörten Bunker an der Zufahrtsstraße, um sie in Ihren Panzer zu laden. Bauen Sie Ihre Bunker nahe der Grenze zwischen Ihrem Land und seinem Land, um die Kontrolle zu übernehmen.",
    },
    welcome = {
      "Willkommen bei WinBolo! Bolo ist ein Mehrspieler-Panzerspiel, das Stuart Cheshire 1987 zuerst geschrieben hat, und WinBolo führt es heute fort. Sie fahren einen Panzer über eine Insel mit Bunkern und Basen. Bunker sind Geschütztürme, die auf feindliche Panzer schießen. Basen füllen Ihre Granaten, Minen und Panzerung auf.",
      "Ein kleiner Mann fährt in Ihrem Panzer mit. Er steigt aus, um Bäume zu fällen und für Sie zu bauen. Sie spielen in Teams, und das Team, das die Bunker und Basen hält, beherrscht die Insel.",
    },
    win = "Gut gemacht: Ihnen gehören alle Basen, und das Tutorial ist abgeschlossen.\n\nFahren Sie herum, so lange Sie möchten. Verlassen Sie das Spiel über das Menü, wenn Sie bereit sind. Versuchen Sie dann ein Spiel gegen einfache Bots.",
  },
  title = {
    [1] = "Station 1: Fahren",
    [2] = "Station 2: Basen",
    [3] = "Station 3: Bauen",
    [4] = "Station 4: Bunker",
    [5] = "Station 5: Bunker erobern",
    [6] = "Station 6: Der Mann",
    [7] = "Station 7: Letzte Runde",
  },
  list = {
    a_build = "Bunker bauen",
    a_pick = "Zerstörten Bunker holen",
    a_refill = "An Ihrer Basis auffüllen",
    a_watch = "Bot beim Erobern zusehen",
    all = "Alle Feindbasen erobern",
    b_build = "Auf grünem Feld bauen",
    b_hide = "Versteckt in Reichweite",
    b_in = "Im Wald in Reichweite",
    b_kill = "Feindbunker zerstören",
    b_park = "Auf Straßenfeld parken",
    b_pick = "Bunker aufnehmen",
    b_take = "Neutrale Basis nehmen",
    base = "Feindbasis stehlen",
    build = "Bunker in Feindesland",
    c_kill = "Bunker zerstören",
    c_pick = "Bunker aufnehmen",
    c_shoot = "Feindbasis leer schießen",
    c_take = "Darüber fahren",
    crater = "Krater",
    dead = "Beide Bunker aufnehmen",
    forest = "Wald",
    harvest = "Einen Baum fällen",
    kill = "Den Mann abschießen",
    mine = "Eine Mine legen",
    river = "Seichter Fluss",
    road = "Eine Straße bauen",
    rubble = "Trümmer",
    shoot = "Die Mauer zerschießen",
    swamp = "Sumpf",
    wall = "Eine Mauer bauen",
  },
  step = {
    wall = {
      keyboard = "Mauer bauen: Drücken Sie {QUICK_WALL} oder den\nMauerknopf, dann klicken Sie auf ein Grasfeld.",
      touch = "Mauer bauen: Tippen Sie auf den Mauerknopf,\ndann auf ein Grasfeld.",
      controller = "Mauer bauen: Drücken Sie {BUILD_TOOL} bis zur Mauer,\nzielen Sie auf Gras, drücken Sie {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Drücken Sie {QUICK_ROAD} oder den Straßenknopf,\ndann klicken Sie auf ein Grasfeld.",
      touch = "Tippen Sie auf den Straßenknopf,\ndann auf ein Grasfeld.",
      controller = "Drücken Sie {BUILD_TOOL} bis zum Straßenwerkzeug,\nzielen Sie auf Gras, drücken Sie {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Drücken Sie {QUICK_MINE} oder den Minenknopf, dann\nklicken Sie auf ein Feld, um eine Mine zu legen.",
      touch = "Tippen Sie auf den Minenknopf, dann auf\nein Feld, um eine Mine zu legen.",
      controller = "Drücken Sie {BUILD_TOOL} bis zum Minenwerkzeug,\nzielen Sie auf ein Feld, drücken Sie {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Drücken Sie {QUICK_TREE} oder den Baumknopf, dann\nklicken Sie auf ein Waldfeld im markierten Hain.",
      touch = "Tippen Sie auf den Baumknopf, dann auf\nein Waldfeld im markierten Hain.",
      controller = "Drücken Sie {BUILD_TOOL} bis zum Baumwerkzeug,\nzielen Sie auf den Hain, drücken Sie {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Drücken Sie den Bunkerknopf oder {QUICK_PILL},\ndann klicken Sie auf die Karte, um ihn zu bauen.",
      touch = "Tippen Sie auf den Bunkerknopf, dann\nauf die Karte, um ihn zu bauen.",
      controller = "Drücken Sie {BUILD_TOOL} bis zum Bunkerwerkzeug,\nsetzen Sie den Cursor, drücken Sie {BUILD_PLACE}.",
    },
    a_pick = "Fahren Sie über den zerstörten Bunker,\num ihn in Ihren Panzer zu laden.",
    a_refill = "Fahren Sie auf die grün markierte Basis. Dann STOPP:\nWarten Sie dort, bis alles aufgefüllt ist.",
    a_watch = "Halten Sie auf dem grünen Feld vor Ihnen an und\nsehen Sie zu, wie der Bot den Bunker erobert.",
    all = "Erobern Sie alle feindlichen Basen:\n%d von 8 gehören Ihnen.",
    b_build = {
      keyboard = "Drücken Sie den Bunkerknopf oder {QUICK_PILL},\ndann klicken Sie auf das grüne Feld.",
      touch = "Tippen Sie auf den Bunkerknopf,\ndann auf das grüne Feld.",
      controller = "Drücken Sie {BUILD_TOOL} bis zum Bunkerwerkzeug,\nzielen Sie aufs grüne Feld, drücken Sie {BUILD_PLACE}.",
    },
    b_hide = "Bleiben Sie tief im Wald in Reichweite stehen.\nVersteckt: %d von 5 Sekunden.",
    b_in = "Fahren Sie in den Wald innerhalb\nder roten Markierungen.",
    b_kill = "Beschießen Sie den feindlichen Bunker: Drücken\nSie {FIRE}, bis er zerstört ist.",
    b_park = "Halten Sie auf dem hervorgehobenen Straßenfeld\nan und zielen Sie auf den feindlichen Bunker.",
    b_pick = "Fahren Sie über den zerstörten Bunker,\num ihn aufzunehmen.",
    b_take = "Fahren Sie auf die neutrale Basis,\num sie zu übernehmen.",
    base = "Schießen Sie eine feindliche Basis leer: Drücken\nSie {FIRE}. Fahren Sie dann auf die Basis.",
    build = "Bauen Sie Ihre Bunker nahe der Grenze im Land des\nBots (Nordosten), um die Kontrolle zu übernehmen.",
    c_kill = "Beschießen Sie den Bunker zweimal:\nDrücken Sie {FIRE}.",
    c_pick = "Fahren Sie über den zerstörten Bunker,\num ihn aufzunehmen.",
    c_shoot = "Schießen Sie auf die rot markierte Basis: Drücken\nSie {FIRE}. Noch %d Schuss.",
    c_take = "Die Basis ist leer. Fahren Sie darauf,\num sie zu übernehmen.",
    dead = "Fahren Sie über beide zerstörten Bunker\nan der Zufahrtsstraße.",
    kill = "Setzen Sie Ihr Fadenkreuz auf die Krater und\nschießen Sie, wenn der Mann sie überquert.",
    shoot = "Beschießen Sie die markierte Mauer: Drücken\nSie {FIRE}, bis sie fällt.",
  },
  line = {
    done = "Station geschafft. Folgen Sie der\nStraße nach Norden.",
    gun = "Drücken Sie {GUN_UP}, bis Ihr Fadenkreuz\nauf größter Reichweite steht.",
    shells = "Wenig Granaten? Halten Sie an der Basis\nan der Straße, um aufzufüllen.",
    win = "Tutorial abgeschlossen. Verlassen Sie das Spiel\nüber das Menü, wenn Sie bereit sind.",
  },
  info = {
    bases = "Ihre Basen: %d/8",
    enemy_base = "Feindbasis: noch %d Schuss",
    men = "Getötete Männer: %d",
    needed = "%d von %d nötig",
    on = "Feld: %s, Tempo %d",
    on_plain = "Feld: %s",
    pill_armour = "Bunkerpanzerung: %d/%d",
    wall_down = "Zielmauer: gefallen",
    wall_left = "Zielmauer: noch %d Schuss",
  },
  hint = {
    full = "Voll",
    low = "Wenig Granaten: auffüllen",
    refill = "Füllt auf: hier warten",
    reset = "RESET-Gehege: Neustart",
    stop = "STOPP auf Basis: warten",
  },
  terrain = {
    boat = "Boot",
    building = "Gebäude",
    crater = "Krater",
    ["deep sea"] = "Tiefsee",
    forest = "Wald",
    grass = "Gras",
    river = "Fluss",
    road = "Straße",
    rubble = "Trümmer",
    swamp = "Sumpf",
    wall = "Mauer",
  },
  say = {
    back = "Der feindliche Bunker ist zurück. Erobern Sie\nihn erneut oder fahren Sie nach Norden.",
    done = "Station %d geschafft. Folgen Sie der\nStraße nach Norden.",
    done4 = "Station %d geschafft. Folgen Sie der Straße nach Norden.\nSchießen Sie auf Bäume, um sie schnell zu räumen.",
    moved = "Zum grünen Feld versetzt.",
    reset = "Station %d zurückgesetzt",
  },
}

-- Spanish
TEXT["es"] = {
  pop = {
    s1 = {
      keyboard = "Pulsa {ACCEL} para avanzar, {BRAKE} para frenar, y {LEFT} y {RIGHT} para girar.\n\nLas zonas junto a la carretera son bosque, pantano, escombros, cráteres y un río poco profundo. Cada una te frena de forma distinta. Prueba a conducir por algunas.\n\nEl agua profunda hunde los tanques. Si mueres en cualquier sitio de aquí, vuelves a tu último punto de control.",
      touch = "Conduce con el stick del lado izquierdo de la pantalla: empújalo hacia donde quieras ir. Cuanto más lo empujes, más rápido irás.\n\nLas zonas junto a la carretera son bosque, pantano, escombros, cráteres y un río poco profundo. Cada una te frena de forma distinta. Prueba a conducir por algunas.\n\nEl agua profunda hunde los tanques. Si mueres en cualquier sitio de aquí, vuelves a tu último punto de control.",
    },
    s2a = "Las bases rellenan tus obuses, minas y blindaje, y a tu tanque le queda poco.\n\nConduce hasta tu base, en el marcador verde. Luego PARA sobre la base y espera mientras se rellena.",
    s2b = "Las bases neutrales no tienen rojo ni verde, y se capturan pasando por encima con el tanque.",
    s2c = "Primero hay que vaciar a disparos las bases enemigas. Una llena necesita %d disparos: apunta a ella y pulsa {FIRE} para disparar. Luego pasa por encima para hacerla tuya.",
    s2d = "Con qué reaparece tu tanque depende del tipo de partida.\n\nAbierta: los tanques siempre reaparecen con todo lleno.\n\nTorneo: cuantas más bases están tomadas, menos obuses tiene un tanque al reaparecer; cuando todas las bases están tomadas, los tanques reaparecen sin obuses.\n\nTorneo estricto: los tanques nunca reaparecen con obuses.\n\nLos modos torneo tienen una condición de victoria clara: cuando un equipo posee todas las bases, gana, porque el equipo enemigo no puede conseguir munición.",
    s2stay = "Esta es tu base. Quédate en ella y no te muevas: tus obuses, minas y blindaje se rellenan poco a poco.",
    s3 = {
      keyboard = "Tu constructor tala árboles y luego los usa para construir carreteras, muros, minas y búnkeres. Haz clic en un botón de construcción o pulsa su tecla: {QUICK_TREE} árboles, {QUICK_ROAD} carretera, {QUICK_WALL} muro, {QUICK_MINE} mina. Luego haz clic en el mapa para enviar allí a tu constructor.\n\nConsejo: con la herramienta de carretera, muro o búnker elegida, haz clic en un árbol y tu constructor lo tala. No tienes que cambiar a la herramienta de árboles cada vez.",
      touch = "Tu constructor tala árboles y luego los usa para construir carreteras, muros, minas y búnkeres. Toca un botón de construcción: árboles, carretera, muro o mina. Luego toca el mapa para enviar allí a tu constructor.\n\nConsejo: con la herramienta de carretera, muro o búnker elegida, toca un árbol y tu constructor lo tala. No tienes que cambiar a la herramienta de árboles cada vez.",
      controller = {
        "Tu constructor tala árboles y luego los usa para construir carreteras, muros, minas y búnkeres. Pulsa {BUILD_MODE} para el cursor de construcción y apúntalo con el stick derecho. Pulsa {BUILD_TOOL} para elegir una herramienta: árboles, carretera, muro o mina. Luego pulsa {BUILD_PLACE} para enviar a tu constructor al cursor.",
        "Consejo: con la herramienta de carretera, muro o búnker elegida, pon el cursor de construcción sobre un árbol y pulsa {BUILD_PLACE}: tu constructor lo tala. No tienes que cambiar a la herramienta de árboles cada vez.",
      },
    },
    s4a = {
      keyboard = "Las partidas empiezan con búnkeres neutrales, y uno lleno necesita 15 disparos para destruirlo.\n\nPrimero, pasa sobre el búnker muerto para meterlo en tu tanque. Luego pulsa el botón de construir búnker o su tecla rápida {QUICK_PILL}. Después haz clic en el mapa para enviar a tu constructor a construir allí un búnker leal a ti.",
      touch = "Las partidas empiezan con búnkeres neutrales, y uno lleno necesita 15 disparos para destruirlo.\n\nPrimero, pasa sobre el búnker muerto para meterlo en tu tanque. Luego toca el botón de construir búnker y toca el mapa para enviar a tu constructor a construir allí un búnker leal a ti.",
      controller = "Las partidas empiezan con búnkeres neutrales, y uno lleno necesita 15 disparos para destruirlo.\n\nPrimero, pasa sobre el búnker muerto para meterlo en tu tanque. Luego pulsa {BUILD_TOOL} hasta elegir la herramienta de búnker, pon el cursor de construcción donde lo quieras y pulsa {BUILD_PLACE}. Tu constructor sale y construye allí un búnker leal a ti.",
    },
    s4b = "Al este de la carretera, un búnker dañado está dentro de un anillo de bosque. Todavía dispara, y un búnker al que golpeas se enfada y dispara más rápido. Los marcadores rojos muestran su alcance.\n\nLos bosques ocultan los tanques tanto de otros tanques como de los búnkeres. Entra en el bosque dentro de los marcadores y quédate quieto. Disparar revela tu posición.",
    s4c = "Este búnker ya está muy dañado: dos disparos más lo destruyen.\n\nTu tanque está lleno. Destrúyelo y luego pasa por encima para recogerlo.",
    s5a = "Para en la casilla verde de delante. Mira cómo el bot amigo de la isla amurallada toma un búnker. Construye su propio búnker justo al lado del enemigo y se coloca detrás, para que los disparos del enemigo den en su búnker y no en su tanque. Luego destruye el búnker enemigo a disparos y pasa por encima.",
    s5b = "Tu turno: llevas un búnker. Construye tu búnker en la casilla verde marcada, justo al lado del suyo. Luego para en la casilla de carretera resaltada y apunta al búnker enemigo: dispara hasta destruirlo. Tu búnker recibe sus disparos y se reconstruye si muere. Entra en la zona RESET para empezar de nuevo.\n\nEmpieza con el blindaje lleno. Es normal que tu tanque reciba algo de daño después de que el búnker enemigo haya destruido el búnker que te cubre.",
    s6 = "Mata a un constructor enemigo y su tanque no podrá construir hasta que llegue uno nuevo. No puedes atropellar a un constructor: solo los obuses, las minas y las explosiones lo matan.\n\nUn obús explota en tu mira y mata a un constructor a menos de media casilla. Pulsa {GUN_DOWN} para acercar la mira y {GUN_UP} para alejarla. Para este tutorial, un bot está programado para enviar a su constructor a través de los cráteres. Pon tu mira ahí y sigue disparando mientras cruza.",
    s6gun = "¡Lo tienes! Ahora pulsa {GUN_UP} hasta que tu mira vuelva a su alcance máximo, lista para la siguiente estación.",
    s7 = "La ronda final, contra un bot Fácil. Tienes tres esquinas de la isla y el bot tiene la noreste. Toma su tierra con tus búnkeres y luego roba sus bases como en la Estación 2. Ganas cuando posees todas las bases. Entra en la zona RESET para empezar de nuevo.\n\nPrimero, pasa sobre los dos búnkeres muertos de la carretera de entrada para meterlos en tu tanque. Construye tus búnkeres cerca de la frontera entre tu tierra y la suya para tomar el control.",
    welcome = "¡Bienvenido a WinBolo! Bolo es un juego multijugador de tanques que Stuart Cheshire escribió por primera vez en 1987, y WinBolo lo mantiene vivo hoy. Conduces un tanque por una isla de búnkeres, torres de cañón que disparan a los tanques enemigos, y de bases, que rellenan tus obuses, minas y blindaje.\n\nUn pequeño constructor va en tu tanque y sale a talar árboles y construir para ti. Juegas en equipos, y el equipo que tiene los búnkeres y las bases domina la isla.",
    win = "Bien hecho: posees todas las bases y el tutorial está completo.\n\nConduce todo lo que quieras. Sal desde el menú cuando estés listo y luego prueba una partida contra bots Fáciles.",
  },
  title = {
    [1] = "Estación 1: Conducción",
    [2] = "Estación 2: Bases",
    [3] = "Estación 3: Construcción",
    [4] = "Estación 4: Búnkeres",
    [5] = "Estación 5: Toma de búnker",
    [6] = "Estación 6: El constructor",
    [7] = "Estación 7: Ronda final",
  },
  list = {
    a_build = "Constrúyelo",
    a_pick = "Recoge el búnker muerto",
    a_refill = "Recarga en tu base",
    a_watch = "Mira al bot tomar uno",
    all = "Toma cada base enemiga",
    b_build = "Construye en casilla verde",
    b_hide = "Escóndete a su alcance",
    b_in = "Entra al alcance en bosque",
    b_kill = "Destruye el búnker enemigo",
    b_park = "Para en la carretera",
    b_pick = "Recógelo",
    b_take = "Toma la base neutral",
    base = "Roba una base enemiga",
    build = "Búnker en tierra enemiga",
    c_kill = "Destruye el búnker",
    c_pick = "Recoge el búnker",
    c_shoot = "Vacía la base enemiga",
    c_take = "Pasa por encima",
    crater = "Cráteres",
    dead = "Recoge ambos búnkeres",
    forest = "Bosque",
    harvest = "Tala un árbol",
    kill = "Dispara al constructor",
    mine = "Coloca una mina",
    river = "Río poco profundo",
    road = "Construye una carretera",
    rubble = "Escombros",
    shoot = "Derriba el muro",
    swamp = "Pantano",
    wall = "Construye un muro",
  },
  step = {
    wall = {
      keyboard = "Construye un muro: haz clic en el botón de muro\no pulsa {QUICK_WALL} y haz clic en la hierba.",
      touch = "Construye un muro: toca el botón de muro\ny luego toca una casilla de hierba.",
      controller = "Para un muro, pulsa {BUILD_TOOL} y elige muro.\nPon el cursor en hierba y pulsa {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Haz clic en el botón de carretera o pulsa\n{QUICK_ROAD} y haz clic en la hierba.",
      touch = "Toca el botón de carretera y luego\ntoca una casilla de hierba.",
      controller = "Elige carretera: pulsa {BUILD_TOOL}. Pon el\ncursor en hierba y pulsa {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Haz clic en el botón de mina o pulsa\n{QUICK_MINE} y haz clic donde quieras la mina.",
      touch = "Toca el botón de mina y luego\ntoca una casilla para colocar una mina.",
      controller = "Elige mina: pulsa {BUILD_TOOL}. Pon el\ncursor en una casilla y pulsa {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Haz clic en el botón de árbol o pulsa\n{QUICK_TREE} y haz clic en la arboleda marcada.",
      touch = "Toca el botón de árbol y luego toca una\ncasilla de bosque de la arboleda marcada.",
      controller = "Elige árbol: pulsa {BUILD_TOOL}. Pon el cursor\nen la arboleda marcada y pulsa {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Pulsa el botón de búnker o {QUICK_PILL}\ny haz clic en el mapa para construirlo.",
      touch = "Toca el botón de búnker\ny luego toca el mapa para construirlo.",
      controller = "Elige búnker: pulsa {BUILD_TOOL}. Coloca el\ncursor y pulsa {BUILD_PLACE} para construirlo.",
    },
    a_pick = "Pasa sobre el búnker muerto para meterlo en tu tanque.",
    a_refill = "Conduce hasta la base con marca verde, luego PARA\nsobre ella y espera mientras se rellena.",
    a_watch = "Para en la casilla verde de delante\ny mira cómo el bot toma el búnker.",
    all = "Toma todas las bases enemigas: %d de 8 son tuyas.",
    b_build = {
      keyboard = "Pulsa el botón de búnker o {QUICK_PILL}\ny haz clic en la casilla verde.",
      touch = "Toca el botón de búnker\ny luego toca la casilla verde.",
      controller = "Elige búnker: pulsa {BUILD_TOOL}. Pon el cursor\nen la casilla verde y pulsa {BUILD_PLACE}.",
    },
    b_hide = "Quédate quieto en lo hondo del bosque, a su alcance.\nOculto: %d de 5 segundos.",
    b_in = "Entra en el bosque dentro de los marcadores rojos.",
    b_kill = "Dispara al búnker enemigo: pulsa {FIRE}\nhasta destruirlo.",
    b_park = "Para en la casilla de carretera resaltada\ny apunta al búnker enemigo.",
    b_pick = "Pasa sobre el búnker muerto para recogerlo.",
    b_take = "Pasa sobre la base neutral para hacerla tuya.",
    base = "Pulsa {FIRE} para vaciar una base enemiga\na disparos; luego pasa por encima.",
    build = "Construye tus búnkeres cerca de la frontera, en tierra\ndel bot (noreste), para tomar el control.",
    c_kill = "Pulsa {FIRE} para disparar\ndos veces al búnker.",
    c_pick = "Pasa sobre el búnker muerto para recogerlo.",
    c_shoot = "Dispara a la base con marca roja:\npulsa {FIRE}. Quedan %d disparos.",
    c_take = "La base está vacía. Pasa por encima para hacerla tuya.",
    dead = "Pasa sobre los dos búnkeres muertos\nde la carretera de entrada.",
    kill = "Pon tu mira sobre los cráteres y dispara\ncuando el constructor los cruce.",
    shoot = "Dispara al muro marcado: pulsa {FIRE}\nhasta que caiga.",
  },
  line = {
    done = "Estación completada. Sigue la carretera al norte.",
    gun = "Pulsa {GUN_UP} hasta que tu mira\nesté en su alcance máximo.",
    shells = "¿Pocos obuses? Para en la base junto\na la carretera para recargar.",
    win = "Tutorial completado. Sal desde el\nmenú cuando estés listo.",
  },
  info = {
    bases = "Bases tuyas: %d/8",
    enemy_base = "Base enemiga: faltan %d",
    men = "Constructores muertos: %d",
    needed = "%d de %d necesarios",
    on = "En %s: vel. %d",
    on_plain = "En %s",
    pill_armour = "Blindaje búnker: %d/%d",
    wall_down = "Muro objetivo: derribado",
    wall_left = "Muro objetivo: faltan %d",
  },
  hint = {
    full = "Lleno",
    low = "Pocos obuses: recarga",
    refill = "Recargando: espera aquí",
    reset = "Zona RESET: reiniciar",
    stop = "PARA en la base: espera",
  },
  terrain = {
    boat = "barca",
    building = "edificio",
    crater = "cráter",
    ["deep sea"] = "mar profundo",
    forest = "bosque",
    grass = "hierba",
    river = "río",
    road = "carretera",
    rubble = "escombros",
    swamp = "pantano",
    wall = "muro",
  },
  say = {
    back = "El búnker enemigo ha vuelto.\nTómalo otra vez o ve al norte.",
    done = "Estación %d completada. Sigue la carretera al norte.",
    done4 = "Estación %d completada. Sigue la carretera al norte.\nDispara a los árboles para despejarlos rápido.",
    moved = "Movido a la casilla verde.",
    reset = "Estación %d reiniciada",
  },
}

-- French
TEXT["fr"] = {
  pop = {
    s1 = {
      keyboard = "Appuyez sur {ACCEL} pour avancer, sur {BRAKE} pour ralentir, et sur {LEFT} ou {RIGHT} pour tourner.\n\nLes zones au bord de la route sont de la forêt, du marais, des décombres, des cratères et une rivière peu profonde. Chacune vous ralentit plus ou moins. Essayez d'en traverser quelques-unes.\n\nL'eau profonde coule les tanks. Si vous mourez ici, vous revenez à votre dernier point de contrôle.",
      touch = "Conduisez avec le joystick à gauche de l'écran : poussez-le dans la direction voulue. Plus vous le poussez, plus vous allez vite.\n\nLes zones au bord de la route sont de la forêt, du marais, des décombres, des cratères et une rivière peu profonde. Chacune vous ralentit plus ou moins. Essayez d'en traverser quelques-unes.\n\nL'eau profonde coule les tanks. Si vous mourez ici, vous revenez à votre dernier point de contrôle.",
    },
    s2a = "Les bases rechargent vos obus, vos mines et votre blindage, et votre tank est presque vide.\n\nRoulez sur votre base au marqueur vert. Puis ARRÊTEZ-VOUS sur la base et attendez pendant qu'elle vous recharge.",
    s2b = "Les bases neutres ne sont ni rouges ni vertes. Vous les prenez en roulant dessus.",
    s2c = "Les bases ennemies doivent d'abord être vidées. Une base pleine demande %d tirs : visez-la et appuyez sur {FIRE} pour tirer. Roulez ensuite dessus pour la prendre.",
    s2d = "Ce que votre tank reçoit à la réapparition dépend du type de partie.\n\nOuvert : les tanks réapparaissent toujours avec le plein.\n\nTournoi : plus il y a de bases prises, moins un tank réapparaît avec d'obus. Quand toutes les bases sont prises, les tanks réapparaissent sans obus.\n\nTournoi strict : les tanks ne réapparaissent jamais avec des obus.\n\nLes modes tournoi ont un but clair : dès qu'une équipe possède toutes les bases, elle gagne, car l'équipe ennemie n'a plus de munitions.",
    s2stay = "Voici votre base. Restez dessus et ne bougez pas : vos obus, vos mines et votre blindage arrivent petit à petit.",
    s3 = {
      keyboard = "Votre constructeur récolte des arbres pour construire des routes, des murs, des mines et des pillboxes. Cliquez sur un bouton de construction, ou appuyez sur sa touche : {QUICK_TREE} arbres, {QUICK_ROAD} route, {QUICK_WALL} mur, {QUICK_MINE} mine. Cliquez ensuite sur la carte pour y envoyer votre constructeur.\n\nAstuce : avec l'outil route, mur ou pillbox, cliquez sur un arbre : votre constructeur le récolte, sans passer à l'outil arbre.",
      touch = "Votre constructeur récolte des arbres pour construire des routes, des murs, des mines et des pillboxes. Tapez sur un bouton de construction : arbres, route, mur ou mine. Tapez ensuite sur la carte pour y envoyer votre constructeur.\n\nAstuce : avec l'outil route, mur ou pillbox, tapez sur un arbre : votre constructeur le récolte, sans passer à l'outil arbre.",
      controller = "Votre constructeur récolte des arbres pour construire des routes, des murs, des mines et des pillboxes. Appuyez sur {BUILD_MODE} et visez avec le stick droit. Appuyez sur {BUILD_TOOL} pour choisir : arbres, route, mur ou mine. Puis appuyez sur {BUILD_PLACE} pour y envoyer votre constructeur.\n\nAstuce : avec l'outil route, mur ou pillbox, visez un arbre et appuyez sur {BUILD_PLACE} : votre constructeur le récolte, sans passer à l'outil arbre.",
    },
    s4a = {
      keyboard = "Les parties commencent avec des pillboxes neutres, et un pillbox plein demande 15 tirs pour être détruit.\n\nD'abord, roulez sur le pillbox détruit pour le charger dans votre tank. Appuyez ensuite sur le bouton de construction de pillbox ou sur sa touche {QUICK_PILL}. Cliquez ensuite sur la carte pour envoyer votre constructeur y bâtir un pillbox qui vous est fidèle.",
      touch = "Les parties commencent avec des pillboxes neutres, et un pillbox plein demande 15 tirs pour être détruit.\n\nD'abord, roulez sur le pillbox détruit pour le charger dans votre tank. Tapez ensuite sur le bouton de construction de pillbox, puis tapez sur la carte pour envoyer votre constructeur y bâtir un pillbox qui vous est fidèle.",
      controller = "Les parties commencent avec des pillboxes neutres, et un pillbox plein demande 15 tirs pour être détruit.\n\nD'abord, roulez sur le pillbox détruit pour le charger dans votre tank. Appuyez ensuite sur {BUILD_TOOL} jusqu'à choisir l'outil pillbox, placez le curseur de construction où vous voulez et appuyez sur {BUILD_PLACE}. Votre constructeur sort et y bâtit un pillbox qui vous est fidèle.",
    },
    s4b = "À l'est de la route, un pillbox endommagé se trouve dans un anneau de forêt. Il tire encore, et un pillbox que vous touchez se met en colère et tire plus vite. Les marqueurs rouges montrent sa portée.\n\nLa forêt cache les tanks des autres tanks et des pillboxes. Entrez dans la forêt à l'intérieur des marqueurs et restez immobile. Tirer révèle votre position.",
    s4c = "Ce pillbox est déjà très endommagé : deux tirs de plus le détruisent.\n\nVotre tank est plein. Détruisez le pillbox, puis roulez dessus pour le ramasser.",
    s5a = "Arrêtez-vous sur la case verte devant vous. Regardez le bot allié sur l'île entourée de murs prendre un pillbox. Il construit son propre pillbox juste à côté de celui de l'ennemi et se gare derrière. Ainsi, les tirs ennemis touchent son pillbox, pas son tank. Puis il détruit le pillbox ennemi et roule dessus.",
    s5b = {
      "À vous : vous portez un pillbox. Construisez votre pillbox sur la case verte marquée, juste à côté du leur. Arrêtez-vous ensuite sur la case de route en surbrillance et visez le pillbox ennemi : tirez jusqu'à ce qu'il soit détruit. Votre pillbox encaisse ses tirs, et il est reconstruit s'il est détruit. Entrez dans l'enclos RESET pour recommencer.",
      "Commencez avec un blindage plein. Notez qu'il est normal que votre tank subisse quelques dégâts une fois que le pillbox ennemi a détruit votre pillbox bouclier.",
    },
    s6 = {
      "Tuez le constructeur d'un ennemi, et son tank ne peut plus construire jusqu'à l'arrivée d'un nouveau constructeur. Vous ne pouvez pas écraser un constructeur : seuls les obus, les mines et les explosions le tuent.",
      "Un obus explose sur votre viseur et tue un constructeur à moins d'une demi-case. Appuyez sur {GUN_DOWN} pour rapprocher le viseur et appuyez sur {GUN_UP} pour l'éloigner. Pour ce tutoriel, un bot envoie son constructeur à travers les cratères. Placez votre viseur là et continuez à tirer pendant qu'il traverse.",
    },
    s6gun = "Touché ! Appuyez maintenant sur {GUN_UP} jusqu'à ce que votre viseur revienne à sa portée maximale. Vous serez prêt pour la prochaine station.",
    s7 = {
      "La dernière manche, contre un bot facile. Vous tenez trois coins de l'île et le bot tient le nord-est. Prenez son territoire avec vos pillboxes, puis volez ses bases comme à la Station 2. Vous gagnez quand vous possédez toutes les bases. Entrez dans l'enclos RESET pour recommencer.",
      "D'abord, roulez sur les deux pillboxes détruits sur la route d'accès pour les charger dans votre tank. Construisez vos pillboxes près de la frontière entre votre territoire et le sien pour en prendre le contrôle.",
    },
    welcome = {
      "Bienvenue dans WinBolo ! Bolo est un jeu de tanks multijoueur écrit pour la première fois par Stuart Cheshire en 1987, et WinBolo le poursuit aujourd'hui. Vous conduisez un tank sur une île pleine de pillboxes et de bases. Les pillboxes sont des tourelles qui tirent sur les tanks ennemis. Les bases rechargent vos obus, vos mines et votre blindage.",
      "Un petit constructeur voyage dans votre tank et en sort pour couper des arbres et construire pour vous. Vous jouez en équipes, et l'équipe qui tient les pillboxes et les bases tient l'île.",
    },
    win = "Bravo : vous possédez toutes les bases, et le tutoriel est terminé.\n\nRoulez aussi longtemps que vous voulez. Quittez la partie depuis le menu quand vous êtes prêt, puis essayez une partie contre des bots faciles.",
  },
  title = {
    [1] = "Station 1 : Conduite",
    [2] = "Station 2 : Bases",
    [3] = "Station 3 : Construction",
    [4] = "Station 4 : Pillboxes",
    [5] = "Station 5 : Capture",
    [6] = "Station 6 : Constructeur",
    [7] = "Station 7 : Manche finale",
  },
  list = {
    a_build = "Construisez-le",
    a_pick = "Ramassez le pillbox mort",
    a_refill = "Rechargez à votre base",
    a_watch = "Regardez le bot en prendre",
    all = "Prenez toutes les bases",
    b_build = "Bâtissez sur la case verte",
    b_hide = "Restez caché à portée",
    b_in = "Entrez à portée en forêt",
    b_kill = "Détruisez le pillbox",
    b_park = "Garez-vous sur la route",
    b_pick = "Ramassez-le",
    b_take = "Prenez la base neutre",
    base = "Volez une base ennemie",
    build = "Pillbox en terre ennemie",
    c_kill = "Détruisez le pillbox",
    c_pick = "Ramassez le pillbox",
    c_shoot = "Videz la base ennemie",
    c_take = "Roulez dessus",
    crater = "Cratères",
    dead = "Ramassez les 2 pillboxes",
    forest = "Forêt",
    harvest = "Récoltez un arbre",
    kill = "Abattez le constructeur",
    mine = "Posez une mine",
    river = "Rivière peu profonde",
    road = "Construisez une route",
    rubble = "Décombres",
    shoot = "Abattez le mur",
    swamp = "Marais",
    wall = "Construisez un mur",
  },
  step = {
    wall = {
      keyboard = "Construire un mur : appuyez sur {QUICK_WALL}\nou sur le bouton mur, puis cliquez sur l'herbe.",
      touch = "Construire un mur : tapez sur le bouton\nmur, puis sur une case d'herbe.",
      controller = "Mur : appuyez sur {BUILD_TOOL} pour l'outil mur,\nvisez l'herbe et appuyez sur {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Appuyez sur {QUICK_ROAD} ou sur le bouton route,\npuis cliquez sur une case d'herbe.",
      touch = "Tapez sur le bouton route, puis\nsur une case d'herbe.",
      controller = "Appuyez sur {BUILD_TOOL} pour l'outil route,\nvisez l'herbe et appuyez sur {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Appuyez sur {QUICK_MINE} ou sur le bouton mine,\npuis cliquez sur une case pour poser une mine.",
      touch = "Tapez sur le bouton mine, puis sur\nune case pour poser une mine.",
      controller = "Appuyez sur {BUILD_TOOL} pour l'outil mine,\nvisez une case et appuyez sur {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Appuyez sur {QUICK_TREE} ou sur le bouton arbre,\npuis cliquez sur une case du bosquet marqué.",
      touch = "Tapez sur le bouton arbre, puis sur une\ncase de forêt du bosquet marqué.",
      controller = "Appuyez sur {BUILD_TOOL} pour l'outil arbre,\nvisez le bosquet et appuyez sur {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Appuyez sur le bouton pillbox ou sur {QUICK_PILL},\npuis cliquez sur la carte pour le construire.",
      touch = "Tapez sur le bouton pillbox, puis\nsur la carte pour le construire.",
      controller = "Appuyez sur {BUILD_TOOL} pour l'outil pillbox,\nvisez et appuyez sur {BUILD_PLACE} pour le construire.",
    },
    a_pick = "Roulez sur le pillbox détruit pour\nle charger dans votre tank.",
    a_refill = "Roulez sur la base marquée en vert, puis ARRÊTEZ-VOUS\ndessus et attendez qu'elle vous recharge.",
    a_watch = "Arrêtez-vous sur la case verte devant vous\net regardez le bot prendre le pillbox.",
    all = "Prenez toutes les bases ennemies :\n%d sur 8 sont à vous.",
    b_build = {
      keyboard = "Appuyez sur le bouton pillbox ou sur {QUICK_PILL},\npuis cliquez sur la case verte.",
      touch = "Tapez sur le bouton pillbox,\npuis sur la case verte.",
      controller = "Appuyez sur {BUILD_TOOL} pour l'outil pillbox,\nvisez la case verte et appuyez sur {BUILD_PLACE}.",
    },
    b_hide = "Restez immobile au fond de la forêt, à portée.\nCaché : %d secondes sur 5.",
    b_in = "Entrez dans la forêt à l'intérieur\ndes marqueurs rouges.",
    b_kill = "Appuyez sur {FIRE} pour tirer sur le\npillbox ennemi jusqu'à ce qu'il soit détruit.",
    b_park = "Arrêtez-vous sur la case de route en\nsurbrillance et visez le pillbox ennemi.",
    b_pick = "Roulez sur le pillbox détruit\npour le ramasser.",
    b_take = "Roulez sur la base neutre\npour la prendre.",
    base = "Videz une base ennemie : appuyez\nsur {FIRE}, puis roulez dessus.",
    build = "Construisez vos pillboxes près de la frontière, en\nterritoire du bot (nord-est), pour prendre le contrôle.",
    c_kill = "Appuyez deux fois sur {FIRE}\npour tirer sur le pillbox.",
    c_pick = "Roulez sur le pillbox détruit\npour le ramasser.",
    c_shoot = "Tirez sur la base marquée en rouge : appuyez\nsur {FIRE}. Tirs restants : %d.",
    c_take = "La base est vide. Roulez dessus\npour la prendre.",
    dead = "Roulez sur les deux pillboxes détruits\nde la route d'accès.",
    kill = "Placez votre viseur sur les cratères, puis\ntirez quand le constructeur les traverse.",
    shoot = "Tirez sur le mur marqué : appuyez sur\n{FIRE} jusqu'à ce qu'il tombe.",
  },
  line = {
    done = "Station terminée. Suivez la route\nvers le nord.",
    gun = "Appuyez sur {GUN_UP} jusqu'à ce que\nvotre viseur soit à sa portée maximale.",
    shells = "Peu d'obus ? Arrêtez-vous sur la base\nprès de la route pour recharger.",
    win = "Tutoriel terminé. Quittez la partie depuis\nle menu quand vous êtes prêt.",
  },
  info = {
    bases = "Vos bases : %d/8",
    enemy_base = "Base ennemie : reste %d",
    men = "Constructeurs tués : %d",
    needed = "%d sur %d requis",
    on = "%s : vitesse %d",
    on_plain = "Terrain : %s",
    pill_armour = "Blindage pillbox : %d/%d",
    wall_down = "Mur cible : abattu",
    wall_left = "Mur cible : reste %d tirs",
  },
  hint = {
    full = "Plein",
    low = "Peu d'obus : rechargez",
    refill = "Recharge : attendez ici",
    reset = "Enclos RESET : recommencer",
    stop = "STOP sur base : attendez",
  },
  terrain = {
    boat = "Bateau",
    building = "Bâtiment",
    crater = "Cratère",
    ["deep sea"] = "Haute mer",
    forest = "Forêt",
    grass = "Herbe",
    river = "Rivière",
    road = "Route",
    rubble = "Décombres",
    swamp = "Marais",
    wall = "Mur",
  },
  say = {
    back = "Le pillbox ennemi est de retour.\nReprenez-le, ou allez vers le nord.",
    done = "Station %d terminée. Suivez la route\nvers le nord.",
    done4 = "Station %d terminée. Suivez la route vers le nord.\nTirez sur les arbres pour les dégager vite.",
    moved = "Déplacé vers la case verte.",
    reset = "Station %d réinitialisée",
  },
}

-- Italian
TEXT["it"] = {
  pop = {
    s1 = {
      keyboard = "Premi {ACCEL} per avanzare, {BRAKE} per rallentare, {LEFT} e {RIGHT} per girare.\n\nLe zone accanto alla strada sono foresta, palude, macerie, crateri e un fiume poco profondo. Ognuna ti rallenta in modo diverso. Prova a guidare su alcune di esse.\n\nL'acqua profonda affonda i tank. Se muori in qualsiasi punto qui, ricompari all'ultimo checkpoint.",
      touch = "Guida con il thumbstick sul lato sinistro dello schermo: spingilo nella direzione in cui vuoi andare. Più lo spingi, più vai veloce.\n\nLe zone accanto alla strada sono foresta, palude, macerie, crateri e un fiume poco profondo. Ognuna ti rallenta in modo diverso. Prova a guidare su alcune di esse.\n\nL'acqua profonda affonda i tank. Se muori in qualsiasi punto qui, ricompari all'ultimo checkpoint.",
    },
    s2a = "Le basi ricaricano proiettili, mine e corazza, e il tuo tank ne ha pochi.\n\nGuida sulla tua base, al segnale verde. Poi FERMATI sulla base e aspetta che ti ricarichi.",
    s2b = "Le basi neutrali non hanno rosso né verde, e si conquistano passandoci sopra.",
    s2c = "Le basi nemiche vanno prima svuotate a colpi. Una piena richiede %d colpi: mira e premi {FIRE} per sparare. Poi passaci sopra per farla tua.",
    s2d = "Con cosa ricompare il tuo tank dipende dal tipo di partita.\n\nAperta: i tank ricompaiono sempre a pieno carico.\n\nTorneo: più basi sono conquistate, meno proiettili ha un tank quando ricompare; quando tutte le basi sono conquistate, i tank ricompaiono senza proiettili.\n\nTorneo rigoroso: i tank non ricompaiono mai con proiettili.\n\nNei tornei la vittoria è chiara: quando una squadra possiede tutte le basi, vince, perché la squadra nemica non può avere munizioni.",
    s2stay = "Questa è la tua base. Resta qui e non muoverti: proiettili, mine e corazza arrivano un po' alla volta.",
    s3 = {
      keyboard = "Il tuo uomo raccoglie alberi, poi li usa per costruire strade, muri, mine e bunker. Clicca un pulsante di costruzione, o premi il suo tasto: {QUICK_TREE} alberi, {QUICK_ROAD} strada, {QUICK_WALL} muro, {QUICK_MINE} mina. Poi clicca sulla mappa per mandarci il tuo uomo.\n\nConsiglio: con lo strumento strada, muro o bunker scelto, clicca un albero e il tuo uomo lo raccoglie. Non devi passare ogni volta allo strumento alberi.",
      touch = "Il tuo uomo raccoglie alberi, poi li usa per costruire strade, muri, mine e bunker. Tocca un pulsante di costruzione: alberi, strada, muro o mina. Poi tocca la mappa per mandarci il tuo uomo.\n\nConsiglio: con lo strumento strada, muro o bunker scelto, tocca un albero e il tuo uomo lo raccoglie. Non devi passare ogni volta allo strumento alberi.",
      controller = "Il tuo uomo raccoglie alberi, poi li usa per costruire strade, muri, mine e bunker. Premi {BUILD_MODE} per il cursore di costruzione e puntalo con lo stick destro. Premi {BUILD_TOOL} per scegliere uno strumento: alberi, strada, muro o mina. Poi premi {BUILD_PLACE} per mandare il tuo uomo al cursore.\n\nConsiglio: con lo strumento strada, muro o bunker scelto, metti il cursore di costruzione su un albero e premi {BUILD_PLACE}: il tuo uomo lo raccoglie. Non devi passare ogni volta allo strumento alberi.",
    },
    s4a = {
      keyboard = "Le partite iniziano con bunker neutrali, e uno pieno richiede 15 colpi per essere distrutto.\n\nPer prima cosa, passa sopra il bunker distrutto per caricarlo nel tank. Poi premi il pulsante di costruzione bunker o il suo tasto rapido {QUICK_PILL}. Poi clicca sulla mappa per mandare il tuo uomo a costruire lì un bunker fedele a te.",
      touch = "Le partite iniziano con bunker neutrali, e uno pieno richiede 15 colpi per essere distrutto.\n\nPer prima cosa, passa sopra il bunker distrutto per caricarlo nel tank. Poi tocca il pulsante di costruzione bunker e tocca la mappa per mandare il tuo uomo a costruire lì un bunker fedele a te.",
      controller = "Le partite iniziano con bunker neutrali, e uno pieno richiede 15 colpi per essere distrutto.\n\nPer prima cosa, passa sopra il bunker distrutto per caricarlo nel tank. Poi premi {BUILD_TOOL} finché non è scelto lo strumento bunker, metti il cursore di costruzione dove vuoi e premi {BUILD_PLACE}. Il tuo uomo esce e costruisce lì un bunker fedele a te.",
    },
    s4b = "A est della strada, un bunker danneggiato si trova in un anello di foresta. Spara ancora, e un bunker che colpisci si arrabbia e spara più veloce. I segnali rossi mostrano la sua portata.\n\nLe foreste nascondono i tank sia agli altri tank sia ai bunker. Entra nella foresta dentro i segnali e resta fermo. Sparare rivela la tua posizione.",
    s4c = "Questo bunker è già molto danneggiato: bastano altri due colpi per distruggerlo.\n\nIl tuo tank è pieno. Distruggilo, poi passaci sopra per raccoglierlo.",
    s5a = "Fermati sul quadrato verde più avanti. Guarda il bot amico sull'isola cinta da muri prendere un bunker. Costruisce un suo bunker proprio accanto a quello nemico e si mette dietro, così i colpi nemici colpiscono il suo bunker e non il suo tank. Poi distrugge il bunker nemico e ci passa sopra.",
    s5b = "Tocca a te: porti un bunker. Costruisci il tuo bunker sul quadrato verde segnato, proprio accanto al loro. Poi fermati sulla casella di strada evidenziata e mira al bunker nemico: spara finché non è distrutto. Il tuo bunker incassa i suoi colpi e viene ricostruito se cade. Entra nel recinto RESET per ricominciare.\n\nParti con la corazza piena. È normale che il tank subisca qualche danno dopo che il bunker nemico ha distrutto il bunker che ti copre.",
    s6 = "Uccidi un uomo nemico e il suo tank non potrà costruire finché non arriva un uomo nuovo. Non puoi investire un uomo: solo proiettili, mine ed esplosioni lo uccidono.\n\nUn proiettile esplode sul tuo mirino e uccide un uomo entro mezzo quadrato. Premi {GUN_DOWN} per avvicinare il mirino e {GUN_UP} per allontanarlo. Per questo tutorial, un bot è programmato per mandare il suo uomo attraverso i crateri. Metti lì il mirino e continua a sparare mentre lo attraversa.",
    s6gun = "Preso! Ora premi {GUN_UP} finché il mirino non torna alla portata massima, pronto per la prossima stazione.",
    s7 = "Il round finale, contro un bot Facile. Tu tieni tre angoli dell'isola e il bot tiene quello a nord-est. Prendi la sua terra con i tuoi bunker, poi ruba le sue basi come nella Stazione 2. Vinci quando possiedi ogni base. Entra nel recinto RESET per ricominciare.\n\nPer prima cosa, passa sopra i due bunker distrutti sulla strada d'ingresso per caricarli nel tank. Costruisci i tuoi bunker vicino al confine tra la tua terra e la sua per prendere il controllo.",
    welcome = "Benvenuto in WinBolo! Bolo è un gioco multigiocatore di tank che Stuart Cheshire scrisse per la prima volta nel 1987, e WinBolo lo porta avanti oggi. Guidi un tank su un'isola di bunker, torri armate che sparano ai tank nemici, e di basi, che ricaricano proiettili, mine e corazza.\n\nUn piccolo uomo viaggia nel tuo tank ed esce per tagliare alberi e costruire per te. Si gioca a squadre, e la squadra che tiene i bunker e le basi tiene l'isola.",
    win = "Ben fatto: possiedi ogni base e il tutorial è completo.\n\nGuida quanto vuoi. Esci dal menu quando sei pronto, poi prova una partita contro bot Facili.",
  },
  title = {
    [1] = "Stazione 1: Guida",
    [2] = "Stazione 2: Basi",
    [3] = "Stazione 3: Costruire",
    [4] = "Stazione 4: Bunker",
    [5] = "Stazione 5: Prendi bunker",
    [6] = "Stazione 6: L'uomo",
    [7] = "Stazione 7: Round finale",
  },
  list = {
    a_build = "Costruiscilo",
    a_pick = "Raccogli il bunker",
    a_refill = "Ricarica alla tua base",
    a_watch = "Guarda il bot prenderne 1",
    all = "Prendi ogni base nemica",
    b_build = "Costruisci sul verde",
    b_hide = "Resta nascosto a portata",
    b_in = "Entra a portata nel bosco",
    b_kill = "Distruggi bunker nemico",
    b_park = "Fermati sulla strada",
    b_pick = "Raccoglilo",
    b_take = "Prendi la base neutrale",
    base = "Ruba una base nemica",
    build = "Bunker in terra nemica",
    c_kill = "Distruggi il bunker",
    c_pick = "Raccogli il bunker",
    c_shoot = "Svuota la base nemica",
    c_take = "Passaci sopra",
    crater = "Crateri",
    dead = "Raccogli i 2 bunker",
    forest = "Foresta",
    harvest = "Raccogli un albero",
    kill = "Spara all'uomo",
    mine = "Posa una mina",
    river = "Fiume basso",
    road = "Costruisci una strada",
    rubble = "Macerie",
    shoot = "Abbatti il muro",
    swamp = "Palude",
    wall = "Costruisci un muro",
  },
  step = {
    wall = {
      keyboard = "Costruisci un muro: clicca il pulsante muro\no premi {QUICK_WALL}, poi clicca sull'erba.",
      touch = "Costruisci un muro: tocca il pulsante muro,\npoi tocca un quadrato d'erba.",
      controller = "Fai un muro: premi {BUILD_TOOL} per il muro.\nMetti il cursore sull'erba, premi {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Clicca il pulsante strada o premi {QUICK_ROAD},\npoi clicca un quadrato d'erba.",
      touch = "Tocca il pulsante strada, poi tocca un quadrato d'erba.",
      controller = "Scegli strada: premi {BUILD_TOOL}. Metti il\ncursore sull'erba e premi {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Clicca il pulsante mina o premi {QUICK_MINE},\npoi clicca un quadrato per posare una mina.",
      touch = "Tocca il pulsante mina, poi tocca\nun quadrato per posare una mina.",
      controller = "Scegli mina: premi {BUILD_TOOL}. Metti il\ncursore su un quadrato e premi {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Clicca il pulsante albero o premi {QUICK_TREE},\npoi clicca un quadrato del bosco segnato.",
      touch = "Tocca il pulsante albero, poi tocca un\nquadrato di foresta nel bosco segnato.",
      controller = "Scegli alberi: premi {BUILD_TOOL}. Metti il\ncursore sul bosco segnato e premi {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Premi il pulsante bunker o {QUICK_PILL},\npoi clicca sulla mappa per costruirlo.",
      touch = "Tocca il pulsante bunker,\npoi tocca la mappa per costruirlo.",
      controller = "Scegli bunker: premi {BUILD_TOOL}. Metti giù il\ncursore e premi {BUILD_PLACE} per costruirlo.",
    },
    a_pick = "Passa sopra il bunker distrutto per caricarlo nel tank.",
    a_refill = "Guida sulla base col segnale verde, poi FERMATI\nsopra e aspetta che ti ricarichi.",
    a_watch = "Fermati sul quadrato verde più avanti\ne guarda il bot prendere il bunker.",
    all = "Prendi ogni base nemica: %d su 8 sono tue.",
    b_build = {
      keyboard = "Premi il pulsante bunker o {QUICK_PILL},\npoi clicca il quadrato verde.",
      touch = "Tocca il pulsante bunker,\npoi tocca il quadrato verde.",
      controller = "Scegli bunker: premi {BUILD_TOOL}. Metti il\ncursore sul quadrato verde e premi {BUILD_PLACE}.",
    },
    b_hide = "Resta fermo nel folto della foresta, a portata.\nNascosto: %d su 5 secondi.",
    b_in = "Entra nella foresta dentro i segnali rossi.",
    b_kill = "Spara al bunker nemico: premi {FIRE}\nfinché non è distrutto.",
    b_park = "Fermati sulla casella di strada evidenziata\ne mira al bunker nemico.",
    b_pick = "Passa sopra il bunker distrutto per raccoglierlo.",
    b_take = "Passa sopra la base neutrale per farla tua.",
    base = "Premi {FIRE} per svuotare a colpi una\nbase nemica, poi passaci sopra.",
    build = "Costruisci i tuoi bunker vicino al confine, nella\nterra del bot (nord-est), per prendere il controllo.",
    c_kill = "Premi {FIRE} per colpire\nil bunker due volte.",
    c_pick = "Passa sopra il bunker distrutto per raccoglierlo.",
    c_shoot = "Spara alla base segnata in rosso:\npremi {FIRE}. Colpi rimasti: %d.",
    c_take = "La base è vuota. Passaci sopra per farla tua.",
    dead = "Passa sopra i due bunker distrutti\nsulla strada d'ingresso.",
    kill = "Metti il mirino sui crateri, poi\nspara mentre l'uomo li attraversa.",
    shoot = "Spara al muro segnato: premi {FIRE}\nfinché non crolla.",
  },
  line = {
    done = "Stazione completata. Segui la strada verso nord.",
    gun = "Premi {GUN_UP} finché il mirino\nnon è alla portata massima.",
    shells = "Pochi proiettili? Fermati sulla base\nvicino alla strada per ricaricare.",
    win = "Tutorial completato. Esci dal\nmenu quando sei pronto.",
  },
  info = {
    bases = "Basi tue: %d/8",
    enemy_base = "Base nemica: %d colpi",
    men = "Uomini uccisi: %d",
    needed = "%d su %d necessari",
    on = "Su %s: vel. %d",
    on_plain = "Su %s",
    pill_armour = "Corazza bunker: %d/%d",
    wall_down = "Muro bersaglio: abbattuto",
    wall_left = "Muro bersaglio: %d colpi",
  },
  hint = {
    full = "Pieno",
    low = "Pochi proiettili: ricarica",
    refill = "Ricarica: aspetta qui",
    reset = "Recinto RESET: ricomincia",
    stop = "FERMO sulla base: aspetta",
  },
  terrain = {
    boat = "barca",
    building = "edificio",
    crater = "cratere",
    ["deep sea"] = "mare aperto",
    forest = "foresta",
    grass = "erba",
    river = "fiume",
    road = "strada",
    rubble = "macerie",
    swamp = "palude",
    wall = "muro",
  },
  say = {
    back = "Il bunker nemico è tornato.\nPrendilo di nuovo, o vai a nord.",
    done = "Stazione %d completata. Segui la strada verso nord.",
    done4 = "Stazione %d completata. Segui la strada verso nord.\nSpara agli alberi per sgombrarli in fretta.",
    moved = "Spostato sul quadrato verde.",
    reset = "Stazione %d azzerata",
  },
}

-- Japanese
TEXT["ja"] = {
  pop = {
    s1 = {
      keyboard = "{ACCEL} を押すと前進、{BRAKE} を押すと減速、\n{LEFT} と {RIGHT} を押すと旋回します。\n\n道路の脇のマスは、森、沼地、瓦礫、\nクレーター、浅い川です。それぞれ速度の\n落ち方が違います。いくつか走ってみてください。\n\n深い水に入るとタンクは沈みます。ここで撃破されても、\n最後のチェックポイントから再開します。",
      touch = "画面左のスティックで操縦します。\n進みたい方向にスティックを押してください。\n遠くまで押すほど、速く進みます。\n\n道路の脇のマスは、森、沼地、瓦礫、\nクレーター、浅い川です。それぞれ速度の\n落ち方が違います。いくつか走ってみてください。\n\n深い水に入るとタンクは沈みます。ここで撃破されても、\n最後のチェックポイントから再開します。",
    },
    s2a = "基地では砲弾、地雷、装甲を補給できます。\nいまタンクの残りが少なくなっています。\n\n緑の印のある自分の基地に乗ってください。\nそして基地の上で必ず停止し、\n補給が終わるまで待ってください。",
    s2b = "中立の基地は赤でも緑でもありません。\n上を走るだけで自分のものにできます。",
    s2c = "敵の基地は、先に撃って空にする必要があります。\n満タンの基地は %d 発で空になります。\n基地に狙いを付けて、{FIRE} を押して撃ってください。\nそれから上を走ると、自分の基地になります。",
    s2d = {
      "タンクが再出撃するときの装備は、\nゲームタイプによって変わります。\n\nオープン：タンクは常に満タンで再出撃します。\n\nトーナメント：占領された基地が多いほど、\n再出撃時の砲弾は少なくなります。\nすべての基地が占領されると、\n砲弾なしで再出撃します。",
      "厳格トーナメント：タンクは砲弾を持たずに\n再出撃します。\n\nトーナメントモードには明確な勝利条件があります：\n一方のチームがすべての基地を手に入れると、\n敵チームは弾薬を得られないので、\nそのチームの勝ちです。",
    },
    s2stay = "ここがあなたの基地です。動かずに乗っていてください。\n砲弾、地雷、装甲が少しずつ補給されます。",
    s3 = {
      keyboard = {
        "ビルダーは木を収穫し、それを使って道路、壁、\n地雷、トーチカを建設します。\n建設ボタンをクリックするか、そのキーを押します。\n{QUICK_TREE} を押すと木、{QUICK_ROAD} を押すと道路、\n{QUICK_WALL} を押すと壁、{QUICK_MINE} を押すと地雷です。\n次にマップをクリックすると、ビルダーがそこへ向かいます。",
        "ヒント：道路、壁、トーチカのツールを選んだまま\n木をクリックしても、ビルダーが木を収穫します。\n毎回木のツールに切り替える必要はありません。",
      },
      touch = "ビルダーは木を収穫し、それを使って道路、壁、\n地雷、トーチカを建設します。\n建設ボタン（木、道路、壁、地雷）をタップします。\n次にマップをタップすると、\nビルダーがそこへ向かいます。\n\nヒント：道路、壁、トーチカのツールを選んだまま\n木をタップしても、ビルダーが木を収穫します。\n毎回木のツールに切り替える必要はありません。",
      controller = {
        "ビルダーは木を収穫し、それを使って道路、壁、\n地雷、トーチカを建設します。\n{BUILD_MODE} を引くと建設カーソルが出ます。\n右スティックでカーソルを動かします。\n{BUILD_TOOL} を押してツール（木、道路、壁、地雷）を選び、\n{BUILD_PLACE} を押すと、ビルダーがカーソルの場所へ向かいます。",
        "ヒント：道路、壁、トーチカのツールを選んだまま\n建設カーソルを木に合わせて {BUILD_PLACE} を押しても、\nビルダーが木を収穫します。\n毎回木のツールに切り替える必要はありません。",
      },
    },
    s4a = {
      keyboard = "ゲーム開始時のトーチカは中立です。満タンの\nトーチカを倒すには 15 発必要です。\n\nまず、破壊されたトーチカの上を走って、\nタンクに積み込んでください。\n次に、トーチカの建設ボタンを押すか、\nショートカットキー {QUICK_PILL} を押します。\nそしてマップをクリックすると、ビルダーがその場所に\n味方のトーチカを建設します。",
      touch = "ゲーム開始時のトーチカは中立です。満タンの\nトーチカを倒すには 15 発必要です。\n\nまず、破壊されたトーチカの上を走って、\nタンクに積み込んでください。\n次に、トーチカの建設ボタンをタップし、\nマップをタップすると、ビルダーがその場所に\n味方のトーチカを建設します。",
      controller = "ゲーム開始時のトーチカは中立です。満タンの\nトーチカを倒すには 15 発必要です。\n\nまず、破壊されたトーチカの上を走って、\nタンクに積み込んでください。\n次に、トーチカのツールが選ばれるまで {BUILD_TOOL} を押し、\n建設カーソルを建てたい場所に合わせて\n{BUILD_PLACE} を押してください。ビルダーが出ていき、\nそこに味方のトーチカを建設します。",
    },
    s4b = "道路の東に、森に囲まれた損傷したトーチカがあります。\nまだ射撃してきます。撃たれたトーチカは怒って、\nより速く撃つようになります。赤い印がその射程です。\n\n森はタンクを、他のタンクからもトーチカからも隠します。\n印の内側の森に入り、じっとしていてください。\n射撃すると位置がばれます。",
    s4c = "このトーチカはすでに大きく損傷しています。\nあと 2 発で倒せます。\n\nタンクは満タンです。トーチカを倒してから、\n上を走って回収してください。",
    s5a = "前方の緑のマスで止まってください。壁に囲まれた島で、\n味方のボットがトーチカを奪う様子を見てください。\nボットは敵のトーチカのすぐ隣に自分のトーチカを建て、\nその後ろに停車します。こうすると敵の弾は\nボットのタンクではなく、ボットのトーチカに当たります。\nそれから敵のトーチカを撃って倒し、その上を走ります。",
    s5b = {
      "あなたの番です。あなたはトーチカを 1 つ積んでいます。\n印の付いた緑のマス、敵のトーチカのすぐ隣に\nトーチカを建設してください。次に、光っている\n道路のマスで止まり、敵のトーチカに狙いを付けて、\n倒れるまで撃ってください。あなたのトーチカが\n敵の弾を受け止め、壊れても建て直されます。\nRESET の囲いに入ると、最初からやり直せます。",
      "装甲を満タンにして始めてください。\n敵のトーチカが盾のトーチカを撃ち抜いた後に、\nタンクが少しダメージを受けるのは普通のことです。",
    },
    s6 = {
      "敵のビルダーを倒すと、新しいビルダーが来るまで\nそのタンクは建設できません。\nビルダーはひいても倒せません。倒せるのは\n砲弾、地雷、爆発だけです。",
      "砲弾は照準の位置で爆発し、半マス以内の\nビルダーを倒します。{GUN_DOWN} を押すと照準が近づき、\n{GUN_UP} を押すと遠ざかります。このチュートリアルでは、\nボットがクレーターを越えてビルダーを送り出すように\nしてあります。照準をそこに合わせて、\nビルダーが渡る間、撃ち続けてください。",
    },
    s6gun = "命中！ 次に {GUN_UP} を押して、照準を\n最大射程まで戻してください。\n次のステーションに備えましょう。",
    s7 = {
      "最終ラウンドです。相手は簡単ボットです。\nあなたは島の 3 つの角を持ち、ボットは北東を\n持っています。トーチカでボットの土地を奪い、\nステーション 2 と同じように基地を奪ってください。\nすべての基地を手に入れると勝ちです。\nRESET の囲いに入ると、最初からやり直せます。",
      "まず、入り口の道路にある破壊されたトーチカ 2 つの\n上を走って、タンクに積み込んでください。\n自分の土地と相手の土地の境目の近くに\nトーチカを建てて、支配を広げてください。",
    },
    welcome = {
      "WinBolo へようこそ！ Bolo は、Stuart Cheshire が\n1987 年に最初に作った多人数対戦のタンクゲームです。\nWinBolo がそれを今に受け継いでいます。",
      "あなたはタンクを操縦して島を走ります。島には、\n敵のタンクを撃つ砲台であるトーチカと、\n砲弾、地雷、装甲を補給する基地があります。\nタンクには小さなビルダーが乗っていて、\n外に出て木を切り、建設をしてくれます。\nチームに分かれて戦い、トーチカと基地を\n持つチームが島を支配します。",
    },
    win = "お見事！ すべての基地を手に入れました。\nチュートリアルは完了です。\n\n好きなだけ走り回ってください。準備ができたら\nメニューから退出し、簡単ボットとの\nゲームに挑戦してみてください。",
  },
  title = {
    [1] = "ステーション1:走行",
    [2] = "ステーション2:基地",
    [3] = "ステーション3:建設",
    [4] = "ステーション4:トーチカ",
    [5] = "ステーション5:トーチカ奪取",
    [6] = "ステーション6:ビルダー",
    [7] = "ステーション7:最終戦",
  },
  list = {
    a_build = "建設する",
    a_pick = "破壊トーチカを回収",
    a_refill = "自分の基地で補給",
    a_watch = "ボットの奪取を見る",
    all = "敵の基地をすべて奪う",
    b_build = "緑のマスに建設",
    b_hide = "射程内に隠れる",
    b_in = "森から射程に入る",
    b_kill = "敵トーチカを倒す",
    b_park = "道路のマスに停車",
    b_pick = "回収する",
    b_take = "中立の基地を奪う",
    base = "敵の基地を奪う",
    build = "敵の土地にトーチカ",
    c_kill = "トーチカを倒す",
    c_pick = "トーチカを回収",
    c_shoot = "敵の基地を撃って空に",
    c_take = "上を走る",
    crater = "クレーター",
    dead = "破壊トーチカ2つを回収",
    forest = "森",
    harvest = "木を収穫",
    kill = "ビルダーを撃つ",
    mine = "地雷を敷設",
    river = "浅い川",
    road = "道路を建設",
    rubble = "瓦礫",
    shoot = "壁を撃ち壊す",
    swamp = "沼地",
    wall = "壁を建設",
  },
  step = {
    wall = {
      keyboard = "壁を建設：{QUICK_WALL} を押すか壁ボタンを\nクリックし、草のマスをクリック。",
      touch = "壁を建設：壁ボタンをタップし、\n草のマスをタップ。",
      controller = "壁を建設：{BUILD_TOOL} を押して壁ツールを選び、\nカーソルを草に合わせ {BUILD_PLACE} を押す。",
    },
    road = {
      keyboard = "{QUICK_ROAD} を押すか道路ボタンをクリックし、\n草のマスをクリック。",
      touch = "道路ボタンをタップし、\n草のマスをタップ。",
      controller = "{BUILD_TOOL} を押して道路のツールを選び、\nカーソルを草に合わせ {BUILD_PLACE} を押す。",
    },
    mine = {
      keyboard = "{QUICK_MINE} を押すか地雷ボタンをクリックし、\nマスをクリックして地雷を敷設。",
      touch = "地雷ボタンをタップし、\nマスをタップして地雷を敷設。",
      controller = "{BUILD_TOOL} を押して地雷のツールを選び、\nカーソルをマスに合わせ {BUILD_PLACE} を押す。",
    },
    harvest = {
      keyboard = "{QUICK_TREE} を押すか木ボタンをクリックし、\n印の付いた林の森マスをクリック。",
      touch = "木ボタンをタップし、\n印の付いた林の森マスをタップ。",
      controller = "{BUILD_TOOL} を押して木のツールを選び、\nカーソルを印の林に合わせ {BUILD_PLACE} を押す。",
    },
    a_build = {
      keyboard = "トーチカボタンか {QUICK_PILL} を押し、\nマップをクリックして建設。",
      touch = "トーチカボタンをタップし、\nマップをタップして建設。",
      controller = "{BUILD_TOOL} を押してトーチカツールを選び、\nカーソルを置いて {BUILD_PLACE} を押して建設。",
    },
    a_pick = "破壊されたトーチカの上を走って、\nタンクに積み込んでください。",
    a_refill = "緑の印の基地に乗り、その上で停止して\n補給が終わるまで待ってください。",
    a_watch = "前方の緑のマスで止まり、ボットが\nトーチカを奪うのを見てください。",
    all = "敵の基地をすべて奪う：%d / 8 を所有。",
    b_build = {
      keyboard = "トーチカボタンか {QUICK_PILL} を押し、\n緑のマスをクリック。",
      touch = "トーチカボタンをタップし、\n緑のマスをタップ。",
      controller = "{BUILD_TOOL} を押してトーチカツールを選び、\nカーソルを緑のマスに合わせ {BUILD_PLACE} を押す。",
    },
    b_hide = "射程内の森の奥でじっとしてください。\n隠れた時間：%d / 5 秒",
    b_in = "赤い印の内側の森に入ってください。",
    b_kill = "敵のトーチカが倒れるまで\n{FIRE} を押して撃ってください。",
    b_park = "光っている道路のマスで止まり、\n敵のトーチカに狙いを付けてください。",
    b_pick = "破壊されたトーチカの上を走って回収。",
    b_take = "中立の基地に乗って自分のものに。",
    base = "{FIRE} を押して敵の基地を撃って空にし、\nその上に乗ってください。",
    build = "境目の近く、ボットの土地（北東）に\nトーチカを建てて支配してください。",
    c_kill = "{FIRE} を押してトーチカを 2 回撃って。",
    c_pick = "破壊されたトーチカの上を走って回収。",
    c_shoot = "{FIRE} を押して赤い印の基地を撃って。\n残り %d 発",
    c_take = "基地は空です。上に乗って自分のものに。",
    dead = "入り口の道路にある破壊された\nトーチカ 2 つの上を走ってください。",
    kill = "照準をクレーターに合わせ、\nビルダーが渡るときに撃ってください。",
    shoot = "印の付いた壁が崩れるまで\n{FIRE} を押して撃ってください。",
  },
  line = {
    done = "ステーション完了。道路を北へ進んでください。",
    gun = "照準が最大射程になるまで\n{GUN_UP} を押してください。",
    shells = "砲弾が少ない？ 道路脇の基地で\n止まって補給してください。",
    win = "チュートリアル完了。準備ができたら\nメニューから退出してください。",
  },
  info = {
    bases = "自分の基地：%d/8",
    enemy_base = "敵の基地：残り%d発",
    men = "倒したビルダー：%d",
    needed = "進行 %d/%d",
    on = "%s：速度 %d",
    on_plain = "地形：%s",
    pill_armour = "トーチカ装甲：%d/%d",
    wall_down = "目標の壁：破壊済み",
    wall_left = "目標の壁：残り%d発",
  },
  hint = {
    full = "満タン",
    low = "砲弾が少ない：補給を",
    refill = "補給中：ここで待機",
    reset = "RESETの囲い：やり直し",
    stop = "基地で停止：待機",
  },
  terrain = {
    boat = "ボート",
    building = "建物",
    crater = "クレーター",
    ["deep sea"] = "深海",
    forest = "森",
    grass = "草",
    river = "川",
    road = "道路",
    rubble = "瓦礫",
    swamp = "沼地",
    wall = "壁",
  },
  say = {
    back = "敵のトーチカが復活しました。\nもう一度奪うか、北へ進んでください。",
    done = "ステーション %d 完了。道路を北へ進んでください。",
    done4 = "ステーション %d 完了。道路を北へ。\n木は撃てばすばやく取り除けます。",
    moved = "緑のマスに移動しました。",
    reset = "ステーション %d をリセット",
  },
}

-- Korean
TEXT["ko"] = {
  pop = {
    s1 = {
      keyboard = "{ACCEL} 누르면 전진, {BRAKE} 누르면 감속, {LEFT} 또는 {RIGHT} 누르면 회전해요.\n\n도로 옆의 지형은 숲, 늪, 잔해, 분화구, 얕은 강이에요. 지형마다 속도가 느려지는 정도가 달라요. 몇 군데를 직접 달려 보세요.\n\n깊은 물에서는 탱크가 가라앉아요. 여기서 죽더라도 마지막 체크포인트에서 다시 시작해요.",
      touch = "화면 왼쪽의 썸스틱으로 운전하세요. 가고 싶은 방향으로 미세요. 더 멀리 밀수록 더 빨리 가요.\n\n도로 옆의 지형은 숲, 늪, 잔해, 분화구, 얕은 강이에요. 지형마다 속도가 느려지는 정도가 달라요. 몇 군데를 직접 달려 보세요.\n\n깊은 물에서는 탱크가 가라앉아요. 여기서 죽더라도 마지막 체크포인트에서 다시 시작해요.",
    },
    s2a = "기지는 포탄, 지뢰, 장갑을 보충해 줘요. 지금 탱크의 보급품이 부족해요.\n\n초록색 표시가 있는 내 기지로 가세요. 그리고 기지 위에서 꼭 멈춰서 보충이 끝날 때까지 기다리세요.",
    s2b = "중립 기지는 빨간색도 초록색도 없어요. 위로 지나가기만 하면 차지할 수 있어요.",
    s2c = "적 기지는 먼저 쏴서 비워야 해요. 가득 찬 기지는 %d발을 맞아야 비워져요. 기지를 조준하고 {FIRE} 눌러 발사하세요. 그런 다음 위로 지나가면 내 기지가 돼요.",
    s2d = {
      "탱크가 부활할 때 가진 것은 게임 유형에 따라 달라요.\n\n오픈: 탱크는 언제나 보급품을 가득 채운 채 부활해요.\n\n토너먼트: 점령된 기지가 많을수록 부활할 때 포탄이 적어요. 모든 기지가 점령되면 탱크는 포탄 없이 부활해요.",
      "엄격한 토너먼트: 탱크는 절대 포탄을 가진 채 부활하지 않아요.\n\n토너먼트 모드에는 분명한 승리 조건이 있어요. 한 팀이 모든 기지를 차지하면 적 팀은 탄약을 얻을 수 없으므로 그 팀이 이겨요.",
    },
    s2stay = "여기가 내 기지예요. 움직이지 말고 그대로 있으세요. 포탄, 지뢰, 장갑이 조금씩 채워져요.",
    s3 = {
      keyboard = {
        "건설자는 나무를 수확하고, 그 나무로 도로, 벽, 지뢰, 토치카를 건설해요. 건설 버튼을 클릭하거나 단축키를 누르세요. {QUICK_TREE} 누르면 나무, {QUICK_ROAD} 누르면 도로, {QUICK_WALL} 누르면 벽, {QUICK_MINE} 누르면 지뢰예요. 그런 다음 지도를 클릭하면 건설자가 그곳으로 가요.",
        "팁: 도로, 벽, 토치카 도구를 고른 상태에서 나무를 클릭해도 건설자가 나무를 수확해요. 매번 나무 도구로 바꿀 필요가 없어요.",
      },
      touch = "건설자는 나무를 수확하고, 그 나무로 도로, 벽, 지뢰, 토치카를 건설해요. 건설 버튼(나무, 도로, 벽, 지뢰)을 탭하세요. 그런 다음 지도를 탭하면 건설자가 그곳으로 가요.\n\n팁: 도로, 벽, 토치카 도구를 고른 상태에서 나무를 탭해도 건설자가 나무를 수확해요. 매번 나무 도구로 바꿀 필요가 없어요.",
      controller = {
        "건설자는 나무를 수확하고, 그 나무로 도로, 벽, 지뢰, 토치카를 건설해요. {BUILD_MODE} 당기면 건설 커서가 나오고, 오른쪽 스틱으로 커서를 움직여요. {BUILD_TOOL} 눌러 도구(나무, 도로, 벽, 지뢰)를 고르세요. 그런 다음 {BUILD_PLACE} 누르면 건설자가 커서 위치로 가요.",
        "팁: 도로, 벽, 토치카 도구를 고른 상태에서 건설 커서를 나무에 두고 {BUILD_PLACE} 눌러도 건설자가 나무를 수확해요. 매번 나무 도구로 바꿀 필요가 없어요.",
      },
    },
    s4a = {
      keyboard = "게임은 중립 토치카로 시작하고, 가득 찬 토치카를 파괴하려면 15발이 필요해요.\n\n먼저 파괴된 토치카 위로 지나가서 탱크에 실으세요. 그런 다음 토치카 건설 버튼이나 단축키 {QUICK_PILL} 누르세요. 그리고 지도를 클릭하면 건설자가 나가서 그 위치에 내 편 토치카를 지어요.",
      touch = "게임은 중립 토치카로 시작하고, 가득 찬 토치카를 파괴하려면 15발이 필요해요.\n\n먼저 파괴된 토치카 위로 지나가서 탱크에 실으세요. 그런 다음 토치카 건설 버튼을 탭하고, 지도를 탭하면 건설자가 나가서 그 위치에 내 편 토치카를 지어요.",
      controller = "게임은 중립 토치카로 시작하고, 가득 찬 토치카를 파괴하려면 15발이 필요해요.\n\n먼저 파괴된 토치카 위로 지나가서 탱크에 실으세요. 그런 다음 토치카 도구가 골라질 때까지 {BUILD_TOOL} 누르고, 건설 커서를 원하는 곳에 두고 {BUILD_PLACE} 누르세요. 건설자가 나가서 그곳에 내 편 토치카를 지어요.",
    },
    s4b = "도로 동쪽에 손상된 토치카가 숲에 둘러싸여 있어요. 아직 발사하고, 공격받은 토치카는 화가 나서 더 빠르게 발사해요. 빨간 표시는 그 사거리예요.\n\n숲은 다른 탱크와 토치카 모두에게서 탱크를 숨겨 줘요. 표시 안쪽의 숲으로 들어가 가만히 있으세요. 발사하면 위치가 드러나요.",
    s4c = "이 토치카는 이미 크게 손상됐어요. 두 발만 더 맞히면 파괴돼요.\n\n탱크는 가득 찼어요. 토치카를 파괴한 다음 위로 지나가서 주우세요.",
    s5a = "앞의 초록색 칸에서 멈추세요. 벽으로 둘러싸인 섬에서 아군 봇이 토치카를 빼앗는 모습을 보세요. 봇은 적 토치카 바로 옆에 자기 토치카를 짓고 그 뒤에 멈춰요. 그래서 적의 포탄은 봇의 탱크가 아니라 봇의 토치카에 맞아요. 그런 다음 적 토치카를 쏴서 파괴하고 그 위로 지나가요.",
    s5b = {
      "이제 내 차례예요. 토치카를 하나 싣고 있어요. 표시된 초록색 칸, 적 토치카 바로 옆에 토치카를 지으세요. 그런 다음 강조된 도로 칸에 멈춰서 적 토치카를 조준하고, 파괴될 때까지 발사하세요. 내 토치카가 적의 포탄을 대신 맞고, 파괴되면 다시 지어져요. RESET 구역으로 들어가면 처음부터 다시 할 수 있어요.",
      "장갑을 가득 채운 채 시작하세요. 적 토치카가 막아 주는 토치카를 뚫은 뒤에는 탱크가 약간 피해를 입는 것이 정상이에요.",
    },
    s6 = {
      "적 건설자를 죽이면 새 건설자가 올 때까지 그 탱크는 건설할 수 없어요. 건설자는 탱크로 치어서 죽일 수 없어요. 포탄, 지뢰, 폭발만 건설자를 죽일 수 있어요.",
      "포탄은 조준선 위치에서 터지고, 반 칸 안에 있는 건설자를 죽여요. {GUN_DOWN} 누르면 조준선이 가까워지고, {GUN_UP} 누르면 멀어져요. 이 튜토리얼에서는 봇이 건설자를 분화구 너머로 보내도록 프로그래밍되어 있어요. 조준선을 그곳에 두고 건설자가 건너는 동안 계속 발사하세요.",
    },
    s6gun = "맞혔어요! 이제 {GUN_UP} 눌러 조준선을 가장 먼 사거리로 되돌리고 다음 스테이션을 준비하세요.",
    s7 = {
      "마지막 라운드, 쉬움 봇과의 대결이에요. 나는 섬의 세 모서리를, 봇은 북동쪽을 차지하고 있어요. 토치카로 봇의 땅을 빼앗고, 스테이션 2에서 한 것처럼 봇의 기지를 빼앗으세요. 모든 기지를 차지하면 이겨요. RESET 구역으로 들어가면 처음부터 다시 할 수 있어요.",
      "먼저 들어오는 도로 위에 있는 파괴된 토치카 두 개 위로 지나가서 탱크에 실으세요. 내 땅과 상대 땅의 경계 근처에 토치카를 지어 영역을 차지하세요.",
    },
    welcome = {
      "WinBolo에 오신 것을 환영해요! Bolo는 Stuart Cheshire가 1987년에 처음 만든 멀티플레이 탱크 게임이고, 오늘날 WinBolo가 그 뒤를 잇고 있어요.",
      "탱크를 몰고 섬을 돌아다녀요. 섬에는 적 탱크를 쏘는 포탑인 토치카와, 포탄, 지뢰, 장갑을 보충해 주는 기지가 있어요. 탱크에는 작은 건설자가 타고 있어서, 밖으로 나가 나무를 베고 건설을 해 줘요. 팀을 나눠 플레이하고, 토치카와 기지를 차지한 팀이 섬을 차지해요.",
    },
    win = "잘했어요! 모든 기지를 차지했고, 튜토리얼을 마쳤어요.\n\n원하는 만큼 돌아다녀 보세요. 준비가 되면 메뉴에서 나간 다음, 쉬움 봇과 게임을 해 보세요.",
  },
  title = {
    [1] = "스테이션 1: 주행",
    [2] = "스테이션 2: 기지",
    [3] = "스테이션 3: 건설",
    [4] = "스테이션 4: 토치카",
    [5] = "스테이션 5: 토치카 점령",
    [6] = "스테이션 6: 건설자",
    [7] = "스테이션 7: 최종전",
  },
  list = {
    a_build = "건설하기",
    a_pick = "파괴된 토치카 줍기",
    a_refill = "내 기지에서 보충",
    a_watch = "봇의 점령 지켜보기",
    all = "적 기지 모두 차지",
    b_build = "초록색 칸에 건설",
    b_hide = "사거리 안에 숨기",
    b_in = "숲으로 사거리 진입",
    b_kill = "적 토치카 파괴",
    b_park = "도로 칸에 정차",
    b_pick = "토치카 줍기",
    b_take = "중립 기지 차지",
    base = "적 기지 빼앗기",
    build = "적 땅에 토치카 짓기",
    c_kill = "토치카 파괴",
    c_pick = "토치카 줍기",
    c_shoot = "적 기지를 쏴서 비우기",
    c_take = "위로 지나가기",
    crater = "분화구",
    dead = "파괴된 토치카 둘 줍기",
    forest = "숲",
    harvest = "나무 수확",
    kill = "건설자 쏘기",
    mine = "지뢰 설치",
    river = "얕은 강",
    road = "도로 건설",
    rubble = "잔해",
    shoot = "벽 쏘아 무너뜨리기",
    swamp = "늪",
    wall = "벽 건설",
  },
  step = {
    wall = {
      keyboard = "벽 건설: 벽 버튼을 클릭하거나\n{QUICK_WALL} 누르고, 풀 칸을 클릭하세요.",
      touch = "벽 건설: 벽 버튼을 탭하고,\n풀 칸을 탭하세요.",
      controller = "벽 건설: {BUILD_TOOL} 눌러 벽 도구를 고르고,\n건설 커서를 풀에 두고 {BUILD_PLACE} 누르세요.",
    },
    road = {
      keyboard = "도로 버튼을 클릭하거나 {QUICK_ROAD} 누르고,\n풀 칸을 클릭하세요.",
      touch = "도로 버튼을 탭하고, 풀 칸을 탭하세요.",
      controller = "{BUILD_TOOL} 눌러 도로 도구를 고르고, 건설\n커서를 풀에 두고 {BUILD_PLACE} 누르세요.",
    },
    mine = {
      keyboard = "지뢰 버튼을 클릭하거나 {QUICK_MINE} 누르고,\n칸을 클릭해 지뢰를 설치하세요.",
      touch = "지뢰 버튼을 탭하고,\n칸을 탭해 지뢰를 설치하세요.",
      controller = "{BUILD_TOOL} 눌러 지뢰 도구를 고르고, 건설\n커서를 칸에 두고 {BUILD_PLACE} 누르세요.",
    },
    harvest = {
      keyboard = "나무 버튼을 클릭하거나 {QUICK_TREE} 누르고,\n표시된 숲에서 숲 칸을 클릭하세요.",
      touch = "나무 버튼을 탭하고,\n표시된 숲에서 숲 칸을 탭하세요.",
      controller = "{BUILD_TOOL} 눌러 나무 도구를 고르고, 건설\n커서를 표시된 숲에 두고 {BUILD_PLACE} 누르세요.",
    },
    a_build = {
      keyboard = "토치카 버튼이나 {QUICK_PILL} 누르고,\n지도를 클릭해 건설하세요.",
      touch = "토치카 버튼을 탭하고,\n지도를 탭해 건설하세요.",
      controller = "{BUILD_TOOL} 눌러 토치카 도구를 고르고, 건설\n커서를 두고 {BUILD_PLACE} 눌러 건설하세요.",
    },
    a_pick = "파괴된 토치카 위로 지나가서\n탱크에 실으세요.",
    a_refill = "초록색 표시 기지에 올라가 꼭 멈추고\n보충이 끝날 때까지 기다리세요.",
    a_watch = "앞의 초록색 칸에 멈춰서\n봇이 토치카를 빼앗는 것을 보세요.",
    all = "적 기지를 모두 차지하세요: %d/8 차지함.",
    b_build = {
      keyboard = "토치카 버튼이나 {QUICK_PILL} 누르고,\n초록색 칸을 클릭하세요.",
      touch = "토치카 버튼을 탭하고,\n초록색 칸을 탭하세요.",
      controller = "{BUILD_TOOL} 눌러 토치카 도구를 고르고, 건설\n커서를 초록색 칸에 두고 {BUILD_PLACE} 누르세요.",
    },
    b_hide = "사거리 안 숲 깊은 곳에 가만히 있으세요.\n숨은 시간: %d/5초",
    b_in = "빨간 표시 안쪽의 숲으로 들어가세요.",
    b_kill = "{FIRE} 눌러 적 토치카가\n파괴될 때까지 쏘세요.",
    b_park = "강조된 도로 칸에 멈춰서\n적 토치카를 조준하세요.",
    b_pick = "파괴된 토치카 위로 지나가서 주우세요.",
    b_take = "중립 기지 위로 지나가서 차지하세요.",
    base = "{FIRE} 눌러 적 기지를 쏴서 비운 다음\n그 위로 지나가세요.",
    build = "경계 근처, 봇의 땅(북동쪽)에\n토치카를 지어 영역을 차지하세요.",
    c_kill = "{FIRE} 눌러 토치카를 두 번 쏘세요.",
    c_pick = "파괴된 토치카 위로 지나가서 주우세요.",
    c_shoot = "{FIRE} 눌러 빨간 표시 기지를 쏘세요.\n남은 발수: %d",
    c_take = "기지가 비었어요. 위로 지나가서 차지하세요.",
    dead = "들어오는 도로 위의 파괴된\n토치카 두 개 위로 지나가세요.",
    kill = "조준선을 분화구에 맞추고, 건설자가\n건너는 동안 발사하세요.",
    shoot = "{FIRE} 눌러 표시된 벽이\n무너질 때까지 쏘세요.",
  },
  line = {
    done = "스테이션 완료. 도로를 따라 북쪽으로 가세요.",
    gun = "조준선이 가장 먼 사거리가 될 때까지\n{GUN_UP} 누르세요.",
    shells = "포탄이 부족한가요? 도로 옆 기지에\n멈춰서 보충하세요.",
    win = "튜토리얼 완료. 준비가 되면\n메뉴에서 나가세요.",
  },
  info = {
    bases = "내 기지: %d/8",
    enemy_base = "적 기지: %d발 남음",
    men = "죽인 건설자: %d",
    needed = "진행: %d/%d",
    on = "%s: 속도 %d",
    on_plain = "지형: %s",
    pill_armour = "토치카 장갑: %d/%d",
    wall_down = "목표 벽: 무너짐",
    wall_left = "목표 벽: %d발 남음",
  },
  hint = {
    full = "가득 참",
    low = "포탄 부족: 보충하세요",
    refill = "보충 중: 여기서 대기",
    reset = "RESET 구역: 다시 시작",
    stop = "기지에서 멈추고 대기",
  },
  terrain = {
    boat = "보트",
    building = "건물",
    crater = "분화구",
    ["deep sea"] = "심해",
    forest = "숲",
    grass = "풀",
    river = "강",
    road = "도로",
    rubble = "잔해",
    swamp = "늪",
    wall = "벽",
  },
  say = {
    back = "적 토치카가 되살아났어요.\n다시 빼앗거나 북쪽으로 가세요.",
    done = "스테이션 %d 완료. 도로를 따라 북쪽으로 가세요.",
    done4 = "스테이션 %d 완료. 도로를 따라 북쪽으로.\n나무는 쏘면 빨리 없앨 수 있어요.",
    moved = "초록색 칸으로 옮겨졌어요.",
    reset = "스테이션 %d 초기화",
  },
}

-- Dutch
TEXT["nl"] = {
  pop = {
    s1 = {
      keyboard = "Druk op {ACCEL} om vooruit te rijden, op {BRAKE} om af te remmen, en op {LEFT} of {RIGHT} om te draaien.\n\nDe stukken naast de weg zijn bos, moeras, puin, kraters en een ondiepe rivier. Elk stuk remt je anders af. Rij over een paar ervan.\n\nDiep water laat tanks zinken. Als je hier ergens doodgaat, kom je terug bij je laatste controlepunt.",
      touch = "Rij met de joystick links op het scherm: duw hem de kant op die je wilt. Hoe verder je hem duwt, hoe sneller je gaat.\n\nDe stukken naast de weg zijn bos, moeras, puin, kraters en een ondiepe rivier. Elk stuk remt je anders af. Rij over een paar ervan.\n\nDiep water laat tanks zinken. Als je hier ergens doodgaat, kom je terug bij je laatste controlepunt.",
    },
    s2a = "Bases vullen je granaten, mijnen en pantser aan, en je tank heeft er nog maar weinig.\n\nRij bij de groene markering op je basis. STOP dan op de basis en wacht tot alles is aangevuld.",
    s2b = "Neutrale bases zijn niet rood of groen. Je neemt ze in door eroverheen te rijden.",
    s2c = "Vijandelijke bases moet je eerst leegschieten. Een volle basis kost %d schoten: richt erop en druk op {FIRE} om te vuren. Rij er daarna overheen om hem van jou te maken.",
    s2d = "Waarmee je tank start, hangt af van het speltype.\n\nOpen: tanks starten altijd volledig geladen.\n\nToernooi: hoe meer bases zijn ingenomen, hoe minder granaten een nieuwe tank krijgt. Zijn alle bases ingenomen, dan starten tanks zonder granaten.\n\nStrikt toernooi: tanks starten nooit met granaten.\n\nToernooimodi hebben een duidelijk winstdoel: zodra één team alle bases bezit, wint het, want het vijandelijke team kan geen munitie meer krijgen.",
    s2stay = "Dit is je basis. Blijf erop staan en beweeg niet: je granaten, mijnen en pantser komen er beetje bij beetje bij.",
    s3 = {
      keyboard = "Je man oogst bomen en bouwt er wegen, muren, mijnen en bunkers mee. Klik op een bouwknop of druk op de toets ervan: {QUICK_TREE} bomen, {QUICK_ROAD} weg, {QUICK_WALL} muur, {QUICK_MINE} mijn. Klik dan op de kaart om je man daarheen te sturen.\n\nTip: met het weg-, muur- of bunkergereedschap gekozen klik je op een boom, en je man oogst hem. Je hoeft niet elke keer naar het boomgereedschap te wisselen.",
      touch = "Je man oogst bomen en bouwt er wegen, muren, mijnen en bunkers mee. Tik op een bouwknop: bomen, weg, muur of mijn. Tik dan op de kaart om je man daarheen te sturen.\n\nTip: met het weg-, muur- of bunkergereedschap gekozen tik je op een boom, en je man oogst hem. Je hoeft niet elke keer naar het boomgereedschap te wisselen.",
      controller = "Je man oogst bomen en bouwt er wegen, muren, mijnen en bunkers mee. Trek aan {BUILD_MODE} voor de bouwcursor en richt hem met de rechter stick. Druk op {BUILD_TOOL} om een gereedschap te kiezen: bomen, weg, muur of mijn. Druk dan op {BUILD_PLACE} om je man naar de cursor te sturen.\n\nTip: met het weg-, muur- of bunkergereedschap gekozen zet je de bouwcursor op een boom en druk je op {BUILD_PLACE}, en je man oogst hem. Je hoeft niet elke keer naar het boomgereedschap te wisselen.",
    },
    s4a = {
      keyboard = "Spellen beginnen met neutrale bunkers. Een volle bunker heeft 15 treffers nodig voordat hij kapot is.\n\nRij eerst over de kapotte bunker om hem in je tank te laden. Druk dan op de bunkerbouwknop of op de sneltoets {QUICK_PILL}. Klik daarna op de kaart om je man erop uit te sturen. Hij bouwt daar een bunker die trouw is aan jou.",
      touch = "Spellen beginnen met neutrale bunkers. Een volle bunker heeft 15 treffers nodig voordat hij kapot is.\n\nRij eerst over de kapotte bunker om hem in je tank te laden. Tik dan op de bunkerbouwknop en tik op de kaart om je man erop uit te sturen. Hij bouwt daar een bunker die trouw is aan jou.",
      controller = "Spellen beginnen met neutrale bunkers. Een volle bunker heeft 15 treffers nodig voordat hij kapot is.\n\nRij eerst over de kapotte bunker om hem in je tank te laden. Druk dan op {BUILD_TOOL} tot het bunkergereedschap gekozen is, zet de bouwcursor waar je hem wilt en druk op {BUILD_PLACE}. Je man gaat erop uit en bouwt daar een bunker die trouw is aan jou.",
    },
    s4b = "Ten oosten van de weg staat een beschadigde bunker in een ring van bos. Hij vuurt nog, en een bunker die je raakt wordt boos en vuurt sneller. De rode markeringen tonen zijn bereik.\n\nBos verbergt tanks voor andere tanks en voor bunkers. Rij het bos in binnen de markeringen en sta stil. Vuren verraadt je positie.",
    s4c = "Deze bunker is al zwaar beschadigd: nog twee treffers en hij is kapot.\n\nJe tank is vol. Schiet de bunker kapot en rij er dan overheen om hem op te pakken.",
    s5a = "Stop op het groene vak voor je. Kijk hoe de bevriende bot op het ommuurde eiland een bunker inneemt. Hij bouwt zijn eigen bunker vlak naast die van de vijand en parkeert erachter. Zo raken de vijandelijke schoten zijn bunker, niet zijn tank. Dan schiet hij de vijandelijke bunker kapot en rijdt eroverheen.",
    s5b = {
      "Jouw beurt: je draagt een bunker. Bouw je bunker op het gemarkeerde groene vak, vlak naast die van hen. Stop dan op het gemarkeerde wegvak en richt op de vijandelijke bunker: vuur tot hij kapot is. Jouw bunker vangt zijn schoten op en wordt opnieuw gebouwd als hij kapotgaat. Rij het RESET-hok in om opnieuw te beginnen.",
      "Begin met vol pantser. Let op: het is normaal dat je tank wat schade oploopt nadat de vijandelijke bunker door je schermbunker heen heeft geschoten.",
    },
    s6 = {
      "Dood de man van een vijand, dan kan zijn tank niet bouwen tot er een nieuwe man komt. Je kunt een man niet overrijden: alleen granaten, mijnen en explosies doden hem.",
      "Een granaat ontploft bij je vizier en doodt een man binnen een half vak. Druk op {GUN_DOWN} om het vizier dichterbij te halen en druk op {GUN_UP} om het verder weg te zetten. Voor deze tutorial stuurt een bot zijn man over de kraters. Zet je vizier daar en blijf vuren terwijl hij oversteekt.",
    },
    s6gun = "Raak! Druk nu op {GUN_UP} tot je vizier weer op het grootste bereik staat. Dan ben je klaar voor het volgende station.",
    s7 = {
      "De laatste ronde, tegen een makkelijke bot. Jij hebt drie hoeken van het eiland en de bot heeft het noordoosten. Neem zijn land in met je bunkers en steel dan zijn bases, zoals in Station 2. Je wint als alle bases van jou zijn. Rij het RESET-hok in om opnieuw te beginnen.",
      "Rij eerst over de twee kapotte bunkers op de toegangsweg om ze in je tank te laden. Bouw je bunkers bij de grens tussen jouw land en het zijne om de controle over te nemen.",
    },
    welcome = {
      "Welkom bij WinBolo! Bolo is een tankspel voor meerdere spelers dat Stuart Cheshire in 1987 voor het eerst schreef, en WinBolo zet het vandaag voort. Je rijdt met een tank over een eiland met bunkers en bases. Bunkers zijn geschuttorens die op vijandelijke tanks schieten. Bases vullen je granaten, mijnen en pantser aan.",
      "Een kleine man rijdt mee in je tank. Hij stapt uit om bomen te kappen en voor je te bouwen. Je speelt in teams, en het team dat de bunkers en bases heeft, heeft het eiland.",
    },
    win = "Goed gedaan: alle bases zijn van jou, en de tutorial is klaar.\n\nRij rond zo lang je wilt. Verlaat het spel via het menu als je klaar bent. Probeer daarna een spel tegen makkelijke bots.",
  },
  title = {
    [1] = "Station 1: Rijden",
    [2] = "Station 2: Bases",
    [3] = "Station 3: Bouwen",
    [4] = "Station 4: Bunkers",
    [5] = "Station 5: Bunker innemen",
    [6] = "Station 6: De man",
    [7] = "Station 7: Laatste ronde",
  },
  list = {
    a_build = "Bouw hem",
    a_pick = "Pak de kapotte bunker",
    a_refill = "Vul aan bij je basis",
    a_watch = "Kijk naar de bot",
    all = "Neem alle vijandbases",
    b_build = "Bouw op het groene vak",
    b_hide = "Zit verborgen in bereik",
    b_in = "In bereik in het bos",
    b_kill = "Schiet de bunker kapot",
    b_park = "Parkeer op het wegvak",
    b_pick = "Pak hem op",
    b_take = "Neem de neutrale basis",
    base = "Steel een vijandbasis",
    build = "Bunker in vijandig land",
    c_kill = "Schiet de bunker kapot",
    c_pick = "Pak de bunker op",
    c_shoot = "Schiet vijandbasis leeg",
    c_take = "Rij eroverheen",
    crater = "Kraters",
    dead = "Pak beide kapotte bunkers",
    forest = "Bos",
    harvest = "Oogst een boom",
    kill = "Schiet de man neer",
    mine = "Leg een mijn",
    river = "Ondiepe rivier",
    road = "Bouw een weg",
    rubble = "Puin",
    shoot = "Schiet de muur kapot",
    swamp = "Moeras",
    wall = "Bouw een muur",
  },
  step = {
    wall = {
      keyboard = "Bouw een muur: klik op de muurknop of\ndruk op {QUICK_WALL}, klik dan op een grasvak.",
      touch = "Bouw een muur: tik op de muurknop\nen tik dan op een grasvak.",
      controller = "Bouw een muur: druk op {BUILD_TOOL} voor de muur,\nzet de cursor op gras en druk op {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Klik op de wegknop of druk op {QUICK_ROAD},\nklik dan op een grasvak.",
      touch = "Tik op de wegknop en tik dan op een grasvak.",
      controller = "Druk op {BUILD_TOOL} voor het weggereedschap, zet de\ncursor op gras en druk op {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Klik op de mijnknop of druk op {QUICK_MINE},\nklik dan op een vak om een mijn te leggen.",
      touch = "Tik op de mijnknop en tik dan\nop een vak om een mijn te leggen.",
      controller = "Druk op {BUILD_TOOL} voor het mijngereedschap, zet de\ncursor op een vak en druk op {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Klik op de boomknop of druk op {QUICK_TREE},\nklik dan op een bosvak in het gemarkeerde bosje.",
      touch = "Tik op de boomknop en tik dan op een\nbosvak in het gemarkeerde bosje.",
      controller = "Druk op {BUILD_TOOL} voor het boomgereedschap, zet de\ncursor op het bosje en druk op {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Druk op de bunkerknop of op {QUICK_PILL},\nklik dan op de kaart om hem te bouwen.",
      touch = "Tik op de bunkerknop,\ntik dan op de kaart om hem te bouwen.",
      controller = "Druk op {BUILD_TOOL} voor het bunkergereedschap,\nzet de cursor neer en druk op {BUILD_PLACE}.",
    },
    a_pick = "Rij over de kapotte bunker om\nhem in je tank te laden.",
    a_refill = "Rij op de groen gemarkeerde basis, STOP dan\nop de basis en wacht tot alles is aangevuld.",
    a_watch = "Stop op het groene vak voor je en kijk\nhoe de bot de bunker inneemt.",
    all = "Neem elke vijandelijke basis in:\n%d van 8 zijn van jou.",
    b_build = {
      keyboard = "Druk op de bunkerknop of op {QUICK_PILL},\nklik dan op het groene vak.",
      touch = "Tik op de bunkerknop,\ntik dan op het groene vak.",
      controller = "Druk op {BUILD_TOOL} voor het bunkergereedschap,\nzet de cursor op het groene vak, druk op {BUILD_PLACE}.",
    },
    b_hide = "Sta stil diep in het bos, binnen bereik.\nVerborgen: %d van 5 seconden.",
    b_in = "Rij het bos in binnen\nde rode markeringen.",
    b_kill = "Druk op {FIRE} en schiet op de\nvijandelijke bunker tot hij kapot is.",
    b_park = "Stop op het gemarkeerde wegvak en\nricht op de vijandelijke bunker.",
    b_pick = "Rij over de kapotte bunker\nom hem op te pakken.",
    b_take = "Rij op de neutrale basis\nom hem van jou te maken.",
    base = "Schiet een vijandelijke basis leeg: druk op\n{FIRE}. Rij er daarna op.",
    build = "Bouw je bunkers bij de grens, in het land van de\nbot (noordoosten), om de controle over te nemen.",
    c_kill = "Druk twee keer op {FIRE} om\nop de bunker te schieten.",
    c_pick = "Rij over de kapotte bunker\nom hem op te pakken.",
    c_shoot = "Schiet op de rood gemarkeerde basis: druk op\n{FIRE}. Schoten over: %d.",
    c_take = "De basis is leeg. Rij erop\nom hem van jou te maken.",
    dead = "Rij over beide kapotte bunkers\nop de toegangsweg.",
    kill = "Zet je vizier op de kraters en vuur\nals de man eroverheen gaat.",
    shoot = "Schiet op de gemarkeerde muur: druk op\n{FIRE} tot hij valt.",
  },
  line = {
    done = "Station klaar. Volg de weg\nnaar het noorden.",
    gun = "Druk op {GUN_UP} tot je vizier\nop het grootste bereik staat.",
    shells = "Weinig granaten? Stop op de basis\nbij de weg om aan te vullen.",
    win = "Tutorial klaar. Verlaat het spel via\nhet menu als je klaar bent.",
  },
  info = {
    bases = "Jouw bases: %d/8",
    enemy_base = "Vijandbasis: %d te gaan",
    men = "Mannen gedood: %d",
    needed = "%d van %d nodig",
    on = "%s: snelheid %d",
    on_plain = "Terrein: %s",
    pill_armour = "Bunkerpantser: %d/%d",
    wall_down = "Doelmuur: neer",
    wall_left = "Doelmuur: nog %d schoten",
  },
  hint = {
    full = "Vol",
    low = "Weinig granaten: vul aan",
    refill = "Aanvullen: wacht hier",
    reset = "RESET-hok: opnieuw",
    stop = "STOP op de basis: wacht",
  },
  terrain = {
    boat = "Boot",
    building = "Gebouw",
    crater = "Krater",
    ["deep sea"] = "Open zee",
    forest = "Bos",
    grass = "Gras",
    river = "Rivier",
    road = "Weg",
    rubble = "Puin",
    swamp = "Moeras",
    wall = "Muur",
  },
  say = {
    back = "De vijandelijke bunker is terug.\nNeem hem opnieuw in, of ga naar het noorden.",
    done = "Station %d klaar. Volg de weg\nnaar het noorden.",
    done4 = "Station %d klaar. Volg de weg naar het noorden.\nSchiet op bomen om ze snel op te ruimen.",
    moved = "Naar het groene vak verplaatst.",
    reset = "Station %d opnieuw gestart",
  },
}

-- Polish
TEXT["pl"] = {
  pop = {
    s1 = {
      keyboard = "Naciśnij {ACCEL}, aby jechać naprzód, {BRAKE}, aby zwolnić, {LEFT} i {RIGHT}, aby skręcać.\n\nPola obok drogi to las, bagno, gruzy, kratery i płytka rzeka. Każde spowalnia cię w innym stopniu. Spróbuj przejechać po kilku z nich.\n\nGłęboka woda topi czołgi. Jeśli zginiesz gdziekolwiek tutaj, wrócisz na ostatni punkt kontrolny.",
      touch = "Steruj drążkiem po lewej stronie ekranu: przesuń go w stronę, w którą chcesz jechać. Im dalej go przesuniesz, tym szybciej jedziesz.\n\nPola obok drogi to las, bagno, gruzy, kratery i płytka rzeka. Każde spowalnia cię w innym stopniu. Spróbuj przejechać po kilku z nich.\n\nGłęboka woda topi czołgi. Jeśli zginiesz gdziekolwiek tutaj, wrócisz na ostatni punkt kontrolny.",
    },
    s2a = "Bazy uzupełniają pociski, miny i pancerz, a twój czołg ma ich mało.\n\nWjedź na swoją bazę przy zielonym znaczniku. Potem STAŃ na bazie i poczekaj, aż cię uzupełni.",
    s2b = "Neutralne bazy nie mają koloru czerwonego ani zielonego. Zajmiesz je, przejeżdżając po nich.",
    s2c = "Wrogie bazy trzeba najpierw opróżnić strzałami. Pełna wytrzymuje %d strzałów: wyceluj w nią i naciśnij {FIRE}, aby strzelać. Potem przejedź po niej, aby była twoja.",
    s2d = "To, z czym czołg się odradza, zależy od typu gry.\n\nOtwarta: czołgi zawsze odradzają się w pełni wyposażone.\n\nTurniej: im więcej baz jest zajętych, tym mniej pocisków dostaje odradzający się czołg. Gdy wszystkie bazy są zajęte, czołgi odradzają się bez pocisków.\n\nŚcisły turniej: czołgi nigdy nie odradzają się z pociskami.\n\nTryby turniejowe mają jasny warunek zwycięstwa: gdy jedna drużyna ma wszystkie bazy, wygrywa, bo wroga drużyna nie może zdobyć amunicji.",
    s2stay = "To twoja baza. Zostań na niej i się nie ruszaj: pociski, miny i pancerz napływają po trochu.",
    s3 = {
      keyboard = "Twój człowiek ścina drzewa, a potem buduje z nich drogi, ściany, miny i bunkry. Kliknij przycisk budowania albo naciśnij jego klawisz: {QUICK_TREE} drzewa, {QUICK_ROAD} droga, {QUICK_WALL} ściana, {QUICK_MINE} mina. Potem kliknij na mapie, aby wysłać tam swojego człowieka.\n\nWskazówka: gdy wybrane jest narzędzie drogi, ściany lub bunkra, kliknij drzewo, a twój człowiek je zetnie. Nie musisz za każdym razem przełączać na narzędzie drzew.",
      touch = "Twój człowiek ścina drzewa, a potem buduje z nich drogi, ściany, miny i bunkry. Stuknij przycisk budowania: drzewa, droga, ściana lub mina. Potem stuknij mapę, aby wysłać tam swojego człowieka.\n\nWskazówka: gdy wybrane jest narzędzie drogi, ściany lub bunkra, stuknij drzewo, a twój człowiek je zetnie. Nie musisz za każdym razem przełączać na narzędzie drzew.",
      controller = {
        "Twój człowiek ścina drzewa, a potem buduje z nich drogi, ściany, miny i bunkry. Naciśnij {BUILD_MODE}, aby włączyć kursor budowania, i celuj nim prawym drążkiem. Naciśnij {BUILD_TOOL}, aby wybrać narzędzie: drzewa, droga, ściana lub mina. Potem naciśnij {BUILD_PLACE}, aby wysłać człowieka do kursora.",
        "Wskazówka: gdy wybrane jest narzędzie drogi, ściany lub bunkra, ustaw kursor budowania na drzewie i naciśnij {BUILD_PLACE}, a twój człowiek je zetnie. Nie musisz za każdym razem przełączać na narzędzie drzew.",
      },
    },
    s4a = {
      keyboard = "Gra zaczyna się z neutralnymi bunkrami, a pełny bunkier niszczy dopiero 15 strzałów.\n\nNajpierw przejedź po zniszczonym bunkrze, aby załadować go do czołgu. Potem naciśnij przycisk budowy bunkra albo jego klawisz {QUICK_PILL}. Potem kliknij na mapie, aby wysłać człowieka, który zbuduje tam bunkier wierny tobie.",
      touch = "Gra zaczyna się z neutralnymi bunkrami, a pełny bunkier niszczy dopiero 15 strzałów.\n\nNajpierw przejedź po zniszczonym bunkrze, aby załadować go do czołgu. Potem stuknij przycisk budowy bunkra i stuknij mapę, aby wysłać człowieka, który zbuduje tam bunkier wierny tobie.",
      controller = "Gra zaczyna się z neutralnymi bunkrami, a pełny bunkier niszczy dopiero 15 strzałów.\n\nNajpierw przejedź po zniszczonym bunkrze, aby załadować go do czołgu. Potem naciskaj {BUILD_TOOL}, aż wybierzesz narzędzie bunkra, ustaw kursor budowania w wybranym miejscu i naciśnij {BUILD_PLACE}. Twój człowiek wyjdzie i zbuduje tam bunkier wierny tobie.",
    },
    s4b = "Na wschód od drogi, w pierścieniu lasu, stoi uszkodzony bunkier. Nadal strzela, a trafiony bunkier się złości i strzela szybciej. Czerwone znaczniki pokazują jego zasięg.\n\nLas ukrywa czołgi przed innymi czołgami i przed bunkrami. Wjedź do lasu w obrębie znaczników i stój w miejscu. Strzał zdradza twoją pozycję.",
    s4c = "Ten bunkier jest już mocno uszkodzony: dwa kolejne strzały go zniszczą.\n\nTwój czołg jest pełny. Zniszcz go, a potem przejedź po nim, aby go zabrać.",
    s5a = "Stań na zielonym polu przed tobą. Patrz, jak sojuszniczy bot na otoczonej murem wyspie zdobywa bunkier. Buduje własny bunkier tuż obok wrogiego i parkuje za nim, więc wrogie strzały trafiają jego bunkier, a nie czołg. Potem niszczy wrogi bunkier i przejeżdża po nim.",
    s5b = "Twoja kolej: wieziesz bunkier. Zbuduj swój bunkier na zaznaczonym zielonym polu, tuż obok ich bunkra. Potem stań na podświetlonym polu drogi i wyceluj we wrogi bunkier: strzelaj, aż padnie. Twój bunkier przyjmuje jego strzały i jest odbudowywany, gdy padnie. Wjedź do zagrody RESET, aby zacząć od nowa.\n\nZacznij z pełnym pancerzem. To normalne, że czołg dostaje trochę obrażeń, gdy wrogi bunkier przestrzeli bunkier osłonowy.",
    s6 = "Zabij wrogiego człowieka, a jego czołg nie może budować, dopóki nie przybędzie nowy. Nie da się go przejechać: zabijają go tylko pociski, miny i wybuchy.\n\nPocisk wybucha w miejscu celownika i zabija człowieka w promieniu pół pola. Naciśnij {GUN_DOWN}, aby przybliżyć celownik, i {GUN_UP}, aby go oddalić. W tym samouczku bot wysyła człowieka przez kratery. Ustaw tam celownik i strzelaj, gdy je przechodzi.",
    s6gun = "Trafiony! Teraz naciskaj {GUN_UP}, aż celownik wróci na największy zasięg, gotowy na następną stację.",
    s7 = "Ostatnia runda, przeciw łatwemu botowi. Masz trzy rogi wyspy, a bot ma północny wschód. Zajmij jego teren bunkrami, a potem ukradnij mu bazy, jak na Stacji 2. Wygrywasz, gdy masz wszystkie bazy. Wjedź do zagrody RESET, aby zacząć od nowa.\n\nNajpierw przejedź po dwóch zniszczonych bunkrach na drodze wjazdowej, aby załadować je do czołgu. Buduj bunkry blisko granicy między twoim a jego terenem, aby przejąć kontrolę.",
    welcome = "Witaj w WinBolo! Bolo to wieloosobowa gra czołgowa, którą Stuart Cheshire napisał w 1987 roku, a WinBolo kontynuuje ją dzisiaj. Jeździsz czołgiem po wyspie pełnej bunkrów, czyli wież strzelających do wrogich czołgów, oraz baz, które uzupełniają pociski, miny i pancerz. W czołgu jedzie mały człowiek, który wysiada, by ścinać drzewa i budować. Gracie w drużynach, a drużyna, która ma bunkry i bazy, rządzi wyspą.",
    win = "Brawo: masz wszystkie bazy, a samouczek jest ukończony.\n\nJeźdź, ile chcesz. Gdy będziesz gotów, wyjdź przez menu, a potem zagraj z łatwymi botami.",
  },
  title = {
    [1] = "Stacja 1: Jazda",
    [2] = "Stacja 2: Bazy",
    [3] = "Stacja 3: Budowanie",
    [4] = "Stacja 4: Bunkry",
    [5] = "Stacja 5: Zdobycie bunkra",
    [6] = "Stacja 6: Człowiek",
    [7] = "Stacja 7: Ostatnia runda",
  },
  list = {
    a_build = "Zbuduj go",
    a_pick = "Zabierz zniszczony bunkier",
    a_refill = "Uzupełnij na bazie",
    a_watch = "Patrz, jak bot to robi",
    all = "Zajmij wszystkie bazy",
    b_build = "Buduj na zielonym polu",
    b_hide = "Ukryj się w zasięgu",
    b_in = "Wjedź w zasięg przez las",
    b_kill = "Zniszcz wrogi bunkier",
    b_park = "Stań na polu drogi",
    b_pick = "Zabierz go",
    b_take = "Zajmij neutralną bazę",
    base = "Ukradnij wrogą bazę",
    build = "Bunkier na wrogim terenie",
    c_kill = "Zniszcz bunkier",
    c_pick = "Zabierz bunkier",
    c_shoot = "Opróżnij wrogą bazę",
    c_take = "Przejedź po niej",
    crater = "Kratery",
    dead = "Zabierz oba bunkry",
    forest = "Las",
    harvest = "Zetnij drzewo",
    kill = "Zastrzel człowieka",
    mine = "Połóż minę",
    river = "Płytka rzeka",
    road = "Zbuduj drogę",
    rubble = "Gruzy",
    shoot = "Zestrzel ścianę",
    swamp = "Bagno",
    wall = "Zbuduj ścianę",
  },
  step = {
    wall = {
      keyboard = "Zbuduj ścianę: kliknij przycisk ściany\nalbo naciśnij {QUICK_WALL}, potem kliknij trawę.",
      touch = "Zbuduj ścianę: stuknij przycisk ściany,\npotem stuknij pole trawy.",
      controller = "Ściana: naciśnij {BUILD_TOOL} (ściana),\nustaw kursor na trawie i naciśnij {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Kliknij przycisk drogi albo naciśnij {QUICK_ROAD},\npotem kliknij pole trawy.",
      touch = "Stuknij przycisk drogi, potem stuknij pole trawy.",
      controller = "Naciśnij {BUILD_TOOL} (droga), ustaw\nkursor na trawie i naciśnij {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Kliknij przycisk miny albo naciśnij {QUICK_MINE},\npotem kliknij pole, aby położyć minę.",
      touch = "Stuknij przycisk miny, potem stuknij\npole, aby położyć minę.",
      controller = "Naciśnij {BUILD_TOOL} (mina), ustaw\nkursor na polu i naciśnij {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Kliknij przycisk drzew albo naciśnij {QUICK_TREE},\npotem kliknij pole lasu w zaznaczonym zagajniku.",
      touch = "Stuknij przycisk drzew, potem stuknij\npole lasu w zaznaczonym zagajniku.",
      controller = "Naciśnij {BUILD_TOOL} (drzewa),\nustaw kursor na zagajniku i naciśnij {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Naciśnij przycisk bunkra albo {QUICK_PILL},\npotem kliknij na mapie, aby go zbudować.",
      touch = "Stuknij przycisk bunkra,\npotem stuknij mapę, aby go zbudować.",
      controller = "Naciśnij {BUILD_TOOL}, aby wybrać bunkier, ustaw\nkursor i naciśnij {BUILD_PLACE}, aby go zbudować.",
    },
    a_pick = "Przejedź po zniszczonym bunkrze, aby go załadować.",
    a_refill = "Wjedź na bazę z zielonym znacznikiem, potem\nSTAŃ na niej i poczekaj na uzupełnienie.",
    a_watch = "Stań na zielonym polu przed tobą\ni patrz, jak bot zdobywa bunkier.",
    all = "Zajmij wszystkie wrogie bazy: twoich jest %d z 8.",
    b_build = {
      keyboard = "Naciśnij przycisk bunkra albo {QUICK_PILL},\npotem kliknij zielone pole.",
      touch = "Stuknij przycisk bunkra,\npotem stuknij zielone pole.",
      controller = "Naciśnij {BUILD_TOOL} (bunkier), ustaw\nkursor na zielonym polu i naciśnij {BUILD_PLACE}.",
    },
    b_hide = "Stój w miejscu głęboko w lesie, w zasięgu.\nUkryty: %d z 5 sekund.",
    b_in = "Wjedź do lasu w obrębie czerwonych znaczników.",
    b_kill = "Naciskaj {FIRE} i strzelaj we wrogi\nbunkier, aż padnie.",
    b_park = "Stań na podświetlonym polu drogi\ni wyceluj we wrogi bunkier.",
    b_pick = "Przejedź po zniszczonym bunkrze, aby go zabrać.",
    b_take = "Wjedź na neutralną bazę, aby była twoja.",
    base = "Naciskaj {FIRE} i opróżnij wrogą bazę,\npotem na nią wjedź.",
    build = "Buduj bunkry blisko granicy, na terenie bota\n(północny wschód), aby przejąć kontrolę.",
    c_kill = "Naciśnij {FIRE} i trafiaj\nbunkier dwa razy.",
    c_pick = "Przejedź po zniszczonym bunkrze, aby go zabrać.",
    c_shoot = "Naciskaj {FIRE} i strzelaj w czerwoną\nbazę. Pozostało strzałów: %d.",
    c_take = "Baza jest pusta. Wjedź na nią, aby była twoja.",
    dead = "Przejedź po obu zniszczonych bunkrach\nna drodze wjazdowej.",
    kill = "Ustaw celownik na kraterach, potem\nstrzelaj, gdy człowiek je przechodzi.",
    shoot = "Naciskaj {FIRE} i strzelaj w zaznaczoną\nścianę, aż upadnie.",
  },
  line = {
    done = "Stacja ukończona. Jedź drogą na północ.",
    gun = "Naciskaj {GUN_UP}, aż celownik\nbędzie na największym zasięgu.",
    shells = "Mało pocisków? Stań na bazie\nprzy drodze, aby je uzupełnić.",
    win = "Samouczek ukończony. Gdy będziesz\ngotów, wyjdź przez menu.",
  },
  info = {
    bases = "Twoje bazy: %d/8",
    enemy_base = "Wroga baza: zostało %d",
    men = "Zabici ludzie: %d",
    needed = "%d z %d potrzebnych",
    on = "%s: prędk. %d",
    on_plain = "%s",
    pill_armour = "Pancerz bunkra: %d/%d",
    wall_down = "Cel, ściana: zburzona",
    wall_left = "Cel, ściana: zostało %d",
  },
  hint = {
    full = "Pełne",
    low = "Mało pocisków: uzupełnij",
    refill = "Uzupełnianie: czekaj",
    reset = "Zagroda RESET: od nowa",
    stop = "STAŃ na bazie: czekaj",
  },
  terrain = {
    boat = "Łódź",
    building = "Budynek",
    crater = "Krater",
    ["deep sea"] = "Otwarte morze",
    forest = "Las",
    grass = "Trawa",
    river = "Rzeka",
    road = "Droga",
    rubble = "Gruzy",
    swamp = "Bagno",
    wall = "Ściana",
  },
  say = {
    back = "Wrogi bunkier wrócił.\nZdobądź go znowu albo jedź na północ.",
    done = "Stacja %d ukończona. Jedź drogą na północ.",
    done4 = "Stacja %d ukończona. Jedź drogą na północ.\nStrzelaj w drzewa, aby szybko je usunąć.",
    moved = "Przeniesiono na zielone pole.",
    reset = "Stacja %d od nowa",
  },
}

-- Portuguese (Brazil)
TEXT["pt-br"] = {
  pop = {
    s1 = {
      keyboard = "Pressione {ACCEL} para avançar, {BRAKE} para desacelerar, {LEFT} e {RIGHT} para virar.\n\nAs áreas ao lado da estrada são floresta, pântano, escombros, crateras e um rio raso. Cada uma deixa você mais lento de um jeito diferente. Tente dirigir por algumas delas.\n\nÁgua profunda afunda tanques. Se você morrer em qualquer lugar aqui, volta no seu último ponto de controle.",
      touch = "Dirija com o thumbstick do lado esquerdo da tela: empurre-o na direção em que quer ir. Quanto mais você empurrar, mais rápido vai.\n\nAs áreas ao lado da estrada são floresta, pântano, escombros, crateras e um rio raso. Cada uma deixa você mais lento de um jeito diferente. Tente dirigir por algumas delas.\n\nÁgua profunda afunda tanques. Se você morrer em qualquer lugar aqui, volta no seu último ponto de controle.",
    },
    s2a = "Bases reabastecem seus projéteis, minas e blindagem, e seu tanque está com pouco.\n\nDirija até a sua base, no marcador verde. Depois PARE na base e espere enquanto ela reabastece.",
    s2b = "Bases neutras não têm vermelho nem verde, e você as captura passando por cima delas.",
    s2c = "Bases inimigas precisam primeiro ser esvaziadas a tiros. Uma cheia leva %d tiros: mire nela e pressione {FIRE} para atirar. Depois passe por cima dela para torná-la sua.",
    s2d = "Com o que seu tanque renasce depende do tipo de jogo.\n\nAberto: os tanques sempre renascem carregados.\n\nTorneio: quanto mais bases são tomadas, menos projéteis um tanque tem ao renascer; quando todas as bases são tomadas, os tanques renascem sem projéteis.\n\nTorneio estrito: os tanques nunca renascem com projéteis.\n\nOs modos torneio têm uma condição de vitória clara: quando um time possui todas as bases, ele vence, porque o time inimigo não consegue munição.",
    s2stay = "Esta é a sua base. Fique nela e não se mova: seus projéteis, minas e blindagem entram aos poucos.",
    s3 = {
      keyboard = "Seu construtor colhe árvores e depois as usa para construir estradas, muros, minas e bunkers. Clique em um botão de construção ou pressione a tecla dele: {QUICK_TREE} árvores, {QUICK_ROAD} estrada, {QUICK_WALL} muro, {QUICK_MINE} mina. Depois clique no mapa para mandar seu construtor até lá.\n\nDica: com a ferramenta de estrada, muro ou bunker escolhida, clique em uma árvore e seu construtor a colhe. Você não precisa trocar para a ferramenta de árvores toda vez.",
      touch = "Seu construtor colhe árvores e depois as usa para construir estradas, muros, minas e bunkers. Toque em um botão de construção: árvores, estrada, muro ou mina. Depois toque no mapa para mandar seu construtor até lá.\n\nDica: com a ferramenta de estrada, muro ou bunker escolhida, toque em uma árvore e seu construtor a colhe. Você não precisa trocar para a ferramenta de árvores toda vez.",
      controller = {
        "Seu construtor colhe árvores e depois as usa para construir estradas, muros, minas e bunkers. Pressione {BUILD_MODE} para o cursor de construção e mire-o com o analógico direito. Pressione {BUILD_TOOL} para escolher uma ferramenta: árvores, estrada, muro ou mina. Depois pressione {BUILD_PLACE} para mandar seu construtor até o cursor.",
        "Dica: com a ferramenta de estrada, muro ou bunker escolhida, ponha o cursor de construção em uma árvore e pressione {BUILD_PLACE}: seu construtor a colhe. Você não precisa trocar para a ferramenta de árvores toda vez.",
      },
    },
    s4a = {
      keyboard = "Os jogos começam com bunkers neutros, e um cheio leva 15 tiros para ser destruído.\n\nPrimeiro, passe por cima do bunker destruído para colocá-lo no seu tanque. Depois pressione o botão de construir bunker ou a tecla de atalho {QUICK_PILL}. Então clique no mapa para mandar seu construtor erguer ali um bunker leal a você.",
      touch = "Os jogos começam com bunkers neutros, e um cheio leva 15 tiros para ser destruído.\n\nPrimeiro, passe por cima do bunker destruído para colocá-lo no seu tanque. Depois toque no botão de construir bunker e toque no mapa para mandar seu construtor erguer ali um bunker leal a você.",
      controller = "Os jogos começam com bunkers neutros, e um cheio leva 15 tiros para ser destruído.\n\nPrimeiro, passe por cima do bunker destruído para colocá-lo no seu tanque. Depois pressione {BUILD_TOOL} até escolher a ferramenta de bunker, ponha o cursor de construção onde quiser e pressione {BUILD_PLACE}. Seu construtor sai e ergue ali um bunker leal a você.",
    },
    s4b = "A leste da estrada, um bunker danificado fica num anel de floresta. Ele ainda atira, e um bunker que você acerta fica bravo e atira mais rápido. Os marcadores vermelhos mostram o alcance dele.\n\nFlorestas escondem os tanques tanto de outros tanques quanto dos bunkers. Entre na floresta dentro dos marcadores e fique parado. Atirar revela a sua posição.",
    s4c = "Este bunker já está muito danificado: mais dois tiros o destroem.\n\nSeu tanque está cheio. Destrua-o e depois passe por cima dele para pegá-lo.",
    s5a = "Pare no quadrado verde à frente. Veja o bot aliado na ilha murada tomar um bunker. Ele constrói o próprio bunker bem ao lado do inimigo e para atrás dele, para que os tiros do inimigo acertem o bunker dele, não o tanque. Depois ele destrói o bunker inimigo a tiros e passa por cima dele.",
    s5b = "Sua vez: você carrega um bunker. Construa o seu bunker no quadrado verde marcado, bem ao lado do deles. Depois pare no quadrado de estrada destacado e mire no bunker inimigo: atire até destruí-lo. Seu bunker recebe os tiros dele e é reconstruído se for destruído. Entre no cercado RESET para recomeçar.\n\nComece com a blindagem cheia. É normal o tanque sofrer algum dano depois que o bunker inimigo destrói o bunker que protege você.",
    s6 = "Mate um construtor inimigo e o tanque dele não poderá construir até chegar um construtor novo. Você não pode atropelar um construtor: só projéteis, minas e explosões o matam.\n\nUm projétil explode na sua mira e mata um construtor a menos de meio quadrado. Pressione {GUN_DOWN} para trazer a mira para perto e {GUN_UP} para levá-la para longe. Neste tutorial, um bot foi programado para mandar o construtor pelas crateras. Coloque a mira ali e continue atirando enquanto ele atravessa.",
    s6gun = "Acertou! Agora pressione {GUN_UP} até a mira voltar ao alcance máximo, pronta para a próxima estação.",
    s7 = "A rodada final, contra um bot Fácil. Você tem três cantos da ilha e o bot tem o nordeste. Tome a terra dele com seus bunkers e depois roube as bases dele como na Estação 2. Você vence quando possuir todas as bases. Entre no cercado RESET para recomeçar.\n\nPrimeiro, passe por cima dos dois bunkers destruídos na estrada de entrada para colocá-los no seu tanque. Construa seus bunkers perto da fronteira entre a sua terra e a dele para tomar o controle.",
    welcome = "Bem-vindo ao WinBolo! Bolo é um jogo de tanques multijogador que Stuart Cheshire escreveu pela primeira vez em 1987, e o WinBolo o mantém vivo hoje. Você dirige um tanque por uma ilha de bunkers, torres armadas que atiram em tanques inimigos, e de bases, que reabastecem seus projéteis, minas e blindagem.\n\nUm pequeno construtor anda no seu tanque e sai para cortar árvores e construir para você. Você joga em times, e o time que tem os bunkers e as bases domina a ilha.",
    win = "Muito bem: você possui todas as bases, e o tutorial está completo.\n\nDirija o quanto quiser. Saia pelo menu quando estiver pronto e depois tente um jogo contra bots Fáceis.",
  },
  title = {
    [1] = "Estação 1: Direção",
    [2] = "Estação 2: Bases",
    [3] = "Estação 3: Construção",
    [4] = "Estação 4: Bunkers",
    [5] = "Estação 5: Tomar bunker",
    [6] = "Estação 6: O construtor",
    [7] = "Estação 7: Rodada final",
  },
  list = {
    a_build = "Construa-o",
    a_pick = "Pegue o bunker destruído",
    a_refill = "Reabasteça na sua base",
    a_watch = "Veja o bot tomar um",
    all = "Tome cada base inimiga",
    b_build = "Construa no quadrado verde",
    b_hide = "Fique escondido no alcance",
    b_in = "Entre no alcance na mata",
    b_kill = "Destrua o bunker inimigo",
    b_park = "Pare na estrada",
    b_pick = "Pegue-o",
    b_take = "Tome a base neutra",
    base = "Roube uma base inimiga",
    build = "Bunker em terra inimiga",
    c_kill = "Destrua o bunker",
    c_pick = "Pegue o bunker",
    c_shoot = "Esvazie a base inimiga",
    c_take = "Passe por cima dela",
    crater = "Crateras",
    dead = "Pegue os 2 bunkers",
    forest = "Floresta",
    harvest = "Colha uma árvore",
    kill = "Atire no construtor",
    mine = "Coloque uma mina",
    river = "Rio raso",
    road = "Construa uma estrada",
    rubble = "Escombros",
    shoot = "Derrube o muro",
    swamp = "Pântano",
    wall = "Construa um muro",
  },
  step = {
    wall = {
      keyboard = "Construa um muro: clique no botão de muro\nou pressione {QUICK_WALL} e clique na grama.",
      touch = "Construa um muro: toque no botão de muro,\ndepois toque num quadrado de grama.",
      controller = "Muro: pressione {BUILD_TOOL} e escolha o muro.\nPonha o cursor na grama, pressione {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Clique no botão de estrada ou pressione\n{QUICK_ROAD} e clique num quadrado de grama.",
      touch = "Toque no botão de estrada e depois\ntoque num quadrado de grama.",
      controller = "Escolha estrada: pressione {BUILD_TOOL}. Ponha o\ncursor na grama e pressione {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Clique no botão de mina ou pressione\n{QUICK_MINE} e clique onde quer pôr a mina.",
      touch = "Toque no botão de mina e depois\ntoque num quadrado para pôr uma mina.",
      controller = "Escolha mina: pressione {BUILD_TOOL}. Ponha o\ncursor num quadrado e pressione {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Clique no botão de árvore ou pressione\n{QUICK_TREE}, depois clique no bosque marcado.",
      touch = "Toque no botão de árvore e depois toque num\nquadrado de floresta do bosque marcado.",
      controller = "Escolha árvore: pressione {BUILD_TOOL}. Ponha o\ncursor no bosque e pressione {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Pressione o botão de bunker ou {QUICK_PILL},\ndepois clique no mapa para construí-lo.",
      touch = "Toque no botão de bunker,\ndepois toque no mapa para construí-lo.",
      controller = "Escolha bunker: pressione {BUILD_TOOL}. Ponha o\ncursor e pressione {BUILD_PLACE} para construir.",
    },
    a_pick = "Passe por cima do bunker destruído para pô-lo no tanque.",
    a_refill = "Dirija até a base com marca verde, depois PARE\nnela e espere enquanto ela reabastece.",
    a_watch = "Pare no quadrado verde à frente\ne veja o bot tomar o bunker.",
    all = "Tome todas as bases inimigas: %d de 8 são suas.",
    b_build = {
      keyboard = "Pressione o botão de bunker ou {QUICK_PILL},\ndepois clique no quadrado verde.",
      touch = "Toque no botão de bunker,\ndepois toque no quadrado verde.",
      controller = "Escolha bunker: pressione {BUILD_TOOL}. Ponha o\ncursor no verde e pressione {BUILD_PLACE}.",
    },
    b_hide = "Fique parado no fundo da floresta, no alcance.\nEscondido: %d de 5 segundos.",
    b_in = "Entre na floresta dentro dos marcadores vermelhos.",
    b_kill = "Atire no bunker inimigo: pressione {FIRE}\naté destruí-lo.",
    b_park = "Pare no quadrado de estrada destacado\ne mire no bunker inimigo.",
    b_pick = "Passe por cima do bunker destruído para pegá-lo.",
    b_take = "Passe por cima da base neutra para torná-la sua.",
    base = "Pressione {FIRE} para esvaziar uma base\ninimiga a tiros, depois passe por cima dela.",
    build = "Construa seus bunkers perto da fronteira, na terra\ndo bot (nordeste), para tomar o controle.",
    c_kill = "Pressione {FIRE} para atirar\nduas vezes no bunker.",
    c_pick = "Passe por cima do bunker destruído para pegá-lo.",
    c_shoot = "Atire na base com marca vermelha:\npressione {FIRE}. Restam %d tiros.",
    c_take = "A base está vazia. Passe por cima para torná-la sua.",
    dead = "Passe por cima dos dois bunkers destruídos\nna estrada de entrada.",
    kill = "Ponha a mira nas crateras e atire\nquando o construtor as atravessar.",
    shoot = "Atire no muro marcado: pressione {FIRE}\naté ele cair.",
  },
  line = {
    done = "Estação concluída. Siga a estrada para o norte.",
    gun = "Pressione {GUN_UP} até a mira\nficar no alcance máximo.",
    shells = "Poucos projéteis? Pare na base\nao lado da estrada para reabastecer.",
    win = "Tutorial concluído. Saia pelo\nmenu quando estiver pronto.",
  },
  info = {
    bases = "Bases suas: %d/8",
    enemy_base = "Base inimiga: faltam %d",
    men = "Construtores mortos: %d",
    needed = "%d de %d necessários",
    on = "Em %s: vel. %d",
    on_plain = "Em %s",
    pill_armour = "Blindagem bunker: %d/%d",
    wall_down = "Muro alvo: derrubado",
    wall_left = "Muro alvo: faltam %d",
  },
  hint = {
    full = "Cheio",
    low = "Pouca munição: recarregue",
    refill = "Reabastecendo: espere",
    reset = "Cercado RESET: recomeçar",
    stop = "PARE na base: espere",
  },
  terrain = {
    boat = "barco",
    building = "prédio",
    crater = "cratera",
    ["deep sea"] = "mar profundo",
    forest = "floresta",
    grass = "grama",
    river = "rio",
    road = "estrada",
    rubble = "escombros",
    swamp = "pântano",
    wall = "muro",
  },
  say = {
    back = "O bunker inimigo voltou.\nTome-o de novo ou siga para o norte.",
    done = "Estação %d concluída. Siga a estrada para o norte.",
    done4 = "Estação %d concluída. Siga a estrada para o norte.\nAtire nas árvores para limpá-las rápido.",
    moved = "Movido para o quadrado verde.",
    reset = "Estação %d reiniciada",
  },
}

-- Russian
TEXT["ru"] = {
  pop = {
    s1 = {
      keyboard = {
        "Нажмите {ACCEL}, чтобы ехать вперёд, {BRAKE}, чтобы замедлиться, {LEFT} и {RIGHT}, чтобы поворачивать.\n\nУчастки у дороги: лес, болото, развалины, кратеры и мелкая река. Каждый замедляет по-своему. Попробуйте проехать по нескольким.",
        "Глубокая вода топит танки. Если вы погибнете здесь, то вернётесь на последнюю контрольную точку.",
      },
      touch = {
        "Управляйте стиком в левой части экрана: толкните его туда, куда хотите ехать. Чем дальше толкнёте, тем быстрее поедете.\n\nУчастки у дороги: лес, болото, развалины, кратеры и мелкая река. Каждый замедляет по-своему. Попробуйте проехать по нескольким.",
        "Глубокая вода топит танки. Если вы погибнете здесь, то вернётесь на последнюю контрольную точку.",
      },
    },
    s2a = "Базы пополняют снаряды, мины и броню, а у вашего танка их мало.\n\nВъезжайте на свою базу у зелёного маркера. Затем ОСТАНОВИТЕСЬ на базе и ждите пополнения.",
    s2b = "Нейтральные базы не красные и не зелёные. Чтобы захватить такую базу, просто проезжайте по ней.",
    s2c = "Вражескую базу сначала надо опустошить. Полная выдерживает %d выстрелов: наведитесь на неё и нажмите {FIRE}, чтобы стрелять. Затем проезжайте по ней, и она ваша.",
    s2d = {
      "С чем возрождается танк, зависит от типа игры.\n\nОткрытая: танки всегда возрождаются с полным запасом.\n\nТурнир: чем больше баз захвачено, тем меньше снарядов при возрождении. Когда все базы захвачены, танки возрождаются без снарядов.",
      "Строгий турнир: танки никогда не возрождаются со снарядами.\n\nВ турнирных режимах ясное условие победы: команда, захватившая все базы, побеждает, ведь врагу негде взять боеприпасы.",
    },
    s2stay = "Это ваша база. Стойте на ней и не двигайтесь: снаряды, мины и броня поступают понемногу.",
    s3 = {
      keyboard = {
        "Ваш строитель рубит деревья и строит из них дороги, стены, мины и бункеры. Щёлкните кнопку постройки или нажмите её клавишу: {QUICK_TREE} деревья, {QUICK_ROAD} дорога, {QUICK_WALL} стена, {QUICK_MINE} мина. Затем щёлкните по карте, чтобы отправить туда строителя.",
        "Совет: когда выбран инструмент дороги, стены или бункера, щёлкните по дереву, и строитель его срубит. Не нужно каждый раз переключаться на инструмент деревьев.",
      },
      touch = {
        "Ваш строитель рубит деревья и строит из них дороги, стены, мины и бункеры. Коснитесь кнопки постройки: деревья, дорога, стена или мина. Затем коснитесь карты, чтобы отправить туда строителя.",
        "Совет: когда выбран инструмент дороги, стены или бункера, коснитесь дерева, и строитель его срубит. Не нужно каждый раз переключаться на инструмент деревьев.",
      },
      controller = {
        "Ваш строитель рубит деревья и строит из них дороги, стены, мины и бункеры. Нажмите {BUILD_MODE}, чтобы включить курсор постройки, и наводите его правым стиком. Нажмите {BUILD_TOOL}, чтобы выбрать инструмент: деревья, дорога, стена или мина.",
        "Затем нажмите {BUILD_PLACE}, чтобы отправить строителя к курсору.\n\nСовет: когда выбран инструмент дороги, стены или бункера, наведите курсор постройки на дерево и нажмите {BUILD_PLACE}, и строитель его срубит. Не нужно каждый раз переключаться на инструмент деревьев.",
      },
    },
    s4a = {
      keyboard = {
        "Игра начинается с нейтральными бункерами; полный бункер выдерживает 15 выстрелов.",
        "Сначала проезжайте по разрушенному бункеру, чтобы погрузить его в танк. Затем нажмите кнопку постройки бункера или её клавишу {QUICK_PILL}. Затем щёлкните по карте, и строитель построит там бункер, верный вам.",
      },
      touch = "Игра начинается с нейтральными бункерами; полный бункер выдерживает 15 выстрелов.\n\nСначала проезжайте по разрушенному бункеру, чтобы погрузить его в танк. Затем коснитесь кнопки постройки бункера и коснитесь карты, и строитель построит там бункер, верный вам.",
      controller = {
        "Игра начинается с нейтральными бункерами; полный бункер выдерживает 15 выстрелов.",
        "Сначала проезжайте по разрушенному бункеру, чтобы погрузить его в танк. Затем нажимайте {BUILD_TOOL}, пока не выберете инструмент бункера, наведите курсор постройки на нужное место и нажмите {BUILD_PLACE}. Строитель выйдет и построит там бункер, верный вам.",
      },
    },
    s4b = {
      "К востоку от дороги, в кольце леса, стоит повреждённый бункер. Он всё ещё стреляет, а бункер, по которому вы попали, злится и стреляет чаще. Красные маркеры показывают его дальность.",
      "Лес скрывает танки от других танков и от бункеров. Въезжайте в лес внутри маркеров и стойте неподвижно. Стрельба выдаёт вашу позицию.",
    },
    s4c = "Этот бункер уже сильно повреждён: ещё два выстрела его уничтожат.\n\nВаш танк полон. Уничтожьте бункер, затем проезжайте по нему, чтобы забрать.",
    s5a = "Остановитесь на зелёной клетке впереди. Смотрите, как союзный бот на острове за стеной захватывает бункер. Он строит свой бункер вплотную к вражескому и встаёт за ним, так что враг попадает в его бункер, а не в танк. Затем он уничтожает вражеский бункер и проезжает по нему.",
    s5b = {
      "Ваша очередь: у вас есть бункер. Постройте его на зелёной клетке, рядом с их бункером. Затем встаньте на выделенную клетку дороги и стреляйте по вражескому бункеру, пока он не падёт. Ваш бункер принимает его выстрелы и отстраивается. Начать сначала: въезжайте в загон RESET.",
      "Начинайте с полной бронёй. Танк может получить немного урона, когда вражеский бункер пробьёт ваш бункер-заслон. Это нормально.",
    },
    s6 = {
      "Убейте вражеского строителя, и его танк не сможет строить, пока не прибудет новый. Задавить строителя нельзя: его убивают только снаряды, мины и взрывы.",
      "Снаряд взрывается в точке прицела и убивает строителя в пределах полклетки. Нажмите {GUN_DOWN}, чтобы приблизить прицел, и {GUN_UP}, чтобы отдалить. В этом обучении бот отправляет строителя через кратеры. Наведите туда прицел и стреляйте, пока он идёт.",
    },
    s6gun = "Попали! Теперь нажимайте {GUN_UP}, пока прицел не вернётся на максимальную дальность, для следующей станции.",
    s7 = {
      "Финальный раунд против лёгкого бота. У вас три угла острова, у бота северо-восток. Займите его земли бункерами, затем захватите его базы, как на станции 2. Вы победите, когда все базы будут ваши. Чтобы начать сначала, въезжайте в загон RESET.",
      "Сначала проезжайте по двум разрушенным бункерам на въездной дороге, чтобы погрузить их в танк. Ставьте бункеры у границы между вашими и его землями, чтобы взять контроль.",
    },
    welcome = {
      "Добро пожаловать в WinBolo! Bolo — сетевая танковая игра, которую Stuart Cheshire написал в 1987 году, и WinBolo продолжает её сегодня. Вы ездите на танке по острову с бункерами (пушечными башнями, которые стреляют по вражеским танкам) и базами, которые пополняют снаряды, мины и броню.",
      "В танке едет маленький строитель: он выходит рубить деревья и строить для вас. Игра идёт командами, и команда, у которой бункеры и базы, владеет островом.",
    },
    win = "Отлично: все базы ваши, обучение пройдено.\n\nЕздите сколько хотите. Когда будете готовы, выйдите через меню и попробуйте игру против лёгких ботов.",
  },
  title = {
    [1] = "Станция 1: Езда",
    [2] = "Станция 2: Базы",
    [3] = "Станция 3: Стройка",
    [4] = "Станция 4: Бункеры",
    [5] = "Станция 5: Захват",
    [6] = "Станция 6: Строитель",
    [7] = "Станция 7: Финал",
  },
  list = {
    a_build = "Постройте его",
    a_pick = "Заберите бункер",
    a_refill = "Пополнитесь на базе",
    a_watch = "Смотрите на бота",
    all = "Захватите все базы",
    b_build = "Стройте на зелёной",
    b_hide = "Прячьтесь в зоне",
    b_in = "Въезжайте в лес",
    b_kill = "Убейте бункер",
    b_park = "Встаньте на дороге",
    b_pick = "Заберите его",
    b_take = "Займите ничью базу",
    base = "Захватите базу врага",
    build = "Бункер на земле врага",
    c_kill = "Убейте бункер",
    c_pick = "Заберите бункер",
    c_shoot = "Опустошите базу",
    c_take = "Проезжайте по ней",
    crater = "Кратеры",
    dead = "Заберите оба бункера",
    forest = "Лес",
    harvest = "Срубите дерево",
    kill = "Убейте строителя",
    mine = "Поставьте мину",
    river = "Мелкая река",
    road = "Постройте дорогу",
    rubble = "Развалины",
    shoot = "Снесите стену",
    swamp = "Болото",
    wall = "Постройте стену",
  },
  step = {
    wall = {
      keyboard = "Стена: щёлкните кнопку стены\nили нажмите {QUICK_WALL}, затем траву.",
      touch = "Постройте стену: коснитесь\nкнопки стены, затем травы.",
      controller = "Стена: нажмите {BUILD_TOOL} (стена),\nнаведите на траву и нажмите {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Щёлкните кнопку дороги или нажмите\n{QUICK_ROAD}, затем щёлкните по траве.",
      touch = "Коснитесь кнопки дороги, затем травы.",
      controller = "Нажмите {BUILD_TOOL} (дорога), наведите\nкурсор на траву и нажмите {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Щёлкните кнопку мины или нажмите\n{QUICK_MINE}, затем щёлкните клетку.",
      touch = "Коснитесь кнопки мины,\nзатем клетки, чтобы поставить мину.",
      controller = "Нажмите {BUILD_TOOL} (мина), наведите\nкурсор на клетку и нажмите {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Нажмите кнопку деревьев или {QUICK_TREE},\nзатем щёлкните отмеченную рощу.",
      touch = "Коснитесь кнопки деревьев,\nзатем леса в отмеченной роще.",
      controller = "Нажмите {BUILD_TOOL} (деревья), наведите\nкурсор на рощу и нажмите {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Нажмите кнопку бункера или {QUICK_PILL},\nзатем щёлкните по карте.",
      touch = "Коснитесь кнопки бункера,\nзатем карты, чтобы построить его.",
      controller = "Нажмите {BUILD_TOOL} (бункер), наведите\nкурсор и нажмите {BUILD_PLACE}.",
    },
    a_pick = "Проезжайте по разрушенному\nбункеру, чтобы погрузить его.",
    a_refill = "Въезжайте на базу с зелёным маркером,\nСТОЙТЕ на ней и ждите.",
    a_watch = "Встаньте на зелёной клетке\nи смотрите, как бот берёт бункер.",
    all = "Захватите все базы врага:\nваших %d из 8.",
    b_build = {
      keyboard = "Нажмите кнопку бункера или {QUICK_PILL},\nзатем щёлкните зелёную клетку.",
      touch = "Коснитесь кнопки бункера,\nзатем зелёной клетки.",
      controller = "Нажмите {BUILD_TOOL} (бункер), наведите\nна зелёную клетку и нажмите {BUILD_PLACE}.",
    },
    b_hide = "Стойте в глубине леса, в зоне огня.\nСкрыты: %d из 5 сек.",
    b_in = "Въезжайте в лес внутри\nкрасных маркеров.",
    b_kill = "Нажимайте {FIRE} и стреляйте\nпо бункеру, пока он не падёт.",
    b_park = "Встаньте на выделенной клетке\nдороги и цельтесь в бункер.",
    b_pick = "Проезжайте по разрушенному\nбункеру, чтобы забрать его.",
    b_take = "Въезжайте на нейтральную\nбазу, чтобы её захватить.",
    base = "Нажимайте {FIRE} и опустошите\nбазу врага, затем въезжайте.",
    build = "Ставьте бункеры у границы,\nна земле бота (северо-восток).",
    c_kill = "Нажмите {FIRE} и попадите\nв бункер дважды.",
    c_pick = "Проезжайте по разрушенному\nбункеру, чтобы забрать его.",
    c_shoot = "Нажимайте {FIRE} и стреляйте\nпо красной базе. Осталось: %d.",
    c_take = "База пуста. Въезжайте\nна неё, чтобы захватить.",
    dead = "Проезжайте по обоим бункерам\nна въездной дороге.",
    kill = "Наведите прицел на кратеры и\nстреляйте, когда строитель идёт.",
    shoot = "Нажимайте {FIRE} и стреляйте\nпо стене, пока она не рухнет.",
  },
  line = {
    done = "Станция пройдена.\nЕзжайте по дороге на север.",
    gun = "Нажимайте {GUN_UP}, пока прицел\nне выйдет на макс. дальность.",
    shells = "Мало снарядов? Встаньте\nна базу у дороги.",
    win = "Обучение пройдено. Выйдите\nчерез меню, когда будете готовы.",
  },
  info = {
    bases = "Ваши базы: %d/8",
    enemy_base = "База врага: ещё %d",
    men = "Убито: %d",
    needed = "Нужно: %d из %d",
    on = "%s: скор. %d",
    on_plain = "%s",
    pill_armour = "Броня бункера: %d/%d",
    wall_down = "Стена: снесена",
    wall_left = "Стена: ещё %d",
  },
  hint = {
    full = "Полно",
    low = "Мало снарядов",
    refill = "Пополнение: ждите",
    reset = "Загон RESET: заново",
    stop = "СТОП на базе: ждите",
  },
  terrain = {
    boat = "Лодка",
    building = "Здание",
    crater = "Кратер",
    ["deep sea"] = "Открытое море",
    forest = "Лес",
    grass = "Трава",
    river = "Река",
    road = "Дорога",
    rubble = "Развалины",
    swamp = "Болото",
    wall = "Стена",
  },
  say = {
    back = "Вражеский бункер вернулся.\nЗахватите снова или езжайте на север.",
    done = "Станция %d пройдена.\nЕзжайте по дороге на север.",
    done4 = "Станция %d пройдена. На север!\nСтреляйте по деревьям, чтобы убрать.",
    moved = "Вы на зелёной клетке.",
    reset = "Станция %d заново",
  },
}

-- Swedish
TEXT["sv"] = {
  pop = {
    s1 = {
      keyboard = "Tryck på {ACCEL} för att köra framåt, {BRAKE} för att sakta ned och {LEFT} eller {RIGHT} för att svänga.\n\nFälten bredvid vägen är skog, träsk, bråte, kratrar och en grund flod. Varje sorts mark bromsar dig olika mycket. Prova att köra på några av dem.\n\nDjupt vatten sänker tankar. Om du dör någonstans här kommer du tillbaka vid din senaste kontrollpunkt.",
      touch = "Kör med tumspaken till vänster på skärmen: skjut den åt det håll du vill åka. Ju längre du skjuter den, desto snabbare åker du.\n\nFälten bredvid vägen är skog, träsk, bråte, kratrar och en grund flod. Varje sorts mark bromsar dig olika mycket. Prova att köra på några av dem.\n\nDjupt vatten sänker tankar. Om du dör någonstans här kommer du tillbaka vid din senaste kontrollpunkt.",
    },
    s2a = "Baser fyller på dina granater, minor och ditt pansar, och din tank har lite kvar.\n\nKör upp på din bas vid den gröna markeringen. STANNA sedan på basen och vänta medan den fyller på.",
    s2b = "Neutrala baser är varken röda eller gröna. Du tar dem genom att köra över dem.",
    s2c = "Fiendebaser måste först skjutas tomma. En full bas kräver %d skott: sikta på den och tryck på {FIRE} för att skjuta. Kör sedan över den för att göra den till din.",
    s2d = "Vad din tank startar med beror på speltypen.\n\nÖppet: tankar startar alltid fullt lastade.\n\nTurnering: ju fler baser som är tagna, desto färre granater får en ny tank. När alla baser är tagna startar tankar utan granater.\n\nStrikt turnering: tankar startar aldrig med granater.\n\nTurneringslägen har ett tydligt mål: när ett lag äger alla baser vinner det, eftersom fiendelaget inte kan få någon ammunition.",
    s2stay = "Det här är din bas. Stanna på den och rör dig inte: dina granater, minor och ditt pansar fylls på lite i taget.",
    s3 = {
      keyboard = "Din man hugger träd och bygger sedan vägar, murar, minor och bunkrar av dem. Klicka på en byggknapp eller tryck på dess tangent: {QUICK_TREE} träd, {QUICK_ROAD} väg, {QUICK_WALL} mur, {QUICK_MINE} mina. Klicka sedan på kartan för att skicka dit din man.\n\nTips: när väg-, mur- eller bunkerverktyget är valt kan du klicka på ett träd, så hugger din man det. Du behöver inte byta till trädverktyget varje gång.",
      touch = "Din man hugger träd och bygger sedan vägar, murar, minor och bunkrar av dem. Tryck på en byggknapp: träd, väg, mur eller mina. Tryck sedan på kartan för att skicka dit din man.\n\nTips: när väg-, mur- eller bunkerverktyget är valt kan du trycka på ett träd, så hugger din man det. Du behöver inte byta till trädverktyget varje gång.",
      controller = "Din man hugger träd och bygger sedan vägar, murar, minor och bunkrar av dem. Dra i {BUILD_MODE} för byggmarkören och rikta den med höger spak. Tryck på {BUILD_TOOL} för att välja verktyg: träd, väg, mur eller mina. Tryck sedan på {BUILD_PLACE} för att skicka din man till markören.\n\nTips: när väg-, mur- eller bunkerverktyget är valt kan du sätta byggmarkören på ett träd och trycka på {BUILD_PLACE}, så hugger din man det. Du behöver inte byta till trädverktyget varje gång.",
    },
    s4a = {
      keyboard = "Spel börjar med neutrala bunkrar, och en full bunker kräver 15 skott för att förstöras.\n\nKör först över den förstörda bunkern för att lasta den i din tank. Tryck sedan på bunkerns byggknapp eller dess snabbtangent {QUICK_PILL}. Klicka sedan på kartan för att skicka ut din man, som bygger en bunker som är lojal mot dig på den platsen.",
      touch = "Spel börjar med neutrala bunkrar, och en full bunker kräver 15 skott för att förstöras.\n\nKör först över den förstörda bunkern för att lasta den i din tank. Tryck sedan på bunkerns byggknapp och tryck på kartan för att skicka ut din man, som bygger en bunker som är lojal mot dig på den platsen.",
      controller = "Spel börjar med neutrala bunkrar, och en full bunker kräver 15 skott för att förstöras.\n\nKör först över den förstörda bunkern för att lasta den i din tank. Tryck sedan på {BUILD_TOOL} tills bunkerverktyget är valt, sätt byggmarkören där du vill ha den och tryck på {BUILD_PLACE}. Din man går ut och bygger en bunker som är lojal mot dig där.",
    },
    s4b = "Öster om vägen står en skadad bunker i en ring av skog. Den skjuter fortfarande, och en bunker som du träffar blir arg och skjuter snabbare. De röda markeringarna visar dess räckvidd.\n\nSkog döljer tankar för både andra tankar och bunkrar. Kör in i skogen innanför markeringarna och stå still. Att skjuta avslöjar var du är.",
    s4c = "Den här bunkern är redan svårt skadad: två skott till förstör den.\n\nDin tank är full. Förstör bunkern och kör sedan över den för att plocka upp den.",
    s5a = "Stanna på den gröna rutan framför dig. Se hur den vänliga boten på den inmurade ön tar en bunker. Den bygger en egen bunker precis bredvid fiendens och parkerar bakom den. Då träffar fiendens skott dess bunker, inte dess tank. Sedan skjuter den sönder fiendens bunker och kör över den.",
    s5b = {
      "Din tur: du bär en bunker. Bygg din bunker på den markerade gröna rutan, precis bredvid deras. Stanna sedan på den markerade vägrutan och sikta på fiendens bunker: skjut tills den är förstörd. Din bunker tar dess skott och byggs upp igen om den förstörs. Kör in i RESET-fållan för att börja om.",
      "Börja med fullt pansar. Observera att det är normalt att tanken tar lite skada efter att fiendens bunker har skjutit sönder din skyddsbunker.",
    },
    s6 = {
      "Döda en fiendes man, så kan hans tank inte bygga förrän en ny man kommer. Du kan inte köra över en man: bara granater, minor och explosioner dödar honom.",
      "En granat exploderar vid ditt sikte och dödar en man inom en halv ruta. Tryck på {GUN_DOWN} för att flytta siktet närmare och tryck på {GUN_UP} för att flytta det längre bort. I den här tutorialen skickar en bot sin man över kratrarna. Sätt ditt sikte där och fortsätt skjuta medan han går över.",
    },
    s6gun = "Träff! Tryck nu på {GUN_UP} tills ditt sikte är tillbaka på längsta räckvidd. Då är du redo för nästa station.",
    s7 = {
      "Sista rundan, mot en lätt bot. Du har tre hörn av ön och boten har nordost. Ta dess land med dina bunkrar och stjäl sedan dess baser som i Station 2. Du vinner när du äger alla baser. Kör in i RESET-fållan för att börja om.",
      "Kör först över de två förstörda bunkrarna på infartsvägen för att lasta dem i din tank. Bygg dina bunkrar nära gränsen mellan ditt land och botens land för att ta kontrollen.",
    },
    welcome = {
      "Välkommen till WinBolo! Bolo är ett tankspel för flera spelare som Stuart Cheshire först skrev 1987, och WinBolo för det vidare i dag. Du kör en tank runt en ö med bunkrar och baser. Bunkrar är kanontorn som skjuter på fiendens tankar. Baser fyller på dina granater, minor och ditt pansar.",
      "En liten man åker med i din tank och går ut för att hugga träd och bygga åt dig. Du spelar i lag, och laget som håller bunkrarna och baserna håller ön.",
    },
    win = "Bra gjort: du äger alla baser, och tutorialen är klar.\n\nKör runt så länge du vill. Lämna spelet via menyn när du är redo, och prova sedan ett spel mot lätta bottar.",
  },
  title = {
    [1] = "Station 1: Köra",
    [2] = "Station 2: Baser",
    [3] = "Station 3: Bygga",
    [4] = "Station 4: Bunkrar",
    [5] = "Station 5: Ta en bunker",
    [6] = "Station 6: Mannen",
    [7] = "Station 7: Sista rundan",
  },
  list = {
    a_build = "Bygg den",
    a_pick = "Ta upp den döda bunkern",
    a_refill = "Fyll på vid din bas",
    a_watch = "Se boten ta en bunker",
    all = "Ta alla fiendebaser",
    b_build = "Bygg på gröna rutan",
    b_hide = "Göm dig inom räckvidd",
    b_in = "Kör in i skogen i räckvidd",
    b_kill = "Förstör fiendebunkern",
    b_park = "Parkera på vägrutan",
    b_pick = "Plocka upp den",
    b_take = "Ta den neutrala basen",
    base = "Stjäl en fiendebas",
    build = "Bunker i fiendeland",
    c_kill = "Förstör bunkern",
    c_pick = "Plocka upp bunkern",
    c_shoot = "Skjut fiendebasen tom",
    c_take = "Kör över den",
    crater = "Kratrar",
    dead = "Ta upp båda döda bunkrar",
    forest = "Skog",
    harvest = "Hugg ett träd",
    kill = "Skjut mannen",
    mine = "Lägg ut en mina",
    river = "Grund flod",
    road = "Bygg en väg",
    rubble = "Bråte",
    shoot = "Skjut ner muren",
    swamp = "Träsk",
    wall = "Bygg en mur",
  },
  step = {
    wall = {
      keyboard = "Bygg en mur: klicka på murknappen eller\ntryck på {QUICK_WALL}, klicka sedan på en gräsruta.",
      touch = "Bygg en mur: tryck på murknappen\noch sedan på en gräsruta.",
      controller = "Bygg en mur: tryck på {BUILD_TOOL} för muren,\nsätt markören på gräs och tryck på {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Tryck på {QUICK_ROAD} eller klicka på vägknappen,\nklicka sedan på en gräsruta.",
      touch = "Tryck på vägknappen och sedan på en gräsruta.",
      controller = "Tryck på {BUILD_TOOL} för vägverktyget, sätt\nmarkören på gräs och tryck på {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Tryck på {QUICK_MINE} eller klicka på minknappen,\nklicka sedan på en ruta för att lägga ut en mina.",
      touch = "Tryck på minknappen och sedan på\nen ruta för att lägga ut en mina.",
      controller = "Tryck på {BUILD_TOOL} för minverktyget, sätt\nmarkören på en ruta och tryck på {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Tryck på {QUICK_TREE} eller klicka på trädknappen,\nklicka sedan på en skogsruta i den markerade dungen.",
      touch = "Tryck på trädknappen och sedan på en\nskogsruta i den markerade dungen.",
      controller = "Tryck på {BUILD_TOOL} för trädverktyget, sätt\nmarkören på dungen och tryck på {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Tryck på bunkerknappen eller {QUICK_PILL},\nklicka sedan på kartan för att bygga den.",
      touch = "Tryck på bunkerknappen och sedan\npå kartan för att bygga den.",
      controller = "Tryck på {BUILD_TOOL} för bunkerverktyget, sätt ut\nmarkören och tryck på {BUILD_PLACE}.",
    },
    a_pick = "Kör över den förstörda bunkern för\natt lasta den i din tank.",
    a_refill = "Kör upp på den grönmarkerade basen, STANNA sedan\npå den och vänta medan den fyller på.",
    a_watch = "Stanna på den gröna rutan framför dig\noch se boten ta bunkern.",
    all = "Ta alla fiendebaser: %d av 8 är dina.",
    b_build = {
      keyboard = "Tryck på bunkerknappen eller {QUICK_PILL},\nklicka sedan på den gröna rutan.",
      touch = "Tryck på bunkerknappen och\nsedan på den gröna rutan.",
      controller = "Tryck på {BUILD_TOOL} för bunkerverktyget, sätt\nmarkören på den gröna rutan och tryck på {BUILD_PLACE}.",
    },
    b_hide = "Stå still djupt inne i skogen, inom räckvidd.\nDold: %d av 5 sekunder.",
    b_in = "Kör in i skogen innanför\nde röda markeringarna.",
    b_kill = "Tryck på {FIRE} och skjut på fiendens\nbunker tills den är förstörd.",
    b_park = "Stanna på den markerade vägrutan\noch sikta på fiendens bunker.",
    b_pick = "Kör över den förstörda bunkern\nför att plocka upp den.",
    b_take = "Kör upp på den neutrala basen\nför att göra den till din.",
    base = "Skjut en fiendebas tom: tryck på {FIRE},\nkör sedan upp på den.",
    build = "Bygg dina bunkrar nära gränsen, i botens land\n(nordost), för att ta kontrollen.",
    c_kill = "Tryck på {FIRE} två gånger för\natt skjuta på bunkern.",
    c_pick = "Kör över den förstörda bunkern\nför att plocka upp den.",
    c_shoot = "Skjut på den rödmarkerade basen: tryck på\n{FIRE}. Skott kvar: %d.",
    c_take = "Basen är tom. Kör upp på den\nför att göra den till din.",
    dead = "Kör över båda förstörda bunkrarna\npå infartsvägen.",
    kill = "Sätt siktet på kratrarna och skjut\nnär mannen går över dem.",
    shoot = "Skjut på den markerade muren: tryck på\n{FIRE} tills den faller.",
  },
  line = {
    done = "Stationen klar. Följ vägen norrut.",
    gun = "Tryck på {GUN_UP} tills ditt sikte\när på längsta räckvidd.",
    shells = "Lite granater? Stanna på basen\nvid vägen för att fylla på.",
    win = "Tutorialen är klar. Lämna spelet via\nmenyn när du är redo.",
  },
  info = {
    bases = "Dina baser: %d/8",
    enemy_base = "Fiendebas: %d skott kvar",
    men = "Dödade män: %d",
    needed = "%d av %d krävs",
    on = "%s: fart %d",
    on_plain = "Terräng: %s",
    pill_armour = "Bunkerpansar: %d/%d",
    wall_down = "Målmur: nere",
    wall_left = "Målmur: %d skott kvar",
  },
  hint = {
    full = "Fullt",
    low = "Lite granater: fyll på",
    refill = "Fyller på: vänta här",
    reset = "RESET-fållan: börja om",
    stop = "STANNA på basen: vänta",
  },
  terrain = {
    boat = "Båt",
    building = "Byggnad",
    crater = "Krater",
    ["deep sea"] = "Djuphav",
    forest = "Skog",
    grass = "Gräs",
    river = "Flod",
    road = "Väg",
    rubble = "Bråte",
    swamp = "Träsk",
    wall = "Mur",
  },
  say = {
    back = "Fiendens bunker är tillbaka.\nTa den igen, eller kör norrut.",
    done = "Station %d klar. Följ vägen norrut.",
    done4 = "Station %d klar. Följ vägen norrut.\nSkjut på träd för att snabbt röja dem.",
    moved = "Flyttad till den gröna rutan.",
    reset = "Station %d återställd",
  },
}

-- Turkish
TEXT["tr"] = {
  pop = {
    s1 = {
      keyboard = "İleri gitmek için {ACCEL}, yavaşlamak için {BRAKE}, dönmek için {LEFT} ve {RIGHT} basın.\n\nYolun yanındaki alanlar orman, bataklık, moloz, krater ve sığ bir nehirdir. Her biri sizi farklı ölçüde yavaşlatır. Birkaçının üzerinde sürmeyi deneyin.\n\nDerin su tankları batırır. Burada herhangi bir yerde ölürseniz son kontrol noktanızda yeniden başlarsınız.",
      touch = "Ekranın solundaki joystick ile sürün: gitmek istediğiniz yöne itin. Ne kadar çok iterseniz o kadar hızlı gidersiniz.\n\nYolun yanındaki alanlar orman, bataklık, moloz, krater ve sığ bir nehirdir. Her biri sizi farklı ölçüde yavaşlatır. Birkaçının üzerinde sürmeyi deneyin.\n\nDerin su tankları batırır. Burada herhangi bir yerde ölürseniz son kontrol noktanızda yeniden başlarsınız.",
    },
    s2a = "Üsler mermilerinizi, mayınlarınızı ve zırhınızı doldurur; tankınızda bunlar azaldı.\n\nYeşil işaretteki üssünüze sürün. Sonra üssün üzerinde DURUN ve dolarken bekleyin.",
    s2b = "Tarafsız üslerde kırmızı ya da yeşil yoktur; üzerlerinden geçerek ele geçirilirler.",
    s2c = "Düşman üsleri önce ateş edilerek boşaltılmalıdır. Dolu bir üs %d atış ister: ona nişan alın ve ateş etmek için {FIRE} basın. Sonra sizin olması için üzerinden geçin.",
    s2d = "Tankınızın neyle yeniden doğduğu oyun türüne bağlıdır.\n\nAçık: tanklar her zaman tam dolu doğar.\n\nTurnuva: ne kadar çok üs alınmışsa tank o kadar az mermiyle doğar; tüm üsler alınınca tanklar mermisiz doğar.\n\nSıkı Turnuva: tanklar asla mermiyle doğmaz.\n\nTurnuva modlarında net bir kazanma koşulu vardır: bir takım tüm üslere sahip olunca kazanır, çünkü düşman takım hiç cephane alamaz.",
    s2stay = "Bu sizin üssünüz. Üzerinde kalın ve kıpırdamayın: mermileriniz, mayınlarınız ve zırhınız azar azar dolar.",
    s3 = {
      keyboard = "İnşaatçınız ağaç toplar, sonra bunlarla yol, duvar, mayın ve bunker yapar. Bir inşa düğmesine tıklayın ya da tuşuna basın: ağaç için {QUICK_TREE}, yol için {QUICK_ROAD}, duvar için {QUICK_WALL}, mayın için {QUICK_MINE}. Sonra inşaatçınızı oraya göndermek için haritaya tıklayın.\n\nİpucu: yol, duvar ya da bunker aracı seçiliyken bir ağaca tıklarsanız inşaatçınız onu toplar. Her seferinde ağaç aracına geçmeniz gerekmez.",
      touch = "İnşaatçınız ağaç toplar, sonra bunlarla yol, duvar, mayın ve bunker yapar. Bir inşa düğmesine dokunun: ağaç, yol, duvar ya da mayın. Sonra inşaatçınızı oraya göndermek için haritaya dokunun.\n\nİpucu: yol, duvar ya da bunker aracı seçiliyken bir ağaca dokunursanız inşaatçınız onu toplar. Her seferinde ağaç aracına geçmeniz gerekmez.",
      controller = "İnşaatçınız ağaç toplar, sonra bunlarla yol, duvar, mayın ve bunker yapar. İnşa imleci için {BUILD_MODE} basın ve imleci sağ çubukla yöneltin. Araç seçmek için {BUILD_TOOL} basın: ağaç, yol, duvar ya da mayın. Sonra inşaatçınızı imlece göndermek için {BUILD_PLACE} basın.\n\nİpucu: yol, duvar ya da bunker aracı seçiliyken inşa imlecini bir ağaca koyup {BUILD_PLACE} basarsanız inşaatçınız onu toplar. Her seferinde ağaç aracına geçmeniz gerekmez.",
    },
    s4a = {
      keyboard = "Oyunlar tarafsız bunkerlerle başlar ve dolu bir bunkeri yok etmek 15 atış ister.\n\nÖnce ölü bunkerin üzerinden geçerek onu tankınıza alın. Sonra bunker inşa düğmesine ya da kısayol olarak {QUICK_PILL} basın. Ardından haritaya tıklayın; inşaatçınız o noktaya gidip size bağlı bir bunker kurar.",
      touch = "Oyunlar tarafsız bunkerlerle başlar ve dolu bir bunkeri yok etmek 15 atış ister.\n\nÖnce ölü bunkerin üzerinden geçerek onu tankınıza alın. Sonra bunker inşa düğmesine dokunun ve haritaya dokunun; inşaatçınız o noktaya gidip size bağlı bir bunker kurar.",
      controller = "Oyunlar tarafsız bunkerlerle başlar ve dolu bir bunkeri yok etmek 15 atış ister.\n\nÖnce ölü bunkerin üzerinden geçerek onu tankınıza alın. Sonra bunker aracı seçilene kadar {BUILD_TOOL} basın, inşa imlecini istediğiniz yere koyun ve {BUILD_PLACE} basın. İnşaatçınız çıkar ve orada size bağlı bir bunker kurar.",
    },
    s4b = "Yolun doğusunda, hasarlı bir bunker bir orman halkasının içinde duruyor. Hâlâ ateş ediyor ve vurduğunuz bir bunker öfkelenip daha hızlı ateş eder. Kırmızı işaretler menzilini gösterir.\n\nOrmanlar tankları hem diğer tanklardan hem de bunkerlerden gizler. İşaretlerin içindeki ormana girin ve kıpırdamadan durun. Ateş etmek yerinizi belli eder.",
    s4c = "Bu bunker zaten ağır hasarlı: iki atış daha onu yok eder.\n\nTankınız dolu. Onu yok edin, sonra almak için üzerinden geçin.",
    s5a = "İlerideki yeşil karede durun. Duvarlı adadaki dost botun bir bunkeri ele geçirmesini izleyin. Bot, kendi bunkerini düşmanınkinin hemen yanına kurar ve arkasına park eder; böylece düşmanın atışları tankına değil bunkerine isabet eder. Sonra düşman bunkerini vurarak yok eder ve üzerinden geçer.",
    s5b = "Sıra sizde: bir bunker taşıyorsunuz. Bunkerinizi işaretli yeşil kareye, onlarınkinin hemen yanına kurun. Sonra vurgulanan yol karesinde durun ve düşman bunkerine nişan alın: yok olana kadar ateş edin. Bunkeriniz onun atışlarını karşılar ve yok olursa yeniden kurulur. Baştan başlamak için RESET alanına girin.\n\nTam zırhla başlayın. Düşman bunkeri sizi koruyan bunkeri yok ettikten sonra tankınızın biraz hasar alması normaldir.",
    s6 = {
      "Bir düşman inşaatçısını öldürürseniz yeni bir inşaatçı gelene kadar tankı inşaat yapamaz. İnşaatçıyı ezemezsiniz: onu yalnızca mermiler, mayınlar ve patlamalar öldürür.",
      "Mermi nişanınızda patlar ve yarım kare içindeki bir inşaatçıyı öldürür. Nişanı yaklaştırmak için {GUN_DOWN}, uzaklaştırmak için {GUN_UP} basın. Bu eğitimde bir bot, inşaatçısını kraterlerin üzerinden geçirecek şekilde programlandı. Nişanınızı oraya koyun ve o geçerken ateş etmeye devam edin.",
    },
    s6gun = "Vurdunuz! Şimdi nişanınız en uzak menziline dönene kadar {GUN_UP} basın; sonraki istasyona hazır olun.",
    s7 = "Son tur, Kolay bir bota karşı. Adanın üç köşesi sizde, kuzeydoğu köşesi botta. Bunkerlerinizle onun toprağını alın, sonra 2. İstasyonda yaptığınız gibi üslerini çalın. Tüm üslere sahip olunca kazanırsınız. Baştan başlamak için RESET alanına girin.\n\nÖnce giriş yolundaki iki ölü bunkerin üzerinden geçerek onları tankınıza alın. Kontrolü ele geçirmek için bunkerlerinizi sizin toprağınızla onunki arasındaki sınırın yakınına kurun.",
    welcome = {
      "WinBolo'ya hoş geldiniz! Bolo, Stuart Cheshire'ın ilk kez 1987'de yazdığı çok oyunculu bir tank oyunudur ve WinBolo onu bugün sürdürüyor. Bir adada tank sürersiniz; adada bunkerler, yani düşman tanklarına ateş eden silah kuleleri, ve mermilerinizi, mayınlarınızı ve zırhınızı dolduran üsler vardır.",
      "Tankınızda küçük bir inşaatçı yolculuk eder ve sizin için ağaç kesip inşaat yapmak üzere dışarı çıkar. Takımlar halinde oynarsınız; bunkerleri ve üsleri tutan takım adayı tutar.",
    },
    win = "Tebrikler: tüm üsler sizin ve eğitim tamamlandı.\n\nİstediğiniz kadar dolaşın. Hazır olduğunuzda menüden çıkın, sonra Kolay botlara karşı bir oyun deneyin.",
  },
  title = {
    [1] = "İstasyon 1: Sürüş",
    [2] = "İstasyon 2: Üsler",
    [3] = "İstasyon 3: İnşaat",
    [4] = "İstasyon 4: Bunkerler",
    [5] = "İstasyon 5: Bunker almak",
    [6] = "İstasyon 6: İnşaatçı",
    [7] = "İstasyon 7: Son tur",
  },
  list = {
    a_build = "İnşa edin",
    a_pick = "Ölü bunkeri alın",
    a_refill = "Üssünüzde dolun",
    a_watch = "Botun almasını izleyin",
    all = "Tüm düşman üslerini alın",
    b_build = "Yeşil kareye kurun",
    b_hide = "Menzilde gizli bekleyin",
    b_in = "Ormandan menzile girin",
    b_kill = "Düşman bunkerini yok edin",
    b_park = "Yol karesinde durun",
    b_pick = "Onu alın",
    b_take = "Tarafsız üssü alın",
    base = "Bir düşman üssü çalın",
    build = "Düşman toprağına bunker",
    c_kill = "Bunkeri yok edin",
    c_pick = "Bunkeri alın",
    c_shoot = "Düşman üssünü boşaltın",
    c_take = "Üzerinden geçin",
    crater = "Kraterler",
    dead = "İki ölü bunkeri alın",
    forest = "Orman",
    harvest = "Ağaç toplayın",
    kill = "İnşaatçıyı vurun",
    mine = "Mayın döşeyin",
    river = "Sığ nehir",
    road = "Yol yapın",
    rubble = "Moloz",
    shoot = "Duvarı yıkın",
    swamp = "Bataklık",
    wall = "Duvar yapın",
  },
  step = {
    wall = {
      keyboard = "Duvar yapın: duvar düğmesine tıklayın veya\n{QUICK_WALL} basın, sonra çimene tıklayın.",
      touch = "Duvar yapın: duvar düğmesine dokunun,\nsonra bir çimen karesine dokunun.",
      controller = "Duvar yapın: duvar aracı için {BUILD_TOOL}\nbasın, imleci çimene koyup {BUILD_PLACE} basın.",
    },
    road = {
      keyboard = "Yol düğmesine tıklayın veya {QUICK_ROAD} basın,\nsonra bir çimen karesine tıklayın.",
      touch = "Yol düğmesine dokunun, sonra bir çimen karesine dokunun.",
      controller = "Yol aracı için {BUILD_TOOL} basın, imleci\nçimene koyup {BUILD_PLACE} basın.",
    },
    mine = {
      keyboard = "Mayın düğmesine tıklayın ya da {QUICK_MINE}\nbasın, sonra mayın döşemek için bir kareye tıklayın.",
      touch = "Mayın düğmesine dokunun, sonra mayın\ndöşemek için bir kareye dokunun.",
      controller = "Mayın aracı için {BUILD_TOOL} basın, imleci\nbir kareye koyup {BUILD_PLACE} basın.",
    },
    harvest = {
      keyboard = "Ağaç düğmesine tıklayın veya {QUICK_TREE} basın,\nsonra işaretli korudaki bir orman karesine tıklayın.",
      touch = "Ağaç düğmesine dokunun, sonra işaretli\nkorudaki bir orman karesine dokunun.",
      controller = "Ağaç aracı için {BUILD_TOOL} basın, imleci\nişaretli koruya koyup {BUILD_PLACE} basın.",
    },
    a_build = {
      keyboard = "Bunker düğmesine ya da {QUICK_PILL} basın,\nsonra kurmak için haritaya tıklayın.",
      touch = "Bunker düğmesine dokunun,\nsonra kurmak için haritaya dokunun.",
      controller = "Bunker aracı için {BUILD_TOOL} basın, imleci\nyerleştirip kurmak için {BUILD_PLACE} basın.",
    },
    a_pick = "Ölü bunkerin üzerinden geçerek onu tankınıza alın.",
    a_refill = "Yeşil işaretli üsse sürün, sonra üzerinde DURUN\nve dolarken bekleyin.",
    a_watch = "İlerideki yeşil karede durun ve\nbotun bunkeri almasını izleyin.",
    all = "Tüm düşman üslerini alın: 8 üsten %d tanesi sizin.",
    b_build = {
      keyboard = "Bunker düğmesine ya da {QUICK_PILL} basın,\nsonra yeşil kareye tıklayın.",
      touch = "Bunker düğmesine dokunun,\nsonra yeşil kareye dokunun.",
      controller = "Bunker aracı için {BUILD_TOOL} basın, imleci\nyeşil kareye koyup {BUILD_PLACE} basın.",
    },
    b_hide = "Ormanın derininde, menzil içinde kıpırdamayın.\nGizli: %d / 5 saniye.",
    b_in = "Kırmızı işaretlerin içindeki ormana girin.",
    b_kill = "Düşman bunkerini vurun: yok olana\nkadar {FIRE} basın.",
    b_park = "Vurgulanan yol karesinde durun ve\ndüşman bunkerine nişan alın.",
    b_pick = "Ölü bunkerin üzerinden geçerek onu alın.",
    b_take = "Tarafsız üssün üzerine sürerek onu sizin yapın.",
    base = "Düşman üssünü boşaltmak için\n{FIRE} basın, sonra üzerine sürün.",
    build = "Kontrolü almak için bunkerlerinizi sınırın yakınına,\nbotun toprağına (kuzeydoğu) kurun.",
    c_kill = "Bunkeri iki kez vurmak için {FIRE} basın.",
    c_pick = "Ölü bunkerin üzerinden geçerek onu alın.",
    c_shoot = "Kırmızı işaretli üsse ateş edin:\n{FIRE} basın. Kalan atış: %d.",
    c_take = "Üs boş. Sizin olması için üzerine sürün.",
    dead = "Giriş yolundaki iki ölü bunkerin\nüzerinden geçin.",
    kill = "Nişanınızı kraterlerin üzerine koyun,\ninşaatçı geçerken ateş edin.",
    shoot = "İşaretli duvarı vurun: yıkılana\nkadar {FIRE} basın.",
  },
  line = {
    done = "İstasyon tamamlandı. Yolu kuzeye doğru izleyin.",
    gun = "Nişanınız en uzak menzile gelene\nkadar {GUN_UP} basın.",
    shells = "Mermi az mı? Doldurmak için yol\nkenarındaki üste durun.",
    win = "Eğitim tamamlandı. Hazır olduğunuzda\nmenüden çıkın.",
  },
  info = {
    bases = "Sizin üsler: %d/8",
    enemy_base = "Düşman üssü: %d atış kaldı",
    men = "Öldürülen inşaatçı: %d",
    needed = "%d / %d gerekli",
    on = "Yer: %s, hız %d",
    on_plain = "Yer: %s",
    pill_armour = "Bunker zırhı: %d/%d",
    wall_down = "Hedef duvar: yıkıldı",
    wall_left = "Hedef duvar: %d atış kaldı",
  },
  hint = {
    full = "Dolu",
    low = "Mermi az: doldurun",
    refill = "Doluyor: burada bekleyin",
    reset = "RESET alanı: baştan başla",
    stop = "Üste DURUN: bekleyin",
  },
  terrain = {
    boat = "tekne",
    building = "bina",
    crater = "krater",
    ["deep sea"] = "derin deniz",
    forest = "orman",
    grass = "çimen",
    river = "nehir",
    road = "yol",
    rubble = "moloz",
    swamp = "bataklık",
    wall = "duvar",
  },
  say = {
    back = "Düşman bunkeri geri geldi.\nOnu yeniden alın ya da kuzeye gidin.",
    done = "%d. istasyon tamamlandı. Yolu kuzeye doğru izleyin.",
    done4 = "%d. istasyon tamamlandı. Yolu kuzeye doğru izleyin.\nAğaçları hızla temizlemek için onlara ateş edin.",
    moved = "Yeşil kareye taşındınız.",
    reset = "%d. istasyon sıfırlandı",
  },
}

-- Ukrainian
TEXT["uk"] = {
  pop = {
    s1 = {
      keyboard = {
        "Натисніть {ACCEL}, щоб їхати вперед, {BRAKE}, щоб сповільнитися, {LEFT} і {RIGHT}, щоб повертати.\n\nДілянки біля дороги: ліс, болото, руїни, кратери й мілка річка. Кожна сповільнює по-своєму. Спробуйте проїхати кількома з них.",
        "Глибока вода топить танки. Якщо ви тут загинете, то повернетеся на останню контрольну точку.",
      },
      touch = {
        "Керуйте джойстиком у лівій частині екрана: штовхніть його туди, куди хочете їхати. Що далі штовхнете, то швидше поїдете.\n\nДілянки біля дороги: ліс, болото, руїни, кратери й мілка річка. Кожна сповільнює по-своєму. Спробуйте проїхати кількома з них.",
        "Глибока вода топить танки. Якщо ви тут загинете, то повернетеся на останню контрольну точку.",
      },
    },
    s2a = "Бази поповнюють снаряди, міни та броню, а у вашого танка їх мало.\n\nЗаїдьте на свою базу біля зеленого маркера. Потім ЗУПИНІТЬСЯ на базі й чекайте поповнення.",
    s2b = "Нейтральні бази не червоні й не зелені. Щоб захопити таку базу, просто проїдьте по ній.",
    s2c = "Ворожу базу спершу треба спустошити. Повна витримує %d пострілів: наведіться на неї й натисніть {FIRE}, щоб стріляти. Потім проїдьте по ній, і вона ваша.",
    s2d = {
      "З чим відроджується танк, залежить від типу гри.\n\nВідкрита: танки завжди відроджуються з повним запасом.\n\nТурнір: що більше баз захоплено, то менше снарядів при відродженні. Коли всі бази захоплено, танки відроджуються без снарядів.",
      "Суворий турнір: танки ніколи не відроджуються зі снарядами.\n\nУ турнірних режимах чітка умова перемоги: команда, що захопила всі бази, перемагає, бо ворогу ніде взяти боєприпаси.",
    },
    s2stay = "Це ваша база. Стійте на ній і не рухайтеся: снаряди, міни та броня надходять потроху.",
    s3 = {
      keyboard = {
        "Ваш чоловічок рубає дерева, а потім будує з них дороги, стіни, міни й бункери. Клацніть кнопку будівництва або натисніть її клавішу: {QUICK_TREE} дерева, {QUICK_ROAD} дорога, {QUICK_WALL} стіна, {QUICK_MINE} міна. Потім клацніть по карті, щоб послати туди чоловічка.",
        "Порада: коли вибрано інструмент дороги, стіни чи бункера, клацніть по дереву, і чоловічок його зрубає. Не треба щоразу перемикатися на інструмент дерев.",
      },
      touch = {
        "Ваш чоловічок рубає дерева, а потім будує з них дороги, стіни, міни й бункери. Торкніться кнопки будівництва: дерева, дорога, стіна або міна. Потім торкніться карти, щоб послати туди чоловічка.",
        "Порада: коли вибрано інструмент дороги, стіни чи бункера, торкніться дерева, і чоловічок його зрубає. Не треба щоразу перемикатися на інструмент дерев.",
      },
      controller = {
        "Ваш чоловічок рубає дерева, а потім будує з них дороги, стіни, міни й бункери. Натисніть {BUILD_MODE}, щоб увімкнути курсор будівництва, і наводьте його правим стіком. Натисніть {BUILD_TOOL}, щоб вибрати інструмент: дерева, дорога, стіна або міна.",
        "Потім натисніть {BUILD_PLACE}, щоб послати чоловічка до курсора.\n\nПорада: коли вибрано інструмент дороги, стіни чи бункера, наведіть курсор будівництва на дерево й натисніть {BUILD_PLACE}, і чоловічок його зрубає. Не треба щоразу перемикатися на інструмент дерев.",
      },
    },
    s4a = {
      keyboard = {
        "Гра починається з нейтральними бункерами; повний бункер витримує 15 пострілів.",
        "Спершу проїдьте по зруйнованому бункеру, щоб завантажити його в танк. Потім натисніть кнопку будівництва бункера або її клавішу {QUICK_PILL}. Потім клацніть по карті, і чоловічок збудує там бункер, вірний вам.",
      },
      touch = "Гра починається з нейтральними бункерами; повний бункер витримує 15 пострілів.\n\nСпершу проїдьте по зруйнованому бункеру, щоб завантажити його в танк. Потім торкніться кнопки будівництва бункера й торкніться карти, і чоловічок збудує там бункер, вірний вам.",
      controller = {
        "Гра починається з нейтральними бункерами; повний бункер витримує 15 пострілів.",
        "Спершу проїдьте по зруйнованому бункеру, щоб завантажити його в танк. Потім натискайте {BUILD_TOOL}, доки не виберете інструмент бункера, наведіть курсор будівництва на потрібне місце й натисніть {BUILD_PLACE}. Чоловічок вийде й збудує там бункер, вірний вам.",
      },
    },
    s4b = {
      "На схід від дороги, в кільці лісу, стоїть пошкоджений бункер. Він досі стріляє, а бункер, у який ви влучили, злиться й стріляє частіше. Червоні маркери показують його дальність.",
      "Ліс ховає танки від інших танків і від бункерів. Заїдьте в ліс усередині маркерів і стійте нерухомо. Стрільба видає вашу позицію.",
    },
    s4c = "Цей бункер уже сильно пошкоджений: ще два постріли його знищать.\n\nВаш танк повний. Знищіть бункер, потім проїдьте по ньому, щоб забрати.",
    s5a = "Зупиніться на зеленій клітинці попереду. Дивіться, як союзний бот на острові за стіною захоплює бункер. Він будує свій бункер упритул до ворожого й стає за ним, тож ворог влучає в його бункер, а не в танк. Потім він знищує ворожий бункер і проїжджає по ньому.",
    s5b = {
      "Ваша черга: у вас є бункер. Збудуйте його на зеленій клітинці, поруч із їхнім бункером. Потім станьте на виділену клітинку дороги й стріляйте по ворожому бункеру, доки він не впаде. Ваш бункер приймає його постріли й відбудовується. Почати знову: заїдьте в загін RESET.",
      "Починайте з повною бронею. Танк може отримати трохи шкоди, коли ворожий бункер проб'є ваш бункер-заслін. Це нормально.",
    },
    s6 = {
      "Убийте ворожого чоловічка, і його танк не зможе будувати, доки не прибуде новий. Переїхати чоловічка не можна: його вбивають лише снаряди, міни й вибухи.",
      "Снаряд вибухає в точці прицілу й убиває чоловічка в межах пів клітинки. Натисніть {GUN_DOWN}, щоб наблизити приціл, і {GUN_UP}, щоб віддалити. У цьому навчанні бот посилає чоловічка через кратери. Наведіть туди приціл і стріляйте, поки він іде.",
    },
    s6gun = "Влучили! Тепер натискайте {GUN_UP}, доки приціл не повернеться на найбільшу дальність, для наступної станції.",
    s7 = {
      "Фінальний раунд проти легкого бота. У вас три кути острова, у бота північний схід. Займіть його землі бункерами, потім захопіть його бази, як на станції 2. Ви переможете, коли всі бази будуть ваші. Почати знову: заїдьте в загін RESET.",
      "Спершу проїдьте по двох зруйнованих бункерах на в'їзній дорозі, щоб завантажити їх у танк. Ставте бункери біля межі між вашими і його землями, щоб узяти контроль.",
    },
    welcome = {
      "Ласкаво просимо до WinBolo! Bolo — мережева танкова гра, яку Stuart Cheshire написав у 1987 році, і WinBolo продовжує її сьогодні. Ви їздите танком по острову з бункерами (гарматними вежами, що стріляють по ворожих танках) і базами, які поповнюють снаряди, міни та броню.",
      "У танку їде маленький чоловічок: він виходить рубати дерева й будувати для вас. Гра йде командами, і команда, що тримає бункери й бази, володіє островом.",
    },
    win = "Чудово: усі бази ваші, навчання пройдено.\n\nЇздіть скільки завгодно. Коли будете готові, вийдіть через меню й спробуйте гру проти легких ботів.",
  },
  title = {
    [1] = "Станція 1: Їзда",
    [2] = "Станція 2: Бази",
    [3] = "Станція 3: Будівництво",
    [4] = "Станція 4: Бункери",
    [5] = "Станція 5: Захоплення",
    [6] = "Станція 6: Чоловічок",
    [7] = "Станція 7: Фінал",
  },
  list = {
    a_build = "Збудуйте його",
    a_pick = "Заберіть бункер",
    a_refill = "Поповніться на базі",
    a_watch = "Дивіться на бота",
    all = "Захопіть усі бази",
    b_build = "Будуйте на зеленій",
    b_hide = "Сховайтеся в зоні",
    b_in = "Заїдьте в ліс",
    b_kill = "Знищіть бункер",
    b_park = "Станьте на дорозі",
    b_pick = "Заберіть його",
    b_take = "Займіть нічию базу",
    base = "Захопіть базу ворога",
    build = "Бункер на землі ворога",
    c_kill = "Знищіть бункер",
    c_pick = "Заберіть бункер",
    c_shoot = "Спустошіть базу",
    c_take = "Проїдьте по ній",
    crater = "Кратери",
    dead = "Заберіть обидва бункери",
    forest = "Ліс",
    harvest = "Зрубайте дерево",
    kill = "Убийте чоловічка",
    mine = "Поставте міну",
    river = "Мілка річка",
    road = "Збудуйте дорогу",
    rubble = "Руїни",
    shoot = "Знесіть стіну",
    swamp = "Болото",
    wall = "Збудуйте стіну",
  },
  step = {
    wall = {
      keyboard = "Стіна: клацніть кнопку стіни\nабо натисніть {QUICK_WALL}, потім траву.",
      touch = "Збудуйте стіну: торкніться\nкнопки стіни, потім трави.",
      controller = "Стіна: натисніть {BUILD_TOOL} (стіна),\nнаведіть на траву й натисніть {BUILD_PLACE}.",
    },
    road = {
      keyboard = "Клацніть кнопку дороги або натисніть\n{QUICK_ROAD}, потім клацніть траву.",
      touch = "Торкніться кнопки дороги, потім трави.",
      controller = "Натисніть {BUILD_TOOL} (дорога),\nнаведіть на траву й натисніть {BUILD_PLACE}.",
    },
    mine = {
      keyboard = "Клацніть кнопку міни або натисніть\n{QUICK_MINE}, потім клацніть клітинку.",
      touch = "Торкніться кнопки міни,\nпотім клітинки, щоб поставити міну.",
      controller = "Натисніть {BUILD_TOOL} (міна),\nнаведіть на клітинку й натисніть {BUILD_PLACE}.",
    },
    harvest = {
      keyboard = "Натисніть кнопку дерев або {QUICK_TREE},\nпотім клацніть позначений гай.",
      touch = "Торкніться кнопки дерев,\nпотім лісу в позначеному гаю.",
      controller = "Натисніть {BUILD_TOOL} (дерева), наведіть\nкурсор на гай і натисніть {BUILD_PLACE}.",
    },
    a_build = {
      keyboard = "Натисніть кнопку бункера або {QUICK_PILL},\nпотім клацніть по карті.",
      touch = "Торкніться кнопки бункера,\nпотім карти, щоб збудувати його.",
      controller = "Натисніть {BUILD_TOOL} (бункер), наведіть\nкурсор і натисніть {BUILD_PLACE}.",
    },
    a_pick = "Проїдьте по зруйнованому\nбункеру, щоб завантажити його.",
    a_refill = "Заїдьте на базу з зеленим маркером,\nСТІЙТЕ на ній і чекайте.",
    a_watch = "Станьте на зеленій клітинці\nй дивіться, як бот бере бункер.",
    all = "Захопіть усі бази ворога:\nваших %d з 8.",
    b_build = {
      keyboard = "Натисніть кнопку бункера або {QUICK_PILL},\nпотім клацніть зелену клітинку.",
      touch = "Торкніться кнопки бункера,\nпотім зеленої клітинки.",
      controller = "Тисніть {BUILD_TOOL} (бункер), наведіть\nна зелену клітинку, тисніть {BUILD_PLACE}.",
    },
    b_hide = "Стійте в глибині лісу, в зоні вогню.\nСховано: %d з 5 с.",
    b_in = "Заїдьте в ліс усередині\nчервоних маркерів.",
    b_kill = "Натискайте {FIRE} і стріляйте\nпо бункеру, доки він не впаде.",
    b_park = "Станьте на виділеній клітинці\nдороги й цільтеся в бункер.",
    b_pick = "Проїдьте по зруйнованому\nбункеру, щоб забрати його.",
    b_take = "Заїдьте на нейтральну\nбазу, щоб її захопити.",
    base = "Натискайте {FIRE} і спустошіть\nбазу ворога, потім заїдьте.",
    build = "Ставте бункери біля межі,\nна землі бота (північний схід).",
    c_kill = "Натисніть {FIRE} і влучте\nв бункер двічі.",
    c_pick = "Проїдьте по зруйнованому\nбункеру, щоб забрати його.",
    c_shoot = "Натискайте {FIRE} і стріляйте\nпо червоній базі. Лишилось: %d.",
    c_take = "База порожня. Заїдьте\nна неї, щоб захопити.",
    dead = "Проїдьте по обох бункерах\nна в'їзній дорозі.",
    kill = "Наведіть приціл на кратери й\nстріляйте, коли чоловічок іде.",
    shoot = "Натискайте {FIRE} і стріляйте\nпо стіні, доки вона не впаде.",
  },
  line = {
    done = "Станцію пройдено.\nЇдьте дорогою на північ.",
    gun = "Натискайте {GUN_UP}, доки приціл\nне вийде на макс. дальність.",
    shells = "Мало снарядів? Станьте\nна базу біля дороги.",
    win = "Навчання пройдено. Вийдіть\nчерез меню, коли будете готові.",
  },
  info = {
    bases = "Ваші бази: %d/8",
    enemy_base = "База ворога: ще %d",
    men = "Убито: %d",
    needed = "Треба: %d з %d",
    on = "%s: шв. %d",
    on_plain = "%s",
    pill_armour = "Броня бункера: %d/%d",
    wall_down = "Стіна: знесена",
    wall_left = "Стіна: ще %d",
  },
  hint = {
    full = "Повно",
    low = "Мало снарядів",
    refill = "Поповнення: чекайте",
    reset = "Загін RESET: знову",
    stop = "СТОП на базі: чекайте",
  },
  terrain = {
    boat = "Човен",
    building = "Будівля",
    crater = "Кратер",
    ["deep sea"] = "Відкрите море",
    forest = "Ліс",
    grass = "Трава",
    river = "Річка",
    road = "Дорога",
    rubble = "Руїни",
    swamp = "Болото",
    wall = "Стіна",
  },
  say = {
    back = "Ворожий бункер повернувся.\nЗахопіть знову або їдьте на північ.",
    done = "Станцію %d пройдено.\nЇдьте дорогою на північ.",
    done4 = "Станцію %d пройдено. На північ!\nСтріляйте по деревах, щоб прибрати.",
    moved = "Ви на зеленій клітинці.",
    reset = "Станція %d знову",
  },
}

-- Chinese (Simplified)
TEXT["zh-cn"] = {
  pop = {
    s1 = {
      keyboard = "按 {ACCEL} 前进，按 {BRAKE} 减速，\n按 {LEFT} 和 {RIGHT} 转向。\n\n道路旁的地块有森林、沼泽、瓦砾、弹坑和浅河。\n每种地形让您减速的程度都不同。\n试着在其中几种上行驶。\n\n深水会让坦克沉没。如果您在这里阵亡，\n会在最后一个检查点重生。",
      touch = "用屏幕左侧的摇杆驾驶：\n朝您想去的方向推动摇杆。\n推得越远，速度越快。\n\n道路旁的地块有森林、沼泽、瓦砾、弹坑和浅河。\n每种地形让您减速的程度都不同。\n试着在其中几种上行驶。\n\n深水会让坦克沉没。如果您在这里阵亡，\n会在最后一个检查点重生。",
    },
    s2a = "基地可以补充您的炮弹、地雷和装甲，\n而您的坦克现在补给不足。\n\n开到绿色标记处您的基地上。\n然后务必在基地上停住，等待补给完成。",
    s2b = "中立基地没有红色或绿色标记，\n开过去就能占领。",
    s2c = "敌方基地必须先打空。满的基地需要 %d 发炮弹：\n瞄准它，按 {FIRE} 射击。\n然后开过去，它就归您了。",
    s2d = "坦克重生时带有什么，取决于游戏类型。\n\n开放：坦克总是满载重生。\n\n锦标赛：被占领的基地越多，\n坦克重生时的炮弹越少；所有基地都被占领后，\n坦克重生时没有炮弹。\n\n严格锦标赛：坦克重生时永远没有炮弹。\n\n锦标赛模式有明确的胜利条件：\n一支队伍占领所有基地后，敌方队伍\n无法获得弹药，于是这支队伍获胜。",
    s2stay = "这是您的基地。待在上面不要动：\n您的炮弹、地雷和装甲会一点点补充进来。",
    s3 = {
      keyboard = "您的建造者会采伐树木，再用木材建造道路、\n墙壁、地雷和碉堡。\n点击建造按钮，或按对应的键：\n按 {QUICK_TREE} 采树，按 {QUICK_ROAD} 建道路，\n按 {QUICK_WALL} 建墙壁，按 {QUICK_MINE} 布地雷。\n然后点击地图，派建造者前往该处。\n\n提示：选中道路、墙壁或碉堡工具时，\n点击树木，建造者也会采伐它。\n不必每次都切换到树木工具。",
      touch = "您的建造者会采伐树木，再用木材建造道路、\n墙壁、地雷和碉堡。\n点击建造按钮：树木、道路、墙壁或地雷。\n然后点击地图，派建造者前往该处。\n\n提示：选中道路、墙壁或碉堡工具时，\n点击树木，建造者也会采伐它。\n不必每次都切换到树木工具。",
      controller = "您的建造者会采伐树木，再用木材建造道路、\n墙壁、地雷和碉堡。\n拉住 {BUILD_MODE} 调出建造光标，\n用右摇杆瞄准。按 {BUILD_TOOL} 选择工具：\n树木、道路、墙壁或地雷。然后按\n{BUILD_PLACE}，派建造者前往光标处。\n\n提示：选中道路、墙壁或碉堡工具时，\n把建造光标放在树上并按 {BUILD_PLACE}，\n建造者也会采伐它。\n不必每次都切换到树木工具。",
    },
    s4a = {
      keyboard = "游戏开始时碉堡是中立的，满的碉堡需要\n15 发炮弹才能摧毁。\n\n首先，开过被摧毁的碉堡，把它装进坦克。\n然后按碉堡建造按钮或其快捷键 {QUICK_PILL}。\n再点击地图，派建造者前往该处，\n建造一座忠于您的碉堡。",
      touch = "游戏开始时碉堡是中立的，满的碉堡需要\n15 发炮弹才能摧毁。\n\n首先，开过被摧毁的碉堡，把它装进坦克。\n然后点击碉堡建造按钮，再点击地图，\n派建造者前往该处，建造一座忠于您的碉堡。",
      controller = "游戏开始时碉堡是中立的，满的碉堡需要\n15 发炮弹才能摧毁。\n\n首先，开过被摧毁的碉堡，把它装进坦克。\n然后按 {BUILD_TOOL}，直到选中碉堡工具，\n把建造光标放到想要的位置，然后按 {BUILD_PLACE}。\n建造者会出去，在那里建造一座忠于您的碉堡。",
    },
    s4b = "道路东侧，一座受损的碉堡被一圈森林围着。\n它仍会开火，而被击中的碉堡会被激怒，\n开火更快。红色标记表示它的射程。\n\n森林能让坦克躲开其他坦克和碉堡。\n开进标记范围内的森林并停住不动。\n开火会暴露您的位置。",
    s4c = "这座碉堡已严重受损：再打两发就能摧毁。\n\n您的坦克是满载的。摧毁它，\n然后开过去把它捡起来。",
    s5a = "在前方的绿色方块上停下。看看被墙围住的小岛上，\n友方机器人如何夺取碉堡。它在敌方碉堡旁边\n建造自己的碉堡，并停在它后面，\n这样敌方的炮弹打中的是它的碉堡，\n而不是它的坦克。然后它击毁敌方碉堡，\n再开过去。",
    s5b = "轮到您了：您带着一座碉堡。在标记的绿色方块上，\n紧挨着敌方碉堡建造您的碉堡。然后停在高亮的\n道路方块上，瞄准敌方碉堡，一直射击直到它被摧毁。\n您的碉堡会替您挡住炮弹，被摧毁后还会重建。\n开进 RESET 围栏可以重新开始。\n\n开始时请保持装甲满格。注意：敌方碉堡打穿\n挡在前面的碉堡后，坦克受到一些伤害是正常的。",
    s6 = "击杀敌方建造者后，在新的建造者到达之前，\n他的坦克无法建造。您不能用坦克碾死建造者：\n只有炮弹、地雷和爆炸能杀死他。\n\n炮弹在瞄准镜处爆炸，能杀死半格以内的建造者。\n按 {GUN_DOWN} 把瞄准镜拉近，按 {GUN_UP} 把它推远。\n在本教程中，一个机器人被设定为让建造者\n穿过弹坑。把瞄准镜放在那里，\n在他穿过时持续射击。",
    s6gun = "打中了！现在按 {GUN_UP}，直到瞄准镜\n回到最远射程，为下一站做好准备。",
    s7 = "最后一轮，对手是简单机器人。您占据岛屿的三个角，\n机器人占据东北角。用您的碉堡夺取它的土地，\n再像第 2 站那样夺取它的基地。\n占领所有基地即可获胜。\n开进 RESET 围栏可以重新开始。\n\n首先，开过入口道路上的两座被摧毁的碉堡，\n把它们装进坦克。在您和对方土地的交界附近\n建造碉堡，以夺取控制权。",
    welcome = "欢迎来到 WinBolo！Bolo 是 Stuart Cheshire\n于 1987 年首次编写的多人坦克游戏，\nWinBolo 让它延续至今。\n您驾驶坦克在岛上行驶。岛上有碉堡，\n即向敌方坦克开火的炮塔，还有基地，\n可以补充您的炮弹、地雷和装甲。\n一个小小的建造者坐在您的坦克里，\n会下车为您砍树和建造。\n您以团队形式游戏，掌握碉堡和基地的\n队伍就掌握了这座岛。",
    win = "干得好：您占领了所有基地，教程完成。\n\n您可以随意继续驾驶。准备好后从菜单退出，\n然后试试与简单机器人对战。",
  },
  title = {
    [1] = "第1站：驾驶",
    [2] = "第2站：基地",
    [3] = "第3站：建造",
    [4] = "第4站：碉堡",
    [5] = "第5站：夺取碉堡",
    [6] = "第6站：建造者",
    [7] = "第7站：最后一轮",
  },
  list = {
    a_build = "建造碉堡",
    a_pick = "捡起被毁碉堡",
    a_refill = "在基地补给",
    a_watch = "观看机器人夺取",
    all = "夺取所有敌方基地",
    b_build = "在绿色方块建造",
    b_hide = "在射程内隐蔽",
    b_in = "从森林进入射程",
    b_kill = "摧毁敌方碉堡",
    b_park = "停在道路方块上",
    b_pick = "捡起碉堡",
    b_take = "占领中立基地",
    base = "夺取一个敌方基地",
    build = "在敌方土地建碉堡",
    c_kill = "摧毁碉堡",
    c_pick = "捡起碉堡",
    c_shoot = "打空敌方基地",
    c_take = "开过基地",
    crater = "弹坑",
    dead = "捡起两座被毁碉堡",
    forest = "森林",
    harvest = "采伐一棵树",
    kill = "射杀建造者",
    mine = "布设地雷",
    river = "浅河",
    road = "建造道路",
    rubble = "瓦砾",
    shoot = "击毁墙壁",
    swamp = "沼泽",
    wall = "建造墙壁",
  },
  step = {
    wall = {
      keyboard = "建造墙壁：点击墙壁按钮\n或按 {QUICK_WALL}，然后点击一个草地方块。",
      touch = "建造墙壁：点击墙壁按钮，\n然后点击一个草地方块。",
      controller = "建造墙壁：按 {BUILD_TOOL} 选择墙壁工具，\n把建造光标放在草地上，然后按 {BUILD_PLACE}。",
    },
    road = {
      keyboard = "点击道路按钮或按 {QUICK_ROAD}，\n然后点击一个草地方块。",
      touch = "点击道路按钮，然后点击一个草地方块。",
      controller = "按 {BUILD_TOOL} 选择道路工具，把建造光标\n放在草地上，然后按 {BUILD_PLACE}。",
    },
    mine = {
      keyboard = "点击地雷按钮或按 {QUICK_MINE}，\n然后点击一个方块布设地雷。",
      touch = "点击地雷按钮，然后点击\n一个方块布设地雷。",
      controller = "按 {BUILD_TOOL} 选择地雷工具，把建造光标\n放在一个方块上，然后按 {BUILD_PLACE}。",
    },
    harvest = {
      keyboard = "点击树木按钮或按 {QUICK_TREE}，\n然后点击标记树林中的森林方块。",
      touch = "点击树木按钮，然后点击\n标记树林中的森林方块。",
      controller = "按 {BUILD_TOOL} 选择树木工具，把建造光标\n放在标记的树林上，然后按 {BUILD_PLACE}。",
    },
    a_build = {
      keyboard = "按碉堡按钮或 {QUICK_PILL}，\n然后点击地图进行建造。",
      touch = "点击碉堡按钮，\n然后点击地图进行建造。",
      controller = "按 {BUILD_TOOL} 选择碉堡工具，放下建造光标，\n然后按 {BUILD_PLACE} 进行建造。",
    },
    a_pick = "开过被摧毁的碉堡，把它装进坦克。",
    a_refill = "开上绿色标记的基地，然后停在上面，\n等待补给完成。",
    a_watch = "在前方的绿色方块上停下，\n观看机器人夺取碉堡。",
    all = "夺取所有敌方基地：已占领 %d / 8。",
    b_build = {
      keyboard = "按碉堡按钮或 {QUICK_PILL}，\n然后点击绿色方块。",
      touch = "点击碉堡按钮，\n然后点击绿色方块。",
      controller = "按 {BUILD_TOOL} 选择碉堡工具，把建造光标\n放在绿色方块上，然后按 {BUILD_PLACE}。",
    },
    b_hide = "在射程内的森林深处停住不动。\n已隐蔽：%d / 5 秒。",
    b_in = "开进红色标记范围内的森林。",
    b_kill = "按 {FIRE} 射击敌方碉堡，\n直到它被摧毁。",
    b_park = "停在高亮的道路方块上，\n瞄准敌方碉堡。",
    b_pick = "开过被摧毁的碉堡，把它捡起来。",
    b_take = "开上中立基地，将其占领。",
    base = "按 {FIRE} 把敌方基地打空，\n然后开上去。",
    build = "在交界附近、机器人的土地（东北）上\n建造碉堡，以夺取控制权。",
    c_kill = "按 {FIRE} 向碉堡射击两次。",
    c_pick = "开过被摧毁的碉堡，把它捡起来。",
    c_shoot = "按 {FIRE} 射击红色标记的基地：\n还剩 %d 发。",
    c_take = "基地已空。开上去将其占领。",
    dead = "开过入口道路上的\n两座被摧毁的碉堡。",
    kill = "把瞄准镜放在弹坑上，\n在建造者穿过时开火。",
    shoot = "按 {FIRE} 射击标记的墙壁，\n直到它倒塌。",
  },
  line = {
    done = "本站完成。沿道路向北前进。",
    gun = "按 {GUN_UP}，直到瞄准镜\n达到最远射程。",
    shells = "炮弹不足？停在路边的\n基地上补给。",
    win = "教程完成。准备好后\n从菜单退出。",
  },
  info = {
    bases = "我方基地：%d/8",
    enemy_base = "敌方基地：还剩 %d 发",
    men = "已击杀建造者：%d",
    needed = "进度：%d/%d",
    on = "%s：速度 %d",
    on_plain = "地形：%s",
    pill_armour = "碉堡装甲：%d/%d",
    wall_down = "目标墙壁：已击毁",
    wall_left = "目标墙壁：还剩 %d 发",
  },
  hint = {
    full = "已满",
    low = "炮弹不足：去补给",
    refill = "补给中：在此等待",
    reset = "RESET 围栏：重新开始",
    stop = "停在基地上：等待",
  },
  terrain = {
    boat = "船",
    building = "建筑",
    crater = "弹坑",
    ["deep sea"] = "深海",
    forest = "森林",
    grass = "草地",
    river = "河流",
    road = "道路",
    rubble = "瓦砾",
    swamp = "沼泽",
    wall = "墙壁",
  },
  say = {
    back = "敌方碉堡又回来了。\n再次夺取它，或向北前进。",
    done = "第 %d 站完成。沿道路向北前进。",
    done4 = "第 %d 站完成。沿道路向北前进。\n射击树木可以快速清除树木。",
    moved = "已移到绿色方块。",
    reset = "第 %d 站已重置",
  },
}

-- Chinese (Traditional)
TEXT["zh-tw"] = {
  pop = {
    s1 = {
      keyboard = "按 {ACCEL} 前進，按 {BRAKE} 減速，\n按 {LEFT} 和 {RIGHT} 轉向。\n\n道路旁的地塊有森林、沼澤、瓦礫、彈坑和淺河。\n每種地形讓您減速的程度都不同。\n試著在其中幾種上行駛。\n\n深水會讓坦克沉沒。如果您在這裡陣亡，\n會在最後一個檢查點重生。",
      touch = "用畫面左側的搖桿駕駛：\n朝您想去的方向推動搖桿。\n推得越遠，速度越快。\n\n道路旁的地塊有森林、沼澤、瓦礫、彈坑和淺河。\n每種地形讓您減速的程度都不同。\n試著在其中幾種上行駛。\n\n深水會讓坦克沉沒。如果您在這裡陣亡，\n會在最後一個檢查點重生。",
    },
    s2a = "基地可以補充您的砲彈、地雷和裝甲，\n而您的坦克現在補給不足。\n\n開到綠色標記處您的基地上。\n然後務必在基地上停住，等待補給完成。",
    s2b = "中立基地沒有紅色或綠色標記，\n開過去就能占領。",
    s2c = "敵方基地必須先打空。滿的基地需要 %d 發砲彈：\n瞄準它，按 {FIRE} 射擊。\n然後開過去，它就歸您了。",
    s2d = "坦克重生時帶有什麼，取決於遊戲類型。\n\n開放：坦克總是滿載重生。\n\n錦標賽：被占領的基地越多，\n坦克重生時的砲彈越少；所有基地都被占領後，\n坦克重生時沒有砲彈。\n\n嚴格錦標賽：坦克重生時永遠沒有砲彈。\n\n錦標賽模式有明確的勝利條件：\n一支隊伍占領所有基地後，敵方隊伍\n無法取得彈藥，於是這支隊伍獲勝。",
    s2stay = "這是您的基地。待在上面不要動：\n您的砲彈、地雷和裝甲會一點一點補充進來。",
    s3 = {
      keyboard = "您的建造者會採伐樹木，再用木材建造道路、\n牆壁、地雷和碉堡。\n點擊建造按鈕，或按對應的鍵：\n按 {QUICK_TREE} 採樹，按 {QUICK_ROAD} 建道路，\n按 {QUICK_WALL} 建牆壁，按 {QUICK_MINE} 佈地雷。\n然後點擊地圖，派建造者前往該處。\n\n提示：選取道路、牆壁或碉堡工具時，\n點擊樹木，建造者也會採伐它。\n不必每次都切換到樹木工具。",
      touch = "您的建造者會採伐樹木，再用木材建造道路、\n牆壁、地雷和碉堡。\n點擊建造按鈕：樹木、道路、牆壁或地雷。\n然後點擊地圖，派建造者前往該處。\n\n提示：選取道路、牆壁或碉堡工具時，\n點擊樹木，建造者也會採伐它。\n不必每次都切換到樹木工具。",
      controller = "您的建造者會採伐樹木，再用木材建造道路、\n牆壁、地雷和碉堡。\n拉住 {BUILD_MODE} 叫出建造游標，\n用右搖桿瞄準。按 {BUILD_TOOL} 選擇工具：\n樹木、道路、牆壁或地雷。然後按\n{BUILD_PLACE}，派建造者前往游標處。\n\n提示：選取道路、牆壁或碉堡工具時，\n把建造游標放在樹上並按 {BUILD_PLACE}，\n建造者也會採伐它。\n不必每次都切換到樹木工具。",
    },
    s4a = {
      keyboard = "遊戲開始時碉堡是中立的，滿的碉堡需要\n15 發砲彈才能摧毀。\n\n首先，開過被摧毀的碉堡，把它裝進坦克。\n然後按碉堡建造按鈕或其快速鍵 {QUICK_PILL}。\n再點擊地圖，派建造者前往該處，\n建造一座忠於您的碉堡。",
      touch = "遊戲開始時碉堡是中立的，滿的碉堡需要\n15 發砲彈才能摧毀。\n\n首先，開過被摧毀的碉堡，把它裝進坦克。\n然後點擊碉堡建造按鈕，再點擊地圖，\n派建造者前往該處，建造一座忠於您的碉堡。",
      controller = "遊戲開始時碉堡是中立的，滿的碉堡需要\n15 發砲彈才能摧毀。\n\n首先，開過被摧毀的碉堡，把它裝進坦克。\n然後按 {BUILD_TOOL}，直到選取碉堡工具，\n把建造游標放到想要的位置，然後按 {BUILD_PLACE}。\n建造者會出去，在那裡建造一座忠於您的碉堡。",
    },
    s4b = "道路東側，一座受損的碉堡被一圈森林圍著。\n它仍會開火，而被擊中的碉堡會被激怒，\n開火更快。紅色標記表示它的射程。\n\n森林能讓坦克躲開其他坦克和碉堡。\n開進標記範圍內的森林並停住不動。\n開火會暴露您的位置。",
    s4c = "這座碉堡已嚴重受損：再打兩發就能摧毀。\n\n您的坦克是滿載的。摧毀它，\n然後開過去把它撿起來。",
    s5a = "在前方的綠色方格上停下。看看被牆圍住的小島上，\n友方機器人如何奪取碉堡。它在敵方碉堡旁邊\n建造自己的碉堡，並停在它後面，\n這樣敵方的砲彈打中的是它的碉堡，\n而不是它的坦克。然後它擊毀敵方碉堡，\n再開過去。",
    s5b = "輪到您了：您帶著一座碉堡。在標記的綠色方格上，\n緊挨著敵方碉堡建造您的碉堡。然後停在醒目標示的\n道路方格上，瞄準敵方碉堡，一直射擊直到它被摧毀。\n您的碉堡會替您擋住砲彈，被摧毀後還會重建。\n開進 RESET 圍欄可以重新開始。\n\n開始時請保持裝甲全滿。注意：敵方碉堡打穿\n擋在前面的碉堡後，坦克受到一些傷害是正常的。",
    s6 = "擊殺敵方建造者後，在新的建造者到達之前，\n他的坦克無法建造。您不能用坦克輾死建造者：\n只有砲彈、地雷和爆炸能殺死他。\n\n砲彈在瞄準器處爆炸，能殺死半格以內的建造者。\n按 {GUN_DOWN} 把瞄準器拉近，按 {GUN_UP} 把它推遠。\n在本教學中，一個機器人被設定為讓建造者\n穿過彈坑。把瞄準器放在那裡，\n在他穿過時持續射擊。",
    s6gun = "打中了！現在按 {GUN_UP}，直到瞄準器\n回到最遠射程，為下一站做好準備。",
    s7 = "最後一輪，對手是簡單機器人。您占據島嶼的三個角，\n機器人占據東北角。用您的碉堡奪取它的土地，\n再像第 2 站那樣奪取它的基地。\n占領所有基地即可獲勝。\n開進 RESET 圍欄可以重新開始。\n\n首先，開過入口道路上的兩座被摧毀的碉堡，\n把它們裝進坦克。在您和對方土地的交界附近\n建造碉堡，以奪取控制權。",
    welcome = "歡迎來到 WinBolo！Bolo 是 Stuart Cheshire\n於 1987 年首次編寫的多人坦克遊戲，\nWinBolo 讓它延續至今。\n您駕駛坦克在島上行駛。島上有碉堡，\n即向敵方坦克開火的砲塔，還有基地，\n可以補充您的砲彈、地雷和裝甲。\n一個小小的建造者坐在您的坦克裡，\n會下車為您砍樹和建造。\n您以團隊形式遊戲，掌握碉堡和基地的\n隊伍就掌握了這座島。",
    win = "做得好：您占領了所有基地，教學完成。\n\n您可以隨意繼續駕駛。準備好後從選單離開，\n然後試試與簡單機器人對戰。",
  },
  title = {
    [1] = "第1站：駕駛",
    [2] = "第2站：基地",
    [3] = "第3站：建造",
    [4] = "第4站：碉堡",
    [5] = "第5站：奪取碉堡",
    [6] = "第6站：建造者",
    [7] = "第7站：最後一輪",
  },
  list = {
    a_build = "建造碉堡",
    a_pick = "撿起被毀碉堡",
    a_refill = "在基地補給",
    a_watch = "觀看機器人奪取",
    all = "奪取所有敵方基地",
    b_build = "在綠色方格建造",
    b_hide = "在射程內隱蔽",
    b_in = "從森林進入射程",
    b_kill = "摧毀敵方碉堡",
    b_park = "停在道路方格上",
    b_pick = "撿起碉堡",
    b_take = "占領中立基地",
    base = "奪取一個敵方基地",
    build = "在敵方土地建碉堡",
    c_kill = "摧毀碉堡",
    c_pick = "撿起碉堡",
    c_shoot = "打空敵方基地",
    c_take = "開過基地",
    crater = "彈坑",
    dead = "撿起兩座被毀碉堡",
    forest = "森林",
    harvest = "採伐一棵樹",
    kill = "射殺建造者",
    mine = "佈設地雷",
    river = "淺河",
    road = "建造道路",
    rubble = "瓦礫",
    shoot = "擊毀牆壁",
    swamp = "沼澤",
    wall = "建造牆壁",
  },
  step = {
    wall = {
      keyboard = "建造牆壁：點擊牆壁按鈕\n或按 {QUICK_WALL}，然後點擊一個草地方格。",
      touch = "建造牆壁：點擊牆壁按鈕，\n然後點擊一個草地方格。",
      controller = "建造牆壁：按 {BUILD_TOOL} 選擇牆壁工具，\n把建造游標放在草地上，然後按 {BUILD_PLACE}。",
    },
    road = {
      keyboard = "點擊道路按鈕或按 {QUICK_ROAD}，\n然後點擊一個草地方格。",
      touch = "點擊道路按鈕，然後點擊一個草地方格。",
      controller = "按 {BUILD_TOOL} 選擇道路工具，把建造游標\n放在草地上，然後按 {BUILD_PLACE}。",
    },
    mine = {
      keyboard = "點擊地雷按鈕或按 {QUICK_MINE}，\n然後點擊一個方格佈設地雷。",
      touch = "點擊地雷按鈕，然後點擊\n一個方格佈設地雷。",
      controller = "按 {BUILD_TOOL} 選擇地雷工具，把建造游標\n放在一個方格上，然後按 {BUILD_PLACE}。",
    },
    harvest = {
      keyboard = "點擊樹木按鈕或按 {QUICK_TREE}，\n然後點擊標記樹林中的森林方格。",
      touch = "點擊樹木按鈕，然後點擊\n標記樹林中的森林方格。",
      controller = "按 {BUILD_TOOL} 選擇樹木工具，把建造游標\n放在標記的樹林上，然後按 {BUILD_PLACE}。",
    },
    a_build = {
      keyboard = "按碉堡按鈕或 {QUICK_PILL}，\n然後點擊地圖進行建造。",
      touch = "點擊碉堡按鈕，\n然後點擊地圖進行建造。",
      controller = "按 {BUILD_TOOL} 選擇碉堡工具，放下建造游標，\n然後按 {BUILD_PLACE} 進行建造。",
    },
    a_pick = "開過被摧毀的碉堡，把它裝進坦克。",
    a_refill = "開上綠色標記的基地，然後停在上面，\n等待補給完成。",
    a_watch = "在前方的綠色方格上停下，\n觀看機器人奪取碉堡。",
    all = "奪取所有敵方基地：已占領 %d / 8。",
    b_build = {
      keyboard = "按碉堡按鈕或 {QUICK_PILL}，\n然後點擊綠色方格。",
      touch = "點擊碉堡按鈕，\n然後點擊綠色方格。",
      controller = "按 {BUILD_TOOL} 選擇碉堡工具，把建造游標\n放在綠色方格上，然後按 {BUILD_PLACE}。",
    },
    b_hide = "在射程內的森林深處停住不動。\n已隱蔽：%d / 5 秒。",
    b_in = "開進紅色標記範圍內的森林。",
    b_kill = "按 {FIRE} 射擊敵方碉堡，\n直到它被摧毀。",
    b_park = "停在醒目標示的道路方格上，\n瞄準敵方碉堡。",
    b_pick = "開過被摧毀的碉堡，把它撿起來。",
    b_take = "開上中立基地，將其占領。",
    base = "按 {FIRE} 把敵方基地打空，\n然後開上去。",
    build = "在交界附近、機器人的土地（東北）上\n建造碉堡，以奪取控制權。",
    c_kill = "按 {FIRE} 向碉堡射擊兩次。",
    c_pick = "開過被摧毀的碉堡，把它撿起來。",
    c_shoot = "按 {FIRE} 射擊紅色標記的基地：\n還剩 %d 發。",
    c_take = "基地已空。開上去將其占領。",
    dead = "開過入口道路上的\n兩座被摧毀的碉堡。",
    kill = "把瞄準器放在彈坑上，\n在建造者穿過時開火。",
    shoot = "按 {FIRE} 射擊標記的牆壁，\n直到它倒塌。",
  },
  line = {
    done = "本站完成。沿道路向北前進。",
    gun = "按 {GUN_UP}，直到瞄準器\n達到最遠射程。",
    shells = "砲彈不足？停在路邊的\n基地上補給。",
    win = "教學完成。準備好後\n從選單離開。",
  },
  info = {
    bases = "我方基地：%d/8",
    enemy_base = "敵方基地：還剩 %d 發",
    men = "已擊殺建造者：%d",
    needed = "進度：%d/%d",
    on = "%s：速度 %d",
    on_plain = "地形：%s",
    pill_armour = "碉堡裝甲：%d/%d",
    wall_down = "目標牆壁：已擊毀",
    wall_left = "目標牆壁：還剩 %d 發",
  },
  hint = {
    full = "已滿",
    low = "砲彈不足：去補給",
    refill = "補給中：在此等待",
    reset = "RESET 圍欄：重新開始",
    stop = "停在基地上：等待",
  },
  terrain = {
    boat = "船",
    building = "建築",
    crater = "彈坑",
    ["deep sea"] = "深海",
    forest = "森林",
    grass = "草地",
    river = "河流",
    road = "道路",
    rubble = "瓦礫",
    swamp = "沼澤",
    wall = "牆壁",
  },
  say = {
    back = "敵方碉堡又回來了。\n再次奪取它，或向北前進。",
    done = "第 %d 站完成。沿道路向北前進。",
    done4 = "第 %d 站完成。沿道路向北前進。\n射擊樹木可以快速清除樹木。",
    moved = "已移到綠色方格。",
    reset = "第 %d 站已重設",
  },
}
