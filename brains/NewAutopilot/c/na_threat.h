#ifndef NA_THREAT_H
#define NA_THREAT_H

/*********************************************************
 * NewAutopilot threat-grid C kernel
 *
 * Hot-path replacement for threat.lua's:
 *   - rebuild_terrain_factors()       (full 65k-tile sweep)
 *   - compute_terrain_factor_at()     (incremental update)
 *   - stamp_pill()                    (per-pill disk stamp)
 *
 * The C side owns a flat float[65536] terrain_mult cache and a
 * uint8_t[65536] terrain_in_trees cache. Lua threat.lua calls
 * naThreatTerrainRebuild on first M.update and naThreatTerrainUpdateAround
 * for each entry in changes.terrain.
 *
 * naThreatStampPill walks the precomputed PILL_RANGE_MAP-radius disk,
 * applies proximity falloff + tree cover + terrain multiplier, and
 * writes into the Lua pill_grid table that's passed in. It also
 * builds the pill's per-tile contribution table and returns it on
 * the Lua stack so the caller can store it in M.pill_contrib[pkey].
 *
 * Tunable parameters (proximity falloff, tree-cover discounts, terrain
 * speeds, hazard set, low-HP discounts) are configured from Lua via
 * naThreatConfigure so cloners can tweak without recompiling.
 *********************************************************/

#include <lua.h>

/* Single entry point: registers all na_threat.* Lua bindings. Call
 * once per lua_State during brain instance creation. */
void naThreatRegister(lua_State *L);

/* C-side accessors for the pill_grid and coverage_grid mirrors.
 * Used by na_attack.c to read grid data with no Lua API overhead.
 * Take lua_State *L so they can locate the per-state ctx.
 * Return NULL before na_threat.configure() has run on the state. */
float *naThreatGetPillGrid(lua_State *L);
float *naThreatGetCovGrid (lua_State *L);

/* Raw terrain type for tile (mx, my).  Returns 0 if world not loaded.
 * Used by na_shield_stamp.c for in-C blocker classification. Reads
 * a process-wide cached host worldPtr; safe without a lua_State. */
int naThreatRawTT(int mx, int my);

#endif /* NA_THREAT_H */
