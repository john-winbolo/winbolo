/*
 * Coverage for a new lobby bot's brain mode and difficulty
 * (serverSimResolveNewBotConfig / serverSimRememberManualBotPick,
 * src/server/sim/server_sim_lobby.c) on a plain map — no scenario, so only
 * the base and the host's manual pick are in play. The scenario half is in
 * test_scenario.c.
 *
 * The host picks a difficulty BY HAND for one bot and the next bot added
 * with Add Bot starts there, so setting up a lobby means choosing once
 * rather than once per bot. The pick is remembered as the brain's own KEY
 * strings, not the indices kept in botConfigs, and is recorded only where a
 * person made it: an automatic config write must not pass for the host's
 * choice.
 *
 * The manifest used is a fixture brain's own modes.txt, written by each case
 * (bcmMakeBrain): modes "default" and "turtle", each with levels
 * easy/medium/hard defaulting to hard. The shipped GoalHunter manifest has
 * only one mode, so it cannot show a pick moving the mode.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "everard_map.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lastBotModeKey / botConfigs / botMgr —
                                    * seeded and read directly; the feature
                                    * is entirely about them */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame,
                                    * ReturnToLobby, ReloadCompressedInMemory */
#include "brain_list.h"            /* BrainModes — to name the expected indices */
#include "test_harness.h"

/* The fixture brain. brain_list.c reads a brain's modes.txt by the name of
 * the DIRECTORY holding init.lua, under brains/ of the working directory, so
 * this is a directory and not a bare file. Each case writes its own
 * directory, named after the case: ctest -j runs the cases at once, and a
 * shared one was deleted by one case while another was still reading it. */
#define TEST_SLOT  3
#define TEST_TEAM  1

typedef struct {
    char dir[64];
    char brain[80];
} BcmBrain;

/* Write the fixture brain: an init.lua and a modes.txt with two modes.
 * False when the working directory is not writable, which bcmRun reports
 * as a skip. brains/ is created when missing and never removed, so a case
 * cannot pull it out from under another. */
static bool bcmMakeBrain(const BcmBrain *b) {
    FILE *f;

    SDL_CreateDirectory("brains");
    SDL_CreateDirectory(b->dir);
    f = fopen(b->brain, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    {
        char path[96];
        SDL_snprintf(path, sizeof(path), "%s/modes.txt", b->dir);
        f = fopen(path, "wb");
    }
    if (f == NULL) return false;
    fputs("[default]\n"
          "label = Default\n"
          "levels = easy:Easy:1, medium:Medium:2, hard:Hard:3\n"
          "default = hard\n"
          "\n"
          "[turtle]\n"
          "label = Turtle\n"
          "levels = easy:Easy:1, medium:Medium:2, hard:Hard:3\n"
          "default = hard\n", f);
    fclose(f);
    return true;
}

static void bcmDropBrain(const BcmBrain *b) {
    char path[96];
    SDL_snprintf(path, sizeof(path), "%s/modes.txt", b->dir);
    remove(path);
    remove(b->brain);
    SDL_RemovePath(b->dir);
}

/* Run one case against its own fixture brain. The fixture is dropped
 * however the body returns, so a failed assertion leaves nothing behind in
 * the build directory's brains/. */
static int bcmRun(const char *name, int (*body)(const char *brain)) {
    BcmBrain b;
    int rc;

    SDL_snprintf(b.dir, sizeof(b.dir), "brains/ut_bcm_%s", name);
    SDL_snprintf(b.brain, sizeof(b.brain), "%s/init.lua", b.dir);
    if (!bcmMakeBrain(&b)) {
        fprintf(stderr, "SKIP: the fixture brain could not be written — "
                        "nothing to resolve keys against\n");
        bcmDropBrain(&b);
        return 0;
    }
    rc = body(b.brain);
    bcmDropBrain(&b);
    return rc;
}

static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* Index of a mode/level key pair in the fixture brain's own manifest, so
 * the expectations are read the same way the code under test reads them.
 * False when the manifest cannot be read, which the callers report as a
 * skip. */
static bool expected_indices(const char *brain, const char *modeKey,
                             const char *lvlKey, int *outMode, int *outLevel) {
    BrainModes modes;
    int m;
    if (!brainListLoadModesForPath(brain, &modes)) return false;
    m = brainModesFindMode(&modes, modeKey);
    if (m < 0) return false;
    *outMode  = m;
    *outLevel = brainModeFindLevel(&modes.modes[m], lvlKey);
    return (*outLevel >= 0);
}

#define SKIP_NO_MANIFEST()                                                  \
    do {                                                                    \
        fprintf(stderr, "SKIP: the fixture brain's modes.txt could not be " \
                        "read — nothing to resolve keys against\n");         \
        return 0;                                                           \
    } while (0)

/* The host picked turtle/easy by hand: the next bot added starts there. */
static int bcm_applies(const char *brain) {
    ServerSim *sim;
    int wantMode = 0, wantLevel = 0;
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;

    if (!expected_indices(brain, "turtle", "easy", &wantMode, &wantLevel)) {
        SKIP_NO_MANIFEST();
    }
    sim = make_lobby_sim();
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    SDL_strlcpy(sim->lastBotModeKey, "turtle", sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "easy", sizeof(sim->lastBotLevelKey));
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, true,
                                           &mode, &level));
    UT_ASSERT_MSG((int)mode == wantMode && (int)level == wantLevel,
                  "the manual pick must reach the new bot (got %d/%d, want "
                  "%d/%d)", (int)mode, (int)level, wantMode, wantLevel);
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_applies(void) {
    return bcmRun("applies", bcm_applies);
}

