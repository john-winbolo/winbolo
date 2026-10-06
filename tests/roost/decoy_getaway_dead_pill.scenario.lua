-- ROOST: a pill that has just died still counts for the decoy getaway,
-- because its shells are still in flight.
--
-- Recorded game 20261006_002825 (bot2): a decoy held (138,119).  Pill 4 at
-- (145,118) died at t=3152.  The t=3169 getaway scan dropped it from the
-- counted pills (P=[14]), so the chain ran west along row 118, on pill 4's
-- line.  The wall at (139,119) broke at about t=3171 and pill 4's shells
-- that were still in flight hit the bot at t=3183, 3193 and 3203.  The fix
-- (decoy_getaway.lua): THE RECENTLY DEAD PILL
-- (DECOY_GETAWAY_DEAD_PILL_TICKS, 40 brain ticks) keeps a pill that died
-- that recently in the getaway's counted pills, P and the chain search.
-- The hold's own pill count (orders.decoy_pills) does not change.
--
-- The round:
--
--   * Pill A (enemy-owned) at (128,120).  The decoy square (128,126) is six
--     south of it.  A full wall at (128,123) stands on pill A's line, so the
--     scan at the start of the hold has P=[].  A row of walls at y=125,
--     x=129..132, gives a chain its shields from pill A (the decoy_getaway
--     arena's layout).
--   * Pill B (enemy-owned) at (128,133), seven south, keeps the hold alive
--     after pill A dies (a hold ends when no live pill is in range).  A full
--     wall at (128,130) stands on pill B's line, so pill B is never in P.
--     The round puts that wall back if it ever falls.
--   * building_life is BUILDING_LIFE (200), so the pills cannot shoot the
--     line walls down on their own.
--   * A goto hint with ping = "1" puts the decoy on the square; it must say
--     "decoying" within HOLD_BY ticks.
--   * KILL_AFTER ticks into the hold the round kills pill A (armour 0).
--     OPEN_AFTER ticks after that it removes pill A's line wall (rubble), as
--     the wall broke after the pill died in the recorded game.  The blocked
--     watch sees the wall go and scans on the next think.  With the fix,
--     pill A (dead some 10 brain ticks) is in P and the scan finds a chain.
--   * HIT_AFTER ticks after the wall opens the round takes HIT_ARMOUR off
--     the decoy (game.set_stocks), as a shell in flight would.
--   * THE CHECK.  A getaway step goes east, behind the walls.  The decoy
--     DEPARTS when its x moves more than DEPART_WU from where it stood when
--     the wall opened.  PASS: it departs within DEPART_BY ticks of the hit.
--     FAIL: no departure by then, it departs before the hit, or it dies.
--
-- With DECOY_GETAWAY_DEAD_PILL_TICKS = 0 (the keel value, and origin/main)
-- the scan drops dead pill A at once: P=[], no chain, so the hit does not
-- move the decoy: FAIL.
--
-- Three seats, `-teams 2,1`: the DECOY and a MATE on team 1, and one enemy
-- on team 2 that owns both pills (so they shoot team 1).  The mate and the
-- enemy are kept in far corners with no shells.  (The mate is there because
-- a bot with no team-mate says nothing on team chat, and the round needs
-- the decoy's "decoying" line.)
--
-- The map is the order_* island (flat grass from (96,96) to (159,159)).

scenario = {
  name        = "ROOST decoy_getaway_dead_pill",
  description = "A pill that has just died still counts for the decoy getaway; a hit after its wall opens moves the decoy.",
  api         = 1,
}

local NAME = "decoy_getaway_dead_pill"

local DECOY = 0
local MATE  = 1
local ENEMY = 2

local AX, AY   = 128, 120   -- pill A: dies mid-hold
local BX, BY   = 128, 133   -- pill B: keeps the hold alive
local DX, DY   = 128, 126   -- the decoy square: in range of both
local LAX, LAY = 128, 123   -- the wall on pill A's line to the decoy square
local LBX, LBY = 128, 130   -- the wall on pill B's line to the decoy square
local WALL_Y   = 125        -- the chain's shields from pill A, just east
local WALL_X0, WALL_X1 = 129, 132
local WX, WY   = 125, 126   -- where the decoy waits before the ping
local FX, FY   = 152, 152   -- the enemy's corner
local MX, MY   = 100, 156   -- the mate's corner

local SETUP_AT    = 150
local HINT_AT     = 300
local PLACE_EARLY = 40      -- ticks on the decoy square before the hint
local HOLD_BY     = 200     -- ticks after the hint to hear "decoying" in
local KILL_AFTER  = 300     -- ticks into the hold pill A is killed
local OPEN_AFTER  = 20      -- ticks after that pill A's line wall is removed
local HIT_AFTER   = 10      -- ticks after that the decoy loses HIT_ARMOUR
local HIT_ARMOUR  = 5
local DEPART_BY   = 120     -- ticks after the hit to depart in (a step takes about 64)
local DEPART_WU   = 96      -- x movement that counts as a departure
local BUILDING_LIFE = 200
local PILL_LOW    = 5       -- pill B's armour is put back to 15 below this

local now      = 0
local done     = false
local pill_a   = nil
local pill_b   = nil
local hint_at  = nil
local hold_at  = nil
local kill_at  = nil
local open_at  = nil
local open_wx  = nil        -- the decoy's x (world units) when the wall opened
local hit_at   = nil

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

-- A game.log line and the end_round text are refused over 127 bytes
-- ("ROOST VERDICT " included), so the verdicts stay short.
local function fail(fmt, ...)
  finish("FAIL " .. NAME .. ": " .. string.format(fmt, ...))
end

local function keep_others()
  game.teleport(ENEMY, FX, FY)
  game.set_stocks(ENEMY, { shells = 0 })
  game.teleport(MATE, MX, MY)
  game.set_stocks(MATE, { shells = 0 })
end

function on_chat(p, text, scripted)
  if scripted or done or p ~= DECOY or hold_at then return end
  if text:find("decoying", 1, true) then
    hold_at = now
    game.log(string.format("%s: said %q at %d", NAME, text, now))
  elseif text:find("holding", 1, true) then
    fail("the hint gave a plain hold (%q), not a decoy hold", text)
  end
end

function on_start()
  if not (game.tank(DECOY) and game.tank(MATE) and game.tank(ENEMY)) then
    fail("the round needs three seated bots")
  end
end

local function wall(x, y)
  local ok, err = game.set_tile(x, y, game.TERRAIN.building)
  if not ok then fail("set_tile (%d,%d) refused: %s", x, y, tostring(err)) end
  return ok
end

local function setup()
  for p = 0, 2 do
    game.builder_recall(p)
    game.set_stocks(p, { shells = 40, armour = 40, trees = 0 })
  end
  local rok, rerr = game.set_rule("building_life", BUILDING_LIFE)
  if not rok then return fail("set_rule building_life refused: %s", tostring(rerr)) end
  if not wall(LAX, LAY) or not wall(LBX, LBY) then return end
  for x = WALL_X0, WALL_X1 do
    if not wall(x, WALL_Y) then return end
  end
  pill_a = game.add_pill(AX, AY, ENEMY, 15)
  pill_b = game.add_pill(BX, BY, ENEMY, 15)
  if not pill_a or not pill_b then return fail("add_pill refused") end
  game.log(string.format("%s: pill A %d (%d,%d), pill B %d (%d,%d), line walls (%d,%d) (%d,%d)",
                         NAME, pill_a, AX, AY, pill_b, BX, BY, LAX, LAY, LBX, LBY))
end

function on_tick(t)
  if done then return end
  now = t

  if t == SETUP_AT then setup() return end
  if t < SETUP_AT then return end

  keep_others()

  if t < HINT_AT - PLACE_EARLY then
    game.teleport(DECOY, WX, WY, 0)
    return
  end
  if t < HINT_AT then
    game.teleport(DECOY, DX, DY, 0)
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
  if not tk or tk.dead then
    return fail("died at %d", t)
  end

  if not hold_at then
    if t - hint_at >= HOLD_BY then
      return fail("no decoying line %d ticks after the hint", HOLD_BY)
    end
    return
  end

  -- Pill B keeps the hold alive and stays behind its wall.
  local pl = game.pill(pill_b)
  local parm = pl and pl.armour or 0
  if parm > 0 and parm < PILL_LOW then game.set_pill_armour(pill_b, 15) end
  local tl = game.map_tile(LBX, LBY)
  if tl and tl ~= game.TERRAIN.building then
    game.set_tile(LBX, LBY, game.TERRAIN.building)
    game.log(string.format("%s: pill B's wall put back at %d", NAME, t))
  end

  if not kill_at then
    if t - hold_at >= KILL_AFTER then
      if tk.mx ~= DX or tk.my ~= DY then
        return fail("the decoy left (%d,%d) before pill A died: on (%d,%d) at %d",
                    DX, DY, tk.mx, tk.my, t)
      end
      game.set_pill_armour(pill_a, 0)
      kill_at = t
      game.log(string.format("%s: pill A killed at %d (hold began %d)", NAME, t, hold_at))
    end
    return
  end

  if not open_at then
    if t - kill_at >= OPEN_AFTER then
      local ok, err = game.set_tile(LAX, LAY, game.TERRAIN.rubble)
      if not ok then return fail("set_tile rubble (%d,%d) refused: %s", LAX, LAY, tostring(err)) end
      open_at, open_wx = t, tk.wx
      game.log(string.format("%s: wall (%d,%d) removed at %d, decoy wx=%d", NAME, LAX, LAY, t, tk.wx))
    end
    return
  end

  if math.abs(tk.wx - open_wx) > DEPART_WU then
    if not hit_at then
      return fail("departed at %d before the hit (wall open %d)", t, open_at)
    end
    return finish(string.format("PASS %s: pill dead %d, wall open %d, hit %d, departed %d",
                                NAME, kill_at, open_at, hit_at, t))
  end

  if not hit_at then
    if t - open_at >= HIT_AFTER then
      game.set_stocks(DECOY, { armour = tk.armour - HIT_ARMOUR })
      hit_at = t
      game.log(string.format("%s: hit at %d, armour %d->%d", NAME, t, tk.armour, tk.armour - HIT_ARMOUR))
    end
    return
  end

  if t - hit_at >= DEPART_BY then
    return fail("pill dead %d, wall open %d, hit %d: no departure in %d ticks",
                kill_at, open_at, hit_at, DEPART_BY)
  end
end
