-- Scenario script for tests/scenario/maps/order_warmup_start.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- AN ORDER AT THE START OF A GAME IS NOT HELD UP BY WARM-UP.  Until its goal
-- pools have priced enough candidates a fresh bot takes a cheap dead-pill
-- pickup or explores, and that gate came before goal selection, so a
-- person's order waited behind it: on the release brain the bot took this
-- order at once and then drove the other way for about 18 seconds.
--
--   A (seat 0) at (104,100), full shells.  Base #2 (112,128) belongs to an
--   enemy idle bot (seat 2, team 2), parked at (152,150).  The dead filler
--   pill #1 (154,98) is the cheap pickup that draws a warming bot east, away
--   from the base.  Pills #2 and #3 are given to A so nothing fires.  The
--   order "capture base 1" comes from seat 1, an idle bot on A's team, at
--   tick 200, as early as the arena can give it.
--
-- PASS when A is at least 8 squares nearer the base at tick 900 (seven
-- seconds after the order) than it was when the order was given.
--
-- GATE: ticks=1200 bots=1 ai=yesfull gametype=open

local A, BOSS, ENEMY = 0, 1, 2
local TARGET = 2
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local CHECK_TICK = 900
local spawned, owned, placed, sent = false, false, false, false
local d0, d1 = nil, nil

function on_setup(g)
  g.set_team(A, 1)
  g.set_base_owner(1, A)
  g.set_pill_owner(2, A)
  g.set_pill_owner(3, A)
end

local function dist(g)
  local t, b = g.tank(A), g.base(TARGET)
  if not t or not b then return nil end
  local dx, dy = t.mx - b.x, t.my - b.y
  return math.sqrt(dx * dx + dy * dy)
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
  if not sent and tick >= 200 then
    sent = true
    d0 = dist(g)
    g.say(BOSS, "!capture base 1")
  end
  if sent and not d1 and tick >= CHECK_TICK then
    d1 = dist(g)
    local t = g.tank(A)
    g.log(string.format("WARMORD t=%d A=(%s,%s) d0=%.1f d1=%.1f", tick,
          tostring(t and t.mx), tostring(t and t.my), d0 or -1, d1 or -1))
  end
end

VERDICT_CHECK = function(g)
  local why = string.format("d_at_order=%.1f d_at_%d=%.1f", d0 or -1,
                            CHECK_TICK, d1 or -1)
  return d0 ~= nil and d1 ~= nil and (d0 - d1) >= 8, why
end
