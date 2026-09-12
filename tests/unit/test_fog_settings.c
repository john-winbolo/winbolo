/*
 * The overview window and line of sight: their defaults, the values
 * classic mode forces onto them, their lock bits and their trip through
 * the lobby snapshot.
 *
 * Five things are pinned here:
 *
 *   1. A fresh sim keeps the live block expanded with nothing blocking
 *      sight inside it, which is what the game did before the settings
 *      existed. Nothing on the client reads either value yet, so a drift
 *      in the default would otherwise go unnoticed until the consumers
 *      land. Both setters ignore a value outside their enum and both
 *      getters answer for a NULL sim.
 *
 *   2. Turning classic mode on narrows the window and turns line of
 *      sight off, over whatever the operator had set. Turning it off
 *      clears the flag and nothing else — both stay where classic mode
 *      put them, the same deliberate asymmetry the three view policies
 *      have.
 *
 *   3. Each setting has its own lock bit, distinct from every other
 *      LOBBY_LOCK_*, and because classic mode writes both, a lock on
 *      either also locks classic mode. Otherwise a host could change a
 *      locked value by ticking the checkbox, and could not undo it
 *      afterwards.
 *
 *   4. Both ride the originalLobbySettings snapshot, so a visiting
 *      host's change is undone when the last human leaves and
 *      serverSimResetLobbyToDefaults restores the operator's startup
 *      configuration.
 *
 *   5. The lobby's own path takes a one-byte payload for each, refuses a
 *      wrong length and refuses a byte neither enum has a name for, and
 *      refuses any edit at all while classic mode is on. Each code maps
 *      to its own lock bit; without that the command dispatcher reads the
 *      unknown sentinel and drops the edit, so the lobby control would
 *      look live and do nothing.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "server_lifecycle.h"             /* ServerInstanceConfig */
#include "server/sim/server_sim_shared.h" /* serverSimResetLobbyToDefaults */
#include "view_policy.h"
#include "wire_limits.h"
#include "everard_map.h"
#include "test_harness.h"

static ServerSim *make_fog_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

int run_fog_settings_defaults(void) {
    ServerSim *sim = make_fog_sim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowExpanded,
                  "a fresh sim's overview window = %u, want expanded",
                  (unsigned)serverSimGetOverviewWindow(sim));
    UT_ASSERT_MSG(serverSimGetLineOfSight(sim) == (uint8_t)lineOfSightOff,
                  "a fresh sim's line of sight = %u, want off",
                  (unsigned)serverSimGetLineOfSight(sim));

    /* Both getters answer for a NULL sim — the broadcast path can call
     * them before the sim is fully constructed in some fixtures. */
    UT_ASSERT(serverSimGetOverviewWindow(NULL) ==
                  (uint8_t)overviewWindowExpanded);
    UT_ASSERT(serverSimGetLineOfSight(NULL) == (uint8_t)lineOfSightOff);

    /* Both values round-trip through their setters. */
    serverSimSetOverviewWindow(sim, (uint8_t)overviewWindowClassic);
    serverSimSetLineOfSight(sim, (uint8_t)lineOfSightBuildingsAndTrees);
    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowClassic,
                  "overview window after set = %u, want classic",
                  (unsigned)serverSimGetOverviewWindow(sim));
    UT_ASSERT_MSG(serverSimGetLineOfSight(sim) ==
                      (uint8_t)lineOfSightBuildingsAndTrees,
                  "line of sight after set = %u, want buildings and trees",
                  (unsigned)serverSimGetLineOfSight(sim));

    /* A value at or above the enum's count is ignored and the stored
     * value is left alone, so a wire byte from a newer sender cannot
     * push either setting somewhere the server has no name for. */
    serverSimSetOverviewWindow(sim, (uint8_t)OVERVIEW_WINDOW_COUNT);
    serverSimSetOverviewWindow(sim, 200);
    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowClassic,
                  "an out-of-range window moved the stored value to %u",
                  (unsigned)serverSimGetOverviewWindow(sim));
    serverSimSetLineOfSight(sim, (uint8_t)LINE_OF_SIGHT_COUNT);
    serverSimSetLineOfSight(sim, 200);
    UT_ASSERT_MSG(serverSimGetLineOfSight(sim) ==
                      (uint8_t)lineOfSightBuildingsAndTrees,
                  "an out-of-range mode moved the stored value to %u",
                  (unsigned)serverSimGetLineOfSight(sim));

    /* Both setters are NULL-safe. */
    serverSimSetOverviewWindow(NULL, (uint8_t)overviewWindowClassic);
    serverSimSetLineOfSight(NULL, (uint8_t)lineOfSightBuildingsAndTrees);

    serverSimDestroy(sim);
    return 0;
}

