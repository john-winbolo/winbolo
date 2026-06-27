-- =========================================================================
-- GoalHunter/init.lua — Brain entry point (open/think/close/settings)
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
-- TAG-prefixed brain chatter (goal shifts, stuck warnings, command
-- echoes) is wrapped in `if BRAIN_DEBUG_MODE then print(...) end` at
-- each call site. The strip removes those blocks from opt/ entirely,
-- so --opt pays zero cost — no function call, no string concat, no
-- print. Bare print(...) calls (no TAG / no BRAIN_DEBUG_MODE wrap)
-- are reserved for things that MUST surface in production: load-time
-- errors, fatal asserts, and one-shot startup banners.
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
local kill_lgm = require("kill_lgm")
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
local ally_state = require("ally_state")
ally_state.init()
local pill_table = require("pill_table")
local lgm_registry = require("lgm_registry")
lgm_registry.init()

local Brain = {}

-- Manual control state (must be before Brain.think so it captures the upvalue)
local manual_active = false
local manual_keys = 0

local AUTOSTART = true
local ENABLE_LOGGING = false

-- Shot-path safety check (init.lua version). Same logic as
-- steering.lua's shot_path_clear. Blocks on walls, pillboxes,
-- hostile bases. Forests and enemy tanks are OK.
local function _shot_path_clear_init(info, world, target_wx, target_wy, target_mx, target_my)
  local tank_positions = {}
  if info.objects then
    for _, ob in ipairs(info.objects) do
      if ob.type == OBJECT_TANK then  -- 0; type 2 is OBJECT_PILLBOX, not a tank
        tank_positions[#tank_positions + 1] = {
          wx = ob.x, wy = ob.y,
          player_num = ob.idnum or 255,
        }
      end
    end
  end
  local tiles
  if #tank_positions > 0 then
    tiles = cpf.simulate_shot_with_tanks(info.tankx, info.tanky,
                                         target_wx, target_wy,
                                         cpf.SHOT_TANK, 0,
                                         tank_positions,
                                         info.player_number or 255)
  else
    tiles = cpf.simulate_shot(info.tankx, info.tanky,
                              target_wx, target_wy,
                              cpf.SHOT_TANK, 0)
  end
  if not tiles then return true end
  local origin_mx = info.tankx >> 8
  local origin_my = info.tanky >> 8
  local do_viz = BRAIN_DEBUG_MODE and viz.is_on("shell_hit_dot")
  local blocked = false
  local block_reason = nil
  local block_mx, block_my = nil, nil
  for ti, st in ipairs(tiles) do
    if st.hit_type and st.hit_type == 1 then
      local tank_is_target = (st.mx == target_mx and st.my == target_my)
      if do_viz then
        local hit_tank = nil
        for _, tp in ipairs(tank_positions) do
          if tp.player_num == st.hit_id then hit_tank = tp; break end
        end
        if hit_tank then
          local tcx = hit_tank.wx / 256.0
          local tcy = hit_tank.wy / 256.0
          local hr = tank_is_target and 0 or 255
          local hg = tank_is_target and 255 or 0
          viz.rect("shell_hit_dot", tcx - 0.5, tcy - 0.5,
                   tcx + 0.5, tcy + 0.5, hr, hg, 0, 150)
          viz.text("shell_hit_dot", tcx, tcy - 0.6,
                   string.format("#%d tank#%d %s", ti, st.hit_id or 0,
                     tank_is_target and "HIT" or "BLOCKED"),
                   "center", hr, hg, 0, 255, 0.6)
        end
      end
      if not tank_is_target then
        blocked = true
        block_reason = string.format("allied_tank#%d", st.hit_id or 0)
        block_mx, block_my = st.mx, st.my
      end
      break
    end
    if st.mx == target_mx and st.my == target_my then
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
        blocked = true
        block_reason = "wall"
        block_mx, block_my = st.mx, st.my
        if do_viz then
          viz.text("shell_hit_dot", st.mx + 0.5, st.my + 0.5,
                   tostring(ti), "center", 255, 0, 0, 200, 0.5)
        end
        break
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
      if bentry and bentry.base and bentry.base.owner == "hostile" then
        blocked = true
        block_reason = "hostile_base"
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
             info.tankx / 256.0, info.tanky / 256.0,
             target_wx / 256.0, target_wy / 256.0,
             blocked and 255 or 100, blocked and 50 or 255, 50,
             blocked and 180 or 80)
    if viz.detail_circle then
      local did = "shot_path_init"
      local hdr = string.format("Shot path: from=(%d,%d) to=(%d,%d) result=%s",
        origin_mx, origin_my, target_mx, target_my, blocked and "BLOCKED" or "CLEAR")
      local mid_wx = (info.tankx + target_wx) / 2 / 256.0
      local mid_wy = (info.tanky + target_wy) / 2 / 256.0
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
  return not blocked
end

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
    if BRAIN_DEBUG_MODE then print(string.format(TAG .. " COMMAND: '%s'", value)) end
    local cmd = cmds.parse(value)
    if cmd then
      local reply = cmds.execute(cmd, state, world)
      if reply then
        if BRAIN_DEBUG_MODE then print(TAG .. " REPLY: " .. reply) end
      end
    else
      if BRAIN_DEBUG_MODE then print(TAG .. " unknown command: " .. value) end
    end
  elseif id == "auto_explore" then
    state.auto_explore = value
    if BRAIN_DEBUG_MODE then print(string.format(TAG .. " auto_explore = %s", tostring(value))) end
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

