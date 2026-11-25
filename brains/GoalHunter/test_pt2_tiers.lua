-- =========================================================================
-- test_pt2_tiers.lua -- standalone unit tests (2026-11-16) for the pill
-- take v2 rank (pilltake_v2_old.lua):
--   1. the rank order: safe, then chain length, then an allied pill in the
--      cover above walls only, then leeway (up to PT2_LEEWAY_MIN), then the
--      travel score;
--   2. evaluate() on small maps: a two-pill chain is found and ranks above
--      a single take; with chains off it is a single take; an allied pill
--      blocker pulls the standoff to its side (knob on) or not (knob off).
-- No engine: a hand-made map, the shadow.lua rays.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_tiers.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local C   = require("constants")
local U   = require("util")
local cpf = require("cpathfinder")
local PC  = require("pill_cover")
_G.print2 = _G.print2 or function() end
local V2  = require("pilltake_v2_old")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

-- ── 0. knobs ─────────────────────────────────────────────────────────────
print("knobs")
check("PILLTAKE_V2_EVALUATOR ships on (HELM, Nov 18)", C.PILLTAKE_V2_EVALUATOR == true)
check("PILLTAKE_V2_EVALUATOR off in MAST", C.PRESETS.mast.PILLTAKE_V2_EVALUATOR == false)
check("PILLTAKE_V2_EVALUATOR off in KEEL", C.PRESETS.keel.PILLTAKE_V2_EVALUATOR == false)
check("PT2_CHAIN_BLOCKER_MIN_SHOTS = 10 (Andrew's value, two walls)", C.PT2_CHAIN_BLOCKER_MIN_SHOTS == 10)
for _, kn in ipairs({ "PT2_CHAINS", "PT2_LINK_COVERS_PREV", "PT2_LEEWAY", "PT2_OWN_PILL_BONUS",
                      "PT2_AIM_NUDGE" }) do
  check(kn .. " default true", C[kn] == true)
  check(kn .. " keel false", C.PRESETS.keel[kn] == false)
  check(kn .. " mast false", C.PRESETS.mast[kn] == false)
end
-- number knob: 0 = the old rank
check("PT2_TRAVEL_BAND_TICKS default 100 (Andrew's value)", C.PT2_TRAVEL_BAND_TICKS == 100)
check("PT2_TRAVEL_BAND_TICKS keel 0", C.PRESETS.keel.PT2_TRAVEL_BAND_TICKS == 0)
check("PT2_TRAVEL_BAND_TICKS mast 0", C.PRESETS.mast.PT2_TRAVEL_BAND_TICKS == 0)
do
  local ki = V2.knob_inp(C, {})
  check("one score unit is 16 ticks (a road square)", ki.ticks_per_score == 16, ki.ticks_per_score)
  check("band 100 ticks = 6.25 score units", math.abs(ki.travel_band - 6.25) < 1e-9, ki.travel_band)
  local save = C.PT2_TRAVEL_BAND_TICKS
  C.PT2_TRAVEL_BAND_TICKS = 0
  check("band 0 -> no band", V2.knob_inp(C, {}).travel_band == 0)
  C.PT2_TRAVEL_BAND_TICKS = save
end
-- number knob: -1 = the old rule (A always needs its cover)
check("PT2_FINISH_NO_COVER_MAX_ANGER default 0.95 (Andrew's value)", C.PT2_FINISH_NO_COVER_MAX_ANGER == 0.95)
check("PT2_FINISH_NO_COVER_MAX_ANGER keel -1", C.PRESETS.keel.PT2_FINISH_NO_COVER_MAX_ANGER == -1)
check("PT2_FINISH_NO_COVER_MAX_ANGER mast -1", C.PRESETS.mast.PT2_FINISH_NO_COVER_MAX_ANGER == -1)
check("SHOT_PATH_SKIP_DEAD_PILLS default true", C.SHOT_PATH_SKIP_DEAD_PILLS == true)
check("SHOT_PATH_SKIP_DEAD_PILLS keel false", C.PRESETS.keel.SHOT_PATH_SKIP_DEAD_PILLS == false)
check("SHOT_PATH_SKIP_DEAD_PILLS mast false", C.PRESETS.mast.SHOT_PATH_SKIP_DEAD_PILLS == false)
do
  local ki = V2.knob_inp(C, {})
  check("finish knob reaches inp", ki.finish_max_anger == 0.95, ki.finish_max_anger)
  check("finish armour is ATTACK_RUSH_MIN_ARMOUR (5)", ki.finish_min_armour == C.ATTACK_RUSH_MIN_ARMOUR
        and ki.finish_min_armour == 5, ki.finish_min_armour)
end

-- PT2_DBL_SPLIT / PT2_DBL_CAN_WIN (Nov 18): on, off in KEEL and MAST
for _, kn in ipairs({ "PT2_DBL_SPLIT", "PT2_DBL_CAN_WIN" }) do
  check(kn .. " default true", C[kn] == true)
  check(kn .. " keel false", C.PRESETS.keel[kn] == false)
  check(kn .. " mast false", C.PRESETS.mast[kn] == false)
end
check("PT2_DBL_SPREAD_TICKS = 5 (tier 10)", C.PT2_DBL_SPREAD_TICKS == 5)
check("PT2_DBL_SPLIT_MAX_TICKS = 10 (2 x spread)", C.PT2_DBL_SPLIT_MAX_TICKS == 10)
check("no tick goal knobs", C.PT2_DBL_TICK_GOAL_MS == nil and C.PT2_DBL_TICK_MARGIN_MS == nil)
check("PT2_DBL_STDERR_LOG default false", C.PT2_DBL_STDERR_LOG == false)
check("PT2_DBL_CACHE_TICKS = 250 (5 s at 50 brain ticks/s)", C.PT2_DBL_CACHE_TICKS == 250)
check("PT2_DBL_EVALS_PER_TICK = 1", C.PT2_DBL_EVALS_PER_TICK == 1)
-- the capacity tier rows carry the double-take values: today's at every tier
for t = 1, 10 do
  local row = C.BRAIN_CAPACITY_LEVELS[t]
  check("tier " .. t .. " dbl_cache 250", row and row.dbl_cache == 250, row and row.dbl_cache)
  check("tier " .. t .. " dbl_evals 1", row and row.dbl_evals == 1, row and row.dbl_evals)
  local sp = ({ 25, 20, 16, 14, 12, 10, 8, 7, 6, 5 })[t]
  check("tier " .. t .. " dbl_spread " .. sp, row and row.dbl_spread == sp, row and row.dbl_spread)
  check("tier " .. t .. " dbl_split = 2 x spread", row and row.dbl_split == 2 * sp, row and row.dbl_split)
  check("tier " .. t .. " no goal fields", row and row.dbl_goal == nil and row.dbl_margin == nil)
