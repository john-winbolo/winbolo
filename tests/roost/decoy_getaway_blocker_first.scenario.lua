-- ROOST: a decoy steps off the decoy square to a shielded square while the
-- wall between it and the pill is nearly gone, before any hit, and stays
-- closer to the pill than the human team-mate.
--
-- Recorded game 20261006_011834 (bot0): a decoy held (126,112), pill 12 at
-- (126,117), the human at (129,110).  A wall at (126,113) stopped the
-- pill's shells, so the getaway scan counted no pill (P=[]) and planned no
-- chain.  The wall was damaged at t=5482 and gone at t=5512; only then was
-- there a chain, and the bot stepped to (125,112) on its first hit at
-- t=5529.  Andrew: it should have stepped to (125,112) while the wall was
-- nearly gone.  The fix (decoy_getaway.lua):
--   THE SHIELDED CHAIN (DECOY_GETAWAY_SHIELDED_CHAIN): with P empty the
--     chain is built against the pills behind a blocker, so (125,112) is
--     ready before the wall falls;
--   THE BLOCKER STEP on the decoy square (DECOY_GETAWAY_BLOCKER_STEP_FIRST):
--     the wall's shots left are counted there too (5 less the wall hits it
--     hears), and at <= 2 it steps;
--   STAY THE CLOSEST (DECOY_GETAWAY_STAY_CLOSEST): every chain square is
--     closer to the pill than the human (5.10 < 7.62 for (125,112)).
--
-- The round:
--
--   * The pill (enemy-owned) at (126,117), the decoy square (126,112), a
--     full wall at (126,113) on the pill's line, and a wall row at y=113,
--     x=120..125, that shields row 112 to the west.  building_life is the
--     default, so the pill knocks the line wall down in 5 shells.  Until
--     the hold starts the round puts every wall back to full each tick.
--   * The MATE stands at (129,110) as the human: the decoy's brain reads
--     its seat as human (game.bot_init cfg HUMAN_DECOY_TEST_HUMANS=2).
--   * A goto hint with ping = "1" puts the decoy on the square; it must say
--     "decoying" within HOLD_BY ticks.
--   * THE CHECK.  The decoy DEPARTS when its tank's square is west of the
--     decoy square.  PASS: it departs while the line
--     wall still stands (full or damaged), with no hit taken in the hold,
--     and on a square closer to the pill than the mate.  FAIL: the wall
--     falls first, a hit comes first, it leaves any other way, or the watch
--     ends.
--
-- With the three knobs at their keel values (false) there is no chain
-- before the wall falls, so the decoy stays until the wall is gone and the
-- next shell hits it: FAIL.
--
-- Three seats, `-teams 2,1`: the DECOY and the MATE on team 1, and one enemy
-- on team 2 that owns the pill.  The mate and the enemy have no shells; the
-- enemy is kept in a far corner.
--
-- The map is the order_* island (flat grass from (96,96) to (159,159)).

scenario = {
  name        = "ROOST decoy_getaway_blocker_first",
  description = "A decoy steps to a shielded square while the wall before it is nearly gone, before any hit.",
  api         = 1,
}

local NAME = "decoy_getaway_blocker_first"

local DECOY = 0
local MATE  = 1
local ENEMY = 2

local PX, PY   = 126, 117   -- the pill
local DX, DY   = 126, 112   -- the decoy square
local LX, LY   = 126, 113   -- the wall on the pill's line to the decoy square
local ROW_Y    = 113        -- the wall row that shields row 112 to the west
local ROW_X0, ROW_X1 = 120, 125
local HX, HY   = 129, 110   -- the mate, standing in for the human
local WX, WY   = 128, 112   -- where the decoy waits before the ping
local FX, FY   = 152, 152   -- the enemy's corner

local SETUP_AT    = 150
local HINT_AT     = 300
local PLACE_EARLY = 40
local HOLD_BY     = 200
local WATCH_FOR   = 900

