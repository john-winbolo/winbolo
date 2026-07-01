local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/perception.lua — shared per-tick perception cache
--
-- Computes a snapshot of commonly-queried world state once per tick at the
-- start of Brain.think().  Downstream systems (goals, builder, steering)
-- read from state.perc instead of re-scanning world.pills, world.bases,
-- and info.objects independently.
-- =========================================================================

local C        = require("constants")
local U        = require("util")
local cpf      = require("cpathfinder")
local danger   = require("danger")
local threat   = require("threat")
local kill_lgm = require("kill_lgm")
local print2   = require("print2")

local M = {}

-- -------------------------------------------------------------------------
-- M.update(state, world, info)
-- Call once per tick, before goal selection / builder / steering.
-- Populates state.perc with the current perception snapshot.
-- -------------------------------------------------------------------------
function M.update(state, world, info)
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local now = state.tick

  local perc = {}

  -- ----- Pill threats: hostile/neutral pills within firing range -----
  local pill_threats = {}
  local nearest_hostile_pill = nil
  local nearest_hostile_pill_dist = math.huge
  local friendly_pills_damaged = 0
  local friendly_pill_count = 0
  local dead_neutral_pill_count = 0
  local neutral_pill_count = 0
  local hostile_pill_count = 0
  local attackable_pill_count = 0  -- hostile or neutral with health > 0
  local allied_pill_count = 0      -- teammates' live pills (not mine, not a threat/target)

  for id, p in pairs(world.pills) do
    if p.owner == "friendly" then
      friendly_pill_count = friendly_pill_count + 1
      if p.health == 0 and not p.in_tank then
        dead_neutral_pill_count = dead_neutral_pill_count + 1
      elseif p.health > 0 and p.health < C.PILLS_MAX_HEALTH then
        friendly_pills_damaged = friendly_pills_damaged + 1
      end
    elseif p.owner == "allied" then
      -- A teammate's CARRIED (in-tank) pill — deployed team pills are shared
      -- and already classify as "friendly". This is in his tank: count it
      -- toward the team total (reposition few-pills guard) but never treat it
      -- as a threat or attack target, and don't try to manage it.
      if p.health > 0 then allied_pill_count = allied_pill_count + 1 end
    else
      -- hostile or neutral
      if p.health == 0 and p.owner == "neutral" and not p.in_tank then
        dead_neutral_pill_count = dead_neutral_pill_count + 1
      elseif p.health > 0 then
        attackable_pill_count = attackable_pill_count + 1
        if p.owner == "hostile" then
          hostile_pill_count = hostile_pill_count + 1
        elseif p.owner == "neutral" then
          neutral_pill_count = neutral_pill_count + 1
        end
        local d = U.mdist(tmx, tmy, p.mx, p.my)
        -- Track nearest hostile/neutral pill (any distance)
        if d < nearest_hostile_pill_dist then
          nearest_hostile_pill_dist = d
          nearest_hostile_pill = { pill = p, id = id, dist = d }
        end
        -- Collect pills within firing range (both hostile AND neutral fire at the tank)
        if d <= C.PILL_RANGE_MAP then
          pill_threats[#pill_threats + 1] = {
            pill = p, id = id, dist = d, anger = p.anger or 0,
          }
        end
      end
    end
  end

  -- Find friendly pills under attack (for defend-under-attack response)
  local worst_attack_pill = nil
  local worst_attack_damage = 0
  for id, p in pairs(world.pills) do
    if p.owner == "friendly" and p.under_attack and p.health > 0 then
      local dmg = p.attack_damage or 0
      if dmg > worst_attack_damage then
        worst_attack_damage = dmg
        worst_attack_pill = { id = id, mx = p.mx, my = p.my, damage = dmg, health = p.health }
      end
    end
  end
  perc.pill_under_attack = worst_attack_pill

  perc.nearest_hostile_pill = nearest_hostile_pill
  perc.pill_threats = pill_threats
  perc.friendly_pill_count = friendly_pill_count
  perc.friendly_pills_damaged = friendly_pills_damaged
  perc.dead_neutral_pill_count = dead_neutral_pill_count
  perc.neutral_pill_count = neutral_pill_count
  perc.hostile_pill_count = hostile_pill_count
  perc.attackable_pill_count = attackable_pill_count
  perc.allied_pill_count = allied_pill_count

  -- ----- Hostile tanks from info.objects (speed from C ObjectInfo) -----
  -- Velocity tracking: match tanks frame-to-frame by proximity to compute
  -- true velocity (WU per tick) for lead-time aiming.
  local prev_tanks = state._prev_enemy_tanks or {}
  local nearest_hostile_tank = nil
  local nearest_hostile_tank_dist = math.huge
  local enemy_tank_count = 0
  local allied_tank_count = 0
  local enemy_tanks = {}  -- all visible hostile tanks

  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_TANK and (bit.band(ob.info, OBJECT_HOSTILE)) == 0 then
      allied_tank_count = allied_tank_count + 1
    end
    if ob.type == OBJECT_TANK and (bit.band(ob.info, OBJECT_HOSTILE)) ~= 0 then
      enemy_tank_count = enemy_tank_count + 1
      local omx = bit.rshift(ob.x, 8)
      local omy = bit.rshift(ob.y, 8)
      local d = U.mdist(tmx, tmy, omx, omy)

      -- Match to closest previous-frame tank, then compute velocity by
      -- averaging position delta over the last N ticks of history. Raw
      -- single-tick delta is too noisy: WU positions quantize to
      -- engine-tick resolution, so a tank moving smoothly NE at ~11
      -- wu/tick reports per-tick deltas like (8,0), (0,-8), (8,-8) —
      -- prediction flips between N / E / NE every tick and lead even
      -- toggles off at the magnitude≤8 stationary cutoff. Averaging over
      -- 3 ticks smooths the quantization and keeps magnitude consistent
      -- without the EMA trail-off of past direction changes.
      local HIST_LEN = 3
      local vx, vy = 0, 0
      local svx, svy = 0, 0
      local best_match_d = 5 * 256
      local matched_pt = nil
      for _, pt in ipairs(prev_tanks) do
        local dx = ob.x - pt.wx
        local dy = ob.y - pt.wy
        local md = math.abs(dx) + math.abs(dy)
        if md < best_match_d then
          best_match_d = md
          matched_pt = pt
        end
      end
      -- Build new history newest-first, capped at HIST_LEN. Index 1 is
      -- the CURRENT tick's position; index N is N-1 ticks back.
      local hist = { { wx = ob.x, wy = ob.y } }
      if matched_pt and matched_pt.hist then
        for i = 1, math.min(HIST_LEN - 1, #matched_pt.hist) do
          hist[#hist + 1] = matched_pt.hist[i]
        end
      end
      -- Velocity from the oldest entry in the buffer back to current,
      -- divided by the number of ticks the span actually covers. Falls
      -- back to single-tick when only 2 entries are available (target
      -- just appeared / re-acquired).
      if #hist >= 2 then
        local oldest = hist[#hist]
        local n_ticks = #hist - 1
        vx = (ob.x - oldest.wx) / n_ticks
        vy = (ob.y - oldest.wy) / n_ticks
        -- Sanity-cap: a real tank can't exceed ~16 wu/tick. > 40 wu/tick
        -- means the matcher snapped to a different tank; reuse last
        -- tick's velocity instead of feeding garbage to lead-prediction.
        if vx * vx + vy * vy > 1600 then
          vx = (matched_pt and matched_pt.vx) or 0
          vy = (matched_pt and matched_pt.vy) or 0
        end
        svx, svy = vx, vy
      end

      local entry = { mx = omx, my = omy, dist = d, obj = ob,
                       id = ob.idnum,
                       speed = ob.speed or 0,
                       wx = ob.x, wy = ob.y, vx = vx, vy = vy,
                       svx = svx, svy = svy, hist = hist }
      enemy_tanks[#enemy_tanks + 1] = entry

      if d < nearest_hostile_tank_dist then
        nearest_hostile_tank_dist = d
        nearest_hostile_tank = entry
      end
    end
  end

  -- Save current positions + velocity + position history for next tick's
  -- computation. hist is the rolling 3-tick lookback used to smooth
  -- velocity against engine-tick position quantization.
  state._prev_enemy_tanks = {}
  for _, et in ipairs(enemy_tanks) do
    state._prev_enemy_tanks[#state._prev_enemy_tanks + 1] = {
      wx = et.wx, wy = et.wy,
      vx = et.vx, vy = et.vy,
      svx = et.svx, svy = et.svy,
      hist = et.hist,
    }
  end

  -- ── Ghost tank tracking ──────────────────────────────────────────────────
  -- Persist the last GHOST_TANK_HIST sightings (position + tick) per tank so
  -- that when one vanishes (forest, fog, LOS break) we can extrapolate its
  -- likely position from averaged velocity and keep it as a TARGETABLE ghost
  -- for GHOST_TANK_TTL_TICKS — flushing out players who duck into cover.
  -- (Done AFTER _prev_enemy_tanks is saved so ghosts never feed the per-tick
  -- velocity matcher. Ghosts go in a SEPARATE perc.ghost_tanks list — NOT
  -- perc.enemy_tanks — so the ~30 consumers that read enemy_tanks (threat,
  -- crossfire, disengage, refuel urgency, opportunistic fire, …) only ever
  -- see REAL sightings. Only the hunting paths (eval_attack_tank + steering
  -- target acquisition) merge in ghosts.)
  local ghost_tanks = {}
  local track = state._tank_track or {}
  local visible_ids = {}
  for _, et in ipairs(enemy_tanks) do
    if et.id ~= nil then
      visible_ids[et.id] = true
      local tr = track[et.id]
      if not tr then tr = { hist = {} }; track[et.id] = tr end
      tr.last_tick = now
      tr.wx, tr.wy = et.wx, et.wy
      tr.speed = et.speed
      tr.dir = et.obj and et.obj.direction or tr.dir
      table.insert(tr.hist, 1, { wx = et.wx, wy = et.wy, tick = now })
      while #tr.hist > (C.GHOST_TANK_HIST or 10) do tr.hist[#tr.hist] = nil end
      -- Average velocity (wu/tick) over the buffered span = "last known speed
      -- + rotation averaged over the last N ticks".
      if #tr.hist >= 2 then
        local oldest = tr.hist[#tr.hist]
        local span = now - oldest.tick
        if span > 0 then
          tr.vx = (et.wx - oldest.wx) / span
          tr.vy = (et.wy - oldest.wy) / span
        end
      end
    end
  end
  local ghost_ttl = C.GHOST_TANK_TTL_TICKS or 100
  local dead_at = state.tank_dead_at
  -- If ANY tank died this tick, flush ALL ghosts. Ghosts are a feature — we keep
  -- firing at a tank that fled out of sight (into trees) — but a dead tank's
  -- ghost must not draw fire. A death stamp can't always be matched to the right
  -- track id (extrapolation drift / id reuse), so rather than risk shooting a
  -- corpse's lingering ghost we drop EVERY out-of-sight track on any death and
  -- re-acquire from fresh real sightings. Visible tanks are re-added below from
  -- this tick's objects, so only ghosts are lost (they rebuild on next sighting).
  local death_this_tick = false
  if dead_at then
    for _, dt in pairs(dead_at) do
      if dt == now then death_this_tick = true; break end
    end
  end
  for id, tr in pairs(track) do
    local age = now - (tr.last_tick or now)
    -- KILLED (not merely occluded): a tank we have a death stamp for, dated at or
    -- after we last saw it, is gone — NOT hiding in cover. Drop the track so it
    -- never becomes a targetable ghost; otherwise the hunting/steering paths keep
    -- firing at its extrapolated ghost for the whole TTL (~2 s) after the kill.
    local killed = dead_at and dead_at[id] and dead_at[id] >= (tr.last_tick or 0)
    if visible_ids[id] then
      -- seen this tick — the real entry is already in enemy_tanks
    elseif killed or death_this_tick then
      track[id] = nil  -- dead, or a tank died this tick → flush ghost immediately
    elseif age > 0 and age <= ghost_ttl then
      local gwx = (tr.wx or 0) + (tr.vx or 0) * age
      local gwy = (tr.wy or 0) + (tr.vy or 0) * age
      local gmx = math.floor(gwx / 256)
      local gmy = math.floor(gwy / 256)
      if gmx < 0 then gmx = 0 elseif gmx > 255 then gmx = 255 end
      if gmy < 0 then gmy = 0 elseif gmy > 255 then gmy = 255 end
      -- Reality check: a ghost is a GUESS at where an out-of-sight tank is. The
      -- engine only HIDES a tank when it's FULLY in trees — center AND all 4
      -- cardinal neighbours forest (utilIsTankInTrees). So a ghost is only
      -- plausible at such a tile; anywhere else (open ground, or just the tree
      -- LINE with 1-3 forest neighbours) a real tank would be visible — it
      -- isn't, so it's gone (dead or moved off): drop the track instead of
      -- firing at empty ground. (Was: ANY one forest neighbour kept it, which
      -- wrongly retained ghosts on the tree line where the tank is still in
      -- plain sight.) Also clears the post-death phantom with no distance math.
      local in_cover =
            U.ttype(gmx, gmy) == C.T_FOREST
        and U.in_map(gmx - 1, gmy) and U.ttype(gmx - 1, gmy) == C.T_FOREST
        and U.in_map(gmx + 1, gmy) and U.ttype(gmx + 1, gmy) == C.T_FOREST
        and U.in_map(gmx, gmy - 1) and U.ttype(gmx, gmy - 1) == C.T_FOREST
        and U.in_map(gmx, gmy + 1) and U.ttype(gmx, gmy + 1) == C.T_FOREST
      if not in_cover then
        track[id] = nil
      else
        ghost_tanks[#ghost_tanks + 1] = {
          mx = gmx, my = gmy, dist = U.mdist(tmx, tmy, gmx, gmy),
          obj = nil, id = id, speed = tr.speed or 0,
          wx = gwx, wy = gwy, vx = tr.vx or 0, vy = tr.vy or 0,
          svx = tr.vx or 0, svy = tr.vy or 0, hist = nil,
          ghost = true, ghost_age = age, ghost_ttl_left = ghost_ttl - age,
          dir = tr.dir,
        }
      end
    elseif age > ghost_ttl then
      track[id] = nil  -- ghost expired
    end
  end
  state._tank_track = track

  perc.nearest_hostile_tank = nearest_hostile_tank
  perc.enemy_tank_count = enemy_tank_count
  perc.enemy_tanks = enemy_tanks
  perc.ghost_tanks = ghost_tanks

  -- ----- Enemy LGM tracking: detect parachutes (dead enemy LGM) -----
  local enemy_lgm_sightings = state._enemy_lgm_sightings or {}
  -- (was: redundant `local now = state.tick or 0` — outer `now` from
  -- line 25 is in scope and identical when state.tick is set.)
  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_PARACHUTE and (bit.band(ob.info, OBJECT_HOSTILE)) ~= 0 then
      local omx = bit.rshift(ob.x, 8)
      local omy = bit.rshift(ob.y, 8)
      enemy_lgm_sightings[U.mkey(omx, omy)] = {
        tick = now, mx = omx, my = omy,
      }
      -- Clear kill_lgm history near this death point so a fast
      -- respawn doesn't inherit the previous life's samples/dest.
      kill_lgm.purge_killed(state, ob.x, ob.y)
    end
  end
  -- enemy_lgm_dead: TRUE while any parachute sighting is within the
  -- ENEMY_LGM_RETURN_TICKS window (i.e. an enemy LGM was seen
  -- parachuting recently and is presumed still respawning). This is
  -- NOT a "currently dead" check — we never see the LGM resurrect,
  -- so the flag is "saw a parachute in the last N ticks". A live LGM
  -- being spotted again does NOT clear the flag; the flag clears only
  -- when ENEMY_LGM_RETURN_TICKS elapses since the last sighting.
  -- Consumers (notably eLGMdead discount on attack_pill cost) should
  -- treat this as "LGM probably can't repair right now" not "definitely
  -- dead".
  local enemy_lgm_dead = false
  local enemy_lgm_eta = math.huge
  for k, s in pairs(enemy_lgm_sightings) do
    local return_tick = s.tick + C.ENEMY_LGM_RETURN_TICKS
    if now < return_tick then
      enemy_lgm_dead = true
      if return_tick < enemy_lgm_eta then enemy_lgm_eta = return_tick end
    else
      enemy_lgm_sightings[k] = nil  -- expired
    end
  end
  state._enemy_lgm_sightings = enemy_lgm_sightings
  perc.enemy_lgm_dead = enemy_lgm_dead
  perc.enemy_lgm_return_tick = enemy_lgm_dead and enemy_lgm_eta or nil

  -- Base Killer Mode: auto-activate when team outnumbers opponents
  -- Count includes self (+1 for our tank)
  perc.allied_tank_count = allied_tank_count + 1  -- +1 = us
  perc.team_advantage = (allied_tank_count + 1) - enemy_tank_count
  perc.base_killer_mode = perc.team_advantage >= C.BASE_KILLER_TEAM_ADVANTAGE
      and enemy_tank_count > 0  -- need at least 1 enemy to make sense

  -- Dead pills on deep sea (bait detection). Piggyback on U.terrain_prev:
  -- every U.ttype/U.traw call from pathfinding/steering/threat populates it,
  -- so we reuse that shared cache instead of running a dedicated view scan.
  -- Unseen tiles are absent (nil), so == C.T_DEEPSEA won't false-flag.
  local terrain_prev = U.terrain_prev
  perc.deepsea_pill_ids = {}
  for pid, p in pairs(world.pills) do
    if p.health == 0 and terrain_prev[U.mkey(p.mx, p.my)] == C.T_DEEPSEA then
      perc.deepsea_pill_ids[pid] = true
    end
  end

  -- LGM-impassable tiles for the reach sim. The bot's brain map (pf->map) is
  -- PURE TERRAIN — it carries no pill/base overlay — so the LGM travel sim
  -- can't see either on its own, and the bot would march its LGM straight into
  -- one and the build never starts. Mirror the engine's mapGetManSpeed here:
  --   * LIVE pills of ANY owner block (MAP_MANSPEED_TPILLBOX = 0); dead/carried
  --     pills don't (dead = passable rubble, carried = off-map).
  --   * Enemy bases block (basesCantDrive: non-ally, non-neutral, armour above
  --     capture). The brain only sees hostile-base armour fogged to 1 alive / 0
  --     capturable, so health>0 = "above capture". Alliance-checked off
  --     owner_player since owner_str lumps ally bases in with "hostile".
  -- Rebuilt every tick before goal selection's reachability checks run.
  do
    local allies  = info.allies or 0
    local self_pn = info.player_number
    local blocked = {}
    for _, p in pairs(world.pills) do
      if (p.health or 0) > 0 and not p.in_tank then
        blocked[#blocked + 1] = { p.mx, p.my }
      end
    end
    for _, b in pairs(world.bases) do
      local op = b.owner_player
      if op and op ~= NEUTRAL_PLAYER and op ~= self_pn
         and (bit.band(allies, (bit.lshift(1, op)))) == 0
         and (b.health or 0) > (C.BASE_MIN_ARMOUR_CAPTURE or 0) then
        blocked[#blocked + 1] = { b.mx, b.my }
      end
    end
    cpf.set_lgm_blocked(blocked)
  end

  -- Friendly-fire repair guard: stamp any friendly pill a friendly shot (own OR
  -- ally) is sitting on / right next to, so eval_repair_pill won't heal a pill
  -- the team is shooting down to reposition. Observing the actual bullet is more
  -- reliable than the repos broadcast (no latency) AND covers our OWN shots, so
  -- we don't heal the pill we were just about to move. Own shots classify
  -- friendly (OBJECT_HOSTILE==0), same as shot_tracker relies on. cheb<=1
  -- window absorbs fast shells skipping a tile between snapshot ticks.
  local fshot_tiles = nil
  for _, ob in ipairs(info.objects) do
    if ob.type == 1 and (bit.band(ob.info, OBJECT_HOSTILE)) == 0 then  -- 1 = OBJECT_SHOT
      fshot_tiles = fshot_tiles or {}
      fshot_tiles[U.mkey(bit.rshift(ob.x, 8), bit.rshift(ob.y, 8))] = true
    end
  end
  if fshot_tiles then
    for _, p in pairs(world.pills) do
      if p.owner == "friendly" and p.health > 0 and not p.in_tank then
        for ddy = -1, 1 do
          for ddx = -1, 1 do
            local nx, ny = p.mx + ddx, p.my + ddy
            if nx >= 0 and nx <= 255 and ny >= 0 and ny <= 255
               and fshot_tiles[U.mkey(nx, ny)] then
              p._friendly_shot_tick = now
            end
          end
        end
      end
    end
  end

  -- ----- Allied LGM protection (aIndy: avoid driving over allied LGMs) -----
  -- ----- Enemy LGM tracking (live sightings in 15x15 view) --------------
  -- Both passes share one scan over info.objects.  Enemy LGMs are
  -- frame-to-frame proximity-matched against last tick's sightings
  -- (LGMs move ~3 wu/tick, so a 1-tile match radius is tight).  We
  -- also try to associate each enemy LGM with the nearest enemy tank
  -- by idnum on first sighting — useful both for "this LGM came out
  -- of tank K" attribution and for predicting where it's heading.
  local allied_lgm_positions = {}
  local enemy_lgms = {}
  local prev_enemy_lgms = state._prev_enemy_lgms or {}
  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_BUILDMAN then
      local lmx = bit.rshift(ob.x, 8)
      local lmy = bit.rshift(ob.y, 8)
      -- Owner classification trace: one line per LGM the brain actually
      -- received this tick. OBJECT_HOSTILE comes straight from the C scan
      -- (players.c playersGetBrainLgmsInRect: hostile unless allied). An LGM
      -- that's our OWN (self-skipped in C), tree-hidden >3 tiles out, or
      -- otherwise not in info.objects will simply never log here — its absence
      -- is the answer to "why didn't the bot see whose LGM that is".
      if (bit.band(ob.info, OBJECT_HOSTILE)) == 0 then
        allied_lgm_positions[#allied_lgm_positions + 1] = { mx = lmx, my = lmy }
      else
        -- Match to last tick's nearest enemy LGM (by wu distance) so
        -- we keep stable identity / velocity even when ObjectInfo
        -- idnum changes across frames.
        local best_match, best_d = nil, 6 * 256  -- 6 wu tolerance
        for _, pl in ipairs(prev_enemy_lgms) do
          local md = math.abs(ob.x - pl.wx) + math.abs(ob.y - pl.wy)
          if md < best_d then best_d = md; best_match = pl end
        end
        local vx, vy = 0, 0
        local seen_since = now
        local near_tank_idnum = nil
        if best_match then
          vx = ob.x - best_match.wx
          vy = ob.y - best_match.wy
          seen_since = best_match.seen_since
          near_tank_idnum = best_match.near_tank_idnum
        end
        -- First sighting: associate with the nearest visible enemy tank.
        if near_tank_idnum == nil then
          local best_t_d = 4 * 256  -- 4 tile tolerance
          for _, et in ipairs(enemy_tanks) do
            local md = math.abs(et.wx - ob.x) + math.abs(et.wy - ob.y)
            if md < best_t_d then best_t_d = md; near_tank_idnum = et.id end
          end
        end
        local _ent = {
          mx = lmx, my = lmy,
          wx = ob.x, wy = ob.y,
          vx = vx, vy = vy,
          idnum = ob.idnum,
          seen_since = seen_since,
          near_tank_idnum = near_tank_idnum,
          dist = U.mdist(tmx, tmy, lmx, lmy),
        }
        -- Lead-predict for kill_lgm targeting.  EMA-smoothed velocity +
        -- convergence loop (D ↔ flight_ticks) produces the aim point and
        -- the gunrange we need to drive the crosshair to.  Used by both
        -- steering (heading lead) and init.lua's fire block (gunrange
        -- key + fire trigger).
        local v_ex, v_ey = kill_lgm.update_velocity(state, _ent, now)
        _ent.v_ema_x = v_ex
        _ent.v_ema_y = v_ey
        -- Pull dest off the history record so steering/viz/etc. can
        -- read it straight from the lgm entry.
        local h = state._enemy_lgm_history and state._enemy_lgm_history[_ent.idnum]
        if h then
          _ent.dest_wx     = h.dest_wx
          _ent.dest_wy     = h.dest_wy
          _ent.dest_locked = h.dest_locked
          _ent.lock_status = h.lock_status
        end
        local aim_wx, aim_wy, sl, ft, d_wu, tier = kill_lgm.predict_aim(
          info.tankx, info.tanky, _ent, h)
        _ent.predicted_wx     = aim_wx
        _ent.predicted_wy     = aim_wy
        _ent.predicted_mx     = bit.rshift(math.floor(aim_wx), 8)
        _ent.predicted_my     = bit.rshift(math.floor(aim_wy), 8)
        _ent.target_sightLen  = sl
        _ent.flight_ticks     = ft
        _ent.predicted_dist_wu = d_wu
        _ent.predict_tier     = tier
        enemy_lgms[#enemy_lgms + 1] = _ent
      end
    end
  end
  state._prev_enemy_lgms = enemy_lgms
  perc.allied_lgm_positions = allied_lgm_positions
  perc.enemy_lgms = enemy_lgms
  kill_lgm.purge_stale(state, now, enemy_lgms)

  -- ----- Under fire: shell danger or angry pill in range -----
  local threat_at_tank = danger.danger_at(tmx, tmy, now, world)
  perc.threat_at_tank = threat_at_tank

  local under_fire = false
  local fire_source_mx, fire_source_my = nil, nil

  if threat_at_tank > 0 then
    under_fire = true
    -- Find dominant threat source: closest angry pill, or shell direction
    local worst_threat = 0
    for _, pt in ipairs(pill_threats) do
      if pt.anger > 0.3 then
        local threat_val = C.PILL_DANGER_BASE + pt.anger * C.PILL_DANGER_ANGER
        if threat_val > worst_threat then
          worst_threat = threat_val
          fire_source_mx = pt.pill.mx
          fire_source_my = pt.pill.my
        end
      end
    end
  end

  perc.under_fire = under_fire
  perc.fire_source_mx = fire_source_mx
  perc.fire_source_my = fire_source_my

  -- ----- Base supply: what the nearby friendly base offers -----
  if info.base then
    perc.base_supply = {
      armour = info.base.armour or 0,
      shells = info.base.shells or 0,
      mines  = info.base.mines or 0,
    }
    -- Store observed stock on the world.bases entry so goal selection can
    -- skip low-stock bases when choosing where to refuel.
    local bmx = info.base.x
    local bmy = info.base.y
    for id, b in pairs(world.bases) do
      if b.mx == bmx and b.my == bmy then
        b.obs_shells = info.base.shells or 0
        b.obs_armour = info.base.armour or 0
        b.obs_tick   = now
        break
      end
    end
  else
    perc.base_supply = nil
  end

  -- ----- Base counts by owner (for quick-skip in goal selection) -----
  local friendly_base_count = 0
  local neutral_base_count = 0
  local hostile_base_count = 0
  local capturable_hostile_base_count = 0
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" then
      friendly_base_count = friendly_base_count + 1
    elseif b.owner == "neutral" then
      neutral_base_count = neutral_base_count + 1
    elseif b.owner == "hostile" then
      hostile_base_count = hostile_base_count + 1
      if b.health == 0 then
        capturable_hostile_base_count = capturable_hostile_base_count + 1
      end
    end
  end
  perc.friendly_base_count = friendly_base_count
  perc.neutral_base_count = neutral_base_count
  perc.hostile_base_count = hostile_base_count
  perc.capturable_hostile_base_count = capturable_hostile_base_count

  -- ----- Nearest forest within gather radius of tank -----
  local best_forest_d = math.huge
  local best_forest_x, best_forest_y = nil, nil
  local radius = C.FARM_GATHER_RADIUS
  for dy = -radius, radius do
    for dx = -radius, radius do
      local fx, fy = tmx + dx, tmy + dy
      if U.in_map(fx, fy) and U.ttype(fx, fy) == C.T_FOREST then
        local d = U.mdist(tmx, tmy, fx, fy)
        if d < best_forest_d then
          best_forest_d = d
          best_forest_x = fx
          best_forest_y = fy
        end
      end
    end
  end

  if best_forest_x then
    perc.nearest_forest = { mx = best_forest_x, my = best_forest_y, dist = best_forest_d }
  else
    perc.nearest_forest = nil
  end

  state.perc = perc
end

return M
