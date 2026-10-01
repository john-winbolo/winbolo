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
 * easy/medium/hard defaulting to hard. The shipped GoalHunter manifest is
 * not read, so its open_default cannot move these cases' base. The open-game
 * cases at the end of the file write a manifest of their own, with
 * `open_default = turtle` above the first section and a third mode, "other",
 * that is neither type's starting mode.
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
#include "wire_limits.h"            /* LST_GAME_TYPE */
#include "brain_list.h"            /* BrainModes — to name the expected indices */
#include "client_command.h"        /* CMD_LOBBY_BOT_CONFIG */
#include "server_sim_scenario.h"   /* serverSimSetScenarioIdentity */
#include "threads.h"               /* the dispatcher asserts the mutex */
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

/* The two-mode manifest every case starts from. */
#define BCM_MANIFEST_BASE                                       \
    "[default]\n"                                               \
    "label = Default\n"                                         \
    "levels = easy:Easy:1, medium:Medium:2, hard:Hard:3\n"      \
    "default = hard\n"                                          \
    "\n"                                                        \
    "[turtle]\n"                                                \
    "label = Turtle\n"                                          \
    "levels = easy:Easy:1, medium:Medium:2, hard:Hard:3\n"      \
    "default = hard\n"

/* The same brain for the open-game cases: an Open game starts in turtle,
 * and a third mode stands in for one a bot was put in some other way. */
#define BCM_MANIFEST_OPEN                                       \
    "open_default = turtle\n"                                   \
    "\n"                                                        \
    BCM_MANIFEST_BASE                                           \
    "\n"                                                        \
    "[other]\n"                                                 \
    "label = Other\n"                                           \
    "levels = easy:Easy:1, medium:Medium:2, hard:Hard:3\n"      \
    "default = hard\n"

/* Write the fixture brain: an init.lua and the given modes.txt. False when
 * the working directory is not writable, which bcmRun reports as a skip.
 * brains/ is created when missing and never removed, so a case cannot pull
 * it out from under another. */
