-- =============================================================================
-- reposition_vote.lua — team-consensus gate for pill repositioning
-- =============================================================================
-- A back pill is only relocated after a TEAM VOTE passes. Each bot scores its
-- own best reposition candidate every replan (goals.lua eval_reposition_pill,
-- exported on state._repo_candidate). When a bot is eligible it OPENS a vote
-- (broadcasts /info rvo); allies cast YES/NO (/info rvy|rvn); after a short
-- window the initiator resolves: silence = abstain = YES, ANY no blocks. On a
-- PASS the initiator is approved to carry the move out via the normal
-- capture_pill + reposition_shoot mechanic (goals gates the actionable goal on
-- state._repo_approved_pid).
--
-- Allies vote NO when (any of):
--   * a HUMAN player is on our team and
--     REPOSITION_DISABLE_WITH_HUMAN_ALLIES is on (human_allies) — the whole
--     mechanic is off in mixed teams; also blocks the OPEN gate and
--     eval_reposition_pill's candidate
--   * the pill is in use as a take blocker (_in_use)
--   * they remember a reposition EXECUTING within the last ~30s (one move
--     per RECENT_MEMORY window; urgent negative-score proposals exempt);
--     any FAILED vote additionally holds all proposals ~30s (FAIL_COOLDOWN)
--   * the pill was PLACED less than PILL_JUST_BUILT_TICKS ago (just_built) —
--     a pill built a minute ago doesn't get moved again
--   * ANY enemy tank is within 15 tiles AND nothing else covers the pill
--   * the pill IS covered by >=1 pill but ANY enemy tank is within 10 tiles
--   * they have a strictly better (lower-cost) reposition candidate of their own
--
-- Both proximity gates trip on a single enemy tank, but LOCAL STRENGTH relaxes
-- them on a ladder. Count allied and enemy tanks within the gate's BASE range
-- of the pill, with the voter counting ITSELF when its own tank is in there:
--   * allies >  enemies      → gate range shrinks x REPOSITION_STALE_RANGE_SCALE
--   * allies >= 2 * enemies  → the distance gates are SKIPPED entirely
-- Equal numbers buys nothing. Only the two distance gates are affected — a
-- skip never bypasses blocker / just_built / recent_repo / better_candidate.
--
-- STALENESS independently shrinks the range by the same 0.8: when the team's
-- last actual reposition is older than REPOSITION_STALE_TICKS (or never
-- happened at all), a frozen back line should get braver about approving a
-- move. Staleness and strength do NOT stack — the scale floors at a single 0.8
-- application, so stale AND allies together is still 0.8, never 0.64.
--
-- All wire traffic rides init.lua's comms batcher (state._repo_outbox drained
-- there). Inbound verbs are parsed in comms.lua onto state._repo_rx_*.
-- =============================================================================

local C          = require("constants")
local viz        = require("viz")
local print2     = require("print2")
local ally_state = require("ally_state")
local bit        = require("bitcompat")
local U          = require("util")

local M = {}

-- Human allies on our team, as a COUNT, but 0 whenever the policy flag is off.
-- One place decides "is repositioning human-blocked right now", and it returns
-- a number so the ballot line can say how many were seen. See
-- util.human_ally_count for why the detection needs no join-lag grace.
local function human_ally_block(info)
  if not C.REPOSITION_DISABLE_WITH_HUMAN_ALLIES then return 0 end
  return U.human_ally_count(info)
end

