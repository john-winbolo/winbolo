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
 * The manifest used is the shipped GoalHunter one (Brains/GoalHunter_1.7/
 * modes.txt, copied next to the test binary by the build): modes "default",
 * "survival" and "turtle", each with levels easy/medium/hard defaulting to
 * hard, and open_default = turtle.
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

#define TEST_BRAIN "Brains/GoalHunter_1.7/init.lua"
#define TEST_SLOT  3
#define TEST_TEAM  1

static ServerSim *make_lobby_sim_type(gameType game) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               game, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* A Tournament lobby: the shipped manifest starts it in mode 0, so the cases
 * below that are about the base and the manual pick see mode 0 as the base.
 * An Open lobby starts in the manifest's open_default instead, which the
 * open-game cases at the end of the file cover. */
static ServerSim *make_lobby_sim(void) {
    return make_lobby_sim_type(gameTournament);
}

/* Index of a mode/level key pair in the test brain's own manifest, so the
 * expectations survive someone reordering modes.txt. False when the manifest
 * is missing — a test binary run from outside the build tree has no Brains/
 * beside it — which the callers report as a skip. */
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

#define SKIP_NO_MANIFEST()                                                  \
    do {                                                                    \
        fprintf(stderr, "SKIP: no GoalHunter modes.txt beside the test "    \
                        "binary — nothing to resolve keys against\n");      \
        return 0;                                                           \
    } while (0)

/* The host picked survival/easy by hand: the next bot added starts there. */
int run_lobby_bot_config_memory_applies(void) {
    ServerSim *sim;
    int wantMode = 0, wantLevel = 0;
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;

    if (!expected_indices("survival", "easy", &wantMode, &wantLevel)) {
        SKIP_NO_MANIFEST();
    }
    sim = make_lobby_sim();
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    SDL_strlcpy(sim->lastBotModeKey, "survival", sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "easy", sizeof(sim->lastBotLevelKey));
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, true,
                                           &mode, &level));
    UT_ASSERT_MSG((int)mode == wantMode && (int)level == wantLevel,
                  "the manual pick must reach the new bot (got %d/%d, want "
                  "%d/%d)", (int)mode, (int)level, wantMode, wantLevel);
    serverSimDestroy(sim);
    return 0;
}

/* No manual pick yet this lobby: the base comes back unchanged. */
int run_lobby_bot_config_memory_empty_is_noop(void) {
    ServerSim *sim = make_lobby_sim();
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    sim->lastBotModeKey[0]  = '\0';
    sim->lastBotLevelKey[0] = '\0';
    if (!serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, true,
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

/* A mode this bot's brain does not declare is dropped, not forced; and a
 * brain with no manifest resolves nothing at all. */
int run_lobby_bot_config_memory_unknown_key_ignored(void) {
    ServerSim *sim = make_lobby_sim();
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    SDL_strlcpy(sim->lastBotModeKey, "no_such_mode",
                sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "hard", sizeof(sim->lastBotLevelKey));
    if (serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, true,
                                     &mode, &level)) {
        UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                      "an unknown mode key must leave the base alone (got "
                      "%d/%d)", (int)mode, (int)level);
    }

    mode = 0;
    level = BOT_DIFFICULTY_HARD;
    SDL_strlcpy(sim->lastBotModeKey, "survival", sizeof(sim->lastBotModeKey));
    UT_ASSERT_MSG(!serverSimResolveNewBotConfig(sim, TEST_TEAM,
                                                "brains/no_such_brain.lua",
                                                true, &mode, &level),
                  "a brain with no manifest must resolve nothing");
    UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                  "...and must leave both values untouched");
    serverSimDestroy(sim);
    return 0;
}

/* A bot that does not honour the manual pick — one first appearing on the
 * map — keeps the base whatever the host picked earlier. */
