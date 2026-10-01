/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Pillbox armour above 15 (test_pill_armour_scale.c).
 *
 * There are sixteen pillbox pictures and pill_max_armour is a rule a scenario
 * can set as high as 255, so the drawing code scales the armour onto the
 * pictures rather than indexing them with it, and the client caps what a
 * server tells it about a pill's armour the way the sim's own load paths do.
 *
 *   classic_cap_is_unchanged — with the cap at 15 each armour still draws as
 *       its own picture, which is the mapping the game has always had.
 *   raised_cap_scales        — with the cap at 30 a full pill draws full, an
 *       empty one empty, and half armour lands halfway along. Before this
 *       every armour above 15 fell through to the empty picture, so a pill on
 *       such a sim drew as a wreck until it was shot down to 15.
 *   client_caps_what_arrives — an EVENT_PILL_UPDATE and a snapshot each
 *       carrying 200 armour land at the cap, not at 200.
 */

#include <string.h>

#include "global.h"
#include "tilenum.h"
#include "input_packet.h"
#include "pillbox.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_snapshot.h"
#include "test_harness.h"

/* The square the single test pillbox stands on. */
#define PA_X 30
#define PA_Y 40

/* The sixteen enemy pictures, worst first — the same order the drawing code
 * keeps them in, written out again here so the case compares against
 * something rather than against the table it is testing. */
static const BYTE paEvil[16] = {
    PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
    PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
    PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
    PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
};

/* A client holding one neutral pillbox on PA_X,PA_Y. Neutral is nobody's
 * ally, so the pill draws with the enemy pictures. */
static ClientSim *paClient(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim   *gs;

    if (cs == NULL) {
        return NULL;
    }
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);
    pillsSetNumPills(&gs->pb, 1);
    (*gs->pb).item[0].x      = PA_X;
    (*gs->pb).item[0].y      = PA_Y;
    (*gs->pb).item[0].owner  = NEUTRAL;
    (*gs->pb).item[0].inTank = FALSE;
    (*gs->pb).item[0].armour = (BYTE)gs->rules.pill_max_armour;
    return cs;
}

/* The picture the pill on PA_X,PA_Y draws as with this armour. Written into
 * the record directly: the question is the lookup, not the clamp. */
static BYTE paTileFor(ClientSim *cs, BYTE armour) {
    GameSim *gs = clientSimGetGameSim(cs);

    (*gs->pb).item[0].armour = armour;
    return pillsGetScreenHealth(gs, &gs->pb, PA_X, PA_Y, gs->viewPlayer);
}

/* A snapshot header with one pill block on it. */
static SnapshotHeader paHeader(uint32_t tick, BYTE pillCount) {
    SnapshotHeader hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.serverTick = tick;
    hdr.pillCount  = pillCount;
    return hdr;
}

/* ================================================================
 * 1. The classic cap: sixteen armour values, sixteen pictures, each its own.
 * ================================================================ */
