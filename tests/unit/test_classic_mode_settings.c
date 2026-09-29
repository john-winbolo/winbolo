/*
 * Classic mode: the LST_CLASSIC_MODE apply path, the values it forces
 * onto the three view policies, and its lock bit.
 *
 * Five things are pinned here:
 *
 *   1. A fresh sim is not in classic mode, and the three view policies
 *      are still the shipped defaults. Nothing on the client reads the
 *      flag yet, so a drift in the default would otherwise go unnoticed
 *      until the consumers land.
 *
 *   2. Turning classic mode on writes pill = key, base = off and
 *      ally = off, keeping each category's own decay seconds, and the
 *      lobby then refuses an edit to any of those three. Turning it off
 *      clears the flag and nothing else — the policies stay where
 *      classic mode put them, which is the deliberate consequence of
 *      setting the values rather than masking them at read time.
 *
 *   3. Classic mode has its own lock bit, distinct from every other
 *      LOBBY_LOCK_*, so locking it leaves the rest editable. The apply
 *      helper itself is not lock-aware by design (its callers check
 *      serverSimIsSettingLocked before calling), the same as the
 *      view-policy settings.
 *
 *   4. The lock runs one way only. Locking classic mode leaves the four
 *      settings it writes editable, but locking any of those four also
 *      locks classic mode — otherwise a host could change a locked value
 *      by ticking the checkbox, and could not undo it afterwards.
 *
 *   5. The flag rides the originalLobbySettings snapshot, so a visiting
 *      host's change is undone when the last human leaves and
 *      serverSimResetLobbyToDefaults restores the operator's startup
 *      configuration.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_command.h"               /* CMD_LOBBY_SETTING, CmdResult */
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "server_lifecycle.h"             /* ServerInstanceConfig */
#include "server/sim/server_sim_shared.h" /* serverSimResetLobbyToDefaults */
#include "threads.h"                      /* the dispatcher asserts the mutex */
#include "view_policy.h"
#include "wire_limits.h"
#include "everard_map.h"
#include "test_harness.h"

static ServerSim *make_classic_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* Build the [policy][decay hi][decay lo] payload the wire carries. */
static void fill_view_payload(uint8_t out[3], int policy, int secs) {
    out[0] = (uint8_t)policy;
    out[1] = (uint8_t)((secs >> 8) & 0xFF);
    out[2] = (uint8_t)(secs & 0xFF);
}

int run_classic_mode_defaults(void) {
    ServerSim *sim = make_classic_sim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!serverSimGetClassicMode(sim),
                  "a fresh sim must not be in classic mode");

    /* The three view policies are untouched by the new field. */
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey,
                  "pill view default = %d, want key",
                  (int)serverSimGetViewPolicy(sim, viewCategoryPill));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyOff,
                  "base view default = %d, want off",
                  (int)serverSimGetViewPolicy(sim, viewCategoryBase));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyOff,
                  "ally view default = %d, want off",
                  (int)serverSimGetViewPolicy(sim, viewCategoryAlly));

    /* NULL-safe read matches the off default. */
    UT_ASSERT(!serverSimGetClassicMode(NULL));

    serverSimDestroy(sim);
    return 0;
}

