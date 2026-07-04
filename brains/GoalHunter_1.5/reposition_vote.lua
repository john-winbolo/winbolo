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
--   * the pill is in use as a take blocker (_in_use)
--   * they remember a reposition EXECUTING within the last ~30s (one move
--     per RECENT_MEMORY window; urgent negative-score proposals exempt);
--     any FAILED vote additionally holds all proposals ~30s (FAIL_COOLDOWN)
--   * an enemy tank is within 15 tiles AND nothing else covers the pill
--   * the pill IS covered by >=1 pill but an enemy tank is within 10 tiles
--   * they have a strictly better (lower-cost) reposition candidate of their own
--
-- All wire traffic rides init.lua's comms batcher (state._repo_outbox drained
-- there). Inbound verbs are parsed in comms.lua onto state._repo_rx_*.
-- =============================================================================

local C          = require("constants")
local viz        = require("viz")
local print2     = require("print2")
local ally_state = require("ally_state")

local M = {}

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

-- Nearest enemy-tank distance in tiles to (mx,my); math.huge if none perceived.
local function nearest_enemy_tank(state, mx, my)
  local best = math.huge
  local ets = state.perc and state.perc.enemy_tanks
  if ets then
    for _, et in ipairs(ets) do
      if et.mx and et.my then
        local d = tdist(mx, my, et.mx, et.my)
        if d < best then best = d end
      end
    end
  end
  return best
end

-- Decide THIS bot's vote on a proposed reposition. prop = {pid,mx,my,score}.
-- Returns is_no(bool), reason(string).
local function evaluate_vote(state, world, info, now, prop)
  local p  = world.pills[prop.pid]
  local mx = (p and p.mx) or prop.mx
  local my = (p and p.my) or prop.my

  if p and p._in_use then return true, "blocker" end

  -- Pacing NO — but never against an URGENT proposal: a negative score means
  -- the pill's position is actively harmful (deep surplus / redundancy /
  -- orphaned). The recent-memory pacing exists to stop marginal churn, not
  -- to ration urgent corrections — one move per 2 minutes team-wide is far
  -- too slow to fix a badly lopsided back line.
  if (prop.score or 0) >= (C.REPOSITION_URGENT_SCORE or 0)
     and state._repo_last_seen_tick
     and (now - state._repo_last_seen_tick) < (C.REPOSITION_VOTE_RECENT_MEMORY_TICKS or 6000) then
    return true, "recent_repo"
  end

  local cover = covering_pills(world, prop.pid, mx, my)
  local enemy = nearest_enemy_tank(state, mx, my)

  if cover == 0 and enemy <= (C.REPOSITION_VOTE_ENEMY_NEAR_TILES or 15) then
    return true, "enemy_uncovered"
  end
  if cover >= 1 and enemy <= (C.REPOSITION_VOTE_TANK_COVER_TILES or 10) then
    return true, "tank_near_covered"
  end

  local mine = state._repo_candidate
  if mine and mine.pid ~= prop.pid and mine.can_carry
     and (mine.score or math.huge) < (prop.score or math.huge) then
    return true, "better_candidate"
  end

  return false, "ok"
end

