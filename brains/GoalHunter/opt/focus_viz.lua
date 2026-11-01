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
    V.go_claims = d.claims
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
        end
      end
    end
    for _, bb in ipairs(bs) do
      local cx, cy = bb.mx + 0.5, bb.my + 0.5
      local own = owner_of(world, bb.id)
      local ours = own == "friendly" or own == "allied"
      if cov[bb.id] == "ours" then
      elseif cov[bb.id] == "neutral" then
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
    end
  end
end

-- FOCUS_SPIKE_ADAPTIVE_SPOT: the live rescore of this bot (focus_vote
-- adapt_step, A.score): the chosen spike spot and stand place, the top 3
-- spots with every score part, as map labels and HUD lines.
-- FOCUS_SPIKE_ALL_AGGRO: the levels of this bot's last rescore (focus_vote
-- level_step, A.level): c(b) at each target base (orange = in S, the bases
-- with c(b) = m), the counted cover places that are teammates' picks, and HUD
-- lines with m, S, every c(b) and the reserved (carried) pills of each
-- player N. Answers the next HUD y.
local ID_PLAN = "focused_spike_plan"
local function draw_level(A, line)
  local L = A.level
  if not L then return end
  for _, id in ipairs(L.ids) do
    local b = L.at[id]
    local inS = L.c[id] == L.m
    local r, g, bl = 200, 200, 200
    if inS then r, g, bl = 255, 170, 60 end
    if inS then viz.circle(ID_PLAN, b.mx + 0.5, b.my + 0.5, 1.2, 255, 170, 60, 230, false, false) end
  end
  for _, q in ipairs(L.pos or {}) do
    if q.src == "pick" then
    end
  end
  local ct = {}
  for _, id in ipairs(L.ids) do ct[#ct + 1] = string.format("b%d=%d", id, L.c[id]) end
  line(string.format("LEVELS m=%s  S=[%s]  c(b): %s", L.m and tostring(L.m) or "none",
    table.concat(L.S, ","), #ct > 0 and table.concat(ct, " ") or "none"), 255, 170, 60)
  local pns = {}
  for pn in pairs(L.reserved or {}) do pns[#pns + 1] = pn end
  table.sort(pns)
  local rt = {}
  for _, pn in ipairs(pns) do rt[#rt + 1] = string.format("player %d: %d", pn, L.reserved[pn]) end
  line(string.format("  reserved pills (team %d): %s", L.team or 0, #rt > 0 and table.concat(rt, ", ") or "none"),
    255, 170, 60)
end

local function draw_score(A)
  local ID = "focused_spike_plan"
  local S = A.score
  if not S then return end
  local x, y, dy = 150, 640, 14
  local function line(s, r, g, b)
    y = y + dy
  end
  if A.level then
    line(string.format("ADAPTIVE SPIKE SPOT id=%d at %s  all aggressive pills: payoff per base of S", A.id,
      mmss(S.t)), 255, 120, 255)
    draw_level(A, line)
  else
    line(string.format("ADAPTIVE SPIKE SPOT id=%d at %s  my claimed bases left: %s", A.id, mmss(S.t),
      #S.mine > 0 and table.concat(S.mine, ",") or "none"), 255, 120, 255)
  end
  local ch = A.mine
  if ch then
    local q = ch.stand
    line(string.format("  chosen (%d,%d) stand (%d,%d) stand risk %.2f + run %.2f",
      ch.mx, ch.my, q and q.mx or -1, q and q.my or -1, q and q.risk or 0, q and (q.val - q.risk) or 0), 120, 255, 120)
  else
    line("  chosen: none", 170, 170, 170)
  end
  for i, c in ipairs(S.top) do
    local p = c.parts
    local mark = (ch and ch.mx == c.mx and ch.my == c.my) and "  CHOSEN" or ""
    line(string.format("  %d. (%d,%d) total %.2f = payoff %.2f - stand %.2f - route %.2f - travel %.2f - run %.2f%s",
      i, c.mx, c.my, c.total, p.payoff, p.stand, p.route, p.travel, p.run, mark))
    local cx, cy = c.mx + 0.5, c.my + 0.5
    local r, g, b = 255, 120, 255
    if mark ~= "" then r, g, b = 120, 255, 120 end
  end
  if #S.top == 0 then line("  no allowed spike spot", 255, 130, 130) end
  if ch and ch.stand then
  end
end

local function draw_plan(F, P)
  local ID = "focused_spike_plan"
  local A = F.active
  if A and A.adaptive then draw_score(A); return end
  if not P or not P.tiles or #P.tiles == 0 then return end
  local R = C.PILL_FIRE_RANGE
  local stands = P.risk and P.risk.stands or {}
  for i, t in ipairs(P.tiles) do
    local cx, cy = t.mx + 0.5, t.my + 0.5
    local q = stands[i]
    if q then
    end
  end
end

local function ring_bases(ID, world, ids, r, g, b, rad)
  for _, id in ipairs(ids or {}) do
    local bb = world.bases and world.bases[id]
    if bb and bb.mx then
    end
  end
end

local function draw_votes(F, world, info, now)
  local ID = "focused_spike_votes"
  local x, y, dy = 150, 420, 14
  local V = F.vv
  local function line(s, r, g, b)
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
    if V.go_claims then
      local ids = {}
      for id in pairs(V.go_claims) do ids[#ids + 1] = id end
      table.sort(ids)
      for _, id in ipairs(ids) do gl[#gl + 1] = string.format("base %d->player %d", id, V.go_claims[id]) end
    end
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
  end
  -- FOCUS_SPIKE_ADAPTIVE_SPOT: the claim of each target base, and a mark on
  -- each spot change of this bot with its reason.
  if A.adaptive then
    local ids = {}
    for id in pairs(A.claims or {}) do ids[#ids + 1] = id end
    table.sort(ids)
    for _, id in ipairs(ids) do
      local bb = world.bases and world.bases[id]
      if bb and bb.mx then
        local pn = A.claims[id]
      end
    end
    for _, w in ipairs(A.switches or {}) do
      if w.new then
      end
      if w.old then
      end
    end
  end
end

function M.draw(state, world, info, now)
  local F = state and state._fs
  if not F then return end
  local P = F.plan
  if on("focused_spike_clusters") then draw_clusters(F, world, P) end
  if on("focused_spike_plan") then draw_plan(F, P) end
  if on("focused_spike_votes") then draw_votes(F, world, info, now) end
  if on("focused_spike_active") then draw_active(F, world) end
end

return M
