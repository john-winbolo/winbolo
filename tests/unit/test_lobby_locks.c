/*
 * Lobby lock-bit coverage.
 *
 * The lock plumbing (serverSimSetServerLocks + the per-setting bit
 * lookup) was dead config until the matching CLI flag landed. These
 * tests pin down two invariants the wire side relies on:
 *
 *   1. The LST_* → LOBBY_LOCK_* mapping is correct and stable. If a
 *      future commit drifts the lock-bit lookup table away from the
 *      values that wire_limits.h advertises, every existing client's
 *      lock badge points at the wrong control. The packet handler in
 *      transport_udp_server.c now reads the table through this same
 *      helper, so verifying the helper covers the packet path too.
 *
 *   2. serverSimIsSettingLocked composes the helper + the live
 *      sim->serverLocks mask correctly — and only returns true for
 *      settings that actually have a lock bit (LST_RANKED used to
 *      hard-code 0 in the packet handler; once we added the bit a
 *      stale 0 would be a silent regression).
 *
 *   3. The full lock mask round-trips through set/get unchanged, so a
 *      CLI mask is preserved across the publish cycle.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "wire_limits.h"
#include "netpacks.h"        /* LST_* setting ids */
#include "everard_map.h"
#include "test_harness.h"

static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

int run_lobby_lock_bit_lookup(void) {
    /* Every LST_* the packet handler knows about must map to the
     * corresponding LOBBY_LOCK_* value. LST_TIME_MINUTES intentionally
     * shares the LOBBY_LOCK_TIME_LIMIT bit with LST_TIME_LIMIT — the
     * minutes are part of the same logical "time limit" setting. */
    UT_ASSERT(serverSimGetSettingLockBit(LST_GAME_TYPE)         == LOBBY_LOCK_GAME_TYPE);
    UT_ASSERT(serverSimGetSettingLockBit(LST_HIDDEN_MINES)      == LOBBY_LOCK_MINES);
    UT_ASSERT(serverSimGetSettingLockBit(LST_AI_POLICY)         == LOBBY_LOCK_AI_POLICY);
    UT_ASSERT(serverSimGetSettingLockBit(LST_TIME_LIMIT)        == LOBBY_LOCK_TIME_LIMIT);
    UT_ASSERT_MSG(serverSimGetSettingLockBit(LST_TIME_MINUTES)  == LOBBY_LOCK_TIME_LIMIT,
                  "LST_TIME_MINUTES must share the TIME_LIMIT bit");
    UT_ASSERT(serverSimGetSettingLockBit(LST_AUTO_LOCK_ON_GAME) == LOBBY_LOCK_AUTO_LOCK_ON_GAME);
    UT_ASSERT_MSG(serverSimGetSettingLockBit(LST_RANKED)        == LOBBY_LOCK_RANKED,
                  "LST_RANKED used to hardcode 0; ensure the new bit is wired");

    /* Unknown setting ids must return 0xFFFF so the packet handler can
     * silently drop them (forward-compat) instead of treating them as
     * "always lockable". 0xFE is unused at the time of writing. */
    UT_ASSERT(serverSimGetSettingLockBit(0xFE) == 0xFFFFu);

    /* All used lock bits must be distinct so no setting accidentally
     * shadows another. Build a mask from every LST_*-derived bit and
     * count the population — if any two collide, popcount drops. */
    uint16_t allBits =
        LOBBY_LOCK_GAME_TYPE | LOBBY_LOCK_MINES | LOBBY_LOCK_AI_POLICY |
        LOBBY_LOCK_TIME_LIMIT | LOBBY_LOCK_AUTO_LOCK_ON_GAME |
        LOBBY_LOCK_RANKED | LOBBY_LOCK_PASSWORD | LOBBY_LOCK_OPEN_HOST |
        LOBBY_LOCK_MAP;
    int popcount = 0;
    for (int i = 0; i < 16; i++) if (allBits & (1u << i)) popcount++;
    UT_ASSERT_MSG(popcount == 9, "every defined LOBBY_LOCK_* bit must be distinct");

    return 0;
}

