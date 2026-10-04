-- =========================================================================
-- Bases Only — a mod that takes every pillbox off the map, so the round is
-- played for the bases alone.
--
-- Before the round's first tick every pillbox on the map is taken off with
-- game.remove_pill. A removed pillbox is out of play for everybody: it is
-- not on the map, it is gone from the pillbox panel, nobody can pick it up
-- or build it, and it fires at nobody. Its slot stays empty for the whole
-- round.
--
-- A tank starts a life with no pillboxes, and a builder can only put down a
-- pillbox his tank is carrying, so with none on the map nobody can get one.
-- The one way back is another script on the list putting a pillbox into
-- play. When one is picked up or put down, this mod takes every pillbox off
-- again straight after. A carried one is dropped first, because a pillbox
-- in a tank cannot be removed: on the tank's square, or on the nearest one
-- round it that holds a pillbox (see DROP_SEARCH), and if there is none it
-- tries again every second until the tank has moved. A pillbox another
-- script adds without it ever being picked up or put down raises neither
-- hook, so that one stays.
--
-- Everything else plays as normal: bases, shells, mines, trees, the builder
-- and his walls and roads.
--
-- What wins. kind = "mod", so the round ends the way the map and the lobby
-- say it does, and this mod changes nothing about that. In a stock round,
-- pillboxes have no part in winning: a round ends when one side (a player
-- and his allies) holds every base on the map with none of them shot dead,
-- or when the lobby's time limit, if it set one, runs out. So with the
-- pillboxes gone, the side that takes and holds every base wins. On a map
-- with no bases only the time limit can end the round.
--
-- Bots need to be told nothing. With no pillbox on the map, a bot has no
-- pillbox to attack, capture or defend, and plays for the bases.
--
-- bound = false, so it is not tied to one map. A host picks it from the mod
-- list and it runs over whatever is loaded.
-- =========================================================================

scenario = {
  name = "Bases Only",
  description = "Every pillbox is taken off the map. Play for the bases alone.",
  api = 1,
  kind = "mod",
  bound = false,
  author = "WinBolo",
  updated = "2026-10-04T02:23Z",

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this mod implements:".
  callbacks = {
    on_setup          = "Takes every pillbox off the map before the round starts.",
    on_pill_picked_up = "If another script brought a pillbox back, takes it off again.",
    on_pill_placed    = "If another script brought a pillbox back, takes it off again.",
  },
}

-- How far from a tank, in squares each way, the sweep looks for a square
-- to drop a carried pillbox on when the tank's own square cannot hold one
-- (a base, a wall, deep sea). Each square tried is one write, and a script
-- has 256 a frame: 5 is at most 120.
local DROP_SEARCH = 5

-- A sweep is waiting on its timer or running, so a hook does not set
-- another: a pillbox the sweep itself drops raises on_pill_placed too.
local sweep_waiting = false

-- Drops pillbox n out of seat p's tank: on the tank's own square, or on the
-- nearest square round it that holds a pillbox. False when none of them
-- does.
local function drop_from_tank(p, n)
  if game.drop_pill(p, n) then
    return true
  end
  local t = game.tank(p)
  if t == nil then
    return false
  end
  for r = 1, DROP_SEARCH do
    for dy = -r, r do
      for dx = -r, r do
        if (math.abs(dx) == r or math.abs(dy) == r)
            and game.drop_pill(p, n, t.mx + dx, t.my + dy) then
          return true
        end
      end
    end
  end
  return false
end

local sweep_soon

-- Takes every pillbox off the map. A pillbox in a tank answers to whoever
-- is carrying it, so its owner is the seat to drop it from first. One that
-- cannot be dropped yet is tried again a second later.
local function remove_every_pill()
  sweep_waiting = true
  local left = false
  for n = 1, game.num_pills() do
    local pb = game.pill(n)
    if pb ~= nil then
      if pb.in_tank then
        drop_from_tank(pb.owner, n)
      end
      if not game.remove_pill(n) then
        left = true
      end
    end
  end
  sweep_waiting = false
  if left then
    sweep_soon(1)
  end
end

-- Sets the sweep's timer. From inside a pillbox hook the engine is still
-- moving that pillbox, so the sweep runs from the timer, after the hook has
-- returned, rather than pulling it out from under the engine.
sweep_soon = function(seconds)
  if not sweep_waiting then
    -- A refused timer (64 already waiting) leaves the flag clear, so the
    -- next hook tries again.
    sweep_waiting = game.timer(seconds, remove_every_pill) ~= nil
  end
end

-- Once, before the round's first tick.
function on_setup()
  remove_every_pill()
end

function on_pill_picked_up(n, p, scripted)
  sweep_soon(0)
end

function on_pill_placed(n, p, armour, scripted)
  sweep_soon(0)
end
