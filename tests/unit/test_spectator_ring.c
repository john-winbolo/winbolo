/*
 * Mechanical proof for the spectator delayed-stream ring buffer
 * (src/bolo/spectator_ring.c). The ring is payload-agnostic, so every
 * case drives it with synthetic byte blobs whose contents encode the
 * game tick that produced them; reads are asserted byte-for-byte to
 * prove identity and ordering.
 *
 * Coverage: segmentation across a game-tick reset, the keyframe-at-
 * segment-start rule and cadence reporting, a mid-interval seek with
 * exact forward-event replay, segment isolation (no seek or replay
 * crosses a boundary), a previous-generation read across a reset, the
 * cold-start status on a too-young stream, and the retention window
 * boundary with its keyframe-aligned oldest survivor.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "log.h"
#include "log_internal.h"
#include "spectator_ring.h"
#include "test_harness.h"

/* Fill buf with a tick-derived pattern so a later read can prove identity. */
static void make_payload(uint8_t *buf, uint32_t gameTick, int len) {
    int i;
    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)(gameTick * 7u + (uint32_t)i);
    }
}

static bool rec_kf(SpectatorRing *r, uint32_t gameTick, int len) {
    uint8_t buf[64];
    make_payload(buf, gameTick, len);
    return spectatorRingRecordTick(r, gameTick, true, len ? buf : NULL, len);
}

static bool rec_ev(SpectatorRing *r, uint32_t gameTick, int len) {
    uint8_t buf[64];
    make_payload(buf, gameTick, len);
    return spectatorRingRecordTick(r, gameTick, false, len ? buf : NULL, len);
}

/* True when payload holds exactly the pattern make_payload writes for gameTick. */
static bool payload_matches(const uint8_t *payload, int len, uint32_t gameTick) {
    uint8_t expect[64];
    if (len < 0 || len > (int)sizeof(expect)) {
        return false;
    }
    if (len == 0) {
        return payload == NULL;
    }
    make_payload(expect, gameTick, len);
    return payload != NULL && memcmp(payload, expect, (size_t)len) == 0;
}

/* ---- segmentation ---- */

static int t_segmentation(void) {
    SpectatorRing *r = spectatorRingCreate(1000, 100000);
    UT_ASSERT(r != NULL);

    UT_ASSERT(rec_kf(r, 0, 8));
    UT_ASSERT(spectatorRingSegmentCount(r) == 1);
    UT_ASSERT(rec_ev(r, 1, 4));
    UT_ASSERT(rec_ev(r, 2, 4));
    /* Ascending ticks stay in one segment. */
    UT_ASSERT(spectatorRingSegmentCount(r) == 1);

    /* A reset to a tick <= the last recorded tick opens a new segment. */
    UT_ASSERT(rec_kf(r, 0, 8));
    UT_ASSERT(spectatorRingSegmentCount(r) == 2);
    UT_ASSERT(rec_ev(r, 1, 4));
    UT_ASSERT(spectatorRingSegmentCount(r) == 2);

    spectatorRingDestroy(r);
    return 0;
}

/* ---- keyframe-at-segment-start rule + cadence reporting ---- */

