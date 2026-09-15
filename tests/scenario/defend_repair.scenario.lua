-- Scenario sidecar for tests/defend_repair.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/defend_repair_test.py.
--
-- Three jobs.
--
-- 1. THE TWO TANKS.  The arena fields BOTH of them, so the GATE line says
--    bots=0.  The measured GoalHunter has to come in with
--    cfg=DEFEND_ALARM_MODE=false (the driver's one token, and the reason is
--    below), and the runner fields its -bots N seats with no init at all, so
--    the only way the pin reaches the brain is for the arena to do the spawn.
--    The second bot runs the scripted brain tests/brains/shell_pill_then_flee
--    .lua: it drives to a standoff six tiles south of our pill, puts eight
--    paced shells into it, then drives away and never comes back.  A scripted
--    brain (not another GoalHunter) because the test needs the shelling to
--    STOP at a knowable moment -- that stop is the whole experiment.
--
-- 2. OWNERSHIP.  The map's pill and base belong to our GoalHunter (slot 0).
--    Each spawn names its own start so the two never swap ends --
--    startsScatterFind would otherwise hand out whichever pond it liked, and
--    "the shooter spawned on our side of the moat" is not the arena.
--
-- 3. THE ARMOUR WATCH.  The driver read defend_repair_hp.log, which this file
--    used to write with io.open, and asked it two questions: did the pill get
--    shelled, and did its armour come back UP afterwards.  There is no io in
--    the scenario sandbox, so both answers are counted here instead.
--
-- WHY THE cfg= TOKEN.  DEFEND_ALARM_MODE defaults to true and REPLACES the
-- evaluator this arena is about: defend_pill is then rejected unless we are
-- MORE than DEFEND_ALARM_MIN_DIST (9) tiles from the pill, and this arena
-- parks the bot six tiles from it, so the goal could never be taken.  The
-- alarm evaluator has its own arena (defend_alarm).

local OUR_PILL   = { 126, 126 }
local OUR_BASE   = { 122, 122 }
local FOE_BASE   = { 126, 133 }
local OUR_BRAIN  = "../brains/GoalHunter_1.7/init.lua"
local FOE_BRAIN  = "../tests/brains/shell_pill_then_flee.lua"

-- defend_repair_test.py TOKENS, verbatim.
local OUR_TOKENS = "cfg=DEFEND_ALARM_MODE=false"

-- The seats.  spawn_bot answers a seat only when the call names one, and the
-- arena needs both numbers at once; with bots=0 every seat is free.
local OUR_SLOT   = 0
local FOE_SLOT   = 1

local our_player  = OUR_SLOT
local foe_player  = nil
local spawn_tried = false
local last_hp     = nil
local drops       = 0        -- what defend_repair_hp.log was read for
local rises       = 0
local first_rise  = nil

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
end

-- Start 1 = ours (north field), start 2 = the shooter (south strip).  Both
-- spawns name their start, so this is only the fallback a respawn takes.
function on_choose_start(g, p)
  if p == our_player then return 1 end
  if foe_player and p == foe_player then return 2 end
  return nil
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local us, uerr = g.spawn_bot{ slot = OUR_SLOT, name = "Defender",
                                  brain = OUR_BRAIN, team = 0, start = 1,
                                  init = g.init_tokens(OUR_TOKENS) }
    if us == nil then
      g.message("DEFEND_REPAIR spawn_bot(us) failed: " .. tostring(uerr))
    end
    foe_player = FOE_SLOT
    local s, err = g.spawn_bot{ slot = FOE_SLOT, name = "Shooter",
                                brain = FOE_BRAIN, team = 1, start = 2 }
    if s == nil then
      foe_player = nil
      g.message("DEFEND_REPAIR spawn_bot(foe) failed: " .. tostring(err))
    else
      g.set_team(FOE_SLOT, 1)
      g.message("DEFEND_REPAIR shooter slot=" .. tostring(FOE_SLOT) .. " team=1")
    end
    -- Re-assert ownership: a fresh join can shuffle pill ownership.
    own_everything(g, our_player)
  end
  if spawn_tried and tick < 120 then own_everything(g, our_player) end

  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      if last_hp ~= p.armour then
        if last_hp then
          if p.armour < last_hp then drops = drops + 1 end
          if p.armour > last_hp then
            rises = rises + 1
            first_rise = first_rise or tick
          end
        end
        last_hp = p.armour
      end
      break
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/defend_repair_test.py, assertions 0 and 5 --
-- the two the engine answers.  0: the scripted shooter really shelled our
-- pill (the driver wanted three attributed hits; the armour trace is the same
-- event counted on the engine's side).  5: the pill's armour went back UP
-- after the shelling stopped, which is the LGM walking out, repairing and
-- coming home -- the end-to-end outcome the other four assertions are the
-- mechanism of.
--
-- LEFT BEHIND, all of it print2 and none of it visible to a scenario:
-- assertion 1 (the goal was defend_pill during the attack and never
-- take_cover, off TICK_COST), assertion 2 (every REPAIR_DISPATCH_FIRED
-- carried hit_age >= REPAIR_QUIET_TICKS), assertion 3 (a dispatch followed
-- the quiet within REPAIR_QUIET_TICKS + 120), and assertion 4 (the ARRIVED
-- HEAT_GATE "REPAIR BID" row fired and its cost multiplied out of its own
-- chips).  So a PASS here says the repair landed and says nothing about
-- whether the bot waited for the shells to stop before sending its man.
--
-- GATE: ticks=3200 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if drops < 3 then
    return false, string.format("the pill was hit %d time(s), wanted 3", drops)
  end
  if rises > 0 then
    return true, string.format("pill hit %d, armour rose %d (first t=%d)",
                               drops, rises, first_rise)
  end
  return false, string.format("pill hit %d times, armour never rose (now %s)",
                              drops, tostring(last_hp))
end
