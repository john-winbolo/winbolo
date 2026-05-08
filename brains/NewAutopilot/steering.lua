-- =========================================================================
-- NewAutopilot/steering.lua — translate goal + pathfinder into holdkeys/tapkeys
-- =========================================================================

local C   = require("constants")
local U   = require("util")
local PF  = require("pathfinder")
local cpf = require("cpathfinder")
local log = require("logger")
local bpc = require("bpc")
local viz = require("viz")
local opt = require("optimize")

local M = {}

-- Debug logging toggle — set via API: curl http://localhost:29016/steerdebug?on
M.debug = false

local function sdbg(fmt, ...)
  if M.debug then print("[STEER] " .. string.format(fmt, ...)) end
end

-- Local alias for the shared turn+speed helper in util.lua
local nav_turn_speed = U.nav_turn_speed

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
}
local _pp_stationary = {
  dispatch=true, wait_place=true, prewait=true, advance=true,
  shield_engage=true, engage=true, reposition=true, finish=true,
  select_pill=true,
}
local _at_stationary = { engage=true, close=true, disengage=true }

local function intentionally_stationary(goal)
  local s = goal.substate or ""
  if goal.kind == "attack_pill" and _ap_stationary[s] then return true end
  if goal.kind == "pill_place"  and _pp_stationary[s] then return true end
  if goal.kind == "attack_tank" and _at_stationary[s] then return true end
  if goal.kind == "rescue_lgm" or goal.kind == "none" then return true end
  if goal.kind == "refuel_at_base" or goal.kind == "flee_to_base" then return true end
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

  if state.wall_clearing or intentionally_stationary(goal) then
    state.stuck_progress = nil
    return
  end

  local pf = state.pf
  if not pf or pf.next_mx == nil or pf.next_mx < 0 then
    state.stuck_progress = nil
    return
  end

  local sp = state.stuck_progress
  if sp == nil
     or sp.next_mx ~= pf.next_mx
     or sp.next_my ~= pf.next_my
     or math.abs(info.tankx - sp.last_tankx) > STUCK_MOVE_WU
     or math.abs(info.tanky - sp.last_tanky) > STUCK_MOVE_WU then
    state.stuck_progress = {
      last_tankx = info.tankx, last_tanky = info.tanky,
      next_mx    = pf.next_mx, next_my    = pf.next_my,
      since      = now,
    }
    return
  end

  if (now - sp.since) < STUCK_TICKS then return end

  -- No progress for STUCK_TICKS toward the same next-step tile: penalize it.
  local k = U.mkey(pf.next_mx, pf.next_my)
  if bl[k] == nil then
    cpf.set_overlay(pf.next_mx, pf.next_my, STUCK_PENALTY)
    print(string.format(
      "[STUCK_RECOVERY] t=%d pos=(%d,%d) spd=%d goal=%s next=(%d,%d) penalize %dt",
      now, info.tankx >> 8, info.tanky >> 8, info.speed or 0,
      goal.kind, pf.next_mx, pf.next_my, STUCK_DURATION))
    log.event("stuck_recovery", string.format(
      "%s next=%d,%d", goal.kind, pf.next_mx, pf.next_my))
  end
  bl[k] = now + STUCK_DURATION
  state.pf.status = "idle"  -- force A* recompute against the new overlay
  state.stuck_progress = nil
end

