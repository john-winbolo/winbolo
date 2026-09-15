-- Scenario sidecar for tests/harvest_follow_through.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/harvest_follow_through_test.py.
--
-- Three jobs.
--
-- 1. THE OPPONENT THAT ISN'T ONE.  Spawn a second bot on team 1 running
--    tests/brains/idle.lua and hand it the northern base.  It exists for
--    exactly one reason: a base has to belong to a PLAYER before the brain
--    stamps it as hostile influence, and without hostile influence there is no
--    front line -- and with no front line the portfolio's most-needed role
--    ("front", with nothing on the board) has no candidate tile and strategic
--    placement never fires at all.  It sits on an islet behind a deep-sea
--    channel and never moves, so it cannot fight, be fought, or be captured.
--
-- 2. THE CARGO.  Load both map pills into our tank once the round is running,
--    so the bot starts the experiment carrying 2 -- the state that makes
--    placement urgent (multi-carry bypass + surplus discount) without waiting
--    for it to go and capture something first.
--
-- 3. OWNERSHIP.  Pin each tank to its own start (startsScatterFind would
--    otherwise hand out whichever pond it liked) and keep the two bases owned
--    by the right sides across the opponent's join.

local OUR_BASE = { 126, 133 }
local FOE_BASE = { 126, 112 }
local FOE_BRAIN = "../tests/brains/idle.lua"
-- THE SEAT IS NAMED, AND IT HAS TO BE (2026-09-15).  spawn_bot answers the
-- seat only when the call named one; a spawn that leaves the seat to the
-- server answers `true, "queued"` and the seat is learned from
-- on_tank_spawned later.  This file wants the slot straight away -- the base
-- ownership, the start pick and the team all key off it -- so it asks for
-- seat 1, which is the first free one behind the gate's single -bots tank.
local FOE_SLOT = 1

local our_player  = 0
local foe_player  = nil
local spawn_tried = false
local gave_pills  = false

local function own_bases(g)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
      g.set_base_owner(i, our_player)
      g.set_base_stock(i, 90, 90, 90)
    elseif b and b.x == FOE_BASE[1] and b.y == FOE_BASE[2] and foe_player then
      g.set_base_owner(i, foe_player)
    end
  end
end

function on_setup(g)
  own_bases(g)
  g.set_team(our_player, 0)
end

-- Start 1 = ours (south arena), start 2 = the idle opponent (north islet).
function on_choose_start(g, p)
  if p == our_player then return 1 end
  if foe_player and p == foe_player then return 2 end
  return nil
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    -- Set before the call, not after it: on_choose_start fires DURING
    -- spawn_bot and has to know which seat the idler is.
    foe_player = FOE_SLOT
    local s, err = g.spawn_bot{ slot = FOE_SLOT, name = "Idler",
                                brain = FOE_BRAIN, team = 1 }
    if s == nil then
      foe_player = nil
      g.message("FOLLOW_THROUGH spawn_bot failed: " .. tostring(err))
    else
      g.set_team(FOE_SLOT, 1)
      g.message("FOLLOW_THROUGH idler slot=" .. tostring(FOE_SLOT) .. " team=1")
    end
    own_bases(g)
  end
  -- After the join (a fresh join can shuffle pill ownership), hand our tank
  -- both pills.  Retried until it takes: give_pill refuses while the tank is
  -- still being placed.
  if spawn_tried and not gave_pills and tick >= 12 then
    local t = g.tank(our_player)
    if t and not t.dead then
      local ok1 = g.give_pill(our_player, 1)
      local ok2 = g.give_pill(our_player, 2)
      if ok1 and ok2 then
        gave_pills = true
        g.message("FOLLOW_THROUGH loaded 2 pills into player 0")
        own_bases(g)
      end
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/harvest_follow_through_test.py, and one of
-- its five assertions came: the last half of assertion 5, "the pill actually
-- lands there", which the driver read out of -finaljson as a fielded pill of
-- ours, out of the tank.  We are handed two pills at the top of the round
-- and they are the only two on the map, so a pill on the ground owned by us
-- is a pill the bot chose to plant -- the follow-through's one visible
-- outcome.
--
-- What is missing is WHICH tile, and everything that made the tile the
-- point.  The driver read the trip tile off init.lua's HARVEST_SET line and
-- then checked that the tank stayed inside the builder's dispatch range of
-- it for the whole walk (2), that a FOLLOW_THROUGH row was in the pool at
-- PLACE_FOLLOW_THROUGH_COST pointing at it (3), and that the trip RESUMED
-- rather than being dropped as worse_than_margin (4) -- which is the field
-- incident this whole file exists for.  All four are the brain's own print2
-- output, and a scenario cannot read it.  So a PASS here says the bot got a
-- pill into the ground and says nothing about whether it followed through on
-- the harvest it paid for.
--
-- GATE: ticks=5000 bots=1 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function(g)
  for n = 1, g.num_pills() do
    local p = g.pill(n)
    if p and p.owner == our_player and not p.in_tank then
      return true, string.format("pill %d planted at (%d,%d)", n, p.x, p.y)
    end
  end
  if not gave_pills then
    return false, "the two pills were never loaded into our tank"
  end
  return false, "both pills still in the tank -- nothing was ever placed"
end
