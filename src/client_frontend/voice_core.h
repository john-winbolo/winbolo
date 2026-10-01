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
 * Name:          Voice Core
 * Filename:      voice_core.h
 * Purpose:
 *   Platform-neutral voice codec wrappers. Compiled into
 *   each client frontend target; the platform capture and
 *   playback backends (src/gui/sdl3/voice.c) drive it.
 *
 *   The Opus types stay inside voice_core.c so no consumer
 *   needs the codec headers on its include path.
 *
 *   The receive side of one remote talker - jitter buffer,
 *   decoder, and loss concealment - is a VoiceSpeaker. It
 *   knows nothing of the wire framing: the caller hands it
 *   already-parsed sequence, flags, and payload.
 *********************************************************/

#ifndef VOICE_CORE_H
#define VOICE_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_SAMPLE_RATE     48000
#define VOICE_FRAME_SAMPLES     960   /* 20 ms mono @ 48 kHz */
#define VOICE_MAX_PACKET        400   /* encoder output ceiling */
#define VOICE_DEFAULT_BITRATE 24000
/* Encoder complexity. The web build encodes on the same single thread that
 * runs the game and draws the frame, so it trades quality for cycles the
 * desktop build does not have to count; 2 keeps most of the saving over 5
 * without dropping to the floor. The split is a judgement - the wasm build
 * has not been profiled. */
#ifdef __EMSCRIPTEN__
#define VOICE_DEFAULT_COMPLEXITY  2
#else
#define VOICE_DEFAULT_COMPLEXITY  5
#endif

typedef struct VoiceEncoder VoiceEncoder;
typedef struct VoiceDecoder VoiceDecoder;

VoiceEncoder *voiceEncoderCreate(int bitrate, int complexity);
void voiceEncoderDestroy(VoiceEncoder *enc);
/* pcm holds VOICE_FRAME_SAMPLES mono S16 samples.
 * Returns bytes written to out, or a negative value on failure. */
int voiceEncoderEncode(VoiceEncoder *enc, const int16_t *pcm,
                       uint8_t *out, int outCap);

VoiceDecoder *voiceDecoderCreate(void);
void voiceDecoderDestroy(VoiceDecoder *dec);
/* data == NULL or len == 0 runs packet-loss concealment.
 * Writes VOICE_FRAME_SAMPLES mono S16 samples to pcm.
 * Returns samples decoded, or a negative value on failure. */
int voiceDecoderDecode(VoiceDecoder *dec, const uint8_t *data, int len,
                       int16_t *pcm);

/* Flags byte carried with each frame. The wire framing in
 * src/bolo/internal/voice_segment.h defines the same bit; that header is a
 * sim internal and is not visible to the client frontends, so the value is
 * stated on both sides of the boundary. tests/unit/test_voice_segment.c
 * sees both headers and holds the two definitions to the same value. */
#define VOICE_FLAG_END_OF_UTTERANCE 0x01

#define VOICE_JITTER_SLOTS   8
#define VOICE_JITTER_TARGET  3   /* frames buffered before playback starts.
                                  * 60 ms of cushion, chosen against a sender
                                  * measured at 18 ms between frames in the
                                  * median and 37 ms at the 99th percentile,
                                  * so ordinary jitter never reaches the
                                  * deadline below                          */
#define VOICE_JITTER_LATE_MS 60  /* three frame times.  How long the speaker
                                  * waits for a frame that has not arrived
                                  * before giving up on it and concealing:
                                  * long enough to cover the sender's own
                                  * emission jitter, short enough that a
                                  * frame which really was lost does not
                                  * stall the talker audibly.  The wait is
                                  * added to that talker's delay and is never
                                  * given back, so a run of late frames walks
                                  * the delay up.  It stops at the buffer's
                                  * VOICE_JITTER_SLOTS frames, about 160 ms,
                                  * where voiceSpeakerPush starts dropping
                                  * the oldest and counts it in evicted     */
#define VOICE_JITTER_MAX_PLC 5   /* 100 ms of concealment with nothing
                                  * arriving ends the utterance            */

typedef struct VoiceSpeaker VoiceSpeaker;

VoiceSpeaker *voiceSpeakerCreate(void);
void voiceSpeakerDestroy(VoiceSpeaker *sp);

/* Queue an arriving frame. Copies the payload. */
void voiceSpeakerPush(VoiceSpeaker *sp, uint8_t seq, uint8_t flags,
                      const uint8_t *opus, int opusLen);

