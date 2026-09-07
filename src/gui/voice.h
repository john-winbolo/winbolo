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
*Filename:      voice.h
*Author:        John Morrison
*Purpose:
*  System specific voice capture and playback.
*  (SDL3 backend: src/gui/sdl3/voice.c)
*********************************************************/

#ifndef VOICE_H
#define VOICE_H

#include <stdbool.h>

#include "platform_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

/* How captured audio reaches the other players.
 *  OFF  - never transmits; the microphone runs only for the microphone test.
 *  PTT  - transmits while the push-to-talk key is held. The default.
 *  OPEN - transmits while the captured level is above the gate. */
typedef enum {
    VOICE_MODE_OFF = 0,
    VOICE_MODE_PTT = 1,
    VOICE_MODE_OPEN = 2
} VoiceMode;

/* Where the microphone test has got to.
 *  IDLE      - not running; the test wants nothing.
 *  RECORDING - the microphone is open and frames are being kept.
 *  PLAYING   - what was kept is being played back to the speakers. */
typedef enum {
    VOICE_MICTEST_IDLE = 0,
    VOICE_MICTEST_RECORDING,
    VOICE_MICTEST_PLAYING
} VoiceMicTestState;

/*********************************************************
*NAME:          voiceInit
*PURPOSE:
*  Opens the playback audio stream and creates the codec.
*  The capture stream is left until the microphone is first
*  wanted, so starting the game does not raise the OS
*  microphone-permission prompt. A device that will not open
*  is not fatal: the module stays disabled and every other
*  entry point becomes a no-op.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceInit(void);

/*********************************************************
*NAME:          voiceCleanup
*PURPOSE:
*  Destroys the audio streams and the codec. Safe to call
*  when voiceInit failed or was never called.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceCleanup(void);

/*********************************************************
*NAME:          voiceMicTestStart
*PURPOSE:
*  Starts the microphone test: records a few seconds and
*  then plays them back. Only starts from idle, and only
*  when the master switch is on and the recording device
*  opens; otherwise it stays idle.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceMicTestStart(void);

/*********************************************************
*NAME:          voiceMicTestCancel
*PURPOSE:
*  Stops the microphone test wherever it has got to and
*  returns it to idle, dropping what was recorded and
*  whatever of it has not been played yet.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceMicTestCancel(void);

/*********************************************************
*NAME:          voiceMicTestGetState
*PURPOSE:
*  Returns where the microphone test has got to.
*
*ARGUMENTS:
*  (none)
*********************************************************/
VoiceMicTestState voiceMicTestGetState(void);

/*********************************************************
*NAME:          voiceMicTestProgress
*PURPOSE:
*  Returns how far through the current phase the microphone
*  test is, as 0..1, for a progress bar. Zero when idle.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceMicTestProgress(void);

/*********************************************************
*NAME:          voiceSetEnabled
*PURPOSE:
*  The master switch. With it off nothing is captured, sent
*  or played, whatever the mode and the microphone test say.
*  Switching off pauses the microphone but does not close it,
*  so switching back on does not ask for it a second time.
*
*ARGUMENTS:
*  on - true to allow voice, false to shut it all off
*********************************************************/
void voiceSetEnabled(bool on);

/*********************************************************
*NAME:          voiceIsEnabled
*PURPOSE:
*  Returns whether the master switch is on.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceIsEnabled(void);

/*********************************************************
*NAME:          voiceSetMode
*PURPOSE:
*  Chooses how captured audio reaches the other players.
*  Opens the microphone if a transmitting mode is chosen and
*  this is the first ask, and pauses it once no mode and no
*  microphone test wants it. A mode change never leaves the
*  microphone latched open.
*
*ARGUMENTS:
*  mode - one of the VoiceMode values
*********************************************************/
void voiceSetMode(VoiceMode mode);

/*********************************************************
*NAME:          voiceGetMode
*PURPOSE:
*  Returns how captured audio reaches the other players.
*
*ARGUMENTS:
*  (none)
*********************************************************/
VoiceMode voiceGetMode(void);

