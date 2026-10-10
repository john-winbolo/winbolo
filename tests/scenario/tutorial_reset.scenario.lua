-- GATE: ticks=16000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, re-arming and the RESET pens.
--
-- Seat 0 plays the tutorial's player, held still.
--
-- Station 4 (re-arming, no pen): the arena puts the player in Station 4,
-- moves the dead pillbox off its square and gives it to the player's side,
-- knocks the damaged pillbox up to 3 armour and gives the friendly base
-- away. Within two seconds the station must put each of them back by
-- itself, while none of those goals is ticked.
--
-- Station 5 (the 5B RESET pen): the arena puts the player in Station 5,
-- knocks down a RESET wall, breaks the arrow road, damages the 5B target,
-- takes the 5B blocker out of the player's tank and ticks a goal. Then it
-- puts the player in the pen.
--
-- Station 7 (the RESET pen): the arena puts the player in Station 7, waits
-- for the Station 7 bot, knocks down a RESET wall, breaks the arrow road,
-- gives one of the player's pillboxes to the bot and one of the bot's bases
-- to the player, and ticks a goal. Then it puts the player in the pen.
--
-- PASS: Station 4 re-arms in full; after each pen everything in that
-- station is back as it started: the walls standing, the arrow road whole,
-- the pillboxes and bases with their first owners, the checklist empty (but
-- for 5A, which is not undone), and for Station 5 the blocker back in the
-- player's tank.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local function walls_ok(list, bad, what)
  for _, w in ipairs(list) do
    if game.map_tile(w[1], w[2]) ~= game.TERRAIN.building then
      bad[#bad + 1] = string.format("%s wall %d,%d down", what, w[1], w[2])
      return
    end
  end
end

local function road_ok(list, bad, what)
  for _, w in ipairs(list) do
    if game.map_tile(w[1], w[2]) ~= game.TERRAIN.road then
      bad[#bad + 1] = string.format("%s arrow %d,%d not road", what, w[1], w[2])
      return
    end
  end
end

local function done_keys(n)
  local k = {}
  for key in pairs(S.done[n]) do k[#k + 1] = key end
  table.sort(k)
  return table.concat(k, " ")
end

function arena_check4()
  local bad = {}
  local L = LAYOUT.pill.p4a
  local a = game.pill(L.n)
  if a == nil or a.in_tank or a.x ~= L.x or a.y ~= L.y or a.armour ~= 0 or
     a.owner ~= game.NEUTRAL then
    bad[#bad + 1] = "dead pill not home, dead and neutral"
  end
  L = LAYOUT.pill.p4
  local b = game.pill(L.n)
  if b == nil or b.in_tank or b.x ~= L.x or b.y ~= L.y or b.armour ~= 1 or
     b.owner ~= game.NEUTRAL then
    bad[#bad + 1] = "damaged pill not home at 1 armour"
  end
  local base = game.base(LAYOUT.base.b4c.n)
  if base == nil or base.owner ~= 0 or
     base.armour ~= game.rule("base_full_armour") then
    bad[#bad + 1] = "base not the player's and full"
  end
  return bad
end

function arena_check5()
  local bad = {}
  walls_ok(LAYOUT.walls5b, bad, "5B RESET")
  road_ok(LAYOUT.arrow_reset5b, bad, "5B")
  local L = LAYOUT.pill.t5b_target
  local tg = game.pill(L.n)
  if tg == nil or tg.in_tank or tg.x ~= L.x or tg.y ~= L.y or
     tg.owner ~= S.bot6 or tg.armour ~= game.rule("pill_max_armour") then
    bad[#bad + 1] = "5B target not home, full and the Station 6 bot's"
  end
  local bl = game.pill(LAYOUT.pill.t5b_blocker.n)
  if bl == nil or not bl.in_tank or S.carrier[LAYOUT.pill.t5b_blocker.n] ~= 0 then
    bad[#bad + 1] = "blocker not in the player's tank"
  end
  local d = S.done[5]
  if d.b_build or d.b_park or d.b_kill or d.b_pick then
    bad[#bad + 1] = "5B checklist not empty (" .. done_keys(5) .. ")"
  end
  return bad
end

function arena_check7()
  local bad = {}
  walls_ok(LAYOUT.walls7, bad, "7 RESET")
  road_ok(LAYOUT.arrow_reset7, bad, "7")
  local pb = game.pill(LAYOUT.pill.p7_nw1.n)
  if pb == nil or pb.owner ~= 0 then
    bad[#bad + 1] = "player pill not the player's"
  end
  local base = game.base(LAYOUT.base.b7_ne1.n)
  if base == nil or base.owner ~= S.bot7 then
    bad[#bad + 1] = "bot base not the bot's"
  end
  if next(S.done[7]) ~= nil then
    bad[#bad + 1] = "checklist not empty (" .. done_keys(7) .. ")"
  end
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
    game.move_pill(LAYOUT.pill.p4a.n, 121, 150)
    game.set_pill_owner(LAYOUT.pill.p4a.n, 0)
    game.set_pill_armour(LAYOUT.pill.p4.n, 3)
    game.set_base_owner(LAYOUT.base.b4c.n)
    A.phase, A.t = 2, tick
  elseif A.phase == 2 and tick >= A.t + 150 then
    local bad = arena_check4()
    if #bad > 0 then
      verdict(false, "Station 4: " .. table.concat(bad, ", "))
      return
    end
    game.log("ARENA Station 4 re-armed")
    game.teleport(0, 126, 110, 0)          -- Station 5, on the road
    A.phase, A.t = 3, tick
  elseif A.phase == 3 and tick >= A.t + 200 then
    local w = LAYOUT.walls5b[1]
    game.set_tile(w[1], w[2], game.TERRAIN.rubble)
    local r = LAYOUT.arrow_reset5b[3]
    game.set_tile(r[1], r[2], game.TERRAIN.crater)
    game.set_pill_armour(LAYOUT.pill.t5b_target.n, 3)
    local bn = LAYOUT.pill.t5b_blocker.n
    local B = LAYOUT.pill.t5b_blocker
    if not game.drop_pill(0, bn, B.x, B.y) then
      verdict(false, "the player did not carry the 5B blocker on entry")
      return
    end
    S.done[5].b_build = true
    A.phase, A.t = 4, tick
  elseif A.phase == 4 and tick >= A.t + 100 then
    local r = LAYOUT.region.reset5b
    game.teleport(0, r.x + 1, r.y, 0)
    A.phase, A.t = 5, tick
  elseif A.phase == 5 and tick >= A.t + 200 then
    local bad = arena_check5()
    if #bad > 0 then
      verdict(false, "Station 5: " .. table.concat(bad, ", "))
      return
    end
    game.log("ARENA Station 5 pen reset")
    local a = LAYOUT.point.s7_arrive
    game.teleport(0, a[1], a[2], 0)
    A.phase, A.t = 6, tick
  elseif A.phase == 6 then
    local t = S.bot7 and game.tank(S.bot7)
    if t ~= nil and not t.dead and tick >= A.t + 300 then
      local w = LAYOUT.walls7[1]
      game.set_tile(w[1], w[2], game.TERRAIN.rubble)
      local r = LAYOUT.arrow_reset7[3]
      game.set_tile(r[1], r[2], game.TERRAIN.crater)
      game.set_pill_owner(LAYOUT.pill.p7_nw1.n, S.bot7)
      game.set_base_owner(LAYOUT.base.b7_ne1.n, 0)
      S.done[7].base = true
      A.phase, A.t = 7, tick
    elseif tick >= A.t + 3000 then
      verdict(false, "the Station 7 bot never took the field")
    end
  elseif A.phase == 7 and tick >= A.t + 100 then
    local r = LAYOUT.region.reset7
    game.teleport(0, r.x + 1, r.y, 0)
    A.phase, A.t = 8, tick
  elseif A.phase == 8 and tick >= A.t + 200 then
    local bad = arena_check7()
    verdict(#bad == 0, #bad == 0 and
            "Station 4 re-armed; Station 5 and 7 pens reset in full"
            or ("Station 7: " .. table.concat(bad, ", ")))
  end
end
