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
 * THE BRAINS' ANNOUNCE LINES GO TO A JOINER; THEIR DOCS DO NOT.
 *
 * announce.txt travels to a client as one CTRL_LOBBY_BRAIN_ANNOUNCE per brain,
 * emitted from inside serverSimSyncSubscriber. commands.txt does not travel
 * with it: the event carries its length and generation, and the text goes
 * compressed on CHANNEL_BULK to a client that asks for it. Sixteen brains'
 * docs in the join burst were what CHANNEL_CONTROL_BACKLOG had been sized
 * for, and every joiner paid for them whether or not it opened one.
 *
 * serverSimSyncSubscriber has TWO callers:
 *
 *   serverSimRegisterSubscriber   — a client or spectator joining, once
 *   serverSimSerializeControlSnapshot — the delayed spectator ring's
 *                                       control snapshot, rebuilt on EVERY
 *                                       lobby keyframe
 *
 * The announces belong to the first and not the second. The cases below pin:
 *
 *   lobby_brain_docs_stay_out_of_the_control_snapshot — every brain the
 *       catalogue holds, each shipping a full-size commands.txt. The snapshot
 *       must serialize and carry no brain texts record of either type.
 *
 *   lobby_brain_docs_reach_a_joining_subscriber — a registering subscriber is
 *       sent every brain's announce line, and the length and generation of
 *       its docs, but none of the docs text; and the docs the server holds
 *       inflate to the bytes on disk.
 *
 *   lobby_brain_docs_refresh_moves_the_generation — a brain whose
 *       commands.txt changed is given a new generation by the refresh, and a
 *       brain whose files did not change keeps its own.
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

/* Every brain the catalogue holds. */
#define BD_BRAINS BRAIN_LIST_MAX

/* Each brain's announce line, and a commands.txt at the cap the wire carries:
 * a brain shipping a real command reference is not a small file, and the
 * point of the join path is that its size no longer reaches the joiner. The
 * docs are filled with a per-brain letter so one brain's text can be told
 * from its neighbour's. */
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
    f->sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
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

/* One subscriber that records the brain texts events it is handed. */
typedef struct {
    int      announces;
    int      chunks;                /* the retired type: must stay 0 */
    int      bodyBytes;             /* what the brain events cost the join */
    bool     sawBrain[BD_BRAINS];
    char     announce[BD_BRAINS][BRAIN_ANNOUNCE_MAX + 1];
    uint32_t gen[BD_BRAINS];
    uint16_t docsLen[BD_BRAINS];
} BdSink;

static void bdDeliver(void *ctx, const struct ControlEvent *evt) {
    BdSink *s = (BdSink *)ctx;
    uint8_t idx;
    if (evt->type == CTRL_LOBBY_BRAIN_DOCS_CHUNK) s->chunks++;
    if (evt->type != CTRL_LOBBY_BRAIN_ANNOUNCE) return;
    s->announces++;
    /* The encoded body: 9 fixed bytes and the announce text. */
    s->bodyBytes += 9 + (int)evt->u.lobbyBrainAnnounce.announceLen;
    idx = evt->u.lobbyBrainAnnounce.brainIdx;
    if (idx < BD_BRAINS) {
        s->sawBrain[idx] = true;
        memcpy(s->announce[idx], evt->u.lobbyBrainAnnounce.announce,
               sizeof(s->announce[idx]));
        s->gen[idx]     = evt->u.lobbyBrainAnnounce.docsGen;
        s->docsLen[idx] = evt->u.lobbyBrainAnnounce.docsLen;
    }
}