/*********************************************************
*NAME:          voiceSetPushToTalkHeld
*PURPOSE:
*  Tells the runtime whether the push-to-talk key is held
*  right now. Called once per input poll by the input layer,
*  which is also what decides that a key held while a dialog
*  has the keyboard does not count.
*
*ARGUMENTS:
*  held - true while the key is down
*********************************************************/
void voiceSetPushToTalkHeld(bool held);

/*********************************************************
*NAME:          voiceSetSelfMuted
*PURPOSE:
*  Sets the player's own transmit gate. Nothing goes out
*  while it is on, whatever the mode or the push-to-talk
*  key, and the state reported to the server follows on the
*  next tick. The recording device stays open so the echo
*  canceller keeps tracking the room.
*
*ARGUMENTS:
*  muted - true to stop transmitting
*********************************************************/
void voiceSetSelfMuted(bool muted);

/*********************************************************
*NAME:          voiceIsSelfMuted
*PURPOSE:
*  Returns whether the player has muted themselves. The
*  toggle alone - the settings that can also stop audio
*  reaching the wire are not folded in, so whatever drives
*  the toggle reads back what it set.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceIsSelfMuted(void);

/*********************************************************
*NAME:          voiceIsTransmitting
*PURPOSE:
*  Returns whether captured audio is going out right now -
*  the master switch, the mode, the push-to-talk key, the
*  open-mic gate and whether this connection carries our
*  voice at all, all folded in.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceIsTransmitting(void);

/*********************************************************
*NAME:          voiceReset
*PURPOSE:
*  Forgets every remote talker, releasing their decoders and
*  playback streams. Called when a connection ends so the
*  next one does not inherit them.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceReset(void);

/*********************************************************
*NAME:          voiceForgetPlayer
*PURPOSE:
*  Drops everything held about one player: their decoder and
*  playback stream, their talking indicator, and the local
*  mute on them. Called when a player leaves, because slots
*  are recycled — without it the next joiner into that slot
*  inherits the mute the departed player was given, and is
*  silenced here with nothing to show for it but a "muted by
*  you" icon on a player nobody muted.
*
*ARGUMENTS:
*  player - the player number to forget
*********************************************************/
void voiceForgetPlayer(int player);

/*********************************************************
*NAME:          voiceSetPlayerMuted
*PURPOSE:
*  Stops or resumes playing one player's voice locally.
*  The server is the authority — this covers the round trip
*  while it is being told. Muting releases that player's
*  decoder and playback stream, so whatever was already
*  buffered stops rather than finishing.
*
*ARGUMENTS:
*  player - the player number to mute
*  muted  - true to mute, false to unmute
*********************************************************/
void voiceSetPlayerMuted(int player, bool muted);

/*********************************************************
*NAME:          voiceIsPlayerMuted
*PURPOSE:
*  Returns whether one player is muted locally.
*
*ARGUMENTS:
*  player - the player number to ask about
*********************************************************/
bool voiceIsPlayerMuted(int player);

/*********************************************************
*NAME:          voiceGetTalkingMap
*PURPOSE:
*  Returns the players whose voice has been heard here in
*  the last moment, bit N set for player N.
*
*  Derived locally from frames arriving rather than read off
*  a wire flag: it is free, it always agrees with what can
*  actually be heard, and it says nothing about players
*  whose voice does not reach this client. A player muted
*  locally is never in it.
*
*  Decays against a real clock, so it is correct however
*  often - or seldom - it is asked.
*
*ARGUMENTS:
*  (none)
*********************************************************/
PlayerBitMap voiceGetTalkingMap(void);

/*********************************************************
*NAME:          voiceSetMicGain
*PURPOSE:
*  Sets the gain applied to captured audio before encoding.
*
*ARGUMENTS:
*  gain - 1.0f is unity
*********************************************************/
void voiceSetMicGain(float gain);

/*********************************************************
*NAME:          voiceGetMicGain
*PURPOSE:
*  Returns the gain applied to captured audio.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceGetMicGain(void);

/*********************************************************
*NAME:          voiceSetOutputVolume
*PURPOSE:
*  Sets the gain applied to decoded remote audio before it
*  is played. Clamped at zero; 1.0f is unity.
*
*ARGUMENTS:
*  gain - 1.0f is unity
*********************************************************/
void voiceSetOutputVolume(float gain);

