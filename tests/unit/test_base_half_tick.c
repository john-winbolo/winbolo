/*
 * The half-tick carry basesHalfTickCalulator keeps between calls.
 *
 * A base's shell and mine refuel intervals are 7.5 ticks and refuelTime is an
 * int, so the function hands back 7 and then 8 and keeps the leftover half
 * until the next call. That carry used to live in two function-level statics,
 * one pair per process, which every sim in the process shared: a server's sim
 * and a client's both reach the function — the client through basesUpdate and
 * through basesRefueling, which calls it before its isServer test — so two
 * sims refuelling in the same process stole each other's halves and each saw
 * 7, 7, 7 or 8, 8, 8 rather than the alternation. The carry is now two fields
 * on GameSim.
 *
 * The cases build bare GameSims: the function reads the two refuel rules and
 * its own two fields and nothing else, and a zeroed struct with the classic
 * rules filled in is what clientSimCreate and serverSimInit hand it.
 *
 *   base_half_tick_per_sim_sequence — two sims in one process each walk their
 *     own 7, 8, 7, 8 for shells and for mines.
 *   base_half_tick_other_sim_does_not_disturb — one sim's sequence is the same
 *     whether or not another sim calls in between its own calls.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "game_sim.h"
#include "bases.h"
#include "sim_rules.h"
#include "test_harness.h"

/* What the two creation sites do to these fields: zero the struct, then fill
 * the rules. Neither writes the carry, so it starts at zero from the memset. */
static void halfTickSim(GameSim *gs) {
    memset(gs, 0, sizeof(*gs));
    simRulesClassic(&gs->rules);
}

/* The classic 7.5-tick interval, so the sequence is 7 then 8. */
#define HT_LOW  7
#define HT_HIGH 8

int run_base_half_tick_per_sim_sequence(void) {
    GameSim a;
    GameSim b;
    int i;

    halfTickSim(&a);
    halfTickSim(&b);

    UT_ASSERT_MSG(a.halfTickShell == 0 && a.halfTickMine == 0,
                  "a zeroed sim should start with no half owing, got %g/%g",
                  a.halfTickShell, a.halfTickMine);

    /* Interleave the two sims deliberately: with one carry per process this
       is exactly the order that made each of them return the same number
       every time. */
    for (i = 0; i < 4; i++) {
        int want = ((i % 2) == 0) ? HT_LOW : HT_HIGH;
        int gotA = basesHalfTickCalulator(&a, BASES_HALFTICK_TYPE_SHELL);
        int gotB = basesHalfTickCalulator(&b, BASES_HALFTICK_TYPE_SHELL);
        UT_ASSERT_MSG(gotA == want,
                      "sim a shell call %d returned %d, expected %d",
                      i, gotA, want);
        UT_ASSERT_MSG(gotB == want,
                      "sim b shell call %d returned %d, expected %d",
                      i, gotB, want);
    }

    /* Mines carry separately from shells, and separately per sim. */
    for (i = 0; i < 4; i++) {
        int want = ((i % 2) == 0) ? HT_LOW : HT_HIGH;
        int gotA = basesHalfTickCalulator(&a, BASES_HALFTICK_TYPE_MINE);
        int gotB = basesHalfTickCalulator(&b, BASES_HALFTICK_TYPE_MINE);
        UT_ASSERT_MSG(gotA == want,
                      "sim a mine call %d returned %d, expected %d",
                      i, gotA, want);
        UT_ASSERT_MSG(gotB == want,
                      "sim b mine call %d returned %d, expected %d",
                      i, gotB, want);
    }

    /* Armour is a whole number of ticks and carries nothing, so it is the
       same answer every time and neither sim's carry moved. */
    UT_ASSERT(basesHalfTickCalulator(&a, BASES_HALFTICK_TYPE_ARMOUR) ==
              BASE_REFUEL_ARMOUR);
    UT_ASSERT_MSG(a.halfTickShell == 0 && a.halfTickMine == 0,
                  "an armour call moved a carry: %g/%g",
                  a.halfTickShell, a.halfTickMine);

    return 0;
}

int run_base_half_tick_other_sim_does_not_disturb(void) {
    GameSim alone;
    GameSim shared;
    GameSim noisy;
    int solo[6];
    int withNoise[6];
    int i;

    /* One sim calling by itself. */
    halfTickSim(&alone);
    for (i = 0; i < 6; i++) {
        solo[i] = basesHalfTickCalulator(&alone, BASES_HALFTICK_TYPE_SHELL);
    }

    /* The same sim's calls with another sim's calls threaded between them,
       an uneven number of them so a shared carry could not come back into
       phase by accident. */
    halfTickSim(&shared);
    halfTickSim(&noisy);
    for (i = 0; i < 6; i++) {
        withNoise[i] = basesHalfTickCalulator(&shared, BASES_HALFTICK_TYPE_SHELL);
        (void) basesHalfTickCalulator(&noisy, BASES_HALFTICK_TYPE_SHELL);
        if ((i % 2) == 0) {
            (void) basesHalfTickCalulator(&noisy, BASES_HALFTICK_TYPE_SHELL);
            (void) basesHalfTickCalulator(&noisy, BASES_HALFTICK_TYPE_MINE);
        }
    }

    for (i = 0; i < 6; i++) {
        UT_ASSERT_MSG(withNoise[i] == solo[i],
                      "call %d returned %d alongside another sim and %d alone",
                      i, withNoise[i], solo[i]);
    }

    /* And that sequence is the alternation, not one number repeated — the
       shape a shared carry used to flatten it into. */
    for (i = 0; i < 6; i++) {
        int want = ((i % 2) == 0) ? HT_LOW : HT_HIGH;
        UT_ASSERT_MSG(solo[i] == want,
                      "call %d returned %d, expected %d", i, solo[i], want);
    }

    return 0;
}
