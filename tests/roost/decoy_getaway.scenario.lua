-- ROOST: a decoy on a ping square steps out of the pill's line, one square
-- per hit.
--
-- The decoy hold (orders.lua, GO-THERE DECOY HARD HOLD) parks a bot on a
-- square a pillbox can shoot, so the pill spends its shells on the decoy
-- while a team-mate takes it down.  The DECOY GETAWAY (decoy_getaway.lua,
-- Andrew, Sep 24) plans a way out the moment the hold starts: a chain of
-- squares, each one ring further out from the decoy square, each one
-- shielded from the pill by a wall.  The bot parks facing the first square
-- of the chain.  Every hit it takes while parked moves it ONE square along
-- the chain, and then it waits for the next hit.  Hits on the way do not
-- count.
--
-- What this round reads, from outside:
--
--   1. THE HOLD.  A goto hint with ping = "1" (the scenario's stand-in for a
--      bot-command ping, which no scenario op can place) sends the decoy to
--      a square six south of the pill, beside a row of walls.  It says
--      "decoying" -- the hold, not the plain goto hold.
--   2. NO MOVE WITHOUT A HIT.  While parked, the tank's square does not
--      change until its armour drops.
--   3. ONE HIT, ONE STEP.  After a hit it moves to a square next to the one
--      it was on, one ring further out from the decoy square, and then it
--      stops there.  Where the hit came late enough for the turn to be done
--      (TURN_TIME after it parked), the gun was already pointing at the
--      square it then drove to.  The first step goes to FIRST_STEP, which is
--      the first square of the chain DECOY_GETAWAY_SCAN logs for this map.
--   4. THE ATTACKER.  After the hold window, the other bot on our team is
--      ordered "!attack <pill>" and the pill's armour has to go down.
--
-- The decoy_getaway_keel round is this script with KEEL = true: the decoy is
-- given cfg=DECOY_GETAWAY=false before the ping, and it must take hits and
-- never step away.  A hit can knock it one square over, where the old hold
-- leaves it (one square of slack counts as on the square); that square must
-- not be the chain's first square, and it must never be further out.
--
-- Three seats, `-teams 2,1`: the DECOY and the ATTACKER on team 1, one
-- enemy on team 2 that owns the pill (so the pill shoots at team 1) and is
-- kept in a far corner with no shells.  The attacker is kept twelve squares
-- north of the pill, out of its range, until its order: a free bot beside a
-- hostile pill goes for it on its own, and the decoy phase is about the
-- decoy alone.
--
-- The map is the order_* island (flat grass from (96,96) to (159,159)).

local KEEL = false

