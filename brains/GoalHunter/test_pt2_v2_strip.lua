-- =========================================================================
-- test_pt2_v2_strip.lua -- standalone unit tests (2026-11-23) of the strip
-- pill take evaluator (pilltake_v2.lua):
--   * a ray with planned blockers made from the plain ray = shadow.cast
--     with those blockers;
--   * wedges and masks against exact casts (CANT never removes a point
--     that has a clear aim; a sure MUST / COVER prediction is what the
--     exact ray gives), on random maps and on the thin strip double take;
--   * the chain DFS (max chain, chain_skip, a blocker in front cuts);
--   * the wall search against a brute force over every set;
--   * the wedges are kept while nothing changes and made again on a change;
--   * evaluate runs and its best point passes the exact tests.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_pt2_v2_strip.lua
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
local SH  = require("shadow")

_G.print2 = _G.print2 or function() end

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
cpf.lgm_travel_ticks_map = function(omx, omy, mx, my)
  if U.is_water(U.ttype(mx, my)) then return -1 end
  return 10
end

local OLD = require("pilltake_v2_old")
local V   = require("pilltake_v2")

-- a seeded random (LuaJIT math.random is fine, but keep it ours)
local seed = 12345
local function rnd(n)
  seed = (seed * 1103515245 + 12345) % 2147483648
  return (seed % n) + 1
end

local function make_world(pills)
  local w = { pills = {}, pill_at = {}, bases = {}, base_at = {} }
  for i, p in ipairs(pills) do
    w.pills[i] = p
    w.pill_at[sq(p.mx, p.my)] = { { id = i, pill = p } }
  end
  return w
end

local function make_inp(world, A, npb)
  local inp = OLD.knob_inp(C, {})
  inp = OLD.world_inp(world, C, U, PC, cpf, inp, false)
  inp.A = A
  inp.radius = C.PPT_STANDOFF or 7.0
  inp.num_pill_blockers = npb or 0
  inp.score_fn = function(deg, mx, my, cx, cy)
    local dx, dy = cx - 128.5, cy - 140.5
    return math.sqrt(dx * dx + dy * dy)
  end
  inp.c_eval = false
  inp.capture_file = nil
  return inp
end

local function clear() for k in pairs(terrain) do terrain[k] = nil end end
local function inp_wall_shots_of(world, A) return make_inp(world, A).env.wall_shots end

