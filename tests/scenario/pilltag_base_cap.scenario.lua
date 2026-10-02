-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a base boost on a seat that is not the holder is capped at
-- the speed a fresh take's boost gives the holder at the same moment on the
-- same ground, and boosts do not add up.
--
-- The pickup cap P is worked out here from the rules and the carry shares:
-- the holder's share of the ground (a base square drives at
-- speed_refuel_base with a road's share), times BOOST_MULT for
-- BOOST_SECONDS, with ground slower than a road given the road's boosted
-- share, then a straight line to the ground's own share over
-- BOOST_DECAY_SECONDS. A tank's cap is the whole part of the ground's speed
-- rule x his modifier / 100.
--
-- Seat 0 gets the prize at tick 300. In the full part of his take's boost
-- his cap on land is the whole part of P or one over it (his band): the
-- take's boost is as it was. Then seat 1, the hunter, gets these, each on a
-- 9 by 9 patch of the case's ground with a new neutral base in the middle
-- that he is put on, so the engine captures it. Three ticks after each
-- capture he is put two squares off the base, on the patch's own ground.
--  1. road: one base;
--  2. forest: one base;
--  3. road: two bases, the second 2.5 seconds after the first, in the fade
--     of the first: the boost starts again from the second, it does not add;
--  4. forest: a base, then the prize half a second later: the take's boost
--     replaces the base's (base boost gone, boost_from at the take, the full
--     BOOST_MULT), and his cap is the take's P or one over it.
-- At every tick of a hunter's base boost: his cap is never over the larger
-- of the ground's own cap and the whole part of P, worked from the last
-- capture; and on the case's ground in the full part of the boost his cap
-- is the whole part of P and over the ground's own cap (the boost happens).
-- on_tick runs every other tick, so "3 ticks" is the first call at or past
-- it. The script takes a base off the map at its pit stop, so the hunter is
-- not on a base square after a capture.
--
-- PASS: every case does what it says.

ARENA = { step = "give", case = 1, caught = {} }

ARENA.cases = {
  { name = "road",            ground = "road" },
  { name = "forest",          ground = "forest" },
  { name = "two bases",       ground = "road",   second = 250 },
  { name = "base then take",  ground = "forest", take = 50 },
}

