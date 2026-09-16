-- ROOST: a bot killed while holding a place order comes back able to drive.
--
-- "Go there and hold" parks the tank: from the arrival tick steering takes
-- the throttle away for as long as the hold runs, so the bot fights from the
-- square instead of walking off it. Two things were missing from that.
--
--   * Nothing cleared the order slot on a DEATH. init.lua's dead-tick block
--     returns before ORD.update is ever reached, so a bot killed mid-hold
--     came back with the hold flag still set.
--   * The park had no distance test at all. It asked whether the slot said
--     "holding" and whether the goal was a park kind, and never whether the
--     tank was anywhere near the square a person had pointed at.
--
-- Together they froze a respawned bot ON ITS SPAWN SQUARE for the rest of
-- the hold -- nowhere near the order it was obeying, and unable to move at
-- all, because the park is the throttle being taken away. Andrew's peer
-- review, Sep 16.
--
-- WHAT THE ROUND READS. A parked tank cannot drive, so the reading is how
-- far the new life gets from the square it came back on. Measured on this
-- arena: a bot with the fix moves four squares in the four hundred ticks
-- after respawning and a bot without it moves none at all, so the round asks
-- for two.
--
-- THE WINDOW HAS TO SIT INSIDE THE HOLD, or the reading says nothing: the
-- hold knob is 500 BRAIN ticks and a brain thinks every other game tick, so
-- the freeze would end by itself about a thousand game ticks after arriving,
-- and a bot that waited it out drives away too. Measured here: the bot
-- arrives about 634, so the hold would run to about 1634; the kill is at
-- once, the respawn lands about 1190 and the window closes about 1590.
-- That is why the kill is twenty ticks after arriving and not two hundred --
-- the respawn itself takes five hundred, and it all has to fit.
--
-- The obr release verb was tried as the signal first and is not available:
-- the order verbs ride msg_dest 0, the brains' internal channel, which is
-- never published, so a scenario's on_chat never sees one. The spoken lines
-- ("holding", the acks, "released") do reach it, which is what phase 2 uses.
--
-- The phases:
--
--   1. GO. Seat 1 says "!goto 128 128" to everyone. Seat 0 is eight squares
--      off it -- inside ORDER_NEARBY_TILES -- and seat 1 never hears its own
--      line, so seat 0 is the only bot that can take it.
--   2. HOLD. Seat 0 gets within two squares and says "holding": that line is
--      said from one place only, the tick the hold starts, so it is the proof
--      the bot really is in the phase this round is about.
--   3. DIE, straight away, so the whole respawn sits inside the hold.
--   4. DRIVE. The new life has to go somewhere.
--
-- Seat 2 is on team 2 and is a prop: parked in a far corner and emptied of
-- shells every tick, so nothing but seat 1's chat line reaches the bot under
-- test and the death in phase 3 is the script's and nobody else's.

scenario = {
  name        = "ROOST order_death_clears_hold",
  description = "A bot killed during a go-there hold respawns able to drive, not frozen on its spawn square.",
  api         = 1,
}

local LISTENER = 0        -- team 1: takes the order, and is killed
local SPEAKER  = 1        -- team 1: gives it, never hears its own line
local ENEMY    = 2        -- team 2: a prop, kept out of the way

-- The island runs (96,96) to (159,159) and is flat grass here.
local LX, LY = 120, 128   -- the listener: eight squares from the target
local SX, SY = 122, 128   -- the speaker, beside it
local TX, TY = 128, 128   -- the square the order names
local FX, FY = 150, 150   -- the enemy's corner

local SETUP_AT  = 150     -- stocks, builders, everybody onto their squares
local SAY_AT    = 300     -- the order
local ARRIVE_BY = 900     -- ticks after the order to get there in
local SAY_BY    = 200     -- ticks after arriving to say "holding" in
local KILL_AT   = 20      -- ticks after arriving: at once, so the whole
                          -- respawn happens inside the hold
local SPAWN_BY  = 900     -- ticks after the kill to be back on the field in
local MOVE_BY   = 400     -- ticks after respawning the tank is watched for

local ARRIVE_AT = 2       -- squares: "got there"
local MOVE_MIN  = 2       -- squares from the spawn: "it can drive again"
local MIN_AWAY  = 4       -- the trip has to be a trip

local now       = 0
local said_at   = nil     -- when the order went out
local arrived_at= nil
local holding_at= nil     -- when "holding" was heard
local killed_at = nil
local spawn_at  = nil     -- the respawn tick
local sx, sy    = nil, nil -- and the square it came back on
local best_move = 0       -- how far it has got from that square since
local done      = false

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

-- Squares apart the way a screen is: the larger of the two axes.
local function away(p, mx, my)
  local tk = game.tank(p)
  if not tk then return nil end
  local dx, dy = tk.mx - mx, tk.my - my
  if dx < 0 then dx = -dx end
  if dy < 0 then dy = -dy end
  return (dx > dy) and dx or dy
end

local function place()
  local a = game.teleport(LISTENER, LX, LY)
  local b = game.teleport(SPEAKER, SX, SY)
  return a and b
end