-- ── 1. ray with planned blockers = cast with them ─────────────────────────
print("ray_view with planned blockers = shadow.cast")
do
  local bad, n = 0, 0
  for trial = 1, 60 do
    clear()
    local pills = { { mx = 128, my = 120, owner = "hostile", health = 15 } }
    for _ = 1, 3 do
      local mx, my = 122 + rnd(12), 114 + rnd(12)
      if not (mx == 128 and my == 120) then
        pills[#pills + 1] = { mx = mx, my = my, owner = rnd(2) == 1 and "friendly" or "hostile", health = 5 * rnd(3) }
      end
    end
    for _ = 1, 25 do
      local t = ({ C.T_BUILDING, C.T_HALFBUILD, C.T_FOREST, C.T_BOAT })[rnd(4)]
      terrain[sq(120 + rnd(16), 112 + rnd(16))] = t
    end
    local world = make_world(pills)
    local inp = make_inp(world, pills[1])
    local env = inp.env
    env.forest_transparent = (trial % 2 == 0)
    for _ = 1, 20 do
      local X = pills[rnd(#pills)]
      if X.owner == "hostile" then
        local px, py = (121 + rnd(14)) * 256 + rnd(255), (113 + rnd(14)) * 256 + rnd(255)
        local sx, sy = X.mx * 256 + 128, X.my * 256 + 128
        local b = SH.cast(env, sx, sy, px, py, SH.PILL_FIRE_LEN, nil)
        b.X = X
        local extra = {}
        for k = 1, b.n_sq do if rnd(4) == 1 then extra[b.sq[k]] = (rnd(2) == 1) and 5 or 15 end end
        local e = SH.cast(env, sx, sy, px, py, SH.PILL_FIRE_LEN, extra)
        local v = V._ray_view(b, extra)
        n = n + 1
        local same = v.cover == e.cover and v.front == e.front and v.n_block == e.n_block
          and v.hit == e.hit and v.t == e.t and v.link == e.link
        if same then
          for k = 1, e.n_block do
            if v.block[k].sq ~= e.block[k].sq or v.block[k].shots ~= e.block[k].shots
               or v.block[k].t ~= e.block[k].t then same = false end
          end
        end
        if not same then bad = bad + 1 end
      end
    end
  end
  check(string.format("%d rays: the view equals the cast with blockers", n), bad == 0 and n > 100, bad)
end

-- ── 2. masks against exact casts (random maps) ────────────────────────────
print("masks against exact casts")
local function mask_trial(world, A, pills)
  local inp = make_inp(world, A)
  local ctx = V._new_ctx(inp, A)
  local asets = V._aim_wedges(ctx, A)
  local env = inp.env
  local st = { cant = 0, cant_bad = 0, pred = 0, pred_sure = 0, pred_bad = 0 }
  for deg = 0, 359 do
    for _, R in ipairs({ 7.0, 6.6, 5.0 }) do
      local cx = A.mx + 0.5 + math.sin(math.rad(deg)) * R
      local cy = A.my + 0.5 - math.cos(math.rad(deg)) * R
      local px, py = math.floor(cx * 256), math.floor(cy * 256)
      env.omx, env.omy = math.floor(cx), math.floor(cy)
      if V._cant(ctx, asets, px, py) then
        st.cant = st.cant + 1
        for _, off in ipairs(OLD.AIMS) do
          local ok, why = SH.tank_shot(env, px, py, A.mx * 256 + off[1], A.my * 256 + off[2], A.mx, A.my, nil)
          if ok or why == "short" then st.cant_bad = st.cant_bad + 1; break end
        end
      end
      local on_pill = env.pill_on(math.floor(px / 256), math.floor(py / 256))
      for _, X in ipairs(pills) do
        local dxr, dyr = X.mx * 256 + 128 - px, X.my * 256 + 128 - py
        local in_range = dxr * dxr + dyr * dyr <= (inp.pill_range + inp.range_margin) ^ 2
        if X.owner == "hostile" and in_range and not on_pill then
          local set = ctx.cw(X)
          local pr = V._predict(ctx, set, px, py)
          st.pred = st.pred + 1
          if pr.sure then
            st.pred_sure = st.pred_sure + 1
            local r = V._ray_view(SH.cast(env, X.mx * 256 + 128, X.my * 256 + 128, px, py, SH.PILL_FIRE_LEN, nil), nil)
            local cut = r.cut
            if ctx.inp.link_clear_front then cut = r.link ~= nil and OLD.link_cut(ctx.inp, r.front) end
            local used = r.link ~= nil and not cut
            if r.link ~= pr.link or used ~= pr.used or r.cover ~= pr.cover or r.front ~= pr.front
               or r.hit ~= pr.hit then
              st.pred_bad = st.pred_bad + 1
              if st.pred_bad <= 6 and os.getenv("V2DBG") then
                print(string.format("    miss X=(%d,%d) P=(%d,%d) link %s/%s used %s/%s cover %d/%d front %d/%d hit %s/%s",
                  X.mx, X.my, px, py, tostring(r.link and r.link.mx), tostring(pr.link and pr.link.mx),
                  tostring(used), tostring(pr.used), r.cover, pr.cover, r.front, pr.front, r.hit, pr.hit))
                for k = 1, r.n_block do local e = r.block[k]; print("      blk", e.sq % 256, math.floor(e.sq / 256), e.kind, e.t) end
              end
            end
          end
        end
      end
    end
  end
  return st
end
do
  local tot = { cant = 0, cant_bad = 0, pred = 0, pred_sure = 0, pred_bad = 0 }
  for trial = 1, 30 do
    clear()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local pills = { A }
    for _ = 1, 4 do
      local mx, my = 120 + rnd(16), 112 + rnd(16)
      if math.abs(mx - 128) + math.abs(my - 120) > 1 then
        pills[#pills + 1] = { mx = mx, my = my, owner = rnd(3) == 1 and "friendly" or "hostile", health = 5 * rnd(3) }
      end
    end
    for _ = 1, 20 + rnd(30) do
      local mx, my = 118 + rnd(20), 110 + rnd(20)
      if not (mx == 128 and my == 120) then
        terrain[sq(mx, my)] = ({ C.T_BUILDING, C.T_HALFBUILD, C.T_FOREST, C.T_BOAT, C.T_BUILDING })[rnd(5)]
      end
    end
    local st = mask_trial(make_world(pills), A, pills)
    for k, v in pairs(st) do tot[k] = tot[k] + v end
  end
  print(string.format("    CANT %d points; predictions %d, sure %d (%.0f%%)", tot.cant, tot.pred,
        tot.pred_sure, 100 * tot.pred_sure / math.max(1, tot.pred)))
  check("CANT never removes a point with a clear (or short) aim", tot.cant_bad == 0 and tot.cant > 0, tot.cant_bad)
  check("a sure MUST / COVER prediction is what the exact ray gives", tot.pred_bad == 0 and tot.pred_sure > 0,
        tot.pred_bad)
  check("most predictions are sure", tot.pred_sure > tot.pred * 0.7, tot.pred_sure)
end

-- ── 3. the thin strip double take ─────────────────────────────────────────
print("thin strip double take")
do
  clear()
  -- Y two squares south of A: A's shell meets Y only on a thin strip of
  -- tank points south of the pair. Walls beside the strip.
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local Y = { mx = 128, my = 122, owner = "hostile", health = 15 }
  terrain[sq(126, 123)] = C.T_BUILDING
  terrain[sq(130, 123)] = C.T_BUILDING
  local pills = { A, Y }
  local world = make_world(pills)
  local st = mask_trial(world, A, pills)
  check("thin strip: CANT never wrong", st.cant_bad == 0, st.cant_bad)
  check("thin strip: sure predictions right", st.pred_bad == 0, st.pred_bad)
  -- DFS chains over the ring: every exact chain {A, Y} point is in the DFS
  -- chain {A, Y} (or not sure)
  local inp = make_inp(world, A)
  local ctx = V._new_ctx(inp, A)
  local pts = {}
  for deg = 0, 359 do
    local cx = A.mx + 0.5 + math.sin(math.rad(deg)) * 7
    local cy = A.my + 0.5 - math.cos(math.rad(deg)) * 7
    local px, py = math.floor(cx * 256), math.floor(cy * 256)
    local T = {}
    for _, p in ipairs(pills) do T[#T + 1] = p end
    pts[#pts + 1] = { px = px, py = py, T = T }
  end
  local list, at, sure = V._chains_dfs(ctx, pts, A)
  local n2, agree, unsure = 0, 0, 0
  for i, P in ipairs(pts) do
    local rec = V._eval_point(ctx, P.px, P.py, nil, nil, false, false)
    if rec and #rec.chain == 2 then n2 = n2 + 1 end
    if not sure[i] then unsure = unsure + 1
    elseif rec then
      local mc = list[at[i]].chain
      if #mc == #rec.chain then agree = agree + 1 end
    else
      agree = agree + 1
    end
  end
  print(string.format("    chain {A,Y} at %d of 360 points; DFS sure and same %d, not sure %d", n2, agree, unsure))
  check("thin strip: some points have the chain {A, Y}", n2 > 0, n2)
  check("thin strip: every sure DFS chain is the exact chain", agree + unsure == 360, agree + unsure)
end

-- ── 4. chain rules ────────────────────────────────────────────────────────
print("chain rules")
do
  local n, bad, by_len = 0, 0, { 0, 0, 0, 0 }
  local found = {}
  for trial = 1, 120 do
    clear()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local pills = { A }
    for _ = 1, 4 + (trial % 8) do
      local mx, my = 122 + rnd(12), 114 + rnd(12)
      if math.abs(mx - 128) + math.abs(my - 120) > 1 then
        pills[#pills + 1] = { mx = mx, my = my, owner = "hostile", health = 15 }
      end
    end
    for _ = 1, rnd(10) do terrain[sq(120 + rnd(16), 112 + rnd(16))] = C.T_BUILDING end
    terrain[sq(128, 120)] = nil
    local world = make_world(pills)
    local inp = make_inp(world, A)
    local ctx = V._new_ctx(inp, A)
    for deg = 0, 359, 3 do
      local cx = A.mx + 0.5 + math.sin(math.rad(deg)) * 7
      local cy = A.my + 0.5 - math.cos(math.rad(deg)) * 7
      local px, py = math.floor(cx * 256), math.floor(cy * 256)
      local rec = V._eval_point(ctx, px, py, nil, nil, false, false)
      if rec then
        n = n + 1
        by_len[#rec.chain] = by_len[#rec.chain] + 1
        if #rec.chain > inp.max_chain then bad = bad + 1 end
        for k = 1, #rec.chain - 1 do
          local r = rec.rays[rec.ri[rec.chain[k]]]
          if r.cut or r.link ~= rec.chain[k + 1] then bad = bad + 1 end
        end
        if #rec.chain >= 2 and (#found < 40 or (#rec.chain >= 3 and #found < 60)) then
          found[#found + 1] = { pills = pills, terrain = {}, A = A, px = px, py = py, rec = rec }
          for k, v in pairs(terrain) do found[#found].terrain[k] = v end
        end
      end
    end
  end
  print(string.format("    %d points: chain length 1/2/3 = %d/%d/%d", n, by_len[1], by_len[2], by_len[3]))
  check("every chain: at most PT2_MAX_CHAIN, each link the uncut first pill", bad == 0 and by_len[2] > 0, bad)
  local skip_ok, cut_ok, cut_n, max_ok, max_n, keep_ok = 0, 0, 0, 0, 0, 0
  for _, f in ipairs(found) do
    clear()
    for k, v in pairs(f.terrain) do terrain[k] = v end
    local world = make_world(f.pills)
    local inp = make_inp(world, f.A)
    inp.chain_skip = { [f.rec.chain[2]] = true }
    local r1 = V._eval_point(V._new_ctx(inp, f.A), f.px, f.py, nil, nil, false, false)
    if r1 and #r1.chain == 1 then skip_ok = skip_ok + 1 end
    inp.chain_skip = nil
    if #f.rec.chain >= 3 then
      max_n = max_n + 1
      inp.max_chain = 2
      local r3 = V._eval_point(V._new_ctx(inp, f.A), f.px, f.py, nil, nil, false, false)
      if r3 and #r3.chain == 2 then max_ok = max_ok + 1 end
      inp.max_chain = C.PT2_MAX_CHAIN or 3
    end
    -- a wall in front of A's link (off the shot lane) cuts the chain there
    -- with PT2_LINK_CUT_MIN_SHOTS 0 (any cover cuts); with the default 10
    -- one wall (5 shots) is thin cover and keeps the link
    local ra = f.rec.rays[f.rec.ri[f.A]]
    local lane = {}
    for _, q in ipairs(f.rec.lanes[f.rec.aim_idx] or {}) do lane[q] = true end
    for k = 1, ra.n_sq do
      local q = ra.sq[k]
      if ra.sq_t[k] < ra.link_t and not lane[q] then
        terrain[q] = C.T_BUILDING
        local i0 = make_inp(world, f.A)
        i0.link_cut_min_shots = 0
        local r2 = V._eval_point(V._new_ctx(i0, f.A), f.px, f.py, nil, f.rec.aim_idx, false, false)
        local r4 = V._eval_point(V._new_ctx(make_inp(world, f.A), f.A), f.px, f.py, nil, f.rec.aim_idx, false, false)
        terrain[q] = f.terrain[q]
        if r2 then
          cut_n = cut_n + 1
          if #r2.chain == 1 and r2.have >= 5 then cut_ok = cut_ok + 1 end
          if r4 and #r4.chain >= 2 and r4.chain[2] == f.rec.chain[2] then keep_ok = keep_ok + 1 end
          break
        end
      end
    end
  end
  check(string.format("chain_skip ends the chain (%d points)", #found), skip_ok == #found and #found > 0, skip_ok)
  check(string.format("PT2_MAX_CHAIN 2 stops a 3 chain (%d points)", max_n), max_ok == max_n, max_ok)
  check(string.format("a wall in front of the link cuts the chain, and counts as cover (%d points)", cut_n),
        cut_ok == cut_n and cut_n > 0, cut_ok)
  check(string.format("PT2_LINK_CUT_MIN_SHOTS 10: one wall (5 shots) keeps the link (%d points)", cut_n),
        keep_ok == cut_n and cut_n > 0, keep_ok)
end

-- ── 4b. PT2_LINK_CLEAR_FRONT: v2's chains = the old evaluator's ──────────
print("PT2_LINK_CLEAR_FRONT: v2 and old agree on the chain")
do
  local function old_ctx(inp, A)
    local ctx = { inp = inp, A = A, env = inp.env, reach_cache = {}, hostile = {} }
    for _, p in ipairs(inp.pills) do
      if (p.health or 0) > 0 and not p.in_tank and not inp.env.is_ally(p) then ctx.hostile[#ctx.hostile + 1] = p end
    end
    ctx.finish = OLD.finish_ok(inp, A)
    return ctx
  end
  local res = {}
  for _, on in ipairs({ true, false }) do
    local n, same, blocked = 0, 0, 0
    for trial = 1, 80 do
      clear()
      local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
      local pills = { A }
      for _ = 1, 4 + (trial % 8) do
        local mx, my = 122 + rnd(12), 114 + rnd(12)
        if math.abs(mx - 128) + math.abs(my - 120) > 1 then
          pills[#pills + 1] = { mx = mx, my = my, owner = "hostile", health = 15 }
        end
      end
      for _ = 1, 4 + rnd(12) do terrain[sq(120 + rnd(16), 112 + rnd(16))] = C.T_BUILDING end
      terrain[sq(128, 120)] = nil
      local world = make_world(pills)
      local inp = make_inp(world, A)
      inp.link_clear_front = on
      local vctx, octx = V._new_ctx(inp, A), old_ctx(inp, A)
      for deg = 0, 359, 3 do
        local cx = A.mx + 0.5 + math.sin(math.rad(deg)) * 7
        local cy = A.my + 0.5 - math.cos(math.rad(deg)) * 7
        local px, py = math.floor(cx * 256), math.floor(cy * 256)
        local rv = V._eval_point(vctx, px, py, nil, nil, false, false)
        inp.env.omx, inp.env.omy = math.floor(cx), math.floor(cy)
        local ro = OLD._eval_point(octx, px, py, nil, nil, nil, nil, false)
        if rv and ro then
          n = n + 1
          local eq = #rv.chain == #ro.chain
          if eq then for k = 1, #rv.chain do if rv.chain[k] ~= ro.chain[k] then eq = false end end end
          if eq then same = same + 1 end
          for k = 1, #rv.rays do
            local r = rv.rays[k]
            if r.link and r.front > 0 then blocked = blocked + 1; break end
          end
        end
      end
    end
    res[on] = { n = n, same = same, blocked = blocked }
    print(string.format("    knob %s: %d points, same chain %d, points with a blocked link %d",
      tostring(on), n, same, blocked))
  end
  check("knob on: every point has old's chain", res[true].n > 0 and res[true].same == res[true].n,
    res[true].n - res[true].same)
  -- the thin strip, with a planned wall on A's shell path in front of Y:
  -- knob on, the chain ends at A in both; knob off, old keeps {A, Y}
  clear()
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local Y = { mx = 128, my = 122, owner = "hostile", health = 15 }
  terrain[sq(126, 123)] = C.T_BUILDING
  terrain[sq(130, 123)] = C.T_BUILDING
  local pills = { A, Y }
  local world = make_world(pills)
  local cnt = { [true] = { n = 0, same = 0, cut = 0 }, [false] = { n = 0, same = 0, cut = 0 } }
  for _, on in ipairs({ true, false }) do
    local inp = make_inp(world, A)
    inp.link_clear_front = on
    inp.link_cut_min_shots = 0     -- the first E2 rule: any cover cuts
    local vctx, octx = V._new_ctx(inp, A), old_ctx(inp, A)
    for deg = 0, 359 do
      local cx = A.mx + 0.5 + math.sin(math.rad(deg)) * 7
      local cy = A.my + 0.5 - math.cos(math.rad(deg)) * 7
      local px, py = math.floor(cx * 256), math.floor(cy * 256)
      local r0 = V._eval_point(vctx, px, py, nil, nil, false, false)
      if r0 and #r0.chain == 2 then
        local ra = r0.rays[r0.ri[A]]
        for k = 1, ra.n_sq do
          local s = ra.sq[k]
          if ra.sq_t[k] < ra.link_t and s ~= sq(A.mx, A.my) then
            local extra = { [s] = 5 }
            local rv = V._eval_point(vctx, px, py, extra, nil, false, false)
            inp.env.omx, inp.env.omy = math.floor(cx), math.floor(cy)
            local ro = OLD._eval_point(octx, px, py, extra, nil, nil, nil, false)
            if rv and ro then
              local c = cnt[on]
              c.n = c.n + 1
              if #rv.chain == #ro.chain then c.same = c.same + 1 end
              if #ro.chain == 1 then c.cut = c.cut + 1 end
            end
          end
        end
      end
    end
  end
  print(string.format("    thin strip, a planned wall in front of Y: knob on %d points (old chain cut %d, v2 same %d);"
    .. " knob off %d points (old chain cut %d)", cnt[true].n, cnt[true].cut, cnt[true].same, cnt[false].n, cnt[false].cut))
  check("knob on: a wall in front of the link ends old's chain at A", cnt[true].n > 0 and cnt[true].cut == cnt[true].n,
    cnt[true].n - cnt[true].cut)
  check("knob on: v2 agrees", cnt[true].same == cnt[true].n, cnt[true].n - cnt[true].same)
  check("knob off: old keeps the link", cnt[false].n > 0 and cnt[false].cut == 0, cnt[false].cut)

  -- PT2_LINK_CUT_MIN_SHOTS (E2 thin cover, 10 = the chain bar): only cover
  -- of 10 shots or more in front of Y cuts the link. The shot counts are
  -- the ones shadow.cast uses: a forest is 1 shot, a wall env.wall_shots.
  local FOREST, WALL = 1, inp_wall_shots_of(world, A)
  check("the knob default is 10", C.PT2_LINK_CUT_MIN_SHOTS == 10, C.PT2_LINK_CUT_MIN_SHOTS)
  check("a wall is 5 shots", WALL == 5, WALL)
  clear()
  local Y2 = { mx = 128, my = 123, owner = "hostile", health = 15 }
  terrain[sq(126, 124)] = C.T_BUILDING
  terrain[sq(130, 124)] = C.T_BUILDING
  local w2 = make_world({ A, Y2 })
  local cases = {
    { name = "a forest (1 shot) in front of Y keeps the link", shots = { FOREST }, cut = false },
    { name = "one wall (5 shots) in front of Y keeps the link", shots = { WALL }, cut = false },
    { name = "two walls in a row (5 + 5 = 10 shots) in front of Y cut the link", shots = { WALL, WALL }, cut = true },
  }
  for _, cs in ipairs(cases) do
    local inp = make_inp(w2, A)
    inp.link_clear_front = true
    inp.link_cut_min_shots = C.PT2_LINK_CUT_MIN_SHOTS
    local vctx, octx = V._new_ctx(inp, A), old_ctx(inp, A)
    local n, want, same = 0, 0, 0
    for deg = 0, 359 do
      local cx = A.mx + 0.5 + math.sin(math.rad(deg)) * 7
      local cy = A.my + 0.5 - math.cos(math.rad(deg)) * 7
      local px, py = math.floor(cx * 256), math.floor(cy * 256)
      local r0 = V._eval_point(vctx, px, py, nil, nil, false, false)
      if r0 and #r0.chain == 2 then
        local ra = r0.rays[r0.ri[A]]
        local front = {}
        for k = 1, ra.n_sq do
          local s = ra.sq[k]
          if ra.sq_t[k] < ra.link_t and s ~= sq(A.mx, A.my) then front[#front + 1] = s end
        end
        if #front >= #cs.shots then
          local extra = {}
          for k = 1, #cs.shots do extra[front[k]] = cs.shots[k] end
          local rv = V._eval_point(vctx, px, py, extra, nil, false, false)
          inp.env.omx, inp.env.omy = math.floor(cx), math.floor(cy)
          local ro = OLD._eval_point(octx, px, py, extra, nil, nil, nil, false)
          if rv and ro then
            n = n + 1
            local cut = #ro.chain == 1
            if cut == cs.cut then want = want + 1 end
            if #rv.chain == #ro.chain then same = same + 1 end
          end
        end
      end
    end
    print(string.format("    %s: %d points, as wanted %d, v2 same %d", cs.name, n, want, same))
    check(cs.name, n > 0 and want == n, n - want)
    check(cs.name .. " (v2 agrees)", same == n, n - same)
  end
end

-- ── 4c. the group step: on a full tie the goal pill's plan stays ────────
print("group step: a full tie keeps the goal pill's own plan")
do
  -- Andrew approved Nov 23: inside one evaluate S0 breaks a full tie (the
  -- smaller angle, then the larger radius); the group step does not, so
  -- the goal pill's own plan (run first) wins over another first pill's
  local G = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local Y = { mx = 128, my = 123, owner = "hostile", health = 15 }
  local function plan(first, deg, score)
    return { safe = true, kills = 2, score = score, tier_min = 10, n_built = 1, depth = 3, blk = 1,
             leeway = 8, deg = deg, radius = 7, chain_mode = true, chain = { first, G },
             in_chain = { [first] = true, [G] = true } }
  end
  local inp = { leeway_min = 8, travel_band = 5, prefer_existing_cover = true, start_while_building = true }
  local goal_plan = plan(G, 90, 12)
  local y_plan = plan(Y, 10, 12)           -- the same keys, a smaller angle
  check("S0 alone would pick Y's plan (smaller angle)",
    V.better(y_plan, goal_plan, 8, 5, true, true) == true)
  check("full tie, two first pills: the goal pill's plan stays",
    V.group_takes(inp, G, y_plan, goal_plan) == false)
  check("full tie the other way: the plan the group has stays",
    V.group_takes(inp, G, goal_plan, y_plan) == false)
  check("a better plan of another first pill still takes over",
    V.group_takes(inp, G, plan(Y, 10, 11), goal_plan) == true)
  check("no plan yet: a safe chain with the goal pill takes it",
    V.group_takes(inp, G, y_plan, nil) == true)
  local no_goal = plan(Y, 10, 11); no_goal.in_chain = { [Y] = true }
  check("a chain without the goal pill never takes it",
    V.group_takes(inp, G, no_goal, goal_plan) == false)
end

-- ── 5. wall search against brute force ────────────────────────────────────
print("PT2_PREFER_EXISTING_COVER: fewer planned blockers first in the band")
do
  local function pt(score, n_built, depth, leeway)
    return { safe = true, kills = 2, score = score, tier_min = 10, n_built = n_built,
             depth = depth, blk = 1, leeway = leeway }
  end
  -- both in the band (10 + 5): the deeper point wins with the knob off,
  -- the point with no planned blocker wins with it on
  local walls, none = pt(12, 2, 3, 8), pt(14, 0, 2, 8)
  check("off: depth first", V.better(walls, none, 8, 5, false) == true)
  check("on: no planned blocker first", V.better(none, walls, 8, 5, true) == true)
  check("on: the other way round is false", V.better(walls, none, 8, 5, true) == false)
  -- a dropped pill counts as one planned blocker
  local one = pt(12, 1, 3, 8)
  check("on: one planned blocker beats two", V.better(one, walls, 8, 5, true) == true)
  -- out of the band: the old order (quicker first)
  local far = pt(20, 0, 3, 8)
  check("on: out of the band, the band point wins", V.better(walls, far, 8, 5, true) == true)
  -- safe and kills still come first
  local unsafe = pt(12, 0, 3, 8); unsafe.safe = false
  check("on: safe before fewer blockers", V.better(walls, unsafe, 8, 5, true) == true)
  local single = pt(12, 0, 0, 8); single.kills = 1
  check("on: kills before fewer blockers", V.better(walls, single, 8, 5, true) == true)
  -- no band: the knob does nothing
  check("on, no band: old order", V.better(walls, none, 8, 0, true) == OLD.better(walls, none, 8, 0))
  check("knob_inp: on by default", V.knob_inp(C, {}).prefer_existing_cover == (C.PT2_PREFER_EXISTING_COVER ~= false))
end

print("wall search against brute force")
do
  local tried, agree, opt_ok, n_sol = 0, 0, 0, 0
  for trial = 1, 400 do
    clear()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local pills = { A }
    for _ = 1, rnd(4) do
      local mx, my = 122 + rnd(12), 114 + rnd(12)
      if math.abs(mx - 128) + math.abs(my - 120) > 2 then
        pills[#pills + 1] = { mx = mx, my = my, owner = "hostile", health = 15 }
      end
    end
    for _ = 1, rnd(12) do terrain[sq(120 + rnd(16), 112 + rnd(16))] = C.T_BUILDING end
    terrain[sq(128, 120)] = nil
    local world = make_world(pills)
    local npb = rnd(2) - 1
    local inp = make_inp(world, A, npb)
    inp.build_max = 2 + (trial % 2)
    local ctx = V._new_ctx(inp, A)
    local deg = rnd(360)
    local cx = A.mx + 0.5 + math.sin(math.rad(deg)) * 7
    local cy = A.my + 0.5 - math.cos(math.rad(deg)) * 7
    local px, py = math.floor(cx * 256), math.floor(cy * 256)
    local rec = V._eval_point(ctx, px, py, nil, nil, false, false)
    if rec and not rec.covered then
      rec.omx = math.floor(cx + math.sin(math.rad(deg)) * 2.25)
      rec.omy = math.floor(cy - math.cos(math.rad(deg)) * 2.25)
      tried = tried + 1
      local aims = V._clear_aims_of(ctx, rec)
      local ai = aims[rnd(#aims)]
      local s = V._search_walls(ctx, rec, ai)
      -- brute force: every legal square of the uncovered rays (the search's
      -- own legal set: slot rules), every set up to build_max, pills first
      local cand, seen = {}, {}
      local lane = {}
      for _, q in ipairs(rec.lanes[ai] or {}) do lane[q] = true end
      for _, ui in ipairs(rec.uncovered) do
        local r = rec.rays[ui]
        for k = 1, r.n_sq do
          local q = r.sq[k]
          if not seen[q] then
            seen[q] = true
            local mx, my = q % 256, math.floor(q / 256)
            local tt = U.ttype(mx, my)
            if not lane[q] and tt ~= C.T_BUILDING and not inp.env.pill_on(mx, my) then
              local cxq, cyq = mx * 256 + 128, my * 256 + 128
              local okd = (cxq - px) ^ 2 + (cyq - py) ^ 2 >= 256 * 256
              local sx, sy = r.X.mx * 256 + 128, r.X.my * 256 + 128
              local before = (cxq - sx) ^ 2 + (cyq - sy) ^ 2 < (px - sx) ^ 2 + (py - sy) ^ 2
              local bl = r.link and r.sq_t[k] <= r.link_t
              if okd and before and not bl then cand[#cand + 1] = q end
            end
          end
        end
      end
      local best_k = nil
      local function covers(extra)
        local f = V._eval_point(ctx, px, py, extra, ai, false, true)
        return f and f.covered and #f.chain == #rec.chain
      end
      local nc = #cand
      for k = 1, inp.build_max do
        if best_k then break end
        local p = math.min(npb, k)
        local idx = {}
        local function rec_sets(start, depth)
          if best_k then return end
          if depth > k then
            -- kinds: the first p (in every way) pills
            local function kinds(i, pl, extra)
              if best_k then return end
              if i > k then
                if pl == 0 and covers(extra) then best_k = k end
                return
              end
              local q = cand[idx[i]]
              if pl > 0 then extra[q] = 15; kinds(i + 1, pl - 1, extra) end
              if k - i >= pl then extra[q] = 5; kinds(i + 1, pl, extra) end
              extra[q] = nil
            end
            kinds(1, p, {})
            return
          end
          for i = start, nc do idx[depth] = i; rec_sets(i + 1, depth + 1) end
        end
        rec_sets(1, 1)
      end
      if (s and s.k) == best_k then agree = agree + 1 end
      if s then
        n_sol = n_sol + 1
        local ex = {}
        for _, e in ipairs(s.items) do ex[e.sq] = e.shots end
        if covers(ex) then opt_ok = opt_ok + 1 end
      end
    end
  end
  print(string.format("    %d uncovered points, %d with a plan", tried, n_sol))
  check("the search finds the fewest blockers the brute force finds", agree == tried and tried > 20, agree)
  check("every plan covers the point (exact cast)", opt_ok == n_sol, opt_ok)
end

-- ── 6. wedges kept and made again ─────────────────────────────────────────
print("wedges kept while nothing changes")
do
  clear()
  V.reset()
  local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
  local Y = { mx = 131, my = 124, owner = "hostile", health = 15 }
  local pills = { A, Y }
  terrain[sq(127, 124)] = C.T_BUILDING
  local world = make_world(pills)
  local inp = make_inp(world, A)
  local r1 = V.evaluate_lua(inp)
  local n1 = V.stats.wedge_new
  local r2 = V.evaluate_lua(inp)
  check("second run makes no wedge", V.stats.wedge_new == n1 and V.stats.wedge_keep > 0,
        V.stats.wedge_new - n1)
  check("second run has the same best", r1.best and r2.best and r1.best.deg == r2.best.deg
        and r1.best.safe == r2.best.safe, r2.best and r2.best.deg)
  terrain[sq(129, 123)] = C.T_BUILDING
  V.evaluate_lua(inp)
  check("a new wall in range makes the wedges again", V.stats.wedge_new > n1, V.stats.wedge_new - n1)
  local n3 = V.stats.wedge_new
  Y.health = 0
  V.evaluate_lua(inp)
  check("a dead pill makes the wedges again", V.stats.wedge_new > n3, V.stats.wedge_new - n3)
end

-- ── 7. evaluate: the best passes the exact tests ──────────────────────────
print("evaluate: the best passes v2's own exact tests")
do
  local n, safe, bad = 0, 0, 0
  for trial = 1, 20 do
    clear()
    V.reset()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local pills = { A }
    for _ = 1, rnd(3) do
      local mx, my = 122 + rnd(12), 114 + rnd(12)
      if math.abs(mx - 128) + math.abs(my - 120) > 2 then
        pills[#pills + 1] = { mx = mx, my = my, owner = rnd(4) == 1 and "friendly" or "hostile", health = 15 }
      end
    end
    for _ = 1, rnd(25) do terrain[sq(120 + rnd(16), 112 + rnd(16))] = C.T_BUILDING end
    terrain[sq(128, 120)] = nil
    local world = make_world(pills)
    local inp = make_inp(world, A, rnd(2) - 1)
    inp.travel_band = (trial % 2 == 0) and 6.25 or 0
    local res = V.evaluate_lua(inp)
    n = n + 1
    local b = res.best
    if b and b.safe then
      safe = safe + 1
      local why = V.judge(inp, b.px, b.py, b.extra, b.aim_idx)
      if why ~= "-" then bad = bad + 1; print("    judge: " .. tostring(why)) end
    end
  end
  print(string.format("    %d maps, %d safe", n, safe))
  check("every safe best passes judge", bad == 0 and safe > 0, bad)
end

-- ── 8. PT2_AIM_POINTS = 9: the edge midpoints ────────────────────────────
print("PT2_AIM_POINTS: 9 aims")
do
  check("AIMS9 has 9 aims", #V.AIMS9 == 9, #V.AIMS9)
  local same = true
  for i = 1, 5 do if V.AIMS9[i] ~= OLD.AIMS[i] then same = false end end
  check("AIMS9 starts with the old 5", same)
  local mids = { { 128, 24 }, { 231, 128 }, { 128, 231 }, { 24, 128 } }
  local ok = true
  for i = 1, 4 do
    local a = V.AIMS9[5 + i]
    if a[1] ~= mids[i][1] or a[2] ~= mids[i][2] then ok = false end
  end
  check("AIMS9 6..9 are the edge midpoints at inset 24", ok)
  local n, safe5, safe9, bad, mid = 0, 0, 0, 0, 0
  for trial = 1, 20 do
    clear()
    V.reset()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local pills = { A }
    for _ = 1, rnd(3) do
      local mx, my = 122 + rnd(12), 114 + rnd(12)
      if math.abs(mx - 128) + math.abs(my - 120) > 2 then
        pills[#pills + 1] = { mx = mx, my = my, owner = "hostile", health = 15 }
      end
    end
    for _ = 1, 20 + rnd(25) do terrain[sq(120 + rnd(16), 112 + rnd(16))] = C.T_BUILDING end
    terrain[sq(128, 120)] = nil
    local world = make_world(pills)
    local inp5 = make_inp(world, A, 0)
    inp5.aim_points = 5
    local r5 = V.evaluate_lua(inp5)
    V.reset()
    local inp9 = make_inp(world, A, 0)
    inp9.aim_points = 9
    local r9 = V.evaluate_lua(inp9)
    n = n + 1
    if r5.best and r5.best.safe then safe5 = safe5 + 1 end
    local b = r9.best
    if b and b.safe then
      safe9 = safe9 + 1
      if b.aim_idx > 5 then mid = mid + 1 end
      local why = V.judge(inp9, b.px, b.py, b.extra, b.aim_idx)
      if why ~= "-" then bad = bad + 1; print("    judge: " .. tostring(why)) end
    end
  end
  print(string.format("    %d maps: safe with 5 aims %d, with 9 aims %d (a midpoint aim %d)", n, safe5, safe9, mid))
  check("9 aims: never fewer safe takes than 5", safe9 >= safe5, safe9 - safe5)
  check("9 aims: every safe best passes judge", bad == 0, bad)
  check("knob_inp: PT2_AIM_POINTS", V.knob_inp(C, {}).aim_points == (C.PT2_AIM_POINTS or 5))
end

-- ── 9. PT2_START_WHILE_BUILDING ────────────────────────────────────────────
print("PT2_START_WHILE_BUILDING: start check and rank")
do
  local PS = require("pt2_start")
  local function pt(score, start_now)
    return { safe = true, kills = 2, score = score, tier_min = 10, n_built = 1,
             depth = 3, blk = 1, leeway = 8, start_now = start_now }
  end
  local go, wait = pt(14, true), pt(12, false)
  check("swb on: a start-now point ranks first in the band", V.better(go, wait, 8, 5, true, true) == true)
  check("swb off: the old order", V.better(go, wait, 8, 5, true, false) == OLD.better(go, wait, 8, 5))
  local far = pt(20, true)
  check("swb on: out of the band no change", V.better(wait, far, 8, 5, true, true) == true)
  local n, planned, go_n, wait_n, bys, bad = 0, 0, 0, 0, 0, 0
  for trial = 1, 30 do
    clear()
    V.reset()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local pills = { A }
    for _ = 1, rnd(3) do
      local mx, my = 122 + rnd(12), 114 + rnd(12)
      if math.abs(mx - 128) + math.abs(my - 120) > 2 then
        pills[#pills + 1] = { mx = mx, my = my, owner = "hostile", health = 15, anger = (rnd(4) - 1) / 3 }
      end
    end
    for _ = 1, rnd(10) do terrain[sq(120 + rnd(16), 112 + rnd(16))] = C.T_BUILDING end
    terrain[sq(128, 120)] = nil
    local world = make_world(pills)
    local inp = make_inp(world, A, 0)
    inp.start_while_building = true
    inp.start_anger_rise = false
    inp.armour = 40
    local res = V.evaluate_lua(inp)
    n = n + 1
    local b = res.best
    if b and b.safe and #b.plan > 0 then
      planned = planned + 1
      if b.start_why == "bystander" then bys = bys + 1
      else
        -- the same answer by hand from pt2_start
        local shells = 0
        local ox, oy = b.omx, b.omy
        local exp = PS.exposure_ticks(ox, oy, b.plan)
        for _, sp in ipairs(b.start_pills or {}) do
          local X
          for _, p in ipairs(pills) do if p.mx == sp.mx and p.my == sp.my then X = p end end
          shells = shells + PS.shells_through(exp, PS.pill_interval(X.anger or 0, inp.anger_bump), sp.cover_now)
        end
        if shells ~= b.shells_through or math.abs(exp - b.exposure) > 1e-6
           or PS.can_start(40, shells, inp.finish_min_armour) ~= b.start_now then bad = bad + 1 end
        if b.start_now then go_n = go_n + 1 else wait_n = wait_n + 1 end
        -- no armour: a point with shells through must wait
        local inp0 = make_inp(world, A, 0)
        inp0.start_while_building = true
        inp0.start_anger_rise = false
        inp0.armour = 0
        V.reset()
        local b0 = V.evaluate_lua(inp0).best
        if b0 and b0.safe and (b0.shells_through or 0) > 0 and b0.start_now then bad = bad + 1 end
      end
    elseif b and b.safe and b.start_now ~= true then
      bad = bad + 1
    end
  end
  print(string.format("    %d maps, %d with a plan: start %d, wait %d, bystander %d", n, planned, go_n, wait_n, bys))
  check("start check = pt2_start by hand, no plan = start", bad == 0, bad)
end

-- ── 9b. PT2_START_ANGER_RISE ───────────────────────────────────────────────
print("PT2_START_ANGER_RISE: the tick walk over the chain")
local PS = require("pt2_start")
do
  local n, walked, go_off, go_on, more, less, bad = 0, 0, 0, 0, 0, 0, 0
  for trial = 1, 30 do
    clear()
    V.reset()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15, anger = (rnd(4) - 1) / 3 }
    local pills = { A }
    for _ = 1, rnd(3) do
      local mx, my = 122 + rnd(12), 114 + rnd(12)
      if math.abs(mx - 128) + math.abs(my - 120) > 2 then
        pills[#pills + 1] = { mx = mx, my = my, owner = "hostile", health = 15, anger = (rnd(4) - 1) / 3 }
      end
    end
    for _ = 1, rnd(10) do terrain[sq(120 + rnd(16), 112 + rnd(16))] = C.T_BUILDING end
    terrain[sq(128, 120)] = nil
    local world = make_world(pills)
    local function run(rise)
      local inp = make_inp(world, A, 0)
      inp.start_while_building = true
      inp.start_anger_rise = rise
      inp.armour = 40
      V.reset()
      return V.evaluate_lua(inp).best, inp
    end
    local b0 = run(false)
    local b1, inp = run(true)
    n = n + 1
    -- the rank does not see the start check's shells inside the band
    -- only through start_now; the point may move, so compare like points
    if b0 and b0.start_now then go_off = go_off + 1 end
    if b1 and b1.start_now then go_on = go_on + 1 end
    if b1 and b1.safe and b1.start_chain then
      walked = walked + 1
      -- by hand: the chain's pills, the wall ticks of the plan, the flight
      local ts = PS.wall_ticks(b1.omx, b1.omy, b1.plan)
      local wt = {}
      for i, q in ipairs(b1.plan) do wt[q.my * 256 + q.mx] = ts[i] end
      local entries = {}
      for k, c in ipairs(b1.start_chain) do
        local X = b1.chain[k]
        if X.mx ~= c.mx or X.my ~= c.my then bad = bad + 1 end
        if (k < #b1.chain) ~= (c.to == k + 1) then bad = bad + 1 end
        entries[k] = { speed = PS.pill_interval(X.anger or 0, inp.anger_bump, nil, nil, nil, true),
                       to = c.to, cover_now = c.cover_now, planned = c.planned, count = c.count }
      end
      local fl = PS.flight_ticks(b1.px, b1.py, A.mx, A.my)
      local sh = PS.rise_chain(entries, b1.exposure, function(mx, my) return wt[my * 256 + mx] end,
                               { tank_reload = 13, flight = fl })
      if sh ~= b1.shells_through then bad = bad + 1 end
      if PS.can_start(40, sh, inp.finish_min_armour) ~= b1.start_now then bad = bad + 1 end
      if b0 and b0.omx == b1.omx and b0.omy == b1.omy and b0.shells_through then
        if sh > b0.shells_through then more = more + 1 elseif sh < b0.shells_through then less = less + 1 end
      end
    end
  end
  print(string.format("    %d maps, %d walked: start off %d, on %d; same point: more shells %d, fewer %d",
    n, walked, go_off, go_on, more, less))
  check("the walk = pt2_start.rise_chain by hand", bad == 0, bad)
  check("some maps walk the chain", walked > 0, walked)
end

-- ── 10. PT2_EXTRA_COVER_WALLS ─────────────────────────────────────────────
print("PT2_EXTRA_COVER_WALLS: extra walls after the rank")
do
  local n, with, bad, same = 0, 0, 0, 0
  for trial = 1, 30 do
    clear()
    V.reset()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local pills = { A }
    for _ = 1, rnd(3) do
      local mx, my = 122 + rnd(12), 114 + rnd(12)
      if math.abs(mx - 128) + math.abs(my - 120) > 2 then
        pills[#pills + 1] = { mx = mx, my = my, owner = "hostile", health = 15 }
      end
    end
    for _ = 1, rnd(20) do terrain[sq(120 + rnd(16), 112 + rnd(16))] = C.T_BUILDING end
    terrain[sq(128, 120)] = nil
    local world = make_world(pills)
    local i0 = make_inp(world, A, 0)
    i0.extra_cover_walls = 0
    -- room under PT2_BUILD_MAX for the extra wall: a bar of 10 (two walls)
    -- leaves one square of the three on a plain line
    i0.bar_single, i0.bar_chain = 10, 10
    local b0 = V.evaluate_lua(i0).best
    V.reset()
    local i1 = make_inp(world, A, 0)
    i1.extra_cover_walls = 1
    i1.bar_single, i1.bar_chain = 10, 10
    local b1 = V.evaluate_lua(i1).best
    n = n + 1
    if b0 and b1 and b0.px == b1.px and b0.py == b1.py and b0.aim_idx == b1.aim_idx then same = same + 1 end
    if b0 and b0.extra_cover then bad = bad + 1 end
    if b1 and b1.safe and b1.extra_cover then
      with = with + 1
      local e = b1.extra_cover
      if #e > 1 or #b1.plan + #e > i1.build_max then bad = bad + 1 end
      -- on the last chain pill's ray, not on the plan, the point still passes judge
      local last = b1.chain[#b1.chain]
      local r = b1.rays[b1.ri[last]]
      local on = false
      for k = 1, r.n_sq do if r.sq[k] == e[1].sq then on = true end end
      for _, q in ipairs(b1.plan) do if q.sq == e[1].sq then on = false end end
      if not on then bad = bad + 1 end
      local ex = {}
      for q, v in pairs(b1.extra) do ex[q] = v end
      ex[e[1].sq] = e[1].shots
      if V.judge(i1, b1.px, b1.py, ex, b1.aim_idx) ~= "-" then bad = bad + 1 end
    end
  end
  print(string.format("    %d maps: best the same %d, an extra wall %d", n, same, with))
  check("the rank does not see the extra wall", same == n, n - same)
  check("extra walls legal, on the last chain ray, judge passes", bad == 0 and with > 0, bad)
end

-- PT2_EXTRA_COVER_SKIP_PILL_LINE (Nov 24, run_c12 gap_n): our pill P
-- (122,118) on A's (124,118) ray already blocks it, so no extra wall
-- (123,118) goes between them.
print("PT2_EXTRA_COVER_SKIP_PILL_LINE: no extra wall on a friendly pill's line")
do
  local A = { mx = 124, my = 118 }
  local ray = { n_sq = 3, sq = { sq(123, 118), sq(122, 118), sq(121, 118) },
                n_block = 0, block = {} }
  local drop = { sq = sq(122, 118), mx = 122, my = 118, shots = 5, kind = "pill" }
  local wall = { sq = sq(122, 118), mx = 122, my = 118, shots = 5, kind = "wall" }
  local function rec(plan, r)
    return { chain = { A }, ri = { [A] = 1 }, rays = { r }, plan = plan }
  end
  check("pill_on_ray: a planned pill drop on the ray", V._pill_on_ray(rec({ drop }, ray), ray) == true)
  check("pill_on_ray: a planned wall is not a pill", V._pill_on_ray(rec({ wall }, ray), ray) == false)
  local off = { sq = sq(122, 119), mx = 122, my = 119, shots = 5, kind = "pill" }
  check("pill_on_ray: a pill drop off the ray", V._pill_on_ray(rec({ off }, ray), ray) == false)
  local placed = { n_sq = 3, sq = ray.sq, n_block = 1,
                   block = { { sq = sq(122, 118), shots = 5, kind = "ally_pill", t = 9 } } }
  check("pill_on_ray: a placed friendly pill on the ray", V._pill_on_ray(rec({}, placed), placed) == true)
  local ctx = { inp = { extra_cover_walls = 1, build_max = 4, xcover_skip_pill_line = true } }
  check("extra_cover: none on a ray our pill blocks", V._extra_cover(ctx, rec({ drop }, ray)) == nil)
end

print("E4-7: a bystander's standing cover")
do
  -- A (in the chain, covered by its link) and a bystander B whose ray has
  -- a planned wall. B's standing cover 5 meets its bar 5: start; 0: wait.
  local A, B = { mx = 10, my = 10 }, { mx = 20, my = 10 }
  local function rec_with(now_shots)
    local blockB = { { kind = "planned", sq = 10 * 256 + 15, shots = 5 } }
    if now_shots > 0 then blockB[2] = { kind = "wall", sq = 10 * 256 + 16, shots = now_shots } end
    return { px = 15 * 256, py = 12 * 256, omx = 15, omy = 12, T = { A, B }, in_chain = { [A] = 1 },
             plan = { { mx = 15, my = 10, sq = 10 * 256 + 15, shots = 5, kind = "wall" } },
             rays = { { n_block = 0, block = {}, link_cover = true, bar = 0, hit = "tank" },
                      { n_block = #blockB, block = blockB, bar = 5, hit = "tank" } } }
  end
  local ctx = { inp = { armour = 40, finish_min_armour = 5, anger_bump = 0.3333 } }
  local r1 = rec_with(5)
  V._start_check(ctx, r1)
  check("standing cover 5, bar 5: start", r1.start_now == true and r1.start_why == nil, tostring(r1.start_why))
  local r2 = rec_with(0)
  V._start_check(ctx, r2)
  check("standing cover 0, bar 5: wait (bystander)", r2.start_now == false and r2.start_why == "bystander",
    tostring(r2.start_why))
end

print("E5-13: a group run's extra walls, once on the final result")
do
  local n, same, with = 0, 0, 0
  local function xs(b)
    local o = {}
    for i, e in ipairs((b and b.extra_cover) or {}) do o[i] = tostring(e.sq) end
    return table.concat(o, ",")
  end
  for trial = 1, 30 do
    clear()
    V.reset()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local pills = { A }
    for _ = 1, 1 + rnd(3) do
      local mx, my = 122 + rnd(12), 114 + rnd(12)
      if math.abs(mx - 128) + math.abs(my - 120) > 2 then
        pills[#pills + 1] = { mx = mx, my = my, owner = "hostile", health = 15 }
      end
    end
    for _ = 1, rnd(20) do terrain[sq(120 + rnd(16), 112 + rnd(16))] = C.T_BUILDING end
    terrain[sq(128, 120)] = nil
    local world = make_world(pills)
    local inp = make_inp(world, A, 0)
    inp.extra_cover_walls, inp.chains = 1, true
    inp.bar_single, inp.bar_chain = 10, 10
    local res = V.evaluate_group(inp)
    -- the result's own run, alone: its extra walls as before E5-13
    V.reset()
    local i2 = make_inp(world, res.A or A, 0)
    i2.extra_cover_walls = 1
    i2.bar_single, i2.bar_chain = 10, 10
    i2.chains_only = (res.A and res.A ~= A) and i2.chains_only_runs or nil
    local b2 = V.evaluate_lua(i2).best
    n = n + 1
    local b = res.best
    if b and b.extra_cover then with = with + 1 end
    if (not b and not b2) or (b and b2 and b.px == b2.px and b.py == b2.py and xs(b) == xs(b2)) then
      same = same + 1
    end
  end
  print(string.format("    %d maps: same extra walls %d, with extra walls %d", n, same, with))
  check("the group's extra walls = the result run's own", same == n and with > 0, n - same)
end

-- ── 11. PT2_AIM_AVOID_CHAIN_NEIGHBOUR ────────────────────────────────────
-- pt2_grass_double_take_demo n1 (Nov 24): pills A (125,118) and B
-- (126,118) side by side, the tank south-west, chain B-A. B's bottom-left
-- corner (aim 4) touches A: shells to it graze A. Its bottom-right corner
-- (aim 5) is free.
print("PT2_AIM_AVOID_CHAIN_NEIGHBOUR: an aim beside a chain member last")
do
  local PA = { mx = 125, my = 118, owner = "hostile", health = 15 }
  local PB = { mx = 126, my = 118, owner = "hostile", health = 15 }
  -- beside: per aim, on B with A in the chain
  local world = make_world({ PB, PA })
  local inp = make_inp(world, PB, 0)
  inp.aim_points = 9
  local ctx = V._new_ctx(inp, PB)
  local set = V._nb_set(ctx, { chain = { PB, PA } })
  local want = { false, true, false, true, false, false, false, false, true }
  local ok = true
  for ai = 1, 9 do if V._aim_beside(ctx, ai, set) ~= want[ai] then ok = false end end
  check("beside A: TL, BL and the left edge only", ok)
  check("a single take with no cover: never beside", V._nb_set(ctx, { chain = { PB } }) == nil
        and V._aim_beside(ctx, 4, nil) == false)
  local dset = { [119 * 256 + 125] = true }   -- a wall at B's bottom-left diagonal
  check("the diagonal counts for a corner", V._aim_beside(ctx, 4, dset) and not V._aim_beside(ctx, 8, dset)
        and not V._aim_beside(ctx, 5, dset))
  -- the rank: same safe and kills, beside last (even with less walls)
  local function pt(aimnb, n_built)
    return { safe = true, kills = 2, score = 10, tier_min = 10, n_built = n_built,
             depth = 3, blk = 1, leeway = 8, aimnb = aimnb, deg = 0, radius = 7 }
  end
  check("rank: a free aim beats a beside aim", V.better(pt(false, 2), pt(true, 1), 8, 5, true, true) == true
        and V.better(pt(true, 1), pt(false, 2), 8, 5, true, true) == false)
  local un = pt(false, 1); un.safe = false
  check("rank: safe first (a beside safe point wins)", V.better(pt(true, 1), un, 8, 5, true, true) == true)
  local k1 = pt(false, 1); k1.kills = 1
  check("rank: kills first (a beside 2-kill point wins)", V.better(pt(true, 1), k1, 8, 5, true, true) == true)
  check("rank: knob off (aimnb nil) = the old order",
        V.better(pt(nil, 2), pt(nil, 1), 8, 5, true, true) == false)
  -- evaluate: walls at (123,119) and (124,121); the best point (deg 232)
  -- is the same both ways, its aim is not
  local function run(on)
    clear(); V.reset()
    terrain[sq(123, 119)] = C.T_BUILDING
    terrain[sq(124, 121)] = C.T_BUILDING
    local i2 = make_inp(make_world({ PB, PA }), PB, 0)
    i2.aim_points = 9
    i2.aim_avoid_nb = on
    i2.score_fn = function(deg, mx, my, cx, cy)
      local dx, dy = cx - 120.5, cy - 124.5
      return math.sqrt(dx * dx + dy * dy)
    end
    return V.evaluate_lua(i2).best
  end
  local off, on = run(false), run(true)
  check("knob off: the old pick stands (BL, beside A)", off and off.safe and off.kills == 2
        and off.aim_idx == 4 and off.aimnb == nil, off and off.aim_idx)
  check("knob on: the free corner wins (BR, not beside)", on and on.safe and on.kills == 2
        and on.aim_idx == 5 and on.aimnb == false, on and on.aim_idx)
  check("knob on: the same point", on and off and on.px == off.px and on.py == off.py)
  check("knob_inp: PT2_AIM_AVOID_CHAIN_NEIGHBOUR",
        V.knob_inp(C, {}).aim_avoid_nb == (C.PT2_AIM_AVOID_CHAIN_NEIGHBOUR ~= false))
  clear(); V.reset()
end

-- ── 12. PT2_FARTHEST_RING ─────────────────────────────────────────────────
print("PT2_FARTHEST_RING: the farthest ring that works")
do
  local function pt(radius, kills, safe, score)
    return { safe = safe, kills = kills, score = score, tier_min = 10, n_built = 1,
             depth = 3, blk = 1, leeway = 8, deg = 0, radius = radius }
  end
  local far, near = pt(7.4, 1, true, 14), pt(7.0, 1, true, 10)
  check("rank: the farther ring wins (same safe and kills)",
        V.better(far, near, 8, 5, true, true, false, true) == true
        and V.better(near, far, 8, 5, true, true, false, true) == false)
  check("rank: far off = the old order (travel first)",
        V.better(near, far, 8, 0, true, true, false, false) == OLD.better(near, far, 8, 0))
  local near2 = pt(7.0, 2, true, 10)
  check("rank: a nearer ring with more kills wins", V.better(near2, far, 8, 5, true, true, false, true) == true)
  local far_un = pt(7.4, 2, false, 10)
  check("rank: a safe nearer point beats an unsafe farther one",
        V.better(near2, far_un, 8, 5, true, true, false, true) == true)
  local fb, nb = pt(7.4, 1, true, 10), pt(7.0, 1, true, 10)
  fb.aimnb, nb.aimnb = true, false
  check("rank: the aim rule comes before the ring", V.better(nb, fb, 8, 5, true, true, false, true) == true)
  -- evaluate: one pill on open grass, walls planned; with far on the
  -- best is on the 7.4 ring with the same kills as the old 7.0 ring
  local function run(far_on, rr)
    clear(); V.reset()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local inp = make_inp(make_world({ A }), A, 0)
    inp.radius = 7.0
    inp.farthest_ring = far_on
    inp.ring_radii = rr or { 7.4, 7.2, 7.0, 6.6 }
    return V.evaluate_lua(inp).best
  end
  local off, on = run(false), run(true)
  check("knob off: the old ring (7.0)", off and off.safe and off.radius == 7.0, off and off.radius)
  check("knob on: the 7.4 ring wins, same kills", on and on.safe and on.radius == 7.4
        and off and on.kills == off.kills, on and on.radius)
  local e = run(true, {})
  check("an empty ring list: the old rings", e and e.radius == 7.0, e and e.radius)
  check("far_on needs the flag and a list", V.far_on({ farthest_ring = true, ring_radii = { 7 } })
        and not V.far_on({ farthest_ring = true, ring_radii = {} })
        and not V.far_on({ farthest_ring = false, ring_radii = { 7 } }))
  check("knob_inp: PT2_FARTHEST_RING", V.knob_inp(C, {}).farthest_ring == (C.PT2_FARTHEST_RING ~= false)
        and #V.knob_inp(C, {}).ring_radii == #(C.PT2_FARTHEST_RING_RADII or { 1, 2, 3, 4 }))
  -- two pills, P at (126,119): the 7.4 ring has safe single takes but no
  -- safe chain; a nearer ring's chain (2 kills) wins
  do
    clear(); V.reset()
    local A = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local P = { mx = 126, my = 119, owner = "hostile", health = 15 }
    local inp = make_inp(make_world({ A, P }), A, 0)
    inp.radius = 7.0
    inp.farthest_ring = true
    inp.ring_radii = { 7.4, 7.2, 7.0, 6.6 }
    inp.keep_all = true
    local r = V.evaluate_lua(inp)
    local single74, chain74 = false, false
    for _, x in ipairs(r.all or {}) do
      if x.safe and x.radius == 7.4 then
        if x.kills == 1 then single74 = true else chain74 = true end
      end
    end
    local b = r.best
    check("the 7.4 ring: a safe single take, no safe chain", single74 and not chain74)
    check("a nearer ring with more kills wins", b and b.safe and b.kills == 2 and b.radius < 7.4,
          b and string.format("%s/%s", tostring(b.radius), tostring(b.kills)))
  end
  clear()
end

-- ── 13. PT2_FAVOR_BLOCKER_PLACE ───────────────────────────────────────────
print("PT2_FAVOR_BLOCKER_PLACE: blocker pills near the target, walls near the tank")
do
  local A = { mx = 128, my = 120 }
  local function ctx_of(carried, on)
    return { A = A, inp = { favor_blocker_place = on ~= false, carried_pills = carried } }
  end
  -- firing square (128,130); plan items as v2 makes them
  local function fin(plan, ally)
    local f = { px = 128.5 * 256, py = 130.5 * 256, plan = plan or {}, chain = { A } }
    if ally then
      local blk = {}
      for i, q in ipairs(ally) do blk[i] = { sq = q[2] * 256 + q[1], kind = "ally_pill", shots = 1 } end
      f.ri = { [A] = 1 }
      f.rays = { { n_block = #blk, block = blk } }
    end
    return f
  end
  local function P(mx, my) return { sq = my * 256 + mx, mx = mx, my = my, shots = 1, kind = "pill" } end
  local function W(mx, my) return { sq = my * 256 + mx, mx = mx, my = my, shots = 1, kind = "wall" } end
  local function tier(carried, f) V._blocker_place(ctx_of(carried), f); return f.bp_tier, f.wall_far end
  -- carrying a pill
  check("carrying: a near pill drop = tier 0", tier(1, fin({ P(129, 121) })) == 0)
  check("carrying: 2.0 squares is near (tier 0)", tier(1, fin({ P(130, 120) })) == 0)
  check("carrying: 2.24 squares is not near (tier 1)", tier(1, fin({ P(130, 121) })) == 1)
  check("carrying: one near and one far = tier 1", tier(2, fin({ P(128, 121), P(125, 120) })) == 1)
  check("carrying: walls only = tier 2", tier(1, fin({ W(128, 122) })) == 2)
  check("carrying: no blocker at all = tier 2", tier(1, fin({})) == 2)
  check("carrying: a near placed pill on a chain ray = tier 0", tier(1, fin({ W(128, 125) }, { { 127, 121 } })) == 0)
  check("carrying: a far placed pill on a chain ray = tier 1", tier(1, fin({}, { { 128, 124 } })) == 1)
  -- carrying none
  check("not carrying: no friendly pill = tier 0", tier(0, fin({ W(128, 122) })) == 0)
  check("not carrying: a near placed pill = tier 0", tier(0, fin({}, { { 129, 119 } })) == 0)
  check("not carrying: a far placed pill = tier 1", tier(0, fin({}, { { 129, 119 }, { 131, 120 } })) == 1)
  -- wall_far: the largest floored distance to (128,130)
  local _, wf = tier(0, fin({ W(128, 127), W(130, 128) }))
  check("wall_far: max of floor(3), floor(2.83)", wf == 3, wf)
  local _, wf2 = tier(1, fin({ P(128, 121) }))
  check("wall_far: no wall = 0", wf2 == 0, wf2)
  local fo = fin({ P(128, 121), W(128, 122) })
  V._blocker_place(ctx_of(1, false), fo)
  check("knob off: no bp_tier, no wall_far", fo.bp_tier == nil and fo.wall_far == nil)
  -- the rank
  local function pt(t, wfar, aimnb)
    return { safe = true, kills = 1, score = 10, tier_min = 10, n_built = 2, depth = 3, blk = 1,
             leeway = 8, deg = 0, radius = 7, bp_tier = t, wall_far = wfar, aimnb = aimnb }
  end
  check("rank: tier 0 beats tier 1 and tier 2",
        V.better(pt(0, 5), pt(1, 1), 8, 5, true, true) and V.better(pt(1, 5), pt(2, 1), 8, 5, true, true)
        and not V.better(pt(2, 1), pt(0, 5), 8, 5, true, true))
  check("rank: same tier, nearer walls win", V.better(pt(0, 2), pt(0, 3), 8, 5, true, true)
        and not V.better(pt(0, 3), pt(0, 2), 8, 5, true, true))
  check("rank: tier before the aim rule", V.better(pt(0, 3, true), pt(1, 3, false), 8, 5, true, true))
  check("rank: walls before the aim rule", V.better(pt(0, 2, true), pt(0, 3, false), 8, 5, true, true))
  local fr, nr = pt(0, 2), pt(0, 3)
  fr.radius, nr.radius = 6.6, 7.4
  check("rank: walls before the farther ring", V.better(fr, nr, 8, 5, true, true, false, true))
  local un = pt(0, 0); un.safe = false
  check("rank: safe first (a tier 2 safe point wins)", V.better(pt(2, 9), un, 8, 5, true, true))
  local k0 = pt(0, 0); k0.kills = 0
  check("rank: kills first (a tier 2 1-kill point wins)", V.better(pt(2, 9), k0, 8, 5, true, true))
  check("rank: knob off (nil) = the old order",
        V.better(pt(nil, nil), pt(nil, nil), 8, 5, true, true) == OLD.better(pt(nil, nil), pt(nil, nil), 8, 5))
  -- evaluate: one pill, one carried pill; every record keeps the rule
  local function run(on, carried)
    clear(); V.reset()
    local T = { mx = 128, my = 120, owner = "hostile", health = 15 }
    local inp = make_inp(make_world({ T }), T, carried > 0 and 1 or 0)
    inp.favor_blocker_place = on
    inp.carried_pills = carried
    inp.keep_all = true
    return V.evaluate_lua(inp)
  end
  for _, carried in ipairs({ 0, 1 }) do
    local r = run(true, carried)
    local b = r.best
    local ok = b ~= nil and b.bp_tier ~= nil
    for _, x in ipairs(r.all or {}) do
      if b and x ~= b and x.bp_tier and x.safe == b.safe and x.kills == b.kills then
        if x.bp_tier < b.bp_tier or (x.bp_tier == b.bp_tier and x.wall_far < b.wall_far) then ok = false end
      end
    end
    check("evaluate (carried " .. carried .. "): no point of the same safe and kills has a lower tier or nearer walls",
          ok, b and string.format("tier %s wfar %s", tostring(b.bp_tier), tostring(b.wall_far)))
  end
  local off = run(false, 1).best
  check("evaluate, knob off: no bp_tier", off and off.bp_tier == nil and off.wall_far == nil)
  check("knob_inp: PT2_FAVOR_BLOCKER_PLACE",
        V.knob_inp(C, {}).favor_blocker_place == (C.PT2_FAVOR_BLOCKER_PLACE ~= false))
  clear(); V.reset()
end

-- ── 14. BLOCKER_MIN_LANE_BRAD: v2 asks attack's shield lane width ────────
-- c13 gap_e (Nov 24): two pills side by side, the tank east of them. v2
-- picked a point whose fire aim had no clear side lane; attack.lua's
-- SHIELD_LANE check threw the plan away. With inp.min_lane_brad set, every
-- aim v2 calls clear has the lane width on both sides.
print("BLOCKER_MIN_LANE_BRAD: v2's clear aims are wide enough")
do
  local AL = require("aim_lane")
  local function narrow_count(mlb_in, mlb_test, far)
    clear(); V.reset()
    local P1 = { mx = 124, my = 118, owner = "hostile", health = 15 }
    local P2 = { mx = 126, my = 118, owner = "hostile", health = 15 }
    local inp = make_inp(make_world({ P1, P2 }), far and P1 or P2, 1)
    inp.carried_pills = 1
    inp.keep_all = true
    inp.min_lane_brad = mlb_in
    inp.score_fn = function(deg, mx, my, cx, cy)
      local dx, dy = cx - 138.5, cy - 118.5
      return math.sqrt(dx * dx + dy * dy) * 10
    end
    local r = V.evaluate_lua(inp)
    local n, bad = 0, 0
    for _, x in ipairs(r.all or {}) do
      if x.safe and x.aim_idx then
        n = n + 1
        local A = inp.A
        local fo = V.FIRE_AIMS[x.aim_idx]
        local dx, dy = A.mx * 256 + fo[1] - x.px, A.my * 256 + fo[2] - x.py
        local a0 = (math.atan2(dx, -dy) * 256 / (2 * math.pi)) % 256
        local ok = AL.wide_enough(function(a)
          return (SH.tank_shot_angle(inp.env, x.px, x.py, a, A.mx, A.my, x.extra))
        end, a0, mlb_test)
        if not ok then bad = bad + 1 end
      end
    end
    return n, bad, r.best
  end
  for _, far in ipairs({ false, true }) do
    local tag = far and "far pill" or "near pill"
    local n0, bad0 = narrow_count(0, 0.125, far)
    local n1, bad1, b1 = narrow_count(0.125, 0.125, far)
    print(string.format("    %s off: %d safe, %d narrow; on: %d safe, %d narrow", tag, n0, bad0, n1, bad1))
    check(tag .. " off: some safe aims are narrow (the c13 case)", bad0 > 0, bad0)
    check(tag .. " on: no safe aim is narrow", n1 > 0 and bad1 == 0, bad1)
    check(tag .. " on: still a safe best", b1 and b1.safe, b1 and b1.safe)
  end
  check("knob_inp: BLOCKER_MIN_LANE_BRAD", V.knob_inp(C, {}).min_lane_brad == (C.BLOCKER_MIN_LANE_BRAD or 0))
  clear()
end

-- ── 15. PT2_MIRROR_SAFE_TIE and PT2_QUICK_SIDE_FIRST ─────────────────────
-- c14 gap_e / gap_w (Nov 24): two pills (124,118) and (126,118), the tank
-- east or west of them on the mirror square. With the same travel on both
-- sides of the pill line, the old S0 (smaller angle) picks north of the
-- pills from the east but south of them from the west. With
-- PT2_MIRROR_SAFE_TIE the east angle and the west angle add up to 360.
print("PT2_MIRROR_SAFE_TIE / PT2_QUICK_SIDE_FIRST")
do
  clear()
  local function group(tx, ty, knobs, ban)
    local P1 = { mx = 124, my = 118, owner = "hostile", health = 15 }
    local P2 = { mx = 126, my = 118, owner = "hostile", health = 15 }
    local goal = tx > 125 and P2 or P1      -- the near pill is the goal
    local inp = make_inp(make_world({ P1, P2 }), goal, 1)
    inp.carried_pills = 1
    inp.aim_points = 9
    inp.prefer_existing_cover, inp.start_while_building = true, true
    inp.quick_side_first, inp.mirror_tie = knobs, knobs
    inp.tank_wx, inp.tank_wy = math.floor((tx + 0.5) * 256), math.floor((ty + 0.5) * 256)
    local function set_A(A)
      inp.banned = (ban and A.mx == ban.mx) and function(d) return d >= ban.lo and d <= ban.hi end or nil
    end
    inp.set_A = set_A
    set_A(goal)
    -- octile travel from the tank square, the same on both sides
    inp.score_fn = function(deg, mx, my)
      local dx, dy = math.abs(mx - tx), math.abs(my - ty)
      local mn, mxv = math.min(dx, dy), math.max(dx, dy)
      return 4 * ((mxv - mn) + mn * math.sqrt(2))
    end
    V.reset()
    local r = V.evaluate_group(inp)
    return r.best, r.A
  end
  local be, Ae = group(138, 118, true)
  local bw, Aw = group(112, 118, true)
  print(string.format("    on: east A=(%d,%d) deg %s, west A=(%d,%d) deg %s",
    Ae.mx, Ae.my, tostring(be and be.deg), Aw.mx, Aw.my, tostring(bw and bw.deg)))
  check("on: east hits the west pill first", be and be.safe and Ae.mx == 124, Ae.mx)
  check("on: west hits the east pill first", bw and bw.safe and Aw.mx == 126, Aw.mx)
  check("on: east deg + west deg = 360 (mirror plans)", be and bw and be.deg + bw.deg == 360,
    be and bw and (be.deg + bw.deg))
  local oe = group(138, 118, false)
  local ow = group(112, 118, false)
  print(string.format("    off: east deg %s, west deg %s", tostring(oe and oe.deg), tostring(ow and ow.deg)))
  check("off: the old S0 gives plans that are not mirrors", oe and ow and oe.deg + ow.deg ~= 360,
    oe and ow and (oe.deg .. "/" .. ow.deg))

  -- c13 gap_e (Nov 24): the tank east (139,118), goal (126,118). The plan
  -- at deg 99 on (124,118) failed its lane check, so 90..100 is banned on
  -- (124,118). The replan must stay east (a double take from the east on
  -- the west pill), not drive all the way to the west side.
  for _, knobs in ipairs({ true, false }) do
    local b, A = group(139, 118, knobs, { mx = 124, lo = 90, hi = 100 })
    local tag = knobs and "on" or "off"
    print(string.format("    c13 %s: A=(%d,%d) deg %s", tag, A.mx, A.my, tostring(b and b.deg)))
    check("c13 " .. tag .. ": replan hits the west pill (124,118) first", b and b.safe and A.mx == 124, A.mx)
    check("c13 " .. tag .. ": replan stays east, off the banned 90..100",
      b and b.deg and b.deg > 45 and b.deg < 135 and not (b.deg >= 90 and b.deg <= 100), b and b.deg)
  end

  -- PT2_QUICK_SIDE_FIRST: a plan with a lower blocker pill tier but far
  -- slower travel (out of the band) no longer wins on the tier alone.
  local function plan(score, tier, deg)
    return { safe = true, kills = 2, score = score, tier_min = 39, n_built = 1, depth = 15, blk = 1,
             leeway = 8, deg = deg, radius = 7.4, bp_tier = tier, wall_far = 0, aimnb = false,
             px = 0, py = 0 }
  end
  local near_side = plan(40, 2, 83)
  local far_side = plan(88, 1, 259)
  local band = 6.25
  check("off: the far side wins on blocker tier", V.better(far_side, near_side, 8, band, true, true, false, false) == true)
  check("on: the near side wins on travel (far side out of the band)",
    V.better(near_side, far_side, 8, band, true, true, false, false, { quick_side_first = true }) == true
    and V.better(far_side, near_side, 8, band, true, true, false, false, { quick_side_first = true }) == false)
  local close_side = plan(44, 1, 259)
  check("on: inside the band the tier still ranks first",
    V.better(close_side, near_side, 8, band, true, true, false, false, { quick_side_first = true }) == true)
  local ginp = { leeway_min = 8, travel_band = band, prefer_existing_cover = true, start_while_building = true,
                 quick_side_first = true }
  local G = { mx = 126, my = 118 }
  near_side.chain_mode, near_side.in_chain = true, { [G] = true }
  far_side.chain_mode, far_side.in_chain = true, { [G] = true }
  check("on: group step keeps the near side plan", V.group_takes(ginp, G, far_side, near_side) == false)
  ginp.quick_side_first = false
  check("off: group step takes the far side plan", V.group_takes(ginp, G, far_side, near_side) == true)

  local k = V.knob_inp(C, {})
  check("knob_inp: PT2_QUICK_SIDE_FIRST", k.quick_side_first == (C.PT2_QUICK_SIDE_FIRST ~= false))
  check("knob_inp: PT2_MIRROR_SAFE_TIE", k.mirror_tie == (C.PT2_MIRROR_SAFE_TIE ~= false))
  clear()
end

print(string.format("%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
