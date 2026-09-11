-- take_pill_and_base_watched.lua - One brain for both ends of a two-client
-- round: the tank that starts west of the road drives it and takes the two
-- neutral items on it, the other sits where it spawned and watches. Both
-- report every capture they are told about and what the item they can see
-- became, so the two reports can be held against each other.
--
-- Written for Watch Road: the driving tank spawns in a boat at (116,124)
-- facing east, road runs east from square 120 along row 124, a neutral
-- pillbox with no armour lies on road square 128 and a neutral base stands
-- on road square 133. Neither has to be shot: a flattened pillbox is carried
-- off by driving over it, and a neutral base is taken by stopping on it. The
-- watching tank spawns at (131,128), four squares south of the road between
-- the two, and never touches a key.
--
-- Nothing here is keyed on a tick, and every line is written at most once,
-- so the report is the same whatever the wire does to the timing of it. In
-- particular the allegiance line is written on the first change only: a
-- client leaving at teardown hands its base back, and that second change
-- must not reach the report.

local brain = {}

local BASE_X, BASE_Y = 133, 124          -- the base both tanks can see
local DRIVE_FROM_X = 120                 -- a tank starting west of this drives
local BRAKE_LEAD = 2 * 256               -- braking distance from full speed

local brake_at = BASE_X * 256 + 128 - BRAKE_LEAD
local role = nil                         -- drive or watch, set on the first think
local phase = "drive"                    -- drive, brake, parked
local me = 0
local pill_captures = 0
local base_captures = 0
local base_said = false                  -- the allegiance line is written once
local base_was = nil

local function allegiance(info_bits)
  if info_bits % 4 >= 2 then return "neutral" end   -- OBJECT_NEUTRAL
  if info_bits % 2 == 1 then return "hostile" end   -- OBJECT_HOSTILE
  return "friendly"
end

-- The allegiance the client holds for the item on square (tx, ty), or nil
-- when it is not among the objects the client can see this tick.
local function seen_allegiance(info, want, tx, ty)
  if not info.objects then return nil end
  for i = 1, #info.objects do
    local o = info.objects[i]
    if o.type == want and math.floor(o.x / 256) == tx
       and math.floor(o.y / 256) == ty then
      return allegiance(o.info)
    end
  end
  return nil
end

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }

  if role == nil then
    me = info.player_number
    if math.floor(info.tankx / 256) < DRIVE_FROM_X then
      role = "drive"
    else
      role = "watch"
    end
    io.write(string.format('{"event":"open","player":%d,"role":"%s"}\n', me, role))
    io.flush()
  end

  -- Captures, as this client was told about them. data[1] is the new owner
  -- and data[2] the one it was taken from; 255 is nobody.
  if info.events then
    for i = 1, #info.events do
      local e = info.events[i]
      local kind = nil
      if e.type == EVENT_PILL_CAPTURED and pill_captures == 0 then
        kind = "pill_captured"
        pill_captures = 1
      elseif e.type == EVENT_BASE_CAPTURED and base_captures == 0 then
        kind = "base_captured"
        base_captures = 1
      end
      if kind then
        io.write(string.format(
          '{"event":"%s","player":%d,"new":%d,"prev":%d}\n',
          kind, me, e.data[1], e.data[2]))
        io.flush()
      end
    end
  end

  -- What the base on the map became, from this client's own side of it.
  if not base_said then
    local now = seen_allegiance(info, OBJECT_REFBASE, BASE_X, BASE_Y)
    if now ~= nil then
      if base_was == nil then
        base_was = now
      elseif now ~= base_was then
        io.write(string.format(
          '{"event":"base_seen","player":%d,"tx":%d,"ty":%d,"from":"%s","to":"%s"}\n',
          me, BASE_X, BASE_Y, base_was, now))
        io.flush()
        base_said = true
      end
    end
  end

  if role ~= "drive" or info.dead then
    return out
  end

  if phase == "drive" then
    if info.tankx >= brake_at then
      phase = "brake"
    else
      out.holdkeys = KEY_FASTER
      return out
    end
  end

  if phase == "brake" then
    if info.speed == 0 then
      phase = "parked"
    else
      out.holdkeys = KEY_SLOWER
    end
  end

  return out
end

function brain.close(info)
  io.write(string.format(
    '{"event":"close","player":%d,"role":"%s","pill_captures":%d,"base_captures":%d}\n',
    me, role or "none", pill_captures, base_captures))
  io.flush()
end

return brain
