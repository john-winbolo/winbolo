-- Scenario script for tests/ally_capture_guard_A.map — THE BLOCK.
--
-- Companion to tests/ally_capture_guard_test.py variant A.
--
-- One dead friendly pill at (126,126).  Our bot is parked at the east start
-- with a full woodpile and the LGM aboard, and is priced out of taking the
-- corpse itself (OUR_CFG below).  A SECOND real GoalHunter 1.7 starts six tiles
-- north with nothing else on the map to do, so it goes for the corpse and
-- advertises `goal=capture_pill target=<pill id>` on the internal channel —
-- which is the ID half of the guard's matching rule.
--
-- What must happen: our builder pool sees the corpse as a `rebuild` row, reads
-- the ally's advert, and refuses — because four trees would make the pill a
-- live friendly one, which cannot be driven over, so the ally's trip and the
-- kill that produced the corpse would both be thrown away.

local CORPSE    = 1                 -- the map's only pillbox
local OUR_SLOT  = 0
local ALLY_SLOT = 1
local GOALHUNTER = "../brains/GoalHunter_1.7/init.lua"  -- from the build dir

-- The driver's OUR_CFG, word for word.  CAPTURE_PILL_BASE_COST prices our own
-- TANK out of simply driving over the corpse (which measures nothing about the
-- builder pool), and PILL_REPOSITION_ENABLED stops a bot with nothing to do
-- shelling the very corpse under test.  1e30 rather than merely large: the
-- competition picks the cheapest candidate, so a 999999 capture_pill still wins
-- an otherwise empty pool.
local OUR_CFG = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=PILL_REPOSITION_ENABLED=false"

-- Team 1, not the 0 this arena was ported with: team 0 is NO team on this host,
-- so two bots on it are not allies and the internal channel delivers nothing —
-- which is the one thing the whole arena depends on.
local TEAM = 1

-- What the python driver read out of the script's trace file.  `io` is not on
-- the scenario sandbox's base list, so the questions the driver asked of that
-- file are answered here instead, in these four locals.
local rose_at    = nil    -- tick the corpse's armour first went up (a rebuild)
local scoop_at   = nil    -- tick it first went in_tank
local scoop_by   = nil    -- who was carrying it then
local man_at     = nil    -- tick our own LGM first stood on the corpse's square
local last_armour = nil

local function own_everything(g)
  for i = 1, g.num_pills() do
    g.set_pill_owner(i, OUR_SLOT)
  end
  for i = 1, g.num_bases() do
    g.set_base_owner(i, OUR_SLOT)
    g.set_base_stock(i, 90, 90, 90)
  end
end

function on_setup(g)
  own_everything(g)
  -- Say the armour again even though the map header carries it, so the arena
  -- cannot silently start with a live "corpse" if a future map loader ever
  -- normalises a 0 in the header.
  g.set_pill_armour(CORPSE, 0)
  -- Both bots are fielded by the arena rather than by the runner's -bots, which
  -- hands out no init: the measured bot cannot be priced off the corpse any
  -- other way.  The roster ops are refused inside on_setup and the prelude
  -- defers them to on_start, so these two land on the round's first tick.
  -- `start` is named here rather than left to on_choose_start because that hook
  -- fires INSIDE spawn_bot, before this file could know which seat it is for.
  g.spawn_bot{ slot = OUR_SLOT, name = "Ours", brain = GOALHUNTER,
               team = TEAM, start = 1, init = g.init_tokens(OUR_CFG) }
  g.spawn_bot{ slot = ALLY_SLOT, name = "Ally", brain = GOALHUNTER,
               team = TEAM, start = 2 }
end

-- Only for a respawn: the first lives come in on the starts named above.
function on_choose_start(g, p)
  if p == OUR_SLOT  then return 1 end
  if p == ALLY_SLOT then return 2 end
  return nil
end

function on_tick(g, tick)
  -- The pill and the bases are handed over again for the first few ticks: an
  -- owner written in on_setup names a seat the roster does not hold yet.
  if tick <= 20 then own_everything(g) end

  local p = g.pill(CORPSE)
  if p == nil then return end

  if p.in_tank then
    if scoop_at == nil then
      scoop_at, scoop_by = tick, p.owner
    end
    return                      -- a carried pill has no armour worth watching
  end
  if last_armour ~= nil and p.armour > last_armour and rose_at == nil then
    rose_at = tick
  end
  last_armour = p.armour

  -- The engine-side stand-in for the driver's "no BP_DISPATCH to the corpse":
  -- the pool is the only thing that puts our man on the ground, so a man out
  -- on that square is a dispatch that happened.
  if man_at == nil then
    local b = g.builder(OUR_SLOT)
    if b and b.state ~= "in_tank" and b.state ~= "dead"
       and b.mx == p.x and b.my == p.y then
      man_at = tick
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/ally_capture_guard_test.py check_A, which
-- read the same facts out of the script's own engine trace: assertion 3 (our
-- man never went to the corpse) and assertion 4 (the armour never rose and the
-- ally ended up carrying the pill).
--
-- LEFT BEHIND: assertions 0, 1 and 2 — that the pool logged a candidate at
-- all, that it logged BP_ALLY_CAPTURE ... BLOCKED naming the ally, and that
-- BP_DENY gave `ally_capturing` as the reason.  All three are print2 lines
-- about the brain's own reasoning, which no scenario can see.
--
-- GATE: ticks=3200 bots=0 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function(g)
  if rose_at then
    return false, string.format("corpse rebuilt at t=%d", rose_at)
  end
  if man_at then
    return false, string.format("our LGM reached the corpse at t=%d", man_at)
  end
  if scoop_at == nil then
    return false, "nobody ever picked the corpse up"
  end
  if scoop_by ~= ALLY_SLOT then
    return false, string.format("p%s carried it off at t=%d, not the ally",
                                tostring(scoop_by), scoop_at)
  end
  return true, string.format("ally scooped the corpse at t=%d, our man stayed "
                             .. "in the tank", scoop_at)
end
