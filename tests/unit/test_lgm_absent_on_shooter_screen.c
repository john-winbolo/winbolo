/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A player's shell against a builder: the shooter's screen decides.
 *
 * When a shell explodes, shellsUpdate (shells.c) tests every builder against
 * the blast through shellsBlastTestBuilder. A player's shell tests the
 * builder where he was on the shooter's screen, which is the builder
 * position history a number of frames back (the shell's rewind, one frame
 * less against a builder when the rewind is more than one). The history
 * records one entry a frame, and an entry says "not out" when the builder
 * was inside his tank or dead on that frame.
 *
 * position_history_lookup         — posHistoryLookup tells "no record that
 *                                   old" from "the record says absent" from
 *                                   "the record has a position", also after
 *                                   the 16 entries have wrapped round, and
 *                                   posHistoryGet still says TRUE only for
 *                                   the last of the three.
 * lgm_kill_absent_on_shooter_screen — the builder was inside the tank on the
 *                                   frame the shooter saw and has stepped out
 *                                   since. A player's shell that explodes on
 *                                   top of him does not kill him. Before this
 *                                   rule the shell fell back to where he is
 *                                   now and killed him: with the "absent"
 *                                   branch of shellsBlastTestBuilder turned
 *                                   back into a lgmDeathCheck call, the two
 *                                   "must live" checks here fail.
 * lgm_kill_rewound_position       — out on both frames: he is tested where
 *                                   he was. A blast at the old place kills, a
 *                                   blast only at the new place does not.
 * lgm_kill_pill_shell_present     — a pill's shell carries no rewind and
 *                                   kills a builder who has just left the
 *                                   tank where he is now.
 * lgm_kill_short_history          — fewer records than the rewind: he is
 *                                   tested where he is now, as before. One
 *                                   record more, and that record saying he
 *                                   was in the tank, and he lives.
 *
 * Every builder case runs at both places a shell explodes: the end of its
 * life (the shell is given no life left and explodes where it is), and a
 * collision (the shell is put 8 world units west of the builder's own tank,
 * moving east, and hits it; the blast is on the tank's centre and the
 * builder is 16 away, well inside the half square that kills).
 *
 * The game is not ticked. The builder is sent out with a real harvest order,
 * which puts him 16 world units from the tank, and the history is written
 * directly, oldest frame first, the way serverSimTick's recording loop
 * writes it (the newest record is the present frame, because that loop runs
 * before shellsUpdate in the same tick).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, posHistory, lgmPosHistory */
#include "server_sim_lifecycle.h"
#include "game_sim.h"              /* GameSim: mp/bs/pb/shs, tanks[], lgmen[] */
#include "position_history.h"
#include "shells.h"                /* shellsAddItem, shellsUpdate, SHELL_DEATH */
#include "lgm.h"                   /* lgmNewPrimaryRequest, struct lgmObj */
#include "bolo_map.h"              /* mapSetPos and the terrain constants */
#include "pillbox.h"               /* pillsExistPos */
#include "bases.h"                 /* basesExistPos */
#include "mines.h"                 /* minesExistPos */
#include "tank.h"                  /* tankSetWorld, tankSetOnBoat */
#include "everard_map.h"
#include "test_harness.h"

#define LAS_BUILDER 0    /* the slot whose builder is shot at */
#define LAS_SHOOTER 1    /* the slot whose shells they are */
#define LAS_FAR     6    /* squares from the tank to the harvest square */
#define LAS_OLD     2    /* squares from the tank to the "old place" */
#define LAS_REWIND  5    /* frames back the shooter's screen is */
/* A shell's rewind against a builder is one less than the shell carries. */
#define LAS_COMP    (LAS_REWIND + 1)

typedef enum {
    LAS_SITE_EXPIRY = 0,   /* the shell runs out of life and explodes */
    LAS_SITE_COLLISION     /* the shell hits the builder's tank */
} LasSite;

