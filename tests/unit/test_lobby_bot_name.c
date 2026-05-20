/*
 * Coverage for lobbyBotNameAcceptable — the pure predicate that gates
 * client-supplied bot names in PACKET_LOBBY_ADD_BOT and
 * PACKET_LOBBY_BOT_CONFIG. Both handlers reject with
 * LOBBY_REJECT_INVALID on any failure here, so the predicate carries
 * the full validator + uniqueness pass.
 *
 * The handlers pass transportUdpServerGetPlayerName as the lookup
 * callback. Tests stub the lookup with a module-static table so the
 * predicate can be driven without seeding udpServer.
 *
 * skipSlot is exercised in two directions: a BOT_CONFIG-style call
 * passing the bot's own slot must still trip the validator (cases d/e),
 * and a collision at the skip-slot itself must be excluded from the
 * uniqueness check (case f) so a no-op or normalization-preserving
 * rename doesn't self-collide on the bot already sitting there.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "netpacks.h"
#include "playername_validate.h"
#include "test_harness.h"

static const char *g_names[MAX_TANKS];

static void clear_names(void) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) g_names[i] = NULL;
}

static const char *stub_lookup(BYTE slot) {
    if (slot >= MAX_TANKS) return NULL;
    return g_names[slot];
}

int run_lobby_bot_name_rejects_reserved_prefix(void) {
    char validated[PACKET_MAX_PLAYER_NAME];
    PlayerNameValidationError err = PLAYER_NAME_OK;
    int collision = -1;
    clear_names();
    UT_ASSERT(!lobbyBotNameAcceptable("*foo", validated, sizeof(validated),
                                       -1, stub_lookup, &err, &collision));
    UT_ASSERT(err == PLAYER_NAME_ERR_RESERVED_PREFIX);
    UT_ASSERT(collision == -1);
    return 0;
}

int run_lobby_bot_name_rejects_disallowed_char(void) {
    char validated[PACKET_MAX_PLAYER_NAME];
    PlayerNameValidationError err = PLAYER_NAME_OK;
    int collision = -1;
    clear_names();
    UT_ASSERT(!lobbyBotNameAcceptable("foo\x01""bar", validated,
                                       sizeof(validated), -1, stub_lookup,
                                       &err, &collision));
    UT_ASSERT(err == PLAYER_NAME_ERR_DISALLOWED_CHAR);
    UT_ASSERT(collision == -1);
    return 0;
}

int run_lobby_bot_name_rejects_collision(void) {
    char validated[PACKET_MAX_PLAYER_NAME];
    PlayerNameValidationError err = PLAYER_NAME_OK;
    int collision = -1;
    clear_names();
    g_names[3] = "Alice";
    /* skipSlot=-1 mirrors the ADD_BOT call site (no slot to exclude). */
    UT_ASSERT(!lobbyBotNameAcceptable("Alice", validated, sizeof(validated),
                                       -1, stub_lookup, &err, &collision));
    UT_ASSERT(err == PLAYER_NAME_OK);
    UT_ASSERT_MSG(collision == 3,
                  "expected collision slot 3, got %d", collision);
    return 0;
}

int run_lobby_bot_name_skip_does_not_bypass_validator_prefix(void) {
    char validated[PACKET_MAX_PLAYER_NAME];
    PlayerNameValidationError err = PLAYER_NAME_OK;
    int collision = -1;
    clear_names();
    /* skipSlot mirrors BOT_CONFIG: the bot's own slot is excluded
     * from uniqueness, but the validator still has to pass. */
    UT_ASSERT(!lobbyBotNameAcceptable("*foo", validated, sizeof(validated),
                                       5, stub_lookup, &err, &collision));
    UT_ASSERT(err == PLAYER_NAME_ERR_RESERVED_PREFIX);
    UT_ASSERT(collision == -1);
    return 0;
}

int run_lobby_bot_name_skip_does_not_bypass_validator_control(void) {
    char validated[PACKET_MAX_PLAYER_NAME];
    PlayerNameValidationError err = PLAYER_NAME_OK;
    int collision = -1;
    clear_names();
    UT_ASSERT(!lobbyBotNameAcceptable("foo\x01""bar", validated,
                                       sizeof(validated), 5, stub_lookup,
                                       &err, &collision));
    UT_ASSERT(err == PLAYER_NAME_ERR_DISALLOWED_CHAR);
    UT_ASSERT(collision == -1);
    return 0;
}

int run_lobby_bot_name_skip_excludes_self_from_uniqueness(void) {
    char validated[PACKET_MAX_PLAYER_NAME];
    PlayerNameValidationError err = PLAYER_NAME_OK;
    int collision = -1;
    clear_names();
    /* The bot currently in slot 5 already holds "Alice"; the
     * predicate must not flag that as a collision when slot 5 is
     * the slot being edited. Regression guard for the BOT_CONFIG
     * no-op / normalization-preserving rename path. */
    g_names[5] = "Alice";
    UT_ASSERT(lobbyBotNameAcceptable("Alice", validated, sizeof(validated),
                                      5, stub_lookup, &err, &collision));
    UT_ASSERT(strcmp(validated, "Alice") == 0);
    return 0;
}

int run_lobby_bot_name_accepts_clean_unique(void) {
    char validated[PACKET_MAX_PLAYER_NAME];
    PlayerNameValidationError err = PLAYER_NAME_OK;
    int collision = -1;
    clear_names();
    g_names[2] = "Bob";
    g_names[7] = "Carol";
    UT_ASSERT(lobbyBotNameAcceptable("Dave", validated, sizeof(validated),
                                      -1, stub_lookup, &err, &collision));
    UT_ASSERT_MSG(strcmp(validated, "Dave") == 0,
                  "expected validated buffer to hold \"Dave\", got \"%s\"",
                  validated);
    return 0;
}
