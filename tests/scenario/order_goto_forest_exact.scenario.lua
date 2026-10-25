-- Scenario script for tests/scenario/maps/order_goto_forest_exact.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- A GO-THERE ORDER ARRIVES ON THE SQUARE (ORDER_GOTO_ARRIVE_TILES 0).  The
-- target is the middle of a 7x7 forest, (132,102).  With the old one-square
-- ring (keel 1) the bot stopped on the first forest square next to it.
--
--   One bot (seat 0) at (128,112).  "!goto 132 102 128 112": the long form,
--   with the speaker's square, so the range rule measures from the bot.
--   The order comes from seat 1, an idle bot on the team (see
--   order_handoff_relief): a bot does not hear its own line.
--
-- PASS when the tank stands on (132,102) itself for at least 1 s (it parks
-- there for the hold) at some point in the round.
--
-- GATE: ticks=3000 bots=1 ai=yesfull gametype=open

local A, BOSS = 0, 1
local spawned = false
local TX, TY = 132, 102
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local placed, sent = false, false
local on_since, best_hold, closest = nil, 0, 99

function on_setup(g)
  g.set_team(A, 1)
end

function on_chat(g, p, text)
  g.log(string.format("GOTO_CHAT p%d %s", p, text))
end

function on_tick(g, tick)
  if not spawned and tick >= 2 then
    spawned = true
    g.spawn_bot{ slot = BOSS, name = "Boss", brain = "idle", team = 1, start = 1 }
  end
  if not placed and tick >= 100 then
    placed = true
    g.teleport(A, 128, 112, 0)
    for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
  end
  if not sent and tick >= 300 then
    sent = true
    g.say(BOSS, "!goto 132 102 128 112")
  end
  local t = g.tank(A)
  if sent and t then
    local dx, dy = math.abs(t.mx - TX), math.abs(t.my - TY)
    local d = (dx > dy) and dx or dy
    if d < closest then closest = d end
    if d == 0 then
      on_since = on_since or tick
      if tick - on_since > best_hold then best_hold = tick - on_since end
    else
      on_since = nil
    end
  end
end

VERDICT_CHECK = function(g)
  local t = g.tank(A)
  local why = string.format("closest=%d longest_on_square=%d ticks end=(%s,%s)",
                            closest, best_hold, tostring(t and t.mx), tostring(t and t.my))
  return best_hold >= 100, why
end
