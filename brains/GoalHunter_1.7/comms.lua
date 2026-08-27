-- =========================================================================
-- GoalHunter/comms.lua — brain-to-brain coordination
--
-- Single protocol: /info state — the full per-bot state slate.  Each
-- bot broadcasts on goal change + a 30 s heartbeat; receivers store
-- the latest state per sender in ally_state and surface it on the
-- right-middle overlay table.  No legacy aIndy verbs (pill/base
-- claims, mytype/goodbye); de-conflict logic reads goal/target/cost
-- straight off the ally_state slate.
--
-- Protocol:
--   /info state k1=v1 k2=v2 ...   — share per-bot state as k=v pairs.
--                                   Receiver clears every key not
--                                   present in this message so stale
--                                   fields self-evict.
--   /info extra k1=v1 k2=v2 ...   — supplementary fields that don't
--                                   fit the 128-byte /info state
--                                   budget.  Receiver MERGES into the
--                                   sender's slot without clearing
--                                   other keys.  Use for low-churn
--                                   payloads (e.g. attack_pill setup
--                                   + standoff coords) that would
--                                   otherwise truncate the main state
--                                   message on the wire.
--
-- Reserved keys (rendered as overlay columns):
--   goal     - main goal name      (e.g. "attack_pill")
--   sub      - sub-state            (e.g. "build_walls", "engage")
--   target   - target id           (e.g. pill or base #)
--   cost     - final goal-selection cost
--   mx, my   - target tile coords (when applicable)
--   help     - 0/1/2 coordination hint (TBD)
--
-- Other keys (ppt=1, pill_hp=12, ...) are free-form per-goal context.
-- Commands from human allies use ! prefix (handled in init.lua via
-- the cmds module).
-- =========================================================================

local ally_state = require("ally_state")
local lgm_registry = require("lgm_registry")
local print2 = require("print2")

local M = {}

-- Batch separator. Several /info messages are packed into the single outbound
-- buffer per tick (init.lua's send section) joined by this byte, then split back
-- apart here. \x1e (ASCII record separator) is non-null (survives the strlen on
-- the internal delivery path, bot_manager.c) and never appears in any /info
-- payload (all alphanumeric + , : . [ ] = and spaces). Keep these in sync.
M.MSG_SEP = "\x1e"

-- Parse one "/info kw" record token into a record table, or nil if malformed.
-- base: b<id>:<mx>:<my>:<cls>:<hp>:<tick>
-- pill: p<id>:<mx>:<my>:<cls>:<intank>:<hp>:<tick>
-- hp is the object's real health at broadcast time (KW used to carry none, so the
-- receiver had to GUESS — which invented full-health phantoms for dead pills).
local function parse_kw_rec(tok)
  local kind = tok:sub(1, 1)
  local f = {}
  for n in tok:sub(2):gmatch("[^:]+") do f[#f + 1] = n end
  if kind == "b" and #f >= 6 then
    return { kind = "b", id = tonumber(f[1]), mx = tonumber(f[2]),
             my = tonumber(f[3]), cls = f[4], hp = tonumber(f[5]),
             tick = tonumber(f[6]) }
  elseif kind == "p" and #f >= 7 then
    return { kind = "p", id = tonumber(f[1]), mx = tonumber(f[2]),
             my = tonumber(f[3]), cls = f[4], intank = tonumber(f[5]),
             hp = tonumber(f[6]), tick = tonumber(f[7]) }
  end
  return nil
end

-- -------------------------------------------------------------------------
-- M.process_message(sender, text, tick)
-- Parse one incoming message.  Only /info state is recognized;
-- anything else (human chat, ! commands, unknown verbs) is silently
-- ignored so adding new verbs later doesn't break older brains.
-- -------------------------------------------------------------------------
function M.process_message(sender, text, tick, state)
  if not text then return end

  -- Batched send: the sender packed several /info messages into one buffer
  -- (separated by MSG_SEP). Split and process each segment as its own message.
  -- A lone (unbatched) message has no separator and falls straight through.
  if text:find(M.MSG_SEP, 1, true) then
    local pos = 1
    while true do
      local s = text:find(M.MSG_SEP, pos, true)
      local seg = s and text:sub(pos, s - 1) or text:sub(pos)
      if seg ~= "" then M.process_message(sender, seg, tick, state) end
      if not s then break end
      pos = s + #M.MSG_SEP
    end
    return
  end

  -- One-shot "LGM back": the sender's killed LGM has respawned.  Clear
  -- our dead-cooldown bookkeeping for them immediately (the engine fires
  -- no "revived" event, so this is the only signal we get).
  if text == "/info lgmback" then
    lgm_registry.note_back(sender, tick)
    return
  end

  -- Blitz-call registry (one-shot events). A commander broadcasts "open" once
  -- when it starts a help-wanted blitz on a pill, "close" once when it ends.
  -- "query" is a discovery request from a (re)spawned/joining bot — holders of
  -- an open call re-announce. We remember open calls in state.blitz_calls so
  -- the proactive-join scan can read them without continuous broadcast.
  local bco_pill = text:match("^/info bco (%d+)$")
  if bco_pill then
    if state then
      state.blitz_calls = state.blitz_calls or {}
      local pillnum = tonumber(bco_pill)
      local prev = state.blitz_calls[sender]
      -- New (or retargeted) open call → request an immediate replan so we can
      -- respond fast. It doesn't commit us: the join discount just lets the pill
      -- compete against our other goals this tick instead of waiting for the timer.
      if not prev or prev.pill ~= pillnum then state._blitz_new_call = true end
      -- Preserve the FIRST-SEEN tick of this call across re-announces (bcq
      -- responses re-send the same bco) so "who started the take first" stays
      -- accurate — the commander-deferral / first-to-the-take rule reads it.
      local first_tick = (prev and prev.pill == pillnum and prev.tick) or tick
      state.blitz_calls[sender] = { pill = pillnum, tick = first_tick }
    end
    print2(string.format("BLITZ_RX bco from p%s pill=%s t=%d", tostring(sender), bco_pill, tick))
    return
  end
  if text == "/info bcc" then
    if state and state.blitz_calls then state.blitz_calls[sender] = nil end
    print2(string.format("BLITZ_RX bcc from p%s t=%d", tostring(sender), tick))
    return
  end
  if text == "/info bcq" then
    if state then state._blitz_rebroadcast = true end  -- re-announce our open call (if any)
    print2(string.format("BLITZ_RX bcq from p%s t=%d", tostring(sender), tick))
    return
  end

  -- Known-world digest: ally-relayed base/pill allegiance + location records.
  -- Stashed on state._kw_inbox; init.lua folds them into world.* via
  -- W.sync_ally_world (newest-tick wins). See world.lua "Known-world sharing".
  local kw_payload = text:match("^/info kw (.*)$")
  if kw_payload then
    if state then
      state._kw_inbox = state._kw_inbox or {}
      local inbox = state._kw_inbox
      local n = 0
      for tok in kw_payload:gmatch("[^,]+") do
        local rec = parse_kw_rec(tok)
        if rec then rec.from = sender; inbox[#inbox + 1] = rec; n = n + 1 end
      end
      print2(string.format("KW_RX t=%d from=p%s n=%d recs=%s", tick, tostring(sender), n, kw_payload))
    end
    return
  end

  -- Resync query (sent on (re)spawn): re-broadcast our known world once.
  if text == "/info kwq" then
    if state then state._kw_resync_req = true end
    print2(string.format("KW_RX kwq from p%s t=%d", tostring(sender), tick))
    return
  end

  -- Commander->soldier blitz handshake on their OWN short verbs (NOT bundled into
  -- /info state, which exceeds the 128-byte chat cap once brj/bes/pblk pile on and
  -- gets dropped — the reject/accept then never lands). Merged into the sender's
  -- slot via set_handshake (protected from /info state's set_info wipe). Payload
  -- absent = clear. brj = "pn:[fx,fy];..." rejects; bac = "pn,pn" accepts.
  if text:match("^/info brj") then
    if state then ally_state.set_handshake(sender, tick, "brj", text:match("^/info brj (.+)$")) end
    print2(string.format("BLITZ_RX brj from p%s payload=%s t=%d", tostring(sender), tostring(text:match("^/info brj (.+)$")), tick))
    return
  end
  if text:match("^/info bac") then
    if state then ally_state.set_handshake(sender, tick, "bac", text:match("^/info bac (.+)$")) end
    print2(string.format("BLITZ_RX bac from p%s payload=%s t=%d", tostring(sender), tostring(text:match("^/info bac (.+)$")), tick))
    return
  end
  -- Blocker tiles on their own verb (variable-length tile list, split off the
  -- state slate to keep /info state under the 128-byte cap). Merged via
  -- set_handshake; empty payload clears.
  if text:match("^/info pblk") then
    if state then ally_state.set_handshake(sender, tick, "pblk", text:match("^/info pblk (.+)$")) end
    print2(string.format("BLITZ_RX pblk from p%s payload=%s t=%d", tostring(sender), tostring(text:match("^/info pblk (.+)$")), tick))
    return
  end
  -- Shield WALL tiles a commander is building (build_walls only). Blitz soldiers
  -- route their engage spot + aim point around these. Merged via set_handshake;
  -- empty payload clears (commander left build_walls).
  if text:match("^/info bwl") then
    if state then ally_state.set_handshake(sender, tick, "bwl", text:match("^/info bwl (.+)$")) end
    print2(string.format("BLITZ_RX bwl from p%s payload=%s t=%d", tostring(sender), tostring(text:match("^/info bwl (.+)$")), tick))
    return
  end

  -- ── Reposition VOTE protocol (one-shot events, stashed on state) ──────────
  -- rvo: a bot OPENS a vote to move pill <pid> at (<mx>,<my>) with desirability
  --      <score> (lower = more worth moving). Everyone records it as the active
  --      vote and (if not us) queues a vote response.
  -- rvy/rvn: an ally's YES / NO vote on <pid>. Only NO actually blocks; YES is
  --      sent too so the vote visualizer can show explicit support.
  -- rvr: the initiator's RESULT for <pid> (<pass> 1/0). On pass, everyone stamps
  --      "a reposition just happened" for the 120s recent-memory NO rule.
  -- rvx: a bot CONSUMED its approval (reposition executed) for <pid> at <exec_tick>.
  --      One-shot, batcher-reliable — every ally stamps the SAME recent-memory tick
  --      instead of hoping to catch the mover's repos=1 heartbeat, so self-gating
  --      before OPEN matches what voters decide (kills doomed proposals).
  do
    local pid, mx, my, score = text:match("^/info rvo (%-?%d+) (%-?%d+) (%-?%d+) (%-?%d+)$")
    if pid then
      if state then
        -- Queue proposals (don't last-write-wins): two allies can open a vote
        -- in the same tick; reposition_vote drains the whole queue.
        state._repo_rx_open = state._repo_rx_open or {}
        state._repo_rx_open[#state._repo_rx_open + 1] = { pid = tonumber(pid), mx = tonumber(mx), my = tonumber(my),
                                score = tonumber(score), from = sender, tick = tick }
      end
      print2(string.format("REPO_RX rvo from p%s pill=%s @(%s,%s) score=%s t=%d", tostring(sender), pid, mx, my, score, tick))
      return
    end
  end
  local rvy_pid = text:match("^/info rvy (%-?%d+)$")
  if rvy_pid then
    if state then
      state._repo_rx_votes = state._repo_rx_votes or {}
      state._repo_rx_votes[sender] = { pid = tonumber(rvy_pid), no = false, tick = tick }
    end
    print2(string.format("REPO_RX rvy from p%s pill=%s t=%d", tostring(sender), rvy_pid, tick))
    return
  end
  local rvn_pid = text:match("^/info rvn (%-?%d+)$")
  if rvn_pid then
    if state then
      state._repo_rx_votes = state._repo_rx_votes or {}
      state._repo_rx_votes[sender] = { pid = tonumber(rvn_pid), no = true, tick = tick }
    end
    print2(string.format("REPO_RX rvn from p%s pill=%s t=%d", tostring(sender), rvn_pid, tick))
    return
  end
  do
    local pid, pass = text:match("^/info rvr (%-?%d+) (%d)$")
    if pid then
      if state then
        state._repo_rx_result = { pid = tonumber(pid), pass = (pass == "1"), from = sender, tick = tick }
      end
      print2(string.format("REPO_RX rvr from p%s pill=%s pass=%s t=%d", tostring(sender), pid, pass, tick))
      return
    end
  end
  do
    local pid, exec_tick = text:match("^/info rvx (%-?%d+) (%-?%d+)$")
    if pid then
      if state then
        state._repo_rx_exec = { pid = tonumber(pid), exec_tick = tonumber(exec_tick), from = sender, tick = tick }
      end
      print2(string.format("REPO_RX rvx from p%s pill=%s exec_t=%s t=%d", tostring(sender), pid, exec_tick, tick))
      return
    end
  end

  -- ── attack_pill steal negotiation (one-shot events, stashed on state) ────
  -- stq: challenger asks <to_pn> "I want to steal pill <pid> from you, my
  --      score is <cost>". Only the addressed holder responds.
  -- sta: holder ACCEPTS — re-evaluated own score is <cost>; it has already
  --      yielded the goal. Challenger may now take the pill.
  -- str: holder REJECTS — re-evaluated own score is <cost> (fresher than its
  --      periodic broadcast). Challenger backs off for STEAL_REQ_COOLDOWN.
  -- Addressee filtering happens in init.lua (comms doesn't know our pn).
  do
    local pid, topn, cost = text:match("^/info stq (%d+) (%d+) (%-?%d+)$")
    if pid then
      if state then
        state._steal_reqs_in = state._steal_reqs_in or {}
        state._steal_reqs_in[#state._steal_reqs_in + 1] = {
          pid = tonumber(pid), to = tonumber(topn), from = sender,
          cost = tonumber(cost), tick = tick }
      end
      print2(string.format("STEAL_RX stq from p%s pill=%s to=p%s cost=%s t=%d",
                           tostring(sender), pid, topn, cost, tick))
      return
    end
  end
  do
    local verb, pid, topn, cost = text:match("^/info (st[ar]) (%d+) (%d+) (%-?%d+)$")
    if verb then
      if state then
        state._steal_replies_in = state._steal_replies_in or {}
        state._steal_replies_in[#state._steal_replies_in + 1] = {
          pid = tonumber(pid), to = tonumber(topn), from = sender,
          cost = tonumber(cost), accept = (verb == "sta"), tick = tick }
      end
      print2(string.format("STEAL_RX %s from p%s pill=%s to=p%s cost=%s t=%d",
                           verb, tostring(sender), pid, topn, cost, tick))
      return
    end
  end

  local state_payload = text:match("^/info state(.*)$")
  if state_payload then
    local hash = {}
    for k, v in state_payload:gmatch("([%w_]+)=(%S+)") do
      hash[k] = v
    end
    ally_state.set_info(sender, tick, hash)
    return
  end

  local extra_payload = text:match("^/info extra(.*)$")
  if extra_payload then
    local hash = {}
    for k, v in extra_payload:gmatch("([%w_]+)=(%S+)") do
      hash[k] = v
    end
    ally_state.merge_info(sender, tick, hash)
    return
  end
end

-- -------------------------------------------------------------------------
-- M.format_state(info_hash)
-- Build a "/info state k1=v1 k2=v2 ..." message from a hash of
-- fields.  Values are stringified.  Keys with empty-string or nil
-- values are omitted (receiver treats absence as "clear this key").
-- Caller is responsible for keeping the total under
-- PACKET_MAX_CHAT_MESSAGE (128 bytes on the wire).
-- -------------------------------------------------------------------------
function M.format_state(info_hash)
  local parts = { "/info state" }
  if info_hash ~= nil then
    for k, v in pairs(info_hash) do
      if k ~= nil and k ~= "" and v ~= nil and v ~= "" then
        parts[#parts + 1] = k .. "=" .. tostring(v)
      end
    end
  end
  return table.concat(parts, " ")
end

-- Same shape as format_state but prefixed "/info extra". Caller is
-- still responsible for keeping the result under
-- PACKET_MAX_CHAT_MESSAGE (128 bytes on the wire). Receiver MERGES
-- the keys into the sender's ally_state slot, so passing a subset
-- here doesn't clear keys already set by an earlier /info state.
function M.format_extra(info_hash)
  local parts = { "/info extra" }
  if info_hash ~= nil then
    for k, v in pairs(info_hash) do
      if k ~= nil and k ~= "" and v ~= nil and v ~= "" then
        parts[#parts + 1] = k .. "=" .. tostring(v)
      end
    end
  end
  return table.concat(parts, " ")
end

return M
