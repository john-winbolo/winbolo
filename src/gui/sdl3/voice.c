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
*  System specific voice capture and playback routines
*  (Uses SDL3)
*
*  Runs its own 48 kHz mono streams rather than sharing the
*  22.05 kHz effects mixer in sound.c, because 48 kHz mono
*  is the codec's native format.
*
*  Both streams are opened with a NULL callback, so SDL owns
*  the cross-thread pull and mix internally and the only two
*  calls that cross to the audio thread -
*  SDL_GetAudioStreamData and SDL_PutAudioStreamData - are
*  themselves thread safe.  Everything here therefore runs on
*  the main thread with no lock of its own.
*********************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>

#include "voice_core.h"
#include "../voice.h"

/* Frames drained per voiceTick.  A tick that has been stalled long enough to
 * bank more than this leaves the excess queued for the ticks that follow,
 * rather than working through an arbitrarily deep backlog in one call. */
#define VOICE_FRAMES_PER_TICK 4

#define VOICE_FRAME_BYTES ((int)(VOICE_FRAME_SAMPLES * sizeof(int16_t)))

static bool isInitialised = false;
static SDL_AudioStream *captureStream = NULL;
static SDL_AudioStream *playbackStream = NULL;
static VoiceEncoder *encoder = NULL;
static VoiceDecoder *decoder = NULL;
static bool loopbackOn = false;
static float micGain = 1.0f;
static float inputLevel = 0.0f;

/*********************************************************
*NAME:          ensureCaptureStream
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens the recording stream the first time the microphone
*  is asked for, and reports whether one is available.
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
static bool ensureCaptureStream(void) {
    SDL_AudioSpec spec;

    if (captureStream) {
        return true;
    }

    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = VOICE_SAMPLE_RATE;

    captureStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING,
                                              &spec, NULL, NULL);
    if (!captureStream) {
        fprintf(stderr, "Voice error: open recording device: %s\n",
                SDL_GetError());
        fflush(stderr);
        return false;
    }

    return true;
}

/*********************************************************
*NAME:          voiceInit
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens the playback stream and creates the codec.  The
*  recording stream is opened later, the first time the
*  microphone is wanted.  Returns whether voice is usable.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceInit(void) {
    SDL_AudioSpec spec;

    if (isInitialised) {
        return true;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "Voice error: init audio: %s\n", SDL_GetError());
        fflush(stderr);
        return false;
    }

    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = VOICE_SAMPLE_RATE;

    playbackStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                               &spec, NULL, NULL);
    if (!playbackStream) {
        fprintf(stderr, "Voice error: open playback device: %s\n",
                SDL_GetError());
        fflush(stderr);
        voiceCleanup();
        return false;
    }

    encoder = voiceEncoderCreate(VOICE_DEFAULT_BITRATE, VOICE_DEFAULT_COMPLEXITY);
    decoder = voiceDecoderCreate();
    if (!encoder || !decoder) {
        fprintf(stderr, "Voice error: create codec\n");
        fflush(stderr);
        voiceCleanup();
        return false;
    }

    /* The playback device runs from here on - it only ever sees data we
     * push.  No recording device is opened yet; ensureCaptureStream does
     * that on first use so launching the game never prompts for the
     * microphone. */
    SDL_ResumeAudioStreamDevice(playbackStream);

    isInitialised = true;
    return true;
}

