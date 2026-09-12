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
#include <string.h>

#include "global.h"
#include "client_net.h"
#include "client_sim.h"
#include "voice_core.h"
#include "voice_backend.h"
#include "voice.h"
#if defined(WINBOLO_VOICE_AEC)
#include "voice_aec.h"
#endif
#if defined(WB_VOICEDEBUG)
#include "voice_debug.h"
#endif

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

/* Open mic: the 0..1 frame RMS at which the open-mic decision counts what it
 * hears as speech.  0.013, about -38 dBFS, was measured rather than picked:
 * onsets were being lost at 0.0131, and room noise in the same recording
 * topped out at 0.0100 across 431 quiet frames.
 *
 * The signal it applies to is the microphone after mic gain and after the
 * echo canceller, and before the preprocessor's automatic gain - which is
 * what voiceAecProcess hands back, and is the signal the numbers above were
 * measured on.  Not the frame that comes out of it: the automatic gain aims
 * every frame at a fixed target, so downstream of it a quiet room and a
 * talker read alike and no absolute number tells them apart.
 *
 * It costs open-microphone time.  At 0.02 the microphone was open for 34%
 * of that recording; at 0.013 it is open for 54%, because every low-level
 * frame that clears the threshold also re-arms the hangover below.
 * Shortening that hangover to claw the time back was measured and
 * rejected: it trims the open time only by opening more often - eight
 * separate openings instead of three - and each opening is one more
 * utterance boundary. */
#define VOICE_OPEN_MIC_RMS_THRESHOLD 0.013f

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

/* voiceTick calls between attempts to open the recording device while the
 * connection carries voice and the microphone is wanted but has not come up.
 * The first attempt is made on the tick the connection starts carrying voice,
 * and on a desktop that is the very moment the operating system puts its
 * microphone permission prompt up, so the open fails while the player is
 * still reading it.  Nothing tells this code when they click Allow, so it
 * asks again this many calls later.  Calls track the rate the loop calling
 * them renders at rather than the wire cadence, so what that works out to in
 * real time varies - roughly 0.4 s at 120 fps and 1.7 s at 30 - which is
 * prompt enough for the player either way, and never often enough to trouble
 * the audio subsystem when there is no microphone at all. */
#define VOICE_CAPTURE_RETRY_TICKS 50

/* How long the microphone test records for.  Frames are 20 ms, so 150 of them
 * is three seconds - long enough to say a whole sentence and hear it come
 * back, short enough that nobody is left waiting on it. */
#define VOICE_MICTEST_FRAMES 150

/* How long since the last mic status went to the server before it is sent
 * again unchanged.  A command submitted before the transport reaches
 * CONNECTED is discarded without a word, and the lobby is on screen calling
 * voiceTick while the map is still downloading - which is exactly when the
 * microphone first opens - so the report that matters is the one most likely
 * to be lost.  Three seconds sits inside the ~5 s the server republishes
 * lobby slots on (server_lifecycle.c), so a status put right here reaches
 * every other client's row on the next republish, and it is quick enough
 * that a lost report is corrected before a player reads the icon. */
#define VOICE_STATE_REPORT_INTERVAL_MS 3000

static bool isInitialised = false;
static VoiceEncoder *encoder = NULL;
static VoiceDecoder *decoder = NULL;
/* Off until the prefs say otherwise, and the same answer the prefs default
   gives, so nothing captures before they are read. */
static bool voiceEnabled = false;
static VoiceMode voiceMode = VOICE_MODE_PTT;
static bool pushToTalkHeld = false;
static float micGain = 1.0f;
static float outputVolume = 1.0f;
static float masterVolume = 1.0f;
static float inputLevel = 0.0f;

/* The recording and playback devices the player chose, by display name, ""
 * for the system default.  Kept verbatim whether or not the device is present:
 * gameFrontPutPrefs reads them back out at save time, and a headset that is
 * unplugged today must not be quietly overwritten with whatever stood in for
 * it.  The backend holds only the choice it could resolve. */
static char wantedRecordingDevice[VOICE_DEVICE_NAME_MAX];
static char wantedPlaybackDevice[VOICE_DEVICE_NAME_MAX];

/* The player's own transmit gate, from the mic icon on their row or the mute
 * key.  Never cleared: this is a standing intent, like a hardware mute switch,
 * so a disconnect, a reconnect, a device change or any per-speaker reset all
 * leave it alone.  voiceInit runs once per process, so zero-initialisation is
 * the only clearing it gets. */
static bool selfMuteRequested = false;

/* Open-mic gate state.  gateOpen is what voiceIsTransmitting reports; the
 * hangover counts the frames it is held open for after the level drops. */
static bool gateOpen = false;
static int gateHangover = 0;

/* The microphone test.  What was recorded is kept as encoded frames rather
 * than samples: the codec is what the other players hear through, so playing
 * it back decoded is the honest answer to "how do I sound", and three seconds
 * of packets cost a fraction of what the same three seconds of PCM would.
 * micTestPlayed counts frames handed to the backend, not frames the device
 * has finished with. */
static VoiceMicTestState micTestState = VOICE_MICTEST_IDLE;
static uint8_t micTestFrames[VOICE_MICTEST_FRAMES][CLIENT_VOICE_MAX_FRAME_BYTES];
static uint8_t micTestLengths[VOICE_MICTEST_FRAMES];
static int micTestRecorded = 0;
static int micTestPlayed = 0;

/* Whether the connection we are on carries this client's voice at all - it
 * has to exist, the server it reaches has to be carrying voice, and a viewer's
 * voice is not passed to the players.  Refreshed every tick, because
 * voiceIsTransmitting is asked by the settings dialog, which has no client of
 * its own to ask. */
static bool connectionCarriesVoice = false;

/* Whether the server we are on has voice turned off, for the settings section
 * and the lobby row to say so.  Separate from connectionCarriesVoice, which is
 * also false in single player and for a viewer - neither is the server
 * declining to carry voice, and neither should be reported as one. */
static bool serverVoiceIsOff = false;

/* Previous tick's captureIsWanted.  The microphone is first asked for on the
 * rising edge - a reason for it appearing is what asks - so starting the game,
 * where there is no reason for one yet, never prompts. */
static bool wasCaptureWanted = false;

/* Previous captured frame's transmit decision.  The falling edge is where
 * the end-of-utterance marker is sent from, since the last frame of speech
 * has already gone by the time transmission stops. */
static bool wasSending = false;

/* Ticks left before the recording device is asked for again after an open
 * that failed.  Only counts while the microphone is wanted but not running. */
static int captureRetryTicks = 0;

/* Whether the recording device was started and has not been stopped since.
 * The backend cannot be asked instead: voiceBackendCaptureIsOpen reports the
 * stream object, and that outlives a stop, because stopping only pauses so
 * that starting again does not put the microphone permission prompt back up. */
static bool captureStarted = false;

/* Whether the settings voice section has drawn since the last tick.  Its
 * level meter reads the captured level, which only moves while the recording
 * device runs, so the section being on screen is a reason to hold the
 * microphone open in its own right - it is where a player checks their
 * microphone before joining a game.  The section sets this on every frame it
 * draws and voiceTick takes it back, so a section that has stopped being
 * drawn stops asking without any dialog having to say so. */
static bool settingsWantsMic = false;

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

/* The most recent decoded frame's RMS amplitude per remote talker, and the
 * held peak over it.  The peak is kept in drawn-height units rather than
 * amplitude: voiceMeterScale is applied before it sees a reading, so the hold
 * and the decay run in the same space as the bar they are drawn in.  Cleared
 * wherever the talking stamps are. */
static float playerLevel[MAX_TANKS];
static VoicePeak playerPeak[MAX_TANKS];

/* How loud each remote talker is played here, 1.0 for unity.  Session-scoped
 * by design: it lasts until that player leaves, which is where it goes back
 * to unity, and it is never written to the preferences.  Zero-initialised
 * statics would mean silent, so voiceInit fills every slot with unity before
 * anything can read one. */
static float perPlayerVolume[MAX_TANKS];

