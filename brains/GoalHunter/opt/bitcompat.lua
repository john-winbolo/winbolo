-- bitcompat — cross-VM bitwise ops for the 5.1-compatible brain source.
--
-- The brains are written in a Lua 5.1 subset (no `&|~<<>>` operators, no `//`)
-- so they load unmodified on both LuaJIT (Lua 5.1) and PUC-Lua 5.x. Bit ops go
-- through this module. Resolution order:
--
--   1. LuaJIT (or any Lua exposing the `bit` library) -> return the builtin. It
--      JIT-compiles and is as fast as the operators were. Signed 32-bit.
--   2. PUC-Lua 5.3+ -> the NATIVE `& | ~ << >>` operators, loaded from a string
--      so this file still PARSES on 5.1. These are 64-bit-integer semantics —
--      BIT-EXACT with the operators the brain source used before the down-
--      conversion, so bot behaviour on PUC-Lua matches the pre-conversion 5.4
--      brain exactly (and runs at native speed, not the polyfill's per-bit loop).
--   3. Older PUC-Lua (5.1/5.2, no `bit` lib) -> pure-arithmetic 32-bit polyfill.
--
-- No 5.3+ operator tokens appear at file scope, so it parses on every Lua.

local ok, jbit = pcall(require, "bit")
if ok and jbit then return jbit end

-- PUC-Lua 5.3+: use the native bitwise operators. They can't appear literally in
-- this file (it must parse under 5.1/LuaJIT), so build them from a string that
-- is only ever compiled on a VM whose parser accepts them. `>>` is a logical
-- shift over the 64-bit integer, matching what the brain's `>>` did natively.
local loader = loadstring or load
if loader then
  local chunk = loader([[
    return {
      tobit   = function(a)    return a end,
      band    = function(a, b) return a & b end,
      bor     = function(a, b) return a | b end,
      bxor    = function(a, b) return a ~ b end,
      bnot    = function(a)    return ~a end,
      lshift  = function(a, n) return a << n end,
      rshift  = function(a, n) return a >> n end,
      arshift = function(a, n) return a >> n end,
    }
  ]])
  if chunk then
    local okc, native = pcall(chunk)
    if okc and type(native) == "table" then return native end
  end
end

-- Fallback: pure-arithmetic 32-bit polyfill (plain 5.1, no bit lib). Signed
-- 32-bit semantics; only exact for values within 32 bits.
local floor = math.floor
local TWO32 = 4294967296
local TWO31 = 2147483648

local function tobit(x)
  x = x % TWO32
  if x >= TWO31 then x = x - TWO32 end
  return x
end

local M = {}

function M.tobit(x) return tobit(floor(x)) end

function M.band(a, b)
  a = a % TWO32; b = b % TWO32
  local r, p = 0, 1
  for _ = 1, 32 do
    if a % 2 == 1 and b % 2 == 1 then r = r + p end
    a = floor(a / 2); b = floor(b / 2); p = p + p
  end
  return tobit(r)
end

function M.bor(a, b)
  a = a % TWO32; b = b % TWO32
  local r, p = 0, 1
  for _ = 1, 32 do
    if a % 2 == 1 or b % 2 == 1 then r = r + p end
    a = floor(a / 2); b = floor(b / 2); p = p + p
  end
  return tobit(r)
end

function M.bxor(a, b)
  a = a % TWO32; b = b % TWO32
  local r, p = 0, 1
  for _ = 1, 32 do
    if (a % 2) ~= (b % 2) then r = r + p end
    a = floor(a / 2); b = floor(b / 2); p = p + p
  end
  return tobit(r)
end

function M.bnot(a) return tobit(-1 - a) end                 -- ~a == -a-1 (two's complement)

-- Integer powers of two, so shifts stay integer-typed on PUC 5.4 (matching the
-- native << / >> the source used before) rather than going float via `2^n`.
-- Exact as doubles too (<= 2^53), so LuaJIT's fallback path agrees.
local POW2 = {}
do local v = 1; for i = 0, 53 do POW2[i] = v; v = v + v end end

function M.lshift(a, n) return tobit((a % TWO32) * POW2[n]) end
function M.rshift(a, n) return tobit(floor((a % TWO32) / POW2[n])) end
function M.arshift(a, n) return floor(tobit(a) / POW2[n]) end

return M
