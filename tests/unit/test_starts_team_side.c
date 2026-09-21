/*
 * Team start-side placement tests (test_starts_team_side.c).
 *
 * startsAssignBatch takes a per-team side table (teamStartSide, indexed by
 * team number) and keeps each team on the side it chose: a side team takes
 * only starts its side accepts, a team with no side stays off the sides
 * the other teams chose, and every slot the placement leaves without a
 * start of its own rides one beside its own side instead of coming back
 * unplaced. These tests pin that contract:
 *
 *   (4)  two side teams each stay on their own side;
 *   (5)  a side team larger than its side rides its own starts;
 *   (6)  ...and never crosses to free starts on the opposite side;
 *   (7)  a NULL side table and an all-ANY table place identically;
 *   (8)  a reservation off the team's side is honoured, the rest go home;
 *   (9)  a side with no start on the map falls back to any side;
 *   (10) a team's quota is capped at what its side can hold, so the other
 *        team is not starved of starts it could have taken;
 *   (11) a team with no side is kept off a chosen side, its overflow
 *        riding the starts it did get.
 *
 * Layouts are built on a running sim's map the way test_starts_assign_batch.c
 * does: each start square forced to deep sea with no mine, the map's own
 * pills and bases cleared so only the layout drives placement. A side is
 * read off the bounding box of every start, so a layout always spans both
 * north and south (starts on land still shape the box), and each result is
 * checked against start_sides.h the same way the batch classifies it.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"     /* mapSetPos */
#include "starts.h"       /* startsAssignBatch, startsGetMaxs */
#include "start_sides.h"  /* START_SIDE_*, startSideMaskFor */
#include "bolo_rand.h"    /* bolo_srand */
#include "server_sim.h"   /* ut_make_running_sim, serverSimGetGameSim */
#include "test_harness.h"

/* One start of a layout. A start on land is still a start (it shapes the
 * bounding box) but startsIsValidSquare rejects it. */
typedef struct {
    BYTE x;
    BYTE y;
    bool sea;
} LayoutStart;

/* Column layout: two rows of four due-north starts (y=40 and y=55) and one
 * row of four due-south starts (y=210), all near x=125 so the sector test
 * gives N or S alone, never a diagonal. The bbox is x 100..150, y 40..210,
 * centre (125,125), tolerance 6 by 21. */
#define K_COL_N_A   0   /* indices 0..3: north row A */
#define K_COL_S     4   /* indices 4..7: south row */
#define K_COL_N_B   8   /* indices 8..11: north row B */
static const LayoutStart k_column[12] = {
    { 100,  40, true }, { 115,  40, true }, { 135,  40, true }, { 150,  40, true },
    { 100, 210, true }, { 115, 210, true }, { 135, 210, true }, { 150, 210, true },
    { 100,  55, true }, { 115,  55, true }, { 135,  55, true }, { 150,  55, true },
};

/* Two centre-band starts for the column layout, within the tolerance of
 * (125,125) on both axes. */
static const LayoutStart k_centre[2] = {
    { 120, 125, true }, { 130, 125, true },
};

/* Corner layout: four clusters of four, one per corner, every start a
 * diagonal (NW = N|W and so on). bbox 40..210 on both axes. */
static const LayoutStart k_corners[16] = {
    {  40,  40, true }, {  50,  40, true }, {  40,  50, true }, {  50,  50, true },
    { 200,  40, true }, { 210,  40, true }, { 200,  50, true }, { 210,  50, true },
    {  40, 200, true }, {  50, 200, true }, {  40, 210, true }, {  50, 210, true },
    { 200, 200, true }, { 210, 200, true }, { 200, 210, true }, { 210, 210, true },
};

/* Ring layout: sixteen starts around the edge of the box, two each of W,
 * NW, N, NE, E, SE, S and SW — the shape of Everard Island, where the
 * starts circle the island rather than sitting in corner clusters. What is
 * not the east here is a horseshoe of west, north and south that comes
 * back to meet the east at both ends, so "off the east" is not the same
 * thing as "on the west". bbox x 76..184, y 100..156. */
