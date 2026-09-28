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
-- Killing the holder does not move a built prize; the others shoot it down to
-- nothing and drive over it, which makes the one who picks it up the new
-- holder. A tank only picks up a dead pillbox, so the holder cannot take
-- his own back up while it stands; it has to be shot down first (his own
-- shells do that too), and then it is a race to drive over it. Picking it up
-- empties the gun again in the same frame. Nobody but the holder may repair it,
-- and a dead one lying on the ground is repaired by nobody.
--
-- Everybody starts on one team. The holder is moved to a team of his own
-- the moment he takes the prize and back when he loses it, so every other
-- tank is his enemy and an ally of the rest: the bots go after him and
-- leave each other alone, and a built prize fires at everybody but him.
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
-- a tank of armour, and the base then goes off the map for half a minute
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
local HUNTERS           = 1     -- the team everybody but the holder is on
local HOLDER_TEAM       = 2     -- the holder's team, on his own

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
local touched     = {}          -- seat -> every knob and flag word it has been handed
local hide_at     = 3           -- squares off a tank in forest stops being seen
local board       = {}          -- the leaderboard, rebuilt once a second
local drawn       = {}          -- seat -> what its panel last showed, as a short key
local team_of     = {}          -- seat -> the team this script last put it on
local boost_from  = nil         -- the game.tick() the holder took the prize on,
                                -- or nil when there is no boost to run
local invuln_seat = nil         -- the seat damage_scale spares, or nil
local invuln_to   = 0           -- the game.tick() that seat is spared until
local last_holder = nil         -- the seat that took the prize last; kept
                                -- when the prize drops, so a player who takes
                                -- back their own prize is not a new hand

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
  work_out_shares()
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