int run_classic_mode_forces_views(void) {
    ServerSim *sim = make_classic_sim();
    uint8_t value[3];
    UT_ASSERT(sim != NULL);

    /* Three decay values the host chose, deliberately different from
     * each other and from the default, so the assertions below prove
     * each category kept its own rather than being reset. */
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyAlways, 90);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyDecay,  45);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyAlways, 15);

    /* On. */
    value[0] = 1;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_CLASSIC_MODE, value, 1),
                  "a valid classic-mode payload was refused");
    UT_ASSERT(serverSimGetClassicMode(sim));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey,
                  "pill view under classic mode = %d, want key",
                  (int)serverSimGetViewPolicy(sim, viewCategoryPill));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyOff,
                  "base view under classic mode = %d, want off",
                  (int)serverSimGetViewPolicy(sim, viewCategoryBase));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyOff,
                  "ally view under classic mode = %d, want off",
                  (int)serverSimGetViewPolicy(sim, viewCategoryAlly));
    UT_ASSERT_MSG(serverSimGetViewDecaySecs(sim, viewCategoryPill) == 90 &&
                  serverSimGetViewDecaySecs(sim, viewCategoryBase) == 45 &&
                  serverSimGetViewDecaySecs(sim, viewCategoryAlly) == 15,
                  "classic mode disturbed the decay seconds (got %u/%u/%u)",
                  (unsigned)serverSimGetViewDecaySecs(sim, viewCategoryPill),
                  (unsigned)serverSimGetViewDecaySecs(sim, viewCategoryBase),
                  (unsigned)serverSimGetViewDecaySecs(sim, viewCategoryAlly));

    /* While it is on, the three view settings refuse an edit. */
    fill_view_payload(value, viewPolicyAlways, 60);
    UT_ASSERT_MSG(!serverSimApplyLobbySetting(sim, LST_PILL_VIEW, value, 3),
                  "a pill-view edit should be refused under classic mode");
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_BASE_VIEW, value, 3));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_ALLY_VIEW, value, 3));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey,
                  "a refused edit still changed the pill view");

    /* A payload of the wrong length is refused and changes nothing. */
    value[0] = 0;
    value[1] = 0;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_CLASSIC_MODE, value, 0));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_CLASSIC_MODE, value, 2));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_CLASSIC_MODE, value, 3));
    UT_ASSERT_MSG(serverSimGetClassicMode(sim),
                  "a refused payload still cleared classic mode");

    /* Any non-zero byte reads as on, matching the other bool settings. */
    value[0] = 2;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_CLASSIC_MODE, value, 1));
    UT_ASSERT(serverSimGetClassicMode(sim));

    /* Off. The flag clears; the three policies stay where classic mode
     * put them, and the settings become editable again. */
    value[0] = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_CLASSIC_MODE, value, 1));
    UT_ASSERT(!serverSimGetClassicMode(sim));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey,
                  "turning classic mode off must leave the pill view at key");
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyOff);
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyOff);

    fill_view_payload(value, viewPolicyAlways, 60);
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_PILL_VIEW, value, 3),
                  "a pill-view edit should be allowed once classic mode is off");
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyAlways);

    /* The direct setter behaves the same as the lobby path — it is what
     * the -classicmode command line switch calls. */
    serverSimSetClassicMode(sim, true);
    UT_ASSERT(serverSimGetClassicMode(sim));
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey);
    serverSimSetClassicMode(sim, false);
    UT_ASSERT(!serverSimGetClassicMode(sim));
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey);
    serverSimSetClassicMode(NULL, true);  /* NULL-safe */

    serverSimDestroy(sim);
    return 0;
}

int run_classic_mode_lock_bit(void) {
    ServerSim *sim = make_classic_sim();
    uint8_t value[1];
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(serverSimGetSettingLockBit(LST_CLASSIC_MODE)
                      == LOBBY_LOCK_CLASSIC_MODE,
                  "LST_CLASSIC_MODE lock bit = 0x%08X, want 0x%08X",
                  (unsigned)serverSimGetSettingLockBit(LST_CLASSIC_MODE),
                  (unsigned)LOBBY_LOCK_CLASSIC_MODE);

    /* The new bit must not collide with any existing one — a shared bit
     * would freeze the wrong control on every client. */
    {
        uint32_t others =
            LOBBY_LOCK_GAME_TYPE | LOBBY_LOCK_AI_POLICY | LOBBY_LOCK_MINES |
            LOBBY_LOCK_TIME_LIMIT | LOBBY_LOCK_AUTO_LOCK_ON_GAME |
            LOBBY_LOCK_PASSWORD | LOBBY_LOCK_RANKED | LOBBY_LOCK_OPEN_HOST |
            LOBBY_LOCK_MAP | LOBBY_LOCK_PILL_VIEW | LOBBY_LOCK_BASE_VIEW |
            LOBBY_LOCK_ALLY_VIEW;
        UT_ASSERT_MSG((others & LOBBY_LOCK_CLASSIC_MODE) == 0u,
                      "LOBBY_LOCK_CLASSIC_MODE collides with an existing bit");
    }

    /* Unlocked by default; locking classic mode leaves the three view
     * settings editable and vice versa. */
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_CLASSIC_MODE));
    serverSimSetServerLocks(sim, LOBBY_LOCK_CLASSIC_MODE);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_CLASSIC_MODE));
    UT_ASSERT_MSG(!serverSimIsSettingLocked(sim, LST_PILL_VIEW),
                  "the classic-mode lock must not gate the pill-view setting");
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_BASE_VIEW));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_ALLY_VIEW));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_RANKED));

    /* The other direction is not symmetric: classic mode writes the
     * pill view, so a pill-view lock has to cover classic mode too or
     * the host reaches the locked value through the checkbox. */
    serverSimSetServerLocks(sim, LOBBY_LOCK_PILL_VIEW);
    UT_ASSERT_MSG(serverSimIsSettingLocked(sim, LST_CLASSIC_MODE),
                  "the pill-view lock must also lock classic mode");

    /* The apply helper is deliberately not lock-aware: a caller that has
     * already checked the lock still gets its change through while the
     * mask is set. Same contract as the view-policy settings. */
    serverSimSetServerLocks(sim, LOBBY_LOCK_CLASSIC_MODE);
    value[0] = 1;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_CLASSIC_MODE, value, 1));
    UT_ASSERT(serverSimGetClassicMode(sim));

    serverSimDestroy(sim);
    return 0;
}

