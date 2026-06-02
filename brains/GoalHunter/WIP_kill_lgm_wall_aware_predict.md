# WIP: Wall-aware LGM lead prediction

**Status:** removed from `kill_lgm.predict_aim` on 2026-05-24 because we
couldn't tell whether it was helping in practice.  Saved here so we can
resurrect it if the simpler `pos + v*T` linear predictor proves too dumb
when LGMs are wall-sliding.

## The problem it was trying to solve

LGMs slide along walls.  Engine logic (lgm.c:686-730):

1. `angle = utilCalcAngle(lgm.xy, dest.xy)` — bearing to destination.
2. `(xAdd, yAdd) = utilCalcDistance(angle, speed)` — speed split into
   axis components via cos/sin.
3. Try Y move: if `mapGetManSpeed(bmx, newbmy) > 0` apply yAdd, else
   `noGo=true`.
4. Try X move: if `mapGetManSpeed(newbmx, newbmy) > 0` AND `xAdd != 0`
   apply xAdd, else if `noGo OR yAdd==0` flip state to LGM_STATE_RETURN.

Net: one-axis-blocked → wall slide along the other axis.  Both blocked
→ give up and return home.

The observed v_ema during a wall slide is `v_max * cos(θ_intent)` where
θ_intent is the angle to the LGM's true destination (or `sin` depending
on which axis is blocked).  So a near-east-pointing LGM hitting an east
wall produces tiny `v_ema_y`, whereas an NE-pointing LGM hitting the
same wall produces `v_max * 0.707` ≈ 11 wu/tick.

A naive `pos + v_ema * T` predictor extrapolates the wall-sliding
*observed* velocity forward.  That's correct for the wall-slide
trajectory in the very near term, but it's wrong if:
- The LGM is about to clear the wall (wall ends within flight_ticks).
- We need the LGM's true intent for some other reason.

## The math

For an LGM with v_max = `MAN_SPEED[tile]` (16 on grass/road, 4 on
swamp/crater, 8 on forest) and observed `(v_ema_x, v_ema_y)`:

- Unobstructed: `|v_ema| ≈ v_max`, just trust `atan2(v_ema_x, -v_ema_y)`.
- Wall-canceled on X: observed `v_ema_x ≈ 0`, `|v_ema_y| < v_max`.
  Then `v_max² = vx_intent² + vy_intent²`, so
  `vx_intent = sign(wall_normal) * sqrt(v_max² - v_ema_y²)`.
  The wall normal sign comes from probing whether `(lgm_mx ± 1)` is
  blocked.
- Wall-canceled on Y: symmetric.

The `0.72 * v_max²` threshold (= |v_ema| within 85% of v_max) was the
unobstructed/obstructed cutoff.

## The removed code

Two helpers + a usage path in `predict_aim`:

