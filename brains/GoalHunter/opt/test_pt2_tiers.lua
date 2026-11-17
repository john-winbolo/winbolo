-- =========================================================================
-- test_pt2_tiers.lua -- standalone unit tests (2026-11-16) for the pill
-- take v2 rank (pilltake_v2.lua):
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
local V2  = require("pilltake_v2")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

-- ── 0. knobs ─────────────────────────────────────────────────────────────
print("knobs")
check("PILLTAKE_V2_EVALUATOR ships off", C.PILLTAKE_V2_EVALUATOR == false)
check("PT2_CHAIN_BLOCKER_MIN_SHOTS = 10 (Andrew's value, two walls)", C.PT2_CHAIN_BLOCKER_MIN_SHOTS == 10)
for _, kn in ipairs({ "PT2_CHAINS", "PT2_LINK_COVERS_PREV", "PT2_LEEWAY", "PT2_OWN_PILL_BONUS" }) do
  check(kn .. " default true", C[kn] == true)
  check(kn .. " keel false", C.PRESETS.keel[kn] == false)
  check(kn .. " mast false", C.PRESETS.mast[kn] == false)
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

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
