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
 *   The two are separate pieces of work.  The canceller is
 *   only wanted where the OS is not already cancelling; the
 *   preprocessor's noise suppression and automatic gain are
 *   wanted everywhere, and a captured frame goes through it
 *   whether or not there is a canceller in front of it.
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

#include "voice_backend.h"
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

/* What the automatic gain aims captured speech at, on Speex's 0-32768 linear
 * sample scale: 8000 is around -12 dBFS RMS.  Speech off a default-configured
 * microphone measures 0.014-0.07 RMS here, 460-2300 on this scale, so this
 * asks for 11 to 25 dB.  Higher would run out of room for the peaks - speech
 * peaks around 11 dB above its own RMS, which already puts the loudest samples
 * of a -12 dBFS frame just short of full scale, and clipping in front of the
 * encoder costs more than the last few dB are worth. */
#define VOICE_AEC_AGC_TARGET_LEVEL 8000.0f

/* Ceiling on the gain the automatic control may apply, in dB.  Speex allows
 * about 30 by default, enough to lift a quiet room - measured at 0.0100 RMS,
 * 330 on the scale above - to a third of full scale between words, so the room
 * swells every time the talker stops.  20 dB holds it to a tenth of full scale
 * before the noise suppression takes its own 15 dB off that.  The cost is that
 * the quietest microphones land short of the target rather than bringing their
 * room up with them, which is the better of the two. */
#define VOICE_AEC_AGC_MAX_GAIN_DB 20

static SpeexEchoState *echoState = NULL;
static SpeexPreprocessState *preprocessState = NULL;

/* The player's setting, held whether or not a canceller exists, so it saves
 * and restores on a machine where the state could not be created. */
static bool aecEnabled = true;

/* Whether the platform is cancelling echo itself, answered once at init.  A
 * fact about the machine, not a setting: nothing the player does changes it,
 * and while it is true no Speex state exists to do the work. */