/* Walk the [u16 type][u16 bodyLen][body] records a control snapshot is made
 * of, counting the brain texts ones. False means the framing did not add up. */
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
        if (type == (uint16_t)CTRL_LOBBY_BRAIN_DOCS_CHUNK ||
            type == (uint16_t)CTRL_LOBBY_BRAIN_ANNOUNCE) {
            (*outDocs)++;
        }
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

    /* A subscriber is sent the announces, so the snapshot has something to
       leave out. */
    sink = (BdSink *)calloc(1, sizeof(*sink));
    UT_ASSERT(sink != NULL);
    h = serverSimRegisterSubscriber(f.sim, bdDeliver, sink);
    UT_ASSERT_MSG(h != SUBSCRIBER_HANDLE_INVALID, "could not register");
    serverSimUnregisterSubscriber(f.sim, h);
    UT_ASSERT_MSG(sink->announces > 0,
                  "the joining subscriber was sent no announces at all, so "
                  "this case cannot say anything about the snapshot");

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
                  "the control snapshot carries %d brain texts record(s) out "
                  "of %d. It is rebuilt on every lobby keyframe, and "
                  "LOG_CONTROL_SNAPSHOT_MAX does not count them",
                  docsRecords, records);

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
    int       i, j;
    uint32_t  gen = 0;
    uint16_t  rawLen = 0, zLen = 0;
    const uint8_t *z = NULL;
    char     *docs = NULL;

    if (!threadsCreate(TRUE)) UT_FAIL("threadsCreate failed");
    if (bdSetup(&f) != 0) return 1;

    sink = (BdSink *)calloc(1, sizeof(*sink));
    UT_ASSERT(sink != NULL);
    h = serverSimRegisterSubscriber(f.sim, bdDeliver, sink);
    UT_ASSERT_MSG(h != SUBSCRIBER_HANDLE_INVALID, "could not register");
    serverSimUnregisterSubscriber(f.sim, h);

    UT_ASSERT_MSG(sink->chunks == 0,
                  "the joiner was sent %d retired docs chunk(s)", sink->chunks);
    for (i = 0; i < BD_BRAINS; i++) {
        UT_ASSERT_MSG(sink->sawBrain[i],
                      "brain %d shipped an announce.txt and a commands.txt "
                      "and the joiner was sent no announce for it", i);
        UT_ASSERT_MSG(strcmp(sink->announce[i], f.announce[i]) == 0,
                      "brain %d's announce arrived as '%s'", i,
                      sink->announce[i]);
        UT_ASSERT_MSG(sink->docsLen[i] == BD_DOCS_BYTES,
                      "brain %d's docs were announced as %u bytes, expected %d",
                      i, (unsigned)sink->docsLen[i], BD_DOCS_BYTES);
        UT_ASSERT_MSG(sink->gen[i] != 0,
                      "brain %d has docs and was announced at generation 0", i);
        for (j = 0; j < i; j++) {
            UT_ASSERT_MSG(sink->gen[i] != sink->gen[j],
                          "brains %d and %d share docs generation %u", j, i,
                          (unsigned)sink->gen[i]);
        }
    }

    /* None of the docs text is in the join: each announce is 9 bytes of
       numbers and its own line, where the retired chunks carried the whole
       commands.txt of every brain. */
    UT_ASSERT_MSG(sink->bodyBytes <= BD_BRAINS * (9 + 64),
                  "the brain events cost the join %d bytes of body for %d "
                  "brains with %d-byte docs; the docs are riding along",
                  sink->bodyBytes, BD_BRAINS, BD_DOCS_BYTES);

    /* The docs the server holds for brain 3 are what is on disk, at the
       generation the joiner was told. The cache is built once at scan time,
       so this is also the check that it holds the right bytes. */
    UT_ASSERT(serverSimGetBrainDocs(f.sim, 3, &gen, &rawLen, &z, &zLen));
    UT_ASSERT_MSG(gen == sink->gen[3], "the server holds generation %u, the "
                  "joiner was told %u", (unsigned)gen, (unsigned)sink->gen[3]);
    UT_ASSERT(rawLen == BD_DOCS_BYTES);
    UT_ASSERT_MSG(zLen < 1024, "%d bytes of one letter compressed to %u",
                  BD_DOCS_BYTES, (unsigned)zLen);
    docs = (char *)malloc(BD_DOCS_BYTES + 1);
    UT_ASSERT(docs != NULL);
    UT_ASSERT(brainDocsDecompress(z, zLen, rawLen, docs));
    UT_ASSERT_MSG(docs[0] == 'd' && docs[BD_DOCS_BYTES - 1] == 'd',
                  "brain 3's docs are some other brain's");

    /* An index that is not a brain has none. */
    UT_ASSERT(!serverSimGetBrainDocs(f.sim, BD_BRAINS, NULL, NULL, NULL, NULL));
    UT_ASSERT(!serverSimGetBrainDocs(f.sim, -1, NULL, NULL, NULL, NULL));

    free(docs);
    free(sink);
    bdTeardown(&f);
    threadsDestroy();
    return 0;
}

int run_lobby_brain_docs_refresh_moves_the_generation(void) {
    BdFixture f;
    uint32_t  gen0 = 0, gen1 = 0, gen0b = 0, gen1b = 0;
    uint16_t  len0b = 0;
    char      path[600];
    static const char kNew[] = "new commands";
    int64_t   before;
    int       tries;

    if (!threadsCreate(TRUE)) UT_FAIL("threadsCreate failed");
    if (bdSetup(&f) != 0) return 1;

    UT_ASSERT(serverSimGetBrainDocs(f.sim, 0, &gen0, NULL, NULL, NULL));
    UT_ASSERT(serverSimGetBrainDocs(f.sim, 1, &gen1, NULL, NULL, NULL));

    /* Rewrite brain 0's commands.txt. The cache keys on mtime, and a
       filesystem can keep whole seconds, so write until the file's time has
       moved past the one the cache read. */
    before = brainListTextsMtimeForPath(f.sim->brainPaths[0]);
    snprintf(path, sizeof(path), "%s/commands.txt", f.dir[0]);
    for (tries = 0; tries < 30; tries++) {
        FILE *fp = fopen(path, "wb");
        UT_ASSERT(fp != NULL);
        fwrite(kNew, 1, strlen(kNew), fp);
        fclose(fp);
        if (brainListTextsMtimeForPath(f.sim->brainPaths[0]) != before) break;
        SDL_Delay(100);
    }
    UT_ASSERT_MSG(brainListTextsMtimeForPath(f.sim->brainPaths[0]) != before,
                  "brain 0's commands.txt kept its modification time after "
                  "3 s of rewrites");

    serverSimRefreshBrainDocs(f.sim);
    UT_ASSERT(serverSimGetBrainDocs(f.sim, 0, &gen0b, &len0b, NULL, NULL));
    UT_ASSERT(serverSimGetBrainDocs(f.sim, 1, &gen1b, NULL, NULL, NULL));
    UT_ASSERT_MSG(len0b == strlen(kNew),
                  "the refresh did not re-read brain 0 (%u bytes)",
                  (unsigned)len0b);
    UT_ASSERT_MSG(gen0b != gen0 && gen0b != 0,
                  "brain 0's docs changed and kept generation %u",
                  (unsigned)gen0);
    UT_ASSERT_MSG(gen1b == gen1,
                  "brain 1's docs did not change and moved from generation "
                  "%u to %u, which would make every client fetch them again",
                  (unsigned)gen1, (unsigned)gen1b);

    bdTeardown(&f);
    threadsDestroy();
    return 0;
}
