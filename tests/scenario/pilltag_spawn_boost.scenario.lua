-- GATE: ticks=9000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a respawn far from the prize gets a speed boost back to it:
-- none within boost_use.spawn_free squares, a full part as long as
-- boost_use.spawn_hold works out from the road's speeds past that, the same
-- cap as a base's boost, an early fade within spawn_free of the prize, and
-- a base during it replaces it.
--
-- Seat 0 gets the prize at tick 300 and is held on a road patch at H for
-- the whole arena (put back there every tick, no damage, no hop or fort),
-- so the prize is in his tank at H. Seat 1 is killed for each case and
-- respawns on a start the arena put on the deep sea for that case
-- (on_choose_start). The arena works the spawn distance D out at the spawn
-- from prize() and the tank, as the script does, and the full part it
-- wants from the rules: v = speed_road, vb = the whole part of v x the
-- road's carry share x BOOST_MULT, both a frame, 256 units a square and
-- boost_use.frames frames a second; d = D - spawn_free; t = (d/v - secs) /
-- (vb/v - 1), at most d/vb, none when d/v <= secs.
--  1. near: D 40, no boost at all;
--  2. mid: D 90 with spawn_secs 12 (with the default 30 a road needs no
--     boost under some 144 squares): the full part t, then the fade, and
--     the boost ends at spawn + t + BOOST_DECAY_SECONDS unless the tank got
--     within spawn_free first;
--  3. far: D over 200 with the defaults, a boost the whole way (t = d/vb);
--     two seconds in, the tank is put on a road patch 40 squares from H:
--     the fade starts that frame and the boost is over within
--     BOOST_DECAY_SECONDS;
--  4. base: the far start again; a second in, the tank is put on a new
--     neutral base 170 squares from H: the base's boost replaces the
--     respawn's (the base tick, no full part of its own), and it ends a
--     base's BOOST_SECONDS + BOOST_DECAY_SECONDS after the capture.
-- At every tick of a boost, seat 1's cap is never over the larger of the
-- ground's own cap and the whole part of a fresh take's share on that
-- ground at the same moment of the boost's curve; in the full part, on
-- ground at or over a road's speed, it is that whole part and over the
-- ground's cap (the boost happens).
--
-- PASS: every case does what it says.

ARENA = { step = "setup", case = 1, spawns = {}, caught = {} }

ARENA.H = { x = 30, y = 30 }
ARENA.cases = {
  { name = "near", sx = 70,  sy = 30,  secs = 30, want = "none" },
  { name = "mid",  sx = 30,  sy = 120, secs = 12, want = "time" },
  { name = "far",  sx = 220, sy = 180, secs = 30, want = "close" },
  { name = "base", sx = 220, sy = 180, secs = 30, want = "base" },
}

ARENA.fail = function(why)
  verdict(false, string.format("case %d (%s) t=%d: %s", ARENA.case,
    ARENA.cases[ARENA.case] and ARENA.cases[ARENA.case].name or "-",
    game.tick(), why))
  ARENA.step = "done"
end