static const LayoutStart k_ring[16] = {
    {  76, 140, true }, {  76, 124, true },   /* W  */
    {  92, 100, true }, { 108, 100, true },   /* NW */
    { 124, 100, true }, { 140, 100, true },   /* N  */
    { 156, 100, true }, { 172, 100, true },   /* NE */
    { 184, 124, true }, { 184, 140, true },   /* E  */
    { 172, 156, true }, { 156, 156, true },   /* SE */
    { 140, 156, true }, { 124, 156, true },   /* S  */
    { 108, 156, true }, {  92, 156, true },   /* SW */
};

static void begin_layout(GameSim *gs) {
    gs->pb->numPills = 0;   /* no pills near the synthetic starts */
    gs->bs->numBases = 0;   /* no owned/neutral bases to steer anchors */
    gs->ss->numStarts = 0;
}

static void add_starts(GameSim *gs, const LayoutStart *items, int n, bool sea) {
    int i;
    for (i = 0; i < n; i++) {
        BYTE idx = gs->ss->numStarts++;
        mapSetPos(gs, &gs->mp, items[i].x, items[i].y,
                  (sea && items[i].sea) ? DEEP_SEA : GRASS, FALSE, TRUE);
        gs->ss->item[idx].x = items[i].x;
        gs->ss->item[idx].y = items[i].y;
        gs->ss->item[idx].dir = 0;
    }
}

/* Reset the per-slot inputs to "nobody connected, solo, no reservation",
 * and every team to no side. */
static void reset_inputs(bool *connected, BYTE *team, BYTE *reserved, BYTE *side) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        connected[i] = false;
        team[i] = 0;
        reserved[i] = MAX_STARTS;
    }
    for (i = 0; i <= MAX_TANKS; i++) {
        side[i] = START_SIDE_ANY;
    }
}

/* Connect slots first..first+count-1 on team tn. */
static void add_team(bool *connected, BYTE *team, int first, int count, BYTE tn) {
    int i;
    for (i = first; i < first + count; i++) {
        connected[i] = true;
        team[i] = tn;
    }
}

/* The side mask the batch itself would compute for a start. */
static BYTE mask_of(GameSim *gs, BYTE idx) {
    int leftPos;
    int rightPos;
    int topPos;
    int bottomPos;
    startsGetMaxs(&gs->ss, &leftPos, &rightPos, &topPos, &bottomPos);
    return startSideMaskFor(gs->ss->item[idx].x, gs->ss->item[idx].y,
                            leftPos, topPos, rightPos, bottomPos);
}

static bool is_placed(GameSim *gs, BYTE idx) {
    return idx < gs->ss->numStarts;
}

static bool is_north(GameSim *gs, BYTE idx) {
    return is_placed(gs, idx) && (mask_of(gs, idx) & START_SIDE_BIT_N) != 0;
}

static bool is_south(GameSim *gs, BYTE idx) {
    return is_placed(gs, idx) && (mask_of(gs, idx) & START_SIDE_BIT_S) != 0;
}

static bool is_west(GameSim *gs, BYTE idx) {
    return is_placed(gs, idx) && (mask_of(gs, idx) & START_SIDE_BIT_W) != 0;
}

/* How many of the slots first..first+count-1 hold a start on the west of
 * the box. On the corner layout that is "in one of the two west corners",
 * which is how a team spread across its side is counted. */
static int count_west(GameSim *gs, const BYTE *out, int first, int count) {
    int west = 0;
    int i;
    for (i = first; i < first + count; i++) {
        if (is_west(gs, out[i])) west++;
    }
    return west;
}

/* Number of slots in first..first+count-1 whose index repeats an earlier
 * slot's in the same range. */
static int count_repeats(const BYTE *out, int first, int count) {
    int repeats = 0;
    int i;
    int j;
    for (i = first; i < first + count; i++) {
        for (j = first; j < i; j++) {
            if (out[j] == out[i]) { repeats++; break; }
        }
    }
    return repeats;
}

