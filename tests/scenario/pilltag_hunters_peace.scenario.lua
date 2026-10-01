-- GATE: ticks=12000 bots=3 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, two hunters side by side: they leave each other alone until
-- one of them shoots the other.
--
-- The script hands every hunter a peace list ("peace=" in its init table):
-- every other seat in the round but the holder. A brain does not pick a fight
-- with a seat on its list until that seat hurts it. Then the seat is a normal
-- enemy for PEACE_HOSTILE_TICKS (30 s), and is left alone for the next
-- PEACE_COOLDOWN_TICKS (5 s) whatever it does.
--
-- Phase 1. Seat 0 gets the prize at tick 300. Seats 1 and 2 are put 4
-- squares apart, and seat 0 8 squares north of them. Seat 0 is kept there
-- and kept at full armour; so are the hunters.
-- PASS phase 1: in 20 s, a hunter hits the holder or the prize, and neither
-- hunter picks a fight with the other. Two kinds of hit are not a fight: a
-- crossfire hit, where the hunter hit was within a square of the holder (the
-- shell was for the holder; the hunters crowd him and can share his square),
-- and a hit back on a hunter that hit this one in the 30 s before (the peace
-- list allows that).
--
-- Phase 2. Seat 0 is put in a far corner of the island, out of sight, and
-- kept there. A hunter hit in phase 1 (a crossfire hit, say) still holds the
-- hitter as an enemy, so first the hunters are parked in two other corners
-- until 36 s have passed since the last hit between them: 30 s of enemy, 5
-- of cooldown and 1 for the brain's look. Then they are put 5 squares apart, facing each other, guns
-- full. The script's orders to both hunters are held off (its own order
-- would send them across the island after the holder), seat 1's peace list is
-- emptied, and seat 1 is told to attack seat 2.
-- PASS phase 2: seat 2 hits seat 1 back inside the 30 s after seat 1's first
-- hit on it, and seat 2 lands no hit on seat 1 from 2 s after those 30 s end
-- to the end of the 5 s that follow. The 2 s are for shells still in the air
-- and the brain's look at its armour, which comes a think after the hit.

ARENA = { given = false, hits = {}, lines = {} }
scenario.callbacks.on_tank_hit = "Arena: counts the tank hits between the seats."
scenario.callbacks.pill_damage_scale = "Arena: counts the hunters' hits on the prize."

