/*
 * Which position a shell tests its target at (test_shell_rewind_rules.c).
 *
 * The server rewinds a target to where the shooter saw it. Three rules decide
 * how far back, and each case here pins one of them:
 *
 *  1. A pill's shell (owner NEUTRAL, lagCompTicks 0) is not rewound. It tests
 *     a tank where the tank is now. Here the tank's history holds nothing but
 *     a place two squares off, so a shell that rewound would hit there and
 *     miss the tank where it stands.
 *       shell_rewind_pill_shell_tank_present
 *
 *  2. A player's shell with lagCompTicks N tests a tank at its history entry
 *     N ticks ago, the full rewind.
 *       shell_rewind_player_shell_tank_full
 *
 *  3. The same shell tests a builder one tick less far back, at his history
 *     entry N-1, because the shooter's client draws builders from a snapshot
 *     one game tick newer than the one it draws tanks at
 *     (SHELL_LGM_REWIND_LESS, shells.c). The shell ends its life on the
 *     builder's N-1 entry and kills him, and on his N entry and does not.
 *       shell_rewind_player_shell_builder_one_less
 *
 * Every place is two map squares from every other, well past both the tank
 * hit circle and the builder's MAP_SQUARE_MIDDLE blast reach, so a hit or a
 * kill names the one position that was tested. The histories are seeded with
 * posHistoryRecord directly, so posHistoryGet with ticksAgo 0 is the newest
 * record, as it is after the server's own record loops each tick.
 *
 * The builder case goes through shellsUpdate's end-of-life branch, which
 * blows the shell up where it is. The collision branch makes the same
 * rewind choice in the same words.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, posHistory, lgmPosHistory */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "game_sim.h"
#include "bases.h"
#include "bolo_map.h"
#include "lgm.h"
#include "pillbox.h"
#include "position_history.h"
#include "shells.h"
#include "tank.h"
#include "everard_map.h"
#include "test_harness.h"

#define SRR_TARGET  0          /* the player shot at */
#define SRR_SHOOTER 1          /* the player whose shell it is */
#define SRR_RUN     7          /* clear squares needed in one row */
#define SRR_REWIND  3          /* lagCompTicks of the player's shell */

