-- Joust
--
-- A round deep-sea arena, 38 squares across (1176 squares of water). Every
-- other square of the map is a building, out to the mined border. There is
-- no land inside it. Every tank starts and comes
-- back on a boat, and any hit on a tank on a boat sinks the boat, so the
-- tank drowns: one hit kills. The first side to reach the kill count the
-- host picked wins.
--
-- Who is a side. The host picks it in the lobby with the "Teams" setting.
-- In "Free For All", the default, every tank is a side of its own. Tanks
-- that share a lobby team are each put on a team of their own when the
-- round starts, so nobody is allied with anybody, and they cannot ally
-- during the round. Their lobby teams are put back when a side reaches the
-- target and this script ends the round. A round ended any other way (the
-- lobby's time limit, the host, the server) keeps the Free For All teams:
-- a roster write is refused once the round has stopped, so on_end cannot
-- put them back, and no hook tells a script how long the round has left.
-- In "Use Lobby Teams" each lobby team is a side: its members are allied,
-- their kills add up to the team's score, and killing a teammate scores
-- nothing. A tank on no team is a side of its own. Every tank also keeps a
-- score of its own kills, which is what the scoreboard lists under its team.
--
-- What counts as a kill. A shell or a mine counts for the tank that fired it
-- or laid it. A tank that drowns, or that dies to its own mine, counts for the
-- last enemy tank that hit it in the last CREDIT_SECONDS, and for nobody when
-- no enemy did. Killing a tank on your own side scores nothing.
--
-- Where a tank starts. Joust.map has sixteen starts, evenly spaced on a ring
-- four squares in from the water's edge, each facing the middle. The script
-- reads them from the map and names no squares. In a team round the ring is
-- cut into one arc for each team, in the order the starts go round the ring,
-- so teammates start side by side and each team has a stretch of its own.
-- The first time a tank comes on, it takes the start that matches its place
-- (its place in its own team in a team round), so no two tanks share one.
-- After that, a tank comes back on the start of its own (its team's arc in a
-- team round) that is farthest from every enemy tank, and no shell can hit it
-- for its first SHIELD_SECONDS, so a shell already flying cannot sink it as
-- it arrives.
--
-- There are no bases: a base is land, and a tank that drives onto it leaves
-- its boat. A tank gets back one shell every REFILL_SECONDS instead.
--
-- The scoreboard moves. When the order changes, each line slides to its new
-- place over SLIDE_TICKS, and a new line rises from the bottom (a line that
-- was just past the last row slides in from just under it). The lines of
-- a tank that scores, and of its team, flash for FLASH_TICKS. A tank that
-- scores again without dying in between is on a streak, and the kill line
-- says so: "Double kill", "Triple kill!", "QUADRUPLE KILL!" and on up. Dying
-- ends the streak, however long ago the last kill was. The panel is only sent while something on it is
-- changing or moving; a still panel sends nothing.
--
-- The leader's line (the top tank, or the top team's line in a team round)
-- is drawn in large text on a taller first row, and the rows under it sit
-- lower to make room. When the lead changes hands, the new leader grows and
-- the old one shrinks at once, and both slide to their new places.

local DEFAULT_TARGET = 10
local CREDIT_SECONDS = 10     -- how long a hit keeps its claim on a drowning
local REFILL_SECONDS = 2      -- one shell back this often, up to full
local OPENING_TICKS  = 200    -- first two seconds: fixed starts, not farthest
local SHIELD_SECONDS = 1      -- a new tank cannot be hit this long
local PANEL_ROWS     = 9
local ROW_TOP        = 30     -- panel y of the first line
local LEAD_STEP      = 18     -- the leader's large row, 16-unit text
local ROW_STEP       = 10     -- panel units from one small line to the next
local ENTER_Y        = 128    -- a new line slides up from the panel's bottom
local HIDDEN_Y       = ROW_TOP + LEAD_STEP + (PANEL_ROWS - 1) * ROW_STEP
                               -- a line past the last row waits here, just
                               -- under it (128: the last row ends at 126)