/* No manual pick yet this lobby: the base comes back unchanged. */
static int bcm_empty_is_noop(const char *brain) {
    ServerSim *sim = make_lobby_sim();
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    sim->lastBotModeKey[0]  = '\0';
    sim->lastBotLevelKey[0] = '\0';
    if (!serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, true,
                                      &mode, &level)) {
        serverSimDestroy(sim);
        SKIP_NO_MANIFEST();
    }
    UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                  "no manual pick: the base must stand (got %d/%d)",
                  (int)mode, (int)level);
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_empty_is_noop(void) {
    return bcmRun("empty_is_noop", bcm_empty_is_noop);
}

/* A mode this bot's brain does not declare is dropped, not forced; and a
 * brain with no manifest resolves nothing at all. */
static int bcm_unknown_key_ignored(const char *brain) {
    ServerSim *sim = make_lobby_sim();
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    SDL_strlcpy(sim->lastBotModeKey, "no_such_mode",
                sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "hard", sizeof(sim->lastBotLevelKey));
    if (serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, true,
                                     &mode, &level)) {
        UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                      "an unknown mode key must leave the base alone (got "
                      "%d/%d)", (int)mode, (int)level);
    }

    mode = 0;
    level = BOT_DIFFICULTY_HARD;
    SDL_strlcpy(sim->lastBotModeKey, "turtle", sizeof(sim->lastBotModeKey));
    UT_ASSERT_MSG(!serverSimResolveNewBotConfig(sim, TEST_TEAM,
                                                "brains/no_such_brain.lua",
                                                true, &mode, &level),
                  "a brain with no manifest must resolve nothing");
    UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                  "...and must leave both values untouched");
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_unknown_key_ignored(void) {
    return bcmRun("unknown_key", bcm_unknown_key_ignored);
}

/* A bot that does not honour the manual pick — one first appearing on the
 * map — keeps the base whatever the host picked earlier. */
static int bcm_not_honoured(const char *brain) {
    ServerSim *sim = make_lobby_sim();
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    SDL_strlcpy(sim->lastBotModeKey, "turtle", sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "easy", sizeof(sim->lastBotLevelKey));
    if (!serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, false,
                                      &mode, &level)) {
        serverSimDestroy(sim);
        SKIP_NO_MANIFEST();
    }
    UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                  "a first-appearing bot must ignore the manual pick (got "
                  "%d/%d)", (int)mode, (int)level);
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_not_honoured(void) {
    return bcmRun("not_honoured", bcm_not_honoured);
}

/* Only a manual pick is remembered. serverSimSetBotConfig is also the write
 * that automatic paths use (single player's add, the CLI), so it must not
 * record anything; serverSimRememberManualBotPick — called from the gear
 * popup's command handler — does. */
