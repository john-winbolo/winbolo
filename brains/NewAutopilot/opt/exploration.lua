-- =========================================================================
-- NewAutopilot/exploration.lua — frontier-based map coverage
-- =========================================================================

local C    = require("constants")
local U    = require("util")
local heap = require("heap")

local M = {}

function M.update(state, info)
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local k   = U.mkey(tmx, tmy)

  if not state.visited[k] then
    state.visited[k] = true
    for _, d in ipairs(C.DIRS8) do
      local nx, ny = tmx + d[1], tmy + d[2]
      if U.in_map(nx, ny) then
        local nk = U.mkey(nx, ny)
        if not state.visited[nk] and not state.frontier_set[nk] then
          local tt = U.ttype(nx, ny)
          local cost_table = info.inboat and C.TERRAIN_COST_BOAT or C.TERRAIN_COST_LAND
          local tc = cost_table[tt] or 9999
          if tc < 100 then
            state.frontier_set[nk] = true
            heap.push(state.frontier, {
              cost = U.hdist(tmx, tmy, nx, ny),
              mx   = nx,
              my   = ny,
            })
          end
        end
      end
    end
  end
end

-- Returns the closest unvisited frontier square, or nil, nil
function M.best_frontier(state)
  while not heap.empty(state.frontier) do
    local top = state.frontier[1]  -- peek
    local k   = U.mkey(top.mx, top.my)
    if state.visited[k] then
      heap.pop(state.frontier)
      state.frontier_set[k] = nil
    else
      return top.mx, top.my
    end
  end
  return nil, nil
end

return M
