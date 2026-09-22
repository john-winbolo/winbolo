/*
 * Team start sides from the lobby to the spawned tanks
 * (test_starts_side_integration.c).
 *
 * The lobby reservations picked on join and team change, the per-team
 * side table handed to startsAssignBatch at game start, and the
 * tank-aware spawn scatter together put every tank of a north team in the
 * northern half of the start bounding box and every tank of a south team
 * in the southern half, on distinct squares, even when the south team has
 * more players than starts. The game starts through
 * serverSimStartGameInPlace: the full serverSimStartGame reloads the
 * starts from the cached map and would discard the injected layout.
 *
 *   (21) sixteen corner starts, four players north against twelve south,
 *        with the sides set before the players join and again after.
 *   (23) eight starts in four corners, a team that named no side kept off
 *        the side another team named, its last member included.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"              /* mapSetPos */
#include "bolo_rand.h"             /* bolo_srand */
#include "everard_map.h"
#include "starts.h"                /* startsGetMaxs */
#include "start_sides.h"           /* START_SIDE_* */
#include "tank.h"                  /* tankGetWorld */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.ss, sim->sim.tanks */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, serverSimSetTeam,
                                    * serverSimSetTeamMeta, serverSimStartGameInPlace */
#include "test_harness.h"

typedef struct {
    BYTE x;
    BYTE y;
} LayoutStart;

/* Four clusters of four, one per corner, every start a diagonal. bbox
 * 40..210 on both axes, so the halves split at y=125. */
static const LayoutStart k_corners[16] = {
    {  40,  40 }, {  50,  40 }, {  40,  50 }, {  50,  50 },
    { 200,  40 }, { 210,  40 }, { 200,  50 }, { 210,  50 },
    {  40, 200 }, {  50, 200 }, {  40, 210 }, {  50, 210 },
    { 200, 200 }, { 210, 200 }, { 200, 210 }, { 210, 210 },
};

/* Lobby-enabled ServerSim on Everard Island with the corner layout in
 * place of the map's own starts, every start square deep sea and the
 * map's pills and bases cleared so only the layout drives placement. */
static ServerSim *make_corner_lobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    GameSim *gs;
    int i;
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    gs = &sim->sim;
    gs->pb->numPills = 0;
    gs->bs->numBases = 0;
    gs->ss->numStarts = 0;
    for (i = 0; i < 16; i++) {
        BYTE idx = gs->ss->numStarts++;
        mapSetPos(gs, &gs->mp, k_corners[i].x, k_corners[i].y, DEEP_SEA, FALSE, TRUE);
        gs->ss->item[idx].x = k_corners[i].x;
        gs->ss->item[idx].y = k_corners[i].y;
        gs->ss->item[idx].dir = 0;
    }
    return sim;
}

/* Slots 0..3 on team 1, slots 4..15 on team 2. */
static void add_players(ServerSim *sim) {
    BYTE s;
    for (s = 0; s < MAX_TANKS; s++) {
        char name[16];
        snprintf(name, sizeof(name), "P%u", (unsigned)s);
        serverSimAddPlayer(sim, s, name, false);
        serverSimSetTeam(sim, s, (BYTE)(s < 4 ? 1 : 2));
    }
}

static void set_sides(ServerSim *sim) {
    serverSimSetTeamMeta(sim, 1, 0, 0, START_SIDE_N, NULL, 0);
    serverSimSetTeamMeta(sim, 2, 0, 0, START_SIDE_S, NULL, 0);
}

/* Map square a live tank sits on. */
static void tank_square(GameSim *gs, BYTE slot, int *mx, int *my) {
    WORLD wx;
    WORLD wy;
    tankGetWorld(&gs->tanks[slot], &wx, &wy);
    *mx = (int)(wx >> M_W_SHIFT_SIZE);
    *my = (int)(wy >> M_W_SHIFT_SIZE);
}

/* How many of the slots first..first+count-1 sit west of midX. */
static int count_west(const int *mx, int first, int count, int midX) {
    int west = 0;
    int i;
    for (i = first; i < first + count; i++) {
        if (mx[i] < midX) west++;
    }
    return west;
}

/* Every team-1 tank north of the bbox midline, every team-2 tank south
 * of it, all sixteen on distinct squares — and each team spread across
 * both of its side's corners rather than piled into one of them. */
