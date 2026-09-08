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
#include <math.h>
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

/* One buffered frame.  The payload cap is the encoder's own output ceiling
 * rather than the wire segment size: the framing header lives in
 * src/bolo/internal and this file is compiled into the GUI targets, which
 * do not see it.  The transport rejects anything larger before it gets
 * here, so this bound is never the one that bites. */
typedef struct {
    bool    present;
    uint8_t seq;
    uint8_t flags;
    int     len;
    uint8_t data[VOICE_MAX_PACKET];
} VoiceJitterSlot;

struct VoiceSpeaker {
    VoiceDecoder   *dec;
    VoiceJitterSlot slots[VOICE_JITTER_SLOTS];
    bool            primed;         /* playing; nextSeq is meaningful  */
    uint8_t         nextSeq;        /* sequence number due next        */
    int             count;          /* frames currently buffered       */
    bool            waiting;        /* holding for nextSeq to arrive   */
    uint32_t        waitingSince;   /* clock reading the wait began at */
    int             consecutivePlc; /* concealed frames since the last
                                     * real one                        */
    VoiceSpeakerStats stats;        /* cumulative; survives un-priming */
};

/* Sequence numbers wrap at 256, so ordering is the signed difference:
 * true when a comes after b.  Plain > breaks across the wrap. */
static bool voiceSeqAfter(uint8_t a, uint8_t b) {
    return (int8_t)(a - b) > 0;
}

/* Drop everything buffered and stop playing.  The next frames to arrive
 * re-prime the buffer and set a fresh nextSeq from what they carry. */
static void voiceSpeakerUnprime(VoiceSpeaker *sp) {
    int i;

    for (i = 0; i < VOICE_JITTER_SLOTS; i++) {
        sp->slots[i].present = false;
    }
    sp->count = 0;
    sp->primed = false;
    sp->waiting = false;
    sp->consecutivePlc = 0;
}

/* Synthesise a replacement for a frame that is missing or unusable. */
static void voiceSpeakerConceal(VoiceSpeaker *sp, int16_t *pcm) {
    sp->stats.concealed++;
    if (voiceDecoderDecode(sp->dec, NULL, 0, pcm) != VOICE_FRAME_SAMPLES) {
        memset(pcm, 0, VOICE_FRAME_SAMPLES * sizeof(int16_t));
    }
}

VoiceSpeaker *voiceSpeakerCreate(void) {
    VoiceSpeaker *sp;

    sp = (VoiceSpeaker *)malloc(sizeof(VoiceSpeaker));
    if (sp == NULL) {
        return NULL;
    }
    memset(sp, 0, sizeof(VoiceSpeaker));

    sp->dec = voiceDecoderCreate();
    if (sp->dec == NULL) {
        voiceSpeakerDestroy(sp);
        return NULL;
    }

    return sp;
}

void voiceSpeakerDestroy(VoiceSpeaker *sp) {
    if (sp == NULL) {
        return;
    }
    voiceDecoderDestroy(sp->dec);
    free(sp);
}