int run_lobby_bot_config_memory_not_honoured(void) {
    ServerSim *sim = make_lobby_sim();
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    SDL_strlcpy(sim->lastBotModeKey, "survival", sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "easy", sizeof(sim->lastBotLevelKey));
    if (!serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, false,
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

/* Only a manual pick is remembered. serverSimSetBotConfig is also the write
 * that automatic paths use (single player's add, the CLI), so it must not
 * record anything; serverSimRememberManualBotPick — called from the gear
 * popup's command handler — does. */
int run_lobby_bot_config_memory_manual_only(void) {
    ServerSim *sim;
    int wantMode = 0, wantLevel = 0;

    if (!expected_indices("survival", "medium", &wantMode, &wantLevel)) {
        SKIP_NO_MANIFEST();
    }
    sim = make_lobby_sim();
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    SDL_strlcpy(sim->botMgr.bots[TEST_SLOT].brainPath, TEST_BRAIN,
                sizeof(sim->botMgr.bots[TEST_SLOT].brainPath));

    serverSimSetBotConfig(sim, TEST_SLOT, (uint8_t)wantMode,
                          (uint8_t)wantLevel, 0, NULL);
    UT_ASSERT_MSG(sim->lastBotModeKey[0] == '\0' &&
                  sim->lastBotLevelKey[0] == '\0',
                  "an automatic config write must not be remembered as the "
                  "host's pick (got '%s'/'%s')",
                  sim->lastBotModeKey, sim->lastBotLevelKey);

    serverSimRememberManualBotPick(sim, TEST_SLOT);
    UT_ASSERT_MSG(strcmp(sim->lastBotModeKey, "survival") == 0 &&
                  strcmp(sim->lastBotLevelKey, "medium") == 0,
                  "the manual pick must be remembered as the brain's keys "
                  "(got '%s'/'%s')", sim->lastBotModeKey, sim->lastBotLevelKey);

    /* No bot was really created in this slot; do not leave the manager
     * thinking one has a brain. */
    sim->botMgr.bots[TEST_SLOT].brainPath[0] = '\0';
    serverSimDestroy(sim);
    return 0;
}

/* The pick lives for ONE lobby session. A round ending starts a new one, so
 * returning to the lobby forgets it even when the host never left — the bug
 * was that a host who stayed through a game found the next lobby's Add Bot
 * still in the mode they picked last game. A map change within the same
 * lobby session is not a new lobby and must NOT forget it. */
int run_lobby_bot_config_memory_cleared_on_return_to_lobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim;
    int wantMode = 0, wantLevel = 0;
    uint8_t mode = 0, level = BOT_DIFFICULTY_HARD;
    bool mapChanged;

    if (!expected_indices("survival", "easy", &wantMode, &wantLevel)) {
        SKIP_NO_MANIFEST();
    }
    sim = make_lobby_sim();
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");

    /* A human host, so the return below takes the "someone stayed" branch —
     * the one that used to keep the pick. serverSimResetLobbyToDefaults (the
     * empty-lobby branch) already cleared it and is not what we are testing. */
    serverSimAddPlayer(sim, 0, "Host", false);

    /* The host picks survival/easy by hand on one bot, through the same path
     * the gear popup's command handler uses. */
    SDL_strlcpy(sim->botMgr.bots[TEST_SLOT].brainPath, TEST_BRAIN,
                sizeof(sim->botMgr.bots[TEST_SLOT].brainPath));
    serverSimSetBotConfig(sim, TEST_SLOT, (uint8_t)wantMode,
                          (uint8_t)wantLevel, 0, NULL);
    serverSimRememberManualBotPick(sim, TEST_SLOT);
    UT_ASSERT_MSG(strcmp(sim->lastBotModeKey, "survival") == 0 &&
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
        UT_ASSERT_MSG(strcmp(sim->lastBotModeKey, "survival") == 0 &&
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
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, true,
                                           &mode, &level));
    UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                  "the first bot of the new lobby must start at the defaults "
                  "(got %d/%d)", (int)mode, (int)level);

    serverSimDestroy(sim);
    return 0;
}

