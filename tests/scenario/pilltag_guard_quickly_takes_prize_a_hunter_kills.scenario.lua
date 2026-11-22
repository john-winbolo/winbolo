-- GATE: ticks=8000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag: a hunter shoots the guard's built prize dead, and the guard
-- quickly drives over it and takes the prize back.
--
-- The setup is the one in pilltag_fort_guard_kills_own_pillbox_to_keep_it.
-- Player 0 gets the prize at tick 300 and is set up for a built prize
-- (armour held at 15, player 1 7 squares behind, then 5 when the built prize
-- goes up). Once player 0's builder is back in his tank, the arena sets the
-- built prize to LAST_SHOT_ARMOUR, so one hit kills it, and the mod starts
-- the take ("last shot").
--
-- Player 1 keeps its shells and the mod's shell a second, and it shoots at
-- the built prize. From the build to the kill, player 0 has no shells, so the
-- kill is player 1's for sure: the hunter wins the race to the last shot.
-- Over the same time his tank is kept whole: with no gun he cannot shoot
-- back, and player 1 shot him dead (t=1760) before it shot the built prize.
-- After the kill, player 0's gun fills again the way the mod fills it, and
-- his armour is his own again.
--
-- The built prize then lies on the ground as a dead pill, and nobody holds
-- it: the first tank to drive over it gets it.
--
-- PASS: player 0 drives over it and has the prize in his tank within
-- PICKUP_SECONDS (5) of player 1's kill, before player 1 picks it up.
-- FAIL: player 1 picked it up first, nobody picked it up within 5 s, player 0
-- died before he picked it up, or the setup did not happen (no built prize,
-- player 0 killed it, or nobody killed it).

-- PICKUP_SECONDS and the rest are fields, not locals: the script and the
-- arena are near Lua's limit of 200 locals in one chunk.
ARENA = { given = false, PICKUP_SECONDS = 5, dry = {} }

-- A seat in ARENA.dry gets no shells from game.add_stocks (the mod's shell a
-- second); anything else in the same call still goes through.
ARENA.real_add_stocks = game.add_stocks
game.add_stocks = function(p, s)
  if ARENA.dry[p] and s ~= nil and s.shells ~= nil then
    local rest = {}
    for k, v in pairs(s) do
      if k ~= "shells" then rest[k] = v end
    end
    if next(rest) == nil then return true end
    s = rest
  end
  return ARENA.real_add_stocks(p, s)
end

-- How far seat p's tank is from the prize's square, in squares (the larger
-- of the two axes), or -1 for a dead or missing tank.
function ARENA.away(p, pb)
  local t = game.tank(p)
  if t == nil or t.dead or pb == nil then return -1 end
  return math.max(math.abs(t.mx - pb.x), math.abs(t.my - pb.y))
end

-- See pilltag_fort_guard_kills_own_pillbox_to_keep_it: the road boost on slow
-- ground moves the guard off the square the setup puts the hunter behind.
boost_use.road = false

local arena_real_killed = on_pill_killed
function on_pill_killed(n, by, scripted)
  local A = ARENA
  if n == pill and A.armour_at ~= nil and A.killed_by == nil then
    local pb = game.pill(pill)
    A.killed_by, A.killed_at = by, game.tick()
    A.dry[0] = nil
    game.log(string.format("ARENA fort killed by %s t=%d at %d,%d, player 0 %d and player 1 %d squares off",
                           tostring(by), game.tick(), pb.x, pb.y,
                           A.away(0, pb), A.away(1, pb)))
  end
  arena_real_killed(n, by, scripted)
end

ARENA.real_tank_killed = on_tank_killed
function on_tank_killed(victim, killer, cause, scripted)
  game.log(string.format("ARENA tank %s killed by %s, cause %s, tick %d",
                         tostring(victim), tostring(killer), tostring(cause),
                         game.tick()))
  return ARENA.real_tank_killed(victim, killer, cause, scripted)
end

