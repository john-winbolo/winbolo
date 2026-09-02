-- log_newtank.lua -- diagnostic brain for tests/spawn_escape_test.py variant B.
--
-- It does nothing at all except report, on every think, whether the engine
-- shows this tank as dead and whether info.newtank -- the one-tick "this is a
-- fresh tank" flag that goals.spawn_escape_tick's arming depends on -- is set.
-- Sitting still in variant B's channel means the pillbox kills it over and
-- over, so a short run produces several respawns to look at.
--
-- Not used by any assertion.  It is here because it is the cheapest way to
-- answer "did the brain ever SEE newtank on a respawn tick?" without
-- instrumenting the real brain.
--
-- print() from a scripted brain does not reach the server console, so the
-- report goes to newtank_log.txt in the debug-session directory -- the same
-- trick tests/brains/drive_east.lua uses for its victim log.

local brain = {}
local fh = nil
local t = 0
local prev_dead = nil
local prev_pos = nil
local newtank_ticks = 0

local function say(s)
  if fh then fh:write(s .. "\n"); fh:flush() end
end

function brain.open(info)
  local dir = _G.DEBUG_SESSION_DIR
  local path = (dir and (dir .. "/newtank_log.txt")) or "newtank_log.txt"
  local ok, f = pcall(io.open, path, "w")
  if ok and f then fh = f end
  say(string.format("open pos=(%d,%d) newtank=%s dead=%s",
    math.floor(info.tankx / 256), math.floor(info.tanky / 256),
    tostring(info.newtank), tostring(info.dead)))
end

function brain.think(info)
  t = t + 1
  local mx = math.floor(info.tankx / 256)
  local my = math.floor(info.tanky / 256)
  local dead = info.dead and true or false
  if info.newtank then
    newtank_ticks = newtank_ticks + 1
    say(string.format("t=%d NEWTANK at (%d,%d) dead=%s", t, mx, my, tostring(dead)))
  end
  if prev_dead ~= nil and dead ~= prev_dead then
    say(string.format("t=%d dead %s -> %s at (%d,%d) prev=(%s,%s)",
      t, tostring(prev_dead), tostring(dead), mx, my,
      tostring(prev_pos and prev_pos[1]), tostring(prev_pos and prev_pos[2])))
  end
  prev_dead = dead
  prev_pos = { mx, my }
  return { holdkeys = 0, tapkeys = 0 }
end

function brain.close(info)
  say(string.format("close t=%d newtank_ticks=%d", t, newtank_ticks))
  if fh then fh:close() end
end

return brain
