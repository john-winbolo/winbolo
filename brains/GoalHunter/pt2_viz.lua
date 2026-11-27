-- =========================================================================
-- pt2_viz.lua -- BrainTest layers for pill take v2 (2026-11-16)
--
-- capture() runs once per v2 evaluation (from pt2_attack.after_scan) in a
-- debug run when any pt2_* layer is on. It keeps v2's own result and does
-- the extra work some layers need (the shadow cells) at that time. draw()
-- runs every tick from attack.lua and draws the kept data, so a recorded
-- session shows the same shapes on every frame.
--
-- Every shape comes from the planner's own numbers: the rays are the
-- engine shell walk (shadow.lua shell_points, the same walk SH.cast uses),
-- the shadow cells are SH.cast on each cell with the best candidate's
-- planned blockers and each ray's own bar, the candidates and the leeway
-- points are v2's records.
--
-- Layers: pt2_light, pt2_shadow, pt2_rays, pt2_chain, pt2_candidates,
-- pt2_leeway, pt2_blocker, pt2_compare; the doll cache layers pt2_d1 ..
-- pt2_d5 and pt2_dchg (dl_draw). All default off.
-- =========================================================================

local viz = require("viz")
local SH  = require("shadow")

local M = {}

local floor, sqrt = math.floor, math.sqrt

M.LAYERS = { "pt2_light", "pt2_shadow", "pt2_rays", "pt2_chain", "pt2_candidates",
             "pt2_leeway", "pt2_blocker", "pt2_compare" }

-- Shadow cell size in world units (plan 6.2).
M.CELL_WU = 64

-- True when any pt2 layer is on (debug runs only; viz.is_on is false in prod).
function M.any_on()
  for _, id in ipairs(M.LAYERS) do
    if viz.is_on(id) then return true end
  end
  return false
end