local LARGE_CHAR_W   = 10     -- a large character's width, allowed on the
                               -- wide side (16-unit text, about 0.6 of that)
local SCORE_GAP      = 4      -- room kept between a large name and its score
local FRAME_SECONDS  = 0.04   -- one panel frame this often while it moves
local SLIDE_TICKS    = 40     -- a line takes 0.4 s to reach its new place
local FLASH_TICKS    = 50     -- a scorer's line flashes for 0.5 s
local LABEL          = "KILLS"

-- The two words of the "Teams" setting, as scenario.settings declares them.
local FREE_FOR_ALL   = "Free For All"
local LOBBY_TEAMS    = "Use Lobby Teams"

-- The GoalHunter preset Joust's bots play with (brains/GoalHunter/
-- constants.lua PRESETS.joust): lead a boat at its real speed, turn onto the
-- target before speeding up, fire as soon as the gun is near the lead
-- point, turn or change speed out of the path of a shell, and never shoot
-- the walls, which take 255 hits here. A brain that has no such preset
-- logs it and plays on unchanged.
local BOT_PRESET     = "joust"

-- LuaJIT and Lua 5.1 name the two-argument arc tangent atan2; 5.3 and later
-- fold it into atan.
local atan2 = math.atan2 or math.atan

local target     = DEFAULT_TARGET
local team_mode  = nil         -- true for "Use Lobby Teams"; read on first use
local kills      = {}          -- side key -> kills
local own_kills  = {}          -- seat -> the kills it made itself
local last_hit   = {}          -- seat -> { by = seat, at = tick }
local dirty      = true        -- the panel needs another frame
local over       = false
local drawn_at   = nil    -- the tick panel 0 was last sent
local settled    = false       -- the round's last, settled frame is drawn
local loop_id    = nil         -- the panel frame timer, while one waits
local row_move   = {}          -- line key -> { from, to, from_at }: its slide
local flash_end  = {}          -- line key -> the tick its flash ends
local streak     = {}          -- seat -> kills since that tank last died
local started    = false       -- on_start has run
local start_at   = 0           -- game.tick() at on_start
local spawned    = {}          -- seat -> true once it has taken the field
local shield     = {}          -- seat -> game.tick() its spawn shield ends
local ring       = nil         -- the map's starts in order round the ring
local lobby_team = {}          -- seat -> its lobby team, kept at on_start or join
local ffa_team   = {}          -- seat -> the team of its own this script gave it
local team_list  = nil         -- the lobby teams in the round, lowest first,
                               -- fixed at on_start
local tuned      = {}          -- seat -> true once its bot has the preset

-- The host's "Teams" choice. game.setting answers from inside a hook, where
-- the scenario table at the bottom of the file has been read, so this is
-- asked the first time a hook needs it rather than at the top of the file.
local function teams_on()
  if team_mode == nil then
    team_mode = (game.setting("teams") == LOBBY_TEAMS)
  end
  return team_mode
end

local function in_round(p)
  local slot = game.lobby_slot(p)
  return slot ~= nil and slot.connected and slot.fielded
end

-- The seat's lobby team: the one kept for it once the round has started,
-- because a Free For All round moves every seat onto a team of its own, and
-- the one the lobby shows before that.
local function team_of(p)
  if lobby_team[p] ~= nil then
    return lobby_team[p]
  end
  local slot = game.lobby_slot(p)
  return (slot ~= nil) and slot.team or 0
end

-- The side key a seat scores for: "t<team>" in a team round when the seat is
-- on a team, "p<seat>" otherwise.
local function side_of(p)
  local team = team_of(p)
  if teams_on() and team > 0 then
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

-- A team's name on the panel and in the win line. A team with no human on
-- it is named after the pool its bots take their names from, as the lobby
-- names them ("Famous Painters"); any other team is "Team <n>". So is a
-- team whose pool another team in the round shares (a server with no lobby
-- gives every team the same pool), so no two teams read alike. A label too
-- long for a panel row is cut short.
local LABEL_MAX = 22

-- The longest line this script writes, in bytes. message and end_round take
-- 128, but an announce line with a position ("top") takes only 125, and the
-- kill and win lines go out that way. A longer one is refused and nobody
-- sees it.
local TEXT_MAX = 125

