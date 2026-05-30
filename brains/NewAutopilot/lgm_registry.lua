-- =============================================================================
-- lgm_registry.lua — per-player LGM state, aggregated from every source
-- the brain has access to.
-- =============================================================================
-- Tracks one slot per player_num (0..15).  Status fields:
--   status      : "in_tank" | "ground" | "dead" | "unknown"
--   mx, my      : last known tile (only meaningful for in_tank/ground)
--   last_update : tick of most recent write
--   source      : "self" | "ally_bcast" | "vis" | "event"
--   dead_at     : tick the LGM was killed (set by EVENT_LGM_LOST)
--   killer_pn   : player_num of the killer (set by EVENT_LGM_LOST)
--   respawn_eta : tick at which the LGM is expected to be back
--
-- Sources:
--   * self (info.man_*)                  → update_self()
--   * ally /info state broadcasts        → update_from_ally_state()
--   * visible OBJECT_BUILDMAN sightings  → (later step)
--   * EVENT_LGM_LOST world events        → note_death()
--
-- GC discipline: 16 slots pre-allocated at init.  Field writes in place.
-- =============================================================================

local M = {}

local C = require("constants")

local MAX_TANKS = 16
M.MAX_TANKS = MAX_TANKS

M.slots = {}

local function _make_slot()
  return {
    status      = "unknown",
    mx          = nil, my = nil,
    last_update = 0,
    source      = nil,
    dead_at     = nil,
    killer_pn   = nil,
    respawn_eta = nil,
  }
end

function M.init()
  for pn = 0, MAX_TANKS - 1 do
    if not M.slots[pn] then
      M.slots[pn] = _make_slot()
    else
      local s = M.slots[pn]
      s.status, s.mx, s.my = "unknown", nil, nil
      s.last_update, s.source = 0, nil
      s.dead_at, s.killer_pn, s.respawn_eta = nil, nil, nil
    end
  end
end

-- Self update: called every tick from init.lua with info.man_* values.
-- man_status: 0 = in tank, 1 = dead, anything else = outside (deployed).
-- Returns true on a status transition (caller can use it to trigger an
-- immediate broadcast).
function M.update_self(self_pn, man_status, man_mx, man_my, now)
  local s = M.slots[self_pn]
  if s == nil then return false end
  local new_status
  if     man_status == 0 then new_status = "in_tank"
  elseif man_status == 1 then new_status = "dead"
  else                        new_status = "ground"
  end
  local transitioned = (s.status ~= new_status)
  s.status      = new_status
  s.mx          = man_mx
  s.my          = man_my
  s.last_update = now
  s.source      = "self"
  -- If we just transitioned out of "dead", clear the death bookkeeping
  -- so consumers stop seeing stale dead_at / respawn_eta values.
  if transitioned and new_status ~= "dead" then
    s.dead_at, s.killer_pn, s.respawn_eta = nil, nil, nil
  end
  return transitioned
end

-- Ally broadcast update: called from the inbox processor when an
-- /info state message arrives carrying lgm_st / lgmx / lgmy fields.
-- Sender's slot is updated from those fields.
function M.update_from_ally_state(sender_pn, info_hash, now)
  local s = M.slots[sender_pn]
  if s == nil or info_hash == nil then return end
  local st = info_hash.lgm_st
  if st ~= "in_tank" and st ~= "ground" and st ~= "dead" then return end
  s.status      = st
  s.mx          = tonumber(info_hash.lgmx)
  s.my          = tonumber(info_hash.lgmy)
  s.last_update = now
  s.source      = "ally_bcast"
  if st ~= "dead" then
    s.dead_at, s.killer_pn, s.respawn_eta = nil, nil, nil
  end
end

-- EVENT_LGM_LOST: stamp death bookkeeping on the victim's slot.
-- respawn_eta is a heuristic; the engine doesn't fire a "revived"
-- event, so consumers should treat respawn_eta as "probably back by
-- this tick" and rely on subsequent self/ally updates to confirm.
function M.note_death(victim_pn, killer_pn, now)
  local s = M.slots[victim_pn]
  if s == nil then return end
  s.status      = "dead"
  s.dead_at     = now
  s.killer_pn   = killer_pn
  s.respawn_eta = now + C.ENEMY_LGM_RETURN_TICKS
  s.last_update = now
  s.source      = "event"
