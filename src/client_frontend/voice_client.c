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

/* Open mic: the 0..1 frame RMS, measured after mic gain, at which the gate
 * counts what it hears as speech.  0.02 is around -34 dBFS - clear of room
 * tone, a fan or a keyboard on a typical desktop microphone, and well under
 * where someone talking at it sits. */
#define VOICE_OPEN_MIC_RMS_THRESHOLD 0.02f

/* Open mic: how many captured frames the gate stays open for after the level
 * falls back under the threshold.  Frames are 20 ms, so 15 is 300 ms - long
 * enough to carry the quiet tail of a word and the gap between two of them,
 * short enough that the microphone does not stay live after a sentence
 * ends. */
#define VOICE_OPEN_MIC_HANGOVER_FRAMES 15

/* How long after a talker's last frame they still count as talking.  Frames
 * arrive every 20 ms with gaps between words and gaps from the network, so
 * without this the indicator would strobe at 50 Hz; 250 ms smooths those gaps
 * over while still dropping within a quarter second of someone stopping. */
#define VOICE_TALKING_HANGOVER_MS 250

static bool isInitialised = false;
static VoiceEncoder *encoder = NULL;
static VoiceDecoder *decoder = NULL;
static bool loopbackOn = false;
static bool voiceEnabled = true;
static VoiceMode voiceMode = VOICE_MODE_PTT;
static bool pushToTalkHeld = false;
static float micGain = 1.0f;
static float outputVolume = 1.0f;
static float inputLevel = 0.0f;

/* Open-mic gate state.  gateOpen is what voiceIsTransmitting reports; the
 * hangover counts the frames it is held open for after the level drops. */
static bool gateOpen = false;
static int gateHangover = 0;

/* Whether the connection we are on carries this client's voice at all - it
 * has to exist, and a viewer's voice is not passed to the players.  Refreshed
 * every tick, because voiceIsTransmitting is asked by the settings dialog,
 * which has no client of its own to ask. */
static bool connectionCarriesVoice = false;

/* Previous tick's connectionCarriesVoice.  The microphone is opened on the
 * rising edge - joining a connection that carries voice is what asks for it,
 * so starting the game on its own never prompts. */
static bool wasCarryingVoice = false;

/* One remote talker per tank slot, keyed by player number.  Both the decoder
 * and the backend's playback are brought up the first time a frame arrives
 * from that player, so a quiet game costs nothing. */
static VoiceSpeaker *speakers[MAX_TANKS];

/* Players this client will not listen to.  The server is what actually
 * stops the frames; this drops whatever is already on its way, and holds
 * while the server is being told. */
static bool mutedPlayers[MAX_TANKS];

/* When each remote talker stops counting as talking, on the backend's
 * monotonic clock, and whether they have ever been stamped at all.  The
 * separate flag is what makes a never-heard player unambiguous: every
 * uint32_t is a reachable clock value, so no stamp could stand in for "not
 * talking" without eventually reading as a live one. */
static uint32_t talkingUntilMs[MAX_TANKS];
static bool talkingStamped[MAX_TANKS];

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
*NAME:          captureIsWanted
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether anything still wants the microphone: the
*  loopback test, or a mode that can put audio on the wire.
*  The master switch overrides both.
*
*  A mode that can transmit holds the recording device open
*  even between words, so the level meter keeps reading and
*  the first syllable after a push-to-talk press is not lost
*  to the device starting up.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static bool captureIsWanted(void) {
    if (!voiceEnabled) {
        return false;
    }
    return loopbackOn || voiceMode != VOICE_MODE_OFF;
}

/*********************************************************
*NAME:          stopCaptureIfIdle
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Pauses the microphone once neither the loopback test nor
*  a transmitting mode wants it any more.  They share one
*  recording stream, so none of them may stop it on its own.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static void stopCaptureIfIdle(void) {
    if (captureIsWanted()) {
        return;
    }
    /* Drops what both sides still hold, or re-enabling would open with a
     * burst of audio recorded before it was switched off. */
    voiceBackendCaptureStop();
    inputLevel = 0.0f;
    gateOpen = false;
    gateHangover = 0;
}

