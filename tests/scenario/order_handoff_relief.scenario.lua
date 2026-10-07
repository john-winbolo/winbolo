-- Scenario script for tests/scenario/maps/order_handoff_relief.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- THE HANDOFF (ORDER_HANDOFF).  A bot on a person's job can hand that job to
-- a free bot and take a new order near it.
--
--   A (seat 0) at (112,120).  B (seat 1) at (150,150), free and far.
--   The orders come from seat 2, an idle bot on the team (tests/brains/
--   idle.lua): a "!" line from a bot on the team is read as an order, the
--   gate has no person to type one, and a bot does not hear its own line.
--   1. "!attack 1" (pill #2 in this file, at (106,106)).  A is nearest and
--      takes it.  It is 15 squares from the pill, so it is still setting up
--      (not shooting) when the next order comes.
--   2. "!defend 2" (pill #3, at (106,124), our team's) 1.5 s later.  A bids
--      its normal bid + 15 (about 10 squares) against B's 70 squares and
--      wins.  A asks for cover on its pill job (obq).  B is free, so B wins
--      that auction and claims the job.  A then takes the defend order.
--
-- Both bots run with PILL_REPOSITION_ENABLED=false (bot_init): our pill #3
-- would otherwise start a reposition vote, and a repositioning bot is busy.
--
-- PASS when, by the end:
--   * A said "Going to defend_pill #2, need cover on attack_pill #1",
--   * B said "Covering attack_pill #1 for <A>",
--   * nobody said "No cover", and
--   * B is within 10 squares of pill #2 at the end (it went to A's old job).
-- A's own walk to pill #3 is not checked: once B is on pill #2 the squad
-- code pairs A with B for a blitz on it (blitz_wait), and A's goal stays
-- attack_pill while it holds the defend order.  That is squad behaviour,
-- not the handoff this arena tests.
--
-- GATE: ticks=4000 bots=2 ai=yesfull gametype=open

local A, B, BOSS = 0, 1, 2
local PILL_ATTACK = { 106, 106 }
local PILL_DEFEND = { 106, 124 }
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local st = { spawned = false, cfg = false, placed = false, chat_on = false,
             x_sent = false, z_sent = false,
             a_going = false, b_cover = false, no_cover = false }

function on_setup(g)
  g.set_team(A, 1)
  g.set_team(B, 1)
  -- Pill #3 is the team's (defend takes a friendly pill), the base too.
  g.set_pill_owner(3, A)
  g.set_base_owner(1, A)
end

function on_chat(g, p, text)
  g.log(string.format("HANDOFF_CHAT p%d %s", p, text))
  if p == A and text == "Going to defend_pill #2, need cover on attack_pill #1" then
    st.a_going = true
  end
  if p == B and text:find("^Covering attack_pill #1 for %S+$") then st.b_cover = true end
  if text:find("No cover", 1, true) then st.no_cover = true end
end

function on_tick(g, tick)
  if not st.spawned and tick >= 2 then
    st.spawned = true
    g.spawn_bot{ slot = BOSS, name = "Boss", brain = "idle", team = 1, start = 2 }
  end
  if not st.cfg and tick >= 3 then
    st.cfg = true
    g.bot_init(A, { cfg = "PILL_REPOSITION_ENABLED=false" })
    g.bot_init(B, { cfg = "PILL_REPOSITION_ENABLED=false" })
  end
  if tick % 200 == 0 then
    local ta, tb = g.tank(A), g.tank(B)
    g.log(string.format("HANDOFF_POS t=%d A=%s,%s B=%s,%s", tick,
      tostring(ta and ta.mx), tostring(ta and ta.my), tostring(tb and tb.mx), tostring(tb and tb.my)))
  end
  if not st.placed and tick >= 100 then
    st.placed = true
    g.teleport(A, 112, 120, 0)
    g.teleport(B, 150, 150, 0)
    for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
  end
  if not st.chat_on and tick >= 200 then
    st.chat_on = true
    g.say(BOSS, "!bot chat on")
  end
  if not st.x_sent and tick >= 400 then
    st.x_sent = true
    g.say(BOSS, "!attack 1")
  end
  if not st.z_sent and tick >= 550 then
    st.z_sent = true
    g.say(BOSS, "!defend 2")
  end
end

local function dist(t, q)
  local dx, dy = math.abs(t.mx - q[1]), math.abs(t.my - q[2])
  return (dx > dy) and dx or dy
end

VERDICT_CHECK = function(g)
  local ta, tb = g.tank(A), g.tank(B)
  if not (ta and tb) then return false, "a tank is missing" end
  local da, db = dist(ta, PILL_DEFEND), dist(tb, PILL_ATTACK)
  local why = string.format("a_going=%s b_cover=%s no_cover=%s A->defend=%d B->pill=%d",
                            tostring(st.a_going), tostring(st.b_cover),
                            tostring(st.no_cover), da, db)
  return st.a_going and st.b_cover and not st.no_cover and db <= 10, why
end
