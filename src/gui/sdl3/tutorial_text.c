/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Tutorial Text
 *Filename:      tutorial_text.c
 *Purpose:       See tutorial_text.h.
 *********************************************************/

#include "tutorial_text.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <string.h>

#include "../input.h"
#include "../lang.h"
#include "../ui_mode.h"
#include "../../common/wb_log.h"
#include "glyphs.h"

/* Global key-binding struct; defined in winbolo.c (and the
 * platform main_*.c files). */
extern keyItems keys;

/* Per-call render buffer.  TEXT and keycap-label runs are copied
 * here so the segment producer can return stable pointers without
 * allocating.  The dialog consumes the result synchronously inside
 * frontEndTutorial; subsequent calls overwrite the buffer. */
static char tutorialBuf[2048];

typedef int (*KeyAccessor)(void);

static int kt_accel(void)        { return keys.kiForward; }
static int kt_brake(void)        { return keys.kiBackward; }
static int kt_left(void)         { return keys.kiLeft; }
static int kt_right(void)        { return keys.kiRight; }
static int kt_fire(void)         { return keys.kiShoot; }
static int kt_mine(void)         { return keys.kiLayMine; }
static int kt_scroll_up(void)    { return keys.kiScrollUp; }
static int kt_scroll_down(void)  { return keys.kiScrollDown; }
static int kt_scroll_left(void)  { return keys.kiScrollLeft; }
static int kt_scroll_right(void) { return keys.kiScrollRight; }
/* Tutorial dismissal isn't user-rebindable — always Return. */
static int kt_dismiss(void)      { return SDL_SCANCODE_RETURN; }

typedef struct {
  const char  *token;
  KeyAccessor  get;
  const char  *gpAction;   /* glyph_* pseudo-action for gamepad */
} TokenEntry;

static const TokenEntry kTokens[] = {
  { "{ACCEL}",        kt_accel,        "glyph_tank_forward"  },
  { "{BRAKE}",        kt_brake,        "glyph_tank_back"     },
  { "{LEFT}",         kt_left,         "glyph_tank_left"     },
  { "{RIGHT}",        kt_right,        "glyph_tank_right"    },
  { "{FIRE}",         kt_fire,         "glyph_fire"          },
  { "{MINE}",         kt_mine,         "glyph_mine"          },
  { "{SCROLL_UP}",    kt_scroll_up,    "glyph_scroll_up"     },
  { "{SCROLL_DOWN}",  kt_scroll_down,  "glyph_scroll_down"   },
  { "{SCROLL_LEFT}",  kt_scroll_left,  "glyph_scroll_left"   },
  { "{SCROLL_RIGHT}", kt_scroll_right, "glyph_scroll_right"  },
  { "{DISMISS}",      kt_dismiss,      "glyph_dismiss"       },
};
static const int kTokenCount = (int)(sizeof(kTokens) / sizeof(kTokens[0]));

/* Desktop -> touch sibling lookup. Strings whose desktop wording
 * references hardware controls (keys, mouse, keypad) get a touch
 * sibling; the rest fall through unchanged. */
typedef struct {
  uint16_t desktop;
  uint16_t touch;
} TouchSibling;

static const TouchSibling kTouchSiblings[] = {
  { STR_TUTORIAL01,       STR_TUTORIAL01_TOUCH       },
  { STR_TUTORIAL02,       STR_TUTORIAL02_TOUCH       },
  { STR_TUTORIAL03,       STR_TUTORIAL03_TOUCH       },
  { STR_TUTORIAL04,       STR_TUTORIAL04_TOUCH       },
  { STR_TUTORIAL05,       STR_TUTORIAL05_TOUCH       },
  { STR_TUTORIAL06,       STR_TUTORIAL06_TOUCH       },
  { STR_TUTORIAL10,       STR_TUTORIAL10_TOUCH       },
  { STR_TUTORIAL14,       STR_TUTORIAL14_TOUCH       },
  { STR_TUTORIAL16,       STR_TUTORIAL16_TOUCH       },
  { STR_TUTORIAL18,       STR_TUTORIAL18_TOUCH       },
  { STR_TUTORIAL19,       STR_TUTORIAL19_TOUCH       },
  { STR_TUTORIAL21,       STR_TUTORIAL21_TOUCH       },
  { STR_TUTORIAL_START01, STR_TUTORIAL_START01_TOUCH },
  { STR_TUTORIAL_START04, STR_TUTORIAL_START04_TOUCH },
};
static const int kTouchSiblingCount =
    (int)(sizeof(kTouchSiblings) / sizeof(kTouchSiblings[0]));

static uint16_t pickStringId(uint16_t mid) {
  int i;
  if (!uiModeIsTablet()) return mid;
  for (i = 0; i < kTouchSiblingCount; i++) {
    if (kTouchSiblings[i].desktop == mid) return kTouchSiblings[i].touch;
  }
  return mid;
}

