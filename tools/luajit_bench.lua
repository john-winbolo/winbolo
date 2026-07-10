-- Representative pure-Lua brain kernel for LuaJIT-vs-PUC-5.4 benchmarking.
-- Mirrors the hot Lua the brains actually run: a binary-heap Dijkstra-ish
-- expansion over a packed grid, using exactly the 5.3/5.4 ops the transpiler
-- handles (// floor-div, >> << & ~ bitwise, incl. unary ~).
--
-- Deterministic (LCG, no math.random) so both VMs MUST produce the same
-- checksum — a correctness gate as well as a timer. Written in Lua 5.4 syntax;
-- run under lua54 directly, and under LuaJIT after tools/lua54to51.py.
--
-- Usage: lua54 luajit_bench.lua [iters]   /   luajit bench51.lua [iters]

local W = 256                      -- grid width (matches brain MAP_W-ish packing)
local function mkey(x, y) return y * W + x end
local function kx(k) return k & (W - 1) end          -- low bits
local function ky(k) return k >> 8 end                -- high bits

-- simple binary min-heap keyed by cost
local Heap = {}
Heap.__index = Heap
local function heap_new() return setmetatable({n = 0, c = {}, k = {}}, Heap) end
function Heap:push(key, cost)
  local n = self.n + 1; self.n = n
  self.c[n] = cost; self.k[n] = key
  while n > 1 do
    local p = n >> 1                                   -- parent (transpiled: bit.rshift)
    if self.c[p] <= self.c[n] then break end
    self.c[p], self.c[n] = self.c[n], self.c[p]
    self.k[p], self.k[n] = self.k[n], self.k[p]
    n = p
  end
end
function Heap:pop()
  local n = self.n
  if n == 0 then return nil end
  local key, cost = self.k[1], self.c[1]
  self.c[1], self.k[1] = self.c[n], self.k[n]
  self.c[n], self.k[n] = nil, nil
  self.n = n - 1; n = self.n
  local i = 1
  while true do
    local l = i << 1                                   -- left child (transpiled: bit.lshift)
    local r = l + 1
    local s = i
    if l <= n and self.c[l] < self.c[s] then s = l end
    if r <= n and self.c[r] < self.c[s] then s = r end
    if s == i then break end
    self.c[i], self.c[s] = self.c[s], self.c[i]
    self.k[i], self.k[s] = self.k[s], self.k[i]
    i = s
  end
  return key, cost
end

-- deterministic terrain cost from a packed key (bit-twiddly, like brain scoring)
local MASK = 0xFF
local function terrain_cost(k)
  local h = (k * 2654435761) & 0x7FFFFFFF              -- knuth hash, masked
  h = (h >> 13) ~ h                                    -- xorshift-ish (binary ~)
  local lo = h & MASK
  local blocked = (h & ~0xFFFFFF) ~= 0 and (lo % 17 == 0)  -- unary ~ here
  if blocked then return 1 / 0 end
  return 1 + (lo // 4)                                 -- floor-div
end

local function dijkstra(iters)
  local total = 0
  for it = 1, iters do
    local h = heap_new()
    local dist = {}
    local start = mkey(it % W, (it * 7) % W)
    h:push(start, 0)
    dist[start] = 0
    local expanded = 0
    while h.n > 0 and expanded < 600 do
      local k, d = h:pop()
      if d <= (dist[k] or 1/0) then
        expanded = expanded + 1
        local x, y = kx(k), ky(k)
        local nb = { mkey((x+1)&(W-1), y), mkey((x-1)&(W-1), y),
                     mkey(x, (y+1)&(W-1)), mkey(x, (y-1)&(W-1)) }
        for j = 1, 4 do
          local nk = nb[j]
          local nd = d + terrain_cost(nk)
          if nd < (dist[nk] or 1/0) then
            dist[nk] = nd
            h:push(nk, nd)
          end
        end
      end
    end
    total = total + expanded
  end
  return total
end

local iters = tonumber(arg and arg[1]) or 20000
local t0 = os.clock()
local checksum = dijkstra(iters)
local dt = os.clock() - t0
io.write(string.format("iters=%d  checksum=%d  time=%.4f s\n", iters, checksum, dt))
