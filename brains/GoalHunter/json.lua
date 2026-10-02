-- json.lua
--
-- Minimal hand-rolled JSON encoder for brain → host data
-- transport (panel data, log lines, etc.). Self-contained;
-- no dependencies. Encodes Lua tables produced by the brain
-- into compact JSON the host's cJSON parser consumes.
--
-- Limitations vs. a full encoder:
--   * Numbers are formatted with %.4g — floats keep ~4 sig
--     figs, integers come out as integers up to 2^31.
--   * NaN / +Inf serialize to null / 9999 (matches the
--     existing logger.lua convention so renderers don't
--     have to special-case).
--   * Mixed array/dict tables aren't supported — call sites
--     pick one or the other (we use is_array() to decide).
--   * String escaping covers \, ", \n, \r, \t, control bytes
--     are stripped (won't appear in any panel data anyway).
--
-- API: M.encode(value) → string

local M = {}

local function escape_str(s)
  -- Order matters: backslash first, then quote.
  s = s:gsub('\\', '\\\\')
       :gsub('"',  '\\"')
       :gsub('\n', '\\n')
       :gsub('\r', '\\r')
       :gsub('\t', '\\t')
  return '"' .. s .. '"'
end

local function is_array(t)
  -- Treat tables with a contiguous 1..#t integer-keyed sequence
  -- as arrays. Empty tables are treated as arrays (renders [],
  -- which the renderer can iterate as zero-length safely).
  local n = #t
  if n == 0 then
    -- Distinguish empty array from empty dict by checking for
    -- ANY key. With no keys it's ambiguous → default to array.
    for _ in pairs(t) do return false end
    return true
  end
  -- Check that all keys 1..n are present (and nothing else).
  local count = 0
  for _ in pairs(t) do count = count + 1 end
  return count == n
end

local encode  -- forward decl for recursion

local function encode_number(n)
  if n ~= n then return "null" end           -- NaN
  if n == math.huge  then return "9999" end
  if n == -math.huge then return "-9999" end
  -- Integer fast path — avoid trailing ".0" on whole numbers.
  if n == math.floor(n) and n > -2147483648 and n < 2147483647 then
    return string.format("%d", n)
  end
  return string.format("%.4g", n)
end

local function encode_array(t)
  local parts = {}
  for i, v in ipairs(t) do
    parts[i] = encode(v)
  end
  return "[" .. table.concat(parts, ",") .. "]"
end

local function encode_object(t)
  local parts = {}
  for k, v in pairs(t) do
    parts[#parts + 1] = escape_str(tostring(k)) .. ":" .. encode(v)
  end
  return "{" .. table.concat(parts, ",") .. "}"
end

encode = function(v)
  local t = type(v)
  if v == nil    then return "null" end
  if t == "string"  then return escape_str(v) end
  if t == "number"  then return encode_number(v) end
  if t == "boolean" then return tostring(v) end
  if t == "table" then
    return is_array(v) and encode_array(v) or encode_object(v)
  end
  return "null"
end

M.encode = encode

return M
