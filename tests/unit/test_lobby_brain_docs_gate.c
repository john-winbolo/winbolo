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
 * THE BRAINS' LOBBY TEXTS GO TO A JOINER, NOT INTO EVERY KEYFRAME.
 *
 * announce.txt and commands.txt travel to a client as
 * CTRL_LOBBY_BRAIN_DOCS_CHUNK fragments, emitted from inside
 * serverSimSyncSubscriber. That function has TWO callers:
 *
 *   serverSimRegisterSubscriber   — a client or spectator joining, once
 *   serverSimSerializeControlSnapshot — the delayed spectator ring's
 *                                       control snapshot, rebuilt on EVERY
 *                                       lobby keyframe
 *
 * The second is the problem. WinBoloDS defaults to 16 spectator slots and the
 * log writer runs in lobby state, so the docs were re-read off disk on the
 * tick thread at the keyframe rate — and, worse, the fragments themselves
 * pushed the snapshot past LOG_CONTROL_SNAPSHOT_MAX, at which point
 * serverSimSerializeControlSnapshot returns -1 and log.c drops the whole
 * keyframe without a word.
 *
 * The two cases below pin both halves:
 *
 *   lobby_brain_docs_stay_out_of_the_control_snapshot — every brain the
 *       catalogue holds, each shipping a full-size commands.txt. The snapshot
 *       must serialize (not overflow) and must carry no docs record — and the
 *       test says by how
 *       much the docs would have overrun the cap, so the number is on the
 *       record rather than taken on trust.
 *
 *   lobby_brain_docs_reach_a_joining_subscriber — the same brains reach
 *       a registering subscriber, in full, reassembling to the bytes on disk.
 *
 * The brains are written into the test's own scratch directory and pointed at
 * through sim->brainPaths, so the case does not depend on which brains happen
 * to be installed beside the binary.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "brain_list.h"
#include "control_event.h"
#include "everard_map.h"
#include "log_internal.h"          /* serverSimSerializeControlSnapshot,
                                    * LOG_CONTROL_SNAPSHOT_MAX */
#include "server_sim.h"
#include "server_sim_internal.h"   /* brainList / brainPaths — the catalogue
                                    * the fixture writes by hand */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "threads.h"
#include "test_harness.h"

/* Every brain the catalogue holds, each shipping full-size docs: sixteen of
 * them come to around 265 KB against a cap of under 200 KB. Nine was the
 * count measured on a real server, and nine full-size docs are no longer
 * enough on their own to clear a cap that counts the scenario markers and
 * score rows as well. */
#define BD_BRAINS BRAIN_LIST_MAX

/* Each brain's announce line, and a commands.txt at the cap the wire carries.
 * Full size on purpose: that is what makes the brains overrun
 * LOG_CONTROL_SNAPSHOT_MAX, which is the failure the gate exists to stop, and
 * a brain shipping a real command reference is not a small file. The docs are
 * filled with a per-brain letter so a reassembled blob can be told from its
 * neighbour's. */
#define BD_DOCS_BYTES BRAIN_DOCS_MAX

typedef struct {
    ServerSim *sim;
    char       dir[BD_BRAINS][512];
    char       announce[BD_BRAINS][64];
} BdFixture;

static bool bdWriteFile(const char *path, const char *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    if (n > 0 && fwrite(bytes, 1, n, f) != n) { fclose(f); return false; }
    fclose(f);
    return true;
}

/* A lobby-state sim whose brain catalogue is BD_BRAINS brains written into
 * this test's scratch directory, each with an announce.txt and a large
 * commands.txt, with the docs cache filled from them. */
