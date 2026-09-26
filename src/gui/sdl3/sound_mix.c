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
 * Name:          sound_mix.c
 * Purpose:
 *   Adds one playing slot's samples into the effects
 *   mixer's 32-bit accumulator, scaled by the slot's left
 *   and right Q8 gains.
 *********************************************************/

#include "sound_mix.h"

/*********************************************************
*NAME:          soundMixSlot
*PURPOSE:
*  Scales each sample by its channel's Q8 gain and adds it
*  to the accumulator. The channel comes from the sample's
*  absolute index in the sound, not its index in this call:
*  the mixer can stop a slot partway through a frame and
*  resume it on the next callback, and counting from the
*  call would then swap left and right for the rest of the
*  sound.
*
*ARGUMENTS:
*  acc         - Accumulator to add into
*  src         - Interleaved 16-bit samples
*  count       - Number of samples to add
*  firstSample - Absolute index of src[0] in the sound
*  channels    - Device channel count
*  gainL       - Q8 left channel gain
*  gainR       - Q8 right channel gain
*********************************************************/
void soundMixSlot(int32_t *acc, const int16_t *src, uint32_t count,
                  uint32_t firstSample, int channels,
                  uint16_t gainL, uint16_t gainR) {
    uint32_t j;
    int32_t g;

    if (channels == 2) {
        for (j = 0; j < count; j++) {
            g = ((firstSample + j) & 1) ? gainR : gainL;
            acc[j] += ((int32_t)src[j] * g) >> 8;
        }
    } else {
        g = ((int32_t)gainL + (int32_t)gainR) / 2;
        for (j = 0; j < count; j++) {
            acc[j] += ((int32_t)src[j] * g) >> 8;
        }
    }
}
