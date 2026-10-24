-- =========================================================================
-- Tank Hits Spare Mines — a mod where a shell that hits a tank does not set
-- off the mines under it.
--
-- How it works in the stock game. When a shell ends, it sets off any mine
-- on the map square where it burst. That is true whatever the shell hit: a
-- pillbox, a base, a wall, the ground, or a tank. So when one tank chases
-- another that is laying mines as it runs, every shot that hits the tank in
-- front can set off a mine it has just laid under or behind it. The mines
-- go up before the chaser gets to them.
--
-- In Mac Bolo a shell that hits a tank does not touch the map square at
-- all. Only a shell that hits the ground or terrain sets off a mine.
--
-- This mod sets tank_hit_sets_off_mines to 0, so a shell that hits a tank
-- leaves the mines on its square alone. The tank still takes the hit, the
-- explosion is still drawn and heard, and a builder near it can still be
-- killed. A shell that hits a pillbox, a base or terrain, or that runs out
-- on a mined square, still sets the mine off as before. A mine a tank
-- drives onto still goes up as before.
--
-- The Mac Bolo Rules mod has this as one of its parts too (the "Tank hits
-- do not set off mines" setting), so there is no need to add both.
--
-- kind = "mod", so it leaves the win condition alone: it can be added to
-- any round, beside a scenario or on its own, and the round still ends the
-- way the map and the lobby say it does.
--
-- bound = false, so it is not tied to one map. A host picks it from the mod
-- list and it runs over whatever is loaded.
-- =========================================================================

scenario = {
  name = "Tank Hits Spare Mines",
  description = "A shell that hits a tank does not set off the mines on " ..
                "its square. Shells that hit the ground still do.",
  api = 1,
  author = "WinBolo",
  updated = "2026-10-24T22:38Z",
  kind = "mod",
  bound = false,

  -- A shell that hits a tank leaves the mines on its square alone; the
  -- classic table plays with 1.
  rules = {
    tank_hit_sets_off_mines = 0,
  },
}
