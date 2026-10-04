-- =========================================================================
-- Faster Tree Growth — a mod that makes forest grow back sooner.
--
-- How trees grow in the stock game. Every frame the server picks one map
-- square at random, once for each tank in the game, and scores it by what is
-- around it: forest and grass count for it, road and buildings count against
-- it. The best square found so far is held, and a wait of tree_grow_ticks
-- (3000) starts again each time a better one turns up. When the wait runs
-- out with nothing better found, that square becomes forest and the search
-- starts over. Until the search finds a square worth growing on, the wait is
-- tree_grow_initial_ticks (30000) instead.
--
-- The very first wait of a round is always the stock one, because the tree
-- search is reset before this mod runs. On a normal map the first square worth
-- growing on turns up within a few frames and brings in the divided wait; only
-- a map with nothing to grow on waits the stock time for its first tree.
--
-- The wait counts down once a frame for every tank, fifty frames a second,
-- so the stock 3000 is a minute with one player and fifteen seconds with
-- four. That is the least time between two trees; a better square turning
-- up late makes it longer.
--
-- This mod divides both waits by the multiplier the host picks in the lobby,
-- rounded to the nearest tick:
--
--   1x   3000 and 30000, the stock game unchanged
--   2x   1500 and 15000, the default
--   10x   300 and  3000
--
-- A shorter wait is also a shorter search, so the square that grows is a
-- little less often the very best one on the map, and the speed-up is close
-- to the multiplier rather than exactly it.
--
-- The waits are read from the rules as the round has them, not from the
-- stock numbers, so a map or a scenario that has already changed them is
-- divided from its own values. Soccer's "trees never grow" stays so at any
-- multiplier.
--
-- kind = "mod", so it leaves the win condition alone: it can be added to
-- any round, beside a scenario or on its own, and the round still ends the
-- way the map and the lobby say it does.
--
-- bound = false, so it is not tied to one map. A host picks it from the mod
-- list and it runs over whatever is loaded.
-- =========================================================================

scenario = {
  name = "Faster Tree Growth",
  description = "Trees grow back faster: 2x by default, up to 10x, set in the lobby.",
  api = 1,
  author = "WinBolo",
  updated = "2026-10-02T17:42Z",
  kind = "mod",
  bound = false,

  -- What the host sets in the lobby's details dialog; read below with
  -- game.setting. Settings are whole numbers, so the choices are 1x to 10x.
  settings = {
    { id = "multiplier", label = "Tree growth speed (x stock)", type = "int",
      min = 1, max = 10, step = 1, default = 2 },
  },

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this mod implements:".
  callbacks = {
    on_setup = "Divides the tree growing waits by the multiplier set in the lobby.",
  },
}

-- The two rules that time tree growth.
local WAITS = { "tree_grow_ticks", "tree_grow_initial_ticks" }

-- Once, before the round's first tick and after every rules block has been
-- applied. Rules go back to the stock table at every round start, so this
-- divides the round's own values each time and never divides twice.
function on_setup()
  local x = game.setting("multiplier")
  if x <= 1 then
    return
  end
  for _, name in ipairs(WAITS) do
    local wait = math.floor(game.rule(name) / x + 0.5)
    if wait < 1 then
      wait = 1
    end
    game.set_rule(name, wait)
  end
end
