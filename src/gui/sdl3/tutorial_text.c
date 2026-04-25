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

#include "../Input.h"
#include "../lang.h"
#include "../ui_mode.h"

/* Global key-binding struct; defined in winbolo.c (and the
 * platform main_*.c files). */
extern keyItems keys;

/* Per-call render buffer. The tutorial sequencer calls this once
 * per dialog and consumes the result before the next call, so a
 * single static buffer is fine. Sized for the longest tutorial
 * string with all tokens expanded — comfortably over the cap. */
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

typedef struct {
  const char *token;
  KeyAccessor get;
} TokenEntry;

static const TokenEntry kTokens[] = {
  { "{ACCEL}",        kt_accel },
  { "{BRAKE}",        kt_brake },
  { "{LEFT}",         kt_left },
  { "{RIGHT}",        kt_right },
  { "{FIRE}",         kt_fire },
  { "{MINE}",         kt_mine },
  { "{SCROLL_UP}",    kt_scroll_up },
  { "{SCROLL_DOWN}",  kt_scroll_down },
  { "{SCROLL_LEFT}",  kt_scroll_left },
  { "{SCROLL_RIGHT}", kt_scroll_right },
};
static const int kTokenCount = (int)(sizeof(kTokens) / sizeof(kTokens[0]));

/* Desktop → touch sibling lookup. Strings whose desktop wording
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

/* Unbound or unknown scancodes return "" from SDL — fall back to a
 * visible placeholder so the dialog doesn't end up reading
 * "press the  key". */
static const char *scancodeName(int scancode) {
  const char *name;
  if (scancode <= 0) return "?";
  name = SDL_GetScancodeName((SDL_Scancode)scancode);
  if (!name || !*name) return "?";
  return name;
}

const char *tutorialResolveText(uint16_t mid) {
  const char *src = langGetText(pickStringId(mid));
  char *out;
  char *end;

  /* Touch strings are plain prose — no tokens to expand. Skip the
   * copy in that common case. */
  if (!strchr(src, '{')) return src;

  out = tutorialBuf;
  end = tutorialBuf + sizeof(tutorialBuf) - 1;
  while (*src && out < end) {
    if (*src == '{') {
      int matched = 0;
      int j;
      for (j = 0; j < kTokenCount; j++) {
        size_t tlen = strlen(kTokens[j].token);
        if (strncmp(src, kTokens[j].token, tlen) == 0) {
          const char *name = scancodeName(kTokens[j].get());
          while (*name && out < end) *out++ = *name++;
          src += tlen;
          matched = 1;
          break;
        }
      }
      if (!matched) *out++ = *src++;
    } else {
      *out++ = *src++;
    }
  }
  *out = '\0';
  return tutorialBuf;
}
