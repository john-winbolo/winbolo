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
--   2  Bases        2A your base (stop and refill), 2B a grey base (take
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
-- gives the tank that station's loadout. Each checkpoint is a deep-water
-- dock on the main road: the tank respawns on its boat and drives north.
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
-- The decisions behind this file are in docs/TUTORIAL_DECISIONS.md.
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
    b5 = { n = 6, x = 127, y = 97 },
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
    cp1 = { n = 1, x = 127, y = 225 },
    cp2 = { n = 2, x = 127, y = 199 },
    cp3 = { n = 3, x = 127, y = 176 },
    cp4 = { n = 4, x = 127, y = 155 },
    bot5a = { n = 5, x = 140, y = 117 },
    cp5 = { n = 6, x = 127, y = 124 },
    bot6 = { n = 7, x = 134, y = 80 },
    cp6 = { n = 8, x = 127, y = 89 },
    bot7 = { n = 9, x = 142, y = 32 },
    cp7 = { n = 10, x = 112, y = 52 },
  },
  point = {
    bot6_park = { 131, 80 },
    p4 = { 138, 141 },
    s7_arrive = { 127, 63 },
    t5a_blocker = { 137, 106 },
    t5a_home = { 136, 115 },
    t5a_target = { 137, 105 },
    t5b_blocker = { 117, 102 },
    t5b_park = { 117, 108 },
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
    s5b = { x = 97, y = 95, w = 28, h = 32 },
    s6 = { x = 97, y = 72, w = 59, h = 20 },
    s7 = { x = 97, y = 25, w = 59, h = 47 },
    t5b_park = { x = 117, y = 108, w = 1, h = 1 },
    watch5a = { x = 129, y = 111, w = 3, h = 1 },
  },
  walls5b = { {106,112}, {107,112}, {106,113}, {108,113}, {106,114}, {107,114}, {106,115}, {108,115}, {106,116}, {108,116}, {110,112}, {111,112}, {112,112}, {110,113}, {110,114}, {111,114}, {110,115}, {110,116}, {111,116}, {112,116}, {115,112}, {116,112}, {114,113}, {115,114}, {116,115}, {114,116}, {115,116}, {118,112}, {119,112}, {120,112}, {118,113}, {118,114}, {119,114}, {118,115}, {118,116}, {119,116}, {120,116}, {122,112}, {123,112}, {124,112}, {123,113}, {123,114}, {123,115}, {123,116}, {113,118}, {114,118}, {115,118}, {116,118}, {117,118}, {113,119}, {113,120}, {113,121}, {114,121}, {115,121}, {116,121}, {117,121} },
  walls7 = { {108,55}, {109,55}, {108,56}, {110,56}, {108,57}, {109,57}, {108,58}, {110,58}, {108,59}, {110,59}, {112,55}, {113,55}, {114,55}, {112,56}, {112,57}, {113,57}, {112,58}, {112,59}, {113,59}, {114,59}, {117,55}, {118,55}, {116,56}, {117,57}, {118,58}, {116,59}, {117,59}, {120,55}, {121,55}, {122,55}, {120,56}, {120,57}, {121,57}, {120,58}, {120,59}, {121,59}, {122,59}, {124,55}, {125,55}, {126,55}, {125,56}, {125,57}, {125,58}, {125,59}, {115,61}, {116,61}, {117,61}, {118,61}, {119,61}, {115,62}, {115,63}, {115,64}, {116,64}, {117,64}, {118,64}, {119,64} },
  arrow_reset5b = { {119,123}, {120,122}, {120,124}, {121,123}, {122,123} },
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
  updated = "2026-10-10T12:00Z",
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
    pill_damage_scale = "Only one in five of the enemy 5B pillbox's shells damages your 5B pillbox.",
    can_hit = "Tank shells pass between you and the demo bots; yours pass your 5B pillbox; the 5A pillboxes' pass you.",
    can_die = "The parked Station 6 tank cannot be destroyed, and the Station 4 pillbox cannot be killed before you have hidden.",
    on_choose_start = "You respawn at the furthest station you have reached.",
    spawn_loadout = "You respawn with the tank that station starts you with.",
    allow_base_win = "Owning every base does not end the round; Station 7 has its own win.",
  },
}