/*********************************************************
*NAME:          voiceCleanup
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Destroys the audio streams and the codec.  Safe to call
*  when init failed or never ran.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceCleanup(void) {
    if (captureStream) {
        SDL_DestroyAudioStream(captureStream);
        captureStream = NULL;
    }
    if (playbackStream) {
        SDL_DestroyAudioStream(playbackStream);
        playbackStream = NULL;
    }
    voiceEncoderDestroy(encoder);
    encoder = NULL;
    voiceDecoderDestroy(decoder);
    decoder = NULL;
    loopbackOn = false;
    micGain = 1.0f;
    inputLevel = 0.0f;
    isInitialised = false;
}

/*********************************************************
*NAME:          voiceLoopbackSetEnabled
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Starts or stops the microphone loopback test.  Turning it
*  on opens the recording device if this is the first ask,
*  and stays off if there is no device to open or the user
*  refuses the microphone.
*
*ARGUMENTS:
*  on - true to start capturing, false to stop
*********************************************************/
void voiceLoopbackSetEnabled(bool on) {
    if (!isInitialised || on == loopbackOn) {
        return;
    }

    if (on) {
        if (!ensureCaptureStream()) {
            return;
        }
        loopbackOn = true;
        SDL_ResumeAudioStreamDevice(captureStream);
    } else {
        loopbackOn = false;
        if (captureStream) {
            SDL_PauseAudioStreamDevice(captureStream);
            /* Drop what both sides still hold, or re-enabling would open
             * with a burst of audio recorded before the test was switched
             * off. */
            SDL_ClearAudioStream(captureStream);
        }
        SDL_ClearAudioStream(playbackStream);
        inputLevel = 0.0f;
    }
}

/*********************************************************
*NAME:          voiceLoopbackIsEnabled
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether the loopback test is running.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceLoopbackIsEnabled(void) {
    return loopbackOn;
}

/*********************************************************
*NAME:          voiceSetMicGain
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Sets the gain applied to captured audio.
*
*ARGUMENTS:
*  gain - 1.0f is unity
*********************************************************/
void voiceSetMicGain(float gain) {
    if (gain < 0.0f) {
        gain = 0.0f;
    }
    micGain = gain;
}

/*********************************************************
*NAME:          voiceGetMicGain
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the gain applied to captured audio.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceGetMicGain(void) {
    return micGain;
}

/*********************************************************
*NAME:          voiceGetInputLevel
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the 0..1 RMS of the most recent captured frame.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceGetInputLevel(void) {
    return inputLevel;
}

/*********************************************************
*NAME:          voiceTick
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Drains whole captured frames, applies mic gain, and runs
*  each one through the encoder and decoder back out to the
*  speakers.  Main thread only.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceTick(void) {
    int16_t pcm[VOICE_FRAME_SAMPLES];
    int16_t decodedPcm[VOICE_FRAME_SAMPLES];
    uint8_t packet[VOICE_MAX_PACKET];
    int frame;
    int i;
    int got;
    int encodedLen;
    int decodedSamples;
    float sample;
    float sumSquares;

    /* Nothing accumulates while the test is off - the recording device is
     * paused, so there is no backlog to drain here. */
    if (!isInitialised || !loopbackOn) {
        return;
    }

    for (frame = 0; frame < VOICE_FRAMES_PER_TICK; frame++) {
        if (SDL_GetAudioStreamAvailable(captureStream) < VOICE_FRAME_BYTES) {
            break;
        }
        got = SDL_GetAudioStreamData(captureStream, pcm, VOICE_FRAME_BYTES);
        if (got != VOICE_FRAME_BYTES) {
            break;
        }

        sumSquares = 0.0f;
        for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
            sample = (float)pcm[i] * micGain;
            if (sample > 32767.0f) {
                sample = 32767.0f;
            } else if (sample < -32768.0f) {
                sample = -32768.0f;
            }
            pcm[i] = (int16_t)sample;
            sumSquares += sample * sample;
        }
        inputLevel = SDL_sqrtf(sumSquares / (float)VOICE_FRAME_SAMPLES) / 32768.0f;
        if (inputLevel > 1.0f) {
            inputLevel = 1.0f;
        }

        encodedLen = voiceEncoderEncode(encoder, pcm, packet, (int)sizeof(packet));
        if (encodedLen <= 0) {
            continue;
        }
        decodedSamples = voiceDecoderDecode(decoder, packet, encodedLen,
                                            decodedPcm);
        if (decodedSamples != VOICE_FRAME_SAMPLES) {
            continue;
        }
        SDL_PutAudioStreamData(playbackStream, decodedPcm, VOICE_FRAME_BYTES);
    }
}
