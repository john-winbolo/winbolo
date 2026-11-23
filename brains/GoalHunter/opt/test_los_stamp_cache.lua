-- =========================================================================
-- test_los_stamp_cache.lua — standalone unit tests (2026-11-23) for the LOS
-- stamp cache in attack.lua (the block after "local LOS_STAMPS_5DEG = nil":
-- compute_los_stamps, serialize_stamps, try_load, try_save).
-- Found 2026-11-23: serialize_stamps closed each [deg]={...,aims={ entry
-- with one brace, so loadfile failed (":509: '}' expected"), every brain
-- start computed the stamps again, and a debug run wrote the broken file
-- again. The tests run attack.lua's own block (cut out of the source, no
-- engine):
--   * with no cache file it computes the stamps (72 angles, 5 aims each);
--   * in debug mode it writes a file that loads, and that file's stamps
--     equal the computed ones;
--   * the committed los_stamp_cache.lua loads (try_load) and its stamps
--     equal freshly computed ones;
--   * the committed file has the version line of today's standoff radius.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_los_stamp_cache.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end

-- attack.lua's LOS stamp block, run in its own environment.
-- (lua_strip writes opt/ with CRLF line ends; git keeps LF)
local src = assert(io.open("attack.lua", "rb")):read("*a"):gsub("\r\n", "\n")
local s0 = src:find("local LOS_STAMPS_5DEG = nil\n", 1, true)
local s1 = s0 and src:find("\n-- Copy precomputed stamps", s0, true)
check("attack.lua has the LOS stamp block", s0 and s1, s0)
local block = src:sub(s0, s1)

-- run(dir, debug, files): BRAIN_DIR = dir, BRAIN_DEBUG_MODE = debug. files
-- (optional) catches files opened for writing: files[path] = text. Answers
-- the stamps and the lines the block printed.
local function run(dir, debug, files)
  local env = setmetatable({}, { __index = _G })
  env._G = env
  env.BRAIN_DIR = dir
  env.BRAIN_DEBUG_MODE = debug
  local said = {}
  env.print = function(...) said[#said + 1] = table.concat({ ... }, " ") end
  if files then
    env.io = setmetatable({
      open = function(path, mode)
        if mode and mode:find("w", 1, true) then
          local buf = {}
          return { write = function(_, ...) for _, v in ipairs({ ... }) do buf[#buf + 1] = v end end,
                   close = function() files[path] = table.concat(buf) end }
        end
        return io.open(path, mode)
      end }, { __index = io })
  end
  local chunk = assert(loadstring("local C = require('constants')\n" .. block ..
                                  "\nreturn LOS_STAMPS_5DEG\n", "=los_block"))
  setfenv(chunk, env)
  return chunk(), table.concat(said, "\n")
end

-- Deep equal by value; answers true or the path of the first difference.
local function deq(a, b, path)
  path = path or "S"
  if type(a) ~= type(b) then return path .. " type " .. type(a) .. " vs " .. type(b) end
  if type(a) ~= "table" then
    if a == b then return true end
    return path .. " " .. tostring(a) .. " vs " .. tostring(b)
  end
  for k, v in pairs(a) do
    local r = deq(v, b[k], path .. "." .. tostring(k))
    if r ~= true then return r end
  end
  for k in pairs(b) do
    if a[k] == nil then return path .. "." .. tostring(k) .. " missing" end
  end
  return true
end

print("compute")
local fresh, said = run("./__no_cache_here__", false)
check("no cache file: computes", said:find("Computing LOS stamps", 1, true) ~= nil, said)
local n, aims_ok = 0, true
for deg = 0, 355, 5 do
  local st = fresh[deg]
  if st then n = n + 1 end
  if not st or #st.aims ~= 5 then aims_ok = false end
end
check("72 angles", n == 72, n)
check("5 aims at every angle", aims_ok)
check("not written outside debug mode", not said:find("written", 1, true), said)

print("write and load back")
local files = {}
local _, said2 = run("./__no_cache_here__", true, files)
local path = "./__no_cache_here__/los_stamp_cache.lua"
local text = files[path]
check("debug mode writes the cache file", text ~= nil, said2)
local chunk, err = loadstring(text or "", "=written")
check("the written file loads", chunk ~= nil, err)
local data = chunk and chunk()
check("the written stamps equal the computed ones", data and deq(fresh, data) == true,
      data and deq(fresh, data))
check("the written file has the version line",
      text and text:match("^[^\n]*\n%-%- version: R=([%d%.]+)\n") == string.format("%.4f", C.ATTACK_PILL_STANDOFF),
      text and text:sub(1, 80))

print("committed file")
local cf = assert(io.open("los_stamp_cache.lua", "rb")):read("*a")
local cchunk, cerr = loadfile("los_stamp_cache.lua")
check("los_stamp_cache.lua loads", cchunk ~= nil, cerr)
check("los_stamp_cache.lua has LF line ends only", not cf:find("\r", 1, true))
local loaded, said3 = run(".", false)
check("try_load takes the committed file", said3:find("loaded from cache", 1, true) ~= nil, said3)
check("the committed stamps equal freshly computed ones", deq(fresh, loaded) == true, deq(fresh, loaded))
check("the committed file is what the writer makes now", text == cf, #cf .. " vs " .. #(text or ""))

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
