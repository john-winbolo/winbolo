-- order_human_decoy_off -- order_human_decoy with A_CFG turning
-- HUMAN_DECOY_ALLY_BOTS and HUMAN_DECOY_NO_BLOCKERS off.  Boss is an ally
-- bot, so A does not read him as a decoy: it never says "Staying wide" and
-- parks at (113,105), 1.4 squares from Boss (seed 42, 2026-10-14).
-- Expected to fail.  Everything below is order_human_decoy's own text.
--
-- Scenario script for tests/scenario/maps/order_human_decoy_off.map --
-- GENERATED map: tests/generate_order_bids_maps.py.
--
-- THE ALLY IS DECOYING FOR US (HUMAN_DECOY_AWARE, HUMAN_DECOY_ALLY_BOTS,
-- HUMAN_DECOY_NO_BLOCKERS).  A pill shoots the closest tank in its range,
-- and a wall does not stop it picking that tank.  A team-mate behind a wall
-- draws the pill's fire into the wall.  The bot attacking that pill must
-- not park beside him, must not take a spot closer to the pill than his,
-- must not rush in, and must not build walls of its own.
--
--   Pill #2 (106,106) belongs to an enemy idle bot (seat 2, team 2), parked
--   in the far corner (152,150), with 15 armour.  Pill #3 is given to A's
--   team.  Boss (seat 1, tests/brains/idle.lua, team 1) is an ally bot,
--   the decoy.  It is parked at (112,104), 6.3 squares from the pill,
--   behind a 3x3 wall of 9 buildings at (108..110,103..105), from tick 100.
--   The pill shoots Boss, so its shells hit the wall.  A reads Boss as the
--   decoy because HUMAN_DECOY_ALLY_BOTS is on (the default).  2026-10-14
--   (Andrew): until then the arena made seat 1 a human through the test
--   seam HUMAN_DECOY_TEST_HUMANS=2; it no longer does.  Boss takes no
--   damage (set_modifiers taken=0), so he stays put.
--   A (seat 0) is put at (117,106) at tick 395, 11 squares from the pill
--   (out of its range) and a few squares from where it parks.  Boss orders
--   "!attack 1" at tick 400.  A parks at (115,107) at about tick 760; the
--   pill is still shooting the wall then (2 of its squares stand on the
--   line to Boss), and is not through it until after tick 1600.
--   With HUMAN_DECOY_AWARE=false (checked by hand, seed 42) A parks at
--   (113,105), 1.4 squares from Boss, and this arena fails.
--
-- PROOF THE DECOY STOOD.  The scenario API cannot read the brain's decoy
-- flag or the pill's target, so the arena reads the map.  When A says
-- "Staying wide" and again when A parks, the arena traces the line from the
-- pill to Boss and counts the building and half-building squares on it.  It
-- also checks that Boss is in the pill's range (8) and closer to the pill
-- than A, so Boss is the tank the pill shoots.  If the line has no wall, or
-- A is the closer tank, the arena fails with "DECOY GONE": the run measured
-- a plain pill take, not a decoy.
--
-- PASS when:
--   * A said "Staying wide of Boss on pill #1" (the detection),
--   * A never said "Suicide run" or "Charging" (no rush),
--   * the decoy stood (see above) when A said "Staying wide" and when A
--     parked,
--   * A's builder built no wall, half-wall or pillbox of its own from the
--     order (tick 400) until A's team owns the pill (on_built "building"
--     and on_pill_placed with armour > 0, seat A).  2026-10-14 (Andrew,
--     recording 20261014t123512): A parked wide but then built its own
--     blockers; it did not use the human's decoy.  That is a FAIL
--     (HUMAN_DECOY_NO_BLOCKERS stops it).  Other
--     builder trips (trees, road, repair) are logged as HDECOY_BUILD only,
--     and
--   * the first square A parks on inside 10 squares of the pill (the same
--     square for 100 ticks) is at least 2 PROJECTED squares from Boss, not
--     on Boss's square or the 8 round it, and farther from the pill than
--     Boss is.
--     2026-10-14 (Andrew, PR #510 review item 3): the brain now measures the
--     spot-to-human distance projected onto the pill's range circle (the
--     chord, HUMAN_DECOY_PROJECTED) with HUMAN_DECOY_MIN_TILES 2.  This
--     arena used the old straight rule (5 squares).  d_boss in the verdict
--     line stays the straight distance; d_proj is the projected one.
--   2026-10-14 (Andrew): A used to start at (130,106), 24 squares away, and
--   the wall was 2x2.  By the time A drove up, the pill had shot through the
--   wall and A did a plain pill take.  Hence the near, late start, the
--   thick wall and the decoy proof.
--
-- order_human_decoy_off is the same arena with HUMAN_DECOY_ALLY_BOTS and
-- HUMAN_DECOY_NO_BLOCKERS off (A_CFG below): A does not read Boss as a decoy
-- and builds its own walls, and that arena is expected to fail.
--
-- GATE: ticks=6000 bots=1 ai=yesfull gametype=open
-- GATE: expect=fail with HUMAN_DECOY_ALLY_BOTS and HUMAN_DECOY_NO_BLOCKERS off, A does not take ally bot Boss as the decoy and parks beside him

