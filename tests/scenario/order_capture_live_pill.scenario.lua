-- Scenario script for tests/scenario/maps/order_capture_live_pill.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- "CAPTURE PILL N" ON A LIVE ENEMY PILL.  The chat verb asks for
-- capture_pill, but capture_pill only picks up a dead pill: the goal check
-- dropped it the tick after it went in, the order put it straight back, and
-- the bot spent the whole order switching between capture_pill and no goal
-- without firing a shot.  The order has to shoot the pill down first
-- (attack_pill) and then pick it up.
--
--   A (seat 0) at (130,120), full stocks, about 27 squares from pill #2
--   (106,106).  The pill belongs to an enemy idle bot (seat 2, team 2),
--   parked in the far corner at (152,150), with 15 armour.  The order comes
--   from seat 1, an idle bot on A's team (see order_handoff_relief).  Pill #3
--   is given to A so only the target fires.
--
-- PASS when pill #2 is in A's tank or flies A's side before the round ends.
--
-- GATE: ticks=6000 bots=1 ai=yesfull gametype=open

local A, BOSS, ENEMY = 0, 1, 2
local TARGET = 2
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local spawned, owned, placed, sent = false, false, false, false
local first_hit, taken = nil, nil
local next_log = 0

function on_setup(g)
  g.set_team(A, 1)
  g.set_base_owner(1, A)
  g.set_pill_owner(3, A)
end

local function is_taken(g, p)
  if not p then
    local t = g.tank(A)
    return t ~= nil and (t.pills or 0) > 0
  end
  if p.in_tank then
    local t = g.tank(A)
    return t ~= nil and (t.pills or 0) > 0
  end
  return p.owner ~= nil and p.owner ~= g.NEUTRAL and g.allied(p.owner, A)
end

function on_tick(g, tick)
  if not spawned and tick >= 2 then
    spawned = true
    g.spawn_bot{ slot = BOSS, name = "Boss", brain = "idle", team = 1, start = 2 }
    g.spawn_bot{ slot = ENEMY, name = "Enemy", brain = "idle", team = 2, start = 3 }
  end
  if spawned and not owned and tick >= 20 and g.tank(ENEMY) then
    owned = true
    g.set_pill_owner(TARGET, ENEMY)
    g.set_pill_armour(TARGET, 15)
  end
  if not placed and tick >= 100 then
    placed = true
    g.teleport(A, 130, 120, 0)
    g.teleport(ENEMY, 152, 150, 0)
    for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
  end
  if not sent and tick >= 400 then
    sent = true
    g.say(BOSS, "!capture pill 1")
  end
  local p = g.pill(TARGET)
  if sent then
    if not first_hit and p and p.armour < 15 then first_hit = tick end
    if not taken and is_taken(g, p) then
      taken = tick
      g.log(string.format("CAPPILL taken t=%d first_hit=%s", tick,
                          tostring(first_hit)))
    end
  end
  if tick >= next_log then
    next_log = tick + 400
    local t = g.tank(A)
    g.log(string.format("CAPPILL t=%d A=(%s,%s) sh=%s arm=%s pill_owner=%s pill_arm=%s",
          tick, tostring(t and t.mx), tostring(t and t.my),
          tostring(t and t.shells), tostring(t and t.armour),
          tostring(p and p.owner), tostring(p and p.armour)))
  end
end

VERDICT_CHECK = function(g)
  local p = g.pill(TARGET)
  local why = string.format("taken=%s first_hit=%s owner=%s arm=%s",
                            tostring(taken), tostring(first_hit),
                            tostring(p and p.owner), tostring(p and p.armour))
  return taken ~= nil, why
end
