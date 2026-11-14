-- =========================================================================
-- test_kill_pickup_lethal.lua — standalone unit tests for the fresh-kill
-- claim's LETHAL exit (knob C.KILL_PICKUP_LETHAL_EXIT) and its replan
-- request (knob C.KILL_PICKUP_LETHAL_REPLAN):
--   * goals.kill_pickup_lethal_verdict — the check Override 3b asks. A non-nil
--     answer drops the claim; nil keeps it.
--   * goals.kill_pickup_lethal_replan — raises _force_replan_reason once per
--     LETHAL verdict on the claimed body, so Override 3b runs at once.
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

-- ── goals.kill_pickup_lethal_replan (KILL_PICKUP_LETHAL_REPLAN) ──────────
-- A state with the active claim on PID and the kill-grab goal on it, plus the
-- recorded LETHAL verdict. The world holds the dead body.
local DEFAULT_REPLAN = C.KILL_PICKUP_LETHAL_REPLAN
local WORLD = { pills = { [PID] = PILL } }
local function claim_state(over)
  local st = verdict(over)
  st.kill_pickup = { id = PID, kill_tick = 6700, created_tick = 6650 }
  st.goal = { kind = "capture_pill", mx = PILL.mx, my = PILL.my,
              target_id = PID, race_mode = true, kill_grab = true }
  return st
end
local function replan(st, now, tmx, tmy)
  return G.kill_pickup_lethal_replan(st, WORLD, now or NOW, tmx or TMX, tmy or TMY)
end

print("goals.kill_pickup_lethal_replan")
C.KILL_PICKUP_LETHAL_EXIT = true
C.KILL_PICKUP_LETHAL_REPLAN = true
do
  local st = claim_state()
  local r = replan(st)
  check("knob on, LETHAL verdict for the claimed body -> replan requested",
        r == true and st._force_replan_reason == "KILL PICKUP LETHAL",
        tostring(r) .. " " .. tostring(st._force_replan_reason))
  check("marker holds the verdict tick and pill",
        st._kp_lethal_replan_tick == 6725 and st._kp_lethal_replan_pid == PID,
        tostring(st._kp_lethal_replan_tick) .. "/" .. tostring(st._kp_lethal_replan_pid))
  -- init.lua consumes the flag on the replan decision.
  st._force_replan_reason = nil
  local r2 = replan(st, NOW + 1)
  check("same verdict next tick -> no second request",
        r2 == false and st._force_replan_reason == nil, st._force_replan_reason)
  -- The split probe's danger half moves the mode on; the verdict tick stays.
  st.capture_route[PILL.my * 256 + PILL.mx].mode = "pending_danger"
  local r3 = replan(st, NOW + 2)
  check("same verdict in another mode -> no second request",
        r3 == false and st._force_replan_reason == nil, st._force_replan_reason)
  -- A new probe (the old verdict went stale) that again says LETHAL.
  st.capture_route[PILL.my * 256 + PILL.mx] =
    { id = PID, tick = NOW + 40, tmx = TMX, tmy = TMY, direct_cost = 2,
      killed = true, damage = 10, npath = 2, mode = "pending_danger" }
  local r4 = replan(st, NOW + 41)
  check("new LETHAL verdict -> one more request",
        r4 == true and st._force_replan_reason == "KILL PICKUP LETHAL",
        st._force_replan_reason)
end
do
  local st = claim_state()
  st._force_replan_reason = "kill_handoff"
  local r = replan(st)
  check("an earlier force reason is kept (the replan runs either way)",
        r == true and st._force_replan_reason == "kill_handoff", st._force_replan_reason)
end
do
  local st = claim_state({ id = PID + 1 })
  check("verdict for another pill -> none",
        replan(st) == false and st._force_replan_reason == nil, st._force_replan_reason)
end
do
  local st = claim_state()
  st.kill_pickup = { id = PID + 1, kill_tick = 6700, created_tick = 6650 }
  st.goal.target_id = PID + 1
  check("claim on another pill -> none",
        replan(st) == false and st._force_replan_reason == nil, st._force_replan_reason)
end
do
  local st = claim_state()
  st.kill_pickup = nil
  check("no claim -> none",
        replan(st) == false and st._force_replan_reason == nil, st._force_replan_reason)
end
do
  local st = claim_state()
  st.goal.kill_grab = nil
  check("goal is capture_pill but not the kill grab -> none",
        replan(st) == false and st._force_replan_reason == nil, st._force_replan_reason)
end
do
  local st = claim_state()
  st.goal = { kind = "take_cover", mx = 120, my = 140 }
  check("goal is not capture_pill -> none",
        replan(st) == false and st._force_replan_reason == nil, st._force_replan_reason)
end
do
  local st = claim_state({ killed = false, mode = "direct", cost = 2 })
  check("verdict says we live -> none",
        replan(st) == false and st._force_replan_reason == nil, st._force_replan_reason)
end
do
  local st = claim_state()
  check("stale verdict (older than CAPTURE_ROUTE_TTL) -> none",
        replan(st, 6725 + C.CAPTURE_ROUTE_TTL + 1) == false and st._force_replan_reason == nil,
        st._force_replan_reason)
end
do
  -- Same tick as the probe: what Override 3b reads on the forced replan.
  local st = claim_state()
  local r = replan(st, 6725)
  check("forced replan on the probe tick reads the verdict as current (exit runs)",
        r == true and ask(st, 6725) ~= nil)
end
do
  C.KILL_PICKUP_LETHAL_EXIT = false
  local st = claim_state()
  check("exit knob off, replan knob on -> request still raised (harmless re-pick)",
        replan(st) == true and st._force_replan_reason == "KILL PICKUP LETHAL",
        st._force_replan_reason)
  C.KILL_PICKUP_LETHAL_EXIT = true
end
do
  C.KILL_PICKUP_LETHAL_REPLAN = false
  local st = claim_state()
  check("knob off -> none (old behaviour)",
        replan(st) == false and st._force_replan_reason == nil
        and st._kp_lethal_replan_tick == nil, st._force_replan_reason)
end
C.KILL_PICKUP_LETHAL_EXIT = DEFAULT_KNOB
C.KILL_PICKUP_LETHAL_REPLAN = DEFAULT_REPLAN
check("default replan knob is true", C.KILL_PICKUP_LETHAL_REPLAN == true, C.KILL_PICKUP_LETHAL_REPLAN)
check("PRESETS.keel pins the replan knob to false",
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.KILL_PICKUP_LETHAL_REPLAN == false,
      C.PRESETS and C.PRESETS.keel and C.PRESETS.keel.KILL_PICKUP_LETHAL_REPLAN)

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
