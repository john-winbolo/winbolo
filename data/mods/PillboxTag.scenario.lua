-- Pillbox Tag
--
-- One pillbox, dead, loose on the map. Whoever is carrying it scores a point a
-- second, and after ten minutes the one with the most points has won. Carrying
-- it costs three tenths of your speed and every shell you own, so the holder
-- runs and everybody else hunts. Kill the holder, or drive over the pillbox
-- where it falls, and it is yours.
--
-- The map's own pillboxes are taken off at setup and one is kept back as the
-- prize. It never fires: a pillbox that has been built or repaired is put back
-- to nothing the moment it goes up, so the only thing it is ever good for is
-- being carried.
--
-- The compass is the panel. A script cannot draw on the edge of the game view,
-- so the square over the view is a compass rose instead: the needle points at
-- whatever is holding the prize — the tank, the man walking it out, or the
-- ground it is lying on — and every player is sent their own, because the
-- bearing is worked out from where that player is standing.
--
-- A bot cannot read a compass, so it is told instead: every bot but the holder
-- is sent after the prize, and every bot is tuned for the part it has. The
-- holder runs and never puts the prize down; the others hunt.
--
-- The bases are pit stops. They start neutral, driving over one hands out half
-- a tank of armour, and the base then goes off the map for half a minute
-- before it comes back.

local ROUND_SECONDS     = 600   -- the whole round
local CARRY_SPEED_PCT   = 70    -- the holder's share of the terrain's speed cap
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

-- The panel square is 128 units on a side, origin top left. The compass fills
-- the middle of it, the round's clock sits above and the scores below.
local CX, CY, RADIUS = 64, 50, 24
local NORTH_Y        = 14       -- the "N" above the rose
local STATUS_Y       = 78       -- what is holding the prize, and how far off
local BOARD_Y        = 90       -- the first score row
local BOARD_STEP     = 9        -- small text is eight units tall
local BOARD_ROWS     = 4

local pill        = nil         -- the one pillbox, 1-based
local holder      = nil         -- the seat carrying it, or nil
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

local function whole(n)
  return math.floor(n + 0.5)
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
local function prize()
  if pill == nil then
    return nil
  end
  local pb = game.pill(pill)
  if pb == nil then
    return nil
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

-- The two parts. Neither has a live pillbox to charge or a reason to move
-- one, and neither should have its builder spend the round on walls and
-- guns. A hunter bids hard for a tank fight, because the holder is a tank.
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

local finish

-- The second is the unit the whole round is counted in: the holder's point,
-- everybody's shell, and every thirtieth of them a mine.
local function each_second()
  if over then
    return
  end
  elapsed = elapsed + 1

  if holder ~= nil then
    seconds[holder] = (seconds[holder] or 0) + 1
    game.score(holder, seconds[holder], SCORE_LABEL)
  end
  board = leaderboard()

  -- Every second, because a bot that has only just joined may not take its
  -- table on the first try, and a chase goes stale in seconds. Neither says
  -- anything when there is nothing new to say.
  tune_everybody()
  aim_everybody()

  local mines = (elapsed % MINE_EVERY == 0)
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil and not t.dead then
      -- The holder is the one tank that is not restocked: an empty gun is
      -- what stops it shooting, and handing it a shell would undo that. A
      -- shell it came by some other way is taken off it here.
      if p == holder then
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
    game.message(
      "Pillbox Tag: carry the pillbox. A point a second, ten minutes.")
  end

  -- How close a tank in a wood has to be before it can be seen, in squares.
  hide_at = math.floor(game.rule("tree_hide_distance") / 256)
  tune_everybody()

  board = leaderboard()
  game.timer(1, each_second)
  refresh()
end

-- Everything the holder gives up, and gets back. The modifier set is replaced
-- whole rather than merged, so the empty table is the classic tank.
local function take_the_prize(p)
  holder = p
  seconds[p] = seconds[p] or 0
  game.set_modifiers(p, { speed = CARRY_SPEED_PCT })
  game.set_stocks(p, { shells = 0 })
  game.score(p, seconds[p], SCORE_LABEL)
  -- A bot takes the holder's part at once, and everybody else is turned on it.
  stop_the_fight(p)
  tune(p)
  aim_everybody()
end