/* Number of distinct indices among slots first..first+count-1. */
static int count_distinct(const BYTE *out, int first, int count) {
    return count - count_repeats(out, first, count);
}

/* True when some slot in first..first+count-1 holds idx. */
static bool slot_range_uses(const BYTE *out, int first, int count, BYTE idx) {
    int i;
    for (i = first; i < first + count; i++) {
        if (out[i] == idx) return true;
    }
    return false;
}

/* (4) Team 1 side N and team 2 side S, two players each, on four north and
 *     four south starts: every team-1 index is north, every team-2 index
 *     south, and all four are distinct. */
int run_starts_side_team_stays_on_its_side(void) {
    ServerSim *sim = ut_make_running_sim("SideNS");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, &k_column[K_COL_N_A], 4, true);
    add_starts(gs, &k_column[K_COL_S], 4, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    int i;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 2, 1);
    add_team(connected, team, 2, 2, 2);
    side[1] = START_SIDE_N;
    side[2] = START_SIDE_S;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    for (i = 0; i < 2; i++) {
        UT_ASSERT_MSG(is_north(gs, out[i]),
                      "team 1 (side N) slot %d landed on start %u, not a north start",
                      i, (unsigned)out[i]);
    }
    for (i = 2; i < 4; i++) {
        UT_ASSERT_MSG(is_south(gs, out[i]),
                      "team 2 (side S) slot %d landed on start %u, not a south start",
                      i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, 0, 4) == 4,
                  "four players on eight starts should not share: %u %u %u %u",
                  (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3]);
    serverSimDestroy(sim);
    return 0;
}

/* Six players on team 1 (side S) against a map whose only valid starts are
 * the four south ones; the north row is on land. Shared by (5) and (6). */
static int check_six_on_four_south(GameSim *gs, const BYTE *out) {
    int i;
    for (i = 0; i < 6; i++) {
        UT_ASSERT_MSG(is_placed(gs, out[i]),
                      "slot %d came back unplaced (%u)", i, (unsigned)out[i]);
        UT_ASSERT_MSG(!is_north(gs, out[i]),
                      "slot %d landed on north start %u", i, (unsigned)out[i]);
        UT_ASSERT_MSG(is_south(gs, out[i]),
                      "slot %d landed on start %u, not a south start", i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_repeats(out, 0, 6) >= 2,
                  "six players on four starts should share at least twice, saw %d repeats",
                  count_repeats(out, 0, 6));
    for (i = K_COL_S; i < K_COL_S + 4; i++) {
        UT_ASSERT_MSG(slot_range_uses(out, 0, 6, (BYTE)i),
                      "south start %d was left unused while teammates doubled up", i);
    }
    return 0;
}

/* (5) Side S with six players and four south starts (the north row on land
 *     so it shapes the bbox but is never valid): everyone is placed, every
 *     index is south, and at least two players ride a teammate's start. */
int run_starts_side_overflow_rides_own_side(void) {
    ServerSim *sim = ut_make_running_sim("SideOver");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, &k_column[K_COL_N_A], 4, false);   /* north row on land */
    add_starts(gs, &k_column[K_COL_S], 4, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 6, 1);
    side[1] = START_SIDE_S;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    UT_ASSERT(check_six_on_four_south(gs, out) == 0);
    serverSimDestroy(sim);
    return 0;
}

/* (6) As (5), but both north rows are deep sea and free: eight valid north
 *     starts sit unused and the south team still never takes one. */
int run_starts_side_never_crosses_when_opposite_free(void) {
    ServerSim *sim = ut_make_running_sim("SideCross");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, &k_column[K_COL_N_A], 4, true);
    add_starts(gs, &k_column[K_COL_S], 4, true);
    add_starts(gs, &k_column[K_COL_N_B], 4, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 6, 1);
    side[1] = START_SIDE_S;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    UT_ASSERT(check_six_on_four_south(gs, out) == 0);
    serverSimDestroy(sim);
    return 0;
}

