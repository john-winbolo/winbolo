-- GATE: ticks=12000 bots=2 allybots=1 script=data/maps/Survival.scenario.lua
--
-- A defender BOT pushed out by Survival's start-camp rule drives back in.
--
-- One defender bot, D, is put on its own puddle start and killed by the
-- script three times, as soon as it is back each time: the third respawn
-- is at an outer start. From there the arena only watches. The bot brain
-- is not told anything; it has to find its own way home.
--
-- D cannot die any other way (can_die below), and no shell touches it
-- (can_hit below): a shell that takes its boat at sea would leave it
-- stranded on deep sea, alive at 0 armour. So this measures the drive and
-- not the fight.
--
-- PASS: inside 60 s of the outer respawn, D is within 8 squares of one of
-- the six centre bases.

ARENA = {
  start_s = 20,
  limit_s = 110,
  plan    = { { at = "hole" }, { at = "hole" }, { at = "hole" } },
  expect  = { "puddle", "puddle", "outer" },
}

function can_hit(attacker, kind, n, pill)
  if kind == "tank" and n == ARENA.D then return false end
  return nil
end

ARENA.after = function(tick)
  local t = game.tank(ARENA.D)
  if t ~= nil and not t.dead then
    for b = CENTER_FIRST, CENTER_FIRST + 5 do
      local bi = game.base(b)
      if math.max(math.abs(t.mx - bi.x), math.abs(t.my - bi.y)) <= 8 then
        return ARENA.say(true, string.format(
          "home from start %d in %.1f s, at (%d,%d) by base %d",
          ARENA.outer, (tick - ARENA.outer_at) / 100, t.mx, t.my, b))
      end
    end
    if ARENA.seen_at == nil or tick >= ARENA.seen_at then
      ARENA.seen_at = tick + secs(5)
      game.log(string.format("ARENA drive (%d,%d) at %.1f s armour %d boat %s tile %s",
        t.mx, t.my, (tick - ARENA.outer_at) / 100, t.armour, tostring(t.boat),
        tostring(game.map_tile(t.mx, t.my))))
    end
  end
  if tick - ARENA.outer_at > secs(60) then
    return ARENA.say(false, "not home 60 s after the outer respawn")
  end
end

-- ===== the driver: the same in every survival_pushout arena =====

ARENA.state, ARENA.k, ARENA.max_age = "idle", 0, 0

ARENA.say = function(ok, why)
  if ARENA.done then return end
  ARENA.done = true
  verdict(ok, why)
end

