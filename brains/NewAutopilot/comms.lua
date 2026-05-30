-- =========================================================================
-- NewAutopilot/comms.lua — brain-to-brain coordination
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

local M = {}

-- -------------------------------------------------------------------------
-- M.process_message(sender, text, tick)
-- Parse one incoming message.  Only /info state is recognized;
-- anything else (human chat, ! commands, unknown verbs) is silently
-- ignored so adding new verbs later doesn't break older brains.
-- -------------------------------------------------------------------------
function M.process_message(sender, text, tick)
  if not text then return end

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