void voiceSpeakerGetStats(const VoiceSpeaker *sp, VoiceSpeakerStats *out) {
    if (out == NULL) {
        return;
    }
    if (sp == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = sp->stats;
}

void voiceSpeakerPush(VoiceSpeaker *sp, uint8_t seq, uint8_t flags,
                      const uint8_t *opus, int opusLen) {
    int i;
    int target = -1;
    bool evicted = false;

    if (sp == NULL || opus == NULL || opusLen <= 0 ||
        opusLen > (int)sizeof(sp->slots[0].data)) {
        return;
    }

    /* Already played past this one - it arrived too late to be of use. */
    if (sp->primed && voiceSeqAfter(sp->nextSeq, seq)) {
        sp->stats.lateDropped++;
        return;
    }

    for (i = 0; i < VOICE_JITTER_SLOTS; i++) {
        if (sp->slots[i].present) {
            /* A duplicate replaces the copy already held rather than
             * occupying a second slot the pop cursor can never reach. */
            if (sp->slots[i].seq == seq) {
                target = i;
                break;
            }
        } else if (target < 0) {
            target = i;
        }
    }

    if (target < 0) {
        /* Full: the oldest frame is the one closest to being played, and
         * dropping it is what sheds the delay that filled the buffer - see
         * the resynchronisation below. */
        target = 0;
        for (i = 1; i < VOICE_JITTER_SLOTS; i++) {
            if (voiceSeqAfter(sp->slots[target].seq, sp->slots[i].seq)) {
                target = i;
            }
        }
        sp->stats.evicted++;
        evicted = true;
    } else if (!sp->slots[target].present) {
        sp->count++;
    }

    sp->slots[target].present = true;
    sp->slots[target].seq = seq;
    sp->slots[target].flags = flags;
    sp->slots[target].len = opusLen;
    memcpy(sp->slots[target].data, opus, (size_t)opusLen);

    /* Start playing once enough is banked to ride out ordinary jitter,
     * beginning at the oldest frame held. */
    if (!sp->primed && sp->count >= VOICE_JITTER_TARGET) {
        int lowest = -1;
        for (i = 0; i < VOICE_JITTER_SLOTS; i++) {
            if (!sp->slots[i].present) {
                continue;
            }
            if (lowest < 0 || voiceSeqAfter(sp->slots[lowest].seq,
                                            sp->slots[i].seq)) {
                lowest = i;
            }
        }
        sp->primed = true;
        sp->nextSeq = sp->slots[lowest].seq;
        sp->consecutivePlc = 0;
    }

    /* An eviction means the receiver has fallen about VOICE_JITTER_SLOTS
     * frames - some 160 ms - behind its talker.  That depth is the delay the
     * waits in voiceSpeakerPop built up, and the frame just dropped is part
     * of it rather than a loss to be concealed later, so playback moves on to
     * what is still held instead of waiting the deadline out for a frame that
     * is no longer here.
     *
     * The cursor only ever moves forward or stays put, never back: a frame
     * behind nextSeq is dropped as late above, so everything held is at or
     * after it.  And this runs after the arriving frame has been stored,
     * because that frame may be the lowest sequence now held. */
    if (evicted && sp->primed) {
        int lowest = -1;
        for (i = 0; i < VOICE_JITTER_SLOTS; i++) {
            if (!sp->slots[i].present) {
                continue;
            }
            if (lowest < 0 || voiceSeqAfter(sp->slots[lowest].seq,
                                            sp->slots[i].seq)) {
                lowest = i;
            }
        }
        sp->nextSeq = sp->slots[lowest].seq;
        sp->waiting = false;
    }
}

bool voiceSpeakerPop(VoiceSpeaker *sp, int16_t *pcm, uint32_t nowMs) {
    int i;
    int found = -1;
    uint8_t flags;
    bool decoded;

    if (sp == NULL || pcm == NULL || !sp->primed) {
        return false;
    }

    for (i = 0; i < VOICE_JITTER_SLOTS; i++) {
        if (sp->slots[i].present && sp->slots[i].seq == sp->nextSeq) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        /* Nothing for this slot in the sequence yet.  Concealing here and
         * stepping over it would spend a sequence number the sender did
         * fill, so the frame is dropped as late when it does turn up and
         * the cushion is one frame smaller from then on - which is how a
         * receiver ends up permanently ahead of its talker.  Wait for it
         * instead, and give up only once it is overdue. */
        if (!sp->waiting) {
            sp->waiting = true;
            sp->waitingSince = nowMs;
            return false;
        }
        /* Unsigned subtraction, so the wait is still measured correctly
         * across the clock's 32-bit wrap. */
        if ((uint32_t)(nowMs - sp->waitingSince) < VOICE_JITTER_LATE_MS) {
            return false;
        }
        /* Overdue - conceal it and move on.  A run of these means the
         * talker has gone away, so stop rather than conceal forever. */
        sp->waiting = false;
        voiceSpeakerConceal(sp, pcm);
        sp->nextSeq++;
        sp->consecutivePlc++;
        if (sp->consecutivePlc >= VOICE_JITTER_MAX_PLC) {
            voiceSpeakerUnprime(sp);
        }
        return true;
    }

    sp->waiting = false;

    decoded = (voiceDecoderDecode(sp->dec, sp->slots[found].data,
                                  sp->slots[found].len, pcm) ==
               VOICE_FRAME_SAMPLES);
    if (decoded) {
        sp->stats.played++;
    } else {
        voiceSpeakerConceal(sp, pcm);
    }

    /* The slot is spent either way: a payload that would not decode is no
     * more use on the next pop than it was on this one. */
    flags = sp->slots[found].flags;
    sp->slots[found].present = false;
    sp->count--;
    sp->nextSeq++;

    if (decoded) {
        sp->consecutivePlc = 0;
    } else {
        /* Concealed like a missing frame, and counted like one: a run of
         * payloads that will not decode has to end playback too, or the
         * speaker conceals forever. */
        sp->consecutivePlc++;
        if (sp->consecutivePlc >= VOICE_JITTER_MAX_PLC) {
            voiceSpeakerUnprime(sp);
        }
    }

    /* The talker finished on this frame: drop anything still held so the
     * next utterance starts cleanly instead of trailing the last one. */
    if ((flags & VOICE_FLAG_END_OF_UTTERANCE) != 0) {
        voiceSpeakerUnprime(sp);
    }

    return true;
}

/* Audio device ids are handed out per run and do not survive a restart, so a
 * chosen device is persisted by display name and matched against the devices
 * present the next time round.  The matching sits here rather than in the
 * platform backend so it can be exercised without an audio device. */
int voiceDeviceResolveName(const char *saved, const char *const *names,
                           int count) {
    int i;

    if (saved == NULL || saved[0] == '\0' || names == NULL) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        /* A hole in the list is a device whose name the platform would not
         * give up; skip it rather than stopping on it. */
        if (names[i] != NULL && strcmp(names[i], saved) == 0) {
            return i;
        }
    }

    return -1;
}