end
-- P2: the tier row's pt2_eval picks the evaluator (pt2_eval.lua). The
-- values are Andrew's, approved Nov 23: 10-7 "v2", 6-1 "old".
do
  local E = require("pt2_eval")
  local NEW, OLD = require("pilltake_v2"), require("pilltake_v2_old")
  for t = 1, 10 do
    local row = C.BRAIN_CAPACITY_LEVELS[t]
    local want = (t >= 7) and "v2" or "old"
    check("tier " .. t .. " pt2_eval " .. want, row and row.pt2_eval == want, row and row.pt2_eval)
  end
  local ev0, bt0 = C.PT2_EVALUATOR, C.PT2_EVAL_BY_TIER
  C.PT2_EVALUATOR, C.PT2_EVAL_BY_TIER = "v2", true
  E.use({ _capacity = C.BRAIN_CAPACITY_LEVELS[10] })
  check("tier 10: v2", E.pick() == NEW and E.choice() == "v2", E.choice())
  E.use({ _capacity = C.BRAIN_CAPACITY_LEVELS[6] })
  check("tier 6: old wins over PT2_EVALUATOR v2", E.pick() == OLD and E.choice() == "old", E.choice())
  check("tier 6: a field read comes from old", E.finish_ok == OLD.finish_ok, nil)
  C.PT2_EVALUATOR = "old"
  E.use({ _capacity = C.BRAIN_CAPACITY_LEVELS[8] })
  check("tier 8: v2 wins over PT2_EVALUATOR old", E.pick() == NEW, E.choice())
  E.use({ _capacity = { dbl_cache = 250 } })
  check("a row with no pt2_eval: PT2_EVALUATOR", E.pick() == OLD, E.choice())
  E.use(nil)
  check("no state: PT2_EVALUATOR", E.pick() == OLD, E.choice())
  C.PT2_EVAL_BY_TIER = false
  E.use({ _capacity = C.BRAIN_CAPACITY_LEVELS[10] })
  check("knob off: PT2_EVALUATOR at tier 10", E.pick() == OLD, E.choice())
  -- a job keeps the module of its inp (inp._pt2_ev)
  C.PT2_EVAL_BY_TIER = true
  local called
  local inp = { _pt2_ev = { evaluate_group = function() called = "kept"; return {} end } }
  E.use({ _capacity = C.BRAIN_CAPACITY_LEVELS[1] })
  E.evaluate_group(inp)
  check("evaluate_group uses the inp's module", called == "kept", called)
  E.use(nil)
  C.PT2_EVALUATOR, C.PT2_EVAL_BY_TIER = ev0, bt0
end
do
  local D = require("pt2_dbl")
  check("tier value wins over the knob", D._tier_val({ _capacity = { dbl_cache = 100 } },
        "dbl_cache", "PT2_DBL_CACHE_TICKS", 250) == 100)
  check("no tier row: the knob", D._tier_val({}, "dbl_cache", "PT2_DBL_CACHE_TICKS", 250) == C.PT2_DBL_CACHE_TICKS)
  check("tier row without the field: the knob", D._tier_val({ _capacity = {} },
        "dbl_split", "PT2_DBL_SPLIT_MAX_TICKS", 9) == C.PT2_DBL_SPLIT_MAX_TICKS)
end

-- ── 1. rank order ────────────────────────────────────────────────────────
print("rank order")
local function k(safe, kills, blk, lee, score, depth)
  return { safe = safe, kills = kills, blk = blk, leeway = lee, score = score, depth = depth }
