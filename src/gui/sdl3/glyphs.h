/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * glyphs.h — Path A (Steam Input) and Path B (Xelu atlas) glyph
 * lookups for controller and keyboard button icons.
 *
 * Path A wraps steam_input_get_glyph_path() with an SDL_Texture
 * cache.  Used by the HUD pause overlay etc. so glyphs follow the
 * Steam Input binding exactly.
 *
 * Path B loads PNG textures from data/controller/{xbox,ps5,switch,
 * keyboardmouse}/ on demand and serves them via stable pseudo-action
 * names (glyph_*) and SDL_Scancode lookups.  Used by the tutorial
 * dialogs which need analog directional sub-glyphs that Steam Input
 * doesn't expose.  Both paths coexist; nothing in this header
 * affects callers of glyphForAction().
 */
#ifndef WB_GLYPHS_H
#define WB_GLYPHS_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#include "input_gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

bool glyphsInit(SDL_Renderer *renderer);
void glyphsShutdown(void);

/* --- Path A --- */

/* Returns an SDL_Texture for the action's currently-bound origin
 * glyph, or NULL if Path A is unavailable / no controller / unknown
 * action.  Texture is owned by the module — do not destroy.  Same
 * texture pointer returned across calls until the binding changes
 * (V1: never; restart required to pick up rebinds). */
SDL_Texture *glyphForAction(const char *action_name);

/* Tries Path A (Steam Input) first; on miss, looks up the Xelu
 * atlas via Path B by prefixing "glyph_" to action_name.  Returns
 * NULL if neither path resolves; callers handle NULL by falling
 * back to text-only labels. */
SDL_Texture *glyphForActionAuto(const char *action_name);

/* Glyph for a logical action, correct on both backends.
 *   siAction  : Steam Input action name (NULL -> skip Path A)
 *   sdlAction : SDL action for the live-binding fallback (GP_ACT_COUNT -> skip)
 * Returns NULL if neither resolves (caller draws its own text fallback). */
SDL_Texture *glyphForControllerAction(const char *siAction, GamepadAction sdlAction);

/* --- Path B --- */

typedef enum {
  GAMEPAD_GLYPH_SET_XBOX   = 0,
  GAMEPAD_GLYPH_SET_PS5    = 1,
  GAMEPAD_GLYPH_SET_SWITCH = 2
} GamepadGlyphSet;

/* Maps an SDL_GamepadType to one of the three controller glyph
 * sets.  PS3/PS4/PS5 -> ps5; any Switch variant -> switch;
 * everything else (Xbox, Standard, Unknown, GameCube, Steam Deck)
 * -> xbox.  `active` may be NULL — falls back to the cached choice
 * established by the most recent SDL_EVENT_GAMEPAD_ADDED. */
GamepadGlyphSet selectGamepadGlyphSet(SDL_Gamepad *active);

/* Texture for a keyboard scancode from data/controller/keyboardmouse.
 * Returns NULL if the scancode has no mapped PNG; the caller draws
 * a procedural keycap fallback in that case.  Texture is owned by
 * the module. */
SDL_Texture *glyphForKeyboardScancode(SDL_Scancode sc);

/* Texture for a tutorial pseudo-action ("glyph_fire", etc.) resolved
 * against the currently-active gamepad glyph set.  Returns NULL if
 * the pseudo-action is not in the table.  Texture is owned by the
 * module. */
SDL_Texture *glyphForGamepadAction(const char *glyphAction);

/* Texture for a specific SDL_GamepadButton resolved against the
 * currently-active gamepad glyph set.  Returns NULL if the button
 * has no entry in the per-set table; callers fall back to a
 * procedural keycap with SDL_GetGamepadStringForButton(). */
SDL_Texture *glyphForGamepadButton(SDL_GamepadButton button);

/* Texture for a trigger axis (LEFT_TRIGGER / RIGHT_TRIGGER) resolved
 * against the currently-active gamepad glyph set.  Returns NULL for
 * non-trigger axes or unmapped sets. */
SDL_Texture *glyphForGamepadAxis(SDL_GamepadAxis axis);

#ifdef __cplusplus
}
#endif

#endif /* WB_GLYPHS_H */
