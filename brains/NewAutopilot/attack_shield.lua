-- =========================================================================
-- NewAutopilot/attack_shield.lua
--
-- Wall-shielded pill-attack standoff finder.
--
-- Given the standoff position picked by evaluate_pill_difficulty, scan
-- 8 nearby positions on the same standoff circle (~2 degrees apart,
-- 4 each side of the chosen angle) AND the standoff itself (9 total
-- candidates). For each candidate compute, per aim point (pill center
-- + 4 corners), how many tiles in the pill's RETURN-fire path block
-- the shot (wall or friendly pill) WITHOUT also blocking the tank's
-- outgoing shot for that aim. Score = 10 * best per-aim blocker count.
--
-- Reject filters:
--   - any of the 5 outgoing aims with a wall in the path (per-aim hard
--     reject of just that aim, not the candidate)
--   - blocker tiles within BLOCKER_MIN_DIST of the candidate's own tile
--     (too close to the tank — no maneuvering room)
-- =========================================================================

local C   = require("constants")
local U   = require("util")
local cpf = require("cpathfinder")
local viz = require("viz")
local opt      = require("optimize")
local clock_us = clock_us or function() return 0 end

local M = {}

-- Tunables -------------------------------------------------------------------
-- Sweep is at ~1 gu of lateral resolution on the standoff arc:
-- arc step = R * θ, so θ_deg ≈ 3.58 / R per 1 gu (1/16 tile). At
-- R=7 tiles (PPT_STANDOFF) that's ~0.51°. STEP_DEG=0.5 gets us 1 gu
-- resolution; NUM_CANDIDATES=28 preserves the original ±7° span
-- (14 each side, 0.5° steps).
M.NUM_CANDIDATES        = 28
M.STEP_DEG              = 0.5
M.BLOCKER_MIN_DIST      = 1.0   -- excludes only the standoff tile itself
                                 -- (tank is 14 px in a 16 px tile so an
                                 -- orthogonal-neighbor wall doesn't
                                 -- collide with the tank body)
M.NEIGHBOR_BONUS        = 4   -- per contiguous neighbor that "covers"
                              -- the candidate. Walks left + right
                              -- separately, stops on first gap, then
                              -- counts only 2 * min(left, right) — the
                              -- symmetric chain. A 8/4 run scores like
                              -- a 4/4 run; favors positions with
                              -- forgivable margin in both directions.
M.SCORE_PER_SLOT        = 1   -- per protection slot (existing OR potential)
M.BUILT_BONUS           = 1   -- extra per slot that already has a wall/pill
                              -- so: actual = 2, potential = 1

-- Wounded-pill biasing for the (aim, subset) chooser. Goal: when
-- the pill is already softened we don't need a heavy setup, so
-- prefer fewer blockers. When it's still tough, prefer more.
-- "Favored" subsets get WOUNDED_FAVOR_BONUS added to their total;
-- subsets failing the chain-length floor are excluded entirely
-- (set total = -inf so they never win).
-- Tiered: full/near-full pills get a bigger bonus for a 3-blocker
-- setup (worth the LGM time when the pill is going to take many
-- shots anyway). Mid-wounded prefers 2 blockers. Low-HP rushes
-- with 1 blocker.
M.WOUNDED_HP_FULL_MAX           = 15  -- HP <= this AND >= FULL_MIN
M.WOUNDED_HP_FULL_MIN           = 14
M.WOUNDED_HP_FULL_FAV2_BLOCKERS = 2   -- 2-blocker bonus tier
M.WOUNDED_HP_FULL_BONUS2        = 1000
M.WOUNDED_HP_FULL_FAV3_BLOCKERS = 3   -- 3-blocker bonus tier (bigger)
M.WOUNDED_HP_FULL_BONUS3        = 2000
M.WOUNDED_HP_FULL_MIN_CHAIN     = 2
M.WOUNDED_HP_HIGH_MAX           = 13  -- HP <= this AND >= MIN
M.WOUNDED_HP_HIGH_MIN           = 11
M.WOUNDED_HP_HIGH_FAV_BLOCKERS  = 2   -- favor subsets w/ this many (a+p)
M.WOUNDED_HP_HIGH_MIN_CHAIN     = 2   -- HARD floor: chain (sym total) must be >= this
M.WOUNDED_HP_LOW_MAX            = 10  -- HP <= this
M.WOUNDED_HP_LOW_FAV_BLOCKERS   = 1
M.WOUNDED_HP_LOW_MIN_CHAIN      = 0   -- no floor for the rush case
M.WOUNDED_FAVOR_BONUS           = 1000  -- bonus for the high/low favored count

-- Viz: hide losing candidates after this many ticks. Lets a human read
-- the full grid right after scan, then de-clutters once they've seen it.
M.NONWINNER_FADE_TICKS  = 200  -- ~4 s at 50 Hz

-- Aim points within the pill tile, in two coordinate systems:
--   AIM_OFFSETS       : world units within the tile (0..255), used
--                       by score_aim to call cpf.simulate_shot.
--   M.AIM_OFFSETS_TILE: tile units (0..1), exposed for callers that
--                       set goal.aim_mx/aim_my (which are in tile
--                       float coords) — attack.lua uses it in two
--                       places, the trajectory-line viz uses it once.
--                       Single source = no drift between sim and aim.
--
-- Corners are inset M.AIM_INSET wu (1 gu) toward the center so the
-- firing trajectory has a bit of error tolerance — a pure corner aim
-- that grazes the next tile slips off-target if the tank drifts a wu
-- or two; an inset corner gives a safer angle while still reading
-- visually as "the corner".
-- AIM_INSET (scoring): how far the four corners are pulled toward the
--                      tile center for the SHIELD-SCAN simulation.
--                      Bigger = more forgiving angles; the chosen aim
--                      already lands a bit inside the corner so a
--                      sub-tile wobble at firing time still reads as
--                      "the corner".
-- AIM_INSET_FIRE     : standalone (NOT stacked with AIM_INSET) inset
--                      used when the shot actually fires. Scoring uses
--                      AIM_INSET (deeper inside the tile) to reject
--                      candidates whose corner aim is borderline; the
--                      fire-time inset is smaller (closer to the edge,
--                      more aggressive corner aim) so the shell has
--                      maximum chance of clearing the wall corner.
--                      Earlier comment claimed FIRE was "extra inset
--                      on top of AIM_INSET" — wrong on both counts;
--                      it's standalone AND smaller.
M.AIM_INSET      = 24   -- 1.5 gu (16 wu/gu) — scoring (more conservative)
M.AIM_INSET_FIRE = 16   -- 1 gu — fire-time aim (more aggressive corner)
local AIM_OFFSETS = {
  { 128, 128 },                                -- 1: center
  { M.AIM_INSET,       M.AIM_INSET       },    -- 2: top-left
  { 255 - M.AIM_INSET, M.AIM_INSET       },    -- 3: top-right
  { M.AIM_INSET,       255 - M.AIM_INSET },    -- 4: bottom-left
  { 255 - M.AIM_INSET, 255 - M.AIM_INSET },    -- 5: bottom-right
}
do
  local t = M.AIM_INSET / 256.0
  M.AIM_OFFSETS_TILE = {
    { 0.5, 0.5 },
    { t,        t        },
    { 1.0 - t,  t        },
    { t,        1.0 - t  },
    { 1.0 - t,  1.0 - t  },
  }
  local f = M.AIM_INSET_FIRE / 256.0
  M.AIM_OFFSETS_TILE_FIRE = {
    { 0.5, 0.5 },
    { f,        f        },
    { 1.0 - f,  f        },
    { f,        1.0 - f  },
    { 1.0 - f,  1.0 - f  },
  }
end
M.AIM_NAMES = { "center", "TL", "TR", "BL", "BR" }
-- Distinct colors for each aim (used by draw_overlay).
M.AIM_COLORS = {
  { 255, 220,  60, 220 },  -- 1: yellow
  { 255,  90,  90, 220 },  -- 2: red
  {  90, 230,  90, 220 },  -- 3: green
  { 110, 170, 255, 220 },  -- 4: blue
  { 230, 110, 230, 220 },  -- 5: magenta
}

-- ── Shield stamp cache — must be declared before score_aim/score_candidate ───
-- Precomputed shot-path tile offsets (relative to pill) for every 0.25°
-- angle, 5 nudge levels, and 6 aims (0=return fire, 1-5=outgoing).
-- angle_key = math.floor((deg % 360) * 4)  →  integer 0..1439
-- C-side stamp module registered by naShieldStampRegister in luabrainshandler.c.
local na_shield = na_shield
-- Fast flat tile lookup: na_shield.get_flat(angle_key, nudge_wu, aim_idx)
-- Returns flat {dx1,dy1,...} relative to pill. nil when not loaded / OOB.
local _get_flat = na_shield and na_shield.get_flat or nil

