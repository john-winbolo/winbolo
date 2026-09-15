-- ROOST: a place order goes, holds for ten seconds, and then lets go.
--
-- "Go there and hold" (a ping on open ground, or three shells on one square)
-- used to run as a take_cover pinned to that square, which left it bidding
-- inside the goal pools with every other goal still competing: bots reached
-- the square and then drove off to a pillbox, or never got there at all.
-- Andrew, Sep 15: a place order is an instruction, so the trip now runs as a
-- command goal (kind goto_tile), the same slot the old `!pill:N` line used.
-- goals.pick_goal answers a command goal BEFORE goal selection, so on the way
-- the bot picks no goals of its own at all.
--
-- Andrew, Sep 15 again: the HOLD is about ten seconds AFTER ARRIVAL, not
-- whatever was left of the sixty-second focus. The focus bounds the TRIP; the
-- tick the tank is on the square the clock is replaced by
-- ORDER_GOTO_HOLD_TICKS, the bot says "holding 10s", and when that runs out
-- the order ends without a word.
--
-- What this round reads is all three of those, from outside:
--
--   1. GO. Three shells land on a square eight away and the bot gets within
--      two of it inside six hundred ticks.
--   2. SAY IT. Within sixty ticks of arriving it says "holding 10s" — the
--      number comes from the knob, so the line is the promise.
--   2b. FIGHT WITHOUT MOVING. An enemy tank is parked five squares out, on
--      whichever side the holder's gun is NOT pointing at, and emptied of
--      shells so it cannot shoot the holder into a flee. The gun has to come
--      round onto it, and the TILE must not change while it does. That is
--      the hold in one picture: the pools are live again underneath it, so
--      attack_tank can win, but winning turns the gun and not the tracks.
--   3. HOLD. Eight hundred ticks after arriving it is STILL within two. This
--      arena offers two neutral bases, which capture_base wants badly, so a
--      bot with the pools live underneath it would have been pulled off.
--   4. LET GO ON ITS OWN. Inside eighteen hundred ticks of arriving — with NO
--      cancel, no second line, nothing said to it at all — it must be more
--      than three squares off the square. That is the hold expiring and goal
--      selection picking up again, which is the half of the change a timer
--      test can see.
--
-- THE TWO CLOCKS. A scenario counts GAME ticks and a brain counts THINKS, and
-- a brain thinks every other game tick — so the knob's 500 is 1000 ticks out
-- here. Measured on this round: the bot arrives about 1080, the hold runs to
-- about 2080 and it is three squares off by about 2450. The windows above sit
-- either side of that: 800 is well inside the hold, 1800 is well past the end
-- of it plus the drive away.
--
-- There is no cancel phase any more: the order ends itself well before a
-- cancel could be sent, and a cancel after that would prove nothing.
--
-- The shells come from game.shell_expired, the same test hook
-- three_shots_go_there uses: a script cannot make a seat pull a trigger. The
-- hook posts the expiry and nothing else, and its fourth argument is the tick
-- the shell LEFT THE GUN, which is what every timing rule reads.
--
-- Three seats, `-teams 2,1`: the SHOOTER and the LISTENER on team 1 and one
-- enemy on team 2. The shooter fires; a sender never hears its own line, so
-- the LISTENER is the only bot that can take the order and the only one the
-- round has to watch. Both are teleported onto named squares on the grass
-- island the order_* rounds share, right before the burst, because the range
-- rule is the shooter's own screen and two bots left alone drift apart. The
-- enemy is a prop: it is kept in a far corner and only brought out for the
-- fight phase.

scenario = {
  name        = "ROOST order_goto_holds",
  description = "A place order: the bot goes, says it is holding, holds ten seconds, then lets go on its own.",
  api         = 1,
}

local LISTENER = 0        -- hears the order; the only bot that can obey
local SHOOTER  = 1        -- fires the shells
local ENEMY    = 2        -- team 2, wheeled in during the hold

-- The island runs (96,96) to (159,159) and is flat grass, so these are all
-- drivable and eight squares apart on one row.
local LX, LY   = 120, 128   -- where the listener starts the trip
local SX, SY   = 122, 128   -- the shooter, two squares away, well in view
local TX, TY   = 128, 128   -- the square the shells land on
local EX, EY   = nil, nil   -- where the enemy is parked during the hold: five
                            -- squares out, picked at the time so it is BEHIND
                            -- whichever way the holder's gun is pointing
