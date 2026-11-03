-- =========================================================================
-- GoalHunter/aim_lane.lua — the clear lane around a pill-attack aim
--
-- A "lane" here is the set of gun angles (brad, 256 per circle) from one
-- origin whose simulated shell reaches the target pill AND has no live pill
-- in the way before it. The finetune turn-in (attack.lua,
-- in_range_aim_finetune) and the shield planner's aim check use it:
--
--   * M.scan     — sample the lane on a fixed grid of angles around a base
--                  angle. The caller gives the clear test.
--   * M.nearest  — the clear sample nearest to an angle, and the error.
--   * M.width    — how far the clear run that holds an angle reaches on
--                  each side of it.
--   * M.wide_enough — is the lane at least a given width on both sides of
--                  an aim (C.BLOCKER_MIN_LANE_BRAD)?
--   * M.turn_in_step — the turn-in tap counter for C.CLEAR_TURN_TAP_PROGRESS.
--
-- No engine calls here. The callers pass a clear test, so the tests can
-- run with a plain function (test_aim_lane.lua).
-- =========================================================================

local M = {}

-- Grid step (brad) and half-width (samples each side) for the finetune lane.
-- 1/16 brad is finer than the smallest tap (0.125 brad), and 32 samples each
-- side (2 brad) covers more than the turn-in can move in 2 x 6 taps.
M.STEP      = 1.0 / 16.0
M.HALF_N    = 32

local function wrap(a)
  a = a % 256
  if a < 0 then a = a + 256 end
  return a
end

-- Signed difference b - a in brad, in (-128, 128].
local function adiff(a, b)
  local d = (b - a) % 256
  if d > 128 then d = d - 256 end
  return d
end
M.adiff = adiff

-- Sample clear_at(angle) on base + k*step for k = -n..n.
-- Returns a lane table: { base, step, n, ok = { [k] = true } }.
function M.scan(clear_at, base, step, n)
  step = step or M.STEP
  n    = n or M.HALF_N
  local ok = {}
  for k = -n, n do
    if clear_at(wrap(base + k * step)) then ok[k] = true end
  end
  return { base = base, step = step, n = n, ok = ok }
end

-- Grid index nearest to angle a, or nil when a is outside the sampled window.
local function index_of(lane, a)
  local k = math.floor(adiff(lane.base, a) / lane.step + 0.5)
  if k < -lane.n or k > lane.n then return nil end
  return k
end
M.index_of = index_of

-- The clear sample nearest to angle a. Returns (angle, abs_error) or nil when
-- no sample in the window is clear.
function M.nearest(lane, a)
  local best_k, best_e = nil, math.huge
  for k = -lane.n, lane.n do
    if lane.ok[k] then
      local e = math.abs(adiff(a, lane.base + k * lane.step))
      if e < best_e then best_k, best_e = k, e end
    end
  end
  if not best_k then return nil end
  return wrap(lane.base + best_k * lane.step), best_e
end

-- Clear run around angle a: how far (brad) the run of clear samples that
-- holds a's nearest sample reaches to the left (lower angles) and right.
-- Returns (left, right) or nil when a's sample is not clear. A run that meets
-- the window edge stops there, so the answer is at most n*step each side.
function M.width(lane, a)
  local k0 = index_of(lane, a)
  if not k0 or not lane.ok[k0] then return nil end
  local lo, hi = k0, k0
  while lo - 1 >= -lane.n and lane.ok[lo - 1] do lo = lo - 1 end
  while hi + 1 <= lane.n and lane.ok[hi + 1] do hi = hi + 1 end
  local ka = adiff(lane.base, a) / lane.step
  -- Half a step past each end sample: the run edge lies between samples.
  return (ka - lo + 0.5) * lane.step, (hi - ka + 0.5) * lane.step
end

-- Is the clear lane around angle a0 at least min_brad wide on BOTH sides?
-- Tests a0, then a0 +/- k*step for k = 1..ceil(min_brad/step), each side
-- stopping at its first blocked angle. Returns (ok, left, right): left and
-- right are how far (brad) the clear samples reach on each side, at most
-- the tested span.
function M.wide_enough(clear_at, a0, min_brad, step)
  step = step or M.STEP
  if not clear_at(wrap(a0)) then return false, 0, 0 end
  local n = math.ceil(min_brad / step - 1e-9)
  local reach = {}
  for side = -1, 1, 2 do
    local r = 0
    for k = 1, n do
      if not clear_at(wrap(a0 + side * k * step)) then break end
      r = k * step
    end
    reach[side] = r
  end
  local left, right = reach[-1], reach[1]
  return left >= min_brad - 1e-9 and right >= min_brad - 1e-9, left, right
end

-- One finetune tick while the shot reaches the pill but is not clear.
--   goal   : the attack goal (state kept in goal._ft_turn, keyed by
--            goal._finetune_start so a new finetune starts fresh)
--   taps   : goal._finetune_taps now
--   err    : abs error (brad) from the gun angle to the nearest clear angle,
--            or nil when no clear angle is known
--   limit  : CLEAR_TURN_IN_TAPS
-- A tick whose error is smaller than the best error so far does not count
-- against the limit. The raw count (every tick since the shot first reached
-- the pill) has a hard cap of 2 * limit.
-- Returns (counted, raw, give_up).
function M.turn_in_step(goal, taps, err, limit)
  local st = goal._ft_turn
  if not st or st.start ~= goal._finetune_start then
    st = { start = goal._finetune_start, best = math.huge, free = 0,
           last_raw = nil }
    goal._ft_turn = st
  end
  local raw = taps - (goal._finetune_reached_tap or taps)
  local delta = raw - (st.last_raw or raw)
  if delta < 0 then delta = 0 end
  if err and err < st.best - 1e-9 then
    if st.last_raw then st.free = st.free + delta end
    st.best = err
  end
  st.last_raw = raw
  local counted = raw - st.free
  local give_up = counted >= limit or raw >= 2 * limit
  return counted, raw, give_up
end

return M
