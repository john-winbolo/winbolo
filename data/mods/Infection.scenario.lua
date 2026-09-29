-- Infection
--
-- Everybody starts on the same side. Ten seconds in one of them turns, and
-- from there every tank that dies comes back infected: which side you are on
-- is decided by who has killed you rather than by what the lobby put you on.
-- The round ends when the last survivor turns, or when the clock runs out (ten
-- minutes unless the host sets another length in the lobby) with one still
-- standing. A last survivor shot dead is watched go up first: the round ends
-- when their tank's explosion has played out, not the moment they die.
--
-- The two sides are the same tank. The one exception is the first to turn,
-- who is half again as quick, turns and accelerates harder, and is harder to
-- kill than anybody: a full tank of armour that gives way in ten hits rather
-- than eight, and a gun as good as anybody's. They keep it all round long and
-- on every life, and nobody else is given it while they are in the horde, not
-- even if they leave. The one time it moves is when the whole horde has left:
-- the infection starts again, and the new first one to turn is given it. Every
-- other infected tank is the classic tank with a full tank of armour, and
-- the last survivor is the classic tank too. The infected come back three
-- seconds after they die, which is what makes the horde a horde: a survivor
-- who wins a fight has bought a few seconds, not a kill.
--
-- The map belongs to the survivors. Only they capture a base or a pillbox,
-- only they build, and only they carry mines. The infected have the ground and
-- nothing on it, and a trickle of shells so that an empty gun is not the end of
-- them. What a survivor owns does not change sides with them when they turn:
-- their pillboxes, standing or carried, go to the nearest survivor still
-- alive, and their bases go neutral, so a fort stays with the survivors rather
-- than being turned around on them.
--
-- Nobody can hurt their own side. Friendly fire would otherwise be the quickest
-- way to end the round, and a survivor shot by a survivor would join the horde
-- for it. What kills and turns a survivor is an infected tank's shell and
-- nothing else: drowning, a mine, a pillbox or any other death only kills
-- them, and they come back a survivor. The help below and deep sea turn a
-- survivor without a death.
--
-- Until the horde has made its first kill it is helped. Every minute that goes
-- by without one, another survivor picked at random turns where they stand, and
-- everybody is told why. The first kill by an infected shell ends the help for
-- the rest of the round. The last survivor is never taken this way: the round
-- ends in a fight or on the clock, not on a draw.
--
-- A bot plays the round the way it plays any game, with one thing taken away:
-- on the infected side it never picks a base to refuel at, because no base
-- refuels the horde. Nothing else about how a bot plays is changed, on either
-- side.
--
-- For the last ninety seconds every infected player's panel is a compass that
-- points at the nearest survivor, which is the last survivor once only one is
-- left. A round that has come down to one player sitting in a wood has stopped
-- being played, so the clock takes the hiding place away rather than the
-- scenario waiting it out. The host can change the ninety in the lobby.
--
-- Deep sea is no hiding place either. Once the first one has turned, a
-- survivor who stays on deep sea for ten seconds turns where they are, the same
-- as any other turn, even the last survivor. Their screen counts the seconds
-- down, and leaving the deep sea starts the count again. The host can change
-- the ten in the lobby. Every start is on deep sea, so a survivor who has just
-- respawned is not counted until they first reach land, or until twenty
-- seconds have gone by, whichever comes first: a death that was not an
-- infected shell must not turn them by the back door.
--
-- Nothing here names a square, a pillbox or a base, so it plays over whatever
-- map the host has committed.

local SURVIVORS = 1
local INFECTED  = 2

local ROUND_SECONDS   = 600   -- the whole round; a lobby setting, in minutes
local HEAD_START      = 10    -- before the first one turns
local WARNING_SECONDS = 3     -- what the first one is told, and nobody else
local HELP_EVERY      = 60    -- seconds with no infected kill before another turns
local FEED_EVERY      = 2     -- seconds between points of armour for the infected
local SHELL_EVERY     = 0.5   -- seconds between shells for the infected
local REFRESH_SECONDS = 0.2   -- how often the deep sea clock and compass run
local SPAWN_GRACE     = 20    -- seconds a respawned survivor has to reach land
                              -- before the deep sea clock counts them
local TICKS_PER_SEC   = 100   -- game.tick() counts a hundred to the second
local BOOM_SECONDS    = 3     -- from the last survivor's death to the end of
                              -- the round, so their explosion plays out

-- The defaults of two more lobby settings (scenario.settings at the bottom of
-- the file declares them from these numbers). on_start puts the host's choice
-- over them.
local COMPASS_SECONDS    = 90   -- the tail of the round the horde has a compass
                                -- in; 0 is no compass