/* An encoder producing frames too large for one voice segment is a
 * configuration problem, not a per-frame event: say so once. */
static bool warnedFrameTooLarge = false;

/* Last mic status put on the wire, and the clock reading it went out at: the
 * report goes out the moment one of them changes, and again once the
 * re-assert interval has passed with no change.  All four are cleared with
 * the rest of the per-connection state, which makes the first tick of the
 * next connection re-report. */
static bool reportedState = false;
static bool reportedHasMic = false;
static bool reportedSelfMuted = false;
static uint32_t reportedAtMs = 0;

/*********************************************************
*NAME:          captureIsWanted
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether anything still has a live reason for the
*  microphone to be open: the microphone test while it is
*  recording, or a mode that can put audio on the wire with
*  somewhere for that audio to go - a connection that carries
*  this client's voice, or the settings voice section on
*  screen with its level meter to feed.  The master switch
*  overrides all of them.
*
*  The test wants it while recording and at no other time -
*  playing back its recording is speakers only.  It is its own
*  reason, so it still runs with the mode set to Off.
*
*  A transmitting mode is not a reason by itself.  It is
*  loaded from the preferences before there is any connection,
*  and on its own it would open the recording device at launch
*  with no server to send to and nothing draining what was
*  captured.  Paired with one of the other two it holds the
*  device open between words, so the level meter keeps reading
*  and the first syllable after a push-to-talk press is not
*  lost to the device starting up.  The other way round, a
*  player who has set the mode to Off does not get their
*  microphone turned on by opening the settings.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static bool captureIsWanted(void) {
    if (!voiceEnabled) {
        return false;
    }
#if defined(WB_VOICEDEBUG)
    /* A recording wants the microphone on its own, with no server and no
     * microphone test, which is what lets a recording be made on one
     * machine. */
    if (voiceDebugIsRecording()) {
        return true;
    }
#endif
    return micTestState == VOICE_MICTEST_RECORDING ||
           (voiceMode != VOICE_MODE_OFF &&
            (connectionCarriesVoice || settingsWantsMic));
}

/*********************************************************
*NAME:          captureIsRunning
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether the recording device is capturing right
*  now, which is not what voiceBackendCaptureIsOpen answers -
*  that is whether a stream exists, and it stays true across
*  a stop.  Both halves are needed: the pause is only known
*  here, and the stream can go away underneath this without
*  the runtime being told, which is what a device change that
*  could not re-open one leaves behind.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static bool captureIsRunning(void) {
    return captureStarted && voiceBackendCaptureIsOpen();
}

/*********************************************************
*NAME:          stopCaptureIfIdle
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Pauses the microphone once nothing has a reason for it any
*  more.  The reasons share one recording stream, so none of
*  them may stop it on its own.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static void stopCaptureIfIdle(void) {
    if (!captureStarted || captureIsWanted()) {
        return;
    }
    /* Drops what both sides still hold, or re-enabling would open with a
     * burst of audio recorded before it was switched off. */
    voiceBackendCaptureStop();
    captureStarted = false;
    inputLevel = 0.0f;
    gateOpen = false;
    gateHangover = 0;
#if defined(WINBOLO_VOICE_AEC)
    /* Remote playback keeps summing into the reference ring while the
     * microphone is shut, but the ring only advances on the capture path,
     * so the same few slots pile up and nothing else clears them.  Dropped
     * here, or the first frames after capture resumes would be cancelled
     * against that pile rather than against what the speakers played. */
    voiceAecReset();
#endif
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
*  every state change.  Returns whether the device is open
*  and capturing on return: false when voice is not up or
*  nothing wants the microphone, otherwise what the backend
*  said, where false is an open that failed and may be
*  tried again later.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static bool startCaptureIfWanted(void) {
    if (!isInitialised || !captureIsWanted()) {
        return false;
    }
    captureStarted = voiceBackendCaptureStart();
    return captureStarted;
}

/*********************************************************
*NAME:          micTestEnd
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the microphone test to idle from wherever it had
*  got to.  Every route out of the test comes through here,
*  so the recording is dropped, the microphone released and
*  the echo canceller reset exactly once however it ends.
*
*ARGUMENTS:
*  discardPlayback - true to drop what is still queued for
*                    the speakers, false to let it finish
*********************************************************/
static void micTestEnd(bool discardPlayback) {
    if (micTestState == VOICE_MICTEST_IDLE) {
        return;
    }

    micTestState = VOICE_MICTEST_IDLE;
    micTestRecorded = 0;
    micTestPlayed = 0;

    if (discardPlayback) {
        voiceBackendLoopbackClear();
    }

#if defined(WINBOLO_VOICE_AEC)
    /* The test's reference was the player's own voice played back at them,
     * which is not the room the canceller will meet in a game and is the one
     * signal an adaptive filter cannot learn from.  What it took from that
     * must not be carried in. */
    voiceAecReset();
#endif

    stopCaptureIfIdle();
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
    int i;

    if (isInitialised) {
        return true;
    }

    /* Every slot at unity before anything reads one: a zero would play that
     * player silent, which is not what "never turned down" means.  Above the
     * backend rather than at the end of this function, because the players
     * panel asks voiceGetPlayerVolume for every row without first asking
     * whether voice came up at all - so on a machine whose audio device will
     * not open, a zeroed array would draw every slider hard left and read as
     * though everybody had been silenced. */
    for (i = 0; i < MAX_TANKS; i++) {
        perPlayerVolume[i] = 1.0f;
    }
    /* Unity here too, and for the same reason: the frontend pushes the master
     * volume down from its preferences after this, and a zero left behind by a
     * backend that would not start plays every talker silent. */
    masterVolume = 1.0f;

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

#if defined(WINBOLO_VOICE_AEC)
    /* A canceller that will not create is not fatal - voice runs on
     * uncancelled audio, which is what every build did before it existed.
     * Said once, because nothing else tells the two apart: the on/off
     * setting reports what the player asked for, not what came up. */
    if (!voiceAecInit()) {
        fprintf(stderr, "Voice error: echo canceller unavailable\n");
        fflush(stderr);
    }
#endif

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
#if defined(WB_VOICEDEBUG)
/* Defined with the rest of the diagnostic writers further down. */
static void voiceStatsClose(void);
#endif

void voiceCleanup(void) {
#if defined(WB_VOICEDEBUG)
    voiceStatsClose();
    /* Closed here so a clean exit patches the WAV lengths. */
    voiceDebugStop();
#endif
    voiceReset();
    voiceBackendShutdown();
#if defined(WINBOLO_VOICE_AEC)
    voiceAecShutdown();
#endif
    voiceEncoderDestroy(encoder);
    encoder = NULL;
    voiceDecoderDestroy(decoder);
    decoder = NULL;
    voiceEnabled = true;
    voiceMode = VOICE_MODE_PTT;
    pushToTalkHeld = false;
    gateOpen = false;
    gateHangover = 0;
    micGain = 1.0f;
    outputVolume = 1.0f;
    masterVolume = 1.0f;
    inputLevel = 0.0f;
    warnedFrameTooLarge = false;
    /* The streams went with the shutdown above, so nothing may still be
     * holding the microphone open across an init that comes after this. */
    captureStarted = false;
    settingsWantsMic = false;
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
*  playback, their talking indicator and level, and the local
*  mute and volume on them.
*
*  The mute goes with them because slots are recycled.  A
*  mute is on the player who was in the slot, not on the
*  slot: leaving it set would silence whoever joins into it
*  next - voice and, through the server's copy of the same
*  bit, chat as well - with nothing to show for it but a
*  "muted by you" icon on a player this client never muted.
*  The server drops its half in serverDisconnectClient.
*
*  The volume goes back to unity here for the same reason,
*  and this is the only place it does: it is set on a player,
*  so it lasts as long as they are in the game and no longer.
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
    playerLevel[player] = 0.0f;
    memset(&playerPeak[player], 0, sizeof(playerPeak[player]));
    perPlayerVolume[player] = 1.0f;
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
    /* A microphone test left running is per-connection state like any other:
     * it must not be found still recording on the other side of this. */
    micTestEnd(true);
    /* Forget what the last server was told, so the next connection is sent
     * this client's mic status rather than inheriting a match against a
     * server that never heard it. */
    reportedState = false;
    reportedHasMic = false;
    reportedSelfMuted = false;
    reportedAtMs = 0;
    connectionCarriesVoice = false;
    serverVoiceIsOff = false;
    wasCaptureWanted = false;
    wasSending = false;
    captureRetryTicks = 0;

    /* The connection was one of the reasons the microphone was open, and it
     * has just gone.  Nothing else would notice: no loop calls voiceTick at
     * the main menu, so a recording device left running there keeps the
     * operating system's microphone indicator lit and banks everything it
     * hears for whoever drains it next. */
    stopCaptureIfIdle();
}

/*********************************************************
*NAME:          voiceMicTestStart
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Starts the microphone test recording.  Opens the
*  recording device if this is the first ask, and stays idle
*  if there is no device to open or the user refuses the
*  microphone.  Does nothing if a test is already running.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceMicTestStart(void) {
    if (!isInitialised || micTestState != VOICE_MICTEST_IDLE) {
        return;
    }

    /* The master switch outranks the test - it is what decides whether the
     * microphone runs at all.  Started here rather than through
     * startCaptureIfWanted, because the test is not a reason for the
     * microphone until it is recording, and it is not recording until the
     * device came up. */
    if (!voiceEnabled || !voiceBackendCaptureStart()) {
        return;
    }
    captureStarted = true;

    micTestRecorded = 0;
    micTestPlayed = 0;
    micTestState = VOICE_MICTEST_RECORDING;
}

