-- GATE: ticks=12000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the station resets.
--
-- Seat 0 plays the tutorial's player, held still.
--
-- Station 4: the arena puts the player in Station 4, hands it the dead
-- pillbox it would pick up there, knocks the live pillbox down to 3 armour,
-- gives the friendly base away and empties the tank. Then it drives the
-- player onto Station 4's reset pad.
--
-- Station 7: the arena puts the player in Station 7, waits for the Station 7
-- bot, then knocks a RESET wall down, hands one of the bot's pillboxes and
-- one of its bases to the player, and drives the player into RESET.
--
-- PASS: after each reset everything is back as the station started: the
-- dead pillbox on its square, dead and neutral; the live one neutral at full
-- armour; the base the player's and full; the RESET wall standing; the bot's
-- pillbox and base the bot's; the checklist empty; and the tank full.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

-- The checks, as a list of failures.
function arena_check4()
  local bad = {}
  local L = LAYOUT.pill.p4a
  local a = game.pill(L.n)
  if a == nil or a.in_tank or a.x ~= L.x or a.y ~= L.y or a.armour ~= 0 or
     a.owner ~= game.NEUTRAL then
    bad[#bad + 1] = "dead pill not home"
  end
  local b = game.pill(LAYOUT.pill.p4.n)
  if b == nil or b.armour ~= game.rule("pill_max_armour") or
     b.owner ~= game.NEUTRAL then
    bad[#bad + 1] = "live pill not neutral and full"
  end
  local base = game.base(LAYOUT.base.b4c.n)
  if base == nil or base.owner ~= 0 or
     base.armour ~= game.rule("base_full_armour") then
    bad[#bad + 1] = "base not the player's and full"
  end
  if next(S.done[4]) ~= nil then bad[#bad + 1] = "checklist not empty" end
  local t = game.tank(0)
  if t == nil or t.shells ~= game.rule("tank_full_shells") then
    bad[#bad + 1] = "tank not full"
  end
  return bad
end

function arena_check7()
  local bad = {}
  local w = LAYOUT.walls7[1]
  if game.map_tile(w[1], w[2]) ~= game.TERRAIN.building or
     game.wall_shots(w[1], w[2]) ~= game.rule("building_life") + 1 then
    bad[#bad + 1] = "RESET wall not standing fresh"
  end
  local pb = game.pill(LAYOUT.pill.p7_ne1.n)
  if pb == nil or pb.owner ~= S.bot7 then
    bad[#bad + 1] = "bot pill not the bot's"
  end
  local base = game.base(LAYOUT.base.b7_ne1.n)
  if base == nil or base.owner ~= S.bot7 then
    bad[#bad + 1] = "bot base not the bot's"
  end
  if next(S.done[7]) ~= nil then bad[#bad + 1] = "checklist not empty" end
  return bad
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.phase == 0 and tick >= 300 then
    game.teleport(0, 126, 140, 0)          -- Station 4, on the road
    A.phase, A.t = 1, tick
  elseif A.phase == 1 and tick >= A.t + 100 then
    game.give_pill(0, LAYOUT.pill.p4a.n)
    game.set_pill_armour(LAYOUT.pill.p4.n, 3)
    game.set_base_owner(LAYOUT.base.b4c.n)
    game.set_stocks(0, { shells = 2 })
    S.done[4].a_pick = true
    A.phase, A.t = 2, tick
  elseif A.phase == 2 and tick >= A.t + 100 then
    local r = LAYOUT.region.pad4
    game.teleport(0, r.x, r.y, 0)
    A.phase, A.t = 3, tick
  elseif A.phase == 3 and tick >= A.t + 200 then
    local bad = arena_check4()
    if #bad > 0 then
      verdict(false, "Station 4: " .. table.concat(bad, ", "))
      return
    end
    local a = LAYOUT.point.s7_arrive
    game.teleport(0, a[1], a[2], 0)
    A.phase, A.t = 4, tick
  elseif A.phase == 4 then
    local t = S.bot7 and game.tank(S.bot7)
    if t ~= nil and not t.dead and tick >= A.t + 300 then
      local w = LAYOUT.walls7[1]
      game.set_tile(w[1], w[2], game.TERRAIN.rubble)
      game.set_pill_owner(LAYOUT.pill.p7_ne1.n, 0)
      game.set_base_owner(LAYOUT.base.b7_ne1.n, 0)
      S.done[7].base = true
      A.phase, A.t = 5, tick
    elseif tick >= A.t + 3000 then
      verdict(false, "the Station 7 bot never took the field")
    end
  elseif A.phase == 5 and tick >= A.t + 100 then
    local r = LAYOUT.region.reset7
    game.teleport(0, r.x + 1, r.y, 0)
    A.phase, A.t = 6, tick
  elseif A.phase == 6 and tick >= A.t + 200 then
    local bad = arena_check7()
    verdict(#bad == 0, #bad == 0 and "Stations 4 and 7 reset in full"
            or ("Station 7: " .. table.concat(bad, ", ")))
  end
end
