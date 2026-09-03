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
*Name:          Voice
*Filename:      voice.c
*Author:        John Morrison
*Purpose:
*  The SDL3 backend for voice capture and playback: the
*  device half of voice_backend.h.  The voice runtime that
*  drives it is shared, in
*  src/client_frontend/voice_client.c.
*
*  Runs its own 48 kHz mono streams rather than sharing the
*  22.05 kHz effects mixer in sound.c, because 48 kHz mono
*  is the codec's native format.
*
*  Every stream is opened with a NULL callback, so SDL owns
*  the cross-thread pull and mix internally and the only two
*  calls that cross to the audio thread -
*  SDL_GetAudioStreamData and SDL_PutAudioStreamData - are
*  themselves thread safe.  Everything here therefore runs on
*  the main thread with no lock of its own.  Each remote
*  talker gets its own playback stream for the same reason:
*  SDL mixes the bound streams, so nothing here has to.
*********************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>

#include "global.h"
#include "voice_core.h"
#include "voice_backend.h"

#define VOICE_FRAME_BYTES ((int)(VOICE_FRAME_SAMPLES * sizeof(int16_t)))

static SDL_AudioStream *captureStream = NULL;
static SDL_AudioStream *playbackStream = NULL;

/* One output stream per tank slot, opened the first time a frame arrives
 * from that player, so a quiet game costs nothing. */
static SDL_AudioStream *speakerStreams[MAX_TANKS];

/*********************************************************
*NAME:          voiceOpenChatDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens one voice device with its stream role declared as
*  game chat, and hands back the stream or NULL.
*
*  The role tells the OS this is a voice call, which on the
*  backends that implement it - AAudio, WASAPI and PipeWire -
*  is what lets the platform apply its own echo
*  cancellation, gain control and noise suppression.
*  CoreAudio ignores it, so this buys macOS nothing.
*  "GameChat" rather than "Communications": SDL treats the
*  two alike except that "GameChat" does not attenuate other
*  audio streams, and a voice call must not duck the game.
*
*  The hint is global to the process and sound.c opens the
*  effects device through the same SDL, so the previous
*  value is put back before returning - including back to
*  unset - and the open is wrapped rather than the hint left
*  standing.
*
*ARGUMENTS:
*  device - the device to open
*  spec   - the format to open it in
*********************************************************/
static SDL_AudioStream *voiceOpenChatDevice(SDL_AudioDeviceID device,
                                            const SDL_AudioSpec *spec) {
    SDL_AudioStream *stream;
    const char *previous = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE);
    /* Copied because SDL_GetHint hands back a string it owns, and only its
     * internals say how long that lives. */
    char *saved = previous ? SDL_strdup(previous) : NULL;

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "GameChat");
    stream = SDL_OpenAudioDeviceStream(device, spec, NULL, NULL);

    if (saved) {
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, saved);
        SDL_free(saved);
    } else {
        SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE);
    }

    return stream;
}

/*********************************************************
*NAME:          voiceBackendInit
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens the playback stream.  No recording device is opened
*  here: voiceBackendCaptureStart does that on first use, so
*  launching the game never prompts for the microphone.
*  Returns whether voice is usable.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendInit(void) {
    SDL_AudioSpec spec;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "Voice error: init audio: %s\n", SDL_GetError());
        fflush(stderr);
        return false;
    }

    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = VOICE_SAMPLE_RATE;

    playbackStream =
        voiceOpenChatDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!playbackStream) {
        fprintf(stderr, "Voice error: open playback device: %s\n",
                SDL_GetError());
        fflush(stderr);
        voiceBackendShutdown();
        return false;
    }

    /* The playback device runs from here on - it only ever sees data we
     * push. */
    SDL_ResumeAudioStreamDevice(playbackStream);

    return true;
}

