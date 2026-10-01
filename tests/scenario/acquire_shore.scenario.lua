-- acquire_shore -- does GoalHunter 1.7 turn on a tank that appears across
-- the water while it drives along a shore?
--
-- The acquire_driving arena (read acquire_idle's header for the measures and
-- the threshold) on a 3-wide road strip in deep sea instead of a field. The
-- shooter drives north along the strip toward the neutral base; the target
-- appears 5 squares away, ahead, at +-45, +-90 or behind. Off the strip it
-- stands on one square of grass the arena puts in the sea.
--
-- THE FAULT THIS ARENA SHOWS. The global cliff brake in steering.lua (the
-- "Global cliff safety" block, about line 3710) runs BEFORE tank_combat_steer
-- (about line 3926). Whenever the heading ray meets deep sea within the
-- stopping distance it returns KEY_SLOWER plus an evade turn toward the side
-- with more runway, so the aim is skipped that tick. Turning toward a target
-- across the water points the ray at the sea, the evade turn swings the gun
-- back, engage then asks for speed 12 again, and the gun wobbles until the
-- tank has stopped. With the brake switched off (cfg=CLIFF_MIN_SPEED=9999)
-- the same events come in at the turn limit (median 5 frames over), so the
-- brake is the whole of the delay. Until that is fixed the arena is expected
-- to fail.
--
-- GATE: ticks=24000 bots=1 gametype=open ai=yesfull expect=fail GoalHunter 1.7 cliff brake (steering.lua ~3710) skips the attack_tank aim turn while moving toward deep sea

local MODE = "driving"

-- ======================================================================
-- Everything below is the same in acquire_idle and acquire_driving.
-- ======================================================================

local SHOOTER = 0
local TARGET  = 1
local DUMMY   = "lead_dummy"
local OFFSETS = { 0, 32, -32, 64, -64, 128 }
local ROUNDS  = 3                -- each offset this many times
local POP_DIST = 5               -- squares
local SETTLE   = 200             -- idle: frames between events
local HOME_LEAD = 5              -- idle: frames from re-homing to the pop
local WATCH    = 400             -- frames an event may take
local AFTER    = 15              -- frames to watch after the shot
local SLACK    = 10              -- the threshold above, in frames
local DRIVE_MIN_SPEED = 8        -- driving: pop once this fast...
local DRIVE_MIN_MOVED = 3        -- ...and this many squares from the start

local frame = 0
local A = nil
local R = {}
local spawned = false
local base_n = nil
local phase = "boot"             -- boot, calib, prep, wait, watch, after, done
local phase_t = 0
local ev_i = 0
local ev = nil
local events = {}
local calib = nil                -- cumulative turn per frame from rest
local restore = nil              -- {x, y, tile} of a square made grass
local prev_shells = nil
local hist = {}

local atan2 = math.atan2 or math.atan

local function brad(dx, dy) return (atan2(dx, -dy) * 128 / math.pi) % 256 end
local function adiff(a, b)
  local d = (a - b) % 256
  if d > 128 then d = d - 256 end
  return d
end
local function median(t)
  if #t == 0 then return 0 end
  local s = {}
  for i = 1, #t do s[i] = t[i] end
  table.sort(s)
  local n = #s
  if n % 2 == 1 then return s[(n + 1) / 2] end
  return (s[n / 2] + s[n / 2 + 1]) / 2
end