end
check("safe above unsafe", V2.better(k(true, 1, 1, 0, 99), k(false, 3, 2, 8, 0), 6))
check("safe longer chain above shorter", V2.better(k(true, 2, 1, 0, 99), k(true, 1, 2, 8, 0), 6))
check("allied pill cover above walls", V2.better(k(true, 2, 2, 0, 99), k(true, 2, 1, 8, 0), 6))
check("single: more leeway wins in a tier", V2.better(k(true, 1, 1, 6, 99), k(true, 1, 1, 5, 0), 6))
check("chain: lower travel score wins before leeway", V2.better(k(true, 2, 1, 6, 10, 10), k(true, 2, 1, 8, 50, 10), 6))
check("chain: same score, more leeway wins", V2.better(k(true, 2, 1, 6, 10, 10), k(true, 2, 1, 5, 10, 10), 6))
check("leeway past the cap: score decides", V2.better(k(true, 1, 1, 6, 1), k(true, 1, 1, 8, 2), 6))
check("same leeway: lower score wins", V2.better(k(true, 1, 1, 4, 1), k(true, 1, 1, 4, 2), 6))
check("not better than itself", not V2.better(k(true, 1, 1, 4, 1), k(true, 1, 1, 4, 1), 6))
-- depth (chains): ally pill + wall 20 > ally pill 15 > two walls 10
check("chain: ally pill + wall (20) above ally pill (15)", V2.better(k(true, 2, 2, 0, 99, 20), k(true, 2, 2, 8, 0, 15), 6))
check("chain: ally pill (15) above two walls (10)", V2.better(k(true, 2, 2, 0, 99, 15), k(true, 2, 1, 8, 0, 10), 6))
check("chain: depth comes after kills", V2.better(k(true, 3, 1, 0, 99, 10), k(true, 2, 2, 8, 0, 20), 6))
check("chain: same depth, the allied pill breaks the tie", V2.better(k(true, 2, 2, 0, 99, 15), k(true, 2, 1, 8, 0, 15), 6))
check("single take: depth is not a key (v1 parity)", V2.better(k(true, 1, 1, 6, 1, 15), k(true, 1, 1, 6, 2, 30), 6))
-- travel band (PT2_TRAVEL_BAND_TICKS 100 = 6.25 score units of 16 ticks)
print("travel band")
local BAND = 100 / 16
-- chain: near (score 5, 80 t) two walls vs far (score 20, 320 t) ally pill + wall
local near_c, far_c = k(true, 2, 1, 8, 5, 10), k(true, 2, 2, 8, 20, 20)
check("band off: far chain wins on depth", V2.better(far_c, near_c, 6))
check("band 0: same as off", V2.better(far_c, near_c, 6, 0))
check("band on: near chain wins, far is 240 t slower (> 100 t)", V2.better(near_c, far_c, 6, BAND))
check("band on: far chain not above near", not V2.better(far_c, near_c, 6, BAND))
-- inside the band (64 t apart) depth still decides
local mid_c = k(true, 2, 2, 8, 9, 20)
check("band on: within the band depth still wins", V2.better(mid_c, near_c, 6, BAND))
-- single take: near with less leeway vs far with full leeway
local near_s, far_s = k(true, 1, 1, 5, 2), k(true, 1, 1, 8, 12)
check("band off: far single wins on leeway", V2.better(far_s, near_s, 6))
check("band on: near single wins (160 t slower is out)", V2.better(near_s, far_s, 6, BAND))
check("band on: within the band leeway still wins", V2.better(k(true, 1, 1, 8, 7), near_s, 6, BAND))
check("band on: safe still above unsafe", V2.better(k(true, 1, 1, 0, 99), k(false, 1, 1, 8, 0), 6, BAND))
check("band on: kills still above travel", V2.better(k(true, 2, 1, 6, 99, 10), k(true, 1, 1, 8, 0), 6, BAND))
-- set_band: the quickest of the tier sets the band for the whole sort
do
  local list = { k(true, 2, 2, 8, 20, 20), k(true, 2, 1, 8, 5, 10), k(true, 2, 2, 8, 9, 20),
                 k(true, 2, 2, 8, 30, 30), k(false, 2, 1, 8, 1, 10) }
  V2.set_band(list, BAND)
  check("set_band: tier min is the quickest safe chain", list[1].tier_min == 5, list[1].tier_min)
  check("set_band: unsafe tier has its own min", list[5].tier_min == 1, list[5].tier_min)
  check("set_band: 9 is in band of 5", list[3].in_band == true)
  check("set_band: 20 is out of band", list[1].in_band == false)
  table.sort(list, function(a, b) return V2.better(a, b, 6, BAND) end)
  check("sorted: in-band deeper point first (score 9)", list[1].score == 9, list[1].score)
  check("sorted: then the quickest (score 5)", list[2].score == 5, list[2].score)
  check("sorted: out of band by travel (20 before 30)", list[3].score == 20 and list[4].score == 30)
  check("sorted: unsafe last", list[5].safe == false)
end

-- ── 2. evaluate on small maps ───────────────────────────────────────────
local terrain = {}
local function sq(mx, my) return my * 256 + mx end
U.ttype      = function(mx, my) return terrain[sq(mx, my)] or C.T_GRASS end
U.ttype_peek = U.ttype
U.base_tile_stops_tank_shell = function() return false end
cpf.lgm_travel_ticks_map = function() return 10 end

local HOME = { 128.5, 134.5 }
local function world_of(pills)
  local w = { pills = {}, pill_at = {}, bases = {}, base_at = {} }
  for i, p in ipairs(pills) do
    w.pills[i] = p
    w.pill_at[sq(p.mx, p.my)] = { { id = i, pill = p } }
  end
  return w
end
local function run(pills, knobs, npb)
  local save = {}
  for kk, v in pairs(knobs or {}) do save[kk] = C[kk]; C[kk] = v end
  local w = world_of(pills)
  local inp = V2.knob_inp(C, {})
  inp = V2.world_inp(w, C, U, PC, cpf, inp, false)
  inp.A = pills[1]
  inp.radius = 7.0
  inp.num_pill_blockers = npb or 0
  inp.score_fn = function(deg, mx, my, cx, cy)
    local dx, dy = cx - HOME[1], cy - HOME[2]
    return math.sqrt(dx * dx + dy * dy)
  end
  local res = V2.evaluate(inp)
  for kk in pairs(knobs or {}) do C[kk] = save[kk] end
  return res.best, res
end

