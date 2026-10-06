local bit = require('bitcompat')
-- =========================================================================
-- NewAutopilot/squad.lua — squad coordination (Phase 1: pill blitz).
--
-- Slice 1 (this file, so far): deterministic role allocation + broadcast +
-- a right-side roster HUD. No behavior change yet — roles are computed and
-- shown so the rest of the blitz machinery can be layered on top.
--
-- Roles (SQUAD_COORDINATION_PLAN.md §3) are a PURE FUNCTION of the active
-- protocol-bot set (self + allies broadcasting recently), so every bot
-- reaches the same assignment with no central authority.
-- =========================================================================

local C          = require("constants")
local ally_state = require("ally_state")
local cpf        = require("cpathfinder")
local U          = require("util")
local viz        = require("viz")
local orders     = require("orders")
local print2     = require("print2")
local SM         = require("spot_margin")  -- blitz spot LOS margin (2026-09-24)

local M = {}

M.ROLE_COMMANDER = "c"
M.ROLE_SOLDIER   = "s"
M.ROLE_HARASSER  = "h"

-- Substates where a blitzer has ARRIVED at its firing spot (past approach) and
-- is settling its aim or already firing — i.e. "parked and go-able" for the
-- early-GO quorum, exactly equivalent to blitz_wait. Covers both the overwhelm
-- path (blitz_wait) and the full PPT firing sequence (aim → in_range_* →
-- shoot_pill). A tank in any of these is at its spot and will fire on GO without
-- needing to travel, so it counts toward the GO numbers like a blitz_wait tank.
M.BLITZ_READY_SUBS = {
  blitz_wait = true, aim = true,
  in_range_position = true, in_range_aim_pre = true,
  in_range_aim = true, in_range_aim_finetune = true,
  shoot_pill = true,
}

-- A commander's blitz call is OPEN while it's still in a PRE-COMMIT substate —
-- planning, gathering/building the shield, driving in, or rallying. Once it
-- COMMITS to firing (aim / charge / in_range_* / engage / shoot_pill / swerve)
-- the call turns OFF: a partner can no longer join the synchronized take. The
-- call is opened ONCE and stays open across these substates (it does NOT close
-- just because goal._blitz momentarily flickers — e.g. DYNAMIC_COMMANDERS
-- demoting the commander as pill HP wobbles near the hard-take threshold); it
-- closes implicitly when the substate leaves this set, or explicitly (bcc).
M.BLITZ_CALL_OPEN_SUB = {
  plan_position = true, approach = true,
  gather_trees = true, detree = true, build_walls = true,
  blitz_wait = true,
}

-- Take-progress rank for commander de-confliction: a higher rank means further
-- into the take. When two bots open a rival blitz on the same pill, the one
-- FURTHER along is the established captain (first to build wins) — the other
-- joins as a soldier, regardless of player number. Unlisted/nil substate = 0.
local _BLITZ_RANK = {
  plan_position = 1, approach = 2, gather_trees = 3,
  detree = 4, build_walls = 5, blitz_wait = 6,
}
local function blitz_rank(sub) return _BLITZ_RANK[sub or ""] or 0 end

