-- ROOST: a decoy on a ping square steps out of the pill's line, one square
-- per hit, while a team-mate shoots the pill.
--
-- The decoy hold (orders.lua, GO-THERE DECOY HARD HOLD) parks a bot on a
-- square a pillbox can shoot, so the pill spends its shells on the decoy
-- while a team-mate takes it down.  The DECOY GETAWAY (decoy_getaway.lua,
-- Andrew, Sep 24) plans a way out the moment the hold starts: a chain of
-- squares, each one ring further out from the decoy square, each one
-- shielded from the pill by a wall.  The bot parks facing the first square
-- of the chain.  Every hit it takes while parked moves it ONE square along
-- the chain, and then it waits for the next hit.  Hits on the way do not
-- count.  THE BLOCKER STEP (Andrew, Sep 24): parked on a chain square (not
-- the decoy square), it also moves on with no hit when the LAST blocker on
-- the closest pill's shell line to it has DECOY_GETAWAY_BLOCKER_SHOTS (2)
-- shots left or fewer.  With 2 or more blockers it holds.  A full wall
-- stops building_life + 1 = 5 shells, and the brain COUNTS the shells that
-- stop on it (it hears each wall hit; the full-to-damaged change is hit 1).
-- So the decoy holds behind a full wall for 3 hits and then moves on.
--
-- The round runs the two jobs at the same time.  The ATTACKER is ordered
-- onto the pill first, and it is shooting the pill by the time the decoy
-- has its first hit.  The walls keep the engine's building_life (4), which
-- is the value the brain works its shots left out from.
--
-- What this round reads, from outside:
--
--   1. THE HOLD.  A goto hint with ping = "1" (the scenario's stand-in for a
--      bot-command ping, which no scenario op can place) sends the decoy to
--      a square six south of the pill, beside a row of walls.  It says
--      "decoying" -- the hold, not the plain goto hold.
--   2. NO MOVE WITHOUT A TRIGGER.  The tank's square changes only within
--      MOVE_BY ticks of a hit, or after the first step from a square whose
--      shell line has no blocker, or one blocker that is not a full wall
--      (the round's own look, below: it cannot count the hits on a damaged
--      wall, so a damaged last wall passes).
--   2b. THE WALL IS HIT 3 TIMES.  The round logs every wall change with its
--      tick: full to damaged is hit 1, damaged to gone is hit 5 (the hits
--      in between change no tile, so the round cannot see them; the brain's
--      DECOY_GETAWAY_WALLHIT lines in a -brain-debug recording list them).
--      A blocker step from a square whose last wall was full when the tank
--      parked there must come while that wall still stands (fewer than 5
--      hits) and at least MIN_GAP ticks after it turned damaged: two more
--      pill shells need 2 x pill_attack_min_ticks (6) at the least.
--   3. ONE TRIGGER, ONE STEP.  A STEP is the tank reaching a square one
--      ring further out from the decoy square than it has been before.  The
--      first step needs a hit.  Every later step needs a hit since the step
--      before it, or a blocker step (2, 2b) from the square it left.
--      The log names each step's trigger ("hit" or "blk").  There are at
--      least two steps, and at least one of them is a blocker step.  The
--      first square it stays still on is FIRST_STEP, the
--      first square of the chain DECOY_GETAWAY_SCAN logs for this map.  A
--      blocker step can leave FIRST_STEP before it counts as still; then
--      the square that made ring 1 must be FIRST_STEP.
--      Where the first hit came late enough for the turn to be done
--      (TURN_TIME after it parked), the gun was already pointing there.
--   4. THE ATTACKER.  The pill's armour goes down while the decoy holds,
--      starting no later than ATTACK_SLACK after the decoy's first hit.  The
--      scenario puts the pill's armour back when it runs low, because a dead
--      pill ends the hold.
--   The log lists every hit on the decoy (tick, square) and the armour it
--   lost in the watch.
--
-- Under an angry pill, a hit knocks the tank back while it turns for the
-- next square, and the next shell often comes before it gets there.  So a
-- step can take a few hundred ticks and cross squares that are not on the
-- chain.  That is why the watch counts rings after the first step, not
-- squares.
--
-- The decoy_getaway_keel round is this script with KEEL = true: the decoy is
-- given cfg=DECOY_GETAWAY=false before the ping, and it must take hits and
-- never step away.  The pill is due north, so a hit knocks the tank straight
-- south, and under an angry pill the old hold is knocked a few squares down
-- that column (it does not drive back while the shells keep coming).  A
-- step goes sideways, behind the walls.  So KEEL fails if the tank ever
-- stops on a square off the decoy square's column.
--
-- Three seats, `-teams 2,1`: the DECOY and the ATTACKER on team 1, one
-- enemy on team 2 that owns the pill (so the pill shoots at team 1) and is
-- kept in a far corner with no shells.  The attacker is a suicider: it does
-- not build a wall shield first, which takes longer than the hold lasts.
--
-- The map is the order_* island (flat grass from (96,96) to (159,159)).

