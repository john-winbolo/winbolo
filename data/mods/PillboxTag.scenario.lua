-- Pillbox Tag
--
-- One pillbox, dead, loose on the map. Whoever is carrying it scores a point a
-- second, and when the clock runs out (ten minutes unless the host sets
-- another length in the lobby) the one with the most points has won. Carrying
-- it costs you speed and every shell you own, so the holder runs and
-- everybody else hunts. Kill the holder, or drive over the pillbox where it
-- falls, and it is yours.
--
-- The holder's slow legs go by the ground under them. The faster the ground,
-- the more of its speed they lose. With the lobby's speed penalty at its
-- default of 30, on a road they keep 60 in a hundred, on grass 70, in a wood
-- 85, and on the slow ground (swamp, crater, rubble, a river) 90, because it
-- is slow for everybody already. Another penalty moves every one of those
-- losses by the same factor (see CARRY_PENALTY).
--
-- Every time the prize changes hands, the new holder gets a head start: twice
-- their carry speed for two seconds, fading back to their carry speed over
-- the next three, and no damage for the first two. All of those numbers are
-- the host's to change in the lobby (see "Tweak these" below).
--
-- The map's own pillboxes are taken off at setup and one is kept back as the
-- prize. The ones taken off are gone from the pillbox panel too. The holder
-- may build it. It then stands where he put it as his own
-- gun, and he is still the holder, but nobody scores while it stands: the
-- points come only from carrying it. He keeps the slow legs, but his gun is
-- filled again a shell a second like everybody else's, and a respawn arms
-- him fully. It never holds more than three armour, so three shells put it
-- down, and it is drawn as a pillbox with three armour left of fifteen.
-- The others shoot it down to nothing and drive over it, which makes the one
-- who picks it up the new holder.
--
-- If the holder dies while the prize is out of his tank, it is a dead
-- pillbox, free for anyone to take, and nobody holds it. Built, it goes to
-- nothing where it stands; shot down by him and not yet picked up, it is dead
-- already; in his man's hands, the man goes on and puts it down dead. In his
-- tank it drops with the tank, dead, as it always has.
--
-- A tank only picks up a dead pillbox, so the holder cannot take
-- his own back up while it stands; it has to be shot down first (his own
-- shells do that too), and then it is a race to drive over it. Picking it up
-- empties the gun again in the same frame. Nobody but the holder may repair it,
-- and a dead one lying on the ground is repaired by nobody.
--
-- Who plays with whom. The host picks it in the lobby with the "Teams"
-- setting. In "Free For All", the default, every tank is on a team of its
-- own from the round start, whatever the lobby teams were, so nobody is
-- allied with anybody and nobody can ally: the holder is against everybody,
-- and a built prize fires at everybody but him. The lobby teams are put
-- back when the round ends. In "Use Lobby Teams" the lobby teams play as
-- teams: teammates are allied, every second the holder carries the prize
-- scores for his team as well as for him, a built prize spares his
-- teammates, and the team with the most seconds wins. The bots on the
-- holder's team are not sent after him.
--
-- Voice chat. In a round the server sends a player's voice only to his
-- allies, so in a Free For All nobody would hear anybody. The host picks who
-- hears whom with the "Voice chat to everyone" setting. "Only in Free For
-- All", the default, sends every voice to everybody in a Free For All and
-- leaves a team round with voice to teammates. "Yes" sends every voice to
-- everybody in both, and "No" keeps voice to allies in both. A server that
-- is older than the setting keeps voice to allies, and a server with voice
-- off has no voice at all.
--
-- The scores under the compass: in Free For All one row a tank; in a team
-- round a row for each team with its total and, under it, a row for each
-- member with the seconds they carried. When that is more rows than there
-- is room for, every team's row stays and the player's own row goes under
-- their team. A team of bots only is named after the pool its bots take
-- their names from, as the lobby names them. The holder's row (in a team
-- round, his team's row and his own) is drawn in yellow, the panel's gold,
-- and is never cut off: when it would fall below the last row there is room
-- for, it takes that last row's place.
--
-- The compass is the panel. A script cannot draw on the edge of the game view,
-- so the square over the view is a compass rose instead: the needle points at
-- whatever is holding the prize — the tank, the man walking it out, or the
-- ground it is lying on — and every player is sent their own, because the
-- bearing is worked out from where that player is standing.
--
-- A bot cannot read a compass, so it is told instead: every bot but the holder
-- is sent after the prize, and every bot is tuned for the part it has. The
-- holder runs and never puts the prize down; the others hunt. A built prize is
-- the only live pillbox on the map, so it is the brain's own pillbox fight
-- that goes after it.
--
-- The bases are pit stops. They start neutral, driving over one hands out half
-- a tank of armour and 3 mines (a holder carrying the prize gets the mines
-- but still no shells), and the base then goes off the map for half a minute
-- before it comes back.

-- Tweak these. Each is the default of a setting the host can change in the
-- lobby's details dialog (scenario.settings at the bottom of the file
-- declares them from these numbers), and read_settings, in on_setup, puts
-- the host's choice over it. The lobby only offers whole numbers, so the
-- boost is set there as a percentage (200 is twice) and the round in minutes.
local ROUND_SECONDS       = 600  -- the whole round
local SPEED_PENALTY_PCT   = 30   -- how much slower the holder is: 30 is 30%
                                 -- slower on grass, more on a road and less
                                 -- on slow ground (see CARRY_PENALTY)
local BOOST_MULT          = 2.0  -- a new holder's speed, times their carry speed
local BOOST_SECONDS       = 2    -- how long the full boost lasts
local BOOST_DECAY_SECONDS = 3    -- how long it then takes to fade back to 1
local INVULN_SECONDS      = 2    -- how long a new holder takes no damage
local DEEP_WATER_SECONDS  = 10   -- how long the holder may carry the prize
                                 -- on deep sea before their tank is killed

local MINE_EVERY        = 30    -- seconds between a tank's mines
local BASE_DOWN_SECONDS = 30    -- how long a used base stays off the map
local REFRESH_SECONDS   = 0.2   -- how often each player's compass is redrawn
local SCORE_LABEL       = "HELD"
local AIM_REFRESH       = 8     -- seconds before a bot is told the same order again
local CHANGE_AFTER      = 4     -- seconds a bot keeps an order before it is changed
local VIEW_SQUARES      = 14    -- how far a brain sees a tank: half its 29-square view
local ATTACK_WITHIN     = 11    -- a chase becomes an attack order this close
local GOTO_SLACK        = 5     -- squares the holder may move before a far bot is
                                -- sent to where he is now
local OVERSHOOT         = 2     -- squares past a grounded pillbox a bot is sent to
local RUN_SQUARES       = 8     -- how far a new holder is sent to get out of a fight
local ORDER_SECONDS     = 60    -- how long a brain holds an order it was handed
local BUILT_ARMOUR      = 3     -- the most armour the prize holds when built. A
                                -- classic pillbox holds 15 and a shell takes 1,
                                -- so this is three hits from standing to dead.
                                -- The cap stays the classic 15, so a built
                                -- prize is drawn as a pillbox shot twelve
                                -- times. A build is set down to this in
                                -- on_pill_placed and a repair in on_built.

-- How the bots play the prize. None of these is a lobby setting, and none of
-- them changes a rule: they are what this script tells a bot to do, and what
-- it tunes the bot's brain to. A person playing the same part is told nothing.
-- Distances are map squares and times are seconds unless a line says other.
local HOP_HUNTER_WITHIN   = 8   -- a bot holder hops (or builds a fort) when a
                                -- hunter is this close
local HOP_EVERY           = 12  -- the least time from one hop or fort to the
                                -- next, counted from the pick-up
local HOP_SIDE_WITHIN     = 5   -- a hunter this close sends the hop square 45
                                -- degrees off the line away from the hunters,
                                -- not straight ahead
local HOP_SPARE_SHELLS    = 2   -- a hop build fills the gun to the prize's
                                -- armour plus this many shells, so a miss or
                                -- two still kills it
local HOP_GIVE_UP         = 6   -- a build order that has not put the prize
                                -- down after this long is forgotten
local HOP_PICKUP_SECONDS  = 10  -- a holder who shot down his own prize keeps
                                -- it this long while he drives over to it
local FORT_ARMOUR_AT      = 20  -- a bot holder whose tank is at or below this
                                -- armour (a full tank is 40) builds a fort
                                -- instead of hopping
local FORT_FREE_WITHIN    = 8   -- the guard takes the fort back when no hunter
                                -- is this close
local FORT_SECONDS        = 10  -- the longest a fort stands before the guard
                                -- takes it back
local FORT_REPAIR_BELOW   = 3   -- the guard holds one tree, for a repair, only
                                -- while the fort has less armour than this
local LAST_SHOT_ARMOUR    = 1   -- a fort shot down to this armour is taken
                                -- back by its guard before a hunter kills it
local RUN_FROM_WITHIN     = 11  -- hunters this close are what a bot holder
                                -- runs from
local RUN_AWAY_SQUARES    = 10  -- how far from those hunters' middle the run
                                -- square is
local RUN_EVERY           = 3   -- how often a running holder gets a new square
local RUN_SEA_MARGIN      = 2   -- a run square is at least this far from deep
                                -- sea
local RUN_PARK_TICKS      = 50  -- brain ticks (50 a second) a holder waits on
                                -- a run square before it plays on
local MINE_BEHIND_WITHIN  = 6   -- a hunter this close behind a bot holder gets
                                -- one of his mines laid in its path
local MINE_BEHIND_GAP     = 3   -- the least time between two of those mines
local ESCORT_BEHIND       = 3   -- team round: how far behind the holder his
                                -- bot teammates are sent
local DROPPED_WITHIN      = 4   -- a hunter this close to a dropped prize is
                                -- sent onto its own square, not past it
local HUNTER_PARK_TICKS   = 25  -- brain ticks a hunter waits on a square it was
                                -- sent to, so its own pillbox fight and
                                -- pick-up take over soon after
local WALL_FALLBACK_SECONDS = 30 -- a standing prize that has lost no armour
                                -- this long after hunters were sent to
                                -- squares with a line to it turns on the
                                -- hunters' shot through the walls

-- The two words of the "Teams" setting, as scenario.settings declares them.
local FREE_FOR_ALL      = "Free For All"
local LOBBY_TEAMS       = "Use Lobby Teams"
local LABEL_MAX         = 22    -- the longest team name a score row shows

-- How much of each ground's speed the holder loses, as a percentage of the
-- distance a classic tank covers there at full speed, when SPEED_PENALTY_PCT
-- is 30. The penalty is not the same everywhere: the faster the ground, the
-- more of it goes. Grass, water and a building lose the penalty itself, a
-- road a third more, a wood half of it, the slow ground a third of it and
-- deep sea nothing. Another penalty scales every row by penalty / 30, so the
-- shape stays and 30 is exactly the table below. The comment is the classic
-- speed cap and the holder's share of it at 30, in world units a frame.
local CARRY_PENALTY = {
  road     = 40,    -- 16 to 9.6
  grass    = 30,    -- 12 to 8.4
  forest   = 15,    --  6 to 5.1
  boat     = 30,    -- 16 to 11.2, on any water
  swamp    = 10,    --  3 to 2.7
  crater   = 10,    --  3 to 2.7
  rubble   = 10,    --  3 to 2.7
  river    = 10,    --  3 to 2.7
  deep_sea = 0,     --  3, sinking
}
local CARRY_PENALTY_OTHER = 30  -- ground not above with no speed of its own
                                -- (a building), set as a plain modifier
local CARRY_PENALTY_AT = 30     -- the SPEED_PENALTY_PCT the table is written for

-- The holder's share of each ground's speed, as a percentage, worked out
-- from the table above and the penalty the host chose. Filled at load from
-- the defaults and again by read_settings; a share can have a fraction,
-- which carry_legs keeps.
local CARRY_SPEED_PCT   = {}
local CARRY_SPEED_OTHER = 100 - CARRY_PENALTY_OTHER

-- The panel square is 128 units on a side, origin top left. The compass fills
-- the middle of it, the round's clock sits above and the scores below.
local CX, CY, RADIUS = 64, 50, 24
local NORTH_Y        = 14       -- the "N" above the rose
local STATUS_Y       = 78       -- what is holding the prize, and how far off
local BOARD_Y        = 90       -- the first score row
local BOARD_STEP     = 9        -- small text is eight units tall
local BOARD_ROWS     = 4

local pill        = nil         -- the one pillbox, 1-based
local holder      = nil         -- the seat carrying it or whose built one
                                -- stands, or nil
local seconds     = {}          -- seconds held, by seat
local running     = false
local over        = false
local elapsed     = 0           -- whole seconds since the round started
local ends_at     = 0           -- the tick the round ends on, for the panel clock
local half_armour = 20          -- half a tank, read off the rules at setup
local base_armour = 90          -- what a restored base comes back with
local told_at     = {}          -- seat -> when a bot was last handed an order
local told        = {}          -- seat -> which order that was, as a short key
local told_for    = {}          -- seat -> what that order was after, as a short key
local goto_at     = {}          -- seat -> the square a goto order sent it to
local tuned       = {}          -- seat -> the init table it was last handed, as text
local peace_of    = {}          -- seat -> the peace list it was last handed
local touched     = {}          -- seat -> every knob and flag word it has been handed
local hide_at     = 3           -- squares off a tank in forest stops being seen
local board       = {}          -- the leaderboard, rebuilt once a second
local drawn       = {}          -- seat -> what its panel last showed, as a short key
local team_of     = {}          -- seat -> the team this script last put it on
local lobby_team  = {}          -- seat -> its lobby team, kept at on_start or join
local team_mode   = nil         -- true for "Use Lobby Teams"; read on first use
local team_seconds = {}         -- lobby team -> seconds its holders carried
local boost_from  = nil         -- the game.tick() the holder took the prize on,
                                -- or nil when there is no boost to run
local invuln_seat = nil         -- the seat damage_scale spares, or nil
local invuln_to   = 0           -- the game.tick() that seat is spared until
local last_holder = nil         -- the seat that took the prize last; kept
                                -- when the prize drops, so a player who takes
                                -- back their own prize is not a new hand
local wet_seat    = nil         -- the holder whose deep sea clock runs, or nil
local wet_from    = 0           -- the game.tick() that clock started on
local warned      = nil         -- the seat with the deep sea warning on
                                -- screen, or nil
local plan        = nil         -- what a bot holder is doing with the prize:
                                -- nil, or { kind = "hop" | "fort" | "take",
                                -- x, y, at, built, dead_at, near }
local hop_after   = 0           -- the game.tick() a bot holder may next hop
local man_was_out = false       -- whether the holder's man had the prize out
                                -- last frame
local forfeit     = nil         -- { p, x, y }: the seat whose man still has
                                -- the prize in his hands after the holder's
                                -- tank died, and the man's square then, or
                                -- nil; what the man puts down is put down
                                -- dead
local run_at      = nil         -- elapsed second of the holder's last run order
local mine_at     = nil         -- elapsed second of the last mine laid behind
local walled      = {}          -- the walled-in prize: watch, the watch on a
                                -- standing prize or nil, and its helpers (see
                                -- walled.update)

local function whole(n)
  return math.floor(n + 0.5)
end

