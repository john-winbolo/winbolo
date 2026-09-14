/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Record a round in which the world moves, replay it, and require the
 * replayed world to be the recorded one.
 *
 * The round changes a terrain cell, captures a base and draws a shell from
 * it, captures a pillbox, and puts a tank on stocks it never spawns with.
 * Each change goes through the sim's own function, so what reaches the file
 * is whatever the recorder makes of an ordinary round — no log record is
 * written by hand.
 *
 * Every change happens after the opening snapshot and the round is far
 * shorter than the interval between snapshots, so no later snapshot ever
 * restates the world. The only way the replayed world can match is if the
 * per-change events carried all of it.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "bolo_map.h"
#include "bases.h"
#include "pillbox.h"
#include "tank.h"
#include "log.h"            /* LOG_VERSION_V3 — the version the file states */
#include "replay_harness.h"
#include "test_harness.h"

/* A cell that can be changed without disturbing anything else the round
 * asserts on: holding grass, clear of every base and pillbox, and inside
 * the border that mapGetPos reports as deep sea whatever is stored there.
 * Returns false when the fixture map has no such cell. */
static bool findPlainGrassCell(GameSim *gs, BYTE *outX, BYTE *outY) {
    int x;
    int y;
    BYTE count;
    BYTE numBases = basesGetNumBases(&gs->bs);
    BYTE numPills = pillsGetNumPills(&gs->pb);

    for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
        for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
            bool taken = false;
            if (mapGetPos(&gs->mp, (BYTE) x, (BYTE) y) != GRASS) {
                continue;
            }
            for (count = 1; count <= numBases && !taken; count++) {
                base b;
                memset(&b, 0, sizeof(b));
                basesGetBase(&gs->bs, &b, count);
                if (b.x == x && b.y == y) taken = true;
            }
            for (count = 1; count <= numPills && !taken; count++) {
                pillbox p;
                memset(&p, 0, sizeof(p));
                pillsGetPill(&gs->pb, &p, count);
                if (p.x == x && p.y == y) taken = true;
            }
            if (!taken) {
                *outX = (BYTE) x;
                *outY = (BYTE) y;
                return true;
            }
        }
    }
    return false;
}

