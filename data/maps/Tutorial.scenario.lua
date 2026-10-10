-- =========================================================================
-- Tutorial — a scripted single-player round on Tutorial.map.
--
-- Seven stations up one road, south to north. Each station shows a short
-- popup when the player first drives into it, a checklist on the scenario
-- panel, and a pad that puts the station back the way it started:
--
--   1  Driving      terrain patches and their speeds; deep water warning
--   2  Bases        2A your base (refill), 2B a grey base (take it),
--                   2C an enemy base (shoot it empty, take it); then the
--                   game types
--   3  Building     harvest, road, wall, mine, shoot down a wall
--   4  Pillboxes    4A a dead neutral pill to pick up and build,
--                   4B a live pill with forest over half of its range,
--                   4C kill it, repair at your base, pick it up
--   5  Taking a pill  5A a bot takes a pill on a loop while you watch,
--                   5B the same layout for you, with a walled RESET
--   6  The man      a parked bot's man harvests; kill him
--   7  Final round  a small Chew Toy island against one Easy bot
--
-- CHECKPOINTS. The furthest station the player has driven into is the
-- checkpoint: on_choose_start puts every respawn there, and spawn_loadout
-- gives the tank that station's loadout. Each checkpoint is a pool in the
-- deep-water trench down the west side, one pool per station, so a tank
-- that respawns on its boat cannot sail into another station.
--
-- BOTS. Three held seats on team 2 (scenario.lobby): the Station 5A demo
-- bot and the Station 6 bot are fielded at the start; the Station 7 bot is
-- fielded the first time the player reaches Station 7. The two demo bots
-- never shoot the player and the player's shells pass through them
-- (can_hit), and they are told to leave the player alone with GoalHunter's
-- "peace=" init token. Where the lobby held no seats (a host trimmed them),
-- a free seat is used instead.
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
    p4a = { n = 1, x = 112, y = 148, armour = 0 },
    p4 = { n = 2, x = 141, y = 141, armour = 15 },
    t5a_target = { n = 3, x = 147, y = 101, armour = 15 },
    t5a_friend = { n = 4, x = 148, y = 105, armour = 15 },
    t5b_target = { n = 5, x = 112, y = 100, armour = 15 },
    t5b_friend = { n = 6, x = 113, y = 104, armour = 15 },
    p7_ne1 = { n = 7, x = 134, y = 39, armour = 15 },
    p7_ne2 = { n = 8, x = 136, y = 41, armour = 15 },
    p7_nw1 = { n = 9, x = 122, y = 39, armour = 15 },
    p7_nw2 = { n = 10, x = 120, y = 41, armour = 15 },
    p7_se1 = { n = 11, x = 134, y = 55, armour = 15 },
    p7_se2 = { n = 12, x = 136, y = 53, armour = 15 },
    p7_sw1 = { n = 13, x = 116, y = 51, armour = 0 },
    p7_sw2 = { n = 14, x = 116, y = 54, armour = 0 },
  },
  base = {
    b2a = { n = 1, x = 121, y = 195 },
    b2b = { n = 2, x = 137, y = 189 },
    b2c = { n = 3, x = 120, y = 185 },
    b4c = { n = 4, x = 149, y = 148 },
    b7_ne1 = { n = 5, x = 132, y = 35 },
    b7_ne2 = { n = 6, x = 140, y = 43 },
    b7_nw1 = { n = 7, x = 124, y = 35 },
    b7_nw2 = { n = 8, x = 116, y = 43 },
    b7_se1 = { n = 9, x = 132, y = 59 },
    b7_se2 = { n = 10, x = 140, y = 51 },
    b7_sw1 = { n = 11, x = 124, y = 55 },
    b7_sw2 = { n = 12, x = 118, y = 51 },
  },
  start = {
    cp1 = { n = 1, x = 98, y = 223 },
    cp2 = { n = 2, x = 98, y = 199 },
    cp3 = { n = 3, x = 98, y = 176 },
    cp4 = { n = 4, x = 98, y = 155 },
    bot5a = { n = 5, x = 152, y = 116 },
    cp5 = { n = 6, x = 98, y = 123 },
    bot6 = { n = 7, x = 154, y = 81 },
    cp6 = { n = 8, x = 98, y = 89 },
    bot7 = { n = 9, x = 142, y = 32 },
    cp7 = { n = 10, x = 112, y = 52 },
  },
  point = {
    bot6_park = { 151, 81 },
    p4 = { 141, 141 },
    s1_shore = { 102, 223 },
    s7_arrive = { 114, 52 },
    t5a_friend = { 148, 105 },
    t5a_home = { 147, 114 },
    t5a_target = { 147, 101 },
    t5b_friend = { 113, 104 },
    t5b_target = { 112, 100 },
    wall3 = { 148, 168 },
  },
  region = {
    b2a_area = { x = 118, y = 194, w = 7, h = 4 },
    b2b_area = { x = 134, y = 188, w = 7, h = 4 },
    b2c_area = { x = 116, y = 184, w = 9, h = 4 },
    gate6 = { x = 105, y = 73, w = 5, h = 3 },
    grove3 = { x = 104, y = 163, w = 15, h = 10 },
    grove6 = { x = 134, y = 76, w = 13, h = 12 },
    island5a = { x = 140, y = 99, w = 14, h = 19 },
    island7 = { x = 107, y = 27, w = 43, h = 41 },
    p4_forest = { x = 133, y = 133, w = 6, h = 17 },
    p4_open = { x = 142, y = 133, w = 8, h = 17 },
    p4a_area = { x = 108, y = 144, w = 9, h = 9 },
    pad1 = { x = 117, y = 209, w = 2, h = 2 },
    pad2 = { x = 146, y = 197, w = 2, h = 2 },
    pad3 = { x = 146, y = 175, w = 2, h = 2 },
    pad4 = { x = 116, y = 136, w = 2, h = 2 },
    pad6 = { x = 116, y = 84, w = 2, h = 2 },
    quarter_ne = { x = 129, y = 27, w = 21, h = 20 },
    reset5b = { x = 111, y = 119, w = 3, h = 2 },
    reset7 = { x = 117, y = 63, w = 3, h = 2 },
    s1 = { x = 97, y = 205, w = 59, h = 23 },
    s2 = { x = 97, y = 182, w = 59, h = 20 },
    s3 = { x = 97, y = 161, w = 59, h = 18 },
    s4 = { x = 97, y = 130, w = 59, h = 28 },
    s5 = { x = 97, y = 95, w = 59, h = 32 },
    s5b = { x = 97, y = 95, w = 29, h = 32 },
    s6 = { x = 97, y = 72, w = 59, h = 20 },
    s7 = { x = 97, y = 25, w = 59, h = 47 },
    t5a_patch = { x = 146, y = 107, w = 3, h = 3 },
    t5b_patch = { x = 111, y = 106, w = 3, h = 3 },
    watch5a = { x = 129, y = 98, w = 6, h = 21 },
  },
  walls5b = { {103,112}, {104,112}, {103,113}, {105,113}, {103,114}, {104,114}, {103,115}, {105,115}, {103,116}, {105,116}, {107,112}, {108,112}, {109,112}, {107,113}, {107,114}, {108,114}, {107,115}, {107,116}, {108,116}, {109,116}, {112,112}, {113,112}, {111,113}, {112,114}, {113,115}, {111,116}, {112,116}, {115,112}, {116,112}, {117,112}, {115,113}, {115,114}, {116,114}, {115,115}, {115,116}, {116,116}, {117,116}, {119,112}, {120,112}, {121,112}, {120,113}, {120,114}, {120,115}, {120,116} },
  walls7 = { {109,56}, {110,56}, {109,57}, {111,57}, {109,58}, {110,58}, {109,59}, {111,59}, {109,60}, {111,60}, {113,56}, {114,56}, {115,56}, {113,57}, {113,58}, {114,58}, {113,59}, {113,60}, {114,60}, {115,60}, {118,56}, {119,56}, {117,57}, {118,58}, {119,59}, {117,60}, {118,60}, {121,56}, {122,56}, {123,56}, {121,57}, {121,58}, {122,58}, {121,59}, {121,60}, {122,60}, {123,60}, {125,56}, {126,56}, {127,56}, {126,57}, {126,58}, {126,59}, {126,60} },
  grove3 = { 104, 163, 118, 172 },
}
-- END LAYOUT