int run_lobby_lock_rejects_settings(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    /* With no locks set, every setting reports as not-locked. */
    UT_ASSERT(serverSimGetServerLocks(sim) == 0);
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_GAME_TYPE));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_HIDDEN_MINES));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_AI_POLICY));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_TIME_LIMIT));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_TIME_MINUTES));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_AUTO_LOCK_ON_GAME));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_RANKED));

    /* Lock just GAME_TYPE — only that LST_* reports locked. */
    serverSimSetServerLocks(sim, LOBBY_LOCK_GAME_TYPE);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_GAME_TYPE));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_HIDDEN_MINES));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_AI_POLICY));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_RANKED));

    /* Lock TIME_LIMIT — both LST_TIME_LIMIT and LST_TIME_MINUTES
     * report locked because they share the bit. This is the same
     * coupling the wire handler relies on; the test pins it. */
    serverSimSetServerLocks(sim, LOBBY_LOCK_TIME_LIMIT);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_TIME_LIMIT));
    UT_ASSERT_MSG(serverSimIsSettingLocked(sim, LST_TIME_MINUTES),
                  "locking TIME_LIMIT must also gate TIME_MINUTES");

    /* Lock RANKED — proves the formerly-hardcoded-zero bit is live. */
    serverSimSetServerLocks(sim, LOBBY_LOCK_RANKED);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_RANKED));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_GAME_TYPE));

    /* Composite mask — every locked setting reports locked, nothing
     * else does. Mirrors a real "-lock gametype,ranked,autolock"
     * invocation. */
    uint16_t composite = LOBBY_LOCK_GAME_TYPE | LOBBY_LOCK_RANKED |
                         LOBBY_LOCK_AUTO_LOCK_ON_GAME;
    serverSimSetServerLocks(sim, composite);
    UT_ASSERT(serverSimGetServerLocks(sim) == composite);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_GAME_TYPE));
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_RANKED));
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_AUTO_LOCK_ON_GAME));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_HIDDEN_MINES));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_AI_POLICY));

    /* Lock bits with no LST_* counterpart (PASSWORD / OPEN_HOST / MAP
     * are enforced by their own dedicated packet handlers, not by
     * PACKET_LOBBY_SET_SETTING) must NOT be reported as locking any
     * LST_*-routed setting — that would block the wrong control. */
    serverSimSetServerLocks(sim, LOBBY_LOCK_PASSWORD | LOBBY_LOCK_OPEN_HOST |
                                  LOBBY_LOCK_MAP);
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_GAME_TYPE));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_RANKED));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_AUTO_LOCK_ON_GAME));

    serverSimDestroy(sim);
    return 0;
}

int run_lobby_lock_mask_roundtrip(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    /* Initial mask is 0 (no locks). */
    UT_ASSERT(serverSimGetServerLocks(sim) == 0);

    /* Set every defined bit and read back unchanged. */
    uint16_t allLocks =
        LOBBY_LOCK_GAME_TYPE | LOBBY_LOCK_AI_POLICY | LOBBY_LOCK_MINES |
        LOBBY_LOCK_TIME_LIMIT | LOBBY_LOCK_AUTO_LOCK_ON_GAME |
        LOBBY_LOCK_PASSWORD | LOBBY_LOCK_RANKED | LOBBY_LOCK_OPEN_HOST |
        LOBBY_LOCK_MAP;
    serverSimSetServerLocks(sim, allLocks);
    UT_ASSERT_MSG(serverSimGetServerLocks(sim) == allLocks,
                  "all-locks mask must round-trip unchanged");

    /* Clear and confirm. */
    serverSimSetServerLocks(sim, 0);
    UT_ASSERT(serverSimGetServerLocks(sim) == 0);

    /* NULL-safe getter (defensive — the broadcast path can call this
     * before the sim is fully constructed in some test fixtures). */
    UT_ASSERT(serverSimGetServerLocks(NULL) == 0);

    serverSimDestroy(sim);
    return 0;
}