-- "holding" is a word no other line in this round carries, and only the bot
-- that took the order says it, on the tick the hold starts.
function on_chat(p, text, scripted)
  if scripted or done or text:sub(1, 1) == "/" then return end
  if p ~= LISTENER or holding_at then return end
  if text:find("holding", 1, true) then
    holding_at = now
    game.log(string.format("order_death_clears_hold: said %q at %d", text, now))
  end
end

function on_tank_spawned(p, mx, my, respawn)
  if done or p ~= LISTENER or not respawn then return end
  if not killed_at or spawn_at then return end
  spawn_at, sx, sy = now, mx, my
  game.log(string.format("order_death_clears_hold: back at (%d,%d) at +%d",
                         mx, my, now - killed_at))
end

function on_start()
  if not (game.tank(LISTENER) and game.tank(SPEAKER) and game.tank(ENEMY)) then
    finish("FAIL order_death_clears_hold: the round needs three seated bots")
    return
  end
  -- The squares this round is written on have to be drivable, or the order
  -- is one no tank could obey and the verdict would mean nothing.
  local ok = { [game.TERRAIN.grass] = true, [game.TERRAIN.road] = true }
  for _, s in ipairs({ { LX, LY }, { SX, SY }, { TX, TY } }) do
    local t = game.map_tile(s[1], s[2])
    if not (t and ok[t]) then
      finish(string.format("FAIL order_death_clears_hold: (%d,%d) is terrain %s",
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
    return
  end

  -- The enemy is a prop: out of reach and out of ammunition, so the only
  -- death in this round is the one the script causes.
  if t > SETUP_AT then
    game.teleport(ENEMY, FX, FY)
    game.set_stocks(ENEMY, { shells = 0 })
  end

  -- ── 1. GO ─────────────────────────────────────────────────────────────
  if not said_at then
    if t < SAY_AT then return end
    -- Placed again: the range rule is measured from the tank, and 150 ticks
    -- is long enough for two bots left alone to drift.
    if not place() then
      finish("FAIL order_death_clears_hold: the seats could not be placed")
      return
    end
    local d = away(LISTENER, TX, TY)
    if not d or d < MIN_AWAY then
      finish(string.format("FAIL order_death_clears_hold: the bot starts %s away; "
                           .. "the trip has to be at least %d",
                           tostring(d), MIN_AWAY))
      return
    end
    local ok, code, detail = game.say(SPEAKER,
                                      string.format("!goto %d %d", TX, TY), "all")
    if not ok then
      finish(string.format("FAIL order_death_clears_hold: game.say refused %s (%s)",
                           tostring(code), tostring(detail)))
      return
    end
    said_at = t
    game.log(string.format("order_death_clears_hold: ordered (%d,%d), the bot is %d away",
                           TX, TY, d))
    return
  end

  if not arrived_at then
    local d = away(LISTENER, TX, TY)
    if d and d <= ARRIVE_AT then
      arrived_at = t
      game.log(string.format("order_death_clears_hold: there in %d ticks", t - said_at))
    elseif (t - said_at) >= ARRIVE_BY then
      finish(string.format(
        "FAIL order_death_clears_hold: %d ticks on, still %s from (%d,%d)",
        ARRIVE_BY, tostring(d), TX, TY))
    end
    return
  end

  -- ── 2. HOLD ───────────────────────────────────────────────────────────
  if not holding_at then
    if (t - arrived_at) >= SAY_BY then
      finish(string.format(
        "FAIL order_death_clears_hold: there at %d, no holding line %d ticks later",
        arrived_at, SAY_BY))
    end
    return
  end

  -- ── 3. DIE, at once, so the whole respawn sits inside the hold ────────
  if not killed_at then
    if (t - arrived_at) < KILL_AT then return end
    local ok, code, detail = game.kill_tank(LISTENER)
    if not ok then
      finish(string.format("FAIL order_death_clears_hold: kill_tank refused %s (%s)",
                           tostring(code), tostring(detail)))
      return
    end
    killed_at = t
    game.log(string.format("order_death_clears_hold: killed at +%d into the hold",
                           t - arrived_at))
    return
  end

  if not spawn_at then
    if (t - killed_at) >= SPAWN_BY then
      finish(string.format("FAIL order_death_clears_hold: no respawn %d ticks after the kill",
                           SPAWN_BY))
    end
    return
  end

  -- ── 4. DRIVE ──────────────────────────────────────────────────────────
  -- The furthest the new life has got from the square it came back on. A bot
  -- that came back parked cannot move at all: the park clears the throttle
  -- key every tick and brakes whatever is still rolling.
  local tk = game.tank(LISTENER)
  if tk and not tk.dead then
    local dx, dy = tk.mx - sx, tk.my - sy
    if dx < 0 then dx = -dx end
    if dy < 0 then dy = -dy end
    local d = (dx > dy) and dx or dy
    if d > best_move then best_move = d end
  end

  if best_move >= MOVE_MIN then
    finish(string.format(
      "PASS order_death_clears_hold: held at %d, killed, drove %d squares in %d",
      arrived_at, best_move, now - spawn_at))
    return
  end

  if (t - spawn_at) >= MOVE_BY then
    finish(string.format(
      "FAIL order_death_clears_hold: %d ticks after respawning at (%d,%d) it moved "
      .. "%d squares (need %d)", MOVE_BY, sx, sy, best_move, MOVE_MIN))
  end
end
