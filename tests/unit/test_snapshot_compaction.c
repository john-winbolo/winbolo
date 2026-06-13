/*
 * Field-presence snapshot compaction.
 *
 * A non-stub TankSnapshot is packed as an 11-byte core (playerNum, presence
 * mask, worldX, worldY, angle, speed, tankStatus) followed by only the field
 * groups whose values are non-zero. The presence mask records which groups
 * follow; the unpacker zeroes everything first and restores absent groups to
 * 0, so the encoding is byte-for-byte equivalent to always sending every
 * field. These tests exercise the pure pack -> unpack roundtrip (no sockets):
 * field fidelity across representative tanks, the wire-size bounds the
 * optimization buys, the unchanged 1-byte stub, and bounds-safety against a
 * truncated buffer.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "input_packet.h"
#include "transport_udp_internal.h"  /* pack/unpackTankSnapshot, TANK_SNAPSHOT_WIRE_SIZE */
#include "test_harness.h"

/* Roundtrip ts through pack -> unpack and assert every field survives.
 * Returns the packed size via *outSize. */
static int roundtrip(const TankSnapshot *ts, int *outSize) {
    uint8_t buf[64];
    TankSnapshot got;
    int packed = packTankSnapshot(buf, ts);
    int consumed = unpackTankSnapshot(buf, (size_t)packed, &got);

    UT_ASSERT_MSG(consumed == packed,
                  "consumed %d != packed %d", consumed, packed);
    UT_ASSERT(got.playerNum == ts->playerNum);
    UT_ASSERT(got.worldX == ts->worldX);
    UT_ASSERT(got.worldY == ts->worldY);
    UT_ASSERT(got.angle == ts->angle);
    UT_ASSERT(got.speed == ts->speed);
    UT_ASSERT(got.tankStatus == ts->tankStatus);
    UT_ASSERT(got.lgmFrame == ts->lgmFrame);
    UT_ASSERT(got.lgmMX == ts->lgmMX);
    UT_ASSERT(got.lgmMY == ts->lgmMY);
    UT_ASSERT(got.lgmPX == ts->lgmPX);
    UT_ASSERT(got.lgmPY == ts->lgmPY);
    UT_ASSERT(got.armour == ts->armour);
    UT_ASSERT(got.shells == ts->shells);
    UT_ASSERT(got.mines == ts->mines);
    UT_ASSERT(got.trees == ts->trees);
    UT_ASSERT(got.firstLeft == ts->firstLeft);
    UT_ASSERT(got.firstRight == ts->firstRight);
    UT_ASSERT(got.gunsightLen == ts->gunsightLen);
    UT_ASSERT(got.deathWait == ts->deathWait);
    UT_ASSERT(got.reload == ts->reload);
    UT_ASSERT(got.pingMs == ts->pingMs);
    UT_ASSERT(got.clientFlags == ts->clientFlags);
    *outSize = packed;
    return 0;
}