typedef struct {
    ServerSim *sim;
    GameSim   *gs;
    BYTE  cx, cy;          /* the tank's square */
    WORLD midX, midY;      /* its centre: where the tank is */
    WORLD oldX, oldY;      /* the centre of the square LAS_OLD to the east */
} LasFix;

/* ── posHistoryLookup ─────────────────────────────────────────────────── */

int run_position_history_lookup(void) {
    PosHistory h;
    WORLD x, y;
    int i;

    /* Nothing recorded: no record at any age, and the outputs are left. */
    posHistoryInit(&h);
    x = 7; y = 9;
    UT_ASSERT(posHistoryLookup(&h, 0, &x, &y) == POS_HISTORY_NO_RECORD);
    UT_ASSERT(posHistoryLookup(&h, 5, &x, &y) == POS_HISTORY_NO_RECORD);
    UT_ASSERT(posHistoryGet(&h, 0, &x, &y) == FALSE);
    UT_ASSERT(x == 7 && y == 9);

    /* One record with a position. */
    posHistoryRecord(&h, 10, 20, TRUE);
    UT_ASSERT(posHistoryLookup(&h, 0, &x, &y) == POS_HISTORY_PRESENT);
    UT_ASSERT(x == 10 && y == 20);
    UT_ASSERT(posHistoryLookup(&h, 1, &x, &y) == POS_HISTORY_NO_RECORD);

    /* A newer record that says absent. Its own x and y are not handed out. */
    posHistoryRecord(&h, 30, 40, FALSE);
    x = 7; y = 9;
    UT_ASSERT(posHistoryLookup(&h, 0, &x, &y) == POS_HISTORY_ABSENT);
    UT_ASSERT(x == 7 && y == 9);
    UT_ASSERT(posHistoryGet(&h, 0, &x, &y) == FALSE);
    UT_ASSERT(x == 7 && y == 9);
    UT_ASSERT(posHistoryLookup(&h, 1, &x, &y) == POS_HISTORY_PRESENT);
    UT_ASSERT(x == 10 && y == 20);
    UT_ASSERT(posHistoryGet(&h, 1, &x, &y) == TRUE);
    UT_ASSERT(posHistoryLookup(&h, 2, &x, &y) == POS_HISTORY_NO_RECORD);
    UT_ASSERT(posHistoryGet(&h, 2, &x, &y) == FALSE);

    /* Twenty records into sixteen places: the oldest four are gone, and the
     * sixteen left read back newest first. Every third one says absent. */
    posHistoryInit(&h);
    for (i = 0; i < POSITION_HISTORY_SIZE + 4; i++) {
        posHistoryRecord(&h, (WORLD)(100 + i), (WORLD)(200 + i), (i % 3) != 0);
    }
    for (i = 0; i < POSITION_HISTORY_SIZE; i++) {
        int rec = POSITION_HISTORY_SIZE + 4 - 1 - i;   /* the record i frames back */
        PosHistoryLookup got;
        x = 7; y = 9;
        got = posHistoryLookup(&h, (uint8_t)i, &x, &y);
        if ((rec % 3) != 0) {
            UT_ASSERT_MSG(got == POS_HISTORY_PRESENT &&
                          x == (WORLD)(100 + rec) && y == (WORLD)(200 + rec),
                          "%d frames back: expected record %d with its position, "
                          "got result %d at (%u,%u)", i, rec, (int)got,
                          (unsigned)x, (unsigned)y);
            UT_ASSERT(posHistoryGet(&h, (uint8_t)i, &x, &y) == TRUE);
        } else {
            UT_ASSERT_MSG(got == POS_HISTORY_ABSENT && x == 7 && y == 9,
                          "%d frames back: expected record %d to say absent, "
                          "got result %d", i, rec, (int)got);
            UT_ASSERT(posHistoryGet(&h, (uint8_t)i, &x, &y) == FALSE);
        }
    }
    UT_ASSERT(posHistoryLookup(&h, POSITION_HISTORY_SIZE, &x, &y) == POS_HISTORY_NO_RECORD);
    UT_ASSERT(posHistoryLookup(&h, 255, &x, &y) == POS_HISTORY_NO_RECORD);

    return 0;
}