/*********************************************************
*NAME:          startCaptureIfWanted
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens the recording device if something now wants it.
*  The backend call is idempotent and only prompts for the
*  microphone the first time, so this is safe to call on
*  every state change.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static void startCaptureIfWanted(void) {
    if (!isInitialised || !captureIsWanted()) {
        return;
    }
    voiceBackendCaptureStart();
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
    voiceEnabled = true;
    voiceMode = VOICE_MODE_PTT;
    pushToTalkHeld = false;
    gateOpen = false;
    gateHangover = 0;
    micGain = 1.0f;
    outputVolume = 1.0f;
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
*NAME:          voiceForgetPlayer
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Drops everything held about one player: their decoder and
*  playback, their talking indicator, and the local mute on
*  them.
*
*  The mute goes with them because slots are recycled.  A
*  mute is on the player who was in the slot, not on the
*  slot: leaving it set would silence whoever joins into it
*  next - voice and, through the server's copy of the same
*  bit, chat as well - with nothing to show for it but a
*  "muted by you" icon on a player this client never muted.
*  The server drops its half in serverDisconnectClient.
*
*ARGUMENTS:
*  player - the player number to forget
*********************************************************/
void voiceForgetPlayer(int player) {
    if (player < 0 || player >= MAX_TANKS) {
        return;
    }
    releaseSpeaker(player);
    mutedPlayers[player] = false;
    talkingUntilMs[player] = 0;
    talkingStamped[player] = false;
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
        voiceForgetPlayer(i);
    }
    /* Forget what the last server was told, so the next connection is sent
     * this client's mic status rather than inheriting a match against a
     * server that never heard it. */
    reportedState = false;
    reportedHasMic = false;
    reportedSelfMuted = false;
    connectionCarriesVoice = false;
    wasCarryingVoice = false;
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
        /* The master switch outranks the test - it is what decides whether
         * the microphone runs at all. */
        if (!voiceEnabled || !voiceBackendCaptureStart()) {
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
*NAME:          voiceSetEnabled
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  The master switch.  Off means no capture, nothing sent
*  and nothing played, whatever the mode and the loopback
*  test are set to.  The recording device is paused rather
*  than closed, so switching back on does not ask for the
*  microphone a second time.
*
*ARGUMENTS:
*  on - true to allow voice, false to shut it all off
*********************************************************/
void voiceSetEnabled(bool on) {
    int i;

    if (on == voiceEnabled) {
        return;
    }
    voiceEnabled = on;

    if (on) {
        startCaptureIfWanted();
        return;
    }

    /* Nothing may be latched across the off state: a key still held, or a
     * gate still open, would put audio on the wire the moment voice came
     * back on. */
    pushToTalkHeld = false;
    gateOpen = false;
    gateHangover = 0;
    voiceBackendLoopbackClear();
    stopCaptureIfIdle();

    /* Drop every remote talker along with what they had buffered, so the
     * ones mid-sentence stop where they are rather than finishing.  The
     * talking stamps go with them: nothing of theirs is audible any more,
     * so nothing of theirs may still be shown as talking. */
    for (i = 0; i < MAX_TANKS; i++) {
        releaseSpeaker(i);
        talkingUntilMs[i] = 0;
        talkingStamped[i] = false;
    }
}

/*********************************************************
*NAME:          voiceIsEnabled
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether the master switch is on.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceIsEnabled(void) {
    return voiceEnabled;
}

/*********************************************************
*NAME:          voiceSetMode
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Chooses how captured audio reaches the other players.
*  Opens the recording device if the new mode can transmit
*  and this is the first ask, and pauses it once nothing
*  wants it any more.
*
*ARGUMENTS:
*  mode - one of the VoiceMode values
*********************************************************/
void voiceSetMode(VoiceMode mode) {
    if (mode != VOICE_MODE_OFF && mode != VOICE_MODE_PTT &&
        mode != VOICE_MODE_OPEN) {
        return;
    }
    if (mode == voiceMode) {
        return;
    }
    voiceMode = mode;

    /* Whatever the old mode had latched belongs to the old mode - a key held
     * through the change, or a gate still inside its hangover, must not carry
     * into the new one. */
    pushToTalkHeld = false;
    gateOpen = false;
    gateHangover = 0;

    startCaptureIfWanted();
    stopCaptureIfIdle();
}

/*********************************************************
*NAME:          voiceGetMode
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns how captured audio reaches the other players.
*
*ARGUMENTS:
*  (none)
*********************************************************/
VoiceMode voiceGetMode(void) {
    return voiceMode;
}

/*********************************************************
*NAME:          voiceSetPushToTalkHeld
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Records whether the push-to-talk key is held.  The input
*  layer says so once per poll; it is also what decides that
*  a key held while a dialog owns the keyboard, or while the
*  window has no focus, does not count as held.
*
*ARGUMENTS:
*  held - true while the key is down
*********************************************************/
void voiceSetPushToTalkHeld(bool held) {
    pushToTalkHeld = held;
}

/*********************************************************
*NAME:          voiceIsTransmitting
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether captured audio is going out right now.
*  The settings dialog lights its indicator from this, and
*  voiceTick uses it to decide whether to send the frame it
*  just encoded, so the light and the wire cannot disagree.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceIsTransmitting(void) {
    if (!isInitialised || !voiceEnabled || !connectionCarriesVoice) {
        return false;
    }
    switch (voiceMode) {
    case VOICE_MODE_PTT:
        return pushToTalkHeld;
    case VOICE_MODE_OPEN:
        return gateOpen;
    default:
        return false;
    }
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
*NAME:          voiceSetOutputVolume
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Sets the gain applied to decoded remote audio before it
*  is handed to the playback device.
*
*ARGUMENTS:
*  gain - 1.0f is unity
*********************************************************/
void voiceSetOutputVolume(float gain) {
    if (gain < 0.0f) {
        gain = 0.0f;
    }
    outputVolume = gain;
}

/*********************************************************
*NAME:          voiceGetOutputVolume
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the gain applied to decoded remote audio.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceGetOutputVolume(void) {
    return outputVolume;
}

/*********************************************************
*NAME:          applyOutputVolume
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Scales one decoded frame by the voice output volume, in
*  place.  Done here rather than in the backend so the
*  device contract stays the same on every platform.
*
*ARGUMENTS:
*  pcm - VOICE_FRAME_SAMPLES mono S16 samples, scaled in
*        place
*********************************************************/
static void applyOutputVolume(int16_t *pcm) {
    int i;
    float sample;

    /* Unity is the common case and every sample would survive it
     * unchanged. */
    if (outputVolume == 1.0f) {
        return;
    }

    for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        sample = (float)pcm[i] * outputVolume;
        if (sample > 32767.0f) {
            sample = 32767.0f;
        } else if (sample < -32768.0f) {
            sample = -32768.0f;
        }
        pcm[i] = (int16_t)sample;
    }
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
        /* Their last frames stop being played the moment they are muted, so
         * the indicator has to stop with them rather than run out whatever
         * was left of the hangover. */
        talkingUntilMs[player] = 0;
        talkingStamped[player] = false;
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
*NAME:          voiceGetTalkingMap
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the players heard from in the last
*  VOICE_TALKING_HANGOVER_MS, bit N set for player N.
*
*  Evaluated against the clock on each call rather than
*  counted down by one, so it decays properly however often
*  it is asked - a caller that goes away for a second and
*  comes back sees a talker who stopped meanwhile as
*  stopped, not as still going.
*
*ARGUMENTS:
*  (none)
*********************************************************/
PlayerBitMap voiceGetTalkingMap(void) {
    PlayerBitMap talking = 0;
    uint32_t now;
    int i;

    now = voiceBackendNowMs();
    for (i = 0; i < MAX_TANKS; i++) {
        if (!talkingStamped[i]) {
            continue;
        }
        /* Signed difference: the clock wraps every 49 days or so, and now <
         * until would read a wrapped stamp as one far in the future. */
        if ((int32_t)(now - talkingUntilMs[i]) >= 0) {
            /* Expired.  Dropped here rather than left to sit, so a stamp
             * cannot come back round as live a wrap later. */
            talkingStamped[i] = false;
            continue;
        }
        talking |= (PlayerBitMap)1u << i;
    }

    return talking;
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
        /* Voice switched off locally still has to be drained off the
         * transport - it just goes nowhere. */
        if (!voiceEnabled) {
            continue;
        }
        /* A frame from a muted player is dropped rather than buffered, so
         * nothing of theirs is waiting to be played if they are unmuted. */
        if (voiceIsPlayerMuted((int)fromPlayer)) {
            continue;
        }
        if (!ensureSpeaker((int)fromPlayer)) {
            continue;
        }
        voiceSpeakerPush(speakers[fromPlayer], seq, flags, packet, len);
        /* Stamped here, below the mute check, so a muted player's dropped
         * frames never light them up as talking. */
        talkingUntilMs[fromPlayer] =
            voiceBackendNowMs() + VOICE_TALKING_HANGOVER_MS;
        talkingStamped[fromPlayer] = true;
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
            applyOutputVolume(pcm);
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
*  microphone that cannot reach the wire, so the two never
*  both read true.
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
    /* Self muted is having a microphone that cannot reach the wire: voice
     * switched off, or the mode set to Off.  A push-to-talk player between
     * presses is not muted - they can talk whenever they choose to. */
    selfMuted = hasMic && !(voiceEnabled && voiceMode != VOICE_MODE_OFF);

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

    /* The connection has to be one that carries voice at all - the local
     * transport single-player attaches goes nowhere - and a viewer captures
     * for the loopback test like anyone else, but its voice is not carried
     * to the players, so there is nothing to send.  Kept here rather than
     * asked for inside voiceIsTransmitting, which the settings dialog calls
     * with no client of its own. */
    connectionCarriesVoice =
        clientSimNetHasVoiceTransport(cs) && !clientSimIsSpectator(cs);

    /* Joining a connection that carries voice is what opens the microphone;
     * the start is idempotent, so only the edge matters. */
    if (connectionCarriesVoice && !wasCarryingVoice) {
        startCaptureIfWanted();
    }
    wasCarryingVoice = connectionCarriesVoice;

    /* Nothing accumulates while the microphone is unwanted - the recording
     * device is paused, so there is no backlog to drain here. */
    if (!captureIsWanted()) {
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

        /* Open mic runs off the level the meter already has: over the
         * threshold opens the gate and re-arms the hangover, under it counts
         * the hangover down so the tail of a word is not cut off. */
        if (voiceMode == VOICE_MODE_OPEN) {
            if (inputLevel >= VOICE_OPEN_MIC_RMS_THRESHOLD) {
                gateOpen = true;
                gateHangover = VOICE_OPEN_MIC_HANGOVER_FRAMES;
            } else if (gateHangover > 0) {
                gateHangover--;
                if (gateHangover == 0) {
                    gateOpen = false;
                }
            } else {
                gateOpen = false;
            }
        }

        sending = voiceIsTransmitting();

        /* Between words in push-to-talk, and with the loopback test off,
         * the frame is only worth its level reading - which is already
         * taken.  Encoding it would be work nobody consumes. */
        if (!sending && !loopbackOn) {
            continue;
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