static int t_keyframe_at_segment_start(void) {
    SpectatorRing *r = spectatorRingCreate(4, 100000);
    UT_ASSERT(r != NULL);

    /* First record ever needs a keyframe. */
    UT_ASSERT(spectatorRingNeedsKeyframe(r, 0) == true);

    /* Opening a segment with a non-keyframe is rejected, no state change. */
    uint8_t buf[4] = {1, 2, 3, 4};
    UT_ASSERT(spectatorRingRecordTick(r, 0, false, buf, 4) == false);
    UT_ASSERT(spectatorRingSegmentCount(r) == 0);
    UT_ASSERT(spectatorRingHeadSeq(r) == 0);

    /* A keyframe opens it; recordSeq starts at 0 (proving nothing advanced). */
    UT_ASSERT(rec_kf(r, 0, 8));
    UT_ASSERT(spectatorRingHeadSeq(r) == 0);
    UT_ASSERT(spectatorRingSegmentCount(r) == 1);

    /* Cadence boundaries within the segment (cadence 4, segment start 0). */
    UT_ASSERT(spectatorRingNeedsKeyframe(r, 1) == false);
    UT_ASSERT(spectatorRingNeedsKeyframe(r, 2) == false);
    UT_ASSERT(spectatorRingNeedsKeyframe(r, 3) == false);
    UT_ASSERT(spectatorRingNeedsKeyframe(r, 4) == true);
    UT_ASSERT(spectatorRingNeedsKeyframe(r, 8) == true);

    /* Segment start is always a keyframe regardless of cadence. */
    UT_ASSERT(rec_ev(r, 1, 4));
    UT_ASSERT(rec_ev(r, 2, 4));
    UT_ASSERT(spectatorRingNeedsKeyframe(r, 0) == true);

    /* And opening the next segment with a non-keyframe is still rejected. */
    UT_ASSERT(spectatorRingRecordTick(r, 0, false, buf, 4) == false);
    UT_ASSERT(spectatorRingSegmentCount(r) == 1);
    UT_ASSERT(spectatorRingHeadSeq(r) == 2);

    spectatorRingDestroy(r);
    return 0;
}

/* ---- seek OK + exact mid-interval replay ---- */

static int t_seek_ok_mid_interval(void) {
    SpectatorRing *r = spectatorRingCreate(1000, 100000);
    const uint8_t *p;
    int len;
    uint32_t gt;
    SpectatorRingCursor cur;

    UT_ASSERT(r != NULL);

    /* seq0..seq5; keyframes at seq0 and (an explicit mid-stream) seq3. */
    UT_ASSERT(rec_kf(r, 0, 8));  /* seq0 */
    UT_ASSERT(rec_ev(r, 1, 5));  /* seq1 */
    UT_ASSERT(rec_ev(r, 2, 5));  /* seq2 */
    UT_ASSERT(rec_kf(r, 3, 8));  /* seq3 */
    UT_ASSERT(rec_ev(r, 4, 5));  /* seq4 */
    UT_ASSERT(rec_ev(r, 5, 5));  /* seq5 */
    UT_ASSERT(spectatorRingHeadSeq(r) == 5);
    UT_ASSERT(spectatorRingSegmentCount(r) == 1);

    /* delay 1 -> target seq4. Newest keyframe <= 4 is seq3; one event (seq4). */
    UT_ASSERT(spectatorRingSeekDelayed(r, 1, &cur) == SPECTATOR_RING_OK);
    p = spectatorRingCursorKeyframe(&cur, &len, &gt);
    UT_ASSERT(gt == 3 && len == 8 && payload_matches(p, len, 3));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt));
    UT_ASSERT(gt == 4 && len == 5 && payload_matches(p, len, 4));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt) == false);

    /* delay 0 -> target seq5. Seed seq3; events seq4, seq5 in order. */
    UT_ASSERT(spectatorRingSeekDelayed(r, 0, &cur) == SPECTATOR_RING_OK);
    p = spectatorRingCursorKeyframe(&cur, &len, &gt);
    UT_ASSERT(gt == 3 && payload_matches(p, len, 3));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt));
    UT_ASSERT(gt == 4 && payload_matches(p, len, 4));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt));
    UT_ASSERT(gt == 5 && payload_matches(p, len, 5));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt) == false);

    /* delay 3 -> target seq2. Seed seq0; events seq1, seq2. */
    UT_ASSERT(spectatorRingSeekDelayed(r, 3, &cur) == SPECTATOR_RING_OK);
    p = spectatorRingCursorKeyframe(&cur, &len, &gt);
    UT_ASSERT(gt == 0 && payload_matches(p, len, 0));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt));
    UT_ASSERT(gt == 1 && payload_matches(p, len, 1));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt));
    UT_ASSERT(gt == 2 && payload_matches(p, len, 2));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt) == false);

    spectatorRingDestroy(r);
    return 0;
}

/* ---- segment isolation: a seek never crosses a boundary ---- */

