-- =========================================================================
-- GoalHunter/pt2_claim.lua -- PT2_CHAIN_CLAIM: a pill take v2 chain claims
-- every pill of the chain, not only the target.
-- =========================================================================
-- A HELM 2v2 replay (Nov 18): pills 13 and 14 side by side. One ally went
-- for 14; the other went for a 13+14 double take. Each claim named only its
-- own target, so the claim system saw no conflict and both went in.
--
-- Wire: "/info pch <target> <id> <id> ..." on its own verb (the /info state
-- slate is near the 128-byte cap). "/info pch" alone clears. The receiver
-- keeps the last list per ally in this module (M.rx), not in slot.info: an
-- ally's /info extra (merge_info) evicts every slot.info key it does not
-- carry. A list is honoured only while that ally's slot is active and its
-- /info state says goal=attack_pill target=<target>, so a stale list ends
-- by itself when the ally's goal changes. Old bots (MAST) do not know the
-- verb: they ignore it and see the target only.
--
-- Rule for two claims on one pill (M.keep), the same on both bots:
--   * a bot past approach/plan_position (engaged) beats one that is not;
--   * both engaged: a target claim beats a chain claim;
--   * else the cost band of ALLY_CLAIMED_STEAL_FRAC, then the lower player
--     number keeps (the attack_pill dual-hold rule).
-- The loser: a bot whose TARGET lost yields through sync_ally_claimed_rejects
-- (goals.lua); a chain owner whose CHAIN pill lost plans without it
-- (M.chain_skip -> inp.chain_skip in pilltake_v2).
-- State: the last list per ally (M.rx) and a payload decode memo (pure,
-- size-capped).
-- =========================================================================

local C          = require("constants")
local print2     = require("print2")
local ally_state = require("ally_state")

local M = {}

function M.on()
  return C.PT2_CHAIN_CLAIM and C.PILLTAKE_V2_EVALUATOR and C.PT2_CHAINS
end

