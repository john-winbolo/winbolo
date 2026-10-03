-- GATE: ticks=3000 bots=1 script=data/mods/BasesOnly.scenario.lua
--
-- Bases Only, no pillbox in play and the bases still work.
--
-- The map has five pillboxes and two neutral bases (see
-- tests/generate_bases_only_maps.py). Seat 0 is the one bot.
--
-- 1. At the round start: the map's five pill slots are still counted, no
--    slot holds a live pillbox, and no tank carries one.
-- 2. At tick 200 the arena empties seat 0's shells, cuts its armour to 5 and
--    puts it on base 1. Within 10 seconds base 1 must be seat 0's and the
--    tank must have taken shells or armour from it.
-- 3. Then the arena brings a pillbox back the way another script could: it
--    adds one and puts it in seat 0's tank. Within 2 seconds the mod must
--    have dropped it and taken it off again.
--
-- Every second, outside step 3, no live pillbox may be on the map and no
-- tank may carry one.
--
-- PASS: all three steps hold, and the checks every second never fail.

ARENA = { phase = "start", next_check = 0 }

-- Live pillboxes on the map, and pillboxes carried by every tank.
function ARENA.count()
  local live, carried = 0, 0
  for n = 1, game.num_pills() do
    if game.pill(n) ~= nil then live = live + 1 end
  end
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t ~= nil then carried = carried + (t.pills or 0) end
  end
  return live, carried
end

function on_start()
  local A = ARENA
  local live, carried = A.count()
  game.log(string.format("ARENA start slots=%d live=%d carried=%d",
                         game.num_pills(), live, carried))
  if game.num_pills() ~= 5 then
    verdict(false, "the map should have 5 pill slots, has " .. game.num_pills())
  elseif live ~= 0 or carried ~= 0 then
    verdict(false, string.format("at start live=%d carried=%d", live, carried))
  else
    A.phase = "wait_base"
  end
end

function on_tick(tick)
  local A = ARENA

  -- The check every second, skipped while step 3 has a pillbox in play.
  if A.phase ~= "net" and tick >= A.next_check then
    A.next_check = tick + 100
    local live, carried = A.count()
    if live ~= 0 or carried ~= 0 then
      verdict(false, string.format("t=%d live=%d carried=%d", tick, live, carried))
      return
    end
  end

  if A.phase == "wait_base" and tick >= 200 then
    local b = game.base(1)
    game.set_stocks(0, { shells = 0, armour = 5 })
    local ok, code = game.teleport(0, b.x, b.y)
    game.log(string.format("ARENA onto base 1 at %d,%d: %s %s", b.x, b.y,
                           tostring(ok), tostring(code)))
    A.phase, A.at = "base", tick
  elseif A.phase == "base" then
    local b, t = game.base(1), game.tank(0)
    if b.owner == 0 and t ~= nil and (t.shells > 0 or t.armour > 5) then
      game.log(string.format("ARENA base 1 taken, tank shells=%d armour=%d",
                             t.shells, t.armour))
      local b2 = game.base(2)
      local n, code = game.add_pill(b2.x, b2.y - 3)
      if n == nil then
        verdict(false, "add_pill refused: " .. tostring(code))
        return
      end
      local ok, why = game.give_pill(0, n)
      if not ok then
        verdict(false, "give_pill refused: " .. tostring(why))
        return
      end
      A.pill, A.phase, A.at = n, "net", tick
      game.log(string.format("ARENA gave pill %d to seat 0", n))
    elseif tick >= A.at + 1000 then
      verdict(false, string.format("base 1 owner=%s tank shells=%s armour=%s",
              tostring(b.owner), tostring(t and t.shells), tostring(t and t.armour)))
    end
  elseif A.phase == "net" then
    local live, carried = A.count()
    if live == 0 and carried == 0 then
      verdict(true, string.format("no pills at start, base taken, given pill gone in %.2fs",
              (tick - A.at) / 100))
    elseif tick >= A.at + 200 then
      verdict(false, string.format("given pill still here: live=%d carried=%d",
              live, carried))
    end
  end
end
