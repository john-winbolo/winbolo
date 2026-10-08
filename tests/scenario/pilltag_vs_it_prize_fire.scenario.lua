-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, Everyone vs It: a built prize fires at the hunters and not at
-- its holder.
--
-- Seat 0 gets the prize at tick 300. Until he builds, the arena holds his
-- armour at 15 (so he builds a fort, not a hop, as in
-- pilltag_shoot_standing) and keeps seat 1 7 squares behind him. When the
-- fort stands, the arena keeps seat 0, its guard, two squares from it and
-- seat 1, the hunter, four squares from it on the other side, both inside
-- the fort's range and at full armour, and seat 1's gun empty so the fort
-- stays up.
--
-- PASS: in the 8 s after the fort goes up, the fort hits seat 1 at least
-- once and never hits seat 0.

ARENA = { given = false, on = { [0] = 0, [1] = 0 } }
scenario.callbacks.on_tank_hit = "Arena: counts the prize's hits on each seat."

function on_tank_hit(victim, attacker, cause, amount, n, scripted)
  local A = ARENA
  if A.built ~= nil and n == pill and A.on[victim] ~= nil then
    A.on[victim] = A.on[victim] + 1
  end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if tick >= GATE_TICKS - 100 then
    verdict(false, A.built == nil and "no fort" or "fort did not last 8 s")
  end
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
  if A.built == nil then
    if plan == nil and tick % 100 == 0 and me ~= nil and not me.dead then
      game.set_stocks(0, { armour = 15 })
      local fx, fy = facing(me.dir)
      local k = 7 / math.max(math.abs(fx), math.abs(fy))
      game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
    end
    local pb = game.pill(pill)
    if plan ~= nil and plan.built and standing(pb) then
      if plan.kind ~= "fort" then
        verdict(false, "built a " .. plan.kind .. " with armour 15")
        return
      end
      A.built, A.px, A.py = tick, pb.x, pb.y
      game.log(string.format("ARENA fort at %d,%d t=%d armour %d; seat 0 team %s, seat 1 team %s",
                             pb.x, pb.y, tick, pb.armour,
                             tostring(game.lobby_slot(0).team),
                             tostring(game.lobby_slot(1).team)))
    end
    return
  end
  if tick % 25 == 0 then
    local full = game.rule("tank_full_armour")
    local man = game.builder(0)
    local x0, y0 = standable_near(A.px + 2, A.py)
    local x1, y1 = standable_near(A.px - 4, A.py)
    if x0 ~= nil and man ~= nil and man.state == "in_tank" then
      game.teleport(0, x0, y0, 64)
    end
    if x1 ~= nil then game.teleport(1, x1, y1, 0) end
    for p = 0, 1 do
      local t = game.tank(p)
      if t ~= nil and not t.dead then
        game.set_stocks(p, { armour = full })
      end
    end
    game.set_stocks(1, { shells = 0 })
  end
  local pb = game.pill(pill)
  if not standing(pb) or holder ~= 0 then
    verdict(false, string.format("the fort fell or changed hands at t=%d (holder %s)",
                                 tick, tostring(holder)))
    return
  end
  if tick >= A.built + 800 then
    game.log(string.format("ARENA fort hits: seat 0 %d, seat 1 %d", A.on[0], A.on[1]))
    verdict(A.on[1] > 0 and A.on[0] == 0,
            string.format("fort hit the hunter %d times, its holder %d times",
                          A.on[1], A.on[0]))
  end
end
