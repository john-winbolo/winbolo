-- Wave Defense
--
-- Defenders hold the four pillboxes at the centre of the map while waves of
-- raiders come in off the edge starts. A wave ends when its time runs out or
-- when the last raider in it is dead, whichever comes first. Hold every wave
-- and the defenders win; let one raider inside the keep and the round is over.
--
-- Nothing here runs every tick. The round is driven by two things: timers,
-- which carry one wave to the next, and hooks, which the server calls when
-- something happens. A scenario that polls the world each tick is doing work
-- the server has already done.
--
-- The raiders are a team of seats rather than a team of bots. The lobby block
-- below asks for six seats on team 2 with fielded = false, so a host opening
-- this map sees the seats in the roster and can trim them, and no brain loads
-- until a wave fields a seat. game.spawn_bot names one of those seats, which
-- puts a raider in it; game.remove_bot hands the seat back, and the next wave
-- uses it again.

local DEFENDERS = 1
local RAIDERS   = 2

-- How many raiders each wave fields, in order. The team's seats are the
-- ceiling: a wave asking for more than the lobby holds fields what is there.
local WAVES = { 2, 4, 6 }

local BRIEF_SECONDS = 2   -- before the first wave, so the defenders can place
local WAVE_SECONDS  = 3   -- how long a wave has to break through
local BREAK_SECONDS = 1   -- between waves, and long enough for the seats a
                          -- cleared wave gave back to come free

local HELD = "The keep held."

local wave       = 0      -- which wave is on the field, 0 before the first
local standing   = 0      -- raiders this wave has on the field
local seats      = {}     -- the team's seats, in seat order
local wave_timer = nil    -- the timer that ends the wave on time
local over       = false  -- the round has been decided

-- The seats the lobby is holding for the raiders. Read once, at the start: a
-- seat that a wave fields and hands back is the same seat, so the list keeps.
local function held_seats()
  local out = {}
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot and slot.bot and slot.team == RAIDERS then
      out[#out + 1] = p
    end
  end
  return out
end

-- Everyone who is not a raider is a defender. A scenario's lobby seats its
-- own team; a human who joined on no team at all is put on the defenders'
-- here, so the round has two sides whoever turned up.
local function enlist_the_defenders()
  local first = nil
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot and slot.fielded and slot.team ~= RAIDERS then
      if slot.team ~= DEFENDERS then
        game.set_team(p, DEFENDERS)
      end
      first = first or p
    end
  end
  return first
end

-- The keep's pillboxes are the ones the table tags "keep", so the squares
-- live in the scenario table and this code names none of them. They are
-- handed to a defender, which is what makes them shoot at raiders: a pillbox
-- answers to a seat, not to a team.
local function hand_over_the_keep(defender)
  if defender == nil then
    return
  end
  for _, n in ipairs(game.tagged("keep", "pill")) do
    game.set_pill_owner(n, defender)
  end
end

-- Declared together: the wave runs into the break and the break back into the
-- wave, so each of the three is named before any of them is written.
local next_wave, wave_over, hold_wins

next_wave = function()
  if over then
    return
  end
  wave = wave + 1
  if wave > #WAVES then
    hold_wins()
    return
  end

  local want = WAVES[wave]
  if want > #seats then
    want = #seats
  end
  standing = 0
  for i = 1, want do
    -- The seat carries the name and the team it was seated with, so a spawn
    -- that names one needs neither. The start is this raider's way in.
    if game.spawn_bot{ slot = seats[i], start = i } then
      standing = standing + 1
    end
  end

  game.message(string.format("Wave %d of %d: %d raiders.", wave, #WAVES,
                             standing))
  wave_timer = game.timer(WAVE_SECONDS, wave_over)
end

wave_over = function()
  if over then
    return
  end
  wave_timer = nil
  -- Whatever is still standing when the time runs out goes home. A seat a
  -- scenario put on the field goes back to being held rather than being
  -- emptied, so the next wave has it.
  for _, p in ipairs(seats) do
    local slot = game.lobby_slot(p)
    if slot and slot.fielded then
      game.remove_bot(p)
    end
  end
  standing = 0
  game.timer(BREAK_SECONDS, next_wave)
end

hold_wins = function()
  over = true
  game.message(HELD)
  game.end_round(HELD, DEFENDERS)
end

-- Before the round: the rules this map is played under, and the keep's
-- pillboxes brought up to full. The seats are not on the field yet, so
-- nothing here asks about a tank.
function on_setup()
  for _, n in ipairs(game.tagged("keep", "pill")) do
    game.set_pill_armour(n, game.rule("pill_max_armour"))
  end
end

-- The round's first tick. Everything that reads the roster waits for this:
-- the tanks exist, and the seats say who is on which side.
function on_start()
  seats = held_seats()
  hand_over_the_keep(enlist_the_defenders())
  game.message(string.format("Wave Defense: hold the keep through %d waves.",
                             #WAVES))
  game.timer(BRIEF_SECONDS, next_wave)
end

-- A raider that dies is out of the wave for good: the seat goes back to being
-- held, so there is no respawn to count twice. The wave ends the moment the
-- last of them is gone, without waiting out the rest of its time.
function on_tank_killed(victim, killer, cause, scripted)
  if over then
    return
  end
  local slot = game.lobby_slot(victim)
  if not slot or slot.team ~= RAIDERS then
    return
  end

  game.remove_bot(victim)
  standing = standing - 1
  if standing <= 0 and wave_timer ~= nil then
    game.cancel_timer(wave_timer)
    wave_timer = nil
    game.message(string.format("Wave %d cleared.", wave))
    game.timer(BREAK_SECONDS, next_wave)
  end
end

-- One raider inside the keep ends it. The region is the rectangle the table
-- names, and the server reports the crossing rather than the script watching
-- for it.
function on_enter_region(p, name)
  if over or name ~= "keep" then
    return
  end
  local slot = game.lobby_slot(p)
  if not slot or slot.team ~= RAIDERS then
    return
  end

  over = true
  local line = string.format("A raider reached the keep on wave %d.", wave)
  game.message(line)
  game.end_round(line, 0)
end

function on_end()
  game.log(string.format("Wave Defense ended on wave %d of %d", wave, #WAVES))
end

scenario = {
  name        = "Wave Defense",
  description = "Hold the four pillboxes at the centre of the map against " ..
                "three waves of raiders.",
  api         = 1,
  game        = "open",
  bound       = true,
  -- The raiders are held bot seats fielded with game.spawn_bot, which a
  -- lobby set to no bots refuses.
  needs_bots  = true,

  -- The lobby this map opens with. The team's seats are held rather than
  -- filled: they are in the roster from the start, where a host can see and
  -- trim them, and each of them takes a brain only when a wave fields it.
  lobby = {
    max_players = 4,
    teams = {
      { id = DEFENDERS, bots = 0, max_bots = 0 },
      { id = RAIDERS,   bots = 6, max_bots = 8, fielded = false },
    },
  },

  -- A short wait to come back, and a quick gun. Every other number the round
  -- runs on is the classic one.
  rules = {
    tank_death_ticks  = 150,
    tank_reload_ticks = 10,
  },

  -- The four pillboxes ringing the centre. The script asks for them by tag,
  -- so the map's own numbering lives here and nowhere else.
  tags = {
    pills = {
      [7]  = "keep",
      [8]  = "keep",
      [9]  = "keep",
      [10] = "keep",
    },
  },

  -- The ground the defenders are holding: the squares around those four.
  regions = {
    keep = { x = 124, y = 124, w = 10, h = 10 },
  },
}
