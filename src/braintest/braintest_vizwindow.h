/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * BrainTest V dialog — toggleable list of viz overlays.
 *
 * Reads its row data straight from braintest_viz_registry,
 * so any brain that registered ids via braintest_viz_register
 * shows up here automatically. Callback fires when a row's
 * checkbox flips so the host can persist + re-push the state.
 *********************************************************/

#ifndef BRAINTEST_VIZWINDOW_H
#define BRAINTEST_VIZWINDOW_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void vizWindowInit(SDL_Window *window, SDL_Renderer *renderer);
void vizWindowShutdown(void);
void vizWindowProcessEvent(SDL_Event *ev);

/* Render one frame. `onToggle` fires for each row the user
 * just clicked (its `is_on` field has already been flipped
 * before the callback runs). */
void vizWindowRender(SDL_Renderer *renderer, int winW, int winH,
                     void (*onToggle)(int idx));

/* True while the V window's filter input has keyboard focus.
 * Main loop uses this to suppress global hotkeys (V/L/T/...). */
bool vizWindowWantsTextInput(void);

/* Replay-collection mode radio (0=on/followed, 1=all/viewed, 2=all/all-tanks).
 * pushVizStateToBots reads this to set each bot's _BT_VIZ_COLLECT override. */
int vizWindowCollectMode(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_VIZWINDOW_H */