static int bdSetup(BdFixture *f) {
    BYTE  emap[6000] = E_MAP;
    char *docs;
    int   i;

    memset(f, 0, sizeof(*f));
    f->sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                       gameOpen, false, 0, -1);
    if (f->sim == NULL) {
        fprintf(stderr, "serverSimCreateCompressed returned NULL\n");
        return 1;
    }
    serverSimSetLobbyEnabled(f->sim, true);
    serverSimAddPlayer(f->sim, 0, "Host", false);

    docs = (char *)malloc(BD_DOCS_BYTES);
    if (docs == NULL) return 1;

    for (i = 0; i < BD_BRAINS; i++) {
        char leaf[64];
        char path[600];

        snprintf(leaf, sizeof(leaf), "brain%d", i);
        if (!utScratchPath(f->dir[i], sizeof(f->dir[i]), leaf)) {
            fprintf(stderr, "utScratchPath failed\n");
            free(docs);
            return 1;
        }
        /* utScratchPath makes the directory above the leaf; the leaf itself
         * is a directory here, so make it. */
        SDL_CreateDirectory(f->dir[i]);

        snprintf(f->announce[i], sizeof(f->announce[i]),
                 "Brain %d reporting for duty", i);
        snprintf(path, sizeof(path), "%s/announce.txt", f->dir[i]);
        if (!bdWriteFile(path, f->announce[i], strlen(f->announce[i]))) {
            fprintf(stderr, "could not write %s\n", path);
            free(docs);
            return 1;
        }
        memset(docs, 'a' + (i % 26), BD_DOCS_BYTES);
        snprintf(path, sizeof(path), "%s/commands.txt", f->dir[i]);
        if (!bdWriteFile(path, docs, BD_DOCS_BYTES)) {
            fprintf(stderr, "could not write %s\n", path);
            free(docs);
            return 1;
        }

        /* The catalogue the server would have built by scanning brains/.
         * brainPaths holds the brain's init.lua; the texts sit beside it. */
        snprintf(f->sim->brainPaths[i], BRAIN_LIST_PATH_LEN, "%s/init.lua",
                 f->dir[i]);
        snprintf(f->sim->brainList.entries[i].name, BRAIN_LIST_NAME_LEN,
                 "TestBrain%d", i);
    }
    f->sim->brainList.count = BD_BRAINS;
    free(docs);

    serverSimRefreshBrainDocs(f->sim);
    return 0;
}

static void bdTeardown(BdFixture *f) {
    if (f->sim) serverSimDestroy(f->sim);
}

/* One subscriber that counts the docs fragments it is handed and keeps the
 * bytes of brain 0's stream so the test can check what arrived. */
typedef struct {
    int      chunks;
    int      wireBytes;             /* what the same events cost a snapshot */
    bool     sawBrain[BD_BRAINS];
    uint8_t  blob[LOBBY_BRAIN_DOCS_WIRE_MAX];
    int      blobLen;
    bool     blobIsBrain0;
} BdSink;

static void bdDeliver(void *ctx, const struct ControlEvent *evt) {
    BdSink *s = (BdSink *)ctx;
    if (evt->type != CTRL_LOBBY_BRAIN_DOCS_CHUNK) return;
    s->chunks++;
    /* What this event would add to a control snapshot: the 4-byte record
     * header plus the encoded body, whose fragment payload is fragLen. The
     * body also carries brainIdx, seq, count and the length itself. */
    s->wireBytes += 4 + 5 + (int)evt->u.lobbyBrainDocsChunk.fragLen;
    if (evt->u.lobbyBrainDocsChunk.brainIdx < BD_BRAINS) {
        s->sawBrain[evt->u.lobbyBrainDocsChunk.brainIdx] = true;
    }
    if (evt->u.lobbyBrainDocsChunk.brainIdx == 0) {
        uint16_t fl = evt->u.lobbyBrainDocsChunk.fragLen;
        if (evt->u.lobbyBrainDocsChunk.seq == 0) s->blobLen = 0;
        if (s->blobLen + (int)fl <= (int)sizeof(s->blob)) {
            memcpy(s->blob + s->blobLen, evt->u.lobbyBrainDocsChunk.frag, fl);
            s->blobLen += (int)fl;
        }
        s->blobIsBrain0 = true;
    }
}

/* Walk the [u16 type][u16 bodyLen][body] records a control snapshot is made
 * of, counting the docs ones. False means the framing did not add up. */
static bool bdCountDocsRecords(const uint8_t *buf, int len, int *outDocs,
                               int *outRecords) {
    int pos = 0;
    *outDocs = 0;
    *outRecords = 0;
    while (pos + 4 <= len) {
        uint16_t type    = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
        uint16_t bodyLen = (uint16_t)((buf[pos + 2] << 8) | buf[pos + 3]);
        pos += 4;
        if (pos + (int)bodyLen > len) return false;
        pos += bodyLen;
        (*outRecords)++;
        if (type == (uint16_t)CTRL_LOBBY_BRAIN_DOCS_CHUNK) (*outDocs)++;
    }
    return (pos == len);
}

