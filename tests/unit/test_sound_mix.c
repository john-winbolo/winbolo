/*
 * Mixing one sound slot into the effects accumulator
 * (src/gui/sdl3/sound_mix.c).
 *
 * soundMixSlot adds a slot's interleaved 16-bit samples into the mixer's
 * 32-bit accumulator, scaled by the slot's left and right Q8 gains. Three
 * things are pinned here. At unity on both channels the result is the plain
 * sum the mixer used before gains existed, so a sound played at unity is
 * unchanged. The channel a sample belongs to comes from its absolute index in
 * the sound, so a slot the mixer stops partway through a frame and resumes on
 * the next callback keeps left on the left. And a one-channel device uses the
 * average of the two gains for every sample.
 *
 * Nothing here opens an audio device.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sound_mix.h"
#include "test_harness.h"

#define MIX_TEST_SAMPLES 14

/* Covers both 16-bit extremes, the values either side of zero, and a spread
   of ordinary values, so the scaling is checked where rounding and sign
   matter. */
static const int16_t s_mixSource[MIX_TEST_SAMPLES] = {
    32767, -32768, -1, 0, 1, 1234, -1234,
    255, -255, 256, -257, 20000, -19999, 7
};

int run_sound_mix_unity_matches_plain_sum(void) {
    int32_t got[MIX_TEST_SAMPLES];
    int32_t want[MIX_TEST_SAMPLES];
    int j;

    /* Start from an accumulator that already holds other slots' samples. */
    for (j = 0; j < MIX_TEST_SAMPLES; j++) {
        got[j] = (j * 3001) - 20000;
        want[j] = got[j];
    }

    for (j = 0; j < MIX_TEST_SAMPLES; j++) {
        want[j] += s_mixSource[j];
    }
    soundMixSlot(got, s_mixSource, MIX_TEST_SAMPLES, 0, 2, 256, 256);

    for (j = 0; j < MIX_TEST_SAMPLES; j++) {
        UT_ASSERT_MSG(got[j] == want[j],
                      "unity: sample %d is %ld, plain sum gives %ld",
                      j, (long)got[j], (long)want[j]);
    }
    return 0;
}

/* Mixes the source in two calls, the first ending on an odd sample count,
   and checks each absolute index landed on the channel it belongs to. */
static int checkParity(uint16_t gainL, uint16_t gainR, const char *label) {
    int32_t acc[MIX_TEST_SAMPLES];
    const uint32_t first = 5;   /* odd, so the second call starts on a right sample */
    int j;
    int32_t want;
    int isLeft;

    memset(acc, 0, sizeof(acc));
    soundMixSlot(acc, s_mixSource, first, 0, 2, gainL, gainR);
    soundMixSlot(acc + first, s_mixSource + first, MIX_TEST_SAMPLES - first,
                 first, 2, gainL, gainR);

    for (j = 0; j < MIX_TEST_SAMPLES; j++) {
        isLeft = (j % 2) == 0;
        if (isLeft) {
            want = (gainL != 0) ? s_mixSource[j] : 0;
        } else {
            want = (gainR != 0) ? s_mixSource[j] : 0;
        }
        UT_ASSERT_MSG(acc[j] == want,
                      "%s: sample %d (%s) is %ld, expected %ld",
                      label, j, isLeft ? "left" : "right",
                      (long)acc[j], (long)want);
    }
    return 0;
}

int run_sound_mix_channel_parity_across_resume(void) {
    if (checkParity(256, 0, "left only") != 0) return 1;
    if (checkParity(0, 256, "right only") != 0) return 1;
    return 0;
}

int run_sound_mix_mono_device(void) {
    int32_t acc[MIX_TEST_SAMPLES];
    int32_t want;
    int j;

    memset(acc, 0, sizeof(acc));
    soundMixSlot(acc, s_mixSource, MIX_TEST_SAMPLES, 0, 1, 256, 0);

    for (j = 0; j < MIX_TEST_SAMPLES; j++) {
        want = ((int32_t)s_mixSource[j] * 128) >> 8;
        UT_ASSERT_MSG(acc[j] == want,
                      "mono: sample %d is %ld, expected %ld",
                      j, (long)acc[j], (long)want);
    }
    return 0;
}