-- KNOB VARIANT.  A_CFG goes to seat A through game.bot_init at tick 3.
-- game.init_tokens packs the pins into 64-byte values (one value of
-- "cfg=...;cfg=..." past 64 bytes is refused by the host).
local A_CFG = "cfg=PILL_REPOSITION_ENABLED=false;cfg=HUMAN_DECOY_ALLY_BOTS=false;cfg=HUMAN_DECOY_NO_BLOCKERS=false"
local A, BOSS, ENEMY = 0, 1, 2
local TARGET = 2
local PILL = { 106, 106 }
local HUMAN = { 112, 104 }
local A_START = { 117, 106 }
local PILL_RANGE = 8
local PONDS = { { 98, 98 }, { 154, 154 }, { 98, 154 }, { 154, 126 } }
local GRASS = 7
local PARK_TICKS = 100
local st = { spawned = false, owned = false, cfg = false, placed = false,
             a_placed = false,
             chat_on = false, sent = false,
             wide = false, rush = nil,
             last_sq = nil, same = 0, park = nil, park_t = nil,
             wide_t = nil, wide_proof = nil, park_proof = nil,
             taken = false, own_block = nil, builds = 0 }
local next_log = 0

function on_setup(g)
  g.set_team(A, 1)
  g.set_base_owner(1, A)
  g.set_pill_owner(3, A)
end

local function edist(ax, ay, bx, by)
  local dx, dy = ax - bx, ay - by
  return math.sqrt(dx * dx + dy * dy)
end

-- Building and half-building squares on the line from the pill's centre to
-- Boss's centre (sampled every 0.1 square, each square counted once).
local function line_walls(g, bx, by)
  local x0, y0 = PILL[1] + 0.5, PILL[2] + 0.5
  local x1, y1 = bx + 0.5, by + 0.5
  local n = math.ceil(edist(x0, y0, x1, y1) * 10)
  if n < 1 then return 0 end
  local seen, walls = {}, 0
  for k = 0, n do
    local sx = math.floor(x0 + (x1 - x0) * k / n)
    local sy = math.floor(y0 + (y1 - y0) * k / n)
    local key = sx * 256 + sy
    if not seen[key] then
      seen[key] = true
      local t = g.map_tile(sx, sy)
      if t == g.TERRAIN.building or t == g.TERRAIN.half_building then
        walls = walls + 1
      end
    end
  end
  return walls
end