static int t_segment_isolation(void) {
    SpectatorRing *r = spectatorRingCreate(1000, 100000);
    const uint8_t *p;
    int len;
    uint32_t gt;
    SpectatorRingCursor cur;

    UT_ASSERT(r != NULL);

    /* Segment A: seq0..seq2. Segment B (reset): seq3..seq5. */
    UT_ASSERT(rec_kf(r, 0, 8));  /* seq0  A keyframe */
    UT_ASSERT(rec_ev(r, 1, 5));  /* seq1  A */
    UT_ASSERT(rec_ev(r, 2, 5));  /* seq2  A */
    UT_ASSERT(rec_kf(r, 0, 8));  /* seq3  B keyframe (gameTick resets to 0) */
    UT_ASSERT(rec_ev(r, 1, 5));  /* seq4  B */
    UT_ASSERT(rec_ev(r, 2, 5));  /* seq5  B */
    UT_ASSERT(spectatorRingSegmentCount(r) == 2);

    /* One tick after the reset (seq4) seeds from B's keyframe (seq3), never A. */
    UT_ASSERT(spectatorRingSeekDelayed(r, 1, &cur) == SPECTATOR_RING_OK);
    p = spectatorRingCursorKeyframe(&cur, &len, &gt);
    UT_ASSERT(gt == 0 && payload_matches(p, len, 0)); /* B keyframe, gameTick 0 */
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt));
    UT_ASSERT(gt == 1 && payload_matches(p, len, 1)); /* B's seq4 */
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt) == false);

    /* The reset record itself (seq3) seeds from B's keyframe, no events. */
    UT_ASSERT(spectatorRingSeekDelayed(r, 2, &cur) == SPECTATOR_RING_OK);
    p = spectatorRingCursorKeyframe(&cur, &len, &gt);
    UT_ASSERT(gt == 0 && payload_matches(p, len, 0));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt) == false);

    /* One tick before the reset (seq2) stays in A: seed seq0, events seq1,seq2,
     * and the replay never reaches into segment B. */
    UT_ASSERT(spectatorRingSeekDelayed(r, 3, &cur) == SPECTATOR_RING_OK);
    p = spectatorRingCursorKeyframe(&cur, &len, &gt);
    UT_ASSERT(gt == 0 && payload_matches(p, len, 0)); /* A keyframe */
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt));
    UT_ASSERT(gt == 1 && payload_matches(p, len, 1));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt));
    UT_ASSERT(gt == 2 && payload_matches(p, len, 2));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt) == false);

    spectatorRingDestroy(r);
    return 0;
}

/* ---- previous-generation read: old segment tail survives a reset ---- */

static int t_previous_generation_read(void) {
    SpectatorRing *r = spectatorRingCreate(1000, 100000);
    const uint8_t *p;
    int len;
    uint32_t gt;
    SpectatorRingCursor cur;

    UT_ASSERT(r != NULL);

    /* Segment A fully recorded, then B just opens (head sits at B's keyframe). */
    UT_ASSERT(rec_kf(r, 0, 8));  /* seq0  A keyframe */
    UT_ASSERT(rec_ev(r, 1, 5));  /* seq1  A */
    UT_ASSERT(rec_ev(r, 2, 5));  /* seq2  A */
    UT_ASSERT(rec_kf(r, 0, 8));  /* seq3  B keyframe */
    UT_ASSERT(spectatorRingHeadSeq(r) == 3);

    /* A delayed target still inside A (seq1) seeds from A's keyframe. */
    UT_ASSERT(spectatorRingSeekDelayed(r, 2, &cur) == SPECTATOR_RING_OK);
    p = spectatorRingCursorKeyframe(&cur, &len, &gt);
    UT_ASSERT(gt == 0 && payload_matches(p, len, 0));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt));
    UT_ASSERT(gt == 1 && payload_matches(p, len, 1));
    UT_ASSERT(spectatorRingCursorNextEvents(&cur, &p, &len, &gt) == false);

    spectatorRingDestroy(r);
    return 0;
}

/* ---- cold start: stream not yet old enough for the requested delay ---- */