int run_lobby_brain_docs_stay_out_of_the_control_snapshot(void) {
    BdFixture f;
    BdSink   *sink = NULL;
    uint8_t  *snap = NULL;
    int       snapLen, docsRecords, records;
    SubscriberHandle h;

    if (!threadsCreate(TRUE)) UT_FAIL("threadsCreate failed");
    if (bdSetup(&f) != 0) return 1;

    /* First, what the docs would have cost. A subscriber gets them, so the
       same events measured there are what the snapshot was carrying. */
    sink = (BdSink *)calloc(1, sizeof(*sink));
    UT_ASSERT(sink != NULL);
    h = serverSimRegisterSubscriber(f.sim, bdDeliver, sink);
    UT_ASSERT_MSG(h != SUBSCRIBER_HANDLE_INVALID, "could not register");
    serverSimUnregisterSubscriber(f.sim, h);
    UT_ASSERT_MSG(sink->chunks > 0,
                  "the joining subscriber was sent no docs at all, so this "
                  "case cannot say anything about the snapshot");

    snap = (uint8_t *)malloc(LOG_CONTROL_SNAPSHOT_MAX);
    UT_ASSERT(snap != NULL);
    snapLen = serverSimSerializeControlSnapshot(f.sim, snap,
                                                LOG_CONTROL_SNAPSHOT_MAX);
    UT_ASSERT_MSG(snapLen >= 0,
                  "the lobby control snapshot overflowed %d bytes and was "
                  "refused — log.c drops the whole spectator keyframe when "
                  "this happens, silently", LOG_CONTROL_SNAPSHOT_MAX);

    UT_ASSERT_MSG(bdCountDocsRecords(snap, snapLen, &docsRecords, &records),
                  "the snapshot's record framing did not add up over %d bytes",
                  snapLen);
    UT_ASSERT_MSG(docsRecords == 0,
                  "the control snapshot carries %d CTRL_LOBBY_BRAIN_DOCS_CHUNK "
                  "record(s) out of %d. It is rebuilt on every lobby keyframe, "
                  "so the brains' texts have no business in it",
                  docsRecords, records);

    /* And the number that makes the gate worth having: with the docs in, the
       snapshot would have been this far over the cap. */
    UT_ASSERT_MSG(snapLen + sink->wireBytes > LOG_CONTROL_SNAPSHOT_MAX,
                  "%d brains' docs are only %d bytes on top of a %d-byte "
                  "snapshot, which still fits %d — the fixture is too small to "
                  "be testing the overflow it claims to",
                  BD_BRAINS, sink->wireBytes, snapLen,
                  LOG_CONTROL_SNAPSHOT_MAX);

    free(snap);
    free(sink);
    bdTeardown(&f);
    threadsDestroy();
    return 0;
}

int run_lobby_brain_docs_reach_a_joining_subscriber(void) {
    BdFixture f;
    BdSink   *sink = NULL;
    SubscriberHandle h;
    int       i;
    uint16_t  aLen, dLen;

    if (!threadsCreate(TRUE)) UT_FAIL("threadsCreate failed");
    if (bdSetup(&f) != 0) return 1;

    sink = (BdSink *)calloc(1, sizeof(*sink));
    UT_ASSERT(sink != NULL);
    h = serverSimRegisterSubscriber(f.sim, bdDeliver, sink);
    UT_ASSERT_MSG(h != SUBSCRIBER_HANDLE_INVALID, "could not register");
    serverSimUnregisterSubscriber(f.sim, h);

    for (i = 0; i < BD_BRAINS; i++) {
        UT_ASSERT_MSG(sink->sawBrain[i],
                      "brain %d shipped an announce.txt and a commands.txt "
                      "and the joiner was sent neither", i);
    }

    /* Brain 0's stream reassembles to what is on disk: [u16 aLen][announce]
       [u16 dLen][docs]. The cache is built once at scan time, so this is also
       the check that it holds the right bytes. */
    UT_ASSERT_MSG(sink->blobIsBrain0 && sink->blobLen >= 4,
                  "brain 0's fragments did not arrive (%d bytes)",
                  sink->blobLen);
    aLen = (uint16_t)((sink->blob[0] << 8) | sink->blob[1]);
    UT_ASSERT_MSG((int)aLen == (int)strlen(f.announce[0]),
                  "announce is %u bytes, expected %d",
                  (unsigned)aLen, (int)strlen(f.announce[0]));
    UT_ASSERT_MSG(memcmp(sink->blob + 2, f.announce[0], aLen) == 0,
                  "brain 0's announce text did not survive the trip");
    dLen = (uint16_t)((sink->blob[2 + aLen] << 8) | sink->blob[3 + aLen]);
    UT_ASSERT_MSG(dLen == (uint16_t)BD_DOCS_BYTES,
                  "commands.txt arrived as %u bytes, expected %d",
                  (unsigned)dLen, BD_DOCS_BYTES);
    UT_ASSERT_MSG(sink->blob[4 + aLen] == 'a',
                  "brain 0's docs body is brain %d's",
                  (int)(sink->blob[4 + aLen] - 'a'));

    free(sink);
    bdTeardown(&f);
    threadsDestroy();
    return 0;
}
