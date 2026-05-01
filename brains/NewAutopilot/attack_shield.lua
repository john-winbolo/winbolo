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
-- AIM_INSET_FIRE     : extra inset (on top of AIM_INSET) applied when
--                      we actually take the shot. Aiming a bit deeper
--                      than the scoring point gives the real shell
--                      another safety margin against drift.
M.AIM_INSET      = 24   -- 1.5 gu (16 wu/gu) — scoring
M.AIM_INSET_FIRE = 16   -- 1 gu — actual fire aim sits 1 gu inside the corner
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

-- Score one (candidate_pos, aim_i) pair.
-- Returns: outgoing_tiles, outgoing_blocked_by_wall,
--          per_aim_blockers (filtered list), per_aim_score
local function score_aim(spot_wx, spot_wy, origin_mx, origin_my,
                          pmx, pmy, pill_wx, pill_wy,
                          aim_offset, return_tiles_set, return_tiles_list,
                          world, no_builder)
  local target_wx = (pmx << 8) + aim_offset[1]
  local target_wy = (pmy << 8) + aim_offset[2]
  local out_tiles = cpf.simulate_shot(spot_wx, spot_wy,
                                      target_wx, target_wy,
                                      cpf.SHOT_TANK, 0)

  -- Outgoing path: walk up to the pill, reject the aim if it crosses
  -- a wall OR any pill (friendly or enemy) other than the target. The
  -- pill-rejection is the important one for shield aiming — without
  -- it, an aim that physically slams the shell into a friendly blocker
  -- pill gets accepted because the score filter ALSO doesn't count
  -- tiles that are on the outgoing path. We'd then prefer that aim
  -- thinking it's "clean", and at runtime the real shell hits the
  -- blocker pill instead of the target.
  local outgoing_blocked_by_wall = false
  local outgoing_set = {}
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
        -- Any non-target pill in the path = the shell hits it. Reject.
        local pk = world.pill_at and world.pill_at[t.my * 256 + t.mx]
        if pk and world.pills and world.pills[pk] then
          outgoing_blocked_by_wall = true
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
  -- not too close to the tank, not the pill or tank tile, and NOT past
  -- the tank from the pill's perspective. The simulated return path
  -- continues until the shell expires; any tile further from the pill
  -- than the tank is "behind" us and a wall built there would block
  -- nothing real.
  local pill_cx, pill_cy = pmx + 0.5, pmy + 0.5
  local tank_cx, tank_cy = origin_mx + 0.5, origin_my + 0.5
  local pdtx, pdty = tank_cx - pill_cx, tank_cy - pill_cy
  local pill_to_tank_d2 = pdtx * pdtx + pdty * pdty
  local actual_blockers, potential_blockers = {}, {}
  if not outgoing_blocked_by_wall and return_tiles_list then
    for ti = 1, #return_tiles_list do
      local t = return_tiles_list[ti]
      local idx = t.my * 256 + t.mx
      local skip = (t.mx == pmx and t.my == pmy) or
                   (t.mx == origin_mx and t.my == origin_my)
      if not skip and not outgoing_set[idx] and U.in_map(t.mx, t.my) then
        local ddx = t.mx + 0.5 - tank_cx
        local ddy = t.my + 0.5 - tank_cy
        local d = math.sqrt(ddx * ddx + ddy * ddy)
        -- Past-the-tank reject: tile's distance from the PILL exceeds
        -- the pill→tank distance, so the pill's shell would have hit
        -- the tank before reaching this tile. A wall here protects
        -- nothing.
        local pdx = t.mx + 0.5 - pill_cx
        local pdy = t.my + 0.5 - pill_cy
        local past_tank = (pdx * pdx + pdy * pdy) > pill_to_tank_d2
        if d >= M.BLOCKER_MIN_DIST and not past_tank then
          local kind = nil
          local pk = world.pill_at and world.pill_at[idx]
          if pk and world.pills and world.pills[pk] then
            if world.pills[pk].owner == "friendly" then
              kind = "friendly_pill"
            end
          end
          if not kind then
            local tt = U.ttype(t.mx, t.my)
            if tt == C.T_BUILDING then kind = "wall_full"
            elseif tt == C.T_HALFBUILD then kind = "wall_half"
            end
          end
          if kind then
            actual_blockers[#actual_blockers + 1] =
              { mx = t.mx, my = t.my, kind = kind }
          else
            -- Empty terrain that COULD be a wall. Only consider tiles
            -- that are buildable land (no water, no pill, no base, no
            -- existing wall). Note: bases use T_REFBASE in constants
            -- (was previously written as the non-existent C.T_BASE,
            -- which silently passed every tile and let the LGM try
            -- to build on a friendly refuel base).
            local tt = U.ttype(t.mx, t.my)
            local buildable =
              tt ~= C.T_DEEPSEA and tt ~= C.T_RIVER and
              tt ~= C.T_SWAMP   and tt ~= C.T_PILLBOX and
              tt ~= C.T_REFBASE and tt ~= C.T_BOAT
            -- No LGM available to build (dead/parachuting): potentials
            -- are imaginary cover, drop them so the score reflects only
            -- what's already there. attack.lua will demote to no-shield
            -- if the resulting score is 0 across all candidates.
            if no_builder then buildable = false end
            if buildable then
              potential_blockers[#potential_blockers + 1] =
                { mx = t.mx, my = t.my, kind = "empty" }
            end
          end
        end
      end
    end
  end
  return out_tiles, outgoing_blocked_by_wall, actual_blockers, potential_blockers
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

  -- Compute return fire ONCE — the pill shoots toward the tank's
  -- precise position regardless of which aim the tank uses.
  local return_tiles = cpf.simulate_shot(pill_wx, pill_wy,
                                         spot_wx, spot_wy,
                                         cpf.SHOT_PILL, 0)
  cand.return_fire = { tiles = return_tiles }

  local best_aim_idx = nil
  local best_score = 0
  for ai = 1, #AIM_OFFSETS do
    local out_tiles, blocked, actual_blockers, potential_blockers = score_aim(
      spot_wx, spot_wy, mx, my,
      pmx, pmy, pill_wx, pill_wy,
      AIM_OFFSETS[ai], nil, return_tiles, world, no_builder)
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
                standoff_cx, standoff_cy, radius, no_builder)
  if not pill or not world then return { candidates = {}, best = nil } end
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
  local candidates = {}
  candidates[1] = make_candidate(sx, sy, standoff_deg, 0, "standoff")
  -- Force the standoff's tile to the integer one we were handed (in case
  -- the float center rounds differently).
  candidates[1].mx = standoff_mx
  candidates[1].my = standoff_my

  local half = M.NUM_CANDIDATES * 0.5
  for i = 1, M.NUM_CANDIDATES do
    local offset = (i - half - 0.5) * M.STEP_DEG
    local deg = standoff_deg + offset
    local rad = math.rad(deg)
    local cx = pcx + math.sin(rad) * R
    local cy = pcy - math.cos(rad) * R
    candidates[#candidates + 1] = make_candidate(cx, cy, deg, offset, "candidate")
  end

  for _, c in ipairs(candidates) do
    score_candidate(c, pill, world, pill_wx, pill_wy, no_builder)
  end

  -- Neighbor bonus: a candidate's BLOCKER SET (every tile on its
  -- chosen aim that contributes to its score — actual walls,
  -- friendly pills, AND potential build slots) must be a subset of
  -- a neighbor's blocker set. Potentials count because they're tiles
  -- the LGM is going to wall in — drifting to a neighbor that
  -- doesn't share that potential slot loses the planned cover too.
  -- Standoff is excluded both as a center and as a neighbor — only
  -- ring-to-ring relationships count.
  -- Per-(candidate, aim) blocker_keys cache. Lazily filled on first
  -- access. With per-subset chain evaluation the same neighbor's
  -- key set is requested up to (2^MAX_SUBSET_BLOCKERS - 1) times
  -- per candidate per aim, so memoizing this small table pays for
  -- itself many times over. Indexed [cand_idx][aim_idx] = set;
  -- nil = not yet built.
  local bk_cache = {}
  local function blocker_keys_for(i, ai)
    local row = bk_cache[i]
    if row and row[ai] then return row[ai] end
    if not row then row = {}; bk_cache[i] = row end
    local set = {}
    local c = candidates[i]
    local a = c and c.aims and c.aims[ai]
    if a then
      if a.blockers then
        for _, b in ipairs(a.blockers) do
          set[b.my * 256 + b.mx] = true
        end
      end
      if a.potential_blockers then
        for _, b in ipairs(a.potential_blockers) do
          set[b.my * 256 + b.mx] = true
        end
      end
    end
    row[ai] = set
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
              local function covers(j)
                local nc = candidates[j]
                if not nc or nc.kind ~= "candidate" then return false end
                local n_a = nc.aims and nc.aims[ai]
                if not n_a or n_a.blocked then return false end
                return subset(subset_set, blocker_keys_for(j, ai))
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

