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

-- ── Helpers for the stop (tick budget) tests ────────────────────────────────
-- Run fn under a count hook that raises an error at instruction k. Returns
-- true if the hook fired (fn was stopped), false if fn ran to the end.
local function run_killed(k, fn)
  local count, fired = 0, false
  local function hook()
    count = count + 1
    if count >= k then fired = true; debug.sethook(); error("tick_budget_exceeded") end
  end
  pcall(function() debug.sethook(hook, "", 1); fn(); debug.sethook() end)
  debug.sethook()
  return fired
end

-- Set of tile keys (my*256+mx) held in slots 1..h.n.
local function heap_keys(h)
  local t = {}
  for i = 1, h.n do local v = h[i]; if v then t[v % 65536] = true end end
  return t
end
local function key_of(mx, my) return my * 256 + mx end

-- Danger field: tiles with mx < DANGER_X are dangerous (danger 9, above 5).
local DANGER_X = 0
local function danger_at(mx, my) return (mx < DANGER_X) and 9 or 0 end

-- State with n frontier tiles: tile i is at (10+i, 60+i%3) with cost i, so
-- the lowest-cost tiles have the smallest mx and are the dangerous ones.
local function danger_state(n)
  local st = { visited = {}, frontier = expl.new_frontier(), frontier_set = {} }
  for i = 1, n do
    local mx, my = 10 + i, 60 + (i % 3)
    st.frontier_set[key_of(mx, my)] = true
    expl.frontier_push(st.frontier, i, mx, my)
  end
  return st
end

-- frontier_set and the heap hold the same tiles (duplicates allowed).
local function set_agrees(st)
  local hk = heap_keys(st.frontier)
  for k in pairs(st.frontier_set) do if not hk[k] then return false, "lost " .. k end end
  for k in pairs(hk) do if not st.frontier_set[k] and not st.visited[k] then return false, "extra " .. k end end
  return true
end

-- best_frontier as it was before this fix (local aside list), on the 2.2.1
-- push/pop. Reference for "same choices when no stop happens".
local function ref_best_frontier(state)
  local h = state.frontier
  local thresh, measure = 5, (cpf_danger_at ~= nil)
  local fb_x, fb_y
  local aside, n_aside = nil, 0
  local skipped, SKIP_LIMIT = 0, 48
  while h.n > 0 do
    local top = h[1]
    local nx, ny = top % 256, math.floor(top / 256) % 256
    local nk = ny * 256 + nx
    if state.visited[nk] then
      ref_pop(h); state.frontier_set[nk] = nil
    else
      local dng = measure and cpf_danger_at(nx, ny) or 0
      if dng <= thresh then
        if aside then for i = 1, n_aside do ref_push(h, aside[i]) end end
        return nx, ny
      end
      if not fb_x then fb_x, fb_y = nx, ny end
      ref_pop(h)
      aside = aside or {}
      n_aside = n_aside + 1; aside[n_aside] = top
      skipped = skipped + 1
      if skipped >= SKIP_LIMIT then break end
    end
  end
  if aside then for i = 1, n_aside do ref_push(h, aside[i]) end end
  return fb_x, fb_y
end

-- ── 8. No stop: best_frontier picks and heap match the old code ─────────────
do
  _G.cpf_danger_at = function(mx, my) return ((mx * 7 + my * 3) % 5 == 0) and 9 or 0 end
  local function mk() return { visited = {}, frontier = { n = 0 }, frontier_set = {} } end
  local a, b = mk(), mk()
  local same_pick, same_heap, picks, danger_seen = true, true, 0, 0
  for step = 1, 3000 do
    local r = rnd(10)
    if r < 6 then
      local mx, my, c = rnd(256), rnd(256), rnd(40)
      local k = key_of(mx, my)
      if not a.frontier_set[k] and not a.visited[k] then
        a.frontier_set[k] = true; b.frontier_set[k] = true
        expl.frontier_push(a.frontier, c, mx, my); ref_push(b.frontier, enc(c, mx, my))
      end
    elseif r < 8 then
      local fx, fy = expl.best_frontier(a)
      local gx, gy = ref_best_frontier(b)
      picks = picks + 1
      if fx ~= gx or fy ~= gy then same_pick = false end
      if fx and cpf_danger_at(fx, fy) > 5 then danger_seen = danger_seen + 1 end
      if fx and rnd(2) == 0 then      -- explore it: mark visited
        local k = key_of(fx, fy); a.visited[k] = true; b.visited[k] = true
      end
    else
      if a.frontier.n > 0 then
        local v = a.frontier[1] % 65536
        a.visited[v] = true; b.visited[v] = true
      end
    end
    if not same_array(a.frontier, b.frontier) then same_heap = false; break end
  end
  check("8a no stop: best_frontier returns the same tile as the old code", same_pick, picks .. " picks")
  check("8b no stop: heap array identical to the old code after every step", same_heap)
  check("8c the run set aside danger tiles and used the fallback", danger_seen > 0, danger_seen .. " fallbacks")
  check("8d no stop: nothing left in hold or aside",
        a.frontier.hold == nil and (a.frontier.aside == nil or a.frontier.aside.n == 0))
