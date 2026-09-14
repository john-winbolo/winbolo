local function __idiv(a,b) return math.floor(a/b) end
local bit = require('bitcompat')

-- LuaJIT (Lua 5.1) math.atan takes ONE argument and silently ignores a second,
-- so the two-arg atan2 form the brain uses for headings (aim_at, aim angles)
-- returns garbage on LuaJIT — the tank can't steer. Restore correct two-arg
-- behaviour from math.atan2 (present on LuaJIT/5.1, absent on PUC-Lua 5.4 where
-- math.atan is already two-arg). One-time, no-op on PUC. See docs/luajit-port.md.
if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

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

-- ══════════════════════════════════════════════════════════════════════════
-- PER-BOT CONSTANT OVERRIDES — must run HERE, before any other require
-- ══════════════════════════════════════════════════════════════════════════
-- Two BRAIN_INIT_ARG tokens write straight into this bot's constants table:
--
--   "preset=NAME"       -> apply every entry of C.PRESETS[NAME] (see
--                          constants.lua; `keel` is the pre-change value of
--                          every behaviour knob changed since the KEEL tag).
--   "cfg=NAME=VALUE"    -> set C.NAME for THIS bot. Repeatable. VALUE is read
--                          as a number if it looks like one, as a boolean for
--                          "true"/"false", and as a plain string otherwise.
--
-- The per-(mode,difficulty) LEVEL bundle (C.MODE_LEVELS) is applied FIRST,
-- then presets, then every cfg=, so precedence is level < preset < cfg and
-- an explicit cfg= always wins no matter where in the token list it sits.
--
-- WHY IT IS UP HERE AND NOT IN THE TICK-1 TOKEN BLOCK (search BRAIN_INIT_ARG,
-- ~line 1250) where every other token is parsed: several modules CAPTURE a
-- constant at require time and never look at C again --
--   squad.lua      BLITZ_MIN / BLITZ_MAX / BLITZ_MIN_SUICIDERS
--   goals.lua      REFUEL_MULT, TAKE_COVER_REJECT_COST
--   threat.lua     PRED_DISK_SIZE / PRED_DISK_R (a precomputed disk)
--   logger.lua     MAX_ENTRIES
--   pill_portfolio FRONT_NEAR_RADIUS
--   (and every module's TAG, from C.BRAIN_NAME)
-- -- so an override applied on the first think is already too late for them:
-- `cfg=SQUAD_MAX_SIZE=1` set at tick 1 would leave squad.lua's BLITZ_MAX on
-- the value it read at require time and do nothing at all. Running here, in
-- the gap between `require("constants")` and the first module that requires
-- it, is what makes the override mean the same thing for every reader.
-- Each bot has its own lua_State, so this table is this bot's alone.
--
-- The other tokens stay where they are: they call setters (squad.set_*,
-- PP.set_targets, goals.set_refuel_mult) that are read live every tick, so
-- they have no such ordering problem, and they need `state`, which does not
-- exist yet up here.
--
-- Nothing can be PRINTED from here: this runs at chunk load, long before the
-- session's print2 file exists. Warnings and the per-override log lines latch
-- into _INIT_CFG_LOG / _INIT_CFG_WARN and are emitted in the captured-tick
-- window with the rest of the [portfolio]/[blitz]/[refuel] config lines.
local _INIT_CFG_LOG  = {}     -- {"[cfg] NAME=VALUE (init_arg)", ...}
local _INIT_CFG_WARN = nil    -- one string, same shape as state._cfg_warn

local function _cfg_warn_add(fmt, ...)
  _INIT_CFG_WARN = (_INIT_CFG_WARN or "") .. string.format(fmt, ...) .. " "
end

-- Write one NAME=VALUE into C, refusing anything that would not survive the
-- rest of the brain: a constant that does not exist (a typo silently doing
-- nothing is the worst outcome for a bench), a table/function constant, and a
-- type change -- `cfg=SOME_FLAG=0` is a particular trap, since 0 is TRUE in
-- Lua and would turn a flag ON while reading as "off".
local function _cfg_set(name, value, source)
  local cur = C[name]
  if cur == nil then
    _cfg_warn_add("[cfg] UNKNOWN CONSTANT '%s' (%s) -- no such name in constants.lua; IGNORED.",
                  tostring(name), source)
    return false
  end
  if type(cur) == "table" or type(cur) == "function" then
    _cfg_warn_add("[cfg] '%s' is a %s (%s) -- only numbers, booleans and strings can be overridden; IGNORED.",
                  tostring(name), type(cur), source)
    return false
  end
  if type(value) ~= type(cur) then
    _cfg_warn_add("[cfg] '%s'=%s (%s) is a %s but the constant is a %s; IGNORED.",
                  tostring(name), tostring(value), source, type(value), type(cur))
    return false
  end
  C[name] = value
  _INIT_CFG_LOG[#_INIT_CFG_LOG + 1] = string.format("[cfg] %s=%s (%s)",
                                                    name, tostring(value), source)
  return true
end

do
  local a = rawget(_G, "BRAIN_INIT_ARG")
  if type(a) == "string" and a ~= "" then
    -- Same token split as the tick-1 block: ',' or ';'. A scenario's
    -- spawn_bot init string uses ';' and so must a command-line [..] suffix
    -- (the CLI parser eats commas).
    local presets, cfgs = {}, {}
    for tok in a:gmatch("[^,;]+") do
      tok = tok:gsub("%s", "")
      local pname = tok:match("^preset=(.+)$")
      local cname, cval = tok:match("^cfg=([%a_][%w_]*)=(.*)$")
      local dname = tok:match("^difficulty=(.*)$")
      local mname = tok:match("^mode=(.*)$")
      if pname then
        presets[#presets + 1] = pname
      elseif cname then
        cfgs[#cfgs + 1] = { cname, cval }
      elseif mname then
        -- "mode=<key>" -- the host's per-bot lobby choice of MODE, appended
        -- to this arg by bot_manager.c at brain-create time. The key comes
        -- from this brain's own modes.txt, so the vocabulary is whatever
        -- that file lists ("default", "survival", ...) and this side only
        -- checks the shape. Written into C.MODE RIGHT HERE (not queued into
        -- cfgs) so the level bundle below can read the chosen mode; it is
        -- type-checked and logged like every other override. Precedence:
        -- level < preset < cfg (a later cfg=MODE= would still win).
        mname = mname:lower()
        if mname:match("^[a-z0-9_]+$") then
          _cfg_set("MODE", mname, "mode")
        else
          _cfg_warn_add("[mode] BAD TOKEN '%s' -- want mode=<key> of [a-z0-9_]; IGNORED.", tok)
        end
      elseif dname then
        -- "difficulty=<key>" -- the host's per-bot lobby choice of LEVEL
        -- inside that mode, likewise appended by bot_manager.c. modes.txt
        -- defines which keys a mode has, so any [a-z0-9_] key is accepted
        -- here rather than the three the default mode happens to use.
        -- Written into C.DIFFICULTY RIGHT HERE (not queued) so the level
        -- bundle below reads it; MODE_LEVELS[C.MODE][C.DIFFICULTY] then
        -- applies BEFORE any preset=. "normal" is the old name for medium;
        -- the C side never sends it, but a hand-written arg might.
        dname = dname:lower()
        if dname == "normal" then dname = "medium" end
        if dname:match("^[a-z0-9_]+$") then
          _cfg_set("DIFFICULTY", dname, "difficulty")
        else
          _cfg_warn_add("[difficulty] BAD TOKEN '%s' -- want difficulty=<key> of [a-z0-9_]; IGNORED.", tok)
        end
      elseif tok:sub(1, 4) == "cfg=" then
        _cfg_warn_add("[cfg] BAD TOKEN '%s' -- want cfg=NAME=VALUE; IGNORED.", tok)
      end
      -- Everything else is one of the tick-1 tokens; not our business.
    end
    -- LEVEL BUNDLE (lowest precedence, applied BEFORE presets): the per-(mode,
    -- difficulty) scalar overrides from C.MODE_LEVELS, pushed through the same
    -- _cfg_set path so its type/table refusals and logging apply with no new
    -- validation. MODE and DIFFICULTY were resolved inline above. A missing
    -- mode/difficulty key (or hard = {}) simply applies nothing.
    --
    -- The level is selected by the difficulty= (and mode=) TOKEN only. A later
    -- cfg=DIFFICULTY= changes the label C.DIFFICULTY but does NOT apply a
    -- different bundle -- the bundle was already chosen when this block ran.
    -- That is intended: cfg= is a single-knob override, not a level selector,
    -- so bench a level with difficulty=<level>, not cfg=DIFFICULTY=<level>.
    -- Every bundle value is FIRST-PASS, to be benched preset=keel vs
    -- difficulty=<level> per the approve-values rule; hard = {} is empty by
    -- design so a default game is bit-for-bit today's brain.
    do
      local mode, diff = C.MODE, C.DIFFICULTY
      local mtbl = C.MODE_LEVELS and C.MODE_LEVELS[mode]
      local ltbl = mtbl and mtbl[diff]
      if type(ltbl) == "table" then
        -- Sorted so the log reads the same on every run (see the preset loop).
        local keys = {}
        for k in pairs(ltbl) do keys[#keys + 1] = k end
        table.sort(keys)
        local n = 0
        for _, k in ipairs(keys) do
          if _cfg_set(k, ltbl[k], "level " .. tostring(mode) .. "/" .. tostring(diff)) then n = n + 1 end
        end
        _INIT_CFG_LOG[#_INIT_CFG_LOG + 1] =
          string.format("[level] %s/%s applied (%d values)", tostring(mode), tostring(diff), n)
      end
    end
    -- Presets FIRST, so an explicit cfg= wins wherever it sits in the list.
    for _, pname in ipairs(presets) do
      local tbl = C.PRESETS and C.PRESETS[pname]
      if type(tbl) ~= "table" then
        local known = {}
        if C.PRESETS then for k in pairs(C.PRESETS) do known[#known + 1] = k end end
        table.sort(known)
        _cfg_warn_add("[preset] UNKNOWN PRESET '%s' -- known: %s; IGNORED.",
                      tostring(pname), table.concat(known, " "))
      else
        -- Sorted so the log reads the same on every run (pairs() order is not
        -- reproducible, and these lines are compared between runs).
        local keys = {}
        for k in pairs(tbl) do keys[#keys + 1] = k end
        table.sort(keys)
        local n = 0
        for _, k in ipairs(keys) do
          if _cfg_set(k, tbl[k], "preset " .. pname) then n = n + 1 end
        end
        _INIT_CFG_LOG[#_INIT_CFG_LOG + 1] =
          string.format("[preset] %s applied (%d values)", pname, n)
      end
    end
    for _, kv in ipairs(cfgs) do
      local name, raw = kv[1], kv[2]
      local v
      if raw == "true" then v = true
      elseif raw == "false" then v = false
      elseif tonumber(raw) then v = tonumber(raw)
      else v = raw end
      _cfg_set(name, v, "init_arg")
    end
  end
end

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
local demine  = require("demine")
-- local bpc  = require("bpc")  -- removed: unified into attack_pill
local log     = require("logger")
local danger  = require("danger")
local builder = require("builder")
local bpool   = require("builder_pool")   -- LGM side-quests (the parallel track)
local metrics = require("metrics")
local changes = require("changes")
local percept  = require("perception")
local strategy = require("strategy")
local comms    = require("comms")
local threat   = require("threat")
local hearing  = require("hearing")
local print2   = require("print2")
local opt      = require("optimize")
local prof     = require("profiler")
local shot_tracker = require("shot_tracker")
local viz      = require("viz")
local ally_state = require("ally_state")
ally_state.init()
local squad = require("squad")
local reposition_vote = require("reposition_vote")
local pill_table = require("pill_table")
local PP = require("pill_portfolio")
local lgm_registry = require("lgm_registry")
local circles = require("circles")
lgm_registry.init()

local Brain = {}

-- Manual control state (must be before Brain.think so it captures the upvalue)
local manual_active = false
local manual_keys = 0
-- Manual BUILD state: which build the 1-5 keys/HUD buttons currently select
-- (1=Trees/farm, 2=Road, 3=Wall/building, 4=Pillbox, 5=Mine — the BUILDMODE_*
-- order), and a one-shot {x,y,action} latched by a map click that the manual
-- think emits for a single tick (the engine kicks off one LGM build per emit).
local manual_build_action = 1          -- BUILDMODE_FARM
local manual_pending_build = nil
-- Display labels for the HUD build menu, indexed by BUILDMODE (1..5).
local MANUAL_BUILD_LABELS = { "TREE", "ROAD", "WALL", "PILL", "MINE" }
-- Representative colors for each build type (indexed by BUILDMODE): forest green
-- trees, gray asphalt road, brick-orange wall, amber pillbox, danger-red mine.
local MANUAL_BUILD_COLORS = {
  { 60, 190, 70 },    -- TREE  — forest green
  { 120, 130, 150 },  -- ROAD  — asphalt gray
  { 205, 110, 45 },   -- WALL  — brick orange
  { 230, 195, 60 },   -- PILL  — amber pillbox
  { 215, 60, 55 },    -- MINE  — danger red
}

-- ── Cause-of-death diagnostics (DEBUG ONLY) ──────────────────────────────
-- File-level upvalues, deliberately NOT fields on `state`: nothing outside
-- the two `if BRAIN_DEBUG_MODE` blocks that maintain them ever reads them,
-- and keeping them off `state` guarantees they can't perturb any iteration
-- over state or any decision. They exist solely to make the one-line DEATH
-- print2 below say WHY the tank died.
--
-- The cause itself comes from the ENGINE, not from a guess: the server sends
-- EVENT_TANK_KILLED with data = [killer, killed, deathCause, carriedPills]
-- (gameEventDataSize == 4), so in Lua d[3] is LAST_DEATH_BY_DEEPSEA (1) or
-- LAST_DEATH_BY_SHELL (2) and d[1] == 255 (NEUTRAL) means a pillbox fired it.
-- The one gap is a mine kill: tankMineDamage never calls the tankKill
-- callback, so no event is emitted and we fall back to "mine_or_other".
-- ONE table, not nine locals: Brain.think is already within a couple of slots
-- of Lua's hard 60-upvalue-per-function limit, and nine separate upvalues
-- overflowed it ("function at line 1121 has more than 60 upvalues" -- the
-- brain then failed to load at all). Fields, in order of use:
--   evt_cause  engine cause byte from EVENT_TANK_KILLED
--   evt_killer killer player from that event (255 = NEUTRAL = a pillbox)
--   evt_tick   tick we saw that event
--   alive_tick / alive_mx / alive_my / alive_armour  last ALIVE snapshot
--   drop_tick  last tick armour went DOWN while alive
--   reported   one DEATH line per death episode
local _dbg_death = { reported = false }

local AUTOSTART = true
local ENABLE_LOGGING = false

-- Shot-path safety check shared with steering.lua (single implementation
-- lives there as M.shot_path_clear). Blocks on walls, pillboxes, hostile
-- bases, and allied tanks. Forests and enemy tanks are OK.
local _shot_path_clear_init = steer.shot_path_clear

-- Hoisted lookup tables (don't realloc every tick of every Brain.think).
-- Used by stuck detection / urgent-replan gating downstream — file-scope
-- so the loop bodies just do membership checks.
local ATTACK_STATIONARY_SUBS = {
  plan_position=true, position=true, aim=true, approach=true, build_walls=true,
  in_range_position=true, in_range_aim_pre=true, in_range_aim=true,
  in_range_aim_finetune=true, shoot_pill=true, engage=true, curve_away=true,
  rush=true, disengage=true, gather_trees=true,
  -- blitz_wait: a blitzer deliberately HOLDS at its setup for the commander's
  -- GO — sitting still is intentional, so it must not trip the stuck-flee timer.
  blitz_wait=true,
}
local PP_STATIONARY_SUBS = {
  dispatch=true, wait_place=true, prewait=true, advance=true,
  shield_engage=true, engage=true, reposition=true, finish=true,
  select_pill=true,
}
local TANK_COMBAT_STATIONARY_SUBS = {
  engage=true, close=true, disengage=true,
}
-- Defend heat (ARRIVED-phase win): parked in firing range, deliberately
-- putting HEAT_PILL_SHOTS shells into our own pill to anger it. Aiming /
-- firing / the post-sequence hold are all intentional stillness.
local DEFEND_HEAT_STATIONARY_SUBS = {
  heat_pill_aim=true, heat_pill_shoot=true, heat_done=true,
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
      if log.open(log.make_filename("brain_p" .. state.player_number), state.tick, state.engine_tick0) then
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

  -- Per-think heap churn from the host's counting allocator. Absent on
  -- hosts that don't install it (WinBolo client, headless server); emit
  -- zeros there so the JSON shape stays stable for downstream readers.
  local alloc_n, alloc_bytes = 0, 0
  if brain_alloc_stats then alloc_n, alloc_bytes = brain_alloc_stats() end

  return string.format(
    '{"tier":%d,"override":%s,"last_ms":%.2f,"target_ms":%.2f,"ratio_ewma":%.2f,"think_total_ms":%s,"alloc_n":%d,"alloc_kb":%.1f,"levels":[%s],"sections":[%s]}',
    tier,
    (type(ovr) == "number") and tostring(math.floor(ovr)) or "null",
    last_ms, tgt_ms, sm,
    think_total_ms_str,
    alloc_n, alloc_bytes / 1024,
    table.concat(rows, ","),
    table.concat(sec_parts, ","))
end

-- idx -> viz_id legend for the brain recorder (winbolods). Lets the BrainTest
-- loader remap recorded overlay viz_idx values to category names.
function Brain.viz_legend_json()
  return viz.legend_json()
end

-- CSV of overlay viz indices the recorder should drop (cosmetic/huge ones).
function Brain.viz_record_skip_csv()
  return viz.record_skip_idx_csv()
end

function Brain.get_pool_breakdown_json()
  if not BRAIN_POOL_VIZ then
    return string.format(
      '{"phase":"%s","tick":%d,"replan_left":0,"bot":%d,"sections":[{"id":"off","label":"Pool viz","rows":[{"id":0,"mx":0,"my":0,"cost":0,"formula":"BRAIN_POOL_VIZ is off","stale":-1,"active":false,"imminent":false,"reject":null}]}]}',
      (state and state.phase) or "?", (state and state.tick) or 0, (state and state.player_number) or 0)
  end
  local ok, result = pcall(goals.get_pool_breakdown_json, state)
  if ok then return result end
  -- The pool builder threw. Don't return empty sections — that renders as a
  -- BLANK / vanished P window with no clue why (drove debugging nuts when it
  -- fired for one bot). Surface the error AS a visible row so the window stays
  -- up and shows the cause, and log it to print2 so it's grep-able per bot.
  local emsg = tostring(result):gsub('\\', '\\\\'):gsub('"', "'")
                               :gsub('[\n\r\t]', ' ')
  io.stderr:write("BRAIN ERROR get_pool_breakdown_json: " .. tostring(result) .. "\n")
  print2("POOL_BREAKDOWN_ERROR bot=" .. tostring(state.player_number)
         .. " t=" .. tostring(state.tick) .. " role=" .. tostring(state.squad_role)
         .. " : " .. tostring(result))
  return string.format(
    '{"phase":"error","tick":%d,"replan_left":0,"bot":%d,"sections":[{"id":"err","label":"POOL ERROR","rows":[{"id":0,"mx":0,"my":0,"cost":0,"formula":"%s","stale":-1,"active":false,"imminent":false,"reject":"error"}]}]}',
    state.tick or 0, state.player_number or 0, emsg)
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
  return bit.bor((bit.lshift(p.mx, 8)), 128), bit.bor((bit.lshift(p.my, 8)), 128)
end

function Brain.shotsim_chosen_standoff_wu()
  local g = state.goal
  if not g then return nil end
  -- Use the SAME source as the attack_chosen_standoff_marker so the POI always
  -- matches the on-map marker — for every take mode, including blitz soldiers
  -- (non-PPT, so _shield_scan is nil but goal.standoff_* is set).
  local sfx = g.standoff_fx or (g.standoff_mx and (g.standoff_mx + 0.5))
  local sfy = g.standoff_fy or (g.standoff_my and (g.standoff_my + 0.5))
  if sfx and sfy then
    return math.floor(sfx * 256 + 0.5), math.floor(sfy * 256 + 0.5)
  end
  -- Fallback: the shield-scan standoff candidate during pre-commit PPT planning.
  local s = g._shield_scan and g._shield_scan.standoff
  if s and s.cx and s.cy then
    return math.floor(s.cx * 256 + 0.5), math.floor(s.cy * 256 + 0.5)
  end
  return nil
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
  -- DETERMINISM FOR TESTS. The capacity tier is driven by wall-clock
  -- lastThinkMs, so the same seed produces different play on a loaded machine:
  -- two headless servers sharing a CPU both tier down, CAPTURE_ROUTE_MIN_TIER
  -- stops the route probe, and the bot makes different choices. Running the
  -- sea-pill variants two at a time turned a raft the bot had correctly split
  -- into one it sailed straight into. WINBOLO_BRAIN_TIER pins the tier so a
  -- functional test measures the BRAIN rather than the machine it ran on;
  -- unset (every real game) the dynamic controller is untouched.
  if os and os.getenv then
    local t = tonumber(os.getenv("WINBOLO_BRAIN_TIER") or "")
    if t and t >= 1 and t <= 10 then
      _G._BT_TIER_OVERRIDE = math.floor(t)
      print(string.format(TAG .. " capacity tier PINNED to %d by WINBOLO_BRAIN_TIER",
                          _G._BT_TIER_OVERRIDE))
    end
  end
  -- Host-callable log flush. BrainTest invokes this (via the C side) when
  -- the sim is paused so the batched print2 log is written to disk
  -- immediately for reading. No-op in opt/non-debug runs (force_flush
  -- early-returns when _PRINT2_ENABLED is false).
  _G.__brain_flush_logs = function() print2.force_flush() end
  -- Kill-path partial flush. braincore.c's killed branch pcalls this global
  -- when the per-tick budget hook aborts think(), passing the Lua
  -- "source:line" where the budget ran out. Without it the killed tick's
  -- buffered print2 lines — exactly the ones needed to diagnose the overrun —
  -- are dropped by the next tick's set_tick(). No-op on an empty buffer, so
  -- opt/ and non-debug runs pay nothing.
  -- Chained: print2's partial buffer FIRST (unchanged behavior), then the
  -- jsonl logger's per-tick accumulators. logger.event/reason append all
  -- tick and are drained only by log_tick — which a kill skips entirely, so
  -- without this drain they carry into the next tick, make its record bigger,
  -- and the bot is budget-killed every tick from then on. Each leg is pcall'd
  -- so a logger fault can't lose the print2 flush (and vice versa).
  _G.brain_flush_killed = function(site)
    pcall(print2.flush_killed, site)
    if log.flush_killed then pcall(log.flush_killed, site) end
  end
  opt("BEGIN Brain.open player=", info.player_number)
  print2("=== BRAIN STARTUP === player=", info.player_number,
         " name=", tostring(info.player_name),
         " debug_session_dir=", tostring(_G.DEBUG_SESSION_DIR))
  local t_open0 = BRAIN_PROFILE and clock_us() or 0
  -- Diagnostic: emit a SELF_DR line per pool-6 candidate per replan
  -- showing raw / subtracted / c_reduction / manual_reduction so we can
  -- verify the target-pill danger subtraction in pool_6's spot_cost.
  -- BRAIN_DEBUG_MODE-gated so it's stripped from opt/.
  -- GC tuning. Production runs on LuaJIT, where the 5.4->5.1 compat shim in
  -- luabrainshandler.c swallows collectgarbage("generational") and returns 0 —
  -- so this call had no effect at all and the brain ran on LuaJIT's stock
  -- pause/stepmul. Use the knobs LuaJIT has (see C.GC_PAUSE / C.GC_STEPMUL for
  -- the measured values), and keep the 5.4 generational request for a PUC-Lua
  -- host, where setpause/setstepmul would flip the collector back to
  -- incremental. `jit` is the standard "am I on LuaJIT" probe.
  if jit then
    collectgarbage("setpause",   C.GC_PAUSE)
    collectgarbage("setstepmul", C.GC_STEPMUL)
  else
    collectgarbage("generational", 10, 100)
  end
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
  local t_open_register = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  viz/panel/POI register done %.2f ms", (t_open_register - t_open0) / 1000))
  end

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
  local t_open_resets = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  module resets done %.2f ms", (t_open_resets - t_open_register) / 1000))
  end

  -- ── Session-unique tick numbering ──────────────────────────────────────
  -- state.tick used to start at 0 on every Brain.open. That is fine in a
  -- game where every bot is created once, but in Survival each wave's bots
  -- are created fresh (game.spawn_bot -> serverSimCreateBot -> a new
  -- lua_State), so five lives all ran t=1..7375: print2_bot10.log held five
  -- overlapping tick ranges, player10.jsonl was truncated per life, and
  -- "bot10 at tick 5785" meant five different moments.
  --
  -- BRAIN_START_ENGINE_TICK is the server sim's tick when THIS instance was
  -- created, injected by luaBrainInstanceCreate (always a number; 0 at game
  -- start, and 0 on any host that stages nothing). Brains think once per two
  -- engine ticks, so half of it is where the session clock already stands in
  -- brain ticks. Seeding from it makes every later t= unique for the session
  -- while leaving a game-start bot (seed 0) numbered exactly as before.
  --
  -- state.birth_tick is the same number kept under a name that says what it
  -- means: any "how long since X" field that must measure from THIS life's
  -- start is initialised to it rather than to 0 (a raw 0 would now read as
  -- "birth_tick ticks ago", i.e. long ago, and fire on the bot's first tick).
  local _eng_t0 = rawget(_G, "BRAIN_START_ENGINE_TICK")
  _eng_t0 = (type(_eng_t0) == "number" and _eng_t0 > 0) and math.floor(_eng_t0) or 0
  state.engine_tick0  = _eng_t0
  state.tick_seed     = math.floor(_eng_t0 / 2)
  state.birth_tick    = state.tick_seed
  state.tick          = state.tick_seed
  -- Re-stamp the optimize log now the seed is known; the call at the top of
  -- Brain.open had to run before it and stamped 0, which would file this
  -- life's open under "tick 0" alongside every other life's.
  opt.set_tick(state.tick)
  state.player_number = info.player_number
  _G._BRAIN_SELF_PN   = info.player_number
  state.player_name   = (info.player_names and info.player_names[info.player_number + 1]) or ""
  state.debug_log     = (state.player_name == "Bot 1" or info.player_number == 0)
  state.send_open_msg = true

  -- One line per life, naming the tick this brain starts counting from and
  -- the engine tick it was created at. This is the row that tells you which
  -- of a Survival session's five lives a "t=5785" line belongs to.
  --
  -- print2 needs its per-bot file bound and its tick stamped before it will
  -- route a line, and set_tick clears the line buffer — so bind, stamp, write
  -- and flush here rather than letting the first Brain.think wipe the line.
  -- The whole block is BRAIN_DEBUG_MODE-gated so lua_strip removes it from
  -- opt/ entirely; the plain print below is the release-visible copy.
  if BRAIN_DEBUG_MODE then
    print2.set_bot(_G.BT_BOT_INDEX or info.player_number or 0)
    print2.set_tick(state.tick)
    print2(string.format("BRAIN_OPEN t=%d engine_tick=%d life_seed=%d pn=%d name=%s",
                         state.tick, state.engine_tick0, state.tick_seed,
                         info.player_number or -1, state.player_name))
    print2.flush()
  end
  print(string.format("%s BRAIN_OPEN t=%d engine_tick=%d life_seed=%d pn=%d",
                      TAG, state.tick, state.engine_tick0, state.tick_seed,
                      info.player_number or -1))

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
  -- Replan phase. Every reader tests `(now + replan_offset) % GOAL_REPLAN_INTERVAL
  -- == 0`, so the offset is what decides WHICH ticks of this bot's life are
  -- replan ticks. `now` used to start at 0 on every Brain.open, so that grid was
  -- anchored to the bot's own life: the Nth replan always landed on the same tick
  -- of the life whatever the game clock said. Seeding state.tick from the engine
  -- tick (above) moved the anchor to absolute game time, which shifts a mid-game
  -- bot's whole replan cadence by (tick_seed % INTERVAL) ticks relative to its
  -- life -- the one thing the tick-sentinel audit missed, because it is a
  -- "every N ticks" schedule rather than a "first N ticks" test. Subtracting the
  -- seed here puts the grid back on the life clock: the expression above then
  -- evaluates `(age + random) % INTERVAL`, exactly what it computed before the
  -- seeding. Folding it into the offset (rather than editing each `now +
  -- replan_offset` site) keeps every reader -- the timer, the predicted-replan
  -- dij slip, the ticks_left displays -- consistent by construction.
  -- Bit-for-bit unchanged at tick_seed 0, i.e. for every bot created at game
  -- start; only a bot created mid-game (a Survival wave, a scenario spawn_bot)
  -- moves, and it moves back to what it did before the seeding.
  state.replan_offset     = (math.random(0, C.GOAL_REPLAN_INTERVAL - 1)
                             - state.tick_seed) % C.GOAL_REPLAN_INTERVAL
  state.goal_set_tick     = state.tick  -- tick when current goal was chosen (for commitment hysteresis). Seeded to birth, not 0: `now - goal_set_tick` is "how long we've held this goal", so a raw 0 would read as "held it forever" on a mid-game bot's first tick and skip the commitment window entirely.
  state.goal_cooldowns    = {}   -- abandoned goals: { [key] = expiry_tick }
  state.goal_history      = {}   -- circular buffer of last N picked goals (oscillation detection)
  state.blitz_calls       = {}   -- open blitz calls: { [commander_pn] = { pill, tick } }
  state._blitz_query_send = true -- discovery query on join, so we learn calls already open

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
  -- Seeded to birth, not 0: the heartbeat test is `now - last >= 1500`, so a
  -- raw 0 makes a mid-game bot's first tick look 1500+ ticks overdue and fire
  -- a broadcast immediately instead of after the usual 30 s.
  state.last_broadcast_state_tick   = state.tick

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

  local t_open_state = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  state init done %.2f ms", (t_open_state - t_open_resets) / 1000))
  end

  -- World knowledge
  world.bases = {}
  world.pills = {}
  W.reset(world)
  local t_open_wreset = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  W.reset done %.2f ms", (t_open_wreset - t_open_state) / 1000))
  end
  -- state.tick, not 0: this pass stamps last_seen / obs_tick / placed_tick on
  -- every object visible at open, and those are read as `now - stamp` ages. A
  -- literal 0 makes everything the bot can see at birth look maximally stale
  -- once ticks are seeded from the engine clock. Identical at seed 0.
  W.update(world, info, state.tick)
  local t_open_wupd = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  W.update (initial) done %.2f ms", (t_open_wupd - t_open_wreset) / 1000))
  end

  -- Pre-build edge costs + pre-warm threat so tick-1 doesn't pay for them.
  -- pf->map is set by the C host before brain.open() is called.
  cpf.rebuild_edge_costs()
  if BRAIN_PROFILE then
    opt(string.format("  rebuild_edge_costs done %.2f ms", (clock_us() - t_open_wupd) / 1000))
  end
  local _t_open_danger = BRAIN_PROFILE and clock_us() or 0
  danger.update(info, state.tick)  -- state.tick, not 0: shell-prediction expiries are absolute ticks, and a 0 here writes entries that are already expired on a seeded bot. Identical at seed 0.
  if BRAIN_PROFILE then
    opt(string.format("  danger.update done %.2f ms", (clock_us() - _t_open_danger) / 1000))
  end
  local _t_open_threat = BRAIN_PROFILE and clock_us() or 0
  threat.update(state, world, info)
  if BRAIN_PROFILE then
    opt(string.format("  threat.update done %.2f ms", (clock_us() - _t_open_threat) / 1000))
  end

  -- Strategy phase detection
  strategy.init(state)
  if BRAIN_PROFILE then
    opt(string.format("  strategy.init done %.2f ms", (clock_us() - t_open_wupd) / 1000))
  end

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
  local t_open_logsetup = BRAIN_PROFILE and clock_us() or 0
  if log_fname then
    -- Pass this life's starting tick + the engine tick it was created at so
    -- the jsonl carries a life marker; the logger appends, so a session in
    -- which this bot is re-created (Survival waves) keeps every life's rows.
    if log.open(log_fname, state.tick, state.engine_tick0) then
      log.dump_map()
      log.dump_world(world)
    end
  end
  local t_open_logdump = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  log.dump_map+world done %.2f ms (fname=%s)",
      (t_open_logdump - t_open_logsetup) / 1000, tostring(log_fname)))
  end

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
  if BRAIN_PROFILE then
    opt(string.format("END Brain.open total=%.2f ms", (clock_us() - t_open0) / 1000))
  end
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
        local pmx = bit.rshift(ob.x, 8)
        local pmy = bit.rshift(ob.y, 8)
        pill_live[pmy * 256 + pmx] = true
      end
    end
    for i = 1, #info.objects do
      local ob = info.objects[i]
      if ob.type == 1 then   -- OBJECT_SHOT
        local smx = bit.rshift(ob.x, 8)
        local smy = bit.rshift(ob.y, 8)
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
          local dir16 = bit.rshift((ob.direction or 0), 4)
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
  -- snapshot at the bottom drives the top-left tick-info HUD.  Debug-only:
  -- the sole reader is inside `if BRAIN_DEBUG_MODE`, so in opt/ this was one
  -- os.clock() per think feeding nothing.
  local _think_t0 = BRAIN_DEBUG_MODE and os.clock() or 0
  state.tick = state.tick + 1
  state._last_info = info
  local now  = state.tick
  -- Ticks since THIS brain instance opened. `now` is seeded from the engine
  -- clock (see Brain.open), so it is NOT the age of the bot: a Survival wave
  -- bot's first think already has now ≈ 4350. Anything that means "for the
  -- first N ticks of this life" must use `age`, not `now`. Identical to `now`
  -- for a bot created at game start, where birth_tick is 0.
  local age  = now - (state.birth_tick or 0)

  -- ── Tick-budget kill catch-all ──
  -- The per-call blacklists (the plan_position angle-sweep chunk, the shield
  -- scan) only notice kills that land inside the one call they guard; a kill
  -- landing anywhere else — the replan path above all — is invisible to them.
  -- This marker counts kills at the granularity of the WHOLE think.
  --
  -- Mechanics: a budget kill unwinds the think, so nothing after the kill point
  -- runs — including the clear at the bottom. A marker still present here
  -- therefore means "last tick did not finish". state.goal is persistent, so
  -- the shape we read now is the same shape that was running when we were
  -- killed. THINK_KILL_TRIES kills in a row on one shape means no reachable
  -- capacity tier makes it fit: abandon that goal instead of re-selecting it
  -- forever (three bots livelocked exactly this way).
  --
  -- Set when this tick's catch-all abandons the current goal. The killed-tick
  -- rollback just below then leaves state.goal alone: the abandonment is a
  -- decision THIS (unkilled, so far) tick deliberately made, and restoring the
  -- abandoned goal over it would resurrect the livelock the catch-all exists
  -- to break.
  local _think_abandoned = false
  do
    local g  = state.goal
    local gk = g and g.kind
    local gs = g and g.substate
    local gt = g and g.target_id
    local m  = state._think_attempt
    if m and (m.kind ~= gk or m.sub ~= gs or m.target ~= gt
              or (now - (m.tick or 0)) > (C.THINK_KILL_TTL or 50)) then
      m = nil                                  -- different shape / stale: fresh count
    end
    if m and (m.count or 0) >= (C.THINK_KILL_TRIES or 5) then
      -- Survival exemption (write side): never blacklist resupply while we're
      -- at/below ARMOUR_LOW. Benching the only goal that restores armour for
      -- GOAL_BLACKLIST_TICKS (~15 s) at 0 armour is a worse outcome than
      -- re-attempting an expensive think. We still drop to goal=none so the
      -- forced replan gets a clean shot at a DIFFERENT base, and the kill
      -- counter still resets, so the catch-all keeps working for every other
      -- goal kind. (goals.lua's pool consult exempts the same case on read.)
      local survival = (gk == "refuel_at_base" or gk == "flee_to_base")
                       and (info.armour or 99) <= C.ARMOUR_LOW
      if survival then
        print2(string.format(
          "THINK_BLACKLIST_SKIP t=%d kind=%s sub=%s target=%s @(%d,%d) killed %d× straight -- NOT blacklisted (arm=%d <= ARMOUR_LOW %d: survival goal), dropped to none so a different base can be picked",
          now, tostring(gk), tostring(gs), tostring(gt),
          (g and g.mx) or -1, (g and g.my) or -1, m.count or 0,
          info.armour or -1, C.ARMOUR_LOW))
      else
        local until_t = now + (C.GOAL_BLACKLIST_TICKS or 750)
        U.goal_blacklist_add(state, gk, gt, g and g.mx, g and g.my, until_t)
        print2(string.format(
          "THINK_BLACKLIST t=%d kind=%s sub=%s target=%s @(%d,%d) killed %d× straight by the tick budget -- goal abandoned, held off selection until t=%d",
          now, tostring(gk), tostring(gs), tostring(gt),
          (g and g.mx) or -1, (g and g.my) or -1, m.count or 0, until_t))
      end
      -- Drop to goal=none; the (rate-limited) goal=none replan then picks
      -- something else, because selection consults _goal_blacklist.
      state.goal = { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
      state._think_attempt = nil
      _think_abandoned = true
    else
      state._think_attempt = { kind = gk, sub = gs, target = gt, tick = now,
                               count = m and ((m.count or 0) + 1) or 1 }
    end
  end

  -- ── Killed-tick decision rollback ────────────────────────────────────────
  -- A tick-budget kill unwinds Brain.think from wherever it happened to be,
  -- but every mutation the tick already made to `state` survives. A replan
  -- that got as far as writing a goal switch therefore "wins" even though the
  -- tick that decided it never finished — and the bot then has to BUY ITS WAY
  -- BACK to the goal it was already on: switch fee (GOAL_SWITCH_PENALTY) plus
  -- commitment plus a rung on the exponential goal-history ratchet, since all
  -- three only charge NON-current goals. Repeat that a few times and a goal
  -- the bot genuinely needs becomes unaffordable. The 20260822 refuel↔repair
  -- cycle at 0 armour was exactly this: a killed tick had written
  -- refuel_at_base → repair_pill, and refuel could never re-take the seat.
  --
  -- Fix: at the top of every tick that starts CLEAN, snapshot a small
  -- allowlist of decision fields; at the top of every tick that follows a
  -- kill, put them back and leave the snapshot alone (so a run of consecutive
  -- kills all roll back to the same last-known-good decision state).
  --
  -- ALLOWLIST CRITERION: a field belongs here iff a half-finished tick writing
  -- it changes what the bot DECIDES later. Recompute-on-next-tick fields (the
  -- pools, cost_cache, perception, pathfinder status, steering) stay out —
  -- restoring them buys nothing. Kill-survival markers and chunked-progress
  -- bookmarks stay out for a stronger reason: they EXIST to carry information
  -- across a kill, so restoring them would erase the very evidence the kill
  -- produced. Explicitly NOT restored: _think_attempt, _goal_blacklist,
  -- _pp_chunk_attempt, _pp_blacklist, the shield-scan attempt/cache,
  -- _pill_eval_progress, _pill_eval_cache, pool/cost caches, _capacity_*,
  -- _plite_*, wsim_kill_until, perception and comms/ally state. Anything not
  -- named below is simply left alone.
  do
    local snap = state._decision_snapshot
    if (_G.brain and _G.brain.wasKilled) and snap then
      -- ── restore ──
      if not _think_abandoned then
        -- state.goal is REPLACED (not mutated) on a real switch — see the
        -- `state.goal = new_goal` in the goal-change path below — while the
        -- same-target branch deliberately keeps the live table and mutates it
        -- in place. So the table IDENTITY is the decision and the in-place
        -- writes (substate, standoff_*, _shield_scan bookmarks, wait_*) are
        -- PROGRESS. Restoring the reference therefore undoes exactly the
        -- switch and keeps every bit of progress the old goal had accrued.
        state.goal          = snap.goal
        state.goal_set_tick = snap.goal_set_tick
        -- The switch path stamps GOAL_ABANDON_COOLDOWN on the goal it leaves,
        -- and goal_selection skips pooled goals still on cooldown — so a
        -- killed switch would bench the goal we just restored. Only the
        -- current goal's own entry can be written that way, so we undo that
        -- one key instead of copying the (unbounded) cooldown table.
        if snap.cd_key and state.goal_cooldowns then
          state.goal_cooldowns[snap.cd_key] = snap.cd_val
        end
      end
      -- goal_history is the ratchet's input: every switch appends one entry
      -- and the penalty is BASE*(EXP^count - 1), so a killed tick's append is
      -- a permanent rung. Rewritten in place (<= GOAL_HISTORY_SIZE entries) so
      -- the snapshot's copy stays pristine for the next kill in the run.
      local hsrc = snap.goal_history
      local hdst = state.goal_history
      if hsrc and hdst then
        for i = #hdst, 1, -1 do hdst[i] = nil end
        for i = 1, #hsrc do hdst[i] = hsrc[i] end
      end
      -- Replan gating that a killed tick can spend without deciding anything:
      -- the replan rate floor, the attack_pill TANK PREEMPT edge stamp and its
      -- rate limiter, the one-shot warmup exit, the consumed blitz-call and
      -- force-replan flags, and the reposition bid clock. Each of these is
      -- read-then-consumed on the replan path, so a kill after the consume
      -- silently swallows the event that would have driven the next decision.
      state._last_full_replan_tick = snap.last_full_replan_tick
      state._atk_interrupt_stamp   = snap.atk_interrupt_stamp
      state._last_atk_preempt_tick = snap.last_atk_preempt_tick
      state._warm_exit_done        = snap.warm_exit_done
      state._blitz_new_call        = snap.blitz_new_call
      state._force_replan_reason   = snap.force_replan_reason
      state._repo_bid_tick         = snap.repo_bid_tick
      state._repo_bid_pid          = snap.repo_bid_pid
      print2(string.format(
        "KILL_RESTORE t=%d restored goal=%s (killed tick's decisions discarded)",
        now, tostring(state.goal and state.goal.kind)))
    else
      -- ── snapshot ──
      -- Only on a tick that starts clean, so the snapshot always describes the
      -- last tick that actually FINISHED. Tables are reused to keep this off
      -- the per-tick allocation path.
      if not snap then snap = {}; state._decision_snapshot = snap end
      snap.goal          = state.goal
      snap.goal_set_tick = state.goal_set_tick
      local g = state.goal
      if g and g.kind and g.kind ~= "none" then
        -- Key format must match the goal-change path's cd_key exactly.
        snap.cd_key = g.kind .. ":" .. (g.mx or 0) .. "," .. (g.my or 0)
        snap.cd_val = state.goal_cooldowns and state.goal_cooldowns[snap.cd_key]
      else
        snap.cd_key, snap.cd_val = nil, nil
      end
      local hsrc = state.goal_history
      if hsrc then
        local hdst = snap.goal_history
        if not hdst then hdst = {}; snap.goal_history = hdst end
        for i = #hdst, 1, -1 do hdst[i] = nil end
        -- Entry tables are written once and never mutated afterwards, so
        -- sharing the entry references is safe and one level of copy is enough.
        for i = 1, #hsrc do hdst[i] = hsrc[i] end
      end
      snap.last_full_replan_tick = state._last_full_replan_tick
      snap.atk_interrupt_stamp   = state._atk_interrupt_stamp
      snap.last_atk_preempt_tick = state._last_atk_preempt_tick
      snap.warm_exit_done        = state._warm_exit_done
      snap.blitz_new_call        = state._blitz_new_call
      snap.force_replan_reason   = state._force_replan_reason
      snap.repo_bid_tick         = state._repo_bid_tick
      snap.repo_bid_pid          = state._repo_bid_pid
    end
  end
  -- ── end killed-tick decision rollback ────────────────────────────────────

  -- Per-bot config from the BRAIN_INIT_ARG Lua global, parsed once. Two
  -- things stage it: the -bot-init [arg] CLI suffix (a test aid) and a
  -- scenario script's game.spawn_bot(..., init) 5th argument (live play —
  -- that is how Survival fields a whole wave of pill suiciders).
  -- Tokens:
  --   "ammoless"/"noammo" -> force never-refuel ON (deterministic; overrides
  --                          the random TEST_NEVER_REFUEL_CHANCE roll below)
  --   "normal"            -> force never-refuel OFF (a plain captain)
  --   "deprive=N"         -> this bot's ammo-deprivation delay = N ticks, so the
  --                          ammoless-helper/decoy kicks in sooner (100 ~= 2 s)
  --   "suicider"          -> FORCE the pill_suicider role on for this bot,
  --                          whatever the harasser slate picked and whatever
  --                          map this is (it does NOT need to be listed in
  --                          C.PILL_SUICIDER_MAPS)
  --   "nosuicider"        -> force it OFF, so a scenario can also field plain
  --                          waves on a map that IS listed there
  --   "portfolio=B/F/A[/U]" -> this bot's pill-portfolio target shares as
  --                          INTEGER PERCENTS (back/front/aggro/utility).
  --                          '/' separated because ';' and ',' already separate
  --                          tokens. U defaults to 100-B-F-A. Rejected (with a
  --                          warning) when a part isn't an integer or B+F+A>100;
  --                          normalised proportionally (with a warning) when an
  --                          explicit U makes the four not sum to 100.
  --   "blitz=MIN[/MAX]"   -> blitz party size in TANKS INCLUDING THE COMMANDER
  --                          (same convention as the constants it replaces).
  --                          MIN is a HARD quorum: no GO path fires below it,
  --                          and a commander whose READY_TIMEOUT expires
  --                          short-handed abandons the take instead of charging.
  --                          MAX caps joiners (a call at MAX reads FULL). MAX
  --                          defaults to SQUAD_MAX_SIZE + 1, raised to MIN when
  --                          MIN is bigger, so "blitz=3" alone is workable.
  --   "blitzsuiciders=N"  -> minimum number of pill_suiciders a blitz should
  --                          have. At GO the commander counts the suiciders
  --                          already in the party (itself included) and
  --                          designates that many random non-suicider SOLDIERS
  --                          to make up the difference, TEMPORARILY, for that
  --                          take only. 0 (the default) never designates. If the
  --                          soldiers can't cover it the commander designates
  --                          itself too.
  --   "noblitz"           -> this bot never blitzes at all: it opens no call
  --                          (never commands one, never enters blitz_wait),
  --                          answers/joins none (availability says "noblitz",
  --                          no join discount on any call's pill), and ignores
  --                          "bsu" suicider designations. It still fights and
  --                          takes pills SOLO, exactly as if no ally were in
  --                          range. No constant — blitzing is on by default.
  --   "noclaimdead"       -> ignore allies' CLAIMS on DEAD pills: pool 4
  --                          (capture_pill) rows never take an ally_claimed
  --                          REJECT, so several bots race to scoop the same
  --                          body and draw fire on the way in. Pool 6
  --                          (attack_pill on a LIVE pill) keeps today's claim /
  --                          steal de-confliction, and the refuel soft penalty
  --                          is UNCHANGED. No constant — claiming is on by
  --                          default.
  --   "refuel=X"          -> float multiplier on the whole "refuel" GOAL_GROUP
  --                          (refuel_at_base + flee_to_base). 1.0 = default;
  --                          1.2 makes this bot resupply less readily.
  -- This block runs early in Brain.think and squad.update (which reads
  -- state.force_pill_suicider) runs much later in the same function, so the
  -- flag is already set on the bot's very first tick. Same for the portfolio
  -- targets and the blitz sizes: every PP.targets / squad.blitz_* reader runs
  -- later in think and reads them LIVE (no cached copies), so an override is in
  -- force from tick 1 onward.
  if state._test_arg_parsed == nil then
    state._test_arg_parsed = true
    -- Mirror the mode and difficulty onto `state` so a reader inside think
    -- doesn't have to know they came in as constants. The parse and the
    -- warnings happened at chunk load (the init-arg block near the top of
    -- this file); C.MODE is "default" and C.DIFFICULTY is "hard" unless a
    -- mode= / difficulty= (or cfg=MODE= / cfg=DIFFICULTY=) token changed
    -- them. Both keys come from modes.txt, this brain's own manifest of
    -- which modes and levels exist. Nothing reads either yet -- plumbing.
    state.mode = C.MODE
    state.difficulty = C.DIFFICULTY
    local a = rawget(_G, "BRAIN_INIT_ARG")
    if type(a) == "string" and a ~= "" then
      -- ';' or ',' separated. -bot-init splits its spec on ',' so the [arg]
      -- passed on the command line must use ';' (e.g. [ammoless;deprive=100]).
      for tok in a:gmatch("[^,;]+") do
        tok = tok:gsub("%s", "")
        if tok == "ammoless" or tok == "noammo" then
          state.test_never_refuel = true
        elseif tok == "normal" then
          state.test_never_refuel = false
        elseif tok == "suicider" then
          state.force_pill_suicider = true
        elseif tok == "nosuicider" then
          state.force_pill_suicider = false
        elseif tok == "noblitz" then
          -- Solo bot: no calls opened, none joined, bsu designations ignored.
          state.blitz_disabled = true
        elseif tok == "noclaimdead" then
          -- Sweeping wave: allies' claims on DEAD pills are ignored (pool 4),
          -- so several bots race the same body and draw fire on the way in.
          state.ally_claim_dead_off = true
        elseif tok:sub(1, 10) == "portfolio=" then
          -- Integer percents, '/' separated: B/F/A or B/F/A/U.
          -- Complaints are LATCHED into state._cfg_warn, not printed here: this
          -- runs on the bot's first think, a pre-game tick whose print2 output
          -- never reaches the session's log file (same trap as TEST_ROLE). The
          -- captured-tick window below re-emits them.
          local nums, bad, extra = {}, false, false
          for part in tok:sub(11):gmatch("[^/]+") do
            if #nums >= 4 then extra = true
            elseif part:match("^%d+$") then nums[#nums + 1] = tonumber(part)
            else bad = true end
          end
          local b, f, ag, u = nums[1], nums[2], nums[3], nums[4]
          if bad or extra or #nums < 3 then
            state._cfg_warn = (state._cfg_warn or "") .. string.format(
              "[portfolio] BAD TOKEN '%s' -- want portfolio=B/F/A[/U] as integer percents; IGNORED. ", tok)
          elseif (b + f + ag) > 100 then
            state._cfg_warn = (state._cfg_warn or "") .. string.format(
              "[portfolio] BAD TOKEN '%s' -- back+front+aggro=%d > 100; IGNORED. ", tok, b + f + ag)
          else
            -- No explicit U -> it takes whatever is left (>=0 by the check above),
            -- so the four always sum to exactly 100 in that form.
            if not u then u = 100 - (b + f + ag) end
            local sum = b + f + ag + u
            if sum <= 0 then
              state._cfg_warn = (state._cfg_warn or "") .. string.format(
                "[portfolio] BAD TOKEN '%s' -- shares sum to 0; IGNORED. ", tok)
            else
              if sum ~= 100 then
                state._cfg_warn = (state._cfg_warn or "") .. string.format(
                  "[portfolio] WARNING '%s' sums to %d, not 100 -- normalising proportionally. ", tok, sum)
              end
              PP.set_targets(b / sum, f / sum, ag / sum, u / sum, "init_arg")
            end
          end
        elseif tok:sub(1, 6) == "blitz=" then
          -- Party size in tanks INCLUDING the commander: MIN or MIN/MAX.
          -- Complaints latch into state._cfg_warn for the same reason as above.
          local nums, bad, extra = {}, false, false
          for part in tok:sub(7):gmatch("[^/]+") do
            if #nums >= 2 then extra = true
            elseif part:match("^%d+$") then nums[#nums + 1] = tonumber(part)
            else bad = true end
          end
          local mn, mx = nums[1], nums[2]
          if bad or extra or #nums < 1 or mn < 1 then
            state._cfg_warn = (state._cfg_warn or "") .. string.format(
              "[blitz] BAD TOKEN '%s' -- want blitz=MIN[/MAX], integers >= 1; IGNORED. ", tok)
          elseif mx and mx < mn then
            state._cfg_warn = (state._cfg_warn or "") .. string.format(
              "[blitz] BAD TOKEN '%s' -- MAX %d < MIN %d; IGNORED. ", tok, mx, mn)
          else
            squad.set_blitz_size(mn, mx, "init_arg")
          end
        elseif tok:sub(1, 7) == "refuel=" then
          -- Float multiplier on the refuel GOAL_GROUP. Complaints latch, as above.
          local x = tonumber(tok:sub(8))
          if x and x > 0 then
            goals.set_refuel_mult(x, "init_arg")
          else
            state._cfg_warn = (state._cfg_warn or "") .. string.format(
              "[refuel] BAD TOKEN '%s' -- want refuel=X, a number > 0; IGNORED. ", tok)
          end
        elseif tok:sub(1, 15) == "blitzsuiciders=" then
          -- Minimum suiciders per blitz. Complaints latch, as above.
          local n = tok:match("^blitzsuiciders=(%d+)$")
          if n then
            squad.set_blitz_min_suiciders(tonumber(n), "init_arg")
          else
            state._cfg_warn = (state._cfg_warn or "") .. string.format(
              "[blitz] BAD TOKEN '%s' -- want blitzsuiciders=N, integer >= 0; IGNORED. ", tok)
          end
        else
          local n = tok:match("^deprive=(%d+)$")
          if n then state.test_deprive_ticks = tonumber(n) end
        end
      end
    end
  end

  -- TEST AID: roll the never-refuel flag once per bot (see
  -- TEST_NEVER_REFUEL_CHANCE in constants.lua — 0 disables). Seeded per
  -- player so the four bots don't all roll the same value.
  if state.test_never_refuel == nil then
    local chance = C.TEST_NEVER_REFUEL_CHANCE or 0
    if chance > 0 then
      math.randomseed(os.time() + (info.player_number or 0) * 7919)
      state.test_never_refuel = math.random() < chance
      if state.test_never_refuel then
        print(string.format("[TEST] bot %d is NEVER-REFUEL (all refuel goals blocked)",
                            info.player_number or -1))
      end
      -- NOTE: the print2 "TEST_ROLE" line is emitted LATER (after
      -- print2.set_bot/set_tick binds this bot's log file, ~line 1055).
      -- Calling print2 HERE — before the per-bot file is bound — silently
      -- drops the line, so don't.
    else
      state.test_never_refuel = false
    end
  end

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
  -- No LGM status rides the periodic /info state slate (allies derive
  -- friend/foe of any LGM they see straight off OBJECT_HOSTILE).  The
  -- ONE thing they can't observe is when our killed LGM has respawned —
  -- there's no engine event for that — so on a dead→alive transition we
  -- queue a single dedicated "/info lgmback" notice (sent once, in the
  -- broadcast block) that clears our dead-cooldown bookkeeping on every
  -- ally's registry.
  local _lgm_self_pn = info.player_number
  if _lgm_self_pn ~= nil then
    local _lgm_self_mx = bit.rshift((info.man_x or 0), 8)
    local _lgm_self_my = bit.rshift((info.man_y or 0), 8)
    local _lgm_prev_slot = lgm_registry.get(_lgm_self_pn)
    local _lgm_prev_status = _lgm_prev_slot and _lgm_prev_slot.status or "unknown"
    local _lgm_transitioned = lgm_registry.update_self(
      _lgm_self_pn, info.man_status or 0,
      _lgm_self_mx, _lgm_self_my, now)
    if _lgm_transitioned then
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
  if BRAIN_DEBUG_MODE and age == 1 then  -- age, not now: this marks THIS life first think; a mid-game bot starts `now` at the game clock
    print2(string.format(
      "BOT_START player_number=%s name=%s tank=(%.1f,%.1f) tile=(%d,%d)",
      tostring(info.player_number),
      tostring(info.player_name),
      (info.tankx or 0) / 256.0, (info.tanky or 0) / 256.0,
      bit.rshift((info.tankx or 0), 8), bit.rshift((info.tanky or 0), 8)))
  end

  -- ── Cause-of-death tracking (DEBUG ONLY, observation only) ─────────────
  -- Two jobs, both pure reads: (1) latch the engine's own death cause out of
  -- this tick's EVENT_TANK_KILLED addressed to us, and (2) keep the last
  -- ALIVE tile/armour, because by the time info.dead is true the server has
  -- already moved the tank to its respawn start, so info.tankx/y no longer
  -- point at the place it died. Everything written here is a file-level
  -- upvalue read only by the DEATH print2 below.
  if BRAIN_DEBUG_MODE then
    local _me = info.player_number
    if info.events and _me ~= nil then
      for _, _ev in ipairs(info.events) do
        local _d = _ev.data
        -- data = [killer, killed, deathCause, carriedPills]
        if _ev.type == EVENT_TANK_KILLED and _d and _d[2] == _me then
          _dbg_death.evt_cause  = _d[3]
          _dbg_death.evt_killer = _d[1]
          _dbg_death.evt_tick   = now
          -- 4th data byte = how many pillboxes we were hauling when we died.
          _dbg_death.evt_carry  = _d[4]
        end
      end
    end
    if not info.dead then
      local _arm = info.armour or 0
      if _dbg_death.alive_armour ~= nil and _arm < _dbg_death.alive_armour then
        _dbg_death.drop_tick = now
      end
      _dbg_death.alive_tick   = now
      _dbg_death.alive_mx     = bit.rshift((info.tankx or 0), 8)
      _dbg_death.alive_my     = bit.rshift((info.tanky or 0), 8)
      _dbg_death.alive_armour = _arm
      -- Carried pills as of the last ALIVE tick: by the time info.dead is true
      -- the haul is already on the ground, so this is the only in-brain view of
      -- what the tank was carrying. The engine's own count rides on the kill
      -- event (evt_carry) and is preferred when the event landed.
      _dbg_death.alive_carry  = info.carried_pills or 0
    end
  end

  -- Open the optimize.log section timer at the EARLIEST possible point
  -- so prelude work (capacity tier calc, debug-mode viz refresh, the
  -- startup-mode block, etc.) is included in the per-section sum. The
  -- optimize.lua tick_start was previously set ~200 lines later, which
  -- left the prelude as unaccounted gap in the time-bar.
  opt.set_tick(now)
  -- Stamp the jsonl logger with the tick number too. log_tick() normally
  -- reads it from `state`, but logger.flush_killed runs on a tick where
  -- log_tick never got there — this is how its record knows which tick died.
  log.set_tick(now)
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

    -- A budget-killed tick reports lastThinkMs ≈ targetMs (the hook fires AT the
    -- cap), so unadjusted it teaches both EWMAs below that the tier "fit" — the
    -- ratio stays under DROP_RATIO and the per-tier history then corroborates
    -- raising straight back into the kill. Charge a kill an inflated cost.
    if killed and last_ms > 0 and tgt_ms > 0 then
      last_ms = math.max(last_ms, tgt_ms * (C.CAPACITY_KILLED_COST_MULT or 1.5))
    end

    -- Per-tier ms history (records the tier we just ran at).
    state._tier_ms = state._tier_ms or {}
    if last_ms > 0 then
      local prior = state._tier_ms[prev_tier]
      state._tier_ms[prev_tier] = prior
        and (prior * (1 - C.CAPACITY_EWMA_ALPHA) + last_ms * C.CAPACITY_EWMA_ALPHA)
        or  last_ms
    end

    state._tier_killed = state._tier_killed or {}

    -- Smoothed cost ratio, fed EVERY tick INCLUDING kills (with the inflated
    -- cost above). Killed ticks used to skip this feed entirely, so a bot that
    -- was being killed repeatedly kept a low ratio and climbed back up the
    -- moment CAPACITY_KILLED_AVOID expired.
    if tgt_ms > 0 and last_ms > 0 then
      local ratio = last_ms / tgt_ms
      state._capacity_ratio_ewma = (state._capacity_ratio_ewma or ratio)
                                 * (1 - C.CAPACITY_EWMA_ALPHA)
                                 + ratio * C.CAPACITY_EWMA_ALPHA
    end

    local cur = prev_tier
    if killed then
      cur = math.max(1, prev_tier - C.CAPACITY_KILLED_CUT)
      state._tier_killed[prev_tier] = state.tick
    elseif tgt_ms > 0 and last_ms > 0 then
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

    -- PROFILING LITE: stash the budget this tick was tiered against so the
    -- end-of-tick TICK_COST / NEAR_BUDGET lines can report ms-vs-target
    -- without re-deriving it. tgt_ms is a local to this do-block, hence the
    -- stash. Debug-only field — stripped from opt/ with the whole block.
    if BRAIN_DEBUG_MODE then state._plite_tgt_ms = tgt_ms end

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
    -- Name print2_bot<N>.log by the index BrainTest shows in its HUD / Copy
    -- reference (_G.BT_BOT_INDEX, pushed per-bot), so the copied "botN" always
    -- maps to print2_botN.log. Falls back to our player_number outside BrainTest.
    print2.set_bot(_G.BT_BOT_INDEX or info.player_number or 0)
    print2.set_tick(now)
    -- PROFILING LITE: clear last tick's stage checkpoints. Two of the three
    -- sit inside the conditional goal section, so without this reset a tick
    -- that skips it (escape_water etc.) would report stale stage times.
    state._plite_cp_prelude, state._plite_cp_replan, state._plite_cp_handler = nil, nil, nil
    print2("BEGIN bot tick=", now, " state.goal.kind = ", state.goal.kind, ", state.goal.substate = ", tostring(state.goal.substate))
    -- TEST AID: log the never-refuel roll once, HERE — the roll runs far
    -- earlier (first think, ~line 830) but print2.set_bot only binds this bot's
    -- file just above, so logging up there drops the line. Gate the latch on
    -- _PRINT2_ENABLED so a print2-DISABLED warmup game (autotest runs one
    -- first) can't silently consume the latch before the real, logged game.
    -- TEST AID: log the never-refuel roll in the captured tick window. Bots
    -- think during pre-game ticks (~1-10) whose print2 never reaches this
    -- session's file, so a once-latch that fires there is silently consumed;
    -- and a once-latch firing at a single tick is racy — a bot budget-killed
    -- mid-think (or that skips that exact tick) misses its only shot. So just
    -- emit every captured tick in a short window (11..40); any completed think
    -- in the window records it, and the reader dedupes (grep | sort -u). The
    -- roll value is stable after first think, so all lines agree.
    -- The window is counted from THIS life's first tick, not from absolute
    -- tick 11: state.tick is seeded from the engine clock, so a bot created
    -- mid-game (a Survival wave) is already well past 40 on its first think
    -- and an absolute window would never emit for it.
    if age >= 11 and age <= 40 then
      print2(string.format("TEST_ROLE bot=%d never_refuel=%s",
                           info.player_number or -1, tostring(state.test_never_refuel)))
      -- Same window, same reason, for the per-bot config parsed from
      -- BRAIN_INIT_ARG: the parse runs on the FIRST think (a pre-game tick), so
      -- a line printed there is silently dropped. Emitting the live values here
      -- means every -brain-debug log states which portfolio shares this bot is
      -- actually running, default or overridden.
      print2(string.format("[portfolio] targets back=%.2f front=%.2f aggro=%.2f util=%.2f (%s)",
                           PP.TARGET_BACK, PP.TARGET_FRONT, PP.TARGET_AGGRO,
                           PP.TARGET_UTIL, PP.targets_source))
      if state.blitz_disabled then
        print2("[blitz] blitz=off (init_arg) — no calls opened or joined, bsu ignored (solo takes)")
      else
        print2(string.format("[blitz] size min=%d max=%d (%s)  suiciders min=%d (%s)",
                             squad.blitz_min(), squad.blitz_max(), squad.blitz_size_source,
                             squad.blitz_min_suiciders(), squad.blitz_suiciders_source))
      end
      print2(string.format("[refuel] cost mult x%.2f (%s)",
                           goals.refuel_mult(), goals.refuel_mult_source))
      -- The LGM-kill fire gate actually in force for this bot: the shared knob,
      -- the capture-hunt override (-1 = follow the shared knob), and the engine's
      -- own kill radius for reference.  One line so a -brain-debug log states
      -- which gate produced (or refused) every LGM shot in the run.
      do
        local _kt = kill_lgm.tuning()
        print2(string.format(
          "[kill_lgm] improved=%s (mode=%s difficulty=%s) fire gate %d wu,"
          .. " capture gate %d wu (engine kills within %d wu)",
          tostring(_kt.on), tostring(C.MODE), tostring(C.DIFFICULTY),
          _kt.fire_wu, _kt.capture_fire_wu, _kt.engine_kill_wu))
        print2(string.format(
          "[kill_lgm] aim target %s wu%s",
          tostring(_kt.aim_wu or "-"),
          _kt.aim_throttle and " (refinement may use the throttle)"
                            or " (refinement never touches the throttle)"))
        print2(string.format(
          "[kill_lgm] track %s, pick=%s, sight holds from %d steps",
          _kt.track_wu and string.format("%.1f tiles from the tank",
                                         _kt.track_wu / 256.0)
                        or "along the heading (old admission)",
          _kt.pick_near_pill and "nearest the pill" or "nearest the shell ray",
          _kt.hold_min_steps))
      end
      if state.ally_claim_dead_off then
        print2("[claims] claims_dead=off (init_arg) — pool 4 ignores allies' capture_pill claims on dead pills (no ally_claimed reject); pool 6 + refuel unchanged")
      end
      -- The preset/cfg overrides, which were applied at CHUNK LOAD (see the
      -- block right after require("constants")) and could not print there.
      -- One line per override so a bench can grep a side's log and see
      -- exactly which constants that side was running.
      for _i = 1, #_INIT_CFG_LOG do print2(_INIT_CFG_LOG[_i]) end
      if _INIT_CFG_WARN then print2(_INIT_CFG_WARN) end
      if state._cfg_warn then print2(state._cfg_warn) end
    end
    -- Raw engine-object dump: EXACTLY what the engine handed the brain this tick
    -- (info.objects — type/id/tile/host/speed). Tanks hidden in trees beyond
    -- MIN_TREEHIDE_DIST are filtered C-side (playersGetBrainTanksInRect), so if a
    -- tank isn't here, the brain literally never received it. type: 0=tank,
    -- 2=pillbox (see OBJECT_* ). Opt-in: set _G._DUMP_ENGINE_OBJ = true.
    if _G._DUMP_ENGINE_OBJ and info.objects then
      local parts = {}
      for _, ob in ipairs(info.objects) do
        parts[#parts + 1] = string.format("{ty=%d id=%s (%d,%d) host=%s spd=%s}",
          ob.type or -1, tostring(ob.idnum), bit.rshift((ob.x or 0), 8), bit.rshift((ob.y or 0), 8),
          tostring((bit.band((ob.info or 0), OBJECT_HOSTILE)) ~= 0), tostring(ob.speed))
      end
      print2(string.format("ENGINE_OBJ t=%d n=%d %s", now, #info.objects, table.concat(parts, " ")))
    end
    -- Full raw BrainInfo dump: EXACTLY what the engine handed THIS bot THIS tick —
    -- self state + every object (raw type/id/tile/info-bits/dir-or-health/speed) +
    -- every event (type + raw data fields). One line per tick per bot (logs are
    -- already per-bot), so a C<->Lua disconnect (e.g. a carried pill the engine
    -- still reports as a deployed object, or a missed EVENT_PILL_UPDATE inTank) is
    -- one grep away. On by default in debug runs; silence with _G._DUMP_BRAININFO=false.
    -- (whole block → stripped from opt via BRAIN_DEBUG_MODE, so production never
    -- pays.) For pillbox/base objects `dir` IS the health; for tanks it's the
    -- heading. info is hex so the owner/HOSTILE/alliance bits are visible.
    if BRAIN_DEBUG_MODE and _G._DUMP_BRAININFO ~= false then
      local _od = {}
      if info.objects then
        for _, ob in ipairs(info.objects) do
          _od[#_od + 1] = string.format("ty%d#%s@(%d,%d)info=0x%X dir=%s spd=%s",
            ob.type or -1, tostring(ob.idnum), bit.rshift((ob.x or 0), 8), bit.rshift((ob.y or 0), 8),
            ob.info or 0, tostring(ob.direction), tostring(ob.speed))
        end
      end
      local _ed = {}
      if info.events then
        for _, ev in ipairs(info.events) do
          local _ds = ""
          if ev.data then
            local _p = {}
            for _i = 1, #ev.data do _p[#_p + 1] = tostring(ev.data[_i]) end
            _ds = table.concat(_p, ",")
          end
          _ed[#_ed + 1] = string.format("ev%s[%s]", tostring(ev.type), _ds)
        end
      end
      -- info.base = the SINGLE base the engine reports within base-status range
      -- (~7 tiles) — i.e. the one we're next to, NOT necessarily our refuel goal.
      -- Its stock (arm/sh/mn) is the only per-tick base-resource data the engine
      -- gives; a base out of range has none. Logged so refuel-stock decisions are
      -- auditable.
      local _bs = info.base
        and string.format("base=arm%s/sh%s/mn%s", tostring(info.base.armour), tostring(info.base.shells), tostring(info.base.mines))
        or "base=none"
      print2(string.format(
        "ENGINE_DUMP t=%d self=(%d,%d) dir=%s spd=%s arm=%s sh=%s mn=%s tr=%s carry=%s man=%s boat=%s gun=%s pn=%s %s | OBJ=%s | EVT=%s",
        now, bit.rshift((info.tankx or 0), 8), bit.rshift((info.tanky or 0), 8),
        tostring(info.direction), tostring(info.speed), tostring(info.armour),
        tostring(info.shells), tostring(info.mines), tostring(info.trees),
        tostring(info.carried_pills), tostring(info.man_status), tostring(info.inboat),
        tostring(info.gunrange), tostring(info.player_number), _bs,
        table.concat(_od, " "), table.concat(_ed, " ")))
    end
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

  -- NOTE ON PLACEMENT: this sits AFTER print2.set_tick (which clears the
  -- per-tick buffer) on purpose. It used to live beside cautious_mode, ~330
  -- lines up, and every line it printed was silently thrown away with the
  -- previous tick's buffer. Nothing downstream reads these fields before
  -- here -- goal selection, the pools and the slate builder are all further
  -- down the tick -- so the answer is just as fresh and the log survives.
  -- LOADED, BUILDER-LESS: the one place the question is asked (goals.lua
  -- M.loaded_no_lgm_eval carries the definition and the note on why it is
  -- never latched).  Per-tick, beside cautious_mode, so every consumer --
  -- the attack multiplier, the escape hysteresis exemption, capture pricing,
  -- the kill_me row -- reads ONE field that cannot disagree with itself
  -- across a tick.  No new chunk-level local: init.lua is close to Lua's
  -- 60-upvalue limit and this lives on `state`.
  do
    local _lnl_prev = state.loaded_no_lgm
    state.loaded_no_lgm = goals.loaded_no_lgm_eval(state, info, now)
    -- Print on the EDGE only: a state that holds for 80k ticks should say so
    -- once, not 80k times.
    if _lnl_prev ~= state.loaded_no_lgm then
      print2(string.format("LOADED_NO_LGM t=%d %s -> %s %s",
        now, tostring(_lnl_prev), tostring(state.loaded_no_lgm),
        goals.loaded_no_lgm_label(state)))
    end
  end

  -- STRANDED-FLAG LIFECYCLE (bug fix, un-gated).  state.lgm_stranded was set
  -- and cleared ONLY inside goal_selection -- i.e. only on replan ticks --
  -- and cleared only when the man was back INTANK.  A man who was flagged
  -- stranded and then KILLED left the flag set for the rest of his next life:
  -- the HUD kept saying "LGM STRANDED", eval_wait_for_lgm kept refusing to
  -- bid, and attack.lua's ATTACK_PP_HOLD_SKIP_STRANDED kept skipping the
  -- plan_position hold, all off a fact that had stopped being true.  The flag
  -- describes a man who is OUT WALKING and cannot get home, so it is cleared
  -- here, every tick, whenever he is not out walking -- INTANK or DEAD alike.
  -- The check tick and the viz factors go with it so the next walk re-earns
  -- the flag from a fresh pathfind instead of inheriting the old verdict.
  if info.man_status ~= C.LGM_MOVING then
    state.lgm_stranded = nil
    state.lgm_stranded_check_tick = nil
    state._lgm_stranded_factors = nil
  end

  -- ── "KILL ME" bookkeeping (per tick, one table on `state`) ─────────────
  -- state.km.claimed_by   who has claimed OUR request (read off allies' kmc
  --                       tokens). The initiator uses it to stop targeting
  --                       that ONE ally -- enemies stay fair game.
  -- state.km.claim        what WE advertise as responder; written by
  --                       eval_attack_tank, read by the slate builder.
  -- state.km.cooldown_until  set on an ACTUAL kill, never on a cancelled
  --                       request (see the death watch below).
  if C.KILL_ME_ENABLED then
    local km = state.km
    if not km then km = {}; state.km = km end
    local _self_pn = info.player_number or -1
    local _claimed_by = nil
    if state.goal and state.goal.kind == "kill_me_wait" then
      for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
        if pn ~= _self_pn then
          local kmc = slot.info and slot.info.kmc
          if kmc and kmc ~= "-" and #kmc >= 6 then
            local cpn = tonumber(string.sub(kmc, 1, 2), 16)
            -- Lowest player number wins a double claim, the same tie-break
            -- every other claim in the brain uses.
            if cpn == _self_pn and (not _claimed_by or pn < _claimed_by) then
              _claimed_by = pn
            end
          end
        end
      end
    end
    if km.claimed_by ~= _claimed_by then
      print2(string.format("KILL_ME_CLAIM t=%d ours claimed_by=%s (was %s)",
        now, tostring(_claimed_by), tostring(km.claimed_by)))
    end
    km.claimed_by = _claimed_by
    -- EXECUTING: the claimant's tank is in view within KILL_ME_EXECUTE_TILES
    -- and no enemy tank is inside the cancel radius, so the shells landing
    -- on us are its delivery and the armour they take off is not danger.
    -- Read by haul_flee_eval (armour trigger), the critical flee injection
    -- and kill_me_wait's own armour floor. Derived every tick, never latched.
    local _exec = false
    if _claimed_by then
      local nh = state.perc and state.perc.nearest_hostile_tank
      local enemy_near = nh and (nh.dist or 1e9) <= (C.KILL_ME_CANCEL_ENEMY_TILES or 10)
      if not enemy_near then
        local tmx0, tmy0 = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
        for _, ob in ipairs(info.objects or {}) do
          if ob.type == OBJECT_TANK and (bit.band(ob.info, OBJECT_HOSTILE)) == 0
             and ob.idnum == _claimed_by then
            local d = U.mdist(tmx0, tmy0, bit.rshift(ob.x, 8), bit.rshift(ob.y, 8))
            if d <= (C.KILL_ME_EXECUTE_TILES or 12) then _exec = true end
            break
          end
        end
      end
    end
    if km.executing ~= _exec then
      print2(string.format("KILL_ME_EXEC t=%d executing=%s claimant=%s armour=%d",
        now, tostring(_exec), tostring(_claimed_by), info.armour or -1))
    end
    km.executing = _exec
    -- DEATH WATCH -> team cooldown. The cooldown exists so a successful
    -- hand-off is not immediately followed by another one; it must therefore
    -- start on a KILL and not on a request that merely fizzled.
    --   responder: the ally we were driving at is now dead.
    --   initiator: we died while advertising -- the hand-off landed on us.
    if state.goal and state.goal.kind == "attack_tank" and state.goal.km_ally_pn then
      -- ARM the watch when the delivery is adopted (or re-targeted): the
      -- death that counts is one AFTER we set out. Comparing against -1 let
      -- any earlier death of that ally on record fire KILL_ME_DONE on the
      -- first tick of the errand and lock the team out for a cooldown for
      -- nothing (recorded 1-pill replay 2026-09-08: 15 of 17 DONE lines had
      -- no matching death within 60 ticks).
      if km.watch_pn ~= state.goal.km_ally_pn then
        km.watch_pn = state.goal.km_ally_pn
        km.watch_from = now
      end
      local _d = state.tank_dead_at and state.tank_dead_at[state.goal.km_ally_pn]
      if _d and _d > km.watch_from then
        km.cooldown_until = now + (C.KILL_ME_COOLDOWN_TICKS or 3000)
        km.watch_from = _d
        print2(string.format("KILL_ME_DONE t=%d responder: ally p%d is dead, team cooldown until t=%d",
          now, state.goal.km_ally_pn, km.cooldown_until))
      end
    end
    if km.was_advertising and info.newtank then
      km.cooldown_until = now + (C.KILL_ME_COOLDOWN_TICKS or 3000)
      print2(string.format("KILL_ME_DONE t=%d initiator: respawned after advertising, cooldown until t=%d",
        now, km.cooldown_until))
    end
    km.was_advertising = (state.goal and state.goal.kind == "kill_me_wait") or false
    if not (state.goal and state.goal.kind == "attack_tank" and state.goal.km_ally_pn) then
      km.watch_pn = nil   -- errand over: the next one re-arms from its own start
    end
  end
  -- t_tick_start is the real top-of-think clock (set above, near
  -- opt.set_tick). t_early is its alias used by the early-viz/HUD timer
  -- so the first section measures from the actual tick start.
  local t_early = t_tick_start

  -- Dead-tick hook: the engine now invokes us while our tank is waiting to
  -- respawn (info.dead). We can't act and our outputs are dropped C-side, but
  -- we reset our OWN state so we come back clean — drop any in-flight blitz
  -- commitment/role/call latch and the current goal — then return a no-op. The
  -- tick counter was already advanced above, so the debug panels keep tracking
  -- instead of freezing while we're dead.
  if info.dead then
    -- One DEATH line per death episode (DEBUG ONLY). First statement in the
    -- block on purpose: the resets below wipe state.goal, and the whole point
    -- of the line is to name the goal we died pursuing.
    if BRAIN_DEBUG_MODE and not _dbg_death.reported then
      _dbg_death.reported = true
      local _cause
      -- Prefer the ENGINE's cause. The event and the dead flag normally land
      -- in the same snapshot; a few ticks of slack covers a snapshot that
      -- splits them, and stops a previous life's event being reused.
      if _dbg_death.evt_tick ~= nil and (now - _dbg_death.evt_tick) <= 4 then
        if _dbg_death.evt_cause == 1 then         -- LAST_DEATH_BY_DEEPSEA
          _cause = "drowned"
        elseif _dbg_death.evt_cause == 2 then     -- LAST_DEATH_BY_SHELL
          -- killer 255 == NEUTRAL == a pillbox pulled the trigger.
          _cause = (_dbg_death.evt_killer == 255) and "shell_pill" or "shell_tank"
        else
          _cause = "engine_cause_" .. tostring(_dbg_death.evt_cause)
        end
      else
        -- No EVENT_TANK_KILLED addressed to us: tankMineDamage kills the tank
        -- without going through the tankKill callback, so a mine death emits
        -- no event and lands here.
        _cause = "mine_or_other"
      end
      local _mx, _my = _dbg_death.alive_mx or 0, _dbg_death.alive_my or 0
      -- ttype_peek, never ttype: a detector call from debug-only code would
      -- prime terrain_prev and split the recorded brain from production.
      local _terr = U.ttype_peek(_mx, _my)
      local _tname = ({ [C.T_BUILDING]="building", [C.T_RIVER]="river",
                        [C.T_SWAMP]="swamp",       [C.T_CRATER]="crater",
                        [C.T_ROAD]="road",         [C.T_FOREST]="forest",
                        [C.T_RUBBLE]="rubble",     [C.T_GRASS]="grass",
                        [C.T_HALFBUILD]="halfbuild", [C.T_BOAT]="boat",
                        [C.T_DEEPSEA]="deepsea",   [C.T_REFBASE]="refbase",
                        [C.T_PILLBOX]="pillbox",   [C.T_UNKNOWN]="unknown",
                      })[_terr] or ("t" .. tostring(_terr))
      print2(string.format(
        "DEATH t=%d cause=%s tile=(%d,%d) terrain=%s armour=%s last_hit_age=%d"
        .. " goal=%s sub=%s tgt=(%s,%s) carry=%s carry_evt=%s killer=%s alive_t=%s",
        now, _cause, _mx, _my, _tname, tostring(_dbg_death.alive_armour),
        _dbg_death.drop_tick and (now - _dbg_death.drop_tick) or -1,
        tostring(state.goal and state.goal.kind),
        tostring(state.goal and state.goal.substate),
        tostring(state.goal and state.goal.mx),
        tostring(state.goal and state.goal.my),
        -- carry: how many pillboxes we were hauling, taken from the BRAIN's
        -- last alive tick. It is NOT taken from the kill event, because the
        -- engine only fills EVENT_TANK_KILLED data[4] on a DROWNING
        -- (tank.c tankKill(..., tankGetNumCarriedPills)); both shell-kill call
        -- sites pass a hardcoded 0 (shells.c), so the event byte reads 0 for
        -- every shell death — which is most of them. carry_evt prints the raw
        -- event byte beside it, so a drowning can still be cross-checked and
        -- the two are never silently conflated.
        tostring(_dbg_death.alive_carry),
        tostring((_dbg_death.evt_tick ~= nil and (now - _dbg_death.evt_tick) <= 4
                  and _dbg_death.evt_carry) or "n/a"),
        tostring(_dbg_death.evt_killer), tostring(_dbg_death.alive_tick)))
    end
    state.goal = { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
    -- Wipe EVERY blitz/squad coordination field (negotiation, offers, rejects,
    -- roster, watchdog, broadcast latches, and the call registry) so we respawn
    -- with a clean slate instead of resuming a dead life's blitz. The registry
    -- in particular must clear: its per-call distance/discount is computed off
    -- the dijkstra slate, rooted at our PRE-DEATH position until it re-expands
    -- after respawn — a far call would look cheap and trigger a silly cross-map
    -- join. reset_blitz_state() also sets _blitz_query_send so commanders resend
    -- bco and every call's distance is recomputed fresh from the new spawn.
    squad.reset_blitz_state(state)
    -- Reset ammo-deprivation on death: a fresh respawn should refuel normally
    -- again, not carry the suicide-decoy flag across a life. (It also time-boxes
    -- itself to AMMO_DEPRIVED_MAX_TICKS in strategy.lua.)
    state.ammo_low_since       = nil
    state.ammo_deprived        = false
    state._ammo_deprived_since = nil
    -- Drop all enemy-tank tracking so we don't respawn carrying stale ghosts.
    -- A bot killed mid-fight keeps a frozen ghost of its killer at the spot it
    -- died (last_tick = death tick); after respawn it sits there firing at the
    -- empty tile. Wiping the track means ghosts rebuild from fresh sightings.
    state._tank_track = nil
    -- Blank ALL dijkstra slates to all-INF while we're dead, so we don't respawn
    -- serving stale PRE-DEATH path costs. The normal respawn reroot only fires
    -- the tick AFTER respawn-detect, leaving the first live tick with death-
    -- rooted costs (that produced a cross-map blitz join). We're dead and idle
    -- now, so reset them here; they aren't stepped while dead, so they stay
    -- all-INF (far-tile lookups return COST_INF, no fake-cheap costs) until the
    -- post-respawn reroot re-expands from the new spawn. Once per death episode
    -- (the flag is cleared the first live tick below). brainPathfinderDijkstraStart
    -- sets every g_cost cell to COST_INF; src (0,0) is irrelevant (never stepped).
    if not state._dij_blanked then
      state._dij_blanked = true
      local mc_s, mc_l, ex = C.DIJKSTRA_SHORT_MAX_COST, C.DIJKSTRA_MAX_COST, C.DIJKSTRA_EXACT
      cpf.dijkstra_start(0, now, 0, 0, 0, 0, 0, 0, 0, mc_s, ex, 1.0, 0, 1)
      cpf.dijkstra_start(1, now, 0, 0, 0, 0, 0, 0, 0, mc_s, ex, 1.0, 0, 1)
      cpf.dijkstra_start(2, now, 0, 0, 0, 0, 0, 0, 0, mc_l, ex, 1.0, 0, 1)
      cpf.dijkstra_start(3, now, 0, 0, 0, 0, 0, 0, 0, mc_l, ex, 1.0, 0, 1)
      if BRAIN_DEBUG_MODE then print2(string.format("DIJ_BLANK_ON_DEATH t=%d — all slates reset to INF while dead", now)) end
    end
    if BRAIN_DEBUG_MODE then print2("DEAD tick t=", now, " -- reset blitz/goal + tank tracks for respawn") end
    -- The dead early-exit never reached the main think's print2.flush(), so
    -- everything printed on a dead tick -- DEATH, DIJ_BLANK_ON_DEATH and the
    -- DEAD tick line above -- sat in the buffer until the next set_tick threw
    -- it away. That is why no death has ever shown up in a print2 log. Flush
    -- here, on the dead path only, so the DEATH line actually lands on disk.
    if BRAIN_DEBUG_MODE then print2.flush() end
    state._think_attempt = nil   -- reached an exit: this think was not killed
    return { holdkeys = 0, tapkeys = 0, build = nil,
             wantallies = info.allies, messagedest = 0, sendmessage = nil }
  end
  state._dij_blanked = nil   -- alive: re-arm the on-death slate blank for next death
  if BRAIN_DEBUG_MODE then _dbg_death.reported = false end  -- alive: re-arm the one-shot DEATH line

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
    local _t_su = BRAIN_PROFILE and clock_us() or 0
    -- Fresh world data so world.bases / world.pills are populated.
    W.process_events(world, info, state)
    W.update(world, info, now)
    if BRAIN_PROFILE then
      opt(string.format("  [startup] W.update %.2f ms", (clock_us() - _t_su) / 1000))
    end

    local tmx_s     = bit.rshift(info.tankx, 8)
    local tmy_s     = bit.rshift(info.tanky, 8)
    local in_boat_s = info.inboat and 1 or 0

    -- Pre-warm the threat grid during startup so tick-11 (first normal tick)
    -- doesn't pay the full 4-5 ms rebuild cost. danger + threat are cheap
    -- on repeat calls once the grid is built; only the first call rebuilds.
    local _t_danger = BRAIN_PROFILE and clock_us() or 0
    danger.update(info, now)
    if BRAIN_PROFILE then
      opt(string.format("  [startup] danger.update %.2f ms", (clock_us() - _t_danger) / 1000))
    end
    local _t_threat = BRAIN_PROFILE and clock_us() or 0
    threat.update(state, world, info)
    if BRAIN_PROFILE then
      opt(string.format("  [startup] threat.update %.2f ms", (clock_us() - _t_threat) / 1000))
    end

    -- Step (or first-time start) the short slate.
    local _t_dij = BRAIN_PROFILE and clock_us() or 0
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
    if BRAIN_PROFILE then
      opt(string.format("  [startup] dijkstra_step %.2f ms", (clock_us() - _t_dij) / 1000))
    end
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
        if BRAIN_PROFILE then
          opt(string.format("startup: capture_base #%d at (%d,%d) cost=%.0f tick=%d",
                            best_id, best_b.mx, best_b.my, best_cost, now))
        end
      end
    end

    -- Exit startup once the short slate has completed its first full
    -- lifetime. By then it's expanded enough of the map that the
    -- normal flow's update_pool_cache will hit cache for distant
    -- candidates instead of falling all the way to A* fallback.
    -- `age`, not `now`: this is "one short-slate lifetime after THIS brain
    -- opened". A wave bot's `now` starts in the thousands, so an absolute
    -- test would leave startup on its very first think and hand the normal
    -- flow a cold pool cache.
    if age >= C.DIJKSTRA_SHORT_INTERVAL then
      state.startup_mode = false
      opt("startup: exit at tick=", now,
          " goal_set=", tostring(state._startup_goal_set))
    end

    if BRAIN_PROFILE then
      opt(string.format("STARTUP TICK TOTAL %.2f ms", (clock_us() - t_tick_start) / 1000))
    end
    opt.flush()

    -- STARTUP_HOLD_TICKS already prevents driving during this window,
    -- so we just return zero keys. No steering, no builder, no log.
    state._think_attempt = nil   -- reached an exit: this think was not killed
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
  if age == 1 then print(TAG .. " >>> CODE VERSION: " .. BOT_VERSION .. " <<<") end
  if age <= 3 then print(TAG .. " think() tick=" .. now) end

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
      -- (The old flashing ">>> MANUAL CONTROL <<<" banner is gone — the BOLO HUD
      -- panel below is itself the manual-mode indicator.)
      -- Show what C side is sending (manual_keys set by Brain.set_manual_keys)
      local mk = manual_keys or 0
      local parts = {}
      if (bit.band(mk, KEY_FASTER))    ~= 0 then parts[#parts+1] = "FWD" end
      if (bit.band(mk, KEY_SLOWER))    ~= 0 then parts[#parts+1] = "BACK" end
      if (bit.band(mk, KEY_TURNLEFT))  ~= 0 then parts[#parts+1] = "LEFT" end
      if (bit.band(mk, KEY_TURNRIGHT)) ~= 0 then parts[#parts+1] = "RIGHT" end
      if (bit.band(mk, KEY_SHOOT))     ~= 0 then parts[#parts+1] = "SHOOT" end
      if (bit.band(mk, KEY_DROPMINE))  ~= 0 then parts[#parts+1] = "MINE" end
      if (bit.band(mk, KEY_MORERANGE)) ~= 0 then parts[#parts+1] = "GUN+" end
      if (bit.band(mk, KEY_LESSRANGE)) ~= 0 then parts[#parts+1] = "GUN-" end
      local key_str = #parts > 0 and table.concat(parts, " ") or "(none)"
      viz.hud_text("hud_manual", 10, 80, "Keys: " .. key_str, "topleft", 255, 255, 100)
      viz.hud_text("hud_manual", 10, 92, string.format("spd=%d dir=%d arm=%d sh=%d",
        info.speed, info.direction, info.armour, info.shells), "topleft", 200, 200, 200)
      -- ── Bolo-style status panel (approximation, primitive shapes) ──
      -- A right-side Bolo HUD has four vertical bars (shells/mines/armour/trees)
      -- growing from the bottom, then a build menu of 5 selectable icons. We draw
      -- a primitive version: a dark panel, 4 colored bars, and a 5-box build menu
      -- where the box matching the 1-5 selection (manual_build_action) is lit.
      -- Single viz layer for the whole manual Bolo HUD (one toggle); only drawn
      -- here, inside the manual-mode branch, so it never renders in autonomous play.
      local HID = "hud_manual"
      local px0, py0, pw, ph = 8, 110, 168, 292
      viz.hud_rect(HID, px0, py0, pw, ph, "topleft", 22, 22, 32, 215, true)
      viz.hud_rect(HID, px0, py0, pw, ph, "topleft", 90, 90, 110, 255, false)
      viz.hud_text(HID, px0 + 6, py0 + 5, "BOLO HUD", "topleft", 210, 210, 220)
      viz.hud_text(HID, px0 + 110, py0 + 5, "MANUAL", "topleft", 255, 80, 80)
      -- Gun-ready light (info.reload is a bool: true = still reloading).
      local gun_ready = not info.reload
      local gr, gg, gb = gun_ready and 90 or 235, gun_ready and 230 or 80, gun_ready and 90 or 80
      viz.hud_rect(HID, px0 + 6, py0 + 18, 78, 11, "topleft", gr, gg, gb, 90, true)
      viz.hud_rect(HID, px0 + 6, py0 + 18, 78, 11, "topleft", gr, gg, gb, 255, false)
      viz.hud_text(HID, px0 + 9, py0 + 19, gun_ready and "GUN READY" or "RELOADING", "topleft", gr, gg, gb)
      -- Four vertical resource bars, max 40, grow from a shared baseline.
      local bars = {
        { lbl = "SH", v = info.shells, r = 255, g = 230, b = 80 },
        { lbl = "MI", v = info.mines,  r = 255, g = 150, b = 40 },
        { lbl = "AR", v = info.armour, r = 90,  g = 230, b = 90 },
        { lbl = "TR", v = info.trees,  r = 70,  g = 175, b = 70 },
      }
      local baseline, maxH, bw = py0 + 150, 108, 24
      for i, b in ipairs(bars) do
        local bx   = px0 + 12 + (i - 1) * (bw + 12)
        local v    = math.max(0, math.min(40, b.v or 0))
        local valH = math.floor((v / 40) * maxH + 0.5)
        viz.hud_rect(HID, bx, baseline - maxH, bw, maxH, "topleft", 80, 80, 95, 255, false)
        if valH > 0 then viz.hud_rect(HID, bx, baseline - valH, bw, valH, "topleft", b.r, b.g, b.b, 235, true) end
        viz.hud_text(HID, bx + 4, baseline - maxH - 11, tostring(v), "topleft", 220, 220, 220)
        viz.hud_text(HID, bx + 6, baseline + 3, b.lbl, "topleft", b.r, b.g, b.b)
      end
      -- Build menu: 5 boxes; the selected one (1-5) is lit blue with a white border.
      local by = py0 + 168
      for i = 1, 5 do
        local bx  = px0 + 2 + (i - 1) * 32
        local sel = (i == manual_build_action)
        local c   = MANUAL_BUILD_COLORS[i]
        -- Fill is the build's representative color: bright when selected, dimmed
        -- when not. Border + label go white on the selected one, gray otherwise.
        local mul = sel and 0.85 or 0.40
        local fr, fg, fb = math.floor(c[1] * mul), math.floor(c[2] * mul), math.floor(c[3] * mul)
        local orr, og, ob = sel and 255 or 90, sel and 255 or 90, sel and 255 or 100
        viz.hud_rect(HID, bx, by, 30, 24, "topleft", fr, fg, fb, 235, true)
        viz.hud_rect(HID, bx, by, 30, 24, "topleft", orr, og, ob, 255, false)
        viz.hud_text(HID, bx + 12, by + 2, tostring(i), "topleft",
          sel and 255 or 220, sel and 255 or 220, sel and 255 or 220)
        viz.hud_text(HID, bx + 3, by + 13, MANUAL_BUILD_LABELS[i], "topleft",
          sel and 255 or 205, sel and 255 or 205, sel and 255 or 210)
      end
      -- Status footer: builder/LGM state, speed, boat, controls hint.
      local bld_str, bld_r, bld_g, bld_b = hud_builder_status(info, state)
      local fy = by + 28
      viz.hud_text(HID, px0 + 6, fy,      string.format("LGM%s", bld_str), "topleft", bld_r, bld_g, bld_b)
      viz.hud_text(HID, px0 + 6, fy + 11, string.format("SPD %d  BOAT %s   1-5=type  click=build",
        info.speed, info.inboat and "Y" or "n"), "topleft", 185, 185, 200)
      -- ── Pillbox + base alliance grids (the iconic Bolo ownership rows) ──
      -- One small square per pill/base, colored by alliance. green=yours,
      -- blue=allied, yellow=neutral, red=hostile, dim=dead (pills only).
      local function owner_col(o)
        if     o == "friendly" then return 90, 230, 90
        elseif o == "allied"   then return 90, 170, 255
        elseif o == "neutral"  then return 230, 220, 90
        else                        return 235, 80, 80 end
      end
      local function gather_sorted(tbl, want_health)
        local out = {}
        for id, e in pairs(tbl or {}) do
          out[#out + 1] = { id = id, owner = e.owner, dead = want_health and (e.health or 0) <= 0 or false }
        end
        table.sort(out, function(a, b) return a.id < b.id end)
        return out
      end
      local function draw_grid(items, gx, gy, per_row)
        for i, it in ipairs(items) do
          local cx = gx + ((i - 1) % per_row) * 11
          local cy = gy + math.floor((i - 1) / per_row) * 11
          local r, g, b = owner_col(it.owner)
          if it.dead then r, g, b = 70, 70, 80 end
          viz.hud_rect(HID, cx, cy, 9, 9, "topleft", r, g, b, 235, true)
          viz.hud_rect(HID, cx, cy, 9, 9, "topleft", 20, 20, 28, 255, false)
        end
      end
      local pillg = gather_sorted(world.pills, true)
      local baseg = gather_sorted(world.bases, false)
      viz.hud_text(HID, px0 + 6, fy + 24, string.format("PILLS %d", #pillg), "topleft", 200, 200, 210)
      draw_grid(pillg, px0 + 6, fy + 34, 8)
      viz.hud_text(HID, px0 + 6, fy + 58, string.format("BASES %d", #baseg), "topleft", 200, 200, 210)
      draw_grid(baseg, px0 + 6, fy + 68, 8)
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
    -- Emit a one-shot build if a tile was clicked this/last tick (manual_pending_build
    -- latched by Brain.on_click). The engine reads {x,y,action} once and kicks off the
    -- LGM build; clear it so it doesn't repeat. build=-1 means "no build" otherwise.
    local mbuild = manual_pending_build or -1
    manual_pending_build = nil
    state._think_attempt = nil   -- reached an exit: this think was not killed
    return { holdkeys = manual_keys, tapkeys = 0, build = mbuild, wantallies = info.allies, messagedest = 0, sendmessage = "" }
  end

  -- Instruction-sampling profiler arm/start. Placed here, after the dead,
  -- startup, and manual-control early-out returns above, for two reasons:
  --   1. Every armed tick reaches a paired prof.stop() — either at the tail
  --      return or at the paused early-out below — so start/stop never desync.
  --      The dead/startup/manual returns exit before arming and are
  --      deliberately unprofiled (no cognition worth attributing).
  --   2. prof.start() installs a debug.sethook that replaces the host's
  --      per-think budget count hook (bot_manager installs one around think;
  --      a Lua state holds only one hook), so tick-budget enforcement is off
  --      while --instr-profile is active. This is a benchmark-only diagnostic.
  -- Arm lazily/once: bot number and DEBUG_SESSION_DIR are only reliable here.
  if _G.BRAIN_INSTR_PROFILE and not state._instr_prof_on then
    state._instr_prof_on = true
    prof.configure({
      dir    = _G.DEBUG_SESSION_DIR or ".",
      prefix = "p" .. tostring(info.player_number or 0),
    })
  end
  if state._instr_prof_on then prof.start() end

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

    -- Live stop-distance prediction (toggle: stop_predict_live). One filled
    -- orange square per simulated brake step, trailing in front of the tank to
    -- the engine-exact predicted brake-now stop point — EVERY tick regardless
    -- of substate (unlike charge_stop_pred / approach_stop_pred, which only draw
    -- during those phases). Drawn here, after overlay_clear, so it survives the
    -- wipe. Default off; flip it on in the V window.
    if viz.is_on("stop_predict_live") and info.tankx then
      local _tmx, _tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
      local _tcap = (C.TERRAIN_SPEED and C.TERRAIN_SPEED[U.ttype_peek(_tmx, _tmy)]) or 16
      local _ang = info.tank_angle or info.direction or 0
      local _espeed = (info.speed or 0) / 4  -- info.speed is engine speed ×4; model wants engine units
      local _psx, _psy, _trace = cpf.predict_stop(info.tankx, info.tanky, _ang, _espeed, _tcap, true)
      if _trace then
        for _, _s in ipairs(_trace) do
          local _x, _y = _s.x / 256, _s.y / 256
          viz.rect("stop_predict_live", _x - 0.18, _y - 0.18, _x + 0.18, _y + 0.18, 255, 150, 0, 220, true, false)
        end
      end
      local _d = math.sqrt((_psx - info.tankx)^2 + (_psy - info.tanky)^2) / 256
      viz.text("stop_predict_live", _psx / 256, _psy / 256 - 0.5, string.format("STOP %.2ft spd=%.1f", _d, _espeed), "center", 255, 180, 60, 230, 0.35)
    end
  end -- BRAIN_DEBUG_MODE

  -- Optional debug log — keeps the per-tick shell positions in
  -- hitboxes.log for one-off bug hunts. Rendering is handled by
  -- draw_shell_hitbox_viz above; this block ONLY writes the log.
  if BRAIN_DEBUG_MODE and viz.is_on("hitbox_logs") then
  local t_hb0 = BRAIN_PROFILE and clock_us() or 0
  do
    local hb_log = io.open("hitboxes.log", "a")
    if hb_log and info.objects then
      local shell_count = 0
      for i = 1, #info.objects do
        local ob = info.objects[i]
        if ob.type == 1 then
          shell_count = shell_count + 1
          local smx = bit.rshift(ob.x, 8)
          local smy = bit.rshift(ob.y, 8)
          local px  = bit.rshift(ob.x, 4)
          local py  = bit.rshift(ob.y, 4)
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
    if BRAIN_PROFILE then
      opt(string.format("  hitboxes.log write done %.2f ms", (clock_us() - t_hb0) / 1000))
    end

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
      local lmx = bit.rshift(info.man_x, 8)
      local lmy = bit.rshift(info.man_y, 8)
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
        local lxt, lxr = bit.rshift(info.man_x, 8), bit.band(info.man_x, 0xFF)
        local lyt, lyr = bit.rshift(info.man_y, 8), bit.band(info.man_y, 0xFF)
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
          row(3, "FIRE_SUPPRESS",    f.fire_suppress)
          row(4, "STRANDED",         state.lgm_stranded == true)
          row(5, string.format("dist=%d", f.lgm_dist or -1), false)
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
    local t_ix0 = BRAIN_PROFILE and clock_us() or 0
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
    if BRAIN_PROFILE then
      opt(string.format("  intersect.log scan done %.2f ms", (clock_us() - t_ix0) / 1000))
    end
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
  --
  -- Whole block under BRAIN_DEBUG_MODE (was a bare `do`): its only effects are
  -- the print2 and the viz.hud_text calls, both of which lua_strip removes, so
  -- in opt/ it was building `goal_str` (one string.format, plus a `{}` for a
  -- nil goal) every tick for nobody. --strip-block now takes the lot.
  if BRAIN_DEBUG_MODE then
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
      -- NEXT goal: the precomputed drive-through lookahead (state.next_goal)
      -- when it's set, otherwise the cheapest NON-winning pool candidate — i.e.
      -- "what I'd switch to next". Own layer so it can be toggled independently.
      if viz.is_on("hud_next_goal") then
        local nk, nmx, nmy, ncost
        if state.next_goal and state.next_goal.kind and state.next_goal.kind ~= "none" then
          nk, nmx, nmy = state.next_goal.kind, state.next_goal.mx, state.next_goal.my
        else
          local best = nil
          for _, c in ipairs(state.last_goal_pool or {}) do
            if not c.winner and c.cost and (not best or c.cost < best.cost) then best = c end
          end
          if best then nk, ncost = best.desc or "?", best.cost end
        end
        if nk then
          local txt = nmx and string.format("NEXT: %s @(%d,%d)", nk, nmx, nmy)
                          or string.format("NEXT: %s  cost=%.0f", nk, ncost or 0)
          viz.hud_text("hud_next_goal", 10, gy, txt, "topright", 120, 200, 255)
          gy = gy + 10
        end
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
  if BRAIN_PROFILE then
    opt(string.format("early-viz/HUD done %.2f ms", (t0 - t_early) / 1000))
  end

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
        U.set_blocked(state, bk, now + 300, "no_build_site")
        log.event("assist_msg", string.format("no_build at %d,%d",
          state.builder.target.x, state.builder.target.y))
      end
    elseif msg == ASSIST_MSG_PILL_NO_REPAIR then
      -- Pill doesn't need repair: cancel repair goal
      if state.goal.kind == "repair_pill" then
        attack.clear_attack_goal(state, "assist: pill_no_repair")
        log.event("assist_msg", "pill_no_repair")
      end
    elseif msg == ASSIST_MSG_MAN_DEAD then
      -- LGM died: note for tactical decisions
      log.event("assist_msg", "man_dead")
    end
  end

  local t_events = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  events+assist done %.2f ms", (t_events - t0) / 1000))
  end

  -- Selective cache invalidation based on pill/terrain changes
  PF.begin_tick(now, world)
  local t_pf_begin = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  PF.begin_tick done %.2f ms", (t_pf_begin - t_events) / 1000))
  end

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
  local t_wu0 = BRAIN_PROFILE and clock_us() or 0
  W.update(world, info, now)
  local t_wu1 = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  W.update done %.2f ms", (t_wu1 - t_wu0) / 1000))
  end

  -- Update exploration frontier
  expl.update(state, info)
  if BRAIN_PROFILE then
    opt(string.format("  expl.update done %.2f ms", (clock_us() - t_wu1) / 1000))
  end

  local t1 = clock_us()
  metrics.set("us_world", t1 - t0)
  if BRAIN_PROFILE then
    opt(string.format("world done %.2f ms", (t1 - t0) / 1000))
  end

  -- Update shell-trajectory danger map
  danger.update(info, now)

  local t2 = clock_us()
  metrics.set("us_danger", t2 - t1)
  if BRAIN_PROFILE then
    opt(string.format("danger done %.2f ms", (t2 - t1) / 1000))
  end

  -- Process sound events into combat heat map
  local t_hearing0 = BRAIN_PROFILE and clock_us() or 0
  hearing.update(info, now)
  local t_hearing1 = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  hearing.update done %.2f ms", (t_hearing1 - t_hearing0) / 1000))
  end

  -- Rebuild spatial threat grid (pill + tank layers, O(1) lookup for consumers)
  threat.update(state, world, info)
  local t_threat_upd = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  threat.update done %.2f ms", (t_threat_upd - t_hearing1) / 1000))
  end

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
  if BRAIN_PROFILE then
    opt(string.format("  cpf.load_danger done %.2f ms (rebuilt=%s)",
      (t_load_danger - t_threat_upd) / 1000, tostring(threat.rebuilt_this_tick)))
  end

  -- The two build scores. Must run AFTER threat.update: imdanger's exposure
  -- term reads threat.pill_at, which is only valid once the grid is rebuilt.
  -- Stored on state so every consumer this tick sees one consistent pair
  -- rather than recomputing (and the log line stays a single source of truth).
  state.vuln, state.imdanger, state.vuln_terms, state.imdanger_terms =
    danger.scores(info, world, state)
  local t_scores = clock_us()
  metrics.set("us_scores", t_scores - t_load_danger)
  if BRAIN_PROFILE then
    opt(string.format("  danger.scores done %.2f ms (vuln=%.1f imd=%.1f)",
      (t_scores - t_load_danger) / 1000, state.vuln, state.imdanger))
  end

  -- ── Placement trip lifecycle ─────────────────────────────────────────────
  -- state._place_trip is set when a placement request goes out (after
  -- builder.decide, below) and examined HERE, once per tick, before goal
  -- selection -- so eval_wait_for_lgm and the resume see this tick's answer.
  -- The return is a man_status EDGE (prev ~= INTANK -> INTANK), never a level,
  -- so a momentary flap mid-trip cannot destroy a harvest already paid for.
  -- Runs after danger.scores because the harvest resume consults panic first.
  -- Table of events -> flag: VULNERABILITY_AND_BUILDS_PLAN.md "The trip flag".
  do
    local trip = state._place_trip
    local man  = info.man_status
    local prev = state._trip_prev_man
    state._trip_prev_man = man
    if trip then
      if man ~= C.LGM_INTANK then trip.left = true end
      local returned = (man == C.LGM_INTANK and prev ~= nil and prev ~= C.LGM_INTANK)
      local reason, resume_score
      if man == C.LGM_DEAD then
        reason = "lgm_lost"
      elseif trip.harvest and (info.carried_pills or 0) == 0 then
        -- Scoped to harvest trips: on a plain placement the pill leaves WITH
        -- the builder (tankGetCarriedPill, lgm.c:443), so carried==0 for the
        -- whole walk is normal there and must not clear the flag.
        reason = "no_pills"
      elseif not trip.left and (now - trip.tick) > (C.PLACE_TRIP_ACCEPT_TICKS or 5) then
        reason = "not_accepted"   -- engine refused the request; builder never left
      elseif (now - trip.tick) > (C.PLACE_TRIP_MAX_TICKS or 2000) then
        reason = "timeout"
      elseif returned then
        if not trip.harvest then
          reason = "placed"
        elseif danger.should_panic_build(state, info) then
          -- Panic is evaluated first and overrides the resume: the pool's
          -- panic build fires this tick on its own.
          reason = "panic_override"
        else
          local ok, why = builder.place_tile_valid(info, world, trip.mx, trip.my)
          if not ok then
            reason = (why == "unreachable") and "unreachable" or ("invalid_" .. why)
          elseif trip.score_at_dispatch then
            -- Fresh re-score, TRAVEL-FREE: sc7 pinned (as before) AND the tank
            -- position pinned to where we stood when the builder was sent.
            -- sc7 was never the only tank-relative term -- the strategic-centre
            -- chain (nearest friendly base / nearest hostile pill / offensive
            -- spike base) is measured from the tank as well -- so a tank that
            -- drove off mid-harvest failed its own margin on distance it
            -- created: 20260902_030233 bot2 t=37445, 311.3 -> 181.7 after
            -- refuelling 22 tiles away, and a paid-for harvest thrown out. The
            -- question the margin has to answer is "is the TILE still worth the
            -- pill", so ask it from the dispatch position. (The follow-through
            -- bid in goals.lua is what stops the tank leaving in the first
            -- place; this is the belt to that pair of braces -- an interrupt
            -- the follow-through row correctly lost to still must not cost us
            -- the wood.)
            -- nil = the tile no longer qualifies at all (category drift under
            -- STRICT_NEED, surplus role, blocked).
            resume_score = goals.score_place_tile(state, world, info,
                                                  trip.mx, trip.my, trip.sc7_at_dispatch,
                                                  trip.tank_mx, trip.tank_my)
            -- The live-tank number, for the log only: it is what this code used
            -- to compare, so both are printed and the change is auditable.
            local ltmx, ltmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
            local full_score
            if trip.tank_mx and (trip.tank_mx ~= ltmx or trip.tank_my ~= ltmy) then
              full_score = goals.score_place_tile(state, world, info,
                                                  trip.mx, trip.my, trip.sc7_at_dispatch)
            else
              full_score = resume_score   -- tank never moved: same question
            end
            local function _sfmt(v) return v and string.format("%.1f", v) or "nil" end
            local _detail = string.format(
              "travel-free from dispatch tank @(%d,%d); full %s from live tank @(%d,%d)",
              trip.tank_mx or ltmx, trip.tank_my or ltmy, _sfmt(full_score), ltmx, ltmy)
            local s0     = trip.score_at_dispatch
            local margin = math.max(C.HARVEST_RESUME_MARGIN_ABS or 30,
                                    s0 * (C.HARVEST_RESUME_MARGIN_FRAC or 0.15))
            if not resume_score then
              -- STRICT_NEED category drift, surplus role, blocked tile, or
              -- reposition-origin exclusion: the re-score refused the cell.
              -- No line here: the generic HARVEST_DROP below covers every
              -- reason except worse_than_margin, and printing one as well gave
              -- two HARVEST_DROP lines for one drop (20260902_092340 bot2
              -- t=14102). The detail rides along on that line instead.
              reason = "disqualified"
            elseif (s0 - resume_score) > margin then
              reason = "worse_than_margin"
              print2(string.format("HARVEST_DROP t=%d tile=(%d,%d) reason=%s score %.1f -> %.1f (%+.1f, margin %.1f) [%s]",
                now, trip.mx, trip.my, reason, s0, resume_score, resume_score - s0, margin, _detail))
            end
            trip._resume_detail = _detail
          end
          if not reason then
            -- Valid (and within margin, when there was a score to hold it to):
            -- re-dispatch to place. builder.set_mode consumes this, no pool.
            state._place_resume = { mx = trip.mx, my = trip.my, tick = now }
            print2(string.format("HARVEST_RESUME t=%d tile=(%d,%d) score %s -> %s%s -> dispatch",
              now, trip.mx, trip.my,
              trip.score_at_dispatch and string.format("%.1f", trip.score_at_dispatch) or "none",
              resume_score and string.format("%.1f", resume_score) or "n/a",
              trip._resume_detail and (" [" .. trip._resume_detail .. "]") or ""))
            reason = "resume"
          end
        end
      end
      if reason then
        if trip.harvest and reason ~= "resume" and reason ~= "worse_than_margin" then
          -- _resume_detail exists only when the re-score actually ran (the
          -- disqualified case); the other reasons never got that far.
          print2(string.format("HARVEST_DROP t=%d tile=(%d,%d) reason=%s%s",
            now, trip.mx, trip.my, reason,
            trip._resume_detail and (" [" .. trip._resume_detail .. "]") or ""))
        elseif not trip.harvest then
          print2(string.format("PLACE_TRIP_END t=%d tile=(%d,%d) reason=%s after %dt",
            now, trip.mx, trip.my, reason, now - trip.tick))
        end
        state._place_trip = nil
      end
    end
    -- A pending resume that decide() never turned into a command (its
    -- priority branches early-return on water/road builds): drop it rather
    -- than hold the builder mode forever. Cleared normally when the trip is
    -- set from the resume dispatch.
    local rs = state._place_resume
    if rs and (now - rs.tick) > (C.HARVEST_RESUME_MAX_TICKS or 25) then
      print2(string.format("HARVEST_DROP t=%d tile=(%d,%d) reason=stalled (resume not dispatched in %dt)",
        now, rs.mx, rs.my, now - rs.tick))
      state._place_resume = nil
    end
  end

  -- Overlay rebuild: pills/bases stamped as impassable/expensive in the
  -- pathfinder. Moved here (right after danger load) so all downstream
  -- goal evaluation sees fresh overlays — previously ran much later and
  -- A* queries during eval would hit stale impassable stamps from dead pills.
  local t_trip_life = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  placement trip lifecycle done %.2f ms",
      (t_trip_life - t_scores) / 1000))
  end
  threat.check_overlay_dirty(world)
  if threat.overlay_dirty then
    threat.rebuild_overlay(world)
    metrics.inc("overlay_reloads")
  else
    metrics.inc("overlay_skips")
  end
  local t_overlay_early = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  overlay rebuild done %.2f ms (dirty=%s)",
      (t_overlay_early - t_trip_life) / 1000, tostring(not threat.overlay_dirty)))
  end

  -- Ally avoidance: stamp a cost penalty around the pill target of
  -- allied bots that are mid-pill-take so we don't drive through
  -- their combat zone. Uses ally_state goal/substate + target mx/my.
  -- Also stamps a 2-tile-wide firing lane from the ally tank to the
  -- pill. Includes approach now — if their tank is far from the pill
  -- (>12 tiles) the firing-lane stamp is skipped below, so the only
  -- effect from far is the small radius around the pill itself.
  -- Substates where an ally is committed AT its pill-take spot (setup/standoff)
  -- and shouldn't be driven through. Gates BOTH the 4-cardinal blitz stamp and
  -- the 5x5 solo stamp below; the in_range euclidean gate keeps a stamp from
  -- appearing while the ally is still driving in. Audited to cover the full
  -- blitz lifecycle (plan_position/approach/gather_trees/detree/build_walls/
  -- blitz_wait/aim/in_range_*/charge/shoot_pill/swerve) AND the solo wall-shield
  -- take (ws_*) and hardline firing — anything where the tank holds/maneuvers
  -- at the spot. Leaving/transient states (disengage/post_engage/loiter/
  -- curve_away/reposition) and non-combat lifecycle states stay excluded.
  local ALLY_COMBAT_SUBS = {
    plan_position=true, approach=true, gather_trees=true,
    aim=true, charge=true, engage=true, shoot_pill=true, swerve=true,
    in_range_position=true, in_range_aim=true, in_range_aim_pre=true,
    in_range_aim_finetune=true, build_walls=true, detree=true,
    blitz_wait=true,
    ws_prebuild=true, ws_prewait=true, ws_engage=true,
    ws_advance=true, ws_rebuild=true, ws_retreat=true,
    kill_hardline=true,
  }
  -- At-spot blitz substates: the ally is PARKED at its standoff, so its BROADCAST
  -- standoff is a reliable position proxy when we can't see its tank (trees / view
  -- limit) — exactly when converging blitzers most need to avoid each other.
  local ALLY_AT_SPOT_SUBS = {
    blitz_wait = true, aim = true, shoot_pill = true,
    in_range_position = true, in_range_aim = true, in_range_aim_pre = true,
    in_range_aim_finetune = true, ws_engage = true, ws_rebuild = true,
    kill_hardline = true,
  }
  -- Substates where the ally is LOCKED ON and firing (or about to / just did),
  -- so shells are crossing the lane to its target pill. Friendly fire kills any
  -- tank in that lane regardless of blitz vs solo, so we stamp the bullet path
  -- as avoid cost in these subs for BOTH take styles. Excludes pure travel/setup
  -- (approach/gather_trees/detree/build_walls/blitz_wait/ws_pre*) where no shells fly.
  local FIRING_SUBS = {
    charge = true, engage = true, shoot_pill = true,
    kill_hardline = true, ws_engage = true,
  }
  local ALLY_AVOID_RADIUS = 2  -- 5x5 block around the ally tank itself
  local ALLY_AVOID_COST   = C.ALLY_AVOID_COST or 800
  local ALLY_BLITZ_TILE_COST = C.ALLY_BLITZ_TILE_COST or 12000  -- single exact tile of a blitz participant
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

    local function stamp(x, y, cost)
      if x < 0 or x > 255 or y < 0 or y > 255 then return end
      local pkey = y * 256 + x
      if world.pill_at and world.pill_at[pkey] then return end
      if world.base_at and world.base_at[pkey] then return end
      local mk = U.mkey(x, y)
      if stuck_bl and stuck_bl[mk] then return end  -- defer to stuck_blacklist
      cpf.set_overlay(x, y, cost or ALLY_AVOID_COST)
      stamped[mk] = true
      if BRAIN_DEBUG_MODE and viz.is_on("ally_avoid_overlay") then
        viz.rect("ally_avoid_overlay", x, y, x + 1, y + 1, 255, 165, 0, 60)
      end
    end

    -- Stamp the 3-wide bullet path from a firing ally at (ox,oy) to its target
    -- pill at (tx,ty) as avoid cost, so we never route a tank across live shells.
    -- Bullets don't discriminate blitz vs solo — both takes call this while in a
    -- FIRING_SUBS substate. Drawn on ally_avoid_overlay via stamp() automatically.
    local function stamp_firing_lane(ox, oy, tx, ty)
      if not (ox and oy and tx and ty) then return end
      if U.mdist(ox, oy, tx, ty) > 12 then return end
      local ldx = ty - oy
      local ldy = -(tx - ox)
      local llen = math.max(1, math.sqrt(ldx * ldx + ldy * ldy))
      local pnx, pny = ldx / llen, ldy / llen
      U.line_walk(ox + 0.5, oy + 0.5, tx + 0.5, ty + 0.5,
        function(lx, ly)
          for w = -1, 1 do
            stamp(U.mclamp(math.floor(lx + pnx * w + 0.5)),
                  U.mclamp(math.floor(ly + pny * w + 0.5)))
          end
        end)
    end

    -- Right-of-way: stamp the projected next tiles of any ally we're JUNIOR to
    -- (their player id < ours) as high cost, so our pathfinder routes around
    -- their near-future path — we give way, they don't. Project 0..LOOKAHEAD
    -- tiles along the ally's heading (only while it's actually moving). Reads
    -- current-tick info.objects directly (this block runs before percept.update,
    -- so state.perc would be a tick stale). Reuses the stamp()/clear set above,
    -- so it's wiped next tick (no trail). ob.idnum == player number for tanks.
    if info.objects and info.player_number then
      local LOOK = C.ALLY_YIELD_LOOKAHEAD or 2
      for _, ob in ipairs(info.objects) do
        if ob.type == 0 and (bit.band(ob.info, OBJECT_HOSTILE)) == 0   -- OBJECT_TANK, friendly
           and ob.idnum ~= nil and info.player_number > ob.idnum
           and (ob.speed or 0) >= (C.ALLY_YIELD_MIN_SPEED or 6) then
          local asd = U.bsin(ob.direction or 0)
          local acd = U.bcos(ob.direction or 0)
          for i = 0, LOOK do
            stamp(bit.rshift((ob.x + asd * 2 * i), 8), bit.rshift((ob.y - acd * 2 * i), 8))
          end
        end
      end
    end

    -- Visible tank positions keyed by player number (idnum), tile coords.
    -- Source for the avoid-stamp centre below: we read each ally's live
    -- position from our own game view instead of having them broadcast
    -- tx/ty every tick.  Player numbers are globally unique, so indexing
    -- all tanks (not just allied) is safe — only ally pns are looked up.
    -- Live obstacle set for the route tracer: exact tiles of OTHER tanks we'd
    -- bump, packed as y*256+x. Passed to cpf.path_to so navigation veers around
    -- them instantly (see steering.cpf_path_to). Always on — you can collide with
    -- an ally in ordinary nav, not just during a blitz.
    local nav_avoid_tiles = {}
    -- Cautious-approach set (packed tile -> true): the 3x3 ring around any ally
    -- currently ON a pill take. When OUR tank enters this zone, steering crawls
    -- tile-by-tile (boat-style lookahead suppression) so it follows the avoiding
    -- route exactly instead of drifting/cutting onto the ally's tile. Strictly
    -- scoped to allies doing a take — unrelated to the generic nav_avoid set.
    local ally_take_tiles = {}
    local _ally_tank_pos = nil
    if info.objects then
      for _, ob in ipairs(info.objects) do
        if ob.type == 0 and ob.idnum ~= nil then  -- OBJECT_TANK
          _ally_tank_pos = _ally_tank_pos or {}
          _ally_tank_pos[ob.idnum] = { mx = bit.rshift(ob.x, 8), my = bit.rshift(ob.y, 8) }
          -- Every visible FRIENDLY tank except ourselves is an avoid-tile.
          if (bit.band(ob.info, OBJECT_HOSTILE)) == 0 and ob.idnum ~= info.player_number then
            nav_avoid_tiles[#nav_avoid_tiles + 1] = (bit.rshift(ob.y, 8)) * 256 + (bit.rshift(ob.x, 8))
          end
        end
      end
    end

    for ally_pn, slot in ally_state.iter_active(now_aa, 1750) do
      if ally_pn ~= info.player_number then
        local ai = slot.info
        -- Compute the "ally tank within 3 euclidean of either setup
        -- or standoff" gate up front so both the diagnostic overlay
        -- and the stamping block use the same source of truth.
        -- Pill tile resolved from the broadcast target id against our own
        -- world view (attack_pill no longer ships mx/my — target ids the
        -- pill, the tile is local knowledge).
        local pmx, pmy
        local _pid  = tonumber(ai.target)
        local _pill = (_pid and world.pills) and world.pills[_pid] or nil
        if _pill then pmx, pmy = _pill.mx, _pill.my end
        -- Ally tank position from our own game view (no longer broadcast
        -- as tx/ty); nil when the ally isn't currently in view.
        local _atp = _ally_tank_pos and _ally_tank_pos[ally_pn] or nil
        local atmx = _atp and _atp.mx or nil
        local atmy = _atp and _atp.my or nil
        -- Setup/standoff geometry from the packed "p" extra (8 hex chars:
        -- approach_mx/my + standoff_mx/my).  Static per take, so it ships
        -- on the extra channel only when it changes.
        local smx, smy, ssx, ssy
        if ai.p and #ai.p == 8 then
          smx = tonumber(ai.p:sub(1, 2), 16)
          smy = tonumber(ai.p:sub(3, 4), 16)
          ssx = tonumber(ai.p:sub(5, 6), 16)
          ssy = tonumber(ai.p:sub(7, 8), 16)
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
          -- Record this take's 3x3 ring for the cautious-approach (slow, per-tile)
          -- steering. Center = the ally's live tile, or its broadcast standoff
          -- when parked out of sight (same source as the blitz tile stamp below).
          local _tkx, _tky
          if atmx and atmy then _tkx, _tky = atmx, atmy
          elseif ALLY_AT_SPOT_SUBS[ai.sub] and ssx and ssy then _tkx, _tky = ssx, ssy end
          if _tkx then
            for _ry = -1, 1 do
              for _rx = -1, 1 do
                local _x, _y = _tkx + _rx, _tky + _ry
                if _x >= 0 and _x <= 255 and _y >= 0 and _y <= 255 then
                  ally_take_tiles[_y * 256 + _x] = true
                end
              end
            end
          end
          -- Bullet-path avoid (BOTH blitz and solo): while this ally is firing,
          -- stamp the shell lane from its position to the target pill so we never
          -- route across live fire. Shooter = live tile if visible, else broadcast
          -- standoff. Friendly fire kills regardless of take style.
          if FIRING_SUBS[ai.sub] then
            local _lox = (atmx and atmy) and atmx or ssx
            local _loy = (atmx and atmy) and atmy or ssy
            stamp_firing_lane(_lox, _loy, pmx, pmy)
          end
          local is_blitz = ai.sqst == "blitz" or ai.sqst == "join"
          if is_blitz then
            -- A blitz converges SEVERAL tanks on ONE pill. Overlapping 5x5 +
            -- firing-lane stamps would price the whole area out, so for a blitz
            -- participant (commander sqst="blitz" / joined soldier sqst="join") we
            -- mark ONLY the single tile it currently occupies as effectively
            -- impassable (ALLY_BLITZ_TILE_COST) — enough to keep others from
            -- driving onto it, without walling off the cluster. Use the ally's
            -- LIVE tank tile whenever we can see it (no in_range gate, so the
            -- block FOLLOWS the ally as it drives into position); fall back to its
            -- BROADCAST standoff (ssx/ssy) only when it's out of sight but parked
            -- at-spot (blitz_wait/aim/in_range_*/…), so converging blitzers still
            -- avoid each other from the slate when trees/view hide the tank.
            local cmx, cmy
            if atmx and atmy then
              cmx, cmy = atmx, atmy
            elseif ALLY_AT_SPOT_SUBS[ai.sub] and ssx and ssy then
              cmx, cmy = ssx, ssy
            end
            if cmx then
              stamp(cmx, cmy, ALLY_BLITZ_TILE_COST)
              -- Visible tanks are already in nav_avoid_tiles from the objects
              -- sweep above; here only add the out-of-view broadcast standoff.
              if not (atmx and atmy) then nav_avoid_tiles[#nav_avoid_tiles + 1] = cmy * 256 + cmx end
              -- Plus the 4 cardinal neighbours of the blitz ally's tile, so the
              -- route tracer (and the nav_veer red tiles) gives a converging
              -- blitzer a one-tile berth on every side, not just its exact tile.
              local _BNDX, _BNDY = { 1, -1, 0, 0 }, { 0, 0, 1, -1 }
              for _bi = 1, 4 do
                local _bcx, _bcy = cmx + _BNDX[_bi], cmy + _BNDY[_bi]
                if _bcx >= 0 and _bcx <= 255 and _bcy >= 0 and _bcy <= 255 then
                  nav_avoid_tiles[#nav_avoid_tiles + 1] = _bcy * 256 + _bcx
                end
              end
              if BRAIN_DEBUG_MODE then print2(string.format("ALLY_AVOID_BLITZ t=%d p%d tile=(%d,%d)+4card src=%s sub=%s", now_aa, ally_pn, cmx, cmy, (atmx and atmy) and "live" or "bcast", tostring(ai.sub))) end
            end
          elseif pmx and pmy and atmx and atmy then
            -- Solo take: 5x5 tank stamp gated on euclidean ≤ 3 to setup or
            -- standoff (stops the breadcrumb trail while driving in). The firing
            -- lane is now stamped above (shared with blitz, FIRING_SUBS-gated).
            if in_range then
              for dy = -ALLY_AVOID_RADIUS, ALLY_AVOID_RADIUS do
                for dx = -ALLY_AVOID_RADIUS, ALLY_AVOID_RADIUS do
                  stamp(atmx + dx, atmy + dy)
                end
              end
            end
          end
        end
      end
    end
    -- Publish the obstacle set for steering.cpf_path_to (nil when no blitz).
    state._nav_avoid_tiles = (#nav_avoid_tiles > 0) and nav_avoid_tiles or nil
    -- Publish the cautious-approach ring (nil when no ally is on a take).
    state._ally_take_tiles = next(ally_take_tiles) and ally_take_tiles or nil
    -- cautious_nav_around_ally_take viz: the 3x3 cautious-approach ring(s) around allies on a
    -- pill take (yellow). When OUR tank sits on/steps onto one, the steer
    -- chokepoint paints that tile bright + logs TAKE_CRAWL (this layer, below).
    if BRAIN_DEBUG_MODE and viz.is_on("cautious_nav_around_ally_take") and state._ally_take_tiles then
      for _tk in pairs(state._ally_take_tiles) do
        local _rx, _ry = _tk % 256, __idiv(_tk, 256)
        viz.rect("cautious_nav_around_ally_take", _rx, _ry, _rx + 1, _ry + 1, 240, 220, 40, 70, true)
      end
    end
    -- nav_veer viz: obstacle tiles (red) + the resulting veered route to our goal
    -- (cyan), traced live via the obstacle-aware next-step so the dodge is visible.
    if BRAIN_DEBUG_MODE and viz.is_on("nav_veer") and #nav_avoid_tiles > 0 then
      for _, _k in ipairs(nav_avoid_tiles) do
        local _ox, _oy = _k % 256, math.floor(_k / 256)
        viz.rect("nav_veer", _ox, _oy, _ox + 1, _oy + 1, 255, 30, 30, 140, true)
      end
      if state.goal and state.goal.mx and state.goal.my then
        local _px, _py = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
        local _gx, _gy = state.goal.mx, state.goal.my
        for _ = 1, 16 do
          local _nx, _ny = cpf.dijkstra_next_step(cpf.KIND_NORMAL, _px, _py, _gx, _gy, nav_avoid_tiles, C.NAV_AVOID_PENALTY)
          if not _nx or (_nx == _px and _ny == _py) then break end
          viz.line("nav_veer", _px + 0.5, _py + 0.5, _nx + 0.5, _ny + 0.5, 0, 230, 230, 230)
          _px, _py = _nx, _ny
          if _px == _gx and _py == _gy then break end
        end
      end
    end
  end

  -- The ally-avoidance stamping above (pill radius + firing lane + tank 5x5
  -- for every ally mid-take) is the real cost of this stretch; it used to be
  -- reported under "pillcontrib export" because that marker measured from
  -- t_load_danger and swallowed everything in between.
  local t_ally_avoid = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  ally-avoid stamp done %.2f ms",
      (t_ally_avoid - t_overlay_early) / 1000))
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
  local t_pillcontrib = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  pillcontrib export done %.2f ms", (t_pillcontrib - t_ally_avoid) / 1000))
  end

  -- Populate influence grid (friendly = positive, hostile = negative).
  -- Rebuilt every tick — it's cheap and consumers expect fresh values.
  cpf.clear_influence()
  local _inf_bf, _inf_bh, _inf_pf, _inf_ph, _inf_sig = 0, 0, 0, 0, 0
  -- Neutral-pill signature for the influence tail below. Accumulated in the
  -- same pill walk as the friendly/hostile stamps rather than in a second
  -- pairs(world.pills) pass: it is a plain sum, so it does not depend on
  -- iteration order and folding the two walks together cannot change it.
  local _nsig = 0
  for id, b in pairs(world.bases) do
    if b.owner == "friendly" then
      cpf.stamp_influence(b.mx, b.my, C.BASE_INFLUENCE_RADIUS, C.BASE_INFLUENCE_STRENGTH)
      _inf_bf = _inf_bf + 1; _inf_sig = _inf_sig + (id * 131 + 1 + b.mx * 7 + b.my * 13)
    elseif b.owner == "hostile" then
      cpf.stamp_influence(b.mx, b.my, C.BASE_INFLUENCE_RADIUS, -C.BASE_INFLUENCE_STRENGTH)
      _inf_bh = _inf_bh + 1; _inf_sig = _inf_sig + (id * 131 + 2 + b.mx * 7 + b.my * 13)
    end
  end
  for id, pm in pairs(world.pills) do
    -- not in_tank: a pillbox riding in a tank holds no ground, so it stamps no
    -- influence. (Also avoids divergence from a moving carried pill whose stale
    -- position differs per ally.)
    if pm.health > 0 and not pm.in_tank then
      if pm.owner == "friendly" then
        cpf.stamp_influence(pm.mx, pm.my, C.PILL_INFLUENCE_RADIUS, C.PILL_INFLUENCE_STRENGTH)
        _inf_pf = _inf_pf + 1; _inf_sig = _inf_sig + (id * 9173 + 1 + pm.mx * 7 + pm.my * 13)
      elseif pm.owner == "hostile" then
        cpf.stamp_influence(pm.mx, pm.my, C.PILL_INFLUENCE_RADIUS, -C.PILL_INFLUENCE_STRENGTH)
        _inf_ph = _inf_ph + 1; _inf_sig = _inf_sig + (id * 9173 + 2 + pm.mx * 7 + pm.my * 13)
      elseif pm.owner == "neutral" then
        -- No stamp (a neutral pill holds no ground for either side); it only
        -- feeds the tail's rebuild signature.
        _nsig = _nsig + (id * 9173 + 3 + pm.mx * 7 + pm.my * 13)
      end
    end
  end
  -- INF_CHANGE: fire only on ticks where the stamped set (and thus the grid)
  -- actually changes. sig is position-sensitive, so two allies with identical
  -- grids share a sig — diff the fire-tick across allies to see convergence lag.
  if state._inf_last_sig ~= _inf_sig then
    print2(string.format("INF_CHANGE t=%s pn=%s bf=%d bh=%d pf=%d ph=%d sig=%d (was %s)",
      tostring(now), tostring(info.player_number), _inf_bf, _inf_bh, _inf_pf, _inf_ph, _inf_sig, tostring(state._inf_last_sig)))
    state._inf_last_sig = _inf_sig
  end
  -- Influence tail (constants.lua EXPAND_*). Rebuilt in C only when the
  -- stamped set or the neutral-pill set changes (or every EXPAND_REFRESH_TICKS
  -- as a backstop); merged into influence_grid every tick, since the stamp
  -- loop above wipes it. Must run BEFORE anything reads influence this tick.
  if C.EXPAND_ENABLED then
    local _tsig = _inf_sig + _nsig
    if state._tail_sig ~= _tsig
       or (now - (state._tail_tick or -1e9)) >= (C.EXPAND_REFRESH_TICKS or 250) then
      -- Debug-only stopwatch (read by the TAIL_REBUILD print2 below, which
      -- lua_strip removes), so don't pay for the clock read in opt/.
      local _t_tail = BRAIN_DEBUG_MODE and clock_us() or 0
      cpf.clear_neutral_zones()
      for _, pm in pairs(world.pills) do
        if pm.owner == "neutral" and pm.health > 0 and not pm.in_tank then
          cpf.stamp_neutral_zone(pm.mx, pm.my, C.PILL_INFLUENCE_RADIUS)
        end
      end
      cpf.rebuild_influence_tail(C.EXPAND_SEED_MIN, C.EXPAND_RADIUS, C.EXPAND_START,
                                 C.EXPAND_NEUTRAL_STEP, C.EXPAND_WATER_STEP,
                                 C.EXPAND_DEEP_MARGIN or 0,
                                 C.INFLUENCE_ENEMY_TAIL)
      state._tail_sig, state._tail_tick = _tsig, now
      state._tail_rebuilt_tick = now
      print2(string.format("TAIL_REBUILD t=%d sig=%d deep_margin=%d enemy_tail=%s %.2fms",
        now, _tsig, C.EXPAND_DEEP_MARGIN or 0, tostring(C.INFLUENCE_ENEMY_TAIL ~= false),
        (clock_us() - _t_tail) / 1000))
    end
    -- Merged unconditionally, even when _tsig says the tail is unchanged:
    -- cpf.clear_influence() above wipes influence_grid at the top of EVERY
    -- tick, so the merge is not re-applying an idempotent overlay, it is the
    -- only thing that puts the tail back on the freshly-restamped grid.
    -- brainPathfinderMergeInfluenceTail also refreshes influence_base_grid,
    -- which brainPathfinderInfluenceTailStats reads to report what the tail
    -- won this tick. Skipping it on an unchanged stamp set would drop the tail
    -- from the grid entirely, so it stays.
    cpf.merge_influence_tail()
    -- What the tail did this rebuild: how much ground each side's tail
    -- covers, how much of it actually won the merge (was not already
    -- stamped), and the front line -- sign-change cells -- on the stamps
    -- alone vs on the merged grid. front_after > front_before is the tail
    -- making a line the discs could not; won=0 means it claimed nothing new.
    if BRAIN_DEBUG_MODE and state._tail_rebuilt_tick == now then
      local tp, tn, wp, wn, fb, fa, tie = cpf.influence_tail_stats()
      print2(string.format(
        "TAIL_STATS t=%d tail_cells +%d -%d | won_merge +%d -%d | front stamps=%d merged=%d (%+d) | ties=%d",
        now, tp, tn, wp, wn, fb, fa, fa - fb, tie))
      -- ASCII picture of the merged grid, 64x64 around the tank, one char
      -- per tile:  '#' friendly stamp  '+' friendly tail  '=' hostile stamp
      -- '-' hostile tail  'F' front-line cell (sign change)  ' ' unclaimed.
      if C.EXPAND_DEBUG_MAP then
        local cx, cy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
        local x0, y0 = math.max(0, cx - 32), math.max(0, cy - 32)
        print2(string.format("TAIL_MAP t=%d origin=(%d,%d) 64x64", now, x0, y0))
        for y = y0, math.min(255, y0 + 63) do
          local row = {}
          for x = x0, math.min(255, x0 + 63) do
            local v = cpf.influence_at(x, y)
            local ch = " "
            if v ~= 0 then
              local n, s, w, e = cpf.influence_at(x, y - 1), cpf.influence_at(x, y + 1),
                                 cpf.influence_at(x - 1, y), cpf.influence_at(x + 1, y)
              local front = (v > 0 and (n < 0 or s < 0 or w < 0 or e < 0))
                         or (v < 0 and (n > 0 or s > 0 or w > 0 or e > 0))
              if front then ch = "F"
              elseif cpf.influence_tail_at(x, y) ~= 0 then ch = (v > 0) and "+" or "-"
              else ch = (v > 0) and "#" or "=" end
            end
            row[#row + 1] = ch
          end
          print2("TAIL_MAP " .. table.concat(row))
        end
      end
    end
  end
  -- KWDIAG (temporary): per-object allegiance + last_seen so allied bots can be
  -- diffed to find residual divergence and its cause (lag vs missed broadcast).
  -- Token: <id><cls><lastseen>; pills add 'T' when in_tank, 'A' if ally-sourced.
  -- The whole block (the two sorted id lists and the token tables, not just the
  -- print2 that consumes them) sits under `if BRAIN_DEBUG_MODE` so lua_strip's
  -- --strip-block drops all of it from opt/. Stripping only the print2 line left
  -- ~9 table allocations + two sorts + one string.format per object running
  -- every 8th tick in production for output nobody could read.
  if BRAIN_DEBUG_MODE then
    if (now % 8) == 0 then
      local _kc = { friendly="f", hostile="h", neutral="n", allied="a" }
      local _bids, _pids = {}, {}
      for id in pairs(world.bases) do _bids[#_bids+1] = id end
      for id in pairs(world.pills) do _pids[#_pids+1] = id end
      table.sort(_bids); table.sort(_pids)
      local _bp, _pp = {}, {}
      for _, id in ipairs(_bids) do
        local b = world.bases[id]
        _bp[#_bp+1] = string.format("%d%s(%d,%d)%s", id, _kc[b.owner] or "?", b.mx, b.my, b._ally_only and "A" or "")
      end
      for _, id in ipairs(_pids) do
        local p = world.pills[id]
        if (p.health or 0) > 0 then
          _pp[#_pp+1] = string.format("%d%s%s(%d,%d)%s", id, _kc[p.owner] or "?", p.in_tank and "T" or "", p.mx, p.my, p._ally_only and "A" or "")
        end
      end
      print2(string.format("KWDIAG t=%s pn=%s B[%s] P[%s]", tostring(now), tostring(info.player_number), table.concat(_bp, ","), table.concat(_pp, ",")))
    end
  end
  local t_influence = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  influence stamp done %.2f ms", (t_influence - t_pillcontrib) / 1000))
  end

  -- Front-line circles (R2): self-throttling rebuild (~5 s) from the now-fresh
  -- influence grid. Maintains state for circle win/loss + reinforcement (R3).
  circles.update(state, world, state.tick or now)
  -- Per-tick win/loss + reinforcement-need assessment (R3).
  circles.assess(state, world, info, state.tick or now)

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
  if BRAIN_PROFILE then
    opt(string.format("threat done %.2f ms", (t3 - t2) / 1000))
  end

  -- Build shared perception snapshot (before goal selection / builder / steering)
  percept.update(state, world, info)
  local t_percept_upd = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  percept.update done %.2f ms", (t_percept_upd - t3) / 1000))
  end

  -- SEA-PILL HARVEST plan refresh. Reads perc.deepsea_pill_ids, so it has to
  -- run after percept.update and before the pools (build_eval_queue prices
  -- deep-sea capture rows straight out of state._sea) and before sea_update.
  -- Self-cached: a full re-plan only happens when the cluster set changes, the
  -- threat grid is rebuilt, or SEA_PILL_SCAN_PERIOD ticks pass — every other
  -- tick this is a handful of comparisons, and on a map with no dead pills in
  -- deep sea it returns on the first line.
  goals.sea_refresh(state, world, info)
  if BRAIN_PROFILE then
    opt(string.format("  sea_refresh done %.2f ms", (clock_us() - t_percept_upd) / 1000))
  end

  -- Track our own fired shots from fire-to-impact (uses info.shells decrement
  -- to detect fires and info.objects OBJECT_SHOT entries to verify in-flight).
  shot_tracker.update(info, now)
  if BRAIN_DEBUG_MODE then shot_tracker.draw_overlay(now) end
  local t_shot_tr = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  shot_tracker done %.2f ms", (t_shot_tr - t_percept_upd) / 1000))
  end

  -- Classify game phase (reads state.perc, must run after percept.update)
  strategy.update(state, world, info)
  if BRAIN_PROFILE then
    opt(string.format("  strategy.update done %.2f ms", (clock_us() - t_shot_tr) / 1000))
  end

  local t4 = clock_us()
  metrics.set("us_percept", t4 - t3)
  if BRAIN_PROFILE then
    opt(string.format("percept done %.2f ms", (t4 - t3) / 1000))
  end

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
  local t_dij0 = BRAIN_PROFILE and clock_us() or 0
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

    -- Predicted goal-replan tick (same predicate as timer_fire, computed here
    -- because the dij scheduler runs BEFORE state.replan_this_tick is set).
    -- GOAL_REPLAN_INTERVAL (50) is a multiple of DIJKSTRA_SHORT_INTERVAL (10),
    -- so a short-slate restart ALWAYS coincides with a replan tick — stacking
    -- the ~0.13ms copy+restart on finalize_pools/pick_goal every cycle. We slip
    -- the discrete restart off predicted-replan ticks (bounded by MAX_DEFER so
    -- it can't starve). The amortized dijkstra_step still runs every tick; only
    -- the copy+start moves, and lookups fall back to the backup meanwhile.
    local predicted_replan =
      (now + (state.replan_offset or 0)) % C.GOAL_REPLAN_INTERVAL == 0
    local DIJ_RESTART_MAX_DEFER = 4

    local tmx = bit.rshift(info.tankx, 8)
    local tmy = bit.rshift(info.tanky, 8)
    local in_boat = info.inboat and 1 or 0
    local d = state.dij
    if not d then
      -- `main`: the two slates the stepper advances (the odd indices are their
      -- frozen backups). Built once with the slate table instead of as a
      -- { SLATE_SHORT_MAIN, SLATE_LONG_MAIN } literal inside the per-tick
      -- scheduler loop, which allocated it every tick.
      d = { slates = {}, main = { SLATE_SHORT_MAIN, SLATE_LONG_MAIN } }
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
      -- `s.active` is in this test for the same reason it is in the yield
      -- guard below: an INACTIVE slate has never been started, so there is
      -- nothing to "keep stepping until it finishes" and no backup to serve
      -- lookups in the meantime — holding off would hold off forever. The C
      -- side reports started_tick=0 / done=false / active=false for a
      -- never-started slate, so now that state.tick is seeded from the engine
      -- clock, `age` on a mid-game bot's FIRST think is already past
      -- `interval`; without this the LONG slate returned here every tick and
      -- was never started for the whole life of the bot, leaving every
      -- long-range cost lookup at INF. Unreachable at seed 0, where the first
      -- think has age=1 < interval and takes the `not s.active` start below.
      if wait_for_done and schedule_hit and s.active and not s.done then
        -- Holding off: keep stepping the current main until it
        -- finishes, then snapshot + restart on the next pass.
        return
      end
      -- Yield the discrete copy+restart off predicted goal-replan ticks so the
      -- snapshot doesn't stack on the goals block. Only when the slate is active
      -- (backup has coverage) and not overdue past MAX_DEFER; an inactive slate
      -- must start now since there's no backup to serve lookups.
      if schedule_hit and s.active and predicted_replan
         and age < interval + DIJ_RESTART_MAX_DEFER then
        if BRAIN_DEBUG_MODE then print2(string.format("dij RESTART yield slate=%d age=%d (replan tick) t=%d", main_idx, age, now)) end
        return
      end
      if not s.active or schedule_hit then
        if s.active then
          local t_cs = BRAIN_PROFILE and clock_us() or 0
          cpf.dijkstra_copy_slate(main_idx, backup_idx)
          if BRAIN_PROFILE then
            opt(string.format("dij COPY %d->%d done %.2f ms",
              main_idx, backup_idx, (clock_us() - t_cs) / 1000))
          end
          refresh_slate(backup_idx)
          if BRAIN_DEBUG_MODE then print2(string.format("dij COPY %d->%d at tick=%d age=%d", main_idx, backup_idx, now, age)) end
        end
        start_slate(main_idx, max_cost, boat, allow_boat)
      end
    end

    -- Respawn re-root: restart ALL slates from the new spawn position now,
    -- instead of waiting out each slate's interval. Mains get stepped/filled
    -- below; the backups are re-seeded empty here so far-tile lookups fall back
    -- to "unknown" (fallback cost) rather than serving DEATH-rooted-cheap costs
    -- until a main completes and overwrites them via the normal copy.
    if state._dij_reroot then
      state._dij_reroot = nil
      start_slate(SLATE_SHORT_MAIN,   C.DIJKSTRA_SHORT_MAX_COST, in_boat, in_boat)
      start_slate(SLATE_SHORT_BACKUP, C.DIJKSTRA_SHORT_MAX_COST, in_boat, in_boat)
      start_slate(SLATE_LONG_MAIN,    C.DIJKSTRA_MAX_COST,       in_boat, 1)
      start_slate(SLATE_LONG_BACKUP,  C.DIJKSTRA_MAX_COST,       in_boat, 1)
      -- The backups are now empty; far-tile lookups return INF until a
      -- main→backup copy refills them. That copy now happens the moment each
      -- main completes (see the step loop below), so the surface goes live in
      -- ~2.5s instead of waiting out the recompute interval (~15s of INF after
      -- a respawn). No special-case flag needed — it's the general behavior.
      if BRAIN_DEBUG_MODE then print2(string.format("DIJ_REROOT t=%d src=(%d,%d) — all slates re-rooted after respawn", now, tmx, tmy)) end
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
    for _, idx in ipairs(d.main) do
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
          -- Go live as soon as a fresh surface is ready: copy the just-completed
          -- main into its backup immediately, instead of waiting for the next
          -- scheduled restart to do it. The schedule only THROTTLES how often we
          -- re-root/recompute (CPU); there's no reason to keep serving an older
          -- backup once a newer COMPLETE one exists. Removes the ~2.5s per-cycle
          -- staleness window, and the ~15s of INF far-tile costs after a respawn
          -- (the reroot empties the backup; this refills it the moment the
          -- re-rooted main finishes). Copy only fires on `done`, so the backup is
          -- always a complete surface by construction.
          local backup = (idx == SLATE_SHORT_MAIN) and SLATE_SHORT_BACKUP
                      or (idx == SLATE_LONG_MAIN)  and SLATE_LONG_BACKUP or nil
          if backup then
            cpf.dijkstra_copy_slate(idx, backup)
            refresh_slate(backup)
          end
          if BRAIN_DEBUG_MODE then print2(string.format(
            "dij DONE slate=%d at_tick=%d (took %d ticks, expanded=%d) -> backup %s",
            idx, now, now - d.slates[idx].started_tick, expanded, tostring(backup))) end
        end
      end
      local step_us = clock_us() - t_step
      if BRAIN_PROFILE then
        opt(string.format("dij STEP %.2f ms  active=%d  budget(s/l=%d/%d)  total_exp=%d",
                             step_us / 1000, #active_slates, budget_short, budget_long, total_exp))
      end
      -- Scan for newly-reached bases on the short slate (the one most
      -- likely to gate goal selection at startup).
      _diag_log_dij_base_discoveries(now, SLATE_SHORT_MAIN, in_boat)
    end
  end
  local t_dij1 = clock_us()
  if BRAIN_PROFILE then
    opt(string.format("dij sched done %.2f ms", (t_dij1 - t_dij0) / 1000))
  end

  -- Outgoing message -- only one per tick
  local send_msg = nil
  local msg_dest = 0

  local open_msg_this_tick = false
  if state.send_open_msg then
    send_msg = state.paused and C.BRAIN_NAME .. " loaded (PAUSED — use 'start' to begin)."
                             or C.BRAIN_NAME .. " loaded."
    msg_dest = bit.lshift(1, state.player_number)
    state.send_open_msg = false
    open_msg_this_tick = true
  end

  -- Process EVERY incoming chat message this tick. info.messages is
  -- the new multi-message inbox added in the brain inbox C-side change;
  -- the legacy info.message field still works but only surfaces the
  -- first one. Iterating the array lets us see all ally /info traffic
  -- when several bots broadcast on the same tick (previously the bus
  -- silently dropped all but one).
  if info.messages then
    local _allies = info.allies or 0
    for _, m in ipairs(info.messages) do
      if m.text and m.text ~= "" then
        -- RECEIVING HERE: one line per inbound message (pairs with MSG_TX on the
        -- sender). ally=false means it'll be ignored as enemy traffic below.
        if BRAIN_DEBUG_MODE then print2(string.format("MSG_RX t=%d from=p%s ally=%s msg=%s", now, tostring(m.sender), tostring((m.sender == state.player_number) or (bit.band(_allies, (bit.lshift(1, (m.sender or 0))))) ~= 0), tostring(m.text))) end
        -- chat_log ring is debug-only (read only by the chat_log_overlay
        -- HUD); skip the ring writes entirely in production.
        if BRAIN_DEBUG_MODE then
          ally_state.chat_log_add("in", m.sender, m.text, now)
        end
        -- Coordination is ALLY-ONLY. Enemy bots run the same brain and
        -- broadcast the same /info verbs (state slate, blitz bco/bcc/bcq), so
        -- processing their messages would pollute our ally_state + blitz
        -- registry and even draw comm lines / negotiate joins with enemies.
        -- Gate on the alliance bitmask (self always allowed).
        local _from_ally = (m.sender == state.player_number)
                           or (bit.band(_allies, (bit.lshift(1, (m.sender or 0))))) ~= 0
        if _from_ally then
          comms.process_message(m.sender, m.text, now, state)
        end

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
            msg_dest = bit.lshift(1, state.player_number)
          end
        end
      end
    end
  end

  -- Reconcile allies' advertised carried pills (comms carry=) into world.pills
  -- so an in-tank pill the team holds is consistent for everyone, including bots
  -- that never saw the pickup. ally_state persists the carry field until the
  -- carrier's next /info state drops it (deploy), so this runs every tick off
  -- the latest slates. max_age 1600 (~32s) outlives the 30s heartbeat so a
  -- carry doesn't flicker-evict right at the heartbeat boundary.
  do
    local ac = {}
    for pn, slot in ally_state.iter_active(now, 1600) do
      if pn ~= state.player_number and slot.info and slot.info.carry then
        for idstr in tostring(slot.info.carry):gmatch("%d+") do
          local id = tonumber(idstr)
          if id then ac[id] = pn end
        end
      end
    end
    W.sync_ally_carried(world, ac, now)
  end

  -- Known-world (comms /info kw): fold ally-relayed base/pill allegiance +
  -- location into world.* (newest observed-tick wins), answer any pending
  -- resync query, then snapshot OUR first-hand allegiance changes this tick
  -- for the next outbound digest. Mirrors the sync_ally_carried placement.
  if state._kw_inbox and #state._kw_inbox > 0 then
    W.sync_ally_world(world, state._kw_inbox, now, info.player_number)
    state._kw_inbox = nil
  end
  -- First think (and after each respawn): ask allies to dump their known world.
  if not state._kw_inited then
    state._kw_inited = true
    state._kw_send_query = true
  end
  if state._kw_resync_req then
    state._kw_resync_req = nil
    if (state._kw_resync_cd or 0) <= now then
      W.kw_queue_all(world)                 -- drains over idle ticks via build_kw_message
      state._kw_resync_cd = now + (C.KW_RESYNC_COOLDOWN or 150)
    end
  end
  W.collect_kw_changes(world, now)

  -- Paused: accept commands but do nothing else
  if state.paused then
    log.log_tick(state, info, state.goal, 0, 0, nil)
    if state._instr_prof_on then prof.stop(state.server_tick or state.tick or 0) end
    state._think_attempt = nil   -- reached an exit: this think was not killed
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
    msg_dest = bit.lshift(1, state.player_number)
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
  local cur_mx = bit.rshift(info.tankx, 8)
  local cur_my = bit.rshift(info.tanky, 8)
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
    state._kw_send_query = true   -- re-acquire team's known world after respawn
    state._tank_track = nil   -- drop pre-death ghosts (fallback if info.dead was missed)
    -- Full blitz/squad wipe + registry re-discover. Fallback for when the
    -- info.dead death-tick reset was missed (GC pause / long think / Lua error):
    -- its distances are off the pre-death-rooted slate, so far calls look cheap
    -- until re-discovered fresh (prevents the cross-map blitz join).
    squad.reset_blitz_state(state)
    attack.clear_attack_goal(state, "respawned")
    -- Death drops EVERYTHING we carried. Clear the in_tank flag on any pill we
    -- were holding so a missed drop EVENT_PILL_UPDATE (the brain doesn't tick
    -- while dead) can't leave a phantom carried pill: it would keep us from
    -- recognizing the dropped (dead) pill as a pickup, keep the portfolio
    -- counting it as held, and keep advertising it to allies (bsi.carry). The
    -- dropped pill is re-localized cleanly the next time we see it on the map.
    for _, p in pairs(world.pills) do
      if p.in_tank and p.owner_player == info.player_number then
        p.in_tank      = false
        p._synth_carry = nil
        p.carrier      = nil
      end
    end
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
    -- Wipe pool_cache so goal_selection sees an empty pool and falls through to
    -- explore until the rolling eval re-costs from the new position.
    state.pool_cache          = nil
    -- KEEP the eval_queue (just rewind it) instead of wiping it. Two wins:
    --   1. The pool panel keeps rendering the pre-death candidates — now at INF
    --      via the invalidated cost_cache above — so it shows every pool marked
    --      invalid instead of going blank for ~50 ticks until a replan rebuilds.
    --   2. The per-tick eval (update_pool_cache) re-costs those candidates
    --      immediately rather than idling on a nil queue until the next replan,
    --      so the warmup actually progresses from tick 1.
    -- (The next replan's build_eval_queue still rebuilds it fresh as usual.)
    state.eval_queue_pos      = state.eval_queue and 1 or nil  -- rewind: re-cost from the top
    -- Re-sort the kept queue by euclidean distance to the NEW spawn position.
    -- build_eval_queue sorted it relative to our PRE-death position, so without
    -- this the rewound re-cost would evaluate goals near where we died first
    -- instead of near where we respawned — closest-to-spawn should win the
    -- limited warmup eval budget.
    if state.eval_queue then
      table.sort(state.eval_queue, function(a, b)
        local da = (a.obj.mx - cur_mx)^2 + (a.obj.my - cur_my)^2
        local db = (b.obj.mx - cur_mx)^2 + (b.obj.my - cur_my)^2
        return da < db
      end)
    end
    state._pill_eval_cache    = nil
    state._pill_eval_progress = nil
    state.goal_cooldowns      = {}
    state.goal_history        = {}
    state.blocked             = {}
    state.banned_pill_angles  = {}
    state.wounded_pill        = nil
    -- Re-engage the warmup gate: we respawned at a new position with all costs
    -- invalidated, so (exactly like startup) force the explore fallback until
    -- the pool re-warms — at least WARMUP_MIN_REAL_GOALS freshly-evaluated
    -- finite goals. Without clearing this latch warm_ready stayed true and the
    -- bot grabbed the FIRST re-costed goal (e.g. attack_pill #0) off a cache
    -- that was otherwise still at infinity.
    state._warm_ready     = nil
    state._warm_count     = 0
    state._warm_exit_done = nil
    state._eval_swept     = nil  -- new position: require a fresh full eval sweep
    -- Blitz-call registry: our knowledge of others' open calls is stale after a
    -- respawn (we ran no Lua while dead), and our own call is gone. Drop both
    -- and fire a discovery query so commanders re-announce their open calls.
    state.blitz_calls       = {}
    state._my_blitz_call    = nil
    state._blitz_query_send = true
    state._blitz_pill_reject = {}   -- stale no-spot rejects (pre-death geometry)
    state._blitz_comm_reject = nil
    -- Squad/blitz commitment: squad_blitz_accepted PERSISTS across ticks (it is
    -- not recomputed each tick), and the negotiation/engage/roster fields hold
    -- pre-death values. After respawn we are committed to nothing — drop it all
    -- so squad.update re-derives cleanly instead of resuming a dead game's blitz.
    state.squad_blitz_accepted    = nil
    state.squad_negotiate_cmdr    = nil
    state.squad_negotiate_pill    = nil
    state.squad_blitz_target      = nil
    state.squad_blitz_engage_mx   = nil
    state.squad_blitz_engage_my   = nil
    state.squad_blitz_bd          = nil
    state.squad_blitz_repos       = nil
    state.squad_blitz_in_position = nil
    state.squad_blitz_aimed       = nil
    state.squad_blitz_go          = nil
    state.squad_blitz_reject      = nil
    state.squad_blitz_roster      = nil
    state.squad_blitz_accept      = nil
    state._blitz_reject           = nil
    state.squad_cmdr              = nil
    -- Enemy ghost / lead-prediction tracking: stale predicted positions from
    -- before death — drop so we don't chase a dead game's ghost for a tick or two.
    state._prev_enemy_tanks  = nil
    state._enemy_lgm_history = nil
    state._kill_lgm_eval     = nil
    -- "New tank appeared" baseline: nil so the first post-respawn tick seeds the
    -- set instead of flagging every currently-visible enemy as newly-appeared
    -- (the new_tank_seen check below only fires when a prior baseline exists).
    state._prev_visible_tank_ids = nil
    state._prev_tank_snapshot    = nil  -- tank-death detector baseline
    -- Builder: fresh LGM after respawn — drop any stale build mode/target.
    state.builder = { mode = "infrastructure", target = nil, need_trees = 0, last_action = nil }
    -- Per-pill diff/spot/pickup cache: pre-death geometry, re-cost from scratch.
    state._pill_diff_cache = nil
    -- Capture-objective latch: stale after death.
    state.capture_objective = nil
    -- Re-root the Dijkstra slates at the NEW spawn next scheduler pass. Without
    -- this the long slate / backups stay rooted at the DEATH position (their
    -- interval is long), so far-tile cost lookups measure from where we died
    -- (e.g. a pill we died next to reads cheap from across the map). The
    -- scheduler restarts all 4 slates here instead of waiting out the interval.
    state._dij_reroot = true
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
        string.format("DEATH_WIPE R=%d", R), "center", 255, 140, 0, 200)
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
    -- Sea-pill harvest: parked on the firing spot while the LGM lays the mine
    -- / builds the boat, and while we shoot the mine.
    or (state.goal.kind == "capture_pill" and state.goal.sea
        and goals.SEA_COMMITTED[state.goal.substate or ""])
    -- Defend heat: parked in range, deliberately tickling our own pill.
    or (state.goal.kind == "defend_pill" and DEFEND_HEAT_STATIONARY_SUBS[state.goal.substate or ""])
    or state.goal.kind == "rescue_lgm"
    or state.goal.kind == "wait_for_lgm"
    -- take_cover parks ON PURPOSE once it reaches the chosen tile; the whole
    -- goal is "stand here instead of there", so stuck detection must not read
    -- the hold as a wedged tank and fire flee_pill.
    or state.goal.kind == "take_cover"
    -- kill_me_wait parks for the same reason: standing still on an advertised
    -- tile IS the goal, so the stuck detector must not read it as wedged.
    or state.goal.kind == "kill_me_wait"
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

  -- ...with one exception. A place_pill_strategic sitting on its drop spot
  -- while builder.decide()'s LGM gate keeps refusing (place_gate_fails is
  -- running) is NOT working by shooting back — it is the park-and-die stall:
  -- the tank can't place, the 50-tick replan re-picks the same spot at an
  -- ever-lower cost, and every returned shell used to zero the one timer that
  -- could have broken it (20260827_115304 bot3, t=12523 to death at t=12678).
  -- Scoped to that goal kind so a tank genuinely fighting from cover on any
  -- other goal still counts firing as progress. The counter is written later in
  -- the tick by builder.decide(), so this reads last tick's value — one tick of
  -- lag on a 100+ tick condition, which doesn't matter.
  -- The stamp check matters: builder.decide() can return before the gate branch
  -- (drowning / slow-terrain road builds), which leaves place_gate_fails frozen
  -- non-zero. Without the freshness test a stale count would keep this true and
  -- strip the firing-is-progress exemption from a tank that has moved off the
  -- drop spot and is genuinely fighting — wrongly tripping stuck-flee.
  -- The gate_stalled exemption is gone with the breaker it was built around.
  -- It stripped the firing-is-progress reset while a placement gate kept
  -- refusing, so a bot parked on an unplaceable spot and shooting back would
  -- eventually trip stuck-flee. Give-up clause (d) in builder.decide now covers
  -- that case directly -- it abandons the spot after PLACE_REFUSE_GIVEUP_TICKS
  -- of consecutive refusals rather than waiting for the stuck detector to
  -- notice -- so firing is simply progress again.
  local fire_is_progress = fired_this_tick
  if fire_is_progress then
    -- Active firing is progress — reset the timer so a planted bot
    -- shooting defenders doesn't trip stuck-flee mid-take.
    state.stuck_for = 0
  end
  -- Track how long the CURRENT destination has been pursued. The stuck
  -- ESCALATION below keys on the physical counter (goal-agnostic, survives
  -- goal churn — a wedged tank whose goals flip every 30 ticks must still
  -- escape), but the dest BLACKLIST stamp must not: par1b bot3 t=8533
  -- accumulated 220 orbit ticks against base #14, re-targeted #13, and 3
  -- ticks later stamped #13 "unreachable" for 600 ticks on evidence gathered
  -- chasing a different base — sending a 0-armour tank on an 1100-tick walk.
  do
    local dk = state.goal.kind ~= "none" and state.goal.mx
               and U.mkey(state.goal.mx, state.goal.my) or nil
    if dk ~= state._stuck_dest_key then
      state._stuck_dest_key   = dk
      state._stuck_dest_since = state.tick or 0
    end
  end
  if cur_mx == state.last_mx and cur_my == state.last_my
     and state.goal.kind ~= "none"
     and not attack_at_standoff
     and not state.wall_clearing
     and not fire_is_progress then
    state.stuck_for = state.stuck_for + 1
    -- In water the tank turns at 0.25 brad/tick (a 90-deg turn alone is
    -- ~256 ticks) and drives 3-4 WU/tick, so the 150-tick same-tile test
    -- misfires on a tank that's legitimately turning toward its escape
    -- target — each misfire rotates the escape destination and restarts
    -- the slow turn, thrashing forever. Give water 450 ticks: a 90-deg
    -- turn plus 2-3 river tiles of driving fits inside it.
    local cur_tt = U.ttype(cur_mx, cur_my)
    local stuck_limit = (cur_tt == C.T_RIVER or cur_tt == C.T_DEEPSEA)
                        and 450 or 150
    if state.stuck_for > stuck_limit then
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
        -- Blacklist the dest ONLY when this dest itself has been pursued
        -- continuously long enough to blame it. The escalation clock is
        -- physical and survives goal churn (correct — a wedged tank must
        -- escape whatever its goals do), but "this dest is unreachable" is
        -- a conclusion about THIS dest, and 3 ticks of pursuit cannot
        -- support it (see the tracker above). The escape below still runs
        -- either way; only the 600-tick stamp is withheld.
        local _dest_ticks = now - (state._stuck_dest_since or 0)
        if _dest_ticks >= (C.STUCK_DEST_BLAME_TICKS or 100) then
          local bk = U.mkey(state.goal.mx, state.goal.my)
          U.set_blocked(state, bk, now + 600, "refuel_base_unreachable")
          if BRAIN_DEBUG_MODE then
            print(string.format(
              TAG .. " t=%d STUCK at (%d,%d) goal=%s dest=(%d,%d) -- blocking for 600t",
              now, cur_mx, cur_my, state.goal.kind, state.goal.mx, state.goal.my))
            print2(string.format(
              "STUCK_BLOCK t=%d pos=(%d,%d) goal=%s dest=(%d,%d)",
              now, cur_mx, cur_my, state.goal.kind, state.goal.mx, state.goal.my))
          end
        elseif BRAIN_DEBUG_MODE then
          print2(string.format(
            "STUCK_NOSTAMP t=%d pos=(%d,%d) goal=%s dest=(%d,%d) pursued=%dt < %dt — escaping without blacklisting",
            now, cur_mx, cur_my, state.goal.kind, state.goal.mx, state.goal.my,
            _dest_ticks, C.STUCK_DEST_BLAME_TICKS or 100))
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
        attack.clear_attack_goal(state, "stuck (nav)")
      end
      state.stuck_for = 0
    end
  else
    state.stuck_for = 0
  end
  state.last_mx = cur_mx
  state.last_my = cur_my

  local t_stuck1 = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  stuck-detection done %.2f ms", (t_stuck1 - t_stuck0) / 1000))
  end

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
  if BRAIN_PROFILE then
    opt(string.format("  blocked/banned sweep done %.2f ms (sweep=%s)",
      (t_mid - t_stuck1) / 1000, tostring(now % 200 == 0)))
  end
  -- mid covers from end-of-dij-sched (t_dij1) to here. Was using t4,
  -- which double-counted the dij sched block since that has its own
  -- main "dij sched done" emit. The unattributed remainder of mid
  -- (mid total minus stuck-detection minus blocked/banned) is the
  -- comms processing + respawn handling + stuck setup between t_dij1
  -- and t_stuck0. Dropped the "  comms+stuck done" sub-emit since it
  -- measured essentially the same span as mid itself.
  metrics.set("us_mid", t_mid - t_dij1)
  if BRAIN_PROFILE then
    opt(string.format("mid done %.2f ms", (t_mid - t_dij1) / 1000))
  end
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
    -- Always try to build a road under ourselves when drowning in river.
    -- NO tree reserve here — this is survival, not a luxury road; spend the
    -- last trees rather than sink (was + TREE_RESERVE, which would refuse a
    -- life-saving build at <6 trees).
    if info.man_status == C.LGM_INTANK and tank_tt == C.T_RIVER
       and info.trees >= C.ROAD_RIVER_COST then
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

    -- SEA-PILL HARVEST: we may be standing in water ON PURPOSE, mid-collect
    -- (the boat was just consumed picking a pill up and the next one is one
    -- tile away). Bailing to shore there abandons the trip and costs another
    -- mine + 21 trees + boat for the pills we came for — sea_pills_A did
    -- exactly that, three times. goals.sea_update owns the release: it clears
    -- state._sea_afloat as soon as the cluster is collected or the water is no
    -- longer safe, and normal escape resumes on the same tick.
    if not pf_routing and not state._sea_afloat then
      -- state/now let find_dry_land skip destinations the stuck handler
      -- blocked, so a wall-pinned escape target rotates instead of being
      -- re-picked every tick forever (5-8 min idles on river-maze maps).
      local dry_x, dry_y = PF.find_dry_land(cur_mx, cur_my, state, now)
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
      state.goal = { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
    end

    -- Build road under self when on slow terrain (swamp/rubble/crater).
    -- ROI: swamp traversal ~85 ticks vs ~44 ticks with road built → saves ~40 ticks/tile.
    -- LGM barely leaves the tank's tile so exposure is short; use LGM_DANGER_MED.
    -- Skip when in a boat: the tank doesn't need roads on water, and dispatching
    -- the LGM triggers pacing that slows the tank below disembark speed.
    local slow_tt = not info.inboat and C.ROAD_BUILD_TERRAIN[tank_tt] or nil
    if slow_tt and info.man_status == C.LGM_INTANK
       and info.trees >= slow_tt + builder.road_tree_reserve(state, world, info) then
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

    -- Portfolio-surplus test for carried-pill DROP overrides (antitank /
    -- emergency below). The strategic placement search already skips roles
    -- at/over target (goals.lua surplus-skip); the drops bypassed it and could
    -- dump a 3rd "back" pill while back was already 2/0. Reuse the SAME
    -- PP.counts/PP.targets math (and the pill-table viz) so a drop avoids
    -- overfilling a role. Counts/targets computed once per tick, lazily.
    local _pf_dc, _pf_dt
    local function drop_role_surplus(mx, my)
      if not _pf_dc then
        _pf_dc = PP.counts(world, now)
        _pf_dt = PP.targets(_pf_dc.back + _pf_dc.front + _pf_dc.aggro + 1)
      end
      local cat = PP.classify(mx, my, false, true)
      local t = _pf_dt[cat]
      return (t ~= nil and (_pf_dc[cat] or 0) >= t), cat
    end

    -- Blocked-tile test for the emergency drop. A spot the place_pill gate or
    -- the stuck detector gave up on is held in state.blocked. pick_goal
    -- enforces that on pool candidates and the strategic scan skips them, but
    -- the drop below elects a tile from U.is_placeable + danger alone, neither
    -- of which reads state.blocked — so without this it hands back the very
    -- tile that was just abandoned. That is an endless loop here: the search
    -- re-runs every tick with no cooldown, and because it re-picks the
    -- lowest-danger neighbour while shell stamps expire, the winner can flip
    -- between tiles and reset the consecutive-refusal counter, so the give-up
    -- rule may never reach its threshold at all. One hash lookup.
    local function drop_tile_blocked(mx, my)
      local bl = state.blocked
      if not bl then return false end
      local until_t = bl[U.mkey(mx, my)]
      return until_t ~= nil and now < until_t
    end

    -- The anti-tank opportunistic drop used to sit here: carrying a pill with
    -- an enemy tank within 12, drop one at the MIDPOINT toward it, vetoed only
    -- by drop_role_surplus, on a 200-tick cooldown. Deleted -- it was a
    -- near-duplicate of the offensive build (goals.lua), which fires on the
    -- same "enemy nearby, carrying a pill" situation with better rules, and
    -- both wrote goals of kind place_pill_strategic, so whichever ran first in
    -- the tick simply won.
    --
    -- Nothing was carried over. Its 12-tile range went because 8 is a
    -- pillbox's actual reach (PILLBOX_RANGE 2048 WU / 256), so a drop at 12
    -- placed a pill that could not engage the tank that triggered it until
    -- that tank closed 4 more tiles -- a range that only made sense next to
    -- the midpoint spot, which put the pill 6 tiles closer to the enemy. The
    -- cooldown went because offensive_build returns a POOL CANDIDATE rather
    -- than writing state.goal directly, so it goes through goal selection
    -- instead of pre-empting it; what bounds repeats is LGM_INTANK (one build
    -- per builder round trip) plus the spacing rule keeping them apart.
    -- drop_role_surplus as its only veto went in favour of the support-pill
    -- rule. See VULNERABILITY_AND_BUILDS_PLAN.md, "Offensive build".

    -- Emergency pill drop: about to die with carried pills — drop one to save it.
    -- aIndy checks: no incoming shells nearby, don't drop in front of tank,
    -- don't drop in path of shells, don't drop if no enemies.
    -- Trigger is now the panic condition, not an armour threshold of its own.
    -- The old gate was armour <= EMERGENCY_DROP_ARMOUR (5) plus at least one
    -- visible enemy, which is a second armour-threshold trigger running beside
    -- the one in goals.lua with different rules. The two scores decide this
    -- better. What survives here is the part panic has no equivalent for: the
    -- tile search below, and the state facts (builder aboard, not in a boat)
    -- that should_panic_build checks for us.
    local _ed_panic = C.EMERGENCY_DROP_ENABLED
                      and state.goal.kind ~= "pill_place"
                      and danger.should_panic_build(state, info)
    if _ed_panic then
      -- The incoming-shell refusal used to sit here: skip the drop when a
      -- hostile shell was within EMERGENCY_DROP_SHELL_SAFE_DIST (3 tiles),
      -- reasoning "don't send the LGM into fire". Removed -- that is the same
      -- 3-tile ring imdanger uses for its shells term, read the opposite way.
      -- Shells there are -15, one of the strongest reasons panic fires at all,
      -- so the refusal blocked exactly the case this design says must build:
      -- carrying pills, low armour, under fire. The builder walks one tile, so
      -- its exposure is small, and losing the pills to death is worse.
      --
      -- FUTURE ENHANCEMENT, not implemented: the check that would actually be
      -- correct is per-tile rather than blanket -- take the chosen drop tile,
      -- project every shell currently in the air along its trajectory, and
      -- refuse only if one of them crosses THAT tile within the number of
      -- ticks the builder needs to walk there. That refuses the genuinely
      -- doomed drop without refusing every drop made under fire.
      do
        -- Find safe drop position: behind the tank (away from threats)
        -- Don't drop in front of tank (LGM gets run over or blocks path)
        local tank_dir = info.direction
        local best_drop_mx, best_drop_my = nil, nil
        local best_drop_danger = math.huge
        -- Ranking bucket: (spacing class, surplus role) flattened to 1..6, so
        -- one comparison orders the whole preference chain
        --   clear/non-surplus .. clear/surplus .. diagonal/non-surplus .. down.
        -- A surplus-role tile is still taken when it is all that is left --
        -- saving the pill from death beats losing it entirely, even if it
        -- overfills a role, and the same goes for a crowded one.
        local best_bucket = math.huge
        local best_drop_cls, best_drop_surplus = nil, nil
        for d = 0, C.EMERGENCY_DROP_SEARCH_DIRS - 1 do
          local angle = d * (2 * math.pi / C.EMERGENCY_DROP_SEARCH_DIRS)
          local dx = math.floor(math.sin(angle) + 0.5)
          local dy = math.floor(-math.cos(angle) + 0.5)
          local px, py = cur_mx + dx, cur_my + dy
          -- drop_tile_blocked: never re-elect a tile the gate breaker or the
          -- stuck detector just abandoned (see the helper above) — this search
          -- runs every tick with no cooldown, so without it the park resumes
          -- immediately and the breaker loops forever.
          -- Forest is REFUSED here, not merely deprioritised. is_placeable
          -- permits it, but lgmCheckNewRequest (lgm.c:410) silently rewrites a
          -- pill request on forest into a TREE request: the builder chops,
          -- returns with wood, no pill is placed, and nothing tells the brain.
          -- guard_build_spot already rejects forest as needs_clearing; this
          -- search only ever checked is_placeable, so it was a second route
          -- into that silent failure.
          if U.in_map(px, py) and U.is_placeable(px, py, world)
             and U.ttype(px, py) ~= C.T_FOREST
             and not drop_tile_blocked(px, py) then
            -- Prefer tiles away from where we're heading (behind us)
            local aim_to_drop = U.aim_at(info.tankx, info.tanky, U.m2w(px), U.m2w(py))
            local angle_from_front = math.abs(U.adiff(tank_dir, aim_to_drop))
            -- Only consider tiles that aren't directly ahead. NOTE: 60 here is
            -- BRADS, not degrees -- U.adiff works in the 256-unit circle
            -- (util.lua), so this is about 84 degrees. The value is right; the
            -- old "60 degrees" comment was the part that was wrong. Do not
            -- "correct" it to 42.7.
            if angle_from_front > 60 then
              local d_danger = danger.danger_at(px, py, now, world)
              -- Ranked: spacing class > surplus role > min danger. Class first
              -- so repeated emergency drops stop landing in a clump; danger
              -- stays the selector WITHIN each class+role bucket, exactly as
              -- before. Nothing is rejected for spacing.
              local cls = builder.spacing_class(world, state, px, py)
              local surplus = drop_role_surplus(px, py)
              local bucket = (cls - 1) * 2 + (surplus and 2 or 1)
              if bucket < best_bucket
                 or (bucket == best_bucket and d_danger < best_drop_danger) then
                best_bucket = bucket
                best_drop_danger = d_danger
                best_drop_mx = px
                best_drop_my = py
                best_drop_cls = cls
                best_drop_surplus = surplus
              end
            end
          end
        end
        if best_drop_mx and BRAIN_DEBUG_MODE then
          print2(string.format(
            "SPACING t=%d class=%s spot=(%d,%d) surplus=%s danger=%.1f (emergency search)",
            now, builder.SPACE_NAME[best_drop_cls] or "?", best_drop_mx, best_drop_my,
            tostring(best_drop_surplus), best_drop_danger))
        end
        if best_drop_surplus then
          -- NOT "only surplus tiles were reachable" any more: under the bucket
          -- ordering a clear/surplus tile legitimately outranks a
          -- diagonal/non-surplus one, so this fires whenever the WINNER is
          -- surplus-role, not only when nothing else existed.
          print2(string.format("EMERGENCY_DROP t=%d best tile is surplus-role — dropping at (%d,%d) anyway to save the pill", now, best_drop_mx, best_drop_my))
        end
        if best_drop_mx then
          -- (place_urgency and the _urgency/_urg_* goal fields are gone with
          -- the LGM danger gate they used to raise -- builder.lua explains.
          -- This drop bypasses the placement pool, so nothing else stamps
          -- them either.)
          state.goal = {
            kind = "place_pill_strategic", mx = best_drop_mx, my = best_drop_my,
            wx = U.m2w(best_drop_mx), wy = U.m2w(best_drop_my),
            emergency = true,
            -- _place_forced is the field the rest of the brain actually
            -- reads (builder.set_mode's PLACE_EMERGENCY_MAX_DIST relaxation,
            -- and the pool's hysteresis / attack_tank exemptions). `emergency`
            -- above is write-only — nothing in the brain has ever read it — so
            -- this drop never got its dispatch relaxation. That mattered: the
            -- search below picks one of EMERGENCY_DROP_SEARCH_DIRS neighbours,
            -- and a DIAGONAL neighbour is Manhattan distance 2, which the
            -- pdist<=1 test rejects outright. Half the candidate spots could
            -- never dispatch. Keeping `emergency` for anything outside the
            -- brain that may inspect the goal.
            _place_forced = true,
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
    local t_gv0 = BRAIN_PROFILE and clock_us() or 0
    local goal_valid = true
    local gk = state.goal.kind
    local gmx, gmy = state.goal.mx, state.goal.my
    local t_gv1 = BRAIN_PROFILE and clock_us() or 0
    if gk == "capture_base" then
      local b = W.base_at(world, gmx, gmy)
      -- Accept neutral (normal capture) and hostile (weakened base drive-over capture)
      if not b or (b.owner ~= "neutral" and b.owner ~= "hostile") then
        -- Base captured — replan immediately (refuel will win if supplies are low)
        if b and b.owner == "friendly" and BRAIN_DEBUG_MODE then
          print(string.format(TAG .. " t=%d BASE CAPTURED: (%d,%d) — replanning", now, gmx, gmy))
        end
        goal_valid = false
      elseif b.owner == "hostile" and (b.health or 0) > 0 then
        -- Hostile base no longer capturable. Bases restock over time, so a base
        -- we knocked down can climb back over MIN_ARMOUR_CAPTURE while we drive
        -- in — and the engine then BLOCKS the drive-over (observed: tank pinned
        -- beside the base at full throttle, spinning in place). Brain-visible
        -- base health is the engine's collapsed capturable flag (0 = capturable,
        -- 1 = not), so health > 0 is exactly "the drive-over will not work".
        -- Flip back to attack_base: its nav parks on the cheapest ADJACENT tile
        -- (usually where we already are — no new nav goal), faces the base and
        -- shoots; the CAPTURABLE switch below flips us back to capture_base for
        -- the drive-over once armour is down again.
        if BRAIN_DEBUG_MODE then
          print(string.format(TAG .. " t=%d BASE RESTOCKED: (%d,%d) hp=%d not capturable — switching back to attack_base", now, gmx, gmy, b.health or 0))
        end
        log.event("base_restocked", string.format("(%d,%d) hp=%d", gmx, gmy, b.health or 0))
        state.goal.kind = "attack_base"
        state.goal.race_mode = nil
        state.pf.status = "idle"
      end
    elseif gk == "attack_base" then
      local b = W.base_at(world, gmx, gmy)
      if not b then
        goal_valid = false
      elseif b.owner == "neutral" or (b.owner == "hostile" and (b.health or 0) == 0) then
        -- Armour depleted to 0 → CAPTURABLE. A base stays HOSTILE-owned at 0
        -- armour (it only becomes ours after we drive over it); a neutral base
        -- is capturable at any armour. Either way: stop shooting, drive over to
        -- capture. Same condition eval_capture_base uses, so the urgent discount
        -- below keeps us locked onto this exact base.
        if BRAIN_DEBUG_MODE then
          print(string.format(TAG .. " t=%d BASE CAPTURABLE: (%d,%d) owner=%s hp=%d — switching to capture_base",
                now, gmx, gmy, b.owner, b.health or 0))
        end
        log.event("base_capturable", string.format("(%d,%d) owner=%s hp=%d", gmx, gmy, b.owner, b.health or 0))
        state.goal.kind = "capture_base"
        state.goal.race_mode = true
        state.pf.status = "idle"
        state.urgent_capture_base = { mx = gmx, my = gmy, tick = now }
      elseif b.owner ~= "hostile" then
        -- Base became friendly/ally (someone else captured it)
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
          -- Abort if our LGM is dead: a reposition shoots the pill down to 0
          -- then needs the LGM to pick it up. With no LGM that just leaves our
          -- own pill destroyed. (LGM_MOVING is fine — that's the LGM out doing
          -- the pickup; only LGM_DEAD means we can't finish.)
          if not lp or lp.owner ~= "friendly" or (lp.health or 0) <= 0
             or (info.shells or 0) <= 0
             or info.man_status == C.LGM_DEAD then
            goal_valid = false
          end
        else
          goal_valid = false
        end
      end
    elseif gk == "attack_pill" and not state.capture_objective then
      -- Autonomous attack (not cp command): invalid if pill died or changed side
      -- BUT NOT during swerve — swerve must complete to dodge damage,
      -- the substate machine handles pill death after swerve finishes.
      -- ALSO not when OUR target just died mid-fire: the firing substate machine
      -- transitions firing -> swerve to dodge the still-in-flight return fire, so
      -- replanning here would abandon the swerve and eat those shells.
      local sub = state.goal.substate
      local FIRING_SUBS = {
        charge = true, shoot_pill = true, engage = true,
        in_range_aim = true, in_range_aim_finetune = true,
      }
      if sub ~= "swerve" then
        local p = W.pill_at(world, gmx, gmy)
        local pill_dead = (not p) or p.health == 0  -- killed/gone (may have return fire in flight)
        if p and p.owner == "friendly" then
          -- Captured (now ours): stop, nothing to dodge.
          goal_valid = false
        elseif pill_dead and FIRING_SUBS[sub] then
          -- Keep the goal valid — attack.lua's firing handler enters swerve this
          -- tick (charge:4462 / shoot_pill:4977 / engage). Don't invalidate.
          -- (A pill_suicider never swerves: those same three sites instead run
          -- the post-kill capture handoff this tick, which installs a
          -- capture_pill goal on the body. Either way the goal must survive
          -- this check so the firing handler gets to run.)
          -- Our shots just dropped this pill to 0 → claim the fresh-kill pickup
          -- so Override 3b grabs the body HARD (ignoring refuel/flee).
          attack.mark_kill_pickup(state, state.goal.target_id, gmx, gmy, now)
        elseif pill_dead then
          goal_valid = false
          -- Our attack_pill target died (we/our blitz finished it). Claim the
          -- fresh-kill pickup so Override 3b force-wins capture_pill on it
          -- instead of letting refuel/flee pull us off the free body.
          attack.mark_kill_pickup(state, state.goal.target_id, gmx, gmy, now)
          -- Wipe the attack_pill pool cache so the urgent replan can't
          -- immediately re-select a stale pill from old cost_cache data. The
          -- eval queue re-evaluates fresh candidates within the next replan.
          if state.pool_cache then state.pool_cache[6] = nil end
        elseif (info.shells or 0) <= 0 then
          -- Out of ammo: we can't damage the pill, so don't sit on it.
          -- Drop and replan (refuel is urgent at 0 shells). Clear pool 6
          -- so we don't immediately re-select it from stale cache.
          -- EXCEPT an ammo-deprived decoy that is IN A BLITZ for this pill: it
          -- charges the pill WITHOUT ammo by design (draw fire for the captain),
          -- so while the blitz is live 0 shells must NOT invalidate the take.
          -- Once the blitz closes (no accepted role / no open call for this
          -- pill) the exemption lapses and it drops the pill like anyone dry —
          -- a lone deprived tank must never sit on a pill solo.
          local decoy_in_blitz = false
          if state.ammo_deprived then
            local pid = state.goal.target_id
            if state.squad_blitz_accepted or (pid and state.squad_blitz_target == pid) then
              decoy_in_blitz = true
            elseif pid and state.blitz_calls then
              for _, bc in pairs(state.blitz_calls) do
                if bc.pill == pid then decoy_in_blitz = true; break end
              end
            end
          end
          if not decoy_in_blitz then
            goal_valid = false
            if state.pool_cache then state.pool_cache[6] = nil end
          end
        end
      end
    elseif gk == "pill_place" and not state.capture_objective then
      -- Pill placement: invalid if target died or became friendly
      local p = W.pill_at(world, gmx, gmy)
      if not p or p.owner == "friendly" or p.health == 0 then goal_valid = false end
    elseif gk == "defend_pill" then
      -- Only definitional invalidation: pill gone, no longer the team's
      -- (own or allied), or dead. Attack state plays no part — the pool
      -- scores every built team pill and replans re-compete naturally.
      -- A WATCH win (heat blocked at the arrival radius) holds on a tile
      -- NEXT TO the pill, so goal.mx/my is not the pill's tile; goal.pill_mx/
      -- pill_my carries the real target and is what gets validated. Without
      -- this the watch goal would be invalid on the very tick it was chosen.
      local pmx = state.goal.pill_mx or gmx
      local pmy = state.goal.pill_my or gmy
      local p = W.pill_at(world, pmx, pmy)
      if not p or (p.owner ~= "friendly" and p.owner ~= "allied")
         or p.health == 0 then goal_valid = false end
      -- ALARM MODE (2026-09-06, C.DEFEND_ALARM_MODE): the alarm is a
      -- PRECONDITION, not a bid.  The tick any of its conditions stops
      -- holding -- no hostile tank visible within 11 tiles of the pill RIGHT
      -- NOW, no enemy damage/build trigger inside the 5 s window, we have
      -- closed to within 9 tiles, or enough allies have arrived that the pill
      -- is now WELL DEFENDED -- the goal dies HERE, with no hysteresis,
      -- no commitment and no grace period, and the invalid-goal path below
      -- forces the immediate replan.  Asked through the SAME
      -- goals.defend_alarm_status the pool row prints its reject reason from,
      -- so the panel and the drop can never be different statements.
      if goal_valid and C.DEFEND_ALARM_MODE and p then
        local _al_on, _al_why = goals.defend_alarm_status(
          state, world, info, p, now,
          bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8))
        if not _al_on then
          goal_valid = false
          print2(string.format(
            "DEFEND_ALARM_OFF t=%d pill@(%d,%d) reason=%s"
            .. " -- alarm condition lapsed, dropping the defend goal",
            now, pmx, pmy, tostring(_al_why)))
        end
      end
    elseif gk == "repair_pill" then
      local p = W.pill_at(world, gmx, gmy)
      -- Abort if our LGM is dead — no one to do the repair (the eval already
      -- requires LGM_INTANK to START; this drops a committed goal the moment
      -- the LGM is killed, so we don't sit on a pill we can't fix).
      if not p or p.owner ~= "friendly" or p.health >= C.PILLS_MAX_HEALTH
         or info.man_status == C.LGM_DEAD then goal_valid = false end
    elseif gk == "place_pill_strategic" and state.goal.follow_through then
      -- FOLLOW_THROUGH hold: goal.mx/my is a tile for the TANK to wait on, not
      -- a drop spot, so the "a friendly pill is already there" test below would
      -- be asking about the wrong tile (and would thrash the goal if the hold
      -- picked a neighbour with a pill on it). It is valid for exactly as long
      -- as the harvest trip it follows -- the lifecycle block above already
      -- cleared _place_trip on the return, so this drops the goal on the same
      -- tick the resume dispatch takes over.
      if not (state._place_trip and state._place_trip.harvest) then
        goal_valid = false
      end
    elseif gk == "place_pill_strategic" then
      -- Invalid if we no longer carry a pill, or pill was placed at target
      if (info.carried_pills or 0) == 0 and info.man_status == C.LGM_INTANK then
        goal_valid = false
      else
        local p = W.pill_at(world, gmx, gmy)
        if p and p.owner == "friendly" and p.health > 0 then goal_valid = false end
      end
    elseif gk == "take_cover" then
      -- The only thing that can invalidate a cover tile is the tile itself:
      -- terrain changed under it (a wall went up, a crater flooded) so it is
      -- no longer somewhere a tank can stand. Whether it is still the SAFEST
      -- tile is the pool's question, re-asked every replan — and the LGM
      -- APPEARED urgent replan already re-runs the pool the moment the
      -- builder is back, which is when place/panic should take over.
      local tt = U.ttype(gmx, gmy)
      if tt == C.T_DEEPSEA or tt == C.T_BUILDING or tt == C.T_HALFBUILD
         or (tt == C.T_RIVER and not info.inboat) then
        goal_valid = false
        if state.pool_cache then state.pool_cache[14] = nil end
        state._cover_spot = nil
        state._cover_scan = nil
      end
    elseif gk == "attack_tank" and state.goal.km_ally_pn then
      -- "KILL ME" DELIVERY: the subject is an ALLY, so every test below (a
      -- visible ENEMY tank, the crossfire disengage, the outgunned bail) asks
      -- the wrong question. The goal is valid for exactly as long as that ally
      -- is still asking: its token is still on the slate, and it is still
      -- alive. Both are read straight off the slate/perception, so the goal
      -- ends on the tick the request does.
      local _pn = state.goal.km_ally_pn
      local _still = false
      for _, rq in ipairs(goals.kill_me_requests(state, info, now)) do
        if rq.pn == _pn then
          _still = true
          -- Track the ally as it moves: the parked tile is a promise, not a
          -- guarantee, and the fight loop steers at goal.mx/my.
          state.goal.mx, state.goal.my = rq.mx, rq.my
          state.goal.wx, state.goal.wy = U.m2w(rq.mx), U.m2w(rq.my)
          break
        end
      end
      if not _still then
        goal_valid = false
        if state.pool_cache then state.pool_cache[9] = nil end
        print2(string.format("KILL_ME_DROP t=%d responder: p%d is no longer advertising", now, _pn))
      end
      -- Our own man leaving the tank ends it too: the pills we are about to
      -- create would be as unplaceable for us as they are for the ally.
      if info.man_status ~= C.LGM_INTANK then
        goal_valid = false
        if state.pool_cache then state.pool_cache[9] = nil end
      end
    elseif gk == "kill_me_wait" then
      -- INITIATOR hold. The whole goal is "stand here and advertise", so the
      -- only things that can end it are the things that make the advert
      -- wrong: the state clearing (builder back, or the stack gone), an enemy
      -- tank arriving inside KILL_ME_CANCEL_ENEMY_TILES, or the master knob
      -- being off. The token rides the goal, so dropping it here is what takes
      -- the request off every ally's slate on the next heartbeat.
      if not C.KILL_ME_ENABLED or not state.loaded_no_lgm then
        goal_valid = false
      else
        local nh = state.perc and state.perc.nearest_hostile_tank
        if nh and (nh.dist or 1e9) <= (C.KILL_ME_CANCEL_ENEMY_TILES or 10) then
          goal_valid = false
          print2(string.format("KILL_ME_CANCEL t=%d enemy tank %d tiles away (<= %d) — request withdrawn",
            now, nh.dist or -1, C.KILL_ME_CANCEL_ENEMY_TILES or 10))
        end
      end
      if not goal_valid and state.pool_cache then state.pool_cache[15] = nil end
    elseif gk == "attack_tank" then
      -- Invalid if no enemy tanks visible (target escaped) or we're too weak.
      -- A GHOST of our specific target keeps the goal alive so we hunt it to
      -- the predicted position for GHOST_TANK_TTL_TICKS instead of giving up
      -- the moment it ducks into a forest.
      local has_target = false
      if state.perc and state.perc.enemy_tanks and #state.perc.enemy_tanks > 0 then
        has_target = true
      elseif state.perc and state.perc.ghost_tanks then
        for _, gt in ipairs(state.perc.ghost_tanks) do
          if gt.id == state.goal.target_id then has_target = true; break end
        end
      end
      if not has_target then goal_valid = false end
      -- Pillbox-crossfire disengage (every phase): if we're standing in heavy
      -- enemy pill danger, drop the tank goal so we replan toward safety
      -- instead of trading armour into a pillbox-defended position.
      if threat.pill_at(bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)) >= C.TANK_COMBAT_DEFENDED_DANGER then
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
    elseif gk == "kill_lgm" then
      -- Target LGM gone for good: the evaluator stops producing a
      -- kill_lgm candidate instantly (it drops off the winners pool) but
      -- the COMMITTED goal had no exit of its own — the bot kept chasing
      -- a nonexistent LGM until the next replan (20260714_093243_1 bot1
      -- t=7010-7044). Track the target's last seen tile on the goal; when
      -- it vanishes from perception, two terminal cases end the hunt NOW:
      --   * BOARDED — a visible enemy tank at/next to the last spot: he
      --     got in.
      --   * KILLED — a shell landed on him: perception records the death
      --     as a fresh hostile OBJECT_PARACHUTE sighting near that spot
      --     (state._enemy_lgm_sightings), whether we or a pill shot him.
      -- A plain LOS flicker (forest / fog) matches neither and leaves the
      -- hunt alone.
      local tid = state.goal.target_id
      local elm = nil
      if tid and state.perc and state.perc.enemy_lgms then
        for _, e in ipairs(state.perc.enemy_lgms) do
          if e.idnum == tid then elm = e break end
        end
      end
      if elm then
        state.goal._kl_last_mx = elm.mx
        state.goal._kl_last_my = elm.my
      elseif state.goal._kl_last_mx then
        local lmx, lmy = state.goal._kl_last_mx, state.goal._kl_last_my
        if state.perc and state.perc.enemy_tanks then
          for _, et in ipairs(state.perc.enemy_tanks) do
            if math.max(math.abs(et.mx - lmx), math.abs(et.my - lmy)) <= 1 then
              goal_valid = false
              if BRAIN_DEBUG_MODE then print2(string.format("KL_TARGET_BOARDED t=%d lgm#%s last=(%d,%d) tank@(%d,%d) — kill_lgm invalid, replanning", now, tostring(tid), lmx, lmy, et.mx, et.my)) end
              break
            end
          end
        end
        if goal_valid and state._enemy_lgm_sightings then
          -- Fresh-parachute window: the sighting is stamped the tick the
          -- LGM dies; anything older is a previous life's death marker
          -- lingering in the ENEMY_LGM_RETURN_TICKS bookkeeping.
          for _, s in pairs(state._enemy_lgm_sightings) do
            if now - s.tick <= 30
               and math.max(math.abs(s.mx - lmx), math.abs(s.my - lmy)) <= 2 then
              goal_valid = false
              if BRAIN_DEBUG_MODE then print2(string.format("KL_TARGET_KILLED t=%d lgm#%s last=(%d,%d) parachute@(%d,%d) age=%d — kill_lgm invalid, replanning", now, tostring(tid), lmx, lmy, s.mx, s.my, now - s.tick)) end
              break
            end
          end
        end
      end
    elseif gk == "refuel_at_base" or gk == "flee_to_base" then
      local b = W.base_at(world, gmx, gmy)
      if not b or (b.owner ~= "friendly" and b.owner ~= "neutral") then goal_valid = false end
      -- Committed refuel target went depleted (or otherwise rejected) since it
      -- was picked: the planner re-flags it every cycle via cost_cache._reject
      -- (that's what dims it "depleted" in the pool grid), but the COMMITTED
      -- goal keeps driving there until the next timer replan — so the HUD shows
      -- refuel_at_base@N while the grid shows N rejected. Abandon now; the
      -- urgent replan picks the next-best base (the reject keeps finalize from
      -- re-choosing this one). refuel only — a flee target under fire is still
      -- better than standing in the open.
      if goal_valid and gk == "refuel_at_base" then
        local tid = state.goal.target_id
        local ce  = tid and state.cost_cache and state.cost_cache["1:" .. tostring(tid)]
        if ce and ce._reject then
          goal_valid = false
          -- Depleted specifically: block the tile for a cooldown so finalize
          -- can't immediately re-pick this base once its depleted observation
          -- ages out (REFUEL_OBS_STALE) — that's the drive-there / see-depleted
          -- / leave flip-flop. Other reject reasons (ally_claimed steal band,
          -- transient block) carry their own re-acquire logic, so don't pile on.
          if ce._reject == "depleted" then
            U.set_blocked(state, U.mkey(gmx, gmy), now + C.REFUEL_DEPLETED_COOLDOWN, "refuel_target_depleted")
          end
          if BRAIN_DEBUG_MODE then print2(string.format("REFUEL_GOAL_REJECT t=%d base#%s @(%d,%d) now _reject=%s — abandoning committed refuel for replan", now, tostring(tid), gmx, gmy, tostring(ce._reject))) end
        end
      end
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
      -- A goal=none always forces an immediate (urgent) full replan — see the
      -- urgent_replan block below. The earlier "non-urgent" optimization for
      -- capture_base/attack_pill churn (let goal=none ride the timer) was
      -- removed: it made the bot idle on a base for up to GOAL_REPLAN_INTERVAL,
      -- and a stale-cache re-pick variant could lock onto a bad goal (e.g.
      -- charge attack_base at low armour because refuel wasn't in the stale
      -- cache). finalize_pools is cheap enough to just run on the churn tick.
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
    if BRAIN_PROFILE then
      opt(string.format("  goal-validation chain done %.2f ms (gk=%s)", (t_goal0 - t_gv0) / 1000, tostring(gk)))
    end
    if BRAIN_PROFILE then
      opt(string.format("water+goal_invalid done %.2f ms", (t_goal0 - t_water0) / 1000))
    end
    -- PROFILING LITE checkpoint 1/3 — "prelude" ends here: everything before
    -- goal selection (world/danger/threat/percept, the incremental Dijkstra
    -- scheduler, mid, water + goal-validation). See the NEAR_BUDGET block at
    -- the main think exit for the stage definitions.
    if BRAIN_DEBUG_MODE then state._plite_cp_prelude = print2.elapsed_ms() end

    -- Rolling candidate evaluation: 2 A* cost_to calls per tick
    -- Skip on ticks where threat grid rebuilt (both are expensive, don't stack)
    local t_pc0 = BRAIN_PROFILE and clock_us() or 0
    if not threat.rebuilt_this_tick then
      goals.update_pool_cache(state, world, info)
    end
    if BRAIN_PROFILE then
      opt(string.format("  update_pool_cache done %.2f ms", (clock_us() - t_pc0) / 1000))
    end

    -- Opening-phase base freshness: ONE TICK BEFORE every timer replan, force a
    -- strict-A* rescore of bases within OPENING_BASE_RESCORE_TILES so the
    -- imminent decision ranks them on real costs. The incremental base eval is
    -- dijkstra-only, so during the opening cold-start a nearby base sits at INF
    -- until the surface reaches it — this makes it selectable now. Runs after
    -- update_pool_cache so it overwrites this cycle's accumulated partial that
    -- finalize_pools reads at the replan next tick.
    if state.phase == "opening" then
      local _nt = (state.tick or 0) + 1 + (state.replan_offset or 0)
      if _nt % C.GOAL_REPLAN_INTERVAL == 0 then
        goals.rescore_nearby_bases(state, world, info, C.OPENING_BASE_RESCORE_TILES or 10)
      end
    end

    -- Per-tick ally-claimed REJECT sync: maintains entry._reject on
    -- cost_cache against the live ally_state slate so the pool grid and
    -- any selection that consults cost_cache between replans see fresh
    -- yield-decisions. Cheap (hash lookups per cached candidate). Pass
    -- info so the armour_too_low branch sees current armour (state.
    -- _last_info isn't populated outside the panel builder).
    if goals.sync_ally_claimed_rejects then
      goals.sync_ally_claimed_rejects(state, info)
    end

    -- ── attack_pill steal negotiation (stq/sta/str) ──────────────────────
    -- Companion to sync_ally_claimed_rejects' pool-6 rules. Three duties:
    --   1. execute a REAL yield the sync decided (dual-hold loser),
    --   2. consume replies to our own steal requests (grants / rejects),
    --   3. answer requests addressed to us: re-evaluate our score for the
    --      pill and reply sta (accept + yield) or str (reject). A holder
    --      past "approach" ALWAYS rejects — it's already getting into
    --      position, nobody can be closer in any way that matters.
    do
      local selfpn = info.player_number or 0
      -- 1. Dual-hold yield: clear the goal FOR REAL (the old reject-only
      --    yield left the goal driving — the zombie co-attacker bug).
      local ab = state._steal_abandon
      state._steal_abandon = nil
      if ab and state.goal and state.goal.kind == "attack_pill"
         and ((state.goal.target_id and state.goal.target_id == ab.pid)
              or (world.pills and world.pills[ab.pid]
                  and world.pills[ab.pid].mx == state.goal.mx
                  and world.pills[ab.pid].my == state.goal.my)) then
        state._steal_yielded = state._steal_yielded or {}
        state._steal_yielded[ab.pid] = { to = ab.to, tick = now }
        if BRAIN_DEBUG_MODE then print2(string.format("STEAL_YIELD t=%d pill=#%d to=p%d (%s)", now, ab.pid, ab.to, ab.why or "?")) end
        attack.clear_attack_goal(state, string.format(
          "steal: yield pill #%d to p%d (%s)", ab.pid, ab.to, ab.why or "?"))
      end
      -- 2. Replies to OUR requests.
      if state._steal_replies_in then
        for _, rp in ipairs(state._steal_replies_in) do
          local sent = state._steal_req_sent and state._steal_req_sent[rp.pid]
          if rp.to == selfpn and sent and sent.to == rp.from then
            if rp.accept then
              state._steal_grants = state._steal_grants or {}
              state._steal_grants[rp.pid] = { by = rp.from, cost = rp.cost, tick = now }
              -- Grab it promptly: force a replan instead of waiting out the timer.
              state._force_replan_reason = state._force_replan_reason or "steal_granted"
              if BRAIN_DEBUG_MODE then print2(string.format("STEAL_GRANTED t=%d pill=#%d by=p%d their_cost=%s", now, rp.pid, rp.from, tostring(rp.cost))) end
            else
              state._steal_rejects = state._steal_rejects or {}
              state._steal_rejects[rp.pid] = { by = rp.from, cost = rp.cost, tick = now }
              if BRAIN_DEBUG_MODE then print2(string.format("STEAL_REJECTED t=%d pill=#%d by=p%d their_cost=%s", now, rp.pid, rp.from, tostring(rp.cost))) end
            end
          end
        end
        state._steal_replies_in = nil
      end
      -- 3. Requests addressed to US. Reply from a FRESH re-evaluation: the
      --    rolling eval queue re-scores pool-6 candidates every ~15-30 ticks;
      --    if our cached score is older than STEAL_REEVAL_MAX_AGE we defer
      --    the reply until it refreshes (or STEAL_REPLY_DEADLINE forces one).
      if state._steal_reqs_in then
        local keep
        for _, rq in ipairs(state._steal_reqs_in) do
          if rq.to == selfpn then
            local g = state.goal
            local holds = g and g.kind == "attack_pill"
              and ((g.target_id and g.target_id == rq.pid)
                   or (world.pills and world.pills[rq.pid]
                       and world.pills[rq.pid].mx == g.mx
                       and world.pills[rq.pid].my == g.my))
            local our_sub = g and g.substate or ""
            local pre_commit = (our_sub == "plan_position" or our_sub == "approach")
            local ce = state.cost_cache and state.cost_cache["6:" .. tostring(rq.pid)]
            -- VARIANT (c): our reply price is the RAW cost_cache cost, the
            -- pre-handshake baseline. No competed total, no commitment
            -- adjustment. The units tag is kept on the log line (always `raw`)
            -- so the price a reply was made on is still stated outright.
            local raw_cost = ce and type(ce.cost) == "number" and ce.cost or nil
            local my_cost, cost_units = raw_cost, "raw"
            local cost_fresh = my_cost and (now - (ce.tick or 0)) <= (C.STEAL_REEVAL_MAX_AGE or 40)
            local deadline = (now - rq.tick) >= (C.STEAL_REPLY_DEADLINE or 20)
            local reply, verdict
            if not holds then
              -- Not on it (already left / never had it): grant freely so the
              -- challenger's pool entry unblocks without waiting for our
              -- claim broadcast to age out.
              reply, verdict = "sta", "not_holding"
            elseif not pre_commit or g._blitz then
              -- Past approach (already getting into position) or blitz-led:
              -- NEVER stealable.
              reply, verdict = "str", (g._blitz and "blitz_led" or "committed_past_approach")
            elseif not cost_fresh and not deadline then
              keep = keep or {}; keep[#keep + 1] = rq   -- wait for a fresh re-eval
            elseif my_cost and rq.cost
                   and rq.cost < my_cost * (1 - (C.ALLY_CLAIMED_STEAL_FRAC or 0.10)) then
              -- Challenger meaningfully cheaper than our re-evaluated score:
              -- accept and yield for real.
              state._steal_yielded = state._steal_yielded or {}
              state._steal_yielded[rq.pid] = { to = rq.from, tick = now }
              attack.clear_attack_goal(state, string.format(
                "steal: accepted p%d's request for pill #%d (%s < our %s %.0f)",
                rq.from, rq.pid, tostring(rq.cost), cost_units, my_cost))
              reply, verdict = "sta", "challenger_cheaper"
            else
              reply, verdict = "str", "we_re_cheap_enough"
            end
            if reply then
              state._steal_outbox = state._steal_outbox or {}
              state._steal_outbox[#state._steal_outbox + 1] =
                string.format("/info %s %d %d %d", reply, rq.pid, rq.from,
                              math.floor(math.min(my_cost or 9999999, 9999999) + 0.5))
              -- our_cost carries its UNITS. VARIANT (c) has one unit only --
              -- `(raw)`, the bare cost_cache cost -- and it is the number that
              -- goes out in the sta/str, so the challenger's band test sees
              -- exactly the price this line prints. raw= repeats it.
              if BRAIN_DEBUG_MODE then print2(string.format("STEAL_REPLY t=%d pill=#%d to=p%d %s [%s] our_cost=%s(%s) raw=%s their_cost=%s sub=%s", now, rq.pid, rq.from, reply, verdict, my_cost and string.format("%.1f", my_cost) or "nil", cost_units, raw_cost and string.format("%.1f", raw_cost) or "nil", tostring(rq.cost), our_sub)) end
            end
          end
        end
        state._steal_reqs_in = keep
      end
    end

    -- Team reposition coordination: track the last tick anyone (self or an
    -- ally) was repositioning a pill. While someone is, reset to now so the
    -- time-based reposition discount (eval_reposition_pill) is 0 and the team
    -- doesn't pile on; it then grows the longer it's been since the last move.
    do
      -- APPROVED/in-flight only — a capture_pill+reposition goal that is still
      -- just a BID must not reset the team discount (nor, via self_repos below,
      -- arm the per-bot cooldown). Same gate as the repos=1 broadcast.
      local repositioning = reposition_vote.is_reposition_active(state, world)
      -- Per-bot reposition cooldown: capture OUR OWN reposition state (before the
      -- ally-broadcast OR below folds teammates in) and stamp the tick it ENDS, so
      -- eval_reposition_pill can hold off starting another for a while.
      local self_repos = repositioning
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
      if state._self_was_repositioning and not self_repos then
        state._reposition_cooldown_tick = now   -- our reposition just ended → start cooldown
      end
      state._self_was_repositioning = self_repos
    end

    -- Self reposition repair-block: while OUR goal is moving a pill (and for
    -- REPAIR_REPOSITION_BLOCK_TICKS after it ends), never let repair_pill heal
    -- the pill we just shot down. Driven by the live goal EVERY tick — more
    -- reliable than reposition_steer's _demolish, which stops refreshing the
    -- instant the pill dies (substate→nil) and leaves a window when the goal
    -- frees to 'none' and a damaged-pill repair suddenly wins (the bug at
    -- bot2 t=15138). Stamp the goal tile AND the target pill's actual tile.
    if state.goal and state.goal.kind == "capture_pill" and state.goal.reposition then
      local rb = state._reposition_block or {}
      rb.tick  = now
      rb.tiles = {}
      if state.goal.mx and state.goal.my then
        rb.tiles[state.goal.my * C.MAP_W + state.goal.mx] = true
      end
      local tp = state.goal.target_id and world.pills[state.goal.target_id]
      if tp and tp.mx and tp.my then
        rb.tiles[tp.my * C.MAP_W + tp.mx] = true
      end
      state._reposition_block = rb
    end

    -- attack_base commit latch: once we put a shot INTO a base we're attacking,
    -- commit to finishing it. Detected by our shell count dropping while the
    -- executing goal is attack_base; refreshed on every shot so it stays live
    -- through the whole take, expiring ATTACK_BASE_COMMIT_TICKS after the last
    -- shot. The goal-override below holds the goal against everything but flee
    -- or a tank/LGM in shooting distance — so we finish the job instead of
    -- wandering off to a cheaper routine goal mid-bombardment.
    do
      local g = state.goal
      if g and g.kind == "attack_base" and g.mx
         and state._base_commit_prev_shells
         and (info.shells or 0) < state._base_commit_prev_shells then
        state._base_commit = { mx = g.mx, my = g.my, tick = now }
      end
      state._base_commit_prev_shells = info.shells or 0
    end

    -- Pill blocker / utility tracking: a friendly pill on a TILE serving an
    -- active pill take (a pre-existing firing-line pill, or a slot the take is
    -- building a blocker on) is a "blocker" → role utility until the take ends.
    -- Each bot computes its own blocker TILES + broadcasts them (bsi.pblk, packed
    -- my*256+mx); here we union the whole team's blocker tiles each tick and flag
    -- any friendly pill sitting on one _in_use so PP.role_of reports "utility".
    -- Tile-keyed (not pill-id) so a freshly-built blocker is protected the moment
    -- it appears on its tile, including its partial-health build-up. Rebuilt live,
    -- so a pill reverts to back/front/aggro as soon as the take stops broadcasting.
    local _ally_blk = nil   -- blocker TILES allies declared this tick (packed, for the yield check below)
    do
      local util = {}   -- packed-tile set; any friendly pill on one of these is _in_use
      local mine = attack.current_blocker_tiles and attack.current_blocker_tiles(state, world) or nil
      state._blocker_tiles = mine
      -- Shield WALL tiles we're building THIS tick (commander, build_walls only):
      -- broadcast separately (bwl) so blitz soldiers route their shot AROUND our
      -- fresh walls. Distinct from _blocker_tiles (pblk = friendly-pill repair
      -- protection) — these are the walls being laid, gated to build_walls.
      state._shield_wall_tiles = attack.current_shield_wall_tiles
        and attack.current_shield_wall_tiles(state, world) or nil
      if mine then for _, ti in ipairs(mine) do util[ti] = true end end
      if ally_state.iter_active then
        for pn, slot in ally_state.iter_active(now, 1750) do
          if pn ~= info.player_number and slot.info and slot.info.pblk then
            for s in string.gmatch(slot.info.pblk, "%d+") do
              local ti = tonumber(s)
              util[ti] = true
              _ally_blk = _ally_blk or {}; _ally_blk[ti] = true
            end
          end
        end
      end
      for _, p in pairs(world.pills) do
        p._in_use = util[p.my * 256 + p.mx] and true or nil
      end
    end

    -- Yield on an incoming blocker declaration: if our CURRENT goal is acting on a
    -- pill an ALLY just declared a blocker (a capture/reposition of it would move
    -- or consume it), drop it — block that pill in its pool(s) so we don't re-pick
    -- it, and clear the goal for a fresh choice. Even a committed/locked
    -- reposition bails. Only capture_pill targets a friendly pill this way; other
    -- goals (defend/repair) leave the pill where it is and are left alone.
    local _bid = state.goal and state.goal.target_id
    local _tp  = _bid and world.pills[_bid]
    local _gt  = _tp and (_tp.my * 256 + _tp.mx)
    if _ally_blk and state.goal and state.goal.kind == "capture_pill"
       and _gt and _ally_blk[_gt] then
      if state.cost_cache then
        for _, e in pairs(state.cost_cache) do
          if e._id == _bid then
            e._reject = "ally_blocker"
            e._reject_remaining = C.ALLY_BLOCKER_REJECT_TICKS or 150
            e.formula = nil
          end
        end
      end
      state._reposition_lock_tick = nil
      print2(string.format("BLOCKER_YIELD t=%d goal=capture_pill pill=#%s declared blocker by ally — reject pool + clear goal", now, tostring(_bid)))
      state.goal = { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
      state._force_replan_reason = "ally_blocker"
    end

    -- Unapproved reposition BID watchdog. steering refuses to demolish until the
    -- vote approves the pill, so a bid whose approval never lands parks in
    -- `approach` indefinitely. reposition_vote bounds it at WINDOW + margin and
    -- stamps the fail cooldown; we drop the goal here (before goal selection, so
    -- the replan lands this tick) exactly like the ally_blocker yield above.
    if reposition_vote.check_bid_timeout(state, world, now) then
      state._reposition_lock_tick = nil
      state.goal = { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
      state._force_replan_reason = "repo_bid_timeout"
    end

    -- blocker_pills viz: every _in_use (blocker/utility, reposition-protected)
    -- pill as a filled orange tile, labelled with where the flag came from
    -- ("me" = our own current_blocker_tiles, "pN" = ally N's pblk broadcast).
    -- Source is keyed by tile (packed my*256+mx), matching the broadcast.
    -- If a pill you expect protected isn't orange, nobody is declaring its tile:
    -- check the BLOCKER_VIZ print2 (mine={...}) and the ally's pblk.
    if BRAIN_DEBUG_MODE and viz.is_on("blocker_pills") then
      local _src = {}
      local function _add(ti, s) _src[ti] = _src[ti] and (_src[ti] .. "," .. s) or s end
      if state._blocker_tiles then for _, _ti in ipairs(state._blocker_tiles) do _add(_ti, "me") end end
      if ally_state.iter_active then
        for _pn, _slot in ally_state.iter_active(now, 1750) do
          if _pn ~= info.player_number and _slot.info and _slot.info.pblk then
            for _s in string.gmatch(_slot.info.pblk, "%d+") do _add(tonumber(_s), "p" .. _pn) end
          end
        end
      end
      local _n = 0
      for _, _p in pairs(world.pills) do
        if _p._in_use then
          _n = _n + 1
          viz.rect("blocker_pills", _p.mx, _p.my, _p.mx + 1, _p.my + 1, 255, 130, 0, 130, true)
          viz.text("blocker_pills", _p.mx + 0.5, _p.my - 0.4, "BLOCKER:" .. (_src[_p.my * 256 + _p.mx] or "?"), "center", 255, 180, 60, 255, 0.4)
        end
      end
      print2(string.format("BLOCKER_VIZ t=%d mine={%s} team_in_use=%d", now, state._blocker_tiles and table.concat(state._blocker_tiles, ",") or "-", _n))
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
    -- Panic-range crossing: while CARRYING a pill, an enemy tank ENTERING the
    -- offensive_build panic range (OFF_BUILD_THREAT_RANGE, euclidean) forces a replan so
    -- eval_place_pill_strategic's offensive_build branch can convert an in-flight
    -- strategic placement into a panic guard build. tank_appeared above keys off
    -- the WIDE perception set (fires when a tank is first SEEN, often far, and
    -- never re-fires as it closes), so it misses this tighter crossing.
    local panic_tank_in_range = false
    if (info.carried_pills or 0) >= 1 and state.perc and state.perc.enemy_tanks then
      local pr2 = (C.OFF_BUILD_THREAT_RANGE or 8) ^ 2
      for _, et in ipairs(state.perc.enemy_tanks) do
        local dx, dy = (et.mx or 0) - cur_mx, (et.my or 0) - cur_my
        if dx * dx + dy * dy <= pr2 then panic_tank_in_range = true; break end
      end
    end
    local panic_tank_appeared = panic_tank_in_range and not (state._prev_panic_tank_in_range or false)
    state._prev_panic_tank_in_range = panic_tank_in_range
    -- Urgent replan the first tick ANY enemy tank becomes newly visible — a
    -- tank id in perception that wasn't there last tick — regardless of the
    -- current goal. Tracks the full visible-id SET (not just a "any tank?"
    -- bool) so a 2nd tank arriving while one is already in view, or a tank
    -- that left and reappeared, also triggers. Real sightings only (ghosts are
    -- tracked separately and aren't "newly visible").
    local new_tank_seen = false
    do
      local prev = state._prev_visible_tank_ids
      local cur
      if state.perc and state.perc.enemy_tanks then
        for _, et in ipairs(state.perc.enemy_tanks) do
          if et.id then
            cur = cur or {}
            cur[et.id] = true
            -- Only "newly appeared" if we HAD a baseline and this id wasn't in
            -- it. A nil baseline (startup / just respawned) seeds without firing,
            -- so we don't spuriously urgent-replan on every already-visible tank.
            if prev and not prev[et.id] then new_tank_seen = true end
          end
        end
      end
      state._prev_visible_tank_ids = cur
    end
    -- Urgent replan when a tank WE CAN SEE dies. EVENT_TANK_KILLED carries no
    -- position/team, so we cross-reference the dead player against last tick's
    -- visible-tank snapshot (built from info.objects): only deaths of tanks we
    -- could actually see count. Any visible ENEMY death triggers; an ally death
    -- triggers only if it died within TANK_DEATH_REPLAN_ALLY_RANGE of us.
    local tank_died_seen = false
    do
      local snap = state._prev_tank_snapshot
      local dead = state.tank_dead_at
      if snap and dead then
        local ar = C.TANK_DEATH_REPLAN_ALLY_RANGE or 20
        -- An ally death only forces the (goal-clearing) replan when there's a
        -- DROPPED pill to grab: a dead, not-in-tank pillbox within PILL_RANGE
        -- (euclidean). Without that the interrupt just churned a live goal to
        -- none. Computed once (independent of which ally died). Enemy deaths are
        -- unaffected. A pill that drops a tick later still trips dead_pill_appeared.
        local pill_r  = C.TANK_DEATH_REPLAN_PILL_RANGE or 12
        local pill_r2 = pill_r * pill_r
        local dead_pill_near = false
        if world.pills then
          for _, p in pairs(world.pills) do
            if (p.health or 0) <= 0 and not p.in_tank then
              local dx, dy = p.mx - cur_mx, p.my - cur_my
              if dx * dx + dy * dy <= pill_r2 then dead_pill_near = true; break end
            end
          end
        end
        for pn, dtick in pairs(dead) do
          if dtick == now then            -- died this tick
            local s = snap[pn]            -- and was visible to us last tick
            if s then
              if s.hostile then
                tank_died_seen = true     -- any visible enemy death
              elseif dead_pill_near
                 and (math.abs(s.mx - cur_mx) + math.abs(s.my - cur_my)) <= ar then
                tank_died_seen = true     -- nearby ally death AND a dropped pill to grab
              end
            end
          end
        end
      end
      -- Rebuild snapshot (pos + team) of currently-visible tanks for next tick.
      local cur = {}
      if info.objects then
        for _, ob in ipairs(info.objects) do
          if ob.type == 0 and ob.idnum ~= nil then  -- OBJECT_TANK
            cur[ob.idnum] = { mx = bit.rshift(ob.x, 8), my = bit.rshift(ob.y, 8),
                              hostile = (bit.band(ob.info, OBJECT_HOSTILE)) ~= 0 }
          end
        end
      end
      state._prev_tank_snapshot = cur
    end
    -- Trigger a one-shot urgent replan the first tick a dead pill appears on
    -- the ground.  capture_pill is HYST_EXEMPT and costs ~0.1× path, so it
    -- almost always wins immediately; only a very close attack_tank beats it.
    local dead_pill_count = (state.perc and state.perc.dead_neutral_pill_count) or 0
    local dead_pill_appeared = dead_pill_count > 0
                            and dead_pill_count > (state.prev_dead_pill_count or 0)
                            and state.goal.kind ~= "capture_pill"
    state.prev_dead_pill_count = dead_pill_count
    -- Trigger a one-shot urgent replan the tick our carried-pill count
    -- INCREASES (drove over a dead pill — opportunistically or as a
    -- capture_pill completing). Several evaluators score differently
    -- while holding pills (cautious-mode danger multipliers,
    -- place_pill_strategic carry urgency, the carrying panic range), so
    -- re-task immediately with carry-aware scores instead of driving up
    -- to GOAL_REPLAN_INTERVAL on the pre-pickup plan.
    local carried_now = info.carried_pills or 0
    local pill_picked_up = carried_now > (state.prev_carried_pills or 0)
    state.prev_carried_pills = carried_now
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
    --
    -- EDGE-triggered, not level-triggered. As a level condition this fired a
    -- full replan EVERY tick for as long as any enemy stayed visible while we
    -- sat in plan_position — and a replan tick is the brain's most expensive
    -- shape, so on a loaded map it budget-killed every tick, re-selected the
    -- same goal, and livelocked (a kill unwinds the think, so the only exit is
    -- completing one; no capacity tier reaches the replan path to rescue it).
    -- Now it fires only when the enemy picture actually CHANGES: the count
    -- rose, a different tank became the nearest, or the nearest crossed INTO
    -- ATK_PREEMPT_NEAR_TILES — and at most once per ATK_PREEMPT_MIN_INTERVAL.
    local atk_pill_interruptible = false
    if state.goal.kind == "attack_pill"
       and (state.goal.substate == "disengage" or state.goal.substate == "plan_position")
       and state.perc and state.perc.enemy_tanks
       and #state.perc.enemy_tanks > 0 then
      local ets = state.perc.enemy_tanks
      local n_et = #ets
      local near_id, near_d = nil, math.huge
      for i = 1, n_et do
        local e = ets[i]
        local d = e.dist or math.huge     -- perception stores dist in TILES (U.mdist)
        if d < near_d then near_d, near_id = d, e.id end
      end
      local st  = state._atk_interrupt_stamp
      local thr = C.ATK_PREEMPT_NEAR_TILES or 8
      local changed = (st == nil)
                   or (n_et > (st.n or 0))
                   or (near_id ~= st.nearest_id)
                   or (near_d < thr and (st.nearest_d or math.huge) >= thr)
      if changed then
        -- Rate-limit. When the floor blocks a real edge we deliberately do NOT
        -- refresh the stamp, so the pending edge re-fires the moment it clears
        -- instead of being silently swallowed.
        if (now - (state._last_atk_preempt_tick or -1e9)) >= (C.ATK_PREEMPT_MIN_INTERVAL or 10) then
          atk_pill_interruptible = true
          state._last_atk_preempt_tick = now
          state._atk_interrupt_stamp = { tick = now, n = n_et,
                                         nearest_id = near_id, nearest_d = near_d }
        end
      else
        -- No edge: track the current picture so an approach that later crosses
        -- the threshold is seen as a crossing rather than a steady state.
        state._atk_interrupt_stamp = { tick = now, n = n_et,
                                       nearest_id = near_id, nearest_d = near_d }
      end
    else
      state._atk_interrupt_stamp = nil   -- left attack_pill (or no enemies): re-arm
    end

    -- goal=none always forces an immediate replan (else the bot idles up to a
    -- full GOAL_REPLAN_INTERVAL — the "sit on a base for a second" bug).
    -- Warmup exit: while on the explore fallback, replan the instant the pools
    -- warm up (>= WARMUP_MIN_REAL_GOALS finite candidates) instead of waiting
    -- out GOAL_MIN_COMMIT_TICKS. One-shot per explore episode (re-armed below
    -- whenever we're not on explore).
    if state.goal.kind ~= "explore" then state._warm_exit_done = nil end
    local warm_exit = state.goal.kind == "explore" and not state._warm_exit_done
                      and goals.warm_ready(state)
    if warm_exit then state._warm_exit_done = true end
    -- New blitz call received this/last tick → replan now so the join discount
    -- can compete immediately (doesn't force the join; just lets it be evaluated).
    local blitz_call_new = state._blitz_new_call or false
    state._blitz_new_call = nil
    -- Hard events are all edges or one-shots (a tank/base/LGM first appearing,
    -- a pill dying or being picked up, taking a hit, a blitz call): they can't
    -- repeat tick after tick, so they bypass the rate floor and replan now.
    local urgent_hard = warm_exit
                     or tank_appeared or new_tank_seen or dead_pill_appeared
                     or new_base_appeared or lgm_appeared
                     or shot_by_tank
                     or blitz_call_new or tank_died_seen
                     or panic_tank_appeared or pill_picked_up
    -- Soft reasons are level conditions that hold for as long as the world
    -- stays put: goal=none (which persists until a replan finds something) and
    -- the attack_pill tank preempt. Unbounded, either one turns every tick into
    -- a replan tick — the shape that budget-kills a loaded bot forever. Both
    -- tolerate a few ticks of delay, so they wait out REPLAN_MIN_INTERVAL.
    local urgent_soft = (state.goal.kind == "none") or atk_pill_interruptible
    local replan_floor_ok =
      (now - (state._last_full_replan_tick or -1e9)) >= (C.REPLAN_MIN_INTERVAL or 5)
    local urgent_replan = urgent_hard or (urgent_soft and replan_floor_ok)
    -- DIFFICULTY REACTION_DELAY_TICKS: hold the URGENT GOAL RE-DECISION for N
    -- ticks after its trigger fires, so an Easy/Medium bot reacts to new
    -- threats/opportunities a beat late. This only gates urgent_replan (the goal
    -- pool re-run); it does NOT touch force_replan / refuel_done (line ~6476) nor
    -- the safety layers -- cliff guards, swerve arming, drain-disengage and
    -- flee_to_base run every tick in steering / goal-invalidation, independent of
    -- this flag. Byte-inert at 0 (guard skips the whole block, no state write).
    -- A SECOND urgent trigger while a window is already pending is absorbed into
    -- that same window (the replan runs once, at the original deadline) rather
    -- than resetting the timer -- fine, and arguably the point: one delayed
    -- reaction per burst of triggers, not an ever-postponed one.
    if C.REACTION_DELAY_TICKS > 0 then
      if urgent_replan and not state._rxn_pending then
        -- First trigger: start the delay window and hold this tick's replan.
        state._rxn_pending = now + C.REACTION_DELAY_TICKS
        urgent_replan = false
      elseif state._rxn_pending then
        if now >= state._rxn_pending then
          state._rxn_pending = nil   -- window elapsed: run the delayed replan now
          urgent_replan = true
        else
          urgent_replan = false      -- still holding
        end
      end
    end
    if urgent_replan then
      -- Record which factor(s) tripped the urgent replan so the HUD
      -- below can flash a banner that's visible for a few seconds.
      -- Most-specific reason wins when more than one is true.
      local reason
      if lgm_appeared           then reason = "LGM APPEARED"
      elseif tank_appeared      then reason = "TANK APPEARED"
      elseif new_tank_seen      then reason = "TANK SEEN"
      elseif atk_pill_interruptible then reason = "TANK PREEMPT (pill loose)"
      elseif dead_pill_appeared then reason = "DEAD PILL"
      elseif pill_picked_up     then reason = "PILL PICKED UP (carrying)"
      elseif new_base_appeared  then reason = "BASE DISCOVERED"
      elseif tank_died_seen     then reason = "TANK DIED"
      elseif panic_tank_appeared then reason = "TANK IN PANIC RANGE (carrying pill)"
      elseif blitz_call_new     then reason = "BLITZ CALL"
      elseif attack_tank_done   then reason = "ATTACK_TANK DONE"
      elseif warm_exit          then reason = "WARMUP DONE"
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
    -- Set when refuel_complete is true but we're deliberately parked on the
    -- base waiting for our LGM to come home (see the release block below).
    local refuel_lgm_hold = false
    if state.goal.kind == "refuel_at_base" then
      -- Yield-to-starved-ally: once we are at/above BOTH COMBAT lines
      -- (30/30) and an ally below one of them is claiming THIS base, cap
      -- our targets at the COMBAT lines -- topping off to 40 while a
      -- starved teammate waits is hogging. Mines never hold us here
      -- anyway (REFUEL_MIN_MINES 0); they only fill while we sit, and
      -- this makes us sit less.
      local armour_target = state.armour_target
      local shell_target  = state.shell_target
      if info.armour >= (C.ARMOUR_COMBAT or 30)
         and info.shells >= (C.SHELLS_COMBAT or 30) then
        for ally_pn, slot in ally_state.iter_active(now, 1750) do
          if ally_pn ~= info.player_number then
            local h = slot.info
            if h and h.goal == "refuel_at_base" and h.low == "1" then
              local aid = tonumber(h.target)
              local amx, amy = tonumber(h.mx), tonumber(h.my)
              if (aid and state.goal.target_id and aid == state.goal.target_id)
                 or (amx and amx == state.goal.mx and amy == state.goal.my) then
                armour_target = C.ARMOUR_COMBAT or 30
                shell_target  = C.SHELLS_COMBAT or 30
                -- Read by both anti-base-hop sites in goals.lua: while this
                -- stamp is fresh the +500 stay-on-base penalty is waived, so
                -- the yield actually moves us off the base.
                state._refuel_yield_tick = now
                print2(string.format("REFUEL_YIELD t=%d base=(%d,%d) to p%d (low) -- targets capped at %d/%d",
                  now, state.goal.mx or -1, state.goal.my or -1, ally_pn,
                  armour_target, shell_target))
                break
              end
            end
          end
        end
      end
      local need_armour = info.armour < armour_target
      local need_shells = info.shells < shell_target
      -- Mines never hold the bot at base (REFUEL_MIN_MINES defaults 0). The
      -- mine-hoard surcharge in goals.lua handles "don't linger for mines".
      local need_mines  = info.mines < (C.REFUEL_MIN_MINES or 0)
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
        local our_mx = bit.rshift(info.tankx, 8)
        local our_my = bit.rshift(info.tanky, 8)
        local dist_cheb = math.max(math.abs(our_mx - state.goal.mx),
                                   math.abs(our_my - state.goal.my))
        local on_base_ourselves = (dist_cheb == 0)
        -- Always scan when refuel_at_base is active so the
        -- hud_refuel_ally_check overlay can show the live answer.
        -- Cheap (info.objects is short).
        local ally_on_base = false
        for _, ob in ipairs(info.objects) do
          if ob.type == 0   -- OBJECT_TANK
             and (bit.band(ob.info, 1)) == 0   -- not OBJECT_HOSTILE → ally (excludes self; self isn't in info.objects)
             and (bit.rshift(ob.x, 8)) == state.goal.mx
             and (bit.rshift(ob.y, 8)) == state.goal.my then
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
            U.set_blocked(state, bk, now + 200, "refuel_ally_camping")
            attack.clear_attack_goal(state, "refuel: ally camping base (wait timeout)")
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
      -- MUST be standing on the GOAL base: info.base is the engine's single
      -- in-range base (the one we're next to), which is NOT necessarily our refuel
      -- goal. Without this gate, parking next to a DIFFERENT (e.g. just-stolen,
      -- empty) base read that base's stock and wrongly blocked our actual target.
      if refuel_needed and info.base
         and (bit.rshift(info.tankx, 8)) == state.goal.mx and (bit.rshift(info.tanky, 8)) == state.goal.my then
        local LOW = 4
        local getting_something = false
        if need_armour and (info.base.armour or 0) >= LOW then getting_something = true end
        if need_shells and (info.base.shells or 0) >= LOW then getting_something = true end
        if need_mines  and (info.base.mines or 0) >= LOW then getting_something = true end
        -- Survival override: at/below ARMOUR_LOW, ANY armour left in the base
        -- is worth staying for — the LOW=4 "is this trip worth it" threshold is
        -- a convenience rule for a healthy tank, and applying it at 0 armour
        -- makes the bot walk away from the 3 armour that would let it survive.
        -- A base that is genuinely EMPTY of armour still releases us: waiting
        -- forever at a dry base is worse than replanning.
        if (info.armour or 99) <= C.ARMOUR_LOW and (info.base.armour or 0) > 0 then
          getting_something = true
        end
        if not getting_something then
          local bk = U.mkey(state.goal.mx, state.goal.my)
          U.set_blocked(state, bk, now + 200, "refuel_base_depleted")
          attack.clear_attack_goal(state, "refuel: base depleted")
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

      -- ── Refuel finished: RELEASE the goal, don't just flag a replan ──
      -- refuel_done below forces a replan every tick while refuel_at_base
      -- is still installed, but goals.lua's pool-1 shaping deliberately
      -- skips the refuel entry once we're at armour_target/shell_target
      -- ("at dynamic target: don't compete"). That leaves the competition
      -- with NO pool entry for the current goal, so the walkover guard in
      -- goal_competition resurrects it from pool_cache at its last
      -- finalized cost — which then beats every real contender, because
      -- challengers pay the switch + commitment penalties and the
      -- carried-forward incumbent pays nothing.
      --
      -- Result: a livelock. 20260828_111758 bot2 sat on base #13 at
      -- (143,115) with arm=40 for 114 straight ticks (t=2807-2920), every
      -- tick logging "refuel_done=true", "hysteresis(carry): current
      -- refuel_at_base@143,115 had no pool entry this cycle — carried at
      -- last cost 48" and "winner attack_pill cost=133 > current 48 * 0.7
      -- — sticking with current". Six such runs on that bot alone; the
      -- loop only broke when the base finally ran dry. Cost-tuning can't
      -- fix it (the incumbent's cost is whatever it last was, and the
      -- guard reinstates it unconditionally) — the goal has to stop being
      -- the incumbent. Same treatment the depleted-base branch above
      -- already gives its sibling case.
      --
      -- Stay-for-LGM exemption: goals.lua keeps refuel competing (clamped
      -- to the LGM_WAIT_COST floor) when we're parked ON the base and our
      -- LGM is still inbound — there, sitting still IS the job and "fully
      -- stocked" is not "finished". Releasing the goal in that case would
      -- just have the pool re-pick refuel next tick, trading one every-
      -- tick replan for another while also resetting goal_set_tick so
      -- commitment and cur_group hysteresis could never build up. Mirrors
      -- the lgm_wait_here test in goals.lua's pool-1 block; it also keeps
      -- build_eval_queue's "queue refuel while refuel_at_base is active"
      -- clause alive so pool 1 still has candidates to clamp.
      if refuel_complete then
        local on_goal_base = state.goal.mx and state.goal.my
          and (bit.rshift(info.tankx, 8)) == state.goal.mx
          and (bit.rshift(info.tanky, 8)) == state.goal.my
        local lgm_eta = state.builder and state.builder.lgm_eta
        local lgm_wait_here = on_goal_base and lgm_eta
          and lgm_eta > now + C.LGM_ETA_DEPART_BUFFER
        refuel_lgm_hold = lgm_wait_here and true or false
        if not lgm_wait_here then
          attack.clear_attack_goal(state, "refuel: complete (fully stocked)")
          if BRAIN_DEBUG_MODE then
            print2("REFUEL_RELEASE t=", now, " arm=", info.armour, "/", state.armour_target,
                   " sh=", info.shells, "/", state.shell_target,
                   " — fully stocked, releasing goal so the pool can pick freely")
          end
        elseif BRAIN_DEBUG_MODE then
          print2("REFUEL_RELEASE t=", now, " SUPPRESSED: staying for LGM (eta=",
                 lgm_eta, " buffer=", C.LGM_ETA_DEPART_BUFFER, ")")
        end
      end
    end
    -- Fully stocked at refuel target: replan immediately instead of
    -- waiting for the next timer fire.
    --
    -- NOT during a stay-for-LGM hold. There the goal is still live and
    -- correctly re-wins the pool every tick (at the LGM_WAIT_COST floor),
    -- so "done" would mean a full replan every tick for the whole wait
    -- with a foregone conclusion — 20260828_164532 bot2 t=4623-4692, 70
    -- straight ticks of replan=true → pick_goal → refuel_at_base while
    -- lgm_eta slid forward one tick at a time. Waiting is an ordinary
    -- committed goal, so let it ride the normal replan timer; urgent
    -- replans still interrupt it if something real happens.
    local refuel_done = refuel_complete and not refuel_lgm_hold
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
      -- Stamp BEFORE the work: a replan that overruns and gets budget-killed
      -- still counted as a replan attempt, and must not be free to retry next
      -- tick. (The kill unwinds everything after this point.)
      state._last_full_replan_tick = now
      if BRAIN_DEBUG_MODE then print2("ENTERING REPLAN") end
      -- Always use cached/partial data — never run the expensive
      -- fill_pool_cache.  The rolling queue refines over ~14 ticks.
      -- At tick 0 the cache is empty so pick_goal returns nil and
      -- the bot idles until the queue fills (~0.3s).
      --
      -- Queue build runs BEFORE finalize_pools: build_eval_queue sync-scores
      -- brand-new pool-4 corpses into cost_cache at add time, and finalize's
      -- backfill loop reconstitutes pool_partial from the (fresh) queue +
      -- cost_cache, so a pill that died since the last replan is visible to
      -- THIS tick's pick_goal. With the old finalize-first order the fresh
      -- corpse missed the decision by a full replan cycle (~50 ticks) —
      -- 20260703_210207 t=6997: bot stood 2 tiles from its freshly-dead
      -- capture target, replanned, and repair_pill won because pool 4 was
      -- empty; the LGM rebuilt the corpse and the free pill was lost.
      if age <= 3 then print(TAG .. " tick=" .. now .. " calling build_eval_queue") end
      if C.INCREMENTAL_REPLAN then
        -- Deferred mode still pays the one-cycle blindness for brand-new
        -- corpses; the spike-splitting tradeoff is explicit here.
        state._deferred_build_eval = true
      else
        local t_be0 = BRAIN_PROFILE and clock_us() or 0
        goals.build_eval_queue(state, world, info)
        if BRAIN_PROFILE then
          opt(string.format("  build_eval_queue done %.2f ms", (clock_us() - t_be0) / 1000))
        end
      end
      local t_fp0 = BRAIN_PROFILE and clock_us() or 0
      goals.finalize_pools(state, world, info)
      if BRAIN_PROFILE then
        opt(string.format("  finalize_pools done %.2f ms", (clock_us() - t_fp0) / 1000))
      end
      if age <= 3 then print(TAG .. " tick=" .. now .. " build_eval_queue done, calling pick_goal") end
      metrics.inc("goal_replan")
      local t_pg0 = BRAIN_PROFILE and clock_us() or 0
      local new_goal = goals.pick_goal(state, world, info)
      if BRAIN_PROFILE and state._pick_goal_timing then
        for _, entry in ipairs(state._pick_goal_timing) do opt(entry) end
      end
      if BRAIN_PROFILE then
        opt(string.format("  pick_goal done %.2f ms", (clock_us() - t_pg0) / 1000))
      end
      if BRAIN_DEBUG_MODE then print2("pick_goal -> ", new_goal and new_goal.kind or "nil",
             " mx=", new_goal and new_goal.mx, " sub=", new_goal and new_goal.substate) end
      -- Dump all pool_cache winners with costs for diagnosing goal switches.
      -- Tier-gated: these per-candidate dumps are ~1 ms of string.format +
      -- print2 on a replan tick, which is exactly the tick the capacity
      -- controller is trying to shrink. Below REPLAN_LOG_MIN_TIER the
      -- controller's low tiers have to be genuinely cheaper than its high
      -- ones or its floor is fiction, so the diagnostics go first.
      local _replan_log = BRAIN_DEBUG_MODE
        and (state._capacity_tier or 10) >= (C.REPLAN_LOG_MIN_TIER or 3)
      if _replan_log and state.pool_cache then
        for pi = 0, 15 do
          local pce = state.pool_cache[pi]
          if pce and pce.goal then
            -- cost= is the RAW cached pool cost: pre phase-weight, pre
            -- influence multiplier, pre refuel shape, pre hysteresis. What
            -- actually competed is the total goal_competition ended up with,
            -- and some rows never competed at all (blocked dest, goal
            -- blacklist, abandon cooldown, refuel already at target). Print
            -- both and name the exclusion, so a row on the panel can never
            -- look like a contender it never was.
            local competed = state._pool_competed and state._pool_competed[pi]
            local excl = state._pool_excluded and state._pool_excluded[pi]
            print2(string.format("  pool[%d] %s @(%d,%d) cost=%.1f %s",
              pi, pce.goal.kind or "?", pce.goal.mx or 0, pce.goal.my or 0,
              pce.cost or -1,
              competed and string.format("competed=%.1f", competed)
                or ("EXCLUDED: " .. tostring(excl or "not in this replan's pool"))))
          end
        end
      end
      -- Dump goal_competition entries (same tier gate as the pool dump above)
      if _replan_log and state.goal_competition then
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
      -- "Killing the pill right now" states — actively rushing / aiming /
      -- firing at the pill. Like charge, these must NOT be interrupted by a
      -- merely-cheaper goal; only the critical preempts below (flee /
      -- attack_tank / kill_lgm / dead-pill grab) break them. swerve is handled
      -- above (fully uninterruptible). The pre-fire positioning/setup states
      -- (plan_position, approach, detree, gather_trees, build_walls, blitz_wait,
      -- dispatch, disengage, post_engage, loiter, ws_* setup, ...) stay
      -- interruptible so high-priority goals can still preempt before the shot.
      local ksub = state.goal.substate
      local engage_locked = state.goal.kind == "attack_pill"
        and (ksub == "charge"        or ksub == "engage"
          or ksub == "shoot_pill"    or ksub == "aim"
          or ksub == "in_range_aim"  or ksub == "in_range_aim_pre"
          or ksub == "in_range_aim_finetune" or ksub == "in_range_position"
          or ksub == "kill_hardline" or ksub == "curve_away"
          or ksub == "ws_engage")
      -- A committed blitz must not be abandoned for routine goals (the bug
      -- where a discounted capture_pill/reposition stole a commander out of
      -- blitz_wait, stranding the squad). Holds through the pre-GO + aim phases
      -- (plan_position/approach/gather_trees/detree/build_walls/blitz_wait/
      -- aim/in_range). charge/engage are excluded — handled by engage_locked
      -- above, which also lets a dead-pill capture take over the final rush.
      -- Only genuinely critical goals preempt a blitz: survival (flee), an
      -- enemy LGM to grab/kill, or a hostile tank close enough to threaten us.
      local blitz_locked = state.goal.kind == "attack_pill" and state.goal._blitz
                           and state.goal.substate ~= "charge"
                           and state.goal.substate ~= "engage"
      if blitz_locked and state.goal.target_id then
        -- Release the hold if the target is gone/taken: dead pill -> let
        -- capture_pill grab it; friendly -> moot. Don't cling to a dead blitz.
        local tp = world.pills[state.goal.target_id]
        if not tp or tp.owner == "friendly" or (tp.health or 0) <= 0 then
          blitz_locked = false
        end
      end
      -- A "blitz" with nobody committed is just a solo take. goal._blitz only
      -- means the take was FLAGGED as a blitz at plan time (pill HP high) and
      -- we're broadcasting the call — NOT that a squad formed. A commander with
      -- zero committed soldiers has no squad to strand, so it must stay
      -- interruptible (respond to a near tank, emergency build, etc.) like any
      -- solo take. Only the commander is gated: a soldier's own ready count is
      -- always 0 (blitz_ready_status counts ITS followers), and a committed
      -- soldier should still hold its converge to the commander's pill.
      if blitz_locked and state.squad_role == squad.ROLE_COMMANDER then
        local joined = squad.blitz_ready_status(state, now, state.player_number, info)
        if (joined or 0) == 0 then blitz_locked = false end
      end
      -- Committed base attack: once a shot is in (latch above), finish the job.
      -- Holds the goal against all routine goals regardless of cost; releases
      -- only when the base is dead/captured/gone (let capture_base + the urgent
      -- capture discount take over) or the commit window has lapsed.
      local base_commit_locked = state.goal.kind == "attack_base"
                                 and state._base_commit
                                 and (now - state._base_commit.tick)
                                     < (C.ATTACK_BASE_COMMIT_TICKS or 500)
      if base_commit_locked then
        local bb = W.base_at(world, state.goal.mx, state.goal.my)
        if not bb or bb.owner ~= "hostile" or (bb.health or 0) <= 0 then
          base_commit_locked = false
        end
      end
      -- A replan only SWITCHES when the winner differs from the current goal.
      -- If the pool re-picks the SAME goal (same kind + tile + target), keep the
      -- current goal object untouched — a same-winner replan (urgent or timer)
      -- must be a no-op, never a churn that resets the substate/progress and
      -- interrupts e.g. an in-flight blitz/take.
      -- ...but "the same goal" has to include the ACTION for defend_pill. The
      -- same pill, held from the same tile, can be a WATCH one replan and a
      -- REPAIR the next, and the two drive completely different behaviour
      -- downstream (builder.decide dispatches the LGM on goal.repair; the heat
      -- substates key off goal.heat). Comparing kind+tile+target alone kept the
      -- stale watch object, so the repair rung could win the pool every replan
      -- and never reach the builder at all.
      local _defend_action_same = true
      if new_goal and new_goal.kind == "defend_pill"
         and state.goal.kind == "defend_pill" then
        _defend_action_same =
              ((new_goal.repair or false) == (state.goal.repair or false))
          and ((new_goal.heat   or false) == (state.goal.heat   or false))
          and ((new_goal.watch  or false) == (state.goal.watch  or false))
      end
      local same_winner = new_goal and new_goal.kind == state.goal.kind
        and new_goal.mx == state.goal.mx and new_goal.my == state.goal.my
        and (new_goal.target_id or -1) == (state.goal.target_id or -1)
        and _defend_action_same
      if same_winner then
        new_goal = state.goal  -- re-affirmed current goal: no switch
      elseif swerving then
        new_goal = state.goal  -- swerve is never interrupted, not even by flee
      elseif state.goal.kind == "kill_mine"
             and new_goal.kind ~= "flee_to_base" then
        -- De-mine interrupt in progress (demine.lua pushed it over the real
        -- goal, which is stashed on state._demine_saved). Hold: demine.update
        -- pops the moment the mine is cleared (timeout + out-of-shells
        -- backstops). Only survival preempts.
        new_goal = state.goal
      elseif engage_locked and new_goal.kind ~= "flee_to_base"
                             and new_goal.kind ~= "attack_tank"
                             and new_goal.kind ~= "kill_lgm"
                             and not (new_goal.kind == "capture_pill" and not new_goal.reposition) then
        -- Hold the charge. A capture_pill REPOSITION (moving our OWN pill) is
        -- never urgent and must not interrupt a charge — follow the take
        -- through. A non-reposition capture (dead/neutral-pill grab, e.g. the
        -- pill we're charging just died) still preempts, as do attack_tank,
        -- kill_lgm (LGM spike) and flee. Only the capture_pill exemption
        -- narrowed here.
        new_goal = state.goal  -- keep current goal
      elseif blitz_locked
             and new_goal.kind ~= "flee_to_base"
             and new_goal.kind ~= "kill_lgm"
             and not (new_goal.kind == "attack_tank"
                      and U.mdist(cur_mx, cur_my, new_goal.mx or 0, new_goal.my or 0)
                          <= (C.SQUAD_BLITZ_PREEMPT_TANK_TILES or 10)) then
        new_goal = state.goal  -- hold the blitz against non-critical goals
      elseif base_commit_locked
             and new_goal.kind ~= "flee_to_base"
             and not ((new_goal.kind == "attack_tank" or new_goal.kind == "kill_lgm")
                      and U.mdist(cur_mx, cur_my, new_goal.mx or 0, new_goal.my or 0)
                          <= (C.ATTACK_BASE_PREEMPT_SHOOT_TILES or 8)) then
        -- Finish the base: only survival or a tank/LGM in shooting distance
        -- breaks the commit; a far tank/LGM doesn't pull us off the base.
        new_goal = state.goal
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
             and new_goal.target_id ~= state.goal.target_id)
         -- Same reason as the same_winner test above, and it has to be repeated
         -- HERE: this is the gate that actually installs the object. A
         -- defend_pill whose ACTION changed (watch -> repair) has the same
         -- kind, tile and target, so without this clause the fresh goal is
         -- discarded and the stale one keeps its old flags -- the repair rung
         -- won the pool at every replan and builder.decide never saw
         -- goal.repair at all.
         or not _defend_action_same then
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
        attack.clear_attack_goal(state, "explore arrived/failed")
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
          U.set_blocked(state, bk, now + 600, "refuel_dest_block")
          print2(string.format(
            "PF_FAIL_BLOCK t=%d goal=%s dest=(%d,%d) pos=(%d,%d) 600t",
            now, state.goal.kind, state.goal.mx, state.goal.my, cur_mx, cur_my))
          attack.clear_attack_goal(state, "pathfind failed (dest blocked)")
          state.pf_fail_count = 0
        else
          -- Retry: reset pf to idle so it tries again next tick
          state.pf.status = "idle"
        end
      end
    end
  end

  -- PROFILING LITE checkpoint 2/3 — "replan" ends here: the replan decision
  -- plus (on replan ticks) build_eval_queue / finalize_pools / pick_goal /
  -- goal-swap, and the pf auto-expire chain. On non-replan ticks this stage
  -- is ~0. See the NEAR_BUDGET block at the main think exit.
  if BRAIN_DEBUG_MODE then state._plite_cp_replan = print2.elapsed_ms() end

  -- Per-tick attack substate machine (runs every tick, not just on replan)
  local t_as0 = BRAIN_PROFILE and clock_us() or 0
  attack.update_attack_substate(state.goal, state, world, info)
  local t_as1 = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  attack_substate done %.2f ms", (t_as1 - t_as0) / 1000))
  end
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
  if BRAIN_PROFILE then
    opt(string.format("goals done %.2f ms", (t_goal1 - t_goal0) / 1000))
  end
  -- PROFILING LITE checkpoint 3/3 — "handler" ends here: the per-tick goal
  -- handler (attack.update_attack_substate) and the attack aim-point solve.
  -- Everything after this (demine, lookahead, steering, builder, HUD, squad/
  -- comms, tail) lands in "rest". See the NEAR_BUDGET block at the main exit.
  if BRAIN_DEBUG_MODE then state._plite_cp_handler = print2.elapsed_ms() end

  -- Automatic de-mine interrupt: when a known mine is in crosshair range
  -- and the goal is interruptible, push a kill_mine goal over it (the real
  -- goal object is stashed with all context and restored on pop the moment
  -- the mine is cleared). See demine.lua; the arbitration chain above holds
  -- a pushed kill_mine against replans.
  -- SEA-PILL HARVEST exclusion. While capture_pill's deep-sea chain is live,
  -- demine's two jobs are actively destructive:
  --   * the kill_mine interrupt targets OUR OWN entrance mine (it is a known
  --     mine on our own ground, which is exactly what demine hunts) and takes
  --     the goal away from the substate machine that is about to shoot it;
  --   * the terrain-repair job treats the crater and the river the mine just
  --     made as battle damage and PAVES THEM BACK TO ROAD — measured on
  --     tests/sea_pills_A: boat built, tile roaded over ~90 ticks later, plan
  --     restarted from scratch.
  -- Both are right about a stray mine and wrong about this one. The chain is
  -- short and self-limiting (it drops its plan the moment we are ashore
  -- again), so demine simply stands down for its duration.
  local _sea_active = state.goal and state.goal.kind == "capture_pill"
                      and state.goal.sea and not state.goal.sea.done
  if not _sea_active then
    demine.update(state, world, info)
  end
  if BRAIN_DEBUG_MODE then demine.draw_overlay(state, world, info) end

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
      local saved_gs = goals.save_goal_state()
      -- Exclude the objective we're about to reach from the peek WITHOUT
      -- touching state.blocked. state.blocked is a persistent, visualizer-
      -- visible "tile on cooldown" map; poking it here (it used to set
      -- now+9999) leaked a phantom "REJECT blocked 9999t" onto the pool
      -- panel and conflated "about to capture" with "unreachable". Instead
      -- drop this goal's pool_cache entry for the peek only, then restore it,
      -- so pick_goal returns the NEXT-best goal and nothing persists.
      local ex_idx, ex_val
      if state.pool_cache then
        for pi = 1, 13 do
          local pce = state.pool_cache[pi]
          if pce and pce.goal and pce.goal.mx == saved_goal.mx
             and pce.goal.my == saved_goal.my then
            ex_idx, ex_val = pi, pce
            state.pool_cache[pi] = nil
            break
          end
        end
      end
      local peek = goals.pick_goal(state, world, info, true)  -- quiet: suppress logs
      goals.restore_goal_state(saved_gs)
      if ex_idx then state.pool_cache[ex_idx] = ex_val end
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
    if BRAIN_PROFILE then
      opt(string.format("lookahead done %.2f ms", (t_steer0 - t_goal1) / 1000))
    end
  end
  state._cautious_lookahead_held = nil   -- reset each tick; the cautious near-ally guard sets it
  -- SEA-PILL HARVEST substate machine. Runs BEFORE steering and the builder so
  -- both act on the substate this tick set (steering parks at F / drives onto
  -- the boat; the builder dispatches the mine and the boat build).
  goals.sea_update(state, world, info)
  -- Keeps the covered-water no-go set alive for the whole boat trip, including
  -- the run home after the harvest plan itself has ended.
  goals.sea_nogo_tick(state, info)
  local keys, taps = steer.steer(state, world, info, state.goal)
  -- NO-GO WATER: SLOW DOWN EARLY, do not brake late.
  -- A boat's turn RATE is terrain-fixed, so its turn RADIUS grows with speed:
  -- at spd 52 a 90-degree arc overshoots about a tile, at spd 16 about a third
  -- of one. On tests/sea_pills_B the tank sat at (138,128) turning while
  -- ACCELERATING 32 -> 52, and the old one-tile-ahead brake did not fire until
  -- spd 52 with one tile to go — 20 ticks too late to stop a boat. The fix is
  -- to never carry the speed in the first place: while covered water is near,
  -- cruise, and never accelerate through a turn.
  -- Single choke point after steering, like the cliff-safety brake, so no goal
  -- branch can route around it. Keyed on _sea_nogo, which outlives the plan.
  if info.inboat and state._sea_nogo and next(state._sea_nogo) ~= nil then
    local nogo   = state._sea_nogo
    local tmx0   = bit.rshift(info.tankx, 8)
    local tmy0   = bit.rshift(info.tanky, 8)
    local slow_r = C.SEA_NOGO_SLOW_RADIUS or 1.5
    local near_r = C.SEA_NOGO_HEADING_RADIUS or 3
    -- Nearest covered tile, over a small box rather than the whole set.
    local near_d = 1e9
    local R = math.ceil(near_r)
    for dy = -R, R do
      for dx = -R, R do
        if nogo[(tmy0 + dy) * 256 + (tmx0 + dx)] then
          local d = math.sqrt(dx * dx + dy * dy)
          if d < near_d then near_d = d end
        end
      end
    end
    -- (a) Would we COAST into one? Ask the same engine-exact stopping model the
    -- cliff brake uses, and treat the stop tile and its neighbours as inside.
    local stop_in_nogo = false
    if near_d <= near_r + 2 then
      local tcap = (C.TERRAIN_SPEED and C.TERRAIN_SPEED[U.ttype(tmx0, tmy0)]) or 16
      local psx, psy = cpf.predict_stop(info.tankx, info.tanky,
                                        info.tank_angle or info.direction,
                                        (info.speed or 0) / 4, tcap)
      local smx0, smy0 = bit.rshift(psx, 8), bit.rshift(psy, 8)
      for dy = -1, 1 do
        for dx = -1, 1 do
          if nogo[(smy0 + dy) * 256 + (smx0 + dx)] then stop_in_nogo = true end
        end
      end
    end
    local cap = C.SEA_NOGO_SPEED_CAP or 16
    if near_d <= slow_r or stop_in_nogo then
      if (info.speed or 0) > cap then
        keys = bit.bor(bit.band(keys, bit.bnot(KEY_FASTER)), KEY_SLOWER)
      else
        keys = bit.band(keys, bit.bnot(KEY_FASTER))
      end
      if BRAIN_DEBUG_MODE then print2(string.format("SEA_NOGO_SLOW t=%d spd=%d near=%.1f stop_in_nogo=%s cap=%d", now, info.speed or 0, near_d, tostring(stop_in_nogo), cap)) end
    elseif near_d <= near_r and state._steer_lx then
      -- (b) Never accelerate THROUGH a turn near the set: a wide arc is exactly
      -- how the boat drifted sideways into the tile it was refusing to enter.
      local want = U.aim_at(info.tankx, info.tanky,
                            U.m2w(state._steer_lx), U.m2w(state._steer_ly))
      if math.abs(U.adiff(info.direction, want)) > (C.SEA_NOGO_TURN_BRADS or 32) then
        keys = bit.band(keys, bit.bnot(KEY_FASTER))
        if BRAIN_DEBUG_MODE then print2(string.format("SEA_NOGO_NOACCEL t=%d spd=%d near=%.1f corr=%.0f", now, info.speed or 0, near_d, U.adiff(info.direction, want))) end
      end
    end
  end
  -- Cautious-approach speed: when our tank is on (or stepping onto) an ally's
  -- pill-take ring, hold a steady boat-in-a-river cruise — CAP the speed rather
  -- than bleed it to zero. Only decelerate when above the cap; otherwise leave
  -- steering's accelerate intact so the tank keeps making progress per-tile.
  -- (info.speed is engine-speed ×4, so 0..64; KEY_SLOWER decelerates, it does
  -- NOT reverse.) Pairs with path_lookahead's per-tile crawl so momentum can't
  -- carry us onto the ally's tile. Scoped strictly to the take ring.
  state._take_crawl_active = nil
  if state._ally_take_tiles then
    local s = state._ally_take_tiles
    local cmx, cmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
    local pf = state.pf
    local nmx, nmy = pf and pf.next_mx, pf and pf.next_my
    local on_cur  = s[cmy * 256 + cmx]
    local on_next = nmx and nmy and s[nmy * 256 + nmx]
    if on_cur or on_next then
      local cap = C.TAKE_CRAWL_MAX_SPEED or 28
      -- Hard-brake exception: if we're within ~1/2 tile of our OWN stop point
      -- (attack_pill approach point / in-range standoff) and still moving fast,
      -- brake HARD even on the ally take ring. Otherwise the crawl-cruise cap
      -- (which only brakes ABOVE the cap) lets us coast straight through our own
      -- setup point — a blitz commander sailed past its setup at spd 28 because
      -- the setup tile sat inside an ally's take ring. The crawl still governs
      -- pure transit; this only fires when WE are arriving at our destination.
      local stop_close_fast = false
      local g = state.goal
      if g and g.kind == "attack_pill" then
        local sx, sy
        if g.substate == "approach" then
          sx = g.approach_fx or (g.approach_mx and (g.approach_mx + 0.5))
          sy = g.approach_fy or (g.approach_my and (g.approach_my + 0.5))
        elseif g.substate == "in_range_position" then
          sx = g.standoff_fx or (g.standoff_mx and (g.standoff_mx + 0.5))
          sy = g.standoff_fy or (g.standoff_my and (g.standoff_my + 0.5))
        end
        if sx and sy then
          local sd = U.wdist(info.tankx, info.tanky, math.floor(sx * 256 + 0.5), math.floor(sy * 256 + 0.5))
          stop_close_fast = sd <= (C.APPROACH_HARDBRAKE_DIST or 128)
                            and (info.speed or 0) > (C.APPROACH_HARDBRAKE_SPEED or 8)
        end
      end
      -- Default is to LEAVE steering's keys alone. We only ever override ONE
      -- specific brake: the cautious near-ally CREEP (state._cautious_lookahead_held,
      -- tagged in steering when it holds the lookahead to crawl through an ally
      -- take). Every other KEY_SLOWER — cliff/drowning, destination-stop, combat —
      -- is steering's deliberate brake and survives untouched. This replaces the
      -- old "strip everything below the cap, then re-exempt cliff & setpoint" that
      -- kept sailing tanks through their stop point and into deep water.
      if stop_close_fast then
        keys = bit.bor((bit.band(keys, bit.bnot(KEY_FASTER))), KEY_SLOWER)   -- arriving fast at our OWN stop point: hard brake
      elseif (info.speed or 0) > cap then
        keys = bit.bor((bit.band(keys, bit.bnot(KEY_FASTER))), KEY_SLOWER)   -- above cruise: brake toward the cap
      elseif state._cautious_lookahead_held then
        keys = bit.band(keys, bit.bnot(KEY_SLOWER))                  -- ONLY the cautious creep: strip it to hold a steady cruise
      end
      state._take_crawl_active = on_cur and "on" or "next"   -- for the viz overlay
      if BRAIN_DEBUG_MODE then local _crawl_act = stop_close_fast and "HARDBRAKE" or ((info.speed or 0) > cap and "BRAKE" or (state._cautious_lookahead_held and "cruise" or "hold-brake")); print2(string.format("TAKE_CRAWL t=%d tile=(%d,%d) trigger=%s spd=%d cap=%d %s — boat-cruise per-tile near ally take", state.tick or 0, cmx, cmy, on_cur and "on-ring" or "stepping-onto", info.speed or 0, cap, _crawl_act)) end
      if BRAIN_DEBUG_MODE and viz.is_on("cautious_nav_around_ally_take") then viz.rect("cautious_nav_around_ally_take", cmx, cmy, cmx + 1, cmy + 1, 255, 90, 0, 200, true) end
      if BRAIN_DEBUG_MODE and viz.is_on("cautious_nav_around_ally_take") then viz.text("cautious_nav_around_ally_take", cmx + 0.5, cmy - 0.5, "CRAWL:" .. state._take_crawl_active, "center", 255, 220, 120, 230, 0.4) end
    end
  end

  -- Log the ACTUAL committed nav path every ~50t (cheap) so log review / playback
  -- can see the exact route the bot chose — the host's key-4 slate re-trace is
  -- unreliable for a blitz soldier (traces from a stale/foreign slate origin).
  if BRAIN_DEBUG_MODE and (state.tick or 0) % 50 == 0 and state.pf then
    local pc = state.pf.path_chain
    local parts = {}
    if pc then for i = 1, __idiv(#pc, 2) do parts[#parts + 1] = string.format("(%d,%d)", pc[2*i-1], pc[2*i]) end end
    print2(string.format("PF_PATH t=%d status=%s dest=(%s,%s) next=(%s,%s) n=%d %s", state.tick or 0, tostring(state.pf.status), tostring(state.pf.dest_mx), tostring(state.pf.dest_my), tostring(state.pf.next_mx), tostring(state.pf.next_my), #parts, (#parts > 0) and table.concat(parts, "->") or "EMPTY"))
  end

  -- pf_path_lines overlay: draw the ACTUAL committed path (state.pf.path_chain) as
  -- a green polyline + next-step box + dest ring. Drawn during think() so it lands
  -- in the per-frame overlay buffer and REPLAYS in playback (BrainTest swaps the
  -- recorded overlay cmds in). Revives the previously-dead pf_path_lines id so
  -- playback finally shows the route the bot really chose (not a slate re-trace).
  if BRAIN_DEBUG_MODE and viz.is_on("pf_path_lines") and state.pf and state.pf.path_chain and #state.pf.path_chain >= 4 then
    local pc = state.pf.path_chain
    for i = 1, (__idiv(#pc, 2)) - 1 do
      viz.line("pf_path_lines", pc[2*i-1] + 0.5, pc[2*i] + 0.5, pc[2*i+1] + 0.5, pc[2*i+2] + 0.5, 0, 255, 90, 235)
    end
    local nmx, nmy = state.pf.next_mx or 0, state.pf.next_my or 0
    local dmx, dmy = state.pf.dest_mx or 0, state.pf.dest_my or 0
    viz.rect("pf_path_lines", nmx + 0.15, nmy + 0.15, nmx + 0.85, nmy + 0.85, 140, 255, 170, 210, true)
    viz.circle("pf_path_lines", dmx + 0.5, dmy + 0.5, 0.45, 0, 255, 90, 220)
  end

  local t_steer1 = clock_us()
  metrics.set("us_steer", t_steer1 - t_steer0)
  if BRAIN_PROFILE then
    opt(string.format("steer done %.2f ms", (t_steer1 - t_steer0) / 1000))
  end
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
    local shooting = (bit.band(keys, KEY_SHOOT)) ~= 0
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
  local t_psv_wounded = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  crosshairs+wounded viz done %.2f ms", (t_psv_wounded - t_steer1) / 1000))
  end

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
  local t_psv_self_dr = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  pool6 self_dr labels done %.2f ms", (t_psv_self_dr - t_psv_wounded) / 1000))
  end

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
  local t_psv_inspect = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  inspect_pill done %.2f ms (active=%s)",
      (t_psv_inspect - t_psv_self_dr) / 1000, tostring(state.inspect_pill ~= nil)))
  end

  -- Opportunistic tank shot: fire at enemy tanks while doing other things.
  -- Only if not already in tank combat and not shooting at something else.
  if state.goal.kind ~= "attack_tank"
     and (bit.band(keys, KEY_SHOOT)) == 0 and (bit.band(taps, KEY_SHOOT)) == 0
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
            taps = bit.bor(taps, KEY_SHOOT)
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
  local _shoot_busy = (bit.band(keys, KEY_SHOOT)) ~= 0 or (bit.band(taps, KEY_SHOOT)) ~= 0
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
        -- DIFFICULTY AIM_ERROR_BRADS: deflect the LGM aim POINT here, BEFORE the
        -- LOS raycast and the impact-offset fire gate below both read aim_wx/
        -- aim_wy (so the validated shell line is the deflected line). Keyed on
        -- elm.idnum so the crosshair search (below) deflects by the SAME amount
        -- for the primary LGM. No-op at 0 (moving-target miss on Easy/Medium).
        aim_wx, aim_wy = U.aim_error_point(state, C.AIM_ERROR_BRADS,
                                           info.tankx, info.tanky, aim_wx, aim_wy, elm.idnum)
        local target_sl = elm.target_sightLen
        local aim_dir = U.aim_at(info.tankx, info.tanky, aim_wx, aim_wy)
        local aim_corr = U.adiff(info.direction, aim_dir)
        _ev.aim_corr = aim_corr
        _ev.ttl = elm.flight_ticks
        _ev.aim_mx = bit.rshift(math.floor(aim_wx + 0.5), 8)
        _ev.aim_my = bit.rshift(math.floor(aim_wy + 0.5), 8)
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
              local origin_mx, origin_my = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
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
                  if bentry and bentry.base then  -- a base of ANY owner stops the shell
                    los_blocked = true; break
                  end
                end
              end
            end
            los_cache[cache_key] = { tick = now, blocked = los_blocked }
          end
          _ev.los_blocked = los_blocked
          -- Fire-gate: the shell explosion point must land inside a
          -- circle of radius kill_lgm.tuning().fire_wu around the predicted
          -- LGM center -- C.LGM_KILL_FIRE_WU while the LGM-kill package is on
          -- (C.LGM_KILL_IMPROVED), the old 64 wu while it is off.
          -- The engine kills on open ground within 128 wu
          -- (lgm.c MAP_SQUARE_MIDDLE); the knob defaults WIDER (256, one
          -- tile) because both points here are PREDICTED — a shot allowed
          -- at a tile of predicted offset still lands in the killing
          -- half-tile whenever the prediction was half right, and a
          -- reload is cheap.  KEEL is 64.  Shell explodes at
          -- tank + 128*sl wu in the firing direction (sl in half-tiles,
          -- so 128 wu/unit).
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
          local KILL_WU = kill_lgm.tuning().fire_wu  -- circle around predicted LGM center
          if los_blocked then
            _ev.status = "los_blocked"
          elseif impact_off > KILL_WU then
            _ev.status = "gunrange_off"
          elseif _shoot_busy or _already_fired then
            _ev.status = "ready_busy"
          else
            _ev.status = "shooting"
            taps = bit.bor(taps, KEY_SHOOT)
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
    keys = bit.bor(keys, KEY_MORERANGE)
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
    -- DIFFICULTY AIM_ERROR_BRADS: deflect the search target by the SAME offset
    -- the fire gate above used (same idnum + tick-block seed), so the crosshair
    -- drives to the deflected point and the gate fires on it. No-op at 0.
    lgm_wx, lgm_wy = U.aim_error_point(state, C.AIM_ERROR_BRADS,
                                       tank_wx, tank_wy, lgm_wx, lgm_wy, _primary_lgm.idnum)
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
              keys = bit.bor(bit.bor(t.k, s.k), g.k),
            }
          end
          if score < best_score then
            best_score = score
            best_keys  = bit.bor(bit.bor(t.k, s.k), g.k)
          end
        end
      end
    end
    -- Override the action bits: clear the six we manage, set the
    -- winning combination.  Anything else (KEY_SHOOT, KEY_DROPMINE,
    -- etc.) is preserved.
    local CLEAR = bit.bor(bit.bor(bit.bor(bit.bor(bit.bor(KEY_TURNLEFT, KEY_TURNRIGHT), KEY_FASTER), KEY_SLOWER), KEY_MORERANGE), KEY_LESSRANGE)
    keys = bit.bor((bit.band(keys, bit.bnot(CLEAR))), best_keys)
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

  -- ── Capture-pill LGM hunt: gunsight + the armour-rise shot ─────────
  -- steering.lua's capture_lgm_hunt already did the detection and gave the
  -- TURN keys to the aim solution (see C.CAPTURE_LGM_HUNT).  Two things are
  -- left, and both live here because this is where the gun is driven:
  --
  --   1. THE GUNSIGHT.  The kill-LGM fire block above fires at any hostile
  --      LGM in range whatever the goal is, but its gate is the real
  --      explosion point (tank + 128*gunrange wu along the heading) landing
  --      within 64 wu of the lead point -- and the only thing that MOVES the
  --      crosshair is the 27-candidate search, which is gated on goal ==
  --      kill_lgm.  Under capture_pill the crosshair sat wherever the last
  --      goal left it, so that gate essentially never opened.  Driving the
  --      gunsight to the LGM's own target_sightLen is the whole fix; the
  --      existing block then pulls the trigger on its own.
  --
  --   2. THE ARMOUR-RISE SHOT.  On the `armour` trigger there is no visible
  --      LGM at all, so the block above has nothing to fire at.  We aim at
  --      the pill tile and shell it ourselves: knocking a half-finished
  --      repair back to 0 is exactly the outcome we want, and a dead pill
  --      cannot be hurt by our own shells.
  --
  -- Throttle is never touched here -- the whole point of the feature is that
  -- the capture keeps its speed.
  do
    -- Helpers are LOCAL to Brain.think (not module scope): hoisting them out made
    -- think reference them as upvalues and blew past PUC-Lua's 60-upvalue cap, so
    -- the brain failed to load.  As think-locals they cost nothing to think's
    -- upvalue count, and they close over info / world directly.
    --
    -- Drive the crosshair so the shell impact lands nearest a WORLD target point
    -- (px_w, py_w in wu): impact for sightLen L sits 128*L wu ahead on the current
    -- heading, so the best length is round(along/128).  Returns TWO keys (held,
    -- tap): >1 sightLen unit to go closes fast with a HELD key (2 packets/think =
    -- 2 units); the FINAL unit is a TAP (one packet = one unit = 64 wu), because a
    -- held key moves in steps of 2 and strands us a unit short on odd offsets.
    local function capture_sight_step(px_w, py_w)
      local G = info.gunrange or 14
      local rad = (info.direction or 0) * C.TWO_PI / 256
      local hx, hy = math.sin(rad), -math.cos(rad)
      local along = (px_w - info.tankx) * hx + (py_w - info.tanky) * hy
      local L = math.floor(along / 128 + 0.5)
      if L < 2 then L = 2 elseif L > 14 then L = 14 end
      if L == G then return 0, 0 end                                -- settled
      local key = (L > G) and KEY_MORERANGE or KEY_LESSRANGE
      -- HOLD vs TAP.  A held key steps the engine every ENGINE tick while we
      -- think every second tick and the engine applies our packet 2-3 ticks
      -- late, so a hold always overshoots by a few units and the sight has to
      -- come back.  C.CAPTURE_SIGHT_HOLD_MIN_STEPS is the gap at which we still
      -- accept that trade; below it we TAP (one packet = one unit).  =2 is the
      -- old behaviour (tap only the final unit); a big number never holds.
      if math.abs(L - G) < (kill_lgm.tuning().hold_min_steps or 2) then
        return 0, key                                               -- close: TAP
      end
      return key, 0                                                 -- far: HELD
    end
    -- A man is "solid" to lgm.c's death check (in-tile-only kill, no splash) when
    -- he stands on a building, a half-build, a base, OR ANY DEPLOYED PILL.  The
    -- engine (pillbox.c pillsScanExistPos) tests DEPLOYMENT, not armour, and the
    -- dead corpse we're sweeping is still a deployed pill -- so a man on it is
    -- solid, and only an in-tile hit kills him.  world.lua indexes pill_at only for
    -- a deployed (not-carried) pill, so ANY pill_at entry means solid regardless of
    -- health.  (This is a DIFFERENT rule from the shell-collision LOS check, which
    -- correctly keeps health>0 because shells only collide with LIVE pills.)
    local function lgm_on_solid(mx, my)
      local tt = U.ttype(mx, my)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then return true end
      if world.pill_at and world.pill_at[my * 256 + mx] then return true end
      local bentry = world.base_at and world.base_at[my * 256 + mx]
      if bentry and bentry.base then return true end
      return false
    end
    -- Does the ray from (ox,oy) along unit (hx,hy), out to maxT wu, cross the
    -- 256-wu tile box at (mx,my)?  Slab test.  Used for the solid-square kill gate:
    -- on a solid square only an in-tile hit kills, so the man is a viable target
    -- only if a shot along the current heading could actually enter his tile.
    local function ray_crosses_tile(ox, oy, hx, hy, mx, my, maxT)
      local x0, y0 = mx * 256, my * 256
      local x1, y1 = x0 + 256, y0 + 256
      local tmin, tmax = 0, maxT
      if math.abs(hx) < 1e-9 then
        if ox < x0 or ox > x1 then return false end
      else
        local ta, tb = (x0 - ox) / hx, (x1 - ox) / hx
        if ta > tb then ta, tb = tb, ta end
        if ta > tmin then tmin = ta end
        if tb < tmax then tmax = tb end
        if tmin > tmax then return false end
      end
      if math.abs(hy) < 1e-9 then
        if oy < y0 or oy > y1 then return false end
      else
        local ta, tb = (y0 - oy) / hy, (y1 - oy) / hy
        if ta > tb then ta, tb = tb, ta end
        if ta > tmin then tmin = ta end
        if tb < tmax then tmax = tb end
        if tmin > tmax then return false end
      end
      return tmax >= tmin
    end
    -- Our OWN builder (LGM on foot) is NEVER in info.objects, but capture_pill sent
    -- him to this very pill, so he is the likeliest friendly-fire victim.  Returns
    -- his world (x,y) when he is OUT of the tank (and alive), else nil.
    local function own_man_out_pos()
      if info.man_x and info.man_y
         and info.man_status ~= C.LGM_INTANK and info.man_status ~= C.LGM_DEAD then
        return info.man_x, info.man_y
      end
      return nil
    end
    -- Squared distance from point (px,py) to segment (sx,sy)->(ex,ey).
    local function seg_dist2(px, py, sx, sy, ex, ey)
      local vx, vy = ex - sx, ey - sy
      local vlen2 = vx * vx + vy * vy
      local t = 0
      if vlen2 > 0 then
        t = ((px - sx) * vx + (py - sy) * vy) / vlen2
        if t < 0 then t = 0 elseif t > 1 then t = 1 end
      end
      local qx, qy = sx + vx * t - px, sy + vy * t - py
      return qx * qx + qy * qy
    end
    local clh = state._clh
    if clh and clh.tick == now and not info.inboat then
      if clh.src == "lgm" then
        -- LGM in the radius: TARGET THE MAN.  Steering already biased the heading
        -- toward him (the sweep-line nudge); here we own the GUN -- pick the best
        -- candidate man, drive the sight onto him, and fire opportunistically.

        -- ── STEP 4: candidate selection ──────────────────────────────────
        -- t = tank, h = heading unit.  For every hostile LGM inside the shoot
        -- radius (CAPTURE_LGM_HUNT_RADIUS) of the PILL we work out along =
        -- (p-t).h and perp = |h x (p-t)|, where p is the man's lead-predicted
        -- position (perception's predict_aim output; one think of lag is fine).
        -- Both are still needed below -- along drives the sight, perp is the
        -- open-ground kill test -- but WHICH men are admitted, and which one
        -- wins, now depend on the LGM-kill master switch (kill_lgm.tuning()):
        --
        --   package ON  : admitted when he is within KT.track_wu of the TANK,
        --                 measured STRAIGHT.  The nose may point anywhere.  The
        --                 old test was along-the-heading only, so while the nose
        --                 was still swinging onto him he dropped out, the sight
        --                 fell back to the pill hover, and the gunsight yo-yoed
        --                 between two targets -- one for the nose, one for the
        --                 sight.  A straight radius keeps ONE target through the
        --                 whole turn.
        --   package OFF : the old admission -- ahead of us (along > 0) and no
        --                 further than max shell travel (7 tiles) along it.
        --
        -- RANKING with KT.pick_near_pill: the man whose PREDICTED point is
        -- nearest the TARGET PILL, tie-break smaller perp.  The prediction, not
        -- the live position, is what separates the two men who matter: one just
        -- passing by is predicted AWAY from the pill, while one a little further
        -- out but walking in to repair is predicted ONTO it.  The pill is what
        -- the capture is for, so the man nearest it is the one about to rebuild
        -- it out from under us.  Otherwise the old rank: MIN perp, then along.
        local KT = kill_lgm.tuning()
        local rad  = (info.direction or 0) * C.TWO_PI / 256
        local hx, hy = math.sin(rad), -math.cos(rad)
        local MAX_ALONG = 14 * 128            -- sightLen 14 -> impact 7 tiles ahead
        local R = C.CAPTURE_LGM_HUNT_RADIUS or 2
        local CIRCLE = C.CAPTURE_LGM_HUNT_CIRCLE
        local cand, cand_perp, cand_along = nil, math.huge, math.huge
        local cand_pilld = math.huge          -- predicted point -> pill, wu (rank only)
        local pcx, pcy = clh.pmx * 256 + 128, clh.pmy * 256 + 128
        if state.perc and state.perc.enemy_lgms then
          for _, elm in ipairs(state.perc.enemy_lgms) do
            local rdx = elm.mx - clh.pmx; if rdx < 0 then rdx = -rdx end
            local rdy = elm.my - clh.pmy; if rdy < 0 then rdy = -rdy end
            local in_radius
            if CIRCLE then
              in_radius = (rdx * rdx + rdy * rdy) <= R * R
            else
              in_radius = ((rdx > rdy) and rdx or rdy) <= R
            end
            if in_radius then
              local pwx = elm.predicted_wx or elm.wx
              local pwy = elm.predicted_wy or elm.wy
              local rx, ry = pwx - info.tankx, pwy - info.tanky
              local along = rx * hx + ry * hy
              local perp  = math.abs(hx * ry - hy * rx)
              local admit
              if KT.track_wu then
                admit = (rx * rx + ry * ry) <= KT.track_wu * KT.track_wu
              else
                admit = (along > 0 and along <= MAX_ALONG)
              end
              if admit then
                local better
                if KT.pick_near_pill then
                  local qx, qy = pwx - pcx, pwy - pcy
                  local pilld = math.sqrt(qx * qx + qy * qy)
                  better = (pilld < cand_pilld)
                           or (pilld == cand_pilld and perp < cand_perp)
                  if better then cand_pilld = pilld end
                else
                  better = (perp < cand_perp)
                           or (perp == cand_perp and along < cand_along)
                end
                if better then
                  cand, cand_perp, cand_along = elm, perp, along
                end
              end
            end
          end
        end
        -- KILLABLE from this heading?  Open ground: within KT.perp_wu of the ray
        -- (splash reaches).  That is 128 wu with the package off, and it opens to
        -- the fire gate when the gate is wider, so this test can never refuse a
        -- shot the fire gate would take.  Solid square (live pill/base/building):
        -- the ray must actually cross his tile -- splash won't kill there, only
        -- an in-tile hit.  This gates the SHOT, not the tracking.
        local killable = false
        local cmx, cmy
        if cand then
          cmx, cmy = cand.predicted_mx or cand.mx, cand.predicted_my or cand.my
          if lgm_on_solid(cmx, cmy) then
            killable = ray_crosses_tile(info.tankx, info.tanky, hx, hy, cmx, cmy, MAX_ALONG)
          else
            killable = (cand_perp <= KT.perp_wu)
          end
        end

        -- ── STEP 4: sight drive (yield to any earlier range key this think) ─
        -- With the package ON the sight follows the candidate from the moment he
        -- is admitted, killable or not: he is the one target, and the sight and
        -- the nose close on him together.  The pill hover is the fallback for NO
        -- candidate at all.  With the package OFF, the old rule -- only a
        -- killable man takes the sight.
        local tgt_wx, tgt_wy
        if cand and (KT.on or killable) then
          tgt_wx, tgt_wy = (cand.predicted_wx or cand.wx), (cand.predicted_wy or cand.wy)
          clh.lgm_id = cand.idnum          -- so the debug gate line tracks this man
        else
          tgt_wx, tgt_wy = clh.pmx * 256 + 128, clh.pmy * 256 + 128   -- pill hover
        end
        if bit.band(bit.bor(keys, taps), bit.bor(KEY_MORERANGE, KEY_LESSRANGE)) == 0 then
          local hk, tk = 0, 0
          if C.CAPTURE_LGM_HUNT_SIGHT_ON_PILL then
            hk, tk = capture_sight_step(tgt_wx, tgt_wy)
          else
            local tsl = (cand and killable and cand.target_sightLen)
                        or kill_lgm.sightlen_for(clh.dist_wu)
            hk = kill_lgm.gunrange_key(info.gunrange or 14, tsl)
          end
          keys = bit.bor(keys, hk)
          taps = bit.bor(taps, tk)
        end

        -- ── STEP 4b: aim refinement to a QUARTER TILE (C.LGM_KILL_AIM_WU) ──
        -- Shoot at a tile, aim for a quarter tile.  While a candidate is being
        -- hunted and the AIM ERROR -- the distance from the predicted impact
        -- point to his predicted point -- is still above KT.aim_wu, every tick
        -- picks the action that makes it smaller.  Nothing here delays or
        -- suppresses a shot: STEP 5 runs next, reads the keys this block just
        -- set (so its sl_eff is the refined length) and fires on THIS tick
        -- whenever the error is inside the fire gate.
        --
        -- What bounded the error before (no code ever "held" the aim; these are
        -- floors):  the sight steps in 128 wu units along the ray, so the
        -- along-ray residual alone is up to 64 wu at the best rounding; and
        -- steering's turn deadband is a flat 2 brads, which at this shot's
        -- 6-7 tile range is about 80 wu sideways.
        --
        -- The search is the shape of the kill_lgm goal's 27-candidate search
        -- above: for each (turn x throttle x sight) combination predict the
        -- tank one THINK ahead, put the impact 128*sightLen along the new
        -- heading, score by distance to the man, keep the minimum.  Which axes
        -- we are allowed to move:
        --   TURN     only when the hunt already owns the turn (steer "turn",
        --            never the partial "nudge") and no U-turn is latched --
        --            with the man behind us the turn is navigation's.  And
        --            only outside the derived deadband, so the nose cannot
        --            chatter: asin(aim_wu / dist) in brads, floored at 1 -- the
        --            angle a quarter tile subtends at this range.
        --   SIGHT    only when nobody asked for a range key this think (the
        --            same yield rule STEP 4 uses).
        --   THROTTLE only with C.LGM_KILL_AIM_THROTTLE (default OFF): a speed
        --            step is worth ~2 wu of along-ray correction, and the
        --            capture must never brake for the builder.
        if KT.aim_wu and cand and tgt_wx then
          local cur_dir = info.direction or 0
          local cur_gun = info.gunrange or 14
          -- info.speed is the engine speed x4 (brain_data.c: tankGetSpeed*4),
          -- and a think is two engine ticks, so one think of travel is
          -- speed/4 * 2 = speed/2 wu.
          local cur_spd = (info.speed or 0) / 2
          local aim_dir  = U.aim_at(info.tankx, info.tanky, tgt_wx, tgt_wy)
          local aim_corr = U.adiff(cur_dir, aim_dir)
          local tdx, tdy = tgt_wx - info.tankx, tgt_wy - info.tanky
          local dist = math.sqrt(tdx * tdx + tdy * tdy)
          -- The error as it stands, at the length this tick will really fly at
          -- (the same sl_eff rule STEP 5 uses: a range key in this packet is
          -- applied before the shot).
          local rk0 = bit.bor(keys, taps)
          local sl0 = cur_gun
          if bit.band(rk0, KEY_MORERANGE) ~= 0 then sl0 = cur_gun + 1
          elseif bit.band(rk0, KEY_LESSRANGE) ~= 0 then sl0 = cur_gun - 1 end
          if sl0 < 2 then sl0 = 2 elseif sl0 > 14 then sl0 = 14 end
          local r0 = cur_dir * C.TWO_PI / 256
          local ex0 = info.tankx + math.sin(r0) * 128 * sl0 - tgt_wx
          local ey0 = info.tanky - math.cos(r0) * 128 * sl0 - tgt_wy
          clh.aim_err_wu = math.sqrt(ex0 * ex0 + ey0 * ey0)
          clh.aim_wu     = KT.aim_wu
          if clh.aim_err_wu > KT.aim_wu then
            local dband = 1
            if dist > 0 then
              local s = KT.aim_wu / dist
              if s > 1 then s = 1 end
              dband = math.floor(math.asin(s) * 256 / C.TWO_PI)
              if dband < 1 then dband = 1 end
            end
            clh.aim_dband = dband
            -- steer == "turn" only, never "nudge": the nudge is a DELIBERATELY
            -- partial lean that keeps the plow-through line to the corpse (it
            -- is what replaced the old full-aim pin), so pointing the nose all
            -- the way at the man on a nudge tick would undo it.
            local own_turn = (clh.steer == "turn")
                             and state._uturn == nil
                             and math.abs(aim_corr) > dband
            local own_gun  = bit.band(bit.bor(keys, taps),
                                      bit.bor(KEY_MORERANGE, KEY_LESSRANGE)) == 0
            -- Action tables built once per bot (no per-tick garbage), same
            -- reason as the kill_lgm search's move tables.
            local at = state._clh_aim_tables
            if not at then
              at = {
                TURNS = { { k = 0,             d =  0 },
                          { k = KEY_TURNLEFT,  d = -6 },
                          { k = KEY_TURNRIGHT, d =  6 } },
                SPEEDS = { { k = 0,          d =  0 },
                           { k = KEY_FASTER, d =  2 },
                           { k = KEY_SLOWER, d = -2 } },
                -- One unit, not two: the search only ever TAPS the sight, so a
                -- refinement step can never overshoot the way a held key does.
                -- The coarse close is still STEP 4's (it may hold), and this
                -- axis is only ours on a think STEP 4 asked for nothing.
                GUNS = { { k = 0,             d =  0 },
                         { k = KEY_MORERANGE, d =  1 },
                         { k = KEY_LESSRANGE, d = -1 } },
              }
              state._clh_aim_tables = at
            end
            local best, best_turn, best_spd, best_gun = math.huge, 0, 0, 0
            for _, t in ipairs(at.TURNS) do
              if t.k == 0 or own_turn then
                local nd = (cur_dir + t.d) % 256
                local rr = nd * C.TWO_PI / 256
                local shx, shy = math.sin(rr), -math.cos(rr)
                for _, s in ipairs(at.SPEEDS) do
                  if s.k == 0 or KT.aim_throttle then
                    local nsp = cur_spd + s.d
                    if nsp < 0 then nsp = 0 end
                    local ptx = info.tankx + shx * nsp
                    local pty = info.tanky + shy * nsp
                    for _, g in ipairs(at.GUNS) do
                      if g.k == 0 or own_gun then
                        -- sl0, not cur_gun: a range key already in this packet
                        -- is applied before the shot, so sl0 is the length the
                        -- "hold the sight" candidate really flies at.
                        local ng = sl0 + g.d
                        if ng < 2 then ng = 2 elseif ng > 14 then ng = 14 end
                        local dx = ptx + shx * 128 * ng - tgt_wx
                        local dy = pty + shy * 128 * ng - tgt_wy
                        local sc = dx * dx + dy * dy
                        if sc < best then
                          best, best_turn, best_spd, best_gun = sc, t.k, s.k, g.k
                        end
                      end
                    end
                  end
                end
              end
            end
            clh.aim_pred_wu = math.sqrt(best)
            -- Apply only the axes we were allowed to move.  A small heading
            -- error gets a TAP (one packet, ~3 brads); a large one gets the key
            -- HELD, the same split steering uses.
            if own_turn and best_turn ~= 0 then
              keys = bit.band(keys, bit.bnot(bit.bor(KEY_TURNLEFT, KEY_TURNRIGHT)))
              taps = bit.band(taps, bit.bnot(bit.bor(KEY_TURNLEFT, KEY_TURNRIGHT)))
              if math.abs(aim_corr) > 8 then keys = bit.bor(keys, best_turn)
              else taps = bit.bor(taps, best_turn) end
            end
            if own_gun and best_gun ~= 0 then
              taps = bit.bor(taps, best_gun)     -- one unit: never overshoot
            end
            if KT.aim_throttle and best_spd ~= 0 then
              keys = bit.bor(keys, best_spd)
            end
          end
        end

        -- ── STEP 5: opportunistic capture-target shot ─────────────────────
        -- Fire when the predicted impact lands within the LGM-kill fire gate of
        -- the man (open ground) or IN HIS EXACT TILE (solid square).  The gate is
        -- the SHARED C.LGM_KILL_FIRE_WU unless CAPTURE_LGM_HUNT_FIRE_WU overrides
        -- it (>= 0); -1, the default, means "follow the shared knob".
        -- Refire every reload-available tick, but cap consecutive misses.
        -- Refire identity is keyed on cand.seen_since, NOT idnum: perception's
        -- idnum churns frame-to-frame (identity is tracked by position match), so
        -- keying on idnum would reset the counter every think and the cap would
        -- never engage.  seen_since is carried through the position match and is
        -- stable per tracked man.  On kill/leave the candidate goes nil, so we
        -- DROP the record entirely -- losing the count when he leaves the radius
        -- is the right trade (his next incarnation must not inherit a stale cap).
        local refire = state._clh_refire
        if not (cand and killable) then
          state._clh_refire = nil
          refire = nil
        else
          if refire == nil then refire = {}; state._clh_refire = refire end
          if refire.id ~= cand.seen_since then
            refire.id = cand.seen_since; refire.misses = 0; refire.last_fire = nil
          end
        end
        local MAXM = C.CAPTURE_LGM_HUNT_REFIRE_MAX_MISSES or 0
        -- MISS CAP DISABLED for now (Andrew 2026-09-10): keep firing every reload
        -- while killable, regardless of misses.  The miss ACCOUNTING below still
        -- runs so the `capture_lgm_misses` visualizer can show the running count.
        -- local capped = MAXM > 0 and cand and killable and refire
        --                and refire.id == cand.seen_since and (refire.misses or 0) >= MAXM
        local capped = false
        if cand and killable and not capped
           and not _no_shells and not _shoot_busy and not _already_fired
           and (bit.band(taps, KEY_SHOOT)) == 0
           and clh.dist_wu <= C.KILL_LGM_SHOOT_RANGE * 256 then
          local cur_sl = info.gunrange or 14
          -- FIX 3c: the engine applies the gunsight change BEFORE the shot in the
          -- SAME tick (server_sim_tick.c: gunsight then shot), and the shoot tap
          -- rides the same packet as the range key -- so a shot fired on a think
          -- that also issues a range key actually flies at cur_sl +/- 1 (~128 wu
          -- along the ray).  Held OR tap both count as one step in that packet.
          -- Predict the impact at that EFFECTIVE length, not cur_sl, or the gate
          -- (and the exact-tile test) is off by a tile.
          local rk = bit.bor(keys, taps)
          local sl_eff = cur_sl
          if bit.band(rk, KEY_MORERANGE) ~= 0 then sl_eff = cur_sl + 1
          elseif bit.band(rk, KEY_LESSRANGE) ~= 0 then sl_eff = cur_sl - 1 end
          if sl_eff < 2 then sl_eff = 2 elseif sl_eff > 14 then sl_eff = 14 end
          local ex_wx = info.tankx + hx * 128 * sl_eff   -- predicted impact this tick
          local ex_wy = info.tanky + hy * 128 * sl_eff
          local hit
          if lgm_on_solid(cmx, cmy) then
            local imx = bit.rshift(math.floor(ex_wx + 0.5), 8)
            local imy = bit.rshift(math.floor(ex_wy + 0.5), 8)
            hit = (imx == cmx and imy == cmy)           -- exact-tile only
          else
            local ddx, ddy = ex_wx - tgt_wx, ex_wy - tgt_wy
            hit = (math.sqrt(ddx * ddx + ddy * ddy) <= KT.capture_fire_wu)
          end
          if hit then
            -- LANE CHECK: shell path clear of friendly tanks/builders (mirror the
            -- armour branch's segment sweep, endpoint = the man).
            local lane_ok = true
            local sx, sy = info.tankx, info.tanky
            local vx, vy = tgt_wx - sx, tgt_wy - sy
            local vlen2 = vx * vx + vy * vy
            for _, ob in ipairs(info.objects or {}) do
              if (ob.type == OBJECT_TANK or ob.type == OBJECT_BUILDMAN)
                 and (bit.band(ob.info, OBJECT_HOSTILE)) == 0
                 and not (ob.type == OBJECT_TANK and ob.idnum == (info.player_number or -1)) then
                local tt = 0
                if vlen2 > 0 then
                  tt = ((ob.x - sx) * vx + (ob.y - sy) * vy) / vlen2
                  if tt < 0 then tt = 0 elseif tt > 1 then tt = 1 end
                end
                local qx, qy = sx + vx * tt - ob.x, sy + vy * tt - ob.y
                if qx * qx + qy * qy <= 256 * 256 then
                  lane_ok = false
                  clh.lane_block = ob.type == OBJECT_TANK and "ally_tank" or "friendly_builder"
                  break
                end
              end
            end
            -- FIX 3b: our OWN builder is never in info.objects, yet capture_pill
            -- sent him to this very pill -- test him against the shell path too.
            if lane_ok then
              local mmx, mmy = own_man_out_pos()
              if mmx and seg_dist2(mmx, mmy, sx, sy, tgt_wx, tgt_wy) <= 256 * 256 then
                lane_ok = false
                clh.lane_block = "own_builder"
              end
            end
            -- IMPACT-RADIUS CHECK: no friendly tank / LGM within 128 wu of the
            -- predicted impact point.  The lane sweep guards the PATH; this guards
            -- the BLAST -- a team-mate standing on the corpse we're shelling.
            local blast_ok = true
            if lane_ok then
              for _, ob in ipairs(info.objects or {}) do
                if (ob.type == OBJECT_TANK or ob.type == OBJECT_BUILDMAN)
                   and (bit.band(ob.info, OBJECT_HOSTILE)) == 0
                   and not (ob.type == OBJECT_TANK and ob.idnum == (info.player_number or -1)) then
                  local bdx, bdy = ob.x - ex_wx, ob.y - ex_wy
                  if bdx * bdx + bdy * bdy <= 128 * 128 then
                    blast_ok = false
                    clh.lane_block = "friendly_blast"
                    break
                  end
                end
              end
              -- FIX 3b: our own builder against the blast radius as well.
              if blast_ok then
                local mmx, mmy = own_man_out_pos()
                if mmx then
                  local bdx, bdy = mmx - ex_wx, mmy - ex_wy
                  if bdx * bdx + bdy * bdy <= 128 * 128 then
                    blast_ok = false
                    clh.lane_block = "own_builder_blast"
                  end
                end
              end
            end
            if lane_ok and blast_ok then
              taps = bit.bor(taps, KEY_SHOOT)
              _already_fired = true
              clh.verdict = "fire"
              -- Miss accounting: if the PREVIOUS shot at this man has finished its
              -- flight and he is still our target, that shot missed.  Kills reset
              -- the counter via the target-change reset above (he leaves the list).
              if refire.last_fire and now >= refire.last_fire + (refire.last_flight or 0) then
                refire.misses = (refire.misses or 0) + 1
              end
              refire.last_fire   = now
              refire.last_flight = kill_lgm.flight_ticks(sl_eff)
            end
          end
        end
        -- Miss-count visualizer (toggle "capture_lgm_misses"): the running number
        -- of consecutive missed capture-target shots at this man, drawn over the
        -- tank.  Resets on kill/leave (record dropped) or target change.  The cap
        -- is currently DISABLED, so this only reports -- it doesn't stop firing.
        if BRAIN_DEBUG_MODE and viz.is_on("capture_lgm_misses") and state._clh_refire then
          viz.text("capture_lgm_misses", info.tankx / 256.0, info.tanky / 256.0 + 1.2,
                   string.format("misses=%d", state._clh_refire.misses or 0),
                   "center", 255, 200, 80, 240, 0.5)
        end
        -- _already_fired may also be set by the shared kill-LGM evaluator above
        -- (its own 64-wu gate); either way the verdict is "fire".
        if _already_fired then clh.verdict = "fire" end
      else
        -- No LGM in the radius (armour-rise trigger): keep the crosshair ON THE
        -- PILL we're sweeping (greedy sight-over-pill), parked on the corpse and
        -- ready to snap onto a man the instant one enters the radius.
        local tsl = kill_lgm.sightlen_for(clh.dist_wu)
        if C.CAPTURE_LGM_HUNT_SIGHT_ON_PILL then
          local hk, tk = capture_sight_step(clh.pmx * 256 + 128, clh.pmy * 256 + 128)
          keys = bit.bor(keys, hk)
          taps = bit.bor(taps, tk)
        else
          keys = bit.bor(keys, kill_lgm.gunrange_key(info.gunrange or 14, tsl))
        end
        -- LANE CHECK for the pill-tile shot: no friendly / allied tank or
        -- builder within a tile of the shell's path or of the tile itself.
        -- The tile is a corpse (our shells cannot hurt it) but the shell
        -- explodes on whatever it meets first, and a team-mate scooping the
        -- same pile is exactly who tends to be standing there.
        local lane_ok = true
        do
          local sx, sy = info.tankx, info.tanky
          local ex, ey = clh.aim_wx, clh.aim_wy
          local vx, vy = ex - sx, ey - sy
          local vlen2 = vx * vx + vy * vy
          for _, ob in ipairs(info.objects or {}) do
            if (ob.type == OBJECT_TANK or ob.type == OBJECT_BUILDMAN)
               and (bit.band(ob.info, OBJECT_HOSTILE)) == 0
               and not (ob.type == OBJECT_TANK and ob.idnum == (info.player_number or -1)) then
              local t = 0
              if vlen2 > 0 then
                t = ((ob.x - sx) * vx + (ob.y - sy) * vy) / vlen2
                if t < 0 then t = 0 elseif t > 1 then t = 1 end
              end
              local px, py = sx + vx * t - ob.x, sy + vy * t - ob.y
              if px * px + py * py <= 256 * 256 then
                lane_ok = false
                clh.lane_block = ob.type == OBJECT_TANK and "ally_tank" or "friendly_builder"
                break
              end
            end
          end
          -- FIX 3b: our OWN builder is never in info.objects; test him too.
          if lane_ok then
            local mmx, mmy = own_man_out_pos()
            if mmx and seg_dist2(mmx, mmy, sx, sy, ex, ey) <= 256 * 256 then
              lane_ok = false
              clh.lane_block = "own_builder"
            end
          end
        end
        if not _no_shells and not _shoot_busy and not _already_fired
           and (bit.band(taps, KEY_SHOOT)) == 0
           and lane_ok
           and clh.dist_wu <= C.KILL_LGM_SHOOT_RANGE * 256 then
          -- Same impact-point gate the kill-LGM block uses (kill_lgm.tuning()).
          local _kt = kill_lgm.tuning()
          local cur_sl = info.gunrange or 14
          local rad    = (info.direction or 0) * C.TWO_PI / 256
          local ex_wx  = info.tankx + math.sin(rad) * 128 * cur_sl
          local ex_wy  = info.tanky - math.cos(rad) * 128 * cur_sl
          local ddx    = ex_wx - clh.aim_wx
          local ddy    = ex_wy - clh.aim_wy
          if math.sqrt(ddx * ddx + ddy * ddy) <= _kt.fire_wu then
            taps = bit.bor(taps, KEY_SHOOT)
            _already_fired = true
            clh.verdict = "fire"
          end
        end
      end
      -- Rate-limited decision line: one print per change of VERDICT or of the
      -- kill-LGM fire gate, not per tick.  The gate is the per-LGM verdict the
      -- block above wrote into _kill_lgm_eval (in range, LOS clear, crosshair
      -- on / off, fired); without it "verdict=turn and never fires" is
      -- unreadable, and that is the single most likely way this feature
      -- silently does half its job.  The whole thing lives inside the
      -- BRAIN_DEBUG_MODE guard so lua_strip takes the eval scan out of the
      -- production copy with the print.
      -- The candidate TRACK RADIUS, drawn around the tank on every hunt tick so
      -- the picture is the admission test the code just ran: a man inside this
      -- circle (and inside the pill's hunt radius) is trackable whatever way the
      -- nose points.  Nothing is drawn while the package is off, because then
      -- admission is the along-the-ray test and a circle would be a lie.
      if BRAIN_DEBUG_MODE then
        local _ktv = kill_lgm.tuning()
        if _ktv.track_wu then
          viz.circle("capture_lgm_track", info.tankx / 256.0, info.tanky / 256.0,
                     _ktv.track_wu / 256.0, 120, 200, 160, 90, false, false)
        end
      end
      if BRAIN_DEBUG_MODE then
        local gate = "-"
        if clh.lgm_id and state._kill_lgm_eval then
          for i = 1, #state._kill_lgm_eval do
            local ev = state._kill_lgm_eval[i]
            if ev.idnum == clh.lgm_id then gate = ev.status or "-"; break end
          end
        end
        local key = clh.steer .. "/" .. clh.verdict .. "/" .. gate
        if key ~= state._clh_verdict then
          state._clh_verdict = key
          print2(string.format(
            "CAPTURE_LGM_HUNT t=%d pill=(%d,%d) src=%s lgm=(%s,%s) pd=%s d=%.1f"
            .. " nav=%s aim=%d err=%s tol=%d uturn=%s gun=%d spd=%d slower=%d"
            .. " thr=%s steer=%s gate=%s verdict=%s aimerr=%s/%s dband=%s",
            now, clh.pmx, clh.pmy, clh.src,
            tostring(clh.lgm_mx), tostring(clh.lgm_my), tostring(clh.pill_d),
            clh.dist_wu / 256.0,
            tostring(clh.nav_dir), clh.aim_dir, tostring(clh.err), clh.tol,
            clh.uturn and 1 or 0,
            info.gunrange or 0, info.speed or 0,
            ((bit.band(keys, KEY_SLOWER)) ~= 0) and 1 or 0,
            tostring(clh.thr), clh.steer, gate, clh.verdict,
            clh.aim_err_wu and string.format("%.0f", clh.aim_err_wu) or "-",
            tostring(clh.aim_wu or "-"), tostring(clh.aim_dband or "-")))
        end
      end
    elseif state._clh_verdict ~= nil then
      -- Hunt over: forget the last verdict so re-entry prints again.
      state._clh_verdict = nil
      state._clh_refire = nil     -- reset the capture-target miss counter
      if BRAIN_DEBUG_MODE then
        print2(string.format("CAPTURE_LGM_HUNT t=%d verdict=off", now))
      end
    end
  end

  -- Post-sweep gunrange (C.CAPTURE_LGM_HUNT_SIGHT_ON_PILL): the instant the
  -- capture-LGM hunt stops driving the sight -- the pill is swept, or the hunt
  -- dropped -- snap the gunrange back to FULL, one notch/tick until maxed, so
  -- the crosshair returns to long range right away. (Andrew 2026-09-09.)
  if C.CAPTURE_LGM_HUNT_SIGHT_ON_PILL then
    local hunting_now = state._clh ~= nil and state._clh.tick == now
                        and not info.inboat
    if hunting_now then
      state._clh_extend = nil                 -- hunt drives the sight; not extending
    elseif state._clh_sighting then
      state._clh_extend = true                -- just stopped hunting -> go full
    end
    state._clh_sighting = hunting_now or nil
    -- YIELD (Fable-5 review): only drive the extend when NOTHING else set a
    -- range key this think, so we never override another goal's gunsight (a set
    -- MORERANGE wins outright at the packer; a LESSRANGE we'd stomp). The
    -- kill_lgm pre-charge/search both run earlier in the think, so this defers
    -- to them. The clamp at 14 clears the flag on its own once maxed. Not in a
    -- boat (the engine ignores it there anyway).
    if state._clh_extend and not info.inboat
       and bit.band(keys, bit.bor(KEY_MORERANGE, KEY_LESSRANGE)) == 0 then
      local k = kill_lgm.gunrange_key(info.gunrange or 14, 14)   -- step toward full
      if k ~= 0 then keys = bit.bor(keys, k) else state._clh_extend = nil end
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
      if bit.band(keys, KEY_FASTER)    ~= 0 then kstr = kstr .. "FAST " end
      if bit.band(keys, KEY_SLOWER)    ~= 0 then kstr = kstr .. "SLOW " end
      if bit.band(keys, KEY_TURNLEFT)  ~= 0 then kstr = kstr .. "LEFT " end
      if bit.band(keys, KEY_TURNRIGHT) ~= 0 then kstr = kstr .. "RIGHT " end
      if bit.band(keys, KEY_SHOOT)     ~= 0 then kstr = kstr .. "SHOOT " end
      if bit.band(taps, KEY_SHOOT)     ~= 0 then kstr = kstr .. "tap-SHOOT " end
      if bit.band(taps, KEY_TURNLEFT)  ~= 0 then kstr = kstr .. "tap-L " end
      if bit.band(taps, KEY_TURNRIGHT) ~= 0 then kstr = kstr .. "tap-R " end
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
      msg_dest = 0  -- bot-only internal channel (don't spam human chat with status)
    end
  else
    if now % 250 == 0 then
      local pf = state.pf
      local step = pf.next_mx >= 0
                   and string.format("(%d,%d)", pf.next_mx, pf.next_my)
                   or "nil"
      local kstr = ""
      if bit.band(keys, KEY_FASTER)    ~= 0 then kstr = kstr .. "F" end
      if bit.band(keys, KEY_SLOWER)    ~= 0 then kstr = kstr .. "S" end
      if bit.band(keys, KEY_TURNLEFT)  ~= 0 then kstr = kstr .. "L" end
      if bit.band(keys, KEY_TURNRIGHT) ~= 0 then kstr = kstr .. "R" end
      if bit.band(taps, KEY_SHOOT)     ~= 0 then kstr = kstr .. "!" end
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
  if BRAIN_PROFILE then
    opt(string.format("  oppshot+nav-debug done %.2f ms", (clock_us() - t_psv_inspect) / 1000))
  end

  -- Builder: set mode from current goal, then decide what to build/farm
  local t_build0 = clock_us()
  -- Draw persistent wsim kill/damage paths every tick
  if BRAIN_DEBUG_MODE then goals.draw_wsim_paths(state) end
  -- Draw attack_tank detection/precondition overlays (navy blue)
  if BRAIN_DEBUG_MODE then goals.draw_attack_tank_viz(state, info) end
  -- Builder-pool active job: tank->target line, target ring, ETA label, and
  -- the leash circle the discovery actually used.
  if BRAIN_DEBUG_MODE then bpool.draw(state, info) end

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

  -- Ghost tank overlay: where we GUESS an out-of-sight enemy tank is, and how
  -- long until we stop hunting it. Drawn faded/purple since it's a prediction.
  if BRAIN_DEBUG_MODE and viz.is_on("ghost_tank") and state.perc and state.perc.ghost_tanks then
    local twx = info.tankx / 256.0
    local twy = info.tanky / 256.0
    for _, gt in ipairs(state.perc.ghost_tanks) do
      local gx, gy = gt.mx + 0.5, gt.my + 0.5
      local ttl_s = (gt.ghost_ttl_left or 0) / 50.0
      -- Dashed-ish hollow box (two nested outlines) + a line from us to it.
      viz.rect("ghost_tank", gt.mx + 0.1, gt.my + 0.1, gt.mx + 0.9, gt.my + 0.9, 200, 80, 255, 180, false)
      viz.rect("ghost_tank", gt.mx + 0.25, gt.my + 0.25, gt.mx + 0.75, gt.my + 0.75, 200, 80, 255, 120, false)
      viz.line("ghost_tank", twx, twy, gx, gy, 200, 80, 255, 90)
      -- Heading hint: short vector along the extrapolation velocity.
      if (gt.vx or 0) ~= 0 or (gt.vy or 0) ~= 0 then
        local mag = math.sqrt(gt.vx * gt.vx + gt.vy * gt.vy)
        if mag > 0 then
          viz.line("ghost_tank", gx, gy, gx + gt.vx / mag * 1.2, gy + gt.vy / mag * 1.2, 255, 160, 255, 200)
        end
      end
      viz.text("ghost_tank", gx, gy - 0.7,
        string.format("GHOST p%d  %.1fs", gt.id or -1, ttl_s),
        "center", 220, 140, 255, 255, 0.45)
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
          local ty = __idiv(k, 256)
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
  if BRAIN_PROFILE then
    opt(string.format("post-steer viz done %.2f ms", (t_build0 - t_steer1) / 1000))
  end
  builder.set_mode(state, world, info, state.goal)
  local t_build_setmode = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  builder.set_mode done %.2f ms", (t_build_setmode - t_build0) / 1000))
  end
  -- Builder POOL, between set_mode and decide, in that order for a reason:
  -- set_mode is what publishes the mode and b.reserve_eta the eligibility
  -- stack reads, and decide()'s new rung only executes the winner this pass
  -- leaves behind. It runs EVERY tick, even when nothing can possibly be
  -- dispatched, because the panel has to be able to say why (the always-show
  -- rule the other strips follow) and because the job lifecycle + the tank's
  -- under-fire clock have to keep ticking while the man is out.
  bpool.update(state, world, info, now)
  local t_build_pool = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  builder_pool.update done %.2f ms",
      (t_build_pool - t_build_setmode) / 1000))
  end
  local build_cmd = builder.decide(state, world, info, now)
  -- Repair-pill completion: the builder dispatched the LGM onto a
  -- friendly damaged pill (engine auto-repairs on arrival). Clear the
  -- goal so this tick's downstream goal-tracking and next tick's
  -- pick_goal see a clean slate — repair runs autonomously from here.
  if state._repair_dispatched then
    state._repair_dispatched = nil
    attack.clear_attack_goal(state, "repair_pill dispatched to LGM")
    if BRAIN_DEBUG_MODE then
      print(string.format(TAG .. " t=%d REPAIR dispatched, clearing goal", now))
    end
  end
  local t_build1 = clock_us()
  if BRAIN_PROFILE then
    opt(string.format("  builder.decide done %.2f ms", (t_build1 - t_build_pool) / 1000))
  end
  metrics.set("us_builder", t_build1 - t_build0)
  if BRAIN_PROFILE then
    opt(string.format("builder done %.2f ms", (t_build1 - t_build0) / 1000))
  end
  -- Track what kind of action was most recently dispatched so steering can
  -- decide whether to pace the tank while the LGM is moving.
  if build_cmd and info.man_status == C.LGM_INTANK then
    state.builder.last_action = build_cmd.action
    -- LGM-dispatch record for the lgmd advert (/info extra): destination
    -- + terrain-based ETA, broadcast so allies can predict repair/build
    -- overlap (heat gate, dedup) without per-shell chatter. The repair
    -- branch stashes its precise walk-sim ETA; other dispatches estimate
    -- from tile distance x walk rate.
    if build_cmd.x and build_cmd.y then
      local dtx = bit.rshift(info.tankx, 8)
      local dty = bit.rshift(info.tanky, 8)
      local eta = state._repair_dispatch_eta
        or (U.mdist(dtx, dty, build_cmd.x, build_cmd.y)
            * (C.REPAIR_DEAD_GRASS_TICKS_PER_TILE or 16))
      state._lgm_dispatch = { x = build_cmd.x, y = build_cmd.y,
                              eta_tick = now + eta, tick = now }
      state._repair_dispatch_eta = nil
    end
    -- Placement trip flag. ONE record per placement dispatch (harvest or
    -- not), on state rather than the goal because the goal changes while the
    -- builder walks. Readers: eval_wait_for_lgm (suppress the park for the
    -- whole trip), builder.spacing_class (the tile counts as occupied), and
    -- the lifecycle block earlier in think() (return edge -> harvest resume
    -- or clear). A repair also sends BUILDMODE_PBOX, so key on the builder
    -- MODE, not the action; pill_place (the take's blocker drop) has its own
    -- substate machine and stays out.
    if build_cmd.action == BUILDMODE_PBOX and state.builder.mode == "place_pill"
       and build_cmd.x and build_cmd.y
       and not (state.goal and state.goal.kind == "pill_place") then
      local g = state.goal
      local on_goal = g and g.kind == "place_pill_strategic"
                        and g.mx == build_cmd.x and g.my == build_cmd.y
      local harvest = U.ttype(build_cmd.x, build_cmd.y) == C.T_FOREST
      state._place_trip = {
        mx = build_cmd.x, my = build_cmd.y, tick = now,
        harvest = harvest,
        -- Score only exists for a pool-picked strategic spot. A guard/panic
        -- drop on forest still harvests and resumes, but on the hard validity
        -- check alone -- there is no strategic score to hold it to.
        score_at_dispatch = on_goal and g._spot_score or nil,
        sc7_at_dispatch   = on_goal and g._spot_sc7 or nil,
        -- Where the tank stood when the builder left. The harvest re-score is
        -- asked FROM here (goals.score_place_tile's tank_mx/tank_my), so the
        -- margin measures the tile changing, not the tank moving.
        tank_mx = bit.rshift(info.tankx, 8),
        tank_my = bit.rshift(info.tanky, 8),
        origin = (state.builder.place_resume and "resume")
              or ((g and g._place_forced) and "forced")
              or (on_goal and "strategic" or "guard"),
        left = false,   -- set once the LGM is seen out of the tank
      }
      if state.builder.place_resume then state._place_resume = nil end
      -- One line per dispatch, on the edge (DISPATCH above repeats every tick
      -- the mode holds; this is the line to count).
      print2(string.format("%s t=%d tile=(%d,%d) score=%s origin=%s carried=%d trees=%d",
        harvest and "HARVEST_SET" or "PLACE_TRIP_SET",
        now, build_cmd.x, build_cmd.y,
        state._place_trip.score_at_dispatch
          and string.format("%.1f", state._place_trip.score_at_dispatch) or "none",
        state._place_trip.origin, info.carried_pills or 0, info.trees or 0))
    end
  end

  local t_pbh_start = t_build1
  -- LGM state overlays (connected to actual decision state)
  if BRAIN_DEBUG_MODE then
    if info.man_status ~= C.LGM_INTANK then
      local ly = 70
      -- LGM position marker
      local man_mx = bit.rshift(info.man_x, 8)
      local man_my = bit.rshift(info.man_y, 8)
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
      local ntt = U.ttype_peek(state.pf.next_mx, state.pf.next_my)
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
  local t_pbh_lgm = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  LGM/stuck HUD done %.2f ms", (t_pbh_lgm - t_pbh_start) / 1000))
  end

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

  local t_pbh_blocked = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  blocked-tiles viz done %.2f ms", (t_pbh_blocked - t_pbh_lgm) / 1000))
  end

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

  local t_pbh_repos = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  pill-reposition viz done %.2f ms", (t_pbh_repos - t_pbh_blocked) / 1000))
  end

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

  local t_pbh_bait = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  bait-pill viz done %.2f ms", (t_pbh_bait - t_pbh_repos) / 1000))
  end

  -- Friendly pill barrier overlay: mark friendly pills used as shields
  if BRAIN_DEBUG_MODE and state.goal and (state.goal.kind == "attack_pill" or state.goal.kind == "attack_pill") and viz.is_on("friendly_pill_shield") then
    local gmx, gmy = state.goal.mx, state.goal.my
    local smx = state.goal.standoff_mx or (bit.rshift(info.tankx, 8))
    local smy = state.goal.standoff_my or (bit.rshift(info.tanky, 8))
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

  local t_pbh_barrier = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  friendly-pill-barrier viz done %.2f ms", (t_pbh_barrier - t_pbh_bait) / 1000))
  end

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
            now - s.tick, s.wx, s.wy, bit.rshift(s.wx, 8), bit.rshift(s.wy, 8), delta_str))
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
        -- Rings sized from the live tuning, not a magic 0.45: outer = the fire
        -- gate we shoot at (kill_lgm.tuning().fire_wu), inner = what the engine
        -- actually kills inside (engine_kill_wu, lgm.c MAP_SQUARE_MIDDLE).
        local _kt = kill_lgm.tuning()
        viz.circle("kill_lgm_engage", sx, sy, _kt.fire_wu / 256.0,
                   cr, cg, cb, 230)
        viz.circle("kill_lgm_engage", sx, sy, _kt.engine_kill_wu / 256.0,
                   cr, cg, cb, 150)
        if _kt.aim_wu then                      -- innermost: what the aim is for
          viz.circle("kill_lgm_engage", sx, sy, _kt.aim_wu / 256.0,
                     cr, cg, cb, 110)
        end
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
                        and string.format(" off=%.0f/%d", ev.land_off_wu,
                                          kill_lgm.tuning().fire_wu)
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
          -- The predicted point IS the centre of the fire gate, so draw the
          -- gate here: outer ring = C.LGM_KILL_FIRE_WU (the impact offset we
          -- will take a shot at), inner ring = C.LGM_ENGINE_KILL_WU (what the
          -- engine actually kills inside, lgm.c MAP_SQUARE_MIDDLE = 128 wu).
          viz.circle("kill_lgm_predict", px, py, kill_lgm.tuning().fire_wu / 256.0,
                     cr, cg, cb, 220)
          viz.circle("kill_lgm_predict", px, py, kill_lgm.tuning().engine_kill_wu / 256.0,
                     cr, cg, cb, 150)
          if kill_lgm.tuning().aim_wu then      -- innermost: the aim target
            viz.circle("kill_lgm_predict", px, py,
                       kill_lgm.tuning().aim_wu / 256.0, cr, cg, cb, 110)
          end
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
        if (bit.band(s.keys, KEY_TURNLEFT))  ~= 0 then parts[#parts + 1] = "L"     end
        if (bit.band(s.keys, KEY_TURNRIGHT)) ~= 0 then parts[#parts + 1] = "R"     end
        if (bit.band(s.keys, KEY_FASTER))    ~= 0 then parts[#parts + 1] = "FWD"   end
        if (bit.band(s.keys, KEY_SLOWER))    ~= 0 then parts[#parts + 1] = "BACK"  end
        if (bit.band(s.keys, KEY_MORERANGE)) ~= 0 then parts[#parts + 1] = "GUN+"  end
        if (bit.band(s.keys, KEY_LESSRANGE)) ~= 0 then parts[#parts + 1] = "GUN-"  end
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


  local t_pbh_ally = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  ally-LGM viz done %.2f ms", (t_pbh_ally - t_pbh_barrier) / 1000))
  end

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
    reposition_vote.draw(state, world, info, state.tick)
    squad.draw_roster(state, info, state.tick)
    squad.draw_labels(state, info, state.tick)
    squad.draw_blitz(state, info, state.tick)
    squad.draw_help_range(state, info, state.tick, world)
    squad.draw_blitz_call(state, info, state.tick)
    squad.draw_blitz_roster(state, info, state.tick)
    squad.draw_blitz_wait_timeout(state, info, state.tick)
    squad.draw_blitz_comm(state, info, world, state.tick)
    squad.draw_blitz_joinable(state, info, world, state.tick)
    squad.draw_blitz_join_highlight(state, info, state.tick)
    -- Negotiating soldier's full standoff scan (ellipse scores + bucket): drawn
    -- while we're offering a blitz standoff, so the score breakdown that picked
    -- the offer is inspectable. Stashed by attack.blitz_negotiate.
    if state._blitz_negotiate_scan and viz.is_on("blitz_negotiate_scan") then
      local bns = state._blitz_negotiate_scan
      attack.draw_pill_eval_spots(bns.spots, bns.mx, bns.my, "blitz_negotiate_scan", "all", bns.best_deg)
      -- Label which pill (and commander) this negotiation is for, above the pill.
      viz.text("blitz_negotiate_scan", bns.mx + 0.5, bns.my - 0.8,
               string.format("NEG pill#%s -> C%s", tostring(bns.pill), tostring(bns.cmdr)),
               "center", 255, 220, 120, 255)
    end
    circles.draw(viz, state.tick, world)
    circles.draw_hud(viz, state.tick)
    circles.draw_history(viz, state.tick)
    circles.draw_warnings(viz, state.tick)
    squad.draw_roles_live(state, info, state.tick)
    squad.draw_hard_takes(world, state.tick)
    PP.draw_roles(viz, world, state.tick)
    PP.draw_front_band(viz)
    -- reinforce_link: line from us to our target losing-circle safe tile.
    if viz.is_on("reinforce_link") and state._reinforce_viz and viz.line then
      local rv = state._reinforce_viz
      local rtx, rty = (bit.rshift(info.tankx, 8)) + 0.5, (bit.rshift(info.tanky, 8)) + 0.5
      viz.line("reinforce_link", rtx, rty, rv.mx + 0.5, rv.my + 0.5, 120, 200, 255, 220)
      if viz.text then viz.text("reinforce_link", rtx, rty - 1.0, "REINFORCING #" .. rv.id, "center", 120, 200, 255, 235) end
    end
    -- repos_claims: pills an ally is repositioning (this bot honors the claim).
    if viz.is_on("repos_claims") and viz.rect and ally_state.iter_active then
      for apn, slot in ally_state.iter_active(state.tick, 1750) do
        if apn ~= info.player_number and slot.info and slot.info.repos == "1" then
          local amx, amy = tonumber(slot.info.mx), tonumber(slot.info.my)
          if amx and amy then
            viz.rect("repos_claims", amx, amy, amx + 1, amy + 1, 255, 120, 255, 120)
            if viz.text then viz.text("repos_claims", amx + 0.5, amy - 0.6, "claim p" .. apn, "center", 255, 120, 255, 220) end
          end
        end
      end
    end
    pill_table.draw(viz, world, state, info)
    goals.draw_pill_spots(viz, state)
    goals.draw_take_cover(viz, state)
    goals.draw_sea_harvest(viz, state)
    goals.draw_build_viz(viz, state, info)
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
  local t_pbh_ally_state = BRAIN_PROFILE and clock_us() or 0
  if BRAIN_PROFILE then
    opt(string.format("  ally-state overlay done %.2f ms", (t_pbh_ally_state - t_pbh_ally) / 1000))
  end

  -- Log this tick
  log.log_tick(state, info, state.goal, keys, taps, build_cmd)
  if BRAIN_PROFILE then
    opt(string.format("  log.log_tick done %.2f ms", (clock_us() - t_pbh_ally) / 1000))
  end

  -- Total tick time and worst-case tracking
  local t_end = clock_us()
  local us_total = t_end - t0
  metrics.set("us_post_build_hud", t_end - t_build1)
  if BRAIN_PROFILE then
    opt(string.format("post-build HUD done %.2f ms", (t_end - t_build1) / 1000))
  end
  -- Anchor for the (tail) timer: real clock immediately after the last
  -- named main-section emit. Anything between here and opt.flush() is
  -- attributed to (tail) so the named-section sum equals the actual
  -- tick wall-clock with no gap.
  local _t_tail_anchor = clock_us()
  if BRAIN_PROFILE then
    opt(string.format("TICK TOTAL %.2f ms", us_total / 1000))
  end
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
    local t_label0 = BRAIN_PROFILE and clock_us() or 0
    for id, p in pairs(world.pills) do
      viz.text("pill_id_label", p.mx + 0.5, p.my + 0.5, tostring(id), "center", 0, 0, 200, 255)
    end
    for id, b in pairs(world.bases) do
      viz.text("pill_id_label", b.mx + 0.5, b.my + 0.5, tostring(id), "center", 0, 255, 255, 255, 2.4)
    end
    if BRAIN_PROFILE then
      opt(string.format("  pill+base id labels done %.2f ms", (clock_us() - t_label0) / 1000))
    end
  end

  -- Defend-pill tier overlay: ring + "tier cost" label per team pill from
  -- the last replan's defend breakdown (state.defend_breakdown rows carry
  -- the tier that priced each pill), plus the Euclidean
  -- DEFEND_ARRIVE_RADIUS circle on the ACTIVE defend goal's pill — the
  -- exact boundary where eval_defend_pill hands the travel phase to the
  -- heat gate. Colors mirror the tier ladder.
  if BRAIN_DEBUG_MODE and viz.is_on("defend_pill_viz") and state.defend_breakdown then
    local DEFEND_TIER_COLS = {
      siege   = { 255,  60,  60 },  setup = { 255, 150,   0 },
      sight   = { 255, 230,   0 },  worn  = {  90, 140, 255 },
      quiet   = { 150, 150, 150 },  heat  = {   0, 230,  80 },
      no_heat = {  90,  90,  90 },  dead  = {  60,  60,  60 },
      -- The ARRIVED rungs the table used to miss and silently render as grey
      -- "quiet": WATCH (hold beside it), REPAIR (fix it -- the new rung), and
      -- the well-defended clamp. eval_defend_pill emits all three as tiers.
      repair  = {   0, 220, 220 },  watch = {   0, 150, 220 },
      welldef = { 130, 200, 130 },
    }
    for _, r in ipairs(state.defend_breakdown.rows or {}) do
      local cc = DEFEND_TIER_COLS[r.tier or "quiet"] or DEFEND_TIER_COLS.quiet
      viz.circle("defend_pill_viz", r.mx + 0.5, r.my + 0.5, 0.65,
                 cc[1], cc[2], cc[3], 200)
      local lbl = (r.cost and r.cost < 1e29)
        and string.format("%s %.0f", r.tier or "?", r.cost)
        or (r.tier or "?")
      viz.text("defend_pill_viz", r.mx + 0.5, r.my - 0.35, lbl,
               "center", cc[1], cc[2], cc[3], 255)
    end
    if state.goal and state.goal.kind == "defend_pill" and state.goal.mx then
      viz.circle("defend_pill_viz", state.goal.mx + 0.5, state.goal.my + 0.5,
                 (C.DEFEND_ARRIVE_RADIUS or 10), 0, 200, 255, 120)
    end
  end

  -- ALARM MODE overlay (2026-09-06): the shape of the actual algorithm, not a
  -- decoration -- condition 1's DEFEND_ALARM_ENEMY_TILES ring, condition 3's
  -- DEFEND_ALARM_MIN_DIST ring, the WATCH LIST membership, the
  -- DEFEND_ALARM_BUILD_RADIUS stamp perception.lua actually sweeps, and the
  -- newest build it found.  Purely a reader: no terrain is touched (the stamp
  -- geometry is arithmetic), so this cannot move the recorded brain off the
  -- production one.  Draws nothing in keel mode, where there is no alarm.
  -- (one line: lua_strip deletes the LINE that opens an `if BRAIN_DEBUG_MODE`
  -- block, so a wrapped condition leaves a dangling `and ... then` in opt/.)
  if BRAIN_DEBUG_MODE and C.DEFEND_ALARM_MODE and viz.is_on("defend_alarm_viz") then
    local ALARM_COLS = {
      alarm     = { 255,  60,  60 },
      alarm_off = { 120, 120, 120 },
      dead      = {  60,  60,  60 },
    }
    local by_tile = {}
    for _, r in ipairs((state.defend_breakdown and state.defend_breakdown.rows) or {}) do
      by_tile[r.my * 256 + r.mx] = r
    end
    local er   = C.DEFEND_ALARM_ENEMY_TILES or 11
    local mind = C.DEFEND_ALARM_MIN_DIST or 9
    local br   = C.DEFEND_ALARM_BUILD_RADIUS or 4
    local win  = C.DEFEND_ALARM_WINDOW_TICKS or 250
    for _, p in pairs(world.pills) do
      if (p.owner == "friendly" or p.owner == "allied")
         and not (p.in_tank or p.carrier or p._synth_carry) then
        local row = by_tile[p.my * 256 + p.mx]
        local cc  = ALARM_COLS[(row and row.tier) or "alarm_off"]
                    or ALARM_COLS.alarm_off
        -- Condition 1 ring (enemy must be visible INSIDE this right now) and
        -- condition 3 ring (we must be OUTSIDE this).
        viz.circle("defend_alarm_viz", p.mx + 0.5, p.my + 0.5, er,
                   cc[1], cc[2], cc[3], 110)
        viz.circle("defend_alarm_viz", p.mx + 0.5, p.my + 0.5, mind,
                   90, 160, 255, 90)
        local watched = p._alarm_watch_tick == now
        if watched then
          -- The build stamp, tile by tile: the same euclidean disc
          -- perception.lua's precomputed offset list sweeps.
          for dy = -br, br do
            for dx = -br, br do
              if dx * dx + dy * dy <= br * br then
                viz.rect("defend_alarm_viz", p.mx + dx + 0.15, p.my + dy + 0.15,
                         p.mx + dx + 0.85, p.my + dy + 0.85,
                         255, 190, 0, 45, false)
              end
            end
          end
        end
        local b = p._alarm_build
        if b and (now - (b.t or 0)) <= win then
          viz.rect("defend_alarm_viz", b.mx + 0.05, b.my + 0.05,
                   b.mx + 0.95, b.my + 0.95, 255, 80, 255, 200, false)
          viz.text("defend_alarm_viz", b.mx + 0.5, b.my + 1.1,
                   string.format("%s %dt", tostring(b.what), now - b.t),
                   "center", 255, 80, 255, 255)
        end
        local lbl
        if row and row.tier == "alarm" then
          lbl = string.format("ALARM %.0f", row.cost or 0)
        elseif row and row.reject then
          lbl = tostring(row.reject)
        else
          lbl = "(no row yet)"
        end
        viz.text("defend_alarm_viz", p.mx + 0.5, p.my - 0.9,
                 lbl .. (watched and " [watched]" or ""),
                 "center", cc[1], cc[2], cc[3], 255)
      end
    end
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
    arrow(40, 80, "^", (bit.band(keys, KEY_FASTER))    ~= 0, (bit.band(taps, KEY_FASTER))    ~= 0)
    arrow(60, 60, "<", (bit.band(keys, KEY_TURNLEFT))  ~= 0, (bit.band(taps, KEY_TURNLEFT))  ~= 0)
    arrow(40, 60, "v", (bit.band(keys, KEY_SLOWER))    ~= 0, (bit.band(taps, KEY_SLOWER))    ~= 0)
    arrow(20, 60, ">", (bit.band(keys, KEY_TURNRIGHT)) ~= 0, (bit.band(taps, KEY_TURNRIGHT)) ~= 0)
    -- Gunrange (crosshair) keys, second row directly below the cross.
    -- "−" = LESSRANGE (retract / pull crosshair in toward tank)
    -- "+" = MORERANGE (expand / push crosshair out toward max)
    -- Same color scheme as the arrow keys: green=held, yellow=tap,
    -- dim grey when not pressed.
    arrow(50, 40, "-", (bit.band(keys, KEY_LESSRANGE)) ~= 0, (bit.band(taps, KEY_LESSRANGE)) ~= 0)
    arrow(30, 40, "+", (bit.band(keys, KEY_MORERANGE)) ~= 0, (bit.band(taps, KEY_MORERANGE)) ~= 0)
  end end -- BRAIN_DEBUG_MODE (arrow HUD)

  -- End-of-decision marker. NOTE: the per-tick print2.flush() is deliberately
  -- NOT here — the /info broadcast block (blitz bco/bcc/bcq TX diagnostics,
  -- state slate) runs ~300 lines below this point, so flushing here would drop
  -- every print2 emitted after it (they'd sit in the buffer and get cleared by
  -- next tick's set_tick). The flush lives just before `return` instead.
  if BRAIN_DEBUG_MODE then
    print2("END state.goal.kind = ", state.goal.kind, ", state.goal.substate = ", tostring(state.goal.substate))
  end
  -- DECOY WATCH: an ammo-deprived decoy must attack a pill ONLY as part of a
  -- blitz (joining the captain's call). A solo attack_pill (no blitz) is a
  -- test failure — flag it with its full blitz-membership state so we can see
  -- WHY it went solo (no call open? not accepted? _blitz not set?).
  if BRAIN_DEBUG_MODE and state.ammo_deprived and state.goal and state.goal.kind == "attack_pill" then
    local g = state.goal
    local pid = g.target_id
    local call_for_pill, ncalls = false, 0
    if state.blitz_calls then for _, bc in pairs(state.blitz_calls) do ncalls = ncalls + 1; if bc.pill == pid then call_for_pill = true end end end
    local joining = (state.squad_blitz_accepted and true) or (g._blitz and true) or (state.squad_blitz_target == pid) or call_for_pill
    print2(string.format("DECOY_ATTACK t=%d pill#%s sub=%s joining=%s | accepted=%s _blitz=%s sqbt=%s call4pill=%s ncalls=%d role=%s", state.tick or 0, tostring(pid), tostring(g.substate), tostring(joining), tostring(state.squad_blitz_accepted), tostring(g._blitz), tostring(state.squad_blitz_target), tostring(call_for_pill), ncalls, tostring(state.squad_role)))
    if not joining then print2(string.format("DECOY_SOLO_FAIL t=%d pill#%s sub=%s — attacking pill with NO blitz", state.tick or 0, tostring(pid), tostring(g.substate))) end
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
  if BRAIN_PROFILE then
    opt(string.format("(tail) done %.2f ms", (clock_us() - _t_tail_anchor) / 1000))
  end
  if BRAIN_PROFILE then
    opt(string.format("END tick=%d total=%.2f ms", now, (clock_us() - t_tick_start) / 1000))
  end
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
  -- on silent print2 failures. Batching is wall-clock based (handoff
  -- every print2 FLUSH_INTERVAL_S seconds), so reason in seconds, not
  -- ticks: only crash if a sync-path write actually failed, or if no
  -- handoff has happened for far longer than the interval (a real stall).
  if _G._PRINT2_ENABLED and (now % 50) == 0 and now > 10 then
    local d = print2.diagnostic()
    -- Allow up to 6x the flush interval before declaring a stall, so a
    -- slow-but-healthy run (or a tick that just missed the boundary)
    -- never trips it. A genuine stall (writer wedged, never draining)
    -- still surfaces within ~30s.
    local stall_limit = (d.flush_interval_s or 5) * 6
    if d.consecutive_failures and d.consecutive_failures > 0 then
      local msg = string.format(TAG ..
        " [print2 watchdog] FATAL tick=%d: %d consecutive write failures (fails=%d open=%s)",
        now, d.consecutive_failures, d.fail_count, tostring(d.file_open))
      print(msg)
      error(msg, 0)
    elseif (d.seconds_since_handoff or 0) > stall_limit then
      local msg = string.format(TAG ..
        " [print2 watchdog] FATAL tick=%d: no flush for %ds (limit %ds, pending=%d open=%s)",
        now, math.floor(d.seconds_since_handoff), stall_limit,
        d.pending_blocks or -1, tostring(d.file_open))
      print(msg)
      error(msg, 0)
    end
  end

  -- Startup hold: give the incremental goal eval queue a chance to warm
  -- up before we commit to moving. Without this the bot picks a goal from
  -- a near-empty cache on tick 1 and often flips direction a few ticks
  -- later once better candidates finish evaluating.
  -- `age`, not `now`: the warm-up is per brain instance. A bot created
  -- mid-game (a Survival wave) has a cold eval queue exactly like a
  -- game-start bot and needs the same hold.
  if age <= C.STARTUP_HOLD_TICKS then
    keys, taps, build_cmd = 0, 0, -1
  end

  -- TEST AID: global bot freeze (BOT_TEST_GLOBAL_HOLD_TICKS, 0 = off).
  -- Outputs zeroed while brains keep ticking, so the human can capture the
  -- map before the bots go live. Bot 0 prints a console countdown every 10s.
  if now <= (C.BOT_TEST_GLOBAL_HOLD_TICKS or 0) then
    keys, taps, build_cmd = 0, 0, -1
    if now % 500 == 0 and (info.player_number or 0) == 0 then
      print(string.format("[BOT-HOLD] bots frozen — live in %ds", math.floor((C.BOT_TEST_GLOBAL_HOLD_TICKS - now) / 50)))
    end
  elseif now == (C.BOT_TEST_GLOBAL_HOLD_TICKS or 0) + 1
         and (info.player_number or 0) == 0 then
    print("[BOT-HOLD] bots are LIVE")
  end

  -- TEST AID banner: the followed bot is a never-refuel test bot (all
  -- refuel candidates rejected). Loud red so nobody mistakes a test run
  -- for real behavior.
  if state.test_never_refuel then
    -- y=70: clear of hud_replan at (10,56) and hud_tick_info at (8,32/44).
    viz.hud_text("test_no_refuel", 10, 70, "TEST: NEVER-REFUEL BOT", "topleft", 255, 60, 60)
  end

  -- Ammoless-helper countdown: ticks until this bot flips to
  -- state.ammo_deprived. Mirrors strategy.lua's clock — starts at
  -- state.ammo_low_since (shells below AMMO_DEPRIVED_SHELLS, non-opening),
  -- fires at AMMO_DEPRIVED_TICKS elapsed (50 ticks/s). hud_text/hud_rect
  -- self-gate on the toggle, but wrap in is_on to skip the string.format
  -- when the overlay is off.
  if viz.is_on("ammo_deprive_countdown") then
    local full = state.test_deprive_ticks or C.AMMO_DEPRIVED_TICKS or 3000
    local txt, r, g, b, frac
    if state.ammo_deprived then
      txt = "AMMOLESS HELPER: ACTIVE"
      r, g, b, frac = 255, 60, 60, 1.0
    elseif state.ammo_low_since then
      local elapsed = state.tick - state.ammo_low_since
      local remain = full - elapsed
      if remain < 0 then remain = 0 end
      frac = elapsed / full
      if frac > 1 then frac = 1 end
      txt = string.format("Ammoless helper in %.1fs (%dt, shells<%d)",
                          remain / 50, remain, C.AMMO_DEPRIVED_SHELLS or 0)
      r, g, b = 255, 180, 40
    else
      -- Clock is nil. Per strategy.lua it's pinned to nil when EITHER shells
      -- recovered to the line OR we're in the opening phase. Distinguish them:
      -- a dry tank in opening is NOT "ammo ok" — the clock is gated, so say so.
      local sh = (info and info.shells) or 0
      if sh < (C.AMMO_DEPRIVED_SHELLS or 0) then
        txt = string.format("Helper clock GATED — %s phase (shells=%d)",
                            state.phase or "?", sh)
        r, g, b, frac = 90, 170, 255, 0.0    -- blue: low ammo, but suppressed
      else
        txt = string.format("Helper clock idle — ammo ok (shells=%d)", sh)
        r, g, b, frac = 130, 130, 130, 0.0   -- grey: genuinely not depriving
      end
    end
    viz.hud_text("ammo_deprive_countdown", 10, 84, txt, "topleft", r, g, b)
    -- Fill bar under the text: grey track + coloured elapsed/total fill.
    local bx, by, bw, bh = 10, 98, 160, 6
    viz.hud_rect("ammo_deprive_countdown", bx, by, bw, bh, "topleft", 60, 60, 60, 255, true)
    if frac and frac > 0 then
      viz.hud_rect("ammo_deprive_countdown", bx, by,
                   math.floor(bw * frac + 0.5), bh, "topleft", r, g, b, 255, true)
    end
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
    if state.last_broadcast_state_tick == nil  then state.last_broadcast_state_tick  = now end  -- `now`, not 0: 0 reads as "1500+ ticks overdue" once ticks are engine-seeded
    local bsi = state.broadcast_state_info
    for k in pairs(bsi) do bsi[k] = nil end
    -- Squad role (Phase 1): recompute deterministically + broadcast so allies
    -- can render the roster and (later) drive recruitment.
    squad.update(state, info, now, world)
    -- Parallel blitz-soldier standoff offer (pre-commit; runs after squad.update
    -- has chosen which commander we're negotiating with). Does not change our goal.
    attack.blitz_negotiate(state, world, info, now)
    if state.squad_role then bsi.role = state.squad_role end
    if state.is_harasser then bsi.har = "1" end   -- harasser flag (decoupled from role)
    if state.is_pill_suicider then bsi.psu = "1" end  -- pill_suicider flag (same slate as har, map-selected or forced per-bot via BRAIN_INIT_ARG; mutually exclusive with it)
    if state.squad_cmdr then bsi.cmdr = tostring(state.squad_cmdr) end
    if state.squad_status and state.squad_status ~= "-" then bsi.sqst = state.squad_status end
    -- Blitz engage standoff claim (`be`): a soldier broadcasts its claimed
    -- blitz engage spot; a commander on an attack_pill broadcasts its own
    -- standoff so squadmates avoid it. `rdy=1` once a soldier is in position.
    do
      local g = state.goal
      -- Broadcast our offered/claimed standoff (bes) when committed to a blitz
      -- (g._blitz) OR while still NEGOTIATING one (squad_negotiate_cmdr set by
      -- blitz_negotiate). The latter lets the commander's roster show a 'maybe'
      -- soldier's live standoff (and watch it move as the soldier repicks),
      -- instead of a blank "-".
      -- Standoff (bes) travels as FLOAT tile coords, 4dp, so spot positions and
      -- the commander's spot de-confliction keep sub-tile precision. Soldier =
      -- its engage tile CENTER; commander = its precise float standoff (standoff_fx
      -- /fy), falling back to the tile center.
      if state.squad_blitz_engage_mx and state.squad_blitz_engage_my
         and ((g and g._blitz) or state.squad_negotiate_cmdr) then
        bsi.bes = string.format("%.4f,%.4f",
                    state.squad_blitz_engage_mx + 0.5, state.squad_blitz_engage_my + 0.5)
        if g and g._blitz and g.substate == "blitz_wait" and state.squad_blitz_aimed then bsi.rdy = "1" end
      elseif state.squad_role == "c" and g and g.kind == "attack_pill"
             and (g.standoff_fx or g.standoff_mx) then
        bsi.bes = string.format("%.4f,%.4f",
                    g.standoff_fx or (g.standoff_mx + 0.5),
                    g.standoff_fy or (g.standoff_my + 0.5))
      end
      -- Commander GO signal. Tied to the live goal (_blitz_go) so it self-clears
      -- when the take ends — a stale GO can't trigger the next blitz. _blitz_go
      -- is set at commit time for an overwhelm charge (rush together), but
      -- DEFERRED to shoot_pill entry for a shielded PPT commander (the slow
      -- aim→in_range route would otherwise launch the soldier far too early).
      if state.squad_role == "c" and g and g._blitz and g._blitz_go then
        bsi.bgo = "1"
      end
      -- blitz: reported walk distance to our chosen standoff (bd) so the
      -- commander can arbitrate conflicts; commander's reject list (brj).
      -- Only advertise bd while STILL negotiating. Once committed
      -- (squad_blitz_accepted), bd is the negotiation-offer signal: allies read
      -- cmdr+bd as "still negotiating" (roster + the commander's arbiter), so a
      -- committed soldier that keeps sending bd is seen as never having joined.
      -- Dropping bd (cmdr stays set) flips us to "committed" — and the slate
      -- change forces an immediate re-send instead of waiting on the heartbeat.
      if state.squad_blitz_bd and not state.squad_blitz_accepted then bsi.bd = tostring(state.squad_blitz_bd) end
      -- NOTE: brj / bac are NO LONGER bundled here — they used to push the state
      -- message past the 128-byte chat cap (so it was dropped and the soldier never
      -- saw its reject/accept). They now ship on their own short /info brj|bac verbs
      -- (re-sent every tick while pending for reliable, fast delivery) — see the
      -- handshake block in the /info send section below.
      -- blitz "where are you?" query: a commander ~1s from giving up in blitz_wait
      -- asks an out-of-sight pending soldier for a fresh position (bwq = pill id),
      -- latched for the query window. The soldier force-refreshes its bd in reply.
      if state.squad_role == "c" and state._blitz_query_pid
         and now <= (state._blitz_query_until or 0) then
        bsi.bwq = tostring(state._blitz_query_pid)
      end
    end
    if state.goal and state.goal.kind and state.goal.kind ~= "none" then
      bsi.goal = state.goal.kind
      -- Resource-need flag: below either COMBAT line (armour/shells 30).
      -- Receivers use it to resolve refuel claims: a needy tank beats a
      -- topping-off one for a base regardless of who is closer, so a
      -- close-but-full tank cannot hog the stock (stacked-bot games).
      if (info.armour or 0) < (C.ARMOUR_COMBAT or 30)
         or (info.shells or 0) < (C.SHELLS_COMBAT or 30) then
        bsi.low = "1"
      end
      if state.goal.substate and state.goal.substate ~= "" then
        bsi.sub = state.goal.substate
      end
      if state.goal.target_id and state.goal.target_id >= 0 then
        bsi.target = tostring(state.goal.target_id)
      end
      -- Reposition marker: tells the team someone is repositioning a pill, so
      -- everyone resets the time-based reposition discount (don't pile on).
      -- APPROVED/in-flight moves only (reposition_vote.is_reposition_active): a
      -- merely-bidding goal is not a reposition, and advertising it self-vetoed
      -- our own proposal — the bid's repos=1 and the /info rvo ride the same
      -- packet, so every voter stamped its recent-memory and NO'd us as
      -- recent_repo before the ballot was cast.
      if reposition_vote.is_reposition_active(state, world) then
        bsi.repos = "1"
      end
      -- Blocker TILES: ships on its OWN /info pblk verb (NOT the state slate) —
      -- it's a variable-length tile list that was the main thing pushing /info
      -- state past the 128-byte cap. Built into _pblk_payload here; sent in the
      -- /info send section below (on change), parsed via set_handshake on receive.
      -- Goal tile mx/my: only broadcast when something actually reads it.
      -- The de-conflict matcher (goals.lua) keys off `target` whenever an
      -- object id is present and only falls back to mx/my for tile-targeted
      -- goals with no target_id.  The ally-demolish path (init.lua ~2961)
      -- reads mx/my but is gated on repos=="1" (capture_pill reposition).
      -- For every other goal — capture_base, attack_pill, capture_pill
      -- non-repos — `target` covers it, so mx/my would never be consulted.
      -- Keyed off the flag we actually sent, not the goal shape: with repos
      -- gated on an in-flight move, an unapproved reposition BID has no reader
      -- for mx/my and the bytes would be wasted against the 128-byte cap.
      local _need_mxmy = not (state.goal.target_id and state.goal.target_id >= 0)
                         or bsi.repos == "1"
      if _need_mxmy and state.goal.mx and state.goal.my then
        bsi.mx = tostring(state.goal.mx)
        bsi.my = tostring(state.goal.my)
      end
    end

    -- Fresh-kill pickup claim (kg=pill id, kc=our capture_pill score for it).
    -- Advertised only while a claim is live (rare, ~5 s windows), so the churn
    -- is bounded. Lets blitz members hand the pickup to the LOWEST-score one
    -- (goals.lua Override 3b handoff reads these off ally_state), and every
    -- other bot treat the pill as claimed (kg → _kill_claimed reject).
    -- Unreachable/gone → large sentinel so we lose to any abler claimer.
    -- Only advertise a claim we're ACTUALLY pursuing (_grabbing set by Override
    -- 3b). A yielded/backup claim stays local so we can re-grab if the chosen
    -- grabber fails, but broadcasting it would make others defer to a bot that
    -- isn't going for the pill (it wandered off to refuel) — leaving the pill
    -- unclaimed. Gate the broadcast, not the claim.
    if state.kill_pickup and state.kill_pickup._grabbing then
      local kp = state.kill_pickup
      local p  = world.pills[kp.id]
      bsi.kg = tostring(kp.id)
      local kc = goals.kill_pickup_score(state, world, info, p)
      if not kc or kc >= 1e29 then kc = 1e9 end
      bsi.kc = string.format("%.0f", kc)
    end

    -- Carried-pill advertisement (carry=id,id,...). Broadcast REGARDLESS of
    -- goal: in-tank pills aren't in the C pill scan and the EVENT_PILL_UPDATE
    -- that sets in_tank is view-gated, so allies out of view never learn the
    -- team holds these. We know our own carried ids first-hand (we stood on the
    -- pill at pickup), so we advertise them; receivers fold them into world.pills
    -- (W.sync_ally_carried) so OUR PILLS / portfolio stays consistent team-wide.
    -- Sorted so the string is stable tick-to-tick (no spurious change-detect
    -- re-sends); change-detect + the 30 s heartbeat below ship it.
    do
      local ids = nil
      for id, p in pairs(world.pills) do
        if p.in_tank and p.owner_player == info.player_number then
          ids = ids or {}; ids[#ids + 1] = id
        end
      end
      if ids then table.sort(ids); bsi.carry = table.concat(ids, ",") end
    end

    -- ── Outbound message batching ──────────────────────────────────────────
    -- The brain API exposes ONE outbound buffer per think (BrainInfo.
    -- sendmessage), but the internal bot channel (messagedest=0) just fans the
    -- string into every ally's inbox, which is fully drained each tick. So we
    -- PACK several queued /info messages into that one buffer, joined by
    -- comms.MSG_SEP, staying under the 128-byte chat cap (PACKET_MAX_CHAT_MESSAGE;
    -- bot_manager.c truncates there). Receivers split on MSG_SEP and process each
    -- segment (comms.process_message). A producer commits its bookkeeping ONLY
    -- when try_send accepts the message; if it doesn't fit, the producer leaves
    -- its "needs send" flag set and re-queues next tick. This kills the verb
    -- starvation where a re-sent handshake (bac) hogged the slot so the GO (bgo,
    -- carried on /info state) never went out and the soldier sat in blitz_wait.
    -- Reposition position-scan scheduler: keep the heavy O(pills^2) coverage
    -- scan OFF the replan tick. Once the cache is stale (>= INTERVAL) run the
    -- rescan on the first NON-REPLAN tick; force it after +MAX_DEFER so it
    -- can't starve.
    -- Determinism note (20260831): this used to also require a "quiet" tick,
    -- judged by comparing this tick's wall-clock elapsed (clock_us()) against
    -- a rolling average — a real-time read steering WHICH tick the rescan ran
    -- on. The cached scores are functions of that tick (act_pen decay, live
    -- enemy positions, roles) and feed the goal pool and the /info rvo
    -- broadcast, so the timing coin-flip could fork same-seed games (seed
    -- 586261041 forked 50/50 at one late decision). Replan ticks are the
    -- expensive ones; skipping only those keeps most of the load-spreading
    -- with zero wall-clock input.
    do
      local _age = now - ((state._repo_score and state._repo_score.tick) or -1000000)
      if _age >= (C.REPOSITION_SCORE_INTERVAL or 50) then
        local _force = _age >= (C.REPOSITION_SCORE_INTERVAL or 50) + (C.REPOSITION_SCORE_MAX_DEFER or 40)
        if (not state.replan_this_tick) or _force then
          goals.rescan_reposition(state, world, info)
        end
      end
    end

    -- Reposition vote: drive the consensus state machine. Runs after goal
    -- selection (so state._repo_candidate is fresh) and queues its verbs on
    -- state._repo_outbox, drained into the batch below.
    reposition_vote.update(state, world, info, now)

    local _batch, _batch_used = {}, 0
    local _BATCH_MAX = C.MSG_BATCH_MAX or 124
    local function try_send(msg, dest)
      dest = dest or 0
      if dest ~= 0 then
        -- Real player-to-player chat (human allies) can't share the internal
        -- batch (different routing); only one such message per tick and it
        -- loses to any batched internal traffic.
        if #_batch > 0 or send_msg then return false end
        send_msg = msg; msg_dest = dest
        return true
      end
      local sep = (#_batch > 0) and #comms.MSG_SEP or 0
      -- The FIRST message is always accepted, even if it alone exceeds the cap
      -- (matches pre-batch behavior — the wire truncates and the *_OVERFLOW
      -- tripwire flags it). Later messages only join if they fit under the cap.
      if #_batch > 0 and _batch_used + sep + #msg > _BATCH_MAX then return false end
      _batch[#_batch + 1] = msg
      _batch_used = _batch_used + sep + #msg
      return true
    end

    -- One-shot "LGM back" notice: fired the single tick our LGM returns from
    -- dead. Internal channel (messagedest=0), same routing as /info state.
    if state.pending_lgm_back and try_send("/info lgmback", 0) then
      state.pending_lgm_back = nil
    end

    -- Reposition-vote verbs (rvo/rvy/rvn/rvr) queued by reposition_vote.update.
    -- Keep any that didn't fit the batch this tick so they retry next tick — a
    -- dropped NO ballot would otherwise let a bad reposition pass unanimously.
    if state._repo_outbox then
      local kept
      for _, m in ipairs(state._repo_outbox) do
        if try_send(m, 0) then
          print2(string.format("REPO_TX t=%d %s", now, m))
        else
          kept = kept or {}; kept[#kept + 1] = m
        end
      end
      state._repo_outbox = kept
    end

    -- Steal-negotiation verbs (stq/sta/str) queued by the ally-claimed sync
    -- (requests) and the steal-request processor (replies). Keep any that
    -- didn't fit the batch — a dropped sta would leave the challenger
    -- yielding to a holder that already gave the pill up.
    if state._steal_outbox then
      local kept
      for _, m in ipairs(state._steal_outbox) do
        if try_send(m, 0) then
          print2(string.format("STEAL_TX t=%d %s", now, m))
        else
          kept = kept or {}; kept[#kept + 1] = m
        end
      end
      state._steal_outbox = kept
    end

    -- Known-world resync query (one-shot, on first think / after respawn):
    -- ask allies to re-broadcast their known base/pill allegiance.
    if state._kw_send_query and try_send("/info kwq", 0) then
      state._kw_send_query = nil
    end

    -- Blitz-call registry one-shots (commander open/close, discovery query/
    -- re-announce). We have an open call iff we're a commander leading an
    -- attack_pill take (a help-wanted blitz); the target pill is goal.target_id.
    -- bco/bcc are emitted on the open/close transition; bcq is our discovery
    -- request on (re)spawn/join; a received bcq sets _blitz_rebroadcast so we
    -- re-announce our open call. Each rides try_send (batched).
    do
      local cur_call = nil
      -- Open-once / close-on-commit protocol: a call OPENS when we're leading a
      -- blitz (goal._blitz) on an attack_pill in a pre-commit substate, and then
      -- STAYS open across the pre-commit substates (squad.BLITZ_CALL_OPEN_SUB)
      -- on the SAME pill — even if goal._blitz momentarily flickers (HP near the
      -- hard-take threshold). It closes implicitly when the substate leaves that
      -- set (we committed to firing), the target changes, we become a soldier,
      -- or the goal ends. The _my_blitz_call latch is what keeps it open through
      -- a _blitz flicker.
      local _g = state.goal
      -- Must actually BE the commander to hold an open call. Without this, a bot
      -- that yielded command (election demoted it to soldier) kept _my_blitz_call
      -- latched, never sent bcc, and lingered as a ghost commander in allies'
      -- registries — so a soldier would defer to / negotiate with a bot that no
      -- longer commands the pill. Dropping to soldier now drives cur_call -> nil
      -- -> bcc (clean hand-off: the call closes and soldiers re-pick the real one).
      if _g and _g.kind == "attack_pill" and not state.squad_cmdr
         and state.squad_role == squad.ROLE_COMMANDER
         and _g.target_id and _g.target_id >= 0
         and squad.BLITZ_CALL_OPEN_SUB[_g.substate or ""] then
        if _g._blitz or state._my_blitz_call == _g.target_id then
          cur_call = _g.target_id
        end
      end
      if BRAIN_DEBUG_MODE and _g and _g.kind == "attack_pill" and (_g._blitz or (_g.substate and _g.substate:find("blitz"))) then print2(string.format("BLITZ_TXDIAG t=%d role=%s sub=%s _blitz=%s cmdr=%s tgt=%s cur=%s my=%s", now, tostring(state.squad_role), tostring(_g.substate), tostring(_g._blitz), tostring(state.squad_cmdr), tostring(_g.target_id), tostring(cur_call), tostring(state._my_blitz_call))) end
      if cur_call ~= state._my_blitz_call and cur_call then
        if try_send("/info bco " .. cur_call, 0) then
          state._my_blitz_call = cur_call
          state._my_blitz_call_tick = now   -- when WE opened this call (first-to-take rule)
          state._blitz_rebroadcast = nil
          print2(string.format("BLITZ_TX bco pill=%d t=%d (open)", cur_call, now))
        end
      elseif cur_call ~= state._my_blitz_call and state._my_blitz_call then
        if try_send("/info bcc", 0) then
          print2(string.format("BLITZ_TX bcc t=%d (close pill=%s)", now, tostring(state._my_blitz_call)))
          state._my_blitz_call = cur_call   -- now nil
          state._my_blitz_call_tick = nil
        end
      elseif state._blitz_su_send and state._blitz_su_send[1] then
        -- Blitz suicider designations queued by squad.blitz_designate_suiciders
        -- at GO. One per tick, oldest first, and dropped from the queue only
        -- once the send actually goes out (try_send is batched and can refuse).
        -- The optional trailing letter is WHY: "c" = contested take (every
        -- blitzer is designated), absent = the ordinary BLITZ_MIN_SUICIDERS
        -- quota top-up. It only feeds the receiver's [role] line / DECISION
        -- breakdown; an older peer that ignores it still reads the pill and pn.
        local _d = state._blitz_su_send[1]
        local _msg = (_d.why == "contested")
                     and string.format("/info bsu %d %d c", _d.pill, _d.pn)
                     or  string.format("/info bsu %d %d", _d.pill, _d.pn)
        if try_send(_msg, 0) then
          table.remove(state._blitz_su_send, 1)
          print2(string.format("BLITZ_TX bsu t=%d pill=%d -> p%d why=%s (%s)",
                 now, _d.pill, _d.pn, tostring(_d.why or "quota"), squad.blitz_suiciders_label()))
        end
      elseif state._blitz_rebroadcast and cur_call then
        if try_send("/info bco " .. cur_call, 0) then
          state._blitz_rebroadcast = nil
          print2(string.format("BLITZ_TX bco pill=%d t=%d (re-announce)", cur_call, now))
        end
      elseif state._blitz_query_send then
        if try_send("/info bcq", 0) then
          state._blitz_query_send = nil
          print2(string.format("BLITZ_TX bcq t=%d (discovery)", now))
        end
      end
    end

    -- Commander->soldier handshake (brj reject / bac accept) on their OWN short
    -- verbs — NOT bundled into /info state (which exceeded the 128-byte cap and got
    -- dropped, so the verdict never reached the soldier and negotiation stalled for
    -- seconds). RE-SENT every tick while pending so it lands within a couple ticks
    -- despite single-send loss — that's what gets negotiation down to a fraction of
    -- a second. Alternate when both are set. Higher priority than the bulky state
    -- (which is event-driven anyway); a call open/close (above) still wins.
    if state.squad_role == "c" then
      local _brj = state.squad_blitz_reject
      local _bac = state.squad_blitz_accept
      local hs
      if _brj and _bac then
        hs = state._hs_toggle and ("/info brj " .. _brj) or ("/info bac " .. _bac)
      elseif _brj then hs = "/info brj " .. _brj
      elseif _bac then hs = "/info bac " .. _bac end
      if hs and try_send(hs, 0) then
        if _brj and _bac then state._hs_toggle = not state._hs_toggle end  -- alternate only on actual send
        -- Tripwire: brj is a per-rejected-soldier list (bounded by offer count, not
        -- SQUAD_MAX_SIZE). Tiny in practice, but flag if a swarm of offers ever
        -- pushes it past the cap so we chunk/rotate it then.
        if BRAIN_DEBUG_MODE and #hs > 124 then print2(string.format("HANDSHAKE_OVERFLOW t=%d len=%d (>124 — chunk brj): %s", now, #hs, hs)) end
        print2(string.format("BLITZ_TX handshake t=%d %s", now, hs))
      end
    end

    -- Blocker tiles on their OWN verb (split off the state slate — it's a
    -- variable-length tile list that was the main thing pushing /info state over
    -- the 128 cap). Sent on change (it changes rarely) + a slow heartbeat so a
    -- late-arriving ally still learns the team's protected blocker pills. Empty
    -- payload clears it on receivers (set_handshake nil).
    do
      local _pblk = (state._blocker_tiles and #state._blocker_tiles > 0)
        and table.concat(state._blocker_tiles, ",") or nil
      if _pblk ~= state._last_pblk_sent
         or (_pblk and (now - (state._last_pblk_tick or -100000)) >= 300) then
        local msg = _pblk and ("/info pblk " .. _pblk)
                    or (state._last_pblk_sent and "/info pblk")   -- now empty → clear
        if msg and try_send(msg, 0) then
          state._last_pblk_sent = _pblk
          state._last_pblk_tick = now
          print2(string.format("BLITZ_TX pblk t=%d %s", now, msg))
        end
      end
    end

    -- Shield WALL tiles on their OWN verb (bwl), broadcast ONLY while a commander
    -- is in build_walls (current_shield_wall_tiles gates it). Blitz soldiers read
    -- it to route their engage spot + aim point AROUND our fresh shield walls.
    -- Change-detect + slow heartbeat, same shape as pblk; empty payload clears.
    do
      local _bwl = (state._shield_wall_tiles and #state._shield_wall_tiles > 0)
        and table.concat(state._shield_wall_tiles, ",") or nil
      if _bwl ~= state._last_bwl_sent
         or (_bwl and (now - (state._last_bwl_tick or -100000)) >= 300) then
        local msg = _bwl and ("/info bwl " .. _bwl)
                    or (state._last_bwl_sent and "/info bwl")   -- now empty → clear
        if msg and try_send(msg, 0) then
          state._last_bwl_sent = _bwl
          state._last_bwl_tick = now
          print2(string.format("BLITZ_TX bwl t=%d %s", now, msg))
        end
      end
    end

    -- `cost` is NOT on the /info state slate (it drifts almost every tick
    -- as we close on the target, which would force a near-per-tick send —
    -- the same trap tx/ty were).  It rides the /info extra channel on a
    -- 1 Hz cadence instead (see the bse block below); the state message
    -- stays purely event-driven.
    local last = state.last_broadcasted_state_info
    local differs = false
    for k, v in pairs(bsi) do if last[k] ~= v then differs = true break end end
    if not differs then
      for k, v in pairs(last) do if bsi[k] ~= v then differs = true break end end
    end
    local heartbeat_due = (now - state.last_broadcast_state_tick) >= 1500
    if differs or heartbeat_due then
      local msg = comms.format_state(bsi)
      -- Overflow tripwire: /info state must stay under the 128-byte chat cap or
      -- it's silently truncated/dropped on the wire (that's the bug that ate brj).
      -- Variable fields (pblk) are split onto their own verbs; this catches any
      -- future field that re-bloats the slate so we split it too.
      if BRAIN_DEBUG_MODE and #msg > 124 then print2(string.format("STATE_OVERFLOW t=%d len=%d (>124 — will be truncated at 128, split a field off): %s", now, #msg, msg)) end
     -- Internal channel: messagedest=0 routes through the brain inbox of every
     -- allied bot in this sim and is shown locally on MSG_AI when run from the
     -- Brains menu — see brain_data.c's brainDataExtractInfo. No human's newswire
     -- ever sees /info state, so we fire it on every goal change + heartbeat.
     if try_send(msg, 0) then
        for k in pairs(last) do last[k] = nil end
        for k, v in pairs(bsi) do last[k] = v end
        state.last_broadcast_state_tick = now
      end
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
                            bit.band(state.goal.approach_mx, 0xFF),
                            bit.band(state.goal.approach_my, 0xFF),
                            bit.band(state.goal.standoff_mx, 0xFF),
                            bit.band(state.goal.standoff_my, 0xFF))
    end
    -- LGM dispatch advert: dest x, dest y (2 hex chars each) + remaining
    -- ETA ticks (4 hex chars) while our LGM is OUT on a dispatch; "-"
    -- once it's back. Explicit "-" (not absence) because /info extra is
    -- MERGED into receiver slots — a dropped key would linger stale.
    -- Consumers: allied heat gates (don't shell a pill an ally's LGM is
    -- walking to), future repair dedup.
    if state._lgm_dispatch and info.man_status ~= C.LGM_INTANK then
      local ld = state._lgm_dispatch
      local left = (ld.eta_tick or now) - now
      if left < 0 then left = 0 elseif left > 65535 then left = 65535 end
      bse.lgmd = string.format("%02X%02X%04X",
                               bit.band(ld.x or 0, 0xFF),
                               bit.band(ld.y or 0, 0xFF), left)
    else
      if state._lgm_dispatch and info.man_status == C.LGM_INTANK
         and (now - (state._lgm_dispatch.tick or 0)) > 2 then
        state._lgm_dispatch = nil  -- LGM home again; advert the clear
      end
      bse.lgmd = "-"
    end
    -- Builder-pool JOB CLAIM: "this side-quest is mine". Same channel and the
    -- same explicit-"-" discipline as lgmd, and deliberately a SEPARATE key:
    -- lgmd says where the man is walking, which is a fact about this tick;
    -- bpj says what OUTCOME we have taken responsibility for, which is what
    -- allies must not duplicate. Five bots must not all rebuild one corpse.
    -- Format and arbitration: builder_pool.claim_advert / ally_claim_on.
    bse.bpj = bpool.claim_advert(state, now)
    -- ── "KILL ME" tokens ─────────────────────────────────────────────────
    -- km  = "XXYYAA"  our request: the tile we are parked on and our armour,
    --                 present ONLY while the kill_me_wait goal is held. The
    --                 goal's own validity check drops it the moment the state
    --                 clears (man back, tank died, respawn) or an enemy tank
    --                 comes inside KILL_ME_CANCEL_ENEMY_TILES, so the token
    --                 leaves on the next heartbeat with no separate lifetime.
    -- kmc = "PPCCCC"  our CLAIM: which initiator we are answering and at what
    --                 cost. Read by other responders (the steal band) AND by
    --                 the initiator, which is how it learns whom not to shoot.
    --
    -- Both are OMITTED rather than sent as "-" when there is nothing to say:
    -- merge_info evicts any /info extra key absent from the payload, so an
    -- omission clears them on every receiver, and the 128-byte slate does not
    -- pay for two dead fields on every heartbeat of every game.
    if state.goal and state.goal.kind == "kill_me_wait" then
      bse.km = string.format("%02X%02X%02X",
                             bit.band(state.goal.mx or 0, 0xFF),
                             bit.band(state.goal.my or 0, 0xFF),
                             bit.band(info.armour or 0, 0xFF))
    end
    if state.km and state.km.claim then
      bse.kmc = string.format("%02X%04X",
                              bit.band(state.km.claim.pn or 0, 0xFF),
                              bit.band(math.floor(math.min(state.km.claim.cost or 0, 65535)), 0xFFFF))
    end
    -- Goal-selection cost (pool_cache winner matching our current goal).
    -- Drifts every tick as we close on the target, so it rides the extra
    -- channel on a ~1 Hz cadence (see cost_due below) rather than the
    -- event-driven state slate.  Carried in EVERY extra send — not just
    -- cadence ticks — so merge_info never evicts it from receivers, who
    -- read it only to break a contention tie (once-per-second is ample).
    if state.goal and state.goal.kind and state.goal.kind ~= "none"
       and state.pool_cache then
      for pi = 0, 15 do
        local pce = state.pool_cache[pi]
        if pce and pce.goal
           and pce.goal.kind == state.goal.kind
           and pce.goal.mx   == state.goal.mx
           and pce.goal.my   == state.goal.my
           and pce.cost ~= nil then
          -- VARIANT (c): the advertised cost is the RAW pool cost on every
          -- pool, pool 6 included, and carries no units tag.
          bse.cost = string.format("%.0f", pce.cost - (pce.ally_claimed_pen or 0))
          break
        end
      end
    end
    local last_ext = state.last_broadcasted_state_extra
    if last_ext == nil then
      last_ext = {}; state.last_broadcasted_state_extra = last_ext
    end
    -- `cost` is kept OUT of the change trigger (it drifts every tick); a
    -- cost-only change must not force a send.  Its own 1 Hz cadence
    -- (cost_due) drives those refreshes instead.
    local extras_differ = false
    for k, v in pairs(bse)      do if k ~= "cost" and last_ext[k] ~= v then extras_differ = true break end end
    if not extras_differ then
      for k, v in pairs(last_ext) do if k ~= "cost" and bse[k] ~= v then extras_differ = true break end end
    end
    -- Send extras when nothing else is going out this tick and either the
    -- non-cost payload changed, the 1 Hz cost refresh is due, or the
    -- 30 s heartbeat fires (so a freshly-joined/stale-slot ally catches up).
    -- Lazy init to `now`, not 0: both drive "ticks since the last send", and
    -- with engine-seeded ticks a 0 would make a mid-game bot's first tick look
    -- long overdue and fire both refreshes at once.
    state.last_broadcast_extra_tick = state.last_broadcast_extra_tick or now
    state.last_broadcast_cost_tick  = state.last_broadcast_cost_tick  or now
    local extra_heartbeat_due = (now - state.last_broadcast_extra_tick) >= 1500
    local cost_due = bse.cost ~= nil and (now - state.last_broadcast_cost_tick) >= 50  -- 50 ticks = 1 s
    if (next(bse) ~= nil or next(last_ext) ~= nil)
       and (extras_differ or cost_due or extra_heartbeat_due) then
      -- Internal channel, same routing as /info state above.
      if try_send(comms.format_extra(bse), 0) then
        for k in pairs(last_ext) do last_ext[k] = nil end
        for k, v in pairs(bse)      do last_ext[k] = v end
        state.last_broadcast_extra_tick = now
        if bse.cost ~= nil then state.last_broadcast_cost_tick = now end
      end
    end

    -- Known-world digest (idle slot, low priority): relay our first-hand
    -- base/pill allegiance CHANGES to allies. Usually empty (allegiance flips
    -- are rare), so no steady-state traffic; on a resync it drains a few
    -- objects per free tick. Receiver folds via W.sync_ally_world.
    -- build_kw_message DRAINS the entries it packs, so only build it when the
    -- batch is empty (it'll definitely fit as the first message) — otherwise a
    -- failed try_send would silently lose the drained changes.
    if world._kw_dirty and next(world._kw_dirty) ~= nil then
      if #_batch == 0 then
        local kwmsg = W.build_kw_message(world)
        if kwmsg and try_send(kwmsg, 0) then
          local rem = 0; for _ in pairs(world._kw_dirty) do rem = rem + 1 end
          print2(string.format("KW_TX t=%d pn=%s remain=%d msg=%s", now, tostring(state.player_number), rem, kwmsg))
        end
      else
        local pend = 0; for _ in pairs(world._kw_dirty) do pend = pend + 1 end
        print2(string.format("KW_STARVE t=%d pn=%s pending=%d (batch busy)", now, tostring(state.player_number), pend))
      end
    end

    -- Finalize the internal-channel batch into the single outbound buffer.
    -- Anything that didn't fit left its producer's "needs send" flag set and
    -- re-queues next tick. Done before the human-chat block (different routing,
    -- shares the one buffer) so batched internal traffic takes precedence.
    -- The internal batch outranks the one-shot "loaded" greeting: the
    -- greeting used to take the tick's single send slot while the batch --
    -- whose /info state slate had ALREADY been recorded as sent -- was
    -- silently dropped, so a bot's first claim (goal=capture_pill target=N)
    -- never reached its allies until the goal changed or the 1500-tick
    -- heartbeat (2026-09-07 ally_capture_guard arena A: the ally scooped the
    -- corpse at t=207 with zero state slates sent). Defer the greeting a tick.
    if #_batch > 0 and send_msg and open_msg_this_tick then
      state.send_open_msg = true
      send_msg = nil
      msg_dest = 0
    end
    if #_batch > 0 and not send_msg then
      send_msg = table.concat(_batch, comms.MSG_SEP)
      msg_dest = 0
      if BRAIN_DEBUG_MODE and #_batch > 1 then print2(string.format("MSG_BATCH t=%d n=%d len=%d", now, #_batch, #send_msg)) end
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
      local human_allies = bit.band(allies, bit.bnot(bots))
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
        anchor_mx = bit.rshift(info.tankx, 8)
        anchor_my = bit.rshift(info.tanky, 8)
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

  -- Audit hook: log EVERY real outbound message (at most one per tick). Grep
  -- "MSG_TX" across a session to audit actual /info bus traffic — cadence and
  -- content of state-slate, bes/bd, bco/bcc/bcq, replies, etc. (dest 0 = the
  -- ally/bot-only internal channel; a nonzero bitmask = specific recipients).
  if BRAIN_DEBUG_MODE and send_msg and send_msg ~= "" then print2(string.format("MSG_TX t=%d dest=%d msg=%s", now, msg_dest or 0, tostring(send_msg))) end

  -- ── PROFILING LITE: TICK_COST / NEAR_BUDGET ─────────────────────────────
  -- Sits immediately before print2.flush() so it measures as much of the
  -- tick as can still be logged, and so the evidence lands in the buffer of
  -- the tick BEFORE a budget kill (a killed tick loses its whole buffer).
  --
  -- Clock: print2.elapsed_ms() — the same os.clock origin behind every
  -- "[x.xxms]" line prefix in this log, so TICK_COST lines up with them.
  --
  -- Happy path cost: one clock read + one string.format. NEAR_BUDGET's
  -- extra format only runs when the tick is already near its budget.
  --
  -- Only the MAIN think exit is instrumented. The four early exits (dead /
  -- startup / manual / paused) never call print2.flush(), so anything
  -- printed on those ticks is discarded when the next set_tick clears the
  -- buffer — a TICK_COST there would be invisible.
  if BRAIN_DEBUG_MODE then
    local _pl_ms   = print2.elapsed_ms()
    local _pl_tgt  = state._plite_tgt_ms or 0
    local _pl_rep  = state._plite_cp_prelude and tostring(state.replan_this_tick) or "n/a"
    print2(string.format("TICK_COST t=%d ms=%.1f tgt=%.1f tier=%d replan=%s goal=%s sub=%s",
      now, _pl_ms, _pl_tgt, state._capacity_tier or -1, _pl_rep,
      tostring(state.goal and state.goal.kind), tostring(state.goal and state.goal.substate)))
    if _pl_tgt > 0 and _pl_ms > _pl_tgt * (C.PROFILE_LITE_NEAR_FRAC or 0.85) then
      -- Cumulative checkpoints -> per-stage deltas. A stage whose checkpoint
      -- never fired (goal section skipped) collapses to 0 rather than going
      -- negative. prelude = world/danger/threat/percept + dijkstra + mid +
      -- goal-validation; replan = replan decision + full replan work;
      -- handler = attack substate machine; rest = steering + builder + HUD +
      -- squad/comms + tail.
      local _c1 = state._plite_cp_prelude or 0
      local _c2 = state._plite_cp_replan  or _c1
      local _c3 = state._plite_cp_handler or _c2
      print2(string.format(
        "NEAR_BUDGET t=%d ms=%.1f/%.1f stages: prelude=%.1f replan=%.1f handler=%.1f rest=%.1f",
        now, _pl_ms, _pl_tgt, _c1, _c2 - _c1, _c3 - _c2, _pl_ms - _c3))
    end
  end

  -- Flush print2 log for this tick. MUST be the last thing before return so it
  -- captures the /info broadcast block above (blitz TX diagnostics included).
  if BRAIN_DEBUG_MODE then print2.flush() end

  -- Paired stop for the sampling profiler armed after the early-out gates.
  -- Every armed tick reaches here, keeping start/stop balanced.
  if state._instr_prof_on then prof.stop(state.server_tick or state.tick or 0) end

  -- Normal completion: clear the tick-budget kill marker written at the top.
  -- Anything still set at the next think's top means that think was killed.
  state._think_attempt = nil

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
  local tmx = bit.rshift((i.tankx or 0), 8)
  local tmy = bit.rshift((i.tanky or 0), 8)
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
      manual_keys = bit.bor(manual_keys, bit)
    else
      manual_keys = bit.band(manual_keys, bit.bnot(bit))
    end
  end
end

-- Manual BUILD-type selector. The host (BrainTest) calls this when the user
-- presses 1-5 (or clicks a HUD build button) in manual mode. n is 1-based
-- BUILDMODE (1=Trees, 2=Road, 3=Wall, 4=Pillbox, 5=Mine); clamp to that range.
function Brain.manual_set_build(n)
  n = tonumber(n) or 1
  if n < 1 then n = 1 elseif n > 5 then n = 5 end
  manual_build_action = n
end

function Brain.on_click(mx, my, mods)
  mods = mods or {}

  -- Manual mode: a plain (no-modifier) left click on a tile issues a build of
  -- the currently selected type at that tile. Latched as a one-shot; the manual
  -- think emits it next tick. Modifier-clicks fall through to the normal
  -- inspect/force-attack handlers below.
  if manual_active and not (mods.shift or mods.ctrl or mods.alt) then
    manual_pending_build = { x = mx, y = my, action = manual_build_action }
    local rp = state._real_print or print
    rp(string.format(TAG .. " MANUAL BUILD: %s at (%d,%d)", MANUAL_BUILD_LABELS[manual_build_action] or "?", mx, my))
    return
  end
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
  -- Flush and close the sampling profiler's per-bot TSV if it was armed.
  if state._instr_prof_on then prof.shutdown() end
  log.dump_world(world)
  log.close()
  -- Push print2's batched tail (up to FLUSH_INTERVAL_S of lines) into the
  -- writer's queue BEFORE opt.close() drains it, so the last second of a run
  -- reaches disk instead of dying with the process.
  print2.force_flush()
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