int run_fog_settings_classic_mode_preset(void) {
    ServerSim *sim = make_fog_sim();
    UT_ASSERT(sim != NULL);

    /* The operator set both to the opposite of what classic mode wants. */
    serverSimSetOverviewWindow(sim, (uint8_t)overviewWindowExpanded);
    serverSimSetLineOfSight(sim, (uint8_t)lineOfSightBuildingsAndTrees);

    serverSimSetClassicMode(sim, true);
    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowClassic,
                  "classic mode left the overview window at %u, want classic",
                  (unsigned)serverSimGetOverviewWindow(sim));
    UT_ASSERT_MSG(serverSimGetLineOfSight(sim) == (uint8_t)lineOfSightOff,
                  "classic mode left line of sight at %u, want off",
                  (unsigned)serverSimGetLineOfSight(sim));

    /* While it is on, both settings refuse a lobby edit, the same way the
     * three view policies do. */
    {
        uint8_t value[1];
        value[0] = (uint8_t)overviewWindowExpanded;
        UT_ASSERT_MSG(!serverSimApplyLobbySetting(sim, LST_OVERVIEW_WINDOW,
                                                  value, 1),
                      "an overview-window edit should be refused under "
                      "classic mode");
        value[0] = (uint8_t)lineOfSightBuildingsAndTrees;
        UT_ASSERT_MSG(!serverSimApplyLobbySetting(sim, LST_LINE_OF_SIGHT,
                                                  value, 1),
                      "a line-of-sight edit should be refused under "
                      "classic mode");
        UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                          (uint8_t)overviewWindowClassic &&
                      serverSimGetLineOfSight(sim) == (uint8_t)lineOfSightOff,
                      "a refused edit still moved a value "
                      "(window %u, sight %u)",
                      (unsigned)serverSimGetOverviewWindow(sim),
                      (unsigned)serverSimGetLineOfSight(sim));
    }

    /* Turning it off clears the flag and nothing else: both stay where
     * classic mode put them, which is the consequence of writing the
     * values rather than masking them at read time. */
    serverSimSetClassicMode(sim, false);
    UT_ASSERT(!serverSimGetClassicMode(sim));
    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowClassic,
                  "leaving classic mode moved the overview window to %u",
                  (unsigned)serverSimGetOverviewWindow(sim));
    UT_ASSERT_MSG(serverSimGetLineOfSight(sim) == (uint8_t)lineOfSightOff,
                  "leaving classic mode moved line of sight to %u",
                  (unsigned)serverSimGetLineOfSight(sim));

    /* With classic mode off both are set freely again. */
    serverSimSetLineOfSight(sim, (uint8_t)lineOfSightBuildingsAndTrees);
    UT_ASSERT_MSG(serverSimGetLineOfSight(sim) ==
                      (uint8_t)lineOfSightBuildingsAndTrees,
                  "line of sight is not editable with classic mode off");

    /* And through the lobby, which is the path a host's click takes. */
    {
        uint8_t value[1];
        value[0] = (uint8_t)overviewWindowExpanded;
        UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_OVERVIEW_WINDOW,
                                                 value, 1),
                      "an overview-window edit should be allowed once "
                      "classic mode is off");
        UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                          (uint8_t)overviewWindowExpanded,
                      "the accepted overview-window edit left the value "
                      "at %u", (unsigned)serverSimGetOverviewWindow(sim));
        value[0] = (uint8_t)lineOfSightOff;
        UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_LINE_OF_SIGHT,
                                                 value, 1),
                      "a line-of-sight edit should be allowed once classic "
                      "mode is off");
        UT_ASSERT_MSG(serverSimGetLineOfSight(sim) == (uint8_t)lineOfSightOff,
                      "the accepted line-of-sight edit left the value at %u",
                      (unsigned)serverSimGetLineOfSight(sim));
    }

    serverSimDestroy(sim);
    return 0;
}

