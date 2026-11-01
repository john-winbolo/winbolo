-- =========================================================================
-- test_focus_adaptive.lua -- standalone unit tests (2026-11-01) for the
-- adaptive spike spot (focus_vote.lua, FOCUS_SPIKE_ADAPTIVE_SPOT):
--   * the switch margin (20% of the current score, at least 0.20)
--   * claims: the go claims, the fsk / fsc / fsp messages, release, take,
--     the lower player number keeps a base two bots took
--   * the spot score: every part, the pill and placeable rules
--   * the rescore: once per FOCUS_SPIKE_RESCORE_TICKS, the first spot, no
--     switch under the margin, a switch when a wall is built on the spot,
--     the end when every claimed base is covered, the release with no pill,
--     a free base taken by a bot with no claim left, the incomplete end
--   * the placed message (bug fix): a team bot's placed spot is marked
--     placed and tagged for care
-- No engine: terrain is a table.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_focus_adaptive.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C  = require("constants")
local U  = require("util")
local FV = require("focus_vote")

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
local terrain = {}
local function T(x, y) return terrain[y * 256 + x] or C.T_GRASS end
U.ttype = T
U.ttype_peek = T
U.is_placeable = function(x, y, world)
  local tt = T(x, y)
  if not (tt == C.T_GRASS or tt == C.T_ROAD or tt == C.T_FOREST) then return false end
  local k = y * C.MAP_W + x
  if world.pill_at[k] or world.base_at[k] then return false end
  return true
end
U.is_placeable_peek = U.is_placeable
print2 = print2 or function() end

C.FOCUS_SPIKE_ENABLED = true
C.FOCUS_SPIKE_RULE    = "cluster"
C.PILL_PLACE_TURTLE   = false
C.FOCUS_SPIKE_ADAPTIVE_SPOT = true
C.FOCUS_SPIKE_STDERR_LOG = false

local function new_world()
  return { bases = {}, pills = {}, pill_at = {}, base_at = {} }
end
local function add_base(w, id, mx, my, owner)
  w.bases[id] = { mx = mx, my = my, owner = owner }
  w.base_at[my * C.MAP_W + mx] = { id = id }
end
local function add_pill(w, id, mx, my, owner, hp)
  w.pills[id] = { mx = mx, my = my, owner = owner, health = hp or 15, in_tank = false }
  w.pill_at[my * C.MAP_W + mx] = { id = id }
end
local function near(a, b) return math.abs(a - b) < 1e-6 end

print("switch margin")
check("1.00 -> 1.15: no switch (margin 0.20)", not FV.should_switch(1.0, 1.15))
check("1.00 -> 1.25: switch", FV.should_switch(1.0, 1.25))
check("2.00 -> 2.35: no switch (20% of 2.00 = 0.40)", not FV.should_switch(2.0, 2.35))
check("2.00 -> 2.45: switch", FV.should_switch(2.0, 2.45))
check("0.10 -> 0.25: no switch (at least 0.20)", not FV.should_switch(0.1, 0.25))
check("-0.50 -> -0.25: switch", FV.should_switch(-0.5, -0.25))

