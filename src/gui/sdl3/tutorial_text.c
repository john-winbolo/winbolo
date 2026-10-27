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

/* Set when a run did not fit in tutorialBuf, so tutorialResolveText can
   say the text was cut. */
static bool tutorialBufFull = false;

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
  [TT_GUN_UP]       = { NULL,                  SI_ACTION_GUNSIGHT_INC,        GP_ACT_GUNSIGHT_INC },
  [TT_GUN_DOWN]     = { NULL,                  SI_ACTION_GUNSIGHT_DEC,        GP_ACT_GUNSIGHT_DEC },
  /* The phrase tokens are filled in with words before any glyph is
     looked for, so none of them is ever drawn as a glyph. */
  [TT_DRIVE_HOW]       = { NULL, NULL, GP_ACT_COUNT },
  [TT_BUILD_HOW]       = { NULL, NULL, GP_ACT_COUNT },
  [TT_HARVEST_TIP]     = { NULL, NULL, GP_ACT_COUNT },
  [TT_PILL_HOW]        = { NULL, NULL, GP_ACT_COUNT },
  [TT_STEP_TREE]       = { NULL, NULL, GP_ACT_COUNT },
  [TT_STEP_ROAD]       = { NULL, NULL, GP_ACT_COUNT },
  [TT_STEP_WALL]       = { NULL, NULL, GP_ACT_COUNT },
  [TT_STEP_MINE]       = { NULL, NULL, GP_ACT_COUNT },
  [TT_STEP_PILL]       = { NULL, NULL, GP_ACT_COUNT },
  [TT_STEP_PILL_GREEN] = { NULL, NULL, GP_ACT_COUNT },
};

/* The player's input, as the tutorial words it. */
typedef enum {
  TUT_INPUT_KEYS = 0,   /* keyboard and mouse */
  TUT_INPUT_TOUCH,      /* the touch layout */
  TUT_INPUT_PAD         /* a controller */
} TutInput;

/* The same order pickStringId follows: on a phone or tablet the touch
   layout first, as its controls are on the screen whatever SDL counts as
   a gamepad; then a controller; then the touch layout on a desktop. */
static TutInput currentInput(void) {
#if defined(__IPHONEOS__) || defined(__ANDROID__)
  if (uiModeIsTablet()) return TUT_INPUT_TOUCH;
#endif
  if (uiShouldUseControllerMode()) return TUT_INPUT_PAD;
  if (uiModeIsTablet()) return TUT_INPUT_TOUCH;
  return TUT_INPUT_KEYS;
}

/* The language file's strings for each phrase token, one for each input.
   A 0 for the controller uses the keyboard's words: a controller draws a
   key token as its own glyph, so the keyboard's sentence fits it. */
typedef struct {
  TutorialTokenId id;
  uint16_t        keys;
  uint16_t        touch;
  uint16_t        pad;
} PhraseRow;

static const PhraseRow kPhrases[] = {
  { TT_DRIVE_HOW,       STR_TUTPHRASE_DRIVE_HOW_KEYS,
    STR_TUTPHRASE_DRIVE_HOW_TOUCH,       0 },
  { TT_BUILD_HOW,       STR_TUTPHRASE_BUILD_HOW_KEYS,
    STR_TUTPHRASE_BUILD_HOW_TOUCH,       STR_TUTPHRASE_BUILD_HOW_PAD },
  { TT_HARVEST_TIP,     STR_TUTPHRASE_HARVEST_TIP_KEYS,
    STR_TUTPHRASE_HARVEST_TIP_TOUCH,     STR_TUTPHRASE_HARVEST_TIP_PAD },
  { TT_PILL_HOW,        STR_TUTPHRASE_PILL_HOW_KEYS,
    STR_TUTPHRASE_PILL_HOW_TOUCH,        STR_TUTPHRASE_PILL_HOW_PAD },
  { TT_STEP_TREE,       STR_TUTPHRASE_STEP_TREE_KEYS,
    STR_TUTPHRASE_STEP_TREE_TOUCH,       STR_TUTPHRASE_STEP_TREE_PAD },
  { TT_STEP_ROAD,       STR_TUTPHRASE_STEP_ROAD_KEYS,
    STR_TUTPHRASE_STEP_ROAD_TOUCH,       STR_TUTPHRASE_STEP_ROAD_PAD },
  { TT_STEP_WALL,       STR_TUTPHRASE_STEP_WALL_KEYS,
    STR_TUTPHRASE_STEP_WALL_TOUCH,       STR_TUTPHRASE_STEP_WALL_PAD },
  { TT_STEP_MINE,       STR_TUTPHRASE_STEP_MINE_KEYS,
    STR_TUTPHRASE_STEP_MINE_TOUCH,       STR_TUTPHRASE_STEP_MINE_PAD },
  { TT_STEP_PILL,       STR_TUTPHRASE_STEP_PILL_KEYS,
    STR_TUTPHRASE_STEP_PILL_TOUCH,       STR_TUTPHRASE_STEP_PILL_PAD },
  { TT_STEP_PILL_GREEN, STR_TUTPHRASE_STEP_PILL_GREEN_KEYS,
    STR_TUTPHRASE_STEP_PILL_GREEN_TOUCH, STR_TUTPHRASE_STEP_PILL_GREEN_PAD },
};

