local bit = require('bitcompat')
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
--
-- Kill safety: the host's tick-budget hook (bot_manager.c brainBudgetHook)
-- raises a Lua error at ANY instruction when think() runs out of time, and the
-- same Lua state then runs think() again next tick. So a push or pop can stop
-- half-way. The writes below are ordered so that a stop at any point leaves
-- every slot 1..h.n holding a number, in heap order, and loses no entry: at
-- worst one entry is there twice. The old order (h.n before h[n] in
-- push; h[n]=nil before h.n in pop) could leave a nil at h[h.n], and the next
-- pop then moved that nil to h[1] and crashed with "attempt to compare number
-- with nil".
--
-- No loss: a sift moves one entry along a path. That entry is kept in h.hold
-- while it is out of the array, and h.hold is cleared only after it is back in
-- a slot. If a stop leaves h.hold set, the next heap call pushes it back
-- (_heap_heal). A duplicate entry is harmless: best_frontier drops it as
-- visited, like any other stale entry.
--
-- Desync guard: every read in the sift loops is nil-checked. A nil inside
-- 1..h.n (from an old stop, or any other cause) makes the heap rebuild itself
-- from its non-nil entries. On a valid heap the checks cost one compare each
-- and the array after every push/pop is exactly as before.

-- Move v down from slot i until heap order holds. On entry v is already in
-- h.hold and slot i holds something no larger than its children (a stale copy
-- is fine), so the array is a valid heap at every instruction: each level
-- copies the smaller child up into slot i, and v is written once at the end.
-- Same choices as the original swap loop (left child first, strict <).
-- Returns false if it meets a nil (heap desync).
local function _sift_hole_down(h, i, n, v)
  local ok = true
  while true do
    local l = i + i
    if l > n then break end
    local lv = h[l]
    if lv == nil then ok = false; break end
    local s, sv = i, v
    if lv < sv then s = l; sv = lv end
    local r = l + 1
    if r <= n then
      local rv = h[r]
      if rv == nil then ok = false; break end
      if rv < sv then s = r; sv = rv end
    end
    if s == i then break end
    h[i] = sv                            -- child moves up (now in two slots)
    i = s
  end
  h[i] = v
  h.hold = nil
  return ok
end

-- Move v up from slot i until heap order holds. On entry v is already in
-- h.hold and slot i is inside 1..h.n holding something no smaller than its
-- parent. Same choices as the original swap loop (stop on parent <= v).
-- Returns false if it meets a nil parent (heap desync).
local function _sift_hole_up(h, i, v)
  local ok = true
  while i > 1 do
    local p = bit.rshift(i, 1)
    local hp = h[p]
    if hp == nil then ok = false; break end
    if hp <= v then break end
    h[i] = hp                            -- parent moves down (now in two slots)
    i = p
  end
  h[i] = v
  h.hold = nil
  return ok
end

-- Restore heap order below slot i (used by the rebuild).
local function _sift_down(h, i, n)
  if i + i > n then return true end
  local v = h[i]
  if v == nil then return false end
  h.hold = v
  return _sift_hole_down(h, i, n, v)
end

-- Heal a desynced heap: keep every non-nil entry in 1..h.n (and h.hold), close
-- the gaps, set h.n to the new count, then re-heapify. Only runs on a desync.
local function _heap_rebuild(h)
  local n = h.n or 0
  local m = 0
  for i = 1, n do
    local v = h[i]
    if v ~= nil then
      m = m + 1
      h[m] = v
    end
  end
  local x = h.hold
  if x ~= nil then m = m + 1; h[m] = x end
  h.n = m                                -- count first: tail slots go stale, not holes
  h.hold = nil
  for i = m + 1, n do h[i] = nil end
  for i = bit.rshift(m, 1), 1, -1 do _sift_down(h, i, m) end
end

