-- Scenario sidecar for tests/builder_pool_A.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/builder_pool_test.py variant A --
-- the "stolen blocker" shape.
--
-- Four jobs.
--
-- 1. AN OPPONENT THAT OWNS THE TARGET.  attack_pill only bids on pills whose
--    owner reads "hostile", so the take needs a real enemy player.  A scripted
--    IDLER (tests/brains/idle.lua) in the far corner is the cheapest way to get
--    one: it never moves, never shoots and never comes near the experiment, so
--    the only thing it contributes is ownership of the pill at (134,126).
--
-- 2. OWNERSHIP.  Our worn pill at (124,121) and our base go to slot 0 (the
--    -bots tank); the target pill goes to the idler.  on_choose_start pins each
--    tank to its own pond so they cannot swap ends.
--
-- 3. RE-WEAR (the reason this sidecar has a set_pill_armour call).  The test's
--    assertion 2 needs a LIVE builder-pool candidate on the ticks the fire
--    exchange holds back -- otherwise assertion 1 ("nothing was dispatched")
--    only proves the pool was empty.  Left alone this arena cannot supply one:
--    repair_pill outbids attack_pill from the spawn, so the seeded topup on our
--    pill finishes at sim ~620, long before the take's exchange starts, and
--    nothing damages the pill again until well after it ends.  So we damage it
--    ourselves, keyed on an OBSERVABLE that means "the take is shooting": the
--    idler's pill has lost armour, and the only thing on this map that can take
--    armour off it is our tank's shells.  From that tick we hold our pill at
--    A_OUR_HP for REWEAR_TICKS engine ticks, re-setting it if anything puts it
--    back up, then let go so the ordinary repair path can have it again.
--
-- 4. TRACE.  Write our worn pill's armour to builder_pool_A_hp.log on every
--    CHANGE, in SIM ticks.  The test's "the repair actually landed" assertion
--    is checked against the ENGINE, not against the brain's account of itself.

local OUR_PILL  = { 124, 121 }
local FOE_PILL  = { 134, 126 }
local BASES     = { { 110, 126 }, { 121, 118 } }
local FOE_BRAIN = "../tests/brains/idle.lua"

-- The seat the idler takes. On the old host spawn_bot answered the seat it
-- had chosen; this one answers `true, "queued"` unless the call names a slot,
-- and the arena needs the number at once to hand it the target pill. The
-- runner fields one bot, so seat 1 is free.
local FOE_SLOT  = 1

-- generate_builder_pool_map.A_OUR_HP: 9 missing, 3 trees' worth of topup.
local WORN_HP      = 6
-- The exchange measured 376 brain ticks (brain t=1121..1497) = 752 of the old
-- host's ticks; hold a bit past it so a shift in the take's timing cannot walk
-- off the end of the window. DOUBLED from 1000 for this host, whose game.tick()
-- counts 100 a second where the old one counted 50 -- the window is a duration.
local REWEAR_TICKS = 2000

local our_player  = 0
local foe_player  = nil
local spawn_tried = false
local last_hp     = nil
local rises       = 0        -- what builder_pool_A_hp.log was read for
local first_rise  = nil

local our_pn      = nil          -- 1-based pill numbers, resolved on setup
local foe_pn      = nil
local foe_hp0     = nil          -- the idler pill's armour before we shot it
local rewear_from = nil          -- engine tick the exchange was first seen

local function index_pills(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      if p.x == FOE_PILL[1] and p.y == FOE_PILL[2] then
        foe_pn = i
        foe_hp0 = foe_hp0 or p.armour
      elseif p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
        our_pn = i
      end
    end
  end
end

local function own_everything(g)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      if p.x == FOE_PILL[1] and p.y == FOE_PILL[2] then
        if foe_player then g.set_pill_owner(i, foe_player) end
      else
        g.set_pill_owner(i, our_player)
      end
    end
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
  index_pills(g)
  own_everything(g)
  g.set_team(our_player, 0)
end

function on_choose_start(g, p)
  if p == our_player then return 1 end
  if foe_player and p == foe_player then return 2 end
  return nil
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    -- The host's own table shape, straight through the compat shim: naming the
    -- slot is what makes the call answer a seat number, and naming the start
    -- pins the idler to its own pond before on_choose_start could be asked.
    local s, err = g.spawn_bot{ slot = FOE_SLOT, name = "Idler",
                                brain = FOE_BRAIN, team = 1, start = 2 }
    if s == nil then
      g.message("BUILDER_POOL_A spawn_bot failed: " .. tostring(err))
    else
      foe_player = FOE_SLOT
      g.message("BUILDER_POOL_A idler slot=" .. tostring(foe_player) .. " team=1")
    end
  end
  -- The spawn lands a tick or two later, so the hand-over of the target pill
  -- is re-asserted until the idler's tank exists.
  if foe_player and tick < 120 then own_everything(g) end
  if not our_pn or not foe_pn then index_pills(g) end

  -- The exchange is on once the target pill has lost armour: nothing else on
  -- this map shoots it.
  if not rewear_from and foe_pn and foe_hp0 then
    local fp = g.pill(foe_pn)
    if fp and not fp.in_tank and fp.armour < foe_hp0 then
      rewear_from = tick
      g.message(string.format(
        "BUILDER_POOL_A exchange seen at t=%d (target hp %d->%d);"
        .. " holding our pill at %d for %d ticks",
        tick, foe_hp0, fp.armour, WORN_HP, REWEAR_TICKS))
    end
  end

  local op = our_pn and g.pill(our_pn) or nil
  if op then
    if rewear_from and tick <= rewear_from + REWEAR_TICKS
       and not op.in_tank and op.armour > WORN_HP then
      g.set_pill_armour(our_pn, WORN_HP)
      op = g.pill(our_pn)
    end
    -- What builder_pool_A_hp.log was for. The driver read that file and asked
    -- one question of it -- did the armour ever go UP -- so the answer is
    -- counted here instead. There is no io in the scenario sandbox.
    if op and last_hp ~= op.armour then
      if last_hp and op.armour > last_hp then
        rises = rises + 1
        first_rise = first_rise or tick
      end
      last_hp = op.armour
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/builder_pool_test.py variant A, assertion 4:
-- the engine agrees our worn pill came back up. Nothing on this map heals it
-- but our own man, so a rise is the repair landing.
--
-- LEFT BEHIND, because it is print2 and nothing else: assertions 1 to 3, which
-- are the variant -- nothing dispatched on any tick whose eligibility verdict
-- reads `fire_exchange:<substate>`, those held-back ticks having a real
-- candidate on them (which is why this sidecar re-wears the pill at all), and
-- the dispatch that does happen reproducing from its own chips. The re-wear is
-- kept even so: it is what stops the run being the empty case, and a future
-- host op that exposes the pool would find the arena already set up for it.
--
-- GATE: ticks=18000 bots=1 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if rises > 0 then
    return true, string.format("pill armour rose %d time(s), first at t=%d",
                               rises, first_rise)
  end
  return false, string.format("pill armour never rose (last %s)",
                              tostring(last_hp))
end
