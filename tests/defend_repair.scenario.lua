-- Scenario sidecar for tests/defend_repair.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/defend_repair_test.py.
--
-- Three jobs.
--
-- 1. THE SHOOTER.  Spawn a second bot on team 1 running the scripted brain
--    tests/brains/shell_pill_then_flee.lua: it drives to a standoff six tiles
--    south of our pill, puts eight paced shells into it, then drives away and
--    never comes back.  A scripted brain (not another GoalHunter) because the
--    test needs the shelling to STOP at a knowable moment -- that stop is the
--    whole experiment.
--
-- 2. OWNERSHIP.  The map's pill and base belong to whatever slot our
--    GoalHunter lands in (0, the -bots tank).  on_choose_start pins each tank
--    to its own start so the two never swap ends -- startsScatterFind would
--    otherwise hand out whichever pond it liked, and "the shooter spawned on
--    our side of the moat" is not the arena.
--
-- 3. TRACE.  Write the pill's armour to defend_repair_hp.log on every CHANGE,
--    in SIM ticks.  That file is the test's ground truth for "when was the
--    pill last hit" -- the assertion that no repair is dispatched while shells
--    are still landing has to be checked against the ENGINE, not against the
--    brain's own account of it.

local OUR_PILL   = { 126, 126 }
local OUR_BASE   = { 122, 122 }
local FOE_BASE   = { 126, 133 }
local FOE_BRAIN  = "../tests/brains/shell_pill_then_flee.lua"
local TRACE      = "defend_repair_hp.log"

local our_player  = 0
local foe_player  = nil
local spawn_tried = false
local last_hp     = nil

local function own_everything(g, owner)
  for i = 1, g.num_pills() do
    if g.pill(i) then g.set_pill_owner(i, owner) end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
      g.set_base_owner(i, owner)
      g.set_base_stock(i, 90, 90, 90)
    elseif b and b.x == FOE_BASE[1] and b.y == FOE_BASE[2] and foe_player then
      -- The far-side base belongs to the shooter. That is what puts the front
      -- line between the two halves, so our pill classifies as FRONT and the
      -- reposition pool stops bidding to shoot it down and move it.
      g.set_base_owner(i, foe_player)
    end
  end
end

function on_setup(g)
  own_everything(g, our_player)
  g.set_team(our_player, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# tick hp\n") f:close() end
end

-- Start 1 = ours (north field), start 2 = the shooter (south strip).
function on_choose_start(g, p)
  if p == our_player then return 1 end
  if foe_player and p == foe_player then return 2 end
  return nil
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local s, err = g.spawn_bot("Shooter", FOE_BRAIN, 1, nil)
    if s == nil then
      g.message("DEFEND_REPAIR spawn_bot failed: " .. tostring(err))
    else
      foe_player = s
      g.set_team(s, 1)
      g.message("DEFEND_REPAIR shooter slot=" .. tostring(s) .. " team=1")
      -- Re-assert ownership: a fresh join can shuffle pill ownership.
      own_everything(g, our_player)
    end
  end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      if last_hp ~= p.armour then
        local f = io.open(TRACE, "a")
        if f then
          f:write(string.format("%d %d\n", tick, p.armour))
          f:close()
        end
        last_hp = p.armour
      end
      break
    end
  end
end
