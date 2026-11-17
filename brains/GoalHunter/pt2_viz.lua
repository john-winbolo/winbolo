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
-- pt2_leeway, pt2_blocker, pt2_compare. All default off.
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
    local pts, n = SH.shell_points(sx, sy, r.angle, SH.PILL_FIRE_LEN, _pts)
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
      string.format("d=%d/%d", r.cover or 0, r.bar or 0), "topleft", c[1], c[2], c[3], 255, 0.8)
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

local function cand_body(r, cap_n, build_cost)
  return {
    string.format("deg=%.1f radius=%.2f point=(%.2f,%.2f)", r.deg, r.radius or 0, r.cx, r.cy),
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
        body = cand_body(r, inp.leeway_min, inp.build_cost)
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