static int check_split(ServerSim *sim, const char *what) {
    GameSim *gs = &sim->sim;
    int leftPos;
    int rightPos;
    int topPos;
    int bottomPos;
    int midY;
    int midX;
    int west;
    int mx[MAX_TANKS];
    int my[MAX_TANKS];
    int i;
    int j;

    startsGetMaxs(&gs->ss, &leftPos, &rightPos, &topPos, &bottomPos);
    midY = (topPos + bottomPos) / 2;
    midX = (leftPos + rightPos) / 2;

    for (i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(gs->tanks[i] != NULL, "%s: slot %d has no tank", what, i);
        tank_square(gs, (BYTE)i, &mx[i], &my[i]);
    }
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(my[i] < midY,
                      "%s: team 1 (north) slot %d spawned at (%d,%d), below the midline %d",
                      what, i, mx[i], my[i], midY);
    }
    for (i = 4; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(my[i] > midY,
                      "%s: team 2 (south) slot %d spawned at (%d,%d), above the midline %d",
                      what, i, mx[i], my[i], midY);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        for (j = 0; j < i; j++) {
            UT_ASSERT_MSG(mx[i] != mx[j] || my[i] != my[j],
                          "%s: slots %d and %d both spawned at (%d,%d)",
                          what, j, i, mx[i], my[i]);
        }
    }

    /* Team 1 has four players and eight north starts, four in each north
       corner, so its side can hold every one of them: two go north-west
       and two north-east. Piling all four into one corner is the bug this
       pins — the side rules alone are happy with it. */
    west = count_west(mx, 0, 4, midX);
    UT_ASSERT_MSG(west == 2,
                  "%s: team 1 (north) put %d of its 4 tanks west of %d, expected 2 in each north corner",
                  what, west, midX);

    /* Team 2 has twelve players for eight south starts, so four of them
       ride a start a team-mate already holds. However the riders land, the
       team still has to use both south corners; an even 6/6 is what the
       spread and the rider balance give, but the corners only have to be
       used, not matched exactly. */
    west = count_west(mx, 4, 12, midX);
    UT_ASSERT_MSG(west >= 4 && west <= 8,
                  "%s: team 2 (south) put %d of its 12 tanks west of %d, expected both south corners used",
                  what, west, midX);
    return 0;
}

/* (21) Four north against twelve south on sixteen corner starts, started
 *      in place. First with the sides set before anyone joins, then with
 *      the sides set after every player is on a team. */
int run_starts_side_end_to_end_four_v_twelve(void) {
    ServerSim *sim;

    sim = make_corner_lobby();
    UT_ASSERT(sim != NULL);
    set_sides(sim);
    add_players(sim);
    bolo_srand(11);
    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT(check_split(sim, "sides first") == 0);
    serverSimDestroy(sim);

    sim = make_corner_lobby();
    UT_ASSERT(sim != NULL);
    add_players(sim);
    set_sides(sim);
    bolo_srand(11);
    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT(check_split(sim, "sides after") == 0);
    serverSimDestroy(sim);
    return 0;
}

/* (22) A sweep over map shapes and team line-ups, asserting a property
 *      rather than checking an example: every player should sit nearer its
 *      own team's ground than any other team's. Two bugs reached a real
 *      game before this existed, both because a team's region was worked
 *      out negatively — "any start not on a side someone else chose" — and
 *      the shape of a negative region depends on the shape of the map. Seven
 *      shapes here and twelve line-ups, so a new map shape is covered
 *      before somebody plays it rather than after. */
typedef struct { const char *name; int n; BYTE x[24]; BYTE y[24]; } SweepLayout;

static const SweepLayout k_sweep[] = {
  { "corners4x4", 16,
    { 40, 50, 40, 50,200,210,200,210, 40, 50, 40, 50,200,210,200,210 },
    { 40, 40, 50, 50, 40, 40, 50, 50,200,200,210,210,200,200,210,210 } },
  { "ring16", 16,
    { 76, 76, 92,108,124,140,156,172,184,184,172,156,140,124,108, 92 },
    {140,124,100,100,100,100,100,100,124,140,156,156,156,156,156,156 } },
  { "column", 12,
    {100,115,135,150,100,115,135,150,100,115,135,150 },
    { 40, 40, 40, 40,210,210,210,210, 55, 55, 55, 55 } },
  { "lopsided", 12,
    { 40, 45, 50, 40, 45, 50, 40, 45, 50,205,210,207 },
    { 40, 45, 50,120,125,130,200,205,210,120,125,130 } },
  { "sparse4", 4, { 40,210, 40,210 }, { 40, 40,210,210 } },
  { "threeclusters", 12,
    { 40, 50, 45,200,210,205,120,130,125,115,135,125 },
    { 40, 40, 50, 40, 40, 50,200,200,210,205,205,215 } },
  { "diagonalband", 10,
    { 40, 60, 80,100,120,140,160,180,200,210 },
    { 40, 60, 80,100,120,140,160,180,200,210 } },
};
#define K_SWEEP_N ((int)(sizeof(k_sweep) / sizeof(k_sweep[0])))

