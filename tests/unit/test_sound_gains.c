/*
 * Positional sound gains (src/bolo/sounddist.c).
 *
 * soundDistGains turns the pan and dist a human recipient is sent into Q8 left
 * and right gains. The mixer sums up to sixteen slots and clips once at the
 * end, so a gain above unity is a clipping risk: the channel towards a sound
 * stays at unity and the other one is ducked. These cases pin that no input
 * ever produces a gain above unity, that a sound straight north or south is
 * centred at full volume, and that east and west are mirror images.
 *
 * The last two cases drive clientSoundDist on a client sim and read the
 * played sound back from the frontend recorder in test_stubs.c: the dist
 * picks the near or the far variant on the SDIST_SOFT edge, and the pan
 * reaches the frontend as its gains while the server has positional sound
 * on. With it off the variant is picked the same way and both channels
 * play at unity.
 */

#include <stdint.h>
#include <stdio.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h" /* positionalSound, set here by hand */
#include "client_enums.h"   /* sndEffects */
#include "game_sim.h"
#include "sounddist.h"      /* soundDistGains, SDIST_SOFT / SDIST_NONE */
#include "input_packet.h"   /* SOUND_PAN_* / SOUND_DIST_*, soundDistBandTop */
#include "test_harness.h"

#define SG_UNITY 256

/* The eight dist band tops a human can be sent. */
static const BYTE s_bandTops[] = { 5, 10, 15, 20, 25, 30, 35, 39 };
#define SG_BAND_COUNT ((int)(sizeof(s_bandTops) / sizeof(s_bandTops[0])))

int run_sound_gains_table(void) {
    int pan, b;
    uint16_t gL, gR;

    for (pan = -SOUND_PAN_MAX; pan <= SOUND_PAN_MAX; pan += SOUND_PAN_STEP) {
        for (b = 0; b < SG_BAND_COUNT; b++) {
            BYTE dist = s_bandTops[b];

            soundDistGains((int8_t)pan, dist, &gL, &gR);
            UT_ASSERT_MSG(gL <= SG_UNITY && gR <= SG_UNITY,
                          "pan %d dist %u gave %u,%u, above unity",
                          pan, (unsigned)dist, (unsigned)gL, (unsigned)gR);
            if (dist <= SDIST_SOFT) {
                if (pan > 0) {
                    UT_ASSERT_MSG(gR == SG_UNITY,
                                  "pan %d dist %u gave right %u; the channel "
                                  "towards the sound must be unity",
                                  pan, (unsigned)dist, (unsigned)gR);
                } else if (pan < 0) {
                    UT_ASSERT_MSG(gL == SG_UNITY,
                                  "pan %d dist %u gave left %u; the channel "
                                  "towards the sound must be unity",
                                  pan, (unsigned)dist, (unsigned)gL);
                }
            }
        }
    }

    /* Full pan ducks the far channel to 77 (0.3). */
    soundDistGains(8, 5, &gL, &gR);
    UT_ASSERT_MSG(gL == 77 && gR == SG_UNITY,
                  "pan 8 dist 5 gave %u,%u, expected 77,256",
                  (unsigned)gL, (unsigned)gR);
    soundDistGains(-8, 5, &gL, &gR);
    UT_ASSERT_MSG(gL == SG_UNITY && gR == 77,
                  "pan -8 dist 5 gave %u,%u, expected 256,77",
                  (unsigned)gL, (unsigned)gR);
    return 0;
}

int run_sound_gains_centre_unity(void) {
    int b;
    uint16_t gL, gR;

    for (b = 0; b < SG_BAND_COUNT; b++) {
        BYTE dist = s_bandTops[b];

        soundDistGains(0, dist, &gL, &gR);
        UT_ASSERT_MSG(gL == gR,
                      "pan 0 dist %u gave %u,%u; a sound straight north or "
                      "south must be centred", (unsigned)dist, (unsigned)gL,
                      (unsigned)gR);
        if (dist <= SDIST_SOFT) {
            UT_ASSERT_MSG(gL == SG_UNITY && gR == SG_UNITY,
                          "pan 0 dist %u gave %u,%u; inside SDIST_SOFT a "
                          "centred sound must be at unity",
                          (unsigned)dist, (unsigned)gL, (unsigned)gR);
        }
    }
    return 0;
}

