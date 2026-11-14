-- =========================================================================
-- test_pool4_slate_inf.lua — standalone unit tests for the capture_pill
-- (pool 4) distance read in compute_pool4_cost, through the public
-- goals.kill_pickup_score:
--   * a fresh Dijkstra slate that has not reached the pill's neighbours yet
--     answers COST_INF (1e30) from dijkstra_cost_at, not math.huge. That is
--     "no value": the cost must come from the A* / old-slate fallbacks, not
--     from 1e30 taken as a distance (which scored 5e43 and dropped the row).
-- cpf.dijkstra_cost_at and cpf.cost_to_astar are stubbed. No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pool4_slate_inf.lua
-- or with any lua on PATH that has the bit library.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C      = require("constants")
local G      = require("goals")
local cpf    = require("cpathfinder")
local threat = require("threat")

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

local INF      = 1e30
local REBUILD  = 500
local PILL     = { mx = 130, my = 120, health = 0 }
local TMX, TMY = 124, 120

-- slate_costs[idx] = function(x, y, boat) -> cost; a missing slate is nil.
local slate_costs = {}
local astar_cost  = INF
local astar_calls = 0

local sv_cost_at, sv_astar = cpf.dijkstra_cost_at, cpf.cost_to_astar
local sv_at, sv_contrib    = threat.at, threat.pill_contrib
cpf.dijkstra_cost_at = function(idx, x, y, boat)
  local f = slate_costs[idx]
  if not f then return INF end
  return f(x, y, boat)
end
cpf.cost_to_astar = function()
  astar_calls = astar_calls + 1
  return astar_cost
end
threat.at = function() return 0 end
threat.pill_contrib = nil
threat.last_rebuild_tick = REBUILD

local function is_nb(x, y)
  return math.max(math.abs(x - PILL.mx), math.abs(y - PILL.my)) == 1
end
local function flat(v) return function() return v end end
local function nb_only(v) return function(x, y) return is_nb(x, y) and v or INF end end

-- state.dij.slates[idx].started_tick: >= REBUILD is fresh, below is old.
local function score(started)
  local slates = {}
  for idx, t in pairs(started) do slates[idx] = { started_tick = t } end
  local state = { dij = { slates = slates }, tick = REBUILD + 1,
                  perc = { enemy_tanks = {} } }
  local world = { pills = {}, bases = {} }
  local info  = { tankx = TMX * 256 + 128, tanky = TMY * 256 + 128,
                  inboat = false, shells = 40, trees = 0, mines = 0, armour = 40 }
  astar_calls = 0
  return G.kill_pickup_score(state, world, info, PILL)
end

print("compute_pool4_cost: fresh slate answers COST_INF at the neighbours")

-- Reference: the fresh slate has the neighbours at 6.
slate_costs = { [0] = nb_only(6) }
astar_cost  = INF
local ref6 = score({ [0] = REBUILD })
check("reference: fresh slate reached the neighbours -> finite", ref6 < 1e29, ref6)
check("reference: no A* call when the slate has a value", astar_calls == 0, astar_calls)

-- The bug: fresh slate is 1e30 everywhere, A* finds 6.
slate_costs = { [0] = flat(INF) }
astar_cost  = 6
local c = score({ [0] = REBUILD })
check("fresh slate 1e30, A* 6 -> finite", c < 1e29, c)
check("fresh slate 1e30, A* 6 -> A* was asked", astar_calls >= 1, astar_calls)
check("fresh slate 1e30, A* 6 -> same cost as a slate value of 6",
      math.abs(c - ref6) < 1e-6, c)

-- Fresh slate 1e30, both A* runs give up, an old slate holds 8.
slate_costs = { [0] = flat(INF), [2] = nb_only(8) }
astar_cost  = INF
local ref8_slate = { [0] = nb_only(8) }
c = score({ [0] = REBUILD, [2] = REBUILD - 50 })
slate_costs = ref8_slate
local ref8 = score({ [0] = REBUILD })
check("fresh slate 1e30, A* 1e30, old slate 8 -> finite", c < 1e29, c)
check("fresh slate 1e30, A* 1e30, old slate 8 -> same cost as a slate value of 8",
      math.abs(c - ref8) < 1e-6, c)

-- Nothing anywhere: the one sentinel, not 5e43.
slate_costs = { [0] = flat(INF) }
astar_cost  = INF
c = score({ [0] = REBUILD })
check("all 1e30 -> returns exactly 1e30", c == INF, c)

-- Only the boat layer of one neighbour has a value: still used.
slate_costs = { [0] = function(x, y, boat)
  return (boat == 1 and x == PILL.mx + 1 and y == PILL.my) and 6 or INF
end }
astar_cost  = INF
c = score({ [0] = REBUILD })
check("fresh slate, one boat-layer neighbour 6 -> same as reference",
      math.abs(c - ref6) < 1e-6, c)

cpf.dijkstra_cost_at, cpf.cost_to_astar = sv_cost_at, sv_astar
threat.at, threat.pill_contrib = sv_at, sv_contrib
threat.last_rebuild_tick = 0

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
