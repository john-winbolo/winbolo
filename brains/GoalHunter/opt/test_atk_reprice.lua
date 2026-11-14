-- =========================================================================
-- test_atk_reprice.lua — standalone unit tests (2026-11-03) for the
-- attack_pill event re-price (goals.note_reprice_events and
-- goals.apply_reprice_queue):
--   * a pill that dies re-prices the live pills in its crossfire reach, drops
--     their difficulty scans, asks for a fresh short slate and REJECTs the
--     dead pill's pool-6 entry
--   * a pill that changes owner does the same
--   * an enemy tank that dies within pill range + 2 of a pill re-prices that
--     pill only (the scan is kept: it does not read tanks)
--   * nothing changes: nothing happens
--   * the pending ids go to the head of the queue one tick later, and only
--     those entries
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_atk_reprice.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")
local G = require("goals")

_G.EVENT_TANK_KILLED = _G.EVENT_TANK_KILLED or 3

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

local function pill(mx, my, owner, hp)
  return { mx = mx, my = my, owner = owner, health = hp }
end

-- The 20261102 bot0 picture round t=50057: #4 dies beside #5, #13 is the
-- live pill 2-3 tiles away, #20 is a far pill that must not move.
local function make()
  local world = { pills = {
    [4]  = pill(141, 126, "hostile", 3),
    [9]  = pill(137, 126, "hostile", 8),
    [13] = pill(139, 127, "hostile", 15),
    [20] = pill(60, 60, "hostile", 15),
  } }
  local state = { tick = 100, cost_cache = {
    ["6:4"]  = { cost = 50, _p = 6, _id = 4 },
    ["6:13"] = { cost = 80, _p = 6, _id = 13 },
  }, _pill_diff_cache = {
    ["139:127:mid"] = { mx = 139, my = 127, score = 10 },
    ["137:126:mid"] = { mx = 137, my = 126, score = 10 },
    ["60:60:mid"]   = { mx = 60, my = 60, score = 10 },
  }, perc = { enemy_tanks = {} } }
  local info = { events = {} }
  return state, world, info
end

-- ── nothing changes ──────────────────────────────────────────────────────
do
  local state, world, info = make()
  G.note_reprice_events(state, world, info)   -- first tick: snapshot only
  state.tick = 101
  G.note_reprice_events(state, world, info)
  check("no change: nothing pending", state._atk_reprice == nil, state._atk_reprice)
  check("no change: no forced slate", state._dij_force_short == nil, state._dij_force_short)
end