end

function M.get(pn)
  return M.slots[pn]
end

function M.is_dead(pn, now)
  local s = M.slots[pn]
  if s == nil then return false end
  if s.status == "dead" then return true end
  if s.respawn_eta and now < s.respawn_eta then return true end
  return false
end

-- =============================================================================
-- Visualization
-- =============================================================================
-- Two surfaces:
--   * HUD table (right-side, below ally_state) — one row per active slot
--     with status / tile / source / countdown.
--   * Map markers — green ring (alive ally), red ring (alive enemy),
--     gray X (dead, with respawn countdown).  Drawn only when we have
--     a meaningful tile (i.e. the slot's mx/my is set).
-- Both are gated by their own viz toggles (see viz.lua).

local _STATUS_COLOR = {
  in_tank = { 160, 220, 255, 240 },
  ground  = { 120, 255, 120, 240 },
  dead    = { 200,  80,  80, 220 },
  unknown = {  90,  90,  90, 180 },
}

function M.draw_hud(viz, now, self_pn)
  if viz == nil or not viz.is_on or not viz.is_on("lgm_registry_hud") then return end
  if not viz.hud_text then return end
  local x, y0, row_dy = 480, 660, 14
  viz.hud_text("lgm_registry_hud", x, y0,
               string.format("%-4s %-7s %-9s %-5s %-7s", "pn", "status", "tile", "src", "resp_in"),
               "topright", 200, 200, 200, 220)
  local y = y0 + row_dy
  for pn = 0, MAX_TANKS - 1 do
    local s = M.slots[pn]
    if s and s.status ~= "unknown" then
      local col = _STATUS_COLOR[s.status] or _STATUS_COLOR.unknown
      local is_self = (self_pn ~= nil and pn == self_pn)
      local r, g, b, a = col[1], col[2], col[3], col[4]
      if is_self then r, g, b = 255, 255, 100 end   -- highlight self
      local tile_str = (s.mx and s.my) and string.format("%d,%d", s.mx, s.my) or "—"
      local src_str  = s.source or "?"
      local resp_str = (s.status == "dead" and s.respawn_eta)
                       and string.format("%ds", math.max(0, math.floor((s.respawn_eta - now) / 50)))
                       or  "—"
      viz.hud_text("lgm_registry_hud", x, y,
                   string.format("%-4d %-7s %-9s %-5s %-7s",
                                 pn, s.status, tile_str, src_str, resp_str),
                   "topright", r, g, b, a)
      y = y + row_dy
    end
  end
end

-- Map-overlay rings on every slot that has a tile and isn't us.  Self
-- already has the ally_lgm_marker / own-LGM overlays handled elsewhere
-- by init.lua, so we skip the self slot here to avoid stacking circles.
function M.draw_map(viz, now, self_pn, allies_bitmap)
  if viz == nil or not viz.is_on or not viz.is_on("lgm_registry_map") then return end
  if not viz.circle or not viz.text then return end
  for pn = 0, MAX_TANKS - 1 do
    local s = M.slots[pn]
    if s and pn ~= self_pn and s.mx and s.my and s.status ~= "unknown" then
      local cx, cy = s.mx + 0.5, s.my + 0.5
      local is_ally = (allies_bitmap and (allies_bitmap & (1 << pn)) ~= 0)
      local r, g, b
      if s.status == "dead" then
        r, g, b = 120, 120, 120
        viz.circle("lgm_registry_map", cx, cy, 0.4, r, g, b, 200)
        viz.text("lgm_registry_map", cx, cy - 0.5,
                 string.format("p%d dead %ds", pn,
                               math.max(0, math.floor(((s.respawn_eta or now) - now) / 50))),
                 "center", r, g, b, 200)
      else
        r, g, b = is_ally and 100 or 255, is_ally and 220 or 120, is_ally and 100 or 120
        viz.circle("lgm_registry_map", cx, cy, 0.35, r, g, b, 220)
        viz.text("lgm_registry_map", cx, cy - 0.5,
                 string.format("p%d %s", pn, s.status),
                 "center", r, g, b, 200)
      end
    end
  end
end

return M
