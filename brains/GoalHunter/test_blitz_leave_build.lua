-- =========================================================================
-- test_blitz_leave_build.lua — standalone unit tests for the blitz fixes of
-- 2026-09-25 (evening), recorded case 20260925_134920 (C3 on pill #2 at
-- (123,137), blitz-only):
--   a. C.BLITZ_SOLDIER_WAIT_FOLLOW_CMDR — attack.blitz_soldier_wait_follow:
--      p2 timed out at t=3044 (1500 backstop) while C3 still extended.
--   b. C.BLITZ_NOSPOT_RENEGOTIATE — clear_attack_goal drops the accepted
--      latch; attack._blitz_ally_spots / _blitz_spot_clash /
--      _blitz_ally_spot_filter: bot2's own pick at t=3079 sat 0.25 tile
--      from C3's bes (122.0745,144.3533).
--   d. C.BLITZ_GO_ACCEPTED_ONLY — squad.go_counts_soldier, blitz_members,
--      blitz_ready_status: GO at t=3889 counted the unaccepted p2.
--   e. C.BLITZ_LINES_AFTER_LEAVE — squad.blitz_window and
--      blitz_shot_lines_raw after bot3's STEAL_YIELD at t=4002.
--   f. C.BLITZ_NO_BUILD_ACTIVE / C.PLACE_PILL_BEHIND_ONLY /
--      C.PLACE_PILL_MAN_PATH_SAFE — builder.place_tile_check and the
--      set_mode net: bot3 sent the man to (123,141) at t=4003, dead t=4004.
--   Keel (all seven knobs false) = old behaviour.
-- No engine: terrain is all road, allies are fed straight into ally_state.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_blitz_leave_build.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C   = require("constants")
local AS  = require("ally_state")
local U   = require("util")
local PF  = require("pathfinder")
local SQ  = require("squad")
local A   = require("attack")
local B   = require("builder")

-- ── Engine stand-ins ─────────────────────────────────────────────────────
U.ttype      = function() return 4 end       -- road everywhere (placeable)
U.ttype_peek = function() return 4 end
PF.wall_hp_between = function() return 0 end
_G.cpf_lgm_travel_ticks_map = function() return 10 end
_G.cpf_estimate_tank_travel_ticks = _G.cpf_estimate_tank_travel_ticks or function() return 10 end
_G.print2 = _G.print2 or function() end

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

local KNOBS = { "BLITZ_SOLDIER_WAIT_FOLLOW_CMDR", "BLITZ_NOSPOT_RENEGOTIATE",
                "BLITZ_GO_ACCEPTED_ONLY", "BLITZ_LINES_AFTER_LEAVE",
                "BLITZ_NO_BUILD_ACTIVE", "PLACE_PILL_MAN_PATH_SAFE",
                "PLACE_PILL_BEHIND_ONLY" }
local DEFAULT = {}
for _, k in ipairs(KNOBS) do DEFAULT[k] = C[k] end
local function knobs(t)          -- set some knobs, the rest to default
  for _, k in ipairs(KNOBS) do
    if t[k] ~= nil then C[k] = t[k] else C[k] = DEFAULT[k] end
  end
end
local function keel()
  for _, k in ipairs(KNOBS) do C[k] = C.PRESETS.keel[k] end
end

-- ── 0. Knobs ─────────────────────────────────────────────────────────────
print("knobs")
do
  local kp = C.PRESETS and C.PRESETS.keel or {}
  check("BLITZ_SOLDIER_WAIT_FOLLOW_CMDR default true", C.BLITZ_SOLDIER_WAIT_FOLLOW_CMDR == true)
  check("BLITZ_NOSPOT_RENEGOTIATE default true", C.BLITZ_NOSPOT_RENEGOTIATE == true)
  check("BLITZ_GO_ACCEPTED_ONLY default true", C.BLITZ_GO_ACCEPTED_ONLY == true)
  check("BLITZ_LINES_AFTER_LEAVE default true", C.BLITZ_LINES_AFTER_LEAVE == true)
  check("BLITZ_NO_BUILD_ACTIVE default true", C.BLITZ_NO_BUILD_ACTIVE == true)
  check("PLACE_PILL_MAN_PATH_SAFE default false", C.PLACE_PILL_MAN_PATH_SAFE == false)
  check("PLACE_PILL_BEHIND_ONLY default false", C.PLACE_PILL_BEHIND_ONLY == false)
  for _, k in ipairs(KNOBS) do
    check("PRESETS.keel " .. k .. " = false", kp[k] == false, kp[k])
  end
