-- =========================================================================
-- NewAutopilot/debugger.lua — Trace capture for BrainTest debugger
-- =========================================================================
--
-- Captures one tick's execution via debug.sethook.  Designed for Lua 5.4.
--
-- Usage:
--   debugger.arm()                    -- mark "capture next tick"
--   debugger.begin_trace(think_fn)    -- call at start of think()
--   ... think() runs ...
--   debugger.end_trace(think_fn)      -- call at end of think()
--   local data = debugger.get_trace_data()
-- =========================================================================

local M = {}

local C = require("constants")

-- -------------------------------------------------------------------------
-- Internal state
-- -------------------------------------------------------------------------
local _armed   = false
local _tracing = false

local _entries       = {}   -- trace entries collected by the hook
local _entry_count   = 0
local _state_before  = nil
local _state_after   = nil
local _sources       = {}
local _stack_depth   = 0

-- -------------------------------------------------------------------------
-- 1. deep_copy — recursive table copy (skip functions, userdata, threads)
-- -------------------------------------------------------------------------
function M.deep_copy(val, visited)
    local t = type(val)
    if t ~= "table" then
        -- primitives (number, string, boolean, nil) pass through;
        -- function, userdata, thread are skipped
        if t == "function" or t == "userdata" or t == "thread" then
            return nil
        end
        return val
    end

    visited = visited or {}
    if visited[val] then return visited[val] end

    local copy = {}
    visited[val] = copy

    for k, v in pairs(val) do
        local kc = M.deep_copy(k, visited)
        local vc = M.deep_copy(v, visited)
        if kc ~= nil then
            copy[kc] = vc
        end
    end

    local mt = getmetatable(val)
    if mt then setmetatable(copy, mt) end

    return copy
end

-- -------------------------------------------------------------------------
-- 2. capture_upvalues — snapshot all upvalues of a function
-- -------------------------------------------------------------------------
function M.capture_upvalues(fn)
    local ups = {}
    local i = 1
    while true do
        local name, value = debug.getupvalue(fn, i)
        if name == nil then break end
        ups[name] = M.deep_copy(value)
        i = i + 1
    end
    return ups
end

-- -------------------------------------------------------------------------
-- 3. capture_locals — snapshot locals at a given stack level
-- -------------------------------------------------------------------------
function M.capture_locals(level)
    local loc = {}
    local i = 1
    while true do
        local name, value = debug.getlocal(level, i)
        if name == nil then break end
        -- skip temporaries (compiler-generated names start with '(')
        if name:sub(1, 1) ~= "(" then
            loc[name] = M.deep_copy(value)
        end
        i = i + 1
    end
    return loc
end

-- -------------------------------------------------------------------------
-- 4-5. arm / is_armed / is_tracing
-- -------------------------------------------------------------------------
function M.arm()
    _armed = true
end

function M.is_armed()
    return _armed
end

function M.is_tracing()
    return _tracing
end

-- -------------------------------------------------------------------------
-- 7. Hook function — installed by begin_trace
-- -------------------------------------------------------------------------

-- Lightweight local capture: grabs names+values without deep_copy.
-- Values are mostly numbers/strings/bools; table references are snapshots
-- of what the variable pointed to at that moment (shared, not copied).
local function capture_locals_raw(level)
    local loc = {}
    local i = 1
    while true do
        local name, value = debug.getlocal(level, i)
        if name == nil then break end
        if name:sub(1, 1) ~= "(" then
            loc[name] = value
        end
        i = i + 1
    end
    return loc
end