/* (7) A NULL side table and an all-START_SIDE_ANY table are the same input:
 *     with the same seed, two teams, two solos and a reservation, the two
 *     runs produce byte-identical output across every slot. */
int run_starts_side_any_matches_legacy(void) {
    ServerSim *sim = ut_make_running_sim("SideAny");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, k_corners, 16, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE outNull[MAX_TANKS];
    BYTE outAny[MAX_TANKS];
    int s;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 4, 1);
    add_team(connected, team, 4, 4, 2);
    add_team(connected, team, 8, 2, 0);   /* two solos */
    reserved[4] = 12;                     /* one team-2 member picked a SE start */

    for (s = 0; s < 4; s++) {
        bolo_srand((uint64_t)(s * 31 + 7));
        startsAssignBatch(gs, &gs->ss, connected, team, outNull, reserved, NULL);
        bolo_srand((uint64_t)(s * 31 + 7));
        startsAssignBatch(gs, &gs->ss, connected, team, outAny, reserved, side);
        UT_ASSERT_MSG(memcmp(outNull, outAny, MAX_TANKS) == 0,
                      "seed %d: NULL side table and all-ANY table placed slots differently", s);
    }
    serverSimDestroy(sim);
    return 0;
}

/* (8) Team 1 side N with three players; slot 0 holds a reservation on a
 *     south start. The reservation wins for that slot, and its unreserved
 *     teammates still go north rather than clustering round it. */
int run_starts_side_reservation_beats_side(void) {
    ServerSim *sim = ut_make_running_sim("SideRes");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, &k_column[K_COL_N_A], 4, true);
    add_starts(gs, &k_column[K_COL_S], 4, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 3, 1);
    side[1] = START_SIDE_N;
    reserved[0] = K_COL_S;                /* a south start */
    UT_ASSERT(is_south(gs, K_COL_S));

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, reserved, side);

    UT_ASSERT_MSG(out[0] == K_COL_S,
                  "reserved slot should land on its south start %d, got %u",
                  K_COL_S, (unsigned)out[0]);
    UT_ASSERT_MSG(is_north(gs, out[1]),
                  "unreserved teammate slot 1 landed on %u, not a north start", (unsigned)out[1]);
    UT_ASSERT_MSG(is_north(gs, out[2]),
                  "unreserved teammate slot 2 landed on %u, not a north start", (unsigned)out[2]);
    UT_ASSERT_MSG(out[1] != out[2],
                  "two teammates share start %u while north starts are free", (unsigned)out[1]);
    serverSimDestroy(sim);
    return 0;
}

/* (9) Team 1 side E on a map with only north and south starts. No start
 *     accepts E, so the team is placed as if it had no side: four players
 *     on four distinct real starts, not stacked on one. */
int run_starts_side_empty_side_falls_back(void) {
    ServerSim *sim = ut_make_running_sim("SideEmpty");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, &k_column[K_COL_N_A], 4, true);
    add_starts(gs, &k_column[K_COL_S], 4, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    int i;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 4, 1);
    side[1] = START_SIDE_E;
    for (i = 0; i < gs->ss->numStarts; i++) {
        UT_ASSERT_MSG(!startSideAccepts(mask_of(gs, (BYTE)i), START_SIDE_E),
                      "layout start %d accepts side E; the fallback would not be exercised", i);
    }

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(is_placed(gs, out[i]),
                      "slot %d came back unplaced (%u)", i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, 0, 4) == 4,
                  "four players on eight starts should not share: %u %u %u %u",
                  (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3]);
    serverSimDestroy(sim);
    return 0;
}

/* Team 1 (side N, slots 0..aCount-1) and team 2 (no side, the next bCount
 * slots) on a layout whose north row is starts 0..3: team 2 holds bCount
 * distinct real starts, none north; team 1 never goes south and its members
 * cover all four north starts, the rest riding. */
