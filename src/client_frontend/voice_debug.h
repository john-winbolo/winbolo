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
 * Name:          Voice Debug
 * Filename:      voice_debug.h
 * Purpose:
 *   Records what the capture chain actually holds at each
 *   stage, to one WAV per stage, plus two CSVs of the
 *   numbers that do not survive as audio. Built only under
 *   WB_VOICEDEBUG; without that option this header is empty
 *   and voice_client.c gains nothing.
 *
 *   It also replaces the microphone with a WAV file, so the
 *   same signal can be pushed through the chain before and
 *   after a change and the two recordings compared.
 *
 *   Main thread only, like the rest of the voice client
 *   (voice_client.c:31, :1421), so there is no locking
 *   anywhere in here.
 *
 *   The WAV length fields are patched when the file is
 *   closed, and every normal exit reaches that: the recorder
 *   hands its own stop to atexit when it starts, so a run
 *   that ends on one of main's early returns is patched too.
 *   A process killed outright runs neither, leaving both
 *   fields at zero, and the two sizes then have to be
 *   patched by hand from the length of the file: the RIFF
 *   size at offset 4 is the file length minus 8, and the
 *   data size at offset 40 is the file length minus 44.
 *
 *   The millisecond column is wall clock. An injected run is
 *   not paced to real time and runs faster than the audio it
 *   is reading, so the frame index is the audio time and the
 *   milliseconds are not.
 *
 *   The bandwidth column is the audio bandwidth Opus chose
 *   for that frame, in kHz: 4 narrowband, 6 mediumband,
 *   8 wideband, 12 superwideband, 20 fullband. 0 is a frame
 *   that was never encoded, or one the packet could not be
 *   read from. Nothing asks the encoder for a bandwidth, so
 *   this is what it picked from the bitrate.
 *
 *   The selfMuted and carriesVoice columns say why sending
 *   is 0 when it is: the player muted their own microphone,
 *   or the connection this client is on does not carry voice
 *   at all. Without them a run that never sends a frame
 *   reads the same either way, and the two want different
 *   fixes.
 *
 *   Three level columns, deliberately different numbers.
 *   inputLevel is the microphone after mic gain, which is
 *   what the meter shows the player. rmsPostAec is that with
 *   the echo taken out and the automatic gain not yet on,
 *   which is what the open-mic threshold is compared against.
 *   rmsPostGain is the frame the encoder is handed, after the
 *   gain has driven it towards its target - so a quiet room
 *   reads low in the middle column and high in the last one,
 *   and comparing the two is how the threshold is measured.
 *********************************************************/

#ifndef VOICE_DEBUG_H
#define VOICE_DEBUG_H

#if defined(WB_VOICEDEBUG)

#include <stdbool.h>
#include <stdint.h>

#include "voice_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_TAP_RAW = 0,      /* straight off the backend, before mic gain  */
    VOICE_TAP_GAINED,       /* after mic gain, what the meter reads       */
    VOICE_TAP_CLEANED,      /* after voiceAecProcess                      */
    VOICE_TAP_ROUNDTRIP,    /* encoded then decoded back - the far end    */
    VOICE_TAP_REMOTE,       /* one remote talker, after the jitter buffer */
    VOICE_TAP_COUNT
} VoiceTap;

/* Opens every WAV and both CSVs under dir, which must already exist.
 * Returns false and records nothing if any of them cannot be opened. */
bool voiceDebugStart(const char *dir);
/* Patches the WAV lengths, closes everything, prints the summary line.
 * Safe to call when not recording, and safe to call twice. */
void voiceDebugStop(void);
bool voiceDebugIsRecording(void);

/* The directory --voice-record named, or NULL when the recorder was never
 * started. The stats writers share it so one run leaves one folder to
 * collect rather than scattering half its output into the working
 * directory. */
const char *voiceDebugOutDir(void);

/* player is ignored except for VOICE_TAP_REMOTE, where it selects the file.
 * pcm is VOICE_FRAME_SAMPLES mono S16. */
void voiceDebugTap(VoiceTap tap, int player, const int16_t *pcm);

/* One frames.csv row per captured frame. */
void voiceDebugFrameStats(uint32_t nowMs, float inputLevel, float rmsPostAec,
                          float rmsPostGain, int clipped, bool micOpen,
                          bool sending, int encodedLen, bool overWireLimit,
                          bool selfMuted, bool carriesVoice);

/* Decodes the frame just encoded through the recorder's own decoder and
 * writes it to VOICE_TAP_ROUNDTRIP. */
void voiceDebugRoundtrip(const uint8_t *packet, int len);

/* True at most once a second, so the caller walks its talkers only when a
 * speakers.csv row is due. */
bool voiceDebugSpeakerStatsDue(uint32_t nowMs);
void voiceDebugSpeakerRow(uint32_t nowMs, int player,
                          const VoiceSpeakerStats *stats, int queued);

/* 48 kHz mono S16 WAV only; any other format is refused with a message. */
bool voiceDebugInjectOpen(const char *wavPath);
bool voiceDebugInjectIsOpen(void);
/* Fills pcm with the next VOICE_FRAME_SAMPLES, zero-padding a short final
 * frame. False at end of file, at which point the recorder has stopped. */
bool voiceDebugInjectRead(int16_t *pcm);

#ifdef __cplusplus
}
#endif

#endif /* WB_VOICEDEBUG */

#endif /* VOICE_DEBUG_H */
