/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Tutorial Text
 *Filename:      tutorial_text.h
 *Purpose:
 *  Resolves a STR_TUTORIAL* id into a renderable string.
 *  Two transformations:
 *    1. If uiModeIsTablet() is true and the id has a
 *       registered _TOUCH sibling, use the sibling.
 *    2. Expand {ACCEL}/{BRAKE}/{LEFT}/{RIGHT}/{FIRE}/
 *       {MINE}/{SCROLL_*} tokens against the live key
 *       bindings via SDL_GetScancodeName.
 *********************************************************/

#ifndef TUTORIAL_TEXT_H
#define TUTORIAL_TEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a renderable C string for the given STR_TUTORIAL* id.
 * Buffer is static and overwritten on each call; the dialog code
 * uses the result synchronously inside frontEndTutorial, so the
 * caller does not need to copy it. Main thread only. */
const char *tutorialResolveText(uint16_t mid);

#ifdef __cplusplus
}
#endif

#endif /* TUTORIAL_TEXT_H */