local KEEL = true

scenario = {
  name        = KEEL and "ROOST decoy_getaway_keel" or "ROOST decoy_getaway",
  description = KEEL and "A decoy with the getaway off takes hits and never moves, while the attacker shoots the pill."
                or "A decoy steps out of the pill's line one square per hit while the attacker shoots the pill.",
  api         = 1,
}

local NAME = KEEL and "decoy_getaway_keel" or "decoy_getaway"

local DECOY    = 0
local ATTACKER = 1
local ENEMY    = 2

local PX, PY   = 128, 120   -- the pill
local DX, DY   = 128, 126   -- the decoy square: six south, in range, open
local WALL_Y   = 125        -- a row of buildings just east of the decoy
local WALL_X0, WALL_X1 = 129, 132
local AX, AY   = 128, 108   -- the attacker, twelve north: sees it, out of range
local WX, WY   = 128, 140   -- where the decoy waits before the ping
local FX, FY   = 152, 152   -- the enemy's corner

-- The first square of the chain, from DECOY_GETAWAY_SCAN on this map
-- (path=(129,126)...).  nil = not checked.
local FIRST_STEP = { 129, 126 }

local SETUP_AT   = 150
local ORDER_AT   = 160     -- the attacker's order
local HINT_AT    = 300
local PLACE_EARLY = 40     -- ticks on the decoy square before the hint
local HOLD_BY    = 200     -- ticks after the hint to hear "decoying" in
local WATCH_FOR  = 960     -- ticks of the hold that are watched (it runs 1000)
local MOVE_BY    = 400     -- a square change must come this soon after a hit
local TURN_TIME  = 60      -- ticks parked before the facing is checked
local AIM_SLACK  = 24      -- of 256
local ATTACK_SLACK = 400   -- the attacker's first hit, at most this after the decoy's
local ARMOUR_LOW = 25      -- refill below this, so no flee rule can fire
local SETTLE     = 30      -- ticks still on one square before it counts as stopped
-- Shell hits a damaged wall takes before it falls.  The keel round keeps
-- the old 1; the getaway round keeps the engine's 4 (BUILDING_LIFE), the
-- value the brain's DECOY_GETAWAY_WALL_LIFE assumes.
local BUILDING_LIFE = KEEL and 1 or 4
local PILL_LOW   = 5       -- the pill's armour is put back to 15 below this
local MIN_GAP    = 12      -- 2 more shells at the fastest pill fire rate (6)

local now       = 0
local done      = false
local pill_n    = nil
local hint_at   = nil
local hold_at   = nil
local hold_text = nil

