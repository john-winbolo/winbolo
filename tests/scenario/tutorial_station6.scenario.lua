-- GATE: ticks=16000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the Station 6 bot and its man.
--
-- Seat 0 plays the tutorial's player, held still. At tick 300 the arena puts
-- it on the road in Station 6, which fields the Station 6 bot. The arena
-- then watches the bot for two and a half minutes without shooting: the
-- seat is a GoalHunter bot, so the arena keeps its shells at 0 (after the
-- 5B blocker moved back to 117,102, the seat drove north and shot the
-- Station 6 man once, which broke the "same trips" check).
--
-- Each tick it reads how far the bot's tank is from the centre of its
-- parking square, both before the tutorial's own tick (what one engine
-- step moved it) and after (what the player sees). It logs every square the
-- man stands on, trip by trip; a trip runs from the man leaving the tank
-- to his getting back in.
--
-- PASS:
--   * the tank is on the parking square's centre after every tick, and one
--     engine step never moves it more than 16 world units;
--   * every square the man stands on is the tree square or on the path row
--     (path6), so all of it is west of the main road's east edge;
--   * there are at least three whole trips, each crosses the craters, and
--     every trip walks the same squares in the same order (which also shows
--     the one tree is put back each time).

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, trips = {}, cur = nil, pre = 0, post = 0, off = 0,
          samples = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local function deviation()
  local t = S.bot6 and game.tank(S.bot6)
  if t == nil or t.dead then return nil end
  local at = LAYOUT.point.bot6_park
  return math.max(math.abs(t.wx - (at[1] * 256 + 128)),
                  math.abs(t.wy - (at[2] * 256 + 128)))
end

local function on_path(x, y)
  local tr = LAYOUT.point.tree6
  if x == tr[1] and y == tr[2] then return true end
  local r = LAYOUT.region.path6
  return y == r.y and x >= r.x and x < r.x + r.w
end

local function crosses_craters(trip)
  local c = LAYOUT.region.craters6
  for _, sq in ipairs(trip) do
    if sq[2] == c.y and sq[1] >= c.x and sq[1] < c.x + c.w then return true end
  end
  return false
end

local function trip_text(trip)
  local parts = {}
  for _, sq in ipairs(trip) do parts[#parts + 1] = sq[1] .. "," .. sq[2] end
  return table.concat(parts, " ")
end

local arena_real_tick = on_tick
function on_tick(tick)
  local A = ARENA
  if A.phase == 1 and S.fielded.bot6 then
    local d = deviation()
    if d ~= nil and d > A.pre then A.pre = d end
  end
  arena_real_tick(tick)
  local me = game.tank(0)
  if me ~= nil and not me.dead and me.shells > 0 then
    game.set_stocks(0, { shells = 0 })
  end
  if A.phase == 0 and tick >= 300 then
    game.teleport(0, 126, 86, 0)           -- Station 6, on the road
    A.phase, A.t = 1, tick
    return
  end
  if A.phase ~= 1 or not S.fielded.bot6 then return end
  local d = deviation()
  if d ~= nil then
    A.samples = A.samples + 1
    if d > A.post then A.post = d end
  end
  local b = game.builder(S.bot6)
  if b ~= nil then
    if b.state == "in_tank" then
      if A.cur ~= nil then
        A.trips[#A.trips + 1] = A.cur
        game.log(string.format("ARENA trip %d: %s", #A.trips, trip_text(A.cur)))
        A.cur = nil
      end
    elseif b.state == "going" or b.state == "returning" then
      A.cur = A.cur or {}
      local last = A.cur[#A.cur]
      if last == nil or last[1] ~= b.mx or last[2] ~= b.my then
        A.cur[#A.cur + 1] = { b.mx, b.my }
        if not on_path(b.mx, b.my) then
          A.off = A.off + 1
          game.log(string.format("ARENA man off the path at %d,%d", b.mx, b.my))
        end
      end
    end
  end
  if tick >= A.t + 15000 and not A.done then
    A.done = true
    local same, craters = true, true
    local first = A.trips[1] and trip_text(A.trips[1])
    for _, trip in ipairs(A.trips) do
      if trip_text(trip) ~= first then same = false end
      if not crosses_craters(trip) then craters = false end
    end
    local ok = A.samples > 0 and A.post == 0 and A.pre <= 16 and A.off == 0 and
               #A.trips >= 3 and same and craters
    verdict(ok, string.format("tank off centre: %d wu per step at most, %d "
      .. "after the tick (%d samples); %d trips, all the same %s, all over "
      .. "the craters %s; %d squares off the path",
      A.pre, A.post, A.samples, #A.trips, tostring(same), tostring(craters),
      A.off))
  end
end