local FX, FY   = 150, 150   -- and where it is kept the rest of the round

local QUIET      = 100    -- SHOT_ORDER_QUIET_TICKS: the order goes out this
                          -- long after the THIRD shot was fired
local GAP        = 40     -- between one shell of the burst and the next

local SETUP_AT   = 150    -- stocks and builders
local FIRE_AT    = 300    -- the first shell, and the teleport that aims it
local ARRIVE_BY  = 600    -- ticks after the third shot to get there in
local SAY_BY     = 60     -- ticks after arriving to say "holding" in
local FIGHT_AT   = 200    -- ticks after arriving the enemy is wheeled in
local FIGHT_FOR  = 400    -- and how long it is left sitting there
local HOLD_FOR   = 800    -- ticks after arriving it must still be there
local FREE_BY    = 1800   -- ticks after arriving it must have left by

local ARRIVE_AT  = 2      -- squares: "got there", and "still there"
local FREE_AT    = 3      -- squares: "left"
local MIN_AWAY   = 4      -- the trip has to be a trip
local AIM_SLACK  = 24     -- of 256: how near the bearing counts as "at it"
local MIN_TURN   = 64     -- of 256: the turn has to be a quarter turn at least

local now        = 0
local fired_at   = nil    -- the fire tick of the third shell
local burst      = {}
local start_away = nil
local arrived_at = nil
local said_at    = nil    -- when "holding" was heard
local said_text  = nil
local fight_from = nil    -- the tile it was standing on when the enemy came
local fight_mx, fight_my = nil, nil
local start_gap  = nil    -- how far off the enemy was when it appeared
local aimed_at   = nil    -- when its gun first pointed at the enemy
local fight_done = false
local held_ok    = nil    -- how far off it was at the hold check
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

-- The heading a tank would need to look at a square, in the engine's 256ths
-- of a turn: 0 is north and 64 is east, which is the frame steering aims in.
local function bearing_to(p, mx, my)
  local tk = game.tank(p)
  if not tk then return nil end
  local dx = (mx * 256 + 128) - tk.wx
  local dy = (my * 256 + 128) - tk.wy
  local a  = math.atan2(dx, -dy) / (2 * math.pi) * 256
  return a % 256
end

-- How far apart two headings are, the short way round.
local function turn_gap(a, b)
  local d = (a - b) % 256
  if d > 128 then d = 256 - d end
  return d
end

-- The enemy is a prop, not an opponent: parked where the script puts it and
-- emptied of shells every tick, so it cannot shoot the holder into a flee
-- and the round measures one thing only — does the holder turn on it without
-- driving at it.
local function park_enemy(mx, my)
  game.teleport(ENEMY, mx, my)
  game.set_stocks(ENEMY, { shells = 0 })
end

-- The arrival line. "holding" is a word no other line in this round carries,
-- and only the bot that took the order says it.
function on_chat(p, text, scripted)
  if scripted or done or text:sub(1, 1) == "/" then return end
  if p ~= LISTENER or said_at then return end
  if text:find("holding", 1, true) then
    said_at, said_text = now, text
    game.log(string.format("order_goto_holds: said %q at %d", text, now))
  end
end

