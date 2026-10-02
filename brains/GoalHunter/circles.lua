local function __idiv(a,b) return math.floor(a/b) end
-- GoalHunter/circles.lua — front-line "circles" (R2, 2026-06-01 brainstorm)
--
-- A "circle" is a radius-~12 region centered on a front "3"-overlay tile
-- (influence sign-flip boundary). Circles are placed greedily to cover the
-- most points of interest (pills + bases, any owner) without overlapping, and
-- form the unit of front-line analysis + reinforcement (see R3 in
-- SQUAD_BRAINSTORM_DECISIONS_2026-06-01.md).
--
-- Recompute is HYBRID: rebuilt on a cadence (~5 s) AND each fresh circle is
-- matched to the nearest prior circle by center proximity, so its influence
-- history carries over. Each circle snapshots:
--   ratio        - current friendly influence share of the disc (0..1)
--   ratio_prev   - the matched prior generation's ratio (short window)
--   ratio_origin - the first-seen ratio at this spot   (long window)
-- R3 compares ratio against ratio_prev / ratio_origin to decide winning/losing.

local cpf = require("cpathfinder")

local M = {}

M.RADIUS          = 12      -- disc radius in tiles
M.RECOMPUTE_TICKS = 250     -- ~5 s @ 50 Hz (history SAMPLE cadence + periodic rebuild)
M.ADJUST_MIN_GAP  = 25      -- min ticks between front-line-CHANGE-triggered rebuilds (debounce)
M.MIN_FRONT_PTS   = 4       -- no circles until a front exists
M.MATCH_DIST      = 12      -- center proximity (tiles) to inherit history
M.MAX_CIRCLES     = 8       -- safety cap
-- Win/loss windows: a circle is "losing" only when it has lost influence-share
-- ground over BOTH the short (1 min) AND long (5 min) windows. Instant
-- pill/base/tank counts are a separate advisory WARNING, not a dispatch trigger.
M.SHORT_TICKS     = 3000    -- 1 min @ 50 Hz
M.LONG_TICKS      = 15000   -- 5 min @ 50 Hz
M.LOSE_DELTA      = 0.03    -- min influence-share drop (both windows) to count as losing
M.HIST_MAX        = 80      -- ratio samples per record (covers >5 min at 5 s cadence)
M.PRUNE_TICKS     = 15000   -- prune a destroyed record once destroyed > 5 min ago

M.circles = {}              -- active circle list (current generation)

-- Circle HISTORY: a persistent record per continuous circle existence. A record
-- is created when a circle first appears, kept alive (samples appended) while it
-- persists, marked destroyed when it vanishes, and linked prev/next to the
-- temporally-adjacent record it handed off to/from at that spot. Pruned only
-- once destroyed longer than PRUNE_TICKS ago, so the lineage always reaches back
-- 5 min for the long-window trend. Fields:
--   idx, cx, cy, created, destroyed(nil=alive), prev, next,
--   ratio/blue/red (latest), samples = { {tick,ratio,blue,red}, ... }
M.history = {}

local _next_idx    = 1       -- monotonic record index
local _last_build  = -1000000  -- last periodic rebuild
local _last_adjust = -1000000  -- last rebuild of any cause (front-change debounce)
local _last_sample = -1000000  -- last history-sample append (keeps windows periodic)
local _last_front_sig = nil    -- signature of the front-line point set

-- Order-insensitive hash of the INFLUENCE INPUTS — alive friendly/hostile pills
-- (id+pos+owner) and friendly/hostile base owners. The influence grid (and thus
-- the front line) is stamped only from these, so this changes exactly when the
-- front can move: pill built / killed / picked up / captured, or base team
-- change. Cheaper than scanning the front line every tick, and event-exact.
-- Addition is commutative, so pairs() iteration order doesn't matter.
local function influence_sig(world)
  local MOD = 2147483647
  local h = 0
  if world.pills then
    for id, p in pairs(world.pills) do
      if (p.health or 0) > 0 then
        local oc = (p.owner == "friendly") and 1 or (p.owner == "hostile") and 2 or 0
        if oc ~= 0 then  -- neutral pills don't stamp influence
          local item = ((id * 131 + p.mx) * 131 + p.my) * 4 + oc
          h = (h + item * 2654435761) % MOD
        end
      end
    end
  end
  if world.bases then
    for id, b in pairs(world.bases) do
      local oc = (b.owner == "friendly") and 1 or (b.owner == "hostile") and 2 or 0
      if oc ~= 0 then
        local item = (((id * 131 + b.mx) * 131 + b.my) * 4 + oc) + 7
        h = (h + item * 2654435761) % MOD
      end
    end
  end
  return h
