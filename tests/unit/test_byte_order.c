/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The little-endian fields of the formats that took their byte order from
 * the hosts that wrote them (see src/bolo/public/byte_order.h).
 *
 * Each case drives the real writer, or the real reader, and pins the exact
 * bytes. Every case but byte_order_helpers and byte_order_info_packet goes
 * through code that predates byte_order.h, so it states the bytes the
 * formats have always had on a little-endian host, and a change that moved
 * them would fail here. What these cannot do is run the big-endian half:
 * on a little-endian host a write in host order and an explicit
 * little-endian write give the same bytes, so a host-order write slipping
 * back in would pass. They pin the format; they do not prove portability.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "global.h"
#include "byte_order.h"
#include "attribution_track.h"
#include "log.h"
#include "server_sim.h"
#include "server_sim_internal.h"  /* ServerSim.tick and its callbacks — T2 */
#include "bases.h"
#include "pillbox.h"
#include "starts.h"
#include "bolo_map.h"
#include "netpacks.h"
#include "brain_pathfinder.h"
#include "lv_attribution.h"
#include "unzip.h"
#include "replay_harness.h"
#include "test_harness.h"

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* One member of a .wbv into *outBuf (caller frees). */
static bool boExtractMember(const char *wbvPath, const char *member,
                            uint8_t **outBuf, size_t *outLen) {
    unzFile uf = unzOpen(wbvPath);
    unz_file_info info;
    uint8_t *buf;
    size_t got = 0;

    if (uf == NULL) return false;
    if (unzLocateFile(uf, member, 0) != UNZ_OK ||
        unzGetCurrentFileInfo(uf, &info, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK ||
        unzOpenCurrentFile(uf) != UNZ_OK) {
        unzClose(uf);
        return false;
    }
    buf = (uint8_t *)malloc(info.uncompressed_size ? info.uncompressed_size : 1);
    if (buf == NULL) {
        unzCloseCurrentFile(uf);
        unzClose(uf);
        return false;
    }
    while (got < info.uncompressed_size) {
        int chunk = unzReadCurrentFile(uf, buf + got,
                                       (unsigned int)(info.uncompressed_size - got));
        if (chunk <= 0) break;
        got += (size_t)chunk;
    }
    unzCloseCurrentFile(uf);
    unzClose(uf);
    if (got != info.uncompressed_size) {
        free(buf);
        return false;
    }
    *outBuf = buf;
    *outLen = got;
    return true;
}

/* A running sim with a .wbv open on it, the writer's owner thread pinned
 * by one empty tick, so the caller's next idle tick is its second. */
static ServerSim *boStartLog(const char *fname) {
    ServerSim *sim = ut_make_running_sim("Tester");
    if (sim == NULL) return NULL;
    logCreate();
    if (!logStart((char *)fname, sim, 0, MAX_TANKS, FALSE)) {
        logDestroy();
        serverSimDestroy(sim);
        return NULL;
    }
    logWriteTick();
    return sim;
}

/* ── byte_order.h itself ─────────────────────────────────────────────── */

int run_byte_order_helpers(void) {
    uint8_t b[4];

    byteOrderPutLe16(b, 0x1234);
    UT_ASSERT(b[0] == 0x34 && b[1] == 0x12);
    UT_ASSERT(byteOrderGetLe16(b) == 0x1234);

    byteOrderPutLe32(b, 0x01020304u);
    UT_ASSERT(b[0] == 0x04 && b[1] == 0x03 && b[2] == 0x02 && b[3] == 0x01);
    UT_ASSERT(byteOrderGetLe32(b) == 0x01020304u);

    /* High bits survive: no sign extension through the shifts. */
    byteOrderPutLe32(b, 0xFEDCBA98u);
    UT_ASSERT(byteOrderGetLe32(b) == 0xFEDCBA98u);
    byteOrderPutLe16(b, 0xFEDC);
    UT_ASSERT(byteOrderGetLe16(b) == 0xFEDC);

    /* 1.5f is 0x3FC00000. */
    byteOrderPutLeFloat(b, 1.5f);
    UT_ASSERT(b[0] == 0x00 && b[1] == 0x00 && b[2] == 0xC0 && b[3] == 0x3F);
    UT_ASSERT(byteOrderGetLeFloat(b) == 1.5f);
    return 0;
}

/* ── log.dat's long counts ───────────────────────────────────────────── */

#define BO_IDLE_TICKS  300   /* 0x012C: past a byte, and a different number either way round */
#define BO_LONG_EVENTS 300
#define BO_SHELL_BYTES 7     /* [type][u16 len][four payload bytes] */

/* 300 idle ticks then a tick of 300 shells, so both long records are
 * written, and nothing after them but the quit. */
static bool boRecordLongCounts(const char *fname) {
    ServerSim *sim = boStartLog(fname);
    int i;

    if (sim == NULL) return false;
    for (i = 1; i < BO_IDLE_TICKS; i++) {
        logWriteTick();
    }
    for (i = 0; i < BO_LONG_EVENTS; i++) {
        logAddEvent(log_Shell, 100, 100, 0x88, 9, 0, NULL);
    }
    logWriteTick();
    logStop();
    logDestroy();
    serverSimDestroy(sim);
    return true;
}

int run_byte_order_log_long_counts(void) {
    const char *fname = "test_byte_order_long_counts.wbv";
    uint8_t *buf = NULL;
    size_t len = 0;
    size_t tail = 6 + BO_LONG_EVENTS * BO_SHELL_BYTES + 2;
    size_t at;

    remove(fname);
    UT_ASSERT_MSG(boRecordLongCounts(fname), "could not record");
    UT_ASSERT_MSG(boExtractMember(fname, "log.dat", &buf, &len), "no log.dat");
    remove(fname);

    UT_ASSERT_MSG(len > tail, "log.dat is %zu bytes", len);
    at = len - tail;
    /* Both counts little-endian: 300 is 2C 01. */
    UT_ASSERT_MSG(buf[at] == LOG_NOEVENTS_LONG && buf[at + 1] == 0x2C && buf[at + 2] == 0x01,
                  "idle record %02X %02X %02X", buf[at], buf[at + 1], buf[at + 2]);
    UT_ASSERT_MSG(buf[at + 3] == LOG_EVENT_LONG && buf[at + 4] == 0x2C && buf[at + 5] == 0x01,
                  "event record %02X %02X %02X", buf[at + 3], buf[at + 4], buf[at + 5]);
    for (int i = 0; i < BO_LONG_EVENTS; i++) {
        const uint8_t *e = buf + at + 6 + (size_t)i * BO_SHELL_BYTES;
        UT_ASSERT_MSG(e[0] == log_Shell && e[1] == 0 && e[2] == 4,
                      "event %d framed as %02X %02X %02X", i, e[0], e[1], e[2]);
    }
    UT_ASSERT(buf[len - 2] == LOG_QUIT && buf[len - 1] == LOG_QUIT);
    free(buf);
    return 0;
}

/* The same recording through the production viewer: playback and the load's
 * byte walk both have to read the two counts as 300. Read the other way
 * round they are 11265, and the playback and walk lengths run to minutes. */
int run_byte_order_log_long_counts_viewer(void) {
    const char *fname = "test_byte_order_long_counts_viewer.wbv";
    static ReplayWorld world;
    ReplayFileInfo info;
    bool played;

    remove(fname);
    UT_ASSERT_MSG(boRecordLongCounts(fname), "could not record");
    memset(&info, 0, sizeof(info));
    played = replayHarnessDecodeFile(fname, &world, &info);
    remove(fname);

    UT_ASSERT_MSG(played, "the viewer did not play the file to its end");
    /* After the opening snapshot: the idle record's own step and its 300,
     * the event block's step and the quit's. */
    UT_ASSERT_MSG(info.totalTimeMs == (BO_IDLE_TICKS + 3) * 20u,
                  "walk length %u ms (want %u)", (unsigned)info.totalTimeMs,
                  (unsigned)((BO_IDLE_TICKS + 3) * 20));
    UT_ASSERT_MSG(info.ticks >= BO_IDLE_TICKS && info.ticks <= BO_IDLE_TICKS + 4,
                  "playback took %d ticks", info.ticks);
    return 0;
}

/* ── Base time ───────────────────────────────────────────────────────── */

/* A snapshot's base record: x, y, owner, armour, shells, mines, refuel time,
 * base time as two little-endian bytes, just stopped. */
int run_byte_order_snapshot_base_time(void) {
    bases bs;
    BYTE buf[1 + MAX_BASES * 10];
    BYTE n;

    basesCreate(&bs);
    basesSetNumBases(&bs, 2);
    bs->item[1].baseTime = 0x1234;
    n = basesGetBaseNetData(&bs, buf);
    basesDestroy(&bs);

    UT_ASSERT_MSG(n == 21, "net data is %d bytes", n);
    UT_ASSERT_MSG(buf[1 + 10 + 7] == 0x34 && buf[1 + 10 + 8] == 0x12,
                  "base time bytes %02X %02X", buf[18], buf[19]);
    return 0;
}

/* The compressed map a server sends: the bases' wire region is struct base
 * as laid out, and its one multi-byte field, baseTime, is little-endian. */
int run_byte_order_compressed_map_base_time(void) {
    static BYTE blob[MAP_COMPRESSED_MAX_SIZE];
    static BYTE raw[MAP_UNCOMPRESSED_SIZE];
    size_t at = offsetof(struct basesObj, item) + 1 * sizeof(base) + offsetof(base, baseTime);
    uLongf rawLen = sizeof(raw);
    map mp, mp2;
    pillboxes pb, pb2;
    bases bs, bs2;
    starts ss, ss2;
    int32_t loaded;
    int n;

    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);
    basesSetNumBases(&bs, 2);
    bs->item[1].baseTime = 0x01020304;
    n = mapSaveCompressedMap(&mp, &pb, &bs, &ss, blob, (int)sizeof(blob));

    mapCreate(&mp2);
    pillsCreate(&pb2);
    basesCreate(&bs2);
    startsCreate(&ss2);
    loaded = -1;
    if (n > 0 && mapLoadCompressedMap(&mp2, &pb2, &bs2, &ss2, blob, n)) {
        loaded = bs2->item[1].baseTime;
    }
    mapDestroy(&mp);  pillsDestroy(&pb);  basesDestroy(&bs);  startsDestroy(&ss);
    mapDestroy(&mp2); pillsDestroy(&pb2); basesDestroy(&bs2); startsDestroy(&ss2);

    UT_ASSERT_MSG(n > 0, "the map did not compress");
    UT_ASSERT_MSG(uncompress(raw, &rawLen, blob, (uLong)n) == Z_OK, "the blob did not inflate");
    UT_ASSERT_MSG(raw[at] == 0x04 && raw[at + 1] == 0x03 && raw[at + 2] == 0x02 && raw[at + 3] == 0x01,
                  "base time bytes %02X %02X %02X %02X",
                  raw[at], raw[at + 1], raw[at + 2], raw[at + 3]);
    UT_ASSERT_MSG(loaded == 0x01020304, "base time loaded as 0x%08X", (unsigned)loaded);
    return 0;
}

