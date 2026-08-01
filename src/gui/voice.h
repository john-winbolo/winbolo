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

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

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
*NAME:          voiceLoopbackSetEnabled
*PURPOSE:
*  Starts or stops the microphone loopback test. While it
*  is on, voiceTick plays captured audio back through the
*  codec to the speakers.
*
*ARGUMENTS:
*  on - true to start capturing, false to stop
*********************************************************/
void voiceLoopbackSetEnabled(bool on);

/*********************************************************
*NAME:          voiceLoopbackIsEnabled
*PURPOSE:
*  Returns whether the microphone loopback test is running.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceLoopbackIsEnabled(void);

/*********************************************************
*NAME:          voiceTransmitSetEnabled
*PURPOSE:
*  Starts or stops sending captured audio to the other
*  players. Like the loopback test it opens the microphone
*  on first use and stays off if there is no device to open.
*  A viewer captures but never transmits.
*
*ARGUMENTS:
*  on - true to start transmitting, false to stop
*********************************************************/
void voiceTransmitSetEnabled(bool on);

/*********************************************************
*NAME:          voiceTransmitIsEnabled
*PURPOSE:
*  Returns whether captured audio is being transmitted.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceTransmitIsEnabled(void);

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
