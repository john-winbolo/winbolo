-- =========================================================================
-- NewAutopilot/init.lua — Brain entry point (open/think/close/settings)
-- =========================================================================

-- Route all require() calls through the stripped opt/ directory when not
-- running under BrainTest.  opt/ files are identical to source but with
-- print2, viz.*, overlay_* and io.open calls removed for zero overhead.
-- BRAIN_DIR and BRAIN_DEBUG_MODE are injected by the C host before this
-- file runs.
--
-- _OPT_INIT_LOADED guards against recursion: opt/init.lua has this same
-- block and would otherwise redirect back to itself indefinitely.

local C       = require("constants")
local TAG     = "[" .. C.BRAIN_NAME .. "]"
local U       = require("util")
local dbg     = require("debugger")
local heap    = require("heap")
local W       = require("world")
local expl    = require("exploration")
local PF      = require("pathfinder")
local cpf     = require("cpathfinder")
local wsim    = require("cworldsim")
local cmds    = require("commands")
local goals   = require("goals")
local attack  = require("attack")
local shield  = require("attack_shield")
local steer   = require("steering")
-- local bpc  = require("bpc")  -- removed: unified into attack_pill
local log     = require("logger")
local danger  = require("danger")
local builder = require("builder")
local metrics = require("metrics")
local changes = require("changes")
local percept  = require("perception")
local strategy = require("strategy")
local comms    = require("comms")
local threat   = require("threat")
local hearing  = require("hearing")
local print2   = require("print2")
local opt      = require("optimize")
local shot_tracker = require("shot_tracker")
local viz      = require("viz")

local Brain = {}

-- Manual control state (must be before Brain.think so it captures the upvalue)
local manual_active = false
local manual_keys = 0

local AUTOSTART = true
local ENABLE_LOGGING = false