static const char *scancodeName(int scancode) {
  const char *name;
  if (scancode <= 0) return "?";
  name = SDL_GetScancodeName((SDL_Scancode)scancode);
  if (!name || !*name) return "?";
  return name;
}

/* Append `s` (NUL-terminated) into tutorialBuf at *pos, NUL-terminating
 * the run, advancing *pos past the terminator, and returning the
 * pointer to the written run.  Returns NULL on overflow.  Each call
 * starts a fresh run — successive calls produce distinct stable
 * pointers within the same buffer. */
static const char *bufAppend(int *pos, const char *s) {
  int avail = (int)sizeof(tutorialBuf) - *pos - 1;
  int len   = (int)strlen(s);
  if (len > avail) return NULL;
  char *dst = tutorialBuf + *pos;
  memcpy(dst, s, (size_t)len);
  dst[len] = '\0';
  *pos += len + 1;
  return dst;
}

static int emitText(TutorialSeg *out, int idx, int max,
                    int *bufPos, const char *start, int len) {
  if (idx >= max || len <= 0) return idx;
  int avail = (int)sizeof(tutorialBuf) - *bufPos - 1;
  if (len > avail) return idx;
  char *dst = tutorialBuf + *bufPos;
  memcpy(dst, start, (size_t)len);
  dst[len] = '\0';
  *bufPos += len + 1;
  out[idx].kind        = TUTORIAL_SEG_TEXT;
  out[idx].text        = dst;
  out[idx].glyph       = NULL;
  out[idx].keycapLabel = NULL;
  return idx + 1;
}

static int emitGlyphForToken(TutorialSeg *out, int idx, int max,
                             int *bufPos, const TokenEntry *t) {
  if (idx >= max) return idx;
  if (uiShouldUseControllerMode()) {
    SDL_Texture *g = glyphForGamepadAction(t->gpAction);
    if (g) {
      out[idx].kind        = TUTORIAL_SEG_GLYPH_PNG;
      out[idx].text        = NULL;
      out[idx].glyph       = g;
      out[idx].keycapLabel = NULL;
      return idx + 1;
    }
    /* Should not happen — table is exhaustive.  Log once and fall
       through to a token-text keycap so the dialog still renders. */
    static bool warned = false;
    if (!warned) {
      WB_LOG_WARN(WB_LOG_CAT_GUI, "tutorial: no gamepad glyph for '%s'", t->gpAction);
      warned = true;
    }
    const char *label = bufAppend(bufPos, t->token);
    if (!label) return idx;
    out[idx].kind        = TUTORIAL_SEG_GLYPH_KEYCAP;
    out[idx].text        = NULL;
    out[idx].glyph       = NULL;
    out[idx].keycapLabel = label;
    return idx + 1;
  }
  /* Keyboard path */
  int sc = t->get();
  SDL_Texture *g = (sc > 0) ? glyphForKeyboardScancode((SDL_Scancode)sc) : NULL;
  if (g) {
    out[idx].kind        = TUTORIAL_SEG_GLYPH_PNG;
    out[idx].text        = NULL;
    out[idx].glyph       = g;
    out[idx].keycapLabel = NULL;
    return idx + 1;
  }
  const char *label = bufAppend(bufPos, scancodeName(sc));
  if (!label) return idx;
  out[idx].kind        = TUTORIAL_SEG_GLYPH_KEYCAP;
  out[idx].text        = NULL;
  out[idx].glyph       = NULL;
  out[idx].keycapLabel = label;
  return idx + 1;
}

int tutorialResolveSegments(uint16_t mid, TutorialSeg *out, int max) {
  if (!out || max <= 0) return 0;
  const char *src = langGetText(pickStringId(mid));
  if (!src) return 0;

  /* Touch strings are plain prose — no tokens to expand.  Emit one
     TEXT segment that points at the live language string. */
  if (!strchr(src, '{')) {
    out[0].kind        = TUTORIAL_SEG_TEXT;
    out[0].text        = src;
    out[0].glyph       = NULL;
    out[0].keycapLabel = NULL;
    return 1;
  }

  int idx    = 0;
  int bufPos = 0;
  const char *runStart = src;
  while (*src && idx < max) {
    if (*src == '{') {
      int matched = 0;
      for (int j = 0; j < kTokenCount; j++) {
        size_t tlen = strlen(kTokens[j].token);
        if (strncmp(src, kTokens[j].token, tlen) != 0) continue;
        if (src > runStart) {
          idx = emitText(out, idx, max, &bufPos, runStart, (int)(src - runStart));
          if (idx >= max) break;
        }
        idx = emitGlyphForToken(out, idx, max, &bufPos, &kTokens[j]);
        src += tlen;
        runStart = src;
        matched = 1;
        break;
      }
      if (!matched) src++;
    } else {
      src++;
    }
  }
  if (idx < max && src > runStart) {
    idx = emitText(out, idx, max, &bufPos, runStart, (int)(src - runStart));
  }
  return idx;
}
