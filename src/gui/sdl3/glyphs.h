/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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

#ifdef __cplusplus
}
#endif

#endif /* WB_GLYPHS_H */