-- Push with no heal. v sits in h.hold until it is in its final slot. Slot n
-- first gets v or a copy of its parent, and only then is h.n raised, so 1..h.n
-- is a valid heap at every instruction.
local function _push_raw(h, v)
  h.hold = v
  local n = h.n + 1
  local p = bit.rshift(n, 1)
  local hp = (n > 1) and h[p] or nil
  if n > 1 and hp ~= nil and hp > v then
    h[n] = hp                            -- parent copy first, then count
    h.n = n
    if not _sift_hole_up(h, p, v) then _heap_rebuild(h) end
    return
  end
  h[n] = v                               -- slot first, then count
  h.n = n
  h.hold = nil
  if n > 1 and hp == nil then _heap_rebuild(h) end   -- hole below n: desync
end

-- A stop inside an earlier push/pop can leave its moving entry only in
-- h.hold. Put it back. On a heap that was never stopped h.hold is nil and this
-- does nothing.
local function _heap_heal(h)
  local x = h.hold
  if x ~= nil then _push_raw(h, x) end
end

local function _heap_push(h, v)
  _heap_heal(h)
  _push_raw(h, v)
end

-- Remove the entry in slot i of a heap whose slot n is a number. The last
-- entry goes to h.hold, the count drops, its old slot is cleared, then it
-- fills the gap at i by a sift. Until the sift writes slot i the removed entry
-- is still there, so a stop before that removes nothing and loses nothing.
local function _remove_slot(h, i, n, last)
  h.hold = last
  h.n = n - 1
  h[n] = nil
  if i == n then h.hold = nil; return true end
  n = n - 1
  if i > 1 then
    local hp = h[bit.rshift(i, 1)]
    if hp == nil then h[i] = last; h.hold = nil; return false end
    if hp > last then return _sift_hole_up(h, i, last) end
  end
  return _sift_hole_down(h, i, n, last)
end

local function _heap_pop(h)
  _heap_heal(h)
  local n = h.n
  if n <= 0 then return nil end
  local top  = h[1]
  local last = h[n]
  if top == nil or last == nil then      -- heap desync: heal, then pop
    _heap_rebuild(h)
    n = h.n
    if n == 0 then return nil end
    top  = h[1]
    last = h[n]
  end
  if not _remove_slot(h, 1, n, last) then _heap_rebuild(h) end
  return top
end

-- Remove the entry in slot i. Returns false if the heap had to be rebuilt
-- first (then nothing was removed).
local function _heap_remove_at(h, i)
  local n = h.n
  local last = h[n]
  if last == nil then _heap_rebuild(h); return false end
  if not _remove_slot(h, i, n, last) then _heap_rebuild(h) end
  return true
end

-- Push every set-aside entry back, in the order they were set aside (the
-- same push order as the old local list). a.done counts the entries already
-- pushed, so a stop part way pushes at most one entry twice next time.
local function _aside_flush(h, a)
  local n = a.n
  for i = (a.done or 0) + 1, n do
    _heap_push(h, a[i])
    a.done = i
  end
  a.n = 0
  a.done = 0
end

function M.new_frontier()             return { n = 0 } end
function M.frontier_pop(h)            return _heap_pop(h) end
function M.frontier_push(h, cost, mx, my) _heap_push(h, math.floor(cost) * 65536 + my * 256 + mx) end

-- Remove the entry for tile (mx, my), whatever its cost. Used when the caller
-- drops the tile best_frontier just returned. best_frontier may have pushed
-- set-aside danger tiles back after it chose, so the tile is not always at
-- h[1]. When it is at h[1] this is exactly frontier_pop. Returns true if an
-- entry was removed.
function M.frontier_remove(h, mx, my)
  local key = my * 256 + mx
  for _ = 1, 2 do                        -- second pass only after a rebuild
    _heap_heal(h)
    local n = h.n
    local hole = false
    for i = 1, n do
      local v = h[i]
      if v == nil then hole = true; break end
      if v % 65536 == key then
        if i == 1 then _heap_pop(h); return true end
        if _heap_remove_at(h, i) then return true end
        hole = true; break               -- rebuilt: look again
      end
    end
    if not hole then return false end
    _heap_rebuild(h)
  end
  return false