local now      = 0
local done     = false
local pill_n   = nil
local hint_at  = nil
local hold_at  = nil
local hold_arm = nil
local wall_dmg = nil        -- the tick the line wall was first seen damaged

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

local function fail(fmt, ...)
  finish("FAIL " .. NAME .. ": " .. string.format(fmt, ...))
end

local function keep_others()
  game.teleport(ENEMY, FX, FY)
  game.set_stocks(ENEMY, { shells = 0 })
  game.teleport(MATE, HX, HY)
  game.set_stocks(MATE, { shells = 0, armour = 40 })
end

local function walls_full()
  local B = game.TERRAIN.building
  if game.map_tile(LX, LY) ~= B then game.set_tile(LX, LY, B) end
  for x = ROW_X0, ROW_X1 do
    if game.map_tile(x, ROW_Y) ~= B then game.set_tile(x, ROW_Y, B) end
  end
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
  -- The decoy's brain reads the mate's seat (1, bit 2) as human.
  local bok, berr = game.bot_init(DECOY, { cfg = "BOT_CHAT_DEFAULT=true;cfg=HUMAN_DECOY_TEST_HUMANS=2" })
  if not bok then return fail("bot_init refused: %s", tostring(berr)) end
  walls_full()
  pill_n = game.add_pill(PX, PY, ENEMY, 15)
  if not pill_n then return fail("add_pill refused") end
  game.log(string.format("%s: pill %d at (%d,%d), line wall (%d,%d), row y=%d x=%d..%d, mate (%d,%d)",
                         NAME, pill_n, PX, PY, LX, LY, ROW_Y, ROW_X0, ROW_X1, HX, HY))
end

function on_tick(t)
  if done then return end
  now = t

  if t == SETUP_AT then setup() return end
  if t < SETUP_AT then return end

  keep_others()

  if not hold_at then walls_full() end

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
  if not tk or tk.dead then return fail("died at %d", t) end

  if not hold_at then
    if t - hint_at >= HOLD_BY then
      return fail("no decoying line %d ticks after the hint", HOLD_BY)
    end
    return
  end
  hold_arm = hold_arm or tk.armour

  -- Keep the pill alive.
  local pl = game.pill(pill_n)
  local parm = pl and pl.armour or 0
  if parm > 0 and parm < 5 then game.set_pill_armour(pill_n, 15) end

  local lt = game.map_tile(LX, LY)
  local standing = (lt == game.TERRAIN.building or lt == game.TERRAIN.half_building)
  if lt == game.TERRAIN.half_building and not wall_dmg then
    wall_dmg = t
    game.log(string.format("%s: line wall damaged at %d", NAME, t))
  end

  if tk.mx < DX then
    if not standing then
      return fail("departed at %d after the line wall fell", t)
    end
    if tk.armour < hold_arm then
      return fail("departed at %d after a hit (armour %d->%d)", t, hold_arm, tk.armour)
    end
    local dme = math.sqrt((tk.mx - PX) ^ 2 + (tk.my - PY) ^ 2)
    local dh  = math.sqrt((HX - PX) ^ 2 + (HY - PY) ^ 2)
    if dme >= dh then
      return fail("departed to (%d,%d), %.2f from the pill, mate %.2f", tk.mx, tk.my, dme, dh)
    end
    return finish(string.format("PASS %s: wall damaged %s, departed %d to (%d,%d) %.2f<%.2f, no hit",
                                NAME, tostring(wall_dmg), t, tk.mx, tk.my, dme, dh))
  end
  if tk.mx > DX or tk.my ~= DY then
    return fail("left (%d,%d) at %d, to (%d,%d), not west", DX, DY, t, tk.mx, tk.my)
  end
  if tk.armour < hold_arm then
    return fail("hit at %d on (%d,%d) before it left, wall %s", t, tk.mx, tk.my,
                standing and "standing" or "gone")
  end
  if t - hold_at >= WATCH_FOR then
    return fail("no departure in %d ticks, wall %s", WATCH_FOR, standing and "standing" or "gone")
  end
end