static int check_north_team_and_any_team(GameSim *gs, const BYTE *out,
                                         int aCount, int bCount, const char *what) {
    int i;
    for (i = 0; i < aCount + bCount; i++) {
        UT_ASSERT_MSG(is_placed(gs, out[i]),
                      "%s: slot %d came back unplaced (%u)", what, i, (unsigned)out[i]);
    }
    for (i = 0; i < aCount; i++) {
        UT_ASSERT_MSG(!is_south(gs, out[i]),
                      "%s: team 1 (side N) slot %d landed on south start %u", what, i, (unsigned)out[i]);
    }
    for (i = K_COL_N_A; i < K_COL_N_A + 4; i++) {
        UT_ASSERT_MSG(slot_range_uses(out, 0, aCount, (BYTE)i),
                      "%s: north start %d was not taken by team 1", what, i);
    }
    for (i = aCount; i < aCount + bCount; i++) {
        UT_ASSERT_MSG(!is_north(gs, out[i]),
                      "%s: team 2 (no side) slot %d landed on north start %u", what, i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, aCount, bCount) == 4,
                  "%s: team 2 should hold four distinct starts, holds %d",
                  what, count_distinct(out, aCount, bCount));
    return 0;
}

/* (10) Team 1 side N with eight players and only four north starts, team 2
 *      with no side and four players on the four south starts. Team 1's
 *      quota stops at the four starts it can use and its other members
 *      ride; team 2 gets all four of its starts instead of the two an
 *      uncapped 8:4 split would leave it. A second run adds two centre
 *      starts and sizes the teams 10 and 5 so the shares are apportioned
 *      and team 1's share comes out above what it can use; the cap still
 *      hands the difference to team 2. */
int run_starts_side_quota_capped_by_eligible(void) {
    ServerSim *sim = ut_make_running_sim("SideQuota");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];

    /* Eight starts, 8 versus 4: the desired counts fit the starts. */
    begin_layout(gs);
    add_starts(gs, &k_column[K_COL_N_A], 4, true);
    add_starts(gs, &k_column[K_COL_S], 4, true);
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 8, 1);
    add_team(connected, team, 8, 4, 2);
    side[1] = START_SIDE_N;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);
    UT_ASSERT(check_north_team_and_any_team(gs, out, 8, 4, "8v4") == 0);

    /* Ten starts, 10 versus 5: team 1 may take six (four north, two
     * centre) and team 2 wants its five of the six it is allowed (four
     * south, two centre), so the desired counts exceed the starts and the
     * shares are apportioned. Team 1's share comes out at seven; capped to
     * six, the seventh goes to team 2, which then holds four real starts
     * instead of three. */
    begin_layout(gs);
    add_starts(gs, &k_column[K_COL_N_A], 4, true);
    add_starts(gs, &k_column[K_COL_S], 4, true);
    add_starts(gs, k_centre, 2, true);
    UT_ASSERT(startSideIsCentre(mask_of(gs, 8)));
    UT_ASSERT(startSideIsCentre(mask_of(gs, 9)));
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 10, 1);
    add_team(connected, team, 10, 5, 2);
    side[1] = START_SIDE_N;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);
    UT_ASSERT(check_north_team_and_any_team(gs, out, 10, 5, "10v5") == 0);
    serverSimDestroy(sim);
    return 0;
}

/* Team 1 (side N, slots 0..3) and team 2 (slots 4..15) on the corner
 * layout: team 1 holds four distinct north starts, team 2 never holds a
 * north start and at least four of its members ride a south start. */
static int check_four_v_twelve(GameSim *gs, const BYTE *out, const char *what) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(is_placed(gs, out[i]),
                      "%s: slot %d came back unplaced (%u)", what, i, (unsigned)out[i]);
    }
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(is_north(gs, out[i]),
                      "%s: team 1 (side N) slot %d landed on %u, not a north start",
                      what, i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, 0, 4) == 4,
                  "%s: team 1 should hold four distinct starts: %u %u %u %u", what,
                  (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3]);
    for (i = 4; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(!is_north(gs, out[i]),
                      "%s: team 2 slot %d landed on north start %u", what, i, (unsigned)out[i]);
        UT_ASSERT_MSG(is_south(gs, out[i]),
                      "%s: team 2 slot %d landed on %u, not a south start", what, i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_repeats(out, 4, 12) >= 4,
                  "%s: twelve players on eight south starts should share at least four times, saw %d",
                  what, count_repeats(out, 4, 12));
    return 0;
}

