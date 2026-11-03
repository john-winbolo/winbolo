-- =========================================================================
-- test_kill_pickup_lethal.lua — standalone unit tests for the fresh-kill
-- claim's LETHAL exit (knob C.KILL_PICKUP_LETHAL_EXIT):
--   * goals.kill_pickup_lethal_verdict — the check Override 3b asks. A non-nil
--     answer drops the claim; nil keeps it.
-- The verdict is the one capture_route_probe stores in state.capture_route;
-- these tests put entries there by hand. No engine, no A*, no wsim.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_kill_pickup_lethal.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C      = require("constants")
local G      = require("goals")
local threat = require("threat")

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

local DEFAULT_KNOB = C.KILL_PICKUP_LETHAL_EXIT
local PID  = 10
local PILL = { mx = 126, my = 141, health = 0 }
local NOW  = 6726
local TMX, TMY = 123, 141

-- One capture_route entry, as capture_route_probe writes it after the split
-- probe's danger half (the recorded game: direct 2 refused, LETHAL, 10 dmg).
local function verdict(over)
  local ent = { id = PID, tick = 6725, tmx = TMX, tmy = TMY,
                direct_cost = 2, killed = true, damage = 10, npath = 2,
                mode = "danger", cost = 268 }
  if over then for k, v in pairs(over) do ent[k] = v end end
  return { capture_route = { [PILL.my * 256 + PILL.mx] = ent } }
end
local function ask(state, now, tmx, tmy)
  return G.kill_pickup_lethal_verdict(state, PILL, PID, now or NOW, tmx or TMX, tmy or TMY)
end

threat.last_rebuild_tick = 0

print("goals.kill_pickup_lethal_verdict")
C.KILL_PICKUP_LETHAL_EXIT = true
check("knob on, fresh LETHAL verdict -> exit", ask(verdict()) ~= nil)
check("knob on, verdict still pending its danger half -> exit",
      ask(verdict({ mode = "pending_danger", cost = nil })) ~= nil)
check("knob on, danger A* found no route (mode none) -> exit",
      ask(verdict({ mode = "none", cost = nil })) ~= nil)
check("knob on, wsim said we live -> keep",
      ask(verdict({ killed = false, mode = "direct", cost = 2 })) == nil)
check("knob on, no-builder strict refusal that is not lethal -> keep",
      ask(verdict({ killed = false, strict = true, damage = 4 })) == nil)
check("knob on, no verdict at all -> keep", ask({}) == nil)
check("knob on, verdict for another pill on the tile -> keep",
      ask(verdict({ id = PID + 1 })) == nil)
check("knob on, verdict older than CAPTURE_ROUTE_TTL -> keep",
      ask(verdict(), 6725 + C.CAPTURE_ROUTE_TTL + 1) == nil)
check("knob on, verdict exactly CAPTURE_ROUTE_TTL old -> exit",
      ask(verdict(), 6725 + C.CAPTURE_ROUTE_TTL) ~= nil)
check("knob on, tank moved CAPTURE_ROUTE_MOVE_TILES since the probe -> keep",
      ask(verdict(), NOW, TMX - C.CAPTURE_ROUTE_MOVE_TILES, TMY) == nil)
threat.last_rebuild_tick = NOW
check("knob on, threat rebuilt after the probe -> keep", ask(verdict()) == nil)
threat.last_rebuild_tick = 0

C.KILL_PICKUP_LETHAL_EXIT = false
check("knob off, fresh LETHAL verdict -> keep (old behaviour)", ask(verdict()) == nil)

C.KILL_PICKUP_LETHAL_EXIT = DEFAULT_KNOB
check("default knob is true", C.KILL_PICKUP_LETHAL_EXIT == true, C.KILL_PICKUP_LETHAL_EXIT)
check("PRESETS.keel pins the knob to false",
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.KILL_PICKUP_LETHAL_EXIT == false,
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.KILL_PICKUP_LETHAL_EXIT)

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
