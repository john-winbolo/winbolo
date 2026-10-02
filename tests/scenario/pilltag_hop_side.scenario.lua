-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, bot holder hop with a hunter close.
--
-- Seat 0 gets the prize at tick 300. Until he starts a hop, the arena keeps
-- his armour full (so he hops, not forts) and keeps seat 1 4 squares off his
-- left side: inside HOP_SIDE_WITHIN (5). So the hop square goes 45 degrees off
-- the line straight away from the hunter.
--
-- PASS: the hop square is BUILT_ARMOUR (+-1) squares from the tank and 30 to
-- 60 degrees off the line away from the hunter.

ARENA = { given = false }

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if not A.given then
    if tick >= 300 then
      local pb = game.pill(pill)
      game.teleport(0, pb.x, pb.y, 64)
      game.teleport(1, pb.x, pb.y - 4, 64)
      game.give_pill(0, pill)
      A.given = true
    end
    return
  end
  local me = game.tank(0)
  local h = game.tank(1)
  if A.done then return end
  if plan ~= nil then
    A.done = true
    local dx, dy = plan.x - me.mx, plan.y - me.my
    local ax, ay = me.mx - h.mx, me.my - h.my
    local len = math.sqrt(ax * ax + ay * ay)
    local dist = math.max(math.abs(dx), math.abs(dy))
    local cosang = (dx * ax + dy * ay) / (math.sqrt(dx * dx + dy * dy) * len)
    local deg = math.deg(math.acos(math.max(-1, math.min(1, cosang))))
    local hd = math.max(math.abs(ax), math.abs(ay))
    local why = string.format("%s %+d,%+d hunter at %d, %.0f deg off away",
                              plan.kind, dx, dy, hd, deg)
    game.log("ARENA " .. why)
    verdict(plan.kind == "hop" and hd <= HOP_SIDE_WITHIN and deg >= 30 and deg <= 60
            and dist >= BUILT_ARMOUR - 1 and dist <= BUILT_ARMOUR + 1, why)
    return
  end
  if tick % 100 == 0 and me ~= nil and not me.dead then
    game.set_stocks(0, { armour = game.rule("tank_full_armour") })
    -- The hunter sits off the holder's left side: facing turned a quarter
    -- anticlockwise.
    local fx, fy = facing(me.dir)
    game.teleport(1, whole(me.mx + 4 * fy), whole(me.my - 4 * fx), me.dir)
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, "no hop")
  end
end
