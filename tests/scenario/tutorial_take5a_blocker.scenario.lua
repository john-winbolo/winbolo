-- GATE: ticks=59000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the Station 5A demo bot builds its blocker on every take
-- (item 39).
--
-- Seat 0 plays the tutorial's player, held on the green watching square in
-- Station 5 with no shells and with noblitz (a human never joins the demo
-- bot's blitz; a GoalHunter seat does, and the demo bot then builds a wall
-- in place of its pillbox). The 5A demo bot takes the target again and
-- again. Every other take window the arena breaks into the demo, one way
-- after another:
--   1. it kills the bot 5 seconds in, before it builds (a respawn);
--   2. it kills the bot 3 seconds after it built (a respawn);
--   3. it calls the script's reset_5a 4 seconds after the bot built (the
--      150-second watchdog's reset, in the middle of a take);
--   4. it takes the player out of Station 5 for 8 seconds, so the script
--      takes the bot off, and back in, so the script fields it again.
-- The window after a break runs to its take untouched.
--
-- A take window opens each time the bot is fielded or comes back to life,
-- and at a reset that leaves the bot on the field. In each window the arena
-- notes when the bot first builds the blocker and when the target first
-- loses armour (the bot's shots are the only ones that reach it).
--
-- PASS: at least 12 windows saw a shot at the target, and in every window
-- the blocker was built before the first shot; at least 8 takes; every
-- take had a blocker built in its window. Before item 39, every reset in
-- the middle of a take was followed by a shot with no blocker.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, wins = 0, shot_wins = 0, bad = 0, takes = 0,
          bare_takes = 0, kills = 0, resets = 0, outs = 0, w = nil,
          deaths = 0 }
scenario.callbacks.on_tank_killed = "Arena: counts the 5A demo bot's deaths."

local function bl_text()
  local bl = game.pill(LAYOUT.pill.t5a_blocker.n)
  if bl == nil then return "gone" end
  if bl.in_tank then return "carried by " .. tostring(S.carrier[LAYOUT.pill.t5a_blocker.n]) end
  return string.format("%d,%d a%d", bl.x, bl.y, bl.armour)
end

local function open_window(why)
  local A = ARENA
  A.wins = A.wins + 1
  A.w = { n = A.wins, at = game.tick(), why = why }
  -- Every other window is broken into, one way after another; the window
  -- after a break runs to its take untouched.
  A.w.brk = not A.last_brk
  A.last_brk = A.w.brk
  if A.w.brk then A.kind = (A.kind or 0) % 4 + 1; A.w.kind = A.kind end
  game.log(string.format("ARENA w%d open t%d: %s", A.wins, game.tick(), why))
end

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
  if p == S.bot5a and S.fielded.bot5a then
    open_window(respawn and "bot respawned" or "bot fielded")
  end
end

-- reset_5a is the script's local; its callers read the variable, so this
-- wrapper sees every reset (the end of a take, the stall reset, ours). A
-- reset that takes the bot off opens its window when the bot is fielded
-- again (on_tank_spawned); one that leaves the bot on opens it here.
local arena_real_reset_5a = reset_5a
reset_5a = function()
  arena_real_reset_5a()
  if live_tank(S.bot5a) ~= nil and S.fielded.bot5a then
    open_window("reset_5a")
  end
end

local arena_real_placed = on_pill_placed
function on_pill_placed(n, p, armour, scripted)
  arena_real_placed(n, p, armour, scripted)
  local w = ARENA.w
  if n == LAYOUT.pill.t5a_blocker.n and p == S.bot5a and not scripted and
     armour > 0 and w ~= nil and w.built == nil then
    w.built = game.tick()
    local pb = game.pill(n)
    game.log(string.format("ARENA w%d built t%d at %s,%s (+%.1fs)", w.n,
      w.built, tostring(pb and pb.x), tostring(pb and pb.y),
      (w.built - w.at) / 100))
  end
end

local arena_real_killed = on_tank_killed
function on_tank_killed(victim, killer, cause, scripted)
  if arena_real_killed ~= nil then
    arena_real_killed(victim, killer, cause, scripted)
  end
  if victim == S.bot5a and not scripted then
    ARENA.deaths = ARENA.deaths + 1
    game.log("ARENA 5A bot died: " .. tostring(cause))
  end
end

local arena_real_picked = on_pill_picked_up
function on_pill_picked_up(n, p, scripted)
  local w = ARENA.w
  if p == S.bot5a and n == LAYOUT.pill.t5a_target.n and not scripted and
     w ~= nil then
    ARENA.takes = ARENA.takes + 1
    if w.built == nil then ARENA.bare_takes = ARENA.bare_takes + 1 end
    game.log(string.format("ARENA w%d take %d t%d built %s", w.n,
      ARENA.takes, game.tick(), tostring(w.built ~= nil)))
  end
  arena_real_picked(n, p, scripted)