local DEEP_WATER_SECONDS = 10   -- how long a survivor can stay on deep sea

-- What an infected tank comes back with, and the most the trickle feeds it
-- to: half a full tank of shells.
local function infected_shells()
  return math.floor(game.rule("tank_full_shells") / 2)
end

-- What the first one to turn is handed, and nobody else. Percentages of the
-- classic tank. Reload and dealt are 100, a gun as good as anybody's; taken
-- prices every blow they take, and a full tank's armour gives way in eight
-- hits, so 80 is ten. The whole set is replaced rather than merged, so every
-- field is written out, and an empty table is the classic tank.
local ZERO_MODS = { speed = 145, accel = 150, turn = 125,
                    reload = 100, dealt = 100, taken = 80 }

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

-- The compass, which takes the roster's place on an infected player's panel.
-- The title and the clock stay where they are, and the rose sits under them.
local CX, CY, RADIUS = 64, 66, 24
local NORTH_Y     = 31        -- the "N" above the rose
local STATUS_Y    = 96        -- who the needle points at, and how far off
local COUNT_Y     = 108       -- how many survivors are left

local side    = {}            -- seat -> SURVIVORS or INFECTED
local turned  = {}            -- seat -> how many it has turned
local lived   = {}            -- seat -> seconds it lasted
local fell    = {}            -- seat -> the square it last died on
local handed  = {}            -- seat -> the word a bot's brain was last handed
local wet     = {}            -- seat -> the tick a survivor went onto deep sea
local warned  = {}            -- seat -> the deep sea warning is on its screen
local fresh   = {}            -- seat -> the tick a respawned survivor's deep sea
                              -- clock starts, if they have not reached land
local drawn   = {}            -- seat -> what its panel was last sent, as a key
local heir_of = {}            -- seat -> who got its pillboxes when it turned
local roster  = nil           -- the roster panel, while it is sent seat by seat
local roster_n = 0            -- counts roster panels, to tell a seat is behind
local elapsed = 0
local running = false
local over    = false
local ending  = false         -- the round is decided and waits on an explosion
local compass_on = false      -- the horde has a compass on the survivors
local loose   = false         -- the first one has turned
local bitten  = false         -- an infected shell has turned somebody
local helper  = nil           -- the help's waiting timer, while there is one
local ends_at = 0             -- the tick the round ends on, for the panel clock
local zero    = nil           -- the first to turn, once one has been picked
local alone   = nil           -- the last survivor, once there is one
local boosted = nil           -- the seat that holds ZERO_MODS, if any does
local crushed = {}            -- pill -> true, run over by the horde, to go
local razed   = {}            -- base -> true, run over by the horde, to go

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

-- A survivor's score is the seconds they have lasted; an infected one's is how
-- many they have turned. The label changes with the side, so the scoreboard says
-- which one a player is on without being told.
local function post_score(p)
  if side[p] == INFECTED then
    game.score(p, turned[p] or 0, SCORE_TURNED)
  elseif side[p] == SURVIVORS then
    game.score(p, lived[p] or 0, SCORE_ALIVE)
  end
end

-- Hands the first one to turn their numbers once they are infected, and takes them
-- off any seat that held them and is not the first one any more, which is the
-- old first one when the infection starts again. The engine keeps a tank's
-- modifiers across a respawn and only clears them when the tank is made new,
-- on a join or a new round, so this is called when zero changes and on a turn,
-- and does nothing when nothing has changed. Only this function and the net
-- in on_tank_spawned ever set modifiers, and the net only hands ZERO_MODS back
-- to boosted, so every seat but boosted is the classic tank.
local function crown_zero()
  if boosted ~= nil and boosted ~= zero then
    game.set_modifiers(boosted, {})
    boosted = nil
  end
  if zero ~= nil and side[zero] == INFECTED and boosted ~= zero then
    game.set_modifiers(zero, ZERO_MODS)
    boosted = zero
  end
end

local function whole(n)
  return math.floor(n + 0.5)
end

-- The compass rose, built once. It never moves and it is the same for
-- everybody, so it is sixteen lines worked out at load and copied into each
-- infected player's panel.
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

-- Takes a survivor's deep sea count off: the clock starts again the next time
-- they go onto deep sea, and the warning comes off their screen.
local function dry_off(p)
  wet[p] = nil
  if warned[p] then
    warned[p] = nil
    game.announce("", 0, p)
  end
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

-- The title bar and the round's clock, which every panel starts with.
local function panel_head()
  return {
    { "rect",  0, 0, 128, TITLE_H, "grey_dark", true },
    { "text",  64, 3, "white", "normal", "centre", "INFECTION" },
    { "timer", 64, CLOCK_Y, "yellow", "normal", "centre", "down", ends_at },
  }
