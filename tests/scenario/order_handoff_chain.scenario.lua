-- Scenario script for tests/scenario/maps/order_handoff_chain.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- CHAINED HANDOFFS (ORDER_HANDOFF_MAX_HOPS 3).  A holder that is asked to
-- cover a job can itself hand its own job on, so one new order can move
-- three bots along a line of jobs.
--
--   Pills (brain ids), in a line: Z = #1 (100,110), X = #2 (114,110),
--   Y = #3 (128,110), all the bots' team's.  The orders come from seat 3,
--   an idle bot on the team (see order_handoff_relief).
--   A (seat 0) at (114,114), next to X.  B (seat 1) at (128,114), next to
--   Y.  C (seat 2) at (154,150), free and far.
--   1. "!defend 2": A is nearest and takes X.
--   2. "!defend 3": B is nearest and takes Y.
--   3. "!defend 1" (Z).  A is nearest, wins Z with its handoff bid and asks
--      cover on X (hop 1, chain {A}).  B, next to X, wins that relief with
--      its own handoff bid against far-off C; it promises X and asks cover
--      on Y (hop 2, chain {A,B}).  Only C may bid on that one, so C wins Y.
--   The tanks are placed once, before anyone holds a job: a later teleport
--   reads to the brain as a respawn, and a respawn drops the bot's order.
--
-- All three bots run with PILL_REPOSITION_ENABLED=false (bot_init): the
-- team's pills would otherwise start a reposition vote, and a repositioning
-- bot is busy.
--
-- PASS when, by the end:
--   * A said "Going to defend_pill #1, need cover on defend_pill #2",
--   * B said "Covering defend_pill #2 for <A>, need cover on defend_pill #3",
--   * C said "Covering defend_pill #3 for <B>",
--   * nobody said "No cover", and
--   * A never said "Covering" (the loop guard: B's relief on Y names A in
--     its chain, so A may not bid on it).
-- The "on my way" markers each bot sends cannot be seen from a scenario;
-- test_orders.lua checks them.
--
-- GATE: ticks=3000 bots=3 ai=yesfull gametype=open

local A, B, CC, BOSS = 0, 1, 2, 3
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local st = { spawned = false, cfg = false, placed = false, chat_on = false,
             x_sent = false, y_sent = false, z_sent = false,
             a_going = false, b_cover = false, c_cover = false,
             no_cover = false, a_covering = false }

function on_setup(g)
  g.set_team(A, 1)
  g.set_team(B, 1)
  g.set_team(CC, 1)
  g.set_pill_owner(2, A)
  g.set_pill_owner(3, A)
  g.set_pill_owner(4, A)
  g.set_base_owner(1, A)
end

function on_chat(g, p, text)
  g.log(string.format("CHAIN_CHAT p%d %s", p, text))
  if p == A and text == "Going to defend_pill #1, need cover on defend_pill #2" then
    st.a_going = true
  end
  if p == B and text:find("^Covering defend_pill #2 for %S+, need cover on defend_pill #3$") then
    st.b_cover = true
  end
  if p == CC and text:find("^Covering defend_pill #3 for %S+$") then st.c_cover = true end
  if p == A and text:find("Covering", 1, true) then st.a_covering = true end
  if text:find("No cover", 1, true) then st.no_cover = true end
end

function on_tick(g, tick)
  if not st.spawned and tick >= 2 then
    st.spawned = true
    g.spawn_bot{ slot = BOSS, name = "Boss", brain = "idle", team = 1, start = 3 }
  end
  if not st.cfg and tick >= 3 then
    st.cfg = true
    for _, p in ipairs({ A, B, CC }) do
      g.bot_init(p, { cfg = "PILL_REPOSITION_ENABLED=false" })
    end
  end
  if not st.placed and tick >= 100 then
    st.placed = true
    g.teleport(A, 114, 114, 0)
    g.teleport(B, 128, 114, 0)
    g.teleport(CC, 154, 150, 0)
    for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
  end
  if not st.chat_on and tick >= 200 then
    st.chat_on = true
    g.say(BOSS, "!bot chat on")
  end
  if not st.x_sent and tick >= 300 then
    st.x_sent = true
    g.say(BOSS, "!defend 2")
  end
  if not st.y_sent and tick >= 400 then
    st.y_sent = true
    g.say(BOSS, "!defend 3")
  end
  if not st.z_sent and tick >= 600 then
    st.z_sent = true
    g.say(BOSS, "!defend 1")
  end
end

VERDICT_CHECK = function(g)
  local why = string.format("a_going=%s b_cover=%s c_cover=%s no_cover=%s a_covering=%s",
                            tostring(st.a_going), tostring(st.b_cover), tostring(st.c_cover),
                            tostring(st.no_cover), tostring(st.a_covering))
  return st.a_going and st.b_cover and st.c_cover and not st.no_cover
         and not st.a_covering, why
end
