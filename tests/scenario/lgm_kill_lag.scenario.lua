-- lgm_kill_lag -- does GoalHunter kill a builder on a job when its keys
-- reach the engine 3 brain ticks late?
--
-- The recorded case (session 20261121_195017_5__2v2_combined, bot 2 on a
-- remote Bot Host, brain ticks 42142-42194): the keys a think sent took
-- effect 3 brain ticks later. The kill-LGM crosshair search asked again for
-- sight steps that were already on the way, the sight swung 14 -> 2 -> 12 ->
-- 2, and both shells missed by about 500 wu.
--
-- WHAT IS REAL. The engine (WinBoloDS), the GoalHunter brain from this
-- checkout on its kill_lgm goal, the enemy builder (an engine builder on a
-- builder_order job: he walks out at 16 wu a brain tick, stops and works for
-- lgm_build_ticks, and walks back the way he came), the shells and the
-- builder kill.
--
-- WHAT IS FAKED. The delay. A bot inside the server sees its keys one think
-- later (delay 1). TEST_KEY_DELAY_THINKS=2 (lgm_aim.note_sent, test only)
-- holds GoalHunter's own keys back 2 more thinks before the engine gets
-- them, so the delay is 3, as on the Bot Host. Two things differ from a
-- real remote seat: the world the bot sees is fresh (only its keys are
-- late), and the server rewinds the builder by 1 entry, not by the seat's
-- viewTick. LKL_KEY_DELAY = 0 is the bot in the server (delay 1).
--
-- THE ARENA. A grass field. The enemy tank (idle brain, team 1) parks on a
-- row 4 squares north of our tank and sends its builder 14 squares along
-- that row to an empty square (a road job; a building job with LKL_PILL).
-- Our tank (team 2) starts near, moving, with
-- its nose 20 brads off the line to the man. TRIALS jobs; the direction and
-- our tank's place change from job to job. A job ends when the man dies, is
-- back in his tank, or WALK_FRAMES pass.
--
-- LKL_ROW, LKL_WALK, LKL_OX_MAX (the _near twins): his row is LKL_ROW squares
-- north of our tank (4), he walks LKL_WALK squares each side of the centre
-- column (7), and our tank's column offset is clamped to LKL_OX_MAX (no
-- clamp). The shell lines and the fire-range line give the tank-to-man
-- distance as each shell leaves.
--
-- LKL_ARRIVE: our tank has no shells while he walks out, and is put in its
-- place, nose 20 brads off, with full shells on the frame he stops. Every
-- shot is then in the stop or the walk back (C.LGM_LEAD_STOP_RETURN).
--
-- LKL_PILL: on the frame he stops, his wall is taken away and a live
-- pillbox is put on his work square, under him, as when a man puts down a
-- pillbox and stands on it (play test 20261121_231141_1, bot 2, tick 4888).
-- The job is a building job so that his square is blessed and he can walk
-- off the pillbox (lgm.c). The pillbox is our tank's seat's, so the bot has
-- no reason to fear it or to take it, and can_target keeps it quiet. Our
-- shells still hit it. It is taken off when the job ends.
--
-- THE SHELLS. game.shells() tracks every shell from the frame it shows to
-- the frame it is gone. A shell that runs out is seen for L frames and
-- bursts where it was last seen (shells_probe.scenario.lua); its burst is
-- frame last-seen + 1. Its sight is the one whose L is the frames it was
-- seen (the sight at the end of the last frame or this one). For each such
-- shell the arena compares how far from the gun it burst, along its own
-- path, with the brain's model of that distance: speed x (start_add + L)
-- with C.LGM_SHELL_MODEL_RULES on (128 x sight + 32 on the classic rules),
-- 128 x sight with it off. The gun is our tank at the end of the last
-- frame (it moves under 8 wu a frame here). The man a burst is measured to
-- is where he was on the frame before the burst (the builder test is one
-- tick back, shells.c SHELL_LGM_REWIND_LESS).
--
-- THE SHELL SOURCE (2026-11-22). The brain tells the arena in chat which
-- code pressed fire on each think (cfg=TEST_SHOT_SOURCE_SAY, lgm_aim
-- note_shot_source): k = the kill-LGM fire check, t / n = a drive-by tank
-- shot (init.lua / steering.lua), o = other. A shell first seen on frame f
-- was pressed on think f - LKL_KEY_DELAY. Each shell line shows its source.
-- The job counts, the phase tallies and the fire range count only kill-LGM
-- shells (k); a kill by any other shell is a "d" in the job list. A shell
-- with no source told is "?" and is counted as a kill-LGM shell.
--
-- THE REWIND PROBE (2026-11-22). Each shell line also gives "scr": the burst
-- to the man on the burst frame, which is what a bot in the server sees.
-- "man" is the burst to the man one frame before, where the server tests
-- him. On the frame he dies his place is his last step carried on. The end line counts the kill-LGM shells where the two places give a
-- different answer (within 128 wu or not) and which one the death agreed
-- with.
--
-- THE VERDICT. A job is a FIRST-SHELL KILL when the builder dies to the
-- first kill-LGM shell our tank fires in that job; the kill goes to the
-- shell whose burst frame is nearest the death frame. PASS when at least NEED_FIRST of
-- the TRIALS jobs are first-shell kills, at least NEED_KILLS jobs end with
-- the builder dead, every shell that ran out burst within 16 wu of the
-- model burst, and (LKL_PILL) no shell hit the pillbox on his
-- square without killing him (C.LGM_FIRE_LOS_STRICT).
--
-- The twins: lgm_kill_lag_old (every LGM_AIM_* knob, LGM_TELEPORT_REPEAT_VIEW,
-- LGM_SHELL_MODEL_RULES, LGM_FIRE_LOS_STRICT, LGM_LEAD_STOP_RETURN and the
-- two DRIVEBY_* knobs off),
-- _d1 / _d1_old (delay 1), _stop / _stop_old (LKL_ARRIVE), _pill (LKL_PILL
-- and LKL_ARRIVE, no old twin: see its header), _near / _near_old (the man
-- 2-3.6 squares from our tank). The _old twins are
-- expected to fail.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open