/*********************************************************
*NAME:          voiceGetOutputVolume
*PURPOSE:
*  Returns the gain applied to decoded remote audio.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceGetOutputVolume(void);

/*********************************************************
*NAME:          voiceGetInputLevel
*PURPOSE:
*  Returns the 0..1 RMS of the most recently captured frame,
*  for the input level meter.
*
*ARGUMENTS:
*  (none)
*********************************************************/
float voiceGetInputLevel(void);

/*********************************************************
*NAME:          voiceRecordingDeviceCount
*PURPOSE:
*  Returns how many microphones there are to choose between,
*  taken fresh at the call. Zero where the platform gives no
*  choice, which is the settings dialog's cue to leave the
*  control out.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voiceRecordingDeviceCount(void);

/*********************************************************
*NAME:          voiceRecordingDeviceName
*PURPOSE:
*  Returns one microphone's display name out of the list the
*  last voiceRecordingDeviceCount took, or NULL when index is
*  outside it. Good until the next count.
*
*ARGUMENTS:
*  index - 0..count-1
*********************************************************/
const char *voiceRecordingDeviceName(int index);

/*********************************************************
*NAME:          voicePlaybackDeviceCount
*PURPOSE:
*  The same for the playback devices, over a list of their
*  own.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voicePlaybackDeviceCount(void);

/*********************************************************
*NAME:          voicePlaybackDeviceName
*PURPOSE:
*  The same for one playback device.
*
*ARGUMENTS:
*  index - 0..count-1
*********************************************************/
const char *voicePlaybackDeviceName(int index);

/*********************************************************
*NAME:          voiceSetRecordingDevice
*PURPOSE:
*  Chooses the microphone by display name. NULL or "" means
*  the system default, and so does a name no device present
*  answers to - capture falls back rather than going silent.
*
*  The device the player chose is kept verbatim even when it
*  is not present, so a headset that is unplugged today is
*  still chosen when it comes back. A name longer than the
*  buffer is truncated.
*
*ARGUMENTS:
*  name - the display name, or NULL/"" for the system default
*********************************************************/
void voiceSetRecordingDevice(const char *name);

/*********************************************************
*NAME:          voiceGetRecordingDevice
*PURPOSE:
*  Returns the microphone the player chose, "" for the system
*  default. Never NULL.
*
*  What was asked for, not what is open: an absent device
*  still reads back, which is what keeps it in the prefs file
*  instead of being overwritten with whatever stood in.
*
*ARGUMENTS:
*  (none)
*********************************************************/
const char *voiceGetRecordingDevice(void);

/*********************************************************
*NAME:          voiceSetPlaybackDevice
*PURPOSE:
*  Chooses the playback device by display name, on the same
*  terms as voiceSetRecordingDevice.
*
*ARGUMENTS:
*  name - the display name, or NULL/"" for the system default
*********************************************************/
void voiceSetPlaybackDevice(const char *name);

/*********************************************************
*NAME:          voiceGetPlaybackDevice
*PURPOSE:
*  Returns the playback device the player chose, "" for the
*  system default. Never NULL.
*
*ARGUMENTS:
*  (none)
*********************************************************/
const char *voiceGetPlaybackDevice(void);

/*********************************************************
*NAME:          voiceTick
*PURPOSE:
*  Pumps captured audio through the codec and out to the
*  server, and plays whatever arrived from the other
*  players. Called once per main loop iteration, on the
*  main thread.
*
*ARGUMENTS:
*  cs - the connected client, or NULL when there is no
*       network yet (the loopback test still runs)
*********************************************************/
void voiceTick(struct ClientSim *cs);

/*********************************************************
*NAME:          voiceReportState
*PURPOSE:
*  Report this client's mic status to the server when it
*  changes. Cheap to call every tick; it only sends on a
*  transition.
*
*ARGUMENTS:
*  cs - the connected client
*********************************************************/
void voiceReportState(struct ClientSim *cs);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_H */
