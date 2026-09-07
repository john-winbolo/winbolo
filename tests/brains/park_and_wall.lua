-- park_and_wall.lua -- the ENEMY for tests/defend_alarm_test.py arenas B and B2.
--
-- Two jobs, chosen by BRAIN_INIT_ARG (spawn_bot's 5th argument):
--
--   "wall"    drive SOUTH off the start pond to PARK_MY, brake, and send the
--             LGM to build a WALL on (WALL_MX, WALL_MY) -- 4 tiles from our
--             enemy's pill, i.e. the OUTER ring of the alarm's build stamp.
--             This is arena B: the tile changes to half-wall then wall while a
--             HOSTILE LGM is standing in the stamp, which is trigger 2b.
--
--   "park"    drive to exactly the same place and then do NOTHING at all --
--             no shot, no man, no build.  This is arena B2, the control: the
--             scenario sidecar itself flips the same tile to a wall with
--             game.set_tile, so a build appears inside the stamp with NO
--             hostile LGM ever seen there.  Alarm mode must NOT fire on it.
--
-- It never fires.  Damage on the pill would arm trigger 2a and the two arenas
-- would stop being about builds at all.
--
-- Bolo angles: 0 = North, 64 = East, 128 = South, 192 = West.  TURNRIGHT
-- increases the angle, TURNLEFT decreases it.

local brain = {}

local PARK_MX, PARK_MY = 126, 120     -- tests/generate_defend_alarm_map.py
local WALL_MX, WALL_MY = 126, 122     -- FOE_WALL
local BUILD_PERIOD     = 25           -- think-frames between re-issues
local SOUTH            = 128
local TOL              = 2            -- aiming tolerance, bradians
local LGM_INTANK       = 0

local mode = "wall"
local t = 0
local phase = "approach"
local last_build_t = -1000
local said_out = false

local function aim_keys(info, target_angle)
  local d = (target_angle - info.direction) % 256
  if d > 128 then d = d - 256 end
  if d > TOL then return KEY_TURNRIGHT end
  if d < -TOL then return KEY_TURNLEFT end
  return 0
end

function brain.open(info)
  local a = rawget(_G, "BRAIN_INIT_ARG")
  if type(a) == "string" and a:find("park") then mode = "park" end
  print(string.format("[park_and_wall] open mode=%s pos=(%d,%d) trees=%s",
                      mode, info.tankx, info.tanky, tostring(info.trees)))
end

function brain.think(info)
  t = t + 1
  local my = math.floor(info.tanky / 256)

  if phase == "approach" then
    if my >= PARK_MY then
      phase = "parked"
      print(string.format("[park_and_wall] t=%d parked at my=%d (mode=%s)",
                          t, my, mode))
    else
      return { holdkeys = KEY_FASTER + aim_keys(info, SOUTH), tapkeys = 0 }
    end
  end

  -- Parked.  KEY_SLOWER holds the tank still; releasing the throttle alone
  -- lets it coast on into the moat and drown.
  local keys = KEY_SLOWER

  if mode == "park" then
    return { holdkeys = keys, tapkeys = 0 }
  end

  -- mode == "wall": keep asking until the man is out, then leave him to it.
  -- (The build request is routed through an InputPacket and the server sim
  -- decides; re-issuing it while he is still aboard is how the real builder
  -- does it too.)
  if info.man_status ~= LGM_INTANK then
    if not said_out then
      said_out = true
      print(string.format("[park_and_wall] t=%d LGM OUT (status=%s) -> "
                          .. "walling (%d,%d)", t, tostring(info.man_status),
                          WALL_MX, WALL_MY))
    end
    return { holdkeys = keys, tapkeys = 0 }
  end
  if (t - last_build_t) >= BUILD_PERIOD then
    last_build_t = t
    print(string.format("[park_and_wall] t=%d BUILD request (%d,%d) trees=%s",
                        t, WALL_MX, WALL_MY, tostring(info.trees)))
    return { holdkeys = keys, tapkeys = 0,
             build = { x = WALL_MX, y = WALL_MY, action = BUILDMODE_BUILD } }
  end
  return { holdkeys = keys, tapkeys = 0 }
end

function brain.close(info)
  print(string.format("[park_and_wall] close t=%d mode=%s phase=%s trees=%s",
                      t, mode, phase, tostring(info.trees)))
end

return brain
