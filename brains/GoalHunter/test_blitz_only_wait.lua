-- =========================================================================
-- test_blitz_only_wait.lua — standalone unit tests for three blitz fixes
-- (2026-09-25):
--   1. blitz-only GO rule (knob C.BLITZ_ONLY_EXTEND_WAIT):
--      attack.blitz_cmdr_go_verdict — under blitz-only the commander goes GO
--      only when the PARKED set reaches blitz_min; at READY_TIMEOUT short of
--      it the wait is extended. Without blitz-only: the old rules.
--   2. brj for a COMMITTED soldier: squad._read_cmdr_brj +
--      attack.blitz_brj_replan + attack._blitz_sync_engage — the soldier
--      drops the rejected spot, replans and re-broadcasts the new spot.
--   3. pending pills (knob C.BLITZ_SPOT_PENDING_PILLS):
--      squad.update_pending_pills, spot_margin (aim_line_trees /
--      clear_aim_margin), the arbiter's shot test, attack's spot pick.
-- No engine: terrain is all grass and the shell simulator walks a straight
-- line through the tiles (both patched onto the real modules).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_blitz_only_wait.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C   = require("constants")
local AS  = require("ally_state")
local U   = require("util")
local cpf = require("cpathfinder")
local SM  = require("spot_margin")
local SQ  = require("squad")
local A   = require("attack")

