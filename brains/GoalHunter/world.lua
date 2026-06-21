-- =========================================================================
-- GoalHunter/world.lua — base/pill tracking, anger model, spatial index
--
-- Invariant: pill and base IDs are stable for the entire game session (Bolo
-- allocates them at map load and never frees them). That lets us mutate the
-- existing record in place on each tick instead of reallocating, and keep
-- the pill_at / base_at spatial indexes incrementally maintained instead of
-- rebuilding both from scratch every tick.
-- =========================================================================

local C       = require("constants")
local log     = require("logger")
local metrics = require("metrics")
local print2  = require("print2")
local TAG     = "[" .. C.BRAIN_NAME .. "]"

-- clock_us is registered as a global by braincore.c; fall back to 0 so the
-- replay harness (which doesn't inject a real clock_us) doesn't crash.
local clock_us = clock_us or function() return 0 end

local M = {}

local function owner_string(obj_info)
  local hostile = (obj_info & OBJECT_HOSTILE) ~= 0
  local neutral = (obj_info & OBJECT_NEUTRAL) ~= 0
  if neutral then return "neutral" end
  if hostile then return "hostile" end
  return "friendly"
end

-- Classify a pill by its raw owner PLAYER NUMBER + carried state.
--
-- WinBolo pillboxes are SHARED team property: a DEPLOYED (built, on-map) pill
-- is treated identically whether I or a teammate placed it — so an ally's
-- deployed pill resolves to "friendly", and every existing my-pill consumer
-- (reposition, repair, coverage, defend, no-friendly-fire) just works.
-- The ONLY pill that is distinctly a teammate's is one carried IN HIS TANK,
-- which we tag "allied" so it (a) counts toward team totals and (b) is never
-- treated as a target/threat or repositioned (it's inside his tank).
--
-- Only the event path carries the real player number; the object scan only
-- has alliance bits, so we cache owner_player on the pill and reuse it there.
-- (Map objects are always deployed, so the scan passes in_tank=false.)
local function classify_owner(owner_val, info, in_tank)
  if owner_val == NEUTRAL_PLAYER then return "neutral" end
  if owner_val == (info and info.player_number) then return "friendly" end
  local allies = (info and info.allies) or 0
  if (allies & (1 << owner_val)) ~= 0 then
    return in_tank and "allied" or "friendly"
  end
  return "hostile"
end

local function mkey(mx, my) return my * 256 + mx end

-- Incremental pill_at maintenance. Multiple pills can share a tile (pickup /
-- replace transients), so pill_at[k] is a list.
local function pill_index_add(world, id, p)
  local k = mkey(p.mx, p.my)
  local list = world.pill_at[k]
  if not list then
    list = {}
    world.pill_at[k] = list
  end
  list[#list + 1] = { id = id, pill = p }
end

local function pill_index_remove(world, id, old_mx, old_my)
  local k = mkey(old_mx, old_my)
  local list = world.pill_at[k]
  if not list then return end
  for i = 1, #list do
    if list[i].id == id then
      table.remove(list, i)
      break
    end
  end
  if #list == 0 then world.pill_at[k] = nil end
end

-- Safety net only — nothing calls this in the hot path now that update and
-- process_events maintain pill_at / base_at incrementally. Kept around for
-- debugging / replay re-seeding if a snapshot ever arrives without indexes.
local function rebuild_index(world)
  local pill_at = {}
  for id, p in pairs(world.pills) do
    local k = mkey(p.mx, p.my)
    pill_at[k] = pill_at[k] or {}
    pill_at[k][#pill_at[k] + 1] = { id = id, pill = p }
  end
  world.pill_at = pill_at

  local base_at = {}
  for id, b in pairs(world.bases) do
    local k = mkey(b.mx, b.my)
    base_at[k] = { id = id, base = b }
  end
  world.base_at = base_at
end

-- Fold allies' advertised carried-pill ids (comms `carry=` field) into the
-- pill table as allied/in-tank entries. In-tank pills are dropped by the C
-- per-tick pill scan (pillsGetBrainPillsInRect requires inTank==FALSE) and the
-- EVENT_PILL_UPDATE that flips in_tank is view-rect gated in non-advantage AI
-- mode — so a bot out of view never learns the team is holding a pill. The
-- carrier knows its own carried ids first-hand (it stood on the pill at
-- pickup) and advertises them; we synthesize a minimal allied/in-tank entry
-- here so the portfolio counts + de-confliction stay consistent team-wide.
--   ally_carry: { [pill_id] = carrier_player_num }   (self excluded by caller)
function M.sync_ally_carried(world, ally_carry, now)
  local prev = world._ally_carry_ids
  local cur = nil
  for id, pn in pairs(ally_carry) do
    local p = world.pills[id]
    -- Never clobber first-hand knowledge refreshed THIS tick by a real event
    -- (last_seen == now) — the in-view truth always wins over the advert.
    if not (p and (p.last_seen or 0) >= now) then
      if p == nil then
        p = { mx = 0, my = 0, health = 0, anger = 0, anger_tick = 0,
              last_hit_tick = 0, under_attack = false, attack_tick = 0,
              attack_damage = 0 }
        world.pills[id] = p
        pill_index_add(world, id, p)
      end
      p.owner        = "allied"
      p.owner_player = pn
      p.in_tank      = true
      p.carrier      = pn
      p.last_seen    = now
      p._synth_carry = pn
    end
    cur = cur or {}; cur[id] = true
  end
  -- Evict phantom carries no longer advertised (carrier deployed / left view /
  -- died): a purely-synthesized in-tank ghost would otherwise linger forever.
  -- Only drop entries WE synthesized that no real event has refreshed this
  -- tick — a far bot honestly doesn't know where the pill landed until it sees
  -- it, which is no worse than the pre-fix "team doesn't have that pill" state.
  if prev then
    for id in pairs(prev) do
      if not (cur and cur[id]) then
        local p = world.pills[id]
        if p and p._synth_carry and p.in_tank and (p.last_seen or 0) < now then
          pill_index_remove(world, id, p.mx, p.my)
          world.pills[id] = nil
        end
      end
    end
  end
  world._ally_carry_ids = cur
end

function M.update(world, info, tick)
  world.tick = tick  -- store for staleness reporting

  local obj_count = 0
  local t0 = clock_us()
  for _, obj in ipairs(info.objects) do
    obj_count = obj_count + 1
    if obj.type == OBJECT_REFBASE then
      local new_mx     = obj.x >> 8
      local new_my     = obj.y >> 8
      local new_health = obj.direction
      local new_owner  = owner_string(obj.info)
      local b = world.bases[obj.idnum]
      if b == nil then
        b = {
          mx          = new_mx,
          my          = new_my,
          health      = new_health,
          owner       = new_owner,
          last_seen   = tick,
          last_health = new_health,
        }
        world.bases[obj.idnum] = b
        world.base_at[mkey(new_mx, new_my)] = { id = obj.idnum, base = b }
      else
        -- Capture last_health BEFORE overwriting health — change detection
        -- downstream (siege, capture alerts) compares health vs last_health.
        b.last_health = b.health
        if b.mx ~= new_mx or b.my ~= new_my then
          world.base_at[mkey(b.mx, b.my)] = nil
          b.mx = new_mx
          b.my = new_my
          world.base_at[mkey(new_mx, new_my)] = { id = obj.idnum, base = b }
        end
        b.health    = new_health
        b.owner     = new_owner
        b.last_seen = tick
      end
    elseif obj.type == OBJECT_PILLBOX then
      local new_mx     = obj.x >> 8
      local new_my     = obj.y >> 8
      local new_health = obj.direction
      local p = world.pills[obj.idnum]
      -- The object scan only carries alliance BITS. A visible pillbox object is
      -- always DEPLOYED (carried pills aren't map objects), so reclassify from
      -- the cached real owner_player with in_tank=false — an ally's deployed
      -- pill correctly resolves to shared "friendly".
      -- Hostility comes from the FRESH object bits: the engine computes them
      -- alliance-aware per viewer every tick (pillbox.c → NEUTRAL / FRIENDLY via
      -- isAllie / HOSTILE), so they're authoritative even when a pill-owner-change
      -- EVENT was missed and the cached owner_player went stale. (Carried pills
      -- never appear in the object scan — the inTank==FALSE gate — so the in_tank
      -- "allied" nuance that needs owner_player can't arise here.) This is what
      -- fixes a captured/ally pill lingering as "hostile" in pool 6 after a
      -- missed EVENT_PILL_UPDATE, even on a fresh sighting.
      local owner_str = owner_string(obj.info)
      -- Self-heal a stale cache: if the cached owner_player disagrees with the
      -- live bits on hostility, a pill-owner event was missed — drop it so the
      -- event path re-sources the real player number.
      if p and p.owner_player ~= nil then
        local cached = classify_owner(p.owner_player, info, false)
        if ((cached == "hostile" or cached == "neutral")) ~= ((owner_str == "hostile" or owner_str == "neutral")) then
          p.owner_player = nil
        end
      end
      if p == nil then
        p = {
          mx            = new_mx,
          my            = new_my,
          health        = new_health,
          owner         = owner_str,
          anger         = 0,
          anger_tick    = 0,
          last_hit_tick = 0,
          last_seen     = tick,
          in_tank       = false,  -- a visible map object is always DEPLOYED
          under_attack  = false,
          attack_tick   = 0,
          attack_damage = 0,
        }
        world.pills[obj.idnum] = p
        pill_index_add(world, obj.idnum, p)
      else
        -- Read old state BEFORE writing new — damage detection, anger bump,
        -- and the index move all need the previous tick's values.
        local old_health = p.health
        local old_mx     = p.mx
        local old_my     = p.my

        -- Anger: each fresh damage hit adds C.PILL_ANGER_BUMP, capped at
        -- 1.0. Three hits saturate. Otherwise decays linearly from the
        -- last bump's tick. Resetting anger_tick on every bump keeps the
        -- decay consistent — the next decay step measures from "now",
        -- not from the first hit hours ago.
        if new_health < old_health and new_health > 0 then
          p.anger      = math.min(1.0, (p.anger or 0) + C.PILL_ANGER_BUMP)
          p.anger_tick = tick
          p.last_hit_tick = tick   -- only on REAL damage (never on decay)
        elseif p.anger > 0 and tick > p.anger_tick then
          local elapsed = tick - p.anger_tick
          p.anger = math.max(0, p.anger - elapsed / C.PILL_ANGER_DECAY)
          p.anger_tick = tick
        end

        -- Under-attack tracking for friendly pills.
        if owner_str == "friendly" and new_health < old_health and new_health > 0 then
          local damage = old_health - new_health
          p.attack_damage = p.attack_damage + damage
          p.under_attack  = true
          p.attack_tick   = tick
          log.event("pill_under_attack", string.format("pill#%d@(%d,%d) dmg=%d hp=%d",
                    obj.idnum, new_mx, new_my, p.attack_damage, new_health))
        elseif p.under_attack and p.attack_tick > 0
               and (tick - p.attack_tick) > C.PILL_ATTACK_COOLDOWN then
          p.under_attack  = false
          p.attack_damage = 0
        end

        if old_mx ~= new_mx or old_my ~= new_my then
          pill_index_remove(world, obj.idnum, old_mx, old_my)
          p.mx = new_mx
          p.my = new_my
          pill_index_add(world, obj.idnum, p)
        end
        p.health    = new_health
        p.owner     = owner_str
        p.last_seen = tick
        -- Clear any stale in_tank flag: a pill visible in the object scan is
        -- DEPLOYED on the map (carried pills aren't map objects — see the
        -- inTank==FALSE gate note above). in_tank is otherwise only set true
        -- (self/ally carry) and only cleared by EVENT_PILL_UPDATE; if that
        -- drop event is missed (e.g. it fires while we're dead and the brain
        -- isn't ticking), the flag would linger true forever even as the
        -- dropped pill reappears on the map — making the brain think it's
        -- still carried and skip it for capture/pickup. Seeing it deployed is
        -- authoritative.
        p.in_tank   = false
      end
    end
  end
  local t1 = clock_us()

  metrics.set("us_world_update_objects", t1 - t0)
  metrics.set("world_update_obj_count", obj_count)
  -- Index is maintained incrementally now; kept for report continuity.
  metrics.set("us_world_update_reindex", 0)
end

-- ---------------------------------------------------------------------------
-- M.process_events(world, info, state)
-- Process game events received from the C brain interface.
-- Events provide instant updates before the normal object-scan in M.update().
-- ---------------------------------------------------------------------------
function M.process_events(world, info, state)
  local events = info.events
  if not events or #events == 0 then return end
  local tick = state.tick

  for _, ev in ipairs(events) do
    local d = ev.data
    if ev.type == EVENT_PILL_CAPTURED and d then
      -- data: [newOwner, prevOwner]
      -- We don't know which pill index this is from the event alone,
      -- but EVENT_PILL_UPDATE follows with full state; this is informational.

    elseif ev.type == EVENT_BASE_CAPTURED and d then
      -- Informational; EVENT_BASE_UPDATE follows with full state.

    elseif ev.type == EVENT_PILL_UPDATE and d then
      -- data: [pillIndex, x, y, owner, armour, speed, inTank]
      local idx = d[1]
      if idx then
        local new_health = d[5] or 0
        local owner_val = d[4] or 0xFF
        local in_tank = (d[7] or 0) ~= 0
        -- Alliance-aware: the event carries the real owner player number AND
        -- the in_tank flag, so an ally's deployed pill becomes shared "friendly"
        -- while an ally's carried pill becomes "allied". owner_player is cached
        -- below so the bits-only object scan can stay consistent.
        local owner_str = classify_owner(owner_val, info, in_tank)

        local p = world.pills[idx]
        if p == nil then
          p = {
            mx            = d[2] or 0,
            my            = d[3] or 0,
            health        = new_health,
            owner         = owner_str,
            owner_player  = owner_val,   -- real player number (event-sourced); lets the object scan stay alliance-aware
            anger         = 0,
            anger_tick    = 0,
            last_hit_tick = 0,
            last_seen     = tick,
            in_tank       = in_tank,
            under_attack  = false,
            attack_tick   = 0,
            attack_damage = 0,
          }
          world.pills[idx] = p
          pill_index_add(world, idx, p)
        else
          -- Read old state BEFORE writing new — index move and damage
          -- detection both need the previous tick's values.
          local old_health = p.health
          local old_mx     = p.mx
          local old_my     = p.my
          local new_mx     = d[2] or old_mx
          local new_my     = d[3] or old_my

          if new_health < old_health and new_health > 0 then
            p.anger      = math.min(1.0, (p.anger or 0) + C.PILL_ANGER_BUMP)
            p.anger_tick = tick
            p.last_hit_tick = tick   -- only on REAL damage (never on decay)
          end

          if owner_str == "friendly" and new_health < old_health and new_health > 0 then
            local damage = old_health - new_health
            p.attack_damage = p.attack_damage + damage
            p.under_attack  = true
            p.attack_tick   = tick
            log.event("pill_under_attack", string.format("pill#%d@(%d,%d) dmg=%d hp=%d",
                      idx, new_mx, new_my, p.attack_damage, new_health))
          elseif p.under_attack and p.attack_tick > 0
                 and (tick - p.attack_tick) > C.PILL_ATTACK_COOLDOWN then
            p.under_attack  = false
            p.attack_damage = 0
          end

          if old_mx ~= new_mx or old_my ~= new_my then
            pill_index_remove(world, idx, old_mx, old_my)
            p.mx = new_mx
            p.my = new_my
            pill_index_add(world, idx, p)
          end
          p.health       = new_health
          p.owner        = owner_str
          p.owner_player = owner_val
          p.last_seen    = tick
          p.in_tank      = in_tank
        end
      end

    elseif ev.type == EVENT_BASE_UPDATE and d then
      -- data: [baseIndex, owner, armour, shells, mines]
      local idx = d[1]
      if idx then
        local owner_val = d[2] or 0xFF
        local new_health = d[3] or 0
        local owner_str
        if owner_val == NEUTRAL_PLAYER then
          owner_str = "neutral"
        elseif owner_val == info.player_number then
          owner_str = "friendly"
        else
          owner_str = "hostile"
        end

        local b = world.bases[idx]
        if b == nil then
          -- EVENT_BASE_UPDATE doesn't carry position; seed at (0,0) and let
          -- the next M.update object-scan move the base_at entry to the
          -- correct key.
          b = {
            mx          = 0,
            my          = 0,
            health      = new_health,
            owner       = owner_str,
            owner_player = owner_val,   -- real player number; lets the LGM-block stamp do an alliance check (owner_str lumps ally bases in with "hostile")
            last_seen   = tick,
            last_health = new_health,
            obs_shells  = d[4],
            obs_armour  = new_health,
            obs_tick    = tick,
          }
          world.bases[idx] = b
          world.base_at[mkey(0, 0)] = { id = idx, base = b }
        else
          -- Capture last_health BEFORE overwriting health — change detection
          -- downstream relies on this ordering.
          b.last_health = b.health
          b.health      = new_health
          b.owner       = owner_str
          b.owner_player = owner_val
          b.last_seen   = tick
          b.obs_shells  = d[4]
          b.obs_armour  = new_health
          b.obs_tick    = tick
        end
      end

    elseif ev.type == EVENT_TANK_KILLED and d then
      -- data: [killedPlayer, killerPlayer] (or just [killedPlayer])
      -- Stamp tick so coordination logic (e.g. capture_pill ally-priority
      -- in goals.lua sync) can short-circuit windows that depend on a
      -- now-dead ally finishing what they started.
      local pn = d[1] or 0
      state.tank_dead_at = state.tank_dead_at or {}
      state.tank_dead_at[pn] = tick or 0
      if BRAIN_DEBUG_MODE then print2(string.format("TANK_KILLED_EVENT t=%d killed_pn=%d killer=%s", tick or 0, pn, tostring(d[2]))) end

    elseif ev.type == EVENT_PLAYER_LEAVE and d then
      -- data: [playerNum]
      -- Player left; downstream systems will notice missing objects.

    elseif ev.type == EVENT_LGM_LOST and d then
      -- data: [victim_pn, killer_pn] — a player's builder was killed.
      -- Stamp the death + respawn ETA on the lgm_registry so
      -- attack_pill cost shaping, ally coordination, etc. can react.
      local _lgmreg = package.loaded["lgm_registry"]
      if _lgmreg then
        _lgmreg.note_death(d[1] or 0, d[2] or 0, tick or 0)
      end
    end
  end
end

-- Reset spatial index (call from Brain.open after clearing bases/pills)
function M.reset(world)
  world.pill_at = {}
  world.base_at = {}
end

-- Lookup: return the pill entry at (mx, my) with health > 0, or nil.
-- When multiple pills share a tile (rare), returns the first live one.
function M.pill_at(world, mx, my)
  local entries = world.pill_at[mkey(mx, my)]
  if not entries then return nil end
  for _, e in ipairs(entries) do
    if e.pill.health > 0 then return e.pill end
  end
  return nil
end

-- Lookup: return the base entry at (mx, my), or nil.
function M.base_at(world, mx, my)
  local entry = world.base_at[mkey(mx, my)]
  return entry and entry.base or nil
end

-- True if the tank is currently parked on a friendly or neutral base tile.
-- Note: info.base in BrainInfo is set whenever a base is within ~7 tiles
-- (BASE_STATUS_RANGE), so it cannot be used for "actually on the base".
function M.tank_on_friendly_base(world, info)
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local b = M.base_at(world, tmx, tmy)
  if not b then return false end
  return b.owner == "friendly" or b.owner == "neutral"
end

function M.print_report(world)
  local base_ids = {}
  for id in pairs(world.bases) do base_ids[#base_ids + 1] = id end
  table.sort(base_ids)
  print(string.format(TAG .. " === Bases (%d) ===", #base_ids))
  for _, id in ipairs(base_ids) do
    local b = world.bases[id]
    local age = b.last_seen and (world.tick and (world.tick - b.last_seen) or 0) or -1
    print(string.format(TAG .. "   Base #%d  (%d, %d)  hp=%d  %s  seen=%dt ago",
          id, b.mx, b.my, b.health or 0, b.owner, age))
  end

  local pill_ids = {}
  for id in pairs(world.pills) do pill_ids[#pill_ids + 1] = id end
  table.sort(pill_ids)
  print(string.format(TAG .. " === Pillboxes (%d) ===", #pill_ids))
  for _, id in ipairs(pill_ids) do
    local p = world.pills[id]
    local age = p.last_seen and (world.tick and (world.tick - p.last_seen) or 0) or -1
    print(string.format(TAG .. "   Pill #%d  (%d, %d)  health=%d  %s  seen=%dt ago",
          id, p.mx, p.my, p.health, p.owner, age))
  end
end

-- =========================================================================
-- Known-world sharing (comms verb /info kw)
--
-- Allies relay FIRST-HAND base/pill allegiance + location changes to each
-- other, so out-of-view objects stay current team-wide. Essential in
-- no-advantage games where every bot is view-rect limited (and a useful
-- fallback otherwise). Newest observed-tick wins on the receiver; only
-- first-hand changes are broadcast (received data is NEVER relayed) so there
-- are no echo loops. Wire class chars: f=friendly h=hostile n=neutral.
--
-- Shared: deployed bases, deployed pills, and ENEMY-carried pills. Friendly/
-- allied carried pills are observer-relative ("friendly" to the carrier,
-- "allied" to everyone else), so they keep riding the separate
-- sync_ally_carried path instead.
-- =========================================================================

local KW_CLS_CHAR   = { friendly = "f", hostile = "h", neutral = "n" }
local KW_CHAR_OWNER = { f = "friendly", h = "hostile", n = "neutral" }

-- Shareable class signature for an object, or nil to skip. Pills append an
-- in_tank flag; only enemy-carried pills (hostile + in_tank) are shared.
local function kw_sig(obj, is_pill)
  local c = KW_CLS_CHAR[obj.owner]
  if not c then return nil end                       -- "allied"/unknown: skip
  if is_pill then
    if obj.in_tank then
      if obj.owner ~= "hostile" then return nil end  -- only enemy-carried
      return c .. "1"
    end
    return c .. "0"
  end
  return c
end

-- Diff this tick's first-hand sightings against the last-broadcast snapshot;
-- queue changed objects into world._kw_dirty for the comms layer to drain.
-- First-hand = last_seen==now AND not adopted-from-ally this tick (_kw_ally).
function M.collect_kw_changes(world, now)
  world._kw_shared = world._kw_shared or {}
  world._kw_dirty  = world._kw_dirty  or {}
  local shared, dirty = world._kw_shared, world._kw_dirty
  for id, b in pairs(world.bases) do
    if b.last_seen == now and b._kw_ally ~= now then
      local sig, key = kw_sig(b, false), "b" .. id
      if sig and shared[key] ~= sig then
        local old = shared[key]
        shared[key] = sig
        dirty[key]  = { kind = "b", id = id, mx = b.mx, my = b.my, cls = sig, tick = now }
        print2(string.format("KW_CHANGE t=%d b%d %s->%s @(%d,%d) queued (first-hand)", now, id, tostring(old), sig, b.mx, b.my))
      end
    end
  end
  for id, p in pairs(world.pills) do
    if p.last_seen == now and p._kw_ally ~= now then
      -- ci = shared class+intank ("h0"/"n0"/"h1"…) or nil to skip. Dead pills
      -- (health<=0) share as neutral so allies holding a stale hostile/friendly
      -- view stop stamping them.
      local ci = ((p.health or 0) <= 0) and "n0" or kw_sig(p, true)
      if ci then
        -- Change-detect signature: DEPLOYED pills (intank 0) are POSITION-
        -- sensitive, so a relocation (picked up + redeployed elsewhere) re-
        -- broadcasts its new tile — otherwise allies stamp influence at the
        -- old spot forever. In-tank pills (intank 1) stay position-INsensitive:
        -- they don't stamp influence and we won't spam every tile they cross.
        local sig = (ci:sub(2, 2) == "0") and (ci .. "@" .. p.mx .. "," .. p.my) or ci
        local key = "p" .. id
        if shared[key] ~= sig then
          local old = shared[key]
          shared[key] = sig
          dirty[key]  = { kind = "p", id = id, mx = p.mx, my = p.my,
                          cls = ci:sub(1, 1), intank = ci:sub(2, 2), tick = now }
          print2(string.format("KW_CHANGE t=%d p%d %s->%s @(%d,%d) queued (first-hand)", now, id, tostring(old), sig, p.mx, p.my))
        end
      end
    end
  end
end

-- Re-queue EVERYTHING we currently know first-hand for (re)broadcast — the
-- response to an ally's /info kwq resync query. Drains over idle ticks. Uses
-- each object's own last_seen tick so newest-wins stays correct on receivers.
function M.kw_queue_all(world)
  world._kw_shared = world._kw_shared or {}
  world._kw_dirty  = world._kw_dirty  or {}
  for id, b in pairs(world.bases) do
    local sig = kw_sig(b, false)
    if sig and b.last_seen then
      world._kw_shared["b" .. id] = sig
      world._kw_dirty["b" .. id]  = { kind = "b", id = id, mx = b.mx, my = b.my, cls = sig, tick = b.last_seen }
    end
  end
  for id, p in pairs(world.pills) do
    -- Dead pills share as neutral too (see collect_kw_changes), so a resync
    -- corrects a respawned ally's stale hostile view of a since-killed pill.
    local ci = ((p.health or 0) <= 0) and "n0" or kw_sig(p, true)
    if ci and p.last_seen then
      -- Mirror collect_kw_changes' position-aware detect sig for deployed pills.
      local sig = (ci:sub(2, 2) == "0") and (ci .. "@" .. p.mx .. "," .. p.my) or ci
      world._kw_shared["p" .. id] = sig
      world._kw_dirty["p" .. id]  = { kind = "p", id = id, mx = p.mx, my = p.my,
                                      cls = ci:sub(1, 1), intank = ci:sub(2, 2), tick = p.last_seen }
    end
  end
end

-- base: b<id>:<mx>:<my>:<cls>:<tick>   pill: p<id>:<mx>:<my>:<cls>:<intank>:<tick>
local function kw_rec_token(r)
  if r.kind == "b" then
    return string.format("b%d:%d:%d:%s:%d", r.id, r.mx, r.my, r.cls, r.tick)
  end
  return string.format("p%d:%d:%d:%s:%s:%d", r.id, r.mx, r.my, r.cls, r.intank, r.tick)
end

-- Drain world._kw_dirty into ONE "/info kw ..." message under the wire cap
-- (PACKET_MAX_CHAT_MESSAGE = 128). Removes the entries it packs. Returns the
-- message string, or nil when there's nothing to send.
function M.build_kw_message(world)
  local dirty = world._kw_dirty
  if not dirty then return nil end
  local PREFIX, BUDGET = "/info kw ", 120
  local toks, len, drained = {}, #PREFIX, {}
  for key, r in pairs(dirty) do
    local tok = kw_rec_token(r)
    local add = #tok + (#toks > 0 and 1 or 0)   -- +1 for the joining comma
    if len + add > BUDGET then break end
    toks[#toks + 1]    = tok
    drained[#drained + 1] = key
    len = len + add
  end
  if #toks == 0 then return nil end
  for _, key in ipairs(drained) do dirty[key] = nil end
  return PREFIX .. table.concat(toks, ",")
end

-- Merge ally-shared known-world records into world.bases/world.pills.
-- Newest observed-tick wins; never overrides a first-hand update made THIS
-- tick (its last_seen==now already beats any tick<=now). Adopted objects are
-- marked _kw_ally=now so collect_kw_changes won't relay them. Never-seen
-- objects get a minimal entry (health defaulted — influence needs only
-- owner+location; targeting evaluators re-confirm health on first real sight).
function M.sync_ally_world(world, recs, now)
  for _, r in ipairs(recs) do
    local owner = r.cls and KW_CHAR_OWNER[r.cls]
    local from  = r.from or "?"
    if owner and r.id and r.tick and r.mx and r.my then
      if r.kind == "b" then
        local b = world.bases[r.id]
        if b == nil then
          b = { mx = r.mx, my = r.my, health = (owner == "neutral") and 0 or 1,
                owner = owner, last_seen = r.tick, last_health = 0,
                _kw_ally = now, _ally_only = true }
          world.bases[r.id] = b
          world.base_at[mkey(r.mx, r.my)] = { id = r.id, base = b }
          print2(string.format("KW_MERGE t=%d b%d NEW %s@%d from p%s", now, r.id, r.cls, r.tick, tostring(from)))
        elseif r.tick > (b.last_seen or -1) then
          local oc = KW_CLS_CHAR[b.owner] or "?"
          if b.mx ~= r.mx or b.my ~= r.my then
            world.base_at[mkey(b.mx, b.my)] = nil
            b.mx, b.my = r.mx, r.my
            world.base_at[mkey(r.mx, r.my)] = { id = r.id, base = b }
          end
          print2(string.format("KW_MERGE t=%d b%d ADOPT %s@%d->%s@%d from p%s", now, r.id, oc, b.last_seen or -1, r.cls, r.tick, tostring(from)))
          b.owner, b.last_seen, b._kw_ally = owner, r.tick, now
        else
          print2(string.format("KW_MERGE t=%d b%d SKIP in@%d <= local@%d(%s) from p%s", now, r.id, r.tick, b.last_seen or -1, KW_CLS_CHAR[b.owner] or "?", tostring(from)))
        end
      elseif r.kind == "p" then
        local intank = (r.intank == 1 or r.intank == "1")
        local p = world.pills[r.id]
        if p == nil then
          p = { mx = r.mx, my = r.my, health = C.PILLS_MAX_HEALTH or 15,
                owner = owner, anger = 0, anger_tick = 0, last_hit_tick = 0,
                last_seen = r.tick, in_tank = intank,
                under_attack = false, attack_tick = 0, attack_damage = 0,
                _kw_ally = now, _ally_only = true }
          world.pills[r.id] = p
          pill_index_add(world, r.id, p)
          print2(string.format("KW_MERGE t=%d p%d NEW %s%s@%d from p%s", now, r.id, r.cls, intank and "T" or "", r.tick, tostring(from)))
        elseif r.tick > (p.last_seen or -1) then
          local oc = KW_CLS_CHAR[p.owner] or "?"
          if p.mx ~= r.mx or p.my ~= r.my then
            pill_index_remove(world, r.id, p.mx, p.my)
            p.mx, p.my = r.mx, r.my
            pill_index_add(world, r.id, p)
          end
          print2(string.format("KW_MERGE t=%d p%d ADOPT %s@%d->%s%s@%d from p%s", now, r.id, oc, p.last_seen or -1, r.cls, intank and "T" or "", r.tick, tostring(from)))
          p.owner, p.in_tank, p.last_seen, p._kw_ally = owner, intank, r.tick, now
        else
          print2(string.format("KW_MERGE t=%d p%d SKIP in@%d <= local@%d(%s) from p%s", now, r.id, r.tick, p.last_seen or -1, KW_CLS_CHAR[p.owner] or "?", tostring(from)))
        end
      end
    end
  end
end

return M
