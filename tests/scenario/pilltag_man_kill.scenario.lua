-- GATE: ticks=9000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a hunter chases down the holder's man and the prize drops dead.
--
-- Seat 0 gets the prize at tick 300 and is set up for a hop: the arena keeps
-- seat 1 7 squares AHEAD of him, with an empty gun so no shell of his is in
-- the air when the man steps out. The moment the man is out, the arena
--   * turns the hop square into a building, so the man turns back,
--   * moves the holder's tank 12 squares off towards the middle of the map,
--     so the man has a long walk back to it with the prize,
--   * moves the hunter 8 squares to the side of that walk and fills his gun:
--     12 or more from the holder, so outside ATTACK_WITHIN, and off the
--     man's line, so the hunter has to drive over and chase the man down.
-- From then on the arena leaves both tanks alone. The hunter runs on the
-- manhunt tuning.
--
-- PASS: seat 1 kills seat 0's man while he has the prize, at least a second
-- after he stepped out, with seat 1 on the manhunt part when he dies; the
-- prize lands dead on the ground in the same frame, and seat 0 no longer
-- holds it then. (Whoever drives over it next is a new take, seat 0
-- included.)

ARENA = { given = false }

function on_lgm_died(p, killer, mx, my, scripted)
  local A = ARENA
  if p == 0 and A.out_at ~= nil and A.died == nil then
    A.died = { killer = killer, t = game.tick(),
               part = A.dropped and A.dropped.part or tuned[1] }
    game.log(string.format("ARENA man died killer=%s t=%d part=%s",
                           tostring(killer), game.tick(), tostring(tuned[1])))
  end
end

local arena_real_placed = on_pill_placed
function on_pill_placed(n, p, armour, scripted)
  -- The part seat 1 had when the man died is read before the script hears of
  -- the drop: the script retunes every bot when the holder loses the prize,
  -- and the engine reports the drop before the death.
  local part = tuned[1]
  arena_real_placed(n, p, armour, scripted)
  local A = ARENA
  -- The engine puts the prize down before it reports the death, so the drop
  -- is kept from the moment the man is out.
  if A.out_at ~= nil and A.died == nil and n == pill then
    A.dropped = { armour = armour, holder = holder, t = game.tick(), part = part }
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
  local s = game.tank(1)
  if A.out_at == nil then
    -- The hunter's gun stays empty until the chase starts.
    if s ~= nil and not s.dead and s.shells > 0 then
      game.set_stocks(1, { shells = 0 })
    end
    if man_out(0) then
      A.out_at = tick
      game.set_tile(plan.x, plan.y, game.TERRAIN.building)
      -- The way from the hop square to the middle of the map, by the game's
      -- own count of squares.
      local ux, uy = 126 - plan.x, 126 - plan.y
      local m = math.max(math.abs(ux), math.abs(uy), 1)
      ux, uy = ux / m, uy / m
      local hx, hy = standable_near(whole(plan.x + 12 * ux),
                                    whole(plan.y + 12 * uy))
      if hx ~= nil then
        game.teleport(0, hx, hy, me.dir)
      end
      -- 8 squares to the side of that walk, from its middle.
      local cx, cy = plan.x + 6 * ux, plan.y + 6 * uy
      local qx, qy = standable_near(whole(cx - 8 * uy), whole(cy + 8 * ux))
      if qx == nil then
        qx, qy = standable_near(whole(cx + 8 * uy), whole(cy - 8 * ux))
      end
      if qx ~= nil then
        game.teleport(1, qx, qy, s.dir)
      end
      game.set_stocks(1, { shells = game.rule("tank_full_shells") })
      game.log(string.format("ARENA man out t=%d plan %s at %d,%d holder to "
                             .. "%s,%s hunter to %s,%s", tick, plan.kind,
                             plan.x, plan.y, tostring(hx), tostring(hy),
                             tostring(qx), tostring(qy)))
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
    local chased = A.died.t - A.out_at
    local why = d and string.format("killer=%s part=%s chased=%d dropped "
                                    .. "armour=%d holder after=%s",
                                    tostring(A.died.killer),
                                    tostring(A.died.part), chased, d.armour,
                                    tostring(d.holder))
                  or "the man died but the prize was not dropped"
    verdict(d ~= nil and A.died.killer == 1 and A.died.part == "manhunt" and
            chased >= 100 and d.armour == 0 and d.holder == nil, why)
  elseif A.died == nil and not man_out(0) then
    -- The man got back with it; wait for the next walk.
    game.log(string.format("ARENA man back t=%d, not caught", tick))
    A.out_at, A.dropped = nil, nil
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, "the man was never killed with the prize")
  end
end