print("chain")
do
  -- A at (128,120); B next to it, east. From points east of the pair A's
  -- shells meet B before the tank: a two-pill chain, held by two walls
  -- (PT2_CHAIN_BLOCKER_MIN_SHOTS 10) on B's ray that are also on A's ray
  -- behind B. (With B two squares east, Nov 16 first pass, the two-wall
  -- spots fail the leeway minimum.)
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local B = { mx = 129, my = 120, owner = "hostile", health = 15 }
  local b = run({ A, B }, { PT2_CHAINS = true })
  check("a safe chain is found", b and b.safe, b and b.safe)
  check("it is a two-pill chain", b and b.kills == 2 and b.chain_mode, b and b.kills)
  -- PT2_LINK_COVERS_PREV (default on): B covers A; only B, the last
  -- chain pill, needs the 10 shots.
  check("the last chain ray has cover >= 10", b and b.depth >= 10, b and b.depth)
  local A_link = false
  for _, r in ipairs(b and b.rays or {}) do if r.X == A and r.link_cover then A_link = true end end
  check("A's ray is covered by B (the link)", A_link)
  local b_off = run({ A, B }, { PT2_CHAINS = true, PT2_LINK_COVERS_PREV = false })
  check("link rule off: A's own cover >= 10", b_off and b_off.have >= 10, b_off and b_off.have)
  check("two walls hold it", b and #b.plan == 2, b and #b.plan)
  check("its depth is two walls (10)", b and b.depth == 10, b and b.depth)
  -- hold point: the tank waits where neither pill hits it with the cover
  -- that stands now (the walls are not built yet)
  check("a take that builds has a hold point", b and b.hold ~= nil)
  check("the hold point is safe", b and b.hold and b.hold.safe, b and b.hold and b.hold.why)
  check("the hold point is at or past the approach point", b and b.hold and b.hold.d >= 2.25 - 1e-6,
        b and b.hold and b.hold.d)
  if b and b.hold then
    local SHm = require("shadow")
    local hx, hy = math.floor(b.hold.fx * 256), math.floor(b.hold.fy * 256)
    local env = V2.world_env(world_of({ A, B }), C, U, PC)
    local hit = false
    local pr = SHm.PILL_RANGE + (C.PT2_RANGE_MARGIN_WU or 128)
    for _, X in ipairs({ A, B }) do
      local sx, sy = X.mx * 256 + 128, X.my * 256 + 128
      local dx, dy = sx - hx, sy - hy
      if dx * dx + dy * dy <= pr * pr then
        local r = SHm.cast(env, sx, sy, hx, hy, SHm.PILL_FIRE_LEN, nil)
        if r.hit == "tank" then hit = true end
      end
    end
    check("each pill is out of range of the hold point, or its shell stops", not hit)
    print(string.format("    hold (%.2f,%.2f) d=%.2f", b.hold.fx, b.hold.fy, b.hold.d))
  end
  check("it has the S6 leeway (>= 6 of 8)", b and (b.leeway or 0) >= 6, b and b.leeway)
  local nb = run({ A, B }, { PT2_CHAINS = false })
  check("chains off: a single take", nb and nb.kills == 1 and not nb.chain_mode, nb and nb.kills)
  check("chains off: still safe", nb and nb.safe, nb and nb.safe)
  check("chains off: A's ray has 15", nb and nb.have >= 15, nb and nb.have)
  if b then
    print(string.format("    chain best deg=%.1f leeway=%s plan=%d  single best deg=%.1f plan=%d",
      b.deg, tostring(b.leeway), #b.plan, nb and nb.deg or -1, nb and #nb.plan or -1))
  end
end

print("allied pill as the chain's last blocker")
do
  -- The same pair, and an allied pill (15) east of B on the rays: it meets
  -- the chain bar (10) alone, so nothing is built, and the depth is 15.
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local B = { mx = 129, my = 120, owner = "hostile", health = 15 }
  local F = { mx = 130, my = 120, owner = "friendly", health = 15 }
  local b = run({ A, B, F }, { PT2_CHAINS = true })
  check("ally: a safe two-pill chain", b and b.safe and b.kills == 2, b and b.kills)
  check("ally: nothing to build", b and #b.plan == 0, b and #b.plan)
  check("ally: depth 15 or more", b and (b.depth or 0) >= 15, b and b.depth)
  check("ally: no hold point needed", b and b.hold == nil)
end

print("group: the near-side mirror chain")
do
  -- Oil Rig shape: target T and P side by side, an allied pill on each
  -- side. T -> P -> ally west is a chain from the west; P -> T -> ally east
  -- is the same chain from the east. Home is east: the east chain must win,
  -- with P as the pill the tank shoots and T dying second.
  local T = { mx = 130, my = 120, owner = "hostile", health = 15 }
  local P = { mx = 129, my = 120, owner = "hostile", health = 15 }
  local FW = { mx = 128, my = 120, owner = "friendly", health = 15 }
  local FE = { mx = 131, my = 120, owner = "friendly", health = 15 }
  local h1, h2 = HOME[1], HOME[2]
  HOME[1], HOME[2] = 145.5, 120.5
  local w = world_of({ T, P, FW, FE })
  local inp = V2.knob_inp(C, {})
  inp = V2.world_inp(w, C, U, PC, cpf, inp, false)
  inp.A = T
  inp.radius = 7.0
  inp.num_pill_blockers = 0
  inp.score_fn = function(deg, mx, my, cx, cy)
    local dx, dy = cx - HOME[1], cy - HOME[2]
    return math.sqrt(dx * dx + dy * dy)
  end
  local solo = V2.evaluate(inp).best
  local res = V2.evaluate_group(inp)
  HOME[1], HOME[2] = h1, h2
  local b = res.best
  check("group: target-only best is west", solo and solo.cx < 130, solo and solo.cx)
  check("group: a safe two-pill chain", b and b.safe and b.kills == 2, b and b.kills)
  check("group: the tank shoots P", res.A == P, res.A and res.A.mx)
  check("group: the target dies in the chain", b and b.in_chain and b.in_chain[T] == 2)
  check("group: the standoff is east", b and b.cx > 131, b and b.cx)
  check("group: inp.A is the target again", inp.A == T)
  -- PT2_DBL_SPLIT: the same group in parts, one evaluate per group_step,
  -- and the old one-loop evaluate_group (copied here) give the same answer.
  HOME[1], HOME[2] = 145.5, 120.5
  local job = V2.group_begin(inp)
  check("split: one part before the first step", V2.group_parts_left(job) == 1)
  local steps = 0
  repeat steps = steps + 1 until V2.group_step(job) or steps > 20
  local old = (function()
    local goalA = inp.A
    local r0 = V2.evaluate(inp)
    r0.A, r0.goal_A = goalA, goalA
    local pr = inp.pill_range + inp.range_margin
    local ne = r0.n_eval
    for _, p in ipairs(r0.hostile) do
      local dx, dy = (p.mx - goalA.mx) * 256, (p.my - goalA.my) * 256
      if p ~= goalA and dx * dx + dy * dy <= pr * pr and not (inp.chain_skip and inp.chain_skip[p]) then
        inp.A = p
        local r = V2.evaluate(inp)
        ne = ne + r.n_eval
        local bb = r.best
        if bb and bb.safe and bb.chain_mode and bb.in_chain and bb.in_chain[goalA]
           and (not r0.best or V2.better(bb, r0.best, inp.leeway_min, inp.travel_band)) then
          r.A, r.goal_A = p, goalA
          r0 = r
        end
      end
    end
    inp.A = goalA
    r0.n_eval = ne
    return r0
  end)()
  HOME[1], HOME[2] = h1, h2
  local jb, ob = job.res.best, old.best
  check("split: 2 parts (T, then P as A)", steps == 2, steps)
  check("split: same A as the one-loop group", job.res.A == old.A and job.res.A == P)
  check("split: same point and kills", jb and ob and jb.cx == ob.cx and jb.cy == ob.cy
        and jb.kills == ob.kills and jb.safe == ob.safe, jb and jb.cx)
  check("split: same n_eval", job.res.n_eval == old.n_eval, job.res.n_eval)
  check("split: inp.A is the target after the parts", inp.A == T)
  print(string.format("    target-only so=(%.2f,%.2f)  group so=(%.2f,%.2f) A=(%d,%d)",
    solo and solo.cx or -1, solo and solo.cy or -1, b and b.cx or -1, b and b.cy or -1,
    res.A.mx, res.A.my))
end

print("allied pill blocker")
do
  -- An allied pill (15) on the north side of A: the north standoff needs no
  -- wall. Home is south, so walls would win on travel alone.
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local F = { mx = 128, my = 117, owner = "friendly", health = 15 }
  local on = run({ A, F }, { PT2_CHAINS = true, PT2_OWN_PILL_BONUS = true })
  check("knob on: standoff on the allied pill's side", on and on.blk == 2 and (on.deg > 300 or on.deg < 60),
        on and string.format("deg=%.1f blk=%d", on.deg, on.blk))
  local off = run({ A, F }, { PT2_CHAINS = true, PT2_OWN_PILL_BONUS = false })
  check("knob off: no allied-pill tier", off and off.blk == 1, off and off.blk)
  print(string.format("    on deg=%.1f plan=%d   off deg=%.1f plan=%d",
    on and on.deg or -1, on and #on.plan or -1, off and off.deg or -1, off and #off.plan or -1))
end

print("aim nudge (PT2_AIM_NUDGE, v1's forward nudge)")
do
  -- A wall two squares north-west of A stops the centre aim from the point
  -- at bearing 302, radius 7.72; the corner aims fall short there. v1's
  -- nudge (8 wu steps toward A) finds the BR corner clear 24 wu forward.
  local SHm = require("shadow")
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  terrain[sq(126, 118)] = C.T_BUILDING
  local w = world_of({ A })
  local function mk(on)
    local save = C.PT2_AIM_NUDGE
    C.PT2_AIM_NUDGE = on
    local inp = V2.world_inp(w, C, U, PC, cpf, V2.knob_inp(C, {}), false)
    C.PT2_AIM_NUDGE = save
    inp.A = A
    return inp, { inp = inp, A = A, env = inp.env, reach_cache = {}, hostile = { A } }
  end
  local deg, R = 302, 7.72
  local cx = A.mx + 0.5 + math.sin(math.rad(deg)) * R
  local cy = A.my + 0.5 - math.cos(math.rad(deg)) * R
  local px, py = math.floor(cx * 256), math.floor(cy * 256)
  local inp_on, ctx_on = mk(true)
  local inp_off, ctx_off = mk(false)
  check("knob reaches inp", inp_on.aim_nudge == true and inp_off.aim_nudge == false)
  check("v1's nudge values: step 8 wu, 32 steps",
        inp_on.nudge_step_wu == 8 and inp_on.nudge_max_steps == 32)
  local _, w_c = V2._eval_point(ctx_on, px, py, nil, 1)
  check("nudge 0: a wall stops the centre aim", w_c == "OUTGOING:wall", w_c)
  local r5, w5 = V2._eval_point(ctx_on, px, py, nil, 5)
  check("nudge 0: the BR corner aim falls short", r5 == nil and w5 == "OUTGOING:short", w5)
  local r_off, why_off = V2._eval_point(ctx_off, px, py, nil, nil, nil, nil, true)
  check("knob off: no clear aim, point rejected",
        r_off == nil and tostring(why_off):sub(1, 9) == "OUTGOING:", why_off)
  local r = V2._eval_point(ctx_on, px, py, nil, nil, nil, nil, true)
  check("knob on: a clear aim after a nudge", r ~= nil)
  check("the smallest nudge that clears it: 24 wu", r and r.nudge_wu == 24, r and r.nudge_wu)
  check("the clear aim is the BR corner", r and r.aim_idx == 5, r and r.aim_idx)
  local acx, acy = A.mx * 256 + 128, A.my * 256 + 128
  local d0 = math.sqrt((acx - px) ^ 2 + (acy - py) ^ 2)
  local d1 = r and math.sqrt((acx - r.px) ^ 2 + (acy - r.py) ^ 2) or 0
  check("the point moved 24 wu toward A", r and math.abs((d0 - d1) - 24) < 1.5, d0 - d1)
  local dmv = r and math.sqrt((r.px - px) ^ 2 + (r.py - py) ^ 2) or 0
  check("on the same bearing (moved 24 wu in all)", math.abs(dmv - 24) < 1.5, dmv)
  local rc = r and V2._eval_point(ctx_on, r.px, r.py, nil, 5)
  check("the BR aim is clear from the nudged point itself", rc ~= nil)
  if r then
    local fresh = SHm.cast(inp_on.env, acx, acy, r.px, r.py, SHm.PILL_FIRE_LEN, nil)
    check("A's ray is cast to the nudged point",
          r.rays[1].hit == fresh.hit and r.rays[1].n_sq == fresh.n_sq and r.rays[1].end_x == fresh.end_x,
          r.rays[1].hit)
  end
  -- evaluate(): only this bearing on one ring; the drive goes to the nudged point.
  local function ev(inp)
    inp.radius = R
    inp.radii_offsets = { 0 }
    inp.num_pill_blockers = 0
    inp.banned = function(dg) return math.abs(dg - deg) > 1e-6 end
    inp.score_fn = function() return 1 end
    return V2.evaluate(inp)
  end
  local res_on = ev(inp_on)
  local b = res_on.best
  check("evaluate: the nudged point is the best", b and b.nudge_wu == 24 and b.deg == deg, b and b.nudge_wu)
  check("evaluate: best.cx/cy is the nudged point", b and r and b.cx == r.px / 256 and b.cy == r.py / 256,
        b and string.format("(%.3f,%.3f)", b.cx, b.cy))
  local out = b and V2.to_scan_result(res_on, A, { cx = cx, cy = cy, mx = math.floor(cx), my = math.floor(cy) })
  check("scan result: the drive point is nudged, nudge_wu handed on",
        out and out.best.cx == b.cx and out.best.cy == b.cy and out.best.aims[5].nudge_wu == 24)
  local res_off = ev(inp_off)
  check("evaluate, knob off: nothing found on this bearing", res_off.best == nil and res_off.n_reject == 1,
        res_off.best and res_off.best.deg)
  terrain[sq(126, 118)] = nil
  if r then
    print(string.format("    ring (%.2f,%.2f) -> nudged (%.2f,%.2f) nudge=%d aim=%s",
      cx, cy, r.px / 256, r.py / 256, r.nudge_wu, V2.AIM_NAMES[r.aim_idx]))
  end
end

print("finish: a 1-health pill without cover (PT2_FINISH_NO_COVER_MAX_ANGER)")
do
  -- A alone at (128,120). Home is south. The old rule: A's ray needs 15
  -- (three walls). The finish rule: no cover on A's ray, so the nearest
  -- point with no build wins.
  local function fin_run(pills, knobs, armour)
    local save = {}
    for kk, v in pairs(knobs or {}) do save[kk] = C[kk]; C[kk] = v end
    local w = world_of(pills)
    local inp = V2.knob_inp(C, {})
    inp = V2.world_inp(w, C, U, PC, cpf, inp, false)
    for kk in pairs(knobs or {}) do C[kk] = save[kk] end
    inp.A = pills[1]
    inp.radius = 7.0
    inp.num_pill_blockers = 0
    inp.armour = armour
    inp.keep_all = true
    inp.score_fn = function(deg, mx, my, cx, cy)
      local dx, dy = cx - HOME[1], cy - HOME[2]
      return math.sqrt(dx * dx + dy * dy)
    end
    local res = V2.evaluate(inp)
    return res.best, res, inp
  end
  local function A_of(hp, anger) return { mx = 128, my = 120, owner = "hostile", health = hp, anger = anger } end
  local function old_bar(b, name)
    check(name .. ": old bar, A needs 15", b and b.need == 15 and not b.finish, b and b.need)
    check(name .. ": old bar, walls are built", b and #b.plan > 0, b and #b.plan)
  end
  -- 1. 1 health, anger 0.9, armour 10: the finish rule
  local b, res = fin_run({ A_of(1, 0.9) }, nil, 10)
  check("1hp anger 0.9: safe", b and b.safe, b and b.safe)
  check("1hp anger 0.9: finish mode", b and b.finish == true, b and b.finish)
  check("1hp anger 0.9: A needs no cover (need 0)", b and b.need == 0, b and b.need)
  check("1hp anger 0.9: nothing to build", b and #b.plan == 0, b and #b.plan)
  check("1hp anger 0.9: no hold point", b and b.hold == nil)
  check("1hp anger 0.9: the near point wins (south, toward home)",
        b and (b.deg >= 160 and b.deg <= 200), b and b.deg)
  local minsc = math.huge
  for _, r in ipairs(res.all or {}) do
    if r.safe and r.score and r.score < minsc then minsc = r.score end
  end
  check("1hp anger 0.9: best is in the band of the quickest safe point", b and b.score <= minsc + 100 / 16, b and b.score)
  local sc = b and V2.to_scan_result(res, b.chain[1], { cx = b.cx, cy = b.cy, mx = b.mx, my = b.my })
  check("1hp anger 0.9: the scan result carries finish", sc and sc.chain.finish == true and sc.chain.need == 0)
  -- the rule's edge: anger exactly at the knob qualifies
  local be = fin_run({ A_of(1, 0.95) }, nil, 10)
  check("1hp anger 0.95 (the knob): finish", be and be.finish == true and be.need == 0, be and be.need)
  -- anger_of (the bot's decayed anger) is used when given
  do
    local w = world_of({ A_of(1, 0.99) })
    local inp = V2.world_inp(w, C, U, PC, cpf, V2.knob_inp(C, {}), false)
    inp.armour = 10
    check("anger 0.99: no finish", V2.finish_ok(inp, w.pills[1]) == false)
    inp.anger_of = function() return 0.5 end
    check("anger_of 0.5 overrides p.anger: finish", V2.finish_ok(inp, w.pills[1]) == true)
  end
  -- 2. 1 health, anger 0.97: the old bar
  old_bar((fin_run({ A_of(1, 0.97) }, nil, 10)), "1hp anger 0.97")
  -- 3. 2 health, anger 0: the old bar
  old_bar((fin_run({ A_of(2, 0) }, nil, 10)), "2hp anger 0")
  -- 4. our armour 4 (< ATTACK_RUSH_MIN_ARMOUR 5): the old bar
  old_bar((fin_run({ A_of(1, 0.5) }, nil, 4)), "armour 4")
  check("armour 5 (the minimum): finish", (fin_run({ A_of(1, 0.5) }, nil, 5)).finish == true)
  -- 5. knob -1 (KEEL / MAST): the old bar
  old_bar((fin_run({ A_of(1, 0) }, { PT2_FINISH_NO_COVER_MAX_ANGER = -1 }, 10)), "knob -1")
  -- 6. another pill in range still needs its cover
  do
    local A = A_of(1, 0.9)
    local B = { mx = 131, my = 120, owner = "hostile", health = 15 }
    local bb = fin_run({ A, B }, nil, 10)
    check("other pill: finish on A", bb and bb.finish == true and bb.need == 0, bb and bb.need)
    local b_ok, b_bar = false, nil
    for _, r in ipairs(bb and bb.rays or {}) do
      if r.X == B then
        b_bar = r.bar
        b_ok = r.ok and (r.hit == "reach" or r.link_cover or r.cover >= r.bar)
      end
    end
    check("other pill: B keeps its bar (>= one wall)", b_bar ~= nil and b_bar >= 5, b_bar)
    check("other pill: B's ray is out of reach or covered at the best point", b_ok)
    -- a point south of A that B can hit with no cover: B fails, A passes
    local w = world_of({ A, B })
    local inp = V2.world_inp(w, C, U, PC, cpf, V2.knob_inp(C, {}), false)
    inp.A, inp.armour = A, 10
    local px, py = math.floor(128.5 * 256), math.floor(127.5 * 256)
    local why = V2.judge(inp, px, py, nil, nil)
    check("other pill: B in range with no cover fails the point", why == "OVERLAP:2", why)
    inp.finish_max_anger = -1
    local why0 = V2.judge(inp, px, py, nil, nil)
    check("other pill, knob -1: A's own bar fails first", tostring(why0):sub(1, 5) == "DEPTH", why0)
    if bb and bb.hold then
      check("other pill: the hold point is not stopped by A", bb.hold.safe or bb.hold.why ~= "1", bb.hold.why)
    end
  end
  -- 7. the shadow overlay uses the same bar: under the finish rule every
  --    cell A can shoot is in A's shadow
  do
    local viz = require("viz")
    local PV = require("pt2_viz")
    local on0 = viz.is_on
    viz.is_on = function(id) return id == "pt2_shadow" end
    local bf, rf, inpf = fin_run({ A_of(1, 0.9) }, nil, 10)
    local g = {}
    PV.capture(g, inpf, rf, nil, nil, 0)
    local n_fin = g._pt2_viz and g._pt2_viz.cells and g._pt2_viz.cells[1] and #g._pt2_viz.cells[1] or 0
    local bo, ro, inpo = fin_run({ A_of(1, 0.97) }, nil, 10)
    local g2 = {}
    PV.capture(g2, inpo, ro, nil, nil, 0)
    local n_old = g2._pt2_viz and g2._pt2_viz.cells and g2._pt2_viz.cells[1] and #g2._pt2_viz.cells[1] or 0
    viz.is_on = on0
    check("shadow: finish shades more of A's ring than the old bar", n_fin > n_old,
          string.format("%d vs %d", n_fin, n_old))
    print(string.format("    finish so=(%.2f,%.2f) deg=%.1f plan=%d | old so=(%.2f,%.2f) deg=%.1f plan=%d | shadow %d vs %d",
      bf.cx, bf.cy, bf.deg, #bf.plan, bo.cx, bo.cy, bo.deg, #bo.plan, n_fin, n_old))
  end
end

-- ── pt2_attack.build_inp: v1's green filter and the finish ───────────────
-- v1's ring spots: north (deg 0) not green (adj 50), south (deg 180) green
-- (adj 1). Off a finish, a point next to a non-green spot costs 10000 +
-- its adj; on a finish (A 1 health, anger under the knob) it costs its
-- travel only, as every other point.
print("pt2_attack: the green filter is off on a finish")
do
  package.loaded["print2"] = package.loaded["print2"] or setmetatable({}, { __call = function() end })
  local PA = require("pt2_attack")
  local save_dij, save_kind = cpf.dijkstra_lookup_by_kind, cpf.KIND_NORMAL
  cpf.KIND_NORMAL = cpf.KIND_NORMAL or 0
  cpf.dijkstra_lookup_by_kind = function() return 42 end
  local function score_at(hp, anger, deg)
    local A = { mx = 128, my = 120, owner = "hostile", health = hp, anger = anger, anger_tick = 100 }
    local w = world_of({ A })
    local goal = { scan_spots = {
      { deg = 0,   has_los = true, _adj_score = 50 },
      { deg = 180, has_los = true, _adj_score = 1 },
    } }
    local inp = PA.build_inp({}, goal, { armour = 10, tankx = 128 * 256, tanky = 134 * 256 },
                             w, A, false, 0, 7.0, 100)
    return inp.score_fn(deg, 128, 113, 128.5, 113.5), inp
  end
  local s1, i1 = score_at(15, 0, 0)
  check("not a finish: non-green spot costs 10000 + adj", s1 == 10050, s1)
  check("not a finish: penalty counted", i1.pen_n == 1 and i1.pen_skip_n == 0, i1.pen_n)
  local s2 = score_at(15, 0, 180)
  check("not a finish: green spot costs its travel", s2 == 42, s2)
  local s3, i3 = score_at(1, 0.9, 0)
  check("finish: non-green spot costs its travel only", s3 == 42, s3)
  check("finish: skip counted, no penalty", i3.pen_n == 0 and i3.pen_skip_n == 1, i3.pen_skip_n)
  local s4 = score_at(1, 0.97, 0)
  check("1hp anger 0.97 (over the knob): penalty stays", s4 == 10050, s4)
  local save_k = C.PT2_FINISH_NO_COVER_MAX_ANGER
  C.PT2_FINISH_NO_COVER_MAX_ANGER = -1
  local s5 = score_at(1, 0, 0)
  C.PT2_FINISH_NO_COVER_MAX_ANGER = save_k
  check("finish knob -1 (KEEL / MAST): penalty stays", s5 == 10050, s5)
  cpf.dijkstra_lookup_by_kind, cpf.KIND_NORMAL = save_dij, save_kind
end

-- ── pt2_attack.build_inp: a finish prices travel without A's danger ─────
-- Arena 20261117_113930_1 t=850: the point 2.4 squares from the tank
-- costs 242 with A's danger on its path, a far point 112. Without A's
-- danger the near point costs 40, the far one 100. On a finish the score
-- is the cost less A's danger (the C subtract lookup with
-- threat.pill_contrib[A]), so the near point wins; off a finish, and with
-- the knob at -1, the plain lookup stays and the subtract is never called.
print("pt2_attack: a finish prices travel without A's danger")
do
  local PA = require("pt2_attack")
  local save_dij, save_sub, save_kind = cpf.dijkstra_lookup_by_kind, cpf.dijkstra_lookup_subtract_by_kind, cpf.KIND_NORMAL
  local save_threat = package.loaded["threat"]
  cpf.KIND_NORMAL = cpf.KIND_NORMAL or 0
  local NEAR, FAR = { 130, 113 }, { 126, 113 }
  local full  = { [NEAR[2] * 256 + NEAR[1]] = 242, [FAR[2] * 256 + FAR[1]] = 112 }
  local minus = { [NEAR[2] * 256 + NEAR[1]] = 40,  [FAR[2] * 256 + FAR[1]] = 100 }
  local sub_calls, sub_tbl = 0, nil
  cpf.dijkstra_lookup_by_kind = function(_, x, y) return full[y * 256 + x] or 500 end
  cpf.dijkstra_lookup_subtract_by_kind = function(_, x, y, _, t)
    sub_calls = sub_calls + 1; sub_tbl = t
    return minus[y * 256 + x] or 500
  end
  local A = { mx = 128, my = 120, owner = "hostile", health = 1, anger = 0.9, anger_tick = 100 }
  local A_contrib = { [113 * 256 + 128] = 50 }
  package.loaded["threat"] = { pill_contrib = { [120 * 256 + 128] = A_contrib } }
  -- both points next to a green spot: no green penalty, travel only
  local function scores(hp, anger, knob)
    A.health, A.anger = hp, anger
    local save_k = C.PT2_FINISH_NO_COVER_MAX_ANGER
    if knob then C.PT2_FINISH_NO_COVER_MAX_ANGER = knob end
    local w = world_of({ A })
    local goal = { scan_spots = { { deg = 0, has_los = true, _adj_score = 1 } } }
    local inp = PA.build_inp({}, goal, { armour = 10, tankx = 130 * 256, tanky = 115 * 256 },
                             w, A, false, 0, 7.0, 100)
    local n = inp.score_fn(0, NEAR[1], NEAR[2], NEAR[1] + 0.5, NEAR[2] + 0.5)
    local f = inp.score_fn(0, FAR[1], FAR[2], FAR[1] + 0.5, FAR[2] + 0.5)
    C.PT2_FINISH_NO_COVER_MAX_ANGER = save_k
    return n, f, inp
  end
  sub_calls = 0
  local n1, f1, i1 = scores(1, 0.9)
  check("finish: near point priced without A (40)", n1 == 40, n1)
  check("finish: far point priced without A (100)", f1 == 100, f1)
  check("finish: near point wins", n1 < f1, string.format("%s vs %s", n1, f1))
  check("finish: subtract gets A's own contrib table", sub_tbl == A_contrib, tostring(sub_tbl))
  check("finish: noA_n counts both points", i1.noA_n == 2 and sub_calls == 2, i1.noA_n)
  sub_calls = 0
  local n2, f2, i2 = scores(15, 0)
  check("not a finish: plain travel, far point wins", n2 == 242 and f2 == 112, string.format("%s %s", n2, f2))
  check("not a finish: subtract never called", sub_calls == 0 and i2.noA_n == 0, sub_calls)
  sub_calls = 0
  local n3, f3, i3 = scores(1, 0.97)
  check("1hp anger 0.97 (over the knob): plain travel", n3 == 242 and f3 == 112 and sub_calls == 0, n3)
  sub_calls = 0
  local n4, f4, i4 = scores(1, 0, -1)
  check("finish knob -1 (KEEL / MAST): plain travel, bit for bit", n4 == 242 and f4 == 112
        and sub_calls == 0 and i4.noA_n == 0, string.format("%s %s", n4, f4))
  -- no contrib table for A (not stamped): the plain lookup, as before
  package.loaded["threat"] = { pill_contrib = {} }
  sub_calls = 0
  local n5, f5, i5 = scores(1, 0.9)
  check("finish, A not stamped: plain travel", n5 == 242 and f5 == 112 and sub_calls == 0
        and i5.noA_n == 0, n5)
  package.loaded["threat"] = save_threat
  cpf.dijkstra_lookup_by_kind, cpf.dijkstra_lookup_subtract_by_kind, cpf.KIND_NORMAL = save_dij, save_sub, save_kind
end

-- ── SHOT_PATH_SKIP_DEAD_PILLS: a dead pill lets the shell pass ──────────
print("SHOT_PATH_SKIP_DEAD_PILLS")
do
  local live = { mx = 130, my = 122, owner = "hostile", health = 15 }
  local dead = { mx = 130, my = 122, owner = "hostile", health = 0 }
  local carried = { mx = 130, my = 122, owner = "hostile", health = 15, in_tank = true }
  local ours = { mx = 130, my = 122, owner = "ours", health = 3 }
  local function stops(p, knob)
    local save = C.SHOT_PATH_SKIP_DEAD_PILLS
    C.SHOT_PATH_SKIP_DEAD_PILLS = knob
    local w = { pills = { [1] = p } }
    local r = U.pill_entry_stops_shell(w, { id = 1, pill = p }, 130, 122)
    C.SHOT_PATH_SKIP_DEAD_PILLS = save
    return r
  end
  check("knob on: live hostile pill stops the shell", stops(live, true) == true)
  check("knob on: dead pill lets it pass", stops(dead, true) == false)
  check("knob on: pill in a tank lets it pass", stops(carried, true) == false)
  check("knob on: our own live pill stops it", stops(ours, true) == true)
  check("knob off: dead pill stops it (old rule)", stops(dead, false) == true)
  -- the live table wins over a stale pill_at entry
  do
    local w = { pills = { [1] = dead } }
    check("knob on: stale entry (live copy dead) lets it pass",
          U.pill_entry_stops_shell(w, { id = 1, pill = live }, 130, 122) == false)
  end
  -- pathfinder.pill_shots_clear: a shot past a dead pill is clear
  local PF = require("pathfinder")
  local save_sim = cpf.simulate_shot
  cpf.SHOT_TANK = cpf.SHOT_TANK or 0
  -- a straight east line from (125,122) to the target at (135,122)
  cpf.simulate_shot = function()
    local out = {}
    for mx = 125, 136 do out[#out + 1] = { mx = mx, my = 122 } end
    return out
  end
  local target = { mx = 135, my = 122, owner = "hostile", health = 15 }
  local function clear_with(p, knob)
    local save = C.SHOT_PATH_SKIP_DEAD_PILLS
    C.SHOT_PATH_SKIP_DEAD_PILLS = knob
    local w = { pills = { [1] = target, [2] = p },
                pill_at = { [sq(135, 122)] = { { id = 1, pill = target } },
                            [sq(130, 122)] = { { id = 2, pill = p } } } }
    local ok = PF.pill_shots_clear(125 * 256 + 128, 122 * 256 + 128, target, w)
    C.SHOT_PATH_SKIP_DEAD_PILLS = save
    return ok
  end
  check("pill_shots_clear knob on: dead pill on the line, clear", clear_with(dead, true) == true)
  check("pill_shots_clear knob on: our live pill on the line, blocked", clear_with(ours, true) == false)
  check("pill_shots_clear knob off: dead pill on the line, blocked", clear_with(dead, false) == false)
  cpf.simulate_shot = save_sim
end

-- -- pilltake_v2.better: the last tie-break (smaller angle, larger radius) --
do
  print("\n[v2 better: the last tie-break]")
  local NV = require("pilltake_v2")
  local function pt(deg, radius)
    return { safe = true, kills = 1, score = 10, blk = 1, leeway = 8, depth = 0, deg = deg, radius = radius }
  end
  local a, b = pt(30, 7), pt(40, 7)
  check("smaller angle wins (a, b)", NV.better(a, b, 8, 0) == true)
  check("smaller angle wins (b, a)", NV.better(b, a, 8, 0) == false)
  local c, d = pt(30, 8), pt(30, 7)
  check("same angle: larger radius wins (c, d)", NV.better(c, d, 8, 0) == true)
  check("same angle: larger radius wins (d, c)", NV.better(d, c, 8, 0) == false)
  local e, f = pt(30, 7), pt(30, 7)
  check("identical points: neither wins", NV.better(e, f, 8, 0) == false and NV.better(f, e, 8, 0) == false)
  -- an earlier key still comes first: the lower score wins over the angle
  local g, h = pt(40, 7), pt(30, 7)
  g.score = 5
  check("an earlier key comes before the angle", NV.better(g, h, 8, 0) == true and NV.better(h, g, 8, 0) == false)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