static int t_cold_start(void) {
    SpectatorRing *r = spectatorRingCreate(1000, 100000);
    SpectatorRingCursor cur;

    UT_ASSERT(r != NULL);

    UT_ASSERT(rec_kf(r, 0, 8));  /* seq0 */
    UT_ASSERT(rec_ev(r, 1, 5));  /* seq1 */
    UT_ASSERT(rec_ev(r, 2, 5));  /* seq2 */

    /* A delay reaching before the segment's keyframe, with nothing yet aged
     * out, is COLD_START. */
    UT_ASSERT(spectatorRingSeekDelayed(r, 5, &cur) == SPECTATOR_RING_COLD_START);
    UT_ASSERT(spectatorRingSeekDelayed(r, 3, &cur) == SPECTATOR_RING_COLD_START);

    /* The exact edge (delay reaches the keyframe) is watchable. */
    UT_ASSERT(spectatorRingSeekDelayed(r, 2, &cur) == SPECTATOR_RING_OK);

    spectatorRingDestroy(r);
    return 0;
}

/* ---- retention window boundary ---- */

static int t_retention_boundary(void) {
    SpectatorRing *r = spectatorRingCreate(3, 5);
    uint32_t gt;
    uint32_t head;
    uint32_t oldest;
    const uint8_t *p;
    int len;
    SpectatorRingCursor cur;

    UT_ASSERT(r != NULL);

    /* One long segment, gameTick == recordSeq. Keyframes recur on the cadence
     * (every 3 ticks), so an aligned eviction always leaves a keyframe oldest. */
    for (gt = 0; gt <= 12; gt++) {
        bool kf = spectatorRingNeedsKeyframe(r, gt);
        UT_ASSERT(spectatorRingRecordTick(r, gt, kf, NULL, 0) == true);
    }

    head = spectatorRingHeadSeq(r);
    oldest = spectatorRingOldestSeq(r);
    UT_ASSERT(head == 12);
    UT_ASSERT(oldest > 0); /* retention has evicted the early records */

    /* Just inside the window: target == oldest -> OK, seeded at the oldest
     * record, which is therefore a keyframe (gameTick == recordSeq here). */
    UT_ASSERT(spectatorRingSeekDelayed(r, head - oldest, &cur) ==
              SPECTATOR_RING_OK);
    p = spectatorRingCursorKeyframe(&cur, &len, &gt);
    (void)p;
    UT_ASSERT(gt == oldest); /* seed is the oldest record => it is a keyframe */

    /* Just outside the window: one older -> AGED_OUT (history existed, dropped). */
    UT_ASSERT(spectatorRingSeekDelayed(r, head - oldest + 1, &cur) ==
              SPECTATOR_RING_AGED_OUT);

    spectatorRingDestroy(r);
    return 0;
}

/* ============================================================
 * Real-sim cases: a live ServerSim drives the ring through log.c's
 * dormant tap (logSetSpectatorRing). Keyframes are sampled byte-exact
 * against an independent logSerializeSnapshotBody oracle, but only at a
 * settled state: the keys half-step that runs after logWriteTick within
 * serverSimTick mutates tank state, so a between-ticks oracle equals the
 * tap keyframe only when the world is at rest. sr_quiesce_kf drives
 * zero-button ticks until two consecutive head-keyframe snapshots are
 * identical (the settled guard), guaranteeing the keys half-step is a
 * no-op at the sample point.
 * ============================================================ */

#define SR_SLOT      0
#define SR_CADENCE   8       /* keyframe every 8 game ticks within a segment */
#define SR_FWD_CADENCE 40    /* wide cadence so a motion burst stays between two
                              * keyframes (forward-event ordering case) */
#define SR_FWD_TICKS   8     /* motion ticks driven there (< SR_FWD_CADENCE/2
                              * records, so no new keyframe is recorded) */

static void sr_tmp(char *out, size_t n, const char *tag) {
    snprintf(out, n, "test_spectator_ring_%s.wbv", tag);
}

static void sr_feed(ServerSim *sim, uint32_t tick, uint8_t buttons) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = SR_SLOT;
    pkt.buttons   = buttons;
    serverSimApplyInput(sim, &pkt);
}

