/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Tutorial Text
 *Filename:      tutorial_text.h
 *Purpose:
 *  Resolves a scenario's popup text into a sequence of
 *  renderable segments (TEXT runs, PNG glyphs, or
 *  procedural keycaps).  Two transformations:
 *    1. A text that is a whole "{LANG:NAME}" reference
 *       becomes that string of the language file.
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
     overwritten on the next tutorialResolveText() call. */
  const char  *text;
  /* For GLYPH_PNG: the loaded texture, owned by glyphs.c.
     For TEXT and KEYCAP: NULL. */
  SDL_Texture *glyph;
  /* For GLYPH_KEYCAP: the label rendered inside the procedural
     cap.  Same lifetime as `text`.  For other kinds: NULL. */
  const char  *keycapLabel;
} TutorialSeg;

#define TUTORIAL_SEG_MAX 64

/* What the player plays with, as the tutorial words it. */
typedef enum {
  TUTORIAL_INPUT_KEYBOARD = 0,   /* keyboard and mouse */
  TUTORIAL_INPUT_TOUCH,          /* the on-screen touch layout */
  TUTORIAL_INPUT_CONTROLLER      /* a gamepad */
} TutorialInput;

/* The input the player uses now. On a phone or tablet the touch layout
 * comes first, as its controls are on the screen whatever SDL counts as a
 * gamepad; elsewhere a controller comes first, then a touch screen. The
 * key tokens and what a single-player server is told for game.input both
 * follow it. Main thread only. */
TutorialInput tutorialInputNow(void);

/* Fills `out` with up to `max` segments of a text the caller holds, such
 * as a scenario's popup, and returns the segment count.  The renderer must
 * consume the segments synchronously before the next call (the per-call
 * buffer holding TEXT/keycap strings is static).  A TEXT segment may point
 * into src itself, so src must outlive the segments.  Main thread only. */
int tutorialResolveText(const char *src, TutorialSeg *out, int max);

/* The same tokens written as words, for a line drawn as plain text (a
 * scenario's status line and announcement): the bound key's name on a
 * keyboard, the touch wording for a quick-build or gunsight key on a touch
 * screen, and a word for the control on a controller. out always ends with a NUL. */
void tutorialExpandTextPlain(const char *src, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* TUTORIAL_TEXT_H */