-- A colour per chain position / pill index.
local PAL = {
  { 255, 80, 80 }, { 255, 170, 40 }, { 200, 90, 255 }, { 60, 200, 255 },
  { 255, 255, 80 }, { 255, 120, 200 }, { 140, 255, 140 }, { 180, 180, 255 },
}
local function pal(i) return PAL[((i - 1) % #PAL) + 1] end

local function pill_centre(p) return p.mx * 256 + 128, p.my * 256 + 128 end

-- Shadow cells for each pill of the best candidate's T(P), over the band
-- radius - 1 .. radius + 1 around A: a cell is in X's shadow when X's shell
-- aimed at the cell centre stops short of it, or meets cover >= the bar
-- that ray had at the best point (the test eval_point makes).
local function shadow_cells(inp, best)
  local env, A = inp.env, inp.A
  local acx, acy = pill_centre(A)
  local r_lo = (inp.radius + (inp.radii_offsets[#inp.radii_offsets] or 0) - 1) * 256
  local r_hi = (inp.radius + 1) * 256
  local cw = M.CELL_WU
  local per = {}
  local tmp = {}
  for i, X in ipairs(best.T) do
    local ray = best.rays[i]
    local bar = ray and ray.bar or inp.bar_chain
    -- PT2_FINISH_NO_COVER_MAX_ANGER: A's own ray needs no cover (bar 0, as
    -- eval_point), so every cell A can shoot counts as in A's shadow
    if X == inp.A and best.finish then bar = 0 end
    -- PT2_LINK_COVERS_PREV: a chain pill is covered by the next chain pill
    local ci = best.in_chain and best.in_chain[X]
    local nxt = inp.link_covers and ci and best.chain[ci + 1] or nil
    local sx, sy = pill_centre(X)
    local cells = {}
    for gy = floor((acy - r_hi) / cw), floor((acy + r_hi) / cw) do
      for gx = floor((acx - r_hi) / cw), floor((acx + r_hi) / cw) do
        local cx, cy = gx * cw + cw / 2, gy * cw + cw / 2
        local dx, dy = cx - acx, cy - acy
        local d = sqrt(dx * dx + dy * dy)
        if d >= r_lo and d <= r_hi then
          local r = SH.cast(env, sx, sy, cx, cy, SH.PILL_FIRE_LEN, best.extra, tmp)
          if r.hit == "reach" or (nxt and r.link == nxt) or r.cover >= bar then
            cells[#cells + 1] = gx * 65536 + gy
          end
        end
      end
    end
    per[i] = cells
  end
  return per
end

-- Keep what the layers draw. v1: pt2_attack's view of v1's winner or nil.
-- cmp: the PT2_CMP text. Writes only goal._pt2_viz, which nothing but
-- draw() reads.
function M.capture(goal, inp, res, v1, cmp, now)
  local best = res and res.best
  -- evaluate_group picked another pill of the group as A: draw around it
  if res and res.A and res.A ~= inp.A then
    inp = setmetatable({ A = res.A }, { __index = inp })
  end
  local cap = { tick = now, inp = inp, res = res, best = best, v1 = v1, cmp = cmp,
                pill_mx = inp.A.mx, pill_my = inp.A.my }
  if best and viz.is_on("pt2_shadow") then cap.cells = shadow_cells(inp, best) end
  goal._pt2_viz = cap
end

local function box(id, mx, my, inset, c, a, filled)
  viz.rect(id, mx + inset, my + inset, mx + 1 - inset, my + 1 - inset, c[1], c[2], c[3], a, filled)
end

local function draw_light(cap)
  local inp = cap.inp
  local range = SH.PILL_RANGE / 256
  -- shell reach: the start step plus a full life of steps
  local reach = (SH.SHELL_START_ADD + SH.life_ticks(SH.PILL_FIRE_LEN)) * SH.SHELL_SPEED / 256
  for _, p in ipairs(inp.pills) do
    if (p.health or 0) > 0 and not p.in_tank and not p.pos_unknown then
      local c
      if inp.env.is_ally(p) then c = { 80, 220, 80 }
      elseif p.owner == "neutral" then c = { 160, 160, 160 }
      else c = { 255, 70, 70 } end
      viz.circle("pt2_light", p.mx + 0.5, p.my + 0.5, range, c[1], c[2], c[3], 160, true)
      viz.circle("pt2_light", p.mx + 0.5, p.my + 0.5, reach, c[1], c[2], c[3], 70, true)
    end
  end
end

local function draw_shadow(cap)
  if not cap.cells then return end
  local cw = M.CELL_WU / 256
  local count = {}
  local n = 0
  for i, cells in ipairs(cap.cells) do
    local c = pal(i)
    n = n + 1
    for _, k in ipairs(cells) do
      local gx, gy = floor(k / 65536), k % 65536
      viz.rect("pt2_shadow", gx * cw, gy * cw, (gx + 1) * cw, (gy + 1) * cw, c[1], c[2], c[3], 60, true, true)
      count[k] = (count[k] or 0) + 1
    end
  end
  if n < 2 then return end
  for k, m in pairs(count) do
    if m == n then
      local gx, gy = floor(k / 65536), k % 65536
      viz.rect("pt2_shadow", gx * cw, gy * cw, (gx + 1) * cw, (gy + 1) * cw, 20, 20, 20, 90, true, true)
    end
  end
end

local _pts = {}
local function draw_rays(cap)
  local best = cap.best
  if not best then return end
  for i, X in ipairs(best.T) do
    local r = best.rays[i]
    local sx, sy = pill_centre(X)
    local c
    if r.hit == "reach" then c = { 150, 150, 150 }
    elseif r.ok then c = { 60, 220, 60 }
    else c = { 255, 50, 50 } end
    local pts, n = SH.shell_points(sx, sy, r.angle, SH.PILL_FIRE_LEN, _pts, cap.inp and cap.inp.env)
    -- a covered shell ends at the first blocker it meets; the others at the
    -- tank (or the end of their life)
    local stop = r.t or n
    if r.ok and r.hit == "tank" and r.n_block > 0 and r.block[1].t then stop = r.block[1].t end
    local last = math.min(n, stop)
    local x0, y0 = sx / 256, sy / 256
    for t = 1, last do
      local x1, y1 = pts[2 * t - 1] / 256, pts[2 * t] / 256
      viz.line("pt2_rays", x0, y0, x1, y1, c[1], c[2], c[3], 220)
      x0, y0 = x1, y1
    end
    if r.link and r.link_t and r.link_t <= n then
      local lx, ly = pts[2 * r.link_t - 1] / 256, pts[2 * r.link_t] / 256
      viz.circle("pt2_rays", lx, ly, 0.18, 255, 160, 40, 230, true, false)
    end
    viz.text("pt2_rays", x0 + 0.1, y0 - 0.2,
      string.format("d=%d/%d%s", r.cover or 0, r.bar or 0, (best.finish and X == cap.inp.A) and " finish" or ""),
      "topleft", c[1], c[2], c[3], 255, 0.8)
  end
end

local function draw_chain(cap)
  local best = cap.best
  if not best then return end
  local inp = cap.inp
  for i, p in ipairs(best.chain) do
    local c = pal(i)
    viz.text("pt2_chain", p.mx + 0.5, p.my - 0.3, tostring(i), "center", c[1], c[2], c[3], 255, 1.2)
    local q = best.chain[i + 1]
    if q then
      local x0, y0, x1, y1 = p.mx + 0.5, p.my + 0.5, q.mx + 0.5, q.my + 0.5
      viz.line("pt2_chain", x0, y0, x1, y1, c[1], c[2], c[3], 255)
      -- arrow head
      local dx, dy = x1 - x0, y1 - y0
      local d = sqrt(dx * dx + dy * dy)
      if d > 0 then
        local ux, uy = dx / d, dy / d
        viz.line("pt2_chain", x1, y1, x1 - 0.4 * ux - 0.25 * uy, y1 - 0.4 * uy + 0.25 * ux, c[1], c[2], c[3], 255)
        viz.line("pt2_chain", x1, y1, x1 - 0.4 * ux + 0.25 * uy, y1 - 0.4 * uy - 0.25 * ux, c[1], c[2], c[3], 255)
      end
    end
  end
  local A = best.chain[1] or inp.A
  viz.text("pt2_chain", A.mx + 0.5, A.my + 1.4,
    string.format("%s kills=%d need=%d have=%d depth=%s", best.chain_mode and "HOLD" or "SINGLE",
      best.kills, best.need or 0, best.have or 0, tostring(best.depth or "-")), "center", 255, 255, 255, 255, 0.9)
  -- hold point: where the tank waits until every planned wall stands
  local h = best.hold
  if h then
    local r, g, b = 80, 220, 255
    if not h.safe then r, g, b = 255, 80, 80 end
    viz.line("pt2_chain", h.fx - 0.3, h.fy - 0.3, h.fx + 0.3, h.fy + 0.3, r, g, b, 255)
    viz.line("pt2_chain", h.fx - 0.3, h.fy + 0.3, h.fx + 0.3, h.fy - 0.3, r, g, b, 255)
    viz.line("pt2_chain", best.cx, best.cy, h.fx, h.fy, r, g, b, 140)
    viz.text("pt2_chain", h.fx, h.fy + 0.6,
      string.format("hold d=%.2f %s", h.d or 0, h.safe and "safe" or ("UNSAFE " .. tostring(h.why))),
      "center", r, g, b, 255, 0.8)
  end
end

local function cand_body(r, cap_n, build_cost, inp)
  local tps = inp and inp.ticks_per_score or 16
  local band_t = inp and inp.travel_band_ticks or 0
  local band_line
  if band_t > 0 and r.tier_min then
    band_line = string.format("travel band: %.0f t (quickest in tier %.0f t + band %d t = %.0f t): %s",
      (r.score or 0) * tps, r.tier_min * tps, band_t, r.tier_min * tps + band_t,
      r.in_band and "IN band" or "OUT of band")
  else
    band_line = string.format("travel band: off (PT2_TRAVEL_BAND_TICKS %d); travel %.0f t", band_t,
      (r.score or 0) * tps)
  end
  return {
    band_line,
    -- point: where the tank stands; with PT2_AIM_NUDGE that is the nudged
    -- point (every ray was cast to it), nudge wu forward of the ring point
    string.format("deg=%.1f radius=%.2f point=(%.2f,%.2f) nudge=%d wu aim=%s", r.deg, r.radius or 0,
      r.cx, r.cy, r.nudge_wu or 0, tostring(r.aim_idx)),
    string.format("tier key: safe=%s kills=%d depth=%s blk=%d %sleeway=%s (capped at %d: %s)",
      tostring(r.safe), r.kills or 1, (r.kills or 1) >= 2 and tostring(r.depth or "-") or "(single: not a key)",
      r.blk or 1, (r.kills or 1) >= 2 and string.format("score=%.2f ", r.score or 0) or "",
      tostring(r.leeway or "-"), cap_n,
      tostring(r.leeway and math.min(r.leeway, cap_n) or "-")),
    string.format("score = base %.2f + builds %d x %d = %.2f", r.base or 0, r.n_built or 0,
      build_cost, r.score or 0),
    string.format("A: need=%d have=%d  covered=%s chain=%d%s%s", r.need or 0, r.have or 0,
      tostring(r.covered), r.chain and #r.chain or 1,
      r.why_unsafe and (" why_unsafe=" .. r.why_unsafe) or "",
      r.unsearched and " (no blocker search)" or ""),
  }
end

local function draw_candidates(cap)
  local res = cap.res
  if not res or not res.all then return end
  local inp = cap.inp
  local live = viz.detail_live
  for i, r in ipairs(res.all) do
    local c
    if r.rejected then c = { 255, 60, 60 }
    elseif r.safe then c = { 60, 230, 60 }
    else c = { 240, 220, 50 } end
    viz.rect("pt2_candidates", r.cx - 0.12, r.cy - 0.12, r.cx + 0.12, r.cy + 0.12, c[1], c[2], c[3], 200, false, true)
    if live then
      local id = string.format("pt2_cand_%d_%d", cap.tick or 0, i)
      local body
      if r.rejected then
        body = { string.format("deg=%.1f point=(%.2f,%.2f) rejected: %s", r.deg, r.cx, r.cy, r.rejected) }
      else
        body = cand_body(r, inp.leeway_min, inp.build_cost, inp)
      end
      viz.detail(id, "rect", { r.cx - 0.12, r.cy - 0.12, r.cx + 0.12, r.cy + 0.12 },
                 string.format("pt2 cand %.0f", r.deg), body)
    end
  end
  local b = cap.best
  if b then
    viz.circle("pt2_candidates", b.cx, b.cy, 0.22, 255, 255, 255, 255, true, true)
  end
end

local function draw_leeway(cap)
  local b = cap.best
  if not b or not b.leeway_pts then return end
  for _, q in ipairs(b.leeway_pts) do
    local x, y = q.x / 256, q.y / 256
    if q.ok then
      viz.circle("pt2_leeway", x, y, 0.06, 60, 230, 60, 255, true, true)
    else
      viz.circle("pt2_leeway", x, y, 0.06, 255, 60, 60, 255, true, true)
      viz.text("pt2_leeway", x + 0.08, y, tostring(q.why or "?"), "topleft", 255, 90, 90, 230, 0.6)
    end
  end
  viz.text("pt2_leeway", b.cx, b.cy + 0.35, string.format("leeway %d/8", b.leeway or 0),
    "center", 255, 255, 255, 230, 0.7)
end

local function draw_blocker(cap)
  local b = cap.best
  if not b then return end
  local seen = {}
  for _, r in ipairs(b.rays) do
    if r.hit == "tank" then
      for k = 1, r.n_block do
        local bl = r.block[k]
        if bl.kind ~= "planned" and not seen[bl.sq] then
          seen[bl.sq] = true
          local c = (bl.kind == "ally_pill") and { 80, 220, 80 } or { 60, 160, 255 }
          box("pt2_blocker", bl.sq % 256, floor(bl.sq / 256), 0.08, c, 150, true)
        end
      end
    end
  end
  for i, s in ipairs(b.plan or {}) do
    box("pt2_blocker", s.mx, s.my, 0.08, { 60, 160, 255 }, 255, false)
    viz.text("pt2_blocker", s.mx + 0.5, s.my + 0.3,
      (s.kind == "pill") and ("P" .. i) or tostring(i), "center", 255, 255, 255, 255, 0.9)
  end
end

local function draw_compare(cap)
  local b, v1 = cap.best, cap.v1
  if v1 then
    viz.circle("pt2_compare", v1.cx, v1.cy, 0.25, 255, 60, 255, 255, true, false)
    for _, s in ipairs(v1.act or {}) do box("pt2_compare", s.mx, s.my, 0.15, { 255, 60, 255 }, 255, false) end
    for _, s in ipairs(v1.pot or {}) do box("pt2_compare", s.mx, s.my, 0.15, { 255, 60, 255 }, 255, false) end
  end
  if b and b.safe then
    viz.circle("pt2_compare", b.cx, b.cy, 0.2, 60, 230, 255, 255, true, true)
    for _, s in ipairs(b.plan or {}) do box("pt2_compare", s.mx, s.my, 0.22, { 60, 230, 255 }, 255, false) end
    for i = 1, #b.chain - 1 do
      local p, q = b.chain[i], b.chain[i + 1]
      viz.line("pt2_compare", p.mx + 0.5, p.my + 0.5, q.mx + 0.5, q.my + 0.5, 60, 230, 255, 255)
    end
  end
  local diff = cap.cmp and cap.cmp:match("diff=(%S+)") or "?"
  local why = cap.cmp and cap.cmp:match("why=(.-) drive=") or "-"
  if v1 and b and b.safe then
    viz.line("pt2_compare", v1.cx, v1.cy, b.cx, b.cy, 255, 255, 255, 200)
    local dx, dy = (v1.cx - b.cx) * 256, (v1.cy - b.cy) * 256
    viz.text("pt2_compare", (v1.cx + b.cx) / 2, (v1.cy + b.cy) / 2,
      string.format("%.0f wu %s", sqrt(dx * dx + dy * dy), diff), "center", 255, 255, 255, 255, 0.8)
  end
  if v1 then
    viz.text("pt2_compare", v1.cx, v1.cy + 0.4, "why=" .. why, "center", 255, 120, 255, 255, 0.7)
  end
  if cap.cmp and viz.detail_live then
    local A = cap.inp.A
    viz.detail("pt2_cmp", "rect", { A.mx, A.my, A.mx + 1, A.my + 1 }, "pt2_cmp", { cap.cmp })
  end
end

-- =========================================================================
-- Doll cache layers (PT2_V2_DOLL, 2026-11-26): pt2_d1 .. pt2_d5 and
-- pt2_dchg. They draw the C store's own records (the event log records,
-- gh_pt2_v2.dl_viz_take): each lookup of the last run of each source
-- (take, dbl) and the changes of the last sync. Nothing here is a second
-- model: a square is coloured by the result the C store gave.
-- =========================================================================

M.DL_LAYERS = { "pt2_d1", "pt2_d2", "pt2_d3", "pt2_d4", "pt2_d5", "pt2_dchg" }

-- PT2_V2_DOLL_VIZ_RECORD (debug only): a headless -brain-debug run (winbolods)
-- records these layers too (viz.RECORD_ON), for a BrainTest replay.
if require("constants").PT2_V2_DOLL_VIZ_RECORD then
  for _, id in ipairs(M.DL_LAYERS) do viz.RECORD_ON[id] = true end
end

-- True when any doll layer is on (debug runs only).
function M.dl_any_on()
  for _, id in ipairs(M.DL_LAYERS) do
    if viz.is_on(id) then return true end
  end
  return false
end

-- The C record fields (gh_pt2_v2.c DlRec): lvl 1 change, 2 pill, 3 sync,
-- 4 new job, 5 ring (D4), 10 ray (D1), 11 filter (D2), 12 ring point
-- (D2/D3), 13 plan (D5). res 0 kept, 1 kept again in the run, 2 dropped by
-- a change, 3 new key, 4 new job, 5 first use, 6 tank in, 7 gone.
local NOSQ = 0xFFFF
local RES_COL = {
  [0] = { 60, 220, 60 }, [1] = { 60, 220, 60 }, [2] = { 255, 60, 60 }, [3] = { 255, 220, 40 },
  [4] = { 80, 140, 255 }, [5] = { 230, 230, 230 }, [6] = { 150, 150, 150 }, [7] = { 150, 150, 150 },
}
local RES_NAME = { [0] = "kept", "kept", "dropped", "new key", "new job", "first", "tank", "gone" }
local DC_NAME = { [0] = "none", "terrain", "forest", "base", "pill_on_square", "pill_reach", "pill_box", "margin",
  "box", "centre_pill", "slot_pill", "filter_square", "filter_slot", "filter_reach", "wsig_drop", "max_bytes",
  "drop_plans", "square" }
local COLD_NAME = { [0] = "first_job", "open", "time", "age", "rules", "lost", "busy", "nojob", "nomap", "other",
  [20] = "world input" }

local function sq_xy(sq) return sq % 256, floor(sq / 256) end

-- per source (0 take, 1 dbl): the records of the last run, the last sync
local dls = { run = {}, sync = {}, lost = 0 }

local function dl_take()
  local recs, lost = gh_pt2_v2.dl_viz_take()
  dls.lost = dls.lost + (lost or 0)
  for i = 1, #recs do
    local r = recs[i]
    if r.lvl >= 10 then
      local s = dls.run[r.src]
      if not s or s.run ~= r.run then
        s = { run = r.run, recs = {} }
        dls.run[r.src] = s
      end
      s.recs[#s.recs + 1] = r
    else
      local s = dls.sync[r.src]
      if not s or s.tick ~= r.tick then
        s = { tick = r.tick, recs = {} }
        dls.sync[r.src] = s
      end
      s.recs[#s.recs + 1] = r
    end
  end
end

local SRC_NAME = { [0] = "take", [1] = "dbl" }

local function hud_lines(id, lines)
  for i, ln in ipairs(lines) do
    viz.hud_text(id, 10, 10 + (i - 1) * 12, ln[1], "topright", ln[2], ln[3], ln[4])
  end
end

local function mark_A(id, s, src)
  local a = s.recs[1] and s.recs[1].asq
  if a and a ~= NOSQ then
    local ax, ay = sq_xy(a)
    viz.text(id, ax + 0.5, ay - 0.6, SRC_NAME[src] .. " A", "center", 255, 255, 255, 230, 0.8)
  end
end

local function res_count(lines, title, cnt, tot)
  local t = {}
  for rs = 0, 7 do if cnt[rs] then t[#t + 1] = string.format("%s %d", RES_NAME[rs], cnt[rs]) end end
  lines[#lines + 1] = { string.format("%s: %d lookups: %s", title, tot, table.concat(t, ", ")), 230, 230, 230 }
end

-- pt2_d1: the rays (pill X to the point's square) by store result.
local function draw_d1()
  local id = "pt2_d1"
  local cnt, tot = {}, 0
  for src, s in pairs(dls.run) do
    mark_A(id, s, src)
    for _, r in ipairs(s.recs) do
      if r.lvl == 10 and r.res ~= 1 and r.xsq ~= NOSQ and r.osq ~= NOSQ then
        tot = tot + 1; cnt[r.res] = (cnt[r.res] or 0) + 1
        local x0, y0 = sq_xy(r.xsq)
        local x1, y1 = sq_xy(r.osq)
        local c = RES_COL[r.res] or RES_COL[6]
        viz.line(id, x0 + 0.5, y0 + 0.5, x1 + 0.5, y1 + 0.5, c[1], c[2], c[3], r.res == 0 and 70 or 170)
      end
    end
  end
  local L = {}
  res_count(L, "D1 rays", cnt, tot)
  L[#L + 1] = { "green kept / red dropped by a change / yellow new key / blue new job / white first use", 200, 200, 200 }
  hud_lines(id, L)
end

-- the worst result per square (kept < first < new job < new key < dropped)
local RANK = { [0] = 0, [1] = 0, [5] = 1, [4] = 2, [3] = 3, [6] = 3, [7] = 3, [2] = 4 }
local function worst(t, sq, rs)
  local o = t[sq]
  if o == nil or (RANK[rs] or 0) > (RANK[o] or 0) then t[sq] = rs end
end

-- pt2_d2: filter answers (outline) and ring points (filled) by square.
local function draw_d2()
  local id = "pt2_d2"
  local fl, pt, cf, cp, nf, np = {}, {}, {}, {}, 0, 0
  for src, s in pairs(dls.run) do
    mark_A(id, s, src)
    for _, r in ipairs(s.recs) do
      if r.res ~= 1 and r.osq ~= NOSQ then
        if r.lvl == 11 then worst(fl, r.osq, r.res); nf = nf + 1; cf[r.res] = (cf[r.res] or 0) + 1
        elseif r.lvl == 12 then worst(pt, r.osq, r.res); np = np + 1; cp[r.res] = (cp[r.res] or 0) + 1 end
      end
    end
  end
  for sq, rs in pairs(fl) do
    local x, y = sq_xy(sq); local c = RES_COL[rs]
    viz.rect(id, x + 0.08, y + 0.08, x + 0.92, y + 0.92, c[1], c[2], c[3], 200, false)
  end
  for sq, rs in pairs(pt) do
    local x, y = sq_xy(sq); local c = RES_COL[rs]
    viz.rect(id, x + 0.3, y + 0.3, x + 0.7, y + 0.7, c[1], c[2], c[3], 200, true)
  end
  local L = {}
  res_count(L, "D2 filter (outline)", cf, nf)
  res_count(L, "D2 ring point (fill)", cp, np)
  L[#L + 1] = { "a square shows its worst result: green kept, white first, blue new job, yellow new key, red dropped", 200, 200, 200 }
  hud_lines(id, L)
end

-- pt2_d3: 12 slices of 30 degrees around A; kept = every point lookup in it kept.
local function draw_d3()
  local id = "pt2_d3"
  local kept, tot = 0, 0
  for src, s in pairs(dls.run) do
    mark_A(id, s, src)
    local sl = {}
    for _, r in ipairs(s.recs) do
      if r.lvl == 12 and r.res ~= 1 and r.deg10 ~= NOSQ and r.asq ~= NOSQ and r.osq ~= NOSQ then
        local i = floor(r.deg10 / 300) % 12
        local k = r.asq * 16 + i
        local e = sl[k]
        if not e then e = { asq = r.asq, i = i, n = 0, miss = 0, d = 0 }; sl[k] = e end
        e.n = e.n + 1
        if r.res ~= 0 then e.miss = e.miss + 1 end
        local ax, ay = sq_xy(r.asq); local x, y = sq_xy(r.osq)
        e.d = e.d + sqrt((x - ax) ^ 2 + (y - ay) ^ 2)
      end
    end
    for _, e in pairs(sl) do
      tot = tot + 1
      if e.miss == 0 then kept = kept + 1 end
      local ax, ay = sq_xy(e.asq)
      ax, ay = ax + 0.5, ay + 0.5
      local R = e.d / e.n
      local c = e.miss == 0 and { 60, 220, 60 } or { 255, 60, 60 }
      -- deg: 0 = north, clockwise (the ring's own angle)
      local a0, a1 = math.rad(e.i * 30), math.rad(e.i * 30 + 30)
      local px, py
      for k = 0, 6 do
        local a = a0 + (a1 - a0) * k / 6
        local x, y = ax + math.sin(a) * R, ay - math.cos(a) * R
        if px then viz.line(id, px, py, x, y, c[1], c[2], c[3], 230) end
        px, py = x, y
      end
      viz.line(id, ax, ay, ax + math.sin(a0) * R, ay - math.cos(a0) * R, c[1], c[2], c[3], 90)
      local am = (a0 + a1) / 2
      viz.text(id, ax + math.sin(am) * (R + 0.6), ay - math.cos(am) * (R + 0.6),
        string.format("%d/%d", e.n - e.miss, e.n), "center", c[1], c[2], c[3], 255, 0.6)
    end
  end
  hud_lines(id, {
    { string.format("D3 slices: %d of %d kept", kept, tot), 230, 230, 230 },
    { "arc at the slice's mean point distance: green = every point lookup in it kept, red = not; label kept/points", 200, 200, 200 },
  })
end

-- pt2_d4: at the last sync, each A level (the ring of a goal pill slot).
local function draw_d4()
  local id = "pt2_d4"
  local kept, tot = 0, 0
  for _, s in pairs(dls.sync) do
    for _, r in ipairs(s.recs) do
      if r.lvl == 5 and r.asq ~= NOSQ then
        tot = tot + 1
        local x, y = sq_xy(r.asq)
        if r.res == 0 then
          kept = kept + 1
          viz.circle(id, x + 0.5, y + 0.5, 1.2, 60, 220, 60, 230, false)
        else
          viz.circle(id, x + 0.5, y + 0.5, 1.2, 255, 60, 60, 255, false)
          viz.text(id, x + 0.5, y + 1.9, DC_NAME[r.code] or tostring(r.code), "center", 255, 90, 90, 255, 0.7)
          if r.csq ~= NOSQ then
            local cx, cy = sq_xy(r.csq)
            viz.line(id, x + 0.5, y + 0.5, cx + 0.5, cy + 0.5, 255, 90, 90, 200)
            viz.rect(id, cx + 0.1, cy + 0.1, cx + 0.9, cy + 0.9, 255, 90, 90, 230, false)
          end
        end
      end
    end
  end
  hud_lines(id, {
    { string.format("D4 pill X ring (last sync): %d of %d kept", kept, tot), 230, 230, 230 },
    { "circle on each goal pill slot: green kept, red dropped (cause under it, line to the square that changed)", 200, 200, 200 },
  })
end

-- pt2_d5: the plans by the point's square.
local function draw_d5()
  local id = "pt2_d5"
  local pl, cnt, tot = {}, {}, 0
  for src, s in pairs(dls.run) do
    mark_A(id, s, src)
    for _, r in ipairs(s.recs) do
      if r.lvl == 13 and r.res ~= 1 and r.osq ~= NOSQ then
        worst(pl, r.osq, r.res); tot = tot + 1; cnt[r.res] = (cnt[r.res] or 0) + 1
      end
    end
  end
  for sq, rs in pairs(pl) do
    local x, y = sq_xy(sq); local c = RES_COL[rs]
    viz.rect(id, x + 0.18, y + 0.18, x + 0.82, y + 0.82, c[1], c[2], c[3], 230, false)
    viz.rect(id, x + 0.22, y + 0.22, x + 0.78, y + 0.78, c[1], c[2], c[3], 230, false)
  end
  local L = {}
  res_count(L, "D5 plans", cnt, tot)
  L[#L + 1] = { "double outline on the point's square, its worst result; colours as D1", 200, 200, 200 }
  hud_lines(id, L)
end

-- pt2_dchg: what the last sync saw change, and new jobs.
local function draw_dchg()
  local id = "pt2_dchg"
  local nc, np, L = 0, 0, {}
  for src, s in pairs(dls.sync) do
    for _, r in ipairs(s.recs) do
      if r.lvl == 1 and r.csq ~= NOSQ then
        nc = nc + 1
        local x, y = sq_xy(r.csq)
        viz.rect(id, x, y, x + 1, y + 1, 255, 230, 40, 120, true)
        viz.text(id, x + 0.5, y + 0.5, string.format("%d>%d", r.ov, r.nv), "center", 0, 0, 0, 255, 0.6)
      elseif r.lvl == 2 then
        np = np + 1
      elseif r.lvl == 4 then
        L[#L + 1] = { string.format("%s: new job at tick %d (%s)", SRC_NAME[src] or "?", r.tick,
          COLD_NAME[r.code] or tostring(r.code)), 80, 140, 255 }
      elseif r.lvl == 3 and r.field == 1 then
        L[#L + 1] = { string.format("%s sync tick %d: %d squares changed, %d parts hit; doll %d kB (slot arenas: %d MB stored, %d MB dropped)",
          SRC_NAME[src] or "?", r.tick, r.ov, r.nv, r.bytes, r.csq, r.ckind), 230, 230, 230 }
      end
    end
  end
  table.insert(L, 1, { string.format("Changes (last sync): %d squares (yellow, old>new terrain), %d pill changes", nc, np), 255, 230, 40 })
  if dls.lost > 0 then L[#L + 1] = { string.format("records lost (list full): %d", dls.lost), 255, 60, 60 } end
  hud_lines(id, L)
end

-- Every tick (init.lua): take the new records, draw the layers that are on.
function M.dl_draw(now)
  if not M.dl_any_on() or not (gh_pt2_v2 and gh_pt2_v2.dl_viz_take) then return end
  dl_take()
  if viz.is_on("pt2_d1") then draw_d1() end
  if viz.is_on("pt2_d2") then draw_d2() end
  if viz.is_on("pt2_d3") then draw_d3() end
  if viz.is_on("pt2_d4") then draw_d4() end
  if viz.is_on("pt2_d5") then draw_d5() end
  if viz.is_on("pt2_dchg") then draw_dchg() end
end

-- Draw the kept data while the goal still targets the same pill.
function M.draw(goal)
  local cap = goal._pt2_viz
  if not cap or cap.pill_mx ~= goal.mx or cap.pill_my ~= goal.my then return end
  if viz.is_on("pt2_light") then draw_light(cap) end
  if viz.is_on("pt2_shadow") then draw_shadow(cap) end
  if viz.is_on("pt2_rays") then draw_rays(cap) end
  if viz.is_on("pt2_chain") then draw_chain(cap) end
  if viz.is_on("pt2_candidates") then draw_candidates(cap) end
  if viz.is_on("pt2_leeway") then draw_leeway(cap) end
  if viz.is_on("pt2_blocker") then draw_blocker(cap) end
  if viz.is_on("pt2_compare") then draw_compare(cap) end
end

return M
