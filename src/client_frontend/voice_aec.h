/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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
 *   Also cleans up the captured frame on its own account -
 *   noise suppression and automatic gain, so a microphone
 *   left at its default settings still reaches the encoder
 *   at a usable level.  That part runs on every platform,
 *   including the ones whose OS cancels the echo for us and
 *   where no canceller of ours is created at all.
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
*  Creates the preprocessor, and a canceller as well unless
*  the platform is already cancelling.  Returns whether
*  cancellation is usable - false if a canceller was wanted
*  and could not be had, which is not fatal: voice runs on
*  uncancelled audio, still cleaned by the preprocessor.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecInit(void);

/*********************************************************
*NAME:          voiceAecShutdown
*PURPOSE:
*  Releases the preprocessor and the canceller.  Safe to call
*  when init failed or never ran, or when only one of the two
*  was created.  Leaves the on/off setting alone - that is the
*  player's, not the device's.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceAecShutdown(void);

/*********************************************************
*NAME:          voiceAecSetEnabled
*PURPOSE:
*  Switches cancellation on or off, and only cancellation -
*  the noise suppression and automatic gain go on running
*  either way.  Starts the filter over in both directions,
*  since nothing is fed to it while it is off.
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
*NAME:          voiceAecIsAvailable
*PURPOSE:
*  Whether something is cancelling: voiceAecInit succeeded and
*  voiceAecShutdown has not run since.  That something is
*  Speex, or on a platform that cancels for us it is the
*  platform - either way the work is being done.  Separate
*  from voiceAecIsEnabled because that one is the player's
*  setting and stays what they chose whether or not a
*  canceller came up, so it cannot answer this and must not be
*  changed to.  For the UI, which has both to report: switched
*  on, and running.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecIsAvailable(void);

/*********************************************************
*NAME:          voiceAecIsPlatform
*PURPOSE:
*  Whether the cancellation voiceAecIsAvailable reports is the
*  platform's own rather than this module's.  When it is, no
*  Speex canceller was created: cancelling an already
*  cancelled signal damages the speech in it rather than
*  cleaning it further.  The preprocessor still was.  False
*  both when Speex is doing the cancelling and when nothing
*  is, so it answers "which", not "whether".
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecIsPlatform(void);

/*********************************************************
*NAME:          voiceAecReset
*PURPOSE:
*  Throws away what the filter has learned and empties the
*  delay line, so the next captured frame starts a fresh
*  convergence.  For the times the reference stops
*  describing the room - a gap in what was fed in, or a
*  reference that was never the room to begin with.  Safe to
*  call when the canceller was never created.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceAecReset(void);

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
*  Cleans one captured frame and moves the delay line on a
*  tick.  Call it on every captured frame, transmitted or not
*  - the filter tracks the room continuously and a frame it
*  never sees is a hole in that.  With cancellation off, or on
*  a platform where none was created, the frame still goes
*  through the noise suppression and the automatic gain, so
*  what comes out is not what went in.
*
*  Returns the frame's 0..1 RMS taken after the cancelling and
*  before the automatic gain, which is a different number from
*  the level of the frame it writes out.  A caller deciding
*  whether it is hearing speech has to compare against this
*  one: the automatic gain drives every frame towards a fixed
*  target, so a quiet room downstream of it reads much like a
*  talker and an absolute threshold there measures nothing.
*  Handed back rather than left to the caller to take off the
*  output, because this is the only point the frame exists in
*  that state - the two SpeexDSP stages run in one call, and
*  the preprocessor has to see every captured frame.
*
*ARGUMENTS:
*  micIn - VOICE_FRAME_SAMPLES mono S16 samples
*  out   - VOICE_FRAME_SAMPLES mono S16 samples; may be the
*          same buffer as micIn
*********************************************************/
float voiceAecProcess(const int16_t *micIn, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_AEC_H */