/* The middle of a map square in world units. */
static WORLD srrMiddle(BYTE m) {
    return (WORLD)(((WORLD)m << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

/* A two-player running game on Everard Island. */
static ServerSim *srrMakeSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimAddPlayer(sim, SRR_TARGET, "Target", false);
    serverSimAddPlayer(sim, SRR_SHOOTER, "Shooter", false);
    serverSimStartGame(sim);
    return sim;
}

/* A row of SRR_RUN grass squares with no pillbox or base on any of them, so
 * no shell test below meets a pillbox or solid ground before it meets the
 * tank or the builder. Its west end goes in *outX, *outY. */
static bool srrFindClearRow(ServerSim *sim, BYTE *outX, BYTE *outY) {
    GameSim *gs = &sim->sim;
    int x, y, i;

    for (y = 20; y < 236; y++) {
        for (x = 20; x < 236 - SRR_RUN; x++) {
            bool clear = true;
            for (i = 0; i < SRR_RUN && clear; i++) {
                BYTE mx = (BYTE)(x + i);
                BYTE my = (BYTE)y;
                if (mapGetPos(&gs->mp, mx, my) != GRASS ||
                    pillsExistPos(&gs->pb, mx, my) == TRUE ||
                    basesExistPos(&gs->bs, mx, my) == TRUE) {
                    clear = false;
                }
            }
            if (clear) {
                *outX = (BYTE)x;
                *outY = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* The tanks in play, compacted the way the tick compacts them. */
static BYTE srrArrays(ServerSim *sim, tank *tanks, lgm **lgms) {
    BYTE n = 0;
    BYTE c;

    for (c = 0; c < MAX_TANKS; c++) {
        if (sim->playerConnected[c] && sim->sim.tanks[c] != NULL) {
            tanks[n] = sim->sim.tanks[c];
            lgms[n]  = &sim->sim.lgmen[c];
            n++;
        }
    }
    return n;
}

/* Puts both tanks down. The target on (tx, ty); the shooter five squares
 * south, out of reach of every shell point in the row. */
static void srrPlaceTanks(ServerSim *sim, BYTE tx, BYTE ty, BYTE rowX, BYTE rowY) {
    tankSetLocationData(&sim->sim.tanks[SRR_TARGET], srrMiddle(tx), srrMiddle(ty),
                        0, 0, FALSE);
    tankSetLocationData(&sim->sim.tanks[SRR_SHOOTER],
                        srrMiddle((BYTE)(rowX + 3)), srrMiddle((BYTE)(rowY + 5)),
                        0, 0, FALSE);
}

/* Asks shellsCalcCollision about one shell of the given owner and rewind at
 * the middle of (mx, my). Returns the outcome it reports. */
static uint8_t srrTankShot(ServerSim *sim, BYTE owner, uint8_t lagCompTicks,
                           BYTE mx, BYTE my) {
    GameSim *gs = &sim->sim;
    tank tanks[MAX_TANKS];
    lgm *lgms[MAX_TANKS];
    BYTE n = srrArrays(sim, tanks, lgms);
    WORLD x = srrMiddle(mx);
    WORLD y = srrMiddle(my);
    uint8_t outcome = SHELL_OUTCOME_EXPIRED;

    gs->lagCompTicks = lagCompTicks;
    shellsAddItem(gs, &gs->shs, x, y, 0, 8, owner, NEUTRAL, DMG_NO_PILL, FALSE);
    gs->lagCompTicks = 0;
    if (gs->shs == NULL) return SHELL_OUTCOME_REJECTED;
    shellsCalcCollision(gs, tanks, &x, &y, gs->shs, n, &outcome);
    return outcome;
}

static bool srrIsTankOutcome(uint8_t outcome) {
    return outcome == SHELL_OUTCOME_TANK_HIT || outcome == SHELL_OUTCOME_TANK_KILL;
}

/* Rule 1: a pill's shell tests the tank where it is now. */
int run_shell_rewind_pill_shell_tank_present(void) {
    BYTE rowX = 0, rowY = 0;
    BYTE nowX, rewX;
    int i;
    uint8_t outcome;

    /* Case A: the shell lands on the tank where it stands. */
    ServerSim *sim = srrMakeSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(srrFindClearRow(sim, &rowX, &rowY),
                  "no row of %d clear grass squares on Everard Island", SRR_RUN);
    nowX = (BYTE)(rowX + 1);
    rewX = (BYTE)(rowX + 3);
    srrPlaceTanks(sim, nowX, rowY, rowX, rowY);
    /* Every history entry is two squares east of where the tank stands. */
    posHistoryInit(&sim->posHistory[SRR_TARGET]);
    for (i = 0; i < POSITION_HISTORY_SIZE; i++) {
        posHistoryRecord(&sim->posHistory[SRR_TARGET], srrMiddle(rewX),
                         srrMiddle(rowY), true);
    }
    outcome = srrTankShot(sim, NEUTRAL, 0, nowX, rowY);
    UT_ASSERT_MSG(srrIsTankOutcome(outcome),
                  "a pill's shell on the tank where it stands did not hit it "
                  "(outcome %u): it was tested somewhere else", (unsigned)outcome);
    serverSimDestroy(sim);

    /* Case B: the shell lands where the history says the tank was. */
    sim = srrMakeSim();
    UT_ASSERT(sim != NULL);
    srrPlaceTanks(sim, nowX, rowY, rowX, rowY);
    posHistoryInit(&sim->posHistory[SRR_TARGET]);
    for (i = 0; i < POSITION_HISTORY_SIZE; i++) {
        posHistoryRecord(&sim->posHistory[SRR_TARGET], srrMiddle(rewX),
                         srrMiddle(rowY), true);
    }
    outcome = srrTankShot(sim, NEUTRAL, 0, rewX, rowY);
    UT_ASSERT_MSG(!srrIsTankOutcome(outcome),
                  "a pill's shell hit the tank at a place in its history "
                  "(outcome %u): pill shells must not be rewound",
                  (unsigned)outcome);
    serverSimDestroy(sim);
    return 0;
}

/* Rule 2: a player's shell with lagCompTicks N tests the tank N ticks ago. */
int run_shell_rewind_player_shell_tank_full(void) {
    BYTE rowX = 0, rowY = 0;
    BYTE nowX, rewX;
    int i;
    uint8_t outcome;
    ServerSim *sim;

    for (int which = 0; which < 2; which++) {
        sim = srrMakeSim();
        UT_ASSERT(sim != NULL);
        UT_ASSERT_MSG(srrFindClearRow(sim, &rowX, &rowY),
                      "no row of %d clear grass squares on Everard Island",
                      SRR_RUN);
        nowX = (BYTE)(rowX + 1);
        rewX = (BYTE)(rowX + 3);
        srrPlaceTanks(sim, nowX, rowY, rowX, rowY);
        /* Entry N ago is two squares east; entries N-1 .. 0 are where the
         * tank stands. A rewind of N-1 would find the tank where it stands. */
        posHistoryInit(&sim->posHistory[SRR_TARGET]);
        posHistoryRecord(&sim->posHistory[SRR_TARGET], srrMiddle(rewX),
                         srrMiddle(rowY), true);
        for (i = 0; i < SRR_REWIND; i++) {
            posHistoryRecord(&sim->posHistory[SRR_TARGET], srrMiddle(nowX),
                             srrMiddle(rowY), true);
        }
        if (which == 0) {
            outcome = srrTankShot(sim, SRR_SHOOTER, SRR_REWIND, rewX, rowY);
            UT_ASSERT_MSG(srrIsTankOutcome(outcome),
                          "a shell with rewind %d did not hit the tank at its "
                          "entry %d ticks ago (outcome %u)", SRR_REWIND,
                          SRR_REWIND, (unsigned)outcome);
        } else {
            outcome = srrTankShot(sim, SRR_SHOOTER, SRR_REWIND, nowX, rowY);
            UT_ASSERT_MSG(!srrIsTankOutcome(outcome),
                          "a shell with rewind %d hit the tank where it stands "
                          "now (outcome %u): the tank test must use the full "
                          "rewind", SRR_REWIND, (unsigned)outcome);
        }
        serverSimDestroy(sim);
    }
    return 0;
}

/* Rule 3: the same shell tests a builder at his entry N-1. */
int run_shell_rewind_player_shell_builder_one_less(void) {
    BYTE rowX = 0, rowY = 0;
    BYTE farX, nearX, nowX;
    int i;
    ServerSim *sim;

    for (int which = 0; which < 2; which++) {
        GameSim *gs;
        lgm *man;
        tank tanks[MAX_TANKS];
        lgm *lgms[MAX_TANKS];
        BYTE n;
        BYTE blastX;

        sim = srrMakeSim();
        UT_ASSERT(sim != NULL);
        gs = &sim->sim;
        UT_ASSERT_MSG(srrFindClearRow(sim, &rowX, &rowY),
                      "no row of %d clear grass squares on Everard Island",
                      SRR_RUN);
        farX  = (BYTE)(rowX + 1);   /* his entry N ago */
        nearX = (BYTE)(rowX + 3);   /* his entry N-1 ago */
        nowX  = (BYTE)(rowX + 5);   /* where he stands, entries N-2 .. 0 */
        srrPlaceTanks(sim, nowX, (BYTE)(rowY + 5), rowX, (BYTE)(rowY + 5));
        tankSetLocationData(&gs->tanks[SRR_SHOOTER],
                            srrMiddle((BYTE)(rowX + 3)),
                            srrMiddle((BYTE)(rowY + 10)), 0, 0, FALSE);

        man = &gs->lgmen[SRR_TARGET];
        UT_ASSERT_MSG(*man != NULL, "the target has no builder");
        (*man)->inTank = FALSE;
        (*man)->isDead = FALSE;
        (*man)->x      = srrMiddle(nowX);
        (*man)->y      = srrMiddle(rowY);

        posHistoryInit(&sim->lgmPosHistory[SRR_TARGET]);
        posHistoryRecord(&sim->lgmPosHistory[SRR_TARGET], srrMiddle(farX),
                         srrMiddle(rowY), true);
        posHistoryRecord(&sim->lgmPosHistory[SRR_TARGET], srrMiddle(nearX),
                         srrMiddle(rowY), true);
        for (i = 0; i < SRR_REWIND - 1; i++) {
            posHistoryRecord(&sim->lgmPosHistory[SRR_TARGET], srrMiddle(nowX),
                             srrMiddle(rowY), true);
        }

        blastX = (which == 0) ? nearX : farX;
        gs->lagCompTicks = SRR_REWIND;
        shellsAddItem(gs, &gs->shs, srrMiddle(blastX), srrMiddle(rowY), 0, 8,
                      SRR_SHOOTER, NEUTRAL, DMG_NO_PILL, FALSE);
        gs->lagCompTicks = 0;
        UT_ASSERT_MSG(gs->shs != NULL, "the shell was not added");
        UT_ASSERT_MSG(gs->shs->compensationTicks == SRR_REWIND,
                      "the shell carries rewind %u, expected %d",
                      (unsigned)gs->shs->compensationTicks, SRR_REWIND);
        /* End its life where it is, so it blows up on (blastX, rowY). */
        gs->shs->x = srrMiddle(blastX);
        gs->shs->y = srrMiddle(rowY);
        gs->shs->length = SHELL_DEATH;

        n = srrArrays(sim, tanks, lgms);
        shellsUpdate(gs, tanks, n, lgms, &gs->ss);

        if (which == 0) {
            UT_ASSERT_MSG((*man)->isDead == TRUE,
                          "a shell with rewind %d blowing up on the builder's "
                          "entry %d ticks ago did not kill him: builders are "
                          "rewound one tick less than tanks", SRR_REWIND,
                          SRR_REWIND - 1);
        } else {
            UT_ASSERT_MSG((*man)->isDead == FALSE,
                          "a shell with rewind %d blowing up on the builder's "
                          "entry %d ticks ago killed him: that is the tanks' "
                          "rewind, builders get one tick less", SRR_REWIND,
                          SRR_REWIND);
        }
        serverSimDestroy(sim);
    }
    return 0;
}