/* A phrase token's words for the player's input, from the language file
   (its English where the language leaves the string out); NULL for any
   other token, which is left for the key pass. */
static const char *phraseText(TutorialTokenId id, void *ctx) {
  size_t i;
  (void)ctx;
  for (i = 0; i < sizeof(kPhrases) / sizeof(kPhrases[0]); i++) {
    uint16_t    sid;
    const char *words;
    if (kPhrases[i].id != id) continue;
    switch (currentInput()) {
      case TUT_INPUT_TOUCH: sid = kPhrases[i].touch; break;
      case TUT_INPUT_PAD:   sid = kPhrases[i].pad;   break;
      case TUT_INPUT_KEYS:
      default:              sid = kPhrases[i].keys;  break;
    }
    if (sid == 0) sid = kPhrases[i].keys;
    words = langGetText(sid);
    return words ? words : "";
  }
  return NULL;
}

/* A popup's text with its phrase tokens filled in; the key tokens they
   carry are left for the segment pass. Good until the next call, like
   tutorialBuf. */
static char phraseBuf[2048];

/* What a token is on a touch screen, in words; NULL when the touch screen
   has nothing of its own for it and the keyboard key is named. A quick-build
   tool has no key and no button there: the player taps the build buttons.
   The gunsight range has its own two round buttons, drawn "+" and "-",
   and firing has its own button. */
static const char *touchTokenWords(TutorialTokenId id) {
  if (tutorialTokenIsQuickBuild(id)) {
    const char *words = langGetText(STR_TUTORIAL_QUICK_BUILD_TOUCH);
    return words ? words : "";
  }
  if (id == TT_FIRE)     return langGetText(STR_TUTTOK_TOUCH_FIRE);
  if (id == TT_GUN_UP)   return langGetText(STR_TUTTOK_TOUCH_GUN_UP);
  if (id == TT_GUN_DOWN) return langGetText(STR_TUTTOK_TOUCH_GUN_DOWN);
  return NULL;
}

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
#if defined(__IPHONEOS__) || defined(__ANDROID__)
    /* The touch layout's wording first: its controls are on the screen,
       whatever SDL counts as a gamepad (an Android keyboard with a d-pad is
       one, and the token in the controller wording then drew a keycap that
       read "{"). */
    if (uiModeIsTablet())
      return kStringVariants[i].touch ? kStringVariants[i].touch : mid;
#endif
    if (uiShouldUseControllerMode())
      return kStringVariants[i].controller ? kStringVariants[i].controller : mid;
    if (uiModeIsTablet())
      return kStringVariants[i].touch ? kStringVariants[i].touch : mid;
    return mid;
  }
  return mid;
}

/* A key's name as the tutorial writes it, in a key cap or in a plain
 * line: SDL's name, with "Keypad " cut to "Num " (tutorialShortKeyName).
 * The answer is good until the next call; both callers copy it at once. */
static const char *scancodeName(int scancode) {
  static char shortName[64];
  if (scancode <= 0) return "?";
  return tutorialShortKeyName(SDL_GetScancodeName((SDL_Scancode)scancode),
                              shortName, sizeof(shortName));
}

/* Append `s` (NUL-terminated) into tutorialBuf at *pos, NUL-terminating
 * the run, advancing *pos past the terminator, and returning the
 * pointer to the written run.  Returns NULL on overflow.  Each call
 * starts a fresh run — successive calls produce distinct stable
 * pointers within the same buffer. */
