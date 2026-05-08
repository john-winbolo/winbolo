-- =========================================================================
-- NewAutopilot/heap.lua — binary min-heap
-- =========================================================================

local M = {}

function M.new()    return { n = 0 }  end
function M.empty(h) return (h.n or #h) == 0 end

function M.push(h, node)
  if not h.n then h.n = #h end  -- recover from deserialization
  h.n = h.n + 1
  h[h.n] = node
  local i = h.n
  while i > 1 do
    local p = i >> 1
    if h[p].cost <= h[i].cost then break end
    h[i], h[p] = h[p], h[i]
    i = p
  end
end

function M.pop(h)
  if not h.n then h.n = #h end
  if h.n == 0 then return nil end
  local top = h[1]
  h[1]   = h[h.n]
  h[h.n] = nil
  h.n    = h.n - 1
  local i = 1
  while true do
    local l = i + i
    local r = l + 1
    local s = i
    if l <= h.n and h[l].cost < h[s].cost then s = l end
    if r <= h.n and h[r].cost < h[s].cost then s = r end
    if s == i then break end
    h[i], h[s] = h[s], h[i]
    i = s
  end
  return top
end

return M
