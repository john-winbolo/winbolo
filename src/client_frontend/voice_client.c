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
 * Name:          Voice Client
 * Filename:      voice_client.c
 * Purpose:
 *   The client voice runtime, shared by every platform:
 *   the remote talker table, the local mute list, transmit
 *   gating, the send and receive legs, the mic status
 *   report, and the pacing of playback off the audio
 *   device.
 *
 *   Nothing here touches an audio device directly. Capture
 *   and playback go through voice_backend.h, which each
 *   platform implements (src/gui/sdl3/voice.c for the
 *   desktop build), so a new platform supplies a driver
 *   rather than a second copy of this file.
 *
 *   Main thread only. A backend that runs an audio thread
 *   of its own owns that crossing.
 *********************************************************/

#include <math.h>
#include <stdio.h>

#include "global.h"
#include "client_net.h"
#include "client_sim.h"
#include "voice_core.h"
#include "voice_backend.h"
#include "voice.h"

/* Frames drained per voiceTick.  A tick that has been stalled long enough to
 * bank more than this leaves the excess queued for the ticks that follow,
 * rather than working through an arbitrarily deep backlog in one call. */
#define VOICE_FRAMES_PER_TICK 4

/* How much audio each remote talker's output stream is kept topped up to.
 * The audio device drains it at real time, so this is the depth playback is
 * refilled to whenever it is looked at - deep enough that a late call still
 * finds audio to play, shallow enough not to add audible delay of its own. */
#define VOICE_PLAYBACK_TARGET_FRAMES 2

/* Frames queued per talker per call.  A caller that has been stalled long
 * enough for a talker to bank more than this leaves the excess in the jitter
 * buffer for the calls that follow, rather than working through an
 * arbitrarily deep backlog in one go. */
#define VOICE_PLAYBACK_MAX_POPS_PER_CALL 4

static bool isInitialised = false;
static VoiceEncoder *encoder = NULL;
static VoiceDecoder *decoder = NULL;
static bool loopbackOn = false;
static bool transmitOn = false;
static float micGain = 1.0f;
static float inputLevel = 0.0f;

/* One remote talker per tank slot, keyed by player number.  Both the decoder
 * and the backend's playback are brought up the first time a frame arrives
 * from that player, so a quiet game costs nothing. */
static VoiceSpeaker *speakers[MAX_TANKS];

/* Players this client will not listen to.  The server is what actually
 * stops the frames; this drops whatever is already on its way, and holds
 * while the server is being told. */
static bool mutedPlayers[MAX_TANKS];

/* An encoder producing frames too large for one voice segment is a
 * configuration problem, not a per-frame event: say so once. */
static bool warnedFrameTooLarge = false;

/* Last mic status put on the wire, so the report only goes out on a change.
 * reportedState is cleared with the rest of the per-connection state, which
 * makes the first tick of the next connection re-report. */
static bool reportedState = false;
static bool reportedHasMic = false;
static bool reportedSelfMuted = false;

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
    /* Drops what both sides still hold, or re-enabling would open with a
     * burst of audio recorded before it was switched off. */
    voiceBackendCaptureStop();
    inputLevel = 0.0f;
}

