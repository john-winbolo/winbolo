-- =========================================================================
-- test_exploration_heap.lua — standalone unit tests for the explore frontier
-- heap in exploration.lua (push / pop / best_frontier).
--
-- Covers the 2.2.1 crash "exploration.lua:47: attempt to compare number with
-- nil" in _heap_pop: a hole (nil) inside 1..h.n. The heap must heal itself,
-- never crash, and keep the exact same push/pop results on a valid heap.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_exploration_heap.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

-- The kill test uses a count hook; JIT traces can skip hooks, so stay in the
-- interpreter.
if jit and jit.off then jit.off() end

-- Stubs for the engine-backed modules. Terrain is all grass, the map is 256x256.
package.loaded["constants"] = {
  MAP_W = 256,
  DIRS8 = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} },
  TERRAIN_COST_LAND = { [4] = 1 },
  TERRAIN_COST_BOAT = { [4] = 1 },
  EXPLORE_DANGER_AVOID = 5,
}
package.loaded["util"] = {
  mkey   = function(mx, my) return my * 256 + mx end,
  in_map = function(mx, my) return mx >= 0 and mx < 256 and my >= 0 and my < 256 end,
  ttype  = function() return 4 end,
  hdist  = function(a, b, c, d) return math.max(math.abs(a - c), math.abs(b - d)) end,
}

local expl = require("exploration")
local bit  = require("bitcompat")

local fails, passes = 0, 0
local function check(name, cond, detail)
  if cond then passes = passes + 1; print("PASS  " .. name .. (detail and ("  " .. detail) or ""))
  else fails = fails + 1; print("FAIL  " .. name .. (detail and ("  " .. detail) or "")) end
end

-- Entry encoding used by frontier_push: cost * 65536 + my * 256 + mx.
local function enc(cost, mx, my) return cost * 65536 + my * 256 + mx end
local function push_v(h, v) expl.frontier_push(h, math.floor(v / 65536), v % 256, math.floor(v / 256) % 256) end

-- A heap is valid when every slot 1..n is a number and each parent <= child.
local function valid(h)
  for i = 1, h.n do
    if type(h[i]) ~= "number" then return false, "slot " .. i .. " is " .. tostring(h[i]) end
    if i > 1 and h[bit.rshift(i, 1)] > h[i] then return false, "order broken at " .. i end
  end
  return true
end

-- The heap code as shipped in 2.2.1, used as the reference for "behaviour
-- unchanged on a valid heap".
local function ref_push(h, v)
  local n = h.n + 1
  h.n = n
  h[n] = v
  local i = n
  while i > 1 do
    local p = bit.rshift(i, 1)
    local hp = h[p]
    if hp == nil or hp <= h[i] then break end
    h[i], h[p] = h[p], h[i]
    i = p
  end
end
local function ref_pop(h)
  local n = h.n
  if n == 0 then return nil end
  local top = h[1]
  h[1] = h[n]; h[n] = nil; h.n = n - 1
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

local function same_array(a, b)
  if a.n ~= b.n then return false end
  for i = 1, a.n + 2 do if a[i] ~= b[i] then return false end end
  return true
end

-- Deterministic pseudo-random values (no math.random state shared with others).
local seed = 12345
local function rnd(m) seed = (seed * 1103515245 + 12345) % 2147483648; return seed % m end
local function rnd_entry() return enc(rnd(40), rnd(256), rnd(256)) end

-- Build a heap of k entries with the new code.
local function build(k)
  local h = expl.new_frontier()
  local vals = {}
  for i = 1, k do local v = rnd_entry(); vals[i] = v; push_v(h, v) end
  return h, vals
end