-- Hoisted lookup tables (don't realloc every tick of every Brain.think).
-- Used by stuck detection / urgent-replan gating downstream — file-scope
-- so the loop bodies just do membership checks.
local ATTACK_STATIONARY_SUBS = {
  plan_position=true, position=true, aim=true, approach=true, build_walls=true,
  in_range_position=true, in_range_aim_pre=true, in_range_aim=true,
  in_range_aim_finetune=true, shoot_pill=true, engage=true, curve_away=true,
  rush=true, disengage=true, gather_trees=true,
}
local PP_STATIONARY_SUBS = {
  dispatch=true, wait_place=true, prewait=true, advance=true,
  shield_engage=true, engage=true, reposition=true, finish=true,
  select_pill=true,
}
local TANK_COMBAT_STATIONARY_SUBS = {
  engage=true, close=true, disengage=true,
}
-- Substates during which a goal-change should preserve standoff/wall
-- state (so a re-target doesn't drop in-progress geometry).
local ACTIVE_SUBS = {
  plan_position=true, approach=true, build_walls=true,
  in_range_position=true, in_range_aim_pre=true, in_range_aim=true,
  in_range_aim_finetune=true, shoot_pill=true, charge=true, aim=true,
  detree=true, engage=true, swerve=true, post_engage=true, loiter=true,
  rush=true, ws_prebuild=true, ws_prewait=true, ws_advance=true,
  ws_engage=true, ws_retreat=true, ws_rebuild=true, gather_trees=true,
}

-- Default initial substate for each goal kind.
-- Enforced whenever a goal changes to prevent skipping positioning.
local INITIAL_SUBSTATE = {
  attack_pill = "plan_position",
  attack_tank = "close",
  attack_base = "approach",
}

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
function Brain.get_queue_status()
  return goals.get_queue_status(state)
end

function Brain.get_pool_breakdown_json()
  if not BRAIN_POOL_VIZ then
    return string.format(
      '{"phase":"%s","tick":%d,"replan_left":0,"bot":%d,"sections":[{"id":"off","label":"Pool viz","rows":[{"id":0,"mx":0,"my":0,"cost":0,"formula":"BRAIN_POOL_VIZ is off","stale":-1,"active":false,"imminent":false,"reject":null}]}]}',
      (state and state.phase) or "?", (state and state.tick) or 0, (state and state.player_number) or 0)
  end
  local ok, result = pcall(goals.get_pool_breakdown_json, state)
  if ok then return result end
  io.stderr:write("BRAIN ERROR get_pool_breakdown_json: " .. tostring(result) .. "\n")
  return string.format(
    '{"phase":"error","tick":%d,"replan_left":0,"sections":[]}',
    state.tick or 0)
end

function Brain.get_strategic_place_heatmap()
  if not state._last_info then return nil end
  return goals.get_strategic_place_heatmap(state, world, state._last_info)
end

-- Shot-sim POI getters. Each returns (wx, wy) when the POI is
-- currently meaningful, or nil. Nothing here mutates state — these
-- are pure reads of whatever the brain happens to have decided this
-- tick. Polled by BrainTest's shot-sim panel.
local function _attack_pill_for_focus()
  if not state.goal then return nil end
  if state.goal.kind ~= "attack_pill" then return nil end
  local id = state.goal.target_id
  if not id then return nil end
  return world.pills and world.pills[id] or nil
end

function Brain.shotsim_focused_pill_take_wu()
  local p = _attack_pill_for_focus()
  if not p then return nil end
  -- Pill center in WU. Pills always sit at MAP_SQUARE_MIDDLE within
  -- their tile so this matches the engine's pill firing geometry.
  return (p.mx << 8) | 128, (p.my << 8) | 128
end

function Brain.shotsim_chosen_standoff_wu()
  if not state.goal or not state.goal._shield_scan then return nil end
  local s = state.goal._shield_scan.standoff
  if not s then return nil end
  -- Candidate cx/cy are tile-center floats (e.g. 12.5). Convert to
  -- WU by multiplying by 256 (1 tile = 256 WU).
  if not s.cx or not s.cy then return nil end
  return math.floor(s.cx * 256 + 0.5), math.floor(s.cy * 256 + 0.5)
end

function Brain.shotsim_shield_candidate_wu(i)
  if not state.goal or not state.goal._shield_scan then return nil end
  local cands = state.goal._shield_scan.candidates
  if not cands then return nil end
  -- attack_shield builds the array as [1] = standoff, [2..N+1] =
  -- ring candidates. The shot-sim button index is 1-based over the
  -- ring candidates only, so shift by +1 to skip the standoff slot
  -- (which already has its own dedicated POI button).
  local c = cands[i + 1]
  if not c or not c.cx or not c.cy then return nil end
  return math.floor(c.cx * 256 + 0.5), math.floor(c.cy * 256 + 0.5)
end

-- Chosen pill-take aim point (which corner of the pill the brain
-- decided to fire at — driven by attack_shield's best_aim_idx +
-- the AIM_OFFSETS_TILE_FIRE table). attack.lua writes this as
-- goal.aim_mx / aim_my in tile-fraction units; we just multiply by
-- 256 to get WU. Returns nil when no aim is set (pre-PPT, between
-- replans, or non-attack_pill goals).
function Brain.shotsim_chosen_aim_wu()
  if not state.goal then return nil end
  local ax, ay = state.goal.aim_mx, state.goal.aim_my
  if not ax or not ay then return nil end
  return math.floor(ax * 256 + 0.5), math.floor(ay * 256 + 0.5)
end

function Brain.get_debug_info()
  local g = state.goal or {}
  local info = {
    kind           = g.kind or "none",
    mx             = g.mx or 0,
    my             = g.my or 0,
    target_id      = g.target_id or -1,
    substate       = g.substate or "",
    pool           = state.last_goal_pool or {},
    replan_this_tick = state.replan_this_tick and true or false,
  }
  return info
end


-- =========================================================================
-- OPEN
-- =========================================================================

function Brain.open(info)
  opt.set_tick(0)
  opt("BEGIN Brain.open player=", info.player_number)
  local t_open0 = clock_us()
  -- Register every viz_id with the host's V dialog. No-op when
  -- braintest_viz_register isn't bound (e.g. running under WinBolo
  -- client); the brain still emits overlay commands but they're
  -- never displayed.
  -- Advertise text panels for BrainTest's P-toggled side window.
  -- Each call: panel name + Lua chunk that returns the body text
  -- to display. Host polls the active tab at ~10Hz. Same NULL-
  -- callback handling as braintest_viz_register: under WinBolo
  -- client this binding is a no-op so the calls are harmless.
  if braintest_panel_register then
    -- Pool breakdown gets the bespoke 2x5 grid renderer (panel type
    -- "pool_grid"). Brain emits structured JSON; host parses with
    -- cJSON and walks the section/row tree. The 4th-arg opts table
    -- requests its own SDL window with shortcut K (pooKs… alright,
    -- "K" for Killset — pick any free letter).
    if BRAIN_POOL_VIZ then
      braintest_panel_register("Pool breakdown", "pool_grid",
        "return brain.get_pool_breakdown_json()",
        { shortcut = "P" })
    end
    -- Queue status uses the generic text renderer (panel type "text").
    -- No shortcut → appears as a tab in the main P window.
    if Brain.get_queue_status then
      braintest_panel_register("Queue status", "text",
        "return brain.get_queue_status()")
    end
  end
  -- Shot-sim points of interest. Each lua_expr returns (wx, wy) when
  -- the POI is currently available, or nil. Polled every BrainTest
  -- frame against the followed bot — keep the bodies cheap.
  if braintest_shotsim_poi_register then
    -- The currently-attacked pill (focused pill take target).
    braintest_shotsim_poi_register("Focused pill take",
      "return brain.shotsim_focused_pill_take_wu()")
    -- Currently-chosen standoff (where the tank is heading for the
    -- shot — driven by attack_shield's scan winner during PPT).
    braintest_shotsim_poi_register("Chosen standoff",
      "return brain.shotsim_chosen_standoff_wu()")
    -- Chosen aim point — which corner of the focused pill we're
    -- firing at (center / TL / TR / BL / BR, picked by the shield
    -- scan). Useful as the shot-sim Target.
    braintest_shotsim_poi_register("Pill take aim",
      "return brain.shotsim_chosen_aim_wu()")
    -- Every individual shield-scan candidate (±7° at 0.5° steps
    -- around the standoff). Indexed 1..NUM_CANDIDATES; the brain's
    -- helper returns nil for indices the live scan doesn't cover so
    -- the panel buttons grey out outside the active scan.
    -- Hardcoded 28 to match attack_shield.NUM_CANDIDATES — bumping
    -- that constant means bumping this too. Worth the duplication
    -- since the registration list is read at brain.open() and we
    -- don't want to drag a runtime require here.
    for i = 1, 28 do
      braintest_shotsim_poi_register(
        string.format("Shield candidate %d", i),
        string.format("return brain.shotsim_shield_candidate_wu(%d)", i))
    end
  end
  local t_open_register = clock_us()
  opt(string.format("  viz/panel/POI register done %.2f ms", (t_open_register - t_open0) / 1000))

  -- Clear module-level caches from any previous game
  U.reset()
  PF.reset()
  cpf.configure()
  wsim.configure()
  danger.reset()
  threat.reset()
  hearing.reset()
  shot_tracker.reset()
  for i = #changes.terrain, 1, -1 do changes.terrain[i] = nil end
  local t_open_resets = clock_us()
  opt(string.format("  module resets done %.2f ms", (t_open_resets - t_open_register) / 1000))

  state.tick          = 0
  state.player_number = info.player_number
  state.player_name   = (info.player_names and info.player_names[info.player_number + 1]) or ""
  state.debug_log     = (state.player_name == "Bot 1" or info.player_number == 0)
  state.send_open_msg = true

  -- Startup mode: minimal first-tick work. Skips threat.update, long
  -- Dijkstra start, pool eval queue, perception, and goal selection
  -- until the short-range Dijkstra has finished its first lifetime
  -- (DIJKSTRA_SHORT_INTERVAL ticks). During those ticks we step the
  -- short slate and pick the nearest reachable neutral base as the
  -- initial goal. STARTUP_HOLD_TICKS already prevents driving during
  -- this window so we don't need to run steering either.
  state.startup_mode = true

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
  state.goal_cooldowns    = {}   -- abandoned goals: { [key] = expiry_tick }
  state.goal_history      = {}   -- circular buffer of last N picked goals (oscillation detection)

  -- Stuck detection
  state.last_mx   = -1
  state.last_my   = -1
  state.stuck_for = 0

  -- Boat state tracking (for detecting boat loss → forced replan)
  state.was_in_boat = false

  -- Blocked destinations: mkey -> expiry tick
  state.blocked = {}

  -- Banned approach angles per pill: { [pill_key] = { [deg_bucket] = expiry_tick } }
  -- Populated when an attack_pill approach times out without reaching
  -- the approach spot — bans that angle for 3 minutes so plan_position
  -- doesn't pick the same unreachable spot on the next replan.
  -- pill_key = my * 256 + mx (stationary while alive); deg_bucket = floor(deg/5)*5.
  state.banned_pill_angles = {}

  -- Exploration
  state.visited      = {}
  state.frontier     = heap.new()
  state.frontier_set = {}

  local t_open_state = clock_us()
  opt(string.format("  state init done %.2f ms", (t_open_state - t_open_resets) / 1000))

  -- World knowledge
  world.bases = {}
  world.pills = {}
  W.reset(world)
  local t_open_wreset = clock_us()
  opt(string.format("  W.reset done %.2f ms", (t_open_wreset - t_open_state) / 1000))
  W.update(world, info, 0)
  local t_open_wupd = clock_us()
  opt(string.format("  W.update (initial) done %.2f ms", (t_open_wupd - t_open_wreset) / 1000))

  -- Pre-build edge costs + pre-warm threat so tick-1 doesn't pay for them.
  -- pf->map is set by the C host before brain.open() is called.
  cpf.rebuild_edge_costs()
  opt(string.format("  rebuild_edge_costs done %.2f ms", (clock_us() - t_open_wupd) / 1000))
  local _t_open_danger = clock_us()
  danger.update(info, 0)
  opt(string.format("  danger.update done %.2f ms", (clock_us() - _t_open_danger) / 1000))
  local _t_open_threat = clock_us()
  threat.update(state, world, info)
  opt(string.format("  threat.update done %.2f ms", (clock_us() - _t_open_threat) / 1000))

  -- Strategy phase detection
  strategy.init(state)
  opt(string.format("  strategy.init done %.2f ms", (clock_us() - t_open_wupd) / 1000))

  -- Load precomputed shield stamp cache (C binary via na_shield.load).
  shield.load_stamp_bin()
  if na_shield then
    na_shield.configure_scan({
      T_BUILDING       = C.T_BUILDING,
      T_HALFBUILD      = C.T_HALFBUILD,
      NON_BUILDABLE    = { C.T_DEEPSEA, C.T_RIVER, C.T_SWAMP,
                           C.T_PILLBOX, C.T_REFBASE, C.T_BOAT },
      APPROACH_OFFSET  = C.ATTACK_APPROACH_OFFSET,
      BLOCKER_MIN_DIST = shield.BLOCKER_MIN_DIST,
      SCORE_PER_SLOT   = shield.SCORE_PER_SLOT,
      BUILT_BONUS      = shield.BUILT_BONUS,
      NEIGHBOR_BONUS   = shield.NEIGHBOR_BONUS,
    })
    na_shield.set_lgm_func(cpf.lgm_travel_ticks_map)
  end

  print(string.format(TAG .. " open: player=%d name='%s' map=%s ai_advantage=%s debug_log=%s",
        info.player_number, state.player_name, info.gameinfo.mapname,
        tostring(state.ai_advantage), tostring(state.debug_log)))

  if state.ai_advantage then
    print(TAG .. " AI Advantage enabled -- full world scan")
  else
    print(TAG .. " No AI Advantage -- world knowledge limited to view")
  end

  -- Auto-start logging with timestamped filenames so sessions never overwrite.
  -- Player 0 logs when _JSONL_LOGGER_ENABLED is set (controlled by the
  -- BrainTest debug modules panel — off by default since the file is huge
  -- and rarely needed). Other players log if ENABLE_LOGGING or debug_log.
  local log_fname = nil
  if info.player_number == 0 then
    if _G._JSONL_LOGGER_ENABLED then
      log_fname = log.make_filename("player0")
    end
  elseif ENABLE_LOGGING or state.debug_log then
    log_fname = log.make_filename("brain_p" .. info.player_number)
  end
  local t_open_logsetup = clock_us()
  if log_fname then
    if log.open(log_fname) then
      log.dump_map()
      log.dump_world(world)
    end
  end
  local t_open_logdump = clock_us()
  opt(string.format("  log.dump_map+world done %.2f ms (fname=%s)",
    (t_open_logdump - t_open_logsetup) / 1000, tostring(log_fname)))

  -- Always open perf-metrics files for the debug bot (player 0, or any bot
  -- whose debug_log flag is set). Independent of the JSONL logger flag —
  -- these files are small and drive scripts/analyze_metrics.py.
  if info.player_number == 0 or state.debug_log then
    local dir = _G.DEBUG_SESSION_DIR or "."
    local prefix = "player" .. info.player_number
    metrics.open_files(dir, prefix)
  end
  opt(string.format("END Brain.open total=%.2f ms", (clock_us() - t_open0) / 1000))
  opt.flush()

  -- --run-script mode: run the specified Lua file in this brain's VM then exit.
  -- The script has full access to all brain globals (cpf, shield, world, etc.).
  if RUN_SCRIPT_PATH then
    print("[brain] --run-script: " .. RUN_SCRIPT_PATH)
    local ok, err = pcall(dofile, RUN_SCRIPT_PATH)
    if not ok then print("[brain] script error: " .. tostring(err)) end
    os.exit(ok and 0 or 1)
  end
end


-- Shell-hitbox + own-tank-hitbox overlay. Factored out so it can run
-- in BOTH manual mode AND autonomous mode — without this, shells the
-- user fires while driving via M-mode wouldn't show their hit dots
-- (the manual-mode early-return in Brain.think used to skip past
-- the autonomous block where this lived).
local function draw_shell_hitbox_viz(info)
  if not info.objects then return end
  -- Hoist viz toggles once so the per-shell loop can short-circuit
  -- whole blocks (skipping arg evaluation, not just the C call).
  local v_grid = viz.is_on("shot_tile_grid")
  local v_dot  = viz.is_on("shell_hit_dot")
  local v_hb   = viz.is_on("shell_hitbox")
  local v_tank = viz.is_on("tank_hitbox")
  if not (v_grid or v_dot or v_hb or v_tank) then return end

  if v_grid or v_dot or v_hb then
    local pill_live = {}
    for i = 1, #info.objects do
      local ob = info.objects[i]
      if ob.type == 2 and (ob.direction or 0) > 0 then
        local pmx = ob.x >> 8
        local pmy = ob.y >> 8
        pill_live[pmy * 256 + pmx] = true
      end
    end
    for i = 1, #info.objects do
      local ob = info.objects[i]
      if ob.type == 1 then   -- OBJECT_SHOT
        local smx = ob.x >> 8
        local smy = ob.y >> 8
        local on_live_pill = pill_live[smy * 256 + smx] or false
        if v_grid then
          local gr, gg, gb, ga = 80, 80, 200, 180
          for ii = 0, 16 do
            local f = ii / 16.0
          end
        end
        if v_dot then
          local cxt = ob.x / 256.0
          local cyt = ob.y / 256.0
          local r, g, b = 255, 140, 0
          if on_live_pill then r, g, b = 255, 0, 0 end
          -- Precise (sub-game-pixel) so the dot lands on the shell's
          -- authoritative WU position instead of being floored to the
          -- nearest game pixel.
        end
        if v_hb then
          local DOT_WU = 4
          local HALF = DOT_WU * 0.5
          local tx2 = (ob.x + HALF) / 256.0
          local ty1 = (ob.y - HALF) / 256.0
          -- Sprite-tip diagnostic (must mirror mapview.c kTipCol/Row).
          local kTipCol = {1.5, 3.0, 4.0, 4.0,  4.0, 4.0, 4.0, 3.0,
                           1.5, 0.0, 0.0, 0.0,  0.0, 0.0, 0.0, 0.0}
          local kTipRow = {0.0, 0.0, 0.0, 0.0,  1.5, 3.0, 4.0, 4.0,
                           4.0, 4.0, 3.0, 3.0,  1.5, 0.0, 0.0, 0.0}
          local dir16 = (ob.direction or 0) >> 4
          local tc = kTipCol[dir16 + 1]
          local tr = kTipRow[dir16 + 1]
          local x_tile = ob.x / 256.0
          local y_tile = ob.y / 256.0
          local tip_x_tile = tc / 16.0
          local tip_y_tile = tr / 16.0
          local render_x_tile = x_tile - tip_x_tile
          local render_y_tile = y_tile - tip_y_tile
        end
      end
    end
  end
  -- Own-tank hitbox outline. Per tank.c:1089 a shell hits when
  -- abs(tank.x-shell.x) < 128 && abs(tank.y-shell.y) < 128 — so
  -- the hitbox is a 256x256 wu (1 tile) centered on the tank.
  if v_tank and info.tankx and info.tanky then
    local htx1 = (info.tankx - 128) / 256.0
    local hty1 = (info.tanky - 128) / 256.0
    local htx2 = (info.tankx + 128) / 256.0
    local hty2 = (info.tanky + 128) / 256.0
  end
end

-- =========================================================================
-- THINK
-- =========================================================================

function Brain.think(info)
  -- Capture wall clock at think entry; the matching exit-time
  -- snapshot at the bottom drives the top-left tick-info HUD.
  local _think_t0 = os.clock()
  state.tick = state.tick + 1
  state._last_info = info
  local now  = state.tick

  -- Snapshot V-dialog enabled state once per tick so call-site
  -- `if viz.is_on("foo") then ... end` guards reduce to a single
  -- table lookup. Skips per-call string concat + :upper() + assert_id.

  -- Shell hitbox viz: pixel at each in-flight shell's world-pixel
  -- position. These are the exact (x, y) coords shellsCalcCollision
  -- tests next tick. Drawn every tick via overlay so per-frame recording
  -- captures it and playback scrubs correctly.
  --
  -- Color code:
  --   orange = shell over normal terrain
  --   red    = shell's TILE matches a hit-testable pillbox (armour > 0,
  --            not in tank). If you see a red pixel and the shell doesn't
  --            die this tick, pillsIsPillHit missed it — that's the bug.
  -- OBJECT_SHOT = 1, OBJECT_PILLBOX = 2 (see brain.h).
  -- Shell-hitbox viz is drawn later, AFTER overlay_clear(), to avoid
  -- being wiped. See the corresponding draw block marked SHELL_HITBOX_VIZ.

  -- Expose brain tick to C side so debug file naming (.ldump, _queue.txt,
  -- _dbg.txt) can all use the same brain-tick number.
  _G._BRAIN_TICK = now
  opt.set_tick(now)
  opt("BEGIN tick=", now, " goal=", state.goal.kind, " sub=", tostring(state.goal.substate))
  local t_tick_start = clock_us()
  local t_early = t_tick_start

  -- Diagnostic: log when Dijkstra newly reaches a base. State-tracked
  -- by base id so we only log the first time. Called after each
  -- dijkstra_step (in startup mode and the normal scheduler) so we
  -- catch the discovery on the tick it happens. The log goes to
  -- optimize.log directly because there's no dedicated discovery file.
  local function _diag_log_dij_base_discoveries(_now, _slate, _in_boat)
    if not state._base_dij_known then state._base_dij_known = {} end
    for id, b in pairs(world.bases) do
      if not state._base_dij_known[id] then
        local c = cpf.dijkstra_cost_at(_slate, b.mx, b.my, _in_boat)
        if c < 1e29 then
          state._base_dij_known[id] = _now
          if BRAIN_PERF_LOG then
            opt.append("optimize.log", string.format(
              "  [diag] base #%d at (%d,%d) owner=%s REACHED by Dijkstra slate=%d cost=%.1f tick=%d",
              id, b.mx, b.my, tostring(b.owner), _slate, c, _now))
          end
        end
      end
    end
  end

  -- ── Startup mode early-out ───────────────────────────────────────
  -- During the first short-range-Dijkstra lifetime, do the minimum
  -- needed to pick a "go grab the nearest base" goal. Skip the heavy
  -- per-tick work (threat rebuild, long Dijkstra, pool eval, etc.) so
  -- tick-1 cost falls from ~11 ms to ~2 ms.
  if state.startup_mode then
    local _t_su = clock_us()
    -- Fresh world data so world.bases / world.pills are populated.
    W.process_events(world, info, state)
    W.update(world, info, now)
    opt(string.format("  [startup] W.update %.2f ms", (clock_us() - _t_su) / 1000))

    local tmx_s     = info.tankx >> 8
    local tmy_s     = info.tanky >> 8
    local in_boat_s = info.inboat and 1 or 0

    -- Pre-warm the threat grid during startup so tick-11 (first normal tick)
    -- doesn't pay the full 4-5 ms rebuild cost. danger + threat are cheap
    -- on repeat calls once the grid is built; only the first call rebuilds.
    local _t_danger = clock_us()
    danger.update(info, now)
    opt(string.format("  [startup] danger.update %.2f ms", (clock_us() - _t_danger) / 1000))
    local _t_threat = clock_us()
    threat.update(state, world, info)
    opt(string.format("  [startup] threat.update %.2f ms", (clock_us() - _t_threat) / 1000))

    -- Step (or first-time start) the short slate.
    local _t_dij = clock_us()
    local active_s, _, _, _, _, _, _, _, _, _, _ = cpf.dijkstra_status(0)
    if not active_s then
      cpf.dijkstra_start(
        0, now, tmx_s, tmy_s, in_boat_s,
        info.shells or 32, info.trees or 0,
        info.mines or 0, info.armour or 40,
        C.DIJKSTRA_SHORT_MAX_COST, C.DIJKSTRA_EXACT,
        1.0, 0 --[[KIND_NORMAL]], in_boat_s)
    end
    cpf.dijkstra_step(0, now, C.DIJKSTRA_SHORT_BUDGET)
    opt(string.format("  [startup] dijkstra_step %.2f ms", (clock_us() - _t_dij) / 1000))
    _diag_log_dij_base_discoveries(now, 0, in_boat_s)

    -- Look for the closest reachable neutral base. Only set the goal
    -- once — then we stop searching but keep stepping so the slate is
    -- well-warmed by the time normal think takes over.
    --
    -- Note: the source tile (where the tank started) reports as
    -- COST_INF via dijkstra_cost_at — see brainPathfinderDijkstraCostAt
    -- in brain_pathfinder.c. So a base sitting on the bot's own start
    -- tile won't false-win with cost 0 here.
    if not state._startup_goal_set then
      local best_cost, best_id, best_b = math.huge, nil, nil
      for id, b in pairs(world.bases) do
        if b.owner == "neutral" then
          local c = cpf.dijkstra_cost_at(0, b.mx, b.my, in_boat_s)
          if c < best_cost and c < 1e29 then
            best_cost, best_id, best_b = c, id, b
          end
        end
      end
      if best_b then
        state.goal = {
          kind      = "capture_base",
          mx        = best_b.mx, my = best_b.my,
          wx        = U.m2w(best_b.mx),
          wy        = U.m2w(best_b.my),
          target_id = best_id,
        }
        state.goal_set_tick   = now
        state.pf.status       = "idle"
        state._startup_goal_set = true
        opt(string.format("startup: capture_base #%d at (%d,%d) cost=%.0f tick=%d",
                          best_id, best_b.mx, best_b.my, best_cost, now))
      end
    end

    -- Exit startup once the short slate has completed its first full
    -- lifetime. By then it's expanded enough of the map that the
    -- normal flow's update_pool_cache will hit cache for distant
    -- candidates instead of falling all the way to A* fallback.
    if now >= C.DIJKSTRA_SHORT_INTERVAL then
      state.startup_mode = false
      opt("startup: exit at tick=", now,
          " goal_set=", tostring(state._startup_goal_set))
    end

    opt(string.format("STARTUP TICK TOTAL %.2f ms", (clock_us() - t_tick_start) / 1000))
    opt.flush()

    -- STARTUP_HOLD_TICKS already prevents driving during this window,
    -- so we just return zero keys. No steering, no builder, no log.
    return {
      holdkeys    = 0,
      tapkeys     = 0,
      build       = -1,
      wantallies  = info.allies,
      messagedest = 0,
      sendmessage = nil,
    }
  end
  local BOT_VERSION = "v8 2026-04-07"
  if now == 1 then print(TAG .. " >>> CODE VERSION: " .. BOT_VERSION .. " <<<") end
  if now <= 3 then print(TAG .. " think() tick=" .. now) end

  -- Debugger: begin trace capture if armed
  if dbg.is_armed() then
    dbg.snapshot_sources("../brains/NewAutopilot")
    dbg.begin_trace(Brain.think)
  end

  local function hud_builder_status(inf, st)
    if inf.man_status == C.LGM_DEAD then
      return ": dead", 255, 80, 80
    elseif inf.man_status == C.LGM_INTANK then
      return ": ready", 100, 220, 100
    else
      local eta = st.builder and st.builder.lgm_eta
      local remaining = eta and (eta - now)
      if remaining and remaining > 0 then
        return string.format(": out (%dt)", remaining), 255, 200, 50
      end
      return ": out", 255, 200, 50
    end
  end

  -- Manual control: skip all AI, just show HUD. C side handles keys directly.
  if manual_active then
    -- Still update world knowledge so anger/danger reflect what we observe
    W.process_events(world, info, state)
    W.update(world, info, now)
    threat.update(state, world, info)
    -- Sync C danger grid so the cost heatmap stays accurate in manual mode.
    -- Batch-load only when threat actually rebuilt this tick.
    if threat.rebuilt_this_tick then
      cpf.load_pill_danger_from_threat()
      metrics.inc("danger_reloads")
    else
      metrics.inc("danger_skips")
    end
    return { holdkeys = manual_keys, tapkeys = 0, build = -1, wantallies = info.allies, messagedest = 0, sendmessage = "" }
  end



  -- Debug overlay

  -- Optional debug log — keeps the per-tick shell positions in
  -- hitboxes.log for one-off bug hunts. Rendering is handled by
  -- draw_shell_hitbox_viz above; this block ONLY writes the log.


  -- Adjacent tile highlights
  if BRAIN_DEBUG_MODE and viz.is_on("adjacent_tiles") then
    local tx = math.floor(info.tankx / 256)
    local ty = math.floor(info.tanky / 256)
    local adj = {{tx-1, ty}, {tx+1, ty}, {tx, ty-1}, {tx, ty+1}}
    for _, pos in ipairs(adj) do
    end
  end

  -- HUD: tank stats in bottom-left (offset up so the kill-attempt
  -- indicator can sit under it without overlap on shorter window heights).
  if BRAIN_DEBUG_MODE and viz.is_on("hud_resources") then
    local y = 90
    local bld_str, bld_r, bld_g, bld_b = hud_builder_status(info, state)
  end

  -- HUD: last attack-goal clear (set by attack.clear_attack_goal). Stays
  -- visible for ~300 ticks after the abort so a silent "goal went to
  -- none mid-finetune" leaves a breadcrumb pointing at the cause.
  if BRAIN_DEBUG_MODE and state._last_attack_clear and viz.is_on("attack_clear_reason") then
    local lc  = state._last_attack_clear
    local age = (state.tick or 0) - (lc.tick or 0)
    if age >= 0 and age <= 300 then
      local fade = math.max(80, 255 - math.floor(age * 0.5))
      local sub_str = lc.sub_was and ("/" .. lc.sub_was) or ""
      local pos_str = (lc.mx_was and lc.my_was)
                      and string.format(" @(%d,%d)", lc.mx_was, lc.my_was)
                      or  ""
    end
  end

  -- HUD: click-cost panel — moved back to C side (braintest_main.c
  -- renderHUD) so it can use the live Dijkstra slate when not in
  -- playback (and show "N/A" when scrubbed). The brain-side
  -- state.click_inspect handler is no longer needed.

  -- HUD: replan countdown (left side, middle)
  if BRAIN_DEBUG_MODE and viz.is_on("hud_replan") then
    local ticks_left = C.GOAL_REPLAN_INTERVAL - ((now + state.replan_offset) % C.GOAL_REPLAN_INTERVAL)
    if ticks_left == C.GOAL_REPLAN_INTERVAL then ticks_left = 0 end
    local r, g, b = 150, 150, 150
    if ticks_left <= 5 then r, g, b = 255, 255, 0 end
    if ticks_left == 0 then r, g, b = 0, 255, 0 end
  end

  -- HUD: goal info in top-right + per-tick goal log (logging always runs;
  -- HUD draws gated by viz toggles). The log captures the goal on every
  -- tick regardless of viz state — it's a debug audit trail, not display.
  do
    local g = state.goal or {}
    local gkind = g.kind or "none"
    local tid = g.target_id
    local goal_str
    if tid and tid >= 0 then
      goal_str = string.format("Goal: %s #%d (%d,%d)",
        gkind, tid, g.mx or 0, g.my or 0)
    else
      goal_str = string.format("Goal: %s (%d,%d)",
        gkind, g.mx or 0, g.my or 0)
    end
    -- Per-tick goal log (file write — non-viz, always runs).
    local sdir = _G.DEBUG_SESSION_DIR
    if sdir then
      local pn = info.player_number or 0
      local path = string.format("%s/goal_player%d.log", sdir, pn)
      if not _G._GOAL_LOG_FILE or _G._GOAL_LOG_PATH ~= path then
        if _G._GOAL_LOG_FILE then pcall(function() _G._GOAL_LOG_FILE:close() end) end
        local f = io.open(path, "a")
        if f then
          _G._GOAL_LOG_FILE = f
          _G._GOAL_LOG_PATH = path
        end
      end
      if _G._GOAL_LOG_FILE then
        local sub = (g.substate and g.substate ~= "" and g.substate ~= "-") and (" sub=" .. g.substate) or ""
        _G._GOAL_LOG_FILE:write(string.format("%d\t%s%s\n", state.tick or 0, goal_str, sub))
        _G._GOAL_LOG_FILE:flush()
      end
    end
    -- HUD draws (gated)
  end

  -- Store server tick for absolute time references
  state.server_tick = info.server_tick or 0

  -- Section timing (clock_us is a C function returning microseconds)
  local t0 = clock_us()
  metrics.set("us_early_viz", t0 - t_early)
  opt(string.format("early-viz/HUD done %.2f ms", (t0 - t_early) / 1000))

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
      if state.builder.target and state.builder.target.x and state.builder.target.y then
        local bk = U.mkey(state.builder.target.x, state.builder.target.y)
        state.blocked[bk] = now + 300
        log.event("assist_msg", string.format("no_build at %d,%d",
          state.builder.target.x, state.builder.target.y))
      end
    elseif msg == ASSIST_MSG_PILL_NO_REPAIR then
      -- Pill doesn't need repair: cancel repair goal
      if state.goal.kind == "repair_pill" then
        attack.clear_attack_goal(state)
        log.event("assist_msg", "pill_no_repair")
      end
    elseif msg == ASSIST_MSG_MAN_DEAD then
      -- LGM died: note for tactical decisions
      log.event("assist_msg", "man_dead")
    end
  end

  local t_events = clock_us()
  opt(string.format("  events+assist done %.2f ms", (t_events - t0) / 1000))

  -- Selective cache invalidation based on pill/terrain changes
  PF.begin_tick(now, world)
  local t_pf_begin = clock_us()
  opt(string.format("  PF.begin_tick done %.2f ms", (t_pf_begin - t_events) / 1000))

  -- Expire wounded pill memory:
  --   1. After WOUNDED_FINISH_DECAY_TICKS (matches the cost-curve decay
  --      so the marker disappears at the same tick the discounts hit 1.0x).
  --   2. If the pill at that tile no longer matches the wounded entry —
  --      either someone else captured it, we captured it, or the pill
  --      was rebuilt with a different id. Without this, the 0.30x and
  --      commit discount keep applying to a now-enemy-owned pill that
  --      may already be healing back to full HP.
  if state.wounded_pill then
    local wp     = state.wounded_pill
    local expire = (now - wp.tick) > (C.WOUNDED_FINISH_DECAY_TICKS or 1500)
    local cur    = wp.id and world.pills and world.pills[wp.id] or nil
    local owner_changed = wp.owner and cur and cur.owner ~= wp.owner
    local pill_gone     = wp.id and not cur
    -- Healed past where we left it: enemy LGM repaired it back up.
    -- Compare to the recorded wp.hp (HP at the moment we tagged it,
    -- typically at swerve / loiter timeout). Even a 1-HP rise means
    -- someone is actively repairing — re-evaluate at normal cost.
    local healed = cur and cur.health and (cur.health > (wp.hp or 0))
    if expire or owner_changed or pill_gone or healed then
      state.wounded_pill = nil
    end
  end

  -- Update world knowledge
  local t_wu0 = clock_us()
  W.update(world, info, now)
  local t_wu1 = clock_us()
  opt(string.format("  W.update done %.2f ms", (t_wu1 - t_wu0) / 1000))

  -- Update exploration frontier
  expl.update(state, info)
  opt(string.format("  expl.update done %.2f ms", (clock_us() - t_wu1) / 1000))

  local t1 = clock_us()
  metrics.set("us_world", t1 - t0)
  opt(string.format("world done %.2f ms", (t1 - t0) / 1000))

  -- Update shell-trajectory danger map
  danger.update(info, now)

  local t2 = clock_us()
  metrics.set("us_danger", t2 - t1)
  opt(string.format("danger done %.2f ms", (t2 - t1) / 1000))

  -- Process sound events into combat heat map
  local t_hearing0 = clock_us()
  hearing.update(info, now)
  local t_hearing1 = clock_us()
  opt(string.format("  hearing.update done %.2f ms", (t_hearing1 - t_hearing0) / 1000))

  -- Rebuild spatial threat grid (pill + tank layers, O(1) lookup for consumers)
  threat.update(state, world, info)
  local t_threat_upd = clock_us()
  opt(string.format("  threat.update done %.2f ms", (t_threat_upd - t_hearing1) / 1000))

  -- Populate C pathfinder danger grid from Lua threat grid (single source of
  -- truth). Batch-load the whole grid in one C call instead of ~13K per-tile
  -- cpf.set_danger calls, and only when threat.update actually rebuilt it.
  if threat.rebuilt_this_tick then
    cpf.load_pill_danger_from_threat()
    metrics.inc("danger_reloads")
  else
    metrics.inc("danger_skips")
  end
  local t_load_danger = clock_us()
  opt(string.format("  cpf.load_danger done %.2f ms (rebuilt=%s)",
    (t_load_danger - t_threat_upd) / 1000, tostring(threat.rebuilt_this_tick)))

  -- Push per-pill contribution maps to the host (BrainTest reads
  -- this for the shift-2 cycle-pill-overlay). Bindings no-op
  -- under non-host runtimes (game client, headless server).
  -- NOTE: clear is done host-side once per tick (BrainTest's
  -- appTickBrain) so multi-bot games don't have one bot wipe
  -- another's entries. We just append from here.
  if pillcontrib_begin_pill then
    -- Walk pills in id-stable order so the cycle index stays
    -- consistent across ticks. Skip dead pills (no contribution).
    if world.pills and threat.pill_contrib then
      local pids = {}
      for id, _ in pairs(world.pills) do pids[#pids+1] = id end
      table.sort(pids)
      for _, id in ipairs(pids) do
        local p = world.pills[id]
        if p and p.health and p.health > 0
           and (p.owner == "hostile" or p.owner == "neutral") then
          local pkey = p.my * 256 + p.mx
          local contrib = threat.pill_contrib[pkey]
          if contrib then
            local slot = pillcontrib_begin_pill(id, p.mx, p.my)
            if slot >= 0 then
              if pillcontrib_add_all then
                pillcontrib_add_all(slot, contrib)
              else
                for tkey, val in pairs(contrib) do
                  if val and val > 0 then
                    local tx = tkey % 256
                    local ty = (tkey - tx) / 256
                    pillcontrib_add_tile(slot, tx, ty, val)
                  end
                end
              end
            end
          end
        end
      end
    end
  end
  local t_pillcontrib = clock_us()
  opt(string.format("  pillcontrib export done %.2f ms", (t_pillcontrib - t_load_danger) / 1000))

  -- Populate influence grid (friendly = positive, hostile = negative).
  -- Rebuilt every tick — it's cheap and consumers expect fresh values.
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
  local t_influence = clock_us()
  opt(string.format("  influence stamp done %.2f ms", (t_influence - t_pillcontrib) / 1000))

  -- Overlay grid: friendly pills = IMPASSABLE (32767 = max int16_t overlay
  -- value — a wall+shoot path costs ~30, so 32767 is effectively infinity
  -- and A* routes around). Hostile bases = 3× wall cost (overlay=89 →
  -- total 90): expensive but traversable, so A* plans through when the
  -- detour is worse and goal scoring "thinks twice" about targets sitting
  -- on their own base.
  --
  -- Rebuilt only when the friendly-pill or hostile-base set changes.
  -- steering.lua re-stamps stuck-blacklist entries every tick (and now
  -- explicitly zeros them on expiry — see stuck_recovery) so those
  -- entries don't need an every-tick clear to stay consistent.
  threat.check_overlay_dirty(world)
  if threat.overlay_dirty then
    cpf.clear_overlay()
    for _, pm in pairs(world.pills) do
      if pm.health > 0 then
        if pm.owner == "friendly" then
          -- Don't drive through our own pills
          cpf.set_overlay(pm.mx, pm.my, 32767)
        elseif pm.owner == "hostile" or pm.owner == "neutral" then
          -- Hostile/neutral pills are impassable obstacles (can't drive on them)
          cpf.set_overlay(pm.mx, pm.my, 32767)
        end
      end
    end
    for _, b in pairs(world.bases) do
      if b.owner == "hostile" then
        -- Hostile bases: 3x the cost of a wall (15 shots equivalent)
        cpf.set_overlay(b.mx, b.my, 15 * C.WALL_SHOOT_COST)
      end
      -- Friendly/neutral bases are fine (road-like terrain, no overlay)
    end
    threat.snapshot_overlay(world)
    threat.overlay_dirty = false
    metrics.inc("overlay_reloads")
  else
    metrics.inc("overlay_skips")
  end
  local t_overlay = clock_us()
  opt(string.format("  overlay rebuild done %.2f ms (dirty=%s)",
    (t_overlay - t_influence) / 1000, tostring(threat.overlay_dirty == false)))

  local t3 = clock_us()
  metrics.set("us_threat", t3 - t2)
  opt(string.format("threat done %.2f ms", (t3 - t2) / 1000))

  -- Build shared perception snapshot (before goal selection / builder / steering)
  percept.update(state, world, info)
  local t_percept_upd = clock_us()
  opt(string.format("  percept.update done %.2f ms", (t_percept_upd - t3) / 1000))

  -- Track our own fired shots from fire-to-impact (uses info.shells decrement
  -- to detect fires and info.objects OBJECT_SHOT entries to verify in-flight).
  shot_tracker.update(info, now)
  local t_shot_tr = clock_us()
  opt(string.format("  shot_tracker done %.2f ms", (t_shot_tr - t_percept_upd) / 1000))

  -- Classify game phase (reads state.perc, must run after percept.update)
  strategy.update(state, world, info)
  opt(string.format("  strategy.update done %.2f ms", (clock_us() - t_shot_tr) / 1000))

  local t4 = clock_us()
  metrics.set("us_percept", t4 - t3)
  opt(string.format("percept done %.2f ms", (t4 - t3) / 1000))

  -- Track per-tick damage for base shield timing: dispatch LGM right
  -- after taking a hit (pill just fired, max time until next shot)
  state.took_damage_this_tick = false
  if info.armour < (state._prev_armour or info.armour) then
    state.took_damage_this_tick = true
    state._last_damage_tick = now
  end
  state._prev_armour = info.armour

  -- ── Incremental Dijkstra scheduler ──
  --
  -- Generic multi-slate scheduler. The C side has 4 slates and a
  -- by-kind lookup that always returns the freshest matching result
  -- (with automatic partial-result fallback to the next-newest slate
  -- of the same kind when the newer one hasn't reached a tile yet).
  --
  -- The brain just decides WHEN to start a new search of each kind.
  -- For every kind we care about (normal-danger and pill-low-danger),
  -- we kick off a new search whenever the freshest slate of that kind
  -- is older than DIJKSTRA_RECOMPUTE_INTERVAL ticks (or the tank moved
  -- enough). The C side picks which physical slate to overwrite via
  -- pick_reuse_slate(kind), preserving the freshest same-kind slate
  -- so partial-result lookups can fall back to it.
  --
  -- KIND_NORMAL = 0 — standard danger weighting, used for refuel,
  --                   capture, exploration, etc.
  -- KIND_PILL   = 1 — path cost with a specific pill's local danger
  --                   contribution subtracted.  No dedicated Dijkstra slate;
  --                   implemented via cpf.smart_cost_minus_pill_danger()
  --                   which runs KIND_NORMAL then walks the traced path and
  --                   removes that pill's per-tile contrib from the total.
  --
  -- state.dij.slates is a Lua-side mirror of slate metadata for
  -- inspection / debugging. Each entry tracks the parameters used to
  -- start it so the brain can reason about what's in each slot.
  local t_dij0 = clock_us()
  do
    -- Slate layout (all KIND_NORMAL):
    --   0 = short-range main    (restarts every DIJKSTRA_SHORT_INTERVAL)
    --   1 = short-range backup  (snapshot of 0 taken at each restart)
    --   2 = long-range main     (restarts every DIJKSTRA_RECOMPUTE_INTERVAL)
    --   3 = long-range backup   (snapshot of 2 taken at each restart)
    -- Lookups prefer the freshest slate that has a finite cost for the tile,
    -- so during recomputation the backup provides full coverage seamlessly.
    local SLATE_SHORT_MAIN   = 0
    local SLATE_SHORT_BACKUP = 1
    local SLATE_LONG_MAIN    = 2
    local SLATE_LONG_BACKUP  = 3

    local tmx = info.tankx >> 8
    local tmy = info.tanky >> 8
    local in_boat = info.inboat and 1 or 0
    local d = state.dij
    if not d then
      d = { slates = {} }
      for i = 0, 3 do
        d.slates[i] = {
          index = i, kind = nil,
          started_tick = 0, completed_tick = 0,
          src_mx = -1, src_my = -1, in_boat = 0,
          danger_scale = 1.0, exact = true,
          done = false, active = false,
        }
      end
      state.dij = d
    end

    -- Refresh our Lua-side mirror of slate metadata from C.
    local function refresh_slate(idx)
      local active, done, kind, started, completed, expanded, peak_open,
            sx, sy, sb, danger_scale = cpf.dijkstra_status(idx)
      local s = d.slates[idx]
      s.active         = active
      s.done           = done
      s.kind           = kind
      s.started_tick   = started or 0
      s.completed_tick = completed or 0
      s.expanded       = expanded or 0
      s.peak_open      = peak_open or 0
      s.src_mx         = sx or -1
      s.src_my         = sy or -1
      s.in_boat        = sb or 0
      s.danger_scale   = danger_scale or 1.0
    end

    -- Start a slate.
    local function start_slate(idx, max_cost, boat, allow_boat)
      if boat == nil then boat = in_boat end
      if allow_boat == nil then allow_boat = 1 end
      cpf.dijkstra_start(
        idx, now, tmx, tmy, boat,
        info.shells or 32, info.trees or 0,
        info.mines or 0, info.armour or 40,
        max_cost, C.DIJKSTRA_EXACT,
        1.0, 0 --[[KIND_NORMAL]], allow_boat)
      refresh_slate(idx)
    end

    -- Double-buffer restart: at each interval, snapshot the main slate into
    -- the backup so lookups always have complete coverage, then restart main.
    local function maybe_copy_and_restart(main_idx, backup_idx, interval, max_cost, boat, allow_boat)
      refresh_slate(main_idx)
      local s = d.slates[main_idx]
      local age = now - s.started_tick
      if not s.active or age >= interval then
        if s.active then
          local t_cs = clock_us()
          cpf.dijkstra_copy_slate(main_idx, backup_idx)
          opt(string.format("dij COPY %d->%d done %.2f ms",
            main_idx, backup_idx, (clock_us() - t_cs) / 1000))
          refresh_slate(backup_idx)
        end
        start_slate(main_idx, max_cost, boat, allow_boat)
      end
    end

    -- Short: land-only unless on a boat. Long: always allow boat.
    maybe_copy_and_restart(SLATE_SHORT_MAIN, SLATE_SHORT_BACKUP,
                           C.DIJKSTRA_SHORT_INTERVAL, C.DIJKSTRA_SHORT_MAX_COST,
                           in_boat, in_boat)
    maybe_copy_and_restart(SLATE_LONG_MAIN, SLATE_LONG_BACKUP,
                           C.DIJKSTRA_RECOMPUTE_INTERVAL, C.DIJKSTRA_MAX_COST,
                           in_boat, 1)

    -- Step only the main slates (backups are frozen snapshots, done=true).
    local budget_short = C.DIJKSTRA_SHORT_BUDGET
    local budget_long  = math.max(500, math.ceil(70000 / C.DIJKSTRA_LONG_SPREAD_TICKS))
    local active_slates = {}
    for _, idx in ipairs({ SLATE_SHORT_MAIN, SLATE_LONG_MAIN }) do
      refresh_slate(idx)
      if d.slates[idx].active and not d.slates[idx].done then
        active_slates[#active_slates + 1] = idx
      end
    end
    if #active_slates > 0 then
      local t_step = clock_us()
      local total_exp = 0
      for _, idx in ipairs(active_slates) do
        local budget = (idx == SLATE_SHORT_MAIN) and budget_short or budget_long
        local done, expanded = cpf.dijkstra_step(idx, now, budget)
        total_exp = total_exp + (expanded or 0)
        if done then
          refresh_slate(idx)
        end
      end
      local step_us = clock_us() - t_step
      opt(string.format("dij STEP %.2f ms  active=%d  budget(s/l=%d/%d)  total_exp=%d",
                           step_us / 1000, #active_slates, budget_short, budget_long, total_exp))
      -- Scan for newly-reached bases on the short slate (the one most
      -- likely to gate goal selection at startup).
      _diag_log_dij_base_discoveries(now, SLATE_SHORT_MAIN, in_boat)
    end
  end
  local t_dij1 = clock_us()
  opt(string.format("dij sched done %.2f ms", (t_dij1 - t_dij0) / 1000))

  -- Outgoing message -- only one per tick
  local t_comms0 = clock_us()
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
    -- Brain-to-brain coordination (ally claims)
    comms.process_message(info.message.sender, info.message.text, now)

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
  comms.expire_claims(now)

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

  -- Respawn handling. Use clear_attack_goal so any in-progress
  -- attack_pill / PPT state (substate, _shield_scan, _aim_locked,
  -- _wall_build_list, etc.) doesn't leak through into the next
  -- goal selection on the new tank.
  if info.newtank then
    state.stuck_for = 0
    attack.clear_attack_goal(state)
    print(string.format(TAG .. " t=%d RESPAWN at (%d,%d)",
          now, info.tankx >> 8, info.tanky >> 8))
    log.event("respawn", string.format("%d,%d", info.tankx >> 8, info.tanky >> 8))
  end

  -- Stuck detection
  local t_stuck0 = clock_us()
  local cur_mx = info.tankx >> 8
  local cur_my = info.tanky >> 8

  -- When attacking a pill in engage/ws_ substates we are intentionally
  -- stationary; don't count that as being stuck.
  -- approach: tank brakes to a true stop (SPEED_TOL=0) before flipping
  --           to aim/build_walls; sits on one tile for several ticks.
  -- build_walls: tank parks at approach point while LGM builds shield
  --              walls (can take many seconds, multi-trip).
  -- Without these, stuck detection fires flee_pill mid-attack.
  local intentionally_stationary =
       (state.goal.kind == "attack_pill" and ATTACK_STATIONARY_SUBS[state.goal.substate or ""])
    or (state.goal.kind == "pill_place" and PP_STATIONARY_SUBS[state.goal.substate or ""])
    or (state.goal.kind == "attack_tank" and TANK_COMBAT_STATIONARY_SUBS[state.goal.substate or ""])
    -- refuel_at_base only counts as stationary when actually parked on the base
    or (state.goal.kind == "refuel_at_base" and W.tank_on_friendly_base(world, info))
    or state.goal.kind == "rescue_lgm"
    or state.goal.kind == "wait_for_lgm"
  local attack_at_standoff = intentionally_stationary

  if cur_mx == state.last_mx and cur_my == state.last_my
     and state.goal.kind ~= "none"
     and not attack_at_standoff
     and not state.wall_clearing then
    state.stuck_for = state.stuck_for + 1
    if state.stuck_for > 150 then  -- ~3 s
      if state.goal.kind == "attack_pill" or state.goal.kind == "pill_place" then
        -- Couldn't reach the attack position: flee away from the pill.
        -- Radial projection can land on water/building — walk the ray back
        -- toward the tank until we find a passable land tile.
        local dx  = cur_mx - state.goal.mx
        local dy  = cur_my - state.goal.my
        local len = math.max(1, math.sqrt(dx * dx + dy * dy))
        local ux, uy = dx / len, dy / len
        local fmx, fmy
        -- Try decreasing distances from FLEE_PILL_DIST down to 1 tile.
        for d = C.FLEE_PILL_DIST, 1, -1 do
          local tx = U.mclamp(math.floor(cur_mx + ux * d + 0.5))
          local ty = U.mclamp(math.floor(cur_my + uy * d + 0.5))
          local tt = U.ttype(tx, ty)
          if not U.is_water(tt)
             and tt ~= C.T_BUILDING and tt ~= C.T_HALFBUILD then
            fmx, fmy = tx, ty
            break
          end
        end
        if not fmx then
          -- All of the flee ray is blocked; stay put.
          fmx, fmy = cur_mx, cur_my
        end
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
        attack.clear_attack_goal(state)
      end
      state.stuck_for = 0
    end
  else
    state.stuck_for = 0
  end
  state.last_mx = cur_mx
  state.last_my = cur_my

  local t_stuck1 = clock_us()
  opt(string.format("  stuck-detection done %.2f ms", (t_stuck1 - t_stuck0) / 1000))

  -- Expire stale blocked entries
  if now % 200 == 0 then
    for k, until_t in pairs(state.blocked) do
      if now >= until_t then state.blocked[k] = nil end
    end
    -- Same cadence: sweep expired banned-approach-angle entries.
    -- Per-pill sub-tables get GC'd when emptied.
    if state.banned_pill_angles then
      for pkey, bans in pairs(state.banned_pill_angles) do
        local any_left = false
        for bucket, exp in pairs(bans) do
          if now >= exp then bans[bucket] = nil
          else any_left = true end
        end
        if not any_left then state.banned_pill_angles[pkey] = nil end
      end
    end
  end

  -- Water escape emergency
  local t_mid = clock_us()
  opt(string.format("  blocked/banned sweep done %.2f ms (sweep=%s)",
    (t_mid - t_stuck1) / 1000, tostring(now % 200 == 0)))
  metrics.set("us_mid", t_mid - t4)
  opt(string.format("mid done %.2f ms", (t_mid - t4) / 1000))
  opt(string.format("  comms+stuck done %.2f ms", (t_mid - t_comms0) / 1000))
  local t_water0 = t_mid
  local t_goal0 = clock_us()   -- initialized here; updated below if goal section runs
  local t_goal1 = nil
  local tank_tt  = U.ttype(cur_mx, cur_my)
  local in_water = not info.inboat
                   and (tank_tt == C.T_RIVER or tank_tt == C.T_DEEPSEA)

  -- Detect boat loss: invalidate pathfinder and pool cache so escape_water
  -- and goal replan use on-foot costs instead of stale in-boat estimates.
  local lost_boat = state.was_in_boat and not info.inboat
  state.was_in_boat = info.inboat
  if lost_boat then
    state.pool_cache = nil
    state.pf.status = "idle"
    -- Keep next_mx/next_my as fallback so steering has a waypoint
    -- while A* recomputes with on-foot costs. Without this, the
    -- steering aims directly at the destination for 1-3 ticks,
    -- which can send the tank into water.
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

    -- Clear escape_water goal once on dry land and trigger immediate replan
    if state.goal.kind == "escape_water" then
      print(string.format(TAG .. " t=%d ESCAPED WATER at (%d,%d) -- replanning",
            now, cur_mx, cur_my))
      state.goal = { kind = "none" }
    end

    -- Build road under self when on slow terrain (swamp/rubble/crater).
    -- ROI: swamp traversal ~85 ticks vs ~44 ticks with road built → saves ~40 ticks/tile.
    -- LGM barely leaves the tank's tile so exposure is short; use LGM_DANGER_MED.
    -- Skip when in a boat: the tank doesn't need roads on water, and dispatching
    -- the LGM triggers pacing that slows the tank below disembark speed.
    local slow_tt = not info.inboat and C.ROAD_BUILD_TERRAIN[tank_tt] or nil
    if slow_tt and info.man_status == C.LGM_INTANK
       and info.trees >= slow_tt + C.TREE_RESERVE then
      state.slow_build = { x = cur_mx, y = cur_my }
    else
      state.slow_build = nil
    end

    -- Track how long we've been carrying a pill (used by eval_place_pill_strategic
    -- to ramp up urgency the longer the pill sits in the tank).
    if (info.carried_pills or 0) >= 1 then
      if not state.carrying_pill_since then state.carrying_pill_since = now end
    else
      state.carrying_pill_since = nil
    end

    -- Anti-tank opportunistic pill drop: if carrying a pill and an enemy
    -- tank is close, place the pill between us and the threat.
    if C.ANTITANK_DROP_ENABLED
       and (info.carried_pills or 0) >= 1
       and info.man_status == C.LGM_INTANK
       and not info.inboat
       and (not state.antitank_drop_cooldown or now >= state.antitank_drop_cooldown)
       and state.goal.kind ~= "pill_place"
       and state.goal.kind ~= "attack_pill"
       and state.goal.kind ~= "attack_pill" then
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

    -- Emergency pill drop: about to die with carried pills — drop one to save it.
    -- aIndy checks: no incoming shells nearby, don't drop in front of tank,
    -- don't drop in path of shells, don't drop if no enemies.
    if C.EMERGENCY_DROP_ENABLED
       and (info.carried_pills or 0) >= 1
       and info.armour <= C.EMERGENCY_DROP_ARMOUR
       and info.man_status == C.LGM_INTANK
       and not info.inboat
       and state.goal.kind ~= "pill_place"
       and state.perc and state.perc.enemy_tank_count >= C.EMERGENCY_DROP_MIN_ENEMIES then
      -- Check for incoming shells nearby — don't send LGM into fire
      local shells_close = false
      for _, ob in ipairs(info.objects) do
        if ob.type == OBJECT_SHOT and (ob.info & OBJECT_HOSTILE) ~= 0 then
          local sdist = U.mdist(cur_mx, cur_my, ob.x >> 8, ob.y >> 8)
          if sdist <= C.EMERGENCY_DROP_SHELL_SAFE_DIST then
            shells_close = true
            break
          end
        end
      end
      if not shells_close then
        -- Find safe drop position: behind the tank (away from threats)
        -- Don't drop in front of tank (LGM gets run over or blocks path)
        local tank_dir = info.direction
        local best_drop_mx, best_drop_my = nil, nil
        local best_drop_danger = math.huge
        for d = 0, C.EMERGENCY_DROP_SEARCH_DIRS - 1 do
          local angle = d * (2 * math.pi / C.EMERGENCY_DROP_SEARCH_DIRS)
          local dx = math.floor(math.sin(angle) + 0.5)
          local dy = math.floor(-math.cos(angle) + 0.5)
          local px, py = cur_mx + dx, cur_my + dy
          if U.in_map(px, py) and U.is_placeable(px, py, world) then
            -- Prefer tiles away from where we're heading (behind us)
            local aim_to_drop = U.aim_at(info.tankx, info.tanky, U.m2w(px), U.m2w(py))
            local angle_from_front = math.abs(U.adiff(tank_dir, aim_to_drop))
            -- Only consider tiles that aren't directly ahead (> 60 degrees off)
            if angle_from_front > 60 then
              local d_danger = danger.danger_at(px, py, now, world)
              if d_danger < best_drop_danger then
                best_drop_danger = d_danger
                best_drop_mx = px
                best_drop_my = py
              end
            end
          end
        end
        if best_drop_mx then
          state.goal = {
            kind = "place_pill_strategic", mx = best_drop_mx, my = best_drop_my,
            wx = U.m2w(best_drop_mx), wy = U.m2w(best_drop_my),
            emergency = true,
          }
          state.pf.status = "idle"
          print(string.format(TAG .. " t=%d EMERGENCY PILL DROP at (%d,%d) armour=%d",
                now, best_drop_mx, best_drop_my, info.armour))
          log.event("emergency_drop", string.format("at(%d,%d) arm=%d", best_drop_mx, best_drop_my, info.armour))
          -- Overlay: emergency drop position
        end
      end
    end

    -- Goal invalidation: check whether the world state still supports
    -- the current goal.  If not, force an immediate replan rather than
    -- waiting for the periodic 25-tick cycle.  In aiFull mode the world
    -- data is fresh every tick so this reacts instantly.
    local t_gv0 = clock_us()
    local goal_valid = true
    local gk = state.goal.kind
    local gmx, gmy = state.goal.mx, state.goal.my
    local t_gv1 = clock_us()
    if gk == "capture_base" then
      local b = W.base_at(world, gmx, gmy)
      -- Accept neutral (normal capture) and hostile (weakened base drive-over capture)
      if not b or (b.owner ~= "neutral" and b.owner ~= "hostile") then
        -- Base captured — replan immediately (refuel will win if supplies are low)
        if b and b.owner == "friendly" then
          print(string.format(TAG .. " t=%d BASE CAPTURED: (%d,%d) — replanning", now, gmx, gmy))
        end
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
      -- W.pill_at filters out health==0 pills, but capture_pill targets dead pills.
      -- Look directly in the spatial index so we can see dead pills too.
      -- A pill that's already in someone's tank (in_tank) is no longer
      -- on the ground — invalidate so we replan instead of driving back
      -- to the stale (mx,my).
      local entries = world.pill_at[gmy * C.MAP_W + gmx]
      local p = nil
      if entries then
        for _, e in ipairs(entries) do
          if e.pill.health == 0 and not e.pill.in_tank then p = e.pill; break end
        end
      end
      -- A dead hostile pill is still capturable (engine sets owner to
       -- dead-player when an enemy dies carrying it; tank.c:1920).
       -- filter_capture_pill in goals.lua explicitly accepts hostile-
       -- owned dead pills, so validation must too — otherwise the goal
       -- gets invalidated the tick after selection and we ping-pong
       -- into attack_pill on a different target.
      if not p then goal_valid = false end
    elseif gk == "attack_pill" and not state.capture_objective then
      -- Autonomous attack (not cp command): invalid if pill died or changed side
      -- BUT NOT during swerve — swerve must complete to dodge damage,
      -- the substate machine handles pill death after swerve finishes
      local sub = state.goal.substate
      if sub ~= "swerve" then
        local p = W.pill_at(world, gmx, gmy)
        if not p or p.owner == "friendly" or p.health == 0 then
          goal_valid = false
          -- Pill killed externally (not our capture): wipe the attack_pill
          -- pool cache so the urgent replan can't immediately re-select a
          -- stale pill from old cost_cache data.  The eval queue will
          -- re-evaluate fresh candidates within the next replan cycle.
          if not (p and p.owner == "friendly") and state.pool_cache then
            state.pool_cache[6] = nil
          end
        end
      end
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
        if #state.perc.enemy_tanks > 0 then
          has_target = true
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
      -- Build a specific reason for the clear-overlay: for attack_pill
      -- name WHICH check failed (the silent killer during finetune is
      -- almost always one of these three).
      local why = string.format("goal_invalid: %s@(%d,%d)", gk, gmx, gmy)
      if gk == "attack_pill" and not state.capture_objective then
        local p = W.pill_at(world, gmx, gmy)
        if not p then
          why = string.format("attack_pill@(%d,%d): pill_at returned nil (picked up / in_tank?)", gmx, gmy)
        elseif p.owner == "friendly" then
          why = string.format("attack_pill@(%d,%d): pill became friendly (we/ally captured)", gmx, gmy)
        elseif p.health == 0 then
          why = string.format("attack_pill@(%d,%d): pill HP=0 (killed externally)", gmx, gmy)
        end
      end
      attack.clear_attack_goal(state, why)
    end

    t_goal0 = clock_us()
    if BRAIN_PERF_LOG and t_goal0 - t_gv0 > 1000 then
      opt.append("optimize.log", string.format(
        "  [gv] SLOW total=%.3f ms pre=%.3f ms body=%.3f ms gk=%s",
        (t_goal0 - t_gv0) / 1000,
        (t_gv1 - t_gv0) / 1000,
        (t_goal0 - t_gv1) / 1000,
        tostring(gk)))
    end
    opt(string.format("  goal-validation chain done %.2f ms (gk=%s)", (t_goal0 - t_gv0) / 1000, tostring(gk)))
    opt(string.format("water+goal_invalid done %.2f ms", (t_goal0 - t_water0) / 1000))

    -- Rolling candidate evaluation: 2 A* cost_to calls per tick
    -- Skip on ticks where threat grid rebuilt (both are expensive, don't stack)
    local t_pc0 = clock_us()
    if not threat.rebuilt_this_tick then
      goals.update_pool_cache(state, world, info)
    end
    opt(string.format("  update_pool_cache done %.2f ms", (clock_us() - t_pc0) / 1000))

    -- Goal selection (not in water)
    -- Also trigger an urgent replan if attack_tank is active but the enemy
    -- has left perception — bot should return to whatever it was doing
    -- (typically refuel) immediately rather than waiting for the timer.
    -- Refuel hysteresis is naturally clear since state.goal.kind == "attack_tank".
    local attack_tank_done = state.goal.kind == "attack_tank"
        and (not state.perc or not state.perc.enemy_tanks
             or #state.perc.enemy_tanks == 0)
    if BRAIN_DEBUG_MODE and state.goal.kind == "attack_tank" then
      local et_count = (state.perc and state.perc.enemy_tanks) and #state.perc.enemy_tanks or 0
    end
    -- Trigger a one-shot urgent replan the first tick an enemy tank enters
    -- combat range while we're doing something else.  attack_tank is HYST_EXEMPT
    -- so if it's genuinely cheaper (LOS engage) it wins immediately; if not,
    -- the existing goal stays.  This avoids waiting up to GOAL_REPLAN_INTERVAL
    -- ticks before reacting to a tank attack.
    local tank_now_in_range = false
    if state.goal.kind ~= "attack_tank"
       and state.perc and state.perc.enemy_tanks then
      if #state.perc.enemy_tanks > 0 then
        tank_now_in_range = true
      end
    end
    local tank_appeared = tank_now_in_range and not (state.prev_tank_in_range or false)
    state.prev_tank_in_range = tank_now_in_range
    -- Trigger a one-shot urgent replan the first tick a dead pill appears on
    -- the ground.  capture_pill is HYST_EXEMPT and costs ~0.1× path, so it
    -- almost always wins immediately; only a very close attack_tank beats it.
    local dead_pill_count = (state.perc and state.perc.dead_neutral_pill_count) or 0
    local dead_pill_appeared = dead_pill_count > 0
                            and dead_pill_count > (state.prev_dead_pill_count or 0)
                            and state.goal.kind ~= "capture_pill"
    state.prev_dead_pill_count = dead_pill_count
    local urgent_replan = state.goal.kind == "none" or attack_tank_done
                       or tank_appeared or dead_pill_appeared
    if urgent_replan then
      -- Record which factor(s) tripped the urgent replan so the HUD
      -- below can flash a banner that's visible for a few seconds.
      -- Most-specific reason wins when more than one is true.
      local reason
      if tank_appeared        then reason = "TANK APPEARED"
      elseif dead_pill_appeared then reason = "DEAD PILL"
      elseif attack_tank_done  then reason = "ATTACK_TANK DONE"
      else                          reason = "GOAL=NONE"
      end
      state._last_urgent_replan = { tick = now, reason = reason }
    end

    -- Refuel state machine.  Three independent flags computed here:
    --
    --   refuel_needed   — actually missing some resource we want at base
    --   refuel_complete — at our refuel target and fully stocked
    --   refuel_hold     — anti-thrash lock; suppresses timer-based replans
    --                     while we're committed to a refuel.  Only set when
    --                     C.REFUEL_LOCK_IN is true.
    --
    -- The "refuel done → replan immediately" trigger uses refuel_complete
    -- (not !refuel_hold) so that turning off REFUEL_LOCK_IN doesn't cause
    -- the bot to replan every single tick while traveling to a refuel.
    local refuel_needed = false
    local refuel_complete = false
    local refuel_hold = false
    if state.goal.kind == "refuel_at_base" then
      local need_armour = info.armour < state.armour_target
      local need_shells = info.shells < state.shell_target
      local need_mines  = info.mines < 10
      refuel_needed = need_armour or need_shells or need_mines
      refuel_complete = not refuel_needed

      -- Depleted-base detection runs regardless of lock-in: if we arrive
      -- at a base that has nothing to give us, block it and replan.
      if refuel_needed and info.base then
        local LOW = 4
        local getting_something = false
        if need_armour and (info.base.armour or 0) >= LOW then getting_something = true end
        if need_shells and (info.base.shells or 0) >= LOW then getting_something = true end
        if need_mines  and (info.base.mines or 0) >= LOW then getting_something = true end
        if not getting_something then
          local bk = U.mkey(state.goal.mx, state.goal.my)
          state.blocked[bk] = now + 200
          attack.clear_attack_goal(state)
          print(string.format(TAG .. " t=%d REFUEL: base at (%d,%d) can't supply us (arm=%d sh=%d mn=%d), replanning",
                now, state.goal.mx or 0, state.goal.my or 0,
                info.base.armour or 0, info.base.shells or 0, info.base.mines or 0))
        elseif C.REFUEL_LOCK_IN then
          refuel_hold = true
        end
      elseif refuel_needed and C.REFUEL_LOCK_IN then
        -- Not on base yet, lock-in mode: hold position-based replans
        refuel_hold = true
      end
    end
    -- Fully stocked at refuel target: replan immediately instead of
    -- waiting for the next timer fire.
    local refuel_done = refuel_complete
    local timer_fire = (now + state.replan_offset) % C.GOAL_REPLAN_INTERVAL == 0
    -- Minimum commitment: suppress timer-based replans shortly after a switch
    local min_commit_met = (now - (state.goal_set_tick or 0)) >= C.GOAL_MIN_COMMIT_TICKS
    local replan = urgent_replan or refuel_done
               or (min_commit_met and not refuel_hold and not state.command_goal and timer_fire)
    -- Only log the replan-decision dump on ticks where something
    -- interesting happens (timer fire, urgent replan, or refuel done).
    -- The vast majority of ticks just print "replan=false" with the
    -- same fields as the previous tick — pure noise.
    if BRAIN_DEBUG_MODE and (replan or timer_fire) then
    end
    if timer_fire and not replan then
      print(string.format(TAG .. " t=%d REPLAN BLOCKED: hold=%s cmd=%s urgent=%s atk_done=%s refuel_done=%s",
        now, tostring(refuel_hold), tostring(state.command_goal ~= nil),
        tostring(urgent_replan), tostring(attack_tank_done), tostring(refuel_done)))
    end

    state.replan_this_tick = replan
    if replan then
      -- Always use cached/partial data — never run the expensive
      -- fill_pool_cache.  The rolling queue refines over ~14 ticks.
      -- At tick 0 the cache is empty so pick_goal returns nil and
      -- the bot idles until the queue fills (~0.3s).
      local t_fp0 = clock_us()
      goals.finalize_pools(state, world, info)
      opt(string.format("  finalize_pools done %.2f ms", (clock_us() - t_fp0) / 1000))
      -- Kick off next cycle's queue immediately so it has ~49 ticks to process
      if now <= 3 then print(TAG .. " tick=" .. now .. " calling build_eval_queue") end
      local t_be0 = clock_us()
      goals.build_eval_queue(state, world, info)
      opt(string.format("  build_eval_queue done %.2f ms", (clock_us() - t_be0) / 1000))
      if now <= 3 then print(TAG .. " tick=" .. now .. " build_eval_queue done, calling pick_goal") end
      metrics.inc("goal_replan")
      local t_pg0 = clock_us()
      local new_goal = goals.pick_goal(state, world, info)
      if BRAIN_PERF_LOG and state._pick_goal_timing then
        for _, entry in ipairs(state._pick_goal_timing) do opt(entry) end
      end
      opt(string.format("  pick_goal done %.2f ms", (clock_us() - t_pg0) / 1000))
      -- Dump all pool_cache winners with costs for diagnosing goal switches
      if BRAIN_DEBUG_MODE and state.pool_cache then
        for pi = 0, 10 do
          local pce = state.pool_cache[pi]
          if pce and pce.goal then
          end
        end
      end
      -- Dump goal_competition entries
      if BRAIN_DEBUG_MODE and state.goal_competition then
        for _, gc in ipairs(state.goal_competition) do
        end
      end
      -- During active pill engage, only allow switching to critical
      -- flee_to_base. Routine refuel_at_base (cost-competition or
      -- Override 2) is NOT a valid preemption — it would skip the
      -- swerve trigger that fires inside the attack_pill substate
      -- machine. Swerve itself is never interrupted, not even by flee.
      local swerving = state.goal.kind == "attack_pill"
                       and state.goal.substate == "swerve"
      -- charge is the final rush — aborting mid-charge is dangerous.
      -- swerve is handled above (fully uninterruptible).
      -- All other attack_pill substates (approach, aim, detree, engage,
      -- post_engage, loiter) are now interruptible so high-priority goals
      -- (dead pill, tank attack, flee) can preempt without waiting.
      local engage_locked = state.goal.kind == "attack_pill"
                            and (state.goal.substate == "charge"
                                 or state.goal.substate == "engage")
      if swerving then
        new_goal = state.goal  -- swerve is never interrupted, not even by flee
      elseif engage_locked and new_goal.kind ~= "flee_to_base"
                             and new_goal.kind ~= "attack_tank"
                             and new_goal.kind ~= "capture_pill" then
        new_goal = state.goal  -- keep current goal
      end
      -- Compare on (kind, mx, my, target_id). Without target_id in
      -- the comparison, a hostile pill that gets captured + replaced
      -- at the same tile (different id) would keep the OLD goal's
      -- _shield_scan / _wall_build_* state, which was computed against
      -- a pill that no longer exists. target_id mismatch forces the
      -- full goal-change path (cooldown + history + new state.goal).
      if new_goal.kind ~= state.goal.kind
         or new_goal.mx ~= state.goal.mx
         or new_goal.my ~= state.goal.my
         or (new_goal.target_id and state.goal.target_id
             and new_goal.target_id ~= state.goal.target_id) then
        local old_kind = state.goal.kind
        local old_id   = state.goal.target_id
        -- Record abandoned goal on cooldown (prevent oscillation)
        if old_kind ~= "none" then
          local cd_key = old_kind .. ":" .. (state.goal.mx or 0) .. "," .. (state.goal.my or 0)
          state.goal_cooldowns[cd_key] = now + C.GOAL_ABANDON_COOLDOWN
        end
        -- Append the NEW goal to the rolling history (oscillation detection).
        -- The history records every distinct goal change; goal_selection
        -- counts occurrences and applies an exponential penalty so a goal
        -- that keeps reappearing gets harder to pick each time.
        if new_goal.kind ~= "none" then
          local hist = state.goal_history
          hist[#hist + 1] = {
            kind = new_goal.kind,
            mx   = new_goal.mx or 0,
            my   = new_goal.my or 0,
            tick = now,
          }
          while #hist > C.GOAL_HISTORY_SIZE do
            table.remove(hist, 1)
          end
        end
        -- Enforce initial substate if the goal doesn't already have one
        if not new_goal.substate or new_goal.substate == "-" then
          new_goal.substate = INITIAL_SUBSTATE[new_goal.kind]
        end
        state.goal = new_goal
        state.goal_set_tick = now
        state.pf.status = "idle"
        state.pf_fail_logged = false
        state.pf_fail_count = 0
        local rp = state._real_print or print
        local old_label = old_id and string.format("%s(%d)", old_kind, old_id) or old_kind
        local new_label = new_goal.target_id and string.format("%s(%d)", new_goal.kind, new_goal.target_id) or new_goal.kind
        rp(string.format(TAG .. " t=%d GOAL CHANGE: %s -> %s dest=(%d,%d) sub=%s cpill=%d arm=%d sh=%d tr=%d name=%s",
              now, old_label, new_label, new_goal.mx, new_goal.my,
              tostring(new_goal.substate or "-"),
              info.carried_pills or 0, info.armour, info.shells, info.trees,
              state.player_name))
        log.event("goal", string.format("%s->%s@%d,%d", old_label, new_label, new_goal.mx, new_goal.my))
      elseif (new_goal.kind == "attack_pill" or new_goal.kind == "pill_place")
             and state.goal.substate then
        -- Same target, same kind, mid-substate: keep state.goal table
        -- intact rather than swap to new_goal. new_goal is essentially
        -- a re-affirmation from the cost evaluator and carries only
        -- the {kind, mx, my, target_id} core — all the substate
        -- machinery (substate, engage_tick, _shield_scan, standoff_*,
        -- _wall_build_*, _gather_*, _finetune_*, etc.) lives on
        -- state.goal and would be wiped by an unconditional swap.
        --
        -- Previously this branch had ~100 lines of hand-written
        -- "new_goal.X = state.goal.X" preservation copies — every
        -- new field added to the goal table needed a corresponding
        -- line or it'd silently drop after the next tick's swap.
        -- Inverting the pattern (keep state.goal, ignore new_goal)
        -- closes that maintenance hole. State.goal_cost is read but
        -- never assigned anywhere in the codebase, so there's nothing
        -- cost-related to carry across.
      -- (bpc_pill block removed — unified into attack_pill above)
      end
    end

    -- Broadcast target claim to allies (brain-to-brain coordination)
    if not send_msg then
      local gk = state.goal.kind
      local gid = state.goal.target_id
      if gid and gid >= 0 then
        if gk == "attack_pill" or gk == "capture_pill"
           or gk == "defend_pill" or gk == "repair_pill" then
          send_msg = comms.format_pill_claim(gid, state.goal_cost or 0)
          msg_dest = 0xFFFF  -- broadcast to all
        elseif gk == "capture_base" or gk == "attack_base" then
          send_msg = comms.format_base_claim(gid, state.goal_cost or 0)
          msg_dest = 0xFFFF
        end
      end
    end

    -- Auto-expire: A* says we arrived or path impossible
    if state.pf.status == "failed" then
      if state.goal.kind == "explore" then
        local gk = U.mkey(state.goal.mx, state.goal.my)
        state.visited[gk] = true
        print(string.format(TAG .. " t=%d ARRIVED/FAILED explore (%d,%d) -- marking visited",
              now, state.goal.mx, state.goal.my))
        attack.clear_attack_goal(state)
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
        -- Retry a few times before giving up — the situation may change
        state.pf_fail_count = (state.pf_fail_count or 0) + 1
        if state.pf_fail_count >= 3 then
          if not state.pf_fail_logged then
            state.pf_fail_logged = true
            print(string.format(TAG .. " t=%d PF FAILED for %s dest=(%d,%d) -- tank at (%d,%d), blocking",
                  now, state.goal.kind, state.goal.mx, state.goal.my, cur_mx, cur_my))
            log.event("pf_failed", string.format("%s@%d,%d", state.goal.kind, state.goal.mx, state.goal.my))
          end
          -- Block this destination so goal selection picks something else
          local bk = U.mkey(state.goal.mx, state.goal.my)
          state.blocked[bk] = now + 600
          attack.clear_attack_goal(state)
          state.pf_fail_count = 0
        else
          -- Retry: reset pf to idle so it tries again next tick
          state.pf.status = "idle"
        end
      end
    end
  end

  -- Per-tick attack substate machine (runs every tick, not just on replan)
  local t_as0 = clock_us()
  attack.update_attack_substate(state.goal, state, world, info)
  local t_as1 = clock_us()
  opt(string.format("  attack_substate done %.2f ms", (t_as1 - t_as0) / 1000))
  if t_as1 - t_as0 > 500 then
    opt.append("optimize.log", string.format(
      "  [as] SLOW sub=%s total=%.3f ms",
      tostring(state._attack_substate_name), (t_as1 - t_as0) / 1000))
  end

  -- Pickup-path overlay: magenta polylines for every attack_pill (pool 6)
  -- cache entry that has a stored spot→pill A* path. Independent of which
  -- goal is active so you can see paths for every candidate the planner
  -- has scored. Toggle via "Attack pickup path" in V dialog.
  if BRAIN_DEBUG_MODE and state.cost_cache and viz.is_on("attack_pill_pickup_path") then
    for k, entry in pairs(state.cost_cache) do
      if entry._pickup_path and #entry._pickup_path >= 2
         and string.sub(k, 1, 2) == "6:" then
        local pp = entry._pickup_path
        local is_active = state.goal and state.goal.kind == "attack_pill"
                          and ("6:" .. tostring(state.goal.target_id)) == k
        local a = is_active and 230 or 130
        for i = 1, #pp - 1 do
        end
      end
    end
  end

  -- Compute near-edge aim point before steering (so engage can use it).
  -- Skipped when the shield scan has chosen a specific corner — its
  -- aim_mx/aim_my would otherwise get clobbered every tick by this
  -- pill-edge fallback, and the tank would shoot at the near edge
  -- instead of the protected corner picked by the scan scoring.
  -- Shielded (PPT) attacks must keep the corner the shield scan
  -- chose — that aim is the lane through the protective walls. For
  -- non-PPT (charge), the shielded angle isn't being used (no walls
  -- between us and the pill), so override with the geometrically
  -- earliest-crossing aim instead. The shield-scan aim is stale data
  -- in that case.
  if state.goal.kind == "attack_pill" and state.goal.mx
     and not state.goal._is_ppt then
    -- Aim at the earliest-crossing point of the pill tile from the
    -- tank's current bearing. 16-way (22.5°) snap of pill→tank
    -- direction: corners (NE/SE/SW/NW) for the four diagonal
    -- 22.5°-wide wedges; the other 12 wedges (N/S/E/W and their
    -- ±22.5° neighbors) aim at the near edge midpoint. Result:
    -- the shell hits the pill tile at the closest possible point
    -- along the firing ray for the tank's current angle. 1-gu
    -- inward inset for drift safety. Skipped when shield scan
    -- picked a specific corner.
    -- Use the planned STANDOFF position (where the tank will end up
    -- when firing) as the bearing reference, falling back to the
    -- live tank position only if no standoff is set yet (very first
    -- ticks of plan_position). Without this, aim is computed from
    -- wherever the tank happens to be RIGHT NOW — which during
    -- approach is far off the firing-position bearing — so the
    -- chosen corner can be wrong by 22.5° or more by the time the
    -- tank arrives at the standoff.
    local twx, twy
    if state.goal.standoff_fx and state.goal.standoff_fy then
      twx = state.goal.standoff_fx
      twy = state.goal.standoff_fy
    elseif state.goal.standoff_mx and state.goal.standoff_my then
      twx = state.goal.standoff_mx + 0.5
      twy = state.goal.standoff_my + 0.5
    else
      twx = info.tankx / 256.0
      twy = info.tanky / 256.0
    end
    local pmx_tile = state.goal.mx
    local pmy_tile = state.goal.my
    local pcx = pmx_tile + 0.5
    local pcy = pmy_tile + 0.5
    -- Bolo bradian bearing from PILL to TANK (0=N, 64=E, 128=S, 192=W).
    -- Snap to the nearest 16-direction (22.5° = 16 brads each).
    local dir = U.aim_at_f(pcx, pcy, twx, twy)
    local d16 = math.floor(dir / 16.0 + 0.5) % 16
    local INSET = 16 / 256.0
    local L = pmx_tile + INSET           -- left x
    local R = pmx_tile + 1 - INSET       -- right x
    local T = pmy_tile + INSET           -- top y
    local B = pmy_tile + 1 - INSET       -- bottom y
    -- Per-wedge (col, row) lookup. Indices match dir16:
    -- 0=N, 1=NNE, 2=NE, 3=ENE, 4=E, 5=ESE, 6=SE, 7=SSE,
    -- 8=S, 9=SSW, 10=SW, 11=WSW, 12=W, 13=WNW, 14=NW, 15=NNW
    local AIM_X = { pcx, pcx, R,   R,   R,   R,   R,   pcx,
                    pcx, pcx, L,   L,   L,   L,   L,   pcx }
    local AIM_Y = { T,   T,   T,   pcy, pcy, pcy, B,   B,
                    B,   B,   B,   pcy, pcy, pcy, T,   T   }
    state.goal.aim_mx = AIM_X[d16 + 1]
    state.goal.aim_my = AIM_Y[d16 + 1]
  end

  t_goal1 = clock_us()
  metrics.set("us_goals", t_goal1 - t_goal0)
  opt(string.format("goals done %.2f ms", (t_goal1 - t_goal0) / 1000))

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
      local peek = goals.pick_goal(state, world, info, true)  -- quiet: suppress logs
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
  if t_goal1 then
    metrics.set("us_lookahead", t_steer0 - t_goal1)
    opt(string.format("lookahead done %.2f ms", (t_steer0 - t_goal1) / 1000))
  end
  local keys, taps = steer.steer(state, world, info, state.goal)
  local t_steer1 = clock_us()
  metrics.set("us_steer", t_steer1 - t_steer0)
  opt(string.format("steer done %.2f ms", (t_steer1 - t_steer0) / 1000))
  if t_steer1 - t_steer0 > 5000 then
    opt.append("optimize.log", string.format(
      "  [steer] SLOW %.2f ms goal=%s sub=%s",
      (t_steer1 - t_steer0) / 1000,
      tostring(state.goal and state.goal.kind),
      tostring(state.goal and state.goal.substate)))
  end

  -- Always-on crosshairs (drawn after steering so they show every tick)
  -- Yellow crosshairs: ALWAYS on
  if BRAIN_DEBUG_MODE and viz.is_on("tank_aim_marker") then
    local ax, ay = U.crosshair_at(info.tankx, info.tanky, info.direction, 7.0)
    local shooting = (keys & KEY_SHOOT) ~= 0
    local cg = shooting and 0 or 255
  end

  -- Target crosshairs + range circles when attacking

  -- Wounded pill marker: highlights the pill we should be finishing
  -- (state.wounded_pill, set by post_engage refuel + swerve completion).
  -- Shows three things at the pill location:
  --   - pulsing red ring → eye-catching at the world position
  --   - "WOUNDED hp=N" label → current HP read live from world.pills
  --     (so we can see it ticking down as in-flight shells land) and
  --     the bonus multiplier on this pill (always 0.30x — the existing
  --     wounded discount in attack_pill_adjustments)
  --   - "other pills x M.MM" label → the live finish_other multiplier
  --     applied to every OTHER pill take this tick. Mirrors the math
  --     in attack_pill_adjustments so the user can verify what the
  --     pool grid will show. Greys out when the multiplier is 1.0
  --     (HP > threshold or time decay finished).
  -- Drawn every tick state.wounded_pill is set; cleared by the
  -- 500-tick expiry in the housekeeping block above.
  if BRAIN_DEBUG_MODE and state.wounded_pill and viz.is_on("wounded_pill_marker") then
    local wp = state.wounded_pill
    local wp_now = wp.id and world.pills and world.pills[wp.id] or nil
    local wp_hp  = wp_now and wp_now.health or wp.hp or 0
    local wpx, wpy = wp.mx + 0.5, wp.my + 0.5
    -- Pulse rate slows as the marker ages out — visual countdown so
    -- you can see at a glance whether the wounded effects are about
    -- to expire. Fresh: ~30-tick cycle (0.21 rad/tick). Fully
    -- decayed: ~120-tick cycle (0.05 rad/tick), barely throbbing.
    local age         = (state.tick or 0) - (wp.tick or 0)
    local time_factor = math.max(0, 1.0 - age / (C.WOUNDED_FINISH_DECAY_TICKS or 500))
    local pulse_speed = 0.05 + 0.16 * time_factor   -- 0.21 fresh -> 0.05 expired
    local pulse = 0.5 + 0.35 * (0.5 + 0.5 * math.sin(now * pulse_speed))
    -- Header line shows the in-pool 0.30x AND the cross-goal commit
    -- discount (decays alongside finish_other; 1.0x once expired).
    local commit_mult = 1.0 - (1.0 - (C.WOUNDED_COMMIT_DISCOUNT or 0.5)) * time_factor
    -- Live "finish_other" multiplier for OTHER pill takes.
    local thresh = C.WOUNDED_FINISH_THRESHOLD or 10
    local mult, mr, mg, mb
    if wp_hp > 0 and wp_hp <= thresh then
      local hp_factor   = (thresh - wp_hp) / thresh
      local peak_mult   = C.WOUNDED_FINISH_OTHER_PENALTY or 3.0
      mult = 1.0 + (peak_mult - 1.0) * hp_factor * time_factor
      if mult > 1.001 then
        mr, mg, mb = 255, 200, 80
      else
        mr, mg, mb = 150, 150, 150
      end
    else
      mult = 1.0
      mr, mg, mb = 150, 150, 150
    end
  end
  local t_psv_wounded = clock_us()
  opt(string.format("  crosshairs+wounded viz done %.2f ms", (t_psv_wounded - t_steer1) / 1000))

  -- Per-pill self_dr labels for pool 6 (attack_pill). Shows the
  -- self-danger reduction the eval queue computed for each pill
  -- candidate this tick — the discount subtracted from spot+combat
  -- cost equal to (pill's own danger contribution along the spot
  -- path) × (1 - hp/15). Higher values = more aggressive closing
  -- in on a low-HP pill that would otherwise scare us off via its
  -- own anger contribution to the danger grid.
  --
  -- Source: state.cost_cache, populated in goals.lua's eval queue.
  -- Each pool-6 entry has _self_dr stashed alongside its position.
  -- We render every entry whose value is non-zero; tiles with no
  -- discount stay unannotated.
  if BRAIN_DEBUG_MODE and state.cost_cache and viz.is_on("pool6_self_dr") then
    for _, e in pairs(state.cost_cache) do
      if e._p == 6 and (e._self_dr or 0) > 0
         and e._mx and e._my then
        local px, py = e._mx + 0.5, e._my + 0.5
        -- Color ramps with magnitude: faint blue at small discounts,
        -- bright cyan at large ones. Cap perception around 200 cost-
        -- units (typical pool-6 spot cost is in low hundreds).
        local mag    = math.min(1.0, e._self_dr / 200.0)
        local r      = math.floor(60 + 30  * mag)
        local g      = math.floor(180 + 60 * mag)
        local b      = math.floor(220 + 35 * mag)
      end
    end
  end
  local t_psv_self_dr = clock_us()
  opt(string.format("  pool6 self_dr labels done %.2f ms", (t_psv_self_dr - t_psv_wounded) / 1000))

  -- Shift+click pill inspect overlay (computed once on click, toggle off/on to refresh)
  if BRAIN_DEBUG_MODE and state.inspect_pill and viz.is_on("inspect_pill") then
    local ip = state.inspect_pill
    local ipill = nil
    for id, p in pairs(world.pills) do
      if p.mx == ip.mx and p.my == ip.my then ipill = p; break end
    end
    if ipill and (ipill.owner == "hostile" or ipill.owner == "neutral") and ipill.health > 0 then
      -- Compute on first display only; stored in inspect_pill table
      if not ip._spots then
        local _, spots = attack.evaluate_pill_difficulty(ipill, world, true, nil, state.phase, state)
        ip._spots = spots or {}
      end
      local spots = ip._spots
      local ipmx, ipmy = ip.mx + 0.5, ip.my + 0.5
      -- Range and standoff circles
      local safe_r = C.ATTACK_SAFE_RADIUS
      if spots then
        for _, s in ipairs(spots) do
          local cr, cg = s.has_los and 0 or 200, s.has_los and 200 or 0
          if s.has_los then
            local sr, sg = (s.total_score <= C.ATTACK_DANGER_THRESHOLD) and 0 or 255,
                           (s.total_score <= C.ATTACK_DANGER_THRESHOLD) and 200 or 165
            if s.maneuver_tiles then
              for _, t in ipairs(s.maneuver_tiles) do
                -- Show coverage count on each maneuver tile
                local cov = threat.coverage_at(t.x, t.y)
                if cov > 0 then
                  local cr, cg = 255, 255
                  if cov > 1 then cr, cg = 255, 0 end  -- red if crossfire
                end
              end
            end
            -- Draw maneuver ellipse outline with line segments
            do
              local edx = ip.mx + 0.5 - s.cx
              local edy = ip.my + 0.5 - s.cy
              local elen = math.sqrt(edx * edx + edy * edy)
              if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
              local ux, uy = edx / elen, edy / elen
              local vx, vy = -uy, ux
              local rl, rs = safe_r, safe_r / 3.0
              local segs = 24
              local px, py
              for i = 0, segs do
                local a = (i / segs) * 2 * math.pi
                local eu = math.cos(a) * rl
                local ev = math.sin(a) * rs
                local nx = s.cx + eu * ux + ev * vx
                local ny = s.cy + eu * uy + ev * vy
                if px then
                  local er, eg = s.total_score < 10 and 0 or 255, s.total_score < 10 and 200 or 165
                end
                px, py = nx, ny
              end
            end
            if s.total_score < 900 then
              local label = string.format("A%.0f+B%.0f+D%.0f+E%.0f=%.0f",
                s.score_a, s.score_b, s.score_d or 0, s.score_e or 0, s.total_score)
            end
          end
        end
      end
      do
        -- Legend
        local lx, ly = ip.mx + 12, ip.my - 6
      end
    else
      -- Pill gone or captured — clear inspect
      state.inspect_pill = nil
    end
  end
  local t_psv_inspect = clock_us()
  opt(string.format("  inspect_pill done %.2f ms (active=%s)",
    (t_psv_inspect - t_psv_self_dr) / 1000, tostring(state.inspect_pill ~= nil)))

  -- Opportunistic tank shot: fire at enemy tanks while doing other things.
  -- Only if not already in tank combat and not shooting at something else.
  if state.goal.kind ~= "attack_tank"
     and (keys & KEY_SHOOT) == 0 and (taps & KEY_SHOOT) == 0
     and info.shells > C.SHELL_RESERVE
     and not info.inboat then
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
  opt(string.format("  oppshot+nav-debug done %.2f ms", (clock_us() - t_psv_inspect) / 1000))

  -- Builder: set mode from current goal, then decide what to build/farm
  local t_build0 = clock_us()
  -- Draw persistent wsim kill/damage paths every tick
  -- Draw attack_tank detection/precondition overlays (navy blue)

  -- Base shield visualization: show wall target, pill source, and blocking line
  if BRAIN_DEBUG_MODE and state._base_shield_viz and viz.is_on("base_shield_viz") then
    local bsv = state._base_shield_viz
    -- Only show for 50 ticks after trigger (5 seconds)
    if now - (bsv.tick or 0) < 50 then
      -- Wall target: cyan filled square
      -- Base: green outline
      -- Pill source: red circle
      -- Line from pill to base (blocked by wall)
      -- Line from base to wall target (LGM path)
      -- Label
    else
      state._base_shield_viz = nil
    end
  end

  -- Coverage grid visualization (E key toggle)
  -- Shows how many hostile/neutral pills can fire on each tile (+1 per pill).
  -- Green=1, gradient to red=5+. Circle outlines show each pill's stamp radius.
  if BRAIN_DEBUG_MODE and _G._SHOW_COVERAGE then
    if viz.is_on("coverage_grid") then
      for k = 0, 65535 do
        local cov = na_threat.cov_grid_at(k)
        if cov > 0 then
          local tx = k % 256
          local ty = k // 256
          local t = math.min((cov - 1) / 4, 1.0)  -- 1=green, 5+=red
          local cr = math.floor(255 * t)
          local cg = math.floor(255 * (1 - t))
        end
      end
    end
    if viz.is_on("pill_threat_overlay") then
      -- Draw stamp radius circle for each hostile/neutral pill
      for _, pm in pairs(world.pills) do
        if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
        end
      end
    end
  end
  metrics.set("us_post_steer_viz", t_build0 - t_steer1)
  opt(string.format("post-steer viz done %.2f ms", (t_build0 - t_steer1) / 1000))
  builder.set_mode(state, world, info, state.goal)
  local t_build_setmode = clock_us()
  opt(string.format("  builder.set_mode done %.2f ms", (t_build_setmode - t_build0) / 1000))
  local build_cmd = builder.decide(state, world, info, now)
  local t_build1 = clock_us()
  opt(string.format("  builder.decide done %.2f ms", (t_build1 - t_build_setmode) / 1000))
  metrics.set("us_builder", t_build1 - t_build0)
  opt(string.format("builder done %.2f ms", (t_build1 - t_build0) / 1000))
  -- Track what kind of action was most recently dispatched so steering can
  -- decide whether to pace the tank while the LGM is moving.
  if build_cmd and info.man_status == C.LGM_INTANK then
    state.builder.last_action = build_cmd.action
  end

  local t_pbh_start = t_build1
  -- LGM state overlays (connected to actual decision state)
  local t_pbh_lgm = clock_us()
  opt(string.format("  LGM/stuck HUD done %.2f ms", (t_pbh_lgm - t_pbh_start) / 1000))

  -- Blocked destinations (show on map)
  if BRAIN_DEBUG_MODE and state.blocked and viz.is_on("blocked_tiles") then
    for k, expires in pairs(state.blocked) do
      if expires > (state.tick or 0) then
        local bx = k % 256
        local by = math.floor(k / 256)
      end
    end
  end

  local t_pbh_blocked = clock_us()
  opt(string.format("  blocked-tiles viz done %.2f ms", (t_pbh_blocked - t_pbh_lgm) / 1000))

  -- Pill reposition overlay: mark badly-positioned friendly pills
  if BRAIN_DEBUG_MODE and C.PILL_REPOSITION_ENABLED and viz.is_on("pill_reposition_marker") then
    for pid, p in pairs(world.pills) do
      if p.owner == "friendly" and p.health > 0 then
        -- Quick badness check (same logic as eval_reposition_pill)
        local nearest_bd = math.huge
        for _, b in pairs(world.bases) do
          if b.owner == "friendly" then
            local bd = U.mdist(p.mx, p.my, b.mx, b.my)
            if bd < nearest_bd then nearest_bd = bd end
          end
        end
        if nearest_bd > C.PILL_REPOSITION_ORPHAN_DIST then
          -- Orphaned pill: orange outline
        end
      end
    end
  end

  local t_pbh_repos = clock_us()
  opt(string.format("  pill-reposition viz done %.2f ms", (t_pbh_repos - t_pbh_blocked) / 1000))

  -- Deep sea bait pill overlay: mark dead pills on known deep sea
  if BRAIN_DEBUG_MODE and state.perc and state.perc.deepsea_pill_ids and viz.is_on("bait_pill_marker") then
    for pid, _ in pairs(state.perc.deepsea_pill_ids) do
      local p = world.pills[pid]
      if p then
      end
    end
  end

  local t_pbh_bait = clock_us()
  opt(string.format("  bait-pill viz done %.2f ms", (t_pbh_bait - t_pbh_repos) / 1000))

  -- Friendly pill barrier overlay: mark friendly pills used as shields
  if BRAIN_DEBUG_MODE and state.goal and
     (state.goal.kind == "attack_pill" or state.goal.kind == "attack_pill")
     and viz.is_on("friendly_pill_shield") then
    local gmx, gmy = state.goal.mx, state.goal.my
    local smx = state.goal.standoff_mx or (info.tankx >> 8)
    local smy = state.goal.standoff_my or (info.tanky >> 8)
    for _, fp in pairs(world.pills) do
      if fp.owner == "friendly" and fp.health > 0 then
        local d_fp_target = U.mdist(fp.mx, fp.my, gmx, gmy)
        local d_fp_us = U.mdist(fp.mx, fp.my, smx, smy)
        local d_total = U.mdist(smx, smy, gmx, gmy)
        if d_fp_target < d_total and d_fp_us < d_total and d_fp_target >= 1 then
          -- This pill is between us and the target: shield icon
        end
      end
    end
  end

  local t_pbh_barrier = clock_us()
  opt(string.format("  friendly-pill-barrier viz done %.2f ms", (t_pbh_barrier - t_pbh_bait) / 1000))

  -- Allied LGM protection overlay: mark allied LGM positions
  if BRAIN_DEBUG_MODE and state.perc and state.perc.allied_lgm_positions and viz.is_on("ally_lgm_marker") then
    for _, alm in ipairs(state.perc.allied_lgm_positions) do
    end
  end

  local t_pbh_ally = clock_us()
  opt(string.format("  ally-LGM viz done %.2f ms", (t_pbh_ally - t_pbh_barrier) / 1000))

  -- Log this tick
  log.log_tick(state, info, state.goal, keys, taps, build_cmd)
  opt(string.format("  log.log_tick done %.2f ms", (clock_us() - t_pbh_ally) / 1000))

  -- Total tick time and worst-case tracking
  local t_end = clock_us()
  local us_total = t_end - t0
  metrics.set("us_post_build_hud", t_end - t_build1)
  opt(string.format("post-build HUD done %.2f ms", (t_end - t_build1) / 1000))
  opt(string.format("TICK TOTAL %.2f ms", us_total / 1000))
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


  -- Label all pills and bases with their IDs (centered on tile)
  if BRAIN_DEBUG_MODE and viz.is_on("pill_id_label") then
    local t_label0 = clock_us()
    for id, p in pairs(world.pills) do
    end
    for id, b in pairs(world.bases) do
    end
    opt(string.format("  pill+base id labels done %.2f ms", (clock_us() - t_label0) / 1000))
  end

  -- Debugger: end trace capture
  if dbg.is_tracing() then
    dbg.end_trace(Brain.think)
  end

  -- HUD: arrow key indicators in bottom-right corner. Each arrow has
  -- three states:
  --   off  (dim grey)   — neither held nor tapped this tick
  --   tap  (yellow)     — single-frame nudge in `taps`; the engine's
  --                       slow-start ramps it to ~1/8 the held rate so
  --                       this is the brain's fine-aim mode
  --   hold (green)      — continuous turn in `keys`; full turn rate

  -- Flush print2 log for this tick
  opt(string.format("END tick=%d total=%.2f ms", now, (clock_us() - t_tick_start) / 1000))
  opt.flush()

  -- Print2 watchdog: every 50 ticks, sanity-check that flushes are
  -- actually happening when the flag is on. The user wants HARD CRASHES
  -- on silent print2 failures, so error() out if the lag is too high.
  if _G._PRINT2_ENABLED and (now % 50) == 0 and now > 10 then
    local d = print2.diagnostic()
    local lag = now - (d.last_successful_flush_tick or -1)
    if d.last_successful_flush_tick < 0 then
      local msg = string.format(TAG ..
        " [print2 watchdog] FATAL tick=%d: NEVER FLUSHED (fails=%d open=%s)",
        now, d.fail_count, tostring(d.file_open))
      print(msg)
      error(msg, 0)
    elseif lag > 100 then
      local msg = string.format(TAG ..
        " [print2 watchdog] FATAL tick=%d: last flush was %d ticks ago (consec fails=%d open=%s)",
        now, lag, d.consecutive_failures, tostring(d.file_open))
      print(msg)
      error(msg, 0)
    end
  end

  -- Startup hold: give the incremental goal eval queue a chance to warm
  -- up before we commit to moving. Without this the bot picks a goal from
  -- a near-empty cache on tick 1 and often flips direction a few ticks
  -- later once better candidates finish evaluating.
  if now <= C.STARTUP_HOLD_TICKS then
    keys, taps, build_cmd = 0, 0, -1
  end

  -- Tick-info HUD (top-left, just below BrainTest's tick/think box).
  -- C-side already renders tick + think_ms; keep this line tight
  -- with the brain-only bits: replan countdown, phase, current goal.

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
-- ON_CLICK — called from BrainTest when user left-clicks a map tile
-- mx, my: map tile coordinates
-- mods: {shift=bool, ctrl=bool, alt=bool}
-- =========================================================================

-- (manual_active and manual_keys declared at top of file)

function Brain.debug_info()
  local g = state.goal or {}
  local i = state._last_info or {}
  local tmx = (i.tankx or 0) >> 8
  local tmy = (i.tanky or 0) >> 8
  local smx = g.standoff_mx or -1
  local smy = g.standoff_my or -1
  local amx = g.approach_mx or -1
  local amy = g.approach_my or -1
  local swx, swy = smx * 256 + 128, smy * 256 + 128
  local sdist = math.sqrt((i.tankx - swx)^2 + (i.tanky - swy)^2)
  return string.format("sub=%s tank=(%d,%d) standoff=(%d,%d) approach=(%d,%d) sdist=%.0f",
    tostring(g.substate), tmx, tmy, smx, smy, amx, amy, sdist)
end

function Brain.set_manual_mode(on)
  manual_active = (on == 1)
  if not manual_active then manual_keys = 0 end
end

-- Manual key dispatcher. The host (BrainTest) sends ROLE NAMES
-- ("forward", "backward", "left", "right", "shoot", "lay_mine")
-- it derived from the user's Bolo key bindings, so the brain
-- doesn't need to care which physical keys are configured. Legacy
-- letter names ("w", "s", "a", "d", "space", "lshift") are kept
-- as aliases so any other host that sends literal SDL key letters
-- still works.
function Brain.manual_key(name, down)
  local map = {
    forward  = KEY_FASTER,
    backward = KEY_SLOWER,
    left     = KEY_TURNLEFT,
    right    = KEY_TURNRIGHT,
    shoot    = KEY_SHOOT,
    lay_mine = KEY_DROPMINE,
    -- Legacy literal-key aliases.
    w = KEY_FASTER, s = KEY_SLOWER, a = KEY_TURNLEFT, d = KEY_TURNRIGHT,
    space = KEY_SHOOT, lshift = KEY_DROPMINE,
  }
  local bit = map[name]
  if bit then
    if down then
      manual_keys = manual_keys | bit
    else
      manual_keys = manual_keys & ~bit
    end
  end
end

function Brain.on_click(mx, my, mods)
  mods = mods or {}
  local rp = state._real_print or print
  -- Debug to file since console may not be visible
  local f = io.open("click_lua.log", "a")
  if f then
    local pill_count = 0
    if world and world.pills then
      for _ in pairs(world.pills) do pill_count = pill_count + 1 end
    end
    f:write(string.format("on_click: (%d,%d) shift=%s ctrl=%s tick=%d pills=%d\n",
            mx, my, tostring(mods.shift), tostring(mods.ctrl), state.tick or -1, pill_count))
    f:close()
  end
  rp(string.format(TAG .. " CLICK: (%d,%d) shift=%s ctrl=%s", mx, my, tostring(mods.shift), tostring(mods.ctrl)))

  if mods.shift then
    -- Toggle pill inspect overlay
    local f2 = io.open("click_lua.log", "a")
    if state.inspect_pill and state.inspect_pill.mx == mx and state.inspect_pill.my == my then
      state.inspect_pill = nil
      if f2 then f2:write("  -> cleared inspect\n"); f2:close() end
      rp(TAG .. " CLICK: cleared pill inspect")
    else
      local found = false
      for id, p in pairs(world.pills) do
        if f2 then f2:write(string.format("  pill %d at (%d,%d) owner=%s hp=%d\n", id, p.mx, p.my, p.owner, p.health)) end
        if p.mx == mx and p.my == my then
          state.inspect_pill = { id = id, mx = mx, my = my }
          if f2 then f2:write(string.format("  -> MATCH pill %d\n", id)) end
          rp(string.format(TAG .. " CLICK: inspecting pill %d at (%d,%d)", id, mx, my))
          found = true
          break
        end
      end
      if not found then
        state.inspect_pill = nil
        if f2 then f2:write("  -> no pill found at click location\n") end
        rp(string.format(TAG .. " CLICK: no pill at (%d,%d)", mx, my))
      end
      if f2 then f2:close() end
    end
  elseif mods.ctrl then
    -- Force attack pill
    for id, p in pairs(world.pills) do
      if p.mx == mx and p.my == my then
        if (p.owner == "hostile" or p.owner == "neutral") and p.health > 0 then
          state.goal = {
            kind = "attack_pill",
            mx = p.mx, my = p.my,
            target_id = id,
            substate = "plan_position",
            wx = U.m2w(p.mx), wy = U.m2w(p.my),
          }
          state.command_goal = { kind = "attack_pill", id = id, mx = p.mx, my = p.my }
          state.goal_set_tick = state.tick
          state.pf.status = "idle"
          rp(string.format(TAG .. " CLICK: forced attack_pill on pill %d at (%d,%d)", id, mx, my))
        else
          rp(string.format(TAG .. " CLICK: pill %d at (%d,%d) is %s hp=%d", id, mx, my, p.owner, p.health))
        end
        return
      end
    end
    rp(string.format(TAG .. " CLICK: no pill at (%d,%d)", mx, my))
  else
    -- Plain click: stash for the hud_click_cost overlay. Per-tick
    -- think emits the actual text so the panel persists until the
    -- next click. The C-side already drew the A* path overlay
    -- itself (computeClickPath); we just supply the rich-info
    -- label that the old C-side panel used to draw.
    state.click_inspect = { mx = mx, my = my, tick = state.tick or 0 }
  end
end

-- =========================================================================
-- CLOSE
-- =========================================================================

function Brain.close(info)
  print(TAG .. " closed after " .. state.tick .. " ticks")
  log.dump_world(world)
  log.close()
  opt.close()
  metrics.close_files()

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

-- Register Brain.think with debugger so it can snapshot upvalues from C
dbg.register_think(Brain.think)

return Brain
