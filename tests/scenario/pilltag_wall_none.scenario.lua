-- GATE: ticks=9000 bots=4 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a person builds the prize inside closed walls, no firing
-- square has a line to it, and the hunters are turned at once to shoot
-- through the walls.
--
-- The ground is pilltag_wall_lines' with no gap: the 29 x 29 squares around
-- (126,126) cleared to grass, and a closed ring of walls two squares out,
-- put up again whole when the prize goes up. Seat 0 plays a person: the
-- script reads him as no bot. His man builds the prize on (126,126) at tick
-- 1300, and he is then kept far off at (142,110). Seats 1, 2 and 3 are
-- hunters, kept 11 squares west of the walls until the prize is up, and at
-- full armour all game.
--
-- PASS: no hunter hits the prize before the wall shot; the wall shot comes on
-- for "no square has a line" within 3 s of the build; and a hunter hits the
-- prize in the 60 s after that.

-- Played as a Free For All (TEAMS.vs_it = false, as the "Teams" setting
-- would), the round this arena was written for. Seat 0 stands in for a
-- person, so the script hands its brain no table and it plays the ordinary
-- game. In Everyone vs It the three hunters are allies, that brain counts
-- itself outnumbered, and it drops the prize at once (the brain's
-- emergency drop), long before the arena's build.
TEAMS.vs_it = false
ARENA = { given = false, hits = {}, lines = {}, arrived = {}, cx = 126, cy = 126, gap = false }
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

-- The arena notes when, and why, the script turns the wall shot on.
ARENA.real_fallback_on = walled.fallback_on
walled.fallback_on = function(why)
  local A = ARENA
  if A.fb_t == nil and A.built ~= nil then
    A.fb_t, A.fb_why, A.fb_elapsed = game.tick(), why, elapsed
    A.fb_sent_at = walled.watch and walled.watch.sent_at
    A.lines[#A.lines + 1] = string.format("ARENA wall shot on t=%d (%s) sent_at=%s now=%d",
      game.tick(), why, tostring(A.fb_sent_at), elapsed)
  end
  return A.real_fallback_on(why)
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
  if A.fb_t == nil then
    if #A.hits > 0 then
      verdict(false, string.format("p%d hit the prize through closed walls before the wall shot",
        A.hits[1].p))
      return
    end
    if tick >= A.built + 400 or tick >= GATE_TICKS - 50 then
      verdict(false, "the shot through the walls was never turned on")
    end
    return
  end
  if A.fb_why:find("no square has a line", 1, true) == nil then
    verdict(false, "the wall shot came on for another reason: " .. A.fb_why)
    return
  end
  if A.fb_t - A.built > 300 then
    verdict(false, string.format("the wall shot came on %.1f s after the build",
      (A.fb_t - A.built) / 100))
    return
  end
  for _, h in ipairs(A.hits) do
    if h.fallback then
      verdict(true, string.format("wall shot on at +%.1fs (%s), p%d hit the prize at +%.1fs",
        (A.fb_t - A.built) / 100, A.fb_why, h.p, (h.t - A.built) / 100))
      return
    end
  end
  if tick >= A.fb_t + 6000 or tick >= GATE_TICKS - 50 then
    verdict(false, string.format("no hit on the prize in %.0f s after the wall shot came on",
      (tick - A.fb_t) / 100))
  end
end