static int bcm_manual_only(const char *brain) {
    ServerSim *sim;
    int wantMode = 0, wantLevel = 0;

    if (!expected_indices(brain, "turtle", "medium", &wantMode, &wantLevel)) {
        SKIP_NO_MANIFEST();
    }
    sim = make_lobby_sim();
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    SDL_strlcpy(sim->botMgr.bots[TEST_SLOT].brainPath, brain,
                sizeof(sim->botMgr.bots[TEST_SLOT].brainPath));

    serverSimSetBotConfig(sim, TEST_SLOT, (uint8_t)wantMode,
                          (uint8_t)wantLevel, 0, NULL);
    UT_ASSERT_MSG(sim->lastBotModeKey[0] == '\0' &&
                  sim->lastBotLevelKey[0] == '\0',
                  "an automatic config write must not be remembered as the "
                  "host's pick (got '%s'/'%s')",
                  sim->lastBotModeKey, sim->lastBotLevelKey);

    serverSimRememberManualBotPick(sim, TEST_SLOT);
    UT_ASSERT_MSG(strcmp(sim->lastBotModeKey, "turtle") == 0 &&
                  strcmp(sim->lastBotLevelKey, "medium") == 0,
                  "the manual pick must be remembered as the brain's keys "
                  "(got '%s'/'%s')", sim->lastBotModeKey, sim->lastBotLevelKey);

    /* No bot was really created in this slot; do not leave the manager
     * thinking one has a brain. */
    sim->botMgr.bots[TEST_SLOT].brainPath[0] = '\0';
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_manual_only(void) {
    return bcmRun("manual_only", bcm_manual_only);
}

/* The pick lives for ONE lobby session. A round ending starts a new one, so
 * returning to the lobby forgets it even when the host never left — the bug
 * was that a host who stayed through a game found the next lobby's Add Bot
 * still in the mode they picked last game. A map change within the same
 * lobby session is not a new lobby and must NOT forget it. */
static int bcm_cleared_on_return_to_lobby(const char *brain) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim;
    int wantMode = 0, wantLevel = 0;
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;
    bool mapChanged;

    if (!expected_indices(brain, "turtle", "easy", &wantMode, &wantLevel)) {
        SKIP_NO_MANIFEST();
    }
    sim = make_lobby_sim();
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    /* A human host, so the return below takes the "someone stayed" branch —
     * the one that used to keep the pick. serverSimResetLobbyToDefaults (the
     * empty-lobby branch) already cleared it and is not what we are testing. */
    serverSimAddPlayer(sim, 0, "Host", false);

    /* The host picks turtle/easy by hand on one bot, through the same path
     * the gear popup's command handler uses. */
    SDL_strlcpy(sim->botMgr.bots[TEST_SLOT].brainPath, brain,
                sizeof(sim->botMgr.bots[TEST_SLOT].brainPath));
    serverSimSetBotConfig(sim, TEST_SLOT, (uint8_t)wantMode,
                          (uint8_t)wantLevel, 0, NULL);
    serverSimRememberManualBotPick(sim, TEST_SLOT);
    UT_ASSERT_MSG(strcmp(sim->lastBotModeKey, "turtle") == 0 &&
                  strcmp(sim->lastBotLevelKey, "easy") == 0,
                  "setup: the manual pick must be remembered first (got "
                  "'%s'/'%s')", sim->lastBotModeKey, sim->lastBotLevelKey);
    /* No bot really runs in this slot; don't let the manager start one for
     * the round below. */
    sim->botMgr.bots[TEST_SLOT].brainPath[0] = '\0';

    /* A map change inside the lobby session keeps the pick. The reload wants
     * a writable data/maps beside the test binary; when there isn't one it
     * returns false and this assertion is simply not made. */
    mapChanged = serverSimReloadCompressedInMemory(sim, emap, E_MAP_LEN,
                                                   "Everard Island B");
    if (mapChanged) {
        UT_ASSERT_MSG(strcmp(sim->lastBotModeKey, "turtle") == 0 &&
                      strcmp(sim->lastBotLevelKey, "easy") == 0,
                      "a map change is not a new lobby: the pick must survive "
                      "it (got '%s'/'%s')",
                      sim->lastBotModeKey, sim->lastBotLevelKey);
    } else {
        fprintf(stderr, "NOTE: no writable map dir beside the test binary — "
                        "the map-change half of this case was not exercised\n");
    }

    /* Play a round and come back with the host still here. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "setup: the game must actually start");
    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "setup: the return must actually land in the lobby");
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) > 0,
                  "setup: the host must still be here, or this is the "
                  "empty-lobby path instead");

    UT_ASSERT_MSG(sim->lastBotModeKey[0] == '\0' &&
                  sim->lastBotLevelKey[0] == '\0',
                  "returning to the lobby must forget the pick (got "
                  "'%s'/'%s')", sim->lastBotModeKey, sim->lastBotLevelKey);

    /* ...and the next Add Bot therefore starts at the brain's defaults. */
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, true,
                                           &mode, &level));
    UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                  "the first bot of the new lobby must start at the defaults "
                  "(got %d/%d)", (int)mode, (int)level);

    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_cleared_on_return_to_lobby(void) {
    return bcmRun("new_lobby", bcm_cleared_on_return_to_lobby);
}
