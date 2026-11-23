/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          text_cache_pick.h
 * Purpose:       Which slot of the game window's text texture
 *                cache (sdl3draw_status.c) a piece of text uses.
 *
 *   A slot holds the texture of one string as it was rendered:
 *   for one renderer, one face at one size, in one colour. Text
 *   that matches a slot on all of those reuses its texture.
 *   Text that matches none takes an empty slot if there is one,
 *   and otherwise the slot drawn longest ago.
 *
 *   The renderer and the face are compared as addresses only
 *   and never read, so this is logic only, with no SDL, and
 *   compiles into the unit test binary.
 *********************************************************/

#ifndef TEXT_CACHE_PICK_H
#define TEXT_CACHE_PICK_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* What a slot's texture was rendered from. str is NULL in an empty slot. */
typedef struct TextCacheKey {
  const void   *renderer;
  const void   *font;
  float         fontSize;
  unsigned char r, g, b, a;
  const char   *str;
} TextCacheKey;

typedef struct TextCacheEntry {
  TextCacheKey key;
  uint64_t     lastUsed;   /* the cache's clock at the slot's last draw */
} TextCacheEntry;

static inline bool textCacheKeyMatches(const TextCacheKey *have, const TextCacheKey *want) {
  return have->str != NULL && want->str != NULL &&
         have->renderer == want->renderer && have->font == want->font &&
         have->fontSize == want->fontSize &&
         have->r == want->r && have->g == want->g &&
         have->b == want->b && have->a == want->a &&
         strcmp(have->str, want->str) == 0;
}

/* The slot for want among count slots: the one that matches it, with *hit
   set true, or the one to replace, with *hit set false: the first empty
   slot, or with none empty the one with the smallest lastUsed. -1 when
   there are no slots. */
static inline int textCachePick(const TextCacheEntry *slots, int count,
                                const TextCacheKey *want, bool *hit) {
  int replace = -1;
  int i;

  if (hit != NULL) *hit = false;
  if (slots == NULL || want == NULL || count <= 0) return -1;
  for (i = 0; i < count; i++) {
    const TextCacheEntry *s = &slots[i];
    if (textCacheKeyMatches(&s->key, want)) {
      if (hit != NULL) *hit = true;
      return i;
    }
    if (replace >= 0 && slots[replace].key.str == NULL) continue;
    if (replace < 0 || s->key.str == NULL ||
        s->lastUsed < slots[replace].lastUsed) {
      replace = i;
    }
  }
  return replace;
}

#endif /* TEXT_CACHE_PICK_H */