-- The holder's share of each ground, worked out from the penalty. Multiplied
-- before it is divided, so the default penalty gives the table's own whole
-- numbers back exactly. A share is kept at 1 at the least: the lobby's range
-- stops well short of that, but a share of nothing would stop the holder dead.
local function work_out_shares()
  for name, loss in pairs(CARRY_PENALTY) do
    CARRY_SPEED_PCT[name] =
      math.max(1, 100 - loss * SPEED_PENALTY_PCT / CARRY_PENALTY_AT)
  end
  CARRY_SPEED_OTHER = math.max(1, whole(100 - CARRY_PENALTY_OTHER *
                                    SPEED_PENALTY_PCT / CARRY_PENALTY_AT))
end
work_out_shares()

-- The host's lobby choices, over the defaults at the top of the file. Run in
-- on_setup: game.setting answers from inside a hook, where the host has read
-- the scenario table at the bottom of the file, and not at the top level of
-- this one, where that table does not exist yet.
local function read_settings()
  ROUND_SECONDS       = game.setting("round_minutes") * 60
  SPEED_PENALTY_PCT   = game.setting("speed_penalty")
  BOOST_MULT          = game.setting("boost_pct") / 100
  BOOST_SECONDS       = game.setting("boost_seconds")
  BOOST_DECAY_SECONDS = game.setting("boost_decay_seconds")
  INVULN_SECONDS      = game.setting("invuln_seconds")
  DEEP_WATER_SECONDS  = game.setting("deep_water_seconds")
  work_out_shares()
end

-- The host's "Teams" choice, asked the first time a hook needs it, for the
-- same reason read_settings is not run at the top of the file.
local function teams_on()
  if team_mode == nil then
    team_mode = (game.setting("teams") == LOBBY_TEAMS)
  end
  return team_mode
end

-- The seat's lobby team: the one kept for it once the round has started,
-- because a Free For All moves every seat onto a team of its own, and the
-- one the lobby shows before that.
local function lobby_team_of(p)
  if lobby_team[p] ~= nil then
    return lobby_team[p]
  end
  local slot = game.lobby_slot(p)
  return (slot ~= nil) and slot.team or 0
end

-- The team seat p scores for in a team round, or nil: a seat on no team, and
-- every seat in a Free For All, scores for nobody but itself.
local function scoring_team(p)
  if p == nil or not teams_on() then
    return nil
  end
  local t = lobby_team_of(p)
  return (t > 0) and t or nil
end

-- The rose, built once. It never moves and it is the same for everybody, so it
-- is sixteen lines worked out at load and copied into each player's list.
local RING = {}
do
  local SEGMENTS = 16
  local px, py
  for i = 0, SEGMENTS do
    local a = (i / SEGMENTS) * 2 * math.pi
    local x = CX + RADIUS * math.sin(a)
    local y = CY - RADIUS * math.cos(a)
    if i > 0 then
      RING[#RING + 1] = { "line", whole(px), whole(py), whole(x), whole(y),
                          "grey_dark" }
    end
    px, py = x, y
  end
end

-- Where the prize is, in map squares, and what has it. A carried pillbox keeps
-- the square it was last put down on, so its own position is no use while
-- somebody has it: the answer comes from the holder instead. The tank's pill
-- count drops the moment the builder takes it out of the hold, which is why
-- the man is asked about before the tank is.
--
-- A prize on the ground with armour in it is one the holder built, and it is
-- still his. One with none is lying there for anybody.
local function standing(pb)
  return pb ~= nil and not pb.in_tank and pb.armour > 0
end

-- Whether the holder's man is out of the tank with the prize in his hands:
-- the tank's pill count says the pillbox is aboard, and the man is out on a
-- pill job. He keeps the prize until he builds it, dies, or brings it back.
local function man_out(p)
  if p == nil or p ~= holder or pill == nil then
    return false
  end
  local pb = game.pill(pill)
  if pb == nil or not pb.in_tank then
    return false
  end
  local man = game.builder(p)
  return man ~= nil and man.state ~= "in_tank" and man.state ~= "dead" and
         man.job == "pill"
end

-- Whether seat p is the holder with the prize in his tank. That is when he
-- scores and the gun stays empty. While his man walks it out, and while a
-- built prize stands, he scores nothing and is restocked like everybody else.
-- The slow legs stay on him all the while he is the holder (see on_tick).
local function carrying(p)
  if p == nil or p ~= holder then
    return false
  end
  local pb = game.pill(pill)
  return pb ~= nil and pb.in_tank and not man_out(p)
end

-- Whether the holder has just shot his own built prize down and is still in
-- the HOP_PICKUP_SECONDS he has to drive over to it. The prize lies on the
-- ground, but he is still its holder.
local function shot_own_down()
  return holder ~= nil and plan ~= nil and plan.dead_at ~= nil and
         game.tick() - plan.dead_at < HOP_PICKUP_SECONDS * 100
end

local function prize()
  if pill == nil then
    return nil
  end
  local pb = game.pill(pill)
  if pb == nil then
    return nil
  end
  if standing(pb) and holder ~= nil then
    return pb.x + 0.5, pb.y + 0.5, "BUILT", "orange", holder
  end
  if not pb.in_tank then
    -- A holder on his way to his own shot-down prize is still its carrier,
    -- so his panel says it is his and refresh leaves his legs alone.
    return pb.x + 0.5, pb.y + 0.5, "GROUND", "green",
           shot_own_down() and holder or nil
  end
  -- A dead holder's man still walking the prize out is on foot with it, but
  -- it is nobody's: the carrier is the holder, nil by then.
  local walker = holder or (forfeit and forfeit.p)
  if walker == nil then
    return nil
  end
  local man = game.builder(walker)
  if man ~= nil and man.state ~= "in_tank" and man.job == "pill" then
    return man.wx / 256, man.wy / 256, "ON FOOT", "cyan", holder
  end
  if holder == nil then
    return nil
  end
  local t = game.tank(holder)
  if t == nil then
    return nil
  end
  return t.wx / 256, t.wy / 256, "IN TANK", "red", holder
end

-- A tank that sinks puts its cargo down where it went under, and the drop will
-- take a deep-sea square: nothing without a boat could ever reach the prize
-- there. It is walked out to the nearest square a tank can stand on instead.
-- Rivers are left alone — a tank crosses one.
local function rescue_from_the_sea(n)
  local pb = game.pill(n)
  if pb == nil or pb.in_tank then
    return
  end
  if game.map_tile(pb.x, pb.y) ~= game.TERRAIN.deep_sea then
    return
  end

  local function dry(x, y)
    local t = game.map_tile(x, y)
    if t == nil or t == game.TERRAIN.deep_sea or t == game.TERRAIN.boat or
       t == game.TERRAIN.building or t == game.TERRAIN.half_building then
      return false
    end
    return game.move_pill(n, x, y) ~= nil
  end

  for r = 1, 20 do
    for i = -r, r do
      if dry(pb.x + i, pb.y - r) or dry(pb.x + i, pb.y + r) or
         dry(pb.x - r, pb.y + i) or dry(pb.x + r, pb.y + i) then
        game.log(string.format(
          "Pillbox Tag: the pillbox went in the sea at %d,%d and was moved",
          pb.x, pb.y))
        return
      end
    end
  end
end