/* Classic mode writes pill / base / ally view and allies in trees, so a
 * lock on any of those four has to reach classic mode as well or the
 * checkbox is a way round the lock. serverSimAddImpliedLocks is where
 * that is settled; this pins the mapping, and
 * run_classic_mode_lock_blocks_dispatch drives it end-to-end. */
int run_classic_mode_lock_implied(void) {
    ServerSim *sim = make_classic_sim();
    static const uint32_t owned[] = {
        LOBBY_LOCK_PILL_VIEW, LOBBY_LOCK_BASE_VIEW,
        LOBBY_LOCK_ALLY_VIEW, LOBBY_LOCK_ALLIES_IN_TREES,
    };
    /* Locks on settings classic mode does not write. LOBBY_LOCK_MAP and
     * LOBBY_LOCK_PASSWORD are in here deliberately: they have no LST_*
     * of their own, so a fold that keyed off "any lock at all" would
     * still catch them and freeze a checkbox for no reason. */
    static const uint32_t unrelated[] = {
        LOBBY_LOCK_GAME_TYPE, LOBBY_LOCK_AI_POLICY, LOBBY_LOCK_MINES,
        LOBBY_LOCK_TIME_LIMIT, LOBBY_LOCK_AUTO_LOCK_ON_GAME,
        LOBBY_LOCK_PASSWORD, LOBBY_LOCK_RANKED, LOBBY_LOCK_OPEN_HOST,
        LOBBY_LOCK_MAP,
    };
    UT_ASSERT(sim != NULL);

    /* An empty mask stays empty — no lock, nothing implied. */
    UT_ASSERT_MSG(serverSimAddImpliedLocks(0u) == 0u,
                  "an empty mask must imply no locks");

    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); i++) {
        uint32_t in  = owned[i];
        uint32_t out = serverSimAddImpliedLocks(in);
        /* Exactly the input plus the classic-mode bit: the fold must add
         * that one bit and nothing else, or an operator who locked the
         * ally view finds unrelated controls frozen too. */
        UT_ASSERT_MSG(out == (uint32_t)(in | LOBBY_LOCK_CLASSIC_MODE),
                      "lock 0x%08X implied 0x%08X, want 0x%08X",
                      (unsigned)in, (unsigned)out,
                      (unsigned)(in | LOBBY_LOCK_CLASSIC_MODE));
        /* Idempotent — the stored mask is re-folded on every lobby
         * reset (serverSimResetLobbyToDefaults restores the snapshot
         * through the same setter), so a second pass must not drift. */
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

    for (size_t i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); i++) {
        uint32_t in = unrelated[i];
        UT_ASSERT_MSG(serverSimAddImpliedLocks(in) == in,
                      "lock 0x%08X must imply nothing (got 0x%08X)",
                      (unsigned)in, (unsigned)serverSimAddImpliedLocks(in));
        serverSimSetServerLocks(sim, in);
        UT_ASSERT_MSG(!serverSimIsSettingLocked(sim, LST_CLASSIC_MODE),
                      "lock 0x%08X must leave classic mode editable",
                      (unsigned)in);
    }

    /* The classic-mode lock on its own still implies nothing: the four
     * settings it writes stay editable, which is the asymmetry
     * run_classic_mode_lock_bit checks from the other side. */
    UT_ASSERT(serverSimAddImpliedLocks(LOBBY_LOCK_CLASSIC_MODE)
                  == LOBBY_LOCK_CLASSIC_MODE);

    serverSimDestroy(sim);
    return 0;
}