static void sr_tick_idle(ServerSim *sim, uint32_t *it) {
    /* Two fresh zero-button inputs per frame keeps the input stream
       established (matches the jitter-buffer test cadence) while commanding
       no motion, so a moving tank decelerates to rest. */
    sr_feed(sim, (*it)++, 0);
    sr_feed(sim, (*it)++, 0);
    serverSimTick(sim);
}

static ServerSim *sr_bringup(const char *fname) {
    ServerSim *sim = ut_make_running_sim("Tester");
    if (sim == NULL) {
        return NULL;
    }
    logCreate();
    if (logStart((char *)fname, sim, 0, MAX_TANKS, FALSE) == FALSE) {
        logDestroy();
        serverSimDestroy(sim);
        return NULL;
    }
    logWriteTick(); /* pin the owner thread, matching production bring-up */
    return sim;
}

static void sr_teardown(ServerSim *sim, SpectatorRing *ring, const char *fname) {
    logSetSpectatorRing(NULL, NULL);
    logStop();
    logDestroy();
    if (ring != NULL) {
        spectatorRingDestroy(ring);
    }
    if (sim != NULL) {
        serverSimDestroy(sim);
    }
    remove(fname);
}

/* Drive idle ticks until the head record is a keyframe whose snapshot body
   matches the previous head-keyframe sample (two consecutive identical = the
   world is at rest). On success fills out (>= LOG_SNAPSHOT_BODY_MAX) with that
   settled body and returns its length; -1 if it never settles. After this the
   ring head is a settled keyframe equal to out. */
static int sr_quiesce_kf(ServerSim *sim, SpectatorRing *ring, uint32_t *it,
                         uint8_t *out) {
    uint8_t *prev = (uint8_t *)malloc(LOG_SNAPSHOT_BODY_MAX);
    int prevLen = -1;
    int curLen;
    int i;
    SpectatorRingCursor cur;

    if (prev == NULL) {
        return -1;
    }
    for (i = 0; i < 800; i++) {
        sr_tick_idle(sim, it);
        if (spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK &&
            cur.seedIdx == cur.targetIdx) {
            curLen = logSerializeSnapshotBody(sim, out, LOG_SNAPSHOT_BODY_MAX);
            if (curLen < 0) {
                break;
            }
            if (prevLen == curLen && memcmp(prev, out, (size_t)curLen) == 0) {
                free(prev);
                return curLen;
            }
            memcpy(prev, out, (size_t)curLen);
            prevLen = curLen;
        }
    }
    free(prev);
    return -1;
}

static void sr_drive_motion(ServerSim *sim, uint32_t *it, int ticks,
                            uint8_t buttons) {
    int m;
    for (m = 0; m < ticks; m++) {
        sr_feed(sim, (*it)++, buttons);
        sr_feed(sim, (*it)++, buttons);
        serverSimTick(sim);
    }
}

/* The settled keyframe the tap records equals an independent snapshot-body
   oracle, before and after the world is evolved by real motion. */