-- The scores, best first, as far down as the panel has room for.
local function leaderboard()
  local rows = {}
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot ~= nil and slot.connected and slot.fielded then
      rows[#rows + 1] = { slot = p, score = seconds[p] or 0 }
    end
  end
  table.sort(rows, function(a, b)
    if a.score ~= b.score then
      return a.score > b.score
    end
    return a.slot < b.slot
  end)
  return rows
end

-- A seat's name for a line in chat. A dead tank has no table, so the lobby's
-- name for the seat is next.
local function name_of(p)
  local t = game.tank(p)
  local slot = game.lobby_slot(p)
  return (t ~= nil and t.name ~= "" and t.name) or
         (slot ~= nil and slot.name ~= "" and slot.name) or "Somebody"
end

-- The lobby teams of the seats in the round, lowest first.
local function teams_in_round()
  local teams, seen = {}, {}
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    local t = lobby_team_of(p)
    if slot ~= nil and slot.connected and slot.fielded and t > 0 and
       not seen[t] then
      seen[t] = true
      teams[#teams + 1] = t
    end
  end
  table.sort(teams)
  return teams
end

-- A team round posts a score for every team in it, 0 until one of its
-- holders scores, so the recap lists every team with its members under it.
-- A seat not yet fielded is counted too: bots are fielded just after the
-- start.
local function post_team_scores(also)
  if not teams_on() then
    return
  end
  local seen = {}
  local function post(t)
    if t ~= nil and t > 0 and not seen[t] then
      seen[t] = true
      game.score({ team = t }, team_seconds[t] or 0, SCORE_LABEL)
    end
  end
  for q = 0, game.max_tanks() - 1 do
    if game.lobby_slot(q) ~= nil then
      post(lobby_team_of(q))
    end
  end
  post(also)
end

-- The pool label of a team of bots only; nil when a human is on it.
local function bot_pool(team)
  local pool, bots = nil, false
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot ~= nil and lobby_team_of(p) == team then
      if not slot.bot then
        return nil
      end
      bots = true
      pool = pool or slot.team_pool
    end
  end
  if not bots or pool == "" then
    return nil
  end
  return pool
end

-- A team's name on the scores and in the last line. A team with no human on
-- it is named after the pool its bots take their names from, as the lobby
-- names them ("Famous Painters"); any other team is "Team <n>". So is a team
-- whose pool another team in the round shares (a server with no lobby gives
-- every team the same pool), so no two teams read alike. A label too long for
-- a score row is cut short.
local function team_label(team)
  local pool = bot_pool(team)
  if pool == nil then
    return "Team " .. team
  end
  for _, other in ipairs(teams_in_round()) do
    if other ~= team and bot_pool(other) == pool then
      return "Team " .. team
    end
  end
  if #pool > LABEL_MAX then
    pool = pool:sub(1, LABEL_MAX - 2) .. ".."
  end
  return pool
end

-- The team round's scores, best first: a row for each team with its total,
-- and under it a row for each member with the seconds they carried
-- themselves. A seat on no team is a row of its own among the teams.
local function team_board(rows)
  local sides, by_team = {}, {}
  for _, row in ipairs(rows) do
    local t = scoring_team(row.slot)
    if t == nil then
      sides[#sides + 1] = { slot = row.slot, score = row.score }
    else
      if by_team[t] == nil then
        by_team[t] = { team = t, score = team_seconds[t] or 0, members = {} }
        sides[#sides + 1] = by_team[t]
      end
      local m = by_team[t].members
      m[#m + 1] = { slot = row.slot, score = row.score, indent = true }
    end
  end
  table.sort(sides, function(a, b)
    if a.score ~= b.score then
      return a.score > b.score
    end
    local ka = a.team and (a.team * 100) or (10000 + a.slot)
    local kb = b.team and (b.team * 100) or (10000 + b.slot)
    return ka < kb
  end)
  local out = {}
  for _, side in ipairs(sides) do
    out[#out + 1] = side
    for _, m in ipairs(side.members or {}) do
      out[#out + 1] = m
    end
  end
  return out
end
-- Takes the deep sea count off: the clock starts again the next time the
-- holder takes the prize onto deep sea, and the warning comes off the screen
-- it was on.
local function dry_off()
  wet_seat = nil
  if warned ~= nil then
    game.announce("", 0, warned)
    warned = nil
  end
end

-- The deep sea clock. It only counts the holder, and only while the prize is
-- in their tank (not built, and not in their man's hands) and the tank is
-- alive and on a deep sea square, afloat or not. Nobody can drive out to a
-- holder there, so the holder has DEEP_WATER_SECONDS to come back to shore.
-- Their screen shows the seconds left, to a tenth, and is written again on
-- every refresh, so the count goes down as they watch. When it runs out their
-- tank is killed. The kill drops the prize the way sinking does, so
-- on_pill_placed takes it off them and rescue_from_the_sea walks it to land.
-- A kill is not a hit, so damage_scale does not see it, and a new holder's
-- cover does not stop it.
local function watch_the_water(what, carrier)
  local t = nil
  if what == "IN TANK" and carrier ~= nil and carrier == holder then
    t = game.tank(carrier)
  end
  if t == nil or t.dead or
     game.map_tile(t.mx, t.my) ~= game.TERRAIN.deep_sea then
    if wet_seat ~= nil or warned ~= nil then
      dry_off()
    end
    return
  end
  local now = game.tick()
  if wet_seat ~= carrier then
    dry_off()
    wet_seat, wet_from = carrier, now
  end
  local left = DEEP_WATER_SECONDS - (now - wet_from) / 100
  if left <= 0 then
    dry_off()
    game.message(name_of(carrier) .. " stayed in deep water too long.")
    game.kill_tank(carrier)
  else
    warned = carrier
    game.announce(string.format("Deep water! You die in %.1f s", left), 1,
                  carrier)
  end
end

-- One player's compass. The needle is the vector from that player's tank to
-- the prize, normalised to the rose's radius, so no angle is ever worked out
-- and nothing here needs a two-argument arctangent — which LuaJIT and PUC-Lua
-- spell differently and this file would rather not choose between.
--
-- me is that player's tank, which refresh has already read. rows_key is the
-- scores as one line of text. The panel is only sent when the needle's tip,
-- the status line, the holder or the scores differ from what this seat was
-- last sent.
local function compass(p, me, tx, ty, what, colour, carrier, rows, rows_key)
  local status, status_colour = "NO PRIZE", "grey"
  local ux, uy, tipx, tipy = nil, nil, nil, nil
  if carrier == p then
    status, status_colour = "YOURS - RUN", "yellow"
    if what == "BUILT" then
      status = "YOURS - BUILT"
    elseif what == "GROUND" then
      status = "YOURS - DOWN"
    end
  elseif tx ~= nil then
    local dx = tx - (me.wx / 256)
    local dy = ty - (me.wy / 256)
    local len = math.sqrt(dx * dx + dy * dy)
    status_colour = colour
    status = string.format("%s %d", what, math.floor(len + 0.5))
    if len >= 0.5 then
      ux, uy = dx / len, dy / len
      tipx, tipy = CX + RADIUS * ux, CY + RADIUS * uy
    end
  end

  local key = string.format("%s %s %s %s %s %s %s", tostring(carrier),
                            tostring(holder),
                            tipx and whole(tipx) or "-",
                            tipy and whole(tipy) or "-", status_colour, status,
                            rows_key)
  if drawn[p] == key then
    return
  end
  drawn[p] = key

  local list = { { "timer", CX, 1, "yellow", "normal", "centre", "down",
                   ends_at } }
  local n = #list
  for i = 1, #RING do
    n = n + 1
    list[n] = RING[i]
  end
  n = n + 1
  list[n] = { "text", CX, NORTH_Y, "grey", "small", "centre", "N" }

  if carrier == p then
    n = n + 1
    list[n] = { "rect", CX - 6, CY - 6, 12, 12, "yellow", true }
  elseif tipx ~= nil then
    local backx, backy = tipx - 8 * ux, tipy - 8 * uy
    -- The head, off a perpendicular of the same unit vector.
    local px, py = -uy * 4, ux * 4
    n = n + 1
    list[n] = { "line", CX, CY, whole(tipx), whole(tipy), colour }
    n = n + 1
    list[n] = { "line", whole(tipx), whole(tipy), whole(backx + px),
                whole(backy + py), colour }
    n = n + 1
    list[n] = { "line", whole(tipx), whole(tipy), whole(backx - px),
                whole(backy - py), colour }
  end
  n = n + 1
  list[n] = { "text", CX, STATUS_Y, status_colour, "small", "centre", status }

  for i = 1, BOARD_ROWS do
    local row = rows[i]
    if row == nil then
      break
    end
    local y = BOARD_Y + (i - 1) * BOARD_STEP
    -- The holder's rows are yellow, the palette's nearest to gold (it has
    -- no gold), whether or not rows_for had to force them on.
    local shade = "grey"
    if row.team ~= nil then
      shade = (holder ~= nil and row.team == scoring_team(holder)) and
              "yellow" or "white"
    elseif row.slot == holder then
      shade = "yellow"
    elseif row.slot == p then
      shade = "white"
    end
    n = n + 1
    if row.team ~= nil then
      list[n] = { "text", 4, y, shade, "small", "left", team_label(row.team) }
    else
      list[n] = { "name", row.indent and 12 or 4, y, shade, "small", "left",
                  row.slot }
    end
    n = n + 1
    list[n] = { "text", 124, y, shade, "small", "right",
                string.format("%d", row.score) }
  end

  game.panel(0, list, p)
end

-- The scores as one line of text, for the key compass compares.
local function board_key(rows)
  local parts = {}
  for i = 1, BOARD_ROWS do
    local row = rows[i]
    if row == nil then
      break
    end
    parts[i] = (row.team and ("t" .. row.team) or row.slot) .. ":" ..
               row.score
  end
  return table.concat(parts, ",")
end

-- The rows one player's panel shows. When there are more than BOARD_ROWS,
-- a team round keeps every team's row and puts the player's own row under
-- their team's; a Free For All shows the best BOARD_ROWS.
--
-- The holder's row is always one of them. It is his team's row in a team
-- round, and his own row in a Free For All or when he is on no team. When
-- it sorts below the last row there is room for, it takes that last row's
-- place. Everything above it outscores it, so the list stays in score order;
-- and an indented row is never left without its team, because a team's
-- members follow its row.
local function rows_for(p, rows)
  local out = rows
  if #rows > BOARD_ROWS and teams_on() then
    local mine = scoring_team(p)
    out = {}
    for _, row in ipairs(rows) do
      if not row.indent then
        out[#out + 1] = row
        if row.team ~= nil and row.team == mine then
          for _, m in ipairs(rows) do
            if m.indent and m.slot == p then
              out[#out + 1] = m
            end
          end
        end
      end
    end
  end
  if holder == nil or #out <= BOARD_ROWS then
    return out
  end
  local ht = scoring_team(holder)
  for k = BOARD_ROWS + 1, #out do
    local row = out[k]
    if (ht ~= nil and row.team == ht) or
       (ht == nil and row.team == nil and not row.indent and
        row.slot == holder) then
      local shown = {}
      for i = 1, BOARD_ROWS - 1 do
        shown[i] = out[i]
      end
      shown[BOARD_ROWS] = row
      return shown
    end
  end
  return out
end

-- The scores as the panel shows them: one row a tank, or grouped by team.
local function panel_board()
  local rows = leaderboard()
  if teams_on() then
    return team_board(rows)
  end
  return rows
end

-- Five times a second: the deep sea clock, and one panel each. Every player's
-- panel is different, and the panel's own rule is one update per audience per
-- tick, so sixteen of them in the one call is fine. The scores only change once a second, so the board
-- each_second built is the one used here, and a seat whose compass looks the
-- same as last time is not sent it again.
local function refresh()
  if over then
    return
  end
  local tx, ty, what, colour, carrier = prize()
  watch_the_water(what, carrier)
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil then
      local rows = rows_for(p, board)
      compass(p, t, tx, ty, what, colour, carrier, rows, board_key(rows))
      -- A seat that somehow kept the holder's legs without the pillbox gets
      -- them back here. The hooks below are what normally clears them; this
      -- is the net under those.
      if p ~= carrier and t.mods.speed ~= 0 then
        game.set_modifiers(p, {})
      end
    end
  end
  game.timer(REFRESH_SECONDS, refresh)
end

-- How a bot plays the round. Left alone a brain plays the ordinary game: it
-- goes after bases and pillboxes, fights whatever it meets, and puts a
-- pillbox it is carrying down somewhere useful, which here throws the prize
-- away. So every bot is tuned for the part it has, and every bot that is not
-- carrying the prize is told where the prize is.
--
-- A brain is retuned with game.bot_init, which hands it a new init table.
-- The flag words in the table are the whole statement of them: GoalHunter
-- puts noblitz and suicider back itself on every new table, so each table
-- names every flag word of the part. A number is different. GoalHunter
-- writes every cfg=NAME=VALUE into its own constants, and a constant stays
-- where it was put until something writes it again. So a table carries only
-- the numbers that change: have[p] is what this script last wrote into seat
-- p's brain, and a knob it never wrote is at DEFAULTS, the ordinary value
-- read off the brain's constants.lua.
--
-- The ordinary value is the Hard one. The brain's level bundles
-- (MODE_LEVELS in constants.lua) move three of the knobs below for a Medium
-- or Easy bot: TANK_COMBAT_BASE_COST (Medium 40, Easy 60),
-- DEFEND_ALARM_BASE_COST (Medium 140, Easy 220) and CAPTURE_LGM_HUNT (Easy
-- false). A script cannot see a seat's level, and the brain takes no level
-- word after its first think, so once a part has set one of the three and
-- the bot takes another part, that knob comes back as the Hard value here:
-- a former hunter bids for a fight as Hard does, a former guard answers a
-- fort alarm as Hard does, and an Easy bot that has held the prize goes
-- after a builder on a pillbox as Hard does.
local DEFAULTS = {
  TANK_COMBAT_ENABLED                  = true,
  TANK_COMBAT_BASE_COST                = 30,
  TAKE_COVER_W_ENEMY                   = 20,
  FLEE_DANGER_WEIGHT                   = 80,
  STRATEGIC_PLACE_ENABLED              = true,
  EMERGENCY_DROP_ENABLED               = true,
  PILL_REPOSITION_ENABLED              = true,
  BUILDER_POOL_ENABLED                 = true,
  PILL_REPOSITION_IN_OPENING           = false,
  PILL_JUST_BUILT_TICKS                = 1500,
  PILL_REPOSITION_SURPLUS_W            = 50,
  PILL_REPOSITION_LOCK_COST            = 30,
  PILL_REPOSITION_COOLDOWN_TICKS       = 1500,
  PILL_REPOSITION_MIN_SHELLS           = 15,
  REPOSITION_VOTE_RECENT_MEMORY_TICKS  = 1500,
  REPOSITION_VOTE_FAIL_COOLDOWN        = 1500,
  REPOSITION_SCORE_INTERVAL            = 50,
  REPOSITION_DISABLE_WITH_HUMAN_ALLIES = true,
  REPOSITION_VOTE_ENABLED              = true,
  CAPTURE_PILL_BASE_COST               = 20,
  ATTACK_PILL_BASE_COST                = 30,
  ATTACK_FAR_PREEMPT_RANGE             = 7,
  ORDER_GOTO_HOLD_TICKS                = 500,
  ORDER_INJECT_COST                    = 20,
  DEFEND_ALARM_MIN_DIST                = 9,
  DEFEND_ALARM_BASE_COST               = 100,
  CAPTURE_LGM_HUNT                     = true,
  CAPTURE_BASE_EXTRA_COST              = 0,
  CAPTURE_BASE_EXTRA_FREE_DIST         = 0,
  ATTACK_PILL_WALL_FALLBACK            = false,
}

-- The flag words a brain keeps until it is told the opposite. GoalHunter puts
-- noblitz, noclaimdead and suicider back itself on every new table; ammoless
-- it leaves standing, and "normal" is the word that takes it off.
local FLAG_UNDO = { ammoless = "normal" }

-- What every part shares. None of the parts has a reason to put a pillbox
-- down of its own accord, and none should have its builder spend the round
-- on walls and guns: the only pillbox build is the one this script orders.
-- A hunter is told not to put one down as well, because a bot that picks the
-- prize up is still a hunter until its new table reaches the brain, most of
-- a second later, and a brain that is outnumbered with a pillbox aboard
-- drops it beside itself on the very next think (the "emergency drop").
--
-- The rest is the brain's own pillbox move, set up for the hop (see the hop
-- part below) and left switched off by PILL_REPOSITION_ENABLED everywhere
-- else. It is the same in every part so that a holder going from one part to
-- the next is handed as few numbers as can be. A move is never refused for
-- the opening phase, which a round with every base neutral never leaves; a
-- built prize may be moved at once and again straight after; the move costs
-- next to nothing so it wins the bot's choice of goal; three shells are
-- enough to start it; and no memory of an earlier move holds it back. A dead
-- pillbox is worth driving over at no extra cost, which is the pick-up.
local SHARED = {
  STRATEGIC_PLACE_ENABLED              = false,
  EMERGENCY_DROP_ENABLED               = false,
  BUILDER_POOL_ENABLED                 = false,
  PILL_REPOSITION_IN_OPENING           = true,
  PILL_JUST_BUILT_TICKS                = 0,
  PILL_REPOSITION_SURPLUS_W            = 5000,
  PILL_REPOSITION_LOCK_COST            = 1,
  PILL_REPOSITION_COOLDOWN_TICKS       = 0,
  PILL_REPOSITION_MIN_SHELLS           = 3,
  REPOSITION_VOTE_RECENT_MEMORY_TICKS  = 0,
  REPOSITION_VOTE_FAIL_COOLDOWN        = 0,
  REPOSITION_DISABLE_WITH_HUMAN_ALLIES = false,
  CAPTURE_PILL_BASE_COST               = 0,
}

-- What a hunter (and a manhunt and a mate) adds to the cost of driving onto
-- a base to capture it. Every base stays neutral all round, so the brain
-- never leaves its opening phase, which multiplies a near base's cost by 0.3
-- and a pillbox fight's by up to 3. A near base with a path cost of 15 to 23
-- came to 6 to 10, under the holder (17 to 37) and the prize (100 to 760). A
-- base is only a pit stop here, and armour from one is the brain's refuel,
-- which this does not touch. 3000 puts the nearest base at 900 or more.
-- A base within 5 tiles (Manhattan, from the tank) does not pay it, so a bot
-- that passes close to a base takes it anyway. The brain knob for that is
-- CAPTURE_BASE_EXTRA_FREE_DIST, set to 5 beside BASE_EXTRA in the three parts
-- below.
local BASE_EXTRA = 3000

-- The parts.
--
-- hunter: bids hard for a tank fight, because the holder is a tank. The only
-- live pillbox a hunter ever meets is a built prize, and it costs nothing to
-- go after, so its own pillbox fight takes it on. It leaves the bases alone
-- (BASE_EXTRA) unless one is within 5 tiles, and so do a manhunt and a mate.
-- A square it was sent to holds it for half a second, not ten, so that fight
-- and the pick-up of a dropped prize take over soon after it arrives.
--
-- manhunt: a hunter while the holder's man walks the prize out. A tank fight
-- costs more and a far target is less of a reason not to go, so the brain's
-- own hunt for a man on foot wins over shooting the holder's empty tank, and
-- the attack order it is handed does not outbid that hunt either.
--
-- mate: in a team round, a bot on the holder's team. It never votes on a
-- pillbox move, so its silence is a yes and the holder's hop goes ahead.
--
-- holder: does not fight at all (it has no shells, and a tank fight is also
-- when a brain puts a carried pillbox down as a guard), weighs danger twice
-- as heavily when it runs, looks for cover sooner, never goes looking for a
-- base to refuel at (nothing will fill its gun), and waits a second on a run
-- square before it plays on.
--
-- hop: the holder once a hop has put the prize down. It moves the prize, which
-- in the brain is: shoot your own pillbox down and drive over it. Danger and
-- cover are weighed the ordinary way, so running off does not beat the
-- pick-up, and the move is looked at five times a second. It drives straight
-- at the prize: the brain's habit of turning to shoot at a man near a pillbox
-- it is going to pick up is off, because a hunter's man near the prize had
-- the tank turning to him and back to the prize, over and over, at range.
--
-- guard: the holder once a fort has put the prize down. It has shells again,
-- so it fights, and it goes to the fort's defence as soon as the fort is
-- under fire, from beside it as well as from afar.
local ROLES = {
  hunter = {
    flags = { "noblitz", "nosuicider" },
    cfg = {
      TANK_COMBAT_BASE_COST        = 10,
      CAPTURE_BASE_EXTRA_COST      = BASE_EXTRA,
      CAPTURE_BASE_EXTRA_FREE_DIST = 5,
      PILL_REPOSITION_ENABLED      = false,
      ATTACK_PILL_BASE_COST        = 0,
      ORDER_GOTO_HOLD_TICKS        = HUNTER_PARK_TICKS,
    },
  },
  manhunt = {
    flags = { "noblitz", "nosuicider" },
    cfg = {
      TANK_COMBAT_BASE_COST        = 60,
      CAPTURE_BASE_EXTRA_COST      = BASE_EXTRA,
      CAPTURE_BASE_EXTRA_FREE_DIST = 5,
      PILL_REPOSITION_ENABLED      = false,
      ATTACK_PILL_BASE_COST        = 0,
      ORDER_GOTO_HOLD_TICKS        = HUNTER_PARK_TICKS,
      ATTACK_FAR_PREEMPT_RANGE     = 11,
      ORDER_INJECT_COST            = 60,
    },
  },
  mate = {
    flags = { "noblitz", "nosuicider" },
    cfg = {
      TANK_COMBAT_BASE_COST        = 10,
      CAPTURE_BASE_EXTRA_COST      = BASE_EXTRA,
      CAPTURE_BASE_EXTRA_FREE_DIST = 5,
      PILL_REPOSITION_ENABLED      = false,
      ATTACK_PILL_BASE_COST        = 0,
      ORDER_GOTO_HOLD_TICKS        = HUNTER_PARK_TICKS,
      REPOSITION_VOTE_ENABLED      = false,
    },
  },
  holder = {
    flags = { "noblitz", "nosuicider", "ammoless" },
    cfg = {
      TANK_COMBAT_ENABLED     = false,
      TAKE_COVER_W_ENEMY      = 60,
      FLEE_DANGER_WEIGHT      = 160,
      PILL_REPOSITION_ENABLED = false,
      ORDER_GOTO_HOLD_TICKS   = RUN_PARK_TICKS,
    },
  },
  hop = {
    flags = { "noblitz", "nosuicider", "ammoless" },
    cfg = {
      TANK_COMBAT_ENABLED       = false,
      PILL_REPOSITION_ENABLED   = true,
      REPOSITION_SCORE_INTERVAL = 10,
      ORDER_GOTO_HOLD_TICKS     = RUN_PARK_TICKS,
      CAPTURE_LGM_HUNT          = false,
    },
  },
  guard = {
    flags = { "noblitz", "nosuicider" },
    cfg = {
      PILL_REPOSITION_ENABLED = false,
      DEFEND_ALARM_MIN_DIST   = 0,
      DEFEND_ALARM_BASE_COST  = 30,
    },
  },
}

-- A table holds sixteen pairs of at most 63 bytes a value, and one key named
-- cfg. More than one override goes in one value by starting each after the
-- first with ";cfg=", because the brain splits the whole table on ";" before
-- it reads any of it; and more than fit in one value go under keys of their
-- own (cfg2, cfg3, ...) whose value starts "0;", so the key's own token,
-- "cfg2=0", is one the brain reads as nothing at all.
local INIT_VALUE_MAX = 63
local INIT_PAIRS_MAX = 16

local function cfg_text(v)
  if v == true then return "true" end
  if v == false then return "false" end
  return tostring(v)
end

-- A seat that is in the round: on the roster, and on the field.
local function in_round(p)
  local slot = game.lobby_slot(p)
  return slot ~= nil and slot.connected and slot.fielded
end

-- The seats a bot leaves alone: every other seat in the round but the holder.
-- It goes to the brain as its peace list ("peace=1/3/4"), so the hunters go
-- for the holder and the prize and not for each other. A seat on the list
-- that shoots the bot is fought back for a while all the same. The holder's
-- own list is empty, and so is the list of a bot with nobody to spare. The
-- list is sent again whenever it changes: a new holder comes off every list,
-- and the old one goes on.
local function peace_text(p)
  if p == holder then
    return ""
  end
  local seats = {}
  for q = 0, game.max_tanks() - 1 do
    if q ~= p and q ~= holder and in_round(q) then
      seats[#seats + 1] = tostring(q)
    end
  end
  return table.concat(seats, "/")
end

local function is_bot(p)
  local slot = game.lobby_slot(p)
  return slot ~= nil and slot.bot
end

local have = {}                 -- seat -> knob -> the value last written into
                                -- its brain; a knob not here is at DEFAULTS

-- The part seat p plays now.
local function role_of(p)
  if p == holder then
    if plan ~= nil and plan.built then
      return (plan.kind == "fort") and "guard" or "hop"
    end
    return "holder"
  end
  local held_by = scoring_team(holder)
  if held_by ~= nil and scoring_team(p) == held_by then
    return "mate"
  end
  if man_out(holder) then
    return "manhunt"
  end
  return "hunter"
end

-- The table for one seat in one part, and the same table as one line of text
-- so two of them can be compared. The numbers in it are the ones that differ
-- from what the brain already has, as many as fit; the rest wait for the next
-- table. `sent` is the numbers it carries.
local function init_table(role_name, p)
  local role = ROLES[role_name]
  local had  = touched[p] or {}
  local now  = have[p] or {}
  local want = {}
  for k, v in pairs(DEFAULTS) do
    want[k] = v
  end
  for k, v in pairs(SHARED) do
    want[k] = v
  end
  for k, v in pairs(role.cfg) do
    want[k] = v
  end
  -- A hunter after a walled-in prize may shoot through the walls once the
  -- watch says so (see walled.update). It goes off with the watch.
  if walled.watch ~= nil and walled.watch.fallback and
     (role_name == "hunter" or role_name == "manhunt") then
    want.ATTACK_PILL_WALL_FALLBACK = true
  end

  local t, pairs_n = {}, 1
  t.peace = peace_text(p)
  local flags = {}
  for _, f in ipairs(role.flags) do
    flags[f] = true
    t[f] = "1"
    pairs_n = pairs_n + 1
  end
  for f, undo in pairs(FLAG_UNDO) do
    if had[f] and not flags[f] then
      t[undo] = "1"
      pairs_n = pairs_n + 1
    end
  end

  local names = {}
  for k, v in pairs(want) do
    local cur = now[k]
    if cur == nil then
      cur = DEFAULTS[k]
    end
    if v ~= cur then
      names[#names + 1] = k
    end
  end
  table.sort(names)
  local chunks, value = {}, nil
  local sent, pending = {}, {}
  for _, k in ipairs(names) do
    local item = k .. "=" .. cfg_text(want[k])
    -- Every value after the first carries the six bytes of "0;cfg=" as well.
    local room = (#chunks == 0) and INIT_VALUE_MAX or INIT_VALUE_MAX - 6
    if value ~= nil and #value + 5 + #item <= room then
      value = value .. ";cfg=" .. item
      pending[k] = want[k]
    elseif pairs_n + #chunks + 1 + ((value ~= nil) and 1 or 0) <= INIT_PAIRS_MAX then
      if value ~= nil then
        chunks[#chunks + 1] = value
        for q, v in pairs(pending) do
          sent[q] = v
        end
        pending = {}
      end
      value = item
      pending[k] = want[k]
    end
  end
  if value ~= nil then
    chunks[#chunks + 1] = value
    for q, v in pairs(pending) do
      sent[q] = v
    end
  end
  for i, v in ipairs(chunks) do
    if i == 1 then
      t.cfg = v
    else
      t["cfg" .. i] = "0;cfg=" .. v
    end
  end

  local keys = {}
  for k in pairs(t) do
    keys[#keys + 1] = k
  end
  table.sort(keys)
  local line = {}
  for i, k in ipairs(keys) do
    line[i] = k .. "=" .. t[k]
  end
  return t, table.concat(line, " "), sent
end

-- Hands a bot the table for the part it has, unless the brain already has all
-- of it: every table a brain takes it says a line about, so the same one
-- twice is noise. A refusal is left for the next second to try again: a bot
-- that has only just joined may not have a brain to take it yet. So is
-- whatever did not fit in one table.
local function tune(p)
  if not is_bot(p) then
    return
  end
  local name = role_of(p)
  local t, line, sent = init_table(name, p)
  if next(sent) == nil and tuned[p] == name and peace_of[p] == t.peace then
    return
  end
  if game.bot_init(p, t) then
    tuned[p] = name
    peace_of[p] = t.peace
    local now = have[p] or {}
    for k, v in pairs(sent) do
      now[k] = v
    end
    have[p] = now
    local had = touched[p] or {}
    for _, f in ipairs(ROLES[name].flags) do
      had[f] = true
    end
    touched[p] = had
  end
end

local function tune_everybody()
  for p = 0, game.max_tanks() - 1 do
    if in_round(p) then
      tune(p)
    end
  end
end

-- Every bot but the holder is told where the prize is with a hint, which the
-- brain takes the way it takes an order a teammate types: it says it back,
-- holds it for a minute, and drops it for anything newer.
--
-- The prize is in one of three places. On the ground, a bot is sent to a
-- square a little past it on the side away from the bot: a bot sent to a
-- square stops once it is within one of it and waits there, so sending it to
-- the pillbox's own square could park it beside the pillbox without ever
-- driving over it. Past it, the pillbox is on the way. A bot that stops
-- short is sent back across from the other side. With the man walking the
-- pillbox out, the bot is sent to the man. In a tank, the bot is told to
-- attack the holder when it is close enough to see him and he is not in a
-- wood, and is sent to where he is otherwise: an attack order is given up,
-- with a line in chat, after ten seconds without a sight of the target.
--
-- The same order handed to a bot that still holds it puts its minute back to
-- the start without a word said, so each bot is handed its order again every
-- eight seconds. A new order is a line in chat, and a changed one is two, so
-- a bot keeps an attack while its man is in view and keeps the square it was
-- sent to until the prize has moved a few squares off it.
local function chebyshev(ax, ay, bx, by)
  local dx, dy = math.abs(ax - bx), math.abs(ay - by)
  return (dx > dy) and dx or dy
end

local function sign(n)
  if n > 0 then return 1 end
  if n < 0 then return -1 end
  return 0
end

local function in_forest(x, y)
  local t = game.map_tile(x, y)
  return t == game.TERRAIN.forest or t == game.TERRAIN.mine_forest
end

-- Somewhere a tank can be sent: on the map, not the sea, not a building.
local function standable(x, y)
  if x < 0 or x > 255 or y < 0 or y > 255 then
    return false
  end
  local t = game.map_tile(x, y)
  return t ~= nil and t ~= game.TERRAIN.deep_sea and
         t ~= game.TERRAIN.building and t ~= game.TERRAIN.half_building
end

local function standable_near(x, y)
  if standable(x, y) then
    return x, y
  end
  for r = 1, 3 do
    for i = -r, r do
      if standable(x + i, y - r) then return x + i, y - r end
      if standable(x + i, y + r) then return x + i, y + r end
      if standable(x - r, y + i) then return x - r, y + i end
      if standable(x + r, y + i) then return x + r, y + i end
    end
  end
  return nil
end

local function goto_hint(x, y, target, px, py)
  return { key = string.format("goto %d,%d", x, y), x = x, y = y,
           target = target, px = px, py = py,
           hint = { verb = "goto", x = x, y = y } }
end

-- Sent after something that moves: the square it was sent to is kept until
-- the target is GOTO_SLACK squares off it, or the bot has got there.
local function goto_order(p, x, y, from, target)
  local g = goto_at[p]
  if g ~= nil and g.px == nil and chebyshev(g.x, g.y, x, y) <= GOTO_SLACK and
     chebyshev(from.x, from.y, g.x, g.y) > 2 then
    return goto_hint(g.x, g.y, target)
  end
  x, y = standable_near(x, y)
  if x == nil then
    return nil
  end
  return goto_hint(x, y, target)
end

-- Sent to the pillbox on the ground: a square OVERSHOOT past it, kept while
-- the pillbox stays put and the bot has not got there.
local function ground_order(p, px, py, from)
  local target = string.format("ground %d,%d", px, py)
  local g = goto_at[p]
  if g ~= nil and g.px == px and g.py == py and
     chebyshev(from.x, from.y, g.x, g.y) > 1 then
    return goto_hint(g.x, g.y, target, px, py)
  end
  local sx, sy = sign(px - from.x), sign(py - from.y)
  for k = OVERSHOOT, 0, -1 do
    if standable(px + k * sx, py + k * sy) then
      return goto_hint(px + k * sx, py + k * sy, target, px, py)
    end
  end
  local x, y = standable_near(px, py)
  if x == nil then
    return nil
  end
  return goto_hint(x, y, target, px, py)
end

local function order_for_tank(p, q, from)
  local s = game.tank(q)
  if s == nil or s.dead then
    return nil
  end
  local d = chebyshev(from.x, from.y, s.mx, s.my)
  local hidden = in_forest(s.mx, s.my) and d > hide_at
  local key = "attack " .. q
  local attack = { key = key, target = "tank " .. q,
                   hint = { verb = "attack", player = q } }
  -- A man who steps into a wood mid-fight is kept as an attack until the
  -- order is due again, rather than swapped for a goto the moment he is out
  -- of sight: at the edge of a forest that would be a new order every other
  -- second. By then the brain has either seen him again or is about to give
  -- the attack up, and he is sent to the square instead.
  if told[p] == key and d <= VIEW_SQUARES then
    if not hidden or (told_at[p] ~= nil and elapsed - told_at[p] < AIM_REFRESH) then
      return attack
    end
  end
  if not hidden and d <= ATTACK_WITHIN then
    return attack
  end
  return goto_order(p, s.mx, s.my, from, "tank " .. q)
end

-- A bot whose man is out of the tank answers any order with "Busy", so it is
-- not told anything until he is back in. A bot that is waiting to come back
-- is told as soon as its man reads as in the tank again.
--
-- A new order for the same quarry waits until the one the bot holds is
-- CHANGE_AFTER seconds old, and a goto that has only moved waits the whole
-- AIM_REFRESH: each change is two lines in chat (the bot says what it is
-- leaving, then answers the new order), and a man on the run would otherwise
-- move the square every few seconds. A new quarry is told at once.
local function tell(p, order, force)
  if order == nil then
    return
  end
  local age = (told_at[p] ~= nil) and (elapsed - told_at[p]) or nil
  if force then
    age = nil
  end
  if age ~= nil and order.key == told[p] then
    if age < AIM_REFRESH then
      return
    end
  elseif age ~= nil and order.target == told_for[p] then
    local wait = CHANGE_AFTER
    if order.hint.verb == "goto" and told[p]:sub(1, 4) == "goto" then
      wait = AIM_REFRESH
    end
    if age < wait then
      return
    end
  end
  local man = game.builder(p)
  if man ~= nil and man.state ~= "in_tank" then
    return
  end
  if game.hint(p, order.hint) then
    told[p]     = order.key
    told_for[p] = order.target
    told_at[p]  = elapsed
    goto_at[p] = order.x and { x = order.x, y = order.y,
                               px = order.px, py = order.py } or nil
  end
end

-- The unit step a 0-255 facing points along, in map squares: 0 is north and
-- 64 east, and y grows to the south.
local function facing(dir)
  local a = (dir - 64) / 256 * 2 * math.pi
  return math.cos(a), math.sin(a)
end

-- In a team round the holder's bot teammates ride with him: each is sent to
-- the square ESCORT_BEHIND behind his tank, kept until he has moved off it.
local function escort_order(p, from)
  local s = game.tank(holder)
  if s == nil or s.dead then
    return nil
  end
  local fx, fy = facing(s.dir)
  local x = whole(s.mx - ESCORT_BEHIND * fx)
  local y = whole(s.my - ESCORT_BEHIND * fy)
  return goto_order(p, x, y, from, "escort " .. holder)
end

-- While the holder's man walks the prize out, every hunter is told to attack
-- the holder: an attack order leaves the brain free to choose its own goal,
-- which is how its hunt for a man on foot gets him (see the manhunt part). A
-- goto would hold the bot to the square and shut that hunt off.
local function man_order(q)
  return { key = "attack " .. q, target = "man " .. q,
           hint = { verb = "attack", player = q } }
end

-- A prize behind walls. A brain fights a pillbox only from its ring of firing
-- squares: 72 squares, one every 5 degrees, STANDOFF squares from the
-- pillbox's middle. It takes a square only when a shell from there has a line
-- to the pillbox (its middle or a corner) that crosses no wall; trees are
-- fine, they only cost shells. A person who builds the prize inside walls can
-- leave no such square, or none a hunter can drive to, and then the hunters
-- never fire at it.
--
-- So while a prize stands, walled.update finds the ring squares with a line,
-- and sends each hunter that can drive to one to its own square (two hunters
-- share one only when there are more hunters than squares). That goto is a
-- short hold, and from that square the brain's own pillbox fight takes over.
-- When there is no such square, or no hunter can drive to one, or the prize
-- has lost no armour for WALL_FALLBACK_SECONDS after the hunters were sent,
-- the hunters are tuned to shoot through the walls
-- (ATTACK_PILL_WALL_FALLBACK). That stays on until the prize is picked up or
-- dies.
--
-- The work is cut to fit the instruction budget of one call: the squares are
-- found in one second, which of them a tank can drive to in the next, and
-- both again every AIM_REFRESH seconds. "Can drive to" is worked out inside
-- the VIEW_SQUARES box around the prize only: the squares a tank can reach
-- from each other inside the box, and whether that ground runs out to the
-- edge of the box. A hunter outside the box can reach the ground that runs to
-- the edge.
walled.STANDOFF  = 7.4          -- the brain's ATTACK_PILL_STANDOFF
walled.AIM_INSET = 16 / 256     -- the brain's AIM_INSET_FIRE: how far inside
                                -- the pillbox's square a corner aim is
walled.AIM_POINTS = {
  { 0.5, 0.5 },
  { walled.AIM_INSET, walled.AIM_INSET },
  { 1 - walled.AIM_INSET, walled.AIM_INSET },
  { walled.AIM_INSET, 1 - walled.AIM_INSET },
  { 1 - walled.AIM_INSET, 1 - walled.AIM_INSET },
}

function walled.is_wall(x, y)
  local t = game.map_tile(x, y)
  return t == game.TERRAIN.building or t == game.TERRAIN.half_building
end

-- Whether a straight line from (fx, fy) to (ax, ay), in squares, reaches the
-- pillbox's square (px, py) without crossing a wall. It steps square by
-- square along the line. The square it starts on does not count, the way the
-- brain's own test leaves it out.
function walled.clear_line(fx, fy, ax, ay, px, py)
  local x, y = math.floor(fx), math.floor(fy)
  local dx, dy = ax - fx, ay - fy
  local sx = (dx > 0) and 1 or -1
  local sy = (dy > 0) and 1 or -1
  local step_x = (dx ~= 0) and math.abs(1 / dx) or math.huge
  local step_y = (dy ~= 0) and math.abs(1 / dy) or math.huge
  local next_x = (dx > 0) and (x + 1 - fx) * step_x or (fx - x) * step_x
  local next_y = (dy > 0) and (y + 1 - fy) * step_y or (fy - y) * step_y
  while next_x <= 1 or next_y <= 1 do
    if next_x < next_y then
      x, next_x = x + sx, next_x + step_x
    else
      y, next_y = y + sy, next_y + step_y
    end
    if x == px and y == py then
      return true
    end
    if walled.is_wall(x, y) then
      return false
    end
  end
  return true
end

-- The ring squares around a pillbox at (px, py) a tank can stand on with a
-- line to it, the way the brain works them out.
function walled.line_squares(px, py)
  local out, seen = {}, {}
  for deg = 0, 355, 5 do
    local r = math.rad(deg)
    local fx = px + 0.5 + math.sin(r) * walled.STANDOFF
    local fy = py + 0.5 - math.cos(r) * walled.STANDOFF
    local x, y = math.floor(fx), math.floor(fy)
    local key = y * 256 + x
    if not seen[key] and standable(x, y) then
      local t = game.map_tile(x, y)
      if t ~= game.TERRAIN.river and t ~= game.TERRAIN.boat then
        for _, a in ipairs(walled.AIM_POINTS) do
          if walled.clear_line(fx, fy, px + a[1], py + a[2], px, py) then
            seen[key] = true
            out[#out + 1] = { x = x, y = y, key = key }
            break
          end
        end
      end
    end
  end
  return out
end

-- The ground a tank can drive over inside the VIEW_SQUARES box around
-- (px, py), in pieces: a flood from each square in `squares` that no earlier
-- flood reached. Returns piece (square key -> piece number) and edge (piece
-- number -> true when the piece runs out to the edge of the box).
function walled.pieces(px, py, squares)
  local x0, x1 = px - VIEW_SQUARES, px + VIEW_SQUARES
  local y0, y1 = py - VIEW_SQUARES, py + VIEW_SQUARES
  local piece, edge, seen = {}, {}, {}
  local deep, wall, half = game.TERRAIN.deep_sea, game.TERRAIN.building,
                           game.TERRAIN.half_building
  for c, sq in ipairs(squares) do
    if seen[sq.key] == nil then
      seen[sq.key] = c
      local queue, head = { sq.key }, 1
      while head <= #queue do
        local k = queue[head]
        head = head + 1
        local kx, ky = k % 256, math.floor(k / 256)
        if kx == x0 or kx == x1 or ky == y0 or ky == y1 then
          edge[c] = true
        end
        for i = 1, 4 do
          local nx, ny = kx, ky
          if i == 1 then nx = kx + 1 elseif i == 2 then nx = kx - 1
          elseif i == 3 then ny = ky + 1 else ny = ky - 1 end
          local nk = ny * 256 + nx
          if nx >= x0 and nx <= x1 and ny >= y0 and ny <= y1 and
             nx >= 0 and nx <= 255 and ny >= 0 and ny <= 255 and
             seen[nk] == nil then
            local t = game.map_tile(nx, ny)
            if t ~= nil and t ~= deep and t ~= wall and t ~= half then
              seen[nk] = c
              queue[#queue + 1] = nk
            else
              seen[nk] = false
            end
          end
        end
      end
    end
  end
  for _, sq in ipairs(squares) do
    piece[sq.key] = seen[sq.key]
  end
  return piece, edge, seen
end

-- A bot that goes after the prize: not the holder, and not on his team.
function walled.hunter(p)
  if p == holder or not is_bot(p) or not in_round(p) then
    return false
  end
  local held_by = scoring_team(holder)
  return held_by == nil or scoring_team(p) ~= held_by
end

function walled.fallback_on(why)
  walled.watch.fallback = true
  game.log(string.format("Pillbox Tag: hunters shoot through the walls at %d,%d (%s)",
                         walled.watch.x, walled.watch.y, why))
  tune_everybody()
end

-- Whether a hunter on (x, y) can drive to square sq.
function walled.can_reach(w, x, y, sq)
  local c = w.piece[sq.key]
  if chebyshev(x, y, w.x, w.y) >= VIEW_SQUARES then
    return w.edge[c] == true
  end
  return w.seen[y * 256 + x] == c
end

-- Once a second.
function walled.update()
  local pb = (pill ~= nil) and game.pill(pill) or nil
  if not standing(pb) or holder == nil or (plan ~= nil and plan.kind == "hop") then
    if walled.watch ~= nil then
      walled.watch = nil
      tune_everybody()
    end
    return
  end
  if walled.watch == nil or walled.watch.x ~= pb.x or walled.watch.y ~= pb.y then
    walled.watch = { x = pb.x, y = pb.y, armour = pb.armour, loss_at = elapsed,
                     assign = {}, sent = {}, taken = {}, fallback = false }
  end
  local w = walled.watch
  if pb.armour < w.armour then
    w.loss_at = elapsed
  end
  w.armour = pb.armour
  if w.fallback then
    return
  end
  if w.sent_at ~= nil and
     elapsed - math.max(w.loss_at, w.sent_at) >= WALL_FALLBACK_SECONDS then
    walled.fallback_on("no armour lost in " .. WALL_FALLBACK_SECONDS .. " s")
    return
  end
  if w.squares == nil or elapsed - w.squares_at >= AIM_REFRESH then
    w.squares, w.squares_at, w.piece = walled.line_squares(pb.x, pb.y), elapsed, nil
    if #w.squares == 0 then
      walled.fallback_on("no square has a line")
    end
    return
  end
  if w.piece == nil then
    w.piece, w.edge, w.seen = walled.pieces(pb.x, pb.y, w.squares)
    return
  end
  -- Give every hunter that has none yet a square it can drive to: the
  -- nearest one nobody else has, if there is one.
  local hunters, can = 0, 0
  for p = 0, game.max_tanks() - 1 do
    local t = walled.hunter(p) and game.tank(p) or nil
    if t ~= nil and not t.dead then
      hunters = hunters + 1
      if w.assign[p] ~= nil then
        can = can + 1
      else
        local best, best_d, best_free = nil, nil, false
        for _, sq in ipairs(w.squares) do
          if walled.can_reach(w, t.mx, t.my, sq) then
            local free = not w.taken[sq.key]
            local d = chebyshev(t.mx, t.my, sq.x, sq.y)
            if best == nil or (free and not best_free) or
               (free == best_free and d < best_d) then
              best, best_d, best_free = sq, d, free
            end
          end
        end
        if best ~= nil then
          w.assign[p] = best
          w.taken[best.key] = true
          can = can + 1
        end
      end
    end
  end
  if hunters > 0 and can == 0 then
    walled.fallback_on("no hunter can reach a square with a line")
  end
end

local function aim_one(p)
  if p == holder or pill == nil or not is_bot(p) or not in_round(p) then
    return
  end
  local t = game.tank(p)
  if t == nil or t.dead then
    return
  end
  local from = { x = t.mx, y = t.my }
  -- In a team round the holder's teammates are not sent after him: they ride
  -- along behind him.
  local held_by = scoring_team(holder)
  if held_by ~= nil and scoring_team(p) == held_by then
    tell(p, escort_order(p, from))
    return
  end
  local pb = game.pill(pill)
  if pb == nil then
    return
  end
  -- A built prize gets no order. The only order a script can give a bot
  -- about a pillbox is defend, and a goto is a hold that shuts off every
  -- other goal, the brain's pillbox fight with them. Left alone, the brain
  -- sees a hostile pillbox and goes after it itself.
  --
  -- The order the bot was given before the prize went up is called off, once:
  -- it still holds it for up to a minute, and an attack on the holder or a
  -- goto after him keeps it on him and off the prize (a person who builds by
  -- his fort had every hunter sitting on him, unable to shoot through the
  -- walls, while the fort shot them). The brain has no word that only ends
  -- an order, so the bot is told to hold where it is: it is there already, so
  -- the hold is the hunter's short park, and then its own choice takes over.
  -- A bot holder's hop is left as it was: the prize stands for a few seconds
  -- only, and he shoots it down and takes it back himself.
  if standing(pb) then
    -- A hunter with a square that has a line to a walled-in prize is sent
    -- there once (see walled.update); after that its own choice plays on.
    if walled.watch ~= nil and walled.watch.sent[p] then
      return
    end
    local sq = (walled.watch ~= nil and not walled.watch.fallback) and walled.watch.assign[p] or nil
    if sq ~= nil then
      local order = goto_hint(sq.x, sq.y, string.format("line %d,%d", pb.x, pb.y))
      tell(p, order, true)
      if told[p] == order.key then
        walled.watch.sent[p] = true
        walled.watch.sent_at = walled.watch.sent_at or elapsed
      end
      return
    end
    if told[p] ~= nil and told[p] ~= "release" and
       (plan == nil or plan.kind ~= "hop") then
      local man = game.builder(p)
      if (man == nil or man.state == "in_tank") and
         game.hint(p, { verb = "hold" }) then
        told[p], told_for[p], told_at[p] = "release", "release", elapsed
        goto_at[p] = nil
      end
    end
    return
  end
  if not pb.in_tank then
    -- Close to it, the bot is sent onto the prize's own square, and the short
    -- park on it hands over to the brain's own pick-up, which drives over a
    -- dead pillbox rather than stopping beside it.
    if chebyshev(from.x, from.y, pb.x, pb.y) <= DROPPED_WITHIN then
      tell(p, goto_hint(pb.x, pb.y, string.format("onto %d,%d", pb.x, pb.y),
                        pb.x, pb.y))
    else
      tell(p, ground_order(p, pb.x, pb.y, from))
    end
    return
  end
  if holder == nil then
    return
  end
  if man_out(holder) then
    tell(p, man_order(holder))
    return
  end
  tell(p, order_for_tank(p, holder, from))
end

local function aim_everybody()
  for p = 0, game.max_tanks() - 1 do
    aim_one(p)
  end
end

-- A bot that picks the prize up may still be holding an order to attack the
-- tank it took the prize from, and a brain in a tank fight with a pillbox in
-- the hold puts the pillbox down as a guard. Tuning the holder to not fight
-- does not stop a fight it was ordered into, so that order is replaced with
-- a run: RUN_SQUARES off, straight away from the nearest other tank. A bot
-- driving to a square it was sent to does nothing else on the way.
local function stop_the_fight(p)
  if told[p] == nil or told[p]:sub(1, 6) ~= "attack" or told_at[p] == nil or
     elapsed - told_at[p] >= ORDER_SECONDS then
    return
  end
  local me = game.tank(p)
  if me == nil or me.dead then
    return
  end
  local near, near_d = nil, nil
  for q = 0, game.max_tanks() - 1 do
    local s = game.tank(q)
    if q ~= p and s ~= nil and not s.dead then
      local d = chebyshev(me.mx, me.my, s.mx, s.my)
      if near_d == nil or d < near_d then
        near, near_d = s, d
      end
    end
  end
  local x, y = me.mx, me.my
  if near ~= nil then
    x = x + RUN_SQUARES * sign(me.mx - near.mx)
    y = y + RUN_SQUARES * sign(me.my - near.my)
  end
  x, y = standable_near(x, y)
  if x == nil then
    x, y = me.mx, me.my
  end
  tell(p, goto_hint(x, y, "run"), true)
end

-- A standing prize with more than BUILT_ARMOUR in it is set back down. The
-- pillbox cap stays the classic 15, which is what makes armour 3 draw as a
-- pillbox shot twelve times: the picture is the armour scaled against the
-- cap, so a cap of 3 would draw the prize whole. A build arrives at the cap
-- and a repair can reach it, so both come through here.
local function hold_down_the_prize()
  if pill == nil then
    return
  end
  local pb = game.pill(pill)
  if standing(pb) and pb.armour > BUILT_ARMOUR then
    game.set_pill_armour(pill, BUILT_ARMOUR)
  end
end

-- Free For All: the lowest team number no other seat was given, or 0 (no
-- team) when all fifteen are taken, which only a sixteenth seat ever meets.
local function free_team(p)
  local used = {}
  for q, t in pairs(team_of) do
    if q ~= p and game.lobby_slot(q) ~= nil then
      used[t] = true
    end
  end
  for t = 1, game.max_tanks() - 1 do
    if not used[t] then
      return t
    end
  end
  return 0
end

-- The team seat p belongs on for the round: its lobby team in a team round,
-- and a team of its own in a Free For All, whoever holds the prize. The
-- lobby team is kept first, to score for and to be put back at the end. A
-- seat is only moved when it is not already there, because each move sends
-- every client the whole alliance table again.
local function sort_team(p)
  local slot = game.lobby_slot(p)
  if over or slot == nil then
    return
  end
  if lobby_team[p] == nil then
    lobby_team[p] = slot.team
  end
  local want
  if teams_on() then
    want = lobby_team[p]
  else
    want = team_of[p] or free_team(p)
  end
  if slot.team == want or game.set_team(p, want) then
    team_of[p] = want
  end
end

-- Free For All: every seat back on the lobby team it came with, so the next
-- lobby shows the teams the host set up. A roster write needs a running
-- round, so this is done just before the round is ended, not in on_end.
local function restore_teams()
  if teams_on() then
    return
  end
  for p, t in pairs(lobby_team) do
    local slot = game.lobby_slot(p)
    if slot ~= nil and slot.team ~= t then
      game.set_team(p, t)
    end
  end
end

local function sort_teams()
  for p = 0, game.max_tanks() - 1 do
    sort_team(p)
  end
end

-- What a bot holder does with the prize besides running with it. A person
-- holding it is told nothing and plays it his own way.
--
-- The hop: with a hunter close, the holder's man puts the prize down a few
-- squares off, the gun comes back while the man is out and while the prize
-- stands, and the holder shoots his own prize down and drives over it. The
-- pick-up is a new take, with its speed boost. The square is straight ahead,
-- as far as the prize has armour, so the tank is already driving at it; with
-- a hunter very close it goes 45 degrees off the line away from the hunters
-- instead, on the side nearer the way the tank faces. The brain does the
-- shooting and the pick-up itself: the hop part turns on its own pillbox move.
--
-- The fort: a holder whose tank is badly hurt puts the prize down and guards
-- it, with a gun that fills a shell a second. The guard takes it back (the
-- same shoot and pick-up) once no hunter is close, after FORT_SECONDS, when
-- fewer hunters are close than when it went up, or when a hunter has shot
-- it down to LAST_SHOT_ARMOUR, so the last shot is the guard's own.

-- Every live tank in the round that hunts seat p, within `within` squares of
-- it, nearest first: everybody but p in a Free For All, everybody off p's team
-- in a team round. Measured from p's tank, or from square `at` ({ mx, my })
-- when one is given, and then whether p's tank is alive or not.
local function hunters_near(p, within, at)
  local list = {}
  local me = game.tank(p)
  local from = at or me
  if from == nil or (at == nil and me.dead) then
    return list, me
  end
  local side = scoring_team(p)
  for q = 0, game.max_tanks() - 1 do
    if q ~= p and in_round(q) and (side == nil or scoring_team(q) ~= side) then
      local s = game.tank(q)
      if s ~= nil and not s.dead then
        local d = chebyshev(from.mx, from.my, s.mx, s.my)
        if d <= within then
          list[#list + 1] = { q = q, s = s, d = d }
        end
      end
    end
  end
  table.sort(list, function(a, b) return a.d < b.d end)
  return list, me
end

-- The unit step from the hunters' middle to tank `me`, or the tank's own
-- facing when it sits right on that middle.
local function away_from(me, list)
  local cx, cy = 0, 0
  for _, h in ipairs(list) do
    cx, cy = cx + h.s.mx, cy + h.s.my
  end
  cx, cy = cx / #list, cy / #list
  local ax, ay = me.mx - cx, me.my - cy
  local len = math.sqrt(ax * ax + ay * ay)
  if len < 0.5 then
    return facing(me.dir)
  end
  return ax / len, ay / len
end

local function turned(x, y, degrees)
  local a = math.rad(degrees)
  local c, s = math.cos(a), math.sin(a)
  return x * c - y * s, x * s + y * c
end

local function base_on(x, y)
  for n = 1, game.num_bases() do
    local b = game.base(n)
    if b ~= nil and b.x == x and b.y == y then
      return true
    end
  end
  return false
end

-- Ground the prize is put down on: open land with no base and no known mine.
-- A run square is any land. Both are filled on first use, from inside a hook,
-- like carry_ground's table.
local PILL_GROUND, RUN_GROUND

local function ground_tables()
  if PILL_GROUND ~= nil then
    return
  end
  PILL_GROUND, RUN_GROUND = {}, {}
  local T = game.TERRAIN
  for _, name in ipairs({ "grass", "road", "swamp", "crater", "rubble" }) do
    PILL_GROUND[T[name]] = true
  end
  for _, name in ipairs({ "grass", "road", "swamp", "crater", "rubble",
                          "forest", "mine_grass", "mine_road", "mine_swamp",
                          "mine_crater", "mine_rubble", "mine_forest" }) do
    RUN_GROUND[T[name]] = true
  end
end

local function pill_ground(x, y)
  if x < 0 or x > 255 or y < 0 or y > 255 then
    return false
  end
  ground_tables()
  return PILL_GROUND[game.map_tile(x, y)] == true and not base_on(x, y)
end

-- A run square: land, and no deep sea within RUN_SEA_MARGIN of it.
local function run_ground(x, y)
  ground_tables()
  if x < 0 or x > 255 or y < 0 or y > 255 or not RUN_GROUND[game.map_tile(x, y)] then
    return false
  end
  for dy = -RUN_SEA_MARGIN, RUN_SEA_MARGIN do
    for dx = -RUN_SEA_MARGIN, RUN_SEA_MARGIN do
      if game.map_tile(x + dx, y + dy) == game.TERRAIN.deep_sea then
        return false
      end
    end
  end
  return true
end

-- The square a hop or fort puts the prize on, or nil when there is none.
local function hop_square(me, close)
  local fx, fy = facing(me.dir)
  local lines = {}
  if #close > 0 then
    local ax, ay = away_from(me, close)
    local lx, ly = turned(ax, ay, 45)
    local rx, ry = turned(ax, ay, -45)
    if lx * fx + ly * fy >= rx * fx + ry * fy then
      lines = { { lx, ly }, { rx, ry } }
    else
      lines = { { rx, ry }, { lx, ly } }
    end
  else
    lines = { { fx, fy } }
  end
  for _, k in ipairs({ BUILT_ARMOUR, BUILT_ARMOUR + 1, BUILT_ARMOUR - 1 }) do
    for _, l in ipairs(lines) do
      local x, y = whole(me.mx + k * l[1]), whole(me.my + k * l[2])
      if k > 0 and (x ~= me.mx or y ~= me.my) and pill_ground(x, y) then
        return x, y
      end
    end
  end
  return nil
end

local function start_plan(kind, p, me, close)
  local x, y = hop_square(me, close)
  if x == nil then
    return false
  end
  -- The hunters near the square, counted from the square: the fort's own
  -- tests count round the fort once it is up, so "hunters left" compares
  -- two counts with the same middle.
  local near = hunters_near(p, FORT_FREE_WITHIN, { mx = x, my = y })
  -- The plan is in place before the order goes, because the engine asks
  -- can_build about the order before it takes it.
  plan = { kind = kind, x = x, y = y, at = game.tick(), built = false,
           near = #near, trees = me.trees }
  local ok, why = game.builder_order(p, "pill", x, y)
  if not ok then
    plan = nil
    return false
  end
  hop_after = game.tick() + HOP_EVERY * 100
  game.log(string.format("Pillbox Tag: player %d %s at (%d, %d)", p, kind, x, y))
  return true
end

local function take_back(why)
  plan.kind = "take"
  plan.taken_at = game.tick()
  game.log(string.format("Pillbox Tag: player %d takes the fort back (%s)",
                         holder, why))
  tune(holder)
  -- The guard's gun is filled the way a hop's is, to the fort's armour and
  -- HOP_SPARE_SHELLS over, and never below the shells the brain wants
  -- before it starts a pillbox move: a guard who has been firing at the
  -- hunters often has too few left for the move to start at all.
  local t, pb = game.tank(holder), game.pill(pill)
  if t ~= nil and not t.dead and pb ~= nil then
    local want = math.min(game.rule("tank_full_shells"),
                          math.max(pb.armour, SHARED.PILL_REPOSITION_MIN_SHELLS)
                          + HOP_SPARE_SHELLS)
    if t.shells < want then
      game.set_stocks(holder, { shells = want })
    end
  end
  -- A run order from before the fort can still be held, often to a square
  -- the fort itself now blocks, and a held order outbids the pillbox move
  -- until it lapses a minute later. A hold where the tank stands replaces it
  -- and is over a second after (the hop part's own hold time).
  if is_bot(holder) and game.hint(holder, { verb = "hold" }) then
    told[holder], told_for[holder], goto_at[holder] = "hold", nil, nil
    told_at[holder] = elapsed
  end
end

-- A bot holder's run: a square RUN_AWAY_SQUARES off the middle of the hunters
-- within RUN_FROM_WITHIN, on land and clear of deep sea. Straight away first,
-- then 45 and 90 degrees either side.
local function run_from(p, me, list)
  local ax, ay = away_from(me, list)
  for _, deg in ipairs({ 0, 45, -45, 90, -90 }) do
    local rx, ry = turned(ax, ay, deg)
    local x = whole(me.mx + RUN_AWAY_SQUARES * rx)
    local y = whole(me.my + RUN_AWAY_SQUARES * ry)
    if run_ground(x, y) then
      tell(p, goto_hint(x, y, "run"), true)
      run_at = elapsed
      return
    end
  end
end

-- One of the holder's own mines on the square behind him, when a hunter is
-- close behind.
local function mine_behind(p, me)
  if me.mines < 1 or (mine_at ~= nil and elapsed - mine_at < MINE_BEHIND_GAP) then
    return
  end
  local fx, fy = facing(me.dir)
  local behind = false
  for _, h in ipairs(hunters_near(p, MINE_BEHIND_WITHIN)) do
    if (h.s.mx - me.mx) * fx + (h.s.my - me.my) * fy < 0 then
      behind = true
      break
    end
  end
  if not behind then
    return
  end
  local x, y = whole(me.mx - fx), whole(me.my - fy)
  if (x == me.mx and y == me.my) or not run_ground(x, y) or base_on(x, y) then
    return
  end
  if game.place_mine(x, y, p, false) then
    game.add_stocks(p, { mines = -1 })
    mine_at = elapsed
  end
end

-- Whether the holder is strictly top on the board: his team's seconds above
-- every other team's in a team round, his own seconds above everybody
-- else's otherwise. A tie is not top.
local function holder_leads()
  local mine_team = scoring_team(holder)
  local mine = (mine_team ~= nil) and (team_seconds[mine_team] or 0)
               or (seconds[holder] or 0)
  for q = 0, game.max_tanks() - 1 do
    if q ~= holder and in_round(q) then
      local t = scoring_team(q)
      local theirs = nil
      if mine_team ~= nil then
        if t ~= nil and t ~= mine_team then
          theirs = team_seconds[t] or 0
        end
      else
        theirs = seconds[q] or 0
      end
      if theirs ~= nil and theirs >= mine then
        return false
      end
    end
  end
  return true
end

-- Once a second, for a bot holder.
local function bot_holder_second()
  if holder == nil or pill == nil or not is_bot(holder) then
    return
  end
  local pb = game.pill(pill)
  local me = game.tank(holder)
  if pb == nil or me == nil then
    return
  end
  local now = game.tick()

  -- A fort is there to hold off the hunters round it, and nobody scores
  -- while it stands. It is taken back, and the prize grabbed:
  --   * when no hunter is near it, counted round the fort, the same middle
  --     start_plan counts round;
  --   * after FORT_SECONDS, unless the holder is strictly top on the board:
  --     a stalled round helps the leader, so the leader's fort stands for as
  --     long as hunters are near it, and anybody else's for FORT_SECONDS.
  --     Asked every second, so a lead won or lost mid-fort counts at once;
  --   * when fewer hunters are near it than when it was ordered (counted
  --     round the same square then; see start_plan);
  --   * at LAST_SHOT_ARMOUR (in on_tick).
  -- A guard who dies loses the prize (see on_tank_killed), so there is no
  -- dead guard to ask for.
  if plan ~= nil and plan.built and plan.kind == "fort" and standing(pb) then
    local near = hunters_near(holder, FORT_FREE_WITHIN,
                              { mx = pb.x, my = pb.y })
    if #near == 0 then
      take_back("no hunter near")
    elseif now - plan.built_at >= FORT_SECONDS * 100 and not holder_leads() then
      take_back("time")
    elseif #near < plan.near then
      take_back("hunters left")
    end
  end

  -- A holder who dies with the prize out of his tank is no holder after
  -- on_tank_killed, and one who dies carrying it is none after the drop.
  if me.dead then
    return
  end

  if plan ~= nil then
    if not plan.built then
      -- The man never put it down: the order was refused, or he came back.
      if pb.in_tank and not man_out(holder) and
         now - plan.at >= HOP_GIVE_UP * 100 then
        game.log(string.format("Pillbox Tag: player %d %s gave up", holder,
                               plan.kind))
        plan = nil
        tune(holder)
      end
      return
    end
    if standing(pb) then
      -- The guard holds one tree only while the fort wants a repair; nobody
      -- else in the plan holds any, so the brain never patches a prize it is
      -- about to shoot down.
      local trees = (plan.kind == "fort" and pb.armour < FORT_REPAIR_BELOW)
                    and 1 or 0
      if me.trees ~= trees then
        game.set_stocks(holder, { trees = trees })
      end
      -- A hop, or a fort that has been taken back, is the holder's to shoot
      -- down and drive over, which the brain's own pillbox move does once
      -- he is near it. A holder who is not near it (he wandered off, or a
      -- fight pulled him away) is sent back: to the square
      -- BUILT_ARMOUR short of it on his side, where a hop leaves him. So is
      -- one it has stood beside for HOP_PICKUP_SECONDS. A goto holds him
      -- only RUN_PARK_TICKS once he is there, and the move takes over.
      if plan.kind ~= "fort" then
        local d = chebyshev(me.mx, me.my, pb.x, pb.y)
        local since = plan.taken_at or plan.built_at
        if d > DROPPED_WITHIN or now - since >= HOP_PICKUP_SECONDS * 100 then
          local sx, sy = sign(me.mx - pb.x), sign(me.my - pb.y)
          if sx == 0 and sy == 0 then
            sx = 1
          end
          local x, y = standable_near(pb.x + BUILT_ARMOUR * sx,
                                      pb.y + BUILT_ARMOUR * sy)
          if x ~= nil then
            local first = (plan.back_told == nil)
            tell(holder, goto_hint(x, y, string.format("back to %d,%d",
                                                       pb.x, pb.y)), first)
            if first and told_for[holder] ~= nil and
               told_for[holder]:sub(1, 7) == "back to" then
              plan.back_told = now
              game.log(string.format("Pillbox Tag: player %d goes back to "
                                     .. "the %s at (%d, %d) from %d away",
                                     holder, plan.kind, pb.x, pb.y, d))
            end
          end
        end
      end
    end
    return
  end

  if not carrying(holder) then
    return
  end
  local man = game.builder(holder)
  if man ~= nil and man.state ~= "in_tank" then
    return
  end
  local list = hunters_near(holder, math.max(HOP_HUNTER_WITHIN, RUN_FROM_WITHIN))
  local hop_n, close, chasers = 0, {}, {}
  for _, h in ipairs(list) do
    if h.d <= HOP_HUNTER_WITHIN then
      hop_n = hop_n + 1
    end
    if h.d <= RUN_FROM_WITHIN then
      chasers[#chasers + 1] = h
    end
    if h.d <= HOP_SIDE_WITHIN then
      close[#close + 1] = h
    end
  end
  if hop_n > 0 and now >= hop_after then
    local kind = (me.armour <= FORT_ARMOUR_AT) and "fort" or "hop"
    if start_plan(kind, holder, me, close) then
      return
    end
  end
  if #chasers > 0 and (run_at == nil or elapsed - run_at >= RUN_EVERY) then
    run_from(holder, me, chasers)
  end
  mine_behind(holder, me)
end

local finish
local lose_the_prize

-- The second is the unit the whole round is counted in: the holder's point,
-- everybody's shell, and every thirtieth of them a mine.
local function each_second()
  if over then
    return
  end
  elapsed = elapsed + 1

  -- A built prize that is dead with a holder still on it went down some way
  -- on_pill_killed did not hear about. Nobody holds a dead pillbox, except a
  -- bot holder for the HOP_PICKUP_SECONDS after he shot his own down.
  if holder ~= nil and pill ~= nil then
    local pb = game.pill(pill)
    if pb ~= nil and not pb.in_tank and pb.armour == 0 and
       not shot_own_down() then
      lose_the_prize(holder)
      aim_everybody()
    end
  end
  -- A dead holder's man who brought the prize back aboard, not down: it is
  -- put down dead where the man was when the holder died, through
  -- on_pill_placed, not under his new tank, which would hand it straight
  -- back to him.
  if forfeit ~= nil and holder == nil and pill ~= nil then
    local f = forfeit
    local pb, t, man = game.pill(pill), game.tank(f.p), game.builder(f.p)
    if pb ~= nil and pb.in_tank and t ~= nil and not t.dead and
       man ~= nil and man.state == "in_tank" then
      local x, y = standable_near(f.x, f.y)
      if x == nil or game.drop_pill(f.p, pill, x, y) == nil then
        game.drop_pill(f.p, pill)
      end
    end
  end

  -- The point is for carrying it in the tank. Nothing scores while the man
  -- walks it out or while a built prize stands.
  if carrying(holder) then
    seconds[holder] = (seconds[holder] or 0) + 1
    game.score(holder, seconds[holder], SCORE_LABEL)
    local team = scoring_team(holder)
    if team ~= nil then
      team_seconds[team] = (team_seconds[team] or 0) + 1
      game.score({ team = team }, team_seconds[team], SCORE_LABEL)
    end
  end
  board = panel_board()

  -- The nets under the hooks: a standing prize is held at BUILT_ARMOUR, and
  -- every seat is on the team the round puts it on. Neither does anything
  -- when that is already so.
  hold_down_the_prize()
  sort_teams()
  walled.update()

  -- Every second, because a bot that has only just joined may not take its
  -- table on the first try, and a chase goes stale in seconds. Neither says
  -- anything when there is nothing new to say.
  tune_everybody()
  aim_everybody()
  bot_holder_second()

  local mines = (elapsed % MINE_EVERY == 0)
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil and not t.dead then
      -- A holder carrying the prize is the one tank that is not restocked:
      -- an empty gun is what stops it shooting, and handing it a shell would
      -- undo that. A shell it came by some other way is taken off it here.
      -- While his man walks it out, and once it is built, he is restocked
      -- like everybody else.
      if carrying(p) then
        if t.shells > 0 then
          game.set_stocks(p, { shells = 0 })
        end
        if mines then
          game.add_stocks(p, { mines = 1 })
        end
      elseif mines then
        game.add_stocks(p, { shells = 1, mines = 1 })
      else
        game.add_stocks(p, { shells = 1 })
      end
    end
  end

  if elapsed >= ROUND_SECONDS then
    finish()
  else
    game.timer(1, each_second)
  end
end

finish = function()
  if over then
    return
  end
  over = true

  -- The sides: every tank in a Free For All; in a team round every team,
  -- and every tank on no team.
  local sides = {}
  for _, row in ipairs(panel_board()) do
    if not row.indent then
      sides[#sides + 1] = row
    end
  end
  local best = sides[1]
  local line, winner = nil, nil
  if best == nil or best.score == 0 then
    line = "Nobody held the pillbox."
  else
    local tied = 0
    for _, row in ipairs(sides) do
      if row.score == best.score then
        tied = tied + 1
      end
    end
    if tied > 1 then
      line = string.format("A %d-second draw, %d ways.", best.score, tied)
    else
      local who = best.team and team_label(best.team) or name_of(best.slot)
      line = string.format("%s held the pillbox for %d seconds.", who,
                           best.score)
      winner = best.team
    end
  end
  game.message(line)
  restore_teams()
  if winner ~= nil then
    game.end_round(line, winner)
  else
    game.end_round(line)
  end
end

-- A base that has been used goes back on the map neutral, with full armour and
-- no shells or mines. It is only
-- put back on an empty square: a tank parked where it stood would capture it
-- again the instant it landed, which is a pit stop nobody had to drive to.
local restore_base

restore_base = function(x, y, tries)
  if over then
    return
  end
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil and t.mx == x and t.my == y then
      game.timer(1, function() restore_base(x, y, tries) end)
      return
    end
  end
  if game.add_base(x, y, game.NEUTRAL, base_armour, 0, 0) == nil then
    -- A mine laid on the square, or a pillbox put down on it. Wait it out
    -- rather than losing the base for the rest of the round.
    if tries < 60 then
      game.timer(1, function() restore_base(x, y, tries + 1) end)
    else
      game.log(string.format("Pillbox Tag: the base at %d,%d never came back",
                             x, y))
    end
  end
end

-- Before the round. The map's pillboxes come off, one is kept back as the
-- prize, and every base is made neutral with full armour and no shells or
-- mines, so the first tank over it gets a pit stop.
function on_setup()
  read_settings()
  half_armour = math.floor(game.rule("tank_full_armour") / 2)
  base_armour = game.rule("base_full_armour")

  for n = 1, game.num_pills() do
    if game.pill(n) ~= nil then
      if pill == nil then
        pill = n
      else
        game.remove_pill(n)
      end
    end
  end
  if pill ~= nil then
    game.set_pill_armour(pill, 0)
  end

  for n = 1, game.num_bases() do
    if game.base(n) ~= nil then
      game.set_base_owner(n, game.NEUTRAL)
      game.set_base_stock(n, base_armour, 0, 0)
    end
  end
end

function on_start()
  running = true
  for p = 0, game.max_tanks() - 1 do
    seconds[p] = 0
    if game.lobby_slot(p) ~= nil then
      game.score(p, 0, SCORE_LABEL)
    end
  end

  ends_at = game.tick() + ROUND_SECONDS * 100
  -- The engine's own time limit would end the round with a line of its own and
  -- no winner, so it is set a few seconds long and the script gets there first.
  game.set_game_time((ROUND_SECONDS + 5) * 100)

  if pill == nil then
    -- A map with no pillbox on it has nothing to chase. The clock still runs,
    -- so the round ends rather than going on forever.
    game.message("Pillbox Tag needs a map with a pillbox on it.")
    game.log("Pillbox Tag: the map has no pillbox; there is nothing to chase")
  else
    local minutes = math.floor(ROUND_SECONDS / 60)
    game.message(string.format(
      "Pillbox Tag: carry the pillbox. A point a second, %d minute%s.",
      minutes, (minutes == 1) and "" or "s"))
  end

  -- Every seat's lobby team is kept now. In a Free For All every seat then
  -- goes on a team of its own, whatever the lobby put people on; in a team
  -- round the lobby teams stay. Roster writes are refused during the setup
  -- that opens a round, which is why this is here and not in on_setup.
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot ~= nil then
      lobby_team[p] = slot.team
    end
  end
  sort_teams()
  post_team_scores()
  game.log("Pillbox Tag: " .. (teams_on() and "lobby teams" or "free for all"))

  -- Who hears whose voice, by the "Voice chat to everyone" setting: "Yes" is
  -- everybody, "No" is allies only, and "Only in Free For All" is everybody
  -- in a Free For All and teammates in a team round. The engine clears it at
  -- every round start, so it is set here each round. An engine without the
  -- call keeps voice to allies.
  if game.set_voice_everyone then
    local voice = game.setting("voice_everyone")
    game.set_voice_everyone(voice == "Yes" or
                            (voice ~= "No" and not teams_on()))
  end

  -- How close a tank in a wood has to be before it can be seen, in squares.
  hide_at = math.floor(game.rule("tree_hide_distance") / 256)
  tune_everybody()

  board = panel_board()
  game.timer(1, each_second)
  refresh()
end

-- A tank's speed modifier is one whole percentage of the ground's speed cap,
-- the engine drops the fraction from the capped speed, and a tank moves the
-- whole part of its speed each frame. So a share such as 2.7 of a swamp's 3
-- cannot be set; it is made by switching between the two whole caps either
-- side of it (2 and 3). What decides is how far the holder really went:
-- carry_ahead adds up the distance he covered past his share. He runs at the
-- upper cap until he is one saved-up move ahead (see carry_legs), then at the
-- lower cap until he is one saved-up move behind, and so on. Each change is a
-- modifier sent to every client, so the band keeps it to about once a second;
-- over the round he covers his share to within one saved-up move. A tank
-- going slower than its share for its own reasons (turning, stuck on a wall)
-- runs up no more than two saved-up moves of credit, and spends it at the
-- upper cap, a unit a frame over his share at most, not as a burst.
--
-- The ground is read with map_tile, which answers the terrain under a base or
-- a pillbox, so on a base square the share is worked from the ground under
-- it. That is one square, and the band carries over it.
local CARRY_JUMP = 64           -- world units in a frame that are not driving
local carry_by_code             -- terrain code to { rule, pct }
local carry_ahead = 0           -- world units the holder has gone past his share
local carry_low = false         -- whether he is on the lower cap
local carry_x, carry_y          -- where he was last frame, nil to start over

local function carry_ground(t)
  if carry_by_code == nil then
    carry_by_code = {}
    for name, pct in pairs(CARRY_SPEED_PCT) do
      local row = { rule = "speed_" .. name, pct = pct }
      carry_by_code[game.TERRAIN[name]] = row
      local mined = game.TERRAIN["mine_" .. name]
      if mined ~= nil then
        carry_by_code[mined] = row
      end
    end
  end
  if t.boat then
    return carry_by_code[game.TERRAIN.boat]
  end
  return carry_by_code[game.map_tile(t.mx, t.my)]
end

-- The smallest percentage that leaves a cap of `want` out of `cap`, worked
-- the way the engine works it: the whole part of cap * pct / 100. A boost
-- can ask for more than the ground's own cap; the modifier is one byte, so
-- 255 is as far as it goes.
local SPEED_MOD_MAX = 255

local function carry_pct(want, cap)
  return math.min(SPEED_MOD_MAX,
                  math.max(1, math.ceil(want * 100 / cap - 1e-9)))
end

-- The new holder's boost as a factor on their carry speed: BOOST_MULT for
-- BOOST_SECONDS, then falling in a straight line to 1 over
-- BOOST_DECAY_SECONDS, then 1. game.tick() counts 100 a second.
local function boost_now()
  if boost_from == nil then
    return 1
  end
  local s = (game.tick() - boost_from) / 100
  if s < BOOST_SECONDS then
    return BOOST_MULT
  end
  s = s - BOOST_SECONDS
  if s < BOOST_DECAY_SECONDS then
    return BOOST_MULT + (1 - BOOST_MULT) * s / BOOST_DECAY_SECONDS
  end
  boost_from = nil
  return 1
end

local function carry_start()
  carry_ahead = 0
  carry_low = false
  carry_x, carry_y = nil, nil
end

-- The modifier set is replaced whole, and an empty one is the classic tank,
-- which the engine reads back as a speed of 0.
--
-- A boost multiplies the share, and the same band makes the fraction of the
-- boosted share, so it is kept to within a saved-up move too.
local function carry_legs(p, t)
  local boost = boost_now()
  local pct = math.min(SPEED_MOD_MAX, whole(CARRY_SPEED_OTHER * boost))
  local row = carry_ground(t)
  local cap = row and game.rule(row.rule) or 0
  if cap > 0 then
    local share = cap * row.pct / 100 * boost
    -- A tank does not move every frame: it saves its speed up until it has
    -- tank_min_move units to go, then goes them all at once. So one frame's
    -- move is anything from 0 to cap + tank_min_move, and that saved-up move
    -- is the band: the sum has to get that far past 0 before the cap
    -- changes. It is let fall one more band behind, so the frames with no
    -- move are not lost off the bottom, and no further. A boost can put the
    -- share over the ground's cap, and then the boosted cap is the one a
    -- frame's move is measured against.
    local band = math.max(cap, math.ceil(share)) + game.rule("tank_min_move")
    if carry_x ~= nil then
      local dx, dy = t.wx - carry_x, t.wy - carry_y
      local moved = math.sqrt(dx * dx + dy * dy)
      -- A bigger jump is a respawn or a teleport, not driving.
      if moved <= CARRY_JUMP then
        carry_ahead = math.max(carry_ahead + moved - share, -2 * band)
      end
    end
    if carry_ahead >= band then
      carry_low = true
    elseif carry_ahead <= -band then
      carry_low = false
    end
    local want = carry_low and math.floor(share) or math.ceil(share)
    pct = carry_pct(want, cap)
  end
  carry_x, carry_y = t.wx, t.wy
  local now = (t.mods.speed == 0) and 100 or t.mods.speed
  if now ~= pct then
    game.set_modifiers(p, (pct == 100) and {} or { speed = pct })
  end
end

-- Everything the holder gives up, and gets back. The modifier set is replaced
-- whole rather than merged, so the empty table is the classic tank.
--
-- Every take of the prize gives the speed boost that carry_legs reads
-- through boost_now, even a player picking up the prize they built. The
-- INVULN_SECONDS that damage_scale spares a holder for come only when the
-- prize changes hands: a seat other than last_holder takes it. A player who
-- takes back their own prize keeps any cover they still have and gets no new
-- cover. A holder the prize is taken straight from loses the prize, and with
-- it their own boost and cover, first.
-- A bot holder's hop or fort is over: the trees the plan took off him go
-- back, and the next one waits HOP_EVERY.
local function end_plan()
  if plan ~= nil and holder ~= nil then
    local t = game.tank(holder)
    if t ~= nil and not t.dead and plan.trees ~= nil and t.trees ~= plan.trees then
      game.set_stocks(holder, { trees = plan.trees })
    end
  end
  plan = nil
  hop_after = game.tick() + HOP_EVERY * 100
end

local function take_the_prize(p)
  if holder ~= nil and holder ~= p then
    lose_the_prize(holder)
  end
  end_plan()
  forfeit = nil
  local new_hand = (last_holder ~= p)
  holder = p
  last_holder = p
  seconds[p] = seconds[p] or 0
  boost_from = game.tick()
  if new_hand then
    if INVULN_SECONDS > 0 then
      invuln_seat = p
      invuln_to   = boost_from + INVULN_SECONDS * 100
    else
      invuln_seat = nil
    end
  end
  -- The teams do not change with the prize: in a Free For All the holder is
  -- already on a team of his own, and in a team round he stays on his.
  sort_team(p)
  carry_start()
  local t = game.tank(p)
  if t ~= nil then
    carry_legs(p, t)
  end
  -- The gun is emptied in the same frame as the pick-up. A prize that went
  -- up some other way (see on_pill_placed) leaves the gun alone.
  if carrying(p) then
    game.set_stocks(p, { shells = 0 })
  end
  game.score(p, seconds[p], SCORE_LABEL)
  -- A bot takes the holder's part at once, and everybody else is turned on it.
  stop_the_fight(p)
  tune_everybody()
  aim_everybody()
end

lose_the_prize = function(p)
  end_plan()
  holder = nil
  boost_from = nil
  invuln_seat = nil
  if p ~= nil then
    game.set_modifiers(p, {})
    tune(p)
    sort_team(p)
  end
end

function on_pill_picked_up(n, p, scripted)
  if over or n ~= pill then
    return
  end
  take_the_prize(p)
end

-- Every way the pillbox reaches the ground comes through here: built, dropped
-- by a tank that sank or was destroyed, or let go by a player on the way out.
-- A built one arrives at the sim's cap, the classic 15, and is set down to
-- BUILT_ARMOUR in the same frame. It stays the holder's, though it scores
-- nothing while it stands. Every other route arrives dead, and a dead prize
-- has no holder.
--
-- The builder is whoever was carrying it, which is the holder. The take is
-- there for a pillbox that went up any other way, so that whoever owns a
-- standing prize is always the holder. A seat that is on its way
-- out gets nothing, and its prize is put back to nothing.
function on_pill_placed(n, p, armour, scripted)
  if over or n ~= pill then
    return
  end
  -- The prize is on the map again, so no man has it in his hands. One a dead
  -- holder's man put down goes the way of a drop below.
  local forfeited = (forfeit ~= nil and p == forfeit.p)
  forfeit = nil
  if armour > 0 and p ~= nil and in_round(p) and not forfeited then
    hold_down_the_prize()
    if p ~= holder then
      if holder ~= nil then
        lose_the_prize(holder)
      end
      take_the_prize(p)
    end
    -- A bot holder's hop or fort is down. A hop fills his gun to the prize's
    -- armour and HOP_SPARE_SHELLS over, so he can shoot it down at once.
    if plan ~= nil and p == holder and not plan.built then
      plan.built = true
      plan.built_at = game.tick()
      if plan.kind == "hop" then
        local t = game.tank(p)
        local want = math.min(game.rule("tank_full_shells"),
                              BUILT_ARMOUR + HOP_SPARE_SHELLS)
        if t ~= nil and t.shells < want then
          game.set_stocks(p, { shells = want })
        end
      end
      tune(p)
    end
    aim_everybody()
    return
  end
  lose_the_prize(p)
  if armour > 0 then
    game.set_pill_armour(n, 0)
  end
  rescue_from_the_sea(n)
  -- Everybody is sent after it where it landed, or where it was walked to.
  aim_everybody()
end

-- A built prize shot down to nothing is lying on the ground like a dropped
-- one: nobody holds it, the old holder gets his speed back (his gun is
-- already being filled while it stands), and the first tank to drive over it
-- is the new holder.
--
-- A bot holder shooting down his own hop or fort is not a loss: it is the
-- first half of taking it back, and he keeps it for HOP_PICKUP_SECONDS while
-- he drives over to it. Anybody who gets there first takes it.
function on_pill_killed(n, by, scripted)
  if over or n ~= pill then
    return
  end
  if plan ~= nil and plan.built and holder ~= nil and by == holder then
    plan.dead_at = game.tick()
    aim_everybody()
    return
  end
  lose_the_prize(holder)
  aim_everybody()
end

-- A holder who dies while the prize is out of his tank loses it, person or
-- bot: a prize standing on the map goes to nothing where it is, the way a
-- leaving holder's does, one he has shot down is dead already, and one in
-- his man's hands is put down dead when the man gets there (see forfeit in
-- on_pill_placed). Nobody holds it after, so nobody scores, and every hunter
-- is sent at it; the dead player comes back as a hunter. A prize in his tank
-- is left to the drop the death makes, which on_pill_placed already handles.
--
-- His orders go with him: a brain forgets its order when its tank dies, so
-- what this script last told him is forgotten too, and the next order he
-- gets is a fresh one.
function on_tank_killed(victim, killer, cause, scripted)
  if over or pill == nil or victim == nil or victim ~= holder then
    return
  end
  local pb = game.pill(pill)
  if pb == nil then
    return
  end
  local walking = man_out(victim)
  if pb.in_tank and not walking then
    return
  end
  if walking then
    -- A dead tank has no square to read, so the man's is kept.
    local man = game.builder(victim)
    forfeit = { p = victim, x = man.mx, y = man.my }
  end
  lose_the_prize(victim)
  told[victim], told_for[victim], told_at[victim], goto_at[victim] =
    nil, nil, nil, nil
  if standing(pb) then
    game.set_pill_armour(pill, 0)
  end
  game.log(string.format("Pillbox Tag: player %d died with the prize out "
                         .. "(%s); it is anybody's", victim,
                         walking and "his man has it" or
                         string.format("at %d,%d", pb.x, pb.y)))
  aim_everybody()
end

-- A standing prize owned by anybody but the holder would be a gun with no
-- holder behind it. Nothing in the ordinary game hands a live pillbox over, so this
-- is the net under the hooks above: it goes to nothing, and it is anybody's.
-- A pick-up is not this — that one is in the tank, not standing.
function on_pill_captured(n, old, new, scripted)
  if over or n ~= pill then
    return
  end
  if standing(game.pill(n)) and new ~= holder then
    game.set_pill_armour(n, 0)
    lose_the_prize(holder)
    aim_everybody()
  end
end

-- A builder sent to a pillbox repairs it. The holder may patch his own prize
-- up, and a repair that goes past BUILT_ARMOUR is set back down to it. A
-- repair of a prize with no holder would bring a dead one back to life as a
-- gun nobody holds, so it goes to nothing again as soon as the work is done.
function on_built(p, action, x, y, scripted)
  -- A pillbox repair arrives as "repair" (a build order says "pill").
  if over or pill == nil or action ~= "repair" then
    return
  end
  local pb = game.pill(pill)
  if not standing(pb) or pb.x ~= x or pb.y ~= y then
    return
  end
  if holder == nil then
    game.set_pill_armour(pill, 0)
  else
    hold_down_the_prize()
  end
end

-- And the repair is refused before it starts where it can be. A pill order
-- on the square the prize is lying on is a repair of it. n is the pillbox the
-- tank would put down, not the one on the square, so the square is what is
-- compared. Only the holder, and only on a prize still standing and under
-- BUILT_ARMOUR: a dead one is picked up, not repaired, and the engine would
-- spend his trees filling a full one to 15 only for on_built to take it back.
--
-- A bot holder puts the prize down only where this script sent his man. A
-- brain can still decide on a build of its own in the second before its new
-- part reaches it (a tank fight with a pillbox aboard is one), and that one
-- is refused here. A person holding the prize builds where he likes.
function can_build(p, action, x, y, n)
  if over or pill == nil or action ~= "pill" then
    return nil
  end
  local pb = game.pill(pill)
  if p == holder and is_bot(p) and pb ~= nil and pb.in_tank then
    if plan ~= nil and not plan.built and plan.x == x and plan.y == y then
      return nil
    end
    return false
  end
  if pb ~= nil and not pb.in_tank and pb.x == x and pb.y == y then
    if p == holder and standing(pb) and pb.armour < BUILT_ARMOUR then
      return nil
    end
    return false
  end
  return nil
end

-- A base is a pit stop: half a tank of armour and 3 mines, and then
-- the base is off the map for half a minute. add_stocks holds the mines at
-- the tank's cap.
function on_base_captured(n, old, new, scripted)
  if over or scripted or new == game.NEUTRAL then
    return
  end
  local b = game.base(n)
  if b == nil then
    return
  end
  local x, y = b.x, b.y
  game.add_stocks(new, { armour = half_armour, mines = 3 })
  -- The empty gun: a base is the one thing that can hand a carrying holder
  -- a shell. The mines stay: a carrying holder gets those like everybody.
  if carrying(new) then
    game.set_stocks(new, { shells = 0 })
  end
  game.remove_base(n)
  game.timer(BASE_DOWN_SECONDS, function() restore_base(x, y, 0) end)
end

-- A player who arrives in the last minute is given a score row and everybody
-- else's, because the panels are replayed to a joiner and the scores are not.
function on_player_join(p, scripted)
  if not running or over then
    return
  end
  -- A new player in a seat starts from nothing, not from the last one's time,
  -- and is sent a whole panel of their own. A seat on_start already kept is
  -- not new: a bot fielded just after the start arrives here too, and by
  -- then a Free For All has moved it off its lobby team.
  if lobby_team[p] == nil then
    seconds[p] = 0
    team_of[p] = nil
  end
  drawn[p]   = nil
  sort_team(p)
  for q = 0, game.max_tanks() - 1 do
    if seconds[q] ~= nil and game.lobby_slot(q) ~= nil then
      game.score(q, seconds[q], SCORE_LABEL)
    end
  end
  -- A team that was not in the round at the start gets its 0 now.
  post_team_scores(lobby_team_of(p))
  tune(p)
end

-- A seat that leaves takes nothing with it into the next seat's play: what
-- it was told and what it was tuned to are forgotten, so a bot that takes
-- the seat starts from the brain's own numbers.
--
-- A holder who leaves with his prize built leaves a gun nobody scores for, so
-- it is put back to nothing, and anybody can drive over it.
function on_player_leave(p, scripted)
  if last_holder == p then
    last_holder = nil
  end
  -- The deep sea clock and its warning go with the seat.
  if wet_seat == p then
    wet_seat = nil
  end
  if warned == p then
    warned = nil
  end
  if forfeit ~= nil and forfeit.p == p then
    forfeit = nil
  end
  if holder == p then
    holder = nil
    plan = nil
    boost_from = nil
    invuln_seat = nil
    if pill ~= nil and standing(game.pill(pill)) then
      game.set_pill_armour(pill, 0)
      aim_everybody()
    end
  end
  drawn[p]    = nil
  told_at[p]  = nil
  told[p]     = nil
  told_for[p] = nil
  goto_at[p] = nil
  tuned[p]   = nil
  peace_of[p] = nil
  touched[p] = nil
  have[p]    = nil
  team_of[p] = nil
  lobby_team[p] = nil
end

-- The teams are the round's, not the players'. A seat that changes team
-- itself is put back: on its own team in a Free For All, on its lobby team
-- in a team round. A move this script made arrives here too, a tick later,
-- and is left alone.
function on_team_changed(p, team, scripted)
  if scripted or not running or over then
    return
  end
  sort_team(p)
end

-- Owning every base is somebody else's way to win a round, and sixteen pit
-- stops changing hands is not news.
function allow_base_win()
  return false
end

-- In a Free For All every player chases the prize for themselves, so
-- players cannot make alliances. In a team round only lobby teammates ally.
function can_ally(p, q)
  if not teams_on() then
    return false
  end
  local tp, tq = lobby_team_of(p), lobby_team_of(q)
  return tp > 0 and tp == tq
end

function announce(kind, subject, actor)
  if kind == "base_captured" then
    return false
  end
  return nil
end

-- Every frame: the distance is counted frame by frame (see carry_legs), and
-- the holder crosses from one ground to the next in a fraction of a second. It is
-- one tank and one square, and the modifier is only set when it changes.
--
-- It is also where the new holder's cover ends. damage_scale is a policy, the
-- engine asks it in the middle of a hit, and it asks the game nothing back:
-- it goes by invuln_seat alone, and this puts that back to nil on the first
-- frame past INVULN_SECONDS.
--
-- The man walking the prize out is watched here too, frame by frame: the
-- moment he goes the gun starts to fill and the hunters are turned on him,
-- and the moment he is back with it the gun is emptied again. And a fort a
-- hunter has shot down to LAST_SHOT_ARMOUR is taken back at once.
function on_tick(tick)
  if invuln_seat ~= nil and game.tick() >= invuln_to then
    invuln_seat = nil
  end
  local out = man_out(holder)
  if out ~= man_was_out and not over then
    man_was_out = out
    if not out and carrying(holder) then
      game.set_stocks(holder, { shells = 0 })
    end
    tune_everybody()
    aim_everybody()
  end
  if over or holder == nil then
    return
  end
  if plan ~= nil and plan.built and plan.kind == "fort" then
    local pb = game.pill(pill)
    if standing(pb) and pb.armour <= LAST_SHOT_ARMOUR then
      take_back("last shot")
    end
  end
  local t = game.tank(holder)
  if t == nil or t.dead then
    carry_start()
  else
    carry_legs(holder, t)
  end
end

-- Full shells, no mines, and enough trees to put the pillbox down — though
-- the rules below make that free, so the trees are only there for the rest of
-- what a builder does. A holder who is carrying the prize comes back with
-- the empty gun; one whose prize stands comes back armed like anybody else.
function spawn_loadout(p)
  return {
    shells = carrying(p) and 0 or game.rule("tank_full_shells"),
    mines  = 0,
    armour = game.rule("tank_full_armour"),
    trees  = game.rule("tank_full_trees"),
  }
end

-- A new holder takes no damage for INVULN_SECONDS. Every shell and mine that
-- hits a tank is priced through here, a pillbox's shell as well; nil is the
-- ordinary amount. It does not keep them afloat: a hit still knocks a tank
-- off its boat, and drowning is not damage.
function damage_scale(attacker, victim, cause)
  if invuln_seat ~= nil and victim == invuln_seat then
    return 0
  end
  return nil
end

function on_end()
  over = true
  game.log(string.format("Pillbox Tag ended after %d seconds", elapsed))
end

scenario = {
  name        = "Pillbox Tag",
  -- The round length here is the default; the lobby can set another.
  -- The engine keeps 255 bytes of this; at 30 minutes it is 254.
  description = string.format("One dead pillbox, %d minutes by default. " ..
                "Carrying it scores a point a second, slows you, empties " ..
                "your gun; a new holder gets a head start. Built or on " ..
                "foot it scores nothing, your gun refills, three shells " ..
                "kill it. Die while it is out and it is anyone's.",
                math.floor(ROUND_SECONDS / 60)),
  api         = 1,
  kind        = "scenario",
  game        = "open",

  -- Nothing here names a square, a pill number or a base number, so this one
  -- plays over whatever map the host has committed.
  bound       = false,

  -- What the host sets in the lobby's details dialog; read_settings reads
  -- them with game.setting. The defaults are the numbers under "Tweak these"
  -- at the top of the file, which is the round as it always played, with the
  -- head start added.
  settings = {
    { id = "round_minutes", label = "Round length (minutes)", type = "int",
      min = 1, max = 30, step = 1, default = math.floor(ROUND_SECONDS / 60) },
    { id = "speed_penalty", label = "Holder speed penalty (%)",
      type = "int", min = 0, max = 60, step = 5,
      default = SPEED_PENALTY_PCT },
    { id = "boost_pct", label = "New holder boost (% of carry speed)",
      type = "int", min = 100, max = 250, step = 10,
      default = whole(BOOST_MULT * 100) },
    { id = "boost_seconds", label = "Boost time (seconds)", type = "int",
      min = 0, max = 10, step = 1, default = BOOST_SECONDS },
    { id = "boost_decay_seconds", label = "Boost fade time (seconds)",
      type = "int", min = 0, max = 10, step = 1,
      default = BOOST_DECAY_SECONDS },
    { id = "invuln_seconds", label = "New holder no-damage time (seconds)",
      type = "int", min = 0, max = 10, step = 1, default = INVULN_SECONDS },
    { id = "deep_water_seconds", label = "Holder deep water time (seconds)",
      type = "int", min = 3, max = 60, step = 1,
      default = DEEP_WATER_SECONDS },
    { id = "teams", label = "Teams", type = "choice",
      choices = { FREE_FOR_ALL, LOBBY_TEAMS }, default = FREE_FOR_ALL },
    { id = "voice_everyone", label = "Voice chat to everyone",
      type = "choice",
      choices = { "Only in " .. FREE_FOR_ALL, "Yes", "No" },
      default = "Only in " .. FREE_FOR_ALL },
  },

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this scenario implements:".
  callbacks = {
    on_setup = "One pillbox is the prize, the rest go; bases start " ..
               "neutral.",
    on_start = "Clock, compass, sea timer; Free For All: own teams; " ..
               "voice to all by setting.",
    on_end = "Logs how long the round ran.",
    on_tick = "Holder speed by terrain, plus boost; man out: gun fills.",
    on_player_join = "A joiner hunts, on 0 points.",
    on_player_leave = "A leaving holder's built prize dies.",
    on_base_captured = "A base: half armour, 3 mines, then gone 30 s.",
    on_pill_placed = "Built: 3 armour, no score. Dropped: dead.",
    on_pill_picked_up = "Holder: 1 point/s (and team), slow, unarmed.",
    on_pill_killed = "A shot-down prize is anybody's (a bot's own hop aside).",
    on_tank_killed = "Holder dies with the prize out of his tank: it is " ..
                     "dead, free for anyone to take.",
    on_pill_captured = "Only the holder may own a built prize.",
    on_built = "A repair stops at 3 armour.",
    can_build = "Only the holder may repair it; bots build where told.",
    on_team_changed = "Teams are fixed for the round.",
    allow_base_win = "Holding every base does not win.",
    can_ally = "No alliances in Free For All; teammates in a team round.",
    announce = "Base captures are not announced.",
    spawn_loadout = "Full shells, no mines; carriers get none.",
    damage_scale = "New holder takes no damage at first.",
  },

  rules = {
    -- Putting the pillbox down is free, so the holder can drop it anywhere
    -- the ground will take it without having gone farming first.
    lgm_cost_pill_new = 0,

    -- A dead builder comes back thirty times as fast. The classic 3 world
    -- units a frame is most of three minutes across a map, and a round is
    -- only ten minutes by default.
    --
    -- The tolerance has to go up with the speed. The helicopter steps
    -- straight at the tank and calls it a landing once both axes are inside
    -- the tolerance, so a step bigger than that window steps over it and the
    -- man circles the tank without ever touching down: at 90 against the
    -- classic 16 he fails to land on more than half the flights. A tolerance
    -- at or above the step lands every one of them. It costs the outbound
    -- walk a third of a square of accuracy and nothing else, because the job
    -- is done on the square that was ordered rather than the one he stopped
    -- on. The walk back to the tank has a tolerance of its own and is
    -- untouched.
    lgm_helicopter_speed = 90,
    lgm_arrive_tolerance = 96,
  },
}
