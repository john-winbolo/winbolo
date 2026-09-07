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
#define VOICE_JITTER_TARGET  2   /* frames buffered before playback starts */
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
 * concealed); false when this speaker has nothing to play. */
bool voiceSpeakerPop(VoiceSpeaker *sp, int16_t *pcm);

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

#ifdef __cplusplus
}
#endif

#endif /* VOICE_CORE_H */
