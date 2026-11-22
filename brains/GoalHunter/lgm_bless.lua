-- =========================================================================
-- lgm_bless.lua -- the builder's "blessed square" for the walk-home sim
-- (C.LGM_STRANDED_BLESS_DISPATCH, Andrew, Nov 21: "Yes").
--
-- The engine lets the builder walk off the square of his own job at
-- man_speed_refuel_base, even when that square is solid for him (a pill he
-- just put down or repaired, a wall, a boat). lgm.c lgmNewPrimaryRequest
-- sets blessX/blessY to the job square for:
--   LGM_PILL_REQUEST      (BUILDMODE_PBOX; a pill order on forest with no
--                          pill becomes a tree order: no bless)
--   LGM_BUILDING_REQUEST  (BUILDMODE_BUILD; on forest it becomes a tree
--                          order: no bless; on river it becomes a boat
--                          order: bless)
--   LGM_ROAD_REQUEST      (BUILDMODE_ROAD) only when the square is river
-- and not for tree (BUILDMODE_FARM) or mine (BUILDMODE_MINE) orders.
-- lgmReturn clears the bless as soon as he is off that square.
--
-- The stranded check (goals.lua) and loaded_no_lgm_eval called the walk sim
-- with no blessed square, so a builder standing on the pill he just put
-- down read "cannot move" on the first step: a false "stranded" and a
-- rescue_lgm drive (grass demo try3, Nov 20; 62 of 86 rescue starts in two
-- 90000-tick Oil Rig games).
--
-- A builder on a solid square that is NOT his job square (for example a pill
-- someone else put down on him) gets no bless, as in the engine: he is
-- really stranded.
-- =========================================================================

local C = require("constants")
local U = require("util")
local print2 = require("print2")

local M = {}

-- Does the engine bless the job square for this order? Decided at dispatch
-- time from the terrain then. `has_pill`: a pill (any owner) on the square.
function M.blessed_order(action, tt, has_pill)
  if action == BUILDMODE_PBOX then
    return not (tt == C.T_FOREST and not has_pill)
  elseif action == BUILDMODE_BUILD then
    return tt ~= C.T_FOREST
  elseif action == BUILDMODE_ROAD then
    return tt == C.T_RIVER
  end
  return false   -- BUILDMODE_FARM, BUILDMODE_MINE, anything else
end

-- For the dispatch record in init.lua (state._lgm_dispatch.bless).
function M.for_dispatch(action, mx, my, world)
  local W = require("world")
  local has_pill = world ~= nil and world.pill_at ~= nil and W.pill_at(world, mx, my) ~= nil
  return M.blessed_order(action, U.ttype(mx, my), has_pill)
end

-- The blessed square to pass to cpf_lgm_travel_ticks for the builder now at
-- square (mx, my): his job square while he stands on it and the order is
-- one the engine blesses; else 0, 0 (no bless). With the knob off, 0, 0.
function M.square(state, mx, my)
  if not C.LGM_STRANDED_BLESS_DISPATCH then return 0, 0 end
  local d = state and state._lgm_dispatch
  if d and d.bless and d.x == mx and d.y == my then return mx, my end
  return 0, 0
end

-- The stranded check from goal_selection (goals.lua, Override 0), moved
-- here unchanged apart from the bless square and the re-check, so the unit
-- test can run it. Called every tick while the builder is out walking
-- (man_status == LGM_MOVING). Sets state.lgm_stranded and
-- state._lgm_stranded_factors (read by the rescue gate and the viz).
function M.update_stranded(state, info, lgm_mx, lgm_my, tmx, tmy, lgm_dist, now, in_build_dispatch)
  -- C.LGM_STRANDED_BLESS_DISPATCH: the walk sim gets
  -- the builder's job square as the blessed square while he stands on
  -- it, as the engine does, so a builder on the pill or wall he just
  -- built is not "stranded". 0, 0 with the knob off (the old way).
  local bless_mx, bless_my = M.square(state, lgm_mx, lgm_my)
  -- C.LGM_STRANDED_RECHECK: while the flag is set, run the same walk
  -- at the same 50-tick cadence and clear the flag once he can walk
  -- home again. Goal selection then drops rescue_lgm the normal way.
  -- A walled builder keeps the flag (the walled check below sets it
  -- again on every pass).
  if state.lgm_stranded and C.LGM_STRANDED_RECHECK and not state.lgm_walled then
    if not state.lgm_stranded_check_tick
       or now - state.lgm_stranded_check_tick >= 50 then
      state.lgm_stranded_check_tick = now
      local ticks = cpf_lgm_travel_ticks(
        info.man_x, info.man_y, info.tankx, info.tanky,
        bless_mx, bless_my, 2000, 150)
      if state._lgm_stranded_factors then
        state._lgm_stranded_factors.recheck_ticks = ticks
        state._lgm_stranded_factors.recheck_tick = now
      end
      if ticks ~= -1 then
        state.lgm_stranded = nil
        print2(string.format("LGM_STRANDED_CLEAR t=%d man=(%d,%d) tank=(%d,%d) walk=%d ticks bless=(%d,%d)",
          now, lgm_mx, lgm_my, tmx, tmy, ticks, bless_mx, bless_my))
      end
    end
  elseif not state.lgm_stranded and not in_build_dispatch then
    if not state.lgm_stranded_check_tick
       or now - state.lgm_stranded_check_tick >= 50 then
      state.lgm_stranded_check_tick = now
      local ticks = cpf_lgm_travel_ticks(
        info.man_x, info.man_y, info.tankx, info.tanky,
        bless_mx, bless_my, 2000, 150)
      if ticks == -1 then
        state.lgm_stranded = true
      end

      -- Smart pickup: if LGM is nearby and arriving soon, don't rescue
      state.builder.lgm_nearby = false
      if ticks and ticks > 0 and lgm_dist <= C.LGM_NEARBY_TILES
         and ticks < C.LGM_NEARBY_ARRIVAL_TICKS then
        state.builder.lgm_nearby = true
        state.builder.lgm_arrival_ticks = ticks
        state.lgm_stranded = false
      end
      -- Stash the per-factor result for the V dialog "LGM stranded
      -- dbg" overlay (drawn next to the LGM in init.lua). Read by
      -- the LGM viz code, never affects logic here.
      state._lgm_stranded_factors = {
        path_ticks    = ticks,
        path_fail     = (ticks == -1),
        build_suppress = false,  -- only set true on the suppression branch
        nearby_carve  = state.builder.lgm_nearby or false,
        lgm_dist      = lgm_dist,
        updated_tick  = now,
        bless_mx      = bless_mx,
        bless_my      = bless_my,
      }
    end
  elseif in_build_dispatch then
    -- Suppressed branch: still expose the suppression to the viz so
    -- the user can see WHY the rescue check isn't firing.
    state._lgm_stranded_factors = {
      path_ticks    = nil,
      path_fail     = false,
      build_suppress = true,
      nearby_carve  = false,
      lgm_dist      = lgm_dist,
      updated_tick  = now,
    }
  end
end

return M