-- Diagnostic: log every write to pf.next_mx so we can see where a
-- multi-tile cheb value came from. Writes to pf_next_ms.log in the cwd.
local function log_pf_next(state, info, who, new_mx, new_my, extra)
  local f = io.open("pf_next_ms.log", "a")
  if not f then return end
  local tmx = (info and info.tankx) and (info.tankx >> 8) or -1
  local tmy = (info and info.tanky) and (info.tanky >> 8) or -1
  local cheb = (new_mx >= 0 and tmx >= 0)
    and math.max(math.abs(new_mx - tmx), math.abs(new_my - tmy))
    or -1
  f:write(string.format(
    "t=%d who=%s tank=(%d,%d) new=(%d,%d) cheb=%d %s\n",
    state and state.tick or -1,
    who, tmx, tmy, new_mx, new_my, cheb, extra or ""))
  f:close()
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

  -- capture_pill: a just-died pill is still impassable in the cached
  -- dijkstra slate (overlay was 32767 when it was alive). Force fresh
  -- A* every tick so we route to the (now drivable) pill tile rather
  -- than treating it as unreachable.
  local skip_dijkstra = state.goal and state.goal.kind == "capture_pill"
  local status, nx, ny = cpf.path_to(tmx, tmy, dest_mx, dest_my, in_boat, shells, trees, mines, armour, C.ASTAR_BUDGET, skip_dijkstra)

  -- Update state.pf tracking fields
  pf.src_mx  = tmx
  pf.src_my  = tmy
  pf.dest_mx = dest_mx
  pf.dest_my = dest_my

  if status == 1 then      -- done
    pf.status  = "done"
    pf.next_mx = nx
    pf.next_my = ny
    log_pf_next(state, info, "cpf_path_to:done", nx, ny,
      string.format("dest=(%d,%d)", dest_mx, dest_my))
    pf.age     = 0
    -- Capture full path chain for debug logging + path_lookahead.
    -- Try A* trace first (works when A* ran); fall back to Dijkstra
    -- trace (the common case now since cpf.path_to tries Dijkstra first
    -- and skips A* when it succeeds — leaving the A* state stale).
    pf.path_chain = cpf.trace_path()
    if not pf.path_chain or #pf.path_chain == 0 then
      pf.path_chain = cpf.dijkstra_trace_path(cpf.KIND_NORMAL, dest_mx, dest_my)
    end
  elseif status == 0 then  -- running
    pf.status = "running"
    if nx >= 0 then
      pf.next_mx = nx
      pf.next_my = ny
      log_pf_next(state, info, "cpf_path_to:running", nx, ny,
        string.format("dest=(%d,%d)", dest_mx, dest_my))
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
        log_pf_next(state, info, "cpf_path_to:chain_fallback", nxt_mx, nxt_my,
          string.format("best_i=%d chain#=%d dest=(%d,%d)",
                        best_i, nwp, dest_mx, dest_my))
      end
    end
    pf.age = (pf.age or 0) + 1
  else                      -- failed (-1)
    pf.status  = "failed"
    pf.next_mx = -1
    pf.next_my = -1
    log_pf_next(state, info, "cpf_path_to:failed", -1, -1,
      string.format("dest=(%d,%d)", dest_mx, dest_my))
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
  for i = start_idx + 1, chain_nwp do
    local cx, cy = chain[2*i-1], chain[2*i]
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

    -- Turn so crosshairs align with the purple aim dot (not standoff center).
    -- This way the tank is already aimed when it arrives.
    local aim_tx = goal.aim_mx or (goal.mx + 0.5)
    local aim_ty = goal.aim_my or (goal.my + 0.5)
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

    -- Fine turn toward aim point
    if     corr >  1 then taps = taps | KEY_TURNRIGHT
    elseif corr < -1 then taps = taps | KEY_TURNLEFT
    end

    -- Arrived — brake to full stop, then engage
    -- Also trigger if tank overshot (closer to pill than standoff is)
    local pill_wx, pill_wy = U.m2w(goal.mx), U.m2w(goal.my)
    local tank_to_pill = U.wdist(info.tankx, info.tanky, pill_wx, pill_wy)
    local standoff_to_pill = U.wdist(swx, swy, pill_wx, pill_wy)
    
    -- Shoot during charge if inside standoff range AND a shell-sim
    -- says the trajectory actually crosses the pill tile. The old
    -- `corr <= 5` brad gate let through edge-of-pill shots that
    -- physically miss (5 brads ≈ 7°; at 7-tile range that's ~0.85
    -- tile lateral error — wider than the pill). Sim is bit-exact
    -- with the engine.
    local dist_to_pill = tank_to_pill
    if BRAIN_DEBUG_MODE then
      viz.hud_text("charge_status", 10, 75, string.format("   dist_to_pill=%d <= %d corr=%.1f",
                                                           dist_to_pill, math.floor(C.ATTACK_PILL_STANDOFF * 256), corr),
                   "topleft", 255, 255, 0)
    end
    if dist_to_pill <= C.ATTACK_PILL_STANDOFF * 256
       and math.abs(corr) <= 5 and info.shells > C.SHELL_RESERVE then
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

    if sdist < 50 or tank_to_pill < standoff_to_pill then
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
    if goal._is_ppt then
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

    -- Once we start braking, commit to it (no re-accelerating).
    -- Exception: if we've stalled to a full stop well before the
    -- arrival window (sdist > 80, vs the 50-wu arrival check above),
    -- something blocked us — clear the brake flag so the next tick
    -- can KEY_FASTER and try to push through.
    if goal._charge_braking and info.speed == 0 and sdist > 80 then
      goal._charge_braking = nil
    end
    local _deceling = stop_dist >= sdist or goal._charge_braking
    if _deceling then
      goal._charge_braking = true
      keys = keys | KEY_SLOWER
    else
      keys = keys | KEY_FASTER
    end

    if BRAIN_DEBUG_MODE then
      if _deceling then
        charge_phase = string.format("DECEL spd=%d stop=%d dist=%d", info.speed, stop_dist, sdist)
      else
        charge_phase = string.format("ACCEL spd=%d stop=%d dist=%d", info.speed, stop_dist, sdist)
      end
      viz.hud_text("charge_status", 10, 56, "CHARGE: " .. charge_phase, "topleft", 255, 255, 0)
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
    if math.abs(corr) <= 5 and info.shells > C.SHELL_RESERVE then
      keys = keys | KEY_SHOOT
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
    local afx = goal.approach_fx or (goal.approach_mx and (goal.approach_mx + 0.5))
                                  or (goal.standoff_mx and (goal.standoff_mx + 0.5))
    local afy = goal.approach_fy or (goal.approach_my and (goal.approach_my + 0.5))
                                  or (goal.standoff_my and (goal.standoff_my + 0.5))
    if afx then
      local awx = math.floor(afx * 256 + 0.5)
      local awy = math.floor(afy * 256 + 0.5)
      local adist = U.wdist(info.tankx, info.tanky, awx, awy)
      if adist <= 256 then
        -- Within 1 tile of the approach point. Creep toward it until
        -- within DIST_TOL (1/16 tile = 16 wu, matches attack.lua's
        -- approach-completion threshold). Lowering this from 64 makes
        -- the tank push right up onto the spot instead of braking
        -- early and coasting to a stop a quarter-tile short.
        if adist > 16 then
          local move_dir = U.aim_at(info.tankx, info.tanky, awx, awy)
          local corr = U.adiff(info.direction, move_dir)
          local k, t = nav_turn_speed(corr, info.speed, 4, 1)
          keys = keys | k
          taps = taps | t
        elseif info.speed > 0 then
          keys = keys | KEY_SLOWER
        end
        return keys, taps
      end
    end
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

  -- Find the current target tank from perception (it moves every tick)
  local perc = state.perc or {}
  local target = nil
  local target_dist = math.huge

  -- Match by proximity to goal position (tank may have moved since goal was set)
  for _, et in ipairs(perc.enemy_tanks or {}) do
    local d = U.mdist(et.mx, et.my, goal.mx, goal.my)
    if d < target_dist then
      target_dist = d
      target = et
    end
  end

  -- If we can't see any enemy tank near the goal, find nearest visible one
  if not target or target_dist > 8 then
    local best_d = math.huge
    for _, et in ipairs(perc.enemy_tanks or {}) do
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

  -- Update goal position to track the moving target
  goal.mx = target.mx
  goal.my = target.my
  goal.wx = U.m2w(target.mx)
  goal.wy = U.m2w(target.my)

  local dist_tiles = target.dist
  -- Use sub-tile precise WU position (ob.x/ob.y), not tile-center from m2w
  local twx = target.wx
  local twy = target.wy

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

  -- Disengage check: flee if outgunned
  if info.armour <= C.TANK_COMBAT_FLEE_ARMOUR
     or info.shells <= C.TANK_COMBAT_FLEE_SHELLS then
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
      for deg = 0, 355, 5 do
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

    -- No shooting while closing — wait until in range (engage substate)

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
  local shell_speed_per_tick = C.TANK_COMBAT_SHELL_SPEED * 2
  local shell_travel_ticks = wdist / shell_speed_per_tick
  local svx = target.svx or 0
  local svy = target.svy or 0
  -- Skip lead-prediction when target is essentially stationary. Use
  -- the actual smoothed velocity magnitude (WU/tick), not target.speed
  -- which is the engine's SPEEDTYPE in a different scale and isn't
  -- directly comparable. ≤8 wu/tick = ≤0.03 tile/tick = barely moving.
  if (svx * svx + svy * svy) <= 64 then svx = 0; svy = 0 end
  local pred_wx = twx + svx * shell_travel_ticks
  local pred_wy = twy + svy * shell_travel_ticks

  -- Debug: lead prediction overlay (red = target, orange = predicted)
  if BRAIN_DEBUG_MODE then
    viz.circle("tank_combat_viz", twx / 256.0, twy / 256.0, 0.3, 255, 50, 50, 180)
    viz.circle("tank_combat_viz", pred_wx / 256.0, pred_wy / 256.0, 0.3, 255, 165, 0, 200)
    viz.line("tank_combat_viz", twx / 256.0, twy / 256.0,
                 pred_wx / 256.0, pred_wy / 256.0, 255, 165, 0, 140)
  end

  local aim_dir = U.aim_at(info.tankx, info.tanky, pred_wx, pred_wy)
  local aim_corr = U.adiff(info.direction, aim_dir)

  -- Jink: periodic lateral offset to make us harder to hit
  -- Alternate direction every JINK_PERIOD ticks
  local jink_phase = math.floor(now / C.TANK_COMBAT_JINK_PERIOD) % 2
  local jink_offset = jink_phase == 0 and C.TANK_COMBAT_JINK_ANGLE
                                       or -C.TANK_COMBAT_JINK_ANGLE

  -- Turn toward predicted target position
  if     aim_corr >  10 then keys = keys | KEY_TURNRIGHT
  elseif aim_corr < -10 then keys = keys | KEY_TURNLEFT
  elseif aim_corr >   2 then taps = taps | KEY_TURNRIGHT
  elseif aim_corr <  -2 then taps = taps | KEY_TURNLEFT
  end

  -- Fire when aimed — wider tolerance because lead prediction compensates
  if math.abs(aim_corr) < 8 and info.shells > C.TANK_COMBAT_FLEE_SHELLS then
    keys = keys | KEY_SHOOT
  end

  -- Distance control: maintain optimal range with jinking
  if dist_tiles < C.TANK_COMBAT_TOO_CLOSE then
    -- Too close: reverse away
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    -- Jink by turning slightly off-axis
    if jink_offset > 0 then
      taps = taps | KEY_TURNRIGHT
    else
      taps = taps | KEY_TURNLEFT
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

  log.reason("steer", {
    mode = "tank_combat_engage",
    dist = dist_tiles, aim_corr = aim_corr,
    lead_wx = pred_wx, lead_wy = pred_wy,
    speed = target.speed, dir = target.obj and target.obj.direction or 0,
    ob_speed = target.obj and target.obj.speed or -1,
    spd_wu = target.speed / 4,
    wdist = wdist, shell_t = shell_travel_ticks,
    jink = jink_offset,
    firing = math.abs(aim_corr) < 5 and info.shells > C.TANK_COMBAT_FLEE_SHELLS,
  })
  return keys, taps