end

local function roster_list()
  local left  = roll(SURVIVORS)
  local horde = roll(INFECTED)
  local list  = panel_head()
  -- Each heading is black on a band of its side's colour, so it reads as a
  -- heading and not as one more name. The band ends two units above the
  -- first name, and a unit is left clear between the two bands.
  local function heading(x0, colour, label)
    list[#list + 1] = { "rect", x0 + 1, HEAD_Y - 1, COL_W - 2, SMALL_EM + 2,
                        colour, true }
    list[#list + 1] = { "text", x0 + COL_PAD, HEAD_Y, "black", "small", "left",
                        label }
  end
  heading(0, "cyan", "UNINFECTED")
  heading(COL_W, "red", "INFECTED")
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

  return list
end

-- There is one panel, and a player is shown the last list that was sent to
-- them, whether it went to everybody or to them alone. So until the compass
-- comes on the roster goes to everybody at once. After that the infected are
-- sent a compass each, and the roster is kept here for refresh to send to
-- everybody else one seat at a time: a list sent to everybody would replace
-- the compasses for a moment every second.
local function panel()
  local list = roster_list()
  if compass_on then
    roster   = list
    roster_n = roster_n + 1
  else
    game.panel(0, list)
  end
end

-- The survivor nearest to seat p's tank, by where the two tanks are, for p's
-- compass. A survivor waiting to respawn is left out: their tank is not
-- anywhere to point at. nil when no survivor has a tank on the field.
local function nearest_survivor(me)
  local best, best_d, bx, by = nil, nil, nil, nil
  for _, q in ipairs(roll(SURVIVORS)) do
    local t = game.tank(q)
    if t ~= nil and not t.dead then
      local x, y = t.wx / 256, t.wy / 256
      local dx, dy = x - me.wx / 256, y - me.wy / 256
      local d = dx * dx + dy * dy
      if best_d == nil or d < best_d then
        best, best_d, bx, by = q, d, x, y
      end
    end
  end
  return best, bx, by
end

-- One infected player's compass. The needle is the vector from their tank to
-- the survivor's, cut to the rose's radius, so no angle is worked out. The
-- panel is only sent when the needle's tip or the lines under it differ from
-- what this seat was last sent.
local function draw_compass(p, me, left)
  local q, tx, ty = nearest_survivor(me)
  local status, colour = "NOBODY TO FIND", "grey"
  local ux, uy, tipx, tipy = nil, nil, nil, nil
  if q ~= nil then
    local dx, dy = tx - me.wx / 256, ty - me.wy / 256
    local len = math.sqrt(dx * dx + dy * dy)
    colour = "cyan"
    status = string.format("%s %d", (left == 1) and "LAST ONE" or "NEAREST",
                           whole(len))
    if len >= 0.5 then
      ux, uy = dx / len, dy / len
      tipx, tipy = CX + RADIUS * ux, CY + RADIUS * uy
    end
  end
  local count = string.format("%d uninfected left", left)

  local key = string.format("c %s %s %s %s", tipx and whole(tipx) or "-",
                            tipy and whole(tipy) or "-", status, count)
  if drawn[p] == key then
    return
  end
  drawn[p] = key

  local list = panel_head()
  local n = #list
  for i = 1, #RING do
    n = n + 1
    list[n] = RING[i]
  end
  n = n + 1
  list[n] = { "text", CX, NORTH_Y, "grey", "small", "centre", "N" }
  if tipx ~= nil then
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
  list[n] = { "text", CX, STATUS_Y, colour, "small", "centre", status }
  n = n + 1
  list[n] = { "text", CX, COUNT_Y, "grey", "small", "centre", count }
  game.panel(0, list, p)
end

-- Every seat's panel while the compass is on: a compass for the infected and
-- the roster for everybody else. Each seat is sent at most one list here, and
-- nothing else sends the panel while the compass is on, so the engine's one
-- update per seat per tick is never met.
local function draw_everybody()
  local left = #roll(SURVIVORS)
  local key  = "r " .. roster_n
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil and in_round(p) then
      if side[p] == INFECTED then
        draw_compass(p, t, left)
      elseif roster ~= nil and drawn[p] ~= key then
        drawn[p] = key
        game.panel(0, roster, p)
      end
    end
  end
end

local finish

