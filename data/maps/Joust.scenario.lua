-- Joust
--
-- A round deep-sea arena, 38 squares across (1176 squares of water), inside
-- a wall of buildings at least five squares thick. There is no land inside
-- it. Every tank starts and comes
-- back on a boat, and any hit on a tank on a boat sinks the boat, so the
-- tank drowns: one hit kills. The first side to reach the kill count the
-- host picked wins.
--
-- Who is a side. When the tanks in the round sit on two or more lobby teams,
-- each team is a side and its members' kills add up. Otherwise it is every
-- tank for itself, and a tank on no team is always a side of its own. The
-- choice is made once, when the round starts.
--
-- What counts as a kill. A shell or a mine counts for the tank that fired it
-- or laid it. A tank that drowns, or that dies to its own mine, counts for the
-- last enemy tank that hit it in the last CREDIT_SECONDS, and for nobody when
-- no enemy did. Killing a tank on your own side scores nothing.
--
-- Where a tank starts. Joust.map has sixteen starts, evenly spaced on a ring
-- four squares in from the water's edge, each facing the middle. The script
-- reads them from the map and names no squares. In a team round each team
-- takes one half of the ring, west or east. The first time a tank comes on, it takes
-- the start that matches its place (its place in its own team in a team
-- round), so no two tanks share one. After that, a tank comes back on the
-- start that is farthest from every enemy tank, and no shell can hit it for
-- its first SHIELD_SECONDS, so a shell already flying cannot sink it as it
-- arrives.
--
-- There are no bases: a base is land, and a tank that drives onto it leaves
-- its boat. A tank gets back one shell every REFILL_SECONDS instead.

local WEST, EAST = 1, 2

local DEFAULT_TARGET = 10
local CREDIT_SECONDS = 10     -- how long a hit keeps its claim on a drowning
local REFILL_SECONDS = 2      -- one shell back this often, up to full
local OPENING_TICKS  = 200    -- first two seconds: fixed starts, not farthest
local SHIELD_SECONDS = 1      -- a new tank cannot be hit this long
local PANEL_SECONDS  = 0.5
local PANEL_ROWS     = 8
local LABEL          = "KILLS"

local target    = DEFAULT_TARGET
local team_mode = false
local kills     = {}          -- side key -> kills
local last_hit  = {}          -- seat -> { by = seat, at = tick }
local dirty     = true
local over      = false
local started   = false       -- on_start has run and fixed team_mode
local start_at  = 0           -- game.tick() at on_start
local spawned   = {}          -- seat -> true once it has taken the field
local shield    = {}          -- seat -> game.tick() its spawn shield ends
local ends      = nil         -- [WEST], [EAST] and all: lists of { n, x, y }

local function in_round(p)
  local slot = game.lobby_slot(p)
  return slot ~= nil and slot.connected and slot.fielded
end

local function team_of(p)
  local slot = game.lobby_slot(p)
  return (slot ~= nil) and slot.team or 0
end

-- The side key a seat scores for: "t<team>" in a team round when the seat is
-- on a team, "p<seat>" otherwise.
local function side_of(p)
  local team = team_of(p)
  if team_mode and team > 0 then
    return "t" .. team
  end
  return "p" .. p
end

local function name_of(p)
  local t = game.tank(p)
  if t ~= nil and t.name ~= "" then
    return t.name
  end
  local slot = game.lobby_slot(p)
  if slot ~= nil and slot.name ~= "" then
    return slot.name
  end
  return "Somebody"
end

local function side_name(key)
  local kind, n = key:sub(1, 1), tonumber(key:sub(2))
  if kind == "t" then
    return "Team " .. n
  end
  return name_of(n)
end