/*********************************************************
*NAME:          voiceBackendShutdown
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Destroys the recording and playback streams.  Safe to
*  call when init failed or never ran.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendShutdown(void) {
    if (captureStream) {
        SDL_DestroyAudioStream(captureStream);
        captureStream = NULL;
    }
    if (playbackStream) {
        SDL_DestroyAudioStream(playbackStream);
        playbackStream = NULL;
    }
}

/*********************************************************
*NAME:          voiceBackendCaptureStart
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens the recording stream the first time the microphone
*  is asked for, starts it, and reports whether one is
*  available.
*
*  Opening a recording device is what makes the OS ask the
*  user for microphone permission, so it is deferred until
*  something actually wants to capture - a player who never
*  turns voice on is never prompted.  The stream is kept for
*  the rest of the process once open, so turning the
*  microphone off and on again does not re-prompt.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureStart(void) {
    SDL_AudioSpec spec;

    if (!captureStream) {
        SDL_zero(spec);
        spec.format = SDL_AUDIO_S16;
        spec.channels = 1;
        spec.freq = VOICE_SAMPLE_RATE;

        captureStream =
            voiceOpenChatDevice(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec);
        if (!captureStream) {
            fprintf(stderr, "Voice error: open recording device: %s\n",
                    SDL_GetError());
            fflush(stderr);
            return false;
        }
    }

    SDL_ResumeAudioStreamDevice(captureStream);
    return true;
}

/*********************************************************
*NAME:          voiceBackendCaptureStop
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Pauses the microphone and drops what both sides still
*  hold, or re-enabling would open with a burst of audio
*  recorded before it was switched off.  The stream itself
*  is kept, because re-opening it would re-prompt.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendCaptureStop(void) {
    if (captureStream) {
        SDL_PauseAudioStreamDevice(captureStream);
        SDL_ClearAudioStream(captureStream);
    }
}

/*********************************************************
*NAME:          voiceBackendCaptureIsOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether a recording device has been opened.  It
*  stays open across a stop and start, so this is whether a
*  microphone was ever had, not whether one is capturing
*  now.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureIsOpen(void) {
    return captureStream != NULL;
}

/*********************************************************
*NAME:          voiceBackendOsCancelsEcho
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Whether the platform cancels echo on the capture device
*  itself, which is true of exactly the drivers that act on
*  the game chat role voiceOpenChatDevice declares.  Three of
*  SDL's audio drivers do; every other one, CoreAudio among
*  them, ignores the role and leaves the work to Speex.
*
*  The live driver is asked for by name rather than inferred
*  from the platform, because more than one driver is built
*  for most platforms and which of them comes up is decided
*  at runtime - a Linux build carries PipeWire, PulseAudio
*  and ALSA, and only the first of those cancels.
*
*  SDL hands back NULL when the audio subsystem is not up.
*  That is not a driver that cancels, so it answers false.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendOsCancelsEcho(void) {
    const char *driver = SDL_GetCurrentAudioDriver();

    if (driver == NULL) {
        return false;
    }

    /* Matched case sensitively against the names the drivers register
     * themselves under.  "AAudio" really is the odd one out: that is the
     * capitalisation SDL gives Android's driver, not a slip to tidy up, and
     * lower-casing it would quietly leave Android running both cancellers. */
    return SDL_strcmp(driver, "wasapi") == 0 ||
           SDL_strcmp(driver, "pipewire") == 0 ||
           SDL_strcmp(driver, "AAudio") == 0;
}

