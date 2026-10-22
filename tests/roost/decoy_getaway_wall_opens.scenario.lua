-- ROOST: the wall between the pill and a decoy is shot away mid-hold, and
-- the decoy's FIRST hit after that moves it.
--
-- Recorded game 20261005_232149 (bot4): a decoy held a square with two
-- neutral pills in range, but walls stopped both pills' shells, so the
-- getaway scan counted no pill (P=[]) and planned no chain.  The walls
-- between one pill and the decoy were then shot away.  The hold's
-- look-again check watched only the tank's square and the counted pill ids
-- (a chain's shield squares too, but there was no chain), so nothing
-- started a fresh scan.  The hit that came through the gap did nothing,
-- because there was no chain to step on, and the bot was killed where it
-- stood.  The fix (decoy_getaway.lua): THE BLOCKED WATCH
-- (DECOY_GETAWAY_WATCH_BLOCKED) also watches the square that stopped each
-- blocked pill's shell, and A HIT WITH NO CHAIN LOOKS AGAIN AT ONCE
-- (DECOY_GETAWAY_HIT_RESCAN) and keeps that hit when it finds a chain.
--
-- The round:
--
--   * One pill (enemy-owned) at (128,120).  The decoy square (128,126) is
--     six south of it.  A full wall at (128,123) stands on the pill's line
--     to the decoy square, so the scan at the start of the hold has P=[].
--     building_life is BUILDING_LIFE (200) so the pill cannot shoot that
--     wall down on its own: the round removes it (rubble) OPEN_AFTER ticks
--     into the hold.  A row of walls at y=125, x=129..132, gives the chain
--     its shields once the pill counts (the decoy_getaway arena's layout).
--   * A goto hint with ping = "1" puts the decoy on the square; it must say
--     "decoying" within HOLD_BY ticks.
--   * THE CHECK.  After the wall opens, the pill's shells reach the decoy.
--     A pill shell comes straight south, so a knock moves the tank down its
--     own column.  A getaway step goes east, behind the walls.  The decoy
--     DEPARTS when its x moves more than DEPART_WU from where it stood when
--     the wall opened.  PASS: it departs after the first hit since the wall
--     opened and before the second.  FAIL: a second hit comes first (the
--     first hit did not move it), it departs with no hit since the wall
--     opened, it dies first, or the watch ends with no departure.
--
-- On origin/main before the fix the first hit does nothing (no chain, no
-- fresh scan), so the second hit comes while it is still parked: FAIL.
--
-- Three seats, `-teams 2,1`: the DECOY and a MATE on team 1, and one enemy
-- on team 2 that owns the pill (so the pill shoots team 1).  The mate and
-- the enemy are kept in far corners with no shells.  (The mate is there
-- because a bot with no team-mate says nothing on team chat, and the round
-- needs the decoy's "decoying" line.)
--
-- The map is the order_* island (flat grass from (96,96) to (159,159)).

scenario = {
  name        = "ROOST decoy_getaway_wall_opens",
  description = "The wall between a pill and a decoy is shot away mid-hold; the first hit after that moves the decoy.",
  api         = 1,
}

local NAME = "decoy_getaway_wall_opens"

local DECOY = 0
local MATE  = 1
local ENEMY = 2

local PX, PY   = 128, 120   -- the pill
local DX, DY   = 128, 126   -- the decoy square: six south, in range
local BX, BY   = 128, 123   -- the wall on the pill's line to the decoy square
local WALL_Y   = 125        -- the chain's shields, just east of the decoy
local WALL_X0, WALL_X1 = 129, 132
local WX, WY   = 125, 126   -- where the decoy waits before the ping: it sees
                            -- the pill from here, past the line wall
local FX, FY   = 152, 152   -- the enemy's corner
local MX, MY   = 100, 156   -- the mate's corner

local SETUP_AT    = 150
local HINT_AT     = 300
local PLACE_EARLY = 40      -- ticks on the decoy square before the hint
local HOLD_BY     = 200     -- ticks after the hint to hear "decoying" in
local OPEN_AFTER  = 300     -- ticks into the hold the wall is removed
local WATCH_FOR   = 960     -- ticks of the hold that are watched (it runs 1000)
local DEPART_WU   = 96      -- x movement that counts as a departure
local BUILDING_LIFE = 200   -- the pill cannot knock the line wall down
local PILL_LOW    = 5       -- the pill's armour is put back to 15 below this

local now       = 0
local done      = false
local pill_n    = nil
local hint_at   = nil
local hold_at   = nil
local open_at   = nil
local open_wx   = nil       -- the decoy's x (world units) when the wall opened
local last_arm  = nil
local hits_pre  = 0         -- hits before the wall opened
local hits_post = 0         -- hits since the wall opened
local first_post = nil      -- the tick of the first of them
local hit_list  = {}

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

-- A game.log line and the end_round text are cut off at the chat limit
-- (127 bytes, "ROOST VERDICT " included: a longer one is refused), so the
-- verdict stays short and the hit list goes on its own line first.
local function hits_line()
  game.log(string.format("%s: hits %s", NAME, table.concat(hit_list, " ")):sub(1, 127))
end

local function fail(fmt, ...)
  hits_line()
  finish("FAIL " .. NAME .. ": " .. string.format(fmt, ...))
end

local function keep_enemy()
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

local function setup()
  for p = 0, 2 do
    game.builder_recall(p)
    game.set_stocks(p, { shells = 40, armour = 40, trees = 0 })
  end
  local rok, rerr = game.set_rule("building_life", BUILDING_LIFE)
  if not rok then return fail("set_rule building_life refused: %s", tostring(rerr)) end
  local ok, err = game.set_tile(BX, BY, game.TERRAIN.building)
  if not ok then return fail("set_tile (%d,%d) refused: %s", BX, BY, tostring(err)) end
  for x = WALL_X0, WALL_X1 do
    ok, err = game.set_tile(x, WALL_Y, game.TERRAIN.building)
    if not ok then return fail("set_tile (%d,%d) refused: %s", x, WALL_Y, tostring(err)) end
  end
  pill_n = game.add_pill(PX, PY, ENEMY, 15)
  if not pill_n then return fail("add_pill refused") end
  game.log(string.format("%s: pill %d at (%d,%d), line wall (%d,%d), walls y=%d x=%d..%d",
                         NAME, pill_n, PX, PY, BX, BY, WALL_Y, WALL_X0, WALL_X1))
end

function on_tick(t)
  if done then return end
  now = t

  if t == SETUP_AT then setup() return end
  if t < SETUP_AT then return end

  keep_enemy()

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
    return fail("died at %d, %d hits before the wall opened, %d after", t, hits_pre, hits_post)
  end

  if not hold_at then
    if t - hint_at >= HOLD_BY then
      return fail("no decoying line %d ticks after the hint", HOLD_BY)
    end
    return
  end

  -- Keep the pill alive: a dead pill ends the hold.
  local pl = game.pill(pill_n)
  local parm = pl and pl.armour or 0
  if parm > 0 and parm < PILL_LOW then game.set_pill_armour(pill_n, 15) end

  if t - hold_at >= WATCH_FOR then
    return fail("no departure in the watch, %d hits before the wall opened, %d after",
                hits_pre, hits_post)
  end

  -- The hits on the decoy.
  if last_arm and tk.armour < last_arm then
    hit_list[#hit_list + 1] = string.format("%d@%d,%d", t, tk.mx, tk.my)
    if open_at then
      hits_post = hits_post + 1
      first_post = first_post or t
    else
      hits_pre = hits_pre + 1
    end
    game.log(string.format("%s: hit on (%d,%d) at %d, armour %d->%d, %s",
                           NAME, tk.mx, tk.my, t, last_arm, tk.armour,
                           open_at and ("hit " .. hits_post .. " since the wall opened")
                           or "before the wall opened"))
  end
  last_arm = tk.armour

  -- Open the wall.
  if not open_at then
    if t - hold_at >= OPEN_AFTER then
      if tk.mx ~= DX or tk.my ~= DY then
        return fail("the decoy left (%d,%d) before the wall opened: on (%d,%d) at %d",
                    DX, DY, tk.mx, tk.my, t)
      end
      local ok, err = game.set_tile(BX, BY, game.TERRAIN.rubble)
      if not ok then return fail("set_tile rubble (%d,%d) refused: %s", BX, BY, tostring(err)) end
      open_at, open_wx = t, tk.wx
      game.log(string.format("%s: wall (%d,%d) removed at %d (hold began %d), decoy wx=%d",
                             NAME, BX, BY, t, hold_at, tk.wx))
    end
    return
  end

  -- THE CHECK.
  if math.abs(tk.wx - open_wx) > DEPART_WU then
    if hits_post == 1 then
      hits_line()
      return finish(string.format("PASS %s: wall open %d, hit %d, departed %d (wx %d->%d)",
                                  NAME, open_at, first_post, t, open_wx, tk.wx))
    end
    return fail("departed at %d with %d hits since the wall opened (want 1)", t, hits_post)
  end
  if hits_post >= 2 then
    return fail("wall open %d, hit 1 at %d did not move it, hit 2 at %d", open_at, first_post, t)
  end
end
