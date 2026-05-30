-- =============================================================================
-- ally_state.lua — pre-allocated per-bot shared-state slate
-- =============================================================================
-- Each NewAutopilot brain tracks what every player slot (allies + self) is
-- currently doing, so the local bot can coordinate (don't double-team the
-- same pill, yield to a higher-cost ally, target the same enemy together,
-- etc.).
--
-- Data shape per slot:
--   info      : open-ended hash of string keys → string values.  The set of
--                keys is whatever the most recent /info state message
--                carried.  Common keys: goal, sub, target, cost, mx, my,
--                help, plus per-goal extras (ppt, pill_id, ...).  Receiver
--                clears every key not in the incoming message, so stale
--                keys self-evict.
--   last_tick : tick number of the most recent update for staleness checks.
--   active    : true once at least one update has landed for this slot.
--
-- GC discipline:
--   * Slot tables (and their .info hashes) are allocated ONCE at init.
--     Subsequent updates set/clear keys in place — Lua's hash part keeps
--     its allocated buckets even after entries are nilled, so a steady-
--     state key set produces zero allocations.
--   * Short repeated state names ("attack_pill", "build_walls") are
--     interned, so re-assigning the same string is free.
-- =============================================================================

local M = {}

local MAX_TANKS = 16
M.MAX_TANKS = MAX_TANKS

M.slots = {}

-- Initialization
-- Allocates the 16 slots up-front.  Idempotent: a second call resets every
-- slot to empty without re-allocating the slot or info table.
function M.init()
  for pn = 0, MAX_TANKS - 1 do
    local slot = M.slots[pn]
    if not slot then
      M.slots[pn] = {
        info      = {},
        last_tick = 0,
        active    = false,
      }
    else
      for k in pairs(slot.info) do slot.info[k] = nil end
      if slot.extra then for k in pairs(slot.extra) do slot.extra[k] = nil end end
      slot.last_tick = 0
      slot.active    = false
    end
  end
end

-- Read accessors
function M.get(player_num)
  return M.slots[player_num]
end

function M.is_fresh(player_num, now, max_age)
  local slot = M.slots[player_num]
  if slot == nil or not slot.active then return false end
  if max_age == nil then return true end
  return (now - slot.last_tick) <= max_age
end

-- Read a single key from a bot's info hash.  Returns "" if absent so the
-- caller doesn't have to handle nil.  For number-typed keys (cost, help)
-- the caller wraps with tonumber() — wire format is all strings.
function M.get_key(player_num, key)
  local slot = M.slots[player_num]
  if slot == nil then return "" end
  return slot.info[key] or ""
end

-- Replace a slot's info hash with new_hash.  Every key not present in
-- new_hash is cleared (set to nil) so stale fields evict.  Updates
-- last_tick + activates the slot.  new_hash MUST contain only string
-- values; callers feeding numbers should tostring() them first.
function M.set_info(player_num, now, new_hash)
  local slot = M.slots[player_num]
  if slot == nil then return end
  local info = slot.info
  -- Clear every existing key so anything absent from new_hash evicts —
  -- EXCEPT keys that arrived via /info extra (tracked on slot.extra),
  -- which represent supplementary fields the sender ships on idle
  -- ticks.  Without this protection a /info state arriving after a
  -- /info extra would wipe attack_pill's "p=..." until the sender's
  -- next idle tick, leaving receivers blind in between.
  local extra = slot.extra
  for k in pairs(info) do
    if not (extra and extra[k]) then info[k] = nil end
  end
  -- Apply new keys.
  for k, v in pairs(new_hash) do info[k] = v end
  slot.last_tick = now
  slot.active    = true
end