-- ── Engine stand-ins ─────────────────────────────────────────────────────
U.ttype      = function() return 4 end       -- grass everywhere
U.ttype_peek = function() return 4 end
-- Straight-line shell: every tile the segment O->A passes through, in order,
-- ending on the aim tile (the pill).
cpf.SHOT_TANK = cpf.SHOT_TANK or 0
cpf.simulate_shot = function(ox, oy, awx, awy)
  local x0, y0, x1, y1 = ox / 256, oy / 256, awx / 256, awy / 256
  local n = math.ceil(math.max(math.abs(x1 - x0), math.abs(y1 - y0)) * 16) + 1
  local out, last = {}, nil
  for i = 0, n do
    local t = i / n
    local mx, my = math.floor(x0 + (x1 - x0) * t), math.floor(y0 + (y1 - y0) * t)
    local k = my * 256 + mx
    if k ~= last then out[#out + 1] = { mx = mx, my = my }; last = k end
  end
  return out
end

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

-- ── 0. Knobs ─────────────────────────────────────────────────────────────
print("knobs")
check("BLITZ_ONLY_EXTEND_WAIT default true", C.BLITZ_ONLY_EXTEND_WAIT == true)
check("BLITZ_SPOT_PENDING_PILLS default true", C.BLITZ_SPOT_PENDING_PILLS == true)
local keel = C.PRESETS and C.PRESETS.keel or {}
check("PRESETS.keel BLITZ_ONLY_EXTEND_WAIT = false", keel.BLITZ_ONLY_EXTEND_WAIT == false)
check("PRESETS.keel BLITZ_SPOT_PENDING_PILLS = false", keel.BLITZ_SPOT_PENDING_PILLS == false)
check("BLITZ_ONLY_EXTEND_MAX default 3", C.BLITZ_ONLY_EXTEND_MAX == 3, C.BLITZ_ONLY_EXTEND_MAX)
check("PRESETS.keel BLITZ_ONLY_EXTEND_MAX = 3 (unused with EXTEND_WAIT off)", keel.BLITZ_ONLY_EXTEND_MAX == 3)

-- ── 1. GO verdict ────────────────────────────────────────────────────────
print("attack.blitz_cmdr_go_verdict")
local V = A.blitz_cmdr_go_verdict
-- (bo_hold, timed_out, ready, total, party, set_inwait, bmin)
-- Recorded case 20260925_105315 bot0 t=3035: min 3, 2 soldiers committed, 0
-- ready, only the commander parked -> KEEL went GO on the timeout.
check("blitzonly + timeout, parked 1 < min 3 -> extend",
      V(true, true, 0, 2, 3, 1, 3) == "extend", V(true, true, 0, 2, 3, 1, 3))
check("blitzonly + timeout, parked 2 < min 3, party short -> extend (no abandon)",
      V(true, true, 0, 1, 2, 2, 3) == "extend", V(true, true, 0, 1, 2, 2, 3))
check("blitzonly + timeout, parked 3 >= min 3 -> go",
      V(true, true, 2, 2, 3, 3, 3) == "go", V(true, true, 2, 2, 3, 3, 3))
check("blitzonly, no timeout, parked 3 >= min 3 -> go",
      V(true, false, 2, 2, 3, 3, 3) == "go", V(true, false, 2, 2, 3, 3, 3))
check("blitzonly, no timeout, all ready but parked 2 < min 3 -> wait",
      V(true, false, 2, 2, 3, 2, 3) == "wait", V(true, false, 2, 2, 3, 2, 3))
check("blitzonly, no timeout, parked 1 -> wait",
      V(true, false, 0, 2, 3, 1, 3) == "wait", V(true, false, 0, 2, 3, 1, 3))
check("no blitzonly + timeout, party 3 >= min, parked 1 -> go (old rule)",
      V(false, true, 0, 2, 3, 1, 3) == "go", V(false, true, 0, 2, 3, 1, 3))
check("no blitzonly + timeout, party 2 < min 3 -> abandon (old rule)",
      V(false, true, 0, 1, 2, 1, 3) == "abandon", V(false, true, 0, 1, 2, 1, 3))
check("no blitzonly, all ready, party >= min -> go (old rule)",
      V(false, false, 2, 2, 3, 1, 3) == "go", V(false, false, 2, 2, 3, 1, 3))
-- Exhaustive: bo_hold=false must equal the pre-change formula everywhere.
do
  local bad = 0
  for _, to in ipairs({ false, true }) do
    for total = 0, 4 do for ready = 0, total do for inw = 0, total do for bmin = 1, 5 do
      local party, si = 1 + total, 1 + inw
      local early = si >= bmin
      local old
      if to and party < bmin then old = "abandon"
      elseif (ready >= total and party >= bmin) or to or early then old = "go"
      else old = "wait" end
      if V(false, to, ready, total, party, si, bmin) ~= old then bad = bad + 1 end
    end end end end
  end
  check("no blitzonly: verdict == old formula over the whole grid", bad == 0, bad)
end
-- Under blitz-only nothing but parked >= min ever says go.
do
  local bad = 0
  for _, to in ipairs({ false, true }) do
    for total = 0, 4 do for ready = 0, total do for inw = 0, total do for bmin = 1, 5 do
      local v = V(true, to, ready, total, 1 + total, 1 + inw, bmin)
      if (v == "go") ~= (1 + inw >= bmin) then bad = bad + 1 end
      if v == "abandon" then bad = bad + 1 end
    end end end end
  end
  check("blitzonly: go iff parked >= min, never abandon", bad == 0, bad)
end
-- 2026-09-26: the extensions are capped (BLITZ_ONLY_EXTEND_MAX). A commander
-- with nobody joined (total 0, party 1) extends N times, then abandons.
do
  local N = C.BLITZ_ONLY_EXTEND_MAX
  local n, v = 0, nil
  for _ = 1, N + 5 do
    v = V(true, true, 0, 0, 1, 1, 2, n, N)
    if v ~= "extend" then break end
    n = n + 1
  end
  check(string.format("blitzonly, nobody joined: extend %d times", N), n == N, n)
  check("  then abandon", v == "abandon", v)
  -- Party big enough but parked short: also abandon at the cap, never go.
  check("blitzonly, party 3 >= min 3, parked 1, ext at cap -> abandon",
        V(true, true, 0, 2, 3, 1, 3, N, N) == "abandon", V(true, true, 0, 2, 3, 1, 3, N, N))
  check("blitzonly, parked 3 >= min 3, ext at cap -> still go",
        V(true, true, 2, 2, 3, 3, 3, N, N) == "go", V(true, true, 2, 2, 3, 3, 3, N, N))
  check("blitzonly, no timeout, ext at cap -> wait (cap only acts at a timeout)",
        V(true, false, 0, 2, 3, 1, 3, N, N) == "wait", V(true, false, 0, 2, 3, 1, 3, N, N))
  check("cap 0 -> abandon at the first timeout",
        V(true, true, 0, 0, 1, 1, 2, 0, 0) == "abandon", V(true, true, 0, 0, 1, 1, 2, 0, 0))
  -- No blitz-only: ext_n/ext_max change nothing.
  check("no blitzonly, ext at cap -> old rule (go)",
        V(false, true, 0, 2, 3, 1, 3, N, N) == "go", V(false, true, 0, 2, 3, 1, 3, N, N))
end

-- ── 2. brj for a committed soldier ───────────────────────────────────────
print("committed soldier brj")
local CMDR, ME, PMX, PMY = 0, 1, 138, 128
local function soldier_state()
  local st = {
    tick = 3032,
    squad_cmdr = CMDR, squad_blitz_accepted = CMDR,
    squad_blitz_engage_mx = 143, squad_blitz_engage_my = 123,
    squad_blitz_engage_fx = 143.7326, squad_blitz_engage_fy = 123.2674,
    pf = { status = "done" },
  }
  local goal = { kind = "attack_pill", target_id = 0, mx = PMX, my = PMY,
                 substate = "approach", _blitz = true, _blitz_started = true,
                 standoff_mx = 143, standoff_my = 123,
                 standoff_fx = 143.7326, standoff_fy = 123.2674, _chosen_deg = 45 }
  st.goal = goal
  return st, goal
end
AS.init()
AS.set_info(CMDR, 3030, { brj = "1:[143.7326,123.2674]" })
-- End of a tick in squad.update: the per-tick reset of the reject flag, then
-- the committed soldier's brj read. attack runs earlier in the next tick.
local function squad_tick_end(st)
  st._blitz_call_rejected = nil
  SQ._read_cmdr_brj(st, ME)
end
do
  local st, goal = soldier_state()
  squad_tick_end(st)                                   -- tick 3032
  check("read_cmdr_brj flags a reject of our committed spot", st._blitz_call_rejected == true)
  -- Tick 3033.
  local r = A.blitz_brj_replan(st, goal, PMX, PMY, 3033)
  check("committed soldier drops the spot -> replan", r == "replan", r)
  check("  substate back to plan_position", goal.substate == "plan_position", goal.substate)
  check("  angle banned", st.banned_pill_angles and st.banned_pill_angles[PMY * 256 + PMX] ~= nil)
  check("  resync flag set", goal._blitz_brj_resync == true)
  squad_tick_end(st)
  -- Still picking: the engage fields hold the old spot, so the read matches
  -- the commander's brj (still naming it) again.
  check("  end of 3033: flag set again against the old spot", st._blitz_call_rejected == true)
  -- Tick 3034: still in plan_position -> sync waits, no replan.
  A._blitz_sync_engage(st, goal, 3034)
  check("sync waits while plan_position", st.squad_blitz_engage_mx == 143)
  check("plan_position -> none", A.blitz_brj_replan(st, goal, PMX, PMY, 3034) == "none")
  -- plan_position picks a new standoff later in tick 3034.
  local exact = C.BLITZ_SPOT_EXACT_ORIGIN
  C.BLITZ_SPOT_EXACT_ORIGIN = false
  goal.standoff_mx, goal.standoff_my = 145, 128
  goal.standoff_fx, goal.standoff_fy = nil, nil
  goal.substate = "approach"
  squad_tick_end(st)
  check("  end of 3034: flag still set (engage fields not synced yet)", st._blitz_call_rejected == true)
  -- Tick 3035: the sync copies the new spot (even with EXACT off); the
  -- replan that follows it must not fire again for the one reject.
  A._blitz_sync_engage(st, goal, 3035)
  C.BLITZ_SPOT_EXACT_ORIGIN = exact
  check("sync copies the new spot (EXACT off, tile centre)",
        st.squad_blitz_engage_mx == 145 and st.squad_blitz_engage_fx == 145.5
        and st.squad_blitz_engage_fy == 128.5,
        string.format("(%s,%s)", tostring(st.squad_blitz_engage_fx), tostring(st.squad_blitz_engage_fy)))
  check("  resync flag cleared", goal._blitz_brj_resync == nil)
  check("  sync clears the reject flag read against the old spot", st._blitz_call_rejected == nil)
  local r3 = A.blitz_brj_replan(st, goal, PMX, PMY, 3035)
  check("one reject -> no second replan after the sync", r3 == "none", r3)
  check("  replan count stays 1", goal._sanity_pill_replans == 1, goal._sanity_pill_replans)
  -- The old brj is now stale: it names a spot we no longer offer.
  squad_tick_end(st)
  check("stale brj for the old spot is ignored", st._blitz_call_rejected == nil)
  -- The commander rejects the NEW spot too: that one does replan.
  AS.set_info(CMDR, 3036, { brj = "1:[145.5000,128.5000]" })
  squad_tick_end(st)
  local r4 = A.blitz_brj_replan(st, goal, PMX, PMY, 3037)
  check("a reject of the new spot -> replan", r4 == "replan", r4)
  check("  replan count 2", goal._sanity_pill_replans == 2, goal._sanity_pill_replans)
  AS.set_info(CMDR, 3030, { brj = "1:[143.7326,123.2674]" })
end
do
  local st, goal = soldier_state()
  st._blitz_call_rejected = true
  st.squad_blitz_accepted = nil
  check("not committed (negotiating) -> none (negotiate handles it)",
        A.blitz_brj_replan(st, goal, PMX, PMY, 3033) == "none")
  st.squad_blitz_accepted = CMDR
  goal._blitz_committed = true
  check("after GO (_blitz_committed) -> none",
        A.blitz_brj_replan(st, goal, PMX, PMY, 3033) == "none")
  goal._blitz_committed = nil
  goal.substate = "charge"
  check("substate charge -> none", A.blitz_brj_replan(st, goal, PMX, PMY, 3033) == "none")
  goal.substate = "blitz_wait"
  check("parked in blitz_wait -> replan", A.blitz_brj_replan(st, goal, PMX, PMY, 3033) == "replan")
end
do
  -- Three different rejected spots: the third hits SANITY_PILL_REPLANS_MAX.
  local st, goal = soldier_state()
  local r
  for i = 1, (C.SANITY_PILL_REPLANS_MAX or 3) do
    st._blitz_call_rejected = true
    st.squad_blitz_engage_fx = 143.7326 + i
    goal.substate = "approach"
    r = A.blitz_brj_replan(st, goal, PMX, PMY, 3033 + i)
  end
  check("third rejected spot -> abandon (goal cleared)", r == "abandon" and st.goal.kind == "none", r)
end

-- ── 2b. Malformed / far-away bes from an ally (2026-09-26) ─────────────
print("bes from the wire")
do
  local P = SQ.parse_bes
  check("parse_bes plain", select(1, P("143.7326,123.2674")) == 143.7326)
  check("parse_bes '1.2.3,4' -> nil", P("1.2.3,4") == nil)
  check("parse_bes '4,..' -> nil", P("4,..") == nil)
  check("parse_bes off the map -> nil", P("99999999,5") == nil and P("-1,5") == nil and P("5,256") == nil)
  check("parse_bes near the pill -> kept", P("143.7,123.3", PMX, PMY) ~= nil)
  check("parse_bes over BES_MAX_PILL_DIST from the pill -> nil",
        P(string.format("%.1f,128.5", PMX + 0.5 + SQ.BES_MAX_PILL_DIST + 0.5), PMX, PMY) == nil)
  check("BES_MAX_PILL_DIST covers the standoffs",
        SQ.BES_MAX_PILL_DIST >= (C.ATTACK_PILL_RANGE or 9.5) + 2)
end
do
  -- Commander arbiter: a soldier broadcasting a broken bes is not answered
  -- (not accepted, not rejected) and the arbiter returns at once.
  AS.init()
  local S1, S2, S3 = 1, 2, 3
  AS.set_info(S1, 3000, { role = "s", cmdr = tostring(CMDR), bes = "99999999,5", bd = "5" })
  AS.set_info(S2, 3000, { role = "s", cmdr = tostring(CMDR), bes = "1.2.3,4", bd = "5" })
  AS.set_info(S3, 3000, { role = "s", cmdr = tostring(CMDR), bes = "143.7326,123.2674", bd = "5" })
  local margin = C.BLITZ_SPOT_LOS_MARGIN
  C.BLITZ_SPOT_LOS_MARGIN = 0.5
  local st = { tick = 3001, world = { pill_at = {}, base_at = {}, pills = {} },
               goal = { kind = "attack_pill", target_id = 0, mx = PMX, my = PMY,
                        standoff_mx = 131, standoff_my = 128, standoff_fx = 131.5, standoff_fy = 128.5 } }
  local t0 = os.clock()
  local ok, err = pcall(SQ.blitz_arbitrate, st, { player_number = CMDR }, 3001, CMDR)
  local dt = os.clock() - t0
  C.BLITZ_SPOT_LOS_MARGIN = margin
  check("arbiter with broken bes runs without error", ok, err)
  check("  and returns promptly (< 1 s)", dt < 1.0, string.format("%.3fs", dt))
  local by = {}
  for _, e in ipairs(st.squad_blitz_roster or {}) do by[e.pn] = e end
  check("  huge bes -> not answered", by[S1] and by[S1].answered == false)
  check("  '1.2.3,4' -> not answered", by[S2] and by[S2].answered == false)
  check("  good bes -> accepted", st.squad_blitz_accept == tostring(S3), st.squad_blitz_accept)
  -- Pill-placement shot lines: the broken ones are dropped, not nil.
  local lines
  ok, err = pcall(function()
    st.world.pills[0] = { mx = PMX, my = PMY, health = 8 }
    st.goal._blitz = true
    lines = SQ.blitz_shot_lines_raw(st, st.world, 3001, CMDR)
  end)
  check("blitz_shot_lines_raw with broken bes runs", ok, err)
  local bad = 0
  for _, ln in ipairs(lines or {}) do
    if type(ln.fx) ~= "number" or type(ln.fy) ~= "number" then bad = bad + 1 end
  end
  check("  every line has numeric fx/fy", lines and bad == 0, bad)
  check("  only self + the good ally", lines and #lines == 2, lines and #lines)
  ok, err = pcall(SQ.tile_on_blitz_line, st.world, lines, 140, 126, 3001)
  check("tile_on_blitz_line over those lines runs", ok, err)
end

-- ── 3. Pending pills ─────────────────────────────────────────────────────
print("pending pills")
-- Recorded geometry (20260925_105315): target pill 0 at (138,128); p1's spot
-- (143.7326,123.2674); p9's man walking to build a pill on (142,123).
local function world_with(pending)
  return { pill_at = {}, base_at = {}, pills = {}, pending_pill_at = pending }
end
local PEND = { [123 * 256 + 142] = "ally_p9" }
-- The recorded tile only touches the lines at a corner (the recording's margin
-- reject said off=0.000), so the shell-only paths (margin 0) are tested with a
-- pending tile the lines really cross: (141,125).
local PEND_ON = { [125 * 256 + 141] = "ally_p9" }
local SPX, SPY = 143.7326, 123.2674
do
  local info = { player_number = 0, man_status = C.LGM_INTANK, carried_pills = 0 }
  local w = {}
  AS.init()
  AS.set_info(9, 2960, { lgmd = "8E7B0038P" })
  AS.set_info(5, 2960, { lgmd = "90800010" })          -- no "P": not a pill job
  SQ.update_pending_pills({}, info, 2961, w)
  check("ally lgmd ...P -> (142,123) pending", w.pending_pill_at and w.pending_pill_at[123 * 256 + 142] ~= nil)
  check("ally lgmd without P -> not pending", w.pending_pill_at and w.pending_pill_at[128 * 256 + 144] == nil)
  -- Own man out on a pill trip.
  AS.init()
  local st = { _lgm_dispatch = { x = 150, y = 124, pbox = true, tick = 10 } }
  info.man_status = 1
  SQ.update_pending_pills(st, info, 20, w)
  check("own man out on a pill trip -> pending", w.pending_pill_at and w.pending_pill_at[124 * 256 + 150] == "own_lgm")
  info.man_status = C.LGM_INTANK
  SQ.update_pending_pills(st, info, 21, w)
  check("own man back in the tank -> none", w.pending_pill_at == nil)
  st._lgm_dispatch.pbox = nil; info.man_status = 1
  SQ.update_pending_pills(st, info, 22, w)
  check("own man out on a non-pill trip -> none", w.pending_pill_at == nil)
  -- Own placement target while carrying.
  info.man_status = C.LGM_INTANK; info.carried_pills = 1
  st.goal = { kind = "place_pill_strategic", mx = 160, my = 100 }
  SQ.update_pending_pills(st, info, 23, w)
  check("own place_pill_strategic target while carrying -> pending",
        w.pending_pill_at and w.pending_pill_at[100 * 256 + 160] == "own_place")
  -- Knob off -> nil (KEEL).
  C.BLITZ_SPOT_PENDING_PILLS = false
  SQ.update_pending_pills(st, info, 24, w)
  check("knob off -> world.pending_pill_at nil", w.pending_pill_at == nil)
  C.BLITZ_SPOT_PENDING_PILLS = true
end
do
  local ox, oy = math.floor(SPX * 256 + 0.5), math.floor(SPY * 256 + 0.5)
  local omx, omy = math.floor(SPX), math.floor(SPY)
  local t0 = SM.aim_line_trees(ox, oy, omx, omy, PMX, PMY, world_with(nil), 1)
  local t1 = SM.aim_line_trees(ox, oy, omx, omy, PMX, PMY, world_with(PEND_ON), 1)
  check("aim_line_trees: centre line clear with no pending pill", t0 == 0, t0)
  check("aim_line_trees: pending pill on the line blocks it", t1 == nil, t1)
  local margin = C.BLITZ_SPOT_LOS_MARGIN
  local i0 = SM.clear_aim_margin(ox, oy, PMX, PMY, world_with(nil), margin, { site = "test", tick = 1 })
  local i1 = SM.clear_aim_margin(ox, oy, PMX, PMY, world_with(PEND), margin, { site = "test", tick = 1 })
  check("clear_aim_margin: spot passes with no pending pill", i0 ~= nil, i0)
  check("clear_aim_margin: pending pill -> spot rejected", i1 == nil, i1)
  -- The arbiter, both code paths.
  check("arbiter (margin): no pending -> not blocked",
        SQ._blitz_spot_shot_blocked(world_with(nil), SPX, SPY, PMX, PMY, 1, ME) == false)
  check("arbiter (margin): pending -> shot_blocked",
        SQ._blitz_spot_shot_blocked(world_with(PEND), SPX, SPY, PMX, PMY, 1, ME) == true)
  C.BLITZ_SPOT_LOS_MARGIN = 0
  check("arbiter (legacy, margin 0): no pending -> not blocked",
        SQ._blitz_spot_shot_blocked(world_with(nil), SPX, SPY, PMX, PMY, 1, ME) == false)
  check("arbiter (legacy, margin 0): pending -> shot_blocked",
        SQ._blitz_spot_shot_blocked(world_with(PEND_ON), SPX, SPY, PMX, PMY, 1, ME) == true)
  C.BLITZ_SPOT_LOS_MARGIN = margin
end
do
  -- Spot pick: A = the recorded spot (best score), B = a clear spot south-east.
  local function spots()
    return {
      { mx = 143, my = 123, cx = SPX, cy = SPY, has_los = true, aim_idx = 1, aim_trees = 0,
        aim_wx = PMX * 256 + 128, aim_wy = PMY * 256 + 128, total_score = 10, deg = 45 },
      { mx = 144, my = 131, cx = 144.5, cy = 131.5, has_los = true, aim_idx = 1, aim_trees = 0,
        aim_wx = PMX * 256 + 128, aim_wy = PMY * 256 + 128, total_score = 30, deg = 120 },
    }
  end
  local pick = A._blitz_pick_from_scan
  local mx0, my0 = pick(spots(), { tick = 3022 }, 147, 123, nil, PMX, PMY, world_with(nil))
  local mx1, my1 = pick(spots(), { tick = 3022 }, 147, 123, nil, PMX, PMY, world_with(PEND))
  check("pick: no pending pill -> the same spot as before (143,123)", mx0 == 143 and my0 == 123,
        string.format("(%s,%s)", tostring(mx0), tostring(my0)))
  check("pick: pending pill on its line -> the other spot (144,131)", mx1 == 144 and my1 == 131,
        string.format("(%s,%s)", tostring(mx1), tostring(my1)))
  -- Same with the margin off (the shell test alone must still see it).
  local margin = C.BLITZ_SPOT_LOS_MARGIN
  C.BLITZ_SPOT_LOS_MARGIN = 0
  local wall = { [0] = true }   -- non-empty wallset -> blitz_clear_aim path
  local mx2, my2 = pick(spots(), { tick = 3022 }, 147, 123, wall, PMX, PMY, world_with(PEND_ON))
  local mx3, my3 = pick(spots(), { tick = 3022 }, 147, 123, wall, PMX, PMY, world_with(nil))
  C.BLITZ_SPOT_LOS_MARGIN = margin
  check("pick (blitz_clear_aim path): pending -> other spot", mx2 == 144 and my2 == 131,
        string.format("(%s,%s)", tostring(mx2), tostring(my2)))
  check("pick (blitz_clear_aim path): no pending -> same spot", mx3 == 143 and my3 == 123,
        string.format("(%s,%s)", tostring(mx3), tostring(my3)))
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
