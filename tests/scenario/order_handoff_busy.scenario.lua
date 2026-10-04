-- Scenario script for tests/scenario/maps/order_handoff_busy.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- SHOOTING A PILL IS BUSY (ORDER_HANDOFF).  The same field as
-- order_handoff_relief, but A is already in the pill's fire and shooting it
-- when the new order comes, so A answers "no" (Busy, taking pill) and the
-- free bot B takes the new order.
--
--   A (seat 0) at (106,113), 7 squares south of pill #2 (106,106): inside
--   its range.  B (seat 1) at (150,150).  The orders come from seat 2, an
--   idle bot on the team (see order_handoff_relief).  Both bots run with
--   PILL_REPOSITION_ENABLED=false, as there, and A is "noblitz": with a
--   free partner A would wait for B in blitz_wait (a set-up phase, not
--   shooting) instead of shooting the pill alone.  A also runs with
--   PPT_HEALTH_THRESHOLD=99: the pill is at full health, and a full pill
--   makes A gather trees and build shield walls first (gather_trees,
--   build_walls: set-up phases).  With the threshold up A charges at once.
--   1. "!attack 1": A takes it and starts shooting at once.
--   2. "!defend 2" (pill #3, (106,124)) 4 s later.  A's handoff bid would be
--      about 11 squares + 15 against B's 70 squares, so without the busy
--      rule A would win it.  A is shooting, so B takes it.
--
-- PASS when B said its ack for the defend order and A never asked for
-- cover ("Going to") or left its job ("Leaving").
--
-- GATE: ticks=4000 bots=2 ai=yesfull gametype=open

local A, B, BOSS = 0, 1, 2
local PILL_DEFEND = { 106, 124 }
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local placed, chat_on, x_sent, z_sent = false, false, false, false
local spawned, cfg = false, false
local a_left, b_defend = false, false

function on_setup(g)
  g.set_team(A, 1)
  g.set_team(B, 1)
  g.set_pill_owner(3, A)
  g.set_base_owner(1, A)
end

function on_chat(g, p, text)
  g.log(string.format("BUSY_CHAT p%d %s", p, text))
  if p == A and (text:find("Leaving", 1, true) or text:find("Going to", 1, true)) then
    a_left = true
  end
  if p == B and text:find("defend_pill #2$") and not text:find("Leaving", 1, true) then
    b_defend = true
  end
end

function on_tick(g, tick)
  if not spawned and tick >= 2 then
    spawned = true
    g.spawn_bot{ slot = BOSS, name = "Boss", brain = "idle", team = 1, start = 2 }
  end
  if not cfg and tick >= 3 then
    cfg = true
    g.bot_init(A, { cfg = "PILL_REPOSITION_ENABLED=false;cfg=PPT_HEALTH_THRESHOLD=99",
                    noblitz = "1" })
    g.bot_init(B, { cfg = "PILL_REPOSITION_ENABLED=false" })
  end
  if not placed and tick >= 100 then
    placed = true
    g.teleport(A, 106, 113, 0)
    g.teleport(B, 150, 150, 0)
    for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
  end
  if not chat_on and tick >= 200 then
    chat_on = true
    g.say(BOSS, "!bot chat on")
  end
  if not x_sent and tick >= 400 then
    x_sent = true
    g.say(BOSS, "!attack 1")
  end
  if not z_sent and tick >= 800 then
    z_sent = true
    g.say(BOSS, "!defend 2")
  end
end

local function dist(t, q)
  local dx, dy = math.abs(t.mx - q[1]), math.abs(t.my - q[2])
  return (dx > dy) and dx or dy
end

VERDICT_CHECK = function(g)
  local tb = g.tank(B)
  if not tb then return false, "B's tank is missing" end
  local db = dist(tb, PILL_DEFEND)
  local why = string.format("a_left=%s b_defend=%s B->defend=%d",
                            tostring(a_left), tostring(b_defend), db)
  return b_defend and not a_left, why
end
