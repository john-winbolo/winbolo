/*
 * Coverage for lobbyTimeMinutesIsValid — the wire-side predicate that
 * gates LST_TIME_MINUTES values before they reach the sim. Boundaries
 * matter because the ticks arithmetic downstream
 * (minutes * 60 * GAME_NUMGAMETICKS_SEC) is int32_t and an attacker-
 * controlled uint16_t could otherwise drive it toward overflow.
 */
#include <stdint.h>

#include "global.h"
#include "netpacks.h"
#include "test_harness.h"

int run_lobby_time_minutes_valid(void) {
    UT_ASSERT(lobbyTimeMinutesIsValid(0) == false);
    UT_ASSERT(lobbyTimeMinutesIsValid(1) == true);
    UT_ASSERT(lobbyTimeMinutesIsValid(30) == true);
    UT_ASSERT(lobbyTimeMinutesIsValid(240) == true);
    UT_ASSERT(lobbyTimeMinutesIsValid(241) == false);
    UT_ASSERT(lobbyTimeMinutesIsValid(65535) == false);
    return 0;
}
