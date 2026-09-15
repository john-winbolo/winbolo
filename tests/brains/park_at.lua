-- park_at.lua -- "drive to one tile and then sit there", the filler tank for
-- tests/defend_alarm_test.py arenas W and W2.
--
-- Arena W needs bodies in known places and nothing else: two ALLIED tanks
-- parked inside DEFEND_WELL_DEFENDED_RADIUS of our pill (so alarm mode's
-- condition 4 counts them and rejects the pill as already held), and two
-- enemy tanks far from the pill whose only job is to make the teams 3v3 so
-- R = ceil(their_team / our_team) is 1.  W2 is the same run with the two
-- allies parked FAR from the pill instead, which is the whole control.
--
-- BRAIN_INIT_ARG (spawn_bot's 5th argument) is "mx,my" -- the tile to sit on.
-- The tank drives Y first, then X, then holds.  Every route this is used for
-- is single-axis over open grass, so no pathfinding is needed or wanted: a
-- tank that took an interesting route would be a tank whose position the test
-- could not predict.
--
-- It NEVER fires and never sends its man.  A shot from one of these would arm
-- alarm trigger 2a on some other pill, or kill the scripted shooter, and the
-- arena would stop being about the well-defended count.
--
-- STARTS ARE DEEP SEA (starts.c startsIsValidSquare), so the first thing this
-- has to do is drive off the one-tile pond before the tank drowns -- which is
-- why the approach phase runs from think #1 with no settling delay, and why
-- the map gives each of these starts a facing that already points at its
-- target.
--
-- Bolo angles: 0 = North, 64 = East, 128 = South, 192 = West.  TURNRIGHT
-- increases the angle, TURNLEFT decreases it.

-- A scenario that spawns this brain hands it its `spawn_bot{ init = {...} }`
-- table as the global BRAIN_INIT. This brain reads BRAIN_INIT_ARG, the
-- "k=v;k=v" string, so flatten the table into that string before anything
-- parses it. Same rule as brains/GoalHunter_1.7/init.lua: keys sorted so the
-- string is the same every time, a value of "1" or true becoming the bare
-- word, "0" or false dropped, everything else staying k=v. A string already
-- in BRAIN_INIT_ARG -- the command-line path -- keeps its place.
do
  local t = rawget(_G, "BRAIN_INIT")
  if type(t) == "table" then
    local keys = {}
    for k, v in pairs(t) do
      if type(k) == "string" and k ~= "" and v ~= nil then keys[#keys + 1] = k end
    end
    table.sort(keys)
    local toks = {}
    for _, k in ipairs(keys) do
      local v = t[k]
      if type(v) ~= "boolean" then v = tostring(v) end
      if v == true or v == "1" or v == "" then
        toks[#toks + 1] = k
      elseif v == false or v == "0" then
        -- off: no token
      else
        toks[#toks + 1] = k .. "=" .. v
      end
    end
    if #toks > 0 then
      local a = rawget(_G, "BRAIN_INIT_ARG")
      local flat = table.concat(toks, ";")
      if type(a) == "string" and a ~= "" then flat = a .. ";" .. flat end
      rawset(_G, "BRAIN_INIT_ARG", flat)
    end
  end
end

local brain = {}

local NORTH, EAST, SOUTH, WEST = 0, 64, 128, 192
local TOL = 2                        -- aiming tolerance, bradians

local tx, ty = nil, nil              -- target tile, from BRAIN_INIT_ARG
local t = 0
local phase = "approach"
local said_parked = false

local function aim_keys(info, target_angle)
  local d = (target_angle - info.direction) % 256
  if d > 128 then d = d - 256 end
  if d > TOL then return KEY_TURNRIGHT end
  if d < -TOL then return KEY_TURNLEFT end
  return 0
end

function brain.open(info)
  local a = rawget(_G, "BRAIN_INIT_ARG")
  if type(a) == "string" then
    -- "126,130" is what a command line writes. A scenario writes a table, and
    -- a table cannot spell a bare pair, so "mx=126;my=130" is taken as well —
    -- the tokens the flatten block above produces from { mx = 126, my = 130 }.
    local sx, sy = a:match("^%s*(-?%d+)%s*,%s*(-?%d+)%s*$")
    if sx == nil then
      sx = a:match("[;,]?mx=(-?%d+)") or a:match("^mx=(-?%d+)")
      sy = a:match("[;,]?my=(-?%d+)") or a:match("^my=(-?%d+)")
    end
    tx, ty = tonumber(sx), tonumber(sy)
  end
  print(string.format("[park_at] open target=%s,%s pos=(%d,%d)",
                      tostring(tx), tostring(ty), info.tankx, info.tanky))
end

function brain.think(info)
  t = t + 1
  -- No target parsed: hold still rather than wander off and pollute the
  -- arena.  brain.open already said so out loud.
  if tx == nil or ty == nil then
    return { holdkeys = KEY_SLOWER, tapkeys = 0 }
  end

  local mx = math.floor(info.tankx / 256)
  local my = math.floor(info.tanky / 256)

  if phase == "approach" then
    -- Y first, then X.  Both legs of every route this brain is given are
    -- straight lines over open grass.
    if my ~= ty then
      local ang = (my < ty) and SOUTH or NORTH
      return { holdkeys = KEY_FASTER + aim_keys(info, ang), tapkeys = 0 }
    end
    if mx ~= tx then
      local ang = (mx < tx) and EAST or WEST
      return { holdkeys = KEY_FASTER + aim_keys(info, ang), tapkeys = 0 }
    end
    phase = "parked"
  end

  -- Parked.  KEY_SLOWER HOLDS the tank still; releasing the throttle alone
  -- lets it coast on past the target (and, on this map, into the moat).
  if not said_parked then
    said_parked = true
    print(string.format("[park_at] t=%d parked at (%d,%d)", t, mx, my))
  end
  return { holdkeys = KEY_SLOWER, tapkeys = 0 }
end

function brain.close(info)
  print(string.format("[park_at] close t=%d phase=%s target=%s,%s",
                      t, phase, tostring(tx), tostring(ty)))
end

return brain