local OUR_BRAIN = "../brains/GoalHunter/init.lua"
local FOE_BRAIN = "../tests/brains/idle.lua"
local US, FOE   = 0, 1
local PINS = "cfg=TANK_COMBAT_ENABLED=false;cfg=BUILDER_POOL_ENABLED=false;"
          .. "cfg=STRATEGIC_PLACE_ENABLED=false"
local WALK_FRAMES = 640           -- out 224, work lgm_build_ticks, back 224
local SETTLE_FRAMES = 150
local NOSE_OFF = 20
local MODEL_TOL = 16              -- real burst to the model burst, wu

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
local shots = {}                  -- this job's shells, in fire order
local tracks = {}                 -- shells in the air: index into shots
local man_at, man_ph = {}, {}     -- frame -> { wx, wy } / "walk" "stop" "back"
local prev_pose = nil             -- our tank at the end of the last frame
local prev_man = nil
local arrived = nil               -- frame he stopped
local died = nil
local pill_n, pill_armour = nil, nil
local first_kills, kills, results = 0, 0, {}
local model_worst, model_n, model_bad = 0, 0, 0
local pill_wasted = 0
local ph_shots = { walk = 0, stop = 0, back = 0 }
local ph_hits = { walk = 0, stop = 0, back = 0 }
local sb_shots, sb_hits = 0, 0
local fire_n, fire_min, fire_max = 0, math.huge, 0   -- tank to man at fire, squares
local src_by_tick = {}            -- brain think -> shot source code (on_chat)
local src_n = {}                  -- shells by source
local drive_kills = 0             -- jobs ended by a shell that was not kill-LGM
-- the rewind probe: kill-LGM shells where the server place (man) and the
-- present place (scr) disagree, and which one the death agreed with
local rw_split, rw_srv_ok, rw_scr_ok = 0, 0, 0

local function opt(name, dflt)
  local v = _G[name]
  if v == nil then return dflt end
  return v
end

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

-- The pillbox he puts down never shoots our tank.
function can_target(g, pill, p)
  return false
end

-- The brain's shot source line: "shotsrc <tick>[-<tick>]:<code> ...".
function on_chat(g, p, text, scripted)
  if p ~= US or scripted then return end
  local body = text:match("shotsrc (.*)$")
  if not body then return end
  for a, b, c in body:gmatch("(%d+)%-?(%d*):(%a)") do
    a = tonumber(a); b = tonumber(b) or a
    for t = a, b do src_by_tick[t] = c end
  end
