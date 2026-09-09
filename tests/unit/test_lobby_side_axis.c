/*
 * Lobby map-preview compass axis tests (test_lobby_side_axis.c).
 *
 * The compass drawn on the lobby map preview is a shortcut for the two
 * per-team "Start:" combos: it shows up only when exactly two teams have
 * members, lights an N/S or E/W axis at a time, and a click either puts
 * the two teams on that axis or, when they are already on it, puts both
 * back to START_SIDE_ANY.
 *
 * All of that judgement lives in lobby_side_axis.h as pure integer
 * helpers, so it can be pinned here without ImGui or a lobby:
 *
 *   (1) lobbySideTwoTeamPair — exactly two populated teams, lower id
 *       first, and nothing reported for none / one / three or more;
 *   (2) lobbySideAxisOfSide / lobbySideAxisOfPair — which axis a side is
 *       on, and which axis (if any) a pair of sides forms;
 *   (3) lobbySideAxisClick — what a click on an axis has to send, set
 *       and unset.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lobby_side_axis.h"
#include "test_harness.h"

#define K_TEAMS 16   /* MAX_TANKS: teams 1..15, team 0 is "no team" */

/* (1) Exactly two populated teams, lower id first. */
int run_lobby_side_axis_two_team_pair(void) {
    int counts[K_TEAMS];
    int a = -1, b = -1;

    /* No team at all. */
    memset(counts, 0, sizeof(counts));
    UT_ASSERT_MSG(!lobbySideTwoTeamPair(counts, K_TEAMS, &a, &b),
                  "an empty lobby is not a two-team pair");
    UT_ASSERT_MSG(a == 0 && b == 0,
                  "a false answer must report no teams, got %d and %d", a, b);

    /* Team 0 is "no team" and never counts, however many sit there. */
    memset(counts, 0, sizeof(counts));
    counts[0] = 5;
    UT_ASSERT_MSG(!lobbySideTwoTeamPair(counts, K_TEAMS, &a, &b),
                  "unassigned players must not make a pair");

    /* One team. */
    memset(counts, 0, sizeof(counts));
    counts[1] = 3;
    UT_ASSERT_MSG(!lobbySideTwoTeamPair(counts, K_TEAMS, &a, &b),
                  "one populated team is not a pair");

    /* Two teams, adjacent ids. */
    memset(counts, 0, sizeof(counts));
    counts[1] = 1;
    counts[2] = 4;
    UT_ASSERT_MSG(lobbySideTwoTeamPair(counts, K_TEAMS, &a, &b),
                  "teams 1 and 2 populated should be a pair");
    UT_ASSERT_MSG(a == 1 && b == 2,
                  "pair should be (1,2), got (%d,%d)", a, b);

    /* Two teams, far apart and out of order in the table — the lower id
     * is still teamA, which is the one that takes north / east. */
    memset(counts, 0, sizeof(counts));
    counts[15] = 2;
    counts[3]  = 1;
    UT_ASSERT_MSG(lobbySideTwoTeamPair(counts, K_TEAMS, &a, &b),
                  "teams 3 and 15 populated should be a pair");
    UT_ASSERT_MSG(a == 3 && b == 15,
                  "pair should be (3,15) lower first, got (%d,%d)", a, b);

    /* An in-use team with no members does not count — the compass keys
     * off members, the way the panel's team headers count them. */
    memset(counts, 0, sizeof(counts));
    counts[1] = 2;
    counts[2] = 2;
    counts[7] = 0;
    UT_ASSERT_MSG(lobbySideTwoTeamPair(counts, K_TEAMS, &a, &b),
                  "an empty third team should leave the pair alone");
    UT_ASSERT_MSG(a == 1 && b == 2,
                  "pair should be (1,2), got (%d,%d)", a, b);

    /* Three teams: no pair, and no teams reported. */
    memset(counts, 0, sizeof(counts));
    counts[1] = 1;
    counts[2] = 1;
    counts[3] = 1;
    a = 9; b = 9;
    UT_ASSERT_MSG(!lobbySideTwoTeamPair(counts, K_TEAMS, &a, &b),
                  "three populated teams are not a pair");
    UT_ASSERT_MSG(a == 0 && b == 0,
                  "a false answer must report no teams, got %d and %d", a, b);

    /* A NULL table answers false rather than reading it. */
    UT_ASSERT_MSG(!lobbySideTwoTeamPair(NULL, K_TEAMS, &a, &b),
                  "a NULL count table is not a pair");
    return 0;
}