/* Produce the next 20 ms. Returns true and fills pcm with
 * VOICE_FRAME_SAMPLES mono S16 when audio was produced (decoded or
 * concealed).
 *
 * False means one of two things: this speaker is not playing, or the frame
 * due next has not arrived and is not yet overdue by VOICE_JITTER_LATE_MS.
 * The caller does not have to tell them apart — it tries again on its next
 * call either way, and the frame plays if it turns up in the meantime.
 *
 * nowMs is a free-running millisecond clock. Only differences between
 * readings are used, so it may wrap. */
bool voiceSpeakerPop(VoiceSpeaker *sp, int16_t *pcm, uint32_t nowMs);

/* Cumulative counters, for measuring what an arrival pattern costs. Playback
 * does not read them: they exist so a caller can tell a decoded frame from a
 * concealed one, which voiceSpeakerPop's bool alone cannot. Counted from
 * create and never reset — un-priming does not clear them — so the totals
 * cover a whole session rather than the current utterance. */
typedef struct {
    uint32_t played;      /* frames decoded from a real packet      */
    uint32_t concealed;   /* frames synthesised by loss concealment */
    uint32_t lateDropped; /* arrived after their slot had played    */
    uint32_t evicted;     /* dropped from a full buffer unplayed    */
} VoiceSpeakerStats;

void voiceSpeakerGetStats(const VoiceSpeaker *sp, VoiceSpeakerStats *out);

/* Longest audio device name kept, terminator included. Names are free-form
 * strings from the driver, so a longer one is truncated rather than assumed
 * not to happen. */
#define VOICE_DEVICE_NAME_MAX 256

/* Index of saved in names[0..count-1], or -1 for no match, which the caller
 * reads as "use the system default". Empty or NULL saved is -1 too. Matches
 * the display name exactly; a NULL entry in names is skipped. */
int voiceDeviceResolveName(const char *saved, const char *const *names,
                           int count);

#define VOICE_PEAK_HOLD_MS 600  /* how long a new maximum stands before it
                                 * starts falling.  Capture runs at 50 frames
                                 * a second, so without a hold the loudest
                                 * frame of a syllable is on screen for 20 ms
                                 * and nobody reads it.  600 ms carries the
                                 * peak across a word and the gap after it,
                                 * and is short enough that it still answers
                                 * "how loud am I" rather than "how loud was
                                 * I a moment ago"                          */
#define VOICE_PEAK_DECAY_PER_SEC 1.2f /* how fast it falls once the hold is
                                       * over, in level units per second.
                                       * Full scale to silence takes a little
                                       * over 800 ms - slow enough to follow
                                       * by eye, quick enough that the peak
                                       * has caught up with the live level
                                       * before the next word starts      */

/* A meter reading that holds its maximum and then falls back to the live
 * level, so a transient stays readable at a glance instead of flickering past
 * at the capture rate.  Zero-initialise it; the first update sets the peak. */
typedef struct {
    float    peak;         /* 0..1, held then decaying to the live level */
    uint32_t holdSinceMs;  /* when the current peak was set             */
    uint32_t lastMs;       /* the previous update, for the decay step    */
} VoicePeak;

/* Feed one level reading and return the peak to draw.  A new maximum replaces
 * the peak and re-arms the hold; after the hold it falls linearly until it
 * reaches the live level and then follows it.  nowMs is an argument so the
 * caller owns the clock and this can be tested without one. */
float voicePeakUpdate(VoicePeak *p, float level, uint32_t nowMs);

/* Quietest level the meter shows, in dB relative to full scale.  Anything at
 * or below it draws empty.  Chosen against what this project measures at 2.1x
 * microphone gain: room noise reaches 0.0100 amplitude and the open-mic
 * threshold is 0.013, which come out at 0.11 and 0.16 of the bar - low enough
 * that a quiet room reads as very nearly empty, high enough that the threshold
 * is visibly off the bottom.  Ordinary speech, 0.03 to 0.15, lands between a
 * third and two thirds.  This is the one number to move if the meter still
 * reads low, or sits too full in a silent room. */
#define VOICE_METER_FLOOR_DB (-45.0f)

/* Map a 0..1 RMS amplitude to the 0..1 height a meter draws it at.  Speech RMS
 * sits in the bottom tenth of the amplitude range, so a bar fed the amplitude
 * directly barely moves; this spreads it over the bar in dB, the way a level
 * meter is read.  Silence and anything at or under the floor return 0, full
 * scale returns 1. */
float voiceMeterScale(float level);

/* The 0..1 RMS amplitude of one frame of mono S16 samples, clamped at 1.  A
 * NULL buffer or a count of zero or less reads as silence rather than making
 * every caller test for it first.  This is the measurement, not the height a
 * meter draws it at - voiceMeterScale above is that. */
float voiceFrameRms(const int16_t *pcm, int count);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_CORE_H */
