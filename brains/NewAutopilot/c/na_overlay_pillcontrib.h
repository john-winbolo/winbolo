#ifndef NA_OVERLAY_PILLCONTRIB_H
#define NA_OVERLAY_PILLCONTRIB_H

/*********************************************************
 * NewAutopilot per-pill danger contribution overlay
 *
 * BrainTest-only: each bot pushes per-pill, per-tile danger
 * contribution data each tick; the host renders an overlay
 * (shift-2 cycles through pills of the currently-followed
 * bot) from the pushed data.
 *
 * Lua bindings (each bot calls these for ITS OWN data only —
 * the host disambiguates by including the bot's player
 * number in every call):
 *   pillcontrib_begin_pill(pill_id, mx, my) -> slot
 *     where slot encodes (bot, within-bot-slot) opaquely;
 *     pass it back into pillcontrib_add_tile / _add_all.
 *   pillcontrib_add_tile(slot, tx, ty, value)
 *   pillcontrib_add_all(slot, contrib_table)
 *
 * The bot's player_num is wired in at brain-instance create
 * time via a Lua upvalue, so the brain doesn't have to pass
 * it on every call.
 *
 * Clear is host-driven (once per tick before the bot loop)
 * — there is no Lua-side clear binding because each bot
 * appending into its own slot range has no inter-bot race.
 *
 * Non-host runtimes (game client, headless server) leave
 * callbacks NULL — the bindings are registered but no-op.
 *********************************************************/

#include <lua.h>

typedef int  (*NaPillContribBeginPillFunc)(int bot, int pill_id, int mx, int my);
typedef void (*NaPillContribAddTileFunc)(int slot, int tx, int ty, float value);

void naPillContribSetBeginPillCallback(NaPillContribBeginPillFunc cb);
void naPillContribSetAddTileCallback(NaPillContribAddTileFunc cb);

/* Register the Lua bindings as globals on this brain's Lua
 * state. player_num is captured as a closure upvalue so the
 * bindings know which bot's slot range to write into. */
void naPillContribRegister(lua_State *L, int player_num);

#endif /* NA_OVERLAY_PILLCONTRIB_H */
