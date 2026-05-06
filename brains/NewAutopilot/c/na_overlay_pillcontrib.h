#ifndef NA_OVERLAY_PILLCONTRIB_H
#define NA_OVERLAY_PILLCONTRIB_H

/*********************************************************
 * NewAutopilot per-pill danger contribution overlay
 *
 * BrainTest-only: brains push per-pill, per-tile danger
 * contribution data each tick; the host renders an overlay
 * (shift-2 cycles through pills) from the pushed data.
 *
 * Lua bindings:
 *   pillcontrib_clear()
 *   pillcontrib_begin_pill(pill_id, mx, my) -> slot
 *   pillcontrib_add_tile(slot, tx, ty, value)
 *
 * Non-host runtimes (game client, headless server) leave
 * callbacks NULL — the bindings are registered but no-op.
 *
 * Moved from src/bolo/braincore.c so the engine's brain
 * runtime stays generic; this file holds bindings that are
 * specific to the NewAutopilot threat-model overlay.
 *********************************************************/

#include <lua.h>

typedef void (*NaPillContribClearFunc)(void);
typedef int  (*NaPillContribBeginPillFunc)(int pill_id, int mx, int my);
typedef void (*NaPillContribAddTileFunc)(int slot, int tx, int ty, float value);

void naPillContribSetClearCallback(NaPillContribClearFunc cb);
void naPillContribSetBeginPillCallback(NaPillContribBeginPillFunc cb);
void naPillContribSetAddTileCallback(NaPillContribAddTileFunc cb);

/* Register the three Lua bindings as globals. Call once per
 * lua_State during brain instance creation, after the generic
 * brainCore* registrations. */
void naPillContribRegister(lua_State *L);

#endif /* NA_OVERLAY_PILLCONTRIB_H */