-- "<target> <id> ...": the target, then the other chain pills (no repeats,
-- target left out). nil when there is no other pill.
function M.encode(target, ids)
  if not target or not ids then return nil end
  local out, seen = { tostring(target) }, { [target] = true }
  for _, id in ipairs(ids) do
    if id and not seen[id] then
      seen[id] = true
      out[#out + 1] = tostring(id)
    end
  end
  if #out < 2 then return nil end
  return table.concat(out, " ")
end

-- payload -> target, { [id] = true } (the other chain pills), or nil.
local memo, memo_n = {}, 0
function M.decode(payload)
  if not payload or payload == "" then return nil end
  local m = memo[payload]
  if m then return m.t, m.set end
  local target, set, n = nil, {}, 0
  for w in payload:gmatch("%S+") do
    local v = tonumber(w)
    if not v then return nil end
    if not target then target = v
    elseif v ~= target then set[v] = true; n = n + 1 end
  end
  if not target or n == 0 then return nil end
  if memo_n >= 64 then memo, memo_n = {}, 0 end
  memo[payload] = { t = target, set = set }
  memo_n = memo_n + 1
  return target, set
end

-- Receiver: the last "/info pch" payload per ally (nil clears).
local chains = {}
function M.rx(pn, payload)
  if pn == nil then return end
  if payload == "" then payload = nil end
  chains[pn] = payload
end
function M.rx_get(pn) return chains[pn] end

-- Ally pn's honoured chain set (h = its slot.info), or nil.
function M.ally_set(h, pn)
  local pl = pn ~= nil and chains[pn]
  if not h or not pl or h.goal ~= "attack_pill" then return nil end
  local t, set = M.decode(pl)
  if not t or tonumber(h.target) ~= t then return nil end
  return set, t
end

local function engaged(sub)
  return sub ~= nil and sub ~= "" and sub ~= "approach" and sub ~= "plan_position"
end
M.engaged = engaged

-- Does our claim keep the pill against the ally's? Both bots compute this
-- with the sides swapped and get opposite answers.
function M.keep(our_cost, our_sub, our_target, ally_cost, ally_sub, ally_target, self_pn, ally_pn)
  local eo, ea = engaged(our_sub), engaged(ally_sub)
  if eo ~= ea then return eo, eo and "we_engaged" or "ally_engaged" end
  if eo and our_target ~= ally_target then
    return our_target, our_target and "target_beats_chain" or "chain_yields_to_target"
  end
  local frac = C.ALLY_CLAIMED_STEAL_FRAC or 0.25
  if our_cost and ally_cost then
    if our_cost < ally_cost * (1 - frac) then return true, "we_cheaper" end
    if ally_cost < our_cost * (1 - frac) then return false, "ally_cheaper" end
  end
  return self_pn < ally_pn, self_pn < ally_pn and "lower_id_keeps" or "higher_id_yields"
end

local function ally_live(state, pn, slot)
  local d = state.tank_dead_at and state.tank_dead_at[pn]
  return not (d and d > (slot.last_tick or 0))
end

-- Every ally claim on pill pid: calls fn(ally_pn, slot.info, is_target).
local function each_claim(state, pid, now, self_pn, fn)
  for pn, slot in ally_state.iter_active(now, 1750) do
    if pn ~= self_pn and ally_live(state, pn, slot) then
      local h = slot.info
      if h.goal == "attack_pill" then
        if tonumber(h.target) == pid then
          if fn(pn, h, true) then return end
        else
          local set = M.ally_set(h, pn)
          if set and set[pid] then
            if fn(pn, h, false) then return end
          end
        end
      end
    end
  end
end
M.each_claim = each_claim

-- The first ally claiming pid (target or chain): pn, is_target, or nil.
function M.claimed_by(state, pid, now, self_pn)
  local by, tg
  each_claim(state, pid, now, self_pn, function(pn, _, is_t) by, tg = pn, is_t; return true end)
  return by, tg
end

-- First pill of ids[2..] (the chain past the target) an ally claims:
-- pid, ally pn. For pt2_dbl: no discount on such a chain.
function M.chain_ally_claimed(state, ids, now, self_pn)
  for i = 2, #ids do
    local by = M.claimed_by(state, ids[i], now, self_pn)
    if by then return ids[i], by end
  end
  return nil
end

-- Our side of the rule: the cost we advertise, our sub.
local function our_bid(state)
  local bse = state.broadcast_state_extra
  local g = state.goal
  return bse and tonumber(bse.cost), g and g.substate or ""
end

-- Pills of the v2 input an ally holds against us: { [pill obj] = ally pn }.
-- The target (A) is never skipped: a lost target is the sync's business.
-- nil when nothing is skipped.
function M.chain_skip(state, inp, now, self_pn)
  if not M.on() or not inp.ids then return nil end
  local our_cost, our_sub = our_bid(state)
  local skip
  for _, p in ipairs(inp.pills or {}) do
    local pid = inp.ids[p]
    if pid and p ~= inp.A then
      each_claim(state, pid, now, self_pn, function(pn, h, is_t)
        local keep, why = M.keep(our_cost, our_sub, false, tonumber(h.cost), h.sub, is_t, self_pn, pn)
        if not keep then
          skip = skip or {}
          skip[p] = pn
          return true
        end
      end)
    end
  end
  return skip
end

-- Sender: the payload for our current goal, or nil. Only live hostile or
-- neutral chain pills from the current step on.
function M.payload(state, world)
  if not M.on() then return nil end
  local g = state.goal
  if not g or g.kind ~= "attack_pill" or not g.target_id then return nil end
  local ch = g._pt2
  if not ch or ch.mode ~= "HOLD" or not ch.ids then return nil end
  local ids = {}
  local wp = world and world.pills or {}
  for k = (ch.step or 1), #ch.ids do
    local id = ch.ids[k]
    local p = id and wp[id]
    if p and (p.owner == "hostile" or p.owner == "neutral") and (p.health or 0) > 0
       and not p.in_tank then
      ids[#ids + 1] = id
    end
  end
  return M.encode(g.target_id, ids)
end

-- Sender: change-detect, 300-tick heartbeat (the pblk / bwl shape).
function M.tx(state, world, now, try_send)
  local pl = M.payload(state, world)
  if pl ~= state._last_pch_sent
     or (pl and (now - (state._last_pch_tick or -100000)) >= 300) then
    local msg = pl and ("/info pch " .. pl) or (state._last_pch_sent and "/info pch")
    if msg and try_send(msg, 0) then
      state._last_pch_sent = pl
      state._last_pch_tick = now
    end
  end
end

return M