end

-- ── 9. Stop at every instruction inside best_frontier: no tile lost ─────────
do
  _G.cpf_danger_at = danger_at
  DANGER_X = 10 + 6                  -- the 5 lowest-cost tiles are dangerous
  local ref = danger_state(20)
  local want_x, want_y = expl.best_frontier(ref)
  local lost, bad_pick, bad_heap, runs, fired_runs = 0, 0, 0, 0, 0
  for k = 1, 100000 do
    local st = danger_state(20)
    local fired = run_killed(k, function() expl.best_frontier(st) end)
    runs = runs + 1
    if fired then fired_runs = fired_runs + 1 end
    local fx, fy = expl.best_frontier(st)       -- next tick, same state
    if fx ~= want_x or fy ~= want_y then bad_pick = bad_pick + 1 end
    if not set_agrees(st) then lost = lost + 1 end
    if not valid(st.frontier) then bad_heap = bad_heap + 1 end
    if not fired then break end
  end
  check("9a stop anywhere in best_frontier then run again: no tile lost", lost == 0,
        lost .. " lost of " .. runs .. " runs (" .. fired_runs .. " stopped)")
  check("9b ...and the next call returns the same tile as with no stop", bad_pick == 0, bad_pick .. " differ")
  check("9c ...and the heap is valid", bad_heap == 0, bad_heap .. " bad")
  check("9d the stop loop covered the whole call", fired_runs > 200, fired_runs .. " stop points")
  DANGER_X = 0
end

-- ── 10. goals.lua explore pick: same as 2.2.1 ─────────────────────────────
-- The pick loop in goals.lua is not changed by this fix: when the tile from
-- best_frontier is too close it marks it visited and calls frontier_pop, which
-- takes h[1]. After best_frontier pushed the danger tiles back, h[1] is the
-- nearest danger tile, so 2.2.1 removes that tile for good. This test keeps
-- that behaviour: the loop below is the goals.lua loop, run on the new code and
-- on the 2.2.1 reference, and both must give the same picks and heap. (The
-- goals.lua lines are the 2.2.1 lines, so they are copied here, not shared.)
do
  _G.cpf_danger_at = danger_at
  DANGER_X = 10 + 6                  -- tiles 11..15 dangerous, 16 is the pick
  local TOO_CLOSE_X = 18             -- tiles 16 and 17 are too close
  local function pick(st, best, pop)
    local fx, fy = best(st)
    while fx and fx < TOO_CLOSE_X do
      local nk = key_of(fx, fy)
      st.visited[nk]      = true
      st.frontier_set[nk] = nil
      pop(st.frontier)
      fx, fy = best(st)
    end
    return fx, fy
  end
  local a, b = danger_state(20), danger_state(20)
  local fx, fy = pick(a, expl.best_frontier, expl.frontier_pop)
  local gx, gy = pick(b, ref_best_frontier, ref_pop)
  check("10a pick loop returns the same tile as 2.2.1", fx == gx and fy == gy and fx == 18,
        tostring(fx) .. " vs " .. tostring(gx))
  check("10b heap array identical to 2.2.1 after the pick loop", same_array(a.frontier, b.frontier))
  local hk = heap_keys(a.frontier)
  check("10c as in 2.2.1, the two nearest danger tiles were dropped",
        not hk[key_of(11, 61)] and not hk[key_of(12, 62)] and hk[key_of(13, 60)]
        and hk[key_of(14, 61)] and hk[key_of(15, 62)])
  check("10d heap valid, nothing left in hold or aside",
        valid(a.frontier) and a.frontier.hold == nil and a.frontier.aside.n == 0)
  DANGER_X = 0
end