-- ---------------------------------------------------------------------
-- Round state. The host builds a fresh Lua state for every round, so none
-- of this needs a reset between rounds.
local S = {
  player   = nil,     -- the tutorial's player seat
  reached  = 1,       -- the furthest station reached: the checkpoint
  at       = 1,       -- the station the player last drove into
  entered  = {},      -- station -> true once driven into
  done     = { {}, {}, {}, {}, {}, {}, {} },  -- checklist, by station
  popped   = {},      -- popup id -> true once shown
  queue    = {},      -- popups waiting for their delay: { id, due }
  carrier  = {},      -- pill n -> seat carrying it
  -- The bots' seats, picked as the round starts; nil where none was free.
  bot5a = nil, bot6 = nil, bot7 = nil,
  free_seat = {},     -- seat -> true where a role took an empty seat
  fielded  = {},      -- role -> true once spawn_bot was asked
  frames   = 0,       -- on_tick calls (50 a second)
  sec      = 0,       -- whole seconds since the start
  last_pos = nil,     -- world position at the last poll
  moving   = false,
  hide     = 0,       -- Station 4: seconds hidden in the forest in range
  told_hide = -100,   -- Station 4: the second "hide first" was last said
  men      = 0,       -- Station 6: men killed
  gun_remind = false, -- Station 6: the gun-range reminder is up
  t5a_reset_at = 0,   -- Station 5A: the second of the last reset
  t5b_restore = false,  -- Station 5B: a restore of the target is waiting
  b5b_dead_at = nil,  -- Station 5B: the second the player's blocker died
  out5     = 0,       -- polls in a row the player's tank was outside Station 5
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
local POP = {
  welcome = "Welcome to WinBolo! Bolo is a tank game Stuart Cheshire first "
    .. "wrote in 1987, and WinBolo carries it on today. You drive a tank "
    .. "round an island of pillboxes, gun towers that shoot at enemy tanks, "
    .. "and bases, which refill your shells, mines and armour. A small man "
    .. "rides in your tank and gets out to cut trees and build for you. You "
    .. "play in teams, and the team that holds the pillboxes and bases "
    .. "holds the island.",
  s1 = "{ACCEL} forward, {BRAKE} slow down, {LEFT} and {RIGHT} turn.\n\n"
    .. "The patches beside the road are forest, swamp, rubble, craters and "
    .. "a shallow river. Each slows you down by a different amount. Drive "
    .. "onto three of them.\n\n"
    .. "Deep water sinks tanks. If you die anywhere here, you come back "
    .. "at your last checkpoint.",
  s2a = "Bases refill your shells, mines and armour, and your tank is low.\n\n"
    .. "Drive onto your base at the green marker. Then STOP on the base and "
    .. "wait while it refills.",
  s2stay = "This is your base. Stay on it and do not move: your shells, "
    .. "mines and armour flow in a little at a time.\n\n"
    .. "The panel says Full when your tank is full.",
  s2b = "Grey bases belong to nobody. Drive onto one to make it yours.",
  s2c = function()
    return string.format("Enemy bases must be shot empty first. A full one "
      .. "takes %d shots: aim at it and fire with {FIRE}. The panel counts "
      .. "the shots left. Then drive over it to make it yours.",
      base_shots(rule("base_full_armour")))
  end,
  s2d = "Bases are your supply line, and what you respawn with depends on "
    .. "the game type.\n\n"
    .. "Open: tanks always respawn fully loaded.\n\n"
    .. "Tournament: the more bases are taken, the fewer shells a tank "
    .. "respawns with; once all bases are taken, tanks respawn without "
    .. "shells.\n\n"
    .. "Strict tournament: tanks never respawn with shells.\n\n"
    .. "A team that takes every base wins, because the enemy can't refuel.",
  s3 = "Your man harvests trees, then uses them to build roads, walls, "
    .. "mines and pillboxes. Click a build button, or press its key: "
    .. "{QUICK_TREE} trees, {QUICK_ROAD} road, {QUICK_WALL} wall, "
    .. "{QUICK_MINE} mine. Then click on the map to send your man there.",
  s4a = "Games start with neutral pillboxes, and a full one takes 15 shots "
    .. "to kill.\n\n"
    .. "First, drive over the dead pillbox to put it in your tank. Then "
    .. "press the pillbox build button or its shortcut key {QUICK_PILL}. "
    .. "Then click on the map to send your man out to build a pillbox loyal "
    .. "to you at that location.",
  s4b = "East of the road, a damaged pillbox sits in a ring of forest. It "
    .. "still fires, and a pillbox you hit gets angry and fires faster. The "
    .. "red markers show its range.\n\n"
    .. "Forests hide tanks from both other tanks and pillboxes. Drive into "
    .. "the forest inside the markers and sit still. Firing reveals your "
    .. "position.",
  s4c = "This pillbox is already badly damaged: two more shots kill it, "
    .. "and it shoots back.\n\n"
    .. "Your tank is full. Kill it, then drive over it to pick it up.",
  s5a = "Stop on the green square at the end of the short road east of "
    .. "the main road, and watch the friendly bot on the walled island take "
    .. "a pillbox. It builds its own pillbox right next to the enemy one and "
    .. "parks behind it, so the enemy's shots hit its pillbox, not its tank. "
    .. "Then it shoots the enemy pillbox dead and drives over it.",
  s5b = "Your turn: you carry a pillbox. Build your pillbox at the marked "
    .. "green square, right next to theirs. Then stop at the road tile "
    .. "highlighted and aim at the enemy pillbox: fire until it dies. Your "
    .. "pillbox takes its shots, and is rebuilt if it dies. Drive into the "
    .. "RESET pen to start over.",
  s6 = "Kill an enemy man and his tank cannot build until a new man "
    .. "arrives. You cannot run a man over: only shells, mines and "
    .. "explosions kill him.\n\n"
    .. "A shell bursts at your crosshair and kills a man within half a "
    .. "square. {GUN_DOWN} brings the crosshair in, {GUN_UP} moves it out. "
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
}

local TITLE = { "1: Driving", "2: Bases", "3: Building", "4: Pillboxes",
                "5: Taking a pill", "6: The man", "7: Final round" }

-- The checklist of each station: { key, label }. A label is short enough
-- for one panel line in the small font.
local LIST = {
  { { "forest", "Forest" },
    { "swamp", "Swamp" },
    { "rubble", "Rubble" },
    { "crater", "Craters" },
    { "river", "Shallow river" } },
  { { "a_refill", "Refill at your base" },
    { "b_take", "Take the grey base" },
    { "c_shoot", "Shoot enemy base empty" },
    { "c_take", "Drive over it" } },
  { { "harvest", "Harvest a tree" },
    { "road", "Build a road" },
    { "wall", "Build a wall" },
    { "mine", "Lay a mine" },
    { "shoot", "Shoot down the wall" } },
  { { "a_pick", "Pick up the dead pill" },
    { "a_build", "Build it" },
    { "b_in", "Enter its range in forest" },
    { "b_hide", "Sit hidden in range" },
    { "c_kill", "Kill the pillbox" },
    { "c_pick", "Pick the pillbox up" } },
  { { "a_watch", "Watch the bot take one" },
    { "b_build", "Build on the green square" },
    { "b_park", "Park on the road square" },
    { "b_kill", "Kill the enemy pill" },
    { "b_pick", "Pick it up" } },
  { { "kill", "Shoot the man" } },
  { { "dead", "Pick up both dead pills" },
    { "build", "Pill in enemy land" },
    { "base", "Steal an enemy base" },
    { "all", "Take every enemy base" } },
}

-- Station 1 is done after this many of its five terrains.
local TERRAINS_NEEDED = 3

-- The status line for each goal: what to do next. A function is called for
-- a line with a live number in it. A line too wide for the game view at a
-- 1280-wide window (about 390 of the client's layout units, with the longest
-- controller button words in) is split in two with "\n"; the client draws
-- each row centred. See docs/TUTORIAL_DECISIONS.md, "Status line width".
local PILL_BUILD = "Press the pillbox button or {QUICK_PILL},\nthen "
local STEP = {
  [2] = {
    a_refill = "Drive onto the green-marked base, then STOP\non it and wait "
      .. "while it refills.",
    b_take = "Drive onto the grey base to make it yours.",
    c_shoot = function()
      local b = game.base(B.b2c.n)
      local left = b and base_shots(b.armour) or 0
      return string.format("Shoot the red-marked base with {FIRE}:\n%d "
        .. "shot%s left.", left, left == 1 and "" or "s")
    end,
    c_take = "The base is empty. Drive onto it to make it yours.",
  },
  [3] = {
    harvest = "Click the tree button or press {QUICK_TREE},\nthen click a "
      .. "forest square in the marked grove.",
    road = "Click the road button or press {QUICK_ROAD},\nthen click a grass "
      .. "square.",
    wall = "Click the wall button or press {QUICK_WALL},\nthen click a grass "
      .. "square.",
    mine = "Click the mine button or press {QUICK_MINE},\nthen click a "
      .. "square to lay a mine.",
    shoot = "Shoot the marked wall with {FIRE}\nuntil it falls.",
  },
  [4] = {
    a_pick = "Drive over the dead pillbox to put it in your tank.",
    a_build = PILL_BUILD .. "click on the map to build it.",
    b_in = "Drive into the forest inside the red markers.",
    b_hide = function()
      return string.format("Sit still deep in the forest, in range.\nHidden: "
        .. "%d of 5 seconds.", math.floor(S.hide))
    end,
    c_kill = "Shoot the pillbox twice with {FIRE}.",
    c_pick = "Drive over the dead pillbox to pick it up.",
  },
  [5] = {
    a_watch = "Stop on the green square east of the road\nand watch the bot "
      .. "take the pillbox.",
    b_build = "Build your pillbox at the marked green square.\n"
      .. "Press the pillbox button or {QUICK_PILL},\nthen click the green "
      .. "square.",
    b_park = "Stop at the road tile highlighted\nand aim at the enemy "
      .. "pillbox.",
    b_kill = "Shoot the enemy pillbox with {FIRE}\nuntil it dies.",
    b_pick = "Drive over the dead pillbox to pick it up.",
  },
  [6] = {
    kill = "Put your crosshair on the craters, then\nfire as the man "
      .. "crosses them.",
  },
  [7] = {
    dead = "Drive over both dead pillboxes on the road in.",
    build = "Build your pillboxes near the border, in the\nbot's land "
      .. "(north-east), to take control.",
    base = "Shoot an enemy base empty with {FIRE},\nthen drive onto it.",
    all = function()
      local mine = 0
      for _, k in ipairs({ "b7_ne1", "b7_ne2", "b7_nw1", "b7_nw2", "b7_se1",
                           "b7_se2", "b7_sw1", "b7_sw2" }) do
        local b = game.base(B[k].n)
        if b ~= nil and b.owner == S.player then mine = mine + 1 end
      end
      return string.format("Take every enemy base: %d of 8 are yours.", mine)
    end,
  },
}
local STEP_GUN = "Press {GUN_UP} until your crosshair\nis at its longest range."
local STEP_SHELLS = "Low on shells? Stop on the base\nby the road to refill."
local STEP_DONE = "Station done. Follow the road north."
local STEP_WIN = "Tutorial complete. Leave from the\nmenu when you are ready."

local PILLS7 = { "p7_nw1", "p7_nw2", "p7_se1", "p7_se2", "p7_sw1", "p7_sw2" }
local BASES7 = { "b7_ne1", "b7_ne2", "b7_nw1", "b7_nw2", "b7_se1", "b7_se2",
                 "b7_sw1", "b7_sw2" }
-- The friendly bases by the road where the player shoots a lot.
local SUPPLY = { "b2a", "b3", "b4c", "b5", "b6" }

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
    -- Damaged from the start: two shots kill it in 4C, and can_die keeps
    -- it alive until the player has hidden from it in 4B.
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
    if not S.done[n][item[1]] then return false end
  end
  return true
end

-- The first unticked goal of station n, or nil.
local function current_goal(n)
  for _, item in ipairs(LIST[n]) do
    if not S.done[n][item[1]] then return item[1] end
  end
  return nil
end

-- ---------------------------------------------------------------------
-- Popups. A popup that follows something the player did waits about 3
-- seconds (POPUP_DELAY on_tick calls), so the player sees what happened
-- first; it shows at once if the player has already moved on to the next
-- goal or station.

local POPUP_DELAY = 150   -- on_tick runs 50 times a second

local function show(id)
  if S.popped[id] or S.player == nil then return end
  S.popped[id] = true
  local text = POP[id]
  if type(text) == "function" then text = text() end
  game.popup(text, S.player)
  if id == "s4c" then
    local t = player_tank()
    if t ~= nil then game.set_stocks(S.player, full_stock()) end
  end
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
    if r == nil then return "On " .. word end
    return string.format("On %s: speed %d", word, rule(r))
  elseif n == 2 then
    local b = game.base(B.b2c.n)
    if b ~= nil and b.owner ~= S.player and not S.done[2].c_shoot then
      return string.format("Enemy base: %d shots left", base_shots(b.armour))
    end
  elseif n == 3 then
    local w = PT.wall3
    local left = game.wall_shots(w[1], w[2])
    if left == nil or left <= 0 then return "Target wall: down" end
    return string.format("Target wall: %d shot%s left", left,
                         left == 1 and "" or "s")
  elseif n == 4 then
    local pb = game.pill(P.p4.n)
    if pb ~= nil and not pb.in_tank then
      return string.format("Pillbox armour: %d/%d", pb.armour,
                           rule("pill_max_armour"))
    end
  elseif n == 6 then
    return string.format("Men killed: %d", S.men)
  elseif n == 7 then
    local mine = 0
    for _, k in ipairs(BASES7) do
      local b = game.base(B[k].n)
      if b ~= nil and b.owner == S.player then mine = mine + 1 end
    end
    return string.format("Bases yours: %d/8", mine)
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
      if S.moving then return "STOP on the base: wait", "yellow" end
      if tank_full(t) then return "Full", "green" end
      return "Refilling: wait here", "yellow"
    end
    if t.shells < 5 and n >= 2 and n <= 6 then
      return "Low on shells: refill", "yellow"
    end
  end
  if n == 5 or n == 7 then return "RESET pen: start over", "grey" end
  return nil
end

local function draw_panel(force)
  if S.player == nil then return end
  local n = S.at
  local t = player_tank()
  local sdone = station_done(n)
  local list = {
    { "rect", 0, 0, 128, 14, sdone and "green" or "grey_dark", true },
    { "text", 64, 2, "white", "normal", "centre", "Station " .. TITLE[n] },
  }
  local key = { n, sdone and "D" or "-" }
  local y = 18
  for _, item in ipairs(LIST[n]) do
    local done = S.done[n][item[1]] == true
    local label = item[2]
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
    local line = string.format("%d of %d needed", math.min(c, TERRAINS_NEEDED),
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
      m = { { PT.wall3[1], PT.wall3[2], "yellow" } }
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
  if S.won then return STEP_WIN end
  if S.gun_remind then return STEP_GUN end
  local n = S.at
  if t ~= nil and t.shells < 5 and n >= 2 and n <= 6 and
     on_friendly_base(t) == nil and not (n == 2 and not S.done[2].a_refill)
     then
    return STEP_SHELLS
  end
  if n == 1 then
    if station_done(1) then return STEP_DONE end
    local c = 0
    for _, item in ipairs(LIST[1]) do
      if S.done[1][item[1]] then c = c + 1 end
    end
    return string.format("Drive onto the patches beside the road:\n%d of %d "
      .. "so far.", c, TERRAINS_NEEDED)
  end
  local g = current_goal(n)
  if g == nil then
    if n == 7 then return STEP_WIN end
    return STEP_DONE
  end
  local s = STEP[n] and STEP[n][g]
  if type(s) == "function" then s = s() end
  return s or ""
end

local function draw_status()
  if S.player == nil then return end
  local text = status_text(player_tank())
  if text == S.status then return end
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
  if not was_done and station_done(n) and n < 7 then
    local text = string.format("Station %d done. Follow the road north.", n)
    if n == 4 then text = text .. "\nShoot trees to clear the trees quickly." end
    game.announce(text, 4, S.player)
  end
  if S.at == n then
    local id = next_popup(n)
    if id ~= nil then popup_later(id) end
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
    return { cfg = "BLOCKER_ORTHOGONAL_ONLY=true;cfg=BOT_PINGS_DEFAULT=false",
             cfg2 = "0;cfg=STRATEGIC_PLACE_ENABLED=false" }
  elseif role == "bot6" then
    -- The Station 6 bot never fights: its man is the target.
    return { peace = tostring(S.player or "") }
  elseif role == "bot7" then
    -- The Station 7 bot shoots at the player's man and usually misses.
    -- LGM_MISS_WU / LGM_HIT_PCT: one aim in ten is true; the rest burst one
    -- square off him (behind him when he walks), so he sees the shots land
    -- around him. The man can die: about one shot in ten at him kills him
    -- (tests/scenario/tutorial_bot7_man). CAPTURE_LGM_HUNT stays off (as
    -- Easy has it): that hunt fires on its own line, which the miss knobs
    -- do not reach. Several cfg values share one init value, "A=1;cfg=B=2",
    -- at most 63 bytes.
    return { cfg = "LGM_MISS_WU=256;cfg=LGM_HIT_PCT=10"
                   .. ";cfg=CAPTURE_LGM_HUNT=false" }
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

-- Start the 5A demo take again: the target back, the blocker back in the
-- bot's tank, the bot home.
local function reset_5a()
  reset_pill("t5a_target")
  if live_tank(S.bot5a) ~= nil then
    hand_pill(S.bot5a, P.t5a_blocker.n)
  else
    reset_pill("t5a_blocker")
  end
  home_bot5a()
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

-- Station 5B: rebuild the player's blocker on its square at full armour,
-- and tell the player. Only while it is built and the take is not done.
local function rebuild_5b()
  S.b5b_dead_at = nil
  local d = S.done[5]
  if not d.b_build or d.b_pick or not blocker_5b_gone() then return end
  if pill_home("t5b_blocker", S.player, rule("pill_max_armour")) then
    game.announce("To help you out, your friendly blocking\npillbox has been "
                  .. "automatically rebuilt.", 4, S.player)
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
      -- Shots taken before the 4B hide are put back; once 4C is on, a hit
      -- stays so the second shot kills it.
      if pb.in_tank or pb.x ~= P.p4.x or pb.y ~= P.p4.y or
         (pb.armour ~= P4_ARMOUR and
          (not d.b_hide or pb.armour > P4_ARMOUR)) then
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
    S.sw = {}
    if live_tank(S.bot7) ~= nil then
      game.teleport_to_start(S.bot7, ST.bot7.n)
      game.set_stocks(S.bot7, full_stock())
    end
    S.won = false
  end
  if player_tank() ~= nil then
    game.set_stocks(S.player, station_loadout(n))
  end
  game.announce(string.format("Station %d reset", n), 2, S.player)
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

local function adopt_player(p)
  if S.player ~= nil then return end
  S.player = p
  local s = game.lobby_slot(p)
  if TUTORIAL_PLAYER == nil and s ~= nil and s.team == 2 then
    game.set_team(p, 1)
  end
  reserve_seats()
  hand_out()
  local t = game.tank(p)
  local n = 1
  if t ~= nil then n = station_of(t.mx, t.my) or 1 end
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

function on_tank_spawned(p, mx, my, respawn, scripted)
  if p == S.bot6 and S.fielded.bot6 then
    park_bot6()
  elseif p == S.bot5a and S.fielded.bot5a then
    game.set_stocks(p, full_stock())
    hand_pill(p, P.t5a_blocker.n)
    S.t5a_reset_at = S.sec
  elseif p == S.bot7 and S.fielded.bot7 then
    if not respawn then
      reset_base("b7_ne1"); reset_base("b7_ne2")
    end
  elseif p == S.player then
    S.last_pos = nil
    S.hide = 0
    if station_of(mx, my) == 5 then hand_5b() end
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
          game.announce("The enemy pillbox is back.\nTake it again, or go north.",
                        4, S.player)
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
          game.announce("Moved to the green square.", 3, S.player)
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

-- Tank shells pass between the player and the two demo bots (shells hit
-- allied tanks too, so the allied 5A bot is in the list). Two Station 5
-- rules more: the player's shells pass the player's own 5B blocker, and the
-- 5A pillboxes' shells pass the player. Every other pillbox shoots as it
-- always does, so the Station 6 bot's pillbox in Station 5B is a real enemy
-- pillbox.
-- 5B: only one in five of the target's shells takes armour off the
-- player's blocker. At the full rate the angry target kills a full blocker
-- in about 3 seconds, before a tank parked on the parking square has killed
-- the target (about 5 seconds of steady fire); this way the blocker lasts
-- about 16 seconds. A shell takes 1 armour and the engine rounds the scaled
-- amount, so a percent such as 20 would round to 0 on every shell: the rule
-- counts the shells instead. See docs/TUTORIAL_DECISIONS.md, "5B parking
-- square".
function pill_damage_scale(attacker, n, cause, pill)
  if n == P.t5b_blocker.n and pill == P.t5b_target.n then
    S.b5b_soaked = (S.b5b_soaked or 0) + 1
    if S.b5b_soaked % 5 == 0 then return 100 end
    return 0
  end
  return nil
end

function can_hit(attacker, kind, n, pill)
  if S.player == nil then return nil end
  if kind == "pill" then
    -- 5B: the player's shells pass the player's own blocker, so a tank
    -- parked straight behind it can hit the target. A real game has no such
    -- rule. See docs/TUTORIAL_DECISIONS.md, "5B parking square".
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

function can_die(kind, n, killer, cause, pill)
  if kind == "tank" and n == S.bot6 and S.bot6 ~= nil then return false end
  if kind == "pill" and n == P.p4.n and not S.done[4].b_hide then
    -- 4B: the pillbox stays at one armour until the player has hidden.
    if S.player ~= nil and S.sec - S.told_hide >= 3 then
      S.told_hide = S.sec
      game.announce("Hide in the forest first, then kill it.", 3, S.player)
    end
    return false
  end
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
    -- Arriving on the base says to stay there; a full tank on it ticks 2A.
    if t.mx == B.b2a.x and t.my == B.b2a.y and not S.done[2].a_refill then
      show("s2stay")
      if tank_full(t) then mark(2, "a_refill") end
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
    if pb ~= nil and not pb.in_tank then
      local cx = pb.x * 256 + 128
      local cy = pb.y * 256 + 128
      local d = math.sqrt((t.wx - cx) ^ 2 + (t.wy - cy) ^ 2)
      local inr = d <= rule("pill_range")
      if pb.armour > 0 then
        if inr and terrain_at(t.mx, t.my) == "forest" then mark(4, "b_in") end
        if inr and hidden_in_trees(t, cx, cy) then
          S.hide = S.hide + 0.2
          if S.hide >= 5 and S.done[4].b_in then mark(4, "b_hide") end
        else
          S.hide = 0
        end
      elseif S.done[4].b_hide then
        mark(4, "c_kill")
      end
    end
  elseif n == 5 then
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
