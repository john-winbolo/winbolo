-- =========================================================================
-- test_human_intent.lua -- standalone unit tests (2026-11-19) for
-- HUMAN_PILL_YIELD (human_intent.lua + the human_claim reject in
-- goals.sync_ally_claimed_rejects). A scripted bot cannot be a human, so
-- these feed fake allied-human tanks (info.objects), fake shells and a fake
-- world.pills, tick by tick:
--   * close claim (within HUMAN_YIELD_CLOSE_TILES) at once, even stopped
--   * cone claim only after HUMAN_YIELD_HOLD_TICKS of moving + closing +
--     heading in the cone; not for a pill behind, or outside the radius
--   * one claim per human: the nearest pill in the cone
--   * a shell of the human near the pill: claim at once
--   * release: turned away for HUMAN_YIELD_AWAY_TICKS, then still claimed
--     for HUMAN_YIELD_POST_TICKS; stopped far; died; pill alive again;
--     not seen; hard limit
--   * two humans on one pill: the closer owns it
--   * a bot ally (player_bots) is not a human
--   * no yield: an enemy tank / hostile LGM clearly closer; an order on the
--     pill; the knob off
--   * goals.sync_ally_claimed_rejects: pool 4 and pool 5 rows get
--     _reject = "human_claim", and it clears after the claim
--   * drop_goal clears a capture_pill goal on the claimed pill
-- No engine.
--
-- Run from this directory:
--   ../../build-own-deps/luajit_src/src/luajit.exe test_human_intent.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

_G.OBJECT_TANK     = _G.OBJECT_TANK or 1
_G.OBJECT_SHOT     = _G.OBJECT_SHOT or 2
_G.OBJECT_BUILDMAN = _G.OBJECT_BUILDMAN or 3
_G.OBJECT_HOSTILE  = _G.OBJECT_HOSTILE or 1

local C  = require("constants")
local HI = require("human_intent")

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

local ME, HUM, HUM2, BOT2, FOE = 0, 1, 2, 3, 4
local function w(t) return t * 256 + 128 end     -- tile centre in world units

local function new_world(pills)
  local world = { pills = {} }
  for id, p in pairs(pills) do
    world.pills[id] = { mx = p[1], my = p[2], health = p[3] or 0 }
  end
  return world
end

-- info: tanks = { {pn, tx, ty, speed, hostile}, ... }, extra objects appended.
local function new_info(tanks, extra, bots)
  local objs = {}
  for _, t in ipairs(tanks) do
    objs[#objs + 1] = { type = OBJECT_TANK, idnum = t[1], x = math.floor(t[2] * 256),
                        y = math.floor(t[3] * 256), speed = t[4] or 0,
                        info = t[5] and OBJECT_HOSTILE or 0 }
  end
  for _, o in ipairs(extra or {}) do objs[#objs + 1] = o end
  return { player_number = ME,
           allies = bit.bor(bit.lshift(1, ME), bit.lshift(1, HUM), bit.lshift(1, HUM2),
                            bit.lshift(1, BOT2)),
           player_bots = bots or bit.bor(bit.lshift(1, ME), bit.lshift(1, BOT2)),
           objects = objs }
end

-- Drive a human from (x0,y0) by (vx,vy) tiles per tick for n ticks.
local function drive(state, world, t0, n, pn, x0, y0, vx, vy, extra_tanks)
  local x, y = x0, y0
  for i = 0, n - 1 do
    local tanks = { { pn, x, y, (vx ~= 0 or vy ~= 0) and 16 or 0 } }
    for _, e in ipairs(extra_tanks or {}) do tanks[#tanks + 1] = e end
    HI.update(state, world, new_info(tanks), t0 + i)
    x, y = x + vx, y + vy
  end
  return x, y, t0 + n
end

local HOLD  = C.HUMAN_YIELD_HOLD_TICKS
local AWAY  = C.HUMAN_YIELD_AWAY_TICKS
local POST  = C.HUMAN_YIELD_POST_TICKS
local STOPT = C.HUMAN_YIELD_STOP_TICKS
local LIMT  = C.HUMAN_YIELD_LIMIT_TICKS

check("knob default on", C.HUMAN_PILL_YIELD == true, C.HUMAN_PILL_YIELD)

-- ── close claim ─────────────────────────────────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 100, 100 } })
  HI.update(state, world, new_info({ { HUM, 102.5, 100.5, 0 } }), 10)
  local cl = HI.yield_claim(state, 7, 10)
  check("close: stopped human within 2.5 tiles claims at once", cl and cl.pn == HUM and cl.why == "close", cl and cl.why)
end