scenario = {
  name = "Tutorial",
  description = "Learn WinBolo one step at a time: driving, bases, "
    .. "building, pillboxes, taking a pillbox and the builder, then a "
    .. "small game against an Easy bot.",
  api  = 1,
  kind = "scenario",
  game = "open",
  -- The demo bots and the final-round bot are held seats this file fields
  -- with game.spawn_bot, so the lobby has to allow bots.
  needs_bots = true,

  lobby = {
    max_players = 1,
    teams = {
      { id = 1, bots = 0 },
      { id = 2, bots = 3, max_bots = 3, fielded = false,
        brain = "GoalHunter", mode = "default", difficulty = "hard" },
    },
  },

  regions = LAYOUT.region,

  callbacks = {
    on_start = "Hands out the bases and pillboxes and fields the two demo bots.",
    on_player_join = "Sets the tutorial up for a player who joins after the round has started.",
    on_tick = "Ticks off the checklist, draws the panel, and runs the demo bots' loops.",
    on_enter_region = "Shows each station's popup the first time you arrive, moves your checkpoint, and runs the reset pads.",
    on_tank_spawned = "Parks the Station 6 bot and gives each bot what it owns.",
    on_tank_hit = "Notices when the Station 4 pillbox shoots you while you hide.",
    on_built = "Ticks off harvesting, roads and walls in Station 3.",
    on_mine_laid = "Ticks off laying a mine in Station 3.",
    on_lgm_died = "Counts the men you kill in Station 6.",
    on_pill_picked_up = "Ticks off pillbox pick-ups and resets the pillbox takes in Station 5.",
    on_pill_placed = "Ticks off building a pillbox, and sends a pillbox taken out of its station back home.",
    on_pill_captured = "Ticks off taking an enemy pillbox in Station 7.",
    on_base_captured = "Ticks off taking bases, and wins the tutorial when you own every base in Station 7.",
    can_build = "Keeps each bot's builder inside its own station, and stops boats in Station 5.",
    can_hit = "Tank shells pass between you and the two demo bots.",
    can_die = "The parked Station 6 tank cannot be destroyed.",
    on_choose_start = "You respawn at the furthest station you have reached.",
    spawn_loadout = "You respawn with the tank that station starts you with.",
    allow_base_win = "Owning every base on the map does not end the round; Station 7 has its own win.",
  },
}

