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
