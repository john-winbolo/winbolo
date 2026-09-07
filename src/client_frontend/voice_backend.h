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
 * Name:          Voice Backend
 * Filename:      voice_backend.h
 * Purpose:
 *   The capture and playback device contract each client
 *   platform implements. voice_client.c holds the whole
 *   client voice runtime - the speaker table, the mute
 *   list, transmit gating, the wire legs and the pacing -
 *   and reaches the audio device only through here, so a
 *   platform supplies a driver rather than its own copy of
 *   the runtime.
 *
 *   Backends: src/gui/sdl3/voice.c (SDL3),
 *   src/wasm/voice_wasm.c (web).
 *
 *   Every entry point is called on the main thread. A
 *   backend that needs an audio thread of its own owns the
 *   crossing; nothing here may block.
 *
 *   PCM throughout is mono signed 16 bit at
 *   VOICE_SAMPLE_RATE, in whole VOICE_FRAME_SAMPLES frames.
 *********************************************************/

#ifndef VOICE_BACKEND_H
#define VOICE_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
*NAME:          voiceBackendInit
*PURPOSE:
*  Brings playback up and reports whether voice is usable.
*
*  Must not open a capture device: opening one is what
*  raises the OS microphone-permission prompt, and a player
*  who never turns voice on must never be prompted.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendInit(void);

/*********************************************************
*NAME:          voiceBackendShutdown
*PURPOSE:
*  Releases the capture and playback devices. Safe to call
*  when init failed or never ran.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendShutdown(void);

/*********************************************************
*NAME:          voiceBackendCaptureStart
*PURPOSE:
*  Opens the capture device on the first call and starts
*  capturing. Returns false when there is no device or the
*  user refused the microphone. Idempotent: calling it while
*  already capturing is not an error and does not re-prompt.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureStart(void);

/*********************************************************
*NAME:          voiceBackendCaptureStop
*PURPOSE:
*  Stops capturing and discards whatever is buffered on both
*  sides, so re-enabling does not replay audio recorded
*  while it was off.
*
*  Does not close the device - re-opening it would prompt
*  for the microphone a second time.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendCaptureStop(void);

/*********************************************************
*NAME:          voiceBackendCaptureIsOpen
*PURPOSE:
*  Returns whether a capture device has been opened and is
*  still alive. This is "a microphone exists", not
*  "capturing right now": it stays true across a
*  start/stop cycle. The mic status other players see is
*  derived from it.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureIsOpen(void);

/*********************************************************
*NAME:          voiceBackendOsCancelsEcho
*PURPOSE:
*  Whether the platform is already cancelling echo on the
*  voice capture device, so the software canceller stands
*  down rather than processing the same signal twice.
*  Cancelling once is cleaning up a microphone; cancelling
*  the same signal twice damages the speech in it.
*
*  A backend that cannot tell must answer false.  Running
*  Speex needlessly is a far smaller harm than running
*  nothing at all.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendOsCancelsEcho(void);

/*********************************************************
*NAME:          voiceBackendCaptureRead
*PURPOSE:
*  Takes the next captured frame. Writes exactly
*  VOICE_FRAME_SAMPLES samples to pcm and returns that
*  count, or returns 0 when a whole frame is not yet
*  available. Never a partial frame.
*
*ARGUMENTS:
*  pcm - filled with VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
int voiceBackendCaptureRead(int16_t *pcm);

/*********************************************************
*NAME:          voiceBackendSpeakerOpen
*PURPOSE:
*  Brings up one remote talker's playback, on the first
*  frame heard from them, and reports whether they can be
*  played. Talkers mix: what is queued for one plays
*  alongside the others.
*
*ARGUMENTS:
*  player - the player number, a valid tank slot
*********************************************************/
bool voiceBackendSpeakerOpen(int player);

/*********************************************************
*NAME:          voiceBackendSpeakerClose
*PURPOSE:
*  Drops one remote talker's playback along with whatever
*  they had queued, so playback stops where it is rather
*  than running to the end of what had already arrived.
*  Safe on a talker who was never opened.
*
*ARGUMENTS:
*  player - the player number, a valid tank slot
*********************************************************/
void voiceBackendSpeakerClose(int player);

/*********************************************************
*NAME:          voiceBackendSpeakerQueuedFrames
*PURPOSE:
*  Returns the whole 20 ms frames still queued for one
*  talker, i.e. handed over but not yet played. In frames,
*  not bytes - the byte conversion is the backend's
*  business. 0 for a talker who was never opened.
*
*ARGUMENTS:
*  player - the player number, a valid tank slot
*********************************************************/
int voiceBackendSpeakerQueuedFrames(int player);