int run_fog_settings_lock_implied(void) {
    ServerSim *sim = make_fog_sim();
    static const uint32_t newBits[] = {
        LOBBY_LOCK_OVERVIEW_WINDOW, LOBBY_LOCK_LINE_OF_SIGHT,
    };
    UT_ASSERT(sim != NULL);

    /* Neither bit may collide with an existing one — a shared bit would
     * freeze the wrong control on every client. */
    {
        uint32_t others =
            LOBBY_LOCK_GAME_TYPE | LOBBY_LOCK_AI_POLICY | LOBBY_LOCK_MINES |
            LOBBY_LOCK_TIME_LIMIT | LOBBY_LOCK_AUTO_LOCK_ON_GAME |
            LOBBY_LOCK_PASSWORD | LOBBY_LOCK_RANKED | LOBBY_LOCK_OPEN_HOST |
            LOBBY_LOCK_MAP | LOBBY_LOCK_PILL_VIEW | LOBBY_LOCK_BASE_VIEW |
            LOBBY_LOCK_ALLY_VIEW | LOBBY_LOCK_CLASSIC_MODE |
            LOBBY_LOCK_ALLIES_IN_TREES;
        UT_ASSERT_MSG((others & LOBBY_LOCK_OVERVIEW_WINDOW) == 0u,
                      "LOBBY_LOCK_OVERVIEW_WINDOW collides with an existing bit");
        UT_ASSERT_MSG((others & LOBBY_LOCK_LINE_OF_SIGHT) == 0u,
                      "LOBBY_LOCK_LINE_OF_SIGHT collides with an existing bit");
        UT_ASSERT_MSG((LOBBY_LOCK_OVERVIEW_WINDOW &
                       LOBBY_LOCK_LINE_OF_SIGHT) == 0u,
                      "the two new lock bits collide with each other");
    }

    for (size_t i = 0; i < sizeof(newBits) / sizeof(newBits[0]); i++) {
        uint32_t in  = newBits[i];
        uint32_t out = serverSimAddImpliedLocks(in);
        /* Classic mode writes both settings, so a lock on either has to
         * reach classic mode as well — and nothing else, or an operator
         * who locked the window finds unrelated controls frozen too. */
        UT_ASSERT_MSG(out == (uint32_t)(in | LOBBY_LOCK_CLASSIC_MODE),
                      "lock 0x%08X implied 0x%08X, want 0x%08X",
                      (unsigned)in, (unsigned)out,
                      (unsigned)(in | LOBBY_LOCK_CLASSIC_MODE));
        /* Idempotent — the stored mask is re-folded on every lobby reset,
         * so a second pass must not drift. */
        UT_ASSERT_MSG(serverSimAddImpliedLocks(out) == out,
                      "serverSimAddImpliedLocks is not idempotent for 0x%08X",
                      (unsigned)in);

        /* Through a sim: what the getter reports is what the lobby
         * settings event carries to every client, so this is the value
         * the lock badge and the disabled checkbox are drawn from. */
        serverSimSetServerLocks(sim, in);
        UT_ASSERT_MSG(serverSimGetServerLocks(sim) == out,
                      "sim mask for 0x%08X = 0x%08X, want 0x%08X",
                      (unsigned)in, (unsigned)serverSimGetServerLocks(sim),
                      (unsigned)out);
        UT_ASSERT_MSG(serverSimIsSettingLocked(sim, LST_CLASSIC_MODE),
                      "lock 0x%08X must also lock classic mode",
                      (unsigned)in);
    }

    serverSimDestroy(sim);
    return 0;
}

