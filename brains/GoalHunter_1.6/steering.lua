-- =========================================================================
-- GoalHunter/steering.lua — translate goal + pathfinder into holdkeys/tapkeys
-- =========================================================================

local C   = require("constants")
local U   = require("util")
local PF  = require("pathfinder")
local cpf = require("cpathfinder")
local log = require("logger")
local bpc = require("bpc")
local viz = require("viz")
local opt = require("optimize")
local threat = require("threat")
local print2 = require("print2")

local M = {}

-- Predictive "this shot will finish the kill" check used in attack_pill
-- firing substates. Run cpf.simulate_shot_angle against the current tank
-- pose; if the simulated trajectory hits the target pill tile AND
-- (_on_target_in_flight + 1) >= pill.health, enter swerve THIS same tick
-- instead of waiting for the next-tick shot_tracker tally. Saves ~1 brain
-- tick of standing still under return fire on the kill shot. The trade
-- the user explicitly accepted: if reality diverges (wall/tree/tank
-- crosses the shell path post-launch) the pill survives at 1 HP, vs.
-- eating an extra return shot.
local function predict_kill_shot_and_swerve(state, world, info, goal, pmx, pmy)
  -- DISABLED: we now keep firing until the pill is ACTUALLY dead (cyan 0/0)
  -- rather than peeling off on the in-flight kill prediction, which stopped a
  -- shot short whenever an in-flight shell diverged. The dead-pill swerve in
  -- attack.lua's charge/engage/shoot_pill handles the exit. No-op so the call
  -- sites don't need touching.
  return
end

-- Debug logging toggle — set via API: curl http://localhost:29016/steerdebug?on
M.debug = false

local function sdbg(fmt, ...)
  if M.debug then print("[STEER] " .. string.format(fmt, ...)) end
end

-- Local alias for the shared turn+speed helper in util.lua
local nav_turn_speed = U.nav_turn_speed

-- Shot-path safety check: simulate a shell from the tank toward
-- (target_wx, target_wy) and verify nothing dangerous is in the way.
-- Returns true if the path is clear. Blocks on:
--   walls, half-walls, hostile/neutral pillboxes, allied tanks,
--   hostile bases (nearby pills will start shooting).
-- Allows through: forests, enemy tanks, empty tiles.
-- target_mx/my is the tile we're aiming at (excluded from the check).
-- max_walls (default 0): how many walls the shot may cross and still count as
-- "clear". 0 = the legacy strict behavior (any wall blocks). >0 lets the path
-- cross that many walls (e.g. attack_base grinds 1 wall down) while STILL
-- blocking on pillboxes (any owner), bases, and allied tanks — anywhere on the
-- line, including beyond a within-budget wall.
-- src_wx/src_wy: optional shot origin (defaults to the tank). Lets callers test
--   whether a shot FROM SOME OTHER TILE would reach the target (e.g. scoring a
--   prospective engage point), not just from where the tank currently sits.
-- require_reach: when true, a shell that runs out of range before reaching the
--   target tile counts as NOT clear (otherwise an out-of-range shot with no
--   blocker in the lane reads as "clear" because nothing stopped it).
local function shot_path_clear(info, world, target_wx, target_wy, target_mx, target_my, max_walls, src_wx, src_wy, require_reach)
  max_walls = max_walls or 0
  local ox = src_wx or info.tankx
  local oy = src_wy or info.tanky
  local wall_count = 0
  local reached = false
  -- Build tanks array from visible objects for tank-aware simulation
  local tank_positions = {}
  if info.objects then
    for _, ob in ipairs(info.objects) do
      if ob.type == OBJECT_TANK then  -- 0; type 2 is OBJECT_PILLBOX, not a tank
        tank_positions[#tank_positions + 1] = {
          wx = ob.x, wy = ob.y,
          player_num = ob.idnum or 255,
          allied = (ob.info & OBJECT_HOSTILE) == 0,  -- friendly tank we must not shoot
        }
      end
    end
  end
  local tiles
  if #tank_positions > 0 then
    tiles = cpf.simulate_shot_with_tanks(ox, oy,
                                         target_wx, target_wy,
                                         cpf.SHOT_TANK, 0,
                                         tank_positions,
                                         info.player_number or 255)
  else
    tiles = cpf.simulate_shot(ox, oy,
                              target_wx, target_wy,
                              cpf.SHOT_TANK, 0)
  end
  if not tiles then return true end
  local origin_mx = ox >> 8
  local origin_my = oy >> 8
  local do_viz = BRAIN_DEBUG_MODE and viz.is_on("shell_hit_dot")
  local blocked = false
  local block_reason = nil
  local block_mx, block_my = nil, nil
  for ti, st in ipairs(tiles) do
    -- Tank hit entry from simulate_shot_with_tanks. Only an ALLIED tank
    -- blocks the shot — hitting an enemy tank (the target, or any other
    -- hostile that wanders into the lead-predicted lane) is a fine
    -- outcome, so we don't suppress fire for it. Tile-matching the hit
    -- against the aim tile was unreliable anyway: the aim tile is the
    -- lead-PREDICTED position while the hitbox sim reports the tank's
    -- CURRENT tile, so the same enemy read as a "blocker" one tile off.
    if st.hit_type and st.hit_type == 1 then
      local hit_tank = nil
      for _, tp in ipairs(tank_positions) do
        if tp.player_num == st.hit_id then hit_tank = tp; break end
      end
      local hit_ally = hit_tank and hit_tank.allied
      if do_viz and hit_tank then
        local tcx = hit_tank.wx / 256.0
        local tcy = hit_tank.wy / 256.0
        local hr = hit_ally and 255 or 0
        local hg = hit_ally and 0 or 255
        viz.rect("shell_hit_dot", tcx - 0.5, tcy - 0.5,
                 tcx + 0.5, tcy + 0.5, hr, hg, 0, 150)
        viz.text("shell_hit_dot", tcx, tcy - 0.6,
                 string.format("#%d tank#%d %s", ti, st.hit_id or 0,
                   hit_ally and "ALLY-BLOCK" or "enemy-ok"),
                 "center", hr, hg, 0, 255, 0.6)
      end
      if hit_ally then
        blocked = true
        block_reason = string.format("allied_tank#%d", st.hit_id or 0)
        block_mx, block_my = st.mx, st.my
      end
      break
    end
    if st.mx == target_mx and st.my == target_my then
      reached = true
      if do_viz then
        viz.rect("shell_hit_dot", st.mx + 0.1, st.my + 0.1,
                 st.mx + 0.9, st.my + 0.9, 0, 255, 0, 80)
        viz.text("shell_hit_dot", st.mx + 0.5, st.my + 0.5,
                 tostring(ti), "center", 0, 255, 0, 200, 0.5)
      end
      break
    end
    if st.mx ~= origin_mx or st.my ~= origin_my then
      local stt = U.ttype(st.mx, st.my)
      if stt == C.T_BUILDING or stt == C.T_HALFBUILD then
        wall_count = wall_count + 1
        if wall_count > max_walls then
          blocked = true
          block_reason = string.format("wall#%d", wall_count)
          block_mx, block_my = st.mx, st.my
          if do_viz then
            viz.text("shell_hit_dot", st.mx + 0.5, st.my + 0.5,
                     tostring(ti), "center", 255, 0, 0, 200, 0.5)
          end
          break
        end
        -- within wall budget: keep scanning past it for pills/bases/allies
      end
      local plist = world.pill_at and world.pill_at[st.my * 256 + st.mx]
      if plist then
        for _, e in ipairs(plist) do
          if e.pill and e.pill.health and e.pill.health > 0 then
            blocked = true
            block_reason = string.format("pill(hp=%d)", e.pill.health)
            block_mx, block_my = st.mx, st.my
            break
          end
        end
        if blocked then break end
      end
      local bentry = world.base_at and world.base_at[st.my * 256 + st.mx]
      if bentry and bentry.base then  -- a base of ANY owner stops the shell
        blocked = true
        block_reason = "base"
        block_mx, block_my = st.mx, st.my
        break
      end
    end
    if do_viz and not blocked then
      viz.rect("shell_hit_dot", st.mx + 0.2, st.my + 0.2,
               st.mx + 0.8, st.my + 0.8, 200, 200, 200, 40)
      viz.text("shell_hit_dot", st.mx + 0.5, st.my + 0.5,
               tostring(ti), "center", 200, 200, 200, 150, 0.4)
    end
  end
  if do_viz then
    if blocked then
      viz.rect("shell_hit_dot", block_mx + 0.05, block_my + 0.05,
               block_mx + 0.95, block_my + 0.95, 255, 0, 0, 150)
      viz.text("shell_hit_dot", block_mx + 0.5, block_my - 0.3,
               block_reason, "center", 255, 80, 80, 255, 0.6)
    end
    viz.line("shell_hit_dot",
             ox / 256.0, oy / 256.0,
             target_wx / 256.0, target_wy / 256.0,
             blocked and 255 or 100, blocked and 50 or 255, 50,
             blocked and 180 or 80)
    -- Detail: full ordered sequence — accessible via D key click
    if viz.detail_circle then
      local did = "shot_path"
      local hdr = string.format("Shot path: from=(%d,%d) to=(%d,%d) result=%s",
        origin_mx, origin_my, target_mx, target_my, blocked and "BLOCKED" or "CLEAR")
      local mid_wx = (ox + target_wx) / 2 / 256.0
      local mid_wy = (oy + target_wy) / 2 / 256.0
      viz.detail_circle(did, mid_wx, mid_wy, 0.3, hdr)
      if tiles then
        for i, st in ipairs(tiles) do
          if st.hit_type and st.hit_type == 1 then
            viz.detail_text(did, string.format(
              "#%d TANK#%d @tile(%d,%d)", i, st.hit_id or 0, st.mx, st.my))
          else
            local tt = U.ttype(st.mx, st.my)
            local tt_name = ({
              [C.T_BUILDING] = "wall", [C.T_HALFBUILD] = "halfwall",
              [C.T_FOREST] = "forest", [C.T_ROAD] = "road",
              [C.T_GRASS] = "grass", [C.T_RIVER] = "river",
              [C.T_DEEPSEA] = "deepsea", [C.T_SWAMP] = "swamp",
              [C.T_RUBBLE] = "rubble",
            })[tt] or tostring(tt)
            viz.detail_text(did, string.format(
              "#%d tile(%d,%d) %s", i, st.mx, st.my, tt_name))
          end
        end
      end
    end
  end
  if require_reach and not blocked and not reached then return false end
  return not blocked
end
-- Exported so init.lua shares this exact implementation (no second copy).
M.shot_path_clear = shot_path_clear

-- Crossfire-aware engage point for attack_base. Trace the approach path to the
-- base-adjacent rush tile, then return the CLOSEST-to-base path tile that (a) can
-- still land a shell on the base (clear LOS + in shell reach) and (b) at most
-- ATTACK_BASE_ENGAGE_MAX_CROSSFIRE enemy/neutral pills can fire on. That's the
-- nearest spot we can shell the base from while staying out of pill crossfire.
-- Returns engage_mx, engage_my — or nil to rush right up to the base. Cached on
-- the goal, refreshed every ATTACK_BASE_ENGAGE_REPLAN ticks (keyed on the rush
-- tile so a changed approach target forces a recompute).
local function pick_base_engage_point(goal, state, world, info, rush_mx, rush_my)
  if not C.ATTACK_BASE_ENGAGE_AVOID_CROSSFIRE then return nil end
  local now = state.tick or 0
  if goal._engage_tick and (now - goal._engage_tick) < (C.ATTACK_BASE_ENGAGE_REPLAN or 40)
     and goal._engage_rush_mx == rush_mx and goal._engage_rush_my == rush_my then
    return goal._engage_mx, goal._engage_my   -- nil mx => rush (cached)
  end
  goal._engage_tick = now
  goal._engage_rush_mx, goal._engage_rush_my = rush_mx, rush_my
  goal._engage_mx, goal._engage_my = nil, nil

  -- Approach path tank -> base-adjacent rush tile (source first, dest last).
  local path = rush_mx and cpf.dijkstra_trace_path_by_kind(cpf.KIND_NORMAL, rush_mx, rush_my)
  if not path or #path < 2 then return nil end

  local bmx, bmy = goal.mx, goal.my
  local bwx, bwy = goal.wx, goal.wy
  local maxcf    = C.ATTACK_BASE_ENGAGE_MAX_CROSSFIRE or 0
  local maxwalls = C.ATTACK_BASE_MAX_WALLS or 1
  local gate     = C.ATTACK_PILL_RANGE or 9.5          -- pre-filter; require_reach is authoritative

  local best_mx, best_my, best_d = nil, nil, math.huge
  -- Trace path is a FLAT array of x,y pairs (path[i], path[i+1]) — same shape
  -- consumed by attack.lua / cpathfinder.lua, NOT a list of {x,y} tables.
  for i = 1, #path - 1, 2 do
    local px, py = path[i], path[i+1]
    local dx, dy = px - bmx, py - bmy
    local d = math.sqrt(dx * dx + dy * dy)
    -- in firing gate, off the base tile, crossfire within budget, and a real shot
    if d >= 1.0 and d <= gate and threat.coverage_at(px, py) <= maxcf then
      if shot_path_clear(info, world, bwx, bwy, bmx, bmy, maxwalls,
                         U.m2w(px), U.m2w(py), true) and d < best_d then
        best_d, best_mx, best_my = d, px, py
      end
    end
  end
  goal._engage_mx, goal._engage_my = best_mx, best_my
  if BRAIN_DEBUG_MODE and best_mx then print2(string.format("BASE_ENGAGE t=%d base(%d,%d) -> engage(%d,%d) d=%.1f cf<=%d (closest crossfire-free shot)", now, bmx, bmy, best_mx, best_my, best_d, maxcf)) end
  if BRAIN_DEBUG_MODE and not best_mx then print2(string.format("BASE_ENGAGE t=%d base(%d,%d) no crossfire-free shot tile on path — rushing", now, bmx, bmy)) end
  return best_mx, best_my
end

-- Per-tick accumulators feeding the nav-dispatch/path breakdown in the
-- BrainTest "Capacity tiers" panel. Reset at the top of M.steer; written
-- to from cpf_path_to (search + trace) and from the path_lookahead call
-- sites; read at each nav-dispatch emit point. Module-scope is fine here
-- — M.steer is called once per tick per brain and brains don't share
-- their steering module instance across instances.
local _path_search_us    = 0  -- cpf.path_to(...) C call (dijkstra+astar)
local _path_trace_us     = 0  -- cpf.trace_path / dijkstra_trace_path
local _path_lookahead_us = 0  -- path_lookahead(state, info, nx, ny)
local _path_method       = "dij" -- cpf._last_method captured at search time

-- ---------------------------------------------------------------------------
-- Stuck-recovery: penalize cpf tiles where the tank is wedged.
--
-- The init.lua stuck detector at 150 ticks blocks the goal destination, which
-- is useless when the tank is stuck mid-path (corner-clipping a wall block,
-- wedged against a friendly pill the planner thought was passable, etc.).
-- This earlier, finer-grained recovery watches per-tile progress toward
-- pf.next_mx/my and, when the tank stops making forward progress for ~30
-- ticks, adds a heavy cost overlay to that tile so the next A* search routes
-- around it.  The penalty decays after STUCK_DURATION ticks so long-lived
-- routes don't poison the map permanently.
--
-- Re-stamp every tick: init.lua may call cpf.clear_overlay() during the
-- threat phase (when the friendly-pill / hostile-base set changed), so live
-- stuck entries have to be written back. When an entry expires we also
-- zero its overlay cell explicitly — init.lua no longer clears the overlay
-- unconditionally, so without that the penalty would leak forever.
-- ---------------------------------------------------------------------------
local STUCK_TICKS    = 100   -- ticks of no progress before triggering (~2s)
local STUCK_MOVE_WU  = 24    -- world-units the tank must move within window
local STUCK_PENALTY  = 1500  -- overlay cost added to the offending tile
local STUCK_DURATION = 600   -- ticks the penalty stays active (~12s)
-- Earlier sub-trigger: if the tank has been not-moving for this long
-- (less than STUCK_TICKS so it fires BEFORE the blacklist kicks in),
-- collapse path_lookahead to the tank's own tile. The tank then aims
-- at its own center, the engine re-centers within the tile, and the
-- next tick's lookahead can advance again. Lets the bot break out of
-- "lookahead pulled past a corner I can't actually reach" without
-- waiting for the heavier blacklist + A* recompute.
local STUCK_LOOKAHEAD_COLLAPSE_TICKS = 30

local _ap_stationary = {
  plan_position=true, position=true, aim=true, engage=true,
  curve_away=true, rush=true, disengage=true,
  in_range_position=true, in_range_aim_pre=true,
  in_range_aim=true, in_range_aim_finetune=true, shoot_pill=true,
  build_walls=true,
  -- blitz_wait: the blitzer deliberately HOLDS at its standoff for the GO, so
  -- it never reaches pf.next — without this, stuck_recovery's hard-escape nukes
  -- the goal to "none" after ~3 firings (saw a commander dropped mid-blitz_wait).
  blitz_wait=true,
}
local _pp_stationary = {
  dispatch=true, wait_place=true, prewait=true, advance=true,
  shield_engage=true, engage=true, reposition=true, finish=true,
  select_pill=true,
}
local _at_stationary = { engage=true, close=true, disengage=true }

local function intentionally_stationary(goal, info)
  local s = goal.substate or ""
  if goal.kind == "attack_pill" and _ap_stationary[s] then return true end
  if goal.kind == "pill_place"  and _pp_stationary[s] then return true end
  if goal.kind == "attack_tank" and _at_stationary[s] then return true end
  if goal.kind == "rescue_lgm" or goal.kind == "none" then return true end
  -- Refueling: once parked ON (or right beside) the refuel base we sit still
  -- while the base tops us up — that's intentional, NOT stuck. (En route to the
  -- base it's still subject to normal stuck recovery.) Without this, the
  -- stuck-detector escalated to STUCK_ESCAPE and cleared the refuel goal.
  if goal.kind == "refuel_at_base" and info and goal.mx then
    local tmx, tmy = info.tankx >> 8, info.tanky >> 8
    if U.mdist(tmx, tmy, goal.mx, goal.my) <= 1 then return true end
  end
  return false
end

local function stuck_recovery(state, info, goal)
  local now = state.tick or 0
  local bl  = state.stuck_blacklist
  if bl == nil then
    bl = {}
    state.stuck_blacklist = bl
  end

  -- Decay expired entries and re-stamp the rest into the per-tick overlay.
  -- Expiring entries must explicitly zero the overlay cell: init.lua only
  -- rebuilds the overlay when the friendly-pill / hostile-base set changes,
  -- so without this the stuck penalty would persist after expiry.
  for k, expiry in pairs(bl) do
    if now >= expiry then
      cpf.set_overlay(U.mkey_x(k), U.mkey_y(k), 0)
      bl[k] = nil
    else
      cpf.set_overlay(U.mkey_x(k), U.mkey_y(k), STUCK_PENALTY)
    end
  end

  if state.wall_clearing or intentionally_stationary(goal, info) then
    state.stuck_progress = nil
    return
  end

  local pf = state.pf
  if not pf or pf.next_mx == nil or pf.next_mx < 0 then
    state.stuck_progress = nil
    return
  end

  -- Progress = getting CLOSER to the next-step tile, not raw displacement.
  -- The old test reset the window whenever the tank drifted STUCK_MOVE_WU
  -- from the window-start position — but a tank corner-grinding at full
  -- speed slides back and forth along the wall face by more than that, so
  -- the window reset forever and recovery never fired (20260704_005544
  -- t≈40200-41009: 800+ ticks wedged at (143,125) at spd=48 against the
  -- pill/wall pinch, dij next=(142,126) physically unreachable through the
  -- blocked diagonal). Track the closest approach (Chebyshev, world units)
  -- to the next tile's center and reset only when the tank beats that best
  -- by STUCK_MOVE_WU — oscillation can't pump the ratchet, while a genuine
  -- slow crawl toward the tile keeps resetting every few ticks.
  local next_wx = pf.next_mx * 256 + 128
  local next_wy = pf.next_my * 256 + 128
  local cur_d = math.max(math.abs(info.tankx - next_wx),
                         math.abs(info.tanky - next_wy))
  local sp = state.stuck_progress
  if sp == nil
     or sp.next_mx ~= pf.next_mx
     or sp.next_my ~= pf.next_my
     or sp.best_d == nil
     or (sp.best_d - cur_d) > STUCK_MOVE_WU then
    state.stuck_progress = {
      best_d  = cur_d,
      next_mx = pf.next_mx, next_my = pf.next_my,
      since   = now,
    }
    return
  end
  if cur_d < sp.best_d then sp.best_d = cur_d end

  if (now - sp.since) < STUCK_TICKS then return end

  -- No progress for STUCK_TICKS toward the same next-step tile: penalize it.
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local k = U.mkey(pf.next_mx, pf.next_my)
  if bl[k] == nil then
    cpf.set_overlay(pf.next_mx, pf.next_my, STUCK_PENALTY)
    if BRAIN_DEBUG_MODE then
      print(string.format(
        "[STUCK_RECOVERY] t=%d pos=(%d,%d) spd=%d goal=%s next=(%d,%d) penalize %dt",
        now, tmx, tmy, info.speed or 0,
        goal.kind, pf.next_mx, pf.next_my, STUCK_DURATION))
      local _p2 = require("print2")
      _p2(string.format(
        "STUCK_RECOVERY t=%d pos=(%d,%d) spd=%d goal=%s next=(%d,%d)",
        now, tmx, tmy, info.speed or 0,
        goal.kind, pf.next_mx, pf.next_my))
    end
    log.event("stuck_recovery", string.format(
      "%s next=%d,%d", goal.kind, pf.next_mx, pf.next_my))
  end
  bl[k] = now + STUCK_DURATION
  state.pf.status = "idle"  -- force A* recompute against the new overlay
  state.stuck_progress = nil

  -- Hard escape: if stuck_recovery keeps firing at the same tank tile,
  -- the bot is trapped regardless of how many neighbors are simultaneously
  -- blacklisted (entries may expire between firings). Track consecutive
  -- recoveries at the same position and escape after STUCK_HARD_ESCAPE.
  local STUCK_HARD_ESCAPE = 3
  if not state._stuck_escape_mx or state._stuck_escape_mx ~= tmx
     or state._stuck_escape_my ~= tmy then
    state._stuck_escape_mx = tmx
    state._stuck_escape_my = tmy
    state._stuck_escape_count = 1
  else
    state._stuck_escape_count = (state._stuck_escape_count or 0) + 1
  end
  if state._stuck_escape_count >= STUCK_HARD_ESCAPE then
    state._stuck_escape_count = 0
    do
      local _p2 = require("print2")
      _p2(string.format(
        "STUCK_ESCAPE t=%d pos=(%d,%d) goal=%s dest=(%d,%d) count=%d 600t",
        now, tmx, tmy, goal.kind, goal.mx or 0, goal.my or 0,
        state._stuck_escape_count))
      if BRAIN_DEBUG_MODE then
        print(string.format(
          "[STUCK_ESCAPE] t=%d pos=(%d,%d) goal=%s count=%d — clearing goal",
          now, tmx, tmy, goal.kind, state._stuck_escape_count))
      end
    end
    -- Block the goal destination so pick_goal doesn't re-select it.
    local gk = U.mkey(goal.mx or 0, goal.my or 0)
    U.set_blocked(state, gk, now + 600, "steer_stuck_dest")
    goal.kind = "none"
    goal.substate = nil
    state.pf.status = "idle"
  end
end

-- Wrapper: call C pathfinder and update state.pf for compatibility with
-- stuck detection, debug logging, and other consumers of state.pf.
local function cpf_path_to(state, info, dest_mx, dest_my)
  local pf  = state.pf
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local in_boat = info.inboat and 1 or 0
  local shells  = info.shells or 0
  local trees   = info.trees or 0
  local mines   = info.mines or 0
  local armour  = info.armour or 40

  -- Save previous next step as fallback while new path computes
  local fallback_nx  = pf.next_mx
  local fallback_ny  = pf.next_my
  local tank_moved   = (pf.src_mx ~= tmx or pf.src_my ~= tmy)
  local dest_changed = (pf.dest_mx ~= dest_mx or pf.dest_my ~= dest_my)
  local use_fallback = tank_moved and not dest_changed and fallback_nx >= 0

  -- capture_pill: Dijkstra slate still has the dead pill's tile at cost INF
  -- (was 32767 overlay when alive). Clear the overlay so A* can route onto
  -- it — same technique as the pickup A* in goals.lua. No restore needed;
  -- dead pill has no overlay and init.lua doesn't stamp health=0 pills.
  if state.goal and state.goal.kind == "capture_pill" then
    cpf.set_overlay(dest_mx, dest_my, 0)
  end
  -- Live ally-tank dodge: pass the per-tick obstacle tile set (built in init.lua
  -- from blitz participants we can see) so the Dijkstra tracer veers around them
  -- at trace time — instant, no slate recompute. nil when no blitz is converging.
  local avoid = state._nav_avoid_tiles
  local _t_s0 = BRAIN_PROFILE and clock_us() or 0
  local status, nx, ny = cpf.path_to(tmx, tmy, dest_mx, dest_my, in_boat, shells, trees, mines, armour, C.ASTAR_BUDGET, false, avoid, C.NAV_AVOID_PENALTY)
  if BRAIN_PROFILE then
    _path_search_us = _path_search_us + (clock_us() - _t_s0)
    -- Snapshot which method (dij/astar) cpf.path_to actually used
    -- right after the call. Reading later isn't safe — other parts of
    -- the brain (goals.lua, etc.) might call cpf.path_to before we
    -- emit, overwriting cpf._last_method.
    _path_method = cpf._last_method or "dij"
  end

  -- Update state.pf tracking fields
  pf.src_mx  = tmx
  pf.src_my  = tmy
  pf.dest_mx = dest_mx
  pf.dest_my = dest_my

  if status == 1 then      -- done
    pf.status  = "done"
    pf.next_mx = nx
    pf.next_my = ny
    pf.age     = 0
    -- Capture full path chain for debug logging + path_lookahead.
    -- Try A* trace first (works when A* ran); fall back to Dijkstra
    -- trace (the common case now since cpf.path_to tries Dijkstra first
    -- and skips A* when it succeeds — leaving the A* state stale).
    local _t_tr0 = BRAIN_PROFILE and clock_us() or 0
    pf.path_chain = cpf.trace_path()
    if not pf.path_chain or #pf.path_chain == 0 then
      pf.path_chain = cpf.dijkstra_trace_path(cpf.KIND_NORMAL, dest_mx, dest_my)
    end
    if BRAIN_PROFILE then _path_trace_us = _path_trace_us + (clock_us() - _t_tr0) end
  elseif status == 0 then  -- running
    pf.status = "running"
    if nx >= 0 then
      pf.next_mx = nx
      pf.next_my = ny
    elseif pf.path_chain and #pf.path_chain >= 4 then
      -- A* restarted (nx=-1) but we have the green path from the last
      -- completed search. Walk it to find our current position and use
      -- the next point as the waypoint.
      local best_i = nil
      local best_d = math.huge
      local nwp = #pf.path_chain // 2
      for i = 1, nwp do
        local dx = pf.path_chain[2*i-1] - tmx
        local dy = pf.path_chain[2*i] - tmy
        local d = dx * dx + dy * dy
        if d < best_d then
          best_d = d
          best_i = i
        end
      end
      if best_i and best_i < nwp then
        local nxt_mx = pf.path_chain[2*best_i+1]
        local nxt_my = pf.path_chain[2*best_i+2]
        pf.next_mx = nxt_mx
        pf.next_my = nxt_my
      end
    end
    pf.age = (pf.age or 0) + 1
  else                      -- failed (-1)
    pf.status  = "failed"
    pf.next_mx = -1
    pf.next_my = -1
  end

  -- Boat-mode near deep water: when the tank sits on a tile bordering deep sea
  -- (any of the 8 neighbours), keep the nav destination within ONE tile of the
  -- tank. Stops steering from aiming a long diagonal that clips a deep-water
  -- corner (instant drown) — forces careful tile-by-tile movement along the
  -- shoreline.
  if pf.next_mx and pf.next_mx >= 0
     and (U.ttype(tmx + 1, tmy    ) == C.T_DEEPSEA or U.ttype(tmx - 1, tmy    ) == C.T_DEEPSEA
       or U.ttype(tmx,     tmy + 1) == C.T_DEEPSEA or U.ttype(tmx,     tmy - 1) == C.T_DEEPSEA
       or U.ttype(tmx + 1, tmy + 1) == C.T_DEEPSEA or U.ttype(tmx - 1, tmy + 1) == C.T_DEEPSEA
       or U.ttype(tmx + 1, tmy - 1) == C.T_DEEPSEA or U.ttype(tmx - 1, tmy - 1) == C.T_DEEPSEA) then
    if     pf.next_mx > tmx + 1 then pf.next_mx = tmx + 1
    elseif pf.next_mx < tmx - 1 then pf.next_mx = tmx - 1 end
    if     pf.next_my > tmy + 1 then pf.next_my = tmy + 1
    elseif pf.next_my < tmy - 1 then pf.next_my = tmy - 1 end
  end

  if (pf.status == "done" or pf.status == "running") and pf.next_mx >= 0 then
    return pf.next_mx, pf.next_my
  end
  if use_fallback then
    return fallback_nx, fallback_ny
  end
  return nil, nil