end

function M.steer(state, world, info, goal)
  local _t_steer_start = BRAIN_PERF_LOG and clock_us() or 0
  local keys = 0
  local taps = 0
  local tmx  = info.tankx >> 8
  local tmy  = info.tanky >> 8
  state._steer_lx = nil
  state._steer_ly = nil

  -- Per-tile stuck-recovery: re-stamp the dynamic blacklist into the overlay
  -- (init.lua wipes it each tick) and watch progress toward pf.next_mx/my.
  stuck_recovery(state, info, goal)
  local _t_after_stuck = BRAIN_PERF_LOG and clock_us() or 0

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
      return KEY_SLOWER, 0
    end
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
      return k, t
    end
  end

  -- Pill placement: self-contained steering for all pill_place substates
  if goal.kind == "pill_place" then
    local k, t = pill_place_steer(state, world, info, goal)
    if k then return k, t end
  end

  -- Attack pill: aim, engage, rush, plan_position substates
  if goal.kind == "attack_pill" then
    local k, t = attack_pill_steer(state, world, info, goal)
    if k then return k, t end
  end

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
           and (goal.substate == "approach"
                or goal.substate == "aim"
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

  elseif goal.kind == "wait_for_lgm" then
    -- Stand still and let the LGM finish whatever he's doing
    -- (farming, opportunistic build) before chasing new goals.
    -- Was incorrectly placed inside attack_pill_steer where it was
    -- unreachable; moved here to actually fire.
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps

  elseif goal.kind == "refuel_at_base" then
    -- Navigate to the base if not on it yet; brake if already there
    local on_base = (tmx == goal.mx and tmy == goal.my)
    if not on_base then
      local nx, ny = cpf_path_to(state, info, goal.mx, goal.my)
      if nx then
        local lx, ly = path_lookahead(state, info, nx, ny)
        state._steer_lx = lx
        state._steer_ly = ly
        move_dir    = U.aim_at(info.tankx, info.tanky, U.m2w(lx), U.m2w(ly))
        target_dist = U.wdist(info.tankx, info.tanky, U.m2w(lx), U.m2w(ly))
      end
      goal_dist = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)

      -- Nav debug overlay (same as the generic navigate branch below)
      if BRAIN_DEBUG_MODE then
        local pf = state.pf
        local twx, twy = info.tankx / 256.0, info.tanky / 256.0
        if pf.next_mx and pf.next_mx >= 0 then
          viz.rect("steering_text", pf.next_mx, pf.next_my, pf.next_mx + 1, pf.next_my + 1,
                       255, 255, 0, 200)
        end
        if state._steer_lx then
          viz.circle("nav_lookahead_marker", state._steer_lx + 0.5, state._steer_ly + 0.5, 0.4,
                         255, 0, 255, 230)
        end
        if move_dir and state._steer_lx then
          viz.line("nav_lookahead_marker", twx, twy, state._steer_lx + 0.5, state._steer_ly + 0.5,
                       255, 0, 255, 160)
        end
        -- Nav destination (white circle at the base tile)
        viz.circle("pf_destination", goal.mx + 0.5, goal.my + 0.5, 0.3, 255, 255, 255, 180)
        if state.next_goal and state.next_goal.wx and state.next_goal.wy then
          local ngx = state.next_goal.wx / 256.0
          local ngy = state.next_goal.wy / 256.0
          viz.line("pf_destination", goal.mx + 0.5, goal.my + 0.5, ngx, ngy, 100, 0, 140, 220)
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
    -- attack_base: navigate to the closest non-water adjacent tile so we
    -- end up beside the base with a clear shot rather than on top of it.
    if goal.kind == "attack_base" then
      local bmx, bmy = goal.mx, goal.my
      local best_amx, best_amy = nil, nil
      local best_d = math.huge
      for _, delta in ipairs({{-1,0},{1,0},{0,-1},{0,1}}) do
        local cx = U.mclamp(bmx + delta[1])
        local cy = U.mclamp(bmy + delta[2])
        if not U.is_water(U.ttype(cx, cy)) then
          local d = math.abs(cx - tmx) + math.abs(cy - tmy)
          if d < best_d then
            best_d = d
            best_amx, best_amy = cx, cy
          end
        end
      end
      if best_amx then
        nav_mx, nav_my = best_amx, best_amy
        nav_wx, nav_wy = U.m2w(nav_mx), U.m2w(nav_my)
      end
    end
    -- capture_pill: route directly to the pill tile. The cached
    -- dijkstra slate may still treat the pill as alive/impassable
    -- (overlay 32767 baked in when it had health > 0), so cpf_path_to
    -- forces fresh A* via skip_dijkstra=true. The pill tile itself
    -- has no overlay applied for dead pills (init.lua only marks
    -- pm.health > 0), so A* will route onto it.
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
        -- Legacy fallback: stand off on the line pill→tank
        local dx  = tmx - goal.mx
        local dy  = tmy - goal.my
        local len = math.sqrt(dx * dx + dy * dy)
        if len > 0.1 then
          nav_mx = U.mclamp(math.floor(goal.mx + dx / len * C.ATTACK_PILL_STANDOFF + 0.5))
          nav_my = U.mclamp(math.floor(goal.my + dy / len * C.ATTACK_PILL_STANDOFF + 0.5))
          nav_wx = U.m2w(nav_mx)
          nav_wy = U.m2w(nav_my)
        end
      end
    -- (was: a second `elseif goal.kind == "attack_pill"` branch with
    -- a BPC_STANDOFF fallback — unreachable because the if branch
    -- above already matches attack_pill. The legacy ATTACK_PILL_STANDOFF
    -- fallback at line 1630-1640 covers the no-standoff_mx case.)
    end

    -- Follow the A* next-step waypoint, with path lookahead to reduce wiggle
    local _t_pre_path = BRAIN_PERF_LOG and clock_us() or 0
    local nx, ny = cpf_path_to(state, info, nav_mx, nav_my)
    local _t_post_path = BRAIN_PERF_LOG and clock_us() or 0
    if BRAIN_PERF_LOG and (_t_post_path - _t_pre_path > 3000 or _t_after_stuck - _t_steer_start > 3000) then
      opt.append("optimize.log", string.format(
        "  [steer-detail] tick=%d goal=%s stuck_r=%.2fms path_to=%.2fms dest=(%d,%d)",
        state.tick or 0, goal.kind or "?",
        (_t_after_stuck - _t_steer_start) / 1000,
        (_t_post_path - _t_pre_path) / 1000,
        nav_mx or -1, nav_my or -1))
    end

    if nx then
      -- Skip ahead on the path when the straight line is clear
      local lx, ly = path_lookahead(state, info, nx, ny)
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
        viz.text("steering_text",
                 pf.next_mx + 0.5, pf.next_my - 0.3,
                 string.format("pf.next=(%d,%d) cheb=%d [%s]",
                               pf.next_mx, pf.next_my, cheb, pf_status),
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

  -- No goal or idle: brake to a stop.
  -- When attack_in_range, skip the navigation block and fall through to
  -- the engage aim/shoot block below.
  if move_dir == nil and not attack_in_range then
    if info.speed > 0 then
      keys = keys | KEY_SLOWER
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
    -- Hoisted so the viz reads the same locals the logic uses.
    local turn_base_cap = 0
    local turn_factor   = 1.0
    local turn_capped   = 256    -- after ramp, before distance ease
    local ramp_start    = plow_through and 20 or 10
    local plow_dist_t   = plow_through and (eff_dist / 256.0) or 0
    local plow_ease     = 0.0    -- 0 = full ramp, 1 = no cap
    if abs_corr > ramp_start then
      if plow_through then
        turn_base_cap = (under_fire or race_mode) and 128 or 96
      else
        turn_base_cap = (under_fire or race_mode) and  64 or 48
      end
      turn_factor    = 1.0 - math.min((abs_corr - ramp_start) / 70.0, 1.0)
      turn_capped    = math.max(6, math.floor(turn_factor * turn_base_cap))
      turn_max_speed = turn_capped

      if plow_through then
        if plow_dist_t >= 10 then
          plow_ease = 1.0
        elseif plow_dist_t > 4 then
          plow_ease = (plow_dist_t - 4) / 6.0
        end
        if plow_ease > 0 then
          turn_max_speed = math.max(turn_max_speed,
            math.floor(turn_max_speed * (1.0 - plow_ease) + 256 * plow_ease))
        end
      end
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
        plow_dist_t, target_kind, math.floor(deg + 0.5), abs_corr)

      local decision_line
      if abs_corr <= ramp_start then
        decision_line = string.format(
          "|err|=%d <= ramp_start=%d brad  ->  no cap  (turn_max_speed=%d)",
          abs_corr, ramp_start, turn_max_speed)
      else
        local ease_note
        if plow_ease >= 1 then
          ease_note = "dist>=10t -> full ease, cap lifts to 256"
        elseif plow_ease <= 0 then
          ease_note = "dist<=4t -> full cap applies"
        else
          ease_note = string.format("dist in 4-10t -> ease=%.2f blend", plow_ease)
        end
        decision_line = string.format(
          "base_cap=%d x ramp_factor=%.2f = %d  ->  %s  ->  turn_max_speed=%d",
          turn_base_cap, turn_factor, turn_capped, ease_note, turn_max_speed)
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
      -- Boat-to-land transition needs high speed to disembark.
      -- Must take priority over LGM pacing or the tank gets stranded.
      -- Only boost when roughly facing the exit (< 24°); otherwise the
      -- tank overshoots the exit tile at speed and enters the wrong tile.
      keys = (keys & ~KEY_SLOWER) | KEY_FASTER
    elseif boat_exit then
      -- Facing away from exit — slow to turn, but keep above exit speed
      if info.speed > turn_max_speed + 4 then
        keys = keys | KEY_SLOWER
      elseif info.speed < 8 then
        keys = keys | KEY_FASTER
      end
    elseif tank_pace and info.speed > tank_pace then
      keys = keys | KEY_SLOWER
    elseif tank_pace and tank_pace > 0 and info.speed < tank_pace then
      keys = keys | KEY_FASTER
    elseif lgm_speed_cap and lgm_speed_cap == 0 then
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    elseif cliff and goal.kind ~= "escape_water" then
      keys = (keys & ~KEY_FASTER) | KEY_SLOWER
    elseif goal.kind == "escape_water" then
      keys = keys | KEY_FASTER
    elseif goal.kind == "attack_pill" and goal.substate == "approach" then
      -- BPC approach: navigate to standoff position, braking to stop exactly
      -- on it.  Uses distance to standoff (not pill) for braking calc.
      local smx = goal.standoff_mx or goal.mx
      local smy = goal.standoff_my or goal.my
      local sdist_wu = U.wdist(info.tankx, info.tanky, U.m2w(smx), U.m2w(smy))
      local approach_brake = math.max(256, info.speed * 24)
      if sdist_wu < approach_brake then
        -- Braking zone: slow proportionally
        local desired = math.max(4, math.floor(sdist_wu * 0.03))
        if info.speed > desired + 4 then
          keys = keys | KEY_SLOWER
        elseif info.speed < desired and sdist_wu > 128 then
          keys = keys | KEY_FASTER
        end
      elseif abs_corr > 80 then
        if info.speed > 8 then keys = keys | KEY_SLOWER end
      else
        -- Outside braking zone: go fast through the danger zone
        keys = keys | KEY_FASTER
      end
    elseif facing_away and C.FACING_AWAY_BRAKE_ENABLED then
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
      if info.speed > facing_brake then keys = keys | KEY_SLOWER end
    elseif orbit_brake then
      -- Stuck circling the destination — brake to tighten the turn.
      -- Cap at 8 wu/tick so the radius shrinks but momentum returns
      -- quickly once we land on the goal.
      if info.speed > 8 then keys = keys | KEY_SLOWER end
    elseif eff_dist < brake_dist and not plow_through then
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
      -- Plow-through: full speed, don't brake for destination
      if info.speed < turn_max_speed then
        keys = keys | KEY_FASTER
      elseif info.speed > turn_max_speed + 4 then
        keys = keys | KEY_SLOWER
      end
    else
      -- Cruise: target turn_max_speed with proportional control
      if info.speed > turn_max_speed + 4 then
        keys = keys | KEY_SLOWER
      elseif info.speed < turn_max_speed then
        keys = keys | KEY_FASTER
      end
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
    -- Valid targets: enemy tanks, enemy bases, friendly pills (to "piss" them
    -- into firing at nearby enemies). Never shoot enemy/neutral pills — wastes
    -- ammo and angers them for no gain.
    if not info.inboat and info.shells > C.SHELL_RESERVE then
      local perc = state.perc or {}
      local shot_fired = false

      -- Enemy tanks
      if not shot_fired then
        for _, et in ipairs(perc.enemy_tanks or {}) do
          if et.dist <= 8 then
            local aim = U.aim_at(info.tankx, info.tanky, U.m2w(et.mx), U.m2w(et.my))
            if math.abs(U.adiff(info.direction, aim)) < 8 then
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

      -- Enemy bases (free damage while passing)
      if not shot_fired then
        for _, b in pairs(world.bases) do
          if b.owner == "hostile" and b.health > 0 then
            local bd = U.mdist(tmx, tmy, b.mx, b.my)
            if bd <= 8 then
              local aim = U.aim_at(info.tankx, info.tanky, U.m2w(b.mx), U.m2w(b.my))
              if math.abs(U.adiff(info.direction, aim)) < 8 then
                taps = taps | KEY_SHOOT
                shot_fired = true
                break
              end
            end
          end
        end
      end

      -- Friendly pills: "piss" them to fire faster, but only if an enemy
      -- tank is nearby (otherwise pointless)
      if not shot_fired and perc.nearest_hostile_tank
         and perc.nearest_hostile_tank.dist <= C.PILL_RANGE_MAP then
        for _, p in pairs(world.pills) do
          if p.owner == "friendly" and p.health > 0 then
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
  -- Attack base: navigate to an adjacent tile, then shoot with clear LOS.
  -- Shells can be blocked by walls/trees between the tank and the base, so we
  -- only fire when wall_hp_between == 0.  If blocked we keep driving to the
  -- adjacent nav target (set above in the nav block) until the path is clear.
  elseif goal.kind == "attack_base" and info.shells > C.SHELL_RESERVE then
    local wdist_base = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    local los_ok = PF.wall_hp_between(tmx, tmy, goal.mx, goal.my) == 0
    if wdist_base <= C.ATTACK_PILL_RANGE * 256 and los_ok and not info.inboat then
      local aim_dir = U.aim_at(info.tankx, info.tanky, goal.wx, goal.wy)
      local corr    = U.adiff(info.direction, aim_dir)

      if info.gunrange < C.GUNSIGHT_MAX then
        keys = keys | KEY_MORERANGE
      end

      -- Override turn keys: point at the base
      keys = keys & ~(KEY_TURNLEFT | KEY_TURNRIGHT)
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

      -- Slow down while shooting to maintain range
      if info.speed > 8 then
        keys = keys & ~KEY_FASTER
        keys = keys | KEY_SLOWER
      end

      log.reason("steer", {
        mode = "attack_base", aim_corr = corr,
        firing = math.abs(corr) < 3 and not still_correcting,
        base_dist = wdist_base, los_ok = true,
      })
    else
      -- Not in range or LOS blocked: navigation (set in nav block above) is
      -- driving us to the closest adjacent tile.  Just log the wait state.
      log.reason("steer", {
        mode = "attack_base_approach",
        base_dist = wdist_base, los_ok = los_ok,
      })
    end
  elseif not attack_in_range then
    if state.tick % 10 == 0 then
      log.reason("steer", { mode = "idle", why = "no goal or at destination" })
    end
  end

  return keys, taps
end

return M
