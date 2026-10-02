-- =========================================================================
-- Base Restocking Rate — a mod that makes bases refill their own stock
-- faster or slower.
--
-- How bases restock in the stock game. The server keeps one restock timer
-- for every player in the game, staggered so they run out at even spacing.
-- Each time one runs out, every base on the map that is short of full adds
-- one armour, one shell and one mine to its own stock, and that timer starts
-- again from base_regen_ticks (1000). More players means more timers, so
-- bases restock more often in a bigger game.
--
-- This mod scales that rate by the percentage the host picks in the lobby.
-- The wait between restocks becomes base_regen_ticks * 100 / percent,
-- rounded to the nearest tick:
--
--    25%   4000, a quarter as often
--   100%   1000, the stock game unchanged
--   200%    500, twice as often, the default
--  1000%    100, ten times as often
--
-- Only the bases' own stock is changed. How fast a base hands its stock to
-- a tank parked on it is a separate set of rules and is left alone, as are
-- the stock limits.
--
-- The wait is read from the rules as the round has them, not from the stock
-- number, so a map or a scenario that has already changed it is scaled from
-- its own value.
--
-- kind = "mod", so it leaves the win condition alone: it can be added to
-- any round, beside a scenario or on its own, and the round still ends the
-- way the map and the lobby say it does.
--
-- bound = false, so it is not tied to one map. A host picks it from the mod
-- list and it runs over whatever is loaded.
-- =========================================================================

scenario = {
  name = "Base Restocking Rate",
  description = "Bases refill their own stock faster or slower: 25% to 1000% of stock, set in the lobby.",
  api = 1,
  kind = "mod",
  bound = false,

  -- What the host sets in the lobby's details dialog; read below with
  -- game.setting. Settings are whole numbers, so the rate is a percentage.
  settings = {
    { id = "percent", label = "Base restocking rate (% of stock)", type = "int",
      min = 25, max = 1000, step = 25, default = 200 },
  },

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this mod implements:".
  callbacks = {
    on_setup = "Scales the wait between base restocks by the rate set in the lobby.",
  },
}

-- Once, before the round's first tick and after every rules block has been
-- applied. Rules go back to the stock table at every round start, so this
-- scales the round's own value each time and never scales twice.
function on_setup()
  local percent = game.setting("percent")
  if percent == 100 then
    return
  end
  local wait = math.floor(game.rule("base_regen_ticks") * 100 / percent + 0.5)
  if wait < 1 then
    wait = 1
  end
  game.set_rule("base_regen_ticks", wait)
end