end

local function hold(at)
  local me = game.tank(0)
  if me ~= nil and not me.dead then
    if me.mx ~= at[1] or me.my ~= at[2] then
      game.teleport(0, at[1], at[2], 0)
    end
    if (me.shells or 0) > 0 then game.set_stocks(0, { shells = 0 }) end
  end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.done then return end
  local watch = LAYOUT.point.watch5a
  if A.phase == 0 then
    if tick >= 300 then
      -- Seat 0 is a GoalHunter bot on the demo bot's team. Left alone it
      -- joins the demo bot's blitz, and the demo bot then builds a wall, not
      -- its pillbox, on a square on seat 0's line to the target. A human
      -- player never joins a blitz, so seat 0 is told not to.
      game.bot_init(0, { noblitz = "1" })
      A.at = watch
      game.teleport(0, watch[1], watch[2], 0)
      A.phase = 1
    end
    return
  end
  if tick % 10 == 0 then hold(A.at) end

  local bl = game.pill(LAYOUT.pill.t5a_blocker.n)
  -- The target's first loss of armour in the window.
  local tg = game.pill(LAYOUT.pill.t5a_target.n)
  local w = A.w
  if tg ~= nil and not tg.in_tank then
    if A.tg_last ~= nil and tg.armour < A.tg_last and w ~= nil and
       w.shot == nil then
      w.shot = tick
      A.shot_wins = A.shot_wins + 1
      local t = game.tank(S.bot5a)
      if w.built == nil then
        A.bad = A.bad + 1
        game.log(string.format("ARENA w%d BARE SHOT t%d (+%.1fs) bot %s,%s "
          .. "pills %s", w.n, tick, (tick - w.at) / 100,
          tostring(t and t.mx), tostring(t and t.my), tostring(t and t.pills)))
        game.log(string.format("ARENA w%d   blocker %s", w.n, bl_text()))
      else
        game.log(string.format("ARENA w%d shot t%d (+%.1fs)", w.n, tick,
          (tick - w.at) / 100))
      end
    end
    A.tg_last = tg.armour
  else
    A.tg_last = nil
  end

  -- Breaking into the demo: every other window, one way, round and round.
  -- The window after a break runs to its take untouched.
  if w ~= nil and not w.hit and live_tank(S.bot5a) ~= nil and
     w.brk then
    local k = w.kind
    local age = tick - w.at
    if k == 1 and age >= 500 and w.built == nil then
      w.hit = true; A.kills = A.kills + 1
      game.log(string.format("ARENA w%d kill the bot before it builds", w.n))
      game.kill_tank(S.bot5a)
    elseif k == 2 and w.built ~= nil and tick >= w.built + 300 then
      w.hit = true; A.kills = A.kills + 1
      game.log(string.format("ARENA w%d kill the bot after it built", w.n))
      game.kill_tank(S.bot5a)
    elseif k == 3 and w.built ~= nil and tick >= w.built + 400 then
      w.hit = true; A.resets = A.resets + 1
      game.log(string.format("ARENA w%d reset_5a mid-take", w.n))
      reset_5a()
    elseif k == 4 and age >= 800 and A.out == nil then
      w.hit = true; A.outs = A.outs + 1
      local r = LAYOUT.region.s5
      A.at = { 127, r.y + r.h + 2 }
      A.out = tick
      game.log(string.format("ARENA w%d player out of Station 5 to %d,%d",
        w.n, A.at[1], A.at[2]))
      game.teleport(0, A.at[1], A.at[2], 0)
    end
  end
  if A.out ~= nil and tick >= A.out + 800 then
    A.out = nil
    A.at = watch
    game.log(string.format("ARENA player back; bot5a fielded %s",
      tostring(S.fielded.bot5a)))
    game.teleport(0, watch[1], watch[2], 0)
  end

  if tick >= 58000 then
    A.done = true
    game.log(string.format("ARENA windows %d, with a shot %d, bare %d",
      A.wins, A.shot_wins, A.bad))
    game.log(string.format("ARENA takes %d, bare %d; kills %d resets %d "
      .. "outs %d deaths %d", A.takes, A.bare_takes, A.kills, A.resets,
      A.outs, A.deaths))
    verdict(A.shot_wins >= 12 and A.bad == 0 and A.takes >= 8 and
            A.bare_takes == 0,
      string.format("%d windows shot in, %d bare; %d takes, %d bare",
        A.shot_wins, A.bad, A.takes, A.bare_takes))
  end
end
