-- =========================================================================
-- NewAutopilot/init.lua — Brain entry point (open/think/close/settings)
-- =========================================================================

local C       = require("constants")
local TAG     = "[" .. C.BRAIN_NAME .. "]"
local U       = require("util")
local heap    = require("heap")
local W       = require("world")
local expl    = require("exploration")
local PF      = require("pathfinder")
local cpf     = require("cpathfinder")
local wsim    = require("cworldsim")
local cmds    = require("commands")
local goals   = require("goals")
local attack  = require("attack")
local steer   = require("steering")
local bpc     = require("bpc")
local log     = require("logger")
local danger  = require("danger")
local builder = require("builder")
local metrics = require("metrics")
local changes = require("changes")
local percept  = require("perception")
local strategy = require("strategy")
local threat   = require("threat")
local hearing  = require("hearing")

local Brain = {}

local AUTOSTART = true
local ENABLE_LOGGING = false

-- =========================================================================
-- Persistent state
-- =========================================================================

local state = {
  tick          = 0,
  player_number = 0,
}

local world = {
  bases = {},
  pills = {},
}


-- =========================================================================
-- Settings API
-- =========================================================================

function Brain.settings()
  return {
    { id="command", label="Command (base:N, pill:N, attack:N, stop, start, status)",
      type="string", value="" },
    { id="auto_explore", label="Auto explore", type="bool",
      value=state.auto_explore or false },
    { id="logging", label="Write tick log to file", type="bool",
      value=log.is_open() },
  }
end

function Brain.set_setting(id, value)
  if id == "command" and type(value) == "string" and value ~= "" then
    print(string.format(TAG .. " COMMAND: '%s'", value))
    local cmd = cmds.parse(value)
    if cmd then
      local reply = cmds.execute(cmd, state, world)
      if reply then
        print(TAG .. " REPLY: " .. reply)
      end
    else
      print(TAG .. " unknown command: " .. value)
    end
  elseif id == "auto_explore" then
    state.auto_explore = value
    print(string.format(TAG .. " auto_explore = %s", tostring(value)))
  elseif id == "logging" then
    if value and not log.is_open() then
      if log.open(log.make_filename("brain_p" .. state.player_number)) then
        log.dump_map()
        log.dump_world(world)
      end
    elseif not value and log.is_open() then
      log.close()
    end
  end
end

-- Debug info for BrainTest viewer (called from C via lua_pcall)
function Brain.get_debug_info()
  local g = state.goal or {}
  local info = {
    kind      = g.kind or "none",
    mx        = g.mx or 0,
    my        = g.my or 0,
    target_id = g.target_id or -1,
    substate  = g.substate or "",
    pool      = state.last_goal_pool or {},
  }
  return info
end


-- =========================================================================
-- OPEN
-- =========================================================================

function Brain.open(info)
  -- Clear module-level caches from any previous game
  U.reset()
  PF.reset()
  cpf.configure()
  wsim.configure()
  danger.reset()
  threat.reset()
  hearing.reset()
  for i = #changes.terrain, 1, -1 do changes.terrain[i] = nil end

  state.tick          = 0
  state.player_number = info.player_number
  state.player_name   = (info.player_names and info.player_names[info.player_number + 1]) or ""
  state.debug_log     = (state.player_name == "Bot 1" or info.player_number == 0)
  state.send_open_msg = true

  -- Silence print() for non-debug bots so only Bot 1 produces console output.
  -- Save the real print on first call; restore it in Brain.close().
  if not state._real_print then state._real_print = print end
  if state.debug_log then
    print = state._real_print
  else
    print = function() end
  end
  state.ai_advantage  = info.gameinfo.assist_ai

  -- Pathfinder state
  state.pf = {
    status  = "idle",
    dest_mx = -1, dest_my = -1,
    src_mx  = -1, src_my  = -1,
    next_mx = -1, next_my = -1,
    age     = 0,
    in_boat = false,
    open    = nil,
    g_cost  = nil,
    parent  = nil,
    closed  = nil,
  }

  -- Goal
  state.goal = { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }

  -- Builder context (populated each tick by builder.set_mode)
  -- last_action: BUILDMODE_* of the most recent LGM dispatch (persists across ticks
  --              so steering can decide whether to pace while the LGM is moving)
  state.builder = { mode = "infrastructure", target = nil, need_trees = 0, last_action = nil }

  -- Command system
  state.command_goal      = nil
  state.capture_objective = nil
  state.base_capture_objective = nil
  state.command_reply     = nil
  state.auto_explore      = AUTOSTART
  state.paused            = not AUTOSTART
  state.replan_offset     = math.random(0, C.GOAL_REPLAN_INTERVAL - 1)
  state.goal_set_tick     = 0    -- tick when current goal was chosen (for commitment hysteresis)

  -- Stuck detection
  state.last_mx   = -1
  state.last_my   = -1
  state.stuck_for = 0

  -- Boat state tracking (for detecting boat loss → forced replan)
  state.was_in_boat = false

  -- Blocked destinations: mkey -> expiry tick
  state.blocked = {}

  -- Exploration
  state.visited      = {}
  state.frontier     = heap.new()
  state.frontier_set = {}

  -- World knowledge
  world.bases = {}
  world.pills = {}
  W.reset(world)
  W.update(world, info, 0)

  -- Strategy phase detection
  strategy.init(state)

  print(string.format(TAG .. " open: player=%d name='%s' map=%s ai_advantage=%s debug_log=%s",
        info.player_number, state.player_name, info.gameinfo.mapname,
        tostring(state.ai_advantage), tostring(state.debug_log)))

  if state.ai_advantage then
    print(TAG .. " AI Advantage enabled -- full world scan")
  else
    print(TAG .. " No AI Advantage -- world knowledge limited to view")
  end

  -- Auto-start logging with timestamped filenames so sessions never overwrite.
  -- Player 0 always logs; other players log if ENABLE_LOGGING or debug_log.
  local log_fname = nil
  if info.player_number == 0 then
    log_fname = log.make_filename("player0")
  elseif ENABLE_LOGGING or state.debug_log then
    log_fname = log.make_filename("brain_p" .. info.player_number)
  end
  if log_fname then
    if log.open(log_fname) then
      log.dump_map()
      log.dump_world(world)
    end
  end
