-- GATE: ticks=9000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a guard who dies while his fort stands keeps the prize
-- (walled.DEAD_HOLDER_KEEPS_BUILT on): he is still the holder, the fort
-- keeps its armour and scores nothing, and the hunters have to shoot it
-- down to take it.
--
-- Seat 0 gets the prize at tick 300 and is set up for a fort the way
-- pilltag_shoot_built_prize is: his armour is held at 15 (at or below
-- FORT_ARMOUR_AT) and seat 1 is kept 7 squares behind him until he builds,
-- then put 5 squares behind him, so the fort is not taken straight back.
-- Seat 1's gun is kept empty and his tank whole while the fort stands, and
-- for the first second after the death, so only the death can change the
-- fort before the first check. Half a second after the fort goes up the
-- arena kills the guard. After the first check nothing is held: seat 1 may
-- shoot the fort, and seat 0 respawns and goes back for it.
--
-- PASS, all three, else FAIL naming the step that broke:
--   1. one second (100 ticks) after the death the fort still stands with
--      the armour it had at the death, and seat 0 is still the holder;
--   2. from the death until somebody picks the prize up again, nobody
--      scores: seat 0's and seat 1's seconds stay as they were at the
--      death (only carrying scores, and the prize stands or lies dead);
--   3. the round resolves within RESOLVE_SECONDS of the death: the prize
--      is shot down and picked up, either by seat 1 (the hunter takes it,
--      and is the new holder) or by seat 0 (the respawned guard takes it
--      back). Either way the one who picked it up is the holder with it in
--      his tank. In the runs on seeds 42 7 123 2026 1 99 555 9001 the
--      guard respawned 5.1 s after the death and started his take-back,
--      but the hunter shot the fort down first (12 to 15 s) and picked it
--      up 17 to 21 s after the death; RESOLVE_SECONDS is 40, about twice
--      that. The gate's 9000 ticks leave room for it: the death is at
--      about tick 1630, so 40 s ends at about tick 5630.

ARENA = { given = false, RESOLVE_SECONDS = 40 }

