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
 * Name:          Voice Echo Canceller
 * Filename:      voice_aec.h
 * Purpose:
 *   Takes the other players' voices back out of this
 *   client's microphone, so someone on loudspeakers with an
 *   open microphone does not send everyone their own voice
 *   a moment later.
 *
 *   Cancelling needs a copy of what the loudspeakers are
 *   about to carry, lined up with the moment the microphone
 *   hears it.  Playback runs ahead of capture, so the caller
 *   hands each frame over as it queues it, saying how much
 *   is already queued in front of it, and this module holds
 *   the frame in a delay line until the tick it belongs to.
 *
 *   Desktop only.  The browser build gets echo cancellation
 *   from getUserMedia and never compiles this file, so every
 *   call site is behind WINBOLO_VOICE_AEC.
 *
 *   Main thread only, like the rest of the voice runtime.
 *********************************************************/

#ifndef VOICE_AEC_H
#define VOICE_AEC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
*NAME:          voiceAecInit
*PURPOSE:
*  Creates the canceller.  Returns false if it could not be
*  had, which is not fatal: every other entry point becomes
*  a no-op and voice runs on uncancelled audio.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecInit(void);

/*********************************************************
*NAME:          voiceAecShutdown
*PURPOSE:
*  Releases the canceller.  Safe to call when init failed or
*  never ran.  Leaves the on/off setting alone - that is the
*  player's, not the device's.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceAecShutdown(void);

/*********************************************************
*NAME:          voiceAecSetEnabled
*PURPOSE:
*  Switches cancellation on or off.  Switching it back on
*  starts the filter over, since nothing was fed to it while
*  it was off.
*
*ARGUMENTS:
*  on - whether captured audio is cancelled
*********************************************************/
void voiceAecSetEnabled(bool on);

/*********************************************************
*NAME:          voiceAecIsEnabled
*PURPOSE:
*  Whether cancellation is switched on.  This is the setting
*  the player chose, not whether a canceller could be
*  created, so it saves and restores either way.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecIsEnabled(void);

/*********************************************************
*NAME:          voiceAecAddReference
*PURPOSE:
*  Sums one frame that is about to be played into the delay
*  line, at the tick the microphone will hear it.  Several
*  talkers land on the same tick, so the frames sum.
*
*ARGUMENTS:
*  pcm                 - VOICE_FRAME_SAMPLES mono S16
*                        samples, at the level they will be
*                        played at
*  playbackDelayFrames - whole frames already queued ahead
*                        of this one on that talker
*********************************************************/
void voiceAecAddReference(const int16_t *pcm, int playbackDelayFrames);

/*********************************************************
*NAME:          voiceAecProcess
*PURPOSE:
*  Runs one captured frame through the canceller and moves
*  the delay line on a tick.  Call it on every captured
*  frame, transmitted or not - the filter tracks the room
*  continuously and a frame it never sees is a hole in that.
*  With cancellation off the input is copied out unchanged.
*
*ARGUMENTS:
*  micIn - VOICE_FRAME_SAMPLES mono S16 samples
*  out   - VOICE_FRAME_SAMPLES mono S16 samples; may be the
*          same buffer as micIn
*********************************************************/
void voiceAecProcess(const int16_t *micIn, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_AEC_H */
