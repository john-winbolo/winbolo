/*
 * View-policy lobby settings: the LST_PILL_VIEW / LST_BASE_VIEW /
 * LST_ALLY_VIEW apply path and the sim's startup defaults.
 *
 * Four things are pinned here:
 *
 *   1. A fresh sim starts with pills and allied tanks always visible,
 *      bases off, and a 30-second decay for all three. Nothing reads
 *      these values yet, so a drift in the defaults would otherwise go
 *      unnoticed until the consumers land.
 *
 *   2. serverSimApplyLobbySetting validates the 3-byte payload the way
 *      the other enum-valued settings do: wrong length or a policy byte
 *      above viewPolicyOff is refused outright, and the decay seconds
 *      are clamped into VIEW_DECAY_MIN_SECS..VIEW_DECAY_MAX_SECS rather
 *      than stored raw.
 *
 *   3. Each category has its own lock bit, so locking one view setting
 *      leaves the other two editable. The apply helper itself is not
 *      lock-aware by design (its callers gate on
 *      serverSimIsSettingLocked before calling), so that is what the
 *      lock case checks.
 *
 *   4. The view policies ride the originalLobbySettings snapshot, so a
 *      visiting host's edits are undone when the last human leaves and
 *      serverSimResetLobbyToDefaults restores the operator's startup
 *      configuration — the same treatment every other lobby setting
 *      already gets.
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

static ServerSim *make_view_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
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

int run_view_policy_defaults(void) {
    ServerSim *sim = make_view_sim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey,
                  "pill view default = %d, want key",
                  (int)serverSimGetViewPolicy(sim, viewCategoryPill));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyOff,
                  "base view default = %d, want off",
                  (int)serverSimGetViewPolicy(sim, viewCategoryBase));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyOff,
                  "ally view default = %d, want off",
                  (int)serverSimGetViewPolicy(sim, viewCategoryAlly));

    for (int vc = 0; vc < VIEW_CATEGORY_COUNT; vc++) {
        UT_ASSERT_MSG(serverSimGetViewDecaySecs(sim, (ViewCategory)vc)
                          == VIEW_DECAY_DEFAULT_SECS,
                      "decay default for category %d = %u, want %d",
                      vc, (unsigned)serverSimGetViewDecaySecs(sim, (ViewCategory)vc),
                      VIEW_DECAY_DEFAULT_SECS);
    }

    /* NULL-safe / out-of-range reads fall back to the same values the
     * wire decoder's zero-init would produce. */
    UT_ASSERT(serverSimGetViewPolicy(NULL, viewCategoryPill) == viewPolicyAlways);
    UT_ASSERT(serverSimGetViewDecaySecs(NULL, viewCategoryPill) == VIEW_DECAY_DEFAULT_SECS);
    UT_ASSERT(serverSimGetViewPolicy(sim, (ViewCategory)VIEW_CATEGORY_COUNT)
                  == viewPolicyAlways);

    serverSimDestroy(sim);
    return 0;
}

int run_view_policy_apply_validates(void) {
    ServerSim *sim = make_view_sim();
    uint8_t value[3];
    UT_ASSERT(sim != NULL);

    /* Good payload lands on the right category and leaves the others
     * alone. */
    fill_view_payload(value, viewPolicyDecay, 120);
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_PILL_VIEW, value, 3),
                  "a valid pill-view payload was refused");
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyDecay);
    UT_ASSERT(serverSimGetViewDecaySecs(sim, viewCategoryPill) == 120);
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyOff,
                  "applying pill view disturbed the base view");
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyOff);

    /* A policy byte above viewPolicyOff is refused and changes nothing
     * — same shape as LST_AI_POLICY refusing an aiType above 3. */
    fill_view_payload(value, 4, 60);
    UT_ASSERT_MSG(!serverSimApplyLobbySetting(sim, LST_BASE_VIEW, value, 3),
                  "policy byte 4 should have been refused");
    fill_view_payload(value, 255, 60);
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_BASE_VIEW, value, 3));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyOff,
                  "a refused payload still changed the base view");

    /* Wrong payload length is refused too. */
    fill_view_payload(value, viewPolicyKey, 60);
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_BASE_VIEW, value, 1));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_BASE_VIEW, value, 2));
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyOff);

    /* Decay seconds clamp rather than reject: below the floor, above
     * the ceiling, and both boundaries pass through unchanged. */
    fill_view_payload(value, viewPolicyDecay, 0);
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLY_VIEW, value, 3));
    UT_ASSERT_MSG(serverSimGetViewDecaySecs(sim, viewCategoryAlly) == VIEW_DECAY_MIN_SECS,
                  "0 seconds should clamp to %d, got %u", VIEW_DECAY_MIN_SECS,
                  (unsigned)serverSimGetViewDecaySecs(sim, viewCategoryAlly));

    fill_view_payload(value, viewPolicyDecay, 65535);
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLY_VIEW, value, 3));
    UT_ASSERT_MSG(serverSimGetViewDecaySecs(sim, viewCategoryAlly) == VIEW_DECAY_MAX_SECS,
                  "65535 seconds should clamp to %d, got %u", VIEW_DECAY_MAX_SECS,
                  (unsigned)serverSimGetViewDecaySecs(sim, viewCategoryAlly));

    fill_view_payload(value, viewPolicyDecay, VIEW_DECAY_MIN_SECS);
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLY_VIEW, value, 3));
    UT_ASSERT(serverSimGetViewDecaySecs(sim, viewCategoryAlly) == VIEW_DECAY_MIN_SECS);

    fill_view_payload(value, viewPolicyDecay, VIEW_DECAY_MAX_SECS);
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLY_VIEW, value, 3));
    UT_ASSERT(serverSimGetViewDecaySecs(sim, viewCategoryAlly) == VIEW_DECAY_MAX_SECS);

    /* The direct setter clamps identically and drops an out-of-range
     * category or policy on the floor. */
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyKey, 1);
    UT_ASSERT(serverSimGetViewDecaySecs(sim, viewCategoryPill) == VIEW_DECAY_MIN_SECS);
    serverSimSetViewPolicy(sim, viewCategoryPill, (ViewPolicy)9, 60);
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey,
                  "an out-of-range policy should have been ignored");
    serverSimSetViewPolicy(sim, (ViewCategory)VIEW_CATEGORY_COUNT,
                           viewPolicyOff, 60);
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyKey);

    serverSimDestroy(sim);
    return 0;
}

