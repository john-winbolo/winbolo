-- Scenario script for tests/scenario/maps/order_human_decoy.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- THE HUMAN IS DECOYING FOR US (HUMAN_DECOY_AWARE).  A pill shoots the
-- closest tank in its range, and a wall does not stop it picking that tank.
-- A human team-mate behind a wall draws the pill's fire into the wall.  The
-- bot attacking that pill must not park beside him, must not take a spot
-- closer to the pill than his, and must not rush in.
--
--   Pill #2 (106,106) belongs to an enemy idle bot (seat 2, team 2), parked
--   in the far corner, with 15 armour.  Pill #3 is given to A's team.
--   Boss (seat 1, tests/brains/idle.lua, team 1) is the stand-in human.  It
--   is parked at (112,104), 6.3 squares from the pill, behind the 2x2 wall
--   at (109..110,104..105).  The gate has no human seat, so A reads seat 1
--   as human through the test seam HUMAN_DECOY_TEST_HUMANS=2 (bot_init).
--   Boss takes no damage (set_modifiers taken=0), so he stays put.
--   A (seat 0) starts at (130,106), due east, and Boss orders "!attack 1".
--   With HUMAN_DECOY_AWARE=false (checked by hand, seed 42) A parks at
--   (113,105), 1.4 squares from Boss, and this arena fails.
--
-- PASS when:
--   * A said "Staying wide of Boss on pill #1" (the detection),
--   * A never said "Suicide run" or "Charging" (no rush), and
--   * the first square A parks on inside 10 squares of the pill (the same
--     square for 100 ticks) is at least 5 squares from Boss and farther
--     from the pill than Boss is.
--
-- GATE: ticks=6000 bots=1 ai=yesfull gametype=open

local A, BOSS, ENEMY = 0, 1, 2
local TARGET = 2
local PILL = { 106, 106 }
local HUMAN = { 112, 104 }
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local PARK_TICKS = 100
local st = { spawned = false, owned = false, cfg = false, placed = false,
             chat_on = false, sent = false,
             wide = false, rush = nil,
             last_sq = nil, same = 0, park = nil, park_t = nil }
local next_log = 0

function on_setup(g)
  g.set_team(A, 1)
  g.set_base_owner(1, A)
  g.set_pill_owner(3, A)
end

function on_chat(g, p, text)
  g.log(string.format("HDECOY_CHAT p%d %s", p, text))
  if p == A and text == "Staying wide of Boss on pill #1" then st.wide = true end
  if p == A and (text:find("^Suicide run") or text:find("^Charging")) then
    st.rush = st.rush or text
  end
end

local function edist(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  return math.sqrt(dx * dx + dy * dy)
end

function on_tick(g, tick)
  if not st.spawned and tick >= 2 then
    st.spawned = true
    g.spawn_bot{ slot = BOSS, name = "Boss", brain = "idle", team = 1, start = 2 }
    g.spawn_bot{ slot = ENEMY, name = "Enemy", brain = "idle", team = 2, start = 3 }
  end
  if st.spawned and not st.owned and tick >= 20 and g.tank(ENEMY) then
    st.owned = true
    g.set_pill_owner(TARGET, ENEMY)
    g.set_pill_armour(TARGET, 15)
  end
  if not st.cfg and tick >= 3 then
    st.cfg = true
    g.bot_init(A, { cfg = "PILL_REPOSITION_ENABLED=false;cfg=HUMAN_DECOY_TEST_HUMANS=2" })
  end
  if not st.placed and tick >= 100 then
    st.placed = true
    g.teleport(A, 130, 106, 0)
    g.teleport(BOSS, HUMAN[1], HUMAN[2], 0)
    g.teleport(ENEMY, 152, 150, 0)
    g.set_modifiers(BOSS, { taken = 0 })
    for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
  end
  if not st.chat_on and tick >= 200 then
    st.chat_on = true
    g.say(BOSS, "!bot chat on")
  end
  if not st.sent and tick >= 400 then
    st.sent = true
    g.say(BOSS, "!attack 1")
  end
  local t = g.tank(A)
  if st.sent and not st.park and t and not t.dead then
    local sq = t.mx * 256 + t.my
    if sq == st.last_sq then st.same = st.same + 1 else st.last_sq, st.same = sq, 0 end
    if st.same >= PARK_TICKS and edist(t.mx, t.my, PILL[1], PILL[2]) <= 10 then
      st.park, st.park_t = { t.mx, t.my }, tick
      g.log(string.format("HDECOY parked t=%d at (%d,%d)", tick, t.mx, t.my))
    end
  end
  if tick >= next_log then
    next_log = tick + 400
    local h, p = g.tank(BOSS), g.pill(TARGET)
    g.log(string.format("HDECOY t=%d A=(%s,%s) Boss=(%s,%s) pill_owner=%s pill_arm=%s",
          tick, tostring(t and t.mx), tostring(t and t.my),
          tostring(h and h.mx), tostring(h and h.my),
          tostring(p and p.owner), tostring(p and p.armour)))
  end
end

VERDICT_CHECK = function(g)
  local dh, dp = -1, -1
  local hp = edist(HUMAN[1], HUMAN[2], PILL[1], PILL[2])
  if st.park then
    dh = edist(st.park[1], st.park[2], HUMAN[1], HUMAN[2])
    dp = edist(st.park[1], st.park[2], PILL[1], PILL[2])
  end
  local why = string.format("wide=%s rush=%s park=%s d_boss=%.1f d_pill=%.1f boss_pill=%.1f",
                            tostring(st.wide), tostring(st.rush),
                            st.park and (st.park[1] .. "," .. st.park[2]) or "nil",
                            dh, dp, hp)
  return st.wide and not st.rush and st.park ~= nil and dh >= 5 and dp > hp, why
end