static bool bcmMakeBrain(const BcmBrain *b, const char *manifest) {
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
    fputs(manifest, f);
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

/* Run one case against its own fixture brain, written from `manifest`. The
 * fixture is dropped however the body returns, so a failed assertion leaves
 * nothing behind in the build directory's brains/. */
static int bcmRunWith(const char *name, const char *manifest,
                      int (*body)(const char *brain)) {
    BcmBrain b;
    int rc;

    SDL_snprintf(b.dir, sizeof(b.dir), "brains/ut_bcm_%s", name);
    SDL_snprintf(b.brain, sizeof(b.brain), "%s/init.lua", b.dir);
    if (!bcmMakeBrain(&b, manifest)) {
        fprintf(stderr, "SKIP: the fixture brain could not be written — "
                        "nothing to resolve keys against\n");
        bcmDropBrain(&b);
        return 0;
    }
    rc = body(b.brain);
    bcmDropBrain(&b);
    return rc;
}

static int bcmRun(const char *name, int (*body)(const char *brain)) {
    return bcmRunWith(name, BCM_MANIFEST_BASE, body);
}

static ServerSim *make_lobby_sim_type(gameType game) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               game, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* An Open lobby. The base manifest names no open_default, so an Open game
 * starts in mode 0 there and the cases about the base and the manual pick
 * see mode 0 as the base. */
static ServerSim *make_lobby_sim(void) {
    return make_lobby_sim_type(gameOpen);
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

/* ── The Open game's starting mode ─────────────────────────────────────
 *
 * The cases below run on BCM_MANIFEST_OPEN: `open_default = turtle`, and a
 * third mode "other". They read the open_default mode back from the
 * manifest rather than naming it, so the expectations are the code's own. */

/* The fixture's open_default mode, or -1 when the manifest is missing or
 * names none (then the open-game cases have nothing to test). */
static int open_default_mode(const char *brain) {
    BrainModes modes;
    if (!brainListLoadModesForPath(brain, &modes)) return -1;
    return (modes.openDefaultMode > 0) ? modes.openDefaultMode : -1;
}

/* Index of level `lvlKey` inside the open_default mode. -1 when it lists no
 * such level. */
static int open_default_level(const char *brain, const char *lvlKey) {
    BrainModes modes;
    int m;
    if (!brainListLoadModesForPath(brain, &modes)) return -1;
    m = modes.openDefaultMode;
    if (m <= 0 || m >= modes.modeCount) return -1;
    return brainModeFindLevel(&modes.modes[m], lvlKey);
}

/* An Open game starts a new bot in the brain's open_default mode at the
 * level it would have had in mode 0; Tournament and Strict start in mode 0;
 * a mode the host picked by hand still wins on Open. */
static int bcm_open_game_start_mode(const char *brain) {
    static const gameType kClosed[2] = { gameTournament, gameStrictTournament };
    ServerSim *sim;
    int openMode = open_default_mode(brain);
    int wantMode = 0, wantLevel = 0;
    uint8_t mode, level;
    int i;

    if (openMode < 0) SKIP_NO_MANIFEST();

    /* Open, no pick: open_default at Hard. */
    sim = make_lobby_sim_type(gameOpen);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    mode = 0; level = BOT_DIFFICULTY_HARD;
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, true,
                                           &mode, &level));
    wantLevel = open_default_level(brain, "hard");
    UT_ASSERT_MSG(wantLevel >= 0, "open_default mode %d lists no hard level",
                  openMode);
    UT_ASSERT_MSG((int)mode == openMode && (int)level == wantLevel,
                  "an Open game must start in open_default at Hard (got "
                  "%d/%d, want %d/%d)", (int)mode, (int)level, openMode,
                  wantLevel);

    /* The level moves by key: Medium in mode 0 is Medium in the new mode. */
    mode = 0; level = BOT_DIFFICULTY_MEDIUM;
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, false,
                                           &mode, &level));
    wantLevel = open_default_level(brain, "medium");
    UT_ASSERT_MSG(wantLevel >= 0, "open_default mode %d lists no medium level",
                  openMode);
    UT_ASSERT_MSG((int)mode == openMode && (int)level == wantLevel,
                  "Medium must stay Medium (got %d/%d)", (int)mode,
                  (int)level);

    /* A remembered mode key this brain does not list is dropped, and the
       new bot falls to the Open start mode, not to mode 0. */
    SDL_strlcpy(sim->lastBotModeKey, "no_such_mode",
                sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "hard", sizeof(sim->lastBotLevelKey));
    mode = 0; level = BOT_DIFFICULTY_HARD;
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, true,
                                           &mode, &level));
    UT_ASSERT_MSG((int)mode == openMode &&
                  (int)level == open_default_level(brain, "hard"),
                  "an unknown remembered mode on Open must fall to "
                  "open_default at Hard (got %d/%d)", (int)mode, (int)level);
    sim->lastBotModeKey[0]  = '\0';
    sim->lastBotLevelKey[0] = '\0';

    /* A caller's base in some other mode gives only its level: the bot
       starts in open_default at that level. */
    UT_ASSERT(expected_indices(brain, "other", "easy", &wantMode, &wantLevel));
    mode = (uint8_t)wantMode; level = (uint8_t)wantLevel;
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, false,
                                           &mode, &level));
    UT_ASSERT_MSG((int)mode == openMode &&
                  (int)level == open_default_level(brain, "easy"),
                  "an other/easy base on Open must start in open_default "
                  "at Easy (got %d/%d)", (int)mode, (int)level);

    /* The host picked Default by hand: the next Add Bot is Default. */
    SDL_strlcpy(sim->lastBotModeKey, "default", sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "easy", sizeof(sim->lastBotLevelKey));
    mode = 0; level = BOT_DIFFICULTY_HARD;
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain, true,
                                           &mode, &level));
    UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_EASY,
                  "a manual Default pick must win on Open (got %d/%d)",
                  (int)mode, (int)level);
    serverSimDestroy(sim);

    /* Tournament and Strict: mode 0 at Hard. */
    for (i = 0; i < 2; i++) {
        sim = make_lobby_sim_type(kClosed[i]);
        UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
        mode = 0; level = BOT_DIFFICULTY_HARD;
        UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain,
                                               true, &mode, &level));
        UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                      "game type %d must start in mode 0 (got %d/%d)",
                      (int)kClosed[i], (int)mode, (int)level);
        serverSimDestroy(sim);
    }
    return 0;
}

