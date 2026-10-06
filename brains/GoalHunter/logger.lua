local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/logger.lua — structured tick log + map dump for post-mortem analysis
-- =========================================================================

local C = require("constants")
local U = require("util")
local TAG = "[" .. C.BRAIN_NAME .. "]"

local M = {}

local file    = nil
local current_filename = nil  -- track path for cleanup on reopen
local events  = {}   -- accumulated during a tick, flushed on log_tick()
local reasons = {}   -- decision reasoning accumulated during a tick

-- Backstop caps. No single tick may grow an unbounded record: past
-- C.LOGGER_TICK_MAX_ENTRIES new entries are dropped and merely counted, and
-- one summary entry ("dropped N ...") is emitted when the tick serializes.
local MAX_ENTRIES     = C.LOGGER_TICK_MAX_ENTRIES or 256
local events_dropped  = 0
local reasons_dropped = 0

-- Current tick number, stamped by Brain.think via M.set_tick (and refreshed
-- from state.tick in log_tick as a fallback). Only used to label the record
-- M.flush_killed emits — that runs on a tick log_tick never reached, so it
-- has no `state` to read the tick number from.
local cur_tick = 0

--- Stamp the current tick number. Called once per Brain.think, next to
--- opt.set_tick. Deliberately trivial so it costs nothing in release runs.
function M.set_tick(t)
  cur_tick = t or 0
end

-- Detach the per-tick accumulators and hand the captured set back to the
-- caller. EVERY serialization path calls this BEFORE doing any work, so that
-- a tick-budget kill (or any other error) part-way through a write can never
-- leave the module-level tables populated. If it did, the next tick would
-- re-serialize the whole backlog plus its own new entries — growing
-- monotonically and budget-killing the bot every tick from then on.
local function take_accumulators()
  local ev,  rs  = events, reasons
  local evd, rsd = events_dropped, reasons_dropped
  events,         reasons         = {}, {}
  events_dropped, reasons_dropped = 0, 0
  return ev, rs, evd, rsd
end

-- =========================================================================
-- Open / Close
-- =========================================================================

--- Generate a timestamped log filename.
--  e.g. prefix="player0" -> "debug_sessions/20260321_115200/player0.jsonl"
--  Uses DEBUG_SESSION_DIR if set by BrainTest, otherwise current directory.
function M.make_filename(prefix)
  local dir = _G.DEBUG_SESSION_DIR
  if dir then
    return dir .. "/" .. prefix .. ".jsonl"
  end
  local ts = os.date("%Y%m%d_%H%M%S")
  return prefix .. "_" .. ts .. ".jsonl"
end

--- Open the jsonl for this brain.
--  APPEND, not truncate. Each brain instance is its own lua_State, so a bot
--  that is re-created mid-game (a Survival wave respawn) runs M.open again on
--  the SAME session path; with "w" every life wiped the previous one and the
--  file only ever held the last life. "a" keeps them all. Nothing else writes
--  this path: the filename is either DEBUG_SESSION_DIR/<prefix>.jsonl (a fresh
--  per-session directory) or a timestamped name, so appending can't pick up a
--  stale file from an earlier run.
--
--  `seed_tick` / `engine_tick` (optional) are the brain's starting tick and
--  the engine tick it was created at. When given, a life-marker row is written
--  first so a reader can tell where one life's rows end and the next begin —
--  the tick rows themselves are now unique across the session (state.tick is
--  seeded from the engine clock), but the marker makes the boundary explicit
--  without having to diff consecutive "t" values.
function M.open(filename, seed_tick, engine_tick)
  -- If reopening to a new path, remove the old file (avoids stale CWD copies)
  if current_filename and current_filename ~= filename then
    os.remove(current_filename)
  end
  file = io.open(filename, "a")
  if file then
    current_filename = filename
    file:write(string.format('{"type":"life","t":%d,"engine_tick":%d}\n',
                             seed_tick or 0, engine_tick or 0))
    file:flush()
    print(TAG .. " LOG: opened " .. filename)
  else
    print(TAG .. " LOG: FAILED to open " .. filename)
  end
  take_accumulators()
  return file ~= nil