int run_fog_settings_lobby_edits(void) {
    ServerSim *sim = make_fog_sim();
    uint8_t value[3];
    UT_ASSERT(sim != NULL);

    /* A good payload of each is accepted and moves the value. */
    value[0] = (uint8_t)overviewWindowClassic;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_OVERVIEW_WINDOW,
                                             value, 1),
                  "a valid overview-window payload was refused");
    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowClassic,
                  "overview window after the edit = %u, want classic",
                  (unsigned)serverSimGetOverviewWindow(sim));
    value[0] = (uint8_t)lineOfSightBuildingsAndTrees;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_LINE_OF_SIGHT,
                                             value, 1),
                  "a valid line-of-sight payload was refused");
    UT_ASSERT_MSG(serverSimGetLineOfSight(sim) ==
                      (uint8_t)lineOfSightBuildingsAndTrees,
                  "line of sight after the edit = %u, want buildings and "
                  "trees", (unsigned)serverSimGetLineOfSight(sim));

    /* Both settings are one byte wide, so a payload of any other length
     * is refused and changes nothing. */
    value[0] = (uint8_t)overviewWindowExpanded;
    value[1] = 0;
    value[2] = 0;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_OVERVIEW_WINDOW, value, 0));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_OVERVIEW_WINDOW, value, 2));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_OVERVIEW_WINDOW, value, 3));
    value[0] = (uint8_t)lineOfSightOff;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_LINE_OF_SIGHT, value, 0));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_LINE_OF_SIGHT, value, 2));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_LINE_OF_SIGHT, value, 3));
    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowClassic &&
                  serverSimGetLineOfSight(sim) ==
                      (uint8_t)lineOfSightBuildingsAndTrees,
                  "a wrong-length payload still moved a value "
                  "(window %u, sight %u)",
                  (unsigned)serverSimGetOverviewWindow(sim),
                  (unsigned)serverSimGetLineOfSight(sim));

    /* A byte at or above the enum's count is refused rather than quietly
     * ignored: the lobby offers a fixed set of choices, so anything else
     * is a malformed command. */
    value[0] = (uint8_t)OVERVIEW_WINDOW_COUNT;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_OVERVIEW_WINDOW, value, 1));
    value[0] = 200;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_OVERVIEW_WINDOW, value, 1));
    value[0] = (uint8_t)LINE_OF_SIGHT_COUNT;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_LINE_OF_SIGHT, value, 1));
    value[0] = 200;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_LINE_OF_SIGHT, value, 1));
    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowClassic &&
                  serverSimGetLineOfSight(sim) ==
                      (uint8_t)lineOfSightBuildingsAndTrees,
                  "an out-of-range payload still moved a value "
                  "(window %u, sight %u)",
                  (unsigned)serverSimGetOverviewWindow(sim),
                  (unsigned)serverSimGetLineOfSight(sim));

    /* Each code maps to its own lock bit. Without the mapping the command
     * dispatcher reads the unknown sentinel and drops the edit, so the
     * lobby control would look live and do nothing. */
    UT_ASSERT_MSG(serverSimGetSettingLockBit(LST_OVERVIEW_WINDOW) ==
                      LOBBY_LOCK_OVERVIEW_WINDOW,
                  "LST_OVERVIEW_WINDOW maps to 0x%08X, want 0x%08X",
                  (unsigned)serverSimGetSettingLockBit(LST_OVERVIEW_WINDOW),
                  (unsigned)LOBBY_LOCK_OVERVIEW_WINDOW);
    UT_ASSERT_MSG(serverSimGetSettingLockBit(LST_LINE_OF_SIGHT) ==
                      LOBBY_LOCK_LINE_OF_SIGHT,
                  "LST_LINE_OF_SIGHT maps to 0x%08X, want 0x%08X",
                  (unsigned)serverSimGetSettingLockBit(LST_LINE_OF_SIGHT),
                  (unsigned)LOBBY_LOCK_LINE_OF_SIGHT);

    serverSimDestroy(sim);
    return 0;
}

int run_fog_settings_lobby_reset(void) {
    ServerSim *sim = make_fog_sim();
    ServerInstanceConfig cfg;
    UT_ASSERT(sim != NULL);

    /* The operator starts the server with the narrow window and sight
     * blocked. */
    serverSimSetOverviewWindow(sim, (uint8_t)overviewWindowClassic);
    serverSimSetLineOfSight(sim, (uint8_t)lineOfSightBuildingsAndTrees);

    /* serverSimApplyInstanceConfig is what server startup runs, and its
     * tail captures the snapshot serverSimResetLobbyToDefaults restores
     * from. A zeroed config leaves every other startup field alone. */
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);

    /* A visiting host moves both. */
    serverSimSetOverviewWindow(sim, (uint8_t)overviewWindowExpanded);
    serverSimSetLineOfSight(sim, (uint8_t)lineOfSightOff);

    /* Last human out. */
    serverSimResetLobbyToDefaults(sim);

    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowClassic,
                  "overview window after reset = %u, want classic",
                  (unsigned)serverSimGetOverviewWindow(sim));
    UT_ASSERT_MSG(serverSimGetLineOfSight(sim) ==
                      (uint8_t)lineOfSightBuildingsAndTrees,
                  "line of sight after reset = %u, want buildings and trees",
                  (unsigned)serverSimGetLineOfSight(sim));

    serverSimDestroy(sim);

    /* The other direction: a server that starts on the defaults gets
     * them back after a host changed both. */
    sim = make_fog_sim();
    UT_ASSERT(sim != NULL);
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);

    serverSimSetOverviewWindow(sim, (uint8_t)overviewWindowClassic);
    serverSimSetLineOfSight(sim, (uint8_t)lineOfSightBuildingsAndTrees);
    serverSimResetLobbyToDefaults(sim);

    UT_ASSERT_MSG(serverSimGetOverviewWindow(sim) ==
                      (uint8_t)overviewWindowExpanded,
                  "the reset must restore expanded, not leave %u",
                  (unsigned)serverSimGetOverviewWindow(sim));
    UT_ASSERT_MSG(serverSimGetLineOfSight(sim) == (uint8_t)lineOfSightOff,
                  "the reset must restore off, not leave %u",
                  (unsigned)serverSimGetLineOfSight(sim));

    serverSimDestroy(sim);
    return 0;
}
