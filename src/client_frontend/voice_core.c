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
 * Name:          Voice Core
 * Filename:      voice_core.c
 * Purpose:
 *   Opus encode/decode of 20 ms mono frames at 48 kHz.
 *   Every create/destroy tolerates NULL and reports failure
 *   by returning NULL, so a caller with no working audio
 *   device never has to special-case the codec.
 *********************************************************/

#include "voice_core.h"

#include <opus.h>
#include <stdlib.h>
#include <string.h>

struct VoiceEncoder {
    OpusEncoder *enc;
};

struct VoiceDecoder {
    OpusDecoder *dec;
};

VoiceEncoder *voiceEncoderCreate(int bitrate, int complexity) {
    VoiceEncoder *v;
    int err = OPUS_OK;
    int ctl = OPUS_OK;

    v = (VoiceEncoder *)malloc(sizeof(VoiceEncoder));
    if (v == NULL) {
        return NULL;
    }
    memset(v, 0, sizeof(VoiceEncoder));

    v->enc = opus_encoder_create(VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP,
                                 &err);
    if (v->enc == NULL || err != OPUS_OK) {
        voiceEncoderDestroy(v);
        return NULL;
    }

    ctl |= opus_encoder_ctl(v->enc, OPUS_SET_BITRATE((opus_int32)bitrate));
    /* Constrained VBR: a single frame cannot spike past the per-tick byte
     * budget the wire carrier is sized for. */
    ctl |= opus_encoder_ctl(v->enc, OPUS_SET_VBR(1));
    ctl |= opus_encoder_ctl(v->enc, OPUS_SET_VBR_CONSTRAINT(1));
    ctl |= opus_encoder_ctl(v->enc, OPUS_SET_COMPLEXITY((opus_int32)complexity));
    /* No discontinuous transmission: the transmit gate decides when to send,
     * so the codec must not silently drop frames of its own accord. */
    ctl |= opus_encoder_ctl(v->enc, OPUS_SET_DTX(0));
    ctl |= opus_encoder_ctl(v->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    ctl |= opus_encoder_ctl(v->enc, OPUS_SET_INBAND_FEC(0));
    if (ctl != OPUS_OK) {
        voiceEncoderDestroy(v);
        return NULL;
    }

    return v;
}

void voiceEncoderDestroy(VoiceEncoder *enc) {
    if (enc == NULL) {
        return;
    }
    if (enc->enc != NULL) {
        opus_encoder_destroy(enc->enc);
    }
    free(enc);
}

int voiceEncoderEncode(VoiceEncoder *enc, const int16_t *pcm,
                       uint8_t *out, int outCap) {
    opus_int32 written;

    if (enc == NULL || enc->enc == NULL || pcm == NULL || out == NULL ||
        outCap <= 0) {
        return -1;
    }

    written = opus_encode(enc->enc, (const opus_int16 *)pcm,
                          VOICE_FRAME_SAMPLES, (unsigned char *)out,
                          (opus_int32)outCap);
    return (int)written;
}

VoiceDecoder *voiceDecoderCreate(void) {
    VoiceDecoder *v;
    int err = OPUS_OK;

    v = (VoiceDecoder *)malloc(sizeof(VoiceDecoder));
    if (v == NULL) {
        return NULL;
    }
    memset(v, 0, sizeof(VoiceDecoder));

    v->dec = opus_decoder_create(VOICE_SAMPLE_RATE, 1, &err);
    if (v->dec == NULL || err != OPUS_OK) {
        voiceDecoderDestroy(v);
        return NULL;
    }

    return v;
}

void voiceDecoderDestroy(VoiceDecoder *dec) {
    if (dec == NULL) {
        return;
    }
    if (dec->dec != NULL) {
        opus_decoder_destroy(dec->dec);
    }
    free(dec);
}

int voiceDecoderDecode(VoiceDecoder *dec, const uint8_t *data, int len,
                       int16_t *pcm) {
    int decoded;

    if (dec == NULL || dec->dec == NULL || pcm == NULL) {
        return -1;
    }

    if (data == NULL || len <= 0) {
        /* Packet-loss concealment — Opus synthesises a replacement frame. */
        decoded = opus_decode(dec->dec, NULL, 0, (opus_int16 *)pcm,
                              VOICE_FRAME_SAMPLES, 0);
    } else {
        decoded = opus_decode(dec->dec, (const unsigned char *)data,
                              (opus_int32)len, (opus_int16 *)pcm,
                              VOICE_FRAME_SAMPLES, 0);
    }

    return decoded;
}