int run_lobby_bot_config_memory_open_game_start_mode(void) {
    return bcmRunWith("open_start", BCM_MANIFEST_OPEN,
                      bcm_open_game_start_mode);
}

/* Andrew: "turtle bots should only go in open OR if the last bot you added
 * IN THAT LOBBY was a turtle bot". Single player's base is the saved
 * "Chosen Mode" pref, which outlives the lobby it was picked in, so a Turtle
 * base must not start a Tournament bot in Turtle. The saved level is kept.
 * A Turtle pick made by hand in THIS lobby still reaches the next Add Bot,
 * and a new lobby has forgotten it. */
static int bcm_saved_mode_not_carried(const char *brain) {
    static const gameType kClosed[2] = { gameTournament, gameStrictTournament };
    ServerSim *sim;
    int turtleMode = 0, turtleMedium = 0, turtleEasy = 0;
    uint8_t mode, level;
    int i;

    if (!expected_indices(brain, "turtle", "medium", &turtleMode,
                          &turtleMedium) ||
        !expected_indices(brain, "turtle", "easy", &turtleMode,
                          &turtleEasy)) {
        SKIP_NO_MANIFEST();
    }

    for (i = 0; i < 2; i++) {
        sim = make_lobby_sim_type(kClosed[i]);
        UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

        /* A saved Turtle/Medium base, no pick in this lobby: Default at
           Medium, for the setup bots and for Add Bot alike. */
        mode = (uint8_t)turtleMode; level = (uint8_t)turtleMedium;
        UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain,
                                               false, &mode, &level));
        UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_MEDIUM,
                      "game type %d: a saved Turtle base must start a setup "
                      "bot in Default at Medium (got %d/%d)", (int)kClosed[i],
                      (int)mode, (int)level);
        mode = (uint8_t)turtleMode; level = (uint8_t)turtleMedium;
        UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain,
                                               true, &mode, &level));
        UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_MEDIUM,
                      "game type %d: a saved Turtle base must start an Add "
                      "Bot in Default at Medium (got %d/%d)", (int)kClosed[i],
                      (int)mode, (int)level);

        /* The host set a bot to Turtle/Easy by hand in this lobby: the next
           Add Bot is Turtle/Easy. A setup bot does not read the pick. */
        SDL_strlcpy(sim->lastBotModeKey, "turtle",
                    sizeof(sim->lastBotModeKey));
        SDL_strlcpy(sim->lastBotLevelKey, "easy",
                    sizeof(sim->lastBotLevelKey));
        mode = 0; level = BOT_DIFFICULTY_HARD;
        UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain,
                                               true, &mode, &level));
        UT_ASSERT_MSG((int)mode == turtleMode && (int)level == turtleEasy,
                      "game type %d: a Turtle pick in this lobby must reach "
                      "the next Add Bot (got %d/%d)", (int)kClosed[i],
                      (int)mode, (int)level);
        mode = 0; level = BOT_DIFFICULTY_HARD;
        UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain,
                                               false, &mode, &level));
        UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                      "game type %d: a setup bot must not read the lobby's "
                      "pick (got %d/%d)", (int)kClosed[i], (int)mode,
                      (int)level);

        /* A new game session is a new server sim, and it has no pick: Add
           Bot is back on Default. (The return to the lobby after a round
           clears it too: lobby_bot_config_memory_new_lobby.) */
        serverSimDestroy(sim);
        sim = make_lobby_sim_type(kClosed[i]);
        UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
        mode = (uint8_t)turtleMode; level = (uint8_t)turtleMedium;
        UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain,
                                               true, &mode, &level));
        UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_MEDIUM,
                      "game type %d: a new lobby must not remember the "
                      "Turtle pick (got %d/%d)", (int)kClosed[i], (int)mode,
                      (int)level);
        serverSimDestroy(sim);
    }

    /* Open: the same saved Turtle/Medium base is Turtle at Medium. */
    sim = make_lobby_sim_type(gameOpen);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    if (open_default_mode(brain) == turtleMode) {
        mode = (uint8_t)turtleMode; level = (uint8_t)turtleMedium;
        UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, brain,
                                               true, &mode, &level));
        UT_ASSERT_MSG((int)mode == turtleMode && (int)level == turtleMedium,
                      "an Open game must start the bot in Turtle at Medium "
                      "(got %d/%d)", (int)mode, (int)level);
    }
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_saved_mode_not_carried(void) {
    return bcmRunWith("saved_mode", BCM_MANIFEST_OPEN,
                      bcm_saved_mode_not_carried);
}

