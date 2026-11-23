-- =========================================================================
-- test_pt2_shadowmap.lua -- standalone unit tests (2026-11-23) for the
-- shadow map pill take evaluator (pilltake_shadow.lua):
--   1. the offset table: a ray built from it is the same as shadow.cast,
--      field by field, for real pill squares (map edges too) and random maps;
--   2. map updates: after a square changes (wall built, wall gone, allied
--      pill health, pill dropped on a path, pill picked up) the updated map
--      is the same as a full rebuild; a moved pill gets a new map;
--   3. the safety rule: a chain pill is never covered by "reach", a
--      bystander is;
--   4. the chain and the blocker stage on small maps (chain found, the plan,
--      a dropped pill first, PT2_BUILD_MAX 0 = no plan);
--   5. a group run keeps only chains that hold the goal pill.
-- No engine: a hand-made map, the shadow.lua rays.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_shadowmap.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local C   = require("constants")
local U   = require("util")
local cpf = require("cpathfinder")
local PC  = require("pill_cover")
_G.print2 = _G.print2 or function() end
local SH  = require("shadow")
local V2  = require("pilltake_v2_old")
local SM  = require("pilltake_shadow")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

local terrain = {}
local function sq(mx, my) return my * 256 + mx end
U.ttype      = function(mx, my) return terrain[sq(mx, my)] or C.T_GRASS end
U.ttype_peek = U.ttype
U.base_tile_stops_tank_shell = function() return false end
cpf.lgm_travel_ticks_map = function() return 10 end

local function world_of(pills)
  local w = { pills = {}, pill_at = {}, bases = {}, base_at = {} }
  for i, p in ipairs(pills) do
    w.pills[i] = p
    w.pill_at[sq(p.mx, p.my)] = { { id = i, pill = p } }
  end
  return w
end

local function inp_of(pills, A)
  local inp = SM.knob_inp(C, {})
  inp = V2.world_inp(world_of(pills), C, U, PC, cpf, inp, false)
  inp.A = A or pills[1]
  inp.radius = 7.0
  inp.num_pill_blockers = 0
  inp.score_fn = function(deg, mx, my, cx, cy) return math.abs(cx - 128) + math.abs(cy - 134) end
  return inp
end

-- ray fields equal (shadow.cast vs the offset table)
local function same_ray(a, b)
  if a.hit ~= b.hit then return false, "hit" end
  if a.t ~= b.t then return false, "t" end
  if a.link ~= b.link then return false, "link" end
  if a.link_t ~= b.link_t then return false, "link_t" end
  if a.cover ~= b.cover then return false, "cover" end
  if a.front ~= b.front then return false, "front" end
  if a.n_sq ~= b.n_sq then return false, "n_sq" end
  if a.n_block ~= b.n_block then return false, "n_block" end
  if a.end_x ~= b.end_x or a.end_y ~= b.end_y then return false, "end" end
  if a.angle ~= b.angle then return false, "angle" end
  for k = 1, a.n_sq do
    if a.sq[k] ~= b.sq[k] or a.sq_t[k] ~= b.sq_t[k] then return false, "sq " .. k end
  end
  for k = 1, a.n_block do
    local x, y = a.block[k], b.block[k]
    if x.sq ~= y.sq or x.shots ~= y.shots or x.kind ~= y.kind or x.t ~= y.t or x.pill ~= y.pill then
      return false, "block " .. k
    end
  end
  return true
end