-- Ends the round, now or after wait seconds. While it waits the round is
-- decided: nothing else can end it, and the clock running out does not hand
-- the survivors a round they have already lost.
finish = function(line, winner, wait)
  if over or ending then
    return
  end
  if wait ~= nil then
    ending = true
    game.timer(wait, function()
      ending = false
      finish(line, winner)
    end)
    return
  end
  over = true
  game.message(line)
  game.end_round(line, winner)
end

-- Called every time the roll could have changed. The round ends here or on the
-- clock, and nowhere else: owning every base is taken out of the round below.
-- Before the first one turns there is no horde, so nothing here can end the
-- round and nobody is the last survivor yet.
--
-- shot is true when the last one turned by being shot dead. Their tank is
-- going up, and the round waits for that: a fireball runs 40 steps of 4 ticks
-- (tank_explosion_length, tank_explosion_update_ticks, on the sim's 50 a
-- second), then bursts for 8 frames of 6 ticks, about 2.1 s in all, the same
-- big or small. BOOM_SECONDS is that and some room for the client to draw it.
-- Any other turn (deep sea, the help, a leave) has no explosion to wait for.
local function check_the_end(shot)
  if over or not running or not loose then
    return
  end
  local left = roll(SURVIVORS)
  if #left == 0 then
    finish("The infection took everyone.", INFECTED,
           shot and BOOM_SECONDS or nil)
    return
  end
  -- The last survivor is told, and is handed nothing: they play the round
  -- out on the classic tank. In the tail of the round the horde's compass
  -- points at them.
  if #left == 1 and alone ~= left[1] then
    alone = left[1]
    game.announce(name_of(alone) .. " is the last one left", 4)
  end
end

-- Who a survivor's pillboxes go to when they turn: the survivor whose tank is
-- nearest theirs, not counting one waiting to respawn; when every survivor is
-- waiting, the first survivor in seat order; when no survivor is left, nobody.
-- The same survivor is used for the drop a second later, unless they have
-- turned or gone by then.
local function heir_for(p)
  local h = heir_of[p]
  if h ~= nil and side[h] == SURVIVORS and in_round(h) then
    return h
  end
  local t = game.tank(p)
  local best, best_d, any = nil, nil, nil
  for _, q in ipairs(roll(SURVIVORS)) do
    if q ~= p then
      any = any or q
      local s = game.tank(q)
      if t ~= nil and s ~= nil and not s.dead then
        local dx, dy = s.mx - t.mx, s.my - t.my
        local d = dx * dx + dy * dy
        if best_d == nil or d < best_d then
          best, best_d = q, d
        end
      end
    end
  end
  heir_of[p] = best or any
  return heir_of[p]
end

-- A pillbox seat p is carrying, handed on. The engine only puts a pillbox
-- into a tank from the ground and only changes the owner of one on the
-- ground, so it is put down first. It is put down under the heir's tank and
-- then put in it, which leaves the square as it was for the next one. When
-- that square will not take it (a base, another pillbox) or the heir is
-- waiting to respawn, it is put down under p's tank and given to the heir
-- where it lies, dead, for them to pick up. When p cannot put it down at
-- all it stays in their tank.
local function pass_carried(p, heir, n)
  local h = game.tank(heir)
  if h ~= nil and not h.dead and game.drop_pill(p, n, h.mx, h.my) then
    if not game.give_pill(heir, n) then
      game.set_pill_owner(n, heir)
    end
    return
  end
  if game.drop_pill(p, n) then
    game.set_pill_owner(n, heir)
  end
end

-- What a survivor owned does not go to the horde. A pillbox and a base answer
-- to a seat, not to a side, so without this a fort would change hands the
-- moment the player holding it did, and the survivors would be shot by their
-- own guns for the rest of the round. Pillboxes, on the ground or carried, go
-- to another survivor (heir_for says which). When no survivor is left they
-- are left as they are: the round is over by then. Bases go neutral.
local function let_go_of(p)
  local heir = heir_for(p)
  if heir ~= nil then
    for n = 1, game.num_pills() do
      local pb = game.pill(n)
      if pb ~= nil and pb.owner == p then
        if pb.in_tank then
          pass_carried(p, heir, n)
        else
          game.set_pill_owner(n, heir)
        end
      end
    end
  end
  for n = 1, game.num_bases() do
    local b = game.base(n)
    if b ~= nil and b.owner == p then
      game.set_base_owner(n, game.NEUTRAL, true)
    end
  end
end

