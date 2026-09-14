-- probe_pairs.lua — diagnostic brain: is table iteration order stable across runs?
--
-- PUC-Lua 5.4 randomises its string hash seed per process (lstate.c, luai_makeseed),
-- so `pairs` over a string-keyed table walks in a different order on every launch.
-- Any "first equal-cost candidate wins" tie-break in a brain then resolves
-- differently run to run, and two same-seed matches diverge no matter how much
-- else is pinned. LuaJIT, and PUC with a pinned seed, are stable instead.
--
-- Run twice and compare the PAIRS_ORDER lines:
--
--   WinBoloDS -bots 1 -brain tests/brains/probe_pairs.lua -ticks 50 ...
--
-- Same order twice  => iteration is stable; look elsewhere for divergence.
-- Different order   => hash randomisation confirmed in this binary.
--
-- Also reports math.random, so the -brain-lua-seed knob can be checked at the
-- same time: with it set, RANDOM_DRAW must match across runs.

local brain = {}

-- The dedicated server swallows plain print(), so results go to a file.
-- Path comes from WB_PROBE_OUT so two runs can write to separate files;
-- needs -allow-unsafe-brains for io/os access.
local function emit(line)
  local path = (os.getenv and os.getenv("WB_PROBE_OUT")) or "probe_pairs.txt"
  local f = io.open(path, "a")
  if f then
    f:write(line, "\n")
    f:close()
  end
end

function brain.open(info)
  local t = {}
  for i = 1, 32 do t["key" .. i] = i end

  local order = {}
  for k in pairs(t) do order[#order + 1] = k end
  emit("PAIRS_ORDER " .. table.concat(order, ","))

  -- Same question for a table keyed by integers, which PUC stores in the array
  -- part and iterates in index order regardless of the hash seed. If the string
  -- table varies and this one does not, the hash seed is confirmed as the cause
  -- rather than something more general.
  local n = {}
  for i = 1, 16 do n[i] = i end
  local norder = {}
  for k in pairs(n) do norder[#norder + 1] = k end
  emit("ARRAY_ORDER " .. table.concat(norder, ","))

  emit(string.format("RANDOM_DRAW %d %d %d",
                      math.random(0, 999), math.random(0, 999), math.random(0, 999)))
  emit("VM " .. (_G.jit and _G.jit.version or _VERSION))
end

function brain.think(info)
  return { holdkeys = 0, tapkeys = 0 }
end

function brain.close(info)
end

return brain
