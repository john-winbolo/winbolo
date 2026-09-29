-- Joust
--
-- A road lane over deep sea, a grass ring in the middle of it and a grass pad
-- at each end. Every tank comes back on its own pad, off its boat, facing the
-- other end. The first side to reach the kill count the host picked wins.
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
-- The map is Joust.map and the squares below are that map's. Change one and
-- change both.

local WEST, EAST = 1, 2
local FACING = { [WEST] = 64, [EAST] = 192 }   -- full 0-255 heading

-- Where a tank comes back, in map squares, on its own end's pad.
local SPOTS = {
  [WEST] = { { 112, 126 }, { 112, 124 }, { 112, 128 }, { 113, 125 },
             { 113, 127 }, { 111, 124 }, { 111, 128 }, { 114, 126 } },
  [EAST] = { { 140, 126 }, { 140, 128 }, { 140, 124 }, { 139, 127 },
             { 139, 125 }, { 141, 128 }, { 141, 124 }, { 138, 126 } },
}

local DEFAULT_TARGET = 10
local CREDIT_SECONDS = 10     -- how long a hit keeps its claim on a drowning
local PANEL_SECONDS  = 0.5
local PANEL_ROWS     = 8
local LABEL          = "KILLS"

local target    = DEFAULT_TARGET
local team_mode = false
local kills     = {}          -- side key -> kills
local last_hit  = {}          -- seat -> { by = seat, at = tick }
local dirty     = true
local over      = false

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

-- Which end a seat plays from. In a team round the teams take the two ends in
-- turn, lowest team number first; otherwise the seats do, by seat number.
local function end_of(p)
  if team_mode and team_of(p) > 0 then
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
  end
  return (p % 2 == 0) and WEST or EAST
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
  -- who knocked the tank in, and then score for nobody.
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

-- A tank arrives on a start out in the water, on a boat. Take it off the boat
-- and put it on its own pad, facing the other end.
function on_tank_spawned(p, mx, my, respawn, scripted)
  if over then
    return
  end
  local side = end_of(p)
  local spots = SPOTS[side]
  -- Free-for-all splits seats by parity, so every second seat shares an end.
  -- A team round puts the whole team on one end, so count the seat's place
  -- among its own team instead, or teammates would land on the same square.
  local slot = math.floor(p / 2)
  if team_mode and team_of(p) > 0 then
    slot = 0
    for q = 0, p - 1 do
      if team_of(q) == team_of(p) and in_round(q) then
        slot = slot + 1
      end
    end
  end
  local at = spots[(slot % #spots) + 1]
  game.set_boat(p, false)
  game.teleport(p, at[1], at[2], FACING[side])
  last_hit[p] = nil
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
  if not team_mode or team_of(p) == 0 then
    kills["p" .. p] = nil     -- the seat's next player starts at nought
  end
  dirty = true
end

-- The lane stays as drawn: no bridges, no boats, no walls.
function can_build(p, action, x, y, n)
  return false
end

-- Two bases, one each end. Owning both does not end a joust.
function allow_base_win()
  return false
end

function on_start()
  local v = game.setting("kills_to_win")
  if type(v) == "number" and v >= 1 then
    target = math.floor(v)
  end

  local teams, count = {}, 0
  for p = 0, game.max_tanks() - 1 do
    local t = team_of(p)
    if in_round(p) and t > 0 and not teams[t] then
      teams[t] = true
      count = count + 1
    end
  end
  team_mode = count >= 2

  for _, key in ipairs(standings()) do
    post_score(key)
  end
  game.message(string.format("Joust: first %s to %d kills wins. %s",
                             team_mode and "team" or "tank", target,
                             "Knock them into the sea."))
  game.log(string.format("Joust: target %d, %s", target,
                         team_mode and "teams" or "free for all"))
  dirty = true
  panel_loop()
end

function on_end()
  over = true
  local parts = {}
  for _, key in ipairs(standings()) do
    parts[#parts + 1] = string.format("%s %d", side_name(key), kills[key] or 0)
  end
  game.log("Joust ended: " .. table.concat(parts, ", "))
end

scenario = {
  name        = "Joust",
  description = "A road lane over deep sea. First to the kill count wins; " ..
                "a tank knocked into the sea counts for whoever hit it last.",
  api         = 1,
  game        = "open",

  -- The pads and spawn squares above are this map's.
  bound       = true,

  settings = {
    { id = "kills_to_win", label = "Kills to win", type = "int",
      min = 1, max = 50, step = 1, default = DEFAULT_TARGET },
  },

  rules = {
    -- Trees never grow, so the lane and the ring stay open. The rule has no
    -- off, so this is the longest wait it will take.
    tree_grow_ticks         = 2000000000,
    tree_grow_initial_ticks = 2000000000,

    -- Back on the pad after about three seconds.
    tank_death_ticks = 150,
  },

  callbacks = {
    on_start = "Reads the kill target, decides team or free-for-all and posts the scores.",
    on_tank_hit = "Remembers the last enemy tank that hit each tank.",
    on_tank_killed = "Gives the kill to the killer, or for a drowning to the last enemy hit, and ends the round at the target.",
    on_tank_spawned = "Takes a new tank off its boat and puts it on its own end's pad.",
    on_player_join = "Shows the new player's score.",
    on_player_leave = "Clears a leaving free-for-all player's kills.",
    can_build = "Nothing can be built, so the lane stays as drawn.",
    allow_base_win = "Holding both bases does not win; only kills count.",
    on_end = "Writes the final standings to the server log.",
  },
}
