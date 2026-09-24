-- Infection
--
-- Everybody starts on the same side. Twenty seconds in one of them turns, and
-- from there every tank that dies comes back infected: which side you are on
-- is decided by who has killed you rather than by what the lobby put you on.
-- The round ends when the last survivor turns, or when six minutes are up with
-- one still standing.
--
-- The two sides are the same tank with different numbers on it. The infected
-- are half again as quick, turn and accelerate harder, and pay for it with
-- armour that gives way in three hits rather than eight and a gun that fires
-- two thirds as often. They come back three seconds after they die, which is
-- what makes the horde a horde: a survivor who wins a fight has bought a few
-- seconds, not a kill.
--
-- The map belongs to the survivors. Only they capture a base or a pillbox,
-- only they build, and only they carry mines. The infected have the ground and
-- nothing on it, and a trickle of shells so that an empty gun is not the end of
-- them. What a survivor owns is let go when he turns: his pillboxes and his
-- bases go neutral rather than changing sides with him, so a fort is taken back
-- rather than turned around, and a neutral gun shoots at whoever comes near it.
--
-- Nobody can hurt his own side. Friendly fire would otherwise be the quickest
-- way to end the round, and a survivor shot by a survivor would join the horde
-- for it. What turns a survivor is the horde, or his own hand: drowning and his
-- own mine count, so the sea is not a way out of a chase, while a pillbox
-- nobody owns and a mine the map came with only kill him.
--
-- A bot on the infected side is told who to chase. Left alone a brain plays the
-- ordinary game, and the ordinary game is the one this scenario has taken off
-- it, so each one is handed a survivor to hunt and given another when that one
-- turns. Every bot is also retuned for its side: a survivor gives ground when
-- he is outnumbered, and the horde does nothing but hunt.
--
-- For the last ninety seconds every survivor is drawn on the infected's map. A
-- round that has come down to one man sitting in a wood has stopped being
-- played, so the clock takes the hiding place away rather than the scenario
-- waiting it out.
--
-- Nothing here names a square, a pillbox or a base, so it plays over whatever
-- map the host has committed.

local SURVIVORS = 1
local INFECTED  = 2

local ROUND_SECONDS   = 360   -- the whole round
local HEAD_START      = 20    -- before the first one turns
local WARNING_SECONDS = 3     -- what the first one is told, and nobody else
local BEACON_SECONDS  = 90    -- the tail of the round the survivors are drawn in
local FEED_EVERY      = 2     -- seconds between an infected shell, and a point of armour
local AIM_REFRESH     = 8     -- seconds before a bot is told the same order again
local CHANGE_AFTER    = 4     -- seconds a bot keeps an order before it is changed
local SWITCH_MARGIN   = 5     -- squares nearer another survivor has to be before a
                              -- bot is moved off the one it is chasing
local VIEW_SQUARES    = 14    -- how far a brain sees a tank: half its 29-square view
local ATTACK_WITHIN   = 11    -- a chase becomes an attack order this close
local GOTO_SLACK      = 5     -- squares a survivor may move before a far bot is
                              -- sent to where he is now
local INFECTED_SHELLS = 8     -- what one comes back with
local INFECTED_CARRY  = 12    -- and the most the trickle feeds it to
local INFECTED_ARMOUR = 50    -- percent of a tank's armour an infected one spawns with

-- Faster, weaker. Speed, accel and turn are percentages of the classic tank;
-- reload is a percentage of the time between shots, so 160 is a gun that fires
-- two thirds as often; dealt and taken price every blow either way, so 180
-- taken is armour that gives way in three hits where a survivor's takes eight.
local INFECTED_MODS   = { speed = 145, accel = 150, turn = 125,
                          reload = 160, dealt = 80, taken = 180 }

-- What the last survivor is handed, on the grounds that being the last one is
-- punishment enough. Speed, accel and turn are left out, which is the classic
-- tank: the whole set is replaced rather than merged.
local LAST_STAND_MODS = { reload = 70, dealt = 130 }

local SCORE_ALIVE  = "ALIVE"
local SCORE_TURNED = "TURNED"

-- The panel square is 128 units on a side, origin top left.
local TITLE_H     = 14
local CLOCK_Y     = 17
local HEAD_Y      = 31        -- the two column headings, under the clock
local ROSTER_Y    = 42
local ROSTER_STEP = 9         -- small text is eight units tall
local ROSTER_ROWS = 9         -- as many rows as fit under ROSTER_Y
local COL_W       = 64        -- two columns, each half the square
local COL_PAD     = 3         -- from a column's edge to its text
local SMALL_EM    = 8         -- small text height in units, which is its em