-- What a bot is handed, and it is one word. An infected bot is handed an init
-- table holding "ammoless", GoalHunter's word for a bot that never picks a base
-- to refuel at. A base refuels nobody on the horde, and a brain that was not
-- told so would drive to one and sit on it. The word does nothing else: it
-- leaves the bot's shells, its fighting and every other goal as they were.
--
-- A survivor is handed "normal", the word that takes "ammoless" off again, and
-- is what a brain plays with when it is told nothing. A survivor needs it: the
-- word outlives the round. The server builds a bot's brain for the next round
-- from the last table it was handed, so a bot that ended the last round
-- infected would start this one as a survivor who never refuels. A seat never
-- goes from the horde back to the survivors inside a round, so this is the
-- only thing a survivor is ever handed.
--
-- Beside the word goes one GoalHunter setting, on for a survivor and off for
-- the horde: a survivor bot never fights a tank itself, and only shoots its
-- own pillbox to anger it at a tank that comes near.
local function word_for(p)
  if side[p] == INFECTED then
    return "ammoless"
  end
  return "normal"
end

-- Hands a bot the word for its side, unless it already has it: every table a
-- brain takes it says a line about to its own side, so the same one twice is
-- noise. A refusal is left for the next second to try again: a bot that has
-- only just joined may not have a brain to take it yet.
local function hand_word(p)
  local slot = game.lobby_slot(p)
  if slot == nil or not slot.bot or side[p] == nil then
    return
  end
  local word = word_for(p)
  if handed[p] == word then
    return
  end
  if game.bot_init(p, { [word] = "1",
                        cfg = "ATTACK_TANK_PILL_HEAT_ONLY=" ..
                              tostring(word == "normal") }) then
    handed[p] = word
  end
end

local function hand_everybody()
  for p = 0, game.max_tanks() - 1 do
    if side[p] ~= nil and in_round(p) then
      hand_word(p)
    end
  end
end

local function infect(p, by)
  if over or side[p] == INFECTED then
    return
  end
  side[p] = INFECTED
  turned[p] = turned[p] or 0
  game.set_team(p, INFECTED)
  -- The first one to turn is handed their numbers here; anybody else stays
  -- the classic tank they already are.
  crown_zero()
  post_score(p)
  -- Everybody but the first turns by dying, and comes back on the loadout the
  -- policy below hands the horde. The first turns where they stand, with a
  -- survivor's forty shells and forty mines still in the tank, so their kit
  -- is brought down to the horde's here rather than at their first death, and
  -- their armour is filled, so one turned mid-fight starts their new life
  -- whole. So is one the help or deep sea turns where they stand.
  local t = game.tank(p)
  if t ~= nil and not t.dead then
    game.set_stocks(p, { shells = infected_shells(), mines = 0, trees = 0,
                         armour = game.rule("tank_full_armour") })
  end
  if by ~= nil and by ~= p and side[by] == INFECTED then
    turned[by] = (turned[by] or 0) + 1
    post_score(by)
  end
  dry_off(p)
  fresh[p] = nil
  heir_of[p] = nil
  let_go_of(p)
  -- A destroyed tank puts its cargo down as it goes, which can land after this
  -- handler has run, so anything it was carrying is handed on a second later.
  game.timer(1, function()
    if not over then
      let_go_of(p)
    end
  end)
  game.announce(name_of(p) .. " has turned", 2)
  -- And the player it happened to is told in their own words, after the line
  -- that goes to everybody so that theirs replaces it on their own screen.
  -- Turning is the one thing in the round that happens to a player rather than
  -- being done by them, and reading your own name in the third person is not
  -- being told.
  game.announce("You have turned", 3, p)
  game.message("You are infected. Everyone you kill joins you.", p)
  game.sound("man_dying_near")
  -- A bot is handed the horde's word straight away rather than on the next
  -- pass, so it comes back from its three seconds dead not looking for a base.
  hand_word(p)
  -- Only a shell kill names who did it, so by says the tank is going up.
  check_the_end(by ~= nil)
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