-- ── 1. the offset table is shadow.cast ───────────────────────────────────
print("1. offset table vs shadow.cast")
do
  local seed = 12345
  local function rnd(n)
    seed = (seed * 1103515245 + 12345) % 2147483648
    return seed % n
  end
  local kinds = { C.T_BUILDING, C.T_HALFBUILD, C.T_BOAT, C.T_FOREST, C.T_GRASS, C.T_GRASS }
  local n_cmp, n_bad, n_off, why_bad = 0, 0, 0, nil
  for trial = 1, 60 do
    for k in pairs(terrain) do terrain[k] = nil end
    -- the pill: anywhere, edges too
    local edge = trial % 4 == 0
    local X = { mx = edge and rnd(6) or 20 + rnd(216), my = edge and 250 + rnd(6) or 20 + rnd(216),
                owner = "hostile", health = 15 }
    local pills = { X }
    for _ = 1, 6 do
      local p = { mx = X.mx - 8 + rnd(17), my = X.my - 8 + rnd(17), health = 1 + rnd(15),
                  owner = (rnd(2) == 0) and "friendly" or "hostile" }
      if p.mx ~= X.mx or p.my ~= X.my then pills[#pills + 1] = p end
    end
    for _ = 1, 40 do
      terrain[sq((X.mx - 9 + rnd(19)) % 256, (X.my - 9 + rnd(19)) % 256)] = kinds[1 + rnd(#kinds)]
    end
    local inp = inp_of(pills, X)
    local env = inp.env
    if trial % 3 == 0 then env.forest_transparent = false end
    if trial % 5 == 0 then SM.live_shell(env, { shell_speed = 30, shell_life = 9, tank_hit_radius = 100 }) end
    local run = SM._new_run(inp)
    local extra = (trial % 2 == 0) and { [sq(X.mx + 1, X.my)] = 5, [sq(X.mx, X.my + 2)] = 15 } or nil
    for _ = 1, 80 do
      local px = (math.floor(X.mx * 4) - 34 + rnd(69)) * 64 + 32
      local py = (math.floor(X.my * 4) - 34 + rnd(69)) * 64 + 32
      local sx, sy = X.mx * 256 + 128, X.my * 256 + 128
      local g = SM._geom(env, px - sx, py - sy)
      local off = X.mx + g.lo_x < 0 or X.mx + g.hi_x > 255 or X.my + g.lo_y < 0 or X.my + g.hi_y > 255
      if off then
        n_off = n_off + 1
      else
        local a = SH.cast(env, sx, sy, px, py, env.pill_fire_len or SH.PILL_FIRE_LEN, extra)
        local b = SM._derive_ray(run, g, X.mx, X.my, extra, X)
        n_cmp = n_cmp + 1
        local ok, why = same_ray(a, b)
        if not ok then n_bad = n_bad + 1; why_bad = why_bad or why end
      end
    end
  end
  check(string.format("%d rays: the offset table gives shadow.cast's ray", n_cmp), n_bad == 0,
        tostring(n_bad) .. " " .. tostring(why_bad))
  check("rays that leave the map are found (cast exactly instead)", n_off > 0, n_off)
end

-- ── 2. map updates = full rebuild ────────────────────────────────────────
print("2. map updates")
local function map_copy(X)
  local m = SM._maps()[X.my * 256 + X.mx]
  if not m then return nil end
  local o = {}
  for k, e in pairs(m.pts) do
    o[k] = { e.cover, e.front, e.link_sq or -1, e.link_t or -1, e.ally }
  end
  return o, m
end
local function same_map(a, b)
  if not a or not b then return false, "missing" end
  local n = 0
  for k, x in pairs(a) do
    local y = b[k]
    if not y then return false, "point" end
    for i = 1, 5 do if x[i] ~= y[i] then return false, "field " .. i end end
    n = n + 1
  end
  for k in pairs(b) do if not a[k] then return false, "extra point" end end
  return true, n
end
do
  for k in pairs(terrain) do terrain[k] = nil end
  local X = { mx = 100, my = 100, owner = "hostile", health = 15 }
  local F = { mx = 103, my = 100, owner = "friendly", health = 15 }
  local B = { mx = 100, my = 104, owner = "hostile", health = 15 }
  local pills = { X, F, B }
  terrain[sq(101, 101)] = C.T_BUILDING
  local function build(fresh)
    if fresh then SM.reset(true) end
    local inp = inp_of(pills, X)
    return SM.build_full_map(inp, X)
  end
  SM.reset()
  local npt = build(true)
  check("a full map has the grid points in range", npt > 3000, npt)
  local function step(name, change)
    change()
    local upd0 = SM.stats.pt_upd
    build(false)
    local a = map_copy(X)
    local nupd = SM.stats.pt_upd - upd0
    build(true)
    local b = map_copy(X)
    local ok, why = same_map(a, b)
    check(name .. ": updated map = full rebuild", ok, why)
    check(name .. ": only some points walked again (" .. nupd .. ")", nupd > 0 and nupd < npt, nupd)
  end
  step("wall built on a path", function() terrain[sq(98, 99)] = C.T_BUILDING end)
  step("wall shot down", function() terrain[sq(101, 101)] = nil end)
  step("half wall", function() terrain[sq(102, 97)] = C.T_HALFBUILD end)
  step("boat", function() terrain[sq(97, 102)] = C.T_BOAT end)
  step("allied pill health", function() F.health = 6 end)
  step("pill dropped on a path", function()
    pills[#pills + 1] = { mx = 99, my = 103, owner = "hostile", health = 15 }
  end)
  step("pill picked up", function() table.remove(pills) end)
  -- the pill itself moves: its old map goes, a new one starts
  build(false)
  local drop0, new0 = SM.stats.map_drop, SM.stats.map_new
  X.mx, X.my = 110, 96
  build(false)
  check("moved pill: the old square's map is dropped", SM.stats.map_drop > drop0)
  check("moved pill: a new map at the new square", SM.stats.map_new > new0 and map_copy(X) ~= nil)
  local a = map_copy(X)
  build(true)
  check("moved pill: its new map = full rebuild", (same_map(a, map_copy(X))))
  -- an owner change drops the map
  build(false)
  X.owner = "friendly"
  local drop1 = SM.stats.map_drop
  local inp = inp_of(pills, B)
  SM.build_full_map(inp, B)
  check("pill changed side: its map is dropped", SM.stats.map_drop > drop1)
  X.owner = "hostile"
end

-- ── 3. the safety rule ───────────────────────────────────────────────────
print("3. safety")
do
  local inp = SM.knob_inp(C, {})
  local A = { mx = 1, my = 1 }
  local B = { mx = 2, my = 1 }
  local Z = { mx = 3, my = 3 }
  local ctx = { inp = inp, A = A, finish = false }
  local function rec_of(T, chain)
    local ic = {}
    for i, p in ipairs(chain) do ic[p] = i end
    return { T = T, chain = chain, in_chain = ic, chain_mode = #chain > 1 }
  end
  -- single take, A's shell dies first: not covered
  local r = rec_of({ A }, { A })
  SM._judge(ctx, r, { { X = A, hit = "reach", cover = 0 } })
  check("single take: reach is not cover for A", not r.covered)
  -- chain A -> B: B's shell dies first: B is not covered
  r = rec_of({ A, B }, { A, B })
  SM._judge(ctx, r, { { X = A, hit = "tank", cover = 0, link = B }, { X = B, hit = "reach", cover = 0 } })
  check("chain: reach is not cover for the last chain pill", not r.covered)
  check("chain: A is covered by the link to B", r.covered == false and #r.uncovered == 1 and r.uncovered[1] == 2,
        #r.uncovered)
  SM._judge(ctx, r, { { X = A, hit = "tank", cover = 0, link = B }, { X = B, hit = "tank", cover = 10 } })
  check("chain: 10 shots on the last chain pill hold it", r.covered and r.safe and r.depth == 10, r.depth)
  -- a bystander that cannot reach: fine
  r = rec_of({ A, Z }, { A })
  SM._judge(ctx, r, { { X = A, hit = "tank", cover = 15 }, { X = Z, hit = "reach", cover = 0 } })
  check("bystander: reach counts", r.covered)
  SM._judge(ctx, r, { { X = A, hit = "tank", cover = 15 }, { X = Z, hit = "tank", cover = 0 } })
  check("bystander in reach with no cover: not covered", not r.covered)
  -- finish: A needs nothing, a bystander keeps its bar
  local fctx = { inp = inp, A = A, finish = true }
  r = rec_of({ A }, { A })
  SM._judge(fctx, r, { { X = A, hit = "tank", cover = 0 } })
  check("finish: A's bar is 0", r.covered and r.need == 0)
end

-- ── 4. small maps: chain, plan, dropped pill ─────────────────────────────
print("4. evaluate")
do
  for k in pairs(terrain) do terrain[k] = nil end
  SM.reset()
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local B = { mx = 129, my = 120, owner = "hostile", health = 15 }
  local inp = inp_of({ A, B }, A)
  local res = SM.evaluate_group(inp)
  local b = res.best
  check("a safe chain is found", b and b.safe, b and b.safe)
  check("it is a two-pill chain", b and b.kills == 2 and b.chain_mode, b and b.kills)
  check("the last chain pill has 10 shots", b and b.depth >= 10, b and b.depth)
  check("it builds", b and #b.plan >= 1, b and #b.plan)
  check("result has full rays", b and b.rays and #b.rays == #b.T and b.rays[1].sq ~= nil)
  check("tank point on the grid (or nudged)", b and (b.nudge_wu > 0 or (b.px - 32) % 64 == 0), b and b.px)
  local out = SM.to_scan_result(res, A, nil, nil)
  check("to_scan_result reads it", out and out.best and out.chain and out.chain.kills == b.kills)
  -- a dropped pill first
  local inp2 = inp_of({ A, B }, A)
  inp2.num_pill_blockers = 1
  local b2 = SM.evaluate_group(inp2).best
  check("with a carried pill: the first blocker is the pill", b2 and b2.plan[1] and b2.plan[1].kind == "pill",
        b2 and b2.plan[1] and b2.plan[1].kind)
  -- no builds allowed: no plan, and no safe point needs one
  local inp3 = inp_of({ A, B }, A)
  inp3.build_max = 0
  local b3 = SM.evaluate_group(inp3).best
  check("PT2_BUILD_MAX 0: no plan", b3 == nil or #b3.plan == 0, b3 and #b3.plan)
  check("PT2_BUILD_MAX 0: nothing safe on open ground", b3 == nil or not b3.safe)
  -- the maps stay for the next run
  local nm = SM.map_count()
  check("maps are kept between runs", nm >= 2, nm)
  local new0 = SM.stats.pt_new
  SM.evaluate_group(inp_of({ A, B }, A))
  check("a second run on the same map walks no new point", SM.stats.pt_new == new0, SM.stats.pt_new - new0)
end

-- ── 5. a group run: the chain must hold the goal ─────────────────────────
print("5. group")
do
  for k in pairs(terrain) do terrain[k] = nil end
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local B = { mx = 129, my = 120, owner = "hostile", health = 15 }
  local inp = inp_of({ A, B }, B)    -- goal B; A is the other pill of the group
  local res = SM.evaluate_group(inp)
  local b = res.best
  check("goal B: a result", b ~= nil)
  check("goal B: the winner's chain holds B", b and b.in_chain and b.in_chain[B] ~= nil)
  check("goal B: res.goal_A is B", res.goal_A == B)
  check("goal B: inp.A is back to B", inp.A == B and inp.need_goal == nil)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