-- Flat boolean table built by C after na_shield.load(): index =
-- (angle_key * N_NUDGES + nudge_idx) * N_AIMS + aim_idx + 1  (1-based)
-- N_NUDGES=5 (wu 0,8,16,24,32), N_AIMS=6 (0=ret-fire, 1-5=outgoing).
-- Set by M.load_stamp_bin(); nil until then → fallback to ensure_nudge_ring.
local _pill_hit = nil
local _PH_NUDGES = 33   -- 0..32 inclusive (33 nudge slots)
local _PH_AIMS   = 6    -- 0=return-fire + 5 outgoing aims (#AIM_OFFSETS+1)
local _PH_STRIDE = _PH_NUDGES * _PH_AIMS  -- 198: per-angle stride
local _NUDGE_IDX = {}
for _ni = 0, 32 do _NUDGE_IDX[_ni * 8] = _ni end

-- Score one (candidate_pos, aim_i) pair.
-- Returns: outgoing_tiles, outgoing_blocked_by_wall,
--          per_aim_blockers (filtered list), per_aim_score
-- precomp_out:   {mx=,my=} table list (from simulate_shot), or nil
-- precomp_flat:  flat {dx1,dy1,...} relative to (pmx,pmy) for outgoing shot, or nil
-- return_flat:   flat {dx1,dy1,...} relative to (pmx,pmy) for return-fire path, or nil
local function score_aim(spot_wx, spot_wy, origin_mx, origin_my,
                          pmx, pmy, pill_wx, pill_wy,
                          aim_offset, return_tiles_set, return_tiles_list,
                          world, no_builder, precomp_out, precomp_flat, return_flat)
  local target_wx = (pmx << 8) + aim_offset[1]
  local target_wy = (pmy << 8) + aim_offset[2]

  -- Outgoing path: walk up to the pill, reject the aim if it crosses
  -- a wall OR any pill (friendly or enemy) other than the target.
  local outgoing_blocked_by_wall = false
  local outgoing_set = {}
  local out_tiles_result = nil  -- returned; nil when using stamp (not needed)

  if precomp_flat then
    -- Fast path: stamp flat dx/dy array — no Lua {mx,my} table per tile
    for i = 1, #precomp_flat, 2 do
      local tmx = pmx + precomp_flat[i]
      local tmy = pmy + precomp_flat[i + 1]
      if tmx == pmx and tmy == pmy then break end
      if not (tmx == origin_mx and tmy == origin_my) then
        outgoing_set[tmy * 256 + tmx] = true
        local tt = U.ttype(tmx, tmy)
        if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
          outgoing_blocked_by_wall = true
        end
        local plist = world.pill_at and world.pill_at[tmy * 256 + tmx]
        if plist then
          for _, e in ipairs(plist) do
            if e.pill and (e.pill.mx ~= pmx or e.pill.my ~= pmy) then
              outgoing_blocked_by_wall = true
              break
            end
          end
        end
      end
    end
  else
    local out_tiles = precomp_out or cpf.simulate_shot(spot_wx, spot_wy,
                                                        target_wx, target_wy,
                                                        cpf.SHOT_TANK, 0)
    out_tiles_result = out_tiles
    if out_tiles then
      for ti = 1, #out_tiles do
        local t = out_tiles[ti]
        if t.mx == pmx and t.my == pmy then break end
        if not (t.mx == origin_mx and t.my == origin_my) then
          outgoing_set[t.my * 256 + t.mx] = true
          local tt = U.ttype(t.mx, t.my)
          if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
            outgoing_blocked_by_wall = true
          end
          local plist = world.pill_at and world.pill_at[t.my * 256 + t.mx]
          if plist then
            for _, e in ipairs(plist) do
              if e.pill and (e.pill.mx ~= pmx or e.pill.my ~= pmy) then
                outgoing_blocked_by_wall = true
                break
              end
            end
          end
        end
      end
    end
  end

  -- Walk the pill's return-fire path and bucket each qualifying tile:
  --   - actual_blockers: tile already contains a wall or friendly pill
  --                      (counts toward score)
  --   - potential_blockers: tile is empty/passable but qualifies as a
  --                         protection slot (wall could be built here)
  -- Both lists are filtered identically: not on tank's outgoing path,
  -- Pill-distance gate: a wall is only useful if it's strictly
  -- between the pill and the standoff (i.e. closer to the pill than
  -- the standoff is). We use the STANDOFF position (cand.cx/cy via
  -- spot_wx/wy) as the reference, NOT origin_mx/my — origin_mx/my
  -- is the LGM dispatch position (approach, ~1.5 tiles further from
  -- pill than standoff), which would let walls behind the standoff
  -- slip through as "in front of" the approach.
  local pill_cx, pill_cy = pmx + 0.5, pmy + 0.5
  local standoff_cx = spot_wx / 256.0
  local standoff_cy = spot_wy / 256.0
  local pdtx, pdty = standoff_cx - pill_cx, standoff_cy - pill_cy
  local pill_to_standoff_d = math.sqrt(pdtx * pdtx + pdty * pdty)
  local actual_blockers, potential_blockers = {}, {}
  local unreachable_blockers = {}  -- buildable tiles the LGM can't reach (viz only)
  local _ret_src = return_flat or return_tiles_list
  if not outgoing_blocked_by_wall and _ret_src then
    local _ret_is_flat = (return_flat ~= nil)
    for _ri = 1, #_ret_src, (_ret_is_flat and 2 or 1) do
      local tmx, tmy
      if _ret_is_flat then
        tmx = pmx + _ret_src[_ri]
        tmy = pmy + _ret_src[_ri + 1]
      else
        local t = _ret_src[_ri]
        tmx, tmy = t.mx, t.my
      end
      local idx = tmy * 256 + tmx
      local skip = (tmx == pmx and tmy == pmy) or
                   (tmx == origin_mx and tmy == origin_my)
      -- replace all t.mx/t.my uses below with tmx/tmy
      if not skip and not outgoing_set[idx] and U.in_map(tmx, tmy) then
        local sdx = tmx + 0.5 - standoff_cx
        local sdy = tmy + 0.5 - standoff_cy
        local d_standoff = math.sqrt(sdx * sdx + sdy * sdy)
        local pdx = tmx + 0.5 - pill_cx
        local pdy = tmy + 0.5 - pill_cy
        local pdist = math.sqrt(pdx * pdx + pdy * pdy)
        local in_front_of_standoff = pdist < pill_to_standoff_d
        if d_standoff >= M.BLOCKER_MIN_DIST and in_front_of_standoff then
          local kind = nil
          local plist = world.pill_at and world.pill_at[idx]
          if plist then
            for _, e in ipairs(plist) do
              if e.pill and e.pill.owner == "friendly" then
                kind = "friendly_pill"
                break
              end
            end
          end
          if not kind then
            local tt = U.ttype(tmx, tmy)
            if tt == C.T_BUILDING then kind = "wall_full"
            elseif tt == C.T_HALFBUILD then kind = "wall_half"
            end
          end
          if kind then
            actual_blockers[#actual_blockers + 1] =
              { mx = tmx, my = tmy, kind = kind }
          else
            local tt = U.ttype(tmx, tmy)
            local buildable =
              tt ~= C.T_DEEPSEA and tt ~= C.T_RIVER and
              tt ~= C.T_SWAMP   and tt ~= C.T_PILLBOX and
              tt ~= C.T_REFBASE and tt ~= C.T_BOAT
            if no_builder then buildable = false end
            if buildable then
              local reachable = true
              if math.abs(tmx - origin_mx) + math.abs(tmy - origin_my) > 1 then
                local ticks = cpf.lgm_travel_ticks_map(
                  origin_mx, origin_my, tmx, tmy, 0, 0, 2000, 150)
                reachable = ticks ~= -1
              end
              if reachable then
                potential_blockers[#potential_blockers + 1] =
                  { mx = tmx, my = tmy, kind = "empty" }
              else
                unreachable_blockers[#unreachable_blockers + 1] =
                  { mx = tmx, my = tmy, kind = "unreachable",
                    origin_mx = origin_mx, origin_my = origin_my }
              end
            end
          end
        end
      end
    end
  end
  return out_tiles_result, outgoing_blocked_by_wall, actual_blockers, potential_blockers, unreachable_blockers
end

-- Score one candidate position fully (all 5 aims + return fire).
-- Returns the populated candidate table.
local function score_candidate(cand, pill, world, pill_wx, pill_wy, no_builder)
  local pmx, pmy = pill.mx, pill.my
  local mx, my = cand.mx, cand.my

  if not U.in_map(mx, my) then return cand end
  local tt = U.ttype(mx, my)
  local passable = (C.TERRAIN_COST_LAND[tt] or 9999) < 9999 and not U.is_water(tt)
  if not passable then return cand end
  cand.valid_tile = true

  -- Use the candidate's actual sub-tile float position as the shooter
  -- (and return-fire target) origin instead of the tile center. The
  -- in-game tank parks at standoff_fx/fy when it fires, and shells
  -- start at the tank center per shellsAddItem (tank.x, tank.y) — the
  -- simulator then applies SHELL_START_ADD itself. Using tile center
  -- here would push the shell origin up to ~half a tile off, enough
  -- to flip which tiles it crosses near the pill corners.
  local spot_wx = math.floor(cand.cx * 256 + 0.5)
  local spot_wy = math.floor(cand.cy * 256 + 0.5)

  -- Compute return fire ONCE: use stamp cache if loaded, else simulate_shot.
  local _ak = _get_flat and math.floor((cand.deg % 360) * 4) or nil
  cand._ak = _ak  -- store for nudge ring reuse
  local _ret_flat = _ak and _get_flat(_ak, 0, 0)  -- aim_idx 0 = return fire
  local return_tiles = nil
  if not _ret_flat then
    return_tiles = cpf.simulate_shot(pill_wx, pill_wy, spot_wx, spot_wy,
                                     cpf.SHOT_PILL, 0)
  end
  cand.return_fire = { tiles = return_tiles }

  -- LGM dispatch origin during build_walls. The tank doesn't park on
  -- the standoff tile during the build — it sits at the APPROACH
  -- position (~1.5 tiles further from the pill, see attack.lua's
  -- approach_fx/fy calc). So the LGM walks from there, not from
  -- standoff. Compute the approach tile here using the same formula
  -- attack.lua uses (ATTACK_APPROACH_OFFSET away from the pill along
  -- the standoff→pill line) and pass into score_aim for the
  -- LGM-reachability check.
  local lgm_omx, lgm_omy = mx, my
  do
    local dx = cand.cx - (pmx + 0.5)
    local dy = cand.cy - (pmy + 0.5)
    local d = math.sqrt(dx * dx + dy * dy)
    if d > 0.01 then
      local ux, uy = dx / d, dy / d
      local afx = cand.cx + ux * (C.ATTACK_APPROACH_OFFSET or 1.5)
      local afy = cand.cy + uy * (C.ATTACK_APPROACH_OFFSET or 1.5)
      local amx = U.mclamp(math.floor(afx))
      local amy = U.mclamp(math.floor(afy))
      if U.in_map(amx, amy) then lgm_omx, lgm_omy = amx, amy end
    end
  end

  local best_aim_idx = nil
  local best_score = 0
  for ai = 1, #AIM_OFFSETS do
    -- Use precomputed stamp tiles when available (aim_idx 1..N for outgoing).
    local _out_flat = _ak and _get_flat(_ak, 0, ai)
    local out_tiles, blocked, actual_blockers, potential_blockers, unreachable_blockers = score_aim(
      spot_wx, spot_wy, lgm_omx, lgm_omy,
      pmx, pmy, pill_wx, pill_wy,
      AIM_OFFSETS[ai], nil, return_tiles, world, no_builder, nil, _out_flat, _ret_flat)
    -- Score = 10 per protection slot (actual or potential) + small
    -- bonus per already-built slot. So ranking is driven by the total
    -- protection geometry, with a tiebreaker that favors spots whose
    -- walls already exist (saves LGM trips and time-to-fire).
    local actual_n    = #actual_blockers
    local potential_n = #potential_blockers
    local aim_score = 0
    if not blocked then
      aim_score = M.SCORE_PER_SLOT * (actual_n + potential_n)
                + M.BUILT_BONUS    * actual_n
    end
    cand.aims[ai] = {
      tiles               = out_tiles,
      blocked             = blocked,
      blockers            = actual_blockers,
      potential_blockers  = potential_blockers,
      unreachable_blockers = unreachable_blockers,
      score               = aim_score,
    }
    if aim_score > best_score then
      best_score   = aim_score
      best_aim_idx = ai
    end
  end
  cand.best_aim_idx = best_aim_idx
  cand.score        = best_score
  -- Stash the per-component breakdown for the chosen best aim so the
  -- score viz can render "14 (4+6+4)" style: actual_score, potential_score,
  -- and the running neighbor-bonus accumulator (set later in scan()).
  if best_aim_idx then
    local a = cand.aims[best_aim_idx]
    cand.score_actual    = (M.SCORE_PER_SLOT + M.BUILT_BONUS) * #a.blockers
    cand.score_potential = M.SCORE_PER_SLOT * #a.potential_blockers
  else
    cand.score_actual    = 0
    cand.score_potential = 0
  end
  cand.score_neighbor = 0
  return cand
end

-- Build an empty candidate skeleton at the given (cx, cy, deg).
local function make_candidate(cx, cy, deg, offset_deg, kind)
  return {
    kind         = kind or "candidate",  -- "standoff" for the original spot
    deg          = deg,
    offset_deg   = offset_deg,
    cx           = cx,
    cy           = cy,
    mx           = math.floor(cx),
    my           = math.floor(cy),
    valid_tile   = false,
    aims         = {},
    return_fire  = { tiles = nil },
    best_aim_idx = nil,
    score        = 0,
  }
end

-- Public scan entry point.
-- no_builder: if true, the LGM is dead/unavailable, so potential
-- (would-be-built) blocker slots are dropped from scoring. Only
-- existing walls and friendly pills count toward the protection
-- score. attack.lua passes this when info.man_status == LGM_DEAD.
function M.scan(pill, world, standoff_mx, standoff_my, standoff_deg,
                standoff_cx, standoff_cy, radius, no_builder, tank_armour,
                positions, step_deg)
  if not pill or not world then return { candidates = {}, best = nil } end
  -- Tier-gated ring density.  Defaults preserve the legacy 28×0.5° sweep.
  -- Lua fallback path uses these locals below; C scan_c receives them
  -- via the new args 20/21.
  local NUM = positions or M.NUM_CANDIDATES
  local SDG = step_deg  or M.STEP_DEG
  local pmx, pmy = pill.mx, pill.my
  -- Caller can override the standoff circle radius (PPT uses a tighter
  -- one). Defaults to the standard ATTACK_PILL_STANDOFF.
  local R = radius or C.ATTACK_PILL_STANDOFF
  local pcx, pcy = pmx + 0.5, pmy + 0.5
  local pill_wx = (pmx << 8) | 128
  local pill_wy = (pmy << 8) | 128

  -- Standoff candidate first, then 8 around.
  local sx = standoff_cx or (standoff_mx + 0.5)
  local sy = standoff_cy or (standoff_my + 0.5)

  -- ── C scan path ───────────────────────────────────────────────────────────
  if na_shield and na_shield.scan_c and _pill_hit then
    local pill_hp = pill.health or 0
    -- HP-dependent neighbor bonus params (passed to scan_c as args 13..19).
    local n_fav, fs1, fb1, fs2, fb2 = 0, 0, 0.0, 0, 0.0
    local min_chain, max_bonus = 0, M.WOUNDED_FAVOR_BONUS
    local fav_bonus_by_size, tier_label = {}, "NONE"
    if pill_hp >= M.WOUNDED_HP_FULL_MIN and pill_hp <= M.WOUNDED_HP_FULL_MAX then
      n_fav = 2
      fs1, fb1 = M.WOUNDED_HP_FULL_FAV2_BLOCKERS, M.WOUNDED_HP_FULL_BONUS2
      fs2, fb2 = M.WOUNDED_HP_FULL_FAV3_BLOCKERS, M.WOUNDED_HP_FULL_BONUS3
      min_chain = M.WOUNDED_HP_FULL_MIN_CHAIN
      max_bonus = math.max(M.WOUNDED_HP_FULL_BONUS2, M.WOUNDED_HP_FULL_BONUS3)
      fav_bonus_by_size[fs1] = fb1; fav_bonus_by_size[fs2] = fb2
      tier_label = "FULL"
    elseif pill_hp >= M.WOUNDED_HP_HIGH_MIN and pill_hp <= M.WOUNDED_HP_HIGH_MAX then
      n_fav = 1; fs1 = M.WOUNDED_HP_HIGH_FAV_BLOCKERS; fb1 = M.WOUNDED_FAVOR_BONUS
      min_chain = M.WOUNDED_HP_HIGH_MIN_CHAIN; max_bonus = M.WOUNDED_FAVOR_BONUS
      fav_bonus_by_size[fs1] = fb1; tier_label = "HIGH"
    elseif pill_hp <= M.WOUNDED_HP_LOW_MAX then
      n_fav = 1; fs1 = M.WOUNDED_HP_LOW_FAV_BLOCKERS; fb1 = M.WOUNDED_FAVOR_BONUS
      min_chain = M.WOUNDED_HP_LOW_MIN_CHAIN; max_bonus = M.WOUNDED_FAVOR_BONUS
      fav_bonus_by_size[fs1] = fb1; tier_label = "LOW"
    end

    -- Low-armour shield demote: when our own HP is below ARMOUR_LOW we
    -- can't soak the extra return fire that comes from a too-thin shield,
    -- so raise the min_chain floor by 1 (cap 3) regardless of pill HP.
    if tank_armour and tank_armour < (C.ARMOUR_LOW or 15) then
      min_chain = math.min(min_chain + 1, 3)
    end

    local pill_table = {}
    if world.pill_at then
      for _, plist in pairs(world.pill_at) do
        for _, e in ipairs(plist) do
          if e.pill then
            local p = e.pill
            pill_table[#pill_table+1] = p.mx
            pill_table[#pill_table+1] = p.my
            pill_table[#pill_table+1] = (p.owner == "friendly") and 1 or 0
          end
        end
      end
    end

    local r = na_shield.scan_c(
      pmx, pmy, pill_wx, pill_wy,
      standoff_deg, sx, sy, R,
      pill_table,
      standoff_mx or math.floor(sx), standoff_my or math.floor(sy),
      no_builder and true or false,
      n_fav, fs1, fb1, fs2, fb2, min_chain, max_bonus,
      NUM, SDG)

    if r then
      local standoff_cand = { cx = sx, cy = sy,
                               mx = standoff_mx or math.floor(sx),
                               my = standoff_my or math.floor(sy) }
      if r[1] == 0 then
        return { best = nil, pill = pill, standoff = standoff_cand,
                 pill_hp = pill_hp, fav_bonus_by_size = fav_bonus_by_size,
                 min_chain = min_chain, tier_label = tier_label }
      end
      local bai   = r[8]
      local n_act = r[10]
      local n_pot = r[21]
      local act, pot = {}, {}
      for k = 0, n_act - 1 do
        act[k+1] = { mx = pmx + r[11+k], my = pmy + r[16+k] }
      end
      for k = 0, n_pot - 1 do
        pot[k+1] = { mx = pmx + r[22+k], my = pmy + r[27+k] }
      end
      local best = {
        kind = "candidate",
        cx = r[2], cy = r[3], mx = r[4], my = r[5], deg = r[6],
        score = r[7], best_aim_idx = bai,
        score_actual = r[32], score_potential = r[33], score_neighbor = r[34],
        valid_tile = true, aims = {}, return_fire = { tiles = nil },
      }
      best.aims[bai] = {
        blocked = false,
        blockers = act, potential_blockers = pot, unreachable_blockers = {},
        nudge_wu = r[9], score = r[32] + r[33],
      }
      return { best = best, pill = pill, standoff = standoff_cand,
               pill_hp = pill_hp, fav_bonus_by_size = fav_bonus_by_size,
               min_chain = min_chain, tier_label = tier_label }
    end
  end
  -- ── End C scan path ───────────────────────────────────────────────────────

  local candidates = {}
  candidates[1] = make_candidate(sx, sy, standoff_deg, 0, "standoff")
  -- Force the standoff's tile to the integer one we were handed (in case
  -- the float center rounds differently).
  candidates[1].mx = standoff_mx
  candidates[1].my = standoff_my

  local half = NUM * 0.5
  for i = 1, NUM do
    local offset = (i - half - 0.5) * SDG
    local deg = standoff_deg + offset
    local rad = math.rad(deg)
    local cx = pcx + math.sin(rad) * R
    local cy = pcy - math.cos(rad) * R
    candidates[#candidates + 1] = make_candidate(cx, cy, deg, offset, "candidate")
  end

  -- ── Nudge infrastructure ─────────────────────────────────────────────────
  -- When simulate_shot from a candidate doesn't include the pill tile, we
  -- move the origin forward (toward the pill) in small steps until it does.
  -- The nudge amount is recorded per aim so other aims for the same candidate
  -- are unaffected.  When the neighbor-bonus loop checks a neighbor for a
  -- nudged aim, it uses the neighbor's position at the same nudge distance —
  -- pre-computed here as a full ring so no trajectory is simulated more than
  -- once per (nudge_wu, aim_idx) across all candidates.
  local NUDGE_STEP_WU = 8    -- 0.03125 tiles per step (half a gu)
  local NUDGE_MAX_STEPS = 32 -- cap at 256 wu = 1 tile forward

  -- nudge_rings[nudge_wu][ai][ci] -> score_aim result table
  local nudge_rings = {}
  -- nudge_pos[nudge_wu][ci] -> {wx, wy}
  local nudge_pos   = {}

  -- Nudged world-unit position for candidate ci moved nudge_wu wu toward pill.
  local function nudge_origin(ci, nudge_wu)
    local row = nudge_pos[nudge_wu]
    if row and row[ci] then return row[ci].wx, row[ci].wy end
    if not row then row = {}; nudge_pos[nudge_wu] = row end
    local c   = candidates[ci]
    local dx  = pcx - c.cx
    local dy  = pcy - c.cy
    local d   = math.sqrt(dx * dx + dy * dy)
    local wx, wy
    if d > 0.001 then
      local frac = (nudge_wu / 256.0) / d
      wx = math.floor((c.cx + dx * frac) * 256 + 0.5)
      wy = math.floor((c.cy + dy * frac) * 256 + 0.5)
    else
      wx = math.floor(c.cx * 256 + 0.5)
      wy = math.floor(c.cy * 256 + 0.5)
    end
    row[ci] = { wx = wx, wy = wy }
    return wx, wy
  end

  -- Build (or return cached) score_aim results for every candidate at
  -- nudge_wu world-units forward, for aim index ai.  Building the whole
  -- ring at once means each (nudge_wu, ai) pair costs O(N) simulate_shot
  -- calls total — neighbors reuse the same cache entries.
  local function ensure_nudge_ring(nudge_wu, ai)
    local ring_d = nudge_rings[nudge_wu]
    if not ring_d then ring_d = {}; nudge_rings[nudge_wu] = ring_d end
    if ring_d[ai] then return ring_d[ai] end
    local aim_ring = {}
    for ci, c in ipairs(candidates) do
      local nwx, nwy = nudge_origin(ci, nudge_wu)
      local nmx = math.floor(nwx / 256)
      local nmy = math.floor(nwy / 256)
      -- Return fire from the nudged position.
      local ret = cpf.simulate_shot(pill_wx, pill_wy, nwx, nwy, cpf.SHOT_PILL, 0)
      -- Approach tile: same outward direction as original, but from nudged pos.
      local lgm_omx, lgm_omy = nmx, nmy
      local odx = c.cx - pcx
      local ody = c.cy - pcy
      local od  = math.sqrt(odx * odx + ody * ody)
      if od > 0.01 then
        local ux, uy = odx / od, ody / od
        local ncx = nwx / 256.0
        local ncy = nwy / 256.0
        local afx = ncx + ux * (C.ATTACK_APPROACH_OFFSET or 1.5)
        local afy = ncy + uy * (C.ATTACK_APPROACH_OFFSET or 1.5)
        local amx = U.mclamp(math.floor(afx))
        local amy = U.mclamp(math.floor(afy))
        if U.in_map(amx, amy) then lgm_omx, lgm_omy = amx, amy end
      end
      local out, blk, act, pot, unr
      local _cak = _get_flat and c._ak
      local _pf = _cak and _get_flat(_cak, nudge_wu, ai)
      if _pf then
        -- Fast path: use stamp for outgoing (avoids simulate_shot in score_aim).
        out, blk, act, pot, unr = score_aim(
          nwx, nwy, lgm_omx, lgm_omy,
          pmx, pmy, pill_wx, pill_wy,
          AIM_OFFSETS[ai], nil, ret, world, no_builder, nil, _pf, nil)
      else
        out, blk, act, pot, unr = score_aim(
          nwx, nwy, lgm_omx, lgm_omy,
          pmx, pmy, pill_wx, pill_wy,
          AIM_OFFSETS[ai], nil, ret, world, no_builder)
      end
      local a_list = act or {}
      local p_list = pot or {}
      aim_ring[ci] = {
        tiles                = out,
        blocked              = blk,
        actual_blockers      = a_list,
        potential_blockers   = p_list,
        unreachable_blockers = unr or {},
      }
      -- Populate slate nudge slot for this (ci, nudge, ai).
      if na_shield then
        local ni = nudge_wu // NUDGE_STEP_WU
        na_shield.slate_set(ci, ni, ai - 1, blk and true or false,
          #a_list,
          a_list[1] and a_list[1].mx - pmx or 0, a_list[1] and a_list[1].my - pmy or 0,
          a_list[2] and a_list[2].mx - pmx or 0, a_list[2] and a_list[2].my - pmy or 0,
          a_list[3] and a_list[3].mx - pmx or 0, a_list[3] and a_list[3].my - pmy or 0,
          a_list[4] and a_list[4].mx - pmx or 0, a_list[4] and a_list[4].my - pmy or 0,
          a_list[5] and a_list[5].mx - pmx or 0, a_list[5] and a_list[5].my - pmy or 0,
          #p_list,
          p_list[1] and p_list[1].mx - pmx or 0, p_list[1] and p_list[1].my - pmy or 0,
          p_list[2] and p_list[2].mx - pmx or 0, p_list[2] and p_list[2].my - pmy or 0,
          p_list[3] and p_list[3].mx - pmx or 0, p_list[3] and p_list[3].my - pmy or 0,
          p_list[4] and p_list[4].mx - pmx or 0, p_list[4] and p_list[4].my - pmy or 0,
          p_list[5] and p_list[5].mx - pmx or 0, p_list[5] and p_list[5].my - pmy or 0)
      end
    end
    ring_d[ai] = aim_ring
    return aim_ring
  end

  local function pill_in_tiles(tiles)
    if not tiles then return false end
    for _, t in ipairs(tiles) do
      if t.mx == pmx and t.my == pmy then return true end
    end
    return false
  end
  -- ── End nudge infrastructure ─────────────────────────────────────────────

  if na_shield then na_shield.slate_clear(#candidates) end

  local _t_score_cands = clock_us()
  for ci, c in ipairs(candidates) do
    score_candidate(c, pill, world, pill_wx, pill_wy, no_builder)
    -- Populate slate with base (nudge=0) blocker data for neighbor bonus.
    if na_shield then
      for ai = 1, #AIM_OFFSETS do
        local a = c.aims and c.aims[ai]
        if a then
          local act, pot = a.blockers or {}, a.potential_blockers or {}
          na_shield.slate_set(ci, 0, ai - 1, a.blocked and true or false,
            #act,
            act[1] and act[1].mx - pmx or 0, act[1] and act[1].my - pmy or 0,
            act[2] and act[2].mx - pmx or 0, act[2] and act[2].my - pmy or 0,
            act[3] and act[3].mx - pmx or 0, act[3] and act[3].my - pmy or 0,
            act[4] and act[4].mx - pmx or 0, act[4] and act[4].my - pmy or 0,
            act[5] and act[5].mx - pmx or 0, act[5] and act[5].my - pmy or 0,
            #pot,
            pot[1] and pot[1].mx - pmx or 0, pot[1] and pot[1].my - pmy or 0,
            pot[2] and pot[2].mx - pmx or 0, pot[2] and pot[2].my - pmy or 0,
            pot[3] and pot[3].mx - pmx or 0, pot[3] and pot[3].my - pmy or 0,
            pot[4] and pot[4].mx - pmx or 0, pot[4] and pot[4].my - pmy or 0,
            pot[5] and pot[5].mx - pmx or 0, pot[5] and pot[5].my - pmy or 0)
        end
      end
    end
  end
  local _t_nudge_start = clock_us()

  -- ── Nudge pass ───────────────────────────────────────────────────────────
  -- For each aim where the pill tile is absent from the simulated trajectory,
  -- find the smallest forward nudge that puts it in the path and replace the
  -- aim's data with the nudged result.  Aims that still miss the pill at max
  -- nudge are marked blocked.  Candidate scores are recalculated afterward.
  for ci, c in ipairs(candidates) do
    -- Precompute per-candidate pill_hit base index (angle stride).
    -- _pill_hit[(base + nudge_idx * _PH_AIMS + aim_idx + 1)] = true/false
    local _ph_base
    if _pill_hit then
      local deg = c.deg % 360
      if deg < 0 then deg = deg + 360 end
      _ph_base = math.floor(deg * 4) * _PH_STRIDE
    end
    local rescore = false
    for ai = 1, #AIM_OFFSETS do
      local aim = c.aims and c.aims[ai]
      -- Check if base position already hits the pill.
      local aim_hits = aim and aim.tiles and pill_in_tiles(aim.tiles)
      if not aim_hits and _ph_base then
        aim_hits = _pill_hit[_ph_base + ai + 1]  -- ni=0: no nudge
      end
      if aim and not aim.blocked and not aim_hits then
        local found_wu = nil
        for step = 1, NUDGE_MAX_STEPS do
          local d = step * NUDGE_STEP_WU
          -- Fast pill-hit check: direct Lua table read, no C call overhead.
          -- _NUDGE_IDX[d] is nil for wu beyond stamp range → fall back.
          local hit
          if _ph_base then
            local ni = _NUDGE_IDX[d]
            if ni then hit = _pill_hit[_ph_base + ni * _PH_AIMS + ai + 1] end
          end
          if hit == nil then
            local ring  = ensure_nudge_ring(d, ai)
            local entry = ring[ci]
            hit = entry and pill_in_tiles(entry.tiles)
          end
          if hit then found_wu = d; break end
        end
        if found_wu then
          ensure_nudge_ring(found_wu, ai)  -- build ring for scoring (cached if already built)
          local entry    = nudge_rings[found_wu][ai][ci]
          local actual_n = #entry.actual_blockers
          local pot_n    = #entry.potential_blockers
          local sc = 0
          if not entry.blocked then
            sc = M.SCORE_PER_SLOT * (actual_n + pot_n)
               + M.BUILT_BONUS    * actual_n
          end
          c.aims[ai] = {
            tiles                = entry.tiles,
            blocked              = entry.blocked,
            blockers             = entry.actual_blockers,
            potential_blockers   = entry.potential_blockers,
            unreachable_blockers = entry.unreachable_blockers,
            score                = sc,
            nudge_wu             = found_wu,
          }
          -- Record which nudge slot to use in the neighbor check.
          if na_shield then
            na_shield.slate_set_nudge_used(ci, ai - 1, found_wu // NUDGE_STEP_WU)
          end
        else
          -- Pill unreachable even at max nudge — discard this aim.
          aim.blocked = true
          aim.score   = 0
        end
        rescore = true
      end
    end
    if rescore then
      local best_sc, best_ai = 0, nil
      for ai = 1, #AIM_OFFSETS do
        local aim = c.aims and c.aims[ai]
        if aim and not aim.blocked and (aim.score or 0) > best_sc then
          best_sc = aim.score
          best_ai = ai
        end
      end
      c.score        = best_sc
      c.best_aim_idx = best_ai
      if best_ai then
        local a = c.aims[best_ai]
        c.score_actual    = (M.SCORE_PER_SLOT + M.BUILT_BONUS) * #a.blockers
        c.score_potential = M.SCORE_PER_SLOT * #a.potential_blockers
      else
        c.score_actual    = 0
        c.score_potential = 0
      end
      c.score_neighbor = 0
    end
  end
  -- ── End nudge pass ───────────────────────────────────────────────────────
  local _t_neighbor_start = clock_us()

  -- Hoisted so the scan() return can include them for viz regardless of path.
  local pill_hp         = pill.health or 0
  local fav_bonus_by_size = {}
  local min_chain       = 0
  local tier_label      = "NONE"

  if na_shield then
    -- ── C neighbor bonus ─────────────────────────────────────────────────
    if pill_hp >= M.WOUNDED_HP_FULL_MIN and pill_hp <= M.WOUNDED_HP_FULL_MAX then
      fav_bonus_by_size[M.WOUNDED_HP_FULL_FAV2_BLOCKERS] = M.WOUNDED_HP_FULL_BONUS2
      fav_bonus_by_size[M.WOUNDED_HP_FULL_FAV3_BLOCKERS] = M.WOUNDED_HP_FULL_BONUS3
      min_chain  = M.WOUNDED_HP_FULL_MIN_CHAIN
      tier_label = "FULL"
    elseif pill_hp >= M.WOUNDED_HP_HIGH_MIN and pill_hp <= M.WOUNDED_HP_HIGH_MAX then
      fav_bonus_by_size[M.WOUNDED_HP_HIGH_FAV_BLOCKERS] = M.WOUNDED_FAVOR_BONUS
      min_chain  = M.WOUNDED_HP_HIGH_MIN_CHAIN
      tier_label = "HIGH"
    elseif pill_hp <= M.WOUNDED_HP_LOW_MAX then
      fav_bonus_by_size[M.WOUNDED_HP_LOW_FAV_BLOCKERS] = M.WOUNDED_FAVOR_BONUS
      min_chain  = M.WOUNDED_HP_LOW_MIN_CHAIN
      tier_label = "LOW"
    end
    local max_bonus = M.WOUNDED_FAVOR_BONUS
    for _, b in pairs(fav_bonus_by_size) do if b > max_bonus then max_bonus = b end end
    -- Flatten fav_bonus_by_size into up to 2 (size, bonus) pairs for C.
    local n_fav, fs1, fb1, fs2, fb2 = 0, 0, 0.0, 0, 0.0
    for sz, bon in pairs(fav_bonus_by_size) do
      if n_fav == 0 then fs1, fb1 = sz, bon
      else fs2, fb2 = sz, bon end
      n_fav = n_fav + 1
    end

    local nb = na_shield.run_neighbor_bonus(
      #candidates, #AIM_OFFSETS,
      M.SCORE_PER_SLOT, M.BUILT_BONUS, M.NEIGHBOR_BONUS,
      n_fav, fs1, fb1, fs2, fb2, min_chain, max_bonus)

    local RS = 29  -- RESULT_STRIDE (9 scalars + 5*2 actual + 5*2 potential)
    for ci = 1, #candidates do
      local c    = candidates[ci]
      local base = (ci - 1) * RS
      local bai  = nb[base + 1]   -- best aim idx (1-based), 0 = no winner
      if bai > 0 then
        c.best_aim_idx    = bai
        c.score           = nb[base + 2]
        c.score_neighbor  = nb[base + 3]
        c.best_chain_len  = nb[base + 4]
        c.score_actual    = nb[base + 5]
        c.score_potential = nb[base + 6]
        c.blockers_count  = nb[base + 7]
        -- Reconstruct winning blocker tables from relative offsets.
        -- Layout: positions 10..19 = interleaved act dx/dy (5 pairs),
        --         positions 20..29 = interleaved pot dx/dy (5 pairs).
        local n_act = nb[base + 8]
        local n_pot = nb[base + 9]
        local act, pot = {}, {}
        for k = 0, n_act - 1 do
          act[k+1] = { mx = pmx + nb[base + 10 + k*2], my = pmy + nb[base + 11 + k*2] }
        end
        for k = 0, n_pot - 1 do
          pot[k+1] = { mx = pmx + nb[base + 20 + k*2], my = pmy + nb[base + 21 + k*2] }
        end
        c.aims[bai].blockers           = act
        c.aims[bai].potential_blockers = pot
      else
        c.best_aim_idx    = nil
        c.score           = 0
        c.score_neighbor  = 0
        c.best_chain_len  = 0
        c.score_actual    = 0
        c.score_potential = 0
        c.blockers_count  = 0
      end
    end

  else
  -- ── Lua neighbor bonus (fallback when C slate not available) ─────────
  local fav_bonus_by_size = {}
  local min_chain = 0
  local tier_label = "NONE"

  local bk_cache = {}
  local function blocker_keys_for(i, ai, nudge_wu)
    nudge_wu = nudge_wu or 0
    -- Composite cache key: nudge_wu shifts into the high bits to avoid
    -- aliasing with plain ai values (max AIM_OFFSETS is 5).
    local ck  = ai + nudge_wu * 16
    local row = bk_cache[i]
    if row and row[ck] then return row[ck] end
    if not row then row = {}; bk_cache[i] = row end
    local set = {}
    local blockers, potentials
    if nudge_wu > 0 then
      local ring_ai = nudge_rings[nudge_wu] and nudge_rings[nudge_wu][ai]
      local entry   = ring_ai and ring_ai[i]
      if entry then
        blockers  = entry.actual_blockers
        potentials = entry.potential_blockers
      end
    else
      local c = candidates[i]
      local a = c and c.aims and c.aims[ai]
      if a then
        blockers  = a.blockers
        potentials = a.potential_blockers
      end
    end
    for _, b in ipairs(blockers  or {}) do set[b.my * 256 + b.mx] = true end
    for _, b in ipairs(potentials or {}) do set[b.my * 256 + b.mx] = true end
    row[ck] = set
    return set
  end
  local function subset(a_set, b_set)
    for k in pairs(a_set) do
      if not b_set[k] then return false end
    end
    return true
  end

  -- Per-aim, per-SUBSET chain evaluation. For each candidate we
  -- enumerate every non-empty subset of the aim's combined blocker
  -- list (actual ∪ potential) and chain-walk neighbors that have
  -- THIS subset as part of their own blocker set. The (aim, subset)
  -- with the highest total wins — meaning a candidate with 3
  -- blockers might win with subset {A} (cheap to set up + many
  -- forgiving neighbors) over subset {A,B,C} (more cover but few
  -- neighbors share the exact triple). Tunable via NEIGHBOR_BONUS
  -- and SCORE_PER_SLOT / BUILT_BONUS.
  --
  -- Subset count cap MAX_SUBSET_BLOCKERS=3 → max 7 subsets per
  -- corner per candidate (singletons 3, pairs 3, triple 1).
  -- Above 3 blockers we degrade to "use the full set only" — no
  -- known PPT geometry actually has >3 walls in a single setup.
  -- Bitmask iteration: mask 1..(2^N - 1).
  local MAX_SUBSET_BLOCKERS = 3

  -- Wounded-pill bias setup (one-shot per scan). pill.armour is
  -- the current HP; ranges chosen to bracket "soft / very-soft".
  -- favored_blockers = 0 means "no preference" (don't apply bonus).
  -- min_chain is a HARD floor: subsets with chain < this are
  -- excluded outright.
  local pill_hp = pill.health or 0
  -- Map of subset_size -> bonus for the active tier. Allows multiple
  -- favored sizes in one tier (e.g. full-HP gets 2-blocker AND
  -- 3-blocker bonuses, with 3 being bigger).
  local fav_bonus_by_size = {}
  local min_chain = 0
  local tier_label = "NONE"
  if pill_hp >= M.WOUNDED_HP_FULL_MIN and pill_hp <= M.WOUNDED_HP_FULL_MAX then
    fav_bonus_by_size[M.WOUNDED_HP_FULL_FAV2_BLOCKERS] = M.WOUNDED_HP_FULL_BONUS2
    fav_bonus_by_size[M.WOUNDED_HP_FULL_FAV3_BLOCKERS] = M.WOUNDED_HP_FULL_BONUS3
    min_chain  = M.WOUNDED_HP_FULL_MIN_CHAIN
    tier_label = "FULL"
  elseif pill_hp >= M.WOUNDED_HP_HIGH_MIN and pill_hp <= M.WOUNDED_HP_HIGH_MAX then
    fav_bonus_by_size[M.WOUNDED_HP_HIGH_FAV_BLOCKERS] = M.WOUNDED_FAVOR_BONUS
    min_chain  = M.WOUNDED_HP_HIGH_MIN_CHAIN
    tier_label = "HIGH"
  elseif pill_hp <= M.WOUNDED_HP_LOW_MAX then
    fav_bonus_by_size[M.WOUNDED_HP_LOW_FAV_BLOCKERS] = M.WOUNDED_FAVOR_BONUS
    min_chain  = M.WOUNDED_HP_LOW_MIN_CHAIN
    tier_label = "LOW"
  end
  -- Low-armour shield demote: when our own HP is below ARMOUR_LOW we
  -- can't soak the extra return fire that comes from a too-thin shield,
  -- so raise the min_chain floor by 1 (cap 3) regardless of pill HP.
  if tank_armour and tank_armour < (C.ARMOUR_LOW or 15) then
    min_chain = math.min(min_chain + 1, 3)
  end
  -- Largest bonus in the tier — drives the hard-exclusion magnitude
  -- so an excluded subset always loses against any favored one.
  local max_bonus = 0
  for _, b in pairs(fav_bonus_by_size) do
    if b > max_bonus then max_bonus = b end
  end
  if max_bonus == 0 then max_bonus = M.WOUNDED_FAVOR_BONUS end

  for i = 1, #candidates do
    local c = candidates[i]
    if c.kind == "candidate" then
      local best_total           = -1
      local best_aim             = nil
      local best_chain           = 0
      local best_subset_keys     = nil
      local best_subset_actual   = nil
      local best_subset_potential = nil
      -- Per-eval log so the viz_detail dialog can surface every
      -- (aim, subset) pair we considered with its chain count and
      -- subtotal. The dialog formats these as body lines.
      local chain_evals = {}
      for ai = 1, #AIM_OFFSETS do
        local a = c.aims and c.aims[ai]
        if a and not a.blocked and (a.score or 0) > 0 then
          -- Combined blocker list with kind tags so per-subset
          -- score knows actual-vs-potential weights.
          local all_blockers = {}
          for _, b in ipairs(a.blockers or {}) do
            all_blockers[#all_blockers + 1] =
              { mx = b.mx, my = b.my, key = b.my * 256 + b.mx, actual = true,
                ref = b }
          end
          for _, b in ipairs(a.potential_blockers or {}) do
            all_blockers[#all_blockers + 1] =
              { mx = b.mx, my = b.my, key = b.my * 256 + b.mx, actual = false,
                ref = b }
          end
          local n = #all_blockers
          if n > 0 then
            local n_iter = math.min(n, MAX_SUBSET_BLOCKERS)
            local fixed_count = n - n_iter   -- entries beyond the cap are
                                             -- always included (degenerate
                                             -- "use full set" behavior for
                                             -- pathological N>3 cases)
            local mask_max = (1 << n_iter) - 1
            for mask = 1, mask_max do
              local subset_set      = {}
              local actual_n        = 0
              local potential_n     = 0
              local subset_actual   = {}
              local subset_potential = {}
              -- Bitmasked entries
              for idx = 1, n_iter do
                if (mask & (1 << (idx - 1))) ~= 0 then
                  local b = all_blockers[idx]
                  subset_set[b.key] = true
                  if b.actual then
                    actual_n = actual_n + 1
                    subset_actual[#subset_actual + 1] = b.ref
                  else
                    potential_n = potential_n + 1
                    subset_potential[#subset_potential + 1] = b.ref
                  end
                end
              end
              -- Always-included entries (only fires when n > MAX)
              for idx = n_iter + 1, n do
                local b = all_blockers[idx]
                subset_set[b.key] = true
                if b.actual then
                  actual_n = actual_n + 1
                  subset_actual[#subset_actual + 1] = b.ref
                else
                  potential_n = potential_n + 1
                  subset_potential[#subset_potential + 1] = b.ref
                end
              end
              -- Score this subset against all neighbors.
              -- If this aim was nudged, test each neighbor from the same
              -- nudge distance (using the pre-built ring) so the geometry
              -- is comparable.
              local nudge = (c.aims[ai] and c.aims[ai].nudge_wu) or 0
              local function covers(j)
                local nc = candidates[j]
                if not nc or nc.kind ~= "candidate" then return false end
                if nudge > 0 then
                  local re = nudge_rings[nudge] and nudge_rings[nudge][ai]
                             and nudge_rings[nudge][ai][j]
                  if not re or re.blocked then return false end
                else
                  local n_a = nc.aims and nc.aims[ai]
                  if not n_a or n_a.blocked then return false end
                end
                return subset(subset_set, blocker_keys_for(j, ai, nudge))
              end
              -- Symmetric chain: only count out as far as BOTH
               -- sides extend. A 8-left/4-right run scores like 4+4,
               -- not 12. Favors candidates with margin in both
               -- directions — nudges off cliff-edges of the chain.
              local left = 0
              for j = i - 1, 1, -1 do
                if not covers(j) then break end
                left = left + 1
              end
              local right = 0
              for j = i + 1, #candidates do
                if not covers(j) then break end
                right = right + 1
              end
              local sym = left < right and left or right
              local chain = sym * 2
              local subset_aim_score =
                (M.SCORE_PER_SLOT + M.BUILT_BONUS) * actual_n
                + M.SCORE_PER_SLOT * potential_n
              local total = subset_aim_score + chain * M.NEIGHBOR_BONUS
              -- Wounded-pill bias: bonus for the "favored" subset
              -- size, exclusion if chain is below the floor. Stored
              -- as bias on the eval row so the dialog shows it.
              local bias = 0
              local subset_size = actual_n + potential_n
              local size_bonus = fav_bonus_by_size[subset_size]
              if size_bonus then
                bias = bias + size_bonus
              end
              if min_chain > 0 and chain < min_chain then
                -- Hard exclusion: -inf-ish so this subset can never win.
                bias = bias - max_bonus * 100
              end
              total = total + bias
              chain_evals[#chain_evals + 1] = {
                aim          = ai,
                actual_n     = actual_n,
                potential_n  = potential_n,
                actual_list  = subset_actual,
                potential_list = subset_potential,
                aim_score    = subset_aim_score,
                chain        = chain,
                wounded_bias = bias,
                total        = total,
              }
              if total > best_total then
                best_total            = total
                best_aim              = ai
                best_chain            = chain
                best_subset_keys      = subset_set
                best_subset_actual    = subset_actual
                best_subset_potential = subset_potential
              end
            end
          end
        end
      end
      c._chain_evals = chain_evals   -- viz_detail reads this
      if best_aim then
        c.best_aim_idx   = best_aim
        c.score          = best_total
        c.score_neighbor = best_chain * M.NEIGHBOR_BONUS
        c.best_chain_len = best_chain
        c.score_actual   = (M.SCORE_PER_SLOT + M.BUILT_BONUS) * #best_subset_actual
        c.score_potential = M.SCORE_PER_SLOT * #best_subset_potential
        c.blockers_count = #best_subset_actual + #best_subset_potential
        -- Store the winning subset so the wall-builder commits to
        -- exactly those tiles (and doesn't waste LGM time building
        -- potentials we decided weren't worth the rigid setup).
        -- Overwrite the aim's potential_blockers list with the
        -- chosen subset so downstream code that already reads
        -- `aims[best_aim_idx].potential_blockers` automatically
        -- uses the winning slice.
        c.aims[best_aim].potential_blockers = best_subset_potential
        -- Same for actual blockers (might also be a strict subset
        -- of the original list when N > MAX_SUBSET_BLOCKERS — for
        -- N <= 3 it'll be == the original).
        c.aims[best_aim].blockers = best_subset_actual
        c.aims[best_aim].subset_keys = best_subset_keys
      end
    end
  end
  end  -- end else (Lua neighbor bonus fallback)

  local _t_end = clock_us()
  if BRAIN_PROFILE_LOG then
    if _t_end - _t_score_cands > 3000 then
      opt.append("optimize.log", string.format(
        "  [shield] scan total=%.2f ms  score_cands=%.2f ms  nudge=%.2f ms  neighbor=%.2f ms  stamp=%s",
        (_t_end - _t_score_cands) / 1000,
        (_t_nudge_start - _t_score_cands) / 1000,
        (_t_neighbor_start - _t_nudge_start) / 1000,
        (_t_end - _t_neighbor_start) / 1000,
        _pill_hit and "yes" or "no"))
    end
  end

  local best
  for _, c in ipairs(candidates) do
    if c.score > 0 and (not best or c.score > best.score) then
      best = c
    end
  end

  return {
    candidates    = candidates,
    best          = best,
    standoff      = candidates[1],
    pill          = pill,
    pill_hp           = pill_hp,
    fav_bonus_by_size = fav_bonus_by_size,
    min_chain         = min_chain,
    tier_label        = tier_label,
  }
end

function M.load_stamp_bin(path)
  if not na_shield then
    print("[shield] na_shield C module not available")
    return false
  end
  local dirs = { _G.BRAIN_DIR, _G.DEBUG_SESSION_DIR, "." }
  local tried = {}
  local paths = path and { path } or (function()
    local seen, out = {}, {}
    for _, dir in ipairs(dirs) do
      if dir and not seen[dir] then
        seen[dir] = true
        out[#out + 1] = dir .. "/shield_stamp_cache.bin"
      end
    end
    return out
  end)()
  for _, p in ipairs(paths) do
    tried[#tried + 1] = p
    if na_shield.load(p) then
      _pill_hit = na_shield.pill_hit  -- direct table reference; nil if C didn't build it
      print("[shield] stamp cache loaded (binary): " .. p)
      return true
    end
  end
  print("[shield] no binary stamp cache found, cpf.simulate_shot fallback active")
  return false
end

function M.compute_shield_stamps()
  local STEP_WU   = 8
  local MAX_STEPS = 32   -- 0..32 = 33 nudge slots (up from 4)
  local MAX_TILES = 15   -- matches ShieldShotPath.tiles[15]
  local R         = C.ATTACK_PILL_STANDOFF
  local pmx, pmy  = 128, 128
  local pill_wx   = (pmx << 8) | 128
  local pill_wy   = (pmy << 8) | 128

  print("[shield] computing " .. 1440 .. " × " .. (MAX_STEPS + 1) ..
        " × 6 stamp entries …")
  local stamps = {}

  local function make_path(sim, src_x, src_y, tgt_x, tgt_y)
    local p = { source_x = src_x, source_y = src_y,
                target_x = tgt_x, target_y = tgt_y,
                has_pill = false, tiles = {} }
    if sim then
      for _, t in ipairs(sim) do
        local dx, dy = t.mx - pmx, t.my - pmy
        if dx == 0 and dy == 0 then p.has_pill = true end
        if #p.tiles < MAX_TILES then p.tiles[#p.tiles+1] = {dx, dy} end
      end
    end
    return p
  end

  for ak = 0, 1439 do
    local deg = ak / 4.0
    local rad = math.rad(deg)
    local scx = pmx + 0.5 + math.sin(rad) * R
    local scy = pmy + 0.5 - math.cos(rad) * R

    local by_step = {}
    stamps[ak] = by_step

    for step = 0, MAX_STEPS do
      local nudge_wu = step * STEP_WU
      local spot_wx, spot_wy
      if nudge_wu == 0 then
        spot_wx = math.floor(scx * 256 + 0.5)
        spot_wy = math.floor(scy * 256 + 0.5)
      else
        local dx   = (pmx + 0.5) - scx
        local dy   = (pmy + 0.5) - scy
        local d    = math.sqrt(dx * dx + dy * dy)
        local frac = (nudge_wu / 256.0) / d
        spot_wx = math.floor((scx + dx * frac) * 256 + 0.5)
        spot_wy = math.floor((scy + dy * frac) * 256 + 0.5)
      end

      local entry = { aim = {} }
      by_step[step] = entry

      -- Return fire: pill → standoff (source = pill at offset 0,0)
      entry.pill_to_tank = make_path(
        cpf.simulate_shot(pill_wx, pill_wy, spot_wx, spot_wy, cpf.SHOT_PILL, 0),
        0, 0, spot_wx - pill_wx, spot_wy - pill_wy)

      -- Outgoing aims 1..5: standoff → pill aim point
      for ai, aim in ipairs(AIM_OFFSETS) do
        local twx = (pmx << 8) + aim[1]
        local twy = (pmy << 8) + aim[2]
        entry.aim[ai] = make_path(
          cpf.simulate_shot(spot_wx, spot_wy, twx, twy, cpf.SHOT_TANK, 0),
          spot_wx - pill_wx, spot_wy - pill_wy,
          twx - pill_wx, twy - pill_wy)
      end
    end

    if ak % 144 == 0 then
      print(string.format("[shield] stamp progress: %d/1440 (%.0f%%)", ak, ak / 14.4))
    end
  end

  print("[shield] stamp computation done.")
  return {
    standoff        = R,
    aim_inset       = M.AIM_INSET,
    step_deg        = M.STEP_DEG,
    nudge_step_wu   = STEP_WU,
    max_nudge_steps = MAX_STEPS,
    stamps          = stamps,
  }
end

-- Binary format v2 matches ShieldStampHeader / ShieldStampEntry in na_shield_stamp.c.
-- Header (32 bytes): magic(I4) version(I4) n_angles(I4) n_nudges(I4) nudge_step_wu(I4)
--                    standoff(f) aim_inset(f) step_deg(f)
-- Entry (240 bytes): aim[5] ShieldShotPath + pill_to_tank ShieldShotPath
-- ShieldShotPath (40 bytes): source_x(i2) source_y(i2) target_x(i2) target_y(i2)
--                            has_pill(B) n_tiles(B) tiles[15] pairs(b,b)
function M.save_shield_stamps_bin(data, outpath)
  local path      = outpath or "shield_stamp_cache.bin"
  local N_ANGLES  = 1440
  local MAX_TILES = 15   -- ShieldShotPath.tiles[15]
  local STEP_WU   = data.nudge_step_wu
  local N_NUDGES  = data.max_nudge_steps + 1  -- 0..max inclusive

  local f, err = io.open(path, "wb")
  if not f then
    print("[shield] cannot write binary stamp: " .. tostring(err))
    return false
  end

  -- Header (32 bytes)
  f:write(string.pack("<I4I4I4I4I4fff",
    0x444C4853, 2, N_ANGLES, N_NUDGES, STEP_WU,
    data.standoff or 0.0, data.aim_inset or 0.0, data.step_deg or 0.0))

  local path_fmt = "<i2i2i2i2BB" .. string.rep("b", MAX_TILES * 2)
  local tile_buf = {}

  local function write_path(p)
    local n = p and math.min(#p.tiles, MAX_TILES) or 0
    for i = 1, MAX_TILES * 2 do tile_buf[i] = 0 end
    for i = 1, n do
      tile_buf[2*i-1] = p.tiles[i][1]   -- dx
      tile_buf[2*i]   = p.tiles[i][2]   -- dy
    end
    f:write(string.pack(path_fmt,
      p and p.source_x or 0, p and p.source_y or 0,
      p and p.target_x or 0, p and p.target_y or 0,
      (p and p.has_pill) and 1 or 0, n,
      table.unpack(tile_buf)))
  end

  -- Entries: laid out as [angle_key * n_nudges + nudge_idx], each 240 bytes.
  -- aim[0..4] = outgoing AIM_OFFSETS[1..5], then pill_to_tank = return fire.
  for ak = 0, N_ANGLES - 1 do
    local by_step = data.stamps[ak]
    for step = 0, data.max_nudge_steps do
      local entry = by_step and by_step[step]
      for ai = 1, 5 do
        write_path(entry and entry.aim and entry.aim[ai])
      end
      write_path(entry and entry.pill_to_tank)
    end
  end

  f:close()
  print(string.format(
    "[shield] binary stamp v2 written: %s  (%d angles × %d nudges × 6 paths)",
    path, N_ANGLES, N_NUDGES))
  return true
end

-- Draw helpers --------------------------------------------------------------

-- Per-tile thin colored border via two stacked rects (slight inset for the
-- inner one). Useful for layering multiple aim borders on the same tile.
local function border_box(viz_id, mx, my, inset, r, g, b, a)
  viz.rect(viz_id, mx + inset, my + inset, mx + 1 - inset, my + 1 - inset,
           r, g, b, a, false)
end

function M.draw_overlay(scan, now_tick)
  if not BRAIN_DEBUG_MODE then return end
  -- Need at least one of: candidates array (Lua scan path) or a best
  -- winner / standoff fallback (C scan path).  The C path doesn't
  -- surface scan.candidates; Pass A/B short-circuit on nil so only
  -- Pass C (the winner's blocker borders) draws — which is what we
  -- want.  Without this looser gate, the entire blocker viz silently
  -- disappeared whenever scan_c was used.
  if not scan or (not scan.candidates and not scan.best and not scan.standoff) then
    return
  end

  -- After NONWINNER_FADE_TICKS, hide everything but the chosen viz
  -- target so the screen de-clutters once the user has had time to
  -- read the grid. Winner (or standoff fallback) keeps its full viz.
  local hide_losers = false
  if now_tick and scan.created_tick and
     (now_tick - scan.created_tick) > M.NONWINNER_FADE_TICKS then
    hide_losers = true
  end
  local kept_target = scan.best or scan.standoff

  -- Pass A: register viz_detail entries for ALL candidates regardless
  -- of the hide_losers fade. The fade only de-clutters the on-screen
  -- markers + score labels; the inspector dialog should always have
  -- every candidate available so the user can scrub the chain-eval
  -- breakdown for losers too. Geometry mirrors the visible marker
  -- (small circle at cx/cy), so a click on the marker still hit-tests
  -- to its entry — but even after fade, clicking the spot will land
  -- on the registered hit area.
  if viz.detail_circle and scan.candidates then
    for ci, c in ipairs(scan.candidates) do
      local did = string.format("shield_cand_%d", ci)
      local kind_str = c.kind == "standoff" and "STANDOFF" or "candidate"
      local hdr = string.format("%s score=%d @ deg=%.1f off=%+.1f  %d = %d + %d + %d[%d]",
                                kind_str,
                                math.floor(c.score or 0),
                                c.deg or 0, c.offset_deg or 0,
                                math.floor(c.score or 0),
                                c.score_potential or 0,
                                c.score_actual or 0,
                                c.score_neighbor or 0,
                                c.best_chain_len or 0)
      viz.detail_circle(did, c.cx, c.cy, 0.04, hdr)
      viz.detail_text(did, string.format("score: total=%d (a=%d p=%d n=%d)",
        math.floor(c.score or 0),
        c.score_actual or 0, c.score_potential or 0, c.score_neighbor or 0))
      if scan.fav_bonus_by_size and next(scan.fav_bonus_by_size) then
        local parts = {}
        local sizes = {}
        for k, _ in pairs(scan.fav_bonus_by_size) do sizes[#sizes+1] = k end
        table.sort(sizes)
        for _, sz in ipairs(sizes) do
          parts[#parts+1] = string.format("%d->+%d", sz, scan.fav_bonus_by_size[sz])
        end
        viz.detail_text(did, string.format(
          "wounded bias: pill_hp=%d tier=%s min_chain=%d  blockers: %s",
          scan.pill_hp or 0, scan.tier_label or "?", scan.min_chain or 0,
          table.concat(parts, ", ")))
      else
        viz.detail_text(did, string.format(
          "wounded bias: pill_hp=%d -> NONE (HP > %d)",
          scan.pill_hp or 0, M.WOUNDED_HP_FULL_MAX))
      end
      viz.detail_text(did, string.format("position: cx=%.4f cy=%.4f wu=(%d,%d) tile=(%d,%d)",
        c.cx, c.cy, math.floor(c.cx*256+0.5), math.floor(c.cy*256+0.5),
        c.mx or 0, c.my or 0))
      if c.best_aim_idx then
        local aim_name = M.AIM_NAMES[c.best_aim_idx] or tostring(c.best_aim_idx)
        viz.detail_text(did, string.format("winning aim: idx=%d (%s)",
          c.best_aim_idx, aim_name))
        local a = c.aims and c.aims[c.best_aim_idx]
        if a then
          if (a.nudge_wu or 0) > 0 then
            viz.detail_text(did, string.format(
              "  nudge: %d wu (%.3f tiles) — origin moved toward pill to hit it",
              a.nudge_wu, a.nudge_wu / 256.0))
          end
          local n_unreach = #(a.unreachable_blockers or {})
          viz.detail_text(did, string.format(
            "  blockers: actual=%d potential=%d  (LGM-unreachable dropped: %d)",
            #(a.blockers or {}), #(a.potential_blockers or {}), n_unreach))
          for _, b in ipairs(a.blockers or {}) do
            viz.detail_text(did, string.format("    actual      @ tile (%d,%d)", b.mx, b.my))
          end
          for _, b in ipairs(a.potential_blockers or {}) do
            viz.detail_text(did, string.format("    potential   @ tile (%d,%d)", b.mx, b.my))
          end
          for _, b in ipairs(a.unreachable_blockers or {}) do
            viz.detail_text(did, string.format(
              "    UNREACHABLE @ tile (%d,%d) (LGM origin (%d,%d) -> dest unreachable per cpf.lgm_travel_ticks_map)",
              b.mx, b.my, b.origin_mx or -1, b.origin_my or -1))
          end
        end
      else
        viz.detail_text(did, "no winning aim (all blocked or scored 0)")
      end
      local evals = c._chain_evals
      if evals and #evals > 0 then
        viz.detail_text(did, string.format("chain evals: %d (aim, subset) combos:", #evals))
        local sorted = {}
        for i, e in ipairs(evals) do sorted[i] = e end
        table.sort(sorted, function(a, b) return (a.total or 0) > (b.total or 0) end)
        for _, e in ipairs(sorted) do
          local aim_name = M.AIM_NAMES[e.aim] or tostring(e.aim)
          local marker = (e.aim == c.best_aim_idx and e.total == c.score) and " *WIN" or ""
          local blocker_strs = {}
          for _, b in ipairs(e.actual_list or {}) do
            blocker_strs[#blocker_strs+1] = string.format("A(%d,%d)", b.mx, b.my)
          end
          for _, b in ipairs(e.potential_list or {}) do
            blocker_strs[#blocker_strs+1] = string.format("P(%d,%d)", b.mx, b.my)
          end
          local bias_str = ""
          if e.wounded_bias and e.wounded_bias ~= 0 then
            bias_str = string.format(" wbias=%+d", e.wounded_bias)
          end
          local aim_data = c.aims and c.aims[e.aim]
          local nudge_str = ""
          if aim_data and (aim_data.nudge_wu or 0) > 0 then
            nudge_str = string.format(" nudge=%dwu", aim_data.nudge_wu)
          end
          viz.detail_text(did, string.format(
            "  aim_idx=%d (%s) a=%d p=%d aim_score=%d chain=%d total=%d%s%s%s",
            e.aim, aim_name, e.actual_n, e.potential_n,
            e.aim_score, e.chain, e.total, bias_str, nudge_str, marker))
          if #blocker_strs > 0 then
            viz.detail_text(did, "    subset: " .. table.concat(blocker_strs, " "))
          end
        end
      else
        viz.detail_text(did, "no chain evals (no non-blocked aims with score>0)")
      end
    end
  end

  -- Pass B: visible markers + score labels. Honors the hide_losers
  -- fade so the on-screen cluster stays clean a few seconds after
  -- the scan is generated.
  for ci, c in ipairs(scan.candidates or {}) do
    if hide_losers and c ~= kept_target then goto next_cand_draw end
    local color_r, color_g, color_b
    if not c.valid_tile then
      color_r, color_g, color_b = 100, 100, 100
    elseif c.best_aim_idx == nil then
      color_r, color_g, color_b = 200, 80, 80
    elseif c.score > 0 then
      color_r, color_g, color_b = 100, 220, 100
    else
      color_r, color_g, color_b = 200, 200, 100
    end

    -- Candidate marker: a 1-wu circle (1/256 tile) at the precise
    -- standoff-arc point. Circles preserve sub-wu precision at any
    -- zoom (rect would floor to game-pixel grid). Standoff gets a
    -- slightly larger ring around it so it's findable in the
    -- cluster.
    local R = 1 / 256.0   -- 1 wu radius
    if c.kind == "standoff" then
      viz.circle("shield_scan_candidates", c.cx, c.cy, 0.04,
                 255, 255, 255, 255)
      viz.circle("shield_scan_candidates", c.cx, c.cy, R,
                 color_r, color_g, color_b, 255)
    else
      viz.circle("shield_scan_candidates", c.cx, c.cy, R,
                 color_r, color_g, color_b, 255)
    end
    -- Score directly on top of the marker, tiny. Format as
    -- "total (A+B+C)" where:
    --   A = self/potential-blocker score (slots we'll build)
    --   B = existing actual-blocker score (walls + friendly pills)
    --   C = neighbor chain bonus
    -- All three always printed (even 0) so the layout is consistent
    -- across the candidate cluster.
    -- Tank wu coords for the candidate (cx/cy are float tile coords).
    local cwx = math.floor(c.cx * 256 + 0.5)
    local cwy = math.floor(c.cy * 256 + 0.5)
    local label = string.format("%d(%d+%d+%d) wu=(%d,%d)",
      math.floor(c.score),
      c.score_potential or 0,
      c.score_actual    or 0,
      c.score_neighbor  or 0,
      cwx, cwy)
    -- Alternate label sides so adjacent candidates' text doesn't
    -- pile on top of each other along the densely-packed standoff
    -- arc. Even index = right side, odd = left side.
    local side = (ci % 2 == 0) and "topleft" or "topright"
    local lx = (ci % 2 == 0) and (c.cx + 0.06) or (c.cx - 0.06)
    viz.text("shield_scan_candidates", lx, c.cy - 0.04,
             label, side, color_r, color_g, color_b, 255, 0.18)

    -- (viz_detail registration moved to Pass A above so loser
     -- candidates still get inspector entries after the visual fade.)
    ::next_cand_draw::
  end

  -- Per-aim blocker visualization for the winner (or for standoff if no
  -- winner) — one colored border per aim that has that tile as a blocker.
  local viz_target = scan.best or scan.standoff
  if viz_target and viz_target.aims then
    -- Brighten the target's marker so the user knows which spot the
    -- blocker borders refer to.
    -- Bright green halo around the chosen target. Sized to be visible
    -- WITHOUT swamping the new 1-gu candidate markers — used to be
    -- 0.9 tile wide which was 14x the new marker size.
    viz.rect("shield_scan_candidates", viz_target.cx - 0.08, viz_target.cy - 0.08,
             viz_target.cx + 0.08, viz_target.cy + 0.08,
             50, 255, 50, 255, false)

    -- Trajectory viz so the user can read the geometry directly:
    --   green CIRCLE on every tile in the tank -> pill-CENTER aim
    --   red SQUARE on every tile in the pill -> tank return path
    --   smaller green CIRCLE on every actual (already-built) blocker
    -- Drawn for the chosen aim's outgoing if the center aim was rejected;
    -- otherwise center aim. Falls back gracefully if either path is nil.
    -- Pick the aim whose trajectory we're drawing: prefer the chosen
    -- best aim (the one that drove the score), fall back to center if
    -- it's not blocked, otherwise just the first aim. So the user
    -- always sees the AIM THAT WON, not just an arbitrary one.
    local pmx = scan.pill and scan.pill.mx or 0
    local pmy = scan.pill and scan.pill.my or 0
    local show_idx = viz_target.best_aim_idx
                  or (viz_target.aims[1] and not viz_target.aims[1].blocked and 1)
                  or 1
    local show_aim = viz_target.aims[show_idx]
    if show_aim and show_aim.tiles then
      for _, t in ipairs(show_aim.tiles) do
        viz.circle("shield_scan_trajectory", t.mx + 0.5, t.my + 0.5, 0.42,
                   60, 220, 60, 200)
      end
    end
    -- Specific line from the winner spot center to the exact aim point
    -- on the pill tile (corner or center). Drawn in the same color as
    -- the per-aim border legend so the user can read which aim won at
    -- a glance.
    if show_aim and not show_aim.blocked then
      local off = M.AIM_OFFSETS_TILE[show_idx] or M.AIM_OFFSETS_TILE[1]
      local col = M.AIM_COLORS[show_idx] or { 60, 220, 60, 255 }
      viz.line("shield_scan_trajectory", viz_target.cx, viz_target.cy,
               pmx + off[1], pmy + off[2],
               col[1], col[2], col[3], 230)
    end
    if viz_target.return_fire and viz_target.return_fire.tiles then
      for _, t in ipairs(viz_target.return_fire.tiles) do
        viz.rect("shield_scan_trajectory", t.mx + 0.08, t.my + 0.08,
                 t.mx + 0.92, t.my + 0.92,
                 230, 60, 60, 90)
        viz.rect("shield_scan_trajectory", t.mx + 0.08, t.my + 0.08,
                 t.mx + 0.92, t.my + 0.92,
                 230, 60, 60, 230, false)
      end
    end
    -- Inner circle on each actual already-built blocker tile (across
    -- all aims, deduped). Smaller than the outgoing-trajectory circles
    -- so they read as a "found a real blocker here" emphasis.
    do
      local seen = {}
      for ai = 1, #viz_target.aims do
        local a = viz_target.aims[ai]
        if a and a.blockers then
          for _, b in ipairs(a.blockers) do
            local key = b.my * 256 + b.mx
            if not seen[key] then
              seen[key] = true
              viz.circle("shield_scan_blockers", b.mx + 0.5, b.my + 0.5, 0.25,
                         60, 255, 60, 255)
            end
          end
        end
      end
    end

    -- Draw per-aim borders. Actual blockers (existing wall/friendly pill)
    -- get a SOLID filled background so they pop. Potential blockers
    -- (empty buildable terrain along the same protection slot) get just
    -- a thin hollow border in the aim's color — these are the tiles
    -- where building a wall would actually help.
    local border_inset_step = 0.05
    for ai = 1, #viz_target.aims do
      local a = viz_target.aims[ai]
      if a then
        local col = M.AIM_COLORS[ai]
        local inset = border_inset_step * ai
        if a.blockers and #a.blockers > 0 then
          for _, b in ipairs(a.blockers) do
            viz.rect("shield_scan_blockers", b.mx + 0.12, b.my + 0.12,
                     b.mx + 0.88, b.my + 0.88,
                     col[1], col[2], col[3], 110)
            border_box("shield_scan_blockers", b.mx, b.my, inset, col[1], col[2], col[3], col[4])
          end
        end
        if a.potential_blockers and #a.potential_blockers > 0 then
          for _, b in ipairs(a.potential_blockers) do
            border_box("shield_scan_blockers", b.mx, b.my, inset,
                       col[1], col[2], col[3], math.floor(col[4] * 0.55))
          end
        end
        -- LGM-unreachable buildable tiles: red X-style cross + dim
        -- border so the user can see why a candidate's score is lower
        -- than expected (the slot exists geometrically but the LGM
        -- can't get to it from the standoff).
        if a.unreachable_blockers and #a.unreachable_blockers > 0 then
          for _, b in ipairs(a.unreachable_blockers) do
            viz.line("shield_scan_blockers",
                     b.mx + 0.15, b.my + 0.15,
                     b.mx + 0.85, b.my + 0.85,
                     220, 60, 60, 230)
            viz.line("shield_scan_blockers",
                     b.mx + 0.85, b.my + 0.15,
                     b.mx + 0.15, b.my + 0.85,
                     220, 60, 60, 230)
            viz.text("shield_scan_blockers",
                     b.mx + 0.5, b.my + 0.95,
                     string.format("LGM-unreach from (%d,%d)",
                                   b.origin_mx or -1, b.origin_my or -1),
                     "center", 220, 60, 60, 220, 0.3)
            -- Faint dashed-ish line from origin to dest to make the
            -- attempted route visible.
            if b.origin_mx then
              viz.line("shield_scan_blockers",
                       b.origin_mx + 0.5, b.origin_my + 0.5,
                       b.mx + 0.5, b.my + 0.5,
                       220, 60, 60, 80)
            end
          end
        end
      end
    end

    -- Color-key legend next to the viz target so the user can read the
    -- aim → color mapping. Format "name:actual+potential" so a 0-blocker
    -- spot still tells you how many slots could be filled.
    local lx = viz_target.cx + 0.6
    local ly = viz_target.cy - 0.5
    for ai = 1, #M.AIM_NAMES do
      local col = M.AIM_COLORS[ai]
      local a = viz_target.aims[ai]
      local n_act = (a and a.blockers) and #a.blockers or 0
      local n_pot = (a and a.potential_blockers) and #a.potential_blockers or 0
      viz.text("shield_scan_legend", lx, ly + (ai - 1) * 0.3,
               string.format("%s:%d+%d", M.AIM_NAMES[ai], n_act, n_pot),
               "topleft", col[1], col[2], col[3], 255, 0.35)
    end

    -- Build-queue highlight: thick orange outline + 1-based build order
    -- number on every tile that the LGM will build a wall on, sorted
    -- closest-to-pill first (matches the order build_walls dispatches).
    -- Only the WINNING aim's potential_blockers count — other aims'
    -- borders are informational, no LGM trip there.
    if viz_target.best_aim_idx and scan.pill then
      local pmx = scan.pill.mx
      local pmy = scan.pill.my
      local pots = viz_target.aims[viz_target.best_aim_idx]
                   and viz_target.aims[viz_target.best_aim_idx].potential_blockers
                   or {}
      if #pots > 0 then
        local sorted = {}
        for _, p in ipairs(pots) do sorted[#sorted + 1] = p end
        table.sort(sorted, function(a, b)
          local da = (a.mx - pmx) * (a.mx - pmx) + (a.my - pmy) * (a.my - pmy)
          local db = (b.mx - pmx) * (b.mx - pmx) + (b.my - pmy) * (b.my - pmy)
          return da < db
        end)
        for i, b in ipairs(sorted) do
          -- Two stacked outlines = visibly thick on both low and high
          -- zoom (overlay_rect doesn't take a stroke width).
          viz.rect("wall_build_queue", b.mx - 0.02, b.my - 0.02,
                   b.mx + 1.02, b.my + 1.02,
                   255, 140, 0, 255, false)
          viz.rect("wall_build_queue", b.mx + 0.04, b.my + 0.04,
                   b.mx + 0.96, b.my + 0.96,
                   255, 140, 0, 255, false)
          viz.text("wall_build_queue", b.mx + 0.5, b.my + 0.5,
                   tostring(i),
                   "center", 255, 200, 80, 255, 0.8)
        end
      end
    end
  end
end

return M
