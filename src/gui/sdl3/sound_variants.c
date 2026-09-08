/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          sound_variants.c
 * Purpose:
 *   Works out which of <name>.wav and <name>_0.wav through
 *   <name>_9.wav a source holds, and hands the members
 *   that decode to the caller with no gaps between them.
 *********************************************************/

#include "sound_variants.h"

#include <stdio.h>
#include <string.h>

/*********************************************************
*NAME:          soundVariantPool
*PURPOSE:
*  Collects the members of one sound's pool: the plain name
*  first, then _0 through _9 in order, keeping whichever of
*  them the source says it holds. The digit goes in front of
*  the final '.', so farming_tree_near.wav gives
*  farming_tree_near_0.wav; a name with no '.' takes the
*  suffix at its end. Names are passed to exists exactly as
*  given, with no path in front of them and no change of
*  case: where a member lives and how names are matched are
*  the callback's business. _10 and above are not part of the
*  scheme and are never asked for.
*
*ARGUMENTS:
*  baseName - the sound's file name, carrying its extension
*  exists   - answers whether the source holds one member
*  ctx      - passed through to exists untouched
*  out      - filled with the names that exist, in pool order
*
*RETURNS:
*  How many names were written to out, 0 to SOUND_VARIANT_MAX
*********************************************************/
int soundVariantPool(const char *baseName, SoundNameExists exists, void *ctx,
                     char out[SOUND_VARIANT_MAX][SOUND_VARIANT_NAME]) {
    const char *dot;
    const char *tail;
    size_t baseLen;
    size_t stemLen;
    int digit;
    int count = 0;

    if (baseName == NULL || exists == NULL || out == NULL) return 0;

    baseLen = strlen(baseName);
    if (baseLen + 1 > SOUND_VARIANT_NAME) return 0;

    if (exists(ctx, baseName)) {
        memcpy(out[count], baseName, baseLen + 1);
        count++;
    }

    dot = strrchr(baseName, '.');
    stemLen = (dot != NULL) ? (size_t)(dot - baseName) : baseLen;
    tail = (dot != NULL) ? dot : "";

    for (digit = 0; digit <= 9; digit++) {
        char name[SOUND_VARIANT_NAME];
        int written = snprintf(name, sizeof(name), "%.*s_%d%s",
                               (int)stemLen, baseName, digit, tail);
        /* A member whose name will not fit is skipped rather than asked
           for under a truncated name. */
        if (written < 0 || (size_t)written >= sizeof(name)) continue;
        if (exists(ctx, name)) {
            memcpy(out[count], name, (size_t)written + 1);
            count++;
        }
    }

    return count;
}

/*********************************************************
*NAME:          soundVariantLoad
*PURPOSE:
*  Builds the pool and offers each member to load in pool
*  order, giving it the position the member would take. A
*  member load refuses does not use up its position, so the
*  members that did decode end up at positions 0 to the
*  return value minus one with nothing missing in between.
*
*ARGUMENTS:
*  baseName - the sound's file name, carrying its extension
*  exists   - answers whether the source holds one member
*  load     - decodes one member into a pool position
*  ctx      - passed through to both callbacks untouched
*
*RETURNS:
*  How many members decoded, 0 to SOUND_VARIANT_MAX
*********************************************************/
int soundVariantLoad(const char *baseName, SoundNameExists exists,
                     SoundMemberLoad load, void *ctx) {
    char names[SOUND_VARIANT_MAX][SOUND_VARIANT_NAME];
    int found;
    int i;
    int slot = 0;

    if (load == NULL) return 0;

    found = soundVariantPool(baseName, exists, ctx, names);
    for (i = 0; i < found; i++) {
        if (load(ctx, names[i], slot)) slot++;
    }

    return slot;
}