/* ── attribution.trk ─────────────────────────────────────────────────── */

/* A damage record written through the sim's own hook and closed out by
 * logStop: the header's slot and record counts and the record's tick and
 * amount are little-endian, and the viewer's reader takes them back. */
int run_byte_order_attribution_member(void) {
    const char *fname = "test_byte_order_attribution.wbv";
    const AttrTrackHeader *hdr;
    const uint8_t *recs;
    AttrDamageRecord dr;
    uint8_t *buf = NULL;
    size_t len = 0;
    size_t recLen = 0;
    uint32_t count;
    uint32_t readCount = 0;
    ServerSim *sim;
    const uint8_t *rec;

    remove(fname);
    sim = boStartLog(fname);
    UT_ASSERT_MSG(sim != NULL, "could not start the log");
    sim->tick = 0x01020304u;
    sim->sim.callbacks.recordDamage(sim->sim.callbacks.ctx, 0, ATTR_TGT_TANK, 3,
                                    ATTR_SRC_SHELL, 0x0102, false, 8, 9);
    count = sim->trackRecordCount;
    logStop();
    logDestroy();
    serverSimDestroy(sim);
    UT_ASSERT_MSG(boExtractMember(fname, ATTRIBUTION_TRACK_MEMBER, &buf, &len),
                  "no attribution member");
    remove(fname);

    UT_ASSERT_MSG(len >= sizeof(AttrTrackHeader) + sizeof(AttrDamageRecord),
                  "member is %zu bytes", len);
    UT_ASSERT(memcmp(buf, ATTRIBUTION_TRACK_MAGIC, 4) == 0);
    UT_ASSERT_MSG(buf[6] == MAX_TANKS && buf[7] == 0, "slot count %02X %02X", buf[6], buf[7]);
    UT_ASSERT_MSG(buf[1064] == (uint8_t)count && buf[1065] == (uint8_t)(count >> 8) &&
                  buf[1066] == (uint8_t)(count >> 16) && buf[1067] == (uint8_t)(count >> 24),
                  "record count bytes %02X %02X %02X %02X",
                  buf[1064], buf[1065], buf[1066], buf[1067]);

    /* The damage record is the last thing in the stream. */
    rec = buf + len - sizeof(AttrDamageRecord);
    UT_ASSERT(rec[0] == ATTR_REC_DAMAGE);
    UT_ASSERT_MSG(rec[1] == 0x04 && rec[2] == 0x03 && rec[3] == 0x02 && rec[4] == 0x01,
                  "tick bytes %02X %02X %02X %02X", rec[1], rec[2], rec[3], rec[4]);
    UT_ASSERT_MSG(rec[9] == 0x02 && rec[10] == 0x01, "amount bytes %02X %02X", rec[9], rec[10]);

    UT_ASSERT_MSG(lvAttributionParseMember(buf, len), "the reader refused the member");
    free(buf);
    hdr = lvAttributionGetHeader();
    recs = lvAttributionGetRecords(&recLen, &readCount);
    UT_ASSERT(hdr != NULL && recs != NULL);
    UT_ASSERT_MSG(hdr->slotCount == MAX_TANKS, "slot count read as %u", hdr->slotCount);
    UT_ASSERT_MSG(readCount == count, "record count read as %u (want %u)", readCount, count);
    memcpy(&dr, recs + recLen - sizeof(dr), sizeof(dr));
    lvAttributionClear();
    UT_ASSERT_MSG(dr.tick == 0x01020304u, "tick read as 0x%08X", (unsigned)dr.tick);
    UT_ASSERT_MSG(dr.amount == 0x0102, "amount read as 0x%04X", (unsigned)dr.amount);
    return 0;
}