-- Draw helpers --------------------------------------------------------------

-- Per-tile thin colored border via two stacked rects (slight inset for the
-- inner one). Useful for layering multiple aim borders on the same tile.
local function border_box(viz_id, mx, my, inset, r, g, b, a)
  viz.rect(viz_id, mx + inset, my + inset, mx + 1 - inset, my + 1 - inset,
           r, g, b, a, false)
end

function M.draw_overlay(scan, now_tick)
  if not scan or not scan.candidates then return end

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
  if viz.detail_circle then
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
          viz.detail_text(did, string.format("  blockers: actual=%d potential=%d",
            #(a.blockers or {}), #(a.potential_blockers or {})))
          for _, b in ipairs(a.blockers or {}) do
            viz.detail_text(did, string.format("    actual    @ tile (%d,%d)", b.mx, b.my))
          end
          for _, b in ipairs(a.potential_blockers or {}) do
            viz.detail_text(did, string.format("    potential @ tile (%d,%d)", b.mx, b.my))
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
          viz.detail_text(did, string.format(
            "  aim_idx=%d (%s) a=%d p=%d aim_score=%d chain=%d total=%d%s%s",
            e.aim, aim_name, e.actual_n, e.potential_n,
            e.aim_score, e.chain, e.total, bias_str, marker))
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
  for ci, c in ipairs(scan.candidates) do
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
