-- =========================================================================
-- pool_render.lua -- the pool panel's text, built from a raw capture.
--
-- Recording a bot used to build the whole pool-panel JSON (every row's
-- formula text) inside the brain, on every tick, for every bot. This module
-- holds all of that text building, so the brain only takes a cheap raw
-- capture (goals.capture_pool_raw) and the text is built later:
--
--   * in the brain, when a caller still asks for JSON
--     (goals.get_pool_breakdown_json = render(capture));
--   * in BrainTest and brainrec_split, from a raw block in a recording, in a
--     plain lua_State that has only this file, json.lua, bitcompat.lua and
--     constants.lua on its package.path.
--
-- THIS FILE MUST LOAD WITHOUT THE BRAIN. It requires only json, bitcompat and
-- (for its default constants) constants. Everything the text needs from the
-- live brain arrives in the capture: the brain's own constant values (C_NAMES
-- below), the multipliers goal_selection used, the threat values the pool-3
-- rows print, and the brain tables the rows are built from.
--
-- The brain modules that print the same text in their debug lines (goals,
-- builder_pool, orders) call the functions here, so each string has one
-- source.
-- =========================================================================
local bit  = require("bitcompat")
local json = require("json")

local M = {}

-- Constants the text reads. In the brain this is the bot's own constants
-- table (per-bot cfg= and preset values included). render() binds the
-- capture's copy for the length of one render and then puts this one back.
local C
do
  local ok, c = pcall(require, "constants")
  C = ok and c or {}
end

-- Live hooks for the two values the text reads that are not on the entry:
-- threat.at() for the pool-3 nobuild_danger chip, and the attack SHELLS_LOW
-- line. goals.lua binds the live functions; render() binds the capture's.
local H = {
  threat_at  = function() return 0 end,
  shells_low = function() return C.SHELLS_LOW end,
}

function M.bind(c, hooks)
  if c then C = c end
  if hooks then
    H = { threat_at = hooks.threat_at or H.threat_at,
          shells_low = hooks.shells_low or H.shells_low }
  end
end

-- Moved here from goals.lua so the WINNERS chip and the goals debug lines
-- print the same string. mult/src are goals' REFUEL_MULT and its source.
function M.refuel_mult_label(mult, src)
  return string.format("refuelmult{x%.2f (%s)}", mult, src)
end

-- The same as goals.turtle_far_search_cap (pool_render.lua loads without
-- goals.lua): the distance where the turtle_far multiplier reaches the cap.
local function turtle_far_search_cap()
  local t = C.TURTLE_BASE_DOUBLE_TILES or 0
  local cap = C.TURTLE_BASE_MULT_CAP or 1
  if t <= 0 or cap <= 1 then return 0 end
  local lim = math.ceil(t * math.log(cap) / math.log(2))
  if lim > 64 then lim = 64 end
  if lim < 1 then lim = 1 end
  return lim
end

-- Breakdown chip and detail line for one row. Empty when there is no term.
-- src = "centre": d was measured to the nearest friendly centre (see
-- goals.turtle_far), shown as d=..(centre).
function M.turtle_far_text(d, m, src)
  if d == nil then return "", "" end
  local lim = turtle_far_search_cap()
  local dtxt = (d >= lim) and string.format(">=%d", d) or tostring(d)
  local ctr = (src == "centre") and "(centre)" or ""
  local how = (src == "centre")
    and "the Chebyshev distance to the nearest friendly base/pill centre (provisional until this base's ring search ends, or the fallback when enemy influence cancels every friendly centre)"
    or "Chebyshev tiles to the nearest tile with influence > 0 (our ground)"
  return string.format(" x turtle_far{d=%s%s,x%.2f}", dtxt, ctr, m), string.format(
    "|turtle_far:Turtle2 mode bot (mode=turtle2). d=%s%s is %s. x min(2^(%d/%g[TURTLE_BASE_DOUBLE_TILES]), %g[TURTLE_BASE_MULT_CAP]) = x%.2f, applied to the row sum before any snap floor. The search stops at %d tiles, where the multiplier reaches the cap; d is rebuilt every %d ticks by one C distance transform (on an older exe without it, by a Lua ring search spread over thinks)",
    dtxt, ctr, how, d, C.TURTLE_BASE_DOUBLE_TILES or 0, C.TURTLE_BASE_MULT_CAP or 1, m,
    lim, C.TURTLE_BASE_D_REFRESH_TICKS or 50)
end

-- A builder walk distance for a log or panel line: one decimal when it is
-- not a whole number (straight line), the old integer text when it is.
-- The same as util.fmt_ld (pool_render.lua loads without util.lua).
function M.fmt_ld(d)
  if d == nil then return "-" end
  if d == math.floor(d) then return string.format("%d", d) end
  return string.format("%.1f", d)
end

-- Moved here from orders.lua (squad.focus_label / orders.focus_label call it).
function M.focus_label(focus)
  return string.format("focus: x%.1f (%s)", C.FOCUS_OTHER_COST_MULT or 1.0,
                       tostring(focus or "off"))
end


-- ---- helpers moved from goals.lua ----
-- One chip per markup so every log line / panel says the same thing: which
-- resource the base is short of and what that multiplied the cost by.
local function refuel_low_stock_chip(d)
  if not d then return "" end
  local s = ""
  if d.low_arm then
    s = s .. string.format(" lowarm{x%.2f}", C.REFUEL_LOW_ARMOUR_MULT or 1.0)
  end
  if d.low_sh then
    s = s .. string.format(" lowsh{x%.2f}", C.REFUEL_LOW_SHELLS_MULT or 1.0)
  end
  return s
end


-- Mine-hoard chip text, shared by the REFUEL_SHAPE debug line and the pool-viz
-- term breakdown so the two can never disagree. `md` is refuel_shape's
-- mine-detail table. Returns:
--   head — the short value that goes inside mines{...}. The panel's formula
--          parser DROPS a term whose braced value reaches 32 characters, so
--          this stays tiny and the derivation lives in `why`.
--   why  — the whole thing, hand-computable: the exponential written out with
--          its constants, whether it was charged, and every number the
--          waive decision looked at.
local function mine_chip(md)
  local head = md.waived
    and string.format("0 of %.1f waived", md.raw)
    or  string.format("%.1f", md.raw)
  local why = string.format(
    "%.1f = %g[MINE_HOARD_WEIGHT] x (%g[MINE_HOARD_BASE]^(%d mines - %d[MINE_FREE]) - 1)"
    .. "; %s: arm %d/%d (%s), sh %d/%d (%s); base under us arm=%s sh=%s,"
    .. " MIN_STOCK=%d",
    md.raw, md.weight, md.expbase, md.carried, md.free,
    (not md.knob) and "CHARGED (NEEDS_SUPPLY off)"
      or (md.waived and "WAIVED (below a target this base can still supply)"
                    or "CHARGED (this base supplies nothing we still need)"),
    md.arm, md.arm_target, md.need_arm and "below" or "at",
    md.sh, md.sh_target, md.need_sh and "below" or "at",
    md.has_base and tostring(md.b_arm) or "none",
    md.has_base and tostring(md.b_sh) or "none",
    md.min_stock)
  return head, why
end

-- The breakdown text for M.capture_base_extra's answer: a chip for the row
-- and a detail line, so the panel shows the extra was ADDED or WAIVED and why.
function M.capture_base_extra_text(extra, d, free)
  if d == nil then return "", "" end
  local full = C.CAPTURE_BASE_EXTRA_COST or 0
  if extra == 0 then
    return " + extra{0 waived}", string.format(
      "|extra:CAPTURE_BASE_EXTRA_COST{%.0f} WAIVED, 0 added: the base is %d tiles (Manhattan) from the tank, within CAPTURE_BASE_EXTRA_FREE_DIST{%d}",
      full, d, free)
  end
  local why = (free > 0)
    and string.format("the base is %d tiles (Manhattan) from the tank, beyond CAPTURE_BASE_EXTRA_FREE_DIST{%d}", d, free)
    or "CAPTURE_BASE_EXTRA_FREE_DIST is 0, so it applies at every distance"
  return string.format(" + extra{%.0f}", extra), string.format(
    "|extra:CAPTURE_BASE_EXTRA_COST{%.0f} ADDED to this capture_base row (a game mode's knob; keel 0 = no such term): %s",
    extra, why)
end

-- Shared detail-string helpers (appended after || in formulas for the popup).
local function fmt_stale_detail(age, stale_cost)
  age = age or 0
  if age <= C.STALE_PENALTY_START then
    return string.format("age=%.0f < %.0f[STALE_PENALTY_START] → 0",
      age, C.STALE_PENALTY_START)
  end
  local raw = (age - C.STALE_PENALTY_START) * C.STALE_PENALTY_PER_TICK
  return string.format(
    "max((%.0f[age] - %.0f[STALE_PENALTY_START]) x %.2f[STALE_PENALTY_PER_TICK], 0)"..
    " → max(%.0f x %.2f, 0) → max(%.0f, 0) → %.0f",
    age, C.STALE_PENALTY_START, C.STALE_PENALTY_PER_TICK,
    age - C.STALE_PENALTY_START, C.STALE_PENALTY_PER_TICK,
    raw, stale_cost)
end

