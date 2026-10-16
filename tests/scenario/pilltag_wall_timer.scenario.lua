-- GATE: ticks=5500 bots=4 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a person builds the prize inside closed walls, and the hunters
-- must get to it: they hit the built prize within 20 s of the build.
--
-- The ground is pilltag_wall_lines' with no gap: the 29 x 29 squares around
-- (126,126) cleared to grass, and a closed ring of walls two squares out,
-- put up again whole when the prize goes up. Player 0 plays a person: the
-- script reads him as no bot. His man builds the prize on (126,126) at tick
-- 1300, and he is then kept far off at (142,110). Players 1, 2 and 3 are
-- hunters, kept 11 squares west of the walls until the prize is up, and at
-- full armour all game.
--
-- No square has a line to the built prize, so the hunters must shoot
-- through the walls. How they get there does not matter: the script's
-- wall shot, the forced attack order, the finish run or the brain's own
-- take. Through the walls is fine and wanted.
--
-- PASS: a hunter hits the built prize within ARENA.HIT_WITHIN (20) s of the
-- build.
-- FAIL: no hunter hits it in that time, or the prize never goes up.
--
-- Note: the name is older than the check. This arena does not time the
-- script's 30 s wall-shot net (WALL_FALLBACK_SECONDS); it checks only that
-- the prize is hit within 20 s, by any route. The 30 s net has no arena of
-- its own.

ARENA = { given = false, hits = {}, lines = {}, cx = 126, cy = 126, gap = false,
          HIT_WITHIN = 20 }
scenario.callbacks.pill_damage_scale = "Arena: counts the hunters' hits on the prize."

-- The script reads player 0 as a person.
ARENA.real_is_bot = is_bot
is_bot = function(p)
  if p == 0 then return false end
  return ARENA.real_is_bot(p)
end

function pill_damage_scale(attacker, n, cause, by_pill)
  local A = ARENA
  if n == pill and attacker ~= nil and attacker >= 1 and attacker <= 3 and
     A.built ~= nil and A.phase ~= "done" then
    local w = walled.watch
    local fb = w ~= nil and w.fallback
    A.hits[#A.hits + 1] = { p = attacker, fallback = fb, t = game.tick() }
    A.lines[#A.lines + 1] = string.format(
      "ARENA p%d hit the prize t=%d +%.1fs fallback=%s", attacker, game.tick(),
      (game.tick() - A.built) / 100, tostring(fb))
  end
  return 100
end

-- The grass, then the walls and the gap. The walls go up again, whole, on
-- the frame the prize does.
function ARENA.ground(gap)
  local A = ARENA
  for y = A.cy - 14, A.cy + 14 do
    for x = A.cx - 14, A.cx + 14 do
      game.set_tile(x, y, game.TERRAIN.grass)
    end
  end
  A.walls(gap)
  for n = 1, game.num_bases() do
    local b = game.base(n)
    if b ~= nil and chebyshev(b.x, b.y, A.cx, A.cy) <= 14 then
      return false
    end
  end
  return true
end

function ARENA.walls(gap)
  local A = ARENA
  for y = A.cy - 2, A.cy + 2 do
    for x = A.cx - 2, A.cx + 2 do
      if chebyshev(x, y, A.cx, A.cy) == 2 and not (gap and x == A.cx + 2 and y == A.cy) then
        game.set_tile(x, y, game.TERRAIN.building)
      end
    end
  end
end

function ARENA.finish(ok, why)
  verdict(ok, why)
  ARENA.phase = "done"
end

ARENA.real_tick = on_tick
function on_tick(tick)
  ARENA.real_tick(tick)
  local A = ARENA
  if A.phase == "done" then return end
  for _, l in ipairs(A.lines) do game.log(l) end
  A.lines = {}
  if pill == nil then return end
  if not A.given then
    if tick >= 300 then
      if not A.ground(A.gap) then
        A.finish(false, "a base is in the arena's square")
        return
      end
      A.hx, A.hy = A.cx - 1, A.cy
      game.teleport(0, A.hx, A.hy, 64)
      for p = 1, 3 do
        game.teleport(p, A.cx - 13, A.cy - 4 + 2 * p, 64)
        game.set_stocks(p, { shells = game.rule("tank_full_shells") })
      end
      game.give_pill(0, pill)
      A.given = true
      game.log(string.format("ARENA holder at %d,%d t=%d", A.hx, A.hy, tick))
    end
    return
  end
  local me = game.tank(0)
  if me ~= nil and not me.dead and tick % 25 == 0 then
    game.set_stocks(0, { armour = game.rule("tank_full_armour") })
    local man = game.builder(0)
    local home_x, home_y = A.hx, A.hy
    if A.built ~= nil then home_x, home_y = 142, 110 end
    if chebyshev(me.mx, me.my, home_x, home_y) > 2 and
       (man == nil or man.state == "in_tank") then
      game.teleport(0, home_x, home_y, 64)
    end
    if tick % 500 == 0 then game.hint(0, { verb = "hold" }) end
  end
  -- The hunters wait on their squares west of the walls until the prize is
  -- up, so no shell of theirs has opened the walls by then.
  if tick % 25 == 0 then
    for p = 1, 3 do
      local s = game.tank(p)
      if s ~= nil and not s.dead then
        game.set_stocks(p, { armour = game.rule("tank_full_armour") })
        if A.built == nil and chebyshev(s.mx, s.my, A.cx - 13, A.cy - 4 + 2 * p) > 1 then
          game.teleport(p, A.cx - 13, A.cy - 4 + 2 * p, 64)
        end
      end
    end
  end
  if A.built == nil and tick >= 1300 and tick % 100 == 0 then
    local ok = game.builder_order(0, "pill", A.cx, A.cy)
    A.ordered = A.ordered or tick
    game.log(string.format("ARENA build ordered t=%d ok=%s", tick, tostring(ok)))
  end
  local pb = game.pill(pill)
  if A.built == nil and pb ~= nil and standing(pb) then
    A.built = tick
    A.walls(A.gap)
    game.log(string.format("ARENA prize up at %d,%d t=%d holder=%s", pb.x, pb.y, tick,
                           tostring(holder)))
  end
  if A.ordered ~= nil and A.built == nil and tick > A.ordered + 1500 then
    A.finish(false, "the prize never went up")
    return
  end
  if A.built == nil then return end
  if tick % 500 == 0 then
    local w = walled.watch
    game.log(string.format("ARENA t=%d +%.0fs fallback=%s", tick, (tick - A.built) / 100,
      tostring(w ~= nil and w.fallback)))
    for p = 1, 3 do
      local s = game.tank(p)
      game.log(s and string.format("ARENA  p%d %d,%d told=%s", p, s.mx, s.my,
        tostring(told[p])) or ("ARENA  p" .. p .. " none"))
    end
  end
  local h = A.hits[1]
  if h ~= nil and h.t <= A.built + A.HIT_WITHIN * 100 then
    A.finish(true, string.format("p%d hit the built prize at +%.1fs (fallback=%s)",
      h.p, (h.t - A.built) / 100, tostring(h.fallback)))
    return
  end
  if tick > A.built + A.HIT_WITHIN * 100 or tick >= GATE_TICKS - 50 then
    A.finish(false, string.format("no hunter hit the built prize in %d s", A.HIT_WITHIN))
  end
end
