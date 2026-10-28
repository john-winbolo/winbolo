-- =========================================================================
-- Tank Hits Set Off Mines — a mod where a shell that hits a tank also sets
-- off the mines under it, as in WinBolo 2.1.
--
-- How it works in the stock game. When a shell ends, it sets off any mine
-- on the map square where it burst if it hit a pillbox, a base, a wall or
-- the ground, or if it ran out there. A shell that hits a tank leaves the
-- mines on its square alone, the same as Mac Bolo.
--
-- In WinBolo 2.1 a shell that hit a tank set off the mines on its square
-- too. So when one tank chased another that was laying mines as it ran,
-- every shot that hit the tank in front could set off a mine it had just
-- laid under or behind it. The mines went up before the chaser got to
-- them.
--
-- This mod sets tank_hit_sets_off_mines to 1, so a shell that hits a tank
-- sets off any mine on the square where it bursts, as in 2.1. The tank
-- takes the hit, the explosion is drawn and heard, and a builder near it
-- can be killed, the same as with the stock rule. A shell that hits a
-- pillbox, a base or terrain, or that runs out on a mined square, sets the
-- mine off as before. A mine a tank drives onto goes up as before.
--
-- The Mac Bolo Rules mod has a "Tank hits do not set off mines" part,
-- on by default, which sets the rule to 0. With both on the list, the one
-- higher on the list sets the rule. To play the Mac Bolo Rules mod with
-- this rule the 2.1 way, turn that part off.
--
-- kind = "mod", so it leaves the win condition alone: it can be added to
-- any round, beside a scenario or on its own, and the round still ends the
-- way the map and the lobby say it does.
--
-- bound = false, so it is not tied to one map. A host picks it from the mod
-- list and it runs over whatever is loaded.
-- =========================================================================

scenario = {
  name = "Tank Hits Set Off Mines",
  description = "A shell that hits a tank also sets off any mine on the " ..
                "square where it bursts, as in WinBolo 2.1.",
  api = 1,
  author = "WinBolo",
  updated = "2026-10-25T05:03Z",
  kind = "mod",
  bound = false,

  -- A shell that hits a tank sets off the mines on its square; the classic
  -- table plays with 0.
  rules = {
    tank_hit_sets_off_mines = 1,
  },
}
