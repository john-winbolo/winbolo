-- bitcompat — cross-VM bitwise ops for the 5.1-compatible brain source.
--
-- The brains are written in a Lua 5.1 subset (no `&|~<<>>` operators, no `//`)
-- so they load unmodified on both LuaJIT (Lua 5.1) and PUC-Lua 5.x. Bit ops go
-- through this module:
--   * On LuaJIT (and any Lua with the `bit` library), return the builtin — it
--     JIT-compiles and is as fast as the operators were.
--   * Otherwise (PUC-Lua 5.x), fall back to a pure-arithmetic 32-bit polyfill.
--
-- Both sides use signed 32-bit semantics, matching LuaJIT, so results agree
-- across VMs (the brains' bit usage is small coordinate/index packing, well
-- within 32 bits). No 5.3+ operators appear here, so it parses on every Lua.

local ok, jbit = pcall(require, "bit")
if ok and jbit then return jbit end

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
