-- GATE: ticks=4000 bots=1 script=data/maps/tutorials/Tutorial.scenario.lua
--
-- Tutorial, the finish.
--
-- Seat 0 plays the tutorial's player, held still. At tick 300 the arena gives
-- it seven of the eight Station 7 bases (a scripted owner change, which the
-- tutorial does not count as a capture), makes the eighth neutral, and puts
-- the tank on that one, so the tank takes it.
--
-- PASS: the tutorial is won, and on the win it called game.tutorial_done
-- once, for seat 0, and the call was taken.

TUTORIAL_PLAYER = 0
-- Kept in ARENA, not in locals: the arena and the script are one chunk, and
-- a chunk may hold only 200 locals.
ARENA = { phase = 0, calls = 0, last = "b7_sw2",
          others = { "b7_ne1", "b7_ne2", "b7_nw1", "b7_nw2", "b7_se1",
                     "b7_se2", "b7_sw1" },
          real_done = game.tutorial_done }

game.tutorial_done = function(p)
  local ok, code = ARENA.real_done(p)
  ARENA.calls = ARENA.calls + 1
  ARENA.seat, ARENA.ok, ARENA.code = p, ok, code
  return ok, code
end

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.done then return end
  if A.phase == 0 and tick >= 300 then
    for _, k in ipairs(A.others) do game.set_base_owner(B[k].n, 0) end
    game.set_base_owner(B[A.last].n, game.NEUTRAL)
    if S.won or A.calls ~= 0 then
      verdict(false, string.format("before the last base: won=%s calls=%d",
        tostring(S.won), A.calls))
      A.done = true
      return
    end
    game.teleport(0, B[A.last].x, B[A.last].y, 0)
    A.phase, A.t = 1, tick
  elseif A.phase == 1 and tick >= A.t + 200 then
    local b = game.base(B[A.last].n)
    local ok = S.won == true and A.calls == 1 and A.seat == 0 and
               A.ok == true
    verdict(ok, string.format("won=%s last_base_owner=%s calls=%d seat=%s "
      .. "answer=%s code=%s", tostring(S.won), tostring(b and b.owner),
      A.calls, tostring(A.seat), tostring(A.ok), tostring(A.code)))
    A.done = true
  end
end
