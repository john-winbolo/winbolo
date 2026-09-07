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

-- k-th newest sighting in a tank-track ring (k = 1 is the newest, k = tr.h_n
-- the oldest retained). Caller guarantees 1 <= k <= tr.h_n. Lets the ghost
-- velocity averaging read the oldest slot without the per-tick front-insert +
-- trim a plain list needs.
local function track_hist_at(tr, k)
  return tr.ring[((tr.h_head - k) % tr.cap) + 1]
end

-- -------------------------------------------------------------------------
-- Damage-source attribution: who is actually shooting our pill?
--
-- Pillboxes only ever fire at TANKS (pillbox.c pillsUpdate), so a shell that
-- lands on a team PILL from a pillbox is a MISS aimed at somebody else. A
-- tank shelling our pill is deliberate and will finish the job; a pillbox
-- stray is noise. Scariness order, and the whole reason this exists:
--     tank fire  >  enemy pill fire  >  neutral pill fire
-- (20260901_160325_1_par2 bot3 t=18080: our 13/15-hp pill #4 read as
-- "taking_damage" and preempted a six-pill free capture, when the damage was
-- stray fire from NEUTRAL pill #8 shooting at our own tank.)
--
-- SRC_RANK ranks the classes so the worst evidence in the window wins.
local SRC_RANK = { tank = 3, epill = 2, npill = 1 }

-- =========================================================================
-- ALARM MODE (2026-09-06) — the build STAMP, computed ONCE here at module
-- load and never rebuilt.  Andrew's mechanism, verbatim: condition 1 puts a
-- pill on a WATCH LIST, and "every tile within 4 tiles of a watched pill is
-- covered by a precomputed stamp ... and watched for builds".
--
-- Flat {dx1,dy1,dx2,dy2,...} so the per-tick sweep is one numeric loop with
-- no table allocation.  EUCLIDEAN disc (dx^2+dy^2 <= R^2), matching the metric
-- the defend evaluator uses everywhere else — 49 tiles at R=4.
--
-- Only read while C.DEFEND_ALARM_MODE is true; building it costs one loop at
-- require time either way, and nothing downstream sees it when the knob is off.
-- =========================================================================
local ALARM_STAMP = {}
local ALARM_STAMP_N = 0
do
  local R = C.DEFEND_ALARM_BUILD_RADIUS or 4
  for dy = -R, R do
    for dx = -R, R do
      if dx * dx + dy * dy <= R * R then
        ALARM_STAMP[ALARM_STAMP_N + 1] = dx
        ALARM_STAMP[ALARM_STAMP_N + 2] = dy
        ALARM_STAMP_N = ALARM_STAMP_N + 2
      end
    end
  end
end
-- Snapshot sentinel for "a deployed HOSTILE pill stands on this tile".  Kept
-- out of the 0..13 terrain range so one number per tile carries both facts.
local ALARM_HOSTILE_PILL = 100
-- Terrain values that mean SOMEBODY BUILT HERE.  T_BUILDING is a finished
-- wall, T_HALFBUILD is one going up (the LGM's first pass) — trigger 2b fires
-- on either, because the point is to catch the build while it is happening.
local ALARM_BUILT_TT = { [C.T_BUILDING] = "wall", [C.T_HALFBUILD] = "halfwall" }
-- Cap on the per-pill enemy-hit ring.  A 15-hp pill cannot absorb more than
-- 15 hits before it dies, so this can never truncate a live count.
local ALARM_HIT_RING = 24

-- Classify ONE visible shell by its muzzle. A shell flies in a straight line,
-- so walk its direction BACKWARDS and see whether a live hostile/neutral
-- pillbox centre sits on (or within PILL_SRC_ORIGIN_SLOP_WU of) that back-ray,
-- no further back than a shell can fly (SHELL_MAX_STEPS x SHELL_SPEED =
-- 2048 wu = the pillbox's own range).
--
-- Returns "epill" / "npill" (the OWNER class of the pillbox that fired — an
-- enemy player's pillbox labels its shells HOSTILE exactly like a tank does,
-- so the shell's own label cannot answer this) plus that pill's id, or
-- "tank", nil when nothing on the back-ray explains it.
--
-- Deterministic: the NEAREST muzzle wins and ties break on the lower pill id,
-- so pairs() order never reaches the answer.
local function shell_source_class(world, ob)
  local ux =  U.bsin(ob.direction) / 128
  local uy = -U.bcos(ob.direction) / 128
  local maxr  = (C.SHELL_MAX_STEPS or 64) * (C.SHELL_SPEED or 32)
  local slop  = C.PILL_SRC_ORIGIN_SLOP_WU or 384
  local slop2 = slop * slop
  local best_id, best_t, best_owner = nil, nil, nil
  for pid, p in pairs(world.pills) do
    if (p.owner == "hostile" or p.owner == "neutral")
       and (p.health or 0) > 0 and not p.in_tank then
      local dx = U.m2w(p.mx) - ob.x
      local dy = U.m2w(p.my) - ob.y
      -- t = how far BACK along the flight path this pill lies. The origin is
      -- at shell - t*u, so t = -(d . u) and the perpendicular miss is d + t*u.
      local t = -(dx * ux + dy * uy)
      if t >= 0 and t <= maxr then
        local ex = dx + t * ux
        local ey = dy + t * uy
        if ex * ex + ey * ey <= slop2 then
          if best_t == nil or t < best_t
             or (t == best_t and pid < best_id) then
            best_id, best_t, best_owner = pid, t, p.owner
          end
        end
      end
    end
  end
  if best_owner == "hostile" then return "epill", best_id end
  if best_owner == "neutral" then return "npill", best_id end
  return "tank", nil
end
M.shell_source_class = shell_source_class

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
  -- Double-buffered: the list filled last tick is this tick's matcher input;
  -- refill the other. Entries are fresh per tick, so a consumer may hold an
  -- entry across ticks — but never the LIST, which is recycled every 2 ticks.
  state._et_buf_a = state._et_buf_a or {}
  state._et_buf_b = state._et_buf_b or {}
  local prev_tanks = state._prev_enemy_tanks or state._et_buf_b
  local enemy_tanks = (prev_tanks == state._et_buf_a) and state._et_buf_b
                                                       or state._et_buf_a
  -- A respawn nils _prev_enemy_tanks to force a clean matcher start (init.lua);
  -- the fallback buffer above may still hold entries from two ticks ago, so
  -- blank it whenever there was no real previous list.
  if state._prev_enemy_tanks == nil then
    for i = 1, #prev_tanks do prev_tanks[i] = nil end
  end
  local n_et = 0
  local nearest_hostile_tank = nil
  local nearest_hostile_tank_dist = math.huge
  local enemy_tank_count = 0
  local allied_tank_count = 0

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
      -- up to 3 ticks smooths the quantization and keeps magnitude
      -- consistent without the EMA trail-off of past direction changes.
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
      -- Position chain carried as scalars rather than a list: p1 is the matched
      -- tank's position last tick, p2 its position the tick before (nil when
      -- unavailable). Chained forward from the matched entry each tick, this
      -- looks back up to 2 ticks (3 positions including the current one).
      local p1x, p1y, p2x, p2y, p3x, p3y, p4x, p4y
      if matched_pt then
        p1x, p1y = matched_pt.wx,  matched_pt.wy
        p2x, p2y = matched_pt.p1x, matched_pt.p1y
        p3x, p3y = matched_pt.p2x, matched_pt.p2y
        p4x, p4y = matched_pt.p3x, matched_pt.p3y
      end
      -- Velocity from the oldest available position back to current, divided by
      -- the number of ticks that span covers. A SHORT window aliases badly: the
      -- engine advances a tank's integer WU position in a +16,+16,+16,+0 stutter
      -- (a real ~12 wu/tick tank), so a 2-tick delta flips between 16 and 8 every
      -- tick — and the 8 trips steering's "<=8 wu/tick = stationary" cutoff,
      -- zeroing the lead and loosing a NO-LEAD shot at a moving target (measured:
      -- that alias was the aim test's whole miss rate). Averaging over 4 ticks
      -- cancels the stutter to a rock-steady 12. Falls back to shorter spans
      -- right after (re)acquisition, when the deeper history isn't there yet.
      local ox, oy, n_ticks
      if p4x then
        ox, oy, n_ticks = p4x, p4y, 4
      elseif p3x then
        ox, oy, n_ticks = p3x, p3y, 3
      elseif p2x then
        ox, oy, n_ticks = p2x, p2y, 2
      elseif p1x then
        ox, oy, n_ticks = p1x, p1y, 1
      end
      if ox then
        vx = (ob.x - ox) / n_ticks
        vy = (ob.y - oy) / n_ticks
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
                       svx = svx, svy = svy,
                       p1x = p1x, p1y = p1y, p2x = p2x, p2y = p2y,
                       p3x = p3x, p3y = p3y }
      n_et = n_et + 1
      enemy_tanks[n_et] = entry

      if d < nearest_hostile_tank_dist then
        nearest_hostile_tank_dist = d
        nearest_hostile_tank = entry
      end
    end
  end

  -- Trim any entries left in the buffer by a longer previous tick, then hand
  -- this tick's list to next tick's velocity matcher as its "previous" input.
  -- The entries carry every field the matcher reads (wx/wy, vx/vy, p1/p2), so
  -- no snapshot copy is needed — the buffer pair keeps this list intact until
  -- recycled two ticks from now.
  for i = n_et + 1, #enemy_tanks do enemy_tanks[i] = nil end
  state._prev_enemy_tanks = enemy_tanks

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
      if not tr then
        tr = { ring = {}, h_head = 0, h_n = 0, cap = C.GHOST_TANK_HIST or 10 }
        track[et.id] = tr
      end
      tr.last_tick = now
      tr.wx, tr.wy = et.wx, et.wy
      tr.speed = et.speed
      tr.dir = et.obj and et.obj.direction or tr.dir
      -- Push this sighting into the fixed ring: advance the head (wrapping),
      -- reuse the slot table already there or create it once, and grow the
      -- valid count up to capacity. No front-insert shift, no trim loop, no
      -- per-tick table once the ring has filled.
      local head = (tr.h_head % tr.cap) + 1
      tr.h_head = head
      local slot = tr.ring[head]
      if not slot then slot = {}; tr.ring[head] = slot end
      slot.wx = et.wx; slot.wy = et.wy; slot.tick = now
      if tr.h_n < tr.cap then tr.h_n = tr.h_n + 1 end
      -- Average velocity (wu/tick) over the buffered span = "last known speed
      -- + rotation averaged over the last N ticks".
      if tr.h_n >= 2 then
        local oldest = track_hist_at(tr, tr.h_n)
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
  if BRAIN_DEBUG_MODE and death_this_tick then print2(string.format("GHOST_FLUSH t=%d a-tank-died-this-tick -> dropping all out-of-sight tracks", now)) end
  for id, tr in pairs(track) do
    local age = now - (tr.last_tick or now)
    if BRAIN_DEBUG_MODE and not visible_ids[id] then print2(string.format("GHOST_EVAL t=%d id=%s age=%d last_tick=%s dead_at=%s killed=%s death_tick=%s", now, tostring(id), age, tostring(tr.last_tick), tostring(dead_at and dead_at[id]), tostring(dead_at and dead_at[id] and dead_at[id] >= (tr.last_tick or 0)), tostring(death_this_tick))) end
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
          svx = tr.vx or 0, svy = tr.vy or 0,
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
  if BRAIN_DEBUG_MODE and #ghost_tanks > 0 then local _ids = {} for _, g in ipairs(ghost_tanks) do _ids[#_ids + 1] = string.format("%s@(%d,%d)a%d", tostring(g.id), g.mx, g.my, g.ghost_age or -1) end print2(string.format("GHOSTS t=%d count=%d [%s]", now, #ghost_tanks, table.concat(_ids, ","))) end

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
    if p.health == 0 then
      local k = U.mkey(p.mx, p.my)
      local at_sea = terrain_prev[k] == C.T_DEEPSEA
      -- The pill tile itself reads T_PILLBOX in the brain map (the pill
      -- overlay hides the terrain under it), so the direct test above only
      -- fires for a pill the cache saw BEFORE it landed there. A pill that
      -- was already in the water when we first looked never matched, and
      -- 20260831_222819 bot3 drove on foot at speed 64 onto dead pill #3 in
      -- the sea at the spawn and drowned. So also call it deep-sea when at
      -- least 3 of its 4 cardinal neighbours are deep sea: a pill afloat in
      -- open water (or on a 1-tile pedestal in it, which beaches a boat and
      -- strands a tank just the same).
      if not at_sea then
        -- Read the four cardinal neighbours DIRECTLY (U.ttype, which also
        -- fills terrain_prev) instead of trusting whatever the shared cache
        -- happened to hold. The cache only knows tiles some other subsystem
        -- has already looked at, so on a map where nothing has pathed near the
        -- water the pill was never flagged at all — and the sea-pill harvest
        -- branch of capture_pill is keyed entirely off this flag. Four
        -- get_terrain reads per DEAD pill per tick is noise.
        local n, seen = 0, 0
        for _, d in ipairs({ {1,0}, {-1,0}, {0,1}, {0,-1} }) do
          local nx, ny = p.mx + d[1], p.my + d[2]
          if U.in_map(nx, ny) then
            local t = U.ttype(nx, ny)
            seen = seen + 1
            -- Another dead pill of the same raft reads T_PILLBOX (the overlay
            -- hides the terrain under it) — count it as water too, or a tight
            -- 3-pill cluster hides its own neighbours from the test.
            if t == C.T_DEEPSEA then
              n = n + 1
            elseif t == C.T_PILLBOX then
              local op = world.pill_at and world.pill_at[ny * 256 + nx]
              if op then
                for _, oe in ipairs(op) do
                  if oe.pill and (oe.pill.health or 0) == 0 then n = n + 1 break end
                end
              end
            end
          end
        end
        at_sea = (seen >= 3 and n >= 3)
      end
      if at_sea then perc.deepsea_pill_ids[pid] = true end
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
  -- Double-buffered like enemy_tanks: last tick's list is this tick's matcher
  -- input; refill the other. Only the LIST is reused — entries stay fresh.
  state._elgm_buf_a = state._elgm_buf_a or {}
  state._elgm_buf_b = state._elgm_buf_b or {}
  local prev_enemy_lgms = state._prev_enemy_lgms or state._elgm_buf_b
  local enemy_lgms = (prev_enemy_lgms == state._elgm_buf_a) and state._elgm_buf_b
                                                             or state._elgm_buf_a
  local n_lgm = 0
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
      if BRAIN_DEBUG_MODE then print2(string.format("LGM_PERCEIVE t=%d idnum=%s tile=(%d,%d) hostile=%s -> %s", now, tostring(ob.idnum), lmx, lmy, tostring((bit.band(ob.info, OBJECT_HOSTILE)) ~= 0), ((bit.band(ob.info, OBJECT_HOSTILE)) == 0) and "ALLY/FRIENDLY" or "ENEMY")) end
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
        n_lgm = n_lgm + 1
        enemy_lgms[n_lgm] = _ent
      end
    end
  end
  -- Trim entries left by a longer previous tick, then hand this list to next
  -- tick's matcher as its "previous" input (the buffer pair keeps it intact).
  for i = n_lgm + 1, #enemy_lgms do enemy_lgms[i] = nil end
  state._prev_enemy_lgms = enemy_lgms
  perc.allied_lgm_positions = allied_lgm_positions
  perc.enemy_lgms = enemy_lgms
  kill_lgm.purge_stale(state, now, enemy_lgms)

  -- ----- Defend-signal stamps: enemy presence near team pills -----
  -- Stamped onto the pill records (world.pills) so goal evaluators read
  -- them as plain tick comparisons:
  --   _enemy_near_tick — hostile TANK seen within DEFEND_ENEMY_NEAR_RADIUS
  --   _lgm_near_tick   — hostile LGM  seen within DEFEND_LGM_NEAR_RADIUS
  --                      (attack setup: wall-shield building / pill plant)
  -- Deployed team pills only (a carried pill has no meaningful tile;
  -- deployed allied pills classify "friendly"). With team pill view these
  -- sightings arrive even when the pill is far from every teammate's tank.
  -- Chebyshev radius; cost is |enemies| x |pills| with early rejects.
  do
    local et = enemy_tanks
    local tank_r = C.DEFEND_ENEMY_NEAR_RADIUS or 10
    local lgm_r  = C.DEFEND_LGM_NEAR_RADIUS or 6
    if (et and #et > 0) or n_lgm > 0 then
      for _, p in pairs(world.pills) do
        if p.owner == "friendly"
           and not (p.in_tank or p.carrier or p._synth_carry) then
          if et then
            for i = 1, #et do
              local e = et[i]
              if math.abs(e.mx - p.mx) <= tank_r
                 and math.abs(e.my - p.my) <= tank_r then
                p._enemy_near_tick = now
                break
              end
            end
          end
          for i = 1, n_lgm do
            local e = enemy_lgms[i]
            if math.abs(e.mx - p.mx) <= lgm_r
               and math.abs(e.my - p.my) <= lgm_r then
              p._lgm_near_tick = now
              break
            end
          end
        end
      end
    end
  end

  -- ----- ALARM MODE watch list + build stamp (2026-09-06) -----
  -- Andrew's mechanism for defend_pill's alarm, verbatim: "condition 1 puts
  -- the pill on a WATCH LIST; every tile within 4 tiles of a watched pill is
  -- covered by a precomputed STAMP and watched for builds."
  --
  -- Runs EVERY tick (the evaluator runs only at replan cadence, and a wall can
  -- go up and be finished between two replans) -- but its EXPENSIVE half, the
  -- terrain sweep, is gated: see "The terrain sweep's gate" below.  The watch
  -- test and the hostile-LGM scan are both info.objects walks and always run;
  -- only the 49 U.ttype calls are conditional.  Writes three fields onto the
  -- pill record, all read back by goals.defend_alarm_status:
  --   _alarm_watch_tick  last tick a hostile tank was VISIBLE within
  --                      DEFEND_ALARM_ENEMY_TILES.  Condition 1 itself is
  --                      re-asked live by the evaluator; this is the watch
  --                      list's own bookkeeping.
  --   _alarm_lgm_tick    last tick a HOSTILE LGM (OBJECT_BUILDMAN with
  --                      OBJECT_HOSTILE, classified in the enemy-LGM pass
  --                      above) stood inside the stamp.  The hostile bit is
  --                      the WHOLE attribution for trigger 2b — Andrew,
  --                      2026-09-06: "if we know the LGM is an enemy, that's
  --                      sufficient."  No ally build-claim lookup gates it.
  --                      Doubles as the terrain sweep's gate.
  --   _alarm_build       the newest build seen inside the stamp:
  --                      { t, mx, my, what }.
  --
  -- The stamp snapshot (_alarm_terr) is PRIMED, not diffed, on the first
  -- watched tick and after any gap in watching: we only claim to know what a
  -- tile looked like while we were actually watching it, so a wall that went
  -- up while no enemy was near is never reported as a fresh build.
  --
  -- Terrain is read through U.ttype — the change DETECTOR, which is the
  -- correct reader for decision code (util.lua's header: ttype_peek is for
  -- debug/viz-gated code only, precisely so the recorded and production brains
  -- prime the same tiles).  This block is decision code, and it is skipped
  -- entirely when DEFEND_ALARM_MODE is false, so the keel path reads no
  -- terrain it did not read before.
  if C.DEFEND_ALARM_MODE then
    local win  = C.DEFEND_ALARM_WINDOW_TICKS or 250
    local er   = C.DEFEND_ALARM_ENEMY_TILES or 11
    local er2  = er * er
    local br   = C.DEFEND_ALARM_BUILD_RADIUS or 4
    local br2  = br * br
    -- Tile -> deployed HOSTILE pill, built once for the whole sweep.
    local hostile_tiles = nil
    for _, p in pairs(world.pills) do
      if (p.owner == "friendly" or p.owner == "allied")
         and not (p.in_tank or p.carrier or p._synth_carry) then
        -- Condition 1, on THIS tick's REAL sightings only.  enemy_tanks holds
        -- what the engine actually showed us this tick; ghosts live in a
        -- separate list and deliberately do not count — a remembered enemy is
        -- not "an enemy within 11 tiles".
        local watched = false
        for i = 1, #enemy_tanks do
          local e = enemy_tanks[i]
          local ddx, ddy = e.mx - p.mx, e.my - p.my
          if ddx * ddx + ddy * ddy <= er2 then watched = true; break end
        end
        if not watched then
          -- Off the watch list: keep the snapshot (cheap, bounded by the pill
          -- count) but mark it stale so the next sweep re-primes.
          p._alarm_watch_gap = true
        else
          p._alarm_watch_tick = now
          -- Hostile LGM inside the stamp — trigger 2b's attribution, and the
          -- gate on the terrain sweep below.  This scan runs EVERY watched
          -- tick and is NOT gated by anything: it walks the enemy-LGM list the
          -- pass above already built from info.objects, touches no terrain, and
          -- is what OPENS the gate — gating it on itself would close it
          -- forever.  Radius is the STAMP's 4 tiles, not 3: trigger 2b counts a
          -- build anywhere in the 4-tile stamp, so an LGM standing on the
          -- outer ring building the outer ring has to register.
          for i = 1, n_lgm do
            local e = enemy_lgms[i]
            local ddx, ddy = e.mx - p.mx, e.my - p.my
            if ddx * ddx + ddy * ddy <= br2 then
              p._alarm_lgm_tick = now
              p._alarm_lgm_mx, p._alarm_lgm_my = e.mx, e.my
              break
            end
          end
        end
        -- ── The terrain sweep's gate (2026-09-06, Andrew) ────────────────
        -- 49 U.ttype calls per watched pill per tick is the expensive half of
        -- this block, and almost all of it was wasted: trigger 2b does not
        -- fire on a build ALONE, it fires on a build WITH a hostile LGM seen
        -- in the same stamp inside the window.  So sweep only while that same
        -- freshness holds.  Same predicate, same radius, same window as
        -- goals.defend_alarm_status's `a.lgm_fresh` — evaluated at DETECTION
        -- time instead of only at evaluation time.
        --
        -- Note it is "seen within the window", not "seen this tick": an LGM
        -- that ducks out of sight leaves the gate open for the full 250 ticks,
        -- which is exactly as long as a build it left behind could still arm
        -- the trigger.  A build the sweep therefore skips is a build that could
        -- not have raised the alarm anyway.
        --
        -- No LGM fresh -> not one terrain read for this pill this tick, and the
        -- snapshot is marked stale so the resumed sweep PRIMES on its first
        -- tick and only diffs from the next — the same rule as the
        -- watching-gap case, so a change that happened while we were not
        -- looking is never reported as new.
        local lgm_fresh = watched and p._alarm_lgm_tick
                          and (now - p._alarm_lgm_tick) <= win
        if watched and not lgm_fresh then
          p._alarm_watch_gap = true
        elseif lgm_fresh then
          if hostile_tiles == nil then
            hostile_tiles = {}
            for _, q in pairs(world.pills) do
              if q.owner == "hostile" and (q.health or 0) > 0
                 and not (q.in_tank or q.carrier or q._synth_carry) then
                hostile_tiles[q.my * 256 + q.mx] = true
              end
            end
          end
          local snap = p._alarm_terr
          local prime = (snap == nil) or p._alarm_watch_gap
          if snap == nil then snap = {}; p._alarm_terr = snap end
          p._alarm_watch_gap = nil
          for i = 1, ALARM_STAMP_N, 2 do
            local sx = p.mx + ALARM_STAMP[i]
            local sy = p.my + ALARM_STAMP[i + 1]
            if U.in_map(sx, sy) then
              local key = sy * 256 + sx
              local cur = hostile_tiles[key] and ALARM_HOSTILE_PILL
                          or U.ttype(sx, sy)
              if not prime then
                local was = snap[key]
                if was ~= nil and was ~= cur then
                  local what = (cur == ALARM_HOSTILE_PILL) and "epill"
                               or ALARM_BUILT_TT[cur]
                  if what then
                    p._alarm_build = { t = now, mx = sx, my = sy, what = what }
                    print2(string.format(
                      "DEFEND_ALARM_WATCH t=%d pill@(%d,%d) BUILD (%d,%d) %s"
                      .. " (was tt=%d) lgm_seen=%s within r=%d win=%d",
                      now, p.mx, p.my, sx, sy, what, was,
                      p._alarm_lgm_tick
                        and string.format("%dt ago", now - p._alarm_lgm_tick)
                        or "never",
                      br, win))
                  end
                end
              end
              snap[key] = cur
            end
          end
        end
      end
    end
  end

  -- ----- Ally-heating shell watch -----
  -- The under-attack alarm should fire only for shells that are NOT
  -- ours/allies. Every tick, examine shells within HEAT_ALLY_SHELL_RADIUS
  -- of each deployed team pill — the engine labels every visible shell
  -- friendly / hostile / neutral (server truth via playersIsAllie; pill
  -- fire is neutral), and team pill view delivers shells near our pills
  -- at any range. Shells present and ALL friendly-labeled -> stamp
  -- _ally_heat_tick: world.lua suppresses the under_attack stamp within
  -- HEAT_SELF_STAMP_TICKS of it, so an ally's (or our own) heat tickle
  -- never reads as an enemy siege. ANY non-friendly shell nearby -> no
  -- stamp -> the alarm fires as usual.
  do
    local SHELL_FRIENDLY = 0  -- SHELLS_BRAIN_FRIENDLY (shells.h)
    local r = C.HEAT_ALLY_SHELL_RADIUS or 2
    local hits = nil  -- pill -> {friendly=n, other=n}, lazily built
    for _, ob in ipairs(info.objects) do
      if ob.type == OBJECT_SHOT then
        local smx = bit.rshift(ob.x, 8)
        local smy = bit.rshift(ob.y, 8)
        for _, p in pairs(world.pills) do
          if p.owner == "friendly"
             and not (p.in_tank or p.carrier or p._synth_carry)
             and math.abs(p.mx - smx) <= r and math.abs(p.my - smy) <= r then
            hits = hits or {}
            local h = hits[p]
            if not h then h = { friendly = 0, other = 0 }; hits[p] = h end
            if ob.info == SHELL_FRIENDLY then
              h.friendly = h.friendly + 1
            else
              h.other = h.other + 1
            end
          end
        end
      end
    end
    if hits then
      for p, h in pairs(hits) do
        if h.friendly > 0 and h.other == 0 then
          -- Log the transition (stale/absent -> fresh), not every tick of
          -- a volley, so the print stays greppable.
          if not p._ally_heat_tick or (now - p._ally_heat_tick) > 30 then
            print2(string.format(
              "ALLY_HEAT t=%d pill@(%d,%d) %d friendly shell(s) inbound, 0 hostile -> alarm suppressed",
              now, p.mx, p.my, h.friendly))
          end
          p._ally_heat_tick = now
        elseif h.other > 0 then
          print2(string.format(
            "ALLY_HEAT t=%d pill@(%d,%d) NOT ally-only (%d friendly, %d hostile/neutral) -> alarm live",
            now, p.mx, p.my, h.friendly, h.other))
        end
      end
    end
  end

  -- ----- Damage-source log: which shells came near which team pill -----
  -- Companion to the ally-heat watch above, but the opposite question. That
  -- one asks "is every shell here OURS" (suppress the alarm); this one asks,
  -- of the shells that are NOT ours, WHERE DID THEY COME FROM — a tank's gun
  -- or a pillbox's. The hp drop itself is reported a tick or more AFTER the
  -- shell object is gone, so the answer has to be banked while the shell is
  -- still on screen: a small per-pill ring of {tick, class} that the
  -- attribution pass below reads back inside PILL_SRC_SHELL_WINDOW.
  -- Only NON-friendly shells are logged and only near DEPLOYED team pills, so
  -- on a quiet map this loop does nothing at all.
  do
    local SHELL_FRIENDLY = 0  -- SHELLS_BRAIN_FRIENDLY (shells.h)
    local r   = C.PILL_SRC_SHELL_RADIUS or 3
    local win = C.PILL_SRC_SHELL_WINDOW or 90
    local cap = C.PILL_SRC_LOG_MAX or 8
    for _, ob in ipairs(info.objects) do
      if ob.type == OBJECT_SHOT and ob.info ~= SHELL_FRIENDLY then
        local smx = bit.rshift(ob.x, 8)
        local smy = bit.rshift(ob.y, 8)
        local cls, by = nil, nil   -- back-ray classified lazily: only pays off
                                   -- when a team pill is actually in the way
        for _, p in pairs(world.pills) do
          if (p.owner == "friendly" or p.owner == "allied")
             and not (p.in_tank or p.carrier or p._synth_carry)
             and math.abs(p.mx - smx) <= r and math.abs(p.my - smy) <= r then
            if cls == nil then cls, by = shell_source_class(world, ob) end
            local lg = p._src_log
            if not lg then lg = {}; p._src_log = lg end
            -- Compact out anything that has aged past the window, in place.
            local k = 0
            for i = 1, #lg do
              if now - lg[i].t <= win then k = k + 1; lg[k] = lg[i] end
            end
            for i = #lg, k + 1, -1 do lg[i] = nil end
            local last = lg[#lg]
            -- One entry per (tick, class): a volley of four identical strays
            -- must not push the tank sighting out of an 8-slot ring.
            if not (last and last.t == now and last.src == cls) then
              lg[#lg + 1] = { t = now, src = cls, by = by }
              if #lg > cap then table.remove(lg, 1) end
            end
          end
        end
      end
    end
  end

  -- ----- Attribute fresh damage on team pills to a source class -----
  -- world.lua stamps p.last_hit_tick on every REAL hp drop. Each new stamp is
  -- classified exactly once (_src_done_tick) into p.last_hit_src, which
  -- defend_pill_score reads as its scariness factor:
  --   1. observed shells — the SCARIEST class in the log inside the window
  --      wins (a tank joining in on top of pill strays is still tank fire);
  --   2. nothing seen at all (offscreen hit) — a fresh hostile-TANK sighting
  --      stamp at the pill (_enemy_near_tick) means tank fire;
  --   3. still nothing — the nearest live enemy/neutral pillbox whose range
  --      covers the pill takes the blame, at ITS owner class;
  --   4. nothing whatsoever — "tank", the conservative answer (defend keeps
  --      its full pre-change urgency when we simply do not know).
  do
    local win = C.PILL_SRC_SHELL_WINDOW or 90
    for _, p in pairs(world.pills) do
      if (p.owner == "friendly" or p.owner == "allied")
         and not (p.in_tank or p.carrier or p._synth_carry) then
        local hit = p.last_hit_tick or 0
        if hit > 0 and hit > (p._src_done_tick or 0) then
          p._src_done_tick = hit
          local src, how, via = nil, nil, nil
          local lg = p._src_log
          if lg then
            local rank = 0
            for i = 1, #lg do
              local e = lg[i]
              if (now - e.t) <= win then
                local rk = SRC_RANK[e.src] or 0
                if rk > rank then rank = rk; src = e.src; via = e.by end
              end
            end
            if src then how = "shell" end
          end
          if not src then
            local pw = C.PILL_SRC_PRESENCE_WINDOW or 300
            if p._enemy_near_tick and (now - p._enemy_near_tick) <= pw then
              src, how = "tank", "presence_tank"
            end
          end
          if not src then
            -- Nearest live enemy/neutral pill whose fire reaches this tile.
            -- Deterministic: nearest wins, ties on the lower id.
            local lim = C.PILLBOX_RANGE_WU or 2048
            local lim2 = lim * lim
            local bd2, bid, bowner = nil, nil, nil
            for pid, q in pairs(world.pills) do
              if (q.owner == "hostile" or q.owner == "neutral")
                 and (q.health or 0) > 0 and not q.in_tank then
                local dwx = U.m2w(q.mx) - U.m2w(p.mx)
                local dwy = U.m2w(q.my) - U.m2w(p.my)
                local d2 = dwx * dwx + dwy * dwy
                if d2 <= lim2
                   and (bd2 == nil or d2 < bd2 or (d2 == bd2 and pid < bid)) then
                  bd2, bid, bowner = d2, pid, q.owner
                end
              end
            end
            if bowner then
              src  = (bowner == "hostile") and "epill" or "npill"
              how, via = "presence_pill", bid
            end
          end
          if not src then src, how = "tank", "unknown" end
          p.last_hit_src      = src
          p.last_hit_src_tick = hit
          p.last_hit_src_how  = how
          p.last_hit_src_by   = via
          print2(string.format(
            "PILL_HIT_SRC t=%d pill@(%d,%d) hp=%d hit_t=%d src=%s via=%s by=%s",
            now, p.mx, p.my, p.health or 0, hit, src, how, tostring(via)))
          -- ALARM MODE trigger 2a (2026-09-06): a ring of ENEMY-attributed hit
          -- TICKS per pill.  Neither existing field can answer "how many times
          -- did an ENEMY hit this pill in the last 5 s": world.lua's p.hit_log
          -- counts every hp drop whatever caused it, and p.last_hit_src only
          -- remembers the classification of the LATEST one.  This is the only
          -- place a hit is ever attributed, so the ring is appended here.
          --   tank  — a hostile tank's aimed fire      -> enemy
          --   epill — a HOSTILE pillbox's stray        -> enemy ("from an
          --           enemy" covers enemy pills too, per the spec)
          --   npill — a NEUTRAL pillbox's stray        -> NOT an enemy, ignored
          -- Compacted to the window on write and capped, so it needs no
          -- per-tick sweep of its own.
          if C.DEFEND_ALARM_MODE and (src == "tank" or src == "epill") then
            local ring = p._alarm_hits
            if not ring then ring = {}; p._alarm_hits = ring end
            local win = C.DEFEND_ALARM_WINDOW_TICKS or 250
            local k = 0
            for i = 1, #ring do
              if now - ring[i].t <= win then k = k + 1; ring[k] = ring[i] end
            end
            for i = #ring, k + 1, -1 do ring[i] = nil end
            ring[#ring + 1] = { t = hit, src = src, how = how }
            if #ring > ALARM_HIT_RING then table.remove(ring, 1) end
          end
        end
      end
    end
  end

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