/* ── the builder cases ────────────────────────────────────────────────── */

/* A square with no base, no pillbox and no mine on it. */
static bool las_square_free(GameSim *gs, int x, int y) {
    return !basesExistPos(&gs->bs, (BYTE)x, (BYTE)y) &&
           !pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y) &&
           !minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y);
}

/* A row of free squares: the tank's own, the one west of it the colliding
 * shell starts in, and every square east of it out to the harvest square. */
static bool las_find_row(GameSim *gs, BYTE *ox, BYTE *oy) {
    int x, y, dx;
    for (y = 40; y <= 200; y++) {
        for (x = 40; x <= 200; x++) {
            bool ok = true;
            for (dx = -1; dx <= LAS_FAR && ok; dx++) {
                ok = las_square_free(gs, x + dx, y);
            }
            if (ok) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* A running two-player game: the builder's player and the shooter. */
static int las_open(LasFix *f) {
    BYTE emap[6000] = E_MAP;
    int dx;

    memset(f, 0, sizeof(*f));
    f->sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                       gameOpen, false, 0, -1);
    UT_ASSERT_MSG(f->sim != NULL, "serverSimCreateCompressed returned NULL");
    serverSimSetLobbyEnabled(f->sim, false);
    serverSimAddPlayer(f->sim, LAS_BUILDER, "Builder", false);
    serverSimAddPlayer(f->sim, LAS_SHOOTER, "Shooter", false);
    serverSimStartGame(f->sim);
    f->gs = &f->sim->sim;

    UT_ASSERT_MSG(f->gs->tanks[LAS_BUILDER] != NULL && f->gs->lgmen[LAS_BUILDER] != NULL &&
                  f->gs->tanks[LAS_SHOOTER] != NULL,
                  "the two players joined without a tank or a man");
    UT_ASSERT_MSG(f->gs->lgmPosHistoryPtr == f->sim->lgmPosHistory,
                  "the game does not read the server's builder history");
    UT_ASSERT_MSG(las_find_row(f->gs, &f->cx, &f->cy),
                  "no row of squares free of bases, pillboxes and mines");

    /* Grass under everything, so nothing depends on what the map has there:
     * the builder walks on it and a blast on it is not a blast on a wall. */
    for (dx = -1; dx < LAS_FAR; dx++) {
        mapSetPos(f->gs, &f->gs->mp, (BYTE)(f->cx + dx), f->cy, GRASS, FALSE, FALSE);
    }

    f->midX = (WORLD)(((WORLD)f->cx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    f->midY = (WORLD)(((WORLD)f->cy << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    f->oldX = (WORLD)(f->midX + LAS_OLD * (MAP_SQUARE_MIDDLE * 2));
    f->oldY = f->midY;
    return 0;
}

static void las_close(LasFix *f) {
    if (f->sim != NULL) {
        serverSimDestroy(f->sim);
        f->sim = NULL;
    }
}

/* Put the tank on its square and send the builder out of it to harvest the
 * square LAS_FAR to the east. He starts 16 world units east of the tank's
 * centre. Also undoes a kill from the case before, so one game serves
 * several cases. */
static int las_send_out(LasFix *f) {
    GameSim *gs = f->gs;
    tank *tnk = &gs->tanks[LAS_BUILDER];
    lgm *man = &gs->lgmen[LAS_BUILDER];

    tankSetWorld(gs, tnk, f->midX, f->midY, 0, FALSE);
    tankSetOnBoat(tnk, FALSE);
    (*man)->isDead = FALSE;
    (*man)->inTank = TRUE;
    (*man)->state = LGM_STATE_IDLE;
    mapSetPos(gs, &gs->mp, (BYTE)(f->cx + LAS_FAR), f->cy, FOREST, FALSE, FALSE);
    lgmNewPrimaryRequest(gs, man, tnk, (BYTE)(f->cx + LAS_FAR), f->cy, LGM_TREE_REQUEST);

    UT_ASSERT_MSG((*man)->state == LGM_STATE_GOING && (*man)->inTank == FALSE &&
                  (*man)->isDead == FALSE,
                  "the harvest order never sent the man out (state %u)",
                  (unsigned)(*man)->state);
    UT_ASSERT_MSG((*man)->x == (WORLD)(f->midX + 16) && (*man)->y == f->midY,
                  "the man is not 16 east of the tank: he is at (%u,%u), the "
                  "tank at (%u,%u)", (unsigned)(*man)->x, (unsigned)(*man)->y,
                  (unsigned)f->midX, (unsigned)f->midY);
    /* The tank test of a player's shell has its own history. Left empty, the
     * tank is tested where it is now. */
    posHistoryInit(&f->sim->posHistory[LAS_BUILDER]);
    return 0;
}

/* Start the builder history again and write `absent` frames with him in the
 * tank, then `atOld` frames with him out at the old place, then `atNow`
 * frames with him out where he is now. The last one written is the present
 * frame. */
static void las_history(LasFix *f, int absent, int atOld, int atNow) {
    PosHistory *h = &f->sim->lgmPosHistory[LAS_BUILDER];
    lgm man = f->gs->lgmen[LAS_BUILDER];
    int i;

    posHistoryInit(h);
    for (i = 0; i < absent; i++) {
        posHistoryRecord(h, f->midX, f->midY, FALSE);
    }
    for (i = 0; i < atOld; i++) {
        posHistoryRecord(h, f->oldX, f->oldY, TRUE);
    }
    for (i = 0; i < atNow; i++) {
        posHistoryRecord(h, man->x, man->y, TRUE);
    }
}

/* What the shell-death callback was told on the last update. */
static int     las_deaths;
static uint8_t las_death_outcome;

static void las_death_cb(void *ctx, uint32_t fireTick, uint32_t serverFireTick,
                         BYTE owner, WORLD impactWX, WORLD impactWY,
                         uint8_t outcome) {
    (void)ctx; (void)fireTick; (void)serverFireTick; (void)owner;
    (void)impactWX; (void)impactWY;
    las_deaths++;
    las_death_outcome = outcome;
}

/* Explode one shell and say through *died whether the builder is dead.
 * comp is the rewind the shell carries: 0 with owner NEUTRAL is a pill's
 * shell. At LAS_SITE_EXPIRY the blast is at (bx, by). At LAS_SITE_COLLISION
 * the shell hits the builder's tank and the blast is on the tank's centre;
 * bx and by are not used. */
static int las_explode(LasFix *f, LasSite site, WORLD bx, WORLD by,
                       BYTE owner, BYTE pill, uint8_t comp, bool *died) {
    GameSim *gs = f->gs;
    tank  tanksArray[MAX_TANKS];
    lgm  *lgmPtrs[MAX_TANKS];
    BYTE  numTanks = 0;
    BYTE  i;
    shells q;

    /* The compacted arrays simRunHalfStep hands shellsUpdate. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (f->sim->playerConnected[i] && gs->tanks[i] != NULL) {
            tanksArray[numTanks] = gs->tanks[i];
            lgmPtrs[numTanks] = &gs->lgmen[i];
            numTanks++;
        }
    }
    UT_ASSERT_MSG(numTanks == 2, "expected 2 players, got %d", (int)numTanks);

    shellsAddItem(gs, &gs->shs, bx, by, 0, 8, owner, NEUTRAL, pill, FALSE);
    q = gs->shs;
    UT_ASSERT_MSG(q != NULL, "the shell was not added");
    q->compensationTicks = comp;
    q->xAcc = 0;
    q->yAcc = 0;
    if (site == LAS_SITE_EXPIRY) {
        /* No life left: it explodes where it is on the next update. */
        q->x = bx;
        q->y = by;
        q->length = SHELL_DEATH;
    } else {
        /* Plenty of life left, so only a collision can end it. One step of 8
         * to the east puts it on the tank's centre. */
        q->x = (WORLD)(f->midX - 8);
        q->y = f->midY;
        q->xStep = 8 * 256;
        q->yStep = 0;
        q->length = 20;
    }

    {
        void (*prev)(void *, uint32_t, uint32_t, BYTE, WORLD, WORLD, uint8_t) =
            gs->callbacks.shellDeath;

        las_deaths = 0;
        las_death_outcome = 0;
        gs->callbacks.shellDeath = las_death_cb;
        shellsUpdate(gs, tanksArray, numTanks, lgmPtrs, &gs->ss);
        gs->callbacks.shellDeath = prev;
    }

    /* Not vacuous: the shell must have exploded on this update, and in the
     * way the case asks for. The server takes a shell that ran out of life
     * off the list on the same update, and one that hit something on the
     * next, so the list does not say; the shell-death callback does. */
    UT_ASSERT_MSG(las_deaths == 1,
                  "%d shells exploded on this update, expected 1 (%s)",
                  las_deaths,
                  site == LAS_SITE_EXPIRY ? "end of life" : "collision");
    if (site == LAS_SITE_EXPIRY) {
        UT_ASSERT_MSG(las_death_outcome == SHELL_OUTCOME_EXPIRED,
                      "the shell did not run out of life (outcome %u)",
                      (unsigned)las_death_outcome);
    } else {
        UT_ASSERT_MSG(las_death_outcome == SHELL_OUTCOME_TANK_HIT ||
                          las_death_outcome == SHELL_OUTCOME_TANK_KILL,
                      "the shell did not hit the tank (outcome %u)",
                      (unsigned)las_death_outcome);
    }

    *died = (gs->lgmen[LAS_BUILDER]->isDead == TRUE);
    return 0;
}

static const char *las_site_name(LasSite site) {
    return site == LAS_SITE_EXPIRY ? "end of life" : "collision";
}

int run_lgm_kill_absent_on_shooter_screen(void) {
    LasFix f;
    int rc = las_open(&f);
    int s;

    for (s = LAS_SITE_EXPIRY; s <= LAS_SITE_COLLISION && rc == 0; s++) {
        LasSite site = (LasSite)s;
        bool died = false;
        lgm man;

        /* In the tank up to and on the frame the shooter saw, LAS_REWIND
         * frames back; out on the five frames since, the present one
         * included. The blast is on top of him as he is now. */
        rc = las_send_out(&f);
        if (rc != 0) break;
        man = f.gs->lgmen[LAS_BUILDER];
        las_history(&f, POSITION_HISTORY_SIZE - LAS_REWIND, 0, LAS_REWIND);
        rc = las_explode(&f, site, man->x, man->y, LAS_SHOOTER, DMG_NO_PILL,
                         LAS_COMP, &died);
        if (rc != 0) break;
        /* Before the rule this fell back to where he is now and killed him. */
        UT_ASSERT_MSG(!died,
                      "%s: the shooter's screen showed no builder, but the "
                      "shell killed him", las_site_name(site));
        UT_ASSERT_MSG(man->inTank == FALSE && man->state == LGM_STATE_GOING,
                      "%s: the builder who lived was disturbed (state %u)",
                      las_site_name(site), (unsigned)man->state);

        /* The same game one frame of rewind shorter: that frame has him out,
         * right where the blast is, and he dies. So the case above is one
         * the blast would have killed in. */
        rc = las_explode(&f, site, man->x, man->y, LAS_SHOOTER, DMG_NO_PILL,
                         (uint8_t)(LAS_COMP - 1), &died);
        if (rc != 0) break;
        UT_ASSERT_MSG(died,
                      "%s: the builder was out, under the blast, on the frame "
                      "the shooter saw, and lived", las_site_name(site));
    }

    las_close(&f);
    return rc;
}

int run_lgm_kill_rewound_position(void) {
    LasFix f;
    int rc = las_open(&f);
    bool died = false;
    lgm man;
    int s;

    /* Out on both frames: at the old place, two squares east, on the frame
     * the shooter saw, and 16 from the tank now. A blast at the old place
     * kills him, though he is nearly two squares from it now. */
    if (rc == 0) rc = las_send_out(&f);
    if (rc == 0) {
        las_history(&f, 0, POSITION_HISTORY_SIZE - LAS_REWIND, LAS_REWIND);
        rc = las_explode(&f, LAS_SITE_EXPIRY, f.oldX, f.oldY, LAS_SHOOTER,
                         DMG_NO_PILL, LAS_COMP, &died);
    }
    if (rc == 0) {
        UT_ASSERT_MSG(died, "a blast where the shooter saw the builder did "
                            "not kill him");
    }

    /* A blast only where he is now does not: the shooter saw him elsewhere. */
    for (s = LAS_SITE_EXPIRY; s <= LAS_SITE_COLLISION && rc == 0; s++) {
        LasSite site = (LasSite)s;
        rc = las_send_out(&f);
        if (rc != 0) break;
        man = f.gs->lgmen[LAS_BUILDER];
        las_history(&f, 0, POSITION_HISTORY_SIZE - LAS_REWIND, LAS_REWIND);
        rc = las_explode(&f, site, man->x, man->y, LAS_SHOOTER, DMG_NO_PILL,
                         LAS_COMP, &died);
        if (rc != 0) break;
        UT_ASSERT_MSG(!died,
                      "%s: a blast where the builder is now killed him, but "
                      "the shooter saw him two squares away",
                      las_site_name(site));
    }

    las_close(&f);
    return rc;
}

int run_lgm_kill_pill_shell_present(void) {
    LasFix f;
    int rc = las_open(&f);
    int s;

    /* In the tank until one frame ago. A pill's shell carries no rewind, so
     * the history is not asked: he dies where he is now. */
    for (s = LAS_SITE_EXPIRY; s <= LAS_SITE_COLLISION && rc == 0; s++) {
        LasSite site = (LasSite)s;
        bool died = false;
        lgm man;

        rc = las_send_out(&f);
        if (rc != 0) break;
        man = f.gs->lgmen[LAS_BUILDER];
        las_history(&f, POSITION_HISTORY_SIZE - 1, 0, 1);
        rc = las_explode(&f, site, man->x, man->y, NEUTRAL, 0, 0, &died);
        if (rc != 0) break;
        UT_ASSERT_MSG(died,
                      "%s: a pill's shell on top of a builder who has just "
                      "left the tank did not kill him", las_site_name(site));
    }

    las_close(&f);
    return rc;
}

int run_lgm_kill_short_history(void) {
    LasFix f;
    int rc = las_open(&f);
    int s;

    for (s = LAS_SITE_EXPIRY; s <= LAS_SITE_COLLISION && rc == 0; s++) {
        LasSite site = (LasSite)s;
        bool died = false;
        lgm man;

        /* LAS_REWIND records, so none is LAS_REWIND frames back (the newest
         * is 0 back). The older ones say he was in the tank, but no record
         * is as old as the shooter's screen: he is tested where he is now
         * and dies. */
        rc = las_send_out(&f);
        if (rc != 0) break;
        man = f.gs->lgmen[LAS_BUILDER];
        las_history(&f, LAS_REWIND - 1, 0, 1);
        rc = las_explode(&f, site, man->x, man->y, LAS_SHOOTER, DMG_NO_PILL,
                         LAS_COMP, &died);
        if (rc != 0) break;
        UT_ASSERT_MSG(died,
                      "%s: with no record as old as the shooter's screen the "
                      "builder was not tested where he is now",
                      las_site_name(site));

        /* One record more. Now there is a record LAS_REWIND frames back and
         * it says he was in the tank: he lives. */
        rc = las_send_out(&f);
        if (rc != 0) break;
        man = f.gs->lgmen[LAS_BUILDER];
        las_history(&f, LAS_REWIND, 0, 1);
        rc = las_explode(&f, site, man->x, man->y, LAS_SHOOTER, DMG_NO_PILL,
                         LAS_COMP, &died);
        if (rc != 0) break;
        UT_ASSERT_MSG(!died,
                      "%s: the oldest record is the frame the shooter saw and "
                      "says the builder was in the tank, but he was killed",
                      las_site_name(site));
    }

    las_close(&f);
    return rc;
}