typedef struct { const char *name; int teams; int size[4]; BYTE side[4]; } SweepCfg;

static const SweepCfg k_cfg[] = {
  { "2x2 one named E",     2, {2,2,0,0}, { START_SIDE_E, START_SIDE_ANY } },
  { "2x2 one named N",     2, {2,2,0,0}, { START_SIDE_N, START_SIDE_ANY } },
  { "2x2 one named W",     2, {2,2,0,0}, { START_SIDE_W, START_SIDE_ANY } },
  { "2x2 one named S",     2, {2,2,0,0}, { START_SIDE_S, START_SIDE_ANY } },
  { "2x2 both named EW",   2, {2,2,0,0}, { START_SIDE_E, START_SIDE_W } },
  { "2x2 both named NS",   2, {2,2,0,0}, { START_SIDE_N, START_SIDE_S } },
  { "2x2 neither named",   2, {2,2,0,0}, { START_SIDE_ANY, START_SIDE_ANY } },
  { "4v2 one named E",     2, {4,2,0,0}, { START_SIDE_E, START_SIDE_ANY } },
  { "3 teams one named E", 3, {2,2,2,0}, { START_SIDE_E, START_SIDE_ANY, START_SIDE_ANY } },
  { "4 teams one named N", 4, {2,2,2,2}, { START_SIDE_N, START_SIDE_ANY, START_SIDE_ANY, START_SIDE_ANY } },
  { "2x2 same side E",     2, {2,2,0,0}, { START_SIDE_E, START_SIDE_E } },
  { "2x3 one named E",     2, {3,3,0,0}, { START_SIDE_E, START_SIDE_ANY } },
};
#define K_CFG_N ((int)(sizeof(k_cfg) / sizeof(k_cfg[0])))

static int cheb(int ax, int ay, int bx, int by) {
    int dx = ax > bx ? ax - bx : bx - ax;
    int dy = ay > by ? ay - by : by - ay;
    return dx > dy ? dx : dy;
}