local function trace_hook(event, line)
    -- Stack level 2 = the function being hooked (1 = this hook itself)
    local info = debug.getinfo(2, "Sl")
    if not info then return end

    -- Extract short source name (strip path, keep filename)
    local src = info.short_src or info.source or "?"
    local fname = src:match("[/\\]([^/\\]+)$") or src

    local entry

    if event == "line" then
        -- Line events: capture locals only (cheapest path)
        _entry_count = _entry_count + 1
        entry = {
            event       = "l",
            source      = fname,
            line        = line,
            locals      = capture_locals_raw(3),  -- 3: hook(1) -> sethook-dispatch(2) -> target(3)
            stack_depth = _stack_depth,
        }
        _entries[_entry_count] = entry

    elseif event == "call" or event == "tail call" then
        _stack_depth = _stack_depth + 1
        _entry_count = _entry_count + 1
        entry = {
            event       = (event == "call") and "c" or "tc",
            source      = fname,
            line        = line or (info.linedefined or 0),
            locals      = capture_locals_raw(3),
            stack_depth = _stack_depth,
        }
        _entries[_entry_count] = entry

    elseif event == "return" or event == "tail return" then
        _entry_count = _entry_count + 1
        entry = {
            event       = "r",
            source      = fname,
            line        = line or (info.lastlinedefined or 0),
            locals      = capture_locals_raw(3),
            stack_depth = _stack_depth,
        }
        _entries[_entry_count] = entry
        _stack_depth = math.max(0, _stack_depth - 1)
    end
end

-- -------------------------------------------------------------------------
-- 6. begin_trace — start capturing
-- -------------------------------------------------------------------------
function M.begin_trace(think_fn)
    if not _armed then return end
    _armed   = false
    _tracing = true

    -- Reset collection
    _entries     = {}
    _entry_count = 0
    _stack_depth = 0

    -- Snapshot upvalues before execution
    _state_before = M.capture_upvalues(think_fn)

    -- Install hook for line, call, and return events
    debug.sethook(trace_hook, "lcr")
end

-- -------------------------------------------------------------------------
-- 8. end_trace — stop capturing, snapshot final state
-- -------------------------------------------------------------------------
function M.end_trace(think_fn)
    debug.sethook()  -- remove hook

    if not _tracing then return end
    _tracing = false

    -- Snapshot upvalues after execution
    _state_after = M.capture_upvalues(think_fn)
end

-- -------------------------------------------------------------------------
-- 9. snapshot_sources — read all .lua files from brain directory
-- -------------------------------------------------------------------------
function M.snapshot_sources(brain_dir)
    local sources = {}
    -- Try to list .lua files in the brain directory
    -- Use io.popen for directory listing (cross-platform best-effort)
    local cmd
    if package.config:sub(1, 1) == "\\" then
        -- Windows
        cmd = 'dir /b "' .. brain_dir .. '\\*.lua" 2>nul'
    else
        -- Unix
        cmd = 'ls -1 "' .. brain_dir .. '"/*.lua 2>/dev/null'
    end

    local handle = io.popen(cmd)
    if handle then
        for filename in handle:lines() do
            -- On Windows, dir /b gives just the filename
            -- On Unix, ls gives the full path
            local basename = filename:match("[/\\]([^/\\]+)$") or filename
            local path = brain_dir .. "/" .. basename
            local f = io.open(path, "r")
            if not f then
                -- Try backslash on Windows
                path = brain_dir .. "\\" .. basename
                f = io.open(path, "r")
            end
            if f then
                sources[basename] = f:read("*a")
                f:close()
            end
        end
        handle:close()
    end

    _sources = sources
    return sources
end

-- -------------------------------------------------------------------------
-- 10. get_trace_data — return the collected trace
-- -------------------------------------------------------------------------
function M.get_trace_data()
    return {
        entries      = _entries,
        state_before = _state_before,
        state_after  = _state_after,
        sources      = _sources,
        entry_count  = _entry_count,
    }
end

-- -------------------------------------------------------------------------
-- 11. get_entry_locals — deep-copy locals for a specific entry on demand
-- -------------------------------------------------------------------------
function M.get_entry_locals(index)
    local entry = _entries[index]
    if not entry then return nil end
    return M.deep_copy(entry.locals)
end

-- -------------------------------------------------------------------------
-- has_trace — returns true if trace data is available
-- -------------------------------------------------------------------------
function M.has_trace()
    return _entry_count > 0 and not _tracing
end

