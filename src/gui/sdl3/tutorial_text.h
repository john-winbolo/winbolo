/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Tutorial Text
 *Filename:      tutorial_text.h
 *Purpose:
 *  Resolves a STR_TUTORIAL* id into a sequence of
 *  renderable segments (TEXT runs, PNG glyphs, or
 *  procedural keycaps).  Two transformations:
 *    1. Three-way variant selection on the string id. When
 *       uiShouldUseControllerMode() is true and the id has a
 *       registered _CTRL sibling, use it (controller takes
 *       precedence — a touchscreen with a pad attached gets
 *       pad wording); else when uiModeIsTablet() is true and
 *       a _TOUCH sibling exists, use that; otherwise the
 *       desktop string.
 *    2. Expand {ACCEL}/{BRAKE}/{LEFT}/{RIGHT}/{FIRE}/{MINE}/
 *       {SCROLL_*}/{DISMISS}/{BUILD_*}/{QUICK_*} tokens into
 *       one glyph segment each (tutorial_tokens.h lists them).
 *       The quick-build tokens show the build tool's button on
 *       a controller and the words "the build buttons" on a
 *       touch screen, which has neither.  In controller mode, discrete buttons
 *       resolve through the binding-aware resolver (Steam
 *       Input origin first, then the live SDL binding) so the
 *       glyph follows the current remap; analog/stick tokens
 *       use the Path-B glyph_* art.  In keyboard mode each
 *       token becomes its scancode PNG (procedural keycap
 *       fallback).
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

/* The same token expansion over a text the caller holds, such as a
 * scenario's popup: no variant selection, since the text has no string id.
 * A TEXT segment may point into src itself, so src must outlive the
 * segments.  Same buffer rules as tutorialResolveSegments(). */
int tutorialResolveText(const char *src, TutorialSeg *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* TUTORIAL_TEXT_H */