end

-- Impassable terrain types for path lookahead line-of-sight checks.
local IMPASSABLE = {
  [C.T_BUILDING]  = true,
  [C.T_HALFBUILD] = true,
  [C.T_DEEPSEA]   = true,
}

-- Water terrain types: tiles where the tank rides the boat.
local WATER_TT = {
  [C.T_RIVER]   = true,
  [C.T_DEEPSEA] = true,
  [C.T_BOAT]    = true,  -- boat pickup tile is on water
}

-- Corridor clearance: when the tank is sitting off-center in its tile, a
-- straight-line traversal that stays on passable tiles at the tile-center
-- level can still clip an impassable *orthogonal* neighbor of a path tile
-- before the tank re-centers.  Example the fix was written for: tank at
-- (125,101) offset south, heading east along y=101.  Path tile (126,101)
-- is clear but (126,102) is a wall; aim_at from the tank's world position
-- to a distant east-row lookahead produces a shallow aim that scrapes
-- (126,102).  Once the tank is snug against the wall the physics engine
-- pins it and us_steering burns ticks without progress.
--
-- A non-zero offset along axis A is only dangerous when the traversal
-- crosses a tile whose neighbor on the +A or -A side (matching the tank's
-- offset) is impassable — in that case the tank body sweeps through
-- forbidden space before centering.  Threshold below is the off-axis
-- distance (in world units; 256 = one tile) beyond which we assume the
-- tank body straddles into the neighboring tile.
local CORRIDOR_CLEARANCE_WU = 48

-- Returns (clipped, clip_x, clip_y).  `clipped` is true iff a Bresenham
-- line from the tank's tile to (cand_cx, cand_cy) passes through any tile
-- whose offset-side perpendicular neighbor is impassable, given the tank's
-- current sub-tile offset.  Tank's own tile is excluded from the scan.
local function corridor_path_clipped(info, tmx, tmy, cand_cx, cand_cy)
  local off_x = info.tankx - U.m2w(tmx)
  local off_y = info.tanky - U.m2w(tmy)
  local check_south = off_y >  CORRIDOR_CLEARANCE_WU
  local check_north = off_y < -CORRIDOR_CLEARANCE_WU
  local check_east  = off_x >  CORRIDOR_CLEARANCE_WU
  local check_west  = off_x < -CORRIDOR_CLEARANCE_WU
  if not (check_south or check_north or check_east or check_west) then
    return false, -1, -1
  end

  local clip_x, clip_y = -1, -1
  U.bresenham(tmx, tmy, cand_cx, cand_cy, function(bx, by)
    if bx == tmx and by == tmy then return end
    if check_south and U.in_map(bx, by + 1)
       and IMPASSABLE[U.ttype(bx, by + 1)] then
      clip_x, clip_y = bx, by + 1
      return true
    end
    if check_north and U.in_map(bx, by - 1)
       and IMPASSABLE[U.ttype(bx, by - 1)] then
      clip_x, clip_y = bx, by - 1
      return true
    end
    if check_east and U.in_map(bx + 1, by)
       and IMPASSABLE[U.ttype(bx + 1, by)] then
      clip_x, clip_y = bx + 1, by
      return true
    end
    if check_west and U.in_map(bx - 1, by)
       and IMPASSABLE[U.ttype(bx - 1, by)] then
      clip_x, clip_y = bx - 1, by
      return true
    end
  end)
  return clip_x >= 0, clip_x, clip_y
end

-- Path lookahead: given the next A* step (nx, ny), walk pf.path_chain
-- forward and return the furthest waypoint reachable in a clear straight
-- line from the tank.  This eliminates per-tile wiggle on straight runs.
-- Returns the lookahead waypoint (lx, ly) or (nx, ny) if no skip is possible.
--
-- Boat-aware:
--   On foot: stop at BOAT tiles (must step on them to pick up).
--   In boat: stop at any water/land boundary.  Cutting diagonals through
--            a river corridor can clip a land tile and lose the boat.
--            Also stop at BOAT tiles on water (transition point).
local function path_lookahead(state, info, nx, ny)
  local pf = state.pf
  local chain = pf.path_chain
  if not chain or #chain < 4 then
    sdbg("lookahead: no chain or chain<2, return nx=%d ny=%d", nx, ny)
    return nx, ny
  end

  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8

  sdbg("lookahead: tank=(%d,%d) nx=(%d,%d) chain#=%d", tmx, tmy, nx, ny, #chain // 2)

  if info.inboat then
    sdbg("lookahead: in boat, return nx=%d ny=%d", nx, ny)
    return nx, ny
  end

  -- Cliff guard: if there's a deep-water tile within 1 tile (8-neighbor)
  -- of the tank, suppress the lookahead entirely and return the immediate
  -- A* next step. The lookahead's straight-line "skip ahead" can aim
  -- the tank past a deep-sea tile that the bresenham check accepted as
  -- on-path but the tank's body sweep clips into. Holding to the
  -- adjacent waypoint forces the engine to re-evaluate per-tile.
  do
    local cliff_near = false
    for dy = -1, 1 do
      for dx = -1, 1 do
        if not (dx == 0 and dy == 0) then
          local cx, cy = tmx + dx, tmy + dy
          if U.in_map(cx, cy) and U.ttype(cx, cy) == C.T_DEEPSEA then
            cliff_near = true
            break
          end
        end
      end
      if cliff_near then break end
    end
    if cliff_near then
      sdbg("lookahead: DEEPSEA within 1 tile, holding to nx=(%d,%d)", nx, ny)
      return nx, ny
    end
  end

  -- Stuck-recovery collapse: if stuck_recovery's progress tracker says
  -- we haven't moved STUCK_MOVE_WU toward the same next-step tile in
  -- STUCK_LOOKAHEAD_COLLAPSE_TICKS, jump the lookahead all the way back
  -- to the tank's own tile. Aiming at our own center lets the engine
  -- re-center us within the tile, after which the next tick's normal
  -- lookahead chain walk will advance from a clean position. Fires
  -- before the heavier STUCK_TICKS-blacklist trigger so we get a
  -- gentler recovery first.
  do
    local sp = state.stuck_progress
    if sp and (state.tick or 0) - sp.since >= STUCK_LOOKAHEAD_COLLAPSE_TICKS then
      sdbg("lookahead: STUCK collapse (%d ticks), return tank tile (%d,%d)",
           (state.tick or 0) - sp.since, tmx, tmy)
      return tmx, tmy
    end
  end

  -- Find nx,ny in the chain
  local start_idx = nil
  for i = 1, #chain // 2 do
    if chain[2*i-1] == nx and chain[2*i] == ny then
      start_idx = i
      break
    end
  end
  if not start_idx then
    sdbg("lookahead: nx,ny not found in chain, return nx=%d ny=%d", nx, ny)
    return nx, ny
  end
  sdbg("lookahead: start_idx=%d", start_idx)

  -- If nx itself is a water/boat tile, don't skip past it — must step on it
  if U.in_map(nx, ny) then
    local nx_tt = U.ttype(nx, ny)
    if nx_tt == C.T_BOAT or WATER_TT[nx_tt] then
      sdbg("lookahead: nx is water/boat tt=%d, return nx=%d ny=%d", nx_tt, nx, ny)
      return nx, ny
    end
  end

  -- Corridor clearance pre-check: if traversing even the immediate A*
  -- step (nx, ny) from the tank's current off-center world position would
  -- sweep into an impassable perpendicular neighbor, force re-centering
  -- by returning the tank's own tile.  steering aims at that tile's
  -- center, which moves the tank toward the center of its current tile
  -- before the next lookahead advances.
  local clip_hit, clip_x, clip_y = corridor_path_clipped(info, tmx, tmy, nx, ny)
  if clip_hit then
    sdbg("lookahead: CORRIDOR next-step unsafe at clip=(%d,%d), re-center (%d,%d)",
         clip_x, clip_y, tmx, tmy)
    return tmx, tmy
  end

  -- Cautious-approach guard: inside (or about to step into) the 3x3 ring around an
  -- ally mid-pill-take (state._ally_take_tiles), suppress the skip-ahead — aim only
  -- at the immediate next step so we crawl the avoiding route tile-by-tile instead
  -- of building momentum and drifting onto the ally's tile (the bowl-through).
  -- MUST run AFTER the cliff / stuck-collapse / corridor-clip checks above: those
  -- re-center the tank to break a stuck/corner-blocked diagonal, and an earlier
  -- return here bypassed them — leaving a crawling bot wedged on a diagonal it
  -- couldn't cut (saw it stuck 10+ ticks on a blocked NE step beside an ally take).
  do
    local take_tiles = state._ally_take_tiles
    if take_tiles and (take_tiles[tmy * 256 + tmx] or take_tiles[ny * 256 + nx]) then
      sdbg("lookahead: near ally pill-take, crawl per-tile, hold to nx=(%d,%d)", nx, ny)
      -- Tag this as the cautious near-ally CREEP: holding the lookahead to the
      -- next tile is what makes the throttle brake here. init's TAKE_CRAWL strips
      -- ONLY this brake (to keep a steady cruise through the ally's take) and
      -- leaves every other KEY_SLOWER — cliff, destination-stop, combat — intact.
      state._cautious_lookahead_held = true
      return nx, ny
    end
  end

  -- Build set of all tiles on the A* path
  -- Cache the on_path set keyed by chain identity. The chain table
  -- is replaced wholesale by cpf_path_to whenever the path changes,
  -- so we use the table itself as the cache key. Avoids rebuilding
  -- a ~200-entry set every tick on long paths. Tank-tile membership
  -- is OR'd in at lookup time so we don't pollute the cache with
  -- per-tick tank positions.
  local pf = state.pf
  local on_path_chain = pf._on_path_cache
  if pf._on_path_chain ~= chain or not on_path_chain then
    on_path_chain = {}
    for i = 1, #chain // 2 do
      on_path_chain[U.mkey(chain[2*i-1], chain[2*i])] = true
    end
    pf._on_path_cache = on_path_chain
    pf._on_path_chain = chain
  end
  local tank_key = U.mkey(tmx, tmy)
  local function on_path_check(key)
    return on_path_chain[key] or key == tank_key
  end

  local chain_nwp = #chain // 2
  local best_x, best_y = nx, ny
  -- Straight-only lookahead. The aim point may advance ONLY along the ray from
  -- the tank through the first step (nx,ny) — i.e. project tank -> nx, then keep
  -- going straight while the path stays on that ray. The moment the A* path
  -- bends off it, stop. Without this the bresenham "stay on path" test below
  -- happily skips the aim point around a diagonal corner (the path IS the
  -- diagonal, so the line to a far diagonal tile is all on-path), pulling the
  -- purple lookahead marker off-axis — see nav diag 2. dx0,dy0 is the unit step
  -- to the first waypoint; `steps` tiles out along it must equal the chain tile.
  local dx0, dy0 = nx - tmx, ny - tmy
  local straight_only = (dx0 ~= 0 or dy0 ~= 0)
                        and math.abs(dx0) <= 1 and math.abs(dy0) <= 1
  for i = start_idx + 1, chain_nwp do
    local cx, cy = chain[2*i-1], chain[2*i]
    if straight_only then
      local steps = i - start_idx + 1
      if cx ~= tmx + dx0 * steps or cy ~= tmy + dy0 * steps then
        sdbg("lookahead: STOP path bends off straight ray at cand=(%d,%d)", cx, cy)
        break
      end
    end
    -- Must-visit: BOAT or water tile when on foot
    if U.in_map(cx, cy) then
      local tt = U.ttype(cx, cy)
      sdbg("lookahead: i=%d cand=(%d,%d) ttype=%d (BOAT=%d RI=%d DS=%d)",
           i, cx, cy, tt, C.T_BOAT, C.T_RIVER, C.T_DEEPSEA)
      if tt == C.T_BOAT or WATER_TT[tt] then
        best_x, best_y = cx, cy
        sdbg("lookahead: STOP water/boat at (%d,%d) tt=%d", cx, cy, tt)
        break
      end
    end
    -- Check if next chain entry is water/unknown
    if i + 1 <= chain_nwp then
      local ncx, ncy = chain[2*i+1], chain[2*i+2]
      if U.in_map(ncx, ncy) then
        local ntt = U.ttype(ncx, ncy)
        sdbg("lookahead: next_chain=(%d,%d) ttype=%d", ncx, ncy, ntt)
        if ntt == C.T_DEEPSEA or ntt == C.T_RIVER or ntt == C.T_UNKNOWN then
          best_x, best_y = cx, cy
          sdbg("lookahead: STOP next-is-water at (%d,%d) next_tt=%d", cx, cy, ntt)
          break
        end
      end
    end
    -- Check that Bresenham from tank to this candidate stays on path
    local all_on_path = true
    local off_tile_x, off_tile_y = -1, -1
    U.bresenham(tmx, tmy, cx, cy, function(bx, by)
      if bx == tmx and by == tmy then return end
      if not on_path_check(U.mkey(bx, by)) then
        all_on_path = false
        off_tile_x, off_tile_y = bx, by
        return true
      end
    end)
    if not all_on_path then
      sdbg("lookahead: STOP bresenham off-path at (%d,%d) for cand=(%d,%d)",
           off_tile_x, off_tile_y, cx, cy)
      break
    end
    -- Corridor clearance: even though every tile on the line is on the
    -- A* path, the tank's sub-tile offset may cause its body to clip an
    -- impassable perpendicular neighbor before the physics can re-center.
    local clip_hit2, clip_x2, clip_y2 = corridor_path_clipped(info, tmx, tmy, cx, cy)
    if clip_hit2 then
      sdbg("lookahead: STOP corridor clip at (%d,%d) for cand=(%d,%d)",
           clip_x2, clip_y2, cx, cy)
      break
    end
    best_x, best_y = cx, cy
    sdbg("lookahead: advanced best to (%d,%d)", cx, cy)
  end

  sdbg("lookahead: RESULT (%d,%d)", best_x, best_y)
  return best_x, best_y
end

-- =========================================================================
-- Pill placement steering
-- =========================================================================
local function pill_place_steer(state, world, info, goal)
  if goal.kind ~= "pill_place" then return nil end
  local keys = 0
  local taps = 0
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8

  -- Gunsight at max range for all substates
  if info.gunrange < C.GUNSIGHT_MAX then
    keys = keys | KEY_MORERANGE
  end

  -- ── pickup: navigate to dead friendly pill ────────────────────────
  if goal.substate == "pickup" then
    local nav_mx = goal.source_mx or goal.mx
    local nav_my = goal.source_my or goal.my
    local nx, ny = cpf_path_to(state, info, nav_mx, nav_my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = nav_turn_speed(corr, info.speed)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    return keys, taps
  end

  -- ── navigate: A* to deploy position (outside pill range) ───────────
  if goal.substate == "navigate" then
    local nav_mx = goal.deploy_mx or goal.place_mx or goal.mx
    local nav_my = goal.deploy_my or goal.place_my or goal.my
    local nx, ny = cpf_path_to(state, info, nav_mx, nav_my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = nav_turn_speed(corr, info.speed)
      keys = keys | k
      taps = taps | t
    else
      -- On the deploy tile but possibly not centered. The LGM has to
      -- pathfind out of the tank's exact tile center, so creep toward
      -- the center if we're not there yet. Tolerance 16 wu (1/16 tile).
      local nav_wx, nav_wy = U.m2w(nav_mx), U.m2w(nav_my)
      local center_dist = U.wdist(info.tankx, info.tanky, nav_wx, nav_wy)
      if center_dist > 16 then
        local move_dir = U.aim_at(info.tankx, info.tanky, nav_wx, nav_wy)
        local corr = U.adiff(info.direction, move_dir)
        -- Use a low max speed (4) so we don't overshoot the center.
        local k, t = nav_turn_speed(corr, info.speed, 4, 1)
        keys = keys | k
        taps = taps | t
      else
        if info.speed > 0 then keys = keys | KEY_SLOWER end
      end
    end
    return keys, taps
  end

  -- ── dispatch / wait_place: hold position or move to engage ────────
  if goal.substate == "dispatch" then
    -- Hold position while LGM goes to place pill
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  if goal.substate == "wait_place" or goal.substate == "prewait" then
    -- Hold position at deploy spot while LGM builds/returns.
    -- Moving toward engage now would put us inside pill range unshielded.
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- ── Post-placement states: delegate to general steer ──────────────
  -- These use the EXACT same steering as attack_pill (wall-shield).
  -- Returning nil makes the main steer() handle navigation + aim/fire.
  if goal.substate == "advance" or goal.substate == "shield_engage"
     or goal.substate == "reposition" then
    return nil
  end

  -- ── select_pill / disengage / other: brake ────────────────────────
  if info.speed > 0 then keys = keys | KEY_SLOWER end
  return keys, taps
end

-- =========================================================================
-- Attack pill steering — aim, engage, rush substates
-- =========================================================================
-- Reposition: self-contained steering for the shoot phase of a reposition
-- capture_pill goal. The APPROACH is handled by the general capture
-- navigation below (drives the tank right up to its own pill); once we're
-- within firing range this takes over: stop, turn to face the pill, and
-- fire until it's dead or we run out of ammo. When the pill dies (or we're
-- dry), it hands back to the normal capture pickup by returning nil.
-- Returns nil while still approaching so the general nav drives us in.
local function reposition_steer(state, world, info, goal)
  if goal.kind ~= "capture_pill" or not goal.reposition then return nil end
  local pill = world.pills and world.pills[goal.target_id]
  -- Shoot phase over: pill gone / dead / no longer ours / out of ammo.
  -- Fall through so the normal capture pickup (or a replan) takes over.
  if not pill or pill.owner ~= "friendly" or (pill.health or 0) <= 0
     or (info.shells or 0) <= 0 then
    goal.substate = nil
    return nil
  end

  -- Not in firing range yet → let general capture nav drive us right up.
  local pill_wx, pill_wy = U.m2w(goal.mx), U.m2w(goal.my)
  local dist          = U.wdist(info.tankx, info.tanky, pill_wx, pill_wy)
  local fire_range_wu = ((info.gunrange or 14) / 2.0) * 256
  if dist > fire_range_wu then
    goal.substate = "approach"
    return nil
  end

  -- In range but the shot is blocked (another pillbox, wall, base... between
  -- us and OUR pill): do NOT park here — that strands the goal at a spot it
  -- can never fire from. Keep driving toward the pill instead: we must end up
  -- beside it for the post-kill pickup anyway, and closing distance is what
  -- clears the obstruction (adjacent = nothing left in between).
  local los_clear = shot_path_clear(info, world, pill_wx, pill_wy, goal.mx, goal.my)
  if not los_clear then
    goal.substate = "approach"
    print2(string.format("REPOS_NOFIRE t=%d pill#%d@(%d,%d) in-range but blocked-LOS -> keep closing in", state.tick or 0, goal.target_id or 0, goal.mx, goal.my))
    return nil
  end

  -- In range with a clear line: stop, face the pill, fire until dead / dry.
  goal.substate = "reposition_shoot"
  -- Mark this pill "being demolished" so repair_pill won't try to heal the
  -- very pill we're tearing down (a damaged pill is CHEAPER to repair, which
  -- would otherwise create a shoot→repair→shoot oscillation). Grace-expires
  -- on its own; see eval_repair_pill.
  state._demolish_mx   = goal.mx
  state._demolish_my   = goal.my
  state._demolish_tick = state.tick
  local keys, taps = 0, 0
  if info.gunrange < C.GUNSIGHT_MAX then keys = keys | KEY_MORERANGE end
  if info.speed > 0 then keys = keys | KEY_SLOWER end
  local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                             goal.mx + 0.5, goal.my + 0.5)
  local corr    = U.adiff(info.direction, aim_dir)
  local h, t = U.aim_turn_bits(corr, 6, 1)
  keys = keys | h; taps = taps | t
  -- los_clear was verified above (blocked → we kept approaching instead of
  -- entering this phase), so once aligned, fire to the last shell — a
  -- friendly pill won't shoot back. shot_path_clear is the shared tank-aware
  -- sim (blocks on walls/half-walls, any live pillbox, allied tanks, and
  -- bases of any owner) and excludes the target tile, so the trajectory
  -- reaching our pill counts as clear.
  if math.abs(corr) <= 1 and (info.shells or 0) > 0 and los_clear then
    keys = keys | KEY_SHOOT
  end
  if BRAIN_DEBUG_MODE and viz.is_on("hud_attack_status") then
    viz.hud_text("hud_attack_status", 10, 44,
      string.format("Reposition shoot: pill#%d hp=%d shells=%d los=%s",
                    goal.target_id or 0, pill.health or 0, info.shells or 0, tostring(los_clear)),
      "topleft", 255, 180, 80)
  end
  return keys, taps
end

-- =========================================================================
-- Kill-mine steering (demine.lua interrupt): stop, drive the crosshair —
-- heading AND gunsight length — RIGHT ONTO the mine tile, then fire. A
-- shell only detonates a mine when it ENDS on the mined square (engine
-- fires minesExpAddItem at shell death: collision or range-expiry), so the
-- fire gate mirrors kill_lgm's: the actual explosion point (tank +
-- 128*gunrange wu along the heading) must land within DEMINE_LAND_WU of
-- the mine tile center before the trigger is pulled.
-- =========================================================================
local KL = require("kill_lgm")

local function demine_steer(state, world, info, goal)
  local keys, taps = 0, 0
  if info.speed > 0 then keys = keys | KEY_SLOWER end
  local wx, wy = goal.wx, goal.wy
  local dist = U.wdist(info.tankx, info.tanky, wx, wy)
  -- Heading: rotate in place toward the mine center.
  local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                             goal.mx + 0.5, goal.my + 0.5)
  local corr = U.adiff(info.direction, aim_dir)
  local h, t = U.aim_turn_bits(corr, 6, 1)
  keys = keys | h; taps = taps | t
  -- Gunsight length: pull the shell's end-of-life onto the mine's distance.
  local target_sl = KL.sightlen_for(dist)
  keys = keys | KL.gunrange_key(info.gunrange, target_sl)
  -- Fire only when the ACTUAL explosion point lands on the mine tile and
  -- nothing (wall / pill / base / ally) eats the shell on the way.
  local cur_sl = info.gunrange or 14
  local travel = 128 * cur_sl
  local rad    = (info.direction or 0) * C.TWO_PI / 256
  local ex_wx  = info.tankx + math.sin(rad) * travel
  local ex_wy  = info.tanky - math.cos(rad) * travel
  local off_dx, off_dy = ex_wx - wx, ex_wy - wy
  local land_off = math.sqrt(off_dx * off_dx + off_dy * off_dy)
  -- ONE shot per flight: the shell takes dist/SHELL_SPEED (~4 ticks/tile)
  -- to reach the mine, and only its LANDING detonates it — extra shells
  -- fired while the first is in flight are pure waste. After firing, hold
  -- until the shell must have landed (+margin); if the mine is still there
  -- (missed / detonated by something else first), fire again. The pop in
  -- demine.update usually ends the goal before a second shot is needed.
  local now = state.tick or 0
  local in_flight = goal._shot_eta and now < goal._shot_eta
  if not in_flight
     and land_off <= (C.DEMINE_LAND_WU or 100) and (info.shells or 0) > 0
     and shot_path_clear(info, world, wx, wy, goal.mx, goal.my) then
    keys = keys | KEY_SHOOT
    goal._shot_eta = now + math.ceil(dist / (C.SHELL_SPEED or 32)) + 10
  end
  if BRAIN_DEBUG_MODE and viz.is_on("hud_attack_status") then
    viz.hud_text("hud_attack_status", 10, 44,
      string.format("De-mine: (%d,%d) dist=%.1ft corr=%.0f sl=%d/%d land=%.0fwu%s",
        goal.mx, goal.my, dist / 256.0, corr, cur_sl, target_sl, land_off,
        in_flight and string.format(" shell-in-flight %dt", goal._shot_eta - now) or ""),
      "topleft", 255, 120, 120)
  end
  return keys, taps
end

