local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/lgm_aim.lua -- our own tank's pose when a shell really leaves,
-- for the kill-LGM aim (2026-11-21).
--
-- Recorded (session 20261121_195017_5__2v2_combined, bot 2, brain ticks
-- 42142-42194, a bot on a remote Bot Host): the keys a think sends take
-- effect 3 brain ticks later. The kill-LGM crosshair search planned from
-- info.gunrange as if its key acted at once, so it asked again for steps
-- that were already on the way: the sight swung 14 -> 2 -> 12 -> 2 -> 10 -> 2
-- round a nearly fixed target. The fire check scored the crosshair of the
-- snapshot, but the shell left 3 ticks later with another sight and missed by
-- about 500 wu.
--
-- What this module knows of the engine (no tuning numbers):
--   * A think makes two input packets: the keys tick, then the game tick
--     (client_snapshot.c). A HELD key is in both, a TAP only in the first.
--   * A packet's sight key moves the sight one step (tankGunsightIncrease /
--     Decrease, server_sim_tick.c), between gunsight_min (2) and
--     gunsight_max (14).
--   * The keys tick runs tankTurn only; the game tick runs tankUpdate: fire
--     FIRST (tank.c tankUpdate: the shell starts at the tank's present x, y,
--     angle and sight), then turn, accel and the move. So a shell fired this
--     think has both of this think's sight steps and the keys-tick turn, and
--     none of this think's move.
--   * tankTurn turns by the terrain's turn rule x our turn modifier per
--     call, but the first 6 calls of a held turn turn 1/8 of that
--     (tank.c tankTurn, firstLeft / firstRight < 6).
--   * The tank moves engine speed = info.speed / 4 wu once per think, on the
--     game tick (tankUpdate, residualSpeed).
--   * The server does NOT rewind the shooter: the shell starts from the
--     tank's present server pose when the fire input is used (tank.c
--     683-689). It DOES rewind the target builder for a remote seat by the
--     input's viewTick (server_sim_tick.c, shells.c), so the builder lead
--     from kill_lgm.predict_aim stays as it is: nothing here moves the man.
--
-- D (the delay): the keys a think sends at brain tick s are first seen in
-- info at tick s + D. D = 1 means keys show in the next think (a bot inside
-- the server). The search and the fire check treat the keys of the last
-- D - 1 thinks as "in flight": sent, not yet in info.
--
-- Knobs (constants.lua, all default on, all off in KEEL / MAST / R221):
--   LGM_AIM_KEYS_IN_FLIGHT  the search plans from info + the keys in flight
--   LGM_AIM_DELAY_EST       D measured online (else LGM_AIM_DELAY_DEFAULT)
--   LGM_AIM_LAUNCH_POSE     the fire check scores the pose at launch, and the
--                           fire think's sight / turn keys are kept as scored
--   LGM_AIM_TURN_RULE       the search turns at the engine's turn rate and
--                           ramp, not KILL_LGM_TURN_DELTA
--   LGM_AIM_SEARCH_MOVE     the search moves the tank speed / 4 wu a think
--                           (the engine), not info.speed wu, over every think
--                           up to launch
--   LGM_AIM_SIGHT_TAP       the search may also TAP the sight (one step), so
--                           an odd sight length can be reached
--   LGM_AIM_DELAY_CONFIRM   the fire check holds fire until D has
--                           LGM_AIM_DELAY_WINDOW / 2 samples (M.delay_confirmed)
-- TEST_KEY_DELAY_THINKS (test only, default 0) holds our keys back that many
-- thinks before the engine gets them, to play a remote seat's delay on a
-- local server.
-- =========================================================================

local C = require("constants")
local print2 = require("print2")
local KL = require("kill_lgm")   -- shell model (C.LGM_SHELL_MODEL_RULES)

local M = {}

-- Engine facts (tank.c tankTurn): the first TURN_RAMP_CALLS calls of a held
-- turn key turn 1 / TURN_RAMP_DIV of the full rate.
local TURN_RAMP_CALLS = 6
local TURN_RAMP_DIV   = 8

