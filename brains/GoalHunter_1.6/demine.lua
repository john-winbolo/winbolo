local bit = require('bitcompat')
-- =========================================================================
-- demine.lua — automatic battle-damage handling: mine clearing + terrain
-- repair (mines take priority when both exist):
--
--   kill_mine      — shoot a known mine off OUR territory (tank's gun).
--                    Push/pop goal interrupt: the tank stops and aims.
--   terrain repair — pave a mine crater / rubble / crater-flood water tile
--                    with a road. NOT a goal: a parallel LGM job
--                    (state._trepair_job), fire-and-forget like farming —
--                    the tank carries on with its current goal while the
--                    LGM walks out and paves. builder.lua dispatches from
--                    the job; this module owns the job lifecycle (paved /
--                    timeout / enemy-near / tank-left-it-behind).
--
-- When a KNOWN mine (TERRAIN_MINE_FLAG set on the brain map) sits within
-- crosshair range of the tank and the current goal is interruptible, PUSH a
-- synthetic kill_mine goal on top of it: the real goal object is stashed
-- UNTOUCHED (substate, timers, scan results, all context) on
-- state._demine_saved. Steering then stops the tank and drives the
-- crosshair — heading AND gunsight length — RIGHT ONTO the mine tile: a
-- shell only detonates a mine when it ENDS on the mined square (shells.c
-- calls minesExpAddItem at both shell-death paths — collision and
-- range-expiry — never mid-flight), exactly like landing a shot on an LGM.
-- The moment the mine flag clears, POP: state.goal = the saved object and
-- the old goal resumes where it left off.
--
-- Interruptible = every goal EXCEPT combat/urgency kinds (kill_lgm,
-- attack_tank, flee/escape/rescue/wait) and any attack_pill substate other
-- than the get-into-position phases (plan_position / approach). The
-- selector holds a pushed kill_mine against replans (init.lua arbitration
-- chain) with flee_to_base as the only preempt.
--
-- Target choice: mines behind the tank cost more, proportionally cheaper
-- the closer the mine's bearing is to the current heading:
--   cost = dist_wu * (1 + DEMINE_BEHIND_MULT * (|heading - bearing| / 128))
-- =========================================================================
local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local viz    = require("viz")
local print2 = require("print2")

local M = {}

-- Goal kinds that must never be interrupted for a mine, and during which a
-- terrain-repair job must not START (combat/urgency — the LGM stays home;
-- an already-running job just keeps going, builder priorities gate it).
local DENY_KINDS = {
  kill_lgm = true, attack_tank = true, kill_mine = true,
  escape_water = true, flee_pill = true, flee_to_base = true,
  rescue_lgm = true, wait_for_lgm = true,
  pill_place = true, place_pill_strategic = true,
}
-- Terrain the repair interrupt paves: mine craters, rubble, and the water
-- a crater floods into. NOT swamp — the opportunistic road-ahead already
-- handles slow terrain on the path; this is damage repair, not paving.
local REPAIR_TT = nil  -- built lazily (constants table indexed by tt)
local function repair_tt()
  if not REPAIR_TT then
    REPAIR_TT = { [C.T_CRATER] = true, [C.T_RUBBLE] = true, [C.T_RIVER] = true }
  end
  return REPAIR_TT
end
-- attack_pill: only the get-into-position phases may be interrupted.
local PILL_OK_SUB = { plan_position = true, approach = true }

-- Shared push/start eligibility (goal-shape only — the mine push adds its
-- own DEMINE_ENABLE + shells gates; the repair job needs neither).
local function eligible(state, info)
  if state.command_goal then return false end
  if info.inboat then return false end
  local g = state.goal
  if not g or DENY_KINDS[g.kind] then return false end
  if g.kind == "attack_pill" and not PILL_OK_SUB[g.substate or ""] then
    return false
  end
  return true
end

-- Best mine in crosshair range, or nil. Raw-terrain box scan around the
-- tank; runs on the DEMINE_SCAN_PERIOD cadence so the ~800 get_terrain
-- reads amortize to noise.
local function find_target(state, world, info)
  local now = state.tick or 0
  local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
  local R      = math.floor((C.GUNSIGHT_MAX or 14) / 2)   -- crosshair reach, tiles
  local min_wu = C.DEMINE_MIN_DIST_WU or 512              -- don't blast our own feet
  local max_wu = R * 256
  local cd     = state._demine_cooldown
  local lgm_out = info.man_status ~= C.LGM_INTANK and info.man_x and info.man_y
  local best_mx, best_my, best_cost
  -- Debug viz collector: every considered mine tile + its verdict, cached on
  -- state for draw_overlay to render every tick (scan runs on a cadence).
  local dbg = BRAIN_DEBUG_MODE and { tick = now, tmx = tmx, tmy = tmy,
                                     range = R, min_wu = min_wu, cands = {} } or nil
  for dy = -R, R do
    for dx = -R, R do
      if dx ~= 0 or dy ~= 0 then
        local mx2, my2 = tmx + dx, tmy + dy
        -- OUR influence only: clearing lanes in friendly territory is worth
        -- shells; a mine in contested/enemy ground is theirs to live with
        -- (and shelling it advertises our position for nothing).
        if U.in_map(mx2, my2)
           and (bit.band(U.traw(mx2, my2), TERRAIN_MINE_FLAG)) ~= 0 then
          local infl = cpf.influence_at(mx2, my2) or 0
          local k = my2 * 256 + mx2
          local wx = bit.bor((bit.lshift(mx2, 8)), 128)
          local wy = bit.bor((bit.lshift(my2, 8)), 128)
          local ddx, ddy = wx - info.tankx, wy - info.tanky
          local dist = math.sqrt(ddx * ddx + ddy * ddy)
          local rej, cost = nil, nil
          if infl <= 0 then rej = "not our ground"
          elseif cd and cd[k] and cd[k] > now then rej = "cooldown"
          elseif dist < min_wu then rej = "too close"
          elseif dist > max_wu then rej = "out of reach"
          elseif lgm_out and U.wdist(info.man_x, info.man_y, wx, wy) < 512 then
            rej = "our LGM near"
          else
            local bearing = U.aim_at(info.tankx, info.tanky, wx, wy)
            local adiff   = math.abs(U.adiff(info.direction, bearing))
            cost = dist * (1.0 + (C.DEMINE_BEHIND_MULT or 2.0) * (adiff / 128.0))
            if not best_cost or cost < best_cost then
              best_cost, best_mx, best_my = cost, mx2, my2
            end
          end
          if dbg then dbg.cands[#dbg.cands + 1] = { mx = mx2, my = my2, cost = cost, rej = rej } end
        end
      end
    end
  end
  if dbg then dbg.best_mx, dbg.best_my, dbg.best_cost = best_mx, best_my, best_cost; state._demine_viz = dbg end
  if not best_mx then return nil end
  -- Only push for a mine we can actually land a shell on from here.
  -- (Lazy require: steering requires other modules but never demine,
  -- so this cannot cycle.)
  local steer = require("steering")
  local wx = bit.bor((bit.lshift(best_mx, 8)), 128)
  local wy = bit.bor((bit.lshift(best_my, 8)), 128)
  if not steer.shot_path_clear(info, world, wx, wy, best_mx, best_my) then
    -- Blocked line: cool the tile down so the scan doesn't re-pick it
    -- every pass; a later scan from elsewhere may see it clear.
    state._demine_cooldown = state._demine_cooldown or {}
    state._demine_cooldown[best_my * 256 + best_mx] = now + 150
    -- Re-tag the (former) winner as blocked in the viz so the overlay shows
    -- WHY the cheapest mine wasn't chosen.
    if dbg then
      for _, c in ipairs(dbg.cands) do
        if c.mx == best_mx and c.my == best_my then c.rej = "shot blocked"; c.cost = nil end
      end
      dbg.best_mx, dbg.best_my, dbg.best_cost = nil, nil, nil
    end
    return nil
  end
  return best_mx, best_my, best_cost
end

-- ── Terrain repair (mine craters / rubble / crater-flood water) ───────────
-- Parallel LGM job, NOT a goal: state._trepair_job is set here, builder.lua
-- dispatches the LGM to pave the tile with a road while the tank carries on
-- with its goal, and the job retires when the tile is paved (backstops in
-- M.update).

-- A hostile tank nearby makes an LGM walk a gift to the enemy.
local function repair_threatened(state, info)
  local perc = state.perc
  if not (perc and perc.enemy_tanks) then return false end
  local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
  local r = C.TREPAIR_ENEMY_RANGE or 10
  for _, et in ipairs(perc.enemy_tanks) do
    local dx, dy = et.mx - tmx, et.my - tmy
    if dx * dx + dy * dy <= r * r then return true end
  end
  return false
end

-- Nearest repairable tile within TREPAIR_RADIUS, or nil.
local function find_repair_target(state, world, info)
  local now = state.tick or 0
  if info.man_status ~= C.LGM_INTANK then return nil end
  if repair_threatened(state, info) then return nil end
  local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
  local R  = C.TREPAIR_RADIUS or 5
  local cd = state._trepair_cooldown
  local rt = repair_tt()
  local best_mx, best_my, best_d2, best_cost
  local dbg = BRAIN_DEBUG_MODE and { tick = now, tmx = tmx, tmy = tmy,
                                     range = R, cands = {} } or nil
  for dy = -R, R do
    for dx = -R, R do
      local mx2, my2 = tmx + dx, tmy + dy
      if U.in_map(mx2, my2) then
        local tt = U.ttype(mx2, my2)
        if rt[tt] and C.ROAD_BUILD_TERRAIN[tt] then
          local infl = cpf.influence_at(mx2, my2) or 0
          local k = my2 * 256 + mx2
          local d2 = dx * dx + dy * dy
          local cost = C.ROAD_BUILD_TERRAIN[tt]
          local rej = nil
          -- Water: only bridge tiles touching land (a crater flooded at the
          -- shore), never open water in the middle of a river run.
          local ok = true
          if tt == C.T_RIVER then
            ok = false
            if U.in_map(mx2 - 1, my2) and not U.is_water(U.ttype(mx2 - 1, my2)) then ok = true end
            if not ok and U.in_map(mx2 + 1, my2) and not U.is_water(U.ttype(mx2 + 1, my2)) then ok = true end
            if not ok and U.in_map(mx2, my2 - 1) and not U.is_water(U.ttype(mx2, my2 - 1)) then ok = true end
            if not ok and U.in_map(mx2, my2 + 1) and not U.is_water(U.ttype(mx2, my2 + 1)) then ok = true end
          end
          if infl <= 0 then rej = "not our ground"
          elseif cd and cd[k] and cd[k] > now then rej = "cooldown"
          elseif not ok then rej = "open water"
          elseif mx2 == tmx and my2 == tmy then rej = "under tank"
          -- Dynamic reserve (lazy require; builder never requires demine, so
          -- no cycle): terrain repair is a luxury road too — don't pave with
          -- trees an imminent pill place/repair needs.
          elseif (info.trees or 0) < cost + require("builder").road_tree_reserve(state, world, info) then rej = "low trees"
          else
            if not best_d2 or d2 < best_d2 then
              best_d2, best_mx, best_my, best_cost = d2, mx2, my2, cost
            end
          end
          if dbg then dbg.cands[#dbg.cands + 1] = { mx = mx2, my = my2, tt = tt, cost = cost, rej = rej } end
        end
      end
    end
  end
  if dbg then dbg.best_mx, dbg.best_my = best_mx, best_my; state._trepair_viz = dbg end
  if not best_mx then return nil end
  -- The LGM must be able to walk there (bless the dest — the target tile
  -- itself is the unwalkable thing we're fixing) and the walk must be safe.
  local reach = cpf_lgm_travel_ticks_map(tmx, tmy, best_mx, best_my, best_mx, best_my, 2000, 150) ~= -1
  local safe  = reach and require("danger").lgm_path_safe_enhanced(info, best_mx, best_my,
                                                     C.LGM_DANGER_LOW, now, world)
  if not (reach and safe) then
    state._trepair_cooldown = state._trepair_cooldown or {}
    state._trepair_cooldown[best_my * 256 + best_mx] = now + 300
    if dbg then
      for _, c in ipairs(dbg.cands) do
        if c.mx == best_mx and c.my == best_my then
          c.rej = reach and "unsafe LGM walk" or "LGM unreachable"
        end
      end
      dbg.best_mx, dbg.best_my = nil, nil
    end
    return nil
  end
  return best_mx, best_my, best_cost
end

-- Per-tick update. Call AFTER goal selection (init.lua think loop):
-- retires a finished/stale terrain-repair job, pops a finished/stale
-- kill_mine back to the saved goal, or — when a shootable mine / repairable
-- tile exists over an interruptible goal — pushes a kill_mine goal (mines
-- first) or starts a parallel repair job.
function M.update(state, world, info)
  local now = state.tick or 0
  local g = state.goal

  -- ── Parallel terrain-repair job lifecycle (independent of the goal) ──
  -- The job is fire-and-forget: builder.lua dispatches the LGM from it
  -- while the tank carries on. Here we just retire it — paved, timed out,
  -- an enemy tank got close, or the tank drove off while the LGM never
  -- managed to start (unsafe walk / no trees / builder never had a slot).
  local job = state._trepair_job
  if job then
    local paved      = not repair_tt()[U.ttype(job.mx, job.my)]
    local timeout    = (now - (job.start or now)) > (C.TREPAIR_MAX_TICKS or 400)
    local threatened = repair_threatened(state, info)
    local left_behind = info.man_status == C.LGM_INTANK
      and U.mdist(bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8), job.mx, job.my)
          > (C.TREPAIR_ABANDON_DIST or 6)
    if paved or timeout or threatened or left_behind then
      if not paved then
        state._trepair_cooldown = state._trepair_cooldown or {}
        state._trepair_cooldown[job.my * 256 + job.mx] = now + 500
      end
      state._trepair_job = nil
      print2(string.format("TREPAIR_DONE t=%d tile@(%d,%d) %s age=%d",
        now, job.mx, job.my,
        paved and "PAVED" or (threatened and "enemy-near"
          or (left_behind and "left-behind" or "timeout")),
        now - (job.start or now)))
    end
  end

  -- ── Active kill_mine: pop when cleared / stale / dry ──────────────
  if g and g.kind == "kill_mine" then
    local cleared = (bit.band(U.traw(g.mx, g.my), TERRAIN_MINE_FLAG)) == 0
    local timeout = (now - (g._push_tick or now)) > (C.DEMINE_MAX_TICKS or 150)
    local dry     = (info.shells or 0) <= 0
    if cleared or timeout or dry then
      if not cleared then
        -- Give up on this tile for a while (blocked geometry / out of
        -- shells) so we don't immediately re-push the same mine.
        state._demine_cooldown = state._demine_cooldown or {}
        state._demine_cooldown[g.my * 256 + g.mx] = now + 500
      end
      local saved = state._demine_saved
      state._demine_saved = nil
      state.goal = saved or { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
      state.pf.status = "idle"   -- resume with a fresh path step
      print2(string.format("DEMINE_POP t=%d mine@(%d,%d) %s -> resume %s%s",
        now, g.mx, g.my,
        cleared and "CLEARED" or (dry and "out-of-shells" or "timeout"),
        state.goal.kind,
        state.goal.substate and ("/" .. tostring(state.goal.substate)) or ""))
    end
    return
  end

  -- ── No interrupt active: consider a mine push / repair job start ────
  if not eligible(state, info) then return end
  if (now - (state._demine_scan_tick or -1e9)) < (C.DEMINE_SCAN_PERIOD or 5) then
    return
  end
  state._demine_scan_tick = now
  if C.DEMINE_ENABLE ~= false
     and (info.shells or 0) >= (C.DEMINE_MIN_SHELLS or 3) then
    local mx, my, cost = find_target(state, world, info)
    if mx then
      state._demine_saved = state.goal
      state.goal = { kind = "kill_mine", mx = mx, my = my,
                     wx = bit.bor((bit.lshift(mx, 8)), 128), wy = bit.bor((bit.lshift(my, 8)), 128),
                     _push_tick = now }
      print2(string.format("DEMINE_PUSH t=%d mine@(%d,%d) cost=%.0f over %s%s shells=%d",
        now, mx, my, cost or -1, state._demine_saved.kind,
        state._demine_saved.substate and ("/" .. tostring(state._demine_saved.substate)) or "",
        info.shells or 0))
      return
    end
  end

  -- No mine to shoot: consider a parallel repair job for battle damage
  -- (crater/rubble/flood water). NOT a goal — the tank carries on with
  -- what it's doing; builder.lua dispatches the LGM from the job.
  -- capture_base is a RACE: no tile repairs while driving to take a base
  -- (the LGM dispatch paces the tank down and delays the grab).
  if C.TREPAIR_ENABLE == false or state._trepair_job then return end
  if state.goal and state.goal.kind == "capture_base" then return end
  local rx, ry, rcost = find_repair_target(state, world, info)
  if not rx then return end
  state._trepair_job = { mx = rx, my = ry, start = now, tree_cost = rcost }
  print2(string.format("TREPAIR_JOB t=%d tile@(%d,%d) tt=%d cost=%d during %s%s trees=%d",
    now, rx, ry, U.ttype(rx, ry), rcost or -1, g.kind,
    g.substate and ("/" .. tostring(g.substate)) or "",
    info.trees or 0))
end

-- =========================================================================
-- Debug overlays. Call every tick from init.lua (BRAIN_DEBUG_MODE only).
-- Renders the cached scan data (find_target / find_repair_target stash it on
-- the scan cadence) so what you see is exactly what the scorer decided.
--   demine_scan  — mine clear: crosshair range ring, each considered mine
--                  (green=chosen, yellow=candidate+cost, red=rejected+reason),
--                  min-dist "no-blast" ring, live shot info on the active kill.
--   trepair_scan — terrain repair: LGM radius ring, each crater/rubble/water
--                  tile (green=chosen, yellow=valid, red=rejected+reason),
--                  the active repair target + LGM link.
-- =========================================================================
function M.draw_overlay(state, world, info)
  if not BRAIN_DEBUG_MODE then return end
  local now = state.tick or 0

  -- ── Mine clear ─────────────────────────────────────────────────────
  if viz.is_on("demine_scan") then
    local v = state._demine_viz
    if v and (now - (v.tick or 0)) <= (C.DEMINE_SCAN_PERIOD or 5) + 2 then
      local tcx, tcy = v.tmx + 0.5, v.tmy + 0.5
      -- Crosshair-reach ring (max) + min-blast ring.
      viz.circle("demine_scan", tcx, tcy, v.range, 120, 200, 255, 60, false, false)
      viz.circle("demine_scan", tcx, tcy, (v.min_wu or 512) / 256.0, 255, 120, 120, 80, false, false)
      for _, c in ipairs(v.cands or {}) do
        local win = v.best_mx and c.mx == v.best_mx and c.my == v.best_my
        local r, g, b = 230, 70, 70                         -- red = rejected
        if win then r, g, b = 60, 230, 60                   -- green = chosen
        elseif c.cost then r, g, b = 235, 220, 70 end       -- yellow = candidate
        viz.rect("demine_scan", c.mx, c.my, c.mx + 1, c.my + 1, r, g, b, win and 180 or 100, true)
        local lbl = c.rej or (c.cost and string.format("%.0f", c.cost)) or "?"
        viz.text("demine_scan", c.mx + 0.5, c.my + 0.5, lbl, "center", 255, 255, 255, 220, 0.4)
      end
    end
    -- Active kill_mine: target ring + live gunsight/land info.
    local g = state.goal
    if g and g.kind == "kill_mine" then
      local mcx, mcy = g.mx + 0.5, g.my + 0.5
      viz.circle("demine_scan", mcx, mcy, 0.55, 255, 60, 60, 255, false, false)
      viz.line("demine_scan", info.tankx / 256.0, info.tanky / 256.0, mcx, mcy, 255, 90, 90, 200)
      local dist = U.wdist(info.tankx, info.tanky, g.wx, g.wy)
      viz.text("demine_scan", mcx, mcy - 0.75,
        string.format("KILL MINE d=%.1ft sl=%d age=%d", dist / 256.0,
                      info.gunrange or 0, now - (g._push_tick or now)),
        "center", 255, 120, 120, 255)
    end
  end

  -- ── Terrain repair ─────────────────────────────────────────────────
  if viz.is_on("trepair_scan") then
    local v = state._trepair_viz
    if v and (now - (v.tick or 0)) <= (C.DEMINE_SCAN_PERIOD or 5) + 2 then
      viz.circle("trepair_scan", v.tmx + 0.5, v.tmy + 0.5, v.range, 120, 255, 160, 60, false, false)
      for _, c in ipairs(v.cands or {}) do
        local win = v.best_mx and c.mx == v.best_mx and c.my == v.best_my
        local r, g, b = 230, 70, 70
        if win then r, g, b = 60, 230, 60
        elseif not c.rej then r, g, b = 235, 220, 70 end
        viz.rect("trepair_scan", c.mx, c.my, c.mx + 1, c.my + 1, r, g, b, win and 170 or 95, true)
        local lbl = c.rej or string.format("road c=%d", c.cost or 0)
        viz.text("trepair_scan", c.mx + 0.5, c.my + 0.5, lbl, "center", 255, 255, 255, 210, 0.4)
      end
    end
    local j = state._trepair_job
    if j then
      local rcx, rcy = j.mx + 0.5, j.my + 0.5
      viz.circle("trepair_scan", rcx, rcy, 0.55, 60, 230, 120, 255, false, false)
      viz.line("trepair_scan", info.tankx / 256.0, info.tanky / 256.0, rcx, rcy, 80, 230, 120, 200)
      -- LGM link if it's out working.
      if info.man_status ~= C.LGM_INTANK and info.man_x then
        viz.line("trepair_scan", info.man_x / 256.0, info.man_y / 256.0, rcx, rcy, 120, 255, 160, 220)
      end
      viz.text("trepair_scan", rcx, rcy - 0.75,
        string.format("REPAIR tt=%d cost=%d age=%d", U.ttype(j.mx, j.my),
                      j.tree_cost or 0, now - (j.start or now)),
        "center", 120, 255, 160, 255)
    end
  end
end

return M
