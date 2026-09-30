-- Soccer
--
-- Two teams, one ball, no guns. The ball is the map's single dead pillbox,
-- and the whole mod turns on one policy: can_capture answers false, so a tank
-- driving over the pillbox does not pick it up — it drives through it. That
-- same policy is the collision test, because the engine only asks it when a
-- tank's pickup reach is actually touching the ball, and it says which tank.
--
-- A policy may only answer, so the touch is written down and the kick is
-- given on the next tick.
--
-- The ball's real position is kept here in world units, 256 to the map
-- square, and integrated every frame with friction and wall bounces. A
-- pillbox only has a square to stand on, so move_pill is called when the
-- square the ball is in changes and not before. The physics is smooth; the
-- drawing hops. That is a property of a pillbox having no sub-square
-- position, not of this script.
--
-- The pitch is Soccer.map and the numbers below are that map's. Change one
-- and change both.

local RED, BLUE = 1, 2

-- The map, in map squares. The two goals are not here: they are regions at
-- the foot of this file, so the squares they cover are named in the scenario
-- table rather than written out in the script.
local PITCH = { x0 = 106, y0 = 113, x1 = 149, y1 = 142 }
local SPOT  = { x = 127, y = 127 }

local SQUARE = 256        -- world units to a map square
local EAST, WEST = 64, 192

-- Where each side lines up, and which way it faces. RED attacks right.
local KICKOFF = {
  [RED]  = { { 112, 120 }, { 112, 127 }, { 112, 135 }, { 118, 117 },
             { 118, 138 } },
  [BLUE] = { { 143, 120 }, { 143, 127 }, { 143, 135 }, { 137, 117 },
             { 137, 138 } },
}
local FACING = { [RED] = EAST, [BLUE] = WEST }

local MATCH_SECONDS = 300
local KICK          = 1.8    -- how much of the tank's speed the ball takes
local NUDGE         = 6      -- so a stationary tank still shifts it
local MAX_TANK_STEP = 40     -- a bigger step than any tank can really make
local FRICTION      = 0.99   -- per tick
local BOUNCE        = 0.8    -- kept off a wall
local STOPPED       = 2      -- below this the ball is standing still
local TOUCH_GAP     = 4      -- ticks before the same tank may kick again
local PANEL_SECONDS = 0.1

local ball                   -- the pillbox that is the ball
local bx, by = 0, 0          -- its real position, in world units
local vx, vy = 0, 0          -- and its velocity, world units a tick
local goals  = { [RED] = 0, [BLUE] = 0 }
local touched = {}           -- seats that reached the ball this tick
local last_kick = {}         -- the tick each seat last kicked
local prev = {}              -- each tank's position last tick
local ends_at, over = 0, false

local function whole(n)
  return math.floor(n + 0.5)
end

local function sq(world)
  return math.floor(world / SQUARE)
end

-- Somewhere the ball may be: the pitch is grass and the two goals are road,
-- and everything else on this map is wall or sea. Read off the map rather
-- than off the rectangles this file declares, so a square the map disagrees
-- about is the map's answer.
local function open_square(x, y)
  local t = game.map_tile(x, y)
  return t == game.TERRAIN.grass or t == game.TERRAIN.road
end

local function put_ball_on_the_spot()
  bx = SPOT.x * SQUARE + SQUARE / 2
  by = SPOT.y * SQUARE + SQUARE / 2
  vx, vy = 0, 0
  if ball ~= nil then
    game.move_pill(ball, SPOT.x, SPOT.y)
  end
end