/* Seat a pretend bot in `slot` on the fixture brain: enough for
 * serverSimIsBot and for the brain path the follow reads. No brain is
 * built. */
static void pretend_bot(ServerSim *sim, BYTE slot, const char *brain) {
    sim->playerConnected[slot]    = true;
    sim->lobbyPlayers[slot].isBot = true;
    SDL_strlcpy(sim->botMgr.bots[slot].brainPath, brain,
                sizeof(sim->botMgr.bots[slot].brainPath));
}

static void unpretend_bot(ServerSim *sim, BYTE slot) {
    sim->playerConnected[slot]    = false;
    sim->lobbyPlayers[slot].isBot = false;
    sim->botMgr.bots[slot].brainPath[0] = '\0';
}

/* The host changes the game type: bots still on the old type's starting
 * mode follow the new one, with their level carried by key; a bot whose mode
 * a person set by hand keeps it; a bot on some other mode keeps it. */
static int bcm_game_type_change(const char *brain) {
    const BYTE follow = 2, byHand = 3, other = 4;
    ServerSim *sim;
    int openMode = open_default_mode(brain);
    int otherMode = 0, otherLevel = 0, medOpenLevel;
    uint8_t v;

    if (openMode < 0) SKIP_NO_MANIFEST();
    UT_ASSERT(expected_indices(brain, "other", "easy", &otherMode,
                               &otherLevel));
    medOpenLevel = open_default_level(brain, "medium");
    UT_ASSERT_MSG(medOpenLevel >= 0, "open_default lists no medium level");

    sim = make_lobby_sim_type(gameOpen);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    pretend_bot(sim, follow, brain);
    pretend_bot(sim, byHand, brain);
    pretend_bot(sim, other, brain);
    /* All three came in as new bots on Open. */
    serverSimSetBotConfig(sim, follow, (uint8_t)openMode,
                          (uint8_t)medOpenLevel, 0, NULL);
    serverSimSetBotConfig(sim, byHand, (uint8_t)openMode,
                          BOT_DIFFICULTY_HARD, 0, NULL);
    serverSimSetBotConfig(sim, other, (uint8_t)otherMode,
                          (uint8_t)otherLevel, 0, NULL);
    /* A person then set byHand's mode in the gear popup (away and back). */
    serverSimMarkBotModeSetByHand(sim, byHand);

    v = (uint8_t)gameTournament;
    sim->botConfigPublishPending = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_GAME_TYPE, &v, 1));
    UT_ASSERT_MSG(sim->botConfigs[follow].mode == 0 &&
                  sim->botConfigs[follow].difficulty == BOT_DIFFICULTY_MEDIUM,
                  "a bot on the Open start mode must follow to Tournament's "
                  "mode 0 at Medium (got %u/%u)",
                  (unsigned)sim->botConfigs[follow].mode,
                  (unsigned)sim->botConfigs[follow].difficulty);
    UT_ASSERT_MSG((sim->botConfigPublishPending & (1u << follow)) != 0,
                  "the moved seat queued no bot-config event");
    UT_ASSERT_MSG((int)sim->botConfigs[byHand].mode == openMode,
                  "a mode set by hand must stick (got %u)",
                  (unsigned)sim->botConfigs[byHand].mode);
    UT_ASSERT_MSG((int)sim->botConfigs[other].mode == otherMode,
                  "a bot on another mode must keep it (got %u)",
                  (unsigned)sim->botConfigs[other].mode);

    /* Tournament to Strict: both start in mode 0, so nothing moves. */
    v = (uint8_t)gameStrictTournament;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_GAME_TYPE, &v, 1));
    UT_ASSERT(sim->botConfigs[follow].mode == 0);

    /* And back to Open: the follower returns to open_default. */
    v = (uint8_t)gameOpen;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_GAME_TYPE, &v, 1));
    UT_ASSERT_MSG((int)sim->botConfigs[follow].mode == openMode &&
                  (int)sim->botConfigs[follow].difficulty == medOpenLevel,
                  "back on Open the follower must be in open_default at "
                  "Medium (got %u/%u)",
                  (unsigned)sim->botConfigs[follow].mode,
                  (unsigned)sim->botConfigs[follow].difficulty);
    UT_ASSERT((int)sim->botConfigs[byHand].mode == openMode);
    UT_ASSERT((int)sim->botConfigs[other].mode == otherMode);

    unpretend_bot(sim, follow);
    unpretend_bot(sim, other);
    /* The seat's player leaving takes the by-hand mark with it. */
    serverSimRemovePlayer(sim, byHand);
    UT_ASSERT_MSG((sim->botModeSetByHand & (1u << byHand)) == 0,
                  "the by-hand mark outlived the seat");
    unpretend_bot(sim, byHand);
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_game_type_change(void) {
    return bcmRunWith("type_change", BCM_MANIFEST_OPEN, bcm_game_type_change);
}