local arena_real_picked = on_pill_picked_up
function on_pill_picked_up(n, p, scripted)
  local A = ARENA
  if n == pill and A.killed_at ~= nil and A.picked_by == nil then
    A.picked_by, A.picked_at = p, game.tick()
    game.log(string.format("ARENA picked up by player %s t=%d, %.2fs after the kill",
                           tostring(p), game.tick(),
                           (game.tick() - A.killed_at) / 100))
  end
  arena_real_picked(n, p, scripted)
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if not A.given then
    if tick >= 300 then
      local pb = game.pill(pill)
      game.teleport(0, pb.x, pb.y, 64)
      game.teleport(1, pb.x - 7, pb.y, 64)
      game.give_pill(0, pill)
      A.given = true
    end
    return
  end
  local me = game.tank(0)
  local pb = game.pill(pill)
  if plan == nil and A.built_at == nil and tick % 100 == 0 and me ~= nil
     and not me.dead then
    game.set_stocks(0, { armour = 15 })
    local fx, fy = facing(me.dir)
    local k = 7 / math.max(math.abs(fx), math.abs(fy))
    game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
  end
  if plan ~= nil and not plan.built and A.moved == nil and me ~= nil then
    A.moved = true
    local fx, fy = facing(me.dir)
    local k = 5 / math.max(math.abs(fx), math.abs(fy))
    game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
  end
  if plan ~= nil and plan.built and A.built_at == nil then
    A.built_at = tick
    if plan.kind ~= "fort" then
      verdict(false, "setup did not happen: built a " .. plan.kind .. ", not a fort")
      return
    end
    -- The guard's tank is made whole, so this tests the pickup, not whether
    -- a hurt tank outlives a hunter 5 squares off.
    game.set_stocks(0, { armour = game.rule("tank_full_armour") })
    A.dry[0] = true
    game.log(string.format("ARENA fort up at %d,%d t=%d, player 0 has no shells",
                           plan.x, plan.y, tick))
  end
  -- From the build to the kill, player 0 has no shells (the take fills his
  -- gun, so this is asked every tick), and his tank is kept whole: with no
  -- gun, player 1 shoots him dead before it shoots the built prize.
  if A.dry[0] and me ~= nil and not me.dead then
    if me.shells > 0 then
      game.set_stocks(0, { shells = 0 })
    end
    if me.armour < game.rule("tank_full_armour") then
      game.set_stocks(0, { armour = game.rule("tank_full_armour") })
    end
  end
  -- One hit kills it, set once the guard's builder is back in his tank.
  if A.built_at ~= nil and A.armour_at == nil and plan ~= nil and
     plan.kind == "fort" and standing(pb) then
    local man = game.builder(0)
    if man ~= nil and man.state == "in_tank" then
      A.armour_at = tick
      game.set_pill_armour(pill, LAST_SHOT_ARMOUR)
      local h = game.tank(1)
      game.log(string.format("ARENA builder in tank t=%d, armour set to %d, player 1 has %d shells, %d squares off",
                             tick, LAST_SHOT_ARMOUR, h and h.shells or -1,
                             A.away(1, pb)))
    end
  end
  if A.built_at ~= nil and A.armour_at == nil and (plan == nil or not standing(pb)) then
    verdict(false, "setup did not happen: the fort was lost before its armour was set")
  end
  if A.armour_at ~= nil and A.killed_by == nil and me ~= nil and me.dead then
    verdict(false, "setup did not happen: player 0 died before the kill")
  end
  if A.killed_by ~= nil and A.killed_by ~= 1 then
    verdict(false, string.format("setup did not happen: player %s killed the fort, not player 1",
                                 tostring(A.killed_by)))
  end
  -- Once a second after the kill, where both tanks are.
  if A.killed_at ~= nil and A.picked_by == nil and
     (tick - A.killed_at) % 100 == 0 and tick - A.killed_at <= 1000 then
    game.log(string.format("ARENA %.0fs after the kill: player 0 %d and player 1 %d squares off",
                           (tick - A.killed_at) / 100, A.away(0, pb), A.away(1, pb)))
  end
  if A.killed_by == 1 then
    local after = (tick - A.killed_at) / 100
    if A.picked_by == 1 then
      verdict(false, string.format("player 1 picked it up %.1fs after its kill, before player 0",
                                   (A.picked_at - A.killed_at) / 100))
    elseif A.picked_by == 0 then
      local secs = (A.picked_at - A.killed_at) / 100
      verdict(secs <= A.PICKUP_SECONDS,
              string.format("player 0 picked it up %.1fs after player 1's kill%s", secs,
                            secs <= A.PICKUP_SECONDS and "" or
                              string.format(" (over %d s)", A.PICKUP_SECONDS)))
    elseif A.picked_by ~= nil then
      verdict(false, string.format("player %s picked it up %.1fs after the kill",
                                   tostring(A.picked_by), (A.picked_at - A.killed_at) / 100))
    elseif me ~= nil and me.dead then
      verdict(false, string.format("player 0 died %.1fs after the kill, before he picked it up",
                                   after))
    elseif after > A.PICKUP_SECONDS then
      verdict(false, string.format("not picked up within %d s of the kill: player 0 %d, player 1 %d squares off",
                                   A.PICKUP_SECONDS, A.away(0, pb), A.away(1, pb)))
    end
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, A.built_at == nil and "setup did not happen: no fort" or
                   (A.killed_by == nil and "setup did not happen: nobody killed the fort" or
                    "never picked up"))
  end
end