-- s cut to at most max bytes, ending in ".." when it was cut. The cut falls
-- between two whole UTF-8 characters, never inside one.
local function cut_text(s, max)
  if #s <= max then
    return s
  end
  local n = math.max(max - 2, 0)
  -- Byte n + 1 starts the part that goes. When it is a continuation byte
  -- (10xxxxxx), the character it belongs to started earlier, so step back.
  while n > 0 do
    local b = s:byte(n + 1)
    if b < 0x80 or b >= 0xC0 then
      break
    end
    n = n - 1
  end
  return s:sub(1, n) .. ".."
end

-- The pool label of a team of bots only; nil when a human is on it.
local function bot_pool(team)
  local pool, bots = nil, false
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot ~= nil and team_of(p) == team then
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

local teams_in_round

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
  return cut_text(pool, LABEL_MAX)
end

local function side_name(key)
  local kind, n = key:sub(1, 1), tonumber(key:sub(2))
  if kind == "t" then
    return team_label(n)
  end
  return name_of(n)
end

-- The lobby teams of the seats in the round, lowest first. Fixed at on_start;
-- before that, during the opening, read from the lobby as it stands.
function teams_in_round()
  if team_list ~= nil then
    return team_list
  end
  local teams, seen = {}, {}
  for q = 0, game.max_tanks() - 1 do
    local t = team_of(q)
    if t > 0 and not seen[t] and in_round(q) then
      seen[t] = true
      teams[#teams + 1] = t
    end
  end
  table.sort(teams)
  return teams
end

-- The map's starts in the order they go round the ring, by their angle about
-- the middle of all of them.
local function read_ring()
  if ring ~= nil then
    return ring
  end
  local list, sx, sy = {}, 0, 0
  for n = 1, game.num_starts() do
    local s = game.start(n)
    if s ~= nil then
      list[#list + 1] = { n = n, x = s.x, y = s.y }
      sx, sy = sx + s.x, sy + s.y
    end
  end
  if #list > 0 then
    local cx, cy = sx / #list, sy / #list
    for _, s in ipairs(list) do
      s.angle = atan2(s.y - cy, s.x - cx)
    end
    table.sort(list, function(a, b)
      if a.angle ~= b.angle then return a.angle < b.angle end
      return a.n < b.n
    end)
  end
  ring = list
  return ring
end

-- The starts a seat may use. A team round gives each team one arc of the
-- ring: with N teams, team i (lowest number first) takes the i-th N-th of it.
-- A seat on no team, and every seat in a Free For All, may use any start; its
-- list is spread round the ring so the first few seats start well apart.
local function starts_for(p)
  local all = read_ring()
  local team = team_of(p)
  if teams_on() and team > 0 then
    local teams = teams_in_round()
    for i, t in ipairs(teams) do
      if t == team then
        local from = math.floor((i - 1) * #all / #teams) + 1
        local to = math.floor(i * #all / #teams)
        local arc = {}
        for k = from, to do
          arc[#arc + 1] = all[k]
        end
        if #arc > 0 then
          return arc
        end
      end
    end
  end
  -- Spread: halve the ring again and again, so seat 0 and seat 1 face each
  -- other, seats 2 and 3 are a quarter round, and so on.
  local spread, taken, step = {}, {}, #all
  while step >= 1 and #spread < #all do
    for k = 1, #all, step do
      if not taken[k] then
        taken[k] = true
        spread[#spread + 1] = all[k]
      end
    end
    step = math.floor(step / 2)
  end
  return spread
end

-- The seat's place among the seats in the round below it: among its own
-- team in a team round, among every seat otherwise.
local function place_of(p)
  local place, team = 0, team_of(p)
  local by_team = teams_on() and team > 0
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

-- A seat's own score is its own kills. A team's score is its side's total.
local function post_seat(p)
  if game.lobby_slot(p) ~= nil then
    game.score(p, own_kills[p] or 0, LABEL)
  end
end

local function post_side(key)
  local kind, n = key:sub(1, 1), tonumber(key:sub(2))
  if kind == "t" then
    game.score({ team = n }, kills[key] or 0, LABEL)
  else
    post_seat(n)
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
  -- Ties go by the number in the key, so p2 comes before p10.
  table.sort(out, function(a, b)
    local ka, kb = kills[a] or 0, kills[b] or 0
    if ka ~= kb then return ka > kb end
    local ta, tb = a:sub(1, 1), b:sub(1, 1)
    if ta ~= tb then return ta < tb end
    return tonumber(a:sub(2)) < tonumber(b:sub(2))
  end)
  return out
end

-- The members of one team in the round, most kills of their own first.
local function members(team)
  local out = {}
  for p = 0, game.max_tanks() - 1 do
    if in_round(p) and team_of(p) == team then
      out[#out + 1] = p
    end
  end
  table.sort(out, function(a, b)
    local ka, kb = own_kills[a] or 0, own_kills[b] or 0
    if ka ~= kb then return ka > kb end
    return a < b
  end)
  return out
end

-- The panel's lines, best first. A Free For All is one line a tank. A team
-- round is a line for each team with its total, and under it a line for
-- each member with the kills they made; a tank on no team is one line of its
-- own among the teams. When that is more lines than the panel holds, the
-- members give way first, so every team's total stays on the panel.
local function panel_lines()
  local lines, heads = {}, {}
  for _, key in ipairs(standings()) do
    local kind, n = key:sub(1, 1), tonumber(key:sub(2))
    if kind == "t" then
      lines[#lines + 1] = { team = n, value = kills[key] or 0 }
      heads[#heads + 1] = lines[#lines]
      for _, p in ipairs(members(n)) do
        lines[#lines + 1] = { seat = p, value = own_kills[p] or 0,
                              indent = true }
      end
    else
      lines[#lines + 1] = { seat = n, value = kills[key] or 0 }
      heads[#heads + 1] = lines[#lines]
    end
  end
  if #lines > PANEL_ROWS then
    return heads
  end
  return lines
end

-- A panel line's key: "t<team>" for a team's line, "p<seat>" for a tank's.
-- A Free For All tank's line has the same key as its side.
local function line_key(line)
  if line.team ~= nil then
    return "t" .. line.team
  end
  return "p" .. line.seat
end

-- The panel y of row n: the leader's tall row first, then the small ones.
local function row_y(n)
  if n == 1 then
    return ROW_TOP
  end
  return ROW_TOP + LEAD_STEP + (n - 2) * ROW_STEP
end

-- A name cut to fit the leader's large row left of its score. The width is
-- counted in bytes, which is never fewer than the characters, so a name of
-- wide UTF-8 characters is cut shorter rather than running into the score.
local function fit_large(s, x, score)
  local room = 124 - #score * LARGE_CHAR_W - SCORE_GAP - x
  return cut_text(s, math.max(math.floor(room / LARGE_CHAR_W), 2))
end

-- Where a sliding line is at tick now, and whether it is still moving. The
-- slide eases out: fast at first, slowing into its place.
local function slide_y(m, now)
  local f = (now - m.from_at) / SLIDE_TICKS
  if f >= 1 then
    return m.to, false
  end
  if f <= 0 then
    return m.from, true
  end
  local e = 1 - (1 - f) ^ 3
  return m.from + (m.to - m.from) * e, true
end

-- Draws one frame of the panel. Each line slides from where it was towards
-- the place its rank gives it, and a scorer's line flashes. dirty is left
-- set while anything is still moving or flashing, so the frame timer comes
-- back; a frame with everything at rest clears it. settle puts every line
-- straight in its place with no flash: the round's last frame.
local function draw_panel(settle)
  local now = game.tick()
  local list = {
    { "rect", 0, 0, 128, 14, "grey_dark", true },
    { "text", 64, 3, "white", "normal", "centre", "JOUST" },
    { "text", 64, 18, "yellow", "small", "centre",
      string.format("First %s to %d", teams_on() and "team" or "tank",
                    target) },
  }
  local busy, seen, shown, leader = false, {}, {}, true
  local drawn = {}   -- the lines drawn: line, y, height, first item, flash
  for row, line in ipairs(panel_lines()) do
    local key = line_key(line)
    seen[key] = true
    if row > PANEL_ROWS then
      -- A line past the last row is not drawn, but it keeps a place just
      -- under the last row, so when it moves up it slides in from there.
      row_move[key] = { from = HIDDEN_Y, to = HIDDEN_Y, from_at = now }
    else
      shown[key] = true
      local to = row_y(row)
      local m = row_move[key]
      if settle then
        m = { from = to, to = to, from_at = now }
      elseif m == nil then
        m = { from = ENTER_Y, to = to, from_at = now }
      elseif m.to ~= to then
        m = { from = (slide_y(m, now)), to = to, from_at = now }
      end
      row_move[key] = m
      local y, moving = slide_y(m, now)
      y = math.floor(y + 0.5)
      busy = busy or moving

      -- The leader is the first line that is not a member's: row 1.
      local colour, size, step = "white", "small", ROW_STEP
      if line.indent then
        colour = "grey"
      elseif leader then
        colour, size, step = "cyan", "large", LEAD_STEP
      end
      if not line.indent then
        leader = false
      end
      local d = { line = line, y = y, step = step, at = #list + 1 }
      drawn[#drawn + 1] = d
      -- The flash: a bright bar behind the line, yellow and then orange,
      -- with the line in black on it.
      local ends = flash_end[key]
      if ends ~= nil and (settle or now >= ends) then
        flash_end[key] = nil
      elseif ends ~= nil then
        busy = true
        local bar = (ends - now > FLASH_TICKS / 2) and "yellow" or "orange"
        list[#list + 1] = { "rect", 2, y - 1, 124, step, bar, true }
        colour = "black"
        d.flash = true
      end
      local x = line.indent and 12 or 4
      local score = string.format("%d", line.value)
      if size == "large" then
        -- A client draws a "name" primitive as long as the name is, so the
        -- large line sends the name as text, cut to fit left of the score.
        local label = line.team ~= nil and team_label(line.team)
                      or name_of(line.seat)
        list[#list + 1] = { "text", x, y, colour, size, "left",
                            fit_large(label, x, score) }
      elseif line.team ~= nil then
        list[#list + 1] = { "text", x, y, colour, size, "left",
                            team_label(line.team) }
      else
        list[#list + 1] = { "name", x, y, colour, size, "left",
                            line.seat }
      end
      list[#list + 1] = { "text", 124, y, colour, size, "right", score }
      d.last = #list
    end
  end
  -- A line that left the list goes; if it comes back it slides in again.
  for key in pairs(row_move) do
    if not seen[key] then
      row_move[key] = nil
    end
  end
  -- A flash is only kept for a line drawn this frame. A kill also flashes
  -- keys that are not drawn (a member's line when only team lines fit, a
  -- tank past the last row); those go here rather than staying behind.
  for key in pairs(flash_end) do
    if not shown[key] then
      flash_end[key] = nil
    end
  end
  -- Each human gets their own copy, with a dark bar behind their own line,
  -- or their team's line when the members do not fit, so they can find
  -- themselves on the list. A flashing line keeps its flash. A member's
  -- grey would not show on the bar, so it is drawn white. Bots get none.
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot ~= nil and not slot.bot then
      local own = nil
      for _, d in ipairs(drawn) do
        if d.line.seat == p then
          own = d
        elseif own == nil and d.line.team ~= nil
               and d.line.team == team_of(p) then
          own = d
        end
      end
      if own ~= nil and own.flash then
        own = nil
      end
      local out = {}
      for i, item in ipairs(list) do
        if own ~= nil and i == own.at then
          out[#out + 1] = { "rect", 2, own.y - 1, 124, own.step,
                            "grey_dark", true }
        end
        if own ~= nil and i >= own.at and i <= own.last
           and item[4] == "grey" then
          item = { item[1], item[2], item[3], "white", item[5], item[6],
                   item[7] }
        end
        out[#out + 1] = item
      end
      game.panel(0, out, p)
    end
  end
  drawn_at = now
  dirty = busy
  if settle then
    settled = true
  end
end

-- Sends a panel frame when one is wanted, then comes back in FRAME_SECONDS
-- while lines are still moving. When the panel is at rest it stops, and
-- kick starts it again on the next change. A second update of the panel in
-- one tick is refused, so a frame never goes out in the tick one was drawn.
local function panel_loop()
  loop_id = nil
  if over or not dirty then
    return
  end
  if drawn_at ~= game.tick() then
    draw_panel(false)
  end
  if dirty then
    loop_id = game.timer(FRAME_SECONDS, panel_loop)
  end
end

-- The panel changed: send a frame soon. The loop starts at on_start.
local function kick()
  dirty = true
  if started and not over and loop_id == nil then
    loop_id = game.timer(FRAME_SECONDS, panel_loop)
  end
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

-- Free For All: the lowest team number no other seat in the round was given,
-- or 0 (no team) when all fifteen are taken, which only a sixteenth seat
-- ever meets.
local function free_team(p)
  local used = {}
  for q, t in pairs(ffa_team) do
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

-- Free For All: seat p goes on a team of its own. Its lobby team is kept
-- first, for team_of and to be put back at the end.
local function split_off(p)
  local slot = game.lobby_slot(p)
  if slot == nil then
    return
  end
  if lobby_team[p] == nil then
    lobby_team[p] = slot.team
  end
  local t = ffa_team[p] or free_team(p)
  if slot.team ~= t and game.set_team(p, t) then
    ffa_team[p] = t
  elseif slot.team == t then
    ffa_team[p] = t
  end
end

-- Free For All: every seat back on the lobby team it came with, so the next
-- lobby shows the teams the host set up. A roster write needs a running
-- round, so this is done just before the round is ended, not in on_end.
local function restore_teams()
  if teams_on() then
    return
  end
  -- Seat order, not pairs(), so a seed gives the same ops in the same order.
  for p = 0, game.max_tanks() - 1 do
    local t = lobby_team[p]
    local slot = (t ~= nil) and game.lobby_slot(p) or nil
    if slot ~= nil and slot.team ~= t then
      game.set_team(p, t)
    end
  end
end

local function finish(key)
  if over then
    return
  end
  over = true
  local tail = string.format(" wins the joust with %d kills.", kills[key])
  local line = cut_text(side_name(key), TEXT_MAX - #tail) .. tail
  game.message(line)
  game.announce(line, 5, nil, "top")
  local kind, n = key:sub(1, 1), tonumber(key:sub(2))
  local function close()
    restore_teams()
    if kind == "t" then
      game.end_round(line, n)
    else
      game.end_round(line)
    end
  end
  -- The last frame shows the final standings, every line in its place and
  -- none flashing. When the panel was already drawn this tick that draw
  -- would be refused, so the round ends one tick later, after the last
  -- draw. Nothing scores once over is set.
  if drawn_at == game.tick() then
    game.timer(0.01, function()
      draw_panel(true)
      close()
    end)
  else
    draw_panel(true)
    close()
  end
end

-- What a streak of n kills is called, louder the longer it runs; nil for a
-- single kill.
local STREAK_WORDS = {
  [2] = "Double kill",
  [3] = "Triple kill!",
  [4] = "QUADRUPLE KILL!",
  [5] = "QUINTUPLE KILL!!",
  [6] = "SEXTUPLE KILL!!!",
}

local function streak_word(n)
  if n < 2 then
    return nil
  end
  return STREAK_WORDS[n] or
         ("UNSTOPPABLE" .. string.rep("!", math.min(n - 3, 10)))
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

-- The words a kill line uses, three for each way to die. A kill takes the
-- next one in turn, by the killer's side's new score, so the lines vary and a
-- replay shows the same words.
local DROWN_VERBS = { "drowned", "sank", "dunked" }
local KILL_VERBS  = { "killed", "destroyed", "blew up" }

-- The announce line for a kill, never longer than TEXT_MAX. The full line is
--   "<killer> (<kills>/<target>): <streak word>  <verb> <victim> (<kills>/<target>)"
-- with each score the one of that tank's side, and with ": <streak word>  "
-- shortened to a space for a kill that is not on a streak. When long names
-- make it too long, the victim's name is cut first and then the killer's.
-- The streak word and both scores always stay.
local function kill_line(by, victim, key, cause, word)
  local verbs = (cause == "deep_sea") and DROWN_VERBS or KILL_VERBS
  local how = verbs[(kills[key] - 1) % #verbs + 1]
  local vkey = side_of(victim)
  local mine = string.format(" (%d/%d)", kills[key], target)
  local theirs = string.format(" (%d/%d)", kills[vkey] or 0, target)
  local lead = (word ~= nil) and (": " .. word .. "  ") or " "
  local fixed = mine .. lead .. how .. " " .. theirs
  local killer, prey = name_of(by), name_of(victim)
  local room = TEXT_MAX - #fixed
  if #killer + #prey > room then
    prey = cut_text(prey, math.max(room - #killer, 8))
  end
  if #killer + #prey > room then
    killer = cut_text(killer, math.max(room - #prey, 0))
  end
  return killer .. mine .. lead .. how .. " " .. prey .. theirs
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
  streak[victim] = nil        -- any death ends the tank's streak
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
  own_kills[by] = (own_kills[by] or 0) + 1
  kills[key] = (kills[key] or 0) + 1
  post_seat(by)
  post_side(key)
  local now = game.tick()
  flash_end["p" .. by] = now + FLASH_TICKS
  flash_end[key] = now + FLASH_TICKS
  kick()

  -- The streak: every kill since the tank last died counts, with no time
  -- limit between them. A death clears it (see above).
  local run = (streak[by] or 0) + 1
  streak[by] = run

  -- One announce line holds the kill and, on a streak, the streak word in
  -- front of it, so the streak never hides who was killed or the score.
  game.announce(kill_line(by, victim, key, cause, streak_word(run)), 3,
                nil, "top")
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
  local list = starts_for(p)
  if #list == 0 then
    return nil
  end
  -- The opening: one start for each place, so no two tanks share a square.
  -- The opening tanks are placed one at a time, and the lobby may not show
  -- every seat and team yet, so the list is read again for each of them.
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

-- A bot seat gets BOT_PRESET once, the first time its tank comes on: a
-- seat that has been fielded has a brain to take it. A human seat is left
-- alone.
local function tune_bot(p)
  if tuned[p] then
    return
  end
  local slot = game.lobby_slot(p)
  if slot == nil or not slot.bot then
    return
  end
  tuned[p] = true
  game.bot_init(p, { preset = BOT_PRESET })
end

-- A tank arrives on a start out in the deep sea, on a boat.
function on_tank_spawned(p, mx, my, respawn, scripted)
  spawned[p] = true
  tune_bot(p)
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

-- Alliances follow the Teams setting: none in a Free For All, and only
-- between members of the same lobby team in a team round.
function can_ally(p, q)
  if not teams_on() then
    return false
  end
  local tp, tq = team_of(p), team_of(q)
  return tp > 0 and tp == tq
end

-- The teams are the round's. A seat that changes team itself is put back:
-- on its own team in a Free For All, on the lobby team it started on in a
-- team round. A move this script made arrives here too and is left alone.
function on_team_changed(p, team, scripted)
  if scripted or not started or over then
    return
  end
  local want = teams_on() and lobby_team[p] or ffa_team[p]
  if want ~= nil and team ~= want then
    game.set_team(p, want)
  end
end

function on_player_join(p, scripted)
  if over then
    return
  end
  -- A seat on_start already recorded is not new: a bot fielded after the
  -- start arrives here too, and by then a Free For All has moved it off
  -- its lobby team, so recording its team again would lose that team.
  if started and lobby_team[p] ~= nil then
    post_seat(p)
    kick()
    return
  end
  own_kills[p] = 0
  if started then
    local slot = game.lobby_slot(p)
    lobby_team[p] = (slot ~= nil) and slot.team or 0
    if not teams_on() then
      ffa_team[p] = nil
      split_off(p)
    elseif lobby_team[p] > 0 then
      -- A team that was not in the round at the start gets no arc of its
      -- own; its members use the whole ring, as a seat on no team does.
      post_side(side_of(p))
    end
  end
  post_seat(p)
  kick()
end

function on_player_leave(p, scripted)
  last_hit[p] = nil
  tuned[p] = nil              -- the seat's next bot has a brain of its own
  spawned[p] = nil
  shield[p] = nil
  own_kills[p] = nil
  lobby_team[p] = nil
  ffa_team[p] = nil
  kills["p" .. p] = nil       -- the seat's next player starts at nought
  streak[p] = nil
  flash_end["p" .. p] = nil
  kick()
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

  -- Every seat's lobby team is kept now. In a team round the teams in play
  -- are fixed here too; in a Free For All every seat goes on a team of its
  -- own. Roster writes are refused in on_setup, which is why this is here.
  for p = 0, game.max_tanks() - 1 do
    local slot = game.lobby_slot(p)
    if slot ~= nil then
      lobby_team[p] = slot.team
    end
  end
  if teams_on() then
    team_list = nil
    team_list = teams_in_round()
  else
    for p = 0, game.max_tanks() - 1 do
      split_off(p)
    end
  end
  started = true
  start_at = game.tick()

  for p = 0, game.max_tanks() - 1 do
    if game.lobby_slot(p) ~= nil then
      own_kills[p] = own_kills[p] or 0
      post_seat(p)
    end
  end
  for _, key in ipairs(standings()) do
    post_side(key)
  end
  game.message(string.format("Joust: first %s to %d kills wins. %s",
                             teams_on() and "team" or "tank", target,
                             "One hit sinks a boat."))
  game.log(string.format("Joust: target %d, %s", target,
                         teams_on() and "lobby teams" or "free for all"))
  kick()
  game.timer(REFILL_SECONDS, refill_loop)
end

function on_end()
  over = true
  -- A round that ends any other way than finish (the time limit, the host)
  -- also gets the last frame, every line in its place and none flashing.
  -- A second panel update in one tick is refused, so when a frame already
  -- went out this tick that frame stays.
  if not settled and drawn_at ~= game.tick() then
    draw_panel(true)
  end
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
                "boat, so one hit drowns it. First to the kill count wins, " ..
                "every tank for itself or as lobby teams.",
  api         = 1,
  author      = "WinBolo",
  updated     = "2026-10-02T15:08Z",
  game        = "open",

  -- The script cuts the map's own starts into arcs of the ring.
  bound       = true,

  settings = {
    { id = "kills_to_win", label = "Kills to win", type = "int",
      min = 1, max = 50, step = 1, default = DEFAULT_TARGET },
    { id = "teams", label = "Teams", type = "choice",
      choices = { FREE_FOR_ALL, LOBBY_TEAMS }, default = FREE_FOR_ALL },
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
    on_start = "Reads the kill target and the Teams setting; in Free For All puts every tank on a team of its own; posts the scores and starts the shell refill.",
    on_choose_start = "Picks each tank's start: one for each place at the opening, then the start farthest from every enemy, on its own team's arc of the ring in a team round.",
    on_tank_hit = "Remembers the last enemy tank that hit each tank.",
    on_tank_killed = "Credits the kill (a drowning to the last enemy hit), adds it to the side, flashes the scorer's line, calls out streaks, and ends the round at the target.",
    on_tank_spawned = "Makes sure a new tank is on its boat, starts its one-second shield, and gives a bot the Joust preset the first time it comes on.",
    can_hit = "Lets shells pass through a tank in its first second, so nobody is sunk as they arrive.",
    can_ally = "No alliances in Free For All; only lobby teammates in a team round.",
    on_team_changed = "Teams are fixed for the round.",
    on_player_join = "Shows the new player's score; in Free For All puts them on a team of their own.",
    on_player_leave = "Clears a leaving player's own kills, streak and flash.",
    can_build = "Nothing can be built, so the arena stays as drawn.",
    allow_base_win = "Only kills win; the map has no bases.",
    on_end = "Draws the final scoreboard when the round ended some other way than a win, and writes the final standings to the server log.",
  },
}