static const char *bufAppend(int *pos, const char *s) {
  int avail = (int)sizeof(tutorialBuf) - *pos - 1;
  int len   = (int)strlen(s);
  if (len > avail) {
    tutorialBufFull = true;
    return NULL;
  }
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
  if (len > avail) {
    tutorialBufFull = true;
    return idx;
  }
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
  /* A touch screen has no key for a quick-build tool or the gunsight
     range: say in words what the player taps there. */
  if (!uiShouldUseControllerMode() && uiModeIsTablet()) {
    const char *words = touchTokenWords(id);
    if (words) {
      return emitText(out, idx, max, bufPos, words, (int)strlen(words));
    }
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

  /* The phrase tokens first: their words carry key tokens of their own,
     which the loop below then draws like any other. */
  if (strchr(src, '{')) {
    tutorialPhrasesExpand(src, phraseBuf, sizeof(phraseBuf), phraseText, NULL);
    src = phraseBuf;
  }

  /* Plain prose — no tokens to expand.  Emit one TEXT segment that points
     at the caller's own string. */
  if (!strchr(src, '{')) {
    out[0].kind        = TUTORIAL_SEG_TEXT;
    out[0].text        = src;
    out[0].glyph       = NULL;
    out[0].keycapLabel = NULL;
    return 1;
  }

  TutorialPiece pieces[TUTORIAL_SEG_MAX];
  const char   *rest = NULL;
  int           count;
  int           idx    = 0;
  int           bufPos = 0;
  int           i;
  if (max > TUTORIAL_SEG_MAX) max = TUTORIAL_SEG_MAX;
  tutorialBufFull = false;
  count = tutorialTokensSplit(src, pieces, max, &rest);
  for (i = 0; i < count; i++) {
    if (pieces[i].id == TT_COUNT) {
      idx = emitText(out, idx, max, &bufPos, pieces[i].start,
                     (int)pieces[i].len);
    } else {
      idx = emitGlyphForToken(out, idx, max, &bufPos, pieces[i].id);
    }
  }
  if (tutorialBufFull || (rest != NULL && *rest != '\0')) {
    /* More pieces than segments, or more text than the buffer holds: the
       rest is not drawn. Said once, as the text comes again every frame. */
    static bool warned = false;
    if (!warned) {
      WB_LOG_WARN(WB_LOG_CAT_GUI, "tutorial: a text is too long to draw "
                  "whole; drew %d of its pieces", idx);
      warned = true;
    }
  }
  return idx;
}

/* A token in words, for a controller: a plain line has no room for a
   glyph, so it names the button ("Shoot ... with the fire button"), in
   the player's language. The quick-build keys are picked with the build
   tool's button. The gunsight buttons are named by what they do: a player
   may rebind them, and the Steam and SDL defaults put them on opposite
   shoulders. 0 for the phrase tokens, which are words already. */
static const uint16_t kTokenWordsPad[TT_COUNT] = {
  [TT_ACCEL]        = STR_TUTTOK_PAD_ACCEL,
  [TT_BRAKE]        = STR_TUTTOK_PAD_BRAKE,
  [TT_LEFT]         = STR_TUTTOK_PAD_LEFT,
  [TT_RIGHT]        = STR_TUTTOK_PAD_RIGHT,
  [TT_FIRE]         = STR_TUTTOK_PAD_FIRE,
  [TT_MINE]         = STR_TUTTOK_PAD_MINE,
  [TT_SCROLL_UP]    = STR_TUTTOK_PAD_SCROLL_UP,
  [TT_SCROLL_DOWN]  = STR_TUTTOK_PAD_SCROLL_DOWN,
  [TT_SCROLL_LEFT]  = STR_TUTTOK_PAD_SCROLL_LEFT,
  [TT_SCROLL_RIGHT] = STR_TUTTOK_PAD_SCROLL_RIGHT,
  [TT_DISMISS]      = STR_TUTTOK_PAD_DISMISS,
  [TT_BUILD_MODE]   = STR_TUTTOK_PAD_BUILD_MODE,
  [TT_BUILD_PLACE]  = STR_TUTTOK_PAD_BUILD_PLACE,
  [TT_BUILD_TOOL]   = STR_TUTTOK_PAD_BUILD_TOOL,
  [TT_QUICK_TREE]   = STR_TUTTOK_PAD_BUILD_TOOL,
  [TT_QUICK_ROAD]   = STR_TUTTOK_PAD_BUILD_TOOL,
  [TT_QUICK_WALL]   = STR_TUTTOK_PAD_BUILD_TOOL,
  [TT_QUICK_PILL]   = STR_TUTTOK_PAD_BUILD_TOOL,
  [TT_QUICK_MINE]   = STR_TUTTOK_PAD_BUILD_TOOL,
  [TT_GUN_UP]       = STR_TUTTOK_PAD_GUN_UP,
  [TT_GUN_DOWN]     = STR_TUTTOK_PAD_GUN_DOWN,
};

static const char *plainTokenName(TutorialTokenId id, void *ctx) {
  (void)ctx;
  if (tutorialTokenIsPhrase(id)) return NULL;   /* filled in already */
  if (uiShouldUseControllerMode()) {
    const char *words = langGetText(kTokenWordsPad[id]);
    return words ? words : "";
  }
  if (uiModeIsTablet()) {
    const char *words = touchTokenWords(id);
    if (words) return words;
  }
  return scancodeName(tutorialTokenScancode(id, &keys));
}

void tutorialExpandTextPlain(const char *src, char *out, size_t cap) {
  /* The phrase tokens first, then the key tokens their words carry. */
  char phrased[1024];
  tutorialPhrasesExpand(src, phrased, sizeof(phrased), phraseText, NULL);
  tutorialTokensExpand(phrased, out, cap, plainTokenName, NULL);
}

int tutorialResolveSegments(uint16_t mid, TutorialSeg *out, int max) {
  if (!out || max <= 0) return 0;
  return tutorialResolveText(langGetText(pickStringId(mid)), out, max);
}
