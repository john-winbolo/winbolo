-- =========================================================================
-- NewAutopilot/comms.lua — brain-to-brain coordination
--
-- Based on aIndy 3.1's BBMPL messaging system. Coordinates with allied
-- brains to prevent multiple bots targeting the same pill/base.
--
-- Protocol:
--   /nap pt <pill_id> <cost>   — I'm targeting pill <id> at cost <cost>
--   /nap gbt <base_id> <cost>  — I'm grabbing base <id> at cost <cost>
--   /nap mytype NewAutopilot   — identify as NewAutopilot brain
--   /nap goodbye               — shutting down
--
-- Informational prefixed with /nap so other brains can filter.
-- Commands from human allies use ! prefix (handled in init.lua).
-- =========================================================================

local C = require("constants")
local U = require("util")

local M = {}

-- State: track what allied brains have claimed
local ally_pill_claims = {}   -- [pill_id] = { player, cost, tick }
local ally_base_claims = {}   -- [base_id] = { player, cost, tick }
local CLAIM_EXPIRE_TICKS = 200  -- claims expire if not refreshed

-- -------------------------------------------------------------------------
-- M.reset()
-- -------------------------------------------------------------------------
function M.reset()
  ally_pill_claims = {}
  ally_base_claims = {}
end

-- -------------------------------------------------------------------------
-- M.process_message(sender, text, tick)
-- Parse incoming messages from allies.
-- -------------------------------------------------------------------------
function M.process_message(sender, text, tick)
  if not text then return end

  -- /nap pt <pill_id> <cost> — ally targeting pill
  local pt_id, pt_cost = text:match("^/nap pt (%d+) ([%d%.]+)")
  if pt_id then
    ally_pill_claims[tonumber(pt_id)] = {
      player = sender, cost = tonumber(pt_cost), tick = tick
    }
    return
  end

  -- /nap gbt <base_id> <cost> — ally targeting base
  local gbt_id, gbt_cost = text:match("^/nap gbt (%d+) ([%d%.]+)")
  if gbt_id then
    ally_base_claims[tonumber(gbt_id)] = {
      player = sender, cost = tonumber(gbt_cost), tick = tick
    }
    return
  end
end

-- -------------------------------------------------------------------------
-- M.expire_claims(tick)
-- Remove stale claims.
-- -------------------------------------------------------------------------
function M.expire_claims(tick)
  for k, v in pairs(ally_pill_claims) do
    if tick - v.tick > CLAIM_EXPIRE_TICKS then
      ally_pill_claims[k] = nil
    end
  end
  for k, v in pairs(ally_base_claims) do
    if tick - v.tick > CLAIM_EXPIRE_TICKS then
      ally_base_claims[k] = nil
    end
  end
end

-- -------------------------------------------------------------------------
-- M.pill_claimed_by_ally(pill_id, my_cost)
-- Returns true if another ally has claimed this pill at a lower cost.
-- -------------------------------------------------------------------------
function M.pill_claimed_by_ally(pill_id, my_cost)
  local claim = ally_pill_claims[pill_id]
  if claim and claim.cost < my_cost then
    return true, claim.player, claim.cost
  end
  return false
end

-- -------------------------------------------------------------------------
-- M.base_claimed_by_ally(base_id, my_cost)
-- Returns true if another ally has claimed this base at a lower cost.
-- -------------------------------------------------------------------------
function M.base_claimed_by_ally(base_id, my_cost)
  local claim = ally_base_claims[base_id]
  if claim and claim.cost < my_cost then
    return true, claim.player, claim.cost
  end
  return false
end

-- -------------------------------------------------------------------------
-- M.format_pill_claim(pill_id, cost)
-- Format a pill claim message to broadcast.
-- -------------------------------------------------------------------------
function M.format_pill_claim(pill_id, cost)
  return string.format("/nap pt %d %.0f", pill_id, cost)
end

-- -------------------------------------------------------------------------
-- M.format_base_claim(base_id, cost)
-- Format a base claim message to broadcast.
-- -------------------------------------------------------------------------
function M.format_base_claim(base_id, cost)
  return string.format("/nap gbt %d %.0f", base_id, cost)
end

-- -------------------------------------------------------------------------
-- M.format_identify()
-- -------------------------------------------------------------------------
function M.format_identify()
  return "/nap mytype NewAutopilot"
end

-- -------------------------------------------------------------------------
-- M.format_goodbye()
-- -------------------------------------------------------------------------
function M.format_goodbye()
  return "/nap goodbye"
end

return M