local function tdist(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  return math.sqrt(dx * dx + dy * dy)
end

-- Can THIS bot actually carry a reposition out RIGHT NOW? Mirrors the carry
-- rejects in goals.eval_reposition_pill, but evaluated fresh every tick (the
-- position scan that built the candidate only runs ~every 50t on a quiet tick).
local function can_carry_now(state, info)
  -- One carried pill (the PLACE_HOLD_UTIL utility reserve) must NOT lock the
  -- bot out of repositioning: the engine multi-carries fine and the swap
  -- returns to one carried after the re-drop. Requiring 0 made any bot
  -- holding a reserve permanently unable to propose OR execute — with the
  -- whole team holding reserves the system went silent while negative-score
  -- pills sat unmoved (20260703_233224 t~73257). Two+ carried = hands full.
  if (info.carried_pills or 0) >= 2 then return false end
  if info.man_status ~= C.LGM_INTANK then return false end
  if info.inboat then return false end
  if (info.shells or 0) < (C.PILL_REPOSITION_MIN_SHELLS or 15) then return false end
  if state.phase == "opening" then return false end
  if state._reposition_cooldown_tick
     and (state.tick or 0) - state._reposition_cooldown_tick < (C.PILL_REPOSITION_COOLDOWN_TICKS or 0) then
    return false
  end
  return true
end

-- Friendly DEPLOYED pills (health>0) within fire range of (mx,my), excluding pid.
local function covering_pills(world, pid, mx, my)
  local n = 0
  local R = C.PILL_FIRE_RANGE or 7
  for id, p in pairs(world.pills) do
    if id ~= pid and p.owner == "friendly" and (p.health or 0) > 0
       and not p.in_tank and tdist(mx, my, p.mx, p.my) <= R then
      n = n + 1
    end
  end
  return n
end

-- Tank census within R tiles of (mx,my): how many ENEMY tanks and how many
-- ALLIED tanks (us excluded) are sitting on the pill right now.
--
-- Enemies come from perc.enemy_tanks (real sightings only — ghosts live in a
-- separate list). Allies come straight from info.objects' tank entries with the
-- hostility bit clear, the same signal defend_pill's well-defended gate uses:
-- there is no perception list of allied tank POSITIONS, and ally_state's
-- broadcast mx/my is the ally's GOAL TARGET, not where its tank is. The engine
-- never puts our own tank in info.objects (players.c playersGetBrainTanksInRect
-- skips count == my player number), and the idnum test below is a belt-and-
-- braces second exclusion in case that ever changes.
local function tanks_near(state, info, mx, my, R)
  local n_enemy, n_ally = 0, 0
  local ets = state.perc and state.perc.enemy_tanks
  if ets then
    for _, et in ipairs(ets) do
      if et.mx and et.my and tdist(mx, my, et.mx, et.my) <= R then
        n_enemy = n_enemy + 1
      end
    end
  end
  local self_pn = info and info.player_number
  for _, ob in ipairs((info and info.objects) or {}) do
    if ob.type == OBJECT_TANK
       and bit.band(ob.info, OBJECT_HOSTILE) == 0
       and ob.idnum ~= self_pn then
      local amx = bit.rshift(ob.x, 8)
      local amy = bit.rshift(ob.y, 8)
      if tdist(mx, my, amx, amy) <= R then n_ally = n_ally + 1 end
    end
  end
  return n_enemy, n_ally
end

-- Range table for a proximity gate whose BASE range is `base` tiles: the
-- effective range plus every factor that produced it (for the ballot log).
--
-- Local strength around the pill relaxes the gates on a LADDER, measured at the
-- BASE range with the voter counted on its own side:
--   * allies >  enemies        → ranges x REPOSITION_STALE_RANGE_SCALE (0.8)
--   * allies >= 2 * enemies    → the distance gates are SKIPPED outright
-- (and >=2x implies >, so the skip tier always carries the scale flag too).
-- Equal numbers is not "more": 1v1 buys nothing. Holding the ground around the
-- pill is what earns the shorter leash, and holding it 2:1 means enemy presence
-- is no reason to veto at all — the team can cover the pill's downtime.
--
-- Staleness is a separate, independent reason to shrink the range, worth the
-- SAME scale, and the two do NOT compound — the floor is one application, so
-- stale+allies is 0.8 rather than 0.64:
--   * stale  — no team reposition within REPOSITION_STALE_TICKS (or ever)
--
-- Self-counting is the exact inverse of tanks_near's exclusion: the engine
-- never puts our own tank in info.objects (players.c skips our player number),
-- so tanks_near can't see us and we add ourselves here from info.tankx/tanky.
-- Both halves are needed — tanks_near must keep excluding us so a duplicate
-- object entry can never double-count the voter.
--
-- Both censuses are taken at the BASE range on purpose: the discount is earned
-- by the balance of force in the gate's natural reach, not inside the shrunken
-- circle the discount itself produces (which would be circular).
local function gate_range(state, info, mx, my, base, now)
  local last  = state._repo_last_seen_tick
  local stale = (not last) or (now - last) >= (C.REPOSITION_STALE_TICKS or 3000)
  local n_enemy_base, n_ally = tanks_near(state, info, mx, my, base)
  local self_in = false
  if info and info.tankx and info.tanky then
    local smx = bit.rshift(info.tankx, 8)
    local smy = bit.rshift(info.tanky, 8)
    if tdist(mx, my, smx, smy) <= base then
      self_in = true
      n_ally  = n_ally + 1
    end
  end
  local allies = n_ally > n_enemy_base
  -- 2:1 or better = skip the distance gates entirely. n_ally >= 1 keeps the
  -- degenerate 0 vs 0 case (nobody anywhere near the pill) off the skip path —
  -- it can't trip a gate anyway, and leaving it on the normal path keeps the
  -- ballot line reporting a real range instead of a meaningless "skip".
  local skip   = n_ally >= 1 and n_ally >= 2 * n_enemy_base
  local scale  = (stale or allies) and (C.REPOSITION_STALE_RANGE_SCALE or 0.8) or 1.0
  return { R = base * scale, base = base, scale = scale,
           stale = stale, allies = allies, skip = skip,
           na = n_ally, self_in = self_in, neb = n_enemy_base }
end

-- Per-tick: stamp state._repo_enemy_activity[pid] = now whenever a hostile pill
-- is within PILL_RANGE, or a hostile LGM within LGM_RANGE, of a friendly pill.
-- goals.eval_reposition_pill reads this as a DECAYING penalty — repositioning
-- kills the pill temporarily, so it's dangerous while enemies are (or recently
-- were) near. The stamp must run every tick (LGM sightings are transient); the
-- decay window means the penalty lingers ~30s after the enemy leaves.
local function stamp_enemy_activity(state, world, now)
  if not world or not world.pills then return end
  local act = state._repo_enemy_activity or {}
  local PR  = C.PILL_REPOSITION_ENEMY_ACTIVITY_PILL_RANGE or 10
  local LR  = C.PILL_REPOSITION_ENEMY_ACTIVITY_LGM_RANGE or 6
  local elgms = (state.perc and state.perc.enemy_lgms) or {}
  for pid, p in pairs(world.pills) do
    if (p.owner == "friendly" or p.owner == "allied")
       and (p.health or 0) > 0 and not p.in_tank then
      local hot = false
      for _, op in pairs(world.pills) do
        if op.owner == "hostile" and (op.health or 0) > 0
           and tdist(p.mx, p.my, op.mx, op.my) <= PR then hot = true; break end
      end
      if not hot then
        for _, lg in ipairs(elgms) do
          if lg.mx and tdist(p.mx, p.my, lg.mx, lg.my) <= LR then hot = true; break end
        end
      end
      if hot then act[pid] = now end
    end
  end
  -- Prune stale entries (pill went quiet / died) once its penalty has decayed.
  if (now % 250) == 0 then
    local decay = C.PILL_REPOSITION_ENEMY_ACTIVITY_DECAY_TICKS or 1500
    for pid, t in pairs(act) do
      if now - t > decay then act[pid] = nil end
    end
  end
  state._repo_enemy_activity = act
