-- lgm_kill_lag -- does GoalHunter kill a walking builder when its keys reach
-- the engine 3 brain ticks late?
--
-- The recorded case (session 20261121_195017_5__2v2_combined, bot 2 on a
-- remote Bot Host, brain ticks 42142-42194): the keys a think sent took
-- effect 3 brain ticks later. The kill-LGM crosshair search asked again for
-- sight steps that were already on the way, the sight swung 14 -> 2 -> 12 ->
-- 2, and both shells missed by about 500 wu.
--
-- WHAT IS REAL. The engine (WinBoloDS), the GoalHunter brain from this
-- checkout on its kill_lgm goal, the enemy builder (an engine builder on a
-- builder_order job, walking a straight line at 16 wu a brain tick), the
-- shells and the builder kill.
--
-- WHAT IS FAKED. The delay. A bot inside the server sees its keys one think
-- later (delay 1). TEST_KEY_DELAY_THINKS=2 (lgm_aim.note_sent, test only)
-- holds GoalHunter's own keys back 2 more thinks before the engine gets
-- them, so the delay is 3, as on the Bot Host. Two things differ from a
-- real remote seat: the world the bot sees is fresh (only its keys are
-- late), and the server rewinds the builder by 1 entry, not by the seat's
-- viewTick. So the man walks about 2 x 16 = 32 wu more than the brain's
-- lead says before the shell bursts; the kill reach is 128 wu.
--
-- THE ARENA. A grass field. The enemy tank (idle brain, team 1) parks on a
-- row 4 squares north of our tank and sends its builder 14 squares along
-- that row to an empty square (a road job). Our tank (team 2) starts near,
-- moving, with its nose 20 brads off the line to the man. TRIALS walks, the
-- direction and our tank's place change from walk to walk.
--
-- THE VERDICT. A walk is a FIRST-SHELL KILL when the builder dies to the
-- first shell our tank fires in that walk. A shell flies longer than the
-- reload, so a second shell is often in the air when the first one bursts.
-- The arena gives each shot its burst frame (the shot frame plus the engine
-- shell life for the sight at that frame, shellLifeTicks in shells.c) and
-- gives the kill to the shell whose burst frame is nearest the death frame.
-- PASS when at least NEED_FIRST of the TRIALS walks are first-shell kills
-- and at least NEED_KILLS walks end with the builder dead. Measured on
-- seeds 42, 7, 123, 999 and 2024 (2026-11-21): the new aim got 5-6
-- first-shell kills and 7-8 kills; the old aim got 0 first-shell kills
-- and 1-3 kills, mostly late shells after the man stopped walking.
--
-- lgm_kill_lag_old is the same arena with every LGM_AIM_* knob and
-- LGM_TELEPORT_REPEAT_VIEW off (the old aim): expected to fail.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open

local OUR_BRAIN = "../brains/GoalHunter/init.lua"
local FOE_BRAIN = "../tests/brains/idle.lua"
local US, FOE   = 0, 1
local PINS = "cfg=TANK_COMBAT_ENABLED=false;cfg=BUILDER_POOL_ENABLED=false;"
          .. "cfg=STRATEGIC_PLACE_ENABLED=false"
local KEY_DELAY = LKL_KEY_DELAY or 2
local EXTRA     = LKL_TOKENS or ""
local TRIALS     = LKL_TRIALS or 8
local NEED_FIRST = LKL_NEED_FIRST or 4
local NEED_KILLS = LKL_NEED_KILLS or 6
local WALK_FRAMES = 320           -- a walk of 14 squares at 16 wu a frame is 224
local SETTLE_FRAMES = 150
local NOSE_OFF = 20

-- dir: +1 the man walks east, -1 west. ox: our tank's column offset.
local LAYOUT = {
  { 1, -2 }, { -1, 2 }, { 1, 1 }, { -1, -1 }, { 1, 0 }, { -1, 0 }, { 1, 3 }, { -1, -3 },
  { 1, -1 }, { -1, 1 },
}

local frame = 0
local A = nil
local spawned_us, spawned_foe = false, false
local trial = 0
local phase = "settle"            -- settle, place, order, out, after
local phase_at = 0
local shots, prev_shells = 0, nil
local shot_log = {}               -- { frame, sight, burst, bx, by } for each shot of this walk
local man_at = {}                 -- frame -> { wx, wy } of the walking man
local prev_pose = nil             -- our tank at the end of the last frame
local died = nil
local first_kills, kills, results = 0, 0, {}

