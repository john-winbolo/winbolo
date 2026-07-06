/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Log-viewer attribution-track reader.
 *
 * Round-trips a member buffer built exactly as the server writer emits it
 * (AttrTrackHeader + packed record stream) through lvAttributionParseMember
 * and the accessors; checks rejection of short / wrong-magic / too-new
 * members; and confirms an old .wbv with no track member loads with the
 * accessor reporting absent.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "attribution_track.h"
#include "lv_attribution.h"
#include "blocks.h"
#include "test_harness.h"

#ifndef WB_WBV_FIXTURE_DIR
#define WB_WBV_FIXTURE_DIR "tests/fixtures/wbv"
#endif

int run_attribution_reader_roundtrip(void) {
    /* Build the member the way the writer does: header then three records. */
    AttrTrackHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, ATTRIBUTION_TRACK_MAGIC, 4);
    hdr.version     = ATTRIBUTION_TRACK_VERSION;
    hdr.truncated   = 0;
    hdr.slotCount   = MAX_TANKS;
    hdr.slots[0].isBot = 0; hdr.slots[0].team = 1;
    strcpy(hdr.slots[0].name, "Alice");
    hdr.slots[1].isBot = 1; hdr.slots[1].team = 2;
    strcpy(hdr.slots[1].name, "Bot2");
    hdr.recordCount = 3;

    AttrDamageRecord dr;
    memset(&dr, 0, sizeof dr);
    dr.type = ATTR_REC_DAMAGE; dr.tick = 100;
    dr.source = ATTR_SRC_SHELL; dr.target = ATTR_TGT_TANK;
    dr.targetIndex = 3; dr.attacker = 0; dr.amount = 10; dr.destroyed = 0;

    AttrActionRecord ar;
    memset(&ar, 0, sizeof ar);
    ar.type = ATTR_REC_ACTION; ar.tick = 101; ar.player = 0; ar.action = ATTR_ACT_FARM;

    AttrKillRecord kr;
    memset(&kr, 0, sizeof kr);
    kr.type = ATTR_REC_KILL; kr.tick = 102; kr.killer = 2; kr.killed = 5;
    kr.deathCause = 1; kr.carriedPills = 4; kr.treesWasted = 7;

    size_t recBytes = sizeof dr + sizeof ar + sizeof kr;
    size_t total = sizeof hdr + recBytes;
    uint8_t *buf = (uint8_t *)malloc(total);
    UT_ASSERT(buf != NULL);
    size_t o = 0;
    memcpy(buf + o, &hdr, sizeof hdr); o += sizeof hdr;
    memcpy(buf + o, &dr, sizeof dr);   o += sizeof dr;
    memcpy(buf + o, &ar, sizeof ar);   o += sizeof ar;
    memcpy(buf + o, &kr, sizeof kr);   o += sizeof kr;

    UT_ASSERT_MSG(lvAttributionParseMember(buf, total), "parse should accept a valid member");

    const AttrTrackHeader *h = lvAttributionGetHeader();
    UT_ASSERT(h != NULL);
    UT_ASSERT(memcmp(h->magic, ATTRIBUTION_TRACK_MAGIC, 4) == 0);
    UT_ASSERT(h->version == ATTRIBUTION_TRACK_VERSION);
    UT_ASSERT(h->truncated == 0);
    UT_ASSERT(h->slotCount == MAX_TANKS);
    UT_ASSERT(h->recordCount == 3);
    UT_ASSERT(h->slots[0].team == 1 && h->slots[0].isBot == 0);
    UT_ASSERT(strcmp(h->slots[0].name, "Alice") == 0);
    UT_ASSERT(h->slots[1].team == 2 && h->slots[1].isBot == 1);
    UT_ASSERT(strcmp(h->slots[1].name, "Bot2") == 0);

    size_t rlen = 0; uint32_t rcount = 0;
    const uint8_t *recs = lvAttributionGetRecords(&rlen, &rcount);
    UT_ASSERT(recs != NULL);
    UT_ASSERT_MSG(rcount == 3, "record count, got %u", rcount);
    UT_ASSERT_MSG(rlen == recBytes, "record bytes, got %zu", rlen);

    /* Walk the heterogeneous stream by leading type tag. */
    size_t off = 0;
    UT_ASSERT(recs[off] == ATTR_REC_DAMAGE);
    AttrDamageRecord gdr; memcpy(&gdr, recs + off, sizeof gdr); off += sizeof gdr;
    UT_ASSERT(gdr.tick == 100 && gdr.source == ATTR_SRC_SHELL && gdr.target == ATTR_TGT_TANK);
    UT_ASSERT(gdr.targetIndex == 3 && gdr.attacker == 0 && gdr.amount == 10 && gdr.destroyed == 0);

    UT_ASSERT(recs[off] == ATTR_REC_ACTION);
    AttrActionRecord gar; memcpy(&gar, recs + off, sizeof gar); off += sizeof gar;
    UT_ASSERT(gar.tick == 101 && gar.player == 0 && gar.action == ATTR_ACT_FARM);

    UT_ASSERT(recs[off] == ATTR_REC_KILL);
    AttrKillRecord gkr; memcpy(&gkr, recs + off, sizeof gkr); off += sizeof gkr;
    UT_ASSERT(gkr.tick == 102 && gkr.killer == 2 && gkr.killed == 5);
    UT_ASSERT(gkr.deathCause == 1 && gkr.carriedPills == 4 && gkr.treesWasted == 7);
    UT_ASSERT(off == recBytes);

    lvAttributionClear();
    UT_ASSERT_MSG(lvAttributionGetHeader() == NULL, "cleared track must report absent");
    size_t clen = 1; uint32_t ccount = 1;
    UT_ASSERT(lvAttributionGetRecords(&clen, &ccount) == NULL);
    UT_ASSERT(clen == 0 && ccount == 0);

    free(buf);
    return 0;
}