end

-- Is a reposition actually IN FLIGHT for this bot right now?
--
-- In win-then-vote the goal becomes capture_pill+reposition at BID time, before
-- any vote exists — that bid must NOT count as a reposition. It used to: the
-- ally-state broadcast advertised repos=1 for any capture_pill+reposition goal,
-- so the proposal (/info rvo) and the proposer's own repos=1 flag rode the SAME
-- packet; every voter stamped _repo_last_seen_tick from the flag and then vetoed
-- the proposal as recent_repo (now-now = 0) — a guaranteed self-veto.
--
-- In flight means: the team approved THIS pill for us, we're demolishing it, or
-- we're past the shoot-down and carrying/replacing it (goals.lua's
-- "finishing_swap" — the approval is cleared on consumption but the move is very
-- much still happening, and the recent-memory window should time from its END).
function M.is_reposition_active(state, world)
  local g = state.goal
  if not (g and g.kind == "capture_pill" and g.reposition) then return false end
  if g.target_id and state._repo_approved_pid == g.target_id then return true end
  if g.substate == "reposition_shoot" then return true end
  local tp = g.target_id and world and world.pills and world.pills[g.target_id]
  if tp and tp.owner == "friendly" and ((tp.health or 0) <= 0 or tp.in_tank) then
    return true
  end
  return false
end

-- Decide THIS bot's vote on a proposed reposition. prop = {pid,mx,my,score}.
-- Returns is_no(bool), reason(string).
local function evaluate_vote(state, world, info, now, prop)
  local p  = world.pills[prop.pid]
  local mx = (p and p.mx) or prop.mx
  local my = (p and p.my) or prop.my

  -- Policy NO, ahead of everything else: with a human on our team we don't
  -- reposition at all (REPOSITION_DISABLE_WITH_HUMAN_ALLIES). Every bot runs
  -- this check independently, so even if the proposer somehow opened a vote —
  -- stale info, a slot flagged late, the flag toggled mid-game — the ballots
  -- kill it. Belt-and-braces with the OPEN gate and eval_reposition_pill.
  local humans = human_ally_block(info)
  if humans > 0 then
    return true, "human_allies", { humans = humans }
  end

  if p and p._in_use then return true, "blocker" end

  -- Freshly placed pills are OFF LIMITS for a minute. A pill that just went
  -- down is the product of a decision someone made seconds ago (a strategic
  -- placement, a rebuild, or the tail of a reposition that just landed);
  -- yanking it straight back up churns the back line and wastes the LGM trip
  -- that put it there. placed_tick is stamped by the world model on a real
  -- placement only (carry->deployed, or alive at a NEW tile) — never on a
  -- plain re-sighting — and is nil for pills we've only ever seen standing
  -- where they started, which are correctly not "just built".
  if p and p.placed_tick then
    local age = now - p.placed_tick
    local lim = C.PILL_JUST_BUILT_TICKS or 1500
    if age < lim then
      return true, "just_built", { age = age, limit = lim }
    end
  end

  -- Pacing NO — but never against an URGENT proposal: a negative score means
  -- the pill's position is actively harmful (deep surplus / redundancy /
  -- orphaned). The recent-memory pacing exists to stop marginal churn, not
  -- to ration urgent corrections — one move per 2 minutes team-wide is far
  -- too slow to fix a badly lopsided back line.
  if (prop.score or 0) >= (C.REPOSITION_URGENT_SCORE or 0)
     and state._repo_last_seen_tick
     and (now - state._repo_last_seen_tick) < (C.REPOSITION_VOTE_RECENT_MEMORY_TICKS or 1500) then
    return true, "recent_repo"
  end

  -- Proximity gates. ANY enemy tank inside the range vetoes: repositioning
  -- kills the pill for the duration of the move, and one tank is enough to
  -- punish that. What allies buy is not immunity but a SHORTER leash — see
  -- gate_range: their presence (or a stale team clock) shrinks the range by
  -- 0.8, so the same enemy has to be closer before it counts.
  local cover = covering_pills(world, prop.pid, mx, my)
  local base  = (cover == 0) and (C.REPOSITION_VOTE_ENEMY_NEAR_TILES or 15)
                             or (C.REPOSITION_VOTE_TANK_COVER_TILES or 10)
  -- gate holds every input of the decision, and is handed back for the log.
  local gate  = gate_range(state, info, mx, my, base, now)
  if gate.skip then
    -- 2:1 local superiority: neither distance gate may trip. Report the
    -- base-range enemy count so the line still shows what we're ignoring.
    gate.ne = gate.neb
  else
    -- Enemies counted at the EFFECTIVE range; the censuses that set that range
    -- were taken at the base range.
    gate.ne = tanks_near(state, info, mx, my, gate.R)
    if gate.ne >= 1 then
      if cover == 0 then
        return true, "enemy_uncovered", gate
      end
      return true, "tank_near_covered", gate
    end
  end

  local mine = state._repo_candidate
  if mine and mine.pid ~= prop.pid and mine.can_carry
     and (mine.score or math.huge) < (prop.score or math.huge) then
    return true, "better_candidate", gate
  end

  return false, "ok", gate