/*********************************************************
*NAME:          voiceBackendSpeakerPlay
*PURPOSE:
*  Queues exactly one VOICE_FRAME_SAMPLES frame for one
*  talker.
*
*ARGUMENTS:
*  player - the player number, a valid tank slot
*  pcm    - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendSpeakerPlay(int player, const int16_t *pcm);

/*********************************************************
*NAME:          voiceBackendLoopbackQueuedFrames
*PURPOSE:
*  Returns the whole 20 ms frames still queued on the local
*  microphone loopback test, i.e. handed over but not yet
*  played - the loopback's answer to
*  voiceBackendSpeakerQueuedFrames. In frames, not bytes.
*  0 when there is no loopback output open.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voiceBackendLoopbackQueuedFrames(void);

/*********************************************************
*NAME:          voiceBackendLoopbackPlay
*PURPOSE:
*  Queues exactly one VOICE_FRAME_SAMPLES frame of the
*  local microphone loopback test.
*
*ARGUMENTS:
*  pcm - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendLoopbackPlay(const int16_t *pcm);

/*********************************************************
*NAME:          voiceBackendLoopbackClear
*PURPOSE:
*  Discards whatever the loopback test still has queued, so
*  switching it off stops it where it is.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendLoopbackClear(void);

/*********************************************************
*NAME:          voiceBackendRecordingDeviceCount
*PURPOSE:
*  Captures the microphones present and returns how many
*  there are, for indexing voiceBackendRecordingDeviceName.
*
*  A snapshot, not a live list: devices come and go, and what
*  the name calls answer with is what was there at this call.
*  0 where the platform gives no choice of device at all,
*  which is what tells a caller to leave the control out
*  rather than draw an empty one.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voiceBackendRecordingDeviceCount(void);

/*********************************************************
*NAME:          voiceBackendPlaybackDeviceCount
*PURPOSE:
*  The same for the playback devices, over its own snapshot -
*  counting one direction does not disturb the other.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voiceBackendPlaybackDeviceCount(void);

/*********************************************************
*NAME:          voiceBackendRecordingDeviceName
*PURPOSE:
*  Returns the display name of one microphone out of the
*  snapshot the last voiceBackendRecordingDeviceCount took,
*  or NULL when index is outside it.
*
*  The pointer is the backend's own and stays good until the
*  next count of the same direction, which replaces the
*  snapshot.  Nothing SDL owns is handed back.
*
*ARGUMENTS:
*  index - 0..count-1 from voiceBackendRecordingDeviceCount
*********************************************************/
const char *voiceBackendRecordingDeviceName(int index);

/*********************************************************
*NAME:          voiceBackendPlaybackDeviceName
*PURPOSE:
*  The same for one playback device, over the playback
*  snapshot.
*
*ARGUMENTS:
*  index - 0..count-1 from voiceBackendPlaybackDeviceCount
*********************************************************/
const char *voiceBackendPlaybackDeviceName(int index);

/*********************************************************
*NAME:          voiceBackendSetRecordingDevice
*PURPOSE:
*  Chooses which microphone to capture from, by display name.
*  NULL or "" is the system default and always succeeds; any
*  other name is resolved against the devices present, and a
*  name that is not among them returns false having changed
*  nothing.
*
*  A call that succeeds and changes the choice closes and
*  reopens whatever streams are open on the old device.  The
*  backend does that itself - a caller does not orchestrate
*  it - and reopening a microphone can put the operating
*  system's permission prompt back up.
*
*ARGUMENTS:
*  name - the display name, or NULL/"" for the system default
*********************************************************/
bool voiceBackendSetRecordingDevice(const char *name);

/*********************************************************
*NAME:          voiceBackendSetPlaybackDevice
*PURPOSE:
*  The same for playback, and on the same terms.  Every
*  stream on the old device is reopened, remote talkers and
*  the microphone test alike; whatever any of them still had
*  queued is dropped with the stream it was queued on.
*
*ARGUMENTS:
*  name - the display name, or NULL/"" for the system default
*********************************************************/
bool voiceBackendSetPlaybackDevice(const char *name);

/*********************************************************
*NAME:          voiceBackendGetRecordingDevice
*PURPOSE:
*  Returns the resolved choice - the microphone being opened
*  on - or "" for the system default.  Never NULL.
*
*  This is not the name the player asked for: a chosen device
*  that is not present was never taken, so it does not read
*  back here.  Whoever asked holds that.
*
*ARGUMENTS:
*  (none)
*********************************************************/
const char *voiceBackendGetRecordingDevice(void);

/*********************************************************
*NAME:          voiceBackendGetPlaybackDevice
*PURPOSE:
*  The same for playback.
*
*ARGUMENTS:
*  (none)
*********************************************************/
const char *voiceBackendGetPlaybackDevice(void);

/*********************************************************
*NAME:          voiceBackendNowMs
*PURPOSE:
*  Monotonic milliseconds since some fixed point in this
*  process. Only the difference between two calls means
*  anything - the origin does not. Never steps backwards, so
*  it is not the wall clock.
*
*  The runtime clocks anything that has to track real time
*  off this rather than off how often it is called: nothing
*  guarantees voiceTick runs at the wire cadence.
*
*  Wraps roughly every 49 days; compare with a signed
*  difference rather than <.
*
*ARGUMENTS:
*  (none)
*********************************************************/
uint32_t voiceBackendNowMs(void);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_BACKEND_H */
