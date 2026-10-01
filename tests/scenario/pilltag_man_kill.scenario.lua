-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a hunter kills the holder's man and the prize drops dead.
--
-- Seat 0 gets the prize at tick 300 and is set up for a hop, but this time the
-- arena keeps seat 1 7 squares AHEAD of him, so the man walks the prize
-- towards the hunter. Once the man is out, the arena leaves both tanks alone.
-- The hunter is now told to attack the holder (the man-out order) and runs on
-- the manhunt tuning.
--
-- PASS: seat 1 kills seat 0's man while he has the prize, the prize lands
-- dead on the ground in the same frame, and seat 0 no longer holds it then.
-- (Whoever drives over it next is a new take, seat 0 included.)

ARENA = { given = false }

function on_lgm_died(p, killer, mx, my, scripted)
  local A = ARENA
  if p == 0 and A.out_at ~= nil and A.died == nil then
    A.died = { killer = killer, t = game.tick() }
    game.log(string.format("ARENA man died killer=%s t=%d", tostring(killer),
                           game.tick()))
  end
end

local arena_real_placed = on_pill_placed
function on_pill_placed(n, p, armour, scripted)
  arena_real_placed(n, p, armour, scripted)
  local A = ARENA
  -- The engine puts the prize down before it reports the death, so the drop
  -- is kept from the moment the man is out.
  if A.out_at ~= nil and A.died == nil and n == pill then
    A.dropped = { armour = armour, holder = holder, t = game.tick() }
    game.log(string.format("ARENA dropped armour=%d holder=%s t=%d", armour,
                           tostring(holder), game.tick()))
  end
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
      game.teleport(1, pb.x + 7, pb.y, 192)
      game.give_pill(0, pill)
      A.given = true
    end
    return
  end
  local me = game.tank(0)
  if A.out_at == nil then
    if man_out(0) then
      A.out_at = tick
      game.log(string.format("ARENA man out t=%d plan %s at %d,%d", tick,
                             plan.kind, plan.x, plan.y))
    elseif tick % 100 == 0 and me ~= nil and not me.dead then
      game.set_stocks(0, { armour = game.rule("tank_full_armour") })
      -- 7 squares by the game's own count (the larger of the two
      -- offsets), on the line the tank faces.
      local fx, fy = facing(me.dir)
      local k = 7 / math.max(math.abs(fx), math.abs(fy))
      game.teleport(1, whole(me.mx + k * fx), whole(me.my + k * fy),
                    (me.dir + 128) % 256)
    end
  elseif A.died ~= nil and tick >= A.died.t + 5 then
    local d = A.dropped
    local why = d and string.format("killer=%s dropped armour=%d holder after=%s",
                                    tostring(A.died.killer), d.armour,
                                    tostring(d.holder))
                  or "the man died but the prize was not dropped"
    verdict(d ~= nil and A.died.killer == 1 and d.armour == 0 and d.holder == nil,
            why)
  elseif A.died == nil and not man_out(0) then
    -- The man got back or put the prize down; wait for the next walk.
    A.out_at, A.dropped = nil, nil
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, "the man was never killed with the prize")
  end
end