end

-- ── a. Soldier wait follows the commander ────────────────────────────────
print("a. soldier blitz_wait follows the commander (recorded p2 t=3044)")
do
  local ME, CM, PILL = 2, 3, 2
  local function setup(bac, last)
    AS.init()
    AS.set_info(CM, last, { goal = "attack_pill", target = "2", role = "c", sqst = "blitz", sub = "blitz_wait" })
    AS.set_handshake(CM, last, "bac", bac)
    return AS.get(CM)
  end
  local function st(open)
    return { blitz_only = true, tick = 3044,
             blitz_calls = open and { [CM] = { pill = PILL, tick = 1400 } } or {} }
  end
  local function goal() return { kind = "attack_pill", target_id = PILL, _blitz_wait_since = 1543, _blitz_wait_entry = 1543 } end

  knobs({})
  local cs = setup("1,2", 3040)
  local g = goal()
  local ok = A.blitz_soldier_wait_follow(st(true), g, cs, CM, ME, 3044)
  check("recorded: C3 call open on #2, bac 1,2, fresh msg -> follow", ok == true, ok)
  if ok then g._blitz_wait_since = cs.last_tick end
  check("  backstop restarts from t=3040: no timeout at t=3044",
        (3044 - g._blitz_wait_since) <= C.SQUAD_BLITZ_WAIT_TIMEOUT, g._blitz_wait_since)
  check("  follow logged once", g._blitz_follow_logged == true)
  -- Commander goes silent: no fresh message -> no restart -> backstop runs.
  local ok2 = A.blitz_soldier_wait_follow(st(true), g, cs, CM, ME, 4600)
  check("  commander silent since t=3040 -> no follow", ok2 == false, ok2)
  check("  ... and times out at t=4600", (4600 - g._blitz_wait_since) > C.SQUAD_BLITZ_WAIT_TIMEOUT)

  check("call closed -> no follow", A.blitz_soldier_wait_follow(st(false), goal(), cs, CM, ME, 3044) == false)
  cs = setup("1", 3040)
  check("bac does not name us -> no follow", A.blitz_soldier_wait_follow(st(true), goal(), cs, CM, ME, 3044) == false)
  cs = setup("1,2", 3040)
  local nb = st(true); nb.blitz_only = nil
  local saved = C.BLITZ_ONLY_PILL_ATTACKS; C.BLITZ_ONLY_PILL_ATTACKS = false
  check("not blitz-only -> no follow", A.blitz_soldier_wait_follow(nb, goal(), cs, CM, ME, 3044) == false)
  C.BLITZ_ONLY_PILL_ATTACKS = saved
  local g3 = goal(); g3.target_id = 5
  check("call is on another pill -> no follow", A.blitz_soldier_wait_follow(st(true), g3, cs, CM, ME, 3044) == false)
  keel()
  check("keel: no follow (old 1500 backstop)", A.blitz_soldier_wait_follow(st(true), goal(), cs, CM, ME, 3044) == false)
  knobs({})
end

