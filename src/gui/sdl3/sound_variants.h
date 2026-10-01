/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          sound_variants.h
 * Purpose:
 *   Works out which of <name>.wav and <name>_0.wav through
 *   <name>_9.wav a source holds, and hands the members
 *   that decode to the caller with no gaps between them.
 *********************************************************/

#ifndef SOUND_VARIANTS_H
#define SOUND_VARIANTS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOUND_VARIANT_MAX  11   /* the plain name plus _0 .. _9 */
#define SOUND_VARIANT_NAME 64

/* Answers whether one source holds a member. The name is bare — no
   sounds/ prefix — and where the source keeps it is the callback's
   business, not this file's. */
typedef bool (*SoundNameExists)(void *ctx, const char *relName);

/* Decodes one member into pool position slot. False if it would not
   decode, and the position is then left for the next member. */
typedef bool (*SoundMemberLoad)(void *ctx, const char *relName, int slot);

/* Writes the names that exist, plain name first then _0 upward, and
   returns how many. baseName carries its .wav. */
int soundVariantPool(const char *baseName, SoundNameExists exists, void *ctx,
                     char out[SOUND_VARIANT_MAX][SOUND_VARIANT_NAME]);

/* Builds that pool and hands each member to load, counting up a slot
   only when one decodes. Returns how many decoded, so the caller's
   positions 0..n-1 are the playable pool with no holes in it. */
int soundVariantLoad(const char *baseName, SoundNameExists exists,
                     SoundMemberLoad load, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SOUND_VARIANTS_H */
