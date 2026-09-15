-- Scenario sidecar for tests/builder_pool_B.map.  Companion to
-- tests/builder_pool_test.py variant B -- the repair_pill SPLIT.
--
-- The arena has no enemies at all, so there is nothing here but ownership and
-- a trace.  Both bases and the single worn pill go to slot 0; the pill sits 18
-- tiles from the spawn, more than twice BUILDER_POOL_LEASH, so repair_pill as
-- a TANK goal means what plan section 7 says it means -- "relocate until the
-- repair becomes leash-reachable" -- and the hand-off to the man is what the
-- test watches for.
--
-- The second base at (134,122) is not decoration: it sits 6 tiles from the
-- pill, inside PILL_FIRE_RANGE, so the pill has a job to do and the
-- reposition pool stops bidding capture_pill/reposition_shoot on it (i.e. the
-- bot shooting its own pill down to move it, which ends the experiment).

local OUR_PILL   = { 132, 126 }
local BASES      = { { 110, 126 }, { 134, 122 } }

local our_player = 0
local our_pn     = nil     -- the pill's slot, found once
local last_hp    = nil
local rises      = 0       -- what the driver's hp trace was read for
local first_rise = nil

local function own_ours(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then g.set_pill_owner(i, our_player) end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      for _, bp in ipairs(BASES) do
        if b.x == bp[1] and b.y == bp[2] then
          g.set_base_owner(i, our_player)
          g.set_base_stock(i, 90, 90, 90)
        end
      end
    end
  end
end

function on_setup(g)
  own_ours(g)
  g.set_team(our_player, 0)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then our_pn = i end
  end
end

function on_choose_start(g, p)
  if p == our_player then return 1 end
  return nil
end

-- What builder_pool_B_hp.log was for. The driver read that file and asked one
-- question of it -- did the armour ever go UP -- so the answer is counted here
-- instead. There is no io in the scenario sandbox, so no file could be kept.
function on_tick(g, tick)
  local p = our_pn and g.pill(our_pn) or nil
  if p and last_hp ~= p.armour then
    if last_hp and p.armour > last_hp then
      rises = rises + 1
      first_rise = first_rise or tick
    end
    last_hp = p.armour
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/builder_pool_test.py variant B, assertion 3:
-- the engine agrees the pill came back up. The pill starts badly worn and
-- nothing on this map can heal it but our own man, so a rise is the repair
-- landing.
--
-- LEFT BEHIND, because it is print2 and nothing else: assertions 1 and 2, the
-- point of the variant -- the repair_pill SPLIT. The tank drives until the
-- repair is inside BUILDER_POOL_REPAIR_LEASH, then HANDS OFF and stops short
-- while the man walks the last ~11 tiles. Either feeder counts (the pool-5 row
-- going INF with REJECT builder_can, or a repair_pill-seeded BP_DISPATCH), and
-- both are builder-pool log lines. What stands here says the pill was fixed,
-- not that the man and not the tank finished it.
--
-- GATE: ticks=8000 bots=1 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if rises > 0 then
    return true, string.format("pill armour rose %d time(s), first at t=%d",
                               rises, first_rise)
  end
  return false, string.format("pill armour never rose (last %s)",
                              tostring(last_hp))
end