int run_sound_gains_never_above_unity(void) {
    int pan, dist;
    uint16_t gL, gR;

    for (pan = -128; pan <= 127; pan++) {
        for (dist = 0; dist <= 255; dist++) {
            soundDistGains((int8_t)pan, (BYTE)dist, &gL, &gR);
            UT_ASSERT_MSG(gL <= SG_UNITY && gR <= SG_UNITY,
                          "pan %d dist %d gave %u,%u, above unity",
                          pan, dist, (unsigned)gL, (unsigned)gR);
        }
    }
    return 0;
}

int run_sound_gains_mirror(void) {
    int pan, dist;
    uint16_t gL, gR, mL, mR;

    for (pan = -127; pan <= 127; pan++) {
        for (dist = 0; dist <= 255; dist++) {
            soundDistGains((int8_t)pan, (BYTE)dist, &gL, &gR);
            soundDistGains((int8_t)-pan, (BYTE)dist, &mL, &mR);
            UT_ASSERT_MSG(gL == mR && gR == mL,
                          "pan %d dist %d gave %u,%u but pan %d gave %u,%u — "
                          "east and west must mirror", pan, dist,
                          (unsigned)gL, (unsigned)gR, -pan, (unsigned)mL,
                          (unsigned)mR);
        }
    }
    return 0;
}

int run_sound_dist_variant_by_band(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT_MSG(gs != NULL, "clientSimGetGameSim returned NULL");
    /* As the lobby-settings event leaves it when the server has positional
     * sound on. */
    cs->positionalSound = true;

    /* SDIST_SOFT is the near band's top: the near variant, panned hard
     * east. */
    ut_sound_reset();
    clientSoundDist(gs, bigExplosionNear, SOUND_PAN_MAX, SDIST_SOFT);
    UT_ASSERT_MSG(ut_sound_count() == 1 &&
                  ut_sound_get(0) == (int)bigExplosionNear,
                  "dist %d played %d sound(s), the first %d; expected "
                  "bigExplosionNear (%d)", SDIST_SOFT, ut_sound_count(),
                  ut_sound_get(0), (int)bigExplosionNear);
    UT_ASSERT_MSG(ut_sound_get_gain_left(0) == 77 &&
                  ut_sound_get_gain_right(0) == SG_UNITY,
                  "pan %d played at %d,%d, expected 77,256", SOUND_PAN_MAX,
                  ut_sound_get_gain_left(0), ut_sound_get_gain_right(0));

    /* The next band up is far. */
    ut_sound_reset();
    clientSoundDist(gs, bigExplosionNear, SOUND_PAN_MAX,
                    SDIST_SOFT + SOUND_DIST_BAND);
    UT_ASSERT_MSG(ut_sound_count() == 1 &&
                  ut_sound_get(0) == (int)bigExplosionFar,
                  "dist %d played %d sound(s), the first %d; expected "
                  "bigExplosionFar (%d)", SDIST_SOFT + SOUND_DIST_BAND,
                  ut_sound_count(), ut_sound_get(0), (int)bigExplosionFar);
    UT_ASSERT_MSG(ut_sound_get_gain_left(0) < ut_sound_get_gain_right(0),
                  "pan %d at dist %d played at %d,%d; the sound is east, so "
                  "the left channel must be the quieter", SOUND_PAN_MAX,
                  SDIST_SOFT + SOUND_DIST_BAND, ut_sound_get_gain_left(0),
                  ut_sound_get_gain_right(0));

    ut_sound_reset();
    clientSimDestroy(cs);
    return 0;
}

int run_sound_positional_off_unity(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT_MSG(gs != NULL, "clientSimGetGameSim returned NULL");
    cs->positionalSound = false;
    UT_ASSERT(!clientSimGetPositionalSound(cs));

    /* Hard east at the last in-range distance: the far variant, and both
     * channels at unity because the server has positional sound off. */
    ut_sound_reset();
    clientSoundDist(gs, bigExplosionNear, SOUND_PAN_MAX, SOUND_DIST_MAX);
    UT_ASSERT_MSG(ut_sound_count() == 1 &&
                  ut_sound_get(0) == (int)bigExplosionFar,
                  "dist %d played %d sound(s), the first %d; expected "
                  "bigExplosionFar (%d)", SOUND_DIST_MAX, ut_sound_count(),
                  ut_sound_get(0), (int)bigExplosionFar);
    UT_ASSERT_MSG(ut_sound_get_gain_left(0) == SG_UNITY &&
                  ut_sound_get_gain_right(0) == SG_UNITY,
                  "with positional sound off pan %d played at %d,%d, "
                  "expected 256,256", SOUND_PAN_MAX,
                  ut_sound_get_gain_left(0), ut_sound_get_gain_right(0));

    ut_sound_reset();
    clientSimDestroy(cs);
    return 0;
}