/* The level meter's peak.  A single frame's RMS is only true for the 20 ms it
 * was measured over, so the drawn value holds the loudest reading for a while
 * and then falls back to what is live now.  The clock arrives as an argument
 * rather than being read here: the caller already has one, and the test drives
 * this with no clock at all. */
float voicePeakUpdate(VoicePeak *p, float level, uint32_t nowMs) {
    float elapsedSec;

    if (p == NULL) {
        return 0.0f;
    }

    /* Clamped before it reaches the state, so one reading from outside the
     * range cannot leave a peak that never comes back down. */
    if (level < 0.0f) {
        level = 0.0f;
    } else if (level > 1.0f) {
        level = 1.0f;
    }

    if (level >= p->peak) {
        /* Louder than what is held, so this becomes the peak and the hold
         * starts again from here.  A zeroed struct takes this branch on its
         * first call, which is what sets lastMs before any decay reads it. */
        p->peak = level;
        p->holdSinceMs = nowMs;
    } else {
        /* Unsigned subtraction, so the hold is measured correctly across the
         * clock's 32-bit wrap. */
        if ((uint32_t)(nowMs - p->holdSinceMs) >= VOICE_PEAK_HOLD_MS) {
            elapsedSec = (float)(uint32_t)(nowMs - p->lastMs) / 1000.0f;
            p->peak -= VOICE_PEAK_DECAY_PER_SEC * elapsedSec;
            /* It falls to the live level and no further: below that it would
             * read as the peak dropping through the bar it sits over. */
            if (p->peak < level) {
                p->peak = level;
            }
            if (p->peak < 0.0f) {
                p->peak = 0.0f;
            }
        }
    }

    p->lastMs = nowMs;
    return p->peak;
}

/* Amplitude to the height it is drawn at.  A meter fed the amplitude straight
 * spends nine tenths of its travel on levels nobody speaks at, so the bottom
 * of it does all the work and normal speech barely lifts off the floor.  The
 * conversion is done in dB instead, which is how a level meter is read: the
 * quiet end gets the room it needs and full scale still reads full. */
float voiceMeterScale(float level) {
    float db;
    float scaled;

    if (level < 0.0f) {
        level = 0.0f;
    } else if (level > 1.0f) {
        level = 1.0f;
    }

    /* Ahead of the logarithm, which has no answer for zero. */
    if (level <= 0.0f) {
        return 0.0f;
    }

    db = 20.0f * log10f(level);
    if (db <= VOICE_METER_FLOOR_DB) {
        return 0.0f;
    }

    scaled = (db - VOICE_METER_FLOOR_DB) / (0.0f - VOICE_METER_FLOOR_DB);
    if (scaled < 0.0f) {
        scaled = 0.0f;
    } else if (scaled > 1.0f) {
        scaled = 1.0f;
    }

    return scaled;
}