end

function M.update(state, info)
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local k   = U.mkey(tmx, tmy)

  if not state.visited[k] then
    -- visited[k] is set after the loop and frontier_set[nk] after the push, so
    -- a stop part way runs the loop again next tick instead of losing tiles.
    for _, d in ipairs(C.DIRS8) do
      local nx, ny = tmx + d[1], tmy + d[2]
      if U.in_map(nx, ny) then
        local nk = U.mkey(nx, ny)
        if not state.visited[nk] and not state.frontier_set[nk] then
          local tt = U.ttype(nx, ny)
          local cost_table = info.inboat and C.TERRAIN_COST_BOAT or C.TERRAIN_COST_LAND
          local tc = cost_table[tt] or 9999
          if tc < 100 then
            local dist = math.floor(U.hdist(tmx, tmy, nx, ny))
            _heap_push(state.frontier, dist * 65536 + ny * 256 + nx)
            state.frontier_set[nk] = true
          end
        end
      end
    end
    state.visited[k] = true
  end
end

-- Returns the closest unvisited frontier tile, or nil, nil.
-- Soft danger avoidance: prefer a frontier tile that's OUT of pill/shell danger
-- (cpf_danger_at) so exploration doesn't march a tank into a pillbox's fire
-- range while it's between other goals. Dangerous tiles are set ASIDE (not
-- discarded — they're re-pushed before we return, so the frontier is intact) and
-- only used as a fallback if everything reachable is dangerous, so exploration
-- never stalls. Search is bounded by SKIP_LIMIT so a big danger field is cheap.
--
-- The set-aside list lives on the heap (h.aside), not in a local, so a stop
-- (tick budget) between the pop and the re-push loses nothing: the next call
-- pushes the left-over entries back first. Each entry is added to h.aside
-- before it is popped. Without a stop h.aside is empty at entry and the heap
-- sees the same pops and pushes in the same order as before.
function M.best_frontier(state)
  local h = state.frontier
  if not h then return nil, nil end
  _heap_heal(h)
  local aside = h.aside
  if aside == nil then
    aside = { n = 0, done = 0 }
    h.aside = aside
  elseif aside.n > 0 then
    _aside_flush(h, aside)               -- left over from a stopped call
  end
  local thresh   = C.EXPLORE_DANGER_AVOID or 5
  local measure  = (cpf_danger_at ~= nil)
  local fb_x, fb_y                      -- nearest unvisited, danger notwithstanding
  local skipped, SKIP_LIMIT = 0, 48
  while h.n > 0 do
    local top = h[1]
    if top == nil then
      -- Heap desync: h[1] missing while n>0 (see the kill-safety note at the
      -- heap helpers). Heal it from its non-nil entries instead of crashing
      -- or dropping the whole frontier, then look again. After a rebuild h[1]
      -- is a number whenever h.n > 0, so this runs at most once per call.
      _heap_rebuild(h)
      if h.n == 0 then break end
      top = h[1]
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
        if aside.n > 0 then _aside_flush(h, aside) end
        return nx, ny
      end
      if not fb_x then fb_x, fb_y = nx, ny end
      local k = aside.n + 1
      if k == 1 then aside.done = 0 end  -- (a stop can leave a stale count)
      aside[k] = top                     -- keep it first, then pop
      aside.n = k
      _heap_pop(h)                       -- frontier_set kept -> re-pushed below
      skipped = skipped + 1
      if skipped >= SKIP_LIMIT then break end
    end
  end
  if aside.n > 0 then _aside_flush(h, aside) end
  return fb_x, fb_y                       -- nearest (dangerous) fallback, or nil
end

return M