int run_starts_side_region_sweep(void) {
    int li, ci;
    int problems = 0;
    for (li = 0; li < K_SWEEP_N; li++) {
        for (ci = 0; ci < K_CFG_N; ci++) {
            BYTE emap[6000] = E_MAP;
            ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                                       gameOpen, false, 0, -1);
            GameSim *gs;
            int i, t, slot, k;
            int px[MAX_TANKS], py[MAX_TANKS], pteam[MAX_TANKS], np = 0;
            int cx[4], cy[4], cn[4], roomless[4];
            /* Three teams of two on six starts in a straight line, which is
               what this layout leaves the teams that named no side. Each
               team's region is big enough, but spreading a team over its
               own region opens a gap in the middle of it that the next
               team fills, so the three interleave. Spreading a team across
               its ground is the point of the feature and contiguity loses
               to it here; regions that balanced their size against the
               team's would settle it, and nothing on a real map has needed
               that yet. */
            bool crowded = (strcmp(k_sweep[li].name, "diagonalband") == 0 &&
                            k_cfg[ci].teams == 4);
            if (sim == NULL) continue;
            serverSimSetLobbyEnabled(sim, true);
            gs = &sim->sim;
            gs->pb->numPills = 0;
            gs->bs->numBases = 0;
            gs->ss->numStarts = 0;
            for (i = 0; i < k_sweep[li].n; i++) {
                mapSetPos(gs, &gs->mp, k_sweep[li].x[i], k_sweep[li].y[i], DEEP_SEA, FALSE, TRUE);
                gs->ss->item[i].x = k_sweep[li].x[i];
                gs->ss->item[i].y = k_sweep[li].y[i];
                gs->ss->item[i].dir = 0;
            }
            gs->ss->numStarts = (BYTE)k_sweep[li].n;
            slot = 0;
            for (t = 0; t < k_cfg[ci].teams; t++) {
                for (k = 0; k < k_cfg[ci].size[t]; k++) {
                    char nm[16];
                    snprintf(nm, sizeof(nm), "P%d", slot);
                    serverSimAddPlayer(sim, (BYTE)slot, nm, false);
                    serverSimSetTeam(sim, (BYTE)slot, (BYTE)(t + 1));
                    slot++;
                }
            }
            for (t = 0; t < k_cfg[ci].teams; t++) {
                if (k_cfg[ci].side[t] != START_SIDE_ANY) {
                    serverSimSetTeamMeta(sim, (BYTE)(t + 1), 0, 0, k_cfg[ci].side[t], NULL, 0);
                }
            }
            for (i = 0; i < slot; i++) {
                BYTE r = sim->lobbyPlayers[i].startIdx;
                if (r == 0xFF) continue;
                px[np] = gs->ss->item[r - 1].x;
                py[np] = gs->ss->item[r - 1].y;
                pteam[np] = sim->lobbyPlayers[i].teamNumber;
                np++;
            }
            for (t = 0; t < 4; t++) { cx[t] = 0; cy[t] = 0; cn[t] = 0; }
            for (i = 0; i < np; i++) {
                int g = pteam[i] - 1;
                cx[g] += px[i]; cy[g] += py[i]; cn[g]++;
            }
            for (t = 0; t < 4; t++) if (cn[t] > 0) { cx[t] /= cn[t]; cy[t] /= cn[t]; }
            /* A region with more players competing for it than it has
               starts cannot give each team ground of its own, so the
               separation below is not asked of it. Teams sharing an
               eligible set compete for it; count them against its size. */
            {
                BYTE sm[MAX_STARTS];
                int el[4];
                int comp[4];
                int closed = 0;
                int leftPos, rightPos, topPos, bottomPos;
                startsGetMaxs(&gs->ss, &leftPos, &rightPos, &topPos, &bottomPos);
                for (i = 0; i < k_sweep[li].n; i++) {
                    sm[i] = startSideMaskFor(gs->ss->item[i].x, gs->ss->item[i].y,
                                             leftPos, topPos, rightPos, bottomPos);
                }
                for (t = 0; t < k_cfg[ci].teams; t++) {
                    closed |= startSideBits(k_cfg[ci].side[t]);
                }
                for (t = 0; t < k_cfg[ci].teams; t++) {
                    BYTE mine = k_cfg[ci].side[t];
                    BYTE other = (BYTE)(closed & ~startSideBits(mine));
                    el[t] = 0;
                    for (i = 0; i < k_sweep[li].n; i++) {
                        if (startSideEligible(sm[i], mine, other)) el[t]++;
                    }
                }
                for (t = 0; t < k_cfg[ci].teams; t++) {
                    int u;
                    comp[t] = 0;
                    for (u = 0; u < k_cfg[ci].teams; u++) {
                        if (el[u] == el[t] && k_cfg[ci].side[u] == k_cfg[ci].side[t]) {
                            comp[t] += k_cfg[ci].size[u];
                        }
                    }
                }
                for (t = 0; t < k_cfg[ci].teams; t++) {
                    if (comp[t] > el[t]) roomless[t] = 1; else roomless[t] = 0;
                }
            }
            /* Separation: every player nearer its own team's centroid than
               any other team's. A player standing in the enemy's area fails. */
            for (i = 0; i < np; i++) {
                int own = pteam[i] - 1;
                int dOwn;
                if (roomless[own] || crowded) continue;
                dOwn = cheb(px[i], py[i], cx[own], cy[own]);
                for (t = 0; t < 4; t++) {
                    if (t == own || cn[t] == 0) continue;
                    if (cheb(px[i], py[i], cx[t], cy[t]) < dOwn) {
                        printf("  %-14s %-22s team %d at (%d,%d) is nearer team %d\n",
                               k_sweep[li].name, k_cfg[ci].name, own + 1, px[i], py[i], t + 1);
                        problems++;
                        t = 4;
                    }
                }
            }
            serverSimDestroy(sim);
        }
    }
    UT_ASSERT_MSG(problems == 0,
                  "%d player(s) were placed nearer another team's ground than their own",
                  problems);
    return 0;
}

/* Eight starts, two to a corner — the shape of Baringi, the map this was
   found on. The bbox is 100..150 on both axes, so the halves split at
   y = 125 and the corners sit well clear of it. */
static const LayoutStart k_eight[8] = {
    { 104, 104 }, { 105, 103 },     /* north-west */
    { 149, 104 }, { 150, 105 },     /* north-east */
    { 149, 149 }, { 148, 150 },     /* south-east */
    { 104, 149 }, { 103, 148 },     /* south-west */
};

/* Everard with the eight-start layout in place of its own, its pills and
   bases cleared, every start square deep sea. */
