-- Panel Check — every panel primitive, the announcement and both markers.
--
-- Drop in the hosting Scenario Dir, pick it in the lobby, start the round.
-- bound = false because a scenario a host picks out of the scenarios
-- directory plays over whatever map is committed.

scenario = {
  name        = "Panel Check",
  description = "Every panel primitive, the announcement and both markers.",
  bound       = false,
}

local BASE_GOOD, PILL_GOOD = 49, 162
local MARK_SQUARE, MARK_FOLLOW = 0, 1

-- A square both a map and a player are likely to be near. Move it if the
-- map you commit has nothing here.
local MARK_X, MARK_Y = 12, 12

local function board(fill, ends_at)
  return {
    { "rect",    0,   0, 128, 128, "grey_dark", true  },
    { "rect",    0,   0, 128, 128, "white",     false },
    { "line",    4,  20, 124,  20, "grey"              },
    { "text",    4,   4, "white",  "normal", "left",   "Panel Check" },
    { "text",   64,  26, "cyan",   "small",  "centre", "centred" },
    { "text",  124,  26, "orange", "small",  "right",  "right" },
    { "name",    4,  40, "yellow", "normal", "left",   0 },
    { "sprite",  4,  56, BASE_GOOD }, { "sprite", 24, 56, PILL_GOOD },
    { "bar",     4,  80, 120,  10, "green", fill, 100 },
    { "timer",  64,  98, "yellow", "normal", "centre", "down", ends_at },
    { "rect",  112, 112,  40,  40, "red", true },   -- clipped at the edge
  }
end

function on_start()
  local ends_at = game.tick() + 120 * 100   -- two minutes, 100 ticks a second

  game.panel(0, board(35, ends_at))

  -- The line across the view: five seconds, then it should go by itself.
  game.announce("Panel Check: hold this ground.", 5)

  -- A mark on the ground, and one that rides seat 0's tank.
  game.marker(MARK_SQUARE, MARK_X, MARK_Y, "cyan")
  game.marker_follow(MARK_FOLLOW, 0, "magenta")

  game.timer(10, function()
    game.panel(0, board(80, ends_at))
    game.announce("Panel Check: second line, three seconds.", 3)
  end)

  game.timer(20, function()
    game.panel(0, {})
    game.clear_marker(MARK_SQUARE)
    game.message("Panel Check: panel and square marker cleared.")
  end)

  game.timer(30, function()
    game.clear_marker(MARK_FOLLOW)
    game.message("Panel Check: follow marker cleared.")
  end)
end