static int t_sim_keyframe_oracle(void) {
    char fname[64];
    ServerSim *sim;
    SpectatorRing *ring = NULL;
    uint32_t it = 1;
    uint8_t *o0 = NULL, *o1 = NULL;
    int o0len, o1len, klen;
    const uint8_t *kf;
    SpectatorRingCursor cur;

    sr_tmp(fname, sizeof(fname), "kf");
    remove(fname);
    sim = sr_bringup(fname);
    UT_ASSERT_MSG(sim != NULL, "bringup failed");
    ring = spectatorRingCreate(SR_CADENCE, 100000);
    UT_ASSERT(ring != NULL);
    logSetSpectatorRing(ring, sim);
    o0 = (uint8_t *)malloc(LOG_SNAPSHOT_BODY_MAX);
    o1 = (uint8_t *)malloc(LOG_SNAPSHOT_BODY_MAX);
    UT_ASSERT(o0 != NULL && o1 != NULL);

    o0len = sr_quiesce_kf(sim, ring, &it, o0);
    UT_ASSERT_MSG(o0len > 0, "initial state never settled");
    UT_ASSERT(spectatorRingSegmentCount(ring) == 1);

    UT_ASSERT(spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK);
    kf = spectatorRingCursorKeyframe(&cur, &klen, NULL);
    UT_ASSERT_MSG(klen == o0len && memcmp(kf, o0, (size_t)klen) == 0,
                  "settled keyframe != oracle (len %d vs %d)", klen, o0len);

    /* Evolve the world with real motion, then resettle. */
    sr_drive_motion(sim, &it, 16, INPUT_BTN_ACCEL | INPUT_BTN_LEFT);
    o1len = sr_quiesce_kf(sim, ring, &it, o1);
    UT_ASSERT_MSG(o1len > 0, "post-motion state never settled");
    UT_ASSERT_MSG(!(o1len == o0len && memcmp(o0, o1, (size_t)o0len) == 0),
                  "snapshot did not change across motion");

    UT_ASSERT(spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK);
    kf = spectatorRingCursorKeyframe(&cur, &klen, NULL);
    UT_ASSERT_MSG(klen == o1len && memcmp(kf, o1, (size_t)klen) == 0,
                  "evolved keyframe != oracle");

    free(o0);
    free(o1);
    sr_teardown(sim, ring, fname);
    return 0;
}

/* A real serverSimResetGameWorld opens a new segment; the new segment seeds
   from its own keyframe, and a delayed read still in the old segment seeds
   from the old segment's keyframe (its tail survived the reset). */
static int t_sim_reset_segmentation(void) {
    char fname[64];
    ServerSim *sim;
    SpectatorRing *ring = NULL;
    uint32_t it = 1;
    uint8_t *oA = NULL, *oB = NULL;
    int oAlen, oBlen, klen;
    uint32_t oldHead, newHead;
    const uint8_t *kf;
    SpectatorRingCursor cur;

    sr_tmp(fname, sizeof(fname), "reset");
    remove(fname);
    sim = sr_bringup(fname);
    UT_ASSERT_MSG(sim != NULL, "bringup failed");
    ring = spectatorRingCreate(SR_CADENCE, 100000);
    UT_ASSERT(ring != NULL);
    logSetSpectatorRing(ring, sim);
    oA = (uint8_t *)malloc(LOG_SNAPSHOT_BODY_MAX);
    oB = (uint8_t *)malloc(LOG_SNAPSHOT_BODY_MAX);
    UT_ASSERT(oA != NULL && oB != NULL);

    /* Segment A: evolve with motion, then settle on a keyframe. */
    sr_drive_motion(sim, &it, 16, INPUT_BTN_ACCEL);
    oAlen = sr_quiesce_kf(sim, ring, &it, oA);
    UT_ASSERT_MSG(oAlen > 0, "segment A never settled");
    UT_ASSERT(spectatorRingSegmentCount(ring) == 1);
    oldHead = spectatorRingHeadSeq(ring); /* a settled segment-A keyframe */

    UT_ASSERT(spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK);
    kf = spectatorRingCursorKeyframe(&cur, &klen, NULL);
    UT_ASSERT_MSG(klen == oAlen && memcmp(kf, oA, (size_t)klen) == 0,
                  "segment A keyframe != oracle");

    /* Real world reset — restarts the game tick at 0. */
    serverSimResetGameWorld(sim);
    oBlen = sr_quiesce_kf(sim, ring, &it, oB);
    UT_ASSERT_MSG(oBlen > 0, "segment B never settled");
    UT_ASSERT_MSG(spectatorRingSegmentCount(ring) == 2,
                  "reset did not open a new segment");
    newHead = spectatorRingHeadSeq(ring);

    UT_ASSERT(spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK);
    kf = spectatorRingCursorKeyframe(&cur, &klen, NULL);
    UT_ASSERT_MSG(klen == oBlen && memcmp(kf, oB, (size_t)klen) == 0,
                  "segment B keyframe != oracle");
    UT_ASSERT_MSG(!(oBlen == oAlen && memcmp(oA, oB, (size_t)oAlen) == 0),
                  "reset world identical to segment A world");

    /* Previous-generation read: a target still inside segment A seeds from
       segment A's keyframe == oA, proving the old tail survived the reset. */
    UT_ASSERT(newHead > oldHead);
    UT_ASSERT(spectatorRingSeekDelayed(ring, newHead - oldHead, &cur) ==
              SPECTATOR_RING_OK);
    kf = spectatorRingCursorKeyframe(&cur, &klen, NULL);
    UT_ASSERT_MSG(klen == oAlen && memcmp(kf, oA, (size_t)klen) == 0,
                  "previous-generation seed != segment A oracle");

    free(oA);
    free(oB);
    sr_teardown(sim, ring, fname);
    return 0;
}

