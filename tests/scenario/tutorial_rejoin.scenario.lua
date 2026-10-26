-- GATE: ticks=4000 bots=0 script=data/maps/tutorials/Tutorial.scenario.lua
--
-- Tutorial: the player leaves and another joins on a different seat.
--
-- The arena spawns a first player on a free seat and adopts it as the
-- tutorial's player, as tutorial_park5b does. It puts the tank in Station
-- 4, which moves the checkpoint there, and ticks a Station 2 goal. Then it
-- takes the first player out of the round (the script's on_player_leave
-- runs), spawns a second player on another free seat, and gives the script
-- the on_player_join a person's join gives it.
--
-- PASS:
--   - after the first player left the script holds no player seat;
--   - the second seat is adopted: the script's player is that seat;
--   - the checkpoint is still Station 4 and the Station 2 goal still
--     ticked, and the second player's tank comes to life in Station 4;
--   - every base and pillbox the first player owned is the second
--     player's;
--   - a popup and the checklist panel were sent to the second seat after
--     it joined, and nothing was sent to the first seat after it left.

TUTORIAL_PLAYER = -1          -- no seat yet; the arena spawns the players
ARENA = { phase = 0, sent = {} }

-- Count what the script sends, by kind and seat, since the last clear.
local function arena_count(kind, real, seat_arg)
  return function(...)
    local seat = select(seat_arg, ...)
    local k = kind .. ":" .. tostring(seat)
    ARENA.sent[k] = (ARENA.sent[k] or 0) + 1
    return real(...)
  end
end
game.popup = arena_count("popup", game.popup, 2)
game.panel = arena_count("panel", game.panel, 3)
game.status = arena_count("status", game.status, 3)

local function arena_sent(kind, seat)
  return ARENA.sent[kind .. ":" .. tostring(seat)] or 0
end

local function arena_spawn(slot, name)
  return game.spawn_bot({ slot = slot, team = 1,
    brain = "tutorial_shoot_north", name = name, loadout = "open" })
end

-- The owner of every base and pillbox in `owned` (S.owned's shape) that is
-- not seat p's, as text; "" when all are.
local function arena_not_owned_by(owned, p)
  local bad = {}
  for _, n in ipairs(owned.pills) do
    local pb = game.pill(n)
    if pb == nil or pb.owner ~= p then
      bad[#bad + 1] = "pill " .. n .. "=" .. tostring(pb and pb.owner)
    end
  end
  for _, n in ipairs(owned.bases) do
    local b = game.base(n)
    if b == nil or b.owner ~= p then
      bad[#bad + 1] = "base " .. n .. "=" .. tostring(b and b.owner)
    end
  end
  return table.concat(bad, " ")
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.over then return end
  local function fail(msg) A.over = true; verdict(false, msg) end
  if A.phase == 0 and tick >= 50 then
    local q, code = arena_spawn(game.max_tanks() - 1, "First")
    if type(q) ~= "number" then
      return fail("first player spawn refused: " .. tostring(code))
    end
    A.first = q
    TUTORIAL_PLAYER = q
    adopt_player(q)
    A.phase, A.t = 1, tick
  elseif A.phase == 1 and tick >= A.t + 100 then
    if S.player ~= A.first then return fail("the first player was not adopted") end
    game.teleport(A.first, 126, 140, 0)        -- Station 4, on the road
    A.phase, A.t = 2, tick
  elseif A.phase == 2 and tick >= A.t + 100 then
    mark(2, "b_take")
    A.phase, A.t = 3, tick
  elseif A.phase == 3 and tick >= A.t + 200 then
    -- What the first player owns, as the script noted it.
    A.owned = S.owned
    if S.reached ~= 4 or #A.owned.bases == 0 or #A.owned.pills == 0 or
       arena_not_owned_by(A.owned, A.first) ~= "" then
      return fail(string.format(
        "before leaving: reached %d, noted %d bases %d pills, not his: %s",
        S.reached, #A.owned.bases, #A.owned.pills,
        arena_not_owned_by(A.owned, A.first)))
    end
    local ok, code = game.remove_bot(A.first)
    if not ok then return fail("remove refused: " .. tostring(code)) end
    A.phase, A.t = 4, tick
  elseif A.phase == 4 and tick >= A.t + 100 then
    if S.player ~= nil then
      return fail("the script still holds seat " .. tostring(S.player)
                  .. " after the player left")
    end
    if game.lobby_slot(A.first) ~= nil then
      return fail("the first seat is not empty")
    end
    A.lost = arena_not_owned_by(A.owned, A.first)
    A.sent = {}
    -- The second player, on a different seat.
    local q, code = arena_spawn(game.max_tanks() - 2, "Second")
    if type(q) ~= "number" or q == A.first then
      return fail("second player spawn refused: " .. tostring(code))
    end
    A.second = q
    TUTORIAL_PLAYER = q
    on_player_join(q, false)
    A.phase, A.t = 5, tick
  elseif A.phase == 5 and tick >= A.t + 300 then
    local q = A.second
    local t = game.tank(q)
    local at = t ~= nil and not t.dead and station_of(t.mx, t.my) or nil
    local not_his = arena_not_owned_by(A.owned, q)
    local ok = S.player == q and
      S.reached == 4 and S.done[2].b_take == true and at == 4 and
      A.lost ~= "" and not_his == "" and
      arena_sent("popup", q) >= 1 and arena_sent("panel", q) >= 1 and
      arena_sent("popup", A.first) == 0 and arena_sent("panel", A.first) == 0
      and arena_sent("status", A.first) == 0
    A.over = true
    local line = string.format(
      "player %s (second %d, first %d); reached %d, "
      .. "2B ticked %s, tank in station %s; given away on leaving: %s; not "
      .. "the second's: [%s]; to second: %d popup %d panel; to first: %d "
      .. "popup %d panel %d status",
      tostring(S.player), q, A.first, S.reached, tostring(S.done[2].b_take), tostring(at),
      A.lost ~= "" and "yes" or "no", not_his,
      arena_sent("popup", q), arena_sent("panel", q),
      arena_sent("popup", A.first), arena_sent("panel", A.first),
      arena_sent("status", A.first))
    game.log("ARENA " .. line)     -- the verdict line is cut at 120
    verdict(ok, line)
  end
end
