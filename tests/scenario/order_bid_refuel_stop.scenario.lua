-- Scenario script for tests/scenario/maps/order_bid_refuel_stop.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- THE BID COUNTS A REFUEL STOP (ORDER_BID_STOP_AWARE).  A bot low on shells
-- has to go to a base before it can take a pill, so its bid is
-- bot -> base -> pill + 30, not bot -> pill.
--
--   A (seat 0) at (110,118), 16 squares from pill #2 (106,106), with 5
--   shells.  B (seat 1) at (130,106), 24 squares from the pill, full.  Our
--   base is at (150,96): A's stop costs about 62 + 54 squares + 30.
--   "!attack 1": with the old bid A wins on 16 squares; with the stop in
--   it, B wins.  The order comes from seat 2, an idle bot on the team (see
--   order_handoff_relief).
--
-- PASS when B said its ack for the attack and A did not.
--
-- GATE: ticks=2500 bots=2 ai=yesfull gametype=open

local A, B, BOSS = 0, 1, 2
local spawned = false
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local placed, chat_on, sent, low = false, false, false, false
local acked = {}

function on_setup(g)
  g.set_team(A, 1)
  g.set_team(B, 1)
  g.set_base_owner(1, A)
end

function on_chat(g, p, text)
  g.log(string.format("REFUEL_CHAT p%d %s", p, text))
  if sent and text:find("attack_pill #1$") and not text:find("Leaving", 1, true) then
    acked[p] = true
  end
end

function on_tick(g, tick)
  if not spawned and tick >= 2 then
    spawned = true
    g.spawn_bot{ slot = BOSS, name = "Boss", brain = "idle", team = 1, start = 2 }
  end
  if not placed and tick >= 100 then
    placed = true
    g.teleport(A, 110, 118, 0)
    g.teleport(B, 130, 106, 0)
    for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
  end
  if not chat_on and tick >= 200 then
    chat_on = true
    g.say(BOSS, "!bot chat on")
  end
  if not low and tick >= 300 then
    low = true
    g.set_stocks(A, { shells = 5 })
  end
  if not sent and tick >= 400 then
    sent = true
    g.say(BOSS, "!attack 1")
  end
end

VERDICT_CHECK = function(g)
  local ta = g.tank(A)
  local why = string.format("A_acked=%s B_acked=%s A_shells=%s",
                            tostring(acked[A] or false), tostring(acked[B] or false),
                            tostring(ta and ta.shells))
  return acked[B] == true and not acked[A], why
end