int run_view_policy_lock_bits(void) {
    ServerSim *sim = make_view_sim();
    uint8_t value[3];
    UT_ASSERT(sim != NULL);

    /* Unlocked: every view setting is editable. */
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_PILL_VIEW));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_BASE_VIEW));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_ALLY_VIEW));

    /* Locking one category leaves the other two alone — the callers of
     * serverSimApplyLobbySetting consult this before applying, so a
     * shared bit would silently freeze the wrong control. */
    serverSimSetServerLocks(sim, LOBBY_LOCK_PILL_VIEW);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_PILL_VIEW));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_BASE_VIEW));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_ALLY_VIEW));
    UT_ASSERT_MSG(!serverSimIsSettingLocked(sim, LST_RANKED),
                  "the pill-view lock must not gate an unrelated setting");

    serverSimSetServerLocks(sim, LOBBY_LOCK_ALLY_VIEW);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_ALLY_VIEW));
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_PILL_VIEW));

    /* The apply helper is deliberately not lock-aware: a caller that
     * has already cleared the lock gate still gets its change through
     * while the mask is set. */
    fill_view_payload(value, viewPolicyKey, 60);
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLY_VIEW, value, 3));
    UT_ASSERT(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyKey);

    serverSimDestroy(sim);
    return 0;
}

int run_view_policy_lobby_reset(void) {
    ServerSim *sim = make_view_sim();
    ServerInstanceConfig cfg;
    uint8_t value[3];
    UT_ASSERT(sim != NULL);

    /* The operator's startup configuration. Deliberately different from
     * both the create-time defaults and the lobby edits below, so the
     * assertions prove the captured snapshot came back rather than the
     * sim merely falling back to how it was created. */
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyDecay, 90);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyKey, 45);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyOff, 15);

    /* serverSimApplyInstanceConfig is what server startup runs, and its
     * tail captures the snapshot serverSimResetLobbyToDefaults restores
     * from. A zeroed config leaves every other startup field alone. */
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);

    /* A visiting host edits all three from the lobby. */
    fill_view_payload(value, viewPolicyOff, 600);
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_PILL_VIEW, value, 3));
    fill_view_payload(value, viewPolicyAlways, VIEW_DECAY_MIN_SECS);
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_BASE_VIEW, value, 3));
    fill_view_payload(value, viewPolicyDecay, 300);
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_ALLY_VIEW, value, 3));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyOff,
                  "the lobby edit did not take");

    /* Last human out. */
    serverSimResetLobbyToDefaults(sim);

    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryPill) == viewPolicyDecay,
                  "pill view after reset = %d, want decay",
                  (int)serverSimGetViewPolicy(sim, viewCategoryPill));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryBase) == viewPolicyKey,
                  "base view after reset = %d, want key",
                  (int)serverSimGetViewPolicy(sim, viewCategoryBase));
    UT_ASSERT_MSG(serverSimGetViewPolicy(sim, viewCategoryAlly) == viewPolicyOff,
                  "ally view after reset = %d, want off",
                  (int)serverSimGetViewPolicy(sim, viewCategoryAlly));
    UT_ASSERT_MSG(serverSimGetViewDecaySecs(sim, viewCategoryPill) == 90,
                  "pill decay after reset = %u, want 90",
                  (unsigned)serverSimGetViewDecaySecs(sim, viewCategoryPill));
    UT_ASSERT_MSG(serverSimGetViewDecaySecs(sim, viewCategoryBase) == 45,
                  "base decay after reset = %u, want 45",
                  (unsigned)serverSimGetViewDecaySecs(sim, viewCategoryBase));
    UT_ASSERT_MSG(serverSimGetViewDecaySecs(sim, viewCategoryAlly) == 15,
                  "ally decay after reset = %u, want 15",
                  (unsigned)serverSimGetViewDecaySecs(sim, viewCategoryAlly));

    serverSimDestroy(sim);
    return 0;
}