-- The arena's own copy of the outer-start rule, asked at the same moment.
ARENA.want_outer = function()
  local horde = {}
  for _, q in ipairs(seats_on(WAVE_TEAM)) do
    local t = game.tank(q)
    if t ~= nil and not t.dead then horde[#horde + 1] = t end
  end
  local best, best_d, txt = nil, nil, {}
  for n = 7, math.min(16, game.num_starts()) do
    local s = game.start(n)
    local d = 999
    for _, t in ipairs(horde) do
      d = math.min(d, math.max(math.abs(t.mx - s.x), math.abs(t.my - s.y)))
    end
    txt[#txt + 1] = n .. ":" .. d
    if d > 10 and ARENA.clear == nil then ARENA.clear = n end
    if best_d == nil or d > best_d then best, best_d = n, d end
  end
  -- Kept for on_tank_spawned to log: a policy's own game.log is not seen.
  ARENA.near = "horde " .. #horde .. " nearest " .. table.concat(txt, " ")
  return ARENA.clear or best
end

ARENA.old_choose = on_choose_start
function on_choose_start(p)
  local want
  if p == ARENA.D and CAMP.pending[p] then
    ARENA.clear = nil
    want = ARENA.want_outer()
  end
  local r = ARENA.old_choose(p)
  if p == ARENA.D then
    ARENA.chose, ARENA.want = r, want
  end
  return r
end

ARENA.old_killed = on_tank_killed
function on_tank_killed(victim, killer, cause, scripted)
  if victim == ARENA.D and ARENA.state == "respawn" then
    local s = CAMP.last[victim]
    if CAMP.hole_n > 0 then
      local age = s and (game.tick() - s.at) or 999
      ARENA.max_age = math.max(ARENA.max_age, age)
      if age > 4 then
        ARENA.say(false, string.format("death %d: saved state %d ticks old",
          ARENA.k, age))
      end
    end
  end
  ARENA.old_killed(victim, killer, cause, scripted)
end

-- Only the arena kills D.
function can_die(kind, n, killer, cause)
  if kind == "tank" and n == ARENA.D and cause ~= "script" then
    return false
  end
  return nil
end

ARENA.old_start = on_start
function on_start()
  ARENA.old_start()
  ARENA.D = seats_on(DEF_TEAM)[1]
  ARENA.home = 1 + (seat_rank(ARENA.D) % 6)
  -- A land square for a death off the water: beside centre base 9.
  local b = game.base(CENTER_FIRST)
  for r = 1, 3 do
    for dx = -r, r do
      for dy = -r, r do
        local tt = game.map_tile(b.x + dx, b.y + dy)
        if ARENA.lx == nil and (tt == game.TERRAIN.grass
           or tt == game.TERRAIN.road) then
          ARENA.lx, ARENA.ly = b.x + dx, b.y + dy
        end
      end
    end
  end
  game.log(string.format("ARENA D=%s home=%d hole=%d land=%s,%s",
    tostring(ARENA.D), ARENA.home, CAMP.hole_n, tostring(ARENA.lx),
    tostring(ARENA.ly)))
  -- The hole: built when the rule is on and not when it is off; every
  -- puddle start in it and no outer start.
  if ARENA.off then
    if CAMP.deaths ~= 0 or CAMP.hole_n ~= 0 then
      return ARENA.say(false, string.format("off: deaths %d hole %d",
        CAMP.deaths, CAMP.hole_n))
    end
    return
  end
  if CAMP.hole_n == 0 then return ARENA.say(false, "no hole was built") end
  for n = 1, math.min(16, game.num_starts()) do
    local s = game.start(n)
    if (CAMP.hole[s.x * 256 + s.y] == true) ~= (n <= 6) then
      return ARENA.say(false, "start " .. n .. " is on the wrong side of the hole")
    end
  end
end

ARENA.old_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  ARENA.old_spawned(p, mx, my, respawn, scripted)
  if p ~= ARENA.D or not respawn or ARENA.state ~= "respawn" then return end
  local k, r = ARENA.k, ARENA.chose
  local t = game.tank(p)
  local want = ARENA.expect[k]
  if ARENA.near ~= nil then game.log("ARENA " .. ARENA.near); ARENA.near = nil end
  game.log(string.format("ARENA respawn %d at (%d,%d) start %s want %s/%s boat %s",
    k, mx, my, tostring(r), want, tostring(ARENA.want),
    tostring(t and t.boat)))
  if want == "puddle" then
    if r == nil or r < 1 or r > 6 then
      return ARENA.say(false, string.format("respawn %d: start %s, not the puddle",
        k, tostring(r)))
    end
  else
    if r == nil or r < 7 or r > 16 or r ~= ARENA.want then
      return ARENA.say(false, string.format("respawn %d: start %s, want outer %s",
        k, tostring(r), tostring(ARENA.want)))
    end
    local s = game.start(r)
    if math.max(math.abs(mx - s.x), math.abs(my - s.y)) > 3 then
      return ARENA.say(false, string.format("respawn %d: (%d,%d) is not at start %d",
        k, mx, my, r))
    end
    if not (t and t.boat) then
      return ARENA.say(false, "respawn " .. k .. ": not on a boat at sea")
    end
    ARENA.outer_at, ARENA.outer = game.tick(), r
  end
  if k >= #ARENA.plan then
    ARENA.state = "after"
    ARENA.after_at = game.tick()
    return
  end
  ARENA.state = "delay"
  ARENA.due = game.tick() + secs((ARENA.plan[k + 1].delay or 0.2))
end

ARENA.old_tick = on_tick
function on_tick(tick)
  ARENA.old_tick(tick)
  local A = ARENA
  if A.done or A.D == nil then return end
  if tick > secs(A.limit_s) then
    return A.say(false, "timed out in state " .. A.state .. " at step " .. A.k)
  end
  if A.state == "idle" then
    local t = game.tank(A.D)
    if tick >= secs(A.start_s) and t ~= nil and not t.dead then
      A.state, A.due = "delay", tick
    end
  elseif A.state == "delay" and tick >= A.due then
    A.k = A.k + 1
    if A.plan[A.k].at == "land" then
      game.teleport(A.D, A.lx, A.ly)
    else
      game.teleport_to_start(A.D, A.home)
    end
    A.state, A.due = "kill", tick + 6
  elseif A.state == "kill" and tick >= A.due then
    local t = game.tank(A.D)
    local deep = t and game.map_tile(t.mx, t.my) == T_DEEP_SEA
    if A.plan[A.k].at == "hole" and not (t and t.boat and deep) then
      return A.say(false, "death " .. A.k .. ": D is not on a boat in the hole")
    end
    if A.plan[A.k].at == "land" and (t == nil or deep or t.boat) then
      return A.say(false, "death " .. A.k .. ": D is not on land")
    end
    game.log(string.format("ARENA kill %d at tick %d (%d,%d) %s", A.k, tick,
      t.mx, t.my, A.plan[A.k].at))
    game.kill_tank(A.D)
    A.state = "respawn"
  elseif A.state == "after" then
    if A.after ~= nil then return A.after(tick) end
    return A.say(true, string.format(
      "%d respawns as planned, outer start %s, hole %d, state age max %d",
      #A.plan, tostring(A.outer), CAMP.hole_n, A.max_age))
  end
end
