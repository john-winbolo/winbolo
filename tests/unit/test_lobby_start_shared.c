/*
 * Shared lobby start tests (test_lobby_start_shared.c).
 *
 * With LOBBY_SHARED_STARTS on, any number of players may reserve the same
 * lobby start. Two pieces of judgement follow from that, and both live in
 * lobby_start_shared.h as pure string / integer helpers so they can be
 * pinned here without ImGui or a lobby:
 *
 *   (1) lobbyStartUniquePrefixLen — the shortest leading run of a name
 *       that tells it apart from every other holder name on the map;
 *   (2) lobbyStartHolderPrefixLabel — the mini map's label for a start's
 *       holders, those prefixes comma-joined, "C, M";
 *   (3) lobbyStartHolderNameLabel — the same list in full names, for the
 *       held-start tooltips;
 *   (4) lobbyStartFoldOwners — the one ownership class a start with
 *       several holders reads as: enemy if any holder is an enemy, self
 *       if the viewer is among them, ally otherwise.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lobby_start_shared.h"
#include "test_harness.h"

/* (1) Minimal unique prefixes over a set of holder names. */
int run_lobby_start_shared_prefix_len(void) {
    /* All distinct in the first letter: one char each. */
    const char *distinct[3] = { "Claude", "Morrison", "Zed" };
    UT_ASSERT_MSG(lobbyStartUniquePrefixLen(distinct, 3, 0) == 1,
                  "Claude is unique at 1 char, got %d",
                  lobbyStartUniquePrefixLen(distinct, 3, 0));
    UT_ASSERT(lobbyStartUniquePrefixLen(distinct, 3, 1) == 1);
    UT_ASSERT(lobbyStartUniquePrefixLen(distinct, 3, 2) == 1);

    /* A shared first letter pushes both to the first differing char. */
    const char *shared[3] = { "Mark", "Mary", "Zed" };
    UT_ASSERT_MSG(lobbyStartUniquePrefixLen(shared, 3, 0) == 4,
                  "Mark vs Mary needs all 4 chars, got %d",
                  lobbyStartUniquePrefixLen(shared, 3, 0));
    UT_ASSERT(lobbyStartUniquePrefixLen(shared, 3, 1) == 4);
    UT_ASSERT(lobbyStartUniquePrefixLen(shared, 3, 2) == 1);

    /* One name a prefix of the other: capped at the shorter name's
     * length, since no prefix can separate them. */
    const char *nested[2] = { "Bob", "Bobby" };
    UT_ASSERT_MSG(lobbyStartUniquePrefixLen(nested, 2, 0) == 3,
                  "Bob caps at its own 3 chars, got %d",
                  lobbyStartUniquePrefixLen(nested, 2, 0));
    UT_ASSERT_MSG(lobbyStartUniquePrefixLen(nested, 2, 1) == 4,
                  "Bobby differs from Bob at char 4, got %d",
                  lobbyStartUniquePrefixLen(nested, 2, 1));

    /* A single name needs one char; an empty name has no prefix. */
    const char *one[2] = { "Solo", "" };
    UT_ASSERT(lobbyStartUniquePrefixLen(one, 1, 0) == 1);
    UT_ASSERT(lobbyStartUniquePrefixLen(one, 2, 1) == 0);
    return 0;
}

/* (2) The mini map's comma-joined initials for a shared start. */
int run_lobby_start_shared_prefix_label(void) {
    /* Two players sharing one start, distinct initials: "C, M". */
    const char *names[2] = { "Claude", "Morrison" };
    int both[2] = { 0, 1 };
    char buf[64];
    int n = lobbyStartHolderPrefixLabel(names, 2, both, 2, buf, sizeof(buf));
    UT_ASSERT_MSG(n == 2, "both holders should be listed, got %d", n);
    UT_ASSERT_MSG(strcmp(buf, "C, M") == 0,
                  "expected \"C, M\", got \"%s\"", buf);

    /* One holder is just its own prefix, no comma. */
    int justOne[1] = { 1 };
    UT_ASSERT(lobbyStartHolderPrefixLabel(names, 2, justOne, 1,
                                          buf, sizeof(buf)) == 1);
    UT_ASSERT_MSG(strcmp(buf, "M") == 0, "expected \"M\", got \"%s\"", buf);

    /* No holders: an empty label, and nothing written past the NUL. */
    UT_ASSERT(lobbyStartHolderPrefixLabel(names, 2, both, 0,
                                          buf, sizeof(buf)) == 0);
    UT_ASSERT_MSG(buf[0] == '\0', "an unheld start has no label, got \"%s\"", buf);

    /* The prefixes are computed against the whole holder set, not just
     * the pair sharing this start: Mark and Mary force each other to four
     * chars even though only Mark and Claude share the start. */
    const char *clash[3] = { "Claude", "Mark", "Mary" };
    int pair[2] = { 0, 1 };
    UT_ASSERT(lobbyStartHolderPrefixLabel(clash, 3, pair, 2,
                                          buf, sizeof(buf)) == 2);
    UT_ASSERT_MSG(strcmp(buf, "C, Mark") == 0,
                  "expected \"C, Mark\", got \"%s\"", buf);

    /* A buffer too small stops before a half-written prefix rather than
     * truncating one, and still leaves a valid string. */
    char tiny[6];
    int fitted = lobbyStartHolderPrefixLabel(clash, 3, pair, 2,
                                             tiny, sizeof(tiny));
    UT_ASSERT_MSG(fitted == 1, "only the first prefix fits in 6 bytes, got %d",
                  fitted);
    UT_ASSERT_MSG(strcmp(tiny, "C") == 0, "expected \"C\", got \"%s\"", tiny);
    return 0;
}