local function st_of(state)
  local s = state._lgm_aim
  if not s then
    s = { recs = {}, D = C.LGM_AIM_DELAY_DEFAULT or 1, last_obs = nil,
          fire = nil, out_q = {} }
    state._lgm_aim = s
  end
  return s
end
M.st_of = st_of

local function ring_len()
  return (C.LGM_AIM_DELAY_MAX or 14) + (C.LGM_AIM_DELAY_WINDOW or 32) + 2
end

local function rec_at(s, tick)
  local r = s.recs[tick % ring_len()]
  if r and r.tick == tick then return r end
  return nil
end

-- Per-packet sight steps and turn signs of one think's output.
-- Returns g1, g2 (sight steps of the keys / game packet: -1, 0, +1) and
-- t1, t2 (turn sign of each packet: -1 left, 0, +1 right).
local function packets(keys, taps)
  keys = keys or 0; taps = taps or 0
  local both = bit.bor(keys, taps)
  local function gstep(k)
    if bit.band(k, KEY_MORERANGE) ~= 0 then return 1 end
    if bit.band(k, KEY_LESSRANGE) ~= 0 then return -1 end
    return 0
  end
  local function tsign(k)
    local l = bit.band(k, KEY_TURNLEFT) ~= 0
    local r = bit.band(k, KEY_TURNRIGHT) ~= 0
    if l and not r then return -1 end
    if r and not l then return 1 end
    return 0
  end
  return gstep(both), gstep(keys), tsign(both), tsign(keys)
end
M.packets = packets

local function sight_limits(info)
  local hi = 14
  local r = info.rules
  if r and type(r.gunsight_max) == "number" and r.gunsight_max >= 2 then hi = r.gunsight_max end
  return 2, hi
end

local function wrap256(a)
  a = a % 256
  if a < 0 then a = a + 256 end
  return a
end

local function adiff_f(a, b)
  local d = (b - a) % 256
  if d >= 128 then d = d - 256 end
  return d
end

-- The delay the planner uses now.
function M.delay(state)
  if not C.LGM_AIM_DELAY_EST then return C.LGM_AIM_DELAY_DEFAULT or 1 end
  return st_of(state).D
end

-- Turn model. `half` = brads per tankTurn call at full rate. With `ramp`
-- the engine's first-6-calls ramp applies; rs = { dir, n } is the run of
-- calls the key has been held. Returns the signed turn of one call.
local function turn_call(rs, dir, half, ramp)
  if dir == 0 then rs.dir, rs.n = 0, 0; return 0 end
  if rs.dir ~= dir then rs.dir, rs.n = dir, 0 end
  local amt = half
  if ramp and rs.n < TURN_RAMP_CALLS then amt = half / TURN_RAMP_DIV end
  if rs.n < TURN_RAMP_CALLS then rs.n = rs.n + 1 end
  return dir * amt
end

-- The run of turn calls already applied in the snapshot (keys sent up to
-- tick last_applied). Replays the 3 thinks (6 calls) before it: enough to
-- fill the ramp.
local function ramp_state(s, last_applied)
  local rs = { dir = 0, n = 0 }
  local first = last_applied - math.ceil(TURN_RAMP_CALLS / 2) + 1
  for t = first, last_applied do
    local r = rec_at(s, t)
    local t1, t2 = 0, 0
    if r then t1, t2 = r.t1, r.t2 end
    turn_call(rs, t1, 0, false)
    turn_call(rs, t2, 0, false)
  end
  return rs
end