-- ---------------------------------------------------------------------
-- Round state. The host builds a fresh Lua state for every round, so none
-- of this needs a reset between rounds.
local S = {
  player   = nil,     -- the tutorial's player seat
  reached  = 1,       -- the furthest station reached: the checkpoint
  at       = 1,       -- the station the player last drove into
  done     = { {}, {}, {}, {}, {}, {}, {} },  -- checklist, by station
  popped   = {},      -- popup id -> true once shown
  carrier  = {},      -- pill n -> seat carrying it
  bot5a = nil, bot6 = nil, bot7 = nil,
  bot7_asked = false,
  frames   = 0,       -- on_tick calls
  sec      = 0,       -- whole seconds since the start
  terrains = {},      -- Station 1: terrain names driven on
  low2     = false,   -- Station 2: the low loadout has been given
  last_stock = nil,   -- stocks at the last poll, for refill and repair
  hide     = 0,       -- Station 4: seconds hidden in the forest in range
  men      = 0,       -- Station 6: men killed
  t5a_reset_at = 0,   -- Station 5A: the second of the last reset
  t5b_restore = false,  -- Station 5B: a restore of E is waiting
  sw       = {},      -- Station 7: dead pillboxes picked up
  won      = false,
  panel_key = nil,
  markers  = 0,       -- how many marker ids are in use
}

-- ---------------------------------------------------------------------
-- Texts.
local POP = {
  s1 = "{ACCEL} forward, {BRAKE} slow down, {LEFT} and {RIGHT} turn.\n\n"
    .. "Deep water sinks tanks. If you die anywhere here, you come back "
    .. "at your last checkpoint.",
  s2a = "Drive onto your base to refill shells, mines and armour.",
  s2b = "Grey bases belong to nobody. Drive onto one to make it yours.",
  s2c = "Enemy bases must be shot empty first. Then drive over to make it "
    .. "yours.",
  s2d = "Bases are your supply line, and what you respawn with depends on "
    .. "the game type.\n\n"
    .. "Open: tanks always respawn fully loaded.\n\n"
    .. "Tournament: the more bases are taken, the fewer shells a tank "
    .. "respawns with; once all bases are taken, tanks respawn without "
    .. "shells.\n\n"
    .. "Strict tournament: tanks never respawn with shells.\n\n"
    .. "A team that takes every base wins, because the enemy can't refuel.",
  s3 = "Your man harvests trees, then uses them to build roads, walls, "
    .. "mines and pillboxes. Quick keys: {QUICK_TREE} trees, {QUICK_ROAD} "
    .. "road, {QUICK_WALL} wall, {QUICK_MINE} mine. Pillboxes come later.",
  s4a = "Games start with neutral pillboxes, and each takes 15 shots to "
    .. "kill.\n\n"
    .. "Drive over the dead pillbox here to pick it up, then build it "
    .. "({QUICK_PILL}) and it is yours.",
  s4b = "A pillbox you hit gets angry and fires faster. The markers show "
    .. "this one's range.\n\n"
    .. "A pillbox cannot see a tank that is all in forest and 3 or more "
    .. "squares away. Firing shows you for about 2 seconds.",
  s4c = "Your armour is the limit: back out to your base to repair, or "
    .. "attack from the forest edge.\n\n"
    .. "Your tank is full. Kill the pillbox, repair once, then pick it up.",
  s5a = "Watch the bot on the island take a pillbox: it knocks the "
    .. "pillbox out, then drives over it to pick it up.",
  s5b = "Your pillbox rebuilds itself if it dies. Park on the road and copy "
    .. "the bot. Drive into RESET to start over.",
  s6 = "Shoot or run over an enemy man to kill him. Then his tank can't "
    .. "build until a new man arrives.\n\n"
    .. "The cyan mark in the north-west takes you to the last station.",
  s7 = "Use your pillboxes to take the enemy's land, and steal their bases "
    .. "like in Station 2. You win when you own every base. Drive into "
    .. "RESET to start over.\n\n"
    .. "Build a pillbox with {QUICK_PILL}.",
  win = "Well done: you own every base, and the tutorial is complete.\n\n"
    .. "Go back to the lobby and play a game against Easy bots next.",
}