-- The watch.
local parked_at = nil      -- when the hold began
local last_arm  = nil
local arm_lost  = 0        -- armour the decoy lost in the watch
local hit_list  = {}       -- "tick@x,y" for every hit
local hits      = 0        -- hits on the decoy in the watch
local first_hit_at = nil
local last_hit_at = nil
local hits_since = 0       -- hits since the last step
local first_dir = nil      -- facing at the first hit
local first_late = false   -- the first hit came TURN_TIME after it parked
local sq_x, sq_y = nil, nil  -- the square it is on
local sq_dir    = nil
local still     = 0        -- ticks on that square, facing one way
local first_still = nil    -- the first square off D it stayed still on
local ring1_x, ring1_y = nil, nil  -- the square that made ring 1
local aimed     = false
local max_ring  = 0        -- steps: the furthest ring it has reached
local step_log  = {}
local blk_steps = 0        -- steps with no hit since the step before
local slack_x, slack_y = nil, nil  -- KEEL: the square a knock left it on, if any
local wall_seen = {}       -- x -> the wall row's tile at x, last tick
local wall_dmg_at = {}     -- x -> the tick it turned damaged
local wall_log  = {}       -- "(x,y) state@tick"
local park_last = nil      -- { x, y, full } the one wall on the line at park
local park_at   = nil      -- when it came onto this square
local gap_log   = {}       -- "(x,y)+gap" for each blocker step from a full wall
-- THE DEPARTURE.  A square takes the tank 80 to 100 ticks to cross, and a
-- shell can hit it on the way out, so a step's trigger is judged when the
-- tank STARTS to move (its world position changes after a tick still), not
-- when it crosses into the next square.
local prev_wx, prev_wy = nil, nil
local was_moving = false
local run_at   = nil       -- the tick the current move began
local run_hits = 0         -- hits since the last step, at run_at
local run_nb, run_last = nil, nil  -- the walls on the line, at run_at
local stop_n   = 0         -- ticks still in a row

-- The attacker.
local pill_last = nil
local pill_hits = 0        -- times the pill's armour went down
local pill_hit_at = nil    -- the first of them

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

local function fail(fmt, ...)
  finish("FAIL " .. NAME .. ": " .. string.format(fmt, ...))
end

local function ring(x, y)
  local dx, dy = math.abs(x - DX), math.abs(y - DY)
  return (dx > dy) and dx or dy
end

-- The heading from the centre of one square to the centre of another, in
-- 256ths of a turn: 0 north, 64 east.
local function bearing(x0, y0, x1, y1)
  local a = math.atan2(x1 - x0, -(y1 - y0)) / (2 * math.pi) * 256
  return a % 256
end

-- THE ROUND'S OWN LOOK at the shell line from the pill to square (x,y): a
-- straight line from centre to centre in 1/16-square steps (the shell's
-- own step), each square once, the pill's square and (x,y) left out.
-- Returns the number of walls on it and the last one (nearest (x,y)):
-- { x, y, full }.  There is one pill and no pill of the decoy's team on
-- this map, so the closest pill is the pill and no pill is a blocker.
local function blockers(x, y)
  local x0, y0 = PX + 0.5, PY + 0.5
  local dx, dy = x + 0.5 - x0, y + 0.5 - y0
  local d = math.sqrt(dx * dx + dy * dy)
  local n, seen, last = 0, {}, nil
  for i = 0, math.floor(d * 16) do
    local mx = math.floor(x0 + dx / d * i / 16)
    local my = math.floor(y0 + dy / d * i / 16)
    local k = my * 256 + mx
    if not seen[k] and not (mx == PX and my == PY) and not (mx == x and my == y) then
      seen[k] = true
      local t = game.map_tile(mx, my)
      if t == game.TERRAIN.building or t == game.TERRAIN.half_building then
        n = n + 1
        last = { x = mx, y = my, full = (t == game.TERRAIN.building) }
      end
    end
  end
  return n, last
end

