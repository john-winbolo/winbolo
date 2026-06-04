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

function M.open(filename)
  -- If reopening to a new path, remove the old file (avoids stale CWD copies)
  if current_filename and current_filename ~= filename then
    os.remove(current_filename)
  end
  file = io.open(filename, "w")
  if file then
    current_filename = filename
    print(TAG .. " LOG: opened " .. filename)
  else
    print(TAG .. " LOG: FAILED to open " .. filename)
  end
  events = {}
  return file ~= nil
end

function M.close()
  if file then
    file:close()
    print(TAG .. " LOG: closed")
  end
  file   = nil
  events = {}
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
      local tt = get_terrain(mx, my) & TERRAIN_MASK
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

function M.log_tick(state, info, goal, keys, taps, build_cmd)
  -- Runtime gate: skip writes when the JSONL logger flag is off.
  -- The C side toggles _G._JSONL_LOGGER_ENABLED whenever the user
  -- changes the checkbox in the BrainTest debug modules panel.
  if not _G._JSONL_LOGGER_ENABLED then return end

  -- Lazy open: if the user enabled the flag mid-session and there's
  -- no open file yet, open one now using the standard naming scheme.
  if not file then
    local fname = M.make_filename("player" .. (info.player_number or 0))
    if not M.open(fname) then return end
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

  -- Events array
  local ev = "[]"
  if #events > 0 then
    local parts = {}
    for _, e in ipairs(events) do
      parts[#parts + 1] = '"' .. e:gsub('"', '\\"') .. '"'
    end
    ev = "[" .. table.concat(parts, ",") .. "]"
  end

  -- Reasoning array
  local rsn = "[]"
  if #reasons > 0 then
    local parts = {}
    for _, r in ipairs(reasons) do
      parts[#parts + 1] = string.format('{"cat":"%s","d":%s}',
        r.cat, to_json_obj(r.data))
    end
    rsn = "[" .. table.concat(parts, ",") .. "]"
  end

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

  file:write(string.format(
    '{"type":"tick","t":%d,'
    .. '"tx":%d,"ty":%d,"mx":%d,"my":%d,"dir":%d,"spd":%d,'
    .. '"boat":%s,"arm":%d,"sh":%d,"mi":%d,"tr":%d,"cpill":%d,'
    .. '"lgm":%d,"lx":%d,"ly":%d,'
    .. '"goal":"%s","gsub":"%s","gx":%d,"gy":%d,'
    .. '"pf":"%s","nx":%d,"ny":%d,"lax":%d,"lay":%d,"pfage":%d,"pfopen":%d,"pfclosed":%d,'
    .. '"path":%s,'
    .. '"cmd":%s,'
    .. '"keys":%d,"taps":%d,"bld":%s,"bmode":"%s",'
    .. '"phase":"%s","str":%.2f,"bstr":%.2f,'
    .. '%s'
    .. '"stuck":%d,"ev":%s,"rsn":%s,"objs":%s}\n',
    state.tick,
    info.tankx, info.tanky, info.tankx >> 8, info.tanky >> 8,
    info.direction, info.speed,
    tostring(info.inboat), info.armour, info.shells, info.mines, info.trees,
    info.carried_pills,
    info.man_status, info.man_x, info.man_y,
    goal.kind, goal.substate or "-", goal.mx, goal.my,
    pf.status, pf.next_mx, pf.next_my,
    state._steer_lx or -1, state._steer_ly or -1,
    pf.age or 0, pf_open_size, pf_closed_size,
    path_str,
    cg,
    keys, taps, bld, bmode,
    phase, strength, base_strength,
    front_str,
    state.stuck_for, ev, rsn, obj_str))

  -- Flush every tick so the log is always up-to-date for live reading
  file:flush()

  -- Clear events and reasons for next tick
  events  = {}
  reasons = {}
end

return M