int run_replay_roundtrip_world(void) {
    ReplayHarness h;
    GameSim *gs;
    base firstBase;
    pillbox firstPill;
    BYTE terrainX = 0;
    BYTE terrainY = 0;
    BYTE shellsBefore;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "world", "Tester"),
                  "could not start recording");
    gs = serverSimGetGameSim(h.sim);
    UT_ASSERT_MSG(gs != NULL, "no GameSim");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "no slot-0 tank");

    /* Let the round settle after the opening snapshot. */
    replayHarnessTick(&h, 4);

    /* Terrain: one cell of grass becomes road. */
    UT_ASSERT_MSG(findPlainGrassCell(gs, &terrainX, &terrainY),
                  "fixture map has no free grass cell to change");
    mapSetPos(gs, &gs->mp, terrainX, terrainY, ROAD, FALSE, FALSE);
    UT_ASSERT_MSG(mapGetPos(&gs->mp, terrainX, terrainY) == ROAD,
                  "terrain at (%d,%d) did not change", terrainX, terrainY);

    /* Base: hand it to player 0, then draw a shell out of it.
     *
     * The owner change is made the migrate way — the path a base takes when
     * it moves between players as an alliance breaks up. It changes the
     * owner and records it exactly as a capture does, and skips the
     * newswire message, the client event and the winbolo.net report that a
     * capture also raises, none of which a headless round has any use for.
     * It also leaves the base's stock alone, so the shell drawn below is
     * the only thing that moves the stock. */
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 1, "fixture map has no bases");
    memset(&firstBase, 0, sizeof(firstBase));
    basesGetBase(&gs->bs, &firstBase, 1);
    UT_ASSERT_MSG(firstBase.owner != 0,
                  "base 1 already belongs to player 0, so handing it over "
                  "would change nothing");
    UT_ASSERT_MSG(firstBase.shells >= BASE_MIN_SHELLS + BASE_SHELLS_GIVE,
                  "base 1 starts with %d shells, too few to draw one",
                  firstBase.shells);
    shellsBefore = firstBase.shells;

    basesSetBaseOwner(gs, 1, 0, TRUE, TRUE);
    basesNetGiveShells(gs, &gs->bs, 0);   /* the give path counts bases from 0 */

    memset(&firstBase, 0, sizeof(firstBase));
    basesGetBase(&gs->bs, &firstBase, 1);
    UT_ASSERT_MSG(firstBase.owner == 0, "base 1 owner is %d, expected 0",
                  firstBase.owner);
    UT_ASSERT_MSG(firstBase.shells != shellsBefore,
                  "base 1 shells stayed at %d", shellsBefore);

    /* Pillbox: hand it to player 0, the same migrate way and for the same
     * reasons as the base above. */
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "fixture map has no pills");
    memset(&firstPill, 0, sizeof(firstPill));
    pillsGetPill(&gs->pb, &firstPill, 1);
    UT_ASSERT_MSG(firstPill.owner != 0,
                  "pill 1 already belongs to player 0, so handing it over "
                  "would change nothing");

    pillsSetPillOwner(gs, &gs->pb, 1, 0, TRUE);
    memset(&firstPill, 0, sizeof(firstPill));
    pillsGetPill(&gs->pb, &firstPill, 1);
    UT_ASSERT_MSG(firstPill.owner == 0, "pill 1 owner is %d, expected 0",
                  firstPill.owner);

    /* Tank stocks: four values it never spawns with, and all different from
     * each other, so a replay that filled them in from spawn defaults or
     * left them empty cannot match by luck. */
    tankSetShells(gs, &gs->tanks[0], 7);
    tankSetMines(gs, &gs->tanks[0], 11);
    tankSetArmour(&gs->tanks[0], 23);
    tankSetTrees(gs, &gs->tanks[0], 5);

    /* Carry the changes into the file: events raised between ticks are
     * flushed by the next recorded tick, and the tank's stocks are read by
     * the recorder's own per-tick pass. */
    replayHarnessTick(&h, 6);

    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");
    UT_ASSERT_MSG(replayHarnessDecode(&h),
                  "replay did not decode to end-of-log");
    UT_ASSERT_MSG(replayHarnessCompare(&h), "replayed world differs: %s",
                  replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}

/* The pill health record names the pillbox and its armour in a byte each, and
 * the writer and the viewer have to agree on that length. A reader that takes
 * one byte where two were written treats the armour as the next record's event
 * code and reads everything after it off its boundaries, so the damage is not
 * to the armour but to whatever follows.
 *
 * The round therefore changes a pillbox's armour and its square in the one
 * call, which writes the health record with two more behind it in the same
 * stream. A length mistake comes back as a pill on the wrong square rather
 * than as an armour value that happens to match.
 *
 * pillsSetPill is the sim's own function for this and writes four records —
 * owner, health, in-tank, placement — so nothing here is a hand-built log.
 *
 * The two-byte record is what LOG_VERSION 3 means, so the case also opens the
 * file and reads its version byte: a recording that carries this record while
 * stating version 2 is the ambiguity the version bump exists to remove, and no
 * reader could size it. The nibble form every earlier version holds is covered
 * by test_replay_version_compat.c. */
