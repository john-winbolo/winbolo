-- GATE: ticks=6000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the bots are fielded late.
--
-- Seat 0 plays the tutorial's player, held still. The tutorial picks out a
-- seat for each of its three bots as the round starts, but fields each bot
-- only when the player first drives into its station. The arena walks the
-- player through Stations 5, 6 and 7 in turn.
--
-- PASS:
--   * at tick 100 the three bot seats are picked out, all different and none
--     the player's; none of them has a tank yet; the 2C base and the 5B
--     target already belong to the Station 6 bot's seat;
--   * after Station 5 only the 5A bot has a tank, and it is on team 1 (the
--     player's);
--   * after Station 6 the Station 6 bot has a tank too, on team 2, and the
--     Station 7 bot still has none;
--   * after Station 7 the Station 7 bot has a tank, on team 2.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local function has_tank(p)
  local t = p and game.tank(p)
  return t ~= nil and not t.dead
end

local function team(p)
  local s = p and game.lobby_slot(p)
  return s and s.team
end

local function seats()
  return string.format("bot5a=%s bot6=%s bot7=%s", tostring(S.bot5a),
                       tostring(S.bot6), tostring(S.bot7))
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.phase == 0 and tick >= 100 then
    local bad = {}
    local a, b, c = S.bot5a, S.bot6, S.bot7
    if a == nil or b == nil or c == nil or a == b or a == c or b == c or
       a == 0 or b == 0 or c == 0 then
      bad[#bad + 1] = "seats not picked out"
    end
    if has_tank(a) or has_tank(b) or has_tank(c) then
      bad[#bad + 1] = "a bot has a tank before its station"
    end
    local base = game.base(LAYOUT.base.b2c.n)
    if base == nil or b == nil or base.owner ~= b then
      bad[#bad + 1] = "2C base not the Station 6 bot's (owner "
                      .. tostring(base and base.owner) .. ")"
    end
    local pb = game.pill(LAYOUT.pill.t5b_target.n)
    if pb == nil or b == nil or pb.owner ~= b then
      bad[#bad + 1] = "5B target not the Station 6 bot's (owner "
                      .. tostring(pb and pb.owner) .. ")"
    end
    game.log("ARENA seats at tick 100: " .. seats())
    if #bad > 0 then
      verdict(false, table.concat(bad, ", ") .. "; " .. seats())
      return
    end
    A.phase = 1
  elseif A.phase == 1 and tick >= 300 then
    game.teleport(0, 126, 110, 0)          -- Station 5, on the road
    A.phase, A.t = 2, tick
  elseif A.phase == 2 and tick >= A.t + 200 then
    if not has_tank(S.bot5a) or team(S.bot5a) ~= 1 or has_tank(S.bot6) or
       has_tank(S.bot7) then
      verdict(false, string.format("after Station 5: 5A tank %s team %s; "
        .. "Station 6 tank %s; Station 7 tank %s", tostring(has_tank(S.bot5a)),
        tostring(team(S.bot5a)), tostring(has_tank(S.bot6)),
        tostring(has_tank(S.bot7))))
      return
    end
    game.log("ARENA Station 5 fielded the 5A bot only, on team 1")
    game.teleport(0, 126, 86, 0)           -- Station 6, on the road
    A.phase, A.t = 3, tick
  elseif A.phase == 3 and tick >= A.t + 200 then
    if not has_tank(S.bot6) or team(S.bot6) ~= 2 or has_tank(S.bot7) then
      verdict(false, string.format("after Station 6: Station 6 tank %s team "
        .. "%s; Station 7 tank %s", tostring(has_tank(S.bot6)),
        tostring(team(S.bot6)), tostring(has_tank(S.bot7))))
      return
    end
    game.log("ARENA Station 6 fielded the Station 6 bot, on team 2")
    local a = LAYOUT.point.s7_arrive
    game.teleport(0, a[1], a[2], 0)
    A.phase, A.t = 4, tick
  elseif A.phase == 4 and tick >= A.t + 200 then
    local ok = has_tank(S.bot7) and team(S.bot7) == 2
    verdict(ok, string.format("each bot fielded on entry to its station; "
      .. "Station 7 tank %s team %s; %s", tostring(has_tank(S.bot7)),
      tostring(team(S.bot7)), seats()))
  end
end