int run_pill_armour_scale_classic_cap(void) {
    ClientSim *cs = paClient();
    GameSim   *gs;
    int        a;

    UT_ASSERT_MSG(cs != NULL, "could not build a client");
    gs = clientSimGetGameSim(cs);
    UT_ASSERT_MSG(gs->rules.pill_max_armour == PILLS_MAX_ARMOUR,
                  "setup: a fresh client should be on the classic cap, got %ld",
                  (long)gs->rules.pill_max_armour);

    for (a = 0; a <= PILLS_MAX_ARMOUR; a++) {
        BYTE got = paTileFor(cs, (BYTE)a);
        if (got != paEvil[a]) {
            clientSimDestroy(cs);
            UT_ASSERT_MSG(got == paEvil[a],
                          "armour %d drew tile %u, expected %u — the classic "
                          "cap must keep every armour on its own picture",
                          a, (unsigned)got, (unsigned)paEvil[a]);
        }
    }

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 2. A raised cap: the sixteen pictures spread across it.
 * ================================================================ */
int run_pill_armour_scale_raised_cap(void) {
    ClientSim *cs = paClient();
    GameSim   *gs;
    BYTE       full, half, empty, hurt;

    UT_ASSERT_MSG(cs != NULL, "could not build a client");
    gs = clientSimGetGameSim(cs);
    gs->rules.pill_max_armour = 30;

    full  = paTileFor(cs, 30);
    half  = paTileFor(cs, 15);
    empty = paTileFor(cs, 0);
    hurt  = paTileFor(cs, 29);
    clientSimDestroy(cs);

    UT_ASSERT_MSG(full == paEvil[15],
                  "a pill at the cap drew tile %u, expected the intact %u — "
                  "an armour above 15 used to fall through to the empty "
                  "picture", (unsigned)full, (unsigned)paEvil[15]);
    UT_ASSERT_MSG(half == paEvil[7],
                  "a pill at half the cap drew tile %u, expected the halfway "
                  "%u", (unsigned)half, (unsigned)paEvil[7]);
    UT_ASSERT_MSG(empty == paEvil[0],
                  "a pill with no armour drew tile %u, expected the empty %u",
                  (unsigned)empty, (unsigned)paEvil[0]);
    UT_ASSERT_MSG(hurt == paEvil[14],
                  "a pill one below the cap drew tile %u, expected %u — a "
                  "pill that has taken a hit must stop drawing intact",
                  (unsigned)hurt, (unsigned)paEvil[14]);
    return 0;
}

/* ================================================================
 * 3. The client caps the armour a server states.
 * ================================================================ */
int run_pill_armour_scale_client_caps(void) {
    ClientSim     *cs = paClient();
    GameSim       *gs;
    GameEvent      ev;
    PillSnapshot   snap;
    SnapshotHeader hdr;
    BYTE           afterEvent;
    BYTE           afterSnapshot;
    int32_t        cap;

    UT_ASSERT_MSG(cs != NULL, "could not build a client");
    gs = clientSimGetGameSim(cs);
    cap = gs->rules.pill_max_armour;

    /* data: [pillIndex, x, y, owner, pillFlags, armour] */
    memset(&ev, 0, sizeof(ev));
    ev.type = EVENT_PILL_UPDATE;
    ev.data[0] = 0;
    ev.data[1] = PA_X;
    ev.data[2] = PA_Y;
    ev.data[3] = NEUTRAL;
    ev.data[4] = pillSetPosCurrent(pillSetInTank(0, false), true);
    ev.data[5] = 200;

    hdr = paHeader(1, 0);
    hdr.reliableEventCount = 1;
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        NULL, 0, &ev, 1, 0);
    afterEvent = (*gs->pb).item[0].armour;

    /* And the pill block on a full sync, which writes the same field. */
    (*gs->pb).item[0].armour = 0;
    memset(&snap, 0, sizeof(snap));
    snap.x      = PA_X;
    snap.y      = PA_Y;
    snap.owner  = NEUTRAL;
    snap.armour = 200;
    snap.pillFlags = pillSetPosCurrent(pillSetInTank(0, false), true);

    hdr = paHeader(2, 1);
    clientApplySnapshot(cs, &hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                        &snap, 1, NULL, 0, 0);
    afterSnapshot = (*gs->pb).item[0].armour;
    clientSimDestroy(cs);

    UT_ASSERT_MSG((int32_t)afterEvent == cap,
                  "an EVENT_PILL_UPDATE carrying 200 armour landed as %u, "
                  "expected the cap %ld", (unsigned)afterEvent, (long)cap);
    UT_ASSERT_MSG((int32_t)afterSnapshot == cap,
                  "a snapshot carrying 200 armour landed as %u, expected the "
                  "cap %ld", (unsigned)afterSnapshot, (long)cap);
    return 0;
}
