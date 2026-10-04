-- Scenario script for tests/scenario/maps/order_capture_enemy_base.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- A CAPTURE ORDER ON A LIVE ENEMY BASE.  "capture base N" (and a bot-command
-- ping on an enemy base, which files the same verb) asks for capture_base,
-- but a live hostile base is shot down first: the goal runs as attack_base
-- until the armour is gone, then capture_base for the drive-over.  The order
-- has to accept both kinds on that base.  When it did not, every goal pick
-- put the ordered capture_base back, the next tick switched it to attack_base
-- again and cleared the path, so the bot re-planned its route twice a second
-- on the way in.
--
--   A (seat 0) at (104,100), full shells, about 28 squares from base #2
--   (112,128).  The base belongs to an enemy idle bot (seat 2, team 2),
--   parked in the far corner at (152,150), with 90 armour.  The order comes
--   from seat 1, an idle bot on A's team (see order_handoff_relief).
--   Pills #2 and #3 are given to A so nothing shoots A on the way.
--
-- PASS when base #2 is on A's side before the round ends.  The brain's own
-- goal changes are not visible here; a -brain-debug run of this arena shows
-- them (GOAL CHANGE / BASE RESTOCKED / BASE CAPTURABLE in print2).
--
-- GATE: ticks=8000 bots=1 ai=yesfull gametype=open

local A, BOSS, ENEMY = 0, 1, 2
local TARGET = 2
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local spawned, owned, placed, sent = false, false, false, false
local first_hit, captured = nil, nil
local next_log = 0

function on_setup(g)
  g.set_team(A, 1)
  g.set_base_owner(1, A)
  -- The two full neutral pills stand by A's road and by the base; ours, they
  -- hold fire, so no hit sends A off to refuel mid-order.
  g.set_pill_owner(2, A)
  g.set_pill_owner(3, A)
end

function on_tick(g, tick)
  if not spawned and tick >= 2 then
    spawned = true
    g.spawn_bot{ slot = BOSS, name = "Boss", brain = "idle", team = 1, start = 2 }
    g.spawn_bot{ slot = ENEMY, name = "Enemy", brain = "idle", team = 2, start = 3 }
  end
  if spawned and not owned and tick >= 20 and g.tank(ENEMY) then
    owned = true
    g.set_base_owner(TARGET, ENEMY, true)
    g.set_base_stock(TARGET, 90, 90, 90)
  end
  if not placed and tick >= 100 then
    placed = true
    g.teleport(A, 104, 100, 0)
    g.teleport(ENEMY, 152, 150, 0)
    for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
  end
  if not sent and tick >= 400 then
    sent = true
    g.say(BOSS, "!capture base 1")
  end
  local b = g.base(TARGET)
  if sent and b then
    if not first_hit and b.armour < 90 then first_hit = tick end
    if not captured and b.owner ~= nil and b.owner ~= g.NEUTRAL
       and g.allied(b.owner, A) then
      captured = tick
      g.log(string.format("CAPBASE captured t=%d first_hit=%s", tick,
                          tostring(first_hit)))
    end
  end
  if tick >= next_log then
    next_log = tick + 400
    local t = g.tank(A)
    g.log(string.format("CAPBASE t=%d A=(%s,%s) sh=%s base_owner=%s arm=%s",
          tick, tostring(t and t.mx), tostring(t and t.my),
          tostring(t and t.shells), tostring(b and b.owner),
          tostring(b and b.armour)))
  end
end

VERDICT_CHECK = function(g)
  local b = g.base(TARGET)
  local why = string.format("captured=%s first_hit=%s owner=%s arm=%s",
                            tostring(captured), tostring(first_hit),
                            tostring(b and b.owner), tostring(b and b.armour))
  return captured ~= nil, why
end
