-- =========================================================================
-- GoalHunter/focus_viz.lua -- "Focused spike" overlays for BrainTest.
--
-- Debug draw only. Nothing here changes what a bot does: focus_vote.lua
-- calls M.note / M.snapshot only inside `if BRAIN_DEBUG_MODE then` blocks
-- (lua_strip drops them from opt/), and init.lua calls M.draw from its
-- BRAIN_DEBUG_MODE overlay block. Every number comes from this bot's own
-- focus_vote state (its plan, its messages, its world model).
--
-- Four layers (viz.lua IDS, BrainTest V window section "Focused spike"):
--   focused_spike_clusters -- per cluster: its bases in one color, a line
--       between every two bases within FOCUS_SPIKE_CLUSTER_GAP (euclid,
--       inclusive: the single-link rule of focus_vote.clusters), covered
--       marks (green ring = within PILL_FIRE_RANGE of our live pill; yellow
--       square = within range of a live neutral pill, only with
--       FOCUS_SPIKE_NEUTRAL_PILL_COVER), and a label with need, every risk
--       part and the rank.
--   focused_spike_plan     -- the best plan: spike spots, their
--       PILL_FIRE_RANGE cover circles, the stand place of each spot and a
--       line from the stand place to the spot.
--   focused_spike_votes    -- HUD panel: the vote as this bot saw it
--       (ballots, pledges, result, go list, abort, cooldown) plus rings on
--       the vote's target bases.
--   focused_spike_active   -- the active plan: its target bases in a
--       strong style, each spike spot with player N and going / placed /
--       dropped / gone.
--
-- The cluster and plan layers draw the last plan focus_vote made (it plans
-- about once per FOCUS_SPIKE_PLAN_INTERVAL ticks, and not while a vote or a
-- plan is on), so the overlay changes at the plan rate, not every tick.
-- =========================================================================
local C   = require("constants")
local viz = require("viz")

local M = {}

-- Ticks a finished vote stays on the HUD panel (10 s at 50 ticks/s).
M.HOLD_TICKS = 500

local PALETTE = {
  { 255, 120, 120 }, { 120, 200, 255 }, { 140, 240, 140 }, { 255, 200, 90 },
  { 220, 140, 255 }, { 90, 230, 220 }, { 255, 150, 210 }, { 200, 200, 120 },
  { 255, 170, 120 }, { 160, 170, 255 }, { 190, 255, 160 }, { 230, 230, 230 },
}

local NO_TEXT = {
  cooldown      = "cooling down or a plan is on",
  home_attacked = "a home base is under attack",
  tile_covered  = "a spike spot is covered already",
  not_target    = "a spike spot hits no target base",
  defended      = "too risky (risk >= max)",
  better        = "has a better plan",
}

local function mmss(t)
  local s = math.floor((t or 0) / 50)
  return string.format("%d:%02d", math.floor(s / 60), s % 60)
end