/* The shipped manifest's open_default mode, or -1 when the manifest is
 * missing or names none (then the open-game cases have nothing to test). */
static int open_default_mode(void) {
    BrainModes modes;
    if (!brainListLoadModesForPath(TEST_BRAIN, &modes)) return -1;
    return (modes.openDefaultMode > 0) ? modes.openDefaultMode : -1;
}

/* Index of level `lvlKey` inside the open_default mode, read from the
 * manifest so the cases do not name that mode. -1 when it lists no such
 * level. */
static int open_default_level(const char *lvlKey) {
    BrainModes modes;
    int m;
    if (!brainListLoadModesForPath(TEST_BRAIN, &modes)) return -1;
    m = modes.openDefaultMode;
    if (m <= 0 || m >= modes.modeCount) return -1;
    return brainModeFindLevel(&modes.modes[m], lvlKey);
}

/* An Open game starts a new bot in the brain's open_default mode at the
 * level it would have had in mode 0; Tournament and Strict start in mode 0;
 * a mode the host picked by hand still wins on Open. */
int run_lobby_bot_config_memory_open_game_start_mode(void) {
    static const gameType kClosed[2] = { gameTournament, gameStrictTournament };
    ServerSim *sim;
    int openMode = open_default_mode();
    int wantMode = 0, wantLevel = 0;
    uint8_t mode, level;
    int i;

    if (openMode < 0) SKIP_NO_MANIFEST();

    /* Open, no pick: open_default at Hard. */
    sim = make_lobby_sim_type(gameOpen);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    mode = 0; level = BOT_DIFFICULTY_HARD;
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, true,
                                           &mode, &level));
    wantLevel = open_default_level("hard");
    UT_ASSERT_MSG(wantLevel >= 0, "open_default mode %d lists no hard level",
                  openMode);
    UT_ASSERT_MSG((int)mode == openMode && (int)level == wantLevel,
                  "an Open game must start in open_default at Hard (got "
                  "%d/%d, want %d/%d)", (int)mode, (int)level, openMode,
                  wantLevel);

    /* The level moves by key: Medium in mode 0 is Medium in the new mode. */
    mode = 0; level = BOT_DIFFICULTY_MEDIUM;
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, false,
                                           &mode, &level));
    wantLevel = open_default_level("medium");
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
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, true,
                                           &mode, &level));
    UT_ASSERT_MSG((int)mode == openMode &&
                  (int)level == open_default_level("hard"),
                  "an unknown remembered mode on Open must fall to "
                  "open_default at Hard (got %d/%d)", (int)mode, (int)level);
    sim->lastBotModeKey[0]  = '\0';
    sim->lastBotLevelKey[0] = '\0';

    /* A caller's base that is not mode 0 is a real choice and stands. */
    UT_ASSERT(expected_indices("survival", "easy", &wantMode, &wantLevel));
    mode = (uint8_t)wantMode; level = (uint8_t)wantLevel;
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, false,
                                           &mode, &level));
    UT_ASSERT_MSG((int)mode == wantMode && (int)level == wantLevel,
                  "a non-default base moved to %d/%d", (int)mode, (int)level);

    /* The host picked Default by hand: the next Add Bot is Default. */
    SDL_strlcpy(sim->lastBotModeKey, "default", sizeof(sim->lastBotModeKey));
    SDL_strlcpy(sim->lastBotLevelKey, "easy", sizeof(sim->lastBotLevelKey));
    mode = 0; level = BOT_DIFFICULTY_HARD;
    UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN, true,
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
        UT_ASSERT(serverSimResolveNewBotConfig(sim, TEST_TEAM, TEST_BRAIN,
                                               true, &mode, &level));
        UT_ASSERT_MSG(mode == 0 && level == BOT_DIFFICULTY_HARD,
                      "game type %d must start in mode 0 (got %d/%d)",
                      (int)kClosed[i], (int)mode, (int)level);
        serverSimDestroy(sim);
    }
    return 0;
}