local function line_up()
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    local t = game.tank(p)
    if slot ~= nil and t ~= nil and KICKOFF[slot.team] ~= nil then
      local spots = KICKOFF[slot.team]
      local at = spots[(p % #spots) + 1]
      game.teleport(p, at[1], at[2], FACING[slot.team])
      prev[p] = nil
    end
  end
end

-- Everyone who turned up on no team is put on the thinner side, so a round
-- with four humans and nobody reading the lobby is still two a side.
local function pick_sides()
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot ~= nil and slot.connected and slot.team ~= RED and
       slot.team ~= BLUE then
      local red = game.team_size(RED)
      local blue = game.team_size(BLUE)
      game.set_team(p, (red <= blue) and RED or BLUE)
    end
  end
end

local function scoreboard()
  local list = {
    { "timer", 64, 2, "yellow", "normal", "centre", "down", ends_at },
    { "text", 30, 20, "red", "small", "centre", "RED" },
    { "text", 98, 20, "blue", "small", "centre", "BLUE" },
    { "text", 30, 32, "red", "normal", "centre",
      string.format("%d", goals[RED]) },
    { "text", 64, 32, "grey", "normal", "centre", "-" },
    { "text", 98, 32, "blue", "normal", "centre",
      string.format("%d", goals[BLUE]) },
    -- The pitch from above, with the ball on it: the view only reaches a
    -- third of the pitch, so this is how a player knows where the ball is.
    { "rect", 12, 58, 104, 62, "grey_dark", true },
    { "rect", 12, 58, 104, 62, "grey", false },
    { "rect", 8, 78, 4, 22, "red", true },
    { "rect", 116, 78, 4, 22, "blue", true },
  }
  local px = 12 + ((bx / SQUARE) - PITCH.x0) / (PITCH.x1 - PITCH.x0 + 1) * 104
  local py = 58 + ((by / SQUARE) - PITCH.y0) / (PITCH.y1 - PITCH.y0 + 1) * 62
  if px < 8 then px = 8 elseif px > 120 then px = 120 end
  if py < 58 then py = 58 elseif py > 118 then py = 118 end
  list[#list + 1] = { "rect", whole(px) - 1, whole(py) - 1, 3, 3, "white",
                      true }
  game.panel(0, list)
end

local function panel_loop()
  if over then
    return
  end
  scoreboard()
  game.timer(PANEL_SECONDS, panel_loop)
end

local finish

local function goal_to(team)
  goals[team] = goals[team] + 1
  game.score({ team = team }, goals[team], "GOALS")
  local who = (team == RED) and "Red" or "Blue"
  game.message(string.format("%s scores. %d - %d", who, goals[RED],
                             goals[BLUE]))
  game.announce(string.format("%s!  %d - %d", who, goals[RED], goals[BLUE]),
                3)
  game.sound("big_explosion_near", SPOT.x, SPOT.y)
  put_ball_on_the_spot()
  line_up()
  scoreboard()
end

-- One tank's touch, given on the tick after the policy saw it. The ball
-- leaves along the line from the tank to the ball — the contact normal,
-- near enough — carrying the speed the tank was actually travelling at,
-- which is read off how far it moved rather than off a field, because a
-- tank's speed is not on the read surface.
local function kick(p, tick)
  if last_kick[p] ~= nil and (tick - last_kick[p]) < (TOUCH_GAP * 2) then
    return
  end
  local t = game.tank(p)
  if t == nil or t.dead then
    return
  end

  local step = 0
  local was = prev[p]
  if was ~= nil then
    local dx, dy = t.wx - was[1], t.wy - was[2]
    step = math.sqrt(dx * dx + dy * dy)
    -- A teleport is not a run-up. Anything past what a tank can cover in a
    -- tick is one, and is not allowed to launch the ball.
    if step > MAX_TANK_STEP then
      step = 0
    end
  end

  local nx, ny = bx - t.wx, by - t.wy
  local len = math.sqrt(nx * nx + ny * ny)
  if len < 1 then
    nx, ny, len = 1, 0, 1        -- dead centre: shove it off to one side
  end
  nx, ny = nx / len, ny / len

  local power = step * KICK + NUDGE
  vx, vy = nx * power, ny * power
  last_kick[p] = tick
  game.sound("hit_tank_near", sq(bx), sq(by))
end

function on_tick(tick)
  if over or ball == nil then
    return
  end

  for p in pairs(touched) do
    kick(p, tick)
    touched[p] = nil
  end

  -- Remembered after the kick, so a kick reads the step into the ball
  -- rather than the step away from it.
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil then
      prev[p] = { t.wx, t.wy }
    else
      prev[p] = nil
    end
  end

  if vx == 0 and vy == 0 then
    return
  end

  local wasx, wasy = sq(bx), sq(by)
  local nx, ny = bx + vx, by + vy

  -- Each axis on its own, so a corner turns the ball round rather than
  -- stopping it, and then the diagonal for the corner both axes pass.
  if not open_square(sq(nx), wasy) then
    vx = -vx * BOUNCE
    nx = bx
  end
  if not open_square(sq(bx), sq(ny)) then
    vy = -vy * BOUNCE
    ny = by
  end
  if not open_square(sq(nx), sq(ny)) then
    vx, vy = -vx * BOUNCE, -vy * BOUNCE
    nx, ny = bx, by
  end
  bx, by = nx, ny

  vx, vy = vx * FRICTION, vy * FRICTION
  if math.sqrt(vx * vx + vy * vy) < STOPPED then
    vx, vy = 0, 0
  end

  local nowx, nowy = sq(bx), sq(by)
  if nowx ~= wasx or nowy ~= wasy then
    if game.move_pill(ball, nowx, nowy) == nil then
      -- The square would not take it. Stay put and stop rather than let the
      -- drawn ball and the real one drift apart.
      bx, by = wasx * SQUARE + SQUARE / 2, wasy * SQUARE + SQUARE / 2
      vx, vy = 0, 0
      return
    end
    -- The square the ball has just reached, not the square it comes to rest
    -- on: a ball crossing the line at speed has scored.
    if game.in_region("goal_right", nowx, nowy) then
      goal_to(RED)
    elseif game.in_region("goal_left", nowx, nowy) then
      goal_to(BLUE)
    end
  end
end

-- The ball is never picked up, and the question is also the only collision
-- test this script gets: the engine asks it exactly when a tank is touching
-- the pillbox, and only on the server.
function can_capture(kind, n, p)
  if kind == "pill" then
    touched[p] = true
    return false
  end
  return nil
end

-- No building, so no builder. The man never leaves the tank.
function can_build(p, action, x, y, n)
  return false
end

function allow_base_win()
  return false
end

-- Two teams, red and blue, which the script picks. An alliance across them
-- would leave a player on one team and allied with the other.
function can_ally(p, q)
  return false
end

-- Full armour, and nothing to shoot with. The caps are zero as well, so
-- there is nowhere for a shell to come from later either.
function spawn_loadout(p)
  return { shells = 0, mines = 0, armour = game.rule("tank_full_armour"),
           trees = 0 }
end

function on_setup()
  for n = 1, game.num_pills() do
    if game.pill(n) ~= nil then
      if ball == nil then
        ball = n
        game.set_pill_armour(n, 0)
      else
        game.remove_pill(n)
      end
    end
  end
end

function on_start()
  pick_sides()
  ends_at = game.tick() + MATCH_SECONDS * 100
  -- A few seconds longer than the whistle, so the script ends the match and
  -- the engine's own limit never gets there first with a line of its own.
  game.set_game_time((MATCH_SECONDS + 5) * 100)

  game.score({ team = RED }, 0, "GOALS")
  game.score({ team = BLUE }, 0, "GOALS")

  if ball == nil then
    game.log("Soccer: no pillbox on this map, so there is no ball")
    game.message("Soccer needs the Soccer map.")
  else
    put_ball_on_the_spot()
    line_up()
    game.message("Soccer: drive into the ball. Five minutes.")
  end

  game.timer(MATCH_SECONDS, finish)
  panel_loop()
end

-- A tank arrives on a start out in the water, on a boat. Step off it while
-- the water is still under it, then go to the line-up.
function on_tank_spawned(p, mx, my, respawn, scripted)
  if over then
    return
  end
  local slot = game.lobby_slot(p)
  local team = (slot ~= nil) and slot.team or 0
  if KICKOFF[team] == nil then
    pick_sides()
    slot = game.lobby_slot(p)
    team = (slot ~= nil) and slot.team or RED
  end
  game.set_boat(p, false)
  local spots = KICKOFF[team]
  local at = spots[(p % #spots) + 1]
  game.teleport(p, at[1], at[2], FACING[team])
  prev[p] = nil
end

function on_player_join(p, scripted)
  if over then
    return
  end
  pick_sides()
  game.score({ team = RED }, goals[RED], "GOALS")
  game.score({ team = BLUE }, goals[BLUE], "GOALS")
end

finish = function()
  if over then
    return
  end
  over = true
  local line
  if goals[RED] == goals[BLUE] then
    line = string.format("Full time: %d - %d, a draw.", goals[RED],
                         goals[BLUE])
    game.message(line)
    game.end_round(line)
  else
    local winner = (goals[RED] > goals[BLUE]) and RED or BLUE
    local who = (winner == RED) and "Red" or "Blue"
    line = string.format("Full time: %s wins %d - %d.", who,
                         math.max(goals[RED], goals[BLUE]),
                         math.min(goals[RED], goals[BLUE]))
    game.message(line)
    game.end_round(line, winner)
  end
end

function on_end()
  over = true
  game.log(string.format("Soccer ended %d - %d", goals[RED], goals[BLUE]))
end

scenario = {
  name        = "Soccer",
  description = "Two teams, one ball, no guns. Drive into the pillbox to " ..
                "move it and put it in the other side's goal.",
  api         = 1,
  game        = "open",

  -- The squares below are this map's, so the script does not travel.
  bound       = true,

  lobby = {
    max_players = 8,
    teams = {
      { id = RED,  bots = 0, max_bots = 0 },
      { id = BLUE, bots = 0, max_bots = 0 },
    },
  },

  rules = {
    -- No shells, ever: the cap is zero, so a tank cannot hold one however it
    -- came by it. The map carries no bases, so there is nothing to refuel
    -- from either.
    --
    -- Mines are kept to zero by the loadout and by can_build rather than by
    -- their cap. tank_full_mines = 0 would need lgm_cost_mine = 0 beside it,
    -- and a rules table is applied one rule at a time against the table as
    -- it stands — so whichever of that pair lands first is refused for
    -- breaking the invariant with the one that has not landed yet.
    tank_full_shells = 0,

    -- A quicker game than Bolo. Road is the goal floor, so it matches the
    -- grass and the ball does not change pace over the line.
    speed_grass = 18,
    speed_road  = 18,

    -- Trees never grow, so the pitch stays a pitch. The rule has no off, so
    -- this is the longest wait it will take.
    tree_grow_ticks         = 2000000000,
    tree_grow_initial_ticks = 2000000000,

    -- Back on the pitch quickly; there is nothing to punish here.
    tank_death_ticks = 100,
  },

  -- The mouths of the two goals, in map squares. A region is a corner and a
  -- size, so these are the four-by-eight boxes behind each line. Nothing
  -- enters them but the ball: the hooks that watch a region watch tanks, so
  -- on_tick asks game.in_region about the ball's square instead.
  regions = {
    goal_left  = { x = 101, y = 124, w = 4, h = 8 },   -- RED defends
    goal_right = { x = 151, y = 124, w = 4, h = 8 },   -- BLUE defends
  },

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this scenario implements:".
  callbacks = {
    on_tick = "Moves the ball, bounces it off walls and scores a goal when it crosses a goal line.",
    can_capture = "Tanks cannot pick up the ball; touching it kicks it instead.",
    can_build = "Nothing can be built, so builders stay in the tank.",
    allow_base_win = "Holding bases does not win; only goals count.",
    can_ally = "Players cannot ally.",
    spawn_loadout = "Tanks spawn with full armour and no shells, mines or trees.",
    on_setup = "Keeps one pillbox as the ball and removes the rest.",
    on_start = "Picks sides, puts the ball on the spot, lines teams up and starts the 5-minute clock.",
    on_tank_spawned = "Takes a new tank off its boat and puts it at its team's kick-off spot.",
    on_player_join = "Puts a new player on the smaller team and keeps the score showing.",
    on_end = "Stops the ball and the scoreboard.",
  },
}
