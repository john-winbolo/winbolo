/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * glyphs.h — Path A glyph lookup for controller button icons.
 *
 * Wraps steam_input_get_glyph_path() with an SDL_Texture cache so the
 * UI can render controller-correct icons (Deck, Xbox, PS, Switch,
 * generic) inline next to action labels.  When Steam Input isn't
 * active or no controller is connected, every lookup returns NULL —
 * callers must handle the no-glyph case and fall back to text only.
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

/* Returns an SDL_Texture for the action's currently-bound origin
 * glyph, or NULL if Path A is unavailable / no controller / unknown
 * action.  Texture is owned by the module — do not destroy.  Same
 * texture pointer returned across calls until the binding changes
 * (V1: never; restart required to pick up rebinds). */
SDL_Texture *glyphForAction(const char *action_name);

#ifdef __cplusplus
}
#endif

#endif /* WB_GLYPHS_H */