-- ── 11. Stop at every instruction of push / pop: no entry lost ─────────────
do
  local lost, runs = 0, 0
  for which = 1, 2 do
    for k = 1, 2000 do
      local h, vals = build(40)
      local v = rnd_entry()
      local snap = { n = h.n }
      for i = 1, h.n + 2 do snap[i] = h[i] end
      local fired = run_killed(k, function()
        if which == 1 then push_v(h, v) else expl.frontier_pop(h) end
      end)
      runs = runs + 1
      -- A push stopped before it took v (h.hold or a slot) must have left the
      -- heap untouched; the caller pushes v again (test 13 checks update).
      -- (The test values can repeat, so count copies of v rather than look.)
      local v_in, v_before = (h.hold == v) and 1 or 0, 0
      for i = 1, h.n do if h[i] == v then v_in = v_in + 1 end end
      for _, x in ipairs(vals) do if x == v then v_before = v_before + 1 end end
      local v_kept = v_in > v_before
      if which == 1 and not v_kept and not same_array(h, snap) then lost = lost + 1 end
      if not valid(h) then lost = lost + 1 end
      -- Every value that should remain must still come out of the heap. A pop
      -- may take one entry: the smallest value.
      local out = drain(h) or {}
      local have = {}
      for _, x in ipairs(out) do have[x] = (have[x] or 0) + 1 end
      local must = {}
      for _, x in ipairs(vals) do must[#must + 1] = x end
      if which == 1 and v_kept then must[#must + 1] = v end
      table.sort(must)
      local missing = {}
      for _, x in ipairs(must) do
        if (have[x] or 0) == 0 then missing[#missing + 1] = x
        else have[x] = have[x] - 1 end
      end
      local allowed = 0
      if #missing == 1 then
        local x = missing[1]
        if which == 2 and x == must[1] then allowed = 1 end
      end
      if #missing > allowed then lost = lost + 1 end
      if not fired then break end
    end
  end
  check("11 push/pop stopped at any instruction: heap valid, no entry lost", lost == 0,
        lost .. " lost of " .. runs)
end

-- ── 12. Stop at every instruction of update: all neighbours end up pushed ───
do
  local bad, runs = 0, 0
  local info = { tankx = 100 * 256 + 128, tanky = 80 * 256 + 128, inboat = false }
  for k = 1, 20000 do
    local st = { visited = {}, frontier = expl.new_frontier(), frontier_set = {} }
    local fired = run_killed(k, function() expl.update(st, info) end)
    expl.update(st, info)                       -- next tick
    runs = runs + 1
    local hk = heap_keys(st.frontier)
    local cnt = 0
    for dy = -1, 1 do for dx = -1, 1 do
      if dx ~= 0 or dy ~= 0 then
        local kk = key_of(100 + dx, 80 + dy)
        if hk[kk] and st.frontier_set[kk] then cnt = cnt + 1 end
      end
    end end
    if cnt ~= 8 or not st.visited[key_of(100, 80)] or not valid(st.frontier) then bad = bad + 1 end
    if not fired then break end
  end
  check("12 update stopped at any instruction then run again: all 8 neighbours kept", bad == 0,
        bad .. " bad of " .. runs)
end

-- ── 13. Stop inside the rebuild of a holed heap: order restored, none lost ──
-- A pop or push on a heap with a hole rebuilds it. A stop inside that rebuild
-- can leave the slots out of order; h.dirty must make the next call rebuild
-- again, so the heap drains in order with every entry kept.
do
  _G.cpf_danger_at = nil
  local bad, runs, dirty_stops = 0, 0, 0
  for which = 1, 2 do
    for k = 1, 5000 do
      local h, vals = build(60)
      local hole = 2 + rnd(57)
      local gone = h[hole]
      h[hole] = nil                    -- a hole left by an old stop
      local must = {}
      for _, x in ipairs(vals) do must[#must + 1] = x end
      for m = 1, #must do if must[m] == gone then table.remove(must, m); break end end
      local v = rnd_entry()
      if which == 1 then must[#must + 1] = v end
      table.sort(must)
      local fired = run_killed(k, function()
        if which == 1 then push_v(h, v) else expl.frontier_pop(h) end
      end)
      runs = runs + 1
      if fired and h.dirty then dirty_stops = dirty_stops + 1 end
      -- Next tick: the first heap call heals, then pop everything.
      local out = drain(h)
      local ok = out ~= nil and sorted(out) and h.dirty == nil
      if ok then
        local have = {}
        for _, x in ipairs(out) do have[x] = (have[x] or 0) + 1 end
        local missing = {}
        for _, x in ipairs(must) do
          if (have[x] or 0) == 0 then missing[#missing + 1] = x else have[x] = have[x] - 1 end
        end
        -- A stopped push may not have taken v yet (the caller pushes it
        -- again); a pop may have taken the smallest entry.
        local allowed = 0
        if #missing == 1 then
          if which == 1 and missing[1] == v then allowed = 1 end
          if which == 2 and missing[1] == must[1] then allowed = 1 end
        end
        if #missing > allowed then ok = false end
      end
      if not ok then bad = bad + 1 end
      if not fired then break end
    end
  end
  check("13a stop inside a rebuild then run again: heap in order, no entry lost", bad == 0,
        bad .. " bad of " .. runs)
  check("13b some stops landed inside the rebuild (h.dirty set)", dirty_stops > 0,
        dirty_stops .. " stops")
end

print(string.format("\n%d passed, %d failed", passes, fails))
os.exit(fails == 0 and 0 or 1)