function on_tank_hit(victim, attacker, cause, amount, n, scripted)
  local A = ARENA
  if cause ~= "shell" or attacker == nil or attacker == victim then return end
  local t = game.tick()
  local cross = false
  local v, h = game.tank(victim), game.tank(0)
  if victim ~= 0 and v ~= nil and h ~= nil and not h.dead then
    cross = chebyshev(v.mx, v.my, h.mx, h.my) <= 1
  end
  A.hits[#A.hits + 1] = { t = t, from = attacker, to = victim, cross = cross }
  A.lines[#A.lines + 1] = string.format("ARENA hit p%d -> p%d t=%d phase=%s%s",
    attacker, victim, t, tostring(A.phase), cross and " crossfire" or "")
end

function pill_damage_scale(attacker, n, cause, by_pill)
  local A = ARENA
  if n == pill and A.phase == 1 and (attacker == 1 or attacker == 2) and
     A.prize_hit == nil then
    A.prize_hit = game.tick()
  end
  return 100
end

-- Hunter-on-hunter hits in [t0, t1] that are a fight: not a crossfire hit
-- and not a hit back within 30 s (3000 ticks) of a hit the other way.
function ARENA.fights(t0, t1)
  local n = 0
  for i, h in ipairs(ARENA.hits) do
    if h.from >= 1 and h.from <= 2 and h.to >= 1 and h.to <= 2 and
       h.t >= t0 and h.t <= t1 and not h.cross then
      local back = false
      for j = 1, i - 1 do
        local g = ARENA.hits[j]
        if g.from == h.to and g.to == h.from and h.t - g.t <= 3000 then
          back = true
        end
      end
      if not back then n = n + 1 end
    end
  end
  return n
end

-- Hits from `from` to `to` that landed in [t0, t1].
function ARENA.hits_between(from, to, t0, t1)
  local n, last = 0, nil
  for _, h in ipairs(ARENA.hits) do
    if h.from == from and h.to == to and h.t >= t0 and h.t <= t1 then
      n = n + 1
      last = h.t
    end
  end
  return n, last
end

function ARENA.hold(p, x, y, dir)
  local s = game.tank(p)
  if s ~= nil and not s.dead and chebyshev(s.mx, s.my, x, y) > 1 then
    game.teleport(p, x, y, dir)
  end
end

-- Seat 0 in a far corner, the hunters face to face (phase 2).
function ARENA.face_to_face()
  local A = ARENA
  A.hx, A.hy = standable_near(110, 110)
  A.ax, A.ay = standable_near(128, 138)
  A.bx, A.by = standable_near(133, 138)
  if A.hx == nil or A.ax == nil or A.bx == nil then
    return false
  end
  game.teleport(0, A.hx, A.hy, 128)
  game.teleport(1, A.ax, A.ay, 64)
  game.teleport(2, A.bx, A.by, 192)
  for p = 1, 2 do
    game.set_stocks(p, { shells = game.rule("tank_full_shells") })
  end
  return true
end

-- The script's next order to a hunter is held off: an order for the same
-- quarry waits CHANGE_AFTER seconds from the last one.
function ARENA.hold_orders()
  for p = 1, 2 do
    told_for[p] = "tank 0"
    told_at[p]  = elapsed
  end
end

function ARENA.top_up()
  for p = 0, 2 do
    local s = game.tank(p)
    if s ~= nil and not s.dead then
      game.set_stocks(p, { armour = game.rule("tank_full_armour") })
    end
  end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  for _, l in ipairs(A.lines) do game.log(l) end
  A.lines = {}
  if pill == nil then return end
  if not A.given then
    if tick >= 300 then
      A.hx, A.hy = standable_near(122, 118)
      A.ax, A.ay = standable_near(120, 126)
      A.bx, A.by = standable_near(124, 126)
      if A.hx == nil or A.ax == nil or A.bx == nil then
        verdict(false, "no square for phase 1")
        return
      end
      game.teleport(0, A.hx, A.hy, 128)
      game.teleport(1, A.ax, A.ay, 0)
      game.teleport(2, A.bx, A.by, 0)
      game.give_pill(0, pill)
      for p = 1, 2 do
        game.set_stocks(p, { shells = game.rule("tank_full_shells") })
      end
      A.given = true
      A.phase = 1
      A.t1 = tick
      game.log(string.format("ARENA phase 1 t=%d holder %d,%d hunters %d,%d and %d,%d",
                             tick, A.hx, A.hy, A.ax, A.ay, A.bx, A.by))
    end
    return
  end
  if tick % 25 == 0 then A.top_up() end

  if A.phase == 1 then
    if tick % 25 == 0 then A.hold(0, A.hx, A.hy, 128) end
    if tick < A.t1 + 2000 then return end
    local hvh = A.hits_between(1, 2, A.t1, tick) + A.hits_between(2, 1, A.t1, tick)
    local fights = A.fights(A.t1, tick)
    local on_holder, first = 0, nil
    for _, h in ipairs(A.hits) do
      if h.to == 0 and (h.from == 1 or h.from == 2) then
        on_holder = on_holder + 1
        first = first or h.t
      end
    end
    game.log(string.format("ARENA phase 1 over: hunter-on-hunter hits %d (%d a fight), hits on "
                           .. "the holder %d (first t=%s), first prize hit t=%s", hvh, fights,
                           on_holder, tostring(first), tostring(A.prize_hit)))
    if fights > 0 then
      verdict(false, string.format("phase 1: the hunters fought each other (%d hits)", fights))
      return
    end
    if on_holder == 0 and A.prize_hit == nil then
      verdict(false, "phase 1: no hunter hit the holder or the prize in 20 s")
      return
    end
    A.p1_why = string.format("phase 1: no fight (%d stray hits), %d on the holder", hvh, on_holder)
    -- Phase 2: first the wait apart.
    A.hx, A.hy = standable_near(110, 110)
    A.ax, A.ay = standable_near(142, 142)
    A.bx, A.by = standable_near(142, 110)
    if A.hx == nil or A.ax == nil or A.bx == nil then
      verdict(false, "no square for the wait")
      return
    end
    local last = 0
    for _, h in ipairs(A.hits) do
      if h.from >= 1 and h.from <= 2 and h.to >= 1 and h.to <= 2 then last = h.t end
    end
    A.wait_to = math.max(tick, last + 3600)
    A.phase = 2
    game.log(string.format("ARENA phase 2 wait t=%d to t=%d hunters parked at %d,%d and %d,%d",
                           tick, A.wait_to, A.ax, A.ay, A.bx, A.by))
    return
  end

  -- Phase 2.
  if tick % 25 == 0 then A.hold(0, A.hx, A.hy, 128) end
  A.hold_orders()
  if A.wait_to ~= nil then
    if tick % 25 == 0 then
      A.hold(1, A.ax, A.ay, 0)
      A.hold(2, A.bx, A.by, 0)
    end
    if tick < A.wait_to then return end
    A.wait_to = nil
    if not A.face_to_face() then
      verdict(false, "no square for phase 2")
      return
    end
    game.bot_init(1, { peace = "" })
    A.tuned1 = tuned[1]
    A.t2 = tick
    game.log(string.format("ARENA phase 2 t=%d holder %d,%d hunters %d,%d and %d,%d",
                           tick, A.hx, A.hy, A.ax, A.ay, A.bx, A.by))
    return
  end
  -- The script re-sends seat 1's table if its part changes; its peace list
  -- is emptied again.
  if tuned[1] ~= A.tuned1 then
    game.bot_init(1, { peace = "" })
    A.tuned1 = tuned[1]
  end
  if A.ta == nil then
    local n, _ = A.hits_between(1, 2, A.t2, tick)
    if n > 0 then
      for _, h in ipairs(A.hits) do
        if h.from == 1 and h.to == 2 and h.t >= A.t2 then A.ta = h.t; break end
      end
      game.log(string.format("ARENA seat 1 first hit seat 2 at t=%d", A.ta))
      return
    end
    if (tick - A.t2) % 500 == 1 then
      game.hint(1, { verb = "attack", player = 2 })
    end
    if tick > A.t2 + 2000 then
      verdict(false, "phase 2: seat 1 did not hit seat 2 in 20 s")
    end
    return
  end
  if tick < A.ta + 3500 then
    if tick % 500 == 0 then
      local s1, s2 = game.tank(1), game.tank(2)
      game.log(string.format("ARENA t=%d +%.1fs p1 %d,%d p2 %d,%d", tick,
                             (tick - A.ta) / 100, s1.mx, s1.my, s2.mx, s2.my))
    end
    return
  end
  local back, last_back = A.hits_between(2, 1, A.ta + 1, A.ta + 3000)
  local late = A.hits_between(2, 1, A.ta + 3200, A.ta + 3500)
  local a_cool = A.hits_between(1, 2, A.ta + 3000, A.ta + 3500)
  game.log(string.format("ARENA phase 2 over: seat 2 hit back %d times in the 30 s (last +%s s), "
                         .. "%d times in the cooldown check; seat 1 hit seat 2 %d times in the cooldown",
                         back, last_back and string.format("%.1f", (last_back - A.ta) / 100) or "-",
                         late, a_cool))
  if back == 0 then
    verdict(false, "phase 2: seat 2 never hit seat 1 back")
    return
  end
  if late > 0 then
    verdict(false, string.format("phase 2: seat 2 hit seat 1 %d times in the cooldown", late))
    return
  end
  verdict(true, string.format("%s; phase 2: seat 2 hit back %d times, last %.1f s after the first hit, "
                              .. "then none in the cooldown (seat 1 hit it %d times there)",
                              A.p1_why, back, (last_back - A.ta) / 100, a_cool))
end
