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
 * Filename:      voice_aec.c
 * Purpose:
 *   SpeexDSP's echo canceller and preprocessor over the
 *   voice frame size, with the delay line that lines the
 *   reference up with the microphone.
 *
 *   The delay line is the substance of this file.  A frame
 *   handed to a talker's output stream is not heard now: it
 *   is heard once the frames already queued in front of it
 *   have played, and once the audio device has carried it
 *   out of the loudspeakers.  So the frame is summed into
 *   the ring slot for the tick it will be heard on, and the
 *   canceller reads the slot for the tick it is cancelling.
 *
 *   The Speex types stay inside this file so no consumer
 *   needs SpeexDSP on its include path.
 *********************************************************/

#include "voice_aec.h"

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <string.h>

#include "voice_core.h"

/* Samples of echo the filter can cancel - 200 ms at 48 kHz.  The plan's range
 * is 100-200 ms: a longer tail covers a more reverberant room and costs more
 * CPU per frame, and one mono canceller at 48 kHz is affordable on a desktop
 * where the whole voice path is 20 ms of work every 20 ms.  Taken at the top
 * of the range because the failure it guards against - a room whose echo
 * outlasts the filter - is the one the player hears. */
#define VOICE_AEC_FILTER_TAIL_SAMPLES (10 * VOICE_FRAME_SAMPLES)

/* Whole frames of playback path between handing a frame to the audio device
 * and the microphone hearing it come back: the device's own buffering plus
 * the trip through the air.  UNMEASURED - nothing here asks the backend what
 * its output latency is, and this is the first constant to move if the echo
 * is reduced but not gone.  One frame is 20 ms and is deliberately short of
 * what a desktop output path is likely to cost, because the two directions of
 * error are not alike: guessing low leaves the reference early, which the
 * filter tail above absorbs, while guessing high leaves it arriving after the
 * echo it is meant to cancel, which nothing can recover. */
#define VOICE_AEC_DEVICE_LATENCY_FRAMES 1

/* Slots in the reference delay line.  The playback pacer tops each talker up
 * to VOICE_PLAYBACK_TARGET_FRAMES, so the delays actually asked for today are
 * a frame or two plus the offset above; 16 slots is 320 ms, which leaves room
 * for a deeper playback target without this needing to be revisited. */
#define VOICE_AEC_REF_FRAMES 16

static SpeexEchoState *echoState = NULL;
static SpeexPreprocessState *preprocessState = NULL;

/* The player's setting, held whether or not a canceller exists, so it saves
 * and restores on a machine where the state could not be created. */
static bool aecEnabled = true;

/* One mono frame per tick of playback that the microphone has not heard yet.
 * refTick is the slot for the frame being captured now. */
static int16_t refRing[VOICE_AEC_REF_FRAMES][VOICE_FRAME_SAMPLES];
static int refTick = 0;

/*********************************************************
*NAME:          voiceAecInit
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Creates the canceller and the preprocessor that suppresses
*  what it leaves behind.  Returns whether cancellation is
*  usable.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecInit(void) {
    int rate = VOICE_SAMPLE_RATE;
    int denoise = 1;

    if (echoState != NULL) {
        return true;
    }

    echoState = speex_echo_state_init(VOICE_FRAME_SAMPLES,
                                      VOICE_AEC_FILTER_TAIL_SAMPLES);
    if (echoState == NULL) {
        return false;
    }
    /* The canceller derives its adaptation rates from the sampling rate, and
     * defaults to 8 kHz until it is told otherwise. */
    speex_echo_ctl(echoState, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);

    preprocessState = speex_preprocess_state_init(VOICE_FRAME_SAMPLES,
                                                  VOICE_SAMPLE_RATE);
    if (preprocessState == NULL) {
        speex_echo_state_destroy(echoState);
        echoState = NULL;
        return false;
    }
    /* Linking the echo state lets the preprocessor tell residual echo from
     * speech and suppress it; the noise suppression is free while we are
     * here, and the microphone is the only thing it sees. */
    speex_preprocess_ctl(preprocessState, SPEEX_PREPROCESS_SET_ECHO_STATE,
                         echoState);
    speex_preprocess_ctl(preprocessState, SPEEX_PREPROCESS_SET_DENOISE,
                         &denoise);

    memset(refRing, 0, sizeof(refRing));
    refTick = 0;
    return true;
}