local function lose_the_prize(p)
  holder = nil
  if p ~= nil then
    game.set_modifiers(p, {})
    tune(p)
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
-- A built one arrives at the sim's cap, and that is the one to put back to
-- nothing — the prize is never a gun.
function on_pill_placed(n, p, armour, scripted)
  if over or n ~= pill then
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

-- A builder sent to a pillbox repairs it, which would turn the prize back into
-- a gun. It goes to nothing again as soon as the work is done.
function on_built(p, action, x, y, scripted)
  -- A pillbox repair arrives as "repair" (a build order says "pill").
  if over or pill == nil or action ~= "repair" then
    return
  end
  local pb = game.pill(pill)
  if pb ~= nil and not pb.in_tank and pb.x == x and pb.y == y and
     pb.armour > 0 then
    game.set_pill_armour(pill, 0)
  end
end

-- And the repair is refused before it starts where it can be. A pill order
-- on the square the prize is lying on is a repair of it. n is the pillbox the
-- tank would put down, not the one on the square, so the square is what is
-- compared.
function can_build(p, action, x, y, n)
  if over or pill == nil or action ~= "pill" then
    return nil
  end
  local pb = game.pill(pill)
  if pb ~= nil and not pb.in_tank and pb.x == x and pb.y == y then
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
  -- The empty gun: a base is the one thing that can hand the holder a shell.
  if new == holder then
    game.set_stocks(new, { shells = 0 })
  end
  game.remove_base(n)
  game.timer(BASE_DOWN_SECONDS, function() restore_base(x, y, 0) end)
end

-- A player who arrives in the tenth minute is given a score row and everybody
-- else's, because the panels are replayed to a joiner and the scores are not.
function on_player_join(p, scripted)
  if not running or over then
    return
  end
  -- A new player in a seat starts from nothing, not from the last one's time,
  -- and is sent a whole panel of their own.
  seconds[p] = 0
  drawn[p]   = nil
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
function on_player_leave(p, scripted)
  if holder == p then
    holder = nil
  end
  drawn[p]    = nil
  told_at[p]  = nil
  told[p]     = nil
  told_for[p] = nil
  goto_at[p] = nil
  tuned[p]   = nil
  touched[p] = nil
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

-- Full shells, no mines, and enough trees to put the pillbox down — though
-- the rules below make that free, so the trees are only there for the rest of
-- what a builder does.
function spawn_loadout(p)
  return {
    shells = game.rule("tank_full_shells"),
    mines  = 0,
    armour = game.rule("tank_full_armour"),
    trees  = game.rule("tank_full_trees"),
  }
end

function on_end()
  over = true
  game.log(string.format("Pillbox Tag ended after %d seconds", elapsed))
end

scenario = {
  name        = "Pillbox Tag",
  description = "One dead pillbox, ten minutes. Carrying it scores a point a " ..
                "second, costs you three tenths of your speed, and empties " ..
                "your gun.",
  api         = 1,
  kind        = "scenario",
  game        = "open",

  -- Nothing here names a square, a pill number or a base number, so this one
  -- plays over whatever map the host has committed.
  bound       = false,

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this scenario implements:".
  callbacks = {
    on_setup = "Keeps one dead pillbox as the prize; bases start neutral.",
    on_start = "Starts the 10-minute clock and the compass panel.",
    on_end = "Logs how long the round ran.",
    on_player_join = "A joiner starts on 0 points.",
    on_player_leave = "Forgets the seat's bot orders.",
    on_base_captured = "A base gives half armour, then is gone for 30 s.",
    on_pill_placed = "The prize is put down and loses its armour.",
    on_pill_picked_up = "The holder scores a point a second, slower and unarmed.",
    on_built = "A repaired prize goes back to no armour.",
    can_build = "Nobody can repair the prize.",
    allow_base_win = "Holding every base does not win.",
    announce = "Base captures are not announced.",
    spawn_loadout = "Tanks spawn with full shells and no mines.",
  },

  rules = {
    -- Putting the pillbox down is free, so the holder can drop it anywhere
    -- the ground will take it without having gone farming first.
    lgm_cost_pill_new = 0,

    -- A dead builder comes back thirty times as fast. The classic 3 world
    -- units a frame is most of three minutes across a map, and a round is
    -- only ten.
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