-- ── cone claim after hold ───────────────────────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 110, 100 }, [8] = { 90, 100 } })
  -- 0.1 tile/tick toward +x; pill 7 ahead (about 9.5 tiles), pill 8 behind.
  local x, y, t = drive(state, world, 0, HOLD, HUM, 100.5, 100.5, 0.1, 0)
  check("cone: no claim before the hold", HI.yield_claim(state, 7, t - 1) == nil)
  check("cone: no claim on the pill behind", HI.yield_claim(state, 8, t - 1) == nil)
  x, y, t = drive(state, world, t, 2, HUM, x, y, 0.1, 0)
  local cl = HI.yield_claim(state, 7, t - 1)
  check("cone: claim after the hold", cl and cl.why == "cone", cl and cl.why)
  -- Turn away: drive -x (pill behind) for AWAY ticks.
  x, y, t = drive(state, world, t, AWAY + C.HUMAN_YIELD_TRACK_TICKS, HUM, x, y, -0.1, 0)
  local post = HI.yield_claim(state, 7, t - 1)
  check("turned away: released, still claimed after release", post and post.post == true, post and tostring(post.post))
  x, y, t = drive(state, world, t, POST + 1, HUM, x, y, -0.1, 0)
  check("turned away: free after the post time", HI.yield_claim(state, 7, t - 1) == nil)
end

-- ── outside the radius ─────────────────────────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 130, 100 } })
  local _, _, t = drive(state, world, 0, HOLD + 5, HUM, 100.5, 100.5, 0.1, 0)
  check("radius: no claim on a pill 25 tiles off", HI.yield_claim(state, 7, t - 1) == nil)
end

-- ── one claim per human: nearest in the cone ───────────────────────────
do
  local state, world = {}, new_world({ [7] = { 106, 100 }, [8] = { 110, 101 } })
  local _, _, t = drive(state, world, 0, HOLD + 2, HUM, 100.5, 100.5, 0.1, 0)
  check("one claim: nearest pill claimed", HI.yield_claim(state, 7, t - 1) ~= nil)
  check("one claim: farther pill not claimed", HI.yield_claim(state, 8, t - 1) == nil)
end

-- ── shell near the pill ────────────────────────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 108, 100 } })
  local shell = { type = OBJECT_SHOT, owner = HUM, x = w(108), y = w(100), angle = 64, life = 4 }
  HI.update(state, world, new_info({ { HUM, 100.5, 100.5, 0 } }, { shell }), 5)
  local cl = HI.yield_claim(state, 7, 5)
  check("shot: the human's shell near the dead pill claims at once", cl and cl.why == "shot", cl and cl.why)
end

-- ── stopped far ────────────────────────────────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 110, 100 } })
  local x, y, t = drive(state, world, 0, HOLD + 2, HUM, 100.5, 100.5, 0.1, 0)
  check("stop: claimed first", HI.yield_claim(state, 7, t - 1) ~= nil)
  x, y, t = drive(state, world, t, STOPT - 5, HUM, x, y, 0, 0)
  local cl = HI.yield_claim(state, 7, t - 1)
  check("stop: still claimed before the stop time", cl and not cl.post)
  x, y, t = drive(state, world, t, 10, HUM, x, y, 0, 0)
  cl = HI.yield_claim(state, 7, t - 1)
  check("stop: released after 3 s stopped far", cl and cl.post == true)
end

-- ── hard limit, and the near exception ─────────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 111, 100 } })
  local x, y, t = drive(state, world, 0, HOLD + 2, HUM, 100.5, 100.5, 0.1, 0)
  -- Creep at 0.005 tile/tick: moving, still in the cone, never 4 tiles stop.
  x, y, t = drive(state, world, t, LIMT, HUM, x, y, 0.005, 0)
  local cl = HI.yield_claim(state, 7, t - 1)
  check("limit: released after 15 s when farther than 2 tiles", cl and cl.post == true, cl and tostring(cl.post))
  local s2, w2 = {}, new_world({ [7] = { 101, 100 } })
  local _, _, t2 = drive(s2, w2, 0, LIMT + 10, HUM, 102.5, 100.5, 0, 0)
  cl = HI.yield_claim(s2, 7, t2 - 1)
  check("limit: kept past 15 s within 2 tiles", cl and not cl.post)
end