ARENA.real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  local px, py = prize()
  local t = game.tank(p)
  local rec = { p = p, t = game.tick(), mx = mx, my = my }
  if px ~= nil and t ~= nil then
    rec.far = math.sqrt((t.wx / 256 - px) ^ 2 + (t.wy / 256 - py) ^ 2)
  end
  ARENA.spawns[#ARENA.spawns + 1] = rec
  ARENA.real_spawned(p, mx, my, respawn, scripted)
end

ARENA.real_captured = on_base_captured
function on_base_captured(n, old, new, scripted)
  ARENA.real_captured(n, old, new, scripted)
  ARENA.caught[#ARENA.caught + 1] = { n = n, p = new, t = game.tick() }
end

function on_choose_start(p)
  if p == 1 and ARENA.start_n ~= nil then
    return ARENA.start_n
  end
  return nil
end

ARENA.real_scale = damage_scale
function damage_scale(attacker, victim, cause)
  if victim == 0 then
    return 0
  end
  return ARENA.real_scale(attacker, victim, cause)
end

-- The full part the arena wants for a spawn `far` squares out.
ARENA.want_hold = function(far)
  local d = (far - boost_use.spawn_free) * 256
  local v = game.rule("speed_road")
  local vb = math.floor(v * CARRY_SPEED_PCT.road / 100 * BOOST_MULT + 1e-9)
  if d <= 0 then return nil end
  local slow = d / (v * boost_use.frames)
  if vb <= v or slow <= boost_use.spawn_secs then return nil end
  return math.min((slow - boost_use.spawn_secs) / (vb / v - 1),
                  d / (vb * boost_use.frames)), v, vb
end

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

-- A fresh take's share (P) on rule/pct, `since` ticks with a full part of
-- `hold` seconds, or nil once the fade is over.
ARENA.pickup = function(rule, pct, since, hold)
  local s = (game.tick() - since) / 100 - hold
  local f = (s < 0) and 0 or s / math.max(BOOST_DECAY_SECONDS, 1e-9)
  if f >= 1 then
    return nil, f
  end
  local cap = game.rule(rule)
  local share = cap * pct / 100
  local road_cap = game.rule("speed_road")
  if cap < road_cap then
    local full = road_cap * CARRY_SPEED_PCT.road / 100 * BOOST_MULT
    return full + (share - full) * f, f
  end
  return share * (BOOST_MULT + (1 - BOOST_MULT) * f), f
end

ARENA.cap_of = function(t, rule)
  local pct = (t.mods.speed == 0) and 100 or t.mods.speed
  return math.floor(game.rule(rule) * pct / 100)
end

ARENA.patch = function(cx, cy)
  for dy = -2, 2 do
    for dx = -2, 2 do
      game.set_tile(cx + dx, cy + dy, game.TERRAIN.road)
    end
  end
end

ARENA.put = function(p, x, y)
  local t = game.tank(p)
  if t ~= nil and t.boat then
    game.set_boat(p, false)
  end
  game.teleport(p, x, y, 64)
end

-- The cap check for one tick of seat 1's boost (from `since`, full part
-- `hold`). Returns false on a failure.
ARENA.check = function(since, hold)
  local A = ARENA
  local t = game.tank(1)
  if t == nil or t.dead then
    A.fail("seat 1 died")
    return false
  end
  local rule, pct, name = ARENA.ground(t)
  if rule == nil then return true end
  local P, f = ARENA.pickup(rule, pct, since, hold)
  if P == nil then return true end
  local cap = game.rule(rule)
  local got = ARENA.cap_of(t, rule)
  local ceiling = math.max(cap, math.floor(P + 1e-9))
  if got > ceiling then
    A.fail(string.format("cap %d on %s (modifier %d) over the pickup cap "
      .. "%d (P %.2f, fade %.2f)", got, name, t.mods.speed, ceiling, P, f))
    return false
  end
  A.max_cap = math.max(A.max_cap or 0, got)
  if f == 0 and cap >= game.rule("speed_road")
     and game.tick() >= since + 3 then
    if got ~= math.floor(P + 1e-9) or got <= cap then
      A.fail(string.format("cap %d on %s in the full boost, wanted %d "
        .. "(P %.2f), ground cap %d", got, name, math.floor(P + 1e-9), P,
        cap))
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

ARENA.dist = function(p)
  local px, py = prize()
  local t = game.tank(p)
  if px == nil or t == nil then return nil end
  return math.sqrt((t.wx / 256 - px) ^ 2 + (t.wy / 256 - py) ^ 2)
end

ARENA.real_tick = on_tick
function on_tick(tick)
  ARENA.real_tick(tick)
  local A = ARENA
  if pill == nil or A.step == "done" then return end
  -- Seat 0 stays at H and makes no hop or fort.
  hop_after = game.tick() + 100000
  if A.step ~= "setup" then
    local t0 = game.tank(0)
    if t0 ~= nil and not t0.dead and (t0.mx ~= A.H.x or t0.my ~= A.H.y) then
      ARENA.put(0, A.H.x, A.H.y)
    end
  end
  local c = A.cases[A.case]
  if A.step == "setup" then
    ARENA.patch(A.H.x, A.H.y)
    for _, cs in ipairs(A.cases) do
      cs.start = game.add_start(cs.sx, cs.sy, 4)
      if cs.start == nil then
        A.fail(string.format("no start at %d,%d", cs.sx, cs.sy))
        return
      end
    end
    ARENA.patch(A.H.x + 40, A.H.y + 4)
    ARENA.put(0, A.H.x, A.H.y)
    A.step = "give"
  elseif A.step == "give" and tick >= 300 then
    game.give_pill(0, pill)
    A.step = "kill"
  elseif A.step == "kill" then
    if c == nil then
      verdict(true, string.format("respawn boosts as worked out, capped, "
        .. "early fade, base replaces; top cap %d", A.max_cap or -1))
      A.step = "done"
      return
    end
    if holder ~= 0 then
      if tick >= 500 then A.fail("seat 0 is not the holder") end
      return
    end
    local t = game.tank(1)
    if t == nil or t.dead then return end
    boost_use.spawn_secs = c.secs
    A.start_n = c.start
    A.full_seen = false
    A.killed_at = tick
    A.nspawns = #A.spawns
    game.kill_tank(1)
    A.step = "spawn"
  elseif A.step == "spawn" then
    local rec = nil
    for i = A.nspawns + 1, #A.spawns do
      if A.spawns[i].p == 1 then rec = A.spawns[i] end
    end
    if rec == nil then
      if tick >= A.killed_at + 2000 then A.fail("seat 1 did not respawn") end
      return
    end
    if rec.far == nil then
      A.fail("no spawn distance")
      return
    end
    if rec.mx ~= c.sx or rec.my ~= c.sy then
      A.fail(string.format("spawned at %d,%d, not the case's %d,%d",
                           rec.mx, rec.my, c.sx, c.sy))
      return
    end
    local want, v, vb = ARENA.want_hold(rec.far)
    game.log(string.format("ARENA case %d spawn D %.1f: want %s (v %s vb %s),"
      .. " script %s", A.case, rec.far, tostring(want), tostring(v),
      tostring(vb), tostring(boost_use.hold[1])))
    A.rec = rec
    if c.want == "none" then
      if want ~= nil or boost_use.base[1] ~= nil or boost_use.hold[1] ~= nil
      then
        A.fail(string.format("a boost at D %.1f", rec.far))
        return
      end
      A.step = "none"
      return
    end
    if want == nil then
      A.fail(string.format("the arena wants no boost at D %.1f", rec.far))
      return
    end
    if boost_use.base[1] ~= rec.t or boost_use.hold[1] == nil
       or math.abs(boost_use.hold[1] - want) > 1e-6 then
      A.fail(string.format("boost from %s hold %s, wanted %d and %.3f",
        tostring(boost_use.base[1]), tostring(boost_use.hold[1]), rec.t,
        want))
      return
    end
    if c.want == "close" and math.abs(want - (rec.far - boost_use.spawn_free)
         * 256 / (vb * boost_use.frames)) > 1e-6 then
      A.fail("the far case is not a boost the whole way")
      return
    end
    A.since, A.hold = rec.t, want
    A.step = "boost"
  elseif A.step == "none" then
    local t = game.tank(1)
    if boost_use.base[1] ~= nil or (t ~= nil and t.mods.speed ~= 0) then
      A.fail("a boost on a near respawn")
      return
    end
    if tick >= A.rec.t + 200 then
      game.log(string.format("ARENA case %d (%s) ok t=%d", A.case, c.name,
                             tick))
      A.case = A.case + 1
      A.step = "kill"
    end
  elseif A.step == "boost" then
    local hold = boost_use.hold[1]
    if boost_use.base[1] ~= nil then
      if boost_use.base[1] ~= A.since then
        if c.want == "base" and A.base_n ~= nil then
          local cap = nil
          for _, cc in ipairs(A.caught) do
            if cc.n == A.base_n and cc.p == 1 then cap = cc end
          end
          if cap ~= nil and boost_use.base[1] == cap.t and hold == nil then
            A.since, A.hold, A.based = cap.t, BOOST_SECONDS, true
            A.full_seen = false
            game.log(string.format("ARENA case %d base boost replaced the "
              .. "respawn's at %d", A.case, cap.t))
          else
            A.fail(string.format("boost from %s hold %s after the base",
              tostring(boost_use.base[1]), tostring(hold)))
            return
          end
        else
          A.fail("the boost restarted")
          return
        end
      elseif A.based then
        if hold ~= nil then
          A.fail("a full part of its own after the base")
          return
        end
      elseif hold ~= A.hold then
        -- An early fade: only within spawn_free of the prize, and the full
        -- part cut to now.
        local dd = ARENA.dist(1)
        if dd == nil or dd > boost_use.spawn_free + 1e-6 or hold == nil
           or math.abs(hold - (game.tick() - A.since) / 100) > 0.03 then
          A.fail(string.format("full part %s from %.3f at D %s",
                               tostring(hold), A.hold, tostring(dd)))
          return
        end
        game.log(string.format("ARENA case %d early fade at D %.1f, full "
          .. "part %.2f of %.2f", A.case, dd, hold, A.hold))
        A.hold = hold
        A.cut_at = tick
      end
      if not ARENA.check(A.since, A.hold) then return end
    else
      -- Over. It must be at the end of the curve.
      local ends = A.since + (A.hold + BOOST_DECAY_SECONDS) * 100
      if tick < ends - 3 then
        A.fail(string.format("the boost ended at %d, curve ends %.0f", tick,
                             ends))
        return
      end
      local t = game.tank(1)
      if t ~= nil and t.mods.speed ~= 0 then
        A.fail(string.format("modifier %d after the boost", t.mods.speed))
        return
      end
      if c.want == "close" and A.cut_at == nil then
        A.fail("no early fade")
        return
      end
      if c.want == "base" and not A.based then
        A.fail("the base never replaced the boost")
        return
      end
      if not A.full_seen then
        A.fail("no full-boost sample on fast ground")
        return
      end
      game.log(string.format("ARENA case %d (%s) ok t=%d", A.case, c.name,
                             tick))
      A.cut_at, A.based, A.base_n, A.moved = nil, nil, nil, nil
      A.case = A.case + 1
      A.step = "kill"
      return
    end
    if tick >= A.since + (A.hold + BOOST_DECAY_SECONDS) * 100 + 10 then
      A.fail("the boost outlived its curve")
      return
    end
    if c.want == "close" and not A.moved and tick >= A.rec.t + 200 then
      A.moved = true
      ARENA.put(1, A.H.x + 40, A.H.y + 4)
    end
    if c.want == "base" and not A.moved and tick >= A.rec.t + 100 then
      A.moved = true
      local bx, by = A.H.x + 170, A.H.y + 30
      for dy = -2, 2 do
        for dx = -2, 2 do
          game.set_tile(bx + dx, by + dy, game.TERRAIN.road)
        end
      end
      A.base_n = game.add_base(bx, by)
      if A.base_n == nil then
        A.fail("no base could be put down")
        return
      end
      ARENA.put(1, bx, by)
    end
  end
  if A.step ~= "done" and tick >= GATE_TICKS - 100 then
    verdict(false, string.format("stuck at case %d step %s", A.case, A.step))
  end
end