print("claims and messages")
local w = new_world()
add_base(w, 0, 100, 100, "hostile")
add_base(w, 1, 106, 100, "neutral")
add_base(w, 2, 130, 100, "hostile")
local claims, order = FV.claims_of(w, { { mx = 103, my = 104, pn = 3 }, { mx = 128, my = 104, pn = 2 } }, { 2, 0, 1 })
check("first tile claims bases 0 and 1", claims[0] == 3 and claims[1] == 3, tostring(claims[0]) .. "," .. tostring(claims[1]))
check("second tile claims base 2", claims[2] == 2, claims[2])
check("claim order sorted", table.concat(order, ",") == "0,1,2", table.concat(order, ","))
local k = FV.parse("/info fsk 7 00030103")
check("fsk parse", k and k.verb == "fsk" and #k.claims == 2 and k.claims[2].b == 1 and k.claims[2].pn == 3)
local c = FV.parse("/info fsc 7 02FF")
check("fsc parse (release)", c and c.verb == "fsc" and c.claims[1].b == 2 and c.claims[1].pn == 255)
local p = FV.parse("/info fsp 7 6A64")
check("fsp parse", p and p.verb == "fsp" and p.tile.mx == 106 and p.tile.my == 100)
check("claims encode", FV.enc_claims({ { b = 0, pn = 3 }, { b = 11, pn = 255 } }) == "00030BFF")

local A = { claims = { [0] = 3, [1] = 3, [2] = 2 }, rel = {} }
check("only the owner releases", not FV.apply_claim(A, 0, 255, 2, 10) and A.claims[0] == 3)
check("owner releases", FV.apply_claim(A, 0, 255, 3, 10) and A.claims[0] == 255 and A.rel[0] == 10)
check("free base taken", FV.apply_claim(A, 0, 2, 2, 20) and A.claims[0] == 2 and A.rel[0] == nil)
check("higher player number cannot take it", not FV.apply_claim(A, 0, 3, 3, 21) and A.claims[0] == 2)
A.claims[1] = 3
check("lower player number keeps it", FV.apply_claim(A, 1, 2, 2, 22) and A.claims[1] == 2)
check("base not in the plan ignored", not FV.apply_claim(A, 9, 2, 2, 23))

print("spot score")
w = new_world()
add_base(w, 0, 100, 100, "hostile")
local left = { { id = 0, mx = 100, my = 100 } }
local list = FV.score_spots(w, {}, left, 100, 130, nil)
check("spots found", #list > 0, #list)
local all_in, sum_ok, sorted = true, true, true
for i, s in ipairs(list) do
  if math.sqrt((s.mx - 100) ^ 2 + (s.my - 100) ^ 2) > C.PILL_FIRE_RANGE then all_in = false end
  local q = s.parts
  if not near(s.total, q.payoff - (q.stand + q.route + q.travel + q.run)) then sum_ok = false end
  if i > 1 and list[i - 1].total < s.total - 1e-9 then sorted = false end
end
check("every spot within PILL_FIRE_RANGE of the base", all_in)
check("total = payoff - (stand + route + travel + run)", sum_ok)
check("best total first", sorted)
local b = list[1]
local mint = math.huge
for _, s in ipairs(list) do mint = math.min(mint, s.parts.travel) end
check("best spot has the least travel (travel counts per 10 tiles)", near(b.parts.travel, mint), b.parts.travel)
check("best spot on the tank side of the base", b.my > 100, b.my)
check("stand next to the spot, run 0.1", b.stand.d == 1 and near(b.parts.run, 0.1), b.parts.run)
check("travel = floor(dist / 10) x 0.5",
  near(b.parts.travel, math.floor(math.sqrt((b.stand.mx - 100) ^ 2 + (b.stand.my - 130) ^ 2) / 10) * 0.5),
  b.parts.travel)
check("payoff 1.0 for one base", near(b.parts.payoff, 1.0))
-- A live neutral pill rules out the spots in its range (knob off too).
local wn = new_world()
add_base(wn, 0, 100, 100, "hostile")
add_pill(wn, 1, 100, 110, "neutral", 15)
C.FOCUS_SPIKE_NEUTRAL_PILL_COVER = false
local ln = FV.score_spots(wn, {}, left, 100, 130, nil)
local none_in = true
for _, s in ipairs(ln) do
  if math.sqrt((s.mx - 100) ^ 2 + (s.my - 110) ^ 2) <= C.PILL_FIRE_RANGE then none_in = false end
end
check("no spot in a live neutral pill's range", none_in and #ln > 0, #ln)
-- A hostile pill that sees every stand place near the base: the stand risk
-- 2.0 x 15/15 per pill; two such pills = 4.0 > DEFENSE_MAX: no spot.
local wh = new_world()
add_base(wh, 0, 100, 100, "hostile")
add_pill(wh, 1, 100, 91, "hostile", 15)
local lh = FV.score_spots(wh, {}, left, 100, 130, nil)
local far_ok = true
for _, s in ipairs(lh) do
  if math.sqrt((s.mx - 100) ^ 2 + (s.my - 91) ^ 2) <= C.PILL_FIRE_RANGE then far_ok = false end
end
check("hostile pill range left out", far_ok)

print("rescore")
w = new_world()
add_base(w, 0, 100, 100, "hostile")
local outbox
local function mk(pn, claims0, carried)
  local state = { _fs = { cool_until = 0, seen = {}, route_cache = {} } }
  local A0 = { id = 7, adaptive = true, claims = claims0, rel = {}, tiles = {}, switches = {}, t_go = 0 }
  state._fs.active = A0
  local info = { player_number = pn, man_status = C.LGM_INTANK, carried_pills = carried,
                 tankx = 100 * 256, tanky = 130 * 256 }
  return state, A0, info
end
local state, A1, info = mk(2, { [0] = 2 }, 2)
check("step 0: go on", FV.adapt_step(state, state._fs, w, info, 0) == nil)
check("first spot chosen", A1.mine ~= nil and A1.switches[1] and A1.switches[1].why == "first",
  A1.switches[1] and A1.switches[1].why)
check("chosen spot in the tile list", A1.tiles[1] == A1.mine)
check("score kept for the viz: top 3", A1.score and #A1.score.top == 3, A1.score and #A1.score.top)
check("plan_stand answers the rescored stand", (function()
  local q = FV.plan_stand(state, w, info, A1.mine, 0)
  return q and q.mx == A1.mine.stand.mx and q.val == A1.mine.stand.val
end)())
local m0 = { mx = A1.mine.mx, my = A1.mine.my }
FV.adapt_step(state, state._fs, w, info, 20)
check("no rescore before FOCUS_SPIKE_RESCORE_TICKS", A1.score.t == 0, A1.score.t)
info.tankx = 104 * 256
FV.adapt_step(state, state._fs, w, info, 50)
check("rescored at 50", A1.score.t == 50, A1.score.t)
check("small gain: same spot (margin)", A1.mine.mx == m0.mx and A1.mine.my == m0.my and #A1.switches == 1,
  #A1.switches)
terrain[A1.mine.my * 256 + A1.mine.mx] = C.T_BUILDING
FV.adapt_step(state, state._fs, w, info, 100)
check("wall on the spot: switch", #A1.switches == 2 and A1.switches[2].why == "spot_blocked",
  A1.switches[2] and A1.switches[2].why)
check("new spot is not the walled one", not (A1.mine.mx == m0.mx and A1.mine.my == m0.my))
terrain = {}
-- Covered by a friendly pill outside the plan (bug fix): bases_covered.
add_pill(w, 5, 101, 104, "friendly", 15)
local r150 = FV.adapt_step(state, state._fs, w, info, 150)
check("bases covered by another pill: bases_covered", r150 == "bases_covered", r150)
w.pills[5], w.pill_at[104 * C.MAP_W + 101] = nil, nil
-- Covered by a placed spike spot of the plan: all_placed.
A1.mine.placed = true
local r200 = FV.adapt_step(state, state._fs, w, info, 200)
check("bases covered by the plan's spike pill: all_placed", r200 == "all_placed", r200)
A1.mine.placed = nil

-- No pill left: release with fsc.
state, A1, info = mk(2, { [0] = 2 }, 0)
FV.adapt_step(state, state._fs, w, info, 0)
check("no pill: base released", A1.claims[0] == 255 and A1.rel[0] == 0, A1.claims[0])
outbox = state._repo_outbox or {}
check("release sent", outbox[1] == "/info fsc 7 00FF", outbox[1])
-- Another bot with no claim left takes it.
local s2, A2, i2 = mk(3, { [0] = 255 }, 1)
A2.rel[0] = 0
FV.adapt_step(s2, s2._fs, w, i2, 10)
check("free base taken by player 3", A2.claims[0] == 3 and A2.mine ~= nil, A2.claims[0])
check("take sent", (s2._repo_outbox or {})[1] == "/info fsc 7 0003", (s2._repo_outbox or {})[1])
-- Free with no taker for FOCUS_SPIKE_RELEASE_WAIT: incomplete.
FV.adapt_step(state, state._fs, w, info, C.FOCUS_SPIKE_RELEASE_WAIT - 50)
check("free, still waiting", A1.claims[0] == 255)
check("free too long: incomplete", FV.adapt_step(state, state._fs, w, info, C.FOCUS_SPIKE_RELEASE_WAIT) == "incomplete")

print("placed message (bug fix)")
local tagged = nil
local A3 = { id = 9, tiles = { { mx = 50, my = 60, pn = 3 } } }
check("team bot's spot marked placed", FV.mark_placed(A3, 50, 60, 3, 400, function(x, y) tagged = x * 1000 + y end)
  and A3.tiles[1].placed and A3.tiles[1].t_placed == 400)
check("care tag on the placed spot", tagged == 50060, tagged)
check("second message changes nothing", not FV.mark_placed(A3, 50, 60, 3, 410, nil))
check("unknown spot ignored (fixed spots)", not FV.mark_placed(A3, 1, 1, 3, 410, nil))
A3.adaptive = true
check("adaptive: a placed spot of another bot is added", FV.mark_placed(A3, 70, 60, 2, 420, nil)
  and #A3.tiles == 2 and A3.tiles[2].placed)

-- Bug fix: comms.lua dropped fsk, fsc and fsp, so no teammate got them.
-- Every fs verb that focus_vote.lua sends must reach state._fs_rx.
print("every sent fs verb is accepted by comms")
local CM = require("comms")
local fh = assert(io.open("focus_vote.lua", "rb"))
local src = fh:read("*a")
fh:close()
local verbs, nv = {}, 0
for v in src:gmatch('send%(state, string%.format%("/info fs(%a) ') do
  if not verbs[v] then verbs[v], nv = true, nv + 1 end
end
check("send verbs found (o y n r g u a k c p)", nv == 10, nv)
local vl = {}
for v in pairs(verbs) do vl[#vl + 1] = v end
table.sort(vl)
for _, v in ipairs(vl) do
  local st = {}
  CM.process_message(3, "/info fs" .. v .. " 7 00", 100, st)
  check("comms accepts fs" .. v, st._fs_rx and #st._fs_rx == 1 and st._fs_rx[1].text == "/info fs" .. v .. " 7 00",
    st._fs_rx and #st._fs_rx)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
