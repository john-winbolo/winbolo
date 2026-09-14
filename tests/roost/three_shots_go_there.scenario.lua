-- ROOST: three shells on one square are an order, and a bot obeys it.
--
-- Three of a player's shells that run their FULL range — nothing hit — and
-- land on the same open square inside two seconds mean "go there and hold".
-- The server spots the pattern in the shell-expiry path and puts one
-- "!goto <mx> <my>" line into every allied bot's inbox, with the SHOOTER as
-- the sender; the brains hold the auction and the nearest bot within ten
-- squares takes it.
--
-- What drives the shots is game.shell_expired, a test hook: a script cannot
-- make a seat pull a trigger, and steering a round into three full-range
-- shells landing on one chosen square is not something a test could read
-- afterwards. The hook posts the expiry notice and nothing else — no
-- explosion, no sound, no shell — so what is under test is the detector and
-- the brain on the other end of it, which is all this round can see anyway.
--
-- Two GoalHunter bots on one team. The SHOOTER is the one that fires:
-- a sender never receives its own line, so the LISTENER is the only bot
-- that hears the order and the round has exactly one bot that could obey.
--
-- The verdict is read off the listener's position. It must get within two
-- squares of the target and still be near it three seconds later, which is
-- the "and hold" half of the order.

scenario = {
  name        = "ROOST three_shots_go_there",
  description = "Three shells on one square order a bot there, and it goes.",
  api         = 1,
}

local LISTENER = 0    -- hears the order, and is the only one who can
local SHOOTER  = 1    -- fires the three shells, never hears its own line

local FIRE_AT    = 600    -- game.tick units: 100 a second, so 6 s in
local DEADLINE   = 1500   -- ticks after the order to arrive in
local HOLD_TICKS = 300    -- and to still be there after

local ARRIVE_AT  = 2      -- squares: "got there"
local HOLD_AT    = 3      -- squares: "stayed there"
local MIN_AWAY   = 4      -- the target has to be a trip, not where it stands
local MAX_AWAY   = 9      -- and inside the ten-square rule the bots apply

-- The squares an order may be dropped on that a tank can also drive over.
-- The server's own list is wider (it takes river, shallow water and deep
-- sea too), and a test that ordered a bot into the sea would prove nothing.
local DRIVABLE = nil      -- built in on_start, once game.TERRAIN is there

local tx, ty     = nil, nil
local fired      = false
local fired_at   = nil
local start_away = nil
local arrived_at = nil
local done       = false

-- A verdict is one short line. Both game.log and game.end_round refuse text
-- of 129 bytes or more (SCN_TEXT_MAX), and a refusal is a return value, not
-- an error — so a long verdict leaves the round saying nothing at all.
local function finish(text)
  if done then return end
  done = true
  -- The log line is what the runner reads: the round's own end text goes to
  -- the lobby, and a headless round has nobody in one.
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

local function away(p, mx, my)
  local tk = game.tank(p)
  if not tk then return nil end
  local dx, dy = tk.mx - mx, tk.my - my
  if dx < 0 then dx = -dx end
  if dy < 0 then dy = -dy end
  return (dx > dy) and dx or dy
end

-- The square the shells will land on: the nearest drivable one to the
-- listener that is a real trip away, searched in a fixed order so the round
-- picks the same square every time.
local function pick_target()
  local me = game.tank(LISTENER)
  if not me then return nil, nil end
  for r = MIN_AWAY, MAX_AWAY do
    for dy = -r, r do
      for dx = -r, r do
        local far = (dx > dy) and dx or dy
        if far < 0 then far = -far end
        if (dx == r or dx == -r or dy == r or dy == -r) then
          local x, y = me.mx + dx, me.my + dy
          if x >= 0 and x <= 255 and y >= 0 and y <= 255 then
            local t = game.map_tile(x, y)
            if t and DRIVABLE[t] then return x, y end
          end
        end
      end
    end
  end
  return nil, nil
end

function on_start()
  DRIVABLE = {
    [game.TERRAIN.grass]      = true,
    [game.TERRAIN.road]       = true,
    [game.TERRAIN.swamp]      = true,
    [game.TERRAIN.crater]     = true,
    [game.TERRAIN.rubble]     = true,
    [game.TERRAIN.mine_grass] = true,
    [game.TERRAIN.mine_road]  = true,
  }
  if not game.tank(LISTENER) or not game.tank(SHOOTER) then
    finish("FAIL three_shots_go_there: the round needs two seated bots")
  end
end

function on_tick(t)
  if done then return end

  if not fired and t >= FIRE_AT then
    fired = true
    tx, ty = pick_target()
    if not tx then
      finish("FAIL three_shots_go_there: no drivable square "
             .. MIN_AWAY .. "-" .. MAX_AWAY .. " squares from the bot")
      return
    end
    start_away = away(LISTENER, tx, ty)
    -- Three shells, one square, one tick: well inside the two-second window
    -- the detector measures.
    for i = 1, 3 do
      local ok, code, detail = game.shell_expired(SHOOTER, tx, ty)
      if not ok then
        finish(string.format(
          "FAIL three_shots_go_there: shell %d was refused %s (%s)",
          i, tostring(code), tostring(detail)))
        return
      end
    end
    fired_at = t
    game.log(string.format(
      "three_shots_go_there: three shells on (%d, %d), the bot is %d away",
      tx, ty, start_away or -1))
    return
  end

  if not fired_at then return end

  local d = away(LISTENER, tx, ty)
  if not d then
    finish("FAIL three_shots_go_there: the listener lost its tank")
    return
  end

  if not arrived_at then
    if d <= ARRIVE_AT then
      arrived_at = t
      game.log(string.format(
        "three_shots_go_there: arrived after %d ticks", t - fired_at))
    elseif (t - fired_at) >= DEADLINE then
      finish(string.format(
        "FAIL three_shots_go_there: %d ticks on, still %d from (%d,%d); "
        .. "it was %d at the order",
        DEADLINE, d, tx, ty, start_away or -1))
    end
    return
  end

  -- Arrived. The other half of the order is holding there.
  if (t - arrived_at) >= HOLD_TICKS then
    if d <= HOLD_AT then
      finish(string.format(
        "PASS three_shots_go_there: %d away at the order, on (%d,%d) in "
        .. "%d ticks, %d away %d later",
        start_away or -1, tx, ty, arrived_at - fired_at, d, HOLD_TICKS))
    else
      finish(string.format(
        "FAIL three_shots_go_there: reached (%d,%d) in %d ticks, then %d "
        .. "away %d later",
        tx, ty, arrived_at - fired_at, d, HOLD_TICKS))
    end
  end
end
