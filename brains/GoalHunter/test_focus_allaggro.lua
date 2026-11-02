-- =========================================================================
-- test_focus_allaggro.lua -- standalone unit tests (2026-11-01) for all
-- aggressive pills in a spike (focus_vote.lua, FOCUS_SPIKE_ALL_AGGRO):
--   * levelling: c(b), m and S; our pills, neutral pills, placed spots and
--     teammates' picks count; this bot's own pick and our bases do not
--   * the payoff: 1.0 per base of S a spot covers
--   * the level step: the pick, the fss message, the all_laid and
--     incomplete ends
--   * the reservation: reserved, may_place, team_carried
--   * the knob off: no picks, no levels, no reservation
-- No engine: terrain is a table.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_focus_allaggro.lua
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
C.FOCUS_SPIKE_ALL_AGGRO = true
C.FOCUS_SPIKE_NEUTRAL_PILL_COVER = false
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
local function plan(bases)
  return { id = 7, adaptive = true, claims = {}, rel = {}, tiles = {}, switches = {}, t_go = 0,
           picks = {}, t_pick = 0, bases = bases }
end

-- Three hostile bases on one row, 15 tiles apart; base 0 covered by our pill.
local w = new_world()
add_base(w, 0, 100, 100, "hostile")
add_base(w, 1, 115, 100, "hostile")
add_base(w, 2, 130, 100, "hostile")
add_pill(w, 5, 100, 104, "friendly", 15)

print("levels")
local A = plan({ 0, 1, 2 })
local L = FV.levels(w, A, 3)
check("c(b): base 0 = 1, bases 1 and 2 = 0", L.c[0] == 1 and L.c[1] == 0 and L.c[2] == 0,
  string.format("%s,%s,%s", tostring(L.c[0]), tostring(L.c[1]), tostring(L.c[2])))