/* (11) The reported layout: sixteen starts in four corner clusters, team 1
 *      side N with four players, team 2 with twelve. With team 2 on no side
 *      it is kept off the north the humans chose: it fills the eight south
 *      starts and its last four ride them. With team 2 on side S the
 *      result is the same. */
int run_starts_side_any_team_kept_off_chosen_side(void) {
    ServerSim *sim = ut_make_running_sim("SideCorners");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, k_corners, 16, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 4, 1);
    add_team(connected, team, 4, 12, 2);
    side[1] = START_SIDE_N;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);
    UT_ASSERT(check_four_v_twelve(gs, out, "team 2 no side") == 0);

    side[2] = START_SIDE_S;
    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);
    UT_ASSERT(check_four_v_twelve(gs, out, "team 2 side S") == 0);
    serverSimDestroy(sim);
    return 0;
}

/* (12) A side has two corners and the team has two players: one goes to
 *      each. Both corners accept the side, so the side rules alone are
 *      happy to put the pair in whichever corner the start list names
 *      first — this is the placement that sent a whole north team into
 *      the north-west. */
int run_starts_side_spreads_across_corners(void) {
    ServerSim *sim = ut_make_running_sim("SideSpread");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, k_corners, 16, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    int i;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 2, 1);
    add_team(connected, team, 2, 2, 2);
    side[1] = START_SIDE_N;
    side[2] = START_SIDE_S;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    for (i = 0; i < 2; i++) {
        UT_ASSERT_MSG(is_north(gs, out[i]),
                      "team 1 (side N) slot %d landed on %u, not a north start",
                      i, (unsigned)out[i]);
    }
    for (i = 2; i < 4; i++) {
        UT_ASSERT_MSG(is_south(gs, out[i]),
                      "team 2 (side S) slot %d landed on %u, not a south start",
                      i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, 0, 4) == 4,
                  "four players should hold four distinct starts: %u %u %u %u",
                  (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3]);
    UT_ASSERT_MSG(count_west(gs, out, 0, 2) == 1,
                  "team 1 put %d of its 2 tanks in the west, expected one north corner each",
                  count_west(gs, out, 0, 2));
    UT_ASSERT_MSG(count_west(gs, out, 2, 2) == 1,
                  "team 2 put %d of its 2 tanks in the west, expected one south corner each",
                  count_west(gs, out, 2, 2));
    serverSimDestroy(sim);
    return 0;
}

/* (13) Four players on a side whose two corners hold four starts each:
 *      two to a corner, not three and one. The nearest claimed start ties
 *      for every start still free once both corners are open, so this is
 *      what the total-distance tie-break buys. */
int run_starts_side_spread_balances_corners(void) {
    ServerSim *sim = ut_make_running_sim("SideBalance");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, k_corners, 16, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    int i;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 4, 1);
    side[1] = START_SIDE_N;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(is_north(gs, out[i]),
                      "slot %d landed on %u, not a north start", i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, 0, 4) == 4,
                  "eight north starts for four players should give four distinct: %u %u %u %u",
                  (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3]);
    UT_ASSERT_MSG(count_west(gs, out, 0, 4) == 2,
                  "team put %d of its 4 tanks in the north-west, expected an even two and two",
                  count_west(gs, out, 0, 4));
    serverSimDestroy(sim);
    return 0;
}

/* (14) Spreading picks among equals, it never buys room by leaving the
 *      side. Four due-north starts within 50 squares of each other and two
 *      centre starts 85 squares south of them: the centre is by far the
 *      farthest thing from the first pick, and the second player must
 *      still take a north start. */