static bool platformCancels = false;

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
*  Creates the preprocessor, and the canceller where the
*  platform is not cancelling for us.  Returns whether
*  cancellation is usable.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecInit(void) {
    spx_int32_t rate = VOICE_SAMPLE_RATE;
    spx_int32_t denoise = 1;
    spx_int32_t agc = 1;
    float agcLevel = VOICE_AEC_AGC_TARGET_LEVEL;
    spx_int32_t agcMaxGain = VOICE_AEC_AGC_MAX_GAIN_DB;

    /* Asked before anything is created, because it decides whether a canceller
     * should be.  It decides that and nothing else: the preprocessor below is
     * wanted on every machine, since noise suppression and automatic gain have
     * nothing to do with who is cancelling the echo. */
    platformCancels = voiceBackendOsCancelsEcho();

    if (preprocessState != NULL) {
        return true;
    }

    /* Only where the OS is not doing it already: cancelling an already
     * cancelled signal damages the speech in it rather than cleaning it
     * further. */
    if (!platformCancels) {
        echoState = speex_echo_state_init(VOICE_FRAME_SAMPLES,
                                          VOICE_AEC_FILTER_TAIL_SAMPLES);
        if (echoState == NULL) {
            return false;
        }
        /* The canceller derives its adaptation rates from the sampling rate,
         * and defaults to 8 kHz until it is told otherwise. */
        speex_echo_ctl(echoState, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    }

    preprocessState = speex_preprocess_state_init(VOICE_FRAME_SAMPLES,
                                                  VOICE_SAMPLE_RATE);
    if (preprocessState == NULL) {
        if (echoState != NULL) {
            speex_echo_state_destroy(echoState);
            echoState = NULL;
        }
        /* True is the honest return on a platform that cancels: the question
         * is whether cancellation is usable, and it is - the OS is doing it.
         * Returning false would print an "echo canceller unavailable" warning
         * on exactly the machines where nothing is wrong.  Elsewhere the
         * canceller has just been destroyed with the preprocessor it feeds,
         * and nothing is cancelling. */
        return platformCancels;
    }

    /* Linking the echo state lets the preprocessor tell residual echo from
     * speech and suppress it.  Set only when there is one: the ctl reads NULL
     * as "no residual suppression", which is what a fresh state already says,
     * so passing it would be saying nothing. */
    if (echoState != NULL) {
        speex_preprocess_ctl(preprocessState, SPEEX_PREPROCESS_SET_ECHO_STATE,
                             echoState);
    }
    /* Noise suppression: the microphone is the only thing it sees, and it is
     * also what keeps the automatic gain below from bringing the room up with
     * the voice. */
    speex_preprocess_ctl(preprocessState, SPEEX_PREPROCESS_SET_DENOISE,
                         &denoise);
    /* Automatic gain, which is the reason the preprocessor runs on every
     * machine: a default-configured microphone hands the encoder a signal far
     * below what it should, and correcting it here rather than at the far end
     * means listeners are not amplifying codec artefacts and room noise along
     * with the voice. */
    speex_preprocess_ctl(preprocessState, SPEEX_PREPROCESS_SET_AGC, &agc);
    speex_preprocess_ctl(preprocessState, SPEEX_PREPROCESS_SET_AGC_LEVEL,
                         &agcLevel);
    speex_preprocess_ctl(preprocessState, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN,
                         &agcMaxGain);

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
*  Releases the preprocessor and, where one was created, the
*  canceller.  The on/off setting survives, so a shutdown
*  between the settings dialog and the preference save does
*  not write back a value the player never chose.
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
    platformCancels = false;
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

    /* Either direction, because either one leaves the filter describing a room
     * that is no longer the one being captured.  Switching on, nothing reached
     * it while it was off, so it would start from a room from before the gap.
     * Switching off, the preprocessor goes on reading it for its residual echo
     * estimate, and a filter that has stopped being fed holds that estimate at
     * whatever it last saw and suppresses speech that resembles it. */
    voiceAecReset();
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
*  Whether something is cancelling, whether that something is
*  Speex or the platform.  echoState is non-NULL exactly when
*  a Speex canceller exists: every failing path in voiceAecInit
*  leaves it NULL and voiceAecShutdown clears it.  Also false
*  when voice as a whole failed to come up, since voiceInit
*  returns before reaching voiceAecInit if the codec could not
*  be created - which is what we want, because either way the
*  honest thing to tell the player is that cancellation is not
*  running.
*
*  Its caller is the settings row, which must not report
*  "unavailable" on a machine where the work is being done,
*  merely by someone else.  Use voiceAecIsPlatform to tell the
*  two apart.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecIsAvailable(void) {
    return platformCancels || echoState != NULL;
}

/*********************************************************
*NAME:          voiceAecIsPlatform
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Whether the cancellation voiceAecIsAvailable reports is the
*  platform's rather than this file's.  False both when Speex
*  is doing the work and when nothing is; ask
*  voiceAecIsAvailable first.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceAecIsPlatform(void) {
    return platformCancels;
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
*  Cleans one captured frame: cancels this tick's reference
*  out of it where there is a canceller running, and runs the
*  preprocessor over it either way.
*
*  aecEnabled and the absence of a canceller both stop the
*  cancelling only.  The setting is labelled echo cancellation
*  in the dialog and that is all it governs, so noise
*  suppression and automatic gain go on running with it off -
*  which is also the case on every platform whose OS cancels
*  for us, where no canceller is ever created.  Only a
*  preprocessor that could not be created leaves the encoder
*  the frame the microphone gave, sample for sample.
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

    if (aecEnabled && echoState != NULL) {
        /* Through a scratch buffer so the caller may pass one buffer for
         * both: the canceller reads the whole microphone frame while it
         * writes its output. */
        speex_echo_cancellation(echoState, micIn, refRing[refTick], cleaned);
        if (preprocessState != NULL) {
            speex_preprocess_run(preprocessState, cleaned);
        }
        memcpy(out, cleaned, sizeof(cleaned));

        /* Cleared before the tick moves on.  The slot comes round again in
         * VOICE_AEC_REF_FRAMES ticks and has to be empty when it does - a
         * stale reference cancels a sound the microphone never heard, which
         * damages speech rather than cleaning it. */
        memset(refRing[refTick], 0, sizeof(refRing[refTick]));
        refTick = (refTick + 1) % VOICE_AEC_REF_FRAMES;
        return;
    }

    /* Nothing to cancel, so the frame only has to reach out before the
     * preprocessor runs over it in place. */
    if (out != micIn) {
        memcpy(out, micIn, VOICE_FRAME_SAMPLES * sizeof(int16_t));
    }
    if (preprocessState != NULL) {
        speex_preprocess_run(preprocessState, out);
    }
}
