/*********************************************************
 * Shot-Sim POI registry — brain-registered "active points
 * of interest" the user can click on inside the shot-sim
 * panel to fill an endpoint.
 *
 * Brains register POIs from Lua via:
 *   braintest_shotsim_poi_register(name, lua_expr [, opts])
 *
 *   - name     : human-readable label shown on the button
 *   - lua_expr : Lua chunk that returns either:
 *                  - two integers wx, wy   → POI is available
 *                  - nil                    → POI not currently
 *                                             available (button
 *                                             greys out)
 *   - opts     : optional table; reserved for future use
 *
 * The panel polls each POI's lua_expr at ~10Hz against the
 * followed bot's Lua state (same pattern as the panel-text
 * registry).
 *********************************************************/

#ifndef BRAINTEST_SHOTSIM_POI_REGISTRY_H
#define BRAINTEST_SHOTSIM_POI_REGISTRY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOTSIM_POI_REG_MAX        32
#define SHOTSIM_POI_REG_NAME_MAX   64
#define SHOTSIM_POI_REG_EXPR_MAX   512

typedef struct {
    char name[SHOTSIM_POI_REG_NAME_MAX];
    char lua_expr[SHOTSIM_POI_REG_EXPR_MAX];
    int  bot_owner;          /* playerNum that registered this POI */
} ShotSimPoiEntry;

/* Register a new POI. Returns the slot index on success, -1 on
 * full registry / dup name (same name + same bot_owner is
 * idempotent — overwrites the lua_expr in place). */
int  shotSimPoiRegister(int bot_owner, const char *name,
                         const char *lua_expr);

/* Active count + by-index lookup. Iteration is in registration order. */
int  shotSimPoiCount(void);
const ShotSimPoiEntry *shotSimPoiGet(int idx);

/* Clear all entries owned by a given bot. Called when the bot's
 * Lua state is rebuilt so we don't accumulate stale entries. */
void shotSimPoiClearBot(int bot_owner);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_SHOTSIM_POI_REGISTRY_H */