int run_attribution_reader_rejects_bad(void) {
    /* (a) shorter than the header. */
    uint8_t shortBuf[4];
    memcpy(shortBuf, ATTRIBUTION_TRACK_MAGIC, 4);
    UT_ASSERT(!lvAttributionParseMember(shortBuf, sizeof shortBuf));
    UT_ASSERT(lvAttributionGetHeader() == NULL);

    /* (b) full-size header, wrong magic. */
    AttrTrackHeader bad;
    memset(&bad, 0, sizeof bad);
    memcpy(bad.magic, "XXXX", 4);
    bad.version = ATTRIBUTION_TRACK_VERSION;
    bad.slotCount = MAX_TANKS;
    UT_ASSERT(!lvAttributionParseMember((const uint8_t *)&bad, sizeof bad));
    UT_ASSERT(lvAttributionGetHeader() == NULL);

    /* (c) valid magic, version too new. */
    AttrTrackHeader newer;
    memset(&newer, 0, sizeof newer);
    memcpy(newer.magic, ATTRIBUTION_TRACK_MAGIC, 4);
    newer.version = ATTRIBUTION_TRACK_VERSION + 1;
    newer.slotCount = MAX_TANKS;
    UT_ASSERT(!lvAttributionParseMember((const uint8_t *)&newer, sizeof newer));
    UT_ASSERT(lvAttributionGetHeader() == NULL);

    return 0;
}

int run_attribution_reader_old_wbv(void) {
    const char *dir = getenv("WB_WBV_FIXTURE_DIR");
    char path[512];
    FILE *f;
    long sz;
    uint8_t *buf;
    size_t got;
    bool loaded;

    if (dir == NULL || dir[0] == '\0') dir = WB_WBV_FIXTURE_DIR;
    snprintf(path, sizeof(path), "%s/spectator_v2.wbv", dir);

    f = fopen(path, "rb");
    if (f == NULL) {
        UT_FAIL("fixture missing: %s — generate it with: "
                "WB_WBV_FIXTURE_DIR=%s ./WinBoloUnitTests --test wbv_v2_capture",
                path, dir);
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        UT_FAIL("fixture empty: %s", path);
    }
    buf = (uint8_t *)malloc((size_t)sz);
    UT_ASSERT_MSG(buf != NULL, "malloc %ld failed", sz);
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    UT_ASSERT_MSG(got == (size_t)sz, "short read: %zu of %ld", got, sz);

    /* Takes ownership of buf (freed by lv_blocksDestroy). This old fixture has
     * no attribution.trk member, so the load must succeed and the accessor
     * must report absent. */
    loaded = lv_blocksCreateFromMemory(buf, (size_t)sz);
    UT_ASSERT_MSG(loaded == TRUE, "old .wbv should still load");
    UT_ASSERT_MSG(lvAttributionGetHeader() == NULL,
                  "old .wbv without a track member must report absent");

    lv_blocksDestroy();
    UT_ASSERT(lvAttributionGetHeader() == NULL);
    return 0;
}