/*********************************************************
*NAME:          voiceMicTestCancel
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Stops the microphone test wherever it had got to.  A
*  cancel is the player asking for it to stop now, so what
*  is still queued for the speakers goes with it.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceMicTestCancel(void) {
    micTestEnd(true);
}

/*********************************************************
*NAME:          voiceMicTestGetState
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns where the microphone test has got to.
*
*ARGUMENTS:
*  (none)
*********************************************************/
VoiceMicTestState voiceMicTestGetState(void) {
    return micTestState;
}

/*********************************************************
*NAME:          voiceMicTestProgress
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns how far through the current phase the microphone
*  test is, as 0..1.  Zero when idle.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceMicTestProgress(void) {
    if (micTestState == VOICE_MICTEST_RECORDING) {
        return (float)micTestRecorded / (float)VOICE_MICTEST_FRAMES;
    }
    if (micTestState == VOICE_MICTEST_PLAYING && micTestRecorded > 0) {
        return (float)micTestPlayed / (float)micTestRecorded;
    }
    return 0.0f;
}

/*********************************************************
*NAME:          voiceSetEnabled
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  The master switch.  Off means no capture, nothing sent
*  and nothing played, whatever the mode and the microphone
*  test are doing.  The recording device is paused rather
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
    micTestEnd(true);
    voiceBackendLoopbackClear();
    stopCaptureIfIdle();

    /* Drop every remote talker along with what they had buffered, so the
     * ones mid-sentence stop where they are rather than finishing.  The
     * talking stamps and the levels go with them: nothing of theirs is
     * audible any more, so nothing of theirs may still be shown as talking
     * or as loud.  The per-player volumes stay - switching voice off and on
     * is not the player leaving. */
    for (i = 0; i < MAX_TANKS; i++) {
        releaseSpeaker(i);
        talkingUntilMs[i] = 0;
        talkingStamped[i] = false;
        playerLevel[i] = 0.0f;
        memset(&playerPeak[i], 0, sizeof(playerPeak[i]));
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
*  Opens the recording device if something now wants it and
*  pauses it once nothing does.  A mode that can transmit is
*  only one with a connection to carry the audio, so a mode
*  chosen from the main menu opens nothing by itself.
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
*NAME:          voiceSetSelfMuted
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Sets the player's own transmit gate.  Nothing goes on the
*  wire while it is on, and the state reported to the server
*  follows on the next tick.  The recording device is left
*  open: the echo canceller keeps tracking the room, so
*  unmuting is not the start of a new conversation for it.
*
*ARGUMENTS:
*  muted - true to stop transmitting
*********************************************************/
void voiceSetSelfMuted(bool muted) {
    selfMuteRequested = muted;
}

/*********************************************************
*NAME:          voiceIsSelfMuted
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether the player has muted themselves.  This is
*  the toggle alone - the settings that can also stop audio
*  reaching the wire are not folded in, so the icon that
*  drives the toggle reads back what it set.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceIsSelfMuted(void) {
    return selfMuteRequested;
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
*  The player's own mute is tested here for that reason: the
*  light and the wire both follow from the one gate.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceIsTransmitting(void) {
    if (!isInitialised || !voiceEnabled || !connectionCarriesVoice ||
        selfMuteRequested) {
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
*NAME:          voiceConnectionCarriesVoice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether the connection we are on carries this
*  client's voice at all.  voiceTick settles it once a tick
*  and this hands it back unchanged, so the game view's mute
*  indicator can stay off a connection that would never have
*  carried anything.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceConnectionCarriesVoice(void) {
    return connectionCarriesVoice;
}

/*********************************************************
*NAME:          voiceServerHasVoiceOff
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether the server we are on was started with
*  voice off, so the settings section and the lobby row can
*  say the feature is unavailable here rather than offering
*  controls that reach nothing.  False in single player and
*  at the main menu: there is no server there declining
*  anything, and a client that has not yet been told the
*  mode reads the on it always ran with.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceServerHasVoiceOff(void) {
    return serverVoiceIsOff;
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
*NAME:          voiceSetMasterVolume
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Sets the master gain, which multiplies the voice output
*  volume and the talker's own volume.  Voice has its own
*  playback path and never reaches the sound mixer, so the
*  frontend hands the master volume here as well as there.
*
*ARGUMENTS:
*  gain - 1.0f is unity
*********************************************************/
void voiceSetMasterVolume(float gain) {
    if (gain < 0.0f) {
        gain = 0.0f;
    }
    masterVolume = gain;
}

/*********************************************************
*NAME:          voiceGetMasterVolume
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the master gain applied to decoded remote audio.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceGetMasterVolume(void) {
    return masterVolume;
}

/*********************************************************
*NAME:          applyOutputVolume
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Scales one decoded frame by the master volume, the voice
*  output volume and that talker's own volume, in place.
*  Done here rather than in the backend so the device
*  contract stays the same on every platform.
*
*ARGUMENTS:
*  pcm    - VOICE_FRAME_SAMPLES mono S16 samples, scaled in
*           place
*  player - the player the frame came from, whose own volume
*           multiplies the other two
*********************************************************/
static void applyOutputVolume(int16_t *pcm, int player) {
    int i;
    float sample;
    float gain = masterVolume * outputVolume * perPlayerVolume[player];

    /* Unity is the common case and every sample would survive it unchanged.
     * Tested against the three together: any one alone can be off unity
     * while the product is not. */
    if (gain == 1.0f) {
        return;
    }

    for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        sample = (float)pcm[i] * gain;
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
*  Returns the 0..1 RMS amplitude of the most recent
*  captured frame. This is the measurement, not the height a
*  meter draws it at - see voiceGetInputMeter for that.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceGetInputLevel(void) {
    return inputLevel;
}

/*********************************************************
*NAME:          voiceGetInputMeter
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the captured level as the 0..1 height a meter
*  draws it at, which is the amplitude mapped into dB.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceGetInputMeter(void) {
    return voiceMeterScale(inputLevel);
}

/*********************************************************
*NAME:          voiceSettingsSectionDrawn
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Holds the recording device open for the frame the settings
*  voice section is drawing.  Its level meter reads what was
*  captured, so with the device shut it reads zero and the one
*  check a player can make on their microphone before joining
*  a game tells them nothing.
*
*  Said again on every frame rather than taken back at the
*  end: the next tick reads it and clears it, so a section
*  that has stopped being drawn releases the microphone on its
*  own and no dialog has to remember to say it closed.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceSettingsSectionDrawn(void) {
    settingsWantsMic = true;
}

/*********************************************************
*NAME:          voiceSettingsSectionClosed
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Gives up the settings voice section's hold on the recording
*  device and releases the device if nothing else wants it.
*
*  The hold otherwise lapses on the next tick, which covers a
*  section that stops being drawn inside a loop that carries on
*  ticking - the in-game overlay.  It does not cover the
*  settings dialog that owns the loop itself: closing it hands
*  control back to the main menu, where nothing calls voiceTick
*  at all, so a microphone left running there stays running,
*  with the operating system's indicator lit and everything it
*  hears banked for whoever drains it next.  That is what this
*  is for.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceSettingsSectionClosed(void) {
    settingsWantsMic = false;
    stopCaptureIfIdle();
}

/*********************************************************
*NAME:          copyDeviceName
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Copies a device name into one of the fixed buffers above,
*  truncating rather than assuming a length: the name is a
*  free-form string that came from an audio driver.
*
*ARGUMENTS:
*  dest - a VOICE_DEVICE_NAME_MAX buffer
*  name - the name to copy, or NULL for the system default
*********************************************************/
static void copyDeviceName(char *dest, const char *name) {
    size_t len;

    if (name == NULL) {
        name = "";
    }
    len = strlen(name);
    if (len >= VOICE_DEVICE_NAME_MAX) {
        len = VOICE_DEVICE_NAME_MAX - 1;
    }
    memcpy(dest, name, len);
    dest[len] = '\0';
}

/*********************************************************
*NAME:          voiceRecordingDeviceCount
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns how many microphones there are to choose between.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voiceRecordingDeviceCount(void) {
    return voiceBackendRecordingDeviceCount();
}

/*********************************************************
*NAME:          voiceRecordingDeviceName
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns one microphone's display name out of the list the
*  last count took.
*
*ARGUMENTS:
*  index - 0..count-1
*********************************************************/
const char *voiceRecordingDeviceName(int index) {
    return voiceBackendRecordingDeviceName(index);
}

/*********************************************************
*NAME:          voicePlaybackDeviceCount
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns how many playback devices there are to choose
*  between.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voicePlaybackDeviceCount(void) {
    return voiceBackendPlaybackDeviceCount();
}

/*********************************************************
*NAME:          voicePlaybackDeviceName
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns one playback device's display name out of the list
*  the last count took.
*
*ARGUMENTS:
*  index - 0..count-1
*********************************************************/
const char *voicePlaybackDeviceName(int index) {
    return voiceBackendPlaybackDeviceName(index);
}

/*********************************************************
*NAME:          voiceSetRecordingDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Takes the microphone the player chose, by display name.
*
*ARGUMENTS:
*  name - the display name, or NULL/"" for the system default
*********************************************************/
void voiceSetRecordingDevice(const char *name) {
    copyDeviceName(wantedRecordingDevice, name);
    /* The backend only takes a device that is present, and its answer is
     * deliberately not passed on: what the player asked for is held here
     * either way, so an absent device stays chosen and capture runs on the
     * default until it comes back. */
    voiceBackendSetRecordingDevice(wantedRecordingDevice);
}

/*********************************************************
*NAME:          voiceGetRecordingDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the microphone the player chose, "" for the system
*  default.
*
*ARGUMENTS:
*  (none)
*********************************************************/
const char *voiceGetRecordingDevice(void) {
    return wantedRecordingDevice;
}

/*********************************************************
*NAME:          voiceSetPlaybackDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Takes the playback device the player chose, by display
*  name.
*
*ARGUMENTS:
*  name - the display name, or NULL/"" for the system default
*********************************************************/
void voiceSetPlaybackDevice(const char *name) {
    copyDeviceName(wantedPlaybackDevice, name);
    voiceBackendSetPlaybackDevice(wantedPlaybackDevice);
}

/*********************************************************
*NAME:          voiceGetPlaybackDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the playback device the player chose, "" for the
*  system default.
*
*ARGUMENTS:
*  (none)
*********************************************************/
const char *voiceGetPlaybackDevice(void) {
    return wantedPlaybackDevice;
}

/*********************************************************
*NAME:          applyWantedDevices
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Puts the chosen devices back in force after the set of
*  devices has moved, so a headset that has just been
*  unplugged is left behind and one that has just come back
*  is picked up again.
*
*  Costs nothing when the choice is already in force: the
*  backend returns early for a name it is already on.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static void applyWantedDevices(void) {
    /* The backend refuses a name that is not plugged in, so a refusal is the
     * device having gone: fall back to the system default and leave the
     * wanted name alone, so the headset reclaims it when it returns. */
    if (!voiceBackendSetRecordingDevice(wantedRecordingDevice)) {
        voiceBackendSetRecordingDevice("");
    }
    if (!voiceBackendSetPlaybackDevice(wantedPlaybackDevice)) {
        voiceBackendSetPlaybackDevice("");
    }
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
*NAME:          voiceSetPlayerVolume
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Sets how loud one player is played here.  Local playback
*  only: nothing is sent, and the player being turned down
*  has no way of knowing.  Lasts until they leave, and is
*  never persisted.
*
*ARGUMENTS:
*  player - the player number to set
*  gain   - 1.0f is unity, clamped to
*           0..VOICE_PLAYER_VOLUME_MAX
*********************************************************/
void voiceSetPlayerVolume(int player, float gain) {
    if (player < 0 || player >= MAX_TANKS) {
        return;
    }
    if (gain < 0.0f) {
        gain = 0.0f;
    } else if (gain > VOICE_PLAYER_VOLUME_MAX) {
        gain = VOICE_PLAYER_VOLUME_MAX;
    }
    perPlayerVolume[player] = gain;
}

/*********************************************************
*NAME:          voiceGetPlayerVolume
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns how loud one player is played here, 1.0f for a
*  player who has not been turned up or down.
*
*ARGUMENTS:
*  player - the player number to ask about
*********************************************************/
float voiceGetPlayerVolume(int player) {
    if (player < 0 || player >= MAX_TANKS) {
        return 1.0f;
    }
    return perPlayerVolume[player];
}

/*********************************************************
*NAME:          playerIsTalking
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether one player's talking stamp is still live
*  at nowMs.  The one test of it: the talking bitmap and the
*  per-player level both ask this rather than each keeping a
*  copy of the comparison, which could disagree with the
*  other.
*
*  Reports on the stamp without touching it.  Dropping an
*  expired one belongs to the caller that walks every slot.
*
*ARGUMENTS:
*  player - the player number to ask about
*  nowMs  - the clock reading to test the stamp against
*********************************************************/
static bool playerIsTalking(int player, uint32_t nowMs) {
    if (!talkingStamped[player]) {
        return false;
    }
    /* Signed difference: the clock wraps every 49 days or so, and now <
     * until would read a wrapped stamp as one far in the future. */
    return (int32_t)(nowMs - talkingUntilMs[player]) < 0;
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
        if (!playerIsTalking(i, now)) {
            /* An expired stamp is dropped here rather than left to sit, so
             * it cannot come back round as live a wrap later.  This is the
             * one loop over every slot, so this is where that happens;
             * clearing a slot that was never stamped costs nothing. */
            talkingStamped[i] = false;
            continue;
        }
        talking |= (PlayerBitMap)1u << i;
    }

    return talking;
}

/*********************************************************
*NAME:          voiceGetPlayerLevel
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the 0..1 height a meter draws one remote talker's
*  loudness at - meter heights, like voiceGetInputMeter, not a
*  raw amplitude - held and then decaying, so a syllable stays
*  readable rather than passing by in one frame.
*
*  Zero for a player who is not talking, was never heard, is
*  muted here, or is out of range.
*
*ARGUMENTS:
*  player - the player number to ask about
*********************************************************/
float voiceGetPlayerLevel(int player) {
    uint32_t now;
    float live;

    if (player < 0 || player >= MAX_TANKS) {
        return 0.0f;
    }

    now = voiceBackendNowMs();
    /* Fed zero rather than left alone once they stop, so a talker fades over
     * the peak's own fall time instead of snapping off, and a player who has
     * never been heard reads a flat zero from the first call. */
    live = playerIsTalking(player, now)
               ? voiceMeterScale(playerLevel[player])
               : 0.0f;
    /* A getter that advances the state, which is worth a sentence: the decay
     * runs against the clock, and between decoded frames nothing else feeds
     * this, so the peak would otherwise stand wherever the last frame left
     * it. */
    return voicePeakUpdate(&playerPeak[player], live, now);
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
/* ── Voice diagnostics ───────────────────────────────────────────────
 *
 * Built only under WB_VOICEDEBUG, beside the capture-chain recorder. The
 * counters these files report — the channel layer's best-effort drops and the
 * server's per-slot refusals — are compiled in always and cost an increment;
 * what is held back is the writing, because a shipping client must not put
 * every conversation on the player's disk in the directory it was launched
 * from. A build handed out for a diagnosis carries it and needs no switch
 * from the person running it.
 */
#if defined(WB_VOICEDEBUG)

/*
 *
 * Two CSVs in the working directory, written for the length of every run:
 * voicestats-client.csv for what this client captured, encoded and queued,
 * and voicestats-talkers.csv for what it played back from each talker.
 *
 * A voice fault is reported afterwards by someone who could not see it while
 * it was happening. Between the encoder and the wire a frame can be dropped
 * by a full best-effort ring or left behind by a frame with no budget for it,
 * and neither shows up anywhere: the counters exist, nothing reads them.
 * These files are what a run leaves behind so the next report has numbers
 * against it. Flushed per row, so a client that is killed still leaves them. */
#define VOICE_STAT_INC(x) ((x)++)

static uint32_t statsCaptured;    /* whole frames off the capture device  */
static uint32_t statsMicOpen;     /* of those, with the microphone open   */
static uint32_t statsSending;     /* of those, while transmitting         */
static uint32_t statsEncoded;     /* frames the encoder produced          */
static uint32_t statsQueued;      /* frames handed to the transport       */
static uint32_t statsTooLarge;    /* frames dropped for exceeding a segment */
static FILE *statsClientFp;
static FILE *statsTalkersFp;

/* The audio beside the counters: voice-sent.wav is this client's own frames
 * encoded and decoded back, voice-heard-<player>.wav is what came out of each
 * talker's jitter buffer. Between them they are what the far end sounded like,
 * which no counter can answer and which is the whole reason a listener is in
 * the room at all.
 *
 * Only frames that actually moved are written — sent on one side, played on
 * the other — so a file grows with talking rather than with wall-clock, and a
 * quiet game leaves almost nothing.
 *
 * Two things this must survive that the capture-chain recorder does not: the
 * run ends when somebody closes the window however they like, and the person
 * running it is not the person who wants the file. So the RIFF lengths are
 * rewritten as it goes rather than at close, leaving a playable file at every
 * moment, and each file stops at a size nobody has to think about before
 * handing it over. */
#define VOICE_STATS_PATH_MAX   512
#define VOICE_WAV_HEADER_BYTES 44

/* Put the file where --voice-record was pointed, so one run leaves one folder
 * to collect. Without that switch the recorder never started and the working
 * directory is all there is to go on. */
static void voiceStatsPath(char *out, size_t outLen, const char *name) {
    const char *dir = voiceDebugOutDir();

    if (dir != NULL) {
        snprintf(out, outLen, "%s/%s", dir, name);
    } else {
        snprintf(out, outLen, "%s", name);
    }
}

/* 45 minutes of audio per file, which is far more speech than a game holds
 * and still bounds what somebody is asked to send. */
#define VOICE_WAV_MAX_SAMPLES  (45u * 60u * VOICE_SAMPLE_RATE)

typedef struct {
    FILE    *fp;
    uint32_t samples;
    bool     full;      /* hit the cap; header stays right, writing stops */
} VoiceStatsWav;

static VoiceStatsWav wavSent;
static VoiceStatsWav wavHeard[MAX_TANKS];
static VoiceDecoder *wavSentDecoder;

static void putLe16At(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void putLe32At(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Rewrite the two length fields to match what has been written, then leave
 * the handle at the end again. Called after every frame: a 44 byte seek and
 * rewrite against a buffered stream costs nothing beside an Opus decode, and
 * it is what makes a file killed mid-run still open. */
static void voiceStatsWavSync(VoiceStatsWav *w) {
    uint8_t field[4];
    uint32_t dataBytes = w->samples * (uint32_t)sizeof(int16_t);

    if (fseek(w->fp, 4, SEEK_SET) == 0) {
        putLe32At(field, (VOICE_WAV_HEADER_BYTES - 8u) + dataBytes);
        fwrite(field, 1, sizeof(field), w->fp);
    }
    if (fseek(w->fp, 40, SEEK_SET) == 0) {
        putLe32At(field, dataBytes);
        fwrite(field, 1, sizeof(field), w->fp);
    }
    fseek(w->fp, 0, SEEK_END);
    fflush(w->fp);
}

static bool voiceStatsWavOpen(VoiceStatsWav *w, const char *name) {
    uint8_t hdr[VOICE_WAV_HEADER_BYTES];
    char path[VOICE_STATS_PATH_MAX];

    voiceStatsPath(path, sizeof(path), name);
    w->fp = fopen(path, "w+b");
    w->samples = 0;
    w->full = false;
    if (w->fp == NULL) {
        return false;
    }
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "RIFF", 4);
    putLe32At(hdr + 4, VOICE_WAV_HEADER_BYTES - 8u);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    putLe32At(hdr + 16, 16);
    putLe16At(hdr + 20, 1);                       /* PCM              */
    putLe16At(hdr + 22, 1);                       /* mono             */
    putLe32At(hdr + 24, VOICE_SAMPLE_RATE);
    putLe32At(hdr + 28, VOICE_SAMPLE_RATE * 2u);  /* bytes per second */
    putLe16At(hdr + 32, 2);                       /* bytes per frame  */
    putLe16At(hdr + 34, 16);                      /* bits per sample  */
    memcpy(hdr + 36, "data", 4);
    putLe32At(hdr + 40, 0);
    if (fwrite(hdr, 1, sizeof(hdr), w->fp) != sizeof(hdr)) {
        fclose(w->fp);
        w->fp = NULL;
        return false;
    }
    return true;
}

static void voiceStatsWavWrite(VoiceStatsWav *w, const char *path,
                               const int16_t *pcm) {
    if (w->full) {
        return;
    }
    if (w->fp == NULL && !voiceStatsWavOpen(w, path)) {
        w->full = true;   /* will not open; stop trying rather than warn a
                           * player about a file they did not ask for */
        return;
    }
    if (fwrite(pcm, sizeof(int16_t), VOICE_FRAME_SAMPLES, w->fp) !=
        VOICE_FRAME_SAMPLES) {
        w->full = true;
        return;
    }
    w->samples += VOICE_FRAME_SAMPLES;
    voiceStatsWavSync(w);
    if (w->samples >= VOICE_WAV_MAX_SAMPLES) {
        w->full = true;
    }
}

/* One frame this client just put on the wire, decoded back so the file holds
 * what a listener's decoder would produce rather than Opus bytes. */
static void voiceStatsRecordSent(const uint8_t *packet, int len) {
    int16_t pcm[VOICE_FRAME_SAMPLES];

    if (packet == NULL || len <= 0) {
        return;
    }
    if (wavSentDecoder == NULL) {
        wavSentDecoder = voiceDecoderCreate();
        if (wavSentDecoder == NULL) {
            return;
        }
    }
    if (voiceDecoderDecode(wavSentDecoder, packet, len, pcm) < 0) {
        return;
    }
    voiceStatsWavWrite(&wavSent, "voice-sent.wav", pcm);
}

/* A clean exit closes what it opened. The files are already valid without
 * this — every frame rewrites the lengths — so it tidies rather than saves. */
static void voiceStatsClose(void) {
    int i;

    if (wavSent.fp != NULL) {
        fclose(wavSent.fp);
        wavSent.fp = NULL;
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if (wavHeard[i].fp != NULL) {
            fclose(wavHeard[i].fp);
            wavHeard[i].fp = NULL;
        }
    }
    if (wavSentDecoder != NULL) {
        voiceDecoderDestroy(wavSentDecoder);
        wavSentDecoder = NULL;
    }
    if (statsClientFp != NULL) {
        fclose(statsClientFp);
        statsClientFp = NULL;
    }
    if (statsTalkersFp != NULL) {
        fclose(statsTalkersFp);
        statsTalkersFp = NULL;
    }
}

/* One frame this client just played from a talker. */
static void voiceStatsRecordHeard(int player, const int16_t *pcm) {
    char path[64];

    if (player < 0 || player >= MAX_TANKS) {
        return;
    }
    snprintf(path, sizeof(path), "voice-heard-%d.wav", player);
    voiceStatsWavWrite(&wavHeard[player], path, pcm);
}

/* Open one of the two files on first use and write its header. A file that
 * will not open is reported once and left alone: a diagnostic must not take
 * the client down with it. */
static FILE *voiceStatsFile(FILE **slot, const char *name,
                            const char *header) {
    if (*slot == NULL) {
        char path[VOICE_STATS_PATH_MAX];

        voiceStatsPath(path, sizeof(path), name);
        *slot = fopen(path, "w");
        if (*slot == NULL) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                fprintf(stderr, "Voice stats: cannot write %s\n", name);
                fflush(stderr);
            }
            return NULL;
        }
        fputs(header, *slot);
    }
    return *slot;
}

static bool voiceStatsTalkersDue(uint32_t nowMs) {
    static uint32_t nextMs = 0;
    if (nowMs < nextMs) {
        return false;
    }
    nextMs = nowMs + 1000;
    return true;
}

/* The send side, once a second. Cumulative, so two rows a minute apart
 * answer "is it still happening" as well as a rate would. */
static void voiceStatsWriteClient(struct ClientSim *cs) {
    static uint32_t nextMs = 0;
    uint32_t nowMs = voiceBackendNowMs();
    uint32_t sent = 0, ringDropped = 0, budgetSkipped = 0;
    FILE *fp;

    if (nowMs < nextMs) {
        return;
    }
    nextMs = nowMs + 1000;

    fp = voiceStatsFile(&statsClientFp, "voicestats-client.csv",
                        "ms,carriesVoice,selfMuted,captured,micOpen,sending,"
                        "encoded,queued,tooLarge,wireSent,ringDropped,"
                        "budgetSkipped\n");
    if (fp == NULL) {
        return;
    }
    clientSimNetGetVoiceChannelStats(cs, &sent, &ringDropped, &budgetSkipped);
    fprintf(fp, "%u,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", nowMs,
            connectionCarriesVoice ? 1 : 0, selfMuteRequested ? 1 : 0,
            statsCaptured, statsMicOpen, statsSending, statsEncoded,
            statsQueued, statsTooLarge, sent, ringDropped, budgetSkipped);
    fflush(fp);
}

#else  /* !WB_VOICEDEBUG — the writing compiles out, the call sites stay put */

#define VOICE_STAT_INC(x)            ((void)0)
#define voiceStatsRecordSent(p, n)   ((void)0)
#define voiceStatsRecordHeard(i, p)  ((void)0)
#define voiceStatsWriteClient(cs)    ((void)0)
#define voiceStatsClose()            ((void)0)

#endif /* WB_VOICEDEBUG */

static void voicePlayRemote(struct ClientSim *cs) {
    uint8_t packet[VOICE_MAX_PACKET];
    int16_t pcm[VOICE_FRAME_SAMPLES];
    uint8_t fromPlayer, seq, flags;
    int len;
    int i;
    int pops;
    int queued;

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
        uint32_t popNowMs;

        if (speakers[i] == NULL) {
            continue;
        }
        /* Read once rather than per pop: the buffer only measures how long
         * it has been waiting for a frame, and the pops below all happen at
         * what is the same instant as far as that is concerned. */
        popNowMs = voiceBackendNowMs();
        /* The queued depth is in whole 20 ms frames, so it compares directly
         * against the target. */
        for (pops = 0; pops < VOICE_PLAYBACK_MAX_POPS_PER_CALL; pops++) {
            queued = voiceBackendSpeakerQueuedFrames(i);
            if (queued >= VOICE_PLAYBACK_TARGET_FRAMES) {
                break;
            }
            /* Nothing left to play - the jitter buffer is waiting on a frame
             * that has not arrived yet, and asking again will not change
             * that until the next call. */
            if (!voiceSpeakerPop(speakers[i], pcm, popNowMs)) {
                break;
            }
            /* Before the output volume, so the file holds the decoded audio
             * rather than this listener's volume setting — someone recording
             * with their speakers down still hands over a usable file. */
            voiceStatsRecordHeard(i, pcm);
#if defined(WB_VOICEDEBUG)
            voiceDebugTap(VOICE_TAP_REMOTE, i, pcm);
#endif
            applyOutputVolume(pcm, i);
            /* Read after both volumes rather than before them: the bar this
             * feeds sits beside the slider that scales this talker, so it
             * has to show that moving the slider did something.  The local
             * input meter is taken after the microphone gain for the same
             * reason. */
            playerLevel[i] = voiceFrameRms(pcm, VOICE_FRAME_SAMPLES);
#if defined(WINBOLO_VOICE_AEC)
            /* Taken after the output gain, so the reference is at the level
             * the loudspeakers will carry, and before the hand-over, since
             * queued is how much sits in front of this frame on that talker
             * and so how long it is until the microphone hears it. */
            voiceAecAddReference(pcm, queued);
#endif
            voiceBackendSpeakerPlay(i, pcm);
        }
    }

#if defined(WB_VOICEDEBUG)
    /* One row per active talker per second, beside the send-side file. What
     * a listener stopped hearing is the other half of the question the send
     * side asks. */
    {
        uint32_t nowMs = voiceBackendNowMs();
        VoiceSpeakerStats stats;

        if (voiceStatsTalkersDue(nowMs)) {
            FILE *fp = voiceStatsFile(&statsTalkersFp, "voicestats-talkers.csv",
                                      "ms,player,played,concealed,lateDropped,"
                                      "evicted,queued\n");
            if (fp != NULL) {
                for (i = 0; i < MAX_TANKS; i++) {
                    if (speakers[i] == NULL) {
                        continue;
                    }
                    voiceSpeakerGetStats(speakers[i], &stats);
                    fprintf(fp, "%u,%d,%u,%u,%u,%u,%d\n", nowMs, i,
                            stats.played, stats.concealed, stats.lateDropped,
                            stats.evicted, voiceBackendSpeakerQueuedFrames(i));
                }
                fflush(fp);
            }
        }
    }

    /* The talker table is private to this file, so the recorder says when a
     * row is due and the walk happens here. */
    {
        uint32_t nowMs = voiceBackendNowMs();
        VoiceSpeakerStats stats;

        if (voiceDebugSpeakerStatsDue(nowMs)) {
            for (i = 0; i < MAX_TANKS; i++) {
                if (speakers[i] == NULL) {
                    continue;
                }
                voiceSpeakerGetStats(speakers[i], &stats);
                voiceDebugSpeakerRow(nowMs, i, &stats,
                                     voiceBackendSpeakerQueuedFrames(i));
            }
        }
    }
#endif
}

