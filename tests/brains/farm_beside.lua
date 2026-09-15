-- farm_beside.lua -- the ENEMY BUILDER for tests/capture_lgm_hunt_test.py.
--
-- ONE JOB: park, and keep a hostile LGM standing on a named tile beside the
-- corpse our bot is driving at.  That is the whole precondition of the
-- capture-pill LGM hunt (constants.lua CAPTURE_LGM_HUNT) -- a builder inside
-- CAPTURE_LGM_HUNT_RADIUS of the TARGET PILL -- and it has to be produced
-- deterministically, on a tile the test can name, every run.
--
-- WHY A SCRIPTED BRAIN AND NOT A SECOND GoalHunter.  Measured on the first
-- draft: a GoalHunter enemy never sends its man anywhere near the corpse.  It
-- starts with trees=40 against a reserve of 4, so the builder pool's FARM row
-- has no candidate at all (cands=0 for the whole run), and everything else it
-- does -- attack_base, capture_base, take_cover -- happens on the far side of
-- the map.  Walking the man there instead with a damaged enemy pillbox next to
-- the corpse works, but a LIVE enemy pillbox one tile from the target shells
-- our bot for the entire approach and the run becomes a take_cover test.
--
-- SO: park, and alternate two errands that both put the man on a tile inside
-- the hunt box and neither of which shoots anybody.
--
--   ROAD on ROAD_MX,ROAD_MY  -- the default.  The tile is GRASS and the
--     scenario script re-stamps it GRASS every tick, so the road is always
--     still needed and the man keeps being sent.  Grass, not forest, is the
--     point: the C-side LGM scan hides a tree-covered man more than 3 tiles
--     out, and this test wants him VISIBLE for the whole approach.
--
--   FARM on FARM_MX,FARM_MY  -- run whenever trees drop below TREE_LOW.  A
--     road costs 2 tree units and we start with 40, so without a top-up the
--     errand runs out after twenty trips.  The script re-stamps that tile
--     FOREST every tick, so there is always something to chop.
--
-- It NEVER fires: a shot at our bot would drag it into tank combat and the
-- capture would stop being what is measured.
--
-- Bolo angles: 0 = North, 64 = East, 128 = South, 192 = West.  TURNRIGHT
-- increases the angle, TURNLEFT decreases it.

local brain = {}

-- Geometry -- tests/generate_capture_lgm_hunt_map.py owns these numbers.
local PARK_MX, PARK_MY = 128, 121     -- P1_PARK
local ROAD_MX, ROAD_MY = 128, 126     -- ROAD_TILE (grass, inside the hunt box)
local FARM_MX, FARM_MY = 128, 125     -- FARM_TILE (forest, inside the box)

local BUILD_PERIOD = 20               -- think-frames between re-issues
local TREE_LOW     = 8                -- below this, top up instead of paving
local SOUTH        = 128
local TOL          = 2                -- aiming tolerance, bradians
local LGM_INTANK   = 0

local t = 0
local phase = "approach"
local last_build_t = -1000
local sent = 0
local last_status = nil

local function aim_keys(info, target_angle)
  local d = (target_angle - info.direction) % 256
  if d > 128 then d = d - 256 end
  if d > TOL then return KEY_TURNRIGHT end
  if d < -TOL then return KEY_TURNLEFT end
  return 0
end

function brain.open(info)
  print(string.format("[farm_beside] open pos=(%d,%d) trees=%s park=(%d,%d)",
                      info.tankx, info.tanky, tostring(info.trees),
                      PARK_MX, PARK_MY))
end

function brain.think(info)
  t = t + 1
  local my = math.floor(info.tanky / 256)

  if phase == "approach" then
    -- Drive SOUTH off the start pond to the park row.  Stop as soon as we are
    -- at or past it: the park spot is chosen to sit outside the navigate
    -- drive-by shot's window from our bot's point of view (see the arena
    -- generator), and drifting off it would put the trigger back in play.
    if my >= PARK_MY then
      phase = "parked"
      print(string.format("[farm_beside] t=%d parked at my=%d", t, my))
    else
      return { holdkeys = KEY_FASTER + aim_keys(info, SOUTH), tapkeys = 0 }
    end
  end

  -- Parked.  KEY_SLOWER holds the tank still; releasing the throttle alone
  -- lets it coast.
  local keys = KEY_SLOWER

  local status = info.man_status
  if status ~= last_status then
    last_status = status
    print(string.format("[farm_beside] t=%d man_status=%s trees=%s sent=%d",
                        t, tostring(status), tostring(info.trees), sent))
  end

  -- While he is out, leave him to it.  Re-issuing a request mid-errand is how
  -- the real builder behaves too, but here it only adds noise to the log.
  if status ~= LGM_INTANK then
    return { holdkeys = keys, tapkeys = 0 }
  end

  if (t - last_build_t) >= BUILD_PERIOD then
    last_build_t = t
    local trees = info.trees or 0
    local bx, by, act, name
    if trees < TREE_LOW then
      bx, by, act, name = FARM_MX, FARM_MY, BUILDMODE_FARM, "FARM"
    else
      bx, by, act, name = ROAD_MX, ROAD_MY, BUILDMODE_ROAD, "ROAD"
    end
    sent = sent + 1
    print(string.format("[farm_beside] t=%d %s request (%d,%d) trees=%d n=%d",
                        t, name, bx, by, trees, sent))
    return { holdkeys = keys, tapkeys = 0,
             build = { x = bx, y = by, action = act } }
  end
  return { holdkeys = keys, tapkeys = 0 }
end

function brain.close(info)
  print(string.format("[farm_beside] close t=%d phase=%s sent=%d trees=%s",
                      t, phase, sent, tostring(info.trees)))
end

return brain
