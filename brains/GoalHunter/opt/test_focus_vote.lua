-- =========================================================================
-- test_focus_vote.lua — standalone unit tests (2026-11-01) for the focused
-- spike deny plan and its team vote (focus_vote.lua, C.FOCUS_SPIKE_RULE
-- "deny"):
--   * clusters on a Chew Toy 3 like map (four quadrants of four bases)
--   * the cover greedy (our pills count, hostile pill range, forest, nil)
--   * the plan table: early 33% fail, top half 50% pass, one quadrant 100%
--     pass, one base per quadrant 75% pass
--   * defense: a full pill line is skipped, the same line with forest beside
--     it is "deep, hidden route", one weak pill is allowed, an undefended
--     cluster goes first, the long path penalty, the tank term
--   * the vote: majority for 2, 3 and 4 bots, silence = yes, pledges,
--     assignments, a full open -> ballot -> go -> abort -> cooldown flow
--   * state A = KEEL: no passed vote, so spike.check is false, no share
--     switch, no spkd advert, the placement scan context is nil
-- No engine: terrain is a table, PP counts and team pills are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_focus_vote.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C      = require("constants")
local U      = require("util")
local PP     = require("pill_portfolio")
local SP     = require("spike")
local FV     = require("focus_vote")
local G      = require("goals")

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

-- ── Stand-ins ────────────────────────────────────────────────────────────
local terrain = {}          -- key y*256+x -> terrain; default grass
local function T(x, y) return terrain[y * 256 + x] or C.T_GRASS end
U.ttype = T
U.is_placeable = function(x, y, world)
  local tt = T(x, y)
  if not (tt == C.T_GRASS or tt == C.T_ROAD or tt == C.T_FOREST) then return false end
  local k = y * C.MAP_W + x
  if world.pill_at[k] or world.base_at[k] then return false end
  return true
end
-- focus_vote reads terrain through the peek versions (no change list).
U.ttype_peek = function(x, y) return U.ttype(x, y) end
U.is_placeable_peek = function(x, y, world) return U.is_placeable(x, y, world) end
local TEAM, FRONT = 10, 8   -- spare K = TEAM - FRONT
SP.team_pills = function() return TEAM, 0, 0, 0 end
PP.normal_targets = function() return { back = 0, front = FRONT, aggro = 0, util = 0 } end
PP.counts = function() return { back = 0, front = 0, aggro = 0, utility = 0 } end

C.FOCUS_SPIKE_ENABLED = true
C.FOCUS_SPIKE_RULE    = "deny"
C.PILL_PLACE_TURTLE   = false

local function new_world()
  return { bases = {}, pills = {}, pill_at = {}, base_at = {}, _bases_on_map = 0 }
end
local function add_base(w, id, mx, my, owner)
  w.bases[id] = { mx = mx, my = my, owner = owner }
  w.base_at[my * C.MAP_W + mx] = { id = id }
  w._bases_on_map = w._bases_on_map + 2 ^ id
end
local function add_pill(w, id, mx, my, owner, hp)
  w.pills[id] = { mx = mx, my = my, owner = owner, health = hp or 15, in_tank = false }
  w.pill_at[my * C.MAP_W + mx] = { id = id }
end
local function info_at(pn, mx, my, carried)
  return { player_number = pn, allies = 0, player_bots = 0,
           tankx = mx * 256 + 128, tanky = my * 256 + 128,
           carried_pills = carried or 0, man_status = C.LGM_INTANK, inboat = false,
           trees = 40 }
end
local function new_state(tick) return { tick = tick or 1000, perc = { enemy_tanks = {} } } end

-- Chew Toy 3 like: four quadrants, four bases each (a 2x2 grid 12 apart),
-- quadrant corners 100 apart. One tile covers a pair of bases 12 apart.
-- ids: NW 0-3, NE 4-7, SW 8-11, SE 12-15.
local QUAD = { NW = { 40, 40 }, NE = { 140, 40 }, SW = { 40, 140 }, SE = { 140, 140 } }
local QORDER = { "NW", "NE", "SW", "SE" }
local function chew(owner_of)
  local w = new_world()
  local id = 0
  for _, q in ipairs(QORDER) do
    local ox, oy = QUAD[q][1], QUAD[q][2]
    for j = 0, 3 do
      local mx, my = ox + (j % 2) * 12, oy + math.floor(j / 2) * 12
      add_base(w, id, mx, my, owner_of(q, j))
      id = id + 1
    end
  end
  return w
end