local TITLE = { "1: Driving", "2: Bases", "3: Building", "4: Pillboxes",
                "5: Taking a pill", "6: The man", "7: Final round" }

-- The checklist of each station: { key, label }. A label is short enough
-- for one panel line in the small font.
local LIST = {
  { { "terr", "Try 3 terrains" } },
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
    { "b_in", "Enter range, open side" },
    { "b_out", "Back out of range" },
    { "b_hide", "Sit in forest in range" },
    { "c_kill", "Kill the pillbox" },
    { "c_repair", "Repair at your base" },
    { "c_pick", "Pick the pillbox up" } },
  { { "a_watch", "Watch the bot take one" },
    { "b_park", "Park on the road" },
    { "b_kill", "Kill the enemy pill" },
    { "b_pick", "Pick it up" } },
  { { "kill", "Kill the man" } },
  { { "dead", "Pick up both dead pills" },
    { "build", "Pill in enemy land" },
    { "pill", "Take an enemy pill" },
    { "base", "Steal an enemy base" },
    { "all", "Take every enemy base" } },
}

-- Entities by name, from the layout.
local P = LAYOUT.pill
local B = LAYOUT.base
local ST = LAYOUT.start

local PILLS7 = { "p7_ne1", "p7_ne2", "p7_nw1", "p7_nw2", "p7_se1", "p7_se2",
                 "p7_sw1", "p7_sw2" }
local BASES7 = { "b7_ne1", "b7_ne2", "b7_nw1", "b7_nw2", "b7_se1", "b7_se2",
                 "b7_sw1", "b7_sw2" }

-- The station a pill belongs to: one carried out of it, or put down outside
-- it, is sent back.
local PILL_HOME = { p4a = 4, p4 = 4, t5a_target = 5, t5a_friend = 5,
                    t5b_target = 5, t5b_friend = 5 }
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

-- ---------------------------------------------------------------------
-- Small helpers.
local function rule(name) return game.rule(name) end

local function full_stock()
  return { shells = rule("tank_full_shells"), mines = rule("tank_full_mines"),
           armour = rule("tank_full_armour"), trees = rule("tank_full_trees") }
end

-- What a tank starts each station with. Station 2 starts low until its
-- base refill is done, so there is something to refill; Station 1 to 3
-- carry no trees, so Station 3's harvest is needed.
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

-- The owner each station's pills and bases start with.
local function pill_start(name)
  if name == "p4a" or name == "p7_sw1" or name == "p7_sw2" then
    return nil, 0
  elseif name == "p4" or name == "t5a_target" then
    return nil, rule("pill_max_armour")
  elseif name == "t5a_friend" then
    return S.bot5a, rule("pill_max_armour")
  elseif name == "t5b_target" then
    return S.bot6, rule("pill_max_armour")
  elseif name == "t5b_friend" then
    return S.player, rule("pill_max_armour")
  elseif name == "p7_ne1" or name == "p7_ne2" then
    return S.bot7, rule("pill_max_armour")
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

-- ---------------------------------------------------------------------
-- The panel and the markers.

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
  elseif n == 3 then
    local w = LAYOUT.point.wall3
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

