-- Scenario script for tests/pill_scariness_B.map.  Variant B: the control.
--
-- GATE: ticks=6200 bots=0 ai=yesfull gametype=open limit=20
--
-- A proves a HEALTHY pill catching NEUTRAL-pillbox strays gets no watch bid.
-- B has to prove the rule did not just switch defend off, so it removes every
-- pillbox except ours and puts a real enemy tank six tiles from it.  With no
-- pillbox anywhere for a shell's back-ray to land on, ANY damage our pill
-- takes attributes to TANK fire (and the presence and no-evidence fallbacks
-- both answer "tank" too), so B expects the pre-change behaviour: src=tank and
-- the ARRIVED watch bid firing.
--
-- Both tanks keep the default (open) loadout here, unlike A: an enemy with no
-- shells cannot shell anything.
--
--   slot 0 : ours, team 1, start 1 = (126,124)
--   slot 1 : the foe, team 2, hostile, start 2 = (126,128)
--
-- TEAMS.  The old host read team 0 as a team; this one reads it as NO team, so
-- the two seats are named on teams 1 and 2 rather than 0 and 1.  Different
-- teams either way, which is all this arena needs of them.
--
-- on_choose_start pins each tank to its own start so the two never swap ends;
-- startsScatterFind would otherwise hand out whichever pond it liked.
--
-- THE PILL'S HP used to be written to pill_scariness_B_hp.log for the python
-- driver.  There is no `io` in the scenario sandbox, so the reading is kept in
-- a local instead.

local OUR_PILL      = { 126, 122 }
local OUR_BASE      = { 120, 132 }

local US            = 0
local FOE           = 1
local US_TEAM       = 1
local FOE_TEAM      = 2

-- Same pin as variant A: the driver guarded the KEEL defend evaluator, and
-- B2 asserts on a rung DEFEND_ALARM_MODE deletes.
local CFG_TOKENS    = "cfg=DEFEND_ALARM_MODE=false"

local PILLS_MAX_HEALTH = 15

local idx_our = nil
local min_hp  = PILLS_MAX_HEALTH

local function own_everything(g, owner)
  for i = 1, g.num_pills() do
    if g.pill(i) then g.set_pill_owner(i, owner) end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
      g.set_base_owner(i, owner)
    end
  end
end

function on_setup(g)
  own_everything(g, US)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then idx_our = i break end
  end
end

-- Start 1 = ours, start 2 = the foe.  Anything else (a respawn we did not
-- plan for) falls through to the engine's own pick.  The seats are constants
-- because this hook fires DURING the spawn that names them.
function on_choose_start(g, p)
  if p == US  then return 1 end
  if p == FOE then return 2 end
  return nil
end

-- Both tanks are spawned here rather than fielded by the runner: a
-- runner-fielded bot gets no init table, and ours needs the cfg pin.
function on_start(g)
  g.spawn_bot{ slot = US, name = "Watcher", team = US_TEAM, loadout = "open",
               init = g.init_tokens(CFG_TOKENS) }
  g.spawn_bot{ slot = FOE, name = "Aggressor", team = FOE_TEAM,
               loadout = "open" }
  own_everything(g, US)
end

function on_tick(g, tick)
  if idx_our then
    local p = g.pill(idx_our)
    if p and p.armour < min_hp then min_hp = p.armour end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/pill_scariness_test.py check_B, and only the
-- world-state half of B1: our pill took damage on a map whose only pillbox is
-- ours, so the hostile tank is the only thing that can have fired the shell
-- and the attribution has no other answer available to it.
--
-- LEFT BEHIND, and it is the larger half: B1's assertion that the brain really
-- printed src=tank, and the whole of B2 -- that the ARRIVED watch bid still
-- fires on the damage path for a healthy pill under tank fire, which is the
-- clause that proves the change did not switch defend off.  Both are HEAT_GATE
-- and PILL_HIT_SRC lines in the brain's print2 log and a scenario cannot see
-- them.  What is kept here is the arena's own staging: without the damage
-- happening at all, neither of those lines could ever have been printed.
VERDICT_CHECK = function(g)
  if idx_our == nil then
    return false, "our pill was never found on the map"
  end
  if min_hp >= PILLS_MAX_HEALTH then
    return false, "our pill was never hit: the foe never engaged it"
  end
  return true, string.format("our pill took tank fire down to %d of %d",
                             min_hp, PILLS_MAX_HEALTH)
end