local side    = {}            -- seat -> SURVIVORS or INFECTED
local turned  = {}            -- seat -> how many it has turned
local lived   = {}            -- seat -> seconds it lasted
local fell    = {}            -- seat -> the square it last died on
local marked  = {}            -- seat -> the horde is being shown where it is
local chasing = {}            -- seat -> the survivor a bot's brain was pointed at
local told_at = {}            -- seat -> when it was last handed an order
local told    = {}            -- seat -> which order that was, as a short key
local told_for = {}           -- seat -> what that order was after, as a short key
local goto_at = {}            -- seat -> the square a goto order sent it to
local tuned   = {}            -- seat -> the init table it was last handed, as text
local touched = {}            -- seat -> every knob and flag word it has been handed
local hide_at = 3             -- squares off a tank in forest stops being seen
local elapsed = 0
local running = false
local over    = false
local beacon  = false
local ends_at = 0             -- the tick the round ends on, for the panel clock
local zero    = nil           -- the first to turn, once one has been picked
local alone   = nil           -- the last survivor, once there is one

-- A seat that is in the round: on the roster, and on the field rather than
-- held. A seat held for a bot is on the roster and has no tank.
local function in_round(p)
  local slot = game.lobby_slot(p)
  return slot ~= nil and slot.connected and slot.fielded
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

