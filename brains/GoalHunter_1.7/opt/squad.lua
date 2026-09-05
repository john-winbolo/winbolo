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
local print2     = require("print2")

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
-- non-suicider SOLDIERS to make up the difference — never itself, never one that
-- already is one. 0 (the default) means it never designates, which is exactly
-- today's behaviour. Per-bot, like the sizes above; the "blitzsuiciders=N" token
-- replaces it.
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

-- Every soldier COMMITTED to our blitz on `our_pid`, as an array of
-- { pn = n, suicider = bool } sorted by player number. Same membership test as
-- blitz_ready_status (role s, cmdr = us, past negotiation, broadcasting
-- attack_pill on our pill, alive); `suicider` is the ally's broadcast psu flag,
-- which is set for a forced, slate-picked OR already blitz-designated suicider.
-- SORTED because the designation picks from it with the seeded RNG and
-- ally_state's pairs() order is not reproducible.
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
         and h.goal == "attack_pill" and tonumber(h.target or "") == our_pid then
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
-- designates the same tanks every time.
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
  if #picked > 0 then
    local q = state._blitz_su_send
    if not q then q = {}; state._blitz_su_send = q end
    for _, pn in ipairs(picked) do q[#q + 1] = { pill = our_pid, pn = pn } end
  end
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
function M.availability(state, info, help_target_id)
  -- Joining a blitz has NO armour floor (a 2+ tank take shares the incoming
  -- fire) — EXCEPT while carrying a pillbox: cautious mode, so a joiner needs
  -- commander-level armour before diving in and risking the pill it's holding.
  local ok, reason
  if (info.carried_pills or 0) >= 1
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
  return ok, reason
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
local function blitz_spot_shot_blocked(world, sfx, sfy, pmx, pmy)
  local ox = math.floor(sfx * 256 + 0.5)
  local oy = math.floor(sfy * 256 + 0.5)
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
        local plist = world and world.pill_at and world.pill_at[t.my * 256 + t.mx]
        if plist then for _, e in ipairs(plist) do local pp = e.id and world.pills[e.id]; if pp and not pp.in_tank and (pp.health or 0) > 0 and pp.mx == t.mx and pp.my == t.my then blocked = true; break end end end
        if blocked then break end
      end
      if reached and not blocked then return false end   -- this aim is clear
    end
  end
  return true   -- every aim point blocked → no clean shot
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
                          fx = state.squad_blitz_engage_mx + 0.5,
                          fy = state.squad_blitz_engage_my + 0.5,
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
      local fx, fy
      if slot.info.bes then fx, fy = slot.info.bes:match("^(%-?[%d.]+),(%-?[%d.]+)$") end
      parts[#parts + 1] = { pn = pn, fx = fx and tonumber(fx), fy = fy and tonumber(fy),
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
         and blitz_spot_shot_blocked(state.world, p.fx, p.fy, gmx, gmy) then
        reject[p.pn] = true
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
  local cur_fx = state.squad_blitz_engage_mx and (state.squad_blitz_engage_mx + 0.5) or nil
  local cur_fy = state.squad_blitz_engage_my and (state.squad_blitz_engage_my + 0.5) or nil
  for entry in string.gmatch(brj, "[^;]+") do
    local pn, fx, fy = entry:match("^(%d+):%[(%-?[%d.]+),(%-?[%d.]+)%]$")
    if pn then
      if tonumber(pn) == self_pn then
        -- Spot-tagged reject: fresh only if it matches our current offer (small
        -- epsilon — the spot round-trips as a 4dp float string).
        local match = cur_fx and (math.abs(tonumber(fx) - cur_fx) < 0.01 and math.abs(tonumber(fy) - cur_fy) < 0.01)
        if (not cur_fx) or match then
          state._blitz_call_rejected = true
        elseif BRAIN_DEBUG_MODE then
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

function M.update(state, info, now, world)
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
    if not p or (p.health or 0) <= 0 or p.owner == "friendly" or p.owner == "allied" then
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
    if not why and bs.by then
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
    state.suicider_src = "blitz"
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
      end
    end
  end

  if role ~= M.ROLE_SOLDIER then state.squad_blitz_accepted = nil end  -- only soldiers commit to a blitz

  if role == M.ROLE_COMMANDER then
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
          return role
        end
      else
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
    if best_pn then
      local ok, reason = M.availability(state, info, best_target)
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
    end
    -- Target pill column (blank for non-pill goals).
    if b.target ~= nil then
    end
    local st = b.status and (STATUS_TEXT[b.status] or b.status) or ""
    if st ~= "" then
    end
    y = y + row_h
  end

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
        -- Parked at its firing spot (past approach), independent of aim — the
        -- overwhelm wait (blitz_wait) OR anywhere in the PPT firing sequence
        -- (aim → in_range_* → shoot_pill). A soldier here is "go-able": it fires
        -- on GO without travelling, so it counts toward the early-GO quorum.
        if M.BLITZ_READY_SUBS[h.sub or ""] then inwait = inwait + 1 end
        if h.rdy == "1" then
          ready = ready + 1
        else
          -- Still approaching/aiming — track the closest one's REMAINING walk
          -- distance so the commander can tell whether anyone is still closing.
          -- Prefer measuring it ourselves from the soldier's LIVE position to its
          -- broadcast standoff (bes); fall back to its broadcast bd when its tank
          -- is out of our perception (and flag it so we can query it directly).
          local bd, seen = nil, false
          local p = pos_by_pn and pos_by_pn[pn]
          if p and h.bes then
            local bx, by = h.bes:match("^(%-?[%d.]+),(%-?[%d.]+)$")
            if bx then bd = U.mdist(p.mx, p.my, tonumber(bx), tonumber(by)); seen = true end
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
          local bx, by = h.bes:match("^(%-?[%d.]+),(%-?[%d.]+)$")
          if bx then
            bx, by = tonumber(bx), tonumber(by)
            local rc = (h.rdy == "1") and { 120, 255, 120 } or { 200, 120, 200 }
            local tag = (h.role == "c") and "C" or "S"
          end
        end
      end
    end
  end

  -- ── Our own engage spot + setup point + line to pill ───────────────────
  if g and g._blitz and state.squad_blitz_engage_mx then
    local ex, ey = state.squad_blitz_engage_mx + 0.5, state.squad_blitz_engage_my + 0.5
    local col = committed and { 80, 220, 255 }
                or (state.squad_blitz_in_position and { 120, 255, 120 }
                or { 255, 0, 255 })
    local lbl = committed and "ENGAGE (GO)"
                or (state.squad_blitz_in_position and "ENGAGE (READY)" or "ENGAGE")
    if g.mx and g.my then
    end
    if g.approach_mx and g.approach_my then
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
    if line2 then
      local lc = committed and { 80, 220, 255 } or { 255, 230, 120 }
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
      local fx, fy = bes:match("^(%-?[%d.]+),(%-?[%d.]+)$")
      if fx then claims[#claims + 1] = { fx = tonumber(fx), fy = tonumber(fy) } end
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
  if info.objects then
    for _, ob in ipairs(info.objects) do
      if ob.type == 0 and ob.idnum and ob.idnum ~= info.player_number then
        local ar = ally_state.get_key(ob.idnum, "role")
        if ar and ar ~= "" then
          local ar2, ag, ab = role_color(ar)
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
  y = y + dy
  local ans, ar, ag, ab
  if state.squad_blitz_accepted then
    ans, ar, ag, ab = "COMMITTED -> C" .. tostring(state.squad_blitz_accepted), 120, 230, 120
  elseif state.squad_negotiate_cmdr then
    ans, ar, ag, ab = "negotiating -> C" .. tostring(state.squad_negotiate_cmdr), 230, 210, 120
  else
    ans, ar, ag, ab = "N (no call)", 180, 180, 180
  end
  y = y + dy
  local so = state.squad_blitz_engage_mx
             and string.format("standoff (%.4f,%.4f)", state.squad_blitz_engage_mx + 0.5, state.squad_blitz_engage_my + 0.5)
             or "standoff -"
  y = y + dy
  local repos = state.squad_blitz_repos or (g and g._blitz_repos) or 0
  -- map: our standoff dot
  if viz.circle and state.squad_blitz_engage_mx then
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

  y = y + dy
  if not cap_pn then
    return
  end
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
        bes = string.format("%.1f,%.1f", state.squad_blitz_engage_mx + 0.5, state.squad_blitz_engage_my + 0.5)
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
        local fx, fy = b:match("^(%-?[%d.]+),(%-?[%d.]+)$")
        if fx then bes = string.format("%.1f,%.1f", tonumber(fx), tonumber(fy)) end
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
    end
    -- Current bot (us) → white border around the row (distinct from the yellow commander fill).
    if m.is_self then
    end
    -- Negotiating (offered a standoff, not yet accepted → sqst "nego") renders
    -- YELLOW: a pending tank, NOT counted as a committed 2-tank blitz member yet.
    -- Committed/other rows stay green.
    local sqst
    if m.is_self then sqst = state.squad_status else sqst = ally_state.get_key(m.pn, "sqst") end
    local tr, tg, tb = 140, 230, 140
    if sqst == "nego" then tr, tg, tb = 245, 215, 50 end
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
  y = y + dy
  y = y + dy
  -- Party size vs the HARD quorum: no GO path fires below MIN, and on the
  -- timeout a short-handed commander abandons the take instead of charging.
  do
    local party = 1 + (v.total or 0)
    local short = party < (v.blitz_min or 2)
    y = y + dy
  end
  y = y + dy
  y = y + dy
  -- Source of the progress signal: live (we can see all pending soldiers) vs a
  -- fallback on an unseen soldier's broadcast bd, and whether we're actively
  -- querying it for a fresh position in the final window.
  local src = v.querying and "QUERYING unseen..." or (v.unseen and "unseen (broadcast bd)" or "live positions")
  y = y + dy
  -- Bar: elapsed vs effective timeout, filling left->right within the track.
  local barw = 220
  local frac = math.min(1.0, elapsed / math.max(1, eff))
  local fillw = math.max(1, math.floor(barw * frac))
  local fr = (closing and not v.timed_out) and 120 or 235
  local fg = (closing and not v.timed_out) and 220 or 120
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
  y = y + dy
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
      end
    end
    y = y + dy
  end
  if n == 0 then
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
  end

  -- C) Transient decline: we said "busy"/no-spot to a call this or a recent
  -- tick. Latched for BLITZ_COMM_LATCH_TICKS so a one-tick reject is actually
  -- visible (red line to the commander we declined). Skipped if we now have an
  -- active (yellow/orange/green) line to that same commander.
  local rej = state._blitz_comm_reject
  if rej and rej.cmdr and rej.cmdr ~= cmdr and pos[rej.cmdr]
     and (now - (rej.tick or -100000)) <= (C.BLITZ_COMM_LATCH_TICKS or 18) then
    local cp = pos[rej.cmdr]
  end
end

-- hard_take_pills: ring + HP on enemy pills at/above HARD_TAKE_MIN_HP — the
-- targets that spawn a commander+squad when DYNAMIC_COMMANDERS is on.
function M.draw_hard_takes(world, now)
  if not viz.is_on("hard_take_pills") or not viz.circle or not world or not world.pills then return end
  local minhp = C.HARD_TAKE_MIN_HP or 12
  for _, p in pairs(world.pills) do
    if (p.owner == "hostile" or p.owner == "neutral") and (p.health or 0) >= minhp then
      if viz.text then
      end
    end
  end
end

return M