-- ── a pill dies ──────────────────────────────────────────────────────────
do
  local state, world, info = make()
  G.note_reprice_events(state, world, info)
  state.tick = 101
  world.pills[4].health = 0
  G.note_reprice_events(state, world, info)
  local r = state._atk_reprice
  check("pill died: pending set", r ~= nil)
  check("pill died: #13 in reach", r and r.ids[13] == true)
  check("pill died: #9 in reach", r and r.ids[9] == true)
  check("pill died: far #20 untouched", r and r.ids[20] == nil)
  check("pill died: dead #4 not re-priced", r and r.ids[4] == nil)
  check("pill died: due next tick", r and r.due == 102, r and r.due)
  check("pill died: short slate forced this tick", state._dij_force_short == 101, state._dij_force_short)
  check("pill died: #4 pool-6 entry REJECT", state.cost_cache["6:4"]._reject == "pill_died"
        and state.cost_cache["6:4"].cost == 1e30, state.cost_cache["6:4"]._reject)
  check("pill died: #13 scan dropped", state._pill_diff_cache["139:127:mid"] == nil)
  check("pill died: #9 scan dropped", state._pill_diff_cache["137:126:mid"] == nil)
  check("pill died: far scan kept", state._pill_diff_cache["60:60:mid"] ~= nil)

  -- Queue: #13 and #20 sit late in a queue that is part done.
  state.eval_queue = {
    { pool = 1, id = 9 }, { pool = 6, id = 20, obj = world.pills[20] },
    { pool = 4, id = 5 }, { pool = 6, id = 13, obj = world.pills[13] },
    { pool = 6, id = 9, obj = world.pills[9] },
  }
  state.eval_queue_pos = 3
  G.apply_reprice_queue(state)
  check("queue: not before due tick", #state.eval_queue == 5, #state.eval_queue)
  state.tick = 102
  G.apply_reprice_queue(state)
  local q = state.eval_queue
  check("queue: two items added", #q == 7, #q)
  local a, b = q[3], q[4]
  check("queue: head items are the pending pool-6 ids",
        a.pool == 6 and b.pool == 6 and a._reprice and b._reprice
        and ((a.id == 13 and b.id == 9) or (a.id == 9 and b.id == 13)),
        tostring(a.id) .. "," .. tostring(b.id))
  check("queue: pos unchanged", state.eval_queue_pos == 3, state.eval_queue_pos)
  check("queue: far #20 not added", q[2].id == 20 and q[5].id == 5)
  local n_before = #q
  G.apply_reprice_queue(state)
  check("queue: applied only once", #state.eval_queue == n_before, #state.eval_queue)
end

-- ── a pill changes owner ─────────────────────────────────────────────────
do
  local state, world, info = make()
  G.note_reprice_events(state, world, info)
  state.tick = 101
  world.pills[4].owner = "friendly"
  G.note_reprice_events(state, world, info)
  local r = state._atk_reprice
  check("owner change: #13 pending", r and r.ids[13] == true)
  check("owner change: #4 REJECT", state.cost_cache["6:4"]._reject == "owner_hostile_to_friendly",
        state.cost_cache["6:4"]._reject)
end

-- ── a live pill changes owner hostile -> neutral: still attackable ───────
do
  local state, world, info = make()
  G.note_reprice_events(state, world, info)
  state.tick = 101
  world.pills[13].owner = "neutral"
  G.note_reprice_events(state, world, info)
  local r = state._atk_reprice
  check("still attackable: own id re-priced", r and r.ids[13] == true)
  check("still attackable: no REJECT", state.cost_cache["6:13"]._reject == nil)
end

-- ── an enemy tank dies near a pill ───────────────────────────────────────
do
  local state, world, info = make()
  state.perc.enemy_tanks = { { id = 2, mx = 137, my = 126 + 9 } }   -- 9 tiles below #9
  G.note_reprice_events(state, world, info)
  state.tick = 101
  state.perc.enemy_tanks = {}
  info.events = { { type = EVENT_TANK_KILLED, data = { 255, 2 } } }
  G.note_reprice_events(state, world, info)
  local r = state._atk_reprice
  check("tank died: #9 within 8+2", r and r.ids[9] == true)
  check("tank died: #13 within 8+2", r and r.ids[13] == true)  -- (139,127) to (137,135): 8.2
  check("tank died: far #20 untouched", r and r.ids[20] == nil)
  check("tank died: scans kept", state._pill_diff_cache["139:127:mid"] ~= nil)
  check("tank died: short slate forced", state._dij_force_short == 101)
end

-- ── an enemy tank dies far from every pill ───────────────────────────────
do
  local state, world, info = make()
  state.perc.enemy_tanks = { { id = 2, mx = 100, my = 100 } }
  G.note_reprice_events(state, world, info)
  state.tick = 101
  state.perc.enemy_tanks = {}
  info.events = { { type = EVENT_TANK_KILLED, data = { 255, 2 } } }
  G.note_reprice_events(state, world, info)
  check("far tank died: nothing pending", state._atk_reprice == nil)
end

-- ── data[1] is the killer, not the killed tank ───────────────────────────
do
  local state, world, info = make()
  state.perc.enemy_tanks = { { id = 2, mx = 137, my = 128 } }
  G.note_reprice_events(state, world, info)
  state.tick = 101
  info.events = { { type = EVENT_TANK_KILLED, data = { 2, 5 } } }   -- tank 2 killed player 5
  G.note_reprice_events(state, world, info)
  check("killer is not the dead tank", state._atk_reprice == nil)
end

-- ── masters: ATK_REPRICE_EVENTS and REPAIR_GIVE_WAY ───────────────────────
-- Default on (today's behaviour), PRESETS.keel off (the old behaviour). The
-- switch sits at the call sites in init.lua, so the check reads that file:
-- the re-price call and the give-way test are each reached only with the
-- master on, and the off branch of the re-price drops the snapshot.
do
  check("ATK_REPRICE_EVENTS default true", C.ATK_REPRICE_EVENTS == true, C.ATK_REPRICE_EVENTS)
  check("REPAIR_GIVE_WAY default true", C.REPAIR_GIVE_WAY == true, C.REPAIR_GIVE_WAY)
  local k = C.PRESETS and C.PRESETS.keel or {}
  check("keel ATK_REPRICE_EVENTS false", k.ATK_REPRICE_EVENTS == false, k.ATK_REPRICE_EVENTS)
  check("keel REPAIR_GIVE_WAY false", k.REPAIR_GIVE_WAY == false, k.REPAIR_GIVE_WAY)
  local f = io.open("init.lua", "rb")
  local src = f and f:read("*a") or ""
  if f then f:close() end
  local on, off = src:match("if C%.ATK_REPRICE_EVENTS then%s+(goals%.note_reprice_events%(state, world, info%))%s+else%s+(state%._atk_reprice_snap = nil)%s+end")
  check("init.lua: re-price only with ATK_REPRICE_EVENTS on", on ~= nil, on)
  check("init.lua: off drops the re-price snapshot", off ~= nil, off)
  local _, n = src:gsub("goals%.note_reprice_events%(", "")
  check("init.lua: one re-price call", n == 1, n)
  local gw = src:match("if goal_valid and C%.REPAIR_GIVE_WAY then%s+local why = goals%.repair_give_way_reason%(")
  check("init.lua: give-way only with REPAIR_GIVE_WAY on", gw ~= nil, gw)
  local _, m = src:gsub("goals%.repair_give_way_reason%(", "")
  check("init.lua: one give-way call", m == 1, m)
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