-- ── b. NO-SPOT re-negotiate + ally spot clash ────────────────────────────
print("b. accepted latch dropped, own spot keeps clear of squad spots")
do
  knobs({})
  local s = { tick = 3044, squad_blitz_accepted = 3, _blitz_commit_tick = 2000,
              goal = { kind = "attack_pill", target_id = 2 } }
  A.clear_attack_goal(s, "test: NO-SPOT")
  check("clear_attack_goal drops squad_blitz_accepted", s.squad_blitz_accepted == nil, s.squad_blitz_accepted)
  check("  ... and _blitz_commit_tick", s._blitz_commit_tick == nil)
  keel()
  local k = { tick = 3044, squad_blitz_accepted = 3, goal = { kind = "attack_pill", target_id = 2 } }
  A.clear_attack_goal(k, "test: keel")
  check("keel: clear_attack_goal keeps the stale latch (old)", k.squad_blitz_accepted == 3, k.squad_blitz_accepted)
  knobs({})

  -- Recorded t=3079: C3 bes (122.0745,144.3533); p1 (another soldier of C3).
  AS.init()
  AS.set_info(3, 3079, { goal = "attack_pill", target = "2", role = "c", sqst = "blitz", bes = "122.0745,144.3533" })
  AS.set_info(1, 3079, { goal = "attack_pill", target = "2", role = "s", cmdr = "3", sqst = "join", bes = "118.5000,140.5000" })
  local st = { tick = 3079, squad_role = "s", squad_cmdr = 3, squad_blitz_target = 2 }
  local g  = { kind = "attack_pill", target_id = 2 }
  local near_c3 = { has_los = true, cx = 122.3, cy = 144.4 }    -- 0.23 tile from C3
  local far     = { has_los = true, cx = 126.5, cy = 141.5 }    -- 5 tiles
  local near_p1 = { has_los = true, cx = 119.5, cy = 141.0 }    -- 1.1 tiles from p1
  local noslos  = { has_los = false, cx = 122.1, cy = 144.3 }
  local skip, nl, ns = A._blitz_ally_spot_filter(st, g, { near_c3, far, near_p1, noslos }, 2)
  check("recorded: spot 0.25 tile from C3's bes is skipped", skip and skip[near_c3] == true)
  check("  spot 1.1 tiles from p1's bes is skipped", skip and skip[near_p1] == true)
  check("  spot 5 tiles away is kept", skip and not skip[far])
  check("  counts los=3 skipped=2", nl == 3 and ns == 2, tostring(nl) .. "/" .. tostring(ns))
  local _, nl2, ns2 = A._blitz_ally_spot_filter(st, g, { near_c3 }, 2)
  check("  every LOS spot clashes -> n_skip == n_los (caller drops the goal)", nl2 == 1 and ns2 == 1)
  local pn = A._blitz_spot_clash(A._blitz_ally_spots(st, g, 2), 122.5, 144.5)
  check("  pick_standoff fallback (122,144) clashes with C3", pn == 3, pn)
  check("  distance is SQUAD_BLITZ_CLASH_TILES (no new number)",
        A._blitz_spot_clash(A._blitz_ally_spots(st, g, 2), 122.0745 + C.SQUAD_BLITZ_CLASH_TILES + 0.01, 144.3533) == nil)
  local gc = { kind = "attack_pill", target_id = 2, _blitz_cmdr = true }
  check("commander's own pick is not filtered", A._blitz_ally_spots(st, gc, 2) == nil)
  local go = { kind = "attack_pill", target_id = 7 }
  check("goal on another pill is not filtered", A._blitz_ally_spots(st, go, 2) == nil)
  check("no commander -> not filtered", A._blitz_ally_spots({ tick = 3079, squad_role = "s" }, g, 2) == nil)
  keel()
  check("keel: not filtered", A._blitz_ally_spot_filter(st, g, { near_c3 }, 2) == nil)
  knobs({})
end