int run_replay_roundtrip_pill_health(void) {
    ReplayHarness h;
    GameSim *gs;
    pillbox firstPill;
    BYTE pillX = 0;
    BYTE pillY = 0;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "pillhealth", "Tester"),
                  "could not start recording");
    gs = serverSimGetGameSim(h.sim);
    UT_ASSERT_MSG(gs != NULL, "no GameSim");

    replayHarnessTick(&h, 4);

    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1, "fixture map has no pills");
    UT_ASSERT_MSG(findPlainGrassCell(gs, &pillX, &pillY),
                  "fixture map has no free grass cell to move a pill onto");

    memset(&firstPill, 0, sizeof(firstPill));
    pillsGetPill(&gs->pb, &firstPill, 1);
    UT_ASSERT_MSG(firstPill.armour != 6,
                  "pill 1 already sits at armour 6, so setting it there would "
                  "record nothing to read back");
    UT_ASSERT_MSG(firstPill.x != pillX || firstPill.y != pillY,
                  "pill 1 is already on the free cell this case moves it to");
    firstPill.armour = 6;
    firstPill.x = pillX;
    firstPill.y = pillY;
    pillsSetPill(gs, &gs->pb, &firstPill, 1);

    memset(&firstPill, 0, sizeof(firstPill));
    pillsGetPill(&gs->pb, &firstPill, 1);
    UT_ASSERT_MSG(firstPill.armour == 6, "pill 1 armour is %d, expected 6",
                  firstPill.armour);
    UT_ASSERT_MSG(firstPill.x == pillX && firstPill.y == pillY,
                  "pill 1 sits at %d,%d, expected %d,%d",
                  firstPill.x, firstPill.y, pillX, pillY);

    replayHarnessTick(&h, 6);

    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    /* The file says which shape its health record holds. */
    {
        uint8_t *logDat = NULL;
        size_t   logLen = 0;
        uint8_t  version;
        UT_ASSERT_MSG(extractLogDat(h.path, &logDat, &logLen),
                      "could not read log.dat out of %s", h.path);
        UT_ASSERT_MSG(logLen > 9, "log.dat is %u bytes", (unsigned) logLen);
        version = logDat[8];
        free(logDat);
        UT_ASSERT_MSG(version == LOG_VERSION_V3,
                      "the recording states version %u; the two-byte health "
                      "record is version %u", (unsigned) version,
                      (unsigned) LOG_VERSION_V3);
    }

    UT_ASSERT_MSG(replayHarnessDecode(&h),
                  "replay did not decode to end-of-log");
    UT_ASSERT_MSG(replayHarnessCompare(&h), "replayed world differs: %s",
                  replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}

/* A base that changes hands because its owner left the game goes through
 * basesMigrate, which writes log_BaseSetOwner for each base it moves. The
 * record has to name the base and the new owner in that order; a record
 * that names the owner where the base index goes puts the change on the
 * wrong base, and the replay shows a base the sim never touched changing
 * hands while the one that moved stays put. */
int run_replay_roundtrip_base_migrate(void) {
    ReplayHarness h;
    GameSim *gs;
    base firstBase;
    BYTE oldOwner;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "migrate", "Tester"),
                  "could not start recording");
    gs = serverSimGetGameSim(h.sim);
    UT_ASSERT_MSG(gs != NULL, "no GameSim");

    replayHarnessTick(&h, 4);

    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 1, "fixture map has no bases");
    memset(&firstBase, 0, sizeof(firstBase));
    basesGetBase(&gs->bs, &firstBase, 1);
    oldOwner = firstBase.owner;
    UT_ASSERT_MSG(oldOwner != 0,
                  "base 1 already belongs to player 0, so migrating it "
                  "would change nothing");

    basesMigrate(gs, oldOwner, 0);

    memset(&firstBase, 0, sizeof(firstBase));
    basesGetBase(&gs->bs, &firstBase, 1);
    UT_ASSERT_MSG(firstBase.owner == 0, "base 1 owner is %d, expected 0",
                  firstBase.owner);

    replayHarnessTick(&h, 6);

    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");
    UT_ASSERT_MSG(replayHarnessDecode(&h),
                  "replay did not decode to end-of-log");
    UT_ASSERT_MSG(replayHarnessCompare(&h), "replayed world differs: %s",
                  replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}

/* Seed distinct stocks before the opening snapshot. Use base 2 so an event
 * that accidentally names base 0 cannot pass the comparison. */