end

function M.close()
  if file then
    file:close()
    print(TAG .. " LOG: closed")
  end
  file = nil
  take_accumulators()
end

function M.is_open()
  return file ~= nil
end

-- =========================================================================
-- Map dump — called once at brain open
-- =========================================================================
-- Finds the bounding box of non-deep-sea terrain, writes one hex row per
-- map row.  Format:
--   {"type":"map","x1":N,"y1":N,"x2":N,"y2":N,"rows":["AABB...","..."]}
-- Each byte is the raw get_terrain() value (2 hex chars).

function M.dump_map()
  if not file then return end

  -- Find bounding box of non-deep-sea tiles
  local x1, y1, x2, y2 = 255, 255, 0, 0
  for my = 0, 255 do
    for mx = 0, 255 do
      local tt = bit.band(get_terrain(mx, my), TERRAIN_MASK)
      if tt ~= C.T_DEEPSEA then
        if mx < x1 then x1 = mx end
        if mx > x2 then x2 = mx end
        if my < y1 then y1 = my end
        if my > y2 then y2 = my end
      end
    end
  end

  -- Nothing found (shouldn't happen)
  if x2 < x1 then
    file:write('{"type":"map","x1":0,"y1":0,"x2":0,"y2":0,"rows":[]}\n')
    file:flush()
    return
  end

  -- Add 1-tile margin
  x1 = math.max(0, x1 - 1)
  y1 = math.max(0, y1 - 1)
  x2 = math.min(255, x2 + 1)
  y2 = math.min(255, y2 + 1)

  local hex = {}
  for i = 0, 255 do
    hex[i] = string.format("%02x", i)
  end

  local rows = {}
  for my = y1, y2 do
    local parts = {}
    for mx = x1, x2 do
      parts[#parts + 1] = hex[get_terrain(mx, my)]
    end
    rows[#rows + 1] = table.concat(parts)
  end

  file:write(string.format(
    '{"type":"map","x1":%d,"y1":%d,"x2":%d,"y2":%d,"rows":[',
    x1, y1, x2, y2))
  for i, row in ipairs(rows) do
    if i > 1 then file:write(",") end
    file:write('"')
    file:write(row)
    file:write('"')
  end
  file:write(']}\n')
  file:flush()

  print(string.format(TAG .. " LOG: map dumped bbox=(%d,%d)-(%d,%d) %dx%d",
        x1, y1, x2, y2, x2 - x1 + 1, y2 - y1 + 1))
end

-- =========================================================================
-- World state dump — bases and pills known at time of call
-- =========================================================================

function M.dump_world(world)
  if not file then return end

  local bases = {}
  for id, b in pairs(world.bases) do
    bases[#bases + 1] = string.format(
      '{"id":%d,"x":%d,"y":%d,"hp":%d,"own":"%s","seen":%d}',
      id, b.mx, b.my, b.health, b.owner, b.last_seen or 0)
  end

  local pills = {}
  for id, p in pairs(world.pills) do
    pills[#pills + 1] = string.format(
      '{"id":%d,"x":%d,"y":%d,"hp":%d,"own":"%s","ang":%.2f,"seen":%d}',
      id, p.mx, p.my, p.health, p.owner, p.anger or 0, p.last_seen or 0)
  end

  file:write('{"type":"world","bases":[')
  file:write(table.concat(bases, ","))
  file:write('],"pills":[')
  file:write(table.concat(pills, ","))
  file:write(']}\n')
  file:flush()
end

-- =========================================================================
-- Event accumulator — call during the tick, flushed by log_tick
-- =========================================================================

function M.event(name, detail)
  if not file then return end
  if #events >= MAX_ENTRIES then
    events_dropped = events_dropped + 1
    return
  end
  if detail then
    events[#events + 1] = name .. ":" .. tostring(detail)
  else
    events[#events + 1] = name
  end
end

-- =========================================================================
-- Reasoning accumulator — structured decision explanations
-- =========================================================================
-- Call during the tick to record WHY a decision was made.
-- category: short key like "goal", "steer", "build", "pf"
-- data: table with decision-specific fields (will be serialized to JSON)

function M.reason(category, data)
  if not file then return end
  if #reasons >= MAX_ENTRIES then
    reasons_dropped = reasons_dropped + 1
    return
  end
  reasons[#reasons + 1] = { cat = category, data = data }
end

-- =========================================================================
-- Per-tick log line
-- =========================================================================
-- Writes one JSON line with all decision-relevant state.

-- Helper: serialize a simple table to a JSON object string (one level deep)
local function to_json_obj(tbl)
  if not tbl then return "null" end
  local parts = {}
  for k, v in pairs(tbl) do
    local vstr
    if type(v) == "string" then
      vstr = '"' .. v:gsub('\\', '\\\\'):gsub('"', '\\"') .. '"'
    elseif type(v) == "boolean" then
      vstr = tostring(v)
    elseif type(v) == "number" then
      if v ~= v then vstr = "null"  -- NaN
      elseif v == math.huge then vstr = "9999"
      else vstr = string.format("%.2f", v) end
    elseif type(v) == "table" then
      -- Nested table: recurse one level
      vstr = to_json_obj(v)
    else
      vstr = '"' .. tostring(v) .. '"'
    end
    parts[#parts + 1] = string.format('"%s":%s', tostring(k), vstr)
  end
  return "{" .. table.concat(parts, ",") .. "}"
end

-- Helper: serialize an array of tables to JSON array
local function to_json_array(arr)
  if not arr or #arr == 0 then return "[]" end
  local parts = {}
  for _, item in ipairs(arr) do
    if type(item) == "table" then
      parts[#parts + 1] = to_json_obj(item)
    elseif type(item) == "string" then
      parts[#parts + 1] = '"' .. item:gsub('"', '\\"') .. '"'
    else
      parts[#parts + 1] = tostring(item)
    end
  end
  return "[" .. table.concat(parts, ",") .. "]"
end

-- Serialize one captured events set (as handed back by take_accumulators).
-- A non-zero `dropped` appends one summary entry so a truncated tick is
-- visible in the log rather than silently short.
local function events_json(ev, dropped)
  if #ev == 0 and dropped == 0 then return "[]" end
  local parts = {}
  for _, e in ipairs(ev) do
    parts[#parts + 1] = '"' .. e:gsub('"', '\\"') .. '"'
  end
  if dropped > 0 then
    parts[#parts + 1] = string.format('"logger:dropped %d events (cap %d)"',
                                      dropped, MAX_ENTRIES)
  end
  return "[" .. table.concat(parts, ",") .. "]"
end

-- Serialize one captured reasons set, same contract as events_json.
local function reasons_json(rs, dropped)
  if #rs == 0 and dropped == 0 then return "[]" end
  local parts = {}
  for _, r in ipairs(rs) do
    parts[#parts + 1] = string.format('{"cat":"%s","d":%s}',
      r.cat, to_json_obj(r.data))
  end
  if dropped > 0 then
    parts[#parts + 1] = string.format(
      '{"cat":"logger","d":{"dropped":%d,"cap":%d}}', dropped, MAX_ENTRIES)
  end
  return "[" .. table.concat(parts, ",") .. "]"
end

-- Single write path for every per-tick record this module emits. Flushes
-- every line so the log is always current for live reading. Wrapped in pcall
-- (no closure — pcall the method directly) so a failing write can never
-- propagate into the brain: by the time we get here the caller has already
-- detached its accumulators, so a failure costs one record instead of
-- leaking the backlog into the next tick.
local function write_line(line)
  if not file then return false end
  local ok = pcall(file.write, file, line)
  if ok then pcall(file.flush, file) end
  return ok
end

function M.log_tick(state, info, goal, keys, taps, build_cmd)
  -- Runtime gate: skip writes when the JSONL logger flag is off.
  -- The C side toggles _G._JSONL_LOGGER_ENABLED whenever the user
  -- changes the checkbox in the BrainTest debug modules panel.
  if not _G._JSONL_LOGGER_ENABLED then
    -- The gate can flip off mid-session with `file` still open. M.event /
    -- M.reason key off `file`, not off this flag, so they keep appending
    -- with nothing left to drain them. Drop this tick's entries rather
    -- than let them accumulate forever.
    if #events > 0 or #reasons > 0 then take_accumulators() end
    return
  end

  cur_tick = state.tick or cur_tick

  -- Swap the accumulators out BEFORE any serialization work (belt-and-braces
  -- alongside M.flush_killed): a budget kill — or any future error — inside
  -- the serialize/write below then cannot poison the next tick. Worst case
  -- this tick's records are lost, and only if the kill lands before the
  -- flush hook runs; the hook drains whatever is left anyway.
  local ev_t, rs_t, ev_drop, rs_drop = take_accumulators()

  -- Lazy open: if the user enabled the flag mid-session and there's
  -- no open file yet, open one now using the standard naming scheme.
  if not file then
    local fname = M.make_filename("player" .. (info.player_number or 0))
    if not M.open(fname, state.tick, state.engine_tick0) then return end
    M.dump_map()
  end

  local pf = state.pf

  -- Build string (nil or "x,y,action")
  local bld = "null"
  if build_cmd then
    bld = string.format('"%d,%d,%d"', build_cmd.x, build_cmd.y, build_cmd.action)
  end

  -- Command goal summary
  local cg = "null"
  if state.command_goal then
    local c = state.command_goal
    cg = string.format('"%s#%d@%d,%d"', c.kind, c.id, c.mx, c.my)
  end

  -- Events + reasoning arrays (from the detached copies taken above)
  local ev  = events_json(ev_t, ev_drop)
  local rsn = reasons_json(rs_t, rs_drop)

  -- Visible objects summary (tanks, shots — compact)
  local objs = {}
  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_TANK or ob.type == OBJECT_SHOT then
      objs[#objs + 1] = string.format('{"t":%d,"x":%d,"y":%d,"d":%d,"i":%d}',
        ob.type, ob.x, ob.y, ob.direction, ob.info)
    end
  end
  local obj_str = "[" .. table.concat(objs, ",") .. "]"

  -- Pathfinder detail
  local pf_open_size = 0
  if pf.open then pf_open_size = pf.open.n or 0 end
  -- Count closed set size only every 10 ticks (can be large)
  local pf_closed_size = 0
  if pf.closed and state.tick % 10 == 0 then
    for _ in pairs(pf.closed) do pf_closed_size = pf_closed_size + 1 end
  end

  -- Builder mode
  local bmode = state.builder and state.builder.mode or "unknown"

  -- Strategy phase
  local phase = state.phase or "unknown"
  local strength = state.strength or 0.5
  local base_strength = state.base_strength or 0.5

  -- Front line (only include when computed, to keep log lines small)
  local front_str = ""
  if state.front_center_mx then
    front_str = string.format('"front_mx":%d,"front_my":%d,"front_n":%d,',
      state.front_center_mx, state.front_center_my, state.front_line_count or 0)
  end

  -- Path chain: flat array [x1,y1,x2,y2,...] from A*/Dijkstra trace
  -- (see cpf_trace_path / cpf_dijkstra_trace_path in braincore.c —
  -- both push pairs of integers, NOT {x,y} tables).
  local path_str = "[]"
  if pf.path_chain and #pf.path_chain > 0 then
    local parts = {}
    for i = 1, #pf.path_chain, 2 do
      parts[#parts + 1] = string.format("%d,%d",
        pf.path_chain[i], pf.path_chain[i + 1])
    end
    path_str = "[[" .. table.concat(parts, "],[") .. "]]"
  end

  write_line(string.format(
    '{"type":"tick","t":%d,'
    .. '"tx":%d,"ty":%d,"mx":%d,"my":%d,"dir":%d,"spd":%d,'
    .. '"boat":%s,"arm":%d,"sh":%d,"mi":%d,"tr":%d,"cpill":%d,'
    .. '"lgm":%d,"lx":%d,"ly":%d,'
    .. '"goal":"%s","gsub":"%s","gx":%d,"gy":%d,'
    .. '"pf":"%s","nx":%d,"ny":%d,"lax":%d,"lay":%d,"pfage":%d,"pfopen":%d,"pfclosed":%d,'
    .. '"path":%s,'
    .. '"cmd":%s,'
    .. '"keys":%d,"taps":%d,"bld":%s,"bmode":"%s",'
    .. '"phase":"%s","str":%.2f,"bstr":%.2f,"ct":%d,'
    .. '%s'
    .. '"stuck":%d,"ev":%s,"rsn":%s,"objs":%s}\n',
    state.tick,
    info.tankx, info.tanky, bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8),
    info.direction, info.speed,
    tostring(info.inboat), info.armour, info.shells, info.mines, info.trees,
    info.carried_pills,
    info.man_status, info.man_x, info.man_y,
    -- A goal table without coordinates (init.lua's bare `{ kind = "none" }`
    -- after a kill) used to crash this format with "bad argument #20" and
    -- freeze the recorded bot for the rest of the session (crash logs from
    -- 2026-09-07 carrier_seed7 / everard, 2026-09-08 nolgm1_diag2).
    goal.kind, goal.substate or "-", goal.mx or -1, goal.my or -1,
    pf.status, pf.next_mx, pf.next_my,
    state._steer_lx or -1, state._steer_ly or -1,
    pf.age or 0, pf_open_size, pf_closed_size,
    path_str,
    cg,
    keys, taps, bld, bmode,
    -- ct: capacity tier (1..10) this tick ran at. Small int so a replay can
    -- correlate degraded behaviour with the tier the controller had settled on.
    phase, strength, base_strength, state._capacity_tier or 0,
    front_str,
    state.stuck_for, ev, rsn, obj_str))
  -- No clear needed here: take_accumulators() at the top of this function
  -- already handed us a detached copy and reset the module-level tables.
end

-- =========================================================================
-- Kill-path flush
-- =========================================================================
-- braincore.c's killed branch reaches this through the _G.brain_flush_killed
-- global (published in init.lua Brain.open). The tick-budget hook longjmps
-- out of Brain.think, so log_tick() never ran and this tick's accumulated
-- events/reasons are still sitting in the module-level tables.
--
-- We write them out as a killed-tick record and then clear UNCONDITIONALLY.
-- The clear is the actual leak fix: without it every following tick
-- re-serializes an ever-growing backlog, blows the budget in turn, and the
-- bot is killed every tick forever.
--
-- braincore.c disarms the budget hook before pcalling this, so the work here
-- is unbudgeted and safe. Cheap no-op when the logger is off or nothing
-- accumulated, so opt/ and non-debug runs pay one global lookup.
--
-- Record shape (its own `type` so readers that index the fixed tick fields
-- aren't handed a partial "tick" line; a killed tick has no `state`, so the
-- positional tick data simply doesn't exist):
--   {"type":"tick_killed","t":N,"killed":"<site>","ev":[...],"rsn":[...]}
function M.flush_killed(site)
  if not _G._JSONL_LOGGER_ENABLED then return end
  if #events == 0 and #reasons == 0
     and events_dropped == 0 and reasons_dropped == 0 then
    return
  end
  -- Swap out FIRST — whatever happens below, the next tick starts empty.
  local ev_t, rs_t, ev_drop, rs_drop = take_accumulators()
  if not file then return end
  local where = (site and site ~= "") and tostring(site) or "?"
  local esc   = where:gsub('\\', '\\\\'):gsub('"', '\\"')
  write_line(string.format(
    '{"type":"tick_killed","t":%d,"killed":"%s","ev":%s,"rsn":%s}\n',
    cur_tick, esc,
    events_json(ev_t, ev_drop), reasons_json(rs_t, rs_drop)))
end

return M