local function bearing(dx, dy)    -- bolo brads: 0 north, 64 east
  return math.floor(((math.atan2 or math.atan)(dx, -dy) * 128 / math.pi) % 256 + 0.5) % 256
end

function on_choose_start(g, p)
  if p == US then return 1 end
  if p == FOE then return 2 end
  return nil
end

function can_die(g, kind, n, killer, cause, pill)
  if kind == "tank" then return false end    -- the tanks never die
  return nil
end

function on_lgm_died(g, p, killer, mx, my, scripted)
  if p == FOE and phase == "out" and not died then
    died = { killer = killer, frame = frame, shots = shots }
  end
end

-- LKL_* globals are read here, not at load: a twin arena includes this file
-- first and sets them in its own body after.
local function tokens()
  local t = PINS .. ";cfg=TEST_KEY_DELAY_THINKS=" .. tostring(LKL_KEY_DELAY or KEY_DELAY)
  local extra = LKL_TOKENS or EXTRA
  if extra ~= "" then t = t .. ";" .. extra end
  return t
end

local function place(g)
  trial = trial + 1
  local L = LAYOUT[(trial - 1) % #LAYOUT + 1]
  local dir, ox = L[1], L[2]
  A.dir = dir
  A.fx, A.fy = A.cx - 7 * dir, A.cy - 4
  A.dx, A.dy = A.cx + 7 * dir, A.cy - 4
  local ux, uy = A.cx + ox, A.cy
  g.set_tile(A.dx, A.dy, g.TERRAIN.grass)
  g.teleport(FOE, A.fx, A.fy, 0)
  g.set_boat(FOE, false)
  local nose = (bearing((A.fx - ux) * 256, (A.fy - uy) * 256) + NOSE_OFF) % 256
  g.teleport(US, ux, uy, nose)
  g.set_boat(US, false)
  g.set_stocks(US, { shells = g.rule("tank_full_shells"), armour = g.rule("tank_full_armour"),
                     trees = 0, mines = 0 })
  g.set_stocks(FOE, { trees = 40, armour = g.rule("tank_full_armour") })
  g.set_speed(US, math.floor(g.rule("speed_grass") / 2))
  prev_shells, shots, died = nil, 0, nil
  shot_log = {}
  man_at = {}
end

-- shellLifeTicks(sight / 2, shell_life, shell_start_add), shells.c
local function rule_or(g, name, dflt)
  local ok, v = pcall(g.rule, name)
  if ok and type(v) == "number" then return v end
  return dflt
end

local function burst_frame(g, frame, sight)
  local life = 1 + math.floor(rule_or(g, "shell_life", 8) * sight / 2)
             - rule_or(g, "shell_start_add", 5)
  return frame + math.max(0, life)
end

-- which shot of this walk killed the man: the one whose burst is nearest
local function killing_shot()
  if not died or died.killer ~= US then return nil end
  local best, best_d = nil, math.huge
  for i, s in ipairs(shot_log) do
    local d = math.abs(s.burst - died.frame)
    if d < best_d then best, best_d = i, d end
  end
  return best, best_d
end

local function finish(g, why)
  local k, kd = killing_shot()
  local first = k == 1
  if died and died.killer == US then kills = kills + 1 end
  if first then first_kills = first_kills + 1 end
  local r = string.format("walk %d dir=%d: %s, %d shots", trial, A.dir,
    died and string.format("died f+%d killer=%s shell=%s (burst off %s)", died.frame - A.out_at,
                           tostring(died.killer), tostring(k), tostring(kd))
         or ("no kill (" .. why .. ")"), #shot_log)
  results[#results + 1] = first and "1" or (died and "k" or "-")
  g.log("LKL " .. r)
  -- one shot a line (a log line is one chat message long):
  -- frame after the order / sight at the shot / burst frame
  for i, s in ipairs(shot_log) do
    local m = man_at[s.burst] or man_at[s.burst - 1]
    if died and s.burst >= died.frame then m = man_at[died.frame - 1] end
    local miss = m and string.format("%.0f wu from the man", math.sqrt((s.bx - m[1]) ^ 2 + (s.by - m[2]) ^ 2))
                 or "man not out"
    g.log(string.format("LKL   shot %d f+%d sight %d burst f+%d %s", i, s.frame - A.out_at,
                        s.sight, s.burst - A.out_at, miss))
  end
end

function on_tick(g, tick)
  frame = frame + 1
  if A == nil then
    local s1 = g.start(1)
    if not s1 then return end
    A = { cx = s1.x, cy = s1.y - 6 }
  end
  if not spawned_us and frame >= 2 then
    spawned_us = true
    local u, err = g.spawn_bot{ slot = US, name = "Hunter", brain = OUR_BRAIN, team = 2,
                                start = 1, init = g.init_tokens(tokens()) }
    if u == nil then g.log("LKL spawn_bot(us) failed: " .. tostring(err)) end
    g.log("LKL tokens " .. tokens())
  end
  if not spawned_foe and frame >= 4 then
    spawned_foe = true
    local f, err = g.spawn_bot{ slot = FOE, name = "Walker", brain = FOE_BRAIN, team = 1,
                                start = 2 }
    if f == nil then g.log("LKL spawn_bot(foe) failed: " .. tostring(err)) end
  end
  if frame <= 240 then
    g.set_team(US, 2)
    g.set_team(FOE, 1)
  end
  local t, f = g.tank(US), g.tank(FOE)
  if not t or not f then return end

  if phase == "settle" then
    if frame == 60 then
      g.teleport(US, A.cx, A.cy, 0); g.set_boat(US, false)
      g.teleport(FOE, A.cx - 7, A.cy - 4, 0); g.set_boat(FOE, false)
    end
    if frame == 62 then
      g.fill_rect(A.cx - 14, A.cy - 12, A.cx + 14, A.cy + 3, g.TERRAIN.grass)
    end
    if frame >= SETTLE_FRAMES then phase, phase_at = "place", frame end
    return
  end

  -- shots: the shell count drops on the frame a shell leaves the gun
  if phase == "out" and not t.dead then
    if prev_shells and t.shells < prev_shells then
      shots = shots + (prev_shells - t.shells)
      -- The shell left at the start of this frame's tick, from the pose
      -- the tank had at the end of the last frame. It bursts 128 wu a
      -- sight step out along the nose (shells.c).
      local p0 = prev_pose or { wx = t.wx, wy = t.wy, dir = t.dir, sight = t.sight }
      local rad = p0.dir * math.pi / 128
      shot_log[#shot_log + 1] = { frame = frame, sight = p0.sight,
                                  burst = burst_frame(g, frame, p0.sight),
                                  bx = p0.wx + math.sin(rad) * 128 * p0.sight,
                                  by = p0.wy - math.cos(rad) * 128 * p0.sight }
    end
    prev_shells = t.shells
  end

  local b = g.builder(FOE)
  local bstate = b and b.state or "?"
  if phase == "out" and not died and b and bstate ~= "in_tank" and bstate ~= "dead" then
    man_at[frame] = { b.wx, b.wy }
  end
  prev_pose = { wx = t.wx, wy = t.wy, dir = t.dir, sight = t.sight }
  if phase == "place" then
    if bstate == "in_tank" then
      place(g)
      phase, phase_at = "order", frame
    elseif bstate == "dead" then
      g.builder_parachute(FOE)
    end
  elseif phase == "order" then
    if frame >= phase_at + 2 then
      g.builder_order(FOE, "road", A.dx, A.dy)
      phase, phase_at = "out", frame
      A.out_at = frame
      prev_shells = t.shells
    end
  elseif phase == "out" then
    if died then
      if frame >= died.frame + 60 then finish(g, "died"); phase, phase_at = "after", frame end
    elseif bstate == "returning" or (bstate == "in_tank" and frame > phase_at + 10) then
      finish(g, "reached the square"); phase, phase_at = "after", frame
    elseif frame >= phase_at + WALK_FRAMES then
      finish(g, "time"); g.builder_recall(FOE); phase, phase_at = "after", frame
    end
  elseif phase == "after" then
    if bstate == "dead" then g.builder_parachute(FOE) end
    if trial >= TRIALS then
      verdict(first_kills >= NEED_FIRST and kills >= NEED_KILLS, string.format(
        "first-shell kills %d/%d (need %d), kills %d (need %d) [%s]", first_kills,
        trial, NEED_FIRST, kills, NEED_KILLS, table.concat(results)))
      return
    end
    if bstate == "in_tank" and frame >= phase_at + 20 then phase, phase_at = "place", frame end
  end
  if tick >= GATE_TICKS - 300 then
    verdict(false, string.format("out of time after %d walks: first-shell kills %d [%s]",
      trial, first_kills, table.concat(results)))
  end
end
