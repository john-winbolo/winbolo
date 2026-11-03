-- =========================================================================
-- test_winners_parts.lua — standalone unit tests (2026-11-03) for the
-- WINNERS row of the pool panel (goals.get_pool_breakdown_json):
--   * the territory-influence x2.0 shows as its own part ("+173 INF") and
--     the history penalty ("+35 HT") is the amount charged, not "whatever is
--     left of the penalty"
--   * x0.5 shows as a negative part
--   * a builder-less multiplier shows as NB, and a penalty no named part
--     explains shows as OT, so the row always adds up
--   * the row carries the tick its pool row was priced and the tick
--     goal_selection made it; pool rows carry their own price tick
--   * an attack_pill re-price copy in the eval queue does not add a
--     second row for the same pill
-- No engine. Skipped in opt/ (the panel is debug-only and stripped there).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_winners_parts.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

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

do
  local f = io.open("goals.lua", "r")
  local src = f and f:read("*a") or ""
  if f then f:close() end
  if not src:find("[priced t=", 1, true) then
    print("opt/ copy: the WINNERS row builder is stripped, nothing to test")
    print(string.format("%d passed, %d failed", pass, fail))
    return
  end
end

_G.BRAIN_POOL_VIZ = true
_G.BRAIN_DEBUG_MODE = true

local function make(gc_extra, entry_cost)
  local pill = { mx = 139, my = 127, owner = "hostile", health = 15 }
  local state = {
    tick = 50070, phase = "middle", player_number = 0,
    goal = { kind = "refuel_at_base", mx = 1, my = 1 },
    cost_cache = { ["6:13"] = { cost = entry_cost or 173, tick = 50061, _p = 6, _id = 13,
                                _mx = 139, _my = 127, desc = "attack_pill#13" } },
    eval_queue = {
      { pool = 6, id = 13, obj = pill, _reprice = true },
      { pool = 6, id = 13, obj = pill },
    },
    pool_cache = { [6] = { cost = entry_cost or 173,
      goal = { kind = "attack_pill", mx = 139, my = 127, target_id = 13 } } },
    goal_competition = {},
  }
  local gc = { kind = "attack_pill", mx = 139, my = 127, base = entry_cost or 173,
    phase_weight = 1.0, inf_mult = 1.0, switch_flat = 0, commit_val = 0,
    hist_pen = 0, tick = 50062 }
  for k, v in pairs(gc_extra) do gc[k] = v end
  gc.penalty = gc.total - gc.base
  state.goal_competition[1] = gc
  return state
end

local function winners_formula(state)
  local ok, js = pcall(G.get_pool_breakdown_json, state)
  if not ok then return nil, js end
  -- The WINNERS row formula is the one that starts with the pool name and
  -- carries the tick tag.
  local f = js:match('"formula":"(attack_pill [^"]-%[priced t=[^"]-)"')
  return f, js
end

local function n_rows_for_13(js)
  local n = 0
  for _ in js:gmatch('"id":13,') do n = n + 1 end
  return n
end

-- x2.0 influence + 35 history: 173 x 2 + 35 = 381
do
  local st = make({ inf_mult = 2.0, hist_pen = 35, total = 381, shaped = 346 })
  local f, js = winners_formula(st)
  check("x2.0: row found", f ~= nil, js)
  f = f or ""
  check("x2.0: INF part", f:find("+173 INF", 1, true) ~= nil, f)
  check("x2.0: HT is the charged 35", f:find("+35 HT", 1, true) ~= nil, f)
  check("x2.0: no OT", f:find(" OT", 1, true) == nil, f)
  check("x2.0: total", f:find("=381", 1, true) ~= nil, f)
  check("x2.0: no x inf chip in the row", f:find("x inf@", 1, true) == nil, f)
  check("ticks: priced and picked", f:find("[priced t=50061, picked t=50062]", 1, true) ~= nil, f)
  check("pool row: price tick", js and js:find("[t=50061]", 1, true) ~= nil, js)
  check("pool row: one row for #13 (re-price copy skipped)", js and n_rows_for_13(js) == 1,
        js and n_rows_for_13(js))
end

-- x0.5 influence, no other penalty: 173 x 0.5 = 86.5
do
  local st = make({ inf_mult = 0.5, total = 86.5, shaped = 86.5 })
  local f = winners_formula(st) or ""
  check("x0.5: negative INF part", f:find("-86 INF", 1, true) ~= nil or f:find("-87 INF", 1, true) ~= nil, f)
  check("x0.5: total shown", f:find("=86", 1, true) ~= nil or f:find("=87", 1, true) ~= nil, f)
end

-- builder-less x3 after 30 switch: (173 + 30) x 3 = 609, NB = 203 x 2 = 406
do
  local st = make({ switch_flat = 30, nobuild_mult = 3.0, pre_nb = 203, total = 609, shaped = 173 })
  local f = winners_formula(st) or ""
  check("nobuild: SW", f:find("+30 SW", 1, true) ~= nil, f)
  check("nobuild: NB part", f:find("+406 NB", 1, true) ~= nil, f)
  check("nobuild: no HT", f:find("HT", 1, true) == nil, f)
end

-- an unexplained 12 shows as OT
do
  local st = make({ hist_pen = 35, total = 220, shaped = 173 })
  local f = winners_formula(st) or ""
  check("other: OT 12", f:find("+12 OT", 1, true) ~= nil, f)
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