/*********************************************************
*NAME:          voiceReportState
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Tells the server this client's mic status: at once on the
*  tick it changes, and again every
*  VOICE_STATE_REPORT_INTERVAL_MS whether it has changed or
*  not.  A microphone is had once voice is up and a recording
*  device has actually opened - asking for one that never came
*  up is not having one.  Muted is having a microphone that
*  cannot reach the wire, so the two never both read true.
*
*  The repeat is what makes the status arrive at all.  A
*  command submitted before the transport reaches CONNECTED is
*  dropped without a word, and the lobby is on screen - and
*  calling this - while the map is still downloading, which is
*  where the first report of an open microphone lands.  Sent
*  once, that report has nothing anywhere to correct it, and
*  the player shows as having no microphone to every other
*  client for the rest of the connection.  A re-send that
*  matches what the slot already holds is dropped by the
*  server without publishing anything, so repeating costs one
*  small command on the reliable channel.
*
*ARGUMENTS:
*  cs - the connected client
*********************************************************/
void voiceReportState(struct ClientSim *cs) {
    bool hasMic;
    bool selfMuted;
    uint32_t nowMs;

    if (cs == NULL) {
        return;
    }

    hasMic = isInitialised && voiceBackendCaptureIsOpen();
    /* Self muted is having a microphone that cannot reach the wire: the
     * player's own mute, voice switched off, or the mode set to Off.  A
     * push-to-talk player between presses is not muted - they can talk
     * whenever they choose to.  The or is inside the hasMic gate, or a
     * player with no microphone would start reporting itself muted. */
    selfMuted = hasMic && (selfMuteRequested ||
                           !(voiceEnabled && voiceMode != VOICE_MODE_OFF));

    /* Unsigned subtraction, so the interval still measures right across the
     * clock's 32-bit wrap. */
    nowMs = voiceBackendNowMs();
    if (reportedState && hasMic == reportedHasMic &&
        selfMuted == reportedSelfMuted &&
        (uint32_t)(nowMs - reportedAtMs) < VOICE_STATE_REPORT_INTERVAL_MS) {
        return;
    }

    clientSimNetSendVoiceState(cs, hasMic, selfMuted);
    reportedState = true;
    reportedHasMic = hasMic;
    reportedSelfMuted = selfMuted;
    reportedAtMs = nowMs;
}

