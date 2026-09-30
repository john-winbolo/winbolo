/*
 * Allies in trees: the LST_ALLIES_IN_TREES apply path, its interaction
 * with classic mode, and the startup snapshot.
 *
 * Two things are pinned here:
 *
 *   1. A fresh sim has the option off — the classic behaviour, where an
 *      ally standing in trees is never visible past the viewer's own
 *      immediate sight box. Nothing reads the flag yet, so a drift in the
 *      default would otherwise go unnoticed until the consumers land. The
 *      apply case takes exactly one byte and reads any non-zero value as
 *      on, matching the other bool settings.
 *
 *   2. Classic mode hides allies in trees, so it owns this value the same
 *      way it owns the three view policies: turning classic mode on forces
 *      the option off, and an edit is refused while it stays on. The flag
 *      also rides the originalLobbySettings snapshot, so a visiting host's
 *      change is undone when the last human leaves and
 *      serverSimResetLobbyToDefaults restores the operator's startup
 *      configuration.
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

static ServerSim *make_trees_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

int run_allies_in_trees_defaults(void) {
    ServerSim *sim = make_trees_sim();
    uint8_t value[3];
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!serverSimGetAlliesInTrees(sim),
                  "a fresh sim must hide allies in trees");

    /* NULL-safe read matches the off default. */
    UT_ASSERT(!serverSimGetAlliesInTrees(NULL));
    serverSimSetAlliesInTrees(NULL, true);  /* NULL-safe */

    /* On, then off, through the lobby apply path. */
    value[0] = 1;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 1),
                  "a valid allies-in-trees payload was refused");
    UT_ASSERT(serverSimGetAlliesInTrees(sim));

    value[0] = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 1));
    UT_ASSERT(!serverSimGetAlliesInTrees(sim));

    /* Any non-zero byte reads as on, matching the other bool settings. */
    value[0] = 2;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 1));
    UT_ASSERT_MSG(serverSimGetAlliesInTrees(sim),
                  "a non-zero byte other than 1 must read as on");

    /* A payload of the wrong length is refused and changes nothing. */
    value[0] = 0;
    value[1] = 0;
    value[2] = 0;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 0));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 2));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 3));
    UT_ASSERT_MSG(serverSimGetAlliesInTrees(sim),
                  "a refused payload still cleared allies in trees");

    /* The lock bit is the setting's own, distinct from every other. */
    UT_ASSERT_MSG(serverSimGetSettingLockBit(LST_ALLIES_IN_TREES)
                      == LOBBY_LOCK_ALLIES_IN_TREES,
                  "LST_ALLIES_IN_TREES lock bit = 0x%04X, want 0x%04X",
                  (unsigned)serverSimGetSettingLockBit(LST_ALLIES_IN_TREES),
                  (unsigned)LOBBY_LOCK_ALLIES_IN_TREES);
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_ALLIES_IN_TREES));
    serverSimSetServerLocks(sim, LOBBY_LOCK_ALLIES_IN_TREES);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_ALLIES_IN_TREES));
    /* Classic mode forces this option off, so locking the option has to
     * lock classic mode as well — otherwise the checkbox is a way round
     * the lock. */
    UT_ASSERT_MSG(serverSimIsSettingLocked(sim, LST_CLASSIC_MODE),
                  "the allies-in-trees lock must also cover classic mode");

    serverSimDestroy(sim);
    return 0;
}

int run_allies_in_trees_classic_mode(void) {
    ServerSim *sim = make_trees_sim();
    ServerInstanceConfig cfg;
    uint8_t value[1];
    UT_ASSERT(sim != NULL);

    /* Turning classic mode on forces the option off. */
    value[0] = 1;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 1));
    UT_ASSERT(serverSimGetAlliesInTrees(sim));
    serverSimSetClassicMode(sim, true);
    UT_ASSERT_MSG(!serverSimGetAlliesInTrees(sim),
                  "classic mode must turn allies in trees off");

    /* While classic mode is on, an edit is refused in both directions. */
    value[0] = 1;
    UT_ASSERT_MSG(!serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 1),
                  "an allies-in-trees edit should be refused under classic mode");
    UT_ASSERT(!serverSimGetAlliesInTrees(sim));
    value[0] = 0;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 1));

    /* Off. The value stays where classic mode put it, and the setting
     * becomes editable again. */
    serverSimSetClassicMode(sim, false);
    UT_ASSERT_MSG(!serverSimGetAlliesInTrees(sim),
                  "leaving classic mode must leave allies in trees off");
    value[0] = 1;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 1),
                  "an edit should be allowed once classic mode is off");
    UT_ASSERT(serverSimGetAlliesInTrees(sim));

    serverSimDestroy(sim);

    /* The operator starts the server with the option on; a visiting host
     * turns it off; the reset restores the startup value. */
    sim = make_trees_sim();
    UT_ASSERT(sim != NULL);
    serverSimSetAlliesInTrees(sim, true);

    /* serverSimApplyInstanceConfig is what server startup runs, and its
     * tail captures the snapshot serverSimResetLobbyToDefaults restores
     * from. A zeroed config leaves every other startup field alone. */
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);

    value[0] = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 1));
    UT_ASSERT(!serverSimGetAlliesInTrees(sim));

    serverSimResetLobbyToDefaults(sim);
    UT_ASSERT_MSG(serverSimGetAlliesInTrees(sim),
                  "allies in trees was not restored from the startup snapshot");

    serverSimDestroy(sim);

    /* The other direction: a server that starts with the option off gets
     * it back off after a host turned it on. */
    sim = make_trees_sim();
    UT_ASSERT(sim != NULL);
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);
    UT_ASSERT(!serverSimGetAlliesInTrees(sim));

    value[0] = 1;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLIES_IN_TREES, value, 1));
    UT_ASSERT(serverSimGetAlliesInTrees(sim));

    serverSimResetLobbyToDefaults(sim);
    UT_ASSERT_MSG(!serverSimGetAlliesInTrees(sim),
                  "the reset must restore allies in trees off, not leave it on");

    serverSimDestroy(sim);
    return 0;
}
