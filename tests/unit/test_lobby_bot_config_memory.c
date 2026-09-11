/*
 * Coverage for the remembered bot mode / difficulty
 * (serverSimApplyLastBotConfig, src/server/sim/server_sim_lobby.c).
 *
 * The host picks a mode and difficulty for one bot and every bot added
 * afterwards starts there, so setting up a lobby means choosing once rather
 * than once per bot. The pair is remembered as the brain's own KEY strings,
 * not the indices kept in botConfigs: an index only means something against
 * one manifest, so carrying the number onto a bot running a different brain
 * would silently select the wrong mode.
 *
 * These tests drive serverSimApplyLastBotConfig directly rather than through
 * CMD_LOBBY_ADD_BOT, for the reason the add-bot dispatch tests give: the
 * happy path there pulls in botManagerAddBot -> ClientSim alloc and needs a
 * real brain to load. The function under test is the whole feature — the
 * dispatcher's only part is calling it immediately before the add.
 *
 * The manifest used is the shipped GoalHunter one (Brains/GoalHunter_1.7/
 * modes.txt, copied next to the test binary by the build): modes "default"
 * and "survival", each with levels easy/medium/hard defaulting to hard.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "everard_map.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lastBotModeKey / botConfigs — seeded and
                                    * read directly; there is no accessor and
                                    * the feature is entirely about them */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "brain_list.h"            /* BrainModes — to name the expected indices */
#include "test_harness.h"

#define TEST_BRAIN "Brains/GoalHunter_1.7/init.lua"
#define TEST_SLOT  3

static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* Index of a mode/level key pair in the test brain's own manifest, so the
 * expectations below survive someone reordering modes.txt. Returns false
 * when the manifest is missing, which the callers report as a skip rather
 * than a failure — a test binary run from outside the build tree has no
 * Brains/ beside it. */
static bool expected_indices(const char *modeKey, const char *lvlKey,
                             int *outMode, int *outLevel) {
    BrainModes modes;
    int m;
    if (!brainListLoadModesForPath(TEST_BRAIN, &modes)) return false;
    m = brainModesFindMode(&modes, modeKey);
    if (m < 0) return false;
    *outMode  = m;
    *outLevel = brainModeFindLevel(&modes.modes[m], lvlKey);
    return (*outLevel >= 0);
}

/* The host picked survival/easy: the next bot added starts there. */
int run_lobby_bot_config_memory_applies(void) {
    ServerSim *sim;
    int wantMode = 0, wantLevel = 0;

    if (!expected_indices("survival", "easy", &wantMode, &wantLevel)) {
        fprintf(stderr, "SKIP: no GoalHunter modes.txt beside the test "
                        "binary — nothing to resolve keys against\n");
        return 0;
    }
    sim = make_lobby_sim();
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    SDL_strlcpy(sim->lastBotModeKey, "survival", sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "easy", sizeof(sim->lastBotLevelKey));
    serverSimApplyLastBotConfig(sim, TEST_SLOT, TEST_BRAIN);

    UT_ASSERT_MSG((wantMode) == ((int)sim->botConfigs[TEST_SLOT].mode),
                  "remembered mode applied to the new bot" " (got %d, want %d)",
                  (int)((int)sim->botConfigs[TEST_SLOT].mode), (int)(wantMode));
    UT_ASSERT_MSG((wantLevel) == ((int)sim->botConfigs[TEST_SLOT].difficulty),
                  "remembered difficulty applied to the new bot" " (got %d, want %d)",
                  (int)((int)sim->botConfigs[TEST_SLOT].difficulty), (int)(wantLevel));
    serverSimDestroy(sim);
    return 0;
}

/* Nothing picked yet this lobby: the slot keeps the ordinary default, which
 * is Hard rather than the memset's 0 (see serverSimResetLobbyToDefaults). */
int run_lobby_bot_config_memory_empty_is_noop(void) {
    ServerSim *sim = make_lobby_sim();
    uint8_t mode0, diff0;
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    mode0 = sim->botConfigs[TEST_SLOT].mode;
    diff0 = sim->botConfigs[TEST_SLOT].difficulty;
    sim->lastBotModeKey[0]  = '\0';
    sim->lastBotLevelKey[0] = '\0';
    serverSimApplyLastBotConfig(sim, TEST_SLOT, TEST_BRAIN);

    UT_ASSERT_MSG(((int)mode0) == ((int)sim->botConfigs[TEST_SLOT].mode),
                  "no memory yet: mode untouched" " (got %d, want %d)",
                  (int)((int)sim->botConfigs[TEST_SLOT].mode), (int)((int)mode0));
    UT_ASSERT_MSG(((int)diff0) == ((int)sim->botConfigs[TEST_SLOT].difficulty),
                  "no memory yet: difficulty untouched" " (got %d, want %d)",
                  (int)((int)sim->botConfigs[TEST_SLOT].difficulty), (int)((int)diff0));
    serverSimDestroy(sim);
    return 0;
}

/* A mode this bot's brain does not declare is dropped, not forced: the slot
 * keeps its default rather than taking an index that means something else
 * here. Same outcome for a brain that ships no manifest at all. */
int run_lobby_bot_config_memory_unknown_key_ignored(void) {
    ServerSim *sim = make_lobby_sim();
    uint8_t mode0, diff0;
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    mode0 = sim->botConfigs[TEST_SLOT].mode;
    diff0 = sim->botConfigs[TEST_SLOT].difficulty;

    SDL_strlcpy(sim->lastBotModeKey, "no_such_mode",
                sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "hard", sizeof(sim->lastBotLevelKey));
    serverSimApplyLastBotConfig(sim, TEST_SLOT, TEST_BRAIN);
    UT_ASSERT_MSG(((int)mode0) == ((int)sim->botConfigs[TEST_SLOT].mode),
                  "unknown mode key leaves the slot alone" " (got %d, want %d)",
                  (int)((int)sim->botConfigs[TEST_SLOT].mode), (int)((int)mode0));
    UT_ASSERT_MSG(((int)diff0) == ((int)sim->botConfigs[TEST_SLOT].difficulty),
                  "unknown mode key leaves the difficulty alone" " (got %d, want %d)",
                  (int)((int)sim->botConfigs[TEST_SLOT].difficulty), (int)((int)diff0));

    /* A brain with no modes.txt reads no mode= token either. */
    SDL_strlcpy(sim->lastBotModeKey, "survival", sizeof(sim->lastBotModeKey));
    serverSimApplyLastBotConfig(sim, TEST_SLOT, "brains/no_such_brain.lua");
    UT_ASSERT_MSG(((int)mode0) == ((int)sim->botConfigs[TEST_SLOT].mode),
                  "brain without a manifest leaves the slot alone" " (got %d, want %d)",
                  (int)((int)sim->botConfigs[TEST_SLOT].mode), (int)((int)mode0));

    serverSimDestroy(sim);
    return 0;
}
