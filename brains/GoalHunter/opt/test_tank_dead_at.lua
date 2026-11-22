-- =========================================================================
-- test_tank_dead_at.lua — standalone unit tests (2026-11-03) for the
-- EVENT_TANK_KILLED stamp in world.process_events:
--   * EVENT_TANK_KILLED data is [killer, killed, deathCause, carriedPills]
--     (input_packet.h, serverSimCbTankKill), so state.tank_dead_at must be
--     keyed by d[2], the tank that died, and never by d[1], the killer
--   * a pillbox kill (killer 255 = NEUTRAL) stamps the dead tank, not 255
--   * a death with no killer (drowning, own mine: killer == killed) stamps
--     that tank
--   * a second death of the same tank moves its stamp to the new tick
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_tank_dead_at.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

_G.EVENT_TANK_KILLED = _G.EVENT_TANK_KILLED or 6

local W = require("world")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end

local function kill(state, tick, killer, killed, cause)
  state.tick = tick
  local world = { pills = {}, bases = {} }
  local info = { events = {
    { type = EVENT_TANK_KILLED, data = { killer, killed, cause or 2, 0 } },
  } }
  W.process_events(world, info, state)
end

-- ── a tank kills another tank ────────────────────────────────────────────
do
  local state = {}
  kill(state, 500, 3, 5)
  local dead = state.tank_dead_at or {}
  check("tank kill: the dead tank (p5) is stamped", dead[5] == 500, dead[5])
  check("tank kill: the killer (p3) is not stamped", dead[3] == nil, dead[3])
end

-- ── a pillbox kills a tank ───────────────────────────────────────────────
do
  local state = {}
  kill(state, 700, 255, 4)
  local dead = state.tank_dead_at or {}
  check("pill kill: the dead tank (p4) is stamped", dead[4] == 700, dead[4])
  check("pill kill: NEUTRAL (255) is not stamped", dead[255] == nil, dead[255])
end

-- ── a tank drowns (killer == killed) ─────────────────────────────────────
do
  local state = {}
  kill(state, 900, 6, 6, 1)
  local dead = state.tank_dead_at or {}
  check("drowning: the dead tank (p6) is stamped", dead[6] == 900, dead[6])
end

-- ── the same tank dies again later ───────────────────────────────────────
do
  local state = {}
  kill(state, 100, 1, 2)
  kill(state, 250, 0, 2)
  local dead = state.tank_dead_at or {}
  check("second death: p2 has the new tick", dead[2] == 250, dead[2])
  check("second death: neither killer is stamped",
        dead[0] == nil and dead[1] == nil, tostring(dead[0]) .. "," .. tostring(dead[1]))
end

print(string.format("test_tank_dead_at: %d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