```lua
-- Per-tile max LGM speed in wu (engine: BYTE per sim tick).  0 = blocked.
local function man_speed_at(mx, my)
  local tt = U.ttype(mx, my)
  return C.MAN_SPEED[tt] or 0
end

local function is_blocked(mx, my)
  if not U.in_map(mx, my) then return true end
  return man_speed_at(mx, my) <= 0
end

-- recover_intent: probe walls + solve v_max² = vx² + vy² for the
-- cancelled axis.  Returns the LGM's true intent vector (magnitude
-- ≈ v_max) rather than the wall-cancelled observation.
function M.recover_intent(lgm_mx, lgm_my, v_ema_x, v_ema_y)
  local v_max = man_speed_at(lgm_mx, lgm_my)
  if v_max <= 0 then return v_ema_x, v_ema_y end
  local obs_mag2 = v_ema_x * v_ema_x + v_ema_y * v_ema_y
  local v_max2 = v_max * v_max
  if obs_mag2 >= 0.72 * v_max2 then
    return v_ema_x, v_ema_y   -- unobstructed
  end
  local blocked_xp = is_blocked(lgm_mx + 1, lgm_my)
  local blocked_xn = is_blocked(lgm_mx - 1, lgm_my)
  local blocked_yp = is_blocked(lgm_mx, lgm_my + 1)
  local blocked_yn = is_blocked(lgm_mx, lgm_my - 1)
  local ax = math.abs(v_ema_x)
  local ay = math.abs(v_ema_y)
  if ax < ay and (blocked_xp or blocked_xn) then
    local vx_mag = math.sqrt(math.max(0, v_max2 - v_ema_y * v_ema_y))
    local sign  = blocked_xp and 1 or -1
    return sign * vx_mag, v_ema_y
  elseif ay < ax and (blocked_yp or blocked_yn) then
    local vy_mag = math.sqrt(math.max(0, v_max2 - v_ema_x * v_ema_x))
    local sign  = blocked_yp and 1 or -1
    return v_ema_x, sign * vy_mag
  end
  return v_ema_x, v_ema_y
end

-- sim_forward: T-tick step-by-step engine sim of LGM motion with
-- wall-sliding.  Mirrors lgm.c:686-730.  v_max is re-read each step so
-- swamp/crater transitions slow the LGM correctly.
function M.sim_forward(lgm_wx, lgm_wy, vx, vy, T)
  local wx, wy = lgm_wx, lgm_wy
  for _ = 1, T do
    local cur_mx = math.floor(wx) >> 8
    local cur_my = math.floor(wy) >> 8
    local v_max = man_speed_at(cur_mx, cur_my)
    if v_max <= 0 then break end
    local vmag = math.sqrt(vx * vx + vy * vy)
    local svx, svy = vx, vy
    if vmag > 1e-6 then
      local scale = v_max / vmag
      svx, svy = vx * scale, vy * scale
    end
    local nx = wx + svx
    local ny = wy + svy
    local new_mx = math.floor(nx) >> 8
    local new_my = math.floor(ny) >> 8
    local x_blocked = (new_mx ~= cur_mx) and is_blocked(new_mx, cur_my)
    local y_blocked = (new_my ~= cur_my) and is_blocked(cur_mx, new_my)
    if x_blocked and y_blocked then break end
    if not x_blocked then wx = nx end
    if not y_blocked then wy = ny end
  end
  return wx, wy
end
```

And inside `predict_aim`, the wall-aware version used:

```lua
local intent_vx, intent_vy = M.recover_intent(lgm_mx, lgm_my, vx, vy)
local pos1 = M.sim_forward(lgm_wx, lgm_wy, intent_vx, intent_vy, T1)
-- ... (instead of linear pos + v*T)
```

The two-pass structure (`T1 → pos1 → T2 → extra ticks`) was kept after
the gut; only the intent recovery + step sim were removed.

## Why we removed it

The simpler `pos + v_ema * T` was good enough in BrainTest — the
linearization error was within the 128 wu splash budget for the LGM
speeds and shot-flight times we see in practice.  And we couldn't tell
whether `recover_intent` was actually helping or hurting (it would be
wrong in cases where the LGM has already cleared the wall but
`v_ema` hasn't caught up yet — then it'd inject a fake intent vector
back into the wall).

## When to consider restoring

- LGM kill rate drops noticeably on wall-heavy maps.
- We see consistent misses on LGMs that are wall-sliding away from us.
- The `kill_lgm_predict` viz shows predictions running off into walls
  while the LGM is actually about to clear and go diagonal.

## How to restore

1. Paste the three helpers back into `kill_lgm.lua` (after
   `flight_ticks`).
2. In `predict_aim`, replace the linear `aim = pos + v*T` lines with
   `recover_intent` → `sim_forward`.
3. Verify the 27-candidate search's `_primary_lgm.predicted_wx/wy` still
   updates each tick — perception calls `predict_aim` per LGM in
   `perception.lua` around line 317.
