/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * voice_wasm.c — web implementation of voice_backend.h for the WASM build.
 *
 * Replaces gui/sdl3/voice.c on the WASM target.  The client voice runtime
 * itself is shared (client_frontend/voice_client.c) and reaches the audio
 * device only through the twelve functions below, so this file is the whole
 * of what the web build has to supply.
 *
 * There is no capture or playback here yet: microphone capture
 * (getUserMedia) and playback (AudioContext / AudioWorklet) arrive with the
 * browser audio work, along with the user-gesture and permission flow the
 * browser requires before either can start.  Until then this reports,
 * honestly, that the platform has no microphone and no output: the shared
 * runtime already handles that - the Opus encoder and decoder still come up,
 * the settings controls are reachable, and every capture and playback call
 * declines instead of pretending.
 */

#include "voice_backend.h"

/*********************************************************
*NAME:          voiceBackendInit
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Reports that voice is usable.  There is no device to open
*  on this platform yet and so nothing that can fail; saying
*  so is what lets the shared runtime go on to create the
*  codec, which is what proves Opus comes up under
*  Emscripten.  Bringing up the AudioContext will happen
*  here.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendInit(void) {
    return true;
}

/*********************************************************
*NAME:          voiceBackendShutdown
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Nothing has been opened, so there is nothing to release.
*  Closing the AudioContext and the capture stream will
*  happen here.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendShutdown(void) {
}

/*********************************************************
*NAME:          voiceBackendCaptureStart
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Declines to capture: this platform has no microphone
*  path yet.  The shared runtime treats that the same way it
*  treats a player refusing the permission prompt, so the
*  loopback and transmit switches stay off and the settings
*  checkboxes snap back.  getUserMedia, behind the user
*  gesture the browser requires, will happen here.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureStart(void) {
    return false;
}

/*********************************************************
*NAME:          voiceBackendCaptureStop
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Nothing is capturing, so there is nothing to stop and
*  nothing buffered to discard.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendCaptureStop(void) {
}

/*********************************************************
*NAME:          voiceBackendCaptureIsOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Reports no microphone, which is what this client has.
*  The mic status the other players see is derived from
*  this, so they are told the truth: a web player is heard
*  by nobody until the capture path exists.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureIsOpen(void) {
    return false;
}

/*********************************************************
*NAME:          voiceBackendCaptureRead
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Reports that no captured frame is available, which is
*  always the case while there is no microphone.  pcm is
*  left untouched.  Draining whole frames from the capture
*  worklet's ring will happen here.
*
*ARGUMENTS:
*  pcm - would be filled with VOICE_FRAME_SAMPLES mono S16
*        samples
*********************************************************/
int voiceBackendCaptureRead(int16_t *pcm) {
    (void)pcm;
    return 0;
}

/*********************************************************
*NAME:          voiceBackendSpeakerOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Declines to play a remote talker: there is no output
*  path yet.  The shared runtime drops that player's frames
*  rather than buffering audio nothing will ever drain.
*  Giving each talker a node on the AudioContext will happen
*  here.
*
*ARGUMENTS:
*  player - the player number the frame came from
*********************************************************/
bool voiceBackendSpeakerOpen(int player) {
    (void)player;
    return false;
}

/*********************************************************
*NAME:          voiceBackendSpeakerClose
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  No talker was ever opened, so there is nothing to
*  release.
*
*ARGUMENTS:
*  player - the player number to release
*********************************************************/
void voiceBackendSpeakerClose(int player) {
    (void)player;
}

/*********************************************************
*NAME:          voiceBackendSpeakerQueuedFrames
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Reports nothing queued for that talker, which is what an
*  unopened talker holds.
*
*ARGUMENTS:
*  player - the player number to ask about
*********************************************************/
int voiceBackendSpeakerQueuedFrames(int player) {
    (void)player;
    return 0;
}

/*********************************************************
*NAME:          voiceBackendSpeakerPlay
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Discards the frame: there is nowhere to play it.  This
*  is only reached if a talker was opened, which cannot
*  happen yet.  Queueing onto that talker's output node
*  will happen here.
*
*ARGUMENTS:
*  player - the player number the frame came from
*  pcm    - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendSpeakerPlay(int player, const int16_t *pcm) {
    (void)player;
    (void)pcm;
}

/*********************************************************
*NAME:          voiceBackendLoopbackPlay
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Discards the frame: there is nowhere to play it.  The
*  loopback test cannot run anyway while there is no
*  microphone to feed it.
*
*ARGUMENTS:
*  pcm - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendLoopbackPlay(const int16_t *pcm) {
    (void)pcm;
}

/*********************************************************
*NAME:          voiceBackendLoopbackClear
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Nothing was ever queued for the loopback test, so there
*  is nothing to drop.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendLoopbackClear(void) {
}
