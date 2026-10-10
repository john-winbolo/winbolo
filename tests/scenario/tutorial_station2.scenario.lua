-- GATE: ticks=4000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, Station 2: the 2A refill is told, not enforced, and the enemy
-- base's shot count waits for the neutral base.
--
-- Seat 0 plays the tutorial's player, held still. The arena puts it on the
-- 2A base (Station 2's low start: 5 shells), then 2 seconds later on the
-- main road, long before the tank is full; then on the 2B base.
--
-- PASS:
--   - on the 2A base the s2stay popup is shown and 2A is not yet ticked;
--   - off the base, not full, "Refill at your base" ticks (it does not wait
--     for a full tank);
--   - before 2B is taken the panel has no "Enemy base" line;
--   - on the 2B base "Take the neutral base" ticks, and then the panel shows
--     "Enemy base: N shots left".

TUTORIAL_PLAYER = 0
ARENA = { phase = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local function panel_has_enemy()
  return S.panel_key ~= nil and
         string.find(S.panel_key, "Enemy base", 1, true) ~= nil
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.done then return end
  if A.phase == 0 and tick >= 300 then
    game.teleport(0, B.b2a.x, B.b2a.y, 0)
    A.phase, A.t = 1, tick
  elseif A.phase == 1 and tick >= A.t + 200 then
    local t = game.tank(0)
    if not S.popped.s2stay or S.done[2].a_refill then
      verdict(false, string.format("on 2A: s2stay=%s a_refill=%s",
        tostring(S.popped.s2stay), tostring(S.done[2].a_refill)))
      A.done = true
      return
    end
    A.shells = t and t.shells
    game.teleport(0, 127, B.b2a.y - 4, 0)
    A.phase, A.t = 2, tick
  elseif A.phase == 2 and tick >= A.t + 100 then
    local t = game.tank(0)
    local full = t ~= nil and t.shells >= game.rule("tank_full_shells")
    if not S.done[2].a_refill or full or panel_has_enemy() then
      verdict(false, string.format("off 2A: a_refill=%s shells=%s "
        .. "enemy_line=%s", tostring(S.done[2].a_refill),
        tostring(t and t.shells), tostring(panel_has_enemy())))
      A.done = true
      return
    end
    game.teleport(0, B.b2b.x, B.b2b.y, 0)
    A.phase, A.t = 3, tick
  elseif A.phase == 3 and tick >= A.t + 200 then
    local ok = S.done[2].b_take == true and panel_has_enemy()
    verdict(ok, string.format("2A ticked off the base at %s shells; "
      .. "b_take=%s enemy_line=%s panel=%s", tostring(A.shells),
      tostring(S.done[2].b_take), tostring(panel_has_enemy()),
      tostring(S.panel_key)))
    A.done = true
  end
end