-- Capacity tier panel data. JSON shape consumed by tier_control.cpp.
-- Includes the active tier, current override (if any), all 10 tier
-- definitions for the legend, and the live values that hash to the
-- active tier for highlighting.
function Brain.get_capacity_state_json()
  local lvls = C.BRAIN_CAPACITY_LEVELS
  local tier = (state and state._capacity_tier) or 10
  local ovr  = _G._BT_TIER_OVERRIDE
  local last_ms = (_G.brain and _G.brain.lastThinkMs) or 0
  local tgt_ms  = (_G.brain and _G.brain.targetMs)    or 0
  local sm      = (state and state._capacity_ratio_ewma) or 0

  -- Build per-tier rows + tier_ms snapshot.
  local function lvl(t)
    local L = lvls[t] or {}
    return string.format(
      '{"tier":%d,"dij_short":%d,"dij_long":%d,"scan_step":%d,"pp_spread":%d,"ttl_mult":%.2f,"eval_iv":%d,"wsim":%s,"place_r":%d,"tank_step":%d,"ms":%s}',
      t, L.dij_short or 0, L.dij_long or 0, L.scan_step or 0,
      L.pp_spread or 1, L.ttl_mult or 1.0,
      L.eval_iv or 1,
      (L.wsim == nil and "null") or (L.wsim == false and "false") or tostring(L.wsim),
      L.place_r or 0, L.tank_step or 0,
      (state._tier_ms and state._tier_ms[t]) and string.format("%.2f", state._tier_ms[t]) or "null")
  end
  local rows = {}
  for t = 1, 10 do rows[#rows + 1] = lvl(t) end

  -- Per-tick section timing breakdown for the panel's stacked bar.
  -- Populated by optimize.lua at flush() — only available when
  -- BRAIN_PROFILE is on (i.e. --perf-log was passed). Shape:
  --   { { name, ms, subs: [{name, ms}, ...] }, ... }
  local secs = (opt and opt.last_sections) or {}
  local sec_parts = {}
  for _, s in ipairs(secs) do
    local sub_parts = {}
    if s.subs then
      for _, sb in ipairs(s.subs) do
        -- Third level: subsubs nested under each sub. Serialise them
        -- the same way so the panel's renderer can draw three levels.
        local ss_parts = {}
        if sb.subs then
          for _, ss in ipairs(sb.subs) do
            ss_parts[#ss_parts + 1] = string.format('{"name":%q,"ms":%.3f}',
              ss.name or "?", ss.ms or 0)
          end
        end
        sub_parts[#sub_parts + 1] = string.format(
          '{"name":%q,"ms":%.3f,"subs":[%s]}',
          sb.name or "?", sb.ms or 0, table.concat(ss_parts, ","))
      end
    end
    sec_parts[#sec_parts + 1] = string.format(
      '{"name":%q,"ms":%.3f,"subs":[%s]}',
      s.name or "?", s.ms or 0, table.concat(sub_parts, ","))
  end

  -- think_total_ms: real wall-clock between the two performance
  -- markers at the top and bottom of Brain.think. Same timer (clock_us)
  -- the section "done" emits use, so sum-of-sections and this number
  -- share a single measurement basis. Null when --perf-log is off
  -- (markers aren't captured to save the function-call cost).
  local think_total_ms_str = "null"
  if state._think_start_us and state._think_end_us then
    think_total_ms_str = string.format("%.3f",
      (state._think_end_us - state._think_start_us) / 1000)
  end

  return string.format(
    '{"tier":%d,"override":%s,"last_ms":%.2f,"target_ms":%.2f,"ratio_ewma":%.2f,"think_total_ms":%s,"levels":[%s],"sections":[%s]}',
    tier,
    (type(ovr) == "number") and tostring(math.floor(ovr)) or "null",
    last_ms, tgt_ms, sm,
    think_total_ms_str,
    table.concat(rows, ","),
    table.concat(sec_parts, ","))
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
  print2("=== BRAIN STARTUP === player=", info.player_number,
         " name=", tostring(info.player_name),
         " debug_session_dir=", tostring(_G.DEBUG_SESSION_DIR))
  local t_open0 = clock_us()
  -- Diagnostic: emit a SELF_DR line per pool-6 candidate per replan
  -- showing raw / subtracted / c_reduction / manual_reduction so we can
  -- verify the target-pill danger subtraction in pool_6's spot_cost.
  -- BRAIN_DEBUG_MODE-gated so it's stripped from opt/.
  -- Switch to Lua 5.4 generational GC. Brain ticks allocate lots of
  -- short-lived tables (closures, per-tick scratch); generational keeps
  -- minor collections cheap and frequent. minor=10 fires minor passes
  -- when heap grows 10% over the last minor (2x default frequency, each
  -- pass tiny). DO NOT add later setpause/setstepmul calls — those flip
  -- the collector back to incremental in 5.4.
  collectgarbage("generational", 10, 100)
  -- Register every viz_id with the host's V dialog. No-op when
  -- braintest_viz_register isn't bound (e.g. running under WinBolo
  -- client); the brain still emits overlay commands but they're
  -- never displayed.
  viz.register_all()
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
    -- Capacity tier control: bespoke renderer with up/down arrows that
    -- push _G._BT_TIER_OVERRIDE back into the bot to lock its tier.
    -- Shortcut Y → own SDL window. (T was the natural choice but it's
    -- reserved by BrainTest core; Y is free and adjacent on QWERTY.)
    -- Type name kept short (<= 10 chars) so PANEL_REG_TYPE_MAX (24) fits
    -- "GoalHunter:" + type without truncating off the trailing letter.
    braintest_panel_register("Capacity tiers", "tier_ctrl",
      "return brain.get_capacity_state_json()",
      { shortcut = "Y" })
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
  _G._BRAIN_SELF_PN   = info.player_number
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

  -- /info state broadcast scratch buffers.
  --   broadcast_state_info      — scratch hash any code can write to during
  --                                the tick (or reset to empty). End-of-tick
  --                                comparator builds the wire message from
  --                                this.
  --   last_broadcasted_state_info — authoritative copy of what we last sent
  --                                to allies. Compared against
  --                                broadcast_state_info to detect change.
  --   last_broadcast_state_tick — tick of the last broadcast; drives the
  --                                30 s (1500 tick @ 50 Hz) heartbeat.
  state.broadcast_state_info       = {}
  state.last_broadcasted_state_info = {}
  state.last_broadcast_state_tick   = 0

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
  state.frontier     = expl.new_frontier()
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

  -- Load precomputed shield stamp cache (C binary via gh_shield.load).
  shield.load_stamp_bin()
  if gh_shield then
    gh_shield.configure_scan({
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
    gh_shield.set_lgm_func(cpf.lgm_travel_ticks_map)
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
  -- JSONL behavior logger setup. Gate `BRAIN_LOG_JSON or BRAIN_DEBUG_MODE`
  -- with LOG_JSON FIRST so the strip's `if BRAIN_DEBUG_MODE then` prefix
  -- doesn't match — opt/ keeps this block, and at runtime BRAIN_LOG_JSON
  -- (set from --log-json) decides. Dev mode auto-sets both true.
  local log_fname = nil
  if BRAIN_LOG_JSON or BRAIN_DEBUG_MODE then
    if info.player_number == 0 then
      if _G._JSONL_LOGGER_ENABLED then
        log_fname = log.make_filename("player0")
      end
    elseif ENABLE_LOGGING or state.debug_log then
      log_fname = log.make_filename("brain_p" .. info.player_number)
    end
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

  -- Open perf-metrics files for the debug bot (player 0, or any bot
  -- whose debug_log flag is set). Independent of the JSONL logger flag —
  -- these files are small and drive scripts/analyze_metrics.py.
  -- Gated on BRAIN_PROFILE_LOG (file-write toggle) — without --profile-log
  -- (or dev mode), never opens these files at all.
  if BRAIN_PROFILE_LOG and (info.player_number == 0 or state.debug_log) then
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
            viz.line("shot_tile_grid", smx + f, smy, smx + f, smy + 1, gr, gg, gb, ga)
            viz.line("shot_tile_grid", smx, smy + f, smx + 1, smy + f, gr, gg, gb, ga)
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
          viz.circle("shell_hit_dot", cxt, cyt, 3 / 256.0, r, g, b, 255, true)
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
          viz.text("shell_hitbox", tx2 + 0.2, ty1 - 0.05,
            string.format("d=%d tip=(%.4f,%.4f) x=%.4f y=%.4f render=(%.4f,%.4f)",
              dir16, tip_x_tile, tip_y_tile,
              x_tile, y_tile,
              render_x_tile, render_y_tile),
            "topleft", 255, 220, 120, 255, 0.25)
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
    viz.rect("tank_hitbox", htx1, hty1, htx2, hty2, 255, 255, 0, 200, false)
  end
end

-- =========================================================================
-- THINK
-- =========================================================================

function Brain.think(info)
  -- ── PERFORMANCE MARKER: START ──
  -- First instruction in Brain.think when --perf-log is on: capture
  -- a real tick-start clock so the Y panel can compute total_ms as
  -- (think_end_us - think_start_us) using the SAME timer (clock_us)
  -- every named section's "done" emit uses. Gated so non-perf runs
  -- don't pay the function-call cost.
  if _G.BRAIN_PROFILE then
    state._think_start_us = clock_us()
  end
  -- Capture wall clock at think entry; the matching exit-time
  -- snapshot at the bottom drives the top-left tick-info HUD.
  local _think_t0 = os.clock()
  state.tick = state.tick + 1
  state._last_info = info
  local now  = state.tick

  -- Cautious mode: per-tick boolean.  When true, danger / threat
  -- terms across cost formulas get multiplied by
  -- C.CAUTIOUS_MODE_MULT (5×) so the bot biases hard toward
  -- safer routes / targets.  Recomputed every tick — flips off
  -- automatically when its triggers stop firing (no manual reset).
  --
  -- Current triggers (logical OR):
  --   * We're carrying any pill in the tank (>=1) — pills are valuable
  --     cargo we shouldn't lose to a stray pill shot, so bias toward
  --     safe routes/targets while we hold one. (Previously also required
  --     a dead LGM; relaxed so even a single carried pill triggers it.)
  --
  -- Add more triggers here as use cases arise.  Stays per-tick (no
  -- sticky latch) so the mode lifts the instant conditions clear.
  do
    local _carrying = (info.carried_pills or 0) > 0
    state.cautious_mode = _carrying or false
  end

  -- LGM registry: self slot updated every tick from info.man_*.
  -- A status transition (in_tank ↔ ground ↔ dead) sets the
  -- pending_lgm_broadcast flag so the periodic /info state broadcast
  -- block fires immediately rather than waiting for the next heartbeat
  -- — allies need fast notification of "LGM back" so they stop yielding
  -- to our cooldown.
  local _lgm_self_pn = info.player_number
  if _lgm_self_pn ~= nil then
    local _lgm_self_mx = (info.man_x or 0) >> 8
    local _lgm_self_my = (info.man_y or 0) >> 8
    local _lgm_prev_slot = lgm_registry.get(_lgm_self_pn)
    local _lgm_prev_status = _lgm_prev_slot and _lgm_prev_slot.status or "unknown"
    local _lgm_transitioned = lgm_registry.update_self(
      _lgm_self_pn, info.man_status or 0,
      _lgm_self_mx, _lgm_self_my, now)
    if _lgm_transitioned then
      state.pending_lgm_broadcast = true
      -- Set lgm_back flag for one broadcast when we transition FROM
      -- dead to anything else (ground / in_tank).  Allies use this to
      -- drop the dead-cooldown bookkeeping immediately rather than
      -- waiting for respawn_eta.  Cleared by the broadcast block.
      local _lgm_new_status = lgm_registry.get(_lgm_self_pn).status
      if _lgm_prev_status == "dead" and _lgm_new_status ~= "dead" then
        state.pending_lgm_back = true
      end
    end
  end
  -- At tick 1 the engine has populated info.tankx/y with the bot's
  -- real spawn position. Brain.open is too early — info isn't
  -- populated yet there. Emit a BOT_START marker so log readers can
  -- correlate bot index ↔ map quadrant.
  if BRAIN_DEBUG_MODE and now == 1 then
    print2(string.format(
      "BOT_START player_number=%s name=%s tank=(%.1f,%.1f) tile=(%d,%d)",
      tostring(info.player_number),
      tostring(info.player_name),
      (info.tankx or 0) / 256.0, (info.tanky or 0) / 256.0,
      (info.tankx or 0) >> 8, (info.tanky or 0) >> 8))
  end

  -- Open the optimize.log section timer at the EARLIEST possible point
  -- so prelude work (capacity tier calc, debug-mode viz refresh, the
  -- startup-mode block, etc.) is included in the per-section sum. The
  -- optimize.lua tick_start was previously set ~200 lines later, which
  -- left the prelude as unaccounted gap in the time-bar.
  opt.set_tick(now)
  -- Real tick-start clock used by every later phase timer (t_early,
  -- t_world, etc. all forward to this value). Captures from the actual
  -- top of Brain.think, not after the prelude work.
  local t_tick_start = clock_us()

  -- ── Capacity tier ──
  -- The host publishes brain.lastThinkMs (previous tick's wall ms),
  -- brain.targetMs (per-bot CPU budget for this tick), and
  -- brain.wasKilled (true if previous tick was force-killed) onto the
  -- global `brain` table each tick.
  --
  -- Decision algorithm:
  --   1. Record per-tier ms history. state._tier_ms[T] is an EWMA of how
  --      long this bot's thinks cost at tier T given current world load.
  --      This corroborates raises: we only raise if next-tier history
  --      shows it actually fits in budget.
  --   2. If wasKilled: drop CAPACITY_KILLED_CUT tiers and mark the prior
  --      tier as "burned" for CAPACITY_KILLED_AVOID ticks so the raise
  --      path doesn't immediately retry it.
  --   3. Otherwise classify smoothed ratio = think_ms / target_ms:
  --        > 1.50 → drop 2 tiers
  --        > 1.20 → drop 1 tier
  --        < 0.40 → raise 1 tier (free, ignores corroboration)
  --        < 0.70 → raise 1 tier (only if next tier's history fits)
  do
    local last_ms = (_G.brain and _G.brain.lastThinkMs) or 0
    -- Test-override: when set, treat this as our per-bot budget regardless
    -- of what the host published. Lets us force-engage tiers for testing.
    local tgt_ms  = C.CAPACITY_FORCED_TARGET_MS
                or (_G.brain and _G.brain.targetMs)
                or 0
    local killed  = (_G.brain and _G.brain.wasKilled)   or false
    local prev_tier = state._capacity_tier or C.CAPACITY_DEFAULT_TIER

    -- Per-tier ms history (records the tier we just ran at).
    state._tier_ms = state._tier_ms or {}
    if last_ms > 0 then
      local prior = state._tier_ms[prev_tier]
      state._tier_ms[prev_tier] = prior
        and (prior * (1 - C.CAPACITY_EWMA_ALPHA) + last_ms * C.CAPACITY_EWMA_ALPHA)
        or  last_ms
    end

    state._tier_killed = state._tier_killed or {}

    local cur = prev_tier
    if killed then
      cur = math.max(1, prev_tier - C.CAPACITY_KILLED_CUT)
      state._tier_killed[prev_tier] = state.tick
    elseif tgt_ms > 0 and last_ms > 0 then
      local ratio = last_ms / tgt_ms
      state._capacity_ratio_ewma = (state._capacity_ratio_ewma or ratio)
                                 * (1 - C.CAPACITY_EWMA_ALPHA)
                                 + ratio * C.CAPACITY_EWMA_ALPHA
      local sm = state._capacity_ratio_ewma
      if sm > C.CAPACITY_DROP_BIG_RATIO and cur > 1 then
        cur = math.max(1, cur - 2)
      elseif sm > C.CAPACITY_DROP_RATIO and cur > 1 then
        cur = cur - 1
      elseif cur < 10 then
        local next_t = cur + 1
        local k = state._tier_killed[next_t]
        local recently_killed = k and (state.tick - k) < C.CAPACITY_KILLED_AVOID
        if not recently_killed then
          if sm < C.CAPACITY_RAISE_FREE_RATIO then
            cur = next_t  -- big headroom: raise even without corroboration
          elseif sm < C.CAPACITY_RAISE_RATIO then
            local hist = state._tier_ms[next_t]
            if not hist or hist < tgt_ms * C.CAPACITY_RAISE_HEADROOM then
              cur = next_t  -- raise only if history shows next tier fits
            end
          end
        end
      end
    end

    -- BrainTest panel override: when _G._BT_TIER_OVERRIDE is set
    -- (integer 1..10), force the capacity tier to that value regardless
    -- of the dynamic algorithm. Lets the user lock the bot into a
    -- specific tier from the T-window for testing.
    local _ovr = _G._BT_TIER_OVERRIDE
    if type(_ovr) == "number" and _ovr >= 1 and _ovr <= 10 then
      cur = math.floor(_ovr)
    end

    state._capacity_tier = cur
    state._capacity = C.BRAIN_CAPACITY_LEVELS[cur]

    -- Tier-shift logging: every change emits a line to optimize.log so
    -- we can verify the algorithm + corroborate per-tier ms history.
    -- Format: tick, prev->new, ratio, last_ms, target_ms, killed flag,
    -- and the per-tier ms history snapshot.
    -- Tier shifts are infrequent but the line build does several
    -- string.formats; gate on BRAIN_PROFILE_LOG so --opt without
    -- profile-log skips this entirely. The console print also
    -- becomes BRAIN_DEBUG_MODE-gated below.
    if cur ~= prev_tier and BRAIN_PROFILE_LOG then
      local hist_parts = {}
      for t = 10, 1, -1 do
        local v = state._tier_ms and state._tier_ms[t]
        hist_parts[#hist_parts + 1] = v and string.format("%d:%.1f", t, v) or string.format("%d:--", t)
      end
      local line = string.format(
        "[capacity] t=%d  tier %d->%d  ratio=%.2f  last=%.2fms  target=%.2fms  killed=%s  hist=[%s]",
        state.tick or 0, prev_tier, cur,
        state._capacity_ratio_ewma or -1, last_ms, tgt_ms, tostring(killed),
        table.concat(hist_parts, " "))
      opt.append("optimize.log", "  " .. line)
      if BRAIN_DEBUG_MODE then
        print(TAG .. " " .. line)  -- BrainTest live console feedback only
      end
    end
  end

  -- Snapshot V-dialog enabled state once per tick so call-site
  -- `if viz.is_on("foo") then ... end` guards reduce to a single
  -- table lookup. Skips per-call string concat + :upper() + assert_id.
  if BRAIN_DEBUG_MODE then viz.refresh() end

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
  if BRAIN_DEBUG_MODE then
    print2.set_bot(info.player_number or 0)
    print2.set_tick(now)
    print2("BEGIN bot tick=", now, " state.goal.kind = ", state.goal.kind, ", state.goal.substate = ", tostring(state.goal.substate))
    -- A* logging is gated on _G._ENABLE_ASTAR_LOG (default off) — the
    -- per-tile cost_to trace is verbose enough to noticeably slow the
    -- sim. Set `_G._ENABLE_ASTAR_LOG = true` (e.g. from a launcher
    -- script or debug console) before the first tick to opt in.
    if _G._ENABLE_ASTAR_LOG then
      if cpf.astar_log_set_tick then cpf.astar_log_set_tick(now) end
      if not _G._ASTAR_LOG_OPENED and cpf.astar_log_enable then
        _G._ASTAR_LOG_OPENED = true
        if _G.DEBUG_SESSION_DIR then
          cpf.astar_log_enable(_G.DEBUG_SESSION_DIR .. "/astar.log")
          print2("ASTAR_LOG enabled at ", _G.DEBUG_SESSION_DIR, "/astar.log")
        end
      end
    end
  end
  -- (opt.set_tick already fired at the top of think; just emit the
  -- BEGIN marker here.)
  opt("BEGIN tick=", now, " goal=", state.goal.kind, " sub=", tostring(state.goal.substate))
  -- t_tick_start is the real top-of-think clock (set above, near
  -- opt.set_tick). t_early is its alias used by the early-viz/HUD timer
  -- so the first section measures from the actual tick start.
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
          if BRAIN_PROFILE_LOG then
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
    dbg.snapshot_sources("../brains/GoalHunter")
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
    if BRAIN_DEBUG_MODE then
      if overlay_clear then overlay_clear() end
      -- Same hitbox overlay as the autonomous path so shells the user
      -- shoots while M-driving still get the orange dot + cyan tile.
      draw_shell_hitbox_viz(info)
      -- Track our own fired shells in manual mode too, so the cyan/green/
      -- magenta sim circles render while the human is driving.
      shot_tracker.update(info, now)
      shot_tracker.draw_overlay(now)
      viz.hud_text("hud_manual_control", 10, 68, ">>> MANUAL CONTROL <<<", "topleft", 255, 50, 50)
      viz.hud_text("hud_manual_control", 10, 10, ">>> MANUAL CONTROL <<<", "topright", 255, 50, 50)
      -- Show what C side is sending (manual_keys set by Brain.set_manual_keys)
      local mk = manual_keys or 0
      local parts = {}
      if (mk & KEY_FASTER)    ~= 0 then parts[#parts+1] = "FWD" end
      if (mk & KEY_SLOWER)    ~= 0 then parts[#parts+1] = "BACK" end
      if (mk & KEY_TURNLEFT)  ~= 0 then parts[#parts+1] = "LEFT" end
      if (mk & KEY_TURNRIGHT) ~= 0 then parts[#parts+1] = "RIGHT" end
      if (mk & KEY_SHOOT)     ~= 0 then parts[#parts+1] = "SHOOT" end
      if (mk & KEY_DROPMINE)  ~= 0 then parts[#parts+1] = "MINE" end
      if (mk & KEY_MORERANGE) ~= 0 then parts[#parts+1] = "GUN+" end
      if (mk & KEY_LESSRANGE) ~= 0 then parts[#parts+1] = "GUN-" end
      local key_str = #parts > 0 and table.concat(parts, " ") or "(none)"
      viz.hud_text("hud_manual_control", 10, 80, "Keys: " .. key_str, "topleft", 255, 255, 100)
      viz.hud_text("hud_manual_control", 10, 92, string.format("spd=%d dir=%d arm=%d sh=%d",
        info.speed, info.direction, info.armour, info.shells), "topleft", 200, 200, 200)
      -- HUD: tank stats (offset up so the kill-attempt indicator can sit
      -- under it without overlap on shorter window heights).
      local y = 90
      local bld_str, bld_r, bld_g, bld_b = hud_builder_status(info, state)
      viz.hud_text("hud_resources", 10, y,      string.format("Shells %d/%d", info.shells, 40), "bottomleft", 255, 255, 100)
      viz.hud_text("hud_resources", 10, y + 12, string.format("Builder%s", bld_str), "bottomleft", bld_r, bld_g, bld_b)
      viz.hud_text("hud_resources", 10, y + 24, string.format("Mines  %d/%d", info.mines,  40), "bottomleft", 255, 180, 50)
      viz.hud_text("hud_resources", 10, y + 36, string.format("Armour %d/%d", info.armour, 40), "bottomleft", 100, 255, 100)
      viz.hud_text("hud_resources", 10, y + 48, string.format("Trees  %d/%d", info.trees,  40), "bottomleft", 80, 200, 80)
      viz.hud_text("hud_resources", 10, y + 60, string.format("Speed  %d", info.speed), "bottomleft", 200, 200, 255)
      viz.hud_text("hud_resources", 10, y + 72, string.format("Boat   %s", info.inboat and "YES" or "no"),
        "bottomleft", info.inboat and 100 or 200, info.inboat and 200 or 200, 255)
      -- Still draw crosshairs at the actual shell-landing distance.
      -- sightLen is in half-tiles (see constants.lua GUNSIGHT_MAX
      -- comment): shell travels sightLen/2 map tiles.  At max
      -- gunrange=14 the shell lands 7 tiles away; at min gunrange=2
      -- it lands 1 tile away.
      local twx, twy = info.tankx / 256.0, info.tanky / 256.0
      local gun_range = (info.gunrange or 14) / 2.0
      local aim_wx = twx + U.bsin_f(info.direction) * gun_range
      local aim_wy = twy - U.bcos_f(info.direction) * gun_range
      viz.line("tank_aim_marker", aim_wx - 0.3, aim_wy, aim_wx + 0.3, aim_wy, 255, 255, 0, 150)
      viz.line("tank_aim_marker", aim_wx, aim_wy - 0.3, aim_wx, aim_wy + 0.3, 255, 255, 0, 150)
    end -- BRAIN_DEBUG_MODE (manual mode)
    return { holdkeys = manual_keys, tapkeys = 0, build = -1, wantallies = info.allies, messagedest = 0, sendmessage = "" }
  end



  -- Debug overlay
  if BRAIN_DEBUG_MODE then
    if overlay_clear then overlay_clear() end
    -- viz_detail registry rebuilds from scratch each think tick. Brain
    -- code that wants to surface clickable map primitives in the D
    -- inspector dialog calls viz.detail_rect/circle/text_anchor +
    -- viz.detail_text after this clear.
    viz.detail_clear()
    viz.hud_text("hud_version", 10, 4, "GoalHunter " .. BOT_VERSION, "bottomright", 150, 150, 150)

    -- SHELL_HITBOX_VIZ + own-tank hitbox: drawn after overlay_clear so
    -- it survives the wipe. Same call as manual mode runs above so the
    -- two paths can't drift.
    draw_shell_hitbox_viz(info)
  end -- BRAIN_DEBUG_MODE

  -- Optional debug log — keeps the per-tick shell positions in
  -- hitboxes.log for one-off bug hunts. Rendering is handled by
  -- draw_shell_hitbox_viz above; this block ONLY writes the log.
  if BRAIN_DEBUG_MODE and viz.is_on("hitbox_logs") then
  local t_hb0 = clock_us()
  do
    local hb_log = io.open("hitboxes.log", "a")
    if hb_log and info.objects then
      local shell_count = 0
      for i = 1, #info.objects do
        local ob = info.objects[i]
        if ob.type == 1 then
          shell_count = shell_count + 1
          local smx = ob.x >> 8
          local smy = ob.y >> 8
          local px  = ob.x >> 4
          local py  = ob.y >> 4
          hb_log:write(string.format(
            "  t=%d SHELL wx=%d wy=%d tile=(%d,%d) px=(%d,%d)\n",
            state.tick, ob.x, ob.y, smx, smy, px, py))
        end
      end
      if shell_count > 0 then
        hb_log:write(string.format("t=%d shell_count=%d\n", state.tick, shell_count))
      end
    end
    if hb_log then hb_log:close() end
    opt(string.format("  hitboxes.log write done %.2f ms", (clock_us() - t_hb0) / 1000))

    -- Tank position readout. Three lines next to the tank: world
    -- units (0..65535), game pixels (wu/16), and tile + sub-tile
    -- fraction (wu/256). Plus the chosen standoff's wu coords next
    -- to the standoff marker when the shield scan is active.
    if info.tankx and info.tanky then
      local twx_t = info.tankx / 256.0
      local twy_t = info.tanky / 256.0
      local tile_x = math.floor(info.tankx / 256)
      local tile_y = math.floor(info.tanky / 256)
      local sub_x  = info.tankx - tile_x * 256
      local sub_y  = info.tanky - tile_y * 256
      local lx = twx_t + 1.0
      local ly = twy_t + 1.5
      viz.text("tank_position", lx, ly + 0.0,
        string.format("wu=(%d,%d)", info.tankx, info.tanky),
        "topleft", 200, 220, 255, 255, 0.4)
      viz.text("tank_position", lx, ly + 0.4,
        string.format("gu=(%.2f,%.2f)", info.tankx / 16.0, info.tanky / 16.0),
        "topleft", 200, 220, 255, 255, 0.4)
      viz.text("tank_position", lx, ly + 0.8,
        string.format("tile=(%d+%d/256, %d+%d/256)", tile_x, sub_x, tile_y, sub_y),
        "topleft", 200, 220, 255, 255, 0.4)

      -- Heading readout — integer info.direction (the brain-visible
      -- coarse angle) plus the float info.tank_angle when present
      -- (engine's actual sub-brad value, used for shot trajectory
      -- sims). Cardinal letter helps when the number is 200ish and
      -- you can't remember which way that's pointing.
      do
        local dir_int = info.direction or 0
        local dir_f   = info.tank_angle  -- may be nil on older snapshots
        local card
        if     dir_int <  16 or dir_int > 240 then card = "N"
        elseif dir_int <  48                  then card = "NE"
        elseif dir_int <  80                  then card = "E"
        elseif dir_int < 112                  then card = "SE"
        elseif dir_int < 144                  then card = "S"
        elseif dir_int < 176                  then card = "SW"
        elseif dir_int < 208                  then card = "W"
        else                                       card = "NW"
        end
        local fl = info.tank_first_left  or 0
        local fr = info.tank_first_right or 0
        local ramp_str = ""
        if fl > 0 or fr > 0 then
          ramp_str = string.format("  ramp_L=%d ramp_R=%d", fl, fr)
        end
        if dir_f then
          viz.text("tank_angle", lx, ly + 1.2,
            string.format("dir=%d (%.3f) %s%s", dir_int, dir_f, card, ramp_str),
            "topleft", 255, 220, 140, 255, 0.4)
        else
          viz.text("tank_angle", lx, ly + 1.2,
            string.format("dir=%d %s%s", dir_int, card, ramp_str),
            "topleft", 255, 220, 140, 255, 0.4)
        end
      end

      local g = state.goal
      if g and g.kind == "attack_pill" and g.standoff_fx and g.standoff_fy then
        local s_wx = math.floor(g.standoff_fx * 256 + 0.5)
        local s_wy = math.floor(g.standoff_fy * 256 + 0.5)
        viz.text("tank_position", g.standoff_fx + 0.5, g.standoff_fy - 1.2,
          string.format("STANDOFF wu=(%d,%d)", s_wx, s_wy),
          "topleft", 200, 255, 200, 255, 0.4)
      end
    end
    -- LGM viz (when "out" = moving, status 2 per lgm.h LGM_BRAIN_MOVING).
    -- Mirrors the shell debug style: blue tile outline + 1/16-tile grid
    -- over the tile, orange filled dot at the exact wu position, and a
    -- small text label showing wu coords.
    if info.man_status == 2 and info.man_x and info.man_y then
      local lmx = info.man_x >> 8
      local lmy = info.man_y >> 8
      if viz.is_on("lgm_tile_grid") then
        local gr, gg, gb, ga = 80, 80, 200, 180
        for i = 0, 16 do
          local f = i / 16.0
          viz.line("lgm_tile_grid", lmx + f, lmy, lmx + f, lmy + 1, gr, gg, gb, ga)
          viz.line("lgm_tile_grid", lmx, lmy + f, lmx + 1, lmy + f, gr, gg, gb, ga)
        end
        viz.rect("lgm_tile_grid", lmx, lmy, lmx + 1, lmy + 1, 120, 120, 255, 255, false)
      end
      -- Orange hit dot centered on LGM's exact (wu) position + the
      -- coord readout, both gated by "LGM hitbox dbg".
      if viz.is_on("lgm_hitbox") then
        local L_DOT_WU = 4
        local L_HALF = L_DOT_WU * 0.5
        viz.rect("lgm_hitbox",
                 (info.man_x - L_HALF) / 256.0, (info.man_y - L_HALF) / 256.0,
                 (info.man_x + L_HALF) / 256.0, (info.man_y + L_HALF) / 256.0,
                 255, 140, 0, 255, true, true)
        local lxt, lxr = info.man_x >> 8, info.man_x & 0xFF
        local lyt, lyr = info.man_y >> 8, info.man_y & 0xFF
        viz.text("lgm_hitbox",
          (info.man_x + 8) / 256.0, (info.man_y - 8) / 256.0,
          string.format("LGM wu=(%d,%d) tile=(%d & %d/256, %d & %d/256)",
            info.man_x, info.man_y, lxt, lxr, lyt, lyr),
          "topleft", 255, 220, 120, 255, 0.25)
      end

      -- Per-factor "would rescue_lgm fire?" debug labels.
      if viz.is_on("lgm_stranded") then
        local f = state._lgm_stranded_factors
        local lx = (info.man_x + 8) / 256.0
        local ly0 = (info.man_y + 8) / 256.0
        local function row(yi, label, triggered)
          local r, g, b
          if triggered then r, g, b = 100, 230, 100   -- green = would fire
          else              r, g, b = 230, 80, 80     -- red = condition not met
          end
          viz.text("lgm_stranded", lx, ly0 + yi * 0.35, label,
                   "topleft", r, g, b, 240, 0.4)
        end
        if f then
          local pf_label
          if f.path_ticks == nil then pf_label = "PATH=skipped"
          elseif f.path_fail        then pf_label = "PATH=FAIL(-1)"
          else pf_label = string.format("PATH=%dt", f.path_ticks)
          end
          row(0, pf_label, f.path_fail)
          row(1, "BUILD_SUPPRESS",   f.build_suppress)
          row(2, "NEARBY_CARVE",     f.nearby_carve)
          row(3, "STRANDED",         state.lgm_stranded == true)
          row(4, string.format("dist=%d", f.lgm_dist or -1), false)
        else
          -- No factor record yet — the rescue check hasn't run for
          -- this LGM trip. Render a placeholder so the user sees the
          -- label area is alive.
          row(0, "PATH=(pending)", false)
        end
      end
    end
    -- Anomaly log: record this tick if an enemy shell is within 128 wu of
    -- any tank at tick-start. Per our collision analysis this should NEVER
    -- happen for still-alive enemy shells — the hit test would have fired
    -- same tick. Writes to intersect.log; empty = no anomalies.
    local t_ix0 = clock_us()
    if info.objects then
      local tanks = {}
      for i = 1, #info.objects do
        local ob = info.objects[i]
        if ob.type == 0 then   -- OBJECT_TANK
          tanks[#tanks + 1] = ob
        end
      end
      -- Include own tank as target too (yellow box is drawn for it).
      if info.tankx and info.tanky then
        tanks[#tanks + 1] = { x = info.tankx, y = info.tanky, info = 0, idnum = info.player_number or 0, _self = true }
      end
      local hits = {}
      for i = 1, #info.objects do
        local ob = info.objects[i]
        if ob.type == 1 then   -- OBJECT_SHOT
          local sinfo = ob.info or 0
          -- SHELLS_BRAIN_FRIENDLY=1, HOSTILE=2, NEUTRAL=3 (see shells.h)
          for _, tk in ipairs(tanks) do
            -- Skip shots that belong to this tank (own shell through own box).
            local sameOwner = false
            if tk._self and sinfo == 1 then sameOwner = true end  -- friendly shell + own tank
            if not sameOwner then
              local dx = math.abs(tk.x - ob.x)
              local dy = math.abs(tk.y - ob.y)
              -- Widened to 256 wu (1/2 tile of margin beyond the 128-wu
               -- hit threshold) so near-misses get logged too for context.
              if dx < 256 and dy < 256 then
                hits[#hits + 1] = string.format(
                  "tick=%d shell@(%d,%d) info=%d  tank@(%d,%d) id=%d self=%s  dx=%d dy=%d",
                  state.tick, ob.x, ob.y, sinfo,
                  tk.x, tk.y, tk.idnum or -1, tostring(tk._self or false), dx, dy)
              end
            end
          end
        end
      end
      if #hits > 0 and BRAIN_DEBUG_MODE and viz.is_on("hitbox_logs") then
        local f = io.open("intersect.log", "a")
        if f then
          for _, h in ipairs(hits) do f:write(h, "\n") end
          f:close()
        end
      end
    end
    opt(string.format("  intersect.log scan done %.2f ms", (clock_us() - t_ix0) / 1000))
  end
  end -- BRAIN_DEBUG_MODE (hitboxes/tank-pos/lgm/intersect debug block)


  -- Adjacent tile highlights
  if BRAIN_DEBUG_MODE and viz.is_on("adjacent_tiles") then
    local tx = math.floor(info.tankx / 256)
    local ty = math.floor(info.tanky / 256)
    local adj = {{tx-1, ty}, {tx+1, ty}, {tx, ty-1}, {tx, ty+1}}
    for _, pos in ipairs(adj) do
      viz.rect("adjacent_tiles", pos[1], pos[2], pos[1]+1, pos[2]+1, 0, 255, 255, 180)
    end
  end

  -- HUD: tank stats in bottom-left (offset up so the kill-attempt
  -- indicator can sit under it without overlap on shorter window heights).
  if BRAIN_DEBUG_MODE and viz.is_on("hud_resources") then
    local y = 90
    local bld_str, bld_r, bld_g, bld_b = hud_builder_status(info, state)
    viz.hud_text("hud_resources", 10, y,      string.format("Shells %d/%d", info.shells, 40), "bottomleft", 255, 255, 100)
    viz.hud_text("hud_resources", 10, y + 12, string.format("Builder%s", bld_str), "bottomleft", bld_r, bld_g, bld_b)
    viz.hud_text("hud_resources", 10, y + 24, string.format("Mines  %d/%d", info.mines,  40), "bottomleft", 255, 180, 50)
    viz.hud_text("hud_resources", 10, y + 36, string.format("Armour %d/%d", info.armour, 40), "bottomleft", 100, 255, 100)
    viz.hud_text("hud_resources", 10, y + 48, string.format("Trees  %d/%d", info.trees,  40), "bottomleft", 80, 200, 80)
    viz.hud_text("hud_resources", 10, y + 60, string.format("Speed  %d", info.speed), "bottomleft", 200, 200, 255)
    viz.hud_text("hud_resources", 10, y + 72, string.format("Boat   %s", info.inboat and "YES" or "no"),
      "bottomleft", info.inboat and 100 or 200, info.inboat and 200 or 200, 255)
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
      viz.hud_text("attack_clear_reason", 10, 220,
        string.format("CLEARED t=%d  %s%s%s",
                      lc.tick or 0, lc.kind_was or "?", sub_str, pos_str),
        "topleft", 255, 120, 80, fade)
      viz.hud_text("attack_clear_reason", 10, 232,
        "  why: " .. (lc.reason or "?"),
        "topleft", 255, 200, 150, fade)
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
    viz.hud_text("hud_replan", 10, 56, string.format("Replan: %d  [%s] %s",
      ticks_left, state.phase or "?",
      state.phase_reason or ""), "topleft", r, g, b)
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
    -- Goal-change trace — one print2 line per change. Lands in
    -- print2_bot<N>.log under the rest of the bot's debug trace. The
    -- outer "if BRAIN_DEBUG_MODE and ..." matches strip.bat's
    -- --strip-block prefix so opt builds drop this entirely.
    if BRAIN_DEBUG_MODE and BRAIN_LOG_GOALS then
      local sub = (g.substate and g.substate ~= "" and g.substate ~= "-") and (" sub=" .. g.substate) or ""
      print2(string.format("GOAL_CHANGE %s%s", goal_str, sub))
    end
    -- HUD draws (gated)
    if BRAIN_DEBUG_MODE then
      local gy = 4
      if viz.is_on("hud_goal") then
        viz.hud_text("hud_goal", 10, gy, goal_str, "topright", 100, 255, 100)
        gy = gy + 10
        if g.substate and g.substate ~= "" and g.substate ~= "-" then
          viz.hud_text("hud_goal", 10, gy, string.format(" sub: %s", g.substate),
            "topright", 180, 180, 180)
          gy = gy + 10
        end
        local pf = state.pf or {}
        viz.hud_text("hud_goal", 10, gy, string.format("PF: %s  age=%d",
          pf.status or "-", pf.age or 0), "topright", 200, 200, 100)
        gy = gy + 10
      else
        -- Skip ahead to keep gy aligned for the candidates block below.
        gy = 4 + 10 + ((g.substate and g.substate ~= "" and g.substate ~= "-") and 10 or 0) + 10
      end
      -- Candidate pool
      local pool = state.last_goal_pool or {}
      if #pool > 0 and viz.is_on("hud_goal_candidates") then
        viz.hud_text("hud_goal_candidates", 10, gy, "-- candidates --", "topright", 140, 140, 140)
        gy = gy + 10
        for i = 1, math.min(#pool, 20) do
          local c = pool[i]
          local prefix = c.winner and ">" or " "
          local cr, cg, cb = 150, 150, 150
          if c.winner then cr, cg, cb = 100, 255, 100 end
          viz.hud_text("hud_goal_candidates", 10, gy, string.format("%s%-28s %5.0f x%.1f",
            prefix, c.desc or "", c.cost or 0, c.phase_weight or 1.0),
            "topright", cr, cg, cb)
          gy = gy + 10
        end
      end
    end -- BRAIN_DEBUG_MODE (hud_goal/candidates)
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

  -- Overlay rebuild: pills/bases stamped as impassable/expensive in the
  -- pathfinder. Moved here (right after danger load) so all downstream
  -- goal evaluation sees fresh overlays — previously ran much later and
  -- A* queries during eval would hit stale impassable stamps from dead pills.
  threat.check_overlay_dirty(world)
  if threat.overlay_dirty then
    threat.rebuild_overlay(world)
    metrics.inc("overlay_reloads")
  else
    metrics.inc("overlay_skips")
  end
  local t_overlay_early = clock_us()
  opt(string.format("  overlay rebuild done %.2f ms (dirty=%s)",
    (t_overlay_early - t_load_danger) / 1000, tostring(not threat.overlay_dirty)))

  -- Ally avoidance: stamp a cost penalty around the pill target of
  -- allied bots that are mid-pill-take so we don't drive through
  -- their combat zone. Uses ally_state goal/substate + target mx/my.
  -- Also stamps a 2-tile-wide firing lane from the ally tank to the
  -- pill. Includes approach now — if their tank is far from the pill
  -- (>12 tiles) the firing-lane stamp is skipped below, so the only
  -- effect from far is the small radius around the pill itself.
  local ALLY_COMBAT_SUBS = {
    approach=true,
    aim=true, charge=true, engage=true, shoot_pill=true, swerve=true,
    in_range_position=true, in_range_aim=true, in_range_aim_pre=true,
    in_range_aim_finetune=true, build_walls=true, detree=true,
  }
  local ALLY_AVOID_RADIUS = 2  -- 5x5 block around the ally tank itself
  local ALLY_AVOID_COST   = C.ALLY_AVOID_COST or 800
  -- Tank-5x5 only stamps when the ally tank is within
  -- TANK_STAMP_NEAR_TILES *euclidean* of either their setup
  -- (approach) point or their standoff point.  Both points are
  -- broadcast in bsi.smx/smy and bsi.ssx/ssy on attack_pill goals.
  -- Tighter than the prior "within 2 of pill" rule and avoids leaving
  -- a breadcrumb trail while the ally is still driving in.
  local TANK_STAMP_NEAR_TILES = 3
  do
    local now_aa = state.tick or 0
    -- Clear previous tick's stamps so a moving ally doesn't leave a
    -- breadcrumb trail of +800 cost behind them.  We stamp each tile
    -- ourselves, so we know exactly which to zero.  Pill/base tiles
    -- are never stamped (skipped at stamp time) so this clear can
    -- safely write 0 without wiping rebuild_overlay's impassable
    -- markers.
    -- All persistent overlay sources we must NOT clobber:
    --   - pill tiles (32767, rebuilt on dirty only)
    --   - hostile-base tiles (~1500, rebuilt on dirty only)
    --   - stuck_blacklist tiles (1500, re-stamped by steering AFTER us)
    -- Skip those on both stamp AND clear so we never zero them and so
    -- our +800 doesn't weaken a stronger pre-existing penalty.
    local stuck_bl = state.stuck_blacklist
    local stamped = state._ally_avoid_stamped
    if stamped then
      for k in pairs(stamped) do
        if not (stuck_bl and stuck_bl[k]) then
          cpf.set_overlay(U.mkey_x(k), U.mkey_y(k), 0)
        end
        stamped[k] = nil
      end
    else
      stamped = {}
      state._ally_avoid_stamped = stamped
    end

    local function stamp(x, y)
      if x < 0 or x > 255 or y < 0 or y > 255 then return end
      local pkey = y * 256 + x
      if world.pill_at and world.pill_at[pkey] then return end
      if world.base_at and world.base_at[pkey] then return end
      local mk = U.mkey(x, y)
      if stuck_bl and stuck_bl[mk] then return end  -- defer to stuck_blacklist
      cpf.set_overlay(x, y, ALLY_AVOID_COST)
      stamped[mk] = true
      if BRAIN_DEBUG_MODE and viz.is_on("ally_avoid_overlay") then
        viz.rect("ally_avoid_overlay", x, y, x + 1, y + 1, 255, 165, 0, 60)
      end
    end

    for ally_pn, slot in ally_state.iter_active(now_aa, 1750) do
      if ally_pn ~= info.player_number then
        local ai = slot.info
        -- Compute the "ally tank within 3 euclidean of either setup
        -- or standoff" gate up front so both the diagnostic overlay
        -- and the stamping block use the same source of truth.
        local pmx = tonumber(ai.mx)
        local pmy = tonumber(ai.my)
        local atmx = tonumber(ai.tx)
        local atmy = tonumber(ai.ty)
        -- Parse the packed "p" field — 8 hex chars,
        -- approach_mx/approach_my/standoff_mx/standoff_my each 2 chars.
        -- Falls back to the legacy 4-key form if a peer is on old code.
        local smx, smy, ssx, ssy
        if ai.p and #ai.p == 8 then
          smx = tonumber(ai.p:sub(1, 2), 16)
          smy = tonumber(ai.p:sub(3, 4), 16)
          ssx = tonumber(ai.p:sub(5, 6), 16)
          ssy = tonumber(ai.p:sub(7, 8), 16)
        else
          smx = tonumber(ai.smx); smy = tonumber(ai.smy)
          ssx = tonumber(ai.ssx); ssy = tonumber(ai.ssy)
        end
        local function edist(x1, y1, x2, y2)
          if not (x1 and y1 and x2 and y2) then return math.huge end
          local ddx = x1 - x2; local ddy = y1 - y2
          return math.sqrt(ddx * ddx + ddy * ddy)
        end
        local d_setup    = edist(atmx, atmy, smx, smy)
        local d_standoff = edist(atmx, atmy, ssx, ssy)
        local d_min      = math.min(d_setup, d_standoff)
        local in_range   = d_min <= TANK_STAMP_NEAR_TILES
        local in_combat_sub = ai.goal == "attack_pill" and ALLY_COMBAT_SUBS[ai.sub] and true or false
        local active        = in_combat_sub and in_range

        -- Diagnostic overlay: always rendered for any attack_pill ally
        -- (on the ally_avoid_overlay layer which defaults on).  Shows
        -- whether the 5x5 stamp is active and, when off, the live
        -- euclidean distance to each of the two activation points.
        if BRAIN_DEBUG_MODE and ai.goal == "attack_pill" and pmx and pmy and atmx and atmy then
          local r, g, b = active and 80 or 255,
                          active and 255 or (in_combat_sub and 200 or 120),
                          80
          local function fmt_d(d)
            return (d == math.huge) and "?" or string.format("%.1f", d)
          end
          local label1 = string.format("p%d BLOCK: %s",
                                       ally_pn, active and "ON" or "OFF")
          local label2 = string.format("sub=%s setup=%s standoff=%s thr=%d",
                                       tostring(ai.sub or "?"),
                                       fmt_d(d_setup), fmt_d(d_standoff),
                                       TANK_STAMP_NEAR_TILES)
          viz.text("ally_avoid_overlay", atmx + 0.5, atmy - 0.8,
                   label1, "center", r, g, b, 255)
          viz.text("ally_avoid_overlay", atmx + 0.5, atmy - 0.35,
                   label2, "center", r, g, b, 230, 0.4)
        end

        if ai.goal == "attack_pill" and ALLY_COMBAT_SUBS[ai.sub] then
          if pmx and pmy and atmx and atmy then
            -- 5x5 tank stamp gated on euclidean ≤ 3 to setup or
            -- standoff (see above).  Stops the breadcrumb trail while
            -- they're still driving in from far away.
            if in_range then
              for dy = -ALLY_AVOID_RADIUS, ALLY_AVOID_RADIUS do
                for dx = -ALLY_AVOID_RADIUS, ALLY_AVOID_RADIUS do
                  stamp(atmx + dx, atmy + dy)
                end
              end
            end
            -- Stamp firing lane: 2-tile-wide line from ally tank to
            -- pill target. Uses broadcast tx/ty for ally position.
            -- (Unchanged — user wants the engage-to-pill lane to stay.)
            if U.mdist(atmx, atmy, pmx, pmy) <= 12 then
              local ldx = pmy - atmy
              local ldy = -(pmx - atmx)
              local llen = math.max(1, math.sqrt(ldx * ldx + ldy * ldy))
              local pnx, pny = ldx / llen, ldy / llen
              U.line_walk(atmx + 0.5, atmy + 0.5, pmx + 0.5, pmy + 0.5,
                function(lx, ly)
                  for w = -1, 1 do
                    stamp(U.mclamp(math.floor(lx + pnx * w + 0.5)),
                          U.mclamp(math.floor(ly + pny * w + 0.5)))
                  end
                end)
            end
          end
        end
      end
    end
  end

  -- Push per-pill contribution maps to the host (BrainTest reads
  -- this for the shift-2 cycle-pill-overlay). Bindings no-op
  -- under non-host runtimes (game client, headless server).
  -- NOTE: clear is done host-side once per tick (BrainTest's
  -- appTickBrain) so multi-bot games don't have one bot wipe
  -- another's entries. We just append from here.
  if pillcontrib_begin_pill and _G._BT_PCONTRIB_NEEDED then
    -- Host pushes _BT_PCONTRIB_NEEDED each tick: true only when the
    -- shift-2 pill_contrib overlay is active OR --record-pcontrib is on.
    -- Everything else (typical debug-mode runs) skips the per-pill push
    -- entirely — saves a measurable chunk of think_ms.
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
  -- Overlay rebuild moved earlier (right after danger load) so goal
  -- evaluation sees fresh data. See the block above t_overlay_early.

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
  if BRAIN_DEBUG_MODE then shot_tracker.draw_overlay(now) end
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
      if BRAIN_DEBUG_MODE then print2(string.format(
        "dij START slate=%d max_cost=%.0f src=(%d,%d) boat=%d allow_boat=%d tick=%d",
        idx, max_cost, tmx, tmy, boat, allow_boat, now)) end
    end

    -- Double-buffer restart: at each interval, snapshot the main slate into
    -- the backup so lookups always have complete coverage, then restart main.
    --
    -- wait_for_done: when true, the schedule-driven restart only fires
    -- once the current main is `done` (full map covered). Used for the
    -- LONG slate, where at low capacity tiers a fresh restart can fail
    -- to reach completion within `interval`. Restarting on schedule
    -- regardless would leave the backup as a partial snapshot — and
    -- the new main also partial — so the bot would never have a
    -- complete cost surface. Holding off the restart until completion
    -- preserves "always-complete backup" at the cost of a longer real
    -- recompute cycle (e.g. ~40s at tier 1). Acceptable for long-range
    -- strategic decisions: relative cost ranking is mostly preserved
    -- under tank movement, and the short slate handles nearby tactical
    -- routing with much fresher data.
    --
    -- Short slate keeps the original "restart on schedule" semantics:
    -- its work fits in the budget at every tier so completion-within-
    -- interval is the normal case, and stale short-range data hurts
    -- tactical decisions much more than stale long-range data.
    local function maybe_copy_and_restart(main_idx, backup_idx, interval,
                                          max_cost, boat, allow_boat,
                                          wait_for_done)
      refresh_slate(main_idx)
      local s = d.slates[main_idx]
      local age = now - s.started_tick
      local schedule_hit = age >= interval
      if wait_for_done and schedule_hit and not s.done then
        -- Holding off: keep stepping the current main until it
        -- finishes, then snapshot + restart on the next pass.
        return
      end
      if not s.active or schedule_hit then
        if s.active then
          local t_cs = clock_us()
          cpf.dijkstra_copy_slate(main_idx, backup_idx)
          opt(string.format("dij COPY %d->%d done %.2f ms",
            main_idx, backup_idx, (clock_us() - t_cs) / 1000))
          refresh_slate(backup_idx)
          if BRAIN_DEBUG_MODE then print2(string.format("dij COPY %d->%d at tick=%d age=%d", main_idx, backup_idx, now, age)) end
        end
        start_slate(main_idx, max_cost, boat, allow_boat)
      end
    end

    -- Short: land-only unless on a boat. Long: always allow boat.
    -- Long passes wait_for_done=true so its backup is ALWAYS a fully-
    -- complete map, even at low tiers where the long search may not
    -- finish within DIJKSTRA_RECOMPUTE_INTERVAL.
    maybe_copy_and_restart(SLATE_SHORT_MAIN, SLATE_SHORT_BACKUP,
                           C.DIJKSTRA_SHORT_INTERVAL, C.DIJKSTRA_SHORT_MAX_COST,
                           in_boat, in_boat, false)
    maybe_copy_and_restart(SLATE_LONG_MAIN, SLATE_LONG_BACKUP,
                           C.DIJKSTRA_RECOMPUTE_INTERVAL, C.DIJKSTRA_MAX_COST,
                           in_boat, 1, true)

    -- Step only the main slates (backups are frozen snapshots, done=true).
    local budget_short = C.DIJKSTRA_SHORT_BUDGET
    local budget_long  = math.max(500, math.ceil(70000 / C.DIJKSTRA_LONG_SPREAD_TICKS))
    -- Capacity tier dij_short / dij_long: cap each slate's per-tick node
    -- expansion budget. Tier 10 → defaults; tier 1 → 100/25 nodes/tick.
    if state._capacity then
      if state._capacity.dij_short and state._capacity.dij_short < budget_short then
        budget_short = state._capacity.dij_short
      end
      if state._capacity.dij_long and state._capacity.dij_long < budget_long then
        budget_long = state._capacity.dij_long
      end
    end
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
          if BRAIN_DEBUG_MODE then print2(string.format(
            "dij DONE slate=%d at_tick=%d (took %d ticks, expanded=%d)",
            idx, now, now - d.slates[idx].started_tick, expanded)) end
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
  local send_msg = nil
  local msg_dest = 0

  if state.send_open_msg then
    send_msg = state.paused and C.BRAIN_NAME .. " loaded (PAUSED — use 'start' to begin)."
                             or C.BRAIN_NAME .. " loaded."
    msg_dest = 1 << state.player_number
    state.send_open_msg = false
  end

  -- Process EVERY incoming chat message this tick. info.messages is
  -- the new multi-message inbox added in the brain inbox C-side change;
  -- the legacy info.message field still works but only surfaces the
  -- first one. Iterating the array lets us see all ally /info traffic
  -- when several bots broadcast on the same tick (previously the bus
  -- silently dropped all but one).
  if info.messages then
    for _, m in ipairs(info.messages) do
      if m.text and m.text ~= "" then
        -- chat_log ring is debug-only (read only by the chat_log_overlay
        -- HUD); skip the ring writes entirely in production. Slate update
        -- via comms.process_message stays unconditional since coordination
        -- logic reads it.
        if BRAIN_DEBUG_MODE then
          ally_state.chat_log_add("in", m.sender, m.text, now)
        end
        comms.process_message(m.sender, m.text, now)

        local cmd = cmds.parse(m.text)
        if cmd then
          if BRAIN_DEBUG_MODE then
            print(string.format(TAG .. " t=%d RECV from player %d: '%s'",
                  now, m.sender, m.text))
          end
          log.event("cmd_recv", m.text)
          local reply = cmds.execute(cmd, state, world)
          if reply and not send_msg then
            send_msg = reply
            msg_dest = 1 << state.player_number
          end
        end
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

  -- Respawn handling. Use clear_attack_goal so any in-progress
  -- attack_pill / PPT state (substate, _shield_scan, _aim_locked,
  -- _wall_build_list, etc.) doesn't leak through into the next
  -- goal selection on the new tank.
  --
  -- Belt-and-suspenders: info.newtank is only true for a SINGLE
  -- tick after respawn (tank.c:463/491). If anything (GC pause, a
  -- long previous Brain.think, a Lua error mid-body) ate that one
  -- tick, in-flight state survives the death — observed as a
  -- respawned tank still in build_walls. Also check for the dead
  -- waiting-to-respawn state: the engine holds armour at
  -- TANK_FULL_ARMOUR+1 (= 41) through the whole deathWait
  -- countdown (~200 ticks), giving us a wide window we can't miss.
  -- Respawn detection. bot_manager skips the brain while the tank is
  -- dead (deathWait > 0), so we never see armour > 40. Instead detect
  -- respawn by a large position jump between consecutive brain ticks:
  -- the last tick was at the pre-death position, the next tick is at
  -- the spawn point — if those are far apart, we just respawned.
  -- Also catches the very first tick (state._prev_mx is nil).
  local cur_mx = info.tankx >> 8
  local cur_my = info.tanky >> 8
  local _just_respawned = false
  if state._prev_mx then
    local jump = U.mdist(cur_mx, cur_my, state._prev_mx, state._prev_my)
    if jump > C.RESPAWN_CACHE_WIPE_DIST then
      _just_respawned = true
      print2(string.format("RESPAWN_DETECTED t=%d jump=%d prev=(%d,%d) now=(%d,%d)",
        now, jump, state._prev_mx, state._prev_my, cur_mx, cur_my))
    end
  end
  if _just_respawned then
    state.stuck_for = 0
    attack.clear_attack_goal(state)
    log.event("respawn", string.format("%d,%d", cur_mx, cur_my))
    if BRAIN_DEBUG_MODE then
      print(string.format(TAG .. " t=%d RESPAWN at (%d,%d) prev=(%d,%d)",
            now, cur_mx, cur_my, state._prev_mx or -1, state._prev_my or -1))
    end
    local jump_dist = U.mdist(cur_mx, cur_my,
                              state._prev_mx or cur_mx,
                              state._prev_my or cur_my)
    state._respawn_wipe_until = now + 150
    state._respawn_wipe_dist  = jump_dist
    state._respawn_prev_mx    = state._prev_mx
    state._respawn_prev_my    = state._prev_my
    state._stuck_escape_count = nil
    state._stuck_escape_mx    = nil
    state._stuck_escape_my    = nil
    state._desp_ticks         = 0
    state._desp_mx            = nil
    state._desp_my            = nil
    state._stuck_desperate    = false
    state.stuck_for           = 0
    print2(string.format("RESPAWN_INVALIDATE t=%d dist=%d — setting all cached costs to infinity",
      now, jump_dist))
    -- Set all cached costs to infinity so the eval queue re-evaluates
    -- from the new position. Keeps the cache entries (and their viz
    -- panel data) alive so the P overlay doesn't go blank — they just
    -- lose every cost comparison until fresh values arrive.
    if state.cost_cache then
      for _, entry in pairs(state.cost_cache) do
        entry.cost = math.huge
      end
    end
    -- Wipe pool_cache so goal_selection sees an empty pool and falls
    -- through to explore. The eval queue rebuild (~14 ticks) will
    -- populate fresh winners from the new position.
    state.pool_cache          = nil
    state.eval_queue          = nil
    state.eval_queue_pos      = nil
    state._pill_eval_cache    = nil
    state._pill_eval_progress = nil
    state.goal_cooldowns      = {}
    state.goal_history        = {}
    state.blocked             = {}
    state.banned_pill_angles  = {}
    state.wounded_pill        = nil
    -- Force-seed an explore goal so the first post-respawn tick has a
    -- sane state.goal before goal_selection runs.
    state.goal.kind     = "explore"
    state.goal.substate = nil
    state.goal.mx       = cur_mx
    state.goal.my       = cur_my
    state.goal.wx       = U.m2w(cur_mx)
    state.goal.wy       = U.m2w(cur_my)
  end

  if BRAIN_DEBUG_MODE and state._respawn_wipe_until and now <= state._respawn_wipe_until and viz.is_on("hud_goal") then
    viz.hud_text("hud_goal", 10, 200,
      string.format("RESPAWN WIPE dist=%d (>%d)",
        state._respawn_wipe_dist or 0, C.RESPAWN_CACHE_WIPE_DIST),
      "topleft", 255, 140, 0, 255)
    if state._respawn_prev_mx then
      local pmx = state._respawn_prev_mx + 0.5
      local pmy = state._respawn_prev_my + 0.5
      local R = C.RESPAWN_CACHE_WIPE_DIST
      viz.circle("hud_goal", pmx, pmy, R, 255, 140, 0, 120)
      viz.text("hud_goal", pmx, pmy - R - 0.5,
        string.format("WIPE R=%d", R), "center", 255, 140, 0, 200)
    end
  end

  -- Update prev position for next tick's respawn detection.
  state._prev_mx = cur_mx
  state._prev_my = cur_my

  -- Stuck detection
  local t_stuck0 = clock_us()

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
    -- Reposition: parked next to our own pill, deliberately shooting it down.
    or (state.goal.kind == "capture_pill" and state.goal.reposition
        and state.goal.substate == "reposition_shoot")
    or state.goal.kind == "rescue_lgm"
    or state.goal.kind == "wait_for_lgm"
  local attack_at_standoff = intentionally_stationary

  -- Long-term desperation: track total ticks at the same tile.
  -- After ~30s (1500 ticks) without moving, disable wsim kill-reject
  -- so the bot commits to a goal even if the sim predicts death.
  local DESPERATE_TICKS = 1500
  if cur_mx == (state._desp_mx or -1) and cur_my == (state._desp_my or -1) then
    state._desp_ticks = (state._desp_ticks or 0) + 1
  else
    state._desp_mx = cur_mx
    state._desp_my = cur_my
    state._desp_ticks = 0
  end
  state._stuck_desperate = state._desp_ticks >= DESPERATE_TICKS
  if state._stuck_desperate and state._desp_ticks == DESPERATE_TICKS then
    if BRAIN_DEBUG_MODE then
      print2(string.format("DESPERATE t=%d pos=(%d,%d) — disabling wsim kill reject",
        now, cur_mx, cur_my))
    end
  end

  -- Firing counts as progress: a bot that's planted while shooting at
  -- a base / pill / tank isn't stuck, it's working.  Detect by shell
  -- count dropping since last tick.  Without this, attack_base bots
  -- that legitimately camp the capture tile while shooting nearby
  -- defenders trip the 3-second stuck timer and abandon the take.
  local fired_this_tick = state.last_shells ~= nil
                          and info.shells ~= nil
                          and info.shells < state.last_shells
  state.last_shells = info.shells

  if fired_this_tick then
    -- Active firing is progress — reset the timer so a planted bot
    -- shooting defenders doesn't trip stuck-flee mid-take.
    state.stuck_for = 0
  end
  if cur_mx == state.last_mx and cur_my == state.last_my
     and state.goal.kind ~= "none"
     and not attack_at_standoff
     and not state.wall_clearing
     and not fired_this_tick then
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
        -- Ensure flee destination is at least 1 tile away from current pos.
        for d = C.FLEE_PILL_DIST, 1, -1 do
          local tx = U.mclamp(math.floor(cur_mx + ux * d + 0.5))
          local ty = U.mclamp(math.floor(cur_my + uy * d + 0.5))
          if tx == cur_mx and ty == cur_my then goto next_flee_d end
          local tt = U.ttype(tx, ty)
          if not U.is_water(tt)
             and tt ~= C.T_BUILDING and tt ~= C.T_HALFBUILD then
            fmx, fmy = tx, ty
            break
          end
          ::next_flee_d::
        end
        if not fmx then
          -- Radial projection failed (pill too far, all tiles blocked).
          -- Try any passable adjacent tile as a last resort.
          for dy = -1, 1 do
            for dx = -1, 1 do
              if dx ~= 0 or dy ~= 0 then
                local tx = U.mclamp(cur_mx + dx)
                local ty = U.mclamp(cur_my + dy)
                local tt = U.ttype(tx, ty)
                if not U.is_water(tt)
                   and tt ~= C.T_BUILDING and tt ~= C.T_HALFBUILD
                   and tt ~= C.T_PILLBOX then
                  fmx, fmy = tx, ty
                  break
                end
              end
            end
            if fmx then break end
          end
        end
        if not fmx then
          fmx, fmy = cur_mx, cur_my
        end
        if BRAIN_DEBUG_MODE then
          print(string.format(
            TAG .. " t=%d STUCK %s pill at (%d,%d) -- fleeing to (%d,%d)",
            now, state.goal.kind, state.goal.mx, state.goal.my, fmx, fmy))
          print2(string.format(
            "STUCK_FLEE t=%d goal=%s pill=(%d,%d) flee=(%d,%d)",
            now, state.goal.kind, state.goal.mx, state.goal.my, fmx, fmy))
        end
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
          if BRAIN_DEBUG_MODE then print(TAG .. " CMD: cancelled due to stuck (attack_pill)") end
        end
      else
        local bk = U.mkey(state.goal.mx, state.goal.my)
        state.blocked[bk] = now + 600
        if BRAIN_DEBUG_MODE then
          print(string.format(
            TAG .. " t=%d STUCK at (%d,%d) goal=%s dest=(%d,%d) -- blocking for 600t",
            now, cur_mx, cur_my, state.goal.kind, state.goal.mx, state.goal.my))
          print2(string.format(
            "STUCK_BLOCK t=%d pos=(%d,%d) goal=%s dest=(%d,%d)",
            now, cur_mx, cur_my, state.goal.kind, state.goal.mx, state.goal.my))
        end
        log.event("stuck", string.format("%s@%d,%d", state.goal.kind, state.goal.mx, state.goal.my))
        if state.command_goal then
          state.command_reply = string.format(
            C.BRAIN_NAME .. ": STUCK trying to reach %s #%d at (%d,%d) -- giving up",
            state.command_goal.kind, state.command_goal.id,
            state.command_goal.mx, state.command_goal.my)
          state.command_goal = nil
          if BRAIN_DEBUG_MODE then print(TAG .. " CMD: cancelled due to stuck") end
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
  -- mid covers from end-of-dij-sched (t_dij1) to here. Was using t4,
  -- which double-counted the dij sched block since that has its own
  -- main "dij sched done" emit. The unattributed remainder of mid
  -- (mid total minus stuck-detection minus blocked/banned) is the
  -- comms processing + respawn handling + stuck setup between t_dij1
  -- and t_stuck0. Dropped the "  comms+stuck done" sub-emit since it
  -- measured essentially the same span as mid itself.
  metrics.set("us_mid", t_mid - t_dij1)
  opt(string.format("mid done %.2f ms", (t_mid - t_dij1) / 1000))
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
    if BRAIN_DEBUG_MODE then
      print(string.format(TAG .. " t=%d BOAT LOST at (%d,%d) goal=%s -- recalc on-foot",
            now, cur_mx, cur_my, state.goal.kind))
    end
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
        if BRAIN_DEBUG_MODE then
          print(string.format(TAG .. " t=%d WATER BUILD ROAD at (%d,%d) trees=%d",
                now, cur_mx, cur_my, info.trees))
        end
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
          if BRAIN_DEBUG_MODE then
            print(string.format(TAG .. " t=%d WATER ESCAPE to (%d,%d)",
                  now, dry_x, dry_y))
          end
          log.event("water_escape", string.format("%d,%d tt=%d", dry_x, dry_y, tank_tt))
        end
      end
    end
  else
    state.water_build = nil
    state.water_build_logged = false

    -- Clear escape_water goal once on dry land and trigger immediate replan
    if state.goal.kind == "escape_water" then
      if BRAIN_DEBUG_MODE then
        print(string.format(TAG .. " t=%d ESCAPED WATER at (%d,%d) -- replanning",
              now, cur_mx, cur_my))
      end
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
            if BRAIN_DEBUG_MODE then
              print(string.format(TAG .. " t=%d ANTITANK DROP at (%d,%d) enemy@(%d,%d) dist=%d",
                    now, mid_mx, mid_my, et.mx, et.my, et.dist))
            end
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
          if BRAIN_DEBUG_MODE then
            print(string.format(TAG .. " t=%d EMERGENCY PILL DROP at (%d,%d) armour=%d",
                  now, best_drop_mx, best_drop_my, info.armour))
          end
          log.event("emergency_drop", string.format("at(%d,%d) arm=%d", best_drop_mx, best_drop_my, info.armour))
          -- Overlay: emergency drop position
          if BRAIN_DEBUG_MODE then
            viz.circle("hud_emergency_drop", best_drop_mx + 0.5, best_drop_my + 0.5, 0.5, 255, 50, 50, 255)
            viz.hud_text("hud_emergency_drop", 10, 24, "EMERGENCY PILL DROP!", "topleft", 255, 50, 50)
          end -- BRAIN_DEBUG_MODE
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
        if b and b.owner == "friendly" and BRAIN_DEBUG_MODE then
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
        if BRAIN_DEBUG_MODE then
          print(string.format(TAG .. " t=%d BASE CAPTURABLE: (%d,%d) owner=%s — switching to capture_base",
                now, gmx, gmy, b.owner))
        end
        log.event("base_capturable", string.format("(%d,%d) owner=%s", gmx, gmy, b.owner))
        state.goal.kind = "capture_base"
        state.goal.race_mode = true
        state.pf.status = "idle"
        state.urgent_capture_base = { mx = gmx, my = gmy, tick = now }
      elseif b.owner ~= "hostile" then
        -- Base became friendly (someone else captured it)
        goal_valid = false
      elseif (info.shells or 0) <= 0 then
        -- Out of ammo: can't damage a live hostile base. Drop and replan
        -- (refuel is urgent at 0 shells). Clear pool 7 so we don't
        -- immediately re-select it from stale cache. (The neutral →
        -- capture_base drive-over above is reached first and needs no ammo.)
        goal_valid = false
        if state.pool_cache then state.pool_cache[7] = nil end
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
      if not p then
        if state.goal.reposition then
          -- Reposition: the target is our OWN live pill. We drive up to it
          -- (capture nav) and shoot it down (reposition_shoot substate);
          -- only once it's dead does the spatial-index lookup above find it
          -- and the normal pickup runs. Stay valid while it's still friendly
          -- and alive and we have ammo to finish the job.
          local lp = world.pills[state.goal.target_id]
          if not lp or lp.owner ~= "friendly" or (lp.health or 0) <= 0
             or (info.shells or 0) <= 0 then
            goal_valid = false
          end
        else
          goal_valid = false
        end
      end
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
        elseif (info.shells or 0) <= 0 then
          -- Out of ammo: we can't damage the pill, so don't sit on it.
          -- Drop and replan (refuel is urgent at 0 shells). Clear pool 6
          -- so we don't immediately re-select it from stale cache.
          goal_valid = false
          if state.pool_cache then state.pool_cache[6] = nil end
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
      -- Pillbox-crossfire disengage (every phase): if we're standing in heavy
      -- enemy pill danger, drop the tank goal so we replan toward safety
      -- instead of trading armour into a pillbox-defended position.
      if threat.pill_at(info.tankx >> 8, info.tanky >> 8) >= C.TANK_COMBAT_DEFENDED_DANGER then
        goal_valid = false
        if state.pool_cache then state.pool_cache[9] = nil end
      end
      -- Also disengage if outgunned — but NOT during the opening phase,
      -- where we stay aggressive regardless of armour/shells.
      if state.phase ~= "opening"
         and (info.armour <= C.TANK_COMBAT_FLEE_ARMOUR
              or info.shells <= C.TANK_COMBAT_FLEE_SHELLS) then
        goal_valid = false
        -- Clear the pool cache so pick_goal doesn't re-select attack_tank
        -- immediately — the low-resource condition persists until we refuel.
        if state.pool_cache then state.pool_cache[9] = nil end
      end
    elseif gk == "refuel_at_base" or gk == "flee_to_base" then
      local b = W.base_at(world, gmx, gmy)
      if not b or (b.owner ~= "friendly" and b.owner ~= "neutral") then goal_valid = false end
    end
    if not goal_valid then
      if BRAIN_DEBUG_MODE then
        print(string.format(TAG .. " t=%d GOAL INVALID: %s at (%d,%d) — replanning",
              now, gk, gmx, gmy))
      end
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
    if BRAIN_PROFILE_LOG and t_goal0 - t_gv0 > 1000 then
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

    -- Per-tick ally-claimed REJECT sync: maintains entry._reject on
    -- cost_cache against the live ally_state slate so the pool grid and
    -- any selection that consults cost_cache between replans see fresh
    -- yield-decisions. Cheap (hash lookups per cached candidate). Pass
    -- info so the armour_too_low branch sees current armour (state.
    -- _last_info isn't populated outside the panel builder).
    if goals.sync_ally_claimed_rejects then
      goals.sync_ally_claimed_rejects(state, info)
    end

    -- Team reposition coordination: track the last tick anyone (self or an
    -- ally) was repositioning a pill. While someone is, reset to now so the
    -- time-based reposition discount (eval_reposition_pill) is 0 and the team
    -- doesn't pile on; it then grows the longer it's been since the last move.
    do
      local repositioning = (state.goal.kind == "capture_pill" and state.goal.reposition) and true or false
      -- Tiles allies are demolishing for a reposition (repos=1 + their goal
      -- tile mx/my). eval_repair_pill skips these so we don't heal a pill a
      -- teammate is busy shooting down.
      local ally_demolish = nil
      if ally_state.iter_active then
        for pn, slot in ally_state.iter_active(now, 1750) do
          if pn ~= info.player_number and slot.info and slot.info.repos == "1" then
            repositioning = true
            local amx = tonumber(slot.info.mx)
            local amy = tonumber(slot.info.my)
            if amx and amy then
              ally_demolish = ally_demolish or {}
              ally_demolish[amy * C.MAP_W + amx] = true
            end
          end
        end
      end
      state._ally_demolish_tiles = ally_demolish
      if repositioning or not state.last_team_reposition_tick then
        state.last_team_reposition_tick = now
      end
    end

    -- Pill blocker / utility tracking: a pre-existing friendly pill on the
    -- firing line of an active pill take is a "blocker" → role utility until
    -- the take ends. Each bot computes its own blockers + broadcasts them
    -- (bsi.pblk); here we union the whole team's blocker ids each tick and flag
    -- those pills _in_use so PP.role_of reports "utility". Rebuilt live, so a
    -- pill reverts to back/front/aggro as soon as the take stops broadcasting.
    do
      local util = {}
      local mine = attack.current_blocker_pids and attack.current_blocker_pids(state, world) or nil
      state._blocker_pids = mine
      if mine then for _, pid in ipairs(mine) do util[pid] = true end end
      if ally_state.iter_active then
        for pn, slot in ally_state.iter_active(now, 1750) do
          if pn ~= info.player_number and slot.info and slot.info.pblk then
            for s in string.gmatch(slot.info.pblk, "%d+") do
              util[tonumber(s)] = true
            end
          end
        end
      end
      for pid, p in pairs(world.pills) do
        p._in_use = util[pid] and true or nil
      end
    end

    -- Purge stale per-pill plan_position cache entries (pills that
    -- have been destroyed / picked up / captured friendly since last
    -- check). Cheap iteration over ~10-20 cached pills.
    attack.purge_dead_pill_eval_entries(state, world)

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
      print2(string.format("ATK_TANK t=%d done=%s et_count=%d perc=%s",
        now, tostring(attack_tank_done), et_count, state.perc and "yes" or "nil"))
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
    -- Trigger a one-shot urgent replan the first tick a brand-new base
    -- enters world.bases. world.bases is monotonic (entries persist once
    -- seen), so a count bump means we just spotted one we hadn't seen
    -- before. Lets refuel/capture_base/attack_base candidates show in the
    -- pool grid the next tick instead of waiting up to GOAL_REPLAN_INTERVAL
    -- for the regular timer fire to rebuild the eval queue.
    local base_count = 0
    for _ in pairs(world.bases) do base_count = base_count + 1 end
    local new_base_appeared = base_count > (state.prev_known_base_count or 0)
    state.prev_known_base_count = base_count
    -- Trigger a one-shot urgent replan the first tick a hostile LGM
    -- appears in perception while our goal isn't already kill_lgm.
    -- kill_lgm is HYST_EXEMPT and cheap (20 + dist*1.5), so it'll win
    -- cost competition immediately on visible LGM — but only if the
    -- replan actually fires. Without this trigger, pool_cache[13]
    -- shows the win but state.goal stays at its old value (explore /
    -- capture_base / attack_pill) until the next timer-fired replan,
    -- which can be up to GOAL_REPLAN_INTERVAL ticks away — long
    -- enough that the LGM has already retreated to its tank.
    local enemy_lgm_count = (state.perc and state.perc.enemy_lgms)
                            and #state.perc.enemy_lgms or 0
    local lgm_appeared = enemy_lgm_count > 0
                       and enemy_lgm_count > (state.prev_enemy_lgm_count or 0)
                       and state.goal.kind ~= "kill_lgm"
    state.prev_enemy_lgm_count = enemy_lgm_count
    -- Trigger urgent replan when we take damage from an enemy tank.
    -- Detect by: took damage this tick + enemy tank visible + not
    -- currently under pill fire (no angry pill in range).
    local shot_by_tank = false
    if state.took_damage_this_tick
       and state.goal.kind ~= "attack_tank"
       and state.perc and state.perc.enemy_tanks
       and #state.perc.enemy_tanks > 0
       and not (state.perc.under_fire) then
      shot_by_tank = true
      state._shot_by_tank = true
      -- Clear pool cache so attack_tank gets re-evaluated fresh
      if state.pool_cache then state.pool_cache[9] = nil end
    else
      state._shot_by_tank = false
    end
    -- attack_pill in disengage / plan_position is non-committed: if an enemy
    -- tank is in range, replan now so attack_tank (HYST-exempt in these
    -- substates) can preempt immediately instead of finishing the maneuver.
    local atk_pill_interruptible = state.goal.kind == "attack_pill"
        and (state.goal.substate == "disengage" or state.goal.substate == "plan_position")
        and state.perc and state.perc.enemy_tanks
        and #state.perc.enemy_tanks > 0

    local urgent_replan = state.goal.kind == "none" or attack_tank_done
                       or tank_appeared or dead_pill_appeared
                       or new_base_appeared or lgm_appeared
                       or shot_by_tank or atk_pill_interruptible
    if urgent_replan then
      -- Record which factor(s) tripped the urgent replan so the HUD
      -- below can flash a banner that's visible for a few seconds.
      -- Most-specific reason wins when more than one is true.
      local reason
      if lgm_appeared           then reason = "LGM APPEARED"
      elseif tank_appeared      then reason = "TANK APPEARED"
      elseif atk_pill_interruptible then reason = "TANK PREEMPT (pill loose)"
      elseif dead_pill_appeared then reason = "DEAD PILL"
      elseif new_base_appeared  then reason = "BASE DISCOVERED"
      elseif attack_tank_done   then reason = "ATTACK_TANK DONE"
      else                           reason = "GOAL=NONE"
      end
      state._last_urgent_replan = { tick = now, reason = reason }
      if BRAIN_DEBUG_MODE then print2(string.format("URGENT_REPLAN t=%d reason=%s goal=%s atk_done=%s tank_appeared=%s dead_pill=%s lgm_appeared=%s",
        now, reason, state.goal.kind, tostring(attack_tank_done), tostring(tank_appeared), tostring(dead_pill_appeared), tostring(lgm_appeared))) end
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

      -- Wait-for-ally substate.  When closing in on the base, if an ally
      -- tank is already standing on the goal tile we yield rather than
      -- pile on (each base supplies one tank at a time; two of us on the
      -- same tile is wasted time + a coordination headache).  Hold up to
      -- REFUEL_ALLY_WAIT_TICKS; if the ally hasn't moved by then they
      -- aren't leaving soon — blocklist the base and let goal selection
      -- find us another one.
      if state.goal.mx and state.goal.my then
        local our_mx = info.tankx >> 8
        local our_my = info.tanky >> 8
        local dist_cheb = math.max(math.abs(our_mx - state.goal.mx),
                                   math.abs(our_my - state.goal.my))
        local on_base_ourselves = (dist_cheb == 0)
        -- Always scan when refuel_at_base is active so the
        -- hud_refuel_ally_check overlay can show the live answer.
        -- Cheap (info.objects is short).
        local ally_on_base = false
        for _, ob in ipairs(info.objects) do
          if ob.type == 0   -- OBJECT_TANK
             and (ob.info & 1) == 0   -- not OBJECT_HOSTILE → ally (excludes self; self isn't in info.objects)
             and (ob.x >> 8) == state.goal.mx
             and (ob.y >> 8) == state.goal.my then
            ally_on_base = true
            break
          end
        end

        -- HUD line — always when refuel_at_base is the goal.
        if BRAIN_DEBUG_MODE then
          local wait_str
          if state.goal.wait_mx and state.goal.wait_my then
            wait_str = string.format("(%d,%d)", state.goal.wait_mx, state.goal.wait_my)
          else
            wait_str = "(-)"
          end
          viz.hud_text("hud_refuel_ally_check", 10, 240,
            string.format("Ally occupied check [%d/%d tiles] (%d,%d): %s, wait at %s",
                          dist_cheb, C.REFUEL_ALLY_WAIT_DIST,
                          state.goal.mx, state.goal.my,
                          ally_on_base and "occupied" or "empty",
                          wait_str),
            "topleft",
            ally_on_base and 255 or 180,
            ally_on_base and 180 or 220,
            ally_on_base and 80  or 180,
            230)
        end

        if refuel_needed
           and not on_base_ourselves
           and dist_cheb <= C.REFUEL_ALLY_WAIT_DIST
           and ally_on_base then
          if state.goal.substate ~= "wait_for_ally" then
            state.goal.substate = "wait_for_ally"
            state.goal.wait_started_tick = now
            -- Pick a low-danger park spot in an 11x11 square around the
            -- base.  Hanging out next to the base is fine but we don't
            -- want to idle next to a heating-up pillbox; rank candidates
            -- by danger first, then by Dijkstra cost from our current
            -- position.  Cap the path cost at ~500 so we don't wander
            -- across the map to wait.
            local WAIT_RADIUS    = 5      -- 11x11 square
            local WAIT_COST_CAP  = 500
            local best_mx, best_my = nil, nil
            local best_d, best_c = math.huge, math.huge
            local bmx, bmy = state.goal.mx, state.goal.my
            for dy = -WAIT_RADIUS, WAIT_RADIUS do
              for dx = -WAIT_RADIUS, WAIT_RADIUS do
                local cx = U.mclamp(bmx + dx)
                local cy = U.mclamp(bmy + dy)
                if (cx ~= bmx or cy ~= bmy)
                   and not U.is_water(U.ttype(cx, cy)) then
                  local pcost = cpf.dijkstra_lookup_by_kind(0 --[[KIND_NORMAL]], cx, cy, 0)
                  if pcost and pcost < WAIT_COST_CAP then
                    local d = threat.at(cx, cy) or 0
                    if d < best_d or (d == best_d and pcost < best_c) then
                      best_d, best_c = d, pcost
                      best_mx, best_my = cx, cy
                    end
                  end
                end
              end
            end
            state.goal.wait_mx = best_mx
            state.goal.wait_my = best_my
            if BRAIN_DEBUG_MODE then
              print(string.format(TAG .. " t=%d REFUEL: ally on base (%d,%d), wait_for_ally → park (%s,%s) danger=%.1f cost=%.0f",
                    now, state.goal.mx, state.goal.my,
                    tostring(best_mx), tostring(best_my),
                    best_d == math.huge and -1 or best_d,
                    best_c == math.huge and -1 or best_c))
            end
          end
          local waited = now - (state.goal.wait_started_tick or now)
          if waited >= C.REFUEL_ALLY_WAIT_TICKS then
            -- Timed out — ally is camping.  Block this base and replan.
            local bk = U.mkey(state.goal.mx, state.goal.my)
            state.blocked[bk] = now + 200
            attack.clear_attack_goal(state)
            if BRAIN_DEBUG_MODE then
              print(string.format(TAG .. " t=%d REFUEL: wait_for_ally timed out at (%d,%d) after %d ticks, blocking base",
                    now, state.goal.mx, state.goal.my, waited))
            end
          else
            -- Hold position; suppress timer/position-based replan while waiting.
            refuel_hold = true
          end
        elseif state.goal.substate == "wait_for_ally" then
          -- Base cleared (or we got far enough away that nobody's on
          -- it) — resume normal approach.
          state.goal.substate = nil
          state.goal.wait_started_tick = nil
          state.goal.wait_mx = nil
          state.goal.wait_my = nil
          if BRAIN_DEBUG_MODE then
            print(string.format(TAG .. " t=%d REFUEL: ally cleared base (%d,%d), resuming approach",
                  now, state.goal.mx, state.goal.my))
          end
        end
      end

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
          if BRAIN_DEBUG_MODE then
            print(string.format(TAG .. " t=%d REFUEL: base at (%d,%d) can't supply us (arm=%d sh=%d mn=%d), replanning",
                  now, state.goal.mx or 0, state.goal.my or 0,
                  info.base.armour or 0, info.base.shells or 0, info.base.mines or 0))
          end
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
    -- Force-replan flag, set by code paths that used to call
    -- clear_attack_goal but need the wounded pill to stay incumbent
    -- (cur_group=attack) through the next pool competition. Consumed
    -- this tick: cleared whether or not we actually replan, since the
    -- substate machine has already moved on.
    local force_replan = state._force_replan_reason ~= nil
    local force_replan_reason = state._force_replan_reason
    state._force_replan_reason = nil
    local replan = urgent_replan or refuel_done or force_replan
               or (min_commit_met and not refuel_hold and not state.command_goal and timer_fire)
    -- Only log the replan-decision dump on ticks where something
    -- interesting happens (timer fire, urgent replan, or refuel done).
    -- The vast majority of ticks just print "replan=false" with the
    -- same fields as the previous tick — pure noise.
    if BRAIN_DEBUG_MODE and (replan or timer_fire) then
      print2("replan=", replan, " urgent=", urgent_replan, " atk_done=", attack_tank_done,
             " refuel_done=", refuel_done, " force=", tostring(force_replan_reason),
             " refuel_hold=", refuel_hold, " timer_fire=", timer_fire,
             " min_commit=", min_commit_met,
             " goal=", state.goal.kind, " sub=", state.goal.substate)
    end
    if timer_fire and not replan and BRAIN_DEBUG_MODE then
      print(string.format(TAG .. " t=%d REPLAN BLOCKED: hold=%s cmd=%s urgent=%s atk_done=%s refuel_done=%s",
        now, tostring(refuel_hold), tostring(state.command_goal ~= nil),
        tostring(urgent_replan), tostring(attack_tank_done), tostring(refuel_done)))
    end

    state.replan_this_tick = replan
    if replan then
      if BRAIN_DEBUG_MODE then print2("ENTERING REPLAN") end
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
      if BRAIN_PROFILE and state._pick_goal_timing then
        for _, entry in ipairs(state._pick_goal_timing) do opt(entry) end
      end
      opt(string.format("  pick_goal done %.2f ms", (clock_us() - t_pg0) / 1000))
      if BRAIN_DEBUG_MODE then print2("pick_goal -> ", new_goal and new_goal.kind or "nil",
             " mx=", new_goal and new_goal.mx, " sub=", new_goal and new_goal.substate) end
      -- Dump all pool_cache winners with costs for diagnosing goal switches
      if BRAIN_DEBUG_MODE and state.pool_cache then
        for pi = 0, 10 do
          local pce = state.pool_cache[pi]
          if pce and pce.goal then
            print2(string.format("  pool[%d] %s @(%d,%d) cost=%.1f",
              pi, pce.goal.kind or "?", pce.goal.mx or 0, pce.goal.my or 0, pce.cost or -1))
          end
        end
      end
      -- Dump goal_competition entries
      if BRAIN_DEBUG_MODE and state.goal_competition then
        for _, gc in ipairs(state.goal_competition) do
          print2(string.format("  gc: %s @(%d,%d) base=%.1f penalty=%.1f total=%.1f hyst=%s",
            gc.kind or "?", gc.mx or 0, gc.my or 0,
            gc.base or 0, gc.penalty or 0, gc.total or 0, gc.hyst or "none"))
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
                             and new_goal.kind ~= "capture_pill"
                             and new_goal.kind ~= "kill_lgm" then
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
        -- Record abandoned goal on cooldown (prevent oscillation).
        -- Skip when a pill goal completed successfully — the pill at that
        -- tile is no longer a hostile target, so the cooldown is either
        -- moot (no candidate exists) or actively harmful if the pill
        -- becomes hostile again shortly.
        local completed = false
        if old_kind == "attack_pill" or old_kind == "capture_pill" then
          local p = W.pill_at(world, state.goal.mx or 0, state.goal.my or 0)
          if not p or p.owner == "friendly" then completed = true end
        end
        if old_kind ~= "none" and not completed then
          local cd_key = old_kind .. ":" .. (state.goal.mx or 0) .. "," .. (state.goal.my or 0)
          state.goal_cooldowns[cd_key] = now + C.GOAL_ABANDON_COOLDOWN
        end
        -- Successful pill take: remove the just-finished goal's entry from
        -- goal_history so the exponential kind/target recurrence penalty
        -- (goals.lua:4555-4560) doesn't punish picking the next low-HP
        -- pill. Finishing a take is not oscillation.
        if completed then
          local hist = state.goal_history
          local gmx  = state.goal.mx or 0
          local gmy  = state.goal.my or 0
          for i = #hist, 1, -1 do
            local h = hist[i]
            if h.kind == old_kind and h.mx == gmx and h.my == gmy then
              table.remove(hist, i)
              break
            end
          end
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

    -- Legacy aIndy /info pt + /info gbt claim broadcasts removed:
    -- the /info state path (in the end-of-tick block below) carries
    -- goal/sub/target/cost in one canonical message, and the
    -- ally_state slate is the single source of truth for de-conflict
    -- comparisons.

    -- Auto-expire: A* says we arrived or path impossible
    if state.pf.status == "failed" then
      if state.goal.kind == "explore" then
        local gk = U.mkey(state.goal.mx, state.goal.my)
        state.visited[gk] = true
        if BRAIN_DEBUG_MODE then
          print(string.format(TAG .. " t=%d ARRIVED/FAILED explore (%d,%d) -- marking visited",
                now, state.goal.mx, state.goal.my))
        end
        attack.clear_attack_goal(state)
      elseif state.goal.kind == "attack_pill" or state.goal.kind == "pill_place" then
        -- Can't reach attack position: flee away from the pill.
        -- No pf_fail_logged guard here — we always want to flee, not sit stuck.
        local dx  = cur_mx - state.goal.mx
        local dy  = cur_my - state.goal.my
        local len = math.max(1, math.sqrt(dx * dx + dy * dy))
        local fmx = U.mclamp(math.floor(cur_mx + dx / len * C.FLEE_PILL_DIST + 0.5))
        local fmy = U.mclamp(math.floor(cur_my + dy / len * C.FLEE_PILL_DIST + 0.5))
        if BRAIN_DEBUG_MODE then
          print(string.format(
            TAG .. " t=%d PF FAILED for %s (%d,%d) -- fleeing to (%d,%d)",
            now, state.goal.kind, state.goal.mx, state.goal.my, fmx, fmy))
        end
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
            if BRAIN_DEBUG_MODE then
              print(string.format(TAG .. " t=%d PF FAILED for %s dest=(%d,%d) -- tank at (%d,%d), blocking",
                    now, state.goal.kind, state.goal.mx, state.goal.my, cur_mx, cur_my))
            end
            log.event("pf_failed", string.format("%s@%d,%d", state.goal.kind, state.goal.mx, state.goal.my))
          end
          -- Block this destination so goal selection picks something else
          local bk = U.mkey(state.goal.mx, state.goal.my)
          state.blocked[bk] = now + 600
          print2(string.format(
            "PF_FAIL_BLOCK t=%d goal=%s dest=(%d,%d) pos=(%d,%d) 600t",
            now, state.goal.kind, state.goal.mx, state.goal.my, cur_mx, cur_my))
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
  if BRAIN_PROFILE_LOG and t_as1 - t_as0 > 500 then
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
          viz.line("attack_pill_pickup_path",
                   pp[i].x + 0.5,   pp[i].y + 0.5,
                   pp[i+1].x + 0.5, pp[i+1].y + 0.5,
                   255, 0, 255, a)
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
    capture_pill = true,
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
  if BRAIN_PROFILE_LOG and t_steer1 - t_steer0 > 5000 then
    opt.append("optimize.log", string.format(
      "  [steer] SLOW %.2f ms goal=%s sub=%s",
      (t_steer1 - t_steer0) / 1000,
      tostring(state.goal and state.goal.kind),
      tostring(state.goal and state.goal.substate)))
  end

  -- Always-on crosshairs (drawn after steering so they show every tick)
  -- Yellow crosshairs: ALWAYS on.  Position = actual shell-landing
  -- distance.  sightLen is in half-tiles (GUNSIGHT_MAX = 14 → 7 tile
  -- shot range), so shell travel in tiles = info.gunrange / 2.
  if BRAIN_DEBUG_MODE and viz.is_on("tank_aim_marker") then
    local ax, ay = U.crosshair_at(info.tankx, info.tanky, info.direction,
                                   (info.gunrange or 14) / 2.0)
    local shooting = (keys & KEY_SHOOT) ~= 0
    local cg = shooting and 0 or 255
    viz.line("tank_aim_marker", ax - 0.3, ay, ax + 0.3, ay, 255, cg, 0, 150)
    viz.line("tank_aim_marker", ax, ay - 0.3, ax, ay + 0.3, 255, cg, 0, 150)
  end

  -- Target crosshairs + range circles when attacking
  if BRAIN_DEBUG_MODE then do
    local twx, twy = info.tankx / 256.0, info.tanky / 256.0
    local g = state.goal
    if g.mx and g.my then
      local gk = g.kind
      if gk == "attack_pill" or gk == "defend_pill"
         or gk == "repair_pill" or gk == "pill_place" then
        local pmx, pmy = g.mx + 0.5, g.my + 0.5
        -- Near-edge aim point (computed before steering)
        local aim_mx = g.aim_mx or pmx
        local aim_my = g.aim_my or pmy
        if viz.is_on("pill_range_circle") then
          -- Red crosshairs on pill center
          viz.line("pill_range_circle", pmx - 0.6, pmy, pmx + 0.6, pmy, 255, 80, 80, 200)
          viz.line("pill_range_circle", pmx, pmy - 0.6, pmx, pmy + 0.6, 255, 80, 80, 200)
          -- Magenta circle on near-edge aim point
          viz.circle("pill_range_circle", aim_mx, aim_my, 0.15, 255, 0, 255, 220)
          -- Pill danger range (red) and standoff ring (green)
          viz.circle("pill_range_circle", pmx, pmy, C.PILL_RANGE_MAP, 255, 60, 60, 140)
          viz.circle("pill_range_circle", pmx, pmy, C.ATTACK_PILL_STANDOFF, 0, 255, 0, 160)
          -- Aim line to near-edge point (green=aimed, red=correcting)
          local aim_dir = U.aim_at(info.tankx, info.tanky,
                                   math.floor(aim_mx * 256), math.floor(aim_my * 256))
          local corr = math.abs(U.adiff(info.direction, aim_dir))
          local ar = corr < 3 and 100 or 255
          local ag = corr < 3 and 255 or 100
          viz.line("pill_range_circle", twx, twy, aim_mx, aim_my, ar, ag, 100, 120)
          -- Standoff position marker (not for bpc — it doesn't use fixed standoff)
          if g.standoff_mx and gk ~= "attack_pill" then
            viz.circle("pill_range_circle", g.standoff_mx + 0.5, g.standoff_my + 0.5, 0.35,
                           0, 255, 0, 160)
          end
        end
        -- Substate label
        if g.substate and viz.is_on("hud_attack_status") then
          viz.hud_text("hud_attack_status", 10, 44, string.format("Attack: %s  sub: %s",
            gk, g.substate), "topleft", 255, 180, 80)
        end
        -- Bullet counter: shots fired / needed
        if g._charge_shells and g._bullets_needed and viz.is_on("attack_bullet_counter") then
          local fired = g._charge_shells - info.shells
          local needed = g._bullets_needed
          local r = fired >= needed and 100 or 255
          local gr = fired >= needed and 255 or 200
          viz.text("attack_bullet_counter", pmx, pmy - 1.2,
            string.format("%d/%d", fired, needed),
            "center", r, gr, 100, 255)
        end
      elseif gk == "attack_base" and viz.is_on("attack_base_marker") then
        local bmx, bmy = g.mx + 0.5, g.my + 0.5
        viz.line("attack_base_marker", bmx - 0.6, bmy, bmx + 0.6, bmy, 255, 80, 80, 200)
        viz.line("attack_base_marker", bmx, bmy - 0.6, bmx, bmy + 0.6, 255, 80, 80, 200)
        viz.line("attack_base_marker", twx, twy, bmx, bmy, 255, 150, 50, 120)
      end
    end
  end end -- BRAIN_DEBUG_MODE (target crosshairs+attack viz)

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
    viz.circle("wounded_pill_marker", wpx, wpy, pulse,        255, 80, 80, 220)
    viz.circle("wounded_pill_marker", wpx, wpy, pulse + 0.04, 255, 80, 80, 120)
    -- Header line shows the in-pool 0.30x AND the cross-goal commit
    -- discount (decays alongside finish_other; 1.0x once expired).
    local commit_mult = 1.0 - (1.0 - (C.WOUNDED_COMMIT_DISCOUNT or 0.5)) * time_factor
    viz.text  ("wounded_pill_marker", wpx + 0.6, wpy - 0.6,
               string.format("WOUNDED hp=%d (this pill x0.30 *commit x%.2f)",
                             wp_hp, commit_mult),
               "topleft", 255, 120, 120, 255, 0.35)
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
    viz.text("wounded_pill_marker", wpx + 0.6, wpy - 0.25,
             string.format("other pills x%.2f", mult),
             "topleft", mr, mg, mb, 255, 0.30)
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
        viz.text("pool6_self_dr", px - 0.4, py + 0.7,
                 string.format("self_dr=%.0f", e._self_dr),
                 "topleft", r, g, b, 230, 0.30)
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
      viz.circle("inspect_pill", ipmx, ipmy, C.PILL_RANGE_MAP, 255, 100, 100, 100)
      viz.circle("inspect_pill", ipmx, ipmy, C.ATTACK_PILL_STANDOFF, 100, 255, 100, 100)
      local safe_r = C.ATTACK_SAFE_RADIUS
      if spots then
        for _, s in ipairs(spots) do
          local cr, cg = s.has_los and 0 or 200, s.has_los and 200 or 0
          viz.rect("inspect_pill", s.cx - 0.15, s.cy - 0.15, s.cx + 0.15, s.cy + 0.15, cr, cg, 0, 200)
          if s.has_los then
            local sr, sg = (s.total_score <= C.ATTACK_DANGER_THRESHOLD) and 0 or 255,
                           (s.total_score <= C.ATTACK_DANGER_THRESHOLD) and 200 or 165
            viz.rect("inspect_pill", s.cx - 0.25, s.cy - 0.25, s.cx + 0.25, s.cy + 0.25, sr, sg, 0, 120)
            if s.maneuver_tiles then
              for _, t in ipairs(s.maneuver_tiles) do
                viz.rect("inspect_pill", t.x, t.y, t.x + 1, t.y + 1, 255, 255, 0, 60)
                -- Show coverage count on each maneuver tile
                local cov = threat.coverage_at(t.x, t.y)
                if cov > 0 then
                  local cr, cg = 255, 255
                  if cov > 1 then cr, cg = 255, 0 end  -- red if crossfire
                  viz.text("inspect_pill", t.x + 0.5, t.y + 0.3, tostring(cov),
                    "center", cr, cg, cg, 200)
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
                  viz.line("inspect_pill", px, py, nx, ny, er, eg, 0, 80)
                end
                px, py = nx, ny
              end
            end
            if s.total_score < 900 then
              local label = string.format("A%.0f+B%.0f+D%.0f+E%.0f=%.0f",
                s.score_a, s.score_b, s.score_d or 0, s.score_e or 0, s.total_score)
              viz.text("inspect_pill", s.cx - 1, s.cy - 0.5, label, "topleft", 255, 0, 255, 200)
            end
          end
        end
      end
      do
        viz.hud_text("inspect_pill", 10, 70, string.format("Inspect: pill#%d (%d,%d) hp=%d %s",
          ip.id, ip.mx, ip.my, ipill.health, ipill.owner), "topleft", 255, 200, 0)
        -- Legend
        local lx, ly = ip.mx + 12, ip.my - 6
        viz.text("inspect_pill", lx, ly,       "Position Score Legend:", "topleft", 255, 200, 0, 255)
        viz.text("inspect_pill", lx, ly + 0.7, "A = avg danger in maneuver area", "topleft", 255, 0, 255, 255)
        viz.text("inspect_pill", lx, ly + 1.4, "B = hotspot penalty (any tile >= " .. C.ATTACK_DANGER_HOTSPOT .. ")", "topleft", 255, 0, 255, 255)
        viz.text("inspect_pill", lx, ly + 2.1, "D = terrain penalty", "topleft", 255, 0, 255, 255)
        viz.text("inspect_pill", lx, ly + 2.8, "E = crossfire 10*(N-1) pills", "topleft", 255, 0, 255, 255)
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
          local et_wx = U.m2w(et.mx)
          local et_wy = U.m2w(et.my)
          local aim_dir = U.aim_at(info.tankx, info.tanky, et_wx, et_wy)
          local aim_corr = U.adiff(info.direction, aim_dir)
          if math.abs(aim_corr) <= C.TANK_COMBAT_OPPORTUNISTIC_AIM
             and _shot_path_clear_init(info, world, et_wx, et_wy, et.mx, et.my) then
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

  -- Kill-LGM aim+fire.  Fires opportunistically whenever ANY hostile
  -- LGM is within KILL_LGM_SHOOT_RANGE — works even when our active
  -- goal is something else (e.g. mid-attack_pill).  We aim at the
  -- LGM's lead-predicted position based on its tracked velocity:
  --   ttl = dist_wu / SHELL_SPEED
  --   predicted = (lgm_wx + vx*ttl, lgm_wy + vy*ttl)
  -- Gated on shells > SHELL_RESERVE and not-in-boat, same as the
  -- opportunistic-tank-shot above.
  --
  -- Wall-LOS check: run cpf.simulate_shot to trace the real shell tile
  -- path; if it crosses a T_BUILDING or T_HALFBUILD BEFORE reaching the
  -- LGM tile, suppress the fire (keep driving — we'll re-evaluate from
  -- a clearer angle).  Walls *after* the LGM don't count.  Cached
  -- per-LGM-idnum for KILL_LGM_LOS_CHECK_INTERVAL ticks to keep the
  -- per-tick cost bounded.
  local KILL_LGM_LOS_CHECK_INTERVAL = 10
  -- Per-tick evaluation table — populated for every visible LGM so the
  -- kill_lgm_status viz can show a label per LGM regardless of whether
  -- we actually fired this tick.  Cleared in place (not reallocated) at
  -- the top of the block to avoid per-tick GC churn.
  state._kill_lgm_eval = state._kill_lgm_eval or {}
  for i = #state._kill_lgm_eval, 1, -1 do state._kill_lgm_eval[i] = nil end
  local _no_shells = info.shells <= C.SHELL_RESERVE
  local _shoot_busy = (keys & KEY_SHOOT) ~= 0 or (taps & KEY_SHOOT) ~= 0
  local _already_fired = false
  -- Crosshair driver state: track the closest viable LGM (in range, LOS
  -- clear) so we can drive info.gunrange toward its target sightLen
  -- when the bot's active goal is kill_lgm.  Other goals (attack_pill
  -- etc.) keep their own crosshair logic — we don't disturb them just
  -- to catch opportunistic LGMs, those shots only fire when crosshair
  -- happens to already align.
  local _primary_lgm = nil
  local _primary_dist = math.huge
  if state.perc and state.perc.enemy_lgms then
    state._kill_lgm_los = state._kill_lgm_los or {}
    local los_cache = state._kill_lgm_los
    -- Prune stale entries (LGMs that died/left) so the cache can't grow
    -- unbounded over a match, and a tile-keyed entry can't outlive its
    -- LGM and wrongly answer for a different LGM that reuses the tile.
    for k, ce in pairs(los_cache) do
      if now - ce.tick > KILL_LGM_LOS_CHECK_INTERVAL * 2 then los_cache[k] = nil end
    end
    for _, elm in ipairs(state.perc.enemy_lgms) do
      -- Euclidean distance in tiles (was Manhattan).  The shoot-range
      -- gate + status label both want true distance, not the
      -- grid-walking estimate perception stamps via U.mdist.
      local _ddx = (elm.wx - info.tankx) / 256.0
      local _ddy = (elm.wy - info.tanky) / 256.0
      local _ev = { mx = elm.mx, my = elm.my,
                    dist = math.sqrt(_ddx * _ddx + _ddy * _ddy),
                    idnum = elm.idnum, vx = elm.vx or 0, vy = elm.vy or 0,
                    v_ema_x = elm.v_ema_x, v_ema_y = elm.v_ema_y,
                    target_sightLen = elm.target_sightLen,
                    flight_ticks = elm.flight_ticks }
      state._kill_lgm_eval[#state._kill_lgm_eval + 1] = _ev
      if info.inboat then
        _ev.status = "in_boat"
      elseif _no_shells then
        _ev.status = "no_shells"
      elseif _ev.dist > C.KILL_LGM_SHOOT_RANGE then
        _ev.status = "out_of_range"
      else
        -- Use perception's lead-predicted aim point (EMA velocity +
        -- convergence loop, see kill_lgm.lua).  Falls back to current
        -- LGM position if perception didn't populate the predicted
        -- fields (shouldn't happen, but be defensive).
        local aim_wx = elm.predicted_wx or elm.wx
        local aim_wy = elm.predicted_wy or elm.wy
        local target_sl = elm.target_sightLen
        local aim_dir = U.aim_at(info.tankx, info.tanky, aim_wx, aim_wy)
        local aim_corr = U.adiff(info.direction, aim_dir)
        _ev.aim_corr = aim_corr
        _ev.ttl = elm.flight_ticks
        _ev.aim_mx = math.floor(aim_wx + 0.5) >> 8
        _ev.aim_my = math.floor(aim_wy + 0.5) >> 8
        -- Cheap pre-gate: if we're more than ~22° off the lead point
        -- the Euclidean check below would reject anyway, so skip the
        -- LOS raycast for that case. (A tighter ~7° lateral gate was
        -- historically used here but proved too strict for a moving
        -- target; this coarse 16-unit reject replaced it.)
        if math.abs(aim_corr) > 16 then  -- ~22° quick-reject
          _ev.status = "off_aim"
        else
          -- LOS check (cached).  Key by idnum if stable, else tile.
          local cache_key = elm.idnum and ("i" .. elm.idnum)
                            or ("t" .. elm.mx .. "," .. elm.my)
          local ce = los_cache[cache_key]
          local los_blocked
          if ce and (now - ce.tick) < KILL_LGM_LOS_CHECK_INTERVAL then
            los_blocked = ce.blocked
          else
            los_blocked = false
            local tiles = cpf.simulate_shot
                          and cpf.simulate_shot(info.tankx, info.tanky,
                                                aim_wx, aim_wy,
                                                cpf.SHOT_TANK or 0, 0)
            if tiles then
              local origin_mx, origin_my = info.tankx >> 8, info.tanky >> 8
              for ti = 1, #tiles do
                local t = tiles[ti]
                if t.mx == elm.mx and t.my == elm.my then break end
                if not (t.mx == origin_mx and t.my == origin_my) then
                  local tt = U.ttype(t.mx, t.my)
                  if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
                    los_blocked = true; break
                  end
                  local plist = world.pill_at and world.pill_at[t.my * 256 + t.mx]
                  if plist then
                    for _, pe in ipairs(plist) do
                      if pe.pill and pe.pill.health and pe.pill.health > 0 then
                        los_blocked = true; break
                      end
                    end
                    if los_blocked then break end
                  end
                  local bentry = world.base_at and world.base_at[t.my * 256 + t.mx]
                  if bentry and bentry.base and bentry.base.owner == "hostile" then
                    los_blocked = true; break
                  end
                end
              end
            end
            los_cache[cache_key] = { tick = now, blocked = los_blocked }
          end
          _ev.los_blocked = los_blocked
          -- Fire-gate: shell explosion point must land inside a
          -- ½-tile-diameter circle (radius 64 wu) around the predicted
          -- LGM center.  Engine's kill radius is the full 128 wu
          -- (lgm.c:1321) but we tighten by half so we don't lean on
          -- splash — small prediction error still kills.  Shell
          -- explodes at tank + 128*sl wu in the firing direction
          -- (sl in half-tiles, so 128 wu/unit).
          local cur_sl = info.gunrange or 14
          local shell_travel_wu = 128 * cur_sl
          local rad = (info.direction or 0) * C.TWO_PI / 256
          local explode_wx = info.tankx + math.sin(rad) * shell_travel_wu
          local explode_wy = info.tanky - math.cos(rad) * shell_travel_wu
          local ex_dx = explode_wx - aim_wx
          local ex_dy = explode_wy - aim_wy
          local impact_off = math.sqrt(ex_dx * ex_dx + ex_dy * ex_dy)
          _ev.gunrange     = cur_sl
          _ev.gunrange_off = target_sl and math.abs(cur_sl - target_sl) or 99
          _ev.land_off_wu  = impact_off
          local KILL_WU = 64  -- ½-tile-diameter circle around predicted LGM center
          if los_blocked then
            _ev.status = "los_blocked"
          elseif impact_off > KILL_WU then
            _ev.status = "gunrange_off"
          elseif _shoot_busy or _already_fired then
            _ev.status = "ready_busy"
          else
            _ev.status = "shooting"
            taps = taps | KEY_SHOOT
            _already_fired = true
            log.event("kill_lgm_shot",
              string.format("lgm@(%d,%d) dist=%d aim=%.0f sl=%d/%d v=(%.1f,%.1f)",
                            elm.mx, elm.my, elm.dist, aim_corr,
                            info.gunrange or 0, target_sl or 0,
                            elm.v_ema_x or 0, elm.v_ema_y or 0))
          end
          -- Track closest non-LOS-blocked LGM in range as primary for
          -- the crosshair driver below.  Picking by raw dist (not
          -- predicted dist) keeps the choice stable as the LGM moves.
          if not los_blocked and _ev.dist < _primary_dist then
            _primary_dist = _ev.dist
            _primary_lgm  = elm
          end
        end
      end
    end
  end

  -- ── Kill-LGM crosshair pre-charge ────────────────────────────────
  -- While goal is kill_lgm but the LGM is still out of shooting
  -- range (no _primary_lgm yet), drive info.gunrange toward MAX so
  -- the crosshair is already fully extended by the time we arrive
  -- in range and the 27-candidate search starts pulling it in.
  -- Saves ~5-7 ticks of "pull crosshair out from wherever attack_pill
  -- left it" the moment we get into firing position.
  if state.goal and state.goal.kind == "kill_lgm"
     and not _primary_lgm
     and not info.inboat
     and (info.gunrange or 14) < 14 then
    keys = keys | KEY_MORERANGE
  end

  -- ── Kill-LGM 27-candidate greedy search ──────────────────────────
  -- Replaces the old "convergence loop + dumb crosshair driver" pair
  -- with a stateless per-tick search: enumerate every (turn × speed
  -- × gunsight) action combination, predict the resulting crosshair
  -- position one tick out, score by distance to where the LGM will
  -- be one tick out, pick the minimum.  Override the relevant key
  -- bits with the winner.
  --
  -- Why 27 candidates: each axis has 3 options (none / + / −) so
  -- 3 × 3 × 3 = 27.  Each candidate is a few FP ops + one sqrt.
  -- Sub-µs total, no allocation.
  --
  -- Why every tick is enough: we re-run the search next tick with
  -- the new tank state, so the action chosen each tick locally
  -- minimizes error; the global trajectory falls out naturally
  -- (greedy descent on the crosshair-to-LGM distance metric).
  --
  -- Gated on goal == kill_lgm so attack_pill / attack_tank etc.
  -- keep their own steering + crosshair management.  Falls back to
  -- the goal's target LGM (looked up by target_id in perception) when
  -- _primary_lgm wasn't set this tick — _primary_lgm requires LOS
  -- clear, but the search itself is meaningful even when blocked
  -- (steering's [ENGAGE] flag only cares about range, and the user
  -- still wants to inspect aim candidates while waiting for LOS).
  if not _primary_lgm and state.goal and state.goal.kind == "kill_lgm"
     and state.goal.target_id and state.perc and state.perc.enemy_lgms then
    for _, elm in ipairs(state.perc.enemy_lgms) do
      if elm.idnum == state.goal.target_id then
        _primary_lgm = elm
        break
      end
    end
  end
  if state.goal and state.goal.kind == "kill_lgm"
     and _primary_lgm
     and not info.inboat then
    -- Per-tick deltas. Approximate; engine ramps each over time
    -- but the search re-runs every tick so picking the right
    -- DIRECTION matters more than the exact magnitude.
    local TURN_DELTA  = 6   -- bolo brads/tick when turn key held
    local SPEED_DELTA = 2   -- speed units/tick when throttle held
    local GUN_DELTA   = 2   -- sightLen units/tick when range held
                            -- (2 input packets/tick @ 1 step each)
    local cur_dir    = info.direction or 0
    local cur_speed  = info.speed or 0
    local cur_gun    = info.gunrange or 14
    local tank_wx    = info.tankx
    local tank_wy    = info.tanky
    -- Where the LGM will be at SHELL IMPACT — kill_lgm.predict_aim's
    -- two-pass wall-aware simulation.  Fallback to next-tick linear
    -- extrapolation if perception didn't stamp a prediction (cold start).
    local lgm_wx = _primary_lgm.predicted_wx
                or (_primary_lgm.wx + (_primary_lgm.v_ema_x or 0))
    local lgm_wy = _primary_lgm.predicted_wy
                or (_primary_lgm.wy + (_primary_lgm.v_ema_y or 0))
    -- Build the turn/speed/gun descriptor tables ONCE (cached on state)
    -- rather than reallocating 9 records every tick the goal is kill_lgm.
    -- Built lazily here, not at module scope, because the KEY_* globals
    -- are host-injected and guaranteed present by the first tick.
    local mt = state._kill_lgm_move_tables
    if not mt then
      mt = {
        TURNS  = { { k = 0,             d =  0,          n = "T:none" },
                   { k = KEY_TURNLEFT,  d = -TURN_DELTA, n = "T:L"    },
                   { k = KEY_TURNRIGHT, d =  TURN_DELTA, n = "T:R"    } },
        SPEEDS = { { k = 0,             d =  0,           n = "S:none" },
                   { k = KEY_FASTER,    d =  SPEED_DELTA, n = "S:+"    },
                   { k = KEY_SLOWER,    d = -SPEED_DELTA, n = "S:-"    } },
        GUNS   = { { k = 0,             d =  0,         n = "G:none" },
                   { k = KEY_MORERANGE, d =  GUN_DELTA, n = "G:+"    },
                   { k = KEY_LESSRANGE, d = -GUN_DELTA, n = "G:-"    } },
      }
      state._kill_lgm_move_tables = mt
    end
    local TURNS, SPEEDS, GUNS = mt.TURNS, mt.SPEEDS, mt.GUNS
    local TWO_PI_OVER_256 = math.pi * 2 / 256
    local best_score = math.huge
    local best_keys  = 0
    local cands
    if BRAIN_DEBUG_MODE then cands = {} end
    for _, t in ipairs(TURNS) do
      local new_dir = (cur_dir + t.d) % 256
      local rad = new_dir * TWO_PI_OVER_256
      local sin_d = math.sin(rad)
      local cos_d = math.cos(rad)
      for _, s in ipairs(SPEEDS) do
        local new_speed = cur_speed + s.d
        if new_speed < 0   then new_speed = 0   end
        if new_speed > 128 then new_speed = 128 end
        -- 1-tick tank position update (small but non-trivial at
        -- speed; ~0.5 tile/tick at max).
        local pred_tank_wx = tank_wx + sin_d * new_speed
        local pred_tank_wy = tank_wy - cos_d * new_speed
        for _, g in ipairs(GUNS) do
          local new_gun = cur_gun + g.d
          if new_gun < 2  then new_gun = 2  end
          if new_gun > 14 then new_gun = 14 end
          -- Crosshair = tank + (gunrange/2 tiles in heading dir),
          -- converted to wu (× 256).  sightLen is in half-tiles
          -- (see GUNSIGHT_MAX comment in constants.lua).
          local cross_wu = (new_gun / 2.0) * 256
          local cross_wx = pred_tank_wx + sin_d * cross_wu
          local cross_wy = pred_tank_wy - cos_d * cross_wu
          local dx = cross_wx - lgm_wx
          local dy = cross_wy - lgm_wy
          local score = math.sqrt(dx * dx + dy * dy)
          if BRAIN_DEBUG_MODE then
            cands[#cands + 1] = {
              name = t.n .. " " .. s.n .. " " .. g.n,
              score = score,
              new_dir = new_dir, new_speed = new_speed, new_gun = new_gun,
              cross_wx = cross_wx, cross_wy = cross_wy,
              keys = t.k | s.k | g.k,
            }
          end
          if score < best_score then
            best_score = score
            best_keys  = t.k | s.k | g.k
          end
        end
      end
    end
    -- Override the action bits: clear the six we manage, set the
    -- winning combination.  Anything else (KEY_SHOOT, KEY_DROPMINE,
    -- etc.) is preserved.
    local CLEAR = KEY_TURNLEFT | KEY_TURNRIGHT | KEY_FASTER
                | KEY_SLOWER   | KEY_MORERANGE | KEY_LESSRANGE
    keys = (keys & ~CLEAR) | best_keys
    if BRAIN_DEBUG_MODE then
      state._kill_lgm_search = {
        score  = best_score,
        keys   = best_keys,
        tank_wx = tank_wx, tank_wy = tank_wy,
        lgm_wx  = lgm_wx,  lgm_wy  = lgm_wy,
        cur_dir = cur_dir, cur_speed = cur_speed, cur_gun = cur_gun,
        cands  = cands,
        lgm_mx = _primary_lgm.mx, lgm_my = _primary_lgm.my,
        lgm_idnum = _primary_lgm.idnum,
      }
    end
    state._kill_lgm_crosshair_drive = nil
  else
    if BRAIN_DEBUG_MODE then state._kill_lgm_search = nil end
    state._kill_lgm_crosshair_drive = nil
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
  if BRAIN_DEBUG_MODE then goals.draw_wsim_paths(state) end
  -- Draw attack_tank detection/precondition overlays (navy blue)
  if BRAIN_DEBUG_MODE then goals.draw_attack_tank_viz(state, info) end

  -- Base shield visualization: show wall target, pill source, and blocking line
  if BRAIN_DEBUG_MODE and state._base_shield_viz and viz.is_on("base_shield_viz") then
    local bsv = state._base_shield_viz
    -- Only show for 50 ticks after trigger (5 seconds)
    if now - (bsv.tick or 0) < 50 then
      -- Wall target: cyan filled square
      viz.rect("base_shield_viz", bsv.wall_mx, bsv.wall_my, bsv.wall_mx + 1, bsv.wall_my + 1,
        0, 255, 255, 150)
      -- Base: green outline
      viz.rect("base_shield_viz", bsv.base_mx - 0.1, bsv.base_my - 0.1,
        bsv.base_mx + 1.1, bsv.base_my + 1.1, 0, 255, 0, 200)
      -- Pill source: red circle
      viz.circle("base_shield_viz", bsv.pill_mx + 0.5, bsv.pill_my + 0.5, 0.6, 255, 0, 0, 200)
      -- Line from pill to base (blocked by wall)
      viz.line("base_shield_viz", bsv.pill_mx + 0.5, bsv.pill_my + 0.5,
        bsv.base_mx + 0.5, bsv.base_my + 0.5, 255, 100, 100, 100)
      -- Line from base to wall target (LGM path)
      viz.line("base_shield_viz", bsv.base_mx + 0.5, bsv.base_my + 0.5,
        bsv.wall_mx + 0.5, bsv.wall_my + 0.5, 0, 255, 255, 200)
      -- Label
      viz.text("base_shield_viz", bsv.wall_mx + 0.5, bsv.wall_my - 0.5,
        string.format("SHIELD anger=%.1f", bsv.anger),
        "center", 0, 255, 255, 255)
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
        local cov = gh_threat.cov_grid_at(k)
        if cov > 0 then
          local tx = k % 256
          local ty = k // 256
          local t = math.min((cov - 1) / 4, 1.0)  -- 1=green, 5+=red
          local cr = math.floor(255 * t)
          local cg = math.floor(255 * (1 - t))
          viz.rect("coverage_grid", tx, ty, tx + 1, ty + 1, cr, cg, 0, 80)
          viz.text("coverage_grid", tx + 0.5, ty + 0.5, tostring(cov),
            "center", cr, cg, 0, 220)
        end
      end
    end
    if viz.is_on("pill_threat_overlay") then
      -- Draw stamp radius circle for each hostile/neutral pill
      for _, pm in pairs(world.pills) do
        if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
          viz.circle("pill_threat_overlay", pm.mx + 0.5, pm.my + 0.5, C.PILL_RANGE_MAP,
            255, 100, 100, 150)
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
  -- Repair-pill completion: the builder dispatched the LGM onto a
  -- friendly damaged pill (engine auto-repairs on arrival). Clear the
  -- goal so this tick's downstream goal-tracking and next tick's
  -- pick_goal see a clean slate — repair runs autonomously from here.
  if state._repair_dispatched then
    state._repair_dispatched = nil
    attack.clear_attack_goal(state)
    if BRAIN_DEBUG_MODE then
      print(string.format(TAG .. " t=%d REPAIR dispatched, clearing goal", now))
    end
  end
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
  if BRAIN_DEBUG_MODE then
    if info.man_status ~= C.LGM_INTANK then
      local ly = 70
      -- LGM position marker
      local man_mx = info.man_x >> 8
      local man_my = info.man_y >> 8
      viz.circle("lgm_destination", man_mx + 0.5, man_my + 0.5, 0.35, 0, 255, 0, 200)
      -- LGM ETA when out on mission
      local eta = state.builder and state.builder.lgm_eta
      if eta then
        local remaining = eta - (state.tick or 0)
        if remaining > 0 then
          viz.hud_text("hud_lgm_status", 10, ly, string.format("LGM ETA: %d ticks", remaining),
            "bottomleft", 0, 255, 0)
          ly = ly + 10
        end
      end
      -- LGM nearby flag
      if state.builder and state.builder.lgm_nearby then
        viz.hud_text("hud_lgm_status", 10, ly, string.format("LGM nearby (%dt)",
          state.builder.lgm_arrival_ticks or 0), "bottomleft", 0, 200, 255)
        ly = ly + 10
      end
      -- Stranded
      if state.lgm_stranded then
        viz.hud_text("hud_lgm_status", 10, ly, "LGM STRANDED", "bottomleft", 255, 50, 50)
        ly = ly + 10
      end
    end
    -- Water-ahead LGM suppression indicator
    if state.pf and state.pf.next_mx and state.pf.next_mx >= 0 then
      local ntt = U.ttype(state.pf.next_mx, state.pf.next_my)
      if ntt == C.T_RIVER or ntt == C.T_DEEPSEA or ntt == C.T_BOAT then
        viz.hud_text("hud_lgm_blocked", 10, 80, "LGM blocked: water ahead", "bottomleft", 255, 200, 50)
      end
    end
    -- Enemy LGM dead indicator
    if state.perc and state.perc.enemy_lgm_dead then
      local ret = state.perc.enemy_lgm_return_tick
      local remaining = ret and (ret - (state.tick or 0)) or 0
      viz.hud_text("hud_enemy_lgm_dead", 10, 4, string.format("Enemy LGM dead (~%ds)", math.floor(remaining / 50)),
        "topleft", 255, 100, 100)
    end
    -- Base Killer Mode indicator
    if state.perc and state.perc.base_killer_mode then
      viz.hud_text("hud_base_killer", 10, 24, string.format("BASE KILLER (adv +%d)",
        state.perc.team_advantage), "topleft", 255, 200, 0)
    end
    -- Stuck counter (yellow when building, red when critical)
    if state.stuck_for > 30 then
      local sr, sg = 255, 255
      if state.stuck_for > 100 then sr, sg = 255, 50
      elseif state.stuck_for > 60 then sr, sg = 255, 180 end
      viz.hud_text("hud_stuck_counter", 10, 14, string.format("Stuck: %d/150", state.stuck_for),
        "topleft", sr, sg, 50)
    end
  end -- BRAIN_DEBUG_MODE (LGM/stuck HUD)
  local t_pbh_lgm = clock_us()
  opt(string.format("  LGM/stuck HUD done %.2f ms", (t_pbh_lgm - t_pbh_start) / 1000))

  -- Blocked destinations (show on map)
  if BRAIN_DEBUG_MODE and state.blocked and viz.is_on("blocked_tiles") then
    for k, expires in pairs(state.blocked) do
      if expires > (state.tick or 0) then
        local bx = k % 256
        local by = math.floor(k / 256)
        viz.rect("blocked_tiles", bx, by, bx + 1, by + 1, 255, 0, 0, 80)
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
          viz.rect("pill_reposition_marker", p.mx, p.my, p.mx + 1, p.my + 1, 255, 165, 0, 150)
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
        viz.rect("bait_pill_marker", p.mx - 1, p.my - 1, p.mx + 2, p.my + 2, 255, 0, 255, 100)
        viz.text("bait_pill_marker", p.mx + 0.5, p.my - 0.5, "BAIT?", 255, 0, 255, 200)
      end
    end
  end

  local t_pbh_bait = clock_us()
  opt(string.format("  bait-pill viz done %.2f ms", (t_pbh_bait - t_pbh_repos) / 1000))

  -- Friendly pill barrier overlay: mark friendly pills used as shields
  if BRAIN_DEBUG_MODE and state.goal and (state.goal.kind == "attack_pill" or state.goal.kind == "attack_pill") and viz.is_on("friendly_pill_shield") then
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
          viz.circle("friendly_pill_shield", fp.mx + 0.5, fp.my + 0.5, 0.5, 0, 200, 255, 180)
          viz.text("friendly_pill_shield", fp.mx + 0.5, fp.my - 0.3, "SHIELD", 0, 200, 255, 180)
        end
      end
    end
  end

  local t_pbh_barrier = clock_us()
  opt(string.format("  friendly-pill-barrier viz done %.2f ms", (t_pbh_barrier - t_pbh_bait) / 1000))

  -- Allied LGM protection overlay: mark allied LGM positions
  if BRAIN_DEBUG_MODE and state.perc and state.perc.allied_lgm_positions and viz.is_on("ally_lgm_marker") then
    for _, alm in ipairs(state.perc.allied_lgm_positions) do
      viz.circle("ally_lgm_marker", alm.mx + 0.5, alm.my + 0.5, 0.3, 100, 255, 100, 160)
      viz.text("ally_lgm_marker", alm.mx + 0.5, alm.my - 0.3, "ALLY LGM", 100, 255, 100, 140)
    end
  end

  -- Enemy LGM marker: yellow X + tracked velocity arrow.  Tile coords
  -- center on (mx+0.5, my+0.5).  Velocity is in wu/tick (~1/256 tiles)
  -- so we scale by 1/64 to get a visible-but-small arrow tip.
  if BRAIN_DEBUG_MODE and state.perc and state.perc.enemy_lgms and viz.is_on("enemy_lgm_marker") then
    -- Cyan when we have a 3-window straight-line lock (dest_locked),
    -- yellow otherwise.  The PRED ring overlay carries the same tier
    -- info more explicitly; this is just an at-a-glance signal on the
    -- LGM tile itself.
    for _, elm in ipairs(state.perc.enemy_lgms) do
      local cx, cy = elm.mx + 0.5, elm.my + 0.5
      local cr, cg, cb
      if elm.dest_locked then cr, cg, cb = 80, 220, 255  -- cyan
      else                    cr, cg, cb = 255, 230, 0   -- yellow
      end
      viz.line("enemy_lgm_marker", cx - 0.35, cy - 0.35, cx + 0.35, cy + 0.35, cr, cg, cb, 240)
      viz.line("enemy_lgm_marker", cx - 0.35, cy + 0.35, cx + 0.35, cy - 0.35, cr, cg, cb, 240)
      if elm.vx ~= 0 or elm.vy ~= 0 then
        local tipx = cx + (elm.vx or 0) / 64.0
        local tipy = cy + (elm.vy or 0) / 64.0
        viz.line("enemy_lgm_marker", cx, cy, tipx, tipy, cr, cg - 50, 0, 220)
      end
      local label = elm.near_tank_idnum
                    and string.format("LGM(t%d)", elm.near_tank_idnum)
                    or "LGM"
      viz.text("enemy_lgm_marker", cx, cy - 0.45, label,
               "center", cr, cg, cb, 220)
    end
  end

  -- Clickable per-LGM detail panel: prediction internals (tier, lock
  -- progress, velocity, linear-extrapolation math).  Click any LGM
  -- tile with the D inspector open to see the panel.  Standalone in
  -- BRAIN_DEBUG_MODE — not gated on any specific viz toggle.
  if BRAIN_DEBUG_MODE and viz.detail_circle and state.perc and state.perc.enemy_lgms then
    for _, elm in ipairs(state.perc.enemy_lgms) do
      local idtxt = tostring(elm.idnum or "?")
      local did = "kill_lgm_track_" .. idtxt
      local tier = elm.predict_tier or "linear"
      local hdr  = string.format("LGM %s  tier=%s  ft=%d  off=%.0fwu",
                                 idtxt, tier, elm.flight_ticks or 0,
                                 elm.predicted_dist_wu or 0)
      viz.detail_circle(did, elm.mx + 0.5, elm.my + 0.5, 0.5, hdr)
      viz.detail_text(did, string.format(
        "lock: %s", elm.lock_status or "?"))
      viz.detail_text(did, string.format(
        "vel: vx=%.2f vy=%.2f wu/tick  (10-tick finite-difference window)",
        elm.v_ema_x or 0, elm.v_ema_y or 0))
      viz.detail_text(did, string.format(
        "lgm pos: wu=(%.0f,%.0f) tile=(%d,%d)",
        elm.wx or 0, elm.wy or 0, elm.mx or 0, elm.my or 0))
      viz.detail_text(did, string.format(
        "predicted: wu=(%.0f,%.0f) tile=(%d,%d)",
        elm.predicted_wx or 0, elm.predicted_wy or 0,
        elm.predicted_mx or 0, elm.predicted_my or 0))
      -- Full sample history (up to V_HISTORY_TICKS=32 ticks) so the
      -- user can sanity-check the velocity calc by hand.
      local h = state._enemy_lgm_history and state._enemy_lgm_history[elm.idnum]
      if h and h.samples then
        viz.detail_text(did, "")
        viz.detail_text(did, string.format(
          "samples (%d retained, ages in ticks; latest first):", #h.samples))
        local now = state.tick or 0
        for i = #h.samples, 1, -1 do
          local s = h.samples[i]
          local prev = h.samples[i - 1]
          local delta_str = ""
          if prev then
            local dt  = s.tick - prev.tick
            local dwx = s.wx - prev.wx
            local dwy = s.wy - prev.wy
            delta_str = string.format("  Δ=(%+d,%+d) over %d", dwx, dwy, dt)
          end
          viz.detail_text(did, string.format(
            "  age=%-3d wu=(%5d,%5d) tile=(%3d,%3d)%s",
            now - s.tick, s.wx, s.wy, s.wx >> 8, s.wy >> 8, delta_str))
        end
      end
      viz.detail_text(did, "")
      if tier == "linear" then
        local D  = elm.predicted_dist_wu or 0
        local T  = elm.flight_ticks or 0
        local vx = elm.v_ema_x or 0
        local vy = elm.v_ema_y or 0
        local LS = kill_lgm.LINEAR_LEAD_SCALE
        local sx = vx * T * LS
        local sy = vy * T * LS
        local lwx = elm.wx or 0
        local lwy = elm.wy or 0
        -- D_now: distance from tank to LGM right now (input to sightLen)
        local twx, twy = info.tankx, info.tanky
        local d_now = math.sqrt((twx - lwx)^2 + (twy - lwy)^2)
        local sl_now = math.floor(d_now / 128 + 0.5)
        if sl_now < 2  then sl_now = 2  end
        if sl_now > 14 then sl_now = 14 end
        viz.detail_text(did, "linear lead — how each value was derived:")
        viz.detail_text(did, "")
        viz.detail_text(did, "1) velocity (10-tick finite difference over samples):")
        viz.detail_text(did, "     v = (cur_pos - oldest_in_window_pos) / Δticks")
        viz.detail_text(did, string.format(
          "     vx = %.4f wu/tick      vy = %.4f wu/tick", vx, vy))
        viz.detail_text(did, string.format(
          "     |v|= %.4f wu/tick      bearing = atan2(vx,-vy) ≈ %.0f°",
          math.sqrt(vx*vx + vy*vy),
          math.deg(math.atan(vx, -vy))))
        viz.detail_text(did, "")
        viz.detail_text(did, "2) shell flight time from current tank→LGM distance:")
        viz.detail_text(did, string.format(
          "     D_now = |tank - lgm| = sqrt((%.0f-%.0f)² + (%.0f-%.0f)²) = %.0fwu",
          twx, lwx, twy, lwy, d_now))
        viz.detail_text(did, string.format(
          "     sightLen = round(D_now / 128) clamped[2..14] = %d", sl_now))
        viz.detail_text(did, string.format(
          "     T = flight_ticks(sl) = 4*sl - 5 (shells.c:133, post-PR-77) = %d sim-ticks", T))
        viz.detail_text(did, "")
        viz.detail_text(did, "3) lead offset (scaled to land empirically on-target):")
        viz.detail_text(did, string.format(
          "     Δ = v * T * LINEAR_LEAD_SCALE(%.2f)", LS))
        viz.detail_text(did, string.format(
          "       = (%.4f, %.4f) * %d * %.2f", vx, vy, T, LS))
        viz.detail_text(did, string.format(
          "       = (%.1f, %.1f) wu", sx, sy))
        viz.detail_text(did, "")
        viz.detail_text(did, "4) projected position (pass 1):")
        viz.detail_text(did, string.format(
          "     pos1 = lgm + Δ = (%.0f,%.0f) + (%.1f,%.1f) = (%.0f,%.0f)",
          lwx, lwy, sx, sy, lwx + sx, lwy + sy))
        viz.detail_text(did, "")
        viz.detail_text(did, "5) two-pass refit:")
        viz.detail_text(did, "     T2 = flight_ticks(sightlen_for(|tank - pos1|))")
        viz.detail_text(did, string.format(
          "     if T2 > T1: aim = pos1 + v * (T2-T1) * %.2f", LS))
        viz.detail_text(did, "     else:       aim = pos1")
        viz.detail_text(did, string.format(
          "     final aim shown on PRED ring: predicted=(%.0f,%.0f)",
          elm.predicted_wx or 0, elm.predicted_wy or 0))
      elseif tier == "dest_lock" then
        local D  = elm.predicted_dist_wu or 0
        local T  = elm.flight_ticks or 0
        local DS = kill_lgm.DEST_LEAD_SCALE
        local sT = math.floor(T * DS + 0.5)
        viz.detail_text(did, "destination-locked engine sim:")
        viz.detail_text(did, string.format(
          "  dest: wu=(%.0f,%.0f) (3-match ray to map edge)",
          elm.dest_wx or 0, elm.dest_wy or 0))
        viz.detail_text(did, string.format(
          "  T = flight_ticks(sightlen_for(D=%.0fwu)) = %d sim-ticks", D, T))
        viz.detail_text(did, string.format(
          "  sim_T = round(T * DEST_LEAD_SCALE(%.2f)) = %d", DS, sT))
        viz.detail_text(did, "  sim_forward_to_dest steps from lgm toward dest,")
        viz.detail_text(did, "  each step: angle-to-dest * MAN_SPEED[tile], wall-slide on block")
      end
    end
  end

  -- Kill-LGM status: per-LGM label under the marker showing the
  -- shoot-evaluation state (out_of_range / off_aim / los_blocked /
  -- ready_busy / shooting / no_shells / in_boat), plus a top-left
  -- HUD line summarizing the chosen target.  Always on when there's
  -- at least one visible LGM and the viz toggle is enabled.
  -- Engage-spot viz: line + ring on the tile the bot is driving to
  -- when out of range.  Lives next to the eval HUD on its own toggle
  -- so it can be enabled/disabled independently.
  if BRAIN_DEBUG_MODE then
    if viz.is_on("kill_lgm_engage")
       and state.pool_cache and state.pool_cache[13]
       and state.pool_cache[13].goal then
      local g = state.pool_cache[13].goal
      if g.shoot_mx and g.shoot_my then
        local twx = info.tankx / 256.0
        local twy = info.tanky / 256.0
        local sx = g.shoot_mx + 0.5
        local sy = g.shoot_my + 0.5
        -- Color-code by mode: magenta = APPROACH (driving to engage
        -- spot), green = ENGAGE (halted + aiming this tick), green*
        -- = sticky engage but LGM drifted back out of range.
        local in_engage = state._kill_lgm_halt == true
        local sticky    = state._kill_lgm_engaged_id ~= nil
                          and state.goal and state.goal.kind == "kill_lgm"
                          and state._kill_lgm_engaged_id == state.goal.target_id
        local cr, cg, cb, label
        if in_engage and sticky then
          cr, cg, cb, label = 80, 230, 80, "ENGAGE"
        elseif in_engage then
          cr, cg, cb, label = 80, 230, 80, "ENGAGE"
        elseif sticky then
          cr, cg, cb, label = 80, 230, 80, "ENGAGE*"  -- sticky, LGM lost/oor
        else
          cr, cg, cb, label = 220, 80, 220, "APPROACH"
        end
        viz.line("kill_lgm_engage", twx, twy, sx, sy, cr, cg, cb, 200)
        viz.circle("kill_lgm_engage", sx, sy, 0.45, cr, cg, cb, 230)
        viz.circle("kill_lgm_engage", sx, sy, 0.08, cr, cg, cb, 255)
        viz.text("kill_lgm_engage", sx, sy + 0.6, label,
                 "center", cr, cg, cb, 255, 0.4)
      end
    end
  end
  -- Clickable detail overlay for the 27-candidate kill_lgm search.
  -- Always registers in debug mode (independent of any specific viz
  -- toggle) so pressing D in BrainTest always reveals it.  Click the
  -- LGM tile to expand the panel; body lists every (turn × speed ×
  -- gun) combo with its predicted crosshair-to-LGM euclidean distance.
  if BRAIN_DEBUG_MODE then
    local _ksrch = state._kill_lgm_search
    if viz.detail_circle and _ksrch and _ksrch.cands then
      local sorted = {}
      for i, c in ipairs(_ksrch.cands) do sorted[i] = c end
      table.sort(sorted, function(a, b) return a.score < b.score end)
      local hdr = string.format("LGM t%s  best=%s score=%.1fwu (%.2ft)",
                                tostring(_ksrch.lgm_idnum or "?"),
                                sorted[1] and sorted[1].name or "?",
                                _ksrch.score or 0,
                                (_ksrch.score or 0) / 256)
      local did = string.format("kill_lgm_search_%s",
                                tostring(_ksrch.lgm_idnum or "p"))
      viz.detail_circle(did, _ksrch.lgm_mx + 0.5, _ksrch.lgm_my + 0.5,
                        0.5, hdr)
      viz.detail_text(did, string.format(
        "tank: dir=%d speed=%d gun=%d  (deltas: turn=%d speed=%d gun=%d per tick)",
        _ksrch.cur_dir or 0, _ksrch.cur_speed or 0, _ksrch.cur_gun or 0,
        6, 2, 2))
      viz.detail_text(did, string.format(
        "lgm next-tick: wu=(%.0f,%.0f)  score = euclidean(crosshair_after, lgm_next) in wu",
        _ksrch.lgm_wx or 0, _ksrch.lgm_wy or 0))
      viz.detail_text(did, "")
      viz.detail_text(did, "candidates (sorted by score, lower = better):")
      for i, c in ipairs(sorted) do
        local marker = (i == 1) and " <- WINNER" or ""
        viz.detail_text(did, string.format(
          "  [%2d] %s  score=%7.1fwu (%.2ft)  -> dir=%d spd=%d gun=%d%s",
          i, c.name, c.score, c.score / 256,
          c.new_dir, c.new_speed, c.new_gun, marker))
      end
    end
  end

  if state._kill_lgm_eval and #state._kill_lgm_eval > 0 and viz.is_on("kill_lgm_status") then
    local _STATUS_COLOR = {
      shooting      = { 255,  80,  80, 240 },   -- red: firing this tick
      ready_busy    = { 255, 200,  80, 220 },   -- amber: would fire but shoot key already used
      off_aim       = { 200, 200, 200, 220 },   -- light gray
      los_blocked   = { 160, 100, 220, 220 },   -- purple: wall in the way
      out_of_range  = { 130, 130, 130, 200 },
      no_shells     = { 100, 100, 100, 200 },
      in_boat       = { 100, 100, 100, 200 },
    }
    local _shooting_target
    for _, ev in ipairs(state._kill_lgm_eval) do
      local cx, cy = ev.mx + 0.5, ev.my + 0.5
      local col = _STATUS_COLOR[ev.status] or _STATUS_COLOR.out_of_range
      local label
      if ev.aim_corr ~= nil then
        -- off=N/M : N is the shell-explosion → predicted-LGM Euclidean
        -- distance in wu, M is the kill-circle radius (fires when N≤M).
        -- "—" when we never got far enough to compute (e.g. off_aim
        -- quick-reject).
        local off_str = ev.land_off_wu
                        and string.format(" off=%.0f/%d", ev.land_off_wu, 64)
                        or ""
        label = string.format("%s d=%.1f aim=%+d%s%s",
                              ev.status, ev.dist or -1,
                              math.floor(ev.aim_corr + 0.5),
                              off_str,
                              ev.los_blocked and " LOS✗" or "")
      else
        label = string.format("%s d=%.1f", ev.status, ev.dist or -1)
      end
      viz.text("kill_lgm_status", cx, cy + 0.5, label, "center",
               col[1], col[2], col[3], col[4])
      if ev.status == "shooting" then _shooting_target = ev end
    end

    -- Lead-prediction overlay: ring + line at the predicted impact tile
    -- per LGM.  Tier color: yellow=linear (no dest), cyan=dest-lock
    -- (3-match map-edge sim).
    if BRAIN_DEBUG_MODE and viz.is_on("kill_lgm_predict") then
      local TIER_COL = {
        linear    = { 255, 220,  60 },
        dest_lock = {  80, 220, 255 },
      }
      for _, elm in ipairs(state.perc.enemy_lgms or {}) do
        if elm.predicted_wx and elm.predicted_wy then
          local lx = (elm.wx or 0) / 256
          local ly = (elm.wy or 0) / 256
          local px = elm.predicted_wx / 256
          local py = elm.predicted_wy / 256
          local col = TIER_COL[elm.predict_tier or "linear"] or TIER_COL.linear
          local cr, cg, cb = col[1], col[2], col[3]
          viz.line("kill_lgm_predict", lx, ly, px, py, cr, cg, cb, 200)
          viz.circle("kill_lgm_predict", px, py, 0.45, cr, cg, cb, 220)
          viz.circle("kill_lgm_predict", px, py, 0.10, cr, cg, cb, 255)
          viz.text("kill_lgm_predict", px, py - 0.55,
                   string.format("PRED ft=%d %s", elm.flight_ticks or 0,
                                 elm.predict_tier or "linear"),
                   "center", cr, cg, cb, 230)
        end
      end
    end

    -- Forward-sim path overlay: dotted trail of the LGM's predicted
    -- positions over the next flight_ticks, using the engine sim
    -- (kill_lgm.sim_forward_to_dest).  Only drawn when predict_aim
    -- picked the dest_lock tier — linear tier is a straight line, no
    -- point dotting.
    if BRAIN_DEBUG_MODE and viz.is_on("kill_lgm_sim_path") then
      for _, elm in ipairs(state.perc.enemy_lgms or {}) do
        local dest_wx, dest_wy
        if elm.predict_tier == "dest_lock" and elm.dest_wx and elm.dest_wy then
          dest_wx, dest_wy = elm.dest_wx, elm.dest_wy
        end
        if dest_wx then
          local col_r, col_g, col_b = 80, 220, 255
          -- Draw a faint line to the destination point itself.
          viz.line("kill_lgm_sim_path",
                   elm.wx / 256, elm.wy / 256,
                   dest_wx / 256, dest_wy / 256,
                   col_r, col_g, col_b, 70)
          -- Then dot the simulated trajectory at 1-tick intervals
          -- (kill_lgm.sim_forward_to_dest one step at a time).
          local steps = math.min(elm.flight_ticks or 8, 24)
          local cwx, cwy = elm.wx, elm.wy
          for _ = 1, steps do
            cwx, cwy = kill_lgm.sim_forward_to_dest(cwx, cwy, dest_wx, dest_wy, 1)
            viz.circle("kill_lgm_sim_path", cwx / 256, cwy / 256,
                       0.06, col_r, col_g, col_b, 200)
          end
        end
      end
    end

    -- Detail panel was relocated out of this status block (it had been
    -- accidentally gated on the kill_lgm_status viz toggle); now lives
    -- standalone in its own BRAIN_DEBUG_MODE-gated block below.
    if viz.hud_text then
      local hud_msg, r, g, b
      -- Mode tag: ENGAGE = halted+aiming this tick, ENGAGE* = sticky
      -- but LGM out of range, APPROACH = driving to engage spot,
      -- empty = goal isn't kill_lgm.
      local mode_tag = ""
      if state.goal and state.goal.kind == "kill_lgm" then
        local in_engage = state._kill_lgm_halt == true
        local sticky    = state._kill_lgm_engaged_id ~= nil
                          and state._kill_lgm_engaged_id == state.goal.target_id
        if in_engage then mode_tag = "[ENGAGE] "
        elseif sticky then mode_tag = "[ENGAGE*] "
        else mode_tag = "[APPROACH] " end
      end
      if _shooting_target then
        hud_msg = string.format("KILL_LGM: %sSHOOTING lgm@(%d,%d) d=%.1f",
                                mode_tag,
                                _shooting_target.mx, _shooting_target.my,
                                _shooting_target.dist or -1)
        r, g, b = 255, 100, 100
      else
        -- pick worst-state to surface
        local best = state._kill_lgm_eval[1]
        for _, ev in ipairs(state._kill_lgm_eval) do
          if best.status ~= "ready_busy" and ev.status == "ready_busy" then best = ev end
        end
        hud_msg = string.format("KILL_LGM: %s%s lgm@(%d,%d) d=%.1f",
                                mode_tag,
                                best.status, best.mx, best.my, best.dist or -1)
        r, g, b = 220, 220, 100
      end
      viz.hud_text("kill_lgm_status", 8, 96, hud_msg, "topleft", r, g, b, 230)
      -- 27-candidate search readout: shows which action combination
      -- this tick's greedy descent picked + the resulting crosshair-
      -- to-LGM error.  Replaces the old crosshair_drive line.
      local s = state._kill_lgm_search
      if s then
        -- Decode the chosen key set into short labels.
        local parts = {}
        if (s.keys & KEY_TURNLEFT)  ~= 0 then parts[#parts + 1] = "L"     end
        if (s.keys & KEY_TURNRIGHT) ~= 0 then parts[#parts + 1] = "R"     end
        if (s.keys & KEY_FASTER)    ~= 0 then parts[#parts + 1] = "FWD"   end
        if (s.keys & KEY_SLOWER)    ~= 0 then parts[#parts + 1] = "BACK"  end
        if (s.keys & KEY_MORERANGE) ~= 0 then parts[#parts + 1] = "GUN+"  end
        if (s.keys & KEY_LESSRANGE) ~= 0 then parts[#parts + 1] = "GUN-"  end
        local action = #parts > 0 and table.concat(parts, "+") or "(no action — aligned)"
        viz.hud_text("kill_lgm_status", 8, 112,
          string.format("  search: err=%.0fwu (%.1ft)  pick=[%s]",
                        s.score, s.score / 256.0, action),
          "topleft", 220, 220, 100, 230)
        viz.hud_text("kill_lgm_status", 8, 124,
          string.format("  state: dir=%d  speed=%d  gunrange=%d (→ %.1ft)",
                        s.cur_dir, s.cur_speed, s.cur_gun,
                        s.cur_gun / 2.0),
          "topleft", 180, 180, 180, 220)
      end
    end
  end


  local t_pbh_ally = clock_us()
  opt(string.format("  ally-LGM viz done %.2f ms", (t_pbh_ally - t_pbh_barrier) / 1000))

  -- Ally-state overlay (right-middle table of every active player's
  -- goal / sub / target / k=v data). The slate is populated from the
  -- chat-based shared-state protocol; if no protocol traffic has
  -- landed yet the table is empty.
  if BRAIN_DEBUG_MODE then
    -- Heartbeat is 30 s; stale at 35 s (1750 ticks @ 50 Hz). 5 s
    -- grace window after the expected next heartbeat — any longer
    -- gap and we don't trust their state.
    ally_state.draw(viz, state.tick, info.player_number, 1750)
    ally_state.draw_chat_log(viz, state.tick, info.player_number)
    pill_table.draw(viz, world, state, info)
    goals.draw_pill_spots(viz, state)
    attack.draw_pill_eval_progress(viz, state)
    attack.draw_plan_trace(viz, state, info)
    lgm_registry.draw_hud(viz, state.tick, info.player_number)
    lgm_registry.draw_map(viz, state.tick, info.player_number, info.allies)
    -- Semi-transparent gray rectangle over each pill/base currently
    -- claimed by another bot (per ally_state slate).  Maps the ally's
    -- broadcast goal+target to a world tile and draws a 1x1 rect.
    if viz.is_on("ally_claimed_marker") then
      local _PILL_KIND = {
        attack_pill = true, capture_pill = true,
        repair_pill = true, defend_pill = true,
      }
      local _BASE_KIND = {
        capture_base = true, attack_base = true, refuel_at_base = true,
      }
      for pn, slot in ally_state.iter_active(state.tick, 1750) do
        if pn ~= info.player_number then
          local g = slot.info.goal
          local tgt = tonumber(slot.info.target)
          local mx, my
          if tgt and _PILL_KIND[g] and world.pills and world.pills[tgt] then
            mx, my = world.pills[tgt].mx, world.pills[tgt].my
          elseif tgt and _BASE_KIND[g] and world.bases and world.bases[tgt] then
            mx, my = world.bases[tgt].mx, world.bases[tgt].my
          else
            mx = tonumber(slot.info.mx)
            my = tonumber(slot.info.my)
          end
          if mx and my then
            viz.rect("ally_claimed_marker", mx, my, mx + 1, my + 1,
                     40, 40, 40, 210, true)  -- filled, dark gray, mostly opaque
          end
        end
      end
    end
  end
  local t_pbh_ally_state = clock_us()
  opt(string.format("  ally-state overlay done %.2f ms", (t_pbh_ally_state - t_pbh_ally) / 1000))

  -- Log this tick
  log.log_tick(state, info, state.goal, keys, taps, build_cmd)
  opt(string.format("  log.log_tick done %.2f ms", (clock_us() - t_pbh_ally) / 1000))

  -- Total tick time and worst-case tracking
  local t_end = clock_us()
  local us_total = t_end - t0
  metrics.set("us_post_build_hud", t_end - t_build1)
  opt(string.format("post-build HUD done %.2f ms", (t_end - t_build1) / 1000))
  -- Anchor for the (tail) timer: real clock immediately after the last
  -- named main-section emit. Anything between here and opt.flush() is
  -- attributed to (tail) so the named-section sum equals the actual
  -- tick wall-clock with no gap.
  local _t_tail_anchor = clock_us()
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
  local _t_tail_post_metrics = BRAIN_PROFILE and clock_us() or 0


  -- Label all pills and bases with their IDs (centered on tile)
  if BRAIN_DEBUG_MODE and viz.is_on("pill_id_label") then
    local t_label0 = clock_us()
    for id, p in pairs(world.pills) do
      viz.text("pill_id_label", p.mx + 0.5, p.my + 0.5, tostring(id), "center", 0, 0, 200, 255)
    end
    for id, b in pairs(world.bases) do
      viz.text("pill_id_label", b.mx + 0.5, b.my + 0.5, tostring(id), "center", 0, 255, 255, 255, 2.4)
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
  if BRAIN_DEBUG_MODE then do
    local function arrow(dx, dy, ch, k_on, t_on)
      local r, g, b
      if k_on then
        r, g, b = 100, 255, 100         -- green = held
      elseif t_on then
        r, g, b = 255, 220,  60         -- yellow = tap
      else
        r, g, b =  60,  60,  60         -- dim = nothing this tick
      end
      viz.hud_text("hud_compass", dx, dy, ch, "bottomright", r, g, b, 255)
    end
    -- Pixel offsets from bottom-right corner. Note bottomright anchor:
    -- (0,0) = bottom-right; positive x goes left, positive y goes up.
    arrow(40, 80, "^", (keys & KEY_FASTER)    ~= 0, (taps & KEY_FASTER)    ~= 0)
    arrow(60, 60, "<", (keys & KEY_TURNLEFT)  ~= 0, (taps & KEY_TURNLEFT)  ~= 0)
    arrow(40, 60, "v", (keys & KEY_SLOWER)    ~= 0, (taps & KEY_SLOWER)    ~= 0)
    arrow(20, 60, ">", (keys & KEY_TURNRIGHT) ~= 0, (taps & KEY_TURNRIGHT) ~= 0)
    -- Gunrange (crosshair) keys, second row directly below the cross.
    -- "−" = LESSRANGE (retract / pull crosshair in toward tank)
    -- "+" = MORERANGE (expand / push crosshair out toward max)
    -- Same color scheme as the arrow keys: green=held, yellow=tap,
    -- dim grey when not pressed.
    arrow(50, 40, "-", (keys & KEY_LESSRANGE) ~= 0, (taps & KEY_LESSRANGE) ~= 0)
    arrow(30, 40, "+", (keys & KEY_MORERANGE) ~= 0, (taps & KEY_MORERANGE) ~= 0)
  end end -- BRAIN_DEBUG_MODE (arrow HUD)

  -- Flush print2 log for this tick
  if BRAIN_DEBUG_MODE then
    print2("END state.goal.kind = ", state.goal.kind, ", state.goal.substate = ", tostring(state.goal.substate))
    print2.flush()
  end
  -- Sub-section breakdown of (tail). Indented sub-section emits MUST
  -- come before the (tail) main emit so rebuild_sections attaches them
  -- as subs.
  --   (tail)/metrics    — TICK TOTAL emit + metrics.set/max +
  --                       metrics.finish_tick. Mostly opt() buffer-push
  --                       overhead.
  --   (tail)/late-debug — pill+base id labels, dbg.end_trace, arrow
  --                       HUD, print2.flush. All stripped in opt/, so
  --                       reads near zero in production. A non-trivial
  --                       value here in opt/ implies a GC pause hit
  --                       during this window.
  if BRAIN_PROFILE then
    opt(string.format("  (tail)/metrics done %.2f ms",
                      (_t_tail_post_metrics - _t_tail_anchor) / 1000))
    opt(string.format("  (tail)/late-debug done %.2f ms",
                      (clock_us() - _t_tail_post_metrics) / 1000))
  end
  -- (tail) main: total from _t_tail_anchor to NOW. Emit AFTER the
  -- indented subs above so the parser sees pending subs and attaches
  -- them to this section.
  opt(string.format("(tail) done %.2f ms", (clock_us() - _t_tail_anchor) / 1000))
  opt(string.format("END tick=%d total=%.2f ms", now, (clock_us() - t_tick_start) / 1000))
  -- Anchor right before the flush itself. opt.flush()'s sync work
  -- (rebuild_sections walk + queue-push to the threaded log writer)
  -- happens between (tail) emit and the END marker — captured below
  -- as a (post-flush) section appended to last_sections so that
  -- sum-of-sections equals the end-marker think_total_ms.
  local _t_pre_flush = clock_us()
  opt.flush()
  if _G.BRAIN_PROFILE and opt.last_sections then
    opt.last_sections[#opt.last_sections + 1] = {
      name = "(post-flush)",
      ms   = (clock_us() - _t_pre_flush) / 1000,
      subs = {},
    }
  end

  -- ── PERFORMANCE MARKER: END ──
  -- Capture the wall-clock at think exit so the Y panel's header can
  -- show think_total_ms (matched against host's lastThinkMs). Gated on
  -- BRAIN_PROFILE because the panel only needs this when profiling is
  -- on; without the marker, get_capacity_state_json reports null.
  if _G.BRAIN_PROFILE then
    state._think_end_us = clock_us()
  end

  -- ── performance.ticks.log ──────────────────────────────────────────
  -- One JSON object per tick recording the same state the Y panel
  -- shows. First byte of each line is `{` so jq -c / line-streaming
  -- readers work. Lands in DEBUG_SESSION_DIR/performance.ticks.log when
  -- the host set one, else cwd. Gated on BRAIN_PROFILE_LOG so we only
  -- pay the JSON build + queue push when we actually want files.
  if _G.BRAIN_PROFILE_LOG then
    local body = Brain.get_capacity_state_json and Brain.get_capacity_state_json() or "{}"
    local path = (_G.DEBUG_SESSION_DIR or ".") .. "/performance.ticks.log"
    -- Key on the GAME tick (server_tick), NOT state.tick (a per-think counter):
    -- the .btr recorder stores serverSimGetTick, so BrainTest aligns recorded
    -- tier data to playback frames by game tick. Using state.tick here diverges
    -- whenever the bot doesn't think every game tick (worker pool / capacity
    -- tiers), mis-aligning and truncating coverage. Fallback to now if unset.
    local perf_tick = state.server_tick or now
    local line = string.format('{"tick":%d,"bot":%d,"data":%s}',
                                perf_tick, state.player_number or 0, body)
    if gh_opt_log then
      gh_opt_log.append(path, line)
    else
      -- Fallback when the threaded writer isn't compiled in. Same
      -- semantics, but file I/O on the brain thread.
      local f = io.open(path, "a")
      if f then f:write(line, "\n"); f:close() end
    end
  end

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
  if BRAIN_DEBUG_MODE then do
    local _think_ms = (os.clock() - _think_t0) * 1000.0  -- unused but cheap; kept in case someone wants it
    local replan_left = C.GOAL_REPLAN_INTERVAL
        - ((now + state.replan_offset) % C.GOAL_REPLAN_INTERVAL)
    if replan_left == C.GOAL_REPLAN_INTERVAL then replan_left = 0 end
    local g = state.goal or {}
    local goal_str = (g.kind or "none")
    if g.target_id and g.target_id >= 0 then
      goal_str = string.format("%s#%d(%d,%d)",
        goal_str, g.target_id, g.mx or 0, g.my or 0)
    elseif g.mx then
      goal_str = string.format("%s(%d,%d)", goal_str, g.mx, g.my)
    end
    viz.hud_text("hud_tick_info", 8, 32,
      string.format("replan:%d  phase:%s  goal:%s",
        replan_left, state.phase or "?", goal_str),
      "topleft", 200, 220, 200)
    -- Capacity tier readout: current tier + ratio + per-tier ms history
    -- (so corroboration data is visible). Color shifts toward red as
    -- tier drops so glances catch it.
    local _tier = state._capacity_tier or 10
    local _ratio = state._capacity_ratio_ewma or 0
    local _r, _g, _b = 200, 220, 200
    if     _tier <= 3 then _r, _g, _b = 255, 80, 80
    elseif _tier <= 5 then _r, _g, _b = 255, 160, 80
    elseif _tier <= 7 then _r, _g, _b = 230, 220, 100
    end
    local _last  = (_G.brain and _G.brain.lastThinkMs) or 0
    local _tgt   = (_G.brain and _G.brain.targetMs)    or 0
    local _killed = _G.brain and _G.brain.wasKilled
    local _killed_str = _killed and " KILLED" or ""
    viz.hud_text("hud_tick_info", 8, 44,
      string.format("capacity: tier %d/10  (ratio %.2f, last %.1fms / target %.1fms)%s",
        _tier, _ratio, _last, _tgt, _killed_str),
      "topleft", _r, _g, _b)
    -- ── Prominent budget + kill banner ──
    -- Top-center: large status line color-coded by usage; flashing red
    -- KILLED banner when the previous tick was force-killed by the
    -- count hook.  Gated on its own viz toggle so users who don't want
    -- another HUD element can hide it.
    if viz.is_on("hud_budget") then
      local _usage = (_tgt > 0) and (_last / _tgt) or 0
      local _br, _bg, _bb
      if     _usage >= 1.10 then _br, _bg, _bb = 255,  60,  60   -- red: over budget
      elseif _usage >= 0.90 then _br, _bg, _bb = 255, 140,  60   -- orange
      elseif _usage >= 0.60 then _br, _bg, _bb = 240, 220, 100   -- yellow
      else                       _br, _bg, _bb = 120, 220, 120   -- green
      end
      viz.hud_text("hud_budget", 8, 116,
        string.format("BUDGET  %.1f / %.1f ms  (%.0f%%)",
                      _last, _tgt, _usage * 100),
        "topright", _br, _bg, _bb, 255)
      if _killed then
        -- 6-tick flash cycle so the kill banner can't be missed.
        local _phase = (now % 6) < 3
        local _kr, _kg, _kb = _phase and 255 or 100,
                              _phase and 30  or 30,
                              _phase and 30  or 100
        viz.hud_text("hud_budget", 8, 134,
          "*** BRAIN KILLED LAST TICK -- tick_budget_exceeded ***",
          "topright", _kr, _kg, _kb, 255)
      end
    end
    -- Per-tier ms history line: shows what we've seen each tier cost.
    -- Dashes for tiers we haven't visited yet. Useful for verifying the
    -- raise corroboration logic is making sensible choices.
    if state._tier_ms then
      local parts = {}
      for t = 10, 1, -1 do
        local v = state._tier_ms[t]
        local marker = (t == _tier) and "*" or " "
        if v then
          parts[#parts + 1] = string.format("%s%d:%.1f", marker, t, v)
        else
          parts[#parts + 1] = string.format("%s%d:--", marker, t)
        end
      end
      viz.hud_text("hud_tick_info", 8, 81,
        "tier ms: " .. table.concat(parts, " "),
        "topleft", 180, 200, 180, 200, 0.85)
    end
  end end -- BRAIN_DEBUG_MODE (hud_tick_info)

  -- (PERFORMANCE MARKER: END is captured inside the perf-log write
  -- block above, just before get_capacity_state_json reads it. Doing
  -- it there instead of here means the JSON's think_total_ms value
  -- is fresh for the current tick.)

  -- /info state broadcast — runs EVERY tick (not gated by goal selection)
  -- so heartbeat actually fires on its 30 s cadence and any goal change
  -- surfaces immediately. Phase 1 populator: clobber broadcast_state_info
  -- with goal/sub/target read off the current goal.
  do
    -- Goal-change detector for the human-readable announcement at the
    -- end of this block. The /info state slate covers allied bots via
    -- the internal channel; humans get a plain-English line only on
    -- a real (kind, target_id) transition AND no more than once per
    -- HUMAN_GOAL_MIN_INTERVAL ticks per bot. mx/my are intentionally
    -- not part of the fingerprint — they shift on every retarget
    -- within the same goal pursuit (e.g. pill snaps to a slightly
    -- different tile) and would dominate the announcement volume.
    do
      local HUMAN_GOAL_MIN_INTERVAL = 500   -- ticks = 10 s at 50 Hz
      local cur_kind = (state.goal and state.goal.kind) or "idle"
      local cur_id   = (state.goal and state.goal.target_id) or -1
      state.last_announced_goal = state.last_announced_goal
                                  or { kind = "", target_id = -2, tick = -10000 }
      local lag = state.last_announced_goal
      local cooldown_ok = (now - (lag.tick or -10000)) >= HUMAN_GOAL_MIN_INTERVAL
      if cooldown_ok and (lag.kind ~= cur_kind or lag.target_id ~= cur_id) then
        -- No "GoalHunter:" prefix — chat row shows the speaker's
        -- name already, so the brain name would double up.
        local line
        if cur_kind == "idle" then
          line = "idle"
        elseif cur_id >= 0 then
          line = string.format("%s #%d", cur_kind, cur_id)
        else
          line = cur_kind
        end
        state.pending_human_goal_msg = line
        lag.kind = cur_kind
        lag.target_id = cur_id
        lag.tick = now
      end
    end

    -- Lazy init in case the brain was created before these state fields
    -- were added (state. is set in open()) — keeps a hot-reload from
    -- erroring out on nil.
    if state.broadcast_state_info == nil       then state.broadcast_state_info       = {} end
    if state.last_broadcasted_state_info == nil then state.last_broadcasted_state_info = {} end
    if state.last_broadcast_state_tick == nil  then state.last_broadcast_state_tick  = 0  end
    local bsi = state.broadcast_state_info
    for k in pairs(bsi) do bsi[k] = nil end
    -- LGM fields (always populated when we have a self slot, so allies
    -- always know our LGM status — even when our goal is "none").
    local _bsi_self_slot = lgm_registry.get(info.player_number or -1)
    if _bsi_self_slot and _bsi_self_slot.status ~= "unknown" then
      bsi.lgm_st = _bsi_self_slot.status
      if _bsi_self_slot.mx and _bsi_self_slot.my
         and _bsi_self_slot.status ~= "dead" then
        bsi.lgmx = tostring(_bsi_self_slot.mx)
        bsi.lgmy = tostring(_bsi_self_slot.my)
      end
      if state.pending_lgm_back then
        bsi.lgm_back = "1"
      end
    end
    bsi.tx = tostring(info.tankx >> 8)
    bsi.ty = tostring(info.tanky >> 8)
    if state.goal and state.goal.kind and state.goal.kind ~= "none" then
      bsi.goal = state.goal.kind
      if state.goal.substate and state.goal.substate ~= "" then
        bsi.sub = state.goal.substate
      end
      if state.goal.target_id and state.goal.target_id >= 0 then
        bsi.target = tostring(state.goal.target_id)
      end
      -- Reposition marker: tells the team someone is repositioning a pill, so
      -- everyone resets the time-based reposition discount (don't pile on).
      if state.goal.kind == "capture_pill" and state.goal.reposition then
        bsi.repos = "1"
      end
      -- Pill ids we're using as blockers in this take (so the team marks them
      -- utility). Computed in the team-tracking block above this tick.
      if state._blocker_pids and #state._blocker_pids > 0 then
        bsi.pblk = table.concat(state._blocker_pids, ",")
      end
      -- Goal tile mx/my as a fallback for the ally_claimed match path
      -- (goals.lua:3660-3664) when target_id isn't carried through.
      if state.goal.mx and state.goal.my then
        bsi.mx = tostring(state.goal.mx)
        bsi.my = tostring(state.goal.my)
      end
      -- Cost: pool_cache holds per-pool winners with .cost. Find the
      -- entry whose .goal matches our current goal (same kind + tile)
      -- and pluck its cost. state.goal_cost was the legacy slot but
      -- nothing ever assigned it; pool_cache is the actual source.
      if state.pool_cache then
        for pi = 0, 12 do
          local pce = state.pool_cache[pi]
          if pce and pce.goal
             and pce.goal.kind == state.goal.kind
             and pce.goal.mx   == state.goal.mx
             and pce.goal.my   == state.goal.my
             and pce.cost ~= nil then
            local raw = pce.cost - (pce.ally_claimed_pen or 0)
            bsi.cost = string.format("%.0f", raw)
            break
          end
        end
      end
    end

    local last = state.last_broadcasted_state_info
    local differs = false
    for k, v in pairs(bsi) do if last[k] ~= v then differs = true break end end
    if not differs then
      for k, v in pairs(last) do if bsi[k] ~= v then differs = true break end end
    end
    local heartbeat_due = (now - state.last_broadcast_state_tick) >= 1500
    local lgm_change_due = state.pending_lgm_broadcast == true
    if (differs or heartbeat_due or lgm_change_due) and not send_msg then
      send_msg = comms.format_state(bsi)
      -- Internal channel: messagedest=0 routes through the brain
      -- inbox of every allied bot in this sim and is shown locally
      -- on MSG_AI when run from the Brains menu — see
      -- brain_data.c's brainDataExtractInfo. No human's newswire
      -- ever sees /info state, so we can fire it on every goal
      -- change and the 30 s heartbeat without polluting chat.
      msg_dest = 0
      for k in pairs(last) do last[k] = nil end
      for k, v in pairs(bsi) do last[k] = v end
      state.last_broadcast_state_tick = now
      state.pending_lgm_broadcast = nil
      state.pending_lgm_back = nil
    end

    -- ── Build the /info extra payload (bse) and ship it on idle ticks ──
    -- /info extra ships supplementary fields that don't fit the 128-byte
    -- /info state budget.  Receiver MERGES into the slot (set_info is the
    -- replacing kind; merge_info is the additive kind).  We only send
    -- when send_msg is otherwise free AND the extras have changed since
    -- last ship; bandwidth stays tiny and the receiver always has fresh
    -- data within a tick or two of a goal transition.
    local bse = state.broadcast_state_extra
    if bse == nil then
      bse = {}; state.broadcast_state_extra = bse
    end
    for k in pairs(bse) do bse[k] = nil end
    -- Attack_pill setup + standoff coords packed as 8 hex chars (each
    -- tile coord is 0..255 = 2 hex chars: approach_mx, approach_my,
    -- standoff_mx, standoff_my).
    if state.goal and state.goal.kind == "attack_pill"
       and state.goal.approach_mx and state.goal.approach_my
       and state.goal.standoff_mx and state.goal.standoff_my then
      bse.p = string.format("%02X%02X%02X%02X",
                            state.goal.approach_mx & 0xFF,
                            state.goal.approach_my & 0xFF,
                            state.goal.standoff_mx & 0xFF,
                            state.goal.standoff_my & 0xFF)
    end
    local last_ext = state.last_broadcasted_state_extra
    if last_ext == nil then
      last_ext = {}; state.last_broadcasted_state_extra = last_ext
    end
    local extras_differ = false
    for k, v in pairs(bse)      do if last_ext[k] ~= v then extras_differ = true break end end
    if not extras_differ then
      for k, v in pairs(last_ext) do if bse[k] ~= v then extras_differ = true break end end
    end
    -- Only send extras when nothing else is going out this tick and
    -- there's actual change.  Heartbeat: every 1500 ticks, re-emit
    -- whatever extras are current so a freshly-joined or stale-slot
    -- ally picks them up without waiting for the next goal change.
    state.last_broadcast_extra_tick = state.last_broadcast_extra_tick or 0
    local extra_heartbeat_due = (now - state.last_broadcast_extra_tick) >= 1500
    if not send_msg
       and (next(bse) ~= nil or next(last_ext) ~= nil)
       and (extras_differ or extra_heartbeat_due) then
      send_msg = comms.format_extra(bse)
      -- Internal channel, same routing as /info state above.
      msg_dest = 0
      for k in pairs(last_ext) do last_ext[k] = nil end
      for k, v in pairs(bse)      do last_ext[k] = v end
      state.last_broadcast_extra_tick = now
    end

    -- Human goal-change line — fired only when the message slot is
    -- still free this tick (slate + extras have first dibs). Targets
    -- allied humans: info.player_bots carries the engine's
    -- PLAYER_FLAG_BOT bitmap (see braincore.c) so masking it out of
    -- info.allies leaves humans on our team. If a busy tick prevents
    -- the announcement going out, we defer; the next tick's slate
    -- heartbeat is at most 30 s away so the slot frees up quickly.
    if not send_msg and state.pending_human_goal_msg then
      local allies = info.allies or 0
      local bots   = info.player_bots or 0
      local human_allies = allies & ~bots
      if human_allies ~= 0 then
        send_msg = state.pending_human_goal_msg
        msg_dest = human_allies
      end
      -- Drop unconditionally: with no humans on our team there's
      -- nothing to announce, and we don't want this queue growing
      -- across ticks. A future ally join produces its own change.
      state.pending_human_goal_msg = nil
    end
  end

  -- Capture outbound for the chat_log overlay (debug-only).
  if BRAIN_DEBUG_MODE and send_msg and send_msg ~= "" then
    ally_state.chat_log_add("out", state.player_number, send_msg, now)
  end

  -- ────────────────────────────────────────────────────────────────────
  -- LGM-kill test harness: victim mode.
  --
  -- Bots marked with _G._BT_VICTIM=true (set by BrainTest's
  -- `-victims <ids>` CLI flag, injected via serverSimBotExecLua after
  -- Brain.open) dispatch their LGM to build walls at random tiles
  -- within 6 of an enemy tank and park their own tank, so the OTHER
  -- bots' kill_lgm targeting can be exercised + observed.  No-op
  -- without the flag (default for production runs).
  -- ────────────────────────────────────────────────────────────────────
  if _G._BT_VICTIM then
    state.test_lgm = state.test_lgm or {}
    local tl = state.test_lgm
    -- Pick a fresh target tile when:
    --   * we don't have one yet
    --   * the LGM has been dispatched for > 300 ticks (~6s) without
    --     the engine turning the tile into a wall (LGM dead / stuck /
    --     enemy shot the wall down / etc.)
    --   * the target tile became a wall (or building / halfbuild) —
    --     LGM succeeded, time to send it out again
    local need_new = not tl.target_mx
    if not need_new and tl.dispatched_tick
       and now - tl.dispatched_tick > 300 then
      need_new = true
    end
    if not need_new and tl.target_mx then
      local tt = U.ttype(tl.target_mx, tl.target_my)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
        need_new = true
      end
    end
    if need_new then
      -- Anchor for "random within 6": enemy tank if visible (real
      -- test scenario), otherwise our own tank tile so the LGM goes
      -- out IMMEDIATELY at tick ~0 without waiting for perception to
      -- spot the hunter. The hunter will discover the LGM once it
      -- closes — the test still exercises kill_lgm cleanly.
      local anchor_mx, anchor_my, anchor_src
      if state.perc and state.perc.enemy_tanks
         and state.perc.enemy_tanks[1] then
        anchor_mx = state.perc.enemy_tanks[1].mx
        anchor_my = state.perc.enemy_tanks[1].my
        anchor_src = "enemy"
      else
        anchor_mx = info.tankx >> 8
        anchor_my = info.tanky >> 8
        anchor_src = "self"
      end
      -- Random tile in ±6 box around anchor.  Must be a terrain the
      -- engine will accept BUILDMODE_BUILD on (grass / road). Forest
      -- works too but BUILDMODE_BUILD just farms it; we want the LGM
      -- to put up an actual wall so the action + location is bait.
      for _ = 1, 20 do
        local mx = anchor_mx + math.random(-6, 6)
        local my = anchor_my + math.random(-6, 6)
        if U.in_map(mx, my) then
          local tt = U.ttype(mx, my)
          if tt == C.T_GRASS or tt == C.T_ROAD then
            tl.target_mx, tl.target_my = mx, my
            tl.dispatched_tick = now
            if BRAIN_DEBUG_MODE then
              print(string.format(
                "[VICTIM] dispatch LGM to (%d,%d) near %s@(%d,%d)",
                mx, my, anchor_src, anchor_mx, anchor_my))
            end
            break
          end
        end
      end
    end
    if tl.target_mx then
      build_cmd = { x = tl.target_mx, y = tl.target_my,
                    action = BUILDMODE_BUILD }
      -- Park: drop movement so the tank doesn't compete for
      -- the hunter's attention as a tank target.  KEY_SLOWER lets
      -- engine decel naturally without us holding speed up.
      keys = KEY_SLOWER
      taps = 0
    end
    -- Viz: cyan rect on the LGM's current build target + big VICTIM
    -- label over the tank so it's instantly clear which bot is in
    -- test-victim mode.  (Nested if's rather than a compound
    -- BRAIN_DEBUG_MODE-and-... so lua_strip can match its single-line
    -- block pattern.)
    if BRAIN_DEBUG_MODE then
      if tl.target_mx and viz.is_on("test_lgm_target") then
        viz.rect("test_lgm_target",
                 tl.target_mx, tl.target_my,
                 tl.target_mx + 1, tl.target_my + 1,
                 80, 220, 220, 200, false)
        viz.text("test_lgm_target",
                 tl.target_mx + 0.5, tl.target_my + 0.5,
                 "VICTIM LGM", "center", 80, 220, 220, 255, 0.45)
      end
      if viz.is_on("test_victim_marker") then
        local twx = info.tankx / 256.0
        local twy = info.tanky / 256.0
        -- Big red outline two tiles wide centered on the tank.
        viz.rect("test_victim_marker",
                 twx - 1.0, twy - 1.0, twx + 1.0, twy + 1.0,
                 255, 60, 60, 230, false)
        -- VICTIM label sits ABOVE the tank so it doesn't overlap the
        -- tank sprite or the tank-position HUD text.  Big scale so
        -- it's readable from any zoom.
        viz.text("test_victim_marker", twx, twy - 1.6,
                 "VICTIM", "center", 255, 60, 60, 255, 1.2)
      end
    end
  end

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
  -- Debug to file since console may not be visible. Gated on
  -- BRAIN_DEBUG_MODE so the strip removes it from the production
  -- brain (live game, no instruction = no file I/O).
  if BRAIN_DEBUG_MODE then
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
  end
  rp(string.format(TAG .. " CLICK: (%d,%d) shift=%s ctrl=%s", mx, my, tostring(mods.shift), tostring(mods.ctrl)))

  if mods.shift then
    -- Toggle pill inspect overlay. f2 is gated on BRAIN_DEBUG_MODE for
    -- the same reason as the on_click line above.
    local f2 = BRAIN_DEBUG_MODE and io.open("click_lua.log", "a") or nil
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
