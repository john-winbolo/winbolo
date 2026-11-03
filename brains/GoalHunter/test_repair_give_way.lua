-- =========================================================================
-- test_repair_give_way.lua — standalone unit tests (2026-11-03) for the
-- repair_pill goal while it drives the tank to the pill:
--   * goals.repair_hold_while_out: the committed target keeps its pool-5
--     row while the builder is out of the tank (LGM_MOVING), and only that
--     pill, and only for a repair_pill goal
--   * goals.build_eval_queue puts that one row in pool 5 while the builder
--     is out, and none when the goal is something else
--   * goals.repair_give_way_reason: a blocked pill tile, or a pool-5 row
--     that is INF for a reason other than the builder_can split, makes the
--     goal give way; the builder_can split and a priced row do not
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_repair_give_way.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

-- The C threat grids are not loaded here: every lookup reads 0.
_G.gh_threat = _G.gh_threat or setmetatable({}, { __index = function()
  return function() return 0 end
end })

local C = require("constants")
local G = require("goals")

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

local function goal_on(mx, my, id)
  return { kind = "repair_pill", mx = mx, my = my, target_id = id }
end

-- ── repair_hold_while_out ────────────────────────────────────────────────
do
  local pill = { mx = 134, my = 111, owner = "friendly", health = 9 }
  local other = { mx = 120, my = 111, owner = "friendly", health = 9 }
  local state = { tick = 50153, goal = goal_on(134, 111, 7) }
  check("hold: target, builder out", G.repair_hold_while_out(pill, state, { man_status = C.LGM_MOVING }))
  check("hold: other pill not held", not G.repair_hold_while_out(other, state, { man_status = C.LGM_MOVING }))
  check("hold: builder aboard is the normal path", not G.repair_hold_while_out(pill, state, { man_status = C.LGM_INTANK }))
  check("hold: builder dead", not G.repair_hold_while_out(pill, state, { man_status = C.LGM_DEAD }))
  state.goal = { kind = "refuel_at_base", mx = 134, my = 111 }
  check("hold: other goal kind", not G.repair_hold_while_out(pill, state, { man_status = C.LGM_MOVING }))
end

-- ── build_eval_queue pool 5 while the builder is out ─────────────────────
local function pool5_ids(state, world, info)
  local ok, err = pcall(G.build_eval_queue, state, world, info)
  if not ok then return nil, err end
  local ids = {}
  for _, it in ipairs(state.eval_queue or {}) do
    if it.pool == 5 then ids[#ids + 1] = it.id end
  end
  return ids
end

do
  local function mk(goal, man, trees)
    local world = { pills = {
      [7] = { mx = 134, my = 111, owner = "friendly", health = 9, last_seen = 50150 },
      [8] = { mx = 120, my = 111, owner = "friendly", health = 9, last_seen = 50150 },
    }, bases = {}, tanks = {} }
    local state = { tick = 50153, goal = goal, cost_cache = {},
      perc = { friendly_pills_damaged = 2, attackable_pill_count = 0, neutral_base_count = 0, hostile_base_count = 0,
               enemy_tanks = {}, threat_at_tank = 0 } }
    local info = { tankx = 146 * 256, tanky = 121 * 256, man_status = man,
      trees = trees, shells = 40, mines = 0, armour = 40, player_number = 0 }
    return state, world, info
  end
  local s, w, i = mk(goal_on(134, 111, 7), C.LGM_MOVING, 0)
  local ids, err = pool5_ids(s, w, i)
  if not ids then
    check("queue: build_eval_queue runs", false, err)
  else
    check("queue: builder out keeps only the target", #ids == 1 and ids[1] == 7,
          table.concat(ids, ","))
    s, w, i = mk({ kind = "refuel_at_base", mx = 1, my = 1 }, C.LGM_MOVING, 0)
    ids = pool5_ids(s, w, i)
    check("queue: builder out, other goal: no pool 5", ids and #ids == 0,
          ids and table.concat(ids, ","))
    s, w, i = mk(goal_on(134, 111, 7), C.LGM_INTANK, 5)
    ids = pool5_ids(s, w, i)
    check("queue: builder aboard: both pills", ids and #ids == 2,
          ids and table.concat(ids, ","))
    s, w, i = mk(goal_on(134, 111, 7), C.LGM_INTANK, 0)
    ids = pool5_ids(s, w, i)
    check("queue: builder aboard, no wood: no pool 5 (as before)", ids and #ids == 0,
          ids and table.concat(ids, ","))
  end
end

-- ── repair_give_way_reason ───────────────────────────────────────────────
do
  local state = { tick = 1000, goal = goal_on(134, 111, 7), cost_cache = {} }
  check("give way: no row, no block: keep", G.repair_give_way_reason(state, 134, 111, 1000) == nil)
  state.cost_cache["5:7"] = { cost = 85 }
  check("give way: priced row: keep", G.repair_give_way_reason(state, 134, 111, 1000) == nil)
  state.cost_cache["5:7"] = { cost = 1e30, _skipped = "builder_can (leash 11, eta 44)" }
  check("give way: builder_can split: keep", G.repair_give_way_reason(state, 134, 111, 1000) == nil)
  state.cost_cache["5:7"] = { cost = 1e30 }
  local r = G.repair_give_way_reason(state, 134, 111, 1000)
  check("give way: INF row, no reason: no_path", r == "pool5_inf(no_path)", r)
  state.cost_cache["5:7"] = { cost = 1e30, _skipped = "ally_capturing (p3, 42t)" }
  r = G.repair_give_way_reason(state, 134, 111, 1000)
  check("give way: ally capturing", r == "pool5_inf(ally_capturing (p3, 42t))", r)
  state.cost_cache["5:7"] = { cost = 85 }
  state.blocked = { [111 * C.MAP_W + 134] = 1200 }
  r = G.repair_give_way_reason(state, 134, 111, 1000)
  check("give way: blocked tile", r == "blocked_until_1200", r)
  r = G.repair_give_way_reason(state, 134, 111, 1300)
  check("give way: block ran out: keep", r == nil, r)
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
