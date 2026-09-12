-- shell_own_pillbox_four_times.lua - Comes ashore, parks beside its own
-- pillbox, turns to face it and puts four shells into it.
--
-- Written for Pill Yard: the tank spawns in a boat at (116,126) facing
-- east, road runs east from square 120 along row 126, and a pillbox owned
-- by player 0 sits on grass at (125,124), two squares north of road square
-- 125. A pill never fires on the tank that owns it, so the whole of what
-- being shot at does to a pill can be read off a run where nothing is
-- shooting back.
--
-- Four shells is what it takes to drive a pill's firing interval from
-- PILLBOX_ATTACK_NORMAL down to PILLBOX_MAX_FIRERATE, which is where it
-- stops halving, and the pill on six armour is still alive at the end of
-- them. The trigger is held rather than tapped because a tap is dropped
-- unless it lands on a tick the tank can fire on, and the shells spent are
-- counted off a reading that has held for two thinks running: the client
-- predicts each shot a tick before the server takes it and is pulled back
-- in between, so the latest reading on its own would let the trigger go
-- before the last shell was away. Nothing is pressed after the fourth
-- shell: what a pill does when it is left alone again is the other half of
-- what this run is for. Every step is keyed on the tank's position, speed,
-- heading or stock, never on the tick.

local brain = {}

local PARK_X = 125                       -- road square under the pillbox
local SHOTS = 4                          -- shells into the pill
local BRAKE_LEAD = 2 * 256               -- braking distance from full speed
local FACING_SLOP = 16                   -- how close to due north is close enough

local brake_at = PARK_X * 256 + 128 - BRAKE_LEAD
local phase = "drive"                    -- drive, brake, turn, shell, done
local last_dir = nil
local last_shells = nil
local stable_shells = nil
local shells_at_stop = nil

function brain.think(info)
  local out = { holdkeys = 0, tapkeys = 0 }
  if info.dead then
    phase = "done"
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
      phase = "turn"
    else
      out.holdkeys = KEY_SLOWER
      return out
    end
  end

  if phase == "turn" then
    -- Facing east is heading 64; turn left towards 0 (north). Letting the
    -- key go takes a few steps to land, so stop pressing once the heading
    -- is near north and wait for it to settle before firing.
    local near_north = info.direction <= FACING_SLOP
                       or info.direction >= 256 - FACING_SLOP
    if near_north and last_dir == info.direction then
      phase = "shell"
    else
      if not near_north then
        out.holdkeys = KEY_TURNLEFT
      end
      last_dir = info.direction
      return out
    end
  end

  if phase == "shell" then
    if shells_at_stop == nil then
      shells_at_stop = info.shells
      stable_shells = info.shells
    end
    if last_shells == info.shells then
      stable_shells = info.shells
    end
    last_shells = info.shells
    if shells_at_stop - stable_shells >= SHOTS then
      phase = "done"
    else
      out.holdkeys = KEY_SHOOT
      return out
    end
  end

  return out
end

return brain
