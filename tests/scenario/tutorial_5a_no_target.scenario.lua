-- GATE: ticks=30000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, Station 5A: the demo's pillboxes never target the player.
--
-- Seat 0 plays the tutorial's player, held still. At tick 300 the arena puts
-- it on the north-east corner of the 5A island, 3 squares east and 1 north
-- of the hostile target pillbox: well inside its range, and nearer to it
-- than the demo bot is for the whole of the bot's drive up the island. The
-- tank inside Station 5 fields the demo bot, which takes the target the way
-- tutorial_take5a watches it do.
--
-- Without the script's can_target the target picks the nearest tank, so it
-- fires at the player until the bot is closer than he is. The arena reads
-- what the pillbox does from the questions the engine asks:
--
--   can_target(pill, p)  the pillbox would take seat p as its target;
--   can_hit(_, "tank", p, pill)  one of pill's shells has reached seat p.
--     A shell is aimed where its target is, so shells reach the player only
--     when the pillbox aims at him: he stands east of the target and the
--     bot works south of it;
--   on_tank_hit(victim, _, _, _, pill)  one of pill's shells took armour.
--
-- PASS:
--   - the player was inside the target's range, alive and nearer to it than
--     the bot, with the target down and alive, for at least 3 seconds;
--   - the target asked can_target about the player (it would have taken
--     him) and the script said false every time;
--   - no shell of either 5A pillbox ever reached the player, and he lost no
--     armour to one;
--   - the target still fired at the bot: its shells reached the bot or the
--     bot's blocker;
--   - the bot took the target within 150 seconds of being fielded.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, asked = 0, let = 0, at_player = 0, hits = 0,
          at_bot = 0, nearer = 0 }

-- 3 east, 1 north of the target (137,105); on the island, off the bot's way.
ARENA.spot = { 140, 104 }

local function arena_5a(pill)
  return pill == LAYOUT.pill.t5a_target.n or pill == LAYOUT.pill.t5a_blocker.n
end

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
  if p == S.bot5a and ARENA.fielded == nil then
    ARENA.fielded = game.tick()
    game.log(string.format("ARENA 5A bot (seat %d) fielded at tick %d", p,
                           ARENA.fielded))
  end
end

local arena_real_target = can_target
function can_target(pill, p)
  local answer = nil
  if arena_real_target ~= nil then answer = arena_real_target(pill, p) end
  if p == S.player and arena_5a(pill) then
    ARENA.asked = ARENA.asked + 1
    if answer ~= false then ARENA.let = ARENA.let + 1 end
  end
  return answer
end

local arena_real_hit = can_hit
function can_hit(attacker, kind, n, pill)
  if pill ~= nil and arena_5a(pill) then
    if kind == "tank" and n == S.player then
      ARENA.at_player = ARENA.at_player + 1
      if ARENA.at_player <= 3 then
        game.log(string.format("ARENA pill %d's shell reached the player at "
                               .. "tick %d", pill, game.tick()))
      end
    elseif pill == LAYOUT.pill.t5a_target.n and
           ((kind == "tank" and n == S.bot5a) or
            (kind == "pill" and n == LAYOUT.pill.t5a_blocker.n)) then
      ARENA.at_bot = ARENA.at_bot + 1
    end
  end
  return arena_real_hit(attacker, kind, n, pill)
end

local arena_real_tank_hit = on_tank_hit
function on_tank_hit(victim, attacker, cause, amount, pill, scripted)
  arena_real_tank_hit(victim, attacker, cause, amount, pill, scripted)
  if victim == S.player and pill ~= nil and arena_5a(pill) then
    ARENA.hits = ARENA.hits + 1
  end
end

local arena_real_picked = on_pill_picked_up
function on_pill_picked_up(n, p, scripted)
  if ARENA.took == nil and p == S.bot5a and n == LAYOUT.pill.t5a_target.n
     and not scripted then
    ARENA.took = game.tick()
  end
  arena_real_picked(n, p, scripted)
end

-- Distance from a tank to the target pillbox's centre, in world units.
local function arena_dist(t)
  local T = LAYOUT.pill.t5a_target
  local dx = t.wx - (T.x * 256 + 128)
  local dy = t.wy - (T.y * 256 + 128)
  return math.sqrt(dx * dx + dy * dy)
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.over then return end
  local s = A.spot
  if A.phase == 0 and tick >= 300 then
    game.teleport(0, s[1], s[2], 0)
    A.phase = 1
    game.log(string.format("ARENA player put at %d,%d, terrain %s", s[1],
                           s[2], tostring(game.map_tile(s[1], s[2]))))
    return
  end
  if A.phase ~= 1 then return end
  local me = game.tank(0)
  -- Seat 0 is a bot held at 1 percent speed, so it creeps: put it back on
  -- its square every tenth of a second.
  if tick % 10 == 0 and me ~= nil and not me.dead and
     (me.mx ~= s[1] or me.my ~= s[2]) then
    game.teleport(0, s[1], s[2], 0)
  end
  if me ~= nil and not me.dead then
    if A.armour == nil then A.armour = me.armour end
    if me.armour < A.armour then A.lost = true end
  elseif me ~= nil and me.dead then
    A.died = true
  end
  -- The player nearer the target than the bot, in range, target able to fire.
  local tg = game.pill(LAYOUT.pill.t5a_target.n)
  local bot = S.bot5a ~= nil and game.tank(S.bot5a) or nil
  if me ~= nil and not me.dead and tg ~= nil and not tg.in_tank and
     tg.armour > 0 and A.took == nil and bot ~= nil and not bot.dead then
    local dm = arena_dist(me)
    if dm < game.rule("pill_range") and dm < arena_dist(bot) then
      A.nearer = A.nearer + 1
    end
  end
  if A.fielded ~= nil and A.took == nil and (tick - A.fielded) % 500 == 0 then
    game.log(string.format("ARENA t=%ds asked %d let %d at_player %d hits %d "
      .. "at_bot %d nearer %d; bot %s,%s", math.floor((tick - A.fielded) / 100),
      A.asked, A.let, A.at_player, A.hits, A.at_bot, A.nearer,
      tostring(bot and bot.mx), tostring(bot and bot.my)))
  end
  if A.took ~= nil and tick >= A.took + 100 then
    A.over = true
    local ok = A.nearer >= 300 and A.asked >= 1 and A.let == 0 and
               A.at_player == 0 and A.hits == 0 and not A.lost and
               not A.died and A.at_bot >= 1
    verdict(ok, string.format("nearer %.1fs; asked %d let %d; at player %d "
      .. "hits %d lost %s died %s; at bot %d; take %.1fs", A.nearer / 100,
      A.asked, A.let, A.at_player, A.hits, tostring(A.lost or false),
      tostring(A.died or false), A.at_bot, (A.took - A.fielded) / 100))
  elseif A.fielded ~= nil and tick >= A.fielded + 15000 then
    A.over = true
    verdict(false, string.format("no 5A take in 150 s; asked %d at player %d "
      .. "at bot %d nearer %d", A.asked, A.at_player, A.at_bot, A.nearer))
  elseif A.fielded == nil and tick >= 3300 then
    A.over = true
    verdict(false, "the 5A bot was never fielded")
  end
end