-- The help. A survivor at random turns where they stand, as the first one did,
-- and the clock goes round again, until an infected shell has made a kill. It
-- stops for good at one survivor: a joiner after the first turn is infected, so
-- the count never goes back up.
local function help_the_horde()
  helper = nil
  if over or not running or bitten then
    return
  end
  local pool = roll(SURVIVORS)
  if #pool < 2 then
    return
  end
  local p = pool[math.random(#pool)]
  game.message("No kills for a minute. The infection takes " ..
               name_of(p) .. ".")
  infect(p, nil)
  helper = game.timer(HELP_EVERY, help_the_horde)
end

local function turn_zero()
  if over or ending or not running then
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
  loose = true
  game.announce("The infection is loose", 3)
  infect(p, nil)
  -- A restart after the whole horde has walked out is a fresh horde, and it is
  -- helped the same way, so the clock is started over rather than doubled.
  bitten = false
  if helper ~= nil then
    game.cancel_timer(helper)
  end
  helper = game.timer(HELP_EVERY, help_the_horde)
end

-- The warning is private, and it is the only advantage the warning gives:
-- three seconds to be standing somewhere useful when it happens. It says what
-- is about to happen rather than hinting at it, because a player reading it has
-- no way of knowing they are the one it is about.
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
-- a point of armour every two seconds, to a full tank of armour. It is slow
-- enough that a fight costs them something and fast enough that one that
-- lives keeps hunting.
local function feed_the_horde()
  local armour_cap = game.rule("tank_full_armour")
  for _, p in ipairs(roll(INFECTED)) do
    local t = game.tank(p)
    if t ~= nil and not t.dead and t.armour < armour_cap then
      game.add_stocks(p, { armour = 1 })
    end
  end
end

-- And a shell every half second, to half a full tank of shells. On its own
-- timer rather than the second's, which is too slow for it.
local function feed_shells()
  if over then
    return
  end
  local cap = infected_shells()
  for _, p in ipairs(roll(INFECTED)) do
    local t = game.tank(p)
    if t ~= nil and not t.dead and t.shells < cap then
      game.add_stocks(p, { shells = 1 })
    end
  end
  game.timer(SHELL_EVERY, feed_shells)
end

-- From here to the end of the round the infected are sent a compass each
-- rather than the roster. refresh draws them; each_second only builds the
-- roster for everybody else.
local function start_compass()
  compass_on = true
  game.announce("The horde has a compass on you", 3, { team = SURVIVORS })
  game.announce("Your compass points at the survivors", 3, { team = INFECTED })
end

-- The deep sea clock. It only runs once the first one has turned: before that
-- there is nobody to hide from, and a turn would start the infection before
-- its time. It only counts survivors, and only while their tank is alive and
-- on a deep sea square. A survivor who has just respawned on a start is not
-- counted until they first reach land or SPAWN_GRACE runs out. Their screen
-- shows the seconds left, to a tenth, and is written again on every pass, so
-- the count goes down as they watch. When it runs out they turn where they
-- are, through infect like every other turn, so their pillboxes are handed on
-- the same way.
local function watch_the_water(now)
  local sea = game.TERRAIN.deep_sea
  for p = 0, game.max_tanks() - 1 do
    local t = nil
    if loose and side[p] == SURVIVORS and in_round(p) then
      t = game.tank(p)
    end
    local alive  = t ~= nil and not t.dead
    local afloat = alive and game.map_tile(t.mx, t.my) == sea
    if fresh[p] ~= nil and ((alive and not afloat) or now >= fresh[p]) then
      fresh[p] = nil
    end
    if afloat and fresh[p] == nil then
      wet[p] = wet[p] or now
      local left = DEEP_WATER_SECONDS - (now - wet[p]) / TICKS_PER_SEC
      if left <= 0 then
        dry_off(p)
        game.message(name_of(p) .. " stayed in deep water too long.")
        infect(p, nil)
        if over then
          return
        end
      else
        warned[p] = true
        game.announce(string.format("Deep water! You turn in %.1f s", left),
                      1, p)
      end
    elseif wet[p] ~= nil or warned[p] then
      dry_off(p)
    end
  end
end

-- Five times a second: the deep sea clock, and while it is on, the compass.
local function refresh()
  if over then
    return
  end
  watch_the_water(game.tick())
  if over then
    return
  end
  if compass_on then
    draw_everybody()
  end
  game.timer(REFRESH_SECONDS, refresh)
end

local function each_second()
  if over then
    return
  end
  elapsed = elapsed + 1

  -- A survivor can leave the field without a leave event, so the roll is
  -- checked every second as well as on every join, leave and death.
  check_the_end()
  if over then
    return
  end

  for _, p in ipairs(roll(SURVIVORS)) do
    lived[p] = (lived[p] or 0) + 1
    post_score(p)
  end

  if elapsed % FEED_EVERY == 0 then
    feed_the_horde()
  end
  -- Every second, because a bot that has only just joined may not take its
  -- word on the first try. It says nothing when there is nothing new to say.
  -- The first pass is a second in rather than in on_start: a brain built for
  -- this round reads the table it was built from on its first think, so a
  -- survivor's "normal" that landed before then would be overruled by the
  -- "ammoless" the last round left in it.
  hand_everybody()
  if not compass_on and COMPASS_SECONDS > 0 and
     elapsed >= ROUND_SECONDS - COMPASS_SECONDS then
    start_compass()
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
  -- The host's lobby choices, over the defaults at the top of the file.
  -- game.setting answers from inside a hook, where the host has read the
  -- scenario table at the bottom of the file, not at the top level of it.
  ROUND_SECONDS      = game.setting("round_minutes") * 60
  COMPASS_SECONDS    = game.setting("compass_seconds")
  DEEP_WATER_SECONDS = game.setting("deep_water_seconds")
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

  -- Three lines, because a player who has not read the scenario has to be able
  -- to play it from what the newswire tells them in the first ten seconds.
  game.message("Infection: one of you turns in ten seconds. " ..
               "Everyone they kill turns with them.")
  game.message(string.format("Survivors: the bases, the pillboxes and the " ..
                             "mines are yours. Hold out for %d minutes.",
                             math.floor(ROUND_SECONDS / 60)))
  game.message("Infected: back three seconds after you die, and the first " ..
               "of you is quicker and tougher. Kill them all.")
  game.message(string.format("Survivors: stay in deep water for %d seconds " ..
                             "and you turn.", DEEP_WATER_SECONDS))
  game.timer(HEAD_START - WARNING_SECONDS, warn_zero)
  game.timer(HEAD_START, turn_zero)
  game.timer(1, each_second)
  game.timer(SHELL_EVERY, feed_shells)
  game.timer(REFRESH_SECONDS, refresh)
  panel()
end

-- Only an infected tank's shell turns you. Every other death is a hazard, not
-- a recruiter: dying to one costs a survivor their position and their pills and
-- nothing else, and the horde does not get to count a kill its gun did not make.
--
-- That covers drowning and your own mine (the dying tank is named as its own
-- killer), a pillbox (the sim fires every pillbox shell with no owner on it, so
-- the killer is game.NEUTRAL), and any mine, even one an infected tank is named
-- for, because it was laid before that player turned.
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
  if cause == "shell" and side[killer] == INFECTED then
    bitten = true
    infect(victim, killer)
  end
end

-- The modifiers are kept across a respawn, so this is a net under the moment of
-- turning rather than the thing that does it: the first one's numbers are
-- only handed to them again if their tank has come back without them. A
-- modifier of 0 is the classic tank, the same as 100.
local function same_pct(a, b)
  if a == nil or a == 0 then a = 100 end
  if b == nil or b == 0 then b = 100 end
  return a == b
end

-- A survivor comes back on a start, and every start is on deep sea, so the
-- deep sea clock leaves them alone until they reach land or SPAWN_GRACE runs
-- out. The first one's numbers are checked here too.
function on_tank_spawned(p, mx, my, respawn, scripted)
  if over then
    return
  end
  if side[p] == SURVIVORS then
    dry_off(p)
    fresh[p] = game.tick() + SPAWN_GRACE * TICKS_PER_SEC
  end
  if p ~= boosted then
    return
  end
  local t = game.tank(p)
  if t == nil then
    return
  end
  for k, v in pairs(ZERO_MODS) do
    if not same_pct(t.mods[k], v) then
      game.set_modifiers(p, ZERO_MODS)
      return
    end
  end
end

-- Anybody who walks in after the first one has turned walks in infected.
-- Joining the side that is winning is the only seat there is: the survivors
-- are a closed set from then on, and letting a latecomer join them would hand
-- them a life. Before that there is no horde to join, so a joiner is one more
-- survivor, and may be the one who turns.
function on_player_join(p, scripted)
  if not running or over then
    return
  end
  -- A new player in a seat starts from nothing, not from the last one's count
  -- or the square the last one died on.
  turned[p] = 0
  lived[p]  = 0
  fell[p]   = nil
  wet[p]    = nil
  warned[p] = nil
  drawn[p]  = nil
  heir_of[p] = nil
  if loose then
    side[p] = INFECTED
    game.set_team(p, INFECTED)
    game.message("You have arrived infected. Hunt them down.", p)
  else
    side[p] = SURVIVORS
    game.set_team(p, SURVIVORS)
    game.message("You are a survivor. One of you turns soon.", p)
  end
  hand_word(p)
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
  -- Nothing is sent to an empty seat: the deep sea count and the panel it was
  -- last sent are only forgotten, so a joiner in the seat starts from nothing.
  wet[p]     = nil
  warned[p]  = nil
  fresh[p]   = nil
  drawn[p]   = nil
  handed[p]  = nil
  if alone == p then
    alone = nil
  end
  -- The first one's numbers leave with them and nobody inherits them. Their
  -- tank goes with the seat, and a joiner in it is handed a new one, so there
  -- is nothing to clear.
  if zero == p then
    zero = nil
  end
  if boosted == p then
    boosted = nil
  end
  if not running or over or ending then
    return
  end
  if was == SURVIVORS then
    check_the_end()
  elseif was == INFECTED and #roll(INFECTED) == 0 then
    -- The horde walked out. Rather than running the clock down with nothing
    -- hunting, the infection starts again, and the old first one, if they are
    -- still on a seat, goes back to the classic tank before a new one is
    -- picked and given the first one's numbers.
    zero = nil
    crown_zero()
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

-- A survivor comes back with everything; an infected one with half a tank
-- of shells, nothing to build with, and a full tank of armour. Asked on every
-- respawn, and by then the seat has changed sides, so a survivor's last death
-- is what fuels their first life on the other side.
function spawn_loadout(p)
  if side[p] == INFECTED then
    return { shells = infected_shells(), mines = 0,
             armour = game.rule("tank_full_armour"), trees = 0 }
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

-- And takes nothing: the map's supplies are the survivors' to hold rather
-- than a prize for killing them. What the horde would have taken it destroys
-- instead. A base it could capture and a dead pillbox it drives over come
-- off the map for the rest of the round, so the survivors cannot have them
-- back either. A policy may not change the game, so each one is only noted
-- here and on_tick takes it off.
function can_capture(kind, n, p)
  if side[p] == INFECTED then
    if n ~= nil then
      if kind == "pill" then
        crushed[n] = true
      elseif kind == "base" then
        razed[n] = true
      end
    end
    return false
  end
  return nil
end

-- The pillboxes and bases the horde ran over, taken off the map. A pillbox is
-- looked at again first: one a survivor picked up or rebuilt in the same tick
-- stays.
function on_tick()
  if next(crushed) == nil and next(razed) == nil then
    return
  end
  for n in pairs(crushed) do
    local pb = game.pill(n)
    if pb ~= nil and not pb.in_tank and pb.armour == 0 then
      game.remove_pill(n)
    end
  end
  for n in pairs(razed) do
    if game.base(n) ~= nil then
      game.remove_base(n)
    end
  end
  crushed = {}
  razed   = {}
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
-- are left to the engine, including one who respawns after any death but an
-- infected shell.
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
  description = "One of you turns, and everyone they kill turns with them. " ..
                "The survivors hold the map; the horde is back in three " ..
                "seconds, and the first to turn is quicker and tougher.",
  api         = 1,
  kind        = "scenario",
  game        = "open",

  -- Nothing here names a square, a pill number or a base number, so this one
  -- plays over whatever map the host has committed.
  bound       = false,

  -- What the host sets in the lobby's details dialog; on_start reads them
  -- with game.setting. The defaults are the numbers at the top of the file.
  settings = {
    { id = "round_minutes", label = "Round length (minutes)", type = "int",
      min = 3, max = 15, step = 1, default = math.floor(ROUND_SECONDS / 60) },
    { id = "deep_water_seconds", label = "Deep water time before turning (s)",
      type = "int", min = 3, max = 60, step = 1,
      default = DEEP_WATER_SECONDS },
    { id = "compass_seconds", label = "Compass on survivors, last (s, 0 off)",
      type = "int", min = 0, max = 300, step = 15,
      default = COMPASS_SECONDS },
  },

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this scenario implements:".
  callbacks = {
    on_start = "All start as survivors; one turns after 10 s. Runs the " ..
               "deep water clock and compass.",
    on_end = "Logs how long the round ran.",
    on_player_join = "A late joiner arrives infected.",
    on_player_leave = "Ends the round with no survivors; restarts it " ..
                      "with no horde.",
    on_team_changed = "Players cannot change side.",
    on_tank_spawned = "Keeps the first infected strong; survivors get " ..
                      "20 s to reach land.",
    on_tank_killed = "A survivor shot by the infected turns; their " ..
                     "pillboxes go to a survivor.",
    allow_extra_teams = "Only the two sides.",
    allow_base_win = "Holding every base does not win.",
    can_build = "The infected cannot build.",
    can_capture = "The infected cannot take bases or pillboxes.",
    on_tick = "Removes bases and dead pillboxes the infected drive over.",
    on_choose_start = "The infected respawn near where they fell.",
    spawn_loadout = "Infected respawn with half shells and full armour.",
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
    -- Three seconds rather than five. Mostly the horde's number: a survivor
    -- only respawns as one after a death that was not an infected shell. It
    -- is what decides how much a fight bought the player who won it.
    tank_death_ticks = 150,

    -- A base is the survivors' only resupply and they are usually standing on
    -- it under fire, so it hands out armour and shells faster and builds its
    -- own stock back four times as fast as the classic table. Mines are left
    -- at the classic rate.
    base_refuel_armour_ticks = 23,
    base_refuel_shells_ticks = 4,
    base_regen_ticks         = 250,
  },
}