local arena_real_picked = on_pill_picked_up
function on_pill_picked_up(n, p, scripted)
  local A = ARENA
  arena_real_picked(n, p, scripted)
  if A.killed_at ~= nil and A.picked == nil and n == pill then
    A.picked = { p = p, t = game.tick(), holder = holder }
    game.log(string.format("ARENA picked by %d t=%d (%.1fs after the death) "
                           .. "holder=%s order=%s", p, game.tick(),
                           (game.tick() - A.killed_at) / 100,
                           tostring(holder), tostring(told_for[p])))
  end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil or GATE_SAID then return end
  if not A.given then
    if tick >= 300 then
      local pb = game.pill(pill)
      game.teleport(0, pb.x, pb.y, 64)
      game.teleport(1, pb.x - 7, pb.y, 64)
      game.give_pill(0, pill)
      A.given = true
    end
    return
  end
  local me = game.tank(0)
  local s = game.tank(1)
  local pb = game.pill(pill)
  if plan == nil and A.built_at == nil and tick % 100 == 0 and me ~= nil
     and not me.dead then
    game.set_stocks(0, { armour = 15 })
    local fx, fy = facing(me.dir)
    local k = 7 / math.max(math.abs(fx), math.abs(fy))
    game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
  end
  if plan ~= nil and not plan.built and A.moved == nil and me ~= nil then
    A.moved = true
    local fx, fy = facing(me.dir)
    local k = 5 / math.max(math.abs(fx), math.abs(fy))
    game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
  end
  if plan ~= nil and plan.built and A.built_at == nil then
    A.built_at, A.fx, A.fy = tick, pb.x, pb.y
    game.log(string.format("ARENA %s up at %d,%d t=%d", plan.kind, pb.x, pb.y,
                           tick))
    if plan.kind ~= "fort" then
      verdict(false, "built a " .. plan.kind .. " with armour 15")
      return
    end
  end
  if A.built_at == nil then
    if tick >= GATE_TICKS - 100 then
      verdict(false, "the fort never went up")
    end
    return
  end
  -- The hunter's gun is kept empty and his tank whole while the fort stands
  -- before the death and for the first second after it, so only the death
  -- can change the fort before step 1.
  if (A.killed_at == nil or tick <= A.killed_at + 100) and s ~= nil and
     not s.dead then
    if s.shells > 0 then
      game.set_stocks(1, { shells = 0 })
    end
    if s.armour < game.rule("tank_full_armour") then
      game.set_stocks(1, { armour = game.rule("tank_full_armour") })
    end
  end
  if A.killed_at == nil and tick >= A.built_at + 50 then
    if holder ~= 0 or not standing(pb) then
      verdict(false, "the fort was not standing with seat 0 on it at the kill")
      return
    end
    A.killed_at = tick
    A.armour = pb.armour
    A.score0, A.score1 = seconds[0] or 0, seconds[1] or 0
    game.kill_tank(0)
    game.log(string.format("ARENA guard killed t=%d, fort armour %d, seat 1 "
                           .. "%d from the fort", tick, pb.armour,
                           s and chebyshev(s.mx, s.my, A.fx, A.fy) or -1))
    return
  end
  if A.killed_at == nil then
    return
  end

  -- Step 1: a second after the death the fort is as it was and his.
  if A.checked == nil and tick >= A.killed_at + 100 then
    A.checked = tick
    if not standing(pb) or pb.armour ~= A.armour or holder ~= 0 then
      verdict(false, string.format("step 1: 1 s after the death the fort "
                                   .. "should stand at armour %d held by 0: "
                                   .. "standing=%s armour %d, holder %s",
                                   A.armour, tostring(standing(pb)),
                                   pb and pb.armour or -1, tostring(holder)))
      return
    end
    game.log(string.format("ARENA step 1 ok t=%d: fort armour %d, holder 0, "
                           .. "guard dead=%s", tick, pb.armour,
                           tostring(me == nil or me.dead)))
  end

  -- Notes for the log: the guard's respawn, the take-back, the fort going
  -- down.
  if A.respawned == nil and me ~= nil and not me.dead then
    A.respawned = tick
    game.log(string.format("ARENA guard respawned t=%d (%.1fs after the "
                           .. "death), holder %s", tick,
                           (tick - A.killed_at) / 100, tostring(holder)))
  end
  if A.took_back == nil and plan ~= nil and plan.kind == "take" then
    A.took_back = tick
    game.log(string.format("ARENA guard's take-back t=%d", tick))
  end
  if A.down == nil and pb ~= nil and not pb.in_tank and pb.armour == 0 then
    A.down = tick
    game.log(string.format("ARENA fort down t=%d (%.1fs after the death), "
                           .. "holder %s", tick, (tick - A.killed_at) / 100,
                           tostring(holder)))
  end

  -- Step 2: nobody scores until the prize is picked up again.
  if A.picked == nil and
     ((seconds[0] or 0) ~= A.score0 or (seconds[1] or 0) ~= A.score1) then
    verdict(false, string.format("step 2: somebody scored with the prize "
                                 .. "out of every tank: seat 0 %d -> %d, "
                                 .. "seat 1 %d -> %d", A.score0,
                                 seconds[0] or 0, A.score1, seconds[1] or 0))
    return
  end

  -- Step 3: picked up again, by the hunter or by the guard, in time.
  if A.picked ~= nil then
    local after = (A.picked.t - A.killed_at) / 100
    local ok = A.checked ~= nil and A.picked.holder == A.picked.p and
               pb ~= nil and pb.in_tank and after <= A.RESOLVE_SECONDS
    local path = (A.picked.p == 0) and "guard took it back" or
                 string.format("hunter %d took it", A.picked.p)
    verdict(ok, string.format("%s %.1fs after the death (down at +%s, "
                              .. "guard back at +%s)%s", path, after,
                              A.down and string.format("%.1fs",
                                (A.down - A.killed_at) / 100) or "?",
                              A.respawned and string.format("%.1fs",
                                (A.respawned - A.killed_at) / 100) or "?",
                              ok and "" or " step 3: not the holder, or late"))
    return
  end
  if tick > A.killed_at + A.RESOLVE_SECONDS * 100 or
     tick >= GATE_TICKS - 100 then
    verdict(false, string.format("step 3: nobody picked the prize up within "
                                 .. "%d s of the death (fort %s, guard "
                                 .. "back %s)", A.RESOLVE_SECONDS,
                                 A.down and "down" or "standing",
                                 A.respawned and "yes" or "no"))
  end
end
