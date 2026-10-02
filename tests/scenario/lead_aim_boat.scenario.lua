-- lead_aim_boat -- does GoalHunter lead a tank on a boat?
--
-- The same arena as lead_aim_land (read its header for the method and the
-- threshold), but the target rides a boat at boat speed (speed_boat, 16 wu a
-- frame) along a lane of river, then along open deep sea. The shooter stands
-- on its grass island as before.
--
--   boat_river  the lane is river, the target is on a boat
--   boat_sea    the lane is deep sea, the target is on a boat
--
-- THE BUG THIS ARENA SHOWS. GoalHunter predicts the target's motion from
-- the speed of the square under it (steering.lua, the engage lead block near
-- line 3342: C.MAP_SPEED[terrain] times the throttle, throttle clamped to 1).
-- C.MAP_SPEED is 3 for river and deep sea, but a tank on a boat moves at
-- speed_boat (16) there (bolo_map.c: an onBoat tank uses speed_boat). So the
-- bot leads a boat as if it crawled at 3 wu a frame: the lead is about a
-- fifth of what it needs and the shells pass behind the boat. Until that is
-- fixed the arena is expected to fail.
--
-- GATE: ticks=20000 bots=1 gametype=open ai=yesfull expect=fail GoalHunter leads a boat at C.MAP_SPEED[river/deep sea]=3, not speed_boat=16 (steering.lua engage lead ~3342)

-- KNOB VARIANT. A copy of this file with SHOOTER_CFG set hands the shooter
-- that one cfg= token through game.bot_init at frame 3, before any event.
local SHOOTER_CFG = nil
local cfg_sent = false

local SHOOTER = 0
local TARGET  = 1
local DUMMY   = "lead_dummy"

local CASES = {
  { name = "boat_river", tile = "river",    boat = true, moving = true, speed = 16, frames = 4000 },
  { name = "boat_sea",   tile = "deep_sea", boat = true, moving = true, speed = 16, frames = 4000 },
}

-- ======================================================================
-- Everything below is the same in lead_aim_land and lead_aim_boat.
-- ======================================================================

local SETTLE_FRAMES = 150        -- before the first case: the brain warms up
local GAP_FRAMES    = 100        -- between cases
local PASS_HALF     = 14         -- a pass starts 14 squares from the centre...
local PASS_TURN     = 12         -- ...and is cut 12 squares past it
local STILL_DX      = { -4, 0, 4, -2, 2, -5, 5, -1, 3, -3, 1 }
local STILL_HOLD    = 110        -- frames at each still point
local MIN_MEASURED  = 10

local frame = 0
local A = nil                    -- the anchor squares, from start 1
local spawned = false
local ci = 0                     -- case index; 0 = settling
local case_start = 0             -- frame the current case began
local gap_until = nil
local leg_dir = 64
local still_i = 0
local still_next = 0
local prev_shells = nil
local hist = {}                  -- seat -> last 8 positions {wx, wy}
local pending = {}               -- shots waiting for a hit or expiry
local stats = {}                 -- case name -> collected numbers
local resets = 0
local target_deaths = 0
local target_was_dead = false
local done = false

local R = {}                     -- the live rules

local function rule(n) return game.rule(n) end
local atan2 = math.atan2 or math.atan

local function brad(dx, dy)      -- bolo angle of a vector: 0 north, 64 east
  return (atan2(dx, -dy) * 128 / math.pi) % 256
end

local function adiff(a, b)       -- a - b in (-128, 128]
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

local function mean(t)
  if #t == 0 then return 0 end
  local s = 0
  for i = 1, #t do s = s + t[i] end
  return s / #t
end

