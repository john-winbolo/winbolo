local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/spike.lua -- focused spike rules, spike pairs and spike care.
--
-- Owns the rules added on top of the focused spike placement
-- (C.FOCUS_SPIKE_ENABLED, goals.lua focus_spike_ctx / score_cell). See the
-- FOCUS_SPIKE_* / SPIKE_* block in constants.lua for every knob.
--
--   check()      may this bot place a focused spike now, and in which role:
--                "single" (FOCUS_SPIKE_PAIR off: the first version, one deep
--                pill at a time), "first" (a pair: this tank carries the
--                second pill too) or "second" (the pair's second pill).
--                false = no focused spike: the normal aggro placement applies.
--   on_dispatch  init.lua PLACE_TRIP_SET, when the dispatched goal carries
--                _spike_role.
--   update()     once per tick from init.lua, before the trip lifecycle:
--                switches the win shares (update_win, FOCUS_SPIKE_WIN_SHARES)
--                and utility to aggro (FOCUS_SPIKE_UTIL_TO_AGGRO),
--                watches landings, moves the pair through its phases, ends a
--                failed pair (care on the first pill, area cooldown), reads
--                ally spkd adverts and new friendly pills on aggro tiles.
--   hold_row()   the tank waits at its dispatch tile while a pair trip is out.
--   care_at()    spike care for one tile: builder pool bonus, wider reach,
--                tank repair discount (C.SPIKE_REPAIR_PRIORITY).
--
-- State lives on state._spike. Loaded by goals.lua as the field M.spike (not
-- a local: goals.lua is at Lua's 200-local cap), and init.lua reaches it
-- through goals.spike so Brain.think() gets no new upvalue.
-- =========================================================================
local C          = require("constants")
local U          = require("util")
local PP         = require("pill_portfolio")
local ally_state = require("ally_state")
local print2     = require("print2")
-- The deny plan and the team vote (C.FOCUS_SPIKE_RULE = "deny").
local FV         = require("focus_vote")

local M = {}
M.vote = FV

-- "deny" (the team vote, focus_vote.lua) or "count" (the base lead and pill
-- rules of the first builds, per bot).
local function deny() return C.FOCUS_SPIKE_RULE == "deny" end

local function st(state)
  local S = state._spike
  if not S then
    S = { pair = nil, watch = {}, care = {}, cool = {}, known = nil, log_key = nil }
    state._spike = S
  end
  return S
end

local function tkey(mx, my) return my * 256 + mx end

-- A live friendly pill on this tile, on the ground.
local function pill_alive_at(world, mx, my)
  for _, p in pairs(world.pills or {}) do
    if p.mx == mx and p.my == my and not p.in_tank and p.owner == "friendly"
       and (p.health or 0) > 0 then
      return p
    end
  end
  return nil
end
M.pill_alive_at = pill_alive_at

local function pair_on()
  -- Deny mode: the vote's go sends every assigned builder at once, in
  -- place of the pair.
  if deny() then return false end
  local m = C.FOCUS_SPIKE_PAIR
  return m == "solo" or m == "team"
end

-- ── Counts, from this bot's own world model only ─────────────────────────
-- ours = friendly + allied bases. theirs = hostile bases, and with
-- C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY also neutral bases and every base whose owner
-- this bot does not know: a base on the map (world._bases_on_map, the map's
-- base list) with no world.bases record yet, or a record with no known owner.
-- Only this bot's own world model: it knows a base exists, not its owner
-- until it (or an ally) has seen it. Answers ours, theirs, hostile, neutral,
-- unknown.
function M.base_counts(world)
  local ours, hostile, neutral, unknown = 0, 0, 0, 0
  local known = {}
  for id, b in pairs(world.bases or {}) do
    known[id] = true
    local o = b.owner
    if o == "friendly" or o == "allied" then
      ours = ours + 1
    elseif o == "hostile" then
      hostile = hostile + 1
    elseif o == "neutral" then
      neutral = neutral + 1
    else
      unknown = unknown + 1
    end
  end
  local mask = world._bases_on_map
  if type(mask) == "number" then
    for id = 0, 31 do
      if bit.band(mask, bit.lshift(1, id)) ~= 0 and not known[id] then
        unknown = unknown + 1
      end
    end
  end
  local theirs = hostile
  if C.FOCUS_SPIKE_NEUTRAL_AS_ENEMY then theirs = hostile + neutral + unknown end
  return ours, theirs, hostile, neutral, unknown
end

-- Team pills: deployed live friendly pills (every role, utility too: a
-- placed pill in use as a blocker counts as utility) + pills in tanks as
-- the world model holds them (own "friendly", ally "allied") + any of this
-- tank's carried pills the world model does not list.
-- Returns total, deployed, own carried, ally carried.
function M.team_pills(world, info, pf_counts)
  local own_listed, ally_c = 0, 0
  for _, p in pairs(world.pills or {}) do
    if p.in_tank then
      if p.owner == "friendly" then own_listed = own_listed + 1
      elseif p.owner == "allied" then ally_c = ally_c + 1 end
    end
  end
  -- pf_counts.utility holds the in-tank pills too (PP.counts); take those
  -- out so they are not counted twice.
  local deployed = 0
  for _, v in pairs(pf_counts) do deployed = deployed + v end
  deployed = math.max(0, deployed - own_listed - ally_c)
  local own = math.max(info.carried_pills or 0, own_listed)
  return deployed + own + ally_c, deployed, own, ally_c
end

-- ── The rules ────────────────────────────────────────────────────────────
-- The base lead rule and the pill rule. Answers why ("ok", "base_lead",
-- "pills", "front") and the numbers behind it. A rule whose knob is off
-- passes. The front test reads the NORMAL targets (PP.normal_targets), so the
-- win shares below do not change the answer they are switched by.
function M.rules(world, info, pf_counts)
  local why = "ok"
  local ours, theirs, b_host, b_neut, b_unk = M.base_counts(world)
  local total, deployed, own, ally_c = M.team_pills(world, info, pf_counts)
  local ftgt = PP.normal_targets(total).front
  local need_ours = theirs * C.FOCUS_SPIKE_BASE_RATIO
  if C.FOCUS_SPIKE_BASE_RULE
     and (ours < need_ours or (ours - theirs) < C.FOCUS_SPIKE_BASE_LEAD) then
    why = "base_lead"
  elseif C.FOCUS_SPIKE_PILL_RULE and total < C.FOCUS_SPIKE_MIN_PILLS then
    why = "pills"
  elseif C.FOCUS_SPIKE_PILL_RULE and (pf_counts.front or 0) < ftgt then
    why = "front"
  end
  return why, { ours = ours, theirs = theirs, need_ours = need_ours,
                b_host = b_host, b_neut = b_neut, b_unk = b_unk,
                total = total, deployed = deployed, own = own, ally_c = ally_c,
                ftgt = ftgt }
end

-- Win shares (C.FOCUS_SPIKE_WIN_SHARES): the portfolio targets switch to the
-- FOCUS_SPIKE_WIN_* shares while M.rules passes, and back when it stops (or
-- when a knob turns off). One FOCUS_SPIKE_WIN_SHARES line per switch.
-- Utility to aggro (C.FOCUS_SPIKE_UTIL_TO_AGGRO): at the same moment the
-- utility share becomes 0 and goes to aggro (PP.set_u2a), on top of the win
-- shares or the normal shares. One FOCUS_SPIKE_UTIL_TO_AGGRO line per switch.
function M.update_win(state, world, info, now)
  local want, want_u2a = false, false
  local why, r, pf = "off", nil, nil
  if deny() then
    -- Deny mode: only a passed team vote switches the shares, for the
    -- whole team, while its plan is active.
    local act = C.FOCUS_SPIKE_ENABLED and C.PILL_PLACE_TURTLE ~= true and FV.active(state)
    why = act and "ok" or "no_vote"
    want = act and C.FOCUS_SPIKE_WIN_SHARES == true
    want_u2a = act and C.FOCUS_SPIKE_UTIL_TO_AGGRO == true
    local before = PP.targets_label()
    if PP.set_win(want) then
    end
    before = PP.targets_label()
    if PP.set_u2a(want_u2a) then
    end
    return
  end
  if C.FOCUS_SPIKE_ENABLED and C.PILL_PLACE_TURTLE ~= true
     and (C.FOCUS_SPIKE_WIN_SHARES or C.FOCUS_SPIKE_UTIL_TO_AGGRO) then
    -- peek: this runs every tick, so it must not write the cached pill
    -- roles (that moved the re-evaluation times and changed play even
    -- while the rules failed).
    pf = PP.counts(world, now, true)
    why, r = M.rules(world, info, pf)
    want = (why == "ok") and C.FOCUS_SPIKE_WIN_SHARES == true
    want_u2a = (why == "ok") and C.FOCUS_SPIKE_UTIL_TO_AGGRO == true
  end
  if PP.set_win(want) then
    local n = PP.normal
    if r and C.FOCUS_SPIKE_WIN_SHARES then
    else
    end
  end
  local before = PP.targets_label()
  if PP.set_u2a(want_u2a) then
  end
end

-- Returns false (no focused spike) or a table { role = ... }.
-- quiet: no FOCUS_SPIKE_RULES line (harvest re-score, heatmap overlay).
function M.check(state, world, info, pf_counts, quiet)
  if not C.FOCUS_SPIKE_ENABLED or C.PILL_PLACE_TURTLE == true then return false end
  -- Deny mode: a focused spike only on this bot's tiles of a passed plan.
  if deny() then
    local tiles = FV.my_tiles(state, info)
    if not tiles or (info.carried_pills or 0) < 1 then return false end
    return { role = "plan", plan = tiles }
  end
  local S = st(state)
  local now = state.tick or 0
  local P = S.pair
  local paired = pair_on()
  local carried = info.carried_pills or 0
  -- An open pair. The second pill skips the base and pill rules: they were
  -- met when the first one went.
  if paired and P then
    if P.phase == "second_wait" and info.man_status == C.LGM_INTANK
       and not info.inboat and carried >= 1
       and (now - (P.t_first or now)) <= C.SPIKE_PAIR_WINDOW then
      return { role = "second", fmx = P.mx, fmy = P.my }
    end
    return false
  end
  local why, r = M.rules(world, info, pf_counts)
  local ours, theirs, need_ours = r.ours, r.theirs, r.need_ours
  local total, deployed, own, ally_c, ftgt = r.total, r.deployed, r.own, r.ally_c, r.ftgt
  if why == "ok" and paired and (carried < 2
         or (info.trees or 0) < 2 * (C.PILL_PLACE_TREE_COST or 4)) then
    why = "no_second_pill"
  end
  local role = (why ~= "ok") and "none" or (paired and "first" or "single")
  if not quiet then
    local key = role .. ":" .. why
    if S.log_key ~= key then
      S.log_key = key
    end
  end
  if why ~= "ok" then return false end
  return { role = role }
end

-- ── Tile rules ───────────────────────────────────────────────────────────
function M.in_cool(state, mx, my, now)
  local S = state._spike
  if not S then return false end
  for _, c in ipairs(S.cool) do
    if c.until_t > now and U.mdist(mx, my, c.mx, c.my) <= C.SPIKE_FAIL_COOLDOWN_RADIUS then
      return true
    end
  end
  return false
end

-- May (cx,cy) be a focused spike tile for this role? "single" keeps the first
-- version's rule (every aggro tile). Pair roles: not forest (a harvest trip
-- would turn the spike into a tree run) and not in a failed pair's cooldown.
function M.deep_ok(ad, cx, cy)
  if ad.role == "single" then return true end
  if U.ttype(cx, cy) == C.T_FOREST then return false end
  if ad.state and M.in_cool(ad.state, cx, cy, ad.now or 0) then return false end
  return true
end

-- Clear line between two tiles: no building or half building on the tiles
-- between them (the same tiles los_coverage stops at).
function M.los_clear(ax, ay, bx, by)
  local dx, dy = bx - ax, by - ay
  local n = math.max(math.abs(dx), math.abs(dy))
  for i = 1, n - 1 do
    local x = math.floor(ax + dx * i / n + 0.5)
    local y = math.floor(ay + dy * i / n + 0.5)
    local tt = U.ttype(x, y)
    if tt == C.T_BUILDING or tt == C.T_HALFBUILD then return false end
  end
  return true
end

-- The pair's second tile: within SPIKE_PAIR_COVER_DIST (euclid) of the first
-- spike with a clear line to it, inside the long send from the tank, not
-- forest, not the first tile itself.
function M.second_ok(ad, cx, cy)
  if cx == ad.fmx and cy == ad.fmy then return false end
  if U.edist(cx, cy, ad.fmx, ad.fmy) > C.SPIKE_PAIR_COVER_DIST then return false end
  if ad.tmx and U.mdist(ad.tmx, ad.tmy, cx, cy) > C.PLACE_DISPATCH_MAX_DIST then return false end
  if U.ttype(cx, cy) == C.T_FOREST then return false end
  return M.los_clear(ad.fmx, ad.fmy, cx, cy)
end

-- ── Care tags ────────────────────────────────────────────────────────────
local function tag(S, mx, my, t, src)
  local k = tkey(mx, my)
  local e = S.care[k]
  if e and e.t >= t and (e.src == "own" or src ~= "own") then return end
  if e and e.src == "plan" and src == "plan" then return end
  S.care[k] = { mx = mx, my = my, t = t, src = src }
end

function M.care_at(state, mx, my, now)
  if not C.SPIKE_REPAIR_PRIORITY then return nil end
  local S = state._spike
  if not S then return nil end
  local e = S.care[tkey(mx, my)]
  if not e then return nil end
  if (now - e.t) > C.SPIKE_CARE_TICKS then return nil end
  -- A plan tile keeps care for its time whatever its category.
  if e.src ~= "plan" and PP.classify(mx, my, false) ~= "aggro" then return nil end
  return e
end

-- Tank repair_pill cost factor for a spike pill, or nil.
function M.tank_mult(state, mx, my, now)
  if not M.care_at(state, mx, my, now) then return nil end
  return 1 - C.SPIKE_REPAIR_TANK_DISCOUNT
end

-- ── Pair lifecycle ───────────────────────────────────────────────────────
function M.on_dispatch(state, goal, mx, my, tmx, tmy, now)
  local role = goal and goal._spike_role
  if not role then return end
  if role == "plan" then
    FV.on_dispatch(state, mx, my, now)
    return
  end
  local S = st(state)
  S.watch[tkey(mx, my)] = { mx = mx, my = my, t = now }
  if role == "first" then
    local P = S.pair
    if P and P.mx == mx and P.my == my and P.phase == "first_out" then return end
    S.pair = { phase = "first_out", mx = mx, my = my, tmx = tmx, tmy = tmy,
               t_disp = now, left = false }
  elseif role == "second" then
    local P = S.pair
    if P and P.phase == "second_wait" then
      P.phase, P.mx2, P.my2, P.t_disp2, P.left2 = "second_out", mx, my, now, false
    end
  end
end

-- End a pair that did not finish. A first pill that landed gets spike care
-- from now; the area around the first tile gets no focused spike for
-- SPIKE_FAIL_COOLDOWN ticks.
function M.fail(state, why, now)
  local S = st(state)
  local P = S.pair
  if not P then return end
  S.pair = nil
  local landed = P.t_first ~= nil
  if landed then tag(S, P.mx, P.my, now, "own") end
  S.cool[#S.cool + 1] = { mx = P.mx, my = P.my, until_t = now + C.SPIKE_FAIL_COOLDOWN }
end

local function update_pair(state, S, world, info, now)
  local P = S.pair
  if not P then return end
  local man = info.man_status
  if P.phase == "first_out" then
    if not P.t_first and pill_alive_at(world, P.mx, P.my) then P.t_first = now end
    if man == C.LGM_DEAD then
      M.fail(state, "lgm_lost", now)
    elseif man ~= C.LGM_INTANK then
      P.left = true
      if (now - P.t_disp) > (C.PLACE_TRIP_MAX_TICKS or 2000) then
        M.fail(state, "trip_timeout", now)
      end
    elseif P.left then
      if P.t_first then
        P.phase = "second_wait"
        tag(S, P.mx, P.my, P.t_first, "own")
      else
        M.fail(state, "first_not_built", now)
      end
    elseif (now - P.t_disp) > (C.PLACE_TRIP_MAX_TICKS or 2000) then
      M.fail(state, "never_left", now)
    end
  elseif P.phase == "second_wait" then
    if (now - P.t_first) > C.SPIKE_PAIR_WINDOW then
      M.fail(state, "window", now)
    elseif man == C.LGM_DEAD then
      M.fail(state, "lgm_lost", now)
    elseif man == C.LGM_INTANK and (info.carried_pills or 0) < 1 then
      M.fail(state, "no_pill", now)
    end
  elseif P.phase == "second_out" then
    if pill_alive_at(world, P.mx2, P.my2) then
      tag(S, P.mx2, P.my2, now, "own")
      tag(S, P.mx, P.my, P.t_first, "own")
      S.watch[tkey(P.mx2, P.my2)] = nil
      S.pair = nil
    elseif (now - P.t_first) > C.SPIKE_PAIR_WINDOW then
      M.fail(state, "window", now)
    elseif man == C.LGM_DEAD then
      M.fail(state, "lgm_lost", now)
    elseif man ~= C.LGM_INTANK then
      P.left2 = true
    elseif P.left2 then
      M.fail(state, "second_not_built", now)
    elseif (now - P.t_disp2) > (C.PLACE_TRIP_MAX_TICKS or 2000) then
      M.fail(state, "never_left", now)
    end
  end
end

-- New friendly pill on a focused spike tile (aggro, and within PILL_FIRE_RANGE
-- of a hostile base this bot knows) = a spike some ally placed. A tile is
-- "new" when no pill of any owner stood on it at the last look; the first
-- look only records.
local function scan_seen(S, world, now)
  local known = S.known
  local first = known == nil
  local next_known = {}
  local fresh = nil
  for _, p in pairs(world.pills or {}) do
    if not p.in_tank and p.mx then
      local k = tkey(p.mx, p.my)
      next_known[k] = true
      if not first and not known[k] and p.owner == "friendly"
         and (p.health or 0) > 0 then
        fresh = fresh or {}
        fresh[#fresh + 1] = p
      end
    end
  end
  S.known = next_known
  if not fresh then return end
  for _, p in ipairs(fresh) do
    if not S.care[tkey(p.mx, p.my)] and PP.classify(p.mx, p.my, false) == "aggro" then
      for _, b in pairs(world.bases or {}) do
        if b.owner == "hostile" and U.mdist(p.mx, p.my, b.mx, b.my) <= C.PILL_FIRE_RANGE then
          tag(S, p.mx, p.my, now, "seen")
          break
        end
      end
    end
  end
end

-- Ally adverts: spkd = one or two "XXYYCCCC" (tile, landing tick low 16 bits).
local function read_adverts(S, info, now)
  local maxage = C.SPIKE_CARE_TICKS
  local adds = nil
  for pn, slot in ally_state.iter_active(now, maxage) do
    if pn ~= info.player_number then
      local s = slot.info and slot.info.spkd
      if s and s ~= "-" then
        for i = 1, #s - 7, 8 do
          local mx = tonumber(string.sub(s, i, i + 1), 16)
          local my = tonumber(string.sub(s, i + 2, i + 3), 16)
          local c16 = tonumber(string.sub(s, i + 4, i + 7), 16)
          if mx and my and c16 then
            local age = bit.band(now - c16, 0xFFFF)
            if age <= maxage then
              adds = adds or {}
              adds[#adds + 1] = { mx = mx, my = my, t = now - age, pn = pn }
            end
          end
        end
      end
    end
  end
  if not adds then return end
  for _, a in ipairs(adds) do
    local e = S.care[tkey(a.mx, a.my)]
    if not e then
    end
    tag(S, a.mx, a.my, a.t, "ally")
  end
end

function M.update(state, world, info, now)
  if C.FOCUS_SPIKE_ENABLED or PP.win_on or PP.u2a_on then M.update_win(state, world, info, now) end
  if not C.FOCUS_SPIKE_ENABLED and not C.SPIKE_REPAIR_PRIORITY then return end
  if deny() then
    if not C.FOCUS_SPIKE_ENABLED then return end
    -- The deny plan and the vote. Every bot tags the plan tiles it sees
    -- placed (src "plan"), so no spkd advert is needed.
    local S = st(state)
    FV.update(state, world, info, now, function(mx, my, t) tag(S, mx, my, t, "plan") end)
    for k, e in pairs(S.care) do
      if (now - e.t) > C.SPIKE_CARE_TICKS then S.care[k] = nil end
    end
    return
  end
  local S = st(state)
  -- Watches: a dispatched spike that lands (late landings too).
  for k, w in pairs(S.watch) do
    if pill_alive_at(world, w.mx, w.my) then
      tag(S, w.mx, w.my, now, "own")
      S.watch[k] = nil
    elseif (now - w.t) > (C.PLACE_TRIP_MAX_TICKS or 2000) then
      S.watch[k] = nil
    end
  end
  if pair_on() then
    update_pair(state, S, world, info, now)
  elseif S.pair then
    S.pair = nil   -- knob changed mid-game
  end
  if C.SPIKE_REPAIR_PRIORITY then
    scan_seen(S, world, now)
    read_adverts(S, info, now)
  end
  -- Prune care tags: out of time, or the tile is no longer aggro.
  for k, e in pairs(S.care) do
    if (now - e.t) > C.SPIKE_CARE_TICKS or PP.classify(e.mx, e.my, false) ~= "aggro" then
      S.care[k] = nil
    end
  end
  local i = 1
  while i <= #S.cool do
    if S.cool[i].until_t <= now then table.remove(S.cool, i) else i = i + 1 end
  end
end

-- ── Tank hold and builder pool wait ──────────────────────────────────────
-- A pair trip is out: the tank waits where it stood at the dispatch.
function M.holding(state, info)
  local S = state._spike
  local P = S and S.pair
  if not P then return false end
  return (P.phase == "first_out" or P.phase == "second_out")
         and info.man_status ~= C.LGM_INTANK
end

function M.hold_row(state, info, tmx, tmy)
  if not pair_on() or not M.holding(state, info) then return nil end
  local P = state._spike.pair
  local cost = C.PLACE_FOLLOW_THROUGH_COST or 25
  local tx = P.mx2 or P.mx
  local ty = P.my2 or P.my
  return {
    cost = cost,
    goal = { kind = "place_pill_strategic", mx = P.tmx, my = P.tmy,
             wx = U.m2w(P.tmx), wy = U.m2w(P.tmy),
             -- follow_through: cost pinned past the phase weight and no
             -- dispatch from builder.set_mode (the man is already out).
             follow_through = true, spike_hold = true,
             trip_mx = tx, trip_my = ty },
    desc = BRAIN_POOL_VIZ and string.format(
           "SPIKE_HOLD %s trip@(%d,%d) first=(%d,%d) hold=(%d,%d) tank=(%d,%d) cost{%.0f} (PLACE_FOLLOW_THROUGH_COST)",
           P.phase, tx, ty, P.mx, P.my, P.tmx, P.tmy, tmx, tmy, cost) or "",
    cands = {
      { id = ty * 256 + tx, mx = P.tmx, my = P.tmy, cost = cost,
        own = "self", hp = 0, stale = 0 },
    },
  }
end

-- The pair's second trip is due: the builder pool keeps the man for it.
function M.second_pending(state)
  local S = state._spike
  local P = S and S.pair
  return P ~= nil and P.phase == "second_wait" and pair_on()
end

-- ── Advert ───────────────────────────────────────────────────────────────
-- spkd: this bot's two most recent own spikes under care, "XXYYCCCC" each,
-- or "-". nil (key not sent) with SPIKE_REPAIR_PRIORITY off.
function M.advert(state, now)
  if not C.SPIKE_REPAIR_PRIORITY then return nil end
  -- Deny mode: every bot tags the plan tiles itself (no key sent).
  if deny() then return nil end
  local S = state._spike
  if not S then return "-" end
  local list = {}
  for k, e in pairs(S.care) do
    if e.src == "own" and (now - e.t) <= C.SPIKE_CARE_TICKS then
      list[#list + 1] = { k = k, e = e }
    end
  end
  if #list == 0 then return "-" end
  table.sort(list, function(a, b)
    if a.e.t ~= b.e.t then return a.e.t > b.e.t end
    return a.k < b.k
  end)
  local out = {}
  for i = 1, math.min(2, #list) do
    local e = list[i].e
    out[#out + 1] = string.format("%02X%02X%04X", bit.band(e.mx, 0xFF),
                                  bit.band(e.my, 0xFF), bit.band(e.t, 0xFFFF))
  end
  return table.concat(out)
end

return M