end

-- -------------------------------------------------------------------------
-- M.check_bid_timeout(state, world, now) -> true on the tick the bid expires
--
-- A capture_pill+reposition goal that WINS the pool is only a BID: steering
-- refuses to fire until state._repo_approved_pid names the pill, so an
-- unapproved bid parks in `approach` beside a pill it may never touch — for the
-- rest of the game if the approval never lands (proposal dropped by the batcher,
-- an ally NO with the result packet lost, or the OPEN gate never let the vote
-- start). Bound it: the vote gets its full WINDOW plus BID_TIMEOUT_MARGIN, then
-- the bid is declared dead and the caller drops the goal.
--
-- The expiry reuses the existing FAIL_COOLDOWN pacing (_repo_fail_tick) rather
-- than a parallel cooldown: a bid that never earned approval is a failed one, so
-- reposition_vote's OPEN gate and goals.eval_reposition_pill's "vote_cooldown"
-- reject both hold the pool off it for the same window.
-- -------------------------------------------------------------------------
function M.check_bid_timeout(state, world, now)
  local g = state.goal
  local is_bid = g and g.kind == "capture_pill" and g.reposition and g.target_id
                 and not M.is_reposition_active(state, world)
  if not is_bid then
    state._repo_bid_tick, state._repo_bid_pid = nil, nil
    return false
  end
  if state._repo_bid_tick == nil or state._repo_bid_pid ~= g.target_id then
    state._repo_bid_tick, state._repo_bid_pid = now, g.target_id
    return false
  end
  local limit = (C.REPOSITION_VOTE_WINDOW_TICKS or 25)
              + (C.REPOSITION_VOTE_BID_TIMEOUT_MARGIN or 15)
  -- Our own ballot may open a replan AFTER the bid was adopted, so give an
  -- in-flight vote the same budget measured from ITS open tick.
  local mv = state._repo_my_vote
  if mv and mv.pid == g.target_id and (now - mv.open_tick) < limit then return false end
  if (now - state._repo_bid_tick) < limit then return false end
  state._repo_fail_tick = now
  state._repo_bid_tick, state._repo_bid_pid = nil, nil
  state._repo_my_vote = nil
  return true
end

