-- ROOST: three shells on one square are an order, and a bot obeys it.
--
-- Three of a player's shells that run their FULL range — nothing hit — and
-- land on the same open square mean "go there and hold", but only when the
-- player meant them: the three have to be FIRED inside two seconds of each
-- other, with a quiet second in front of the first and a quiet second after
-- the third. The server spots the pattern in the shell-expiry path, waits
-- out that second, and then puts one "!goto <mx> <my> <sx> <sy>" line into
-- every allied bot's inbox with the SHOOTER as the sender; the brains hold
-- the auction and a bot inside the shooter's view takes it.
--
-- What drives the shots is game.shell_expired, a test hook: a script cannot
-- make a seat pull a trigger, and steering a round into three full-range
-- shells landing on one chosen square is not something a test could read
-- afterwards. The hook posts the expiry notice and nothing else — no
-- explosion, no sound, no shell — so what is under test is the detector and
-- the brain on the other end of it, which is all this round can see anyway.
--
-- The hook's fourth argument is the tick the shell LEFT THE GUN, and every
-- timing rule reads that tick rather than the landing. Here each shell is
-- posted on the tick it was fired, which is the plainest shape that still
-- lets the round place a shot inside or outside a quiet second.
--
-- Two GoalHunter bots on one team. The SHOOTER is the one that fires: a
-- sender never receives its own line, so the LISTENER is the only bot that
-- hears the order and the round has exactly one bot that could obey. The
-- shooter is put beside the listener before each burst, because the range
-- rule is the shooter's own 29x29 view and two bots left to themselves
-- drift out of each other's sight.
--
-- The round runs two phases, in this order because the second needs the
-- first: an ordered bot is standing on open ground when phase TWO starts,
-- and a bot left to itself wanders into the sea, where there is nowhere to
-- stand the shooter.
--
--   PHASE ONE, the positive. Three clean shells on one square. The listener
--   must not answer before the quiet second after the third shot is up, and
--   it must then get within two squares of the target and still be near it
--   three seconds later, which is the "and hold" half of the order.
--
--   PHASE TWO, the negative. Three shells on a fresh square, and then a
--   FOURTH fired half a second after the third. That fourth shot takes the
--   order away before it is ever sent, so the listener must not answer.
--
-- The answer is read off the chat ack a bot says when it TAKES an order:
-- a "goto" line from the listener is the order being obeyed, and it is the
-- only line in this round that can carry that word.  (It said "take_cover"
-- until Sep 15, when a place order stopped being a take_cover pinned to the
-- square and became goto_tile, a hard lock outside the goal pools.)

scenario = {
  name        = "ROOST three_shots_go_there",
  description = "Three shells on one square order a bot there, and it goes.",
  api         = 1,
}

local LISTENER = 0    -- hears the order, and is the only one who can
local SHOOTER  = 1    -- fires the shells, never hears its own line

-- The server's own numbers, which this round has to agree with.
local QUIET      = 100    -- SHOT_ORDER_QUIET_TICKS: the second either side
local GAP        = 40     -- between one shell of a burst and the next

local FIRE_AT    = 400    -- game.tick units: 100 a second, so 4 s in
local DEADLINE   = 1500   -- ticks after the order to arrive in
local HOLD_TICKS = 250    -- and to still be there after
local SETTLE     = 100    -- a clear gap between the two phases
local WATCH      = 300    -- how long the cancelled order is watched for.
                          -- Phase one's ack came 22 ticks after its order
                          -- went out, so 300 is ample, and the whole round
                          -- has to be over inside the server's own limit

local ARRIVE_AT  = 2      -- squares: "got there"
local HOLD_AT    = 3      -- squares: "stayed there"
local MIN_AWAY   = 4      -- the target has to be a trip, not where it stands
local MAX_AWAY   = 9      -- and a trip a bot can finish inside the deadline
local VIEW       = 12     -- the shooter is moved only when it is further off

