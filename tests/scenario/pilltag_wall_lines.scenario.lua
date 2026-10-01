-- GATE: ticks=9000 bots=4 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a person builds the prize inside walls with one gap in them,
-- and the hunters are sent to squares that have a line to it.
--
-- The arena clears the 29 x 29 squares around (126,126) to grass and puts a
-- ring of walls two squares out from (126,126), with one gap at (128,126) on
-- the east side, and puts the ring up again, whole, when the prize goes up.
-- Seat 0 plays a person: the script reads him as no bot. He
-- gets the prize at tick 300 inside the walls, and at tick 1300 his man
-- builds it on (126,126). Then he is put far off at (142,110), out of the
-- hunters' way, and kept there with full armour. Seats 1, 2 and 3 are hunters,
-- kept 11 squares west of the walls until the prize is up, and at full armour
-- all game.
--
-- The only firing squares with a line to the prize are on the east side,
-- through the gap. The script finds them and sends each hunter to one
-- (walled.update); after a short hold the brain's own pillbox fight takes over.
--
-- PASS: a hunter that was sent to a square, and got within one square of it,
-- hits the prize before the shot through the walls is turned on.
-- FAIL: the shot through the walls is turned on first, or no such hit comes
-- in the 60 s after the build.

ARENA = { given = false, hits = {}, lines = {}, arrived = {}, cx = 126, cy = 126, gap = true }
scenario.callbacks.pill_damage_scale = "Arena: counts the hunters' hits on the prize."

-- The script reads seat 0 as a person.
ARENA.real_is_bot = is_bot
is_bot = function(p)
  if p == 0 then return false end
  return ARENA.real_is_bot(p)
end

function pill_damage_scale(attacker, n, cause, by_pill)
  local A = ARENA
  if n == pill and attacker ~= nil and attacker >= 1 and attacker <= 3 and
     A.built ~= nil then
    local w = walled.watch
    local fb = w ~= nil and w.fallback
    local sent = w ~= nil and w.sent[attacker]
    A.hits[#A.hits + 1] = { p = attacker, fallback = fb, sent = sent,
                            arrived = A.arrived[attacker], t = game.tick() }
    A.lines[#A.lines + 1] = string.format(
      "ARENA p%d hit the prize t=%d fallback=%s sent=%s arrived=%s",
      attacker, game.tick(), tostring(fb), tostring(sent), tostring(A.arrived[attacker]))
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

ARENA.real_tick = on_tick
function on_tick(tick)
  ARENA.real_tick(tick)
  local A = ARENA
  for _, l in ipairs(A.lines) do game.log(l) end
  A.lines = {}
  if pill == nil then return end
  if not A.given then
    if tick >= 300 then
      if not A.ground(A.gap) then
        verdict(false, "a base is in the arena's square")
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
    verdict(false, "the prize never went up")
    return
  end
  if A.built == nil then return end
  local w = walled.watch
  for p = 1, 3 do
    local s = game.tank(p)
    local sq = w ~= nil and w.assign[p] or nil
    if sq ~= nil and s ~= nil and not s.dead and not A.arrived[p] and
       chebyshev(s.mx, s.my, sq.x, sq.y) <= 1 then
      A.arrived[p] = tick
      game.log(string.format("ARENA p%d reached its square %d,%d t=%d", p, sq.x, sq.y, tick))
    end
  end
  if tick % 500 == 0 then
    game.log(string.format("ARENA t=%d +%.0fs squares=%s fallback=%s", tick,
      (tick - A.built) / 100, w and w.squares and tostring(#w.squares) or "-",
      tostring(w ~= nil and w.fallback)))
    for p = 1, 3 do
      local s = game.tank(p)
      local sq = w ~= nil and w.assign[p] or nil
      game.log(s and string.format("ARENA  p%d %d,%d sq=%s told=%s", p, s.mx, s.my,
        sq and (sq.x .. "," .. sq.y) or "-", tostring(told[p])) or ("ARENA  p" .. p .. " none"))
    end
  end
  for _, h in ipairs(A.hits) do
    if h.fallback then
      verdict(false, string.format("p%d hit the prize only after the wall shot was on", h.p))
      return
    end
    if h.sent and h.arrived then
      verdict(true, string.format("p%d sent, reached its square at +%.1fs, hit at +%.1fs",
        h.p, (h.arrived - A.built) / 100, (h.t - A.built) / 100))
      return
    end
  end
  if w ~= nil and w.fallback then
    verdict(false, "the shot through the walls was turned on before a line hit")
    return
  end
  if tick >= A.built + 6000 or tick >= GATE_TICKS - 50 then
    verdict(false, string.format("no hit from a sent hunter in %.0f s (hits %d)",
      (tick - A.built) / 100, #A.hits))
  end
end