end

-- Friendly influence share of the disc: sum(+inf) / sum(|inf|). >0 = ours,
-- <0 = enemy. 0.5 when even or empty. Also returns the highest-influence
-- (safest) tile in the disc — the reinforcement destination (R3).
local function influence_scan(cx, cy, r)
  local pos, tot = 0, 0
  local blue, red = 0, 0
  local best_v, best_mx, best_my = -1e30, cx, cy
  local r2 = r * r
  for dy = -r, r do
    for dx = -r, r do
      if dx * dx + dy * dy <= r2 then
        local tx, ty = cx + dx, cy + dy
        local v = cpf.influence_at(tx, ty)
        if v > best_v then best_v, best_mx, best_my = v, tx, ty end
        if v ~= 0 then
          local a = (v < 0) and -v or v
          tot = tot + a
          if v > 0 then pos = pos + a; blue = blue + 1 else red = red + 1 end
        end
      end
    end
  end
  local ratio = (tot == 0) and 0.5 or (pos / tot)
  return ratio, best_mx, best_my, blue, red
end

-- Greedy non-overlapping circle placement over the front points, maximizing
-- coverage of still-uncovered POIs.
local function build(world, fpts)
  fpts = fpts or cpf.find_front_line()
  local np = fpts and (__idiv(#fpts, 2)) or 0
  if np < M.MIN_FRONT_PTS then return {} end

  -- POIs: every pill (alive) + every base, any owner, weighted equally.
  local pois = {}
  if world.pills then
    for _, p in pairs(world.pills) do
      if (p.health or 0) > 0 then pois[#pois + 1] = { mx = p.mx, my = p.my } end
    end
  end
  if world.bases then
    for _, b in pairs(world.bases) do
      pois[#pois + 1] = { mx = b.mx, my = b.my }
    end
  end

  local r        = M.RADIUS
  local r2       = r * r
  local sep2     = (2 * r) * (2 * r)   -- non-overlap: centers >= 2r apart
  local placed   = {}
  local covered  = {}                  -- poi index -> true

  while #placed < M.MAX_CIRCLES do
    local best, best_n, best_cov = nil, 0, nil
    for i = 1, np do
      local cmx, cmy = fpts[2 * i - 1], fpts[2 * i]
      -- reject centers that would overlap an already-placed circle
      local ok = true
      for _, pc in ipairs(placed) do
        local ddx, ddy = cmx - pc.cx, cmy - pc.cy
        if ddx * ddx + ddy * ddy < sep2 then ok = false; break end
      end
      if ok then
        local n, cov = 0, nil
        for pi = 1, #pois do
          if not covered[pi] then
            local poi = pois[pi]
            local ddx, ddy = poi.mx - cmx, poi.my - cmy
            if ddx * ddx + ddy * ddy <= r2 then
              n = n + 1
              cov = cov or {}
              cov[#cov + 1] = pi
            end
          end
        end
        if n > best_n then best, best_n, best_cov = { cx = cmx, cy = cmy }, n, cov end
      end
    end
    if not best or best_n == 0 then break end
    for _, pi in ipairs(best_cov) do covered[pi] = true end
    best.poi = best_n
    placed[#placed + 1] = best
  end
  return placed
end

-- Rebuild circles if the cadence is due (self-throttling). Maintains the
-- persistent circle HISTORY (records with created/destroyed/prev/next + a sample
-- series) and computes each current circle's win/loss trend by walking its
-- lineage back to the 1 min and 5 min marks.
function M.update(state, world, now)
  if world == nil then return end

  -- Rebuild when: first time / empty, OR the periodic cadence is due, OR an
  -- influence input changed (pill built/killed/picked-up/captured, base team
  -- change) — debounced by ADJUST_MIN_GAP. History samples are only appended on
  -- the periodic cadence so the 1 min / 5 min windows stay evenly spaced
  -- regardless of how often the front shifts.
  local sig = influence_sig(world)
  local empty        = (#M.circles == 0)
  local cadence_due  = (now - _last_build) >= M.RECOMPUTE_TICKS
  local front_change = (sig ~= _last_front_sig) and ((now - _last_adjust) >= M.ADJUST_MIN_GAP)
  if not (empty or cadence_due or front_change) then return end

  local do_sample = empty or cadence_due
  _last_adjust = now
  _last_front_sig = sig
  if do_sample then _last_build = now; _last_sample = now end

  local fresh  = build(world)
  local match2 = M.MATCH_DIST * M.MATCH_DIST

  for _, c in ipairs(fresh) do
    c.ratio, c.safe_mx, c.safe_my, c.blue, c.red = influence_scan(c.cx, c.cy, M.RADIUS)
  end

  -- 1) Match fresh circles to ALIVE records (nearest, one record per circle).
  local alive = {}
  for _, r in ipairs(M.history) do if not r.destroyed then alive[#alive + 1] = r end end
  local taken = {}
  for _, c in ipairs(fresh) do
    local m, md = nil, match2 + 1
    for _, r in ipairs(alive) do
      if not taken[r.idx] then
        local ddx, ddy = c.cx - r.cx, c.cy - r.cy
        local d = ddx * ddx + ddy * ddy
        if d <= match2 and d < md then md, m = d, r end
      end
    end
    if m then
      taken[m.idx] = true
      c._rec = m
      m.cx, m.cy = c.cx, c.cy          -- record follows the front each adjustment
      m.ratio, m.blue, m.red = c.ratio, c.blue, c.red
      if do_sample then                -- but only sample on the periodic cadence
        local s = m.samples
        s[#s + 1] = { tick = now, ratio = c.ratio, blue = c.blue, red = c.red, cx = c.cx, cy = c.cy }
        while #s > M.HIST_MAX do table.remove(s, 1) end
      end
    end
  end

  -- 2) Alive records with no match this rebuild are DESTROYED.
  local just_destroyed = {}
  for _, r in ipairs(alive) do
    if not taken[r.idx] then r.destroyed = now; just_destroyed[#just_destroyed + 1] = r end
  end

  -- 3) New record per unmatched fresh circle; link prev to a record destroyed
  --    THIS rebuild nearby (a hand-off at that spot).
  for _, c in ipairs(fresh) do
    if not c._rec then
      local r = { idx = _next_idx, cx = c.cx, cy = c.cy, created = now,
                  ratio = c.ratio, blue = c.blue, red = c.red,
                  samples = { { tick = now, ratio = c.ratio, blue = c.blue, red = c.red, cx = c.cx, cy = c.cy } } }
      _next_idx = _next_idx + 1
      local p, pd = nil, match2 + 1
      for _, d in ipairs(just_destroyed) do
        local ddx, ddy = c.cx - d.cx, c.cy - d.cy
        local dd = ddx * ddx + ddy * ddy
        if dd <= match2 and dd < pd then pd, p = dd, d end
      end
      if p then r.prev = p.idx; p.next = r.idx end
      M.history[#M.history + 1] = r
      c._rec = r
    end
  end

  -- 4) Prune records destroyed > PRUNE_TICKS ago; unlink their successors.
  local by_idx = {}
  local kept = {}
  for _, r in ipairs(M.history) do
    if not (r.destroyed and (now - r.destroyed) > M.PRUNE_TICKS) then
      kept[#kept + 1] = r
      by_idx[r.idx] = r
    end
  end
  for _, r in ipairs(kept) do
    if r.prev and not by_idx[r.prev] then r.prev = nil end   -- predecessor pruned
  end
  M.history = kept
  M._by_idx = by_idx

  -- 5) Per-current-circle trend: walk the lineage back to the window mark.
  for _, c in ipairs(fresh) do
    local function lb(window)
      local target = now - window
      local r, oldest = c._rec, nil
      while r do
        local s = r.samples
        for i = #s, 1, -1 do
          oldest = s[i]
          if s[i].tick <= target then return s[i], true end
        end
        r = r.prev and by_idx[r.prev] or nil
      end
      return oldest, false
    end
    local s, sready = lb(M.SHORT_TICKS)
    local l, lready = lb(M.LONG_TICKS)
    s = s or { ratio = c.ratio, cx = c.cx, cy = c.cy, blue = c.blue, red = c.red }
    l = l or { ratio = c.ratio, cx = c.cx, cy = c.cy, blue = c.blue, red = c.red }
    c.ratio_prev = s.ratio; c.prev_cx = s.cx; c.prev_cy = s.cy; c.prev_blue = s.blue; c.prev_red = s.red
    c.ratio_origin = l.ratio; c.origin_cx = l.cx; c.origin_cy = l.cy; c.origin_blue = l.blue; c.origin_red = l.red
    c.short_ready = sready; c.long_ready = lready
    c.id = c._rec.idx
    c.created_tick = c._rec.created
    local short_lose = sready and (c.ratio < s.ratio - M.LOSE_DELTA)
    local long_lose  = lready and (c.ratio < l.ratio - M.LOSE_DELTA)
    c.losing = short_lose and long_lose
  end

  M.circles = fresh
end

-- Per-tick win/loss + reinforcement need (cheap: small pill/base/tank lists).
-- Primary signal is the influence TREND — current ratio vs both the prior
-- generation (short window) and the first-seen baseline (long window); BOTH
-- must show a drop of >= LOSE_DELTA to count as losing. A brand-new circle
-- (younger than one rebuild cycle, no real history) falls back to instant
-- counts: enemy pills+bases+visible-tanks outnumber friendly pills+bases.
-- Counts use GLOBAL world data (pills/bases) where possible; enemy tanks are
-- limited to the bot's view (we no longer broadcast positions), so they only
-- refine the read, they don't drive it.
function M.assess(state, world, info, now)
  if world == nil then return end
  local enemy_tanks = (state.perc and state.perc.enemy_tanks) or {}
  local our_n   = (state.perc and state.perc.allied_tank_count) or 1   -- incl. self
  local enemy_n = (state.perc and state.perc.enemy_tank_count) or 0
  local team_ratio = (enemy_n > 0) and (our_n / enemy_n) or 1.0
  local r2 = M.RADIUS * M.RADIUS

  for _, c in ipairs(M.circles) do
    local fp, ep, fb, eb, et = 0, 0, 0, 0, 0
    if world.pills then
      for _, p in pairs(world.pills) do
        if (p.health or 0) > 0 then
          local ddx, ddy = p.mx - c.cx, p.my - c.cy
          if ddx * ddx + ddy * ddy <= r2 then
            if p.owner == "friendly" then fp = fp + 1
            elseif p.owner == "hostile" then ep = ep + 1 end
          end
        end
      end
    end
    if world.bases then
      for _, b in pairs(world.bases) do
        local ddx, ddy = b.mx - c.cx, b.my - c.cy
        if ddx * ddx + ddy * ddy <= r2 then
          if b.owner == "friendly" then fb = fb + 1
          elseif b.owner == "hostile" then eb = eb + 1 end
        end
      end
    end
    for _, t in ipairs(enemy_tanks) do
      local ddx, ddy = t.mx - c.cx, t.my - c.cy
      if ddx * ddx + ddy * ddy <= r2 then et = et + 1 end
    end
    c.fp, c.ep, c.fb, c.eb, c.et = fp, ep, fb, eb, et

    -- Instant counts are an advisory WARNING only — NOT a dispatch trigger.
    -- (Enemy pills+bases+visible-tanks outnumber our pills+bases here.)
    c.warning = (ep + eb + et) > (fp + fb)

    -- Reinforcement need keys off the CONFIRMED losing trend (set in update():
    -- lost ground over both the 1 min and 5 min windows). Target allies
    -- in-circle = enemy tanks * global team ratio (e.g. 8 bots vs 4 humans ->
    -- 2:1). Self-regulates: as the influence trend recovers, losing clears.
    c.need = c.losing and math.max(0, math.ceil(et * team_ratio)) or 0
  end
end

-- Nearest LOSING circle to a tile that still needs reinforcement. Used by an
-- uncommitted bot to decide where to help (decentralized: each bot picks the
-- nearest needy circle to itself; arrivals clear the need).
function M.nearest_losing(mx, my)
  local best, bd = nil, math.huge
  for _, c in ipairs(M.circles) do
    if c.losing and (c.need or 0) > 0 then
      local ddx, ddy = mx - c.cx, my - c.cy
      local d = ddx * ddx + ddy * ddy
      if d < bd then bd, best = d, c end
    end
  end
  return best, (best and math.sqrt(bd) or nil)
end

-- Nearest circle to a tile (for "which circle am I in / near"). Returns the
-- circle and its center distance, or nil.
function M.nearest(mx, my)
  local best, bd = nil, math.huge
  for _, c in ipairs(M.circles) do
    local ddx, ddy = mx - c.cx, my - c.cy
    local d = ddx * ddx + ddy * ddy
    if d < bd then bd, best = d, c end
  end
  if not best then return nil end
  return best, math.sqrt(bd)
end

-- Map overlay: ring + center dot + label (id, POI count, current ratio).
-- Also drives the circle_trend and circle_poi overlays when those are on.
function M.draw(viz, now, world)
  if viz == nil or not viz.is_on or not viz.circle then return end
  local show_main  = viz.is_on("circles")
  local show_trend = viz.is_on("circle_trend")
  local show_poi   = viz.is_on("circle_poi")

  if show_main or show_trend then
    for _, c in ipairs(M.circles) do
      local cx, cy = c.cx + 0.5, c.cy + 0.5
      local r, g, b
      if c.losing then r, g, b = 240, 70, 70           -- confirmed losing (both windows)
      elseif c.warning then r, g, b = 240, 160, 60      -- instant-count warning only
      elseif c.ratio >= 0.6 then r, g, b = 100, 220, 100
      elseif c.ratio <= 0.4 then r, g, b = 230, 140, 90
      else r, g, b = 220, 220, 120 end
      if show_main then
        viz.circle("circles", cx, cy, M.RADIUS, r, g, b, 70)
        viz.circle("circles", cx, cy, 0.5, r, g, b, 220)
        if c.safe_mx then viz.circle("circles", c.safe_mx + 0.5, c.safe_my + 0.5, 0.6, 120, 200, 255, 200) end
        if viz.text then
          local tag = c.losing and (" LOSING need=" .. (c.need or 0))
                      or (c.warning and " WARN" or "")
          viz.text("circles", cx, cy - 1.0,
                   string.format("circle#%d poi=%d r=%.2f%s", c.id, c.poi or 0, c.ratio or 0, tag),
                   "center", r, g, b, 230)
        end
      end
      if show_trend and viz.text then
        -- vs the 1 min and 5 min lookbacks; "(<win)" when that window isn't full.
        local ds = (c.ratio or 0) - (c.ratio_prev   or c.ratio or 0)
        local dl = (c.ratio or 0) - (c.ratio_origin or c.ratio or 0)
        local function arr(d) return d > 0.005 and "^" or (d < -0.005 and "v" or "=") end
        viz.text("circle_trend", cx, cy + 1.0,
                 string.format("1m %s%+.2f%s  5m %s%+.2f%s",
                               arr(ds), ds, c.short_ready and "" or "?",
                               arr(dl), dl, c.long_ready and "" or "?"),
                 "center", 200, 200, 255, 230)
      end
    end
  end

  -- POI coverage: green dot on each pill/base inside a circle, faint dot on
  -- those outside every circle.
  if show_poi and world then
    local r2 = M.RADIUS * M.RADIUS
    local function covered(mx, my)
      for _, c in ipairs(M.circles) do
        local ddx, ddy = mx - c.cx, my - c.cy
        if ddx * ddx + ddy * ddy <= r2 then return true end
      end
      return false
    end
    local function mark(mx, my)
      if covered(mx, my) then viz.circle("circle_poi", mx + 0.5, my + 0.5, 0.3, 120, 255, 120, 220)
      else viz.circle("circle_poi", mx + 0.5, my + 0.5, 0.3, 120, 120, 120, 130) end
    end
    if world.pills then for _, p in pairs(world.pills) do if (p.health or 0) > 0 then mark(p.mx, p.my) end end end
    if world.bases then for _, b in pairs(world.bases) do mark(b.mx, b.my) end end
  end
end

-- circle_history: the full lineage GRAPH of circle records (alive + destroyed
-- within the last 5 min). Each record drawn at its center with index +
-- created/(destroyed) ticks + current ratio; a line connects each record to its
-- predecessor (prev) so hand-offs at a spot read as a chain. Green = alive,
-- gray = destroyed.
function M.draw_history(viz, now)
  if viz == nil or not viz.is_on or not viz.is_on("circle_history") then return end
  if not viz.circle then return end
  local by = M._by_idx or {}
  for _, r in ipairs(M.history) do
    local x, y = r.cx + 0.5, r.cy + 0.5
    local alive = (r.destroyed == nil)
    local cr, cg, cb = (alive and 120 or 150), (alive and 230 or 130), (alive and 150 or 190)
    viz.circle("circle_history", x, y, 0.8, cr, cg, cb, alive and 210 or 130)
    if r.prev and by[r.prev] and viz.line then
      local p = by[r.prev]
      viz.line("circle_history", p.cx + 0.5, p.cy + 0.5, x, y, 200, 190, 120, 150)
    end
    if viz.text then
      viz.text("circle_history", x, y - 1.5,
               string.format("#%d c=%d%s r=%.2f", r.idx, r.created,
                             alive and " alive" or (" d=" .. r.destroyed), r.ratio or 0),
               "center", cr, cg, cb, 230)
    end
  end
end

-- circle_warning: advisory-only overlay (display, no dispatch). Orange ring +
-- the instant enemy-vs-friendly counts on circles whose instant counts flag a
-- warning. Hook real behavior to c.warning later.
function M.draw_warnings(viz, now)
  if viz == nil or not viz.is_on or not viz.is_on("circle_warning") then return end
  if not viz.circle then return end
  for _, c in ipairs(M.circles) do
    if c.warning then
      viz.circle("circle_warning", c.cx + 0.5, c.cy + 0.5, M.RADIUS, 240, 160, 60, 55)
      if viz.text then
        viz.text("circle_warning", c.cx + 0.5, c.cy + 1.6,
                 string.format("WARN enemy p%d/b%d/t%d vs p%d/b%d",
                               c.ep or 0, c.eb or 0, c.et or 0, c.fp or 0, c.fb or 0),
                 "center", 240, 160, 60, 235)
      end
    end
  end
end

-- Right-side HUD: one row per circle for tuning the trend / need logic.
function M.draw_hud(viz, now)
  if viz == nil or not viz.is_on or not viz.is_on("circles_hud") then return end
  if not viz.hud_text then return end
  local x, y, dy = 300, 150, 14
  viz.hud_text("circles_hud", x, y,
               string.format("%-3s %-5s %-5s %-5s %-7s %-7s %-3s %-4s", "id", "now", "prev", "orig", "fp/ep", "fb/eb", "et", "need"),
               "topright", 200, 200, 200, 220)
  y = y + dy
  for _, c in ipairs(M.circles) do
    local col = c.losing and { 240, 120, 120 } or { 170, 220, 170 }
    viz.hud_text("circles_hud", x, y,
                 string.format("%-3d %-5.2f %-5.2f %-5.2f %d/%-5d %d/%-5d %-3d %-4s",
                               c.id, c.ratio or 0, c.ratio_prev or 0, c.ratio_origin or 0,
                               c.fp or 0, c.ep or 0, c.fb or 0, c.eb or 0, c.et or 0,
                               c.losing and tostring(c.need or 0) or "-"),
                 "topright", col[1], col[2], col[3], 235)
    y = y + dy
  end
end

return M
