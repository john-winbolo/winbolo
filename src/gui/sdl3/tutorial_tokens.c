/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Tutorial Tokens
 *Filename:      tutorial_tokens.c
 *Purpose:       See tutorial_tokens.h.
 *********************************************************/

#include "tutorial_tokens.h"

#include <string.h>

/* Return is how a tutorial message is closed, and it is not rebindable.
 * The scancode is spelled out so this file needs no SDL header. */
#define TUTORIAL_TOKEN_RETURN_SCANCODE 40   /* SDL_SCANCODE_RETURN */

static const char *const kTokenText[TT_COUNT] = {
  "{ACCEL}",
  "{BRAKE}",
  "{LEFT}",
  "{RIGHT}",
  "{FIRE}",
  "{MINE}",
  "{SCROLL_UP}",
  "{SCROLL_DOWN}",
  "{SCROLL_LEFT}",
  "{SCROLL_RIGHT}",
  "{DISMISS}",
  "{BUILD_MODE}",
  "{BUILD_PLACE}",
  "{BUILD_TOOL}",
  "{QUICK_TREE}",
  "{QUICK_ROAD}",
  "{QUICK_WALL}",
  "{QUICK_PILL}",
  "{QUICK_MINE}",
  "{GUN_UP}",
  "{GUN_DOWN}",
  "{DRIVE_HOW}",
  "{BUILD_HOW}",
  "{HARVEST_TIP}",
  "{PILL_HOW}",
  "{STEP_TREE}",
  "{STEP_ROAD}",
  "{STEP_WALL}",
  "{STEP_MINE}",
  "{STEP_PILL}",
  "{STEP_PILL_GREEN}",
};

const char *tutorialTokenText(TutorialTokenId id) {
  if ((int)id < 0 || id >= TT_COUNT) return NULL;
  return kTokenText[id];
}

TutorialTokenId tutorialTokenMatch(const char *s, size_t *outLen) {
  int i;
  if (s == NULL || *s != '{') return TT_COUNT;
  for (i = 0; i < (int)TT_COUNT; i++) {
    size_t len = strlen(kTokenText[i]);
    if (strncmp(s, kTokenText[i], len) == 0) {
      if (outLen != NULL) *outLen = len;
      return (TutorialTokenId)i;
    }
  }
  return TT_COUNT;
}

int tutorialTokenScancode(TutorialTokenId id, const keyItems *k) {
  if (k == NULL) return 0;
  switch (id) {
    case TT_ACCEL:        return k->kiForward;
    case TT_BRAKE:        return k->kiBackward;
    case TT_LEFT:         return k->kiLeft;
    case TT_RIGHT:        return k->kiRight;
    case TT_FIRE:         return k->kiShoot;
    case TT_MINE:         return k->kiLayMine;
    case TT_SCROLL_UP:    return k->kiScrollUp;
    case TT_SCROLL_DOWN:  return k->kiScrollDown;
    case TT_SCROLL_LEFT:  return k->kiScrollLeft;
    case TT_SCROLL_RIGHT: return k->kiScrollRight;
    case TT_DISMISS:      return TUTORIAL_TOKEN_RETURN_SCANCODE;
    case TT_QUICK_TREE:   return k->kiQuickTree;
    case TT_QUICK_ROAD:   return k->kiQuickRoad;
    case TT_QUICK_WALL:   return k->kiQuickWall;
    case TT_QUICK_PILL:   return k->kiQuickPillbox;
    case TT_QUICK_MINE:   return k->kiQuickMine;
    case TT_GUN_UP:       return k->kiGunIncrease;
    case TT_GUN_DOWN:     return k->kiGunDecrease;
    case TT_BUILD_MODE:
    case TT_BUILD_PLACE:
    case TT_BUILD_TOOL:
    case TT_DRIVE_HOW:
    case TT_BUILD_HOW:
    case TT_HARVEST_TIP:
    case TT_PILL_HOW:
    case TT_STEP_TREE:
    case TT_STEP_ROAD:
    case TT_STEP_WALL:
    case TT_STEP_MINE:
    case TT_STEP_PILL:
    case TT_STEP_PILL_GREEN:
    case TT_COUNT:
      break;
  }
  return 0;
}

