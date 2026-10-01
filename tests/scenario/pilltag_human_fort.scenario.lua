-- GATE: ticks=8000 bots=4 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag on Everard Island: a holder the script does not drive builds the
-- prize, and the hunters must go after it.
--
-- Seat 0 plays a person: the arena makes the script read him as no bot, so
-- he gets no holder part and no orders, the way a human holder does not. He
-- gets the prize at tick 300 and is kept on one square with full armour.
-- Seats 1, 2 and 3 are hunters, put 5, 10 and 15 squares east of him. At tick
-- 1300 his man builds the prize on the square beside him, and he stays there
-- by his fort.
--
-- The hunters' armour is kept full until the build, and when the fort is up
-- each is put back on its square, still holding the order the script gave it
-- during the chase.
--
-- PASS: no hunter is frozen on the holder. In the 20 s after the build no
-- hunter spends more than 8 s in a row alive, within 8 squares of the holder,
-- within 2 squares of one spot and with no hit on the fort or the holder.
-- Every hunter's square is logged once a second.
--
-- The holder is put in the walled middle of the island, the way a person
-- builds a fort among walls. With the script as it was, the order to attack
-- the holder that each hunter held from the chase kept it sitting 2 to 7
-- squares from him for 10 to 15 s, unable to shoot through the walls, while
-- the fort shot it.

ARENA = { given = false, hit = {}, lines = {}, last = {}, anchor = {}, busy = {}, still = {} }
scenario.callbacks.on_tank_hit = "Arena: counts the hunters' hits on the holder."
scenario.callbacks.pill_damage_scale = "Arena: counts the hunters' hits on the fort."

-- The script reads seat 0 as a person.
local arena_real_is_bot = is_bot
is_bot = function(p)
  if p == 0 then return false end
  return arena_real_is_bot(p)
end

function on_tank_hit(victim, attacker, cause, amount, n, scripted)
  local A = ARENA
  if victim == 0 and attacker ~= nil and attacker >= 1 and attacker <= 3 and
     A.built ~= nil then
    A.busy[attacker] = game.tick()
    if A.hit[attacker] == nil then
      A.hit[attacker] = game.tick()
      A.lines[#A.lines + 1] = string.format("ARENA p%d hit the holder t=%d", attacker, game.tick())
    end
  end
end

function pill_damage_scale(attacker, n, cause, by_pill)
  local A = ARENA
  if n == pill and attacker ~= nil and attacker >= 1 and attacker <= 3 and
     A.built ~= nil then
    A.busy[attacker] = game.tick()
    if A.hit[attacker] == nil then A.hit[attacker] = game.tick() end
  end
  return 100
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
      A.hx, A.hy = standable_near(126, 126)
      if A.hx == nil then
        verdict(false, "no square for the holder")
        return
      end
      game.teleport(0, A.hx, A.hy, 64)
      for p = 1, 3 do
        local x, y = standable_near(A.hx + 5 * p, A.hy)
        if x == nil then x, y = standable_near(A.hx - 6 * p, A.hy) end
        game.teleport(p, x, y, 192)
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
    if chebyshev(me.mx, me.my, A.hx, A.hy) > 2 then
      game.teleport(0, A.hx, A.hy, 64)
    end
    if tick % 500 == 0 then game.hint(0, { verb = "hold" }) end
  end
  if A.built == nil and tick >= 1300 and tick % 100 == 0 then
    local bx, by = standable_near(A.hx, A.hy - 1)
    if bx == nil or (bx == A.hx and by == A.hy) then bx, by = standable_near(A.hx + 1, A.hy) end
    local man = game.builder(0)
    local ok = game.builder_order(0, "pill", bx, by)
    A.ordered = A.ordered or tick
    game.log(string.format("ARENA build ordered at %d,%d t=%d ok=%s man=%s", bx, by, tick,
      tostring(ok), man and tostring(man.state) or "none"))
  end
  local pb = game.pill(pill)
  if A.built == nil and tick % 25 == 0 then
    for p = 1, 3 do
      local s = game.tank(p)
      if s ~= nil and not s.dead then
        game.set_stocks(p, { armour = game.rule("tank_full_armour") })
      end
    end
  end
  if A.built == nil and pb ~= nil and standing(pb) then
    A.built = tick
    -- Each hunter goes back to its square east of the holder, order and all.
    for p = 1, 3 do
      local x, y = standable_near(A.hx + 5 * p, A.hy)
      if x ~= nil then
        game.teleport(p, x, y, 192)
        A.anchor[p] = { x, y }
        A.busy[p] = tick
      end
    end
    game.log(string.format("ARENA fort up at %d,%d t=%d holder=%s", pb.x, pb.y, tick, tostring(holder)))
  end
  if A.ordered ~= nil and A.built == nil and tick > A.ordered + 1500 then
    verdict(false, "the fort never went up")
    return
  end
  if A.built == nil then return end
  if tick % 100 == 0 then
    local parts = {}
    for p = 1, 3 do
      local s = game.tank(p)
      parts[#parts + 1] = s and string.format("p%d %d,%d a%d s%d %s told=%s", p, s.mx, s.my,
        s.armour, s.shells, A.hit[p] and "HIT" or "-", tostring(told[p])) or ("p" .. p .. " none")
    end
    game.log(string.format("ARENA t=%d +%.0fs %s", tick, (tick - A.built) / 100,
                           table.concat(parts, " | ")))
  end
  -- A hunter is busy while it moves (2 squares from where it last stood),
  -- hits the fort or the holder, is dead, or is more than NEAR squares from
  -- the holder. The longest time each hunter spends not busy is its still
  -- time: sitting by the holder and his fort without a shot.
  local NEAR, STILL_MAX = 8, 800
  for p = 1, 3 do
    local s = game.tank(p)
    if s == nil or s.dead or s.mx == 0 then
      A.busy[p], A.anchor[p] = tick, nil
    elseif chebyshev(s.mx, s.my, A.hx, A.hy) > NEAR then
      A.busy[p], A.anchor[p] = tick, { s.mx, s.my }
    elseif A.anchor[p] == nil or
           chebyshev(s.mx, s.my, A.anchor[p][1], A.anchor[p][2]) >= 2 then
      A.busy[p], A.anchor[p] = tick, { s.mx, s.my }
    end
    local still = tick - (A.busy[p] or tick)
    if still > (A.still[p] or 0) then A.still[p] = still end
  end
  if not GATE_SAID and (tick >= A.built + 2000 or tick >= GATE_TICKS - 50) then
    local stuck, parts = {}, {}
    for p = 1, 3 do
      if (A.still[p] or 0) > STILL_MAX then stuck[#stuck + 1] = "p" .. p end
      parts[#parts + 1] = string.format("p%d still %.1fs %s", p, (A.still[p] or 0) / 100,
        A.hit[p] and string.format("first hit +%.1fs", (A.hit[p] - A.built) / 100) or "no hit")
    end
    game.log("ARENA " .. table.concat(parts, ", "))
    verdict(#stuck == 0, (#stuck == 0 and "no hunter frozen" or
      ("frozen over 8 s: " .. table.concat(stuck, ", "))) ..
      " in the 20 s after the build: " .. table.concat(parts, ", "))
  end
end
