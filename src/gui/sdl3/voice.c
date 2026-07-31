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
#include "client_net.h"
#include "client_sim.h"
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
static bool transmitOn = false;
static float micGain = 1.0f;
static float inputLevel = 0.0f;

/* One remote talker per tank slot, keyed by player number.  Both halves are
 * opened the first time a frame arrives from that player, so a quiet game
 * costs nothing. */
static VoiceSpeaker *speakers[MAX_TANKS];
static SDL_AudioStream *speakerStreams[MAX_TANKS];

/* An encoder producing frames too large for one voice segment is a
 * configuration problem, not a per-frame event: say so once. */
static bool warnedFrameTooLarge = false;

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
*NAME:          stopCaptureIfIdle
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Pauses the microphone once neither the loopback test nor
*  transmission wants it any more.  The two share one
*  recording stream, so neither may stop it on its own.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static void stopCaptureIfIdle(void) {
    if (loopbackOn || transmitOn) {
        return;
    }
    if (captureStream) {
        SDL_PauseAudioStreamDevice(captureStream);
        /* Drop what both sides still hold, or re-enabling would open
         * with a burst of audio recorded before it was switched off. */
        SDL_ClearAudioStream(captureStream);
    }
    inputLevel = 0.0f;
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
    voiceReset();
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
    transmitOn = false;
    micGain = 1.0f;
    inputLevel = 0.0f;
    warnedFrameTooLarge = false;
    isInitialised = false;
}

/*********************************************************
*NAME:          voiceReset
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Forgets every remote talker.  Player numbers are reused
*  from one game to the next, so the decoders and streams
*  are released when a connection ends rather than carried
*  into the next one.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceReset(void) {
    int i;

    for (i = 0; i < MAX_TANKS; i++) {
        voiceSpeakerDestroy(speakers[i]);
        speakers[i] = NULL;
        if (speakerStreams[i]) {
            SDL_DestroyAudioStream(speakerStreams[i]);
            speakerStreams[i] = NULL;
        }
    }
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
        SDL_ClearAudioStream(playbackStream);
        stopCaptureIfIdle();
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
*NAME:          voiceTransmitSetEnabled
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Starts or stops sending captured audio to the other
*  players.  Turning it on opens the recording device if
*  this is the first ask, and stays off if there is no
*  device to open or the user refuses the microphone.
*
*ARGUMENTS:
*  on - true to start transmitting, false to stop
*********************************************************/
void voiceTransmitSetEnabled(bool on) {
    if (!isInitialised || on == transmitOn) {
        return;
    }

    if (on) {
        if (!ensureCaptureStream()) {
            return;
        }
        transmitOn = true;
        SDL_ResumeAudioStreamDevice(captureStream);
    } else {
        transmitOn = false;
        stopCaptureIfIdle();
    }
}

/*********************************************************
*NAME:          voiceTransmitIsEnabled
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether captured audio is being transmitted.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceTransmitIsEnabled(void) {
    return transmitOn;
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
*NAME:          ensureSpeaker
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Brings up the decoder and playback stream for one remote
*  player on the first frame heard from them, and reports
*  whether that player can be played.
*
*ARGUMENTS:
*  player - the player number the frame came from
*********************************************************/
static bool ensureSpeaker(int player) {
    SDL_AudioSpec spec;

    if (player < 0 || player >= MAX_TANKS) {
        return false;
    }
    if (speakers[player] != NULL && speakerStreams[player] != NULL) {
        return true;
    }

    if (speakers[player] == NULL) {
        speakers[player] = voiceSpeakerCreate();
        if (speakers[player] == NULL) {
            return false;
        }
    }

    if (speakerStreams[player] == NULL) {
        SDL_zero(spec);
        spec.format = SDL_AUDIO_S16;
        spec.channels = 1;
        spec.freq = VOICE_SAMPLE_RATE;

        speakerStreams[player] = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (speakerStreams[player] == NULL) {
            fprintf(stderr, "Voice error: open playback device: %s\n",
                    SDL_GetError());
            fflush(stderr);
            return false;
        }
        /* Bound to the device with no callback of our own, so SDL pulls and
         * mixes this talker against the others on its own thread. */
        SDL_ResumeAudioStreamDevice(speakerStreams[player]);
    }

    return true;
}