-- Pose model. Starts from info, applies the thinks in flight (sent at
-- now - D + 1 .. now - 1, from the record), then this think's keys `cur`
-- ({g1, g2, t1, t2}) either:
--   mode "fire": as the shell leaves on this think's game tick (both sight
--                steps, the keys-tick turn, no move);
--   mode "next": the whole think (both steps, both turns, the move), which
--                is the pose a shell fired NEXT think with no new key starts
--                from.
-- opts: D, half (brads per tankTurn call), ramp (bool), move (wu per think
-- per info.speed unit: 0.25 engine, 1.0 the old search), spd (info.speed for
-- this think's move), pending (bool: apply the thinks in flight).
-- Returns x, y, ang (float brads), gun.
local function pose(s, info, now, cur, mode, o)
  local lo, hi = sight_limits(info)
  local x, y = info.tankx + 0.0, info.tanky + 0.0
  local ang = (info.tank_angle or info.direction or 0) + 0.0
  if o.int_dir then ang = (info.direction or 0) + 0.0 end
  local gun = info.gunrange or 14
  local spd0 = (info.speed or 0) * o.move
  local D = o.D
  local rs = o.ramp and ramp_state(s, now - D) or { dir = 0, n = 0 }
  local TWO_PI_256 = math.pi * 2 / 256
  local function step(g)
    gun = gun + g
    if gun < lo then gun = lo elseif gun > hi then gun = hi end
  end
  if o.pending then
    for t = now - D + 1, now - 1 do
      local r = rec_at(s, t)
      local g1, g2, t1, t2 = 0, 0, 0, 0
      if r then g1, g2, t1, t2 = r.g1, r.g2, r.t1, r.t2 end
      step(g1); ang = ang + turn_call(rs, t1, o.half, o.ramp)
      step(g2); ang = ang + turn_call(rs, t2, o.half, o.ramp)
      local a = ang * TWO_PI_256
      x = x + math.sin(a) * spd0
      y = y - math.cos(a) * spd0
    end
  end
  step(cur[1]); ang = ang + turn_call(rs, cur[3], o.half, o.ramp)
  step(cur[2])
  if mode == "next" then
    ang = ang + turn_call(rs, cur[4], o.half, o.ramp)
    local a = ang * TWO_PI_256
    local sp = (o.spd or info.speed or 0) * o.move
    x = x + math.sin(a) * sp
    y = y - math.cos(a) * sp
  end
  return x, y, wrap256(ang), gun
end
M.pose = pose

-- ── fix 3: the burst point of a shell fired this think ───────────────────
-- keys / taps = the keys as they stand at the fire check. rate = our full
-- turn rate in brads per think (steering.turn_rate_brads). Returns the burst
-- x, y, the sight the shell leaves with, and the launch angle.
function M.launch_impact(state, info, now, keys, taps, rate)
  local s = st_of(state)
  local g1, g2, t1, t2 = packets(keys, taps)
  local x, y, ang, gun = pose(s, info, now, { g1, g2, t1, t2 }, "fire", {
    D = M.delay(state), half = (rate or 2) / 2, ramp = true, move = 0.25,
    pending = true })
  local a = ang * math.pi * 2 / 256
  -- C.LGM_SHELL_MODEL_RULES: where the engine bursts it (else 128 * gun).
  local travel = KL.shell_travel(gun)
  return x + math.sin(a) * travel, y - math.cos(a) * travel, gun, ang
end

-- The fire check passed with these keys: keep the sight and turn bits as
-- scored on this think (freeze_fire_keys, at the end of think).
local AIM_BITS
local function aim_bits()
  if not AIM_BITS then
    AIM_BITS = bit.bor(bit.bor(KEY_TURNLEFT, KEY_TURNRIGHT), bit.bor(KEY_MORERANGE, KEY_LESSRANGE))
  end
  return AIM_BITS
end
function M.note_fire(state, now, keys, taps)
  local A = aim_bits()
  st_of(state).fire = { tick = now, k = bit.band(keys or 0, A), t = bit.band(taps or 0, A) }
end

function M.freeze_fire_keys(state, keys, taps, now)
  local s = state._lgm_aim
  local f = s and s.fire
  if not (C.LGM_AIM_LAUNCH_POSE and f and f.tick == now) then return keys, taps end
  if bit.band(bit.bor(keys, taps), KEY_SHOOT) == 0 then return keys, taps end
  local A = aim_bits()
  local nA = bit.bnot(A)
  keys = bit.bor(bit.band(keys, nA), f.k)
  taps = bit.bor(bit.band(taps, nA), f.t)
  return keys, taps
end

-- ── fixes 1, 5 and the two search fixes: the crosshair search ─────────────
-- lgm_wx / lgm_wy = the aim point (kill_lgm.predict_aim, deflected as the
-- fire check deflects it). rate = steering.turn_rate_brads (fix 5);
-- turn_delta / speed_delta = the old search's steps.
-- Returns best held keys, best tap keys, best score, candidates (debug).
local CAND_T, CAND_S, CAND_G
function M.search(state, info, now, lgm_wx, lgm_wy, rate, turn_delta, speed_delta, want_cands)
  local s = st_of(state)
  if not CAND_T then
    CAND_T = { { k = 0, d = 0, n = "T:none" }, { k = KEY_TURNLEFT, d = -1, n = "T:L" },
               { k = KEY_TURNRIGHT, d = 1, n = "T:R" } }
    CAND_S = { { k = 0, d = 0, n = "S:none" }, { k = KEY_FASTER, d = 1, n = "S:+" },
               { k = KEY_SLOWER, d = -1, n = "S:-" } }
    CAND_G = { { k = 0, tap = false, d = 0, n = "G:none" },
               { k = KEY_MORERANGE, tap = false, d = 1, n = "G:+" },
               { k = KEY_LESSRANGE, tap = false, d = -1, n = "G:-" },
               { k = KEY_MORERANGE, tap = true, d = 1, n = "G:tap+" },
               { k = KEY_LESSRANGE, tap = true, d = -1, n = "G:tap-" } }
  end
  local n_guns = C.LGM_AIM_SIGHT_TAP and 5 or 3
  local o = {
    D = C.LGM_AIM_KEYS_IN_FLIGHT and M.delay(state) or 1,
    pending = C.LGM_AIM_KEYS_IN_FLIGHT and true or false,
    ramp = C.LGM_AIM_TURN_RULE and true or false,
    -- Old search: turn_delta brads a think (two calls of half each).
    half = C.LGM_AIM_TURN_RULE and ((rate or 2) / 2) or (turn_delta / 2),
    move = C.LGM_AIM_SEARCH_MOVE and 0.25 or 1.0,
    spd = 0,
    -- The old search starts from info.direction (the floored byte); the
    -- fixed one from info.tank_angle, the angle the engine fires at.
    int_dir = not (C.LGM_AIM_KEYS_IN_FLIGHT or C.LGM_AIM_TURN_RULE),
  }
  local cur_speed = info.speed or 0
  local best_score, best_keys, best_taps = math.huge, 0, 0
  local cands = want_cands and {} or nil
  local cur = { 0, 0, 0, 0 }
  for ti = 1, 3 do
    local t = CAND_T[ti]
    for si = 1, 3 do
      local sp = CAND_S[si]
      local ns = cur_speed + sp.d * speed_delta
      if ns < 0 then ns = 0 end
      if ns > 128 then ns = 128 end
      o.spd = ns
      for gi = 1, n_guns do
        local g = CAND_G[gi]
        cur[1] = g.d
        cur[2] = g.tap and 0 or g.d
        cur[3] = t.d
        cur[4] = t.d
        local x, y, ang, gun = pose(s, info, now, cur, "next", o)
        local a = ang * math.pi * 2 / 256
        local cw = KL.shell_travel(gun)   -- the burst (128 * gun, knob off)
        local cx = x + math.sin(a) * cw
        local cy = y - math.cos(a) * cw
        local dx, dy = cx - lgm_wx, cy - lgm_wy
        local score = math.sqrt(dx * dx + dy * dy)
        local hk = bit.bor(t.k, sp.k)
        local tk = 0
        if g.tap then tk = g.k else hk = bit.bor(hk, g.k) end
        if cands then
          cands[#cands + 1] = { name = t.n .. " " .. sp.n .. " " .. g.n, score = score,
                                new_dir = ang, new_speed = ns, new_gun = gun,
                                cross_wx = cx, cross_wy = cy, keys = bit.bor(hk, tk) }
        end
        if score < best_score then
          best_score, best_keys, best_taps = score, hk, tk
        end
      end
    end
  end
  return best_keys, best_taps, best_score, cands, o.D
end

-- ── fix 2: the delay estimate ───────────────────────────────────────────
-- Each think, the change seen in info since the last think (sight steps,
-- turn sign) is compared with the keys sent d thinks ago, for every d in
-- 1..LGM_AIM_DELAY_MAX. A tick counts for a d when either side moved; it is
-- an error when they differ. Over the last LGM_AIM_DELAY_WINDOW thinks the d
-- with the lowest error rate wins; D moves only to a clearly better d (see
-- choose).
--
-- 2026-11-21 (play test 20261121_231141_1, bot 1, tick 2037): one key
-- made D run away. The key sent at s matched at s + 1 (d = 1: 0 errors in
-- 1) and was an error for d = 2 at s + 2. At s + 32 the d = 1 tick left the
-- window first: d = 1 had no samples, which counted as rate math.huge, so
-- d = 2 with 1 error in 1 "won" (D 1 -> 2), and so on, one step a tick, up
-- to 14, with no key at all. Now a tick that tells no d apart (the same
-- verdict for every d) is not kept, the present D with no samples has rate
-- 0 (nothing against it), and a new d needs LGM_AIM_DELAY_WINDOW / 2
-- samples and at least one full error fewer than the present D.
local function observe(s, info, now)
  local prev = s.last_obs
  local gun = info.gunrange or 14
  local ang = (info.tank_angle or info.direction or 0) + 0.0
  s.last_obs = { tick = now, gun = gun, ang = ang }
  if not prev or prev.tick ~= now - 1 then return nil end
  local lo, hi = sight_limits(info)
  local og = gun - prev.gun
  local gun_ok = gun > lo and gun < hi and prev.gun > lo and prev.gun < hi
  local da = adiff_f(prev.ang, ang)
  local ot = 0
  if da > 1e-4 then ot = 1 elseif da < -1e-4 then ot = -1 end
  local dmax = C.LGM_AIM_DELAY_MAX or 14
  local err, inf = {}, {}
  for d = 1, dmax do
    local r = rec_at(s, now - d)
    local e, n = 0, 0
    if r then
      local sg = r.g1 + r.g2
      if gun_ok and (og ~= 0 or sg ~= 0) then
        n = n + 1
        if og ~= sg then e = e + 1 end
      end
      local stt = r.t1 + r.t2
      if stt > 0 then stt = 1 elseif stt < 0 then stt = -1 end
      if ot ~= 0 or stt ~= 0 then
        n = n + 1
        if ot ~= stt then e = e + 1 end
      end
    end
    err[d], inf[d] = e, n
  end
  -- A tick with the same verdict for every d tells no d apart: no
  -- information, not kept (with the knob LGM_AIM_DELAY_EST on, see above).
  local same = true
  for d = 2, dmax do
    if err[d] ~= err[1] or inf[d] ~= inf[1] then same = false; break end
  end
  if same then return nil end
  return err, inf
end

local function choose(s, now)
  local dmax = C.LGM_AIM_DELAY_MAX or 14
  local W = C.LGM_AIM_DELAY_WINDOW or 32
  local E, N = {}, {}
  for d = 1, dmax do E[d], N[d] = 0, 0 end
  for t = now - W + 1, now do
    local r = rec_at(s, t)
    if r and r.err then
      for d = 1, dmax do
        E[d] = E[d] + (r.err[d] or 0)
        N[d] = N[d] + (r.inf[d] or 0)
      end
    end
  end
  local cur = s.D
  if cur < 1 then cur = 1 elseif cur > dmax then cur = dmax end
  -- The present D with no samples has nothing against it (rate 0). A d
  -- needs half a window of samples to win, and at least one full error
  -- fewer than the present D at its own sample count.
  local n_min = math.floor(W / 2)
  local cur_rate = (N[cur] > 0) and (E[cur] / N[cur]) or 0
  local bar = (N[cur] > 0) and ((E[cur] - 1) / N[cur]) or -1
  local best, br = cur, cur_rate
  for d = 1, dmax do
    if d ~= cur and N[d] >= n_min then
      local rd = E[d] / N[d]
      if rd < bar and rd < br then best, br = d, rd end
    end
  end
  if best ~= s.D then
    s.D = best
    -- C.LGM_AIM_DELAY_CONFIRM: a new D starts unconfirmed.
    s.confirmed = false
  end
  -- C.LGM_AIM_DELAY_CONFIRM: D is confirmed once it has n_min samples in the
  -- window; it stays confirmed until D changes.
  if not s.confirmed and N[s.D] >= n_min then
    s.confirmed = true
  end
end

-- C.TEST_SHOT_SOURCE_SAY (test only, default false): tell the arena which
-- code pressed fire. A think whose keys or taps hold KEY_SHOOT is recorded
-- with state._shot_src (set where the fire bit is set: k = the kill-LGM fire
-- check, t = init.lua's opportunistic tank shot, n = steering's drive-by),
-- else o (any other code). When info.shells drops (a shell left), the
-- records not yet told go out in one chat line to our own player:
--   "shotsrc <tick>[-<tick>]:<code> ..."
-- (a run of ticks with one code is one item). The line waits while the think
-- already has a message. Returns send_msg, msg_dest for the think.
local SHOT_SRC_MAX_LEN = 120
function M.note_shot_source(state, info, keys, taps, now, send_msg, msg_dest)
  local src = state._shot_src
  state._shot_src = nil
  if not C.TEST_SHOT_SOURCE_SAY then return send_msg, msg_dest end
  local s = st_of(state)
  local q = s.src_q
  if not q then q = {}; s.src_q = q end
  if bit.band(bit.bor(keys, taps), KEY_SHOOT) ~= 0 then
    local c = src or "o"
    local last = q[#q]
    if last and last.c == c and last.b == now - 1 then last.b = now
    else q[#q + 1] = { a = now, b = now, c = c } end
  end
  local sh = info.shells or 0
  if s.src_shells and sh < s.src_shells then s.src_due = true end
  s.src_shells = sh
  if s.src_due and not send_msg and #q > 0 then
    local parts, len = {}, 8
    while #q > 0 do
      local r = q[1]
      local item = (r.a == r.b) and string.format("%d:%s", r.a, r.c)
                   or string.format("%d-%d:%s", r.a, r.b, r.c)
      if len + #item + 1 > SHOT_SRC_MAX_LEN then break end
      parts[#parts + 1] = item
      len = len + #item + 1
      table.remove(q, 1)
    end
    s.src_due = #q > 0
    return "shotsrc " .. table.concat(parts, " "), bit.lshift(1, state.player_number or 0)
  end
  return send_msg, msg_dest
end

-- C.LGM_AIM_DELAY_CONFIRM (2026-11-22): may the kill-LGM fire check fire on
-- the present D? Not before D has n_min (LGM_AIM_DELAY_WINDOW / 2) samples
-- in the window, the first time and again after each change of D. True
-- with the knob off, or with LGM_AIM_DELAY_EST off (D is then the fixed
-- LGM_AIM_DELAY_DEFAULT).
function M.delay_confirmed(state)
  if not (C.LGM_AIM_DELAY_CONFIRM and C.LGM_AIM_DELAY_EST) then return true end
  return st_of(state).confirmed and true or false
end

local function any_on()
  return C.LGM_AIM_KEYS_IN_FLIGHT or C.LGM_AIM_DELAY_EST or C.LGM_AIM_LAUNCH_POSE
end

-- End of think: measure, record this think's keys, and (test only) hold the
-- keys back TEST_KEY_DELAY_THINKS thinks. Returns the keys for the engine.
function M.note_sent(state, info, keys, taps, now)
  local K = C.TEST_KEY_DELAY_THINKS or 0
  if not any_on() and K <= 0 then return keys, taps end
  local s = st_of(state)
  local err, inf
  if C.LGM_AIM_DELAY_EST then err, inf = observe(s, info, now) end
  local g1, g2, t1, t2 = packets(keys, taps)
  s.recs[now % ring_len()] = { tick = now, g1 = g1, g2 = g2, t1 = t1, t2 = t2,
                               err = err, inf = inf }
  if C.LGM_AIM_DELAY_EST and err then choose(s, now) end
  if K > 0 then
    local q = s.out_q
    q[#q + 1] = { keys, taps }
    local out
    if #q > K then out = table.remove(q, 1) end
    if out then return out[1], out[2] end
    return 0, 0
  end
  return keys, taps
end

return M
