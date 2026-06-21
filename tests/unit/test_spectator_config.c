/*
 * Coverage for the -maxspectators / -specdelay config plumbing:
 * ServerInstanceConfig fields flow through serverSimApplyInstanceConfig
 * into the sim and back out via the getters, with seconds converted to
 * ticks at 50 ticks/s.
 *
 * The cases pin the non-fallback semantics that separate these caps from
 * -maxplayers: maxSpectators == 0 means "disabled" (the getter must return
 * 0, not MAX_TANKS), and specDelaySeconds == 0 means "live" (0 ticks).
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "everard_map.h"
#include "server_sim.h"
#include "server_lifecycle.h"      /* ServerInstanceConfig */
#include "server_sim_lifecycle.h"  /* serverSimApplyInstanceConfig */
#include "test_harness.h"

static ServerSim *make_sim(void) {
    BYTE emap[6000] = E_MAP;
    return serverSimCreateCompressed(emap, 5097, "Everard Island",
                                     gameOpen, false, 0, -1);
}

int run_spectator_config(void) {
    /* Case 1: non-zero cap + delay → applied and converted (30 × 50 = 1500). */
    {
        ServerSim *sim = make_sim();
        UT_ASSERT(sim != NULL);
        ServerInstanceConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.maxSpectators    = 8;
        cfg.specDelaySeconds = 30;
        serverSimApplyInstanceConfig(sim, &cfg);
        UT_ASSERT_MSG(serverSimGetMaxSpectators(sim) == 8,
                      "maxSpectators=8 must round-trip through apply");
        UT_ASSERT_MSG(serverSimGetSpecDelayTicks(sim) == 1500,
                      "specDelaySeconds=30 must convert to 1500 ticks (30 x 50)");
        serverSimDestroy(sim);
    }

    /* Case 2: cap 0 stays 0 — disabled is meaningful, no MAX_TANKS fallback. */
    {
        ServerSim *sim = make_sim();
        UT_ASSERT(sim != NULL);
        ServerInstanceConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.maxSpectators    = 0;
        cfg.specDelaySeconds = 90;
        serverSimApplyInstanceConfig(sim, &cfg);
        UT_ASSERT_MSG(serverSimGetMaxSpectators(sim) == 0,
                      "maxSpectators=0 must stay 0 (disabled, no fallback)");
        serverSimDestroy(sim);
    }

    /* Case 3: delay 0 → live (0 ticks). */
    {
        ServerSim *sim = make_sim();
        UT_ASSERT(sim != NULL);
        ServerInstanceConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.maxSpectators    = 4;
        cfg.specDelaySeconds = 0;
        serverSimApplyInstanceConfig(sim, &cfg);
        UT_ASSERT_MSG(serverSimGetSpecDelayTicks(sim) == 0,
                      "specDelaySeconds=0 must convert to 0 ticks (live)");
        serverSimDestroy(sim);
    }

    return 0;
}