-- Lazy formula builder for cost_cache entries.
-- step_eval_queue stores raw component numbers (_p pool-tag + _dc table)
-- instead of building formula strings on every tick. This function builds
-- the display string on first access (cold path: BrainTest UI only) and
-- caches the result in e.formula so subsequent calls are free.
local reject_with_breakdown   -- forward decl (mutual recursion w/ get_formula_inner)
local function get_formula_inner(e)
  local p = e._p
  local raw = e.raw
  local f
  -- Bot-command ORDER reject row. While this bot holds a chat order every
  -- other STRATEGIC candidate stays in its pool but is priced out, so the
  -- panel still shows what it would have cost and who took the decision away.
  if e._reject == "order" then
    return reject_with_breakdown(e, "REJECT order",
      string.format("reject:order — %s's order %s #%s, %d s left",
        tostring(e._order_by), tostring(e._order_kind),
        tostring(e._order_tid), e._reject_remaining or 0))
  end

  -- Generic ally-claimed REJECT row.  Pools 2/3/4/5/6/7 use the per-tick
  -- sync to set _reject="ally_claimed" against fresh ally_state.  We
  -- render a uniform row so the pool grid shows who out-bid us and by
  -- how much.
  if e._reject == "ally_claimed" then
    local rem = e._reject_remaining or 0
    local their = e._ally_score
    local by    = e._ally_by
    local our   = e.cost or 0
    -- Joinable-blitz case: an ally has an OPEN blitz call on this pill, so it's
    -- not a solo ally_claim we're locked out of — it's a blitz we *can* join and
    -- simply aren't (yet), e.g. too far. Label it honestly; "ally_claimed" is
    -- reserved for a truly-closed take we can't participate in.
    if e._reject_joinable_blitz then
      return reject_with_breakdown(e,
        string.format("REJECT blitz(joinable) @(%d,%d)", e._mx or 0, e._my or 0),
        string.format("reject:blitz — p%s is running a JOINABLE blitz here; join via the squad layer (in help range) rather than soloing it. Not joining now (out of join range / busy).",
          tostring(by or "?")))
    end
    local their_str = their and string.format("%.0f", their) or "?"
    local diff_str  = (their and string.format(" (we_more_by=%.0f)", our - their)) or ""
    -- Say which price this pool-6 row was judged on (always the raw cost_cache
    -- cost -- see sync_ally_claimed_rejects) and what the last outgoing-request
    -- decision was, so the row is reproducible.
    local steal_str = ""
    if e._steal_units then
      steal_str = string.format(" — priced on OUR %s cost %.1f (raw cost_cache %.1f)",
        e._steal_units, e._steal_cost or our, our)
    end
    if e._steal_reason then steal_str = steal_str .. " — " .. tostring(e._steal_reason) end
    if e._steal_note then steal_str = steal_str .. " || request: " .. tostring(e._steal_note) end
    return reject_with_breakdown(e,
      string.format("REJECT ally_claimed %dt @(%d,%d)", rem, e._mx or 0, e._my or 0),
      string.format("reject:ally_claimed %dt — p%s bid %s < ours %.0f%s%s",
        rem, tostring(by or "?"), their_str, our, diff_str, steal_str))
  end
  if e._reject == "armour_too_low" then
    return reject_with_breakdown(e,
      string.format("REJECT armour_too_low @(%d,%d)", e._mx or 0, e._my or 0),
      string.format("reject:armour_too_low — arm=%d (need >= %d) vs pillHP=%d (>= %d)",
        e._armour_at_reject or 0, C.ATTACK_PILL_UNSAFE_ARMOUR_FLOOR,
        e._pillhp_at_reject or 0, C.ATTACK_PILL_UNSAFE_HP_THRESHOLD))
  end
  if e._reject == "blitz_only" then
    return reject_with_breakdown(e,
      string.format("REJECT blitz_only @(%d,%d)", e._mx or 0, e._my or 0),
      string.format("reject:blitz_only — pills only inside a blitz (%s); this pill is not our blitz, has no open call to join, and we cannot lead one: %s%s",
        tostring(e._blitz_only_src), tostring(e._blitz_only_why),
        e._blitz_only_desc and (" — allies: " .. e._blitz_only_desc) or ""))
  end
  if e._reject == "ally_pill_take_priority" then
    local rem = e._reject_remaining or 0
    local by  = e._priority_by
    return reject_with_breakdown(e,
      string.format("REJECT ally_pill_take_priority %dt @(%d,%d)", rem, e._mx or 0, e._my or 0),
      string.format("reject:ally_pill_take_priority %dt — p%s is doing the take, holding off ~%.1fs",
        rem, tostring(by or "?"), rem / 50.0))
  end
  if p == 1 then
    -- Rejected refuel base: short-circuit with REJECT formula so the
    -- breakdown panel makes clear why the row exists with INF cost.
    -- Cost compute was skipped at queue-build time.
    if e._reject then
      local rem = e._reject_remaining or 0
      local rem_tok = (e._reject == "blocked" or e._reject == "stale"
                       or e._reject == "depleted")
                      and string.format(" %dt", rem) or ""
      local desc = ({
        blocked  = "tile blocked (cooldown)",
        stale    = "neutral, unseen too long",
        depleted = "recently observed low on stock we need",
        danger   = "pill fire on base — too dangerous to refuel",
      })[e._reject] or e._reject
      e.formula = string.format(
        "REJECT %s%s @(%d,%d)||reject:%s%s — %s",
        e._reject, rem_tok, e._mx or 0, e._my or 0,
        e._reject, rem_tok, desc)
      return e.formula
    end
    local _lgm_mult_d = e._lgm_mult or 1
    local _d_danger
    if _lgm_mult_d ~= 1 then
      _d_danger = string.format(
        "%.1f[danger_val] x %.1f[REFUEL_DANGER_WEIGHT] x %d (cautious mode) = %.0f",
        e._dv, C.REFUEL_DANGER_WEIGHT, _lgm_mult_d, e._dang)
    else
      _d_danger = string.format("%.1f[danger_val] x %.1f[REFUEL_DANGER_WEIGHT] = %.0f",
        e._dv, C.REFUEL_DANGER_WEIGHT, e._dang)
    end
    -- Refuel-pad reach (goals.lua refuel_pad_pill_reach): the danger_val above
    -- is a PARKED read, not a driving one. Say which verdict produced it, with
    -- the two distances compared, and — when the pill layer was dropped — what
    -- came off, so danger_val is still hand-derivable from threat.at().
    local _pad_thresh = (C.PILL_FIRE_RANGE or 8) + (C.REFUEL_PAD_TANK_OFFSET or 0.7071)
    local _pad_token = ""
    if e._pad_safe ~= nil then
      _pad_token = (e._pad_safe
        and ((e._pad_pill == nil)
             and " padsafe{no live hostile pill on the map}"
             or string.format(" padsafe{nearest #%s at %.1f > %.2f}",
                              tostring(e._pad_pill), e._pad_dist or 0, _pad_thresh))
        or string.format(" padhit{#%s reaches: %.1f <= %.2f}",
                         tostring(e._pad_pill), e._pad_dist or 0, _pad_thresh))
      if e._pad_safe then
        _d_danger = _d_danger .. string.format(
          " -- PAD SAFE: %s, so the whole PILL layer (%.1f) was REMOVED from "
          .. "threat.at() and danger_val is the TANK-layer remainder %.1f. "
          .. "A pill fires on a tank at <= %.0f[PILL_FIRE_RANGE] tiles from its "
          .. "own tile centre (util.c utilIsItemInRange, PILLBOX_RANGE 2048wu), "
          .. "and a docked tank is at most %.4f[REFUEL_PAD_TANK_OFFSET] tiles "
          .. "off the base centre, so nothing past %.2f can touch the pad. "
          .. "(The PILL_RANGE_MAP %d driving stamp is unchanged.)",
          (e._pad_pill == nil) and "there is no live hostile/neutral pill on the map"
            or string.format("the nearest live pill #%s is %.1f tiles away",
                             tostring(e._pad_pill), e._pad_dist or 0),
          e._pad_removed or 0, e._dv or 0,
          C.PILL_FIRE_RANGE or 8, C.REFUEL_PAD_TANK_OFFSET or 0.7071, _pad_thresh,
          C.PILL_RANGE_MAP or 9)
      else
        _d_danger = _d_danger .. string.format(
          " -- PAD HIT: pill #%s is %.1f tiles from the base centre, inside "
          .. "%.0f[PILL_FIRE_RANGE] + %.4f[REFUEL_PAD_TANK_OFFSET] = %.2f, so it "
          .. "can shell part of this tile and the stamped value stands in full "
          .. "(no fractional scaling -- we do not pick where on the tile we stop).",
          tostring(e._pad_pill), e._pad_dist or 0,
          C.PILL_FIRE_RANGE or 8, C.REFUEL_PAD_TANK_OFFSET or 0.7071, _pad_thresh)
      end
    end
    local _d_stale   = fmt_stale_detail(e._age, e._stale)
    -- Contested: sum over MOVING enemy tanks inside the range of
    -- PENALTY x (1 - dist/RANGE), skipping any tank an ally's attack_tank
    -- already owns. Show the head counts so the number reconciles.
    local _ct_n = e._contest_n or 0
    local _ct_h = e._contest_h or 0
    local _d_contest
    if e._contest > 0 then
      _d_contest = string.format(
        "%d moving enemy tank(s) within %.0f[CONTESTED_BASE_RANGE] tiles, "
        .. "sum of %.0f[CONTESTED_BASE_PENALTY] x (1 - dist/%.0f) = %.0f%s",
        _ct_n, C.CONTESTED_BASE_RANGE, C.CONTESTED_BASE_PENALTY,
        C.CONTESTED_BASE_RANGE, e._contest,
        _ct_h > 0 and string.format(" (%d more skipped — ally attack_tank on them)", _ct_h) or "")
    elseif _ct_h > 0 then
      _d_contest = string.format(
        "all %d enemy tank(s) in range handled by an ally's attack_tank → 0", _ct_h)
    else
      _d_contest = string.format("no moving enemy tank within %.0f[CONTESTED_BASE_RANGE] tiles → 0",
            C.CONTESTED_BASE_RANGE)
    end
    local _d_deplete = e._ratio >= 1.0
      and string.format("supply_ratio=%.2f (fully stocked) → 0", e._ratio)
      or  string.format("(1 - %.2f[supply_ratio]) x %.0f[REFUEL_DEPLETION_PENALTY] = %.0f",
            e._ratio, C.REFUEL_DEPLETION_PENALTY, e._dep)

    -- Live cost shape stashed by goal_selection's pool-1 cost competition.
    -- All five fields are populated together; if _urgency is nil the stash
    -- hasn't run yet (first tick / pool-1 cache built but not yet shaped).
    local _has_shape = e._urgency ~= nil
    local _shape_head = ""
    local _shape_detail = ""
    if _has_shape then
      local _u = e._urgency or 1
      local _bf = e._base_floor or 0
      local _db = e._defic_bonus or 0
      local _fm = e._fill_mult or 1
      local _lgm = e._lgm_wait_floor
      local _nl = e._notlow or 1.0
      _shape_head = string.format(
        " × ur{%.2f}%s - def{%.0f} × mult{%.2f fill %.2f sh %d/%d arm %d/%d} scar{%.2f}%s",
        _u, (_nl ~= 1.0) and string.format(" × notlow{%.2f}", _nl) or "",
        _db, _fm, e._fill or 0,
        e._sh or 0, e._sh_target or 0, e._arm or 0, e._arm_target or 0,
        e._scarcity or 1,
        _lgm and string.format(" → lgm_wait_floor{%.0f}", _lgm) or "")
      local _arm     = e._arm or 0
      local _sh      = e._sh or 0
      local _arm_def = e._arm_def or 0
      local _sh_def  = e._sh_def or 0
      local _fill    = e._fill or 0
      local _arm_lin = math.min(1.0, _arm / C.ARMOUR_LOW)
      local _sh_lin  = math.min(1.0, _sh  / C.SHELLS_LOW)
      local _arm_sq  = _arm_lin * _arm_lin
      local _sh_sq   = _sh_lin  * _sh_lin
      local _d_urgency = string.format(
        "armour=%d/%d→%.2f², shells=%d/%d→%.2f² → min=%.2f, clamped(min=%.2f)=%.2f",
        _arm, C.ARMOUR_LOW, _arm_sq,
        _sh,  C.SHELLS_LOW, _sh_sq,
        math.min(_arm_sq, _sh_sq),
        C.REFUEL_URGENCY_MIN, _u)
      local _d_def   = string.format(
        "arm_def=%.2f, sh_def=%.2f → max=%.2f × %.0f[REFUEL_DEFICIT_BONUS] = %.0f",
        _arm_def, _sh_def, math.max(_arm_def, _sh_def), C.REFUEL_DEFICIT_BONUS, _db)
      local _d_fill  = (_fm > 1.0)
        and string.format(
          "fill=%.2f = min((sh %d-%d)/(%d-%d), (arm %d-%d)/(%d-%d)) → "
          .. "1 + %.2f² x (%.2f[FULL_MULT]-1) x %.2f[scarcity] = %.2f",
          _fill,
          _sh, C.SHELLS_LOW, e._sh_target or 0, C.SHELLS_LOW,
          _arm, C.ARMOUR_LOW, e._arm_target or 0, C.ARMOUR_LOW,
          _fill, C.REFUEL_FULL_COST_MULT, e._scarcity or 1, _fm)
        or  "fill=0 (at/below LOW thresholds) → 1.00"
      local _d_scar  = string.format(
          "scarcity=%.2f = 1 + %.2f[RATIO_K] x max(0, team/friendly_bases - 1)"
          .. " + %.2f[LOCAL_K] x allies within %d tiles, capped at %.2f",
          e._scarcity or 1, C.REFUEL_SHARE_RATIO_K or 0, C.REFUEL_SHARE_LOCAL_K or 0,
          C.REFUEL_SHARE_LOCAL_TILES or 20, C.REFUEL_SHARE_SCARCITY_CAP or 8.0)
      local _d_lgm   = _lgm
        and string.format("LGM returning, at this base → floor=%.0f (cost capped)", _lgm)
        or  "no LGM-wait active → no floor"
      -- Not-low markup: applied after urgency (and the REFUEL_MIN_COST floor);
      -- the LGM-wait floor above is not scaled by it.
      local _d_notlow = (_nl ~= 1.0)
        and string.format(
          "|notlow:armour %d > %d[ARMOUR_LOW] and shells %d > %d[SHELLS_LOW]"
          .. " (neither low) → x%.2f[REFUEL_NOT_LOW_MULT]; the LGM-wait floor is not scaled",
          _arm, C.ARMOUR_LOW, _sh, C.SHELLS_LOW, _nl)
        or ""
      _shape_detail = string.format("|urgency:%s%s|deficit:%s|fill:%s|scarcity:%s|lgm:%s",
        _d_urgency, _d_notlow, _d_def, _d_fill, _d_scar, _d_lgm)
    end

    local _danger_pen = C.REFUEL_DANGER_PENALTY or (1 / 0.75)
    local _safe_token = (not e._safe_refuel)
      and string.format(" × danger{%.2f}", _danger_pen) or ""
    local _safe_detail = (not e._safe_refuel)
      and string.format(
        "|danger:danger_val>0 (exposed base) → multiply final cost by %.2f[REFUEL_DANGER_PENALTY]",
        _danger_pen)
      or ""
    -- Mine-hoard chip. Shown whenever there IS a hoard at this base, charged or
    -- waived, so a 0 that was waived can never be mistaken for "no mines".
    -- Same text the REFUEL_SHAPE debug line prints (mine_chip is shared).
    local _mine_token, _mine_detail = "", ""
    if e._mine_d and e._mine_d.over > 0 then
      local _mh, _mw = mine_chip(e._mine_d)
      _mine_token  = string.format(" + mines{%s}", _mh)
      _mine_detail = "|mines:" .. _mw
        .. ". Charged ONLY at the base you're parked on, to push a mine-stuffed"
        .. " tank off the pad; waived while that base can still hand it a"
        .. " resource it is short of (C.REFUEL_MINE_HOARD_NEEDS_SUPPLY)."
    end
    local _hop_token = (e._hop and e._hop > 0)
      and string.format(" + hop{%.0f}", e._hop) or ""
    local _hop_detail = (e._hop and e._hop > 0)
      and string.format(
        "|hop:%.0f[REFUEL_BASE_HOP_PENALTY] — we're parked on ANOTHER base; switching to this one is wasteful churn, so it's penalised. Finish where you are (a depleted current base drops out, freeing the move).",
        e._hop)
      or ""
    -- Low-stock markup chips (refuel_low_stock_mult): one per short resource,
    -- multiplied in right after the danger penalty, before the additive
    -- mine/hop terms -- same order the code applies them.
    local _low_token, _low_detail = "", ""
    if e._low_d and (e._low_d.low_arm or e._low_d.low_sh) then
      _low_token = " ×" .. refuel_low_stock_chip(e._low_d)
      _low_detail = string.format(
        "|low:observed stock below %d[REFUEL_MIN_STOCK] for a resource we need (obs %dt ago, fresh < %d[REFUEL_OBS_STALE]): %s%s— multiply by %.2f",
        C.REFUEL_MIN_STOCK, e._low_d.age or 0, C.REFUEL_OBS_STALE,
        e._low_d.low_arm and string.format("armour obs %d (raw %d / 5, rounded down), we need armour → x%.2f[REFUEL_LOW_ARMOUR_MULT] ",
                                           e._low_d.obs_arm or 0, e._low_d.obs_arm_raw or 0, C.REFUEL_LOW_ARMOUR_MULT or 1) or "",
        e._low_d.low_sh and string.format("shells obs %d, we need shells → x%.2f[REFUEL_LOW_SHELLS_MULT] ",
                                          e._low_d.obs_sh or 0, C.REFUEL_LOW_SHELLS_MULT or 1) or "",
        e._low_mult or 1)
    end
    local _d_astar = string.format(
      "danger-weighted Dijkstra-slate travel cost to base (%d,%d) = %.0f; path %s",
      e._mx or 0, e._my or 0, raw, e._path or "(not traced)")
    local _d_base = string.format(
      "%.0f[REFUEL_BASE_COST] flat floor so refuel-at-own-base isn't ~0",
      C.REFUEL_BASE_COST)
    f = string.format(
      "A*{%.0f}@(%d,%d) + base{%.0f} + danger{%.0f}%s + stale{%.0f} + contest{%.0f} + deplete{%.0f}%s%s%s%s%s"..
      "||A*:%s|base:%s|danger:%s|stale:%s|contest:%s|deplete:%s%s%s%s%s%s",
      raw, e._mx or 0, e._my or 0, C.REFUEL_BASE_COST, e._dang, _pad_token, e._stale, e._contest, e._dep, _shape_head, _safe_token, _low_token, _mine_token, _hop_token,
      _d_astar, _d_base, _d_danger, _d_stale, _d_contest, _d_deplete, _shape_detail, _safe_detail, _low_detail, _mine_detail, _hop_detail)
  elseif p == 6 then
    local _d_hp = string.format(
      "ATTACK_PILL_HP_MULT[%d] = %.2f (hand-tuned table: 5/10/18/28%% for hp 1-4, then linear 40%%→100%% over hp 5-15)",
      e._hpv, e._hp)
    local _d_anger = e._ttc > 0
      and string.format(
        "anger=%.0f > %.0f[ANGER_ATTACK_THRESHOLD]; ticks_to_calm=(%.0f-%.0f)x%.2f[PILL_ANGER_DECAY]=%.2f;"
        .." %.2f[ticks] x %.1f[ANGER_COST_PER_TICK] = %.0f",
        e._pa, C.ANGER_ATTACK_THRESHOLD, e._pa, C.ANGER_ATTACK_THRESHOLD,
        C.PILL_ANGER_DECAY, e._ttc, e._ttc, C.ANGER_COST_PER_TICK, e._anger)
      or string.format("anger=%.0f <= %.0f[ANGER_ATTACK_THRESHOLD] → 0",
        e._pa, C.ANGER_ATTACK_THRESHOLD)
    local _d_stale = fmt_stale_detail(e._age, e._stale)
    -- _wound shows as *wound{0.30} on the wounded pill (discount); on
    -- every OTHER pill it's the finish_other multiplier (>1, penalty),
    -- because step_eval_queue folds finish_other into the same wound_mult
    -- variable. Display them with distinct labels so it's obvious
    -- which kind of adjustment is in play.
    local _fin_mult = e._fin_mult or 1.0
    local _wound_detail
    if e._wound < 1 then
      local _cm = e._commit_mult or 1.0
      if _cm < 0.999 then
        _wound_detail = string.format("*wound{%.2f}*commit{%.2f}", e._wound / _cm, _cm)
      else
        _wound_detail = string.format("*wound{%.2f}", e._wound)
      end
    elseif _fin_mult > 1.001 then
      _wound_detail = string.format("*finish_other{%.2f}", _fin_mult)
    else
      _wound_detail = ""
    end
    local _d_finish_other = _fin_mult > 1.001
      and string.format(
        "wounded pill (id=%d, hp=%d, age=%d ticks) is at or below WOUNDED_FINISH_THRESHOLD=%d;"..
        " mult = 1 + (%.1f-1) × (%d-%d)/%d × max(0, 1 - %d/%d) = %.2f",
        e._fin_wpid or -1,
        e._fin_wphp or 0, e._fin_age or 0,
        C.WOUNDED_FINISH_THRESHOLD or 10,
        C.WOUNDED_FINISH_OTHER_PENALTY or 3.0,
        C.WOUNDED_FINISH_THRESHOLD or 10, e._fin_wphp or 0,
        C.WOUNDED_FINISH_THRESHOLD or 10,
        e._fin_age or 0, C.WOUNDED_FINISH_DECAY_TICKS or 500,
        _fin_mult)
      or  "1.00 (no wounded pill, or this IS the wounded pill, or HP > threshold, or decayed out)"
    local _ammo = e._ammo or 0
    local _sh_now = e._sh_now or 0
    local _sh_end = e._sh_end or 0
    local _ammo_str = (_ammo >= 1e29) and "INF" or string.format("%.0f", _ammo)
    local _d_ammo
    -- The line the cost used (U.attack_shells_low): SHELLS_LOW, or
    -- SIDE_SHELLS_LOW_CAP when that is below it. No new locals here.
    if _ammo >= 1e29 then
      _d_ammo = string.format(
        "shells=%d < pill_hp=%d → cannot finish; cost = INF (filtered)",
        _sh_now, e._hpv)
    elseif _ammo > 0 then
      _d_ammo = string.format(
        "shells=%d, pill_hp=%d, end=%d < %s=%d → (%d-%d) × 5 = %d",
        _sh_now, e._hpv, _sh_end,
        (H.shells_low() < C.SHELLS_LOW) and "SIDE_SHELLS_LOW_CAP" or "SHELLS_LOW",
        H.shells_low(), H.shells_low(), _sh_end, _ammo)
    else
      _d_ammo = string.format(
        "shells=%d - pill_hp=%d = %d ≥ %s=%d → 0",
        _sh_now, e._hpv, _sh_end,
        (H.shells_low() < C.SHELLS_LOW) and "SIDE_SHELLS_LOW_CAP" or "SHELLS_LOW",
        H.shells_low())
    end
    -- "How spot was computed" detail. Method tag (dijkstra/astar/none),
    -- slate index when dijkstra was used, the tick smart_cost ran on,
    -- and the realized path tiles (truncated at 24 by the producer to
    -- fit the 400-char per-segment cap in pool_grid.cpp's parser).
    local _spot_method = e._spot_method or "?"
    local _spot_slate  = e._spot_slate
    local _spot_tick   = e._spot_tick
    local _spot_path   = e._spot_path or "(no path captured)"
    local _spot_pathlen= e._spot_path_len or 0
    local _d_spot
    if _spot_method == "dijkstra" then
      _d_spot = string.format(
        "method=dijkstra slate=%s tick=%s path_len=%d path=%s",
        tostring(_spot_slate), tostring(_spot_tick), _spot_pathlen, _spot_path)
    elseif _spot_method == "astar" then
      _d_spot = string.format(
        "method=astar (dijkstra slate hadn't reached spot) tick=%s path_len=%d path=%s",
        tostring(_spot_tick), _spot_pathlen, _spot_path)
    else
      _d_spot = string.format(
        "method=%s tick=%s — no path produced",
        tostring(_spot_method), tostring(_spot_tick))
    end
    local _tw = e._travel_wound or 1.0
    local _d_pickup = e._pickup_detail or "(no path captured)"
    local _danger_term = e._danger_mult
      and string.format(" * danger_nearby{%.2fx %dt}", e._danger_mult, e._danger_left or 0)
      or ""
    local _d_danger = e._danger_mult
      and string.format("recent abort due to enemy LGM near pill — %.2fx multiplier, %dt (~%.1fs) remaining",
            e._danger_mult, e._danger_left or 0, (e._danger_left or 0) / 50.0)
      or "no recent LGM-near-pill abort on this target"
    local _atk_tank_term = e._atk_tank_pen and string.format(" + atk_tank{%d}", e._atk_tank_pen) or ""
    local _d_atk_tank = e._atk_tank_pen
      and string.format("a non-rejected enemy tank is present this eval → flat +%d on every attack_pill (prefer fighting the tank over chipping pills)", e._atk_tank_pen)
      or "no live enemy tank → no cross-penalty"
    -- Spike shaping terms: multiplier on the combat block when THIS pill is
    -- spiking a friendly base, or the flat cross-penalty when a spike exists
    -- elsewhere. Detail row is always present (matches the intcpt pattern —
    -- an absent row read as "not part of the cost").
    local _spike_mult_term = e._spike_mult and string.format(" * spike{%.2f}", e._spike_mult) or ""
    local _spike_pen_term  = e._spike_pen and string.format(" * spike_pen{%.3f}", e._spike_pen)
                             or (e._spike_pen_skip and string.format(" * spike_pen{skip: %d shots}", e._spike_pen_skip))
                             or ""
    local _d_spike = e._spike_mult
      and string.format(
        "SPIKING: this pill sits within PILL_FIRE_RANGE=%d of %d friendly base(s) (first @(%d,%d)); its least-contested base is covered by %d spike(s) → decisiveness 1/%d → combat × %.2f (SPIKE_PILL_DISCOUNT=%.2f at full decisiveness; each EXTRA denied base strengthens the pull by SPIKE_BASES_BONUS, floored at SPIKE_DISCOUNT_FLOOR; a co-spiked area is 'kinda lost' so clearing one of many pulls weakly)",
        C.PILL_FIRE_RANGE, e._spike_n or 1, e._spike_bmx or -1, e._spike_bmy or -1,
        e._spike_cover or 1, e._spike_cover or 1, e._spike_mult, C.SPIKE_PILL_DISCOUNT or 0.8)
      or "this pill is not in firing range of any friendly base → no spike discount"
    local _d_spike_pen = e._spike_pen
      and string.format(
        "a spiking pill exists elsewhere (@(%d,%d), in range of a friendly base) → WHOLE cost × %.3f [1 + (SPIKE_OTHER_PENALTY_MULT-1) × best decisiveness] on every non-spiking pill (clear the spike first; fades toward ×1.0 when every spike shares its base with others)",
        e._spike_ex_mx or -1, e._spike_ex_my or -1, e._spike_pen)
      or (e._spike_pen_skip and string.format(
        "a spiking pill exists elsewhere (@(%d,%d)), but this pill dies in %d shot(s) (known armour / rules.pill_shell_damage, rounded up) <= SPIKE_PEN_SKIP_MAX_SHOTS=%d → x1.0, no cross-penalty",
        e._spike_ex_mx or -1, e._spike_ex_my or -1, e._spike_pen_skip, C.SPIKE_PEN_SKIP_MAX_SHOTS or 0))
      or "no spiking pill elsewhere (or this IS the spike) → no cross-penalty"
    -- INF terms print as "INF" (a %.0f of 1e30 is a 31-digit number and
    -- math.huge prints "inf"); the SKIP prefix + skip: row name which
    -- term(s) went INF and why, so an INF row is readable at a glance.
    local function _fmt_inf(v) return (v or 0) >= 1e29 and "INF" or string.format("%.0f", v or 0) end
    local _skip_prefix, _d_skip = "", "cost is finite; no INF term"
    if e._skipped then
      _skip_prefix = "SKIP " .. e._skipped .. " !! "
      local _skip_parts = {}
      for r in string.gmatch(e._skipped, "[^+]+") do
        _skip_parts[#_skip_parts + 1] = ({
          low_shells     = string.format("shells=%d < pill_hp=%d → can't finish the pill, ammo=INF", _sh_now, e._hpv or 0),
          no_spot        = string.format("no firing tile with LOS on the pill within ATTACK_PILL_STANDOFF=%d (or every angle banned) → diff=INF", C.ATTACK_PILL_STANDOFF or 0),
          unreachable    = "no spot AND the Dijkstra slate never reached the pill's cheapest adjacent tile (island / needs a boat / slate still building) → pickup=INF",
          no_pickup_path = string.format("spot (%d,%d) found but the spot→pill A* returned COST_INF (no path, or the 4096-node budget ran out) → pickup=INF", e._spot_mx or 0, e._spot_my or 0),
        })[r] or (r .. ": INF from a term not named above")
      end
      _d_skip = table.concat(_skip_parts, "; ")
    end
    f = string.format(
      "%s(spot{%.0f}@(%d,%d) + pickup{%s}@(%d,%d)→(%d,%d)*wound_x2{%.2f} + (stale{%.0f} + diff{%s} + anger{%.0f} + xfire{%.0f} + intcpt{%.0f}) * hp{%.2f}%s%s + ammo{%s}%s)%s%s"..
      "||spot cost is offset-aware (target pill's danger contribution subtracted via load_danger_offset before A*); NOT scaled by hp or wound"..
      "|skip:%s|pickup:%s|hp:%s|anger:%s|stale:%s|finish_other:%s|ammo:%s|spot:%s|danger_nearby:%s|atk_tank:%s|spike:%s|spike_pen:%s",
      _skip_prefix,
      e._spot, e._spot_mx or 0, e._spot_my or 0,
      _fmt_inf(e._travel),
      e._spot_mx or 0, e._spot_my or 0, e._mx or 0, e._my or 0,
      _tw,
      e._stale, _fmt_inf(e._diff), e._anger, e._xfire, e._intcpt,
      e._hp, _wound_detail, _spike_mult_term, _ammo_str, _atk_tank_term, _spike_pen_term, _danger_term,
      -- (order: atk_tank inside the parens; spike_pen + danger_nearby are
      -- whole-cost multipliers, displayed trailing outside the parens)
      _d_skip, _d_pickup, _d_hp, _d_anger, _d_stale, _d_finish_other, _d_ammo, _d_spot, _d_danger, _d_atk_tank, _d_spike, _d_spike_pen)
  elseif p == 7 then
    local _lgm_mult_b = e._lgm_mult or 1
    local _fcov_detail = e._fcov_m and string.format(
      " x %.2f[%d friendly pill(s) cover the base tile or our approach:"
      .. " ATTACK_BASE_FRIENDLY_COVER_MULT %.2f^%d, floor %.2f]",
      e._fcov_m, e._fcov_n or 0, C.ATTACK_BASE_FRIENDLY_COVER_MULT or 0.8,
      e._fcov_n or 0, C.ATTACK_BASE_FRIENDLY_COVER_FLOOR or 0.5) or ""
    local _d_threat
    if _lgm_mult_b ~= 1 then
      _d_threat = string.format(
        "%.2f[threat_val] x %.1f[ATTACK_BASE_THREAT_WEIGHT] x %d (cautious mode)%s = %.0f",
        e._tv, C.ATTACK_BASE_THREAT_WEIGHT, _lgm_mult_b, _fcov_detail, e._thr)
    else
      _d_threat = string.format(
        "%.2f[threat_val] x %.1f[ATTACK_BASE_THREAT_WEIGHT]%s = %.0f",
        e._tv, C.ATTACK_BASE_THREAT_WEIGHT, _fcov_detail, e._thr)
    end
    local _d_stale = fmt_stale_detail(e._age, e._stale)
    -- ARMOUR-AWARE MARKUP.  base{} is no longer the flat ATTACK_BASE_EXTRA_COST:
    -- it is that cost scaled by the WORK LEFT on this base, shells-still-needed
    -- over shells-from-full (17).  `hp n/17` is exactly that ratio, so the chip
    -- multiplies out to the printed number by hand.
    local _b_n     = e._b_n            -- shells still needed (nil = never observed)
    local _b_nfull = e._b_nfull or C.BASE_FULL_SHELLS_TO_KILL
    local _b_frac  = e._b_frac or 1.0  -- the fraction charged, AFTER stale decay
    local _b_arm   = e._b_arm          -- observed engine armour (nil = never seen)
    local _b_age   = e._b_age or 0
    local _b_decay = e._b_decay or 1.0
    -- Friendly-pill cover: multiplies BOTH the markup and the threat term.
    -- The chip has to appear on both or neither number multiplies out.
    local _fc_m, _fc_n = e._fcov_m, e._fcov_n
    local _fc_chip = _fc_m
      and string.format(" x fcover{%.2f n=%d}", _fc_m, _fc_n or 0) or ""
    local _b_chip
    if _b_n then
      _b_chip = string.format("%d x hp %d/%d%s = %.0f",
        C.ATTACK_BASE_EXTRA_COST, _b_n, _b_nfull, _fc_chip, e._base or 0)
    else
      _b_chip = string.format("%d x hp ?/%d%s = %.0f (unseen)",
        C.ATTACK_BASE_EXTRA_COST, _b_nfull, _fc_chip, e._base or 0)
    end
    local _d_base
    if _b_n then
      local _fresh = C.ATTACK_BASE_EXTRA_COST * (_b_n / _b_nfull)
      _d_base = string.format(
        "%.0f[ATTACK_BASE_EXTRA_COST] x %.3f[work left]" .. _fc_chip .. " = %.1f. armour=%d, capturable at <= %d,"
        .. " %d[DAMAGE] per shell -> ceil((%d - %d)/%d) = %d shell(s) left of the %d a FULL base"
        .. " (%d armour) needs. Reading is %d tick(s) old: decay = min(1, %d/%d[BASE_MARKUP_STALE]) = %.2f,"
        .. " so the charged fraction is %d/%d + (1 - %d/%d) x %.2f = %.3f (fresh would have been %.1f)",
        C.ATTACK_BASE_EXTRA_COST, _b_frac, e._base or 0,
        _b_arm or 0, C.BASE_CAPTURE_ARMOUR, C.BASE_SHELL_DAMAGE,
        _b_arm or 0, C.BASE_CAPTURE_ARMOUR, C.BASE_SHELL_DAMAGE, _b_n, _b_nfull,
        C.BASE_FULL_ARMOUR, _b_age, _b_age, C.BASE_MARKUP_STALE, _b_decay,
        _b_n, _b_nfull, _b_n, _b_nfull, _b_decay, _b_frac, _fresh)
      _d_base = _d_base .. _fcov_detail
    else
      _d_base = string.format(
        "%.0f[ATTACK_BASE_EXTRA_COST] x 1.000%s = %.1f. This base has NEVER reported its stock"
        .. " (EVENT_BASE_STOCK fires only when a base's armour/shells/mines change), so there is no"
        .. " armour reading to discount and it pays the full flat markup",
        C.ATTACK_BASE_EXTRA_COST,
        e._fcov_m and string.format(" x %.2f", e._fcov_m) or "",
        e._base or 0)
      _d_base = _d_base .. _fcov_detail
    end
    -- STEAL chip: whether the imminent-base-steal floor took this row, or which
    -- gate it failed.  See refresh_base_steal.
    local _st_chip, _d_steal = "", ""
    if e._steal then
      _st_chip = string.format(" STEAL{%.0f}", C.BASE_STEAL_COST)
      _d_steal = string.format(
        "|steal:YES -- fresh armour %d (<= %d[BASE_STEAL_MAX_ARMOUR]) is %d shell(s) from capturable,"
        .. " base is %d tile(s) away (<= %d[BASE_STEAL_RANGE]), nothing covers the base tile or the"
        .. " approach line, and we have the ammo. Cost is SNAPPED to %.0f[BASE_STEAL_COST] so one shell"
        .. " then the armour-0 IMMINENT capture wins the pool outright",
        _b_arm or 0, C.BASE_STEAL_MAX_ARMOUR, _b_n or 0,
        e._steal_dist or 0, C.BASE_STEAL_RANGE, C.BASE_STEAL_COST)
    elseif e._steal_rej then
      _st_chip = string.format(" steal{no:%s}", e._steal_rej)
      local why = ({
        stale_obs      = string.format("no armour reading, or one older than %d[BASE_STEAL_OBS_STALE] ticks (age=%s)",
                           C.BASE_STEAL_OBS_STALE, (e._b_age and e._b_age < 1e17) and tostring(e._b_age) or "never"),
        not_capturable = string.format("armour %s > %d[BASE_STEAL_MAX_ARMOUR] -- %s shells from capturable, that is a normal attack_base errand",
                           tostring(_b_arm), C.BASE_STEAL_MAX_ARMOUR, tostring(_b_n)),
        out_of_range   = string.format("%s tiles away > %d[BASE_STEAL_RANGE]",
                           tostring(e._steal_dist), C.BASE_STEAL_RANGE),
        no_shells      = string.format("shells <= %d[SHELL_RESERVE] + %s needed",
                           C.SHELL_RESERVE or 0, tostring(_b_n)),
        covered        = string.format("live hostile/neutral pill#%s can put a shell on the base tile or on our straight approach to it",
                           tostring(e._steal_pill)),
        inboat         = "we are afloat -- a boat has no business taking a base",
        tank_armour    = string.format("tank armour %s < %d[BASE_STEAL_MIN_ARMOUR]",
                           tostring(e._steal_arm), C.BASE_STEAL_MIN_ARMOUR),
      })[e._steal_rej] or e._steal_rej
      _d_steal = "|steal:no -- " .. why
    end
    -- Turtle far-base multiplier: the whole sum is multiplied, BEFORE the
    -- steal snap. "(" .. sum .. ") x turtle_far{..}" only when it applies.
    local _tf_chip, _tf_det = M.turtle_far_text(e._tf_d, e._tf_m or 1, e._tf_s)
    local _tf_open = (_tf_chip ~= "") and "(" or ""
    local _tf_close = (_tf_chip ~= "") and (")" .. _tf_chip) or ""
    if e._steal then
      -- The snap floor is a min(), not another term: print it as one so the
      -- displayed chain still resolves to the cost the pool competed.
      f = string.format(
        "min(%sA*{%.0f}@(%d,%d) + base{%s} + threat{%.0f} + stale{%.0f}%s,%s)||base:%s|threat:%s|stale:%s%s%s",
        _tf_open, raw, e._mx or 0, e._my or 0, _b_chip, e._thr, e._stale, _tf_close, _st_chip,
        _d_base, _d_threat, _d_stale, _d_steal, _tf_det)
    else
      f = string.format(
        "%sA*{%.0f}@(%d,%d) + base{%s} + threat{%.0f} + stale{%.0f}%s%s||base:%s|threat:%s|stale:%s%s%s",
        _tf_open, raw, e._mx or 0, e._my or 0, _b_chip, e._thr, e._stale, _tf_close, _st_chip,
        _d_base, _d_threat, _d_stale, _d_steal, _tf_det)
    end
  elseif p == 4 then
    -- Rejected dead-pill rows: short-circuit with a "REJECT: <reason>"
    -- formula so the breakdown panel makes clear why the row exists
    -- with INF cost. Cost compute was skipped at queue-build time.
    if e._reject then
      local rem = e._reject_remaining or 0
      local rem_tok = (e._reject == "blocked" or e._reject == "stale")
                      and string.format(" %dt", rem) or ""
      local desc = ({
        in_tank = "pill is in flight (picked up by a tank)",
        blocked = "tile on retry cooldown (recent stuck-escape or pathfinder failed to reach it)",
        stale   = "not seen recently — fog of war",
        -- SEA-PILL HARVEST rejects (goals.lua sea_plan_cluster). Every one of
        -- these means "the pill is in deep sea and the mine->river->boat
        -- entrance is not on", named so the panel says which gate failed.
        deepsea_no_boat = "pill lies in DEEP SEA and no harvest plan exists for it (sea harvest disabled, or the cluster scan has not run yet)",
        lgm_dead = "pill lies in DEEP SEA: the harvest needs the LGM to lay the mine and build the boat, and he is dead",
        low_shells = string.format("pill lies in DEEP SEA: fewer than %d shells left, and detonating the mine needs a reserve", C.SEA_PILL_MIN_SHELLS or 3),
        enemy_tank_near = string.format("pill lies in DEEP SEA: a visible enemy tank is within %d tiles of the cluster — a boat is one shell from dead", C.SEA_PILL_ENEMY_TANK_NEAR or 10),
        path_enemy_tank = string.format("pill lies in DEEP SEA: a visible enemy tank is within %d tiles of the boat path", C.SEA_PILL_ENEMY_TANK_NEAR or 10),
        pills_landlocked = "pill lies in a one-tile puddle, not in navigable water — a boat could not sail from any entrance to it, so there is nothing to harvest (the tile is still lethal to walk onto, which is why perception flags it)",
        no_entrance = string.format("pill lies in DEEP SEA and no land tile with a CARDINAL deep-sea neighbour within %d tiles passed the gates (pill heat / line of fire / tank or LGM reach / no firing spot)", C.SEA_PILL_ENTRANCE_MAX_DIST or 12),
        entrance_unreachable = "pill lies in DEEP SEA: the chosen entrance has no tank route (Dijkstra and A* both INF)",
        no_shells_anywhere = "pill lies in DEEP SEA: the entrance needs a mine detonated and we have neither the shells nor a reachable friendly base to fetch them from",
        no_mines_anywhere = "pill lies in DEEP SEA, we carry no mine, and there is no reachable friendly/allied base at all to fetch one from — nothing can crater the shore. (A base that merely READ empty is still offered: stock is only reported up close and bases restock.)",
        no_safe_trees = string.format("pill lies in DEEP SEA: short of the %d trees the mine+boat need (LGM_COST_BOAT 20 + LGM_COST_MINE 1) and there are not enough harvestable forest tiles within %d tiles of the entrance (%d trees per tile) that are NOT COVERED by a live hostile/neutral pillbox. Coverage, not influence: whose neighbourhood it is by the numbers does not kill an LGM, a pillbox with a line on the tile does", C.SEA_PILL_TREES_TOTAL or 21, C.SEA_TREES_RADIUS or 12, C.SEA_TREES_PER_FOREST or 4),
      })[e._reject]
      -- The three reject reasons that name a pill id.
      if not desc and type(e._reject) == "string" then
        local pn = e._reject:match("^covered_by_pill#(%d+)$")
        if pn then
          desc = string.format("this pill of the raft sits inside hostile pill #%s's firing circle (PILLBOX_RANGE, euclidean, inclusive) with a clear shell line, or every boat route to it does — one hit sinks the boat, so it is dropped from the trip. The rest of the cluster still goes.", pn)
        end
        if not desc then
          pn = e._reject:match("^cluster_member_of#(%d+)$")
        end
        if pn then
          desc = string.format("pill lies in DEEP SEA and dead pill #%s of the SAME cluster is carrying the harvest plan — one boat trip takes them all, so only the lowest-id member competes for the goal (same plan, same cost, shown here for visibility)", pn)
        end
        if not desc then
          pn = e._reject:match("^pills_covered_by_pill#(%d+)$")
          if pn then
            desc = string.format("pill lies in DEEP SEA and hostile pill #%s has a CLEAR shell line onto it — one hit sinks the boat, so the whole cluster is off (calm pills count: they wake when we sail into range)", pn)
          end
        end
        if not desc then
          pn = e._reject:match("^path_covered_by_pill#(%d+)$")
          if pn then
            desc = string.format("pill lies in DEEP SEA and hostile pill #%s has a CLEAR shell line onto a tile of the boat path", pn)
          end
        end
      end
      desc = desc or "pill exists on the map but cannot be picked this tick"
      f = string.format(
        "REJECT %s%s @(%d,%d)||reject:%s%s — %s",
        e._reject, rem_tok, e._mx or 0, e._my or 0,
        e._reject, rem_tok, desc)
    elseif e._sea then
      -- SEA-PILL HARVEST row. The land distance formula does not apply: the
      -- tank cannot walk to a pill in deep sea at all. What it pays is the
      -- drive to the entrance S, the resource legs, and the boat crossing —
      -- all shared across the cluster, all at SEA_PILL_COST_MULT because the
      -- pills themselves are free. Every factor below appears in the term
      -- list, so the final number is hand-computable from this row alone.
      local sc = e._sea
      local mult  = C.SEA_PILL_COST_MULT or 0.3
      local floor = C.SEA_PILL_COST_FLOOR or 5
      local gross = sc.gross or 0
      local scaled = gross * mult / math.max(1, sc.n or 1)
      f = string.format(
        "SEA c%d (travel{%.0f} + refuel_leg{%.0f} + tree_leg{%.0f} + boat_path{%.0f}) x %.2f[SEA_PILL_COST_MULT] / n{%d} = %.1f%s"
        .. "||sea:cluster %d, %d dead pill(s) in deep sea at %s — entrance{%s} S=(%d,%d)%s water_component{%d tiles}, park=%s, mine=%s, boat=%s (%s)"
        .. "|travel:danger-weighted route tank -> S(%d,%d) = %.0f"
        .. "|refuel_leg:%s"
        .. "|tree_leg:trees{%d}/%d[SEA_PILL_TREES_TOTAL = LGM_COST_BOAT 20 + LGM_COST_MINE 1] short{%d} need_tiles{%d} (LGM_GATHER_TREE %d trees per forest tile) forest_ok{%d} (uncovered, within %dt) -> %d x %.0f[SEA_PILL_TREE_LEG_PER_TREE] = %.0f"
        .. "|mines:mines{%d}/%d needed for the crater"
        .. "|shells:shells{%d}/%d — one to land on the mine, plus one for every FOREST tile in the F->S lane (a shell dies on the first forest and only turns it to grass)"
        .. "|coverage:a pill covers a tile only if the tile centre is within PILLBOX_RANGE %d wu of it (euclidean, inclusive — util.c utilIsItemInRange) AND the shell reaches it. CALM pills use r=%.1f tiles, HEATED ones r=%.1f (they reload fast, so the boat keeps a tile of buffer). Covered water in this component: %d tiles. Dropped members: %s"
        .. "|boat_path:boat-layer A* from the water tile beside S to the farthest pill = %.0f"
        .. "|n:cost is split across the %d pill(s) one boat trip collects"
        .. "|floor:%.0f[SEA_PILL_COST_FLOOR] — a real fight beside us must still win"
        .. "|substate:%s",
        sc.id or 0, sc.travel or 0, sc.leg_mines or 0, sc.leg_trees or 0,
        sc.boat_cost or 0, mult, sc.n or 1, sc.cost or 0,
        (scaled < floor) and string.format(" -> FLOOR %.0f", floor) or "",
        sc.id or 0, sc.n or 1,
        table.concat((function()
          local t = {}
          for i, tt in ipairs(sc.tiles or {}) do t[i] = string.format("(%d,%d)", tt[1], tt[2]) end
          return t
        end)(), " "),
        tostring(sc.entrance),
        sc.S and sc.S.mx or 0, sc.S and sc.S.my or 0,
        sc.S and string.format(" pill_at=%.0f", sc.S.hot or 0) or "",
        sc.comp_n or 0,
        sc.F and string.format("(%d,%d)", sc.F.mx, sc.F.my) or "-",
        tostring(sc.needs_mine), tostring(sc.needs_boat),
        tostring(sc.entrance_why),
        sc.S and sc.S.mx or 0, sc.S and sc.S.my or 0, sc.travel or 0,
        sc.refuel_base
          and string.format("mines=0, detour via base #%s%s = %.0f", tostring(sc.refuel_base),
                            sc.refuel_guess and " (stock not confirmed — nearest reachable friendly base)" or "",
                            sc.leg_mines or 0)
          or "0 (a mine is already aboard, or the entrance needs none)",
        (e._sea_trees_have or 0), sc.trees_need or 0, sc.trees_short or 0,
        sc.need_tiles or 0, C.SEA_TREES_PER_FOREST or 4, sc.forest_ok or 0,
        C.SEA_TREES_RADIUS or 12,
        sc.trees_short or 0, C.SEA_PILL_TREE_LEG_PER_TREE or 6, sc.leg_trees or 0,
        (e._sea_mines_have or 0), sc.mines_need or 0,
        (e._sea_shells_have or 0), sc.shells_need or 0,
        C.PILLBOX_RANGE_WU or 2048,
        ((C.PILLBOX_RANGE_WU or 2048) + (C.SEA_COVER_MARGIN_CALM_WU or 0)) / 256,
        ((C.PILLBOX_RANGE_WU or 2048) + (C.SEA_COVER_MARGIN_HOT_WU or 256)) / 256,
        sc.covered_n or 0,
        (function()
          local d = {}
          for pid, by in pairs(sc.dropped or {}) do
            d[#d + 1] = string.format("#%d covered by #%s", pid, tostring(by))
          end
          table.sort(d)
          return (#d > 0) and table.concat(d, ", ") or "none"
        end)(),
        sc.boat_cost or 0, sc.n or 1, floor,
        tostring(e._sea_sub or "-"))
    else
      local _lgm_mult_c = e._lgm_mult or 1
      local _cpill_danger_score = e._dv * C.CAPTURE_PILL_DANGER_SCALE * _lgm_mult_c
      local _intcpt = e._intcpt or 0
      -- Always-on intcpt term (matches attack_pill's formula display
      -- pattern at line 2795).  Empty when 0 was confusing — looked
      -- like the cost didn't include it at all, when really it just
      -- evaluated to zero.
      local intcpt_det = _intcpt > 0
        and string.format("|intcpt:%.0f (enemy can beat us to pill)", _intcpt)
        or  "|intcpt:0 (no enemy tank within INTERCEPT_MAX_RANGE that can beat us)"
      -- LOADED, BUILDER-LESS: which rule produced the danger multiplier.
      -- "no_builder" means CAPTURE_NO_LGM_DANGER_MULT replaced the ordinary
      -- cautious-mode CAUTIOUS_MODE_MULT for the duration of the state.
      local _mult_src = e._mult_src or (_lgm_mult_c ~= 1 and "cautious" or "none")
      local _lgm_mult_str = (_lgm_mult_c ~= 1)
        and string.format(" × %d (%s)", _lgm_mult_c,
              (_mult_src == "no_builder") and "no_builder{loaded, builder-less}"
              or "cautious mode") or ""
      local _lgm_mult_det = (_mult_src == "no_builder")
        and string.format(
          "|no_builder:LOADED, BUILDER-LESS — the danger multiplier is CAPTURE_NO_LGM_DANGER_MULT{%d}, NOT the cautious-mode CAUTIOUS_MODE_MULT{%d} it replaces. A corpse is only worth the trip if we live to place it, and with no builder we cannot place anything at all",
          C.CAPTURE_NO_LGM_DANGER_MULT or 0, C.CAUTIOUS_MODE_MULT or 0)
        or ""
      local _dm_str = e._dist_method or "dij"
      local _fd = e._free or 0
      local _fm = e._free_mult or 1
      local _free_mult = (_fd > 0.001) and (" − %.1f[FREE]"):format(_fd) or ""
      local _free_det
      if _fm == 0 then
        -- The bonus was computed and then scaled to nothing: say so, or the
        -- row looks like it merely failed the distance/danger ramp.
        local _fd_raw = C.CAPTURE_FREE_PILL_VALUE
                        * (1 - math.min(1, (e._dv or 0) / C.CAPTURE_FREE_PILL_DANGER_FALLOFF))
        _free_det = string.format(
          "|free:0 — freebonus{x%.0f} — LOADED, BUILDER-LESS. The ramp would have paid up to −%.1f, and CAPTURE_NO_LGM_FREE_BONUS_MULT{%d} scales the whole thing to zero: a pill we cannot place is not free, it costs a drive we may not survive",
          _fm, _fd_raw, _fm)
      elseif _fd > 0.001 then
        _free_det = string.format("|free:close/safe grab (≤ %.1f tiles, danger %.1f) → −%.1f (0=none .. %.1f=best, floor %.1f)%s",
              C.TANK_COMBAT_ENGAGE_RANGE * C.CAPTURE_FREE_PILL_RANGE_MULT, e._dv or 0, _fd, C.CAPTURE_FREE_PILL_VALUE, C.CAPTURE_FREE_PILL_MIN_COST,
              (_fm ~= 1) and string.format(" × freebonus{%.2f}", _fm) or "")
      else
        _free_det = "|free:none (too far or too dangerous)"
      end
      if _fm == 0 then _free_mult = " freebonus{x0}" end
      -- DIRECT-ROUTE probe verdict (capture_route_probe). Only the focus pill
      -- carries one; every other row is still a plain slate/A* distance. The
      -- "too far" case gets its own line so it can't be misread as "the direct
      -- route was tried and lost".
      local _route_det = ""
      if _dm_str == "astar_direct" then
        _route_det = string.format(
          "|route:DIRECT A* — danger_scale=0, the straight run at the body. The wsim drove it and we LIVE (%d dmg), so this route and its cost are what the pool ranks on",
          e._route_dmg or 0)
      elseif _dm_str == "astar_danger" then
        local _strict_now = (_mult_src == "no_builder")
                            and C.CAPTURE_NO_LGM_ROUTE_STRICT
        _route_det = string.format(
          "|route:DANGER A* — the direct (danger_scale=0) run %s in the wsim (%d dmg), so we re-priced on the danger-weighted A* route instead%s",
          _strict_now and "took DAMAGE" or "KILLED us", e._route_dmg or 0,
          _strict_now and " |routestrict:LOADED, BUILDER-LESS — CAPTURE_NO_LGM_ROUTE_STRICT moves the bar from \"the sim says we survive it\" to \"the sim says nothing touches us\": ANY predicted damage refuses the direct run" or "")
      elseif e._route_far then
        _route_det = string.format(
          "|route:NOT PROBED — pill is %d tiles away, past CAPTURE_ROUTE_MAX_TILES (%d). No direct-route A* and no wsim were run for it; the dist above is the plain Dijkstra/A* slate cost",
          e._route_far, C.CAPTURE_ROUTE_MAX_TILES or 10)
      end
      -- Land dead-pill cluster (Parts 2/3). Both chips print only when the
      -- pill is actually in a cluster of 2+, and each shows the factor it
      -- applied, so the printed chain still multiplies out to the total:
      --     (...)[−FREE] / div  [floor 5]  × guard
      local _cl_n  = e._cl_n or 1
      local _cl_dv = e._cl_div or 1
      local _cl_gn = e._cl_guard_n or 0
      local _cl_gm = e._cl_guard_m or 1.0
      local _cluster_mult, _cluster_det = "", ""
      if _cl_n > 1 and e._cl_off then
        -- LOADED, BUILDER-LESS: the ÷n discount is OFF. The guard SURCHARGE
        -- (below) still applies; only the discount is suppressed.
        _cluster_mult = string.format(" nocluster{%d pills, /1}", _cl_n)
        _cluster_det = string.format(
          "|nocluster:%d dead pills within %d tiles of each other, and the ÷%d cluster discount is SUPPRESSED — CAPTURE_NO_LGM_CLUSTER_DISCOUNT is false while the tank is loaded and builder-less. \"Three corpses in one trip\" is only a bargain when the trip is the cheap part; here the trip is the thing that kills us",
          _cl_n, C.CAPTURE_CLUSTER_RADIUS or 3,
          math.min(_cl_n, C.CAPTURE_CLUSTER_DIVISOR_MAX or 6))
        if _cl_gn > 0 then
          _cluster_mult = _cluster_mult ..
            string.format(" guard{%d pills, x%.2f}", _cl_gn, _cl_gm)
          _cluster_det = _cluster_det .. string.format(
            "|guard:%d live hostile/neutral pill(s) can put a shell on a cluster tile → x%.2f. The guard is a SURCHARGE and is never suppressed",
            _cl_gn, _cl_gm)
        end
      elseif _cl_n > 1 then
        _cluster_mult = string.format(" cluster{%d} /%dX", _cl_n, _cl_dv)
        _cluster_det = string.format(
          "|cluster:%d dead pills within %d tiles of each other — ONE errand, so each member pays its share:"
          .. " cost / min(%d, %d[DIVISOR_MAX]) = /%d, floored at %.0f",
          _cl_n, C.CAPTURE_CLUSTER_RADIUS or 3, _cl_n,
          C.CAPTURE_CLUSTER_DIVISOR_MAX or 6, _cl_dv,
          C.CAPTURE_CLUSTER_MIN_COST or 5)
        local _gids = e._cl_guard_ids
        if _cl_gn > 0 then
          _cluster_mult = _cluster_mult ..
            string.format(" guard{%d pills, x%.2f}", _cl_gn, _cl_gm)
          _cluster_det = _cluster_det .. string.format(
            "|guard:%d live hostile/neutral pill(s) [%s] can put a shell on a cluster tile"
            .. " (within %.0f wu + heat margin AND a clear line of fire) → x(1 + %.2f × %d) capped at %.1f = x%.2f."
            .. " Route danger is NOT counted here — the Dijkstra/A* dist above already prices it",
            _cl_gn, _gids and table.concat(_gids, ",") or "?",
            C.PILLBOX_RANGE_WU or 2048, C.CAPTURE_CLUSTER_GUARD_MULT or 0.75,
            _cl_gn, C.CAPTURE_CLUSTER_GUARD_MAX or 4.0, _cl_gm)
        else
          _cluster_det = _cluster_det ..
            "|guard:none — no live hostile/neutral pillbox has both the range and a clear line onto any cluster tile, so the discount stands undiluted"
        end
      end
      f = string.format(
        "(base{%d} + dist{%.1f}[%s]@(%d,%d) + danger{%.1f} + intcpt{%.0f})%s%s||dist:%.0f^1.5 × %.3f[DIST_SCALE] = %.1f [%s]|danger:%.1f × %.3f[DANGER_SCALE]%s = %.1f%s%s%s%s%s",
        C.CAPTURE_PILL_BASE_COST, e._ds, _dm_str, e._mx or 0, e._my or 0, _cpill_danger_score, _intcpt, _free_mult, _cluster_mult,
        raw, C.CAPTURE_PILL_DIST_SCALE, e._ds, _dm_str,
        e._dv, C.CAPTURE_PILL_DANGER_SCALE, _lgm_mult_str, _cpill_danger_score, _lgm_mult_det, intcpt_det, _free_det, _route_det, _cluster_det)
    end
  elseif p == 3 then
    -- capture_base: the A* number IS the danger-weighted dijkstra travel cost —
    -- danger is baked into the per-tile path cost, NOT a separate additive term,
    -- so a CLOSE base behind enemy fire can out-cost a FAR safe one (by design:
    -- biases toward safer captures). Plus staleness for a neutral base unseen a
    -- while. Note: BASE_PILL_COVER_PEN is applied only on the finalize path
    -- (eval_capture_base), NOT this rolling cost, so it's not part of this total.
    local _cb_dv = H.threat_at(e._mx or 0, e._my or 0)
    -- LOADED, BUILDER-LESS surcharge: the one danger term pool 3 otherwise
    -- does not have. Absent (and its chip absent) at the keel value 0.
    local _cb_nb_chip, _cb_nb_det = "", ""
    if e._p3_xd then
      _cb_nb_chip, _cb_nb_det = M.capture_base_extra_text(e._p3_extra or 0, e._p3_xd, e._p3_xf or 0)
    end
    if e._p3_mult then
      _cb_nb_chip = _cb_nb_chip .. string.format(" + nobuild_danger{%.1f}", e._p3_dang or 0)
      _cb_nb_det = _cb_nb_det .. string.format(
        "|nobuild_danger:LOADED, BUILDER-LESS — threat.at(base)%.1f × DANGER_SCALE{%.3f} × CAPTURE_BASE_NO_LGM_DANGER_MULT{%d} = %.1f, ADDED. A base is a tile you have to sit on, and sitting on a covered one with an unplaceable stack aboard loses the whole load. At the keel value 0 this term does not exist, which is what pool 3 did before",
        e._p3_dv or 0, C.CAPTURE_PILL_DANGER_SCALE, e._p3_mult, e._p3_dang or 0)
    end
    -- Turtle far-base multiplier on the whole sum (absent off-turtle).
    local _tf_chip, _tf_det = M.turtle_far_text(e._tf_d, e._tf_m or 1, e._tf_s)
    f = string.format(
      "%sA*{%.0f}@(%d,%d) + stale{%.0f}%s%s||A*:danger-weighted dijkstra travel to base; danger at base tile=%.1f is BAKED INTO the path cost (not a separate term) — that's why a near dangerous base can cost more than a far safe one|stale:%s%s%s",
      (_tf_chip ~= "") and "(" or "", raw, e._mx or 0, e._my or 0, e._stale or 0, _cb_nb_chip,
      (_tf_chip ~= "") and (")" .. _tf_chip) or "", _cb_dv,
      fmt_stale_detail(e._age, e._stale), _cb_nb_det, _tf_det)
  else
    f = string.format("A*{%.0f}@(%d,%d) + stale{%.0f}||stale:%s",
      raw, e._mx or 0, e._my or 0, e._stale, fmt_stale_detail(e._age, e._stale))
  end
  e.formula = f
  return f
end

-- Combine a REJECT line with the candidate's normal score breakdown so the
-- detail panel shows the term math for rejected rows too (handy for debugging
-- "why was this rejected and what would it have cost"). Only when the cost was
-- actually computed (rejects that skip costing keep the bare reject line). The
-- recursion runs get_formula_inner with _reject cleared to get the real terms,
-- then we overwrite the cache with the combined string. pcall-guarded.
function reject_with_breakdown(e, reject_line, reject_desc)
  local combined = reject_line .. "||" .. reject_desc
  if e.cost and e.cost < 1e29 then
    local saved = e._reject
    e._reject  = nil
    e.formula  = nil
    local ok, normal = pcall(get_formula_inner, e)
    e._reject = saved
    if ok and type(normal) == "string" and normal ~= "" then
      local disp, comp = normal:match("^(.-)||(.*)$")
      if disp and comp then
        local _, full = disp:match("^(.-)!!(.*)$")
        combined = reject_line .. " !! " .. (full or disp) .. "||" .. comp .. " | " .. reject_desc
      else
        combined = reject_line .. " !! " .. normal .. "||" .. reject_desc
      end
    end
  end
  e.formula = combined   -- overwrite the recursion's cache with the combined string
  return combined
end

local function get_formula(e)
  local f = e.formula
  if f == nil then
    if not e._p or not e.raw then return "" end
    local ok, ff = pcall(get_formula_inner, e)
    if ok and ff then
      f = ff
    else
      e.formula = ""
      return ""
    end
  end
  -- Blitz join discount term. Appended before the ally branches below so it
  -- survives their early-returns. Shows "x blitz_discount{factor}" and the
  -- base→result in the map half (REF base when the entry was reject-sentinel'd).
  local bd = e._blitz_discount
  if bd then
    local disp = string.format(" x blitz_discount{%.2f}", bd.factor or 1.0)
    local map  = string.format("|blitz_discount:join C%s — %s%.0f x %.2f = %.0f",
      tostring(bd.cmdr), bd.sentinel and "REF " or "", bd.base or 0, bd.factor or 1.0, bd.now or 0)
    local sep = f:find("||", 1, true)
    if sep then f = f:sub(1, sep - 1) .. disp .. " " .. f:sub(sep) .. map
    else        f = f .. disp .. " ||" .. map:sub(2) end
  elseif e._blitz_off then
    -- Same slot, but this bot has blitz=off (init_arg): an open call exists on
    -- this pill and the join discount was deliberately NOT applied, so the row
    -- is the plain solo cost.
    local disp = " x blitz_discount{1.00 off}"
    local map  = string.format("|blitz_discount:OFF (%s) — an open call on this pill, but this bot never joins; cost unchanged", tostring(e._blitz_off))
    local sep = f:find("||", 1, true)
    if sep then f = f:sub(1, sep - 1) .. disp .. " " .. f:sub(sep) .. map
    else        f = f .. disp .. " ||" .. map:sub(2) end
  end
  -- The ORDERED goal keeps its real cost and its full breakdown; one extra
  -- line says why it is the only STRATEGIC row still bidding, so the panel
  -- stays hand-computable.
  if e._order_held_by then
    local disp = " [ORDER]"
    local map  = string.format("|order: held, from %s", tostring(e._order_held_by))
    local sep = f:find("||", 1, true)
    if sep then f = f:sub(1, sep - 1) .. disp .. " " .. f:sub(sep) .. map
    else        f = f .. disp .. " ||" .. map:sub(2) end
  end
  -- REJECT row already formatted by inner — no trailing term to append.
  if e._reject == "ally_claimed"
     or e._reject == "order"
     or e._reject == "armour_too_low"
     or e._reject == "blitz_only"
     or e._reject == "ally_pill_take_priority" then
    return f
  end
  -- Cost-baked ally penalty (pool 1 soft +100/ally, pool 8 hard +10000).  Only
  -- emit the term when the penalty actually exists on this entry — EXCEPT
  -- refuel (pool 1), which ALWAYS shows ally_claimed (0 when no allies) so the
  -- Term Breakdown column stays stable.
  local pen = e.ally_claimed_pen or 0
  local term_disp, term_map
  if pen > 0 then
    term_disp = string.format(" + ally_claimed{%d}", pen)
    if e._p == 1 then
      -- Refuel (pool 1): soft, per-ally — NOT a yield. Show the count math.
      term_map = string.format(
        "|ally_claimed:%d ally%s targeting this base x %.0f[ALLY_CLAIMED_REFUEL_PENALTY] = %.0f (soft, still selectable; p%s first)",
        e._ally_n or 0, ((e._ally_n or 0) == 1) and "" or "s",
        C.ALLY_CLAIMED_REFUEL_PENALTY or 100, pen,
        tostring(e.ally_claimed_by or "-"))
    else
      term_map = string.format("|ally_claimed:yielding to ally (p%s)",
                               tostring(e.ally_claimed_by or "-"))
    end
  elseif e._p == 1 and not e._reject then
    -- Refuel with NO allies contesting: still emit the term at 0 so it's
    -- always present in the breakdown (becomes +N00 once allies pile on).
    term_disp = " + ally_claimed{0}"
    term_map  = string.format(
      "|ally_claimed:0 allies targeting this base -> 0 (soft +%.0f[ALLY_CLAIMED_REFUEL_PENALTY] per ally when contested)",
      C.ALLY_CLAIMED_REFUEL_PENALTY or 100)
  elseif e._ally_score then
    -- Hard-REJECT pool, ally is bidding, but we kept the candidate
    -- (either we're cheaper or we're within the steal band).  Display
    -- the bid + relationship so the band is visible without polluting
    -- the cost equation.
    local our   = e.cost or 0
    local their = e._ally_score
    -- capture_pill uses the near-zero steal threshold (any cost edge wins
    -- a drive-over grab); other pools the conservative default.
    local frac  = (e._p == 4) and (C.ALLY_CLAIMED_STEAL_FRAC_CAPTURE or 0.01)
                  or (C.ALLY_CLAIMED_STEAL_FRAC or 0.25)
    local rel
    if our < their * (1 - frac) then
      rel = string.format("we cheaper by %.0f%%", (1 - our / math.max(1, their)) * 100)
    elseif their < our * (1 - frac) then
      rel = string.format("they cheaper by %.0f%% (would REJECT)", (1 - their / math.max(1, our)) * 100)
    else
      rel = string.format("within steal band (need %.0f%% cheaper)", frac * 100)
    end
    term_disp = ""  -- no cost contribution
    term_map  = string.format("|ally_claimed:p%s bid %.0f, ours %.0f — %s",
                              tostring(e._ally_by or "?"), their, our, rel)
    if e._steal_units then
      term_map = term_map .. string.format(" (ours priced on our %s cost %.1f)",
                                           e._steal_units, e._steal_cost or our)
    end
  elseif e._steal_released then
    -- The steal-yield block on this row ENDED EARLY: we had yielded the pill,
    -- and the stealer's own advert then showed it going somewhere else, so the
    -- pill is priceable again well inside STEAL_YIELD_BLOCK. Say so on the row —
    -- otherwise a pill that was ally_claimed for 300 ticks a moment ago just
    -- silently reappears as a candidate.
    local sr = e._steal_released
    term_disp = ""
    term_map  = string.format(
      "|steal_yield:RELEASED — yielded to p%s, released after %dt of STEAL_YIELD_BLOCK(%d) because %s; re-priced at our own cost %.0f",
      tostring(sr.by or "?"), sr.after or 0, C.STEAL_YIELD_BLOCK or 300,
      tostring(sr.reason or "?"), e.cost or 0)
  else
    -- No penalty, no ally claim — suppress the term entirely.
    return f
  end
  local sep_start = f:find("||", 1, true)
  if sep_start then
    return f:sub(1, sep_start - 1) .. term_disp .. " "
           .. f:sub(sep_start) .. term_map
  else
    return f .. term_disp .. " ||" .. term_map:sub(2)
  end
end

-- ---- pool tables moved from goals.lua ----

-- Phase weight keys for each pool evaluator (maps into C.PHASE_WEIGHTS[phase]).
-- nil = no phase weighting (always critical, e.g. refuel/attack_tank).
local POOL_NAMES = {
  "refuel", "defend_pill", "capture_base", "capture_pill", "repair_pill",
  "attack_pill", "attack_base", "place_strategic", "attack_tank",
  [10] = "reposition",
  [12] = "wait_for_lgm",
  [13] = "kill_lgm",
  [14] = "take_cover",
  -- 15 = kill_me_wait: the INITIATOR half of the "kill me" hand-off. Like
  -- 12/13/14 it is written straight into pool_cache by its own evaluator and
  -- renders as a WINNERS strip row, not a main-grid cell.
  [15] = "kill_me_wait",
}

-- Reverse map: actual goal.kind → pool index, for looking up cost_cache
-- entries by candidate.  Note pool 1 (refuel) and pool 8 (place_strategic)
-- have different UI labels than their goal.kind values.
-- Pool 10 in the JSON is the WINNERS section, 11 is offensive_build, 12 is
-- wait_for_lgm, 13 is kill_lgm, 14 is take_cover — those render as strips
-- below the main 2x5 grid.
local KIND_TO_POOL = {
  refuel_at_base = 1, defend_pill = 2, capture_base = 3, capture_pill = 4,
  repair_pill = 5, attack_pill = 6, attack_base = 7,
  place_pill_strategic = 8, attack_tank = 9, wait_for_lgm = 12,
  kill_lgm = 13, take_cover = 14, kill_me_wait = 15,
}

-- Pool DISPLAY name → goal.kind, for the two labels that differ (see the
-- KIND_TO_POOL note above). Lets the panel/grid renderers ask
-- suicider_cost_mult the same question goal_selection asked, so a suicider's
-- displayed `weighted` cost and row ordering match the cost actually competed.
local POOL_NAME_TO_KIND = {
  refuel = "refuel_at_base", place_strategic = "place_pill_strategic",
}

-- Human-readable one-liner for the HUD / breakdown chips.
function M.loaded_no_lgm_label(state)
  local f = state and state._loaded_no_lgm_factors
  if not f then return "loaded_no_lgm{unknown}" end
  local eta
  if f.eta == nil then eta = "n/a"
  elseif f.eta == math.huge then eta = "STUCK"
  else eta = string.format("%.0ft", f.eta) end
  return string.format("loaded_no_lgm{carry %d>=%d, man=%s, eta %s>%d, %s}",
    f.carry or 0, f.min_pills or 0,
    (f.man == C.LGM_DEAD) and "DEAD"
      or (f.man == C.LGM_MOVING) and "OUT" or "INTANK",
    eta, f.eta_thresh or 0, tostring(f.why))
end

-- ---- builder pool text moved from builder_pool.lua ----
-- -------------------------------------------------------------------------
-- score_terms: the one-line term breakdown, hand-checkable on its own.
--
-- THE RULE this obeys (the author's, standing): every factor in the formula
-- appears in the string, so the final number can be recomputed from the line
-- alone without opening constants.lua. Two shapes, one per formula:
--
--   linear  score 362 = hp_w(30) x missing(15) = 450 - trip_w(0.25) x trip(352t) = 88
--   legacy  val 184 - trip 95 - danger 0
--
-- ...plus, on the ONE row per tick that is the tank goal's own pill and only
-- when BUILDER_POOL_GOAL_PILL_BONUS is not 1, a third link on the end of the
-- chain: `= bp_raw{373} x goal_w{1.20}`, whose product is the bp_score{} at
-- the head. Absent on every other row, and on every row under preset=keel.
--
-- Used by BP_DISPATCH, BP_DENY's neighbours and the panel row, so all three
-- print the SAME arithmetic.
-- -------------------------------------------------------------------------
-- The trip's three legs as chips, appended to BOTH formulas so the printed
-- trip{} is never a number the reader has to take on trust: out{} + build{} +
-- back{} adds up to it, pred{} says where the return leg was walked TO, and on
-- a farm row wedge{} says which quarter of the leash square offered the tile.
--
-- Appended at the END, after tripcost{}, on purpose: tests/repair_priority_test
-- .py anchors LINEAR_RE at the start of this string, so anything inserted
-- ahead of tripcost{} breaks it. No "||" is ever produced here (the chip
-- parser splits the display half off at the first one) and no chip value
-- reaches 32 characters, which is the popup's limit.
local function leg_chips(row)
  local pred
  if row.pred_mx then
    pred = string.format("pred{%d,%d}", row.pred_mx, row.pred_my)
  else
    pred = "pred{same}"
  end
  local wedge = row.wedge and string.format(" wedge{%s}", row.wedge) or ""
  return string.format(
    " [legs out{%s} + build{%s} + back{%s} %s predsrc{%s}%s]",
    tostring(row.out_ticks or "-"),
    tostring(row.build_ticks or (C.LGM_BUILD_TIME or 20)),
    tostring(row.back_ticks or "-"), pred,
    tostring(row.pred_src or "-"), wedge)
end

-- The goal-pill bonus, as the last link of the chain: the arithmetic above it
-- produces bp_raw{}, and bp_raw x goal_w is the bp_score{} at the head of the
-- line. Printed ONLY when the factor is not 1 -- a chip that always says x1 is
-- noise on every row of every game, and its absence means exactly "this row is
-- not the goal's pill" (or BUILDER_POOL_GOAL_PILL_BONUS is 1.0, e.g.
-- preset=keel), which is why the docs say so.
local function goal_chips(row)
  local w = row.goal_w or 1.0
  if w == 1.0 then return "" end
  return string.format(" = bp_raw{%.0f} x goal_w{%.2f}", row.raw_score or 0, w)
end

-- The PING_DEFEND_REPAIR force, as the last link of the chain: the arithmetic
-- above it produces bp_raw{}, and bp_raw + ping_force is the bp_score{} at the
-- head of the line. forced{} names the step (repair / harvest). Absent on
-- every row that is not forced, and on every row with the knob off.
-- The spike care bonus, as a link of the chain after the goal weight:
-- pre_spike{} + spike_care{} is the next number. Absent unless the row is a
-- spike pill (C.SPIKE_REPAIR_PRIORITY).
local function spike_chips(row)
  if not row.spike_bonus then return "" end
  return string.format(" = pre_spike{%.0f} + spike_care{%.0f}",
                       row.spike_pre or 0, row.spike_bonus)
end
M.bp_spike_chips = spike_chips

local function force_chips(row)
  if not row.forced then return "" end
  if not row.force_bonus then
    return string.format(" forced{%s}", row.forced)
  end
  return string.format(" = bp_raw{%.0f} + ping_force{%.0f} forced{%s}",
                       row.raw_score or 0, row.force_bonus, row.forced)
end

-- Adds PING_DEFEND_REPAIR_BONUS to a forced row's finished score. Never on

function M.bp_score_terms(row)
  -- Every chip is word{value}: BrainTest's pool-grid detail popup parses
  -- exactly that shape (pool_grid.cpp) into its term table, so the same
  -- string is the hand-checkable log line AND the popup's term list.
  local score_str = row.trip and string.format("%.0f", row.score or 0)
                    or "n/a: no route"
  if row.linear then
    return string.format(
      "bp_score{%s} = hp_w{%d} x missing{%d} = value{%.0f}"
      .. " - trip_w{%.2f} x trip{%st} = tripcost{%.0f}%s%s%s",
      score_str, row.v_hp_w or (C.BUILDER_POOL_REPAIR_HP_W or 30),
      row.missing or 0, row.value or 0,
      C.BUILDER_POOL_REPAIR_TRIP_W or 0.25, tostring(row.trip or "-"),
      row.c_trip or 0, goal_chips(row) .. spike_chips(row), force_chips(row), leg_chips(row))
  end
  local urg_name = (row.type == "farm") and "urg" or "topup_hp"
  return string.format(
    "bp_score{%s} = bp_base{%.0f} + %s{%.0f} + front{%.0f} = value{%.0f}"
    .. " - trip_w{%.2f} x trip{%st} = tripcost{%.0f}"
    .. " - danger_w{%.2f} x bp_danger{%.0f} = dangercost{%.0f}%s%s%s",
    score_str, row.v_base or 0, urg_name, row.v_hp or 0, row.v_front or 0,
    row.value or 0,
    C.BUILDER_POOL_TRIP_W or 0.5, tostring(row.trip or "-"), row.c_trip or 0,
    C.BUILDER_POOL_DANGER_W or 1.5, row.danger or 0, row.c_danger or 0,
    goal_chips(row) .. spike_chips(row), force_chips(row), leg_chips(row))
end

-- -------------------------------------------------------------------------
-- The pool-grid detail string for one scored row: the short chip line before
-- "||" and, after it, one "name:computation" segment per chip (joined by
-- "|"), which the popup shows next to the chip's value and meaning.
--
-- COLD PATH.  This used to run inside score_row, so a ~35-argument
-- string.format (plus the label and score_str formats feeding it) executed for
-- every candidate on every tick even in production, where the only consumer --
-- M.bp_panel_section, below, driving BrainTest's pool grid -- never asked for it.
-- It is built on demand instead; every chip is read straight back off the row,
-- so the text is byte-for-byte what score_row used to store.
-- -------------------------------------------------------------------------
function M.bp_row_formula(row)
  local FMAX     = C.BUILDER_POOL_FRONT_MAX_TILES or 12
  local fd       = row.front_dist or 0
  local trip     = row.trip
  local out_ticks = row.out_ticks
  local dgr      = row.danger or 0
  local reject   = row.reject
  local MIN      = C.BUILDER_POOL_MIN_SCORE or 20
  local label = (row.type == "farm")
    and string.format("farm@(%d,%d)", row.mx, row.my)
    or string.format("%s p#%d@(%d,%d) %s hp=%d/%d", row.type, row.id,
                     row.mx, row.my, row.own or "?",
                     row.hp or 0, C.PILLS_MAX_HEALTH or 15)
  -- A row with no walkable route has no score to print: the sentinel that
  -- sorts it last (-1e9) is an ordering device, not an arithmetic result, and
  -- printing it as one ("= -1000000000") invites the reader to check a sum
  -- that does not exist. Say what actually happened instead.
  local score_str = trip and string.format("%.0f", row.score)
                    or "n/a (no walkable route for the man)"
  -- The goal-pill bonus, if this row has one: the tail of the arithmetic
  -- chain, so the bp_score segment ends in the number that is actually on the
  -- row rather than in the pre-bonus one.
  local gw = row.goal_w or 1.0
  local chain = score_str
  if gw ~= 1.0 then
    chain = string.format("bp_raw(%.0f) x goal_w(%.2f) = %s",
                          row.raw_score or 0, gw, score_str)
  elseif row.force_bonus then
    chain = string.format("bp_raw(%.0f) + ping_force(%.0f) = %s",
                          row.raw_score or 0, row.force_bonus, score_str)
  end
  -- Spike care: the chain ends in pre_spike + spike_care (after goal_w).
  local gw_res = score_str
  if row.spike_bonus then
    gw_res = string.format("%.0f", row.spike_pre or 0)
    local pre = gw_res
    if gw ~= 1.0 then
      pre = string.format("bp_raw(%.0f) x goal_w(%.2f) = %s", row.raw_score or 0, gw, pre)
    end
    chain = string.format("%s + spike_care(%.0f) = %s", pre, row.spike_bonus, score_str)
  end
  local short = string.format("%s %s%s", label, M.bp_score_terms(row),
                              reject and (" REJECT " .. reject) or "")
  local trip_seg = string.format(
    "trip:out(%s) + build(%s) + back(%s) = %s brain ticks for the man"
    .. " to walk out, build and walk back",
    tostring(out_ticks or "-"),
    tostring(row.build_ticks or (C.LGM_BUILD_TIME or 20)),
    tostring(row.back_ticks or "-"), tostring(trip or "-"))
  -- The legs, one segment per chip. Written once and spliced into both
  -- formulas' segment lists, so a repair row and a farm row explain the walk
  -- with the same words.
  local out_seg = string.format(
    "out:cpf_lgm_travel_ticks_map(tank -> (%d,%d), that tile BLESSED so a pill"
    .. "/base on it does not self-block) = %s brain ticks. The straight-line"
    .. " engine walk sim (brain_pathfinder.c lgmTravelTicksCore), not a path"
    .. "finder -- a wall in the way reads STUCK and the row goes unreachable."
    .. " A target 1 tile away or less skips the sim and is charged"
    .. " BUILDER_POOL_GRASS_TICKS_PER_TILE(%d).",
    row.mx, row.my, tostring(out_ticks or "-"),
    C.BUILDER_POOL_GRASS_TICKS_PER_TILE or 16)
  local build_seg = string.format(
    "build:LGM_BUILD_TIME(%d) = brain ticks the man stands on the tile"
    .. " building / repairing / chopping (lgm.h:84)",
    C.LGM_BUILD_TIME or 20)
  local back_seg, pred_seg, predsrc_seg
  if not C.BUILDER_POOL_RETURN_PREDICT then
    back_seg = string.format(
      "back:BUILDER_POOL_RETURN_PREDICT is OFF, so the return leg is the"
      .. " outbound leg mirrored -- out(%s) again, i.e. the tank is assumed to"
      .. " wait on the spot.", tostring(out_ticks or "-"))
    pred_seg = "pred:no prediction (BUILDER_POOL_RETURN_PREDICT off) -- the"
      .. " walk home is measured back to the tank's CURRENT tile."
  elseif row.pred_mx then
    back_seg = string.format(
      "back:cpf_lgm_travel_ticks_map((%d,%d) -> the PREDICTED tank tile"
      .. " (%d,%d), the job tile BLESSED because he starts standing on it)"
      .. " = %s brain ticks.",
      row.mx, row.my, row.pred_mx, row.pred_my, tostring(row.back_ticks or "-"))
    pred_seg = string.format(
      "pred:tank(%d,%d) walked %s brain ticks forward along its OWN ROUTE"
      .. " (state.pf.path_chain, waypoint %s of %s, reached at t=%s) at the"
      .. " engine's per-terrain speed caps (C.MAP_SPEED, bolo_map.h"
      .. " MAP_SPEED_T*; 256 WU a tile, 362 diagonally) -> tile (%d,%d)."
      .. " Horizon = min(out + build, BUILDER_POOL_RETURN_PREDICT_MAX_TICKS(%d)).",
      bit.rshift((row.f_tankx or 0), 8), bit.rshift((row.f_tanky or 0), 8),
      tostring(row.pred_horizon or "-"), tostring(row.pred_route_i or "-"),
      tostring(row.pred_route_n or "-"),
      row.pred_route_t and string.format("%.0f", row.pred_route_t) or "-",
      row.pred_mx, row.pred_my,
      C.BUILDER_POOL_RETURN_PREDICT_MAX_TICKS or 400)
  else
    back_seg = string.format(
      "back:the prediction FELL BACK to the tank's current tile (%s), so the"
      .. " return leg is the outbound leg mirrored -- out(%s).",
      tostring(row.pred_why or "-"), tostring(out_ticks or "-"))
    pred_seg = string.format(
      "pred:same tile as the tank -- fallback reason '%s'. no_route / "
      .. "route_stale / route_ended / route_no_progress = the tank has no"
      .. " committed route to walk (idle, or the chain belongs to a goal it has"
      .. " left); horizon_too_short = it has one but cannot clear its own tile"
      .. " inside out+build; off_map / unwalkable = the predicted tile is"
      .. " water, a building or a live pill; back_unreachable = the walk sim"
      .. " could not get the man from the job to it. Horizon was %s ticks.",
      tostring(row.pred_why or "-"), tostring(row.pred_horizon or "-"))
  end
  predsrc_seg = string.format(
    "predsrc:where pred{} came from. route = walked along state.pf.path_chain,"
    .. " the route the navigator computed and steering is already driving"
    .. " (nothing is re-searched here). same = a fallback landed on the tank's"
    .. " own tile, which makes back == out, the pre-2026-09-06 number."
    .. " off = BUILDER_POOL_RETURN_PREDICT is disabled. This row: %s.",
    tostring(row.pred_src or "-"))
  local wedge_seg = row.wedge and string.format(
    "wedge:the 90-degree quarter of the BUILDER_POOL_LEASH(%d) square this"
    .. " forest was the nearest in -- N/E/S/W, boundaries on the 45-degree"
    .. " diagonals (abs(dx) > abs(dy) is E/W, otherwise N/S; the diagonal"
    .. " itself goes to the vertical wedge). One row per wedge, so a nearer forest the"
    .. " man cannot straight-line to loses to a clear one elsewhere."
    .. " 'all' = BUILDER_POOL_FARM_SECTORS is 1 (one row, nearest anywhere).",
    C.BUILDER_POOL_LEASH or 8) or nil
  local tail = string.format(
    " %s trees need %d, have %d, reserved %d. leash %d, dist %s, front_dist %d.%s",
    row.forced
      and "FORCED (PING_DEFEND_REPAIR): MIN_SCORE, leash, tree reserve,"
          .. " reserve_eta and mode_owned are waived; the safety gates still apply."
      or string.format("Fires only if score >= BUILDER_POOL_MIN_SCORE(%d) and no"
                       .. " gate rejects.", MIN),
    row.trees_need or 0, row.f_trees or 0, row.f_reserve or 0,
    row.leash or C.BUILDER_POOL_LEASH or 8, row.dist and M.fmt_ld(row.dist) or "-1", fd,
    reject and (" REJECTED: " .. reject) or " ACCEPTED.")
  local segs
  -- The linear repair row (BUILDER_POOL_REPAIR_LINEAR). Two terms and no
  -- others, so the string is two terms and no others -- printing a front /
  -- danger chip that scores nothing would invite the reader to check a sum
  -- that is not the sum the code computed.
  if row.linear then
    local hp_w = row.v_hp_w or (C.BUILDER_POOL_REPAIR_HP_W or 30)
    local tw   = C.BUILDER_POOL_REPAIR_TRIP_W or 0.25
    segs = {
      string.format("bp_score:value(%.0f) - tripcost(%.0f) = %s. NO danger term"
        .. " (threat.at(%.0f) here is printed on the panel, not charged) and"
        .. " NO path-safety gate on repair rows.%s",
        row.value or 0, row.c_trip or 0, chain, dgr, tail),
      string.format("hp_w:BUILDER_POOL_REPAIR_HP_W(%d) = points per missing hp", hp_w),
      string.format("missing:PILLS_MAX_HEALTH(%d) - hp(%d) = %d",
        C.PILLS_MAX_HEALTH or 15, row.hp or 0, row.missing or 0),
      string.format("value:hp_w(%d) x missing(%d) = %.0f",
        hp_w, row.missing or 0, row.value or 0),
      trip_seg,
      out_seg, build_seg, back_seg, pred_seg, predsrc_seg,
      string.format("trip_w:BUILDER_POOL_REPAIR_TRIP_W(%.2f) = points per round-trip tick", tw),
      string.format("tripcost:trip_w(%.2f) x trip(%s) = %.0f",
        tw, tostring(trip or "-"), row.c_trip or 0),
    }
  else
    local tw = C.BUILDER_POOL_TRIP_W or 0.5
    local dw = C.BUILDER_POOL_DANGER_W or 1.5
    local urg_seg
    if row.type == "topup" then
      urg_seg = string.format("topup_hp:BUILDER_POOL_TOPUP_PER_HP(%d) x missing(%d) = %.0f",
        C.BUILDER_POOL_TOPUP_PER_HP or 6, row.missing or 0, row.v_hp or 0)
    elseif row.type == "farm" then
      urg_seg = string.format(
        "urg:BUILDER_POOL_FARM_URGENCY(%d) x max(0, FARM_LOW_TREES(%d) - trees(%d)) = %.0f",
        C.BUILDER_POOL_FARM_URGENCY or 12, C.BUILDER_POOL_FARM_LOW_TREES or 12,
        row.f_trees or 0, row.v_hp or 0)
    else
      urg_seg = "topup_hp:no per-hp term on a rebuild row = 0"
    end
    local front_seg = (row.type == "farm")
      and "front:no front clock on farm rows (a forest cannot be stolen) = 0"
      or string.format(
        "front:BUILDER_POOL_FRONT_URGENCY(%d) x max(0, FRONT_MAX(%d) - front_dist(%d)) / FRONT_MAX(%d) = %.0f",
        C.BUILDER_POOL_FRONT_URGENCY or 120, FMAX, fd, FMAX, row.v_front or 0)
    segs = {
      string.format("bp_score:value(%.0f) - tripcost(%.0f) - dangercost(%.0f) = %s.%s",
        row.value or 0, row.c_trip or 0, row.c_danger or 0, chain, tail),
      string.format("bp_base:BUILDER_POOL_VALUE_%s(%.0f) = fixed value of this job type",
        string.upper(row.type), row.v_base or 0),
      urg_seg,
      front_seg,
      string.format("value:base(%.0f) + %s(%.0f) + front(%.0f) = %.0f",
        row.v_base or 0, (row.type == "farm") and "urg" or "topup_hp",
        row.v_hp or 0, row.v_front or 0, row.value or 0),
      trip_seg,
      out_seg, build_seg, back_seg, pred_seg, predsrc_seg,
      string.format("trip_w:BUILDER_POOL_TRIP_W(%.2f) = points per round-trip tick", tw),
      string.format("tripcost:trip_w(%.2f) x trip(%s) = %.0f",
        tw, tostring(trip or "-"), row.c_trip or 0),
      string.format("bp_danger:threat.at(%d,%d) = %.0f (hostile pills/tanks with the tile in range)",
        row.mx, row.my, dgr),
      string.format("danger_w:BUILDER_POOL_DANGER_W(%.2f) = points per danger unit", dw),
      string.format("dangercost:danger_w(%.2f) x danger(%.0f) = %.0f",
        dw, dgr, row.c_danger or 0),
    }
  end
  -- The bonus's own two segments, on both formulas, only when it applied.
  if gw ~= 1.0 then
    segs[#segs + 1] = string.format(
      "bp_raw:the score BEFORE the goal-pill bonus -- value(%.0f) minus the"
      .. " costs above = %.0f. bp_raw x goal_w is the bp_score at the head of"
      .. " the line.", row.value or 0, row.raw_score or 0)
    if row.goal_src == "ping" then
      segs[#segs + 1] = string.format(
        "goal_w:BUILDER_POOL_PING_PILL_BONUS(%.2f) -- a human (p%s) bot-command"
        .. " pinged this row's pill (#%s) and this bot took the order, so its whole score is multiplied by"
        .. " it until t=%s, the pill is dead or fully repaired, or another of"
        .. " our pills is pinged: %.0f x %.2f = %s. It replaces the goal-pill"
        .. " bonus (the larger one is used, never both).",
        gw, tostring(row.ping_by or "?"), tostring(row.id or "?"),
        tostring(row.ping_until or "?"),
        row.raw_score or 0, gw, gw_res)
    else
      segs[#segs + 1] = string.format(
        "goal_w:BUILDER_POOL_GOAL_PILL_BONUS(%.2f) -- this row's pill (#%s) IS"
        .. " the tank goal's own target (%s target_id=%s), so its whole score is"
        .. " multiplied by it: %.0f x %.2f = %s. The chip is absent on every"
        .. " other row, and on every row when the bonus is 1.0 (preset=keel).",
        gw, tostring(row.id or "?"),
        tostring(row.goal_kind or "goal"), tostring(row.id or "?"),
        row.raw_score or 0, gw, gw_res)
    end
  end
  -- The spike care bonus's own two segments, only when it applied.
  if row.spike_bonus then
    segs[#segs + 1] = string.format(
      "pre_spike:the score BEFORE the spike care bonus = %.0f", row.spike_pre or 0)
    segs[#segs + 1] = string.format(
      "spike_care:SPIKE_REPAIR_BONUS(%.0f) -- this pill is a focused spike (src %s,"
      .. " landed t=%s) under spike care for SPIKE_CARE_TICKS(%d) while its tile"
      .. " stays aggro; it also tops up from SPIKE_REPAIR_MIN_MISSING(%d) hp and"
      .. " reaches SPIKE_REPAIR_LEASH(%d): %.0f + %.0f = %s",
      row.spike_bonus, tostring(row.spike_src), tostring(row.spike_t),
      C.SPIKE_CARE_TICKS, C.SPIKE_REPAIR_MIN_MISSING, C.SPIKE_REPAIR_LEASH,
      row.spike_pre or 0, row.spike_bonus, score_str)
  end
  -- The PING_DEFEND_REPAIR force's own segments, one per chip it prints.
  if row.forced then
    if row.force_bonus then
      segs[#segs + 1] = string.format(
        "bp_raw:the score BEFORE the ping force -- value(%.0f) minus the costs"
        .. " above = %.0f. bp_raw + ping_force is the bp_score at the head of"
        .. " the line.", row.value or 0, row.raw_score or 0)
      segs[#segs + 1] = string.format(
        "ping_force:PING_DEFEND_REPAIR_BONUS(%.0f) added to the score: %.0f +"
        .. " %.0f = %s. It only lifts the row to the top of the panel; the"
        .. " sort puts a forced row first whatever the numbers.",
        row.force_bonus, row.raw_score or 0, row.force_bonus, score_str)
    end
    segs[#segs + 1] = string.format(
      "forced:%s -- this bot holds a human (p%s) bot-command PING defend order"
      .. " on our pill #%s (hp %s/%d), and the tank is %.1f tiles from it"
      .. " (straight line) <= PING_DEFEND_REPAIR_RANGE(%d). %s The row ignores"
      .. " the ping / goal-pill weight. It ends when the order ends, the pill is"
      .. " full, dead or lost, or the tank is beyond the range.",
      tostring(row.forced), tostring(row.force_by or "?"),
      tostring(row.force_tid or "?"), tostring(row.force_hp or "?"),
      C.PILLS_MAX_HEALTH or 15, row.force_dist or 0,
      C.PING_DEFEND_REPAIR_RANGE or 10,
      (row.forced == "harvest")
        and "We carry no trees, so step 1 is this harvest: of the farm rows"
            .. " that pass their own gates (path safety kept), the one with"
            .. " the shortest walk out (out{}) is forced; the repair follows"
            .. " when the man is home with wood."
        or "Needs 1 tree (engine lgm_cost_pill_repair); the man takes a full"
           .. " load and the engine spends only what the damage needs.")
  end
  if wedge_seg then segs[#segs + 1] = wedge_seg end
  return short .. "||" .. table.concat(segs, "|")
end

-- -------------------------------------------------------------------------
-- Panel section (pool_grid JSON, section BUILDER_POOL_PANEL_IDX).
--
-- NOT a new pool: 1..10 keep the 2x5 grid and 11..14 keep their strips, so old
-- recordings load unchanged. Three header lines then one row per candidate:
--
--   BUILDER  owner=attack_pill/wall_shield  (man out, eta 60)
--     eligibility: mode=idle-ish - under_fire=no(38t) - reserve_eta=110 - trees 14(res 8)
--     active: rebuild p15 (outbound, eta 44t)
--
-- Rows use the shared renderer (cost + formula short||long), so the long form
-- is hand-checkable exactly like every other pool row. cost is -score, because
-- the renderer sorts and colours ASCENDING (cheapest = best) and the pool
-- scores DESCENDING.
-- -------------------------------------------------------------------------
function M.bp_panel_section(state)
  local bp = state._builder_pool
  if not bp then return nil end
  local hdr = {}
  local job = bp.job
  -- Name the actual obstacle rather than "unavailable": "the man is walking a
  -- job", "he is dead", "we are afloat" and "he is out on a wall shield" are
  -- four different situations and only one of them is the pool's doing.
  local why_no_man = ""
  if not bp.can_send then
    if job then
      why_no_man = string.format("  (man out on %s @(%d,%d), %s)",
                                 job.type, job.mx, job.my, job.phase or "?")
    elseif bp.man_status == C.LGM_DEAD then     why_no_man = "  (man dead)"
    elseif bp.man_status == C.LGM_MOVING then   why_no_man = "  (man out, not on a pool job)"
    elseif bp.inboat then                       why_no_man = "  (afloat)"
    else                                        why_no_man = "  (man unavailable)" end
  end
  hdr[#hdr + 1] = string.format("owner=%s/%s%s%s",
    bp.owner_kind, bp.owner_mode,
    bp.owner_sub and ("/" .. bp.owner_sub) or "", why_no_man)
  hdr[#hdr + 1] = string.format(
    "eligibility: %s | under_fire=%s | reserve_eta=%s | trees %d (res %d = base %d + pills %d + goal %d + sea %d)",
    bp.ok and "OK" or ("DENY " .. tostring(bp.reason)),
    (bp.detail and bp.detail.fire_age)
      and string.format("%dt ago (%s)", bp.detail.fire_age,
                        tostring(bp.detail.fire_why)) or "never",
    -- WAIVED is printed beside the number, never instead of it: the panel has
    -- to show both what the goal asked for and that the pool did not charge
    -- the other rows for it (BUILDER_POOL_SEEDED_COMPETES).
    string.format("%s (%s)%s", tostring(bp.reserve_eta or "-"),
                  tostring(bp.reserve_why or "none"),
                  bp.reserve_waived
                    and (" WAIVED: " .. bp.reserve_waived) or ""),
    bp.trees, bp.reserve, bp.r_base, bp.r_pills, bp.r_goal, bp.r_sea)
  if job then
    hdr[#hdr + 1] = string.format("active: %s @(%d,%d) %s eta=%st claim=t%d%s",
      job.type, job.mx, job.my, job.phase or "?",
      tostring(job.eta or "-"), job.claim_tick or 0,
      job.seeded and (" seeded_by=" .. tostring(job.seeded)) or "")
  else
    local last = state._bp_last
    hdr[#hdr + 1] = last
      and string.format("active: none (last %s @(%d,%d) %s at t%d%s)",
                        last.type, last.mx, last.my, last.outcome, last.tick,
                        last.lgm_dead and ", LGM killed" or "")
      or "active: none"
  end
  -- The PING_DEFEND_REPAIR force, when a ping defend order is held (the
  -- panel's fourth and last header line; pool_grid.cpp keeps four).
  local f = bp.force
  if f then
    hdr[#hdr + 1] = string.format(
      "PING REPAIR FORCED: pill #%d @(%d,%d) hp %d/%d from p%s, tank %.1f <= %d"
      .. " tiles, step=%s%s",
      f.tid, f.mx, f.my, f.hp, C.PILLS_MAX_HEALTH or 15, tostring(f.sender),
      f.dist, f.range, f.step,
      bp.force_wait and (", waiting: " .. bp.force_wait) or "")
  elseif bp.force_why and bp.force_why ~= "no_ping_defend_order"
         and bp.force_why ~= "knob_off" then
    hdr[#hdr + 1] = string.format(
      "ping repair: not forced (%s%s)", bp.force_why,
      bp.force_dist and string.format(", tank %.1f tiles, range %d",
                                      bp.force_dist,
                                      C.PING_DEFEND_REPAIR_RANGE or 10) or "")
  end

  local rows = {}
  local cap = C.BUILDER_POOL_PANEL_ROWS or 8
  for i = 1, math.min(#bp.rows, cap) do
    local r = bp.rows[i]
    rows[#rows + 1] = {
      id = (r.id and r.id >= 0) and r.id or (r.my * 256 + r.mx),
      mx = r.mx, my = r.my,
      -- Renderer convention: lower is better. The pool's score is
      -- higher-is-better, so the row cost is its negation; a rejected row
      -- shows INF like every other rejected row in the grid.
      cost = r.reject and 1e30 or -(r.score or 0),
      -- Built here, on the panel's cold path, rather than stored on the row by
      -- score_row (see M.bp_row_formula).
      formula = M.bp_row_formula(r),
      stale = 0,
      reject = r.reject and (r.reject:match("^[a-z_]+") or r.reject) or nil,
      reject_remaining = 0,
    }
  end
  return {
    idx = C.BUILDER_POOL_PANEL_IDX or 15,
    name = "BUILDER",
    weight = 1.0,
    winner_id = (bp.job and bp.job.pill_id) or -1,
    hdr = hdr,
    rows = rows,
  }
end

-- ---- breakdown moved from goals.lua ----
-- The breakdown itself: the same code that used to run inside
-- goals.get_pool_breakdown_json, run on a decoded capture. `state` is the
-- capture's copy of the brain tables (same field names as the brain state);
-- `K` holds the values the brain computed for the text (see
-- goals.capture_pool_raw). Writes here only touch the decoded copy.
local function render_state(state, K)
  if K.off then
    return string.format(
      '{"phase":"%s","tick":%d,"replan_left":0,"bot":%d,"sections":[{"id":"off","label":"Pool viz","rows":[{"id":0,"mx":0,"my":0,"cost":0,"formula":"BRAIN_POOL_VIZ is off","stale":-1,"active":false,"imminent":false,"reject":null}]}]}',
      state.phase or "?", state.tick or 0, state.player_number or 0)
  end
  local now = state.tick or 0
  local cache = state.cost_cache or {}
  local pc = state.pool_cache or {}
  local phase_weights = C.PHASE_WEIGHTS[state.phase]

  -- Refuel live cost-shape (pool 1) for the term breakdown. goal_selection
  -- only stashes ur{}/def{}/fill{} inside its pool-competition block, which is
  -- skipped whenever an earlier override (command / critical flee / capture
  -- objective) already set the goal — leaving pool-1 rows with a stale,
  -- shapeless cached formula. Recompute it here (display-only; does NOT touch
  -- the real cost path) every time the grid is read so the shape always
  -- reflects current armour/shells regardless of how the goal was chosen.
  do
    local li = state._last_info
    local rs = K.rs
    if li and li.armour and rs then
      -- Same shared shape the real cost path uses (scarcity + mine surcharge).
      -- The capture ran goals' refuel_shape and kept its nine results.
      local bonus, mult, fill, scarcity, mine_cost, urgency, arm_def, sh_def, mine_d =
        rs.bonus, rs.mult, rs.fill, rs.scarcity, rs.mine_cost, rs.urgency,
        rs.arm_def, rs.sh_def, rs.mine_d
      -- The surcharge is only ever charged at the base under the tank, so only
      -- that row gets the mines chip — same rule the cost path applies.
      local _ltmx = li.tankx and bit.rshift(li.tankx, 8) or nil
      local _ltmy = li.tanky and bit.rshift(li.tanky, 8) or nil
      for _, e in pairs(cache) do
        if e._p == 1 then
          local _here = (_ltmx ~= nil and e._mx == _ltmx and e._my == _ltmy)
          e._urgency     = urgency
          e._base_floor  = C.REFUEL_BASE_COST
          e._defic_bonus = bonus
          e._fill_mult   = mult
          e._fill        = fill
          e._scarcity    = scarcity
          e._mine_cost   = mine_cost
          e._mine_d      = _here and mine_d or nil
          e._arm         = li.armour
          e._sh          = li.shells or 0
          e._arm_def     = arm_def
          e._sh_def      = sh_def
          e._notlow      = rs.notlow
          e.formula      = nil  -- force live re-render with shape tokens
        end
      end
    end
  end

  -- Is this pill a blitz (for the pool-viz BLITZ row style)? True when it's our
  -- own blitz lead (commander), the blitz we've committed to, the one we're
  -- negotiating to join, or any teammate's currently-open blitz call.
  local function is_blitz_pill(pid)
    if not pid or pid < 0 then return false end
    local g = state.goal
    if g and g.kind == "attack_pill" and g.target_id == pid and g._blitz then return true end
    if state.squad_blitz_target == pid then return true end
    if state.squad_negotiate_pill == pid then return true end
    if state.blitz_calls then
      for _, c in pairs(state.blitz_calls) do
        if c.pill == pid then return true end
      end
    end
    return false
  end

  local replan_left = 0
  if state.replan_offset then
    replan_left = C.GOAL_REPLAN_INTERVAL
        - ((now + state.replan_offset) % C.GOAL_REPLAN_INTERVAL)
    if replan_left == C.GOAL_REPLAN_INTERVAL then replan_left = 0 end
  end

  -- Fixed 2x5 layout matching the optimize-branch poolwindow.
  -- (Indexes 11/12 used by offensive_build/wait_for_lgm strips below the grid.)
  local LAYOUT_CELL = {
    [1] = {1,1}, [2] = {1,2}, [3] = {1,3}, [4] = {1,4}, [5] = {1,5},
    [6] = {2,1}, [7] = {2,2}, [8] = {2,3}, [9] = {2,4}, [10]= {2,5},
  }

  -- Active-goal lookup. The renderer marks the row whose (pool, id)
  -- matches the bot's currently-committed goal so the user can see at
  -- a glance which candidate is actually being acted on (vs. just the
  -- sort-winner). Map the ACTIVE goal to its pool via the module-level
  -- KIND_TO_POOL (goal-kind keyed). NOT a pool-NAME map: some goal kinds differ
  -- from their pool name (place_pill_strategic→8, refuel_at_base→1), and the old
  -- name map left active_pool nil for those, so the active goal vanished from
  -- WINNERS. Also capture the goal tile so we can match place_strategic (which
  -- has no target_id) by position.
  local active_pool, active_id, active_mx, active_my = nil, nil, nil, nil
  if state.goal and state.goal.kind then
    active_pool = KIND_TO_POOL[state.goal.kind]
    active_id   = state.goal.target_id
    active_mx, active_my = state.goal.mx, state.goal.my
  end

  -- Group eval_queue items by pool, same iteration as the text version.
  -- Format a per-entry reject-history segment that gets appended to the
  -- formula breakdown so the pool panel shows the full timeline of
  -- _reject changes for this (pool, id) across the session.  Compact
  -- form: "|history: t=NNNN <action>[/by=pN] / t=NNNN <action> / ...".
  -- Cap at the last 8 events to keep the formula string under
  -- pool_grid.cpp's 400-byte segment cap.
  local function format_reject_history(rh)
    if not rh or #rh == 0 then return "" end
    local first = math.max(1, #rh - 7)
    local parts = { "|history:" }
    for i = first, #rh do
      local h = rh[i]
      local act
      if h.reason then
        act = "set:" .. h.reason
        if h.by then act = act .. " by=p" .. tostring(h.by) end
      else
        act = "cleared"
        if h.prev then act = act .. " (was " .. h.prev .. ")" end
      end
      parts[#parts + 1] = string.format(" t=%d %s", h.t or 0, act)
      if i < #rh then parts[#parts + 1] = " /" end
    end
    return table.concat(parts)
  end
  local reject_history = state.reject_history or {}

  -- Capture cached.tick so we can compute staleness per row.
  local by_pool = {}
  for _, item in ipairs(state.eval_queue or {}) do
    local p = item.pool
    local obj = item.obj
    -- _reprice items are extra copies of a queue item (apply_reprice_queue):
    -- the original item already gives the pill its row.
    if obj and not item._reprice then
      by_pool[p] = by_pool[p] or {}
      local cached = cache[p .. ":" .. item.id]
      local formula_str = (cached and get_formula(cached)) or ""
      -- The tick this price was computed, on the row line itself.
      if cached and cached.tick then
        local tag = string.format(" [t=%d]", cached.tick)
        local cut = formula_str:find("!!", 1, true) or formula_str:find("||", 1, true)
        if cut then
          formula_str = formula_str:sub(1, cut - 1) .. tag .. " " .. formula_str:sub(cut)
        else
          formula_str = formula_str .. tag
        end
      end
      local hist_str = format_reject_history(reject_history[p .. ":" .. item.id])
      if hist_str ~= "" then
        local sep = formula_str:find("||", 1, true)
        if sep then
          formula_str = formula_str .. hist_str
        else
          formula_str = formula_str .. "||" .. hist_str:sub(2)
        end
      end
      by_pool[p][#by_pool[p] + 1] = {
        id = item.id, mx = obj.mx or 0, my = obj.my or 0,
        -- 1e30 (≥ renderer's 1e9 INF threshold) for candidates not yet
        -- evaluated, so the panel shows INF rather than a misleading -1.
        cost = (cached and cached.cost) or 1e30,
        formula = formula_str,
        stale = (cached and cached.tick) and (now - cached.tick) or -1,
        -- Show the reject chip honestly: an open/joinable blitz reads as "blitz",
        -- not "ally_claimed" (which is reserved for a truly-closed solo take).
        -- Pool 6 INF rows carry their INF reason (_skipped) in the same chip
        -- when no real reject is set — same as attack_tank's `skipped` rows.
        reject = cached and ((cached._reject == "ally_claimed" and cached._reject_joinable_blitz)
                             and "blitz" or cached._reject or cached._skipped) or nil,
        reject_remaining = cached and cached._reject_remaining or 0,
        ally_score = cached and cached._ally_score or nil,
        ally_by    = cached and cached._ally_by    or nil,
        stealing   = (cached and cached._stealing) or false,
        imminent = (cached and cached.imminent) or false,
      }
    end
  end

  local function attack_tank_formula(b)
    -- "KILL ME" delivery row: the subject is an ALLY, and its price is
    -- base + travel and nothing else. Its own formula, so the row can never
    -- be read as an ordinary attack_tank standoff calculation.
    if b.kill_me then
      local head = string.format("subject=ally p%d @(%d,%d)", b.km_ally_pn or -1,
                                 b.mx or 0, b.my or 0)
      if b.skipped then
        return string.format(
          "REJECT %s %s||reject:%s -- %s|kill_me:p%d is loaded and builder-less and has asked to be killed for its cargo."
          .. " Allied shells DO damage allied tanks (tank.c tankIsTankHit ignores only the shooter's own shells) and the"
          .. " corpses keep the dead player's ownership, which capture_pill already scoops."
          .. " The row is priced INF but still shown so the reason is visible|need:ceil(%d armour / %d[TANK_SHELL_DAMAGE])"
          .. " + %d[KILL_ME_SHELL_MARGIN], floored at %d[TANK_COMBAT_MIN_SHELLS] = %d shells; we hold %d",
          b.skipped, head, b.skipped, tostring(b.kill_me_why),
          b.km_ally_pn or -1, b.km_ally_armour or 0, C.TANK_SHELL_DAMAGE or 5,
          C.KILL_ME_SHELL_MARGIN or 2, C.TANK_COMBAT_MIN_SHELLS or 10,
          b.km_need_shells or 0, b.tank_shells or 0)
      end
      return string.format(
        "KILL_ME %s (base{%.0f} + travel{%.0f}) = %.0f"
        .. "||kill_me:p%d is loaded and builder-less and has asked to be killed for its cargo. Drive into gun range and"
        .. " fire until it dies; the corpses it drops keep p%d's ownership and capture_pill takes them"
        .. "|base:KILL_ME_RESPONDER_BASE{%.0f}|travel:Dijkstra/A* cost from (%d,%d) to the ally's advertised tile (%d,%d)"
        .. "|terms:NOTHING else is charged -- no aim bonus, no crossfire, no low-shells ramp, no influence x0.5/x2 and no"
        .. " ATTACK_NO_BUILDER_MULT. A delivery is a delivery"
        .. "|need:ceil(%d armour / %d[TANK_SHELL_DAMAGE]) + %d[KILL_ME_SHELL_MARGIN], floored at %d[TANK_COMBAT_MIN_SHELLS]"
        .. " = %d shells; we hold %d",
        head, b.base or 0, b.path_cost or 0, b.cost or 0,
        b.km_ally_pn or -1, b.km_ally_pn or -1,
        b.base or 0,
        state._last_info and bit.rshift(state._last_info.tankx or 0, 8) or -1,
        state._last_info and bit.rshift(state._last_info.tanky or 0, 8) or -1,
        b.mx or 0, b.my or 0,
        b.km_ally_armour or 0, C.TANK_SHELL_DAMAGE or 5,
        C.KILL_ME_SHELL_MARGIN or 2, C.TANK_COMBAT_MIN_SHELLS or 10,
        b.km_need_shells or 0, b.tank_shells or 0)
    end
    local reason = b.skipped
    if reason then
      local who = b.player_name and (" player=" .. b.player_name) or ""
      return string.format(
        "REJECT %s @(%d,%d)%s||reject:%s; dist=%d max=%d shells=%d armour=%d in_boat=%s%s",
        reason, b.mx or 0, b.my or 0, who, reason,
        b.dist or 0, C.TANK_COMBAT_MAX_RANGE,
        b.tank_shells or 0, state._last_info and state._last_info.armour or 0,
        tostring(state._last_info and state._last_info.inboat or false), who)
    end
    if b.los_engage then
      return string.format(
        "LOS (los_base{%.0f} + dist{%.1f}*per_tile{%.0f} + low_sh{%.0f}) * boat{%.2f} + threat{%.0f} + far_preempt{%.0f} = %.0f" ..
        "||dist=%.1f; shells_now=%d; aim_diff=%.1f; far_preempt=exp penalty when on an attack_pill + tank past shoot range",
        C.TANK_COMBAT_LOS_BASE_COST, b.dist or 0, C.TANK_COMBAT_LOS_COST_PER_TILE,
        b.low_shells_penalty or 0, b.boat_mult or 1.0,
        b.tank_tile_threat or 0, b.far_preempt_pen or 0, b.cost or 0,
        b.dist or 0, b.tank_shells or 0, b.aim_diff or 0)
    end
    return string.format(
      "standoff->(%s,%s) (A* to standoff{%.0f} + base{%.0f} + wall{%.0f} + low_sh{%.0f} - aim{%.0f} + xfire{%.0f}) * boat{%.2f} + threat{%.0f} + far_preempt{%.0f} = %.0f" ..
      "||A* is to the FIRING STANDOFF (%s,%s), NOT the enemy tile (%d,%d) — that's why it's cheaper than a test-click on the enemy. far_preempt=exp penalty when on an attack_pill + tank past shoot range. dist=%d; shells_now=%d; shells_arrival=%s; aim_diff=%.1f; wall_hp=%s; deg=%s",
      tostring(b.standoff_mx), tostring(b.standoff_my),
      b.path_cost or 0, b.base or 0, b.wall_penalty or 0,
      b.low_shells_penalty or 0, b.aim_bonus or 0, b.crossfire or 0,
      b.boat_mult or 1.0, b.tank_tile_threat or 0, b.far_preempt_pen or 0, b.cost or 0,
      tostring(b.standoff_mx), tostring(b.standoff_my), b.mx or 0, b.my or 0,
      b.dist or 0, b.tank_shells or 0, tostring(b.shells_on_arrival),
      b.aim_diff or 0, tostring(b.wall_hp), tostring(b.standoff_deg))
  end

  local function append_attack_tank_breakdown()
    local bd = state.attack_tank_breakdown or {}
    by_pool[9] = {}
    local seen_ids = {}

    local function append_row(b)
      local id = b.id
      if id == nil or id < 0 then id = (b.my or 0) * 256 + (b.mx or 0) end
      if id ~= nil then seen_ids[id] = true end
      by_pool[9][#by_pool[9] + 1] = {
        id = id, mx = b.mx or 0, my = b.my or 0,
        cost = b.cost or 1e30,
        formula = attack_tank_formula(b),
        stale = b.stale or 0,
        reject = b.skipped,
        reject_remaining = 0,
      }
    end

    for i, b in ipairs(bd) do
      append_row(b)
    end

    -- eval_attack_tank only runs when the planner evaluates that pool, but
    -- perception is refreshed every tick. Fill the panel from perception so
    -- visible tanks appear even before/after a full attack_tank scoring pass.
    local perc = state.perc
    for _, et in ipairs((perc and perc.enemy_tanks) or {}) do
      local id = et.id
      if id == nil or not seen_ids[id] then
        local skipped = "pending_eval"
        append_row({
          id = id, mx = et.mx, my = et.my, dist = et.dist, speed = et.speed,
          tank_shells = state._last_info and state._last_info.shells or 0,
          cost = 1e30, shells_on_arrival = 0, skipped = skipped,
          stale = 0,
        })
      end
    end

    -- Also list every known enemy player slot whose tank is not currently
    -- visible. This keeps the pool informative when no hostile tank object is
    -- in perception range or line of sight.
    local li = state._last_info
    if li and li.player_names then
      local allies = li.allies or 0
      for pn = 0, (li.max_players or 0) - 1 do
        if pn ~= li.player_number and not seen_ids[pn] then
          local name = li.player_names[pn + 1]
          local active = (type(name) == "string" and name ~= "")
                      or pn < (li.num_players or 0)
          local allied = (bit.band(allies, (bit.lshift(1, pn)))) ~= 0
          if active and not allied then
            if type(name) ~= "string" or name == "" then name = "player " .. tostring(pn) end
            append_row({
              id = pn, mx = -1, my = -1, dist = 0, speed = 0,
              tank_shells = li.shells or 0,
              cost = 1e30, shells_on_arrival = 0,
              skipped = "not_visible", player_name = name,
              stale = 0,
            })
          end
        end
      end
    end
  end

  append_attack_tank_breakdown()

  -- Inject place_strategic (pool 8) candidates from finalize_pools result.
  -- When not evaluated, inject a single stub row explaining why.
  if pc[8] and pc[8].cands then
    by_pool[8] = pc[8].cands
  else
    local li = state._last_info
    local reason
    if not li or (li.carried_pills or 0) < 1 then
      reason = "no_pill_in_tank"
    elseif li.inboat then
      reason = "in_boat"
    elseif li.man_status ~= C.LGM_INTANK then
      reason = "lgm_not_in_tank"
    else
      reason = "not_evaluated"
    end
    by_pool[8] = {{
      id = -1, mx = 0, my = 0,
      cost = -1,
      formula = "SKIP " .. reason .. "||" .. reason,
      stale = 0,
      reject = reason,
      reject_remaining = 0,
    }}
  end

  -- Inject defend_pill (pool 2) rows. eval_defend_pill is a finalize pool —
  -- it never enqueues eval_queue items — and scores ALL owned pills itself,
  -- leaving the full candidate list in state.defend_breakdown every replan
  -- (including replans where nothing is defendable), so the cell always
  -- lists every owned pill with its score or reject reason.
  do
    local db = state.defend_breakdown
    if db and db.rows and #db.rows > 0 then
      local age = now - (db.tick or now)
      for _, r in ipairs(db.rows) do r.stale = age end
      by_pool[2] = db.rows
    end
  end

  -- Build a normal pool section. Used for indexes 1..9 and the
  -- offensive_build (11) / wait_for_lgm (12) strips below the main grid.
  local function build_section(idx)
    local pname = POOL_NAMES[idx] or ("p"..idx)
    local pw = (phase_weights and phase_weights[pname]) or 1.0  -- PHASE_WEIGHTS is name-keyed, not idx-keyed
    -- Suicider surcharge shares the phase weight's role here: it is a
    -- selection-layer multiplier on this whole pool, so it belongs in
    -- `weighted` (and therefore in the row ordering). 1.0 for non-suiciders.
    local sui = K.sui[pname] or 1.0
    local pwx = pw * sui * (K.rfm[pname] or 1.0)
                   * (K.fcm[pname] or 1.0)
    -- Sort a COPY. by_pool[2/8/13/14] are the brain's own lists
    -- (state.defend_breakdown.rows, pool_cache[n].cands); sorting them in
    -- place reordered them only in recorded games.
    local rows_raw = {}
    for i, r in ipairs(by_pool[idx] or {}) do rows_raw[i] = r end
    table.sort(rows_raw, function(a, b)
      local ac = (a.cost >= 0) and a.cost * pwx or math.huge
      local bc = (b.cost >= 0) and b.cost * pwx or math.huge
      return ac < bc
    end)
    local rows = {}
    for i, r in ipairs(rows_raw) do
      rows[i] = {
        id = r.id, mx = r.mx, my = r.my,
        cost = r.cost,
        weighted = (r.cost >= 0) and (r.cost * pwx) or -1,
        is_winner = false,   -- assigned below from pool_cache (the real winner)
        active_goal = (active_pool == idx
                       and ((active_id and active_id >= 0 and active_id == r.id)
                            or (active_mx ~= nil and r.mx == active_mx and r.my == active_my))),
        stale = r.stale,
        formula = (sui ~= 1.0 and r.formula)
                  and (r.formula .. string.format(" * suicider{%.1f}", sui))
                  or r.formula,
        reject = r.reject,
        reject_remaining = r.reject_remaining,
        stealing = r.stealing or false,
        ally_by = r.ally_by,
        imminent = r.imminent or false,
        blitz = (pname == "attack_pill") and is_blitz_pill(r.id) or false,
      }
    end
    -- Winner = the pool's ACTUAL selected candidate, not merely the raw-cheapest
    -- rolling row — otherwise a sibling that's raw-cheaper but loses on
    -- penalties / current-goal commitment takes the WINNERS slot and the real
    -- (active) goal vanishes from the strip. Priority:
    --   1. If this is the ACTIVE goal's pool, use the active-goal row. The
    --      committed goal is sticky (commitment discount) and may not be the
    --      pool's raw-cheapest or even the current pool_cache winner, so this
    --      guarantees it is always present in WINNERS.
    --   2. Else the pool_cache winner (post-penalty selection) matched to a row.
    --   3. Else the raw-cheapest (rows[1]).
    local win = nil
    if active_pool == idx then
      for _, rr in ipairs(rows) do
        if rr.active_goal then win = rr; break end
      end
    end
    if not win then
      local pcw = pc[idx]
      if pcw and pcw.goal then
        local gid = pcw.goal.target_id
        local gmx, gmy = pcw.goal.mx, pcw.goal.my
        for _, rr in ipairs(rows) do
          if (gid and gid >= 0 and rr.id == gid)
             or (gmx and rr.mx == gmx and rr.my == gmy) then
            win = rr; break
          end
        end
      end
    end
    if not win then win = rows[1] end
    if win and win.cost >= 0 and win.cost < 1e29 and not win.reject then
      win.is_winner = true
    end
    -- BASE POOLS: cap the rendered rows.  A 16-base map otherwise buries the
    -- interesting bases (the ones a shell or two from falling, which now sort to
    -- the top on the armour-aware markup) under a wall of full-armour rows.
    -- Rows are already sorted cheapest-first; keep the top BASE_PANEL_MAX and
    -- never drop the winner / active-goal row.
    if (idx == 3 or idx == 7) and #rows > (C.BASE_PANEL_MAX or 6) then
      local keep = {}
      for i = 1, (C.BASE_PANEL_MAX or 6) do keep[i] = rows[i] end
      if win then
        local present = false
        for i = 1, #keep do if keep[i] == win then present = true break end end
        if not present then keep[#keep + 1] = win end
      end
      rows = keep
    end
    local winner_id = (win and win.is_winner) and win.id or -1
    return {
      idx = idx, name = pname, weight = pw, winner_id = winner_id,
      layout_cell = LAYOUT_CELL[idx], rows = rows,
    }, win
  end

  -- Group by-pool results with 'imminent' awareness for the winners section.
  -- The by_pool data from eval_queue is augmented with results from finalize_pools
  -- (which calculates imminent floor costs).
  for idx = 1, 9 do
    local winner_data = pc[idx]
    if winner_data and winner_data.imminent then
      local rows = by_pool[idx] or {}
      for _, r in ipairs(rows) do
        if r.id == winner_data.goal.target_id then
          r.imminent = true
          break
        end
      end
    end
  end

  -- Fetch the post-penalty competition results from the last goal_selection
  -- pass so we can show why a winner was or wasn't picked (switch penalties,
  -- commitment, oscillation history).
  local comp = {}
  for _, gc in ipairs(state.goal_competition or {}) do
    local k = string.format("%s@%d,%d", gc.kind, gc.mx or 0, gc.my or 0)
    comp[k] = gc
  end

  local sections = {}
  local winners = {}
  -- Iterate 1..9 (main grid pools) plus 13 (kill_lgm strip), so the
  -- WINNERS cross-pool table includes a kill_lgm row whenever an LGM
  -- is in view and pool 13 holds a candidate.
  for idx = 1, 9 do
    local sec, w = build_section(idx)
    sections[#sections + 1] = sec
    if w and w.cost >= 0 and w.cost < 1e29 and not w.reject then
      -- Cross-pool WINNERS row carries src_pool so the renderer can
      -- color it with its origin pool's hue.
      local pname = POOL_NAMES[idx] or ("p"..idx)
      -- PHASE_WEIGHTS is keyed by pool NAME (matching goal_selection at the
      -- base computation), NOT by numeric pool index — looking it up by idx
      -- always missed and fell back to 1.0, so the row showed "@1.00" while
      -- the base already had the real weight (e.g. opening attack_pill 3.0)
      -- baked in. Use the name so the displayed multiplier matches reality.
      local pw = (phase_weights and phase_weights[pname]) or 1.0

      -- Look up the actual post-penalty total used by goal_selection.
      -- Goal kinds often differ from UI pool names (e.g. refuel_at_base vs refuel).
      local k = string.format("%s@%d,%d", pname, w.mx or 0, w.my or 0)
      local gc = comp[k]

      -- Fallback: try mapping pname to goal kind
      if not gc then
        local kind_map = {
          refuel = "refuel_at_base",
          place_strategic = "place_pill_strategic",
        }
        local alt_k = string.format("%s@%d,%d", kind_map[pname] or "??", w.mx or 0, w.my or 0)
        gc = comp[alt_k]
      end

      -- Second fallback: try fuzzy kind match (if mx/my match)
      if not gc then
        -- Walk comp in sorted key order. Several goal kinds can sit on the same
        -- tile and loosely substring-match ("refuel" vs "refuel_at_base"), so
        -- more than one entry can match and `break` takes whoever comes first.
        -- comp is string-keyed ("<kind>@<mx>,<my>"), so raw pairs() order is a
        -- per-process hash order. Panel display only — no gameplay effect — but
        -- it kept the pool breakdown from matching between paired replays.
        local _cks = {}
        for _ck in pairs(comp) do _cks[#_cks + 1] = _ck end
        table.sort(_cks)
        for _, _ck in ipairs(_cks) do
          local entry = comp[_ck]
          if (entry.mx == w.mx and entry.my == w.my) then
            -- Check if entry.kind "contains" pname or vice-versa
            if (entry.kind and entry.kind:find(pname)) or pname:find(entry.kind or "??") then
              gc = entry; break
            end
          end
        end
      end

      local final_cost = gc and gc.total or w.weighted
      local penalty = gc and gc.penalty or 0   -- additive penalties BEFORE wsim
      local wsim_add = gc and gc.wsim_add or 0  -- wsim is tracked separately

      -- gc.base is the phase-weighted cost actually used by goal_selection
      -- (may differ from w.cost*pw for pools with extra adjustments like refuel).
      -- Fall back to w.weighted when no gc match was found.
      local base_cost = gc and gc.base or w.weighted
      -- gc.base is raw_pool_cost * phase_weight. w.cost is the raw pre-phase
      -- cost shown in the per-pool section / detail popup.

      -- gc.penalty = total - base, so it holds EVERY step after the phase
      -- weight: the whole-cost multipliers (influence, suicider, focus,
      -- refuel, behind-team, fight), switch + commitment, the history
      -- penalty and the builder-less multiplier. hist used to be "whatever
      -- is left" after switch / commit / ally, so the influence x2.0 (and
      -- any other multiplier) showed up inside HT. Each part is now its own
      -- number, in the order goal_selection applies it:
      --   base x inf x suicider x focus x refuel x ahead x fight
      --     + switch + commit + hist, then x nobuild
      -- A multiplier's part is the amount it added: running cost x (m - 1).
      -- ally_claimed is NOT in penalty: step_eval_queue adds it to the pool
      -- cost, so it is already inside raw (the row says "AC .. in raw").
      -- wsim is not inside gc.penalty either (captured before wsim runs).
      local switch_flat = gc and gc.switch_flat or 0
      local commit_val  = gc and gc.commit_val  or 0
      local ally_pen    = gc and gc.ally_claimed_pen or 0
      local ally_by     = gc and gc.ally_claimed_by
      local hist_pen    = gc and gc.hist_pen or 0
      local mult_parts  = {}   -- { abbr, v = value added, run = running cost, m, name }
      do
        local run = base_cost
        local function madd(m, abbr, name)
          if m and m ~= 1.0 then
            mult_parts[#mult_parts + 1] = { abbr = abbr, v = run * (m - 1),
                                            run = run, m = m, name = name }
            run = run * m
          end
        end
        if gc then
          madd(gc.inf_mult, "INF", "territory influence")
          madd(gc.suicider_mult, "SU", "pill_suicider")
          madd(gc.focus_mult, "FO", "team focus")
          madd(gc.refuel_mult, "RF", "refuel group")
          madd(gc.ahead_mult, "AH", "behind-team attack")
          madd(gc.fight_mult, "FT", "scenario fight")
        end
      end
      local mult_sum = 0
      for _, mp in ipairs(mult_parts) do mult_sum = mult_sum + mp.v end
      local nb_m   = (gc and gc.nobuild_mult) or 1.0
      local nb_add = (gc and gc.pre_nb and nb_m ~= 1.0) and gc.pre_nb * (nb_m - 1) or 0
      -- Anything in penalty the named parts do not explain (0 when the
      -- parts are complete; shown as OT so the row still adds up).
      local other_pen = gc and (penalty - mult_sum - switch_flat - commit_val
                                - hist_pen - nb_add) or 0
      if math.abs(other_pen) < 0.5 then other_pen = 0 end
      -- Ticks: when the pool row was priced (the cost_cache entry) and when
      -- goal_selection made this competition record.
      local priced_t = (w.stale and w.stale >= 0) and (now - w.stale) or nil
      local picked_t = gc and gc.tick
      local tick_tag = string.format(" [priced t=%s, picked t=%s]",
        priced_t and tostring(priced_t) or "?", picked_t and tostring(picked_t) or "?")

      -- Build a rich formula for the detail popup
      local detail_formula = string.format("%s(x%.2f): %s", pname,
        (gc and gc.phase_weight) or pw, w.formula)
      local detail_map = {}

      detail_map[#detail_map + 1] = string.format("base:%.0f (raw=%.0f x pw=%.1f, %s phase)",
        base_cost, w.cost or 0, pw,
        state.phase or "unknown")

      -- Territory-influence scale (x0.5 in our half, x2 in theirs), applied at
      -- selection right before the suicider surcharge. It was stashed on the
      -- competition record but never rendered, which left every row in scaled
      -- territory unable to reproduce its own total from the chips.
      local inf_mult_d = (gc and gc.inf_mult) or 1.0
      if inf_mult_d ~= 1.0 then
        detail_formula = string.format("%s * inf{x%.1f}", detail_formula, inf_mult_d)
        detail_map[#detail_map + 1] = string.format(
          "inf:territory influence at the GOAL TILE (%d,%d) is %s -> x%.1f. cpf.influence_at < -50 (their half) doubles the bid, > 50 (our half) halves it, in between is x1. Skipped in the opening phase and for refuel_at_base / defend_pill / take_cover / repair_pill (with the repair fix on), for the place follow-through row, and for the kill_me delivery row.",
          w.mx or -1, w.my or -1,
          (inf_mult_d > 1) and "hostile" or "friendly", inf_mult_d)
      end

      -- LOADED, BUILDER-LESS surcharge. The LAST multiplier applied to the
      -- row -- after influence, after the suicider/refuel shaping, after
      -- hysteresis and the history penalty -- so it multiplies the total the
      -- other chips add up to, and the row reads
      --   (base x pw x inf x suicider x refuelmult + penalties) x nobuild.
      local nobuild_mult = (gc and gc.nobuild_mult) or 1.0
      if nobuild_mult ~= 1.0 then
        detail_formula = string.format("%s * nobuild{x%.0f}", detail_formula, nobuild_mult)
        detail_map[#detail_map + 1] = string.format(
          "nobuild:LOADED, BUILDER-LESS -- %s. ATTACK_NO_BUILDER_MULT multiplies the FINAL competed cost of attack_tank / attack_pill / attack_base / defend_pill by %.0f while the state holds, so a fight has to be the ONLY thing on offer before a tank carrying an unplaceable stack takes it. Not a reject: with an empty pool the bot can still shoot back. The kill_me delivery row is exempt.",
          M.loaded_no_lgm_label(state), nobuild_mult)
      end

      if gc and gc.escape_no_hyst then
        detail_map[#detail_map + 1] = string.format(
          "hyst:none:no_lgm -- ESCAPE_NO_BUILDER_SKIP_HYST. %s, so this escape row (flee_to_base, or a take_cover that fired on its haul / panic trigger) pays switch(0) + commit(0) to ENTER and skips the multiplicative stickiness bar. Asymmetric: once it IS the goal, every other row pays the normal switch + commitment to displace it.",
          M.loaded_no_lgm_label(state))
      end

      -- Pillbox-suicider surcharge, applied at selection alongside the phase
      -- weight / influence scale. Rendered whenever it isn't 1.0 so the row's
      -- numbers still reconcile (base x pw x inf x suicider + penalties = total).
      local suicider_mult = (gc and gc.suicider_mult) or 1.0
      if suicider_mult ~= 1.0 then
        detail_formula = string.format("%s * suicider{%.1f, %s}", detail_formula, suicider_mult,
                                       tostring(state.suicider_src or "?"))
        local _sui_bs = state.blitz_suicider
        detail_map[#detail_map + 1] = string.format(
          "suicider:pill_suicider role (source=%s%s) -> this goal kind (%s) costs x%.1f (attack_pill and the refuel group are exempt; defend_pill x%.1f, everything else x%.1f)",
          tostring(state.suicider_src or "?"),
          _sui_bs and string.format(", TEMPORARY for blitz pill #%s designated by p%s, %s",
                                    tostring(_sui_bs.pill), tostring(_sui_bs.by),
                                    (_sui_bs.why == "contested")
                                      and "contested{yes -- a hostile tank was within BLITZ_CONTESTED_RANGE of that pill, so BLITZ_CONTESTED_SUICIDERS of the blitz were designated}"
                                      or  "contested{no -- BLITZ_MIN_SUICIDERS quota top-up}") or "",
          tostring(w.kind or (gc and gc.kind) or pname), suicider_mult,
          C.PILL_SUICIDER_DEFEND_MULT or 1.0, C.PILL_SUICIDER_OTHER_MULT or 1.0)
      end

      -- Refuel-group multiplier, same layer as the suicider surcharge, so the
      -- row's numbers still reconcile (base x pw x inf x suicider x refuelmult
      -- + penalties = total).
      local focus_mult_d = (gc and gc.focus_mult) or 1.0
      if focus_mult_d ~= 1.0 then
        detail_formula = detail_formula .. (" |" .. M.focus_label(state._focus))
        detail_map[#detail_map + 1] = string.format(
          "focus:the team is focused on %s, so every %s goal costs x%.1f -- FOCUS_OTHER_COST_MULT, set by the chat command \"focus %s\" and cleared by \"focus off\"",
          tostring(state._focus), (state._focus == "bases") and "PILL" or "BASE",
          focus_mult_d, tostring(state._focus))
      end

      local refuel_mult_d = (gc and gc.refuel_mult) or 1.0
      if refuel_mult_d ~= 1.0 then
        detail_formula = string.format("%s * %s", detail_formula, M.refuel_mult_label(K.refuel_mult, K.refuel_src))
        detail_map[#detail_map + 1] = string.format(
          "refuelmult:the refuel GOAL_GROUP (refuel_at_base + flee_to_base) costs x%.2f for this bot (%s) -- REFUEL_COST_MULT, per-bot overridable with the \"refuel=X\" init token",
          refuel_mult_d, K.refuel_src)
      end

      -- Behind-team attack surcharge, same layer as suicider/refuel, so the row's
      -- numbers still reconcile (base x pw x inf x suicider x refuelmult x ahead
      -- + penalties = total).
      local ahead_mult_d = (gc and gc.ahead_mult) or 1.0
      if ahead_mult_d ~= 1.0 then
        detail_formula = string.format("%s * ahead{x%.2f}", detail_formula, ahead_mult_d)
        detail_map[#detail_map + 1] = string.format(
          "ahead:team NOT ahead (state.team_ahead false: pill lead %.2f < AHEAD_PILL_FRAC %.2f AND base lead %.2f < AHEAD_BASE_FRAC %.2f) -> this offensive row (attack_pill / attack_base / attack_tank) costs x%.2f (BEHIND_ATTACK_MULT), so the bot picks fewer fights while behind",
          state.strength or 0, C.AHEAD_PILL_FRAC, state.base_strength or 0, C.AHEAD_BASE_FRAC, ahead_mult_d)
      end

      -- Scenario fight multiplier, same layer again.
      local fight_mult_d = (gc and gc.fight_mult) or 1.0
      if fight_mult_d ~= 1.0 then
        detail_formula = string.format("%s * fight{x%.2f}", detail_formula, fight_mult_d)
        detail_map[#detail_map + 1] = string.format(
          "fight:a script set FIGHT_COST_MULT (cfg=) -> this attack_tank / kill_lgm / take_cover row costs x%.2f (not on an ordered row, the kill-me row, a reject sentinel, or at armour <= ARMOUR_CRITICAL %d)",
          fight_mult_d, C.ARMOUR_CRITICAL)
      end

      if w.imminent then
        detail_map[#detail_map + 1] = string.format("imminent:cost_forced_to_floor(%.0f) because neutral and close", C.IMMINENT_CAPTURE_FLOOR)
      end

      for _, mp in ipairs(mult_parts) do
        detail_map[#detail_map + 1] = string.format(
          "%s:%s x%.2f adds %.0f = running cost %.0f x (%.2f - 1)",
          mp.abbr, mp.name, mp.m, mp.v, mp.run, mp.m)
      end
      if nb_add ~= 0 then
        detail_map[#detail_map + 1] = string.format(
          "NB:builder-less x%.0f adds %.0f = cost before it %.0f x (%.0f - 1)",
          nb_m, nb_add, gc.pre_nb, nb_m)
      end
      if other_pen ~= 0 then
        detail_map[#detail_map + 1] = string.format(
          "OT:%.0f of the penalty that no named part explains (penalty %.0f - multipliers %.0f - switch %.0f - commit %.0f - hist %.0f - nobuild %.0f)",
          other_pen, penalty, mult_sum, switch_flat, commit_val, hist_pen, nb_add)
      end
      detail_map[#detail_map + 1] = string.format(
        "ticks:pool row priced at t=%s (cost_cache entry), WINNERS record made at t=%s (goal_selection), panel built at t=%d",
        priced_t and tostring(priced_t) or "?", picked_t and tostring(picked_t) or "?", now)

      if (penalty ~= 0 or wsim_add > 0) then
        local parts = {}
        if switch_flat > 0 then
          parts[#parts + 1] = string.format("hyst{%.0f}", switch_flat)
        end
        if commit_val > 0 then
          parts[#parts + 1] = string.format("hyst{%.0f}", commit_val)
        end
        if hist_pen > 0 then
          parts[#parts + 1] = string.format("hist{%.0f}", hist_pen)
        end
        if other_pen ~= 0 then
          parts[#parts + 1] = string.format("other{%.0f}", other_pen)
        end
        if wsim_add > 0 then
          parts[#parts + 1] = string.format("wsim{%.0f}", wsim_add)
        end

        if #parts > 0 then
          detail_formula = string.format("%s + %s = %.0f", detail_formula, table.concat(parts, "+"), final_cost)
        end

        if switch_flat > 0 or commit_val > 0 then
          local hparts = {}
          if switch_flat > 0 then hparts[#hparts + 1] = string.format("switch(%.0f)", switch_flat) end
          if commit_val  > 0 then hparts[#hparts + 1] = string.format("commit(%.0f)", commit_val)  end
          detail_map[#detail_map + 1] = "hyst:" .. table.concat(hparts, " + ")
        end
        if hist_pen > 0 then
          detail_map[#detail_map + 1] = string.format("hist:recurrence_penalty(%.0f)", hist_pen)
        end
        if other_pen ~= 0 then
          detail_map[#detail_map + 1] = string.format("other:see OT (%.0f)", other_pen)
        end
        if wsim_add > 0 then
          detail_map[#detail_map + 1] = string.format("wsim:damage_prediction(%.0f)", wsim_add)
        end
      end

      if ally_pen > 0 then
        detail_map[#detail_map + 1] = string.format("ally_claimed:p%s holds same goal(+%.0f, already inside the raw pool cost)",
                                                     tostring(ally_by), ally_pen)
      end

      if gc and gc.hyst then
        detail_formula = string.format("%s (must be < cur*%.1f)", detail_formula, C.GOAL_SWITCH_RATIO)
      end

      -- Row second line: base + penalties = total (all numbers must balance).
      -- Use base_cost (from gc.base) so the equation holds even for pools
      -- like refuel where goal_selection adds constants before phase-weighting.
      local phase_abbrev = ({
        opening = "OP", early = "EA", middle = "MD", late = "LT",
        endgame = "EG", unknown = "??"
      })[(state.phase or "unknown")] or (state.phase or "??"):sub(1,2):upper()
      -- Prefix with pool kind so the WINNERS row tells you WHAT goal
      -- the cost belongs to at a glance (was: "34 MD@1.00" → now:
      -- "attack_tank 34 MD@1.00"). Most-recently asked for on
      -- attack_tank winners but applies uniformly to all main pools.
      -- Show the TRUE raw cost (w.cost, same number the per-pool section and
      -- detail popup show) times the phase multiplier, so the line reads
      -- "raw x PHASE@pw +penalties = total" and every number reconciles with
      -- the other views.
      local raw_base = w.cost or (pw ~= 0 and base_cost / pw) or base_cost
      -- Reconciling multipliers: show the APPLIED phase weight
      -- (distance-attenuated, recorded by goal_selection) and the
      -- influence scale — the raw table weight (e.g. "MD@0.50") never
      -- balanced against =total (defend_pill 100 x MD@0.50 +38 "=75"
      -- actually meant 100 x 0.74 x inf0.5 + 38 = 75).
      local eff_pw   = (gc and gc.phase_weight) or pw
      -- Every multiplier after the phase weight is shown as the amount it
      -- added (e.g. "+173 INF" for x2.0 on 173), in the order applied, so
      -- the row reads raw x PHASE@pw +parts = total.
      local row_summary = string.format("%s %.0f x %s@%.2f",
        pname, raw_base, phase_abbrev, eff_pw)
      if ally_pen > 0 then
        row_summary = row_summary .. string.format(" (AC %.0f in raw)", ally_pen)
      end

      if penalty ~= 0 or wsim_add > 0 or w.imminent then
        local parts = {}
        if w.imminent then
          parts[#parts + 1] = "IM"
        end
        for _, mp in ipairs(mult_parts) do
          parts[#parts + 1] = string.format("%+.0f %s", mp.v, mp.abbr)
        end
        if switch_flat > 0 then
          parts[#parts + 1] = string.format("+%.0f SW", switch_flat)
        end
        if commit_val > 0 then
          parts[#parts + 1] = string.format("+%.0f CM", commit_val)
        end
        if hist_pen > 0 then
          parts[#parts + 1] = string.format("+%.0f HT", hist_pen)
        end
        if nb_add ~= 0 then
          parts[#parts + 1] = string.format("%+.0f NB", nb_add)
        end
        if other_pen ~= 0 then
          parts[#parts + 1] = string.format("%+.0f OT", other_pen)
        end
        if wsim_add > 0 then
          parts[#parts + 1] = string.format("+%.0f WS", wsim_add)
        end
        if #parts > 0 then
          row_summary = row_summary .. " " .. table.concat(parts, " ")
        end
        row_summary = row_summary .. " =" .. string.format("%.0f", final_cost)
      end
      row_summary = row_summary .. tick_tag

      -- Final formula format: "RowSummary !! FullDisplayFormula || TermMapping"
      local final_formula = string.format("%s !! %s", row_summary, detail_formula)
      if #detail_map > 0 then
        final_formula = final_formula .. " || " .. table.concat(detail_map, "|")
      end

      -- Generate a unique synthetic ID for the WINNERS pool to avoid collisions
      -- between different source pools (e.g. attack_pill #0 and attack_base #0).
      -- Use src_pool in the high bits.
      local synthetic_id = bit.bor((bit.lshift(idx, 16)), (w.id or 0))

      winners[#winners + 1] = {
        id = synthetic_id, src_pool = idx,
        mx = w.mx, my = w.my,
        -- cost   = RAW per-pool cost (pre-phase, pre-loc_adj) so the
        --          detail popup's "Cost" matches the per-pool row.
        -- weighted = FINAL cost goal_selection used (post-phase weight,
        --            post-loc_adj, post-penalties).  Same value that
        --            ranks the WINNERS row.  Was set to final_cost for
        --            both fields, making "Cost: N    Weighted: N"
        --            uninformative when the two genuinely differ.
        cost = w.cost, weighted = final_cost,
        is_winner = false,
        active_goal = w.active_goal,
        stale = w.stale,
        formula = final_formula,
        blitz = (pname == "attack_pill") and is_blitz_pill(w.id) or false,
      }
    end
  end

  -- Strip pools (11/12/13) — injected directly into pool_cache by
  -- their respective evaluators, not routed through eval_queue or
  -- goal_competition.  We synthesize a WINNERS row from pool_cache so
  -- they compete on the same cost axis as the regular pools.  Note:
  -- 11 (offensive_build) is reserved but currently unused — the entry will
  -- show up here as soon as something writes pool_cache[11].
  --
  -- Strip rows get the same "<pool_name> <cost> <phase>@<pw>" row
  -- summary as main pools so the WINNERS column is uniformly readable.
  -- Their detail formula comes from cost_cache (richer breakdown) when
  -- available, falling back to sw.desc (the short tagline).
  for _, idx in ipairs({10, 11, 12, 13, 14, 15}) do
    local sw = pc[idx]
    if sw and sw.goal and sw.cost and sw.cost >= 0 and sw.cost < 1e29 then
      local synthetic_id = bit.bor((bit.lshift(idx, 16)), (sw.goal.target_id or 0))
      local pname = POOL_NAMES[idx] or ("p"..idx)
      local pw   = (phase_weights and phase_weights[pname]) or 1.0  -- name-keyed, not idx
      local phase_abbrev = ({
        opening = "OP", early = "EA", middle = "MD", late = "LT",
        endgame = "EG", unknown = "??"
      })[(state.phase or "unknown")] or (state.phase or "??"):sub(1,2):upper()
      local raw_base = (pw and pw ~= 0) and (sw.cost / pw) or sw.cost
      local row_summary = string.format("%s %.0f x %s@%.2f",
        pname, raw_base, phase_abbrev, pw)
      -- Suicider surcharge (reposition / offensive_build / wait_for_lgm / kill_lgm
      -- are all non-exempt kinds, so these strips DO pay it).
      local _strip_sui = K.sui[pname] or 1.0
      if _strip_sui ~= 1.0 then
        row_summary = row_summary .. string.format(" x suicider{%.1f}", _strip_sui)
      end
      local _strip_fcm = K.fcm[pname] or 1.0
      if _strip_fcm ~= 1.0 then row_summary = row_summary .. (" |" .. M.focus_label(state._focus)) end
      -- Prefer the cost_cache formula (full base + breakdown + detail
      -- map) over sw.desc (one-line tagline).  Both kill_lgm and
      -- wait_for_lgm stamp cost_cache under "<idx>:<target_id>" so we
      -- look that up first.
      local cc_key = string.format("%d:%s", idx, sw.goal.target_id or 0)
      local cc = state.cost_cache and state.cost_cache[cc_key]
      local detail = (cc and get_formula(cc)) or sw.desc or ""
      local final_formula = string.format("%s !! %s", row_summary, detail)
      winners[#winners + 1] = {
        id = synthetic_id, src_pool = idx,
        mx = sw.goal.mx, my = sw.goal.my,
        cost = sw.cost, weighted = sw.cost,
        is_winner = false,
        reject = sw._reject,
        active_goal = (active_pool == idx
                       and ((active_id and active_id >= 0 and active_id == (sw.goal.target_id or -1))
                            or (active_mx ~= nil and sw.goal.mx == active_mx and sw.goal.my == active_my))),
        stale = 0,
        formula = final_formula,
      }
    end
  end

  -- Cell 10 = cross-pool WINNERS table, ranked ascending by FINAL cost
  -- (`weighted` = the yellow "cost= C" the renderer shows = post-phase-weight,
  -- post-penalty total). Sorting by the raw per-pool `cost` instead would put
  -- rows out of order vs. the number displayed on them.
  table.sort(winners, function(a, b) return (a.weighted or a.cost) < (b.weighted or b.cost) end)
  -- The winner is the cheapest NON-rejected row (rejected rows, e.g. a
  -- reposition with no team pills, are shown for visibility but can't win).
  local first_winner = nil
  for _, w in ipairs(winners) do
    if not w.reject then w.is_winner = true; first_winner = w; break end
  end
  -- Warmup: while the pools are still warming (fewer than WARMUP_MIN_REAL_GOALS
  -- candidates costed below the wall cost) surface a reject row at the TOP of
  -- WINNERS so the panel shows the warmup state — even if the bot has grabbed a
  -- single reachable goal (e.g. attack_tank against the tank that just killed
  -- it) and so isn't on the explore fallback this tick. Shown whenever not
  -- warm, regardless of current goal; marked active only when actually
  -- exploring. warm_ready latches once warm so this clears in steady state.
  -- The same test as warm_ready, but read-only (M.warm_count): this report
  -- must not set the latch -- see M.warm_count.
  local warm_n, warm_need = nil, nil
  if state.goal and not state._warm_ready and not state._eval_swept then
    warm_n, warm_need = K.warm_n, K.warm_need
  end
  if warm_n and warm_n < warm_need then
    local need = warm_need
    local n = warm_n
    local gmx, gmy = state.goal.mx or 0, state.goal.my or 0
    table.insert(winners, 1, {
      id = 0, src_pool = 0,
      mx = gmx, my = gmy,
      cost = -1e9, weighted = -1e9,   -- renders as "cost= ?"
      is_winner = false,
      active_goal = (state.goal.kind == "explore"),
      reject = "warmup", reject_remaining = math.max(0, need - n),
      formula = string.format(
        "REJECT warmup %d/%d @(%d,%d)||reject:warmup — only %d of %d goals costed below the wall cost (9999); pools still warming from this position",
        n, need, gmx, gmy, n, need),
    })
  end
  sections[#sections + 1] = {
    idx = 10, name = "WINNERS", weight = 1.0,
    winner_id = (first_winner and first_winner.id) or -1,
    layout_cell = LAYOUT_CELL[10], rows = winners,
  }

  -- Strips below the grid: offensive_build (11), wait_for_lgm (12),
  -- kill_lgm (13), take_cover (14).  Only emit the section if the brain actually
  -- produced candidates for that pool this tick — keeps the renderer
  -- from drawing empty placeholders when the brain doesn't use the slot.
  -- kill_lgm doesn't go through eval_queue (it's injected directly into
  -- pool_cache from perception); seed by_pool[13] from pool_cache[13]
  -- here so build_section finds rows.
  if pc[13] and pc[13].cands then by_pool[13] = pc[13].cands end
  -- take_cover (14) is the same shape: injected straight into pool_cache by
  -- eval_take_cover with the full candidate scan in `cands`.
  if pc[14] and pc[14].cands then by_pool[14] = pc[14].cands end
  for _, idx in ipairs({11, 12, 13, 14}) do
    if by_pool[idx] and #by_pool[idx] > 0 then
      sections[#sections + 1] = (build_section(idx))
    end
  end

  -- BUILDER (15) — the builder pool's own strip. NOT a new pool and not part
  -- of the eval_queue: this arbiter spends the MAN, not the tank, so it has no
  -- goal, no phase weight and no WINNERS entry. It is built entirely by
  -- builder_pool.panel_section from the record update() left on state, which
  -- is why the numbers on it are the same numbers the dispatch used.
  -- Numbering starts above take_cover (14) so every recording made before
  -- this existed still loads: nothing that was 1..14 has moved.
  --
  -- The section carries `hdr` (owner / eligibility / active job) on top of the
  -- usual rows; the renderer prints those lines above the row list. Rows use
  -- the shared cost + short||long formula renderer like every other pool row.
  do
    local bsec = M.bp_panel_section(state)
    if bsec then sections[#sections + 1] = bsec end
  end

  return json.encode({
    -- Bump schema_version when the shape changes in a way that
    -- breaks existing renderers / recorded snapshots. Renderers
    -- check this and surface a warning for unknown versions
    -- instead of silently misparsing.
    schema_version = 1,
    phase = state.phase or "?",
    tick = now,
    replan_left = replan_left,
    bot = state.player_number or 0,
    debug_session = K.debug_session or "",
    sections = sections,
  })
end

-- =========================================================================
-- Exports for the brain modules that print the same text.
-- =========================================================================
M.POOL_NAMES        = POOL_NAMES
M.KIND_TO_POOL      = KIND_TO_POOL
M.POOL_NAME_TO_KIND = POOL_NAME_TO_KIND
M.get_formula       = get_formula
M.mine_chip         = mine_chip
M.refuel_low_stock_chip = refuel_low_stock_chip

-- =========================================================================
-- RAW CAPTURE FORMAT
--
-- goals.capture_pool_raw builds one table per bot per tick:
--   { v = 1, S = <brain tables>, K = <computed values>, C = <constants> }
-- and the brain returns it as   RAW_TAG .. string.buffer.encode(cap).
--
-- The recording pipeline carries the block as a NUL-terminated C string, so
-- the C side (poolRenderDupBrainString in pool_render_host.c) escapes the
-- two bytes \0 and \1 as \1\2 and \1\3 when it copies the block out of the
-- brain. The escape is in C because a Lua gsub over the block cost about
-- 1 ms per bot per tick, thirty times the encode. unpack() undoes it. A
-- block that starts with "{" is the old JSON form; a block that starts with
-- RAW_TAG is this one.
--
-- string.buffer is LuaJIT's. Without it (PUC Lua on the WASM build) the brain
-- keeps sending JSON (goals.get_pool_breakdown_raw falls back).
-- =========================================================================
M.RAW_TAG = "WBPR1:"

local sbuf
do
  local ok, b = pcall(require, "string.buffer")
  sbuf = ok and b or nil
end
M.HAVE_RAW = (sbuf ~= nil)

local ESC   = { ["\0"] = "\1\2", ["\1"] = "\1\3" }
local UNESC = { ["\2"] = "\0",   ["\3"] = "\1" }

-- The block the brain returns: not escaped (the C side escapes it).
function M.pack(cap)
  return M.RAW_TAG .. sbuf.encode(cap)
end

-- The escape the C side applies, for a test that has a block from pack()
-- and wants the recorded form.
function M.escape(blob)
  return M.RAW_TAG .. blob:sub(#M.RAW_TAG + 1):gsub("[%z\1]", ESC)
end

function M.is_raw(blob)
  return type(blob) == "string" and blob:sub(1, #M.RAW_TAG) == M.RAW_TAG
end

-- unpack(blob): the capture table from a RECORDED (escaped) block.
function M.unpack(blob)
  if not sbuf then error("pool_render: string.buffer is not available", 0) end
  local body = blob:sub(#M.RAW_TAG + 1):gsub("\1([\2\3])", UNESC)
  return sbuf.decode(body)
end

-- A decoded copy of a capture, for the brain's own JSON path: the render
-- writes into the tables it is given, so it must never get the live ones.
function M.round_trip(cap)
  return sbuf.decode(sbuf.encode(cap))
end

-- Deep copy, for a Lua without string.buffer: the render writes into the
-- tables it is given, so it must never get the live ones.
function M.copy(t, seen)
  if type(t) ~= "table" then return t end
  seen = seen or {}
  if seen[t] then return seen[t] end
  local o = {}
  seen[t] = o
  for k, v in pairs(t) do o[M.copy(k, seen)] = M.copy(v, seen) end
  return o
end

-- Optional check used by the equality test: every constant the render reads
-- must be in the capture. When set, a read of a name the capture does not
-- hold is recorded in M.missing_c.
M.check_c   = false
M.missing_c = {}

-- render(cap): the JSON string for one decoded capture. Binds the capture's
-- constants and hooks for the length of the call, then restores the live ones.
function M.render(cap)
  local saved_c, saved_h = C, H
  local K = cap.K
  local cc = cap.C or {}
  if M.check_c then
    local missing = M.missing_c
    cc = setmetatable(cc, { __index = function(_, k) missing[k] = true end })
  end
  C = cc
  H = {
    threat_at  = function(mx, my) return (K.threat and K.threat[my * 256 + mx]) or 0 end,
    shells_low = function() return K.shells_low end,
  }
  local ok, res = pcall(render_state, cap.S, K)
  C, H = saved_c, saved_h
  if not ok then error(res, 0) end
  return res
end

-- render_blob(blob): JSON for one recorded pool block. An old JSON block is
-- returned unchanged, so a caller can pass any block it finds.
function M.render_blob(blob)
  if not M.is_raw(blob) then return blob end
  return M.render(M.unpack(blob))
end

-- The constant names this file reads. goals.capture_pool_raw copies these
-- (and only these) from the bot's live constants into every capture.
-- tests/pool_render_names.py checks the list against the C.NAME reads above.
M.C_NAMES = {
  "AHEAD_BASE_FRAC",
  "AHEAD_PILL_FRAC",
  "ALLY_CLAIMED_REFUEL_PENALTY",
  "ALLY_CLAIMED_STEAL_FRAC",
  "ALLY_CLAIMED_STEAL_FRAC_CAPTURE",
  "ANGER_ATTACK_THRESHOLD",
  "ANGER_COST_PER_TICK",
  "ARMOUR_CRITICAL",
  "ARMOUR_LOW",
  "ATTACK_BASE_EXTRA_COST",
  "ATTACK_BASE_FRIENDLY_COVER_FLOOR",
  "ATTACK_BASE_FRIENDLY_COVER_MULT",
  "ATTACK_BASE_THREAT_WEIGHT",
  "ATTACK_PILL_STANDOFF",
  "ATTACK_PILL_UNSAFE_ARMOUR_FLOOR",
  "ATTACK_PILL_UNSAFE_HP_THRESHOLD",
  "BASE_CAPTURE_ARMOUR",
  "BASE_FULL_ARMOUR",
  "BASE_FULL_SHELLS_TO_KILL",
  "BASE_MARKUP_STALE",
  "BASE_PANEL_MAX",
  "BASE_SHELL_DAMAGE",
  "BASE_STEAL_COST",
  "BASE_STEAL_MAX_ARMOUR",
  "BASE_STEAL_MIN_ARMOUR",
  "BASE_STEAL_OBS_STALE",
  "BASE_STEAL_RANGE",
  "BUILDER_POOL_DANGER_W",
  "BUILDER_POOL_FARM_LOW_TREES",
  "BUILDER_POOL_FARM_URGENCY",
  "BUILDER_POOL_FRONT_MAX_TILES",
  "BUILDER_POOL_FRONT_URGENCY",
  "BUILDER_POOL_GRASS_TICKS_PER_TILE",
  "BUILDER_POOL_LEASH",
  "BUILDER_POOL_MIN_SCORE",
  "BUILDER_POOL_PANEL_IDX",
  "BUILDER_POOL_PANEL_ROWS",
  "BUILDER_POOL_REPAIR_HP_W",
  "BUILDER_POOL_REPAIR_TRIP_W",
  "BUILDER_POOL_RETURN_PREDICT",
  "BUILDER_POOL_RETURN_PREDICT_MAX_TICKS",
  "BUILDER_POOL_TOPUP_PER_HP",
  "BUILDER_POOL_TRIP_W",
  "CAPTURE_BASE_EXTRA_COST",
  "CAPTURE_CLUSTER_DIVISOR_MAX",
  "CAPTURE_CLUSTER_GUARD_MAX",
  "CAPTURE_CLUSTER_GUARD_MULT",
  "CAPTURE_CLUSTER_MIN_COST",
  "CAPTURE_CLUSTER_RADIUS",
  "CAPTURE_FREE_PILL_DANGER_FALLOFF",
  "CAPTURE_FREE_PILL_MIN_COST",
  "CAPTURE_FREE_PILL_RANGE_MULT",
  "CAPTURE_FREE_PILL_VALUE",
  "CAPTURE_NO_LGM_DANGER_MULT",
  "CAPTURE_NO_LGM_ROUTE_STRICT",
  "CAPTURE_PILL_BASE_COST",
  "CAPTURE_PILL_DANGER_SCALE",
  "CAPTURE_PILL_DIST_SCALE",
  "CAPTURE_ROUTE_MAX_TILES",
  "CAUTIOUS_MODE_MULT",
  "CONTESTED_BASE_PENALTY",
  "CONTESTED_BASE_RANGE",
  "FOCUS_OTHER_COST_MULT",
  "GOAL_REPLAN_INTERVAL",
  "GOAL_SWITCH_RATIO",
  "IMMINENT_CAPTURE_FLOOR",
  "KILL_ME_SHELL_MARGIN",
  "LGM_BUILD_TIME",
  "LGM_DEAD",
  "LGM_INTANK",
  "LGM_MOVING",
  "MAP_SPEED",
  "PHASE_WEIGHTS",
  "PILLBOX_RANGE_WU",
  "PILLS_MAX_HEALTH",
  "PILL_ANGER_DECAY",
  "PILL_FIRE_RANGE",
  "PILL_RANGE_MAP",
  "PILL_SUICIDER_DEFEND_MULT",
  "PILL_SUICIDER_OTHER_MULT",
  "PING_DEFEND_REPAIR_RANGE",
  "REFUEL_BASE_COST",
  "REFUEL_DANGER_PENALTY",
  "REFUEL_DANGER_WEIGHT",
  "REFUEL_DEFICIT_BONUS",
  "REFUEL_DEPLETION_PENALTY",
  "REFUEL_FULL_COST_MULT",
  "REFUEL_LOW_ARMOUR_MULT",
  "REFUEL_LOW_SHELLS_MULT",
  "REFUEL_MINE_HOARD_NEEDS_SUPPLY",
  "REFUEL_MIN_STOCK",
  "REFUEL_OBS_STALE",
  "REFUEL_PAD_TANK_OFFSET",
  "REFUEL_SHARE_LOCAL_K",
  "REFUEL_SHARE_LOCAL_TILES",
  "REFUEL_SHARE_RATIO_K",
  "REFUEL_SHARE_SCARCITY_CAP",
  "REFUEL_URGENCY_MIN",
  "SEA_COVER_MARGIN_CALM_WU",
  "SEA_COVER_MARGIN_HOT_WU",
  "SEA_PILL_COST_FLOOR",
  "SEA_PILL_COST_MULT",
  "SEA_PILL_ENEMY_TANK_NEAR",
  "SEA_PILL_ENTRANCE_MAX_DIST",
  "SEA_PILL_MIN_SHELLS",
  "SEA_PILL_TREES_TOTAL",
  "SEA_PILL_TREE_LEG_PER_TREE",
  "SEA_TREES_PER_FOREST",
  "SEA_TREES_RADIUS",
  "SHELLS_LOW",
  "SHELL_RESERVE",
  "SPIKE_CARE_TICKS",
  "SPIKE_PEN_SKIP_MAX_SHOTS",
  "SPIKE_PILL_DISCOUNT",
  "SPIKE_REPAIR_LEASH",
  "SPIKE_REPAIR_MIN_MISSING",
  "STALE_PENALTY_PER_TICK",
  "STALE_PENALTY_START",
  "STEAL_YIELD_BLOCK",
  "TANK_COMBAT_ENGAGE_RANGE",
  "TANK_COMBAT_LOS_BASE_COST",
  "TANK_COMBAT_LOS_COST_PER_TILE",
  "TANK_COMBAT_MAX_RANGE",
  "TANK_COMBAT_MIN_SHELLS",
  "TANK_SHELL_DAMAGE",
  "TURTLE_BASE_DOUBLE_TILES",
  "TURTLE_BASE_D_REFRESH_TICKS",
  "TURTLE_BASE_MULT_CAP",
  "WOUNDED_FINISH_DECAY_TICKS",
  "WOUNDED_FINISH_OTHER_PENALTY",
  "WOUNDED_FINISH_THRESHOLD",
}

return M