local function attack_pill_steer(state, world, info, goal)
  if goal.kind ~= "attack_pill" then return nil end
  local keys = 0
  local taps = 0

  -- ── detree: hold position, shoot trees in the way ──────────────────
  if goal.substate == "detree" then
    local aim_tx = goal.aim_mx or (goal.mx + 0.5)
    local aim_ty = goal.aim_my or (goal.my + 0.5)
    local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0, aim_tx, aim_ty)
    local corr = U.adiff(info.direction, aim_dir)

    do
      local h, t = U.aim_turn_bits(corr, 6, 1)
      keys = keys | h; taps = taps | t
    end

    if math.abs(corr) <= 1 and info.shells > C.SHELL_RESERVE then
      keys = keys | KEY_SHOOT
    end

    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end

    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- ── charge: accelerate toward standoff, decel to stop on green circle ──
  if goal.substate == "charge" then
    -- PPT-charge gate for THIS substate only: the slow PPT creep / standoff-stop
    -- exists to thread precisely behind a BUILT wall. A blitz overwhelm with no
    -- shield (a soldier, or a commander that skipped walls) has nothing to thread,
    -- so it should close FAST via the non-PPT reach-based path below (full speed,
    -- brake at shot range) even though the pill's HP set goal._is_ppt at plan
    -- time. Shielded PPT takes and solo PPT still creep.
    -- Charge is the FAST rush by definition — it never creeps. Even a shielded /
    -- solo PPT take closes at full speed here; precise shielded-standoff landing
    -- is the in_range_position path's job, not charge. (Was: creep at
    -- PPT_CHARGE_MAX_SPEED for PPT takes — that produced the spd=4 crawl.)
    local _charge_ppt = false

    -- First non-PPT charge tick: pull the engage spot in from the planned 7.4-tile
    -- ring to ATTACK_PILL_STANDOFF_CHARGE (7.0) along the same bearing, so a spot
    -- that rounded just outside shell reach engages without an extra creep. PPT
    -- keeps its precisely-placed shielded standoff. Done at charge time (not plan)
    -- because a PPT can demote to non-PPT before/at charge entry — only pull once
    -- the charge is actually running non-PPT. Gated to fire once per attack.
    if not _charge_ppt and not goal._charge_pulled then
      local pcx, pcy = goal.mx + 0.5, goal.my + 0.5
      local dx, dy = (goal.standoff_fx or pcx) - pcx, (goal.standoff_fy or pcy) - pcy
      local d = math.sqrt(dx * dx + dy * dy)
      if d > 0.001 then
        local R2 = C.ATTACK_PILL_STANDOFF_CHARGE or 7.0
        goal.standoff_fx = pcx + dx / d * R2
        goal.standoff_fy = pcy + dy / d * R2
      end
      goal._charge_pulled = true
    end

    local sfx = goal.standoff_fx or (goal.mx + 0.5)
    local sfy = goal.standoff_fy or (goal.my + 0.5)
    -- Round-to-nearest matches in_range_position (line 914) and the
    -- attack-side dist viz; truncating here would split the standoff
    -- by 1 wu vs the substate that owns the transition decision.
    local swx, swy = math.floor(sfx * 256 + 0.5), math.floor(sfy * 256 + 0.5)
    local sdist = U.wdist(info.tankx, info.tanky, swx, swy)

    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end

    -- Turn so crosshairs align with a CLEAR line of fire to the pill. Test the
    -- pill tile's center plus its 4 corners and aim at whichever sub-point the
    -- shell can actually reach without first hitting a wall, another pill, an
    -- enemy base, or a half-wall. simulate_shot_angle is bit-exact with the
    -- engine and terminates at the first obstacle, so a path that reaches the
    -- target tile is unobstructed by construction. Falls back to the planned
    -- aim dot (center) when every sub-point is blocked.
    local aim_tx = goal.aim_mx or (goal.mx + 0.5)
    local aim_ty = goal.aim_my or (goal.my + 0.5)
    do
      local cands = {
        {goal.mx + 0.5, goal.my + 0.5},
        {goal.mx + 0.2, goal.my + 0.2}, {goal.mx + 0.8, goal.my + 0.2},
        {goal.mx + 0.2, goal.my + 0.8}, {goal.mx + 0.8, goal.my + 0.8},
      }
      for _, c in ipairs(cands) do
        local ang = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0, c[1], c[2])
        local p = cpf.simulate_shot_angle(info.tankx, info.tanky, ang,
                                          cpf.SHOT_TANK, info.gunrange or 14)
        local clear = false
        if p then
          for _, t in ipairs(p) do
            if t.mx == goal.mx and t.my == goal.my then clear = true; break end
          end
        end
        if clear then aim_tx, aim_ty = c[1], c[2]; break end
      end
    end
    local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0, aim_tx, aim_ty)
    local corr = U.adiff(info.direction, aim_dir)

    local charge_phase = "?"
    -- Braking from full speed covers ~0.25 tile. speed * 4 ≈ 64 wu at speed 16.
    local stop_dist = info.speed * 4

    -- If way off target, go back to aim
    if math.abs(corr) > 10 then
      goal.substate = "aim"
      goal.aim_tick = state.tick
      goal._aim_locked = nil
      if info.speed > 0 then keys = keys | KEY_SLOWER end
      if BRAIN_DEBUG_MODE then
        charge_phase = "REARM corr=" .. math.floor(corr)
        viz.hud_text("charge_status", 10, 56, "CHARGE: " .. charge_phase, "topleft", 255, 100, 100)
      end
      return keys, taps
    end

    -- Turn toward the aim point WHILE charging. A real correction uses a HELD
    -- turn (full rate) so a fast charge actually tracks the target instead of
    -- drifting past it; only the last brad or two uses a fine tap.
    if     corr >  3 then keys = keys | KEY_TURNRIGHT
    elseif corr < -3 then keys = keys | KEY_TURNLEFT
    elseif corr >  1 then taps = taps | KEY_TURNRIGHT
    elseif corr < -1 then taps = taps | KEY_TURNLEFT
    end

    -- Arrived — brake to full stop, then engage
    -- Also trigger if tank overshot (closer to pill than standoff is)
    local pill_wx, pill_wy = U.m2w(goal.mx), U.m2w(goal.my)
    local tank_to_pill = U.wdist(info.tankx, info.tanky, pill_wx, pill_wy)
    local standoff_to_pill = U.wdist(swx, swy, pill_wx, pill_wy)
    
    -- Shoot during charge whenever a shell-sim says the trajectory actually
    -- crosses the pill tile -- the sim IS the range + line-of-fire check, so no
    -- static standoff-distance gate is needed (the old `dist <= standoff` gate
    -- pinned the tank just outside standoff, never firing even though the shell
    -- clearly reached). The `corr <= 5` brad pre-gate just skips the sim when
    -- we're way off; the sim itself rejects edge-of-pill shots that physically
    -- miss. Sim is bit-exact with the engine.
    local dist_to_pill = tank_to_pill
    if BRAIN_DEBUG_MODE then
      viz.hud_text("charge_status", 10, 75, string.format("   dist_to_pill=%d corr=%.1f",
                                                           dist_to_pill, corr),
                   "topleft", 255, 255, 0)
    end
    if math.abs(corr) <= 5 and info.shells > C.SHELL_RESERVE then
      -- Pre-gate: rough corr check first (cheap) to avoid the sim
      -- when we're way off. Sim only when within 5 brads.
      local angle_f = info.tank_angle or info.direction
      local path = cpf.simulate_shot_angle(info.tankx, info.tanky, angle_f,
                                            cpf.SHOT_TANK, info.gunrange or 14)
      local hits_pill = false
      if path then
        for _, t in ipairs(path) do
          if t.mx == goal.mx and t.my == goal.my then hits_pill = true; break end
        end
      end
      if hits_pill then
        keys = keys | KEY_SHOOT
        -- Pre-fire predictive swerve: if this on-target shot would be
        -- the one that brings in-flight count up to remaining pill HP,
        -- enter swerve right now (same tick as the shot fires) instead
        -- of waiting for next-tick tracker confirmation.
        predict_kill_shot_and_swerve(state, world, info, goal, goal.mx, goal.my)
        if BRAIN_DEBUG_MODE then
          charge_phase = charge_phase .. " FIRE"
          viz.hud_text("charge_status", 10, 90, "CHARGE, IN RANGE, FIRE (sim hits)",
                       "topleft", 255, 255, 0)
        end
      else
        if BRAIN_DEBUG_MODE then
          viz.hud_text("charge_status", 10, 90, "CHARGE, IN RANGE, hold (sim misses)",
                       "topleft", 255, 180, 100, 255)
        end
      end
    end

    -- PPT engages on standoff arrival/overshoot (the non-PPT stop decision is
    -- reach-based, handled below). Plain `if arrived` here would stop a non-PPT
    -- charge at a standoff that rounded out of shell reach.
    if _charge_ppt and (sdist < 50 or tank_to_pill < standoff_to_pill) then
      if info.speed <= 1 then
        goal.substate = "engage"
        goal.engage_tick = state.tick
      else
        keys = keys | KEY_SLOWER
      end
      if BRAIN_DEBUG_MODE then
        if info.speed <= 1 then
          charge_phase = "ENGAGE"
        else
          charge_phase = string.format("BRAKING spd=%d", info.speed)
        end
        viz.hud_text("charge_status", 10, 56, "CHARGE: " .. charge_phase, "topleft", 255, 255, 0)
      end
      return keys, taps
    end

    -- Stopping distance estimate.  Auto-slowdown decelerates slowly
    -- (not 1 unit/tick).  Empirically, multiply by ~4 to match actual
    -- braking distance.  speed=16 → stop_dist ≈ 544 wu ≈ 2.1 tiles.

    -- Protected pill take (PPT): the wall-shielded standoff is angle-
    -- sensitive — overshooting the green spot exposes us to the pill
    -- around the wall. So creep toward the standoff at PPT_CHARGE_MAX_SPEED
    -- and brake inside PPT_CHARGE_BRAKE_DIST. Slower entry costs a few
    -- extra hits in transit but lands on the spot precisely.
    if _charge_ppt then
      local cap   = C.PPT_CHARGE_MAX_SPEED  or 4
      local brake = C.PPT_CHARGE_BRAKE_DIST or 32
      if sdist <= brake then
        if info.speed > 0 then keys = keys | KEY_SLOWER end
      elseif info.speed >= cap then
        -- Hold at cap by pulsing the slower key; KEY_FASTER would push
        -- us past it. The natural drag won't drop us below cap quickly
        -- so we stay close to it.
        keys = keys | KEY_SLOWER
      else
        keys = keys | KEY_FASTER
      end
      if BRAIN_DEBUG_MODE then
        if sdist <= brake then
          charge_phase = string.format("PPT-BRAKE spd=%d dist=%d", info.speed, sdist)
        elseif info.speed >= cap then
          charge_phase = string.format("PPT-HOLD spd=%d cap=%d dist=%d", info.speed, cap, sdist)
        else
          charge_phase = string.format("PPT-CREEP spd=%d cap=%d dist=%d", info.speed, cap, sdist)
        end
        viz.hud_text("charge_status", 10, 56, "CHARGE: " .. charge_phase, "topleft", 200, 220, 100)
      end
      return keys, taps
    end

    -- ── non-PPT stop decision: brake on REACH, not standoff distance ──
    -- Roll forward (toward the aimed pill) until braking from HERE would still
    -- land a shot on the pill, then brake to a stop. The stop point is predicted
    -- with the engine-exact decel+move model (cpf.predict_stop, terrain-capped
    -- by the tile under the tank) and test-fired from there. This self-corrects:
    -- if a standoff rounded just outside shell reach, we creep the extra bit
    -- instead of parking out of range; if a stop lands a hair short, the next
    -- tick (speed ~0 → predicted stop ≈ here) sees the miss and creeps again.
    -- Floored at CHARGE_MIN_STANDOFF so we never drive onto the pill.
    local tmx_now, tmy_now = info.tankx >> 8, info.tanky >> 8
    local tcap = (C.TERRAIN_SPEED and C.TERRAIN_SPEED[U.ttype(tmx_now, tmy_now)]) or 16
    local ang_f = info.tank_angle or info.direction
    -- info.speed is the engine speed ×4 (brain_data.c: actual_speed*4, 64=road top).
    -- predict_stop's model is in ENGINE units (decel 0.25/tick, TERRAIN_SPEED cap),
    -- so divide by 4 or the stop blows up ~16x (saw 9.3 tiles at grass-cap).
    local espeed = (info.speed or 0) / 4
    local psx, psy = cpf.predict_stop(info.tankx, info.tanky, ang_f, espeed, tcap)
    local stop_hits = false
    if math.abs(corr) <= 5 then
      local sp = cpf.simulate_shot_angle(psx, psy, ang_f, cpf.SHOT_TANK, info.gunrange or 14)
      if sp then for _, t in ipairs(sp) do if t.mx == goal.mx and t.my == goal.my then stop_hits = true; break end end end
    end
    -- Decelerate when braking from HERE would stop at (or inside) the engage
    -- point's distance from the pill — a pure DISTANCE gate, not a shot-sim:
    -- trust the aim ("hopefully you aimed right; if not, go with it"). stop_hits
    -- is kept only for the green/red stop-prediction overlay.
    local pred_stop_to_pill = U.wdist(psx, psy, pill_wx, pill_wy)
    -- Ammo-deprived SUICIDE charge: a starved bot can't fight from standoff, so
    -- it drives in as close as possible — brake only when the predicted stop is
    -- within the suicide floor (≈ adjacent; the pill tile is solid so it can't go
    -- onto it) and lower the never-closer floor to match. Normal standoff engage
    -- otherwise. Aim/heading already track the pill, so only the stop point moves.
    local suicide        = state.ammo_deprived == true
    local engage_dist    = suicide and ((C.CHARGE_SUICIDE_STANDOFF or 1.5) * 256) or standoff_to_pill
    local floor_tiles    = suicide and (C.CHARGE_SUICIDE_STANDOFF or 1.5) or (C.CHARGE_MIN_STANDOFF or 5.0)
    local stop_at_engage = pred_stop_to_pill <= engage_dist
    local at_floor = tank_to_pill <= floor_tiles * 256

    -- Visualize the predicted brake-now stop: line tank->stop + marker, green
    -- if a shot from there hits the pill, red if short. (charge_stop_pred toggle)
    if BRAIN_DEBUG_MODE and viz.is_on("charge_stop_pred") then local _r, _g, _b = stop_hits and 60 or 255, stop_hits and 220 or 70, 70; local _sx, _sy = psx / 256, psy / 256; viz.line("charge_stop_pred", info.tankx / 256, info.tanky / 256, _sx, _sy, _r, _g, _b, 150); viz.rect("charge_stop_pred", _sx - 0.35, _sy - 0.35, _sx + 0.35, _sy + 0.35, _r, _g, _b, 200, false); viz.text("charge_stop_pred", _sx, _sy - 0.55, stop_hits and "STOP-HIT" or "STOP-SHORT", "center", _r, _g, _b, 230, 0.3) end
    -- DEBUG: why aren't the hover details showing? Logs whether we reach the
    -- charge branch, whether the layer is on, and whether the detail binding exists.
    if BRAIN_DEBUG_MODE then print2(string.format("STOPSIM_DBG t=%d charge spd=%d viz_on=%s detail_fn=%s", state.tick, info.speed, tostring(viz.is_on("charge_stop_pred")), tostring(overlay_detail ~= nil))) end
    -- Per-step brake-sim dump (toggle charge_stop_pred). Each simulation tick is
    -- a hoverable point in the "D" inspector showing speed/decel/move; the stop
    -- marker carries the full step list. Shows exactly how the engine model ramps
    -- speed down and advances the 16-dir residual each tick.
    if BRAIN_DEBUG_MODE and viz.is_on("charge_stop_pred") then
      local _sx2, _sy2, _trace = cpf.predict_stop(info.tankx, info.tanky, ang_f, espeed, tcap, true)
      print2(string.format("STOPSIM_REG t=%d stop=(%d,%d) steps=%s", state.tick, _sx2, _sy2, _trace and #_trace or "nil"))
      viz.detail_circle("stopsim_stop", _sx2 / 256, _sy2 / 256, 0.5, string.format("STOP %s", stop_hits and "HIT" or "SHORT"))
      viz.detail_text("stopsim_stop", string.format("from=(%d,%d) ang=%.1f spd=%d cap=%d", info.tankx, info.tanky, ang_f, info.speed, tcap))
      viz.detail_text("stopsim_stop", string.format("stop=(%d,%d)  tank_to_pill=%d  steps=%d", _sx2, _sy2, tank_to_pill, _trace and #_trace or 0))
      local _cum = 0
      if _trace then for _i, _s in ipairs(_trace) do
        _cum = _cum + _s.dist
        local _line = string.format("[%2d] spd=%.2f -decel=%.2f-> %.2f  moved=%d resid=%d cum=%d", _i, _s.speed, _s.decel, _s.after, _s.dist, _s.resid, _cum)
        viz.detail_text("stopsim_stop", _line)
        -- one hoverable marker per step at its predicted position
        viz.detail_circle("stopsim_" .. _i, _s.x / 256, _s.y / 256, 0.25, string.format("step %d  moved=%d", _i, _s.dist))
        viz.detail_text("stopsim_" .. _i, _line)
        viz.rect("charge_stop_pred", _s.x / 256 - 0.12, _s.y / 256 - 0.12, _s.x / 256 + 0.12, _s.y / 256 + 0.12, 255, 200, 60, 200, true)
      end end
    end

    if stop_at_engage or at_floor then
      -- Braking now lands us at/inside the engage distance (or we hit the
      -- no-closer floor): stop, engage.
      if info.speed <= 1 then
        goal.substate = "engage"
        goal.engage_tick = state.tick
      else
        keys = keys | KEY_SLOWER
      end
      if BRAIN_DEBUG_MODE then viz.hud_text("charge_status", 10, 56, string.format("CHARGE: %s d2p=%d%s%s", (info.speed <= 1) and "ENGAGE" or "BRAKE", tank_to_pill, at_floor and " floor" or "", suicide and " SUICIDE" or ""), "topleft", 255, 255, 0) end
    else
      -- Stopping here wouldn't reach the pill yet → keep closing on it.
      keys = keys | KEY_FASTER
      if BRAIN_DEBUG_MODE then viz.hud_text("charge_status", 10, 56, string.format("CHARGE: %s d2p=%d stop=(%d,%d)", suicide and "SUICIDE-CLOSE" or "CLOSE", tank_to_pill, psx >> 8, psy >> 8), "topleft", 255, 200, 80) end
    end

    return keys, taps
  end

  -- ── aim: turn to face pill, no shooting, no movement ───────────────
  if goal.substate == "aim" then
    local aim_tx = goal.aim_mx or (goal.mx + 0.5)
    local aim_ty = goal.aim_my or (goal.my + 0.5)
    local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0, aim_tx, aim_ty)
    local corr = U.adiff(info.direction, aim_dir)

    if     corr >  6 then keys = keys | KEY_TURNRIGHT
    elseif corr < -6 then keys = keys | KEY_TURNLEFT
    elseif corr >  1 then taps = taps | KEY_TURNRIGHT
    elseif corr < -1 then taps = taps | KEY_TURNLEFT
    end

    if math.abs(corr) <= 1 then
      goal._aim_locked = true
    end

    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end

    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- ── engage: aim at pill near-edge and fire ─────────────────────────
  if goal.substate == "engage" then
    local aim_tx = goal.aim_mx or (goal.mx + 0.5)
    local aim_ty = goal.aim_my or (goal.my + 0.5)
    local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0, aim_tx, aim_ty)
    local corr = U.adiff(info.direction, aim_dir)

    do
      local h, t = U.aim_turn_bits(corr, 6, 1)
      keys = keys | h; taps = taps | t
    end

    if math.abs(corr) <= 1 and info.shells > C.SHELL_RESERVE then
      keys = keys | KEY_SHOOT
      goal._engage_aimed = true
      -- Pre-fire predictive swerve (same rationale as charge).
      predict_kill_shot_and_swerve(state, world, info, goal, goal.mx, goal.my)
    end

    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end

    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- ── rush: pill dead, rush to capture ───────────────────────────────
  if goal.substate == "rush" then
    local nx, ny = cpf_path_to(state, info, goal.mx, goal.my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = nav_turn_speed(corr, info.speed, 64)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    return keys, taps
  end

  -- ── swerve: turn while turn_ticks_left > 0, always accelerate ──
  if goal.substate == "swerve" then
    if (goal._swerve_turn_ticks_left or 0) > 0 then
      local dir = goal._swerve_dir or 1
      if dir > 0 then
        keys = keys | KEY_TURNRIGHT
      else
        keys = keys | KEY_TURNLEFT
      end
    end
    keys = keys | KEY_FASTER
    return keys, taps
  end

  -- ── post_engage: brake while deciding ─────────────────────────────
  if goal.substate == "post_engage" then
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- ── blitz_wait: HOLD at the setup point for the squad GO ───────────
  -- Issue NO drive: brake to a stop if still rolling and stay put while the GO
  -- handshake (attack.lua) runs. Face the pill so we're oriented to charge/fire
  -- the instant GO arrives — turning in place doesn't move us off the spot.
  -- Without this branch the substate fell through to the default approach
  -- navigation and the tank kept driving instead of waiting.
  if goal.substate == "blitz_wait" then
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                               goal.mx + 0.5, goal.my + 0.5)
    local h, t = U.aim_turn_bits(U.adiff(info.direction, aim_dir), 6, 1)
    keys = keys | h; taps = taps | t
    return keys, taps
  end

  -- ── loiter: hold position, wait for pill to cool ───────────────────
  if goal.substate == "loiter" then
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- ── plan_position: brake while planning ───────────────────────────
  if goal.substate == "plan_position" then
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- build_walls: park at the approach point while LGM builds the
  -- shield walls, but TURN toward the chosen aim point so the gun is
  -- already lined up by the time the wall queue is exhausted. Same
  -- turn logic as the aim substate; movement keys are never set.
  if goal.substate == "build_walls" then
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    local aim_tx = goal.aim_mx or (goal.mx + 0.5)
    local aim_ty = goal.aim_my or (goal.my + 0.5)
    local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                               aim_tx, aim_ty)
    local corr = U.adiff(info.direction, aim_dir)
    if     corr >  6 then keys = keys | KEY_TURNRIGHT
    elseif corr < -6 then keys = keys | KEY_TURNLEFT
    elseif corr >  1 then taps = taps | KEY_TURNRIGHT
    elseif corr < -1 then taps = taps | KEY_TURNLEFT
    end
    -- Stretch the gunsight to max while we wait, like aim does.
    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end
    return keys, taps
  end

  -- (was: wait_for_lgm branch — moved to M.steer's main dispatch
  -- since attack_pill_steer early-returns for non-attack_pill kinds,
  -- making the inline check unreachable.)

  -- in_range_position (PPT): drive toward standoff at a moderate cap
  -- (faster than the old creep of 4 — the user complained it was
  -- glacial), then brake earlier to compensate for the extra momentum
  -- so we still stop on the spot instead of overshooting like charge.
  if goal.substate == "in_range_position" then
    local sfx = goal.standoff_fx or (goal.standoff_mx and (goal.standoff_mx + 0.5))
                                 or (goal.mx + 0.5)
    local sfy = goal.standoff_fy or (goal.standoff_my and (goal.standoff_my + 0.5))
                                 or (goal.my + 0.5)
    local swx = math.floor(sfx * 256 + 0.5)
    local swy = math.floor(sfy * 256 + 0.5)
    local sdist = U.wdist(info.tankx, info.tanky, swx, swy)
    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end
    -- FAST_APPROACH: outside 1 tile (FAST_APPROACH_HANDOFF_WU = 256 wu) of the
    -- standoff, accelerate HARD and brake purely off cpf.predict_stop landing on
    -- the spot — same fast-path the `approach` substate uses. Steering still aims
    -- at the standoff every tick; we just strip nav_turn_speed's throttle and
    -- drive it off the predictor. Inside 1 tile we fall through to the existing
    -- creep below for the micro-corrections (AT_SPOT / BRAKE / CREEP / friction-stuck).
    if C.FAST_APPROACH and sdist > (C.FAST_APPROACH_HANDOFF_WU or 256) then
      local move_dir = U.aim_at(info.tankx, info.tanky, swx, swy)
      local corr = U.adiff(info.direction, move_dir)
      -- Constantly correct heading toward the standoff every tick (tight
      -- deadband, like the in_range wait turn). Keeps the tank pointed at the
      -- spot so cpf.predict_stop — which projects along the tank facing — stays
      -- accurate. This owns turning; the predictor owns throttle (below).
      if     corr >  6 then keys = keys | KEY_TURNRIGHT
      elseif corr < -6 then keys = keys | KEY_TURNLEFT
      elseif corr >  1 then taps = taps | KEY_TURNRIGHT
      elseif corr < -1 then taps = taps | KEY_TURNLEFT
      end
      local tmx_now, tmy_now = info.tankx >> 8, info.tanky >> 8
      local tcap = (C.TERRAIN_SPEED and C.TERRAIN_SPEED[U.ttype(tmx_now, tmy_now)]) or 16
      local ang_f = info.tank_angle or info.direction
      local espeed = (info.speed or 0) / 4   -- info.speed is engine speed ×4
      local psx, psy = cpf.predict_stop(info.tankx, info.tanky, ang_f, espeed, tcap)
      local stop_dist = U.wdist(info.tankx, info.tanky, psx, psy)
      local _brake_tol = C.APPROACH_PRECISE_TOL or 16
      if stop_dist >= sdist - _brake_tol then keys = keys | KEY_SLOWER else keys = keys | KEY_FASTER end
      if BRAIN_DEBUG_MODE and viz.is_on("fast_approach") then local _ff = (stop_dist >= sdist - _brake_tol); local _r, _g, _b = _ff and 60 or 255, _ff and 230 or 200, 60; viz.text("fast_approach", info.tankx / 256.0, info.tanky / 256.0 - 1.4, string.format("FAST_INRANGE  spd=%d  corr=%d  sdist=%d  stopd=%d", info.speed, corr, sdist, stop_dist), "center", _r, _g, _b, 245, 0.35); viz.line("fast_approach", info.tankx / 256.0, info.tanky / 256.0, psx / 256.0, psy / 256.0, _r, _g, _b, 160); viz.circle("fast_approach", swx / 256.0, swy / 256.0, 0.18, 80, 160, 255, 220) end
      return keys, taps
    end
    -- Wider window so we accelerate sooner (was 1 tile / 256 wu).
    if sdist <= 512 then
      -- Brake distance: charge uses speed*4 but in practice the tank
      -- stops short with that — user reported needing ~4 more ticks of
      -- coasting before brakes fire. speed*2 lines up the decel curve
      -- with the actual stop point so the tank drifts onto the spot
      -- instead of crawling the last quarter-tile.
      -- +4 wu (~¼ game-pixel) of brake-earlier slack. User saw a
      -- consistent dist=9/8 overshoot — tank coasts ~1 wu past the
      -- tolerance window. This nudges the brake gate barely earlier
      -- without changing the overall approach feel.
      local target_speed = 8   -- cruising cap inside the creep window
      -- Match the attack.lua transition tolerance so we don't brake
      -- at 16 wu and stall outside the (possibly tighter) window
      -- the 3-blocker take needs.
      local close_enough = goal._in_range_dist_tol or 16
      -- Decision tree:
      --   Inside the tolerance window: settle (brake to stop).
      --   Inside it BUT moving fast enough to overshoot: brake.
      --   Otherwise: keep creeping toward the spot (re-accelerate
      --              even after a brake stopped us short).
      -- The earlier "brake whenever sdist <= max(close_enough, stop_dist)"
      -- gate could leave the tank parked at e.g. dist=12 with speed 0
      -- because the brake pinned it without ever re-accelerating to
      -- close the remaining 4 wu. Now: only brake when actually at the
      -- spot OR when current speed would overshoot the gap.
      local at_spot = sdist <= close_enough
      -- speed * 2 wu of coast is the empirical brake distance. If that
      -- exceeds the gap to the tolerance window, brake; otherwise keep
      -- going (slowly).
      local would_overshoot = (info.speed * 2 + 4) > (sdist - close_enough)
      -- Detect "info.speed lies, real motion is 0" (friction-stuck on
      -- tree/swamp): if sdist hasn't changed for several ticks even
      -- though info.speed > 0, the brake is useless and we should be
      -- pushing through with KEY_FASTER instead. Tracker on the goal.
      local now_t = state.tick or 0
      if goal._inrange_prev_sdist == nil
         or math.abs(sdist - goal._inrange_prev_sdist) >= 2 then
        goal._inrange_prev_sdist  = sdist
        goal._inrange_stuck_since = now_t
      end
      local stuck_ticks = now_t - (goal._inrange_stuck_since or now_t)
      local friction_stuck = stuck_ticks >= 8 and not at_spot
      local branch  -- which decision tier fired this tick (for viz)
      if at_spot then
        branch = "AT_SPOT"
        if info.speed > 0 then keys = keys | KEY_SLOWER end
      elseif friction_stuck then
        -- Wheels spinning, tank not moving. Force forward instead of
        -- braking — the brake key would only confirm what the friction
        -- is already enforcing. Re-aim and accelerate; engine + terrain
        -- will resolve.
        branch = "FRICTION_STUCK"
        local move_dir = U.aim_at(info.tankx, info.tanky, swx, swy)
        local corr = U.adiff(info.direction, move_dir)
        local k, t = nav_turn_speed(corr, 0, target_speed, 2)
        keys = keys | k
        taps = taps | t
      elseif would_overshoot and info.speed > 2 then
        -- Coasting tail will land us in the window — brake. Speed gate
        -- (>2) prevents a permanent brake-pin when we're already nearly
        -- stopped but the brake-distance heuristic keeps re-arming.
        branch = "BRAKE"
        keys = keys | KEY_SLOWER
        -- Keep correcting heading toward the standoff while braking so we hold
        -- the line into the spot instead of coasting straight off it. Throttle
        -- stays the brake above; this only adds turn keys (tight ±6/±1 deadband).
        local move_dir = U.aim_at(info.tankx, info.tanky, swx, swy)
        local corr = U.adiff(info.direction, move_dir)
        if     corr >  6 then keys = keys | KEY_TURNRIGHT
        elseif corr < -6 then keys = keys | KEY_TURNLEFT
        elseif corr >  1 then taps = taps | KEY_TURNRIGHT
        elseif corr < -1 then taps = taps | KEY_TURNLEFT
        end
      else
        branch = "CREEP"
        local move_dir = U.aim_at(info.tankx, info.tanky, swx, swy)
        local corr = U.adiff(info.direction, move_dir)
        local k, t = nav_turn_speed(corr, info.speed, target_speed, 2)
        keys = keys | k
        taps = taps | t
      end

      -- Visualization: state machine status near the tank.
      if BRAIN_DEBUG_MODE then
        local twx = info.tankx / 256.0
        local twy = info.tanky / 256.0
        local stop_dist_now = info.speed * 2 + 4
        local gap = sdist - close_enough
        -- Color by branch: green=at_spot/creep, yellow=brake, red=stuck
        local r, g, b = 100, 255, 100
        if branch == "BRAKE"          then r, g, b = 255, 220, 80
        elseif branch == "FRICTION_STUCK" then r, g, b = 255, 80,  80
        end
        viz.text("approach_dist", twx + 1.0, twy + 0.4,
          string.format("in_range: %s  sdist=%d gap=%+d stop=%d",
                        branch, sdist, gap, stop_dist_now),
          "topleft", r, g, b, 255, 0.4)
        viz.text("approach_dist", twx + 1.0, twy + 1.0,
          string.format("stuck=%d/8t prev_sd=%d",
                        stuck_ticks, goal._inrange_prev_sdist or -1),
          "topleft", r, g, b, 255, 0.35)
        -- Standoff target marker (small magenta dot) so we can see the
        -- spot the brake/creep is aiming at.
        viz.circle("approach_dist", swx / 256.0, swy / 256.0, 0.18,
                   255, 60, 200, 220)
      end

      return keys, taps
    end
    -- Outside the creep window, fall through to A* nav so the normal
    -- pathfinder still works us closer.
  end

  -- in_range_aim_pre / in_range_aim (PPT): stop, turn to an aim
  -- point exactly, no firing. _aim_locked flips true once corr is
  -- within 1 brad. Pure trig — atan2 to compute target heading,
  -- adiff for the correction, hold/tap turn keys to close it. Does
  -- NOT consult the shell physics simulator.
  --
  -- The two substates share this handler but read from different
  -- aim fields:
  --   in_range_aim_pre → goal.aim_pre_mx/my (1 game-pixel outside
  --                      the pillbox on the chosen-corner side, or
  --                      pillbox center for center aims)
  --   in_range_aim     → goal.aim_mx/my     (the chosen aim corner)
  -- That way the canonical aim corner stays in goal.aim_mx/my and
  -- pre-aim doesn't have to mutate it.
  if goal.substate == "in_range_aim_pre" or goal.substate == "in_range_aim" then
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    local aim_tx, aim_ty
    if goal.substate == "in_range_aim_pre" then
      aim_tx = goal.aim_pre_mx or goal.aim_mx or (goal.mx + 0.5)
      aim_ty = goal.aim_pre_my or goal.aim_my or (goal.my + 0.5)
    else
      aim_tx = goal.aim_mx or (goal.mx + 0.5)
      aim_ty = goal.aim_my or (goal.my + 0.5)
    end
    local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                               aim_tx, aim_ty)
    local corr = U.adiff(info.direction, aim_dir)
    if     corr >  6 then keys = keys | KEY_TURNRIGHT
    elseif corr < -6 then keys = keys | KEY_TURNLEFT
    elseif corr >  1 then taps = taps | KEY_TURNRIGHT
    elseif corr < -1 then taps = taps | KEY_TURNLEFT
    end
    if math.abs(corr) <= 1 then
      -- Use distinct flags per substate so pre's "I'm on the right
      -- SIDE of the pill" decision doesn't pre-pop the in_range_aim
      -- lock that signals "I'm aimed at the chosen corner". Without
      -- this, in_range_aim would inherit a true _aim_locked the
      -- moment it took over and immediately cascade into
      -- in_range_aim_finetune without ever actually settling on
      -- the corner — which is a different point than pre-aim.
      if goal.substate == "in_range_aim_pre" then
        goal._pre_aim_locked = true
      else
        goal._aim_locked = true
      end
    end
    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end
    return keys, taps
  end

  -- in_range_aim_finetune (PPT): brake to 0, then nudge ONE brad per
  -- tick toward the pill CENTER until attack.lua's per-tick sim
  -- (cpf.simulate_shot_angle with info.tank_angle) reports the
  -- trajectory crosses the pill tile. The success transition out
  -- (to shoot_pill) is owned by attack.lua's substate handler — we
  -- just keep tapping while it tells us we haven't hit yet via
  -- goal._finetune_on_pill. May succeed on tick 0 with no taps
  -- needed (corner aim already lined up); typical case is 0-2
  -- taps. Counts taps via goal._finetune_taps so attack.lua can
  -- cap and abort if the geometry won't converge.
  if goal.substate == "in_range_aim_finetune" then
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    -- First tick of finetune is always idle: we don't know whether
    -- the previous substate (in_range_aim) was holding a turn key,
    -- so the engine's firstLeft/firstRight counter could be
    -- anywhere from 0 to 6+. One blank tick guarantees it resets
    -- to 0, so the very first emitted tap below starts at the /8
    -- ramp rate as intended.
    if (goal._finetune_taps or 0) == 0 and not goal._finetune_on_pill then
      goal._finetune_taps = 1
      goal._finetune_burst = 0
      if info.gunrange < C.GUNSIGHT_MAX then
        keys = keys | KEY_MORERANGE
      end
      return keys, taps
    end
    if not goal._finetune_on_pill then
      local pcx = (goal.mx or 0) + 0.5
      local pcy = (goal.my or 0) + 0.5
      local pdir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                              pcx, pcy)
      local pcorr = U.adiff(info.direction, pdir)
      -- 3-tier turn (hold for big corrections, tap for fine):
      -- holds give continuous engine rotation; taps stay at /8 ramp
      -- speed PROVIDED the engine's firstLeft/firstRight counter
      -- doesn't saturate (tank.c:1751 — first 6 turn ticks at /8,
      -- then full speed). The brain runs at half the engine's rate,
      -- so 1 brain tick of held key = 2 engine ticks. That makes
      -- the safe burst max 3 brain ticks (= 6 engine ticks at /8).
      -- A 4-brain-tick burst overshoots: the last 2 engine ticks
      -- of L (delivered AFTER the brain's "release" tick due to a
      -- 1-brain-tick input lag) hit at full speed and produce a
      -- 2.0-brad jump on what looks like an idle tick. Burst 3
      -- brain ticks then 1 idle to force the engine's ramp counter
      -- back to 0, then 3 more — keeps every emitted tap at /8.
      -- The sim check (attack.lua's per-tick simulate_shot_angle) flips
      -- _finetune_on_pill the moment the trajectory crosses the pill,
      -- so a brief overshoot on the hold→tap boundary is caught.
      -- Unified burst: ANY turn-emitting tick (hold OR tap) counts
      -- against the same 3-burst cap, then 1 idle to drain the
      -- engine's firstLeft/firstRight ramp. Holds saturate the ramp
      -- twice as fast as taps (1 brain tick of held key = 2 engine
      -- ticks), so 3 consecutive holds = 6 engine ticks = exactly the
      -- /8 window. The previous design only capped consecutive taps,
      -- so a sustained hold (pcorr stuck >2) would burn the ramp into
      -- full-speed territory and the 1-tick input lag spilled the
      -- final engine tick onto the next brain tick — visible as a
      -- jarring 2-brad jump on what looked like an idle frame.
      local turn_key = nil
      if pcorr > 0 then turn_key = KEY_TURNRIGHT
      elseif pcorr < 0 then turn_key = KEY_TURNLEFT end
      if turn_key then
        local burst = goal._finetune_burst or 0
        if burst < 3 then
          if math.abs(pcorr) > 2 then
            keys = keys | turn_key       -- hold (1 brain = 2 engine ticks)
          else
            taps = taps | turn_key       -- tap (1 engine tick)
          end
          goal._finetune_burst = burst + 1
        else
          -- Idle tick: emit nothing so firstLeft/firstRight resets to 0.
          goal._finetune_burst = 0
        end
      else
        goal._finetune_burst = 0
      end
      goal._finetune_taps = (goal._finetune_taps or 0) + 1
    end
    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end
    return keys, taps
  end

  -- shoot_pill (PPT): park, hold aim on the corner, fire continuously.
  -- Mirrors engage's tap-correction + fire-while-aimed pattern.
  if goal.substate == "shoot_pill" then
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    -- First-tick idle, same reason as finetune: scrub any leftover
    -- engine-side firstLeft/Right ramp from the previous substate so
    -- the very first emitted tap below starts at /8.
    if goal._shoot_first_steer then
      goal._shoot_first_steer = nil
      if info.gunrange < C.GUNSIGHT_MAX then
        keys = keys | KEY_MORERANGE
      end
      return keys, taps
    end
    local aim_tx = goal.aim_mx or (goal.mx + 0.5)
    local aim_ty = goal.aim_my or (goal.my + 0.5)
    local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                               aim_tx, aim_ty)
    local corr = U.adiff(info.direction, aim_dir)
    if     corr >  6 then keys = keys | KEY_TURNRIGHT
    elseif corr < -6 then keys = keys | KEY_TURNLEFT
    elseif corr >  1 then taps = taps | KEY_TURNRIGHT
    elseif corr < -1 then taps = taps | KEY_TURNLEFT
    end
    -- Hold fire once the shells ALREADY in the air will finish the pill:
    -- on-target in-flight >= pill HP means the kill is locked, so more shots are
    -- wasted (matters most in the tank_finish soak case, where the swerve exit is
    -- suppressed). update_shot_accounting re-sims every tick, so if an in-flight
    -- shell diverges/dies short the count drops and we resume firing — no risk of
    -- stopping short.
    local _sp_pill = goal.target_id and world.pills and world.pills[goal.target_id] or nil
    local _sp_hp = _sp_pill and (_sp_pill.health or 0) or 0
    local _sp_in_air = goal._on_target_in_flight or 0
    if math.abs(corr) <= 5 and info.shells > C.SHELL_RESERVE and _sp_in_air < _sp_hp then
      keys = keys | KEY_SHOOT
      -- Pre-fire predictive swerve (same rationale as charge).
      predict_kill_shot_and_swerve(state, world, info, goal, goal.mx, goal.my)
    end
    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end
    return keys, taps
  end

  -- gather_trees (PPT pre-flight): hold position while builder dispatches
  -- the LGM to nearby forest. Tank stays put — moving would force the
  -- planner to re-pick a standoff after gathering.
  if goal.substate == "gather_trees" then
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- approach: brake when near approach point, otherwise fall through to A* nav
  if goal.substate == "approach" then
    -- Prefer precise float position; fall back to tile-snapped if the
    -- planner only produced an integer target.
    if not goal.approach_fx and not goal.approach_mx then
      return nil  -- no setup point yet; fall through to A* nav
    end
    local afx = goal.approach_fx or (goal.approach_mx + 0.5)
    local afy = goal.approach_fy or (goal.approach_my + 0.5)
    if afx then
      local awx = math.floor(afx * 256 + 0.5)
      local awy = math.floor(afy * 256 + 0.5)
      local adist = U.wdist(info.tankx, info.tanky, awx, awy)
      -- FAST_APPROACH normally skips this speed-4 precise creep (stage 2) and
      -- falls through to the throttle dispatcher, where the predict_stop brake
      -- carries the tank in at full speed. A* still homes the steering precisely
      -- on approach_fx (APPROACH_PRECISE_DIST exact-center creep). BUT once the
      -- predict_stop brake has done its job — the tank has slowed (speed <= 4)
      -- OR closed to within 1 tile (FAST_APPROACH_HANDOFF_WU = 256 wu) of the
      -- approach point — we hand back to the normal creep for the final precise
      -- landing onto the spot.
      local fast_handoff = C.FAST_APPROACH and (info.speed <= 4 or adist <= (C.FAST_APPROACH_HANDOFF_WU or 256))
      if adist <= 256 and (not C.FAST_APPROACH or fast_handoff) then
        -- Creep toward the approach point until within 1/2 tile (128 wu), the
        -- generous spot tolerance attack.lua now accepts. Inside that, stop and
        -- rotate to face the pill so the approach-completion facing gate can
        -- fire — no need to creep right onto the exact point any more.
        if adist > 128 then
          local move_dir = U.aim_at(info.tankx, info.tanky, awx, awy)
          local corr = U.adiff(info.direction, move_dir)
          -- Friction-stuck detection (mirrors in_range_position):
          -- if the tank hasn't closed any distance for 8+ ticks,
          -- force re-aim + accelerate instead of relying on
          -- nav_turn_speed which may just spin at speed 0.
          local now_t = state.tick or 0
          if goal._approach_prev_sdist == nil
             or math.abs(adist - goal._approach_prev_sdist) >= 2 then
            goal._approach_prev_sdist  = adist
            goal._approach_stuck_since = now_t
          end
          local stuck_ticks = now_t - (goal._approach_stuck_since or now_t)
          if stuck_ticks >= 8 then
            local k, t = nav_turn_speed(corr, 0, 4, 1)
            keys = keys | k
            taps = taps | t
            if info.speed == 0 then keys = keys | KEY_FASTER end
          else
            local k, t = nav_turn_speed(corr, info.speed, 4, 1)
            keys = keys | k
            taps = taps | t
          end
        else
          -- Within 1/2 tile of the approach point: brake and rotate to face
          -- the pill. The approach point lies on the pill->standoff line, so
          -- "toward the pill" is the inward heading the engage will use next.
          if info.speed > 0 then keys = keys | KEY_SLOWER end
          local pdir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                                  goal.mx + 0.5, goal.my + 0.5)
          local pcorr = U.adiff(info.direction, pdir)
          -- Speed over precision: HOLD the fast-turn key (no taps) so the tank
          -- pivots to the pill as fast as the engine ramp allows. attack.lua
          -- completes the moment it sweeps within FACE_TOL (~10°), so the hard
          -- hold can't oscillate — we stop caring once inside the window.
          if     pcorr > 0 then keys = keys | KEY_TURNRIGHT
          elseif pcorr < 0 then keys = keys | KEY_TURNLEFT
          end
        end
        return keys, taps
      end
    end
  end

  -- ── kill_hardline: nav to a tile beside the pill, fire on every clear
  --    shot until it's dead. Keeps moving toward the target even while
  --    firing. Re-picks the neighbour tile each tick (nearest navigable to
  --    the bot); if none of the 8 are reachable, signals abort to attack.lua.
  if goal.substate == "kill_hardline" then
    local pill = goal.target_id and world.pills and world.pills[goal.target_id]
    if not pill or (pill.health or 0) <= 0 then return nil end
    local pmx, pmy = pill.mx, pill.my
    local pill_wx, pill_wy = U.m2w(pmx), U.m2w(pmy)

    local tmx, tmy = info.tankx >> 8, info.tanky >> 8

    -- Pick a navigable tile beside the pill (nearest the bot first), skipping
    -- any that prior ticks proved unreachable. Stick with the committed tile
    -- until it's blacklisted, so the incremental A* below isn't restarted
    -- every tick.
    goal._hardline_bad = goal._hardline_bad or {}
    local function pick_neighbour()
      local best, bd
      for ddx = -1, 1 do
        for ddy = -1, 1 do
          if not (ddx == 0 and ddy == 0) then
            local cx, cy = pmx + ddx, pmy + ddy
            local k = cy * 256 + cx
            if U.in_map(cx, cy) and not goal._hardline_bad[k] then
              local d = U.wdist(info.tankx, info.tanky, U.m2w(cx), U.m2w(cy))
              if not bd or d < bd then bd = d; best = { mx = cx, my = cy } end
            end
          end
        end
      end
      return best
    end
    if not goal._hardline_mx
       or goal._hardline_bad[goal._hardline_my * 256 + goal._hardline_mx] then
      local c = pick_neighbour()
      if not c then
        goal._hardline_abort = "no navigable tile beside pill"
        if info.speed > 0 then keys = keys | KEY_SLOWER end
        return keys, taps
      end
      goal._hardline_mx, goal._hardline_my = c.mx, c.my
    end

    -- Path to the chosen tile with the TARGET pill's own danger field
    -- subtracted — we're about to kill it, so its fire shouldn't deflect our
    -- approach. set_danger_offset only applies during A*, so skip the
    -- Dijkstra slate (skip_dijkstra=true) for this search.
    local pcontrib = threat.pill_contrib and threat.pill_contrib[pmy * 256 + pmx]
    if pcontrib then cpf.load_danger_offset(pcontrib, -1) end
    local status, nx, ny = cpf.path_to(tmx, tmy,
      goal._hardline_mx, goal._hardline_my,
      info.inboat and 1 or 0, info.shells or 0, info.trees or 0,
      info.mines or 0, info.armour or 40, C.ASTAR_BUDGET, true)
    local trace = cpf.trace_last_search(goal._hardline_mx, goal._hardline_my)
    if pcontrib then cpf.clear_danger_offset() end

    if status == -1 then
      -- Chosen tile is unreachable — blacklist it and re-pick next tick.
      goal._hardline_bad[goal._hardline_my * 256 + goal._hardline_mx] = true
      goal._hardline_mx, goal._hardline_my = nil, nil
      if info.speed > 0 then keys = keys | KEY_SLOWER end
      return keys, taps
    end

    -- trace_last_search returns a FLAT array {x1,y1,x2,y2,...}; waypoint i is
    -- (trace[2*i-1], trace[2*i]) and the count is #trace // 2.
    local tn = trace and (#trace // 2) or 0

    -- Overlay: the path the hardline take is driving (magenta).
    if BRAIN_DEBUG_MODE and viz.is_on("hardline_path") and tn > 1 then
      for i = 2, tn do
        local ax, ay = trace[2 * i - 3], trace[2 * i - 2]
        local bx, by = trace[2 * i - 1], trace[2 * i]
        viz.line("hardline_path", ax + 0.5, ay + 0.5, bx + 0.5, by + 0.5, 255, 0, 255, 220)
      end
      viz.rect("hardline_path", goal._hardline_mx + 0.2, goal._hardline_my + 0.2, goal._hardline_mx + 0.8, goal._hardline_my + 0.8, 255, 0, 255, 160)
    end

    -- KEEP MOVING toward the path (even while firing below). Aim a few tiles
    -- ahead along the trace for a smoother heading than the immediate step.
    local lookx, looky = goal._hardline_mx, goal._hardline_my
    if tn >= 4 then lookx, looky = trace[7], trace[8]
    elseif nx and nx >= 0 and tn < 2 then lookx, looky = nx, ny end
    local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(lookx), U.m2w(looky))
    local mcorr = U.adiff(info.direction, move_dir)
    local k, t = nav_turn_speed(mcorr, info.speed, 48, 4)
    keys = keys | k
    taps = taps | t

    -- Extend gunsight to max so the shot reaches.
    if info.gunrange < C.GUNSIGHT_MAX then keys = keys | KEY_MORERANGE end

    -- Within (shoot distance + 1) tiles of the pill, fire whenever the shot
    -- will hit the pillbox and NOT a base / other pill / allied tank. Shells
    -- travel gunrange/2 map tiles; forests are shot through. shot_path_clear
    -- is the same safety check used elsewhere.
    local shoot_tiles = (info.gunrange or C.GUNSIGHT_MAX) / 2.0
    local fire_w      = (shoot_tiles + 1.0) * 256.0
    local dist_to_pill = U.wdist(info.tankx, info.tanky, pill_wx, pill_wy)
    -- Only fire when actually POINTED at the pill (within tol). kill_hardline
    -- steers toward its movement lookahead, so without this it lobs shells off-
    -- axis. shot_path_clear validates the PILL-AIMED line — it blocks ANY
    -- pillbox (except the target) + ANY base + allied tanks, and lets enemy
    -- tanks through — so gating fire on alignment makes the shot we FIRE match
    -- the line we validated; we never kill a stray pill/base in passing.
    local fire_corr = U.adiff(info.direction, U.aim_at(info.tankx, info.tanky, pill_wx, pill_wy))
    if dist_to_pill <= fire_w and info.shells > C.SHELL_RESERVE
       and math.abs(fire_corr) <= (C.HARDLINE_FIRE_AIM_TOL or 8)
       and shot_path_clear(info, world, pill_wx, pill_wy, pmx, pmy) then
      keys = keys | KEY_SHOOT
    end
    print2(string.format("HARDLINE_DRV t=%d tank=(%d,%d) inboat=%s spd=%d dir=%d look=(%d,%d) mdir=%d corr=%d next=(%s,%s) nextT=%s keys=%d taps=%d",
      state.tick or 0, tmx, tmy, tostring(info.inboat), info.speed or -1, info.direction or -1,
      lookx, looky, move_dir, mcorr, tostring(nx), tostring(ny),
      (nx and nx >= 0) and tostring(U.ttype(nx, ny)) or "?", keys, taps))
    return keys, taps
  end

  -- other: fall through to main steer for A* navigation
  return nil