-- -------------------------------------------------------------------------
-- serialize_value — convert a Lua value to a JSON-ish string
-- -------------------------------------------------------------------------
local function serialize_value(val, depth, visited)
    if depth > 8 then return '"<too deep>"' end
    local t = type(val)
    if t == "nil" then return "null"
    elseif t == "boolean" then return val and "true" or "false"
    elseif t == "number" then
        if val ~= val then return '"NaN"' end
        if val == math.huge then return '"Inf"' end
        if val == -math.huge then return '"-Inf"' end
        return string.format("%.6g", val)
    elseif t == "string" then
        local s = val:gsub('\\', '\\\\'):gsub('"', '\\"')
                     :gsub('\n', '\\n'):gsub('\r', '\\r')
                     :gsub('\t', '\\t')
                     :gsub('[%z\1-\31]', function(c)
                         return string.format('\\u%04x', string.byte(c))
                     end)
        return '"' .. s .. '"'
    elseif t == "table" then
        visited = visited or {}
        if visited[val] then return '"<cycle>"' end
        visited[val] = true
        local parts = {}
        local n = #val
        if n > 0 then
            parts[#parts+1] = "["
            for i = 1, math.min(n, 100) do
                if i > 1 then parts[#parts+1] = "," end
                parts[#parts+1] = serialize_value(val[i], depth+1, visited)
            end
            if n > 100 then parts[#parts+1] = ',"<...>' .. n .. ' items>"' end
            parts[#parts+1] = "]"
        else
            parts[#parts+1] = "{"
            local count = 0
            for k, v in pairs(val) do
                if count > 0 then parts[#parts+1] = "," end
                if count >= 50 then parts[#parts+1] = '"<...>":"truncated"'; break end
                local ks = type(k) == "string" and k or tostring(k)
                parts[#parts+1] = '"' .. ks:gsub('"', '\\"') .. '":'
                parts[#parts+1] = serialize_value(v, depth+1, visited)
                count = count + 1
            end
            parts[#parts+1] = "}"
        end
        visited[val] = nil
        return table.concat(parts)
    else
        return '"<' .. t .. '>"'
    end
end

-- -------------------------------------------------------------------------
-- get_trace_json — serialize the trace to a JSON string for C side
-- -------------------------------------------------------------------------
function M.get_trace_json()
    if not M.has_trace() then return nil end

    local parts = {}
    parts[#parts+1] = '{"entry_count":' .. _entry_count

    parts[#parts+1] = ',"entries":['
    local limit = math.min(_entry_count, 10000)
    for i = 1, limit do
        if i > 1 then parts[#parts+1] = "," end
        local e = _entries[i]
        parts[#parts+1] = string.format(
            '{"event":"%s","source":"%s","line":%d,"depth":%d,"locals":%s}',
            e.event, e.source:gsub('"', '\\"'), e.line, e.stack_depth,
            serialize_value(e.locals, 0))
    end
    parts[#parts+1] = "]"

    parts[#parts+1] = ',"state_before":' .. serialize_value(_state_before, 0)
    parts[#parts+1] = ',"state_after":' .. serialize_value(_state_after, 0)
    parts[#parts+1] = ',"sources":' .. serialize_value(_sources, 0)

    parts[#parts+1] = "}"
    return table.concat(parts)
end

-- -------------------------------------------------------------------------
-- snapshot_state_json — capture Brain.think upvalues as JSON string
-- Called from C every tick for recording. Needs Brain.think reference.
-- -------------------------------------------------------------------------
local _think_fn = nil  -- set once by register_think

-- Upvalues that are constant module references — skip to save time/space
-- Modules NOT skipped (data persists in M.<field> for replay fidelity):
--   danger, threat, hearing, world
-- Their function fields are silently dropped by the table walker; only the
-- data fields survive into the snapshot.
local _skip_upvalues = {
    _ENV = true,
    C = true, TAG = true, U = true, dbg = true, heap = true,
    W = true, expl = true, PF = true, cpf = true, wsim = true,
    cmds = true, goals = true, attack = true, steer = true,
    log = true, builder = true, metrics = true, shot_tracker = true,
    changes = true, percept = true, strategy = true, comms = true,
    Brain = true, print2 = true,
    AUTOSTART = true, ENABLE_LOGGING = true, INITIAL_SUBSTATE = true,
}

-- Keys within tables whose values are large binary blobs
local _skip_keys = {
    viewdata = true,
    _real_print = true,
}

function M.register_think(fn)
    _think_fn = fn
end

-- Serialize value but skip known-useless keys
local function serialize_state_value(val, depth, visited)
    -- (also exposed as M.serialize_state_value below)
    if depth > 6 then return '"<deep>"' end
    local t = type(val)
    if t ~= "table" then
        return serialize_value(val, depth, visited)
    end

    visited = visited or {}
    if visited[val] then return '"<cycle>"' end
    visited[val] = true

    local parts = {}
    local n = #val
    if n > 0 then
        parts[#parts+1] = "["
        for i = 1, math.min(n, 50) do
            if i > 1 then parts[#parts+1] = "," end
            parts[#parts+1] = serialize_state_value(val[i], depth+1, visited)
        end
        if n > 50 then parts[#parts+1] = ',"<...>' .. n .. ' items>"' end
        parts[#parts+1] = "]"
    else
        parts[#parts+1] = "{"
        local count = 0
        -- Emit important keys first so they survive the 50-key truncation
        local _priority = {"goal","phase","tick","pf","kind","substate","mx","my","target_id"}
        local _emitted = {}
        for _, pk in ipairs(_priority) do
            if val[pk] ~= nil and not _skip_keys[pk] then
                if count > 0 then parts[#parts+1] = "," end
                parts[#parts+1] = '"' .. pk .. '":'
                parts[#parts+1] = serialize_state_value(val[pk], depth+1, visited)
                count = count + 1
                _emitted[pk] = true
            end
        end
        for k, v in pairs(val) do
            local ks = type(k) == "string" and k or tostring(k)
            if not _skip_keys[ks] and not _emitted[ks] then
                if count > 0 then parts[#parts+1] = "," end
                if count >= 50 then parts[#parts+1] = '"<...>":"truncated"'; break end
                parts[#parts+1] = '"' .. ks:gsub('"', '\\"') .. '":'
                parts[#parts+1] = serialize_state_value(v, depth+1, visited)
                count = count + 1
            end
        end
        parts[#parts+1] = "}"
    end
    visited[val] = nil
    return table.concat(parts)
end
M.serialize_state_value = serialize_state_value
M.serialize_value = serialize_value

function M.snapshot_state_json()
    if not _think_fn then return nil end

    local ok, result = pcall(function()
        local parts = {"{"}
        local first = true
        local i = 1
        while true do
            local name, value = debug.getupvalue(_think_fn, i)
            if name == nil then break end
            if not _skip_upvalues[name] then
                if not first then parts[#parts+1] = "," end
                first = false
                parts[#parts+1] = '"' .. name .. '":'
                parts[#parts+1] = serialize_state_value(value, 0)
            end
            i = i + 1
        end
        parts[#parts+1] = "}"
        return table.concat(parts)
    end)

    if ok then return result end
    print("[debugger] snapshot_state_json error: " .. tostring(result))
    return nil
end

-- =========================================================================
-- ldump-based state snapshot (lossless: preserves metatables, functions,
-- numeric keys, circular refs, shared table identity)
-- =========================================================================

local ldump = require("ldump")
ldump.require_path = "ldump"
ldump.strict_mode = false  -- warn instead of error on unserializable values
ldump.preserve_modules = true  -- serialize modules via require()

-- Keys inside tables that hold C functions or other unserializable values
local _strip_state_keys = {
    _real_print = true,
}

-- Recursively strip unserializable keys from a table (shallow copy)
local function strip_for_ldump(t, depth)
    if type(t) ~= "table" or depth > 4 then return t end
    local out = {}
    for k, v in pairs(t) do
        if not _strip_state_keys[k] then
            out[k] = v  -- don't recurse into subtables (ldump handles them)
        end
    end
    local mt = getmetatable(t)
    if mt then setmetatable(out, mt) end
    return out
end

function M.snapshot_state_ldump()
    if not _think_fn then return nil end

    -- Collect mutable upvalues into a table, stripping unserializable refs
    local state_data = {}
    local i = 1
    while true do
        local name, value = debug.getupvalue(_think_fn, i)
        if name == nil then break end
        if not _skip_upvalues[name] then
            if name == "state" and type(value) == "table" then
                state_data[name] = strip_for_ldump(value, 0)
            else
                state_data[name] = value
            end
        end
        i = i + 1
    end

    -- Capture C pathfinder state for exact trace replay.
    -- cpf_serialize returns a binary string of grids + Dijkstra slates.
    if cpf_serialize then
        local pfblob = cpf_serialize()
        if pfblob then state_data._cpf_blob = pfblob end
    end

    local ok, result = pcall(ldump, state_data)
    if ok then return result end
    print("[debugger] snapshot_state_ldump error: " .. tostring(result))
    return nil
end

function M.restore_state_ldump(data)
    local function dbglog(msg)
        local f = io.open("ldump_debug.log", "a")
        if f then f:write(msg .. "\n"); f:flush(); f:close() end
    end

    if not _think_fn or not data then
        dbglog("restore: no think_fn or no data")
        return false
    end

    dbglog("restore: loading " .. #data .. " bytes...")
    local loader, err = load(data)
    if not loader then
        dbglog("restore load error: " .. tostring(err))
        return false
    end

    dbglog("restore: executing...")
    local ok, restored = pcall(loader)
    if not ok or type(restored) ~= "table" then
        dbglog("restore exec error: " .. tostring(restored))
        return false
    end
    dbglog("restore: restoring " .. (restored and "table" or "nil") .. " upvalues...")

    -- Module upvalues whose live tables hold function refs we must NOT
    -- replace. Instead, merge restored data fields into the existing table
    -- (replacing data, preserving functions).
    local MERGE_UPVALUES = {
        danger = true, threat = true, hearing = true, world = true,
    }

    -- Set upvalues on Brain.think from restored data
    local i = 1
    while true do
        local name, current = debug.getupvalue(_think_fn, i)
        if name == nil then break end
        if not _skip_upvalues[name] then
            local rv = restored[name]
            if rv ~= nil then
                if MERGE_UPVALUES[name] and type(current) == "table" and type(rv) == "table" then
                    -- Wipe existing data fields (keep functions/userdata/etc.)
                    for k, v in pairs(current) do
                        local vt = type(v)
                        if vt ~= "function" and vt ~= "userdata" and vt ~= "thread" then
                            current[k] = nil
                        end
                    end
                    -- Copy restored data over
                    for k, v in pairs(rv) do
                        current[k] = v
                    end
                else
                    debug.setupvalue(_think_fn, i, rv)
                end
            end
        end
        i = i + 1
    end

    -- Restore C pathfinder state from the captured blob.
    if restored._cpf_blob and cpf_deserialize then
        local pfok = cpf_deserialize(restored._cpf_blob)
        dbglog("restore: cpf_deserialize = " .. tostring(pfok))
    end

    return true
end

-- =========================================================================
-- JSON decoder (minimal, handles our serialize_value output)
-- =========================================================================

local json_decode  -- forward declaration

local function json_skip_ws(s, i)
    while i <= #s do
        local c = s:byte(i)
        if c == 32 or c == 9 or c == 10 or c == 13 then i = i + 1
        else break end
    end
    return i
end

local function json_decode_string(s, i)
    i = i + 1  -- skip opening "
    local parts = {}
    while i <= #s do
        local c = s:sub(i, i)
        if c == '"' then return table.concat(parts), i + 1 end
        if c == '\\' then
            i = i + 1
            local esc = s:sub(i, i)
            if     esc == 'n' then parts[#parts+1] = '\n'
            elseif esc == 'r' then parts[#parts+1] = '\r'
            elseif esc == 't' then parts[#parts+1] = '\t'
            elseif esc == '"' then parts[#parts+1] = '"'
            elseif esc == '\\' then parts[#parts+1] = '\\'
            elseif esc == '/' then parts[#parts+1] = '/'
            elseif esc == 'u' then
                local hex = s:sub(i+1, i+4)
                local code = tonumber(hex, 16)
                if code then parts[#parts+1] = string.char(code) end
                i = i + 4
            else parts[#parts+1] = esc end
        else parts[#parts+1] = c end
        i = i + 1
    end
    return table.concat(parts), i
end

local function json_decode_number(s, i)
    local j = i
    if s:byte(j) == 45 then j = j + 1 end
    while j <= #s and s:byte(j) >= 48 and s:byte(j) <= 57 do j = j + 1 end
    if j <= #s and s:byte(j) == 46 then
        j = j + 1
        while j <= #s and s:byte(j) >= 48 and s:byte(j) <= 57 do j = j + 1 end
    end
    if j <= #s and (s:byte(j) == 101 or s:byte(j) == 69) then
        j = j + 1
        if j <= #s and (s:byte(j) == 43 or s:byte(j) == 45) then j = j + 1 end
        while j <= #s and s:byte(j) >= 48 and s:byte(j) <= 57 do j = j + 1 end
    end
    return tonumber(s:sub(i, j-1)), j
end

json_decode = function(s, i)
    i = json_skip_ws(s, i or 1)
    if i > #s then return nil, i end
    local c = s:sub(i, i)
    if c == '"' then return json_decode_string(s, i)
    elseif c == '{' then
        local obj = {}
        i = json_skip_ws(s, i + 1)
        if s:sub(i, i) == '}' then return obj, i + 1 end
        while true do
            i = json_skip_ws(s, i)
            local key; key, i = json_decode_string(s, i)
            i = json_skip_ws(s, i) + 1  -- skip ':'
            local val; val, i = json_decode(s, i)
            obj[key] = val
            i = json_skip_ws(s, i)
            if s:sub(i, i) == ',' then i = i + 1 else break end
        end
        if s:sub(i, i) == '}' then i = i + 1 end
        return obj, i
    elseif c == '[' then
        local arr = {}
        i = json_skip_ws(s, i + 1)
        if s:sub(i, i) == ']' then return arr, i + 1 end
        while true do
            local val; val, i = json_decode(s, i)
            arr[#arr+1] = val
            i = json_skip_ws(s, i)
            if s:sub(i, i) == ',' then i = i + 1 else break end
        end
        if s:sub(i, i) == ']' then i = i + 1 end
        return arr, i
    elseif c == 't' then return true, i + 4
    elseif c == 'f' then return false, i + 5
    elseif c == 'n' then return nil, i + 4
    else return json_decode_number(s, i) end
end

M.json_decode = json_decode

-- =========================================================================
-- Upvalue restoration: set Brain.think upvalues from parsed JSON table
-- =========================================================================

-- Returns true if `tbl` has at least one function-valued field at any
-- depth. Used to recognize live module references that would be broken
-- by restoring from a snapshot (deep_copy strips functions).
local function table_has_function(tbl, seen)
    if type(tbl) ~= "table" then return false end
    seen = seen or {}
    if seen[tbl] then return false end
    seen[tbl] = true
    for _, v in pairs(tbl) do
        local tv = type(v)
        if tv == "function" then return true end
        if tv == "table" and table_has_function(v, seen) then return true end
    end
    return false
end

function M.restore_upvalues(fn, state_table)
    if not fn or not state_table then return 0 end
    local count = 0
    local i = 1
    while true do
        local name, live_value = debug.getupvalue(fn, i)
        if name == nil then break end
        if state_table[name] ~= nil then
            -- Skip module upvalues: if the live value has function fields
            -- and the snapshot copy doesn't, the snapshot is just stripped
            -- module data — overwriting would nil out the methods.
            local snap_value = state_table[name]
            local skip = type(live_value) == "table"
                     and table_has_function(live_value)
                     and not table_has_function(snap_value)
            if not skip then
                debug.setupvalue(fn, i, snap_value)
                count = count + 1
            end
        end
        i = i + 1
    end
    return count
end

-- =========================================================================
-- run_traced_think: full hook capture, returns trace table
-- Called from C on the trace thread's isolated Lua state.
-- abort_check is a C function that returns true to stop early.
-- =========================================================================

function M.run_traced_think(think_fn, info, abort_check)
    _entries = {}
    _entry_count = 0
    _stack_depth = 0
    _tracing = true

    _state_before = M.capture_upvalues(think_fn)

    debug.sethook(function(event, line)
        if abort_check and abort_check() then
            debug.sethook()
            return
        end
        trace_hook(event, line)
    end, "lcr")

    local ok, result = pcall(think_fn, info)

    debug.sethook()
    _tracing = false
    _state_after = M.capture_upvalues(think_fn)

    return {
        ok = ok,
        result = result,
        entries = _entries,
        entry_count = _entry_count,
        state_before = _state_before,
        state_after = _state_after,
    }
end

return M
