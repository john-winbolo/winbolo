-- ROOST: a place order is a HARD LOCK — the bot goes, and then it STAYS.
--
-- "Go there and hold" (a ping on open ground, or three shells on one square)
-- used to run as a take_cover pinned to that square, which left it bidding
-- inside the goal pools with every other goal still competing: bots reached
-- the square and then drove off to a pillbox, or never got there at all.
-- Andrew, Sep 15: a place order is an instruction, so it now runs as a
-- command goal (kind goto_tile), the same slot the old `!pill:N` line used.
-- goals.pick_goal answers a command goal BEFORE goal selection, so while the
-- order stands the bot picks no goals of its own at all.
--
-- What this round reads is the HOLD, which is the half a test can see from
-- outside and the half that was broken:
--
--   1. GO. Three shells land on a square eight away and the bot gets within
--      two of it inside six hundred ticks.
--   2. HOLD. Fifteen hundred ticks after the order it is STILL within two.
--      That is the assertion: a bot running the old take_cover version had
--      the whole pool bidding underneath it and a nine hundred tick window
--      to be pulled away by anything the map offered. This arena offers two
--      neutral bases, which capture_base wants badly.
--   3. LET GO. `!cancel all` ends the order, and the bot must be more than
--      three squares off the square inside six hundred ticks — proof the
--      lock was the order and not the tank simply having stopped.
--
-- The shells come from game.shell_expired, the same test hook
-- three_shots_go_there uses: a script cannot make a seat pull a trigger. The
-- hook posts the expiry and nothing else, and its fourth argument is the tick
-- the shell LEFT THE GUN, which is what every timing rule reads.
--
-- Two seats, both bots, on one team. The SHOOTER fires and says the cancel;
-- a sender never hears its own line, so the LISTENER is the only bot that can
-- take the order and the only one the round has to watch. Both are teleported
-- onto named squares on the grass island the order_* rounds share, right
-- before the burst, because the range rule is the shooter's own screen and
-- two bots left alone drift apart.

scenario = {
  name        = "ROOST order_goto_holds",
  description = "A place order is a hard lock: the bot goes, holds, and only a cancel frees it.",
  api         = 1,
}

local LISTENER = 0        -- hears the order; the only bot that can obey
local SHOOTER  = 1        -- fires the shells and says the cancel

-- The island runs (96,96) to (159,159) and is flat grass, so these are all
-- drivable and eight squares apart on one row.
local LX, LY   = 120, 128   -- where the listener starts the trip
local SX, SY   = 122, 128   -- the shooter, two squares away, well in view
local TX, TY   = 128, 128   -- the square the shells land on

local QUIET      = 100    -- SHOT_ORDER_QUIET_TICKS: the order goes out this
                          -- long after the THIRD shot was fired
local GAP        = 40     -- between one shell of the burst and the next

local SETUP_AT   = 150    -- stocks and builders
local FIRE_AT    = 300    -- the first shell, and the teleport that aims it
local ARRIVE_BY  = 600    -- ticks after the third shot to get there in
local HOLD_UNTIL = 1500   -- and to still be there at
local CANCEL_GAP = 50     -- after the hold check, the cancel
local FREE_BY    = 600    -- ticks after the cancel to have left by

local ARRIVE_AT  = 2      -- squares: "got there", and "still there"
local FREE_AT    = 3      -- squares: "left"
local MIN_AWAY   = 4      -- the trip has to be a trip

local now        = 0
local fired_at   = nil    -- the fire tick of the third shell
local burst      = {}
local start_away = nil
local arrived_at = nil
local held_ok    = nil    -- how far off it was at the hold check
local cancel_at  = nil
local done       = false

-- A verdict is one short line: game.log and game.end_round both refuse text
-- of 129 bytes or more, and a refusal is a return value, not an error.
local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

-- Squares apart the way a screen is: the larger of the two axes, which is
-- the distance the range rule is written in.
local function away(p, mx, my)
  local tk = game.tank(p)
  if not tk then return nil end
  local dx, dy = tk.mx - mx, tk.my - my
  if dx < 0 then dx = -dx end
  if dy < 0 then dy = -dy end
  return (dx > dy) and dx or dy
end

-- Both seats onto their squares. Done again at the burst, because the range
-- rule is the shooter's own view and 150 ticks is long enough to drift.
local function place()
  local a = game.teleport(LISTENER, LX, LY)
  local b = game.teleport(SHOOTER, SX, SY)
  return a and b