/*********************************************************
*NAME:          voiceInit
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Brings playback up and creates the codec.  The recording
*  device is opened later, the first time the microphone is
*  wanted, so starting the game never prompts for it.
*  Returns whether voice is usable.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceInit(void) {
    if (isInitialised) {
        return true;
    }

    if (!voiceBackendInit()) {
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

    isInitialised = true;
    return true;
}

/*********************************************************
*NAME:          voiceCleanup
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Releases the audio devices and the codec.  Safe to call
*  when init failed or never ran.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceCleanup(void) {
    voiceReset();
    voiceBackendShutdown();
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
*NAME:          releaseSpeaker
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Drops one remote talker's decoder and playback.
*  Anything they had buffered goes with it, so playback
*  stops where it is rather than running to the end of what
*  had already arrived.
*
*ARGUMENTS:
*  player - the player number to release
*********************************************************/
static void releaseSpeaker(int player) {
    voiceSpeakerDestroy(speakers[player]);
    speakers[player] = NULL;
    voiceBackendSpeakerClose(player);
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
        releaseSpeaker(i);
        mutedPlayers[i] = false;
    }
    /* Forget what the last server was told, so the next connection is sent
     * this client's mic status rather than inheriting a match against a
     * server that never heard it. */
    reportedState = false;
    reportedHasMic = false;
    reportedSelfMuted = false;
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
        if (!voiceBackendCaptureStart()) {
            return;
        }
        loopbackOn = true;
    } else {
        loopbackOn = false;
        voiceBackendLoopbackClear();
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
        if (!voiceBackendCaptureStart()) {
            return;
        }
        transmitOn = true;
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
*  Brings up the decoder and playback for one remote player
*  on the first frame heard from them, and reports whether
*  that player can be played.
*
*ARGUMENTS:
*  player - the player number the frame came from
*********************************************************/
static bool ensureSpeaker(int player) {
    if (player < 0 || player >= MAX_TANKS) {
        return false;
    }

    if (speakers[player] == NULL) {
        speakers[player] = voiceSpeakerCreate();
        if (speakers[player] == NULL) {
            return false;
        }
    }

    return voiceBackendSpeakerOpen(player);
}

/*********************************************************
*NAME:          voiceSetPlayerMuted
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Stops or resumes playing one player's voice here.  The
*  server is what stops the frames being sent at all; this
*  covers the round trip while it is being told, and drops
*  what that player already has buffered so a muted talker
*  does not finish the sentence they were half way through.
*
*ARGUMENTS:
*  player - the player number to mute
*  muted  - true to mute, false to unmute
*********************************************************/
void voiceSetPlayerMuted(int player, bool muted) {
    if (player < 0 || player >= MAX_TANKS) {
        return;
    }
    mutedPlayers[player] = muted;
    if (muted) {
        releaseSpeaker(player);
    }
}

/*********************************************************
*NAME:          voiceIsPlayerMuted
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether one player is muted here.
*
*ARGUMENTS:
*  player - the player number to ask about
*********************************************************/
bool voiceIsPlayerMuted(int player) {
    if (player < 0 || player >= MAX_TANKS) {
        return false;
    }
    return mutedPlayers[player];
}

/*********************************************************
*NAME:          voicePlayRemote
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Feeds everything that arrived from other players into
*  their jitter buffers, then tops each talker's playback
*  back up to a small target depth.
*
*  Nothing guarantees this runs at the rate the frames were
*  sent at - it is driven from the render loop, and from the
*  settings dialog's own loop at a different rate again.  So
*  playback is paced by the audio device rather than by the
*  call: a talker whose playback still holds enough audio is
*  popped zero times, however often we are asked, and pops
*  only resume once the device has drained it.
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
    int pops;

    while ((len = clientSimNetReceiveVoice(cs, &fromPlayer, &seq, &flags,
                                           packet, (int)sizeof(packet))) > 0) {
        /* A frame from a muted player is dropped rather than buffered, so
         * nothing of theirs is waiting to be played if they are unmuted. */
        if (voiceIsPlayerMuted((int)fromPlayer)) {
            continue;
        }
        if (!ensureSpeaker((int)fromPlayer)) {
            continue;
        }
        voiceSpeakerPush(speakers[fromPlayer], seq, flags, packet, len);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (speakers[i] == NULL) {
            continue;
        }
        /* The queued depth is in whole 20 ms frames, so it compares directly
         * against the target. */
        for (pops = 0; pops < VOICE_PLAYBACK_MAX_POPS_PER_CALL; pops++) {
            if (voiceBackendSpeakerQueuedFrames(i) >=
                VOICE_PLAYBACK_TARGET_FRAMES) {
                break;
            }
            /* Nothing left to play - the jitter buffer is waiting on a frame
             * that has not arrived yet, and asking again will not change
             * that until the next call. */
            if (!voiceSpeakerPop(speakers[i], pcm)) {
                break;
            }
            voiceBackendSpeakerPlay(i, pcm);
        }
    }
}

/*********************************************************
*NAME:          voiceReportState
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Tells the server this client's mic status, on the tick it
*  changes.  A microphone is had once voice is up and a
*  recording device has actually opened - asking for one that
*  never came up is not having one.  Muted is having a
*  microphone and not sending from it, so the two never both
*  read true.
*
*ARGUMENTS:
*  cs - the connected client
*********************************************************/
void voiceReportState(struct ClientSim *cs) {
    bool hasMic;
    bool selfMuted;

    if (cs == NULL) {
        return;
    }

    hasMic = isInitialised && voiceBackendCaptureIsOpen();
    selfMuted = hasMic && !transmitOn;

    if (reportedState && hasMic == reportedHasMic &&
        selfMuted == reportedSelfMuted) {
        return;
    }

    clientSimNetSendVoiceState(cs, hasMic, selfMuted);
    reportedState = true;
    reportedHasMic = hasMic;
    reportedSelfMuted = selfMuted;
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
        voiceReportState(cs);
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
        got = voiceBackendCaptureRead(pcm);
        if (got != VOICE_FRAME_SAMPLES) {
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
        inputLevel = sqrtf(sumSquares / (float)VOICE_FRAME_SAMPLES) / 32768.0f;
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
            voiceBackendLoopbackPlay(decodedPcm);
        }
    }
}