-- ── died, pill alive, unseen ───────────────────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 101, 100 } })
  HI.update(state, world, new_info({ { HUM, 102.5, 100.5, 0 } }), 10)
  state.tank_dead_at = { [HUM] = 20 }
  HI.update(state, world, new_info({ { HUM, 102.5, 100.5, 0 } }), 20)
  local cl = HI.yield_claim(state, 7, 20)
  check("died: released (post) on the kill event", cl == nil or cl.post == true, cl and tostring(cl.post))

  local s2, w2 = {}, new_world({ [7] = { 101, 100 } })
  HI.update(s2, w2, new_info({ { HUM, 102.5, 100.5, 0 } }), 10)
  w2.pills[7].health = 5
  HI.update(s2, w2, new_info({ { HUM, 102.5, 100.5, 0 } }), 11)
  check("pill alive: no claim", HI.yield_claim(s2, 7, 11) == nil)

  local s3, w3 = {}, new_world({ [7] = { 101, 100 } })
  HI.update(s3, w3, new_info({ { HUM, 102.5, 100.5, 0 } }), 10)
  for t = 11, 10 + AWAY - 1 do HI.update(s3, w3, new_info({}), t) end
  cl = HI.yield_claim(s3, 7, 10 + AWAY - 1)
  check("unseen: kept for less than 1 s", cl and not cl.post)
  HI.update(s3, w3, new_info({}), 10 + AWAY + 1)
  cl = HI.yield_claim(s3, 7, 10 + AWAY + 1)
  check("unseen: released after 1 s", cl and cl.post == true)
end

-- ── two humans: the closer owns it ─────────────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 100, 100 } })
  HI.update(state, world, new_info({ { HUM, 102.4, 100.5, 0 }, { HUM2, 101.5, 100.5, 0 } }), 10)
  local cl = HI.yield_claim(state, 7, 10)
  check("two humans: the closer owns the pill", cl and cl.pn == HUM2, cl and cl.pn)
end

-- ── a bot ally is not a human ──────────────────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 100, 100 } })
  HI.update(state, world, new_info({ { BOT2, 101.5, 100.5, 0 } }), 10)
  check("bot ally: no claim", HI.yield_claim(state, 7, 10) == nil)
end

-- ── no yield: enemy closer, order, knob off ────────────────────────────
do
  local state, world = {}, new_world({ [7] = { 100, 100 } })
  HI.update(state, world, new_info({ { HUM, 102.5, 100.5, 0 }, { FOE, 100.5, 101.0, 0, true } }), 10)
  check("enemy tank much closer: no yield", HI.yield_claim(state, 7, 10) == nil)
  HI.update(state, world, new_info({ { HUM, 102.5, 100.5, 0 }, { FOE, 104.0, 100.5, 0, true } }), 11)
  check("enemy tank farther: yield", HI.yield_claim(state, 7, 11) ~= nil)
  local lgm = { type = OBJECT_BUILDMAN, idnum = FOE, x = w(100), y = w(100), info = OBJECT_HOSTILE }
  HI.update(state, world, new_info({ { HUM, 102.5, 100.5, 0 } }, { lgm }), 12)
  check("hostile LGM on the pill: no yield", HI.yield_claim(state, 7, 12) == nil)
  HI.update(state, world, new_info({ { HUM, 102.5, 100.5, 0 } }), 13)
  state._order = { tkind = "pill", tid = 7, kind = "capture_pill" }
  check("order on the pill: no yield", HI.yield_claim(state, 7, 13) == nil)
  state._order = nil
  C.HUMAN_PILL_YIELD = false
  check("knob off: no yield", HI.yield_claim(state, 7, 13) == nil)
  HI.update(state, world, new_info({ { HUM, 102.5, 100.5, 0 } }), 14)
  check("knob off: state dropped", state._hi == nil)
  C.HUMAN_PILL_YIELD = true
end

-- ── goals.sync_ally_claimed_rejects + drop_goal ────────────────────────
do
  local G = require("goals")
  local state, world = { tick = 30 }, new_world({ [7] = { 100, 100 } })
  local info = new_info({ { HUM, 102.5, 100.5, 0 } })
  HI.update(state, world, info, 30)
  state.cost_cache = {
    a = { _p = 4, _id = 7, _mx = 100, _my = 100, cost = 50 },
    b = { _p = 5, _id = 7, _mx = 100, _my = 100, cost = 60 },
    c = { _p = 4, _id = 9, _mx = 120, _my = 100, cost = 70 },
  }
  G.sync_ally_claimed_rejects(state, info)
  check("sync: pool 4 row rejected human_claim", state.cost_cache.a._reject == "human_claim", state.cost_cache.a._reject)
  check("sync: pool 5 row rejected human_claim", state.cost_cache.b._reject == "human_claim", state.cost_cache.b._reject)
  check("sync: other pill not rejected", state.cost_cache.c._reject == nil, state.cost_cache.c._reject)
  state.goal = { kind = "capture_pill", target_id = 7, mx = 100, my = 100 }
  check("drop_goal: drops the claimed pill's goal", HI.drop_goal(state, 30) == true and state.goal.kind == "none"
        and state._force_replan_reason == "human_claim")
  -- Human gone for good: post time, then the reject clears.
  for t = 31, 31 + AWAY + POST + 2 do HI.update(state, world, new_info({}), t) end
  state.tick = 31 + AWAY + POST + 2
  G.sync_ally_claimed_rejects(state, info)
  check("sync: reject clears after the release + post", state.cost_cache.a._reject == nil, state.cost_cache.a._reject)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