static int prepareBaseStockReplay(ReplayHarness *h, const char *tag, BYTE owner) {
    GameSim *gs;
    base item;

    UT_ASSERT_MSG(replayHarnessPrepare(h, tag, "Tester"),
                  "could not prepare recording");
    gs = serverSimGetGameSim(h->sim);
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 2, "fixture needs two bases");
    memset(&item, 0, sizeof(item));
    basesGetBase(&gs->bs, &item, 2);
    item.owner = owner;
    item.armour = 18;
    item.shells = 37;
    item.mines = 29;
    basesSetBase(&gs->bs, &item, 2);
    UT_ASSERT_MSG(replayHarnessBeginRecording(h), "could not start recording");
    return 0;
}

static int finishBaseStockReplay(ReplayHarness *h) {
    /* Flush immediately, before regeneration or a snapshot can conceal a
     * missing stock event. */
    replayHarnessTick(h, 2);
    UT_ASSERT_MSG(replayHarnessStopRecording(h), "could not stop recording");
    UT_ASSERT_MSG(replayHarnessDecode(h), "could not decode recording");
    UT_ASSERT_MSG(replayHarnessCompare(h), "replayed world differs: %s",
                  replayHarnessDiff(h));
    replayHarnessStop(h);
    return 0;
}

int run_replay_roundtrip_base_damage(void) {
    int damageCase;

    /* Default damage, custom damage, and damage clamped to the armour left. */
    for (damageCase = 0; damageCase < 3; damageCase++) {
        ReplayHarness h;
        GameSim *gs;
        base item;
        BYTE expectedArmour;

        UT_ASSERT(prepareBaseStockReplay(&h, "basedamage", 1) == 0);
        gs = serverSimGetGameSim(h.sim);
        if (damageCase == 0) {
            UT_ASSERT(gs->rules.shell_damage == 5);
            expectedArmour = 13;
        } else if (damageCase == 1) {
            gs->rules.shell_damage = 7;
            expectedArmour = 11;
        } else {
            gs->rules.shell_damage = 25;
            expectedArmour = 0;
        }
        memset(&item, 0, sizeof(item));
        basesGetBase(&gs->bs, &item, 2);
        UT_ASSERT(basesCanHit(gs, item.x, item.y, 0));
        basesDamagePos(gs, item.x, item.y, 0);
        basesGetBase(&gs->bs, &item, 2);
        UT_ASSERT(item.armour == expectedArmour);
        UT_ASSERT(item.shells == 37 && item.mines == 29);
        UT_ASSERT(finishBaseStockReplay(&h) == 0);
    }
    return 0;
}

int run_replay_roundtrip_base_capture_stock(void) {
    int captureCase;

    /* Both capture entry points: stealing empties the base, taking a
     * neutral base preserves stock. Also cover an explicit keepStock transfer. */
    for (captureCase = 0; captureCase < 5; captureCase++) {
        ReplayHarness h;
        GameSim *gs;
        base item;
        BYTE previousOwner = captureCase == 2 || captureCase == 3 ? NEUTRAL : 1;
        bool emptied = captureCase < 2;

        UT_ASSERT(prepareBaseStockReplay(&h, "basecapture", previousOwner) == 0);
        gs = serverSimGetGameSim(h.sim);
        memset(&item, 0, sizeof(item));
        basesGetBase(&gs->bs, &item, 2);
        if (captureCase == 0 || captureCase == 2) {
            UT_ASSERT(basesSetOwner(gs, item.x, item.y, 0, FALSE) == previousOwner);
        } else {
            UT_ASSERT(basesSetBaseOwner(gs, 2, 0, FALSE,
                                       captureCase == 4) == previousOwner);
        }
        basesGetBase(&gs->bs, &item, 2);
        UT_ASSERT(item.owner == 0);
        UT_ASSERT(item.armour == (emptied ? 0 : 18));
        UT_ASSERT(item.shells == (emptied ? 0 : 37));
        UT_ASSERT(item.mines == (emptied ? 0 : 29));
        UT_ASSERT(finishBaseStockReplay(&h) == 0);
    }
    return 0;
}