int run_starts_side_spread_keeps_its_tier(void) {
    ServerSim *sim = ut_make_running_sim("SideTier");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, &k_column[K_COL_N_A], 4, true);
    add_starts(gs, &k_column[K_COL_S], 4, true);
    add_starts(gs, k_centre, 2, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    int i;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 2, 1);
    side[1] = START_SIDE_N;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    for (i = 0; i < 2; i++) {
        UT_ASSERT_MSG(is_north(gs, out[i]),
                      "slot %d landed on %u, a centre or south start, not a north one",
                      i, (unsigned)out[i]);
        UT_ASSERT_MSG(!startSideIsCentre(mask_of(gs, out[i])),
                      "slot %d landed on centre start %u while north starts were free",
                      i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(out[0] != out[1],
                  "both slots took start %u", (unsigned)out[0]);
    serverSimDestroy(sim);
    return 0;
}

/* (15) Two teams both chose north and share the side's eight starts. Each
 *      is given a region of its own inside the side — one north corner
 *      each — and spreads across the four starts of it. Straddling both
 *      corners with both teams would spread each team more widely, but it
 *      would also leave the two of them interleaved, which is the thing a
 *      side is chosen to avoid: a team's own ground comes before spreading
 *      further over it. */
int run_starts_side_spread_two_teams_one_side(void) {
    ServerSim *sim = ut_make_running_sim("SideShared");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, k_corners, 16, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    int i;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 2, 1);
    add_team(connected, team, 2, 2, 2);
    side[1] = START_SIDE_N;
    side[2] = START_SIDE_N;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(is_north(gs, out[i]),
                      "slot %d landed on %u, not a north start", i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, 0, 4) == 4,
                  "two teams on one side should hold four distinct starts: %u %u %u %u",
                  (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3]);
    {
        int w1 = count_west(gs, out, 0, 2);
        int w2 = count_west(gs, out, 2, 2);
        UT_ASSERT_MSG(w1 == 0 || w1 == 2,
                      "team 1 straddles the two north corners (%d of 2 in the west)", w1);
        UT_ASSERT_MSG(w2 == 0 || w2 == 2,
                      "team 2 straddles the two north corners (%d of 2 in the west)", w2);
        UT_ASSERT_MSG(w1 != w2,
                      "both teams took the same north corner (%d and %d in the west)", w1, w2);
    }
    serverSimDestroy(sim);
    return 0;
}

/* (16) Only one team names a side. The other never chose one, but the
 *      choice made against it leaves it just the west, so it is on a side
 *      in everything but name and spreads over it the same way. Leaving it
 *      clustered was the first report back from a real game: the team that
 *      picked east split across the two east corners and the team that had
 *      picked nothing put both its players in one west corner. */
int run_starts_side_spread_unsided_team_confined(void) {
    ServerSim *sim = ut_make_running_sim("SideUnsided");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, k_corners, 16, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    int i;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 2, 1);
    add_team(connected, team, 2, 2, 2);
    side[1] = START_SIDE_N;          /* team 2 is left on no side at all */

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    for (i = 0; i < 2; i++) {
        UT_ASSERT_MSG(is_north(gs, out[i]),
                      "team 1 (side N) slot %d landed on %u, not a north start",
                      i, (unsigned)out[i]);
    }
    for (i = 2; i < 4; i++) {
        UT_ASSERT_MSG(!is_north(gs, out[i]),
                      "team 2 (no side) slot %d landed on north start %u, the side team's own",
                      i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, 0, 4) == 4,
                  "four players should hold four distinct starts: %u %u %u %u",
                  (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3]);
    UT_ASSERT_MSG(count_west(gs, out, 0, 2) == 1,
                  "team 1 put %d of its 2 tanks in the west, expected one north corner each",
                  count_west(gs, out, 0, 2));
    UT_ASSERT_MSG(count_west(gs, out, 2, 2) == 1,
                  "team 2 put %d of its 2 tanks in the west, expected one south corner each",
                  count_west(gs, out, 2, 2));
    serverSimDestroy(sim);
    return 0;
}