-- The squares an order may be dropped on that a tank can also drive over.
-- The server's own list is wider (it takes river, shallow water and deep
-- sea too), and a test that ordered a bot into the sea would prove nothing.
local DRIVABLE = nil      -- built in on_start, once game.TERRAIN is there

local now      = 0        -- the tick on_chat reads, kept by on_tick
local phase    = "wait_one"
local tx, ty   = nil, nil
local burst    = { n = 0, left = 0 }  -- when this phase's shells go out
local fired_at = nil      -- the fire tick of the THIRD shell of the burst
local ack_at   = nil      -- when the listener answered this phase
local watch_from          -- when phase two started watching for an answer
local start_away, arrived_at
local why_no_shooter = "no drivable square beside the bot"
local done     = false

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

-- Squares apart the way a screen is: the larger of the two axes, which is
-- the distance the range rule is written in.
local function away_tiles(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  if dx < 0 then dx = -dx end
  if dy < 0 then dy = -dy end
  return (dx > dy) and dx or dy
end

local function away(p, mx, my)
  local tk = game.tank(p)
  if not tk then return nil end
  return away_tiles(tk.mx, tk.my, mx, my)
end

-- The first drivable square on the ring `r` out from (ox, oy), scanned in a
-- fixed order so the round picks the same square every time.
local function ring_square(ox, oy, r)
  for dy = -r, r do
    for dx = -r, r do
      if dx == r or dx == -r or dy == r or dy == -r then
        local x, y = ox + dx, oy + dy
        if x >= 0 and x <= 255 and y >= 0 and y <= 255 then
          local t = game.map_tile(x, y)
          if t and DRIVABLE[t] then return x, y end
        end
      end
    end
  end
  return nil, nil
end

-- The square the shells will land on: the nearest drivable one to the
-- listener that is a real trip away.
local function pick_target()
  local me = game.tank(LISTENER)
  if not me then return nil, nil end
  for r = MIN_AWAY, MAX_AWAY do
    local x, y = ring_square(me.mx, me.my, r)
    if x then return x, y end
  end
  return nil, nil
end

-- The range rule is the SHOOTER's 29x29 view, so the shooter has to be
-- beside the listener when it fires. Two bots playing their own game drift
-- apart, and a round that fired from wherever the shooter happened to be
-- would be testing the map rather than the rule.
local function place_shooter()
  local me  = game.tank(LISTENER)
  local him = game.tank(SHOOTER)
  if not me or not him then
    why_no_shooter = "one of the two bots has no tank"
    return false
  end
  if away_tiles(me.mx, me.my, him.mx, him.my) <= VIEW then return true end
  for r = 2, MAX_AWAY do
    local x, y = ring_square(me.mx, me.my, r)
    if x then
      local ok, code, detail = game.teleport(SHOOTER, x, y)
      if ok then return true end
      why_no_shooter = string.format("(%d,%d) %s %s", x, y,
                                     tostring(code), tostring(detail))
    end
  end
  return false
end

-- One burst: three shells on one square, each posted on the tick it was
-- fired, GAP ticks apart. `extra` adds a fourth shot that many ticks after
-- the third, which is what takes an order away again.
local function plan_burst(t, extra)
  burst = { n = 3, t, t + GAP, t + 2 * GAP }
  fired_at = burst[3]
  if extra then
    burst.n = 4
    burst[4] = fired_at + extra
  end
  burst.left = burst.n
end

-- Every shell whose fire tick has come round. Each is posted ON that tick,
-- so the tick the hook is told the shell was fired on is the tick it hears
-- about it — the plainest shape a burst can have.
local function post_due()
  for i = 1, burst.n do
    if burst[i] and now >= burst[i] then
      local ok, code, detail = game.shell_expired(SHOOTER, tx, ty, burst[i])
      burst[i] = nil
      burst.left = burst.left - 1
      if not ok then
        finish(string.format(
          "FAIL three_shots_go_there: shell %d was refused %s (%s)",
          i, tostring(code), tostring(detail)))
        return
      end
    end
  end
end

-- Aim a phase: put the shooter in view, choose a square, and set the burst
-- going. Answers false when it has already said why it could not.
local function open_phase(t, extra)
  if not place_shooter() then
    finish("FAIL three_shots_go_there: shooter: " .. why_no_shooter)
    return false
  end
  tx, ty = pick_target()
  if not tx then
    finish("FAIL three_shots_go_there: no drivable square "
           .. MIN_AWAY .. "-" .. MAX_AWAY .. " squares from the bot")
    return false
  end
  ack_at = nil
  arrived_at = nil
  start_away = away(LISTENER, tx, ty)
  plan_burst(t, extra)
  return true
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

-- A bot says one line when it TAKES an order, and "goto" is the goal a "go
-- there and hold" order runs. No other line in this round carries that word,
-- so this is the order being obeyed and nothing else.
function on_chat(p, text, scripted)
  if scripted or done or text:sub(1, 1) == "/" then return end
  if p ~= LISTENER or ack_at then return end
  if text:find("goto", 1, true) then
    ack_at = now
    game.log(string.format("three_shots_go_there: %s ack at %d (+%d)",
                           phase, now, now - (fired_at or now)))
  end
end

function on_tick(t)
  if done then return end
  now = t

  -- ── PHASE ONE: three clean shells, and the bot goes ───────────────────
  if phase == "wait_one" then
    if t < FIRE_AT then return end
    if not open_phase(t, nil) then return end
    phase = "one"
    game.log(string.format(
      "three_shots_go_there: phase one on (%d, %d), the bot is %d away",
      tx, ty, start_away or -1))
    return
  end

  if phase == "one" then
    post_due()
    if done then return end
    if burst.left > 0 then return end        -- the burst is still going out

    -- The order is sent a quiet second after the THIRD shot was fired, so
    -- an answer before then would mean the server never waited.
    if ack_at and ack_at < fired_at + QUIET then
      finish(string.format(
        "FAIL three_shots_go_there: answered %d ticks after the third shot; "
        .. "the quiet second is %d", ack_at - fired_at, QUIET))
      return
    end

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
          .. "it was %d", DEADLINE, d, tx, ty, start_away or -1))
      end
      return
    end

    -- Arrived. The other half of the order is holding there.
    if (t - arrived_at) >= HOLD_TICKS then
      if d > HOLD_AT then
        finish(string.format(
          "FAIL three_shots_go_there: reached (%d,%d) in %d ticks, then %d "
          .. "away %d later",
          tx, ty, arrived_at - fired_at, d, HOLD_TICKS))
        return
      end
      game.log(string.format(
        "three_shots_go_there: phase one done, there in %d ticks and %d "
        .. "away %d later", arrived_at - fired_at, d, HOLD_TICKS))
      watch_from = t + SETTLE
      phase = "wait_two"
    end
    return
  end

  -- ── PHASE TWO: a fourth shot takes the order back ─────────────────────
  if phase == "wait_two" then
    if t < watch_from then return end
    local took = arrived_at and (arrived_at - fired_at) or -1
    if not open_phase(t, QUIET / 2) then return end
    phase = "two"
    watch_from = t
    arrived_at = took          -- kept for the verdict line
    game.log(string.format(
      "three_shots_go_there: phase two at %d on (%d, %d), a fourth shot %d "
      .. "ticks after the third", t, tx, ty, QUIET / 2))
    return
  end

  post_due()
  if done then return end
  if ack_at then
    finish(string.format(
      "FAIL three_shots_go_there: a fourth shot %d ticks on left the order "
      .. "standing; answered at %d", QUIET / 2, ack_at))
    return
  end
  if (t - watch_from) >= WATCH then
    finish(string.format(
      "PASS three_shots_go_there: there in %d ticks; a cancelled burst then "
      .. "said nothing for %d", arrived_at or -1, WATCH))
  end
end