/* A delay reaching before the first retained keyframe, with nothing aged out,
   reports COLD_START; delay 0 is watchable. */
static int t_sim_cold_start(void) {
    char fname[64];
    ServerSim *sim;
    SpectatorRing *ring = NULL;
    uint32_t it = 1;
    uint8_t *o = NULL;
    int olen;
    SpectatorRingCursor cur;

    sr_tmp(fname, sizeof(fname), "cold");
    remove(fname);
    sim = sr_bringup(fname);
    UT_ASSERT_MSG(sim != NULL, "bringup failed");
    ring = spectatorRingCreate(SR_CADENCE, 100000);
    UT_ASSERT(ring != NULL);
    logSetSpectatorRing(ring, sim);
    o = (uint8_t *)malloc(LOG_SNAPSHOT_BODY_MAX);
    UT_ASSERT(o != NULL);

    olen = sr_quiesce_kf(sim, ring, &it, o);
    UT_ASSERT_MSG(olen > 0, "state never settled");

    UT_ASSERT(spectatorRingSeekDelayed(ring, spectatorRingHeadSeq(ring) + 100,
                                       &cur) == SPECTATOR_RING_COLD_START);
    UT_ASSERT(spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK);

    free(o);
    sr_teardown(sim, ring, fname);
    return 0;
}

/* With a tight retention window, early frames age out: a target just inside
   the window is OK and seeds from the oldest survivor (which is a keyframe,
   here byte-equal to the idle settled oracle); one tick older is AGED_OUT. */
static int t_sim_retention_boundary(void) {
    char fname[64];
    ServerSim *sim;
    SpectatorRing *ring = NULL;
    uint32_t it = 1;
    uint8_t *o = NULL;
    int olen, klen, i;
    uint32_t head, oldest;
    const uint8_t *kf;
    SpectatorRingCursor cur;

    sr_tmp(fname, sizeof(fname), "ret");
    remove(fname);
    sim = sr_bringup(fname);
    UT_ASSERT_MSG(sim != NULL, "bringup failed");
    ring = spectatorRingCreate(SR_CADENCE, 16);
    UT_ASSERT(ring != NULL);
    logSetSpectatorRing(ring, sim);
    o = (uint8_t *)malloc(LOG_SNAPSHOT_BODY_MAX);
    UT_ASSERT(o != NULL);

    /* Idle throughout: every keyframe is the same settled world. */
    olen = sr_quiesce_kf(sim, ring, &it, o);
    UT_ASSERT_MSG(olen > 0, "state never settled");
    for (i = 0; i < 40; i++) {
        sr_tick_idle(sim, &it);
    }

    head = spectatorRingHeadSeq(ring);
    oldest = spectatorRingOldestSeq(ring);
    UT_ASSERT_MSG(oldest > 0, "retention did not evict early frames");

    UT_ASSERT(spectatorRingSeekDelayed(ring, head - oldest, &cur) ==
              SPECTATOR_RING_OK);
    kf = spectatorRingCursorKeyframe(&cur, &klen, NULL);
    UT_ASSERT_MSG(klen == olen && memcmp(kf, o, (size_t)klen) == 0,
                  "oldest survivor keyframe != settled oracle");

    UT_ASSERT(spectatorRingSeekDelayed(ring, head - oldest + 1, &cur) ==
              SPECTATOR_RING_AGED_OUT);

    free(o);
    sr_teardown(sim, ring, fname);
    return 0;
}

