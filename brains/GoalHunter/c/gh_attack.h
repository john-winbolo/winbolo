#ifndef GH_ATTACK_H
#define GH_ATTACK_H

#include <lua.h>

/*
 * gh_attack — C port of evaluate_pill_difficulty (detailed=false hot path).
 *
 * Eliminates per-tile Lua->C overhead for the ~2,800 terrain/grid reads
 * per invocation.  The detailed=true path (plan_position viz) stays in Lua.
 *
 * Lua API (registered as global table "gh_attack"):
 *   gh_attack.init_stamps(los_5deg, ellipse_5deg)
 *       Called once at Brain.open after stamps are built in Lua.
 *
 *   gh_attack.sync_pill_at(world_pill_at, target_pmx, target_pmy)
 *       Rebuild the C pill-occupancy array from world.pill_at.
 *       Call whenever changes.pills is non-empty.
 *
 *   gh_attack.sync_grids(pill_grid, cov_grid)  [also on gh_threat]
 *       Copy Lua pill_grid / coverage_grid tables into C float arrays.
 *       Called after threat.rebuild_pill_grid() completes.
 *
 *   gh_attack.evaluate_pill_difficulty(
 *       pmx, pmy, step_deg, phase_not_opening, tmx, tmy,
 *       self_contrib_table_or_nil)
 *   -> best_score, best_mx, best_my, best_deg
 *      (returns math.huge, -1, -1, -1 when no valid spot found)
 */

void naAttackRegister(lua_State *L);

#endif /* GH_ATTACK_H */