/*********************************************************
*NAME:          voiceBackendCaptureRead
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Takes the next whole captured frame, or reports that one
*  is not ready.  Returns VOICE_FRAME_SAMPLES on success and
*  0 otherwise; a partial frame is never handed back.
*
*ARGUMENTS:
*  pcm - filled with VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
int voiceBackendCaptureRead(int16_t *pcm) {
    if (!captureStream) {
        return 0;
    }
    if (SDL_GetAudioStreamAvailable(captureStream) < VOICE_FRAME_BYTES) {
        return 0;
    }
    if (SDL_GetAudioStreamData(captureStream, pcm, VOICE_FRAME_BYTES) !=
        VOICE_FRAME_BYTES) {
        return 0;
    }
    return VOICE_FRAME_SAMPLES;
}

/*********************************************************
*NAME:          voiceBackendSpeakerOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens one remote player's output stream on the first
*  frame heard from them, and reports whether that player
*  can be played.
*
*ARGUMENTS:
*  player - the player number the frame came from
*********************************************************/
bool voiceBackendSpeakerOpen(int player) {
    SDL_AudioSpec spec;

    if (speakerStreams[player] != NULL) {
        return true;
    }

    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = VOICE_SAMPLE_RATE;

    speakerStreams[player] =
        voiceOpenChatDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (speakerStreams[player] == NULL) {
        fprintf(stderr, "Voice error: open playback device: %s\n",
                SDL_GetError());
        fflush(stderr);
        return false;
    }
    /* Bound to the device with no callback of our own, so SDL pulls and
     * mixes this talker against the others on its own thread. */
    SDL_ResumeAudioStreamDevice(speakerStreams[player]);

    return true;
}

/*********************************************************
*NAME:          voiceBackendSpeakerClose
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Destroys one remote player's output stream.  Anything
*  they had queued goes with it.
*
*ARGUMENTS:
*  player - the player number to release
*********************************************************/
void voiceBackendSpeakerClose(int player) {
    if (speakerStreams[player]) {
        SDL_DestroyAudioStream(speakerStreams[player]);
        speakerStreams[player] = NULL;
    }
}

/*********************************************************
*NAME:          voiceBackendSpeakerQueuedFrames
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the whole frames one remote player still has
*  waiting to be played.
*
*  SDL_GetAudioStreamQueued counts the bytes as they were
*  put in, and nothing here ever changes a stream's input
*  format, so those are 20 ms frames of S16 mono at the
*  codec's rate.
*
*ARGUMENTS:
*  player - the player number to ask about
*********************************************************/
int voiceBackendSpeakerQueuedFrames(int player) {
    if (speakerStreams[player] == NULL) {
        return 0;
    }
    return (int)(SDL_GetAudioStreamQueued(speakerStreams[player]) /
                 VOICE_FRAME_BYTES);
}

/*********************************************************
*NAME:          voiceBackendSpeakerPlay
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Queues one frame for a remote player.
*
*ARGUMENTS:
*  player - the player number the frame came from
*  pcm    - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendSpeakerPlay(int player, const int16_t *pcm) {
    if (speakerStreams[player]) {
        SDL_PutAudioStreamData(speakerStreams[player], pcm, VOICE_FRAME_BYTES);
    }
}

/*********************************************************
*NAME:          voiceBackendLoopbackQueuedFrames
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the whole frames the microphone loopback test
*  still has waiting to be played.
*
*  Counted the same way as a remote talker's: the bytes SDL
*  still holds, as they were put in, over the size of a
*  frame.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voiceBackendLoopbackQueuedFrames(void) {
    if (playbackStream == NULL) {
        return 0;
    }
    return (int)(SDL_GetAudioStreamQueued(playbackStream) / VOICE_FRAME_BYTES);
}

/*********************************************************
*NAME:          voiceBackendLoopbackPlay
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Queues one frame of the microphone loopback test.
*
*ARGUMENTS:
*  pcm - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendLoopbackPlay(const int16_t *pcm) {
    if (playbackStream) {
        SDL_PutAudioStreamData(playbackStream, pcm, VOICE_FRAME_BYTES);
    }
}

/*********************************************************
*NAME:          voiceBackendLoopbackClear
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Drops whatever the loopback test still has queued.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendLoopbackClear(void) {
    if (playbackStream) {
        SDL_ClearAudioStream(playbackStream);
    }
}

/*********************************************************
*NAME:          voiceBackendNowMs
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Monotonic milliseconds, from SDL's tick source - the
*  milliseconds since SDL was brought up, which only ever
*  moves forwards.  Truncated to 32 bits, so callers compare
*  with a signed difference.
*
*ARGUMENTS:
*  (none)
*********************************************************/
uint32_t voiceBackendNowMs(void) {
    return (uint32_t)SDL_GetTicks();
}