-- ── Clusters ─────────────────────────────────────────────────────────────
print("clusters")
do
  local w = chew(function() return "neutral" end)
  local list = FV.bases(w)
  local cl = FV.clusters(list, C.FOCUS_SPIKE_CLUSTER_GAP)
  check("four quadrants = four clusters", #cl == 4, #cl)
  local ok = true
  for _, c in ipairs(cl) do if #c.bases ~= 4 then ok = false end end
  check("four bases in each", ok)
  check("cluster 1 is NW (lowest base id first)", cl[1].bases[1].id == 0 and cl[1].cx == 46, cl[1].cx)
  local cl2 = FV.clusters(list, 11)
  check("gap 11 < 12: every base alone", #cl2 == 16, #cl2)
  -- Single link: a chain joins.
  local w2 = new_world()
  add_base(w2, 0, 10, 10, "neutral"); add_base(w2, 1, 25, 10, "neutral"); add_base(w2, 2, 40, 10, "neutral")
  local c3 = FV.clusters(FV.bases(w2), 16)
  check("chain 15 + 15 apart is one cluster (single link)", #c3 == 1, #c3)
  -- Positionless and unknown bases count in the total.
  local w3 = new_world()
  add_base(w3, 0, 10, 10, "hostile")
  w3.bases[1] = { mx = 0, my = 0, owner = "hostile" }
  w3._bases_on_map = 1 + 2 + 4   -- base 2 on the map, no record
  local l3, tot, nopos = FV.bases(w3)
  check("positioned list 1, total targets 3, 2 with no position", #l3 == 1 and tot == 3 and nopos == 2,
        #l3 .. "/" .. tot .. "/" .. nopos)
end

-- ── Cover greedy ─────────────────────────────────────────────────────────
print("cover greedy")
do
  local w = new_world()
  add_base(w, 0, 100, 100, "hostile"); add_base(w, 1, 106, 100, "hostile"); add_base(w, 2, 112, 100, "hostile")
  local tg = { w.bases[0], w.bases[1], w.bases[2] }
  local t = FV.cover(w, tg, {}, {}, 106, 130)
  check("three bases in a row 6 apart: one tile", t and #t == 1, t and #t)
  if t and t[1] then
    local okd = true
    for _, b in ipairs(tg) do
      if U.edist(t[1].mx, t[1].my, b.mx, b.my) > C.PILL_FIRE_RANGE then okd = false end
    end
    check("the tile is within 8 of all three", okd)
    check("nearest the anchor (south side)", t[1].my > 100, t[1].my)
  end
  -- Our live pill already covers base 0: 1 tile still covers 1 and 2.
  local t2 = FV.cover(w, tg, {}, { { mx = 100, my = 104 } }, 106, 130)
  check("our pill counts: still 1 tile for the other two", t2 and #t2 == 1)
  local t3 = FV.cover(w, tg, {}, { { mx = 100, my = 104 }, { mx = 109, my = 104 } }, 106, 130)
  check("all covered by our pills: 0 tiles", t3 and #t3 == 0, t3 and #t3)
  -- A hostile pill whose range takes every tile near base 2: uncoverable.
  local t4 = FV.cover(w, tg, { { mx = 112, my = 100 } }, {}, 106, 130)
  check("a hostile pill on base 2's ground: cannot cover", t4 == nil)
  -- Forest is no cover tile.
  for y = 80, 120 do for x = 80, 130 do terrain[y * 256 + x] = C.T_FOREST end end
  local t5 = FV.cover(w, tg, {}, {}, 106, 130)
  check("all forest: cannot cover", t5 == nil)
  terrain = {}
  U.ttype = function(x, y) return terrain[y * 256 + x] or C.T_GRASS end
  T = U.ttype
  -- Euclid inclusive at 8.
  local w6 = new_world()
  add_base(w6, 0, 100, 100, "hostile")
  local t6 = FV.cover(w6, { w6.bases[0] }, {}, {}, 100, 108)
  check("one base: the tile 8 away toward the anchor (inclusive)", t6 and t6[1].mx == 100 and t6[1].my == 108,
        t6 and (t6[1].mx .. "," .. t6[1].my))
end

-- ── The plan table ───────────────────────────────────────────────────────
print("deny plan")
local function plan_of(w, K, st)
  FRONT = TEAM - K
  st = st or new_state(1000)
  return FV.plan(st, w, info_at(0, 46, 146, 1), st.tick)
end
do
  -- Early: we hold SW (4), 12 targets, K = 2 -> one quadrant = 4/12 = 33%.
  local w = chew(function(q) return q == "SW" and "friendly" or "neutral" end)
  local P = plan_of(w, 2)
  check("early: 4 / 12 = 0.33 fails", not P.pass and P.why == "frac" and P.planned == 4 and P.total == 12,
        P.why .. " " .. P.planned .. "/" .. P.total)
  check("early: need 2 tiles for the quadrant", P.need == 2, P.need)
  -- Top half: they hold NW + NE (8), we hold the bottom half.
  local w2 = chew(function(q) return (q == "NW" or q == "NE") and "hostile" or "friendly" end)
  local P2 = plan_of(w2, 2)
  check("top half: 4 / 8 = 0.5 passes", P2.pass and P2.planned == 4 and P2.total == 8,
        P2.why .. " " .. P2.planned .. "/" .. P2.total)
  check("top half: the second quadrant does not fit K", #P2.chosen == 1)
  local P2b = plan_of(w2, 4)
  check("top half, K 4: both quadrants, 8 / 8", P2b.pass and P2b.planned == 8 and P2b.need == 4)
  -- One quadrant: they hold NE only.
  local w3 = chew(function(q) return q == "NE" and "hostile" or "friendly" end)
  local P3 = plan_of(w3, 2)
  check("one quadrant: 4 / 4 = 1.0 passes", P3.pass and P3.frac == 1, P3.frac)
  local P3b = plan_of(w3, 1)
  check("one quadrant, K 1: does not fit, fails", not P3b.pass and P3b.planned == 0)
  check("K 1: the row says no_spare", P3b.rows[1].skip == "no_spare", P3b.rows[1].skip)
  local P3c = plan_of(w3, 0)
  check("K 0: why no_spare", not P3c.pass and P3c.why == "no_spare", P3c.why)
  -- One base per quadrant: they hold base j=0 of each quadrant.
  local w4 = chew(function(q, j) return j == 0 and "hostile" or "friendly" end)
  local P4 = plan_of(w4, 3)
  check("1 per quadrant, K 3: 3 / 4 = 0.75 passes", P4.pass and P4.planned == 3 and P4.total == 4,
        P4.planned .. "/" .. P4.total)
  check("1 per quadrant: 3 tiles", P4.need == 3 and #P4.tiles == 3)
  check("plan line prints", FV.plan_line(1000, P4):find("FOCUS_SPIKE_PLAN t=1000 result=pass", 1, true) ~= nil)
  -- Neutral and unknown count as targets.
  local w5 = chew(function(q) return q == "SW" and "friendly" or "neutral" end)
  w5._bases_on_map = w5._bases_on_map + 2 ^ 16
  local P5 = plan_of(w5, 2)
  check("an unknown base (mask only) adds to the total: 13", P5.total == 13 and P5.nopos == 1, P5.total)
  -- Ally bases are ours.
  local w6 = chew(function(q) return q == "NE" and "hostile" or (q == "SW" and "allied" or "friendly") end)
  local P6 = plan_of(w6, 2)
  check("allied bases are ours: 4 targets", P6.total == 4, P6.total)
  -- Nothing to place.
  local w7 = chew(function(q) return q == "NE" and "hostile" or "friendly" end)
  add_pill(w7, 1, 146, 36, "friendly"); add_pill(w7, 2, 146, 56, "friendly")
  local P7 = plan_of(w7, 2)
  check("already covered by our pills: nothing_to_place", not P7.pass and P7.why == "nothing_to_place", P7.why)
end

-- ── Defense ──────────────────────────────────────────────────────────────
print("defense")
-- Target pair (96,40) + (108,40): one cover tile (102,45). Our base (102,120).
-- A sea band on row 80 with a gap x 86..118 and a line of three pills in it.
local function line_world(forest, hp, pills, g0, g1)
  terrain = {}
  g0, g1 = g0 or 86, g1 or 118
  for x = 0, 255 do
    if x < g0 or x > g1 then terrain[80 * 256 + x] = C.T_DEEPSEA end
  end
  if forest then
    for y = 66, 94 do for x = 84, 120 do
      if y ~= 80 or (x >= 86 and x <= 118) then terrain[y * 256 + x] = C.T_FOREST end
    end end
  end
  local w = new_world()
  add_base(w, 0, 96, 40, "hostile"); add_base(w, 1, 108, 40, "hostile")
  add_base(w, 2, 102, 120, "friendly")
  for i, x in ipairs(pills or { 94, 102, 110 }) do add_pill(w, 10 + i, x, 80, "hostile", hp or 15) end
  return w
end
do
  FRONT = TEAM - 2
  local w = line_world(false)
  local P = FV.plan(new_state(1000), w, info_at(0, 102, 120, 1), 1000)
  local r = P.rows[1]
  check("pill line: one target cluster", #P.rows == 2 or #P.rows == 1, #P.rows)
  for _, row in ipairs(P.rows) do if row.nb == 2 then r = row end end
  check("pill line: label deep", r.def.label == "deep", r.def.label)
  check("pill line: route >= 3 pills worth (2 x 2.0 or more)", r.def.route >= 4 - 1e-9, r.def.route)
  check("pill line: skipped as defended", r.skip == "defended" and not P.pass, r.skip)
  -- Same line, forest beside it.
  local wf = line_world(true)
  local Pf = FV.plan(new_state(1000), wf, info_at(0, 102, 120, 1), 1000)
  local rf
  for _, row in ipairs(Pf.rows) do if row.nb == 2 then rf = row end end
  check("forest strip: label hidden", rf.def.label == "hidden", rf.def.label)
  check("forest strip: route 0", rf.def.route == 0, rf.def.route)
  check("forest strip: not skipped, plan passes", rf.taken and Pf.pass, rf.skip)
  check("forest strip: log says deep, hidden route", FV.plan_line(1000, Pf):find("deep, hidden route", 1, true) ~= nil)
  -- One weak pill (3 hp) in the gap.
  local ww = line_world(false, 3, { 102 }, 96, 108)   -- a narrow gap: the pill sees all of it
  local Pw = FV.plan(new_state(1000), ww, info_at(0, 102, 120, 1), 1000)
  local rw
  for _, row in ipairs(Pw.rows) do if row.nb == 2 then rw = row end end
  check("one weak pill: route = 3/15 x 2.0 = 0.4", math.abs(rw.def.route - 0.4) < 1e-9, rw.def.route)
  check("one weak pill: allowed (taken)", rw.taken and Pw.pass, rw.skip)
  check("one weak pill: label deep", rw.def.label == "deep", rw.def.label)
  -- No forest, no pills: shallow.
  local wn = line_world(false, 15, {})
  local Pn = FV.plan(new_state(1000), wn, info_at(0, 102, 120, 1), 1000)
  local rn
  for _, row in ipairs(Pn.rows) do if row.nb == 2 then rn = row end end
  check("no pills: shallow, defense 0", rn.def.label == "shallow" and rn.def.total == 0, rn.def.total)
  -- Long path: a building wall across x 60..140 at row 90 with the gap far west.
  terrain = {}
  for x = 30, 200 do terrain[90 * 256 + x] = C.T_BUILDING end
  for y = 60, 118 do terrain[y * 256 + 141] = C.T_BUILDING end
  local wl = new_world()
  add_base(wl, 0, 96, 40, "hostile"); add_base(wl, 1, 108, 40, "hostile")
  add_base(wl, 2, 102, 120, "friendly")
  C.FOCUS_SPIKE_ROUTE_PAD = 80
  local stl = new_state(1000)
  local Pl = FV.plan(stl, wl, info_at(0, 102, 120, 1), 1000)
  C.FOCUS_SPIKE_ROUTE_PAD = 12
  local rl
  for _, row in ipairs(Pl.rows) do if row.nb == 2 then rl = row end end
  local extra = rl.def.plen - rl.def.straight
  check("long path: route goes round the wall (20+ tiles longer)", extra >= 20, extra)
  check("long path: detour = floor(extra / 10) x 0.5",
        math.abs(rl.def.detour - math.floor(extra / 10) * 0.5) < 1e-9 and rl.def.detour >= 1,
        rl.def.detour)
  -- Tanks seen near the cluster within the memory.
  terrain = {}
  local wt = line_world(false, 15, {})
  local stt = new_state(1000)
  local F = FV.st(stt)
  F.seen[1] = { mx = 100, my = 50, t = 900 }
  F.seen[2] = { mx = 100, my = 50, t = 300 }    -- too old (600 memory)
  F.seen[3] = { mx = 200, my = 200, t = 990 }   -- too far
  local Pt = FV.plan(stt, wt, info_at(0, 102, 120, 1), 1000)
  local rt
  for _, row in ipairs(Pt.rows) do if row.nb == 2 then rt = row end end
  check("tanks: one recent near tank = 0.5", math.abs(rt.def.tanks - 0.5) < 1e-9, rt.def.tanks)
  -- Cluster pills: a hostile pill 10 from a target base (8 + 2 margin), half armour.
  local wc = line_world(false, 15, {})
  add_pill(wc, 30, 86, 40, "hostile", 9)    -- 10 from (96,40)
  add_pill(wc, 31, 70, 40, "hostile", 15)   -- 26 away: not counted
  local Pc = FV.plan(new_state(1000), wc, info_at(0, 102, 120, 1), 1000)
  local rc
  for _, row in ipairs(Pc.rows) do if row.nb == 2 then rc = row end end
  check("cluster pill within 8 + 2: 9/15 = 0.6", rc and math.abs(rc.def.pills - 0.6) < 1e-9, rc and rc.def.pills)
end

-- Rank order.
print("rank order")
do
  local function row(id, nb, need, def, label, home)
    return { id = id, nb = nb, need = need, home = home or 10,
             def = { total = def, label = label or "shallow" },
             rank = nb / (need + C.FOCUS_SPIKE_DEFENSE_WEIGHT * def) }
  end
  local A = row(1, 2, 1, 0)          -- undefended, 2 per pill
  local B = row(2, 4, 2, 0.6)        -- bigger, defended, 2 per pill before defense
  check("undefended first: A before B", FV.before(A, B) and not FV.before(B, A))
  local B2 = row(3, 9, 2, 0.6)       -- rank 9 / 2.6 = 3.46 > 2
  check("undefended first even when B ranks higher", FV.before(A, B2))
  C.FOCUS_SPIKE_UNDEFENDED_FIRST = false
  check("knob off: the higher rank goes first", FV.before(B2, A))
  C.FOCUS_SPIKE_UNDEFENDED_FIRST = true
  local S1 = row(4, 4, 2, 0.6, "shallow")   -- 1.538
  local D1 = row(5, 4, 1.9, 0.6, "deep")    -- 1.6, within 10%
  check("within 10%: shallow before deep", FV.before(S1, D1))
  local D2 = row(6, 6, 1, 0.6, "deep")      -- 3.75
  check("more than 10% apart: the higher rank", FV.before(D2, S1))
  local N1 = row(7, 2, 1, 0, "shallow", 5)
  local N2 = row(8, 2, 1, 0, "shallow", 20)
  check("equal: nearer our bases first", FV.before(N1, N2) and not FV.before(N2, N1))
  -- In a plan: undefended NE pair over a defended bigger SE group.
  terrain = {}
  local w = new_world()
  add_base(w, 0, 140, 40, "hostile"); add_base(w, 1, 152, 40, "hostile")          -- NE pair
  add_base(w, 2, 140, 140, "hostile"); add_base(w, 3, 152, 140, "hostile")        -- SE four
  add_base(w, 4, 140, 152, "hostile"); add_base(w, 5, 152, 152, "hostile")
  add_base(w, 6, 40, 90, "friendly")
  add_pill(w, 20, 130, 146, "hostile", 9)   -- 10 from (140,146)? near SE bases
  FRONT = TEAM - 2
  local P = FV.plan(new_state(1000), w, info_at(0, 40, 90, 1), 1000)
  local ne, se
  for _, r in ipairs(P.rows) do if r.nb == 2 then ne = r elseif r.nb == 4 then se = r end end
  check("plan: SE defended (> 0.5)", se and se.def.total >= 0.5, se and se.def.total)
  check("plan: NE undefended taken first with K 2", ne and ne.taken and se and not se.taken,
        ne and tostring(ne.taken))
end

-- ── The vote ─────────────────────────────────────────────────────────────
print("vote")
do
  local function open_of(nbots, pledge, need, ballots)
    local bots = {}
    for i = 0, nbots - 1 do bots[#bots + 1] = i end
    return { me = 0, bots = bots, ballots = ballots or {}, pledge = pledge, need = need }
  end
  -- 2 bots.
  local ok = FV.resolve(open_of(2, 1, 1, {}), 2)
  check("2 bots: proposer + silence = 2 yes > 1: pass", ok)
  ok = FV.resolve(open_of(2, 1, 1, { [1] = { yes = false, reason = "not_target" } }), 2)
  check("2 bots: one no = 1 yes, not > 1: fail", not ok)
  -- 3 bots.
  local why
  ok = FV.resolve(open_of(3, 1, 1, { [1] = { yes = false, reason = "cooldown" } }), 3)
  check("3 bots: one no, 2 yes > 1.5: pass (majority, not all)", ok)
  ok, why = FV.resolve(open_of(3, 1, 1, { [1] = { yes = false, reason = "x" }, [2] = { yes = false, reason = "y" } }), 3)
  check("3 bots: two no: fail by majority", not ok and why == "majority", why)
  -- 4 bots.
  ok = FV.resolve(open_of(4, 1, 1, { [3] = { yes = false, reason = "x" } }), 4)
  check("4 bots: one no, 3 yes: pass", ok)
  ok = FV.resolve(open_of(4, 1, 1, { [2] = { yes = false, reason = "x" }, [3] = { yes = false, reason = "y" } }), 4)
  check("4 bots: two no, 2 yes not > 2: fail", not ok)
  -- Pledges.
  local pl
  ok, why, _, pl = FV.resolve(open_of(3, 1, 3, { [1] = { yes = true, pledge = 2, eta = 5 } }), 3)
  check("pledges: 1 + 2 = 3 >= need 3: pass", ok and pl == 3, pl)
  ok, why = FV.resolve(open_of(3, 1, 3, {}), 3)
  check("silence pledges 0: 1 < 3: fail by pledge", not ok and why == "pledge", why)
  ok, why = FV.resolve(open_of(2, 1, 1, { [1] = { yes = false, reason = "better" } }), 2)
  check("only 'better' no ballots: why better (no cooldown)", not ok and why == "better", why)
  -- Assignments: nearest ETA first, up to the pledge.
  local tiles = { { mx = 1, my = 1 }, { mx = 2, my = 2 }, { mx = 3, my = 3 } }
  local a = FV.assign(tiles, { { pn = 0, pledge = 1, eta = 20 }, { pn = 2, pledge = 2, eta = 5 } })
  check("assign: p2 (eta 5) takes 2 tiles, p0 the third",
        a and a[1].pn == 2 and a[2].pn == 2 and a[3].pn == 0)
  check("assign: pledges short -> nil",
        FV.assign(tiles, { { pn = 0, pledge = 1, eta = 1 } }) == nil)
  -- Team bots from the masks: humans do not vote.
  local inf = { player_number = 1, allies = 2 + 4 + 8, player_bots = 2 + 8 }
  local tb = FV.team_bots(inf)
  check("team bots: self + bot ally, human ally left out", #tb == 2 and tb[1] == 1 and tb[2] == 3,
        #tb)
  check("majority helper: 2 of 3 yes, 2 of 4 no", FV.majority(2, 3) and not FV.majority(2, 4))
  -- Wire round trip.
  local p = FV.parse("/info fso 65537 2 4 8 h 8C2498")
  check("parse fso", p and p.verb == "fso" and p.need == 2 and p.label == "hidden"
        and #p.tiles == 1 and p.tiles[1].mx == 0x8C and p.tiles[1].my == 0x24)
  local g = FV.parse("/info fsg 65537 8C240198300002")
  check("parse fsg", g and #g.tiles == 2 and g.tiles[1].pn == 1 and g.tiles[2].mx == 0x98)
  check("parse fsn", FV.parse("/info fsn 7 home_attacked").reason == "home_attacked")
  check("parse fsr", FV.parse("/info fsr 7 0 majority").pass == false)
end

-- ── The whole flow, two bots ─────────────────────────────────────────────
print("vote flow")
do
  terrain = {}
  local function team_world()
    return chew(function(q) return q == "NE" and "hostile" or "friendly" end)
  end
  FRONT = TEAM - 2
  local wA, wB = team_world(), team_world()
  local sA, sB = new_state(1000), new_state(1000)
  local iA = info_at(0, 140, 90, 2); iA.allies = 1 + 2; iA.player_bots = 1 + 2
  local iB = info_at(1, 160, 90, 1); iB.allies = 1 + 2; iB.player_bots = 1 + 2
  local function deliver(from_state, from_pn, to_state)
    local out = from_state._repo_outbox or {}
    from_state._repo_outbox = nil
    to_state._fs_rx = to_state._fs_rx or {}
    for _, m in ipairs(out) do to_state._fs_rx[#to_state._fs_rx + 1] = { from = from_pn, text = m } end
    return out
  end
  -- No vote while the placement goal did not win.
  sA.goal = { kind = "attack_tank" }
  FV.update(sA, wA, iA, 1000, function() end)
  check("plan passes but placement did not win: no open", sA._fs.open == nil and sA._fs.plan.pass)
  sA.goal = { kind = "place_pill_strategic" }
  FV.update(sA, wA, iA, 1060, function() end)
  check("win-then-vote: open", sA._fs.open ~= nil)
  local sent = deliver(sA, 0, sB)
  check("fso on the wire", #sent == 1 and sent[1]:match("^/info fso ") ~= nil, sent[1])
  FV.update(sB, wB, iB, 1061, function() end)
  local ballot = deliver(sB, 1, sA)
  check("B votes yes with pledge 1", #ballot == 1 and ballot[1]:match("^/info fsy %d+ 1 %d+$") ~= nil, ballot[1])
  FV.update(sA, wA, iA, 1062, function() end)
  check("all answered: resolved early, A active", sA._fs.open == nil and FV.active(sA))
  local go = deliver(sA, 0, sB)
  check("fsr pass + fsg on the wire", #go == 2 and go[1]:match("^/info fsr %d+ 1 ok$") ~= nil
        and go[2]:match("^/info fsg ") ~= nil, table.concat(go, " | "))
  FV.update(sB, wB, iB, 1063, function() end)
  check("B active after go", FV.active(sB))
  local tA, tB = FV.my_tiles(sA, iA), FV.my_tiles(sB, iB)
  check("2 tiles split: one each (pledges A 2, B 1; nearest first)",
        (tA and #tA or 0) + (tB and #tB or 0) == 2, (tA and #tA or 0) .. "+" .. (tB and #tB or 0))
  -- spike.check now gives the plan role to a bot with a tile and a pill.
  local chk = SP.check(sA, wA, iA, nil, true)
  check("spike.check: role plan while active", chk and chk.role == "plan" and #chk.plan >= 1)
  -- Placed: a friendly pill on a tile tags care, src plan.
  local tag_n = 0
  local any = sA._fs.active.tiles[1]
  add_pill(wA, 50, any.mx, any.my, "friendly", 15)
  FV.update(sA, wA, iA, 1100, function() tag_n = tag_n + 1 end)
  check("placed tile seen: one care tag", tag_n == 1 and any.placed)
  -- Abort: B sees a hostile pill covering the other tile.
  local other = sB._fs.active.tiles[2]
  if other.mx == any.mx and other.my == any.my then other = sB._fs.active.tiles[1] end
  add_pill(wB, 60, other.mx + 3, other.my, "hostile", 15)
  FV.update(sB, wB, iB, 1110, function() end)
  local ab = deliver(sB, 1, sA)
  check("B aborts: tile_covered", #ab == 1 and ab[1]:match("^/info fsa %d+ tile_covered$") ~= nil, ab[1])
  check("B off and in cooldown", not FV.active(sB) and sB._fs.cool_until == 1110 + C.FOCUS_SPIKE_VOTE_FAIL_COOLDOWN)
  FV.update(sA, wA, iA, 1111, function() end)
  check("A drops it on fsa, cooldown too", not FV.active(sA) and sA._fs.cool_until == 1111 + C.FOCUS_SPIKE_VOTE_FAIL_COOLDOWN)
  FV.update(sA, wA, iA, 1200, function() end)
  check("cooldown: no new open", sA._fs.open == nil)
  -- In cooldown a ballot says cooldown.
  local yn, why = FV.ballot(sA, wA, iA, { tiles = {}, planned = 1, total = 1 }, 1300)
  check("ballot in cooldown: no cooldown", yn == "no" and why == "cooldown", why)
  -- Home attacked ballot.
  local sC = new_state(1000)
  sC.perc.enemy_tanks = { { mx = 44, my = 144, id = 9 } }
  local yn2, why2 = FV.ballot(sC, team_world(), info_at(1, 0, 0, 1), { tiles = {}, planned = 1, total = 1 }, 1000)
  check("ballot: home_attacked", yn2 == "no" and why2 == "home_attacked", why2)
  -- not_target: every base near the tiles is ours in this view.
  local wo = chew(function() return "friendly" end)
  local yn3, why3 = FV.ballot(new_state(1000), wo, info_at(1, 0, 0, 1),
    { tiles = { { mx = 146, my = 36 } }, planned = 4, total = 4 }, 1000)
  check("ballot: not_target", yn3 == "no" and why3 == "not_target", why3)
  -- TTL end.
  local sT = new_state(1000)
  FV.st(sT).active = { id = 1, t_go = 0, from = 0, tiles = { { mx = 146, my = 36, pn = 0 } } }
  FV.update(sT, team_world(), info_at(0, 0, 0, 0), C.FOCUS_SPIKE_APPROVAL_TTL + 1, function() end)
  check("TTL: active ends", not FV.active(sT))
  -- A failed vote (majority) starts the cooldown; by "better" only it does not.
  local sF = new_state(1000)
  local iF = info_at(0, 140, 90, 2); iF.allies = 3; iF.player_bots = 3
  sF.goal = { kind = "place_pill_strategic" }
  FV.update(sF, team_world(), iF, 1000, function() end)
  sF._repo_outbox = nil
  sF._fs_rx = { { from = 1, text = string.format("/info fsn %d better", sF._fs.open.id) } }
  FV.update(sF, team_world(), iF, 1001, function() end)
  check("fail by better only: no cooldown", sF._fs.cool_until < 1001 and not FV.active(sF), sF._fs.cool_until)
  FV.update(sF, team_world(), iF, 1060, function() end)
  sF._fs_rx = { { from = 1, text = string.format("/info fsn %d tile_covered", sF._fs.open.id) } }
  FV.update(sF, team_world(), iF, 1061, function() end)
  check("fail by majority: cooldown", sF._fs.cool_until == 1061 + C.FOCUS_SPIKE_VOTE_FAIL_COOLDOWN, sF._fs.cool_until)
  -- Window: silence resolves after VOTE_WINDOW_TICKS.
  local sW = new_state(1000)
  local iW = info_at(0, 140, 90, 2); iW.allies = 3; iW.player_bots = 3
  sW.goal = { kind = "place_pill_strategic" }
  FV.update(sW, team_world(), iW, 1000, function() end)
  FV.update(sW, team_world(), iW, 1000 + C.FOCUS_SPIKE_VOTE_WINDOW_TICKS - 1, function() end)
  check("window: still open one tick before", sW._fs.open ~= nil)
  FV.update(sW, team_world(), iW, 1000 + C.FOCUS_SPIKE_VOTE_WINDOW_TICKS, function() end)
  check("window: silence = yes, passes (pledge 2 >= need 2)", sW._fs.open == nil and FV.active(sW))
end

-- ── State A = KEEL ───────────────────────────────────────────────────────
print("state A")
do
  local s = new_state(1000)
  local w = chew(function(q) return q == "NE" and "hostile" or "friendly" end)
  local inf = info_at(0, 140, 90, 2)
  check("no vote: spike.check false", SP.check(s, w, inf, nil, true) == false)
  check("no vote: focus_spike_ctx nil", G.focus_spike_ctx(w, SP.check(s, w, inf, nil, true), s, 140, 90) == nil)
  check("no vote: plan role ctx is nil too", G.focus_spike_ctx(w, { role = "plan", plan = {} }, s, 140, 90) == nil)
  local before = PP.targets_label()
  SP.update_win(s, w, inf, 1000)
  check("no vote: shares unchanged", PP.targets_label() == before and not PP.win_on and not PP.u2a_on)
  check("no vote: no spkd key", SP.advert(s, 1000) == nil)
  check("deny: no pair hold", SP.hold_row(s, inf, 140, 90) == nil)
  -- Active: shares switch on, team-wide.
  FV.st(s).active = { id = 1, t_go = 1000, from = 1, tiles = { { mx = 146, my = 36, pn = 1 } } }
  SP.update_win(s, w, inf, 1001)
  check("active: win shares and utility to aggro on", PP.win_on and PP.u2a_on)
  check("active, no tile of mine: spike.check false", SP.check(s, w, inf, nil, true) == false)
  FV.st(s).active = nil
  SP.update_win(s, w, inf, 1002)
  check("off again after the plan", not PP.win_on and not PP.u2a_on)
  -- KEEL preset: the vote never runs.
  check("PRESETS.keel: FOCUS_SPIKE_ENABLED false", C.PRESETS.keel.FOCUS_SPIKE_ENABLED == false)
  local kb = C.PRESETS.keel_focus_bench
  local same = true
  for k, v in pairs(C.PRESETS.keel) do
    if not (k:sub(1, 12) == "FOCUS_SPIKE_" or k == "SPIKE_REPAIR_PRIORITY") and kb[k] ~= v then same = false end
  end
  -- The plan reads terrain only through the peek versions. U.ttype records
  -- each tile it reads in the terrain change list, so a plan that used it
  -- would change play before any vote (bench seed 201 left KEEL at t=3599).
  do
    local real_tt, real_pl = U.ttype, U.is_placeable
    local n = 0
    U.ttype = function(x, y) n = n + 1; return real_tt(x, y) end
    U.is_placeable = function(x, y, w) n = n + 1; return real_pl(x, y, w) end
    U.ttype_peek = function(x, y) return real_tt(x, y) end
    U.is_placeable_peek = function(x, y, w) return real_pl(x, y, w) end
    FRONT = TEAM - 2
    local Pp = FV.plan(new_state(1000), line_world(false), info_at(0, 102, 120, 1), 1000)
    check("plan reads no terrain through U.ttype / U.is_placeable", n == 0 and #Pp.rows > 0, n)
    U.ttype, U.is_placeable = real_tt, real_pl
    U.ttype_peek = function(x, y) return U.ttype(x, y) end
    U.is_placeable_peek = function(x, y, w) return U.is_placeable(x, y, w) end
  end
  check("keel_focus_bench = keel + the focused spike values", same and kb.FOCUS_SPIKE_STDERR_LOG == true
        and kb.FOCUS_SPIKE_RULE == "deny" and kb.FOCUS_SPIKE_ENABLED == true
        and kb.REPAIR_FRIENDLY_FIRE_REJECT_TICKS == C.PRESETS.keel.REPAIR_FRIENDLY_FIRE_REJECT_TICKS)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