/* Seat a pretend bot in `slot` on the test brain: enough for serverSimIsBot
 * and for the brain path the follow reads. No brain is built. */
static void pretend_bot(ServerSim *sim, BYTE slot) {
    sim->playerConnected[slot]    = true;
    sim->lobbyPlayers[slot].isBot = true;
    SDL_strlcpy(sim->botMgr.bots[slot].brainPath, TEST_BRAIN,
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
int run_lobby_bot_config_memory_game_type_change(void) {
    const BYTE follow = 2, byHand = 3, other = 4;
    ServerSim *sim;
    int openMode = open_default_mode();
    int survMode = 0, survLevel = 0, medOpenLevel;
    uint8_t v;

    if (openMode < 0) SKIP_NO_MANIFEST();
    UT_ASSERT(expected_indices("survival", "easy", &survMode, &survLevel));
    medOpenLevel = open_default_level("medium");
    UT_ASSERT_MSG(medOpenLevel >= 0, "open_default lists no medium level");

    sim = make_lobby_sim_type(gameOpen);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    pretend_bot(sim, follow);
    pretend_bot(sim, byHand);
    pretend_bot(sim, other);
    /* All three came in as new bots on Open. */
    serverSimSetBotConfig(sim, follow, (uint8_t)openMode,
                          (uint8_t)medOpenLevel, 0, NULL);
    serverSimSetBotConfig(sim, byHand, (uint8_t)openMode,
                          BOT_DIFFICULTY_HARD, 0, NULL);
    serverSimSetBotConfig(sim, other, (uint8_t)survMode, (uint8_t)survLevel,
                          0, NULL);
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
    UT_ASSERT_MSG((int)sim->botConfigs[other].mode == survMode,
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
    UT_ASSERT((int)sim->botConfigs[other].mode == survMode);

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

/* A map commit that attaches or drops a scenario can change the game type
 * too (serverSimScenarioApplyLobbyRules). The bots follow it there as they
 * do when the host changes the type by hand. */
int run_lobby_bot_config_memory_map_commit_type(void) {
    const BYTE bot = 2, byHand = 3;
    ServerSim *sim;
    int openMode = open_default_mode();
    int medOpenLevel;

    if (openMode < 0) SKIP_NO_MANIFEST();
    medOpenLevel = open_default_level("medium");
    UT_ASSERT_MSG(medOpenLevel >= 0, "open_default lists no medium level");

    /* 1. An Open lobby picks a scripted map with no lobby template. The
          host's bots were added on Open, so they are on the Open start
          mode. The map puts the lobby on Scripted: the bots move to mode 0
          at the same level, and a mode set by hand stays. */
    sim = make_lobby_sim_type(gameOpen);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    pretend_bot(sim, bot);
    pretend_bot(sim, byHand);
    serverSimSetBotConfig(sim, bot, (uint8_t)openMode, (uint8_t)medOpenLevel,
                          0, NULL);
    serverSimSetBotConfig(sim, byHand, (uint8_t)openMode,
                          BOT_DIFFICULTY_HARD, 0, NULL);
    serverSimMarkBotModeSetByHand(sim, byHand);
    serverSimSetScenarioIdentity(sim, lobbyScenarioMap, "Test", "test.lua",
                                 "", false, false, false, false);
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
                                 false, false, false, false);
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

int run_lobby_bot_config_memory_dispatch_hand_mark(void) {
    const BYTE bot = 2;
    ServerSim *sim;
    int openMode = open_default_mode();

    if (openMode < 0) SKIP_NO_MANIFEST();
    sim = make_lobby_sim_type(gameOpen);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed");
    /* The host in slot 0, so the sender may edit the lobby. */
    serverSimAddPlayer(sim, 0, "Host", false);
    pretend_bot(sim, bot);
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