end

-- Returns true if every intermediate map tile on the line from (x0,y0) to
-- (x1,y1) is water (river or deep sea).  When this holds, a shell fired from
-- a boat travels over those water tiles and strikes the first land square
-- (the target tile), so shooting is valid even from a boat.
local water_corridor_to = U.water_corridor_to

-- =========================================================================
-- Tank combat steering — chase, aim with lead prediction, shoot, jink
-- =========================================================================
local function tank_combat_steer(state, world, info, goal)
  if goal.kind ~= "attack_tank" then return nil end
  local keys = 0
  local taps = 0
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local now = state.tick

  -- Find the current target tank from perception (it moves every tick).
  -- Hunting includes GHOSTS (out-of-sight extrapolations) so we keep driving
  -- to the predicted position; a real sighting is always preferred since it
  -- sorts ahead and ties break on it.
  local perc = state.perc or {}
  local cand_tanks = perc.enemy_tanks or {}
  if perc.ghost_tanks and #perc.ghost_tanks > 0 then
    cand_tanks = {}
    for _, et in ipairs(perc.enemy_tanks or {}) do cand_tanks[#cand_tanks + 1] = et end
    for _, gt in ipairs(perc.ghost_tanks) do cand_tanks[#cand_tanks + 1] = gt end
  end
  local target = nil
  local target_dist = math.huge

  -- Match by proximity to goal position (tank may have moved since goal was set)
  for _, et in ipairs(cand_tanks) do
    local d = U.mdist(et.mx, et.my, goal.mx, goal.my)
    if d < target_dist then
      target_dist = d
      target = et
    end
  end

  -- If we can't see any enemy tank near the goal, find nearest visible one
  if not target or target_dist > 8 then
    local best_d = math.huge
    for _, et in ipairs(cand_tanks) do
      if et.dist < best_d then
        best_d = et.dist
        target = et
      end
    end
  end

  -- Target lost — can't see any enemy tanks
  if not target then
    -- Navigate to last known position
    local nx, ny = cpf_path_to(state, info, goal.mx, goal.my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = nav_turn_speed(corr, info.speed)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    log.reason("steer", { mode = "tank_combat_lost", goal_mx = goal.mx, goal_my = goal.my })
    return keys, taps
  end

  if BRAIN_DEBUG_MODE then print2(string.format("TANK_TARGET t=%d id=%s ghost=%s @(%d,%d) dist=%.1f cand_real=%d cand_ghost=%d", now, tostring(target.id), tostring(target.ghost or false), target.mx, target.my, target_dist, #(perc.enemy_tanks or {}), #(perc.ghost_tanks or {}))) end

  -- Update goal position to track the moving target
  goal.mx = target.mx
  goal.my = target.my
  goal.wx = U.m2w(target.mx)
  goal.wy = U.m2w(target.my)

  -- ── Boat handling for tank combat ───────────────────────────────────────
  -- We engage tanks while afloat now. Two sub-cases when WE are boated:
  --   (a) target is also on water (boat-vs-boat): fall through, fight normally.
  --   (b) target is on LAND: get onto land ASAP. A tank can disembark onto ANY
  --       land tile, so head DIRECTLY to the NEAREST land we can drive straight
  --       at — the nearest drivable land tile (in a 21x21 box) with a clear
  --       straight WATER run to its shore. (Ranking by Dijkstra land-cost was
  --       wrong: that measures cost to *stand on* the tile incl. walking on
  --       land, so it chose a far shore over the near one.) If none has a clear
  --       shot, fall through and fight from the boat.
  -- (Both-on-land needs no special case — it's the normal path below.)
  if info.inboat then
    local ttgt = U.ttype(target.mx, target.my)
    if ttgt ~= C.T_DEEPSEA and ttgt ~= C.T_RIVER then
      local R = 10
      local best_d2, best_mx, best_my = math.huge, nil, nil
      for dy = -R, R do
        for dx = -R, R do
          local cx, cy = tmx + dx, tmy + dy
          if U.in_map(cx, cy) then
            local tt = U.ttype(cx, cy)
            -- drivable land = not water, not a wall (boat climbs onto it)
            if tt ~= C.T_DEEPSEA and tt ~= C.T_RIVER
               and tt ~= C.T_BUILDING and tt ~= C.T_HALFBUILD then
              local d2 = dx * dx + dy * dy
              if d2 < best_d2 and U.water_corridor_to(tmx, tmy, cx, cy) then
                best_d2 = d2; best_mx, best_my = cx, cy
              end
            end
          end
        end
      end
      if best_mx then
        -- Straight charge at the nearest shore (we verified a clear water run).
        -- nav_turn_speed builds speed on the straight; within 2 tiles of the
        -- shore FORCE full throttle (override any brake) so speed clears
        -- BOAT_FAST_EXIT_SPEED (16) and the boat climbs onto soft land instead
        -- of being position-reverted and pinned at the edge.
        local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(best_mx), U.m2w(best_my))
        local corr = U.adiff(info.direction, move_dir)
        local k, t = nav_turn_speed(corr, info.speed)
        keys = keys | k
        taps = taps | t
        local land_dist = U.mdist(tmx, tmy, best_mx, best_my)
        if land_dist <= 2 and math.abs(corr) < 60 then
          keys = (keys | KEY_FASTER) & (~KEY_SLOWER)
        end
        if BRAIN_DEBUG_MODE then print2(string.format("TANK_BOAT_DISEMBARK t=%d -> land(%d,%d) spd=%d corr=%d ld=%d (enemy#%s on land @(%d,%d))", now, best_mx, best_my, info.speed or -1, corr, land_dist, tostring(target.id), target.mx, target.my)) end
        return keys, taps
      end
      -- no straight-shot land in the box → fall through and fight from the boat
    end
  end

  -- Euclidean distance for engage-range check (Manhattan overcounts diagonals)
  local twx = target.wx
  local twy = target.wy
  local _ddx = (twx - info.tankx) / 256.0
  local _ddy = (twy - info.tanky) / 256.0
  local dist_tiles = math.sqrt(_ddx * _ddx + _ddy * _ddy)

  -- ── Draw persistent scan spots from standoff evaluation ──
  if BRAIN_DEBUG_MODE and goal.tank_scan_spots then
    local safe_r = C.ATTACK_SAFE_RADIUS
    for _, s in ipairs(goal.tank_scan_spots) do
      if s.has_los then
        -- Inner box: green=safe, orange=dangerous
        local sr, sg = s.total_score < 10 and 0 or 255, s.total_score < 10 and 200 or 165
        viz.rect("tank_combat_standoff_scan", s.cx - 0.15, s.cy - 0.15, s.cx + 0.15, s.cy + 0.15, sr, sg, 0, 200)
        -- Maneuver tiles (yellow)
        if s.maneuver_tiles then
          for _, t in ipairs(s.maneuver_tiles) do
            viz.rect("tank_combat_standoff_scan", t.x, t.y, t.x + 1, t.y + 1, 255, 255, 0, 40)
          end
        end
        -- Score label
        if s.total_score < 900 then
          viz.text("tank_combat_standoff_scan", s.cx - 1, s.cy - 0.5,
            string.format("D%.0f+X%.0f+T%.0f+A%.0f=%.0f",
              s.score_danger, s.score_crossfire, s.score_terrain, s.score_approach, s.total_score),
            "topleft", 255, 0, 255, 255)
        end
      else
        -- Blocked: red box
        viz.rect("tank_combat_standoff_scan", s.cx - 0.12, s.cy - 0.12, s.cx + 0.12, s.cy + 0.12, 200, 0, 0, 150)
      end
    end
    -- Chosen standoff: green circle + ellipse outline
    if goal.tank_standoff_mx then
      viz.circle("tank_combat_standoff_scan", goal.tank_standoff_mx + 0.5, goal.tank_standoff_my + 0.5, 0.45, 0, 255, 0, 220)
    end
    if goal.tank_standoff_mx then
      viz.line("tank_combat_standoff_scan", goal.mx + 0.5, goal.my + 0.5,
                   goal.tank_standoff_mx + 0.5, goal.tank_standoff_my + 0.5, 0, 255, 0, 100)
      -- Ellipse outline on chosen standoff
      local chosen = nil
      for _, s in ipairs(goal.tank_scan_spots) do
        if s.deg == goal.tank_standoff_deg and s.has_los then chosen = s; break end
      end
      if chosen then
        local edx = goal.mx + 0.5 - chosen.cx
        local edy = goal.my + 0.5 - chosen.cy
        local elen = math.sqrt(edx * edx + edy * edy)
        if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
        local ux, uy = edx / elen, edy / elen
        local vx, vy = -uy, ux
        local rl, rs = 4, safe_r / 3.0
        local segs = 24
        local px, py
        for i = 0, segs do
          local a = (i / segs) * 2 * math.pi
          local eu = math.cos(a) * rl
          local ev = math.sin(a) * rs
          local nx = chosen.cx + eu * ux + ev * vx
          local ny = chosen.cy + eu * uy + ev * vy
          if px then
            viz.line("tank_combat_standoff_scan", px, py, nx, ny, 0, 200, 0, 100)
          end
          px, py = nx, ny
        end
      end
    end
  end

  -- Gunsight at max range
  if info.gunrange < C.GUNSIGHT_MAX then
    keys = keys | KEY_MORERANGE
  end

  -- Pillbox-crossfire disengage (applies in EVERY phase — this is the one
  -- that matters): if our current tile is heavily covered by the enemy's own
  -- pillboxes, break off rather than trade armour into a defended nest.
  local _pdanger = threat.pill_at(tmx, tmy)
  if _pdanger >= C.TANK_COMBAT_DEFENDED_DANGER then
    goal.substate = "disengage"
    log.reason("steer", { mode = "tank_combat_pill_danger",
      danger = _pdanger, thr = C.TANK_COMBAT_DEFENDED_DANGER })
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- Disengage check: flee if outgunned. Skipped entirely during the opening
  -- phase — early aggression is worth more than preserving armour/shells.
  if state.phase ~= "opening"
     and (info.armour <= C.TANK_COMBAT_FLEE_ARMOUR
          or info.shells <= C.TANK_COMBAT_FLEE_SHELLS) then
    goal.substate = "disengage"
    -- Will be invalidated next replan
    log.reason("steer", { mode = "tank_combat_disengage",
      arm = info.armour, sh = info.shells })
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  if dist_tiles > C.TANK_COMBAT_ENGAGE_RANGE then
    -- ── CLOSE: navigate toward standoff position around enemy tank ──
    goal.substate = "close"
    goal._engage_blocked_ticks = 0
    goal._engage_stuck_mx = nil
    goal._engage_stuck_my = nil

    -- Recompute standoff position every 5 ticks as the target moves.
    -- Pick the closest of 8 positions at STANDOFF range around the target
    -- that has clear LOS and passable terrain.
    local nav_mx = goal.tank_standoff_mx or target.mx
    local nav_my = goal.tank_standoff_my or target.my
    if not goal._standoff_tick or (now - goal._standoff_tick) >= 5 then
      goal._standoff_tick = now
      nav_mx, nav_my = target.mx, target.my  -- fallback: drive at enemy
      local R = C.TANK_COMBAT_STANDOFF_RANGE
      local best_nav_dist = math.huge
      -- Use 5° scan (72 directions) for precision once we've committed
      -- to attacking this target. Pool eval uses 45° for speed.
      -- Capacity tier tank_step coarsens the step at lower tiers.
      local _tank_step = (state._capacity and state._capacity.tank_step) or 5
      for deg = 0, 355, _tank_step do
        local rad = math.rad(deg)
        local sx = math.floor(target.mx + 0.5 + math.sin(rad) * R)
        local sy = math.floor(target.my + 0.5 - math.cos(rad) * R)
        if U.in_map(sx, sy) then
          local tt = U.ttype(sx, sy)
          local passable = (C.TERRAIN_COST_LAND[tt] or 9999) < 9999 and not U.is_water(tt)
          if passable and PF.wall_hp_between(sx, sy, target.mx, target.my) == 0 then
            -- Use Dijkstra cost (full danger) for real path cost, O(1) lookup.
            -- Fall back to Manhattan if Dijkstra hasn't reached this tile.
            local d = cpf.dijkstra_lookup_by_kind(cpf.KIND_NORMAL, sx, sy, 0)
            if d >= 1e29 then d = U.mdist(tmx, tmy, sx, sy) * 10 end
            if d < best_nav_dist then
              best_nav_dist = d
              nav_mx, nav_my = sx, sy
            end
          end
        end
      end
      goal.tank_standoff_mx = nav_mx
      goal.tank_standoff_my = nav_my
    end

    local nx, ny = cpf_path_to(state, info, nav_mx, nav_my)

    -- Wall-clear: if the next A* step is a wall tile, stop and shoot it
    -- down. cpf_path_to may route through walls (wall_shoot_cost) and
    -- return a wall tile as the next step.
    if nx and info.shells > C.TANK_COMBAT_FLEE_SHELLS then
      local next_tt = U.ttype(nx, ny)
      if next_tt == C.T_BUILDING or next_tt == C.T_HALFBUILD then
        local wall_wx = U.m2w(nx)
        local wall_wy = U.m2w(ny)
        local wall_dist = U.wdist(info.tankx, info.tanky, wall_wx, wall_wy)
        if wall_dist < 768 then  -- within 3 tiles
          local aim_dir = U.aim_at(info.tankx, info.tanky, wall_wx, wall_wy)
          local corr    = U.adiff(info.direction, aim_dir)
          if     corr >  10 then keys = keys | KEY_TURNRIGHT
          elseif corr < -10 then keys = keys | KEY_TURNLEFT
          elseif corr >   2 then taps = taps | KEY_TURNRIGHT
          elseif corr <  -2 then taps = taps | KEY_TURNLEFT
          end
          if info.speed > 0 then keys = keys | KEY_SLOWER end
          if math.abs(corr) < 8 then
            taps = taps | KEY_SHOOT
          end
          state.wall_clearing = true
          log.reason("steer", {
            mode = "tank_combat_wall_clear",
            wall_mx = nx, wall_my = ny,
            wall_dist = wall_dist, aim_corr = corr,
          })
          return keys, taps
        end
      end
    end

    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = nav_turn_speed(corr, info.speed)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end

    if BRAIN_DEBUG_MODE and viz.is_on("tank_combat_viz") then
      local twx_f = info.tankx / 256.0
      local twy_f = info.tanky / 256.0
      viz.text("tank_combat_viz", twx_f - 1.5, twy_f,
               string.format("CLOSE dist=%.1f (need<=%d) nav=(%d,%d)",
                 dist_tiles, C.TANK_COMBAT_ENGAGE_RANGE, nav_mx, nav_my),
               "topright", 255, 200, 100, 255, 0.45)
    end

    log.reason("steer", { mode = "tank_combat_close",
      dist = dist_tiles, nav_mx = nav_mx, nav_my = nav_my, sub = "close" })
    return keys, taps
  end

  -- ── ENGAGE: in range, aim with lead prediction, shoot, and jink ──
  goal.substate = "engage"

  -- Lead-target prediction: where will the target be when our shell arrives?
  -- TANK_COMBAT_SHELL_SPEED is WU per sim step; brain ticks are 2 sim steps,
  -- so effective shell speed per brain tick = SHELL_SPEED * 2.
  -- Velocity svx/svy is EMA-smoothed WU per brain tick from perception.
  -- Skip lead prediction if target is barely moving (speed ≤ 4 in swamp/stuck)
  -- to avoid EMA residual noise offsetting the aim point.
  local wdist = U.wdist(info.tankx, info.tanky, twx, twy)
  -- Shell speed in WU per BRAIN-tick — the frame the bot samples at, the SAME
  -- unit as svx (which perception derives per brain-tick). Measured empirically
  -- (TANK_ENGAGE shell_meas ≈ 33): the shell advances one SHELL_SPEED (~32) per
  -- brain-tick, NOT two. The old "* 2" assumed 2 sim-steps advanced the shell
  -- per brain-tick, which under-estimated flight ~2x and left the lead trailing
  -- by half. Use SHELL_SPEED directly so flight (and the lead) match reality.
  local shell_speed_per_tick = C.TANK_COMBAT_SHELL_SPEED
  local svx = target.svx or 0
  local svy = target.svy or 0
  -- Skip lead-prediction when target is essentially stationary. Use
  -- the actual smoothed velocity magnitude (WU/tick), not target.speed
  -- which is the engine's SPEEDTYPE in a different scale and isn't
  -- directly comparable. ≤8 wu/tick = ≤0.03 tile/tick = barely moving.
  if (svx * svx + svy * svy) <= 64 then svx = 0; svy = 0 end
  -- Iterated intercept: the shell's flight time depends on the distance to the
  -- LEAD point, not the target's current position. A crossing target's intercept
  -- sits FARTHER out than its current range, so a single-step lead (flight time
  -- from the current range) under-estimates the time and the shells trail behind
  -- the target. Fixed-point iterate t = |tank -> (target + v*t)| / shell_speed;
  -- it converges in a few steps because a shell (64 wu/tick) far outruns a tank
  -- (<=16 wu/tick). Each pass re-measures the range to the freshly-leaded point.
  local shell_travel_ticks = wdist / shell_speed_per_tick
  local pred_wx = twx + svx * shell_travel_ticks
  local pred_wy = twy + svy * shell_travel_ticks
  for _ = 1, 3 do
    shell_travel_ticks = U.wdist(info.tankx, info.tanky, pred_wx, pred_wy) / shell_speed_per_tick
    pred_wx = twx + svx * shell_travel_ticks
    pred_wy = twy + svy * shell_travel_ticks
  end

  -- Lead visualizer ("tank_combat_viz"): shows the shell-travel intercept the
  -- aim is built on, so the leading can be eyeballed frame-by-frame.
  --   red dot + arrow = target now + its smoothed velocity (x8 for visibility)
  --   green dot       = predicted intercept (where the shell and target meet)
  --   orange line     = lead vector (how far ahead of the target we aim)
  --   cyan line       = shell flight path (our tank -> intercept)
  if BRAIN_DEBUG_MODE then
    local t_tx, t_ty = twx / 256.0, twy / 256.0
    local p_tx, p_ty = pred_wx / 256.0, pred_wy / 256.0
    local g_tx, g_ty = info.tankx / 256.0, info.tanky / 256.0
    local lead_tiles = U.wdist(twx, twy, pred_wx, pred_wy) / 256.0
    viz.line("tank_combat_viz", g_tx, g_ty, p_tx, p_ty, 0, 220, 255, 150)
    viz.line("tank_combat_viz", t_tx, t_ty, p_tx, p_ty, 255, 165, 0, 170)
    viz.line("tank_combat_viz", t_tx, t_ty, t_tx + svx * 8 / 256.0, t_ty + svy * 8 / 256.0, 255, 60, 60, 210)
    viz.circle("tank_combat_viz", t_tx, t_ty, 0.3, 255, 50, 50, 180)
    viz.rect("tank_combat_viz", p_tx - 0.5, p_ty - 0.5, p_tx + 0.5, p_ty + 0.5, 0, 255, 90, 90)
    viz.circle("tank_combat_viz", p_tx, p_ty, 0.28, 0, 255, 0, 220)
    viz.text("tank_combat_viz", p_tx - 0.48, p_ty - 0.92, string.format("proj @+%.0ft", shell_travel_ticks), "topleft", 0, 255, 90, 255, 0.4)
  end

  local aim_dir = U.aim_at(info.tankx, info.tanky, pred_wx, pred_wy)
  local aim_corr = U.adiff(info.direction, aim_dir)

  -- Jink: periodic lateral offset to make us harder to hit
  -- Alternate direction every JINK_PERIOD ticks
  local jink_phase = math.floor(now / C.TANK_COMBAT_JINK_PERIOD) % 2
  local jink_offset = jink_phase == 0 and C.TANK_COMBAT_JINK_ANGLE
                                       or -C.TANK_COMBAT_JINK_ANGLE

  -- Turn toward the lead point. Commit to a turn direction and HOLD it so the
  -- engine's firstLeft/firstRight ramp builds to full rate (a released key
  -- resets it — that reset at ~1/8 rate is the "tapping" crawl), but choose the
  -- direction from aim_corr EACH tick so we never coast a stale direction past
  -- the lead:
  --   • |aim_corr| > 2  → commit toward the lead (its sign) and hold
  --   • crossed the lead (sign now opposes the held dir) → REVERSE immediately
  --   • stationary target, on-aim → stop (no jitter)
  --   • otherwise (moving, correct side, in-band) → keep holding to sustain ramp
  -- The previous version latched THROUGH the deadband and drove the stale way
  -- past the lead — swinging AWAY on every zero-crossing (the t=354 bug). This
  -- reverses the instant the sign flips, so it always turns toward the lead.
  local _td = goal._aim_turn_dir or 0
  local _reason
  if aim_corr > 2 then _td = 1; _reason = "aimcorr>+2: swing CW to lead"
  elseif aim_corr < -2 then _td = -1; _reason = "aimcorr<-2: swing CCW to lead"
  elseif svx == 0 and svy == 0 then _td = 0; _reason = "stationary + on-aim: STOP"
  elseif _td == 1 and aim_corr < 0 then _td = -1; _reason = "crossed lead: reverse CW->CCW"
  elseif _td == -1 and aim_corr > 0 then _td = 1; _reason = "crossed lead: reverse CCW->CW"
  else _reason = "in-band: hold " .. ((_td == 1 and "CW") or (_td == -1 and "CCW") or "stop")
  end
  goal._aim_turn_dir = _td
  goal._aim_turn_reason = _reason
  if     _td ==  1 then keys = keys | KEY_TURNRIGHT
  elseif _td == -1 then keys = keys | KEY_TURNLEFT
  end

  -- Crosshair + turn-decision overlay:
  --   yellow line  = where the GUN points NOW (info.direction × gunrange)
  --   cyan line (above) = where it SHOULD point (tank -> lead)
  --   magenta stub at the crosshair = which way it's rotating (CW/CCW)
  --   magenta text = the turn-logic branch that decided this tick
  -- The gap between the yellow (now) and cyan (want) lines IS the aim error.
  if BRAIN_DEBUG_MODE then
    local _cr = (info.direction or 0) * C.TWO_PI / 256
    local _sl = 128 * (info.gunrange or 14)
    local _gx, _gy = info.tankx / 256.0, info.tanky / 256.0
    local _cx = (info.tankx + math.sin(_cr) * _sl) / 256.0
    local _cy = (info.tanky - math.cos(_cr) * _sl) / 256.0
    viz.line("tank_combat_viz", _gx, _gy, _cx, _cy, 255, 255, 0, 210)
    viz.circle("tank_combat_viz", _cx, _cy, 0.18, 255, 255, 0, 230)
    viz.line("tank_combat_viz", _cx, _cy, _cx + math.cos(_cr) * _td * 0.8, _cy + math.sin(_cr) * _td * 0.8, 255, 0, 255, 230)
    viz.text("tank_combat_viz", _gx + 0.4, _gy + 0.5, string.format("TURN %s  aimcorr=%+.0f", (_td == 1 and "CW") or (_td == -1 and "CCW") or "STOP", aim_corr), "topleft", 255, 0, 255, 255, 0.4)
    viz.text("tank_combat_viz", _gx + 0.4, _gy + 0.86, _reason or "", "topleft", 255, 0, 255, 220, 0.38)
  end

  -- Fire when aimed — wider tolerance because lead prediction compensates.
  --
  -- Stuck-fire: when aim is on the enemy but a wall keeps blocking the
  -- shell path tick after tick (two tanks dug in across a wall), bypass
  -- the LOS gate after TANK_COMBAT_STUCK_FIRE_TICKS so the shells chip
  -- the wall down and eventually open LOS. Without this the bot just
  -- stares at the wall forever, "engaging" but never firing.
  -- Fire only when the gun is genuinely ON the lead point. The lead angle at
  -- combat range is only ~10 brads, so the old ±8 gate was nearly as wide as the
  -- whole lead — it loosed shots while the gun was still short of the lead,
  -- sitting on the tank (aim_corr +6 = ~0.5 tile ahead of the tank). ±3 brads is
  -- ~0.3-0.4 tile of lateral slop at 5-7 tiles: on the lead, not on the tank.
  local _aim_ok    = math.abs(aim_corr) < 3
  local _shells_ok = info.shells > C.TANK_COMBAT_FLEE_SHELLS
  -- Stuck only counts when WE are also pinned in place — if the bot is
  -- still moving around looking for a clean angle it isn't stuck yet,
  -- it's just mid-reposition. Reset whenever our tile changes.
  local _stuck_in_place = (goal._engage_stuck_mx == tmx
                           and goal._engage_stuck_my == tmy)
  goal._engage_stuck_mx = tmx
  goal._engage_stuck_my = tmy
  if _aim_ok and _shells_ok then
    local _clear = shot_path_clear(info, world, pred_wx, pred_wy,
                                   math.floor(pred_wx) >> 8,
                                   math.floor(pred_wy) >> 8)
    if _clear then
      keys = keys | KEY_SHOOT
      if BRAIN_DEBUG_MODE then print2(string.format("TANK_FIRE t=%d id=%s ghost=%s @(%d,%d) dist=%.1f aim_corr=%.0f", now, tostring(target.id), tostring(target.ghost or false), target.mx, target.my, dist_tiles, aim_corr)) end
      goal._engage_blocked_ticks = 0
    elseif _stuck_in_place then
      goal._engage_blocked_ticks = (goal._engage_blocked_ticks or 0) + 1
      if goal._engage_blocked_ticks >= C.TANK_COMBAT_STUCK_FIRE_TICKS then
        keys = keys | KEY_SHOOT
        log.event("tank_combat_stuck_fire",
          string.format("blocked_ticks=%d aim_corr=%.0f dist=%.1f",
                        goal._engage_blocked_ticks, aim_corr, dist_tiles))
      end
    else
      -- Blocked but we moved this tick — not stuck, reset.
      goal._engage_blocked_ticks = 0
    end
  else
    -- Not aimed yet (still turning) or out of shells: don't accrue stuck count.
    goal._engage_blocked_ticks = 0
  end

  -- Per-tick engage trace: the lead + aim + fire state each tick. perc = the
  -- position perception handed us; v = smoothed velocity; flight/lead/pred = the
  -- iterated shell-travel intercept the aim is built on.
  if BRAIN_DEBUG_MODE then
    local _turn = (goal._aim_turn_dir == 1 and "CW") or (goal._aim_turn_dir == -1 and "CCW") or "-"
    local _lead = U.wdist(twx, twy, pred_wx, pred_wy) / 256.0
    print2(string.format("TANK_ENGAGE t=%d tgt#%s%s perc=(%d,%d) d=%.1f v=(%.0f,%.0f) flight=%.1f lead=%.1f pred=(%d,%d) aimcorr=%+.0f turn=%s fire=%s", now, tostring(target.id), target.ghost and "G" or "", target.mx, target.my, dist_tiles, svx, svy, shell_travel_ticks, _lead, math.floor(pred_wx) >> 8, math.floor(pred_wy) >> 8, aim_corr, _turn, tostring((keys & KEY_SHOOT) ~= 0)))
  end

  -- Distance control: maintain optimal range with jinking
  if dist_tiles < C.TANK_COMBAT_TOO_CLOSE then
    -- Too close: reverse away
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    -- Jink by turning slightly off-axis — but ONLY when the aim turn above
    -- isn't already holding a turn key. Otherwise the jink tap presses the
    -- opposite turn in the same engine read, the two cancel to a near-zero
    -- net turn, and we're back to slow-motion rotation. Aim wins; jink only
    -- fires when aim is inside the deadband (no hold active).
    if (keys & (KEY_TURNLEFT | KEY_TURNRIGHT)) == 0 then
      if jink_offset > 0 then
        taps = taps | KEY_TURNRIGHT
      else
        taps = taps | KEY_TURNLEFT
      end
    end
  elseif dist_tiles <= C.TANK_COMBAT_ENGAGE_RANGE then
    -- In range: hold moderate speed for evasion, use jink
    local desired_speed = 12  -- keep moving to dodge
    if info.speed > desired_speed + 4 then
      keys = keys | KEY_SLOWER
    elseif info.speed < desired_speed then
      keys = keys | KEY_FASTER
    end
  end

  -- Real-time combat readout — per-tick LIVE state (distinct from the ~1 s
  -- d=/c= scoring label). Drawn in WORLD SPACE floating beside the TARGET tank
  -- so it TRACKS the enemy and scales to multiple tanks (each engaged tank
  -- carries its own readout) instead of a single fixed screen corner that could
  -- only ever show one. Shows live range, target velocity, the iterated lead +
  -- flight time, aim error, the SUSTAINED turn command (hold-vs-tap), and the
  -- fire-gate outcome. Turns green the tick it actually fires.
  if BRAIN_DEBUG_MODE and viz.is_on("tank_combat_viz") then
    local firing = (keys & KEY_SHOOT) ~= 0
    local aim_ok = math.abs(aim_corr) < 3
    local shells_ok = info.shells > C.TANK_COMBAT_FLEE_SHELLS
    local _td = goal._aim_turn_dir or 0
    local turn_s = (_td == 1 and "HOLD-R") or (_td == -1 and "HOLD-L") or "--"
    local vmag = math.sqrt(svx * svx + svy * svy)
    local lead_t = U.wdist(twx, twy, pred_wx, pred_wy) / 256.0
    local fire_s = firing and "SHOOTING"
                   or (not shells_ok and "hold:low-shells")
                   or (not aim_ok and "hold:off-aim")
                   or "hold:LOS"
    local hx, hy = twx / 256.0 - 4.7, twy / 256.0 - 1.8
    local lines = {
      string.format("#%s%s  d=%.1ft  |v|=%.0f", tostring(target.id), target.ghost and "G" or "", dist_tiles, vmag),
      string.format("lead=%.1ft  hit@+%.0ft", lead_t, shell_travel_ticks),
      string.format("aim=%+.0f  turn=%s", aim_corr, turn_s),
      "FIRE: " .. fire_s,
    }
    for i, line in ipairs(lines) do
      local r, g, b = 0, 230, 255
      if firing then r, g, b = 0, 255, 90 end
      viz.text("tank_combat_viz", hx, hy + (i - 1) * 0.34, line, "topleft", r, g, b, 255, 0.42)
    end
  end

  log.reason("steer", {
    mode = "tank_combat_engage",
    dist = dist_tiles, aim_corr = aim_corr,
    lead_wx = pred_wx, lead_wy = pred_wy,
    speed = target.speed, dir = target.obj and target.obj.direction or 0,
    ob_speed = target.obj and target.obj.speed or -1,
    spd_wu = target.speed / 4,
    wdist = wdist, shell_t = shell_travel_ticks,
    jink = jink_offset,
    firing = (keys & KEY_SHOOT) ~= 0,
  })
  return keys, taps
end

function M.steer(state, world, info, goal)
  local _t_steer_start = BRAIN_PROFILE and clock_us() or 0
  local _t_phase = _t_steer_start
  -- Mark "steer" as the current main so cpf.path_to / cost_to wrappers
  -- attribute their timing as "  steer/path_to". Overwrite-style — no
  -- cleanup needed; the next major section's set_main replaces it.
  if BRAIN_PROFILE then
    opt.set_main("steer")
    -- Reset path-sub accumulators so this tick's nav-dispatch/path
    -- breakdown reflects only this tick's work.
    _path_search_us    = 0
    _path_trace_us     = 0
    _path_lookahead_us = 0
    _path_method       = "dij"
  end
  local keys = 0
  local taps = 0
  local tmx  = info.tankx >> 8
  local tmy  = info.tanky >> 8
  state._steer_lx = nil
  state._steer_ly = nil
  if goal.kind ~= "kill_lgm" then
    state._kill_lgm_halt = false
  end

  -- Per-tile stuck-recovery: re-stamp the dynamic blacklist into the overlay
  -- (init.lua wipes it each tick) and watch progress toward pf.next_mx/my.
  stuck_recovery(state, info, goal)
  local _t_after_stuck = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  steer/stuck_recovery done %.2f ms",
                      (_t_after_stuck - _t_phase) / 1000))
    _t_phase = _t_after_stuck
  end

  -- ── Global cliff safety: runs BEFORE any goal-specific self-contained
  -- steering so no goal can drive us off a deep-sea edge at speed.
  -- Philosophy: only intervene when momentum is the problem. At low speed
  -- normal steering can stop itself, so being conservative creates "stuck
  -- at water's edge" cases. Only active above CLIFF_MIN_SPEED, look-ahead
  -- is tight (~stopping distance), and we only brake if water is within
  -- the minimum runway needed.
  -- escape_water is explicitly exempt (it's how we recover FROM water).
  local CLIFF_MIN_SPEED = 12   -- below this, no preemptive brake
  if not info.inboat and goal.kind ~= "escape_water"
     and info.speed >= CLIFF_MIN_SPEED then
    -- Stopping distance ≈ speed * 6 wu; clamp to at most 3 tiles of look.
    local look_wu = math.min(info.speed * 6, 768)
    local steps   = math.max(1, math.ceil(look_wu / 256))
    local sdir    = U.bsin(info.direction)
    local cdir    = U.bcos(info.direction)
    local twx, twy = info.tankx / 256.0, info.tanky / 256.0
    local trigger_step, trigger_mx, trigger_my
    for i = 1, steps do
      local amx = (info.tankx + sdir * 2 * i) >> 8
      local amy = (info.tanky - cdir * 2 * i) >> 8
      if U.ttype(amx, amy) == C.T_DEEPSEA then
        trigger_step, trigger_mx, trigger_my = i, amx, amy
        break
      end
      -- Pale yellow square for scanned-clear tiles + small label so
      -- they're not confused with the bright pf.next overlay.
      if BRAIN_DEBUG_MODE then
        viz.rect("cliff_safety",
                 amx + 0.15, amy + 0.15, amx + 0.85, amy + 0.85,
                 255, 255, 150, 50)
        viz.text("cliff_safety",
                 amx + 0.5, amy + 0.95, "cliff safety",
                 "center", 255, 255, 150, 180, 0.5)
      end
    end
    if trigger_step then
      -- Trigger viz: orange tile + line from tank + CLIFF BRAKE label.
      -- The braking BEHAVIOR still fires (return KEY_SLOWER below) —
      -- only the visual markers are gated.
      if BRAIN_DEBUG_MODE then
        viz.rect("cliff_safety",
                 trigger_mx, trigger_my, trigger_mx + 1, trigger_my + 1,
                 255, 140, 0, 230)
        viz.line("cliff_safety",
                 twx, twy, trigger_mx + 0.5, trigger_my + 0.5,
                 255, 140, 0, 200)
        viz.text("cliff_safety",
                 trigger_mx + 0.5, trigger_my - 0.4,
                 string.format("CLIFF BRAKE  step=%d  speed=%d  look=%.1ft",
                               trigger_step, info.speed, look_wu / 256.0),
                 "center", 255, 160, 40, 255)
      end
      log.reason("steer", {
        mode = "global_cliff_brake", goal_kind = goal.kind,
        tile_mx = trigger_mx, tile_my = trigger_my,
        step = trigger_step, speed = info.speed,
      })
      if BRAIN_PROFILE then
        opt(string.format("  steer/cliff_safety done %.2f ms",
                          (clock_us() - _t_phase) / 1000))
      end
      -- This is a real (drowning) brake, NOT the cautious near-ally creep. If the
      -- cautious guard tagged the creep earlier this tick, clear it so init's
      -- TAKE_CRAWL leaves THIS KEY_SLOWER intact instead of sailing into the water.
      state._cautious_lookahead_held = nil
      return KEY_SLOWER, 0
    end
  end
  if BRAIN_PROFILE then
    local _t_now = clock_us()
    opt(string.format("  steer/cliff_safety done %.2f ms",
                      (_t_now - _t_phase) / 1000))
    _t_phase = _t_now
  end

  -- Tank combat: self-contained steering for attack_tank goals.
  -- After it runs, mask KEY_FASTER if water is DIRECTLY adjacent (1 tile)
  -- so a stopped tank can't accelerate into deep sea. Narrower than the
  -- global brake above — only the immediate next tile counts here because
  -- if we're moving we'd have already been caught above.
  if goal.kind == "attack_tank" then
    local k, t = tank_combat_steer(state, world, info, goal)
    if k then
      if not info.inboat then
        local sdir    = U.bsin(info.direction)
        local cdir    = U.bcos(info.direction)
        local amx = (info.tankx + sdir * 2) >> 8   -- 1 tile ahead only
        local amy = (info.tanky - cdir * 2) >> 8
        if U.ttype(amx, amy) == C.T_DEEPSEA then
          k = (k & ~KEY_FASTER) | KEY_SLOWER
          if BRAIN_DEBUG_MODE then
            viz.rect("cliff_safety", amx + 0.1, amy + 0.1, amx + 0.9, amy + 0.9,
                         255, 140, 0, 180)
          end
        end
      end
      if BRAIN_PROFILE then
        opt(string.format("  steer/tank_combat done %.2f ms",
                          (clock_us() - _t_phase) / 1000))
      end
      return k, t
    end
  end

  -- Pill placement: self-contained steering for all pill_place substates
  if goal.kind == "pill_place" then
    local k, t = pill_place_steer(state, world, info, goal)
    if k then
      if BRAIN_PROFILE then
        opt(string.format("  steer/pill_place done %.2f ms",
                          (clock_us() - _t_phase) / 1000))
      end
      return k, t
    end
  end

  -- Attack pill: aim, engage, rush, plan_position substates
  if goal.kind == "attack_pill" then
    local k, t = attack_pill_steer(state, world, info, goal)
    if k then
      if BRAIN_PROFILE then
        opt(string.format("  steer/attack_pill done %.2f ms",
                          (clock_us() - _t_phase) / 1000))
      end
      return k, t
    end
  end

  -- Reposition shoot: once driven up to our own pill, stop and shoot it
  -- down. Returns nil while still approaching, so we fall through to the
  -- general capture navigation that drives us in.
  if goal.kind == "capture_pill" and goal.reposition then
    local k, t = reposition_steer(state, world, info, goal)
    if k then
      if BRAIN_PROFILE then
        opt(string.format("  steer/reposition done %.2f ms",
                          (clock_us() - _t_phase) / 1000))
      end
      return k, t
    end
  end

  -- De-mine interrupt (demine.lua pushed a kill_mine goal): stop, put the
  -- crosshair on the mine, fire. Always self-contained — never falls
  -- through to generic navigation (the mine is a shot, not a destination).
  if goal.kind == "kill_mine" then
    local k, t = demine_steer(state, world, info, goal)
    if BRAIN_PROFILE then
      opt(string.format("  steer/demine done %.2f ms",
                        (clock_us() - _t_phase) / 1000))
    end
    return k or 0, t or 0
  end

  -- Sub-anchor for steer/nav-* breakdowns. _t_phase is the fall-through
  -- start (right after cliff_safety completed and the goal-specific
  -- dispatches all returned NIL).
  local _t_nav_start = _t_phase
  local _t_nav_dispatch_start = BRAIN_PROFILE and clock_us() or 0
  -- Sub-breakdowns of nav-dispatch — accumulate across the elseif
  -- branches and emit at each return point. `path` covers cpf_path_to
  -- + path_lookahead in BOTH the refuel branch and the general branch
  -- (only one fires per tick). `los` covers the attack_in_range LOS
  -- check. `setup` is computed at emit time as total - path - los.
  local _t_path_us = 0
  local _t_los_us  = 0

  -- Perception cache: read once at top of steer
  local perc = state.perc or {}
  local under_fire = perc.under_fire or false
  -- Race-mode captures relax threat-driven speed caps so we beat the opponent
  -- to the dead pill / base. Wsim lethality rejection is the safety net.
  local race_mode = (goal and goal.race_mode) or false

  -- For goals where the tank needs to be very close to the exact tile
  -- center before the next phase, tighten the centering tolerance.
  --   * attack_pill approach/aim/detree — sets up the standoff, where
  --     swerve/charge math expects the tank to be centered on its tile.
  -- (rescue_lgm uses the LGM's precise sub-tile world position as the
  --  steering target, with the normal loose 64 wu tolerance — no need
  --  to chase exact tile center, just meet the LGM where it actually
  --  is. pill_place has its own steering function above.)
  -- Nav modes: "precision" = slow creep to exact tile center (pill aim/approach)
  --            "plow"      = maintain speed through destination, no braking
  --            nil/default = normal braking
  local nav_mode = goal.nav_mode
  local needs_exact_center = nav_mode == "precision"
       or (goal.kind == "attack_pill"
           and (goal.substate == "aim"
                or goal.substate == "detree"))
  local plow_through = nav_mode == "plow"
       or goal.kind == "capture_base"
       or goal.kind == "capture_pill"
  local center_tol = needs_exact_center and 16 or 64

  -- Determine desired movement direction
  local move_dir    = nil
  local target_dist = 0x7FFF
  local goal_dist   = 0x7FFF

  if goal.kind == "escape_water" then
    -- Bypass A*: steer directly at dry land
    move_dir    = U.aim_at(info.tankx, info.tanky, goal.wx, goal.wy)
    target_dist = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    goal_dist   = target_dist

  -- attack_pill: plan_position just visualizes, no steering needed.
  -- Falls through to general navigation for position substate.

  elseif goal.kind == "wait_for_lgm"
         and goal.mx == (info.tankx >> 8) and goal.my == (info.tanky >> 8) then
    -- ON the wait spot: stand still and let the LGM finish whatever he's
    -- doing (farming, opportunistic build) before chasing new goals.
    -- When goal.mx/my is a danger-aware SAFE SPOT elsewhere (picked by
    -- pick_wait_spot — parked tile under fire), this branch doesn't match
    -- and the goal falls through to general navigation, which drives to
    -- goal.wx/wy like any other destination; once there, this branch takes
    -- over and parks.
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    if BRAIN_PROFILE then
      local _total_us = clock_us() - _t_nav_dispatch_start
      local _setup_us = _total_us - _t_path_us - _t_los_us
      if _setup_us < 0 then _setup_us = 0 end
      local _path_misc_us = _t_path_us
                           - _path_search_us - _path_trace_us - _path_lookahead_us
      if _path_misc_us < 0 then _path_misc_us = 0 end
      -- 4-space indent = level-3 (subsub) attached to the next 2-space
      -- sub line (nav-dispatch/path). Order matters: subsubs are
      -- emitted BEFORE their parent so the parser can attach them.
      opt(string.format("    search (%s) done %.2f ms",
                        _path_method == "dij" and "dijkstra" or "A*",
                        _path_search_us / 1000))
      opt(string.format("    trace done %.2f ms",     _path_trace_us     / 1000))
      opt(string.format("    lookahead done %.2f ms", _path_lookahead_us / 1000))
      opt(string.format("    misc done %.2f ms",      _path_misc_us      / 1000))
      opt(string.format("  steer/nav-dispatch/path done %.2f ms",  _t_path_us / 1000))
      opt(string.format("  steer/nav-dispatch/los done %.2f ms",   _t_los_us / 1000))
      opt(string.format("  steer/nav-dispatch/setup done %.2f ms", _setup_us / 1000))
    end
    return keys, taps

  elseif goal.kind == "kill_lgm" then
    -- Reset the sticky-engage flag when the target_id changes (new LGM
    -- target this goal cycle).
    if state._kill_lgm_engaged_id
       and state._kill_lgm_engaged_id ~= goal.target_id then
      state._kill_lgm_engaged_id = nil
    end
    -- Match the goal's target LGM in perception's predicted list.
    local target_mx, target_my = goal.mx, goal.my
    local matched_elm = nil
    if state.perc and state.perc.enemy_lgms then
      for _, elm in ipairs(state.perc.enemy_lgms) do
        if (goal.target_id and elm.idnum == goal.target_id)
           or (elm.mx == goal.mx and elm.my == goal.my) then
          matched_elm = elm
          if elm.predicted_mx and elm.predicted_my then
            target_mx, target_my = elm.predicted_mx, elm.predicted_my
          end
          break
        end
      end
    end
    -- When in shooting range, aim heading at the SUB-TILE predicted
    -- world position (predicted_wx/wy), not the path's tile-center
    -- lookahead.  At realistic LGM speeds the lead is < 1 tile, so
    -- predicted_mx/my == current tile and tile-snapped pathing loses
    -- the lateral lead entirely.  Bypassing the pathfinder here also
    -- keeps the tank turret pointed precisely at the lead point for
    -- the firing block in init.lua to gate on aim_corr.
    local in_shooting_range = matched_elm and matched_elm.dist
                              and matched_elm.dist <= C.KILL_LGM_SHOOT_RANGE
    -- Sticky engage: once we've crossed into shooting range for THIS
    -- LGM (same target_id), stay in engage mode even if the LGM later
    -- drifts back out of range.  Reverting to approach would brake the
    -- tank and start a chase the LGM can win; staying in engage keeps
    -- the turret on it and lets us keep firing (or wait for it to
    -- reenter range).  Cleared when the goal changes or target_id
    -- changes (see clear at top of dispatcher when goal.kind ~=
    -- "kill_lgm").
    if in_shooting_range then
      state._kill_lgm_engaged_id = goal.target_id
    end
    local sticky_engaged = state._kill_lgm_engaged_id
                       and state._kill_lgm_engaged_id == goal.target_id
                       and matched_elm
    local treat_as_engaged = in_shooting_range or sticky_engaged
    state._kill_lgm_halt = false
    if treat_as_engaged and matched_elm and matched_elm.predicted_wx then
      move_dir    = U.aim_at(info.tankx, info.tanky,
                              matched_elm.predicted_wx,
                              matched_elm.predicted_wy)
      -- target_dist = 0 tells the throttle dispatcher we've arrived,
      -- and the _kill_lgm_halt flag below makes it brake even when
      -- the generic brake-when-close code would still let the tank
      -- creep at ~20 wu/tick.  Combined: full stop, only the turn
      -- keys fire so the tank rotates in place to aim.
      target_dist = 0
      state._steer_lx = matched_elm.predicted_mx
      state._steer_ly = matched_elm.predicted_my
      state._kill_lgm_halt = true
    else
      -- Out of shooting range: drive to the ENGAGE SPOT (the closest
      -- in-range boundary tile, computed live in refresh_kill_lgm),
      -- not to the LGM tile itself.  The engage spot sits at the
      -- maximum shooting distance from the LGM along whichever
      -- approach is cheapest — bot stops naturally on arrival and
      -- only needs to pivot + fire.  Pulled live from pool_cache[13]
      -- since state.goal.shoot_mx is only refreshed on replans.
      local engage_mx, engage_my = target_mx, target_my
      local pc13 = state.pool_cache and state.pool_cache[13]
      if pc13 and pc13.goal and pc13.goal.shoot_mx and pc13.goal.shoot_my then
        engage_mx = pc13.goal.shoot_mx
        engage_my = pc13.goal.shoot_my
      end
      state._kill_lgm_engage_mx = engage_mx
      state._kill_lgm_engage_my = engage_my
      local nx, ny = cpf_path_to(state, info, engage_mx, engage_my)
      if nx then
        local lx, ly = path_lookahead(state, info, nx, ny)
        state._steer_lx = lx
        state._steer_ly = ly
        move_dir    = U.aim_at(info.tankx, info.tanky, U.m2w(lx), U.m2w(ly))
        target_dist = U.wdist(info.tankx, info.tanky, U.m2w(lx), U.m2w(ly))
      end
    end
    goal_dist = U.wdist(info.tankx, info.tanky, U.m2w(goal.mx), U.m2w(goal.my))

  elseif goal.kind == "refuel_at_base" then
    -- wait_for_ally: an ally is camping our target base, so we park at
    -- a low-danger tile in the surrounding 11x11 square (picked at
    -- substate entry in init.lua) instead of crowding the base.  Fall
    -- back to braking if no park spot was found.
    local nav_mx, nav_my = goal.mx, goal.my
    if goal.substate == "wait_for_ally" then
      if goal.wait_mx and goal.wait_my then
        nav_mx, nav_my = goal.wait_mx, goal.wait_my
      else
        return keys, taps  -- no park spot — brake in place
      end
    end
    -- Navigate to the (possibly-overridden) destination if not on it
    -- yet; brake if already there.
    local on_base = (tmx == nav_mx and tmy == nav_my)
    if not on_base then
      local _t_p0 = BRAIN_PROFILE and clock_us() or 0
      local nx, ny = cpf_path_to(state, info, nav_mx, nav_my)
      if nx then
        local _t_la0 = BRAIN_PROFILE and clock_us() or 0
        local lx, ly = path_lookahead(state, info, nx, ny)
        if BRAIN_PROFILE then
          _path_lookahead_us = _path_lookahead_us + (clock_us() - _t_la0)
        end
        state._steer_lx = lx
        state._steer_ly = ly
        move_dir    = U.aim_at(info.tankx, info.tanky, U.m2w(lx), U.m2w(ly))
        target_dist = U.wdist(info.tankx, info.tanky, U.m2w(lx), U.m2w(ly))
      end
      if BRAIN_PROFILE then _t_path_us = _t_path_us + (clock_us() - _t_p0) end
      goal_dist = U.wdist(info.tankx, info.tanky, U.m2w(nav_mx), U.m2w(nav_my))

      -- Nav debug overlay (same as the generic navigate branch below)
      if BRAIN_DEBUG_MODE then
        local pf = state.pf
        local twx, twy = info.tankx / 256.0, info.tanky / 256.0
        if pf.next_mx and pf.next_mx >= 0 then
          viz.rect("steering_text", pf.next_mx, pf.next_my, pf.next_mx + 1, pf.next_my + 1,
                       255, 255, 0, 200)
          -- Show which pathfinder produced this step. path_to picks
          -- Dijkstra slate KIND_NORMAL first; falls back to A* on miss.
          local method = cpf._last_method or "?"
          local label  = (method == "dij") and "dij/NORMAL" or method
          viz.text("steering_text", pf.next_mx + 0.05, pf.next_my + 0.05, label,
                   "topleft", 255, 255, 0, 220)
        end
        if state._steer_lx then
          viz.circle("nav_lookahead_marker", state._steer_lx + 0.5, state._steer_ly + 0.5, 0.4,
                         255, 0, 255, 230)
        end
        if move_dir and state._steer_lx then
          viz.line("nav_lookahead_marker", twx, twy, state._steer_lx + 0.5, state._steer_ly + 0.5,
                       255, 0, 255, 160)
        end
        -- Nav destination (white circle at the destination tile)
        viz.circle("pf_destination", nav_mx + 0.5, nav_my + 0.5, 0.3, 255, 255, 255, 180)
        if state.next_goal and state.next_goal.wx and state.next_goal.wy then
          local ngx = state.next_goal.wx / 256.0
          local ngy = state.next_goal.wy / 256.0
          viz.line("pf_destination", nav_mx + 0.5, nav_my + 0.5, ngx, ngy, 100, 0, 140, 220)
          viz.circle("pf_destination", ngx, ngy, 0.25, 100, 0, 140, 220)
        end
      end
    end

  elseif goal.kind ~= "none" then
    -- For attack_pill: navigate to the planned standoff position (pre-scored by
    -- goals.lua for land quality, crossfire, pushback, and escape cost).
    -- Fall back to the old "approach from current direction" heuristic only if
    -- no standoff was planned (e.g. goal was created before the planner existed).
    local nav_mx, nav_my = goal.mx, goal.my
    local nav_wx, nav_wy = goal.wx, goal.wy
    -- attack_base: park beside the base (not on top of it) for a clean shot.
    -- Prefer the LOWEST-COST REACHABLE tile in the base's 8-neighbourhood, via
    -- the per-tick dijkstra slate (O(1) smart_cost_dij_only) — that's the
    -- cheapest point-blank spot a tank can actually get to. If NONE of the
    -- neighbours is reachable (base walled in / behind water), fall back to the
    -- closest non-water adjacent tile by Manhattan (old behaviour).
    if goal.kind == "attack_base" then
      local bmx, bmy = goal.mx, goal.my
      local bf = info.inboat and 1 or 0
      local best_amx, best_amy, best_c = nil, nil, math.huge
      -- Already in the base's 8-neighbourhood? STAY: any adjacent tile is
      -- point-blank, so hold this one, face the base and shoot. Without this
      -- the lowest-cost pick below can prefer a DIFFERENT adjacent tile and
      -- the tank crawls around the base instead of firing (seen when a
      -- capture_base drive-over degrades to attack_base after the base
      -- restocked above capturable armour).
      if math.abs(tmx - bmx) <= 1 and math.abs(tmy - bmy) <= 1
         and not (tmx == bmx and tmy == bmy) then
        best_amx, best_amy = tmx, tmy
      else
      for dy = -1, 1 do
        for dx = -1, 1 do
          if dx ~= 0 or dy ~= 0 then
            local cx, cy = U.mclamp(bmx + dx), U.mclamp(bmy + dy)
            if not U.is_water(U.ttype(cx, cy)) then
              local c = cpf.smart_cost_dij_only(cpf.KIND_NORMAL, cx, cy, bf)
              if c and c < best_c then best_c = c; best_amx, best_amy = cx, cy end
            end
          end
        end
      end
      end
      if not best_amx then
        -- Nothing reachable via the slate → closest non-water adjacent tile.
        local best_d = math.huge
        for _, delta in ipairs({{-1,0},{1,0},{0,-1},{0,1}}) do
          local cx, cy = U.mclamp(bmx + delta[1]), U.mclamp(bmy + delta[2])
          if not U.is_water(U.ttype(cx, cy)) then
            local d = math.abs(cx - tmx) + math.abs(cy - tmy)
            if d < best_d then best_d = d; best_amx, best_amy = cx, cy end
          end
        end
      end
      if best_amx == tmx and best_amy == tmy then
        -- Staying put (already beside the base): nav to our own tile — no
        -- engage-point reroute either, just aim and fire from here.
        nav_mx, nav_my = tmx, tmy
        nav_wx, nav_wy = info.tankx, info.tanky
      elseif best_amx then
        -- Crossfire-aware standoff: stop short at the closest path tile we can
        -- still shell the base from while staying out of enemy/neutral pill fire.
        -- Falls back to best_amx (rush right up) when no such tile exists.
        local emx, emy = pick_base_engage_point(goal, state, world, info, best_amx, best_amy)
        nav_mx, nav_my = emx or best_amx, emy or best_amy
        nav_wx, nav_wy = U.m2w(nav_mx), U.m2w(nav_my)
      end
    end
    -- Rescue LGM: chase the LGM's LIVE sub-tile world position (not the
    -- cached goal.wx/wy from when the goal was created — the LGM moves).
    -- Tile coords still come from goal.mx/my for the A* path target.
    if goal.kind == "rescue_lgm" then
      nav_mx = info.man_x >> 8
      nav_my = info.man_y >> 8
      nav_wx = info.man_x
      nav_wy = info.man_y
    end
    if goal.kind == "attack_pill" or goal.kind == "pill_place" then
      if goal.wall_shield and goal.substate == "approach" and goal.prebuild_mx then
        -- Wall-shield: navigate to prebuild position (outside pill range) first
        nav_mx = goal.prebuild_mx
        nav_my = goal.prebuild_my
        nav_wx = U.m2w(nav_mx)
        nav_wy = U.m2w(nav_my)
      elseif goal.approach_mx and goal.substate == "approach" then
        -- Navigate to approach position (1.5 tiles behind standoff).
        -- Tile fields drive the A* path target; precise float fields
        -- (approach_fx/fy) are the actual world destination.
        nav_mx = goal.approach_mx
        nav_my = goal.approach_my
        if goal.approach_fx then
          nav_wx = math.floor(goal.approach_fx * 256 + 0.5)
          nav_wy = math.floor(goal.approach_fy * 256 + 0.5)
        else
          nav_wx = U.m2w(nav_mx)
          nav_wy = U.m2w(nav_my)
        end
      elseif goal.standoff_mx then
        nav_mx = goal.standoff_mx
        nav_my = goal.standoff_my
        if goal.standoff_fx then
          nav_wx = math.floor(goal.standoff_fx * 256 + 0.5)
          nav_wy = math.floor(goal.standoff_fy * 256 + 0.5)
        else
          nav_wx = U.m2w(nav_mx)
          nav_wy = U.m2w(nav_my)
        end
      else
        -- No standoff computed yet — navigate to explore frontier so the
        -- tank keeps moving usefully while attack planning catches up.
        local eb = state.explore_breakdown
        if eb and eb.mx then
          nav_mx = eb.mx
          nav_my = eb.my
          nav_wx = U.m2w(nav_mx)
          nav_wy = U.m2w(nav_my)
        end
      end
    -- (was: a second `elseif goal.kind == "attack_pill"` branch with
    -- a BPC_STANDOFF fallback — unreachable because the if branch
    -- above already matches attack_pill. The legacy ATTACK_PILL_STANDOFF
    -- fallback at line 1630-1640 covers the no-standoff_mx case.)
    end

    -- Precise final-approach: once A* has gotten us within APPROACH_PRECISE_DIST
    -- of the exact float approach point (nav_wx/wy = approach_fx/fy), switch to
    -- the exact-center creep so we actually reach the in-position threshold
    -- (16 wu) instead of parking at the loose 64 wu default and stalling. A* only
    -- has to get us close; this nav owns the last bit (game-unit precision).
    if goal.kind == "attack_pill" and goal.substate == "approach" and nav_wx then
      local _dappr = U.wdist(info.tankx, info.tanky, nav_wx, nav_wy)
      if _dappr <= (C.APPROACH_PRECISE_DIST or 384) then
        needs_exact_center = true
        center_tol = C.APPROACH_PRECISE_TOL or 16
      end
    end

    -- Follow the next-step waypoint, with path lookahead to reduce wiggle
    local _t_pre_path = BRAIN_PROFILE and clock_us() or 0
    local nx, ny
    -- rescue_lgm chases the LIVE (moving) LGM tile, so dest changes nearly
    -- every tick — that defeats cpf_path_to's src/dest A* cache and forces a
    -- full A* search every tick (the bot's single biggest steering cost, and
    -- the worst-case route since the goal only exists because the LGM is
    -- stranded). The tank-rooted KIND_NORMAL Dijkstra slate already floods the
    -- whole map (LGM tile included) and is recomputed on an interval, so read
    -- the next step straight off it (a parent-pointer walk, ~free). Fall back
    -- to A* only when the slate hasn't reached that tile yet.
    if goal.kind == "rescue_lgm" then
      local rtmx, rtmy = info.tankx >> 8, info.tanky >> 8
      local dnx, dny = cpf.dijkstra_next_step(cpf.KIND_NORMAL, rtmx, rtmy,
                         nav_mx, nav_my, state._nav_avoid_tiles, C.NAV_AVOID_PENALTY)
      if dnx and not (dnx == rtmx and dny == rtmy) then
        nx, ny = dnx, dny
        -- Mirror the bookkeeping cpf_path_to does so path_lookahead and the
        -- steering overlays stay coherent (chain in the same flat format).
        local pf = state.pf
        pf.status  = "done"
        pf.src_mx,  pf.src_my  = rtmx, rtmy
        pf.dest_mx, pf.dest_my = nav_mx, nav_my
        pf.next_mx, pf.next_my = nx, ny
        pf.age = 0
        pf.path_chain = cpf.dijkstra_trace_path(cpf.KIND_NORMAL, nav_mx, nav_my)
      end
    end
    if not nx then
      nx, ny = cpf_path_to(state, info, nav_mx, nav_my)
    end
    local _t_post_path = BRAIN_PROFILE and clock_us() or 0
    if BRAIN_PROFILE_LOG and (_t_post_path - _t_pre_path > 3000 or _t_after_stuck - _t_steer_start > 3000) then
      opt.append("optimize.log", string.format(
        "  [steer-detail] tick=%d goal=%s stuck_r=%.2fms path_to=%.2fms dest=(%d,%d)",
        state.tick or 0, goal.kind or "?",
        (_t_after_stuck - _t_steer_start) / 1000,
        (_t_post_path - _t_pre_path) / 1000,
        nav_mx or -1, nav_my or -1))
    end

    if nx then
      -- Skip ahead on the path when the straight line is clear
      local _t_la0 = BRAIN_PROFILE and clock_us() or 0
      local lx, ly = path_lookahead(state, info, nx, ny)
      if BRAIN_PROFILE then
        _path_lookahead_us = _path_lookahead_us + (clock_us() - _t_la0)
      end
      state._steer_lx = lx
      state._steer_ly = ly
      local step_wx, step_wy = U.m2w(lx), U.m2w(ly)

      move_dir      = U.aim_at(info.tankx, info.tanky, step_wx, step_wy)
      target_dist   = U.wdist(info.tankx, info.tanky, step_wx, step_wy)
    else
      -- On the destination tile but not centered: steer to tile center
      local center_dist = U.wdist(info.tankx, info.tanky, nav_wx, nav_wy)
      if center_dist > center_tol then
        move_dir    = U.aim_at(info.tankx, info.tanky, nav_wx, nav_wy)
        target_dist = center_dist
      end
    end
    if BRAIN_PROFILE then _t_path_us = _t_path_us + (clock_us() - _t_pre_path) end
    goal_dist = U.wdist(info.tankx, info.tanky, nav_wx, nav_wy)

    -- Steering debug overlays (always draw when we have nav data)
    if BRAIN_DEBUG_MODE then
      local pf = state.pf
      local twx, twy = info.tankx / 256.0, info.tanky / 256.0

      -- Raw A* next step (yellow square outline + coord label). The
      -- yellow rect itself stays on the same gate as the labels —
      -- consistent with "every drawn thing has a checkbox".
      if pf.next_mx and pf.next_mx >= 0 then
        viz.rect("steering_text",
                 pf.next_mx, pf.next_my, pf.next_mx + 1, pf.next_my + 1,
                 255, 255, 0, 200)
        local dmx = math.abs(pf.next_mx - tmx)
        local dmy = math.abs(pf.next_my - tmy)
        local cheb = math.max(dmx, dmy)
        local pf_status = pf.status or "?"
        -- Detailed info at top: coords + chebyshev + status.
        local method = cpf._last_method or "?"
        local m_label = (method == "dij") and "dij/NORMAL" or method
        viz.text("steering_text",
                 pf.next_mx + 0.5, pf.next_my - 0.3,
                 string.format("pf.next=(%d,%d) cheb=%d [%s] %s",
                               pf.next_mx, pf.next_my, cheb, pf_status, m_label),
                 "center", 255, 255, 120, 230, 0.5)
        -- Tiny "pf.next" tag at bottom, paired with the cliff-safety
        -- tag on the scan squares so the two yellows are distinguishable.
        viz.text("steering_text",
                 pf.next_mx + 0.5, pf.next_my + 0.95, "pf.next",
                 "center", 255, 255, 0, 220, 0.5)
      end

      -- Lookahead target (magenta circle) — where steering actually aims
      if state._steer_lx then
        viz.circle("nav_lookahead_marker", state._steer_lx + 0.5, state._steer_ly + 0.5, 0.4,
                       255, 0, 255, 230)
      end

      -- Line from tank to lookahead target (magenta)
      if move_dir and state._steer_lx then
        viz.line("nav_lookahead_marker", twx, twy, state._steer_lx + 0.5, state._steer_ly + 0.5,
                     255, 0, 255, 160)
      end

      -- Navigation destination (white circle, or thick purple if plowing)
      if plow_through then
        -- Thick purple ring: plow mode on — no braking at destination
        viz.circle("pf_destination", nav_mx + 0.5, nav_my + 0.5, 0.55, 180, 0, 220, 230)
        viz.circle("pf_destination", nav_mx + 0.5, nav_my + 0.5, 0.45, 180, 0, 220, 230)
        viz.circle("pf_destination", nav_mx + 0.5, nav_my + 0.5, 0.35, 180, 0, 220, 230)
      else
        viz.circle("pf_destination", nav_mx + 0.5, nav_my + 0.5, 0.3, 255, 255, 255, 180)
      end

      -- Next-goal line: deep purple line from the current nav destination to
      -- the next goal tile, so it's visible when the lookahead override will
      -- kick in and swing steering early.
      if state.next_goal and state.next_goal.wx and state.next_goal.wy then
        local ngx = state.next_goal.wx / 256.0
        local ngy = state.next_goal.wy / 256.0
        viz.line("pf_destination", nav_mx + 0.5, nav_my + 0.5, ngx, ngy, 100, 0, 140, 220)
        viz.circle("pf_destination", ngx, ngy, 0.25, 100, 0, 140, 220)
      end

      -- Wall-shoot precondition viz: if the next A* step is a wall tile,
      -- label it with OK/X markers so we can see which preconditions fail.
      -- Fires here (in the nav overlay block) so it doesn't get skipped by
      -- later early returns.
      if pf.next_mx and pf.next_mx >= 0 then
        local ntt = U.ttype(pf.next_mx, pf.next_my)
        if ntt == C.T_BUILDING or ntt == C.T_HALFBUILD then
          local wdist_wall = U.wdist(info.tankx, info.tanky,
                                     U.m2w(pf.next_mx), U.m2w(pf.next_my))
          local under_fire_here = state.perc and state.perc.under_fire or false
          local allow_wall = not under_fire_here
                             or (goal.kind == "attack_pill" and goal.substate == "approach")
                             or goal.kind == "rescue_lgm"
                             or goal.kind == "refuel_at_base"
                             or goal.kind == "flee_to_base"
                             or goal.kind == "capture_base"
                             or goal.kind == "capture_pill"
          local c_shells = info.shells > C.SHELL_RESERVE
          local c_move   = move_dir ~= nil
          local c_allow  = allow_wall
          local c_dist   = wdist_wall < 768
          local function mark(ok) return ok and "OK" or "X" end
          local lbl = string.format(
            "wall@(%d,%d) %s  dist=%.1ft(%s<3)  shells=%d>%d(%s)  allow=%s  nav=%s",
            pf.next_mx, pf.next_my,
            ntt == C.T_BUILDING and "FULL" or "HALF",
            wdist_wall / 256.0, mark(c_dist),
            info.shells, C.SHELL_RESERVE, mark(c_shells),
            mark(c_allow),
            mark(c_move))
          local all_ok = c_shells and c_move and c_allow and c_dist
          local r, g = (all_ok and 100 or 255), (all_ok and 255 or 120)
          viz.text("wall_shoot_precond", pf.next_mx + 0.5, pf.next_my - 0.3,
            lbl, "center", r, g, 80, 255)
          if not all_ok then
            local reasons = {}
            if not c_dist   then reasons[#reasons+1] = string.format("too far (%.1ft >= 3t)", wdist_wall/256.0) end
            if not c_shells then reasons[#reasons+1] = string.format("low shells (%d <= %d)", info.shells, C.SHELL_RESERVE) end
            if not c_allow  then reasons[#reasons+1] = string.format("under_fire + goal=%s not in allowlist", goal.kind or "?") end
            if not c_move   then reasons[#reasons+1] = "no move_dir (on destination tile)" end
            viz.text("wall_shoot_precond", pf.next_mx + 0.5, pf.next_my + 1.1,
              "BLOCKED: " .. table.concat(reasons, "; "),
              "center", 255, 80, 80, 255)
          end
        end
      end
    end
  end

  -- Attack_pill in range with clear LOS: stop navigating and stand to fight.
  -- Must be computed before the early return below, since that return fires
  -- exactly when move_dir is nil (i.e. we've reached the standoff tile).
  --
  -- On a boat: only engage if the pill is right at the water's edge (water
  -- corridor check).  Otherwise the shell won't reach, the boat blocks us
  -- from shooting, and — critically — setting move_dir=nil here prevents
  -- the boat_exit logic below from firing, stranding the tank at the water's
  -- edge indefinitely until the pill knocks the boat off.
  -- Pill engage: only shoot when in engage substate, in range, with LOS
  local _t_l0 = BRAIN_PROFILE and clock_us() or 0
  local attack_in_range = false
  if goal.kind == "attack_pill" and goal.substate == "engage"
     and info.shells > C.SHELL_RESERVE then
    local wdist_pill = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    local wall_hp = PF.wall_hp_between(tmx, tmy, goal.mx, goal.my)
    local los_ok = wall_hp == 0
    sdbg("ENGAGE check: dist=%.0f range=%.0f los=%s shells=%d",
         wdist_pill, C.ATTACK_PILL_RANGE * 256, tostring(los_ok), info.shells)

    if wdist_pill <= C.ATTACK_PILL_RANGE * 256 and los_ok then
      local can_engage = not info.inboat
                         or water_corridor_to(tmx, tmy, goal.mx, goal.my)
      if can_engage then
        attack_in_range = true
      end
    end
  end
  if BRAIN_PROFILE then _t_los_us = clock_us() - _t_l0 end

  local ws_holding = false  -- wall-shield removed
  if ws_holding then
    if info.speed > 0 then
      return KEY_SLOWER, 0
    end
    return 0, 0
  end

  -- Wall-shield retreat: move directly AWAY from the pill to escape range
  -- as fast as possible, then return to prebuild distance.
  if goal.kind == "attack_pill" and goal.wall_shield
     and goal.substate == "ws_retreat" then
    local pmx, pmy = goal.mx, goal.my
    local pwx, pwy = U.m2w(pmx), U.m2w(pmy)
    -- Direction directly away from the pill
    local dx = info.tankx - pwx
    local dy = info.tanky - pwy
    local dist = math.sqrt(dx * dx + dy * dy)
    if dist > 1 then
      local ux, uy = dx / dist, dy / dist
      -- Target: prebuild distance from pill (outside range) straight back
      local omx = U.mclamp(math.floor(pmx + ux * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5))
      local omy = U.mclamp(math.floor(pmy + uy * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5))
      local owx, owy = U.m2w(omx), U.m2w(omy)
      local offset_dir = U.aim_at(info.tankx, info.tanky, owx, owy)
      local offset_dist = U.wdist(info.tankx, info.tanky, owx, owy)

      if offset_dist > 128 then
        local corr = U.adiff(info.direction, offset_dir)
        -- Retreat: high max speed, low min speed — escape ASAP
        local k, t = nav_turn_speed(corr, info.speed, 64, 2)
        keys = keys | k
        taps = taps | t
      else
        if info.speed > 0 then keys = keys | KEY_SLOWER end
      end

      log.reason("steer", {
        mode = "ws_retreat",
        offset_mx = omx, offset_my = omy,
        offset_dist = offset_dist,
      })
      return keys, taps
    end
  end

  -- Anchor: end of nav-dispatch (the goal-kind elseif chain that picks
  -- a target tile + pf path), start of nav-apply (the wall-clear /
  -- turn-rate / threat-cap / final keys+taps decision block below).
  local _t_nav_apply_start = BRAIN_PROFILE and clock_us() or 0

  -- No goal or idle: brake to a stop.
  -- When attack_in_range, skip the navigation block and fall through to
  -- the engage aim/shoot block below.
  -- Attack base: when we've arrived at our shooting spot (move_dir nil) and
  -- we're within shell range of the base, don't return early — fall through to
  -- the attack_base shooting block. Covers BOTH a point-blank rush (adjacent)
  -- and a crossfire-safe standoff engage point picked out at range.
  local attack_base_engaging = (goal.kind == "attack_base" and move_dir == nil
    and U.wdist(info.tankx, info.tanky, goal.wx, goal.wy) <= (C.ATTACK_PILL_RANGE or 9.5) * 256)
  if move_dir == nil and not attack_in_range and not attack_base_engaging then
    if info.speed > 0 then
      keys = keys | KEY_SLOWER
    end
    if BRAIN_PROFILE then
      local _total_us = _t_nav_apply_start - _t_nav_dispatch_start
      local _setup_us = _total_us - _t_path_us - _t_los_us
      if _setup_us < 0 then _setup_us = 0 end
      local _path_misc_us = _t_path_us
                           - _path_search_us - _path_trace_us - _path_lookahead_us
      if _path_misc_us < 0 then _path_misc_us = 0 end
      -- 4-space indent = level-3 (subsub) attached to the next 2-space
      -- sub line (nav-dispatch/path). Order matters: subsubs are
      -- emitted BEFORE their parent so the parser can attach them.
      opt(string.format("    search (%s) done %.2f ms",
                        _path_method == "dij" and "dijkstra" or "A*",
                        _path_search_us / 1000))
      opt(string.format("    trace done %.2f ms",     _path_trace_us     / 1000))
      opt(string.format("    lookahead done %.2f ms", _path_lookahead_us / 1000))
      opt(string.format("    misc done %.2f ms",      _path_misc_us      / 1000))
      opt(string.format("  steer/nav-dispatch/path done %.2f ms",  _t_path_us / 1000))
      opt(string.format("  steer/nav-dispatch/los done %.2f ms",   _t_los_us / 1000))
      opt(string.format("  steer/nav-dispatch/setup done %.2f ms", _setup_us / 1000))
      opt(string.format("  steer/nav-apply done %.2f ms",
                        (clock_us() - _t_nav_apply_start) / 1000))
    end
    return keys, taps
  end

  if move_dir ~= nil and not attack_in_range then

    -- Wall-clear: if the A* next waypoint is a wall tile, stop and shoot it
    -- down before proceeding.  This is the primary wall-clearing mechanism;
    -- the opportunistic drive-by shooting further below is a bonus for walls
    -- we happen to be aimed at while moving.
    -- Skip when under fire — stopping to demolish a wall while being shot is
    -- too dangerous; fall through to normal navigation instead.
    -- In a boat: allowed for the immediate next A* tile — shells from a boat
    -- reach the first land square, which is exactly the water-edge wall we
    -- need to clear to disembark.
    local wall_clearing = false
    state.wall_clearing = false  -- reset each tick; stuck detection reads this
    local allow_wall_clear = not under_fire
                            or (goal.kind == "attack_pill" and goal.substate == "approach")
                            or goal.kind == "rescue_lgm"
                            or goal.kind == "refuel_at_base"
                            or goal.kind == "flee_to_base"
                            or goal.kind == "capture_base"
                            or goal.kind == "capture_pill"

    if allow_wall_clear and info.shells > C.SHELL_RESERVE then
      local pf = state.pf
      if pf.next_mx >= 0 then
        local next_tt = U.ttype(pf.next_mx, pf.next_my)

        -- Hostile base on the path: shoot it down before crossing.
        -- Only fire if the base is actually still hostile — if it flipped
        -- neutral/friendly since the last overlay update, just drive through.
        if next_tt == C.T_REFBASE then
          local nb = W.base_at(world, pf.next_mx, pf.next_my)
          if nb and nb.owner == "hostile" and (nb.health or 0) > 0 then
            local base_wx = U.m2w(pf.next_mx)
            local base_wy = U.m2w(pf.next_my)
            local base_dist = U.wdist(info.tankx, info.tanky, base_wx, base_wy)
            if base_dist < 768 then
              wall_clearing = true
              state.wall_clearing = true
              local aim_dir = U.aim_at(info.tankx, info.tanky, base_wx, base_wy)
              local corr    = U.adiff(info.direction, aim_dir)
              if     corr >  10 then keys = keys | KEY_TURNRIGHT
              elseif corr < -10 then keys = keys | KEY_TURNLEFT
              elseif corr >   2 then taps = taps | KEY_TURNRIGHT
              elseif corr <  -2 then taps = taps | KEY_TURNLEFT
              end
              if info.speed > 0 then keys = keys | KEY_SLOWER end
              if math.abs(corr) < 8 then taps = taps | KEY_SHOOT end
              if state.tick % 10 == 0 then
                log.reason("steer", {
                  mode = "base_clear",
                  base_mx = pf.next_mx, base_my = pf.next_my,
                  base_dist = base_dist, aim_corr = corr,
                  base_health = nb.health,
                  firing = math.abs(corr) < 8,
                })
              end
            end
          end
        end

        if not wall_clearing and (next_tt == C.T_BUILDING or next_tt == C.T_HALFBUILD) then
          local wall_wx = U.m2w(pf.next_mx)
          local wall_wy = U.m2w(pf.next_my)
          local wall_dist = U.wdist(info.tankx, info.tanky, wall_wx, wall_wy)
          -- Only enter wall-clear when we're within 3 tiles (close enough
          -- that we should be dealing with it, not still far away navigating)
          if wall_dist < 768 then
            wall_clearing = true
            state.wall_clearing = true
            local aim_dir = U.aim_at(info.tankx, info.tanky, wall_wx, wall_wy)
            local corr    = U.adiff(info.direction, aim_dir)

            -- Turn to face the wall
            if     corr >  10 then keys = keys | KEY_TURNRIGHT
            elseif corr < -10 then keys = keys | KEY_TURNLEFT
            elseif corr >   2 then taps = taps | KEY_TURNRIGHT
            elseif corr <  -2 then taps = taps | KEY_TURNLEFT
            end

            -- Brake to a stop so we hold position while firing
            if info.speed > 0 then
              keys = keys | KEY_SLOWER
            end

            -- Fire when roughly aimed
            if math.abs(corr) < 8 then
              taps = taps | KEY_SHOOT
            end

            if state.tick % 10 == 0 then
              log.reason("steer", {
                mode = "wall_clear",
                wall_mx = pf.next_mx, wall_my = pf.next_my,
                wall_dist = wall_dist, aim_corr = corr,
                firing = math.abs(corr) < 8,
                wall_type = next_tt == C.T_BUILDING and "full" or "half",
              })
            end
          end
        end
      end
    end

    if wall_clearing then
      -- Wall-clear has set keys/taps; skip normal navigation but still run
      -- the logging at the end of this block.
      -- Fall through to the steer log + return below.

    else -- normal navigation

    -- Turn toward move_dir
    local correction = U.adiff(info.direction, move_dir)

    if     correction >  10 then keys = keys | KEY_TURNRIGHT
    elseif correction < -10 then keys = keys | KEY_TURNLEFT
    elseif correction >   2 then taps = taps | KEY_TURNRIGHT
    elseif correction <  -2 then taps = taps | KEY_TURNLEFT
    end

    local eff_dist = goal_dist
    -- Attack pill: start braking much earlier to avoid overshooting standoff
    local brake_mult = (goal.kind == "attack_pill" or goal.kind == "pill_place") and 24 or 12
    local brake_dist = math.max(128, info.speed * brake_mult)
    local facing_away = math.abs(correction) > 64

    -- Orbit detector: at high speed the tank's turning radius can be
    -- wider than its remaining distance to the goal. The tank ends
    -- up circling — distance occasionally drops as it sweeps closest,
    -- climbs as it sweeps farthest, but never converges. Detect via
    -- "should have arrived by now" rather than per-tick progress: if
    -- we've been close-and-off-bearing-and-fast for a long time, we're
    -- orbiting regardless of moment-to-moment distance changes.
    --
    -- Brake hard when triggered to shrink the turn radius; release
    -- as soon as we're well-aligned (correction < 8 brads) so a
    -- successful turn-in immediately restores speed.
    local orbit_brake = false
    do
      local ORBIT_NEAR_WU      = 1024  -- 4 tiles — only matters when close
      local ORBIT_CORR_BRAD    = 16    -- ~22.5° off-bearing
      local ORBIT_DETECT_TICKS = 60    -- ~1.2 s in the danger zone
      local ORBIT_RELEASE_BRAD = 8     -- ~11° — release brake when aligned
      local in_zone = eff_dist < ORBIT_NEAR_WU
                  and math.abs(correction) > ORBIT_CORR_BRAD
                  and info.speed > 16
      if in_zone then
        goal._orbit_stuck = (goal._orbit_stuck or 0) + 1
        if goal._orbit_stuck >= ORBIT_DETECT_TICKS then
          orbit_brake = true
        end
      elseif math.abs(correction) <= ORBIT_RELEASE_BRAD then
        goal._orbit_stuck = 0
      else
        -- Out of zone but still misaligned: decay rather than freeze,
        -- so a long break (e.g. driving away from the goal) lets the
        -- counter drift back down. Without this, _orbit_stuck stays
        -- pinned and the next time we re-enter the zone we'd brake
        -- immediately even after a clean detour.
        local s = goal._orbit_stuck or 0
        if s > 0 then goal._orbit_stuck = s - 1 end
      end
      goal._orbit_last_dist = eff_dist
    end

    -- Goal lookahead: if we have a next_goal, steer toward it instead of
    -- braking at the current destination.  Override move_dir and eff_dist
    -- so the tank drives through the capture point at speed.
    --
    -- Suppress for substates that REQUIRE landing precisely on the
    -- destination (attack_pill approach/in_range_position/aim/build_walls,
    -- pill_place dispatch, etc). For those, the lookahead would re-aim
    -- past the standoff just as the tank gets within brake_dist, the
    -- tank overshoots, comes back, gets re-redirected — a wide orbit
    -- that never converges.
    local PRECISE_SUBSTATES = {
      gather_trees       = true,
      approach           = true,
      in_range_position  = true,
      in_range_aim_pre   = true,
      in_range_aim       = true,
      in_range_aim_finetune = true,
      shoot_pill         = true,
      build_walls        = true,
      aim                = true,
      ws_prebuild        = true,
      ws_prewait         = true,
      ws_advance         = true,
      ws_engage          = true,
      ws_retreat         = true,
      dispatch           = true,
      navigate           = true,
      wait_place         = true,
    }
    local sub = goal.substate
    local precise_landing = sub and PRECISE_SUBSTATES[sub]
    local lookahead_active = false
    if state.next_goal and eff_dist < brake_dist and not info.inboat
       and not precise_landing then
      local ng = state.next_goal
      move_dir   = U.aim_at(info.tankx, info.tanky, ng.wx, ng.wy)
      eff_dist   = U.wdist(info.tankx, info.tanky, ng.wx, ng.wy)
      brake_dist = math.max(128, info.speed * 12)
      correction = U.adiff(info.direction, move_dir)
      facing_away = math.abs(correction) > 64
      lookahead_active = true
    end

    -- Emergency stop: deep sea ahead while on land.
    -- Look-ahead distance scales with current speed so high-speed plow tanks
    -- get enough runway to brake. At speed 0 look 1 tile; at speed 128 look
    -- ~5 tiles. Scan multiple points along the facing line so we don't miss
    -- a cliff edge between samples.
    local cliff = false
    local cliff_hit_mx, cliff_hit_my, cliff_hit_steps  -- for viz/debug
    if not info.inboat then
      local look_wu = math.max(256, info.speed * 10)   -- ~10 ticks of motion
      local steps   = math.min(6, math.max(1, math.ceil(look_wu / 256)))
      local sdir    = U.bsin(info.direction)
      local cdir    = U.bcos(info.direction)
      for i = 1, steps do
        local amx = (info.tankx + sdir * 2 * i) >> 8
        local amy = (info.tanky - cdir * 2 * i) >> 8
        if U.ttype(amx, amy) == C.T_DEEPSEA then
          cliff = true
          cliff_hit_mx, cliff_hit_my, cliff_hit_steps = amx, amy, i
          break
        end
      end
    end
    if cliff and BRAIN_DEBUG_MODE then
      -- Orange outline on the deep-sea tile that triggered the stop
      viz.rect("cliff_safety", cliff_hit_mx, cliff_hit_my, cliff_hit_mx + 1, cliff_hit_my + 1,
                   255, 140, 0, 220)
    end

    -- Boat-to-land transition: must maintain high speed to disembark.
    -- Only applies when the tank is actually riding the boat ON water,
    -- not when merely carrying a boat on land.
    local boat_exit = false
    if info.inboat and move_dir ~= nil then
      local cur_tt = U.ttype(tmx, tmy)
      local on_water = WATER_TT[cur_tt]
      if on_water then
        local next_mx = (info.tankx + U.bsin(move_dir) * 2) >> 8
        local next_my = (info.tanky - U.bcos(move_dir) * 2) >> 8
        if U.in_map(next_mx, next_my) then
          local next_tt = U.ttype(next_mx, next_my)
          if next_tt ~= C.T_RIVER and next_tt ~= C.T_DEEPSEA then
            boat_exit = true
          end
        end
      end
    end

    -- Turn-sharpness speed limit — proportional ramp.
    -- Plow mode: cap applies but is EASED OUT by distance to the effective
    -- target. Close target -> full cap (slow to turn tight). Far target ->
    -- no cap (wide arc is fine, carry momentum). Linear blend 4-10 tiles.
    local abs_corr = math.abs(correction)
    local turn_max_speed = 256

    -- Attack-pill approach brake-zone gate.  Pre-computed here so the
    -- throttle chain below can decide whether to take the specialised
    -- attack_pill branch (inside the brake zone) or fall through to
    -- the generic cruise / facing_away handlers (outside it).  See the
    -- branch body around the matching `_approach_brake_active` check.
    local _approach_brake_active = false
    local _approach_sdist_wu     = 0
    if goal.kind == "attack_pill" and goal.substate == "approach" then
      local _smx = goal.standoff_mx or goal.mx
      local _smy = goal.standoff_my or goal.my
      _approach_sdist_wu = U.wdist(info.tankx, info.tanky,
                                   U.m2w(_smx), U.m2w(_smy))
      -- FAST_APPROACH: the predict_stop fast-path must own the throttle for the
      -- ENTIRE approach, not only inside the standoff brake zone. This gate is
      -- keyed to the STANDOFF distance, but the generic `approach_brake` branch
      -- below is keyed to the nearer APPROACH-POINT distance (eff_dist < sdist
      -- always, since the approach point sits between tank and standoff). So in
      -- the speed band where eff_dist < speed*24 but sdist isn't yet, the gate
      -- stays off, the generic proportional brake fires, slows the tank, and
      -- keeps the gate off — a crawl all the way in. Forcing it on under the
      -- flag makes the (earlier) predictor branch win; stage-2 creep still takes
      -- over once speed<=4 or within 1/2 tile (it returns before reaching here).
      _approach_brake_active = C.FAST_APPROACH
                               or _approach_sdist_wu < math.max(256, info.speed * 24)
    end
    -- Throttle-branch diagnostic.  Set by each branch below so the
    -- hud_throttle overlay can show which decision tier fired this
    -- tick + the key inputs that drove it.
    local _throttle_branch = "(none)"
    -- Hoisted so the viz reads the same locals the logic uses.
    local turn_base_cap = 0
    local turn_factor   = 1.0
    local turn_capped   = 256    -- after ramp, before distance ease
    local ramp_start    = plow_through and 20 or 10
    -- Hard distance gate: while target is >= 4 tiles out we keep the
    -- turn-cap OFF entirely (turn_max_speed stays 256 = full speed) for
    -- every goal type, plow or not. Inside 4 tiles the heading-error
    -- ramp re-enables so the tank can still slow into a precise
    -- landing. Used to be plow-only — non-plow goals would crawl the
    -- whole way home at base_cap=48 every time the A* heading wobbled
    -- past 14°, which made attack_pill approach feel sluggish across
    -- the map.
    local dist_t        = eff_dist / 256.0
    local plow_ease     = (dist_t >= 4) and 1.0 or 0.0  -- legacy var name kept for viz
    if abs_corr > ramp_start and dist_t < 4 then
      if plow_through then
        turn_base_cap = (under_fire or race_mode) and 128 or 96
      else
        turn_base_cap = (under_fire or race_mode) and  64 or 48
      end
      turn_factor    = 1.0 - math.min((abs_corr - ramp_start) / 70.0, 1.0)
      turn_capped    = math.max(6, math.floor(turn_factor * turn_base_cap))
      turn_max_speed = turn_capped
    end

    -- Plow-mode debug viz: two live lines below the tank (state + decision).
    -- The legend explaining every term lives in the comment below. Ask me
    -- for the "turning formula legend" and I'll read it back.
    --
    -- ─────────────── turning-formula legend ───────────────────────────────
    --   target_dist   : straight-line (in tiles) to where steering is aiming;
    --                   `next_goal` when the lookahead override is active,
    --                   else the current nav destination.
    --   heading_err   : signed angle between tank facing and target. Shown
    --                   in degrees and bradians (|N| brad). 1 brad = 1/256
    --                   of a full turn = ~1.4°.
    --   ramp_start    : |err| below this => NO turn-cap at all. plow=20 brad
    --                   (~28°), non-plow=10 brad (~14°).
    --   base_cap      : top turn speed when |err| is sharp. plow: 96 normal
    --                   or 128 under_fire/race. non-plow: 48 / 64.
    --   ramp_factor   : linear ramp 1.0 at ramp_start down to 0.0 at
    --                   ramp_start+70 brad (~110°). Applied as base_cap×ramp.
    --   ease          : plow-only distance easing. 0 when target_dist<=4t
    --                   (full cap applies); 1 when target_dist>=10t (cap
    --                   lifts to 256 = no slowdown); linear blend between.
    --   turn_max_speed: final cap used for speed control while turning. In
    --                   plow with a far target this goes back to 256.
    -- ───────────────────────────────────────────────────────────────────────
    if BRAIN_DEBUG_MODE and plow_through then
      local twx, twy = info.tankx / 256.0, info.tanky / 256.0
      local deg = correction * (360.0 / 256.0)
      local target_kind = lookahead_active and "next_goal" or "current nav dest"

      local state_line = string.format(
        "PLOW  target_dist=%.1f tiles (to %s)  heading_err=%+d° (|%d| brad)",
        dist_t, target_kind, math.floor(deg + 0.5), abs_corr)

      local decision_line
      if abs_corr <= ramp_start then
        decision_line = string.format(
          "|err|=%d <= ramp_start=%d brad  ->  no cap  (turn_max_speed=%d)",
          abs_corr, ramp_start, turn_max_speed)
      elseif dist_t >= 4 then
        decision_line = string.format(
          "dist=%.1ft >= 4t -> cap OFF  (turn_max_speed=%d)",
          dist_t, turn_max_speed)
      else
        decision_line = string.format(
          "dist=%.1ft < 4t -> base_cap=%d x ramp_factor=%.2f = %d  (turn_max_speed=%d)",
          dist_t, turn_base_cap, turn_factor, turn_capped, turn_max_speed)
      end

      viz.text("steering_text", twx, twy + 1.3, state_line,    "center", 180, 220, 255, 255)
      viz.text("steering_text", twx, twy + 1.8, decision_line, "center", 180, 220, 255, 255)
    end

    -- Pace the LGM: if the builder is out on a *build* mission, limit tank
    -- speed so we don't outrun him (important when building bridges).
    -- Farming is excluded: the LGM walks ahead to chop a tree and returns;
    -- the tank has no reason to slow down for that.
    local lgm_out = info.man_status == C.LGM_MOVING
    local lgm_is_farming = state.builder.last_action == BUILDMODE_FARM
    local lgm_speed_cap = nil
    if lgm_out and not lgm_is_farming and goal.kind ~= "escape_water"
       and goal.kind ~= "rescue_lgm" then
      -- Estimate LGM speed: use the terrain at the LGM's position
      local man_mx = info.man_x >> 8
      local man_my = info.man_y >> 8
      local man_tt = U.ttype(man_mx, man_my)
      local man_spd = C.MAN_SPEED[man_tt] or 0
      -- If LGM is on his blessed build square, he moves at full speed
      if man_spd == 0 then man_spd = C.MAN_SPEED_BLESSED end
      lgm_speed_cap = man_spd
    end

    -- Smart LGM pickup: if LGM is nearby and arriving soon, adjust behavior.
    -- In a boat: stop completely so LGM can reach us before we sail away.
    -- Very close: don't pace at all, LGM will catch up naturally.
    local lgm_nearby = state.builder and state.builder.lgm_nearby
    if lgm_nearby and info.inboat then
      lgm_speed_cap = 0  -- full stop in boat
    elseif lgm_nearby and state.builder.lgm_arrival_ticks
           and state.builder.lgm_arrival_ticks < C.LGM_NEARBY_NOPACE_TICKS then
      lgm_speed_cap = nil  -- arriving imminently, don't slow down
    end

    -- Slightly slower than the LGM so he can catch up
    local tank_pace = lgm_speed_cap and math.max(1, math.floor(lgm_speed_cap * 0.7))

    if boat_exit and abs_corr < 24 then
      _throttle_branch = "boat_exit_aligned"
      keys = (keys & ~KEY_SLOWER) | KEY_FASTER
    elseif boat_exit then
      _throttle_branch = "boat_exit_turning"
      if info.speed > turn_max_speed + 4 then
        keys = keys | KEY_SLOWER
      elseif info.speed < 8 then
        keys = keys | KEY_FASTER
      end
    elseif tank_pace and info.speed > tank_pace then
      _throttle_branch = "lgm_pace_brake"
      keys = keys | KEY_SLOWER
    elseif tank_pace and tank_pace > 0 and info.speed < tank_pace then
      _throttle_branch = "lgm_pace_accel"
      keys = keys | KEY_FASTER
    elseif lgm_speed_cap and lgm_speed_cap == 0 then
      _throttle_branch = "lgm_halt"
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    elseif cliff and goal.kind ~= "escape_water" then
      _throttle_branch = "cliff_brake"
      keys = (keys & ~KEY_FASTER) | KEY_SLOWER
    elseif goal.kind == "escape_water" then
      _throttle_branch = "escape_water"
      keys = keys | KEY_FASTER
    elseif state._kill_lgm_halt then
      _throttle_branch = "kill_lgm_halt"
      -- kill_lgm in shooting range: full stop, only the turn keys
      -- above (set from move_dir aimed at the predicted LGM tile)
      -- fire so the tank pivots in place to align the crosshair.
      -- Init.lua's kill_lgm fire block handles the gunrange driver
      -- + fire trigger.
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    elseif _approach_brake_active then
      -- Aligned fast-path: when we're aimed straight at the approach point we
      -- can skip the slow proportional creep and drive at full speed, braking
      -- only once the engine-exact stop predictor (cpf.predict_stop, the model
      -- behind the charge_stop_pred viz) says braking-from-here would carry us
      -- to/past the approach point. predict_stop projects along the tank's
      -- facing, so it's only trustworthy when facing ≈ the target; when NOT
      -- aligned (still turning / circling toward the spot) we fall back to the
      -- gentle proportional brake so we don't overshoot while cornering.
      local afx = goal.approach_fx or (goal.approach_mx and (goal.approach_mx + 0.5))
      local afy = goal.approach_fy or (goal.approach_my and (goal.approach_my + 0.5))
      -- FAST_APPROACH forces the predict_stop fast-path for the whole brake zone
      -- (drops the abs_corr alignment gate / proportional crawl), so stage 1's
      -- slow sdist*0.03 creep never runs. Without the flag, only the lined-up
      -- final glide uses the predictor and cornering falls back to proportional.
      if afx and (C.FAST_APPROACH or abs_corr <= 12) then   -- lined up (or forced)
        _throttle_branch = "ap_linedup_fast"
        local awx = math.floor(afx * 256 + 0.5)
        local awy = math.floor(afy * 256 + 0.5)
        local adist = U.wdist(info.tankx, info.tanky, awx, awy)
        local tcap = (C.TERRAIN_SPEED and C.TERRAIN_SPEED[U.ttype(tmx, tmy)]) or 16
        local ang_f = info.tank_angle or info.direction
        local espeed = (info.speed or 0) / 4   -- info.speed is engine speed ×4
        local psx, psy = cpf.predict_stop(info.tankx, info.tanky, ang_f, espeed, tcap)
        local stop_dist = U.wdist(info.tankx, info.tanky, psx, psy)
        -- Brake the tick the predicted stop reaches the approach point, minus the
        -- landing tolerance so we settle within APPROACH_PRECISE_TOL rather than
        -- coasting a hair past it.
        local _brake_tol = C.APPROACH_PRECISE_TOL or 16
        if stop_dist >= adist - _brake_tol then keys = keys | KEY_SLOWER else keys = keys | KEY_FASTER end
        if BRAIN_DEBUG_MODE and viz.is_on("approach_stop_pred") then local _hit = (stop_dist >= adist - 16) and (stop_dist <= adist + 24); local _r, _g, _b = _hit and 60 or 255, _hit and 220 or 160, 60; viz.line("approach_stop_pred", info.tankx / 256, info.tanky / 256, psx / 256, psy / 256, _r, _g, _b, 160); viz.rect("approach_stop_pred", psx / 256 - 0.3, psy / 256 - 0.3, psx / 256 + 0.3, psy / 256 + 0.3, _r, _g, _b, 200, false); viz.rect("approach_stop_pred", awx / 256 - 0.15, awy / 256 - 0.15, awx / 256 + 0.15, awy / 256 + 0.15, 80, 160, 255, 220, true); viz.text("approach_stop_pred", psx / 256, psy / 256 - 0.5, string.format("stopd=%d adist=%d", stop_dist, adist), "center", _r, _g, _b, 230, 0.3) end
      else
        _throttle_branch = "ap_brake_zone"
        local sdist_wu = _approach_sdist_wu
        local desired = math.max(4, math.floor(sdist_wu * 0.03))
        if info.speed > desired + 4 then
          keys = keys | KEY_SLOWER
        elseif info.speed < desired and sdist_wu > 128 then
          keys = keys | KEY_FASTER
        end
      end
    elseif facing_away and C.FACING_AWAY_BRAKE_ENABLED then
      _throttle_branch = "facing_away"
      -- Viz: yellow ring around tank when facing-away brake is active, plus
      -- the correction angle (in degrees) under the rings so we can tell
      -- what triggered it — lookahead override, next_goal behind us, etc.
      if BRAIN_DEBUG_MODE then
        local twx, twy = info.tankx / 256.0, info.tanky / 256.0
        viz.circle("facing_away_brake", twx, twy, 0.7, 255, 230, 0, 220)
        viz.circle("facing_away_brake", twx, twy, 0.6, 255, 230, 0, 220)
        local deg = correction * (360.0 / 256.0)
        local tag = lookahead_active and "lookahead" or "nav"
        viz.text("facing_away_brake", twx, twy + 0.8,
          string.format("corr=%.0f° [%s]", deg, tag),
          "center", 255, 230, 0, 255)
      end
      -- Under fire or race_mode: tolerate higher speed even when facing away —
      -- momentum helps escape the threat zone or win the capture race faster
      -- than braking and re-accelerating.
      local facing_brake = (under_fire or race_mode) and 16 or 8
      if info.speed > facing_brake then
        keys = keys | KEY_SLOWER
      elseif info.speed == 0 then
        keys = keys | KEY_FASTER
      end
    elseif orbit_brake then
      _throttle_branch = "orbit_brake"
      if info.speed > 8 then keys = keys | KEY_SLOWER end
    elseif eff_dist < brake_dist and not plow_through then
      _throttle_branch = "approach_brake"
      -- Approach braking: slow proportionally to remaining distance.
      -- Plow-through goals (capture_base/capture_pill/nav_mode="plow")
      -- skip this — keep cruising through the destination.
      -- Attack pill: brake harder to avoid overshooting the standoff
      local brake_factor = (goal.kind == "attack_pill" or goal.kind == "pill_place")
                           and 0.04 or 0.08
      -- For exact-center goals (pill_place final tile, rescue_lgm) allow
      -- a much lower floor so the tank can creep onto the exact center
      -- without stalling at speed 6. Cap the absolute max at 4 too so
      -- the tank doesn't overshoot at close range.
      local floor_speed = needs_exact_center and 1 or 6
      local desired_speed = math.max(floor_speed, math.floor(eff_dist * brake_factor))
      if needs_exact_center and desired_speed > 4 then desired_speed = 4 end
      -- Also respect turn_max_speed
      if desired_speed > turn_max_speed then desired_speed = turn_max_speed end
      -- Tighter brake tolerance for centering — +1 instead of +4 so we
      -- don't coast past the target with leftover momentum.
      local brake_tol = needs_exact_center and 1 or 4
      if info.speed > desired_speed + brake_tol then
        keys = keys | KEY_SLOWER
      elseif info.speed < desired_speed and (eff_dist > 128 or needs_exact_center) then
        -- Normally only accelerate when far enough out (> 128 wu) so we
        -- don't overshoot. For exact-center goals always allow nudging
        -- forward as long as we're not yet within tolerance.
        keys = keys | KEY_FASTER
      end
    elseif plow_through then
      _throttle_branch = "plow_through"
      if info.speed < turn_max_speed then
        keys = keys | KEY_FASTER
      elseif info.speed > turn_max_speed + 4 then
        keys = keys | KEY_SLOWER
      end
    else
      _throttle_branch = "cruise"
      -- (intentionally fall-through — body sets keys below)
      if info.speed > turn_max_speed + 4 then
        keys = keys | KEY_SLOWER
      elseif info.speed < turn_max_speed then
        keys = keys | KEY_FASTER
      end
    end

    -- Fast-approach branch tracer: which throttle branch is actually driving
    -- the tank this tick during the approach substate. GREEN = predict_stop
    -- fast-path owns throttle; RED = another branch pre-empted it (the real
    -- reason a "stage 1" crawl reappears). Single line for the lua_strip rule.
    if BRAIN_DEBUG_MODE and viz.is_on("fast_approach") and goal.kind == "attack_pill" and goal.substate == "approach" then local _ff = (_throttle_branch == "ap_linedup_fast"); local _r, _g, _b = _ff and 60 or 255, _ff and 230 or 80, 60; viz.text("fast_approach", info.tankx / 256.0, info.tanky / 256.0 - 1.4, string.format("FAST_APPROACH=%s  br=%s  spd=%d  corr=%d  sdist=%d  apbrake=%s", tostring(C.FAST_APPROACH), _throttle_branch, info.speed, abs_corr, _approach_sdist_wu, tostring(_approach_brake_active)), "center", _r, _g, _b, 245, 0.35) end

    -- Throttle decision HUD: shows which `elseif` branch the throttle
    -- chain landed in this tick, plus the key inputs each branch
    -- considered, plus what keys ended up pressed.  Use the
    -- `hud_throttle` viz toggle to enable.  Great for diagnosing the
    -- "tank stuck at speed 0 despite being far from goal" class of bug.
    if BRAIN_DEBUG_MODE and viz.is_on("hud_throttle") and viz.hud_text then
      local _kparts = {}
      if (keys & KEY_FASTER)    ~= 0 then _kparts[#_kparts+1] = "FAST" end
      if (keys & KEY_SLOWER)    ~= 0 then _kparts[#_kparts+1] = "SLOW" end
      if (keys & KEY_TURNLEFT)  ~= 0 then _kparts[#_kparts+1] = "L"    end
      if (keys & KEY_TURNRIGHT) ~= 0 then _kparts[#_kparts+1] = "R"    end
      local _kstr = #_kparts > 0 and table.concat(_kparts, "+") or "(none)"
      viz.hud_text("hud_throttle", 10, 156,
        string.format("THROTTLE: %s  keys=[%s]", _throttle_branch, _kstr),
        "topleft", 120, 220, 255, 230)
      viz.hud_text("hud_throttle", 10, 168,
        string.format("  spd=%d  abs_corr=%d  eff_dist=%d  brake_dist=%d",
                      info.speed, abs_corr, eff_dist, brake_dist),
        "topleft", 180, 200, 220, 200)
      viz.hud_text("hud_throttle", 10, 180,
        string.format("  flags: boat_exit=%s  inboat=%s  cliff=%s  facing_away=%s  orbit=%s  ap_brake=%s  ap_sdist=%d",
                      tostring(boat_exit), tostring(info.inboat), tostring(cliff),
                      tostring(facing_away), tostring(orbit_brake),
                      tostring(_approach_brake_active), _approach_sdist_wu),
        "topleft", 180, 200, 220, 200)
    end

    -- Shoot walls on our planned path while driving by (opportunistic).
    -- Suppress on a boat: drive-by shots may destroy bridges we're sailing
    -- on or knock the tank into open water.  Water-edge walls are handled
    -- by the wall-clearing mode above which stops and aims deliberately.
    if not info.inboat and info.shells > C.SHELL_RESERVE and math.abs(correction) < 16 then
      local pf = state.pf
      if pf.next_mx >= 0 then
        local next_tt = U.ttype(pf.next_mx, pf.next_my)
        if next_tt == C.T_BUILDING or next_tt == C.T_HALFBUILD then
          taps = taps | KEY_SHOOT
        end
      end
    end

    -- Opportunistic shooting while navigating.
    -- Valid targets: enemy tanks, friendly pills (to "piss" them
    -- into firing at nearby enemies). Never shoot enemy/neutral pills — wastes
    -- ammo and angers them for no gain.  Enemy bases used to be in this
    -- list but were removed — passing-by base shots don't deal enough
    -- damage to be worth the shell + the threat of waking the base up
    -- mid-transit on an unrelated goal.
    if not info.inboat and info.shells > C.SHELL_RESERVE then
      local perc = state.perc or {}
      local shot_fired = false

      -- Enemy tanks
      if not shot_fired then
        for _, et in ipairs(perc.enemy_tanks or {}) do
          if et.dist <= 8 then
            local aim = U.aim_at(info.tankx, info.tanky, U.m2w(et.mx), U.m2w(et.my))
            if math.abs(U.adiff(info.direction, aim)) < 8
               and shot_path_clear(info, world, U.m2w(et.mx), U.m2w(et.my), et.mx, et.my) then
              taps = taps | KEY_SHOOT
              shot_fired = true
              if BRAIN_DEBUG_MODE then
                viz.line("tank_combat_viz", info.tankx / 256.0, info.tanky / 256.0,
                  et.mx + 0.5, et.my + 0.5, 255, 255, 0, 120)
              end
              break
            end
          end
        end
      end

      -- Friendly pills: "piss" them to fire faster, but only if an enemy
      -- tank is nearby (otherwise pointless)
      if not shot_fired and perc.nearest_hostile_tank
         and perc.nearest_hostile_tank.dist <= C.PILL_RANGE_MAP then
        for _, p in pairs(world.pills) do
          -- Stop pissing once the pill is down to 12 HP: each "piss" shot
          -- also DAMAGES the friendly pill, so firing past this would chip
          -- our own pill toward death. 12 leaves a safe buffer below full.
          if p.owner == "friendly" and p.health > 12 then
            local pd = U.mdist(tmx, tmy, p.mx, p.my)
            if pd <= 6 then
              local aim = U.aim_at(info.tankx, info.tanky, U.m2w(p.mx), U.m2w(p.my))
              if math.abs(U.adiff(info.direction, aim)) < 6 then
                taps = taps | KEY_SHOOT
                shot_fired = true
                break
              end
            end
          end
        end
      end
    end

    -- Shoot walls blocking our path when stuck (fallback).
    -- Allowed in a boat: shell reaches the water-edge wall blocking us.
    if info.tank_obstructed and math.abs(correction) < 10
       and state.stuck_for > 0 then
      local bx = (info.tankx + U.bsin(info.direction) * 1) >> 8
      local by = (info.tanky - U.bcos(info.direction) * 1) >> 8
      local bt = U.ttype(bx, by)
      if (bt == C.T_BUILDING or bt == C.T_HALFBUILD) and info.shells > 0 then
        taps = taps | KEY_SHOOT
      end
    end

    -- Log navigation steering reasoning (every 10 ticks to reduce volume)
    if state.tick % 10 == 0 then
      local steer_why = "navigate"
      if boat_exit then steer_why = "boat_exit_boost"
      elseif cliff then steer_why = "cliff_avoidance"
      elseif lgm_speed_cap then steer_why = "lgm_pacing"
      end
      if lookahead_active then steer_why = "lookahead" end
      log.reason("steer", {
        mode = steer_why,
        move_dir = move_dir,
        correction = correction,
        target_dist = target_dist,
        goal_dist = goal_dist,
        turn_max_spd = turn_max_speed,
        lgm_cap = lgm_speed_cap,
        lookahead = lookahead_active or nil,
        under_fire = under_fire or nil,
      })
    end

    end -- else (normal navigation vs wall_clearing)
  end

  -- Attack pill: aim and shoot when in range with clear LOS.
  -- The tank points at the pill and fires, but also creeps toward the
  -- standoff position to compensate for pill knockback.  This keeps the
  -- tank at optimal range rather than being slowly pushed out of position.
  local boat_can_hit = info.inboat
                       and water_corridor_to(tmx, tmy, goal.mx, goal.my)
  if attack_in_range and (not info.inboat or boat_can_hit) then
    -- Aim at near-edge of pill (computed in init.lua) for max standoff
    local aim_wx = goal.aim_mx and math.floor(goal.aim_mx * 256) or goal.wx
    local aim_wy = goal.aim_my and math.floor(goal.aim_my * 256) or goal.wy
    local aim_dir = U.aim_at(info.tankx, info.tanky, aim_wx, aim_wy)
    local corr    = U.adiff(info.direction, aim_dir)

    -- Gunsight at max range: shells travel further, hitting the pill from
    -- the greatest possible distance.
    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end

    -- Override navigation turn keys: point at the pill
    keys = keys & ~(KEY_TURNLEFT | KEY_TURNRIGHT | KEY_FASTER | KEY_SLOWER)
    taps = taps & ~(KEY_TURNLEFT | KEY_TURNRIGHT)
    if     corr >  10 then keys = keys | KEY_TURNRIGHT
    elseif corr < -10 then keys = keys | KEY_TURNLEFT
    elseif corr >   2 then taps = taps | KEY_TURNRIGHT
    elseif corr <  -2 then taps = taps | KEY_TURNLEFT
    end
    local still_correcting = (taps & (KEY_TURNLEFT | KEY_TURNRIGHT)) ~= 0
    if math.abs(corr) < 3 and not still_correcting then
      keys = keys | KEY_SHOOT
    end

    -- Movement during engage: stay at max range where pill shots are hardest
    -- to land.  Knockback naturally pushes the tank outward — let it.
    -- Only nudge forward if knocked completely out of range.
    local wdist_pill = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)

    local standoff_wu = C.ATTACK_PILL_STANDOFF * 256
    local too_close_wu = (C.ATTACK_PILL_STANDOFF - 2) * 256  -- 2 tiles inside standoff

    if wdist_pill > C.ATTACK_PILL_RANGE * 256 then
      -- Knocked out of range: gently push back in
      if info.speed < 4 and math.abs(corr) < 16 then
        keys = keys | KEY_FASTER
      end
    elseif wdist_pill < too_close_wu then
      -- Way too close: reverse away from pill
      keys = keys & ~KEY_FASTER
      keys = keys | KEY_SLOWER
      sdbg("engage: TOO CLOSE dist=%.0f standoff=%d, reversing", wdist_pill, standoff_wu)
    else
      -- At standoff: stop and shoot
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end

    log.reason("steer", {
      mode = "attack_pill", aim_corr = corr,
      firing = math.abs(corr) < 3 and not still_correcting,
      boat_can_hit = boat_can_hit,
      pill_dist = wdist_pill,
    })

    -- (overlays drawn by init.lua — no duplicates here)
  -- Attack base: the nav block above drives us to our shooting spot — a
  -- crossfire-safe standoff engage point if one exists on the approach path,
  -- otherwise right up beside the base (rush). Whenever a shot from where we are
  -- would actually HIT the base (clear LOS, <= MAX_WALLS walls, no pillbox/other
  -- base/ally tank in the lane) and we're pointed at it, fire — including
  -- opportunistically while still closing in. Once stopped at the engage point we
  -- hold and keep shelling. The goal ends on its own the moment the base flips to
  -- dead/capturable (eval_attack_base stops matching → capture_base takes over).
  elseif goal.kind == "attack_base" and info.shells > C.SHELL_RESERVE then
    local wdist_base = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    local shot_ok = shot_path_clear(info, world, goal.wx, goal.wy, goal.mx, goal.my,
                                    C.ATTACK_BASE_MAX_WALLS or 1)
    if wdist_base <= C.ATTACK_PILL_RANGE * 256 and shot_ok and not info.inboat then
      local aim_dir = U.aim_at(info.tankx, info.tanky, goal.wx, goal.wy)
      local corr    = U.adiff(info.direction, aim_dir)

      if info.gunrange < C.GUNSIGHT_MAX then
        keys = keys | KEY_MORERANGE
      end

      -- Stalled-approach promotion: the rush toward the engage point can be
      -- rebuffed indefinitely by pill knockback — crawling at a few wu/tick,
      -- never arriving, and with a steady heading offset the opportunistic
      -- fire below never triggers either (observed: neutral pill held a bot
      -- at ~4 wu/tick with corr stuck at -23° for hundreds of ticks). If
      -- we're in range with a clear shot but barely closed any distance over
      -- the last ATTACK_BASE_STALL_WINDOW ticks, give up on arriving: latch
      -- engage mode and shell the base from right here. The latch holds until the
      -- shot degrades (out of range / blocked resets in the else branch
      -- below) or the goal moves to a different base.
      local now = state.tick or 0
      local st = state._ab_stall
      if not st or st.base_mx ~= goal.mx or st.base_my ~= goal.my
         or now - (st.t or now) > 3 then
        st = { base_mx = goal.mx, base_my = goal.my,
               ref_wdist = wdist_base, ref_t = now, latched = false }
        state._ab_stall = st
      end
      st.t = now
      if not attack_base_engaging and not st.latched then
        -- Windowed (not per-tick) so knockback's spiky rhythm — shove back,
        -- re-accelerate, shove back — averages out instead of resetting a
        -- consecutive-slow-ticks counter on every brief fast stretch.
        local W = C.ATTACK_BASE_STALL_WINDOW or 50
        if now - st.ref_t >= W then
          local closed = st.ref_wdist - wdist_base
          if closed < (C.ATTACK_BASE_STALL_WU_PER_TICK or 6) * W then
            -- Only latch if a shell fired at the AIM heading would actually
            -- cross the base tile. shot_ok tolerates ATTACK_BASE_MAX_WALLS
            -- walls in the lane (the rush grinds them down) — stopping
            -- behind one would aim forever without the fire sim below ever
            -- passing. Until the lane is truly clear, keep rushing.
            local pa = cpf.simulate_shot_angle(info.tankx, info.tanky, aim_dir,
                                               cpf.SHOT_TANK, info.gunrange or 14)
            if pa then
              for _, t in ipairs(pa) do
                if t.mx == goal.mx and t.my == goal.my then st.latched = true; break end
              end
            end
            if BRAIN_DEBUG_MODE and st.latched then
              print2(string.format(
                "BASE_STALL_ENGAGE t=%d wdist=%.0f closed=%.0fwu over %d ticks -> stop and shoot from here",
                now, wdist_base, closed, W))
            end
          end
          st.ref_wdist = wdist_base
          st.ref_t = now
        end
      end
      if st.latched then
        -- Stop trying to advance; hold position and aim like the arrived case.
        keys = keys & ~KEY_FASTER
        if info.speed > 0 then keys = keys | KEY_SLOWER end
        attack_base_engaging = true
      end

      -- Heading control depends on whether we've ARRIVED at the engage point:
      --   * arrived (attack_base_engaging — nav block idle this tick): hijack the
      --     heading to point the body straight at the base and hold. Continuous
      --     aimed shelling from the standoff.
      --   * still closing in (nav block driving): DON'T steal the heading. A tank
      --     aims by its BODY heading, so locking onto the base here fights the nav
      --     heading to the engage point — the throttle can't satisfy "drive toward
      --     the engage point" and "aim at the base" at once, so it brakes and the
      --     tank fires forever from range without advancing. Instead let nav drive
      --     us in; we still fire opportunistically below whenever the body heading
      --     sweeps across the base. Advance + shoot, simultaneously.
      if attack_base_engaging then
        keys = keys & ~(KEY_TURNLEFT | KEY_TURNRIGHT)
        taps = taps & ~(KEY_TURNLEFT | KEY_TURNRIGHT)
        if     corr >  10 then keys = keys | KEY_TURNRIGHT
        elseif corr < -10 then keys = keys | KEY_TURNLEFT
        elseif corr >   2 then taps = taps | KEY_TURNRIGHT
        elseif corr <  -2 then taps = taps | KEY_TURNLEFT
        end
      end
      -- FIRE decision is NOT the heading-correction angle — keep turning freely
      -- toward dead-center (taps above) for tighter follow-up shots. Instead,
      -- simulate the shell at our CURRENT heading: if its trajectory crosses the
      -- base tile, that's good enough — shoot. The sim is bit-exact with the
      -- engine and terminates at the first wall/pillbox/base, so "reaches the base
      -- tile" already means no pillbox or OTHER base is in the way. We additionally
      -- reject an ALLIED tank sitting in the lane (friendly fire). This kills the
      -- old corr<3 deadband that left the tank lined-up-enough but never firing.
      local firing = false
      do
        local p = cpf.simulate_shot_angle(info.tankx, info.tanky, info.direction,
                                          cpf.SHOT_TANK, info.gunrange or 14)
        if p then
          local ally_tiles
          if info.objects then
            for _, ob in ipairs(info.objects) do
              if ob.type == OBJECT_TANK and (ob.info & OBJECT_HOSTILE) == 0
                 and ob.idnum ~= info.player_number then
                ally_tiles = ally_tiles or {}
                ally_tiles[(ob.y >> 8) * 256 + (ob.x >> 8)] = true
              end
            end
          end
          for _, t in ipairs(p) do
            if ally_tiles and ally_tiles[t.my * 256 + t.mx] then break end  -- friendly in the lane → hold
            if t.mx == goal.mx and t.my == goal.my then firing = true; break end
          end
        end
      end
      if firing then
        keys = keys | KEY_SHOOT
      end
      if BRAIN_DEBUG_MODE and not firing then print2(string.format("BASE_NOFIRE t=%d reason=heading_misses corr=%.1f wdist=%.0f (shell at current heading doesn't cross base tile)", state.tick or 0, corr, wdist_base)) end

      log.reason("steer", {
        mode = "attack_base", aim_corr = corr,
        firing = firing, stalled = st.latched or nil,
        base_dist = wdist_base, shot_ok = true,
      })
    else
      -- Out of range or no valid shot (pill/base/ally/2+ walls in the way):
      -- the nav block above is driving us to the closest adjacent tile for a
      -- clean point-blank shot. Just log the wait state. Any stall latch is
      -- void here — a blocked/out-of-range shot means we MUST keep moving.
      state._ab_stall = nil
      if BRAIN_DEBUG_MODE then print2(string.format("BASE_NOFIRE t=%d reason=%s wdist=%.0f range=%d shot_ok=%s inboat=%s", state.tick or 0, (wdist_base > C.ATTACK_PILL_RANGE * 256) and "out_of_range" or (not shot_ok and "shot_blocked" or "inboat"), wdist_base, C.ATTACK_PILL_RANGE * 256, tostring(shot_ok), tostring(info.inboat))) end
      log.reason("steer", {
        mode = "attack_base_approach",
        base_dist = wdist_base, shot_ok = shot_ok,
      })
    end
  elseif not attack_in_range then
    if state.tick % 10 == 0 then
      log.reason("steer", { mode = "idle", why = "no goal or at destination" })
    end
  end

  -- Forest lane-clear: driving THROUGH forest is slow (speed 6 vs 16). When
  -- the tank's CURRENT square is forest, fire ONE shot straight ahead — the
  -- shell turns the forest tile it lands on into grass (shells.c FOREST
  -- case), opening the lane. Once per forest tile entered (keyed on the
  -- tank tile), so a long crossing clears as it goes without spamming.
  -- Applies to capture_pill (racing to a dead pill) and, during the OPENING
  -- phase, capture_base (the land-grab race — later-game base drives don't
  -- justify advertising our position with tree shots).
  if (goal.kind == "capture_pill"
      or (goal.kind == "capture_base" and state.phase == "opening"))
     and not info.inboat
     and (info.shells or 0) > (C.SHELL_RESERVE or 0) then
    local fmx, fmy = info.tankx >> 8, info.tanky >> 8
    if U.ttype(fmx, fmy) == C.T_FOREST then
      local fkey = fmy * 256 + fmx
      if goal._forest_shot_key ~= fkey then
        -- Never fire the blind straight-ahead shot with our own or an
        -- allied LGM anywhere on the shell's path — a shell landing on the
        -- man kills it. Tile key is only consumed when we actually fire,
        -- so the shot re-arms once the LGM moves clear.
        local occ = nil
        if info.man_status ~= C.LGM_INTANK and info.man_x then
          occ = { [(info.man_y >> 8) * 256 + (info.man_x >> 8)] = true }
        end
        for _, al in ipairs((state.perc and state.perc.allied_lgm_positions) or {}) do
          occ = occ or {}
          occ[al.my * 256 + al.mx] = true
        end
        local lgm_clear = true
        if occ then
          local p = cpf.simulate_shot_angle(info.tankx, info.tanky, info.direction,
                                            cpf.SHOT_TANK, info.gunrange or 14)
          if p then
            for _, t in ipairs(p) do
              if occ[t.my * 256 + t.mx] then lgm_clear = false; break end
            end
          end
        end
        if lgm_clear then
          goal._forest_shot_key = fkey
          keys = keys | KEY_SHOOT
          print2(string.format("CAPTURE_FOREST_SHOT t=%d tile=(%d,%d) — clear lane ahead", state.tick or 0, fmx, fmy))
        elseif goal._forest_hold_key ~= fkey then
          goal._forest_hold_key = fkey
          print2(string.format("CAPTURE_FOREST_SHOT t=%d tile=(%d,%d) HELD — friendly LGM on the shot line", state.tick or 0, fmx, fmy))
        end
      end
    end
  end

  if BRAIN_PROFILE then
    local _total_us = _t_nav_apply_start - _t_nav_dispatch_start
    local _setup_us = _total_us - _t_path_us - _t_los_us
    if _setup_us < 0 then _setup_us = 0 end
    local _path_misc_us = _t_path_us
                         - _path_search_us - _path_trace_us - _path_lookahead_us
    if _path_misc_us < 0 then _path_misc_us = 0 end
    opt(string.format("    search (%s) done %.2f ms",
                      _path_method == "dij" and "dijkstra" or "A*",
                      _path_search_us / 1000))
    opt(string.format("    trace done %.2f ms",     _path_trace_us     / 1000))
    opt(string.format("    lookahead done %.2f ms", _path_lookahead_us / 1000))
    opt(string.format("    misc done %.2f ms",      _path_misc_us      / 1000))
    opt(string.format("  steer/nav-dispatch/path done %.2f ms",  _t_path_us / 1000))
    opt(string.format("  steer/nav-dispatch/los done %.2f ms",   _t_los_us / 1000))
    opt(string.format("  steer/nav-dispatch/setup done %.2f ms", _setup_us / 1000))
    opt(string.format("  steer/nav-apply done %.2f ms",
                      (clock_us() - _t_nav_apply_start) / 1000))
  end
  return keys, taps
end

return M
