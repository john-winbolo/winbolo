-- GATE: ticks=6000 bots=3 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a bot holder hops in a team round.
--
-- The arena turns the round into a team round as it starts: seats 0 and 1 on
-- team 1, seat 2 on team 2. Seat 0 gets the prize at tick 300, so seat 1 is
-- his mate and seat 2 the only hunter. Until he starts a hop, the arena keeps
-- his armour full (a hop, not a fort) and seat 2 7 squares behind him, the
-- pilltag_hop_ahead set-up.
--
-- The mate plays with REPOSITION_VOTE_ENABLED off, so it casts no vote on
-- the holder's pillbox move, and the move goes ahead on the holder's vote.
--
-- PASS: the mate is sent to ride behind the holder (an "escort 0" order),
-- and the holder's hop is built and picked up again by seat 0.

ARENA = { given = false }

local arena_real_start = on_start
function on_start()
  team_mode = true
  arena_real_start()
  lobby_team[0], lobby_team[1], lobby_team[2] = 1, 1, 2
  sort_teams()
  tune_everybody()
  game.log("ARENA teams: 0,1 on team 1; 2 on team 2")
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if not A.given then
    if tick >= 300 then
      local pb = game.pill(pill)
      game.teleport(0, pb.x, pb.y, 64)
      game.teleport(1, pb.x - 3, pb.y + 3, 64)
      game.teleport(2, pb.x - 7, pb.y, 64)
      game.give_pill(0, pill)
      A.given = true
    end
    return
  end
  if A.escort == nil and told_for[1] == "escort 0" then
    A.escort = tick
    game.log(string.format("ARENA mate escort order t=%d", tick))
  end
  local me = game.tank(0)
  if A.plan == nil then
    if plan ~= nil then
      A.plan = plan.kind
      game.log(string.format("ARENA %s at %d,%d t=%d", plan.kind, plan.x, plan.y, tick))
      if plan.kind ~= "hop" then
        verdict(false, "a " .. plan.kind .. " with full armour")
      end
    elseif tick % 100 == 0 and me ~= nil and not me.dead then
      game.set_stocks(0, { armour = game.rule("tank_full_armour") })
      local fx, fy = facing(me.dir)
      local k = 7 / math.max(math.abs(fx), math.abs(fy))
      game.teleport(2, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
    end
  end
  local pb = game.pill(pill)
  if A.plan ~= nil and A.built == nil and standing(pb) then
    A.built = tick
  end
  if A.built ~= nil and pb.in_tank and holder ~= nil then
    verdict(holder == 0 and A.escort ~= nil,
            string.format("hop picked up by %d %.1fs after the build, mate escort %s",
                          holder, (tick - A.built) / 100,
                          A.escort and "sent" or "never sent"))
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, A.plan == nil and "no hop" or
                   (A.built == nil and "never built" or "built, never picked up"))
  end
end