end

function on_start()
  if not game.tank(LISTENER) or not game.tank(SHOOTER) then
    finish("FAIL order_goto_holds: the round needs two seated bots")
    return
  end
  -- The three squares this round is written on have to be drivable, or the
  -- order is one no tank could obey and the verdict would mean nothing.
  local ok = { [game.TERRAIN.grass] = true, [game.TERRAIN.road] = true }
  for _, s in ipairs({ { LX, LY }, { SX, SY }, { TX, TY } }) do
    local t = game.map_tile(s[1], s[2])
    if not (t and ok[t]) then
      finish(string.format("FAIL order_goto_holds: (%d,%d) is terrain %s",
                           s[1], s[2], tostring(t)))
      return
    end
  end
end

function on_tick(t)
  if done then return end
  now = t

  if t == SETUP_AT then
    for p = 0, 1 do
      game.builder_recall(p)
      game.set_stocks(p, { shells = 40, armour = 40, trees = 20 })
    end
    place()
    return
  end

  -- ── THE BURST ─────────────────────────────────────────────────────────
  if t == FIRE_AT then
    if not place() then
      finish("FAIL order_goto_holds: the seats could not be placed")
      return
    end
    start_away = away(LISTENER, TX, TY)
    if not start_away or start_away < MIN_AWAY then
      finish(string.format("FAIL order_goto_holds: the bot starts %s away; "
                           .. "the trip has to be at least %d",
                           tostring(start_away), MIN_AWAY))
      return
    end
    burst = { t, t + GAP, t + 2 * GAP }
    fired_at = burst[3]
    game.log(string.format(
      "order_goto_holds: burst on (%d,%d), the bot is %d away",
      TX, TY, start_away))
  end

  -- Each shell is posted on the tick it was fired, which is the plainest
  -- shape a burst can have and the one the detector's rules are written for.
  for i = 1, 3 do
    if burst[i] and t >= burst[i] then
      local ok, code, detail = game.shell_expired(SHOOTER, TX, TY, burst[i])
      burst[i] = nil
      if not ok then
        finish(string.format("FAIL order_goto_holds: shell %d refused %s (%s)",
                             i, tostring(code), tostring(detail)))
        return
      end
    end
  end
  if not fired_at then return end

  local since = t - fired_at
  local d = away(LISTENER, TX, TY)
  if not d then
    finish("FAIL order_goto_holds: the listener lost its tank")
    return
  end

  -- ── 1. GO ─────────────────────────────────────────────────────────────
  if not arrived_at then
    if d <= ARRIVE_AT then
      arrived_at = t
      game.log(string.format("order_goto_holds: there in %d ticks",
                             t - fired_at - QUIET))
    elseif since >= (QUIET + ARRIVE_BY) then
      finish(string.format(
        "FAIL order_goto_holds: %d ticks on, still %d from (%d,%d); it was %d",
        ARRIVE_BY, d, TX, TY, start_away))
    end
    return
  end

  -- ── 2. HOLD ───────────────────────────────────────────────────────────
  if not held_ok then
    if since < HOLD_UNTIL then return end
    if d > ARRIVE_AT then
      finish(string.format(
        "FAIL order_goto_holds: there in %d ticks, then %d away at +%d",
        arrived_at - fired_at, d, HOLD_UNTIL))
      return
    end
    held_ok = d
    game.log(string.format("order_goto_holds: still %d away at +%d",
                           d, HOLD_UNTIL))
    return
  end

  -- ── 3. LET GO ─────────────────────────────────────────────────────────
  if not cancel_at then
    if since < (HOLD_UNTIL + CANCEL_GAP) then return end
    if not game.say(SHOOTER, "!cancel all", "all") then
      finish("FAIL order_goto_holds: the cancel was refused")
      return
    end
    cancel_at = t
    game.log("order_goto_holds: cancelled at " .. t)
    return
  end

  if d > FREE_AT then
    finish(string.format(
      "PASS order_goto_holds: there in %d, %d away at +%d, gone %d after cancel",
      arrived_at - fired_at - QUIET, held_ok, HOLD_UNTIL, t - cancel_at))
    return
  end
  if (t - cancel_at) >= FREE_BY then
    finish(string.format(
      "FAIL order_goto_holds: %d ticks after the cancel it is still %d away",
      FREE_BY, d))
  end
end