bool tutorialTokenIsQuickBuild(TutorialTokenId id) {
  return id == TT_QUICK_TREE || id == TT_QUICK_ROAD ||
         id == TT_QUICK_WALL || id == TT_QUICK_PILL ||
         id == TT_QUICK_MINE;
}

bool tutorialTokenIsPhrase(TutorialTokenId id) {
  return id >= TT_DRIVE_HOW && id < TT_COUNT;
}

/* The phrase pass's name function: the phrase's text for a phrase token,
 * and NULL, which leaves the token as it is written, for any other. */
typedef struct {
  TutorialTokenNameFn phrase;
  void               *ctx;
} PhraseOnly;

static const char *phraseOnlyName(TutorialTokenId id, void *ctx) {
  const PhraseOnly *p = (const PhraseOnly *)ctx;
  if (!tutorialTokenIsPhrase(id) || p->phrase == NULL) return NULL;
  return p->phrase(id, p->ctx);
}

size_t tutorialPhrasesExpand(const char *src, char *out, size_t cap,
                             TutorialTokenNameFn phrase, void *ctx) {
  PhraseOnly p;
  p.phrase = phrase;
  p.ctx    = ctx;
  return tutorialTokensExpand(src, out, cap, phraseOnlyName, &p);
}

size_t tutorialTokensExpand(const char *src, char *out, size_t cap,
                            TutorialTokenNameFn name, void *ctx) {
  size_t n = 0;
  if (out == NULL || cap == 0) return 0;
  out[0] = '\0';
  if (src == NULL) return 0;
  while (*src != '\0') {
    size_t          tlen = 0;
    TutorialTokenId id   = tutorialTokenMatch(src, &tlen);
    const char     *put  = NULL;
    size_t          plen;
    if (id != TT_COUNT) {
      put = (name != NULL) ? name(id, ctx) : NULL;
      if (put == NULL) put = kTokenText[id];
      plen = strlen(put);
    } else {
      put  = src;
      plen = 1;
      tlen = 1;
    }
    if (n + plen >= cap) break;
    memcpy(out + n, put, plen);
    n += plen;
    src += tlen;
  }
  out[n] = '\0';
  return n;
}

const char *tutorialShortKeyName(const char *name, char *buf, size_t cap) {
  static const char kLong[]  = "Keypad ";
  static const char kShort[] = "Num ";
  size_t longLen = sizeof(kLong) - 1;
  size_t restLen;
  if (name == NULL || *name == '\0') return "?";
  if (strncmp(name, kLong, longLen) != 0 || name[longLen] == '\0') return name;
  restLen = strlen(name + longLen);
  if (buf == NULL || cap < sizeof(kShort) + restLen) return name;
  memcpy(buf, kShort, sizeof(kShort) - 1);
  memcpy(buf + sizeof(kShort) - 1, name + longLen, restLen + 1);
  return buf;
}

float tutorialKeycapWidth(float capH, float labelW) {
  /* A margin of a third of the cap's height each side keeps the label
     clear of the rounded corners. */
  float w = labelW + capH * 0.66f;
  return (w > capH) ? w : capH;
}

bool tutorialLangRefParse(const char *s, char *name, size_t cap) {
  static const char kOpen[] = "{LANG:";
  const size_t openLen = sizeof(kOpen) - 1;
  size_t len;
  size_t i;

  if (!s || !name || cap == 0) return false;
  if (strncmp(s, kOpen, openLen) != 0) return false;
  s += openLen;
  for (len = 0; s[len] && s[len] != '}'; len++) {
    char c = s[len];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  /* A name, then the closing brace, then the end of the text. */
  if (len == 0 || s[len] != '}' || s[len + 1] != '\0') return false;
  if (len >= cap) return false;
  for (i = 0; i < len; i++) name[i] = s[i];
  name[len] = '\0';
  return true;
}