/* (2) Which axis a side sits on, and which axis a pair of sides forms.
 *     Only complementary opposites count as a set axis. */
int run_lobby_side_axis_pair_classification(void) {
    UT_ASSERT_MSG(lobbySideAxisOfSide(START_SIDE_N) == LOBBY_SIDE_AXIS_NS,
                  "north is on the N/S axis");
    UT_ASSERT_MSG(lobbySideAxisOfSide(START_SIDE_S) == LOBBY_SIDE_AXIS_NS,
                  "south is on the N/S axis");
    UT_ASSERT_MSG(lobbySideAxisOfSide(START_SIDE_E) == LOBBY_SIDE_AXIS_EW,
                  "east is on the E/W axis");
    UT_ASSERT_MSG(lobbySideAxisOfSide(START_SIDE_W) == LOBBY_SIDE_AXIS_EW,
                  "west is on the E/W axis");
    UT_ASSERT_MSG(lobbySideAxisOfSide(START_SIDE_ANY) == LOBBY_SIDE_AXIS_NONE,
                  "Any is on no axis");
    UT_ASSERT_MSG(lobbySideAxisOfSide((BYTE)200) == LOBBY_SIDE_AXIS_NONE,
                  "a side value off the wire range is on no axis");

    static const struct {
        BYTE a;
        BYTE b;
        int  want;
        const char *what;
    } k_pairs[] = {
        { START_SIDE_N,   START_SIDE_S,   LOBBY_SIDE_AXIS_NS,   "N then S" },
        { START_SIDE_S,   START_SIDE_N,   LOBBY_SIDE_AXIS_NS,   "S then N" },
        { START_SIDE_E,   START_SIDE_W,   LOBBY_SIDE_AXIS_EW,   "E then W" },
        { START_SIDE_W,   START_SIDE_E,   LOBBY_SIDE_AXIS_EW,   "W then E" },
        { START_SIDE_N,   START_SIDE_N,   LOBBY_SIDE_AXIS_NONE, "both north" },
        { START_SIDE_N,   START_SIDE_E,   LOBBY_SIDE_AXIS_NONE, "N with E" },
        { START_SIDE_N,   START_SIDE_ANY, LOBBY_SIDE_AXIS_NONE, "only one side set" },
        { START_SIDE_ANY, START_SIDE_S,   LOBBY_SIDE_AXIS_NONE, "only the other set" },
        { START_SIDE_ANY, START_SIDE_ANY, LOBBY_SIDE_AXIS_NONE, "neither set" },
    };
    size_t i;
    for (i = 0; i < sizeof(k_pairs) / sizeof(k_pairs[0]); i++) {
        int got = lobbySideAxisOfPair(k_pairs[i].a, k_pairs[i].b);
        UT_ASSERT_MSG(got == k_pairs[i].want,
                      "%s: want axis %d, got %d",
                      k_pairs[i].what, k_pairs[i].want, got);
    }

    /* The sides of an axis, in the order they are handed out. */
    BYTE sa = 0xFF, sb = 0xFF;
    lobbySideAxisSides(LOBBY_SIDE_AXIS_NS, &sa, &sb);
    UT_ASSERT_MSG(sa == START_SIDE_N && sb == START_SIDE_S,
                  "N/S hands out north then south, got %u then %u",
                  (unsigned)sa, (unsigned)sb);
    lobbySideAxisSides(LOBBY_SIDE_AXIS_EW, &sa, &sb);
    UT_ASSERT_MSG(sa == START_SIDE_E && sb == START_SIDE_W,
                  "E/W hands out east then west, got %u then %u",
                  (unsigned)sa, (unsigned)sb);
    lobbySideAxisSides(LOBBY_SIDE_AXIS_NONE, &sa, &sb);
    UT_ASSERT_MSG(sa == START_SIDE_ANY && sb == START_SIDE_ANY,
                  "no axis hands out Any twice, got %u then %u",
                  (unsigned)sa, (unsigned)sb);
    return 0;
}