/* For a target a few ticks after a settled keyframe, the seed keyframe matches
   its oracle and the cursor yields one event record per intervening game tick
   in ascending gameTick, none crossing the segment. Motion (game-tick tank
   position deltas) gives at least one non-empty event record. */
static int t_sim_forward_events(void) {
    char fname[64];
    ServerSim *sim;
    SpectatorRing *ring = NULL;
    uint32_t it = 1;
    uint8_t *o = NULL;
    int olen, klen;
    uint32_t seedSeq, oldest;
    const uint8_t *kf;
    SpectatorRingCursor cur;
    int eplen;
    uint32_t egt;
    int nEvents = 0, nonEmpty = 0;
    uint32_t lastGt = 0;
    bool first = true;

    sr_tmp(fname, sizeof(fname), "fwd");
    remove(fname);
    sim = sr_bringup(fname);
    UT_ASSERT_MSG(sim != NULL, "bringup failed");
    ring = spectatorRingCreate(SR_FWD_CADENCE, 100000);
    UT_ASSERT(ring != NULL);
    logSetSpectatorRing(ring, sim);
    o = (uint8_t *)malloc(LOG_SNAPSHOT_BODY_MAX);
    UT_ASSERT(o != NULL);

    /* Settle so the head is a keyframe at rest; that is the seed. */
    olen = sr_quiesce_kf(sim, ring, &it, o);
    UT_ASSERT_MSG(olen > 0, "state never settled");
    seedSeq = spectatorRingHeadSeq(ring);

    /* Drive a motion burst shorter than half the cadence (in records), so no
       new keyframe is recorded and the pre-motion keyframe stays the seed.
       Accel + turn changes the logged tank direction/position each game tick,
       so the intervening event records carry real log_PlayerLocation bytes. */
    sr_drive_motion(sim, &it, SR_FWD_TICKS, INPUT_BTN_ACCEL | INPUT_BTN_LEFT);

    UT_ASSERT(spectatorRingSeekDelayed(ring, 0, &cur) == SPECTATOR_RING_OK);
    oldest = spectatorRingOldestSeq(ring);
    UT_ASSERT_MSG(oldest + (uint32_t)cur.seedIdx == seedSeq,
                  "seed drifted off the pre-motion keyframe");
    kf = spectatorRingCursorKeyframe(&cur, &klen, NULL);
    UT_ASSERT_MSG(klen == olen && memcmp(kf, o, (size_t)klen) == 0,
                  "seed keyframe != oracle after motion");

    while (spectatorRingCursorNextEvents(&cur, NULL, &eplen, &egt)) {
        if (!first) {
            UT_ASSERT_MSG(egt == lastGt + 2,
                          "event gameTick not strictly +2 ascending");
        }
        first = false;
        lastGt = egt;
        if (eplen > 0) {
            nonEmpty++;
        }
        nEvents++;
    }
    UT_ASSERT_MSG(nEvents == SR_FWD_TICKS,
                  "expected %d intervening event records, got %d",
                  SR_FWD_TICKS, nEvents);
    UT_ASSERT_MSG(nonEmpty >= 1, "no non-empty event record from motion");
    UT_ASSERT(spectatorRingSegmentCount(ring) == 1);

    free(o);
    sr_teardown(sim, ring, fname);
    return 0;
}

int run_spectator_ring(void) {
    if (t_segmentation()) {
        return 1;
    }
    if (t_keyframe_at_segment_start()) {
        return 1;
    }
    if (t_seek_ok_mid_interval()) {
        return 1;
    }
    if (t_segment_isolation()) {
        return 1;
    }
    if (t_previous_generation_read()) {
        return 1;
    }
    if (t_cold_start()) {
        return 1;
    }
    if (t_retention_boundary()) {
        return 1;
    }
    if (t_sim_keyframe_oracle()) {
        return 1;
    }
    if (t_sim_reset_segmentation()) {
        return 1;
    }
    if (t_sim_cold_start()) {
        return 1;
    }
    if (t_sim_retention_boundary()) {
        return 1;
    }
    if (t_sim_forward_events()) {
        return 1;
    }
    return 0;
}
