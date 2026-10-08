-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, the hunters rush a built prize (RUSH), and stop when it
-- falls.
--
-- The set-up is pilltag_unguarded's. Seat 0 gets the prize at tick 300 and
-- hops (his armour kept full, seat 1 kept 7 squares behind him). When the
-- hop stands and his man is back in the tank, seat 0 is put 25 squares off
-- and kept there, out of the fight, and seat 1, the hunter, is put 10
-- squares from the prize, gun full, and left to the brain.
--
-- Check 1: within 2 s of the build, the numbers this script wrote into seat
-- 1's brain (have[1]) are the rush ones: ATTACK_PILL_STANDOFF at
-- RUSH.STANDOFF, TAKE_COVER_W_EXPO and ANGER_COST_PER_TICK at RUSH.DANGER
-- of DEFAULTS, ENGAGE_MAX_INCOMING_TICKS at DEFAULTS over RUSH.DANGER,
-- ATTACK_PILL_TANK_PRESENT_PENALTY at 0 and TANK_COMBAT_BASE_COST at
-- RUSH.TANK_COST.
-- Check 2: the prize falls, by the hunter's gun, or by the arena at 20 s
-- after the build if it still stands then. Within 2 s of the fall, every one
-- of those six numbers is back at DEFAULTS, except TANK_COMBAT_BASE_COST,
-- which is back at the part's own value (10 for a hunter; a holder does not
-- set it).
--
-- The hunter's first hit and nearest approach are logged, not judged.
--
-- PASS: both checks hold.

ARENA = { step = 0, near = 999 }
scenario.callbacks.pill_damage_scale = "Arena: notes the hunter's first hit on the prize."

function pill_damage_scale(attacker, n, cause, by_pill)
  local A = ARENA
  if n == pill and attacker == 1 and A.built ~= nil and A.hit == nil then
    A.hit = game.tick()
  end
  return 100
end

function ARENA.want(rush)
  local d = RUSH.DANGER
  if rush then
    return {
      ATTACK_PILL_STANDOFF             = RUSH.STANDOFF,
      TAKE_COVER_W_EXPO                = DEFAULTS.TAKE_COVER_W_EXPO * d,
      ANGER_COST_PER_TICK              = DEFAULTS.ANGER_COST_PER_TICK * d,
      ENGAGE_MAX_INCOMING_TICKS        =
        math.floor(DEFAULTS.ENGAGE_MAX_INCOMING_TICKS / d + 0.5),
      ATTACK_PILL_TANK_PRESENT_PENALTY = 0,
      TANK_COMBAT_BASE_COST            = RUSH.TANK_COST,
    }
  end
  local role = ROLES[role_of(1)].cfg
  return {
    ATTACK_PILL_STANDOFF             = DEFAULTS.ATTACK_PILL_STANDOFF,
    TAKE_COVER_W_EXPO                = DEFAULTS.TAKE_COVER_W_EXPO,
    ANGER_COST_PER_TICK              = DEFAULTS.ANGER_COST_PER_TICK,
    ENGAGE_MAX_INCOMING_TICKS        = DEFAULTS.ENGAGE_MAX_INCOMING_TICKS,
    ATTACK_PILL_TANK_PRESENT_PENALTY = DEFAULTS.ATTACK_PILL_TANK_PRESENT_PENALTY,
    TANK_COMBAT_BASE_COST            = role.TANK_COMBAT_BASE_COST or
                                       DEFAULTS.TANK_COMBAT_BASE_COST,
  }
end

-- The knobs that are not yet where they should be, as text; nil when all are.
function ARENA.off(rush)
  local now, bad = have[1] or {}, {}
  local want = ARENA.want(rush)
  local names = {}
  for k in pairs(want) do names[#names + 1] = k end
  table.sort(names)
  for _, k in ipairs(names) do
    local v = now[k]
    if v == nil then v = DEFAULTS[k] end
    if v ~= want[k] then
      bad[#bad + 1] = k .. "=" .. tostring(v) .. " want " .. tostring(want[k])
    end
  end
  return (#bad > 0) and table.concat(bad, "; ") or nil
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if tick >= GATE_TICKS - 100 then
    verdict(false, "stuck at step " .. A.step)
  end
  local pb = game.pill(pill)
  if A.step == 0 and tick >= 300 then
    game.teleport(0, pb.x, pb.y, 64)
    game.teleport(1, pb.x - 7, pb.y, 64)
    game.give_pill(0, pill)
    A.step = 2
  elseif A.step == 2 then
    local me = game.tank(0)
    if plan == nil then
      if tick % 100 == 0 and me ~= nil and not me.dead then
        game.set_stocks(0, { armour = game.rule("tank_full_armour") })
        local fx, fy = facing(me.dir)
        local k = 7 / math.max(math.abs(fx), math.abs(fy))
        game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
      end
      return
    end
    local man = game.builder(0)
    if not standing(pb) or holder ~= 0 or man == nil or
       man.state ~= "in_tank" then
      return
    end
    A.built, A.px, A.py = tick, pb.x, pb.y
    local dx, dy = sign(126 - pb.x), sign(126 - pb.y)
    if dx == 0 then dx = 1 end
    if dy == 0 then dy = 1 end
    A.hx, A.hy = standable_near(pb.x + 25 * dx, pb.y + 25 * dy)
    local wx, wy = standable_near(pb.x + 10 * dx, pb.y)
    if A.hx == nil or wx == nil then
      verdict(false, "no square for the holder or the hunter")
      return
    end
    game.teleport(0, A.hx, A.hy, 64)
    game.teleport(1, wx, wy, 64)
    game.set_stocks(1, { shells = game.rule("tank_full_shells") })
    game.log(string.format("ARENA prize stands at %d,%d t=%d; hunter at %d,%d",
                           pb.x, pb.y, tick, wx, wy))
    A.step = 3
  elseif A.step == 3 then
    local me = game.tank(0)
    if me ~= nil and not me.dead and tick % 25 == 0 and
       chebyshev(me.mx, me.my, A.hx, A.hy) > 1 then
      game.teleport(0, A.hx, A.hy, 64)
    end
    local s = game.tank(1)
    if s ~= nil and not s.dead then
      A.near = math.min(A.near, chebyshev(s.mx, s.my, A.px, A.py))
    end
    if A.rushed == nil and A.off(true) == nil then
      A.rushed = tick
      game.log(string.format("ARENA rush table in seat 1's brain %.2fs after the build",
                             (tick - A.built) / 100))
    end
    if A.rushed == nil and tick >= A.built + 200 then
      verdict(false, "no rush: " .. A.off(true))
      return
    end
    if not standing(pb) then
      A.fell, A.step = tick, 4
      game.log(string.format("ARENA prize fell t=%d (%s); hunter's first hit %s, nearest %d squares",
                             tick, A.forced and "by the arena" or "shot down",
                             A.hit and string.format("%.1fs", (A.hit - A.built) / 100) or "none",
                             A.near))
      return
    end
    if not A.forced and tick >= A.built + 2000 then
      A.forced = true
      game.set_pill_armour(pill, 0)
    end
  elseif A.step == 4 then
    local off = A.off(false)
    if off == nil then
      verdict(true, string.format("rush on %.2fs after the build, off %.2fs after the fall (%s)",
                                  (A.rushed - A.built) / 100, (tick - A.fell) / 100,
                                  A.forced and "arena" or "shot down"))
    elseif tick >= A.fell + 200 then
      verdict(false, "rush still on: " .. off)
    end
  end
end