/* A map commit that attaches or drops a scenario can change the game type
 * too (serverSimScenarioApplyLobbyRules). The bots follow it there as they
 * do when the host changes the type by hand. */
static int bcm_map_commit_type(const char *brain) {
    const BYTE bot = 2, byHand = 3;
    ServerSim *sim;
    int openMode = open_default_mode(brain);
    int medOpenLevel;

    if (openMode < 0) SKIP_NO_MANIFEST();
    medOpenLevel = open_default_level(brain, "medium");
    UT_ASSERT_MSG(medOpenLevel >= 0, "open_default lists no medium level");

    /* 1. An Open lobby picks a scripted map with no lobby template. The
          host's bots were added on Open, so they are on the Open start
          mode. The map puts the lobby on Scripted: the bots move to mode 0
          at the same level, and a mode set by hand stays. */
    sim = make_lobby_sim_type(gameOpen);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    pretend_bot(sim, bot, brain);
    pretend_bot(sim, byHand, brain);
    serverSimSetBotConfig(sim, bot, (uint8_t)openMode, (uint8_t)medOpenLevel,
                          0, NULL);
    serverSimSetBotConfig(sim, byHand, (uint8_t)openMode,
                          BOT_DIFFICULTY_HARD, 0, NULL);
    serverSimMarkBotModeSetByHand(sim, byHand);
    serverSimSetScenarioIdentity(sim, lobbyScenarioMap, "Test", "test.lua",
                                 "", false, false, false, false, false);
    UT_ASSERT(!serverSimScenarioHasLobbyTemplate(sim));
    sim->botConfigPublishPending = 0;
    serverSimScenarioApplyLobbyRules(sim);
    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameScripted,
                  "setup: the scripted map must put the lobby on Scripted "
                  "(got %d)", (int)serverSimGetGameType(sim));
    UT_ASSERT_MSG(sim->botConfigs[bot].mode == 0 &&
                  sim->botConfigs[bot].difficulty == BOT_DIFFICULTY_MEDIUM,
                  "a bot on the Open start mode must follow the scripted "
                  "map to mode 0 at Medium (got %u/%u)",
                  (unsigned)sim->botConfigs[bot].mode,
                  (unsigned)sim->botConfigs[bot].difficulty);
    UT_ASSERT_MSG((sim->botConfigPublishPending & (1u << bot)) != 0,
                  "the moved seat queued no bot-config event");
    UT_ASSERT_MSG((int)sim->botConfigs[byHand].mode == openMode,
                  "a mode set by hand must stay (got %u)",
                  (unsigned)sim->botConfigs[byHand].mode);

    /* 2. The host then adds a bot on the scripted map (mode 0) and picks a
          plain map. The lobby goes back to Open, and every bot on mode 0
          that nobody set by hand moves to the Open start mode. */
    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false, false, false, false, false);
    serverSimScenarioApplyLobbyRules(sim);
    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameOpen,
                  "setup: the plain map must give Open back (got %d)",
                  (int)serverSimGetGameType(sim));
    UT_ASSERT_MSG((int)sim->botConfigs[bot].mode == openMode &&
                  (int)sim->botConfigs[bot].difficulty == medOpenLevel,
                  "back on Open the bot must be in open_default at Medium "
                  "(got %u/%u)", (unsigned)sim->botConfigs[bot].mode,
                  (unsigned)sim->botConfigs[bot].difficulty);
    UT_ASSERT((int)sim->botConfigs[byHand].mode == openMode);

    unpretend_bot(sim, bot);
    unpretend_bot(sim, byHand);
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_map_commit_type(void) {
    return bcmRunWith("map_commit_type", BCM_MANIFEST_OPEN,
                      bcm_map_commit_type);
}