/* (17) A map whose starts ring the island, one team on the east and the
 *      other left alone. Being kept off the east is not on its own the
 *      other side of the map: everything but the east runs west, north and
 *      south and comes back to the east at both ends, so a team spread
 *      over the whole of that puts somebody on a due-north start next to
 *      the east team. The team with no side takes the far side of the map
 *      instead, so both of its players carry the west bit. */
int run_starts_side_unsided_team_takes_far_side(void) {
    ServerSim *sim = ut_make_running_sim("SideRing");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, k_ring, 16, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    int i;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 2, 1);
    add_team(connected, team, 2, 4, 2);
    side[1] = START_SIDE_E;          /* team 2 is left on no side at all */

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    for (i = 0; i < 2; i++) {
        UT_ASSERT_MSG((mask_of(gs, out[i]) & START_SIDE_BIT_E) != 0,
                      "team 1 (side E) slot %d landed on %u, not an east start",
                      i, (unsigned)out[i]);
    }
    /* Four players and six starts carrying the west bit (two W, two NW,
       two SW), so the whole team fits on the far side and none of it has
       to reach round the ring to the north or the south. */
    for (i = 2; i < 6; i++) {
        UT_ASSERT_MSG(is_west(gs, out[i]),
                      "team 2 (no side) slot %d landed on start %u, which is off the east but not on the west",
                      i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, 0, 6) == 6,
                  "six players should hold six distinct starts: %u %u %u %u %u %u",
                  (unsigned)out[0], (unsigned)out[1], (unsigned)out[2],
                  (unsigned)out[3], (unsigned)out[4], (unsigned)out[5]);
    serverSimDestroy(sim);
    return 0;
}

/* (18) Two teams both chose north on a map whose north row is narrow and
 *      whose centre band holds starts. The second team on the side anchors
 *      at the far end of the side, and that has to mean a north start: the
 *      side accepts the centre band too, and the centre is 85 squares from
 *      the north row's centroid where the row's own far end is 25, so a
 *      pick among everything the side accepts would anchor the second
 *      team in the middle of the map and place it there while north starts
 *      were free. Four north starts for four players, so nobody should
 *      reach the centre at all. */
int run_starts_side_shared_side_anchor_stays_on_side(void) {
    ServerSim *sim = ut_make_running_sim("SideSharedCentre");
    UT_ASSERT(sim != NULL);
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    begin_layout(gs);
    add_starts(gs, &k_column[K_COL_N_A], 4, true);
    add_starts(gs, &k_column[K_COL_S], 4, true);
    add_starts(gs, k_centre, 2, true);

    bool connected[MAX_TANKS];
    BYTE team[MAX_TANKS];
    BYTE reserved[MAX_TANKS];
    BYTE side[MAX_TANKS + 1];
    BYTE out[MAX_TANKS];
    int i;
    reset_inputs(connected, team, reserved, side);
    add_team(connected, team, 0, 2, 1);
    add_team(connected, team, 2, 2, 2);
    side[1] = START_SIDE_N;
    side[2] = START_SIDE_N;

    bolo_srand(11);
    startsAssignBatch(gs, &gs->ss, connected, team, out, NULL, side);

    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(is_north(gs, out[i]),
                      "slot %d landed on %u, not a north start", i, (unsigned)out[i]);
        UT_ASSERT_MSG(!startSideIsCentre(mask_of(gs, out[i])),
                      "slot %d landed on centre start %u while north starts were free",
                      i, (unsigned)out[i]);
    }
    UT_ASSERT_MSG(count_distinct(out, 0, 4) == 4,
                  "four north starts for four players should give four distinct: %u %u %u %u",
                  (unsigned)out[0], (unsigned)out[1], (unsigned)out[2], (unsigned)out[3]);
    serverSimDestroy(sim);
    return 0;
}