-- Pop everything; return the list, or nil + error text on a crash.
local function drain(h)
  local out = {}
  while h.n > 0 do
    local ok, v = pcall(expl.frontier_pop, h)
    if not ok then return nil, v end
    if v == nil then break end
    out[#out + 1] = v
  end
  return out
end
local function sorted(t) for i = 2, #t do if t[i - 1] > t[i] then return false end end return true end

-- ── 1. Valid heap: same array state and pop order as the 2.2.1 code ─────────
do
  local h, r = expl.new_frontier(), { n = 0 }
  local same, popped_same = true, true
  for step = 1, 4000 do
    if rnd(3) > 0 or h.n == 0 then
      local v = rnd_entry()
      push_v(h, v); ref_push(r, v)
    else
      local a, b = expl.frontier_pop(h), ref_pop(r)
      if a ~= b then popped_same = false end
    end
    if not same_array(h, r) then same = false; break end
  end
  check("1a valid heap: array identical to 2.2.1 code after every step", same)
  check("1b valid heap: pop results identical to 2.2.1 code", popped_same)
  local out = drain(h)
  check("1c drained in ascending order", out and sorted(out) and #out > 0, "n=" .. (out and #out or -1))
  check("1d pop on empty heap returns nil", expl.frontier_pop(expl.new_frontier()) == nil)
end

-- ── 2. Hole in the middle ───────────────────────────────────────────────────
do
  local h = build(50)
  local gone = h[20]
  h[20] = nil
  local out, err = drain(h)
  check("2a pop with a hole in the middle does not crash", out ~= nil, err and tostring(err))
  check("2b remaining 49 entries come out in order", out and #out == 49 and sorted(out),
        "got " .. (out and #out or -1) .. " (dropped " .. tostring(gone) .. ")")
end

-- ── 3. Hole at the end (h[n] nil: the budget-kill state of the old code) ────
do
  local h = build(30)
  h[h.n] = nil
  local ok, v = pcall(expl.frontier_pop, h)
  check("3a pop with h[n] nil does not crash", ok, not ok and tostring(v))
  local good, why = valid(h)
  check("3b heap valid after that pop", good, why)
  local out = drain(h)
  check("3c rest drains in order, 28 left", out and #out == 28 and sorted(out), "got " .. (out and #out or -1))
end

-- ── 4. h[1] nil with n > 0 ──────────────────────────────────────────────────
do
  local h = build(25)
  h[1] = nil
  local ok, v = pcall(expl.frontier_pop, h)
  check("4a pop with h[1] nil does not crash and returns a number", ok and type(v) == "number", tostring(v))
  local good, why = valid(h)
  check("4b heap valid after that pop, 23 left", good and h.n == 23, (why or "") .. " n=" .. h.n)
  local h2 = { n = 3 }                 -- every slot missing
  local ok2, v2 = pcall(expl.frontier_pop, h2)
  check("4c pop on a heap with only holes returns nil, n=0", ok2 and v2 == nil and h2.n == 0)
end

-- ── 5. Old-code budget kill replay: count set, slot not written; then more ──
do
  local h = build(10)
  h.n = h.n + 1                          -- killed between h.n = n and h[n] = v
  local ok1, e1 = pcall(function()
    for i = 1, 5 do push_v(h, rnd_entry()) end -- hole now sits in the middle
  end)
  check("5a push onto a holed heap does not crash", ok1, e1 and tostring(e1))
  local out, err = drain(h)
  check("5b then drains in order without crash", out and sorted(out) and #out == 15, err and tostring(err))
end

-- ── 6. Kill at every instruction: push/pop leave no hole in 1..n ────────────
do
  local holes, runs = 0, 0
  for which = 1, 2 do
    for k = 1, 400 do
      local h = build(40)
      local v = rnd_entry()
      local count = 0
      local function hook()
        count = count + 1
        if count >= k then debug.sethook(); error("tick_budget_exceeded") end
      end
      local function op()
        debug.sethook(hook, "", 1)
        if which == 1 then push_v(h, v) else expl.frontier_pop(h) end
        debug.sethook()
      end
      pcall(op)
      debug.sethook()
      runs = runs + 1
      for i = 1, h.n do if h[i] == nil then holes = holes + 1; break end end
      local out = drain(h)
      if not out then holes = holes + 1 end
    end
  end
  check("6 push/pop killed at any instruction never leave a hole", holes == 0,
        holes .. " bad of " .. runs)
end

-- ── 7. best_frontier on a holed heap ────────────────────────────────────────
do
  _G.cpf_danger_at = nil
  local function fresh_state()
    local st = { visited = {}, frontier = expl.new_frontier(), frontier_set = {} }
    for i = 1, 12 do
      local mx, my = 100 + i, 50
      st.frontier_set[my * 256 + mx] = true
      expl.frontier_push(st.frontier, i, mx, my)
    end
    return st
  end
  local st = fresh_state()
  st.frontier[1] = nil
  local ok, fx, fy = pcall(expl.best_frontier, st)
  check("7a best_frontier with h[1] nil returns a tile", ok and fx ~= nil and fy == 50,
        tostring(fx) .. "," .. tostring(fy))
  local good, why = valid(st.frontier)
  check("7b heap valid after, other 11 entries kept", good and st.frontier.n == 11, (why or "") .. " n=" .. st.frontier.n)
  check("7c returned tile is the nearest remaining (cost 2)", fx == 102)

  local st2 = fresh_state()
  st2.frontier[st2.frontier.n] = nil
  st2.visited[st2.frontier[1] % 256 + 50 * 256] = true   -- forces a pop inside best_frontier
  local ok2, fx2 = pcall(expl.best_frontier, st2)
  check("7d best_frontier popping a heap with h[n] nil does not crash", ok2 and fx2 ~= nil, tostring(fx2))
  local good2, why2 = valid(st2.frontier)
  check("7e heap valid after", good2, why2)

  local st3 = fresh_state()
  st3.frontier[5] = nil
  for i = 1, 12 do st3.visited[(100 + i) + 50 * 256] = (i <= 6) end
  local ok3, fx3 = pcall(expl.best_frontier, st3)
  check("7f best_frontier with a middle hole skips visited tiles, no crash", ok3 and fx3 ~= nil and fx3 >= 107,
        tostring(fx3))
  local good3, why3 = valid(st3.frontier)
  check("7g heap valid after", good3, why3)
end

print(string.format("\n%d passed, %d failed", passes, fails))
os.exit(fails == 0 and 0 or 1)
