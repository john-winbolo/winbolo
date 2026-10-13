-- GATE: ticks=6000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, Station 5A: the watch spot is told, not forced, and a 5A demo
-- reset puts the island's wall rings and moat back.
--
-- Seat 0 plays the tutorial's player, held still (put back on its square
-- every tenth of a second). At tick 300 the arena parks it on the main road
-- 7 squares (the larger of the x and y distances) from the watch spot, for
-- 15 seconds; then 4 squares from it, off the watch road.
--
-- Then, with the demo bot fielded, the arena knocks down walls of both
-- rings (rubble, crater, grass) and fills moat squares (grass, road), and
-- runs the script's 5A reset.
--
-- Last, the frame's write count: the arena breaks every ring and moat
-- square, and on the next frame spends 200 of the frame's 256 writes before
-- it runs the script's ring rebuild, so the rebuild is refused part way.
--
-- PASS:
--   - 7 squares off for 15 seconds: "Watch the bot take one" is not ticked
--     and no timer started;
--   - 4 squares off: it ticks 9 to 11 seconds after the tank got there;
--   - after the reset every LAYOUT.walls5a square is a full wall again and
--     every LAYOUT.moat5a square is deep water again;
--   - the rebuild that was refused part way leaves squares broken in that
--     frame, notes that it is not finished, and has every square back
--     three frames later with no help from the arena.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local FAR = { 127, 104 }    -- 4 west, 7 north of watch5a (131,111)
local NEAR = { 127, 111 }   -- 4 west of watch5a, on the main road

local function hold(at)
  local me = game.tank(0)
  if me ~= nil and not me.dead and (me.mx ~= at[1] or me.my ~= at[2]) then
    game.teleport(0, at[1], at[2], 0)
  end
end

local function rings_ok()
  local bad = {}
  local full = game.rule("building_life") + 1
  for _, w in ipairs(LAYOUT.walls5a) do
    if game.map_tile(w[1], w[2]) ~= game.TERRAIN.building or
       game.wall_shots(w[1], w[2]) ~= full then
      bad[#bad + 1] = w[1] .. "," .. w[2]
    end
  end
  for _, w in ipairs(LAYOUT.moat5a) do
    if game.map_tile(w[1], w[2]) ~= game.TERRAIN.deep_sea then
      bad[#bad + 1] = w[1] .. "," .. w[2]
    end
  end
  return bad
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.done then return end
  local function fail(msg) verdict(false, msg); A.done = true end
  if A.phase == 0 and tick >= 300 then
    game.teleport(0, FAR[1], FAR[2], 0)
    A.phase, A.t = 1, tick
  elseif A.phase == 1 then
    if tick % 10 == 0 then hold(FAR) end
    if S.done[5].a_watch or S.near5a_at ~= nil then
      return fail("ticked or timed 7 squares off, at "
                  .. (tick - A.t) / 100 .. " s")
    end
    if tick >= A.t + 1500 then
      game.teleport(0, NEAR[1], NEAR[2], 0)
      A.phase, A.t = 2, tick
    end
  elseif A.phase == 2 then
    if tick % 10 == 0 then hold(NEAR) end
    if S.done[5].a_watch then
      A.near_s = (tick - A.t) / 100
      if A.near_s < 9 or A.near_s > 11 then
        return fail(string.format("4 squares off: ticked after %.2f s",
                                  A.near_s))
      end
      A.phase, A.t = 3, tick
    elseif tick >= A.t + 1500 then
      return fail("4 squares off: not ticked after 15 s")
    end
  elseif A.phase == 3 and tick >= A.t + 50 then
    if live_tank(S.bot5a) == nil then
      if tick >= A.t + 1000 then return fail("5A bot never fielded") end
      return
    end
    -- Knock down walls of both rings and fill some moat.
    local T = game.TERRAIN
    local hits = {
      { 132, 101, T.rubble }, { 143, 110, T.crater }, { 137, 120, T.grass },
      { 134, 103, T.rubble }, { 141, 112, T.grass }, { 136, 118, T.rubble },
      { 133, 105, T.grass }, { 142, 119, T.road }, { 138, 102, T.grass },
    }
    for _, h in ipairs(hits) do game.set_tile(h[1], h[2], h[3]) end
    A.broken = #rings_ok()
    A.phase, A.t = 4, tick
  elseif A.phase == 4 and tick >= A.t + 10 then
    reset_5a()
    A.phase, A.t = 5, tick
  elseif A.phase == 5 and tick >= A.t + 10 then
    local bad = rings_ok()
    if A.broken ~= 9 or #bad ~= 0 then
      return fail(string.format("rings broken %d, after reset bad %d (%s)",
                                A.broken, #bad, table.concat(bad, " ")))
    end
    -- Break every square of the rings and the moat.
    for _, w in ipairs(LAYOUT.walls5a) do
      game.set_tile(w[1], w[2], game.TERRAIN.rubble)
    end
    for _, w in ipairs(LAYOUT.moat5a) do
      game.set_tile(w[1], w[2], game.TERRAIN.grass)
    end
    A.all = #LAYOUT.walls5a + #LAYOUT.moat5a
    A.broken2 = #rings_ok()
    A.phase, A.t = 6, tick
  elseif A.phase == 6 then
    -- The next frame. Spend 200 of the frame's writes (a square written
    -- with the terrain it has still costs one), then run the rebuild.
    local far = FAR
    local tile = game.map_tile(far[1], far[2])
    for _ = 1, 200 do game.set_tile(far[1], far[2], tile) end
    rebuild_5a_rings()
    A.left = #rings_ok()
    A.again = S.rings5a_again
    A.phase, A.t = 7, tick
  elseif A.phase == 7 and tick >= A.t + 6 then
    local bad = rings_ok()
    verdict(A.broken2 == A.all and A.left > 0 and A.left < A.all and
            A.again == true and #bad == 0 and S.rings5a_again == false,
      string.format(
      "7 off: no tick in 15 s; 4 off: ticked after %.2f s; rings back after "
      .. "reset; all %d broken (%d), cut short with %d left (again %s), "
      .. "3 frames later bad %d (%s) again %s", A.near_s, A.all, A.broken2,
      A.left, tostring(A.again), #bad, table.concat(bad, " "),
      tostring(S.rings5a_again)))
    A.done = true
  end
end