-- -------------------------------------------------------------------------
-- M.update(state, world, info, now)
-- Drive the vote state machine for one tick. Queues outbound verbs on
-- state._repo_outbox (init.lua's send section drains them via try_send).
-- -------------------------------------------------------------------------
function M.update(state, world, info, now)
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
    print2(string.format("REPO_VOTE_TBL t=%d ev=result pid=%d from=p%s pass=%s",
           now, res.pid, tostring(res.from), tostring(res.pass)))
    state._repo_active = nil
    state._repo_vote_panel = { pid = res.pid, from = res.from, pass = res.pass,
                               until_tick = now + (C.REPOSITION_VOTE_RESULT_LATCH_TICKS or 120) }
    state._repo_rx_result = nil
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
  if ally_state.iter_active then
    for apn, slot in ally_state.iter_active(now, 1750) do
      if apn ~= self_pn and slot.info and slot.info.repos == "1" then
        state._repo_last_seen_tick = now
        guard_mark(tonumber(slot.info.mx), tonumber(slot.info.my))
      end
    end
  end
  -- Our own reposition target: the committed goal's tile, and the approved
  -- pill's tile while the approval is held.
  if state.goal and state.goal.kind == "capture_pill" and state.goal.reposition then
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
          print2(string.format("REPO_VOTE t=%d cancel own open on pill#%d — p%s opened same pill and wins tiebreak",
                 now, op.pid, tostring(op.from)))
          state._repo_my_vote = nil
        end
        local is_no, reason = evaluate_vote(state, world, info, now, op)
        tx(is_no and ("/info rvn " .. op.pid) or ("/info rvy " .. op.pid))
        -- Remember our own ballot + WHY (we never receive our own vote back, so the
        -- votes visualizer reads our reason from here).
        state._repo_my_ballot = { pid = op.pid, no = is_no, reason = reason, tick = now }
        print2(string.format("REPO_VOTE t=%d cast %s on pill#%d (from p%s) reason=%s",
               now, is_no and "NO" or "YES", op.pid, tostring(op.from), reason))
        print2(string.format("REPO_VOTE_TBL t=%d ev=ballot pid=%d from=p%s by=p%s vote=%s reason=%s",
               now, op.pid, tostring(op.from), tostring(self_pn),
               is_no and "NO" or "YES", reason))
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
    if (now - mv.open_tick) >= WINDOW then
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
      print2(string.format("REPO_VOTE_TBL t=%d ev=resolve pid=%d from=p%s pass=%s no=%d score=%d",
             now, mv.pid, tostring(self_pn), tostring(pass), no_n, math.floor(mv.score or 0)))
      state._repo_vote_panel = { pid = mv.pid, from = self_pn, pass = pass,
                                 yes = mv.yes_set, no = mv.no_set,
                                 until_tick = now + (C.REPOSITION_VOTE_RESULT_LATCH_TICKS or 120) }
      print2(string.format("REPO_VOTE t=%d RESOLVE pill#%d pass=%s no=%d",
             now, mv.pid, tostring(pass), no_n))
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
      print2(string.format("REPO_VOTE t=%d approval pill#%d consumed (taken/built) — cleared, recent-memory starts", now, state._repo_approved_pid))
      print2(string.format("REPO_VOTE_TBL t=%d ev=consumed pid=%d by=p%s", now, state._repo_approved_pid, tostring(info.player_number)))
      state._repo_last_seen_tick      = now
      state.last_team_reposition_tick = now   -- feed the existing team time-discount
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
          print2(string.format("REPO_VOTE t=%d approval pill#%d expired unconsumed (used=%d wall=%d)",
                 now, state._repo_approved_pid, state._repo_approved_used or 0, wall))
          print2(string.format("REPO_VOTE_TBL t=%d ev=expired pid=%d by=p%s used=%d wall=%d",
                 now, state._repo_approved_pid, tostring(info.player_number),
                 state._repo_approved_used or 0, wall))
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
    if cand and cand.can_carry and fail_ok and (mem_ok or urgent) and not guarded then
      tx(string.format("/info rvo %d %d %d %d", cand.pid, cand.mx, cand.my, math.floor(cand.score or 0)))
      state._repo_my_vote = { pid = cand.pid, mx = cand.mx, my = cand.my, score = cand.score,
                              open_tick = now, no = 0 }
      print2(string.format("REPO_VOTE t=%d OPEN pill#%d @(%d,%d) score=%d",
             now, cand.pid, cand.mx, cand.my, math.floor(cand.score or 0)))
      print2(string.format("REPO_VOTE_TBL t=%d ev=open pid=%d from=p%s mx=%d my=%d score=%d urgent=%s",
             now, cand.pid, tostring(self_pn), cand.mx, cand.my,
             math.floor(cand.score or 0), tostring(urgent or false)))
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
    for i, c in ipairs(state._repo_topN) do
      local px, py = c.mx + 0.5, c.my + 0.5
      local top = (i == 1)
      local r, g, b = top and 255 or 150, top and 200 or 150, top and 60 or 120
      viz.line("reposition_scores", twx, twy, px, py, r, g, b, top and 180 or 120)
      viz.circle("reposition_scores", px, py, 0.45, r, g, b, 210)
      viz.text("reposition_scores", px, py - 0.7,
               string.format("#%d pill%s s=%d%s", i, tostring(c.pid),
                             math.floor(c.score or 0), c.can_carry and "" or " (x)"),
               "center", r, g, b, 235, 0.35)
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
      viz.hud_text("reposition_scores", 150, 286,
        string.format("move-pill scan: tick %d, %d ticks ago, %.1fs ago%s",
                      tk, age, age / 50.0, just and "  <- COMPUTED" or ""),
        "topleft", r, g, b, 255)
    end
  end

  -- (b) reposition_vote: live vote + latched result panel (HUD, top-left block).
  if viz.is_on and viz.is_on("reposition_vote") and viz.hud_text then
    local x, y, dy = 150, 300, 14
    local mv = state._repo_my_vote
    if mv then
      local left = math.max(0, (mv.open_tick + (C.REPOSITION_VOTE_WINDOW_TICKS or 10)) - now)
      viz.hud_text("reposition_vote", x, y,
        string.format("VOTE(me) pill#%d  %dt left  NO=%d YES=%d", mv.pid, left,
                      count_set(mv.no_set), count_set(mv.yes_set)),
        "topleft", 120, 220, 255, 255)
      y = y + dy
    elseif state._repo_active then
      viz.hud_text("reposition_vote", x, y,
        string.format("VOTE(p%s) pill#%d  casting ballot...",
                      tostring(state._repo_active.from), state._repo_active.pid),
        "topleft", 200, 220, 160, 255)
      y = y + dy
    end
    local panel = state._repo_vote_panel
    if panel and now <= panel.until_tick then
      local r, g, b = panel.pass and 120 or 255, panel.pass and 240 or 110, panel.pass and 120 or 110
      viz.hud_text("reposition_vote", x, y,
        string.format("RESULT pill#%d by p%s: %s", panel.pid, tostring(panel.from),
                      panel.pass and "PASS" or "FAIL"),
        "topleft", r, g, b, 255)
      y = y + dy
      if panel.yes or panel.no then
        viz.hud_text("reposition_vote", x, y,
          string.format("  yes=%s  no=%s", set_keys(panel.yes), set_keys(panel.no)),
          "topleft", r, g, b, 230)
      end
    end
  end

  -- (c) reposition_scores_hud: the same candidates as (a) but a top-5 TABLE.
  if viz.is_on and viz.is_on("reposition_scores_hud") and viz.hud_text then
    local x, y, dy = 470, 300, 14
    viz.hud_text("reposition_scores_hud", x, y, "move-pill candidates (top 5)", "topleft", 200, 200, 200, 225)
    y = y + dy
    viz.hud_text("reposition_scores_hud", x, y,
      string.format("%-2s %-5s %-7s %-10s %-3s", "#", "pill", "score", "tile", "go"),
      "topleft", 175, 175, 175, 215)
    local tn = state._repo_topN
    if tn and #tn > 0 then
      for i = 1, math.min(5, #tn) do
        local c = tn[i]
        y = y + dy
        local top = (i == 1)
        viz.hud_text("reposition_scores_hud", x, y,
          string.format("%-2d p%-4s %-7d (%d,%d) %-3s", i, tostring(c.pid),
                        math.floor(c.score or 0), c.mx, c.my, c.can_carry and "yes" or "-"),
          "topleft", top and 255 or 200, top and 220 or 200, top and 120 or 200, 235)
      end
    else
      y = y + dy
      viz.hud_text("reposition_scores_hud", x, y, "(none — no surplus / too few built)",
        "topleft", 150, 150, 150, 200)
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

    viz.hud_text("reposition_votes", x, y,
      vpid and string.format("REPOSITION VOTE pill#%d  [%s]", vpid, string.upper(status))
            or "REPOSITION VOTE  (idle)",
      "topleft", sr, sg, sb, 245)
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
      viz.hud_text("reposition_votes", x, y, "[#]", "topleft", sr, sg, sb, 245)
      viz.hud_text("reposition_votes", x + 28, y,
        string.format("%-3s %s", (pn == self_pn) and "me" or ("p" .. pn), note),
        "topleft", 215, 215, 215, 235)
      y = y + dy
    end
  end
end

return M