/* The CMD_LOBBY_BOT_CONFIG arm marks a seat's mode as set by hand only when
 * the command changed the MODE. A difficulty change, a rename or a
 * personality edit carries the unchanged mode and must not mark it. */
static CmdResult apply_bot_config(ServerSim *sim, BYTE slot, uint8_t mode,
                                  uint8_t level) {
    ClientCommand cmd;
    CmdResult     r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_BOT_CONFIG;
    cmd.cmdSeq = 1;
    cmd.u.lobbyBotConfig.slot       = slot;
    cmd.u.lobbyBotConfig.mode       = mode;
    cmd.u.lobbyBotConfig.difficulty = level;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    return r;
}

static int bcm_dispatch_hand_mark(const char *brain) {
    const BYTE bot = 2;
    ServerSim *sim;
    int openMode = open_default_mode(brain);

    if (openMode < 0) SKIP_NO_MANIFEST();
    sim = make_lobby_sim_type(gameOpen);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    /* The host in slot 0, so the sender may edit the lobby. */
    serverSimAddPlayer(sim, 0, "Host", false);
    pretend_bot(sim, bot, brain);
    serverSimSetBotConfig(sim, bot, (uint8_t)openMode, BOT_DIFFICULTY_HARD,
                          0, NULL);
    sim->botModeSetByHand = 0;

    /* Same mode, new level: no mark. */
    UT_ASSERT(apply_bot_config(sim, bot, (uint8_t)openMode,
                               BOT_DIFFICULTY_EASY) == CMD_OK);
    UT_ASSERT_MSG(sim->botConfigs[bot].difficulty == BOT_DIFFICULTY_EASY,
                  "setup: the level change did not land");
    UT_ASSERT_MSG((sim->botModeSetByHand & (1u << bot)) == 0,
                  "a level-only change marked the mode as set by hand");

    /* Same pair again (a rename sends this): no mark. */
    UT_ASSERT(apply_bot_config(sim, bot, (uint8_t)openMode,
                               BOT_DIFFICULTY_EASY) == CMD_OK);
    UT_ASSERT((sim->botModeSetByHand & (1u << bot)) == 0);

    /* A new mode: marked. */
    UT_ASSERT(apply_bot_config(sim, bot, 0, BOT_DIFFICULTY_EASY) == CMD_OK);
    UT_ASSERT_MSG(sim->botConfigs[bot].mode == 0,
                  "setup: the mode change did not land");
    UT_ASSERT_MSG((sim->botModeSetByHand & (1u << bot)) != 0,
                  "a mode change did not mark the mode as set by hand");

    unpretend_bot(sim, bot);
    serverSimDestroy(sim);
    return 0;
}

int run_lobby_bot_config_memory_dispatch_hand_mark(void) {
    return bcmRunWith("dispatch_mark", BCM_MANIFEST_OPEN,
                      bcm_dispatch_hand_mark);
}