check("m = 0", L.m == 0, L.m)
check("S = {1, 2}", table.concat(L.S, ",") == "1,2", table.concat(L.S, ","))
check("S list has the base places", #L.list == 2 and L.list[1].mx == 115 and L.list[2].mx == 130)
A.picks[4] = { mx = 115, my = 104, t = 0 }
L = FV.levels(w, A, 3)
check("a teammate's pick is pending cover: S = {2}", L.c[1] == 1 and table.concat(L.S, ",") == "2",
  table.concat(L.S, ","))
L = FV.levels(w, A, 4)
check("this bot's own pick does not count", L.c[1] == 0 and table.concat(L.S, ",") == "1,2",
  table.concat(L.S, ","))
A.tiles[1] = { mx = 130, my = 104, pn = 2, placed = true }
L = FV.levels(w, A, 3)
check("a placed spike spot counts: every c(b) = 1, m = 1", L.m == 1 and #L.S == 3, L.m)
A.tiles[2] = { mx = 101, my = 103, pn = 2 }
L = FV.levels(w, A, 3)
check("a spike spot not placed does not count", L.c[0] == 1, L.c[0])
A.tiles[3] = { mx = 100, my = 104, pn = 2, placed = true }
L = FV.levels(w, A, 3)
check("one place counts once (our pill and the placed spot)", L.c[0] == 1, L.c[0])
check("Euclidean range, inclusive: the spot at 8 tiles counts",
  (function()
    local A2 = plan({ 1 })
    A2.tiles[1] = { mx = 115, my = 108, placed = true }
    return FV.levels(w, A2, 3).c[1] == 1
  end)())
check("out of range at 8.06 tiles: no cover",
  (function()
    local A2 = plan({ 1 })
    A2.tiles[1] = { mx = 116, my = 108, placed = true }
    return FV.levels(w, A2, 3).c[1] == 0
  end)())
-- A base that is ours now is not a target.
local wo = new_world()
add_base(wo, 0, 100, 100, "hostile")
add_base(wo, 1, 115, 100, "friendly")
L = FV.levels(wo, plan({ 0, 1 }), 3)
check("a base that is ours is left out", #L.ids == 1 and L.ids[1] == 0, #L.ids)
-- A live neutral pill counts as cover with FOCUS_SPIKE_NEUTRAL_PILL_COVER.
local wn = new_world()
add_base(wn, 0, 100, 100, "hostile")
add_base(wn, 1, 115, 100, "hostile")
add_pill(wn, 6, 100, 104, "neutral", 15)
C.FOCUS_SPIKE_NEUTRAL_PILL_COVER = true
L = FV.levels(wn, plan({ 0, 1 }), 3)
check("neutral pill counts as cover (knob on)", L.c[0] == 1 and table.concat(L.S, ",") == "1",
  table.concat(L.S, ","))
C.FOCUS_SPIKE_NEUTRAL_PILL_COVER = false
L = FV.levels(wn, plan({ 0, 1 }), 3)
check("neutral pill does not count (knob off)", L.c[0] == 0 and #L.S == 2, #L.S)
-- No plan base list: the claimed bases.
local Ac = plan(nil)
Ac.claims = { [1] = 3, [2] = 255 }
L = FV.levels(w, Ac, 3)
check("no base list: the claimed bases", table.concat(L.ids, ",") == "1,2", table.concat(L.ids, ","))

print("payoff")
local S2 = { { id = 1, mx = 115, my = 100 }, { id = 2, mx = 130, my = 100 } }
local list = FV.score_spots(w, {}, S2, 122, 112, nil)
local two, one_ok = false, true
for _, c in ipairs(list) do
  local n = 0
  for _, b in ipairs(S2) do
    if math.sqrt((c.mx - b.mx) ^ 2 + (c.my - b.my) ^ 2) <= C.PILL_FIRE_RANGE then n = n + 1 end
  end
  if n == 2 and near(c.parts.payoff, 2 * C.FOCUS_SPIKE_BASE_PAYOFF) then two = true end
  if not near(c.parts.payoff, n * C.FOCUS_SPIKE_BASE_PAYOFF) then one_ok = false end
end
check("payoff = 1.0 per base of S the spot covers", one_ok)
check("a spot that covers both bases of S pays 2.0", two)
check("best spot covers both bases of S", list[1] and near(list[1].parts.payoff, 2.0),
  list[1] and list[1].parts.payoff)

print("level step")
local function mk(pn, carried, bases)
  local state = { _fs = { cool_until = 0, seen = {}, route_cache = {} } }
  local A0 = plan(bases)
  state._fs.active = A0
  local info = { player_number = pn, man_status = C.LGM_INTANK, carried_pills = carried,
                 tankx = 115 * 256, tanky = 112 * 256 }
  return state, A0, info
end
local state, A1, info = mk(3, 2, { 0, 1, 2 })
check("step 0: go on", FV.adapt_step(state, state._fs, w, info, 0) == nil)
check("levels kept for the viz", A1.level and A1.level.m == 0 and table.concat(A1.level.S, ",") == "1,2",
  A1.level and A1.level.m)
check("reserved pills per player N for the viz", A1.level.reserved[3] == 2 and A1.level.team == 2,
  A1.level.team)
check("a spot is picked", A1.mine ~= nil and A1.switches[1] and A1.switches[1].why == "first")
local covS = false
for _, b in ipairs(S2) do
  if math.sqrt((A1.mine.mx - b.mx) ^ 2 + (A1.mine.my - b.my) ^ 2) <= C.PILL_FIRE_RANGE then covS = true end
end
check("the picked spot covers a base of S", covS)
local ob = state._repo_outbox or {}
check("pick sent (fss)", ob[1] == string.format("/info fss 7 %02X%02X", A1.mine.mx, A1.mine.my), ob[1])
local fs = FV.parse(ob[1])
check("fss parse", fs and fs.verb == "fss" and fs.id == 7 and fs.tile.mx == A1.mine.mx)
local fn = FV.parse("/info fss 7 FFFF")
check("fss FFFF parse: no pick", fn and fn.verb == "fss" and fn.tile == nil)
check("no claims sent in this mode", #ob == 1, #ob)
-- A teammate picks the base this bot covers: the level moves and the bot follows.
local mine0 = { mx = A1.mine.mx, my = A1.mine.my }
A1.picks[4] = { mx = mine0.mx, my = mine0.my, t = 40 }
add_pill(w, 8, 130, 104, "friendly", 15)
add_pill(w, 9, 115, 104, "friendly", 15)
FV.adapt_step(state, state._fs, w, info, 50)
check("every base covered at 50 (pick + pills): m = 1, S = the bases with c(b) = 1",
  (function()
    local L5 = A1.level
    if L5.m ~= 1 then return false end
    for _, id in ipairs(L5.ids) do
      local inS = false
      for _, s in ipairs(L5.S) do if s == id then inS = true end end
      if inS ~= (L5.c[id] == 1) then return false end
    end
    return true
  end)(), A1.level.m)
w.pills[8], w.pill_at[104 * C.MAP_W + 130] = nil, nil
w.pills[9], w.pill_at[104 * C.MAP_W + 115] = nil, nil
A1.picks[4] = nil
-- The spot_eval log line (log only): written to the event log (errline)
-- and to print2 on each rescore, one line, with the stable key=value names.
-- (opt/ strips the print2 calls: the print2 check runs only in the source.)
do
  local P2 = require("print2")
  local mt = getmetatable(P2)
  local old_call, old_print, old_log = mt.__call, print, C.FOCUS_SPIKE_STDERR_LOG
  local got, out = {}, {}
  mt.__call = function(_, s) got[#got + 1] = s end
  print = function(s) out[#out + 1] = s end
  C.FOCUS_SPIKE_STDERR_LOG = true
  local se, Ae, ie = mk(3, 2, { 0, 1, 2 })
  FV.adapt_step(se, se._fs, w, ie, 0)
  mt.__call, print, C.FOCUS_SPIKE_STDERR_LOG = old_call, old_print, old_log
  local function find(t)
    local r = nil
    for _, s in ipairs(t) do
      if type(s) == "string" and s:match("^FOCUS_SPIKE spot_eval ") then r = s end
    end
    return r
  end
  local line, l2 = find(out), find(got)
  check("spot_eval line written to the event log", line ~= nil and Ae.eval_line == line, #out)
  check("spot_eval line written to print2 (source only)", #got == 0 or l2 == line, #got)
  check("spot_eval is one line", line and not line:find("\n", 1, true), line)
  local miss = {}
  for _, k in ipairs({ "t", "id", "tank", "arm", "carried", "builder", "m", "S", "c", "skip", "tiles",
                       "not_placeable", "found", "cut", "tested", "no_stand", "unreachable", "stand_risk",
                       "route_risk", "noreach", "unchecked", "other", "allowed", "risk_max", "chosen",
                       "chosen_p", "alt1", "alt1_p", "alt1_margin", "alt2", "alt2_p", "alt2_margin",
                       "switch_need", "rej", "rej_why", "rej_p", "change" }) do
    if not (line and line:find(" " .. k .. "=", 1, true)) then miss[#miss + 1] = k end
  end
  check("spot_eval has every key", #miss == 0, table.concat(miss, ","))
  check("spot_eval chosen parts", line and line:find("chosen_p=st:%d+,%d+|sd:%d+|sr:%d+|cov:") ~= nil
    and line:find("|pb:") ~= nil and line:find("|nm:") ~= nil and line:find("|boost:") ~= nil
    and line:find("|route:") ~= nil and line:find("|plen:") ~= nil and line:find("|rp:") ~= nil
    and line:find("|risk:") ~= nil and line:find("|total:") ~= nil, line)
  check("spot_eval first pick: change=first", line and line:find(" change=first ", 1, true) ~= nil, line)
end
-- Out of pills, no picks, one spike pill laid: all_laid.
local s5, A5, i5 = mk(3, 0, { 0, 1, 2 })
A5.tiles[1] = { mx = 115, my = 104, pn = 3, placed = true }
check("no reserved pill left, one laid: all_laid", FV.adapt_step(s5, s5._fs, w, i5, 0) == "all_laid")
local s6, A6, i6 = mk(3, 0, { 0, 1, 2 })
check("no reserved pill left, none laid: incomplete", FV.adapt_step(s6, s6._fs, w, i6, 0) == "incomplete")
-- A teammate still carries a pill (in this bot's world model): go on.
local wa = new_world()
add_base(wa, 1, 115, 100, "hostile")
wa.pills[20] = { mx = 0, my = 0, owner = "allied", in_tank = true, carrier = 4, health = 0 }
local s7, A7, i7 = mk(3, 0, { 1 })
check("team_carried counts the allies' carried pills",
  (function() local n, per = FV.team_carried(wa, i7); return n == 1 and per[4] == 1 end)())
check("an ally still carries a pill, its pick is pending: go on",
  (function() A7.picks[4] = { mx = 115, my = 104, t = 0 }; return FV.adapt_step(s7, s7._fs, wa, i7, 0) == nil end)())
-- Pills left but no bot has a pick for FOCUS_SPIKE_RELEASE_WAIT: incomplete.
-- This bot has no pill; only ally 4 (not a plan bot) carries one, so the
-- wait counts (see the bug fix test below for the builder-out case).
local s8, A8, i8 = mk(3, 0, { 1 })
A8.t_pick = 0
check("no pick, still waiting", FV.adapt_step(s8, s8._fs, wa, i8, C.FOCUS_SPIKE_RELEASE_WAIT - 50) == nil)
check("no pick for RELEASE_WAIT: incomplete",
  FV.adapt_step(s8, s8._fs, wa, i8, C.FOCUS_SPIKE_RELEASE_WAIT) == "incomplete")
-- Bug fix (2026-11-02): the builder of the carrier is out (it walks back
-- from a spike pill). The wait stops, so the plan goes on.
local s8b, A8b, i8b = mk(3, 1, { 1 })
i8b.man_status = C.LGM_MOVING
A8b.t_pick = 0
check("builder out with a pill: the wait stops",
  FV.adapt_step(s8b, s8b._fs, wa, i8b, C.FOCUS_SPIKE_RELEASE_WAIT * 3) == nil)
check("builder out with a pill: t_pick moves", A8b.t_pick == C.FOCUS_SPIKE_RELEASE_WAIT * 3, tostring(A8b.t_pick))
-- The pill runs out with a pick: the pick is cleared and sent as FFFF.
local s9, A9, i9 = mk(3, 1, { 0, 1, 2 })
FV.adapt_step(s9, s9._fs, w, i9, 0)
A9.picks[4] = { mx = 1, my = 1, t = 0 }   -- a teammate keeps the plan on
i9.carried_pills = 0
FV.adapt_step(s9, s9._fs, w, i9, 50)
check("no pill: pick cleared", A9.mine == nil)
check("no pill: FFFF sent", (s9._repo_outbox or {})[2] == "/info fss 7 FFFF", (s9._repo_outbox or {})[2])

print("reservation")
local sr, Ar, ir = mk(3, 2, { 0, 1, 2 })
FV.adapt_step(sr, sr._fs, w, ir, 0)
check("reserved while the plan is active", FV.reserved(sr))
check("may place on the picked spot", FV.may_place(sr, w, Ar.mine.mx, Ar.mine.my))
check("may not place a new pill anywhere else", not FV.may_place(sr, w, 60, 60))
check("may repair a pill of ours (one stands on the tile)", FV.may_place(sr, w, 100, 104))
local wh = new_world()
add_pill(wh, 1, 60, 60, "hostile", 0)
check("a hostile pill's tile is not a repair", not FV.may_place(sr, wh, 60, 60))
check("no plan: not reserved", not FV.reserved({ _fs = { cool_until = 0 } }))

print("knob off")
C.FOCUS_SPIKE_ALL_AGGRO = false
check("knob off: not reserved", not FV.reserved(sr))
check("knob off: may place anywhere", FV.may_place(sr, w, 60, 60))
local so = { _fs = { cool_until = 0, seen = {}, route_cache = {} } }
local Ao = { id = 7, adaptive = true, claims = { [1] = 3 }, rel = {}, tiles = {}, switches = {}, t_go = 0,
             bases = { 0, 1, 2 } }
so._fs.active = Ao
local io_ = { player_number = 3, man_status = C.LGM_INTANK, carried_pills = 2,
              tankx = 115 * 256, tanky = 112 * 256 }
FV.adapt_step(so, so._fs, w, io_, 0)
check("knob off: the claims step (no levels)", Ao.level == nil and Ao.mine ~= nil)
local sent_fss = false
for _, m in ipairs(so._repo_outbox or {}) do if m:match("^/info fss ") then sent_fss = true end end
check("knob off: no fss sent", not sent_fss)
C.FOCUS_SPIKE_ALL_AGGRO = true

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