end


-- =========================================================================
-- THINK
-- =========================================================================

function Brain.think(info)
  state.tick = state.tick + 1
  local now  = state.tick
  if now == 1 then print(TAG .. " >>> CODE VERSION: turn-cost-v1 <<<") end

  -- Store server tick for absolute time references
  state.server_tick = info.server_tick or 0

  -- Section timing (clock_us is a C function returning microseconds)
  local t0 = clock_us()

  -- Process game events (instant updates before object scan)
  W.process_events(world, info, state)

  -- Handle assistant messages (builder feedback from server)
  if info.assistant_msg and info.assistant_msg ~= 0 then
    local msg = info.assistant_msg
    if msg == ASSIST_MSG_INSUFFICIENT_TREES then
      -- Need more trees: switch builder to gather mode
      state.builder.need_trees = math.max(state.builder.need_trees, 10)
      log.event("assist_msg", "insufficient_trees")
    elseif msg == ASSIST_MSG_NO_BUILD or msg == ASSIST_MSG_NO_BUILD_BOAT then
      -- Can't build here: blacklist this location
      if state.builder.target then
        local bk = U.mkey(state.builder.target.x, state.builder.target.y)
        state.blocked[bk] = now + 300
        log.event("assist_msg", string.format("no_build at %d,%d",
          state.builder.target.x, state.builder.target.y))
      end
    elseif msg == ASSIST_MSG_PILL_NO_REPAIR then
      -- Pill doesn't need repair: cancel repair goal
      if state.goal.kind == "repair_pill" then
        state.goal.kind = "none"
        state.pf.status = "idle"
        log.event("assist_msg", "pill_no_repair")
      end
    elseif msg == ASSIST_MSG_MAN_DEAD then
      -- LGM died: note for tactical decisions
      log.event("assist_msg", "man_dead")
    end
  end

  -- Selective cache invalidation based on pill/terrain changes
  PF.begin_tick(now, world)

  -- Update world knowledge
  W.update(world, info, now)

  -- Update exploration frontier
  expl.update(state, info)

  local t1 = clock_us()
  metrics.set("us_world", t1 - t0)

  -- Update shell-trajectory danger map
  danger.update(info, now)

  local t2 = clock_us()
  metrics.set("us_danger", t2 - t1)

  -- Process sound events into combat heat map
  hearing.update(info, now)

  -- Rebuild spatial threat grid (pill + tank layers, O(1) lookup for consumers)
  threat.update(state, world, info)

  -- Populate C pathfinder danger grid (simplified: no tree cover / wall shielding)
  cpf.clear_danger()
  for _, pm in pairs(world.pills) do
    if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
      local anger = pm.anger or 0
      cpf.stamp_pill(pm.mx, pm.my, C.PILL_RANGE_MAP, C.PILL_DANGER_BASE, C.PILL_DANGER_ANGER * anger * anger)
    end
  end

  -- Populate influence grid (friendly = positive, hostile = negative)
  cpf.clear_influence()
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" then
      cpf.stamp_influence(b.mx, b.my, C.BASE_INFLUENCE_RADIUS, C.BASE_INFLUENCE_STRENGTH)
    elseif b.owner == "hostile" then
      cpf.stamp_influence(b.mx, b.my, C.BASE_INFLUENCE_RADIUS, -C.BASE_INFLUENCE_STRENGTH)
    end
  end
  for _, pm in pairs(world.pills) do
    if pm.health > 0 then
      if pm.owner == "friendly" then
        cpf.stamp_influence(pm.mx, pm.my, C.PILL_INFLUENCE_RADIUS, C.PILL_INFLUENCE_STRENGTH)
      elseif pm.owner == "hostile" then
        cpf.stamp_influence(pm.mx, pm.my, C.PILL_INFLUENCE_RADIUS, -C.PILL_INFLUENCE_STRENGTH)
      end
    end
  end

  local t3 = clock_us()
  metrics.set("us_threat", t3 - t2)

  -- Build shared perception snapshot (before goal selection / builder / steering)
  percept.update(state, world, info)

  -- Classify game phase (reads state.perc, must run after percept.update)
  strategy.update(state, world, info)

  local t4 = clock_us()
  metrics.set("us_percept", t4 - t3)

  -- Outgoing message -- only one per tick
  local send_msg = nil
  local msg_dest = 0

  if state.send_open_msg then
    send_msg = state.paused and C.BRAIN_NAME .. " loaded (PAUSED — use 'start' to begin)."
                             or C.BRAIN_NAME .. " loaded."
    msg_dest = 1 << state.player_number
    state.send_open_msg = false
  end

  -- Process incoming newswire message
  if info.message and info.message.text then
    local cmd = cmds.parse(info.message.text)
    if cmd then
      print(string.format(TAG .. " t=%d RECV from player %d: '%s'",
            now, info.message.sender, info.message.text))
      log.event("cmd_recv", info.message.text)
      local reply = cmds.execute(cmd, state, world)
      if reply and not send_msg then
        send_msg = reply
        msg_dest = 1 << state.player_number
      end
    end
  end

  -- Paused: accept commands but do nothing else
  if state.paused then
    log.log_tick(state, info, state.goal, 0, 0, nil)
    return {
      holdkeys    = 0,
      tapkeys     = 0,
      build       = nil,
      wantallies  = info.allies,
      messagedest = msg_dest,
      sendmessage = send_msg,
    }
  end

  -- Pick up deferred arrival reply
  if state.command_reply and not send_msg then
    send_msg = state.command_reply
    msg_dest = 1 << state.player_number
    state.command_reply = nil
  end

  -- Respawn handling
  if info.newtank then
    state.stuck_for = 0
    state.pf.status = "idle"
    state.goal.kind = "none"
    print(string.format(TAG .. " t=%d RESPAWN at (%d,%d)",
          now, info.tankx >> 8, info.tanky >> 8))
    log.event("respawn", string.format("%d,%d", info.tankx >> 8, info.tanky >> 8))
  end

  -- Stuck detection
  local cur_mx = info.tankx >> 8
  local cur_my = info.tanky >> 8

  -- When attacking a pill in engage/ws_ substates we are intentionally
  -- stationary; don't count that as being stuck.
  local ws_stationary = { engage=true, ws_prebuild=true, ws_prewait=true, ws_advance=true, ws_engage=true, ws_retreat=true, ws_rebuild=true }
  local pp_stationary = { dispatch=true, wait_place=true, prewait=true, advance=true, shield_engage=true, engage=true, reposition=true, finish=true, select_pill=true }
  local tank_combat_stationary = { engage=true }
  local attack_at_standoff = (state.goal.kind == "attack_pill"
    and ws_stationary[state.goal.substate or ""])
    or (state.goal.kind == "pill_place"
    and pp_stationary[state.goal.substate or ""])
    or (state.goal.kind == "attack_tank"
    and tank_combat_stationary[state.goal.substate or ""])

  if cur_mx == state.last_mx and cur_my == state.last_my
     and state.goal.kind ~= "none"
     and not attack_at_standoff
     and not state.wall_clearing then
    state.stuck_for = state.stuck_for + 1
    if state.stuck_for > 150 then  -- ~3 s
      if state.goal.kind == "attack_pill" or state.goal.kind == "pill_place" then
        -- Couldn't reach the attack position: flee away from the pill
        local dx  = cur_mx - state.goal.mx
        local dy  = cur_my - state.goal.my
        local len = math.max(1, math.sqrt(dx * dx + dy * dy))
        local fmx = U.mclamp(math.floor(cur_mx + dx / len * C.FLEE_PILL_DIST + 0.5))
        local fmy = U.mclamp(math.floor(cur_my + dy / len * C.FLEE_PILL_DIST + 0.5))
        print(string.format(
          TAG .. " t=%d STUCK %s pill at (%d,%d) -- fleeing to (%d,%d)",
          now, state.goal.kind, state.goal.mx, state.goal.my, fmx, fmy))
        log.event("stuck", string.format("%s@%d,%d->flee(%d,%d)",
          state.goal.kind, state.goal.mx, state.goal.my, fmx, fmy))
        state.goal = { kind = "flee_pill", mx = fmx, my = fmy,
                       wx = U.m2w(fmx), wy = U.m2w(fmy) }
        state.pf.status = "idle"
        if state.command_goal then
          state.command_reply = string.format(
            C.BRAIN_NAME .. ": STUCK attacking pill #%d -- fleeing",
            state.command_goal.id or 0)
          state.command_goal = nil
          print(TAG .. " CMD: cancelled due to stuck (attack_pill)")
        end
      else
        local bk = U.mkey(state.goal.mx, state.goal.my)
        state.blocked[bk] = now + 600
        print(string.format(
          TAG .. " t=%d STUCK at (%d,%d) goal=%s dest=(%d,%d) -- blocking for 600t",
          now, cur_mx, cur_my, state.goal.kind, state.goal.mx, state.goal.my))
        log.event("stuck", string.format("%s@%d,%d", state.goal.kind, state.goal.mx, state.goal.my))
        if state.command_goal then
          state.command_reply = string.format(
            C.BRAIN_NAME .. ": STUCK trying to reach %s #%d at (%d,%d) -- giving up",
            state.command_goal.kind, state.command_goal.id,
            state.command_goal.mx, state.command_goal.my)
          state.command_goal = nil
          print(TAG .. " CMD: cancelled due to stuck")
        end
        state.goal.kind = "none"
        state.pf.status = "idle"
      end
      state.stuck_for = 0
    end
  else
    state.stuck_for = 0
  end
  state.last_mx = cur_mx
  state.last_my = cur_my

  -- Expire stale blocked entries
  if now % 200 == 0 then
    for k, until_t in pairs(state.blocked) do
      if now >= until_t then state.blocked[k] = nil end
    end
  end

  -- Water escape emergency
  local t_goal0 = clock_us()   -- initialized here; updated below if goal section runs
  local t_goal1 = nil
  local tank_tt  = U.ttype(cur_mx, cur_my)
  local in_water = info.inboat == 0
                   and (tank_tt == C.T_RIVER or tank_tt == C.T_DEEPSEA)

  -- Detect boat loss: invalidate pathfinder and pool cache so escape_water
  -- and goal replan use on-foot costs instead of stale in-boat estimates.
  local lost_boat = (state.was_in_boat ~= 0) and (info.inboat == 0)
  state.was_in_boat = info.inboat
  if lost_boat then
    state.pool_cache = nil
    state.pf.status = "idle"
    state.pf.next_mx = -1
    state.pf.next_my = -1
    -- Don't nuke the current goal — the tank may be very close to its
    -- target and can still reach it on foot.  Just invalidate the
    -- pathfinder and pool cache so they recalculate with on-foot costs.
    -- The periodic replan will naturally re-evaluate if a better goal
    -- exists with on-foot costs.
    print(string.format(TAG .. " t=%d BOAT LOST at (%d,%d) goal=%s -- recalc on-foot",
          now, cur_mx, cur_my, state.goal.kind))
    log.event("boat_lost", string.format("%d,%d goal=%s", cur_mx, cur_my, state.goal.kind))
  end

  -- If A* is actively computing or has a valid waypoint, don't override with
  -- escape_water. A* restarts every tile move (tank_moved), so it's often
  -- "running" with next=-1 for 1-3 ticks — we must wait for it to finish
  -- before deciding to escape, or we get an infinite escape loop.
  local pf_routing = state.pf.status == "running"
                  or (state.pf.status == "done" and state.pf.next_mx >= 0)

  if in_water then
    -- Always try to build a road under ourselves when drowning in river
    if info.man_status == C.LGM_INTANK and tank_tt == C.T_RIVER
       and info.trees >= C.ROAD_RIVER_COST + C.TREE_RESERVE then
      state.water_build = { x = cur_mx, y = cur_my }
      if not state.water_build_logged then
        state.water_build_logged = true
        print(string.format(TAG .. " t=%d WATER BUILD ROAD at (%d,%d) trees=%d",
              now, cur_mx, cur_my, info.trees))
      end
    else
      state.water_build = nil
    end

    -- Only override goal with escape_water if A* isn't actively routing us through
    if not pf_routing then
      local dry_x, dry_y = PF.find_dry_land(cur_mx, cur_my)
      if dry_x then
        if state.goal.kind ~= "escape_water"
           or state.goal.mx ~= dry_x or state.goal.my ~= dry_y then
          state.goal = { kind = "escape_water", mx = dry_x, my = dry_y,
                         wx = U.m2w(dry_x), wy = U.m2w(dry_y) }
          print(string.format(TAG .. " t=%d WATER ESCAPE to (%d,%d)",
                now, dry_x, dry_y))
          log.event("water_escape", string.format("%d,%d tt=%d", dry_x, dry_y, tank_tt))
        end
      end
    end
  else
    state.water_build = nil
    state.water_build_logged = false

    -- Build road under self when on slow terrain (swamp/rubble/crater).
    -- ROI: swamp traversal ~85 ticks vs ~44 ticks with road built → saves ~40 ticks/tile.
    -- LGM barely leaves the tank's tile so exposure is short; use LGM_DANGER_MED.
    -- Skip when in a boat: the tank doesn't need roads on water, and dispatching
    -- the LGM triggers pacing that slows the tank below disembark speed.
    local slow_tt = info.inboat == 0 and C.ROAD_BUILD_TERRAIN[tank_tt] or nil
    if slow_tt and info.man_status == C.LGM_INTANK
       and info.trees >= slow_tt + C.TREE_RESERVE then
      state.slow_build = { x = cur_mx, y = cur_my }
    else
      state.slow_build = nil
    end

    -- Anti-tank opportunistic pill drop: if carrying a pill and an enemy
    -- tank is close, place the pill between us and the threat.
    if C.ANTITANK_DROP_ENABLED
       and (info.carried_pills or 0) >= 1
       and info.man_status == C.LGM_INTANK
       and info.inboat == 0
       and (not state.antitank_drop_cooldown or now >= state.antitank_drop_cooldown)
       and state.goal.kind ~= "pill_place"
       and state.goal.kind ~= "attack_pill"
       and state.goal.kind ~= "bpc_pill" then
      local perc = state.perc
      local et = perc and perc.nearest_hostile_tank
      if et and et.dist <= C.ANTITANK_DROP_RANGE then
        local mid_mx = U.mclamp(math.floor((cur_mx + et.mx) / 2 + 0.5))
        local mid_my = U.mclamp(math.floor((cur_my + et.my) / 2 + 0.5))
        if mid_mx ~= cur_mx or mid_my ~= cur_my then  -- don't place on self
          if U.is_placeable(mid_mx, mid_my, world) then
            state.goal = {
              kind = "place_pill_strategic", mx = mid_mx, my = mid_my,
              wx = U.m2w(mid_mx), wy = U.m2w(mid_my),
              antitank = true,
            }
            state.pf.status = "idle"
            state.antitank_drop_cooldown = now + C.ANTITANK_DROP_COOLDOWN
            print(string.format(TAG .. " t=%d ANTITANK DROP at (%d,%d) enemy@(%d,%d) dist=%d",
                  now, mid_mx, mid_my, et.mx, et.my, et.dist))
            log.event("antitank_drop", string.format("at(%d,%d) enemy(%d,%d)", mid_mx, mid_my, et.mx, et.my))
          end
        end
      end
    end
    -- Expire antitank drop cooldown
    if state.antitank_drop_cooldown and now >= state.antitank_drop_cooldown then
      state.antitank_drop_cooldown = nil
    end

    -- Goal invalidation: check whether the world state still supports
    -- the current goal.  If not, force an immediate replan rather than
    -- waiting for the periodic 25-tick cycle.  In aiFull mode the world
    -- data is fresh every tick so this reacts instantly.
    local goal_valid = true
    local gk = state.goal.kind
    local gmx, gmy = state.goal.mx, state.goal.my
    if gk == "capture_base" then
      local b = W.base_at(world, gmx, gmy)
      -- Accept neutral (normal capture) and hostile (weakened base drive-over capture)
      if not b or (b.owner ~= "neutral" and b.owner ~= "hostile") then
        goal_valid = false
      end
    elseif gk == "attack_base" then
      local b = W.base_at(world, gmx, gmy)
      if not b then
        goal_valid = false
      elseif b.owner == "neutral" then
        -- Base armour depleted — it went neutral, now just drive over to capture
        print(string.format(TAG .. " t=%d BASE CAPTURABLE: (%d,%d) owner=%s — switching to capture_base",
              now, gmx, gmy, b.owner))
        log.event("base_capturable", string.format("(%d,%d) owner=%s", gmx, gmy, b.owner))
        state.goal.kind = "capture_base"
        state.pf.status = "idle"
      elseif b.owner ~= "hostile" then
        -- Base became friendly (someone else captured it)
        goal_valid = false
      end
    elseif gk == "capture_pill" then
      local p = W.pill_at(world, gmx, gmy)
      if not p or p.owner == "hostile" or p.health > 0 then goal_valid = false end
    elseif gk == "attack_pill" and not state.capture_objective then
      -- Autonomous attack (not cp command): invalid if pill died or changed side
      local p = W.pill_at(world, gmx, gmy)
      if not p or p.owner == "friendly" or p.health == 0 then goal_valid = false end
    elseif gk == "pill_place" and not state.capture_objective then
      -- Pill placement: invalid if target died or became friendly
      local p = W.pill_at(world, gmx, gmy)
      if not p or p.owner == "friendly" or p.health == 0 then goal_valid = false end
    elseif gk == "defend_pill" then
      local p = W.pill_at(world, gmx, gmy)
      if not p or p.owner ~= "friendly" or p.health == 0 then goal_valid = false end
      -- Also invalidate if attack has stopped
      if p and not p.under_attack then goal_valid = false end
    elseif gk == "repair_pill" then
      local p = W.pill_at(world, gmx, gmy)
      if not p or p.owner ~= "friendly" or p.health >= C.PILLS_MAX_HEALTH then goal_valid = false end
    elseif gk == "place_pill_strategic" then
      -- Invalid if we no longer carry a pill, or pill was placed at target
      if (info.carried_pills or 0) == 0 and info.man_status == C.LGM_INTANK then
        goal_valid = false
      else
        local p = W.pill_at(world, gmx, gmy)
        if p and p.owner == "friendly" and p.health > 0 then goal_valid = false end
      end
    elseif gk == "attack_tank" then
      -- Invalid if no enemy tanks visible (target escaped) or we're too weak
      local has_target = false
      if state.perc and state.perc.enemy_tanks then
        for _, et in ipairs(state.perc.enemy_tanks) do
          if et.dist <= C.TANK_COMBAT_MAX_RANGE then
            has_target = true
            break
          end
        end
      end
      if not has_target then goal_valid = false end
      -- Also disengage if outgunned
      if info.armour <= C.TANK_COMBAT_FLEE_ARMOUR
         or info.shells <= C.TANK_COMBAT_FLEE_SHELLS then
        goal_valid = false
      end
    elseif gk == "refuel_at_base" or gk == "flee_to_base" then
      local b = W.base_at(world, gmx, gmy)
      if not b or (b.owner ~= "friendly" and b.owner ~= "neutral") then goal_valid = false end
    end
    if not goal_valid then
      print(string.format(TAG .. " t=%d GOAL INVALID: %s at (%d,%d) — replanning",
            now, gk, gmx, gmy))
      log.event("goal_invalid", string.format("%s@%d,%d", gk, gmx, gmy))
      state.goal.kind = "none"
      state.pf.status = "idle"
    end

    t_goal0 = clock_us()

    -- Rolling candidate evaluation: 2 A* cost_to calls per tick
    goals.update_pool_cache(state, world, info)

    -- Goal selection (not in water)
    local urgent_replan = state.goal.kind == "none"
    local replan = urgent_replan
               or (not state.command_goal and (now + state.replan_offset) % C.GOAL_REPLAN_INTERVAL == 0)

    if replan then
      if urgent_replan then
        -- Detect "about to refuel": we're on a friendly base that can supply us.
        -- In that case skip the expensive fill_pool_cache — Override 2 will fire
        -- without needing pool data, and the incremental queue will build options
        -- during the refueling downtime.
        local skip_fill = false
        if info.base then
          local bmx, bmy = info.base.x, info.base.y
          if bmx == cur_mx and bmy == cur_my
             and (info.armour < C.TANK_FULL_ARMOUR or info.shells < C.TANK_FULL_SHELLS) then
            local bs = state.perc and state.perc.base_supply
            if bs and (bs.armour > 0 or bs.shells > 0) then
              skip_fill = true
            end
          end
        end
        if skip_fill then
          -- Finalize whatever partial cache we have; queue will fill the rest
          goals.finalize_pools(state, world, info)
        else
          -- Truly urgent (respawn, boat loss, etc.): evaluate all pools now
          goals.fill_pool_cache(state, world, info)
        end
      else
        -- Normal replan: finalize incremental results + run cheap evaluators
        goals.finalize_pools(state, world, info)
      end
      -- Kick off next cycle's queue immediately so it has ~49 ticks to process
      goals.build_eval_queue(state, world, info)
      metrics.inc("goal_replan")
      local new_goal = goals.pick_goal(state, world, info)
      if new_goal.kind ~= state.goal.kind
         or new_goal.mx ~= state.goal.mx
         or new_goal.my ~= state.goal.my then
        local old_kind = state.goal.kind
        local old_id   = state.goal.target_id
        state.goal = new_goal
        state.goal_set_tick = now
        state.pf.status = "idle"
        state.pf_fail_logged = false
        local rp = state._real_print or print
        local old_label = old_id and string.format("%s(%d)", old_kind, old_id) or old_kind
        local new_label = new_goal.target_id and string.format("%s(%d)", new_goal.kind, new_goal.target_id) or new_goal.kind
        rp(string.format(TAG .. " t=%d GOAL CHANGE: %s -> %s dest=(%d,%d) sub=%s cpill=%d arm=%d sh=%d tr=%d name=%s",
              now, old_label, new_label, new_goal.mx, new_goal.my,
              tostring(new_goal.substate or "-"),
              info.carried_pills or 0, info.armour, info.shells, info.trees,
              state.player_name))
        log.event("goal", string.format("%s->%s@%d,%d", old_label, new_label, new_goal.mx, new_goal.my))
      elseif new_goal.kind == "attack_pill" and state.goal.substate then
        -- Same pill target: preserve substate and timing fields.
        -- During engage/ws_ substates: keep the OLD standoff and wall positions.
        -- Adopting new positions while fighting triggers spurious repositioning.
        new_goal.substate        = state.goal.substate
        new_goal.engage_tick     = state.goal.engage_tick
        new_goal.reposition_tick = state.goal.reposition_tick
        new_goal.first_hit_tick  = state.goal.first_hit_tick
        new_goal.last_armour     = state.goal.last_armour
        -- Preserve standoff/wall positions during active engage or ws_ substates
        local active_sub = { engage=true, ws_prebuild=true, ws_prewait=true, ws_advance=true, ws_engage=true, ws_retreat=true, ws_rebuild=true }
        if active_sub[state.goal.substate or ""] then
          if state.goal.standoff_mx then
            new_goal.standoff_mx = state.goal.standoff_mx
            new_goal.standoff_my = state.goal.standoff_my
          end
          if state.goal.wall_mx then
            new_goal.wall_shield = state.goal.wall_shield
            new_goal.wall_mx     = state.goal.wall_mx
            new_goal.wall_my     = state.goal.wall_my
          end
          if state.goal.prebuild_mx then
            new_goal.prebuild_mx = state.goal.prebuild_mx
            new_goal.prebuild_my = state.goal.prebuild_my
          end
        end
        -- Preserve wall-shield timing fields
        new_goal.ws_build_tick   = state.goal.ws_build_tick
        new_goal.ws_wait_tick    = state.goal.ws_wait_tick
        new_goal.ws_rebuild_tick = state.goal.ws_rebuild_tick
        new_goal.ws_retreat_tick = state.goal.ws_retreat_tick
        new_goal.lgm_return_tick = state.goal.lgm_return_tick
        state.goal = new_goal
      elseif new_goal.kind == "pill_place" and state.goal.substate then
        -- Same pill_place target: preserve substate and all timing/position fields
        new_goal.substate       = state.goal.substate
        new_goal.source_mx      = state.goal.source_mx
        new_goal.source_my      = state.goal.source_my
        new_goal.source_id      = state.goal.source_id
        new_goal.place_mx       = state.goal.place_mx
        new_goal.place_my       = state.goal.place_my
        new_goal.deploy_mx      = state.goal.deploy_mx
        new_goal.deploy_my      = state.goal.deploy_my
        new_goal.placed_mx      = state.goal.placed_mx
        new_goal.placed_my      = state.goal.placed_my
        -- Shield engage fields (shared with wall-shield pipeline)
        new_goal.standoff_mx    = state.goal.standoff_mx
        new_goal.standoff_my    = state.goal.standoff_my
        new_goal.shield_mx      = state.goal.shield_mx
        new_goal.shield_my      = state.goal.shield_my
        new_goal.shield_type    = state.goal.shield_type
        new_goal.lgm_return_tick = state.goal.lgm_return_tick
        new_goal.reposition_tick = state.goal.reposition_tick
        -- Timing fields
        new_goal.dispatch_tick  = state.goal.dispatch_tick
        new_goal.wait_tick      = state.goal.wait_tick
        new_goal.engage_tick    = state.goal.engage_tick
        new_goal.collect_tick   = state.goal.collect_tick
        new_goal.finish_tick    = state.goal.finish_tick
        new_goal.first_hit_tick = state.goal.first_hit_tick
        new_goal.last_armour    = state.goal.last_armour
        state.goal = new_goal
      elseif new_goal.kind == "bpc_pill" and state.goal.substate then
        -- Same bpc target: preserve substate and stand_shoot/curve state
        new_goal.substate       = state.goal.substate
        new_goal.engage_tick    = state.goal.engage_tick
        new_goal.engage_armour  = state.goal.engage_armour
        new_goal.hits_taken     = state.goal.hits_taken
        new_goal.curve_tick     = state.goal.curve_tick
        new_goal.curve_dir      = state.goal.curve_dir
        new_goal.rush_tick      = state.goal.rush_tick
        if state.goal.standoff_mx then
          new_goal.standoff_mx = state.goal.standoff_mx
          new_goal.standoff_my = state.goal.standoff_my
        end
        state.goal = new_goal
      end
    end

    -- Per-tick attack substate transitions (approach → engage → reposition)
    attack.update_attack_substate(state.goal, state, world, info)
    bpc.update(state.goal, state, world, info)
    attack.update_pill_place_substate(state.goal, state, world, info)

    -- Auto-expire: A* says we arrived or path impossible
    if state.pf.status == "failed" and now % 10 == 0 then
      print(string.format(TAG .. " t=%d DEBUG pf=failed goal=%s dest=(%d,%d) pf_fail_logged=%s",
            now, state.goal.kind, state.goal.mx, state.goal.my, tostring(state.pf_fail_logged)))
    end
    if state.pf.status == "failed" then
      if state.goal.kind == "explore" then
        local gk = U.mkey(state.goal.mx, state.goal.my)
        state.visited[gk] = true
        print(string.format(TAG .. " t=%d ARRIVED/FAILED explore (%d,%d) -- marking visited",
              now, state.goal.mx, state.goal.my))
        state.goal.kind = "none"
        state.pf.status = "idle"
      elseif state.goal.kind == "attack_pill" or state.goal.kind == "pill_place" then
        -- Can't reach attack position: flee away from the pill.
        -- No pf_fail_logged guard here — we always want to flee, not sit stuck.
        local dx  = cur_mx - state.goal.mx
        local dy  = cur_my - state.goal.my
        local len = math.max(1, math.sqrt(dx * dx + dy * dy))
        local fmx = U.mclamp(math.floor(cur_mx + dx / len * C.FLEE_PILL_DIST + 0.5))
        local fmy = U.mclamp(math.floor(cur_my + dy / len * C.FLEE_PILL_DIST + 0.5))
        print(string.format(
          TAG .. " t=%d PF FAILED for %s (%d,%d) -- fleeing to (%d,%d)",
          now, state.goal.kind, state.goal.mx, state.goal.my, fmx, fmy))
        log.event("pf_failed", string.format("%s@%d,%d->flee(%d,%d)",
          state.goal.kind, state.goal.mx, state.goal.my, fmx, fmy))
        state.goal = { kind = "flee_pill", mx = fmx, my = fmy,
                       wx = U.m2w(fmx), wy = U.m2w(fmy) }
        state.pf.status = "idle"
        state.pf_fail_logged = false
      elseif state.goal.kind ~= "none" then
        if not state.pf_fail_logged then
          state.pf_fail_logged = true
          print(string.format(TAG .. " t=%d PF FAILED for %s dest=(%d,%d) -- tank at (%d,%d), blocking",
                now, state.goal.kind, state.goal.mx, state.goal.my, cur_mx, cur_my))
          log.event("pf_failed", string.format("%s@%d,%d", state.goal.kind, state.goal.mx, state.goal.my))
        end
        -- Block this destination so goal selection picks something else
        local bk = U.mkey(state.goal.mx, state.goal.my)
        state.blocked[bk] = now + 600
        state.goal.kind = "none"
        state.pf.status = "idle"
      end
    end
  end

  t_goal1 = clock_us()
  metrics.set("us_goals", t_goal1 - t_goal0)

  -- Goal lookahead: when close to a capture goal, pre-compute the next
  -- goal.  If the next goal isn't "stay here and refuel", pass it to
  -- steering so the tank drives through the capture point at speed
  -- instead of braking to a stop and then re-accelerating.
  state.next_goal = nil
  -- Drive-through lookahead: pre-compute next goal so the tank maintains
  -- momentum through the current destination instead of braking to a stop.
  -- capture_base/capture_pill: captured on drive-over, no need to stop.
  -- explore: tile marked visited on entry (expl.update), drive through.
  -- flee_pill: escaping, keep momentum toward next objective.
  local lookahead_kinds = {
    capture_base = true, capture_pill = true,
    explore = true, flee_pill = true,
  }
  if lookahead_kinds[state.goal.kind] and not state.command_goal then
    local gdist = U.wdist(info.tankx, info.tanky, state.goal.wx, state.goal.wy)
    if gdist < C.LOOKAHEAD_DIST and gdist > 0 then
      -- Temporarily pretend we've arrived: mark goal "none" and block the
      -- current destination so pick_goal selects the NEXT thing (not the
      -- same base/pill we're about to capture), then restore everything.
      local saved_goal = state.goal
      state.goal = { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
      local block_key = U.mkey(saved_goal.mx, saved_goal.my)
      local had_block = state.blocked[block_key]
      state.blocked[block_key] = now + 9999
      local saved_gs = goals.save_goal_state()
      local peek = goals.pick_goal(state, world, info)
      goals.restore_goal_state(saved_gs)
      if had_block then state.blocked[block_key] = had_block
      else state.blocked[block_key] = nil end
      state.goal = saved_goal
      -- Use lookahead only if the next goal is elsewhere (not refueling here)
      if peek and peek.kind ~= "none"
         and not (peek.kind == "refuel_at_base"
                  and peek.mx == saved_goal.mx and peek.my == saved_goal.my) then
        state.next_goal = peek
        log.reason("goal", {
          pick = "lookahead", next_kind = peek.kind,
          next_mx = peek.mx, next_my = peek.my,
          cur_dist = gdist,
        })
      end
    end
  end

  -- Steering
  local t_steer0 = clock_us()
  local keys, taps = steer.steer(state, world, info, state.goal)
  local t_steer1 = clock_us()
  metrics.set("us_steer", t_steer1 - t_steer0)

  -- Opportunistic tank shot: fire at enemy tanks while doing other things.
  -- Only if not already in tank combat and not shooting at something else.
  if state.goal.kind ~= "attack_tank"
     and (keys & KEY_SHOOT) == 0 and (taps & KEY_SHOOT) == 0
     and info.shells > C.SHELL_RESERVE
     and info.inboat == 0 then
    local perc = state.perc
    if perc and perc.enemy_tanks then
      for _, et in ipairs(perc.enemy_tanks) do
        if et.dist <= C.TANK_COMBAT_OPPORTUNISTIC_RANGE then
          local aim_dir = U.aim_at(info.tankx, info.tanky,
                                   U.m2w(et.mx), U.m2w(et.my))
          local aim_corr = U.adiff(info.direction, aim_dir)
          if math.abs(aim_corr) <= C.TANK_COMBAT_OPPORTUNISTIC_AIM then
            taps = taps | KEY_SHOOT
            log.event("opportunistic_tank_shot",
              string.format("at(%d,%d) dist=%d aim=%.0f",
                            et.mx, et.my, et.dist, aim_corr))
            break
          end
        end
      end
    end
  end

  -- Navigation debug -- verbose prints for command goals
  if state.command_goal then
    local pf = state.pf
    local cg = state.command_goal
    local dist = U.mdist(cur_mx, cur_my, cg.mx, cg.my)

    if now % 50 == 0 then
      local step = pf.next_mx >= 0
                   and string.format("(%d,%d)", pf.next_mx, pf.next_my)
                   or "nil"
      local kstr = ""
      if keys & KEY_FASTER    ~= 0 then kstr = kstr .. "FAST " end
      if keys & KEY_SLOWER    ~= 0 then kstr = kstr .. "SLOW " end
      if keys & KEY_TURNLEFT  ~= 0 then kstr = kstr .. "LEFT " end
      if keys & KEY_TURNRIGHT ~= 0 then kstr = kstr .. "RIGHT " end
      if keys & KEY_SHOOT     ~= 0 then kstr = kstr .. "SHOOT " end
      if taps & KEY_SHOOT     ~= 0 then kstr = kstr .. "tap-SHOOT " end
      if taps & KEY_TURNLEFT  ~= 0 then kstr = kstr .. "tap-L " end
      if taps & KEY_TURNRIGHT ~= 0 then kstr = kstr .. "tap-R " end
      if kstr == "" then kstr = "none" end
      print(string.format(
        TAG .. " NAV t=%d  %s #%d  tank=(%d,%d) dest=(%d,%d) dist=%d  pf=%s next=%s  dir=%.0f spd=%.0f  keys=[%s]  stuck=%d  boat=%s",
        now, cg.kind, cg.id, cur_mx, cur_my, cg.mx, cg.my, dist,
        pf.status, step, info.direction, info.speed,
        kstr, state.stuck_for, tostring(info.inboat)))
    end

    if now % 250 == 0 and not send_msg then
      send_msg = string.format(C.BRAIN_NAME .. ": en route to %s #%d -- %d sq away, pf=%s",
        cg.kind, cg.id, dist, pf.status)
      msg_dest = 1 << state.player_number
    end
  else
    if now % 250 == 0 then
      local pf = state.pf
      local step = pf.next_mx >= 0
                   and string.format("(%d,%d)", pf.next_mx, pf.next_my)
                   or "nil"
      local kstr = ""
      if keys & KEY_FASTER    ~= 0 then kstr = kstr .. "F" end
      if keys & KEY_SLOWER    ~= 0 then kstr = kstr .. "S" end
      if keys & KEY_TURNLEFT  ~= 0 then kstr = kstr .. "L" end
      if keys & KEY_TURNRIGHT ~= 0 then kstr = kstr .. "R" end
      if taps & KEY_SHOOT     ~= 0 then kstr = kstr .. "!" end
      if kstr == "" then kstr = "-" end
      local co_str = ""
      if state.capture_objective then
        local co = state.capture_objective
        local p  = world.pills[co.id]
        local hp = p and p.health or "?"
        co_str = string.format("  [cp#%d hp=%s]", co.id, tostring(hp))
      elseif state.base_capture_objective then
        local bco = state.base_capture_objective
        if bco.all then
          local bid = bco.id or "?"
          co_str = string.format("  [cb:all #%s]", tostring(bid))
        elseif bco.id then
          local b = world.bases[bco.id]
          local hp = b and b.health or "?"
          co_str = string.format("  [cb#%d hp=%s]", bco.id, tostring(hp))
        end
      end
      local sub_str = state.goal.substate and ("/" .. state.goal.substate) or ""
      print(string.format(
        TAG .. " t=%d pos=(%d,%d) dir=%.0f goal=%s%s dest=(%d,%d) pf=%s next=%s keys=%s spd=%.0f stuck=%d%s",
        now, cur_mx, cur_my, info.direction,
        state.goal.kind, sub_str, state.goal.mx, state.goal.my,
        pf.status, step, kstr, info.speed, state.stuck_for, co_str))
    end
  end

  -- Builder: set mode from current goal, then decide what to build/farm
  local t_build0 = clock_us()
  builder.set_mode(state, world, info, state.goal)
  local build_cmd = builder.decide(state, world, info, now)
  local t_build1 = clock_us()
  metrics.set("us_builder", t_build1 - t_build0)
  -- Track what kind of action was most recently dispatched so steering can
  -- decide whether to pace the tank while the LGM is moving.
  if build_cmd and info.man_status == C.LGM_INTANK then
    state.builder.last_action = build_cmd.action
  end

  -- Log this tick
  log.log_tick(state, info, state.goal, keys, taps, build_cmd)

  -- Total tick time and worst-case tracking
  local t_end = clock_us()
  local us_total = t_end - t0
  metrics.set("us_total", us_total)
  metrics.max("us_total", us_total)
  metrics.max("us_world", t1 - t0)
  metrics.max("us_danger", t2 - t1)
  metrics.max("us_threat", t3 - t2)
  metrics.max("us_percept", t4 - t3)
  if t_goal0 and t_goal1 then
    metrics.max("us_goals", t_goal1 - t_goal0)
  end
  if t_steer0 and t_steer1 then
    metrics.max("us_steer", t_steer1 - t_steer0)
  end
  if t_build0 and t_build1 then
    metrics.max("us_builder", t_build1 - t_build0)
  end

  -- Finish metrics for this tick
  metrics.finish_tick(now)

  -- Output
  return {
    holdkeys    = keys,
    tapkeys     = taps,
    build       = build_cmd,
    wantallies  = info.allies,
    messagedest = msg_dest,
    sendmessage = send_msg,
  }
end


-- =========================================================================
-- CLOSE
-- =========================================================================

function Brain.close(info)
  print(TAG .. " closed after " .. state.tick .. " ticks")
  log.dump_world(world)
  log.close()

  -- Restore real print so other code isn't affected
  if state._real_print then print = state._real_print end

  -- Release large tables for GC
  state.visited      = nil
  state.frontier     = nil
  state.frontier_set = nil
  state.pf           = nil
  state.blocked      = nil
  world.bases        = nil
  world.pills        = nil
end

return Brain