local function edist(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  return math.sqrt(dx * dx + dy * dy)
end

local function on(id) return viz.is_on and viz.is_on(id) end

-- ── Data hooks (focus_vote.lua, BRAIN_DEBUG_MODE only) ──────────────────
-- kind: "open" (d = {id, from, me, t, need, tiles, cluster, bases, bots}),
-- "ballot" (d = {id, pn, yes, pledge, eta, reason, armour}),
-- "result" (d = {id, pass, why, t, yes, pledged, nbots, reasons}),
-- "go" (d = {id, tiles, t}), "end" (d = {id, why, t}).
function M.note(F, kind, d)
  if kind == "open" then
    F.vv = { id = d.id, from = d.from, me = d.me, t = d.t, need = d.need,
             tiles = d.tiles or {}, cluster = d.cluster, bases = d.bases or {},
             bots = d.bots or {}, ballots = {} }
    return
  end
  local V = F.vv
  if not V or V.id ~= d.id then return end
  if kind == "ballot" then
    V.ballots[d.pn] = { yes = d.yes, pledge = d.pledge, eta = d.eta, reason = d.reason, proposer = d.proposer,
                        armour = d.armour }
  elseif kind == "result" then
    V.result = d
    if not d.pass then V.t_end = d.t end
  elseif kind == "go" then
    V.go = d.tiles
    V.t_go = d.t
  elseif kind == "end" then
    V.ended = d
    V.t_end = d.t
  end
end

-- Covered marks at plan time: base id -> "ours" / "neutral". ours = this
-- bot's known live friendly pills (focus_vote pills_of(world, false)).
function M.snapshot(F, world, ours)
  local cov = {}
  local R = C.PILL_FIRE_RANGE
  local neutral = {}
  if C.FOCUS_SPIKE_NEUTRAL_PILL_COVER == true then
    for _, p in pairs(world.pills or {}) do
      if not p.in_tank and p.mx and (p.health or 0) > 0 and p.owner == "neutral" then
        neutral[#neutral + 1] = p
      end
    end
  end
  for _, cl in ipairs(F.clusters or {}) do
    for _, b in ipairs(cl.bases) do
      for _, p in ipairs(ours or {}) do
        if edist(b.mx, b.my, p.mx, p.my) <= R then cov[b.id] = "ours"; break end
      end
      if not cov[b.id] then
        for _, p in ipairs(neutral) do
          if edist(b.mx, b.my, p.mx, p.my) <= R then cov[b.id] = "neutral"; break end
        end
      end
    end
  end
  F.viz_cov = cov
end

-- ── Draw ────────────────────────────────────────────────────────────────
local function owner_of(world, id)
  local b = world.bases and world.bases[id]
  return b and b.owner or "unknown"
end

local function draw_clusters(F, world, P)
  local ID = "focused_spike_clusters"
  local gap = C.FOCUS_SPIKE_CLUSTER_GAP
  local rows = {}
  for _, r in ipairs(P and P.rows or {}) do rows[tostring(r.key)] = r end
  local cov = F.viz_cov or {}
  for ci, cl in ipairs(F.clusters or {}) do
    local col = PALETTE[(ci - 1) % #PALETTE + 1]
    local r, g, b = col[1], col[2], col[3]
    local bs = cl.bases
    -- Single link: every two bases within the gap.
    for i = 1, #bs do
      for j = i + 1, #bs do
        if edist(bs[i].mx, bs[i].my, bs[j].mx, bs[j].my) <= gap then
          viz.line(ID, bs[i].mx + 0.5, bs[i].my + 0.5, bs[j].mx + 0.5, bs[j].my + 0.5, r, g, b, 200)
        end
      end
    end
    for _, bb in ipairs(bs) do
      local cx, cy = bb.mx + 0.5, bb.my + 0.5
      local own = owner_of(world, bb.id)
      local ours = own == "friendly" or own == "allied"
      viz.circle(ID, cx, cy, 0.55, r, g, b, 230, true, not ours)
      viz.text(ID, cx, cy - 0.9, string.format("b%d %s", bb.id, ours and "ours" or "target"),
               "center", r, g, b, 235, 0.32)
      if cov[bb.id] == "ours" then
        viz.circle(ID, cx, cy, 1.0, 80, 255, 80, 240, true, false)
      elseif cov[bb.id] == "neutral" then
        viz.rect(ID, bb.mx - 0.4, bb.my - 0.4, bb.mx + 1.4, bb.my + 1.4, 255, 230, 60, 240, false, true)
      end
    end
    -- Label at the mean of the bases.
    local key = tostring(bs[1].id)
    local row = rows[key]
    local nt, no = 0, 0
    for _, bb in ipairs(bs) do
      local own = owner_of(world, bb.id)
      if own == "friendly" or own == "allied" then no = no + 1 else nt = nt + 1 end
    end
    local lx, ly = cl.cx + 0.5, cl.cy + 2.0
    local lines = { string.format("cluster %s: %d bases (ours %d, target %d)", key, #bs, no, nt) }
    if row and row.def then
      local d = row.def
      lines[#lines + 1] = string.format("need %d  risk %.2f = spot %.2f + route %.2f + detour %.2f + tanks %.2f - credit %.2f",
        row.need or 0, d.total or 0, d.spot or 0, d.route or 0, d.detour or 0, d.tanks or 0, d.credit or 0)
      lines[#lines + 1] = string.format("rank %.3f = %d / (%d + %.1f x %.2f)  %s",
        row.rank or 0, row.nb or 0, math.max(0, row.need or 0), C.FOCUS_SPIKE_DEFENSE_WEIGHT,
        d.total or 0, row.taken and "BEST" or (row.skip and ("skip " .. row.skip) or "not best"))
    elseif row then
      lines[#lines + 1] = string.format("need %s  skip %s", tostring(row.need), tostring(row.skip))
    else
      lines[#lines + 1] = "no target base (all ours)"
    end
    if C.FOCUS_SPIKE_NEUTRAL_PILL_COVER == true then
      lines[#lines + 1] = "neutral pills: cover + counted as hostile in spot/route"
    end
    for i, s in ipairs(lines) do
      viz.text(ID, lx, ly + (i - 1) * 0.7, s, "center", r, g, b, 235, 0.32)
    end
  end
  viz.hud_text(ID, 150, 600, string.format("Focused spike clusters: gap %s (euclid, inclusive), %d clusters, plan t=%s",
    tostring(gap), #(F.clusters or {}), P and mmss(P.t) or "-"), "topleft", 220, 220, 220, 230)
end

local function draw_plan(P)
  local ID = "focused_spike_plan"
  if not P or not P.tiles or #P.tiles == 0 then return end
  local R = C.PILL_FIRE_RANGE
  local stands = P.risk and P.risk.stands or {}
  for i, t in ipairs(P.tiles) do
    local cx, cy = t.mx + 0.5, t.my + 0.5
    viz.circle(ID, cx, cy, R, 255, 120, 255, 170, true, false)
    viz.rect(ID, t.mx, t.my, t.mx + 1, t.my + 1, 255, 80, 255, 240, true, true)
    viz.text(ID, cx, cy - 0.9, string.format("spike spot %d (%d,%d)", i, t.mx, t.my),
             "center", 255, 120, 255, 240, 0.34)
    local q = stands[i]
    if q then
      viz.line(ID, q.mx + 0.5, q.my + 0.5, cx, cy, 120, 220, 255, 220)
      viz.rect(ID, q.mx + 0.15, q.my + 0.15, q.mx + 0.85, q.my + 0.85, 120, 220, 255, 240, true, true)
      viz.text(ID, q.mx + 0.5, q.my + 1.4, string.format("stand (%d,%d) r=%.2f", q.mx, q.my, q.risk or 0),
               "center", 120, 220, 255, 240, 0.32)
    end
  end
end

local function ring_bases(ID, world, ids, r, g, b, rad)
  for _, id in ipairs(ids or {}) do
    local bb = world.bases and world.bases[id]
    if bb and bb.mx then
      viz.circle(ID, bb.mx + 0.5, bb.my + 0.5, rad, r, g, b, 250, true, false)
      viz.circle(ID, bb.mx + 0.5, bb.my + 0.5, rad + 0.15, r, g, b, 250, true, false)
    end
  end
end

local function draw_votes(F, world, info, now)
  local ID = "focused_spike_votes"
  local x, y, dy = 150, 420, 14
  local V = F.vv
  local function line(s, r, g, b)
    viz.hud_text(ID, x, y, s, "topleft", r or 230, g or 230, b or 230, 245)
    y = y + dy
  end
  local cool = (F.cool_until or 0) - now
  if not V or (V.t_end and now - V.t_end > M.HOLD_TICKS) then
    line(string.format("FOCUSED SPIKE VOTE: none%s",
      cool > 0 and string.format("  (cooldown %ds left)", math.ceil(cool / 50)) or ""), 160, 160, 160)
    return
  end
  ring_bases(ID, world, V.bases, 255, 240, 120, 1.3)
  line(string.format("FOCUSED SPIKE VOTE id=%d by player %d at %s  cluster %s  bases %s",
    V.id, V.from, mmss(V.t), tostring(V.cluster), table.concat(V.bases, ",")), 255, 240, 120)
  local sp = {}
  for _, t in ipairs(V.tiles) do sp[#sp + 1] = string.format("(%d,%d)", t.mx, t.my) end
  line(string.format("  spike spots %s  need %d", table.concat(sp, " "), V.need or #V.tiles))
  -- Ballots: proposer = yes with its pledge; a silent bot counts as yes.
  local yes, pledged = 0, 0
  for _, pn in ipairs(V.bots) do
    local bl = V.ballots[pn]
    local s
    if bl then
      if bl.yes and bl.proposer and bl.pledge == nil then
        yes = yes + 1
        s = string.format("  player %d: YES (proposer; its pledge is not in the messages)", pn)
      elseif bl.yes then
        yes = yes + 1
        pledged = pledged + (bl.pledge or 0)
        s = string.format("  player %d: YES pledge %d", pn, bl.pledge or 0)
        if (bl.pledge or 0) == 0 and bl.armour and bl.armour < C.FOCUS_SPIKE_PLEDGE_MIN_ARMOR then
          s = s .. string.format(" (armour %d < %d: no pledge)", bl.armour, C.FOCUS_SPIKE_PLEDGE_MIN_ARMOR)
        end
        if pn == V.from then s = s .. " (proposer)" end
      else
        s = string.format("  player %d: NO - %s", pn, NO_TEXT[bl.reason] or tostring(bl.reason))
      end
    else
      yes = yes + 1
      s = string.format("  player %d: silent (counts as yes, pledge 0)", pn)
    end
    line(s, bl and (bl.yes and 140 or 255) or 200, bl and (bl.yes and 240 or 130) or 200, bl and (bl.yes and 140 or 130) or 200)
  end
  local R = V.result
  if R then
    local nb = R.nbots or #V.bots
    local ny = R.yes or yes
    local np = R.pledged or pledged
    local rr, rg, rb = R.pass and 140 or 255, R.pass and 240 or 120, R.pass and 140 or 120
    line(string.format("  RESULT %s (%s): yes %d of %d bots (need more than %d), pledges %d of need %d",
      R.pass and "PASSED" or "FAILED", tostring(R.why), ny, nb, math.floor(nb / 2), np, V.need or 0),
      rr, rg, rb)
    if R.yes == nil then
      line("  (counts from the ballots this bot heard; the proposer's pledge is not sent)", 170, 170, 170)
    end
  else
    line(string.format("  voting... %dt left", math.max(0, V.t + C.FOCUS_SPIKE_VOTE_WINDOW_TICKS - now)), 200, 200, 255)
  end
  if V.go then
    local gl = {}
    for _, t in ipairs(V.go) do gl[#gl + 1] = string.format("(%d,%d)->player %d", t.mx, t.my, t.pn or -1) end
    line(string.format("  GO at %s: %s", mmss(V.t_go), table.concat(gl, " ")), 140, 240, 140)
  end
  if V.ended then
    local ab = tostring(V.ended.why):match("^abort_(.*)$")
    line(string.format("  %s at %s: %s", ab and "ABORT" or "END", mmss(V.ended.t), ab or tostring(V.ended.why)),
      ab and 255 or 200, ab and 120 or 200, ab and 120 or 200)
  end
  if cool > 0 then line(string.format("  cooldown %ds left", math.ceil(cool / 50)), 255, 200, 120) end
end

local function draw_active(F, world)
  local ID = "focused_spike_active"
  local A = F.active
  if not A then return end
  ring_bases(ID, world, A.bases, 255, 60, 60, 1.6)
  for _, t in ipairs(A.tiles or {}) do
    local st = t.placed and "placed" or (t.dropped and "dropped" or (t.gone and "gone" or "going"))
    local r, g, b = 255, 200, 60
    if t.placed then r, g, b = 80, 255, 80 elseif t.dropped or t.gone then r, g, b = 160, 160, 160 end
    viz.rect(ID, t.mx - 0.1, t.my - 0.1, t.mx + 1.1, t.my + 1.1, r, g, b, 250, false, true)
    viz.text(ID, t.mx + 0.5, t.my + 1.6, string.format("player %d: %s", t.pn or -1, st),
             "center", r, g, b, 250, 0.36)
  end
end

function M.draw(state, world, info, now)
  local F = state and state._fs
  if not F then return end
  local P = F.plan
  if on("focused_spike_clusters") then draw_clusters(F, world, P) end
  if on("focused_spike_plan") then draw_plan(P) end
  if on("focused_spike_votes") then draw_votes(F, world, info, now) end
  if on("focused_spike_active") then draw_active(F, world) end
end

return M
