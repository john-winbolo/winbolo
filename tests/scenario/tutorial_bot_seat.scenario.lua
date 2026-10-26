-- GATE: ticks=6000 bots=0 script=data/maps/tutorials/Tutorial.scenario.lua
--
-- Tutorial: a person joins on the seat put by for the Station 7 bot.
--
-- A person can only take a bot's seat when the lobby held none for the
-- bots and the script picked empty ones (reserve_seats). The gate's lobby
-- holds them, so the arena first takes the three held seats out of the
-- round. It then spawns a first player on a free seat and adopts it, as
-- tutorial_rejoin does, and puts the tank in Station 6, which fields the
-- Station 6 bot. The Station 7 bot is not fielded yet, so its seat is
-- empty. The first player then leaves, and a second one is spawned on that
-- very seat and given the on_player_join a person's join gives the script.
-- The script has to move the Station 7 bot to another empty seat
-- (move_bot_seat) and hand the Station 6 bot its peace list again: the list
-- leaves out the Station 7 bot's seat, so the old one would leave out the
-- new player.
--
-- PASS:
--   - before the join the Station 6 bot is a live tank and the Station 7
--     bot's seat is empty;
--   - after it the script's player is the second seat, and the Station 7
--     bot has another seat that is empty, not the Station 6 bot's and not
--     the 5A bot's;
--   - game.bot_init was called once, for the Station 6 bot's seat, and was
--     not refused; its peace list names the second player's seat and names
--     neither the Station 6 bot's seat nor the Station 7 bot's new one;
--   - the Station 6 bot's man leaves the tank after the join (the bot goes
--     on with its trips), and the second player's tank is alive in
--     Station 6.

TUTORIAL_PLAYER = -1          -- no seat yet; the arena spawns the players
ARENA = { phase = 0, inits = {}, out = 0 }

-- Every game.bot_init the script makes, with the answer it got.
local arena_real_bot_init = game.bot_init
game.bot_init = function(p, t)
  local ok, code = arena_real_bot_init(p, t)
  ARENA.inits[#ARENA.inits + 1] = { p = p, peace = t and t.peace, ok = ok,
                                    code = code }
  return ok, code
end

local function arena_spawn(slot, name)
  return game.spawn_bot({ slot = slot, team = 1,
    brain = "tutorial_shoot_north", name = name, loadout = "open" })
end

-- Whether a peace list ("0/1/4/...") names seat p.
local function arena_names(peace, p)
  for s in string.gmatch(peace or "", "[^/]+") do
    if tonumber(s) == p then return true end
  end
  return false
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.over then return end
  local function fail(msg) A.over = true; verdict(false, msg) end
  if not A.emptied and tick >= 20 then
    -- The seats the lobby holds for the bots are let go, so the script
    -- picks empty seats for them (reserve_seats): a held seat cannot be
    -- taken by a person, an empty one can.
    A.emptied = true
    for p = 0, game.max_tanks() - 1 do
      if game.lobby_slot(p) ~= nil then game.remove_bot(p) end
    end
    return
  end
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
    if S.player ~= A.first then
      return fail("the first player was not adopted")
    end
    game.teleport(A.first, 126, 86, 0)         -- Station 6, on the road
    A.phase, A.t = 2, tick
  elseif A.phase == 2 and tick >= A.t + 400 then
    A.old7 = S.bot7
    if live_tank(S.bot6) == nil or A.old7 == nil or
       not S.free_seat[A.old7] or game.lobby_slot(A.old7) ~= nil or
       #A.inits ~= 0 then
      return fail(string.format(
        "before leaving: Station 6 bot (seat %s) live %s, Station 7 seat %s "
        .. "empty %s, %d bot_init calls", tostring(S.bot6),
        tostring(live_tank(S.bot6) ~= nil), tostring(A.old7),
        tostring(A.old7 ~= nil and game.lobby_slot(A.old7) == nil),
        #A.inits))
    end
    local ok, code = game.remove_bot(A.first)
    if not ok then return fail("remove refused: " .. tostring(code)) end
    A.phase, A.t = 3, tick
  elseif A.phase == 3 and tick >= A.t + 100 then
    if S.player ~= nil then
      return fail("the script still holds seat " .. tostring(S.player)
                  .. " after the player left")
    end
    -- The second player, on the seat put by for the Station 7 bot.
    local q, code = arena_spawn(A.old7, "Second")
    if type(q) ~= "number" or q ~= A.old7 then
      return fail("second player spawn refused: " .. tostring(code))
    end
    A.second = q
    TUTORIAL_PLAYER = q
    on_player_join(q, false)
    A.phase, A.t = 4, tick
  elseif A.phase == 4 then
    -- Count the times the Station 6 bot's man is seen out of the tank.
    local b = game.builder(S.bot6)
    local out = b ~= nil and (b.state == "going" or b.state == "returning")
    if out and not A.was_out then A.out = A.out + 1 end
    A.was_out = out
    if tick < A.t + 3000 then return end
    local q = A.second
    local t = game.tank(q)
    local at = t ~= nil and not t.dead and station_of(t.mx, t.my) or nil
    local i = A.inits[1]
    local new7 = S.bot7
    local seat_ok = new7 ~= nil and new7 ~= q and new7 ~= S.bot6 and
      new7 ~= S.bot5a and game.lobby_slot(new7) == nil
    local init_ok = #A.inits == 1 and i.p == S.bot6 and i.ok and
      arena_names(i.peace, q) and not arena_names(i.peace, S.bot6) and
      not arena_names(i.peace, new7)
    local ok = S.player == q and seat_ok and init_ok and A.out >= 1 and
      at == 6
    A.over = true
    -- The verdict line is cut at 120, so the peace list has a line to
    -- itself.
    game.log("ARENA peace " .. tostring(i and i.peace))
    verdict(ok, string.format(
      "player %s; bot7 seat %d -> %s; %d bot_init, seat %s (bot6 %s) %s; "
      .. "man out %d; station %s",
      tostring(S.player), A.old7, tostring(new7), #A.inits,
      tostring(i and i.p), tostring(S.bot6), tostring(i and (i.ok or i.code)),
      A.out, tostring(at)))
  end
end