/* (3) The full-name list the held-start tooltips name. */
int run_lobby_start_shared_name_label(void) {
    const char *names[3] = { "Claude", "Morrison", "Zed" };
    char buf[64];

    UT_ASSERT(lobbyStartHolderNameLabel(names, 3, buf, sizeof(buf)) == 3);
    UT_ASSERT_MSG(strcmp(buf, "Claude, Morrison, Zed") == 0,
                  "expected the three names comma-joined, got \"%s\"", buf);

    UT_ASSERT(lobbyStartHolderNameLabel(names, 1, buf, sizeof(buf)) == 1);
    UT_ASSERT_MSG(strcmp(buf, "Claude") == 0,
                  "a single holder is just its name, got \"%s\"", buf);

    UT_ASSERT(lobbyStartHolderNameLabel(names, 0, buf, sizeof(buf)) == 0);
    UT_ASSERT(buf[0] == '\0');

    /* An empty name is skipped, not left as a stray comma. */
    const char *withBlank[3] = { "Claude", "", "Zed" };
    UT_ASSERT(lobbyStartHolderNameLabel(withBlank, 3, buf, sizeof(buf)) == 2);
    UT_ASSERT_MSG(strcmp(buf, "Claude, Zed") == 0,
                  "expected \"Claude, Zed\", got \"%s\"", buf);
    return 0;
}

/* (4) One ownership class for a start with several holders. */
int run_lobby_start_shared_owner_fold(void) {
    int owners[4];

    /* Nobody on it. */
    UT_ASSERT(lobbyStartFoldOwners(NULL, 0) == LOBBY_START_OWNER_UNCLAIMED);
    owners[0] = LOBBY_START_OWNER_SELF;
    UT_ASSERT(lobbyStartFoldOwners(owners, 0) == LOBBY_START_OWNER_UNCLAIMED);

    /* A single holder folds to itself. */
    UT_ASSERT(lobbyStartFoldOwners(owners, 1) == LOBBY_START_OWNER_SELF);
    owners[0] = LOBBY_START_OWNER_ALLY;
    UT_ASSERT(lobbyStartFoldOwners(owners, 1) == LOBBY_START_OWNER_ALLY);
    owners[0] = LOBBY_START_OWNER_ENEMY;
    UT_ASSERT(lobbyStartFoldOwners(owners, 1) == LOBBY_START_OWNER_ENEMY);

    /* Allies only stay ally; the viewer among them makes it self. */
    owners[0] = LOBBY_START_OWNER_ALLY;
    owners[1] = LOBBY_START_OWNER_ALLY;
    UT_ASSERT(lobbyStartFoldOwners(owners, 2) == LOBBY_START_OWNER_ALLY);
    owners[1] = LOBBY_START_OWNER_SELF;
    UT_ASSERT_MSG(lobbyStartFoldOwners(owners, 2) == LOBBY_START_OWNER_SELF,
                  "a start the viewer shares with an ally reads as self");

    /* One enemy anywhere in the list wins, whatever the order. */
    owners[0] = LOBBY_START_OWNER_SELF;
    owners[1] = LOBBY_START_OWNER_ALLY;
    owners[2] = LOBBY_START_OWNER_ENEMY;
    UT_ASSERT_MSG(lobbyStartFoldOwners(owners, 3) == LOBBY_START_OWNER_ENEMY,
                  "an enemy on the start must colour it enemy");
    owners[0] = LOBBY_START_OWNER_ENEMY;
    owners[1] = LOBBY_START_OWNER_SELF;
    owners[2] = LOBBY_START_OWNER_ALLY;
    UT_ASSERT(lobbyStartFoldOwners(owners, 3) == LOBBY_START_OWNER_ENEMY);
    return 0;
}