scenario = {
  name        = KEEL and "ROOST decoy_getaway_keel" or "ROOST decoy_getaway",
  description = KEEL and "A decoy with the getaway off takes hits and never moves."
                or "A decoy steps out of the pill's line one square per hit, then the attacker shoots the pill.",
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
local HINT_AT    = 300
local PLACE_EARLY = 40     -- ticks on the decoy square before the hint
local HOLD_BY    = 200     -- ticks after the hint to hear "decoying" in
local WATCH_FOR  = 900     -- ticks of the hold that are watched (it runs 1000)
local MOVE_BY    = 250     -- ticks after a hit to have left the square in
local TURN_TIME  = 60      -- ticks parked before the facing is checked
local AIM_SLACK  = 24      -- of 256
local ATTACK_BY  = 1500    -- ticks after the order for the pill to lose armour
local ARMOUR_LOW = 25      -- refill below this, so no flee rule can fire
local SETTLE     = 30      -- ticks on one square before it counts as the square

local now       = 0
local done      = false
local pill_n    = nil
local hint_at   = nil
local hold_at   = nil
local hold_text = nil

-- The watch.
local mode      = "wait"   -- "wait" parked, "move" hit and not yet on a new square
local tile_x, tile_y = nil, nil
local parked_at = nil
local last_arm  = nil
local hit_at    = nil
local hit_dir   = nil
local hit_late  = false
local hits      = 0        -- hits taken while parked
local steps     = 0
local aimed     = 0        -- steps whose facing was checked and was right
local first_to  = nil
local knocks    = 0        -- times a hit knocked it over an edge and it came back
local raw_x, raw_y, raw_for = nil, nil, 0
local slack_x, slack_y = nil, nil  -- KEEL: the square a knock left it on, if any
local stopped   = false    -- a hit that did not move it: the chain is over

local order_at  = nil
local pill_arm0 = nil

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
--
-- A SQUARE is where the tank has SETTLED: on one square for SETTLE ticks in
-- a row.  A shell that hits a tank knocks it back, and a knock can carry it
-- over a square's edge for a moment; the hold drives it straight back.  That
-- is not a step, so the watch only reads a square the tank stays on.
local function watch(tk)
  if tk.armour < ARMOUR_LOW then
    game.set_stocks(DECOY, { armour = 40 })
  end
  local arm = tk.armour
  local hit = last_arm and arm < last_arm
  last_arm = arm
  if tk.armour < ARMOUR_LOW then last_arm = 40 end

  if tk.mx ~= raw_x or tk.my ~= raw_y then
    if raw_x and (raw_x ~= tile_x or raw_y ~= tile_y) and raw_for < SETTLE then
      knocks = knocks + 1
    end
    raw_x, raw_y, raw_for = tk.mx, tk.my, 0
  end
  raw_for = raw_for + 1
  local moved = (raw_x ~= tile_x or raw_y ~= tile_y) and raw_for >= SETTLE

  if mode == "wait" then
    if hit then
      hits = hits + 1
      if not KEEL then
        mode, hit_at, hit_dir = "move", now, tk.dir
        hit_late = (now - parked_at) >= TURN_TIME
        game.log(string.format("%s: hit %d on (%d,%d) at %d, facing %d, parked %d",
                               NAME, hits, tile_x, tile_y, now, tk.dir, now - parked_at))
        return
      end
    end
    if moved then
      -- With the getaway off, a hit can leave the tank on the next square
      -- over: the old hold counts one square of slack as "on it" and does
      -- not drive back.  That is the knock, not a step, so KEEL only fails
      -- on a square further out, or on the chain's first square.
      if KEEL then
        local d = math.max(math.abs(raw_x - DX), math.abs(raw_y - DY))
        local first = FIRST_STEP and raw_x == FIRST_STEP[1] and raw_y == FIRST_STEP[2]
        if d > 1 or first then
          return fail("with the getaway off it went to (%d,%d) at %d (%d hits)",
                      raw_x, raw_y, now, hits)
        end
        slack_x, slack_y = raw_x, raw_y
        return
      end
      return fail("left (%d,%d) for (%d,%d) at %d with no hit (%d steps)",
                  tile_x, tile_y, raw_x, raw_y, now, steps)
    end
    return
  end

  -- mode == "move": hits on the way do not count.
  if not moved then
    if now - hit_at >= MOVE_BY then
      if steps == 0 then
        return fail("hit at %d on the decoy square and still there %d later",
                    hit_at, MOVE_BY)
      end
      -- The chain is over: a hit on its last square moves nothing.
      stopped = true
      mode, parked_at = "wait", now
      game.log(string.format("%s: no move after the hit at %d: end of the chain",
                             NAME, hit_at))
    end
    return
  end
  local nx, ny = raw_x, raw_y
  local cheb = math.max(math.abs(nx - tile_x), math.abs(ny - tile_y))
  if cheb ~= 1 then
    return fail("one hit took it from (%d,%d) to (%d,%d)", tile_x, tile_y, nx, ny)
  end
  if ring(nx, ny) ~= ring(tile_x, tile_y) + 1 then
    return fail("step (%d,%d)->(%d,%d) is not one ring further out",
                tile_x, tile_y, nx, ny)
  end
  if stopped then
    return fail("moved again after the chain had ended, to (%d,%d)", nx, ny)
  end
  if steps == 0 and FIRST_STEP and (nx ~= FIRST_STEP[1] or ny ~= FIRST_STEP[2]) then
    return fail("first step went to (%d,%d), the chain says (%d,%d)",
                nx, ny, FIRST_STEP[1], FIRST_STEP[2])
  end
  if hit_late then
    local want = bearing(tile_x, tile_y, nx, ny)
    local gap = turn_gap(hit_dir, want)
    if gap > AIM_SLACK then
      return fail("parked facing %d, then went to (%d,%d) at bearing %d",
                  hit_dir, nx, ny, math.floor(want))
    end
    aimed = aimed + 1
  end
  steps = steps + 1
  if steps == 1 then first_to = { nx, ny } end
  game.log(string.format("%s: step %d (%d,%d)->(%d,%d), settled at %d, %d after the hit%s",
                         NAME, steps, tile_x, tile_y, nx, ny, now, now - hit_at,
                         hit_late and "" or " (hit before the turn: facing not checked)"))
  tile_x, tile_y = nx, ny
  mode, parked_at = "wait", now
end

function on_tick(t)
  if done then return end
  now = t

  if t == SETUP_AT then setup() return end
  if t < SETUP_AT then return end

  keep_enemy()
  if not order_at then park(ATTACKER, AX, AY, 128) end

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

  -- 2 and 3. THE WATCH.
  if t - hold_at < WATCH_FOR then
    if not tile_x then
      tile_x, tile_y, parked_at, last_arm = tk.mx, tk.my, t, tk.armour
      if tile_x ~= DX or tile_y ~= DY then
        return fail("the hold began on (%d,%d), not (%d,%d)", tile_x, tile_y, DX, DY)
      end
    end
    return watch(tk)
  end

  if KEEL then
    if hits < 2 then
      return fail("only %d hits on the decoy square in %d ticks", hits, WATCH_FOR)
    end
    return finish(string.format("PASS %s: %q, %d hits on (%d,%d), no step (%d knocks, left on %s)",
                                NAME, hold_text, hits, DX, DY, knocks,
                                slack_x and string.format("%d,%d", slack_x, slack_y) or "the square"))
  end

  if steps == 0 then
    return fail("%d hits and no step in %d ticks", hits, WATCH_FOR)
  end

  -- 4. THE ATTACKER.
  if not order_at then
    local p = game.pill(pill_n)
    pill_arm0 = p and p.armour or 0
    local ok = game.say(DECOY, "!attack " .. (pill_n - 1), "all")
    if not ok then return fail("game.say was refused") end
    order_at = t
    game.log(string.format("%s: attacker ordered at %d, pill armour %d",
                           NAME, t, pill_arm0))
    return
  end
  local p = game.pill(pill_n)
  local arm = p and p.armour or 0
  if arm < pill_arm0 then
    return finish(string.format(
      "PASS %s: %d steps (first %d,%d; %d aimed) on %d hits; pill %d->%d at +%d",
      NAME, steps, first_to[1], first_to[2], aimed, hits, pill_arm0, arm, t - order_at))
  end
  if t - order_at >= ATTACK_BY then
    return fail("the pill is still at armour %d %d ticks after the order",
                arm, ATTACK_BY)
  end
end