local function push_hist(p, t)
  local h = hist[p] or {}
  h[#h + 1] = { wx = t.wx, wy = t.wy }
  if #h > 8 then table.remove(h, 1) end
  hist[p] = h
end

-- velocity per frame over the last n frames, and whether it held steady:
-- the first half and the second half of the window agree within 1.5 wu.
local function velocity(p, n)
  local h = hist[p]
  if not h or #h < n + 1 then return 0, 0, false end
  local a, m, b = h[#h - n], h[#h - n / 2], h[#h]
  local vx, vy = (b.wx - a.wx) / n, (b.wy - a.wy) / n
  local v1x, v1y = (m.wx - a.wx) / (n / 2), (m.wy - a.wy) / (n / 2)
  local v2x, v2y = (b.wx - m.wx) / (n / 2), (b.wy - m.wy) / (n / 2)
  local steady = math.abs(v1x - v2x) <= 1.5 and math.abs(v1y - v2y) <= 1.5
  return vx, vy, steady
end

local function intercept(sx, sy, tx, ty, vx, vy)
  local k = 0
  local px, py = tx, ty
  for _ = 1, 30 do
    local d = math.sqrt((px - sx) ^ 2 + (py - sy) ^ 2)
    k = math.max(0, d / R.speed - R.start_add)
    px, py = tx + vx * k, ty + vy * k
  end
  return px, py, k
end

local function stat(name)
  local s = stats[name]
  if not s then
    s = { shots = 0, hits = 0, measured = 0, mhits = 0, err = {}, abserr = {},
          range = {}, tv = {}, sv = {}, direct = {}, oor = 0 }
    stats[name] = s
  end
  return s
end

local function cur_case() return CASES[ci] end

local function lane_tile(c)
  return game.TERRAIN[c.tile]
end

local function place_target_pass(dir)
  leg_dir = dir
  local x = (dir == 64) and (A.cx - PASS_HALF) or (A.cx + PASS_HALF)
  game.teleport(TARGET, x, A.ly, dir)
  if cur_case().boat then game.set_boat(TARGET, true) end
end

local function place_target_still()
  still_i = still_i % #STILL_DX + 1
  game.teleport(TARGET, A.cx + STILL_DX[still_i], A.ly, 0)
  if cur_case().boat then game.set_boat(TARGET, true) end
  still_next = frame + STILL_HOLD
end

local function home_shooter()
  game.teleport(SHOOTER, A.cx, A.cy, 0)
  game.set_boat(SHOOTER, false)
  game.set_stocks(SHOOTER, { shells = R.full_shells, armour = R.full_armour,
                             trees = 0, mines = 0 })
end

local function start_case(i)
  ci = i
  case_start = frame
  local c = CASES[ci]
  game.fill_rect(A.x0, A.ly - 1, A.x1, A.ly + 1, lane_tile(c))
  if c.tile == "deep_sea" then
    -- game.teleport will not put a tank on deep sea, so the squares where a
    -- pass starts and where the target parks stay river ("docks"). They are
    -- 14 and more squares from the island, outside the shooter's range.
    for _, x in ipairs({ A.cx - PASS_HALF, A.cx + PASS_HALF, A.x0 + 1 }) do
      game.set_tile(x, A.ly, game.TERRAIN.river)
    end
  end
  hist[TARGET] = nil
  if c.moving then place_target_pass(64) else still_i = 0; place_target_still() end
  game.log(string.format("LEAD case %s begins f=%d lane=%s boat=%s", c.name,
                         frame, c.tile, tostring(c.boat)))
end

-- a resolved shot: log it, and add it to its case's numbers
local function resolve(sh)
  local s = stat(sh.case)
  s.shots = s.shots + 1
  if sh.hit then s.hits = s.hits + 1 end
  if sh.measured then
    s.measured = s.measured + 1
    if sh.hit then s.mhits = s.mhits + 1 end
    s.err[#s.err + 1] = sh.err
    s.abserr[#s.abserr + 1] = math.abs(sh.err)
    s.range[#s.range + 1] = sh.range
    s.tv[#s.tv + 1] = sh.tv
    s.sv[#s.sv + 1] = sh.sv
    s.direct[#s.direct + 1] = sh.direct
  elseif sh.oor then
    s.oor = s.oor + 1
  end
  game.log(string.format("SHOT %s f=%d d=%.1f tv=%.1f sv=%.1f k=%.1f ideal=%.1f fired=%.1f err=%+.1f direct=%+.1f m=%d hit=%d",
    sh.case, sh.f, sh.range / 256, sh.tv, sh.sv, sh.k, sh.ideal, sh.fired, sh.err,
    sh.direct, sh.measured and 1 or 0, sh.hit and 1 or 0))
end

local function on_shot(n)
  local c = cur_case()
  local t, u = game.tank(SHOOTER), game.tank(TARGET)
  if not c or not t or not u or u.dead then return end
  local vx, vy, steady = velocity(TARGET, 4)
  local svx, svy = velocity(SHOOTER, 4)
  local tv = math.sqrt(vx * vx + vy * vy)
  local px, py, k = intercept(t.wx, t.wy, u.wx, u.wy, vx, vy)
  local ideal = brad(px - t.wx, py - t.wy)
  local fired = (t.dir + 0.5) % 256
  local raw = adiff(fired, ideal)
  -- sign by the target's motion: where does the shell pass, relative to the
  -- target at the meeting frame, along the target's direction of travel?
  local err = raw
  if tv >= 1 then
    local rad = fired * math.pi / 128
    local qx = t.wx + math.sin(rad) * (R.start_add + k) * R.speed
    local qy = t.wy - math.cos(rad) * (R.start_add + k) * R.speed
    local along = ((qx - px) * vx + (qy - py) * vy) / tv
    err = (along >= 0) and math.abs(raw) or -math.abs(raw)
  end
  local direct = adiff(fired, brad(u.wx - t.wx, u.wy - t.wy))
  local oor = k > R.life_ticks
  local moving_ok = c.moving and steady and tv >= 0.8 * c.speed
  local still_ok = (not c.moving) and tv < 1
  for _ = 1, n do
    pending[#pending + 1] = {
      case = c.name, f = frame, k = k, ideal = ideal, fired = fired, err = err,
      direct = direct, tv = tv, sv = math.sqrt(svx * svx + svy * svy),
      range = math.sqrt((px - t.wx) ^ 2 + (py - t.wy) ^ 2),
      measured = (moving_ok or still_ok) and not oor, oor = oor, hit = false,
    }
  end
end

local function expire_pending()
  local keep = {}
  for _, sh in ipairs(pending) do
    if frame - sh.f > R.life_ticks + 6 then resolve(sh) else keep[#keep + 1] = sh end
  end
  pending = keep
end

function on_tank_hit(victim, attacker, cause, amount, pill, scripted)
  if victim ~= TARGET or attacker ~= SHOOTER or cause ~= "shell" then return end
  local best, bi = nil, nil
  for i, sh in ipairs(pending) do
    if not sh.hit then
      local age = frame - sh.f
      if age <= R.life_ticks + 4 then
        local miss = math.abs(age - sh.k)
        if best == nil or miss < best then best, bi = miss, i end
      end
    end
  end
  if bi then pending[bi].hit = true end
end

local function report_case(c)
  local s = stat(c.name)
  local med = median(s.err)
  local amed = median(s.abserr)
  local rng = median(s.range)
  local tol = (rng > 0) and (math.atan(R.hit_radius / rng) * 128 / math.pi) or 0
  local tv = median(s.tv)
  game.log(string.format("LEAD %s: shots=%d hits=%d rate=%.0f%% measured=%d mhits=%d oor=%d",
    c.name, s.shots, s.hits, 100 * s.hits / math.max(1, s.shots), s.measured, s.mhits, s.oor))
  game.log(string.format("LEAD %s: err mean=%+.1f median=%+.1f |err| median=%.1f tol=%.1f R=%.1fsq tv=%.1f sv=%.1f direct=%+.1f",
    c.name, mean(s.err), med, amed, tol, rng / 256, tv, median(s.sv), median(s.direct)))
  local ok, why = true, nil
  if s.measured < MIN_MEASURED then
    ok, why = false, string.format("%s: %d measured shots (< %d)", c.name, s.measured, MIN_MEASURED)
  elseif c.moving and math.abs(tv - c.speed) > 0.2 * c.speed then
    ok, why = false, string.format("%s: target ran at %.1f, not %d (arena broken)", c.name, tv, c.speed)
  elseif amed > tol then
    ok, why = false, string.format("%s: median |err| %.1f > %.1f brads (median signed %+.1f)",
                                   c.name, amed, tol, med)
  end
  return ok, why, string.format("%s %d/%d med%+.1f", c.name, s.hits, s.shots, med)
end

local function finish()
  done = true
  for _, sh in ipairs(pending) do resolve(sh) end
  pending = {}
  local all_ok, first_why, parts = true, nil, {}
  for _, c in ipairs(CASES) do
    local ok, why, part = report_case(c)
    parts[#parts + 1] = part
    if not ok and all_ok then all_ok, first_why = false, why end
  end
  game.log(string.format("LEAD shooter resets=%d target deaths=%d", resets, target_deaths))
  verdict(all_ok, all_ok and table.concat(parts, "; ") or first_why)
end

-- The target never dies: a shell that would kill it, and the deep sea a
-- knock pushes it into, leave it at zero armour, alive, and the script
-- tops its armour up again.
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
  if done then return end
  frame = frame + 1
  if A == nil then
    local s1 = g.start(1)
    if not s1 then return end
    A = { cx = s1.x, cy = s1.y - 6, ly = s1.y - 12, x0 = s1.x - 26, x1 = s1.x + 26 }
    R.speed = rule("shell_speed")
    R.start_add = rule("shell_start_add")
    R.life_ticks = 1 + rule("shell_life") * 7 - R.start_add
    R.hit_radius = rule("tank_hit_radius")
    R.full_shells = rule("tank_full_shells")
    R.full_armour = rule("tank_full_armour")
    game.log(string.format("LEAD rules shell_speed=%d start_add=%d life=%d hit_radius=%d island=(%d,%d) lane_y=%d",
      R.speed, R.start_add, R.life_ticks, R.hit_radius, A.cx, A.cy, A.ly))
  end
  if not spawned and frame >= 2 then
    spawned = true
    g.spawn_bot{ slot = TARGET, name = "Target", brain = DUMMY, team = 1, start = 2 }
  end

  if SHOOTER_CFG and not cfg_sent and frame >= 3 then
    cfg_sent = true
    local ok, code = g.bot_init(SHOOTER, { cfg = SHOOTER_CFG })
    g.log("KNOB cfg=" .. SHOOTER_CFG .. " bot_init " .. tostring(ok) .. " " .. tostring(code))
  end
  local t, u = g.tank(SHOOTER), g.tank(TARGET)
  if not t or not u then return end

  -- keep the shooter home, fed and on its island
  if t.dead then
    prev_shells = nil
  else
    local off = math.abs(t.mx - A.cx) > 2 or math.abs(t.my - A.cy) > 2
    if off or t.boat then
      home_shooter()
      resets = resets + 1
      prev_shells = nil
      hist[SHOOTER] = nil
      return
    end
    push_hist(SHOOTER, t)
  end
  if not u.dead then push_hist(TARGET, u) end

  -- shell knockback can push the target off the 3-row lane into the sea,
  -- where it drowns. Count it, and put it back on its case when it respawns.
  if u.dead then
    if not target_was_dead and ci > 0 then target_deaths = target_deaths + 1 end
    target_was_dead = true
  elseif target_was_dead then
    target_was_dead = false
    hist[TARGET] = nil
    local c = cur_case()
    if c and not gap_until then
      if c.moving then place_target_pass(leg_dir) else place_target_still() end
    elseif c then
      game.teleport(TARGET, A.x0 + 1, A.ly, 0)
    end
  end

  if ci == 0 then
    if frame == 3 then home_shooter() end
    if frame >= SETTLE_FRAMES then start_case(1) end
    return
  end

  -- shots: the shell count drops on the frame a shell leaves the gun
  if not t.dead then
    if prev_shells and t.shells < prev_shells and not gap_until then
      on_shot(prev_shells - t.shells)
    end
    prev_shells = t.shells
    if t.shells < R.full_shells - 15 then
      g.set_stocks(SHOOTER, { shells = R.full_shells, armour = R.full_armour })
      prev_shells = nil
    end
  end
  expire_pending()

  -- keep the target alive, afloat in the boat case, and on its pass
  if not u.dead then
    if u.armour < R.full_armour - 10 then g.set_stocks(TARGET, { armour = R.full_armour }) end
    local c = cur_case()
    if c and c.boat and not u.boat then g.set_boat(TARGET, true) end
  end

  if gap_until then
    if frame >= gap_until then
      gap_until = nil
      if ci + 1 > #CASES then finish() else start_case(ci + 1) end
    end
    return
  end

  local c = cur_case()
  if frame - case_start >= c.frames then
    gap_until = frame + GAP_FRAMES
    game.teleport(TARGET, A.x0 + 1, A.ly, 0)   -- parked, still, out of sight
    if c.boat then game.set_boat(TARGET, true) end
    return
  end
  if c.moving then
    if not u.dead then
      local dx = u.wx / 256 - A.cx
      if leg_dir == 64 and dx > PASS_TURN then place_target_pass(192)
      elseif leg_dir == 192 and dx < -PASS_TURN then place_target_pass(64) end
    end
  elseif frame >= still_next then
    place_target_still()
  end
end