/* ── INFO_PACKET ─────────────────────────────────────────────────────── */

/* The six numbers little-endian at the offsets docs/info_packet_wire.md
 * gives, start_time left in network order, and the readers' macros taking
 * the six back. */
int run_byte_order_info_packet(void) {
    INFO_PACKET pkt;
    BYTE b[sizeof(INFO_PACKET)];

    UT_ASSERT(offsetof(INFO_PACKET, gameid.serverport) == 48);
    UT_ASSERT(offsetof(INFO_PACKET, gameid.start_time) == 52);
    UT_ASSERT(offsetof(INFO_PACKET, start_delay) == 60);
    UT_ASSERT(offsetof(INFO_PACKET, time_limit) == 64);
    UT_ASSERT(offsetof(INFO_PACKET, num_players) == 68);
    UT_ASSERT(offsetof(INFO_PACKET, free_pills) == 70);
    UT_ASSERT(offsetof(INFO_PACKET, free_bases) == 72);

    memset(&pkt, 0, sizeof(pkt));
    pkt.gameid.serverport = 27500;   /* 0x6B6C */
    pkt.gameid.start_time = htonl(0x0A0B0C0Du);
    pkt.start_delay = 0x01020304;
    pkt.time_limit = -2;
    pkt.num_players = 0x0102;
    pkt.free_pills = 0x0304;
    pkt.free_bases = 0x0506;
    infoPacketEncode(b, &pkt);

    UT_ASSERT(b[48] == 0x6C && b[49] == 0x6B);
    UT_ASSERT(b[52] == 0x0A && b[53] == 0x0B && b[54] == 0x0C && b[55] == 0x0D);
    UT_ASSERT(b[60] == 0x04 && b[61] == 0x03 && b[62] == 0x02 && b[63] == 0x01);
    UT_ASSERT(b[64] == 0xFE && b[65] == 0xFF && b[66] == 0xFF && b[67] == 0xFF);
    UT_ASSERT(b[68] == 0x02 && b[69] == 0x01);
    UT_ASSERT(b[70] == 0x04 && b[71] == 0x03);
    UT_ASSERT(b[72] == 0x06 && b[73] == 0x05);

    UT_ASSERT(INFO_PACKET_GET16(b, gameid.serverport) == 27500);
    UT_ASSERT((int32_t)INFO_PACKET_GET32(b, start_delay) == 0x01020304);
    UT_ASSERT((int32_t)INFO_PACKET_GET32(b, time_limit) == -2);
    UT_ASSERT(INFO_PACKET_GET16(b, num_players) == 0x0102);
    UT_ASSERT(INFO_PACKET_GET16(b, free_pills) == 0x0304);
    UT_ASSERT(INFO_PACKET_GET16(b, free_bases) == 0x0506);
    return 0;
}

