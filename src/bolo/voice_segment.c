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
 *Name:          Voice Segment
 *Filename:      voice_segment.c
 *Author:        John Morrison
 *Purpose:
 *  Pack and parse the CHANNEL_VOICE segment layout. See
 *  voice_segment.h for the model.
 *
 *  The audio payload is opaque here: nothing in this file
 *  interprets it, which is what lets the server forward
 *  voice without a codec.
 *********************************************************/

#include "voice_segment.h"

#include <string.h>

#include "global.h"

int voiceSegmentPackUp(uint8_t *out, int outCap, uint8_t seq, uint8_t flags,
                       const uint8_t *opus, int opusLen) {
    if (out == NULL || opus == NULL || opusLen <= 0 ||
        opusLen > VOICE_SEG_MAX_OPUS) {
        return 0;
    }
    if (outCap < VOICE_SEG_UP_HEADER + opusLen) {
        return 0;
    }

    out[0] = seq;
    out[1] = flags;
    memcpy(out + VOICE_SEG_UP_HEADER, opus, (size_t)opusLen);
    return VOICE_SEG_UP_HEADER + opusLen;
}

bool voiceSegmentUnpackUp(const uint8_t *in, int inLen, uint8_t *seq,
                          uint8_t *flags, const uint8_t **opus, int *opusLen) {
    int payloadLen;

    if (in == NULL || seq == NULL || flags == NULL || opus == NULL ||
        opusLen == NULL) {
        return false;
    }
    /* A segment with no audio in it carries nothing to play. */
    if (inLen <= VOICE_SEG_UP_HEADER) {
        return false;
    }
    payloadLen = inLen - VOICE_SEG_UP_HEADER;
    if (payloadLen > VOICE_SEG_MAX_OPUS) {
        return false;
    }

    *seq = in[0];
    *flags = in[1];
    *opus = in + VOICE_SEG_UP_HEADER;
    *opusLen = payloadLen;
    return true;
}

int voiceSegmentPackDown(uint8_t *out, int outCap, uint8_t fromPlayer,
                         uint8_t seq, uint8_t flags,
                         const uint8_t *opus, int opusLen) {
    if (out == NULL || opus == NULL || opusLen <= 0 ||
        opusLen > VOICE_SEG_MAX_OPUS) {
        return 0;
    }
    if (fromPlayer >= MAX_TANKS) {
        return 0;
    }
    if (outCap < VOICE_SEG_DOWN_HEADER + opusLen) {
        return 0;
    }

    out[0] = fromPlayer;
    out[1] = seq;
    out[2] = flags;
    memcpy(out + VOICE_SEG_DOWN_HEADER, opus, (size_t)opusLen);
    return VOICE_SEG_DOWN_HEADER + opusLen;
}

bool voiceSegmentUnpackDown(const uint8_t *in, int inLen, uint8_t *fromPlayer,
                            uint8_t *seq, uint8_t *flags,
                            const uint8_t **opus, int *opusLen) {
    int payloadLen;

    if (in == NULL || fromPlayer == NULL || seq == NULL || flags == NULL ||
        opus == NULL || opusLen == NULL) {
        return false;
    }
    if (inLen <= VOICE_SEG_DOWN_HEADER) {
        return false;
    }
    payloadLen = inLen - VOICE_SEG_DOWN_HEADER;
    if (payloadLen > VOICE_SEG_MAX_OPUS) {
        return false;
    }
    /* The sender is a tank slot; anything else has no speaker to play it. */
    if (in[0] >= MAX_TANKS) {
        return false;
    }

    *fromPlayer = in[0];
    *seq = in[1];
    *flags = in[2];
    *opus = in + VOICE_SEG_DOWN_HEADER;
    *opusLen = payloadLen;
    return true;
}