local arena_real_captured = on_base_captured
function on_base_captured(n, old, new, scripted)
  arena_real_captured(n, old, new, scripted)
  ARENA.caught[#ARENA.caught + 1] = { n = n, p = new, t = game.tick() }
end

ARENA.capture = function(n, p, since)
  for _, c in ipairs(ARENA.caught) do
    if c.n == n and c.p == p and c.t >= since then
      return c
    end
  end
  return nil
end

ARENA.fail = function(why)
  verdict(false, string.format("case %d (%s) t=%d: %s", ARENA.case,
    ARENA.cases[ARENA.case] and ARENA.cases[ARENA.case].name or "-",
    game.tick(), why))
  ARENA.step = "done"
end

-- The speed rule and the holder's share (a percentage) of the square tank t
-- is on, worked the way the engine drives it, or nil.
ARENA.ground = function(t)
  for n = 1, game.num_bases() do
    local b = game.base(n)
    if b ~= nil and b.x == t.mx and b.y == t.my then
      return "speed_refuel_base", CARRY_SPEED_PCT.road, "base"
    end
  end
  if t.boat then
    return "speed_boat", CARRY_SPEED_PCT.boat, "boat"
  end
  local code = game.map_tile(t.mx, t.my)
  for name, pct in pairs(CARRY_SPEED_PCT) do
    if game.TERRAIN[name] == code or game.TERRAIN["mine_" .. name] == code then
      return "speed_" .. name, pct, name
    end
  end
  return nil
end

-- A fresh take's share (P) on rule/pct `from` ticks into the boost, with
-- factor `mult`, or nil once the fade is over.
ARENA.pickup = function(rule, pct, since, mult)
  local s = (game.tick() - since) / 100 - BOOST_SECONDS
  local f = (s < 0) and 0 or s / math.max(BOOST_DECAY_SECONDS, 1e-9)
  if f >= 1 then
    return nil, f
  end
  local cap = game.rule(rule)
  local share = cap * pct / 100
  local road_cap = game.rule("speed_road")
  if cap < road_cap then
    local full = road_cap * CARRY_SPEED_PCT.road / 100 * mult
    return full + (share - full) * f, f
  end
  return share * (mult + (1 - mult) * f), f
end

-- Tank t's cap on rule: the whole part of the rule x his modifier / 100.
ARENA.cap_of = function(t, rule)
  local pct = (t.mods.speed == 0) and 100 or t.mods.speed
  return math.floor(game.rule(rule) * pct / 100)
end

-- Makes the 9 by 9 squares round (cx, cy) `ground`, puts a neutral base in
-- the middle and seat 1 on it. Returns the base's number or nil.
ARENA.lay = function(cx, cy, ground)
  for dy = -4, 4 do
    for dx = -4, 4 do
      game.set_tile(cx + dx, cy + dy, game.TERRAIN[ground])
    end
  end
  return ARENA.base_at(cx, cy)
end

ARENA.base_at = function(x, y)
  game.set_tile(x, y, game.TERRAIN.road)
  local n = game.add_base(x, y)
  if n == nil then
    return nil
  end
  local t = game.tank(1)
  if t ~= nil and t.boat then
    game.set_boat(1, false)
  end
  game.teleport(1, x, y, 64)
  return n
end

-- The per-tick check of seat 1's base boost, from capture tick `since`.
ARENA.check_hunter = function(since, c)
  local A = ARENA
  local t = game.tank(1)
  if t == nil or t.dead then
    A.fail("the hunter died")
    return false
  end
  if holder == 1 then
    return true
  end
  if game.tick() >= since + (BOOST_SECONDS + BOOST_DECAY_SECONDS) * 100 then
    return true
  end
  if boost_use.base[1] ~= since then
    A.fail(string.format("base boost from %s, last capture %d",
                         tostring(boost_use.base[1]), since))
    return false
  end
  local rule, pct, name = ARENA.ground(t)
  if rule == nil then
    return true
  end
  local P, f = ARENA.pickup(rule, pct, since, BOOST_MULT)
  if P == nil then
    return true
  end
  local cap = game.rule(rule)
  local got = ARENA.cap_of(t, rule)
  local ceiling = math.max(cap, math.floor(P + 1e-9))
  if got > ceiling then
    A.fail(string.format("hunter cap %d on %s (modifier %d) over the "
      .. "pickup cap %d (P %.2f, fade %.2f)", got, name, t.mods.speed,
      ceiling, P, f))
    return false
  end
  A.max_cap = math.max(A.max_cap or 0, got)
  if f == 0 and name == c.ground and game.tick() >= since + 3 then
    if got ~= math.floor(P + 1e-9) or got <= cap then
      A.fail(string.format("hunter cap %d on %s in the full boost, wanted "
        .. "%d (P %.2f), ground cap %d", got, name,
        math.floor(P + 1e-9), P, cap))
      return false
    end
    if not A.full_seen then
      game.log(string.format("ARENA case %d full boost on %s: cap %d of "
        .. "ground %d (modifier %d)", A.case, name, got, cap, t.mods.speed))
    end
    A.full_seen = true
  end
  return true
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil or A.step == "done" then return end
  local c = A.cases[A.case]
  if A.step == "give" and tick >= 300 then
    game.give_pill(0, pill)
    A.step = "holder"
  elseif A.step == "holder" then
    -- Seat 0's take: cap on land is the whole part of P or one over it.
    if holder ~= 0 or boost_from == nil then
      if tick >= 400 then A.fail("seat 0 did not take the prize") end
      return
    end
    local t = game.tank(0)
    local P, f = nil, 1
    local rule, pct, name
    if t ~= nil and not t.dead then
      rule, pct, name = ARENA.ground(t)
      if rule ~= nil then
        P, f = ARENA.pickup(rule, pct, boost_from, boost_use.mult)
      end
    end
    if P ~= nil and f == 0 and tick >= boost_from + 3 then
      local got = ARENA.cap_of(t, rule)
      if boost_use.mult ~= BOOST_MULT then
        A.fail(string.format("first take factor %.3f", boost_use.mult))
        return
      end
      if got < math.floor(P + 1e-9) or got > math.ceil(P - 1e-9) then
        A.fail(string.format("holder cap %d on %s, take's P %.2f", got,
                             name, P))
        return
      end
      A.holder_seen = (A.holder_seen or 0) + 1
    end
    if tick >= boost_from + BOOST_SECONDS * 100 then
      if (A.holder_seen or 0) == 0 then
        A.fail("no holder sample in his full boost")
        return
      end
      game.log(string.format("ARENA holder take ok, %d samples",
                             A.holder_seen))
      A.step = "put"
    end
  elseif A.step == "put" then
    if c == nil then
      verdict(true, string.format("base boosts capped at a fresh take's, "
        .. "no stacking; hunter's top cap %d", A.max_cap or -1))
      A.step = "done"
      return
    end
    local t = game.tank(1)
    if t == nil or t.dead then return end
    if boost_use.base[1] ~= nil or holder == 1 then return end
    A.full_seen = false
    A.cx, A.cy = t.mx + 6, t.my
    A.base_n = ARENA.lay(A.cx, A.cy, c.ground)
    if A.base_n == nil then
      A.fail("no base could be put down")
      return
    end
    A.put_at = tick
    A.since = nil
    A.step = "caught"
  elseif A.step == "caught" then
    local got = ARENA.capture(A.base_n, 1, A.put_at)
    if got == nil then
      if tick >= A.put_at + 500 then
        A.fail("no capture of base " .. A.base_n)
      end
      return
    end
    A.since = got.t
    A.first = got.t
    A.step = "boost"
  elseif A.step == "boost" then
    if c.second and A.second_n == nil and tick >= A.first + c.second then
      A.second_n = ARENA.base_at(A.cx - 2, A.cy)
      A.second_at = tick
      if A.second_n == nil then
        A.fail("no second base could be put down")
        return
      end
    end
    if A.second_n ~= nil and A.since == A.first then
      local g2 = ARENA.capture(A.second_n, 1, A.second_at)
      if g2 ~= nil then
        -- Start again from the second: checked from here on, and the full
        -- boost must be seen again.
        A.since = g2.t
        A.full_seen = false
        if boost_use.base[1] ~= g2.t then
          A.fail(string.format("second base boost from %s, capture %d",
                               tostring(boost_use.base[1]), g2.t))
          return
        end
      elseif tick >= A.second_at + 300 then
        A.fail("no capture of the second base")
        return
      end
    end
    if not ARENA.check_hunter(A.since, c) then return end
    if A.moved ~= A.since and tick >= A.since + 3 then
      A.moved = A.since
      game.teleport(1, A.cx + 2, A.cy, 64)
    end
    if c.take and tick >= A.first + c.take then
      game.give_pill(1, pill)
      A.step = "take"
      A.take_wait = tick
      return
    end
    local ends = A.since + (BOOST_SECONDS + BOOST_DECAY_SECONDS) * 100
    if tick >= ends + 10 then
      if not A.full_seen then
        A.fail("no full-boost sample on the case's ground")
        return
      end
      if c.second and A.since == A.first then
        A.fail("the second base was never counted")
        return
      end
      if boost_use.base[1] ~= nil then
        A.fail("the base boost outlived its fade")
        return
      end
      local t = game.tank(1)
      if t ~= nil and t.mods.speed ~= 0 then
        A.fail(string.format("modifier %d after the fade", t.mods.speed))
        return
      end
      game.log(string.format("ARENA case %d (%s) ok t=%d", A.case, c.name,
                             tick))
      A.second_n = nil
      A.case = A.case + 1
      A.step = "put"
    end
  elseif A.step == "take" then
    if holder ~= 1 then
      if tick >= A.take_wait + 50 then A.fail("seat 1 did not take") end
      return
    end
    if A.take_at == nil then
      A.take_at = boost_from
      if boost_use.base[1] ~= nil then
        A.fail("the base boost outlived the take")
        return
      end
      if boost_use.mult ~= BOOST_MULT then
        A.fail(string.format("take factor %.3f", boost_use.mult))
        return
      end
    end
    if boost_from ~= A.take_at then
      if boost_from == nil and tick >= A.take_at + (BOOST_SECONDS
           + BOOST_DECAY_SECONDS) * 100 then
        game.log(string.format("ARENA case %d (%s) ok t=%d", A.case,
                               c.name, tick))
        A.case = A.case + 1
        A.step = "put"
        return
      end
      A.fail("the take's boost restarted or ended early")
      return
    end
    local t = game.tank(1)
    if t == nil or t.dead then
      A.fail("seat 1 died holding")
      return
    end
    local rule, pct, name = ARENA.ground(t)
    if rule ~= nil and tick >= A.take_at + 1 then
      local P = ARENA.pickup(rule, pct, A.take_at, BOOST_MULT)
      if P ~= nil then
        local got = ARENA.cap_of(t, rule)
        if got > math.ceil(P - 1e-9) then
          A.fail(string.format("new holder cap %d on %s over the take's "
            .. "P %.2f", got, name, P))
          return
        end
      end
    end
  end
  if A.step ~= "done" and tick >= GATE_TICKS - 100 then
    verdict(false, string.format("stuck at case %d step %s", A.case, A.step))
  end
end