-- ── d. GO counts only accepted soldiers ──────────────────────────────────
print("d. GO counts only accepted soldiers (recorded t=3889)")
do
  local ME, NOW = 3, 3889
  AS.init()
  AS.set_info(1, NOW, { goal = "attack_pill", target = "2", role = "s", cmdr = "3", sqst = "join", sub = "blitz_wait", rdy = "1" })
  AS.set_info(2, NOW, { goal = "attack_pill", target = "2", role = "s", cmdr = "3", sqst = "join", sub = "blitz_wait", rdy = "1" })
  local st = { tick = NOW, squad_role = "c", squad_blitz_accept = "1",
               goal = { kind = "attack_pill", target_id = 2 } }
  knobs({})
  check("go_counts_soldier p1 (accepted) true", SQ.go_counts_soldier(st, 1) == true)
  check("go_counts_soldier p2 (not in bac) false", SQ.go_counts_soldier(st, 2) == false)
  local total, ready, _, _, inwait = SQ.blitz_ready_status(st, NOW, ME)
  check("ready_status total stays 2", total == 2, total)
  check("ready 1 / inwait 1 (p2 ignored)", ready == 1 and inwait == 1, tostring(ready) .. "/" .. tostring(inwait))
  local m = SQ.blitz_members(st, NOW, ME, 2)
  check("blitz_members (suicider pick) only p1", #m == 1 and m[1].pn == 1, #m)
  local st0 = { tick = NOW, squad_role = "c", goal = { kind = "attack_pill", target_id = 2 } }
  check("no accept list -> nobody counts", SQ.go_counts_soldier(st0, 1) == false)
  check("soldier role -> always counts", SQ.go_counts_soldier({ squad_role = "s" }, 2) == true)
  keel()
  local t2, r2, _, _, i2 = SQ.blitz_ready_status(st, NOW, ME)
  check("keel: ready 2 / inwait 2 (old)", t2 == 2 and r2 == 2 and i2 == 2, tostring(r2) .. "/" .. tostring(i2))
  check("keel: blitz_members p1+p2", #SQ.blitz_members(st, NOW, ME, 2) == 2)
  knobs({})
end

-- ── e. Blitz window + shot lines after leaving ──────────────────────────
print("e. blitz window / shot lines after bot3's STEAL_YIELD (t=4002)")
local function recorded_world(pill_health)
  return { pills = { [2] = { mx = 123, my = 137, health = pill_health or 15, owner = "hostile" } } }
end
local function yield_state()
  -- bot3 led the take on #2 until t=4001 ...
  local st = { tick = 4001, squad_role = "c", squad_cmdr = nil,
               blitz_calls = { [3] = { pill = 2, tick = 1400 } },
               goal = { kind = "attack_pill", target_id = 2, _blitz = true, _blitz_cmdr = true,
                        standoff_fx = 122.0745, standoff_fy = 144.3533 } }
  return st
end
local function soldiers(now)
  AS.init()
  AS.set_info(1, now, { goal = "attack_pill", target = "2", role = "s", cmdr = "3", sqst = "join", bes = "126.5000,140.5000" })
  AS.set_info(2, now, { goal = "attack_pill", target = "2", role = "s", cmdr = "3", sqst = "join", bes = "119.5000,141.5000" })
end
do
  knobs({})
  soldiers(4001)
  local w = recorded_world()
  local st = yield_state()
  local win = SQ.blitz_window(st, w, 4001, 3)
  check("in the take: window live", win and win.live == true and win.pill == 2)
  -- ... STEAL_YIELD at t=4002: goal is now a pill placement.
  st.goal = { kind = "pill_place", place_mx = 123, place_my = 141, substate = "dispatch" }
  st.squad_role = nil
  st.tick = 4003
  win = SQ.blitz_window(st, w, 4003, 3)
  check("after yield: window still open, not live", win and win.open and not win.live)
  local lines = SQ.blitz_shot_lines_raw(st, w, 4003, 3)
  check("after yield: p1 and p2 lines kept, no self line", lines and #lines == 2, lines and #lines)
  if lines then
    local who = {}
    for _, l in ipairs(lines) do who[l.who] = true end
    check("  lines are p1 and p2", who.p1 and who.p2)
  end
  check("blitz_shot_lines (PILL_PLACE_AVOID_BLITZ_LINE on) keeps them too",
        SQ.blitz_shot_lines(st, w, 4003, 3) ~= nil)
  -- Ends: pill dead.
  local stD = yield_state(); SQ.blitz_window(stD, w, 4001, 3)
  stD.goal = { kind = "none" }; stD.squad_role = nil
  check("pill dead -> window closes", SQ.blitz_window(stD, recorded_world(0), 4004, 3) == nil)
  -- Ends: timeout.
  local stT = yield_state(); SQ.blitz_window(stT, w, 4001, 3)
  stT.goal = { kind = "none" }; stT.squad_role = nil
  check("SQUAD_BLITZ_WAIT_TIMEOUT after leaving -> closes",
        SQ.blitz_window(stT, w, 4001 + C.SQUAD_BLITZ_WAIT_TIMEOUT + 1, 3) == nil)
  -- Ends: nobody on it (no call, allies off the pill).
  local stN = yield_state(); SQ.blitz_window(stN, w, 4001, 3)
  stN.goal = { kind = "none" }; stN.squad_role = nil; stN.blitz_calls = {}
  AS.set_info(1, 4005, { goal = "refuel_at_base", role = "s" })
  AS.set_info(2, 4005, { goal = "none", role = "s" })
  check("call closed + allies off the pill -> closes", SQ.blitz_window(stN, w, 4006, 3) == nil)
  -- Ends: our own death (2026-09-26). reset_blitz_state drops the window,
  -- so the respawned bot has no lines and no no-build rule from the last life.
  soldiers(4001)
  local stR = yield_state(); SQ.blitz_window(stR, w, 4001, 3)
  check("window latched before death", stR._blitz_win ~= nil and stR._blitz_win.open)
  SQ.reset_blitz_state(stR)
  check("death: reset_blitz_state clears _blitz_win", stR._blitz_win == nil)
  stR.goal = { kind = "none" }
  check("  respawned bot: no window", SQ.blitz_window(stR, w, 4010, 3) == nil)
  check("  respawned bot: no shot lines", SQ.blitz_shot_lines_raw(stR, w, 4010, 3) == nil)
  -- Knob off: no lines after leaving.
  knobs({ BLITZ_LINES_AFTER_LEAVE = false })
  soldiers(4001)
  local stK = yield_state(); SQ.blitz_window(stK, w, 4001, 3)
  stK.goal = { kind = "none" }; stK.squad_role = nil
  check("BLITZ_LINES_AFTER_LEAVE off: no lines after leaving", SQ.blitz_shot_lines_raw(stK, w, 4003, 3) == nil)
  keel()
  local stP = yield_state()
  check("keel: no window at all", SQ.blitz_window(stP, w, 4001, 3) == nil)
  stP.goal = { kind = "none" }
  check("keel: no lines after leaving (old)", SQ.blitz_shot_lines_raw(stP, w, 4003, 3) == nil)
  knobs({})
end

-- ── f. New-pill tile check ───────────────────────────────────────────────
print("f. place_tile_check (recorded bot3 tank (121,143) -> tile (123,141))")
local INFO = { player_number = 3, tankx = 121 * 256 + 128, tanky = 143 * 256 + 128,
               carried_pills = 2, man_status = C.LGM_INTANK, inboat = false }
local function left_state(w)
  soldiers(4001)
  local st = yield_state()
  SQ.blitz_window(st, w, 4001, 3)
  st.goal = { kind = "pill_place", place_mx = 123, place_my = 141, substate = "dispatch" }
  st.squad_role = nil
  st.tick = 4003
  return st
end
do
  local w = recorded_world()
  knobs({})
  local why = B.place_tile_check(left_state(w), w, INFO, 123, 141, 4003)
  check("recorded: bot3 after STEAL_YIELD blocked from (123,141): blitz_active", why == "blitz_active", why)
  -- Repairs are not new pills: the check is only called for placements, and
  -- a window-closed state places again.
  local wd = recorded_world(0)
  check("pill dead -> blitz over -> (123,141) allowed",
        B.place_tile_check(left_state(wd), wd, INFO, 123, 141, 4004) == nil)
  -- No blitz at all, default knobs: nothing changes.
  AS.init()
  check("no blitz -> allowed", B.place_tile_check({ tick = 10, goal = { kind = "pill_place" } }, w, INFO, 123, 141, 10) == nil)

  -- Behind-only (window open, no-build off).
  knobs({ BLITZ_NO_BUILD_ACTIVE = false, PLACE_PILL_BEHIND_ONLY = true })
  why = B.place_tile_check(left_state(w), w, INFO, 123, 141, 4003)
  check("behind-only: (123,141) toward the pill rejected", why == "not_behind", why)
  why = B.place_tile_check(left_state(w), w, INFO, 119, 145, 4003)
  check("behind-only: (119,145) behind the tank allowed", why == nil, why)
  AS.init()
  check("behind-only: no blitz -> (123,141) allowed",
        B.place_tile_check({ tick = 10, goal = { kind = "pill_place" } }, w, INFO, 123, 141, 10) == nil)

  -- Man-path safe.
  knobs({ BLITZ_NO_BUILD_ACTIVE = false, PLACE_PILL_MAN_PATH_SAFE = true })
  AS.init()
  local plain = { tick = 10, goal = { kind = "pill_place" } }
  why = B.place_tile_check(plain, w, INFO, 123, 141, 10)
  check("path-safe: (123,141) inside pill #2's range rejected", why == "man_path", why)
  local wfar = { pills = { [2] = { mx = 123, my = 120, health = 15, owner = "hostile" } } }
  plain = { tick = 11, goal = { kind = "pill_place" } }
  check("path-safe: pill 23 tiles away -> (119,145) allowed",
        B.place_tile_check(plain, wfar, INFO, 119, 145, 11) == nil)
  local wfr = { pills = { [2] = { mx = 123, my = 137, health = 15, owner = "friendly" } } }
  plain = { tick = 12, goal = { kind = "pill_place" } }
  check("path-safe: friendly pill in range does not block",
        B.place_tile_check(plain, wfr, INFO, 123, 141, 12) == nil)
  local wdead = { pills = { [2] = { mx = 123, my = 137, health = 0, owner = "hostile" } } }
  plain = { tick = 13, goal = { kind = "pill_place" } }
  check("path-safe: dead pill does not block",
        B.place_tile_check(plain, wdead, INFO, 123, 141, 13) == nil)
  -- A live ally shot line across the walk (pill far, window open after leaving).
  soldiers(4001)
  AS.set_info(1, 4001, { goal = "attack_pill", target = "2", role = "s", cmdr = "3", sqst = "join", bes = "122.0745,144.3533" })
  local stL = yield_state(); SQ.blitz_window(stL, wfar, 4001, 3)
  stL.goal = { kind = "pill_place" }; stL.squad_role = nil
  why = B.place_tile_check(stL, wfar, INFO, 122, 140, 4003)
  check("path-safe: walk crosses p1's shot line -> rejected", why == "man_path", why)
  stL.tick = 4003
  check("path-safe: walk clear of the lines -> allowed",
        B.place_tile_check(stL, wfar, INFO, 119, 145, 4003) == nil)

  -- Keel: every tile allowed.
  keel()
  local wk = recorded_world()
  check("keel: (123,141) after yield allowed (old)", B.place_tile_check(left_state(wk), wk, INFO, 123, 141, 4003) == nil)
  knobs({})
end

-- ── f2. set_mode net (bot3's pill_place dispatch after the yield) ───────
print("f2. builder.set_mode net")
do
  local function run()
    local w = recorded_world()
    local st = left_state(w)
    st.builder = {}
    local g = st.goal
    local ok, err = pcall(B.set_mode, st, w, INFO, g)
    return ok, err, st.builder
  end
  knobs({})
  local ok, err, b = run()
  check("set_mode runs", ok, err)
  check("recorded: pill_place dispatch to (123,141) blocked (mode not place_pill)",
        ok and b.mode ~= "place_pill", b and b.mode)
  check("  logged once (key set)", ok and b._place_block_key ~= nil, b and b._place_block_key)
  keel()
  ok, err, b = run()
  check("keel: set_mode runs", ok, err)
  check("keel: dispatch goes out (old)", ok and b.mode == "place_pill", b and b.mode)
  knobs({})
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
