-- =========================================================================
-- GoalHunter/exploration.lua — frontier-based map coverage
-- =========================================================================
--
-- Frontier entries are encoded as plain integers to avoid per-push table
-- allocation and the GC pressure that causes periodic multi-ms pauses:
--   entry = cost * 65536 + my * 256 + mx
-- cost is Chebyshev distance (max 255 on 256x256), mx/my are tile coords.
-- The frontier is a binary min-heap of these integers.

local C    = require("constants")
local U    = require("util")

local M = {}

-- Min-heap helpers operating on plain integer entries (no table nodes).
local function _heap_push(h, v)
  local n = h.n + 1
  h.n = n
  h[n] = v
  local i = n
  while i > 1 do
    local p = i >> 1
    local hp = h[p]
    -- nil parent = the heap is out of sync (a hole below n). Stop sifting rather
    -- than crash on `nil <= number`; best_frontier heals the heap on next read.
    if hp == nil or hp <= h[i] then break end
    h[i], h[p] = h[p], h[i]
    i = p
  end
end

local function _heap_pop(h)
  local n = h.n
  if n == 0 then return nil end
  local top = h[1]
  h[1]   = h[n]
  h[n]   = nil
  h.n    = n - 1
  local i = 1
  n = h.n
  while true do
    local l = i + i
    local r = l + 1
    local s = i
    if l <= n and h[l] < h[s] then s = l end
    if r <= n and h[r] < h[s] then s = r end
    if s == i then break end
    h[i], h[s] = h[s], h[i]
    i = s
  end
  return top
end

function M.new_frontier()             return { n = 0 } end
function M.frontier_pop(h)            return _heap_pop(h) end
function M.frontier_push(h, cost, mx, my) _heap_push(h, math.floor(cost) * 65536 + my * 256 + mx) end

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
            local dist = math.floor(U.hdist(tmx, tmy, nx, ny))
            _heap_push(state.frontier, dist * 65536 + ny * 256 + nx)
          end
        end
      end
    end
  end
end

-- Returns the closest unvisited frontier tile, or nil, nil.
-- Soft danger avoidance: prefer a frontier tile that's OUT of pill/shell danger
-- (cpf_danger_at) so exploration doesn't march a tank into a pillbox's fire
-- range while it's between other goals. Dangerous tiles are set ASIDE (not
-- discarded — they're re-pushed before we return, so the frontier is intact) and
-- only used as a fallback if everything reachable is dangerous, so exploration
-- never stalls. Search is bounded by SKIP_LIMIT so a big danger field is cheap.
function M.best_frontier(state)
  local h = state.frontier
  if not h then return nil, nil end
  local thresh   = C.EXPLORE_DANGER_AVOID or 5
  local measure  = (cpf_danger_at ~= nil)
  local fb_x, fb_y                      -- nearest unvisited, danger notwithstanding
  local aside, n_aside = nil, 0         -- dangerous tiles popped this call
  local skipped, SKIP_LIMIT = 0, 48
  while h.n > 0 do
    local top = h[1]
    if top == nil then
      -- Heap desync: h.n is out of step with the backing array (h[1] missing
      -- while n>0). The push/pop logic can't produce this on its own, but a
      -- snapshot restore / replay can hand back a frontier whose count and
      -- array disagree. Rather than crash the whole think with arithmetic-on-nil,
      -- drop the corrupt heap to empty — M.update rebuilds the frontier next
      -- tick, and goals.pick_goal falls back to its nearest-unvisited scan
      -- (which re-pushes) for this one.
      h.n = 0
      break
    end
    local nx  = top % 256
    local ny  = math.floor(top / 256) % 256
    local nk  = U.mkey(nx, ny)
    if state.visited[nk] then
      _heap_pop(h)
      state.frontier_set[nk] = nil
    else
      -- Accept the first unvisited tile that's out of danger; otherwise set it
      -- aside (remembering the nearest as a fallback) and keep looking.
      local dng = measure and cpf_danger_at(nx, ny) or 0
      if dng <= thresh then
        if aside then for i = 1, n_aside do _heap_push(h, aside[i]) end end
        return nx, ny
      end
      if not fb_x then fb_x, fb_y = nx, ny end
      _heap_pop(h)                       -- frontier_set kept -> re-pushed below
      aside = aside or {}
      n_aside = n_aside + 1
      aside[n_aside] = top
      skipped = skipped + 1
      if skipped >= SKIP_LIMIT then break end
    end
  end
  if aside then for i = 1, n_aside do _heap_push(h, aside[i]) end end
  return fb_x, fb_y                       -- nearest (dangerous) fallback, or nil
end

return M