-- Merge new_hash into the slot WITHOUT clearing existing keys.
-- Used by /info extra so a sender can ship supplementary fields on a
-- separate tick without wiping the keys an earlier /info state set.
-- Updates last_tick so heartbeat-style staleness checks still see the
-- slot as fresh.
function M.merge_info(player_num, now, new_hash)
  local slot = M.slots[player_num]
  if slot == nil then return end
  local info = slot.info
  -- Track every key we receive via /info extra on slot.extra so the
  -- next /info state doesn't wipe them.  Lazy-init the set so the
  -- common-case "this bot never sends extras" stays allocation-free.
  local extra = slot.extra
  if extra == nil then extra = {}; slot.extra = extra end
  -- /info extra carries the sender's COMPLETE current extra set (the
  -- sender rebuilds it from scratch each send), so any extra key absent
  -- from new_hash has been dropped and must evict.  Without this a stale
  -- "p=" (an ally's old attack_pill standoff) would linger forever,
  -- because set_info deliberately protects extra keys from its own
  -- eviction sweep.  /info state keys (never tracked on slot.extra) are
  -- left untouched.
  for k in pairs(extra) do
    if new_hash[k] == nil then
      info[k]  = nil
      extra[k] = nil
    end
  end
  for k, v in pairs(new_hash) do
    info[k]  = v
    extra[k] = true
  end
  slot.last_tick = now
  slot.active    = true
end

function M.clear(player_num)
  local slot = M.slots[player_num]
  if slot == nil then return end
  for k in pairs(slot.info) do slot.info[k] = nil end
  if slot.extra then for k in pairs(slot.extra) do slot.extra[k] = nil end end
  slot.last_tick = 0
  slot.active    = false
end

-- Iteration helper.  Yields (player_num, slot) for active+fresh slots.
function M.iter_active(now, max_age)
  local pn = -1
  return function()
    while pn < MAX_TANKS - 1 do
      pn = pn + 1
      local slot = M.slots[pn]
      if slot ~= nil and slot.active then
        if max_age == nil or (now - slot.last_tick) <= max_age then
          return pn, slot
        end
      end
    end
    return nil
  end
end

-- =============================================================================
-- Overlay
-- =============================================================================
-- Right-middle HUD table: one row per active slot.  Columns are
-- (#, goal, sub, target, cost, data) where the first five are PROMOTED
-- well-known keys and `data` is the remaining k=v pairs from .info.
--
-- Caller passes:
--   viz             — the viz module
--   now             — current tick
--   self_player_num — local bot's slot, highlighted cyan
--   max_age         — ticks; slots older than this render gray
--
-- Pixel layout: anchored to top-right.  Header at y=200, rows +14 px each.

local _HEADER_FMT = "%-3s %-13s %-13s %-10s %-5s %-6s %s"
local _ROW_FMT    = "%-3d %-13s %-13s %-10s %-5s %-6d %s"

-- Keys we promote to dedicated columns.  Everything else falls into `data`.
local _PROMOTED = { goal = true, sub = true, target = true, cost = true }

-- Scratch buffer reused across draws so the per-tick k=v render doesn't
-- create a fresh array.
local _kv_buf = {}

local function _build_data_string(info)
  local n = 0
  for k, v in pairs(info) do
    if not _PROMOTED[k] then
      n = n + 1
      _kv_buf[n] = k .. "=" .. v
    end
  end
  for i = n + 1, #_kv_buf do _kv_buf[i] = nil end
  if n == 0 then return "" end
  return table.concat(_kv_buf, " ")
end

function M.draw(viz, now, self_player_num, max_age)
  if viz == nil or not viz.is_on or not viz.is_on("ally_state_overlay") then
    return
  end
  if not viz.hud_text then return end

  local x      = 480
  local y0     = 200
  local row_dy = 14


  local y = y0 + row_dy
  for pn = 0, MAX_TANKS - 1 do
    local slot = M.slots[pn]
    if slot ~= nil and slot.active then
      local stale = max_age ~= nil and (now - slot.last_tick) > max_age
      local is_self = (self_player_num ~= nil and pn == self_player_num)
      local r, g, b, a
      if stale then
        r, g, b, a = 120, 120, 120, 180
      elseif is_self then
        r, g, b, a = 120, 220, 255, 255
      else
        r, g, b, a = 160, 255, 160, 240
      end
      local info = slot.info
      y = y + row_dy
    end
  end
end

-- =============================================================================
-- Chat log
-- =============================================================================
-- Ring buffer of the most recent chat messages this brain saw or sent.
-- Used by the "chat_log_overlay" HUD so a human watching BrainTest can
-- follow the /info traffic in real time.  Allocated once, slots reused —
-- entry tables are not freed/recreated, only their fields are overwritten.

local CHAT_LOG_CAP = 20

local _cl_entries = {}
local _cl_head    = 1   -- next write slot (1..CHAT_LOG_CAP)
local _cl_count   = 0   -- 0..CHAT_LOG_CAP

for i = 1, CHAT_LOG_CAP do
  _cl_entries[i] = { dir = "", sender = 0, text = "", tick = 0 }
end

-- Record a chat line.  dir is "in" or "out".  sender is the player_num
-- of the originator (self for outbound, the actual sender for inbound).
function M.chat_log_add(dir, sender, text, tick)
  if text == nil or text == "" then return end
  local e = _cl_entries[_cl_head]
  e.dir    = dir
  e.sender = sender or 0
  e.text   = text
  e.tick   = tick or 0
  _cl_head = (_cl_head % CHAT_LOG_CAP) + 1
  if _cl_count < CHAT_LOG_CAP then _cl_count = _cl_count + 1 end
end

-- Draw the chat log under the ally state table.  Shows ONLY entries
-- from the current tick — older messages drop off as the tick advances.
-- Newest-on-top within that tick.
function M.draw_chat_log(viz, now, self_player_num)
  if viz == nil or not viz.is_on or not viz.is_on("chat_log_overlay") then
    return
  end
  if not viz.hud_text then return end

  local x      = 480
  local y0     = 460
  local row_dy = 14


  local y = y0 + row_dy
  for i = 0, _cl_count - 1 do
    local idx = _cl_head - 1 - i
    while idx < 1 do idx = idx + CHAT_LOG_CAP end
    local e = _cl_entries[idx]
    if e.tick == now then
      local r, g, b, a
      if e.dir == "out" then
        r, g, b, a = 120, 220, 255, 255
      else
        r, g, b, a = 160, 255, 160, 240
      end
      local arrow = (e.dir == "out") and ">" or "<"
      local line = string.format("%s p%-2d %s", arrow, e.sender, e.text)
      y = y + row_dy
    end
  end
end

return M