local function push_hist(t)
  hist[#hist + 1] = { wx = t.wx, wy = t.wy }
  if #hist > 5 then table.remove(hist, 1) end
end
local function speed()
  if #hist < 5 then return 0 end
  local a, b = hist[1], hist[#hist]
  return math.sqrt((b.wx - a.wx) ^ 2 + (b.wy - a.wy) ^ 2) / (#hist - 1)
end

local function park_target()
  game.teleport(TARGET, A.px, A.py, 0)
  if restore then
    game.set_tile(restore.x, restore.y, restore.tile)
    restore = nil
  end
end

local function min_frames(angle)
  angle = math.abs(angle)
  if angle < 3 then return 0 end
  -- the bot fires inside 3 brads, so the turn needed is angle - 3
  local need = angle - 3
  for i = 1, #calib do
    if calib[i] >= need then return i end
  end
  local n = #calib
  local rate = calib[n] - calib[n - 1]
  return n + math.ceil((need - calib[n]) / rate)
end

local function home_shooter()
  if MODE == "idle" then
    game.teleport(SHOOTER, A.sx, A.mid, 0)
  else
    game.teleport(SHOOTER, A.sx, A.south, 0)
  end
  game.set_boat(SHOOTER, false)
  game.set_stocks(SHOOTER, { shells = R.full_shells, armour = R.full_armour,
                             trees = 0, mines = 0 })
  if base_n then
    if MODE == "idle" then
      game.set_base_owner(base_n, SHOOTER, true)
    else
      game.set_base_owner(base_n, game.NEUTRAL, true)
    end
  end
  hist = {}
end

local function pop(t)
  local off = OFFSETS[(ev_i - 1) % #OFFSETS + 1]
  local b = ((t.dir + 0.5) + off) % 256
  local rad = b * math.pi / 128
  local x = t.mx + math.floor(math.sin(rad) * POP_DIST + 0.5)
  local y = t.my - math.floor(math.cos(rad) * POP_DIST + 0.5)
  local terr = game.terrain()
  local tile = string.byte(terr, y * 256 + x + 1)
  if tile == game.TERRAIN.deep_sea then
    game.set_tile(x, y, game.TERRAIN.grass)
    restore = { x = x, y = y, tile = tile }
  end
  local ok, code, why = game.teleport(TARGET, x, y, 0)
  if not ok then game.log("ACQ pop refused " .. tostring(code) .. " " .. tostring(why)) end
  ev = { i = ev_i, off = off, E = frame, sx = t.mx, sy = t.my, spd0 = speed(),
         turn = nil, aim = nil, shot = nil, stall = 0, flips = 0, last = nil,
         lastdir = t.dir, movedir = 0, err0 = nil, maxstep = 0, stray = 0 }
end

local function close_event()
  local e = ev
  local m = min_frames(e.err0 or 0)
  e.min = m
  local tot = e.shot and (e.shot - e.E) or nil
  game.log(string.format(
    "ACQ %s ev=%d off=%+d err0=%+.0f spd=%.1f turn=%s aim=%s shot=%s min=%d over=%s stall=%d flips=%d stray=%d",
    MODE, e.i, e.off, e.err0 or 0, e.spd0,
    e.turn and tostring(e.turn - e.E) or "-", e.aim and tostring(e.aim - e.E) or "-",
    tot and tostring(tot) or "-", m, tot and tostring(tot - m) or "-",
    e.stall, e.flips, e.stray))
  events[#events + 1] = e
  ev = nil
end

local function finish()
  phase = "done"
  local all_ok, why = true, nil
  local overs, by_off = {}, {}
  for _, e in ipairs(events) do
    local k = e.off
    by_off[k] = by_off[k] or { turn = {}, aim = {}, shot = {}, min = {}, over = {}, n = 0, miss = 0 }
    local s = by_off[k]
    s.n = s.n + 1
    if e.shot then
      s.shot[#s.shot + 1] = e.shot - e.E
      s.over[#s.over + 1] = e.shot - e.E - e.min
      overs[#overs + 1] = e.shot - e.E - e.min
    else
      s.miss = s.miss + 1
      if all_ok then all_ok, why = false, string.format("off %+d ev %d: no shot in %d frames", e.off, e.i, WATCH) end
    end
    if e.turn then s.turn[#s.turn + 1] = e.turn - e.E end
    if e.aim then s.aim[#s.aim + 1] = e.aim - e.E end
    s.min[#s.min + 1] = e.min
  end
  local parts = {}
  for _, off in ipairs(OFFSETS) do
    local s = by_off[off]
    if s then
      game.log(string.format("ACQ %s off=%+d n=%d turn=%.0f aim=%.0f shot=%.0f min=%.0f over=%.0f nofire=%d",
        MODE, off, s.n, median(s.turn), median(s.aim), median(s.shot), median(s.min),
        median(s.over), s.miss))
      parts[#parts + 1] = string.format("%+d:%.0f/%.0f", off, median(s.shot), median(s.min))
    end
  end
  local mo = median(overs)
  game.log(string.format("ACQ %s median over=%.0f frames (slack %d)", MODE, mo, SLACK))
  if all_ok and mo > SLACK then
    all_ok, why = false, string.format("median shot-min %.0f > %d frames", mo, SLACK)
  end
  verdict(all_ok, (all_ok and "" or (why .. " ")) .. "shot/min " .. table.concat(parts, " "))
end

function can_die(g, kind, n, killer, cause, pill)
  if kind == "tank" and n == TARGET then return false end
  return nil
end

function on_choose_start(g, p)
  if p == SHOOTER then return 1 end
  if p == TARGET then return 2 end
  return nil
end

function on_tick(g, tick)
  if phase == "done" then return end
  frame = frame + 1
  if A == nil then
    local s1 = g.start(1)
    if not s1 then return end
    A = { sx = s1.x, mid = s1.y - 30, south = s1.y - 8, px = s1.x - 80, py = s1.y - 34,
          cal_y = s1.y - 50 }
    R.full_shells = g.rule("tank_full_shells")
    R.full_armour = g.rule("tank_full_armour")
    for n = 0, 3 do
      local b = g.base(n)
      if b then base_n = n; break end
    end
    g.log(string.format("ACQ setup mode=%s field x=%d mid y=%d park=(%d,%d) base=%s",
      MODE, A.sx, A.mid, A.px, A.py, tostring(base_n)))
  end
  if not spawned and frame >= 2 then
    spawned = true
    g.spawn_bot{ slot = TARGET, name = "Target", brain = DUMMY, team = 1, start = 2 }
  end
  local t, u = g.tank(SHOOTER), g.tank(TARGET)
  if not t or not u then return end
  if not t.dead then push_hist(t) end
  if not u.dead and u.armour < R.full_armour then g.set_stocks(TARGET, { armour = R.full_armour }) end

  phase_t = phase_t + 1

  if phase == "boot" then
    if frame == 3 then home_shooter() end
    if frame >= 20 and not u.dead then
      -- calibration: the dummy, facing 33, holds TURNRIGHT until it faces 63
      -- (deadband 1), from rest. Its turn per frame is a tank's from rest.
      g.teleport(TARGET, A.sx, A.cal_y, 33)
      calib = { }
      phase, phase_t = "calib", 0
    end
    return
  end

  if phase == "calib" then
    local turned = adiff(u.dir, 33)
    if phase_t >= 1 then calib[#calib + 1] = turned end
    if turned >= 30 or phase_t > 80 then
      -- the dummy stops one brad short (deadband); extend at the last rate
      local n = #calib
      local s = {}
      for i = 1, n do s[#s + 1] = string.format("%d", calib[i]) end
      g.log("ACQ calib cumulative turn per frame: " .. table.concat(s, ","))
      park_target()
      home_shooter()
      phase, phase_t = "prep", 0
    end
    return
  end

  -- keep the shooter fed; count shells
  if not t.dead then
    if t.shells < R.full_shells - 10 then
      g.set_stocks(SHOOTER, { shells = R.full_shells })
      prev_shells = nil
    end
  end

  if phase == "prep" then
    if t.dead then return end
    if MODE == "idle" then
      if phase_t == SETTLE - HOME_LEAD then home_shooter() end
      if phase_t >= SETTLE then
        ev_i = ev_i + 1
        pop(t)
        phase, phase_t = "watch", 0
        prev_shells = t.shells
      end
    else
      local moved = math.abs(t.my - A.south) + math.abs(t.mx - A.sx)
      if speed() >= DRIVE_MIN_SPEED and moved >= DRIVE_MIN_MOVED then
        ev_i = ev_i + 1
        pop(t)
        phase, phase_t = "watch", 0
        prev_shells = t.shells
      elseif phase_t > WATCH then
        g.log(string.format("ACQ driving: shooter did not set off (speed %.1f moved %d); re-homing", speed(), moved))
        home_shooter()
        phase_t = 0
      end
    end
    return
  end

  if phase == "watch" or phase == "after" then
    local e = ev
    if phase == "watch" and not t.dead and not u.dead then
      local b = brad(u.wx - t.wx, u.wy - t.wy)
      local err = adiff(t.dir, b)            -- what the brain compares (integer dir)
      if e.err0 == nil then e.err0 = adiff(e.lastdir + 0.5, b) end
      local step = adiff(t.dir, e.lastdir)
      if math.abs(step) > e.maxstep then e.maxstep = math.abs(step) end
      local toward = (step ~= 0) and ((err < 0) == (step > 0) or math.abs(err) < 3)
      if step ~= 0 then
        local sgn = step > 0 and 1 or -1
        if e.movedir ~= 0 and sgn ~= e.movedir and not e.aim then e.flips = e.flips + 1 end
        e.movedir = sgn
      end
      if not e.turn and toward then e.turn = frame end
      if e.turn and not e.aim and step == 0 then e.stall = e.stall + 1 end
      if not e.aim and math.abs(err) < 3 then e.aim = frame end
      e.lastdir = t.dir
      if prev_shells and t.shells < prev_shells then
        -- only a shell aimed at the target counts; one fired elsewhere
        -- (the shooter was already shooting at something) is a stray
        if math.abs(err) <= 4 then
          e.shot = frame
          phase, phase_t = "after", 0
        else
          e.stray = e.stray + 1
        end
      end
      prev_shells = t.shells
      if phase == "watch" and frame - e.E >= WATCH then
        phase, phase_t = "after", AFTER
      end
    elseif phase == "watch" then
      phase, phase_t = "after", AFTER
    end
    if phase == "after" and phase_t >= AFTER then
      close_event()
      park_target()
      if ev_i >= #OFFSETS * ROUNDS then finish(); return end
      home_shooter()
      phase, phase_t = "prep", 0
    end
  end
end