/* The scenario the implied lock exists for, driven through the real
 * dispatcher: an operator pins the ally view, a host ticks classic mode,
 * and the locked value must not move. Without the implied lock the
 * command is accepted, the ally view goes to off, and the host cannot
 * put it back — turning classic mode off leaves the values where classic
 * mode wrote them, and a direct ally-view edit is refused by the lock. */
int run_classic_mode_lock_blocks_dispatch(void) {
    ServerSim *sim = make_classic_sim();
    ClientCommand cmd;
    CmdResult r;
    UT_ASSERT(sim != NULL);

    /* Host in slot 0, which is the default hostSlot, so lobbyClientMayEdit
     * passes and the lock is the only thing left to refuse the command. */
    serverSimAddPlayer(sim, 0, "Host", false);
    /* The operator's own choice, set before the lock and deliberately not
     * the value classic mode writes, so the control at the end of the case
     * proves classic mode moved it. */
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyAlways,
                           VIEW_DECAY_DEFAULT_SECS);
    serverSimSetServerLocks(sim, LOBBY_LOCK_ALLY_VIEW);
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyAlways,
                  "precondition: the operator's ally view is always");

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_SETTING;
    cmd.cmdSeq = 1;
    cmd.u.lobbySetting.settingType = LST_CLASSIC_MODE;
    cmd.u.lobbySetting.valueLen    = 1;
    cmd.u.lobbySetting.value[0]    = 1;

    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();

    UT_ASSERT_MSG(r == CMD_REJECT_LOCKED,
                  "classic mode under an ally-view lock returned %d, "
                  "want CMD_REJECT_LOCKED (%d)", (int)r, (int)CMD_REJECT_LOCKED);
    UT_ASSERT_MSG(!serverSimGetClassicMode(sim),
                  "a refused command still turned classic mode on");
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyAlways,
                  "the locked ally view moved to %d",
                  (int)serverSimGetViewPolicy(sim, viewCategoryAlly));

    /* The control: the same command with no locks set is accepted and
     * does move the ally view. Without this the test above would still
     * pass if classic mode were refused for some unrelated reason. */
    serverSimSetServerLocks(sim, 0);
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();

    UT_ASSERT_MSG(r == CMD_OK,
                  "classic mode with no locks returned %d, want CMD_OK", (int)r);
    UT_ASSERT(serverSimGetClassicMode(sim));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyOff,
                  "classic mode did not write the ally view");

    serverSimDestroy(sim);
    return 0;
}

int run_classic_mode_lobby_reset(void) {
    ServerSim *sim = make_classic_sim();
    ServerInstanceConfig cfg;
    uint8_t value[3];
    UT_ASSERT(sim != NULL);

    /* The operator starts the server in classic mode. */
    serverSimSetClassicMode(sim, true);

    /* serverSimApplyInstanceConfig is what server startup runs, and its
     * tail captures the snapshot serverSimResetLobbyToDefaults restores
     * from. A zeroed config leaves every other startup field alone. */
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);

    /* A visiting host turns it off and moves the pill view. */
    value[0] = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_CLASSIC_MODE, value, 1));
    UT_ASSERT(!serverSimGetClassicMode(sim));
    fill_view_payload(value, viewPolicyAlways, 600);
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_PILL_VIEW, value, 3));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyAlways,
                  "the lobby edit did not take");

    /* Last human out. */
    serverSimResetLobbyToDefaults(sim);

    UT_ASSERT_MSG(serverSimGetClassicMode(sim),
                  "classic mode was not restored from the startup snapshot");
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey,
                  "pill view after reset = %d, want key",
                  (int)serverSimGetViewPolicy(sim, viewCategoryPill));

    serverSimDestroy(sim);

    /* The other direction: a server that starts without classic mode
     * gets it back off after a host turned it on. */
    sim = make_classic_sim();
    UT_ASSERT(sim != NULL);
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);
    UT_ASSERT(!serverSimGetClassicMode(sim));

    value[0] = 1;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_CLASSIC_MODE, value, 1));
    UT_ASSERT(serverSimGetClassicMode(sim));

    serverSimResetLobbyToDefaults(sim);
    UT_ASSERT_MSG(!serverSimGetClassicMode(sim),
                  "the reset must restore classic mode off, not leave it on");
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyOff,
                  "ally view after reset = %d, want off",
                  (int)serverSimGetViewPolicy(sim, viewCategoryAlly));

    serverSimDestroy(sim);
    return 0;
}
