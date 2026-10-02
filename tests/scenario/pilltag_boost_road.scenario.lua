-- GATE: ticks=2500 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, the full boost drives slow ground as a road, for the holder
-- only.
--
-- At the first tick the arena notes every speed_* and turn_* rule. Seat 0
-- gets the prize at tick 300, and seat 1's speed modifier is noted then.
--
-- PASS, all of:
--  * from the take to the end of the fade no rule changes, and seat 1's
--    speed modifier stays what it was (only the holder's modifier moves);
--  * in the full boost the squares round the holder are made river, then
--    swamp, then road, with his band started again each time: his cap
--    (the whole part of the ground's speed rule x his modifier / 100) is
--    the same on all three, and it is the road's carry share x the boost,
--    rounded up;
--  * once the fade is over, on river his cap is the river's own carry
--    share, rounded up.

ARENA = { step = "note", seen = {}, puts = 0, tries = 10 }

ARENA.rules = { "speed_road", "speed_grass", "speed_forest", "speed_river",
                "speed_swamp", "speed_crater", "speed_rubble", "speed_boat",
                "speed_deep_sea", "speed_refuel_base", "turn_road",
                "turn_river", "turn_swamp", "turn_grass" }

-- Makes the 5 by 5 squares round seat 0 terrain `code`, puts him in the
-- middle one and starts his band again, so the next carry_legs sets the
-- upper cap of his share there. A boosted bot can leave the patch before it
-- is read; ARENA.cap then puts him back, up to ARENA.tries times.
ARENA.put = function(code)
  local A = ARENA
  local me = game.tank(0)
  -- A boat would carry him at the boat's speed: he is taken off it while
  -- the square under him is still water.
  if me.boat then
    game.set_boat(0, false)
  end
  for dy = -2, 2 do
    for dx = -2, 2 do
      game.set_tile(me.mx + dx, me.my + dy, code)
    end
  end
  game.teleport(0, me.mx, me.my, 64)
  -- A teleport onto water puts the tank on a boat; it is taken off again.
  if code == game.TERRAIN.river then
    game.set_boat(0, false)
  end
  carry_start()
  A.put_at = game.tick()
  A.code = code
  return true
end

-- Seat 0's cap on the square he was put on, once carry_legs has run there;
-- nil while it is too soon.
ARENA.cap = function(what, rule)
  local A = ARENA
  if game.tick() < A.put_at + 3 then
    return nil
  end
  local t = game.tank(0)
  if t ~= nil and not t.dead and holder == 0 and A.puts < ARENA.tries
     and (t.boat or game.map_tile(t.mx, t.my) ~= A.code) then
    A.puts = A.puts + 1
    ARENA.put(A.code)
    return nil
  end
  if t == nil or t.dead or holder ~= 0 or t.boat
     or game.map_tile(t.mx, t.my) ~= A.code then
    -- A teleport can take a tick or two to settle; past that it is a fail.
    if game.tick() >= A.put_at + 30 or A.puts >= ARENA.tries then
      verdict(false, string.format("seat 0 is not a holder on land on the "
        .. "%s: holder %s dead %s boat %s tile %s want %s", what,
        tostring(holder), tostring(t and t.dead), tostring(t and t.boat),
        tostring(t and game.map_tile(t.mx, t.my)), tostring(A.code)))
    end
    return nil
  end
  local pct = (t.mods.speed == 0) and 100 or t.mods.speed
  local cap = math.floor(game.rule(rule) * pct / 100)
  game.log(string.format("ARENA %s t=%d modifier=%d cap=%d", what,
                         game.tick(), pct, cap))
  return cap
end

-- Fails when a rule moved or seat 1's speed modifier moved.
ARENA.still = function()
  local A = ARENA
  for name, was in pairs(A.seen) do
    if game.rule(name) ~= was then
      verdict(false, string.format("t=%d: %s is %d, was %d", game.tick(),
                                   name, game.rule(name), was))
      return false
    end
  end
  local t1 = game.tank(1)
  if t1 ~= nil and t1.mods.speed ~= A.seat1_speed then
    verdict(false, string.format("t=%d: seat 1's speed modifier is %d, "
                                 .. "was %d", game.tick(), t1.mods.speed,
                                 A.seat1_speed))
    return false
  end
  return true
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if A.step ~= "note" and A.step ~= "give" and A.step ~= "done" then
    if not ARENA.still() then return end
  end
  if A.step == "note" then
    for _, name in ipairs(A.rules) do
      A.seen[name] = game.rule(name)
    end
    A.step = "give"
  elseif A.step == "give" and tick >= 300 then
    local t1 = game.tank(1)
    A.seat1_speed = t1 and t1.mods.speed or 0
    game.give_pill(0, pill)
    A.given_at = tick
    A.step = "start"
  elseif A.step == "start" and tick >= A.given_at + 10 then
    if holder ~= 0 then
      verdict(false, "seat 0 is not the holder after the give")
      return
    end
    local road = carry_by_code[game.TERRAIN.road]
    A.want = math.ceil(game.rule("speed_road") * road.pct / 100
                       * boost_use.mult)
    A.puts = 0
    if ARENA.put(game.TERRAIN.river) then
      A.step = "on_river"
    end
  elseif A.step == "on_river" then
    A.river = ARENA.cap("river", "speed_river")
    if A.river ~= nil then A.puts = 0 end
    if A.river ~= nil and ARENA.put(game.TERRAIN.swamp) then
      A.step = "on_swamp"
    end
  elseif A.step == "on_swamp" then
    A.swamp = ARENA.cap("swamp", "speed_swamp")
    if A.swamp ~= nil then A.puts = 0 end
    if A.swamp ~= nil and ARENA.put(game.TERRAIN.road) then
      A.step = "on_road"
    end
  elseif A.step == "on_road" then
    local road = ARENA.cap("road", "speed_road")
    if road ~= nil then
      if tick >= A.given_at + BOOST_SECONDS * 100 then
        verdict(false, "the full boost ended before the road was read")
        return
      end
      if A.river ~= A.want or A.swamp ~= A.want or road ~= A.want then
        verdict(false, string.format("caps in the boost: river %d swamp %d "
                                     .. "road %d, wanted %d", A.river,
                                     A.swamp, road, A.want))
        return
      end
      A.step = "faded"
    end
  elseif A.step == "faded"
     and tick >= A.given_at + (BOOST_SECONDS + BOOST_DECAY_SECONDS) * 100
                 + 20 then
    if boost_from ~= nil then
      verdict(false, "the boost outlived its fade")
      return
    end
    A.puts = 0
    if ARENA.put(game.TERRAIN.river) then
      A.step = "after"
    end
  elseif A.step == "after" then
    local cap = ARENA.cap("river after the boost", "speed_river")
    if cap ~= nil then
      local river = carry_by_code[game.TERRAIN.river]
      local want = math.ceil(game.rule("speed_river") * river.pct / 100)
      if cap ~= want then
        verdict(false, string.format("river cap after the boost %d, "
                                     .. "wanted %d", cap, want))
        return
      end
      A.step = "done"
      verdict(true, string.format("cap %d on river, swamp and road in the "
                                  .. "boost, %d on river after; no rule "
                                  .. "and no seat 1 change", A.want, cap))
    end
  end
  if tick >= GATE_TICKS - 100 and A.step ~= "done" then
    verdict(false, "stuck at step " .. A.step)
  end
end