-- The decoy stands when the pill's shells at Boss stop in a wall and Boss
-- is the tank the pill shoots (in range, closer than A).  Returns ok, text.
local function decoy_proof(g, when, tick)
  local h, a = g.tank(BOSS), g.tank(A)
  if not h or h.dead or not a or a.dead then
    local txt = string.format("%s t=%d: Boss or A missing", when, tick)
    return false, txt, txt
  end
  local w = line_walls(g, h.mx, h.my)
  local hb = edist(h.mx, h.my, PILL[1], PILL[2])
  local ab = edist(a.mx, a.my, PILL[1], PILL[2])
  local ok = w >= 1 and hb <= PILL_RANGE and hb < ab
  local txt = string.format("%s t=%d walls_on_line=%d boss=(%d,%d) boss_pill=%.1f a_pill=%.1f",
                            when, tick, w, h.mx, h.my, hb, ab)
  local short = string.format("%s@%d walls=%d boss_pill=%.1f a_pill=%.1f",
                              when, tick, w, hb, ab)
  return ok, txt, short
end

function on_chat(g, p, text)
  g.log(string.format("HDECOY_CHAT p%d %s", p, text))
  if p == A and text == "Staying wide of Boss on pill #1" and not st.wide then
    st.wide = true
    st.wide_t = g.tick()
    local ok, txt, short = decoy_proof(g, "wide", st.wide_t)
    st.wide_proof = { ok = ok, txt = short }
    g.log("HDECOY_PROOF " .. txt .. " ok=" .. tostring(ok))
  end
  if p == A and (text:find("^Suicide run") or text:find("^Charging")) then
    st.rush = st.rush or text
  end
end

-- A's own blockers: a wall, half-wall or pillbox A's builder puts down
-- between the order and A's team owning the pill.  The first one fails the
-- arena; every builder action in that window is logged.
local function note_build(g, what, x, y)
  local tick = g.tick()
  g.log(string.format("HDECOY_BUILD t=%d %s (%d,%d)%s", tick, what, x, y,
                      st.taken and " after_take" or ""))
  if not st.sent or st.taken then return end
  if what == "building" or what:find("^pill") then
    st.builds = st.builds + 1
    st.own_block = st.own_block or { x = x, y = y, t = tick, what = what }
  end
end

function on_built(g, p, action, x, y, scripted)
  if scripted or p ~= A then return end
  note_build(g, action, x, y)
end

function on_pill_placed(g, n, p, armour, scripted)
  if scripted or p ~= A or armour == 0 then return end
  local pl = g.pill(n)
  note_build(g, "pill#" .. n, pl and pl.x or -1, pl and pl.y or -1)
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
    local ok, code = g.bot_init(A, g.init_tokens(A_CFG))
    g.log("HDECOY_KNOB " .. A_CFG .. " bot_init " .. tostring(ok) .. " " .. tostring(code))
  end
  if not st.placed and tick >= 100 then
    st.placed = true
    g.teleport(BOSS, HUMAN[1], HUMAN[2], 0)
    g.teleport(ENEMY, 152, 150, 0)
    g.set_modifiers(BOSS, { taken = 0 })
    for _, q in ipairs(PONDS) do g.set_tile(q[1], q[2], GRASS) end
  end
  if not st.chat_on and tick >= 200 then
    st.chat_on = true
    g.say(BOSS, "!bot chat on")
  end
  -- A goes to its start just before the order, so it does not wander off
  -- first; Boss has drawn the pill's fire into the wall since tick 100.
  if not st.a_placed and tick >= 395 then
    st.a_placed = true
    g.teleport(A, A_START[1], A_START[2], 0)
  end
  if not st.sent and tick >= 400 then
    st.sent = true
    g.say(BOSS, "!attack 1")
  end
  if st.sent and not st.taken then
    local tp = g.pill(TARGET)
    if tp and tp.owner == A then st.taken = true end
  end
  local t = g.tank(A)
  if st.sent and not st.park and t and not t.dead then
    local sq = t.mx * 256 + t.my
    if sq == st.last_sq then st.same = st.same + 1 else st.last_sq, st.same = sq, 0 end
    if st.same >= PARK_TICKS and edist(t.mx, t.my, PILL[1], PILL[2]) <= 10 then
      st.park, st.park_t = { t.mx, t.my }, tick
      g.log(string.format("HDECOY parked t=%d at (%d,%d)", tick, t.mx, t.my))
      local ok, txt, short = decoy_proof(g, "park", tick)
      st.park_proof = { ok = ok, txt = short }
      g.log("HDECOY_PROOF " .. txt .. " ok=" .. tostring(ok))
    end
  end
  if tick >= next_log then
    next_log = tick + 400
    local h, p = g.tank(BOSS), g.pill(TARGET)
    local w = (h and not h.dead) and line_walls(g, h.mx, h.my) or -1
    g.log(string.format("HDECOY t=%d A=(%s,%s) Boss=(%s,%s) pill_owner=%s pill_arm=%s walls_on_line=%d",
          tick, tostring(t and t.mx), tostring(t and t.my),
          tostring(h and h.mx), tostring(h and h.my),
          tostring(p and p.owner), tostring(p and p.armour), w))
  end