-- Whether seat p is the holder with the prize in hand: in the tank, or in
-- his man's hands on the way to a build. That is when the gun stays empty.
-- While a built prize stands, the holder is restocked like everybody else.
local function carrying(p)
  return p ~= nil and p == holder and not standing(game.pill(pill))
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
    return pb.x + 0.5, pb.y + 0.5, "GROUND", "green", nil
  end
  if holder == nil then
    return nil
  end
  local man = game.builder(holder)
  if man ~= nil and man.state ~= "in_tank" and man.job == "pill" then
    return man.wx / 256, man.wy / 256, "ON FOOT", "cyan", holder
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

  local key = string.format("%s %s %s %s %s %s", tostring(carrier),
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
    local shade = "grey"
    if row.slot == carrier then
      shade = "yellow"
    elseif row.slot == p then
      shade = "white"
    end
    n = n + 1
    list[n] = { "name", 4, y, shade, "small", "left", row.slot }
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
    parts[i] = row.slot .. ":" .. row.score
  end
  return table.concat(parts, ",")
end

-- Five times a second, one panel each. Every player's is different, and the
-- panel's own rule is one update per audience per tick, so sixteen of them in
-- the one call is fine. The scores only change once a second, so the board
-- each_second built is the one used here, and a seat whose compass looks the
-- same as last time is not sent it again.
local function refresh()
  if over then
    return
  end
  local tx, ty, what, colour, carrier = prize()
  local rows = board
  local rows_key = board_key(rows)
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil then
      compass(p, t, tx, ty, what, colour, carrier, rows, rows_key)
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
-- The table replaces the last one whole, but the numbers it set do not go
-- back on their own: GoalHunter writes every cfg=NAME=VALUE into its own
-- constants, and a constant stays where it was put until something writes it
-- again. So a bot that changes part is handed its new numbers AND the
-- ordinary value of every number its old part changed. DEFAULTS is that
-- ordinary value, read off the brain's constants.lua, for every knob either
-- part touches.
--
-- The ordinary value is the Hard one. A Medium or Easy bot plays with
-- TANK_COMBAT_BASE_COST moved by its level, and a script cannot see a seat's
-- level, so once a hunter has set it and been the holder, it comes back as
-- the Hard number. A hunter sets it lower than any level does, so the only
-- difference is how a former holder bids for a fight it has no shells for.
local DEFAULTS = {
  TANK_COMBAT_ENABLED     = true,
  TANK_COMBAT_BASE_COST   = 30,
  TAKE_COVER_W_ENEMY      = 20,
  FLEE_DANGER_WEIGHT      = 80,
  STRATEGIC_PLACE_ENABLED = true,
  EMERGENCY_DROP_ENABLED  = true,
  PILL_REPOSITION_ENABLED = true,
  BUILDER_POOL_ENABLED    = true,
}

-- The flag words a brain keeps until it is told the opposite. GoalHunter puts
-- noblitz, noclaimdead and suicider back itself on every new table; ammoless
-- it leaves standing, and "normal" is the word that takes it off.
local FLAG_UNDO = { ammoless = "normal" }

-- The two parts. Neither has a reason to move a pillbox, and neither should
-- have its builder spend the round on walls and guns. The only live pillbox a
-- hunter ever meets is a built prize, and its own pillbox fight is left on for
-- that. A hunter bids hard for a tank fight, because the holder is a tank.
-- The holder does not fight at all (it has no shells, and a tank fight is
-- also when a brain puts a carried pillbox down as a guard), weighs danger
-- twice as heavily when it runs, looks for cover sooner, never goes looking
-- for a base to refuel at (nothing will fill its gun), and never puts the
-- prize down on purpose.
--
-- The hunter is told not to put a pillbox down as well. A bot that picks the
-- prize up is still a hunter until its new table reaches the brain, most of
-- a second later, and a brain that is outnumbered with a pillbox aboard drops
-- it beside itself on the very next think (the "emergency drop"). Measured
-- on a headless run before this line: holders built the prize back into a
-- gun within half a second of picking it up.
local ROLES = {
  hunter = {
    flags = { "noblitz", "nosuicider" },
    cfg = {
      TANK_COMBAT_BASE_COST   = 10,
      STRATEGIC_PLACE_ENABLED = false,
      EMERGENCY_DROP_ENABLED  = false,
      PILL_REPOSITION_ENABLED = false,
      BUILDER_POOL_ENABLED    = false,
    },
  },
  holder = {
    flags = { "noblitz", "nosuicider", "ammoless" },
    cfg = {
      TANK_COMBAT_ENABLED     = false,
      TAKE_COVER_W_ENEMY      = 60,
      FLEE_DANGER_WEIGHT      = 160,
      STRATEGIC_PLACE_ENABLED = false,
      EMERGENCY_DROP_ENABLED  = false,
      PILL_REPOSITION_ENABLED = false,
      BUILDER_POOL_ENABLED    = false,
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

local function is_bot(p)
  local slot = game.lobby_slot(p)
  return slot ~= nil and slot.bot
end

-- The whole table for one seat in one part, and the same table as one line
-- of text so two of them can be compared. Every knob this seat has been
-- handed before and this part does not set goes back to DEFAULTS.
local function init_table(role_name, p)
  local role = ROLES[role_name]
  local had  = touched[p] or {}
  local want = {}
  for k, v in pairs(role.cfg) do
    want[k] = v
  end
  for k in pairs(had) do
    if want[k] == nil and DEFAULTS[k] ~= nil then
      want[k] = DEFAULTS[k]
    end
  end

  local t, pairs_n = {}, 0
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
  for k in pairs(want) do
    names[#names + 1] = k
  end
  table.sort(names)
  local chunks, value = {}, nil
  for _, k in ipairs(names) do
    local item = k .. "=" .. cfg_text(want[k])
    -- Every value after the first carries the six bytes of "0;cfg=" as well.
    local room = (#chunks == 0) and INIT_VALUE_MAX or INIT_VALUE_MAX - 6
    if value ~= nil and #value + 5 + #item <= room then
      value = value .. ";cfg=" .. item
    else
      if value ~= nil then
        chunks[#chunks + 1] = value
      end
      value = item
    end
  end
  if value ~= nil then
    chunks[#chunks + 1] = value
  end
  for i, v in ipairs(chunks) do
    if i == 1 then
      t.cfg = v
    else
      t["cfg" .. i] = "0;cfg=" .. v
    end
  end
  pairs_n = pairs_n + #chunks
  if pairs_n > INIT_PAIRS_MAX then
    return nil
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
  return t, table.concat(line, " "), want
end

-- Hands a bot the table for the part it has, unless it already has that
-- table: every table a brain takes it says a line about, so the same one
-- twice is noise. A refusal is left for the next second to try again: a bot
-- that has only just joined may not have a brain to take it yet.
local function tune(p)
  if not is_bot(p) then
    return
  end
  local name = (p == holder) and "holder" or "hunter"
  local t, line, want = init_table(name, p)
  if t == nil or tuned[p] == line then
    return
  end
  if game.bot_init(p, t) then
    tuned[p] = line
    local had = touched[p] or {}
    for k in pairs(want) do
      had[k] = true
    end
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

local function aim_one(p)
  if p == holder or pill == nil or not is_bot(p) or not in_round(p) then
    return
  end
  local t = game.tank(p)
  if t == nil or t.dead then
    return
  end
  local from = { x = t.mx, y = t.my }
  local pb = game.pill(pill)
  if pb == nil then
    return
  end
  -- A built prize gets no order. The only order a script can give a bot
  -- about a pillbox is defend, and a goto is a hold that shuts off every
  -- other goal, the brain's pillbox fight with them. Left alone, the brain
  -- sees a hostile pillbox and goes after it itself.
  if standing(pb) then
    return
  end
  if not pb.in_tank then
    tell(p, ground_order(p, pb.x, pb.y, from))
    return
  end
  if holder == nil then
    return
  end
  local man = game.builder(holder)
  if man ~= nil and man.state ~= "in_tank" and man.job == "pill" then
    tell(p, goto_order(p, man.mx, man.my, from, "man"))
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

-- The team seat p belongs on: the holder on his own, everybody else together.
-- A seat is only moved when that differs from where this script last put it,
-- because each move sends every client the whole alliance table again.
local function sort_team(p)
  if over or not in_round(p) then
    return
  end
  local want = (p == holder) and HOLDER_TEAM or HUNTERS
  if team_of[p] ~= want and game.set_team(p, want) then
    team_of[p] = want
  end
end

local function sort_teams()
  for p = 0, game.max_tanks() - 1 do
    sort_team(p)
  end
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
  -- on_pill_killed did not hear about. Nobody holds a dead pillbox.
  if holder ~= nil and pill ~= nil then
    local pb = game.pill(pill)
    if pb ~= nil and not pb.in_tank and pb.armour == 0 then
      lose_the_prize(holder)
      aim_everybody()
    end
  end

  -- The point is for carrying it, in the tank or in the man's hands on the
  -- way to a build. A built prize scores nothing while it stands.
  if holder ~= nil and not standing(game.pill(pill)) then
    seconds[holder] = (seconds[holder] or 0) + 1
    game.score(holder, seconds[holder], SCORE_LABEL)
  end
  board = leaderboard()

  -- The nets under the hooks: a standing prize is held at BUILT_ARMOUR, and
  -- every seat is on the team its part puts it on. Neither does anything
  -- when that is already so.
  hold_down_the_prize()
  sort_teams()

  -- Every second, because a bot that has only just joined may not take its
  -- table on the first try, and a chase goes stale in seconds. Neither says
  -- anything when there is nothing new to say.
  tune_everybody()
  aim_everybody()

  local mines = (elapsed % MINE_EVERY == 0)
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil and not t.dead then
      -- A holder carrying the prize is the one tank that is not restocked:
      -- an empty gun is what stops it shooting, and handing it a shell would
      -- undo that. A shell it came by some other way is taken off it here.
      -- Once the prize is built he is restocked like everybody else.
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

  local rows = leaderboard()
  local best = rows[1]
  local line
  if best == nil or best.score == 0 then
    line = "Nobody held the pillbox."
  else
    -- A dead tank has no table, so the lobby's name for the seat is next.
    local t = game.tank(best.slot)
    local slot = game.lobby_slot(best.slot)
    local who = (t ~= nil and t.name ~= "" and t.name) or
                (slot ~= nil and slot.name ~= "" and slot.name) or "Somebody"
    local tied = 0
    for _, row in ipairs(rows) do
      if row.score == best.score then
        tied = tied + 1
      end
    end
    if tied > 1 then
      line = string.format("A %d-second draw, %d ways.", best.score, tied)
    else
      line = string.format("%s held the pillbox for %d seconds.", who,
                           best.score)
    end
  end
  game.message(line)
  game.end_round(line)
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

  -- Whatever the lobby put people on, they start together: nobody holds the
  -- prize yet, so everybody is a hunter. Roster writes are refused during the
  -- setup that opens a round, which is why this is here and not in on_setup.
  sort_teams()

  -- How close a tank in a wood has to be before it can be seen, in squares.
  hide_at = math.floor(game.rule("tree_hide_distance") / 256)
  tune_everybody()

  board = leaderboard()
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
local function take_the_prize(p)
  if holder ~= nil and holder ~= p then
    lose_the_prize(holder)
  end
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
  -- On a team of their own before anybody is told to go after them, so a bot
  -- handed the order to attack them is attacking an enemy.
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
  tune(p)
  aim_everybody()
end

lose_the_prize = function(p)
  holder = nil
  boost_from = nil
  invuln_seat = nil
  if p ~= nil then
    game.set_modifiers(p, {})
    tune(p)
    -- Back with everybody else. A seat on its way out is left alone.
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
  if armour > 0 and p ~= nil and in_round(p) then
    hold_down_the_prize()
    if p ~= holder then
      if holder ~= nil then
        lose_the_prize(holder)
      end
      take_the_prize(p)
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
function on_pill_killed(n, by, scripted)
  if over or n ~= pill then
    return
  end
  lose_the_prize(holder)
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
function can_build(p, action, x, y, n)
  if over or pill == nil or action ~= "pill" then
    return nil
  end
  local pb = game.pill(pill)
  if pb ~= nil and not pb.in_tank and pb.x == x and pb.y == y then
    if p == holder and standing(pb) and pb.armour < BUILT_ARMOUR then
      return nil
    end
    return false
  end
  return nil
end

-- A base is a pit stop: half a tank of armour, and then the base is off the
-- map for half a minute.
function on_base_captured(n, old, new, scripted)
  if over or scripted or new == game.NEUTRAL then
    return
  end
  local b = game.base(n)
  if b == nil then
    return
  end
  local x, y = b.x, b.y
  game.add_stocks(new, { armour = half_armour })
  -- The empty gun: a base is the one thing that can hand a carrying holder
  -- a shell.
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
  -- and is sent a whole panel of their own.
  seconds[p] = 0
  drawn[p]   = nil
  team_of[p] = nil
  sort_team(p)
  for q = 0, game.max_tanks() - 1 do
    if seconds[q] ~= nil and game.lobby_slot(q) ~= nil then
      game.score(q, seconds[q], SCORE_LABEL)
    end
  end
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
  if holder == p then
    holder = nil
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
  touched[p] = nil
  team_of[p] = nil
end

-- The teams are the round's, not the players'. A seat that changes team
-- itself is put back on the one its part says; a move this script made
-- arrives here too, a tick later, and is left alone.
function on_team_changed(p, team, scripted)
  if scripted or not running or over then
    return
  end
  team_of[p] = team
  sort_team(p)
end

-- Owning every base is somebody else's way to win a round, and sixteen pit
-- stops changing hands is not news.
function allow_base_win()
  return false
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
function on_tick(tick)
  if invuln_seat ~= nil and game.tick() >= invuln_to then
    invuln_seat = nil
  end
  if over or holder == nil then
    return
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
  description = string.format("One dead pillbox, %d minutes by default. " ..
                "Carrying it scores a point a second, slows you most on " ..
                "the fastest ground, and empties your gun; a new holder " ..
                "gets a short head start. Build it and it stops scoring " ..
                "but your gun refills; three shells kill it.",
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
  },

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this scenario implements:".
  callbacks = {
    on_setup = "One pillbox is the prize, the rest go; bases start " ..
               "neutral.",
    on_start = "Starts the clock and the compass.",
    on_end = "Logs how long the round ran.",
    on_tick = "Holder speed by terrain, plus boost.",
    on_player_join = "A joiner hunts, on 0 points.",
    on_player_leave = "A leaving holder's built prize dies.",
    on_base_captured = "A base: half armour, then gone 30 s.",
    on_pill_placed = "Built: 3 armour, no score. Dropped: dead.",
    on_pill_picked_up = "Holder: own team, 1 point/s, slow, unarmed.",
    on_pill_killed = "A shot-down prize is anybody's.",
    on_pill_captured = "Only the holder may own a built prize.",
    on_built = "A repair stops at 3 armour.",
    can_build = "Only the holder may repair it.",
    on_team_changed = "Teams are fixed.",
    allow_base_win = "Holding every base does not win.",
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
