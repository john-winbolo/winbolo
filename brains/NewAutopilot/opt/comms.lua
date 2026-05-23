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
--
-- Reserved keys (rendered as overlay columns):
--   goal     - main goal name      (e.g. "attack_pill")
--   sub      - sub-state            (e.g. "build_walls", "engage")
--   target   - target id           (e.g. pill or base #)
--   cost     - final goal-selection cost
--   mx, my   - target tile coords (when applicable)
--   help     - 0/1/2 coordination hint (TBD)
--   lgm_st   - sender's LGM status: "in_tank" | "ground" | "dead"
--   lgmx, lgmy - sender's LGM tile (omitted when dead)
--   lgm_back - "1" for one broadcast when LGM transitions out of dead
--
-- Other keys (ppt=1, pill_hp=12, ...) are free-form per-goal context.
-- Commands from human allies use ! prefix (handled in init.lua via
-- the cmds module).
-- =========================================================================

local ally_state = require("ally_state")
local lgm_registry = require("lgm_registry")

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
    for k, v in state_payload:gmatch("(%w+)=(%S+)") do
      hash[k] = v
    end
    ally_state.set_info(sender, tick, hash)
    -- Mirror the LGM fields (lgm_st / lgmx / lgmy) onto the lgm_registry
    -- so it's the single source of truth across visual / event / broadcast
    -- channels.  lgm_back is an edge signal — when set, the sender's LGM
    -- just transitioned out of dead and we clear our dead bookkeeping.
    if hash.lgm_st then
      lgm_registry.update_from_ally_state(sender, hash, tick)
    end
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

return M