function on_start()
  if not (game.tank(LISTENER) and game.tank(SHOOTER) and game.tank(ENEMY)) then
    finish("FAIL order_goto_holds: the round needs three seated bots")
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
    for p = 0, 2 do
      game.builder_recall(p)
      game.set_stocks(p, { shells = 40, armour = 40, trees = 20 })
    end
    place()
    park_enemy(FX, FY)
    return
  end

  -- The enemy is kept in its corner until the fight phase wants it, or the
  -- round would be about two bots meeting by accident.
  if not fight_from and t > SETUP_AT then park_enemy(FX, FY) end

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

  local held = t - arrived_at

  -- ── 2. SAY IT ─────────────────────────────────────────────────────────
  if not said_at then
    if held >= SAY_BY then
      finish(string.format(
        "FAIL order_goto_holds: there at %d, no holding line %d ticks later",
        arrived_at, SAY_BY))
    end
    return
  end

  -- ── 2b. FIGHT WITHOUT MOVING ──────────────────────────────────────────
  -- The hold is not a coma. From the arrival tick the hard lock is off and
  -- goal selection runs again, so attack_tank can win — and winning must
  -- turn the gun, not the tracks. An enemy tank is parked five squares due
  -- east and held there, empty of shells so it cannot shoot the holder into
  -- a flee. The two assertions are the whole point of the change: the gun
  -- comes round to point at it, and the TILE never changes while it does.
  if not fight_done then
    if held < FIGHT_AT then return end
    local tk = game.tank(LISTENER)
    if not tk then
      finish("FAIL order_goto_holds: the listener lost its tank")
      return
    end
    if not fight_from then
      fight_from, fight_mx, fight_my = t, tk.mx, tk.my
      -- BEHIND IT, whichever way it happens to be facing. A tank that was
      -- already pointing the right way proves nothing, so the enemy goes on
      -- whichever of the four squares five out is furthest from the gun's
      -- current heading — with four of them the furthest is always most of a
      -- half turn away, and the island is flat grass for sixty squares in
      -- every direction from here, so all four are drivable ground.
      local best
      for _, s in ipairs({ { fight_mx, fight_my - 5 }, { fight_mx + 5, fight_my },
                           { fight_mx, fight_my + 5 }, { fight_mx - 5, fight_my } }) do
        local want = bearing_to(LISTENER, s[1], s[2])
        local gap  = want and turn_gap(tk.dir, want) or -1
        if not best or gap > best.gap then
          best = { mx = s[1], my = s[2], gap = gap }
        end
      end
      EX, EY, start_gap = best.mx, best.my, best.gap
      game.log(string.format(
        "order_goto_holds: the enemy goes to (%d,%d); the holder is on (%d,%d) "
        .. "facing %d, %d off it",
        EX, EY, fight_mx, fight_my, tk.dir, start_gap))
      if start_gap < MIN_TURN then
        finish(string.format(
          "FAIL order_goto_holds: the holder already faces every side (%d off)",
          start_gap))
        return
      end
    end
    park_enemy(EX, EY)
    if tk.mx ~= fight_mx or tk.my ~= fight_my then
      finish(string.format(
        "FAIL order_goto_holds: it drove off the square to fight — (%d,%d) not (%d,%d)",
        tk.mx, tk.my, fight_mx, fight_my))
      return
    end
    if not aimed_at then
      local want = bearing_to(LISTENER, EX, EY)
      if want and turn_gap(tk.dir, want) <= AIM_SLACK then
        aimed_at = t
        game.log(string.format("order_goto_holds: on target at +%d, facing %d",
                               t - fight_from, tk.dir))
      end
    end
    if (t - fight_from) >= FIGHT_FOR then
      if not aimed_at then
        finish(string.format(
          "FAIL order_goto_holds: %d ticks beside an enemy and never turned on it",
          FIGHT_FOR))
        return
      end
      fight_done = true
      park_enemy(FX, FY)
    end
    return
  end

  -- ── 3. HOLD ───────────────────────────────────────────────────────────
  if not held_ok then
    if held < HOLD_FOR then return end
    if d > ARRIVE_AT then
      finish(string.format(
        "FAIL order_goto_holds: there at %d, then %d away at +%d",
        arrived_at, d, HOLD_FOR))
      return
    end
    held_ok = d
    game.log(string.format("order_goto_holds: still %d away at +%d",
                           d, HOLD_FOR))
    return
  end

  -- ── 4. LET GO ON ITS OWN ──────────────────────────────────────────────
  -- Nothing is said to the bot here. The ten seconds runs out, the order
  -- ends, goal selection picks up again and the island's two neutral bases
  -- take it away.
  if d > FREE_AT then
    finish(string.format(
      "PASS order_goto_holds: %q, turned %d in %d, %d away at +%d, gone +%d",
      said_text, start_gap, aimed_at - fight_from, held_ok, HOLD_FOR, held))
    return
  end
  if held >= FREE_BY then
    finish(string.format(
      "FAIL order_goto_holds: %d ticks after arriving it is still %d away",
      FREE_BY, d))
  end
end