/* (3) What a click sends: set the axis, or clear the one already set. */
int run_lobby_side_axis_click_targets(void) {
    BYTE sa = 0xFF, sb = 0xFF;

    /* Unset teams, click N/S: the lower team id takes north. */
    UT_ASSERT_MSG(lobbySideAxisClick(LOBBY_SIDE_AXIS_NS,
                                     LOBBY_SIDE_AXIS_NONE, &sa, &sb),
                  "clicking N/S on unset teams should send something");
    UT_ASSERT_MSG(sa == START_SIDE_N && sb == START_SIDE_S,
                  "N/S click: want N then S, got %u then %u",
                  (unsigned)sa, (unsigned)sb);

    /* Unset teams, click E/W. */
    UT_ASSERT_MSG(lobbySideAxisClick(LOBBY_SIDE_AXIS_EW,
                                     LOBBY_SIDE_AXIS_NONE, &sa, &sb),
                  "clicking E/W on unset teams should send something");
    UT_ASSERT_MSG(sa == START_SIDE_E && sb == START_SIDE_W,
                  "E/W click: want E then W, got %u then %u",
                  (unsigned)sa, (unsigned)sb);

    /* The axis already set: clicking it puts both teams back to Any. */
    UT_ASSERT_MSG(lobbySideAxisClick(LOBBY_SIDE_AXIS_NS,
                                     LOBBY_SIDE_AXIS_NS, &sa, &sb),
                  "clicking the set axis should send something");
    UT_ASSERT_MSG(sa == START_SIDE_ANY && sb == START_SIDE_ANY,
                  "clearing N/S: want Any twice, got %u then %u",
                  (unsigned)sa, (unsigned)sb);
    UT_ASSERT_MSG(lobbySideAxisClick(LOBBY_SIDE_AXIS_EW,
                                     LOBBY_SIDE_AXIS_EW, &sa, &sb),
                  "clicking the set E/W axis should send something");
    UT_ASSERT_MSG(sa == START_SIDE_ANY && sb == START_SIDE_ANY,
                  "clearing E/W: want Any twice, got %u then %u",
                  (unsigned)sa, (unsigned)sb);

    /* The other axis while one is set: overwrite both sides. */
    UT_ASSERT_MSG(lobbySideAxisClick(LOBBY_SIDE_AXIS_EW,
                                     LOBBY_SIDE_AXIS_NS, &sa, &sb),
                  "clicking E/W while N/S is set should send something");
    UT_ASSERT_MSG(sa == START_SIDE_E && sb == START_SIDE_W,
                  "switching to E/W: want E then W, got %u then %u",
                  (unsigned)sa, (unsigned)sb);

    /* A half-set or mismatched pair reads as no axis, so a click on
     * either axis assigns it rather than clearing it. */
    UT_ASSERT_MSG(lobbySideAxisClick(LOBBY_SIDE_AXIS_NS,
                                     lobbySideAxisOfPair(START_SIDE_N,
                                                         START_SIDE_E),
                                     &sa, &sb),
                  "clicking N/S over a mismatched pair should send something");
    UT_ASSERT_MSG(sa == START_SIDE_N && sb == START_SIDE_S,
                  "mismatched pair: want N then S, got %u then %u",
                  (unsigned)sa, (unsigned)sb);

    /* No axis: nothing to send, and Any reported either way. */
    sa = 0xFF; sb = 0xFF;
    UT_ASSERT_MSG(!lobbySideAxisClick(LOBBY_SIDE_AXIS_NONE,
                                      LOBBY_SIDE_AXIS_NONE, &sa, &sb),
                  "there is nothing to click when there is no axis");
    UT_ASSERT_MSG(sa == START_SIDE_ANY && sb == START_SIDE_ANY,
                  "no axis: want Any twice, got %u then %u",
                  (unsigned)sa, (unsigned)sb);
    return 0;
}