-- Everybody on one side, in seat order. The script's own table is what says
-- which side a seat is on: game.set_team has told the server, but this is the
-- answer the hooks and the policies are written against and it is never behind.
local function roll(s)
  local out = {}
  for p = 0, game.max_tanks() - 1 do
    if side[p] == s and in_round(p) then
      out[#out + 1] = p
    end
  end
  return out
end

-- A survivor's score is the seconds he has lasted; an infected one's is how
-- many he has turned. The label changes with the side, so the scoreboard says
-- which one a player is on without being told.
local function post_score(p)
  if side[p] == INFECTED then
    game.score(p, turned[p] or 0, SCORE_TURNED)
  elseif side[p] == SURVIVORS then
    game.score(p, lived[p] or 0, SCORE_ALIVE)
  end
end

-- The mark rides the tank and only the infected are shown it. Marker ids run 0
-- to 15 and so do seats, so a seat's own number is its mark.
local function show(p)
  if marked[p] then
    return
  end
  marked[p] = true
  game.marker_follow(p, p, "red", { team = INFECTED })
end

local function unshow(p)
  if not marked[p] then
    return
  end
  marked[p] = nil
  game.clear_marker(p, { team = INFECTED })
end

-- How wide a string draws in small text, in panel units. The panel has no clip
-- of its own and the font is the player's, so this is an estimate from Inter's
-- advance widths, the default font, rounded up. A monospace font is narrower
-- than this, so a name cut to fit here fits there too.
local function glyph_em(c)
  if c:find("^[ijlI%.,:;'!|]$") then return 0.30 end
  if c:find("^[frt1 %(%)%[%]%-]$") then return 0.42 end
  if c:find("^[mwMW@]$") then return 1.00 end
  if c:find("^%u$") then return 0.76 end
  return 0.64
end

local function text_width(s)
  local w = 0
  for ch in s:gmatch("[%z\1-\127\194-\244][\128-\191]*") do
    w = w + ((#ch > 1) and 1.00 or glyph_em(ch)) * SMALL_EM
  end
  return w
end

-- s, cut at a whole character and ended with ".." when it runs past width, so a
-- long name on the left never reaches the right column.
local function fit(s, width)
  if text_width(s) <= width then
    return s
  end
  local out, w = "", text_width("..")
  for ch in s:gmatch("[%z\1-\127\194-\244][\128-\191]*") do
    local cw = text_width(ch)
    if w + cw > width then
      break
    end
    out, w = out .. ch, w + cw
  end
  return out .. ".."
end

local function panel()
  local left  = roll(SURVIVORS)
  local horde = roll(INFECTED)
  local list  = {
    { "rect",  0, 0, 128, TITLE_H, "grey_dark", true },
    { "text",  64, 3, "white", "normal", "centre", "INFECTION" },
    { "timer", 64, CLOCK_Y, "yellow", "normal", "centre", "down", ends_at },
    { "text",  COL_PAD, HEAD_Y, "cyan", "small", "left", "Uninfected" },
    { "text",  COL_W + COL_PAD, HEAD_Y, "red", "small", "left", "Infected" },
  }
  local n = #list

  -- One column: a name on the left of it and its number on the right, cut so
  -- the two never meet. When the side is longer than the column, the last row
  -- says how many more there are.
  local function column(seats, x0, colour, value_of)
    local right = x0 + COL_W - COL_PAD
    for row, p in ipairs(seats) do
      local y = ROSTER_Y + (row - 1) * ROSTER_STEP
      if row == ROSTER_ROWS and #seats > ROSTER_ROWS then
        n = n + 1
        list[n] = { "text", x0 + COL_PAD, y, colour, "small", "left",
                    string.format("+%d more", #seats - ROSTER_ROWS + 1) }
        return
      end
      local value = string.format("%d", value_of(p))
      local room  = right - (x0 + COL_PAD) - text_width(value) - COL_PAD
      n = n + 1
      list[n] = { "text", x0 + COL_PAD, y, colour, "small", "left",
                  fit(name_of(p), room) }
      n = n + 1
      list[n] = { "text", right, y, colour, "small", "right", value }
    end
  end

  column(left, 0, "cyan", function(p) return lived[p] or 0 end)
  column(horde, COL_W, "red", function(p) return turned[p] or 0 end)

  game.panel(0, list)
end

local finish

finish = function(line, winner)
  if over then
    return
  end
  over = true
  game.message(line)
  game.end_round(line, winner)
end

-- Called every time the roll could have changed. The round ends here or on the
-- clock, and nowhere else: owning every base is taken out of the round below.
local function check_the_end()
  if over or not running then
    return
  end
  local left = roll(SURVIVORS)
  if #left == 0 then
    finish("The infection took everyone.", INFECTED)
    return
  end
  if #left == 1 and alone ~= left[1] then
    alone = left[1]
    game.set_modifiers(alone, LAST_STAND_MODS)
    game.add_stocks(alone, { armour = game.rule("tank_full_armour"),
                             shells = game.rule("tank_full_shells") })
    game.announce(name_of(alone) .. " is the last one left", 4)
    show(alone)
  end
end

-- What a survivor owned goes to nobody rather than to the horde. A pillbox and
-- a base answer to a seat, not to a side, so without this a fort would change
-- hands the moment the man holding it did, and the survivors would be shot by
-- their own guns for the rest of the round.
local function let_go_of(p)
  for n = 1, game.num_pills() do
    local pb = game.pill(n)
    if pb ~= nil and pb.owner == p and not pb.in_tank then
      game.set_pill_owner(n, game.NEUTRAL)
    end
  end
  for n = 1, game.num_bases() do
    local b = game.base(n)
    if b ~= nil and b.owner == p then
      game.set_base_owner(n, game.NEUTRAL, true)
    end
  end
end

-- How a bot on either side is tuned. Left alone a brain plays the ordinary
-- game at the ordinary numbers, and both sides here are playing something
-- else: a survivor is outnumbered from the twentieth second on and should
-- give ground rather than trade armour, and an infected tank is a fast, soft
-- thing with a trickle of shells that should do nothing but hunt.
--
-- A brain is retuned with game.bot_init, which hands it a new init table.
-- The table replaces the last one whole, but the numbers it set do not go
-- back on their own: GoalHunter writes every cfg=NAME=VALUE into its own
-- constants, and a constant stays where it was put until something writes it
-- again. So a bot that changes sides is handed its new numbers AND the
-- ordinary value of every number its old side changed. DEFAULTS is that
-- ordinary value, read off the brain's constants.lua, for every knob either
-- side touches.
--
-- The ordinary value is the Hard one. A Medium or Easy bot plays with some of
-- these knobs moved by its level (the flee numbers, OUTNUMBERED_DISENGAGE,
-- TANK_COMBAT_BASE_COST), and a script cannot see a seat's level, so a knob
-- put back is put back to Hard. That only happens to a knob this script
-- changed on that seat earlier, and the only one it happens to is a survivor
-- turning: the horde loses the flee numbers the survivors were given, and a
-- tank with twenty armour that ran at twelve would never fight.
local DEFAULTS = {
  OUTNUMBERED_DISENGAGE        = false,
  OUTNUMBERED_NET              = 2,
  TANK_COMBAT_FLEE_ARMOUR      = 0,
  TANK_COMBAT_FLEE_SHELLS      = 0,
  TANK_COMBAT_STANDOFF_RANGE   = 7,
  TAKE_COVER_W_ENEMY           = 20,
  TAKE_COVER_BASE_COST         = 60,
  TANK_COMBAT_MIN_SHELLS       = 10,
  TANK_COMBAT_BASE_COST        = 30,
  ATTACK_TANK_COMMITMENT_BONUS = 50,
  STRATEGIC_PLACE_ENABLED      = true,
  PILL_REPOSITION_ENABLED      = true,
  BUILDER_POOL_ENABLED         = true,
}

-- The flag words a brain keeps until it is told the opposite. GoalHunter puts
-- noblitz, noclaimdead and suicider back itself on every new table; ammoless
-- it leaves standing, and "normal" is the word that takes it off.
local FLAG_UNDO = { ammoless = "normal" }

-- The two sides. A survivor stays on his base and his pillboxes as he always
-- would; what changes is that he breaks off a fight he is losing, one enemy
-- more than his side is enough to count as losing, and he looks for cover
-- sooner and harder. An infected tank engages on one shell, bids for a tank
-- fight over everything else and sees one through, never goes looking for a
-- base it could not take (ammoless: nothing on the map refuels it), and has
-- its builder's pillbox and building work switched off, since the horde can
-- build nothing and hold nothing.
local ROLES = {
  survivor = {
    flags = { "noblitz" },
    cfg = {
      OUTNUMBERED_DISENGAGE      = true,
      OUTNUMBERED_NET            = 1,
      TANK_COMBAT_FLEE_ARMOUR    = 15,
      TANK_COMBAT_FLEE_SHELLS    = 3,
      TANK_COMBAT_STANDOFF_RANGE = 8,
      TAKE_COVER_W_ENEMY         = 40,
      TAKE_COVER_BASE_COST       = 30,
    },
  },
  infected = {
    flags = { "noblitz", "nosuicider", "ammoless" },
    cfg = {
      TANK_COMBAT_MIN_SHELLS       = 1,
      TANK_COMBAT_BASE_COST        = 5,
      ATTACK_TANK_COMMITMENT_BONUS = 150,
      STRATEGIC_PLACE_ENABLED      = false,
      PILL_REPOSITION_ENABLED      = false,
      BUILDER_POOL_ENABLED         = false,
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

-- The whole table for one seat on one side, and the same table as one line
-- of text so two of them can be compared. Every knob this seat has been
-- handed before and this side does not set goes back to DEFAULTS.
local function init_table(side_name, p)
  local role = ROLES[side_name]
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

-- Hands a bot the table for the side it is on, unless it already has that
-- table: every table a brain takes it says a line about to its own side, so
-- the same one twice is noise. A refusal is left for the next second to try
-- again: a bot that has only just joined may not have a brain to take it yet.
local function tune(p)
  local slot = game.lobby_slot(p)
  if slot == nil or not slot.bot or side[p] == nil then
    return
  end
  local name = (side[p] == INFECTED) and "infected" or "survivor"
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
    if side[p] ~= nil and in_round(p) then
      tune(p)
    end
  end
end

-- A bot the horde is made of is told who to chase with a hint, which the
-- brain takes the way it takes an order a teammate types: it says it back to
-- its own side, holds it for a minute, and drops it for anything a person on
-- that side says afterwards.
--
-- Two orders are used. Attack names the survivor's tank, and the brain only
-- keeps it while it can see him: ten seconds without a sight of him and it
-- gives the order up, saying so. So attack is for a survivor inside the
-- bot's own view and out of the woods, and for anybody further off, or
-- sitting in a forest where a tank cannot be seen from more than a few
-- squares, the bot is sent to the square he is on instead. It drives there
-- without stopping for anything, which is what a chase across the map wants,
-- and is handed the attack as soon as it is close enough to see him.
--
-- The same order handed to a bot that still holds it puts its minute back to
-- the start without a word said, so each bot is handed its order again every
-- eight seconds. A new order is a line in the horde's chat, and a changed
-- one is two, so a bot keeps the survivor it was given until another is
-- clearly nearer, keeps an attack while its man is still in view, and keeps
-- the square it was sent to until the man has moved a few squares off it.
local function chebyshev(ax, ay, bx, by)
  local dx, dy = math.abs(ax - bx), math.abs(ay - by)
  return (dx > dy) and dx or dy
end

local function in_forest(x, y)
  local t = game.map_tile(x, y)
  return t == game.TERRAIN.forest or t == game.TERRAIN.mine_forest
end

-- Somewhere a tank can be sent: on the map, not the sea, not a building. A
-- survivor in a boat is chased to the nearest shore rather than into the sea.
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

local function goto_order(p, x, y, from, target)
  local g = goto_at[p]
  if g ~= nil and chebyshev(g.x, g.y, x, y) <= GOTO_SLACK and
     chebyshev(from.x, from.y, g.x, g.y) > 2 then
    x, y = g.x, g.y
  else
    x, y = standable_near(x, y)
    if x == nil then
      return nil
    end
  end
  return { key = string.format("goto %d,%d", x, y), x = x, y = y,
           target = target, hint = { verb = "goto", x = x, y = y } }
end

local function order_for(p, q, from)
  local s = game.tank(q)
  if s == nil then
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
local function tell(p, order)
  if order == nil then
    return
  end
  local age = (told_at[p] ~= nil) and (elapsed - told_at[p]) or nil
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
    goto_at[p] = order.x and { x = order.x, y = order.y } or nil
  end
end

-- The nearest survivor, measured from where the bot is or, while it waits to
-- come back, from where it fell. The one it is already chasing is kept unless
-- another is SWITCH_MARGIN squares nearer: picking the nearest afresh every
-- pass would swing a bot between two men a square apart.
local function pick_target(p, from)
  local cur = chasing[p]
  local best, best_d, cur_d = nil, nil, nil
  for _, q in ipairs(roll(SURVIVORS)) do
    local s = game.tank(q)
    if s ~= nil and not s.dead then
      local d = chebyshev(from.x, from.y, s.mx, s.my)
      if q == cur then
        cur_d = d
      end
      if best_d == nil or d < best_d then
        best, best_d = q, d
      end
    end
  end
  if cur_d ~= nil and cur_d - best_d < SWITCH_MARGIN then
    return cur
  end
  return best
end

local function point_one(p)
  local slot = game.lobby_slot(p)
  if slot == nil or not slot.bot or side[p] ~= INFECTED then
    return
  end
  local t = game.tank(p)
  local from = fell[p]
  if t ~= nil and (not t.dead or from == nil) then
    from = { x = t.mx, y = t.my }
  end
  if from == nil then
    return
  end
  local q = pick_target(p, from)
  if q == nil then
    return
  end
  if q ~= chasing[p] then
    chasing[p] = q
    goto_at[p] = nil
  end
  tell(p, order_for(p, q, from))
end

local function point_the_horde()
  for _, p in ipairs(roll(INFECTED)) do
    point_one(p)
  end
end

local function infect(p, by)
  if over or side[p] == INFECTED then
    return
  end
  side[p] = INFECTED
  turned[p] = turned[p] or 0
  game.set_team(p, INFECTED)
  game.set_modifiers(p, INFECTED_MODS)
  post_score(p)
  -- Everybody but the first turns by dying, and comes back on the loadout the
  -- policy below hands the horde. The first turns where he stands, with a
  -- survivor's forty shells and forty mines still in the tank, so his kit is
  -- brought down to the horde's here rather than at his first death. The armour
  -- he has is left alone: the modifiers have already made it worth half.
  local t = game.tank(p)
  if t ~= nil and not t.dead then
    game.set_stocks(p, { shells = INFECTED_SHELLS, mines = 0, trees = 0 })
  end
  if by ~= nil and by ~= p and side[by] == INFECTED then
    turned[by] = (turned[by] or 0) + 1
    post_score(by)
  end
  unshow(p)
  let_go_of(p)
  -- A destroyed tank puts its cargo down as it goes, which lands after this
  -- handler has run, so the pillbox it was carrying is released a second later.
  game.timer(1, function() let_go_of(p) end)
  game.announce(name_of(p) .. " has turned", 2)
  -- And the man it happened to is told in his own words, after the line that
  -- goes to everybody so that his replaces it on his own screen. Turning is the
  -- one thing in the round that happens to a player rather than being done by
  -- him, and reading his own name in the third person is not being told.
  game.announce("You have turned", 3, p)
  game.message("You are infected. Everyone you kill joins you.", p)
  game.sound("man_dying_near")
  -- A bot is retuned for the horde straight away rather than on the next pass,
  -- so it comes back from its three seconds dead already playing the part.
  tune(p)
  point_one(p)
  check_the_end()
end

-- Who turns first, and it matters who is in the room. Two people or more and
-- it is one of them, because a brain plays the ordinary game and would spend
-- its first minute looking for a base rather than hunting anybody. One person
-- and it is a bot: picking the only human is not a draw, it is a certainty, and
-- it hands the one player in the round the one seat with nobody to hunt. Two in
-- the round at least, either way, or there is nobody to hunt at all.
local function pick_zero()
  local pool, humans, bots = {}, {}, {}
  for _, p in ipairs(roll(SURVIVORS)) do
    pool[#pool + 1] = p
    local slot = game.lobby_slot(p)
    if slot ~= nil and slot.bot then
      bots[#bots + 1] = p
    else
      humans[#humans + 1] = p
    end
  end
  if #pool < 2 then
    return nil
  end
  local from = pool
  if #humans > 1 then
    from = humans
  elseif #humans == 1 and #bots > 0 then
    from = bots
  end
  return from[math.random(#from)]
end

local function turn_zero()
  if over or not running then
    return
  end
  local p = zero
  if p == nil or side[p] ~= SURVIVORS or not in_round(p) then
    p = pick_zero()
  end
  if p == nil then
    game.message("Infection: nobody to turn. The round runs to the clock.")
    return
  end
  zero = p
  game.announce("The infection is loose", 3)
  infect(p, nil)
end

-- The warning is private, and it is the only advantage either side is given:
-- three seconds to be standing somewhere useful when it happens. It says what
-- is about to happen rather than hinting at it, because a player reading it has
-- no way of knowing he is the one it is about.
local function warn_zero()
  if over or not running then
    return
  end
  zero = pick_zero()
  if zero == nil then
    game.message("Infection needs two players. Nobody turns.")
    return
  end
  game.announce("You turn in three seconds", WARNING_SECONDS, zero)
  game.message("You are the first to turn. Get among them.", zero)
end

-- The infected are not resupplied by anything on the map, so they are fed here:
-- a shell and a point of armour every two seconds, to a fraction of what a tank
-- holds. It is slow enough that a fight costs them something and fast enough
-- that one that lives keeps hunting.
local function feed_the_horde()
  local armour_cap = math.floor(game.rule("tank_full_armour") * INFECTED_ARMOUR
                                / 100)
  if armour_cap < 1 then
    armour_cap = 1
  end
  for _, p in ipairs(roll(INFECTED)) do
    local t = game.tank(p)
    if t ~= nil and not t.dead then
      local add = nil
      if t.shells < INFECTED_CARRY then
        add = { shells = 1 }
      end
      if t.armour < armour_cap then
        add = add or {}
        add.armour = 1
      end
      if add ~= nil then
        game.add_stocks(p, add)
      end
    end
  end
end

local function light_the_beacon()
  beacon = true
  for _, p in ipairs(roll(SURVIVORS)) do
    show(p)
  end
  game.announce("They can see you now", 3, { team = SURVIVORS })
  game.announce("The survivors are marked", 3, { team = INFECTED })
end

local function each_second()
  if over then
    return
  end
  elapsed = elapsed + 1

  for _, p in ipairs(roll(SURVIVORS)) do
    lived[p] = (lived[p] or 0) + 1
    post_score(p)
  end

  if elapsed % FEED_EVERY == 0 then
    feed_the_horde()
  end
  -- Every second, because a bot that has only just joined may not take its
  -- table on the first try, and a chase goes stale in seconds. Neither says
  -- anything when there is nothing new to say.
  tune_everybody()
  point_the_horde()
  if not beacon and elapsed >= ROUND_SECONDS - BEACON_SECONDS then
    light_the_beacon()
  end

  panel()

  if elapsed >= ROUND_SECONDS then
    local left = roll(SURVIVORS)
    if #left == 1 then
      finish(name_of(left[1]) .. " was the only one left alive.", SURVIVORS)
    else
      finish(string.format("%d survivors held out.", #left), SURVIVORS)
    end
  else
    game.timer(1, each_second)
  end
end

function on_start()
  running = true
  elapsed = 0
  ends_at = game.tick() + ROUND_SECONDS * 100
  -- The engine's own time limit would end the round with a line of its own and
  -- no winner, so it is set a few seconds long and the script gets there first.
  game.set_game_time((ROUND_SECONDS + 5) * 100)

  -- Whatever the lobby put people on, they start together. Roster writes are
  -- refused during the setup that opens a round, which is why this is here and
  -- not in on_setup.
  for p = 0, game.max_tanks() - 1 do
    if in_round(p) then
      side[p]   = SURVIVORS
      lived[p]  = 0
      turned[p] = 0
      game.set_team(p, SURVIVORS)
      post_score(p)
    end
  end
  -- How close a tank in a wood has to be before it can be seen, in squares.
  hide_at = math.floor(game.rule("tree_hide_distance") / 256)
  tune_everybody()

  -- Three lines, because a player who has not read the scenario has to be able
  -- to play it from what the newswire tells him in the first twenty seconds.
  game.message("Infection: one of you turns in twenty seconds. " ..
               "Everyone he kills turns with him.")
  game.message("Survivors: the bases, the pillboxes and the mines are yours. " ..
               "Hold out for six minutes.")
  game.message("Infected: quicker, weaker, and back three seconds after " ..
               "you die. Kill them all.")
  game.timer(HEAD_START - WARNING_SECONDS, warn_zero)
  game.timer(HEAD_START, turn_zero)
  game.timer(1, each_second)
  panel()
end

-- The horde turns you, and so does your own hand. The map does not.
--
-- A pillbox nobody owns and a mine the map came with are hazards, not
-- recruiters: dying to one costs a survivor his position and his pills and
-- nothing else, and the horde does not get to count a kill it had no part in.
-- The sim fires every pillbox shell with no owner on it, whoever owns the gun,
-- so a pillbox kill arrives here as game.NEUTRAL and is one of those.
--
-- A death nobody caused names the dying tank as its own killer, which is how
-- drowning and your own mine read. Those do turn you. Without that the sea is a
-- way out of any chase the horde is winning, and a full tank of armour on the
-- other side of it.
function on_tank_killed(victim, killer, cause, scripted)
  if over or not running then
    return
  end
  local t = game.tank(victim)
  if t ~= nil then
    fell[victim] = { x = t.mx, y = t.my }
  end
  if side[victim] == INFECTED then
    return
  end
  if killer == victim then
    infect(victim, nil)
  elseif side[killer] == INFECTED then
    infect(victim, killer)
  else
    -- Now that a death is sometimes a conversion and sometimes not, the man it
    -- happened to is the one who has to be told which it was.
    game.message("That was not the infection. You are still a survivor.",
                 victim)
  end
end

-- The modifiers are kept across a respawn, so this is a net under the moment of
-- turning rather than the thing that does it.
function on_tank_spawned(p, mx, my, respawn, scripted)
  if over or side[p] ~= INFECTED then
    return
  end
  local t = game.tank(p)
  if t ~= nil and t.mods.speed ~= INFECTED_MODS.speed then
    game.set_modifiers(p, INFECTED_MODS)
  end
end

-- Anybody who walks in mid-round walks in infected. Joining the side that is
-- winning is the only seat there is: the survivors are a closed set from the
-- first tick, and letting a latecomer join them would hand them a life.
function on_player_join(p, scripted)
  if not running or over then
    return
  end
  side[p]   = INFECTED
  -- A new player in a seat starts from nothing, not from the last one's count
  -- or the square the last one died on.
  turned[p] = 0
  fell[p]   = nil
  game.set_team(p, INFECTED)
  game.message("You have arrived infected. Hunt them down.", p)
  tune(p)
  point_one(p)
  -- A joiner is given the panels the round is holding, but not the scores, so
  -- everybody's is written again for them.
  for q = 0, game.max_tanks() - 1 do
    if side[q] ~= nil and in_round(q) then
      post_score(q)
    end
  end
  check_the_end()
end

function on_player_leave(p, scripted)
  local was = side[p]
  side[p]    = nil
  -- The engine does not clear a marker when its seat empties.
  unshow(p)
  chasing[p] = nil
  told_at[p]  = nil
  told[p]     = nil
  told_for[p] = nil
  goto_at[p] = nil
  tuned[p]   = nil
  touched[p] = nil
  if alone == p then
    alone = nil
  end
  -- A bot chasing the seat that just left is pointed somewhere else on the next
  -- pass: its target reads as out of the round, which is what the pass tests.
  if not running or over then
    return
  end
  if was == SURVIVORS then
    check_the_end()
  elseif was == INFECTED and #roll(INFECTED) == 0 then
    -- The horde walked out. Rather than running the clock down with nothing
    -- hunting, the infection starts again.
    zero = nil
    turn_zero()
  end
end

-- Which side you are on is what the round is playing for, so it is not a lobby
-- setting any more.
function on_team_changed(p, team, scripted)
  if scripted or not running or over then
    return
  end
  if side[p] ~= nil and team ~= side[p] then
    game.set_team(p, side[p])
    game.message("Which side you are on is not yours to choose.", p)
  end
end

function on_end()
  over = true
  game.log(string.format("Infection ended after %d seconds, %d still alive",
                         elapsed, #roll(SURVIVORS)))
end

-- Owning every base is somebody else's way to win a round. This one ends when a
-- side is empty or when the clock runs out, and the script says which.
function allow_base_win()
  return false
end

-- Two sides, and a host cannot open a third for somebody to sit the round out
-- on.
function allow_extra_teams()
  return false
end

-- A survivor comes back with everything; an infected one with half a tank of
-- armour, eight shells and nothing to build with. Asked on every respawn, and
-- by then the seat has changed sides, so a survivor's last death is what fuels
-- his first life on the other side.
function spawn_loadout(p)
  if side[p] == INFECTED then
    local armour = math.floor(game.rule("tank_full_armour") * INFECTED_ARMOUR
                              / 100)
    if armour < 1 then
      armour = 1
    end
    return { shells = INFECTED_SHELLS, mines = 0, armour = armour, trees = 0 }
  end
  return { shells = game.rule("tank_full_shells"),
           mines  = game.rule("tank_full_mines"),
           armour = game.rule("tank_full_armour"),
           trees  = game.rule("tank_full_trees") }
end

-- The horde builds nothing: no walls, no roads, no boats, no mines, and no
-- farming for the trees any of them would cost.
function can_build(p, action, x, y, n)
  if side[p] == INFECTED then
    return false
  end
  return nil
end

-- And takes nothing. A base the horde drives over stays neutral, a dead pillbox
-- stays on the ground, and the map's supplies stay the survivors' to hold
-- rather than a prize for killing them. Shooting a base flat still neutralises
-- it, so the horde can deny what it cannot use.
function can_capture(kind, n, p)
  if side[p] == INFECTED then
    return false
  end
  return nil
end

-- No friendly fire, either way. A survivor shot by a survivor would join the
-- horde for it, which is a round anybody can end on their own, and a charge
-- that shoots itself apart is not a charge. A shell from a neutral pillbox has
-- no side, reads as nil here, and is priced as it always was.
function damage_scale(attacker, victim, cause)
  if attacker ~= victim and side[attacker] ~= nil and
     side[attacker] == side[victim] then
    return 0
  end
  return nil
end

-- The horde comes back at the start nearest to where it fell, so a fight that
-- was won is a fight that is about to happen again in the same place. Survivors
-- are left to the engine: they only ever spawn once.
function on_choose_start(p)
  if side[p] ~= INFECTED then
    return nil
  end
  local at = fell[p]
  if at == nil then
    return nil
  end
  local best, best_d = nil, nil
  for n = 1, game.num_starts() do
    local s = game.start(n)
    if s ~= nil then
      local dx, dy = s.x - at.x, s.y - at.y
      local d = dx * dx + dy * dy
      if best_d == nil or d < best_d then
        best, best_d = n, d
      end
    end
  end
  return best
end

scenario = {
  name        = "Infection",
  description = "One of you turns, and everyone he kills turns with him. " ..
                "The survivors hold the map; the horde is quicker, weaker, " ..
                "and back in three seconds.",
  api         = 1,
  kind        = "scenario",
  game        = "open",

  -- Nothing here names a square, a pill number or a base number, so this one
  -- plays over whatever map the host has committed.
  bound       = false,

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this scenario implements:".
  callbacks = {
    on_start = "Puts everyone on the survivors; one turns after 20 s.",
    on_end = "Logs how long the round ran.",
    on_player_join = "A late joiner arrives infected.",
    on_player_leave = "Ends the round if no survivor is left.",
    on_team_changed = "Players cannot change side.",
    on_tank_spawned = "Keeps the infected quick and weak.",
    on_tank_killed = "A survivor killed by the infected turns.",
    allow_extra_teams = "Only the two sides.",
    allow_base_win = "Holding every base does not win.",
    can_build = "The infected cannot build.",
    can_capture = "The infected cannot take bases or pillboxes.",
    on_choose_start = "The infected respawn near where they fell.",
    spawn_loadout = "The infected respawn with half armour and 8 shells.",
    damage_scale = "No friendly fire.",
  },

  -- Both sides are named so the lobby has them, but nobody picks one: every
  -- seat is put on the survivors at the first tick and the sides are decided by
  -- who dies after that.
  lobby = {
    teams = {
      { id = SURVIVORS, bots = 0 },
      { id = INFECTED,  bots = 0 },
    },
  },

  rules = {
    -- Three seconds rather than five. A survivor never respawns as a survivor,
    -- so this is the horde's number alone: it is what decides how much a fight
    -- bought the man who won it.
    tank_death_ticks = 150,

    -- A base is the survivors' only resupply and they are usually standing on
    -- it under fire, so it hands out twice as fast and builds its own stock
    -- back four times as fast as the classic table.
    base_refuel_armour_ticks = 23,
    base_refuel_shells_ticks = 4,
    base_refuel_mines_ticks  = 4,
    base_regen_ticks         = 250,

    -- Walls and guns are how a survivor holds ground he cannot outrun, and a
    -- builder working at the classic rate is a builder who is caught at it.
    lgm_build_ticks   = 10,
    lgm_cost_pill_new = 2,

    -- A mine is the one thing a survivor has that the horde has not, and it is
    -- laid in front of something already charging. The shorter fuse is what
    -- makes it a trap rather than a warning.
    mine_fuse_ticks = 5,

    -- The dead take the fort with them. A wreck going up beside a pillbox takes
    -- eight of the fifteen armour it holds, so two bodies is a gun, which is
    -- the horde's answer to a wall it cannot shoot down.
    tank_explosion_damage = 8,

    -- Gunfire carries. The hunt only works if the horde can hear where the
    -- shooting is, so a shot is heard near out to twenty squares and heard at
    -- all out to sixty, rather than fifteen and forty.
    sound_soft_range = 20,
    sound_none_range = 60,
  },
}