static ServerSim *make_eight_start_lobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    GameSim *gs;
    int i;
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    gs = &sim->sim;
    gs->pb->numPills = 0;
    gs->bs->numBases = 0;
    gs->ss->numStarts = 0;
    for (i = 0; i < 8; i++) {
        BYTE idx = gs->ss->numStarts++;
        mapSetPos(gs, &gs->mp, k_eight[i].x, k_eight[i].y, DEEP_SEA, FALSE, TRUE);
        gs->ss->item[idx].x = k_eight[i].x;
        gs->ss->item[idx].y = k_eight[i].y;
        gs->ss->item[idx].dir = 0;
    }
    return sim;
}

/* Every tank of a team that named no side sits south of the midline — the
   half left to it once another team has taken the north. */
static int check_unsided_stays_south(ServerSim *sim, int firstSlot, int lastSlot,
                                     const char *what) {
    GameSim *gs = &sim->sim;
    int leftPos;
    int rightPos;
    int topPos;
    int bottomPos;
    int midY;
    int i;

    startsGetMaxs(&gs->ss, &leftPos, &rightPos, &topPos, &bottomPos);
    midY = (topPos + bottomPos) / 2;
    for (i = firstSlot; i <= lastSlot; i++) {
        int mx;
        int my;
        UT_ASSERT_MSG(gs->tanks[i] != NULL, "%s: slot %d has no tank", what, i);
        tank_square(gs, (BYTE)i, &mx, &my);
        UT_ASSERT_MSG(my > midY,
                      "%s: slot %d (team %u, no side) spawned at (%d,%d), north of "
                      "the midline %d — the side team 1 chose",
                      what, i, (unsigned)sim->lobbyPlayers[i].teamNumber,
                      mx, my, midY);
    }
    return 0;
}

/* (23) A team that named no side is kept off the side another team named,
 *      including its last member, the one with no start of its own.
 *
 *      Found in a single-player game on Baringi: three players took north
 *      and five were left on Any, which put four of them on the four
 *      southern starts and sent the fifth to sea. The batch works a
 *      side-less team's ground out as "every start off the sides the other
 *      teams chose", then re-checks that the team has somewhere to go — and
 *      that check only counted starts no reservation held. Every southern
 *      start was held by that team's own members, so it read as shut out,
 *      every side was opened to it, and the claim pass handed its last
 *      member the one start still free, which was northern. It spawned in
 *      the middle of the other team.
 *
 *      Riding is the answer for a member with no start of its own, and a
 *      rider takes a start its own team holds, which is southern. Two
 *      shapes here: two teams, and three teams where the two side-less ones
 *      have to share what is left. The three-team shape is the one no
 *      "give the other team the opposite side" rule in the lobby can cover.
 */
int run_starts_side_unsided_team_kept_off_chosen_side(void) {
    ServerSim *sim;
    BYTE s;

    /* Two teams: three north, five with no side and only four starts. */
    sim = make_eight_start_lobby();
    UT_ASSERT(sim != NULL);
    for (s = 0; s < 8; s++) {
        char name[16];
        snprintf(name, sizeof(name), "P%u", (unsigned)s);
        serverSimAddPlayer(sim, s, name, false);
        serverSimSetTeam(sim, s, (BYTE)(s < 3 ? 1 : 2));
    }
    serverSimSetTeamMeta(sim, 1, 0, 0, START_SIDE_N, NULL, 0);
    /* Team 2 names no side at all, which is the state every team starts in. */
    bolo_srand(11);
    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT(check_unsided_stays_south(sim, 3, 7, "two teams") == 0);
    serverSimDestroy(sim);

    /* Three teams: three north, then three and two with no side between
       them. The fifth side-less player again has no start of its own. */
    sim = make_eight_start_lobby();
    UT_ASSERT(sim != NULL);
    for (s = 0; s < 8; s++) {
        char name[16];
        BYTE team = (BYTE)(s < 3 ? 1 : (s < 6 ? 2 : 3));
        snprintf(name, sizeof(name), "P%u", (unsigned)s);
        serverSimAddPlayer(sim, s, name, false);
        serverSimSetTeam(sim, s, team);
    }
    serverSimSetTeamMeta(sim, 1, 0, 0, START_SIDE_N, NULL, 0);
    bolo_srand(11);
    serverSimStartGameInPlace(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT(check_unsided_stays_south(sim, 3, 7, "three teams") == 0);
    serverSimDestroy(sim);
    return 0;
}