/*********************************************************
*NAME:          micTestPlayTick
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Plays the microphone test's recording back, decoding the
*  stored frames one at a time and keeping the playback
*  stream topped up to the same depth a remote talker is
*  held at.  Paced off that depth rather than off the tick,
*  because voiceTick is called at whatever rate the loop
*  calling it renders at, which is not the fifty frames a
*  second the recording was made at.
*
*  Handing the last frame over ends the test.  The device
*  still has a fraction of a second of it left to play, and
*  that is left to finish on its own rather than waited for.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static void micTestPlayTick(void) {
    int16_t pcm[VOICE_FRAME_SAMPLES];
    int pops;
    int queued;

    for (pops = 0; pops < VOICE_PLAYBACK_MAX_POPS_PER_CALL; pops++) {
        if (micTestPlayed >= micTestRecorded) {
            micTestEnd(false);
            return;
        }

        /* The queued depth is in whole 20 ms frames, so it compares directly
         * against the target. */
        queued = voiceBackendLoopbackQueuedFrames();
        if (queued >= VOICE_PLAYBACK_TARGET_FRAMES) {
            break;
        }

        if (voiceDecoderDecode(decoder, micTestFrames[micTestPlayed],
                               (int)micTestLengths[micTestPlayed],
                               pcm) != VOICE_FRAME_SAMPLES) {
            /* A frame that will not decode costs the playback 20 ms and
             * nothing else - the rest of the recording is still worth
             * hearing. */
            micTestPlayed++;
            continue;
        }
        micTestPlayed++;