local function draw_panel(force)
  if S.player == nil then return end
  local n = S.at
  local t = player_tank()
  local list = {
    { "rect", 0, 0, 128, 14, "grey_dark", true },
    { "text", 64, 2, "white", "normal", "centre", "Station " .. TITLE[n] },
  }
  local key = { n }
  local y = 18
  for _, item in ipairs(LIST[n]) do
    local done = S.done[n][item[1]] == true
    local label = item[2]
    if n == 1 then
      local c = 0
      for _ in pairs(S.terrains) do c = c + 1 end
      if c > 3 then c = 3 end
      label = string.format("%s (%d/3)", label, c)
    end
    list[#list + 1] = { "rect", 4, y + 1, 6, 6, done and "green" or "grey", done }
    list[#list + 1] = { "text", 13, y, done and "green" or "white", "small",
                        "left", label }
    key[#key + 1] = label .. (done and "1" or "0")
    y = y + 10
  end
  local info = station_info(n, t)
  if info ~= nil then
    list[#list + 1] = { "text", 64, 106, "yellow", "small", "centre", info }
    key[#key + 1] = info
  end
  local hint
  if n == 5 or n == 7 then
    hint = "RESET: start over"
  else
    hint = "Rubble pad: reset station"
  end
  list[#list + 1] = { "text", 64, 117, "grey", "small", "centre", hint }
  key[#key + 1] = hint
  local k = table.concat(key, "|")
  if not force and k == S.panel_key then return end
  if game.panel(0, list, S.player) then S.panel_key = k end
end

local RING = {}
do
  local cx, cy = LAYOUT.point.p4[1], LAYOUT.point.p4[2]
  for i = 0, 15 do
    local a = i * math.pi / 8
    RING[#RING + 1] = { math.floor(cx + 8 * math.cos(a) + 0.5),
                        math.floor(cy + 8 * math.sin(a) + 0.5) }
  end
end

local function set_markers(n)
  if S.player == nil then return end
  for id = 0, S.markers - 1 do game.clear_marker(id, S.player) end
  local m = {}
  if n == 2 then
    m = { { B.b2a.x, B.b2a.y, "green" }, { B.b2b.x, B.b2b.y, "grey" },
          { B.b2c.x, B.b2c.y, "red" } }
  elseif n == 3 then
    m = { { LAYOUT.point.wall3[1], LAYOUT.point.wall3[2], "yellow" } }
  elseif n == 4 then
    for _, r in ipairs(RING) do m[#m + 1] = { r[1], r[2], "red" } end
  elseif n == 5 then
    local r = LAYOUT.region.t5b_patch
    m = { { r.x + 1, r.y + 1, "green" } }
  elseif n == 6 then
    local r = LAYOUT.region.gate6
    m = { { r.x + 2, r.y + 1, "cyan" } }
  end
  for i, v in ipairs(m) do game.marker(i - 1, v[1], v[2], v[3], S.player) end
  S.markers = #m
end

-- ---------------------------------------------------------------------
-- Popups and the checklist.

local function popup(id)
  if S.popped[id] or S.player == nil then return end
  S.popped[id] = true
  game.popup(POP[id], S.player)
end

local function all_done(n)
  for _, item in ipairs(LIST[n]) do
    if not S.done[n][item[1]] then return false end
  end
  return true
end

-- The station's next popup, for what the player has done so far.
local function advance(n)
  local d = S.done[n]
  if n == 1 then
    popup("s1")
  elseif n == 2 then
    if d.c_take then
      popup("s2d")
    elseif not d.a_refill then
      popup("s2a")
    elseif not d.b_take then
      popup("s2b")
    else
      popup("s2c")
    end
  elseif n == 3 then
    popup("s3")
  elseif n == 4 then
    if not (d.a_pick and d.a_build) then
      popup("s4a")
    elseif not (d.b_in and d.b_out and d.b_hide) then
      popup("s4b")
    elseif not S.popped.s4c then
      popup("s4c")
      if S.player ~= nil then game.set_stocks(S.player, full_stock()) end
    end
  elseif n == 5 then
    popup("s5a")
    if d.a_watch then popup("s5b") end
  elseif n == 6 then
    popup("s6")
  elseif n == 7 then
    popup("s7")
  end
end

local function mark(n, key)
  if S.done[n][key] then return end
  S.done[n][key] = true
  game.sound("lobby_ready")
  if all_done(n) and n < 7 then
    game.announce(string.format("Station %d done. Follow the road north.", n),
                  4, S.player)
  end
  if S.at == n then advance(n) end
  draw_panel(false)
end

-- ---------------------------------------------------------------------
-- Station resets.

local function reset_station(n)
  S.done[n] = {}
  if n == 1 then
    S.terrains = {}
  elseif n == 2 then
    reset_base("b2a"); reset_base("b2b"); reset_base("b2c")
  elseif n == 3 then
    local w = LAYOUT.point.wall3
    game.set_tile(w[1], w[2], game.TERRAIN.building)
  elseif n == 4 then
    reset_pill("p4a"); reset_pill("p4"); reset_base("b4c")
    S.hide = 0
  elseif n == 5 then
    reset_pill("t5b_target"); reset_pill("t5b_friend")
    rebuild_walls(LAYOUT.walls5b)
    -- Watching the bot is not undone: it keeps taking pills either way.
    S.done[5].a_watch = S.popped.s5b and true or nil
  elseif n == 6 then
    S.men = 0
  elseif n == 7 then
    for _, k in ipairs(PILLS7) do reset_pill(k) end
    for _, k in ipairs(BASES7) do reset_base(k) end
    rebuild_walls(LAYOUT.walls7)
    S.sw = {}
    if S.bot7 ~= nil then
      local t = game.tank(S.bot7)
      if t ~= nil and not t.dead then
        game.teleport_to_start(S.bot7, ST.bot7.n)
        game.set_stocks(S.bot7, full_stock())
      end
    end
    S.won = false
  end
  if S.player ~= nil and player_tank() ~= nil then
    game.set_stocks(S.player, station_loadout(n))
  end
  game.announce(string.format("Station %d reset", n), 2, S.player)
  draw_panel(true)
end

-- ---------------------------------------------------------------------
-- The bots.

local function team2_seats()
  -- A seat spawn_bot was just given still reads as held, and an empty one
  -- as empty, until the sim fields it a tick or more later, so the seats
  -- already handed to a role are skipped by name.
  local held, free = {}, {}
  for p = 0, game.max_tanks() - 1 do
    local s = game.lobby_slot(p)
    if p == S.bot5a or p == S.bot6 or p == S.bot7 then
      -- taken
    elseif s == nil then
      free[#free + 1] = p
    elseif s.bot and s.team == 2 and not s.fielded and p ~= TUTORIAL_PLAYER
           then
      held[#held + 1] = p
    end
  end
  return held, free
end

-- Field one bot in the next held seat, or a free one. Answers the seat.
local function field_bot(role, start, extra)
  local held, free = team2_seats()
  local t = { start = start, loadout = "open" }
  for k, v in pairs(extra or {}) do t[k] = v end
  if held[1] ~= nil then
    t.slot = held[1]
  elseif free[1] ~= nil then
    t.slot, t.team, t.brain, t.name = free[1], 2, "GoalHunter", role
  else
    game.log("Tutorial: no seat for the " .. role .. " bot")
    return nil
  end
  local p, code = game.spawn_bot(t)
  if type(p) ~= "number" then
    game.log("Tutorial: spawn of the " .. role .. " bot refused: "
             .. tostring(code))
    return nil
  end
  return p
end

local function peace_init()
  return { peace = tostring(S.player or "") }
end

local function park_bot6()
  local at = LAYOUT.point.bot6_park
  game.teleport(S.bot6, at[1], at[2], 128)
  -- 1 percent, not 0: a modifier of 0 is read as the classic 100.
  game.set_modifiers(S.bot6, { speed = 1, turn = 1 })
end

local function home_bot5a()
  local h = LAYOUT.point.t5a_home
  local t = game.tank(S.bot5a)
  if t ~= nil and not t.dead then
    game.teleport(S.bot5a, h[1], h[2], 64)
    game.set_stocks(S.bot5a, full_stock())
  end
end

local function reset_5a()
  reset_pill("t5a_target")
  reset_pill("t5a_friend")
  home_bot5a()
  S.t5a_reset_at = S.sec
end

local function field_bot7()
  if S.bot7_asked then return end
  S.bot7_asked = true
  S.bot7 = field_bot("Final", ST.bot7.n,
                     { mode = "default", difficulty = "easy" })
end

-- The Station 6 man: send him to the nearest forest square of the grove
-- whenever he is in the tank. Refill the grove once it runs low.
local function bot6_loop()
  if S.bot6 == nil then return end
  local t = game.tank(S.bot6)
  if t == nil or t.dead then return end
  -- A 1 percent speed still creeps a square every ten seconds or so.
  local at = LAYOUT.point.bot6_park
  if math.abs(t.mx - at[1]) + math.abs(t.my - at[2]) > 1 then
    game.teleport(S.bot6, at[1], at[2])
  end
  local b = game.builder(S.bot6)
  if b == nil or b.state ~= "in_tank" then return end
  local g = LAYOUT.region.grove6
  local best, bx, by, count = nil, nil, nil, 0
  for y = g.y, g.y + g.h - 1 do
    for x = g.x, g.x + g.w - 1 do
      if game.map_tile(x, y) == game.TERRAIN.forest then
        count = count + 1
        local d = (x - at[1]) ^ 2 + (y - at[2]) ^ 2
        if best == nil or d < best then best, bx, by = d, x, y end
      end
    end
  end
  if count < 40 then
    game.fill_rect(g.x, g.y, g.x + g.w - 1, g.y + g.h - 1,
                   game.TERRAIN.forest)
    return
  end
  game.set_stocks(S.bot6, { trees = 0 })
  game.builder_order(S.bot6, "trees", bx, by)
end

-- ---------------------------------------------------------------------
-- Setting the round up for the player.

local function hand_out()
  reset_base("b2a"); reset_base("b2b"); reset_base("b2c"); reset_base("b4c")
  for _, k in ipairs({ "p4a", "p4", "t5a_target", "t5a_friend", "t5b_target",
                       "t5b_friend" }) do
    reset_pill(k)
  end
  for _, k in ipairs(PILLS7) do reset_pill(k) end
  for _, k in ipairs(BASES7) do reset_base(k) end
end

local function adopt_player(p)
  if S.player ~= nil then return end
  S.player = p
  local s = game.lobby_slot(p)
  if TUTORIAL_PLAYER == nil and s ~= nil and s.team == 2 then
    game.set_team(p, 1)
  end
  hand_out()
  if S.bot5a == nil then
    S.bot5a = field_bot("Demo", ST.bot5a.n, { init = peace_init() })
  end
  if S.bot6 == nil then
    S.bot6 = field_bot("Builder", ST.bot6.n, { init = peace_init() })
  end
  local t = game.tank(p)
  if t ~= nil then
    S.at = station_of(t.mx, t.my) or 1
  end
  advance(S.at)
  set_markers(S.at)
  draw_panel(true)
end

-- A pill carried out of its station, or put down outside it, goes home.
local function tidy_pills(station)
  for name, home in pairs(PILL_HOME) do
    if home ~= station then
      local n = P[name].n
      local pb = game.pill(n)
      if pb ~= nil and pb.in_tank and S.carrier[n] == S.player then
        reset_pill(name)
      end
    end
  end
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
  if p == S.bot6 then
    park_bot6()
    if not respawn then
      reset_base("b2c")
      reset_pill("t5b_target")
    end
  elseif p == S.bot5a then
    if not respawn then reset_pill("t5a_friend") end
    S.t5a_reset_at = S.sec
  elseif p == S.bot7 then
    if not respawn then
      reset_pill("p7_ne1"); reset_pill("p7_ne2")
      reset_base("b7_ne1"); reset_base("b7_ne2")
    end
  elseif p == S.player then
    S.last_stock = nil
    S.hide = 0
  end
end

local function enter_station(n)
  if n > S.reached then S.reached = n end
  if n ~= S.at then
    S.at = n
    set_markers(n)
  end
  tidy_pills(n)
  if n == 2 and not S.low2 and not S.done[2].a_refill then
    S.low2 = true
    game.set_stocks(S.player, station_loadout(2))
  end
  if n == 7 then field_bot7() end
  advance(n)
  draw_panel(true)
end

function on_enter_region(p, name)
  if p ~= S.player then return end
  local n = string.match(name, "^s(%d)$")
  if n ~= nil then
    enter_station(tonumber(n))
    return
  end
  local pad = string.match(name, "^pad(%d)$")
  if pad ~= nil then
    reset_station(tonumber(pad))
  elseif name == "reset5b" then
    reset_station(5)
  elseif name == "reset7" then
    reset_station(7)
  elseif name == "s5b" then
    popup("s5b")
  elseif name == "t5b_patch" then
    mark(5, "b_park")
  elseif name == "gate6" then
    local a = LAYOUT.point.s7_arrive
    game.teleport(p, a[1], a[2], 0)
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
  if p == S.bot6 and S.player ~= nil and
     (killer == S.player or S.at == 6) then
    S.men = S.men + 1
    mark(6, "kill")
    draw_panel(false)
  end
end

function on_pill_picked_up(n, p, scripted)
  S.carrier[n] = p
  if scripted then return end
  local name = PILL_NAME[n]
  if p == S.bot5a then
    if name == "t5a_target" then
      -- The demo take is done: put the island back and go again.
      if S.player ~= nil and S.at == 5 then mark(5, "a_watch") end
      reset_5a()
    elseif name == "t5a_friend" then
      reset_pill("t5a_friend")
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
      game.timer(3, function()
        S.t5b_restore = false
        if reset_pill("t5b_target") then
          game.announce("The enemy pillbox is back. Take it again, or go north.",
                        4, S.player)
        end
      end)
    end
  elseif name == "p7_sw1" or name == "p7_sw2" then
    -- Each counts once picked up, so building the first before picking up
    -- the second still ticks the item.
    S.sw[name] = true
    if S.sw.p7_sw1 and S.sw.p7_sw2 then mark(7, "dead") end
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
  end
  if p ~= S.player or armour == 0 or pb == nil then return end
  if name == "p4a" then mark(4, "a_build") end
  if region_has("quarter_ne", pb.x, pb.y) then mark(7, "build") end
end

function on_pill_captured(n, old, new, scripted)
  if scripted then return end
  if new == S.player and old == S.bot7 and S.bot7 ~= nil then
    mark(7, "pill")
  end
end

local function check_win()
  if S.won or S.player == nil then return end
  for _, k in ipairs(BASES7) do
    local b = game.base(B[k].n)
    if b == nil or b.owner ~= S.player then return end
  end
  S.won = true
  mark(7, "all")
  popup("win")
  game.announce("Tutorial complete!", 6, S.player, "center")
  game.timer(30, function()
    if S.won then game.end_round("Tutorial complete", 1) end
  end)
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
  if p == S.bot6 then
    return action == "trees" and region_has("grove6", x, y)
  elseif p == S.bot5a then
    return action ~= "boat" and region_has("island5a", x, y)
  elseif p == S.bot7 and S.bot7 ~= nil then
    return action ~= "boat" and region_has("island7", x, y)
  elseif p == S.player and action == "boat" and region_has("s5", x, y) then
    return false
  end
  return nil
end

-- Only tank shells are filtered. A pillbox shoots as it always does, so the
-- Station 6 bot's pillbox in Station 5B is a real enemy pillbox.
function can_hit(attacker, kind, n, pill)
  if kind ~= "tank" or pill ~= nil or S.player == nil then return nil end
  if (attacker == S.player and is_demo_bot(n)) or
     (n == S.player and is_demo_bot(attacker)) then
    return false
  end
  return nil
end

function can_die(kind, n, killer, cause, pill)
  if kind == "tank" and n == S.bot6 and S.bot6 ~= nil then return false end
  return nil
end

function on_choose_start(p)
  if p == S.bot5a and p ~= nil then return ST.bot5a.n end
  if p == S.bot6 and p ~= nil then return ST.bot6.n end
  if p == S.bot7 and p ~= nil then return ST.bot7.n end
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

local function poll()
  local t = player_tank()
  if t == nil then return end
  local n = S.at
  local stock = t
  local last = S.last_stock
  if n == 1 then
    local word = terrain_at(t.mx, t.my)
    if t.boat then word = "boat" end
    if word == "forest" or word == "swamp" or word == "rubble" or
       word == "crater" or word == "river" then
      S.terrains[word] = true
      local c = 0
      for _ in pairs(S.terrains) do c = c + 1 end
      if c >= 3 then mark(1, "terr") end
    end
  elseif n == 2 then
    if last ~= nil and t.mx == B.b2a.x and t.my == B.b2a.y and
       (stock.shells > last.shells or stock.armour > last.armour or
        stock.mines > last.mines) then
      mark(2, "a_refill")
    end
    local b = game.base(B.b2c.n)
    if b ~= nil and b.owner ~= S.player and
       b.armour <= rule("base_capture_armour") then
      mark(2, "c_shoot")
    end
  elseif n == 3 then
    local w = LAYOUT.point.wall3
    local left = game.wall_shots(w[1], w[2])
    if left ~= nil and left <= 0 then mark(3, "shoot") end
  elseif n == 4 then
    local pb = game.pill(P.p4.n)
    if pb ~= nil and not pb.in_tank then
      local cx = pb.x * 256 + 128
      local cy = pb.y * 256 + 128
      local d = math.sqrt((t.wx - cx) ^ 2 + (t.wy - cy) ^ 2)
      local range = rule("pill_range")
      if pb.armour > 0 then
        if d <= range and region_has("p4_open", t.mx, t.my) then
          mark(4, "b_in")
        end
        if S.done[4].b_in and d > range then mark(4, "b_out") end
        if d <= range and terrain_at(t.mx, t.my) == "forest" then
          S.hide = S.hide + 0.2
          if S.hide >= 5 then mark(4, "b_hide") end
        else
          S.hide = 0
        end
      else
        mark(4, "c_kill")
      end
    end
    if last ~= nil and t.mx == B.b4c.x and t.my == B.b4c.y and
       stock.armour > last.armour then
      mark(4, "c_repair")
    end
  elseif n == 5 then
    local pb = game.pill(P.t5b_target.n)
    if pb ~= nil and not pb.in_tank and pb.armour == 0 then
      mark(5, "b_kill")
    end
  end
  S.last_stock = { shells = stock.shells, armour = stock.armour,
                   mines = stock.mines }
  draw_panel(false)
end

-- Once a second: the demo loops.
local function every_second()
  S.sec = S.sec + 1
  if S.sec % 3 == 0 then bot6_loop() end
  -- 5A watchdog: a demo take that stalls is started again.
  if S.bot5a ~= nil and S.sec - S.t5a_reset_at >= 150 then reset_5a() end
  -- 5B: the friendly pill rebuilds itself every 10 s.
  if S.player ~= nil and S.sec % 10 == 0 then
    local L = P.t5b_friend
    local pb = game.pill(L.n)
    if pb ~= nil and (pb.in_tank or pb.armour == 0 or pb.x ~= L.x or
                      pb.y ~= L.y) and S.carrier[L.n] ~= S.player then
      reset_pill("t5b_friend")
    end
  end
end

function on_tick(tick)
  S.frames = S.frames + 1
  if S.frames % 10 == 0 then poll() end
  if S.frames % 50 == 0 then every_second() end
end
