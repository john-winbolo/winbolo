/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include "tutorial_tokens.h"
#include "input_gamepad.h"
#include "../../steam/steam_input_actions.h"

/* Global key-binding struct; defined in winbolo.c (and the
 * platform main_*.c files). */
extern keyItems keys;

/* Per-call render buffer.  TEXT and keycap-label runs are copied
 * here so the segment producer can return stable pointers without
 * allocating.  The dialog consumes the result synchronously inside
 * frontEndTutorial; subsequent calls overwrite the buffer. */
static char tutorialBuf[2048];

/* What a token draws as on a controller, one row per TutorialTokenId and
 * in its order. The token's text and its keyboard key come from
 * tutorial_tokens.c. The quick-build keys have no button of their own on a
 * pad, so they show the build tool's, which is how a pad picks a tool. */
typedef struct {
  const char   *gpAction;   /* Path B glyph_* pseudo-action (NULL if none) */
  const char   *siAction;   /* Steam Input action name (NULL -> skip Path A) */
  GamepadAction sdlAction;  /* live SDL binding action (GP_ACT_COUNT -> none) */
} TokenEntry;

static const TokenEntry kTokens[TT_COUNT] = {
  [TT_ACCEL]        = { "glyph_tank_forward",  NULL,                          GP_ACT_COUNT },
  [TT_BRAKE]        = { "glyph_tank_back",     NULL,                          GP_ACT_COUNT },
  [TT_LEFT]         = { "glyph_tank_left",     NULL,                          GP_ACT_COUNT },
  [TT_RIGHT]        = { "glyph_tank_right",    NULL,                          GP_ACT_COUNT },
  [TT_FIRE]         = { "glyph_fire",          SI_ACTION_FIRE,                GP_ACT_FIRE },
  [TT_MINE]         = { "glyph_mine",          SI_ACTION_MINE,                GP_ACT_MINE },
  [TT_SCROLL_UP]    = { "glyph_scroll_up",     NULL,                          GP_ACT_COUNT },
  [TT_SCROLL_DOWN]  = { "glyph_scroll_down",   NULL,                          GP_ACT_COUNT },
  [TT_SCROLL_LEFT]  = { "glyph_scroll_left",   NULL,                          GP_ACT_COUNT },
  [TT_SCROLL_RIGHT] = { "glyph_scroll_right",  NULL,                          GP_ACT_COUNT },
  [TT_DISMISS]      = { "glyph_dismiss",       SI_ACTION_MENU_ACCEPT,         GP_ACT_COUNT },
  [TT_BUILD_MODE]   = { NULL,                  SI_ACTION_BUILD_CURSOR_TOGGLE, GP_ACT_BUILD_CURSOR_TOGGLE },
  [TT_BUILD_PLACE]  = { NULL,                  SI_ACTION_BUILD_CONFIRM,       GP_ACT_BUILD_CONFIRM },
  [TT_BUILD_TOOL]   = { NULL,                  SI_ACTION_BUILD_NEXT,          GP_ACT_BUILD_NEXT },
  [TT_QUICK_TREE]   = { NULL,                  SI_ACTION_BUILD_NEXT,          GP_ACT_BUILD_NEXT },
  [TT_QUICK_ROAD]   = { NULL,                  SI_ACTION_BUILD_NEXT,          GP_ACT_BUILD_NEXT },
  [TT_QUICK_WALL]   = { NULL,                  SI_ACTION_BUILD_NEXT,          GP_ACT_BUILD_NEXT },
  [TT_QUICK_PILL]   = { NULL,                  SI_ACTION_BUILD_NEXT,          GP_ACT_BUILD_NEXT },
  [TT_QUICK_MINE]   = { NULL,                  SI_ACTION_BUILD_NEXT,          GP_ACT_BUILD_NEXT },
};

/* Desktop -> input-variant lookup. Strings whose desktop wording
 * references hardware controls (keys, mouse, keypad) get touch and/or
 * controller siblings; the rest fall through unchanged. A 0 in a slot
 * means "no variant — reuse the desktop string". */
typedef struct {
  uint16_t desktop;
  uint16_t touch;       /* 0 = no touch variant, reuse desktop */
  uint16_t controller;  /* 0 = no controller variant, reuse desktop */
} StringVariants;