end

function on_lgm_died(g, p, killer, mx, my, scripted)
  if p == FOE and phase == "out" and not died then
    died = { killer = killer, frame = frame }
  end
end

-- LKL_* globals are read here, not at load: a twin arena includes this file
-- first and sets them in its own body after.
local function tokens()
  local t = PINS .. ";cfg=TEST_KEY_DELAY_THINKS=" .. tostring(opt("LKL_KEY_DELAY", 2))
          .. ";cfg=TEST_SHOT_SOURCE_SAY=true"
  local extra = opt("LKL_TOKENS", "")
  if extra ~= "" then t = t .. ";" .. extra end
  return t
end

local function rule_or(g, name, dflt)
  local ok, v = pcall(g.rule, name)
  if ok and type(v) == "number" then return v end
  return dflt
end

-- The engine's shell (tank.c, shells.c): L steps of speed after start_add.
local function engine_shell(g, sight)
  local speed = rule_or(g, "shell_speed", 32)
  local add = rule_or(g, "shell_start_add", 5)
  local L = math.max(0, 1 + math.floor(rule_or(g, "shell_life", 8) * sight / 2) - add)
  return speed * (add + L), L
end

-- The burst the brain's model gives (kill_lgm.shell_travel).
local function model_travel(g, sight)
  if tokens():find("LGM_SHELL_MODEL_RULES=false", 1, true) then return 128 * sight end
  return (engine_shell(g, sight))
end

local function our_place(L)
  local ox, m = L[2], opt("LKL_OX_MAX", nil)
  if m then ox = math.max(-m, math.min(m, ox)) end
  return A.cx + ox, A.cy
end