#if defined(WINBOLO_VOICE_AEC)
        /* What is played, not what was captured, and the depth is read
         * before the hand-over, since that is what sits in front of this
         * frame and so how long it is until the room hears it. */
        voiceAecAddReference(pcm, queued);
#endif
        voiceBackendLoopbackPlay(pcm);
    }
}

/*********************************************************
*NAME:          voiceTick
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Drains whole captured frames, applies mic gain, and hands
*  each encoded frame to the microphone test's recording,
*  the server, or both.  Plays whatever the other players
*  sent, and whatever the microphone test has recorded.
*  Main thread only.
*
*ARGUMENTS:
*  cs - the connected client, or NULL when there is no
*       network (the microphone test still runs)
*********************************************************/
void voiceTick(struct ClientSim *cs) {
    int16_t pcm[VOICE_FRAME_SAMPLES];
    uint8_t packet[VOICE_MAX_PACKET];
    bool sending;
    bool wanted;
    int frame;
    int i;
    int got;
    int encodedLen;
    float sample;
    float sumSquares;
    float gateLevel;
#if defined(WB_VOICEDEBUG)
    /* Samples the mic gain pushed out of the int16 range, counted per frame
     * before the clamp writes them back. */
    int clipped = 0;
    /* What the frame reads once the automatic gain has had it - the level
     * the encoder sees, recorded so a run shows it against the gate's. */
    float gainedLevel = 0.0f;
#endif

    if (!isInitialised) {
        return;
    }

    /* Above every early return below. A client that has stopped wanting the
     * microphone — the state a player describes as nobody being able to hear
     * them — is exactly the one whose row has to keep appearing. */
    voiceStatsWriteClient(cs);

    /* Before anything is played: a device that has just gone takes its
     * streams with it, and a talker's frames belong on the one that is open
     * now rather than on the one that went away. */
    if (voiceBackendDevicesChanged()) {
        applyWantedDevices();
    }

    if (cs != NULL) {
        voicePlayRemote(cs);
    }

    /* The connection has to be one that carries voice at all - the local
     * transport single-player attaches goes nowhere - and a viewer captures
     * for the microphone test like anyone else, but its voice is not carried
     * to the players, so there is nothing to send.  A server started with
     * -voice off drains and discards what it is sent, so it is no more a
     * destination than the local transport is: without this the microphone
     * would be opened and frames encoded for nothing.  Kept here rather than
     * asked for inside voiceIsTransmitting, which the settings dialog calls
     * with no client of its own. */
    serverVoiceIsOff = clientSimNetHasVoiceTransport(cs) &&
                       clientSimGetServerVoiceMode(cs) == serverVoiceOff;
    connectionCarriesVoice =
        clientSimNetHasVoiceTransport(cs) && !clientSimIsSpectator(cs) &&
        !serverVoiceIsOff;

    /* Settled once for the tick, above everything that acts on it: the
     * settings section's asking is taken back at the end of this call, so
     * a second reading part way down would be a different answer. */
    wanted = captureIsWanted();

    /* A reason for the microphone appearing is what opens it.  The first
     * attempt goes out on the tick the reason appears; while that has not
     * produced a running device it is asked for again every
     * VOICE_CAPTURE_RETRY_TICKS, since the open fails for as long as the
     * operating system's permission prompt is still up.  The start is
     * idempotent, so an open that succeeded is never repeated. */
#if defined(WB_VOICEDEBUG)
    /* An injected run takes its audio from the file rather than the
     * microphone, so it opens no recording device at all and runs on a
     * machine whose microphone does not work. */
    if (wanted && !voiceDebugInjectIsOpen() && !captureIsRunning()) {
#else
    if (wanted && !captureIsRunning()) {
#endif
        if (!wasCaptureWanted || captureRetryTicks == 0) {
            startCaptureIfWanted();
            captureRetryTicks = VOICE_CAPTURE_RETRY_TICKS;
        } else {
            captureRetryTicks--;
        }
    }
    wasCaptureWanted = wanted;

    /* And the other way round.  A reason going - the connection ended, the
     * settings section stopped being drawn, the test finished - is not an
     * event anything reports, so the microphone is released here, on the
     * first tick after there is nothing left to hold it open. */
    stopCaptureIfIdle();

    /* Reported after the open attempt, so a device that came up on this
     * tick is what the server hears about rather than last tick's absence
     * of one. */
    if (cs != NULL) {
        voiceReportState(cs);
    }

    /* Above the capture check: the test plays its recording back with the
     * microphone closed, which is the whole point of recording it first. */
    if (micTestState == VOICE_MICTEST_PLAYING) {
        micTestPlayTick();
    }

    /* Nothing accumulates while the microphone is unwanted - the recording
     * device is paused, so there is no backlog to drain here. */
    if (!wanted) {
        settingsWantsMic = false;
        return;
    }

    for (frame = 0; frame < VOICE_FRAMES_PER_TICK; frame++) {
#if defined(WB_VOICEDEBUG)
        clipped = 0;
        /* An injected file stands in for the recording device, so the same
         * signal can be put through the chain before and after a change.
         * The end of the file ends the run. */
        if (voiceDebugInjectIsOpen()) {
            if (!voiceDebugInjectRead(pcm)) {
                break;
            }
            got = VOICE_FRAME_SAMPLES;
        } else {
            got = voiceBackendCaptureRead(pcm);
        }
#else
        got = voiceBackendCaptureRead(pcm);
#endif
        if (got != VOICE_FRAME_SAMPLES) {
            break;
        }
        VOICE_STAT_INC(statsCaptured);

#if defined(WB_VOICEDEBUG)
        voiceDebugTap(VOICE_TAP_RAW, 0, pcm);
#endif

        sumSquares = 0.0f;
        for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
            sample = (float)pcm[i] * micGain;
#if defined(WB_VOICEDEBUG)
            if (sample > 32767.0f || sample < -32768.0f) {
                clipped++;
            }
#endif
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

#if defined(WB_VOICEDEBUG)
        voiceDebugTap(VOICE_TAP_GAINED, 0, pcm);
#endif

        gateLevel = inputLevel;
#if defined(WINBOLO_VOICE_AEC)
        /* Below the level meter, which reports the microphone as it is, and
         * above the encoder, which wants the frame cleaned and brought up to
         * a level listeners can hear.  Above the transmit test too: every
         * captured frame goes through, sent or not, because the filter tracks
         * the room continuously and a frame it never sees is a hole in that.
         *
         * The gate's reading comes back out of the call rather than off the
         * frame it wrote.  Echo the canceller is about to remove must not
         * open the microphone, and the automatic gain that runs in the same
         * call would leave any reading taken afterwards saying only that the
         * gain had reached its target. */
        gateLevel = voiceAecProcess(pcm, pcm);
#endif

#if defined(WB_VOICEDEBUG)
        /* Written whether or not the canceller is built.  Without it this is
         * the same frame as gained, and writing it anyway keeps the set of
         * files the same shape either way. */
        voiceDebugTap(VOICE_TAP_CLEANED, 0, pcm);
        /* The gained level beside the gate's, so a recording shows what the
         * automatic gain did to the frame the decision is no longer read
         * off.  Where no canceller is built the two are the same number. */
        gainedLevel = voiceFrameRms(pcm, VOICE_FRAME_SAMPLES);
#endif

        /* Open mic decides on the cancelled signal rather than on what the
         * meter reports, so echo the canceller has just taken out cannot open
         * the microphone, and on it before the automatic gain, so a quiet
         * room stays a quiet room to the decision.  Over the threshold opens
         * the gate and re-arms the hangover, under it counts the hangover
         * down so the tail of a word is not cut off. */
        if (voiceMode == VOICE_MODE_OPEN) {
            if (gateLevel >= VOICE_OPEN_MIC_RMS_THRESHOLD) {
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
        if (gateOpen) VOICE_STAT_INC(statsMicOpen);
        if (sending) VOICE_STAT_INC(statsSending);

        /* Transmission has just stopped, so tell the listeners the utterance
         * ended.  Without it they have nothing to separate a pause from a
         * talker who has finished, and each utterance ends in
         * VOICE_JITTER_MAX_PLC concealed frames before the buffer gives up.
         *
         * The marker carries silence rather than the frame in hand because
         * transmission also stops when the player mutes themselves, and
         * sending what was just captured would put 20 ms of their audio on
         * the wire after they muted.
         *
         * It is a hint, not a guarantee.  Lost, evicted, or never sent at
         * all - the connection went away, or capture stopped and this loop
         * is not reached - and the receiver still tears itself down after
         * its run of concealed frames, exactly as it did before. */
        if (!sending && wasSending) {
            int16_t silence[VOICE_FRAME_SAMPLES];
            uint8_t marker[VOICE_MAX_PACKET];
            int markerLen;

            memset(silence, 0, sizeof(silence));
            markerLen = voiceEncoderEncode(encoder, silence, marker,
                                           (int)sizeof(marker));
            /* Silence encodes far under the segment limit, so a length past
             * it means something is wrong with the encoder and the marker is
             * not worth sending. */
            if (markerLen > 0 && markerLen <= CLIENT_VOICE_MAX_FRAME_BYTES) {
                clientSimNetSendVoice(cs, marker, markerLen,
                                      VOICE_FLAG_END_OF_UTTERANCE);
            }
        }
        wasSending = sending;

        /* Between words in push-to-talk, and with the microphone test not
         * recording, the frame is only worth its level reading - which is
         * already taken.  Encoding it would be work nobody consumes. */
        if (!sending && micTestState != VOICE_MICTEST_RECORDING
#if defined(WB_VOICEDEBUG)
            /* A recording encodes every captured frame, sent or not, so the
             * round-trip file and the encoded length in frames.csv cover the
             * whole run. */
            && !voiceDebugIsRecording()
#endif
        ) {
            continue;
        }

        /* One encode feeds both consumers. */
        encodedLen = voiceEncoderEncode(encoder, pcm, packet, (int)sizeof(packet));
        if (encodedLen > 0) VOICE_STAT_INC(statsEncoded);
        if (encodedLen <= 0) {
#if defined(WB_VOICEDEBUG)
            /* Recorded before the frame is dropped: a frame that would not
             * encode is one a hole in the audio is found in. */
            voiceDebugFrameStats(voiceBackendNowMs(), inputLevel, gateLevel,
                                 gainedLevel, clipped, gateOpen, sending, 0,
                                 false, selfMuteRequested,
                                 connectionCarriesVoice);
#endif
            continue;
        }

#if defined(WB_VOICEDEBUG)
        voiceDebugRoundtrip(packet, encodedLen);
#endif

        if (sending) {
            if (encodedLen > CLIENT_VOICE_MAX_FRAME_BYTES) {
                /* Too big for one segment.  Sending a piece of it would
                 * decode to noise, so drop the frame. */
                VOICE_STAT_INC(statsTooLarge);
                if (!warnedFrameTooLarge) {
                    fprintf(stderr,
                            "Voice error: %d byte frame exceeds the %d byte "
                            "limit, dropping\n",
                            encodedLen, CLIENT_VOICE_MAX_FRAME_BYTES);
                    fflush(stderr);
                    warnedFrameTooLarge = true;
                }
            } else {
                clientSimNetSendVoice(cs, packet, encodedLen, 0);
                VOICE_STAT_INC(statsQueued);
                voiceStatsRecordSent(packet, encodedLen);
            }
        }

#if defined(WB_VOICEDEBUG)
        voiceDebugFrameStats(voiceBackendNowMs(), inputLevel, gateLevel,
                             gainedLevel, clipped, gateOpen, sending,
                             encodedLen,
                             encodedLen > CLIENT_VOICE_MAX_FRAME_BYTES,
                             selfMuteRequested, connectionCarriesVoice);
#endif

        /* Kept at the same size a frame may be on the wire, so the test hears
         * what a listener would.  A frame over that is dropped here exactly as
         * the send above drops it. */
        if (micTestState == VOICE_MICTEST_RECORDING &&
            encodedLen <= CLIENT_VOICE_MAX_FRAME_BYTES) {
            memcpy(micTestFrames[micTestRecorded], packet, (size_t)encodedLen);
            micTestLengths[micTestRecorded] = (uint8_t)encodedLen;
            micTestRecorded++;
            if (micTestRecorded == VOICE_MICTEST_FRAMES) {
                micTestState = VOICE_MICTEST_PLAYING;
                micTestPlayed = 0;
                /* The recording is done, so the test has no further use for
                 * the microphone.  This closes it unless something else still
                 * wants it, and clears the level meter with it. */
                stopCaptureIfIdle();
            }
        }
    }

    /* The settings voice section asks again on every frame it draws, so
     * taking the asking back here is what makes it lapse: a section that has
     * stopped being drawn is not read as still asking on the next tick. */
    settingsWantsMic = false;
}
