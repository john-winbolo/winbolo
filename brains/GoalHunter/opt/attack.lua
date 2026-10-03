local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/attack.lua — pill attack position planning + substate machines
-- =========================================================================

local C      = require("constants")
local TAG    = "[" .. C.BRAIN_NAME .. "]"
local U      = require("util")
local PF     = require("pathfinder")
local cpf    = require("cpathfinder")
local log    = require("logger")
local threat = require("threat")
local danger = require("danger")
local shot_tracker = require("shot_tracker")
local shield = require("attack_shield")
local viz    = require("viz")
local squad      = require("squad")
local ally_state = require("ally_state")
local SM         = require("spot_margin")  -- blitz spot LOS margin (2026-09-24)

local smart_cost = cpf.smart_cost
local KIND_PILL   = cpf.KIND_PILL
local KIND_NORMAL = cpf.KIND_NORMAL

local print2 = require("print2")
local opt    = require("optimize")
-- clock_us is registered as a C global. Cache the upvalue and provide
-- a no-op fallback so unit-test runs without the C host don't crash.
local clock_us = clock_us or function() return 0 end
print("[attack] loaded from: " .. tostring(debug.getinfo(1, "S").source))

local M = {}

local PRE_ENGAGE_SUBS = {
  plan_position=true, approach=true, gather_trees=true, build_walls=true,
  aim=true, in_range_position=true, in_range_aim_pre=true,
  in_range_aim=true, in_range_aim_finetune=true, detree=true,
  blitz_wait=true,
}

-- Substates where a blitz COMMANDER is still converging on its standoff but is
-- NOT yet parked in blitz_wait. The substate-independent early-GO uses this so a
-- slow commander rushes in when enough SOLDIERS are already parked waiting on it.
-- Excludes plan_position (standoff not finalized) and blitz_wait (its own handler).
local BLITZ_EARLY_GO_ENROUTE_SUBS = {
  gather_trees=true, approach=true, build_walls=true, detree=true,
  aim=true, in_range_position=true, in_range_aim_pre=true,
  in_range_aim=true, in_range_aim_finetune=true,
}

-- "Effectively stopped" gate for the substate transitions in approach
-- and in_range_position. Returns true if EITHER the reported speed is
-- at/below speed_tol OR the tank's wu position hasn't changed for the
-- last `still_ticks` ticks. Catches the corner case where info.speed
-- reads a low value (e.g. 4) but the tank is no longer making any
-- actual progress (terrain friction, wedged against a tile boundary,
-- etc.). Per-key trackers live on `state.attack_motion` so they
-- survive the per-tick goal-table swap without polluting the goal.
-- Pick which side to swerve toward based on perpendicular cover.
-- Samples 2 tiles perpendicular to the tank->pill line, walks each
-- back to the pill counting cover (forest/walls/friendly pills) and
-- subtracting hazards (water/swamp/rubble/enemy bases). The side with
-- more cover wins. Sets goal._best_swerve_dir (1=LEFT-sample,
-- -1=RIGHT-sample) and goal._swerve_viz for the HUD overlay.
-- Called from BOTH the legacy aim->swerve path AND PPT shoot_pill->
-- swerve so PPT doesn't fall back to a random direction.
local function compute_best_swerve_dir(goal, world, pmx, pmy, tmx, tmy)
  local dx = pmx - tmx
  local dy = pmy - tmy
  local len = math.sqrt(dx * dx + dy * dy)
  if len <= 0.01 then return end
  local ux, uy = dx / len, dy / len
  local tcx, tcy = tmx + 0.5, tmy + 0.5
  local pcx, pcy = pmx + 0.5, pmy + 0.5
  local lfx = tcx + (-uy) * 2
  local lfy = tcy + ux * 2
  local rfx = tcx + uy * 2
  local rfy = tcy + (-ux) * 2
  -- Push the far end of each cover line SWERVE_COVER_EXTEND_TILES further
  -- out along the pill->sample direction. The pill end is the origin and
  -- stays put; only the outer end grows, so the walk also scores the tile
  -- just beyond the sample point (cover or hazard sitting right where the
  -- swerve arc ends). Viz uses the extended points so the overlay is the
  -- line actually walked.
  do
    local ext = C.SWERVE_COVER_EXTEND_TILES or 0.5
    local function extend(fx, fy)
      local ex, ey = fx - pcx, fy - pcy
      local l = math.sqrt(ex * ex + ey * ey)
      if l <= 0.01 then return fx, fy end
      return fx + ex / l * ext, fy + ey / l * ext
    end
    lfx, lfy = extend(lfx, lfy)
    rfx, rfy = extend(rfx, rfy)
  end

  local fire_r2 = C.PILL_FIRE_RANGE * C.PILL_FIRE_RANGE
  local function in_fire_range(bx, by)
    local ddx = bx - pmx
    local ddy = by - pmy
    return ddx * ddx + ddy * ddy <= fire_r2
  end
  local function count_cover(fx0, fy0)
    local n = 0
    U.line_walk(fx0, fy0, pcx, pcy, function(bx, by)
      if not U.in_map(bx, by) then return end
      local tt = U.ttype(bx, by)
      -- pill_at[k] is a list of {id=,pill=} records (see world.lua:32) —
      -- old code did world.pills[<list>] which is always nil, so the
      -- friendly_pill cover bonus never fired.
      local plist = world.pill_at[by * 256 + bx]
      local friendly_pill = false
      if plist then
        for _, e in ipairs(plist) do
          if e.pill and e.pill.owner == "friendly" then
            friendly_pill = true; break
          end
        end
      end
      local base_entry = world.base_at[by * 256 + bx]
      local enemy_base = base_entry and base_entry.base
                         and base_entry.base.owner == "hostile"
      if in_fire_range(bx, by) then
        if tt == C.T_FOREST or tt == C.T_BUILDING or tt == C.T_HALFBUILD then
          n = n + 1
        end
        if friendly_pill then n = n + 3 end
        if enemy_base then n = n - 10 end
      else
        if tt == C.T_BUILDING or tt == C.T_HALFBUILD
           or tt == C.T_RIVER  or tt == C.T_DEEPSEA
           or tt == C.T_SWAMP  or tt == C.T_RUBBLE then
          n = n - 10
        end
        if friendly_pill then n = n - 10 end
        if enemy_base then n = n - 10 end
      end
    end)
    return n
  end

  local left_cover  = count_cover(lfx, lfy)
  local right_cover = count_cover(rfx, rfy)

  -- Deep-water penalty on the swerve retreat path. The tank swerves
  -- perpendicular then retreats outward from standoff to the setup
  -- circle. Check each side's retreat line (from standoff radius to
  -- approach radius along the swerve direction) for deep water.
  local standoff_r = goal._is_ppt and C.PPT_STANDOFF or C.ATTACK_PILL_STANDOFF
  local setup_r    = standoff_r + C.ATTACK_APPROACH_OFFSET
  local DEEP_WATER_PENALTY = -50
  local function check_retreat_deepsea(perp_x, perp_y)
    local penalty = 0
    -- Walk from standoff radius to setup radius along the swerve
    -- direction (perpendicular + outward from pill).
    -- Sample every 0.5 tiles along the retreat line.
    local steps = math.ceil((setup_r - standoff_r) / 0.5)
    for i = 0, steps do
      local t = standoff_r + (setup_r - standoff_r) * i / steps
      -- retreat point: pill center + swerve-perpendicular offset + outward
      local rx = pcx + perp_x * 2 + ux * t
      local ry = pcy + perp_y * 2 + uy * t
      local rmx = math.floor(rx)
      local rmy = math.floor(ry)
      if U.in_map(rmx, rmy) and U.ttype(rmx, rmy) == C.T_DEEPSEA then
        penalty = penalty + DEEP_WATER_PENALTY
      end
    end
    return penalty
  end

  left_cover  = left_cover  + check_retreat_deepsea(-uy, ux)
  right_cover = right_cover + check_retreat_deepsea(uy, -ux)

  goal._best_swerve_dir = left_cover >= right_cover and 1 or -1
  goal._swerve_viz = {
    lfx = lfx, lfy = lfy, left_cover = left_cover,
    rfx = rfx, rfy = rfy, right_cover = right_cover,
    chosen = goal._best_swerve_dir,
    pcx = pcx, pcy = pcy,
  }
end

-- Enter swerve substate. Centralises the duplicated swerve-entry
-- setup (timing, direction, pill-dead flag) so all 6 entry points
-- (charge-kill, charge-defensive, engage-kill, engage-dodge,
-- shoot_pill, lgm-near) share one path.
--
-- A pill_suicider NEVER gets here: every call site is gated on
-- `not state.is_pill_suicider` (defensive swerves are cancelled, the
-- pill-dead "kill" swerves run suicider_kill_handoff instead). This is
-- the single choke point that sets substate="swerve", so the
-- BRAIN_DEBUG_MODE SWERVE_ENTER line below is the proof of that rule:
-- a suicider must emit zero SWERVE_ENTER lines over a whole game.
--
-- mode:
--   "kill"      — pill dead or enough shots fired (offensive swerve)
--   "defensive" — pill still alive, dodging return fire
local LOW_HP_SWERVE = { [1] = 30, [2] = 36, [3] = 40 }
local function enter_swerve(goal, world, state, info, pmx, pmy, mode)
  local now = state.tick or 0
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  goal.substate    = "swerve"
  goal._swerve_start = now
  if mode == "kill" then
    local low_hp_ticks = LOW_HP_SWERVE[goal._charge_start_hp]
    if low_hp_ticks then
      goal._swerve_ticks_left      = low_hp_ticks
      goal._swerve_turn_ticks_left = math.min(low_hp_ticks, C.SWERVE_TURN_TICKS)
    elseif goal._kill_attempt then
      goal._swerve_ticks_left      = C.SWERVE_TOTAL_TICKS
      goal._swerve_turn_ticks_left = C.SWERVE_TURN_TICKS
    else
      goal._swerve_ticks_left      = C.SWERVE_DEFENSIVE_TOTAL_TICKS
      goal._swerve_turn_ticks_left = C.SWERVE_DEFENSIVE_TURN_TICKS
    end
    goal._swerve_pill_dead = true
  else
    goal._swerve_ticks_left      = C.SWERVE_DEFENSIVE_TOTAL_TICKS
    goal._swerve_turn_ticks_left = C.SWERVE_DEFENSIVE_TURN_TICKS
    goal._swerve_pill_dead       = false
  end
  if not goal._best_swerve_dir then
    compute_best_swerve_dir(goal, world, pmx, pmy, tmx, tmy)
  end
  goal._swerve_dir = goal._best_swerve_dir or ((now % 2 == 0) and 1 or -1)
  goal._engage_hits = nil
  -- Re-baseline the charge defensive-swerve counters so a charge RESUMED after
  -- this swerve starts fresh (no instant re-swerve from the old accumulated hits).
  goal._charge_hits_total = nil
  goal._charge_armour      = nil
  -- Threat-driven lifecycle: the swerve starts UNLOADED and arms the first tick a
  -- shell is inside the swerve circle (or predicted to land inside it). The
  -- armed/unloaded/timeout logic lives in the swerve substate handler.
  goal._swerve_armed = nil
end

-- Clear the active goal, idle the pathfinder, and wipe ALL transient
-- attack-side state that downstream code shouldn't see after we
-- abandon. Centralises the bug class where one site clears 25
-- fields by hand and another site clears just `kind`, leaving stale
-- _shield_scan / _aim_locked / etc. to leak into the next goal that
-- happens to inherit the same goal table.
--
-- Mutates state.goal IN PLACE (preserves table identity) so callers
-- that pre-cached `local goal = state.goal` continue to see "none"
-- on subsequent reads. Only the core identity fields are kept so
-- the new goal selector can re-derive everything else cleanly.
local CORE_GOAL_FIELDS = {
  kind = true, mx = true, my = true, wx = true, wy = true,
  target_id = true,
}
-- no_urgent: when true, the resulting goal=none should NOT force an urgent
-- (this-tick) replan — it rides the normal goal timer instead. Used for
-- routine invalidations (capture_base/attack_pill target gone) where an
-- immediate expensive re-eval isn't worth it. Default (nil) keeps the
-- existing urgent-on-none behavior for genuine aborts.
local function clear_attack_goal(state, reason, no_urgent)
  -- Snapshot what we're killing BEFORE we wipe it, so the overlay can
  -- show "attack_pill/in_range_aim_finetune cleared at t=N because X".
  local kind_was = state.goal and state.goal.kind or nil
  local sub_was  = state.goal and state.goal.substate or nil
  local mx_was   = state.goal and state.goal.mx or nil
  local my_was   = state.goal and state.goal.my or nil
  if state.goal then
    for k in pairs(state.goal) do
      if not CORE_GOAL_FIELDS[k] then state.goal[k] = nil end
    end
    state.goal.kind = "none"
  else
    state.goal = { kind = "none" }
  end
  -- Blitz lifecycle: a cleared attack goal means the take ended (success or
  -- fail), so stop broadcasting our blitz standoff claim / reported distance.
  -- (goal._blitz_* fields were wiped above; the commander's roster/reject and
  -- squad_blitz_target are recomputed by squad.update each tick.)
  state.squad_blitz_engage_mx, state.squad_blitz_engage_my = nil, nil
  state.squad_blitz_engage_fx, state.squad_blitz_engage_fy = nil, nil
  state.squad_blitz_engage_deg = nil
  state.squad_blitz_in_position = nil
  state.squad_blitz_aimed = nil
  state.squad_blitz_bd = nil
  state._blitz_reject = nil   -- soldier's rejected-standoff set (parallel negotiation)
  -- C.BLITZ_NOSPOT_RENEGOTIATE: the engage spot just went, so the accept that
  -- was given FOR that spot goes too. squad.update then negotiates again (a
  -- fresh bes, checked by the commander's arbiter) instead of re-committing
  -- with no spot and self-scanning (20260925_134920 bot2 t=3044-3079).
  if C.BLITZ_NOSPOT_RENEGOTIATE and state.squad_blitz_accepted then
    state.squad_blitz_accepted = nil
    state._blitz_commit_tick   = nil
  end
  if state.pf then state.pf.status = "idle" end
  -- Auto-derive a caller location if the call site didn't pass a reason,
  -- so the overlay still tells you which line cleared the goal.
  local r = reason
  if not r then
    local info = debug.getinfo(2, "Sl")
    if info then r = string.format("(no reason) called from %s:%d",
                                   info.short_src or "?", info.currentline or 0)
    else r = "(no reason)" end
  end
  state._last_attack_clear = {
    tick = state.tick or 0,
    reason = r,
    kind_was = kind_was, sub_was = sub_was,
    mx_was = mx_was, my_was = my_was,
  }
  -- Mark whether the resulting goal=none may ride the timer (no_urgent) or
  -- must replan immediately. Set on every clear so an urgent abort right
  -- after a non-urgent clear correctly re-enables urgency.
  state._clear_no_urgent = no_urgent and true or nil
  if reason and BRAIN_DEBUG_MODE then
    print(string.format("[clear_attack_goal] %s", reason))
  end
  -- Same reason in the print2 log (print() above is invisible there; the
  -- reason used to live only in the recording's panel via _last_attack_clear).
end
M.clear_attack_goal = clear_attack_goal

-- Stamp a fresh-kill pickup claim: WE just dropped pill `id` to 0 armour.
-- goals.lua Override 3b force-wins capture_pill on this id, overriding
-- refuel/flee/survival entirely (no armour gate). Dedups by id (re-stamping
-- the same kill just refreshes the kill_tick). Cleared on capture / in_tank /
-- TTL expiry by the override, and on tank death by squad.reset_blitz_state.
--
-- Count the tanks COMMITTED to blitzing THIS pill right now: self + every active
-- ally broadcasting attack_pill on the same target that is past negotiation and
-- not dead.  Mirrors squad.blitz_members' field access + dead test
-- (squad.lua blitz_members) but WITHOUT the role/commander filter -- a blitz on
-- one pill can draw tanks from more than one squad, and every body on it counts.
-- Used by the "blitz only when hit" hold (blitz_commit_hold) and by the kill
-- claim's blitz size (kill_blitz_size below).
local function blitz_party_count(state, now, self_pn, target_id)
  local n = 1                                   -- self
  if not target_id then return n end
  local dead = state.tank_dead_at
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= self_pn then
      local h = slot.info
      local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
      if not is_dead and h.goal == "attack_pill"
         and tonumber(h.target or "") == target_id and h.sqst ~= "nego" then
        n = n + 1
      end
    end
  end
  return n
end

-- Blitz size for a kill claim on pill `id` (KILL_PICKUP_PAIR_MIN_SQUAD). The
-- largest party this bot knew for the take: the per-tick running max that
-- update_attack_substate keeps on the goal (goal._kp_party_max) and a live
-- count now. 1 when our goal is not a blitz attack_pill on this pill (a solo
-- take). Read from the GOAL before clear_attack_goal wipes it, so the size
-- survives the squad breaking up right after the kill. Every committed member
-- counts the same set of tanks, so claimers normally agree on the size.
local function kill_blitz_size(state, id, now)
  local g = state.goal
  if not (g and g.kind == "attack_pill" and g.target_id == id) then return 1 end
  local n = g._kp_party_max or 1
  if g._blitz or g._blitz_committed then
    local live = blitz_party_count(state, now, state.player_number or -1, id)
    if live > n then n = live end
  end
  return n
end
M.kill_blitz_size = kill_blitz_size

local function mark_kill_pickup(state, id, mx, my, now)
  if not C.KILL_PICKUP_ENABLED then return end
  if not id or id < 0 then return end
  -- Blitz size, only while the pair rule is on (keel: no scan, no field read).
  local sz = nil
  if (C.KILL_PICKUP_PAIR_MIN_SQUAD or 0) > 0 then sz = kill_blitz_size(state, id, now) end
  local kp = state.kill_pickup
  if kp and kp.id == id then
    kp.kill_tick = now; kp.mx = mx; kp.my = my
    -- Re-stamp (e.g. swerve done): keep the LARGEST size seen. By now the
    -- other members may already be on capture_pill, so a fresh count is low.
    if sz and sz > (kp.squad_n or 1) then kp.squad_n = sz end
    return
  end
  -- created_tick = first time we claimed THIS pill (absolute commitment cap,
  -- since the grabber refreshes kill_tick while driving in). kill_tick is the
  -- rolling TTL anchor. squad_n = blitz size at kill time (1 = solo / rule off).
  state.kill_pickup = { id = id, mx = mx, my = my, kill_tick = now, created_tick = now,
                        squad_n = sz or 1 }
end
M.mark_kill_pickup = mark_kill_pickup

-- Post-kill handoff. The pill we were taking is dead and the swerve has run its
-- course, so hand the goal STRAIGHT to capture_pill on that same pill instead of
-- dropping to goal=none and trusting the next replan to re-derive it. The gap
-- was real: goal=none only replans once REPLAN_MIN_INTERVAL is up, and refuel /
-- flee can win that competition and walk us off a free body we just paid shells
-- for. Installing the goal here costs nothing and closes the gap.
--
-- This does NOT take the decision away from goal_selection: we also raise
-- _force_replan_reason, so the very next tick runs a full replan and Override 3b
-- re-derives the claim with its ally handoff (lowest capture_pill score grabs),
-- its TTL, and its unreachable way-out. This just decides what we do MEANWHILE.
--
-- Refuses, leaving goal=none and the normal selector to sort it out, when:
--   * mark_kill_pickup declined the claim (no target id, or feature off), so
--     Override 3b has nothing to continue with,
--   * the pill is gone from world.pills, alive again, or already in a tank —
--     a carried pill is not capturable and its (mx,my) is stale,
--   * an ally is already broadcasting capture_pill on this pill id: they are
--     driving in on it, and we don't contest a take already under way.
-- The pill_suicider role needs no special case — capture_pill is in
-- goals.lua's SUICIDER_EXEMPT_KINDS (x1), so scooping the pills it kills is
-- part of the job and carries no role multiplier.
local function handoff_to_capture_pill(state, world, info, pid, now)
  if not C.KILL_PICKUP_ENABLED then return false end
  if not pid or pid < 0 then return false end
  local kp = state.kill_pickup
  if not (kp and kp.id == pid) then return false end
  local p = world.pills and world.pills[pid]
  if not p or (p.health or 0) ~= 0 or p.in_tank or p.carrier or p._synth_carry then
    return false
  end
  local self_pn = info.player_number
  -- Pair pickup (KILL_PICKUP_PAIR_MIN_SQUAD): a big enough blitz keeps TWO
  -- capturers, so we skip only when `keep` allies are already capturing it.
  local keep = 1
  local pair_min = C.KILL_PICKUP_PAIR_MIN_SQUAD or 0
  if pair_min > 0 and (kp.squad_n or 1) >= pair_min then keep = 2 end
  local n_on = 0
  for apn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if apn ~= self_pn then
      local si = slot.info
      if si and si.goal == "capture_pill" and tonumber(si.target) == pid then
        n_on = n_on + 1
        if n_on >= keep then
          return false
        end
      end
    end
  end
  -- clear_attack_goal keeps the CORE goal fields (kind/mx/my/wx/wy/target_id)
  -- and wipes everything else, so the table is already the right shape and we
  -- mutate it in place — the decision-snapshot rollback in init.lua treats goal
  -- table IDENTITY as the decision, so we must not swap the table here.
  local g = state.goal
  g.kind      = "capture_pill"
  g.mx, g.my  = p.mx, p.my
  g.wx, g.wy  = U.m2w(p.mx), U.m2w(p.my)
  g.target_id = pid
  g.race_mode = C.CAPTURE_RACE_MODE_CAPTURE
  g.kill_grab = true
  state.goal_set_tick = now
  state._force_replan_reason = state._force_replan_reason or "kill_handoff"
  return true
end
M.handoff_to_capture_pill = handoff_to_capture_pill   -- exported for tests
-- Pill-suicider post-kill handoff. A suicider NEVER swerves, in any situation,
-- so where a normal tank enters the "kill" swerve the tick its pill dies and
-- runs the capture handoff when that swerve FINISHES, the suicider runs the
-- same handoff immediately — no swerve, straight from firing to driving onto
-- the body.
--
-- Mirrors the pill-dead branch of the swerve-completion block exactly:
-- mark_kill_pickup -> clear_attack_goal -> handoff_to_capture_pill, with the
-- same fallback print when the handoff refuses (ally already capturing, pill
-- carried/gone) and the same command_goal semantics — that branch does NOT
-- release state.command_goal on a dead pill (only the pill-ALIVE exits do), so
-- neither do we. _swerve_* bookkeeping is deliberately not written: those
-- fields are read only by the swerve substate handler and the hud_swerve_debug
-- overlay, and clear_attack_goal wipes the whole goal on the next line anyway.
--
-- Callers MUST `return` immediately after calling this: clear_attack_goal
-- mutates state.goal in place (kind="none", every other field wiped), so the
-- caller's cached `goal` local is no longer an attack_pill goal.
-- reason: why we skip the kill swerve and go straight to capture -- either a
-- pill_suicider (default) or, under BLITZ_SWERVE_ONLY_WHEN_HIT, an undamaged
-- committed 3+ blitzer (reason="blitz_hold"): same "commit onto the corpse, no
-- dodge" behaviour, its dodge returns only once it is hit.
local function suicider_kill_handoff(goal, state, world, info, now, site, reason)
  reason = reason or "pill_suicider"
  local pid = goal.target_id
  local gmx, gmy = goal.mx, goal.my
  mark_kill_pickup(state, pid, gmx, gmy, now)
  clear_attack_goal(state, "pill dead, " .. reason .. ": straight to capture")
  local took = handoff_to_capture_pill(state, world, info, pid, now)
  if took then
    print(string.format(TAG ..
      " ATTACK: pill dead, %s: straight to capture (site=%s) — taking capture_pill on it", reason, site))
  else
    print(string.format(TAG ..
      " ATTACK: pill dead, %s: straight to capture (site=%s) — releasing to capture_pill", reason, site))
  end
end

M.enter_swerve      = enter_swerve

local _EMPTY = {}
-- TILES serving as BLOCKERS in this bot's active pill take, broadcast (pblk) so
-- allies don't reposition / capture / repair a friendly pill sitting on one.
-- Two sources, both read straight off the chosen aim of this take's shield scan
-- (no geometric guessing → the set is stable, changing only when the chosen aim
-- does — no per-tick churn):
--   * actual_blockers of kind "friendly_pill" — a pre-existing friendly pill on
--     the firing line between our standoff and the target (we didn't build it,
--     but the aim uses it as cover).
--   * potential_blockers — empty, buildable, LGM-reachable slots this take
--     intends to BUILD a blocker pill on.
-- We broadcast the TILE (packed my*256+mx), not a pill id, on purpose: a built
-- blocker has no id until it materialises, so a tile lets us declare it at
-- intent time. Each client then flags whatever friendly pill is (or gets built)
-- on a declared tile as _in_use — which also covers the vulnerable partial-
-- health build-up window (a half-built blocker is "damaged" → repair-pool bait).
-- Walls aren't pills, so wall actual_blockers are skipped (nothing to flag).
-- Returns a list of packed tile indices (the shared empty table when not in a take).
function M.current_blocker_tiles(state, world)
  local g = state and state.goal
  if not g or g.kind ~= "attack_pill" then return _EMPTY end
  local w = g._shield_scan and g._shield_scan.best
  local aim = w and w.best_aim_idx and w.aims and w.aims[w.best_aim_idx] or nil
  if not aim then return _EMPTY end
  local out
  local function add(b)
    if b.mx and b.my then
      out = out or {}
      out[#out + 1] = b.my * 256 + b.mx
    end
  end
  if aim.blockers then
    for _, b in ipairs(aim.blockers) do
      if b.kind == "friendly_pill" then add(b) end
    end
  end
  if aim.potential_blockers then
    for _, b in ipairs(aim.potential_blockers) do add(b) end
  end
  return out or _EMPTY
end

-- Shield WALL tiles this take is BUILDING, broadcast (bwl) ONLY while in
-- build_walls so blitz soldiers route their shot AROUND them — a freshly built
-- friendly wall blocks a soldier's shell exactly like an enemy one, and the wall
-- isn't in the soldier's world map until it materialises. Distinct from
-- current_blocker_tiles (pblk): that's the friendly-PILL repair-protection set;
-- this is the WALL set (potential_blocker slots being laid + any non-pill, i.e.
-- wall, actual blockers used as cover). Gated to build_walls because an early-
-- joiner take aborts the shield (BLITZ_ABORT_BUILD_ON_READY) and builds nothing.
-- Returns packed tile indices (my*256+mx), or the shared empty table.
function M.current_shield_wall_tiles(state, world)
  local g = state and state.goal
  if not g or g.kind ~= "attack_pill" or g.substate ~= "build_walls" then return _EMPTY end
  local w = g._shield_scan and g._shield_scan.best
  local aim = w and w.best_aim_idx and w.aims and w.aims[w.best_aim_idx] or nil
  if not aim then return _EMPTY end
  local out
  local function add(b)
    if b.mx and b.my then out = out or {}; out[#out + 1] = b.my * 256 + b.mx end
  end
  if aim.potential_blockers then for _, b in ipairs(aim.potential_blockers) do add(b) end end
  if aim.blockers then for _, b in ipairs(aim.blockers) do if b.kind ~= "friendly_pill" then add(b) end end end
  return out or _EMPTY
end

-- Skip the swerve/curve-away when we can simply TANK the rest of the kill:
-- while the pill is still alive and pill.health * SWERVE_SKIP_ARMOUR_PER_HP
-- <= our armour, we can absorb finishing it, so buck in and keep firing
-- instead of peeling off (which costs shots/time). Only true for a live pill —
-- once it's dead the normal swerve still runs to exit/capture.
local function can_tank_finish(pill, info)
  -- Tank the finish ONLY when ALL hold: pill nearly dead (HP <= 3), pill
  -- currently calm (anger <= 0.34 this instant), AND armour is sufficient to
  -- soak it (hp * per_hp <= armour). Otherwise swerve as usual.
  local hp = pill and (pill.health or 0) or 0
  if hp <= 0 then return false end
  local anger = pill and (pill.anger or 0) or 0
  return hp <= (C.TANK_FINISH_MAX_HP or 3)
     and anger <= (C.TANK_FINISH_MAX_ANGER or 0.34)
     and hp * (C.SWERVE_SKIP_ARMOUR_PER_HP or 5) <= (info.armour or 0)
end
M.can_tank_finish = can_tank_finish

-- One-time COMMIT decision (made at build_walls / charge, or lazily at first
-- fire): when this pill is down to its last HP, will we SOAK the finish (skip
-- the swerve and eat the last shots) or always peel off? Decided ONCE and
-- stored on the goal so the firing substates don't re-decide every tick.
-- Soak only if armour can cover finishing a pill AND the firing spot isn't in
-- heavy pill crossfire AND no enemy tank can hit us — the old per-tick check
-- looked only at the target pill, so a tank could soak into a defended cluster
-- (crossfire / enemy tank) and die. print2-logged at the moment of decision.
local function commit_soak_finish(goal, state, info)
  if goal._soak_finish ~= nil then return goal._soak_finish end
  local per_hp  = C.SWERVE_SKIP_ARMOUR_PER_HP or 5
  local need    = (C.TANK_FINISH_MAX_HP or 3) * per_hp
  local fx      = goal.standoff_mx or (bit.rshift(info.tankx, 8))
  local fy      = goal.standoff_my or (bit.rshift(info.tanky, 8))
  local pdang   = threat.pill_at(fx, fy) or 0
  local n_tanks = (state.perc and state.perc.enemy_tanks) and #state.perc.enemy_tanks or 0
  local armour  = info.armour or 0
  local ok, why
  if armour < need then ok, why = false, "armour_low"
  elseif pdang >= (C.TANK_COMBAT_DEFENDED_DANGER or 30) then ok, why = false, "crossfire"
  elseif n_tanks > 0 then ok, why = false, "enemy_tank"
  else ok, why = true, "ok" end
  goal._soak_finish = ok
  return ok
end
M.commit_soak_finish = commit_soak_finish

-- Returns a non-nil reason string when current armour vs pill HP make
-- pressing on with the take unsafe.  Used at the start of approach /
-- build_walls / charge to abort attack_pill early instead of dying
-- mid-charge.  The reason text feeds clear_attack_goal so the left-top
-- "last attack cleared" overlay shows WHY we bailed.
local function armour_unsafe_for_pill_take(info, pill_hp, blitz_2plus, goal)
  -- On a true blitz with >= 2 tanks the ally shares the incoming fire, so don't
  -- abort the take on low armour — even armour 0 presses on, the partner helps.
  if blitz_2plus then return nil end
  -- CHARGE NOW IGNORES SAFETY: a person said go in; armour is not a reason
  -- to stop (C.CHARGE_NOW_IGNORE_SAFETY).
  if goal and goal._charge_now and C.CHARGE_NOW_IGNORE_SAFETY then return nil end
  if not pill_hp or pill_hp < C.ATTACK_PILL_UNSAFE_HP_THRESHOLD then return nil end
  local arm = info and info.armour or 0
  if arm >= C.ATTACK_PILL_UNSAFE_ARMOUR_FLOOR then return nil end
  return string.format("armour_too_low: arm=%d (need >= %d) vs pillHP=%d (>= %d)",
                       arm, C.ATTACK_PILL_UNSAFE_ARMOUR_FLOOR,
                       pill_hp, C.ATTACK_PILL_UNSAFE_HP_THRESHOLD)
end
M.armour_unsafe_for_pill_take = armour_unsafe_for_pill_take

-- True when our take's target pill is an ally's ACTIVE blitz call and we haven't
-- been accepted into that blitz (and we don't command it). Gates plan_position ->
-- approach: hold and negotiate a coordinated standoff FIRST, instead of barreling
-- onto the pill uninvited as a rogue solo attacker (which is how multiple tanks
-- end up piling on one blitz pill). Resolves the moment the commander accepts us
-- (squad_blitz_accepted -> approach) or its call closes / we're rejected (re-plan).
local function blitz_join_unaccepted(state, info)
  local g = state.goal
  if not (g and g.target_id and g.target_id >= 0 and state.blitz_calls) then return false end
  if state.squad_role == "c" then return false end       -- we command this take
  if state.squad_blitz_accepted then return false end     -- already an accepted soldier
  local self_pn = info and info.player_number
  for cmdr, call in pairs(state.blitz_calls) do
    if cmdr ~= self_pn and call.pill == g.target_id then return true end
  end
  return false
end
M.blitz_join_unaccepted = blitz_join_unaccepted

-- STILL_POS_TOL: max world-unit drift over the still-window that
-- still counts as "stopped". Without this, a 1-wu-per-tick jitter
-- (common on tree/swamp tiles where info.speed lies about actual
-- ground motion) keeps resetting `since` and effectively_stopped
-- never returns true even when the tank is visually frozen.
-- 4 wu = ¼ game-pixel — well below anything that matters for aim.
local STILL_POS_TOL = 4
local function effectively_stopped(state, info, now, speed_tol, still_ticks, key)
  if info.speed <= speed_tol then return true end
  state.attack_motion = state.attack_motion or {}
  local sub = state.attack_motion[key] or {}
  state.attack_motion[key] = sub
  -- Anchor on first call OR when drift exceeds tolerance.
  if not sub.wx then
    sub.wx, sub.wy, sub.since = info.tankx, info.tanky, now
    return false
  end
  local dx = info.tankx - sub.wx
  local dy = info.tanky - sub.wy
  if dx * dx + dy * dy > STILL_POS_TOL * STILL_POS_TOL then
    sub.wx, sub.wy, sub.since = info.tankx, info.tanky, now
    return false
  end
  return (now - sub.since) >= still_ticks
end

-- Attack pill constants (from constants.lua)
-- ATTACK_PILL_STANDOFF: distance to stand from pill
-- ATTACK_PILL_RANGE: max distance to start shooting
-- ATTACK_CURVE_AFTER_HITS: hits before curving away
-- ATTACK_CURVE_TICKS: duration of curve-away maneuver

-- Find a pill at (mx, my) via spatial index. Returns the pill entry or nil.
-- Emit the candidate-spot overlay (LOS box, danger box, maneuver
-- ellipse, maneuver tiles, score label) for a list of spots around a
-- pill. Mirrors the inline draw block at the bottom of
-- update_attack_substate so the same visual style is used for both
-- the goal pill (where show_all gates non-chosen) and the per-pill
-- emission from the pool-6 evaluator (where every spot draws).
-- viz_id picks which V-dialog row gates the emission.
-- mode (optional): "all" | "bucket" | "winner". Default "all" draws
-- every spot. "bucket" filters to s.in_bucket; "winner" filters to
-- s.deg == chosen_deg.
-- alpha_scale (optional, default 1.0): multiplier on every alpha so
-- the goal-pill emission can render bolder than the pool-6 candidates
-- without duplicating the renderer.
function M.draw_pill_eval_spots(spots, pmx, pmy, viz_id, mode, chosen_deg, alpha_scale)
  if not spots then return end
  if not viz.is_on(viz_id) then return end
  mode = mode or "all"
  alpha_scale = alpha_scale or 1.0
  local safe_r = C.ATTACK_SAFE_RADIUS
  local n_detail = 0   -- clickable D-detail entries recorded this call (for the print2 marker)
  local function a(v)
    local x = math.floor(v * alpha_scale + 0.5)
    if x > 255 then x = 255 elseif x < 0 then x = 0 end
    return x
  end
  for _, s in ipairs(spots) do
    if s.cx
       and (mode ~= "bucket" or s.in_bucket)
       and (mode ~= "winner" or s.deg == chosen_deg) then
      -- Inner box: green=LOS, red=no LOS
      local cr, cg = s.has_los and 0 or 200, s.has_los and 200 or 0
      if s.has_los then
        local sr, sg = (s.total_score or 999) < 10 and 0 or 255,
                       (s.total_score or 999) < 10 and 200 or 165
        if s.maneuver_tiles then
          for _, t in ipairs(s.maneuver_tiles) do
          end
        end
        -- Maneuver ellipse outline. Radii match score_attack_spot
        -- (r_long = ATTACK_SAFE_RADIUS, r_short = that / 3) so the
        -- drawn shape tracks the actual scoring geometry.
        do
          local edx = pmx + 0.5 - s.cx
          local edy = pmy + 0.5 - s.cy
          local elen = math.sqrt(edx * edx + edy * edy)
          if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
          local ux, uy = edx / elen, edy / elen
          local vx, vy = -uy, ux
          local rl, rs = safe_r, safe_r / 3.0
          local segs = 24
          local px, py
          for i = 0, segs do
            local ang = (i / segs) * 2 * math.pi
            local eu = math.cos(ang) * rl
            local ev = math.sin(ang) * rs
            local nx = s.cx + eu * ux + ev * vx
            local ny = s.cy + eu * uy + ev * vy
            if px then
              local er, eg = (s.total_score or 999) < 10 and 0 or 255,
                             (s.total_score or 999) < 10 and 200 or 165
            end
            px, py = nx, ny
          end
        end
        -- Score label. Pink for everyone; bucket members get a dark
        -- purple overdraw on top so the contenders stand out from the
        -- losers at a glance.
        if (s.total_score or 999) < 900 then
          local label = string.format("A%.0f+B%.0f+D%.0f+E%.0f=%.0f",
            s.score_a or 0, s.score_b or 0, s.score_d or 0,
            s.score_e or 0, s.total_score or 0)
          if s.in_bucket then
            local dij_label = string.format("dij=%.0f", s._dij or 0)
          end
        end
        -- D-detail: each clear-LOS candidate becomes clickable in the inspector
        -- with its full score breakdown, so you can see WHY one spot beat another
        -- (lower total_score wins; ties broken in the bucket). The winner is the
        -- chosen_deg spot. No-LOS spots are omitted (already shown as red boxes).
        if viz.detail_rect then
          local is_win = chosen_deg and s.deg == chosen_deg
          local did = string.format("%s_%d_%d", viz_id, s.mx or 0, s.my or 0)
          if is_win then viz.detail_text(did, "WINNER: lowest total_score among clear-LOS, non-rejected spots") end
          n_detail = n_detail + 1
        end
      end
    end
  end
  -- Marker so you can grep which ticks/bots recorded clickable spot details
  -- (the per-bot log + ===TICK=== context give the tick & bot). Only fires when
  -- the layer is on and at least one candidate was recorded.
  if n_detail > 0 then print2(string.format("DETAIL_REC viz=%s pill=(%d,%d) candidates=%d/%d", viz_id, pmx or -1, pmy or -1, n_detail, #spots)) end
end

function M.find_pill_at(world, mx, my)
  local entries = world.pill_at[my * 256 + mx]
  if not entries then return nil end
  return entries[1] and entries[1].pill or nil
end

-- =========================================================================
-- Pill attack position planner
-- =========================================================================

-- Check every intermediate tile between (x0,y0) and (x1,y1) is water.
-- When true a boat shell travels over them and strikes the first land (the pill).
local water_corridor_to = U.water_corridor_to

-- Simulate the tank shot from the chosen standoff to the target pill and
-- return a reason string if the path is blocked, or nil if it is clear.
-- Blocking conditions:
--   * any pill (friendly or enemy) other than the target on the path
--   * more than one wall tile on the outgoing path
-- Walls are allowed up to a count of 1 (one shield wall sitting on the
-- path edge is acceptable; two means something went badly wrong with the
-- setup).  Called every tick in the pre-fire substates so a newly-placed
-- strategic pill that landed in the corridor triggers an immediate replan.
local function standoff_shot_obstacle(goal, pill, world)
  if not (goal.standoff_fx and goal.standoff_fy) then return nil end
  local pmx, pmy = pill.mx, pill.my
  -- Aim: the point on the pill the planner proved we can actually hit from this
  -- spot (goal.aim_wx/aim_wy — centre or a corner, set by plan_position's
  -- clear-aim gate and by the shield scan), then the winning PPT corner, then
  -- the pill centre. Testing the CENTRE line on a take that is deliberately
  -- aiming at a corner is what made this check ban perfectly good spots.
  local target_wx, target_wy
  if goal.aim_wx and goal.aim_wy then
    target_wx, target_wy = goal.aim_wx, goal.aim_wy
  elseif goal._shield_scan and goal._shield_scan.best then
    local w   = goal._shield_scan.best
    local off = shield.AIM_OFFSETS_TILE_FIRE[w.best_aim_idx or 1]
                or shield.AIM_OFFSETS_TILE_FIRE[1]
    target_wx = (bit.lshift(pmx, 8)) + math.floor(off[1] * 256)
    target_wy = (bit.lshift(pmy, 8)) + math.floor(off[2] * 256)
  else
    target_wx = bit.bor((bit.lshift(pmx, 8)), 128)
    target_wy = bit.bor((bit.lshift(pmy, 8)), 128)
  end
  local spot_wx  = math.floor(goal.standoff_fx * 256 + 0.5)
  local spot_wy  = math.floor(goal.standoff_fy * 256 + 0.5)
  local origin_mx = bit.rshift(spot_wx, 8)
  local origin_my = bit.rshift(spot_wy, 8)
  local tiles = cpf.simulate_shot(spot_wx, spot_wy,
                                  target_wx, target_wy,
                                  cpf.SHOT_TANK, 0)
  if not tiles then return nil end
  local wall_n = 0
  for _, t in ipairs(tiles) do
    if t.mx == pmx and t.my == pmy then break end  -- reached target pill tile
    if t.mx ~= origin_mx or t.my ~= origin_my then
      -- Pill in path (other than target)?
      local plist = world.pill_at and world.pill_at[t.my * 256 + t.mx]
      if plist then
        for _, e in ipairs(plist) do
          if e.pill then
            return string.format("pill at (%d,%d) in shot path", t.mx, t.my)
          end
        end
      end
      -- A pill our man (or an ally's) is walking to build here: it will be a
      -- blocker in a few seconds, so treat it as one now (knob-gated fill,
      -- see spot_margin.pending_pill_at).
      if SM.pending_pill_at(world, t.mx, t.my) then
        return string.format("pending pill at (%d,%d) in shot path", t.mx, t.my)
      end
      -- Wall count gate.
      local tt = U.ttype(t.mx, t.my)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
        wall_n = wall_n + 1
        -- A wall-pass take (goal.through_walls, C.ATTACK_PILL_WALL_FALLBACK)
        -- chose this line knowing the walls are on it: it shoots through them.
        if wall_n > 1 and not goal.through_walls then
          return string.format("2+ walls in shot path (at (%d,%d))", t.mx, t.my)
        end
      end
    end
  end
  return nil
end

-- Shot-path obstacle check: simulate a shell from the tank's current
-- position toward the aim point. Count how many shots it would take to
-- clear all obstacles (forest, walls) before the shell reaches the
-- target pill tile. Returns (shots_needed, reason_str) where
-- shots_needed is the extra shots to clear the path (0 = clear path),
-- or (math.huge, reason) if an impassable obstacle (other pillbox) blocks.
local function shot_path_obstacle_count(info, goal, world, aim_wx, aim_wy, from_wx, from_wy)
  local pmx, pmy = goal.mx, goal.my
  -- Fire the tank's LIVE float gun angle (info.tank_angle, straight from the
  -- engine's MY_TANK->angle) — bit-exact with the shot the engine will actually
  -- fire. This is the fire-time twin of standoff_shot_obstacle: that one SCORES
  -- hypothetical standoff tiles (no live tank → it must infer the angle from
  -- spot->pill, brad-quantized), but here the tank is parked and aimed, so
  -- inferring/quantizing an angle to the aim-point diverged from the real shot
  -- and false-aborted takes that actually land. Use the known heading instead.
  --
  -- Optional aim override (aim_wx/aim_wy, world coords): for a caller that must
  -- test a line the gun is not pointing down YET. The blitz GO gate asks "if we
  -- charge from here, aiming at the pill CENTRE the way steering's charge does,
  -- is anything solid in the way?" while the gun is still settling inside
  -- SQUAD_BLITZ_AIM_TOL. Pass nothing and the behaviour is exactly as before.
  --
  -- Optional origin override (from_wx/from_wy, world units, only together with
  -- an aim override): fire the test shell from a point other than the tank.
  -- The blitz GO gate passes the soldier's planned standoff here
  -- (C.BLITZ_GO_GATE_FROM_STANDOFF): the soldier waits at its SETUP point,
  -- 2.25 tiles behind the standoff, and the charge fires from the standoff.
  local ox, oy = info.tankx, info.tanky
  if from_wx and from_wy and aim_wx and aim_wy then ox, oy = from_wx, from_wy end
  local tiles
  if aim_wx and aim_wy then
    tiles = cpf.simulate_shot(ox, oy, aim_wx, aim_wy,
                              cpf.SHOT_TANK, 0)
  else
    tiles = cpf.simulate_shot_angle(info.tankx, info.tanky,
                                    info.tank_angle or 0,
                                    cpf.SHOT_TANK, 0)
  end
  if not tiles then return 0, "no sim" end
  local origin_mx = bit.rshift(ox, 8)
  local origin_my = bit.rshift(oy, 8)
  local shots = 0
  local reached_pill = false
  local prev_mx, prev_my = nil, nil
  for _, t in ipairs(tiles) do
    -- Exact hit on the pill tile…
    if t.mx == pmx and t.my == pmy then
      reached_pill = true
      break
    end
    -- …OR a diagonal corner-skip: a dead-on diagonal shot passes through the
    -- CORNER of the pill tile, and the discretized walk records the two
    -- flanking tiles but not the pill tile itself (so the exact test above
    -- misses even though the shell visibly crosses the pill). When the step
    -- from the previous tile to this one is diagonal, the shell also crossed
    -- the two corner tiles (prev_mx,t.my) and (t.mx,prev_my) — accept the
    -- pill if it's one of them.
    if prev_mx and t.mx ~= prev_mx and t.my ~= prev_my then
      if (pmx == prev_mx and pmy == t.my)
         or (pmx == t.mx and pmy == prev_my) then
        reached_pill = true
        break
      end
    end
    prev_mx, prev_my = t.mx, t.my
    if t.mx ~= origin_mx or t.my ~= origin_my then
      local tt = U.ttype(t.mx, t.my)
      if tt == C.T_BUILDING then
        shots = shots + 5                    -- 1 hit → halfbuild + 4 life
      elseif tt == C.T_HALFBUILD then
        shots = shots + 4                    -- worst case: life=4
      elseif tt == C.T_FOREST then
        shots = shots + 1
      else
        local plist = world.pill_at and world.pill_at[t.my * 256 + t.mx]
        if plist then
          for _, e in ipairs(plist) do
            -- Validate against the LIVE pill table, not the cached index entry:
            -- world.pills[id] is the source of truth, and a pill_at entry can go
            -- stale (point to an old table) when a pill is captured/carried/replaced
            -- — that's how a pill an ally has DRIVEN OVER (now in_tank, hp 0) was
            -- still blocking shots as a phantom hp-15 obstacle. Only a genuinely
            -- DEPLOYED, alive pill ACTUALLY on this tile blocks the shell.
            local p = e.id and world.pills[e.id]
            if p and not p.in_tank and (p.health or 0) > 0
               and p.mx == t.mx and p.my == t.my then
              return math.huge, string.format("pill at (%d,%d) hp=%d", t.mx, t.my, p.health)
            end
          end
        end
      end
    end
  end
  if not reached_pill then
    local lastt = tiles[#tiles]
  end
  return shots, nil, reached_pill
end

-- Ban the approach angle we are CURRENTLY using on this pill (the 5° bucket of
-- goal._chosen_deg plus its two neighbours) so the next plan_position sweep has
-- to pick a different line, and drop the caches that would otherwise hand the
-- same angle straight back. Returns the banned centre bucket, or nil if we have
-- no chosen angle to ban.
--
-- Factored out of the per-tick standoff sanity check (SANITY_BAN) because the
-- blitz GO gate needs exactly the same thing: both discover mid-take that the
-- line we picked is blocked by something that will NOT move (one of our own
-- pills), so replanning on the same angle would just loop.
local function ban_current_pill_angle(state, goal, pmx, pmy, now, ttl)
  if not (state and goal and goal._chosen_deg) then return nil end
  ttl = ttl or 9000                              -- ~3 min @ 50 Hz
  local pkey = pmy * 256 + pmx
  state.banned_pill_angles = state.banned_pill_angles or {}
  local pill_bans = state.banned_pill_angles[pkey]
  if not pill_bans then
    pill_bans = {}
    state.banned_pill_angles[pkey] = pill_bans
  end
  local bucket = math.floor((goal._chosen_deg % 360) / 5) * 5
  for _, off in ipairs({ -5, 0, 5 }) do
    pill_bans[(bucket + off) % 360] = now + ttl
  end
  -- The ban is honoured by the angle SWEEP, but plan_position reuses this
  -- pill's cached sweep (state._pill_eval_cache, TTL 250) and the pool's spot
  -- cache -- both computed before the ban -- so the same angle came straight
  -- back (g9fix bot3 t=29069..29177: 90..100 deg re-picked three times inside
  -- one ban). Drop both caches so the next plan_position sweeps afresh with the
  -- ban in force.
  local pid = goal.target_id
  if pid ~= nil then
    if state._pill_eval_cache    then state._pill_eval_cache[pid]    = nil end
    if state._pill_eval_progress then state._pill_eval_progress[pid] = nil end
  end
  if state._pill_diff_cache then
    state._pill_diff_cache[pmx .. ":" .. pmy .. ":" .. (state.phase or "")] = nil
  end
  -- The SHIELD scan is cached too, keyed partly on the standoff we are about to
  -- move off. If the sweep happens to hand back the same standoff tile the key
  -- matches and a scan computed BEFORE this ban -- whose nudged candidate ring
  -- still contains the banned bucket -- would be reused wholesale. Drop it so
  -- the next plan_position rescans with the ban in force.
  state._shield_scan_cache = nil
  return bucket
end

-- Count forest tiles on the Bresenham line from (x0,y0) to (x1,y1), excluding
-- endpoints.  Each one would be destroyed by a shell fired along this path —
-- a resource cost since the brain farms trees for road building and pill repair.
-- Counts forest tiles on the line from (x0,y0) to (x1,y1).
-- If `do_viz` is true, draws a white outline on each forest tile considered.
local function forest_tiles_on_path(x0, y0, x1, y1, do_viz)
  local count = 0
  -- Use precise sub-pixel walk: tile centers at +0.5
  U.line_walk(x0 + 0.5, y0 + 0.5, x1 + 0.5, y1 + 0.5, function(cx, cy)
    if U.ttype(cx, cy) == C.T_FOREST then
      count = count + 1
      if do_viz then
      end
    end
  end)
  return count
end

-- Score a single candidate standoff tile (cx, cy) for attacking `pill`.
-- Returns a score (lower is better) or math.huge if the position is unusable.
-- When orbit_radius is non-nil, also penalizes slow/hazardous terrain on the
-- orbit arc (for circle-strafe attacks like bpc).
-- Clear SHOT from a standoff tile to the pill, using the C shell SIMULATION
-- (cpf.simulate_shot — same physics as the engine, the authoritative "what tiles
-- does this shell actually cover" answer) rather than a naive grid line. The shot
-- is clear iff the simulated trajectory REACHES the pill tile; if it stops short
-- on a wall, a hostile base, or another pillbox, the shell hits that instead — a
-- take from such a standoff would just shell the obstacle (e.g. an enemy base
-- between us and the pill). Endpoints excluded. Shared by score_standoff AND
-- pick_standoff's fallback so EVERY chosen standoff is verified to have a real
-- shot at the pill.
local function standoff_clear_shot(world, cx, cy, pill)
  local ox = bit.bor((bit.lshift(cx, 8)), 128)
  local oy = bit.bor((bit.lshift(cy, 8)), 128)
  local tx = bit.bor((bit.lshift(pill.mx, 8)), 128)
  local ty = bit.bor((bit.lshift(pill.my, 8)), 128)
  local tiles = cpf.simulate_shot(ox, oy, tx, ty, cpf.SHOT_TANK, 0)
  if not tiles then return true end  -- sim unavailable → don't over-reject
  for _, t in ipairs(tiles) do
    if t.mx == pill.mx and t.my == pill.my then
      return true  -- shell trajectory reaches the target pill → clear shot
    end
    if not (t.mx == cx and t.my == cy) then  -- skip our own (origin) tile
      -- A base of ANY owner (hostile/friendly/neutral) is solid and stops the
      -- shell before it reaches the pill.
      local bentry = world.base_at and world.base_at[t.my * 256 + t.mx]
      if bentry and bentry.base then return false end
      -- A wall stops the shell.
      local tt = U.ttype(t.mx, t.my)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then return false end
      -- A LIVE pillbox (any owner) in the path stops the shell; a dead one is
      -- passable rubble and doesn't.
      local plist = world.pill_at and world.pill_at[t.my * 256 + t.mx]
      if plist then
        -- Live-table check (see shot_path_obstacle_count): a stale index entry
        -- (captured/carried/replaced pill) must not phantom-block the shell.
        for _, e in ipairs(plist) do
          local p = e.id and world.pills[e.id]
          if p and not p.in_tank and (p.health or 0) > 0 and p.mx == t.mx and p.my == t.my then return false end
        end
      end
    end
  end
  return false  -- trajectory ended without ever covering the pill tile
end

local function score_standoff(world, cx, cy, pill, info, orbit_radius)
  if not U.in_map(cx, cy) then return math.huge end

  -- Must have a clear shot to the pill (no wall or hostile base in the way).
  if not standoff_clear_shot(world, cx, cy, pill) then return math.huge end

  -- Drive-in path: the tank holds at the SETUP point (behind the standoff, out
  -- of pill range) and charges to the standoff on GO. That short radial segment
  -- must be cleanly drivable — TREES are fine (cover / the tank pushes through),
  -- but a wall, pillbox, or enemy base in the way blocks the charge, so reject
  -- the spot. Same setup radius as blitz_commit_negotiated (ideal-floored).
  do
    local pdx, pdy = cx - pill.mx, cy - pill.my
    local plen = math.max(0.001, math.sqrt(pdx * pdx + pdy * pdy))
    local ux, uy = pdx / plen, pdy / plen
    local setup_dist = (C.ATTACK_PILL_STANDOFF or 7.4) + (C.ATTACK_APPROACH_OFFSET or 2.25)
    local sfx = (pill.mx + 0.5) + ux * setup_dist
    local sfy = (pill.my + 0.5) + uy * setup_dist
    local drive_blocked = false
    U.line_walk(sfx, sfy, cx + 0.5, cy + 0.5, function(wx, wy)
      if wx == cx and wy == cy then return end  -- standoff tile itself is fine
      local wt = U.ttype(wx, wy)
      if wt == C.T_BUILDING or wt == C.T_HALFBUILD or wt == C.T_PILLBOX then
        drive_blocked = true; return true
      end
      local be = world.base_at and world.base_at[wy * 256 + wx]
      if be and be.base and be.base.owner == "hostile" then
        drive_blocked = true; return true
      end
    end)
    if drive_blocked then return math.huge end
  end

  -- Forest tiles on the shot path are destroyed by each shell fired.
  -- Prefer angles that don't waste trees the brain may want to farm later.
  local trees_destroyed = forest_tiles_on_path(cx, cy, pill.mx, pill.my)

  local tt        = U.ttype(cx, cy)
  local land_cost = C.TERRAIN_COST_LAND[tt] or 9999

  -- Walls/impassable non-water: completely unusable
  if land_cost >= 9999 and not U.is_water(tt) then
    return math.huge
  end

  -- Water standoff: only valid when every tile between here and the pill is also
  -- water, meaning the shell clears the water and strikes the pill at the edge.
  -- Otherwise the shell hits the first land square it encounters, not the pill —
  -- making this position completely useless for the attack.
  if U.is_water(tt) then
    if not water_corridor_to(cx, cy, pill.mx, pill.my) then
      return math.huge
    end
  end

  -- Water at standoff (edge case — pill is right at water boundary):
  -- penalise heavily because the boat can be destroyed by return fire and
  -- the tank ends up stranded in open water.
  local water_pen = 0
  if tt == C.T_RIVER   then water_pen = 150 end
  if tt == C.T_DEEPSEA then water_pen = 300 end

  -- Pushback: terrain in the direction the tank gets shoved when hit.
  -- Any terrain that slows or blocks movement is dangerous: buildings trap you
  -- while the pill keeps firing, slow terrain (swamp/rubble/crater) leaves you
  -- unable to manoeuvre away, water kills you outright.
  -- Penalty per tile type — higher = harder to escape while taking damage.
  local PUSH_PEN = {
    [C.T_DEEPSEA]   = 400,  -- instant death without boat
    [C.T_RIVER]     = 200,  -- very slow without boat, easily killed
    [C.T_BUILDING]  = 350,  -- impassable: completely stuck while being shot
    [C.T_HALFBUILD] = 350,  -- impassable: same
    [C.T_SWAMP]     = 100,  -- speed 4 — sitting duck
    [C.T_RUBBLE]    = 100,  -- speed 4 — sitting duck
    [C.T_CRATER]    = 100,  -- speed 4 — sitting duck
    [C.T_FOREST]    = 20,   -- slightly slow but provides cover
    -- ROAD/GRASS/REFBASE/BOAT = 0 (fast escape)
  }
  local pdx_raw = cx - pill.mx
  local pdy_raw = cy - pill.my
  local plen    = math.max(1, math.sqrt(pdx_raw * pdx_raw + pdy_raw * pdy_raw))
  local push_dx = pdx_raw / plen
  local push_dy = pdy_raw / plen
  local pushback_pen = 0
  for step = 1, 5 do
    local bx = U.mclamp(math.floor(cx + push_dx * step + 0.5))
    local by = U.mclamp(math.floor(cy + push_dy * step + 0.5))
    local bt = U.ttype(bx, by)
    local pen = PUSH_PEN[bt] or 0
    if pen > 0 then
      -- Closer steps hurt more: weight 1.0 / 0.7 / 0.5 / 0.35 / 0.2
      local weight = math.max(0.2, 1.0 - (step - 1) * 0.2)
      pushback_pen = pushback_pen + pen * weight
    end
  end
  -- Proximity scan: hazardous terrain within radius 4 adds penalty regardless
  -- of exact pushback direction.  Multiple hits from the pill can push the tank
  -- sideways, the boat gets dropped on the first land tile (leaving it behind),
  -- and even a single recoil can reach water 3 tiles away.
  -- Weight falls off with Chebyshev distance so adjacent tiles hurt most.
  for dy = -4, 4 do
    for dx = -4, 4 do
      local d = math.max(math.abs(dx), math.abs(dy))  -- Chebyshev distance
      if d >= 1 then
        local nx, ny = U.mclamp(cx + dx), U.mclamp(cy + dy)
        local npen = PUSH_PEN[U.ttype(nx, ny)] or 0
        if npen > 0 then
          -- weight: 0.5 at d=1, 0.35 at d=2, 0.2 at d=3, 0.1 at d=4
          local w = math.max(0.1, 0.65 - d * 0.15)
          pushback_pen = pushback_pen + npen * w
        end
      end
    end
  end

  -- Crossfire from all OTHER hostile/neutral pills in range of this tile
  -- (the target pill itself is excluded — we expect to be shot at by it).
  -- Weighted heavily (×8): taking fire from a second pill while engaged
  -- with the target is devastating and worth a significant detour to avoid.
  local crossfire = 0
  for _, pm in pairs(world.pills) do
    if (pm.mx ~= pill.mx or pm.my ~= pill.my)
       and (pm.owner == "hostile" or pm.owner == "neutral")
       and pm.health > 0 then
      local d = U.mdist(cx, cy, pm.mx, pm.my)
      if d <= C.PILL_RANGE_MAP then
        local proximity = 1.0 - d / (C.PILL_RANGE_MAP + 1)
        crossfire = crossfire + (C.PILL_DANGER_BASE
                    + C.PILL_DANGER_ANGER * (pm.anger or 0)) * proximity * 8
      end
    end
  end

  -- Approach cost from the tank's current position.
  -- Use KIND_NORMAL Dijkstra (full danger) — we're navigating to a firing
  -- position, not through the pill, so full danger applies.
  -- Falls back to straight-line estimate if Dijkstra hasn't reached this tile.
  local tmx    = bit.rshift(info.tankx, 8)
  local tmy    = bit.rshift(info.tanky, 8)
  local ammo   = (info.shells or 0) + (info.mines or 0)
  local approach = cpf.dijkstra_lookup_by_kind(cpf.KIND_NORMAL, cx, cy, info.inboat and 1 or 0)
  if approach >= 1e29 then
    approach = cpf.estimate_cost(tmx, tmy, cx, cy, info.inboat and 1 or 0)
  end

  -- Approach exposure: penalise routes that cross slow terrain within the
  -- target pill's firing range.  On swamp/rubble/crater the tank moves at
  -- speed 4, too slow to dodge pill shots — each such tile means eating
  -- unavoidable damage on the way in.  Fast terrain (grass/road) is fine:
  -- the tank can outrun pill aim at range.
  local SLOW_IN_RANGE = {
    [C.T_SWAMP]  = true,
    [C.T_RUBBLE] = true,
    [C.T_CRATER] = true,
  }
  local approach_exposure = 0
  local adist = U.mdist(tmx, tmy, cx, cy)
  if adist > 0 then
    local asteps = math.min(adist, 30)
    for i = 1, asteps do
      local t  = i / asteps
      local ax = U.mclamp(math.floor(tmx + (cx - tmx) * t + 0.5))
      local ay = U.mclamp(math.floor(tmy + (cy - tmy) * t + 0.5))
      if SLOW_IN_RANGE[U.ttype(ax, ay)]
         and U.mdist(ax, ay, pill.mx, pill.my) <= C.PILL_RANGE_MAP then
        approach_exposure = approach_exposure + C.APPROACH_SLOW_IN_RANGE_PEN
      end
    end
  end

  -- Escape cost: always in land mode — the tank will be on foot when it needs
  -- to escape (boat is dropped on first land tile when approaching standoff).
  local escape_cost = 0
  local best_escape = math.huge
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" or b.owner == "neutral" then
      local ec = cpf.estimate_cost(cx, cy, b.mx, b.my, 0)
      if ec < best_escape then best_escape = ec end
    end
  end
  if best_escape < math.huge then escape_cost = best_escape * 0.2 end

  -- Each forest tile on the shot path costs ~80: meaningful enough to prefer a
  -- clear angle, but less than the water/pushback penalties so it doesn't block
  -- the only viable position on an enclosed map.
  local tree_pen = trees_destroyed * 80

  -- Orbit path penalty: for circle-strafe attacks (bpc), check the terrain the
  -- tank will drive through while orbiting.  Sample 8 points on the orbit arc
  -- (±90° from the candidate's angle around the pill) and penalize slow or
  -- hazardous tiles.  This ensures the planner picks a starting position where
  -- the tank can orbit smoothly on fast terrain.
  local orbit_pen = 0
  if orbit_radius then
    local base_angle = math.atan(cx - pill.mx, -(cy - pill.my))
    local ORBIT_PEN = {
      [C.T_DEEPSEA]   = 500,
      [C.T_RIVER]     = 250,
      [C.T_BUILDING]  = 400,
      [C.T_HALFBUILD] = 400,
      [C.T_SWAMP]     = 120,
      [C.T_RUBBLE]    = 120,
      [C.T_CRATER]    = 120,
      [C.T_FOREST]    = 15,
    }
    -- Sample a half-circle in each direction (the tank will orbit through these)
    for step = -4, 4 do
      if step ~= 0 then
        local a = base_angle + step * (math.pi / 4)  -- 45° increments, ±180°
        local ox = U.mclamp(math.floor(pill.mx + math.sin(a) * orbit_radius + 0.5))
        local oy = U.mclamp(math.floor(pill.my - math.cos(a) * orbit_radius + 0.5))
        local ott = U.ttype(ox, oy)
        local open = ORBIT_PEN[ott] or 0
        if open > 0 then
          -- Closer arc steps (±45°, ±90°) matter more than far ones (±135°, ±180°)
          local weight = math.max(0.3, 1.0 - math.abs(step) * 0.15)
          orbit_pen = orbit_pen + open * weight
        end
      end
    end
  end

  -- Threat map penalty: use the unified threat grid (pills + enemy tanks) to
  -- penalise standoff positions in high-threat areas.  The crossfire term above
  -- only considers pills visible in world.pills; the threat grid also captures
  -- tank presence zones.  Scale by 2× so it meaningfully influences the score
  -- without dominating terrain/pushback penalties.
  local threat_pen = threat.at(cx, cy) * 2

  -- Influence bias: prefer standing on the friendly side of the pill so a
  -- retreat after the exchange lands in our territory, not deep in the
  -- enemy's. Friendly influence is positive, hostile is negative — negating
  -- converts to a penalty (hostile tile bad, friendly tile bonus).
  local influence_pen = -(cpf.influence_at(cx, cy) or 0)
                        * C.ATTACK_STANDOFF_INFLUENCE_WEIGHT

  -- Friendly pill interaction: a friendly pill BETWEEN us and the target
  -- (closer to the target) absorbs enemy fire — bonus. A friendly pill
  -- BEHIND us (further from the target, on the shot path outward) blocks
  -- our shells — penalty like a wall.
  local fpill_barrier_bonus = 0
  local fpill_behind_pen = 0
  for _, fp in pairs(world.pills) do
    if fp.owner == "friendly" and fp.health > 0 then
      local d_fp_target = U.mdist(fp.mx, fp.my, pill.mx, pill.my)
      local d_fp_us = U.mdist(fp.mx, fp.my, cx, cy)
      local d_total = U.mdist(cx, cy, pill.mx, pill.my)
      if d_fp_target < (d_total - 1) and d_fp_us < d_total and d_fp_target >= 1 then
        -- Between us and the target, at least 1 tile inside our standoff
        -- radius (not flush against us) — real shield position.
        fpill_barrier_bonus = fpill_barrier_bonus + C.FPILL_BARRIER_BONUS
      elseif d_fp_target > d_total and d_fp_us <= 3 then
        -- Behind us (further from target), close enough to block shots
        fpill_behind_pen = fpill_behind_pen + 200
      end
    end
  end

  return approach + water_pen + pushback_pen + crossfire + escape_cost + tree_pen
       + approach_exposure + orbit_pen + threat_pen + influence_pen
       + fpill_behind_pen - fpill_barrier_bonus
end

-- Enumerate candidate standoff positions around `pill` and pick the best scored one.
-- Samples multiple radii (max range down to max-2) so that if the ring at exactly
-- shell range lands on walls, nearby passable tiles are still considered.
-- Falls back to the closest candidate if none have finite scores (e.g. pill in open water).
function M.pick_standoff(world, info, pill, state, standoff_override, orbit_radius)
  local R_MAX = standoff_override or C.ATTACK_PILL_STANDOFF
  local R_MIN = R_MAX   -- stay on the circle edge, don't go closer
  local N     = C.ATTACK_PLAN_DIRS
  local tmx   = bit.rshift(info.tankx, 8)
  local tmy   = bit.rshift(info.tanky, 8)

  local best_score = math.huge
  local best_mx, best_my = nil, nil
  local fallback_dist = math.huge
  local fallback_mx, fallback_my = nil, nil
  -- Clear-shot fallback: the closest standable candidate that the shell sim says
  -- actually reaches the pill. Used when NO candidate passes the full scorer, so
  -- we never fall back onto a base/wall-blocked tile and shell the obstacle.
  local fb_clear_dist = math.huge
  local fb_clear_mx, fb_clear_my = nil, nil
  -- Hoist viz toggle for the candidate-overlay emissions below.
  local v_take = BRAIN_DEBUG_MODE and viz.is_on("pill_take_target")

  local seen = {}
  for R = R_MAX, R_MIN, -1 do
    -- Slight penalty for shorter radii: each tile closer = more pill damage taken.
    -- 20 per tile makes radius 6 cost +20 and radius 5 cost +40 vs radius 7.
    local range_pen = (R_MAX - R) * 20
    for i = 0, N - 1 do
      local angle = i * (2 * math.pi / N)
      local cx = U.mclamp(math.floor(pill.mx + math.sin(angle) * R + 0.5))
      local cy = U.mclamp(math.floor(pill.my - math.cos(angle) * R + 0.5))
      local ck = U.mkey(cx, cy)
      if not seen[ck] and not (state and state._blitz_reject and state._blitz_reject[ck]) then
        seen[ck] = true
        -- Always track closest as fallback
        local d = U.mdist(tmx, tmy, cx, cy)
        if d < fallback_dist then
          fallback_dist = d; fallback_mx = cx; fallback_my = cy
        end
        -- Closest STANDABLE candidate with a verified clear shot (sim) — the
        -- preferred fallback when nothing passes the full scorer.
        if d < fb_clear_dist then
          local _tt = U.ttype(cx, cy)
          if not U.is_water(_tt) and (C.TERRAIN_COST_LAND[_tt] or 9999) < 9999
             and standoff_clear_shot(world, cx, cy, pill) then
            fb_clear_dist = d; fb_clear_mx = cx; fb_clear_my = cy
          end
        end
        local score = score_standoff(world, cx, cy, pill, info, orbit_radius)
        if score < math.huge then score = score + range_pen end
        local tt   = U.ttype(cx, cy)
        local trees_on_path = forest_tiles_on_path(cx, cy, pill.mx, pill.my)
        if C.LOG_STANDOFF_CANDIDATES then
          print(string.format(
            TAG .. "   cand (%d,%d) R=%d tt=%d score=%s trees_hit=%d",
            cx, cy, R, tt,
            score >= math.huge and "INF" or string.format("%.1f", score),
            trees_on_path))
        end
        if score < best_score then
          best_score = score; best_mx = cx; best_my = cy
        end

        -- Overlay: show candidate positions with color-coded scores
        if v_take then
          if score >= math.huge then
            -- Unreachable: dim red
          elseif score == best_score then
            -- Currently best: bright green (will be overwritten by final pick)
          else
            -- Scored: yellow-to-red gradient based on relative cost
            local rel = math.min(1.0, score / math.max(1, best_score * 3))
            local r = math.floor(255 * rel)
            local g = math.floor(255 * (1 - rel))
          end
        end
      end
    end
  end

  -- Prefer a fully-scored pick; then the closest verified clear-shot tile; only
  -- as a last resort the blind closest (may be blocked — but at that point no
  -- orbit position has any shot at the pill, so the take will bail elsewhere).
  local smx = best_mx or fb_clear_mx or fallback_mx
  local smy = best_my or fb_clear_my or fallback_my
  if smx then
    if v_take then
      -- Overlay: mark chosen standoff with bright green circle
      -- Line from pill to chosen standoff (green)
    end
    print(string.format(TAG .. " ATTACK PLAN: pill@(%d,%d) standoff=(%d,%d) score=%s",
          pill.mx, pill.my, smx, smy,
          best_score >= math.huge and "INF(fallback)" or string.format("%.1f", best_score)))
  end
  return smx, smy
end

-- =========================================================================
-- Wall-shield attack planner
-- =========================================================================
-- Pick a wall tile 1 tile from the pill, between the pill and the tank's
-- approach direction.  Then pick a standoff tile ~5 tiles from the pill
-- behind the wall on the same line.  Returns wall_mx, wall_my, stand_mx,
-- stand_my or nil if no valid wall-shield position exists.

local function pick_wall_shield(world, info, pill, state)
  if not C.WALL_SHIELD_ENABLED then return nil end
  if (info.trees or 0) < C.WALL_SHIELD_MIN_TREES then return nil end
  -- Don't attempt in a boat
  if info.inboat then return nil end

  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local pmx, pmy = pill.mx, pill.my

  -- Direction from pill to tank (approach angle)
  local dx = tmx - pmx
  local dy = tmy - pmy
  local len = math.sqrt(dx * dx + dy * dy)
  if len < 2 then return nil end  -- too close already

  local ux, uy = dx / len, dy / len

  -- Try the primary angle and rotations of ±30°, ±60° to find a buildable wall tile
  local candidates = {}
  local angles = { 0, 0.52, -0.52, 1.05, -1.05 }  -- radians: 0, ±30°, ±60°
  for _, ang in ipairs(angles) do
    local cos_a = math.cos(ang)
    local sin_a = math.sin(ang)
    local rx = ux * cos_a - uy * sin_a
    local ry = ux * sin_a + uy * cos_a

    -- Wall tile: 1 tile from pill in this direction
    local wmx = math.floor(pmx + rx * C.WALL_SHIELD_WALL_DIST + 0.5)
    local wmy = math.floor(pmy + ry * C.WALL_SHIELD_WALL_DIST + 0.5)
    wmx = U.mclamp(wmx); wmy = U.mclamp(wmy)

    -- Wall tile must be buildable (grass, rubble, swamp, crater, road, or already a wall)
    local wtt = U.ttype(wmx, wmy)
    local buildable = (wtt == C.T_GRASS or wtt == C.T_RUBBLE or wtt == C.T_SWAMP
                       or wtt == C.T_CRATER or wtt == C.T_ROAD or wtt == C.T_BUILDING
                       or wtt == C.T_HALFBUILD or wtt == C.T_FOREST)
    -- Can't build on water, deep sea, bases, pills
    if not buildable then goto next_angle end
    -- Wall tile must not be the pill tile itself
    if wmx == pmx and wmy == pmy then goto next_angle end

    -- Standoff tile: offset ~25° from the wall direction so shells clear the wall.
    -- The wall blocks pill return fire (adjacent to pill on our side) but the tank
    -- shoots at an angle that misses the wall tile.  Try +25° and -25° offsets at
    -- distances 7 then 6 (max shell range down to 1 less) to find a position with
    -- clear LOS to the pill that doesn't pass through the wall.
    local smx, smy = nil, nil
    local off_rad = math.rad(C.WALL_SHIELD_STANDOFF_ANGLE_OFFSET)
    -- Try offset angles first, then 0° as fallback (directly behind wall)
    local offsets = { off_rad, -off_rad, off_rad * 2, -off_rad * 2, 0 }
    for _, sdist in ipairs({ C.WALL_SHIELD_STANDOFF, C.WALL_SHIELD_STANDOFF - 1, C.WALL_SHIELD_STANDOFF + 1 }) do
      for _, off in ipairs(offsets) do
        local cos_o = math.cos(off)
        local sin_o = math.sin(off)
        local ox = rx * cos_o - ry * sin_o
        local oy = rx * sin_o + ry * cos_o
        local cx = U.mclamp(math.floor(pmx + ox * sdist + 0.5))
        local cy = U.mclamp(math.floor(pmy + oy * sdist + 0.5))
        -- Must be passable land
        local ctt = U.ttype(cx, cy)
        local cland = C.TERRAIN_COST_LAND[ctt] or 9999
        if cland < 9999 and not U.is_water(ctt) then
          -- Must have clear LOS to pill: Bresenham must NOT pass through wall tile
          local hits_wall = U.bresenham(cx, cy, pmx, pmy, function(lx, ly)
            if lx == wmx and ly == wmy then return true end
          end)
          -- Check for OTHER walls on path (exclude the wall we're building)
          local other_wall_hp = 0
          U.bresenham(cx, cy, pmx, pmy, function(lx, ly)
            if not (lx == wmx and ly == wmy) then
              local ltt = U.ttype(lx, ly)
              if ltt == C.T_BUILDING then other_wall_hp = other_wall_hp + C.WALL_HP_FULL end
              if ltt == C.T_HALFBUILD then other_wall_hp = other_wall_hp + C.WALL_HP_HALF end
            end
          end)
          if not hits_wall and other_wall_hp == 0 then
            -- Must be within shell range
            local cd = math.sqrt((cx - pmx) * (cx - pmx) + (cy - pmy) * (cy - pmy))
            if cd <= C.ATTACK_PILL_RANGE then
              smx = cx; smy = cy
              break
            end
          end
        end
      end
      if smx then break end
    end
    if not smx then goto next_angle end

    -- Prebuild position: outside pill range on the wall direction line
    local pbmx = math.floor(pmx + rx * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5)
    local pbmy = math.floor(pmy + ry * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5)
    pbmx = U.mclamp(pbmx); pbmy = U.mclamp(pbmy)
    local pbtt = U.ttype(pbmx, pbmy)
    local pbland = C.TERRAIN_COST_LAND[pbtt] or 9999
    if pbland >= 9999 or U.is_water(pbtt) then goto next_angle end

    -- Check crossfire from other hostile pills at standoff
    local crossfire = 0
    for _, pm in pairs(world.pills) do
      if (pm.mx ~= pmx or pm.my ~= pmy)
         and (pm.owner == "hostile" or pm.owner == "neutral")
         and pm.health > 0 then
        local d = U.mdist(smx, smy, pm.mx, pm.my)
        if d <= C.PILL_RANGE_MAP then
          crossfire = crossfire + 1
        end
      end
    end

    -- LGM reachability: can the LGM walk from prebuild to wall and back?
    local lgm_to_wall = cpf.lgm_travel_ticks_map(
      pbmx, pbmy, wmx, wmy, wmx, wmy,
      C.WALL_SHIELD_LGM_MAX_TICKS, C.WALL_SHIELD_LGM_STUCK_TICKS)
    if lgm_to_wall == -1 then goto next_angle end

    local lgm_return = cpf.lgm_travel_ticks_map(
      wmx, wmy, pbmx, pbmy, wmx, wmy,
      C.WALL_SHIELD_LGM_MAX_TICKS, C.WALL_SHIELD_LGM_STUCK_TICKS)
    if lgm_return == -1 then goto next_angle end

    local lgm_round_trip = lgm_to_wall + C.LGM_BUILD_TIME + lgm_return

    -- Approach cost
    local ammo = (info.shells or 0) + (info.mines or 0)
    local approach = cpf.estimate_cost(tmx, tmy, smx, smy, 0)

    -- Threat map: penalise standoff positions in high-threat areas (enemy
    -- tanks nearby, additional pill fire not captured by crossfire count).
    local ws_threat = threat.at(smx, smy) * 2

    -- Score: lower is better
    local score = approach + crossfire * 200 + math.abs(ang) * 50
                + lgm_round_trip * C.WALL_SHIELD_LGM_TRIP_WEIGHT
                + ws_threat
    candidates[#candidates + 1] = {
      wmx = wmx, wmy = wmy, smx = smx, smy = smy,
      pbmx = pbmx, pbmy = pbmy,
      score = score, ang = ang, crossfire = crossfire, lgm_trip = lgm_round_trip,
    }

    ::next_angle::
  end

  if #candidates == 0 then return nil end

  -- Pick best
  table.sort(candidates, function(a, b) return a.score < b.score end)
  local best = candidates[1]
  print(string.format(
    TAG .. " WALL-SHIELD PLAN: pill@(%d,%d) wall=(%d,%d) standoff=(%d,%d) prebuild=(%d,%d) score=%.0f crossfire=%d lgm_trip=%d",
    pmx, pmy, best.wmx, best.wmy, best.smx, best.smy,
    best.pbmx, best.pbmy, best.score, best.crossfire, best.lgm_trip))
  return best.wmx, best.wmy, best.smx, best.smy, best.pbmx, best.pbmy
end

-- Return cached or freshly computed standoff position for the given pill.
-- Invalidated when the pill target changes or every PILL_ATTACK_REPLAN_TICKS.
-- Also re-plans if the cached tile has become impassable (e.g. built over).
function M.get_standoff(world, info, pill_key, pill, state)
  local plan = state.pill_attack_plan
  local now  = state.tick or 0
  if plan and plan.pill_key == pill_key and now < plan.replan_at then
    -- Validate: cached tile must be passable dry land (not water, not a wall).
    -- TERRAIN_COST_LAND[T_RIVER] == -1 (dynamic sentinel) so we must test
    -- for water explicitly rather than relying on the cost threshold.
    local tt = U.ttype(plan.standoff_mx, plan.standoff_my)
    if not U.is_water(tt) then
      local lc = C.TERRAIN_COST_LAND[tt] or 9999
      if lc < 9999 then
        return plan.standoff_mx, plan.standoff_my
      end
    end
  end

  local smx, smy = M.pick_standoff(world, info, pill, state)
  state.pill_attack_plan = {
    pill_key    = pill_key,
    standoff_mx = smx,
    standoff_my = smy,
    replan_at   = now + C.PILL_ATTACK_REPLAN_TICKS,
  }
  return smx, smy
end

-- =========================================================================
-- Unified attack pill substate machine
-- =========================================================================
-- Substates: position → aim → engage → curve_away → (back to engage)
--            pill dies at any point → rush
--            armour critical → disengage
--
--
-- =========================================================================
-- M.evaluate_pill_difficulty(pill, world, detailed)
-- Scan the standoff circle around a pill and score how hard it is to attack.
-- Returns: best_score (lower=easier), spots (list of scored positions)
-- No pathfinding calls — only grid lookups, safe to call on all pills.
--
-- detailed=false (default): returns only best_score and minimal spots
--   (no maneuver_tiles, no cx/cy floats — lightweight for goal selection)
-- detailed=true: full data including maneuver_tiles and precise positions
--   (for visualization when actively attacking this pill)
-- =========================================================================

-- ── Precomputed ellipse stamps ──
--
-- For each scan angle, compute once: the list of integer (dx, dy) tile
-- offsets RELATIVE TO THE SPOT TILE that fall inside the maneuver
-- ellipse oriented at that angle. The runtime code iterates the stamp
-- directly instead of nested loops + tile_in_ellipse calls.
--
-- The ellipse geometry depends only on the angle (orientation) and
-- the standard radii (r_long = ATTACK_SAFE_RADIUS,
-- r_short = ATTACK_SAFE_RADIUS / 3) — same axes the inspector viz
-- uses to draw the outline.
-- The float center sub-tile offset varies per angle, so each stamp is
-- generated for that exact angle's spot center.
--
-- ELLIPSE_STAMPS_5DEG  — 72 stamps at 5° steps, used by the full
--                        plan_position scan (detailed=true case).
-- ELLIPSE_STAMPS_45DEG —  8 stamps at 45° steps, used by the quick
--                        per-candidate eval in goals.lua step_eval_queue.
local ELLIPSE_STAMPS_5DEG  = {}
local ELLIPSE_STAMPS_45DEG = {}
do
  local R       = C.ATTACK_PILL_STANDOFF
  -- r_long was hardcoded to 4 for years while the inspector viz drew
  -- the outline using ATTACK_SAFE_RADIUS; shrinking SAFE_RADIUS made
  -- the visible ellipse smaller but left the stamps the original
  -- size. Source both axes from the same constant so they track.
  local r_long  = C.ATTACK_SAFE_RADIUS
  local r_short = C.ATTACK_SAFE_RADIUS / 3.0
  local iter_r  = math.ceil(r_long) + 1

  local function build_stamp(deg)
    local rad = math.rad(deg)
    -- Spot center if pill is at (0, 0). pmx + 0.5 = 0.5, etc.
    local cx = 0.5 + math.sin(rad) * R
    local cy = 0.5 - math.cos(rad) * R
    local mx = math.floor(cx)
    local my = math.floor(cy)
    -- Orientation: unit vector from spot toward pill (0, 0).
    local edx = 0.5 - cx
    local edy = 0.5 - cy
    local elen = math.sqrt(edx * edx + edy * edy)
    if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
    local ux, uy = edx / elen, edy / elen   -- radial (toward pill)
    local vx, vy = -uy, ux                  -- tangential

    -- 5 sample offsets (4 corners + center) — same as the original
    -- tile_in_ellipse so the stamp matches its result exactly.
    local sample_offsets = {
      {0,   0}, {1,   0}, {0,   1}, {1,   1}, {0.5, 0.5},
    }
    local function tile_in(sx, sy)
      for _, off in ipairs(sample_offsets) do
        local rel_x = (sx + off[1]) - cx
        local rel_y = (sy + off[2]) - cy
        local pu = rel_x * ux + rel_y * uy
        local pv = rel_x * vx + rel_y * vy
        if (pu * pu) / (r_long * r_long) + (pv * pv) / (r_short * r_short) <= 1.0 then
          return true
        end
      end
      return false
    end

    local stamp = {}
    for dy = -iter_r, iter_r do
      for dx = -iter_r, iter_r do
        local sx = mx + dx
        local sy = my + dy
        if tile_in(sx, sy) then
          -- key_offset = dy*256+dx so inner loops compute key as
          -- base_key + off.key_offset (one add, no mul, one table read).
          -- proj_ux/uy baked in so scan_a/b half-ellipse checks are free.
          stamp[#stamp + 1] = {
            dx = dx, dy = dy,
            key_offset = dy * 256 + dx,
            proj = dx * ux + dy * uy,
          }
        end
      end
    end
    return stamp
  end

  for deg = 0, 359, 5 do
    ELLIPSE_STAMPS_5DEG[deg] = build_stamp(deg)
  end
  for deg = 0, 359, 45 do
    ELLIPSE_STAMPS_45DEG[deg] = build_stamp(deg)
  end
end

-- =========================================================================
-- LOS stamps — precomputed per-angle lists of intermediate tiles that must
-- be clear (no wall, no pill) for a shot from the standoff spot to the
-- target pill to have line of sight.
--
-- Pill treated as at tile (0,0). Spot tile offset and intermediate tiles
-- are stored as (dx,dy) offsets from pill tile so the runtime check is
-- purely local table lookups: base_pill_key + off.key_offset.
--
-- 5 aim points per angle: tile center + 4 corners inset 1 gu from the
-- tile boundary (so corner shots don't clip the pixel edge).
-- =========================================================================
local LOS_STAMPS_5DEG = nil

do
  local LOS_CACHE_VERSION = string.format("R=%.4f", C.ATTACK_PILL_STANDOFF)

  -- DDA ray traversal in world units (256 per tile).
  -- Returns list of {dx,dy} tile offsets relative to pill tile (0,0).
  local function dda_tiles(wx0, wy0, wx1, wy1)
    local tx0, ty0 = wx0 / 256, wy0 / 256
    local tx1, ty1 = wx1 / 256, wy1 / 256
    local cx  = math.floor(tx0)
    local cy  = math.floor(ty0)
    local ex  = math.floor(tx1)
    local ey  = math.floor(ty1)
    local ddx = tx1 - tx0
    local ddy = ty1 - ty0
    local sx  = ddx > 0 and 1 or -1
    local sy  = ddy > 0 and 1 or -1
    local tmx = (ddx ~= 0) and (((ddx > 0 and cx + 1 or cx) - tx0) / ddx) or math.huge
    local tmy = (ddy ~= 0) and (((ddy > 0 and cy + 1 or cy) - ty0) / ddy) or math.huge
    local tdx = (ddx ~= 0) and math.abs(1 / ddx) or math.huge
    local tdy = (ddy ~= 0) and math.abs(1 / ddy) or math.huge
    local tiles = {}
    for _ = 1, 64 do
      tiles[#tiles + 1] = { dx = cx, dy = cy }
      if cx == ex and cy == ey then break end
      if tmx < tmy then cx = cx + sx; tmx = tmx + tdx
      else              cy = cy + sy; tmy = tmy + tdy end
    end
    return tiles
  end

  local function compute_los_stamps()
    local R = C.ATTACK_PILL_STANDOFF
    -- Aim points on the pill tile (world units, pill at tile 0,0).
    -- Corners inset 1 gu from tile boundary to avoid pixel-edge clips.
    local aim_wx = { 128,   1, 254,   1, 254 }
    local aim_wy = { 128,   1,   1, 254, 254 }
    local stamps = {}
    for deg = 0, 355, 5 do
      if deg % 100 == 0 and bt_yield then bt_yield() end
      local rad = math.rad(deg)
      local cx  = 0.5 + math.sin(rad) * R   -- spot center, tile units (pill at 0.5,0.5)
      local cy  = 0.5 - math.cos(rad) * R
      local smx = math.floor(cx)             -- spot tile offset from pill
      local smy = math.floor(cy)
      local spot_wx = smx * 256 + 128        -- spot tile center, world units
      local spot_wy = smy * 256 + 128
      local stamp = { spot_dx = smx, spot_dy = smy, aims = {} }
      for ai = 1, 5 do
        local ray = dda_tiles(spot_wx, spot_wy, aim_wx[ai], aim_wy[ai])
        local blocking = {}
        for _, t in ipairs(ray) do
          -- Exclude spot tile and pill tile — always present, never blocking.
          if not (t.dx == smx and t.dy == smy)
          and not (t.dx == 0   and t.dy == 0 ) then
            blocking[#blocking + 1] = {
              dx = t.dx, dy = t.dy,
              key_offset = t.dy * 256 + t.dx,
            }
          end
        end
        stamp.aims[ai] = blocking
      end
      stamps[deg] = stamp
    end
    return stamps
  end

  local function serialize_stamps(stamps)
    local out = { "-- LOS stamp cache. Do not edit.\n",
                  "-- version: ", LOS_CACHE_VERSION, "\n",
                  "return {\n" }
    for deg = 0, 355, 5 do
      local s = stamps[deg]
      out[#out+1] = string.format("[%d]={spot_dx=%d,spot_dy=%d,aims={\n",
                                   deg, s.spot_dx, s.spot_dy)
      for ai = 1, 5 do
        out[#out+1] = "{"
        for _, off in ipairs(s.aims[ai]) do
          out[#out+1] = string.format("{dx=%d,dy=%d,key_offset=%d},",
                                       off.dx, off.dy, off.key_offset)
        end
        out[#out+1] = "},\n"
      end
      out[#out+1] = "},\n"
    end
    out[#out+1] = "}\n"
    return table.concat(out)
  end

  local function cache_path()
    -- Normalize: strip trailing /opt so both source and opt/ runs share
    -- the same cache file in the source brain directory.
    local dir = _G.BRAIN_DIR or "."
    dir = dir:gsub("[/\\]opt$", "")
    return dir .. "/los_stamp_cache.lua"
  end

  local function try_load()
    local path = cache_path()
    local chunk, err = loadfile(path)
    if not chunk then return nil end
    local ok, data = pcall(chunk)
    if not ok or type(data) ~= "table" then return nil end
    -- Version check: re-read first line for the version comment.
    local f = io.open(path, "r")
    if not f then return nil end
    f:read("*l")  -- skip "-- LOS stamp cache" line
    local ver_line = f:read("*l") or ""
    f:close()
    if not ver_line:find(LOS_CACHE_VERSION, 1, true) then return nil end
    return data
  end

  local function try_save(stamps)
    -- Only persist the cache in debug mode. The committed los_stamp_cache.lua
    -- matches the shipped standoff radius, so production (winbolo.exe,
    -- BRAIN_DEBUG_MODE off) loads it and never reaches here. If a constant
    -- change ever invalidates the committed file, production still computes
    -- the stamps in memory above — it just won't write a file to disk.
    if not BRAIN_DEBUG_MODE then return end
    local path = cache_path()
    local f = io.open(path, "w")
    if not f then return end
    f:write(serialize_stamps(stamps))
    f:close()
    print("[attack] LOS stamp cache written to " .. path)
  end

  LOS_STAMPS_5DEG = try_load()
  if not LOS_STAMPS_5DEG then
    print("[attack] Computing LOS stamps...")
    LOS_STAMPS_5DEG = compute_los_stamps()
    try_save(LOS_STAMPS_5DEG)
  else
    print("[attack] LOS stamps loaded from cache.")
  end
end

-- Copy precomputed stamps into the C evaluate_pill_difficulty module, plus
-- every constant its sweep needs.  Nothing the C port scores is hardcoded in
-- C any more: the terrain ids, the passability rule, the score weights and
-- the five aim points all come from here, so a tuning change in
-- constants.lua reaches both implementations at once.  (The C file shipped a
-- terrain numbering that never matched the engine's -- it read GRASS and
-- FOREST as impassable and rejected nearly every standoff spot.)
if gh_attack then
  local _passable = {}
  for tt = 0, 15 do
    _passable[tt] = (((C.TERRAIN_COST_LAND[tt] or 9999) < 9999)
                     and not U.is_water(tt)) or false
  end
  gh_attack.init_stamps(LOS_STAMPS_5DEG, ELLIPSE_STAMPS_5DEG, {
    standoff       = C.ATTACK_PILL_STANDOFF,
    danger_hotspot = C.ATTACK_DANGER_HOTSPOT,
    inf_threshold  = C.PILL_TAKE_HOSTILE_INF_THRESHOLD,
    inf_mult       = C.PILL_TAKE_HOSTILE_INF_MULT,
    tree_penalty   = C.STANDOFF_SHOT_TREE_PENALTY or 8,
    t_building     = C.T_BUILDING,
    t_halfbuild    = C.T_HALFBUILD,
    t_forest       = C.T_FOREST,
    t_deepsea      = C.T_DEEPSEA,
    t_swamp        = C.T_SWAMP,
    t_river        = C.T_RIVER,
    t_pillbox      = C.T_PILLBOX,
    passable       = _passable,
    aim_offsets    = shield.AIM_OFFSETS_TILE_FIRE,
  })
  print("[attack] gh_attack C module initialized")
end

-- Module-level constants hoisted out of the per-angle loop.
-- sample_offsets and tile_in_ellipse are only used in the stamp fallback
-- path, but were previously allocated fresh every LOS-passing angle.
local _TILE_SAMPLE_OFFSETS = {
  {0, 0}, {1, 0}, {0, 1}, {1, 1}, {0.5, 0.5},
}
-- Slate order for two-pass spot selection: short-range before long-range.
local _SLATE_ORDER = { 0, 1, 2, 3 }

-- =========================================================================
-- plan_position chunked angle sweep — full 72-spot quality preserved at
-- every tier, just spread across more ticks at lower tiers via
-- pp_spread.  ONLY runs when attack_pill is the committed goal (no
-- background pre-caching for pills the bot may never attack).
--
-- Lifecycle for one committed pill take:
--   1. plan_position substate calls M.advance_pill_eval_chunk every
--      tick.
--   2. If a fresh cached result exists in state._pill_eval_cache[pid]
--      (TTL = 250 ticks / 5 s), the chunk call returns "cached" — no
--      work done, plan_position uses the cached spots.
--   3. Otherwise, M.evaluate_pill_difficulty processes one chunk
--      (ceil(72 / pp_spread) angles).  Progress lives in
--      state._pill_eval_progress[pid].
--   4. When the cursor reaches 360 the sweep finalizes — result
--      stored at state._pill_eval_cache[pid], progress entry cleared.
--
-- pp_spread = 1 (tier 10): one chunk = all 72 angles → single tick.
-- pp_spread = 50 (tier 1): one chunk =  ceil(72/50) = 2 angles → ~36
-- ticks (~0.72 s) wall-clock per sweep.
-- =========================================================================
local _PILL_EVAL_CACHE_TTL = 250  -- ticks (5 s @ 50 Hz)

-- Fresh accumulator for a chunked plan_position scan.  All inter-chunk
-- state (spots / all_valid / counters / timings) lives here so the
-- function body can read and write through one table reference.
function M.new_pill_eval_acc(detailed)
  return {
    spots      = detailed and {} or nil,
    all_valid  = {},
    best_score = math.huge,
    best_spot  = nil,
    -- Diagnostic counters (sub-µs to update; cumulative across chunks).
    _angles_total = 0, _angles_los = 0, _angles_pass = 0,
    _t_los        = 0, _t_scan_a   = 0, _t_scan_b    = 0,
    _t_prefetch_us = 0,
    _t_total_us    = 0,  -- summed across every chunk for the sweep
  }
end

-- Two-pass selection: 50-bucket the LOS-valid spots, pick the cheapest-
-- to-reach spot in the best bucket.  Plus the optimize.log diagnostic
-- line (kept from the legacy single-shot version, now emitted after
-- the full sweep completes — _t_*_us are cumulative across chunks).
function M.finalize_pill_eval(acc, tmx, tmy)
  local COST_INF = 1e29
  local best_score = acc.best_score
  local best_spot  = acc.best_spot
  local all_valid  = acc.all_valid

  if #all_valid > 0 then
    local min_score = math.huge
    for _, s in ipairs(all_valid) do
      if s.score < min_score then min_score = s.score end
    end
    local bucket_floor = math.floor(min_score / 50) * 50
    local bucket_ceil  = bucket_floor + 50

    local bucket = {}
    for _, s in ipairs(all_valid) do
      if s.score >= bucket_floor and s.score < bucket_ceil then
        bucket[#bucket + 1] = s
      end
    end

    local costs      = {}
    local slate_used = nil
    for _, sl in ipairs(_SLATE_ORDER) do
      local ok = true
      for i, s in ipairs(bucket) do
        local c = cpf.dijkstra_cost_at(sl, s.mx, s.my, 0)
        if c >= COST_INF then ok = false; break end
        costs[i] = c
      end
      if ok then slate_used = sl; break end
    end

    if not slate_used and tmx then
      for i, s in ipairs(bucket) do
        costs[i] = cpf.estimate_cost(tmx, tmy, s.mx, s.my, 0)
      end
    end

    local best_dij = math.huge
    for i, s in ipairs(bucket) do
      local dij = costs[i] or math.huge
      if s.spot then s.spot.in_bucket = true; s.spot._dij = dij end
      if dij < best_dij then
        best_dij   = dij
        best_spot  = s
        best_score = s.score
      end
    end
  end

  acc.best_score = best_score
  acc.best_spot  = best_spot

  -- Cumulative diagnostic emit (only on full-sweep finalize).  Skip
  -- noise from sub-millisecond cheap cache-tier evals.
  if BRAIN_PROFILE_LOG and (acc._t_total_us or 0) > 1000 then
    local _pf = acc._t_prefetch_us or 0
    local _t_other = acc._t_total_us - acc._t_los - acc._t_scan_a - acc._t_scan_b - _pf
    opt.append("optimize.log", string.format(
      "  [diag] pill_eval total=%.2f prefetch=%.2f los=%.2f scan_a=%.2f scan_b=%.2f other=%.2f angles=%d/%d/%d",
      acc._t_total_us / 1000, _pf / 1000, acc._t_los / 1000, acc._t_scan_a / 1000,
      acc._t_scan_b / 1000, _t_other / 1000,
      acc._angles_pass, acc._angles_los, acc._angles_total))
  end

  return best_score, acc.spots, best_spot
end

-- (The chunked scan body itself is M.evaluate_pill_difficulty below.
-- It accepts optional start_deg/end_deg/acc args; when acc is nil it
-- runs the full 0..359 sweep in one shot and finalizes — back-compat
-- with the legacy single-shot signature.)

-- Advance one pill's chunked sweep by one chunk.  Called every tick
-- by plan_position substate while attack_pill is committed.
--
-- Returns "cached" when a fresh result already exists (no work done),
-- "in_progress" when the sweep advanced but isn't done yet, "done"
-- when the final chunk just landed (caller can read
-- state._pill_eval_cache[pid].spots).
-- Is this pill's angle sweep currently abandoned (budget-killed too many
-- times)? Read by plan_position and by goals.eval_attack_pill's candidate
-- filter so pick_goal can't re-adopt the take we just gave up on.
function M.pp_blacklisted(state, pid, now)
  local bl = state and state._pp_blacklist
  if not bl or pid == nil then return false end
  local untl = bl[pid]
  if not untl then return false end
  now = now or (state.tick or 0)
  if untl > now then return true end
  bl[pid] = nil
  return false
end

function M.advance_pill_eval_chunk(state, world, info, tmx, tmy, pid, pill)
  if state == nil or pill == nil or pid == nil or pid < 0 then
    return "cached"
  end
  state._pill_eval_cache    = state._pill_eval_cache    or {}
  state._pill_eval_progress = state._pill_eval_progress or {}
  local now = state.tick or 0
  if M.pp_blacklisted(state, pid, now) then return "blacklisted" end
  local sweep = state._pill_eval_progress[pid]
  -- "done" lingers up to 50 ticks (1 s) so the visualizer can show the
  -- completed bar; after that, drop it and let the cache TTL govern.
  if sweep and sweep.done_tick and (now - sweep.done_tick) > 50 then
    state._pill_eval_progress[pid] = nil
    sweep = nil
  end
  local cached = state._pill_eval_cache[pid]
  if cached and (now - cached.tick) <= _PILL_EVAL_CACHE_TTL
     and (not sweep or sweep.done_tick) then
    return "cached"
  end
  if not sweep then
    sweep = {
      acc        = M.new_pill_eval_acc(true),
      deg_cursor = 0,
      start_tick = now,
      mx         = pill.mx,
      my         = pill.my,
    }
    state._pill_eval_progress[pid] = sweep
  end
  local spread = (state._capacity and state._capacity.pp_spread) or 1
  if spread < 1 then spread = 1 end
  local angles_per_tick = math.ceil(72 / spread)
  -- Never schedule a chunk that can't fit the per-tick budget: deg_cursor only
  -- advances AFTER the call returns, so a chunk killed mid-call makes zero
  -- progress and the identical call re-runs next tick — forever. Tier 10 asked
  -- for all 72 angles (~9 ms) in one call; capped it takes 6 ticks per sweep.
  local cap = C.PP_ANGLES_PER_TICK_CAP or 12
  if angles_per_tick > cap then angles_per_tick = cap end
  local end_deg = sweep.deg_cursor + (angles_per_tick - 1) * 5
  if end_deg > 355 then end_deg = 355 end
  -- Budget-kill escape hatch (mirrors the shield-scan blacklist in
  -- plan_position): a kill unwinds the whole think, so nothing after the call
  -- runs — the only way to notice repeated kills is a marker written BEFORE it.
  -- Keyed on pid + cursor, so it counts kills on THE SAME chunk; a completed
  -- chunk clears it. After PP_CHUNK_KILL_TRIES the sweep is unfittable at any
  -- cursor we can reach, so abandon it and hold the pill off the pool.
  local att = state._pp_chunk_attempt
  if att and (att.pid ~= pid or att.deg ~= sweep.deg_cursor
              or (now - (att.tick or 0)) > (C.PP_CHUNK_ATTEMPT_TTL or 250)) then
    att = nil
    state._pp_chunk_attempt = nil
  end
  if att and (att.count or 0) >= (C.PP_CHUNK_KILL_TRIES or 3) then
    local until_t = now + (C.PP_BLACKLIST_TICKS or 500)
    state._pp_blacklist = state._pp_blacklist or {}
    state._pp_blacklist[pid] = until_t
    state._pill_eval_progress[pid] = nil
    state._pp_chunk_attempt = nil
    return "blacklisted"
  end
  if att then
    att.count = (att.count or 0) + 1
    att.tick  = now
  else
    state._pp_chunk_attempt = { pid = pid, deg = sweep.deg_cursor, count = 1, tick = now }
  end
  M.evaluate_pill_difficulty(pill, world, true, nil, state.phase, state, tmx, tmy,
                              sweep.deg_cursor, end_deg, sweep.acc)
  state._pp_chunk_attempt = nil   -- returned normally: this chunk was not killed
  sweep.deg_cursor = end_deg + 5
  if sweep.deg_cursor > 355 then
    local best_score, spots, best_spot = M.finalize_pill_eval(sweep.acc, tmx, tmy)
    if best_spot == nil and C.ATTACK_PILL_WALL_FALLBACK and not sweep.acc.walls then
      -- No spot has a line that misses every wall: sweep the ring again with
      -- built walls counted as shells to spend (the wall pass).
      sweep.acc = M.new_pill_eval_acc(true)
      sweep.acc.walls = true
      sweep.deg_cursor = 0
      return "in_progress"
    end
    state._pill_eval_cache[pid] = {
      tick = now, best_score = best_score, spots = spots, best_spot = best_spot,
    }
    sweep.done_tick = now
    return "done"
  end
  return "in_progress"
end

-- Drop cache + in-progress entries for pills that are no longer
-- attackable (destroyed, picked up, captured friendly, etc.).  Called
-- once per tick from the main brain loop so dead entries don't leak
-- across goal switches.
function M.purge_dead_pill_eval_entries(state, world)
  if state == nil or world == nil or world.pills == nil then return end
  local pills = world.pills
  if state._pill_eval_cache then
    for pid, _ in pairs(state._pill_eval_cache) do
      local p = pills[pid]
      if not p or not p.health or p.health <= 0
         or (p.owner ~= "hostile" and p.owner ~= "neutral") then
        state._pill_eval_cache[pid] = nil
      end
    end
  end
  if state._pill_eval_progress then
    for pid, _ in pairs(state._pill_eval_progress) do
      local p = pills[pid]
      if not p or not p.health or p.health <= 0
         or (p.owner ~= "hostile" and p.owner ~= "neutral") then
        state._pill_eval_progress[pid] = nil
      end
    end
  end
  -- Sweep-abandon blacklist: drop expired entries and any pill that stopped
  -- being an attack target, so the map can't grow across a long game.
  if state._pp_blacklist then
    local now = state.tick or 0
    for pid, untl in pairs(state._pp_blacklist) do
      local p = pills[pid]
      if untl <= now or not p or not p.health or p.health <= 0
         or (p.owner ~= "hostile" and p.owner ~= "neutral") then
        state._pp_blacklist[pid] = nil
      end
    end
  end
end

-- Draw a small progress bar above the pill currently being eval'd.
-- Only one sweep is ever in flight (plan_position drives one pill at
-- a time), so this is a single label/bar — not a per-pill swarm.
function M.draw_pill_eval_progress(viz, state)
  if not BRAIN_DEBUG_MODE then return end
  if not viz or not viz.is_on or not viz.is_on("pill_eval_progress") then return end
  if not state._pill_eval_progress then return end
  local now = state.tick or 0
  for _, sweep in pairs(state._pill_eval_progress) do
    if sweep.mx and sweep.my then
      local px, py = sweep.mx + 0.5, sweep.my + 0.5
      local angles_done = math.min(72, sweep.deg_cursor / 5)
      local total_ticks = (sweep.done_tick or now) - (sweep.start_tick or now)
      local done = sweep.done_tick ~= nil
      local label
      if done then
        label = string.format("eval done 72/72 (%dt)", total_ticks)
      else
        label = string.format("eval %d/72 (%dt)", angles_done, total_ticks)
      end
      local r, g, b = 180, 220, 255
      if done then r, g, b = 140, 240, 160 end
      local bar_w, bar_h = 2.0, 0.18
      local bar_x0 = px - bar_w / 2
      local bar_y0 = py - 1.05
      local frac = angles_done / 72
    end
  end
end

-- Always-on multiline label above the tank showing the current state of
-- each plan_position gate (chunk → spots → greens → best → shield).
-- Gives an at-a-glance read of which stage the pipeline reached and
-- which one failed, without scrolling stdout.
function M.draw_plan_trace(viz, state, info)
  if not BRAIN_DEBUG_MODE then return end
  if not viz or not viz.is_on or not viz.is_on("plan_trace") then return end
  local t = state._plan_trace
  if not t then return end
  if not info or not info.tankx then return end
  local tx = (bit.rshift(info.tankx, 8)) + 0.5
  local ty = (bit.rshift(info.tanky, 8)) + 0.5

  local goal = state.goal or {}
  local age = (state.tick or 0) - (t.tick or 0)
  local function push(lines, txt) lines[#lines + 1] = txt end
  local lines = {}
  push(lines, string.format("plan_trace  goal=%s sub=%s  age=%dt",
                            tostring(goal.kind), tostring(goal.substate), age))
  push(lines, string.format("  pill: pid=%s pmxy=(%s,%s)",
                            tostring(t.pid), tostring(t.pmx), tostring(t.pmy)))
  push(lines, string.format("  chunk: status=%s deg=%s",
                            tostring(t.chunk_status), tostring(t.chunk_deg or "--")))
  if t.spots_n then
    push(lines, string.format("  spots: n=%d los=%d", t.spots_n, t.spots_los or 0))
  else
    push(lines, "  spots: (none — chunk not done)")
  end
  if t.influence_err then
    push(lines, "  INFLUENCE PASS CRASHED:")
    local n = 0
    for line in tostring(t.influence_err):gmatch("[^\n]+") do
      push(lines, "    " .. line); n = n + 1; if n >= 3 then break end
    end
    push(lines, string.format("    spot1: mx=%s my=%s",
                              tostring(t.spot1_mx), tostring(t.spot1_my)))
  elseif t.spots_n and not t.passed_influence then
    push(lines, "  HALT after spots, before influence pass")
  elseif t.passed_influence and t.passed_collect == nil then
    push(lines, "  HALT in influence pass")
  elseif t.passed_collect ~= nil and t.passed_fallback == nil then
    push(lines, string.format("  HALT between collect=%d and fallback", t.passed_collect))
  elseif t.passed_fallback ~= nil and t.greens_n == nil then
    push(lines, string.format("  HALT after fallback=%d, before best", t.passed_fallback))
  end
  if t.greens_n then
    push(lines, string.format("  greens: n=%d  best=(%s,%s) score=%s",
                              t.greens_n,
                              tostring(t.best_mx or "--"), tostring(t.best_my or "--"),
                              t.best_score and string.format("%.1f", t.best_score) or "--"))
  end
  if t.no_best_fallback then
    push(lines, "  best: NONE -- fell to pick_standoff (no shield.scan)")
  end
  if t.shield_blacklisted then
    push(lines, "  shield: BLACKLISTED -- scan budget-killed repeatedly; shieldless plan")
  elseif t.shield_err then
    push(lines, "  shield: CRASHED")
    -- Print first 3 lines of the traceback (err msg + first 2 frames).
    local first_lines = {}
    for line in tostring(t.shield_err):gmatch("[^\n]+") do
      first_lines[#first_lines + 1] = line
      if #first_lines >= 3 then break end
    end
    for _, l in ipairs(first_lines) do push(lines, "    " .. l) end
    push(lines, "    args: " .. (t.shield_args or "?"))
  elseif t.shield_ran then
    local nb = t.shield_no_builder and " no_builder" or ""
    if t.shield_best_score then
      push(lines, string.format("  shield[%s]: cands=%d best=%.1f aim=%s%s",
                                t.shield_path or "?", t.shield_cands_n or 0,
                                t.shield_best_score,
                                tostring(t.shield_best_aim_idx), nb))
      push(lines, string.format("    actual=%d potential=%d  (a=%s p=%s n=%s)",
                                t.shield_actual_n or 0, t.shield_pots_n or 0,
                                t.shield_score_act and string.format("%.0f", t.shield_score_act) or "?",
                                t.shield_score_pot and string.format("%.0f", t.shield_score_pot) or "?",
                                t.shield_score_nb  and string.format("%.0f", t.shield_score_nb)  or "?"))
    else
      push(lines, string.format("  shield[%s]: cands=%d best=NONE%s -- PPT demoted",
                                t.shield_path or "?", t.shield_cands_n or 0, nb))
    end
  elseif t.about_to_shield_scan then
    push(lines, "  HALT INSIDE shield.scan -- crashed")
    push(lines, "  args: " .. (t.shield_args or "?"))
  elseif t.greens_n and (t.greens_n == 0 or not t.best_mx) then
    push(lines, "  shield: (skipped — no best)")
  elseif t.greens_n and t.best_mx then
    push(lines, "  HALT between greens and shield.scan")
  end

  local y = ty - 2.8
  for _, line in ipairs(lines) do
    y = y + 0.36
  end
end

-- ── Per-spot clear-aim test ──────────────────────────────────────────────
-- Which point on the target pill can THIS standoff spot actually hit?
--
-- Walks the same five aim points the shielded PPT scan fires at (pill centre
-- plus the four corners — shield.AIM_OFFSETS_TILE_FIRE, one table so planner,
-- commander and soldier all aim at the same sub-tile points) and simulates the
-- real shell from the spot's precise position to each one. Every line comes
-- back as one of three things:
--   CLEAR       shell reaches the pill tile with nothing in between
--   TREES_ONLY  only forest in the way — we can shoot the trees down first
--   BLOCKED     a live deployed pill of ANY owner, a base of any owner, or a
--               built wall / half-wall stops the shell, or it never gets there
-- A wall counts as BLOCKED even though we could shell it down: the planner has
-- a whole circle of angles to choose from, so there is no reason to take a line
-- that costs five shells before the pill takes one.
--
-- Order of preference: a CLEAR centre (simplest aim, and the cheapest answer —
-- one simulation and we are done, which is the common case), then a CLEAR
-- corner, then the TREES_ONLY line with the fewest trees.
--
-- Why this exists: 20260831_173448 bot2 — a blitz spot at (133,141) on pill
-- (124,139) was accepted with our OWN pill #6 at (126,139) straight down the
-- middle, the charge aimed at the centre, and the take died one tick after GO.
-- 20260831_222819 bot2 — 467 blocked GOs in a row, re-picking the same spot
-- every time. Rejecting the spot outright (see the caller) stops both.
--
-- Returns aim_idx, aim_wx, aim_wy (world units), aim_trees — or nil when every
-- aim point is blocked, and the caller must drop the spot.

-- One aim point, one shell simulation. Returns the forest-tile count on the
-- line (0 = perfectly clear) plus the aim point in world units, or nil when the
-- line is BLOCKED / never reaches the pill.
--
-- Split out of spot_clear_aim (below) as a plain module-level local rather than
-- a closure so the per-spot scan, which calls this five times per candidate,
-- allocates nothing extra.
-- (The body now lives in spot_margin.lua, unchanged, so squad.lua's arbiter
-- can share it. Called with no wallset it behaves exactly as before.)
local aim_line_trees = SM.aim_line_trees

-- Same test, from an arbitrary ORIGIN in world units (not just a planned
-- standoff tile). This is the shared "which point on this pill can I hit from
-- HERE?" primitive: the spot scan asks it about a candidate tile, and the
-- blocked-line ladder (try_reaim, below) asks it about the tank's live position
-- when a firing site finds its current line blocked. One implementation so the
-- planner and the fire-time re-aim never disagree about what is shootable.
--
-- prefer_idx (optional): try this aim point FIRST and return it the moment it
-- comes back clear, even if another point has fewer trees. Callers pass the aim
-- the take is already committed to, so a line that is still fine does not get
-- swapped out from under a gun that is halfway through turning onto it.
local function clear_aim_from_world(ox, oy, pmx, pmy, world, prefer_idx)
  local omx, omy = bit.rshift(ox, 8), bit.rshift(oy, 8)
  if prefer_idx and shield.AIM_OFFSETS_TILE_FIRE[prefer_idx] then
    local trees, awx, awy = aim_line_trees(ox, oy, omx, omy, pmx, pmy, world, prefer_idx)
    if trees then return prefer_idx, awx, awy, trees end
  end
  local best_i, best_trees, best_wx, best_wy = nil, nil, nil, nil
  for i = 1, 5 do
    if i ~= prefer_idx then
      local trees, awx, awy = aim_line_trees(ox, oy, omx, omy, pmx, pmy, world, i)
      if trees then
        if i == 1 and trees == 0 then
          return 1, awx, awy, 0            -- clear centre: one simulation, done
        end
        if best_trees == nil or trees < best_trees then
          best_i, best_trees, best_wx, best_wy = i, trees, awx, awy
          if trees == 0 then break end     -- clear corner: nothing beats it
        end
      end
    end
  end
  if not best_i then return nil end
  return best_i, best_wx, best_wy, best_trees
end

local function spot_clear_aim(sfx, sfy, pmx, pmy, world)
  return clear_aim_from_world(math.floor(sfx * 256 + 0.5),
                              math.floor(sfy * 256 + 0.5),
                              pmx, pmy, world)
end

-- ── Blocked-line ladder ──────────────────────────────────────────────────
-- Every site that is about to fire tests its shot line first, and every one of
-- them used to bin the WHOLE take the moment something impassable was on it.
-- loss_b6 bot3 t=9483: the blitz COMMANDER had just broadcast GO, was still 8
-- tiles short of its standoff on pill #14 at (129,130), and a second pill at
-- (130,131) — diagonally in front of the target — crossed the live gun line for
-- one tick. CHARGE_ABORT_OBSTACLE dropped the take and the soldier charged in
-- alone. Two things were wrong: we asked the question before arriving, and a
-- "no" meant giving up rather than aiming somewhere else on the same pill.
--
-- The ladder that replaces it, used at every blocked-line site:
--   0. (charge only) do not even ask until the tank is AT its standoff — a line
--      that is bad while driving in usually clears itself on arrival.
--   1. try_reaim: try the other aim points on the pill (centre + four corners)
--      from where we actually are. A different corner is very often clear when
--      the centre is not, and swapping the aim keeps the take alive with no
--      repositioning at all.
--   2. blocked_line_replan: only when NO aim point works, ban this approach
--      angle and go back to plan_position for a different SPOT. Give the take
--      up only after SANITY_PILL_REPLANS_MAX spots have failed.

-- Write one aim point onto the goal in every representation the pipeline reads.
-- Steering's PPT substates (in_range_aim / shoot_pill) read aim_mx/aim_my, while
-- charge / aim / engage read aim_wx/aim_wy through planned_aim_tile — set only
-- half and the gun keeps turning onto the line we just rejected.
local function set_goal_aim(goal, pmx, pmy, idx, awx, awy)
  local off = shield.AIM_OFFSETS_TILE_FIRE[idx] or shield.AIM_OFFSETS_TILE_FIRE[1]
  goal.aim_idx = idx
  goal.aim_wx  = awx
  goal.aim_wy  = awy
  goal.aim_mx  = pmx + off[1]
  goal.aim_my  = pmy + off[2]
end

-- Step 1. Returns:
--   "ok"      a clear aim was found (already written onto the goal) — carry on
--   "wait"    we searched too recently — carry on, do NOT abort or re-search
--   "blocked" nothing on this pill is shootable from here — caller runs step 2
-- (ox,oy) is the origin in WORLD units: the tank's live position for the sites
-- that are already at their firing position, the planned standoff for the
-- standoff sanity check. `tag` names the site and keys the rate limiter.
local function try_reaim(state, goal, world, ox, oy, pmx, pmy, now, tag, obstacle)
  -- Rate limit: the search is five shell simulations, and right after a
  -- successful re-aim the gun still needs a few ticks to swing onto the new
  -- corner — so the live line reads blocked again next tick. Hold instead of
  -- re-searching (or aborting) every tick.
  local key  = "_reaim_tick_" .. tag
  local last = goal[key]
  if last and (now - last) < (C.BLOCKED_AIM_RETRY_TICKS or 10) then return "wait" end
  goal[key] = now
  local cur  = goal.aim_idx
  local from = (cur and (shield.AIM_NAMES[cur] or tostring(cur))) or "planned"
  local idx, awx, awy, trees = clear_aim_from_world(ox, oy, pmx, pmy, world, cur)
  if not idx then return "blocked" end
  set_goal_aim(goal, pmx, pmy, idx, awx, awy)
  -- Keep the shield scan's own choice in step. On a PPT take the wall list is
  -- w.aims[best_aim_idx].potential_blockers, so leaving best_aim_idx on the old
  -- corner would build the shield around a lane we no longer fire down (and
  -- standoff_shot_obstacle's fallback would still test the old line). Only when
  -- the scan actually evaluated this aim — otherwise leave it alone.
  local w = goal._shield_scan and goal._shield_scan.best
  if w and w.aims and w.aims[idx] then w.best_aim_idx = idx end
  return "ok"
end

-- Step 2. Count this SPOT as tried, ban the approach angle (the blocker is a
-- pill — it will not move, so replanning onto the same line just loops) and
-- reset to plan_position for a fresh spot. _sanity_pill_replans is the one
-- SHARED counter for the whole take (the standoff sanity check, the blitz GO
-- gate, charge and shoot_pill all bump it) so "spots tried" is a single number
-- instead of one per site.
-- Returns (abandon, banned_bucket, tries). When abandon is true the pill has
-- already been blacklisted and the caller must clear the goal and return.
--
-- The reset set itself lives in reset_to_plan_position so the one site that
-- replans WITHOUT counting a spot against the take (the standoff sanity check's
-- 2+-walls reason, which has always just re-planned) uses exactly the same list.
local function reset_to_plan_position(state, goal)
  goal.substate                 = "plan_position"
  goal.scan_spots               = nil
  goal._shield_scan             = nil
  -- Also clear the DEFERRED scan flag: left set, the next plan_position would
  -- take the "scan already pending" branch instead of re-deciding it for the
  -- spot it is about to pick. plan_position sets it fresh when it wants a scan.
  goal._shield_scan_pending     = nil
  goal._plan_show_tick          = nil
  goal._plan_logged             = nil
  goal._approach_start          = nil
  goal._approach_last_progress  = nil
  goal._approach_last_dist      = nil
  goal._wall_build_list         = nil
  goal._wall_build_idx          = nil
  -- We are not parked at an engage spot any more. _blitz and _blitz_committed
  -- STAY: we are still this take's commander, or a soldier that already got GO.
  -- Only the handshake timers go, so the re-approach doesn't inherit this one's
  -- clock (same set the old BLITZ_GO_BLOCKED path cleared).
  goal._blitz_ready_since       = nil
  goal._blitz_timeout_ext       = nil
  goal._blitz_prog_bd           = nil
  goal._blitz_prog_tick         = nil
  goal._blitz_wait_since        = nil
  goal._blitz_wait_entry        = nil
  goal._blitz_follow_logged     = nil
  state.squad_blitz_in_position = nil
  -- A re-entered charge has to start its own accounting. Left set, the stall
  -- backstop would still be measuring progress against the FIRST charge and
  -- could fire the moment we arrive at the new spot. (commit_soak_finish is
  -- memoized on goal._soak_finish, so the re-entry does not re-decide the soak.)
  goal._charge_shells           = nil
  goal._charge_last_progress    = nil
  goal._charge_best_d2          = nil
  goal._charge_best_hp          = nil
  goal._charge_armour           = nil
  goal._charge_hits_total       = nil
  -- New spot, new geometry: let every site search again immediately.
  goal._reaim_tick_CHARGE       = nil
  goal._reaim_tick_SHOOT_PILL   = nil
  goal._reaim_tick_BLITZ_GO     = nil
  goal._reaim_tick_SANITY       = nil
end

local function blocked_line_replan(state, goal, pmx, pmy, now)
  goal._sanity_pill_replans = (goal._sanity_pill_replans or 0) + 1
  local tries  = goal._sanity_pill_replans
  local bucket = ban_current_pill_angle(state, goal, pmx, pmy, now)
  if tries >= (C.SANITY_PILL_REPLANS_MAX or 3) then
    -- Hold the pill off the pool too, or pick_goal re-adopts it within a few
    -- ticks and the whole cycle repeats.
    local pid = goal.target_id
    if pid ~= nil then
      state._pp_blacklist = state._pp_blacklist or {}
      state._pp_blacklist[pid] = now + (C.PP_BLACKLIST_TICKS or 500)
    end
    return true, bucket, tries
  end
  reset_to_plan_position(state, goal)
  return false, bucket, tries
end

-- Blitz GO gate origin, world units (C.BLITZ_GO_GATE_FROM_STANDOFF, fix C,
-- 2026-09-24). A blitz SOLDIER waits at its SETUP point, 2.25 tiles behind its
-- standoff, but the charge fires from the standoff, so the GO-time shot check
-- runs from the planned standoff float point. The commander, and KEEL, use the
-- live tank position.
local function blitz_go_origin(info, goal)
  if C.BLITZ_GO_GATE_FROM_STANDOFF and goal._blitz and not goal._blitz_cmdr
     and goal.standoff_fx and goal.standoff_fy then
    return math.floor(goal.standoff_fx * 256 + 0.5),
           math.floor(goal.standoff_fy * 256 + 0.5)
  end
  return info.tankx, info.tanky
end

-- Blitz GO gate: the LOS margin (C.BLITZ_SPOT_LOS_MARGIN, see spot_margin.lua)
-- on the line from (ox,oy) (world units) to the goal's aim point. When the
-- current aim fails it, try the other aim points (margin + shell test) and
-- re-aim onto the first that passes. Returns nil when the GO may stand, or an
-- obstacle string when no aim point passes both (caller replans).
local function blitz_go_margin(state, goal, world, ox, oy, pmx, pmy, now)
  local margin = C.BLITZ_SPOT_LOS_MARGIN or 0
  if margin <= 0 then return nil end
  local ctx = SM.new_ctx(world, pmx, pmy)
  local awx = goal.aim_wx or bit.bor((bit.lshift(pmx, 8)), 128)
  local awy = goal.aim_wy or bit.bor((bit.lshift(pmy, 8)), 128)
  local ok, f = SM.line_margin(ctx, ox / 256.0, oy / 256.0, awx / 256.0, awy / 256.0, margin)
  if ok then return nil end
  local idx, nwx, nwy, trees = SM.clear_aim_margin(ox, oy, pmx, pmy, world, margin,
    { ctx = ctx, prefer_idx = goal.aim_idx, site = "go", tick = now })
  if not idx then
    return string.format("LOS margin: blocker (%d,%d) off=%.3f need=%.3f", f.bx, f.by, f.off, f.need)
  end
  set_goal_aim(goal, pmx, pmy, idx, nwx, nwy)
  local w = goal._shield_scan and goal._shield_scan.best
  if w and w.aims and w.aims[idx] then w.best_aim_idx = idx end
  return nil
end

-- Known blitz spots a soldier's own plan_position pick must keep clear of
-- (C.BLITZ_NOSPOT_RENEGOTIATE, 2026-09-25 evening). Recorded case: after a
-- NO-SPOT, bot2's own scan picked a spot 0.25 tile from its commander's bes
-- (122.0745,144.3533). Returns a list of {fx, fy, pn} -- the commander's bes
-- plus the bes of every other soldier of that commander -- or nil when the
-- knob is off, we lead this take, or the goal is not on the blitz pill.
function M._blitz_ally_spots(state, goal, self_pn)
  if not C.BLITZ_NOSPOT_RENEGOTIATE then return nil end
  if not goal or goal._blitz_cmdr or state.squad_role == "c" then return nil end
  local cmdr = state.squad_blitz_accepted or state.squad_cmdr or state.squad_negotiate_cmdr
  if not cmdr or cmdr == self_pn then return nil end
  local call = state.blitz_calls and state.blitz_calls[cmdr]
  local pill = state.squad_blitz_target or state.squad_negotiate_pill or (call and call.pill)
  if pill == nil or pill ~= goal.target_id then return nil end
  local out = {}
  local function add(pn, b)
    if b and b ~= "" then
      local x, y = tostring(b):match("^(%-?[%d.]+),(%-?[%d.]+)$")
      x, y = tonumber(x), tonumber(y)
      if x and y then out[#out + 1] = { x, y, pn } end
    end
  end
  add(cmdr, ally_state.get_key(cmdr, "bes"))
  local now = state.tick or 0
  local dead = state.tank_dead_at
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= self_pn and pn ~= cmdr and slot.info
       and tonumber(slot.info.cmdr or "") == cmdr
       and not (dead and dead[pn] and dead[pn] > (slot.last_tick or 0)) then
      add(pn, slot.info.bes)
    end
  end
  return out
end

-- First known spot within SQUAD_BLITZ_CLASH_TILES of (fx, fy), as
-- (pn, dist), or nil. No new number: the same clash distance the
-- commander's arbiter uses.
function M._blitz_spot_clash(known, fx, fy)
  if not (known and fx and fy) then return nil end
  local clash = C.SQUAD_BLITZ_CLASH_TILES or 1
  for i = 1, #known do
    local k = known[i]
    local dx, dy = fx - k[1], fy - k[2]
    local dd = math.sqrt(dx * dx + dy * dy)
    if dd <= clash then return k[3], dd end
  end
  return nil
end

-- plan_position filter: skip every LOS spot that clashes with a known blitz
-- spot. Returns (skip, n_los, n_skip) or nil when M._blitz_ally_spots is nil
-- or there is nothing to test.
function M._blitz_ally_spot_filter(state, goal, spots, self_pn)
  local known = M._blitz_ally_spots(state, goal, self_pn)
  if not known or #known == 0 then return nil end
  local now = state.tick or 0
  local skip, n_los, n_skip = {}, 0, 0
  for _, s in ipairs(spots) do
    if s.has_los and s.cx and s.cy then
      n_los = n_los + 1
      local pn, dd = M._blitz_spot_clash(known, s.cx, s.cy)
      if pn then
        skip[s] = true
        n_skip = n_skip + 1
      end
    end
  end
  return skip, n_los, n_skip
end

-- A committed blitz SOLDIER whose line was blocked goes back to plan_position
-- (blocked_line_replan) and picks a fresh spot from the same scan as a solo
-- take would. That pick is still a BLITZ spot, so (2026-09-24):
--   * C.BLITZ_SPOT_LOS_MARGIN > 0: skip spots that fail the LOS margin, and
--     remember which aim point passed for the ones that do.
--   * C.BLITZ_SPOT_EXACT_ORIGIN: skip spots within SQUAD_BLITZ_CLASH_TILES of
--     the commander's broadcast standoff (bes). 20260924_224514 bot0: the
--     replan landed on the commander's own standoff (118,142).
-- Returns skip (spot -> true), aims (spot -> {idx, awx, awy}) -- or nil when
-- the filter is off, this is not a committed soldier, or it would leave no
-- spot at all (then plan_position picks exactly as before, and says so).
local function blitz_soldier_replan_filter(state, goal, world, spots, pmx, pmy)
  if not (goal._blitz and goal._blitz_started and not goal._blitz_cmdr
          and state.squad_cmdr) then return nil end
  local margin = C.BLITZ_SPOT_LOS_MARGIN or 0
  local exact  = C.BLITZ_SPOT_EXACT_ORIGIN
  if margin <= 0 and not exact then return nil end
  local now = state.tick or 0
  local cfx, cfy
  if exact then
    local b = ally_state.get_key(state.squad_cmdr, "bes")
    if b and b ~= "" then
      local x, y = b:match("^(%-?[%d.]+),(%-?[%d.]+)$")
      cfx, cfy = tonumber(x), tonumber(y)
    end
  end
  local clash = C.SQUAD_BLITZ_CLASH_TILES or 1
  local ctx = (margin > 0) and SM.new_ctx(world, pmx, pmy) or nil
  local skip, aims = {}, {}
  local n_los, n_skip, n_clash, n_margin = 0, 0, 0, 0
  for _, s in ipairs(spots) do
    if s.has_los and s.cx and s.cy then
      n_los = n_los + 1
      local bad = false
      if cfx and cfy then
        local dx, dy = s.cx - cfx, s.cy - cfy
        local dd = math.sqrt(dx * dx + dy * dy)
        if dd <= clash then
          bad = true
          n_clash = n_clash + 1
        end
      end
      if not bad and margin > 0 then
        -- The scan shell-tested s.aim_idx from exactly (s.cx,s.cy): trust it.
        local idx, awx, awy = SM.clear_aim_margin(
          math.floor(s.cx * 256 + 0.5), math.floor(s.cy * 256 + 0.5),
          pmx, pmy, world, margin,
          { ctx = ctx, prefer_idx = s.aim_idx, trusted_idx = s.aim_idx,
            trusted_trees = s.aim_trees, site = "replan", tick = now })
        if idx then aims[s] = { idx, awx, awy }
        else bad = true; n_margin = n_margin + 1 end
      end
      if bad then skip[s] = true; n_skip = n_skip + 1 end
    end
  end
  if n_los == 0 or n_skip >= n_los then return nil end
  return skip, aims
end

-- Fix A, after a blocked replan (C.BLITZ_SPOT_EXACT_ORIGIN): a committed soldier
-- that re-planned has a NEW standoff, but kept broadcasting the OLD engage spot
-- (bes), so the commander's clash check could not see it land on the
-- commander's own standoff. Once plan_position has picked, copy the new
-- standoff into the engage spot the bes broadcast and the arbiter read.
--
-- goal._blitz_brj_resync (set by the committed-soldier brj replan, 2026-09-25)
-- forces the same copy with EXACT off too: the commander must see the NEW spot,
-- or it keeps rejecting the old one. The float falls back to the tile centre
-- when the pick stored none.
local function blitz_sync_engage(state, goal, now)
  local resync = goal._blitz_brj_resync
  if not (C.BLITZ_SPOT_EXACT_ORIGIN or resync) then return end
  local sfx = goal.standoff_fx or (resync and goal.standoff_mx and goal.standoff_mx + 0.5)
  local sfy = goal.standoff_fy or (resync and goal.standoff_my and goal.standoff_my + 0.5)
  if not (goal._blitz_started and state.squad_blitz_engage_mx
          and goal.standoff_mx and goal.standoff_my
          and sfx and sfy) then return end
  if goal.substate == "plan_position" then return end   -- still picking
  goal._blitz_brj_resync = nil
  local efx = state.squad_blitz_engage_fx or (state.squad_blitz_engage_mx + 0.5)
  local efy = state.squad_blitz_engage_fy or (state.squad_blitz_engage_my + 0.5)
  if math.abs(efx - sfx) < 0.01 and math.abs(efy - sfy) < 0.01 then return end
  state.squad_blitz_engage_mx, state.squad_blitz_engage_my = goal.standoff_mx, goal.standoff_my
  state.squad_blitz_engage_fx, state.squad_blitz_engage_fy = sfx, sfy
  state.squad_blitz_engage_deg = goal._chosen_deg
  -- 2026-09-26: the reject flag was read against the OLD spot. While we sat
  -- in plan_position this function returned early, so squad.update's
  -- read_cmdr_brj kept matching the commander's brj (still naming the old
  -- spot) against the old engage fields and set the flag again. Left set,
  -- M.blitz_brj_replan below saw it with the NEW spot's key and replanned a
  -- second time for one reject: a good angle banned and one of the
  -- SANITY_PILL_REPLANS_MAX tries used. A reject of the new spot sets it
  -- again at the next read.
  state._blitz_call_rejected = nil
end

-- brj for a COMMITTED soldier (bug, 20260925_105315 bot1 t=3032+): the
-- commander rejected the spot we already committed to (shot_blocked: a pill
-- appeared on our line), but only a NEGOTIATING soldier ever acted on brj, so
-- we drove to the rejected spot forever. Drop the spot: ban its angle and
-- replan (the blocked-line path, same try count and abandon), and re-broadcast
-- the new spot once picked (goal._blitz_brj_resync -> blitz_sync_engage). Once
-- per rejected spot: brj for the old spot keeps arriving until the new bes
-- lands. Returns "none" (nothing to do), "replan" or "abandon" (goal cleared).
function M.blitz_brj_replan(state, goal, pmx, pmy, now)
  if not (state._blitz_call_rejected and state.squad_blitz_accepted
          and goal._blitz_started and not goal._blitz_committed
          and (goal.substate == "approach" or goal.substate == "blitz_wait")) then
    return "none"
  end
  local rfx = state.squad_blitz_engage_fx
              or (state.squad_blitz_engage_mx and state.squad_blitz_engage_mx + 0.5)
  local rfy = state.squad_blitz_engage_fy
              or (state.squad_blitz_engage_my and state.squad_blitz_engage_my + 0.5)
  local rkey = rfx and string.format("%.4f,%.4f", rfx, rfy) or "-"
  if rkey == goal._blitz_brj_spot then return "none" end
  goal._blitz_brj_spot = rkey
  local sub = goal.substate
  local abandon, bucket, tries = blocked_line_replan(state, goal, pmx, pmy, now)
  if abandon then
    clear_attack_goal(state, string.format("blitz: commander rejected committed spot %d times", tries or 0))
    return "abandon"
  end
  goal._blitz_brj_resync = true
  return "replan"
end
M._blitz_sync_engage = blitz_sync_engage   -- unit tests

-- Soldier blitz_wait backstop follow (C.BLITZ_SOLDIER_WAIT_FOLLOW_CMDR,
-- 2026-09-25 evening). True when the soldier should restart its
-- SQUAD_BLITZ_WAIT_TIMEOUT count from the commander's last message
-- (cslot.last_tick): blitz-only with BLITZ_ONLY_EXTEND_WAIT (the only case a
-- commander extends its wait, at most BLITZ_ONLY_EXTEND_MAX times), the
-- commander's call is open on our pill,
-- its bac names us, and it has sent something since the count last started.
-- Logs BLITZ_WAIT_FOLLOW once per goal.
function M.blitz_soldier_wait_follow(state, goal, cslot, cmdr, self_pn, now)
  if not (C.BLITZ_SOLDIER_WAIT_FOLLOW_CMDR and C.BLITZ_ONLY_EXTEND_WAIT
          and squad.blitz_only(state)) then return false end
  if not (cslot and cslot.info and goal and goal._blitz_wait_since) then return false end
  local call = state.blitz_calls and state.blitz_calls[cmdr]
  if not (call and call.pill == goal.target_id) then return false end
  local named = false
  local bac = cslot.info.bac
  if bac then
    for s in string.gmatch(tostring(bac), "%d+") do
      if tonumber(s) == self_pn then named = true; break end
    end
  end
  if not named then return false end
  local last = cslot.last_tick or 0
  if last <= goal._blitz_wait_since then return false end
  -- Log once, on the first tick the follow keeps us past the old backstop.
  local entry = goal._blitz_wait_entry or goal._blitz_wait_since
  if not goal._blitz_follow_logged
     and (now - entry) > (C.SQUAD_BLITZ_WAIT_TIMEOUT or 1500) then
    goal._blitz_follow_logged = true
  end
  return true
end

-- HUMAN ORDER BLITZ CAP (C.HUMAN_ATTACK_BLITZ_WAIT_MAX_S, 2026-09-28).
-- True when this attack_pill goal is a person's held order (state._order is an
-- attack_pill order on the same pill) and the cap has run out.  The clock
-- starts on the first tick this is asked for that order -- the first tick the
-- bot runs the attack goal for it -- and is kept on state, keyed by the
-- order's oid and take tick, so a replan that rebuilds the goal table does not
-- restart it, and a new order on the same pill does.  0 / nil = no cap, and a
-- goal the bot picked for itself always answers false.
-- update_attack_substate stores the answer in goal._hcap_over every tick; the
-- blitz waits read that field.
function M.human_blitz_cap_over(goal, state, now)
  local cap_s = C.HUMAN_ATTACK_BLITZ_WAIT_MAX_S
  if not (C.BOT_COMMANDS_ENABLED and cap_s and cap_s > 0) then return false end
  local o = state._order
  if not (o and o.kind == "attack_pill" and goal and goal.target_id
          and o.tid == goal.target_id) then
    return false
  end
  local hc = state._hcap
  if not hc or hc.oid ~= o.oid or hc.since ~= o.since then
    hc = { oid = o.oid, since = o.since, t0 = now, logged = false }
    state._hcap = hc
  end
  -- 50 ticks per second.
  if (now - hc.t0) < cap_s * 50 then return false end
  if not hc.logged then
    hc.logged = true
  end
  return true
end

-- HUMAN ORDER GATHER CAP (C.HUMAN_ATTACK_GATHER_MAX_S, 2026-09-29).
-- Same order test as human_blitz_cap_over, own clock: it starts on the first
-- tick this is asked for that order, which is the first gather_trees tick
-- (only the gather_trees handler asks).  Kept on state, keyed by oid + take
-- tick, so a replan does not restart it.  True = end gathering now through
-- the PPT_GATHER_TIMEOUT exit.  0 / nil = no cap; a goal the bot picked for
-- itself always answers false.
function M.human_gather_cap_over(goal, state, now)
  local cap_s = C.HUMAN_ATTACK_GATHER_MAX_S
  if not (C.BOT_COMMANDS_ENABLED and cap_s and cap_s > 0) then return false end
  local o = state._order
  if not (o and o.kind == "attack_pill" and goal and goal.target_id
          and o.tid == goal.target_id) then
    return false
  end
  local hc = state._hgcap
  if not hc or hc.oid ~= o.oid or hc.since ~= o.since then
    hc = { oid = o.oid, since = o.since, t0 = now, logged = false }
    state._hgcap = hc
  end
  -- 50 ticks per second.
  if (now - hc.t0) < cap_s * 50 then return false end
  if not hc.logged then
    hc.logged = true
  end
  return true
end

-- Commander GO verdict in blitz_wait, as a pure function (unit tests).
--   bo_hold    squad.blitz_only(state) and C.BLITZ_ONLY_EXTEND_WAIT
--   timed_out  READY_TIMEOUT (+ extensions) elapsed
--   ready/total  soldiers ready / committed; party = 1 + total
--   set_inwait   PARKED set: commander + soldiers in a ready substate
--   ext_n        extensions already added on this goal (nil = 0)
--   ext_max      cap on ext_n (nil = C.BLITZ_ONLY_EXTEND_MAX)
-- Returns "extend" (blitz-only, timed out, parked < min, ext_n < ext_max: add
-- a timeout), "abandon" (timed out and party < min; or blitz-only, timed out,
-- parked < min with the extensions used up), "go", or "wait".
-- bo_hold false = exactly the old rules.
-- 2026-09-26: the extensions are capped. With no cap a commander that nobody
-- joined extended forever, its soldiers followed it (BLITZ_SOLDIER_WAIT_FOLLOW
-- _CMDR) and BLITZ_NO_BUILD_ACTIVE kept pill building off the whole time. Once
-- the cap is reached, a blitz-only commander still short of parked tanks gives
-- the take up: blitz-only never charges short.
function M.blitz_cmdr_go_verdict(bo_hold, timed_out, ready, total, party, set_inwait, bmin, ext_n, ext_max)
  local early_go = set_inwait >= bmin
  if bo_hold and timed_out and not early_go then
    ext_max = ext_max or C.BLITZ_ONLY_EXTEND_MAX or 3
    if (ext_n or 0) < ext_max then return "extend" end
    return "abandon"
  end
  if timed_out and party < bmin then return "abandon" end
  local go_now
  if bo_hold then
    go_now = early_go                  -- parked set >= MIN, nothing else
  else
    go_now = (ready >= total and party >= bmin) or timed_out or early_go
  end
  return go_now and "go" or "wait"
end

-- ── C / Lua parity self-check ────────────────────────────────────────────
-- Set _G.GH_ATTACK_PARITY_CHECK (or pass the "attackparity" token in
-- -bot-init's [arg]) and every C evaluation is repeated through the Lua
-- sweep, the two answers logged as one ATTACK_PARITY line.  Purely
-- diagnostic: the decision returned is always the C one, whatever this
-- prints.  The re-entry is what _parity_lua_only guards -- without it the
-- second call would take the C fast path again and compare C with itself.
--
-- The whole harness lives inside `if BRAIN_DEBUG_MODE`, so lua_strip removes
-- it from opt/ and production never sees the flag, the second sweep, or the
-- string formatting.  _parity_lua_only stays OUTSIDE that block: the sweep
-- reads it, and in opt/ it must still resolve to a plain `false` upvalue.
local _parity_lua_only = false


function M.evaluate_pill_difficulty(pill, world, detailed, scan_step, phase, state, tmx, tmy,
                                     start_deg, end_deg, acc)
  -- Per-section diagnostic accumulators. Sub-µs to update; enables
  -- breakdown of where the ~2 ms first-eval cost lives. Logged via
  -- optimize.log when total > 1 ms.
  local _t_func0      = clock_us()
  -- Chunked mode: caller passes acc + an explicit [start_deg, end_deg]
  -- range. We accumulate into acc and skip the post-loop two-pass
  -- selection (caller does that via M.finalize_pill_eval once all
  -- chunks are done). Single-shot mode: acc is nil, we run the full
  -- 0..359 loop and the post-loop selection in one call (legacy).
  local chunked    = acc ~= nil and not acc._single
  acc              = acc or M.new_pill_eval_acc(detailed)
  local pmx, pmy = pill.mx, pill.my
  local step_deg = scan_step or C.ATTACK_SCAN_DEGREES

  -- C fast path: skip the per-tile Lua overhead entirely.
  -- Only activates for the non-detailed hot path (step=5, no banned angles).
  -- _parity_lua_only is set only by the parity self-check below, which
  -- re-enters this function to run the Lua sweep it is comparing against.
  if not detailed and gh_attack and step_deg == 5 and not _parity_lua_only
      and not acc.walls
      and not (state and state.banned_pill_angles
               and state.banned_pill_angles[pmy * 256 + pmx]) then
    -- The WHOLE world, not just world.pill_at: the C sweep needs base_at and
    -- the live world.pills table too (hostile bases penalise the maneuver
    -- area, and the clear-aim gate rejects a spot whose only shot lines are
    -- crossed by a base or a live pill).  Passing the world table is also
    -- what selects the C parity evaluator -- GoalHunter 1.5 / 1.6 link the
    -- same C object and still pass a bare pill_at, which keeps them on the
    -- frozen legacy evaluator.
    gh_attack.sync_pill_at(world, pmx, pmy)
    local phase_not_opening = (phase and phase ~= "opening") and true or false
    local self_contrib = threat.pill_contrib and threat.pill_contrib[pmy * 256 + pmx] or nil
    local c_score, c_mx, c_my, c_deg = gh_attack.evaluate_pill_difficulty(
      pmx, pmy, step_deg, phase_not_opening,
      tmx or -1, tmy or -1, self_contrib)
    if c_mx >= 0 then
      return c_score, nil, { mx = c_mx, my = c_my, deg = c_deg, score = c_score }
    elseif not C.ATTACK_PILL_WALL_FALLBACK then
      return math.huge, nil, nil
    end
    -- No spot from C and the wall pass is on: the C sweep has no wall pass,
    -- so run the Lua sweep below with built walls counted as shells.
    acc.walls = true
  end
  -- Wall pass (C.ATTACK_PILL_WALL_FALLBACK): built walls do not block a spot's
  -- line, they add the shells needed to shoot through them (see
  -- spot_margin.aim_line_trees). Pills and bases still block.
  local walls = acc.walls and true or false
  local _walls_prev = SM.walls_ok

  local R = C.ATTACK_PILL_STANDOFF
  -- Banned-angle map for this pill (set by approach-timeout handler).
  -- nil if state isn't passed (legacy callers / unit tests) or this
  -- pill has no bans.
  local banned_for_pill = nil
  local now_for_ban = 0
  if state and state.banned_pill_angles then
    banned_for_pill = state.banned_pill_angles[pmy * 256 + pmx]
    now_for_ban = state.tick or 0
  end
  local safe_r = C.ATTACK_SAFE_RADIUS
  -- Pick the right precomputed stamp set for this scan resolution.
  -- Falls back to nil for other step values (the runtime then uses
  -- the slower tile_in_ellipse path).
  local stamps = (step_deg == 5) and ELLIPSE_STAMPS_5DEG
              or (step_deg == 45) and ELLIPSE_STAMPS_45DEG
              or nil

  -- Abort-safe accumulation: this chunk's spots go into SCRATCH tables and are
  -- merged into acc only at the write-back below. The per-tick budget hook kills
  -- a chunk by unwinding the whole think, so appending straight into acc.spots /
  -- acc.all_valid (which is what binding to them did — they're the same table
  -- objects) left the killed chunk's partial results behind; the identical
  -- retry then appended them again, every tick, forever.
  local spots      = detailed and {} or nil
  local all_valid  = {}
  local best_score = acc.best_score
  local best_spot  = acc.best_spot
  -- Hoist diagnostic locals out of acc so the inner-loop body can still
  -- use them by short name (kept the same names so the body code didn't
  -- have to change). Written back to acc just before return.
  local _angles_total = acc._angles_total
  local _angles_los   = acc._angles_los
  local _angles_pass  = acc._angles_pass
  local _t_los        = acc._t_los
  local _t_scan_a     = acc._t_scan_a
  local _t_scan_b     = acc._t_scan_b

  -- Cache frequently-accessed tables as locals — single table index per
  -- tile in the stamp loops, no function call overhead, no GC pressure.
  local _pill_grid_at = gh_threat and gh_threat.pill_grid_at
  local _cov_grid_at  = gh_threat and gh_threat.cov_grid_at
  local _base_at     = world.base_at
  local _pill_at     = world.pill_at
  local _ttype       = U.ttype
  local _in_map      = U.in_map
  local _self_pcontrib = threat.pill_contrib[pmy * 256 + pmx]
  local _t_prefetch_us = 0

  -- Chunked range: [start_deg, end_deg] step step_deg. Single-shot
  -- callers pass nil, nil → full 0..359 sweep.
  local _lo = start_deg or 0
  local _hi = end_deg   or 359
  for deg = _lo, _hi, step_deg do
    -- Skip banned approach angles (set by approach timeout). 5° bucket.
    if banned_for_pill then
      local bucket = math.floor((deg % 360) / 5) * 5
      local exp = banned_for_pill[bucket]
      if exp and now_for_ban < exp then goto next_spot end
    end
    local rad = math.rad(deg)
    local cx = pmx + 0.5 + math.sin(rad) * R
    local cy = pmy + 0.5 - math.cos(rad) * R
    local mx = math.floor(cx)
    local my = math.floor(cy)
    if not U.in_map(mx, my) then goto next_spot end

    local tt = U.ttype(mx, my)
    local passable = (C.TERRAIN_COST_LAND[tt] or 9999) < 9999 and not U.is_water(tt)
    if not passable then goto next_spot end

    -- LOS test: use precomputed stamp (intermediate tiles the ray crosses
    -- from the spot to each of 5 aim points on the pill). Any clear aim
    -- wins. Falls back to pill_shots_clear for non-5° step sizes.
    _angles_total = _angles_total + 1
    local _t_los0 = clock_us()
    local has_los = false
    local _los_stamp = LOS_STAMPS_5DEG and LOS_STAMPS_5DEG[deg]
    if walls then
      has_los = true   -- the shell test below is the whole test in the wall pass
    elseif _los_stamp and step_deg == 5 then
      local _bpk = pmy * 256 + pmx   -- base pill key
      for _ai = 1, 5 do
        local _blocked = false
        local _aim = _los_stamp.aims[_ai]
        for _ti = 1, #_aim do
          local _off = _aim[_ti]
          local _tx, _ty = pmx + _off.dx, pmy + _off.dy
          local _ttt = _ttype(_tx, _ty)
          if _ttt == C.T_BUILDING or _ttt == C.T_HALFBUILD then
            _blocked = true; break
          end
          if _pill_at[_bpk + _off.key_offset] then
            _blocked = true; break
          end
        end
        if not _blocked then has_los = true; break end
      end
    else
      local spot_wx = bit.bor((bit.lshift(mx, 8)), 128)
      local spot_wy = bit.bor((bit.lshift(my, 8)), 128)
      has_los = PF.pill_shots_clear(spot_wx, spot_wy, pill, world,
                                    cpf.SHOT_TANK, 0)
    end
    _t_los = _t_los + (clock_us() - _t_los0)

    -- ── Clear-aim gate ──
    -- A spot we cannot actually shoot the pill from is no spot at all, so it is
    -- rejected here the same way an impassable or banned tile is — it never
    -- enters spots/all_valid, and nothing downstream can pick it. The stamp LOS
    -- above is only a cheap pre-filter (walls + pills, tile centres, no bases,
    -- no trees, no shell physics); this is the real shell simulation from the
    -- spot's precise float position, and it also decides WHICH point on the pill
    -- this spot will aim at. Only spots that got past the stamp pay for it, and
    -- a clear centre costs a single simulation.
    local aim_idx, aim_wx, aim_wy, aim_trees
    if has_los then
      local _t_aim0 = clock_us()
      SM.walls_ok = walls
      aim_idx, aim_wx, aim_wy, aim_trees = spot_clear_aim(cx, cy, pmx, pmy, world)
      SM.walls_ok = _walls_prev
      _t_los = _t_los + (clock_us() - _t_aim0)
      if not aim_idx then goto next_spot end
    end

    local score_a, score_b, score_d, score_e, total_score = 0, 0, 0, 0, 999
    local maneuver_tiles = nil  -- only populated in BRAIN_DEBUG_MODE
    if has_los then
      local total_danger = 0
      local safe_tiles = 0
      local terrain_penalty = 0
      -- Ellipse oriented toward the pill: long axis = radial (safe_r),
      -- short axis = tangential (safe_r / 3).  Compute unit vector from
      -- spot toward pill to define the axes.
      local edx = pmx + 0.5 - cx
      local edy = pmy + 0.5 - cy
      local elen = math.sqrt(edx * edx + edy * edy)
      if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
      local ux, uy = edx / elen, edy / elen  -- radial (toward pill)
      local vx, vy = -uy, ux                 -- tangential (perpendicular)
      -- Match the stamp + inspector viz: r_long sourced from safe_r
      -- so all three (runtime fallback, precomputed stamp, drawn
      -- ellipse) stay in lockstep.
      local r_long  = safe_r         -- radial half-length
      local r_short = safe_r / 3.0   -- tangential half-width

      -- Pre-compute crossfire: count hostile pills where ANY ellipse tile
      -- is in range.  Once a pill is marked as covering, skip to the next.
      -- Iterate up to ceil(r_long) + 1 to cover the long axis (buffer for the
      -- offset between integer tile mx,my and the precise float spot cx,cy).
      local iter_r = math.ceil(r_long) + 1
      -- Use the precomputed ellipse stamp for this angle if available;
      -- otherwise fall back to the slower nested-loop + tile_in_ellipse
      -- path. The stamp is a flat list of (dx, dy) offsets relative to
      -- the spot tile (mx, my) — exactly the tiles inside the ellipse
      -- for this angle.
      local stamp = stamps and stamps[deg] or nil

      -- Build tile_in_ellipse only when the stamp fallback is needed.
      -- Hoisted here so both scan A and scan B else-branches can share it.
      local tile_in_ellipse = nil
      if not stamp then
        tile_in_ellipse = function(sx, sy)
          for _, off in ipairs(_TILE_SAMPLE_OFFSETS) do
            local rel_x = (sx + off[1]) - cx
            local rel_y = (sy + off[2]) - cy
            local pu = rel_x * ux + rel_y * uy
            local pv = rel_x * vx + rel_y * vy
            if (pu*pu)/(r_long*r_long) + (pv*pv)/(r_short*r_short) <= 1.0 then
              return true
            end
          end
          return false
        end
      end

      _angles_los = _angles_los + 1

      -- ── Crossfire (Scan A) ──
      -- Worst (highest-coverage) tile in the BACK HALF of the ellipse
      -- (away from the pill). The pill-side half is already expected to
      -- take fire from the target — only unexpected crossfire from behind
      -- matters for positioning. Tiles with positive projection onto ux,uy
      -- (toward pill) are skipped.
      local _t_a0 = clock_us()
      local max_coverage = 0
      local _base_key = my * 256 + mx
      if stamp then
        for i = 1, #stamp do
          local off = stamp[i]
          if off.proj <= 0 then
            local cov = _cov_grid_at and _cov_grid_at(_base_key + off.key_offset) or 0
            if cov > max_coverage then max_coverage = cov end
          end
        end
      else
        for dy2 = -iter_r, iter_r do
          for dx2 = -iter_r, iter_r do
            local proj = dx2 * ux + dy2 * uy
            if proj <= 0 then
              local sx2, sy2 = mx + dx2, my + dy2
              if tile_in_ellipse(sx2, sy2) and _in_map(sx2, sy2) then
                local cov = _cov_grid_at and _cov_grid_at(sy2 * 256 + sx2) or 0
                if cov > max_coverage then max_coverage = cov end
              end
            end
          end
        end
      end
      if max_coverage > 1 then
        score_e = 100 * (max_coverage - 1)
      end
      _t_scan_a = _t_scan_a + (clock_us() - _t_a0)

      -- ── Maneuver area scan (Scan B) ──
      local _t_b0 = clock_us()
      local forest_count = 0
      if stamp then
        for i = 1, #stamp do
          local off = stamp[i]
          if off.proj > 0 then goto next_scan_b_tile end
          local sx, sy = mx + off.dx, my + off.dy
          local key = _base_key + off.key_offset
          local d = _pill_grid_at and _pill_grid_at(key) or 0
          if _self_pcontrib then
            local sd = _self_pcontrib[key]
            if sd then d = math.max(0, d - sd) end
          end
          local tt = _ttype(sx, sy)
          if tt == C.T_FOREST then forest_count = forest_count + 1 end
          total_danger = total_danger + d
          safe_tiles = safe_tiles + 1
          local be = _base_at[key]
          if tt == C.T_DEEPSEA or (be and be.base and be.base.owner == "hostile") then
            terrain_penalty = terrain_penalty + 1000
          elseif tt == C.T_BUILDING or tt == C.T_HALFBUILD or tt == C.T_SWAMP
              or tt == C.T_RIVER   or tt == C.T_PILLBOX then
            terrain_penalty = terrain_penalty + 100
          else
            local plist = _pill_at[key]
            if plist then
              for _, e in ipairs(plist) do
                if e.pill and e.pill.owner == "friendly" and (e.pill.health or 0) > 0 then
                  terrain_penalty = terrain_penalty + 100; break
                end
              end
            end
          end
          ::next_scan_b_tile::
        end
      else
        for dy = -iter_r, iter_r do
          for dx = -iter_r, iter_r do
            local proj2 = dx * ux + dy * uy
            if proj2 > 0 then goto next_scan_b_fb end
            local sx, sy = mx + dx, my + dy
            if tile_in_ellipse(sx, sy) and _in_map(sx, sy) then
              local key = sy * 256 + sx
              local d = _pill_grid_at and _pill_grid_at(key) or 0
              if _self_pcontrib then
                local sd = _self_pcontrib[key]
                if sd then d = math.max(0, d - sd) end
              end
              local tt = _ttype(sx, sy)
              if tt == C.T_FOREST then forest_count = forest_count + 1 end
              total_danger = total_danger + d
              safe_tiles = safe_tiles + 1
              local be = _base_at[key]
              if tt == C.T_DEEPSEA or (be and be.base and be.base.owner == "hostile") then
                terrain_penalty = terrain_penalty + 1000
              elseif tt == C.T_BUILDING or tt == C.T_HALFBUILD or tt == C.T_SWAMP
                  or tt == C.T_RIVER   or tt == C.T_PILLBOX then
                terrain_penalty = terrain_penalty + 100
              else
                local plist = _pill_at[key]
                if plist then
                  for _, e in ipairs(plist) do
                    if e.pill and e.pill.owner == "friendly" and (e.pill.health or 0) > 0 then
                      terrain_penalty = terrain_penalty + 100; break
                    end
                  end
                end
              end
            end
            ::next_scan_b_fb::
          end
        end
      end
      if forest_count > 0 then
        total_danger = math.max(0, total_danger - forest_count * 0.75)
      end
      score_a = safe_tiles > 0 and (total_danger / safe_tiles) or 999
      score_b = 0
      if total_danger / math.max(1, safe_tiles) >= C.ATTACK_DANGER_HOTSPOT then
        score_b = 10
      end
      score_d = terrain_penalty
      total_score = score_a + score_b + score_d + score_e
      -- Prefer a spot whose CHOSEN aim line crosses fewer trees — each tree eats
      -- a shell before the pill takes one. This is the line we will really
      -- shoot down (the clear-aim gate above picked it), not the pill centre.
      -- Pills, bases and walls no longer score anything here: a spot with no
      -- clean line to ANY of the five aim points was rejected outright by the
      -- gate, so there is nothing left for STANDOFF_SHOT_BLOCKED_PENALTY to
      -- soften. It used to be the only defence, and a 200-point nudge is not a
      -- defence — 20260831_173448 bot2 took a spot with our own pill dead on the
      -- centre line anyway, because the rest of the spot scored well enough.
      if aim_trees and aim_trees > 0 then
        total_score = total_score + aim_trees * (C.STANDOFF_SHOT_TREE_PENALTY or 8)
      end
      -- Mid/late-game spots deep in enemy influence are much riskier to
      -- hold during the take — boost their cost so we prefer takes
      -- through friendlier or contested ground when one exists.
      local hostile_inf_mult = 1
      if phase and phase ~= "opening" then
        local infl = cpf.influence_at(mx, my) or 0
        if infl <= C.PILL_TAKE_HOSTILE_INF_THRESHOLD then
          hostile_inf_mult = C.PILL_TAKE_HOSTILE_INF_MULT
          total_score = total_score * hostile_inf_mult
        end
      end
      local spot_ref = nil
      if detailed then
        spot_ref = {
          cx = cx, cy = cy, mx = mx, my = my,
          has_los = has_los,
          score_a = score_a, score_b = score_b, score_d = score_d, score_e = score_e,
          total_score = total_score, deg = deg,
          -- The point on the pill this spot can actually hit (world units) plus
          -- how many trees are in the way. plan_position copies these onto the
          -- goal so charge/engage and the blitz GO gate all use the same line.
          aim_idx = aim_idx, aim_wx = aim_wx, aim_wy = aim_wy, aim_trees = aim_trees,
          walls = walls or nil,
          maneuver_tiles = BRAIN_DEBUG_MODE and maneuver_tiles or nil,
        }
        spots[#spots + 1] = spot_ref
      end
      all_valid[#all_valid + 1] = {
        mx = mx, my = my, cx = cx, cy = cy, score = total_score, deg = deg,
        hostile_inf_mult = hostile_inf_mult,
        aim_idx = aim_idx, aim_wx = aim_wx, aim_wy = aim_wy, aim_trees = aim_trees,
        walls = walls or nil,
        spot = spot_ref,
      }
      _angles_pass = _angles_pass + 1
      _t_scan_b = _t_scan_b + (clock_us() - _t_b0)
    end
    ::next_spot::
  end

  -- Write the diagnostic + accumulator state back to acc so the next
  -- chunk (and the legacy single-shot post-loop) sees the latest. Everything
  -- from here down is append-only bookkeeping — nothing above this point
  -- mutated acc, so a budget kill mid-loop leaves acc exactly as it arrived and
  -- the retry re-does the chunk cleanly instead of duplicating it.
  if spots then
    local dst = acc.spots
    if dst then
      local n = #dst
      for i = 1, #spots do dst[n + i] = spots[i] end
    else
      acc.spots = spots
    end
  end
  do
    local dst = acc.all_valid
    if dst then
      local n = #dst
      for i = 1, #all_valid do dst[n + i] = all_valid[i] end
    else
      acc.all_valid = all_valid
    end
  end
  acc.best_score    = best_score
  acc.best_spot     = best_spot
  acc._angles_total = _angles_total
  acc._angles_los   = _angles_los
  acc._angles_pass  = _angles_pass
  acc._t_los        = _t_los
  acc._t_scan_a     = _t_scan_a
  acc._t_scan_b     = _t_scan_b
  acc._t_prefetch_us = _t_prefetch_us
  -- Accumulate this chunk's wall time so finalize_pill_eval emits the
  -- correct cumulative total when BRAIN_PROFILE_LOG is on.
  acc._t_total_us   = (acc._t_total_us or 0) + (clock_us() - _t_func0)

  -- Chunked mode skips the post-loop two-pass selection; caller invokes
  -- M.finalize_pill_eval(acc, tmx, tmy) once all chunks are done. Returns the
  -- MERGED list (acc.spots), not this chunk's scratch — same contract as before
  -- the scratch tables were introduced.
  if chunked then
    return nil, acc.spots, nil
  end

  -- Single-shot mode: run the two-pass selection now (writes best_score,
  -- best_spot back into acc) and return the legacy triple.
  local f_score, f_spots, f_best = M.finalize_pill_eval(acc, tmx, tmy)
  if f_best == nil and C.ATTACK_PILL_WALL_FALLBACK and not walls
     and not _parity_lua_only then
    -- No spot with a wall-free line: run the wall pass over the whole ring.
    local wacc = M.new_pill_eval_acc(detailed)
    wacc.walls = true
    wacc._single = true
    return M.evaluate_pill_difficulty(pill, world, detailed, scan_step, phase, state,
                                      tmx, tmy, nil, nil, wacc)
  end
  return f_score, f_spots, f_best
end

-- =========================================================================
-- evaluate_tank_standoff — find best engagement position around enemy tank.
--
-- Walks the Manhattan-distance==R boundary around the enemy tank
-- (R = TANK_COMBAT_STANDOFF_RANGE), filters tiles that are impassable
-- or wall-blocked LOS, and picks the cheapest reachable one from the
-- tank-rooted Dijkstra slate (KIND_NORMAL).  Mirror of the kill_lgm
-- engage-spot picker in goals.refresh_kill_lgm — same pattern,
-- different range and target.
--
-- Falls back to a geometric point on the direct line enemy→tank at
-- distance R when the Dijkstra slate hasn't reached any boundary tile
-- (cold start / unreachable).
--
-- See M.evaluate_tank_standoff_ring8 for the prior 8-position ring
-- approach (preserved for reference, currently unused).
--
-- Returns: best_mx, best_my, best_score, path_cost, shells_on_arrival,
--          scan_spots, best_deg, or nil if no valid standoff position.
-- =========================================================================
function M.evaluate_tank_standoff(et, tmx, tmy, info, world, state)
  local R = C.TANK_COMBAT_STANDOFF_RANGE
  local boat = (info.inboat and 1) or 0
  local best_cost = math.huge
  local best_mx, best_my = nil, nil
  local scan_spots = {}

  -- Walk Manhattan boundary (|dx| + |dy| == R) around the enemy tank.
  -- ~4*R tiles total (28 for R=7) — cheap.
  for dx = -R, R do
    local dy_abs = R - math.abs(dx)
    local _ys = (dy_abs == 0) and { 0 } or { dy_abs, -dy_abs }
    for _, dy in ipairs(_ys) do
      local mx = et.mx + dx
      local my = et.my + dy
      if U.in_map(mx, my) then
        local tt = U.ttype(mx, my)
        local passable = (C.TERRAIN_COST_LAND[tt] or 9999) < 9999
                         and not U.is_water(tt)
        if passable then
          -- Need clear LOS to the enemy tank from this position — a
          -- walled-off engage spot is worthless.
          local wall_hp = PF.wall_hp_between(mx, my, et.mx, et.my)
          if wall_hp == 0 then
            local c = cpf.smart_cost_dij_only(cpf.KIND_NORMAL, mx, my, boat)
            scan_spots[#scan_spots + 1] = {
              cx = mx + 0.5, cy = my + 0.5, mx = mx, my = my,
              has_los = true, total_score = c or 99999,
            }
            if c and c < best_cost then
              best_cost = c
              best_mx, best_my = mx, my
            end
          else
            scan_spots[#scan_spots + 1] = {
              cx = mx + 0.5, cy = my + 0.5, mx = mx, my = my,
              has_los = false, total_score = 999, reason = "wall_blocked",
            }
          end
        else
          scan_spots[#scan_spots + 1] = {
            cx = mx + 0.5, cy = my + 0.5, mx = mx, my = my,
            has_los = false, total_score = 999, reason = "impassable",
          }
        end
      end
    end
  end

  -- Geometric fallback: Dijkstra slate hasn't reached any boundary
  -- tile yet (cold start / unreachable).  Pick the point on the
  -- direct line enemy→tank at distance R so we still have a sensible
  -- engage target.
  if not best_mx then
    local vdx = tmx - et.mx
    local vdy = tmy - et.my
    local vlen = math.sqrt(vdx * vdx + vdy * vdy)
    if vlen > 0.5 then
      best_mx = math.floor(et.mx + (vdx / vlen) * R + 0.5)
      best_my = math.floor(et.my + (vdy / vlen) * R + 0.5)
    else
      best_mx, best_my = tmx, tmy
    end
    if best_mx < 0   then best_mx = 0   end
    if best_mx > 255 then best_mx = 255 end
    if best_my < 0   then best_my = 0   end
    if best_my > 255 then best_my = 255 end
    if not U.in_map(best_mx, best_my) then
      return nil
    end
    best_cost = cpf.estimate_cost(tmx, tmy, best_mx, best_my, boat) * 0.1
  end

  local best_deg = math.deg(math.atan(best_mx - et.mx, -(best_my - et.my))) % 360
  local shells_on_arrival = cpf.dijkstra_shells_at(cpf.KIND_NORMAL, best_mx, best_my)
                         or cpf.astar_shells_at(best_mx, best_my)


  return best_mx, best_my, best_cost, best_cost, shells_on_arrival, scan_spots, best_deg
end

-- =========================================================================
-- evaluate_tank_standoff_ring8 — LEGACY 8-position ring scoring (preserved
-- for reference; not called from live code).
--
-- Samples 8 candidate tiles at TANK_COMBAT_STANDOFF_RANGE around the
-- target (every 45°), scores each by terrain + maneuver-ellipse danger +
-- crossfire + wall LOS, and returns the best one + its A* cost.  Same
-- structure as evaluate_pill_difficulty.
--
-- We swapped to a Manhattan-boundary-scan version (see
-- M.evaluate_tank_standoff below) that mirrors the kill_lgm engage-spot
-- picker: walks every tile on the engage-range boundary and picks the
-- one with the lowest Dijkstra cost.  Boundary scan trades the per-spot
-- ellipse/crossfire scoring for far more position candidates (~28 vs 8)
-- and shares the slate the steering layer already uses, so the chosen
-- standoff is reachable by definition rather than being "best ring spot
-- but maybe walled off."
--
-- Kept around in case we want to revisit the per-spot maneuver/crossfire
-- scoring.  Safe to delete once the boundary-scan version has been in
-- use for a while.
--
-- Returns: best_mx, best_my, best_score, path_cost, shells_on_arrival,
--          scan_spots, best_deg, or nil if no valid standoff position.
-- =========================================================================
function M.evaluate_tank_standoff_ring8(et, tmx, tmy, info, world, state)
  local R = C.TANK_COMBAT_STANDOFF_RANGE
  local safe_r = C.ATTACK_SAFE_RADIUS
  local stamps = ELLIPSE_STAMPS_45DEG
  local best_score = math.huge
  local best_mx, best_my = nil, nil
  local best_deg = 0
  local scan_spots = {}
  -- Hoist viz toggle: this function does both scoring (logic) and
  -- per-candidate overlay drawing. Score loop runs always; viz blocks
  -- gate on this so arg evaluation is skipped when overlay is off.
  local v_scan = BRAIN_DEBUG_MODE and viz.is_on("tank_combat_standoff_scan")

  for deg = 0, 315, 45 do
    local rad = math.rad(deg)
    local cx = et.mx + 0.5 + math.sin(rad) * R
    local cy = et.my + 0.5 - math.cos(rad) * R
    local mx = math.floor(cx)
    local my = math.floor(cy)
    if not U.in_map(mx, my) then goto next_tank_spot end

    local tt = U.ttype(mx, my)
    local passable = (C.TERRAIN_COST_LAND[tt] or 9999) < 9999 and not U.is_water(tt)
    if not passable then
      scan_spots[#scan_spots + 1] = {
        cx = cx, cy = cy, mx = mx, my = my, deg = deg,
        has_los = false, total_score = 999, reason = "impassable",
        score_danger = 0, score_crossfire = 0, score_terrain = 0, score_approach = 0,
      }
      goto next_tank_spot
    end

    -- Wall check: need clear shot to the enemy tank from this position
    local wall_hp = PF.wall_hp_between(mx, my, et.mx, et.my)
    if wall_hp > 0 then
      scan_spots[#scan_spots + 1] = {
        cx = cx, cy = cy, mx = mx, my = my, deg = deg,
        has_los = false, total_score = 999, reason = "wall_blocked",
        score_danger = 0, score_crossfire = 0, score_terrain = 0, score_approach = 0,
      }
      goto next_tank_spot
    end

    -- ── Maneuver ellipse scan ──
    -- Oriented toward the enemy tank, same geometry as pill attack.
    local total_danger = 0
    local safe_tiles = 0
    local terrain_penalty = 0
    local max_coverage = 0
    local maneuver_tiles = {}

    local stamp = stamps and stamps[deg] or nil
    local function process_tile(sx, sy)
      if not U.in_map(sx, sy) then return end
      local d = threat.pill_at(sx, sy)
      total_danger = total_danger + d
      safe_tiles = safe_tiles + 1
      maneuver_tiles[#maneuver_tiles + 1] = { x = sx, y = sy, val = math.floor(d + 0.5) }
      local stt = U.ttype(sx, sy)
      local base_entry = world.base_at[sy * 256 + sx]
      local enemy_base = base_entry and base_entry.base
                         and base_entry.base.owner == "hostile"
      if stt == C.T_DEEPSEA or enemy_base then
        terrain_penalty = terrain_penalty + 1000
      elseif stt == C.T_BUILDING or stt == C.T_HALFBUILD
         or stt == C.T_SWAMP or stt == C.T_RIVER then
        terrain_penalty = terrain_penalty + 100
      end
      local cov = threat.coverage_at(sx, sy)
      if cov > max_coverage then max_coverage = cov end
    end

    if stamp then
      for i = 1, #stamp do
        local off = stamp[i]
        process_tile(mx + off.dx, my + off.dy)
      end
    end

    local score_danger = safe_tiles > 0 and (total_danger / safe_tiles) or 999
    local score_crossfire = max_coverage > 0 and (100 * max_coverage) or 0
    -- Approach cost: lightweight distance tiebreaker only. The A* path_cost
    -- to the winning standoff already captures the real travel cost — the
    -- standoff score should just pick the better LOCAL position, not
    -- double-count travel via the full Dijkstra cost.
    local score_approach = U.mdist(tmx, tmy, mx, my) * 2
    local score = score_danger + score_crossfire + terrain_penalty + score_approach

    scan_spots[#scan_spots + 1] = {
      cx = cx, cy = cy, mx = mx, my = my, deg = deg,
      has_los = true, total_score = score,
      score_danger = score_danger, score_crossfire = score_crossfire,
      score_terrain = terrain_penalty, score_approach = score_approach,
      maneuver_tiles = maneuver_tiles,
    }


    -- Overlay: color-coded candidate positions
    if v_scan and score >= best_score then
      local rel = math.min(1.0, score / math.max(1, best_score * 3))
      local cr = math.floor(255 * rel)
      local cg = math.floor(255 * (1 - rel))
    end

    if score < best_score then
      best_score = score
      best_mx = mx
      best_my = my
      best_deg = deg
    end

    ::next_tank_spot::
  end

  if not best_mx then
    return nil
  end

  if v_scan then
    -- Overlay: mark chosen standoff with green circle + line to enemy

    -- Draw ellipse outline on winner
    local best_spot = nil
    for _, s in ipairs(scan_spots) do
      if s.deg == best_deg and s.has_los then best_spot = s; break end
    end
    if best_spot then
      local edx = et.mx + 0.5 - best_spot.cx
      local edy = et.my + 0.5 - best_spot.cy
      local elen = math.sqrt(edx * edx + edy * edy)
      if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
      local ux, uy = edx / elen, edy / elen
      local vx, vy = -uy, ux
      -- Same r_long source as the scoring code (was hardcoded to 4
      -- and drifted from ATTACK_SAFE_RADIUS — drawn shape now matches).
      local rl, rs = safe_r, safe_r / 3.0
      local segs = 24
      local px, py
      for i = 0, segs do
        local a = (i / segs) * 2 * math.pi
        local eu = math.cos(a) * rl
        local ev = math.sin(a) * rs
        local nx = best_spot.cx + eu * ux + ev * vx
        local ny = best_spot.cy + eu * uy + ev * vy
        if px then
        end
        px, py = nx, ny
      end
    end

    -- Score labels on each candidate
    for _, s in ipairs(scan_spots) do
      if s.has_los and s.total_score < 900 then
        local label = string.format("D%.0f+X%.0f+T%.0f+A%.0f=%.0f",
          s.score_danger, s.score_crossfire, s.score_terrain, s.score_approach,
          s.total_score)
      elseif not s.has_los then
      end
    end
  end

  -- Path cost to the best standoff position. Use KIND_NORMAL then subtract
  -- 90% of danger along the traced path — equivalent to KIND_PILL's 0.1
  -- danger scale but without a dedicated slate and without set_config global
  -- state mutation. Falls back to a scaled estimate if trace returns nil.
  local path_cost
  do
    local raw = cpf.dijkstra_lookup_by_kind(cpf.KIND_NORMAL, best_mx, best_my, 0)
    if raw < 1e29 then
      local danger_adj = 0
      local path = cpf.dijkstra_trace_path_by_kind(cpf.KIND_NORMAL, best_mx, best_my)
      if path then
        for i = 1, #path, 2 do
          local px, py = path[i], path[i+1]
          local tt = U.ttype(px, py)
          local spd = C.TERRAIN_SPEED and C.TERRAIN_SPEED[tt] or 16
          if spd <= 0 then spd = 16 end
          danger_adj = danger_adj + 0.9 * threat.at(px, py) * (16 / spd)
        end
      end
      path_cost = math.max(0, raw - danger_adj)
    else
      path_cost = cpf.estimate_cost(tmx, tmy, best_mx, best_my, 0) * 0.1
    end
  end

  local shells_on_arrival = cpf.dijkstra_shells_at(KIND_NORMAL, best_mx, best_my)
                         or cpf.astar_shells_at(best_mx, best_my)


  return best_mx, best_my, best_score, path_cost, shells_on_arrival, scan_spots, best_deg
end

-- position:   navigating to standoff, braking to stop
-- aim:        stopped at standoff, turning to face pill
-- engage:     firing at pill, monitoring armour
-- curve_away: evasive turn after taking hits
-- rush:       pill dead, drive to capture
-- disengage:  armour critical, flee

-- Walk shot_tracker.shots and update goal._on_target_fired (count of shots
-- fired during this attack whose crosshair-at-fire was on the target pill)
-- and goal._on_target_misses (shots that died without landing on the pill).
-- For each newly-detected miss, bump goal._bullets_needed by one so the
-- charge/engage swerve trigger waits for a replacement shot.
local function update_shot_accounting(goal, world)
  if not goal._attack_start_tick then
    goal._on_target_fired  = 0
    goal._on_target_misses = 0
    return
  end
  local pmx, pmy = goal.mx, goal.my
  local pwx = (pmx + 0.5) * 256
  local pwy = (pmy + 0.5) * 256
  local hit_tol_wu = 384        -- death within 1.5 tiles counts as a hit
  local hit2 = hit_tol_wu * hit_tol_wu
  local fired, on_target_fired, on_target_in_flight, misses = 0, 0, 0, 0
  for _, s in ipairs(shot_tracker.shots) do
    if s.fire_tick >= goal._attack_start_tick then
      -- Total shots fired during this attack (any direction).
      fired = fired + 1

      -- A shot is "on target" if its actual simulated trajectory
      -- (cpf.simulate_shot, same physics as the engine) crosses the
      -- pill tile. Stricter than the prior ray-perpendicular check —
      -- a near-miss that fires from where the trajectory would only
      -- clip the corner of an adjacent tile no longer counts.
      local sm = math.sqrt(s.step_x * s.step_x + s.step_y * s.step_y)
      local on_target = false
      if sm > 0 then
        -- Project a target point far down the firing direction so
        -- simulate_shot traces the full trajectory's range. Round to
        -- integers — cpf_simulate_shot's binding uses luaL_checkinteger
        -- so floats from the (far/sm) scale crash the brain.
        local far = 14 * 256
        local tx_w = math.floor(s.fire_fx + s.step_x * (far / sm) + 0.5)
        local ty_w = math.floor(s.fire_fy + s.step_y * (far / sm) + 0.5)
        local path = cpf.simulate_shot(s.fire_fx, s.fire_fy, tx_w, ty_w,
                                       cpf.SHOT_TANK, 14)
        if path then
          local o_mx = math.floor(s.fire_fx / 256)
          local o_my = math.floor(s.fire_fy / 256)
          for _, t in ipairs(path) do
            if t.mx == pmx and t.my == pmy then
              on_target = true
              break
            end
            -- A tree, wall, or another live pillbox between the muzzle and
            -- the pill stops the REAL shell short, so this shot will not
            -- reach the pill even though the geometric ray crosses its tile.
            -- Don't count such a shell as on-target/in-flight — otherwise the
            -- "in-flight shells already cover the HP -> swerve" kill gate
            -- fires on shots that are actually eaten by an obstacle, leaving
            -- the pill alive. (Skip the muzzle's own tile.)
            if t.mx ~= o_mx or t.my ~= o_my then
              local tt = U.ttype(t.mx, t.my)
              if tt == C.T_FOREST or tt == C.T_BUILDING or tt == C.T_HALFBUILD then
                break
              end
              local plist = world.pill_at and world.pill_at[t.my * 256 + t.mx]
              if plist then
                local hit_other = false
                for _, e in ipairs(plist) do
                  if e.pill and (e.pill.health or 0) > 0 then hit_other = true break end
                end
                if hit_other then break end
              end
            end
          end
        end
      end
      if on_target then
        on_target_fired = on_target_fired + 1
        if s.status == "in_flight" then
          on_target_in_flight = on_target_in_flight + 1
        elseif s.status == "dead" then
          local ddx = s.dead_fx - pwx
          local ddy = s.dead_fy - pwy
          local missed = (ddx * ddx + ddy * ddy) > hit2
          if missed then
            misses = misses + 1
            if not s._attack_handled then
              -- First detection of this miss — bump the kill budget.
              goal._bullets_needed = (goal._bullets_needed or 0) + 1
            end
          end
          s._attack_handled = true
        end
      end
    end
  end
  goal._fired               = fired
  goal._on_target_fired     = on_target_fired
  goal._on_target_in_flight = on_target_in_flight
  goal._on_target_misses    = misses
end

-- Commit a soldier's blitz take from the NEGOTIATED engage spot. The squad
-- negotiation (blitz_negotiate, via M.pick_standoff) already chose this spot,
-- broadcast it as bes, and the commander approved it (no clash, shot not
-- wall-blocked) — so there is no eval-cache re-scan here. We set the standoff
-- and head to its SETUP point (offset back from the standoff, away from the
-- pill) via the shared `approach` substate, exactly like a regular non-PPT
-- take — NOT the standoff tile itself. Returns true on commit, false if no
-- spot could be resolved (caller falls back to a normal plan_position take).
local function blitz_commit_negotiated(goal, state, world, info, pmx, pmy)
  local smx, smy = state.squad_blitz_engage_mx, state.squad_blitz_engage_my
  -- No negotiated engage spot (the ellipse scan produced none) → DO NOT commit a
  -- geometric pick_standoff fallback. That fallback ignores neutral/hostile pills
  -- and lands on garbage spots (saw a soldier rejoin onto a tile wedged between
  -- two neutral pills). Bail instead so the caller runs a normal plan_position
  -- take, which scans for a real, screened spot.
  if not smx then return false end
  local cx, cy = smx + 0.5, smy + 0.5
  -- C.BLITZ_SPOT_EXACT_ORIGIN (fix A, 2026-09-24): commit the scan's validated
  -- FLOAT point (the point the shell test and the margin ran from), not the tile
  -- centre, and its angle, so ban_current_pill_angle can ban it after a blocked
  -- replan. KEEL committed the tile centre and left _chosen_deg nil.
  local efx, efy = state.squad_blitz_engage_fx, state.squad_blitz_engage_fy
  if C.BLITZ_SPOT_EXACT_ORIGIN and efx and efy
     and math.floor(efx) == smx and math.floor(efy) == smy then
    cx, cy = efx, efy
    goal._chosen_deg = state.squad_blitz_engage_deg
  end
  goal.standoff_fx, goal.standoff_fy = cx, cy
  goal.standoff_mx, goal.standoff_my = smx, smy
  -- Aim at the wall-clear point chosen during negotiation (center or a corner that
  -- dodges the commander's shield walls); fall back to pill center when none set.
  goal.aim_mx = state.squad_blitz_aim_fx or (pmx + 0.5)
  goal.aim_my = state.squad_blitz_aim_fy or (pmy + 0.5)
  -- Same point in world units — that is the copy steering, the GO gate and the
  -- standoff sanity check read (aim_mx/aim_my get rewritten every tick by
  -- init.lua's near-edge fallback on a non-PPT take).
  goal.aim_wx = math.floor(goal.aim_mx * 256 + 0.5)
  goal.aim_wy = math.floor(goal.aim_my * 256 + 0.5)
  -- Setup point: identical to every other pill-take mode (see plan_position) —
  -- the SETUP sits at the standoff RADIUS + ATTACK_APPROACH_OFFSET from the pill,
  -- along this spot's direction. We use the ideal radius (ATTACK_PILL_STANDOFF),
  -- NOT the picked tile's distance, so the soldier's setup matches the
  -- commander's (whose standoff is the precise float arc at that radius); a
  -- tile that rounds in closer must not pull the hold point into firing range.
  local dx, dy = cx - (pmx + 0.5), cy - (pmy + 0.5)
  local dlen = math.sqrt(dx * dx + dy * dy)
  if dlen > 0.01 then
    local ux, uy = dx / dlen, dy / dlen
    local setup_dist = (C.ATTACK_PILL_STANDOFF or 7.4) + (C.ATTACK_APPROACH_OFFSET or 2.25)
    goal.approach_fx = (pmx + 0.5) + ux * setup_dist
    goal.approach_fy = (pmy + 0.5) + uy * setup_dist
    goal.approach_mx = U.mclamp(math.floor(goal.approach_fx))
    goal.approach_my = U.mclamp(math.floor(goal.approach_fy))
  else
    goal.approach_fx, goal.approach_fy = cx, cy
    goal.approach_mx, goal.approach_my = smx, smy
  end
  goal._is_ppt = false
  state.squad_blitz_engage_mx, state.squad_blitz_engage_my = smx, smy
  goal._approach_start = nil
  goal.substate = "approach"
  return true
end

-- Parallel blitz-soldier standoff OFFER (pre-commit, runs while the soldier is
-- on its normal goal). For a soldier negotiating with a commander (squad layer
-- set squad_negotiate_cmdr) but not yet committed: offer our closest valid
-- standoff for that commander's pill (pick_standoff skips state._blitz_reject)
-- + reported walk distance. On a reject (brj → _blitz_call_rejected) exclude the
-- spot and offer the next-closest (repos++). Clears offer state when not
-- negotiating. Does NOT touch the soldier's goal — that switches only on accept.
-- Pick the offered blitz standoff from a completed ellipse-scan: the best-scoring
-- (lowest total_score) spot with LOS that isn't blitz-rejected (de-confliction).
-- Returns nil if the scan has no usable spot (caller falls back to pick_standoff).
-- Pick the soldier's offered blitz standoff from the scored ellipse spots.
-- NOT the single globally-best-scored spot — that can sit on the far side of the
-- pill (16 tiles away, even next to the commander) when a near spot scores almost
-- as well. Instead: bucket scores into fixed bands (BLITZ_STANDOFF_SCORE_BUCKET
-- wide, e.g. [0,50),[50,100),…), take the band the best spot falls in as the
-- "best pool", and within that pool pick the spot CLOSEST to our tank — same
-- effective shield quality, least travel. tmx,tmy = our tank tile.
-- Aim points within a pill tile (world units, tile = 256 wu): center + 4 corners
-- inset 1 gu (so a corner shot doesn't clip the pixel edge). Same set the LOS
-- shield scan uses. Tried center-first so a clear center keeps the simplest aim.
local _AIM_OFFS = { {128,128}, {1,1}, {254,1}, {1,254}, {254,254} }

-- First aim point whose shell path from spot float (sfx,sfy) REACHES pill
-- (pmx,pmy) crossing no tile in `wallset` (packed my*256+mx — a commander's
-- broadcast bwl shield walls) and no world wall / base / live pill. Returns the
-- aim point as float tile coords (aim_fx, aim_fy), or nil if every aim is blocked.
local function blitz_clear_aim(sfx, sfy, pmx, pmy, wallset, world)
  local ox = math.floor(sfx * 256 + 0.5)
  local oy = math.floor(sfy * 256 + 0.5)
  for _, a in ipairs(_AIM_OFFS) do
    local tiles = cpf.simulate_shot(ox, oy, bit.bor((bit.lshift(pmx, 8)), a[1]), bit.bor((bit.lshift(pmy, 8)), a[2]), cpf.SHOT_TANK, 0)
    if tiles then
      local blocked, reached = false, false
      for _, t in ipairs(tiles) do
        if t.mx == pmx and t.my == pmy then reached = true; break end
        if wallset and wallset[t.my * 256 + t.mx] then blocked = true; break end
        local tt = U.ttype(t.mx, t.my)
        if tt == C.T_BUILDING or tt == C.T_HALFBUILD then blocked = true; break end
        local be = world and world.base_at and world.base_at[t.my * 256 + t.mx]
        if be and be.base then blocked = true; break end
        if SM.pending_pill_at(world, t.mx, t.my) then blocked = true; break end
        local plist = world and world.pill_at and world.pill_at[t.my * 256 + t.mx]
        if plist then for _, e in ipairs(plist) do local pp = e.id and world.pills[e.id]; if pp and not pp.in_tank and (pp.health or 0) > 0 and pp.mx == t.mx and pp.my == t.my then blocked = true; break end end end
        if blocked then break end
      end
      if reached and not blocked then return pmx + a[1] / 256, pmy + a[2] / 256 end
    end
  end
  return nil
end

-- When `wallset` (commander's bwl shield walls) is non-empty, a spot is eligible
-- only if it has a wall-CLEAR aim point (center or a corner) — mandatory, per the
-- "never fire through a friendly wall" rule. Returns the chosen spot AND its aim
-- point (afx,afy float, nil when no walls → take aims center as before). Returns
-- nil when no eligible spot exists (caller bails rather than picking a blocked line).
--
-- Two knobs (2026-09-24):
--   C.BLITZ_SPOT_EXACT_ORIGIN -- each spot is tested from the scan's FLOAT point
--     (s.cx,s.cy), the point the scan's shell test ran from, and the pick
--     returns that point and its angle so the commit / bes / arbiter / GO gate
--     all use it. false (KEEL) = tile centre, as before.
--   C.BLITZ_SPOT_LOS_MARGIN > 0 -- a spot is eligible only if one of its five aim
--     points passes the shell test AND the tapering line margin
--     (spot_margin.lua). The margin runs first (pure maths); a shell simulation
--     is paid only for an aim that passed it, and not at all for the scan's own
--     aim when we test from the scan's own point. 0 (KEEL) = no margin.
-- Returns smx, smy, afx, afy, sfx, sfy, deg -- or nil, reason. reason "margin"
-- means spots existed but the margin rejected them all (the caller must not
-- fall back to pick_standoff, which knows nothing about the margin).
local function blitz_pick_from_scan(spots, state, tmx, tmy, wallset, pmx, pmy, world)
  if not spots then return nil end
  local rej = state._blitz_reject
  local need_clear = wallset and next(wallset) ~= nil
  local margin = C.BLITZ_SPOT_LOS_MARGIN or 0
  local exact  = C.BLITZ_SPOT_EXACT_ORIGIN
  local mctx   = (margin > 0) and SM.new_ctx(world, pmx, pmy) or nil
  local n_mrej = 0
  -- Eligible spots (LOS, not rejected, and — when walling — a wall-clear aim).
  local elig = {}
  for _, s in ipairs(spots) do
    if s.has_los and s.mx and not (rej and rej[U.mkey(s.mx, s.my)]) then
      local sfx, sfy = s.mx + 0.5, s.my + 0.5
      if exact and s.cx and s.cy then sfx, sfy = s.cx, s.cy end
      local afx, afy
      local ok = true
      if margin > 0 then
        -- The scan shell-tested s.aim_idx from (s.cx,s.cy) with no shield walls:
        -- trust it only when we test from that same point without walls.
        local trusted = (exact and s.cx and not need_clear) and s.aim_idx or nil
        local idx, awx, awy = SM.clear_aim_margin(
          math.floor(sfx * 256 + 0.5), math.floor(sfy * 256 + 0.5),
          pmx, pmy, world, margin,
          { ctx = mctx, prefer_idx = s.aim_idx, trusted_idx = trusted,
            trusted_trees = s.aim_trees, wallset = need_clear and wallset or nil,
            site = "pick", tick = state.tick })
        if idx then afx, afy = awx / 256.0, awy / 256.0
        else ok = false; n_mrej = n_mrej + 1 end
      elseif need_clear then
        afx, afy = blitz_clear_aim(sfx, sfy, pmx, pmy, wallset, world)
        ok = afx ~= nil
      elseif s.aim_wx and s.aim_wy then
        -- No commander walls to dodge, so take the aim the spot scan already
        -- proved is shootable from here instead of defaulting to the centre.
        afx, afy = s.aim_wx / 256.0, s.aim_wy / 256.0
      end
      if ok then elig[#elig + 1] = { s = s, afx = afx, afy = afy, sfx = sfx, sfy = sfy } end
    end
  end
  if #elig == 0 then
    if n_mrej > 0 then
      return nil, "margin"
    end
    return nil
  end
  -- Pass 1: best (lowest) score among eligible. Pass 2: closest within the band.
  local best_s
  for _, e in ipairs(elig) do local sc = e.s.total_score or 1e9; if not best_s or sc < best_s then best_s = sc end end
  local band = C.BLITZ_STANDOFF_SCORE_BUCKET or 50
  local lo = math.floor(best_s / band) * band
  local hi = lo + band
  local chosen, best_d
  for _, e in ipairs(elig) do
    local sc = e.s.total_score or 1e9
    if sc >= lo and sc < hi then
      local d = (tmx and tmy) and U.mdist(tmx, tmy, e.s.mx, e.s.my) or 0
      if not best_d or d < best_d then best_d = d; chosen = e end
    end
  end
  if not chosen then return nil end
  return chosen.s.mx, chosen.s.my, chosen.afx, chosen.afy, chosen.sfx, chosen.sfy, chosen.s.deg
end

M._blitz_pick_from_scan = blitz_pick_from_scan   -- unit tests

function M.blitz_negotiate(state, world, info, now)
  local cmdr = state.squad_negotiate_cmdr
  if not cmdr or state.squad_blitz_accepted then
    if not state.squad_blitz_accepted then
      state.squad_blitz_engage_mx, state.squad_blitz_engage_my = nil, nil
      state.squad_blitz_engage_fx, state.squad_blitz_engage_fy = nil, nil
      state.squad_blitz_engage_deg = nil
      state.squad_blitz_aim_fx, state.squad_blitz_aim_fy = nil, nil
      state.squad_blitz_bd = nil
      state._blitz_reject = nil
      state.squad_blitz_repos = nil
    else
      -- Committed (accepted): keep bd LIVE — cost from our MOVING position to the
      -- committed standoff — so the commander's progress-based blitz_wait timeout
      -- can see us closing when its own perception can't reach us. The commander
      -- measures visible soldiers itself every tick (free), so this broadcast is
      -- only a coarse FALLBACK for when we're out of its sight: refresh slowly
      -- (≤ REFRESH_TICKS) and only re-store on a ≥1-tile change to avoid /info
      -- spam. EXCEPTION: if the commander is directly asking where we are (bwq,
      -- sent ~1s before it would give up), answer immediately this tick.
      local goal = state.goal
      if goal and goal.kind == "attack_pill" and goal.standoff_mx
         and not state.squad_blitz_aimed then
        local force = false
        local cpn = state.squad_blitz_accepted
        if cpn then
          local q = ally_state.get_key(cpn, "bwq")
          if q ~= "" and tonumber(q) == goal.target_id then
            if state._blitz_query_ack ~= q then state._blitz_query_ack = q; force = true end
          else
            state._blitz_query_ack = nil
          end
        end
        if force or (now - (state._blitz_bd_refresh_tick or -100000)) >= (C.SQUAD_BLITZ_BD_REFRESH_TICKS or 500) then
          state._blitz_bd_refresh_tick = now
          local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
          local nbd = math.floor((cpf.estimate_cost(tmx, tmy, goal.standoff_mx, goal.standoff_my, 0) or 0) + 0.5)
          if force or not state.squad_blitz_bd or math.abs(nbd - state.squad_blitz_bd) >= 1 then
            state.squad_blitz_bd = nbd
          end
        end
      end
    end
    state._blitz_negotiate_scan = nil  -- not negotiating → drop the scan viz
    state._blitz_offer_pill = nil
    return
  end
  local pid  = state.squad_negotiate_pill
  local pill = pid and world and world.pills and world.pills[pid] or nil
  if not pill then return end
  -- Hold a STABLE offer to avoid flooding the /info bus. The offered standoff
  -- (bes) and walk distance (bd) ride the event-driven state slate, which
  -- re-broadcasts whenever it changes — and bd = cost(our MOVING position ->
  -- standoff) drifts every tick, so recomputing the offer each tick would
  -- re-broadcast continuously. Only (re)compute when we have no offer yet for
  -- this pill, the pill changed, or the commander rejected our spot (repick).
  -- Also bust the hold when the commander's shield walls (bwl) change since our
  -- offer — a spot picked before the walls appeared may now shoot through one, so
  -- re-pick a wall-clear spot+aim.
  if state.squad_blitz_engage_mx ~= nil
     and state._blitz_offer_pill == pid
     and state._blitz_offer_bwl == (ally_state.get_key(cmdr, "bwl") or "")
     and not state._blitz_call_rejected then
    return
  end
  -- Recompute the FULL standoff scoring (the ellipse scan) for this pill so the
  -- soldier offers the best BUCKET spot — same scoring the commander/PPT uses —
  -- not just the nearest orbit tile. Drive the chunked detailed scan (shared
  -- cache) and pick from its scored spots; stash them for the one-frame viz
  -- (blitz_negotiate_scan). Fall back to pick_standoff until the scan completes.
  local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
  M.advance_pill_eval_chunk(state, world, info, tmx, tmy, pid, pill)
  local cached = state._pill_eval_cache and state._pill_eval_cache[pid]
  -- Commander's shield walls (broadcast bwl only while it's in build_walls): route
  -- our engage spot + aim point AROUND them so our shell never strikes a friendly wall.
  local wallset
  local bwl = cmdr and ally_state.get_key(cmdr, "bwl")
  if bwl and bwl ~= "" then
    wallset = {}
    for s in string.gmatch(bwl, "%d+") do wallset[tonumber(s)] = true end
  end
  local walls_active = wallset and next(wallset) ~= nil
  local smx, smy, aim_fx, aim_fy, spot_fx, spot_fy, spot_deg, nospot_why
  local scan_ready = cached and cached.spots
  if scan_ready then
    smx, smy, aim_fx, aim_fy, spot_fx, spot_fy, spot_deg = blitz_pick_from_scan(cached.spots, state, tmx, tmy, wallset, pill.mx, pill.my, world)
    if not smx then nospot_why = smy end   -- "margin" when the LOS margin rejected every spot
  end
  -- Fall back to pick_standoff only when the scan isn't ready yet OR there are no
  -- shield walls to dodge. If the commander IS walling and a READY scan found no
  -- wall-clear spot, do NOT fall back (pick_standoff ignores the walls) — bail via
  -- the no-spot path so we never commit a spot whose only shot crosses a friendly wall.
  -- Same for the LOS margin (C.BLITZ_SPOT_LOS_MARGIN): pick_standoff knows
  -- nothing about it, so a ready scan whose spots all failed the margin bails.
  if not smx and not (scan_ready and (walls_active or nospot_why == "margin")) then
    smx, smy = M.pick_standoff(world, info, pill, state)
    spot_fx, spot_fy, spot_deg = nil, nil, nil   -- tile centre, no scan angle
  end
  if not smx then
    -- No appropriate standoff spot for this blitz pill: reject it for ~30s so the
    -- join discount / join-scan stop re-picking it, drop the negotiation, and
    -- stamp a comm-line "no-spot" decline so the (often one-tick) reject shows.
    state.squad_blitz_engage_mx = nil
    state.squad_blitz_engage_fx, state.squad_blitz_engage_fy = nil, nil
    state.squad_blitz_engage_deg = nil
    state._blitz_pill_reject = state._blitz_pill_reject or {}
    state._blitz_pill_reject[pid] = now + (C.SQUAD_BLITZ_NOSPOT_REJECT_TICKS or 1500)
    state._blitz_comm_reject = { tick = now, cmdr = cmdr, reason = "nospot" }
    state.squad_negotiate_cmdr = nil
    state.squad_negotiate_pill = nil
    state.squad_blitz_bd = nil
    return
  end
  state.squad_blitz_engage_mx, state.squad_blitz_engage_my = smx, smy
  -- Fix A (C.BLITZ_SPOT_EXACT_ORIGIN): keep the scan's validated float point and
  -- angle. bes, the arbiter, the commit and the GO gate read them. nil (KEEL, or
  -- a pick_standoff fallback) = the tile centre as before.
  if C.BLITZ_SPOT_EXACT_ORIGIN then
    state.squad_blitz_engage_fx, state.squad_blitz_engage_fy = spot_fx, spot_fy
    state.squad_blitz_engage_deg = spot_deg
  else
    state.squad_blitz_engage_fx, state.squad_blitz_engage_fy = nil, nil
    state.squad_blitz_engage_deg = nil
  end
  state.squad_blitz_aim_fx, state.squad_blitz_aim_fy = aim_fx, aim_fy   -- wall-clear aim (nil = aim pill center)
  state._blitz_offer_pill = pid   -- mark the offer as made for this pill (hold it)
  state._blitz_offer_bwl = bwl or ""   -- commander's wall set we validated against (busts hold on change)
  state.squad_blitz_bd = math.floor((cpf.estimate_cost(tmx, tmy, smx, smy, 0) or 0) + 0.5)
  if state._blitz_call_rejected
     and (now - (state._blitz_repick_tick or -100000)) >= (C.SQUAD_BLITZ_REPICK_GAP or 30) then
    state._blitz_reject = state._blitz_reject or {}
    state._blitz_reject[U.mkey(smx, smy)] = true
    state._blitz_repick_tick = now
    state.squad_blitz_repos = (state.squad_blitz_repos or 0) + 1
  end
end

-- Tanks committed to the CURRENT blitz (self + partners). A SOLDIER is always
-- >= 2 (its commander + itself). A COMMANDER is 1 + its committed soldiers. A
-- non-blitz / solo take is 1. Used to relax solo-only caution (detree, low
-- armour) once an ally is actually committed to help on the take.
local function blitz_tank_count(goal, state, info)
  if not (goal and goal._blitz) then return 1 end
  -- Soldier: only a COMMITTED soldier (read the commander's bac → squad_blitz_accepted)
  -- counts as a real 2-tank blitz. While still negotiating (offered, awaiting bac)
  -- we're not a member yet — fall through so we don't claim 2-tank caution exemptions.
  if state.squad_role == "s" and state.squad_blitz_accepted then return 2 end
  local total = squad.blitz_ready_status(state, state.tick or 0, info.player_number or -1, info)
  return 1 + (total or 0)
end

-- CONTESTED TAKE (2026-09-05, C.BLITZ_CONTESTED_ALL_SUICIDERS): commander-only.
-- If a live hostile tank is sitting within C.BLITZ_CONTESTED_RANGE tiles of the
-- pill we are taking, the take is CONTESTED and C.BLITZ_CONTESTED_SUICIDERS (1)
-- of the party go in as temporary pill_suiciders, regardless of
-- BLITZ_MIN_SUICIDERS. squad.blitz_designate_contested picks them (soldiers
-- first, the commander only if they cannot cover it) and reports back a MODE:
--   "done"  any party of 2 or more. Terminal: nothing re-checks after it, so
--           a party that grows later designates nothing more.
--   "solo"  no party at all (a solo take). Designates nothing and latches
--           nothing, so a soldier that joins later still gets the rule.
-- Called at BOTH GO sites, and again on the commander's replans while the take
-- is live so an enemy that arrives AFTER GO still flips the party. The latch is
-- the mode itself on the goal (`_blitz_contested_mode`), so the designation
-- happens at most once per take and dies with the goal; expiry of the
-- designations themselves is unchanged.
local function blitz_contested_check(goal, state, world, info, now)
  if not C.BLITZ_CONTESTED_ALL_SUICIDERS then return end
  if goal._blitz_contested_mode then return end
  local pid = goal.target_id
  if not pid then return end
  local p = world and world.pills and world.pills[pid]
  local pmx = (p and p.mx) or goal.mx
  local pmy = (p and p.my) or goal.my
  if not (pmx and pmy) then return end
  local epn, edist = squad.blitz_contested_enemy(state, pmx, pmy)
  if not epn then return end
  local n, mode, party = squad.blitz_designate_contested(
    state, info, now, pid, epn, edist)
  -- Solo: no party to designate, and no latch either -- ask again next replan.
  if mode == "solo" then return end
  goal._blitz_contested_mode = mode
  -- Kept on the goal so every blitz panel / DECISION line that shows the quorum
  -- or the suicider count can show contested{...} beside it.
  goal._blitz_contested = { pn = epn, dist = edist, tick = now, mode = mode,
                            party = party, n = n }
end

-- ── "Blitz only when hit" (BLITZ_SWERVE_ONLY_WHEN_HIT) ────────────────────
-- (blitz_party_count is defined near mark_kill_pickup, which also uses it.)
-- True when a committed blitzer should HOLD its proactive swerves: the mode is
-- on, >= MIN blitzers are on this pill, we are still UNDAMAGED (armour has not
-- dropped below the value we committed with -- ANY hit ends the hold), and we
-- are not a pill_suicider (that path is strictly stronger and is checked first
-- at every gated site).  The party count is recomputed ONCE PER TICK (cached
-- across the several calls within a tick) -- cheap (one <=15-slot ally scan),
-- and only while committed + mode on. A 50-tick cache was too stale: a blitzer
-- that committed early would miss a late joiner and still see party<MIN when the
-- pill dies (e.g. bot2 committed t=20, bot1 t=475, pill died t=479 -> bot2's
-- 470-tick recount saw only 2 and peeled). Per-tick keeps every member current.
local function blitz_commit_hold(goal, state, info, now)
  if not (C.BLITZ_SWERVE_ONLY_WHEN_HIT and goal._blitz_committed) then return false end
  if goal._blitz_party_tick ~= now then
    goal._blitz_party      = blitz_party_count(state, now, info.player_number or -1, goal.target_id)
    goal._blitz_party_tick = now
  end
  return (goal._blitz_party or 0) >= (C.BLITZ_ONLY_WHEN_HIT_MIN or 3)
     and (info.armour or 0) >= (goal._blitz_start_armour or 0)
     and not state.is_pill_suicider
end

-- Effective loiter-wait cap. Starts at ANGER_WAIT_MAX and shrinks (divisors
-- stack) when sitting out the pill's anger cooldown is cheap or pointless:
--   * pill one hit from death — a single shot kills it even fully angry, so the
--     whole wait is wasted (÷ANGER_WAIT_NEAR_KILL_DIV)
--   * our armour is high — we can tank the angry pill on approach, so the wait
--     matters far less (÷ANGER_WAIT_HIGH_ARMOUR_DIV)
local function effective_anger_wait_max(pill, info)
  local w = C.ANGER_WAIT_MAX
  if pill and (pill.health or 99) <= (C.ANGER_WAIT_NEAR_KILL_HP or 1) then
    w = w - (C.ANGER_WAIT_NEAR_KILL_SUB or 0)
  end
  if (info.armour or 0) >= (C.ANGER_WAIT_HIGH_ARMOUR_ARM or 9999) then
    w = w - (C.ANGER_WAIT_HIGH_ARMOUR_SUB or 0)
  end
  if w < 0 then w = 0 end
  return w
end

local function update_attack_substate_body(goal, state, world, info)
  if goal.kind ~= "attack_pill" then return end

  -- True multi-tank blitz? (commander + >= 1 committed soldier, or we're a
  -- soldier joining one.) Lets the take skip solo-only caution below.
  local blitz_2plus = goal._blitz and blitz_tank_count(goal, state, info) >= 2 or false

  -- Blitz "in position / aimed" status is a CURRENT-TICK fact, true only while
  -- actually sitting in blitz_wait (in position, facing the pill). Default-clear
  -- it every tick here; the blitz_wait handler (and the in-position transitions)
  -- re-set it below. Without this it was sticky — a soldier that reached
  -- blitz_wait then dropped back to approach kept broadcasting rdy=1, and the
  -- commander GO'd without actually waiting for it.
  state.squad_blitz_in_position = nil
  state.squad_blitz_aimed       = nil

  state._attack_substate_name = goal.substate
  -- Tally on-target shots and bump bullets_needed for any misses.
  update_shot_accounting(goal, world)

  -- HUD: kill attempt indicator (top-left)

  -- Floating count above the target pill:
  --   "<on-target shots still in flight> / <pill HP remaining>"
  -- A tree-blocked shot drops out of the in-flight side without changing
  -- the HP side, so the indicator visibly goes down by one rather than
  -- pretending we somehow need more bullets to kill the pill.

  -- Orange counter — shells currently IN THE AIR that will HIT this pill: an
  -- on-target in-flight shot is one whose C-sim trajectory (cpf.simulate_shot,
  -- run each tick by update_shot_accounting) reaches the pill tile with NO
  -- forest/wall/other-pill blocking first. That's exactly goal._on_target_in_flight.
  -- Own layer so it toggles independently of the cyan pill_shot_count.

  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local now = state.tick
  local pmx, pmy = goal.mx, goal.my

  if not goal.substate then goal.substate = "plan_position" end
  -- Person's attack order: has the blitz-wait cap run out? (M.human_blitz_cap_over)
  goal._hcap_over = M.human_blitz_cap_over(goal, state, now or 0)

  -- Kill-claim blitz size (KILL_PICKUP_PAIR_MIN_SQUAD): keep the largest party
  -- seen on this take, so mark_kill_pickup still has it if members drop off
  -- the take (or their broadcasts go stale) right before the pill dies.
  if (C.KILL_PICKUP_PAIR_MIN_SQUAD or 0) > 0 and (goal._blitz or goal._blitz_committed) then
    local n
    if goal._blitz_party_tick == now and goal._blitz_party then
      n = goal._blitz_party
    else
      n = blitz_party_count(state, now, info.player_number or -1, goal.target_id)
    end
    if n > (goal._kp_party_max or 1) then goal._kp_party_max = n end
  end

  -- PING SUICIDE RUN (orders.lua). The goal rides kill_hardline, the straight
  -- rush: path to a tile beside the pill with the pill's own danger taken off
  -- the costs, fire whenever aligned. None of the blitz, LGM-near or
  -- plan_position hold code below runs. The run ends only on pill death
  -- (here) or tank death (orders.on_death). When no tile beside the pill is
  -- reachable, kill_hardline sets _hardline_abort; wipe the bad-tile memory
  -- every PING_SUICIDE_RETRY_TICKS and try again instead of giving up.
  if goal._ping_suicide then
    goal.substate = "kill_hardline"
    local pill = world.pills[goal.target_id]
    if not pill or (pill.health or 0) <= 0 or pill.in_tank
       or pill.owner == "friendly" then
      clear_attack_goal(state, "suicide run: pill dead")
      return
    end
    if goal._hardline_abort
       and now - (goal._suicide_wipe or -1e9) >= (C.PING_SUICIDE_RETRY_TICKS or 50) then
      goal._suicide_wipe = now
      goal._hardline_bad, goal._hardline_abort = nil, nil
      goal._hardline_mx, goal._hardline_my = nil, nil
    end
    return
  end

  -- ATTACK PING = "CHARGE NOW" (C.PING_ATTACK_CHARGE_NOW; orders.lua keeps
  -- state._charge_now).  Decided once per goal, on the first think that sees
  -- the record:
  --   * no blocker standing in this take's wall slots (goal._wall_build_list:
  --     a slot that is a building, half-built wall or pillbox, built this take
  --     or already there): in to shell range, firing -- kill_hardline, which
  --     aims at the pill with the suicide-run aim and paces like the suicide
  --     run (steering.lua: SUICIDE_AIM_AT_PILL, SUICIDE_PACE_AHEAD_TILES,
  --     SUICIDE_HOLD_MARGIN_TILES) but is NOT a suicide run (CHARGE_NOW_IGNORE_SAFETY waives armour, flee,
  --     refuel and shell aborts; see constants.lua): the other goal rules still
  --     apply.  A blitz commander also sends GO so its soldiers go in too.
  --   * a blocker standing: the careful way with no waits and no more
  --     blockers (the hooks test goal._charge_now and are marked CHARGE NOW
  --     below: gather_trees, the approach build decision, blitz_wait,
  --     build_walls and the aim anger gate).
  -- When the straight rush finds no tile beside the pill (kill_hardline's
  -- _hardline_abort) the goal is NOT dropped: it falls back to the careful
  -- way (goal._charge_now_careful, which also keeps the 1-HP kill_hardline
  -- shortcuts off so it does not go back into the same dead end).
  do
    local cn = state._charge_now
    if cn and cn.tid == goal.target_id and not goal._charge_now then
      goal._charge_now = true
      local placed = false
      for _, wp in ipairs(goal._wall_build_list or {}) do
        local tt = U.ttype(wp.mx, wp.my)
        if tt == C.T_BUILDING or tt == C.T_HALFBUILD or tt == C.T_PILLBOX then
          placed = true; break
        end
      end
      if not placed then
        if goal._blitz and state.squad_role == "c" then
          goal._blitz_go = true        -- init.lua broadcasts bgo from this
          state.squad_blitz_go = true
        end
        goal.substate = "kill_hardline"
        goal._kill_rush = true
        goal._kill_rush_decided = true
        goal._hardline_abort, goal._hardline_bad = nil, nil
        goal._hardline_mx, goal._hardline_my = nil, nil
        goal._aim_locked = nil
        goal.wall_shield, goal.wall_mx, goal.wall_my = false, nil, nil
        return
      end
    end
  end

  -- CONTESTED TAKE, re-checked. The GO-time check only sees the enemies that
  -- were near the pill at GO; a defender that rolls up mid-charge should flip
  -- the party just the same. Commander only, only once GO has actually gone out
  -- (goal._blitz_go, the flag every GO route sets, or goal._blitz_su_done, set
  -- by the two GO sites that designate) and only on the bot's REPLAN ticks:
  -- init.lua stamps state.replan_this_tick above this call, and a replan is
  -- already the cadence at which this bot re-reads the world. Cheap either way
  -- (a scan of this tick's visible hostile tanks). The latch is the goal's
  -- contested MODE: once a party of 2+ has been designated ("done") this stops
  -- entirely, so a take designates at most once. A "solo" check sets no mode,
  -- so a take that later gains a soldier is still caught here.
  -- BOTH flags are on the GOAL, never state.squad_blitz_go: that one is only
  -- cleared on death/reset_blitz_state, so a commander that finished one take
  -- and opened another in the same life still carries it, and this check would
  -- fire on the new take BEFORE its GO.
  if state.squad_role == "c" and goal._blitz and state.replan_this_tick
     and (goal._blitz_go or goal._blitz_su_done)
     and not goal._blitz_contested_mode then
    blitz_contested_check(goal, state, world, info, now)
  end

  -- Squad blitz: a soldier attacking its commander's blitz pill flags the goal
  -- and, once committed, heads straight to its NEGOTIATED engage spot via the
  -- normal `approach`. If the blitz ends — commander died (squad_cmdr cleared)
  -- or the squad retargeted — drop the take entirely.
  -- Committed takes are NOT aborted by the blitz ending — once we've started
  -- EXECUTING (building the shield, aiming, charging, firing) we finish the
  -- kill. Only bail from a PRE-COMMIT substate (plan_position / approach /
  -- blitz_wait). NOTE: _blitz_committed is only latched by the blitz_wait GO
  -- handshake, but a full-pill commander blitz fires via the normal PPT path
  -- (build_walls→aim→charge→…→shoot_pill) and never sets it — so the substate
  -- gate, not _blitz_committed alone, is what protects a live shoot_pill from
  -- the abort (saw it clobber one: 20260604_141133 bot0 t4911).
  local _blitz_precommit = goal.substate == "plan_position"
                           or goal.substate == "approach"
                           or goal.substate == "blitz_wait"
  -- ...but NOT for the blitz COMMANDER. squad_blitz_target is recomputed from
  -- scratch every tick in squad.update and is only re-set for a bot whose role
  -- reads COMMANDER that tick; the DYNAMIC_COMMANDERS partition flickers, so a
  -- bot LEADING its own blitz briefly loses squad_blitz_target on an off-tick
  -- even though it's still on the same pill. goal._blitz_cmdr (latched below)
  -- marks "we're the commander of this take" so the flicker can't make us abort
  -- our own blitz with a bogus "commander gone" (saw bot8 do exactly this).
  -- "Blitz ended" = the commander really left (we're no longer committed to
  -- anyone — squad_blitz_accepted cleared) OR the squad genuinely retargeted to a
  -- DIFFERENT non-nil pill. A nil squad_blitz_target while STILL committed is a
  -- transient bookkeeping gap (the commander's /info state target went stale while
  -- it only sent bac/pblk), NOT the commander leaving — don't drop our negotiated
  -- standoff over it (saw bot0 lose its approach spot exactly this way).
  local _cmdr_gone  = not state.squad_blitz_accepted
  local _retargeted = state.squad_blitz_target and goal.target_id ~= state.squad_blitz_target
  if goal._blitz and not goal._blitz_committed and _blitz_precommit
     and not goal._blitz_cmdr
     and (_cmdr_gone or _retargeted) then
    state.squad_blitz_engage_mx, state.squad_blitz_engage_my = nil, nil
    state.squad_blitz_engage_fx, state.squad_blitz_engage_fy = nil, nil
    state.squad_blitz_engage_deg = nil
    state.squad_blitz_in_position = nil
    clear_attack_goal(state, "blitz ended: commander gone / squad retargeted")
    return
  end
  -- _blitz_solo latches when a blitz falls back to a solo protected take (nobody
  -- joined by the time we were in position) so the blitz transition can't grab
  -- us again — we finish the take solo with the normal blocker/wall-shield plan.
  if state.squad_blitz_target and goal.target_id == state.squad_blitz_target
     and not goal._blitz_solo then
    goal._blitz = true
    -- Latch commander-ness for this take. Reaching here with NO squad_cmdr can
    -- only mean we're the COMMANDER: a soldier's squad_blitz_target is always
    -- set alongside squad_cmdr (the accept/commit path), whereas a commander's
    -- comes from the role==COMMANDER branch in squad.update with squad_cmdr nil.
    -- Latch-only (never cleared here) so a later role flicker can't unset it
    -- mid-take; it dies with the goal via clear_attack_goal.
    if not state.squad_cmdr then goal._blitz_cmdr = true end
    -- The blitz goal is only adopted once the negotiation is ACCEPTED (squad
    -- layer), so reaching here means we're committed. The blitz CALL is an
    -- ongoing PARALLEL thing — goal._blitz drives bco recruiting + the bes
    -- standoff broadcast every tick, active for the whole approach — NOT a
    -- blocking substate.
    --   * Soldier (squad_cmdr set): commit the NEGOTIATED engage spot and head
    --     to its setup point via the normal `approach`. No re-scan.
    --   * Commander (no squad_cmdr): stay in plan_position and plan the take as
    --     a normal PPT (standoff + wall-shield). At the in-position decision it
    --     SKIPS the walls if squadmates joined (a double-take overwhelms with no
    --     walls), else builds them for a solo protected take.
    if state.squad_cmdr then
      -- Just DEFERRED from commander to soldier (role flipped c->s, e.g. yielding
      -- to a lower-pn ally also opening on this pill)? goal._blitz_cmdr is latched
      -- ONLY while we led this take as commander (set when squad_cmdr was nil), so
      -- its presence now — with squad_cmdr set — means the goal still carries the
      -- commander's OWN plan: a self-picked standoff + setup point, a latched
      -- _blitz_started, and possibly a commander-only BUILD substate (gather_trees
      -- / build_walls / detree / ws_*). Left alone we'd FREEZE in a build substate
      -- OR drive to the OLD self-picked setup point — the soldier commit below is
      -- gated on `not _blitz_started`, so it never overrides, and we only re-commit
      -- much later when we happen to re-enter a build substate (saw a deferred bot
      -- run its whole commander plan ~450 ticks before correcting). Shed ALL
      -- commander planning AND the stale standoff/nav target so the soldier
      -- re-commits to the NEGOTIATED engage spot from plan_position. Clearing
      -- _blitz_cmdr makes this fire exactly once (the commit below latches
      -- _blitz_started fresh; squad_blitz_engage_mx — the negotiated spot — is left
      -- untouched). NOTE: do NOT clear state.squad_blitz_engage_mx here — that's the
      -- negotiated soldier standoff blitz_commit_negotiated reads next.
      if goal._blitz_cmdr then
        goal.substate         = "plan_position"
        goal._blitz_started   = nil
        goal._blitz_cmdr      = nil
        goal._wall_build_list = nil
        goal.standoff_fx, goal.standoff_fy = nil, nil
        goal.standoff_mx, goal.standoff_my = nil, nil
        goal.approach_fx, goal.approach_fy = nil, nil
        goal.approach_mx, goal.approach_my = nil, nil
        goal._approach_start  = nil
      end
      -- SOLDIER: commit the NEGOTIATED, de-conflicted engage spot as our
      -- standoff. Allow this from plan_position OR approach: with discount-driven
      -- joining the soldier is already on the blitz pill's attack_pill while it
      -- negotiates, so its OWN plan_position scan can run and advance the goal to
      -- approach (picking a spot that clashes with the commander, since both use
      -- the same scorer) BEFORE the squad layer marks it committed. Re-committing
      -- here overrides that self-scan with the negotiated spot once committed.
      local in_precommit = goal.substate == "plan_position" or goal.substate == "approach"
      if in_precommit and not goal._blitz_started then
        goal._blitz_started = true
        if not blitz_commit_negotiated(goal, state, world, info, pmx, pmy) then
          -- No negotiated/derivable spot yet — retry the commit next tick.
          goal._blitz_started = nil
          -- C.BLITZ_NOSPOT_RENEGOTIATE: committed with no engage spot. Drop
          -- the accept so squad.update negotiates a fresh, arbiter-checked
          -- spot; the blitz-ended check then clears this goal next tick.
          if C.BLITZ_NOSPOT_RENEGOTIATE and state.squad_blitz_accepted then
            state.squad_blitz_accepted = nil
            state._blitz_commit_tick   = nil
          end
        else
        end
      end
      blitz_sync_engage(state, goal, now)
      -- brj for a COMMITTED soldier: drop the rejected spot and replan
      -- (M.blitz_brj_replan). Anything but "none" ends this tick's update.
      if M.blitz_brj_replan(state, goal, pmx, pmy, now) ~= "none" then return end
    elseif goal.substate == "plan_position" and not goal._blitz_started then
      -- COMMANDER: plan its own take at plan_position (standoff + wall-shield).
      goal._blitz_started = true
    end
  end

  -- BLITZ-ONLY GATE ("blitzonly" flag / C.BLITZ_ONLY_PILL_ATTACKS, see
  -- squad.blitz_only). A take may sit in the pre-GO substates (the ones a
  -- blitz call stays open in: plan_position / approach / gather_trees /
  -- detree / build_walls / blitz_wait) as long as it likes -- that is where a
  -- commander calls, recruits and gathers. It may go past them (aim, charge,
  -- in_range_*, shoot_pill, engage, swerve, kill_hardline, ...) ONLY as a
  -- committed blitz: goal._blitz, goal._blitz_committed (commit_fire latched it
  -- on a real GO -- quorum met, or the soldier heard GO) and not the solo
  -- fall-back _blitz_solo. Anything else is a solo attack and the take is
  -- dropped. The pool gate (goals.lua apply_blitz_only_gate) keeps most solo
  -- takes from being picked at all; this is the backstop for the ones that
  -- were picked (role changed, the call closed, a take that never became a
  -- blitz) so none of them fires on the pill.
  if squad.blitz_only(state) then
    local _bo_sub = goal.substate or "plan_position"
    if not squad.BLITZ_CALL_OPEN_SUB[_bo_sub]
       and not (goal._blitz and goal._blitz_committed and not goal._blitz_solo)
       -- CHARGE NOW: a person told this bot to go in; that is not a solo
       -- take the blitz-only rule should drop.
       and not goal._charge_now then
      clear_attack_goal(state, "blitz_only: " .. _bo_sub .. " outside a committed blitz")
      return
    end
  end

  -- LGM-near-pill abort: a hostile LGM within the danger radius of the
  -- target pill means the defender is right there ready to retake /
  -- repair, AND will be supported by their tank. Different reaction
  -- depending on substate:
  --   * "Firing" substates (charge / shoot_pill / engage / in_range_aim
  --     [_finetune]) — we have rounds in flight or are about to fire;
  --     enter swerve to dodge return-fire instead of bailing flat-footed.
  --   * Everything else — clear_attack_goal so pick_goal picks something
  --     safer next tick.
  -- Either way, stamp pill_danger_nearby[pill_id] = now + ~30s so the
  -- eval re-pick adds a danger_nearby ×1.5 multiplier and we don't
  -- bounce right back onto this same pill.
  if goal.substate ~= "kill_hardline" then
    local LGM_RADIUS = C.PILL_DANGER_NEARBY_RADIUS or 3
    local lgm_seen = nil
    local perc = state.perc
    if perc and perc.enemy_lgms then
      for _, el in ipairs(perc.enemy_lgms) do
        local dx = (el.mx or 0) - pmx
        local dy = (el.my or 0) - pmy
        if dx >= -LGM_RADIUS and dx <= LGM_RADIUS
           and dy >= -LGM_RADIUS and dy <= LGM_RADIUS then
          lgm_seen = el
          break
        end
      end
    end
    if lgm_seen then
      local pid = goal.target_id
      if pid then
        state.pill_danger_nearby = state.pill_danger_nearby or {}
        state.pill_danger_nearby[pid] = now + (C.PILL_DANGER_NEARBY_TICKS or 1500)
      end
      local FIRING_SUBS = {
        charge=true, shoot_pill=true, engage=true,
        in_range_aim=true, in_range_aim_finetune=true,
      }
      local cur_sub = goal.substate or "?"
      -- Committed blitz: the squad converges together and commits to the kill.
      -- Don't break off defensively just because a defender LGM is near — only
      -- swerve once we've actually TAKEN damage (armour dropped below the value
      -- we committed with). Until then, hold the line and keep firing.
      local blitz_hold = goal._blitz_committed
                         and (info.armour or 0) >= (goal._blitz_start_armour or 0)
      -- Pill-suicider: never peels off a take it is already firing on, so a
      -- defender LGM arriving is not a reason to swerve OR abort — it holds
      -- exactly like a committed blitz and keeps shooting. (Same fall-through
      -- branch, so nothing else in this block changes.)
      if not blitz_hold and state.is_pill_suicider and FIRING_SUBS[cur_sub] then
        blitz_hold = true
      end
      if FIRING_SUBS[cur_sub] and not blitz_hold then
        print(string.format(TAG ..
          " ATTACK: LGM@(%d,%d) within %dt of pill@(%d,%d) — entering swerve from %s",
          lgm_seen.mx or -1, lgm_seen.my or -1, LGM_RADIUS, pmx, pmy, cur_sub))
        enter_swerve(goal, world, state, info, pmx, pmy, "defensive")
        return
      elseif blitz_hold then
        -- Hold: keep firing through the danger. Fall through to the substate
        -- handler below instead of swerving or aborting.
      else
        clear_attack_goal(state, string.format(
          "abort@%s — enemy LGM@(%d,%d) within %dt of pill@(%d,%d)",
          cur_sub, lgm_seen.mx or -1, lgm_seen.my or -1, LGM_RADIUS, pmx, pmy))
        return
      end
    end
  end

  -- Before committing to plan_position, wait for the LGM to return.
  -- Without the builder we can't capture after killing or build shields.
  -- If the LGM is out (not in tank, not dead) and the builder isn't
  -- actively dispatching it for THIS goal's purposes (gather_trees etc.),
  -- hold in plan_position without doing work — the LGM will return and
  -- we resume. This avoids aborting the goal (which causes oscillation)
  -- while still not starting the expensive angle sweep until the LGM
  -- is available.
  -- kill_hardline doesn't need the builder (no post-kill capture/shield), so
  -- a hardline candidate must NOT be held here — let it fall through to the
  -- plan_position detection below and switch immediately.
  local _hardline_candidate = goal.substate == "plan_position"
    and pill and (pill.health or 0) == 1
    and info.armour >= (C.ATTACK_RUSH_MIN_ARMOUR or 5)
    and ((pill.anger or 0) <= (C.ATTACK_RUSH_MAX_ANGER or 0.34)
         -- ...or known-calm by time: a 1-HP pill can't re-heat, so once it's
         -- gone PILL_ANGER_DECAY ticks without a hit it's fully calm even if
         -- the anger proxy reads stale-high.
         or ((state.tick or 0) - (pill.last_hit_tick or 0)) >= (C.PILL_ANGER_DECAY or 3000))
  -- ...and a man the brain has already flagged state.lgm_stranded is NOT
  -- coming back, so waiting for him is not a wait, it is a freeze. On
  -- Everard (2026-09-07) this hold ran for 79k ticks: the tank never moved,
  -- so the static danger field over its tile never cleared, so the
  -- rescue_lgm override that would have fetched the man stayed suppressed,
  -- so the man never returned. Skipping the hold on a stranded man breaks
  -- that loop from this side; goals.lua's RESCUE_LGM_SUPPRESS_BY_FIRE_AGE
  -- breaks it from the other. Deliberately no timeout: a man who is merely
  -- walking home is still worth waiting for, and every freeze seen so far
  -- had the stranded flag set.
  local _pp_hold_stranded = C.ATTACK_PP_HOLD_SKIP_STRANDED
                            and state.lgm_stranded or false
  if goal.substate == "plan_position"
     and not goal.scan_spots
     and not _hardline_candidate
     and not _pp_hold_stranded
     -- A person's attack order past HUMAN_ATTACK_BLITZ_WAIT_MAX_S does not
     -- wait for the man: the tank goes and he walks back to it.
     and not goal._hcap_over
     and info.man_status ~= C.LGM_INTANK
     and info.man_status ~= C.LGM_DEAD then
    return  -- hold, don't advance plan_position until LGM is back
  end
  goal._pp_hold_since = nil
  goal._pp_hold_print = nil

  -- Only log on substate transitions (avoid spamming every tick)
  if goal.substate ~= goal._last_logged_sub then
    goal._last_logged_sub = goal.substate
  end

  -- Track whether the LGM was dead when this pill take started.
  -- If it was dead and then respawns (transitions to ground/intank),
  -- abort pre-engage substates so the bot can go pick up the builder
  -- instead of continuing a take it started without one.
  if not goal._lgm_was_dead_at_start then
    goal._lgm_was_dead_at_start = (info.man_status == C.LGM_DEAD)
  end
  if goal._lgm_was_dead_at_start
     and info.man_status ~= C.LGM_DEAD
     and PRE_ENGAGE_SUBS[goal.substate] then
    print(string.format(TAG .. " ATTACK: aborting pre-engage (%s) — LGM respawned mid-take (status=%d)",
      goal.substate, info.man_status))
    clear_attack_goal(state, "LGM respawned mid-take")
    return
  end

  -- Look up pill
  local pill = M.find_pill_at(world, pmx, pmy)

  -- PPT eligibility is decided ONCE at goal start (plan_position's
  -- scan-once block sets _is_ppt from the initial pill HP). We used
  -- to re-check every tick and demote shoot_pill -> engage if the
  -- pill dropped below threshold mid-attack — that produced a
  -- jarring "I was about to fire and now I'm in the wrong substate
  -- with the wrong aim point and the shield wall still up" moment
  -- that lost shots. Since we already paid the PPT setup cost
  -- (built shield walls, picked corner aim), riding it out for the
  -- rest of the kill is the right call. The non-PPT engage path
  -- only makes sense as the INITIAL choice for a low-HP pill.

  -- The fresh-entry clear inside plan_position is gated by
  -- _plan_position_cleared so it doesn't re-run every tick we sit there. But
  -- that flag MUST reset once we leave plan_position, or a SECOND pass
  -- (plan_position -> aim -> shoot_pill -> swerve -> plan_position) skips the
  -- clear and inherits the first attack's stale _wall_build_list / _is_ppt /
  -- _aim_locked / _shield_scan — which makes the re-entry build_walls stall.
  if goal.substate ~= "plan_position" then goal._plan_position_cleared = nil end

  -- ══════════════════════════════════════════════════════════════════
  -- plan_position: full terrain analysis to find best attack spot
  -- ══════════════════════════════════════════════════════════════════
  -- _plan_position_cleared gates a once-per-episode clear; it MUST reset when we
  -- leave plan_position, or a second pass (plan_position -> aim -> shoot_pill ->
  -- swerve -> plan_position) inherits the first attack's stale _wall_build_list /
  -- _is_ppt / _aim_locked / _shield_scan and the re-entry build_walls stalls.
  if goal.substate ~= "plan_position" then goal._plan_position_cleared = nil end

  if goal.substate == "plan_position" then
    -- Fresh-entry cleanup: an earlier attack on this same pill (or any
    -- prior goal-of-the-same-target round-trip) can leave stale shield
    -- planning hanging on the goal struct.  goal.substate=plan_position
    -- is the canonical fresh-start state for attack_pill, so wipe the
    -- shield + scan + wall-build artifacts here before we recompute.
    -- Re-entries within the same goal (we stayed in plan_position across
    -- ticks) are idempotent — goal.scan_spots is what gates the
    -- expensive re-scan below, and this runs even on the first tick of
    -- the substate, which is the worst case (one extra nil assignment
    -- batch per tick of plan_position).
    if not goal._plan_position_cleared then
      goal._shield_scan         = nil
      goal._shield_scan_pending = nil
      goal._wall_build_list     = nil
      goal._wall_build_idx      = nil
      goal._wall_build_done     = nil
      goal._aim_locked          = nil
      goal._is_ppt              = nil
      goal._kill_rush           = nil
      goal._kill_rush_decided   = nil
      goal._hardline_abort      = nil
      goal._hardline_mx         = nil
      goal._hardline_my         = nil
      goal._hardline_bad        = nil
      goal._trees_for_walls     = nil
      goal.scan_spots           = nil
      goal.standoff_mx          = nil
      goal.standoff_my          = nil
      goal.standoff_fx          = nil
      goal.standoff_fy          = nil
      goal.approach_fx          = nil
      goal.approach_fy          = nil
      goal.approach_mx          = nil
      goal.approach_my          = nil
      goal._chosen_deg          = nil
      goal.aim_wx               = nil
      goal.aim_wy               = nil
      goal.aim_idx              = nil
      goal.through_walls        = nil
      goal._plan_show_tick      = nil
      goal._plan_logged         = nil
      goal._plan_position_cleared = true
    end

    -- ── kill_hardline fast path ───────────────────────────────────────
    -- A 1-HP pill that hasn't been provoked is a free kill: hand off to the
    -- dedicated kill_hardline substate, which navs to a tile beside the pill
    -- and fires on every clear shot (through trees) until it's dead. Gated on:
    -- pill HP == 1, armour > ATTACK_RUSH_MIN_ARMOUR (soak the return fire),
    -- and pill anger <= ATTACK_RUSH_MAX_ANGER. Decided ONCE per goal.
    if not goal._kill_rush_decided then
      goal._kill_rush_decided = true
      local php    = pill and pill.health or 0
      local panger = pill and pill.anger  or 0
      -- A charge-now that already fell back from kill_hardline stays careful.
      if php == 1 and not goal._charge_now_careful
         and info.armour >= (C.ATTACK_RUSH_MIN_ARMOUR or 5)
         and panger <= (C.ATTACK_RUSH_MAX_ANGER or 0.34)
         -- Blitz-only: a 1-HP rush is a solo attack unless the blitz already
         -- went GO, so it is not taken (BLITZ_ONLY_ABORT would drop it).
         and not (squad.blitz_only(state) and not goal._blitz_committed)
         and U.mdist(bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8), pmx, pmy) <= (C.HARDLINE_ENGAGE_RANGE or 10) then
        goal._kill_rush = true
        goal.substate   = "kill_hardline"
        return
      end
    end

    -- Only scan once, reuse stored results for drawing
    if not goal.scan_spots then
      goal._scan_tank_mx = tmx
      goal._scan_tank_my = tmy

      local _t_pp0 = BRAIN_PROFILE and clock_us() or 0
      -- Per-pill cache of the plan_position angle sweep result.  Reusing
      -- a cached sweep up to 5 s (250 ticks @ 50 Hz) old skips the
      -- ~9 ms evaluate_pill_difficulty call entirely on re-entry.
      -- Pills move slowly enough (anger decays over hundreds of ticks,
      -- ownership flips are rare mid-take) that 5 s of staleness is
      -- acceptable; downstream shield scan + standoff selection still
      -- run fresh each time, picking up wall/blocker changes.
      -- Capacity-tier pp_spread: this is the heaviest single scan in
      -- the brain (~9 ms at 5° × 72 angles). When the tier requests a
      -- spread > 1 we coarsen to 45° — 8 angles via precomputed
      -- stamps, ~1 ms.  Cache hit path skips both costs.
      -- Chunked plan_position angle sweep. State lives in
      -- state._pill_eval_progress[pid]; result lands in
      -- state._pill_eval_cache[pid] (TTL 250 ticks). No background
      -- pre-caching — only the committed pill gets swept.
      local pid = goal.target_id
      local best_score, spots
      local status = M.advance_pill_eval_chunk(state, world, info, tmx, tmy, pid, pill)
      -- Sweep abandoned (chunk budget-killed PP_CHUNK_KILL_TRIES times, or the
      -- pill is still inside its blacklist window): there is no plan to be had
      -- for this take, so drop the goal instead of re-entering plan_position
      -- every tick. eval_attack_pill skips the pill while the window lasts, so
      -- pick_goal moves on to another target.
      if status == "blacklisted" then
        clear_attack_goal(state, string.format(
          "plan_position: pill#%s angle sweep abandoned (tick-budget kills)", tostring(pid)))
        return
      end
      -- Plan-trace overlay: stamp every gate so the screen can show
      -- exactly where the chain falls off. Reset at substate entry.
      if status == "cached" or status == "done" then
        local cached = state._pill_eval_cache and state._pill_eval_cache[pid]
        if cached then
          best_score = cached.best_score
          spots      = cached.spots
        end
        if BRAIN_PROFILE_LOG and status == "cached" then
          opt.append("optimize.log", string.format(
            "  [as] plan_position CACHE HIT pid=%s pill=(%d,%d)",
            tostring(pid), pmx, pmy))
        end
      end
      -- if status == "in_progress", spots stays nil and the gate below
      -- skips the rest of plan_position setup. Re-enters next tick.
      -- Only proceed with the rest of plan_position setup if the angle
      -- sweep is complete (cache hit OR final chunk just landed).
      -- Otherwise spots is nil — chunking still in progress; the goal
      -- re-enters plan_position next tick and continues the sweep.
      if spots then
      -- DIAG_PP_GATE: pin down which path lit `spots` so we can tell
      -- a fresh completed sweep apart from a stale-cache hit.
      -- status is "done" (just-completed this tick) or "cached"
      -- (prior sweep within 250-tick TTL).  deg_cursor (if any) shows
      -- where the in-flight sweep was — useful for spotting "cache hit
      -- while a separate in-progress sweep was visible".
      do
        local _deg = "-"
        if state._pill_eval_progress and state._pill_eval_progress[pid] then
          _deg = tostring(state._pill_eval_progress[pid].deg_cursor)
        end
      end
      -- (goal.scan_spots = spots is set AT THE END of this block — see
      -- the matching assignment just before the `end` below.  If we set
      -- it here, a tick_budget_exceeded abort mid-flow would leave the
      -- substate stuck: scan_spots set → next tick skips this block →
      -- greens/best never re-run, _shield_scan_pending never set.
      -- Deferring the gate-bit until ALL the setup is done makes the
      -- whole block idempotent across budget aborts — the cache in
      -- state._pill_eval_cache survives, so re-entry just re-runs
      -- greens/best from the same cached spots.)
      -- plan-trace: count spots + LOS subset
      if BRAIN_PROFILE_LOG then
        opt.append("optimize.log", string.format(
          "  [as] plan_position eval_pill=%.3f ms pill=(%d,%d)",
          (clock_us() - _t_pp0) / 1000, pmx, pmy))
      end

      -- Step 1: apply influence bonus/penalty to all LOS spots.
      --   friendly territory (influence > 0): -5 (cheaper)
      --   hostile territory  (influence < 0): +5 (more expensive)
      -- Stored as adj_score for all subsequent ranking.
      for _, s in ipairs(spots) do
        if s.has_los then
          s._influence = cpf.influence_at(s.mx, s.my)
          local infl_adj = 0
          if s._influence > 0 then infl_adj = -5
          elseif s._influence < 0 then infl_adj = 5
          end
          s._infl_adj = infl_adj
          s._adj_score = (s.total_score or 999) + infl_adj
        end
      end

      -- Committed blitz soldier re-planning after a blocked line: drop spots
      -- that fail the LOS margin or sit on the commander's standoff (nil = no
      -- filter; see blitz_soldier_replan_filter). Solo takes: always nil.
      local bz_skip, bz_aims = blitz_soldier_replan_filter(state, goal, world, spots, pmx, pmy)
      -- C.BLITZ_NOSPOT_RENEGOTIATE: a soldier's own pick on the blitz pill
      -- keeps SQUAD_BLITZ_CLASH_TILES from the commander's and allies' spots.
      -- Every LOS spot clashes = no spot of our own; drop the goal and
      -- negotiate again.
      do
        local az_skip, az_los, az_n = M._blitz_ally_spot_filter(state, goal, spots, info.player_number or -1)
        if az_skip and az_n > 0 then
          if az_n >= az_los then
            clear_attack_goal(state, string.format("blitz: every own spot (%d) clashes with a squad spot, renegotiate", az_los))
            return
          end
          bz_skip = bz_skip or {}
          for s in pairs(az_skip) do bz_skip[s] = true end
        end
      end

      -- Step 2: collect all "green" spots (adj_score < 10 with LOS)
      local greens = {}
      for _, s in ipairs(spots) do
        if s.has_los and not (bz_skip and bz_skip[s]) and s._adj_score and s._adj_score < 10 then
          greens[#greens + 1] = s
        end
      end

      -- Step 3: no greens — pick top-scoring LOS spot + all within 25% of it.
      -- "Within 25%" means adj_score <= top_score * 1.25 (lower is better).
      if #greens == 0 then
        local los_spots = {}
        for _, s in ipairs(spots) do
          if s.has_los and not (bz_skip and bz_skip[s]) then los_spots[#los_spots + 1] = s end
        end
        table.sort(los_spots, function(a, b)
          return (a._adj_score or math.huge) < (b._adj_score or math.huge)
        end)
        if #los_spots > 0 then
          local top_score = los_spots[1]._adj_score or math.huge
          local threshold = top_score * 1.25
          for _, s in ipairs(los_spots) do
            if (s._adj_score or math.huge) <= threshold then
              greens[#greens + 1] = s
            else
              break  -- sorted, so we can stop
            end
          end
        end
      end

      local best = nil
      if #greens > 0 then
        -- Step 4: rank by real Dijkstra travel cost from the tank.
        -- The bot already maintains a KIND_NORMAL full-map Dijkstra
        -- rooted at its current position, so this is a cached lookup
        -- (no per-spot search) that beats the linear estimate the
        -- shortlist tiebreak used to use.
        local boat = info.inboat and 1 or 0
        for _, s in ipairs(greens) do
          local dij = cpf.dijkstra_lookup_by_kind(cpf.KIND_NORMAL, s.mx, s.my, boat)
          if dij >= 1e29 then
            -- Tile not yet reached by the search (rare on land paths
            -- but possible early in a search or for unreachable spots).
            -- Fall back to the linear estimate so ranking still works.
            dij = cpf.estimate_cost(tmx, tmy, s.mx, s.my, boat)
          end
          s._est = dij
        end
        table.sort(greens, function(a, b) return (a._est or math.huge) < (b._est or math.huge) end)
        best = greens[1]
      end

      -- Log selection

      if best then
        goal.standoff_mx = best.mx   -- integer tile for A* nav
        goal.standoff_my = best.my
        goal.standoff_fx = best.cx  -- precise float for charge/engage
        goal.standoff_fy = best.cy
        goal._chosen_deg = best.deg
        -- The aim point the spot scan proved this spot can hit (pill centre or
        -- one of its corners). It rides the goal in WORLD units as
        -- aim_wx/aim_wy rather than aim_mx/aim_my because init.lua rewrites
        -- aim_mx/aim_my from near-edge geometry on EVERY tick of a non-PPT take,
        -- which would throw the planned corner away before we ever fired.
        goal.aim_wx  = best.aim_wx
        goal.aim_wy  = best.aim_wy
        goal.aim_idx = best.aim_idx
        -- Wall-pass spot (C.ATTACK_PILL_WALL_FALLBACK): its line crosses built
        -- walls, and the take shoots through them.
        goal.through_walls = best.walls or nil
        -- Blitz soldier replan: the aim point that passed the LOS margin.
        local bz_a = bz_aims and bz_aims[best]
        if bz_a then goal.aim_idx, goal.aim_wx, goal.aim_wy = bz_a[1], bz_a[2], bz_a[3] end
        goal._scan_tick = state.tick   -- frame stamp for staged overlay reveal
        -- Approach position: extend line from pill through standoff by
        -- APPROACH_OFFSET. Stored as both float (precise final target)
        -- and clamped integer tile (for the A* path target).
        local dx = best.cx - (pmx + 0.5)
        local dy = best.cy - (pmy + 0.5)
        local d = math.sqrt(dx * dx + dy * dy)
        if d > 0.01 then
          local ux, uy = dx / d, dy / d
          goal.approach_fx = best.cx + ux * C.ATTACK_APPROACH_OFFSET
          goal.approach_fy = best.cy + uy * C.ATTACK_APPROACH_OFFSET
          goal.approach_mx = U.mclamp(math.floor(goal.approach_fx))
          goal.approach_my = U.mclamp(math.floor(goal.approach_fy))
        else
          goal.approach_mx = best.mx
          goal.approach_my = best.my
          goal.approach_fx = best.cx
          goal.approach_fy = best.cy
        end

        -- Protected pill take (PPT) gating: high-HP pills use a tighter
        -- standoff and a slower charge so the wall-shielded angle is
        -- preserved. Determined ONCE here for the lifetime of this
        -- attack_pill goal and stashed on the goal for downstream
        -- substates / steering.
        local pill_hp = pill and pill.health or 0
        goal._is_ppt = pill_hp >= (C.PPT_HEALTH_THRESHOLD or 8)
        -- Force PPT when our armour is low enough that taking return
        -- fire during a charge could be lethal, even on a soft pill:
        --   * armour <= ARMOUR_LOW (15): one or two hits from flee.
        --   * armour <= ARMOUR_MODERATE (25) AND chosen standoff is in
        --     a hot threat zone (>= ARMOUR_MOD_PPT_DANGER): mid-armour
        --     plus dangerous approach corridor.
        if not goal._is_ppt then
          local force_low = info.armour <= (C.ARMOUR_LOW or 15)
          local force_mod = false
          if info.armour <= (C.ARMOUR_MODERATE or 25)
             and goal.standoff_mx and goal.standoff_my then
            local sd = threat.at(goal.standoff_mx, goal.standoff_my)
            if sd >= (C.ARMOUR_MOD_PPT_DANGER or 75) then
              force_mod = true
            end
          end
          -- Angry pill: anger above ~1 hit's worth means it reloads fast and
          -- will punish a bare hardline/non-PPT charge — take the wall shield
          -- even on a soft pill. Gate on a RECENT real hit: a pill at low HP
          -- can't re-heat (any shot kills it), so once it's gone PILL_ANGER_DECAY
          -- ticks without a hit it's known-calm — don't let stale anger force a
          -- wall then (the bot should hardline).
          local _now = state.tick or 0
          local hit_recent = pill and pill.last_hit_tick
            and (_now - pill.last_hit_tick) < (C.PILL_ANGER_DECAY or 3000)
          local force_anger = hit_recent and (pill.anger or 0) > (C.PPT_ANGER_THRESHOLD or 0.34)
          if force_low or force_mod or force_anger then
            goal._is_ppt = true
            local reason = force_low and "LOW_ARMOUR"
                        or force_mod and "MOD_ARMOUR+HOT_STANDOFF"
                        or string.format("ANGRY_PILL(%.2f)", pill and pill.anger or 0)
          end
        end
        -- Pill-suicider: never a protected take. PPT is the wall-shielded
        -- build-then-fire mode, and a suicider builds no blockers at all, so
        -- force it OFF here (after the force_* overrides, which would otherwise
        -- reinstate it) — that alone routes the whole take down the plain
        -- standoff/charge path: the gather_trees pre-flight and the build_walls
        -- entry are both gated on _is_ppt + _shield_scan. The shield.scan
        -- itself is skipped below (its ~20 ms of tick budget goes unspent).
        --
        -- A HUMAN TEAM-MATE STANDING CLOSE DOES THE SAME THING, and for a
        -- different reason: the wall shield costs trees, a gather detour and a
        -- slow shielded creep, and a person watching from seven squares away
        -- reads all of that as the bot doing nothing. Close to a human the bot
        -- goes straight in. Same effect on the take, same one flag, so it
        -- lands here beside the designated suicider rather than in a branch of
        -- its own; ORDER_HUMAN_NEAR_SUICIDE_TILES = 0 turns it off.
        local human_tiles = C.ORDER_HUMAN_NEAR_SUICIDE_TILES or 0
        local human_near = nil
        if human_tiles > 0 then
          human_near = U.human_ally_near(info, bit.rshift(info.tankx or 0, 8),
                                         bit.rshift(info.tanky or 0, 8), human_tiles)
        end
        goal._human_near = human_near
        if (state.is_pill_suicider or human_near) and goal._is_ppt then
          goal._is_ppt = false
        end
        local scan_radius = goal._is_ppt and C.PPT_STANDOFF
                            or C.ATTACK_PILL_STANDOFF
        -- For PPT, pull the chosen standoff in from 7.4 to 7.0 along
        -- the same angle the planner picked. evaluate_pill_difficulty
        -- already laid the standoff on the 7.4 circle; rescaling keeps
        -- the angle and just trims the radius.
        if goal._is_ppt and goal.standoff_fx and goal.standoff_fy then
          local cdx = goal.standoff_fx - (pmx + 0.5)
          local cdy = goal.standoff_fy - (pmy + 0.5)
          local clen = math.sqrt(cdx * cdx + cdy * cdy)
          if clen > 0.01 then
            local scale = scan_radius / clen
            goal.standoff_fx = (pmx + 0.5) + cdx * scale
            goal.standoff_fy = (pmy + 0.5) + cdy * scale
            goal.standoff_mx = U.mclamp(math.floor(goal.standoff_fx))
            goal.standoff_my = U.mclamp(math.floor(goal.standoff_fy))
          end
        end

        -- Defer shield.scan to NEXT tick. The chunk-completion frame
        -- has already paid for the final pill_eval chunk + influence
        -- pass + greens picking; piling shield.scan on top tends to
        -- blow the per-tick budget at low capacity tiers (the budget
        -- hook then raises tick_budget_exceeded mid-influence-pass).
        -- Splitting it across two ticks lets each stage fit naturally.
        --
        -- These two assignments are paired: scan_spots is the gate that
        -- prevents re-entry into the chunked block; pending is the gate
        -- that triggers the deferred shield-scan block.  Setting them
        -- together (and last) makes the whole block idempotent under
        -- budget abort — if we don't reach this point, scan_spots
        -- stays nil and next tick re-runs the cheap re-derivation
        -- (cached chunk, fresh greens/best) without losing the shield
        -- step.
        goal.scan_spots = spots
        -- A pill_suicider never builds blockers, so the shield scan has nothing
        -- to plan: skip it outright (leaving _shield_scan nil, exactly the state
        -- the existing DEMOTED(no-shield) path produces) and keep the standoff
        -- the spot-selection pass just chose. Also saves the scan's tick budget.
        -- A human standing close skips it for the same reason (see the
        -- PPT_SKIP block above): nothing will be built, so nothing needs
        -- planning, and the scan's tick budget goes unspent.
        goal._shield_scan_pending =
          (not (state.is_pill_suicider or goal._human_near)) or nil
      else
        local smx, smy = M.pick_standoff(world, info, pill, state)
        -- C.BLITZ_NOSPOT_RENEGOTIATE: the geometric fallback gets the same
        -- clash test as the scan spots.
        if smx then
          local cpn, cdd = M._blitz_spot_clash(
            M._blitz_ally_spots(state, goal, info.player_number or -1), smx + 0.5, smy + 0.5)
          if cpn then
            clear_attack_goal(state, string.format("blitz: fallback standoff (%d,%d) %.2f tiles from p%s's spot, renegotiate",
              smx, smy, cdd, tostring(cpn)))
            return
          end
        end
        goal.standoff_mx = smx
        goal.standoff_my = smy
        if smx then
          goal.standoff_fx = smx + 0.5
          goal.standoff_fy = smy + 0.5
          local dx = goal.standoff_fx - (pmx + 0.5)
          local dy = goal.standoff_fy - (pmy + 0.5)
          local d = math.sqrt(dx * dx + dy * dy)
          if d > 0.01 then
            local ux, uy = dx / d, dy / d
            goal.approach_fx = goal.standoff_fx + ux * C.ATTACK_APPROACH_OFFSET
            goal.approach_fy = goal.standoff_fy + uy * C.ATTACK_APPROACH_OFFSET
            goal.approach_mx = U.mclamp(math.floor(goal.approach_fx))
            goal.approach_my = U.mclamp(math.floor(goal.approach_fy))
          end
        end
        -- Fallback path: no shield scan needed (no winner from greens).
        -- Pair scan_spots assignment with the end of this branch so the
        -- block is idempotent under budget abort, same as the if-best
        -- branch above.
        goal.scan_spots = spots
      end
      end -- if spots (chunk done or cache hit)
    end -- if not goal.scan_spots (scan once)

    -- ── Deferred shield.scan ─────────────────────────────────────────
    -- Runs on the tick AFTER spot selection so the chunk-completion
    -- frame doesn't also have to pay for shield.scan + its post-
    -- processing (which can blow the per-tick budget at low capacity
    -- tiers).  Reads goal.standoff_*/goal._chosen_deg already set by
    -- the spot-selection pass; recomputes scan_radius from goal._is_ppt
    -- (no need to persist it).
    if goal._shield_scan_pending then
      goal._shield_scan_pending = nil
      local scan_radius = goal._is_ppt and C.PPT_STANDOFF
                          or C.ATTACK_PILL_STANDOFF
      local no_builder = (info.man_status == C.LGM_DEAD)
      local _cap = state._capacity
      local _sb_pos  = (_cap and _cap.sb_positions) or 28
      local _sb_step = (_cap and _cap.sb_step)      or 0.5
      -- Friendly pillboxes we can drop onto buildable shield slots = carried
      -- pills, capped at the per-shield pillbox budget. The shield scorer counts
      -- each as PPT_PILL_WALL_EQUIV walls of cover, so one carried pill can stand
      -- in for a 3-wall shield and the planner won't over-build walls.
      local _num_pill_blockers = math.min(info.carried_pills or 0,
                                          C.PPT_PILL_BLOCKERS_MAX or 2)
      -- Shield-scan cache: shield.scan is ~20ms. A stuck / re-planning bot can
      -- re-enter plan_position every tick (the goal is re-adopted each replan,
      -- which re-sets _shield_scan_pending), recomputing the SAME plan and
      -- burning full CPU every tick — dry bots that can't finish a take sat
      -- here 200-1100 ticks at 34-70ms. Reuse a recent result keyed on the
      -- scan's stable inputs; the key includes pill health + our armour +
      -- standoff, so it busts the instant the situation actually changes
      -- (pill damaged, we moved/took damage, switched pills). Cost is then paid
      -- at most once per SHIELD_SCAN_CACHE_TICKS instead of every tick.
      local _now = state.tick or 0
      local _scan_key = string.format("%d,%d,%d|%s,%s,%s|%s,%s|%s,%s,%s|%d",
        pill.mx, pill.my, pill.health or 0,
        tostring(goal.standoff_mx), tostring(goal.standoff_my),
        tostring(goal._chosen_deg or 0),
        tostring(goal.standoff_fx), tostring(goal.standoff_fy),
        tostring(scan_radius), tostring(no_builder), tostring(info.armour),
        _num_pill_blockers)
      local sscan
      local _sc = state._shield_scan_cache
      -- Budget-kill blacklist. A scan the budget hook kills mid-run unwinds
      -- the WHOLE think — no code after the call site executes, so the only
      -- way to notice repeated kills is a marker written BEFORE the call.
      -- After SHIELD_SCAN_BLACKLIST_TRIES incomplete attempts on the same
      -- scan key, stop retrying and take the no-shield plan (sscan = nil is
      -- the existing DEMOTED path) instead of livelocking on a scan that
      -- can't fit the budget. Marker resets when the key changes (pill hp /
      -- our armour / standoff moved) or after SHIELD_SCAN_BLACKLIST_TICKS.
      local _att = state._shield_scan_attempt
      if _att and (_att.key ~= _scan_key
                   or (_now - (_att.tick or 0)) > (C.SHIELD_SCAN_BLACKLIST_TICKS or 250)) then
        _att = nil
        state._shield_scan_attempt = nil
      end
      if _sc and _sc.key == _scan_key
         and (_now - (_sc.tick or -1000000)) < (C.SHIELD_SCAN_CACHE_TICKS or 25) then
        sscan = _sc.result
      elseif _att and (_att.count or 0) >= (C.SHIELD_SCAN_BLACKLIST_TRIES or 2) then
        sscan = nil
      else
        if _att then
          _att.count = (_att.count or 0) + 1
          _att.tick  = _now
        else
          state._shield_scan_attempt = { key = _scan_key, count = 1, tick = _now }
        end
        local _ok, sscan_or_err = xpcall(function()
          return shield.scan(pill, world,
                             goal.standoff_mx, goal.standoff_my,
                             goal._chosen_deg or 0,
                             goal.standoff_fx, goal.standoff_fy,
                             scan_radius, no_builder, info.armour,
                             _sb_pos, _sb_step, _num_pill_blockers,
                             -- Banned approach angles on THIS pill. The 5-degree
                             -- sweep already honours them; without passing them
                             -- here the shield scan nudges the standoff several
                             -- degrees along the standoff circle and lands back
                             -- inside the bucket the take just banned, so the
                             -- same spot gets "tried" twice and the take burns
                             -- its SANITY_PILL_REPLANS_MAX budget on one line
                             -- (20260901_032030 bot0: banned 140..150 at t=506,
                             -- deg=146.25 adopted at t=510, banned again t=531;
                             -- 146.25 is not a multiple of 5 -- that is the tell
                             -- that it came from the scan's nudge, not the sweep).
                             state.banned_pill_angles
                               and state.banned_pill_angles[pmy * 256 + pmx]
                               or nil,
                             state.tick or 0)
        end, debug.traceback)
        if _ok then
          state._shield_scan_attempt = nil
          sscan = sscan_or_err
          state._shield_scan_cache = { key = _scan_key, result = sscan, tick = _now }
        else
          local msg = tostring(sscan_or_err)
          -- Re-raise budget abort so the brain runtime sees its own signal
          -- and aborts the tick properly (the attempt marker survives —
          -- that's the blacklist counting the kill). Only catch genuine
          -- shield-scan bugs (everything else).
          if msg:find("tick_budget_exceeded", 1, true) then
            error(sscan_or_err)
          end
          -- Genuine crash, not a budget kill: the attempt DID complete
          -- (just badly) — clear the marker so the blacklist stays a
          -- budget-kill detector only.
          state._shield_scan_attempt = nil
          print(TAG .. " SHIELD SCAN CRASH:\n" .. msg)
          sscan = nil
        end
      end
      if sscan and (not sscan.best or (sscan.best.score or 0) <= 0) then
        goal._is_ppt = false
        sscan = nil
      end
      if sscan then sscan.created_tick = now end
      goal._shield_scan = sscan
      if sscan and sscan.best then
        local w = sscan.best
        -- Belt and braces on top of the filter inside shield.scan: never adopt a
        -- winner whose 5-degree bucket is banned on this pill. If one ever gets
        -- through (a cached scan from before the ban, a future scoring path that
        -- builds its own ring), keep the sweep's standoff -- unbanned by
        -- construction -- and say so.
        local _wb = state.banned_pill_angles
                    and state.banned_pill_angles[pmy * 256 + pmx]
        if _wb and w.deg then
          local _bucket = math.floor((w.deg % 360) / 5) * 5
          local _exp = _wb[_bucket]
          if _exp and (state.tick or 0) < _exp then
            w = nil
          end
        end
        if w then
        goal.standoff_mx = w.mx
        goal.standoff_my = w.my
        goal.standoff_fx = w.cx
        goal.standoff_fy = w.cy
        goal._chosen_deg = w.deg
        goal._scan_tick = state.tick
        local dxn = w.cx - (pmx + 0.5)
        local dyn = w.cy - (pmy + 0.5)
        local dn  = math.sqrt(dxn * dxn + dyn * dyn)
        if dn > 0.01 then
          local ux, uy = dxn / dn, dyn / dn
          goal.approach_fx = w.cx + ux * C.ATTACK_APPROACH_OFFSET
          goal.approach_fy = w.cy + uy * C.ATTACK_APPROACH_OFFSET
          goal.approach_mx = U.mclamp(math.floor(goal.approach_fx))
          goal.approach_my = U.mclamp(math.floor(goal.approach_fy))
        end
        local off = shield.AIM_OFFSETS_TILE_FIRE[w.best_aim_idx or 1]
                    or shield.AIM_OFFSETS_TILE_FIRE[1]
        goal.aim_mx = pmx + off[1]
        goal.aim_my = pmy + off[2]
        -- The shield scan moved the standoff, so the spot scan's aim is stale.
        -- Keep the world-unit aim in step with the corner the shield picked, or
        -- the GO gate and steering would still be testing the old line.
        goal.aim_wx  = bit.lshift(pmx, 8) + math.floor(off[1] * 256)
        goal.aim_wy  = bit.lshift(pmy, 8) + math.floor(off[2] * 256)
        goal.aim_idx = w.best_aim_idx or 1
        end  -- if w (winner not on a banned angle)
      end
    end

    -- Transition to approach after 3 ticks. The old 50-tick (1s) hold was
    -- for letting a human inspect the spot-scoring viz; planning is now
    -- reliable enough that we don't need the pause in normal play. 3 ticks
    -- is enough to be catchable when scrubbing a replay. Bump higher
    -- (e.g. 50 for 1 second) if the scoring viz needs live dwell time.
    if goal.standoff_mx and not goal._shield_scan_pending then
      if not goal._plan_show_tick then
        goal._plan_show_tick = now
      elseif (now - goal._plan_show_tick) >= 3 then
        -- Pre-flight tree check: PPT plans build wall_shields, each
        -- costs WALL_SHIELD_BUILD_COST trees. Better to gather BEFORE
        -- approaching — once at the standoff, the forest may be on the
        -- other side of the pill / a river / etc., and gathering then
        -- forces the tank to leave its hard-won standoff.
        local need_gather = false
        local trees_needed = 0
        local n_pots = 0
        -- CHARGE NOW: no tree gather.
        if goal._is_ppt and goal._shield_scan and goal._shield_scan.best
           and not goal._charge_now then
          local w = goal._shield_scan.best
          local pots = w.best_aim_idx and w.aims[w.best_aim_idx]
                       and w.aims[w.best_aim_idx].potential_blockers
          if pots and #pots > 0 then
            n_pots = #pots
            trees_needed = n_pots * (C.WALL_SHIELD_BUILD_COST or 2)
            if (info.trees or 0) < trees_needed then
              need_gather = true
            end
          end
        end
        if need_gather then
          goal.substate = "gather_trees"
          goal._trees_for_walls = trees_needed
          goal._gather_start    = now
          goal._gather_last_progress = now
          goal._gather_last_trees    = info.trees or 0
          print(string.format(TAG .. " ATTACK: plan_position -> gather_trees (%d/%d trees for %d walls)",
                info.trees or 0, trees_needed, n_pots))
        else
          local unsafe = armour_unsafe_for_pill_take(info, pill and pill.health, blitz_2plus, goal)
          if unsafe then
            clear_attack_goal(state, "abort@approach_entry — " .. unsafe)
            return
          end
          -- A person's attack order past HUMAN_ATTACK_BLITZ_WAIT_MAX_S does
          -- not wait for the accept any more: it approaches.
          if blitz_join_unaccepted(state, info) and not goal._hcap_over then
            -- Ally is blitzing this pill and we're not accepted yet — HOLD at
            -- plan_position (keep the standoff fresh, let squad negotiation run);
            -- don't approach uninvited. Flips to approach on accept, or re-plans
            -- when the call closes / we're rejected.
          else
            goal.substate = "approach"
            print(string.format(TAG .. " ATTACK: plan_position -> approach, standoff=(%d,%d) precise=(%.1f,%.1f)",
                  goal.standoff_mx, goal.standoff_my,
                  goal.standoff_fx or goal.standoff_mx + 0.5,
                  goal.standoff_fy or goal.standoff_my + 0.5))
          end
        end
      end
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- Substate-independent early GO — a commander still EN ROUTE to its standoff.
  -- The blitz_wait handler's early-GO only fires once the COMMANDER is parked,
  -- so a slow commander (planning / gathering trees / walking in) makes ready
  -- soldiers wait on it even when the squad already has the numbers. Here we
  -- check the whole SET's parked blitzers from ANY en-route substate: the
  -- commander isn't parked (contributes 0), so it takes SQUAD_BLITZ_GO_EARLY_READY
  -- SOLDIERS already in blitz_wait to trigger. On trigger we abandon the
  -- walls/PPT creep and rush in with them (overwhelm charge), broadcasting GO.
  if state.squad_role == "c" and goal._blitz and not goal._blitz_committed
     and BLITZ_EARLY_GO_ENROUTE_SUBS[goal.substate or ""] then
    local _wp = goal.target_id and world.pills and world.pills[goal.target_id] or nil
    if _wp and (_wp.health or 0) > 0 then
      local _t, _r, _mb, _un, _inwait = squad.blitz_ready_status(state, now, info.player_number or -1, info)
      -- Commander counts itself if it too is parked at a firing spot (any of the
      -- past-approach PPT substates: aim / in_range_* — blitz_wait is handled by
      -- its own branch, not this en-route path). Same BLITZ_READY_SUBS set the
      -- soldier tally uses, so the quorum is symmetric across the squad.
      local _self_parked = squad.BLITZ_READY_SUBS[goal.substate or ""] and 1 or 0
      local set_inwait = _self_parked + (_inwait or 0)
      if set_inwait >= squad.blitz_min() then
        local _prev = goal.substate
        goal._blitz_committed    = true
        goal._blitz_start_armour = info.armour or 0
        goal._blitz_party        = blitz_party_count(state, now, info.player_number or -1, goal.target_id)
        goal._blitz_party_tick   = now
        goal._is_ppt             = false        -- charge FAST like the soldiers, not PPT creep
        goal._charge_braking     = nil
        goal.substate            = "charge"
        goal._blitz_go           = true         -- broadcast GO (bgo) in init.lua
        state.squad_blitz_go     = true
        -- Quorum met -> this is a real GO, so top the party up to the blitz
        -- suicider minimum. Once per take (the goal field dies with the goal).
        if not goal._blitz_su_done then
          goal._blitz_su_done = true
          squad.blitz_designate_suiciders(state, info, now, goal.target_id)
        end
        -- ...and if an enemy tank is already sitting on the pill we're charging,
        -- the take is CONTESTED: everyone goes in as a suicider.
        blitz_contested_check(goal, state, world, info, now)
        return
      end
    end
  end

  -- ══════════════════════════════════════════════════════════════════
  -- gather_trees: hold position before approach, let LGM harvest nearby
  -- forest until we have enough trees for the planned shield walls.
  -- The builder's existing "gather" mode (set via builder.set_mode) does
  -- the actual work — including all the smarts about reachable forest,
  -- LGM_DEPLOY_DIST cap, and danger-safe path. We just gate the
  -- transition to approach.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "gather_trees" then
    local trees_have = info.trees or 0
    local trees_need = goal._trees_for_walls or 0
    -- Track tree-count progress so we can detect "no nearby forest"
    -- via a no-progress timeout instead of waiting the full hard cap.
    if trees_have ~= goal._gather_last_trees then
      goal._gather_last_trees    = trees_have
      goal._gather_last_progress = now
    end
    local stalled = (now - (goal._gather_last_progress or now)) > 250  -- ~5 s
    -- A person's attack order also times out at HUMAN_ATTACK_GATHER_MAX_S.
    local timed_out = (now - (goal._gather_start or now)) > (C.PPT_GATHER_TIMEOUT or 1500)
                      or M.human_gather_cap_over(goal, state, now)
    -- CHARGE NOW: stop gathering and go.
    if trees_have >= trees_need or goal._charge_now then
      local unsafe = armour_unsafe_for_pill_take(info, pill and pill.health, blitz_2plus, goal)
      if unsafe then
        clear_attack_goal(state, "abort@approach_entry — " .. unsafe)
        return
      end
      goal.substate = "approach"
      goal._trees_for_walls = nil
      goal._gather_start    = nil
      goal._gather_last_progress = nil
      goal._gather_last_trees    = nil
      print(string.format(TAG .. " ATTACK: gather_trees -> approach (%d trees stocked)", trees_have))
    elseif stalled or timed_out then
      -- Demote out of PPT entirely so the rest of the attack runs the
      -- legacy non-PPT flow (charge -> engage -> swerve, near-edge aim
      -- via the per-tick fallback in init.lua). Wiping _shield_scan
      -- frees init.lua's aim override to set aim_mx/aim_my from the
      -- pill-edge geometry instead of the corner the scan picked,
      -- which would otherwise be unprotected without walls.
      local unsafe = armour_unsafe_for_pill_take(info, pill and pill.health, blitz_2plus, goal)
      if unsafe then
        clear_attack_goal(state, "abort@approach_entry — " .. unsafe)
        return
      end
      goal.substate = "approach"
      goal._is_ppt = false
      goal._shield_scan = nil
      local why = stalled and "no nearby forest reachable" or "hard timeout"
      print(string.format(TAG .. " ATTACK: gather_trees -> approach (%s, %d/%d trees) — demoted to no-shield",
            why, trees_have, trees_need))
      goal._trees_for_walls = nil
      goal._gather_start    = nil
      goal._gather_last_progress = nil
      goal._gather_last_trees    = nil
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- Per-tick standoff shot-path sanity check.
  -- Runs for every pre-fire substate where the standoff is committed
  -- but the shot hasn't been taken yet.  Catches pills placed in the
  -- corridor (e.g. by build_pill_strategic racing with build_walls)
  -- and ≥2-wall pile-ups without waiting for LGM to return to tank.
  -- ══════════════════════════════════════════════════════════════════
  do
    local sub = goal.substate
    if pill and goal.standoff_fx and
       (sub == "approach"         or sub == "build_walls"       or
        sub == "aim"              or sub == "in_range_position"  or
        sub == "in_range_aim_pre" or sub == "in_range_aim"       or
        sub == "in_range_aim_finetune") then
      -- Rate-limit to once per ~0.5 s (25 ticks @ 50 Hz).
      goal._sanity_check_tick = goal._sanity_check_tick or 0
      local reason
      if now - goal._sanity_check_tick >= 25 then
        goal._sanity_check_tick = now
        reason = standoff_shot_obstacle(goal, pill, world)
      end
      if reason then
        -- Step 1 of the blocked-line ladder (loss_b6 bot3 t=9483): before giving
        -- up this spot, ask whether a DIFFERENT point on the same pill — another
        -- corner, or the centre — is shootable from the standoff we already
        -- picked. Same origin the sanity check itself uses (the planned spot, not
        -- the moving tank), so a "yes" is a line this spot can genuinely hold.
        local ladder = try_reaim(state, goal, world,
                                 math.floor(goal.standoff_fx * 256 + 0.5),
                                 math.floor(goal.standoff_fy * 256 + 0.5),
                                 pill.mx, pill.my, now, "SANITY", reason)
        if ladder ~= "blocked" then
          -- Re-aimed (or we searched a moment ago and are letting the gun catch
          -- up): the take carries on from the same spot, no replan at all.
          reason = nil
          if ladder == "ok" and
             (sub == "in_range_aim" or sub == "in_range_aim_pre" or
              sub == "in_range_aim_finetune") then
            -- The PPT aim substates latch _aim_locked / _pre_aim_locked against
            -- the OLD corner. Moving the aim without dropping those would let
            -- in_range_aim advance on a stale lock (and finetune keep tapping
            -- off a heading it verified for a line we just abandoned), so restart
            -- the aim leg on the new corner.
            goal.substate              = "in_range_aim"
            goal.aim_tick              = now
            goal._aim_locked           = nil
            goal._pre_aim_locked       = nil
            goal._finetune_taps        = 0
            goal._finetune_start       = nil
            goal._finetune_reached_tap = nil
            goal._finetune_path        = nil
            goal._finetune_on_pill     = nil
          end
        end
      end
      if reason then
        print(string.format(TAG ..
          " SANITY: shot path blocked (%s) in %s — replanning", reason, sub))
        -- Step 2. A PILL in the line does not move: replanning to the same angle
        -- just loops (20260831_092854 bot2: four SANITY_REPLANs on pill 6 at
        -- (127,139), same standoff every time, then a blocker dropped beside
        -- it and the LGM stranded). Ban the chosen angle bucket and its two
        -- neighbours on this pill -- same mechanism as the approach timeout,
        -- which the spot scan honours -- so the next plan_position picks a
        -- line the pill is not in. Give the take up only after
        -- SANITY_PILL_REPLANS_MAX different SPOTS have failed with no clear aim
        -- on any of them.
        if reason:sub(1, 7) == "pill at" then
          -- Ladder step 1 already ran above and found NO clear aim from this
          -- standoff. Tag it the way the three firing sites tag theirs, so
          -- "the ladder ran" is one greppable shape across all four.
          local abandon, bucket, tries =
            blocked_line_replan(state, goal, pill.mx, pill.my, now)
          if bucket then
          end
          if abandon then
            clear_attack_goal(state, "shot path blocked by our own pill on every tried angle")
            return
          end
        else
          -- 2+ walls on the line: never counted as a spot tried and never
          -- abandons the take (unchanged) — just replan.
          reset_to_plan_position(state, goal)
        end
      end
    end
  end

  -- ══════════════════════════════════════════════════════════════════
  -- kill_hardline: nav to a tile beside the pill and fire on every clear
  -- shot until it's dead. Steering (attack_pill_steer) owns the navigation,
  -- neighbour-tile selection, and firing; this block only handles the two
  -- terminal conditions — pill dead, or steering signalled it can't reach
  -- any tile beside the pill (goal._hardline_abort).
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "kill_hardline" then
    if goal._hardline_abort then
      -- CHARGE NOW: no tile beside the pill.  Do not drop the goal (the
      -- same pill would be picked again and the charge would restart, over
      -- and over); take the careful charge-now way from plan_position
      -- (standoff, aim, fire, no waits, no new blockers).  orders.lua does
      -- not restart a human-shooting charge on this pill for a while.
      if goal._charge_now then
        state._charge_now_abort = { tid = goal.target_id, t = now }
        goal._charge_now_careful = true
        goal._kill_rush, goal._kill_rush_decided = nil, nil
        goal._hardline_abort, goal._hardline_bad = nil, nil
        goal._hardline_mx, goal._hardline_my = nil, nil
        reset_to_plan_position(state, goal)
        return
      end
      clear_attack_goal(state, "kill_hardline abort: " .. tostring(goal._hardline_abort))
      return
    end
    if not pill or (pill.health or 0) <= 0 then
      clear_attack_goal(state, "kill_hardline: pill dead")
      return
    end
    -- nothing else to do — steering drives + fires; fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- approach: navigate to standoff position, brake to stop
  -- ══════════════════════════════════════════════════════════════════
  -- ══════════════════════════════════════════════════════════════════
  -- blitz_wait: in position at our engage spot. Hold (issue no drive
  -- command so the tank stays put) and run the squad GO handshake.
  --   * soldier — reports rdy=1 (broadcast in init.lua) and watches the
  --     commander's GO key (bgo). On GO → commit and charge.
  --   * commander — waits until every live squad soldier is in blitz_wait
  --     (rdy=1), or the SQUAD_BLITZ_READY_TIMEOUT elapses, then broadcasts GO
  --     and charges.
  -- Pre-commit commander-death abort is handled at the top of update().
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "blitz_wait" then
    state.squad_blitz_in_position = true
    -- "Aimed" gate for our readiness report: we only tell the commander we're
    -- ready (rdy=1, broadcast in init.lua) once we're not just in position but
    -- actually FACING the pill within SQUAD_BLITZ_AIM_TOL brad. The blitz_wait
    -- hold turns us onto the pill over a few ticks; until that converges we hold
    -- back rdy, so on GO every soldier can fire/charge immediately rather than
    -- burning the strike window spinning to face the target.
    do
      -- Measure against the aim we will actually fire down (centre or the corner
      -- the spot scan picked), so readiness agrees with where the gun is turning.
      local _atx = goal.aim_wx and (goal.aim_wx / 256.0) or (pmx + 0.5)
      local _aty = goal.aim_wy and (goal.aim_wy / 256.0) or (pmy + 0.5)
      local _ad = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0, _atx, _aty)
      state.squad_blitz_aimed = math.abs(U.adiff(info.direction, _ad)) <= (C.SQUAD_BLITZ_AIM_TOL or 8)
    end

    -- Target pill gone (someone else killed it / it died) → nothing to GO for.
    -- Abort for a clean re-pick. Applies to commander AND soldier.
    local _wp = goal.target_id and world.pills and world.pills[goal.target_id] or nil
    if not (_wp and (_wp.health or 0) > 0) then
      clear_attack_goal(state, "blitz_wait: target pill gone")
      return
    end

    -- Commit to firing on GO. If we BUILT a shield (goal._blitz_shielded — only a
    -- no-joiner full-pill blitz does) AND we're still effectively solo, fire from
    -- BEHIND it via the normal aim → in_range PPT path; it must NOT charge into its
    -- own walls. Every other case charges in — INCLUDING a true 2+ tank blitz
    -- (blitz_2plus): with an ally rushing we overwhelm together instead of the
    -- commander hanging back threading its shield while the soldier charges alone.
    local function commit_fire()
      -- ── GO gate: never commit to a charge line we cannot shoot down. ──
      -- 20260831_173448 bot2 t6150: solo blitz on pill #4 at (124,139), we GO'd
      -- from (133,141) with our OWN pill #6 sitting at (126,139) dead on the
      -- line. The charge's per-tick shot-path check caught it one tick later
      -- (CHARGE_ABORT_OBSTACLE) and binned the whole take. Check it HERE, before
      -- we enter charge, with the SAME simulation the charge aborts on, so we
      -- never target a line we can't hit.
      --   * Only the charging branch needs this. The shielded/PPT branch below
      --     fires from behind its walls and threads its own corner aim.
      --   * We test the aim the PLANNER chose (goal.aim_wx/aim_wy — pill centre
      --     or the corner the spot scan proved shootable from this spot), not
      --     the pill centre, because that is the line the charge now steers
      --     down. Falls back to the centre when no aim was planned.
      --   * Still re-checked at the GO moment against the LIVE world: an ally
      --     can drop a pill into the corridor long after our standoff was
      --     chosen, so screening at spot-selection time alone is not enough.
      --     The scan hard-rejects blocked spots now, so this should fire rarely
      --     instead of looping (20260831_222819 bot2: 467 blocked GOs).
      -- A wall on the line is fine (shells clear it) — only the impassable set
      -- shot_path_obstacle_count returns math.huge for (a live deployed pill)
      -- stops us.
      if not (goal._blitz_shielded and goal._shield_scan) then
        local aim_wx = goal.aim_wx or bit.bor((bit.lshift(pmx, 8)), 128)
        local aim_wy = goal.aim_wy or bit.bor((bit.lshift(pmy, 8)), 128)
        -- Where the test shell starts. A soldier is NOT parked at its standoff
        -- here: it waits at its SETUP point, ATTACK_APPROACH_OFFSET (2.25 tiles)
        -- further out along the same ray, and fires only after the charge has
        -- carried it in to the standoff. KEEL tested from the live tank (the
        -- SETUP point), a different line from the one the charge fires down
        -- (20260924_224514 bot0: SETUP (127.52,146.37) missed our pill #15 by
        -- 0.01 tile, the standoff line ran into it). With
        -- C.BLITZ_GO_GATE_FROM_STANDOFF the soldier tests from its planned
        -- standoff float point instead. The commander keeps the live position.
        local gox, goy = blitz_go_origin(info, goal)
        local go_obs, go_why = shot_path_obstacle_count(info, goal, world, aim_wx, aim_wy, gox, goy)
        if go_obs == math.huge then
          -- Step 1 of the blocked-line ladder: a blocked CENTRE (or blocked
          -- planned corner) says nothing about the pill's other three corners.
          -- Try them from the same origin before touching the plan (loss_b6
          -- bot3 t=9483 is the charge-side twin of this: a neighbouring pill
          -- clipped one aim line and binned the take).
          local ladder = try_reaim(state, goal, world, gox, goy,
                                   pmx, pmy, now, "BLITZ_GO", go_why)
          if ladder ~= "blocked" then
            -- Re-aimed on the spot (or holding while the gun swings onto the new
            -- corner): GO stands, fall through and commit.
            go_obs = 0
          end
        end
        -- Blitz LOS margin (C.BLITZ_SPOT_LOS_MARGIN > 0): the line must also
        -- keep its tapering distance from walls / pills / bases. Re-aims onto
        -- another aim point that passes both tests; nil when none does.
        if go_obs ~= math.huge and goal._blitz then
          local mwhy = blitz_go_margin(state, goal, world, gox, goy, pmx, pmy, now)
          if mwhy then go_obs, go_why = math.huge, mwhy end
        end
        if go_obs == math.huge then
          -- Step 2. Ban this approach angle the way SANITY_BAN does — the
          -- blocker is a pill, it will not move, so replanning on the same line
          -- just loops — and go back to plan_position on the SAME pill for a
          -- fresh SPOT.
          -- Escalate like SANITY_ABANDON. A friendly pill sitting between us and
          -- the target blocks every line from this side, and plan_position kept
          -- re-picking the same eastern spot every 10 ticks (loss_b2 bot2: 467
          -- BLITZ_GO_BLOCKED on pill (128,139), ally pill at (130,139), ~90 s
          -- burned). After SANITY_PILL_REPLANS_MAX SPOTS with no clear aim on
          -- any of them, give the take up and blacklist the pill for a while.
          -- The counter is _sanity_pill_replans, shared with the standoff sanity
          -- check and the two firing substates: one "spots tried" number for the
          -- whole take rather than one per site.
          local abandon, bucket, tries =
            blocked_line_replan(state, goal, pmx, pmy, now)
          if abandon then
            clear_attack_goal(state, "blitz GO blocked by a friendly pill on every tried line")
            return
          end
          return
        end
      end
      goal._blitz_committed    = true
      goal._blitz_start_armour = info.armour or 0  -- baseline for damage-gated swerve
      goal._blitz_party        = blitz_party_count(state, now, info.player_number or -1, goal.target_id)
      goal._blitz_party_tick   = now
      if goal._blitz_shielded and goal._shield_scan then
        goal.substate = "aim"
        goal.aim_tick = now
        goal._aim_locked = nil
        if blitz_2plus then
          -- Ally is here and rushing. Broadcast GO RIGHT AWAY — we no longer make
          -- the ally wait for us to be in position. But we STILL thread the full
          -- PPT in_range procedure to the EXACT engage spot behind the shield:
          -- firing from off-spot risks our own shots clipping the blockers. Only
          -- the wait is dropped, not the precise positioning.
          goal._blitz_go = true
          state.squad_blitz_go = true
        end
        -- Solo shielded (no ally): GO stays DEFERRED to shoot_pill entry (see the
        -- shoot_pill handler). A shielded PPT commander spends many ticks threading
        -- aim → in_range_aim_pre → in_range_aim → shoot_pill before it can fire;
        -- broadcasting GO now would send a lone late soldier charging in long
        -- before we're actually shooting from behind the shield to support it.
      else
        -- Overwhelm charge: no shield to thread around, so charge FAST like the
        -- soldiers. Clear _is_ppt — otherwise steering's charge uses the slow PPT
        -- creep (PPT_CHARGE_MAX_SPEED) meant for landing precisely behind a wall,
        -- and the commander crawls in while the soldiers rush (saw it stall:
        -- 20260604_165828 bot0 t3734).
        goal._is_ppt = false
        goal._charge_braking = nil
        goal.substate = "charge"
        goal._blitz_go = true   -- charge is immediate → GO now so we all rush together
      end
    end

    -- CHARGE NOW (attack ping): no wait for the handshake.  A commander sends
    -- GO so its soldiers go in with it; a soldier goes without the GO.
    if goal._charge_now then
      if state.squad_role == "c" then state.squad_blitz_go = true end
      commit_fire()
      return
    end

    -- Pill softened below the blitz threshold while we waited: a coordinated
    -- overwhelm is overkill for a near-dead pill, so stop waiting for the GO
    -- handshake and just finish it (solo). commit_fire broadcasts GO so any
    -- joiner stops waiting too. Only pre-fire (commit_fire latches it).
    -- Blitz-only (squad.blitz_only): no "finish it solo" -- keep waiting for GO.
    if _wp and (_wp.health or 0) > 0 and (_wp.health or 0) < (C.HARD_TAKE_MIN_HP or 12)
       and not squad.blitz_only(state) then
      goal._blitz = false
      goal._blitz_solo = true
      state.squad_blitz_go = true
      commit_fire()
      return
    end

    -- Already committed and back here? Then the blocked-line ladder sent us from
    -- charge / shoot_pill / the GO gate to plan_position, we re-approached, and
    -- the in-position decision routed us into blitz_wait a second time. GO is
    -- already out — the soldiers are charging — so do NOT run the handshake
    -- again and make everyone wait on a commander who has already said go.
    -- Re-run the fire gate straight away from the new standoff.
    if goal._blitz_committed then
      commit_fire()
      return
    end

    if state.squad_role == "c" then
      local total, ready, min_bd, any_unseen, inwait = squad.blitz_ready_status(state, now, info.player_number or -1, info)
      -- Blitz-only (squad.blitz_only): nobody answering is NOT a solo GO. Fall
      -- through to the wait below: a joiner can still arrive. At READY_TIMEOUT
      -- with party=1 < MIN: without BLITZ_ONLY_EXTEND_WAIT the take is given
      -- up (BLITZ_ABANDON_SHORT); with it the wait is extended up to
      -- BLITZ_ONLY_EXTEND_MAX times first, then given up the same way
      -- (2026-09-26: before the cap it extended forever).
      -- HUMAN_NEAR_BLITZ_CAP: a human team-mate within
      -- ORDER_HUMAN_NEAR_SUICIDE_TILES (goal._human_near) and no soldier at
      -- all: go now, blitz-only or not.
      local _hn_solo = C.HUMAN_NEAR_BLITZ_CAP and goal._human_near and total == 0
      if total == 0 and (not squad.blitz_only(state) or _hn_solo) then
        -- Nobody (left) answering. If we SKIPPED walls for a joiner who is now
        -- gone (full-pill PPT, no shield built), degrade to a normal SOLO
        -- PROTECTED take: re-approach so the in-position decision builds the
        -- shield (no joiner now → it won't skip). Otherwise (shield already up,
        -- or a soft pill) just GO solo immediately — nobody to wait for.
        if goal._is_ppt and not goal._blitz_shielded then
          goal._blitz             = false
          goal._blitz_solo        = true
          goal._blitz_committed   = nil
          goal._blitz_ready_since = nil
          state.squad_blitz_engage_mx, state.squad_blitz_engage_my = nil, nil
          state.squad_blitz_engage_fx, state.squad_blitz_engage_fy = nil, nil
          state.squad_blitz_engage_deg  = nil
          state.squad_blitz_bd          = nil
          state.squad_blitz_in_position = nil
          goal.substate = "approach"
          return
        end
        state.squad_blitz_go = true
        commit_fire()
        return
      end
      if not goal._blitz_ready_since then
        goal._blitz_ready_since   = now
        goal._blitz_prog_tick     = now      -- last progress check
        goal._blitz_prog_bd       = min_bd   -- closest pending soldier's walk dist then
        goal._blitz_timeout_ext   = 0        -- accumulated patience extension
      end
      -- Progress-based patience: every PROGRESS_CHECK ticks, if the closest
      -- still-approaching soldier got CLOSER to its standoff (min bd dropped),
      -- grant another PROGRESS_CHECK ticks. A soldier still closing in keeps the
      -- commander waiting; one that stalls (no bd drop) stops extending, so the
      -- base READY_TIMEOUT still fires and we don't wait on a stuck tank forever.
      -- min_bd is measured from the soldier's LIVE position whenever we can see
      -- its tank (every tick, exact); for an out-of-sight soldier we fall back to
      -- its broadcast bd, which only refreshes coarsely — so as the deadline
      -- nears we directly QUERY any unseen soldier (bwq) for a fresh position and
      -- grant one more window if the answer shows it's still closing.
      local CHK = C.SQUAD_BLITZ_PROGRESS_CHECK or 100
      if (now - (goal._blitz_prog_tick or now)) >= CHK then
        if min_bd and goal._blitz_prog_bd and min_bd < goal._blitz_prog_bd then
          goal._blitz_timeout_ext = (goal._blitz_timeout_ext or 0) + CHK
        end
        goal._blitz_prog_bd   = min_bd
        goal._blitz_prog_tick = now
      end
      -- Final-window "where are you?" query for unseen pending soldiers.
      local eff       = (C.SQUAD_BLITZ_READY_TIMEOUT or 150) + (goal._blitz_timeout_ext or 0)
      local remaining = eff - (now - goal._blitz_ready_since)
      local LEAD      = C.SQUAD_BLITZ_QUERY_LEAD or 50
      local querying  = false
      if any_unseen and remaining > 0 and remaining <= LEAD then
        querying = true
        -- (Re)latch the broadcast through the window so the soldier sees it; reset
        -- the one-shot extension gate each fresh window.
        if not state._blitz_query_until or now > state._blitz_query_until then
          state._blitz_query_pid    = goal.target_id
          state._blitz_query_until  = now + LEAD
          goal._blitz_query_checked = false
        end
        -- One extension per query if the (freshly answered) min_bd shows progress.
        if not goal._blitz_query_checked and min_bd and goal._blitz_prog_bd
           and min_bd < goal._blitz_prog_bd then
          goal._blitz_timeout_ext   = (goal._blitz_timeout_ext or 0) + CHK
          goal._blitz_prog_bd       = min_bd
          goal._blitz_query_checked = true
        end
      else
        state._blitz_query_pid = nil   -- not in the window → stop asking
      end
      eff = (C.SQUAD_BLITZ_READY_TIMEOUT or 150) + (goal._blitz_timeout_ext or 0)
      local timed_out = (now - goal._blitz_ready_since) >= eff
      -- Heartbeat: why are we still waiting? (print is a no-op in the brain, so
      -- the GO decision was previously invisible in print2 logs.)
      -- Early GO on critical mass: once ENOUGH of the SET (commander + all
      -- soldiers) are PARKED at their standoffs (sub=blitz_wait), fire GO now
      -- instead of waiting for stragglers or the timeout. We're in blitz_wait
      -- here, so the commander counts itself (+1); inwait is the soldiers parked
      -- at their spots. The threshold is the per-bot party MIN (squad.blitz_min,
      -- default 2 = commander + 1 parked soldier already goes); a
      -- still-approaching extra joins on the broadcast GO. (Mirrors the
      -- substate-independent pre-dispatch check that lets a commander still en
      -- route GO when 2 soldiers are already waiting on it.)
      local bmin       = squad.blitz_min()
      -- The party MIN is a floor on: the commander plus every soldier that has
      -- COMMITTED to this take (total). Committed, not ready — at the timeout
      -- the stragglers are by definition not ready yet, and requiring MIN READY
      -- there would make the timeout unusable. At the default MIN of 2 this is
      -- always satisfied (the total==0 case returned above), so nothing changes
      -- unless a "blitz=" token raised it.
      local party      = 1 + (total or 0)
      local set_inwait = 1 + (inwait or 0)
      local early_go = set_inwait >= bmin
      -- Blitz-only hold (C.BLITZ_ONLY_EXTEND_WAIT, 2026-09-25, Andrew): with no
      -- soloers allowed and a party MIN set, the only GO is the PARKED set
      -- (commander + soldiers at their spots in a ready substate = set_inwait)
      -- reaching MIN. Committed-but-still-driving soldiers do not count, and
      -- the timeout neither charges nor abandons: it adds one more
      -- SQUAD_BLITZ_READY_TIMEOUT to the wait. The call stays open in
      -- blitz_wait, so recruiting goes on meanwhile. At most
      -- BLITZ_ONLY_EXTEND_MAX extensions (2026-09-26), then the take is given
      -- up below (BLITZ_ABANDON_SHORT).
      local bo_hold = squad.blitz_only(state) and C.BLITZ_ONLY_EXTEND_WAIT
      local verdict = M.blitz_cmdr_go_verdict(bo_hold, timed_out, ready, total, party, set_inwait, bmin,
                                              goal._blitz_only_ext_n or 0, C.BLITZ_ONLY_EXTEND_MAX or 3)
      -- HUMAN_NEAR_BLITZ_CAP: with a human team-mate near (goal._human_near)
      -- the commander waits at most HUMAN_ATTACK_BLITZ_WAIT_MAX_S at its spot.
      if verdict ~= "go" and C.HUMAN_NEAR_BLITZ_CAP and goal._human_near
         and (C.HUMAN_ATTACK_BLITZ_WAIT_MAX_S or 0) > 0
         and (now - goal._blitz_ready_since) >= C.HUMAN_ATTACK_BLITZ_WAIT_MAX_S * 50 then
        verdict = "go"
      end
      -- A person's attack order past HUMAN_ATTACK_BLITZ_WAIT_MAX_S: GO now,
      -- whoever is parked. No more waiting, no blitz-only extension, and no
      -- short-handed abandon of the take the person asked for.
      if goal._hcap_over and verdict ~= "go" then
        verdict = "go"
      end
      if verdict == "extend" then
        local add = C.SQUAD_BLITZ_READY_TIMEOUT or 150
        goal._blitz_timeout_ext = (goal._blitz_timeout_ext or 0) + add
        goal._blitz_only_ext_n  = (goal._blitz_only_ext_n or 0) + 1
        return
      end
      -- Short-handed at the deadline: MIN is HARD, so do NOT charge with fewer
      -- tanks than the blitz asks for. Give up the take instead — clearing the
      -- attack goal also drops our blitz standoff/claim state, which closes the
      -- open call (init.lua broadcasts bcc on the transition) and frees any
      -- soldier still holding for a GO that is never coming. The next replan
      -- picks a fresh goal; if this pill still looks worth a blitz the call
      -- reopens, which is also the recruiting window a third tank needs.
      -- Blitz-only with the extensions used up (BLITZ_ONLY_EXTEND_MAX) ends
      -- here too, with party possibly >= min but parked < min.
      if verdict == "abandon" then
        clear_attack_goal(state, string.format("blitz_wait: READY_TIMEOUT short-handed (party=%d parked=%d < min=%d, ext=%d)", party, set_inwait, bmin, goal._blitz_only_ext_n or 0))
        return
      end
      if verdict == "go" then
        state.squad_blitz_go = true        -- broadcast GO (bgo) in init.lua
        -- Same moment the quorum is met (NOT the short-handed abandon above):
        -- designate random soldiers until the blitz has BLITZ_MIN_SUICIDERS
        -- suiciders. Once per take.
        if not goal._blitz_su_done then
          goal._blitz_su_done = true
          squad.blitz_designate_suiciders(state, info, now, goal.target_id)
        end
        -- ...and if an enemy tank is already sitting on the pill we're charging,
        -- the take is CONTESTED: everyone goes in as a suicider.
        blitz_contested_check(goal, state, world, info, now)
        commit_fire()
      end
      return
    else
      -- Soldier: hold for the commander's GO, but never freeze forever. Abort if
      -- the commander is no longer a live leader of THIS take:
      --   * its tank died (tank_dead_at, fast),
      --   * it retargeted / dropped attack_pill, or went silent/disconnected
      --     (slot inactive at ally-expiry), checked via its broadcast,
      --   * or the hard SQUAD_BLITZ_WAIT_TIMEOUT backstop elapses (commander
      --     silently stuck). The squad layer clears our commitment on the same
      --     signals (tripping the top-of-update abort); this is the in-state
      --     guarantee we don't stand frozen.
      local cmdr  = state.squad_cmdr
      local cslot = cmdr and ally_state.get(cmdr) or nil
      local cdead = cmdr and state.tank_dead_at and state.tank_dead_at[cmdr]
                    and cslot and state.tank_dead_at[cmdr] > (cslot.last_tick or 0)
      local con_take = cslot and cslot.active and cslot.info
                       and cslot.info.goal == "attack_pill"
                       and tonumber(cslot.info.target or "") == goal.target_id
      if not cmdr or cdead or not con_take then
        clear_attack_goal(state, "blitz_wait: commander gone/retargeted/dead")
        return
      end
      if not goal._blitz_wait_since then
        goal._blitz_wait_since = now
        goal._blitz_wait_entry = now   -- first blitz_wait tick (the follow log reads it)
      end
      -- C.BLITZ_SOLDIER_WAIT_FOLLOW_CMDR: under blitz-only the commander
      -- extends its own wait (BLITZ_WAIT_EXTEND, at most BLITZ_ONLY_EXTEND_MAX
      -- times, then it clears its goal and sends bcc). While it still
      -- has the call open on our pill and names us in bac, restart our
      -- backstop from its last message, so we do not time out under a
      -- commander that is still waiting for the set. Silent / call closed /
      -- dropped us = no restart, and the backstop runs from there.
      if M.blitz_soldier_wait_follow(state, goal, cslot, cmdr, info.player_number or -1, now) then
        goal._blitz_wait_since = cslot.last_tick
      end
      if (now - goal._blitz_wait_since) > (C.SQUAD_BLITZ_WAIT_TIMEOUT or 1500) then
        clear_attack_goal(state, "blitz_wait: GO never arrived (timeout)")
        return
      end
      local go = ally_state.get_key(cmdr, "bgo")
      -- Missed-GO fallback: bgo is a one-shot broadcast (and a shielded/PPT
      -- commander only sends it at shoot_pill entry), so it's easy to miss. If we
      -- never saw it but the commander's OWN broadcast shows it's already FIRING
      -- on our pill (shoot_pill / charge / engage / swerve), the GO happened and
      -- we missed it — commit now instead of waiting out SQUAD_BLITZ_WAIT_TIMEOUT.
      -- (aim / in_range_* are pre-fire — bgo isn't sent yet — so we keep holding.)
      if go ~= "1" then
        local csub = cslot.info.sub
        if csub == "shoot_pill" or csub == "charge" or csub == "engage" or csub == "swerve" then
          go = "1"
        end
      end
      -- HUMAN_NEAR_BLITZ_CAP: the same cap for a soldier with a human
      -- team-mate near.
      if go ~= "1" and C.HUMAN_NEAR_BLITZ_CAP and goal._human_near
         and (C.HUMAN_ATTACK_BLITZ_WAIT_MAX_S or 0) > 0
         and (now - goal._blitz_wait_since) >= C.HUMAN_ATTACK_BLITZ_WAIT_MAX_S * 50 then
        go = "1"
      end
      -- A person's attack order past HUMAN_ATTACK_BLITZ_WAIT_MAX_S: do not
      -- wait for the commander's GO any more; go in now.
      if go ~= "1" and goal._hcap_over then
        go = "1"
      end
      if go == "1" then
        commit_fire()
      end
      return
    end
  end

  if goal.substate == "approach" then
    -- Armour minimums are re-enforced EVERY tick on approach, not just at join /
    -- build-walls entry: if we take fire on the way in and drop below the floor,
    -- ABORT the take instead of diving in weak.
    --   * Carrying a pillbox is cautious mode — same floor as commanding
    --     (SQUAD_COMMANDER_MIN_ARMOUR); don't risk the pill we're holding.
    --   * The pill-HP-relative unsafe floor (armour_unsafe_for_pill_take) still
    --     exempts a 2+ tank blitz, where the partner shares the incoming fire.
    --   * CHARGE NOW IGNORES SAFETY waives both floors.
    if (info.carried_pills or 0) >= 1
       and (info.armour or 0) < (C.SQUAD_COMMANDER_MIN_ARMOUR or 30)
       and not (goal._charge_now and C.CHARGE_NOW_IGNORE_SAFETY) then
      clear_attack_goal(state, string.format("approach abort: carrying pill, armour %d < %d",
        info.armour or 0, C.SQUAD_COMMANDER_MIN_ARMOUR or 30))
      return
    end
    local _appr_unsafe = armour_unsafe_for_pill_take(info, pill and pill.health, blitz_2plus, goal)
    if _appr_unsafe then
      clear_attack_goal(state, "approach abort: " .. _appr_unsafe)
      return
    end
    -- C.BLITZ_SOLDIER_JOIN_CHARGE: the commander charged (bgo seen, or its own
    -- broadcast shows a firing substate) while this soldier is still driving
    -- to its setup point.
    --   * At least as close to the pill (Euclidean, tank to pill centre) as
    --     its setup point is: join the charge now.  blitz_wait's soldier
    --     branch sees the GO on the same think and commits (commit_fire).
    --   * Further out: drop the goal, so goal selection picks whatever fits.
    --     state._blitz_charge_missed remembers the pill.  When goal selection
    --     hands the SAME pill back while that charge still runs (a person's
    --     order on the pill, or the blitz row again), the new goal joins the
    --     charge at once instead of dropping again: no loop, and no wait at
    --     a setup point the charge has already left behind.
    if C.BLITZ_SOLDIER_JOIN_CHARGE and goal._blitz and not goal._blitz_committed
       and state.squad_role == "s" and state.squad_cmdr then
      local cslot = ally_state.get(state.squad_cmdr)
      local ci = cslot and cslot.active and cslot.info
      if ci and ci.goal == "attack_pill" and tonumber(ci.target or "") == goal.target_id then
        local csub = ci.sub
        local charged = ally_state.get_key(state.squad_cmdr, "bgo") == "1"
          or csub == "charge" or csub == "shoot_pill" or csub == "engage"
          or csub == "swerve"
        if charged then
          local pcx, pcy = pmx + 0.5, pmy + 0.5
          local sx = goal.approach_fx or goal.standoff_fx
          local sy = goal.approach_fy or goal.standoff_fy
          local dt = math.sqrt((info.tankx / 256.0 - pcx) ^ 2 + (info.tanky / 256.0 - pcy) ^ 2)
          local ds = sx and sy and math.sqrt((sx - pcx) ^ 2 + (sy - pcy) ^ 2) or nil
          local miss = state._blitz_charge_missed
          local rejoin = miss and miss.tid == goal.target_id
                         and now - miss.tick <= (C.SQUAD_BLITZ_WAIT_TIMEOUT or 1500)
          if rejoin or (ds and dt <= ds) then
            state._blitz_charge_missed = nil
            goal.substate = "blitz_wait"
            state.squad_blitz_in_position = true
            return
          elseif ds then
            state._blitz_charge_missed = { tid = goal.target_id, tick = now }
            clear_attack_goal(state, "blitz soldier too far when the commander charged")
            return
          end
        end
      end
    end
    local _t_app0 = BRAIN_PROFILE and clock_us() or 0
    if not goal.standoff_mx then
      goal.substate = "plan_position"
      goal.scan_spots = nil
      goal._shield_scan = nil
      goal._shield_scan_pending = nil
      goal._plan_show_tick = nil
      goal._plan_logged = nil
      print(TAG .. " ATTACK: approach has no standoff, replanning")
    else
      -- First-tick init for approach progress timer (used for the
      -- timeout below). Reset on every entry from another substate.
      if not goal._approach_start then
        goal._approach_start = now
        goal._approach_last_progress = now
        goal._approach_last_dist = nil
      end

      -- Reach the approach point (precise float, 1.5 tiles behind standoff).
      -- Tolerance: 64 wu (1/4 tile) AND speed <= 4.
      if not goal.approach_fx and not goal.approach_mx then
        -- Recover: compute approach point from standoff on the fly.
        if goal.standoff_mx then
          local sfx = goal.standoff_fx or (goal.standoff_mx + 0.5)
          local sfy = goal.standoff_fy or (goal.standoff_my + 0.5)
          local dx = sfx - (pmx + 0.5)
          local dy = sfy - (pmy + 0.5)
          local d = math.sqrt(dx * dx + dy * dy)
          if d > 0.01 then
            local ux, uy = dx / d, dy / d
            goal.approach_fx = sfx + ux * C.ATTACK_APPROACH_OFFSET
            goal.approach_fy = sfy + uy * C.ATTACK_APPROACH_OFFSET
            goal.approach_mx = U.mclamp(math.floor(goal.approach_fx))
            goal.approach_my = U.mclamp(math.floor(goal.approach_fy))
          else
            goal.approach_fx = sfx
            goal.approach_fy = sfy
            goal.approach_mx = goal.standoff_mx
            goal.approach_my = goal.standoff_my
          end
        else
          goal.substate = "plan_position"
          goal.scan_spots = nil
          goal._plan_show_tick = nil
          goal._plan_logged = nil
          return
        end
      end
      local afx = goal.approach_fx or (goal.approach_mx + 0.5)
      local afy = goal.approach_fy or (goal.approach_my + 0.5)
      local awx = math.floor(afx * 256 + 0.5)
      local awy = math.floor(afy * 256 + 0.5)
      local adist = U.wdist(info.tankx, info.tanky, awx, awy)
      if now % 25 == 0 then
      end
      -- Closing the distance counts as progress and resets the timer.
      if goal._approach_last_dist == nil or adist < goal._approach_last_dist - 4 then
        goal._approach_last_progress = now
        goal._approach_last_dist     = adist
      end

      -- Stall give-up: if we make no progress closing the gap for
      -- ~10s (most often an ally is parked in our approach path),
      -- abandon the attack_pill goal entirely. pick_goal picks a new
      -- target next tick — could be the same pill from a different
      -- angle, or something else. Cheaper than waiting on a contested
      -- approach. Mirrors APPROACH_GIVE_UP_TICKS in spirit but skips
      -- the angle-ban / plan_position transition; we want a fresh
      -- goal selection at the top, not a same-pill retry.
      local APPROACH_STALL_GIVE_UP_TICKS = 500
      if (now - goal._approach_last_progress) > APPROACH_STALL_GIVE_UP_TICKS then
        print(string.format(TAG ..
          " ATTACK: approach stalled (no progress in %d ticks, dist=%.0f) — abandoning attack_pill",
          APPROACH_STALL_GIVE_UP_TICKS, adist))
        clear_attack_goal(state, "approach stalled")
        return
      end
      -- Generous spot tolerance: 1/2 tile (128 wu). We don't need to land
      -- exactly on the approach point — close + stopped + facing the pill is
      -- enough to start the take. (Was 16 wu / 1/16 tile, which forced a slow
      -- creep right onto the spot.)
      local DIST_TOL  = 128
      -- Speed gate stays tight so we actually stop before engaging. Matches the
      -- creep target speed in steering.lua; insisting on 0 hangs the substate.
      local SPEED_TOL = 4
      -- Facing gate: the tank must be pointed at the pill so the take can
      -- aim/fire immediately instead of pivoting from a bad heading. ~10° — wide
      -- enough that the hard-hold turn in steering doesn't oscillate past it.
      local FACE_TOL  = 7   -- brad (256 = full circle), ~9.8 deg
      local pill_dir  = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                                   pmx + 0.5, pmy + 0.5)
      local face_corr = U.adiff(info.direction, pill_dir)
      local facing_ok = math.abs(face_corr) <= FACE_TOL

      -- HUD overlay near the tank: current distance + threshold so we
      -- can see live what's blocking the transition.

      if adist <= DIST_TOL and facing_ok and
         effectively_stopped(state, info, now, SPEED_TOL, 5, "approach") then
        -- Blitz routing at the setup point:
        --   * a JOINER already answered (>=1 squadmate), OR it's a non-PPT take —
        --     SKIP walls and rally now (blitz_wait → GO). The simultaneous
        --     overwhelm is the protection, so on GO EVERYONE charges (the
        --     commander too: no shield was built → goal._blitz_shielded stays nil).
        --   * full-pill PPT with NOBODY joined yet — fall through and build the
        --     shield FIRST. The call stays JOINABLE through build_walls; a soldier
        --     who joins mid-build just makes us finish the walls then rally
        --     (build_walls -> blitz_wait, goal._blitz_shielded set). After a shield
        --     IS built the commander fires the usual PPT route, never charges.
        if goal._blitz then
          -- Pill already softened below the blitz threshold by the time we got
          -- in position: skip the coordinated blitz_wait entirely and finish it
          -- solo (fall through to the normal in-position decision below). A
          -- near-dead pill doesn't warrant a multi-tank overwhelm / GO handshake.
          local _wp  = world.pills and world.pills[goal.target_id]
          local _php = _wp and _wp.health or 0
          -- Blitz-only (squad.blitz_only): no solo shortcut -- blitz it anyway.
          if _php > 0 and _php < (C.HARD_TAKE_MIN_HP or 12)
             and not squad.blitz_only(state) then
            goal._blitz = false
            goal._blitz_solo = true
          else
            local _joined = squad.blitz_ready_status(state, now, info.player_number or -1)
            if _joined > 0 or not goal._is_ppt then
              goal.substate = "blitz_wait"
              goal._blitz_ready_since = nil
              goal._approach_start = nil
              goal._approach_last_progress = nil
              goal._approach_last_dist = nil
              state.squad_blitz_in_position = true
              return
            end
            -- full-pill PPT, no joiner yet → fall through to build the shield
          end
        end
        -- Shield-scan-driven wall building: prefer the explicit winner;
        -- if there's no winner, fall back to the standoff candidate so
        -- we can still build out its protection slots if any exist.
        local target_for_build = nil
        local why_no_build = "no shield scan"
        if goal._shield_scan then
          target_for_build = goal._shield_scan.best or goal._shield_scan.standoff
          if not goal._shield_scan.best then why_no_build = "no winner; using standoff" end
        end
        local pots = nil
        if target_for_build then
          if target_for_build.best_aim_idx and target_for_build.aims[target_for_build.best_aim_idx] then
            pots = target_for_build.aims[target_for_build.best_aim_idx].potential_blockers
            -- Lock the gun aim onto the chosen corner/center for the
            -- shield target. If best_aim_idx is set, that means at least
            -- one outgoing aim is clean, and that's the lane we want
            -- charge/aim/engage to all use. (Done unconditionally here
            -- in case the standoff itself ends up being target_for_build
            -- — the original best-only path doesn't set aim_mx/my then.)
            local off = shield.AIM_OFFSETS_TILE_FIRE[target_for_build.best_aim_idx]
                        or shield.AIM_OFFSETS_TILE_FIRE[1]
            goal.aim_mx = pmx + off[1]
            goal.aim_my = pmy + off[2]
            -- Same aim in world units, for the GO gate / steering / sanity check.
            goal.aim_wx  = bit.lshift(pmx, 8) + math.floor(off[1] * 256)
            goal.aim_wy  = bit.lshift(pmy, 8) + math.floor(off[2] * 256)
            goal.aim_idx = target_for_build.best_aim_idx
          else
            why_no_build = "target has no clean aim (all blocked by wall)"
            -- No clean shot through any aim means PPT can't function
            -- (in_range_aim/shoot_pill rely on a chosen corner). Demote
            -- to non-PPT so aim transitions to the regular charge path
            -- instead of in_range_position, which would just sit idle.
            if goal._is_ppt then
              print(TAG .. " ATTACK: demoting shielded -> no-shield (no clean aim available)")
              goal._is_ppt = false
            end
          end
        end
        -- Tree budget check: each wall needs WALL_SHIELD_BUILD_COST
        -- trees. The pre-flight gather_trees substate (entered before
        -- approach in plan_position) should have stocked us by now —
        -- if we're still short here, gathering would either be wasted
        -- (no nearby forest reachable from the standoff) or undoing
        -- the approach we just finished. Degrade to no-shield aim.
        local cost_per_wall = C.WALL_SHIELD_BUILD_COST or 2
        local needs_build = pots and #pots > 0
        -- Wall-building is a PPT-only behavior. If the per-tick health
        -- check has demoted us to non-PPT (pill HP < threshold), the
        -- whole point of shielded charge is moot — slot straight into
        -- the legacy charge path instead of detouring through a build.
        if needs_build and not goal._is_ppt then
          needs_build = false
          why_no_build = "no-shield (pill HP below threshold) — no walls needed"
        end
        -- A HUMAN TEAM-MATE WHO ARRIVED AFTER THE PLAN. plan_position makes
        -- the _is_ppt call once for the life of the goal, so a person who
        -- drove up while the bot was approaching would otherwise still watch
        -- it stop and build. The distance is re-read here, at the one place
        -- the walls are actually committed to, so the rule holds whenever the
        -- human turns up. ORDER_HUMAN_NEAR_SUICIDE_TILES = 0 turns it off.
        if needs_build and (C.ORDER_HUMAN_NEAR_SUICIDE_TILES or 0) > 0 then
          local hn = U.human_ally_near(info, bit.rshift(info.tankx or 0, 8),
                                       bit.rshift(info.tanky or 0, 8),
                                       C.ORDER_HUMAN_NEAR_SUICIDE_TILES)
          if hn then
            needs_build = false
            goal._is_ppt = false
            goal._human_near = hn
            why_no_build = string.format("human ally %d away — going straight in", hn)
          end
        end
        -- CHARGE NOW: no more blockers.
        if needs_build and goal._charge_now then
          needs_build = false
          why_no_build = "charge now (attack ping) - no more blockers"
        end
        local trees_needed = needs_build and (#pots * cost_per_wall) or 0
        if needs_build and (info.trees or 0) < trees_needed then
          needs_build = false
          why_no_build = string.format("not enough trees (%d/%d for %d walls) at standoff",
                                       info.trees or 0, trees_needed, #pots)
        end
        -- Full-pill PPT blitz that has NO buildable wall slots: nothing to build,
        -- so rally now (blitz_wait → GO) instead of falling into the solo firing
        -- path. (A non-blitz take falls straight through to the normal decision.)
        if goal._blitz and not needs_build then
          goal.substate = "blitz_wait"
          goal._blitz_ready_since = nil
          state.squad_blitz_in_position = true
          return
        end
        local decision_msg
        if needs_build then
          local unsafe = armour_unsafe_for_pill_take(info, pill and pill.health, blitz_2plus, goal)
          if unsafe then
            clear_attack_goal(state, "abort@build_walls_entry — " .. unsafe)
            return
          end
          goal.substate = "build_walls"
          -- Reset the per-wall + global stall timers EVERY entry into
          -- build_walls so a re-entry (build_walls → aim → ... →
          -- build_walls again on the same goal) doesn't inherit stale
          -- timer state. Without this the per-wall stall check at the
          -- top of build_walls compares against a >5s-old _wall_idx_started
          -- on tick 1 and instantly skips the slot before the LGM can
          -- move. Lazy-init only runs when _wall_build_list is nil, so
          -- it can't be the sole reset point.
          goal._wall_build_last_progress  = now
          goal._wall_build_start          = now
          goal._wall_idx_started          = nil
          goal._wall_idx_prev_tt          = nil
          goal._wall_build_prev_man       = nil
          goal._wall_build_prev_idx       = nil
          decision_msg = string.format("BUILD_WALLS: %d slots (%s, score=%d, trees=%d/%d)",
                                       #pots, target_for_build.kind or "?",
                                       target_for_build.score or 0,
                                       info.trees or 0, trees_needed)
        else
          goal.substate = "aim"
          goal.aim_tick = now
          goal._aim_locked = nil
          if pots and #pots == 0 then why_no_build = "0 potential blockers (already all built or no slots exist)" end
          decision_msg = "skip build_walls: " .. why_no_build
        end
        -- Approach finished — clear its timer so a future re-entry
        -- starts a fresh countdown.
        goal._approach_start = nil
        goal._approach_last_progress = nil
        goal._approach_last_dist = nil
        print(TAG .. " ATTACK: at approach " .. decision_msg)
        -- Park the message on screen for ~2 seconds (100 ticks @ 50Hz)
        -- so the human can read it without scrubbing the trace.
        goal._build_decision_msg   = decision_msg
        goal._build_decision_until = now + 100
      else
        -- Hard approach timeout: if we make NO net progress closing
        -- the gap to the approach point in APPROACH_GIVE_UP_TICKS
        -- (~10 s @ 50 Hz), bail back to plan_position. Without this,
        -- the stuck-detection whitelist for "approach" can leave the
        -- tank parked indefinitely against an obstacle.
        local APPROACH_GIVE_UP_TICKS = 500
        goal._approach_timeout_total = APPROACH_GIVE_UP_TICKS
        if (now - goal._approach_last_progress) > APPROACH_GIVE_UP_TICKS then
          -- Ban this approach angle on this pill for 3 minutes (9000
          -- ticks @ 50Hz). evaluate_pill_difficulty's per-degree scan
          -- will skip banned buckets so plan_position's next pass picks
          -- a different angle. Bucket to 5° so close-but-not-identical
          -- candidate angles around the failed one are also excluded.
          if goal._chosen_deg then
            local pkey = pmy * 256 + pmx
            local pill_bans = state.banned_pill_angles[pkey]
            if not pill_bans then
              pill_bans = {}
              state.banned_pill_angles[pkey] = pill_bans
            end
            local bucket = math.floor((goal._chosen_deg % 360) / 5) * 5
            pill_bans[bucket] = now + 9000
            print(string.format(TAG .. " ATTACK: banning approach angle %d° on pill (%d,%d) for 3 min",
                  bucket, pmx, pmy))
          end
          print(string.format(TAG .. " ATTACK: approach stalled (no progress in %d ticks, dist=%d), replanning",
                APPROACH_GIVE_UP_TICKS, adist))
          goal.substate = "plan_position"
          goal.scan_spots = nil
          goal._shield_scan = nil
          goal._plan_show_tick = nil
          goal._plan_logged = nil
          goal._approach_start = nil
          goal._approach_last_progress = nil
          goal._approach_last_dist = nil
        end
      end
    end
    -- Fall through to draw
    if BRAIN_PROFILE_LOG then
      local _t_app1 = clock_us()
      if _t_app1 - _t_app0 > 300 then
        opt.append("optimize.log", string.format(
          "  [app] SLOW total=%.3f ms", (_t_app1 - _t_app0) / 1000))
      end
    end
  end

  -- ══════════════════════════════════════════════════════════════════
  -- build_walls: tank waits at approach point while LGM builds the
  -- shield walls along the chosen aim's protection slots, closest-to-
  -- pill first so the LGM never has to walk past a freshly built wall
  -- to reach the next site. Sets goal.wall_shield + goal.wall_mx/my
  -- one wall at a time; builder.lua picks them up via the wall_shield
  -- mode (which already knows how to dispatch BUILDMODE_BUILD).
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "build_walls" then
    commit_soak_finish(goal, state, info)   -- one-time soak/swerve decision (PPT commit point)
    -- Early blitz on join (flag-gated): we only entered build_walls because we
    -- were a full-pill PPT with NOBODY joined at the in-position checkpoint. Keep
    -- building while joiners are still APPROACHING — the half-built shield is our
    -- protection until enough tanks actually arrive. Only once at least
    -- BLITZ_MIN_READY_TO_CHARGE blitzers are IN POSITION AND READY (rdy=1) does
    -- the simultaneous overwhelm replace the shield: abandon the remaining blocks
    -- and rally NOW. The commander itself always counts as 1 (it's at its
    -- standoff), so _ready ready soldiers means (_ready + 1) ready blitzers; a
    -- still-approaching tank beyond the threshold keeps closing and joins on GO.
    -- Mirror the in-position skip-walls route: blitz_wait WITHOUT _blitz_shielded,
    -- so on GO everyone (commander included) charges rather than firing from cover.
    if C.BLITZ_ABORT_BUILD_ON_READY and goal._blitz then
      local _total, _ready = squad.blitz_ready_status(state, now, info.player_number or -1)
      _ready = _ready or 0
      if (_ready + 1) >= squad.blitz_min() then
        -- Do we already have a blocker (built this take OR pre-existing) in our
        -- chosen shield slots? If so KEEP the PPT shield route: on GO we thread to
        -- the exact engage spot and fire from behind cover (firing off-spot would
        -- clip our own blockers). With nothing up yet there's no shield to fire
        -- behind, so fall back to the unshielded overwhelm charge.
        local _have_blocker = false
        if goal._wall_build_list then
          for _, _p in ipairs(goal._wall_build_list) do
            local _tt = U.ttype(_p.mx, _p.my)
            if _tt == C.T_BUILDING or _tt == C.T_HALFBUILD or _tt == C.T_PILLBOX then
              _have_blocker = true; break
            end
          end
        end
        goal.wall_shield = false
        goal.wall_mx = nil
        goal.wall_my = nil
        goal.substate = "blitz_wait"
        goal._blitz_ready_since = nil
        goal._blitz_shielded = _have_blocker or nil   -- blocker up → GO fires PPT from cover; none → charge
        state.squad_blitz_in_position = true
        return
      end
    end
    -- Lazy init: build the wall queue once, sorted closest-to-pill first.
    if not goal._wall_build_list then
      local pots = nil
      if goal._shield_scan and goal._shield_scan.best then
        local w = goal._shield_scan.best
        if w.best_aim_idx and w.aims[w.best_aim_idx] then
          pots = w.aims[w.best_aim_idx].potential_blockers
        end
      end
      pots = pots or {}
      local sorted = {}
      for _, p in ipairs(pots) do sorted[#sorted + 1] = p end
      -- Closest-to-pill first, but slots that touch an existing friendly
      -- pill go AFTER the rest (they were already penalised in the scan;
      -- here they simply get built last, so a carried pill lands on a
      -- slot that adds cover before one that mostly adds exposure).
      table.sort(sorted, function(a, b)
        local aa = a.adj_friendly and 1 or 0
        local ab = b.adj_friendly and 1 or 0
        if aa ~= ab then return aa < ab end
        local da = (a.mx - pmx) * (a.mx - pmx) + (a.my - pmy) * (a.my - pmy)
        local db = (b.mx - pmx) * (b.mx - pmx) + (b.my - pmy) * (b.my - pmy)
        return da < db
      end)
      goal._wall_build_list  = sorted
      goal._wall_build_idx   = 1
      goal._wall_build_start = now
      goal._wall_build_last_progress = now
      goal._last_wall_early  = nil  -- reset the last-wall early-end latch per build
      state._wall_shield_dispatch = nil  -- clear stale dispatch so the last-wall
                                         -- early-end can't match a prior goal's
      -- Snapshot initial tile types per slot so we can tell pre-existing cover
      -- from blockers we actually placed (used by the early-success gate — one
      -- newly-built blocker is enough — and the "0 BUILT" diagnostic banner).
      do
        local initial_tt = {}
        local preexisting = 0
        for i, p in ipairs(sorted) do
          local tt0 = U.ttype(p.mx, p.my)
          initial_tt[i] = tt0
          if tt0 == C.T_BUILDING or tt0 == C.T_HALFBUILD or tt0 == C.T_PILLBOX then
            preexisting = preexisting + 1
          end
        end
        goal._wall_build_initial_tt   = initial_tt
        goal._wall_build_preexisting  = preexisting
      end
      -- Reset the LGM-progress trackers too, otherwise they retain
      -- state from a previous build attempt on the same goal table
      -- and the give-up timer compares against stale "last seen
      -- making progress" values.
      goal._wall_build_prev_man  = nil
      goal._wall_build_prev_idx  = nil
      goal._wall_idx_started     = nil
      goal._build_decision_msg        = nil
      goal._build_decision_until      = nil
      goal._build_decision_is_failure = nil
      goal._build_timeout_total       = nil
    end

    local list = goal._wall_build_list
    local idx  = goal._wall_build_idx

    -- Skip walls that are already built (could be by us, ally, or just
    -- pre-existing terrain we re-checked). Counts as progress so the
    -- give-up timer resets.
    while idx <= #list do
      local target = list[idx]
      local tt = U.ttype(target.mx, target.my)
      -- T_PILLBOX too: a slot that got a pillbox dropped on it (by us
      -- using one of the carried pills as a blocker) is just as valid
      -- a shield as a wall — even better since the pillbox actively
      -- shoots back. Advance past it.
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD or tt == C.T_PILLBOX then
        idx = idx + 1
        goal._wall_build_last_progress = now
        goal._wall_idx_started = nil  -- reset per-wall sub-timer
      else
        break
      end
    end
    goal._wall_build_idx = idx
    -- Sync wall_mx/my + wall_shield so builder.lua's wall_shield
    -- dispatch can find the current target. The PPT build_walls path
    -- uses _wall_build_list (not the legacy goal.wall_mx), so without
    -- this the builder never enters wall_shield mode and the LGM sits
    -- idle for the entire build_walls phase.
    if idx <= #list then
      goal.wall_mx     = list[idx].mx
      goal.wall_my     = list[idx].my
      goal.wall_shield = true
    else
      goal.wall_mx     = nil
      goal.wall_my     = nil
      goal.wall_shield = nil
    end

    -- Build-progress heartbeat: every 25 ticks dump the current wall target, its
    -- tile type, the LGM's status/position + distance to the wall, the tank's
    -- distance to the wall, and how long since the give-up timer last reset. Lets
    -- us see WHY a wall round-trip takes hundreds of ticks (LGM traveling far,
    -- stuck against terrain, or oscillating) instead of guessing from transitions.

    -- Per-wall sub-timeout: if a single queue entry has been the
    -- current target for WALL_STALL_TICKS without finishing, skip to
    -- the next one. Catches unreachable walls (LGM can't get there),
    -- builder safety rejections (danger / no trees / pill nearby),
    -- etc., without waiting on the overall give-up window.
    local WALL_STALL_TICKS = 250  -- ~5 s @ 50 Hz
    if idx <= #list then
      local target = list[idx]
      local cur_tt = U.ttype(target.mx, target.my)
      if not goal._wall_idx_started then
        goal._wall_idx_started = now
        goal._wall_idx_prev_tt = cur_tt
      elseif goal._wall_idx_prev_tt ~= cur_tt then
        -- Tile type changed (e.g. forest harvested → grass) — that's
        -- real LGM progress on this slot. Reset the per-wall timer
        -- so the BUILD round-trip after a harvest doesn't trip the
        -- stall and skip a slot we're actively working on.
        goal._wall_idx_started = now
        goal._wall_idx_prev_tt = cur_tt
      elseif (now - goal._wall_idx_started) > WALL_STALL_TICKS then
        -- Detailed post-mortem: dump everything we knew about this
        -- slot at the moment the per-wall timer tripped, so we can see
        -- WHY the LGM never moved it to T_BUILDING/T_HALFBUILD.  Goes
        -- to print2 unconditionally (in BRAIN_DEBUG_MODE) so it lands
        -- in the per-bot log without needing BRAIN_LOG_BUILDER set.
        idx = idx + 1
        goal._wall_build_idx = idx
        goal._wall_idx_started = nil
        goal._wall_idx_prev_tt = nil
      end
    end

    -- LGM round-trip: any change in man_status between ticks counts as
    -- progress. So a successful dispatch (in -> out) AND the LGM coming
    -- back to the tank (out -> in) both reset the give-up timer. Lets
    -- a pillbox-shielded build that requires multiple LGM trips run
    -- as long as the LGM keeps moving.
    local cur_man_status = info.man_status or 0
    if goal._wall_build_prev_man ~= nil and
       goal._wall_build_prev_man ~= cur_man_status then
      goal._wall_build_last_progress = now
      -- Also reset the per-wall stall timer: any LGM transition
      -- (going out / returning to tank) is real work on the current
      -- slot. Without this, a wall that needs harvest+build (two
      -- round-trips ≈ 10s) trips the 5s WALL_STALL even though the
      -- LGM is genuinely moving on its behalf.
      if goal._wall_idx_started then
        goal._wall_idx_started = now
      end
    end
    goal._wall_build_prev_man = cur_man_status
    -- Wall target advancement (idx incremented above) is already
    -- captured by the in-loop progress reset, but record it explicitly
    -- here too for any future case where the queue is mutated outside
    -- this block.
    if goal._wall_build_prev_idx ~= nil and
       goal._wall_build_prev_idx ~= idx then
      goal._wall_build_last_progress = now
    end
    goal._wall_build_prev_idx = idx

    -- Hard timeout: if NO progress (no wall built, no LGM transition)
    -- in BUILD_GIVE_UP_TICKS (~10 sec at 50 Hz), proceed without the
    -- rest. Avoids hanging the attack flow when trees are short, the
    -- LGM is genuinely dead, or the build site is unreachable.
    local BUILD_GIVE_UP_TICKS = 500
    local stalled = (now - goal._wall_build_last_progress) > BUILD_GIVE_UP_TICKS

    -- Stash for the per-tick draw block below; it owns rendering so
    -- the countdown shows even at the moment we transition out (and
    -- stays put across substate flips while the goal still has the
    -- build state populated).
    goal._build_timeout_total = BUILD_GIVE_UP_TICKS

    -- Early success: a single blocker is enough for a protected take. As soon as
    -- we've NEWLY placed at least PPT_BLOCKERS_ENOUGH completed blocker(s) — a
    -- wall or a dropped pillbox on a slot that wasn't cover when we started —
    -- end the build phase successfully instead of grinding through the rest of
    -- the queue. (Counts only T_BUILDING/T_PILLBOX, not a still-rising halfwall.)
    local newly_built = 0
    do
      local initial_tt = goal._wall_build_initial_tt or {}
      for i, p in ipairs(list) do
        local tt1 = U.ttype(p.mx, p.my)
        local now_block = (tt1 == C.T_BUILDING or tt1 == C.T_PILLBOX)
        local was_block = (initial_tt[i] == C.T_BUILDING or initial_tt[i] == C.T_HALFBUILD or initial_tt[i] == C.T_PILLBOX)
        if now_block and not was_block then newly_built = newly_built + 1 end
      end
    end
    -- One blocker is "enough" cover ONLY when a blitz overwhelm is actually
    -- happening — the soldiers share the pill's fire. Solo (or before anyone is
    -- committed) we still need the full planned shield. Gate the early success
    -- on a blitz being underway: either enough blitzers are READY to charge
    -- (the per-bot party MIN — commander counts as 1, so +1 below), OR at
    -- least PPT_BLOCKERS_ENOUGH_MIN_INWAIT soldier(s) are already parked in
    -- blitz_wait while we (the commander) keep building.
    local _bt, _bready, _bmb, _bun, _binwait =
      squad.blitz_ready_status(state, now, info.player_number or -1, info)
    local blitz_supported = goal._blitz and (
         ((_bready or 0) + 1) >= squad.blitz_min()
      or (_binwait or 0) >= (C.PPT_BLOCKERS_ENOUGH_MIN_INWAIT or 1))
    local built_enough = newly_built >= (C.PPT_BLOCKERS_ENOUGH or 1)
                         and blitz_supported
    if built_enough and idx <= #list and not stalled then
    end

    -- Cover-sufficient: a friendly pill blocker is worth 3 walls (PILLS_MAX_HEALTH
    -- 15 vs WALL_HP_FULL 5 shots-to-break), matching the shield scorer. Sum the
    -- current cover among the slots in shots and stop the build the moment it
    -- reaches PPT_COVER_TARGET_SHOTS — so one dropped/pre-existing pill ends it
    -- immediately, while a walls-only shield still builds out to three walls.
    -- Independent of the blitz gate (real cover is real cover, solo or not).
    local cover_shots = 0
    for _, p in ipairs(list) do
      local tt1 = U.ttype(p.mx, p.my)
      if tt1 == C.T_PILLBOX then
        local fp = M.find_pill_at(world, p.mx, p.my)
        if fp and (fp.owner == "friendly" or fp.owner == "allied")
           and (fp.health or 0) > 0 then
          cover_shots = cover_shots + (C.PILLS_MAX_HEALTH or 15)
        end
      elseif tt1 == C.T_BUILDING or tt1 == C.T_HALFBUILD then
        cover_shots = cover_shots + (C.WALL_HP_FULL or 5)
      end
    end
    local cover_enough = cover_shots >= (C.PPT_COVER_TARGET_SHOTS or 15)
    if cover_enough and idx <= #list and not stalled then
    end

    -- Last-wall early end: we're on the FINAL blocker of a multi-wall shield
    -- (>=1 other blocker already up so the tank has cover), the LGM has been
    -- SENT OUT specifically for THIS last blocker, and the estimated round-trip
    -- for it to reach the slot, build, and walk back is short. Then don't sit
    -- idle on the last wall + the walk home — proceed with the take in parallel
    -- (the engine finishes the in-flight build even after we leave build_walls;
    -- leaving only suppresses NEW dispatches, it doesn't recall the LGM).
    --
    -- "Sent out for the last blocker" = the builder's most recent wall dispatch
    -- targets this exact slot with a blocker action (BUILD or PBOX — NOT FARM, a
    -- forest harvest means the wall isn't going up yet), AND the LGM has left the
    -- tank to do it. Without the dispatch match we could skip while the LGM is
    -- merely walking back from a PREVIOUS wall (idx just advanced to the last
    -- slot but it was never dispatched), abandoning the last blocker entirely.
    local last = list[#list]
    local disp = state._wall_shield_dispatch
    local sent_for_last = disp and (disp.action == "BUILD" or disp.action == "PBOX")
                          and disp.wx == last.mx and disp.wy == last.my
    if not goal._last_wall_early and idx == #list and #list >= 2 and not stalled
       and sent_for_last
       and info.man_status ~= C.LGM_INTANK and info.man_status ~= C.LGM_DEAD
       and info.man_x and (now % 10 == 0) then
      local tmx_t, tmy_t = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
      local lgm_mx, lgm_my = bit.rshift(info.man_x, 8), bit.rshift(info.man_y, 8)
      local to_wall = cpf.lgm_travel_ticks_map(lgm_mx, lgm_my, last.mx, last.my,
        last.mx, last.my, C.WALL_SHIELD_LGM_MAX_TICKS, C.WALL_SHIELD_LGM_STUCK_TICKS)
      local back = cpf.lgm_travel_ticks_map(last.mx, last.my, tmx_t, tmy_t,
        last.mx, last.my, C.WALL_SHIELD_LGM_MAX_TICKS, C.WALL_SHIELD_LGM_STUCK_TICKS)
      if to_wall and to_wall > 0 and back and back > 0 then
        local round_trip = to_wall + (C.LGM_BUILD_TIME or 0) + back
        if round_trip <= (C.PPT_LAST_WALL_EARLY_TICKS or 250) then
          goal._last_wall_early = true
        end
      end
    end

    -- CHARGE NOW: stop building; what stands is the shield.
    if idx > #list or stalled or built_enough or goal._last_wall_early or cover_enough
       or goal._charge_now then
      -- Debug-only: tally built / pre-existing / unbuilt for the on-screen
      -- decision banner. The brain itself doesn't act on these counts.
      goal.wall_shield = false
      goal.wall_mx = nil
      goal.wall_my = nil
      -- Shield built. A blitz now rallies (blitz_wait → GO) — the call stayed
      -- joinable through the whole build, so any soldier who joined waits with us
      -- and we fire together on GO (PPT fires from behind the shield). A non-blitz
      -- take goes straight to the normal PPT firing path (aim), identical to main.
      if goal._blitz then
        goal.substate = "blitz_wait"
        goal._blitz_ready_since = nil
        -- Shielded only if a blocker is actually present (normal completion built
        -- one — cover_shots > 0; a stalled/empty build may have none, in which
        -- case GO should overwhelm-charge rather than thread to a coverless spot).
        goal._blitz_shielded = ((cover_shots or 0) > 0) and true or nil
        state.squad_blitz_in_position = true
      else
        goal.substate = "aim"
        goal.aim_tick = now
        goal._aim_locked = nil
      end
    else
      -- Point the builder at the current target wall.
      local target = list[idx]
      goal.wall_shield = true
      goal.wall_mx = target.mx
      goal.wall_my = target.my
    end
    -- Fall through to draw.
  end

  -- ══════════════════════════════════════════════════════════════════
  -- charge: accelerate toward standoff, let auto-slowdown stop us
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "charge" then
    if not goal.standoff_mx then
      goal.substate = "plan_position"
    end
    -- Record shell count and pill HP at start of charge for swerve trigger.
    -- Decide intent NOW: if bullets we'll fire >= pill HP, we're going for a
    -- confirmed kill; otherwise we're just chipping (defensive swerve later).
    if not goal._charge_shells then
      commit_soak_finish(goal, state, info)   -- one-time soak/swerve decision (legacy commit point)
      goal._charge_shells = info.shells
      local pill_hp_now = pill and pill.health or 15
      goal._bullets_needed = math.min(10, pill_hp_now)
      goal._kill_attempt = (goal._bullets_needed >= pill_hp_now)
      goal._attack_start_tick = now
      -- Stash for the swerve-duration override below — low-HP pills get
      -- shorter swerves since the kill is fast and we don't need a long
      -- evasion window.
      goal._charge_start_hp = pill_hp_now
      -- Charge stall give-up tracker (mirrors approach): reset on charge entry.
      -- Closest squared distance to the pill + lowest pill HP seen so far.
      goal._charge_last_progress = now
      goal._charge_best_d2 = nil
      goal._charge_best_hp = pill_hp_now
    end

    -- Cumulative return-fire hits taken during the charge (armour drop),
    -- mirroring shoot_pill/engage. Feeds the flag-gated defensive swerve below.
    if not goal._charge_armour then goal._charge_armour = info.armour end
    -- Count actual HITS (one per tick our armour drops), not armour points — a
    -- single pill shell is ~5 armour, so the old armour-delta sum tripped the
    -- "N hits" swerve on the very first hit.
    if info.armour < goal._charge_armour then goal._charge_hits_total = (goal._charge_hits_total or 0) + 1 end
    goal._charge_armour     = info.armour

    -- Charge stall give-up: abort if we close NO distance to the pill AND drop
    -- NO pill HP for ~5s. The condition-based aborts below only fire on a
    -- blocked shot path / out-of-shells; a charge that's movement-wedged (e.g. a
    -- wall blocks the standoff) hits none of them and would otherwise sit there
    -- grinding shells / to death. This backstops that. Mirrors approach's stall.
    do
      local CHARGE_STALL_GIVE_UP_TICKS = 250   -- ~5s @ 50Hz
      if not goal._charge_last_progress then goal._charge_last_progress = now end
      local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
      local ddx, ddy = tmx - pmx, tmy - pmy
      local d2 = ddx * ddx + ddy * ddy
      local php = pill and pill.health or 0
      local progressed = false
      if not goal._charge_best_d2 or d2 < goal._charge_best_d2 then goal._charge_best_d2 = d2; progressed = true end
      if not goal._charge_best_hp or php < goal._charge_best_hp then goal._charge_best_hp = php; progressed = true end
      if progressed then goal._charge_last_progress = now end
      if (now - goal._charge_last_progress) > CHARGE_STALL_GIVE_UP_TICKS then
        clear_attack_goal(state, "charge stalled - no progress")
        return
      end
    end

    -- Shot-path obstacle check: every tick, simulate the shell path
    -- and count obstacles. Updates _bullets_needed so the swerve
    -- trigger accounts for walls/trees that need clearing before the
    -- shell reaches the pill. During charge the tank is still closing,
    -- so "not reached" is normal — only abort on impassable obstacles
    -- or insufficient ammo when the shot DOES reach.
    do
      local pill_hp_live = pill and pill.health or 0
      local obstacle_shots, obstacle_reason, reached = shot_path_obstacle_count(info, goal, world)
      if obstacle_shots == math.huge then
        -- ── Step 0 of the blocked-line ladder: are we even there yet? ──
        -- loss_b6 bot3 t=9483: the blitz commander was 8 tiles short of its
        -- standoff on pill (129,130), still driving in with the gun swinging,
        -- when the live line clipped a second pill at (130,131) for a single
        -- tick. This check fired, binned the whole take, and the soldier charged
        -- alone. A bad angle mid-approach usually resolves itself further in, so
        -- do not ask the question until the tank is actually at its firing spot.
        -- The 250-tick CHARGE_ABORT_STALL above stays the safety net for a
        -- charge that never gets anywhere.
        local sfx = goal.standoff_fx or (goal.standoff_mx and (goal.standoff_mx + 0.5))
        local sfy = goal.standoff_fy or (goal.standoff_my and (goal.standoff_my + 0.5))
        local at_spot = true
        if sfx and sfy then
          local tfx, tfy = info.tankx / 256.0, info.tanky / 256.0
          local sdx, sdy = tfx - sfx, tfy - sfy
          local tol = C.CHARGE_SHOT_CHECK_AT_SPOT_TILES or 1.0
          at_spot = (sdx * sdx + sdy * sdy) <= (tol * tol)
          if not at_spot then
            -- Driven PAST the standoff (a charge often overshoots inward) counts
            -- as arrived too: we are as close to the pill as the plan wanted.
            local pcx, pcy = pmx + 0.5, pmy + 0.5
            local tdx, tdy = tfx - pcx, tfy - pcy
            local qdx, qdy = sfx - pcx, sfy - pcy
            at_spot = (tdx * tdx + tdy * tdy) <= (qdx * qdx + qdy * qdy)
          end
        end
        if at_spot then
          -- Step 1: try the pill's other aim points from where we are standing.
          local ladder = try_reaim(state, goal, world, info.tankx, info.tanky,
                                   pmx, pmy, now, "CHARGE", obstacle_reason)
          if ladder == "blocked" then
            -- Step 2: nothing on this pill is shootable from this spot. Ban the
            -- angle and go find another spot; only give the take up once
            -- SANITY_PILL_REPLANS_MAX spots have failed.
            local abandon, bucket, tries =
              blocked_line_replan(state, goal, pmx, pmy, now)
            if abandon then
              print(string.format(TAG .. " CHARGE: impassable obstacle — %s, no clear aim from %d spots, aborting", obstacle_reason, tries))
              clear_attack_goal(state, "shot path blocked: " .. obstacle_reason)
              return
            end
            return
          end
        end
        -- Blocked but not aborting: skip the ammo bookkeeping this tick
        -- (obstacle_shots is math.huge, so total_needed would be nonsense and
        -- CHARGE_ABORT_SHELLS would fire on a line we are still fixing).
        reached = false
      end
      if reached then
        local total_needed = obstacle_shots + pill_hp_live
        -- Credit in-flight on-target shells (left inventory, pill_hp not yet
        -- reduced) so info.shells doesn't undercount and false-abort.
        local in_flight = goal._on_target_in_flight or 0
        local avail_shots = info.shells + in_flight
        -- In a 2+ blitz, don't abort for insufficient ammo: the ally(ies) share the
        -- finishing shells, so the team's combined fire can drop the pill even when
        -- OUR magazine alone can't. Only a solo (or lone-"blitz"-of-one) take needs
        -- enough shells on its own — there, bail and let refuel replan take over.
        -- CHARGE NOW IGNORES SAFETY: no shell abort, it goes in anyway.
        if avail_shots < total_needed and not blitz_2plus
           and not (goal._charge_now and C.CHARGE_NOW_IGNORE_SAFETY) then
          print(string.format(TAG .. " CHARGE: not enough shells (%d obstacles + %d hp = %d needed, have %d + %d in-flight = %d) — aborting",
            obstacle_shots, pill_hp_live, total_needed, info.shells, in_flight, avail_shots))
          clear_attack_goal(state, "not enough shells to finish take")
          return
        end
        goal._bullets_needed = total_needed
      end
    end

    -- Immediate swerve trigger.  New rule (replaces fired >= bullets_needed):
    -- pill dead OR the count of currently-in-flight on-target shells covers
    -- the remaining pill HP.  C/D = _on_target_in_flight / pill.health.
    -- update_shot_accounting re-simulates each in-flight shell against
    -- live terrain every tick, so if a tree grows into the trajectory or
    -- the shell dies short, it drops back out of in_flight and the gate
    -- naturally fails — we fire a replacement next reload.  Steering's
    -- pre-fire predictor (see steering.lua charge/shoot_pill/engage) may
    -- have already entered swerve this same tick on the just-fired killing
    -- shot; this gate handles the case where prediction didn't apply
    -- (e.g. shell tracker confirmed an off-target shot's status flip).
    local bullets_fired = (goal._charge_shells or info.shells) - info.shells
    local pill_hp = pill and pill.health or 0
    local on_target_in_flight = goal._on_target_in_flight or 0
    -- Keep firing until the pill is ACTUALLY dead — don't swerve off on the
    -- in-flight prediction (on_target_in_flight >= hp), which stops one shot
    -- short if any in-flight shell diverges. Swerve only once hp hits 0.
    if pill_hp <= 0 then
      -- Pill-suicider: no kill swerve either — it goes straight from charging
      -- the pill to capturing the body (same handoff the swerve-done block runs).
      if state.is_pill_suicider then
        suicider_kill_handoff(goal, state, world, info, now, "charge")
        return
      end
      -- Blitz-only-when-hit: an undamaged committed 3+ blitzer likewise does NOT
      -- peel on the kill -- it commits onto the corpse and captures. The dodge
      -- returns only once it is actually hit (blitz_commit_hold goes false).
      if blitz_commit_hold(goal, state, info, now) then
        suicider_kill_handoff(goal, state, world, info, now, "charge", "blitz_hold")
        return
      end
      enter_swerve(goal, world, state, info, pmx, pmy, "kill")
      goal._swerve_pill_dead = (pill_hp <= 0)
      print(string.format(TAG .. " ATTACK: immediate swerve from charge (fired=%d in_flight=%d hp=%d kill_attempt=%s start_hp=%s)",
            bullets_fired, on_target_in_flight, pill_hp,
            tostring(goal._kill_attempt), tostring(goal._charge_start_hp)))
    end
    -- Flag-gated DEFENSIVE swerve during charge (CHARGE_SWERVE_ENABLED). Same
    -- conditions engage/shoot_pill use: ATTACK_CURVE_AFTER_HITS hits taken, OR
    -- the kill is locked (on-target in-flight shells already cover the pill's
    -- remaining HP). The pill-dead case is handled by the block above. Honors the
    -- one-time soak decision: don't peel off the last HP of a calm pill we chose
    -- to buck in and finish. The straight no-dodge rush lives in kill_hardline.
    -- An ammo-deprived decoy never peels off to dodge return fire: its whole
    -- job is to STAY on the pill drawing fire for the captain, so it keeps
    -- charging through the hits instead of defensive-swerving. The pill-dead
    -- swerve above (kill mode) still fires the moment the pill dies, so the
    -- dead-pill handoff (rush / capture / exit) runs exactly as normal.
    -- A pill_suicider is excluded on the same line as the ammo-deprived decoy,
    -- and for the same reason: its job is to stay on the pill. It charges
    -- through the return fire instead of peeling off. (The pill-DEAD case above
    -- returns before this block for a suicider — it hands straight to
    -- capture_pill, so a suicider never swerves for any reason at all.)
    -- Debug-only: log the skip when this WOULD have swerved. Deliberately
    -- re-derives the two triggers instead of moving the suicider test inside the
    -- block, so commit_soak_finish (a one-time committing side effect) keeps
    -- being skipped for suiciders exactly as before.
    -- (single-line condition on purpose: lua_strip's --strip-block only removes
    -- the FIRST line of an `if BRAIN_DEBUG_MODE` header, so a wrapped condition
    -- would leave dangling `and ...` lines in opt/.)
    if C.CHARGE_SWERVE_ENABLED and pill and (pill.health or 0) > 0
       and not state.ammo_deprived and not state.is_pill_suicider then
      local _soak_ok = commit_soak_finish(goal, state, info)
      local _tank_finish = _soak_ok and (pill.health or 0) <= (C.TANK_FINISH_MAX_HP or 3)
                           and (pill.anger or 0) <= (C.TANK_FINISH_MAX_ANGER or 0.25)
      local _kill_locked = goal._kill_attempt and (goal._on_target_in_flight or 0) >= (pill.health or 0)
      local _hits_swerve = (goal._charge_hits_total or 0) >= (C.ATTACK_CURVE_AFTER_HITS or 3)
      -- "Blitz only when hit": an UNDAMAGED committed blitzer in a 3+ party holds
      -- its PROACTIVE kill-locked swerve -- falls through to the substate handler
      -- below, exactly like the suicider skip. The hits-taken swerve is NOT held
      -- (once _commit_hold is true we are undamaged, so _hits_swerve is 0 anyway),
      -- and neither is the pill-DEAD swerve above.
      local _commit_hold = blitz_commit_hold(goal, state, info, now)
      -- Kill-lock bypasses the soak: once the lethal shot is in flight the pill is
      -- as good as dead, so swerve even on a calm low-HP pill we'd otherwise soak.
      -- tank_finish still suppresses the purely-defensive (hits-taken) swerve.
      if (_kill_locked and not _commit_hold) or (_hits_swerve and not _tank_finish) then
        enter_swerve(goal, world, state, info, pmx, pmy, "defensive")
        return
      end
    end
    -- Steering handles movement and transition to engage
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- aim: stopped at standoff, turning to face pill, no shooting
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "aim" then
    -- Check if aimed (steering sets _aim_locked when corr <= 1)
    if goal._aim_locked then
      compute_best_swerve_dir(goal, world, pmx, pmy, tmx, tmy)

      -- Anger cool-down gate: don't commit to the take while the pill
      -- is still hot. We wait in aim until anger drops to <= 0.65.
      -- aim_tick is bumped each waiting tick so the 3-second aim
      -- timeout doesn't fire while we're just waiting on the cool-down.
      local anger = (pill and pill.anger) or 0
      -- ONLY change over the original cool-down gate: the moment the pill is
      -- calm enough that we'd open a hardline rush (anger at/below the rush
      -- threshold), switch straight to the dedicated kill_hardline take
      -- instead of continuing the normal aim path.
      if pill and (pill.health or 0) == 1 and not goal._charge_now_careful
         and info.armour >= (C.ATTACK_RUSH_MIN_ARMOUR or 5)
         and anger <= (C.ATTACK_RUSH_MAX_ANGER or 0.34)
         and U.mdist(bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8), pmx, pmy) <= (C.HARDLINE_ENGAGE_RANGE or 10) then
        goal.substate    = "kill_hardline"
        goal._kill_rush  = true
        goal._aim_locked = nil
        return
      end
      -- CHARGE NOW: no wait for the pill to cool.
      if anger > 0.65 and not goal._charge_now then
        goal.aim_tick = now
      else

      -- Check for trees between tank and crosshairs (pill direction).
      -- Skip detree entirely for shielded pill takes — the shield scan
      -- already picked an aim corner with a clear shot through the
      -- chosen angle, so any trees on the pill-center line aren't on
      -- our actual firing path. Burning shells to clear them just
      -- wastes ammo and time before we can take the corner shot.
      -- Also skip for a 2+ tank blitz: the overwhelm charge doesn't need a
      -- pre-cleared lane (solo would bother; with an ally it's wasted time).
      local trees = (goal._is_ppt or blitz_2plus) and 0
                    or forest_tiles_on_path(tmx, tmy, pmx, pmy)
      if trees > 0 then
        goal.substate = "detree"
        goal._aim_locked = nil
        goal._detree_shells_at_start = info.shells  -- baseline for actual shots fired
        goal._detree_shots_needed = trees
        print(string.format(TAG .. " ATTACK: aimed, clearing %d trees", trees))
      elseif goal._is_ppt and goal._shield_scan then
        -- Shielded: skip the aggressive charge — move carefully into
        -- range, re-aim precisely, then shoot.
        goal.substate = "in_range_position"
        goal._aim_locked = nil
        print(TAG .. " ATTACK: shielded aimed, moving into firing range")
      else
        -- Demote-on-missing-scan: if we were PPT but lost the shield
        -- scan (sanity replan / partial re-plan), drop PPT and run the
        -- non-PPT charge → engage path from where we are now (already
        -- at or near the PPT standoff = ~7 tiles, well inside the
        -- non-PPT 7.4-tile range, so charge typically completes in
        -- one tick).
        if goal._is_ppt and not goal._shield_scan then
          goal._is_ppt = false
          print(TAG .. " ATTACK: PPT had no shield_scan at aim — demoting to non-PPT charge")
        end
        local unsafe = armour_unsafe_for_pill_take(info, pill and pill.health, blitz_2plus, goal)
        if unsafe then
          clear_attack_goal(state, "abort@charge_entry — " .. unsafe)
          return
        end
        goal.substate = "charge"
        goal._aim_locked = nil
        goal._charge_braking = nil
        print(TAG .. " ATTACK: aimed, charging to standoff")
      end
      end  -- anger gate
    -- Abort if can't aim within 3 seconds
    elseif goal.aim_tick and (now - goal.aim_tick) > 150 then
      print(TAG .. " ATTACK: aim timeout, aborting")
      clear_attack_goal(state, "aim timeout (>150t)")
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- detree: shoot trees between tank and pill until clear
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "detree" then
    -- Tree recount is display-only: the substate advances on `fired`/`needed`
    -- below, and the only reader of trees_left is the stripped viz.text.  The
    -- walk (a U.line_walk with a per-tile closure) is therefore debug-only, and
    -- its do_viz argument was already BRAIN_DEBUG_MODE.
    local trees_left = 0
    local needed = goal._detree_shots_needed or 0
    -- Count actual shots fired by tracking shell count drops
    local fired = (goal._detree_shells_at_start or info.shells) - info.shells
    -- HUD: detree progress above the tank
    if fired >= needed or fired >= 6 then
      if goal._is_ppt then
        goal.substate = "in_range_position"
        print(string.format(TAG .. " ATTACK: PPT detree done (shots=%d/%d), moving into range",
              fired, needed))
      else
        local unsafe = armour_unsafe_for_pill_take(info, pill and pill.health, blitz_2plus, goal)
        if unsafe then
          clear_attack_goal(state, "abort@charge_entry — " .. unsafe)
          return
        end
        -- A blitz SOLDIER (committed to a commander, no GO yet) must rally in
        -- blitz_wait after clearing its lane — the overwhelm has to be
        -- simultaneous, so it waits for the commander's GO rather than charging
        -- in alone while the captain is still settling at its standoff.
        if goal._blitz and state.squad_cmdr and not goal._blitz_committed then
          goal.substate = "blitz_wait"
          goal._blitz_ready_since = nil
          print(string.format(TAG .. " ATTACK: detree done (shots=%d/%d), rally in blitz_wait for GO",
                fired, needed))
        else
          goal.substate = "charge"
          goal._charge_braking = nil
          print(string.format(TAG .. " ATTACK: detree done (shots=%d/%d), charging",
                fired, needed))
        end
      end
    end
    -- Steering handles shooting; fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- in_range_position (PPT): creep to the standoff at the same speed
  -- the approach uses, hold to the same dist+spd tolerance, no firing.
  -- Steering owns the actual movement; attack.lua just decides when to
  -- transition out.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "in_range_position" then
    if not goal.standoff_mx then
      goal.substate = "plan_position"
      goal.scan_spots = nil
      goal._shield_scan = nil
      goal._shield_scan_pending = nil
      goal._plan_show_tick = nil
      goal._plan_logged = nil
      print(TAG .. " ATTACK: in_range_position has no standoff, replanning")
    else
      local sfx = goal.standoff_fx or (goal.standoff_mx + 0.5)
      local sfy = goal.standoff_fy or (goal.standoff_my + 0.5)
      -- Round-to-nearest (not truncate) so this matches steering's
      -- in_range_position quantization (steering.lua:914-915). Truncating
      -- here while steering rounds caused a 1-wu discrepancy on sub-wu
      -- standoffs (attack viz showed dist=17/16 while steering showed
      -- sdist=16 in the same tick).
      local swx = math.floor(sfx * 256 + 0.5)
      local swy = math.floor(sfy * 256 + 0.5)
      local sdist = U.wdist(info.tankx, info.tanky, swx, swy)

      -- Two-bot collision recovery: if we make no progress closing
      -- the gap for ~10s (most often because an ally is parked in our
      -- creep path), give up THIS attack_pill goal and let pick_goal
      -- choose another target. clear_attack_goal sets goal.kind="none"
      -- which the next tick's selector treats as a clean slate.
      -- Mirrors APPROACH_GIVE_UP_TICKS so the in_range creep gets the
      -- same patient timeout the approach substate just before it does.
      local IN_RANGE_GIVE_UP_TICKS = 500
      if goal._in_range_last_progress == nil then
        goal._in_range_last_progress = now
        goal._in_range_last_dist     = sdist
      elseif sdist < (goal._in_range_last_dist or sdist) - 4 then
        goal._in_range_last_progress = now
        goal._in_range_last_dist     = sdist
      elseif (now - goal._in_range_last_progress) > IN_RANGE_GIVE_UP_TICKS then
        print(string.format(TAG ..
          " ATTACK: in_range stalled (no progress in %d ticks, sdist=%d) — abandoning attack_pill",
          IN_RANGE_GIVE_UP_TICKS, sdist))
        clear_attack_goal(state, "in_range_position stalled")
        return
      end
      -- EXPERIMENT: use 16 wu for ALL takes, including 3-blocker. The
      -- old rule tightened to 8 wu when 3+ blockers were in play, on the
      -- theory that the gun-line geometry is more sensitive there. In
      -- practice the tighter window stalled the transition more than it
      -- helped accuracy.
      -- Revert by restoring this line:
      --   local DIST_TOL = (n_blockers >= 3) and 8 or 16
      -- with n_blockers sourced from goal._shield_scan.best.blockers_count.
      local n_blockers = (goal._shield_scan and goal._shield_scan.best
                          and goal._shield_scan.best.blockers_count) or 0
      local DIST_TOL = 12
      local SPEED_TOL = 4
      -- Mirror to a goal field so steering can match the brake
      -- threshold to the same tolerance — without this, steering
      -- brakes at the default 16 wu and a 3-blocker take with
      -- tol=8 will stall just outside the transition window.
      goal._in_range_dist_tol = DIST_TOL


      if sdist <= DIST_TOL and
         effectively_stopped(state, info, now, SPEED_TOL, 5, "in_range_position") then
        -- Compute the pre-aim point: the chosen corner overshot LATERALLY —
        -- perpendicular to our line of sight, on the side away from the pill
        -- center — by PRE_OVERSHOOT_BRADS of arc at the actual distance.
        -- The idea: settle the gun slightly PAST the corner first, then
        -- refine inward, so the final approach always turns the same
        -- direction and never has to creep across the lock threshold while
        -- the finetune sim samples trajectories.
        --
        -- Angle-space on purpose. The old form offset the corner 3 game-px
        -- "outside the tile" per AXIS, which fails both ways: the component
        -- along the line of sight (depth) barely moves the BEARING, and the
        -- lateral 3px is smaller than the 1-brad lock tolerance (~2.8px at a
        -- 7-tile standoff) — so the settled gunsight never visibly left the
        -- pill and the overshoot was a no-op. Sizing the offset in brads
        -- keeps it meaningful at any range. Center aims stay un-offset.
        local PRE_OVERSHOOT_BRADS = 2.5   -- > the 1-brad lock tolerance
        local tfx, tfy = info.tankx / 256.0, info.tanky / 256.0
        local cxp, cyp = goal.aim_mx, goal.aim_my       -- chosen corner (float)
        local ddx, ddy = cxp - tfx, cyp - tfy
        local dlen = math.sqrt(ddx * ddx + ddy * ddy)
        goal.aim_pre_mx = cxp
        goal.aim_pre_my = cyp
        if dlen > 0.01 then
          local perp_x, perp_y = -ddy / dlen, ddx / dlen  -- perpendicular to LOS
          -- Side: away from the pill center (sign of the corner's lateral
          -- displacement). ~0 = center aim / corner dead ahead: no offset.
          local side = (cxp - (pmx + 0.5)) * perp_x + (cyp - (pmy + 0.5)) * perp_y
          if math.abs(side) > 1e-6 then
            if side < 0 then perp_x, perp_y = -perp_x, -perp_y end
            local overshoot = dlen * (PRE_OVERSHOOT_BRADS * 2.0 * math.pi / 256.0)
            goal.aim_pre_mx = cxp + perp_x * overshoot
            goal.aim_pre_my = cyp + perp_y * overshoot
          end
        end

        goal.substate        = "in_range_aim_pre"
        goal.aim_tick        = now
        goal._aim_locked     = nil
        goal._pre_aim_locked = nil
        goal._in_range_last_progress = nil
        goal._in_range_last_dist     = nil
        print(string.format(TAG ..
          " ATTACK: PPT in range (%.2f,%.2f) sdist=%d spd=%d, pre-aiming to (%.3f,%.3f)",
          sfx, sfy, sdist, info.speed, goal.aim_pre_mx, goal.aim_pre_my))
      end
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- in_range_aim_pre (PPT): turn to a coarse pre-aim point — the chosen
  -- corner overshot ~2.5 brads LATERALLY past the pill's side (or the
  -- center itself, if the chosen aim was center; no offset then).
  -- Same lock condition as in_range_aim — once corr <= 1, hand off
  -- to in_range_aim for the final corner aim. No trajectory check
  -- here; that lives in in_range_aim_finetune.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "in_range_aim_pre" then
    -- pre-aim is "I'm pointed at the correct SIDE of the pillbox to
    -- begin fine aiming" — it uses its own _pre_aim_locked flag
    -- (set by steering when corr <= 1 of aim_pre_mx/my). The next
    -- substate (in_range_aim) needs its own fresh _aim_locked, so
    -- we explicitly null both here on transition.
    if goal._pre_aim_locked then
      goal.substate        = "in_range_aim"
      goal.aim_tick        = now
      goal._pre_aim_locked = nil
      goal._aim_locked     = nil
      print(TAG .. " ATTACK: pre-aim locked, refining to chosen corner")
    elseif goal.aim_tick and (now - goal.aim_tick) > 150 then
      print(TAG .. " ATTACK: shielded in_range_aim_pre timeout, aborting")
      clear_attack_goal(state, "in_range_aim_pre timeout (>150t)")
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- in_range_aim (PPT): turn to face the chosen aim point exactly.
  -- Steering does the turning; once aim is locked we simulate the
  -- actual shell trajectory (using info.tank_angle = float, bit-
  -- exact with engine) and verify the pill tile is hit.
  --   - hit  → shoot_pill
  --   - miss → fall back to aiming at pill CENTER and try again
  --   - miss while already aimed at center → abort the pill take
  --     (something changed since plan-time: pill moved, blocker
  --     appeared, range insufficient, etc.)
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "in_range_aim" then
    -- Phase 1: turn to the chosen aim point (corner from the shield
    -- scan). Pure trig in steering — no trajectory simulation here.
    -- Hands off to in_range_aim_finetune the moment steering
    -- reports the corner is locked (corr <= 1).
    if goal._aim_locked then
      goal.substate        = "in_range_aim_finetune"
      goal._finetune_start = now
      goal._finetune_taps  = 0
      goal._finetune_reached_tap = nil  -- tap# when the shot first reaches the pill
      print(TAG .. " ATTACK: aim locked, entering in_range_aim_finetune (sim-verify)")
    elseif goal.aim_tick and (now - goal.aim_tick) > 150 then
      print(TAG .. " ATTACK: shielded in_range_aim timeout, aborting")
      clear_attack_goal(state, "in_range_aim timeout (>150t)")
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- in_range_aim_finetune (PPT): per-tick trajectory verify.
  --   Success: cpf.simulate_shot_angle(info.tank_angle) — using the
  --     FLOAT tank angle for bit-exact match to the engine's actual
  --     shell flight — produces a tile path that includes the pill
  --     tile. May succeed immediately on the first tick (no tap
  --     needed) if the corner aim already lines up. Otherwise
  --     steering taps 1 brad/tick toward the pill center until the
  --     sim crosses the pill.
  --   Cap: FINETUNE_MAX_TAPS taps OR FINETUNE_TIMEOUT ticks without
  --     a hit → abort the take. We won't fire on a verified miss;
  --     if the geometry won't converge after a fair attempt the
  --     plan is wrong and goal-selector should re-pick.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "in_range_aim_finetune" then
    -- 24 (was 16): _finetune_taps counts every brain tick in finetune,
    -- including the forced-idle ticks from steering's 3-emit/1-idle
    -- burst cap. 24 brain ticks ≈ 18 actual nudges, restoring the
    -- pre-cap budget of ~12+ effective taps for stubborn geometries.
    local FINETUNE_MAX_TAPS = 24
    local FINETUNE_TIMEOUT  = 100  -- ~2 s @ 50 Hz
    local angle_f = info.tank_angle or info.direction
    local path = cpf.simulate_shot_angle(info.tankx, info.tanky, angle_f,
                                         cpf.SHOT_TANK, info.gunrange or 14)
    local on_pill = false
    if path then
      for _, t in ipairs(path) do
        if t.mx == pmx and t.my == pmy then on_pill = true; break end
      end
    end
    -- Reaching the pill isn't sufficient: if the SAME path crosses a live
    -- pillbox/base BEFORE the pill, shoot_pill's obstacle check just hard-aborts
    -- the take (e.g. friendly pill #6 one tile off the lane). So don't fire on
    -- the first reaching angle — keep tapping toward center until the shot BOTH
    -- reaches the pill AND is clear of a hard blocker (same math.huge test
    -- shoot_pill uses), letting a couple more brad of turn-in thread past it.
    local _fine_obs = shot_path_obstacle_count(info, goal, world)
    local _fine_clear = (_fine_obs ~= math.huge)
    goal._finetune_path    = path        -- viz reads these
    goal._finetune_on_pill = on_pill
    -- Steering's tap gate reads this too: it must KEEP tapping toward the
    -- centre while the shot reaches the pill but a blocker is still on the
    -- lane. It used to stop the moment on_pill went true — with the lane
    -- blocked that froze the heading, made the +N turn-in bound below
    -- unreachable (the tap counter only advanced while on_pill was false),
    -- and the take sat motionless until FINETUNE_TIMEOUT (par2 bot3
    -- t=21684: pill #8 with friendly #4 grazing the lane; the clear lane
    -- was ~1 brad further clockwise, exactly where the next taps would
    -- have swept).
    goal._finetune_clear   = _fine_clear
    -- Bound the "turn in for clearance" search: once the shot first REACHES the
    -- pill, allow only CLEAR_TURN_IN_TAPS more taps (a few brad of extra turn-in)
    -- to also clear a grazed blocker. If it can't clear in that small window the
    -- lane is genuinely pinched — give up rather than turning arbitrarily far off
    -- the corner chasing a lane that isn't there. 6 (was 4): the counter now
    -- advances while blocked (see above), and at the /8 ramp with the 3-burst
    -- cap 6 ticks is ~1.5-2.5 brads of real turn — enough to thread a grazed
    -- corner, still far too little to wander off the pill.
    local CLEAR_TURN_IN_TAPS = 6
    if on_pill and not goal._finetune_reached_tap then
      goal._finetune_reached_tap = goal._finetune_taps or 0
    end

    if on_pill and _fine_clear then
      -- Lock the verified angle as the new aim point so shoot_pill's
      -- corner-correction tap (steering.lua's shoot_pill block) doesn't
      -- pull the angle back toward the original shield-scan corner —
      -- finetune just spent N taps drifting AWAY from that corner to
      -- satisfy the sim, and reverting would immediately invalidate
      -- the verified hit. Project ~16 tiles along the current angle so
      -- shoot_pill's `aim_at_f(tank → aim)` returns essentially the
      -- current direction; corr stays near 0, no further tapping.
      local far_t = 16.0
      local rad   = angle_f * (3.14159265358979323846 / 128.0)
      goal.aim_mx = info.tankx / 256.0 + math.sin(rad) * far_t
      goal.aim_my = info.tanky / 256.0 - math.cos(rad) * far_t
      goal.substate          = "shoot_pill"
      goal._shoot_armour     = info.armour
      goal._shoot_shells     = info.shells
      goal._shoot_hits_total = 0
      goal._shoot_start_tick = now
      goal._shoot_reach_checked = nil  -- re-run the "reach pill" test on the first shoot_pill tick
      -- First steering tick of shoot_pill is forced idle for the same
      -- reason finetune's first tick is: the previous substate may have
      -- been holding a turn key, so the engine's firstLeft/firstRight
      -- ramp counter is unknown. One blank tick guarantees the next
      -- emitted tap starts at /8 instead of full speed (its first
      -- engine tick is the visible-nudge multiple of /8, see
      -- steering.lua's 3-tier turn note).
      goal._shoot_first_steer = true
      print(string.format(TAG ..
        " ATTACK: finetune verified (angle %.2f, %d taps) — opening fire",
        angle_f, goal._finetune_taps or 0))
    elseif on_pill and goal._finetune_reached_tap
       and ((goal._finetune_taps or 0) - goal._finetune_reached_tap) >= CLEAR_TURN_IN_TAPS then
      -- Reached the pill but the blocker is still on the lane after a few more
      -- brad of turn-in — the lane is pinched (e.g. friendly pill + own walls
      -- straddle it). Give up here rather than firing a blocked shot.
      local extra = (goal._finetune_taps or 0) - goal._finetune_reached_tap
      print(string.format(TAG ..
        " ATTACK: reached pill but blocker won't clear after +%d turn-in taps (angle %.2f, pill@(%d,%d)) — aborting",
        extra, angle_f, pmx, pmy))
      clear_attack_goal(state, string.format(
        "turn-in clear failed: +%d/%d taps angle=%.2f pill@(%d,%d)",
        extra, CLEAR_TURN_IN_TAPS, angle_f, pmx, pmy))
    elseif (goal._finetune_taps or 0) >= FINETUNE_MAX_TAPS
       or (now - (goal._finetune_start or now)) > FINETUNE_TIMEOUT then
      local n_taps = goal._finetune_taps or 0
      local n_ticks = now - (goal._finetune_start or now)
      print(string.format(TAG ..
        " ATTACK: finetune couldn't line up after %d taps / %d ticks (angle %.2f, pill@(%d,%d)) — aborting pill take",
        n_taps, n_ticks, angle_f, pmx, pmy))
      clear_attack_goal(state, string.format(
        "finetune timeout: taps=%d/%d ticks=%d/%d angle=%.2f pill@(%d,%d)",
        n_taps, FINETUNE_MAX_TAPS, n_ticks, FINETUNE_TIMEOUT,
        angle_f, pmx, pmy))
    end
    -- Else: steering keeps tapping toward the pill centre this tick (it taps
    -- whenever the shot is not yet reached-AND-clear).
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- shoot_pill (PPT): park, hold aim, fire — same swerve heuristic as
  -- engage: ATTACK_CURVE_AFTER_HITS hits taken or pill killed.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "shoot_pill" then
    -- Blitz commander on the (slow) shielded PPT path: fire the squad GO the
    -- instant we actually start shooting from behind the shield — NOT back in
    -- blitz_wait. The PPT route (aim → in_range_aim_pre → in_range_aim →
    -- shoot_pill) is slow to reach in_range, so a blitz_wait GO sent the soldier
    -- charging long before the commander could support it. Idempotent: the
    -- overwhelm-charge path already set _blitz_go in commit_fire.
    if state.squad_role == "c" and goal._blitz and not goal._blitz_go then
      goal._blitz_go = true
    end
    if not goal._shoot_armour then goal._shoot_armour = info.armour end
    local hits_taken = (info.armour < goal._shoot_armour) and 1 or 0   -- count HITS, not armour points (see charge note)
    goal._shoot_armour = info.armour
    goal._shoot_hits_total = (goal._shoot_hits_total or 0) + hits_taken

    local pill_hp = pill and pill.health or 0

    -- The PPT path skips `charge`, so the shot accounting (set there) never
    -- initialized. Do it here so update_shot_accounting tracks on-target/in-flight
    -- shells for a PPT take too — without this, _on_target_in_flight stays nil,
    -- so the pill_shot_count viz never shows and the kill-locked swerve below
    -- can't fire (only the "pill actually dead" swerve does). A PPT take fires
    -- until the pill is dead, so it's a kill attempt.
    if not goal._attack_start_tick then
      goal._attack_start_tick = now
      goal._charge_start_hp   = pill_hp
      goal._bullets_needed    = math.min(10, pill_hp)
      goal._kill_attempt      = true
    end

    -- Shot-path obstacle check: live-update _bullets_needed and abort
    -- if path is blocked or ammo short. The "shot does not reach pill"
    -- test, however, only runs on the FIRST shoot_pill tick: finetune
    -- already verified the trajectory crosses the pill, and re-simulating
    -- it every tick is sensitive to sub-tile recoil drift / DDA rounding,
    -- so it false-aborts takes that are actually landing. Obstacle and
    -- ammo checks stay live (a pill/building moving into the lane, or
    -- running dry, are real per-tick problems).
    do
      local first_shoot_tick = not goal._shoot_reach_checked
      goal._shoot_reach_checked = true
      local obstacle_shots, obstacle_reason, reached = shot_path_obstacle_count(info, goal, world)
      local total_needed = obstacle_shots + pill_hp
      -- Credit shells already in flight toward the pill: they've left inventory
      -- but haven't reduced pill_hp yet, so info.shells alone undercounts our
      -- effective ammo and false-aborts a take that's actively landing shots.
      local in_flight = goal._on_target_in_flight or 0
      local avail_shots = info.shells + in_flight
      if obstacle_shots == math.huge then
        -- Blocked-line ladder (loss_b6 bot3 t=9483 is the charge-side twin).
        -- We are parked behind the shield at our engage spot, so a blocked line
        -- is a real answer — but only about THIS aim point. Step 1: try the
        -- pill's other corners/centre from here. A hit means re-lining the gun,
        -- which is the PPT aim path (in_range_aim -> finetune -> shoot_pill),
        -- not a plain aim swap: steering's shoot_pill only taps a few brad of
        -- correction, so it would never swing all the way onto a new corner.
        local ladder = try_reaim(state, goal, world, info.tankx, info.tanky,
                                 pmx, pmy, now, "SHOOT_PILL", obstacle_reason)
        if ladder == "ok" then
          goal.substate             = "in_range_aim"
          goal.aim_tick             = now
          goal._aim_locked          = nil
          goal._pre_aim_locked      = nil
          goal._finetune_taps       = 0
          goal._finetune_start      = nil
          goal._finetune_reached_tap = nil
          goal._finetune_path       = nil
          goal._finetune_on_pill    = nil
          goal._shoot_reach_checked = nil
          goal._shoot_first_steer   = nil
          -- _shoot_progress_hp/_shoot_progress_tick deliberately survive: they
          -- are the 200-tick no-progress backstop, and resetting them on every
          -- re-aim would let a pill we can never actually hurt loop forever.
          return
        elseif ladder == "blocked" then
          -- Step 2: no aim point works from this spot — replan onto another one,
          -- and only abandon after SANITY_PILL_REPLANS_MAX spots have failed.
          local abandon, bucket, tries =
            blocked_line_replan(state, goal, pmx, pmy, now)
          if abandon then
            print(string.format(TAG .. " SHOOT_PILL: impassable obstacle — %s, no clear aim from %d spots, aborting", obstacle_reason, tries))
            clear_attack_goal(state, "shot path blocked: " .. obstacle_reason)
            return
          end
          return
        end
        -- ladder == "wait": we searched a moment ago and are letting the gun
        -- settle. Don't abort and don't feed math.huge into the ammo estimate —
        -- just leave _bullets_needed alone this tick. The 200-tick no-progress
        -- timeout below is the backstop.
        total_needed = goal._bullets_needed or pill_hp
      elseif not reached and first_shoot_tick then
        print(string.format(TAG .. " SHOOT_PILL: shot does not reach pill tile, aborting"))
        clear_attack_goal(state, "shot does not reach pill")
        return
      elseif avail_shots < total_needed and not blitz_2plus
             and not (goal._charge_now and C.CHARGE_NOW_IGNORE_SAFETY) then
        -- 2+ blitz exemption (same as charge): the ally(ies) supply finishing
        -- shells, so our own magazine running short isn't a reason to bail.
        -- CHARGE NOW IGNORES SAFETY: no shell abort either.
        print(string.format(TAG .. " SHOOT_PILL: not enough shells (%d obstacles + %d hp = %d needed, have %d + %d in-flight = %d) — aborting",
          obstacle_shots, pill_hp, total_needed, info.shells, in_flight, avail_shots))
        clear_attack_goal(state, "not enough shells to finish take")
        return
      end
      goal._bullets_needed = total_needed
    end

    -- No-progress timeout. shoot_pill has no built-in escape if the
    -- shells are silently missing (trajectory off, friendly LGM in
    -- the lane, pill picked up — all leave pill HP unchanged while
    -- we keep "firing"). Track the highest pill HP we've observed
    -- and the last tick HP went DOWN. If too long without progress,
    -- abort the take so the goal selector can re-plan from scratch.
    --
    -- Threshold: 200 ticks (~4 s @ 50 Hz). Reload is ~0.5 s so we
    -- expect ~8 shells fired in that window — none landing means
    -- something's actually wrong, not just bad luck.
    local SHOOT_NO_PROGRESS_TICKS = 200
    if not goal._shoot_progress_hp then
      goal._shoot_progress_hp   = pill_hp
      goal._shoot_progress_tick = now
      goal._shoot_initial_hp    = pill_hp  -- baseline for the kill bar
    elseif pill_hp < goal._shoot_progress_hp then
      goal._shoot_progress_hp   = pill_hp
      goal._shoot_progress_tick = now
    elseif (now - (goal._shoot_progress_tick or now)) > SHOOT_NO_PROGRESS_TICKS
       and pill_hp > 0 then
      print(string.format(TAG ..
        " ATTACK: shoot_pill no progress for %d ticks (pill_hp=%d) — aborting take",
        now - (goal._shoot_progress_tick or now), pill_hp))
      clear_attack_goal(state, string.format("shoot_pill no progress %dt (hp=%d)",
        now - (goal._shoot_progress_tick or now), pill_hp))
      -- clear_attack_goal mutates state.goal in place (wipes all
      -- non-core fields, sets kind="none"). The local `goal` here
      -- aliases the same table, so downstream reads like
      -- `goal._shoot_hits_total >= C.ATTACK_CURVE_AFTER_HITS` would
      -- compare nil and crash. Bail out of the substate handler
      -- immediately — the goal selector will pick a fresh goal next
      -- tick. (Brain crash on May 1: this exact path, line 2461 of
      -- 1c528b1.)
      return
    end

    -- HUD: live progress toward each of the three exit triggers.
    -- Bar fills as we approach the exit (kill / swerve-from-hits / abort).

    local should_swerve = false
    local pill_dead     = false
    -- "Blitz only when hit": hold the proactive (kill-locked) swerve while an
    -- undamaged committed blitzer in a 3+ party. The pill-dead and hits-taken
    -- exits are not gated (see below). Falls through to the substate handler.
    local _commit_hold = blitz_commit_hold(goal, state, info, now)
    local on_target_in_flight = goal._on_target_in_flight or 0
    -- Tank-the-finish: honor the ONE-TIME soak decision committed at
    -- build_walls/charge (lazily committed here if this take skipped those).
    -- Soak only applies once the pill is at its last HP, AND only while the pill
    -- is CALM (anger <= TANK_FINISH_MAX_ANGER) — an angrier pill reloads fast, so
    -- don't stand in to finish it; let the kill-shot swerve below dodge instead.
    local soak_ok = commit_soak_finish(goal, state, info)
    local tank_finish = soak_ok and pill and (pill.health or 0) > 0
                        and (pill.health or 0) <= (C.TANK_FINISH_MAX_HP or 3)
                        and (pill.anger or 0) <= (C.TANK_FINISH_MAX_ANGER or 0.25)
    if pill_hp <= 0 then
      -- Pill actually dead → kill swerve (rush to capture after).
      -- Pill-suicider: no swerve at all — run the post-kill capture handoff
      -- right here instead of after a swerve, then bail (goal is wiped).
      if state.is_pill_suicider then
        suicider_kill_handoff(goal, state, world, info, now, "shoot_pill_ppt")
        return
      end
      -- Blitz-only-when-hit: undamaged committed 3+ blitzer commits onto the
      -- corpse instead of peeling; the dodge returns once hit.
      if _commit_hold then
        suicider_kill_handoff(goal, state, world, info, now, "shoot_pill_ppt", "blitz_hold")
        return
      end
      should_swerve = true
      pill_dead     = true
    elseif goal._kill_attempt and on_target_in_flight >= pill_hp and not _commit_hold then
      -- Last sure shot fired: the in-flight shells whose simulated path actually
      -- reaches the pill already cover its remaining HP, so the kill is locked —
      -- curve away NOW instead of standing another tick under return fire. Stays
      -- a defensive swerve (pill not dead yet) so a diverging shell just re-engages.
      -- NOT gated by tank_finish: the soak only buys us standing time to FIRE the
      -- lethal shot — once it's in flight there's no reason to keep sitting, even
      -- on a calm low-HP pill. (Swerve as soon as the killing shot is away.)
      should_swerve = true
    elseif goal._shoot_hits_total >= C.ATTACK_CURVE_AFTER_HITS and not tank_finish then
      should_swerve = true
    end
    -- Pill-suicider: cancel every DEFENSIVE swerve (pill still alive — the
    -- kill-locked and hits-taken exits above). The pill-dead case never reaches
    -- here: it returned above through suicider_kill_handoff, because a suicider
    -- does not swerve in ANY situation — not even the rush-to-capture one.
    if should_swerve and not pill_dead and state.is_pill_suicider then
      should_swerve = false
    end

    if should_swerve then
      enter_swerve(goal, world, state, info, pmx, pmy,
                   pill_dead and "kill" or "defensive")
      print(string.format(TAG .. " ATTACK: PPT shoot_pill -> swerve (hits=%d hp=%d dead=%s)",
            goal._shoot_hits_total or 0, pill_hp, tostring(pill_dead)))
    end
    -- Steering handles braking, aim hold, and shooting; fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- engage: stationary, fire at pill
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "engage" then
    -- Track armour to detect incoming hits
    if not goal._engage_armour then goal._engage_armour = info.armour end
    local hits_taken = (info.armour < goal._engage_armour) and 1 or 0   -- count HITS, not armour points (see charge note)
    goal._engage_armour = info.armour

    -- Track bullets fired since charge started
    local bullets_fired = (goal._charge_shells or info.shells) - info.shells
    local pill_hp = pill and pill.health or 0

    -- Keep firing until the pill is ACTUALLY dead (cyan 0/0) — don't swerve on
    -- the in-flight prediction, which stops a shot short if a shell diverges.
    local on_target_in_flight = goal._on_target_in_flight or 0
    if pill_hp <= 0 then
      -- Pill-suicider: no kill swerve — hand straight to capture_pill on the
      -- body we just made, then bail out (clear_attack_goal wiped `goal`).
      if state.is_pill_suicider then
        suicider_kill_handoff(goal, state, world, info, now, "engage")
        return
      end
      -- Blitz-only-when-hit: undamaged committed 3+ blitzer commits onto the
      -- corpse instead of peeling; the dodge returns once hit.
      if blitz_commit_hold(goal, state, info, now) then
        suicider_kill_handoff(goal, state, world, info, now, "engage", "blitz_hold")
        return
      end
      enter_swerve(goal, world, state, info, pmx, pmy, "kill")
      goal._swerve_pill_dead = (pill_hp <= 0)
      print(string.format(TAG .. " ATTACK: immediate swerve (fired=%d in_flight=%d hp=%d kill_attempt=%s start_hp=%s)",
            bullets_fired, on_target_in_flight, pill_hp,
            tostring(goal._kill_attempt), tostring(goal._charge_start_hp)))
    else
      -- Count cumulative hits taken during this engage
      goal._engage_hits = (goal._engage_hits or 0) + hits_taken

      -- Swerve early: after taking ATTACK_CURVE_AFTER_HITS hits, dodge
      -- This triggers while crosshairs are still on pill — preemptive evasion.
      -- Skip when the ONE-TIME soak decision (committed at charge) says soak AND
      -- the pill is at its last HP: buck in to finish instead of peeling off.
      local _soak_ok = commit_soak_finish(goal, state, info)
      local _tank_finish = _soak_ok and pill and (pill.health or 0) > 0
                           and (pill.health or 0) <= (C.TANK_FINISH_MAX_HP or 3)
                           and (pill.anger or 0) <= (C.TANK_FINISH_MAX_ANGER or 0.25)
      -- "Blitz only when hit": an UNDAMAGED committed blitzer in a 3+ party holds
      -- its proactive swerves. Folded into _kill_locked (kills that disjunct in
      -- BOTH should_swerve and the crosshairs_off branch) and added to the
      -- crosshairs_off condition below so the ANGER disjunct routes to post_engage
      -- (same as a suicider) rather than dodging. Hits-taken and pill-dead are not
      -- gated; while _commit_hold holds we are undamaged, so _engage_hits is 0.
      local _commit_hold = blitz_commit_hold(goal, state, info, now)
      -- Last sure shot fired (in-flight on-target shells already cover the pill's
      -- remaining HP) → kill is locked, dodge now. Only on a kill attempt, and
      -- not while soaking the last HP of a calm pill.
      local _kill_locked = goal._kill_attempt
                           and (goal._on_target_in_flight or 0) >= pill_hp
                           and pill_hp > 0 and not _tank_finish
      -- "Blitz only when hit": an undamaged committed blitzer in a 3+ party holds
      -- its PROACTIVE (crosshairs-ON) dodge -- keep firing instead of peeling off.
      -- The crosshairs-OFF threat case below RE-AIMS rather than retreating; the
      -- dodge resumes once hit (_commit_hold releases on the armour drop). NOTE
      -- _kill_locked stays ungated so the crosshairs-off branch can still see the
      -- threat and re-aim on it.
      local should_swerve = (goal._engage_hits >= C.ATTACK_CURVE_AFTER_HITS or _kill_locked)
                            and not _tank_finish and not _commit_hold
      -- Pill-suicider: hits taken and a locked kill are NOT reasons to peel off
      -- — it stands in the fire and keeps shooting until the pill dies (the
      -- pill_hp<=0 branch above then hands straight to capture_pill with no
      -- swerve at all). The crosshairs_off exit below is left alone: that one
      -- isn't a dodge, it means we can no longer hit anything from here, and a
      -- suicider with no shot has nothing to be brave about — but it routes to
      -- post_engage, never to a swerve.
      if should_swerve and state.is_pill_suicider then
        should_swerve = false
      end

      -- Also check if crosshairs off pill (knocked out of range)
      -- Float angle so the displayed crosshair matches the engine's
      -- actual firing direction; info.direction is a floor and would
      -- show the crosshair offset from the true shell path.
      local cx, cy = U.crosshair_at(info.tankx, info.tanky,
                                    info.tank_angle or info.direction, 7.0)
      local cdx, cdy = cx - (pmx + 0.5), cy - (pmy + 0.5)
      local crosshairs_off = math.sqrt(cdx * cdx + cdy * cdy) > 0.7

      if should_swerve or crosshairs_off then
        local pill_anger = pill and pill.anger or 0
        -- (suicider: should_swerve is already false above, so we only get here
        -- on crosshairs_off — route that to post_engage, never to a swerve.)
        if (pill_anger > C.ANGER_ATTACK_THRESHOLD or _kill_locked)
           and not state.is_pill_suicider then
          if _commit_hold then
            -- Held blitzer keeps its sight on the pill; the dodge resumes once hit (_commit_hold false).
            goal.substate = "aim"
            goal.aim_tick = now
          else
          -- Pill angry, OR the kill is already locked (last sure shot fired) —
          -- swerve to dodge (defensive, so a diverging shell re-engages). The
          -- kill-locked case dodges regardless of anger instead of falling to
          -- post_engage, so we never abandon the take with the pill still alive.
          enter_swerve(goal, world, state, info, pmx, pmy, "defensive")
          print(string.format(TAG .. " ATTACK: swerving (hits=%d anger=%.2f xhair_off=%s)",
                goal._engage_hits or 0, pill_anger, tostring(crosshairs_off)))
          end
        else
          -- Pill is calm (or we're a suicider, which never dodges) — go
          -- straight to loiter/refuel decision
          goal.substate = "post_engage"
          goal._post_engage_tick = now
        end
      -- Abort if can't aim within 3 seconds (~150 ticks)
      elseif goal.engage_tick and not goal._engage_aimed
             and (now - goal.engage_tick) > 150 then
        print(TAG .. " ATTACK: engage timeout — can't aim, aborting")
        clear_attack_goal(state, "engage aim timeout (>150t)")
      end
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- loiter: wait outside pill range for anger to decay, then re-engage
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "loiter" then
    if not pill or pill.health <= 0 then
      -- Pill killed (by us-via-leftover-shells or by an ally) while
      -- we waited. Release to capture_pill via the normal selector
      -- instead of locking into rush — see swerve completion above.
      mark_kill_pickup(state, goal.target_id, goal.mx, goal.my, now)
      clear_attack_goal(state, "pill died during loiter")
      print(TAG .. " ATTACK: pill died during loiter — releasing to capture_pill")
    elseif pill.anger and pill.anger < C.ANGER_ATTACK_THRESHOLD then
      -- Pill calmed down — re-engage with a FULL fresh plan. We have
      -- to wipe everything plan_position will re-derive (scan grid,
      -- PPT decision, shield-scan, chosen aim, ranged-armour
      -- counters) so a take that was PPT against a tough pill
      -- doesn't re-enter PPT machinery against the same pill now at
      -- 1-2 HP — the right move on a weakened pill is the cheap
      -- engage path, but only plan_position knows that.
      goal.substate              = "plan_position"
      goal.scan_spots            = nil
      goal._plan_show_tick       = nil
      goal._plan_logged          = nil
      goal._is_ppt               = nil
      goal._shield_scan          = nil
      goal._aim_locked           = nil
      goal._pre_aim_locked       = nil
      goal._finetune_taps        = nil
      goal._finetune_start       = nil
      goal._finetune_path        = nil
      goal._finetune_on_pill     = nil
      goal.aim_mx                = nil
      goal.aim_my                = nil
      goal.aim_wx                = nil
      goal.aim_wy                = nil
      goal.aim_idx               = nil
      goal.through_walls         = nil
      goal._shoot_armour         = nil
      goal._shoot_shells         = nil
      goal._shoot_hits_total     = nil
      goal._shoot_progress_hp    = nil
      goal._shoot_progress_tick  = nil
      goal._engage_armour        = nil
      goal._engage_hits          = nil
      goal._charge_braking       = nil
      goal._charge_shells        = nil
      goal._charge_start_hp      = nil
      goal._kill_attempt         = nil
      goal._bullets_needed       = nil
      goal._on_target_in_flight  = nil
      goal._swerve_extends       = nil
      print(string.format(TAG ..
        " ATTACK: pill cooled (anger=%.2f, hp=%d), re-planning from scratch",
        pill.anger, pill.health or 0))
    elseif goal._loiter_start and (now - goal._loiter_start) > effective_anger_wait_max(pill, info) then
      -- Loitered too long. Same fix as post_engage refuel branch
      -- below: don't clear_attack_goal (causes capture_base to steal
      -- via cur_group=none penalty stack). Instead request urgent
      -- replan while keeping goal.kind=attack_pill so the wounded
      -- pill stays competitive as the incumbent.
      state.wounded_pill = { id = goal.target_id, mx = pmx, my = pmy, hp = pill.health, tick = now, owner = pill.owner }
      state._force_replan_reason = "loiter_timeout"
      goal.substate    = "plan_position"
      goal.scan_spots  = nil
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- swerve: hard turn to dodge pill's predictive aim after engage
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "swerve" then
    -- Per-tick swerve trace: capture pill state every tick we're in
    -- swerve so we can pinpoint the exact tick a pill flipped to
    -- dead/friendly/nil. BRAIN_LOG_SWERVE-gated so it's opt-in even
    -- with debug on (chatty — fires every swerve tick).

    -- If pill dies mid-swerve, shorten the turn portion to 25 ticks remaining
    if not goal._swerve_pill_dead then
      local pill_now_dead = (not pill or pill.health <= 0)
      if pill_now_dead then
        goal._swerve_pill_dead = true
        if (goal._swerve_turn_ticks_left or 0) > C.SWERVE_TURN_TICKS then
          goal._swerve_turn_ticks_left = C.SWERVE_TURN_TICKS
          if (goal._swerve_ticks_left or 0) > C.SWERVE_TOTAL_TICKS then
            goal._swerve_ticks_left = C.SWERVE_TOTAL_TICKS
          end
          print(TAG .. " ATTACK: swerve shortened — pill confirmed dead")
        end
      end
    end
    -- Threat-driven swerve lifecycle (armed / unloaded). `active` = at least one
    -- hostile/neutral shell WILL HIT us — its closest approach computed in our
    -- MOVING frame (relative velocity) lands within SWERVE_HIT_RADIUS_WU, i.e. a
    -- red marker sits ON the tank. So `active` already accounts for our dodge:
    -- once we maneuver out of every shell's path, nothing hits and active drops.
    --   * UNLOADED at start. If nothing is going to hit us within SWERVE_ARM_TIMEOUT
    --     (~1s), no fire is connecting -> finish (peel off, nothing to dodge).
    --   * ARMS the first tick `active` is true (a shot will hit on our current line).
    --   * Once ARMED: keep swerving while `active`; finish the instant it isn't —
    --     no red marker on the tank means we've dodged them all.
    local active, shell_detail = danger.shells_incoming_near(info, info.tankx, info.tanky)
    goal._swerve_shell_detail = shell_detail  -- stashed for the viz draw below
    local elapsed = now - (goal._swerve_start or now)
    local finish = false
    if not goal._swerve_armed then
      if active then
        goal._swerve_armed = true
      elseif elapsed >= (C.SWERVE_ARM_TIMEOUT_TICKS or 50) then
        finish = true
      end
    elseif not active then
      finish = true
    end
    -- The evasive TURN portion still counts down (it steers the dodge) but no
    -- longer gates completion — threat presence does.
    if (goal._swerve_turn_ticks_left or 0) > 0 then
      goal._swerve_turn_ticks_left = goal._swerve_turn_ticks_left - 1
    end
    goal._swerve_ticks_left = finish and 0 or 1
    if goal._swerve_ticks_left <= 0 then
      -- Swerve done — check if pill died.
      -- Detailed diagnostic logged BEFORE the dead-check so we can see
      -- exactly which state drove the decision. BRAIN_LOG_SWERVE gate.
      if not pill or pill.health <= 0 then
        -- Drop the attack_pill goal, then hand it straight to capture_pill on
        -- the pill we just killed. The old behaviour left goal=none and waited
        -- for the next replan to pick the body out of the pool — a gap refuel
        -- or flee could win. handoff_to_capture_pill installs the goal now AND
        -- raises _force_replan_reason, so the selector (Override 3b, with its
        -- ally handoff and reachability way-out) still gets its say next tick;
        -- it just never sees a tick where we have no goal at all. If the
        -- handoff refuses (ally already capturing, pill carried/gone), we fall
        -- back to the old release-to-the-selector behaviour.
        local _pid = goal.target_id
        mark_kill_pickup(state, _pid, goal.mx, goal.my, now)
        clear_attack_goal(state, "swerve done, pill dead")
        if handoff_to_capture_pill(state, world, info, _pid, now) then
          print(TAG .. " ATTACK: swerve done, pill dead — taking capture_pill on it")
        else
          print(TAG .. " ATTACK: swerve done, pill dead — releasing to capture_pill")
        end
      elseif (goal._on_target_in_flight or 0) >= pill.health then
        -- Pill still alive but enough on-target shells are in flight
        -- to expect a kill. Don't drop into post_engage yet (it'd
        -- pick loiter/refuel based on the pill being "alive" before
        -- our inbound shells finish the job). Extend the swerve a
        -- bit and recheck — usually the in-flight shots resolve
        -- within a tick or two. Cap the extensions so a stuck
        -- in-flight count can't loop forever.
        goal._swerve_extends = (goal._swerve_extends or 0) + 1
        if goal._swerve_extends <= 30 then
          goal._swerve_ticks_left = 1
          if goal._swerve_extends == 1 then
            print(string.format(TAG ..
              " ATTACK: swerve extending — %d on-target in flight vs hp=%d",
              goal._on_target_in_flight or 0, pill.health))
          end
        else
          -- Give up extending; treat as a real swerve completion.
          if state.command_goal then
            print(TAG .. " ATTACK: command pill take done — releasing command, replanning")
            state.command_goal = nil
            clear_attack_goal(state, "command take done (swerve extends exhausted)")
          else
            goal.substate = "post_engage"
            goal._post_engage_tick = now
            print(TAG .. " ATTACK: swerve done (extends exhausted), evaluating next move")
          end
        end
      else
        -- If this was a manual command (ctrl-click), clear it so normal
        -- goal selection takes over instead of loitering/refueling.
        if state.command_goal then
          print(TAG .. " ATTACK: command pill take done — releasing command, replanning")
          state.command_goal = nil
          clear_attack_goal(state, "command take done (swerve complete)")
        else
          goal.substate = "post_engage"
          goal._post_engage_tick = now
          print(TAG .. " ATTACK: swerve done, evaluating next move")
        end
      end
    end
    -- HUD overlay: show raw swerve goal._* values (screen-relative)
    -- Incoming-shell scan (relative frame): for each hostile/neutral shell, a line
    -- to its closest-approach point computed against our MOVING tank. RED = it'll
    -- HIT (relative CPA within SWERVE_HIT_RADIUS_WU → marker sits on the tank),
    -- GREEN = it misses. Yellow ring = scan radius; orange ring = the hit zone.
    -- The cyan ghost is where we PREDICT the tank to be (our velocity projected
    -- forward) — the path the dodge math projects against.
    -- During swerve: do NOT check pill health or allow any interrupts.
    -- Swerve MUST complete to minimize damage taken.
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- post_engage: decide loiter vs refuel after engage/swerve
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "post_engage" then
    local pill_anger = pill and pill.anger or 1.0
    local ticks_to_calm = math.max(0, (pill_anger - C.ANGER_ATTACK_THRESHOLD) * C.PILL_ANGER_DECAY)

    local refuel_cost = math.huge
    for _, b in pairs(world.bases) do
      if b.owner == "friendly" then
        local there = cpf.estimate_cost(tmx, tmy, b.mx, b.my, info.inboat and 1 or 0)
        local back  = cpf.estimate_cost(b.mx, b.my, pmx, pmy, 0)
        local trip = there + back + 100
        if trip < refuel_cost then refuel_cost = trip end
      end
    end

    local _anger_wait_max = effective_anger_wait_max(pill, info)
    if ticks_to_calm < refuel_cost and ticks_to_calm < _anger_wait_max then
      goal.substate = "loiter"
      goal._loiter_start = now
      print(string.format(TAG .. " ATTACK: loitering (wait=%d vs refuel=%d, cap=%d hp=%d arm=%d)",
            math.floor(ticks_to_calm), refuel_cost < math.huge and math.floor(refuel_cost) or 99999, math.floor(_anger_wait_max), pill and pill.health or -1, info.armour or -1))
    else
      -- Refuel beats loiter for this take. Don't clear_attack_goal here
      -- — that drops state.goal to none, and on the next replan EVERY
      -- candidate (including the wounded pill we want to come back to)
      -- pays the +sw+commit hysteresis penalty because cur_group=none.
      -- That penalty stack lets capture_base steal the goal even when
      -- the wounded pill is 1 HP. Instead: keep goal.kind=attack_pill
      -- so cur_group stays "attack", flag the brain to replan urgently,
      -- and reset substate to plan_position so if pick_goal keeps us
      -- on this pill the take continues from a fresh scan. If refuel
      -- really is cheaper (low armour → low pool-1 urgency score), it
      -- wins the competition and we bail normally.
      state.wounded_pill = { id = goal.target_id, mx = pmx, my = pmy, hp = pill and pill.health or 0, tick = now, owner = pill and pill.owner or nil }
      state._force_replan_reason = "post_engage_refuel"
      goal.substate    = "plan_position"
      goal.scan_spots  = nil  -- force fresh plan_position scan if we stay
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- rush: pill dead, drive to pill tile to capture
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "rush" then
    if pill and pill.health > 0 then
      -- Pill came back (repaired?), re-engage
      goal.substate = "engage"
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- DRAW swerve direction choice (left/right cover sample lines)
  -- ══════════════════════════════════════════════════════════════════

  -- Shield-scan overlay: 8 candidate spots + winner blocker tiles.
  -- Drawn while planning so a human can see where the alternate
  -- standoffs landed and what's giving cover. Draws every tick the
  -- goal still owns _shield_scan — but ONLY when that scan is for the
  -- pill this goal is currently taking. A _shield_scan can leak across a
  -- re-target (goal object reused for a new pill before the scan is
  -- recomputed/cleared); without this guard the followed tank would show
  -- a different pill's candidate cluster — a take it's no longer on.
  -- (Followed-tank filtering is already handled by the host binding
  -- overlay_* only in the followed bot, so this is purely the pill guard.)
  local _ssc = goal._shield_scan
  if _ssc and _ssc.pill and _ssc.pill.mx == goal.mx and _ssc.pill.my == goal.my then
    shield.draw_overlay(_ssc, now)

    -- Pronounced TARGET marker on the chosen aim point. Drawn on top
    -- of the per-aim borders so the user can verify the corner the
    -- shield scan actually chose vs which one charge/shoot is using.
    -- Stays visible during all PPT substates (in_range_position /
    -- in_range_aim / shoot_pill) and the legacy aim/charge/engage
    -- substates too.
  end

  -- Build-walls decision banner: re-emit each tick until expiry so the
  -- text actually persists on screen (overlay commands are cleared per
  -- frame). 100 ticks ~= 2 seconds at the 50 Hz sim rate.
  if goal._build_decision_msg and goal._build_decision_until and
     now < goal._build_decision_until and BRAIN_DEBUG_MODE and viz.is_on("build_decision_banner") then
    local twx = info.tankx / 256.0
    local twy = info.tanky / 256.0
    local r, g, b = 255, 240, 100        -- default yellow
    local scale = 0.5
    if goal._build_decision_is_failure then
      r, g, b = 255, 60, 60               -- red for 0-BUILT failures
      scale = 0.7                          -- bigger so it's unmissable
    end
  end

  -- Build-walls status / timeout countdown. Drawn at TWO positions so
  -- it's always visible: once next to the standoff (in case the camera
  -- is on the pill), once next to the tank (in case the camera is on
  -- the tank). Shows three states:
  --   - countdown (queue active)
  --   - "build pending" (scan winner exists, queue not yet built)
  --   - nothing if no shield scan at all
  if goal._shield_scan then
    -- Color helper: drains green -> yellow -> red as fraction goes
    -- from 1 (fresh) to 0 (timeout).
    local function color_for(frac)
      if frac > 0.5 then return 100, 230, 100
      elseif frac > 0.2 then return 230, 220, 80
      else return 230, 60, 60 end
    end

    local labels = {}  -- list of {label, r, g, b, viz_id}

    -- Approach timeout while in approach substate.
    if goal.substate == "approach" and goal._approach_last_progress
       and goal._approach_timeout_total then
      local total = goal._approach_timeout_total
      local ticks_left = total - (now - goal._approach_last_progress)
      if ticks_left < 0 then ticks_left = 0 end
      local r, g, b = color_for(ticks_left / total)
      labels[#labels + 1] = {
        string.format("APPROACH t-%d (%.1fs)", ticks_left, ticks_left / 50.0),
        r, g, b, "build_status" }
    end

    -- Build-walls: countdown if active, "pending" hint otherwise.
    if goal._wall_build_list and goal._wall_build_last_progress
       and goal._build_timeout_total then
      local total = goal._build_timeout_total
      local ticks_left = total - (now - goal._wall_build_last_progress)
      if ticks_left < 0 then ticks_left = 0 end
      local r, g, b = color_for(ticks_left / total)
      local idx_now = goal._wall_build_idx or 0
      labels[#labels + 1] = {
        string.format("BUILD t-%d (%.1fs)  q=%d/%d",
                      ticks_left, ticks_left / 50.0, idx_now,
                      #goal._wall_build_list),
        r, g, b, "build_status" }
      -- Per-wall stall timer (WALL_STALL_TICKS=250). Tile-type
      -- change (forest→grass after harvest, grass→half-build after
      -- build start) resets it; only fires when the LGM is genuinely
      -- not progressing on this slot.
      if goal._wall_idx_started then
        local STALL = 250
        local stall_left = STALL - (now - goal._wall_idx_started)
        if stall_left < 0 then stall_left = 0 end
        local sr, sg, sb = color_for(stall_left / STALL)
        local cur_target = goal._wall_build_list[idx_now]
        local tt_str = (goal._wall_idx_prev_tt ~= nil)
          and string.format(" tt=%d", goal._wall_idx_prev_tt) or ""
        labels[#labels + 1] = {
          string.format("WALL t-%d (%.1fs) @(%d,%d)%s",
                        stall_left, stall_left / 50.0,
                        cur_target and cur_target.mx or -1,
                        cur_target and cur_target.my or -1,
                        tt_str),
          sr, sg, sb, "build_status" }
      end
    elseif goal.substate ~= "approach" then
      -- Don't double up with the APPROACH timer — only show the
      -- "BUILD pending" line once approach is finished.
      labels[#labels + 1] = {
        string.format("BUILD pending (sub=%s)", goal.substate or "?"),
        180, 180, 180, "build_status" }
    end

    -- Wall-shield builder skip reason — surfaces silent gate failures
    -- (no trees, angry pill, LGM can't reach, path unsafe) right next
    -- to the build queue so the user sees why a wall isn't going up
    -- instead of staring at an idle tank.
    --
    -- Window is generous (200 ticks ≈ 4 s) so a one-off skip stays
    -- legible long enough to read; once a skip is older than that
    -- it's stale and likely no longer the active blocker.
    if goal.substate == "build_walls" and state._wall_shield_skip
       and (now - state._wall_shield_skip.tick) < 200 then
      local s = state._wall_shield_skip
      local parts = {}
      if not s.has_trees    then parts[#parts + 1] =
        string.format("OUT_OF_TREES(have=%d need=%d)",
                      s.trees_have or 0, s.trees_need or 0) end
      if not s.can_reach    then parts[#parts + 1] = "LGM_NO_REACH" end
      if not s.path_safe    then parts[#parts + 1] = "LGM_PATH_UNSAFE" end
      if s.angry_pill_close then parts[#parts + 1] = "ANGRY_PILL_NEAR" end
      if #parts > 0 then
        local age = now - s.tick
        labels[#labels + 1] = {
          string.format("WALL_SKIP (%dt ago): %s @(%d,%d)%s",
                        age, table.concat(parts, " "),
                        s.wx or 0, s.wy or 0,
                        s.force_mode and "  [force_mode: safety bypassed]" or ""),
          230, 80, 80, "wall_skip_reason" }
      end
    end

    -- LGM status during build_walls: if the LGM isn't on the ground
    -- (in tank / dead / parachuting) it physically cannot go out to
    -- build walls.  Surface that directly rather than letting the
    -- user wonder why nothing's happening.
    if goal.substate == "build_walls" and info.man_status ~= nil then
      local lgm_msg
      if info.man_status == C.LGM_INTANK then
        lgm_msg = "LGM_IN_TANK — needs to be dispatched"
      elseif info.man_status == C.LGM_DEAD then
        lgm_msg = "LGM_DEAD — no walls possible until respawn"
      end
      if lgm_msg then
        labels[#labels + 1] = { lgm_msg, 230, 80, 80, "wall_skip_reason" }
      end
    end

    -- Anchor labels near the TANK (not 4 tiles diagonally away from
    -- the standoff — that put them off-screen at normal zoom and the
    -- user reported never seeing the BUILD countdown). Stack 0.6 tile
    -- per line so multiple labels (BUILD + WALL_SKIP + APPROACH) read
    -- vertically.
    do
      local twx = info.tankx / 256.0
      local twy = info.tanky / 256.0
      for i, lbl in ipairs(labels) do
      end
    end
  end

  -- ══════════════════════════════════════════════════════════════════
  -- DRAW scan results every tick (persisted in goal.scan_spots)
  -- ══════════════════════════════════════════════════════════════════

  -- ══════════════════════════════════════════════════════════════════
  -- Persistent chosen-standoff marker: solid beige disc + pill id label.
  -- Drawn every tick the attack goal has a standoff, so it stays visible
  -- until the standoff is replaced (e.g. shield re-pick) or the goal ends.
  -- Disc faked with concentric outline circles since the overlay primitive
  -- has no fill mode.
  -- ══════════════════════════════════════════════════════════════════
  if goal.standoff_fx or goal.standoff_mx then
    local sfx = goal.standoff_fx or (goal.standoff_mx + 0.5)
    local sfy = goal.standoff_fy or (goal.standoff_my + 0.5)
    local R, G, B = 245, 222, 179   -- beige
    for i = 0, 10 do
    end
    if goal.target_id then
    end
  end

end

-- A wall-pass take (goal.through_walls, C.ATTACK_PILL_WALL_FALLBACK) runs with
-- spot_margin's walls_ok on, so every shell-line test it makes (re-aim ladder,
-- blitz margin, spot checks) treats built walls as shells to spend. Off again
-- on the way out; walls_reset at the top of each think covers a budget kill.
local function walls_off(...)
  SM.walls_ok = false
  return ...
end

function M.update_attack_substate(goal, state, world, info)
  SM.walls_ok = (goal and goal.through_walls) and true or false
  return walls_off(update_attack_substate_body(goal, state, world, info))
end

function M.walls_reset()
  SM.walls_ok = false
end

-- =========================================================================
-- attack_tank: heat a FRIENDLY pillbox mid-fight (C.ATTACK_TANK_HEAT_PILL)
-- =========================================================================
-- While we are fighting enemy tank E, a friendly pill that sits CLOSER to E
-- than we do is a second gun already in position -- but only if it is angry.
--
-- ENGINE FACTS (all cited, none guessed):
--
--  * A pill's "anger" IS its reload period, the `speed` field.
--    PILLBOX_ATTACK_NORMAL = 100 is calm, PILLBOX_MAX_FIRERATE = 6 is as
--    angry as a pill can get   (src/bolo/internal/pillbox.h:46-48).
--  * Anger is NOT additive.  Every shell that DAMAGES a pill HALVES it:
--        speed /= 2, then clamped up to PILLBOX_MAX_FIRERATE
--    (src/bolo/pillbox.c:504-511, and the identical block at :1393-1398).
--    So the ladder from calm is
--        100 -> 50 -> 25 -> 12 -> 6
--    i.e. exactly HEAT_MAX_HITS (4) hits to max heat.  A 5th
--    hit does nothing to speed: the halving is guarded by `speed > 6`.
--  * OUR OWN shell angers our OWN pill, and costs it 1 armour.  The server
--    branch calls pillsDamagePos(sim, mapX, mapY, TRUE, TRUE, owner) for ANY
--    shell landing on a pill tile with NO ownership test at all
--    (src/bolo/shells.c:658-661; the damage itself at pillbox.c:485-487).
--    That is the whole reason for the HP floor below.
--  * Cool-down: coolDown counts down each tick and, at 0, gives back exactly
--    +1 speed and re-arms PILLBOX_COOLDOWN_TIME = 32 ticks
--    (src/bolo/pillbox.c:347-355).  So 6 -> 100 takes 94 x 32 = 3008 ticks --
--    which is what C.PILL_ANGER_DECAY (3000) models.  Our volley is 4 shots
--    = 3 x TANK_RELOAD_TIME (13) = 39 engine ticks plus flight
--    (src/bolo/internal/tank.h:161), over which the pill claws back at most
--    1-2 speed points; the next hit halves them straight back off, so
--    cool-down DURING the volley never adds a shot to the count.  (Worst
--    case the volley ends on speed 7 instead of 6.)
--
-- WHAT THE BRAIN CAN ACTUALLY SEE: not `speed`.  world.lua:285-291 keeps a
-- PROXY, pill.anger in [0,1], which adds C.PILL_ANGER_BUMP (0.3333) per
-- OBSERVED armour drop and decays linearly over PILL_ANGER_DECAY.  Since one
-- observed armour drop == one halving, the proxy is just a hit counter:
--        hits_landed   = round(anger / PILL_ANGER_BUMP)     [0 .. FULL_HITS]
--        shots_needed  = FULL_HITS - hits_landed            [floored at 0]
-- (The proxy saturates at 3 bumps = 0.9999 while the engine wants 4 hits;
-- counting in HITS rather than trusting the proxy's ceiling is what makes the
-- last shot happen.)
--
-- Progress is measured in HITS THAT LANDED -- the pill's observed armour
-- drop since the volley started -- not in key presses, with a bounded miss
-- allowance (ATTACK_TANK_HEAT_MAX_MISSES) on the shells-fired delta so a
-- volley that keeps missing still terminates.

-- HEAT_MAX_HITS is NOT a tunable. It is the length of the engine's halving
-- ladder, counted off src/bolo/pillbox.c:504-511: from PILLBOX_ATTACK_NORMAL
-- (100) the sequence `speed /= 2` clamped up to PILLBOX_MAX_FIRERATE (6) runs
--     100 -> 50 -> 25 -> 12 -> 6
-- and stops, because the halving is guarded by `speed > 6`. Four hits, and a
-- fifth changes nothing. Turning it into a knob would only ever let a bot ask
-- the engine for a fifth halving that does not exist.
local HEAT_MAX_HITS = 4

-- Hits already landed on a pill, inferred from world.lua's anger proxy.
local function heat_hits_landed(anger)
  local bump = C.PILL_ANGER_BUMP or 0.3333
  if bump <= 0 then return 0 end
  local n = math.floor((anger or 0) / bump + 0.5)
  if n < 0 then n = 0 elseif n > HEAT_MAX_HITS then n = HEAT_MAX_HITS end
  return n
end

-- Shells needed to take a pill at `anger` all the way to PILLBOX_MAX_FIRERATE.
function M.heat_pill_shots_needed(anger)
  local n = HEAT_MAX_HITS - heat_hits_landed(anger)
  if n < 0 then n = 0 end
  return n
end

-- HOW MANY HITS THIS PILL'S HEALTH CAN AFFORD (Andrew, 2026-09-06:
-- "proportionally less shots to heat up depending how hurt the pill is").
-- Every heat shell costs the pill 1 armour (src/bolo/shells.c:658-661 ->
-- pillbox.c:485-487), so a wounded pill buys its rate of fire with armour it
-- may not have. The allowance scales linearly from "full health, all four
-- halvings" down to "at the floor, none at all":
--     allowed = round(HEAT_MAX_HITS x (hp - MIN_HP) / (PILLS_MAX_HEALTH - MIN_HP))
--     hp 15 -> 4    13 -> 3    10 -> 2    7 -> 1    6 -> 0    <=5 -> 0
-- NOTE ON ROUNDING: the spec said "floor", but floor sends hp 7 to 0 while the
-- spec's own worked table says 7 -> 1. Rounding reproduces every value in that
-- table (15/13/10/7/6/5 -> 4/3/2/1/0/0); floor reproduces all but hp 7. The
-- enumerated table is the intent, so this rounds.
-- This REPLACES the old per-shot "hp >= 5" test: the cap is computed ONCE at
-- entry, so a pill at 10 fires exactly 2 (its health falling to 8 mid-volley
-- does not re-cap it to 1), and a pill at 6 now fires none at all rather than
-- one-then-stop.
--
-- TWO MODES, so the proportional rule can be benched against the rule it
-- replaced (C.ATTACK_TANK_HEAT_CAP_MODE):
--   "proportional" (default) -- the linear allowance above.
--   "floor"                  -- no proportional cap at all. The volley runs to
--                               shots_needed; the only limit is that a shot
--                               must not take the pill below MIN_HP, so the
--                               allowance is simply hp - MIN_HP (hp 6 buys 1
--                               shot, hp 5 buys none) and the volley ends
--                               `hp_floor` rather than `hp_cap`.
-- Both are clamped to HEAT_MAX_HITS so `allow{n of N}` always reads "n of the
-- N halvings this pill's health can pay for".
local function heat_allowed_shots(hp)
  local min_hp = C.ATTACK_TANK_HEAT_MIN_HP or 5
  local n
  if C.ATTACK_TANK_HEAT_CAP_MODE == "floor" then
    n = (hp or 0) - min_hp
  else
    local span = (C.PILLS_MAX_HEALTH or 15) - min_hp
    if span <= 0 then return 0 end
    n = math.floor(HEAT_MAX_HITS * ((hp or 0) - min_hp) / span + 0.5)
  end
  if n < 0 then n = 0 elseif n > HEAT_MAX_HITS then n = HEAT_MAX_HITS end
  return n
end

-- Euclidean tile distance between two map tiles (centre to centre).
local function heat_tdist(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  return math.sqrt(dx * dx + dy * dy)
end

-- Pick the friendly pill worth heating against enemy tank `target`, or nil.
-- Deterministic: candidate ids are collected then table.sort-ed, so the scan
-- order never depends on pairs(); the winner is the lowest id among the
-- closest-to-the-enemy candidates (ties broken by id).
-- Emits one HEAT_PILL line per decision, with every factor that produced it,
-- unless `quiet` (the heat_pill_available ask from goal selection).
local function heat_pill_select(state, world, info, goal, target, now, tmx, tmy, los_fn, quiet)
  local pills = world.pills
  if not pills then return nil end

  local ids = {}
  for id in pairs(pills) do ids[#ids + 1] = id end
  table.sort(ids)

  local d_us_enemy   = heat_tdist(tmx, tmy, target.mx, target.my)
  local fire_range_wu = ((info.gunrange or 14) / 2.0) * 256
  local max_frac     = C.ATTACK_TANK_HEAT_MAX_FRAC or 0.75
  local min_hp       = C.ATTACK_TANK_HEAT_MIN_HP or 5
  local floor_mode   = (C.ATTACK_TANK_HEAT_CAP_MODE == "floor")
  local retry_ticks  = C.ATTACK_TANK_HEAT_RETRY_TICKS or 150
  local retry        = state._heat_pill_retry

  local best, best_d = nil, math.huge
  for _, id in ipairs(ids) do
    local p = pills[id]
    if p and p.owner == "friendly" and (p.health or 0) > 0 and not p.in_tank then
      local hp      = p.health or 0
      local anger   = p.anger or 0
      local d_pe    = heat_tdist(p.mx, p.my, target.mx, target.my)
      local pill_wx, pill_wy = U.m2w(p.mx), U.m2w(p.my)
      local d_wu    = U.wdist(info.tankx, info.tanky, pill_wx, pill_wy)
      local in_rng  = d_wu <= fire_range_wu
      -- need    = shells to reach PILLBOX_MAX_FIRERATE from where the anger
      --           proxy says this pill already is
      -- allowed = shells its remaining armour can afford
      -- shots   = what we will actually fire
      local need    = M.heat_pill_shots_needed(anger)
      local allowed = heat_allowed_shots(hp)
      local shots   = (need < allowed) and need or allowed
      local reason  = nil
      local los     = nil   -- nil = never tested (an earlier gate rejected first)

      if d_pe >= d_us_enemy then
        reason = "not_closer"
      elseif not in_rng then
        reason = "out_of_range"
      elseif floor_mode and hp < min_hp then
        -- floor mode rejects only a pill already under the floor; a pill AT the
        -- floor is admitted and simply affords no shots (no_shots_needed).
        reason = "hp"
      elseif (not floor_mode) and allowed <= 0 then
        reason = "hp"
      elseif anger >= max_frac then
        reason = "already_hot"
      elseif shots <= 0 then
        reason = "no_shots_needed"
      elseif retry and retry[id] and (now - retry[id]) < retry_ticks then
        reason = "retry_wait"
      elseif (info.shells or 0) < shots + (C.SHELL_RESERVE or 0) then
        reason = "low_shells"
      else
        los = los_fn(info, world, pill_wx, pill_wy, p.mx, p.my)
        if not los then reason = "no_los" end
      end


      if not reason and d_pe < best_d then
        best_d = d_pe
        best   = { id = id, pill = p, shots = shots, d_pe = d_pe,
                   d_ue = d_us_enemy, hp = hp, anger = anger, d_wu = d_wu,
                   need = need, allowed = allowed }
      end
    end
  end
  return best
end

-- Could this bot heat a friendly pill at enemy tank `target` right now, or is
-- a volley already running? The same tests heat_pill_steer makes before it
-- starts a volley (no ghost, not afloat, heat_pill_select), asked quietly.
-- Read by goals.lua eval_attack_tank for C.ATTACK_TANK_PILL_HEAT_ONLY.
function M.heat_pill_available(state, world, info, target, now, tmx, tmy, los_fn)
  if not C.ATTACK_TANK_HEAT_PILL or not target or not los_fn then return false end
  if state._heat_active then return true end
  if target.ghost or info.inboat then return false end
  return heat_pill_select(state, world, info, nil, target, now,
                          tmx, tmy, los_fn, true) ~= nil
end

-- REAP AN ABANDONED VOLLEY.
-- The volley's bookkeeping lives on the GOAL table, but nothing tells us when
-- a goal is thrown away: a replan, or the enemy tank dying, simply hands the
-- next tick a different goal. The volley then evaporated with no HEAT_EXIT
-- line and, worse, no retry stamp -- so the next attack_tank could pick the
-- same pill and start over, and a measured early run did exactly that
-- (2 ENTERs, 213 HEAT_SHOT ticks, zero HEAT_EXITs).
--
-- So the ACTIVE volley is mirrored on `state`, which outlives any goal, and
-- this runs every tick from steering.M.steer whatever the goal is. If the
-- mirror no longer matches a live attack_tank volley on the current goal, the
-- volley is closed out properly: a HEAT_EXIT with the reason, and the retry
-- latch stamped exactly as the ordinary exits stamp it.
-- No new tunable: it reuses ATTACK_TANK_HEAT_RETRY_TICKS.
function M.heat_pill_reap(state, world, info, goal, now)
  local act = state._heat_active
  if not act then return end
  -- Still the same volley, on the same goal table, still an attack_tank? Then
  -- heat_pill_steer owns it and will emit its own exit.
  if goal and act.goal == goal and goal.kind == "attack_tank"
     and goal._heat_pid == act.pid then
    return
  end

  local pill  = world.pills and world.pills[act.pid] or nil
  local dead  = (not pill) or (pill.health or 0) <= 0
  local hits  = pill and ((act.hp0 or 0) - (pill.health or 0)) or 0
  local fired = (act.shells0 or 0) - (info.shells or 0)
  state._heat_pill_retry = state._heat_pill_retry or {}
  state._heat_pill_retry[act.pid] = now
  state._heat_active = nil
  -- If the goal table itself survived (reused, or just no longer attack_tank),
  -- scrub its volley fields too so nothing downstream reads a stale volley.
  if goal and act.goal == goal then
    goal._heat_pid, goal._heat_shots, goal._heat_need = nil, nil, nil
    goal._heat_hp0, goal._heat_shells0, goal._heat_start = nil, nil, nil
    goal._heat_allow, goal._heat_hold = nil, nil
    goal._heat_flight, goal._heat_fire_tick = nil, nil
    if goal.substate == "heat_pill" then goal.substate = nil end
  end
end

-- Heat-pill executor.  Returns keys, taps while a volley is running (or
-- starting) so attack_tank holds fire on E for those few ticks; returns nil
-- when the fight should proceed normally.
--
-- CALLED FROM steering.lua's tank_combat_steer, AFTER both of its disengage
-- returns -- the pillbox-crossfire break-off (threat.pill_at(tmx,tmy) >=
-- C.TANK_COMBAT_DEFENDED_DANGER) and the armour/shells flee check.  Those
-- already `return` before this point, so a volley can never start or continue
-- on a tick the existing code has decided we must break off: that is the
-- "under fire, must evade" gate this reuses rather than inventing a new one.
--
-- los_fn is steering.lua's shot_path_clear -- the SAME reach-and-blocker test
-- the fight loop uses to decide whether a shell would land on its target.
function M.heat_pill_steer(state, world, info, goal, target, now, tmx, tmy, los_fn)
  if not C.ATTACK_TANK_HEAT_PILL then return nil end
  if not target or not los_fn then return nil end

  local min_hp = C.ATTACK_TANK_HEAT_MIN_HP or 5
  local pid    = goal._heat_pid
  local pill   = pid and world.pills and world.pills[pid] or nil

  -- ── Finish / abandon an in-flight volley ──────────────────────────────
  if pid then
    local hits   = pill and ((goal._heat_hp0 or 0) - (pill.health or 0)) or 0
    local fired  = (goal._heat_shells0 or 0) - (info.shells or 0)
    local shots  = goal._heat_shots or 0
    local done   = nil

    -- SHELLS IN FLIGHT. Progress is counted in HITS -- the pill's observed
    -- armour drop -- but a shell takes 8 brain ticks per tile to get there
    -- (C.TANK_COMBAT_SHELL_SPEED, 32 WU per brain tick, against a 256 WU tile)
    -- while the tank reloads in about half that. Firing blind until `hits`
    -- catches up therefore overshoots by roughly one shell per 8 ticks of
    -- flight: a volley with PERFECT aim measured `hits{3/4} fired{6}` from
    -- ~7 tiles and aborted with a false `misses`, having missed nothing.
    -- So the trigger is allowed only `allow` shells (starting at shots_needed),
    -- and `allow` is raised by one -- up to the MAX_MISSES budget -- only after
    -- the shells already fired have had time to LAND and did not all register.
    -- That way a real miss is still retried and a slow shell never counts as
    -- one. `flight` is deliberately generous; MAX_TICKS bounds the whole thing.
    local allow  = goal._heat_allow or shots
    local flight = goal._heat_flight or 8
    if fired >= allow and hits < shots
       and (now - (goal._heat_fire_tick or now)) >= flight then
      if (allow - shots) < (C.ATTACK_TANK_HEAT_MAX_MISSES or 2) then
        allow = allow + 1
        goal._heat_allow = allow
        goal._heat_fire_tick = now   -- restart the landing clock for the retry
      end
    end
    goal._heat_allow = allow
    -- The trigger is held once we have `allow` shells fired or in the air.
    goal._heat_hold = (fired >= allow)
    if not pill or (pill.health or 0) <= 0 or pill.owner ~= "friendly"
       or pill.in_tank then
      done = "pill_gone"
    elseif hits >= shots then
      -- Which of the two ceilings actually stopped us? `maxed` means the pill
      -- is at PILLBOX_MAX_FIRERATE; otherwise its armour ran the volley short
      -- of that, named for the mode that did it (heat_allowed_shots).
      if shots < (goal._heat_need or shots) then
        done = (C.ATTACK_TANK_HEAT_CAP_MODE == "floor") and "hp_floor" or "hp_cap"
      else
        done = "maxed"
      end
    elseif C.ATTACK_TANK_HEAT_CAP_MODE == "floor"
           and (pill.health or 0) <= min_hp then
      -- floor mode, belt and braces: the entry allowance already counted the
      -- shots the floor permits, but the pill can lose armour to somebody ELSE
      -- mid-volley. Stop before the shot that would take it under the floor.
      done = "hp_floor"
    elseif fired >= allow + (C.ATTACK_TANK_HEAT_MAX_MISSES or 2) then
      done = "misses"
    elseif (info.shells or 0) <= (C.SHELL_RESERVE or 0) then
      done = "out_of_shells"
    elseif (now - (goal._heat_start or now)) >= (C.ATTACK_TANK_HEAT_MAX_TICKS or 150) then
      -- Hard ceiling on the whole sequence. Rotating onto the pill is the slow
      -- part -- a measured volley spent 39 of its 42 ticks getting aim_corr
      -- from +10 to +1 -- and if the tank is drifting, or the bearing keeps
      -- moving, `corr` can hover just outside the fire gate indefinitely. Left
      -- unbounded that is a tank standing next to its own pill not fighting.
      -- The volley is abandoned, the retry latch stamps, and the fight resumes.
      done = "timeout"
    end

    if not done then
      local pill_wx, pill_wy = U.m2w(pill.mx), U.m2w(pill.my)
      local d_wu = U.wdist(info.tankx, info.tanky, pill_wx, pill_wy)
      if d_wu > ((info.gunrange or 14) / 2.0) * 256 then
        done = "out_of_range"
      elseif not los_fn(info, world, pill_wx, pill_wy, pill.mx, pill.my) then
        done = "lost_los"
      end
    end

    if done then
      state._heat_pill_retry = state._heat_pill_retry or {}
      state._heat_pill_retry[pid] = now
      state._heat_active = nil          -- closed out here; nothing to reap
      goal._heat_pid, goal._heat_shots = nil, nil
      goal._heat_hp0, goal._heat_shells0, goal._heat_start = nil, nil, nil
      goal._heat_allow, goal._heat_hold = nil, nil
      goal._heat_flight, goal._heat_fire_tick = nil, nil
      goal._heat_need = nil
      if goal.substate == "heat_pill" then goal.substate = nil end
      return nil
    end

    return M.heat_pill_fire(state, world, info, goal, pill, pid, hits,
                            goal._heat_shots or 0, now)
  end

  -- ── Consider starting a volley ────────────────────────────────────────
  -- Heating pays only against an enemy that is actually THERE: a ghost is an
  -- extrapolation, and the pill would burn our armour and shells at nothing.
  -- (Same "live enemy" requirement the defend_pill heat gate applies through
  -- C.HEAT_REQUIRE_ENEMY_RANGE.)
  if target.ghost then return nil end
  -- Never start one afloat. heat_pill_fire brakes the tank and holds it still
  -- to aim, and a boated tank that stops is a boated tank that stays boated:
  -- a measured run opened a volley at brain tick 12 while still sitting on its
  -- spawn pond and spent 117 ticks firing from the water instead of coming
  -- ashore. Getting onto land is always the better use of those ticks.
  if info.inboat then return nil end

  local cand = heat_pill_select(state, world, info, goal, target, now,
                                tmx, tmy, los_fn)
  if not cand then return nil end

  goal._heat_pid     = cand.id
  goal._heat_shots   = cand.shots
  goal._heat_hp0     = cand.hp
  goal._heat_shells0 = info.shells or 0
  goal._heat_start   = now
  goal._heat_allow   = cand.shots
  goal._heat_need    = cand.need      -- shells to MAX heat, before the hp cap
  goal._heat_hold    = false
  goal._heat_fire_tick = now
  -- Brain ticks for one of our shells to reach the pill, plus a margin: a
  -- shell covers C.TANK_COMBAT_SHELL_SPEED (32) WU per brain tick and a tile
  -- is 256 WU, so 8 brain ticks per tile. This is the window the miss counter
  -- waits out before deciding a shell is never going to land.
  goal._heat_flight  = math.ceil((cand.d_wu or 0) / 256.0 * 8) + 4
  -- Stamp the retry latch on ENTER as well as on exit. The volley's state
  -- lives on the GOAL, so a replan that swaps the goal out mid-volley drops it
  -- silently -- no exit, no stamp -- and the next tick would select the same
  -- pill and start over, aiming forever without ever landing the hit that
  -- would raise anger past the already_hot gate. Stamping here bounds that:
  -- one attempt per pill per ATTACK_TANK_HEAT_RETRY_TICKS however the attempt
  -- ends. A normal exit re-stamps with the later tick.
  state._heat_pill_retry = state._heat_pill_retry or {}
  state._heat_pill_retry[cand.id] = now
  -- Mirror the volley on `state` so M.heat_pill_reap can close it out if this
  -- goal is replaced before it finishes (state outlives any goal table).
  state._heat_active = { pid = cand.id, goal = goal, shots = cand.shots,
                         need = cand.need, hp0 = cand.hp,
                         shells0 = info.shells or 0 }
  return M.heat_pill_fire(state, world, info, goal, cand.pill, cand.id, 0,
                          cand.shots, now)
end

-- Aim at the pill and hold the trigger.  The engine's reload paces the
-- shots; we stop when the observed armour drop says enough of them landed.
function M.heat_pill_fire(state, world, info, goal, pill, pid, hits, shots, now)
  local keys, taps = 0, 0
  goal.substate = "heat_pill"

  if info.gunrange < C.GUNSIGHT_MAX then keys = bit.bor(keys, KEY_MORERANGE) end
  if info.speed > 0 then keys = bit.bor(keys, KEY_SLOWER) end

  local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                             pill.mx + 0.5, pill.my + 0.5)
  local corr = U.adiff(info.direction, aim_dir)
  local h, t = U.aim_turn_bits(corr, 6, 1)
  keys = bit.bor(keys, h); taps = bit.bor(taps, t)

  -- Hold the trigger once `allow` shells are fired or still in the air, so the
  -- volley cannot overshoot while it waits for the hits to register.
  local shooting = (math.abs(corr) <= 1) and not goal._heat_hold
  if shooting and not U.fire_hold_block(state, C.FIRE_HOLD_TICKS) then
    keys = bit.bor(keys, KEY_SHOOT)
    goal._heat_fire_tick = now
    -- Refresh while firing (and through the shell's flight) so world.lua
    -- skips the under_attack stamp for HEAT_SELF_STAMP_TICKS: deliberately
    -- tickling our own pill must not read as an enemy siege to us or an ally.
    -- Exactly what defend_pill_steer does (steering.lua:1320).
    pill._heat_shot_tick = now
  end


  if viz.is_on("attack_heat_pill_viz") then
  end

  return keys, taps
end

-- ── Peace list ("peace=1/3/4" init token, C.PEACE_*) ─────────────────────
-- A script can name seats this bot does not pick a fight with (Pillbox Tag:
-- every hunter lists the other hunters, so they all go for the holder). A
-- listed seat is skipped as a tank-combat target, a kill_lgm target and an
-- opportunistic shot, until it hurts this bot. Then it is a normal enemy for
-- C.PEACE_HOSTILE_TICKS. A cooldown of C.PEACE_COOLDOWN_TICKS follows, and
-- in it the seat is spared again even if it keeps shooting. The next hit
-- after the cooldown opens a new hostile window. Hits inside a window do not
-- extend it.
--
-- WHO HIT US: the engine gives no attacker id. A shell object does carry its
-- firer (ob.owner, 0xFF for a pill), so peace_update keeps the owner of the
-- NEAREST hostile shell within C.IMD_SHELLS_RADIUS_WU of the tank that is
-- flying towards it (a shell heading away cannot hit it). When armour
-- drops, the attacker is the owner kept at the last think (the shell that hit
-- is gone by the time we look), or this think's if the last think saw none.
-- Only that one owner is blamed, and only if it is on the list. That is a
-- guess, not the real attacker: the nearest inbound shell is not always the
-- one that hit. When the holder's shell (or a pill's, owner 0xFF) is the
-- nearest, a listed hunter's shell flying past does not count. When a listed
-- hunter's shell was the nearest at the last think, that hunter is blamed,
-- whoever really hit us.
--
-- With no list, or C.PEACE_ENABLED off, both helpers do nothing and touch no
-- state, so a bot without the token behaves exactly as before.

-- Is seat `pn` spared right now? `pn` is a player number (a tank's id, a
-- man's idnum).
function M.peace_spared(state, pn)
  local set = state.peace
  if not set or pn == nil or not set[pn] or not C.PEACE_ENABLED then return false end
  local hu = state._peace_hostile_until
  if hu and hu[pn] and (state.tick or 0) < hu[pn] then return false end
  return true
end

-- Once per think, after state.took_damage_this_tick is set.
function M.peace_update(state, info, now)
  local set = state.peace
  if not set or next(set) == nil or not C.PEACE_ENABLED then return end
  local nearest, best = nil, C.IMD_SHELLS_RADIUS_WU * C.IMD_SHELLS_RADIUS_WU
  for _, ob in ipairs(info.objects or {}) do
    if ob.type == OBJECT_SHOT and bit.band(ob.info or 0, OBJECT_HOSTILE) ~= 0 then
      local dx, dy = info.tankx - ob.x, info.tanky - ob.y
      local d2 = dx * dx + dy * dy
      local d = ob.direction or 0
      if d2 <= best and U.bsin(d) * dx - U.bcos(d) * dy > 0 then
        nearest, best = ob.owner, d2
      end
    end
  end
  if state.took_damage_this_tick then
    local pn = state._peace_nearest
    if pn == nil then pn = nearest end
    local cu = state._peace_cooldown_until or {}
    if pn ~= nil and set[pn] and now >= (cu[pn] or 0) then
      local hu = state._peace_hostile_until or {}
      state._peace_hostile_until, state._peace_cooldown_until = hu, cu
      hu[pn] = now + C.PEACE_HOSTILE_TICKS
      cu[pn] = hu[pn] + C.PEACE_COOLDOWN_TICKS
      log.event("peace_hostile", string.format("p%d hit us; enemy until %d, spared again until %d",
                                               pn, hu[pn], cu[pn]))
    end
  end
  state._peace_nearest = nearest
end

return M
--[[ REMOVED: position/aim/engage/curve_away/rush/disengage substates
  if goal.substate == "position" then
    if not goal.standoff_mx then
      goal.substate = "plan_position"
      return
    end
    local sdist = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
    if sdist <= 1 and info.speed <= 4 and not info.inboat then
      goal.substate = "aim"
      print(string.format(TAG .. " ATTACK: arrived at standoff (%d,%d)",
            goal.standoff_mx, goal.standoff_my))
      log.event("attack", "aim")
    end
    return
  end

  -- ══════════════════════════════════════════════════════════════════
  -- aim: stopped, turn to face pill, no shooting
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "aim" then
    local aim_dir = U.aim_at(info.tankx, info.tanky, goal.wx, goal.wy)
    local corr = math.abs(U.adiff(info.direction, aim_dir))
    local clear = PF.wall_hp_between(tmx, tmy, pmx, pmy) == 0
    if corr < 4 and clear then
      goal.substate = "engage"
      goal.engage_tick = now
      goal.engage_armour = info.armour
      goal.hits_taken = 0
      print(string.format(TAG .. " ATTACK: aimed, engaging pill@(%d,%d)", pmx, pmy))
      log.event("attack", "engage")
    end
    return
  end

  -- ══════════════════════════════════════════════════════════════════
  -- engage: fire at pill, monitor armour
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "engage" then
    local hits = (goal.engage_armour or info.armour) - info.armour
    goal.hits_taken = hits

    if goal.engage_tick then
      local ticks_shooting = now - goal.engage_tick
      if ticks_shooting >= C.DRAIN_PROJECTION_MIN_TICKS and hits > 0 then
        local drain_rate = hits / ticks_shooting
        local pill_hp = pill and pill.health or 0
        local projected = info.armour - drain_rate * (pill_hp * C.TTK_TICKS_PER_HIT)
        if projected < C.ARMOUR_CRITICAL + C.DRAIN_ARMOUR_MARGIN then
          goal.substate = "disengage"
          print(string.format(TAG .. " ATTACK: drain disengage arm=%d proj=%.1f", info.armour, projected))
          return
        end
      end
    end

    if hits >= C.ATTACK_CURVE_AFTER_HITS then
      goal.substate = "curve_away"
      goal.curve_tick = now
      goal.curve_dir = (math.random() > 0.5) and 1 or -1
      print(string.format(TAG .. " ATTACK: curve_away after %d hits", hits))
      log.event("attack", "curve_away")
    end
    return
  end

  -- ══════════════════════════════════════════════════════════════════
  -- curve_away: evasive turn, then replan position
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "curve_away" then
    if now - (goal.curve_tick or now) >= C.ATTACK_CURVE_TICKS then
      goal.substate = "plan_position"
      goal.standoff_mx = nil
      goal.standoff_my = nil
      print(TAG .. " ATTACK: curve done, replanning position")
      log.event("attack", "replan")
    end
    return
  end

  -- rush: pill dead, steering handles navigation
  if goal.substate == "rush" then return end

  -- disengage: clear goal
  if goal.substate == "disengage" then
    goal.kind = "none"
    state.pf.status = "idle"
    print(TAG .. " ATTACK: disengaged")
    return
  end

end

return M
--[[ REMOVED: old wall-shield, pill-place, and legacy attack substates
      goal.standoff_mx = smx; goal.standoff_my = smy
      goal.substate = smx and "approach" or "plan"
    end
    return
  end

  if goal.substate == "approach" then
    if not goal.standoff_mx then return end

    if goal.wall_shield and goal.wall_mx then
      -- Wall-shield approach: navigate to PREBUILD position (outside pill range)
      local pb_mx = goal.prebuild_mx or goal.standoff_mx
      local pb_my = goal.prebuild_my or goal.standoff_my
      local pbdist = U.mdist(tmx, tmy, pb_mx, pb_my)
      if pbdist <= C.ATTACK_ENGAGE_RADIUS and not info.inboat then
        goal.substate      = "ws_prebuild"
        goal.ws_build_tick = now
        goal.lgm_return_tick = nil
        print(string.format(TAG .. " WALL-SHIELD: arrived at prebuild (%d,%d), dispatching LGM to build wall@(%d,%d)",
              pb_mx, pb_my, goal.wall_mx, goal.wall_my))
        log.reason("attack_sub", {
          transition = "approach->ws_prebuild", pbdist = pbdist,
          prebuild_mx = pb_mx, prebuild_my = pb_my,
          wall_mx = goal.wall_mx, wall_my = goal.wall_my,
        })
      end
    else
      -- Normal approach: navigate to standoff, must stop before engaging
      local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
      local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
      local in_range  = pdist_w <= C.ATTACK_PILL_RANGE * 256
      local clear_los = PF.wall_hp_between(tmx, tmy, goal.mx, goal.my) == 0
      if sdist <= C.ATTACK_ENGAGE_RADIUS and in_range and clear_los
         and info.speed <= 4 and not info.inboat then
        goal.substate    = "engage"
        goal.engage_tick = now
        print(string.format(TAG .. " ATTACK: engage pill@(%d,%d) from (%d,%d)",
              goal.mx, goal.my, tmx, tmy))
        log.reason("attack_sub", {
          transition = "approach->engage", sdist = sdist,
          pill_x = goal.mx, pill_y = goal.my,
        })
      end
    end
    return
  end

  -- ── Wall-shield substates ───────────────────────────────────────────
  -- ws_prebuild: tank stopped OUTSIDE pill range, LGM dispatched to build wall
  -- ws_prewait:  wall built, LGM returning; tank still outside pill range
  -- ws_advance:  LGM safe in tank, wall up; tank advances to engagement standoff
  -- ws_engage:   at standoff, shooting through/past wall
  -- ws_retreat:  wall destroyed, tank retreating perpendicular to draw fire away
  -- ws_rebuild:  tank repositioned, LGM dispatched to rebuild wall

  if goal.substate == "ws_prebuild" then
    -- Wait for the wall to appear at the target tile
    local wtt = U.ttype(goal.wall_mx, goal.wall_my)
    if wtt == C.T_BUILDING or wtt == C.T_HALFBUILD then
      -- Wall is up — wait for LGM to return
      goal.substate = "ws_prewait"
      goal.ws_wait_tick = now
      goal.lgm_return_tick = nil
      print(string.format(TAG .. " WALL-SHIELD: wall built@(%d,%d), waiting for LGM return (outside range)",
            goal.wall_mx, goal.wall_my))
      log.reason("attack_sub", {
        transition = "ws_prebuild->ws_prewait",
        wall_mx = goal.wall_mx, wall_my = goal.wall_my,
      })
    elseif now - (goal.ws_build_tick or now) > 400 then
      -- Timeout: LGM failed to build, fall back to normal engage
      goal.substate = "engage"
      goal.engage_tick = now
      goal.wall_shield = false
      print(TAG .. " WALL-SHIELD: prebuild timeout, falling back to normal attack")
    end
    return
  end

  if goal.substate == "ws_prewait" then
    -- Wait for LGM to return to tank (tank still outside pill range)
    if info.man_status == C.LGM_INTANK then
      goal.lgm_return_tick = goal.lgm_return_tick or now
      local safe_ticks = now - goal.lgm_return_tick
      if safe_ticks >= C.WALL_SHIELD_LGM_SAFE_TICKS then
        goal.substate = "ws_advance"
        print(string.format(TAG .. " WALL-SHIELD: LGM safe, advancing to standoff (%d,%d) with wall@(%d,%d)",
              goal.standoff_mx, goal.standoff_my, goal.wall_mx, goal.wall_my))
        log.reason("attack_sub", {
          transition = "ws_prewait->ws_advance",
          standoff_mx = goal.standoff_mx, standoff_my = goal.standoff_my,
        })
      end
    else
      goal.lgm_return_tick = nil  -- LGM still out
    end
    return
  end

  if goal.substate == "ws_advance" then
    -- LGM safe in tank, wall already built — advance to engagement standoff
    local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
    local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    local in_range = pdist_w <= C.ATTACK_PILL_RANGE * 256
    if sdist <= C.ATTACK_ENGAGE_RADIUS and in_range then
      goal.substate    = "ws_engage"
      goal.engage_tick = now
      goal.first_hit_tick = nil
      goal.last_armour = info.armour
      print(string.format(TAG .. " WALL-SHIELD: engaging pill@(%d,%d) with wall@(%d,%d)",
            goal.mx, goal.my, goal.wall_mx, goal.wall_my))
      log.reason("attack_sub", {
        transition = "ws_advance->ws_engage",
        wall_mx = goal.wall_mx, wall_my = goal.wall_my,
      })
    end
    -- If wall got destroyed during advance, retreat back
    local wtt = U.ttype(goal.wall_mx, goal.wall_my)
    if wtt ~= C.T_BUILDING and wtt ~= C.T_HALFBUILD then
      if (info.trees or 0) >= C.WALL_SHIELD_BUILD_COST then
        goal.substate = "ws_retreat"
        goal.ws_retreat_tick = now
        print(TAG .. " WALL-SHIELD: wall destroyed during advance, retreating to rebuild")
      else
        goal.substate = "engage"
        goal.engage_tick = now
        goal.wall_shield = false
        print(TAG .. " WALL-SHIELD: wall destroyed during advance, no trees, normal attack")
      end
    end
    return
  end

  if goal.substate == "ws_engage" then
    -- Shooting at pill with wall absorbing return fire.
    -- Check if wall is still intact
    local wtt = U.ttype(goal.wall_mx, goal.wall_my)
    local wall_intact = (wtt == C.T_BUILDING or wtt == C.T_HALFBUILD)

    if not wall_intact then
      -- Wall destroyed — retreat first, then rebuild
      if info.man_status == C.LGM_INTANK
         and (info.trees or 0) >= C.WALL_SHIELD_BUILD_COST then
        goal.substate = "ws_retreat"
        goal.ws_retreat_tick = now
        print(string.format(TAG .. " WALL-SHIELD: wall@(%d,%d) destroyed, retreating to rebuild",
              goal.wall_mx, goal.wall_my))
        log.reason("attack_sub", {
          transition = "ws_engage->ws_retreat",
          wall_mx = goal.wall_mx, wall_my = goal.wall_my,
          trees = info.trees,
        })
      else
        if (info.trees or 0) < C.WALL_SHIELD_BUILD_COST then
          goal.substate = "engage"
          goal.engage_tick = now
          goal.wall_shield = false
          print(TAG .. " WALL-SHIELD: out of trees, switching to normal attack")
        end
      end
      return
    end

    -- Disengage check — more lenient with wall-shield (wall absorbs shots)
    if info.armour <= C.ARMOUR_CRITICAL then
      goal.substate = "disengage"
      print(string.format(TAG .. " WALL-SHIELD: armour critical (%d), disengaging",
            info.armour))
      log.reason("attack_sub", {
        transition = "ws_engage->disengage",
        armour = info.armour,
      })
    end
    return
  end

  if goal.substate == "ws_retreat" then
    -- Tank retreating perpendicular to the pill->wall line to draw fire away.
    -- Once outside pill range, transition to ws_rebuild.
    local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    local outside_range = pdist_w > C.PILL_FIRE_RANGE * 256
    -- Also accept if we've been retreating long enough
    local retreat_time = now - (goal.ws_retreat_tick or now)
    if outside_range or retreat_time > 150 then
      goal.substate = "ws_rebuild"
      goal.ws_rebuild_tick = now
      goal.lgm_return_tick = nil
      print(string.format(TAG .. " WALL-SHIELD: retreated, dispatching LGM to rebuild wall@(%d,%d)",
            goal.wall_mx, goal.wall_my))
      log.reason("attack_sub", {
        transition = "ws_retreat->ws_rebuild",
        wall_mx = goal.wall_mx, wall_my = goal.wall_my,
        retreat_ticks = retreat_time,
      })
    end
    return
  end

  if goal.substate == "ws_rebuild" then
    -- Tank is repositioned outside pill range. LGM rebuilds the wall.
    local wtt = U.ttype(goal.wall_mx, goal.wall_my)
    if wtt == C.T_BUILDING or wtt == C.T_HALFBUILD then
      -- Wall rebuilt! Wait for LGM return
      goal.substate = "ws_prewait"
      goal.ws_wait_tick = now
      goal.lgm_return_tick = nil
      print(string.format(TAG .. " WALL-SHIELD: wall rebuilt@(%d,%d), waiting for LGM",
            goal.wall_mx, goal.wall_my))
      log.reason("attack_sub", {
        transition = "ws_rebuild->ws_prewait",
        wall_mx = goal.wall_mx, wall_my = goal.wall_my,
      })
    elseif now - (goal.ws_rebuild_tick or now) > 400 then
      goal.substate = "engage"
      goal.engage_tick = now
      goal.wall_shield = false
      print(TAG .. " WALL-SHIELD: rebuild timeout, falling back to normal attack")
    end
    return
  end

  -- ── Normal (non-wall-shield) engage ─────────────────────────────────

  if goal.substate == "engage" then
    if not goal.standoff_mx then return end
    local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
    local tank_tt = U.ttype(tmx, tmy)
    local hazard  = (tank_tt == C.T_RIVER or tank_tt == C.T_DEEPSEA
                     or tank_tt == C.T_BUILDING or tank_tt == C.T_HALFBUILD)
    local min_time_met = (now - (goal.engage_tick or 0)) >= C.ATTACK_MIN_ENGAGE_TICKS

    -- Knocked too far or hazardous terrain → reposition
    if hazard or (sdist > C.ATTACK_REPOSITION_RADIUS and min_time_met) then
      goal.substate        = "reposition"
      goal.reposition_tick = now
      print(string.format(TAG .. " ATTACK: reposition pill@(%d,%d) sdist=%d hazard=%s",
            goal.mx, goal.my, sdist, tostring(hazard)))
      log.reason("attack_sub", {
        transition = "engage->reposition", sdist = sdist,
        hazard = hazard, tank_tt = tank_tt,
      })
      return
    end

    -- Time-under-fire based disengage.
    local pill = nil
    pill = M.find_pill_at(world, goal.mx, goal.my)
    local pill_anger = pill and (pill.anger or 0) or 0

    if not goal.first_hit_tick then
      if info.armour < (goal.last_armour or info.armour) then
        goal.first_hit_tick = now
        goal.engage_start_armour = goal.last_armour or info.armour
      end
    end
    goal.last_armour = info.armour

    if goal.first_hit_tick then
      local ticks_under_fire = now - goal.first_hit_tick
      local anger_scale = 1.0 - pill_anger * (1.0 - C.ENGAGE_ANGRY_FLEE_FACTOR)
      local esc_dx = tmx - goal.mx
      local esc_dy = tmy - goal.my
      local esc_len = math.max(1, math.sqrt(esc_dx * esc_dx + esc_dy * esc_dy))
      local escape_slow = false
      for step = 1, 3 do
        local ex = U.mclamp(math.floor(tmx + esc_dx / esc_len * step + 0.5))
        local ey = U.mclamp(math.floor(tmy + esc_dy / esc_len * step + 0.5))
        local ett = U.ttype(ex, ey)
        if ett == C.T_SWAMP or ett == C.T_RUBBLE or ett == C.T_CRATER
           or ett == C.T_RIVER or ett == C.T_DEEPSEA then
          escape_slow = true
          break
        end
      end
      local terrain_scale = escape_slow and 0.6 or 1.0
      local max_ticks = C.ENGAGE_MAX_INCOMING_TICKS * anger_scale * terrain_scale

      if ticks_under_fire >= max_ticks then
        goal.substate = "disengage"
        print(string.format(
          TAG .. " ATTACK: disengage pill@(%d,%d) fire=%d/%.0f anger=%.2f esc_slow=%s",
          goal.mx, goal.my, ticks_under_fire, max_ticks, pill_anger,
          tostring(escape_slow)))
        log.reason("attack_sub", {
          transition = "engage->disengage",
          ticks_under_fire = ticks_under_fire, max_ticks = max_ticks,
          pill_anger = pill_anger, escape_slow = escape_slow,
        })
        return
      end

      -- Feature 3: armour drain projection — disengage early if projected
      -- armour at kill time would be below the flee threshold
      if ticks_under_fire >= C.DRAIN_PROJECTION_MIN_TICKS then
        local armour_lost = (goal.engage_start_armour or info.armour) - info.armour
        if armour_lost > 0 then
          local drain_rate = armour_lost / ticks_under_fire
          local remaining_hp = pill and pill.health or 0
          local remaining_ttk = remaining_hp * C.TTK_TICKS_PER_HIT
          local projected_armour = info.armour - drain_rate * remaining_ttk
          local flee_with_margin = C.ARMOUR_CRITICAL + C.DRAIN_ARMOUR_MARGIN
          if projected_armour < flee_with_margin then
            goal.substate = "disengage"
            print(string.format(
              TAG .. " ATTACK: drain disengage pill@(%d,%d) arm=%d proj=%.1f drain=%.3f/tick rem_hp=%d",
              goal.mx, goal.my, info.armour, projected_armour, drain_rate, remaining_hp))
            log.reason("attack_sub", {
              transition = "engage->disengage(drain)",
              armour = info.armour, projected = projected_armour,
              drain_rate = drain_rate, remaining_hp = remaining_hp,
              remaining_ttk = remaining_ttk, threshold = flee_with_margin,
            })
            return
          end
        end
      end
    end
    return
  end

  if goal.substate == "disengage" then
    -- Disengage triggers the normal flee/refuel logic by clearing the
    -- attack goal.  The dynamic flee threshold in goal_selection will
    -- pick a base to retreat to.  Clear the capture objective so goal
    -- selection doesn't immediately re-attack.
    -- The capture objective is preserved — after refueling, the brain
    -- will re-engage the pill.  Only clear attack plan so it replans
    -- the standoff from the new position.
    state.pill_attack_plan = nil
    goal.kind = "none"
    goal.substate = nil
    print(TAG .. " ATTACK: disengaged — will refuel and re-engage")
    return
  end

  if goal.substate == "reposition" then
    -- Force replan of standoff position from current location
    local pill = nil
    pill = M.find_pill_at(world, goal.mx, goal.my)
    if pill then
      local pk = U.mkey(goal.mx, goal.my)
      state.pill_attack_plan = nil  -- force fresh computation
      local smx, smy = M.get_standoff(world, info, pk, pill, state)
      goal.standoff_mx = smx; goal.standoff_my = smy
      goal.substate = smx and "approach" or "plan"
      print(string.format(TAG .. " ATTACK: re-approach pill@(%d,%d) new standoff=(%s,%s)",
            goal.mx, goal.my, tostring(smx), tostring(smy)))
      log.reason("attack_sub", {
        transition = "reposition->approach",
        new_standoff_mx = smx, new_standoff_my = smy,
      })
    end
    return
  end
end

-- =========================================================================
-- Pill placement planner
-- =========================================================================

-- Pick the best position to place a friendly pill near a hostile target.
-- Mirrors pick_wall_shield: place 1 tile from the target on the approach
-- line, try ±30°/±60° rotations if that tile isn't valid.
-- Returns place_mx, place_my, deploy_mx, deploy_my, standoff_mx, standoff_my
-- or nil if no valid position found.
function M.pick_pill_placement(world, info, target_pill, state)
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local pmx, pmy = target_pill.mx, target_pill.my

  -- Direction from pill to tank (approach angle) — same as pick_wall_shield
  local dx = tmx - pmx
  local dy = tmy - pmy
  local len = math.sqrt(dx * dx + dy * dy)
  if len < 2 then return nil, nil, nil, nil, nil, nil end

  local ux, uy = dx / len, dy / len

  -- Try the primary angle and rotations of ±30°, ±60° — same as pick_wall_shield
  local angles = { 0, 0.52, -0.52, 1.05, -1.05 }
  for _, ang in ipairs(angles) do
    local cos_a = math.cos(ang)
    local sin_a = math.sin(ang)
    local rx = ux * cos_a - uy * sin_a
    local ry = ux * sin_a + uy * cos_a

    -- Placement tile: 1 tile from target in this direction (same as wall dist)
    local wmx = U.mclamp(math.floor(pmx + rx * C.WALL_SHIELD_WALL_DIST + 0.5))
    local wmy = U.mclamp(math.floor(pmy + ry * C.WALL_SHIELD_WALL_DIST + 0.5))

    -- Must be placeable land (no water, no existing pill/base)
    local wtt = U.ttype(wmx, wmy)
    local placeable = (wtt == C.T_GRASS or wtt == C.T_ROAD or wtt == C.T_RUBBLE
                       or wtt == C.T_SWAMP or wtt == C.T_CRATER or wtt == C.T_FOREST
                       or wtt == C.T_REFBASE)
    if not placeable then goto next_angle end
    if wmx == pmx and wmy == pmy then goto next_angle end
    if world.pill_at[wmy * 256 + wmx] then goto next_angle end
    if world.base_at[wmy * 256 + wmx] then goto next_angle end

    -- Standoff tile: offset angle so shells clear the placed pill — same as pick_wall_shield
    do
      local smx, smy = nil, nil
      local off_rad = math.rad(C.WALL_SHIELD_STANDOFF_ANGLE_OFFSET)
      local offsets = { off_rad, -off_rad, off_rad * 2, -off_rad * 2, 0 }
      for _, sdist in ipairs({ C.WALL_SHIELD_STANDOFF, C.WALL_SHIELD_STANDOFF - 1, C.WALL_SHIELD_STANDOFF + 1 }) do
        for _, off in ipairs(offsets) do
          local cos_o = math.cos(off)
          local sin_o = math.sin(off)
          local ox = rx * cos_o - ry * sin_o
          local oy = rx * sin_o + ry * cos_o
          local cx = U.mclamp(math.floor(pmx + ox * sdist + 0.5))
          local cy = U.mclamp(math.floor(pmy + oy * sdist + 0.5))
          local ctt = U.ttype(cx, cy)
          local cland = C.TERRAIN_COST_LAND[ctt] or 9999
          if cland < 9999 and not U.is_water(ctt) then
            -- LOS to target must NOT pass through placed pill tile
            local hits_pill = U.bresenham(cx, cy, pmx, pmy, function(lx, ly)
              if lx == wmx and ly == wmy then return true end
            end)
            if not hits_pill then
              local cd = math.sqrt((cx - pmx) * (cx - pmx) + (cy - pmy) * (cy - pmy))
              if cd <= C.ATTACK_PILL_RANGE then
                smx = cx; smy = cy
                break
              end
            end
          end
        end
        if smx then break end
      end
      if not smx then goto next_angle end

      -- Deploy position: outside pill range on the approach line — same as wall-shield prebuild
      local deploy_mx = U.mclamp(math.floor(pmx + rx * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5))
      local deploy_my = U.mclamp(math.floor(pmy + ry * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5))

      print(string.format(
        TAG .. " [PP] PLACEMENT PLAN: target@(%d,%d) place@(%d,%d) deploy@(%d,%d) standoff@(%d,%d)",
        pmx, pmy, wmx, wmy, deploy_mx, deploy_my, smx, smy))
      return wmx, wmy, deploy_mx, deploy_my, smx, smy
    end

    ::next_angle::
  end
  return nil, nil, nil, nil, nil, nil
end

-- Find the best friendly pill to pick up for placement.
-- Only considers dead friendly pills (health == 0) since alive pills can't be picked up.
-- Returns pill entry and pill_id, or nil if none available.
function M.pick_source_pill(world, info, state)
  if (info.carried_pills or 0) > 0 then
    return nil, nil  -- already carrying a pill
  end
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local best_cost = math.huge
  local best_pill, best_id = nil, nil
  local ammo = (info.shells or 0) + (info.mines or 0)
  for id, p in pairs(world.pills) do
    if p.owner == "friendly" and p.health == 0 then
      local c = cpf.estimate_cost(tmx, tmy, p.mx, p.my, info.inboat and 1 or 0)
      if c < best_cost then
        best_cost = c
        best_pill = p
        best_id = id
      end
    end
  end
  return best_pill, best_id
end

-- =========================================================================
-- Pill placement substate machine
-- =========================================================================
-- States: select_pill → pickup → navigate → dispatch → wait_place →
--         engage → collect_target → (chain back to select_pill)
-- Also:   engage → finish (placed pill died, continue shooting with tank)
--         engage/finish → disengage (armour critical)

function M.update_pill_place_substate(goal, state, world, info)
  if goal.kind ~= "pill_place" then return end

  if M.USE_PILLPLACE_BT then
    if not pillplace_bt then pillplace_bt = require("pillplace_bt") end
    pillplace_bt.tick(goal, state, world, info)
    return
  end

  -- ── Legacy FSM (kept as fallback) ─────────────────────────────────
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local now = state.tick

  if not goal.substate then
    goal.substate = "select_pill"
  end

  -- Look up target pill
  local target = M.find_pill_at(world, goal.mx, goal.my)

  -- Target dead in any substate → collect it
  if target and target.health == 0 and goal.substate ~= "collect_target"
     and goal.substate ~= "disengage" then
    goal.substate = "collect_target"
    goal.collect_tick = now
    print(string.format(TAG .. " [PP] target@(%d,%d) dead, collecting", goal.mx, goal.my))
    log.reason("pp_sub", { transition = goal.substate .. "->collect_target" })
    return
  end

  -- Target became friendly → done
  if target and target.owner == "friendly" then
    print(string.format(TAG .. " [PP] target@(%d,%d) is now friendly, done", goal.mx, goal.my))
    goal.kind = "none"
    goal.substate = nil
    return
  end

  -- ── select_pill ─────────────────────────────────────────────────────
  if goal.substate == "select_pill" then
    -- Already carrying a pill?
    if (info.carried_pills or 0) > 0 then
      -- Pick placement position for target
      if not target then
        goal.kind = "none"; goal.substate = nil
        print(TAG .. " [PP] select_pill: target not found, aborting")
        return
      end
      local pmx, pmy, dmx, dmy, smx, smy = M.pick_pill_placement(world, info, target, state)
      if not pmx then
        goal.kind = "none"; goal.substate = nil
        print(TAG .. " [PP] select_pill: no valid placement position, aborting")
        return
      end
      goal.place_mx    = pmx
      goal.place_my    = pmy
      goal.deploy_mx   = dmx
      goal.deploy_my   = dmy
      goal.standoff_mx = smx
      goal.standoff_my = smy
      goal.substate = "navigate"
      print(string.format(TAG .. " [PP] select_pill: have pill, navigate to deploy@(%d,%d) place@(%d,%d)", dmx, dmy, pmx, pmy))
      log.reason("pp_sub", { transition = "select_pill->navigate", place_mx = pmx, place_my = pmy, deploy_mx = dmx, deploy_my = dmy })
      return
    end

    -- Find a dead friendly pill to pick up
    local src_pill, src_id = M.pick_source_pill(world, info, state)
    if not src_pill then
      -- No pills available — can't do pill placement
      goal.kind = "none"; goal.substate = nil
      print(TAG .. " [PP] select_pill: no friendly pills to pick up, aborting")
      return
    end

    goal.source_mx = src_pill.mx
    goal.source_my = src_pill.my
    goal.source_id = src_id
    goal.substate = "pickup"
    print(string.format(TAG .. " [PP] select_pill: picking up dead pill#%s@(%d,%d)",
          tostring(src_id), src_pill.mx, src_pill.my))
    log.reason("pp_sub", {
      transition = "select_pill->pickup",
      source_id = src_id, source_mx = src_pill.mx, source_my = src_pill.my,
    })
    return
  end

  -- ── pickup ──────────────────────────────────────────────────────────
  if goal.substate == "pickup" then
    -- Drive to the dead friendly pill to pick it up
    if (info.carried_pills or 0) > 0 then
      -- Got it! Now select placement position
      goal.substate = "select_pill"  -- re-enter to compute placement
      print(TAG .. " [PP] pickup: pill picked up, selecting placement")
      return
    end
    -- Check the source pill is still there and dead
    local src = M.find_pill_at(world, goal.source_mx, goal.source_my)
    if not src or src.health > 0 or src.owner ~= "friendly" then
      -- Source pill gone or revived
      goal.substate = "select_pill"
      print(TAG .. " [PP] pickup: source pill no longer available, re-selecting")
      return
    end
    -- Navigation handled by steering; just monitor arrival
    return
  end

  -- ── navigate ────────────────────────────────────────────────────────
  if goal.substate == "navigate" then
    if not goal.place_mx then
      goal.substate = "select_pill"
      return
    end
    -- Check arrival: at deploy position (outside pill range)
    local nav_mx = goal.deploy_mx or goal.place_mx
    local nav_my = goal.deploy_my or goal.place_my
    local pdist = U.mdist(tmx, tmy, nav_mx, nav_my)
    if pdist <= C.ATTACK_ENGAGE_RADIUS and info.man_status == C.LGM_INTANK
       and (info.carried_pills or 0) > 0 and not info.inboat then
      goal.substate = "dispatch"
      goal.dispatch_tick = now
      print(string.format(TAG .. " [PP] navigate: arrived at deploy@(%d,%d), dispatching LGM to place@(%d,%d)",
            nav_mx, nav_my, goal.place_mx, goal.place_my))
      log.reason("pp_sub", {
        transition = "navigate->dispatch",
        deploy_mx = nav_mx, deploy_my = nav_my,
        place_mx = goal.place_mx, place_my = goal.place_my,
      })
    end
    return
  end

  -- ── dispatch ────────────────────────────────────────────────────────
  if goal.substate == "dispatch" then
    -- Builder handles the actual BUILDMODE_PBOX command.
    -- Transition to wait_place once LGM leaves the tank.
    if info.man_status ~= C.LGM_INTANK then
      goal.substate = "wait_place"
      goal.wait_tick = now
      print(TAG .. " [PP] dispatch: LGM dispatched, waiting for placement")
      log.reason("pp_sub", { transition = "dispatch->wait_place" })
    elseif now - (goal.dispatch_tick or now) > 50 then
      -- LGM didn't leave after 50 ticks — maybe can't build, retry
      goal.substate = "select_pill"
      print(TAG .. " [PP] dispatch: LGM didn't leave, re-selecting")
    end
    return
  end

  -- ── wait_place ──────────────────────────────────────────────────────
  if goal.substate == "wait_place" then
    -- Check if a friendly pill appeared at the placement position
    local placed = M.find_pill_at(world, goal.place_mx, goal.place_my)
    if placed and placed.owner == "friendly" and placed.health > 0 then
      goal.placed_mx = goal.place_mx
      goal.placed_my = goal.place_my
      goal.substate = "engage"
      goal.engage_tick = now
      goal.last_armour = info.armour
      print(string.format(TAG .. " [PP] wait_place: pill placed@(%d,%d) hp=%d, engaging target@(%d,%d)",
            goal.place_mx, goal.place_my, placed.health, goal.mx, goal.my))
      log.reason("pp_sub", {
        transition = "wait_place->engage",
        placed_mx = goal.place_mx, placed_my = goal.place_my,
        placed_hp = placed.health,
      })
      return
    end
    -- LGM returned without placing (died, blocked, etc.)
    if info.man_status == C.LGM_INTANK and now - (goal.wait_tick or now) > 50 then
      -- Check if we still have a pill
      if (info.carried_pills or 0) > 0 then
        goal.substate = "select_pill"  -- retry with different position
        print(TAG .. " [PP] wait_place: LGM returned, pill not placed, re-selecting")
      else
        -- LGM died and lost the pill
        goal.substate = "select_pill"
        print(TAG .. " [PP] wait_place: pill lost, re-selecting")
      end
      return
    end
    -- Timeout
    if now - (goal.wait_tick or now) > C.PILL_PLACE_TIMEOUT then
      goal.substate = "select_pill"
      print(TAG .. " [PP] wait_place: timeout, re-selecting")
      return
    end
    return
  end

  -- ── engage ──────────────────────────────────────────────────────────
  if goal.substate == "engage" then
    -- Armour critical → disengage
    if info.armour <= C.ARMOUR_CRITICAL then
      goal.substate = "disengage"
      print(string.format(TAG .. " [PP] engage: armour critical (%d), disengaging", info.armour))
      log.reason("pp_sub", { transition = "engage->disengage", armour = info.armour })
      return
    end

    -- Time-under-fire disengage: bail if taking sustained hits
    if not goal.engage_first_hit then
      if info.armour < (goal.last_armour or info.armour) then
        goal.engage_first_hit = now
      end
    end
    goal.last_armour = info.armour

    if goal.engage_first_hit then
      local ticks_under_fire = now - goal.engage_first_hit
      if ticks_under_fire >= C.ENGAGE_MAX_INCOMING_TICKS then
        goal.substate = "disengage"
        print(string.format(TAG .. " [PP] engage: under fire %d ticks, disengaging", ticks_under_fire))
        log.reason("pp_sub", {
          transition = "engage->disengage", ticks_under_fire = ticks_under_fire,
        })
        return
      end
    end

    -- Check placed pill health
    local placed = nil
    if goal.placed_mx then
      placed = M.find_pill_at(world, goal.placed_mx, goal.placed_my)
    end
    if placed and placed.owner == "friendly" and placed.health > 0 then
      -- Placed pill still fighting, continue engagement
    else
      -- Placed pill died or gone — continue shooting with tank alone
      goal.substate = "finish"
      goal.finish_tick = now
      print(string.format(TAG .. " [PP] engage: placed pill died, finishing target@(%d,%d) with tank fire",
            goal.mx, goal.my))
      log.reason("pp_sub", { transition = "engage->finish" })
      return
    end
    return
  end

  -- ── finish ──────────────────────────────────────────────────────────
  -- Placed pill died but target is still alive; continue with tank-only fire.
  -- Behaves like a normal attack_pill engage — disengage if armour gets low.
  if goal.substate == "finish" then
    if info.armour <= C.ARMOUR_CRITICAL then
      goal.substate = "disengage"
      print(string.format(TAG .. " [PP] finish: armour critical (%d), disengaging", info.armour))
      return
    end

    -- Time-under-fire based disengage (reuse attack constants)
    if not goal.first_hit_tick then
      if info.armour < (goal.last_armour or info.armour) then
        goal.first_hit_tick = now
      end
    end
    goal.last_armour = info.armour

    if goal.first_hit_tick then
      local ticks_under_fire = now - goal.first_hit_tick
      if ticks_under_fire >= C.ENGAGE_MAX_INCOMING_TICKS then
        goal.substate = "disengage"
        print(string.format(TAG .. " [PP] finish: under fire %d ticks, disengaging", ticks_under_fire))
        log.reason("pp_sub", {
          transition = "finish->disengage", ticks_under_fire = ticks_under_fire,
        })
        return
      end
    end
    return
  end

  -- ── collect_target ──────────────────────────────────────────────────
  if goal.substate == "collect_target" then
    -- Drive onto dead target pill
    local pdist = U.mdist(tmx, tmy, goal.mx, goal.my)
    if pdist <= 1 then
      -- Arrived — pill auto-picked up by tank
      -- Chain: immediately re-select next target
      print(string.format(TAG .. " [PP] collect: arrived at target@(%d,%d), chaining to next",
            goal.mx, goal.my))
      log.reason("pp_sub", { transition = "collect_target->done" })
      -- Clear goal to let goal selection pick the next target
      state.pill_attack_plan = nil
      goal.kind = "none"
      goal.substate = nil
    end
    return
  end

  -- ── disengage ───────────────────────────────────────────────────────
  if goal.substate == "disengage" then
    state.pill_attack_plan = nil
    goal.kind = "none"
    goal.substate = nil
    print(TAG .. " [PP] disengaged — will refuel and re-engage")
    return
  end
end
--]]
