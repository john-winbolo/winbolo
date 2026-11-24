-- =========================================================================
-- test_pt2_blocker_mark.lua -- standalone unit tests (2026-11-24) for
-- C.PT2_BLOCKER_MARK. Owner: "if a bot is using a built pill as a blocker
-- it needs to communicate that".
--
-- A pill a pill take uses as a blocker is marked in use (the pblk tiles,
-- init.lua -> p._in_use) from the moment the take is the plan, and
-- reposition skips it. Arena: dt_demo_nov24 a1_noc_gap_w (open grass;
-- neutral pills A (124,118) and B (126,118); our pill (121,118); the tank
-- at (112,118)). The old code marked only the attack_pill goal's own scan,
-- so at brain t=15 reposition won (90.7 vs attack_pill 123.2) and shot our
-- pill: the take took 2950 ticks against 2008 without it.
--
--   1. knobs: on by default, off in MAST.
--   2. the gap_w shadow plan (the real v2 evaluator, Lua copy): with our
--      pill it needs no wall and its entry names (121,118); without it,
--      two walls and no blocker pill. A carried-pill plan names the square
--      it places the pill on.
--   3. pending plan: the best pool-6 row priced by that plan marks the
--      tile; a cheaper row with no double take, a blitz-skipped row, a
--      rejected row and the knob off do not.
--   4. chain latch: an attack_pill take marks; capture_pill and goal none
--      keep it while a chain pill lies dead; it ends when the chain is
--      ours, when a take on another pill starts, or when the take is
--      dropped with no chain pill dead.
--   5. source checks: init.lua marks and sends take_blocker_tiles, drops
--      our own reposition on a marked pill; goals.lua copies blk on the
--      pool-6 row and the reposition pool skips _in_use; the ally vote
--      says no to an _in_use pill.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build-xc/_deps/luajit_src-src/src/luajit.exe test_pt2_blocker_mark.lua
-- Arguments:
--   old        C.PT2_BLOCKER_MARK = false (3 and 4 fail where the rule
--              changes the answer).
--   src=<dir>  run 5 on <dir>/*.lua (default: this directory).
-- =========================================================================

package.path = "./?.lua;" .. package.path

local OLD, SRC = false, "."
for _, a in ipairs(arg or {}) do
  if a == "old" then OLD = true end
  local d = a:match("^src=(.+)$")
  if d then SRC = d end
end

if math.atan2 then
  local _a1, _a2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _a1(y) else return _a2(y, x) end end
end
package.loaded["print2"] = setmetatable({ set_tick = function() end }, { __call = function() end })
_G.print2 = _G.print2 or function() end

local C   = require("constants")
local DEFAULT_ON = C.PT2_BLOCKER_MARK
if OLD then C.PT2_BLOCKER_MARK = false end
print(string.format("C.PT2_BLOCKER_MARK = %s", tostring(C.PT2_BLOCKER_MARK)))
local U   = require("util")
local cpf = require("cpathfinder")
local PC  = require("pill_cover")
local V2  = require("pt2_eval")
local D   = require("pt2_dbl")
local A   = require("attack")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

local function sq(mx, my) return my * 256 + mx end
U.ttype      = function() return C.T_GRASS end
U.ttype_peek = U.ttype
U.base_tile_stops_tank_shell = function() return false end
cpf.lgm_travel_ticks_map = function() return 10 end
C.PT2_C_EVAL = false
C.PT2_V2_C = false

local OURS = sq(121, 118)

-- ── 1. knobs ──────────────────────────────────────────────────────────────
print("1. knobs")
check("PT2_BLOCKER_MARK on by default", DEFAULT_ON == true, DEFAULT_ON)
check("PT2_BLOCKER_MARK off in MAST", C.PRESETS.mast.PT2_BLOCKER_MARK == false,
  C.PRESETS.mast.PT2_BLOCKER_MARK)

-- ── 2. the gap_w shadow plan ──────────────────────────────────────────────
print("2. gap_w shadow plan")
local function gap_world(with_ours, ours_hp)
  local w = { pills = {}, pill_at = {}, bases = {}, base_at = {} }
  local list = { { mx = 124, my = 118, health = 15, owner = "neutral" },
                 { mx = 126, my = 118, health = 15, owner = "neutral" } }
  if with_ours then list[3] = { mx = 121, my = 118, health = ours_hp or 15, owner = "friendly" } end
  for i, p in ipairs(list) do
    w.pills[i] = p
    w.pill_at[sq(p.mx, p.my)] = { { id = i, pill = p } }
  end
  return w
end

-- The shadow run pt2_dbl makes for pill 1 (A), from the tank at (112,118).
local function shadow(w, carried)
  local inp = V2.world_inp(w, C, U, PC, cpf, V2.knob_inp(C, {}), false)
  inp.A = w.pills[1]
  inp.radius = C.PPT_STANDOFF or 5.0
  inp.num_pill_blockers = math.min(carried or 0, C.PPT_PILL_BLOCKERS_MAX or 2)
  inp.carried_pills = carried or 0
  inp.armour = 40
  inp.tank_wx, inp.tank_wy = 112 * 256 + 128, 118 * 256 + 128
  inp.anger_of = function() return 0 end
  if inp.start_while_building then
    local x = require("live_physics").context({}, {})
    inp.pill_attack_ticks, inp.pill_attack_min_ticks = x.pill_attack_ticks, x.pill_attack_min_ticks
    inp.pill_angry_divisor, inp.pill_cooldown_ticks = x.pill_angry_divisor, x.pill_cooldown_ticks
    inp.tank_reload = x.reload
    inp.lgm_ticks = function() return 10 end
  end
  inp.score_fn = function(deg, mx, my, cx, cy)
    local dx, dy = cx - 112.5, cy - 118.5
    return math.sqrt(dx * dx + dy * dy)
  end
  local ids = {}
  for id, p in pairs(w.pills) do ids[p] = id end
  inp.ids = inp.ids or ids
  local res = V2.evaluate_group(inp)
  local e = { tick = 0, carried = carried or 0, trees = 40, ids = { 1 }, hp = { 15 },
              owner = { "neutral" }, pos = { { 124, 118 } },
              safe = false, kills = 0, have = 0, need = 0, pills = 0, walls = 0, trees_need = 0 }
  D._fill_entry(e, res, inp, 1)
  return e, res
end

local function has(list, v)
  for _, x in ipairs(list or {}) do if x == v then return true end end
  return false
end
local function str(list) return list and table.concat(list, ",") or "-" end

local W_OURS = gap_world(true)
local e_ours, res_ours = shadow(W_OURS)
check("with our pill: safe double take, no wall (arena t=15: walls=0)",
  e_ours.safe and e_ours.kills == 2 and e_ours.walls == 0,
  string.format("safe=%s kills=%d walls=%d", tostring(e_ours.safe), e_ours.kills, e_ours.walls))
check("with our pill: the entry names (121,118) as a blocker pill",
  has(e_ours.blk, OURS) and #e_ours.blk == 1, str(e_ours.blk))
local e_none = shadow(gap_world(false))
check("without it: two walls (arena t=303: walls=2) and no blocker pill",
  e_none.walls == 2 and e_none.blk == nil,
  string.format("walls=%d blk=%s", e_none.walls, str(e_none.blk)))
local e_carry = shadow(gap_world(false), 1)
local placed = nil
for _, s in ipairs((select(2, shadow(gap_world(false), 1))).best.plan or {}) do
  if s.kind == "pill" then placed = sq(s.mx, s.my) end
end
check("carried pill, no ally pill: the entry names the square the plan places it on",
  e_carry.pills >= 1 and placed and has(e_carry.blk, placed),
  string.format("pills=%d placed=%s blk=%s", e_carry.pills, tostring(placed), str(e_carry.blk)))

-- ── 3. pending plan ───────────────────────────────────────────────────────
print("3. pending plan")
local function row(id, cost, dbl)
  return { _p = 6, _id = id, cost = cost, _dbl = dbl }
end
local function st_with(cache, goal)
  return { cost_cache = cache, goal = goal or { kind = "capture_pill", reposition = true, target_id = 3 } }
end
local GW = gap_world(true)
local dbl = { mult = 0.6, ids = e_ours.ids, kills = 2, blk = e_ours.blk }
local st = st_with({ ["6:1"] = row(1, 123.2, dbl), ["6:2"] = row(2, 200),
                     ["14:3"] = { _p = 14, _id = 3, cost = 90.7 } })
local t = A.take_blocker_tiles(st, GW)
check("gap_w t=15: reposition goal, best pool-6 row is the double take: (121,118) marked",
  has(t, OURS), str(t))
st = st_with({ ["6:1"] = row(1, 123.2, dbl), ["6:2"] = row(2, 100) })
t = A.take_blocker_tiles(st, GW)
check("a cheaper pool-6 row with no double take: nothing marked", #t == 0, str(t))
st = st_with({ ["6:1"] = row(1, 123.2, { mult = 0.6, blk = e_ours.blk, skip = "blitz" }) })
t = A.take_blocker_tiles(st, GW)
check("a blitz-skipped double take: nothing marked", #t == 0, str(t))
local rj = row(1, 123.2, dbl); rj._reject = "ally_claim"
st = st_with({ ["6:1"] = rj })
t = A.take_blocker_tiles(st, GW)
check("a rejected row: nothing marked", #t == 0, str(t))
do
  local was = C.PT2_BLOCKER_MARK
  C.PT2_BLOCKER_MARK = false
  st = st_with({ ["6:1"] = row(1, 123.2, dbl) })
  t = A.take_blocker_tiles(st, GW)
  check("knob off: the pending plan marks nothing (old rule)", #t == 0, str(t))
  C.PT2_BLOCKER_MARK = was
end
-- the marks reach the pill the way init.lua sets _in_use
st = st_with({ ["6:1"] = row(1, 123.2, dbl) })
t = A.take_blocker_tiles(st, GW)
local util = {}
for _, ti in ipairs(t) do util[ti] = true end
for _, p in pairs(GW.pills) do p._in_use = util[sq(p.mx, p.my)] and true or nil end
check("our pill (121,118) is _in_use; the take pills are not",
  GW.pills[3]._in_use == true and not GW.pills[1]._in_use and not GW.pills[2]._in_use,
  tostring(GW.pills[3]._in_use))

-- ── 4. chain latch ────────────────────────────────────────────────────────
print("4. chain latch")
local CW = gap_world(true)
local b = res_ours.best
local AP = res_ours.A or CW.pills[1]
local sscan = V2.to_scan_result(res_ours, AP, { cx = b.cx, cy = b.cy, mx = math.floor(b.cx), my = math.floor(b.cy) })
local goal = { kind = "attack_pill", target_id = 1, mx = 124, my = 118, _shield_scan = sscan,
               _pt2 = { ids = { 1, 2 }, mode = "HOLD", step = 1 } }
local cs = { goal = goal, cost_cache = {} }
t = A.take_blocker_tiles(cs, CW)
check("attack_pill take: its own scan marks (121,118)", has(t, OURS), str(t))
-- first kill: the goal turns capture_pill on dead pill 1
CW.pills[1].health = 0
cs.goal = { kind = "capture_pill", target_id = 1, mx = 124, my = 118 }
t = A.take_blocker_tiles(cs, CW)
check("between the kills: capture_pill on chain pill 1 keeps the mark", has(t, OURS), str(t))
-- both dead, pill 2 picked up, goal none (diag_n t=843: reposition won here)
CW.pills[2].health = 0
CW.pills[2].owner = "friendly"; CW.pills[2].in_tank = true
cs.goal = { kind = "none", mx = 0, my = 0 }
t = A.take_blocker_tiles(cs, CW)
check("one chain pill still dead on the ground, goal none: mark kept", has(t, OURS), str(t))
CW.pills[1].owner = "friendly"; CW.pills[1].in_tank = true
t = A.take_blocker_tiles(cs, CW)
check("every chain pill ours: the mark ends", #t == 0 and cs._take_blk == nil, str(t))
-- a take on another pill ends the latch
local CW2 = gap_world(true)
CW2.pills[4] = { mx = 140, my = 110, health = 15, owner = "hostile" }
cs = { goal = goal, cost_cache = {} }
goal._shield_scan = sscan
A.take_blocker_tiles(cs, CW2)
CW2.pills[1].health = 0
cs.goal = { kind = "attack_pill", target_id = 4, mx = 140, my = 110 }
t = A.take_blocker_tiles(cs, CW2)
check("an attack_pill on a pill outside the chain: the mark ends", #t == 0, str(t))
-- the take dropped before any kill
local CW3 = gap_world(true)
cs = { goal = goal, cost_cache = {} }
A.take_blocker_tiles(cs, CW3)
cs.goal = { kind = "explore", mx = 0, my = 0 }
t = A.take_blocker_tiles(cs, CW3)
check("take dropped, no chain pill dead: the mark ends", #t == 0, str(t))
do
  local was = C.PT2_BLOCKER_MARK
  C.PT2_BLOCKER_MARK = false
  local CW4 = gap_world(true)
  cs = { goal = goal, cost_cache = {} }
  A.take_blocker_tiles(cs, CW4)
  CW4.pills[1].health = 0
  cs.goal = { kind = "capture_pill", target_id = 1, mx = 124, my = 118 }
  t = A.take_blocker_tiles(cs, CW4)
  check("knob off: capture_pill between the kills marks nothing (old rule)", #t == 0, str(t))
  C.PT2_BLOCKER_MARK = was
end

-- ── 5. source checks ──────────────────────────────────────────────────────
print("5. source checks")
local function read(name)
  local f = assert(io.open(SRC .. "/" .. name, "rb"))
  local s = f:read("*a"); f:close(); return s
end
local init_src, goals_src, vote_src = read("init.lua"), read("goals.lua"), read("reposition_vote.lua")
check("init.lua: the _in_use / pblk set is attack.take_blocker_tiles",
  init_src:find("local mine = attack.take_blocker_tiles(state, world)", 1, true) ~= nil)
check("init.lua: pblk sends state._blocker_tiles",
  init_src:find('("/info pblk " .. _pblk)', 1, true) ~= nil
  and init_src:find("state._blocker_tiles = mine", 1, true) ~= nil)
check("init.lua: our own reposition on a marked live pill drops",
  init_src:find('_g.kind == "capture_pill" and _g.reposition', 1, true) ~= nil
  and init_src:find('"own_blocker"', 1, true) ~= nil)
check("goals.lua: the pool-6 row keeps the plan's blocker tiles",
  goals_src:find("blk = _dbl.e.blk", 1, true) ~= nil)
check("goals.lua: the reposition pool skips an _in_use pill",
  goals_src:find("and not p.in_tank and not p._in_use then", 1, true) ~= nil)
check("reposition_vote.lua: an ally votes no on an _in_use pill",
  vote_src:find('if p and p._in_use then return true, "blocker" end', 1, true) ~= nil)

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
