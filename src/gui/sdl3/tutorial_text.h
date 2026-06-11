/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Tutorial Text
 *Filename:      tutorial_text.h
 *Purpose:
 *  Resolves a STR_TUTORIAL* id into a sequence of
 *  renderable segments (TEXT runs, PNG glyphs, or
 *  procedural keycaps).  Two transformations:
 *    1. If uiModeIsTablet() is true and the id has a
 *       registered _TOUCH sibling, use the sibling.
 *    2. Expand {ACCEL}/{BRAKE}/{LEFT}/{RIGHT}/{FIRE}/
 *       {MINE}/{SCROLL_*} tokens into one glyph segment
 *       each.  The chosen glyph follows the most recently
 *       used input source: keyboard scancode PNG (with
 *       procedural keycap fallback) or the active gamepad
 *       set's button glyph.
 *********************************************************/

#ifndef TUTORIAL_TEXT_H
#define TUTORIAL_TEXT_H

#include <SDL3/SDL.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TUTORIAL_SEG_TEXT       = 0,
  TUTORIAL_SEG_GLYPH_PNG  = 1,
  TUTORIAL_SEG_GLYPH_KEYCAP = 2
} TutorialSegKind;

typedef struct {
  TutorialSegKind kind;
  /* For TEXT: a NUL-terminated run from the tutorial buffer.
     For KEYCAP: the scancode name (or token text on miss).
     For GLYPH_PNG: NULL.  Pointer is stable for the lifetime of
     the dialog — the producer parks runs in a static buffer
     overwritten on the next tutorialResolveSegments() call. */
  const char  *text;
  /* For GLYPH_PNG: the loaded texture, owned by glyphs.c.
     For TEXT and KEYCAP: NULL. */
  SDL_Texture *glyph;
  /* For GLYPH_KEYCAP: the label rendered inside the procedural
     cap.  Same lifetime as `text`.  For other kinds: NULL. */
  const char  *keycapLabel;
} TutorialSeg;

#define TUTORIAL_SEG_MAX 64

/* Fills `out` with up to `max` segments and returns the segment
 * count.  The renderer must consume the segments synchronously
 * before the next tutorialResolveSegments() call (the per-call
 * buffer holding TEXT/keycap strings is static).  Main thread only.
 */
int tutorialResolveSegments(uint16_t mid, TutorialSeg *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* TUTORIAL_TEXT_H */
