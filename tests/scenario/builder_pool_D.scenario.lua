-- Scenario script for tests/builder_pool_D.map.  Companion to
-- tests/builder_pool_test.py variant D -- the RESERVATION test.
--
-- Same shape as variant A (a scripted idler owns the pill we take, so
-- attack_pill has a "hostile" target to bid on), but the geometry is the
-- experiment: our worn pill sits near the FAR standoff rather than near our
-- spawn.  While the tank is still crossing the map the round trip to it does
-- not fit inside b.reserve_eta -- the walls are due when the tank arrives, and
-- arriving is soon relative to a 25-tile walk -- so the row must read
-- `reserve(<eta> < trip <n>)`.  Once the tank is at the standoff the trip is
-- short and the reservation is satisfied, and it must dispatch.

local OUR_PILL  = { 127, 124 }
local FOE_PILL  = { 140, 126 }
local BASES     = { { 110, 130 }, { 124, 119 } }
local FOE_BRAIN = "../tests/brains/idle.lua"

-- The seat the idler takes. On the old host spawn_bot answered the seat it
-- had chosen; this one answers `true, "queued"` unless the call names a slot,
-- and the arena needs the number at once to hand it the pill. The runner
-- fields one bot, so seat 1 is free.
local FOE_SLOT  = 1

local our_player  = 0
local foe_player  = nil
local spawn_tried = false
local last_hp     = nil
local our_pn      = nil
local rises       = 0
local first_rise  = nil

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
  own_everything(g)
  g.set_team(our_player, 0)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then our_pn = i end
  end
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
      g.message("BUILDER_POOL_D spawn_bot failed: " .. tostring(err))
    else
      foe_player = FOE_SLOT
      g.message("BUILDER_POOL_D idler slot=" .. tostring(foe_player) .. " team=1")
    end
  end
  -- The spawn lands a tick or two later, so the hand-over of the target pill
  -- is re-asserted until the idler's tank exists.
  if foe_player and tick < 120 then own_everything(g) end

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
-- PORTED (2026-09-15) from tests/builder_pool_test.py variant D, assertion 3:
-- the engine agrees the worn pill came back up. Nothing on this map heals it
-- but our own man, so a rise is the side-quest completing.
--
-- LEFT BEHIND, because it is print2 and nothing else: the RESERVATION itself,
-- which is the whole variant -- b.reserve_eta declared for the far take, the
-- eta moving as the tank drives rather than sitting on a constant, and no
-- dispatch ever launched into a reservation its trip does not fit. Those are
-- BP_DISPATCH / BP_DENY chips. The driver already noted this arena does not
-- FORCE a deferral, so what stands here is the weaker end of what it checked.
--
-- GATE: ticks=24000 bots=1 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if rises > 0 then
    return true, string.format("pill armour rose %d time(s), first at t=%d",
                               rises, first_rise)
  end
  return false, string.format("pill armour never rose (last %s)",
                              tostring(last_hp))
end
