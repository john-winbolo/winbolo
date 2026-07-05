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
--   * they remember ANY reposition within the last ~120s (one move at a time)
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
  if (info.carried_pills or 0) >= 1 then return false end
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

  if state._repo_last_seen_tick
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
  local res = state._repo_rx_result
  if res then
    if res.pass then state._repo_last_seen_tick = now end
    state._repo_active = nil
    state._repo_vote_panel = { pid = res.pid, from = res.from, pass = res.pass,
                               until_tick = now + (C.REPOSITION_VOTE_RESULT_LATCH_TICKS or 120) }
    state._repo_rx_result = nil
  end

  -- 2. Inbound PROPOSALS: record the active vote, cast our ballot once each.
  --    The queue may hold several proposals opened in the same tick — handle
  --    every one so no ally's vote is silently dropped.
  local opq = state._repo_rx_open
  if opq then
    for i = 1, #opq do
      local op = opq[i]
      state._repo_active = { pid = op.pid, from = op.from, tick = op.tick }
      if op.from ~= self_pn then
        local is_no, reason = evaluate_vote(state, world, info, now, op)
        tx(is_no and ("/info rvn " .. op.pid) or ("/info rvy " .. op.pid))
        -- Remember our own ballot + WHY (we never receive our own vote back, so the
        -- votes visualizer reads our reason from here).
        state._repo_my_ballot = { pid = op.pid, no = is_no, reason = reason, tick = now }
        print2(string.format("REPO_VOTE t=%d cast %s on pill#%d (from p%s) reason=%s",
               now, is_no and "NO" or "YES", op.pid, tostring(op.from), reason))
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
        state._repo_last_seen_tick  = now
        state.last_team_reposition_tick = now   -- feed the existing team time-discount
      end
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
  if state._repo_approved_pid then
    local ap = world.pills and world.pills[state._repo_approved_pid]
    local consumed = (not ap) or (ap.health or 0) <= 0 or ap.in_tank
    if consumed then
      -- The pill was actually taken (shot to 0 / picked up / re-dropped) — the
      -- move is underway and held by the reposition lock from here. Drop the
      -- approval so we don't re-target this same pill again inside the window
      -- (e.g. reposition it a second time right after it was just re-built).
      print2(string.format("REPO_VOTE t=%d approval pill#%d consumed (taken/built) — cleared", now, state._repo_approved_pid))
      state._repo_approved_pid = nil
    elseif (now - (state._repo_approved_tick or now)) > (C.REPOSITION_VOTE_APPROVAL_TTL or 1500) then
      local committed = state.goal and state.goal.kind == "capture_pill"
                        and state.goal.reposition and state.goal.target_id == state._repo_approved_pid
      if not committed then
        print2(string.format("REPO_VOTE t=%d approval pill#%d expired unconsumed", now, state._repo_approved_pid))
      end
      state._repo_approved_pid = nil
    end
  end

  -- 4. Maybe OPEN a new vote.
  if not state._repo_my_vote and not state._repo_active and not state._repo_approved_pid then
    local cand   = state._repo_candidate
    local cd_ok  = not state._repo_initiate_tick
                   or (now - state._repo_initiate_tick) >= (C.REPOSITION_VOTE_INITIATE_COOLDOWN or 3000)
    local mem_ok = not state._repo_last_seen_tick
                   or (now - state._repo_last_seen_tick) >= (C.REPOSITION_VOTE_RECENT_MEMORY_TICKS or 6000)
    if cand and cand.can_carry and cd_ok and mem_ok then
      tx(string.format("/info rvo %d %d %d %d", cand.pid, cand.mx, cand.my, math.floor(cand.score or 0)))
      state._repo_my_vote = { pid = cand.pid, mx = cand.mx, my = cand.my, score = cand.score,
                              open_tick = now, no = 0 }
      state._repo_initiate_tick = now
      print2(string.format("REPO_VOTE t=%d OPEN pill#%d @(%d,%d) score=%d",
             now, cand.pid, cand.mx, cand.my, math.floor(cand.score or 0)))
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