-- Which half of the ring a seat plays from in a team round. The teams take
-- the two halves in turn, lowest team number first.
local function end_of(p)
  local teams, seen = {}, {}
  for q = 0, game.max_tanks() - 1 do
    local t = team_of(q)
    if t > 0 and not seen[t] and in_round(q) then
      seen[t] = true
      teams[#teams + 1] = t
    end
  end
  table.sort(teams)
  for i, t in ipairs(teams) do
    if t == team_of(p) then
      return (i % 2 == 1) and WEST or EAST
    end
  end
  return (p % 2 == 0) and WEST or EAST
end

-- How many lobby teams the seats now in the round sit on.
local function count_teams()
  local teams, count = {}, 0
  for p = 0, game.max_tanks() - 1 do
    local t = team_of(p)
    if in_round(p) and t > 0 and not teams[t] then
      teams[t] = true
      count = count + 1
    end
  end
  return count
end

-- The map's starts, split into a west and an east half at the middle of all
-- of them. "all" keeps the map's own order: west, east, west, east, and any
-- first few seats spread out round the ring.
local function read_ends()
  if ends ~= nil then
    return ends
  end
  local list, sum = {}, 0
  for n = 1, game.num_starts() do
    local s = game.start(n)
    if s ~= nil then
      list[#list + 1] = { n = n, x = s.x, y = s.y }
      sum = sum + s.x
    end
  end
  ends = { [WEST] = {}, [EAST] = {}, all = {} }
  local mid = (#list > 0) and (sum / #list) or 0
  for _, s in ipairs(list) do
    local side = (s.x < mid) and WEST or EAST
    ends[side][#ends[side] + 1] = s
    ends.all[#ends.all + 1] = s
  end
  return ends
end

-- The seat's place among the seats in the round below it: among its own
-- team in a team round, among every seat otherwise.
local function place_of(p)
  local place, team = 0, team_of(p)
  local by_team = team_mode and team > 0
  for q = 0, p - 1 do
    if in_round(q) and (not by_team or team_of(q) == team) then
      place = place + 1
    end
  end
  return place
end

-- How far a start is from the nearest live enemy tank, and from the nearest
-- live tank of any side, in squares. 999 when there is none.
local function start_gaps(s, p)
  local enemy, any = 999, 999
  local mine = side_of(p)
  for q = 0, game.max_tanks() - 1 do
    if q ~= p then
      local t = game.tank(q)
      if t ~= nil and not t.dead then
        local gap = math.max(math.abs(s.x - t.mx), math.abs(s.y - t.my))
        if gap < any then
          any = gap
        end
        if side_of(q) ~= mine and gap < enemy then
          enemy = gap
        end
      end
    end
  end
  return enemy, any
end

local function post_score(key)
  local kind, n = key:sub(1, 1), tonumber(key:sub(2))
  if kind == "t" then
    game.score({ team = n }, kills[key] or 0, LABEL)
  elseif game.lobby_slot(n) ~= nil then
    game.score(n, kills[key] or 0, LABEL)
  end
end

-- Every side in the round, most kills first.
local function standings()
  local seen, out = {}, {}
  for p = 0, game.max_tanks() - 1 do
    if in_round(p) then
      local key = side_of(p)
      if not seen[key] then
        seen[key] = true
        out[#out + 1] = key
      end
    end
  end
  table.sort(out, function(a, b)
    local ka, kb = kills[a] or 0, kills[b] or 0
    if ka ~= kb then return ka > kb end
    return a < b
  end)
  return out
end

local function draw_panel()
  local list = {
    { "rect", 0, 0, 128, 14, "grey_dark", true },
    { "text", 64, 3, "white", "normal", "centre", "JOUST" },
    { "text", 64, 18, "yellow", "small", "centre",
      string.format("First to %d kills", target) },
  }
  for row, key in ipairs(standings()) do
    if row > PANEL_ROWS then
      break
    end
    local y = 30 + (row - 1) * 10
    local colour = (row == 1) and "cyan" or "white"
    local kind, n = key:sub(1, 1), tonumber(key:sub(2))
    if kind == "t" then
      list[#list + 1] = { "text", 4, y, colour, "small", "left", "Team " .. n }
    else
      list[#list + 1] = { "name", 4, y, colour, "small", "left", n }
    end
    list[#list + 1] = { "text", 124, y, colour, "small", "right",
                        string.format("%d", kills[key] or 0) }
  end
  game.panel(0, list)
end

local function panel_loop()
  if over then
    return
  end
  if dirty then
    dirty = false
    draw_panel()
  end
  game.timer(PANEL_SECONDS, panel_loop)
end

-- One shell back every REFILL_SECONDS, up to full, for every live tank.
-- There are no bases, and a tank with no shells could never score again.
local function refill_loop()
  if over then
    return
  end
  local full = game.rule("tank_full_shells")
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil and not t.dead and t.shells < full then
      game.set_stocks(p, { shells = t.shells + 1 })
    end
  end
  game.timer(REFILL_SECONDS, refill_loop)
end

local function finish(key)
  if over then
    return
  end
  over = true
  local line = string.format("%s wins the joust with %d kills.",
                             side_name(key), kills[key])
  game.message(line)
  game.announce(line, 5)
  draw_panel()
  local kind, n = key:sub(1, 1), tonumber(key:sub(2))
  if kind == "t" then
    game.end_round(line, n)
  else
    game.end_round(line)
  end
end

-- Who a death is credited to, or nil for nobody.
local function credit_for(victim, killer, cause)
  if killer ~= nil and killer ~= victim and killer ~= game.NEUTRAL and
     cause ~= "deep_sea" then
    return killer
  end
  -- A drowning, or a tank that killed itself: the last enemy that hit it.
  local hit = last_hit[victim]
  if hit ~= nil and (game.tick() - hit.at) <= CREDIT_SECONDS * 100 then
    return hit.by
  end
  return nil
end

function on_tank_hit(victim, attacker, cause, amount, pill, scripted)
  if attacker == nil or attacker == game.NEUTRAL or attacker == victim then
    return
  end
  -- A teammate's hit is not kept: it would take the drowning from the enemy
  -- who hit the tank first, and then score for nobody.
  if side_of(attacker) == side_of(victim) then
    return
  end
  last_hit[victim] = { by = attacker, at = game.tick() }
end

function on_tank_killed(victim, killer, cause, scripted)
  if over then
    return
  end
  local by = credit_for(victim, killer, cause)
  last_hit[victim] = nil
  if by == nil or game.lobby_slot(by) == nil then
    return
  end
  local key = side_of(by)
  if key == side_of(victim) then
    return                    -- own side: no point
  end
  kills[key] = (kills[key] or 0) + 1
  post_score(key)
  dirty = true
  local how = (cause == "deep_sea") and "drowned" or "killed"
  game.announce(string.format("%s %s %s  (%s %d/%d)", name_of(by), how,
                              name_of(victim), side_name(key), kills[key],
                              target), 3)
  if kills[key] >= target then
    finish(key)
  end
end

-- Which start a tank comes on. The engine then puts it on the nearest deep
-- sea square that is clear of other tanks, and a tank on deep sea is on a
-- boat.
function on_choose_start(p)
  -- The shield starts here, as the tank is placed. on_tank_spawned comes
  -- after the tick's shells have flown, too late for one that lands at once.
  shield[p] = game.tick() + SHIELD_SECONDS * 100
  local e = read_ends()
  if not started then
    team_mode = count_teams() >= 2
  end
  local list = e.all
  if team_mode and team_of(p) > 0 then
    list = e[end_of(p)]
  end
  if #list == 0 then
    return nil
  end
  -- The opening: one start for each place, so no two tanks share a square.
  -- A free-for-all takes the map's order, which spreads the seats round the
  -- ring. The opening tanks are placed one at a time, and the lobby may not
  -- show every seat and team yet, so a team round can fall back to that
  -- order too. It alternates west and east, so seats on round-robin teams
  -- still start on their own half.
  if not spawned[p] and
     (not started or game.tick() - start_at < OPENING_TICKS) then
    return list[(place_of(p) % #list) + 1].n
  end
  -- Later: the start farthest from every enemy tank, then from every tank.
  local best, best_enemy, best_any = list[1], -1, -1
  for _, s in ipairs(list) do
    local enemy, any = start_gaps(s, p)
    if enemy > best_enemy or (enemy == best_enemy and any > best_any) then
      best, best_enemy, best_any = s, enemy, any
    end
  end
  return best.n
end

-- A tank arrives on a start out in the deep sea, on a boat.
function on_tank_spawned(p, mx, my, respawn, scripted)
  spawned[p] = true
  last_hit[p] = nil
  -- A start a script op named skips on_choose_start, so start the shield
  -- here when that did not.
  if shield[p] == nil or shield[p] <= game.tick() then
    shield[p] = game.tick() + SHIELD_SECONDS * 100
  end
  if over then
    return
  end
  -- The engine gives every new tank a boat. This only guards the rule the
  -- whole game stands on.
  local t = game.tank(p)
  if t ~= nil and not t.boat then
    game.set_boat(p, true)
  end
end

-- A shell passes through a tank in its first SHIELD_SECONDS.
function can_hit(attacker, kind, n, pill)
  if kind == "tank" and shield[n] ~= nil and game.tick() < shield[n] then
    return false
  end
  return nil
end

function on_player_join(p, scripted)
  if over then
    return
  end
  post_score(side_of(p))
  dirty = true
end

function on_player_leave(p, scripted)
  last_hit[p] = nil
  spawned[p] = nil
  shield[p] = nil
  if not team_mode or team_of(p) == 0 then
    kills["p" .. p] = nil     -- the seat's next player starts at nought
  end
  dirty = true
end

-- The arena stays as drawn: no bridges, no boats, no walls.
function can_build(p, action, x, y, n)
  return false
end

-- The map has no bases. This keeps a round with no bases from counting as
-- one side holding them all.
function allow_base_win()
  return false
end

function on_start()
  local v = game.setting("kills_to_win")
  if type(v) == "number" and v >= 1 then
    target = math.floor(v)
  end

  team_mode = count_teams() >= 2
  started = true
  start_at = game.tick()

  for _, key in ipairs(standings()) do
    post_score(key)
  end
  game.message(string.format("Joust: first %s to %d kills wins. %s",
                             team_mode and "team" or "tank", target,
                             "One hit sinks a boat."))
  game.log(string.format("Joust: target %d, %s", target,
                         team_mode and "teams" or "free for all"))
  dirty = true
  panel_loop()
  game.timer(REFILL_SECONDS, refill_loop)
end

function on_end()
  over = true
  -- A log line holds 128 bytes, and sixteen names do not fit in one, so the
  -- standings go out a few sides to a line.
  local line = "Joust ended:"
  for _, key in ipairs(standings()) do
    local part = string.format(" %s %d", side_name(key), kills[key] or 0)
    if #line + #part > 100 then
      game.log(line)
      line = "Joust ended (more):"
    end
    line = line .. part
  end
  game.log(line)
end

scenario = {
  name        = "Joust",
  description = "A round deep-sea arena inside thick walls. Every tank is on a " ..
                "boat, so one hit drowns it. First to the kill count wins.",
  api         = 1,
  game        = "open",

  -- The script splits the map's own starts into its two halves.
  bound       = true,

  settings = {
    { id = "kills_to_win", label = "Kills to win", type = "int",
      min = 1, max = 50, step = 1, default = DEFAULT_TARGET },
  },

  rules = {
    -- Trees never grow, so no land ever shows up in the arena. The rule has
    -- no off, so this is the longest wait it will take.
    tree_grow_ticks         = 2000000000,
    tree_grow_initial_ticks = 2000000000,

    -- A wall square takes 256 shells to turn to rubble, not 5, so the wall
    -- holds for the whole round and nobody shoots a way out.
    building_life = 255,

    -- A dying tank never makes the big blast that flattens the squares
    -- round it, so a tank that dies at the wall cannot open it.
    big_explosion_threshold = 510,

    -- Back on a boat after about three seconds.
    tank_death_ticks = 150,
  },

  callbacks = {
    on_start = "Reads the kill target, decides team or free-for-all, posts the scores and starts the shell refill.",
    on_choose_start = "Picks each tank's start: one for each place at the opening, then the start farthest from every enemy, on its own half of the ring in a team round.",
    on_tank_hit = "Remembers the last enemy tank that hit each tank.",
    on_tank_killed = "Gives the kill to the killer, or for a drowning to the last enemy hit, and ends the round at the target.",
    on_tank_spawned = "Makes sure a new tank is on its boat and starts its one-second shield.",
    can_hit = "Lets shells pass through a tank in its first second, so nobody is sunk as they arrive.",
    on_player_join = "Shows the new player's score.",
    on_player_leave = "Clears a leaving free-for-all player's kills.",
    can_build = "Nothing can be built, so the arena stays as drawn.",
    allow_base_win = "Only kills win; the map has no bases.",
    on_end = "Writes the final standings to the server log.",
  },
}