/*********************************************************
*NAME:          voicePlayRemote
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Feeds everything that arrived from other players into
*  their jitter buffers, then plays one 20 ms frame from
*  each.  One frame per talker per tick is what holds
*  playback to the rate the frames were sent at.
*
*ARGUMENTS:
*  cs - the connected client
*********************************************************/
static void voicePlayRemote(struct ClientSim *cs) {
    uint8_t packet[VOICE_MAX_PACKET];
    int16_t pcm[VOICE_FRAME_SAMPLES];
    uint8_t fromPlayer, seq, flags;
    int len;
    int i;

    while ((len = clientSimNetReceiveVoice(cs, &fromPlayer, &seq, &flags,
                                           packet, (int)sizeof(packet))) > 0) {
        if (!ensureSpeaker((int)fromPlayer)) {
            continue;
        }
        voiceSpeakerPush(speakers[fromPlayer], seq, flags, packet, len);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (speakers[i] == NULL || speakerStreams[i] == NULL) {
            continue;
        }
        if (voiceSpeakerPop(speakers[i], pcm)) {
            SDL_PutAudioStreamData(speakerStreams[i], pcm, VOICE_FRAME_BYTES);
        }
    }
}

/*********************************************************
*NAME:          voiceTick
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Drains whole captured frames, applies mic gain, and hands
*  each encoded frame to the loopback test, the server, or
*  both.  Then plays whatever the other players sent.  Main
*  thread only.
*
*ARGUMENTS:
*  cs - the connected client, or NULL when there is no
*       network (the loopback test still runs)
*********************************************************/
void voiceTick(struct ClientSim *cs) {
    int16_t pcm[VOICE_FRAME_SAMPLES];
    int16_t decodedPcm[VOICE_FRAME_SAMPLES];
    uint8_t packet[VOICE_MAX_PACKET];
    bool sending;
    int frame;
    int i;
    int got;
    int encodedLen;
    int decodedSamples;
    float sample;
    float sumSquares;

    if (!isInitialised) {
        return;
    }

    if (cs != NULL) {
        voicePlayRemote(cs);
    }

    /* A viewer captures for the loopback test like anyone else, but its
     * voice is not carried to the players, so there is nothing to send. */
    sending = transmitOn && cs != NULL && !clientSimIsSpectator(cs);

    /* Nothing accumulates while the microphone is unwanted - the recording
     * device is paused, so there is no backlog to drain here. */
    if (!loopbackOn && !transmitOn) {
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

        /* One encode feeds both consumers. */
        encodedLen = voiceEncoderEncode(encoder, pcm, packet, (int)sizeof(packet));
        if (encodedLen <= 0) {
            continue;
        }

        if (sending) {
            if (encodedLen > CLIENT_VOICE_MAX_FRAME_BYTES) {
                /* Too big for one segment.  Sending a piece of it would
                 * decode to noise, so drop the frame. */
                if (!warnedFrameTooLarge) {
                    fprintf(stderr,
                            "Voice error: %d byte frame exceeds the %d byte "
                            "limit, dropping\n",
                            encodedLen, CLIENT_VOICE_MAX_FRAME_BYTES);
                    fflush(stderr);
                    warnedFrameTooLarge = true;
                }
            } else {
                clientSimNetSendVoice(cs, packet, encodedLen);
            }
        }

        if (loopbackOn) {
            decodedSamples = voiceDecoderDecode(decoder, packet, encodedLen,
                                                decodedPcm);
            if (decodedSamples != VOICE_FRAME_SAMPLES) {
                continue;
            }
            SDL_PutAudioStreamData(playbackStream, decodedPcm,
                                   VOICE_FRAME_BYTES);
        }
    }
}