-- 2b. The wall row, every watched tick: log each change with its tick.
local function watch_walls()
  for x = WALL_X0, WALL_X1 do
    local t = game.map_tile(x, WALL_Y)
    local was = wall_seen[x]
    if was and t ~= was then
      local what, short
      if was == game.TERRAIN.building and t == game.TERRAIN.half_building then
        what, short, wall_dmg_at[x] = "damaged (hit 1)", "dmg", now
      elseif t == game.TERRAIN.building then
        what, short = "full again", "full"
      else
        what, short = string.format("gone (hit %d)", BUILDING_LIFE + 1), "gone"
      end
      wall_log[#wall_log + 1] = string.format("(%d,%d)%s@%d", x, WALL_Y, short, now)
      game.log(string.format("%s: wall (%d,%d) %s at %d", NAME, x, WALL_Y, what, now))
    end
    wall_seen[x] = t
  end
end

local function turn_gap(a, b)
  local d = (a - b) % 256
  if d > 128 then d = 256 - d end
  return d
end

local function park(p, x, y, dir)
  game.teleport(p, x, y, dir)
end

local function keep_enemy()
  park(ENEMY, FX, FY)
  game.set_stocks(ENEMY, { shells = 0 })
end

-- The shortest prefix of the attacker's name, three letters at the least,
-- that no other seat's name begins with (order_by_name does the same).
local function attacker_prefix()
  local mine = game.lobby_slot(ATTACKER)
  if not mine or not mine.name then return nil end
  local me = mine.name:lower()
  for k = 3, #me do
    local head, clash = me:sub(1, k), false
    for q = 0, game.max_tanks() - 1 do
      local o = q ~= ATTACKER and game.lobby_slot(q)
      if o and o.name and o.name:lower():sub(1, k) == head then clash = true end
    end
    if not clash then return head end
  end
  return nil
end

function on_chat(p, text, scripted)
  if scripted or done or p ~= DECOY or hold_at then return end
  if text:find("decoying", 1, true) then
    hold_at, hold_text = now, text
    game.log(string.format("%s: said %q at %d", NAME, text, now))
  elseif text:find("holding", 1, true) then
    fail("the hint gave a plain hold (%q), not a decoy hold", text)
  end
end

function on_start()
  if not (game.tank(DECOY) and game.tank(ATTACKER) and game.tank(ENEMY)) then
    fail("the round needs three seated bots")
  end
end

local function setup()
  for p = 0, 2 do
    game.builder_recall(p)
    game.set_stocks(p, { shells = 40, armour = 40, trees = 0 })
  end
  local sok, serr = game.bot_init(ATTACKER, { suicider = "1" })
  if not sok then return fail("bot_init attacker refused: %s", tostring(serr)) end
  -- One hit turns a wall into a half wall and the next one knocks it down,
  -- so the pill opens the cover of the square the decoy stepped to.
  local rok, rerr = game.set_rule("building_life", BUILDING_LIFE)
  if not rok then return fail("set_rule building_life refused: %s", tostring(rerr)) end
  for x = WALL_X0, WALL_X1 do
    local ok, err = game.set_tile(x, WALL_Y, game.TERRAIN.building)
    if not ok then return fail("set_tile (%d,%d) refused: %s", x, WALL_Y, tostring(err)) end
  end
  pill_n = game.add_pill(PX, PY, ENEMY, 15)
  if not pill_n then return fail("add_pill refused") end
  if KEEL then
    local ok, err = game.bot_init(DECOY, { cfg = "DECOY_GETAWAY=false" })
    if not ok then return fail("bot_init refused: %s", tostring(err)) end
  end
  game.log(string.format("%s: pill %d at (%d,%d), walls y=%d x=%d..%d",
                         NAME, pill_n, PX, PY, WALL_Y, WALL_X0, WALL_X1))
end

-- The decoy's square, armour and facing, one tick of the watch.
local function watch(tk)
  if tk.armour < ARMOUR_LOW then
    game.set_stocks(DECOY, { armour = 40 })
  end
  local arm = tk.armour
  if last_arm and arm < last_arm then
    arm_lost = arm_lost + (last_arm - arm)
    hit_list[#hit_list + 1] = string.format("%d@%d,%d", now, tk.mx, tk.my)
    hits = hits + 1
    hits_since = hits_since + 1
    last_hit_at = now
    first_hit_at = first_hit_at or now
    if not first_dir then
      first_dir, first_late = tk.dir, (now - parked_at) >= TURN_TIME
    end
    game.log(string.format("%s: hit %d on (%d,%d) at %d, facing %d",
                           NAME, hits, tk.mx, tk.my, now, tk.dir))
  end
  last_arm = (arm < ARMOUR_LOW) and 40 or arm

  -- THE DEPARTURE: a move begins.  A hit's knock-back begins one too, on
  -- the hit's own tick, so it is judged a hit.
  local moving = prev_wx ~= nil and (tk.wx ~= prev_wx or tk.wy ~= prev_wy)
  if moving and not was_moving and run_nb == nil then
    run_at, run_hits = now, hits_since
    run_nb, run_last = blockers(sq_x, sq_y)
  end
  -- A tank still for 10 ticks has not left: the next move is a new one.
  if moving then stop_n = 0 else stop_n = stop_n + 1 end
  if stop_n >= 10 then run_nb, run_last = nil, nil end
  was_moving, prev_wx, prev_wy = moving, tk.wx, tk.wy

  if tk.mx ~= sq_x or tk.my ~= sq_y then
    -- The walls on the square it left, when it began to leave.  The blocker
    -- step never runs on the decoy square (ring 0) nor with the getaway off
    -- (KEEL).
    local nb, last, left_at, h_at = run_nb, run_last, run_at, run_hits
    if nb == nil then
      nb, last = blockers(sq_x, sq_y)
      left_at, h_at = now, hits_since
    end
    local lw = last and (last.full and "full" or "damaged") or "-"
    local blk_ok = not KEEL and max_ring >= 1
                   and (nb == 0 or (nb == 1 and not last.full))
    -- 2. NO MOVE WITHOUT A TRIGGER.
    local by_hit = h_at > 0 and last_hit_at and left_at - last_hit_at <= MOVE_BY
    if not by_hit and not blk_ok then
      return fail("left (%d,%d) (%d walls, last %s) at %d for (%d,%d) at %d with no hit in %d ticks",
                  sq_x, sq_y, nb, lw, left_at, tk.mx, tk.my, now, MOVE_BY)
    end
    -- 2b. A blocker step from a square whose one wall was full at park: the
    -- wall still stands, and turned damaged at least MIN_GAP ago.
    if not by_hit and blk_ok and park_last and park_last.full then
      local dmg = wall_dmg_at[park_last.x]
      if park_last.y ~= WALL_Y or not dmg or nb ~= 1 then
        return fail("left (%d,%d) at %d: its wall (%d,%d) was full at park and was %s",
                    sq_x, sq_y, left_at, park_last.x, park_last.y, nb == 0 and "gone" or lw)
      end
      if left_at - dmg < MIN_GAP then
        return fail("left (%d,%d) at %d, %d ticks after its wall (%d,%d) turned damaged; 3 hits need %d",
                    sq_x, sq_y, left_at, left_at - dmg, park_last.x, park_last.y, MIN_GAP)
      end
      gap_log[#gap_log + 1] = string.format("(%d,%d)+%d", park_last.x, park_last.y, left_at - dmg)
      game.log(string.format("%s: left (%d,%d) at %d (parked %d); wall (%d,%d) full at park, damaged %d (+%d), standing",
                             NAME, sq_x, sq_y, left_at, park_at or -1, park_last.x, park_last.y, dmg, left_at - dmg))
    end
    run_nb, run_last = nil, nil
    local from_x, from_y = sq_x, sq_y
    sq_x, sq_y, sq_dir, still = tk.mx, tk.my, tk.dir, 0
    local pn, pl = blockers(sq_x, sq_y)
    park_last = (pn == 1) and pl or nil
    park_at = now
    -- 3. ONE TRIGGER, ONE STEP.
    local r = ring(sq_x, sq_y)
    if r > max_ring then
      local trig
      if by_hit then
        trig = string.format("hit (%d since ring %d)", h_at, max_ring)
      elseif blk_ok then
        trig = string.format("blk (%d walls on (%d,%d), last %s)", nb, from_x, from_y, lw)
        blk_steps = blk_steps + 1
      else
        return fail("ring %d at (%d,%d) at %d with no hit since ring %d and %d walls on (%d,%d)",
                    r, sq_x, sq_y, now, max_ring, nb, from_x, from_y)
      end
      if r == 1 then ring1_x, ring1_y = sq_x, sq_y end
      max_ring, hits_since = r, 0
      step_log[#step_log + 1] = string.format("%d,%d %s", sq_x, sq_y, trig:sub(1, 3))
      game.log(string.format("%s: ring %d at (%d,%d) at %d, trigger %s, %d after the last hit",
                             NAME, r, sq_x, sq_y, now, trig, now - (last_hit_at or now)))
    end
  end
  if tk.dir ~= sq_dir then sq_dir, still = tk.dir, 0 end
  still = still + 1

  if still == SETTLE and (sq_x ~= DX or sq_y ~= DY) then
    local on_first = FIRST_STEP and sq_x == FIRST_STEP[1] and sq_y == FIRST_STEP[2]
    -- KEEL: the pill is due north, so a knock pushes the tank straight
    -- south, down the decoy square's column.  A step goes sideways, behind
    -- the walls.  So it never stops off that column.
    if KEEL then
      if on_first or sq_x ~= DX then
        return fail("with the getaway off it stopped on (%d,%d) at %d (%d hits)",
                    sq_x, sq_y, now, hits)
      end
      slack_x, slack_y = sq_x, sq_y
      return
    end
    if first_still then return end
    first_still = { sq_x, sq_y }
    game.log(string.format("%s: first stop off the decoy square: (%d,%d) at %d",
                           NAME, sq_x, sq_y, now))
    local via_blk = FIRST_STEP and blk_steps > 0 and ring1_x == FIRST_STEP[1]
                    and ring1_y == FIRST_STEP[2]
    if FIRST_STEP and not on_first and not via_blk then
      return fail("first stop is (%d,%d), the chain says (%d,%d)",
                  sq_x, sq_y, FIRST_STEP[1], FIRST_STEP[2])
    end
    if first_late then
      local ax, ay = sq_x, sq_y
      if FIRST_STEP then ax, ay = FIRST_STEP[1], FIRST_STEP[2] end
      local want = bearing(DX, DY, ax, ay)
      if turn_gap(first_dir, want) > AIM_SLACK then
        return fail("parked facing %d, then went to (%d,%d) at bearing %d",
                    first_dir, ax, ay, math.floor(want))
      end
      aimed = true
    end
  end
end

function on_tick(t)
  if done then return end
  now = t

  if t == SETUP_AT then setup() return end
  if t < SETUP_AT then return end

  keep_enemy()
  -- THE ATTACKER goes first.  It is held out of the pill's range until
  -- ORDER_AT and then ordered onto the pill.  Its drive in takes about as
  -- long as the decoy's wait, so it is shooting the pill by the time the
  -- decoy has its first hit.  The decoy's seat says the order, with the
  -- attacker's name in front: a named order goes to that bot alone, and a
  -- sender never receives its own line.
  if t < ORDER_AT then park(ATTACKER, AX, AY, 128) end
  if t == ORDER_AT then
    local prefix = attacker_prefix()
    if not prefix then return fail("no prefix of the attacker's name is its own") end
    if not game.say(DECOY, "!" .. prefix .. " attack " .. (pill_n - 1), "all") then
      return fail("game.say was refused")
    end
    game.log(string.format("%s: attacker ordered at %d", NAME, t))
  end
  local at = game.tank(ATTACKER)
  if at and not at.dead and at.armour < ARMOUR_LOW then
    game.set_stocks(ATTACKER, { armour = 40 })
  end

  -- The decoy goes onto its square PLACE_EARLY ticks before the hint: a
  -- jump that long reads to the brain as a respawn, and a respawn ends any
  -- order it holds, so the jump has to be old news when the order comes.
  if t < HINT_AT - PLACE_EARLY then
    park(DECOY, WX, WY, 0)
    return
  end
  if t < HINT_AT then
    park(DECOY, DX, DY, 0)
    return
  end

  if t == HINT_AT then
    game.set_stocks(DECOY, { armour = 40 })
    local ok, err = game.hint(DECOY, { verb = "goto", x = DX, y = DY, ping = "1" })
    if not ok then return fail("hint refused: %s", tostring(err)) end
    hint_at = t
    return
  end

  local tk = game.tank(DECOY)
  if not tk or tk.dead then return fail("the decoy died at %d", t) end

  -- 1. THE HOLD.
  if not hold_at then
    if t - hint_at >= HOLD_BY then
      return fail("no decoying line %d ticks after the hint", HOLD_BY)
    end
    return
  end

  -- The pill's armour, every tick: the attacker's shells.  The pill is kept
  -- alive through the watch, because a dead pill ends the hold.
  local pl = game.pill(pill_n)
  local parm = pl and pl.armour or 0
  if pill_last and parm < pill_last then
    pill_hits = pill_hits + 1
    if not pill_hit_at then
      pill_hit_at = t
      game.log(string.format("%s: the attacker hit the pill at %d (armour %d->%d)",
                             NAME, t, pill_last, parm))
    end
  end
  if parm > 0 and parm < PILL_LOW and t - hold_at < WATCH_FOR then
    game.set_pill_armour(pill_n, 15)
    parm = 15
  end
  pill_last = parm

  -- 2 and 3. THE WATCH, for the hold window.
  if t - hold_at < WATCH_FOR then
    if not parked_at then
      parked_at, last_arm = t, tk.armour
      sq_x, sq_y, sq_dir = tk.mx, tk.my, tk.dir
      if sq_x ~= DX or sq_y ~= DY then
        return fail("the hold began on (%d,%d), not (%d,%d)", sq_x, sq_y, DX, DY)
      end
    end
    watch_walls()
    return watch(tk)
  end

  -- 4. THE ATTACKER, at the same time as the decoy.
  if not pill_hit_at then
    return fail("the attacker never hit the pill (%d decoy hits)", hits)
  end
  if first_hit_at and pill_hit_at > first_hit_at + ATTACK_SLACK then
    return fail("the attacker's first hit at %d is %d after the decoy's",
                pill_hit_at, pill_hit_at - first_hit_at)
  end

  -- The hits, a few to a log line (a line that is too long is dropped).
  for i = 1, #hit_list, 8 do
    game.log(string.format("%s: hits %s", NAME,
                           table.concat(hit_list, " ", i, math.min(i + 7, #hit_list))))
  end
  game.log(string.format("%s: %d hits, %d armour lost in the watch", NAME, hits, arm_lost))
  for i = 1, #wall_log, 6 do
    game.log(string.format("%s: walls %s", NAME,
                           table.concat(wall_log, " ", i, math.min(i + 5, #wall_log))))
  end
  if #gap_log > 0 then
    game.log(string.format("%s: blocker steps from a full wall, ticks after hit 1: %s",
                           NAME, table.concat(gap_log, " ")))
  end
  if KEEL then
    if hits < 2 then
      return fail("only %d hits on the decoy", hits)
    end
    return finish(string.format("PASS %s: %d hits, no step (knocked to ring %d, last stop %s); pill hit %d times",
                                NAME, hits, max_ring,
                                slack_x and string.format("%d,%d", slack_x, slack_y) or "D",
                                pill_hits))
  end

  if max_ring < 2 then
    return fail("%d step(s) on %d hits; want 2 or more", max_ring, hits)
  end
  if not first_still then
    return fail("it never stopped on a square off the decoy square")
  end
  -- Three pill shells on a full wall leave it with 2 shots (the brain's
  -- count), so at least one step must come with no hit.
  if blk_steps < 1 then
    return fail("no blocker step in %d steps on %d hits", max_ring, hits)
  end
  -- The squares go on a line of their own: a log line or a verdict that is
  -- too long is dropped.
  game.log(string.format("%s: steps %s", NAME, table.concat(step_log, ", ")))
  local trigs = {}
  for i, st in ipairs(step_log) do trigs[i] = st:sub(-3) end
  return finish(string.format("PASS %s: %d steps (%s) on %d hits%s; pill hit %d times",
                              NAME, max_ring, table.concat(trigs, " "), hits,
                              aimed and ", aimed" or "", pill_hits))
end