end

-- The brain's projected distance (attack.hd_proj_dist): both squares pushed
-- out from the pill onto the circle of radius 8 (PILL_FIRE_RANGE), the
-- chord between them.
local function proj_dist(ax, ay, bx, by)
  local atan2 = math.atan2 or math.atan   -- Lua 5.4: math.atan(y, x)
  local a1 = atan2(ay - PILL[2], ax - PILL[1])
  local a2 = atan2(by - PILL[2], bx - PILL[1])
  local d = math.abs(a1 - a2)
  if d > math.pi then d = 2 * math.pi - d end
  return 2 * 8 * math.sin(d / 2)
end

VERDICT_CHECK = function(g)
  local dh, dp, dj, ring = -1, -1, -1, true
  local hp = edist(HUMAN[1], HUMAN[2], PILL[1], PILL[2])
  if st.park then
    dh = edist(st.park[1], st.park[2], HUMAN[1], HUMAN[2])
    dp = edist(st.park[1], st.park[2], PILL[1], PILL[2])
    dj = proj_dist(st.park[1], st.park[2], HUMAN[1], HUMAN[2])
    ring = math.abs(st.park[1] - HUMAN[1]) <= 1 and math.abs(st.park[2] - HUMAN[2]) <= 1
  end
  local function walls(pr) return pr and pr.txt:match("walls=(%d+)") or "-" end
  -- The gate cuts the VERDICT line at 120 characters, so the numbers come
  -- first; the HDECOY_DETAIL lines below give each proof in full.  walls=
  -- is the wall count on the pill-to-Boss line at "wide" / at park.
  local why = string.format("park=%s d_proj=%.1f d_boss=%.1f d_pill=%.1f boss_pill=%.1f walls=%s/%s wide=%s rush=%s",
                            st.park and (st.park[1] .. "," .. st.park[2]) or "nil",
                            dj, dh, dp, hp, walls(st.wide_proof), walls(st.park_proof),
                            tostring(st.wide), tostring(st.rush))
  -- The decoy must have stood when A chose (said "Staying wide") and when
  -- it parked; otherwise this run measured a plain pill take.
  local gone = nil
  if st.wide and not st.wide_proof.ok then
    gone = st.wide_proof.txt
  elseif st.park and not st.park_proof.ok then
    gone = st.park_proof.txt
  end
  if gone then
    why = "DECOY GONE " .. gone .. ": a plain pill take, not a decoy test; " .. why
  end
  local ob = st.own_block
  if ob then
    why = string.format("A built its own blocker at (%d,%d) t=%d: it did not use the human's decoy; %s",
                        ob.x, ob.y, ob.t, why)
    g.log(string.format("HDECOY_DETAIL own_block first=%s (%d,%d) t=%d count=%d",
                        ob.what, ob.x, ob.y, ob.t, st.builds))
  end
  -- game.log drops a line past 128 bytes, so one proof per line (the
  -- HDECOY_PROOF lines logged at the moment have the boss square too).
  g.log("HDECOY_DETAIL proof " .. (st.wide_proof and st.wide_proof.txt or "wide: none"))
  g.log("HDECOY_DETAIL proof " .. (st.park_proof and st.park_proof.txt or "park: none"))
  return gone == nil and ob == nil and st.wide and not st.rush and st.park ~= nil
         and dj >= 2 and not ring and dp > hp, why
end