/* ── The pathfinder state blob ───────────────────────────────────────── */

/* "BPFS", version 1 and the blob's own length little-endian, then the
 * config floats as little-endian IEEE-754; and a round trip that keeps a
 * scalar and a cell of two of the compressed grids. */
int run_byte_order_pathfinder_blob(void) {
    BrainPathfinder *pf = brainPathfinderCreate();
    BrainPathfinder *back = brainPathfinderCreate();
    unsigned char *blob = NULL;
    size_t size = 0;
    int ok = 0;
    float turnCost = 0.0f;
    uint16_t danger = 0;
    int16_t influence = 0;

    if (pf != NULL && back != NULL) {
        pf->turn_cost = 1.5f;
        pf->danger_grid[1] = 0x1234;
        pf->influence_grid[2] = -2;
        blob = brainPathfinderSerialize(pf, &size);
        if (blob != NULL) {
            ok = brainPathfinderDeserialize(back, blob, size);
            turnCost = back->turn_cost;
            danger = back->danger_grid[1];
            influence = back->influence_grid[2];
        }
    }
    if (pf != NULL) brainPathfinderDestroy(pf);
    if (back != NULL) brainPathfinderDestroy(back);

    UT_ASSERT_MSG(blob != NULL && size > 16, "no blob");
    UT_ASSERT(blob[0] == 'B' && blob[1] == 'P' && blob[2] == 'F' && blob[3] == 'S');
    UT_ASSERT(blob[4] == 1 && blob[5] == 0 && blob[6] == 0 && blob[7] == 0);
    UT_ASSERT_MSG(blob[8] == (uint8_t)size && blob[9] == (uint8_t)(size >> 8) &&
                  blob[10] == (uint8_t)(size >> 16) && blob[11] == (uint8_t)(size >> 24),
                  "length bytes %02X %02X %02X %02X", blob[8], blob[9], blob[10], blob[11]);
    UT_ASSERT_MSG(blob[12] == 0x00 && blob[13] == 0x00 && blob[14] == 0xC0 && blob[15] == 0x3F,
                  "turn cost bytes %02X %02X %02X %02X", blob[12], blob[13], blob[14], blob[15]);
    free(blob);
    UT_ASSERT_MSG(ok, "the blob did not deserialize");
    UT_ASSERT(turnCost == 1.5f);
    UT_ASSERT(danger == 0x1234);
    UT_ASSERT(influence == -2);
    return 0;
}