-- -------------------------------------------------------------------------
-- M.update(state, world, info, now)
-- Drive the vote state machine for one tick. Queues outbound verbs on
-- state._repo_outbox (init.lua's send section drains them via try_send).
-- -------------------------------------------------------------------------
function M.update(state, world, info, now)
  -- Enemy-activity decay feeds the reposition score every replan, so stamp it
  -- each tick regardless of whether voting itself is enabled.
  stamp_enemy_activity(state, world, now)
  if C.REPOSITION_VOTE_ENABLED == false then return end
  -- _repo_outbox PERSISTS across ticks: the send section removes only the
  -- messages that fit the batch this tick, so an overflowed ballot/result is
  -- retried next tick (a dropped NO must never let a bad reposition pass).
  local function tx(msg)
    state._repo_outbox = state._repo_outbox or {}
    state._repo_outbox[#state._repo_outbox + 1] = msg
  end

  local WINDOW  = C.REPOSITION_VOTE_WINDOW_TICKS or 10
  local self_pn = info.player_number

  -- Refresh carry-eligibility on the cached candidate (+ top-N for the viz)
  -- every tick — the position scan that built them runs only periodically.
  local carry = can_carry_now(state, info)
  if state._repo_candidate then state._repo_candidate.can_carry = carry end
  if state._repo_topN then
    for i = 1, #state._repo_topN do state._repo_topN[i].can_carry = carry end
  end

  -- 1. Inbound RESULT from another initiator.
  -- NOTE: a PASS no longer sets _repo_last_seen_tick — the memory (which
  -- suppresses proposals AND drives the recent_repo NO-vote) must track
  -- repositions that actually HAPPEN, not votes that merely passed. An
  -- approval that expires unconsumed used to blackout the whole team for
  -- ~2 min for nothing (seen in 20260703_202213: both initiators too busy,
  -- approval wasted, team silent until the session ended). Execution is
  -- observed via the ally repos-goal broadcast below / our own consumption.
  local res = state._repo_rx_result
  if res then
    -- Guard the pill immediately on a PASSED vote: the initiator will shoot
    -- it down shortly, and its repos=1 goal broadcast takes a heartbeat to
    -- arrive — without this an ally could dispatch a repair onto the pill in
    -- that window (topping it up as the initiator starts demolishing it).
    -- Refreshed by the repos-goal observation below once the move runs.
    if res.pass and world.pills and world.pills[res.pid] then
      local ap = world.pills[res.pid]
      state._repos_guard = state._repos_guard or {}
      state._repos_guard[ap.my * 256 + ap.mx] = now + (C.REPOS_GUARD_TTL or 300)
    end
    -- 30/30 pacing: a FAILED vote (anyone's) holds ALL proposals for
    -- FAIL_COOLDOWN — retrying a just-vetoed move via a different proposer
    -- is spam; the NO reasons haven't changed.
    if not res.pass then state._repo_fail_tick = now end
    state._repo_active = nil
    state._repo_vote_panel = { pid = res.pid, from = res.from, pass = res.pass,
                               until_tick = now + (C.REPOSITION_VOTE_RESULT_LATCH_TICKS or 120) }
    state._repo_rx_result = nil
  end

  -- 1a. Observed ally reposition EXECUTION (rvx): a one-shot, batcher-reliable
  -- stamp fired the moment an ally consumes its approval (the move actually
  -- happened). This is the authoritative shared "last reposition" clock. The
  -- repos=1 heartbeat below still refreshes during a move, but an initiator can
  -- miss it between broadcasts and then open a vote the executing ally vetoes as
  -- recent_repo; rvx closes that gap so mem_ok self-gating matches the voters.
  -- Take the max (exec_tick is server-synced) so a retried/out-of-order packet
  -- never rewinds the memory.
  local rx_exec = state._repo_rx_exec
  if rx_exec then
    local et = rx_exec.exec_tick or now
    if not state._repo_last_seen_tick or et > state._repo_last_seen_tick then
      state._repo_last_seen_tick = et
    end
    if not state.last_team_reposition_tick or et > state.last_team_reposition_tick then
      state.last_team_reposition_tick = et
    end
    state._repo_rx_exec = nil
  end

  -- 1b. Observe EXECUTING ally repositions: an ally broadcasting an active
  -- reposition goal (capture_pill + repos=1 in ally_state) IS a reposition
  -- happening — that's what the recent-memory should time from. Refreshes
  -- every tick the move runs, so the ~2 min window starts at its END.
  --
  -- Same pass maintains state._repos_guard: a tile-keyed TTL map of pills
  -- that are mid-reposition (ally targets here; our own goal/approval below).
  -- goals.filter_repair_pill refuses to repair/rebuild guarded tiles — the
  -- repair pool once resurrected a reposition corpse during the one-tick
  -- handoff between shoot-down and pickup (20260703_210207 t=6997).
  local guard = state._repos_guard
  local guard_ttl = C.REPOS_GUARD_TTL or 300
  local function guard_mark(gmx, gmy)
    if not gmx then return end
    guard = guard or {}
    guard[gmy * 256 + gmx] = now + guard_ttl
  end
  --
  -- Belt-and-braces on the stamp: never let a proposer's own repos flag veto its
  -- OWN proposal. The winning bid and the /info rvo ride the same packet, so if
  -- a proposer ever advertises repos=1 while its vote is open (the old emit gate
  -- did exactly that — see M.is_reposition_active), we'd stamp the memory a tick
  -- before balloting and then NO it as recent_repo. Skip the proposer of every
  -- proposal we're about to evaluate; every other ally still stamps normally,
  -- and the guard-mark below is unaffected (guarding the pill is always right).
  local proposers = nil
  local _opq_pre = state._repo_rx_open
  if _opq_pre then
    for i = 1, #_opq_pre do
      local f = _opq_pre[i].from
      if f and f ~= self_pn then proposers = proposers or {}; proposers[f] = true end
    end
  end
  local _act_from = state._repo_active and state._repo_active.from
  if _act_from and _act_from ~= self_pn then
    proposers = proposers or {}; proposers[_act_from] = true
  end
  if ally_state.iter_active then
    for apn, slot in ally_state.iter_active(now, 1750) do
      if apn ~= self_pn and slot.info and slot.info.repos == "1" then
        if not (proposers and proposers[apn]) then
          state._repo_last_seen_tick = now
        end
        guard_mark(tonumber(slot.info.mx), tonumber(slot.info.my))
      end
    end
  end
  -- Our own reposition target: guard it ONLY once the move is APPROVED / in-flight
  -- (or being carried below), NOT for a pre-approval BID. In win-then-vote the goal
  -- becomes a reposition at BID time BEFORE the vote opens; guarding it then would
  -- self-block the OPEN gate below (which refuses guarded pills), so the vote could
  -- never open for the very pill the goal just committed to — reposition stalled
  -- forever (0 votes, 0 reposition_shoot). Guard only from approval onward; the
  -- REPOS_GUARD_TTL tail then covers the shoot-down -> pickup handoff.
  if state.goal and state.goal.kind == "capture_pill" and state.goal.reposition
     and state._repo_approved_pid == state.goal.target_id then
    guard_mark(state.goal.mx, state.goal.my)
  end
  if state._repo_approved_pid and world.pills then
    local ap = world.pills[state._repo_approved_pid]
    if ap then guard_mark(ap.mx, ap.my) end
  end
  -- Lazy prune so the table can't grow unbounded across a long game.
  if guard and (now % 250) == 0 then
    for k, untl in pairs(guard) do
      if untl <= now then guard[k] = nil end
    end
  end
  state._repos_guard = guard

  -- 2. Inbound PROPOSALS: record the active vote, cast our ballot once each.
  --    The queue may hold several proposals opened in the same tick — handle
  --    every one so no ally's vote is silently dropped.
  local opq = state._repo_rx_open
  if opq then
    for i = 1, #opq do
      local op = opq[i]
      state._repo_active = { pid = op.pid, from = op.from, tick = op.tick }
      if op.from ~= self_pn then
        -- Double-open tiebreak: if WE also have an open vote for this SAME
        -- pill (both bots proposed it in the same tick — seen in
        -- 20260703_202213 where p0 and p1 both passed and both held an
        -- approval), the LOWER player number keeps its proposal; the higher
        -- one cancels its own and just ballots on the survivor. Prevents two
        -- simultaneous approvals for one pill.
        if state._repo_my_vote and state._repo_my_vote.pid == op.pid
           and (op.from or 99) < (self_pn or 0) then
          state._repo_my_vote = nil
        end
        local is_no, reason = evaluate_vote(state, world, info, now, op)
        tx(is_no and ("/info rvn " .. op.pid) or ("/info rvy " .. op.pid))
        -- Remember our own ballot + WHY (we never receive our own vote back, so the
        -- votes visualizer reads our reason from here).
        state._repo_my_ballot = { pid = op.pid, no = is_no, reason = reason, tick = now }
      end
    end
    state._repo_rx_open = nil
  end
  if state._repo_active and (now - state._repo_active.tick) > (WINDOW + 30) then
    state._repo_active = nil   -- proposer went silent; release the lock
  end

  -- 3. Our OWN open vote: tally NOs, resolve after the window.
  local mv = state._repo_my_vote
  if mv then
    local votes = state._repo_rx_votes
    if votes then
      for pn, v in pairs(votes) do
        if v.pid == mv.pid and v.tick >= mv.open_tick then
          if v.no then
            mv.no_set = mv.no_set or {}; mv.no_set[pn] = true
          else
            mv.yes_set = mv.yes_set or {}; mv.yes_set[pn] = true
          end
        end
      end
    end
    local no_n = 0
    if mv.no_set then for _ in pairs(mv.no_set) do no_n = no_n + 1 end end
    mv.no = no_n
    -- Resolve as soon as EVERY currently-active ally has cast a ballot — no need
    -- to sit out the whole window once the responses are all in. WINDOW is just a
    -- hard timeout cap (~1s) so a silent ally can't stall the vote.
    local n_allies, n_resp, seen_resp = 0, 0, {}
    if ally_state.iter_active then
      for apn in ally_state.iter_active(now, 1750) do
        if apn ~= self_pn then n_allies = n_allies + 1 end
      end
    end
    if mv.yes_set then for pn in pairs(mv.yes_set) do seen_resp[pn] = true end end
    if mv.no_set  then for pn in pairs(mv.no_set)  do seen_resp[pn] = true end end
    for _ in pairs(seen_resp) do n_resp = n_resp + 1 end
    local all_in = (n_resp >= n_allies)
    if all_in or (now - mv.open_tick) >= WINDOW then
      local pass = (no_n == 0)   -- silence = abstain = yes; any NO blocks
      tx(string.format("/info rvr %d %d", mv.pid, pass and 1 or 0))
      if pass then
        state._repo_approved_pid    = mv.pid
        state._repo_approved_tick   = now
        state._repo_approved_used   = 0   -- TTL burned only while we can act (see watch below)
        -- _repo_last_seen_tick / last_team_reposition_tick are NOT set here
        -- any more — they now mark CONSUMPTION (the pill actually taken),
        -- so an unusable approval doesn't blackout the team's proposals.
      else
        -- 30/30 pacing: our own fail also starts the team-wide hold (the
        -- receivers start theirs from the rvr result broadcast).
        state._repo_fail_tick = now
      end
      state._repo_vote_panel = { pid = mv.pid, from = self_pn, pass = pass,
                                 yes = mv.yes_set, no = mv.no_set,
                                 until_tick = now + (C.REPOSITION_VOTE_RESULT_LATCH_TICKS or 120) }
      state._repo_my_vote = nil
      state._repo_active  = nil
    end
  end

  -- Clear the approval only when its window runs out. It deliberately survives
  -- the first commit (and brief preemption by refuel/survival) so the
  -- initiator's reposition keeps its fixed cost-80 priority for the WHOLE ~30s
  -- window — long enough to travel to, pick up, move and re-drop the pill.
  -- goals.lua's APPROVED branch reads state._repo_approved_pid every tick.
  --
  -- The TTL burns only on ticks the bot could actually act (can_carry_now:
  -- LGM in tank, hands free, shells, no cooldown) — a busy stretch (LGM out
  -- farming, mid-refuel) PAUSES the countdown instead of silently eating the
  -- window (the 20260703_202213 failure mode). A wall-clock cap at 3× TTL
  -- still bounds a permanently-blocked approval.
  if state._repo_approved_pid then
    local ap = world.pills and world.pills[state._repo_approved_pid]
    local consumed = (not ap) or (ap.health or 0) <= 0 or ap.in_tank
    if consumed then
      -- The pill was actually taken (shot to 0 / picked up / re-dropped) — the
      -- move is underway and held by the reposition lock from here. Drop the
      -- approval so we don't re-target this same pill again inside the window
      -- (e.g. reposition it a second time right after it was just re-built).
      -- THIS is when the recent-reposition memory starts: a move happened.
      state._repo_last_seen_tick      = now
      state.last_team_reposition_tick = now   -- feed the existing team time-discount
      -- Broadcast the completion so every ally stamps the SAME recent-memory tick
      -- (rvx: reposition executed). Reliable via the retrying batcher — closes the
      -- window where an ally that missed our repos=1 heartbeats opens a doomed
      -- vote we then veto with recent_repo. Sent before we clear the pid below.
      tx(string.format("/info rvx %d %d", state._repo_approved_pid, now))
      state._repo_approved_pid  = nil
      state._repo_approved_used = nil
    else
      if carry then
        state._repo_approved_used = (state._repo_approved_used or 0) + 1
      end
      local ttl  = C.REPOSITION_VOTE_APPROVAL_TTL or 1500
      local wall = now - (state._repo_approved_tick or now)
      if (state._repo_approved_used or 0) > ttl or wall > 3 * ttl then
        local committed = state.goal and state.goal.kind == "capture_pill"
                          and state.goal.reposition and state.goal.target_id == state._repo_approved_pid
        if not committed then
        end
        state._repo_approved_pid  = nil
        state._repo_approved_used = nil
      end
    end
  end

  -- 4. Maybe OPEN a new vote. 30/30 pacing:
  --   * one MOVE per RECENT_MEMORY (~30s), timed from consumption/observed
  --     execution — URGENT (negative-score) candidates bypass this;
  --   * any FAILED vote (ours or observed) holds ALL proposals for
  --     FAIL_COOLDOWN (~30s) — no bypass; the NO reasons haven't changed.
  --   * never propose a pill that's guard-marked (someone's move in flight).
  if not state._repo_my_vote and not state._repo_active and not state._repo_approved_pid then
    local cand    = state._repo_candidate
    local fail_ok = not state._repo_fail_tick
                    or (now - state._repo_fail_tick) >= (C.REPOSITION_VOTE_FAIL_COOLDOWN or 1500)
    local mem_ok  = not state._repo_last_seen_tick
                    or (now - state._repo_last_seen_tick) >= (C.REPOSITION_VOTE_RECENT_MEMORY_TICKS or 1500)
    local urgent  = cand and (cand.score or 0) < (C.REPOSITION_URGENT_SCORE or 0)
    local guarded = cand and state._repos_guard
                    and (state._repos_guard[cand.my * 256 + cand.mx] or 0) > now
    -- Win-then-vote: only open a vote once reposition has actually WON the pool
    -- (its honest BID beat every other goal, so it's state.goal for this pill).
    -- Before that the bid just competes; nothing is proposed. This replaces the
    -- old "open whenever a candidate exists" trigger.
    local won_pool = cand and state.goal and state.goal.kind == "capture_pill"
                     and state.goal.reposition and state.goal.target_id == cand.pid
    -- Human teammate on the roster: never open a vote at all
    -- (REPOSITION_DISABLE_WITH_HUMAN_ALLIES). eval_reposition_pill already
    -- refuses to build a candidate, so this is the second of three belts.
    local humans_ok = human_ally_block(info) == 0
    if cand and won_pool and cand.can_carry and fail_ok and (mem_ok or urgent)
       and not guarded and humans_ok then
      tx(string.format("/info rvo %d %d %d %d", cand.pid, cand.mx, cand.my, math.floor(cand.score or 0)))
      state._repo_my_vote = { pid = cand.pid, mx = cand.mx, my = cand.my, score = cand.score,
                              open_tick = now, no = 0 }
    end
  end

  -- Age out the per-sender ballot cache so a past vote can't bleed into the next.
  local votes = state._repo_rx_votes
  if votes then
    for pn, v in pairs(votes) do
      if (now - v.tick) > (WINDOW + 60) then votes[pn] = nil end
    end
  end
end

-- -------------------------------------------------------------------------
-- M.draw(state, world, info, now) — the two visualizers.
-- -------------------------------------------------------------------------
local function count_set(s)
  if not s then return 0 end
  local n = 0
  for _ in pairs(s) do n = n + 1 end
  return n
end

local function set_keys(s)
  if not s then return "-" end
  local parts = {}
  for pn in pairs(s) do parts[#parts + 1] = "p" .. pn end
  return #parts > 0 and table.concat(parts, ",") or "-"
end

function M.draw(state, world, info, now)
  -- (a) reposition_scores: the local bot's top-N scored reposition candidates.
  if viz.is_on and viz.is_on("reposition_scores") and state._repo_topN then
    local twx, twy = info.tankx / 256.0, info.tanky / 256.0
    -- EVERY friendly pill is shown with its "initiate-a-vote-to-reposition"
    -- score. Only BACK-role pills are eligible to win; the first eligible one
    -- (lowest total) is the current BID (gold + line to us). Ineligible pills
    -- (front/aggro/util) are dimmed gray and tagged with their category.
    local shown_winner = false
    for _, c in ipairs(state._repo_topN) do
      local px, py = c.mx + 0.5, c.my + 0.5
      local is_winner = c.eligible and not shown_winner
      if is_winner then shown_winner = true end
      local r, g, b
      if not c.eligible   then r, g, b = 110, 110, 120
      elseif is_winner    then r, g, b = 255, 200, 60
      else                     r, g, b = 150, 170, 120 end
      if is_winner then viz.line("reposition_scores", twx, twy, px, py, r, g, b, 180) end
      -- %.0f, NOT %d: ineligible pills carry a large (or inf) sentinel score,
      -- and "%d" throws "no integer representation" on any float outside integer
      -- range. %.0f renders every finite/inf score without crashing the board.
    end
  end

  -- (a2) last "move-pill" position-scan tick (when the heavy scan last ran). A
  -- short flash on the compute tick itself so it's catchable live; otherwise the
  -- tick number + age so you can see the quiet-tick scheduler's cadence.
  if viz.is_on and viz.is_on("reposition_scores") and viz.hud_text then
    local sc = state._repo_score
    local tk = sc and sc.tick
    if tk then
      local age  = now - tk
      local just = age <= 3
      local r, g, b = just and 255 or 150, just and 240 or 200, just and 120 or 120
    end
  end

  -- (b) reposition_vote: live vote + latched result panel (HUD, top-left block).
  if viz.is_on and viz.is_on("reposition_vote") and viz.hud_text then
    local x, y, dy = 150, 300, 14
    local mv = state._repo_my_vote
    if mv then
      local left = math.max(0, (mv.open_tick + (C.REPOSITION_VOTE_WINDOW_TICKS or 10)) - now)
      y = y + dy
    elseif state._repo_active then
      y = y + dy
    end
    local panel = state._repo_vote_panel
    if panel and now <= panel.until_tick then
      local r, g, b = panel.pass and 120 or 255, panel.pass and 240 or 110, panel.pass and 120 or 110
      y = y + dy
      if panel.yes or panel.no then
      end
    end
  end

  -- (c) reposition_scores_hud: the same candidates as (a) but a top-5 TABLE.
  if viz.is_on and viz.is_on("reposition_scores_hud") and viz.hud_text then
    local x, y, dy = 470, 300, 14
    y = y + dy
    local tn = state._repo_topN
    if tn and #tn > 0 then
      local shown_winner = false
      for i = 1, #tn do
        local c = tn[i]
        y = y + dy
        local is_winner = c.eligible and not shown_winner
        if is_winner then shown_winner = true end
        local r, g, b
        if not c.eligible then r, g, b = 140, 140, 150
        elseif is_winner  then r, g, b = 255, 220, 120
        else                   r, g, b = 190, 210, 180 end
      end
    else
      y = y + dy
    end
  end

  -- (d) reposition_votes: per-ally vote table. One colored square per ally
  -- (yellow = vote ongoing, green = passed, red = failed) + a notes column
  -- (initiating / voted YES|NO; for THIS bot, the reason it voted that way).
  if viz.is_on and viz.is_on("reposition_votes") and viz.hud_text then
    local x, y, dy = 470, 470, 14
    local my, act, panel = state._repo_my_vote, state._repo_active, state._repo_vote_panel
    local status, vpid, initiator, yes_set, no_set
    if my then
      status, vpid, initiator, yes_set, no_set = "ongoing", my.pid, info.player_number, my.yes_set, my.no_set
    elseif act then
      status, vpid, initiator = "ongoing", act.pid, act.from
    elseif panel and now <= panel.until_tick then
      status, vpid, initiator, yes_set, no_set = (panel.pass and "pass" or "fail"), panel.pid, panel.from, panel.yes, panel.no
    else
      status = "idle"
    end
    -- Vote-lifecycle color: yellow ongoing -> green pass / red fail; gray idle.
    local sr, sg, sb = 130, 130, 130
    if     status == "ongoing" then sr, sg, sb = 235, 215, 80
    elseif status == "pass"    then sr, sg, sb = 100, 230, 120
    elseif status == "fail"    then sr, sg, sb = 235, 90, 90 end

    y = y + dy

    -- Participant set: self + every fresh ally, sorted.
    local self_pn = info.player_number
    local pns, seen = {}, {}
    if self_pn then pns[#pns + 1] = self_pn; seen[self_pn] = true end
    if ally_state.iter_active then
      for pn in ally_state.iter_active(now, 1750) do
        if not seen[pn] then pns[#pns + 1] = pn; seen[pn] = true end
      end
    end
    table.sort(pns)
    local rx = state._repo_rx_votes
    for _, pn in ipairs(pns) do
      local voted_yes, voted_no, reason
      if yes_set and yes_set[pn] then voted_yes = true end
      if no_set  and no_set[pn]  then voted_no  = true end
      if not voted_yes and not voted_no and rx and rx[pn] and rx[pn].pid == vpid then
        if rx[pn].no then voted_no = true else voted_yes = true end
      end
      if pn == self_pn and pn ~= initiator and state._repo_my_ballot
         and state._repo_my_ballot.pid == vpid then
        voted_yes = not state._repo_my_ballot.no
        voted_no  = state._repo_my_ballot.no
        reason    = state._repo_my_ballot.reason
      end
      local note
      if pn == initiator and vpid then
        note = "INITIATING vote"
        if pn == self_pn and my then note = note .. string.format("  (NO=%d YES=%d)", count_set(my.no_set), count_set(my.yes_set)) end
      elseif voted_no then
        note = "voted NO" .. (reason and ("  (" .. reason .. ")") or "")
      elseif voted_yes then
        note = "voted YES" .. (reason and ("  (" .. reason .. ")") or "")
      elseif status == "ongoing" then
        note = "(awaiting)"
      else
        note = "-"
      end
      -- col 1: the status-colored square; col 2: label + notes (neutral).
      y = y + dy
    end
  end
end

return M
