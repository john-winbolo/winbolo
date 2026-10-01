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
 * Name:          sound_mix.h
 * Purpose:
 *   Adds one playing slot's samples into the effects
 *   mixer's 32-bit accumulator, scaled by the slot's left
 *   and right Q8 gains.
 *********************************************************/

#ifndef SOUND_MIX_H
#define SOUND_MIX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adds count interleaved 16-bit samples from src into acc, each scaled by
   its channel's Q8 gain as (sample * gain) >> 8. firstSample is the absolute
   index of src[0] within the sound, which starts on a left sample, so on a
   two-channel device an even absolute index is left and an odd one right.
   On a one-channel device every sample uses (gainL + gainR) / 2. */
void soundMixSlot(int32_t *acc, const int16_t *src, uint32_t count,
                  uint32_t firstSample, int channels,
                  uint16_t gainL, uint16_t gainR);

#ifdef __cplusplus
}
#endif

#endif /* SOUND_MIX_H */
