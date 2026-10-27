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

size_t tutorialTokensExpand(const char *src, char *out, size_t cap,
                            TutorialTokenNameFn name, void *ctx) {
  size_t n    = 0;
  size_t keep = 0;   /* the end of the last name written: a cut stays past it */
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
    if (id != TT_COUNT) keep = n;
  }
  /* A cut inside a character (the byte left out continues a UTF-8
     sequence) takes off the part of it already written, lead byte and
     all, so the text never ends in half a character. A name is never
     touched, whatever broken bytes follow it. */
  if (((unsigned char)*src & 0xC0) == 0x80) {
    while (n > keep && ((unsigned char)out[n - 1] & 0xC0) == 0x80) n--;
    if (n > keep && (unsigned char)out[n - 1] >= 0xC0) n--;
  }
  out[n] = '\0';
  return n;
}

int tutorialTokensSplit(const char *src, TutorialPiece *out, int max,
                        const char **rest) {
  int         count = 0;
  const char *run;
  if (rest != NULL) *rest = src;
  if (src == NULL || out == NULL || max <= 0) return 0;
  run = src;
  for (;;) {
    size_t          tlen = 0;
    TutorialTokenId id   = TT_COUNT;
    if (*src != '\0') {
      id = tutorialTokenMatch(src, &tlen);
      if (id == TT_COUNT) {
        src++;
        continue;
      }
    }
    /* At a token or at the end: first the plain run before it. */
    if (src > run) {
      if (count == max) break;   /* the run is left out */
      out[count].start = run;
      out[count].len   = (size_t)(src - run);
      out[count].id    = TT_COUNT;
      count++;
      run = src;
    }
    if (*src == '\0') break;
    if (count == max) break;     /* the token is left out */
    out[count].start = src;
    out[count].len   = tlen;
    out[count].id    = id;
    count++;
    src += tlen;
    run = src;
  }
  if (rest != NULL) *rest = run;
  return count;
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