static const StringVariants kStringVariants[] = {
  { STR_TUTORIAL01,       STR_TUTORIAL01_TOUCH,       STR_TUTORIAL01_CTRL       },
  { STR_TUTORIAL02,       STR_TUTORIAL02_TOUCH,       STR_TUTORIAL02_CTRL       },
  { STR_TUTORIAL03,       STR_TUTORIAL03_TOUCH,       STR_TUTORIAL03_CTRL       },
  { STR_TUTORIAL04,       STR_TUTORIAL04_TOUCH,       STR_TUTORIAL04_CTRL       },
  { STR_TUTORIAL05,       STR_TUTORIAL05_TOUCH,       STR_TUTORIAL05_CTRL       },
  { STR_TUTORIAL06,       STR_TUTORIAL06_TOUCH,       STR_TUTORIAL06_CTRL       },
  { STR_TUTORIAL10,       STR_TUTORIAL10_TOUCH,       STR_TUTORIAL10_CTRL       },
  { STR_TUTORIAL14,       STR_TUTORIAL14_TOUCH,       STR_TUTORIAL14_CTRL       },
  { STR_TUTORIAL16,       STR_TUTORIAL16_TOUCH,       STR_TUTORIAL16_CTRL       },
  { STR_TUTORIAL18,       STR_TUTORIAL18_TOUCH,       STR_TUTORIAL18_CTRL       },
  { STR_TUTORIAL19,       STR_TUTORIAL19_TOUCH,       STR_TUTORIAL19_CTRL       },
  { STR_TUTORIAL21,       STR_TUTORIAL21_TOUCH,       STR_TUTORIAL21_CTRL       },
  { STR_TUTORIAL_START01, STR_TUTORIAL_START01_TOUCH, 0                         },
  { STR_TUTORIAL_START04, STR_TUTORIAL_START04_TOUCH, STR_TUTORIAL_START04_CTRL },
};
static const int kStringVariantCount =
    (int)(sizeof(kStringVariants) / sizeof(kStringVariants[0]));

static uint16_t pickStringId(uint16_t mid) {
  int i;
  for (i = 0; i < kStringVariantCount; i++) {
    if (kStringVariants[i].desktop != mid) continue;
    if (uiShouldUseControllerMode())
      return kStringVariants[i].controller ? kStringVariants[i].controller : mid;
    if (uiModeIsTablet())
      return kStringVariants[i].touch ? kStringVariants[i].touch : mid;
    return mid;
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
                             int *bufPos, TutorialTokenId id) {
  const TokenEntry *t = &kTokens[id];
  if (idx >= max) return idx;
  /* A touch screen has no key for a quick-build tool and no pad button
     either: the player taps the build buttons, so say that in words. */
  if (!uiShouldUseControllerMode() && uiModeIsTablet() &&
      tutorialTokenIsQuickBuild(id)) {
    const char *words = langGetText(STR_TUTORIAL_QUICK_BUILD_TOUCH);
    if (!words) words = "";
    return emitText(out, idx, max, bufPos, words, (int)strlen(words));
  }
  if (uiShouldUseControllerMode()) {
    SDL_Texture *g = NULL;
    if (t->siAction || t->sdlAction < GP_ACT_COUNT) {
      g = glyphForControllerAction(t->siAction, t->sdlAction);   /* discrete: backend-correct */
    }
    if (!g && t->gpAction) {
      g = glyphForGamepadAction(t->gpAction);   /* analog stick art, or legacy fallback on a discrete miss */
    }
    if (g) {
      out[idx].kind        = TUTORIAL_SEG_GLYPH_PNG;
      out[idx].text        = NULL;
      out[idx].glyph       = g;
      out[idx].keycapLabel = NULL;
      return idx + 1;
    }
    /* Nothing resolved (e.g. a build token with no Steam + no glyph art).
       Render a token-text keycap so the dialog still shows something. */
    static bool warned = false;
    if (!warned) {
      WB_LOG_WARN(WB_LOG_CAT_GUI, "tutorial: no controller glyph for '%s'",
                  tutorialTokenText(id));
      warned = true;
    }
    const char *label = bufAppend(bufPos, tutorialTokenText(id));
    if (!label) return idx;
    out[idx].kind        = TUTORIAL_SEG_GLYPH_KEYCAP;
    out[idx].text        = NULL;
    out[idx].glyph       = NULL;
    out[idx].keycapLabel = label;
    return idx + 1;
  }
  /* Keyboard path */
  int sc = tutorialTokenScancode(id, &keys);
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

int tutorialResolveText(const char *src, TutorialSeg *out, int max) {
  if (!out || max <= 0 || !src) return 0;

  /* Plain prose — no tokens to expand.  Emit one TEXT segment that points
     at the caller's own string. */
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
    size_t tlen = 0;
    TutorialTokenId id = (*src == '{') ? tutorialTokenMatch(src, &tlen) : TT_COUNT;
    if (id == TT_COUNT) {
      src++;
      continue;
    }
    if (src > runStart) {
      idx = emitText(out, idx, max, &bufPos, runStart, (int)(src - runStart));
      if (idx >= max) break;
    }
    idx = emitGlyphForToken(out, idx, max, &bufPos, id);
    src += tlen;
    runStart = src;
  }
  if (idx < max && src > runStart) {
    idx = emitText(out, idx, max, &bufPos, runStart, (int)(src - runStart));
  }
  return idx;
}

int tutorialResolveSegments(uint16_t mid, TutorialSeg *out, int max) {
  if (!out || max <= 0) return 0;
  return tutorialResolveText(langGetText(pickStringId(mid)), out, max);
}