int run_snapshot_compaction(void) {
    int size;

    /* (a) Non-owner moving tank: core fields only, no owner resources, no
     * LGM, no death/reload. Just position/heading + a ping. Should collapse
     * to the 11-byte core plus the 2-byte ping group. */
    {
        TankSnapshot ts;
        memset(&ts, 0, sizeof(ts));
        ts.playerNum = 3;
        ts.worldX = 12345;
        ts.worldY = 23456;
        ts.angle = 4096;
        ts.speed = 256;
        ts.tankStatus = 0x01;   /* onBoat */
        ts.pingMs = 84;
        if (roundtrip(&ts, &size) != 0) return 1;
        UT_ASSERT_MSG(size <= 13, "non-owner moving entry was %d bytes", size);

        /* Same tank with a zero ping packs even smaller (bare core). */
        ts.pingMs = 0;
        if (roundtrip(&ts, &size) != 0) return 1;
        UT_ASSERT_MSG(size == 11, "bare-core entry was %d bytes", size);
    }

    /* (b) Owner full tank: resources + reload + gunsight + ping all live. */
    {
        TankSnapshot ts;
        memset(&ts, 0, sizeof(ts));
        ts.playerNum = 0;
        ts.worldX = 5000;
        ts.worldY = 6000;
        ts.angle = 1024;
        ts.speed = 128;
        ts.tankStatus = 0;
        ts.armour = 40;
        ts.shells = 30;
        ts.mines = 20;
        ts.trees = 12;
        ts.gunsightLen = 7;
        ts.reload = 5;
        ts.pingMs = 42;
        if (roundtrip(&ts, &size) != 0) return 1;
        UT_ASSERT_MSG(size <= TANK_SNAPSHOT_WIRE_SIZE,
                      "owner full entry was %d bytes", size);
    }

    /* (c) Dead tank: deathWait group set. */
    {
        TankSnapshot ts;
        memset(&ts, 0, sizeof(ts));
        ts.playerNum = 5;
        ts.worldX = 100;
        ts.worldY = 200;
        ts.tankStatus = 0x10;   /* isDead */
        ts.deathWait = 99;
        if (roundtrip(&ts, &size) != 0) return 1;
    }

    /* (d) LGM-out tank: the LGM group set, plus a turn-ramp value. */
    {
        TankSnapshot ts;
        memset(&ts, 0, sizeof(ts));
        ts.playerNum = 7;
        ts.worldX = 700;
        ts.worldY = 800;
        ts.angle = 2048;
        ts.lgmFrame = 3;
        ts.lgmMX = 11;
        ts.lgmMY = 22;
        ts.lgmPX = 33;
        ts.lgmPY = 44;
        ts.firstLeft = 6;
        ts.firstRight = 0;
        ts.clientFlags = 0x02;
        if (roundtrip(&ts, &size) != 0) return 1;
    }

    /* Every field maxed at once exercises all groups present together; must
     * stay within the conservative wire bound. */
    {
        TankSnapshot ts;
        int j;
        uint8_t *p = (uint8_t *)&ts;
        for (j = 0; j < (int)sizeof(ts); j++) p[j] = 0xFF;
        ts.playerNum = 0x7F;    /* keep the hidden-flag bit clear */
        if (roundtrip(&ts, &size) != 0) return 1;
        UT_ASSERT_MSG(size <= TANK_SNAPSHOT_WIRE_SIZE,
                      "all-groups entry was %d bytes", size);
    }

    /* (e) Stub (HIDDEN_FLAG) stays 1 byte and decodes to a cleared tank. */
    {
        uint8_t buf[8];
        TankSnapshot ts, got;
        int packed, consumed;
        memset(&ts, 0, sizeof(ts));
        ts.playerNum = (uint8_t)(TANK_SNAPSHOT_HIDDEN_FLAG | 4);
        ts.worldX = 9999;       /* payload that must NOT ride the wire */
        packed = packTankSnapshot(buf, &ts);
        UT_ASSERT_MSG(packed == 1, "stub packed to %d bytes", packed);
        consumed = unpackTankSnapshot(buf, (size_t)packed, &got);
        UT_ASSERT(consumed == 1);
        UT_ASSERT(got.playerNum == ts.playerNum);
        UT_ASSERT(got.worldX == 0);   /* stub leaves all payload fields zero */
    }

    /* (f) Truncated buffer: the unpacker must report 0, not read past avail. */
    {
        uint8_t buf[64];
        TankSnapshot ts, got;
        int packed;
        memset(&ts, 0, sizeof(ts));
        ts.playerNum = 2;
        ts.worldX = 1;
        ts.armour = 10;         /* force the OWNER_RES group on */
        ts.pingMs = 9;          /* and the PING group */
        packed = packTankSnapshot(buf, &ts);
        UT_ASSERT(packed > 11);

        /* Short of even the core. */
        UT_ASSERT(unpackTankSnapshot(buf, 0, &got) == 0);
        UT_ASSERT(unpackTankSnapshot(buf, 10, &got) == 0);
        /* Core present but a group truncated mid-way. */
        UT_ASSERT(unpackTankSnapshot(buf, (size_t)(packed - 1), &got) == 0);
        /* Exactly enough still works. */
        UT_ASSERT(unpackTankSnapshot(buf, (size_t)packed, &got) == packed);
    }

    return 0;
}