local function put_us(g, full)
  local L = LAYOUT[(trial - 1) % #LAYOUT + 1]
  local ux, uy = our_place(L)
  local mx = (prev_man and prev_man[1] or (A.fx * 256 + 128)) / 256
  local my = (prev_man and prev_man[2] or (A.fy * 256 + 128)) / 256
  local nose = (bearing((mx - ux) * 256, (my - uy) * 256) + NOSE_OFF) % 256
  g.teleport(US, ux, uy, nose)
  g.set_boat(US, false)
  g.set_stocks(US, { shells = full and g.rule("tank_full_shells") or 0,
                     armour = g.rule("tank_full_armour"), trees = 0, mines = 0 })
  g.set_speed(US, math.floor(g.rule("speed_grass") / 2))
end

local function place(g)
  trial = trial + 1
  local L = LAYOUT[(trial - 1) % #LAYOUT + 1]
  local dir = L[1]
  A.dir = dir
  local row, walk = opt("LKL_ROW", 4), opt("LKL_WALK", 7)
  A.fx, A.fy = A.cx - walk * dir, A.cy - row
  A.dx, A.dy = A.cx + walk * dir, A.cy - row
  g.set_tile(A.dx, A.dy, g.TERRAIN.grass)
  g.teleport(FOE, A.fx, A.fy, 0)
  g.set_boat(FOE, false)
  prev_man = nil
  put_us(g, not opt("LKL_ARRIVE", false))
  g.set_stocks(FOE, { trees = 40, armour = g.rule("tank_full_armour") })
  shots = {}
  man_at, man_ph = {}, {}
  arrived, died, pill_armour = nil, nil, nil
end

-- The source of a shot: the brain's code for the think that pressed fire.
local function src_of(s)
  return src_by_tick[s.frame - opt("LKL_KEY_DELAY", 2)] or "?"
end
local function is_kill_shot(s) local c = src_of(s); return c == "k" or c == "?" end

-- the shot that killed the man: the one whose burst frame is nearest
local function killing_shot()
  if not died or died.killer ~= US then return nil end
  local best, best_d = nil, math.huge
  for i, s in ipairs(shots) do
    if s.burst then
      local d = math.abs(s.burst - died.frame)
      if d < best_d then best, best_d = i, d end
    end
  end
  return best, best_d
end

local function finish(g, why)
  if pill_n then g.remove_pill(pill_n); pill_n = nil end
  local k, kd = killing_shot()
  -- the first kill-LGM shell of the job
  local first_k = nil
  for i, s in ipairs(shots) do
    if is_kill_shot(s) then first_k = i; break end
  end
  local by_k = k and is_kill_shot(shots[k])
  local first = k ~= nil and k == first_k
  if died and died.killer == US then
    if k and not by_k then drive_kills = drive_kills + 1 else kills = kills + 1 end
  end
  if first then first_kills = first_kills + 1 end
  local dph = died and (man_ph[died.frame - 1] or man_ph[died.frame]) or "-"
  results[#results + 1] = first and "1" or (died and (by_k == false and "d" or "k") or "-")
  g.log(string.format("LKL job %d dir=%d: %s, %d shots", trial, A.dir,
    died and string.format("died f+%d in %s killer=%s shell=%s (burst off %s)",
                           died.frame - A.out_at, dph, tostring(died.killer),
                           tostring(k), tostring(kd))
         or ("no kill (" .. why .. ")"), #shots))
  -- One shell a line (a log line is one chat message long, 128 bytes):
  -- fire frame after the order, man phase at the burst, sight, real burst
  -- to the model burst, real burst to the man.
  for i, s in ipairs(shots) do
    local src = src_of(s)
    local ks = is_kill_shot(s)
    src_n[src] = (src_n[src] or 0) + 1
    local fm = man_at[s.frame - 1] or man_at[s.frame]
    if fm and not s.stray and ks then
      local r = math.sqrt((fm[1] - s.gx) ^ 2 + (fm[2] - s.gy) ^ 2) / 256
      fire_n = fire_n + 1
      if r < fire_min then fire_min = r end
      if r > fire_max then fire_max = r end
      s.range = r
    end
    local ph = s.burst and (man_ph[s.burst - 1] or man_ph[s.burst]) or "?"
    local m = s.burst and (man_at[s.burst - 1] or man_at[s.burst])
    local man_d = (m and s.rx) and math.sqrt((s.rx - m[1]) ^ 2 + (s.ry - m[2]) ^ 2)
    -- On the burst frame of the shell that killed him he is no longer
    -- recorded: his place then is one more step of his last move.
    local mn = s.burst and man_at[s.burst]
    if not mn and m and s.burst then
      local m2 = man_at[s.burst - 2]
      if m2 then mn = { 2 * m[1] - m2[1], 2 * m[2] - m2[2] } end
    end
    local scr_d = (mn and s.rx) and math.sqrt((s.rx - mn[1]) ^ 2 + (s.ry - mn[2]) ^ 2)
    if ks and man_d and scr_d and ((man_d <= 128) ~= (scr_d <= 128)) then
      rw_split = rw_split + 1
      local dead = (k == i)
      if (man_d <= 128) == dead then rw_srv_ok = rw_srv_ok + 1 end
      if (scr_d <= 128) == dead then rw_scr_ok = rw_scr_ok + 1 end
    end
    if not ks then
      -- not a kill-LGM shell: shown, not counted
    elseif ph_shots[ph] then
      ph_shots[ph] = ph_shots[ph] + 1
      if man_d and man_d <= 128 then ph_hits[ph] = ph_hits[ph] + 1 end
    end
    -- Fired while he stands, burst once he walks back: the shells the
    -- stop-and-return lead is for.
    local fph = man_ph[s.frame - 1] or man_ph[s.frame] or "?"
    if ks and fph == "stop" and ph == "back" then
      sb_shots = sb_shots + 1
      if man_d and man_d <= 128 then sb_hits = sb_hits + 1 end
    end
    -- man: burst to the man, and (dx, dy) the burst less the man, wu
    g.log(string.format("LKL  shell %d %s f+%d r%s %s>%s sight %d %s model %s man %s%s scr %s%s", i, src,
      s.frame - A.out_at, s.range and string.format("%.1f", s.range) or "-",
      fph, ph, s.sight, s.expired and "ran out" or "hit",
      s.model_err and string.format("%+.0f", s.model_err) or "-",
      man_d and string.format("%.0f", man_d) or "-",
      man_d and string.format(" (%+.0f,%+.0f)", s.rx - m[1], s.ry - m[2]) or "",
      scr_d and string.format("%.0f", scr_d) or "-",
      s.pill and " PILL" or ""))
  end
end

-- The sight whose engine L is n frames: the sight at the end of the last
-- frame or at this one (a key can move it on the frame the shell leaves).
local function sight_of(g, n, s0, s1)
  for _, sl in ipairs({ s0, s1 }) do
    local _, L = engine_shell(g, sl)
    if L == n then return sl end
  end
  return nil
end

-- Track the shells. A shell this frame is the track whose last place is
-- within 48 wu of it (one 32 wu step); else it is new, fired from the pose
-- our tank had at the end of the last frame (only our tank fires here).
-- Shells fired between jobs are tracked too, so they never look new, but
-- only a shell fired while he is out is one of this job's shots.
local function track_shells(g, t)
  local live = g.shells()
  local seen = {}
  for _, sh in ipairs(live) do
    local best, bd = nil, 48
    for i, tr in ipairs(tracks) do
      if not seen[i] then
        local d = math.sqrt((sh.wx - tr.x) ^ 2 + (sh.wy - tr.y) ^ 2)
        if d <= bd then best, bd = i, d end
      end
    end
    if best then
      local tr = tracks[best]
      tr.x, tr.y, tr.n, tr.last = sh.wx, sh.wy, tr.n + 1, frame
      seen[best] = true
    else
      local p0 = prev_pose or { wx = t.wx, wy = t.wy, dir = t.dir, sight = t.sight }
      local s = { frame = frame, sight = p0.sight, sight1 = t.sight, gx = p0.wx, gy = p0.wy,
                  angle = sh.angle }
      -- A shell that is not this job's, or that left on a frame the arena
      -- teleported our tank (the gun is then not where the pose says), is
      -- tracked but not measured.
      s.stray = phase ~= "out"
                or math.sqrt((t.wx - p0.wx) ^ 2 + (t.wy - p0.wy) ^ 2) > 32
      if phase == "out" then shots[#shots + 1] = s end
      tracks[#tracks + 1] = { x = sh.wx, y = sh.wy, n = 1, last = frame, shot = s }
      seen[#tracks] = true
    end
  end
  for i = #tracks, 1, -1 do
    if not seen[i] then
      local tr = table.remove(tracks, i)
      local s = tr.shot
      s.rx, s.ry, s.burst = tr.x, tr.y, tr.last + 1
      local sl = sight_of(g, tr.n, s.sight, s.sight1)
      s.expired = sl ~= nil
      if sl then
        s.sight = sl
        -- Along the shell's own path (its angle, brads, 0 north): + when it
        -- burst past the model burst.
        local rad = (s.angle or 0) * math.pi / 128
        local along = (tr.x - s.gx) * math.sin(rad) - (tr.y - s.gy) * math.cos(rad)
        s.model_err = along - model_travel(g, sl)
        if not s.stray then
          local e = math.abs(s.model_err)
          model_n = model_n + 1
          if e > model_worst then model_worst = e end
          if e > MODEL_TOL then model_bad = model_bad + 1 end
        end
      end
    end
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
      g.teleport(FOE, A.cx - opt("LKL_WALK", 7), A.cy - opt("LKL_ROW", 4), 0)
      g.set_boat(FOE, false)
    end
    if frame == 62 then
      g.fill_rect(A.cx - 14, A.cy - 12, A.cx + 14, A.cy + 3, g.TERRAIN.grass)
    end
    if frame >= SETTLE_FRAMES then phase, phase_at = "place", frame end
    return
  end

  track_shells(g, t)

  local b = g.builder(FOE)
  local bstate = b and b.state or "?"
  local out = b and bstate ~= "in_tank" and bstate ~= "dead"
  if phase == "out" and not died and out then
    man_at[frame] = { b.wx, b.wy }
    -- lgm.c: on arrival the state is RETURN with waitTime = lgm_build_ticks;
    -- he stands until it runs out, then walks back.
    local still = prev_man and prev_man[1] == b.wx and prev_man[2] == b.wy
    if bstate == "returning" and still and not arrived then
      arrived = frame - 1
      man_ph[frame - 1] = "stop"
      if opt("LKL_PILL", false) then
        -- His wall goes up as he arrives; a pillbox cannot stand on a wall.
        g.set_tile(A.dx, A.dy, g.TERRAIN.grass)
        local n, err = g.add_pill(A.dx, A.dy, US, rule_or(g, "pill_max_armour", 15))
        if n then pill_n = n else g.log("LKL add_pill failed: " .. tostring(err)) end
        pill_armour = nil
      end
      if opt("LKL_ARRIVE", false) then put_us(g, true) end
    end
    man_ph[frame] = (bstate ~= "returning") and "walk" or (still and "stop" or "back")
    prev_man = { b.wx, b.wy }
  end
  -- The pillbox on his square: a hit on it while he stands there that does
  -- not kill him is a shell the line-of-sight check should have stopped.
  if pill_n and phase == "out" then
    local pi = g.pill(pill_n)
    if pi and not pi.in_tank then
      if pill_armour and pi.armour < pill_armour and out and not died
            and b.mx == pi.x and b.my == pi.y then
        pill_wasted = pill_wasted + (pill_armour - pi.armour)
        for i = #shots, 1, -1 do
          if shots[i].burst == frame then shots[i].pill = true; break end
        end
      end
      pill_armour = pi.armour
    end
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
      -- LKL_PILL: a building job, so his work square is blessed (lgm.c
      -- lgmNewPrimaryRequest) and he can walk off it with the pillbox on it,
      -- as from a pillbox he built. On a road job on grass he could not.
      g.builder_order(FOE, opt("LKL_PILL", false) and "building" or "road", A.dx, A.dy)
      phase, phase_at = "out", frame
      A.out_at = frame
    end
  elseif phase == "out" then
    if died then
      if frame >= died.frame + 60 then finish(g, "died"); phase, phase_at = "after", frame end
    elseif bstate == "in_tank" and frame > phase_at + 10 then
      finish(g, "back in his tank"); phase, phase_at = "after", frame
    elseif frame >= phase_at + WALK_FRAMES then
      finish(g, "time"); g.builder_recall(FOE); phase, phase_at = "after", frame
    end
  elseif phase == "after" then
    if bstate == "dead" then g.builder_parachute(FOE) end
    if trial >= opt("LKL_TRIALS", 8) and #tracks == 0 then
      local need_first, need_kills = opt("LKL_NEED_FIRST", 4), opt("LKL_NEED_KILLS", 6)
      local pill = opt("LKL_PILL", false)
      g.log(string.format("LKL shells by man phase at the burst (within 128 wu / shells): "
        .. "walk %d/%d stop %d/%d back %d/%d", ph_hits.walk, ph_shots.walk,
        ph_hits.stop, ph_shots.stop, ph_hits.back, ph_shots.back))
      g.log(string.format("LKL fired in the stop, burst in the walk back: %d/%d within 128 wu",
        sb_hits, sb_shots))
      g.log(string.format("LKL fire range (tank to man as the shell leaves): %d shells, %.1f-%.1f squares",
        fire_n, fire_n > 0 and fire_min or 0, fire_max))
      local sn = {}
      for c, n in pairs(src_n) do sn[#sn + 1] = c .. "=" .. n end
      table.sort(sn)
      g.log(string.format("LKL shell sources: %s; kills by other shells %d",
        table.concat(sn, " "), drive_kills))
      g.log(string.format("LKL rewind probe: %d kill-LGM shells split (man/scr); death agrees with man %d, scr %d",
        rw_split, rw_srv_ok, rw_scr_ok))
      -- (a verdict line is cut at VERDICT_MAX, so the words are short)
      verdict(first_kills >= need_first and kills >= need_kills and model_bad == 0
              and (not pill or pill_wasted == 0), string.format(
        "first %d/%d (need %d) kills %d (need %d) [%s] model>%dwu %d/%d worst %.0f%s",
        first_kills, trial, need_first, kills, need_kills, table.concat(results),
        MODEL_TOL, model_bad, model_n, model_worst,
        pill and string.format(" pill hits %d", pill_wasted) or ""))
      return
    end
    if bstate == "in_tank" and frame >= phase_at + 20 then phase, phase_at = "place", frame end
  end
  if tick >= GATE_TICKS - 300 then
    verdict(false, string.format("out of time after %d jobs: first-shell kills %d [%s]",
      trial, first_kills, table.concat(results)))
  end
end