-- ── Blitz party size (per bot) ────────────────────────────────────────────
-- Both numbers are TANKS INCLUDING THE COMMANDER, the same convention the
-- constants they default from already used ("2 = commander + 1 soldier").
-- Module-level values, and each bot has its own lua_State, so these are per
-- bot; init.lua's "blitz=MIN[/MAX]" BRAIN_INIT_ARG token calls set_blitz_size.
--
--   MIN — a HARD quorum. No GO path fires below it: not the early-GO on
--         critical mass, not the abort-build-and-charge, and not the
--         READY_TIMEOUT (a commander that times out short-handed abandons the
--         blitz instead of charging under-strength). Defaults to the larger of
--         the two constants that used to gate those paths separately —
--         SQUAD_BLITZ_GO_EARLY_READY and BLITZ_MIN_READY_TO_CHARGE, both 2 —
--         so the default behaviour is unchanged and they now seed ONE quorum.
--   MAX — caps the party: a call already holding MAX tanks accepts no more
--         joiners and reads as FULL to a soldier looking for a call. Defaults
--         to SQUAD_MAX_SIZE + 1 (commander + today's soldier cap). When only
--         MIN is given it is raised to MIN, so "blitz=3" alone is workable
--         instead of asking for a quorum the cap can never supply.
local BLITZ_MIN = math.max(C.SQUAD_BLITZ_GO_EARLY_READY or 2,
                           C.BLITZ_MIN_READY_TO_CHARGE or 2)
local BLITZ_MAX = (C.SQUAD_MAX_SIZE or 1) + 1
M.blitz_size_source = "default"

function M.blitz_min() return BLITZ_MIN end
function M.blitz_max() return BLITZ_MAX end
-- Max SOLDIERS a commander accepts = party max minus the commander itself.
-- This is what every old `cap = C.SQUAD_MAX_SIZE` reader wants.
function M.blitz_soldier_cap() return math.max(0, BLITZ_MAX - 1) end

function M.set_blitz_size(mn, mx, source)
  BLITZ_MIN = math.max(1, mn or BLITZ_MIN)
  BLITZ_MAX = math.max(BLITZ_MIN, mx or BLITZ_MAX)
  M.blitz_size_source = source or "init_arg"
end

-- One canonical string for every place that DISPLAYS the party size (print2 /
-- DECISION lines, the blitz_wait timeout HUD, the joinable-calls HUD).
function M.blitz_size_label()
  return string.format("blitz min=%d max=%d (%s)", BLITZ_MIN, BLITZ_MAX, M.blitz_size_source)
end

-- ── Blitz suicider quota (per bot) ────────────────────────────────────────
-- "At least this many members of a blitz should be pill_suiciders." At GO the
-- commander counts the suiciders already in the party (itself included, whether
-- by the "suicider" token or the harasser slate) and designates that many random
-- non-suicider SOLDIERS to make up the difference — never one that already is
-- one. Soldiers are preferred (the commander is the tank that leads the take),
-- but if they can't cover the minimum the commander designates ITSELF too, so
-- blitzsuiciders=4 on a party of 4 really does field four suiciders. 0 (the
-- default) means it never designates, which is exactly today's behaviour.
-- Per-bot, like the sizes above; the "blitzsuiciders=N" token replaces it.
local BLITZ_MIN_SUICIDERS = C.BLITZ_MIN_SUICIDERS or 0
M.blitz_suiciders_source = "default"

function M.blitz_min_suiciders() return BLITZ_MIN_SUICIDERS end
function M.set_blitz_min_suiciders(n, source)
  BLITZ_MIN_SUICIDERS = math.max(0, n or BLITZ_MIN_SUICIDERS)
  M.blitz_suiciders_source = source or "init_arg"
end
function M.blitz_suiciders_label()
  return string.format("blitzsuiciders min=%d (%s)", BLITZ_MIN_SUICIDERS, M.blitz_suiciders_source)
end

-- ── Blitz-only pill attacks (2026-09-24) ──────────────────────────────────
-- On when the "blitzonly" init flag (state.blitz_only) OR the constant
-- C.BLITZ_ONLY_PILL_ATTACKS is set. Then a LIVE pill is attacked only inside
-- a blitz. Three gates read this:
--   goals.lua  apply_blitz_only_gate -- pool-6 row REJECT "blitz_only" when the
--              pill has no blitz of ours, no open call, and M.can_lead_blitz
--              says we could not lead one.
--   attack.lua update_attack_substate -- BLITZ_ONLY_ABORT when a take is about
--              to leave the pre-GO substates (BLITZ_CALL_OPEN_SUB) without a
--              committed GO; and the "finish it solo" shortcuts (pill under
--              HARD_TAKE_MIN_HP, nobody joined) are skipped.
--   squad.lua  the DYNAMIC_COMMANDERS election -- any LIVE pill may be led, not
--              only one at or over HARD_TAKE_MIN_HP, because a soft pill has no
--              other way to be taken.
function M.blitz_only(state)
  return (state and state.blitz_only) or C.BLITZ_ONLY_PILL_ATTACKS or false
end
function M.blitz_only_label(state)
  if state and state.blitz_only then return "blitzonly flag" end
  if C.BLITZ_ONLY_PILL_ATTACKS then return "cfg BLITZ_ONLY_PILL_ATTACKS" end
  return "off"
end

-- Could this bot OPEN (lead) a fresh blitz on a pill with `pill_hp` HP right
-- now? Answers ok, why -- `why` is the first gate that fails, for the reject
-- line. MIRRORS the fresh-command gates of the DYNAMIC_COMMANDERS election in
-- M.update (following another commander, armour, ammo_deprived, shells vs pill
-- HP, AHEAD_BLITZ_ONLY) plus the noblitz / BLITZ_ENABLED switch; keep the two in
-- step. Without DYNAMIC_COMMANDERS the role is the deterministic one, so the
-- last tick's squad_role answers. Only read while blitz_only is on.
function M.can_lead_blitz(state, info, pill_hp)
  if state.blitz_disabled then return false, "noblitz" end
  if not C.BLITZ_ENABLED then return false, "BLITZ_ENABLED=false" end
  if state.squad_cmdr then
    return false, string.format("soldier of C%s", tostring(state.squad_cmdr))
  end
  if not C.DYNAMIC_COMMANDERS then
    if state.squad_role ~= M.ROLE_COMMANDER then
      return false, string.format("role %s (not a commander)", tostring(state.squad_role))
    end
    return true, "commander role"
  end
  local arm = (info and info.armour) or 0
  local need = C.SQUAD_COMMANDER_MIN_ARMOUR or 30
  if arm < need then return false, string.format("armour %d < %d", arm, need) end
  if state.ammo_deprived then return false, "ammo_deprived" end
  local sh = (info and info.shells) or 0
  if sh <= (pill_hp or 0) then
    return false, string.format("shells %d <= pill hp %d", sh, pill_hp or 0)
  end
  if C.AHEAD_BLITZ_ONLY and not state.team_ahead then
    return false, "AHEAD_BLITZ_ONLY and team behind"
  end
  return true, "can lead"
end

-- Free allies for the blitz-only commander gate (C.BLITZ_ONLY_CMDR_NEEDS_FREE,
-- 2026-09-25). An ally is FREE when it is live (active slot, not dead since
-- its last broadcast) and in NO blitz. In a blitz means any of:
--   call   it has an open call of its own (state.blitz_calls[pn])
--   cmdr   it is a commander on a take (broadcast sqst "blitz")
--   C<n>   it answers commander n: committed (cmdr set, sqst "join") or
--          negotiating (cmdr set, sqst "nego" -> shown "nego:C<n>")
--   bac    a live ally commander lists it in its bac accept list
-- Everything else (sqst "free" / "bz" / "full" / none) is free. /info state
-- replaces the whole slot, so role/cmdr/sqst are the ally's current values.
-- Returns count, desc: desc names every live ally and its state, e.g.
-- "p0=cmdr p1=C0 p2=nego:C0 p4=C6 p6=call", so the reject breakdown shows
-- where the count comes from.
function M.free_ally_count(state, now, self_pn)
  local max_age = C.SQUAD_ALLY_MAX_AGE or 1750
  local dead    = state.tank_dead_at
  local calls   = state.blitz_calls
  -- Soldiers any live commander has accepted (bac "1" / "1,2").
  local accepted = nil
  for pn, slot in ally_state.iter_active(now, max_age) do
    if pn ~= self_pn then
      local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
      local bac = slot.info and slot.info.bac
      if not is_dead and bac and bac ~= "" then
        for s in tostring(bac):gmatch("%d+") do
          accepted = accepted or {}
          accepted[tonumber(s)] = pn
        end
      end
    end
  end
  local n, parts = 0, {}
  for pn, slot in ally_state.iter_active(now, max_age) do
    if pn ~= self_pn then
      local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
      if not is_dead then
        local h = slot.info or {}
        local cm = tonumber(h.cmdr or "")
        local st
        if calls and calls[pn] then st = "call"
        elseif h.sqst == "blitz" then st = "cmdr"
        elseif cm and h.sqst == "nego" then st = "nego:C" .. cm
        elseif cm then st = "C" .. cm
        elseif accepted and accepted[pn] then st = "bac:C" .. accepted[pn]
        else st = "free"; n = n + 1 end
        parts[#parts + 1] = string.format("p%d=%s", pn, st)
      end
    end
  end
  return n, (#parts > 0) and table.concat(parts, " ") or "no live allies"
end

-- Soldier call pick under the commander gate (C.BLITZ_ONLY_CMDR_NEEDS_FREE,
-- blitz-only only). The pick in M.update answers the NEAREST joinable call,
-- but availability() says yes only for the call on our goal pill. When goal
-- selection chose a different open call's pill (the gate sends a blocked bot
-- there), return that call instead: pn, dist, pill. Joinable matches only
-- (is_full(pn, pill) false); nearest wins, tie -> lower pn. nil = keep the
-- nearest pick (knob off, no blitz-only, a cost switch chose already, no
-- attack_pill goal, nearest already on our pill, or no joinable match).
function M.goal_call_pick(state, cmd_info, best_pn, best_target, is_full)
  if not (best_pn and not state._blitz_switch_to
          and C.BLITZ_ONLY_CMDR_NEEDS_FREE and M.blitz_only(state)) then
    return nil
  end
  local g = state.goal
  local gt = g and g.kind == "attack_pill" and g.target_id
  if not gt or best_target == gt then return nil end
  local m_pn, m_d
  for pn, ci in pairs(cmd_info) do
    if ci.target == gt and not is_full(pn, ci.target)
       and (not m_d or ci.dist < m_d or (ci.dist == m_d and pn < m_pn)) then
      m_pn, m_d = pn, ci.dist
    end
  end
  if not m_pn then return nil end
  return m_pn, m_d, gt
end

-- Every soldier COMMITTED to our blitz on `our_pid`, as an array of
-- { pn = n, suicider = bool } sorted by player number. Same membership test as
-- blitz_ready_status (role s, cmdr = us, past negotiation, broadcasting
-- attack_pill on our pill, alive); `suicider` is the ally's broadcast psu flag,
-- which is set for a forced, slate-picked OR already blitz-designated suicider.
-- SORTED because the designation picks from it with the seeded RNG and
-- ally_state's pairs() order is not reproducible.
-- C.BLITZ_GO_ACCEPTED_ONLY (2026-09-25 evening): is soldier `pn` in the
-- accept list this commander computed (state.squad_blitz_accept, "1,2", the
-- list it sends as bac)? Knob off, or we are not the commander -> true (count
-- as before). 20260925_134920 bot3 t=3889: GO counted p2, which the arbiter
-- had dropped at t=3043 when p2's bes vanished.
function M.go_counts_soldier(state, pn)
  if not C.BLITZ_GO_ACCEPTED_ONLY or state.squad_role ~= M.ROLE_COMMANDER then return true end
  local acc = state.squad_blitz_accept
  if not acc then return false end
  for s in string.gmatch(acc, "%d+") do
    if tonumber(s) == pn then return true end
  end
  return false
end

function M.blitz_members(state, now, self_pn, our_pid)
  local out = {}
  if not our_pid then return out end
  local dead = state.tank_dead_at
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= self_pn then
      local h = slot.info
      local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
      if not is_dead and h.role == "s" and tonumber(h.cmdr or "") == self_pn
         and h.sqst ~= "nego"
         and h.goal == "attack_pill" and tonumber(h.target or "") == our_pid
         and M.go_counts_soldier(state, pn) then
        out[#out + 1] = { pn = pn, suicider = (h.psu == "1") }
      end
    end
  end
  table.sort(out, function(a, b) return a.pn < b.pn end)
  return out
end

-- Commander-only, called ONCE per take at the moment GO fires (not on an abort
-- path). Tops the blitz up to BLITZ_MIN_SUICIDERS suiciders by picking that many
-- soldiers uniformly at random from the ones that are not suiciders already, and
-- queues a "bsu <pill> <pn>" broadcast for each (init.lua's blitz TX block sends
-- them). Uses math.random — the brain's seeded RNG — so a -brain-lua-seed run
-- designates the same tanks every time. If the soldiers can't cover the
-- minimum, the commander finishes the job by designating itself (see below).
function M.blitz_designate_suiciders(state, info, now, our_pid)
  local want = BLITZ_MIN_SUICIDERS
  if want <= 0 or not our_pid then return end
  local self_pn = info and info.player_number or -1
  local members = M.blitz_members(state, now, self_pn, our_pid)
  -- The commander counts toward the quota but is never a candidate: it is the
  -- one tank that has to survive to lead the take.
  local have = state.is_pill_suicider and 1 or 0
  local pool = {}
  for _, m in ipairs(members) do
    if m.suicider then have = have + 1 else pool[#pool + 1] = m.pn end
  end
  local need = want - have
  local picked = {}
  while need > 0 and #pool > 0 do
    local i = math.random(#pool)
    picked[#picked + 1] = pool[i]
    table.remove(pool, i)
    need = need - 1
  end
  -- Soldiers alone couldn't make the minimum (too few of them, or they are all
  -- suiciders already) → the commander designates ITSELF as well. Same
  -- temporary blitz-suicider state a "bsu" broadcast installs on a soldier
  -- (comms.lua), so it expires exactly the same way: pill gone/ours, off the
  -- take, BLITZ_SUICIDER_MAX_TICKS backstop, or our own death via
  -- reset_blitz_state. `by = self_pn` marks it as self-designated — the
  -- expiry check in update() skips its commander-gone test for that case (we
  -- are the commander, and we never appear in our own ally_state).
  -- This is what makes blitzsuiciders=4 on a party of 4 field four suiciders.
  local self_designated = false
  if need > 0 and not state.is_pill_suicider and not state.blitz_suicider then
    state.blitz_suicider = { pill = our_pid, by = self_pn, since = now }
    self_designated = true
    have = have + 1
    need = need - 1
  end
  if #picked > 0 then
    local q = state._blitz_su_send
    if not q then q = {}; state._blitz_su_send = q end
    for _, pn in ipairs(picked) do q[#q + 1] = { pill = our_pid, pn = pn } end
  end
  if BRAIN_DEBUG_MODE then
    local names = {}
    for _, pn in ipairs(picked) do names[#names + 1] = "p" .. tostring(pn) end
    if self_designated then names[#names + 1] = "self" end
    local why = ""
    if #names == 0 then
      if #members == 0 and have >= want then why = " (no soldiers, quota already met)"
      elseif #members == 0 then why = " (no soldiers)"
      elseif need <= 0 then why = " (quota already met)"
      else why = " (all soldiers already suiciders)" end
    elseif need > 0 then
      why = string.format(" (still %d short)", need)
    end
    print2(string.format("BLITZ_SUICIDER t=%d pill=#%s GO members=%d suiciders=%d min=%d designate=[%s]%s",
      now, tostring(our_pid), #members + 1, have, want, table.concat(names, " "), why))
  end
end

-- ── Contested take → one of the party goes in as a suicider ─────────
-- The take of a pill is CONTESTED when a live hostile TANK is sitting within
-- C.BLITZ_CONTESTED_RANGE tiles (euclidean) of that pill. That is the take
-- that most often gets undone: we kill the pill, the defender's LGM walks
-- straight back out and repairs it while our survivors are reloading and
-- backing off. So on a contested take C.BLITZ_CONTESTED_SUICIDERS (1) of the
-- blitzers is made a suicider — a suicider is the role that actually goes and
-- kills the repairing LGM instead of backing off — while the rest stay normal
-- tanks that can finish the pill and hold the ground. Any party of 2 or more,
-- soldiers picked first. (It used to designate EVERY blitzer; the 2026-09-05
-- evening bench had that losing 7-3 in 2v2 against KEEL, so it is one now.)
--
-- Only REAL sightings count. perc.enemy_tanks is built from this tick's
-- OBJECT_TANK objects carrying OBJECT_HOSTILE, so allies are never in it and
-- neither are ghosts (perception keeps those in perc.ghost_tanks — a ghost is
-- a GUESS at where an out-of-sight tank went, no basis for rewriting the whole
-- party's role).
--
-- Returns the nearest such tank's player number (ob.idnum == player number for
-- tanks) and its distance from the pill in tiles, or nil when uncontested.
function M.blitz_contested_enemy(state, pmx, pmy)
  if not (pmx and pmy) then return nil end
  local perc = state and state.perc
  local ets  = perc and perc.enemy_tanks
  if not ets then return nil end
  local r     = C.BLITZ_CONTESTED_RANGE or 9
  local r2    = r * r
  local best, best_d2 = nil, math.huge
  for i = 1, #ets do
    local et = ets[i]
    local dx = (et.mx or 0) - pmx
    local dy = (et.my or 0) - pmy
    local d2 = dx * dx + dy * dy
    if d2 <= r2 and d2 < best_d2 then
      best_d2 = d2
      best    = et
    end
  end
  if not best then return nil end
  -- `or -1` so a sighting with no id (never seen in practice — the engine
  -- always stamps a tank's idnum) still reads as CONTESTED rather than
  -- silently as "no enemy": the caller treats a nil return as uncontested.
  return best.id or -1, math.sqrt(best_d2)
end

-- Commander-only. Designates C.BLITZ_CONTESTED_SUICIDERS (1) of the blitz --
-- the committed soldiers plus the commander itself -- temporary blitz
-- suiciders on a CONTESTED take, regardless of BLITZ_MIN_SUICIDERS. The same
-- number for ANY party of 2 or more:
--
--   party >= 2
--       C.BLITZ_CONTESTED_SUICIDERS of them, picked the same way as the
--       BLITZ_MIN_SUICIDERS quota: SOLDIERS first, uniformly at random over
--       the seeded RNG, and the commander designates ITSELF only if the
--       soldiers cannot cover the number. With the default 1 that is exactly
--       one soldier, and the commander stays a normal tank that can finish
--       the pill and hold the ground.
--   party == 1
--       a solo take -- no call, no party -- so nothing is designated and
--       nothing is LATCHED either (mode "solo"): the caller is free to ask
--       again on its next replan, when a soldier may have joined.
--
-- Members that are already suiciders (permanent token, harasser slate, or a
-- designation queued moments ago by blitz_designate_suiciders) are left exactly
-- as they are, and they COUNT toward the number -- so a party whose only
-- soldier is already a suicider designates nobody new (the log says who
-- covers it). Same "bsu" verb and the same expiry rules as the quota
-- designation -- pill gone/ours, off the take, commander gone,
-- BLITZ_SUICIDER_MAX_TICKS, or death -- so nothing about the wind-down changes.
--
-- ONCE PER TAKE. Any party of 2+ latches (mode "done"), and the caller stops
-- asking; a party that grows afterwards designates nothing more. Only a solo
-- take leaves the door open.
--
-- Returns: number newly designated, mode ("done" | "solo"), party size.
function M.blitz_designate_contested(state, info, now, our_pid, enemy_pn, enemy_dist)
  if not our_pid then return 0, "solo", 0 end
  local self_pn = info and info.player_number or -1
  local members = M.blitz_members(state, now, self_pn, our_pid)
  local party   = #members + 1
  if party < 2 then return 0, "solo", party end
  -- Anything already queued for this take (the BLITZ_MIN_SUICIDERS top-up runs
  -- at the same GO, and only one bsu goes out per tick) has not reached the
  -- soldier yet, so its broadcast psu flag still reads 0. Skip those, or we
  -- send a second bsu and restart its BLITZ_SUICIDER_MAX_TICKS clock.
  local q = state._blitz_su_send
  local queued = nil
  if q then
    for _, d in ipairs(q) do
      if d.pill == our_pid then queued = queued or {}; queued[d.pn] = true end
    end
  end
  local self_su = (state.is_pill_suicider or state.blitz_suicider) and true or false

  local names, n = {}, 0
  local function designate(pn)
    if not q then q = {}; state._blitz_su_send = q end
    q[#q + 1] = { pill = our_pid, pn = pn, why = "contested" }
    names[#names + 1] = "p" .. tostring(pn)
    n = n + 1
  end
  -- Self-designation uses `by = self_pn`, which the expiry check reads as "we
  -- designated ourselves" and so skips its commander-gone test -- see update().
  local function designate_self()
    if self_su then return false end
    state.blitz_suicider = { pill = our_pid, by = self_pn, since = now, why = "contested" }
    self_su = true
    names[#names + 1] = "self"
    n = n + 1
    return true
  end

  local want = C.BLITZ_CONTESTED_SUICIDERS or 1
  -- Already-suiciders count toward the number, the commander included.
  local have, covered = 0, {}
  if self_su then have = 1; covered[#covered + 1] = "self" end
  local pool = {}
  for _, m in ipairs(members) do
    if m.suicider or (queued and queued[m.pn]) then
      have = have + 1
      covered[#covered + 1] = "p" .. tostring(m.pn)
    else
      pool[#pool + 1] = m.pn
    end
  end
  local need = want - have
  -- Soldiers FIRST, uniformly at random over the brain's seeded RNG
  -- (blitz_members is sorted, so the draw is reproducible).
  while need > 0 and #pool > 0 do
    local i = math.random(#pool)
    designate(pool[i])
    table.remove(pool, i)
    need = need - 1
  end
  -- Only when the soldiers cannot cover it does the commander take a slot.
  if need > 0 and designate_self() then need = need - 1 end

  print2(string.format(
    "BLITZ_CONTESTED t=%d pill=#%s enemy=p%s dist=%.1f party=%d -> %d suicider%s designate=[%s]%s",
    now, tostring(our_pid), tostring(enemy_pn), enemy_dist or -1, party,
    want, want == 1 and "" or "s", table.concat(names, " "),
    (n == 0 and #covered > 0)
      and string.format(" (already covered by %s)", table.concat(covered, " ")) or ""))
  return n, "done", party
end

-- One canonical string for every place that DISPLAYS whether a take is
-- contested, next to the quorum / suicider counts it changes the meaning of.
-- `designated` is how many the take actually designated, so a panel showing a
-- contested take with one suicider reads as the rule working
-- (BLITZ_CONTESTED_SUICIDERS is 1), not as a designation that went missing.
function M.blitz_contested_label(c)
  -- Three states, not two. "no" means the rule LOOKED and found no hostile tank
  -- inside BLITZ_CONTESTED_RANGE; "off" means the rule never ran at all because
  -- BLITZ_CONTESTED_ALL_SUICIDERS is false (the default since 2026-09-05).
  -- Collapsing them made a switched-off take read exactly like an uncontested
  -- one, so a log could not tell "nobody was near" from "we never asked".
  if not c then
    return C.BLITZ_CONTESTED_ALL_SUICIDERS and "contested{no}" or "contested{off}"
  end
  return string.format("contested{yes enemy=p%s dist=%.1f t=%d party=%d designated=%d}",
                       tostring(c.pn), c.dist or -1, c.tick or -1,
                       c.party or -1, c.n or 0)
end

-- Wipe ALL blitz/squad coordination state. Call on tank death so a respawn
-- comes back with a clean slate — no stale negotiation, offer, reject,
-- roster, watchdog, broadcast latch, or call registry leaking across the
-- death. Most of these are recomputed each tick by update()/blitz_arbitrate(),
-- but several PERSIST (pill-reject table, offered pill, negotiate watchdog,
-- commander roster/accept/reject, comm latches) and would otherwise survive
-- into the next life. Keep this exhaustive: every blitz/squad state field
-- below should appear here. (Goal-level _blitz* fields die with state.goal,
-- which the caller resets separately.)
function M.reset_blitz_state(state)
  -- soldier-side negotiation / commitment
  state.squad_blitz_accepted    = nil
  state.squad_blitz_target      = nil
  state.squad_negotiate_cmdr    = nil
  state.squad_negotiate_pill    = nil
  state.squad_blitz_engage_mx   = nil
  state.squad_blitz_engage_my   = nil
  state.squad_blitz_engage_fx   = nil
  state.squad_blitz_engage_fy   = nil
  state.squad_blitz_engage_deg  = nil
  state.squad_blitz_bd          = nil
  state.squad_blitz_in_position = nil
  state.squad_blitz_aimed       = nil
  state.squad_blitz_go          = nil
  state.squad_blitz_repos       = nil
  -- commander-side roster / broadcast
  state.squad_blitz_roster      = nil
  state.squad_blitz_reject      = nil
  state.squad_blitz_accept      = nil
  state._blitz_spot_since       = nil   -- first-come spot-claim timestamps (commander arbiter)
  -- squad membership / status (recomputed each tick, cleared for cleanliness)
  state.squad_role              = nil
  state.squad_commander_pill    = nil   -- sticky commander latch dies with the take
  state.squad_cmdr              = nil
  state.squad_status            = nil
  state.squad_help_target       = nil
  state.squad_pns               = nil
  state.squad_joinable_pills    = nil
  -- commander's own open call
  state._my_blitz_call          = nil
  state._my_blitz_call_tick     = nil
  -- negotiation internals / watchdog / offers / rejects (these PERSIST)
  state._blitz_call_rejected    = nil
  state._blitz_comm_reject      = nil
  state._blitz_negotiate_key    = nil
  state._blitz_negotiate_since  = nil
  state._blitz_negotiate_scan   = nil
  state._blitz_new_call         = nil
  state._blitz_offer_pill       = nil
  state._blitz_pill_reject      = nil
  -- Blitz-designated suicider is per-take and per-life: a respawn must not come
  -- back still suiciding for a blitz that ended while it was dead. The pending
  -- send queue goes too — those designations were for the previous life's take.
  -- Keep a reason when we actually cancel one, so the [role] revert line says
  -- WHY rather than going quiet; clear it otherwise so a stale reason can't
  -- attach itself to an unrelated later revert.
  state._blitz_su_end_why       = state.blitz_suicider and "death_respawn" or nil
  state.blitz_suicider          = nil
  state._blitz_su_send          = nil
  state._blitz_rebroadcast      = nil
  state._blitz_reject           = nil
  state._blitz_repick_tick      = nil
  state._blitz_switch_to        = nil
  -- committed-soldier bd refresh + commander "where are you?" query latches
  state._blitz_bd_refresh_tick  = nil
  state._blitz_query_ack        = nil
  state._blitz_query_pid        = nil
  state._blitz_query_until      = nil
  -- known-call registry: clear + re-announce discovery from the new spawn
  state.blitz_calls             = {}
  state._blitz_query_send       = true
  -- fresh-kill pickup claim dies with the tank (a respawn shouldn't chase a
  -- pill it "killed" in a previous life).
  state.kill_pickup             = nil
  -- Blitz window (M.blitz_window, 2026-09-26): a respawned bot kept the last
  -- life's window and went on keeping that blitz's lines and the no-build
  -- rule for up to SQUAD_BLITZ_WAIT_TIMEOUT ticks.
  state._blitz_win              = nil
end


-- Build the sorted list of active protocol-bot player numbers (self + allies
-- seen recently). Humans don't broadcast our protocol so they're excluded
-- automatically by iter_active (they never populate ally_state slots here).
local function protocol_pns(state, info, now)
  local self_pn = info.player_number or -1
  local pns = { self_pn }
  local max_age = C.SQUAD_ALLY_MAX_AGE or 1750
  for pn in ally_state.iter_active(now, max_age) do
    if pn ~= self_pn then pns[#pns + 1] = pn end
  end
  table.sort(pns)
  return pns, self_pn
end

-- Deterministic squad role for `self_pn` given the sorted protocol set.
-- Commanders = ceil(N / baseline) lowest player numbers; everyone else soldier.
-- (Harasser is NO LONGER a role — it's an independent flag, M.is_harasser, so a
-- harasser is a full squad member that can command/join blitzes like anyone.)
function M.role_for(pns, self_pn)
  local n = #pns
  if n == 0 then return M.ROLE_SOLDIER end
  local self_idx
  for i = 1, n do if pns[i] == self_pn then self_idx = i break end end
  if not self_idx then return M.ROLE_SOLDIER end

  local baseline = C.BASELINE_SQUAD_SIZE or 3
  local n_cmd    = math.max(1, math.ceil(n / baseline))
  if self_idx <= n_cmd then return M.ROLE_COMMANDER end
  return M.ROLE_SOLDIER
end

-- Dynamic harasser fraction: baseline HARASSER_FRAC, ramping toward
-- HARASSER_FRAC_MAX as our base advantage (base_strength) climbs past
-- HARASSER_BASE_THRESHOLD — but only while pill strength is at/above
-- HARASSER_PILL_FLOOR ("at least not losing a lot"). Dominating bases while
-- holding pills frees more bots to harass. Falls back to the floor without state.
local function harasser_frac(state)
  local frac = C.HARASSER_FRAC or 0.20
  if not state then return frac end
  if (state.strength or 0.5) < (C.HARASSER_PILL_FLOOR or 0.40) then return frac end
  local thr  = C.HARASSER_BASE_THRESHOLD or 0.60
  local base = state.base_strength or 0.5
  if base <= thr then return frac end
  local t = math.min(1.0, (base - thr) / math.max(0.01, 1.0 - thr))
  return frac + t * ((C.HARASSER_FRAC_MAX or frac) - frac)
end

-- Harasser designation, INDEPENDENT of squad role: the highest floor(frac*N)
-- player numbers in the protocol set. A harasser is a normal squad member (it
-- commands/joins blitzes like anyone); the flag only drives its goal-cost biases
-- (pill cost ×HARASSER_PILL_COST_MULT, distance ×HARASSER_TRAVEL_MULT — see
-- goals.lua). frac is dynamic (harasser_frac), but base/pill strength are nearly
-- identical across the team, so the set stays effectively agreed.
function M.is_harasser(pns, self_pn, state)
  local n = #pns
  local n_har = math.floor(harasser_frac(state) * n)
  if n_har <= 0 then return false end
  local self_idx
  for i = 1, n do if pns[i] == self_pn then self_idx = i break end end
  if not self_idx then return false end
  return self_idx > (n - n_har)
end

-- Pillbox-suicider map opt-in. On a map listed in C.PILL_SUICIDER_MAPS the
-- harasser slate is repurposed wholesale: the SAME bots is_harasser would have
-- picked become pill_suiciders instead, and nobody is a harasser. The count is
-- still governed by HARASSER_FRAC / the dynamic ramp — this only decides which
-- of the two roles the picked bots get.
--
-- Identity string: info.gameinfo.mapname, pushed each tick by braincore.c from
-- ClientSim's mapName. That is the map file's BASENAME with the directory AND
-- the ".map" extension stripped (server_sim.c does the stripping before it goes
-- out over CTRL_LOBBY_SETTINGS), so "data/maps/Survival.map" arrives as
-- "Survival". Table keys must match that exact form.
function M.is_pill_suicider_map(info)
  local t = C.PILL_SUICIDER_MAPS
  if not t then return false end
  local name = info and info.gameinfo and info.gameinfo.mapname
  if not name or name == "" then return false end
  return t[name] == true
end

-- Per-tick update: recompute self's role and stash it on state. The broadcast
-- block in init.lua copies state.squad_role into bsi.role.
-- (Hysteresis / spawn-settle is a TODO — for now the set stabilises within ~1s
-- of spawn and the role follows it. No behavior keys off role yet.)
-- Goal kinds a soldier will drop outright to answer a blitz (the
-- Can this bot answer a commander's blitz on pill `help_target_id`?
-- Returns ok(bool), reason(code|nil): "lh" low hp / "na" no ammo / "bz" busy.
--
-- We ONLY join a blitz once our OWN cost competition (with the blitz join
-- discount applied in goal_selection) has already made the call's pill our
-- goal — i.e. we're on attack_pill for the SAME pill the commander wants. The
-- old "freely interruptible" set (explore / attack_tank / capture / place /
-- early attack_pill on any pill / topped-off refuel) let a soldier commit to a
-- blitz its goal_selection would never actually pursue, which produced endless
-- accept/commit/leave churn. The discount alone now decides whether a blitz
-- pill is worth switching to; availability just confirms we made that switch.
-- =========================================================================
-- M.busy(state, info) -> busy, reason
-- THE one "not interruptible" test, shared by the blitz availability gate
-- below and by the chat-order auction (orders.lua).  Reasons:
--   man_out        the LGM is out of the tank, for any reason
--   capturing      capture_pill in its final phase (man dispatched)
--   repositioning  a reposition move executing after its vote passed
--   kill_me        the kill-me hand-off, while executing
--   escaping       escape_water or a stuck escape in progress
-- Everything else is interruptible: attack_pill on another pill, a topped-off
-- refuel, explore, seek_trees.  Low armour and no ammo are NOT busy reasons
-- (the refuel pause covers them) and there is no danger check.
-- Blitz behaviour is unchanged by routing availability() through this: every
-- case here already failed availability()'s own blitz-only rule.
-- The rule itself lives in orders.lua so the order code and this gate cannot
-- drift apart; this is the name the design doc asked for.
function M.busy(state, info)
  return orders.busy(state, info)
end

-- The rest of the bot-command questions goal selection has to ask, re-exported
-- through this module for the same reason M.busy lives here: the rule itself
-- stays in orders.lua, and goals.lua cannot take another top-level local --
-- its main chunk is at Lua's 200-local cap.
--   focus_mult(state, kind)          the `focus bases` / `focus pills` factor
--   focus_label(state)               the one breakdown chip for it
--   reposition_human_blocked(state)  `reposition on` / `off` vs the constant
function M.focus_mult(state, kind)  return orders.focus_mult(state, kind) end
function M.focus_label(state)       return orders.focus_label(state) end
function M.reposition_human_blocked(state)
  return orders.reposition_human_blocked(state)
end

function M.availability(state, info, help_target_id)
  -- Joining a blitz has NO armour floor (a 2+ tank take shares the incoming
  -- fire) — EXCEPT while carrying a pillbox: cautious mode, so a joiner needs
  -- commander-level armour before diving in and risking the pill it's holding.
  local ok, reason
  -- The shared "not interruptible" test (see M.busy above) runs first, but
  -- only for the reasons that CANNOT be true at the same time as the
  -- blitz-only rule at the bottom of this chain — otherwise routing this gate
  -- through busy() would refuse joins the old chain allowed, and blitz
  -- behaviour has to stay bit-for-bit.
  --
  -- Checked, reason by reason, against "g.kind == 'attack_pill' and
  -- g.target_id == help_target_id":
  --   capturing      IMPOSSIBLE. It needs g.kind == "capture_pill".
  --   man_out        POSSIBLE. A commander on attack_pill for the help pill
  --                  with its man out (building a blocker or a wall shield)
  --                  answered YES before. Ignored.
  --   kill_me        POSSIBLE. km.executing is set purely by an ally
  --                  claimant's distance; it does not look at the goal.
  --                  Ignored.
  --   escaping       POSSIBLE. The stuck-escape counter does not look at the
  --                  goal either. Ignored.
  --   repositioning  POSSIBLE in a narrow window: the approved pill is one of
  --                  OURS, and an enemy that takes it makes it an attack_pill
  --                  target for a few ticks before the approval is cleared.
  --                  Ignored, because "narrow" is not "never".
  -- Only `capturing` is left, and it is a no-op by construction — so this
  -- gate cannot change a single blitz answer. It stays in the chain so the
  -- shared rule has ONE home and the SQUAD_AVAIL line names the busy state.
  local _busy, _busy_reason = M.busy(state, info)
  local _busy_blocks = _busy and _busy_reason == "capturing"
  if _busy_blocks and C.BOT_COMMANDS_ENABLED then
    ok, reason = false, "bz"
  elseif state.blitz_disabled or not C.BLITZ_ENABLED then
    -- "noblitz" BRAIN_INIT_ARG (or BLITZ_ENABLED=false, e.g. Easy difficulty):
    -- this bot never joins anyone's blitz. Answered
    -- here (rather than at every call site) because availability() is the one
    -- gate every join path runs through — the squad-layer pick AND
    -- goals.apply_blitz_target, which is what would force the commander's pill
    -- to SQUAD_BLITZ_COST.
    ok, reason = false, "noblitz"
  elseif (info.carried_pills or 0) >= 1
     and (info.armour or 0) < (C.SQUAD_COMMANDER_MIN_ARMOUR or 30) then
    ok, reason = false, "lh"
  elseif (info.shells or 0) < (C.SQUAD_MIN_HELP_SHELLS or 3) and not state.ammo_deprived then
    -- Normal low-ammo tanks can't help shoot, so they don't join. But an
    -- ammo-DEPRIVED tank is the designated suicide decoy — it joins WITHOUT ammo
    -- specifically to charge the pill and draw fire for the captain, so it must
    -- bypass the no-ammo gate (the "suicide/decoy body" the blitz code expects).
    ok, reason = false, "na"
  else
    local g = state.goal
    if g and g.kind == "attack_pill" and help_target_id and g.target_id == help_target_id then
      ok, reason = true, nil
    else
      ok, reason = false, "bz"
    end
  end
  if BRAIN_DEBUG_MODE then print2(string.format("SQUAD_AVAIL t=%d help_pill=%s -> ok=%s reason=%s busy=%s (shells=%d deprived=%s goal=%s tgt=%s)", state.tick or 0, tostring(help_target_id), tostring(ok), tostring(reason), tostring(_busy_reason or "-"), info.shells or 0, tostring(state.ammo_deprived), state.goal and state.goal.kind or "?", state.goal and tostring(state.goal.target_id) or "?")) end
  return ok, reason
end

-- M.parse_bes(bes, pmx, pmy) -> fx, fy or nil
--   An ally's broadcast standoff ("fx,fy", float tile coords) as two numbers,
--   or nil when it is not two numbers, is off the map (outside 0..255), or,
--   when the pill tile (pmx,pmy) is given, lies more than BES_MAX_PILL_DIST
--   tiles from the pill centre. 2026-09-26: `bes` comes off the wire from any
--   ally. "1.2.3,4" matches the pattern but tonumber gives nil, which then
--   reached math.floor; "99999999,5" made the spot tests (spot_margin
--   .line_margin) walk a box millions of tiles wide on every tick until the
--   think budget ran out.
-- Bound: a standoff is picked at ATTACK_PILL_STANDOFF (7.4) or PPT_STANDOFF
-- (7) from the pill, a wall-shield spot at WALL_SHIELD_STANDOFF +1 (8), and
-- nothing is fired from beyond ATTACK_PILL_RANGE (9.5). 12 tiles leaves a
-- margin over all of them; a spot further out is not a spot on this pill.
local BES_MAX_PILL_DIST = 12
M.BES_MAX_PILL_DIST = BES_MAX_PILL_DIST   -- unit tests
function M.parse_bes(bes, pmx, pmy)
  if type(bes) ~= "string" then return nil end
  local sx, sy = bes:match("^(%-?[%d.]+),(%-?[%d.]+)$")
  local fx, fy = tonumber(sx), tonumber(sy)
  if not (fx and fy) then return nil end
  if fx < 0 or fx >= 256 or fy < 0 or fy >= 256 then return nil end
  if pmx and pmy then
    local dx, dy = fx - (pmx + 0.5), fy - (pmy + 0.5)
    if dx * dx + dy * dy > BES_MAX_PILL_DIST * BES_MAX_PILL_DIST then return nil end
  end
  return fx, fy
end

-- Commander standoff arbiter: gather every soldier answering THIS commander
-- (broadcast cmdr == self) plus self, each with their chosen standoff (bes) and
-- reported walk distance (bd). On a conflict (standoffs within CLASH tiles) the
-- FURTHER tank (larger bd) keeps it; tie -> lower player# keeps; the loser is
-- told to repick. Sets state.squad_blitz_reject (brj broadcast) + a roster for
-- the commander viz, and flips self's repick flag if the commander itself loses.
-- True if a shot from engage spot (sfx,sfy — FLOAT tile coords) to the pill
-- CENTER does NOT cleanly reach the pill — a wall, a HOSTILE BASE, or another
-- pillbox stops the shell first (e.g. a blocker the commander built, or an enemy
-- base between the soldier and the pill), or the trajectory ends short. Uses the
-- C shot-tile simulation (spot float -> world units; pill is tile-centered).
-- Aim points within the pill tile (world units): center + 4 corners (inset 1 gu),
-- same set attack.blitz_clear_aim uses so commander and soldier agree.
local _ARB_AIM_X = { 128, 1, 254, 1, 254 }
local _ARB_AIM_Y = { 128, 1, 1, 254, 254 }
--
-- C.BLITZ_SPOT_LOS_MARGIN > 0 (2026-09-24): use the SHARED spot test instead
-- (spot_margin.clear_aim_margin — the same function the soldier's pick runs):
-- aim set shield.AIM_OFFSETS_TILE_FIRE, shell test aim_line_trees, plus the
-- tapering line margin. Blocked unless one aim point passes both. 0 (KEEL) =
-- this function's own copy below, unchanged.
local function blitz_spot_shot_blocked(world, sfx, sfy, pmx, pmy, now, pn, quiet)
  local ox = math.floor(sfx * 256 + 0.5)
  local oy = math.floor(sfy * 256 + 0.5)
  local margin = C.BLITZ_SPOT_LOS_MARGIN or 0
  if margin > 0 then
    local idx = SM.clear_aim_margin(ox, oy, pmx, pmy, world, margin,
      { site = "arb_p" .. tostring(pn), tick = now, spot_fx = sfx, spot_fy = sfy, quiet = quiet })
    return idx == nil
  end
  -- Clear if ANY aim point (pill center or a corner) has an unobstructed shell
  -- path — so the arbiter doesn't reject a spot the soldier validated via a corner
  -- aim that dodges our shield walls. Blocked only if EVERY aim point is obstructed.
  for i = 1, 5 do
    local tiles = cpf.simulate_shot(ox, oy, bit.bor((bit.lshift(pmx, 8)), _ARB_AIM_X[i]), bit.bor((bit.lshift(pmy, 8)), _ARB_AIM_Y[i]), cpf.SHOT_TANK, 0)
    if tiles then
      local blocked, reached = false, false
      for _, t in ipairs(tiles) do
        if t.mx == pmx and t.my == pmy then reached = true; break end
        local tt = U.ttype(t.mx, t.my)
        if tt == C.T_BUILDING or tt == C.T_HALFBUILD then blocked = true; break end
        local be = world and world.base_at and world.base_at[t.my * 256 + t.mx]
        if be and be.base then blocked = true; break end
        if SM.pending_pill_at(world, t.mx, t.my) then blocked = true; break end
        local plist = world and world.pill_at and world.pill_at[t.my * 256 + t.mx]
        if plist then for _, e in ipairs(plist) do local pp = e.id and world.pills[e.id]; if pp and not pp.in_tank and (pp.health or 0) > 0 and pp.mx == t.mx and pp.my == t.my then blocked = true; break end end end
        if blocked then break end
      end
      if reached and not blocked then return false end   -- this aim is clear
    end
  end
  return true   -- every aim point blocked → no clean shot
end

M._blitz_spot_shot_blocked = blitz_spot_shot_blocked   -- unit tests

-- ── Pill placement vs blitz shot lines (C.PILL_PLACE_AVOID_BLITZ_LINE) ──
-- M.blitz_shot_lines(state, world, now, self_pn) -> list or nil
--   The live blitz shot lines a NEW pill of ours must not block:
--   { fx, fy, pmx, pmy, who } -- spot float point -> target pill tile.
--   Ours: the soldier's engage spot (squad_blitz_engage_*), else the attack
--   goal's standoff (the commander's own, or a soldier's assigned one).
--   Squadmates': their broadcast `bes`, for the same commander (the commander
--   itself, and every soldier whose cmdr key names it). Same pill for all.
--   nil when the knob is off or no blitz is live. Live = an attack_pill goal
--   that is a blitz (goal._blitz, or still negotiating one) and not a solo
--   fallback, or a soldier still accepted into a call (squad_blitz_accepted +
--   squad_blitz_target) while another goal briefly holds the tank; and the
--   target pill alive on the map. Covers negotiating, committed, approach,
--   blitz_wait and charge alike.
-- Raw lines, no knob test: M.blitz_shot_lines (PILL_PLACE_AVOID_BLITZ_LINE)
-- and the man-path test (PLACE_PILL_MAN_PATH_SAFE) both read it.
function M.blitz_shot_lines_raw(state, world, now, self_pn)
  local g = state and state.goal
  local tid, cmdr
  local left = false   -- lines of a blitz we already LEFT (blitz window)
  if g and g.kind == "attack_pill" and not g._blitz_solo
     and (g._blitz or state.squad_negotiate_cmdr) then
    tid = g.target_id
  elseif state and state.squad_blitz_accepted and state.squad_blitz_target then
    -- A committed soldier whose goal is briefly something else (an offensive
    -- guard drop won the pool): the squad still takes the pill, so its lines
    -- still count.
    tid = state.squad_blitz_target
    g = nil
  else
    -- C.BLITZ_LINES_AFTER_LEAVE: we left a blitz (yield / steal / switch /
    -- goal cleared) that is still running. Its allies still fire down their
    -- lines, so keep them until the blitz ends (M.blitz_window).
    local win = state and C.BLITZ_LINES_AFTER_LEAVE and M.blitz_window(state, world, now, self_pn)
    if not win or win.live then return nil end
    tid, cmdr, left, g = win.pill, win.cmdr, true, nil
  end
  local p = tid and world and world.pills and world.pills[tid]
  if not p or p.in_tank or (p.health or 0) <= 0 or not p.mx then return nil end
  local pmx, pmy = p.mx, p.my
  local lines = {}
  -- A blitz we left: our old spot is not a line any more (we no longer shoot
  -- from it), so only the allies' lines below count.
  if state.squad_blitz_engage_mx and not left then
    lines[#lines + 1] = { fx = state.squad_blitz_engage_fx or (state.squad_blitz_engage_mx + 0.5),
                          fy = state.squad_blitz_engage_fy or (state.squad_blitz_engage_my + 0.5),
                          pmx = pmx, pmy = pmy, who = "self" }
  elseif g and (g.standoff_fx or g.standoff_mx) then
    lines[#lines + 1] = { fx = g.standoff_fx or (g.standoff_mx + 0.5),
                          fy = g.standoff_fy or (g.standoff_my + 0.5),
                          pmx = pmx, pmy = pmy, who = "self" }
  end
  -- Commander of this blitz: our squad_cmdr / the call we negotiate, or us.
  -- (A blitz we left: the commander latched in the window.)
  cmdr = cmdr or state.squad_cmdr or state.squad_negotiate_cmdr or self_pn
  local dead = state.tank_dead_at
  local function add(pn)
    local bes = ally_state.get_key(pn, "bes")
    if not bes then return end
    -- 2026-09-26: parse_bes drops a malformed or off-map spot (tonumber nil
    -- used to reach math.floor in tile_on_blitz_line). No distance bound
    -- here: line_margin keeps its box on the map, so a long line only costs
    -- more reads.
    local fx, fy = M.parse_bes(bes)
    if fx then
      lines[#lines + 1] = { fx = fx, fy = fy,
                            pmx = pmx, pmy = pmy, who = "p" .. tostring(pn) }
    end
  end
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
    if pn ~= self_pn and not is_dead
       and (pn == cmdr or tonumber(slot.info.cmdr or "") == cmdr
            or (left and slot.info.goal == "attack_pill"
                and tonumber(slot.info.target or "") == tid)) then
      add(pn)
    end
  end
  if #lines == 0 then return nil end
  return lines
end

function M.blitz_shot_lines(state, world, now, self_pn)
  if not C.PILL_PLACE_AVOID_BLITZ_LINE then return nil end
  return M.blitz_shot_lines_raw(state, world, now, self_pn)
end

-- ── Blitz window (2026-09-25 evening) ──────────────────────────────────
-- C.BLITZ_LINES_AFTER_LEAVE / C.BLITZ_NO_BUILD_ACTIVE / C.PLACE_PILL_BEHIND_ONLY.
-- M.blitz_window(state, world, now, self_pn) -> win or nil
--   win = { pill, cmdr, last, live }. live = we are in the blitz THIS tick
--   (the test blitz_shot_lines uses: an attack_pill blitz goal, or a
--   committed soldier). After we leave it the window stays open until the
--   blitz ends:
--     * the pill is dead, in a tank, off the map or ours;
--     * no live ally is on attack_pill on that pill and no call is open on it;
--     * SQUAD_BLITZ_WAIT_TIMEOUT ticks since we were last in it (backstop).
--   Kept on state._blitz_win; evaluated once per tick. squad.update calls it
--   every tick so the window latches even on ticks nothing else asks.
--   Opening / closing logs BLITZ_WINDOW.
local function _live_blitz(state)
  local g = state.goal
  if g and g.kind == "attack_pill" and not g._blitz_solo
     and (g._blitz or state.squad_negotiate_cmdr) and g.target_id then
    return g.target_id, state.squad_blitz_accepted or state.squad_cmdr or state.squad_negotiate_cmdr
  elseif state.squad_blitz_accepted and state.squad_blitz_target then
    return state.squad_blitz_target, state.squad_blitz_accepted
  end
  return nil
end

local function _pill_live(world, pid)
  local p = pid and world and world.pills and world.pills[pid]
  if not p or p.in_tank or (p.health or 0) <= 0 or not p.mx then return nil end
  if p.owner == "friendly" then return nil end
  return p
end

function M.blitz_window(state, world, now, self_pn)
  if not (C.BLITZ_LINES_AFTER_LEAVE or C.BLITZ_NO_BUILD_ACTIVE or C.PLACE_PILL_BEHIND_ONLY) then
    state._blitz_win = nil
    return nil
  end
  local win = state._blitz_win
  local pid, cmdr = _live_blitz(state)
  if pid and _pill_live(world, pid) then
    if not win then win = {}; state._blitz_win = win end
    if BRAIN_DEBUG_MODE and not (win.open and win.pill == pid) then
      print2(string.format("BLITZ_WINDOW t=%d OPEN pill=#%s cmdr=p%s", now, tostring(pid), tostring(cmdr or self_pn)))
    end
    win.pill, win.cmdr, win.last = pid, cmdr or self_pn, now
    win.live, win.open, win.tick = true, true, now
    return win
  end
  if not (win and win.open) then return nil end
  -- Left the blitz. The end test walks the allies, so run it once per tick.
  if win.tick == now and not win.live then return win end
  win.live, win.tick = false, now
  local why
  if not _pill_live(world, win.pill) then
    why = "pill dead/taken/gone"
  elseif (now - (win.last or now)) > (C.SQUAD_BLITZ_WAIT_TIMEOUT or 1500) then
    why = "timeout"
  else
    local busy = false
    local calls = state.blitz_calls
    if calls then
      for _, call in pairs(calls) do
        if call.pill == win.pill then busy = true; break end
      end
    end
    if not busy then
      local dead = state.tank_dead_at
      for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
        local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
        if pn ~= self_pn and not is_dead and slot.info.goal == "attack_pill"
           and tonumber(slot.info.target or "") == win.pill then
          busy = true
          break
        end
      end
    end
    if not busy then why = "nobody on it" end
  end
  if why then
    win.open = false
    if BRAIN_DEBUG_MODE then
      print2(string.format("BLITZ_WINDOW t=%d CLOSE pill=#%s cmdr=p%s left %dt ago: %s",
        now, tostring(win.pill), tostring(win.cmdr), now - (win.last or now), why))
    end
    return nil
  end
  return win
end

-- M.tile_on_blitz_line(world, lines, tx, ty, now) -> line or nil
--   The first line a pill on tile (tx,ty) would block. "Block" is decided by
--   the arbiter's own spot test (blitz_spot_shot_blocked, so the margin and
--   the shell path both count, as they will when the spot is re-checked): the
--   spot passes now and fails with a pill on the tile. A spot that is already
--   blocked is not blamed on the tile. A tile further than margin + 1.5 from
--   the spot->pill segment cannot block and is skipped with no shell test.
function M.tile_on_blitz_line(world, lines, tx, ty, now)
  if not lines then return nil end
  local reach = (C.BLITZ_SPOT_LOS_MARGIN or 0) + 1.5
  local key = ty * 256 + tx
  for _, ln in ipairs(lines) do
    if not (tx == ln.pmx and ty == ln.pmy)
       and not (tx == math.floor(ln.fx) and ty == math.floor(ln.fy))
       and SM.seg_square_dist(ln.pmx + 0.5, ln.pmy + 0.5, ln.fx, ln.fy, tx, ty) <= reach then
      local before = blitz_spot_shot_blocked(world, ln.fx, ln.fy, ln.pmx, ln.pmy, now, -1, true)
      if not before then
        local pend = world.pending_pill_at
        local had = pend and pend[key]
        if not pend then pend = {}; world.pending_pill_at = pend end
        pend[key] = "probe"
        local after = blitz_spot_shot_blocked(world, ln.fx, ln.fy, ln.pmx, ln.pmy, now, -1, true)
        pend[key] = had
        if next(pend) == nil then world.pending_pill_at = nil end
        if after then return ln end
      end
    end
  end
  return nil
end

function M.blitz_arbitrate(state, info, now, self_pn)
  local max_age = C.SQUAD_ALLY_MAX_AGE or 1750
  local clash   = C.SQUAD_BLITZ_CLASH_TILES or 1
  local dead    = state.tank_dead_at
  local parts = {}
  -- Standoff positions travel as FLOAT tile coords (4dp) so spot de-confliction
  -- keeps sub-tile precision instead of snapping to integer tiles. Our own
  -- engage tile -> its float CENTER; allies' come parsed from their bes float.
  -- Whether WE have our own engage spot in this arbitration. The commander must
  -- NOT accept soldiers until it does: with no own spot (e.g. still in
  -- plan_position choosing its standoff) there's nothing to de-conflict joiners
  -- against, so they'd commit on spots that clash with our eventual standoff —
  -- and a committed soldier can't repick, leaving two tanks stacked. Defer accepts.
  local have_self_spot = false
  if state.squad_blitz_engage_mx then
    have_self_spot = true
    parts[#parts + 1] = { pn = self_pn,
                          fx = state.squad_blitz_engage_fx or (state.squad_blitz_engage_mx + 0.5),
                          fy = state.squad_blitz_engage_fy or (state.squad_blitz_engage_my + 0.5),
                          bd = state.squad_blitz_bd or 0, me = true }
  elseif state.goal and (state.goal.standoff_fx or state.goal.standoff_mx) then
    have_self_spot = true
    -- Commander: it doesn't negotiate an engage spot (soldiers do) — it plans
    -- its OWN standoff (goal.standoff). Include it so soldiers de-conflict
    -- against the COMMANDER's spot too, not just against each other; otherwise a
    -- joiner can pick a standoff sitting right on top of the commander's (saw a
    -- ~0.85-tile overlap accepted). Same float source as the bes broadcast.
    parts[#parts + 1] = { pn = self_pn,
                          fx = state.goal.standoff_fx or (state.goal.standoff_mx + 0.5),
                          fy = state.goal.standoff_fy or (state.goal.standoff_my + 0.5),
                          bd = state.squad_blitz_bd or 0, me = true }
  end
  for pn, slot in ally_state.iter_active(now, max_age) do
    -- Skip a soldier that has DIED since its last broadcast: its slot lingers
    -- active for up to max_age (~35 s), so without this a dead soldier stays on
    -- the roster, holds an accept, occupies a squad slot, and gets de-conflicted
    -- against — the commander would wait on / route around a ghost.
    local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
    if pn ~= self_pn and slot.info.role == "s"
       and tonumber(slot.info.cmdr or "") == self_pn and not is_dead then
      -- 2026-09-26: a malformed, off-map or far-away bes counts as no offer
      -- (not answered, so not accepted), so it never reaches the spot test.
      local fx, fy = M.parse_bes(slot.info.bes, state.goal and state.goal.mx, state.goal and state.goal.my)
      if BRAIN_DEBUG_MODE and slot.info.bes and not fx then
        print2(string.format("BLITZ_ARB_BAD_BES t=%d p%s bes=%q pill=(%s,%s) -- not two numbers, off the map or over %d tiles from the pill; ignored",
          now, tostring(pn), tostring(slot.info.bes):sub(1, 40),
          tostring(state.goal and state.goal.mx), tostring(state.goal and state.goal.my), BES_MAX_PILL_DIST))
      end
      parts[#parts + 1] = { pn = pn, fx = fx, fy = fy,
                            bd = tonumber(slot.info.bd or ""),
                            committed = (slot.info.sub == "blitz_wait") or (slot.info.rdy == "1") }
    end
  end
  -- First-come-first-serve claim tracking: remember the tick each soldier first
  -- offered its CURRENT spot. A tank that has already settled on a spot keeps it;
  -- a later arrival on the same spot is the one told to repick. (The old rule
  -- picked by walk distance, which both churned the tank already in position and
  -- flapped on noisy bd estimates.) A soldier that repicks resets its own claim,
  -- and entries for soldiers no longer in the roster are pruned each pass.
  state._blitz_spot_since = state._blitz_spot_since or {}
  local since_tbl, live = state._blitz_spot_since, {}
  for _, p in ipairs(parts) do
    if p.fx and not p.me then
      local key = string.format("%.4f,%.4f", p.fx, p.fy)
      local rec = since_tbl[p.pn]
      if not rec or rec.key ~= key then rec = { key = key, since = now }; since_tbl[p.pn] = rec end
      p.since = rec.since
      live[p.pn] = true
    end
  end
  for pn in pairs(since_tbl) do if not live[pn] then since_tbl[pn] = nil end end

  local reject = {}
  for i = 1, #parts do
    for j = i + 1, #parts do
      local a, b = parts[i], parts[j]
      if a.fx and b.fx then
        local ddx, ddy = a.fx - b.fx, a.fy - b.fy
        if math.sqrt(ddx * ddx + ddy * ddy) <= clash then  -- euclidean float distance
          local loser
          -- The commander (me) never repicks its own standoff — the soldier
          -- yields. Otherwise first-come-first-serve: a soldier already IN
          -- POSITION (blitz_wait / aimed) outranks one still approaching, and
          -- among equal commitment whoever claimed the spot earlier keeps it.
          -- Never make a settled tank move — that's pure churn.
          if a.me then loser = b
          elseif b.me then loser = a
          else
            local ac, bc = a.committed and true or false, b.committed and true or false
            if ac ~= bc then
              loser = ac and b or a                      -- the one not yet in position repicks
            else
              local as, bs = a.since or now, b.since or now
              if as ~= bs then loser = (as > bs) and a or b   -- later claimant repicks
              else loser = (a.pn > b.pn) and a or b end       -- stable final tiebreak
            end
          end
          reject[loser.pn] = true
          if BRAIN_DEBUG_MODE then local _w = (loser == a) and b or a; print2(string.format("BLITZ_ARB_REJECT t=%d loser=p%s reason=clash vs=p%s dist=%.2f<=%.2f loser_spot=(%.2f,%.2f)", now, tostring(loser.pn), tostring(_w.pn), math.sqrt(ddx * ddx + ddy * ddy), clash, loser.fx, loser.fy)) end
        end
      end
    end
  end
  -- Invalid-spot reject: a soldier's spot whose shot to the pill CENTER is
  -- blocked by a wall (a blocker we built) can't hit the pill — reject it so
  -- the soldier repicks a clear one.
  local gmx = state.goal and state.goal.mx
  local gmy = state.goal and state.goal.my
  if gmx then
    for _, p in ipairs(parts) do
      if p.fx and p.pn ~= self_pn and not reject[p.pn]
         and blitz_spot_shot_blocked(state.world, p.fx, p.fy, gmx, gmy, now, p.pn) then
        reject[p.pn] = true
        if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_ARB_REJECT t=%d loser=p%s reason=shot_blocked spot=(%.2f,%.2f) pill=(%d,%d) margin=%.2f (our shield walls / a pill block the soldier's line%s)", now, tostring(p.pn), p.fx, p.fy, gmx, gmy, C.BLITZ_SPOT_LOS_MARGIN or 0, (C.BLITZ_SPOT_LOS_MARGIN or 0) > 0 and ", or fail the LOS margin: see BLITZ_SPOT_MARGIN_REJECT" or "")) end
      end
    end
  end
  if reject[self_pn] then state._blitz_call_rejected = true end

  -- COUNT CAP: a commander accepts at most SQUAD_MAX_SIZE soldiers (commander +
  -- cap = the per-pill tank cap). Standoff de-confliction above can still leave
  -- MORE than cap conflict-free soldiers, each on a distinct valid spot — and
  -- without this the commander accepted them all (observed bac=0,5 -> 3 tanks on
  -- one pill on a laggy DS, because the soldier-side squad_full gate read stale
  -- broadcasts). The commander has every offer locally each tick, so capping
  -- here is deterministic and lag-proof. Keep already-committed soldiers
  -- (blitz_wait/rdy) so we never shed one mid-take, then earliest claimant, then
  -- lowest pn; reject the surplus so they peel off to another target.
  do
    local cap = M.blitz_soldier_cap()   -- party MAX minus the commander
    local cands = {}
    for _, p in ipairs(parts) do
      if p.pn ~= self_pn and p.fx and not reject[p.pn] then cands[#cands + 1] = p end
    end
    if #cands > cap then
      table.sort(cands, function(a, b)
        local ac, bc = a.committed and 1 or 0, b.committed and 1 or 0
        if ac ~= bc then return ac > bc end                  -- committed first
        local as, bs = a.since or now, b.since or now
        if as ~= bs then return as < bs end                  -- earliest claim
        return a.pn < b.pn                                   -- stable
      end)
      for i = cap + 1, #cands do reject[cands[i].pn] = true end  -- surplus -> reject
    end
  end

  local roster, rlist, alist = {}, {}, {}
  for _, p in ipairs(parts) do
    if p.pn ~= self_pn then
      local rj = reject[p.pn] and true or false
      roster[#roster + 1] = { pn = p.pn, fx = p.fx, fy = p.fy, bd = p.bd,
                              rejected = rj, answered = (p.fx ~= nil) }
      -- Tag the rejected SPOT as "pn:[fx,fy]" (4dp floats) so the soldier only
      -- repicks when we rejected the spot it is CURRENTLY offering (not a stale
      -- earlier one) — what makes a 1-tick repick gap safe (no list burn).
      -- Entries are ';'-separated, e.g. "3:[114.5000,141.5000];5:[130.5000,132.5000]".
      if rj then rlist[#rlist + 1] = p.fx and string.format("%d:[%.4f,%.4f]", p.pn, p.fx, p.fy) or tostring(p.pn)
      elseif p.fx and have_self_spot then alist[#alist + 1] = p.pn end   -- answered + conflict-free + WE have our own spot = ACCEPTED
    end
  end
  state.squad_blitz_roster = roster
  state.squad_blitz_reject = (#rlist > 0) and table.concat(rlist, ";") or nil
  state.squad_blitz_accept = (#alist > 0) and table.concat(alist, ",") or nil
end

-- If our commander's brj reject list names us, flag a repick (closer tank
-- yields). SPOT-AWARE: each reject is tagged "pn:[fx,fy]" (the 4dp float spot
-- the commander evaluated). We only repick when the reject targets the spot
-- we're STILL offering — a reject for a different spot is stale (we already
-- moved off it), and acting on it would burn the candidate list a tick at a
-- time. This is what lets SQUAD_BLITZ_REPICK_GAP be 1 without thrashing.
local function read_cmdr_brj(state, self_pn)
  if not state.squad_cmdr then return end
  local brj = ally_state.get_key(state.squad_cmdr, "brj")
  if not brj then return end
  -- The spot we actually broadcast as bes: the fix-A float point when one is
  -- stored (C.BLITZ_SPOT_EXACT_ORIGIN), else the tile centre.
  local cur_fx = state.squad_blitz_engage_mx and (state.squad_blitz_engage_fx or (state.squad_blitz_engage_mx + 0.5)) or nil
  local cur_fy = state.squad_blitz_engage_my and (state.squad_blitz_engage_fy or (state.squad_blitz_engage_my + 0.5)) or nil
  for entry in string.gmatch(brj, "[^;]+") do
    local pn, fx, fy = entry:match("^(%d+):%[(%-?[%d.]+),(%-?[%d.]+)%]$")
    if pn then
      if tonumber(pn) == self_pn then
        -- Spot-tagged reject: fresh only if it matches our current offer (small
        -- epsilon — the spot round-trips as a 4dp float string).
        local match = cur_fx and (math.abs(tonumber(fx) - cur_fx) < 0.01 and math.abs(tonumber(fy) - cur_fy) < 0.01)
        if (not cur_fx) or match then
          state._blitz_call_rejected = true
          if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_BRJ_RX t=%d C%s rejected our spot (%s,%s); cur=(%s,%s) match=%s -> repick", state.tick or 0, tostring(state.squad_cmdr), tostring(fx), tostring(fy), tostring(cur_fx), tostring(cur_fy), tostring(match and true or false))) end
        elseif BRAIN_DEBUG_MODE then
          print2(string.format("BLITZ_BRJ_RX t=%d C%s rejected STALE spot (%s,%s) but we now offer (%s,%s) — ignored", state.tick or 0, tostring(state.squad_cmdr), tostring(fx), tostring(fy), tostring(cur_fx), tostring(cur_fy)))
        end
        return
      end
    elseif tonumber(entry) == self_pn then
      -- Bare pn (no spot): legacy / no-offer reject → always repick.
      state._blitz_call_rejected = true
      return
    end
  end
end

M._read_cmdr_brj = read_cmdr_brj   -- unit tests

-- ── Budget-killed squad.update guard ──────────────────────────────────────
-- update_body (below) clears its per-tick outputs (squad_cmdr,
-- squad_blitz_target, ...) part-way through and re-derives them further down.
-- The host can kill a think at its budget ANYWHERE, so a kill between the clear
-- and the re-derive leaves them nil. squad.update runs near the END of think,
-- but the goal logic that reads these fields runs EARLIER in the next tick, so
-- that next tick sees "no commander". 20260924_211112_1__2v2_decoy bot5:
-- killed tick t=2892, then at t=2893 its blitz_wait soldier branch read
-- squad_cmdr == nil and dropped a live blitz ("blitz_wait: commander
-- gone/retargeted/dead") while C6 was still waiting on it; squad.update at
-- the end of t=2893 re-set squad_cmdr = 6 (BLITZ_COMMITTED), too late.
-- M.update snapshots these fields on entry and marks the update open; a normal
-- return closes it. M.recover_killed_update (called at the top of think, before
-- any reader) finds an update still open, which can only mean the last one was
-- killed, and puts the snapshot back: the values the last COMPLETE update left.
-- Knob C.SQUAD_KILL_RECOVER (KEEL false): off = no snapshot, no restore.
local _TICK_FIELDS = {
  "squad_cmdr", "squad_help_target", "squad_blitz_target", "squad_status",
  "squad_negotiate_cmdr", "squad_negotiate_pill", "squad_role",
  "_blitz_call_rejected", "_blitz_switch_to",
}
local _N_TICK_FIELDS = #_TICK_FIELDS
local update_body

function M.update(state, info, now, world)
  if not C.SQUAD_KILL_RECOVER then
    local r0 = update_body(state, info, now, world)
    M.blitz_window(state, world, now, info and info.player_number or -1)
    return r0
  end
  local snap = state._squad_upd_snap
  if not snap then snap = {}; state._squad_upd_snap = snap end
  for i = 1, _N_TICK_FIELDS do
    local k = _TICK_FIELDS[i]
    snap[k] = state[k]
  end
  snap.tick = now
  state._squad_upd_open = true
  local r = update_body(state, info, now, world)
  state._squad_upd_open = nil
  -- Latch / expire the blitz window (C.BLITZ_LINES_AFTER_LEAVE and friends)
  -- every tick, after this tick's squad fields are final.
  M.blitz_window(state, world, now, info and info.player_number or -1)
  return r
end

function M.recover_killed_update(state, now)
  if not C.SQUAD_KILL_RECOVER then return end
  if not state._squad_upd_open then return end
  state._squad_upd_open = nil
  local snap = state._squad_upd_snap
  if not snap then return end
  for i = 1, _N_TICK_FIELDS do
    local k = _TICK_FIELDS[i]
    state[k] = snap[k]
  end
  if BRAIN_DEBUG_MODE then
    print2(string.format("SQUAD_UPDATE_KILLED t=%d killed_tick=%s -> restored last complete update: role=%s cmdr=%s blitz_target=%s status=%s",
      now, tostring(snap.tick), tostring(state.squad_role), tostring(state.squad_cmdr),
      tostring(state.squad_blitz_target), tostring(state.squad_status)))
  end
end

-- Pending pills (C.BLITZ_SPOT_PENDING_PILLS, 2026-09-25): tiles where a pill
-- WILL stand in a few seconds, so the blitz spot tests (spot_margin
-- world_blocker / aim_line_trees, blitz_clear_aim, the arbiter's legacy loop,
-- the standoff sanity check) block on them like a live pill. Sources:
--   own   our man is out on a pill build/repair dispatch (_lgm_dispatch.pbox)
--   own   our goal is place_pill_strategic and we carry a pill (the target)
--   ally  an ally's lgmd advert ends in "P" (its man is out on a pill job)
-- Allies do NOT broadcast a place_pill_strategic target before the man
-- leaves, so an ally's pill counts only from its dispatch on.
-- Writes world.pending_pill_at (packed y*256+x -> source label), or nil when
-- the knob is off (KEEL) or no tile is pending.
function M.update_pending_pills(state, info, now, world)
  if not world then return end
  if not C.BLITZ_SPOT_PENDING_PILLS then world.pending_pill_at = nil; return end
  local set = nil
  local ld = state._lgm_dispatch
  if ld and ld.pbox and ld.x and ld.y and info.man_status ~= C.LGM_INTANK then
    set = set or {}
    set[ld.y * 256 + ld.x] = "own_lgm"
  end
  local g = state.goal
  if g and g.kind == "place_pill_strategic" and g.mx and g.my
     and (info.carried_pills or 0) > 0 then
    set = set or {}
    local k = g.my * 256 + g.mx
    if not set[k] then set[k] = "own_place" end
  end
  local self_pn = info.player_number
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= self_pn then
      local h = slot.info
      local al = h and h.lgmd
      if al and #al >= 9 and string.sub(al, 9, 9) == "P" then
        local lx = tonumber(string.sub(al, 1, 2), 16)
        local ly = tonumber(string.sub(al, 3, 4), 16)
        if lx and ly then
          set = set or {}
          local k = ly * 256 + lx
          if not set[k] then set[k] = "ally_p" .. tostring(pn) end
        end
      end
    end
  end
  world.pending_pill_at = set
  if BRAIN_DEBUG_MODE then
    local sig = ""
    if set then
      local keys = {}
      for k, src in pairs(set) do keys[#keys + 1] = string.format("(%d,%d)%s", k % 256, math.floor(k / 256), src) end
      table.sort(keys)
      sig = table.concat(keys, " ")
    end
    if sig ~= (state._pending_pill_sig or "") then
      state._pending_pill_sig = sig
      print2(string.format("PENDING_PILLS t=%d %s", now, sig == "" and "none" or sig))
    end
  end
end

update_body = function(state, info, now, world)
  M.update_pending_pills(state, info, now, world)
  local pns, self_pn = protocol_pns(state, info, now)
  state.squad_pns  = pns

  -- Blitz-call registry maintenance: drop calls whose commander went inactive
  -- (stale/dead — ran no Lua to send a close), or whose latest broadcast no
  -- longer shows that attack_pill (deduced close). Explicit bcc is handled in
  -- comms.process_message; this catches the rest.
  if state.blitz_calls then
    local max_age = C.SQUAD_ALLY_MAX_AGE or 1750
    for cmdr, call in pairs(state.blitz_calls) do
      local slot = ally_state.get(cmdr)
      local active = slot and slot.active and (now - (slot.last_tick or 0)) <= max_age
      -- Only let the ally's broadcast goal/substate CLOSE the call when that
      -- broadcast is NEWER than the bco we registered. /info state is sparse
      -- (event-driven + 30s heartbeat) and lossy, so a commander on a long
      -- approach frequently has a slot whose last goal PREDATES its bco (seen:
      -- a stale refuel_at_base while it was really blitzing pill 15). Trusting
      -- that stale slot pruned a just-opened call the same tick it arrived, and
      -- since bco is open-once it never came back. Trust the explicit bco until
      -- a NEWER state contradicts it. Explicit bcc, commander-gone (not active),
      -- death, and age-out still close the call.
      --
      -- Use state_tick (when the GOAL/TARGET were last refreshed via full /info
      -- state) — NOT last_tick, which /info extra heartbeats bump WITHOUT
      -- carrying goal/target. A blitzing commander streams /info extra (its
      -- standoff fields) every tick, so last_tick stays current while its goal
      -- field can be a stale pre-blitz value (saw capture_base linger while it
      -- was really blitzing); that made slot_fresh wrongly true and pruned a
      -- live call → the bco, being open-once, never re-registered.
      local slot_fresh = slot and slot.state_tick and call.tick
                         and slot.state_tick > call.tick
      local goal_ok = active and slot.info and slot.info.goal == "attack_pill"
                      and tonumber(slot.info.target or "") == call.pill
      -- Substate-based close: once the commander's broadcast substate leaves the
      -- open/pre-commit set (it's engaging/firing — aim/in_range/shoot/charge/…),
      -- the call is CLOSED even if its explicit bcc was dropped. Only act when a
      -- substate is actually present (nil = no slate yet → keep the call).
      local sub = goal_ok and slot.info.sub or nil
      local sub_closed = sub and not M.BLITZ_CALL_OPEN_SUB[sub]
      local goal_contradicts = slot_fresh and not goal_ok   -- only if slot newer than bco
      local sub_says_closed  = slot_fresh and sub_closed
      -- Commander GONE → the call dies with it, so the pill reverts to a regular
      -- (un-blitzed) target. Disconnect/leave-game is caught by `not active`
      -- (slot goes inactive); death is caught here via tank_dead_at, since a
      -- just-killed commander's last broadcast can still look active+on-pill for
      -- up to the ally-expiry window.
      local dead = state.tank_dead_at and state.tank_dead_at[cmdr]
                   and slot and state.tank_dead_at[cmdr] > (slot.last_tick or 0)
      if cmdr == self_pn or not active or goal_contradicts or sub_says_closed or dead then
        if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_PRUNE t=%d C%s active=%s fresh=%s ally_goal=%s ally_tgt=%s sub=%s sub_closed=%s dead=%s call_pill=%s", now, tostring(cmdr), tostring(active), tostring(slot_fresh), slot and slot.info and tostring(slot.info.goal) or "nil", slot and slot.info and tostring(slot.info.target) or "nil", tostring(sub), tostring(sub_closed), tostring(dead), tostring(call.pill))) end
        state.blitz_calls[cmdr] = nil
      end
    end
  end

  -- Joinable-blitz pills: open blitz calls (state.blitz_calls) whose pill is in
  -- our help range and not in the no-spot reject window. Keyed by pill id ->
  -- {mx,my}. Two consumers: (1) goals.lua EXEMPTS these from ally_claimed
  -- de-confliction — the whole point of a blitz is a second tank joining, so we
  -- must NOT be steered off a pill an ally is blitzing; (2) the blitz_join_hl
  -- viz borders them. This is REAL-GAME logic (the exemption needs it), but the
  -- table is left nil unless there's actually an in-range open call — the common
  -- no-blitz case does zero work / zero allocation. Both consumers nil-check.
  do
    local joinable = nil
    if state.blitz_calls and world and world.pills then
      local pill_rej = state._blitz_pill_reject
      for _, call in pairs(state.blitz_calls) do
        local pp  = call.pill and world.pills[call.pill] or nil
        local rej = pill_rej and pill_rej[call.pill]
        -- No distance gate: any live open call is joinable. The join DISCOUNT
        -- (blitz_join_factor, exponential, zeroes by FULL_TILES) is what scales
        -- by distance, so the score decides who actually joins — a far call is
        -- "joinable" but gets no discount and won't be picked over closer goals.
        if pp and (pp.health or 0) > 0 and not (rej and now < rej) then
          joinable = joinable or {}
          joinable[call.pill] = { mx = pp.mx, my = pp.my }
        end
      end
    end
    state.squad_joinable_pills = joinable
  end

  local role = M.role_for(pns, self_pn)
  -- Harasser is an independent flag now (not a role), so it does NOT gate squad
  -- membership — a harasser commands/joins blitzes like any other bot. It only
  -- biases goal costs (see goals.lua is_harasser checks).
  state._harasser_frac = harasser_frac(state)
  -- One designation, two possible roles. is_harasser picks the slate; the map
  -- decides whether that slate is harassers (normal maps) or pill_suiciders
  -- (C.PILL_SUICIDER_MAPS). The two flags are mutually exclusive — a suicider
  -- must NOT also carry the harasser cost biases (×5 pill / ×0.2 travel), which
  -- are the opposite of what a suicider wants.
  local _designated = M.is_harasser(pns, self_pn, state)
  state._pill_suicider_map = M.is_pill_suicider_map(info)
  -- A PER-BOT FORCE beats the slate entirely (state.force_pill_suicider,
  -- set from the BRAIN_INIT_ARG "suicider"/"nosuicider" tokens — see
  -- init.lua — which a scenario stages through game.spawn_bot's init
  -- argument). Forced ON makes this bot a suicider whatever the slate said
  -- and whatever map this is: the whole point is a scenario fielding one
  -- wave of nothing but suiciders, which the fractional slate can't
  -- express, so it must not depend on C.PILL_SUICIDER_MAPS either. Forced
  -- OFF only bars the suicider role; the bot still takes the ordinary
  -- harasser slate if it was designated (that is what lets a scenario run
  -- a plain wave on a suicider map).
  -- ── Blitz-designated suicider (TEMPORARY) ────────────────────────────────
  -- Set by a commander's "bsu" broadcast (see comms.lua) when it tops its blitz
  -- up to BLITZ_MIN_SUICIDERS. It lasts only as long as THAT blitz: the pill
  -- dying or turning ours, this bot leaving the take, the commander going
  -- silent/dying, our own death (reset_blitz_state wipes it) or the
  -- BLITZ_SUICIDER_MAX_TICKS backstop all end it and the bot reverts to
  -- whatever it was. Resolved BEFORE the flags below so the precedence is
  -- explicit: permanent force > blitz temp > slate.
  if state.blitz_suicider then
    local bs   = state.blitz_suicider
    local why  = nil
    local p    = world and world.pills and world.pills[bs.pill]
    if state.blitz_disabled or not C.BLITZ_ENABLED then
      -- "noblitz" / BLITZ_ENABLED=false: we take no part in blitzes, so we hold no designation either
      -- (comms.lua already ignores incoming bsu; this drops any that predates
      -- the flag).
      why = "noblitz"
    elseif not p or (p.health or 0) <= 0 or p.owner == "friendly" or p.owner == "allied" then
      why = "pill_gone"
    elseif (now - (bs.since or now)) > (C.BLITZ_SUICIDER_MAX_TICKS or 1600) then
      why = "timeout"
    else
      local g = state.goal
      local on_it = g and (g.kind == "attack_pill" or g.kind == "capture_pill")
                    and g.target_id == bs.pill
      -- Short grace: the designation can land a tick or two before pick_goal
      -- has adopted the blitz pill, and reverting on that would undo it
      -- immediately. After the grace, being off the take really does end it.
      if not on_it and (now - (bs.since or now)) > 50 then why = "left_take" end
    end
    -- bs.by == self_pn means WE designated ourselves (blitz_designate_suiciders
    -- couldn't make the minimum from the soldiers). Skip the commander-gone
    -- test in that case: we are the commander, and we never have an
    -- ally_state slot of our own, so the test would cancel it instantly.
    if not why and bs.by and bs.by ~= self_pn then
      local cs      = ally_state.get(bs.by)
      local cactive = cs and cs.active and (now - (cs.last_tick or 0)) <= (C.SQUAD_ALLY_MAX_AGE or 1750)
      local cdead   = state.tank_dead_at and state.tank_dead_at[bs.by]
                      and cs and state.tank_dead_at[bs.by] > (cs.last_tick or 0)
      if not cactive or cdead then why = "commander_gone" end
    end
    if why then
      state.blitz_suicider    = nil
      state._blitz_su_end_why = why
    end
  end

  if state.force_pill_suicider ~= nil then
    state.is_pill_suicider = state.force_pill_suicider
    -- The two flags stay mutually exclusive — a suicider must NOT also
    -- carry the harasser cost biases (×5 pill / ×0.2 travel), which are the
    -- opposite of what a suicider wants.
    state.is_harasser = (not state.force_pill_suicider) and _designated or false
    state.suicider_src = state.force_pill_suicider and "forced" or nil
  elseif state.blitz_suicider then
    state.is_pill_suicider = true
    state.is_harasser = false
    -- Name WHICH blitz rule designated us: a contested take
    -- (BLITZ_CONTESTED_SUICIDERS) or the BLITZ_MIN_SUICIDERS quota top-up.
    -- Display only — the designation and its expiry are identical either way.
    state.suicider_src = (state.blitz_suicider.why == "contested")
                         and "blitz_contested" or "blitz"
  else
    state.is_pill_suicider = _designated and state._pill_suicider_map or false
    state.is_harasser = _designated and not state._pill_suicider_map
    state.suicider_src = state.is_pill_suicider and "slate" or nil
  end
  -- One line per bot the first time the designation resolves, and again on
  -- any change: the [harass] line below only fires while the dynamic ramp
  -- has pushed the fraction above its floor, so it is no use for reading
  -- back what a given bot actually IS. Debug-only, so it costs production
  -- nothing (lua_strip drops it from opt/).
  if BRAIN_DEBUG_MODE and state._psu_said ~= state.is_pill_suicider then
    state._psu_said = state.is_pill_suicider
    -- src says WHICH rule made it true (forced token / blitz designation /
    -- harasser slate); on a blitz designation the pill and the commander that
    -- sent it, and on the way back down the reason the designation ended.
    local extra = ""
    local bs = state.blitz_suicider
    if bs then
      extra = string.format(" blitz_suicider=true pill=#%s by=p%s reason=%s",
                            tostring(bs.pill), tostring(bs.by),
                            (bs.why == "contested") and "blitz_contested" or "blitz_quota")
    elseif state._blitz_su_end_why then
      extra = string.format(" blitz_suicider=false reason=%s", state._blitz_su_end_why)
      state._blitz_su_end_why = nil
    end
    print2(string.format(
      "[role] t=%d suicider=%s src=%s harasser=%s forced=%s suicider_map=%s%s",
      state.tick or 0, tostring(state.is_pill_suicider),
      tostring(state.suicider_src or "-"),
      tostring(state.is_harasser), tostring(state.force_pill_suicider),
      tostring(state._pill_suicider_map), extra))
  end
  if BRAIN_DEBUG_MODE and state._harasser_frac > (C.HARASSER_FRAC or 0.20) + 0.001 then print2(string.format("[harass] t=%d frac=%.2f base=%.2f pill=%.2f n_har=%d/%d role=%s forced=%s", state.tick or 0, state._harasser_frac, state.base_strength or 0, state.strength or 0, math.floor(state._harasser_frac * #pns), #pns, state.is_pill_suicider and "pill_suicider" or (state.is_harasser and "harasser" or "-"), tostring(state.force_pill_suicider))) end
  -- R0 (dynamic commanders, flag-gated): commander status is EMERGENT — you are a
  -- commander only while leading a HARD pill take (your attack_pill target has HP
  -- >= HARD_TAKE_MIN_HP); otherwise you are a soldier. Reverts automatically when
  -- the take ends. Off by default → original deterministic roles.
  if C.DYNAMIC_COMMANDERS then
    -- A bot already following another commander (squad_cmdr held from last tick,
    -- before the reset below) stays a SOLDIER even though it adopted the same
    -- attack_pill as the blitz target — otherwise every squad member would turn
    -- into a commander and the squad would collapse.
    if state.squad_cmdr then
      role = M.ROLE_SOLDIER
    else
      local g = state.goal
      local is_hard_take = false
      local pill_hp = 0
      if g and g.kind == "attack_pill" and g.target_id and world and world.pills then
        local p = world.pills[g.target_id]
        if p then
          pill_hp = p.health or 0
          if pill_hp >= (C.HARD_TAKE_MIN_HP or 12) then is_hard_take = true end
          -- Blitz-only: a soft pill (under HARD_TAKE_MIN_HP) can only be taken
          -- by a blitz too, so any LIVE pill makes us its commander.
          if pill_hp > 0 and M.blitz_only(state) then is_hard_take = true end
        end
      end
      -- An established leader stays commander even after the pill's HP falls
      -- below the hard-take threshold: if we already hold an open blitz call on
      -- this target (_my_blitz_call) or we've committed to firing
      -- (_blitz_committed), we ARE the leader, not a fresh elector. Without this
      -- a solo blitzer that wears a pill down to <12 HP on a second pass gets
      -- demoted to soldier mid-take, then the blitz_wait soldier branch finds no
      -- commander above it and aborts with "commander gone" (it WAS the leader).
      -- ...OR we still hold the sticky commander latch on this pill (set at the
      -- end of any tick we were elected its commander, cleared only on a real
      -- end). This is what makes "first to initiate STAYS commander until a real
      -- reason ends the take": HP dropping below the hard-take threshold or a
      -- recruiting call closing during approach no longer demotes an established
      -- leader. The deferral ladder below is still the ONLY thing that can hand
      -- command to an earlier / tie-lower-pn rival — and that demotion clears the
      -- latch (a genuine yield), so it doesn't fight this.
      local is_leading = (g and g.target_id and
                          (state._my_blitz_call == g.target_id or g._blitz_committed
                           or state.squad_commander_pill == g.target_id))
      role = (is_hard_take or is_leading) and M.ROLE_COMMANDER or M.ROLE_SOLDIER
      -- Armour gate on FRESH command: a weak tank (< SQUAD_COMMANDER_MIN_ARMOUR)
      -- may JOIN a blitz but not OPEN/lead one. Established leaders (is_leading)
      -- keep command even if their armour later drops — don't abandon mid-take.
      if role == M.ROLE_COMMANDER and not is_leading
         and (info.armour or 0) < (C.SQUAD_COMMANDER_MIN_ARMOUR or 30) then
        role = M.ROLE_SOLDIER
      end
      -- Ammo-deprived: a tank starved of ammo can't finish a pill, so it must
      -- NEVER open or hold a blitz — it can still JOIN one as a soldier (suicide
      -- body), just never lead. Unlike the armour gate this has no is_leading
      -- exemption: deprivation means any take it's "leading" has stalled, so it
      -- hands command off (the latch clears below since role lands soldier).
      if state.ammo_deprived and role == M.ROLE_COMMANDER then
        role = M.ROLE_SOLDIER
        if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_NO_CMD t=%d ammo_deprived -> soldier (pill=%s)", state.tick or 0, g and tostring(g.target_id) or "?")) end
      end
      -- Ammo gate on FRESH command (INITIATION): a tank that can't finish the
      -- pill by itself (shells <= pill HP) must not OPEN/lead a blitz — a dry /
      -- low-ammo would-be commander can't shoot the pill down, so it just parks
      -- in plan_position re-planning a take it can never complete. It can still
      -- JOIN one as a soldier (decoy body). Fires immediately on current ammo
      -- (unlike the 60s ammo_deprived flag above). Established leaders
      -- (is_leading) are exempt — don't collapse a working multi-tank take
      -- mid-fight; a leader that then stays starved is caught by the gate above.
      if role == M.ROLE_COMMANDER and not is_leading
         and (info.shells or 0) <= pill_hp then
        role = M.ROLE_SOLDIER
        if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_NO_CMD t=%d low_ammo(sh=%d<=hp=%d) -> soldier (pill=%s)", state.tick or 0, info.shells or 0, pill_hp, g and tostring(g.target_id) or "?")) end
      end
      -- AHEAD_BLITZ_ONLY (difficulty gate): only OPEN a blitz call while our team
      -- is ahead. When behind, a FRESH would-be commander stays a SOLDIER so it
      -- doesn't initiate a gang-up -- but it can still ANSWER someone else's call
      -- via M.availability. Established leaders (is_leading) are exempt so an
      -- in-progress take isn't collapsed if the lead slips mid-fight. Default
      -- false (and team_ahead defaults true) -> no-op, Hard unchanged.
      if C.AHEAD_BLITZ_ONLY and not state.team_ahead
         and role == M.ROLE_COMMANDER and not is_leading then
        role = M.ROLE_SOLDIER
        if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_NO_CMD t=%d ahead_only(behind) -> soldier (pill=%s)", state.tick or 0, g and tostring(g.target_id) or "?")) end
      end
      -- Don't elect a SECOND commander of a pill an ally is already blitzing:
      -- FIRST TO THE TAKE WINS. If a live blitz call on OUR target has been open
      -- LONGER than ours (the ally committed first), defer to it and become a
      -- SOLDIER so we JOIN instead of running a rival blitz. A literal same-tick
      -- race (both opened within SQUAD_CMD_RACE_TOL ticks — accounts for the
      -- 1-tick broadcast latency) is the ONLY case broken by lower player id.
      -- Our own call's open tick is _my_blitz_call_tick (nil = we're electing this
      -- tick → age 0); the ally's is its first-seen tick (preserved across
      -- re-announces). blitz_calls is already pruned of stale/engaging calls.
      --
      -- A COMMITTED blitz (goal._blitz_committed, set by commit_fire when we left
      -- blitz_wait to fire) is LOCKED — we are the established leader, not a fresh
      -- elector. Never defer once committed: our recruiting bco has closed (firing
      -- phase isn't an open-sub), so _my_blitz_call cleared and my_age would
      -- collapse to 0, making the deferral mistake us for a brand-new joiner and
      -- demote us to a soldier of a LATECOMER (saw it: an in_range commander
      -- handed its take to an ally that opened a rival call ~1000 ticks later).
      if role == M.ROLE_COMMANDER and g and g.target_id and state.blitz_calls
         and not g._blitz_committed then
        local now2 = state.tick or 0
        local tol  = C.SQUAD_CMD_RACE_TOL or 3
        -- "electing" = still in the open ELECTION stage (plan_position). Once we
        -- advance into our take (approach/gather_trees/build_walls/...) we're the
        -- established captain: a same-tick-tie or lower-pn LATECOMER must JOIN us,
        -- not bump us to a soldier. (A rival that is genuinely FURTHER along still
        -- wins, just below — first to build, regardless of pn.)
        local electing = (g.substate == nil or g.substate == "plan_position")

        -- Progress beats the pn/age tiebreak: defer to an ally commanding our pill
        -- who is FURTHER into the take than us (e.g. it reached gather_trees /
        -- build_walls while we're still at plan_position). First-to-build is the
        -- captain; we join as a soldier. This is the half that makes the OTHER bot
        -- yield to an established builder, so we don't end up with two commanders.
        do
          local my_rank = blitz_rank(g.substate)
          local dead = state.tank_dead_at
          for ally_pn, slot in ally_state.iter_active(now2, C.SQUAD_ALLY_MAX_AGE or 1750) do
            if ally_pn ~= self_pn and slot.info
               and slot.info.role == M.ROLE_COMMANDER
               and slot.info.goal == "attack_pill"
               and tonumber(slot.info.target or "") == g.target_id
               and blitz_rank(slot.info.sub) > my_rank
               and not (dead and dead[ally_pn] and dead[ally_pn] > (slot.last_tick or 0)) then
              role = M.ROLE_SOLDIER
              if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_DEFER t=%d pill=%s -> ally p%s further along (sub=%s); join, don't rival-command", now2, tostring(g.target_id), tostring(ally_pn), tostring(slot.info.sub))) end
              break
            end
          end
        end

        local my_open = (state._my_blitz_call == g.target_id and state._my_blitz_call_tick) or now2
        local my_age  = now2 - my_open
        for cmdr, call in pairs(state.blitz_calls) do
          if role == M.ROLE_COMMANDER and cmdr ~= self_pn and call.pill == g.target_id then
            local ally_age = now2 - (call.tick or now2)
            -- defer if the ally has been on it clearly longer, OR it's a ~tie and
            -- they hold the lower player id. The lower-pn TIEBREAK only applies
            -- while we're still electing — an established builder doesn't yield to
            -- a same-tick-tie latecomer.
            if (ally_age - my_age) > tol
               or (electing and math.abs(ally_age - my_age) <= tol and cmdr < self_pn) then
              role = M.ROLE_SOLDIER
              if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_DEFER t=%d pill=%s -> join C%s (ally_age=%d my_age=%d) instead of commanding", now2, tostring(g.target_id), tostring(cmdr), ally_age, my_age)) end
              break
            end
          end
        end
        -- Also defer to an ally already PAST RECRUITING on our pill — its blitz
        -- left the open set (BLITZ_CALL_OPEN_SUB), so its recruiting call closed
        -- and it's gone from blitz_calls above, but it's still the established
        -- taker: broadcasting attack_pill on our target in a committed substate
        -- (firing OR winding down — aim/in_range/shoot/charge/engage/swerve/
        -- kill_hardline, and disengage/post_engage/curve_away/loiter). Don't run
        -- a rival blitz on a pill someone already owns. We become a SOLDIER with
        -- no open call to join, so the pill reads as plain ally_claimed in our
        -- pool (no blitz exemption) and we drop it / re-plan — instead of
        -- promoting to a second commander. (sub=nil = no slate yet → don't defer.)
        if role == M.ROLE_COMMANDER then
          local dead = state.tank_dead_at
          for ally_pn, slot in ally_state.iter_active(now2, C.SQUAD_ALLY_MAX_AGE or 1750) do
            if ally_pn ~= self_pn and slot.info
               and slot.info.goal == "attack_pill"
               and tonumber(slot.info.target or "") == g.target_id
               and slot.info.sub and not M.BLITZ_CALL_OPEN_SUB[slot.info.sub]
               and not (dead and dead[ally_pn] and dead[ally_pn] > (slot.last_tick or 0)) then
              role = M.ROLE_SOLDIER
              if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_DEFER t=%d pill=%s -> ally p%s committed (sub=%s, past recruiting); drop, don't rival-command", now2, tostring(g.target_id), tostring(ally_pn), tostring(slot.info.sub))) end
              break
            end
          end
        end
        -- BACKSTOP: defer to an ally commanding our pill in an OPEN (recruiting)
        -- substate even when its call is ABSENT from blitz_calls. Path-1 (above)
        -- needs the registry entry; path-2 needs a committed substate. A call can
        -- be missing yet the take live — a dropped/never-heard one-shot bco, or we
        -- elected after it was sent — leaving an ally broadcasting role=c
        -- attack_pill on our pill in approach/plan_position with nothing for the
        -- other paths to catch (saw two rival commanders run ~650 ticks until the
        -- ally finally hit a committed sub). Tiebreak is deterministic LOWER pn so
        -- both bots agree on exactly one commander; the committed latch above
        -- (g._blitz_committed) already shields an established leader from being
        -- demoted to a fresh elector, so this only fires pre-commit.
        if role == M.ROLE_COMMANDER and electing then
          local dead = state.tank_dead_at
          for ally_pn, slot in ally_state.iter_active(now2, C.SQUAD_ALLY_MAX_AGE or 1750) do
            if ally_pn ~= self_pn and ally_pn < self_pn and slot.info
               and slot.info.role == M.ROLE_COMMANDER
               and slot.info.goal == "attack_pill"
               and tonumber(slot.info.target or "") == g.target_id
               and slot.info.sub and M.BLITZ_CALL_OPEN_SUB[slot.info.sub]
               and not (dead and dead[ally_pn] and dead[ally_pn] > (slot.last_tick or 0)) then
              role = M.ROLE_SOLDIER
              if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_DEFER t=%d pill=%s -> ally p%s open-recruiting (sub=%s, lower pn, no registry call); join, don't rival-command", now2, tostring(g.target_id), tostring(ally_pn), tostring(slot.info.sub))) end
              break
            end
          end
        end
      end
    end
  end
  -- Sticky commander latch reconcile (end of election). Remember the pill we
  -- command so next tick's is_leading keeps an established leader as COMMANDER
  -- through HP wobble / a closed recruiting call — i.e. first-to-initiate holds
  -- the role until a REAL end. We set it whenever we ended this tick as the
  -- commander of an attack_pill; we CLEAR it the instant role lands soldier —
  -- which only happens for a real reason: we deferred to an earlier / tie-lower-
  -- pn rival (the ladder above), we joined someone's squad (squad_cmdr), or we
  -- left the pill (goal changed / pill captured / dead). On tank death the whole
  -- latch is wiped by reset_blitz_state.
  if C.DYNAMIC_COMMANDERS then
    local gg = state.goal
    if role == M.ROLE_COMMANDER and gg and gg.kind == "attack_pill"
       and gg.target_id and gg.target_id >= 0 then
      state.squad_commander_pill = gg.target_id
    else
      state.squad_commander_pill = nil
    end
  end
  state.squad_role = role
  state._blitz_call_rejected = nil   -- recomputed each tick (arbiter / commander brj)

  -- Recruitment (slice 2): decide squad membership + a status code. No
  -- movement behavior yet — both are broadcast (cmdr / sqst) for the panel.
  state.squad_cmdr         = nil
  state.squad_help_target  = nil
  state.squad_blitz_target = nil   -- pill a squad soldier adopts (commander's target)
  state.squad_status       = "-"
  state.squad_negotiate_cmdr = nil -- commander we're offering a standoff to (pre-commit)
  state.squad_negotiate_pill = nil
  -- (squad_blitz_accepted PERSISTS across ticks — it's the committed-blitz latch.)
  local max_age = C.SQUAD_ALLY_MAX_AGE or 1750

  -- Proactive switch-off-own-take: if we're on our OWN attack_pill but still
  -- early (approach / plan_position) and a registry blitz call's pill is
  -- STRICTLY cheaper for us (our pool-6 cost for it < our pool-6 cost for our
  -- current pill), drop our solo take and go help that call instead — flip to
  -- SOLDIER and remember which call to prefer. Our own bco auto-closes when the
  -- role leaves commander. _blitz_switch_to biases the soldier pick below.
  state._blitz_switch_to = nil
  if role == M.ROLE_COMMANDER and state.goal and state.goal.kind == "attack_pill"
     and (state.goal.substate == "approach" or state.goal.substate == "plan_position")
     and state.goal.target_id and state.blitz_calls and state.cost_cache then
    local mine = state.cost_cache["6:" .. state.goal.target_id]
    local my_cost = mine and mine.cost
    if my_cost then
      local pick_pn, pick_cost
      for cmdr, call in pairs(state.blitz_calls) do
        if cmdr ~= self_pn and call.pill ~= state.goal.target_id then
          local ce = state.cost_cache["6:" .. call.pill]
          local c  = ce and ce.cost
          if c and c < my_cost and (not pick_cost or c < pick_cost) then
            pick_cost, pick_pn = c, cmdr
          end
        end
      end
      if pick_pn then
        role = M.ROLE_SOLDIER
        state._blitz_switch_to = pick_pn
        print2(string.format("BLITZ_SWITCH t=%d -> help C%d (cost %.0f < own %.0f)",
          now, pick_pn, pick_cost, my_cost))
      end
    end
  end

  if role ~= M.ROLE_SOLDIER then state.squad_blitz_accepted = nil end  -- only soldiers commit to a blitz

  -- "noblitz" BRAIN_INIT_ARG: skip the whole membership stage. The fields it
  -- would fill were just reset above, so we leave with squad_cmdr = nil,
  -- squad_blitz_target = nil and status "-": no commander branch (nothing sets
  -- the take as an ask, so init.lua never broadcasts a bco call and attack.lua
  -- never flips the goal to _blitz / blitz_wait), and no soldier branch (so
  -- BLITZ_SCAN / BLITZ_PICK never run and we answer nobody). The bot keeps its
  -- elected role for everything else and just takes pills solo.
  if state.blitz_disabled or not C.BLITZ_ENABLED then
    state.squad_blitz_accepted = nil
    state.squad_blitz_roster   = nil
    state.squad_blitz_reject   = nil
    state.squad_blitz_accept   = nil
    state.squad_status         = "-"
    if BRAIN_DEBUG_MODE and state._blitz_off_said ~= role then
      state._blitz_off_said = role
      print2(string.format("BLITZ_OFF t=%d reason=noblitz role=%s goal=%s tgt=%s — no call opened, none joined",
        now, tostring(role), state.goal and state.goal.kind or "?",
        state.goal and tostring(state.goal.target_id) or "?"))
    end
  elseif role == M.ROLE_COMMANDER then
    local g = state.goal
    if g and g.kind == "attack_pill" and g.target_id then
      state.squad_help_target  = g.target_id   -- commander's attack_pill IS the ask
      state.squad_blitz_target = g.target_id
      state.squad_status       = "blitz"
      -- Arbitrate standoff conflicts among answering soldiers (+ self): the
      -- FURTHER-traveling tank keeps a contested spot, the closer one is told to
      -- repick (broadcast via brj). Builds the answer roster for the viz.
      M.blitz_arbitrate(state, info, now, self_pn)
    else
      state.squad_blitz_roster = nil
      state.squad_blitz_reject = nil
      state.squad_blitz_accept = nil
    end
  elseif role == M.ROLE_SOLDIER then
    -- Blitz = "join my squad, then help with the blitz." A soldier joins a
    -- commander's squad iff: it's in a follow-the-call state, the squad isn't
    -- full, and it's not already in that squad. Once joined it STAYS (no
    -- re-decide each tick) while the commander keeps leading.
    local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
    local cap = M.blitz_soldier_cap()   -- party MAX minus the commander
    local dead = state.tank_dead_at

    -- Open blitz calls come from the REGISTRY (state.blitz_calls), populated by
    -- one-shot bco/bcc broadcasts + discovery — NOT re-derived from continuous
    -- broadcasts (so we don't depend on a transient substate and can't be fooled
    -- by a plain approach that isn't a blitz). Maintenance above already pruned
    -- inactive/closed calls, so every entry here is a live commander wanting
    -- help. NO distance gate — any live open call is a candidate; the join
    -- DISCOUNT (distance-scaled, zeroes by FULL_TILES) decides via the score
    -- which one we actually go for. Per-commander member counts come from live
    -- soldier broadcasts.
    local cmd_info, members = {}, {}
    local pill_rej = state._blitz_pill_reject
    for cmdr, call in pairs(state.blitz_calls or {}) do
      local pp = call.pill and world and world.pills and world.pills[call.pill] or nil
      -- Skip pills in the 30s no-spot reject window (no standoff was findable).
      local rej = pill_rej and pill_rej[call.pill]
      if pp and not (rej and now < rej) then
        cmd_info[cmdr] = { target = call.pill,
                           dist = math.abs(pp.mx - tmx) + math.abs(pp.my - tmy) }
      end
    end
    -- pill_attackers[pill] = total tanks already committed to attacking/
    -- capturing each pill, role-AGNOSTIC: the commander, its soldiers, AND
    -- independent attackers (e.g. a harasser solo-attacking the same pill, who
    -- is NOT a squad member so members[] never counts it). A blitz is capped at
    -- 2 TANKS total, so a pill with cap+1 attackers is FULL no matter how the
    -- c/s/h roles broke down — this is what stops a 3rd tank piling on when a
    -- commander + a harasser are already on it.
    local pill_attackers = {}
    for pn, slot in ally_state.iter_active(now, max_age) do
      -- Don't count a soldier that died since its last broadcast — its slot
      -- lingers active, but it no longer occupies a squad slot, so counting it
      -- would falsely fill the cap and block live joiners.
      local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
      if pn ~= self_pn and not is_dead and slot.info then
        if slot.info.role == "s" and slot.info.cmdr then
          local c = tonumber(slot.info.cmdr)
          if c then members[c] = (members[c] or 0) + 1 end
        end
        local g = slot.info.goal
        if g == "attack_pill" or g == "capture_pill" then
          local t = tonumber(slot.info.target or "")
          if t then pill_attackers[t] = (pill_attackers[t] or 0) + 1 end
        end
      end
    end
    local _reg_n = 0; for _ in pairs(state.blitz_calls or {}) do _reg_n = _reg_n + 1 end
    local _inr_n = 0; for _ in pairs(cmd_info) do _inr_n = _inr_n + 1 end
    if BRAIN_DEBUG_MODE and _reg_n > 0 then print2(string.format("BLITZ_SCAN t=%d registry=%d in_range=%d switch_to=%s acc=%s", now, _reg_n, _inr_n, tostring(state._blitz_switch_to), tostring(state.squad_blitz_accepted))) end

    -- COMMITTED already? (we accepted a blitz earlier and the commander is still
    -- leading.) Stay in its squad and adopt the blitz pill. squad_blitz_accepted
    -- PERSISTS across ticks (unlike squad_cmdr, which is recomputed each tick).
    local acc = state.squad_blitz_accepted
    if acc then
      local cslot = ally_state.get(acc)
      local cdead = dead and dead[acc] and cslot and dead[acc] > (cslot.last_tick or 0)
      -- "Still leading?" from LIVE signals, NOT the event-driven /info state goal.
      -- A commander stably running a blitz only re-broadcasts /info state on a
      -- goal CHANGE + a 30s heartbeat, so the `goal` field in our slot goes stale
      -- (saw a soldier read an ~800-tick-old goal=refuel and FALSELY uncommit
      -- mid-approach, dropping to none). Trust instead: an OPEN bco call from it,
      -- or it still naming US in its bac accept list — both arrive every tick. A
      -- fresh attack/capture goal is kept only as a last-resort positive.
      local call       = state.blitz_calls and state.blitz_calls[acc]
      local accepts_us = false
      local bac        = cslot and cslot.info and cslot.info.bac
      if bac then for s in tostring(bac):gmatch("%d+") do if tonumber(s) == self_pn then accepts_us = true break end end end
      local goal_ok = cslot and cslot.info
                      and (cslot.info.goal == "attack_pill" or cslot.info.goal == "capture_pill")
      if cslot and cslot.active and not cdead and (call or accepts_us or goal_ok) then
        -- Commander still leading. But if OUR OWN goal got pulled off the blitz
        -- (a reactive attack_tank, a flee, a refuel, kill_lgm, etc.) we must
        -- LEAVE the squad so the commander stops counting us — clearing
        -- squad_blitz_accepted drops our cmdr broadcast, and blitz_arbitrate
        -- (which filters on cmdr==self) removes us from the commander's roster
        -- next tick. That broadcast change IS the "I'm out" notification.
        -- A grace window lets pick_goal (runs before squad.update) ADOPT the
        -- blitz attack_pill goal right after the accept before we judge it; a
        -- transient goal=none between goals doesn't count as pulled.
        local g = state.goal
        local committed_for = now - (state._blitz_commit_tick or now)
        local pulled = g and g.kind and g.kind ~= "none"
                       and not ((g.kind == "attack_pill" or g.kind == "capture_pill")
                                and (not state.squad_blitz_target or g.target_id == state.squad_blitz_target))
        if pulled and committed_for >= (C.SQUAD_BLITZ_COMMIT_GRACE_TICKS or 5) then
          print2(string.format("BLITZ_LEAVE t=%d C%s — pulled to goal=%s tgt=%s (blitz pill=%s), leaving squad",
            now, tostring(acc), tostring(g.kind), tostring(g.target_id), tostring(state.squad_blitz_target)))
          state.squad_blitz_accepted = nil
          state._blitz_commit_tick   = nil
          -- fall through to negotiate below (availability() will likely decline
          -- since we're busy on the new goal → no offer, so we cleanly exit).
        else
          state.squad_cmdr   = acc
          state.squad_status = "join"
          -- Adopt the commander's blitz pill. Prefer its broadcast target, but
          -- that rides the event-driven /info state and goes STALE while the
          -- commander sends only bac/pblk handshakes — so fall back to its LIVE
          -- bco call pill (blitz_calls[acc], refreshed every tick). NEVER clobber
          -- a known target to nil: a nil here made attack.lua abort the committed
          -- take as "commander gone" and drop the negotiated standoff.
          local _call = state.blitz_calls and state.blitz_calls[acc]
          if cslot.info.goal == "attack_pill" and cslot.info.target then
            state.squad_blitz_target = tonumber(cslot.info.target)
          elseif _call and _call.pill then
            state.squad_blitz_target = _call.pill
          end
          if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_COMMITTED t=%d to C%s pill=%s", now, tostring(acc), tostring(state.squad_blitz_target))) end
          -- A committed soldier still reads brj: the commander can reject the
          -- spot we committed to (e.g. shot_blocked by a pill that appeared
          -- after the accept). attack.lua drops the spot and replans on it
          -- (BLITZ_BRJ_REPLAN); before this, only negotiating soldiers read
          -- brj and a committed one drove to a rejected spot forever.
          read_cmdr_brj(state, self_pn)
          return role
        end
      else
        if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_UNCOMMIT t=%d C%s gone/changed", now, tostring(acc))) end
        state.squad_blitz_accepted = nil  -- commander gone → uncommit, free to renegotiate
      end
    end

    -- NEGOTIATING: pick the nearest in-range commander with a free slot and
    -- offer to join it. We DON'T adopt the blitz pill yet — only on the
    -- commander's explicit accept (bac). The offered standoff (bes/bd) is
    -- computed in parallel by attack.blitz_negotiate while we keep our current
    -- goal; rejects (brj) make it repick.
    -- A squad is FULL when the commander already has `cap` soldiers OR the pill
    -- already has cap+1 tanks on it (commander + soldiers + independents). The
    -- second test is the role-agnostic "max 2 tanks per pill" cap.
    local function squad_full(pn, target)
      return (members[pn] or 0) >= cap
             or (pill_attackers[target] or 0) >= (cap + 1)
    end
    local best_pn, best_d, best_target, saw_full
    for pn, ci in pairs(cmd_info) do
      if squad_full(pn, ci.target) then saw_full = true
      elseif not best_d or ci.dist < best_d then best_d, best_pn, best_target = ci.dist, pn, ci.target end
    end
    -- A cost-switch above chose a specific cheaper call to help — prefer it over
    -- the nearest (only if it's joinable / not full).
    if state._blitz_switch_to and cmd_info[state._blitz_switch_to]
       and not squad_full(state._blitz_switch_to, cmd_info[state._blitz_switch_to].target) then
      best_pn     = state._blitz_switch_to
      best_target = cmd_info[best_pn].target
    end
    -- Blitz-only commander gate (C.BLITZ_ONLY_CMDR_NEEDS_FREE): a bot the gate
    -- kept from starting its own take joins through goal selection, which
    -- picks ONE open call's pill. Answer THAT call: availability() says yes
    -- only for the call on our goal pill, so with two calls open the
    -- nearest-call pick above declined "bz" every tick when the nearest call
    -- was the other one. Joinable matches only; nearest wins, tie -> lower pn.
    do
      local m_pn, m_d, m_t = M.goal_call_pick(state, cmd_info, best_pn, best_target, squad_full)
      if m_pn then
        if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_PICK_GOAL t=%d C%s pill=%s (our goal pill) instead of nearest C%s pill=%s [cmdr gate]", now, tostring(m_pn), tostring(m_t), tostring(best_pn), tostring(best_target))) end
        best_pn, best_d, best_target = m_pn, m_d, m_t
      end
    end
    if best_pn then
      local ok, reason = M.availability(state, info, best_target)
      if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_PICK t=%d C%s pill=%s dist=%s avail=%s reason=%s saw_full=%s [%s, soldier cap %d]", now, tostring(best_pn), tostring(best_target), tostring(best_d), tostring(ok), tostring(reason), tostring(saw_full), M.blitz_size_label(), cap)) end
      if ok then
        -- Accepted by the commander? → COMMIT (adopt the pill next tick).
        local bac, accepted = ally_state.get_key(best_pn, "bac"), false
        if bac then for s in string.gmatch(bac, "%d+") do if tonumber(s) == self_pn then accepted = true; break end end end
        -- Negotiation watchdog: track how long we've been awaiting THIS
        -- commander/pill's accept. A healthy commander rosters a conflict-free
        -- soldier within a tick or two; a wedged one (e.g. stuck in build_walls,
        -- never sending bac OR brj) would otherwise pause us at goal=none forever
        -- via the blitz-negotiation gate in goals.lua. Reset on target change.
        local nkey = best_pn * 1000 + (best_target or 0)
        if state._blitz_negotiate_key ~= nkey then
          state._blitz_negotiate_key   = nkey
          state._blitz_negotiate_since = now
        end
        local timeout = C.SQUAD_BLITZ_NEGOTIATE_TIMEOUT_TICKS or 250
        if accepted and not state.squad_blitz_engage_mx then
          -- Accepted by the commander, but our negotiated engage spot is GONE
          -- (e.g. a goal=none blip cleared the offer). Do NOT fast-commit
          -- spotless — that lets attack.lua fall to a bad, un-de-conflicted
          -- own-scan spot. Stay in negotiation so attack.blitz_negotiate
          -- re-offers a FRESH spot, re-validated by the commander against
          -- whoever may have taken our old one (3-blitzer safe). We commit only
          -- once we actually hold an offer again.
          state.squad_cmdr           = best_pn
          state.squad_negotiate_cmdr = best_pn
          state.squad_negotiate_pill = best_target
          state.squad_status         = "nego"
          read_cmdr_brj(state, self_pn)
          if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_RENEGOTIATE t=%d C%s pill=%s — accepted but engage spot lost, re-offering", now, tostring(best_pn), tostring(best_target))) end
        elseif accepted then
          state.squad_cmdr           = best_pn   -- broadcast which call we're answering
          state.squad_negotiate_cmdr = best_pn
          state.squad_negotiate_pill = best_target
          state.squad_status         = "join"
          state.squad_blitz_accepted = best_pn
          state.squad_blitz_target   = best_target
          state._blitz_commit_tick   = now   -- for the pulled-off-blitz grace window
          state._blitz_negotiate_key   = nil
          state._blitz_negotiate_since = nil
          if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_ACCEPTED t=%d by C%s pill=%s", now, tostring(best_pn), tostring(best_target))) end
        elseif state._blitz_negotiate_since and (now - state._blitz_negotiate_since) >= timeout then
          -- Commander never accepted in time. Abandon: reject this pill for a
          -- cooldown (same slate the no-spot path uses) so it drops out of
          -- cmd_info next tick and stops re-picking, and leave squad_negotiate_*
          -- nil so the goal-selection pause lifts (attack.blitz_negotiate clears
          -- our engage offer this tick) — we solo / pick another goal instead of
          -- standing idle forever.
          state._blitz_pill_reject = state._blitz_pill_reject or {}
          state._blitz_pill_reject[best_target] = now + (C.SQUAD_BLITZ_NEGOTIATE_REJECT_TICKS or 500)
          state.squad_status           = "free"
          state._blitz_negotiate_key   = nil
          state._blitz_negotiate_since = nil
          print2(string.format("BLITZ_NEGOTIATE_TIMEOUT t=%d C%s pill=%s — abandon (no bac in %dt), reject %dt", now, tostring(best_pn), tostring(best_target), timeout, C.SQUAD_BLITZ_NEGOTIATE_REJECT_TICKS or 500))
        else
          state.squad_cmdr           = best_pn   -- broadcast which call we're answering
          state.squad_negotiate_cmdr = best_pn
          state.squad_negotiate_pill = best_target
          -- "nego" (not "join"): we've OFFERED a standoff but the commander hasn't
          -- accepted us yet (awaiting bac). Distinct from a committed soldier's
          -- "join" so the commander doesn't count us as a real 2-tank blitz member
          -- and the roster can show us as a pending (yellow) tank. We still appear
          -- in the commander's de-confliction (it filters on role/cmdr, not sqst).
          state.squad_status         = "nego"
          if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_NEGOTIATING t=%d -> C%s pill=%s (awaiting bac)", now, tostring(best_pn), tostring(best_target))) end
          read_cmdr_brj(state, self_pn)  -- rejected → attack.blitz_negotiate repicks
        end
      else
        state.squad_status = reason  -- lh / na / bz  (declines: "won't join")
        -- Stamp a comm-line decline so the (often one-tick) "I'm busy" answer to
        -- this commander's call is rendered, not silently dropped.
        state._blitz_comm_reject = { tick = now, cmdr = best_pn, reason = reason }
        state._blitz_negotiate_key   = nil
        state._blitz_negotiate_since = nil
      end
    elseif saw_full then
      state.squad_status = "full"
      state._blitz_negotiate_key   = nil
      state._blitz_negotiate_since = nil
    else
      state.squad_status = "free"
      state._blitz_negotiate_key   = nil
      state._blitz_negotiate_since = nil
    end
  end

  return role
end

-- Role → display color for the roster panel.
local ROLE_COLOR = {
  [M.ROLE_COMMANDER] = { 255, 80,  80  },  -- red
  [M.ROLE_SOLDIER]   = { 50,  100, 235 },  -- blue (deeper royal — was too pale)
  [M.ROLE_HARASSER]  = { 255, 220, 60  },  -- yellow
}
-- pill_suicider row tint (orange) — distinct from the harasser yellow so the
-- two never read as the same role at a glance. Not a ROLE_COLOR entry: the
-- suicider is a FLAG on top of a commander/soldier role, exactly like harasser.
local SUICIDER_COLOR = { 255, 140, 30 }
-- Sort order in the panel: commanders, soldiers, harassers, then by pn.
local ROLE_ORDER = { [M.ROLE_COMMANDER] = 0, [M.ROLE_SOLDIER] = 1, [M.ROLE_HARASSER] = 2 }

-- Short status code → display text shown to the right of a tank #.
local STATUS_TEXT = {
  blitz = "blitz", join = "joining", free = "free", full = "squad full",
  lh = "no:low_hp", na = "no:no_ammo", bz = "no:busy",
  blitz_negotiating = "negotiating",
}

-- Right-side roster HUD, grouped into squads. Always-on (raw overlay binding,
-- no toggle). Each bot draws its own view; in BrainTest you see the followed
-- bot's roster.
--   commander (red)   — overall status to the right
--     soldier (blue)  — indented under its commander, status to the right
--   harasser (yellow) — own group
-- Goals whose target_id names a PILL (so the roster's target column is
-- meaningful). attack_/capture_/defend_/repair_pill + pill_place all carry a
-- pill id; base/tank goals carry a base/tank id and are left blank.
local PILL_TARGET_GOAL = {
  attack_pill = true, capture_pill = true, defend_pill = true,
  repair_pill = true, pill_place = true,
}

function M.draw_roster(state, info, now)
  if not viz.is_on("squad_roster") then return end
  local self_pn = info.player_number or -1
  local max_age = C.SQUAD_ALLY_MAX_AGE or 1750

  -- Gather every protocol bot's {role, cmdr, status}. Self comes from live
  -- state; allies from their broadcasts.
  local bots = {}
  -- Status column shows the full goal SUBSTATE (e.g. "blitz_wait"), with a
  -- special PARALLEL pseudo-substate "blitz_negotiating" surfaced while the bot
  -- is mid blitz-negotiation (offering a standoff, not yet committed). It's not
  -- a real goal substate — the bot keeps doing its actual thing — but we show
  -- it so the negotiation is visible. "committed" once accepted.
  -- Status column is a SQUAD/blitz readout: only the attack_pill substate is
  -- meaningful here. For any other goal (attack_tank, capture_base, refuel, ...)
  -- the substate vocabulary overlaps ("engage" etc.) and just reads as noise, so
  -- leave it blank. Blitz negotiate/commit always show — they're squad state.
  local self_status
  if state.squad_blitz_accepted then        self_status = "committed"
  elseif state.squad_negotiate_cmdr then     self_status = "blitz_negotiating"
  elseif state.goal and state.goal.kind == "attack_pill" then
    self_status = state.goal.substate
  else self_status = nil end
  local self_target
  if state.goal and state.goal.target_id and state.goal.target_id >= 0
     and PILL_TARGET_GOAL[state.goal.kind] then
    self_target = state.goal.target_id
  end
  bots[self_pn] = {
    role     = state.squad_role or M.ROLE_SOLDIER,
    cmdr     = state.squad_cmdr,
    status   = self_status,
    target   = self_target,
    harasser = state.is_harasser or false,
    suicider = state.is_pill_suicider or false,
    me       = true,
  }
  for pn in ally_state.iter_active(now, max_age) do
    if pn ~= self_pn then
      local a_cmdr = tonumber(ally_state.get_key(pn, "cmdr") or "")
      local a_status
      -- An ally's committed-vs-negotiating state is its OWN self-report: sqst
      -- "nego" = offered, awaiting our bac; sqst "join" = it read our bac and
      -- committed. (Don't infer from bd-presence — bd lingers stale in the slate
      -- from the negotiation phase, so a committed soldier kept reading as
      -- "negotiating" forever.) No extra confirmation handshake needed.
      local a_sqst = ally_state.get_key(pn, "sqst")
      if a_cmdr and a_sqst == "nego" then a_status = "blitz_negotiating"
      elseif a_cmdr then                  a_status = "committed"
      elseif ally_state.get_key(pn, "goal") == "attack_pill" then
        a_status = ally_state.get_key(pn, "sub")
      else a_status = nil end
      local a_target
      if PILL_TARGET_GOAL[ally_state.get_key(pn, "goal")] then
        local t = tonumber(ally_state.get_key(pn, "target") or "")
        if t and t >= 0 then a_target = t end
      end
      bots[pn] = {
        role     = ally_state.get_key(pn, "role") or "?",
        cmdr     = a_cmdr,
        status   = a_status,
        target   = a_target,
        harasser = ally_state.get_key(pn, "har") == "1",
        suicider = ally_state.get_key(pn, "psu") == "1",
      }
    end
  end

  -- Sorted player list for stable ordering.
  local order = {}
  for pn in pairs(bots) do order[#order + 1] = pn end
  table.sort(order)

  -- Right-anchored: larger x = further LEFT. Numbers sit at NUM_X (commanders)
  -- / NUM_X-IND (soldiers, indented right); target pill at TGT_X; status text
  -- right-aligned at STAT_X.
  local NUM_X, IND, TGT_X, STAT_X = 240, 22, 175, 16
  local y = 350
  local row_h = 16

  -- Effective-commander detection. With DYNAMIC_COMMANDERS the role partition
  -- flips tick-to-tick and a bot that is actually LEADING a blitz can briefly
  -- broadcast role="soldier" (its soldiers still point cmdr at it). Treat any
  -- bot that someone follows as a commander for display, so the leader + its
  -- squad render as a group instead of as loose soldiers. `leads[pn]` = some
  -- other bot has cmdr==pn.
  local leads = {}
  for _, pn in ipairs(order) do
    local c = bots[pn].cmdr
    if c ~= nil and c ~= pn then leads[c] = true end
  end
  local function is_cmdr(pn, b)
    return b.role == M.ROLE_COMMANDER or leads[pn]
  end

  -- Team-global ordering. The whole roster is one deterministic sort by a key
  -- every bot computes identically from the SAME data (broadcasts + the bot's
  -- own state), so in theory every teammate renders the roster in the same
  -- order. Key = (squad_id, in-squad rank, pn):
  --   * squad_id groups a commander with its soldiers — the commander's pn for
  --     itself and for any soldier whose cmdr is that (effective) commander;
  --     own pn for an unattached soldier / orphan / harasser (a singleton group).
  --   * rank puts the commander (0) above its soldiers (1) within the group.
  --   * pn breaks remaining ties.
  -- (Residual divergence is only from data skew — a bot sees its OWN state live
  -- but allies via slightly-laggy broadcasts — not from the ordering itself.)
  local function squad_id(pn, b)
    if is_cmdr(pn, b) then return pn end
    local c = b.cmdr
    if c and bots[c] and is_cmdr(c, bots[c]) then return c end
    return pn
  end
  local function as_cmdr_of(pn, b)
    return b.role ~= M.ROLE_HARASSER and is_cmdr(pn, b)
  end
  table.sort(order, function(a, b)
    local ba, bb = bots[a], bots[b]
    local sa, sb = squad_id(a, ba), squad_id(b, bb)
    if sa ~= sb then return sa < sb end
    local ra = as_cmdr_of(a, ba) and 0 or 1
    local rb = as_cmdr_of(b, bb) and 0 or 1
    if ra ~= rb then return ra < rb end
    return a < b
  end)

  local function draw_row(pn, b, indent, as_cmdr)
    -- as_cmdr forces commander display; "!" flags the inferred mismatch (a
    -- bot we render as commander because it's followed, but whose broadcast
    -- role still reads soldier — surfaces the underlying role flip-flop).
    local inferred  = as_cmdr and b.role ~= M.ROLE_COMMANDER
    local disp_role = as_cmdr and M.ROLE_COMMANDER or b.role
    -- Harassers carry large goal-cost biases (pill ×2, travel ×0.5), so make them
    -- pop: tint the whole row the harasser color. The C/S letter + "h" suffix
    -- still convey the actual squad role and harasser status.
    local col = (b.suicider and SUICIDER_COLOR)
                or (b.harasser and ROLE_COLOR[M.ROLE_HARASSER])
                or ROLE_COLOR[disp_role] or { 180, 180, 180 }
    local num = string.upper(tostring(disp_role or "?")) .. tostring(pn)
                .. (b.harasser and "h" or "")   -- harasser flag (e.g. "S5h"), decoupled from role
                .. (b.suicider and "x" or "")   -- pill_suicider flag (e.g. "S5x"), same slate as h, map-selected
                .. (inferred and "!" or "")
    -- Self (the followed bot): a box around the whole row instead of a "*" tag.
    -- topright hud_rect: x = right-edge offset, w extends leftward (see blitz_roster).
    if b.me then
      local _box_l = (NUM_X - (indent and IND or 0)) + 52   -- ~role left edge + pad
      viz.hud_rect("squad_roster", STAT_X - 2, y - 1, _box_l - (STAT_X - 2), row_h,
                       "topright", 235, 235, 255, 255, false)
    end
    viz.hud_text("squad_roster", NUM_X - (indent and IND or 0), y, num, "topright",
                     col[1], col[2], col[3], 255)
    -- Target pill column (blank for non-pill goals).
    if b.target ~= nil then
      viz.hud_text("squad_roster", TGT_X, y, tostring(b.target), "topright",
                       200, 200, 140, 255)
    end
    local st = b.status and (STATUS_TEXT[b.status] or b.status) or ""
    if st ~= "" then
      viz.hud_text("squad_roster", STAT_X, y, st, "topright", 190, 190, 190, 255)
    end
    y = y + row_h
  end

  viz.hud_text("squad_roster", NUM_X, y, "-- SQUADS --", "topright", 210, 210, 210, 255)
  viz.hud_text("squad_roster", TGT_X, y, "tgt", "topright", 160, 160, 120, 255)
  y = y + 20

  -- One pass over the team-global sorted order. Each squad's commander prints
  -- first (no indent), its soldiers indented under it; unattached soldiers and
  -- harassers print as their own singleton groups in pn order interleaved by
  -- squad_id. Same order for every teammate (given synced data).
  for _, pn in ipairs(order) do
    local b = bots[pn]
    local as_cmdr = as_cmdr_of(pn, b)
    -- Indent ONLY a soldier that is actually attached to a commander (its squad_id
    -- resolves to someone else). A commander, an unattached/squadless soldier, and
    -- a harasser all sit flush-left at the same indentation — so indentation means
    -- exactly "this bot is under the commander above it."
    local indent = (not as_cmdr) and (squad_id(pn, b) ~= pn)
    draw_row(pn, b, indent, as_cmdr)
  end
end

-- help_range: the call-out range for a commander's pill take — the region in
-- which a soldier will answer (join the blitz). The join gate is MANHATTAN
-- distance from the soldier's tank to the commander's PILL <= SQUAD_HELP_RANGE,
-- so this draws a diamond (not a circle) of that radius around each blitzing
-- commander's pill: self (from state.goal) + allied commanders (from broadcast).
function M.draw_help_range(state, info, now, world)
  if not viz.is_on("help_range") or not viz.line then return end
  local rng = C.SQUAD_HELP_RANGE or 30
  local function diamond(pmx, pmy, label, cr, cg, cb)
    local n, e, s, w = pmy - rng, pmx + rng, pmy + rng, pmx - rng
    local cx, cy = pmx + 0.5, pmy + 0.5
    viz.line("help_range", cx, n + 0.5, e + 0.5, cy, cr, cg, cb, 170)
    viz.line("help_range", e + 0.5, cy, cx, s + 0.5, cr, cg, cb, 170)
    viz.line("help_range", cx, s + 0.5, w + 0.5, cy, cr, cg, cb, 170)
    viz.line("help_range", w + 0.5, cy, cx, n + 0.5, cr, cg, cb, 170)
    if viz.text then viz.text("help_range", cx, n + 0.2, label, "center", cr, cg, cb, 220) end
  end
  -- Self, if commanding a pill take.
  if state.squad_role == M.ROLE_COMMANDER and state.goal
     and state.goal.kind == "attack_pill" and state.goal.mx then
    diamond(state.goal.mx, state.goal.my, "help r=" .. rng .. " (me)", 255, 230, 120)
  end
  -- Allied commanders mid pill take: resolve the pill tile from their broadcast
  -- target id against our world (attack_pill doesn't ship mx/my).
  local self_pn = info.player_number or -1
  for pn in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= self_pn and ally_state.get_key(pn, "role") == "c"
       and ally_state.get_key(pn, "goal") == "attack_pill" then
      local pid = tonumber(ally_state.get_key(pn, "target"))
      local pp  = pid and world and world.pills and world.pills[pid] or nil
      if pp then diamond(pp.mx, pp.my, "help r=" .. rng .. " C" .. pn, 255, 180, 80) end
    end
  end
end

-- Commander GO handshake: count this commander's live squad soldiers and how
-- many have reported IN POSITION (rdy=1). Used in blitz_wait to decide GO
-- (all ready, or timeout). A dead soldier (tank_dead_at after its last
-- broadcast) is excluded from the total so it can't stall the quorum.
function M.blitz_ready_status(state, now, self_pn, info)
  local total, ready = 0, 0
  local inwait = 0             -- soldiers PARKED at their standoff (sub=blitz_wait), aimed or not
  local min_pending_bd = nil   -- closest NOT-yet-ready committed soldier's walk dist (bd)
  local any_unseen = false     -- a pending soldier we can't see (relying on its broadcast bd)
  local dead = state.tank_dead_at
  -- We KNOW our allies' live positions when their tanks are in our perception:
  -- a tank brain-object's idnum IS the player slot number (players.c
  -- brainDataAddObject), so build pn -> live tile position. This lets us measure
  -- a pending soldier's remaining distance OURSELVES every tick (free, exact)
  -- instead of trusting its periodically-broadcast bd.
  local pos_by_pn = nil
  if info and info.objects then
    pos_by_pn = {}
    for _, ob in ipairs(info.objects) do
      if ob.type == OBJECT_TANK and (bit.band(ob.info, OBJECT_HOSTILE)) == 0 then
        pos_by_pn[ob.idnum] = { mx = bit.rshift(ob.x, 8), my = bit.rshift(ob.y, 8) }
      end
    end
  end
  -- Count only soldiers COMMITTED to OUR pill — i.e. broadcasting attack_pill on
  -- our target (roster Status "y"). A soldier merely NEGOTIATING ("m") already
  -- broadcasts cmdr=us (so it could offer a standoff) but is still on its OWN
  -- goal and has NOT accepted a position — it must not count toward blitz_wait /
  -- the GO quorum. Without our pill, nothing is committed yet → 0.
  local our_pid = state.goal and state.goal.target_id
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= self_pn and our_pid then
      local h = slot.info
      local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
      if not is_dead and h.role == "s" and tonumber(h.cmdr or "") == self_pn
         and h.sqst ~= "nego"   -- still negotiating (offered, not yet accepted) → not a member yet
         and h.goal == "attack_pill" and tonumber(h.target or "") == our_pid then
        total = total + 1
        -- C.BLITZ_GO_ACCEPTED_ONLY: a soldier we have not accepted (not in our
        -- bac) is still committed (total) but is not parked or ready for GO.
        local go_ok = M.go_counts_soldier(state, pn)
        -- Parked at its firing spot (past approach), independent of aim — the
        -- overwhelm wait (blitz_wait) OR anywhere in the PPT firing sequence
        -- (aim → in_range_* → shoot_pill). A soldier here is "go-able": it fires
        -- on GO without travelling, so it counts toward the early-GO quorum.
        if go_ok and M.BLITZ_READY_SUBS[h.sub or ""] then inwait = inwait + 1 end
        if h.rdy == "1" then
          if go_ok then ready = ready + 1 end
        else
          -- Still approaching/aiming — track the closest one's REMAINING walk
          -- distance so the commander can tell whether anyone is still closing.
          -- Prefer measuring it ourselves from the soldier's LIVE position to its
          -- broadcast standoff (bes); fall back to its broadcast bd when its tank
          -- is out of our perception (and flag it so we can query it directly).
          local bd, seen = nil, false
          local p = pos_by_pn and pos_by_pn[pn]
          if p and h.bes then
            local bx, by = M.parse_bes(h.bes)   -- 2026-09-26: nil-safe
            if bx then bd = U.mdist(p.mx, p.my, bx, by); seen = true end
          end
          if not bd then bd = tonumber(h.bd or "") end
          if not seen then any_unseen = true end
          if bd and (not min_pending_bd or bd < min_pending_bd) then min_pending_bd = bd end
        end
      end
    end
  end
  return total, ready, min_pending_bd, any_unseen, inwait
end

-- Full blitz visualizer for the followed bot. Toggle "squad_blitz". Draws:
--   * the bot's own engage spot (magenta; green once IN POSITION; cyan once
--     committed/charging) + setup point (orange) + a line to the pill;
--   * a state label over the tank (role + blitz substate, plus rdy/GO);
--   * for a commander, the rally tally (ready/total) and a GO flash;
--   * every squadmate's broadcast engage spot (smaller, ready-colored) so you
--     can watch the whole squad converge.
function M.draw_blitz(state, info, now)
  if not viz.is_on("squad_blitz") then return end
  local g = state.goal
  local sub = g and g.substate
  local committed = g and g._blitz_committed
  local self_pn = info.player_number or -1

  -- ── Squadmates' engage spots (everyone sharing my commander) ───────────
  -- "My squad" id: a commander uses its own pn; a soldier uses its cmdr.
  local squad_id = (state.squad_role == M.ROLE_COMMANDER) and self_pn or state.squad_cmdr
  if squad_id then
    local dead = state.tank_dead_at
    for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
      local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
      if pn ~= self_pn and not is_dead then
        local h = slot.info
        local mine = (tonumber(h.cmdr or "") == squad_id) or (pn == squad_id)
        if mine and h.bes then
          -- bes is the standoff's FLOAT tile center (4dp) — use as-is, no +0.5.
          local bx, by = M.parse_bes(h.bes)
          if bx then
            local rc = (h.rdy == "1") and { 120, 255, 120 } or { 200, 120, 200 }
            viz.circle("squad_blitz", bx, by, 0.45, rc[1], rc[2], rc[3], 160)
            local tag = (h.role == "c") and "C" or "S"
            viz.text("squad_blitz", bx, by - 0.8,
                     tag .. tostring(pn) .. ((h.rdy == "1") and " RDY" or ""),
                     "center", rc[1], rc[2], rc[3], 220)
          end
        end
      end
    end
  end

  -- ── Our own engage spot + setup point + line to pill ───────────────────
  if g and g._blitz and state.squad_blitz_engage_mx then
    local ex = state.squad_blitz_engage_fx or (state.squad_blitz_engage_mx + 0.5)
    local ey = state.squad_blitz_engage_fy or (state.squad_blitz_engage_my + 0.5)
    local col = committed and { 80, 220, 255 }
                or (state.squad_blitz_in_position and { 120, 255, 120 }
                or { 255, 0, 255 })
    viz.circle("squad_blitz", ex, ey, 0.6, col[1], col[2], col[3], 220)
    local lbl = committed and "ENGAGE (GO)"
                or (state.squad_blitz_in_position and "ENGAGE (READY)" or "ENGAGE")
    viz.text("squad_blitz", ex, ey - 1.0, lbl, "center", col[1], col[2], col[3], 255)
    if g.mx and g.my then
      viz.line("squad_blitz", ex, ey, g.mx + 0.5, g.my + 0.5, col[1], col[2], col[3], 120)
    end
    if g.approach_mx and g.approach_my then
      viz.circle("squad_blitz", g.approach_mx + 0.5, g.approach_my + 0.5, 0.4, 255, 180, 0, 200)
      viz.text("squad_blitz", g.approach_mx + 0.5, g.approach_my + 0.5 - 0.7,
               "setup", "center", 255, 180, 0, 255)
    end
  end

  -- ── State label over the tank ──────────────────────────────────────────
  if g and g._blitz and sub then
    local twx, twy = info.tankx / 256.0, info.tanky / 256.0
    local role_tag = (state.squad_role == M.ROLE_COMMANDER) and "CMDR" or "SOLDIER"
    local line2
    if state.squad_role == M.ROLE_COMMANDER and sub == "blitz_wait" then
      local total, ready = M.blitz_ready_status(state, now, self_pn)
      -- party = commander + committed soldiers, against the HARD size floor the
      -- GO paths enforce, so the label says whether this call can fire at all.
      line2 = string.format("RALLY %d/%d  party %d/%d", ready, total, 1 + (total or 0), M.blitz_min())
    elseif sub == "blitz_wait" then
      line2 = state.squad_blitz_in_position and "READY -- WAIT GO" or "..."
    elseif committed then
      line2 = "GO!"
    end
    viz.text("squad_blitz", twx, twy - 1.6,
             "BLITZ " .. role_tag .. ":" .. sub, "center", 255, 230, 120, 255)
    if line2 then
      local lc = committed and { 80, 220, 255 } or { 255, 230, 120 }
      viz.text("squad_blitz", twx, twy - 1.0, line2, "center", lc[1], lc[2], lc[3], 255)
    end
  end
end

-- Engage-standoff claims of everyone in MY squad (commander + fellow soldiers),
-- from their broadcast `bes` (blitz engage spot) key. A negotiating soldier
-- avoids picking a spot within 1 tile of any of these. Returns a list of {mx,my}.
function M.blitz_claims(state, now, self_pn)
  local cmdr = state.squad_cmdr
  if not cmdr then return {} end
  local claims = {}
  local function add(pn)
    local bes = ally_state.get_key(pn, "bes")
    if bes then
      local fx, fy = M.parse_bes(bes)   -- 2026-09-26: nil-safe
      if fx then claims[#claims + 1] = { fx = fx, fy = fy } end
    end
  end
  add(cmdr)  -- the commander's own standoff
  local dead = state.tank_dead_at
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
    if pn ~= self_pn and not is_dead
       and tonumber(ally_state.get_key(pn, "cmdr") or "") == cmdr then
      add(pn)
    end
  end
  return claims
end

-- Role label above each tank on the map: "C<pn>" / "S<pn> [C<cmdr>]" /
-- "H<pn>", colored by role. Toggleable via the "squad_labels" viz id.
local function squad_label(role, pn, cmdr)
  if role == M.ROLE_COMMANDER then return "C" .. pn end
  if role == M.ROLE_SOLDIER then
    return "S" .. pn .. (cmdr and (" [C" .. cmdr .. "]") or "")
  end
  if role == M.ROLE_HARASSER then return "H" .. pn end
  return "?" .. pn
end

function M.draw_labels(state, info, now)
  if not viz.is_on("squad_labels") then return end
  local self_pn = info.player_number or -1

  -- Exact sub-tile positions of every visible tank, keyed by player number.
  local exact = {}
  if info.objects then
    for _, ob in ipairs(info.objects) do
      if ob.type == OBJECT_TANK and ob.idnum then
        exact[ob.idnum] = { x = ob.x / 256.0, y = ob.y / 256.0 }
      end
    end
  end

  local function draw_at(px, py, role, pn, cmdr)
    local c = ROLE_COLOR[role] or { 180, 180, 180 }
    -- Just over the tank's head (a tank is ~1 tile tall).
    viz.text("squad_labels", px, py - 0.7,
             squad_label(role, pn, cmdr), "center", c[1], c[2], c[3], 255)
  end

  -- Self: exact own position.
  draw_at(info.tankx / 256.0, info.tanky / 256.0,
          state.squad_role or M.ROLE_SOLDIER, self_pn, state.squad_cmdr)

  -- Allies (skip dead): exact if visible, else the broadcast tile center.
  local dead = state.tank_dead_at
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= self_pn
       and not (dead and dead[pn] and dead[pn] > (slot.last_tick or 0)) then
      local role = ally_state.get_key(pn, "role")
      if role then
        local px, py
        local e = exact[pn]
        if e then
          px, py = e.x, e.y
        else
          local tx = tonumber(ally_state.get_key(pn, "tx"))
          local ty = tonumber(ally_state.get_key(pn, "ty"))
          if tx and ty then px, py = tx + 0.5, ty + 0.5 end
        end
        if px then
          draw_at(px, py, role, pn, tonumber(ally_state.get_key(pn, "cmdr") or ""))
        end
      end
    end
  end
end

local function role_color(r)
  if r == "c" then return 255, 120, 120
  elseif r == "h" then return 240, 230, 120
  else return 120, 200, 255 end
end

-- role_live: C/S/H over each tank; self shows its live role + (for a dynamic
-- commander) the hard-take trigger. Allies sourced from broadcast role.
function M.draw_roles_live(state, info, now)
  if not viz.is_on("role_live") or not viz.text then return end
  local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
  local role = state.squad_role or "s"
  local reason = ""
  if role == "c" and state.goal and state.goal.kind == "attack_pill" and state.goal.target_id then
    reason = " take#" .. state.goal.target_id
  end
  local r, g, b = role_color(role)
  viz.text("role_live", tmx + 0.5, tmy - 1.4,
           string.format("%s%s (me)", role:upper(), reason), "center", r, g, b, 245)
  if info.objects then
    for _, ob in ipairs(info.objects) do
      if ob.type == 0 and ob.idnum and ob.idnum ~= info.player_number then
        local ar = ally_state.get_key(ob.idnum, "role")
        if ar and ar ~= "" then
          local ar2, ag, ab = role_color(ar)
          viz.text("role_live", ob.x / 256.0, ob.y / 256.0 - 1.4,
                   ar:upper() .. ob.idnum, "center", ar2, ag, ab, 230)
        end
      end
    end
  end
end

-- blitz_call (soldier/self view): our answer to the call, our computed standoff,
-- the walk distance we reported, and repos (commander 'pick another' count).
function M.draw_blitz_call(state, info, now)
  if not viz.is_on("blitz_call") then return end
  local g = state.goal
  local x, y, dy = 480, 90, 14
  viz.hud_text("blitz_call", x, y, "-- BLITZ NEGOTIATION (me) --", "topright", 210, 210, 210, 255)
  y = y + dy
  local ans, ar, ag, ab
  if state.squad_blitz_accepted then
    ans, ar, ag, ab = "COMMITTED -> C" .. tostring(state.squad_blitz_accepted), 120, 230, 120
  elseif state.squad_negotiate_cmdr then
    ans, ar, ag, ab = "negotiating -> C" .. tostring(state.squad_negotiate_cmdr), 230, 210, 120
  else
    ans, ar, ag, ab = "N (no call)", 180, 180, 180
  end
  viz.hud_text("blitz_call", x, y, "answer: " .. ans, "topright", ar, ag, ab, 255)
  y = y + dy
  local so = state.squad_blitz_engage_mx
             and string.format("standoff (%.4f,%.4f)", state.squad_blitz_engage_fx or (state.squad_blitz_engage_mx + 0.5), state.squad_blitz_engage_fy or (state.squad_blitz_engage_my + 0.5))
             or "standoff -"
  viz.hud_text("blitz_call", x, y, so, "topright", 200, 200, 255, 255)
  y = y + dy
  local repos = state.squad_blitz_repos or (g and g._blitz_repos) or 0
  viz.hud_text("blitz_call", x, y, string.format("walk dist: %s   repos: %d",
                   tostring(state.squad_blitz_bd or "-"), repos),
                   "topright", 200, 200, 200, 255)
  -- map: our standoff dot
  if viz.circle and state.squad_blitz_engage_mx then
    viz.circle("blitz_call", state.squad_blitz_engage_fx or (state.squad_blitz_engage_mx + 0.5), state.squad_blitz_engage_fy or (state.squad_blitz_engage_my + 0.5),
               0.6, 140, 230, 120, 230)
  end
end

-- True if we're leading an OPEN blitz call and at least one allied soldier is
-- positioned to answer it: in help range (Manhattan tank->our pill) of our
-- pill, a free slot in our squad, and not already committed to another
-- commander. Drives the roster panel's "Waiting..." vs idle text.
function M.blitz_has_joiner(state, info, now)
  local g = state.goal
  if not (g and g._blitz and g.kind == "attack_pill"
          and not state.squad_cmdr and g.target_id and g.mx) then
    return false
  end
  local self_pn = info.player_number or -1
  local rng     = C.SQUAD_HELP_RANGE or 30
  local cap     = M.blitz_soldier_cap()   -- party MAX minus the commander
  local pmx, pmy = g.mx, g.my
  -- Allied tank tile positions from our own game view, keyed by player number
  -- (idnum is globally unique). attack_pill no longer broadcasts tx/ty.
  local pos
  if info.objects then
    for _, ob in ipairs(info.objects) do
      if ob.type == 0 and ob.idnum ~= nil then  -- OBJECT_TANK
        pos = pos or {}
        pos[ob.idnum] = { mx = bit.rshift(ob.x, 8), my = bit.rshift(ob.y, 8) }
      end
    end
  end
  if not pos then return false end
  local max_age = C.SQUAD_ALLY_MAX_AGE or 1750
  local dead = state.tank_dead_at
  local members = 0
  for pn, slot in ally_state.iter_active(now, max_age) do
    local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
    if pn ~= self_pn and slot.info.role == "s"
       and tonumber(slot.info.cmdr or "") == self_pn and not is_dead then
      members = members + 1
    end
  end
  if members >= cap then return false end
  for pn, slot in ally_state.iter_active(now, max_age) do
    local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
    if pn ~= self_pn and not is_dead then
      local h = slot.info
      local committed_elsewhere = h.cmdr and tonumber(h.cmdr) ~= self_pn
      if h.role == "s" and not committed_elsewhere then
        local p = pos[pn]
        if p and (math.abs(p.mx - pmx) + math.abs(p.my - pmy)) <= rng then
          return true
        end
      end
    end
  end
  return false
end

-- blitz_join_hl: a bright cyan border around every pill an ally is running a
-- JOINABLE blitz on (state.squad_joinable_pills). Taking one of these joins the
-- blitz instead of de-conflicting away (they're exempt from ally_claimed), so
-- the border flags "a take here joins a blitz".
function M.draw_blitz_join_highlight(state, info, now)
  if not viz.is_on("blitz_join_hl") or not viz.line then return end
  local jp = state.squad_joinable_pills
  if not jp then return end
  for _, p in pairs(jp) do
    local x0, y0 = p.mx - 0.2, p.my - 0.2
    local x1, y1 = p.mx + 1.2, p.my + 1.2
    viz.line("blitz_join_hl", x0, y0, x1, y0, 90, 220, 255, 240)
    viz.line("blitz_join_hl", x1, y0, x1, y1, 90, 220, 255, 240)
    viz.line("blitz_join_hl", x1, y1, x0, y1, 90, 220, 255, 240)
    viz.line("blitz_join_hl", x0, y1, x0, y0, 90, 220, 255, 240)
    viz.text("blitz_join_hl", p.mx + 0.5, p.my - 0.5, "JOIN BLITZ", "center", 90, 220, 255, 255)
  end
end

-- blitz_roster (COMMANDER's negotiation view): one row per ally answering our
-- blitz call. Columns:
--   tank          — S<pn>
--   Status        — the ALLY's answer (y/n/m): m = negotiating (still offering /
--                   repicking), y = committed to the take (adopted attack_pill
--                   on our pill), n = declined.
--   Standoff      — their offered/claimed standoff spot (bes), "fx,fy" (4dp
--                   float). Updates live as a negotiating ('m') soldier repicks.
--   Dist          — their reported walk distance (bd).
--   Ans           — OUR verdict on their standoff: ACC = accepted, REJ = invalid
--                   (clash with another claim, or shot wall-blocked), "-" while
--                   they haven't offered one yet.
-- Status flips to Y once negotiation settles on a spot we accept and the ally
-- commits; while we keep rejecting, the ally stays at M (it repicks).
function M.draw_blitz_roster(state, info, now)
  if not viz.is_on("blitz_roster") then return end
  local x, y, dy = 480, 90, 14
  local self_pn = info.player_number or -1
  local max_age = C.SQUAD_ALLY_MAX_AGE or 1750
  local dead = state.tank_dead_at

  -- My squad's captain (commander): myself when I'm commanding, otherwise the
  -- commander I'm committed to / negotiating with. Shows the FULL squad from
  -- whatever seat I'm in (captain or soldier), including me.
  local cap_pn
  if state.squad_role == M.ROLE_COMMANDER then cap_pn = self_pn
  else cap_pn = state.squad_blitz_accepted or state.squad_cmdr or state.squad_negotiate_cmdr end

  viz.hud_text("blitz_roster", x, y, "-- BLITZ SQUAD --", "topright", 210, 210, 210, 255)
  y = y + dy
  if not cap_pn then
    viz.hud_text("blitz_roster", x, y, "(not in a blitz squad)", "topright", 150, 150, 150, 255)
    return
  end
  viz.hud_text("blitz_roster", x, y, string.format("%-4s %-24s %-12s %s", "tank", "goal", "standoff", "rdy"),
                   "topright", 200, 200, 200, 255)
  y = y + dy

  -- Member list: captain first, then its live soldiers, then self if a soldier.
  local seen, list = {}, {}
  local function add(pn, is_cap)
    if pn == nil or seen[pn] then return end
    seen[pn] = true
    list[#list + 1] = { pn = pn, is_cap = is_cap, is_self = (pn == self_pn) }
  end
  add(cap_pn, true)
  for pn, slot in ally_state.iter_active(now, max_age) do
    local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
    if not is_dead and slot.info.role == "s" and tonumber(slot.info.cmdr or "") == cap_pn then
      add(pn, false)
    end
  end
  if self_pn ~= cap_pn
     and (state.squad_cmdr == cap_pn or state.squad_blitz_accepted == cap_pn
          or state.squad_negotiate_cmdr == cap_pn) then
    add(self_pn, false)
  end

  for _, m in ipairs(list) do
    local gkind, gtgt, gsub, bes, rdy
    if m.is_self then
      gkind = (state.goal and state.goal.kind) or "none"
      gtgt  = state.goal and state.goal.target_id
      gsub  = state.goal and state.goal.substate
      if state.squad_blitz_engage_mx then
        bes = string.format("%.1f,%.1f", state.squad_blitz_engage_fx or (state.squad_blitz_engage_mx + 0.5), state.squad_blitz_engage_fy or (state.squad_blitz_engage_my + 0.5))
      elseif state.goal and (state.goal.standoff_fx or state.goal.standoff_mx) then
        bes = string.format("%.1f,%.1f", state.goal.standoff_fx or (state.goal.standoff_mx + 0.5),
                            state.goal.standoff_fy or (state.goal.standoff_my + 0.5))
      end
      rdy = state.squad_blitz_aimed
    else
      gkind = ally_state.get_key(m.pn, "goal") or "?"
      gtgt  = tonumber(ally_state.get_key(m.pn, "target") or "")
      gsub  = ally_state.get_key(m.pn, "sub")
      local b = ally_state.get_key(m.pn, "bes")
      if b then
        local fx, fy = M.parse_bes(b)
        if fx then bes = string.format("%.1f,%.1f", fx, fy) end
      end
      rdy = ally_state.get_key(m.pn, "rdy") == "1"
    end
    local goal_str = gkind .. ((gtgt and gtgt >= 0) and ("#" .. gtgt) or "")
                       .. ((gsub and gsub ~= "") and ("/" .. gsub) or "")
    local row = string.format("%-4s %-24s %-12s %s",
                  (m.is_cap and "C" or "S") .. tostring(m.pn), goal_str, bes or "-", rdy and "RDY" or "")
    local tw = #row * 12   -- HUD text is 8px/char at 1.5x scale = 12px
    -- Commander → semi-transparent yellow background (visible over water; drawn first, text on top).
    if m.is_cap then
      viz.hud_rect("blitz_roster", x, y, tw, 13, "topright", 255, 225, 90, 90, true)
    end
    -- Current bot (us) → white border around the row (distinct from the yellow commander fill).
    if m.is_self then
      viz.hud_rect("blitz_roster", x - 2, y - 2, tw + 4, 17, "topright", 255, 255, 255, 255, false)
    end
    -- Negotiating (offered a standoff, not yet accepted → sqst "nego") renders
    -- YELLOW: a pending tank, NOT counted as a committed 2-tank blitz member yet.
    -- Committed/other rows stay green.
    local sqst
    if m.is_self then sqst = state.squad_status else sqst = ally_state.get_key(m.pn, "sqst") end
    local tr, tg, tb = 140, 230, 140
    if sqst == "nego" then tr, tg, tb = 245, 215, 50 end
    viz.hud_text("blitz_roster", x, y, row, "topright", tr, tg, tb, 255)
    y = y + dy
  end
end

-- blitz_wait_timeout: commander-only HUD for the progress-based GO timeout while
-- holding in blitz_wait. Stashed by the attack.lua blitz_wait commander branch
-- as state._blitz_wait_viz each tick it waits. Shows ready/total, the effective
-- timeout (base + progress extension), elapsed/remaining, the closest pending
-- soldier's walk dist (min_bd) with CLOSING/STALLED, and a bar of elapsed vs
-- effective timeout (green while a soldier is still closing so the timeout keeps
-- extending; red when stalled / about to fire GO).
function M.draw_blitz_wait_timeout(state, info, now)
  if not viz.is_on("blitz_wait_timeout") then return end
  local v = state._blitz_wait_viz
  if not v or (now - (v.tick or 0)) > 2 then return end   -- only while actively waiting
  local x, y, dy = 480, 320, 14
  local elapsed  = now - (v.ready_since or now)
  local eff      = v.eff_timeout or 150
  local rem      = math.max(0, eff - elapsed)
  local closing  = v.min_bd and v.prog_bd and v.min_bd < v.prog_bd
  viz.hud_text("blitz_wait_timeout", x, y, "-- BLITZ WAIT: GO timeout --", "topright", 210, 210, 210, 255)
  y = y + dy
  viz.hud_text("blitz_wait_timeout", x, y, string.format("pill#%s   ready %d/%d", tostring(v.pill), v.ready or 0, v.total or 0),
               "topright", 200, 220, 200, 255)
  y = y + dy
  -- Party size vs the HARD quorum: no GO path fires below MIN, and on the
  -- timeout a short-handed commander abandons the take instead of charging.
  do
    local party = 1 + (v.total or 0)
    local short = party < (v.blitz_min or 2)
    local short_txt = v.bo_hold and "  SHORT -> extend at timeout" or "  SHORT -> abandon at timeout"
    viz.hud_text("blitz_wait_timeout", x, y,
                 string.format("party %d/%d (max %d, %s)%s", party, v.blitz_min or 2,
                               v.blitz_max or 2, tostring(v.blitz_src or "default"),
                               short and short_txt or ""),
                 "topright", short and 240 or 200, short and 160 or 220, short and 90 or 200, 255)
    y = y + dy
  end
  -- Blitz-only hold (C.BLITZ_ONLY_EXTEND_WAIT): GO needs the PARKED set
  -- (commander + soldiers at their spots) >= MIN; each timeout short of it
  -- adds one more READY_TIMEOUT, up to BLITZ_ONLY_EXTEND_MAX, then abandons.
  if v.bo_hold then
    local parked = v.parked or 1
    local pshort = parked < (v.blitz_min or 2)
    viz.hud_text("blitz_wait_timeout", x, y,
                 string.format("parked %d/min %d (blitzonly: GO needs parked>=min)  ext %d/%d",
                               parked, v.blitz_min or 2, v.bo_ext_n or 0, C.BLITZ_ONLY_EXTEND_MAX or 3),
                 "topright", pshort and 240 or 140, pshort and 160 or 230, pshort and 90 or 140, 255)
    y = y + dy
  end
  -- Contested: a hostile tank within BLITZ_CONTESTED_RANGE of the pill turns
  -- BLITZ_CONTESTED_SUICIDERS (1) of the blitzers into a temporary suicider at
  -- GO, so it belongs right under the party line it changes the meaning of.
  do
    local c = v.contested
    viz.hud_text("blitz_wait_timeout", x, y, M.blitz_contested_label(c), "topright",
                 c and 240 or 170, c and 140 or 180, c and 140 or 170, 255)
    y = y + dy
  end
  viz.hud_text("blitz_wait_timeout", x, y, string.format("wait %d/%d  rem %d (%.1fs)  base %d +ext %d",
               elapsed, eff, rem, rem / 50.0, v.base_timeout or 150, v.ext or 0),
               "topright", 200, 200, 200, 255)
  y = y + dy
  viz.hud_text("blitz_wait_timeout", x, y, string.format("min_bd %s (prog %s)  %s",
               tostring(v.min_bd or "-"), tostring(v.prog_bd or "-"), closing and "CLOSING" or "STALLED"),
               "topright", closing and 140 or 240, closing and 230 or 170, closing and 140 or 80, 255)
  y = y + dy
  -- Source of the progress signal: live (we can see all pending soldiers) vs a
  -- fallback on an unseen soldier's broadcast bd, and whether we're actively
  -- querying it for a fresh position in the final window.
  local src = v.querying and "QUERYING unseen..." or (v.unseen and "unseen (broadcast bd)" or "live positions")
  viz.hud_text("blitz_wait_timeout", x, y, src, "topright",
               v.querying and 255 or 170, v.querying and 220 or 180, v.querying and 90 or 170, 255)
  y = y + dy
  -- Bar: elapsed vs effective timeout, filling left->right within the track.
  local barw = 220
  local frac = math.min(1.0, elapsed / math.max(1, eff))
  local fillw = math.max(1, math.floor(barw * frac))
  viz.hud_rect("blitz_wait_timeout", x, y, barw, 9, "topright", 60, 60, 60, 160, true)        -- track
  local fr = (closing and not v.timed_out) and 120 or 235
  local fg = (closing and not v.timed_out) and 220 or 120
  viz.hud_rect("blitz_wait_timeout", x + (barw - fillw), y, fillw, 9, "topright", fr, fg, 90, 230, true)
end

-- blitz_joinable: HUD list of every ongoing blitz call this bot knows about
-- (the state.blitz_calls registry) plus whether it can join each one. Mirrors
-- the soldier join-scan gating (in help range, a free squad slot, current goal
-- interruptible) so what's shown matches what the bot would actually join.
-- Also rings each known pill on the map: green = joinable, grey = known but not
-- joinable. The cmdr column is prefixed '>' for the call we're committed to and
-- '~' for the one we're negotiating with.
function M.draw_blitz_joinable(state, info, world, now)
  if not viz.is_on("blitz_joinable") then return end
  local calls   = state.blitz_calls
  local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
  local cap     = M.blitz_soldier_cap()   -- party MAX minus the commander
  local self_pn = info.player_number or -1
  local max_age = C.SQUAD_ALLY_MAX_AGE or 1750

  -- Live per-commander member counts (soldier broadcasts), excluding any
  -- soldier that died since its last broadcast (slot still lingers active).
  local members = {}
  local dead = state.tank_dead_at
  for pn, slot in ally_state.iter_active(now, max_age) do
    local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
    if pn ~= self_pn and slot.info.role == "s" and slot.info.cmdr and not is_dead then
      local c = tonumber(slot.info.cmdr)
      if c then members[c] = (members[c] or 0) + 1 end
    end
  end

  local x, y, dy = 480, 230, 14
  -- The "slot" column below is soldiers/cap; name the party size that produced
  -- that cap (and the quorum a call must reach to fire) right in the header.
  viz.hud_text("blitz_joinable", x, y,
               string.format("-- JOINABLE BLITZES (%s) --", M.blitz_size_label()),
               "topright", 210, 210, 210, 255)
  y = y + dy
  viz.hud_text("blitz_joinable", x, y, string.format("%-5s %-5s %-4s %-5s %s", "cmdr", "pill", "dist", "slot", "disc"),
               "topright", 170, 170, 170, 255)
  y = y + dy

  local n = 0
  for cmdr, call in pairs(calls or {}) do
    n = n + 1
    local pp  = call.pill and world and world.pills and world.pills[call.pill] or nil
    local tag = (cmdr == state.squad_blitz_accepted) and ">C"
                or (cmdr == state.squad_negotiate_cmdr and "~C" or " C")
    local slots = string.format("%d/%d", members[cmdr] or 0, cap)
    -- Every known open call is a joinable candidate now (the join discount, not
    -- a range gate, decides whether we actually go) — so draw all rows + rings
    -- GREEN. The old grey "far"/"pill gone" states were confusing; "far" no
    -- longer exists, and FULL/busy stay as status TEXT (still green).
    local dist, disc, r, g2, b = "-", "-", 120, 230, 120
    if pp then
      dist = tostring(math.abs(pp.mx - tmx) + math.abs(pp.my - tmy))
      -- Join-discount multiplier the goal pool would apply to this call (mirror
      -- of goals.blitz_join_factor — keep in sync): exponential on the dijkstra
      -- path cost to the pill, MIN at 0 → 1.00 (no discount) at FULL_TILES.
      -- Lower = stronger pull to join; x1.00 = too far to bother.
      local bf, dpath = info.inboat and 1 or 0, math.huge
      for dy2 = -1, 1 do for dx2 = -1, 1 do if dx2 ~= 0 or dy2 ~= 0 then
        local c = cpf.smart_cost_dij_only(cpf.KIND_NORMAL, pp.mx + dx2, pp.my + dy2, bf)
        if c and c < dpath then dpath = c end
      end end end
      local minmul = C.SQUAD_BLITZ_JOIN_MIN_MULT  or 0.25
      local full_t = C.SQUAD_BLITZ_JOIN_FULL_TILES or 20
      local f
      if dpath >= full_t or dpath >= 9999 then f = 1.0
      elseif dpath <= 0 then                   f = minmul
      else                                     f = minmul ^ (1.0 - dpath / full_t) end
      disc = string.format("x%.2f", f)
      if viz.circle then
        viz.circle("blitz_joinable", pp.mx + 0.5, pp.my + 0.5, 0.9, 120, 230, 120, 220)
      end
    end
    viz.hud_text("blitz_joinable", x, y, string.format("%-5s %-5s %-4s %-5s %s",
                 tag .. tostring(cmdr), "#" .. tostring(call.pill), dist, slots, disc),
                 "topright", r, g2, b, 255)
    y = y + dy
  end
  if n == 0 then
    viz.hud_text("blitz_joinable", x, y, "(none)", "topright", 140, 140, 140, 255)
  end
end

-- blitz_comm_lines: map lines showing the blitz negotiation channel.
--   Commander view (we're running a call): yellow out to every in-range tank
--     with no answer yet; green to accepted soldiers; orange to one that
--     answered but isn't locked; a declined (No) tank's line is hidden.
--   Soldier view (we're answering): one line to our commander — yellow while
--     answering, orange once we have a standoff offer, green once accepted.
function M.draw_blitz_comm(state, info, world, now)
  if not viz.is_on("blitz_comm_lines") or not viz.line then return end
  local self_pn = info.player_number or -1
  -- Exact tank position in tile units (world wu / 256), not the snapped
  -- tile center — lines anchor on the real tank, not its tile.
  local smx, smy = info.tankx / 256.0, info.tanky / 256.0
  -- ALLIED tank positions by player number, from our own game view. Filter out
  -- hostiles — blitz comm lines are an ally-only concept; without this the
  -- commander view drew "call" lines to enemy tanks that happened to be in range.
  local pos = {}
  if info.objects then
    for _, ob in ipairs(info.objects) do
      if ob.type == 0 and ob.idnum ~= nil and (bit.band(ob.info, OBJECT_HOSTILE)) == 0 then  -- allied OBJECT_TANK
        pos[ob.idnum] = { ob.x / 256.0, ob.y / 256.0 }
      end
    end
  end

  -- A) We're LEADING a blitz (goal._blitz, not following anyone) → lines out to
  -- in-range tanks. Keyed off the blitz flag (matches the bco "open call"
  -- signal) rather than squad_role, which can flicker mid-take.
  local g = state.goal
  if g and g.kind == "attack_pill" and g._blitz and not state.squad_cmdr and g.mx then
    local rng = C.SQUAD_HELP_RANGE or 30
    local accepted, rostered = {}, {}
    for s in string.gmatch(state.squad_blitz_accept or "", "%d+") do accepted[tonumber(s)] = true end
    for _, e in ipairs(state.squad_blitz_roster or {}) do rostered[e.pn] = e end
    for pn, p in pairs(pos) do
      if pn ~= self_pn and (math.abs(p[1] - g.mx) + math.abs(p[2] - g.my)) <= rng then
        local e = rostered[pn]
        local r, gn, b = 230, 230, 60                   -- yellow: call out, no answer
        if e and e.rejected then r, gn, b = 230, 60, 60 -- red: declined (No) — shown, not hidden
        elseif accepted[pn] then r, gn, b = 0, 220, 0   -- green: accepted, going in
        elseif e and e.answered then r, gn, b = 255, 160, 0 end  -- orange: answered, not locked
        viz.line("blitz_comm_lines", smx, smy, p[1], p[2], r, gn, b, 200)
      end
    end
  end

  -- B) We're a soldier answering → one line to our commander.
  local cmdr = state.squad_blitz_accepted or state.squad_negotiate_cmdr
  if cmdr and pos[cmdr] then
    local cp = pos[cmdr]
    local r, gn, b
    if state.squad_blitz_accepted then        r, gn, b = 0, 220, 0       -- green: accepted
    elseif state.squad_blitz_engage_mx then    r, gn, b = 255, 160, 0     -- orange: have offer
    else                                       r, gn, b = 230, 230, 60 end -- yellow: answering
    viz.line("blitz_comm_lines", smx, smy, cp[1], cp[2], r, gn, b, 220)
  end

  -- C) Transient decline: we said "busy"/no-spot to a call this or a recent
  -- tick. Latched for BLITZ_COMM_LATCH_TICKS so a one-tick reject is actually
  -- visible (red line to the commander we declined). Skipped if we now have an
  -- active (yellow/orange/green) line to that same commander.
  local rej = state._blitz_comm_reject
  if rej and rej.cmdr and rej.cmdr ~= cmdr and pos[rej.cmdr]
     and (now - (rej.tick or -100000)) <= (C.BLITZ_COMM_LATCH_TICKS or 18) then
    local cp = pos[rej.cmdr]
    viz.line("blitz_comm_lines", smx, smy, cp[1], cp[2], 230, 60, 60, 220)
  end
end

-- hard_take_pills: ring + HP on enemy pills at/above HARD_TAKE_MIN_HP — the
-- targets that spawn a commander+squad when DYNAMIC_COMMANDERS is on.
function M.draw_hard_takes(world, now)
  if not viz.is_on("hard_take_pills") or not viz.circle or not world or not world.pills then return end
  local minhp = C.HARD_TAKE_MIN_HP or 12
  for _, p in pairs(world.pills) do
    if (p.owner == "hostile" or p.owner == "neutral") and (p.health or 0) >= minhp then
      viz.circle("hard_take_pills", p.mx + 0.5, p.my + 0.5, 1.0, 255, 150, 60, 180)
      if viz.text then
        viz.text("hard_take_pills", p.mx + 0.5, p.my - 0.8, "HARD " .. p.health,
                 "center", 255, 150, 60, 225)
      end
    end
  end
end

return M
