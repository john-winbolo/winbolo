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
    local s, err = g.spawn_bot("Idler", FOE_BRAIN, 1, nil)
    if s == nil then
      g.message("FOLLOW_THROUGH spawn_bot failed: " .. tostring(err))
    else
      foe_player = s
      g.set_team(s, 1)
      g.message("FOLLOW_THROUGH idler slot=" .. tostring(s) .. " team=1")
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
