-- round_reset_count.lua -- probe brain for tests/round_reset_test.py.
--
-- Counts its own thinks and appends the count to round_reset_count.out
-- next to this file (a brain may write inside its own directory; print()
-- goes to wb_log, which a headless test cannot rely on seeing).  The count
-- lives in a module-level local, so it can only ever go back to 1 if the
-- host built a NEW lua_State and ran this file again: that is exactly what
-- the test looks for across a round boundary.
local brain = {}
local n = 0

local function out_path()
  local src = debug.getinfo(1, "S").source or ""
  src = src:gsub("^@", "")
  local dir = src:match("^(.*)[/\\][^/\\]+$") or "."
  return dir .. "/round_reset_count.out"
end

local function say(line)
  local f = io.open(out_path(), "a")
  if f then
    f:write(line, "\n")
    f:close()
  end
end

function brain.open(info)
  say("ROUND_RESET_BRAIN open player=" .. tostring(info.player_number))
end

function brain.think(info)
  n = n + 1
  if n == 1 or n % 50 == 0 then
    say("ROUND_RESET_BRAIN think=" .. n)
  end
  return { holdkeys = 0, tapkeys = 0 }
end

function brain.close(info)
  say("ROUND_RESET_BRAIN close after " .. n .. " thinks")
end

return brain
