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

-- ── 1. rank order ────────────────────────────────────────────────────────
print("rank order")
local function k(safe, kills, blk, lee, score)
  return { safe = safe, kills = kills, blk = blk, leeway = lee, score = score }
end
check("safe above unsafe", V2.better(k(true, 1, 1, 0, 99), k(false, 3, 2, 8, 0), 6))
check("safe longer chain above shorter", V2.better(k(true, 2, 1, 0, 99), k(true, 1, 2, 8, 0), 6))
check("allied pill cover above walls", V2.better(k(true, 2, 2, 0, 99), k(true, 2, 1, 8, 0), 6))
check("more leeway wins in a tier", V2.better(k(true, 2, 1, 6, 99), k(true, 2, 1, 5, 0), 6))
check("leeway past the cap: score decides", V2.better(k(true, 1, 1, 6, 1), k(true, 1, 1, 8, 2), 6))
check("same leeway: lower score wins", V2.better(k(true, 1, 1, 4, 1), k(true, 1, 1, 4, 2), 6))
check("not better than itself", not V2.better(k(true, 1, 1, 4, 1), k(true, 1, 1, 4, 1), 6))

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
  -- A at (128,120); B two squares east of A. From points east of the pair
  -- A's shells meet B before the tank: a two-pill chain, held by one wall
  -- on B's ray that is also on A's ray behind B.
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local B = { mx = 130, my = 120, owner = "hostile", health = 15 }
  local b = run({ A, B }, { PT2_CHAINS = true })
  check("a safe chain is found", b and b.safe, b and b.safe)
  check("it is a two-pill chain", b and b.kills == 2 and b.chain_mode, b and b.kills)
  check("its cover per chain ray >= 5", b and b.have >= 5, b and b.have)
  check("one wall holds it", b and #b.plan == 1, b and #b.plan)
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