/*********************************************************
*NAME:          voiceAecShutdown
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Releases the canceller.  The on/off setting survives, so
*  a shutdown between the settings dialog and the preference
*  save does not write back a value the player never chose.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceAecShutdown(void) {
    if (preprocessState != NULL) {
        speex_preprocess_state_destroy(preprocessState);
        preprocessState = NULL;
    }
    if (echoState != NULL) {
        speex_echo_state_destroy(echoState);
        echoState = NULL;
    }
    memset(refRing, 0, sizeof(refRing));
    refTick = 0;
}

/*********************************************************
*NAME:          voiceAecReset
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Drops the filter's adaptation and the delay line, so the
*  next captured frame converges from silence.  Does nothing
*  harmful if the canceller was never created.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceAecReset(void) {
    if (echoState != NULL) {
        speex_echo_state_reset(echoState);
    }
    memset(refRing, 0, sizeof(refRing));
    refTick = 0;
}

/*********************************************************
*NAME:          voiceAecSetEnabled
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Switches cancellation on or off.
*
*ARGUMENTS:
*  on - whether captured audio is cancelled
*********************************************************/
void voiceAecSetEnabled(bool on) {
    if (on == aecEnabled) {
        return;
    }
    aecEnabled = on;

    /* Nothing reached the filter or the delay line while it was off, so both
     * describe a room from before the gap.  Starting over makes it converge
     * from silence rather than from that. */
    if (on) {
        voiceAecReset();
    }
}

/*********************************************************
*NAME:          voiceAecIsEnabled
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Whether cancellation is switched on.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecIsEnabled(void) {
    return aecEnabled;
}

/*********************************************************
*NAME:          voiceAecIsAvailable
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Whether a canceller exists.  echoState is non-NULL exactly
*  when one does: every failing path in voiceAecInit leaves it
*  NULL and voiceAecShutdown clears it.  Also false when voice
*  as a whole failed to come up, since voiceInit returns before
*  reaching voiceAecInit if the codec could not be created -
*  which is what we want, because either way the honest thing
*  to tell the player is that cancellation is not running.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecIsAvailable(void) {
    return echoState != NULL;
}

/*********************************************************
*NAME:          voiceAecAddReference
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Sums one frame about to be played into the slot for the
*  tick the microphone will hear it on.
*
*ARGUMENTS:
*  pcm                 - VOICE_FRAME_SAMPLES mono S16 samples
*  playbackDelayFrames - whole frames already queued ahead of
*                        this one on that talker
*********************************************************/
void voiceAecAddReference(const int16_t *pcm, int playbackDelayFrames) {
    int ahead;
    int slot;
    int i;
    int32_t sum;

    if (!aecEnabled || echoState == NULL || pcm == NULL) {
        return;
    }
    if (playbackDelayFrames < 0) {
        playbackDelayFrames = 0;
    }

    ahead = playbackDelayFrames + VOICE_AEC_DEVICE_LATENCY_FRAMES;
    /* Further out than the ring reaches is further out than the filter tail
     * could line up with in any case.  Dropped rather than wrapped onto a
     * slot that belongs to a different moment. */
    if (ahead >= VOICE_AEC_REF_FRAMES) {
        return;
    }
    slot = (refTick + ahead) % VOICE_AEC_REF_FRAMES;

    /* Talkers sum: two people speaking at once are one sound in the room.
     * Saturating, because a sum that wrapped would be a reference bearing no
     * resemblance to what the microphone hears. */
    for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        sum = (int32_t)refRing[slot][i] + (int32_t)pcm[i];
        if (sum > 32767) {
            sum = 32767;
        } else if (sum < -32768) {
            sum = -32768;
        }
        refRing[slot][i] = (int16_t)sum;
    }
}

/*********************************************************
*NAME:          voiceAecProcess
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Cancels this tick's reference out of one captured frame
*  and moves the delay line on.
*
*ARGUMENTS:
*  micIn - VOICE_FRAME_SAMPLES mono S16 samples
*  out   - VOICE_FRAME_SAMPLES mono S16 samples, which may be
*          the same buffer as micIn
*********************************************************/
void voiceAecProcess(const int16_t *micIn, int16_t *out) {
    int16_t cleaned[VOICE_FRAME_SAMPLES];

    if (micIn == NULL || out == NULL) {
        return;
    }

    /* Switched off, or no canceller to switch on: the frame the encoder gets
     * is the frame the microphone gave, sample for sample. */
    if (!aecEnabled || echoState == NULL) {
        if (out != micIn) {
            memcpy(out, micIn, VOICE_FRAME_SAMPLES * sizeof(int16_t));
        }
        return;
    }

    /* Through a scratch buffer so the caller may pass one buffer for both:
     * the canceller reads the whole microphone frame while it writes its
     * output. */
    speex_echo_cancellation(echoState, micIn, refRing[refTick], cleaned);
    speex_preprocess_run(preprocessState, cleaned);
    memcpy(out, cleaned, sizeof(cleaned));

    /* Cleared before the tick moves on.  The slot comes round again in
     * VOICE_AEC_REF_FRAMES ticks and has to be empty when it does - a stale
     * reference cancels a sound the microphone never heard, which damages
     * speech rather than cleaning it. */
    memset(refRing[refTick], 0, sizeof(refRing[refTick]));
    refTick = (refTick + 1) % VOICE_AEC_REF_FRAMES;
}
