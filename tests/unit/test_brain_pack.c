/*
 * The packed Bot Host brain sync (brain_pack.c, and the v2 manifest and
 * packed plan of brain_sync.c), and the session folders of the Bot Host
 * inside a client.
 *
 *   1. brain_pack_manifest_v2: the v2 form keeps every SHA-256 through the
 *      wire; any changed byte is refused; the v1 form decodes without them.
 *   2. brain_pack_all_equal: a host whose install holds the same brain runs
 *      it in place and asks for nothing.
 *   3. brain_pack_one_changed: with one file changed on the server only
 *      that file is asked for; it comes through the stream, is checked and
 *      committed into the version folder, and the install folder is
 *      byte-identical to before.
 *   4. brain_pack_bad_hash: a file whose bytes do not match the manifest is
 *      refused, and a damaged stream ends as bad.
 *   5. brain_pack_old_peer: a v1 manifest has no SHA-256; the server sends
 *      v1 to a host with no caps after the wait, and waits before it; the
 *      caps and pack request messages keep their fields on the wire.
 *   6. brain_pack_session: a session folder is gone after its end; a
 *      leftover marked folder is removed by a sweep only once its lock is
 *      free; a folder without the marker and the recordings are kept.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "brain_pack.h"
#include "brain_sync.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "test_harness.h"

static bool bp_write(const char *dir, const char *rel, const char *text) {
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s/%s", dir, rel);
    return brainSyncWriteFile(path, text, strlen(text));
}

static bool bp_exists(const char *path) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path, &pi);
}

/* The brain "TestBrain" in <dir>, with initText as its init.lua. No opt/
 * folder: outside debug mode a brain with one is sent as its opt/ files. */
static bool bp_fill_brain(const char *dir, const char *initText) {
    return bp_write(dir, "init.lua", initText) &&
           bp_write(dir, "lib/util.lua", "return { n = 1 }\n") &&
           bp_write(dir, "lib/deep/more.lua", "return 42\n");
}

/* A v2 manifest (not in debug mode) of the brain in dir. */
static bool bp_collect(const char *dir, BrainSyncManifest *m) {
    memset(m, 0, sizeof(*m));
    m->debugMode = false;
    m->haveSha = true;
    if (!brainSyncCollectBrain(&m->brains[0], "TestBrain", dir, false, NULL)) {
        return false;
    }
    m->brainCount = 1;
    brainSyncComputeVersions(m);
    return true;
}

/* Every file under dir, read into one buffer as "path\0size\0bytes"... in
 * the manifest's sorted order, for a byte-for-byte compare. */
static uint8_t *bp_snapshot(const char *dir, size_t *outLen) {
    BrainSyncBrain b;
    uint8_t *out = NULL;
    size_t len = 0;
    int i;
    memset(&b, 0, sizeof(b));
    if (!brainSyncCollectBrain(&b, "TestBrain", dir, true, NULL)) return NULL;
    for (i = 0; i < b.fileCount; i++) {
        char path[1100];
        size_t sz = 0, pl = strlen(b.files[i].path) + 1;
        void *data;
        uint8_t *grown;
        SDL_snprintf(path, sizeof(path), "%s/%s", dir, b.files[i].path);
        data = SDL_LoadFile(path, &sz);
        grown = (uint8_t *)realloc(out, len + pl + 8 + sz + 1);
        if (grown == NULL) { SDL_free(data); free(out); free(b.files); return NULL; }
        out = grown;
        memcpy(out + len, b.files[i].path, pl);
        len += pl;
        {
            uint64_t sz64 = (uint64_t)sz;
            memcpy(out + len, &sz64, sizeof(sz64));
        }
        len += 8;
        if (sz > 0) memcpy(out + len, data, sz);
        len += sz;
        SDL_free(data);
    }
    free(b.files);
    *outLen = len;
    return out;
}

/* ================================================================
 * 1. v2 manifest
 * ================================================================ */
int run_brain_pack_manifest_v2(void) {
    BrainSyncManifest m, d;
    char dir[1024];
    uint8_t *blob, *blob1, *bad;
    size_t len = 0, len1 = 0, i;
    int f;

    UT_ASSERT(utScratchPath(dir, sizeof(dir), "TestBrain"));
    UT_ASSERT(bp_fill_brain(dir, "print('v2')\n"));
    UT_ASSERT(bp_collect(dir, &m));

    blob = brainSyncManifestEncodeV2(&m, &len);
    UT_ASSERT(blob != NULL);
    UT_ASSERT(brainSyncManifestDecode(blob, len, &d));
    UT_ASSERT(d.haveSha);
    UT_ASSERT(d.version == m.version);
    UT_ASSERT(d.brains[0].version == m.brains[0].version);
    UT_ASSERT(d.brains[0].fileCount == m.brains[0].fileCount);
    for (f = 0; f < d.brains[0].fileCount; f++) {
        UT_ASSERT(strcmp(d.brains[0].files[f].path,
                         m.brains[0].files[f].path) == 0);
        UT_ASSERT(d.brains[0].files[f].hash == m.brains[0].files[f].hash);
        UT_ASSERT(memcmp(d.brains[0].files[f].sha, m.brains[0].files[f].sha,
                         BRAIN_SYNC_SHA_LEN) == 0);
    }
    brainSyncFree(&d);

    bad = (uint8_t *)malloc(len);
    UT_ASSERT(bad != NULL);
    for (i = 0; i < len; i++) {
        memcpy(bad, blob, len);
        bad[i] ^= 0x01;
        UT_ASSERT_MSG(!brainSyncManifestDecode(bad, len, &d),
                      "a change at byte %u decoded", (unsigned)i);
    }
    free(bad);

    /* The v1 form of the same list: the same versions, no SHA-256. */
    blob1 = brainSyncManifestEncode(&m, &len1);
    UT_ASSERT(blob1 != NULL && len1 < len);
    UT_ASSERT(brainSyncManifestDecode(blob1, len1, &d));
    UT_ASSERT(!d.haveSha);
    UT_ASSERT(d.version == m.version);
    brainSyncFree(&d);
    free(blob1);
    free(blob);
    brainSyncFree(&m);
    return 0;
}

/* ================================================================
 * 2. All files equal: nothing sent
 * ================================================================ */
int run_brain_pack_all_equal(void) {
    BrainSyncManifest m;
    BrainSyncPlan p;
    char inst[1024], brainDir[1100], cache[1024];
    uint8_t bits[BRAIN_PACK_BITMAP_BYTES];

    UT_ASSERT(utScratchPath(inst, sizeof(inst), "install"));
    SDL_snprintf(brainDir, sizeof(brainDir), "%s/TestBrain", inst);
    UT_ASSERT(bp_fill_brain(brainDir, "print('same')\n"));
    UT_ASSERT(utScratchPath(cache, sizeof(cache), "cache"));
    /* The server's copy is the same brain. */
    UT_ASSERT(bp_collect(brainDir, &m));

    UT_ASSERT(brainSyncPlanPacked(&m, cache, inst, &p));
    UT_ASSERT(p.inPlace[0]);
    UT_ASSERT(!p.staged[0]);
    UT_ASSERT_MSG(p.needCount == 0, "need %d", p.needCount);
    UT_ASSERT(p.installCount == m.brains[0].fileCount);
    UT_ASSERT(p.installOf[0] == m.brains[0].fileCount);
    brainPackBitsFromPlan(&p, 0, bits);
    UT_ASSERT(brainPackBitCount(bits, m.brains[0].fileCount) == 0);
    /* Nothing was made in the cache. */
    {
        char stage[1100], ver[1100];
        UT_ASSERT(brainSyncStageDir(stage, sizeof(stage), cache, "TestBrain",
                                    m.brains[0].version));
        UT_ASSERT(brainSyncCacheDir(ver, sizeof(ver), cache, "TestBrain",
                                    m.brains[0].version));
        UT_ASSERT(!bp_exists(stage));
        UT_ASSERT(!bp_exists(ver));
    }
    free(p.need);

    /* In debug mode the brain may write into its folder, so it never runs
     * from the install: every file is copied into the stage instead, and
     * still nothing is sent. */
    m.debugMode = true;
    UT_ASSERT(brainSyncPlanPacked(&m, cache, inst, &p));
    UT_ASSERT(!p.inPlace[0]);
    UT_ASSERT(p.staged[0]);
    UT_ASSERT(p.needCount == 0);
    UT_ASSERT(p.installCount == m.brains[0].fileCount);
    UT_ASSERT(brainSyncCommitStage(cache, &m.brains[0]));
    free(p.need);
    UT_ASSERT(brainSyncPlanPacked(&m, cache, inst, &p));
    UT_ASSERT(p.haveCount == m.brains[0].fileCount && p.needCount == 0);
    free(p.need);
    brainSyncFree(&m);
    return 0;
}

/* ================================================================
 * 3. One changed file: only that file, install unchanged
 * ================================================================ */
typedef struct {
    const BrainSyncBrain *br;
    const char *stage;
    int written;
    int refused;
} BpSink;

static bool bp_sink(void *ctx, int file, const uint8_t *data, uint32_t size) {
    BpSink *s = (BpSink *)ctx;
    const BrainSyncFile *f = &s->br->files[file];
    char path[1400];
    if (!brainSyncDataIs(f, data, size)) {
        s->refused++;
        return false;
    }
    SDL_snprintf(path, sizeof(path), "%s/%s", s->stage, f->path);
    if (!brainSyncWriteFile(path, data, size)) return false;
    s->written++;
    return true;
}

/* Pack the files of bits from br's folder, as the server does. */
static uint8_t *bp_pack(const BrainSyncBrain *br, const uint8_t *bits,
                        size_t *outLen) {
    BrainPackWriter *w;
    uint8_t *out = NULL;
    size_t len = 0;
    int i, n = brainPackBitCount(bits, br->fileCount);
    w = brainPackWriterNew(BRAIN_PACK_LEVEL, 0, (uint16_t)n);
    if (w == NULL) return NULL;
    for (i = 0; i < br->fileCount; i++) {
        char path[1100];
        size_t sz = 0;
        void *data;
        if (!brainPackBitGet(bits, i)) continue;
        SDL_snprintf(path, sizeof(path), "%s/%s", br->dir, br->files[i].path);
        data = SDL_LoadFile(path, &sz);
        if (!brainPackWriterBeginFile(w, (uint16_t)i, (uint32_t)sz) ||
            (sz > 0 && !brainPackWriterPut(w, data, sz))) {
            SDL_free(data);
            brainPackWriterFree(w);
            return NULL;
        }
        SDL_free(data);
    }
    if (!brainPackWriterFinish(w)) {
        brainPackWriterFree(w);
        return NULL;
    }
    while (brainPackWriterPending(w) > 0) {
        size_t k = brainPackWriterPending(w);
        uint8_t *grown = (uint8_t *)realloc(out, len + k);
        if (grown == NULL) { free(out); brainPackWriterFree(w); return NULL; }
        out = grown;
        len += brainPackWriterTake(w, out + len, k);
    }
    if (!brainPackWriterDone(w)) {
        free(out);
        out = NULL;
    }
    brainPackWriterFree(w);
    *outLen = len;
    return out;
}

int run_brain_pack_one_changed(void) {
    BrainSyncManifest m;
    BrainSyncPlan p;
    BrainPackReader *r;
    BpSink sink;
    char inst[1024], instBrain[1100], srv[1024], cache[1024], stage[1100];
    char ver[1100];
    uint8_t bits[BRAIN_PACK_BITMAP_BYTES];
    uint8_t *before, *after, *packed;
    size_t beforeLen = 0, afterLen = 0, packedLen = 0, half;
    BrainPackStatus st;
    int i;

    UT_ASSERT(utScratchPath(inst, sizeof(inst), "install"));
    SDL_snprintf(instBrain, sizeof(instBrain), "%s/TestBrain", inst);
    UT_ASSERT(bp_fill_brain(instBrain, "print('release')\n"));
    UT_ASSERT(utScratchPath(srv, sizeof(srv), "server/TestBrain"));
    UT_ASSERT(bp_fill_brain(srv, "print('custom server brain')\n"));
    UT_ASSERT(utScratchPath(cache, sizeof(cache), "cache"));
    before = bp_snapshot(instBrain, &beforeLen);
    UT_ASSERT(before != NULL);

    UT_ASSERT(bp_collect(srv, &m));
    UT_ASSERT(brainSyncPlanPacked(&m, cache, inst, &p));
    UT_ASSERT(!p.inPlace[0]);
    UT_ASSERT_MSG(p.needCount == 1, "need %d", p.needCount);
    UT_ASSERT(strcmp(m.brains[0].files[p.need[0] & 0xffffu].path,
                     "init.lua") == 0);
    UT_ASSERT(p.installOf[0] == m.brains[0].fileCount - 1);
    UT_ASSERT(p.staged[0]);
    brainPackBitsFromPlan(&p, 0, bits);
    UT_ASSERT(brainPackBitCount(bits, m.brains[0].fileCount) == 1);

    packed = bp_pack(&m.brains[0], bits, &packedLen);
    UT_ASSERT(packed != NULL && packedLen > 0);

    UT_ASSERT(brainSyncStageDir(stage, sizeof(stage), cache, "TestBrain",
                                m.brains[0].version));
    memset(&sink, 0, sizeof(sink));
    sink.br = &m.brains[0];
    sink.stage = stage;
    r = brainPackReaderNew(&m.brains[0], 0, bits);
    UT_ASSERT(r != NULL);
    /* In two pieces, as from two bulk transfers. */
    half = packedLen / 2;
    st = brainPackReaderFeed(r, packed, half, bp_sink, &sink);
    UT_ASSERT(st == BRAIN_PACK_MORE);
    st = brainPackReaderFeed(r, packed + half, packedLen - half, bp_sink, &sink);
    UT_ASSERT_MSG(st == BRAIN_PACK_DONE, "status %d", (int)st);
    UT_ASSERT(sink.written == 1 && sink.refused == 0);
    brainPackReaderFree(r);
    free(packed);

    /* Not yet a version folder: a half transfer leaves none. */
    UT_ASSERT(brainSyncCacheDir(ver, sizeof(ver), cache, "TestBrain",
                                m.brains[0].version));
    UT_ASSERT(!bp_exists(ver));
    UT_ASSERT(brainSyncCommitStage(cache, &m.brains[0]));
    UT_ASSERT(bp_exists(ver));
    UT_ASSERT(!bp_exists(stage));
    for (i = 0; i < m.brains[0].fileCount; i++) {
        char path[1400];
        UT_ASSERT(brainSyncCacheFile(path, sizeof(path), cache, "TestBrain",
                                     m.brains[0].version,
                                     m.brains[0].files[i].path));
        UT_ASSERT_MSG(brainSyncFileIs(path, &m.brains[0].files[i], true),
                      "%s", m.brains[0].files[i].path);
    }
    free(p.need);

    /* Next time every file is held in the cache. */
    UT_ASSERT(brainSyncPlanPacked(&m, cache, inst, &p));
    UT_ASSERT(p.needCount == 0 && p.haveCount == m.brains[0].fileCount);
    UT_ASSERT(p.cacheOf[0] == m.brains[0].fileCount);
    free(p.need);

    /* The install folder is byte-identical to before. */
    after = bp_snapshot(instBrain, &afterLen);
    UT_ASSERT(after != NULL);
    UT_ASSERT(afterLen == beforeLen && memcmp(after, before, beforeLen) == 0);
    free(after);
    free(before);
    brainSyncFree(&m);
    return 0;
}

/* ================================================================
 * 4. A bad hash is refused
 * ================================================================ */
int run_brain_pack_bad_hash(void) {
    BrainSyncManifest m;
    BrainPackReader *r;
    BpSink sink;
    char srv[1024], stage[1024];
    uint8_t bits[BRAIN_PACK_BITMAP_BYTES];
    uint8_t *packed;
    size_t packedLen = 0;
    BrainPackStatus st;
    int i;

    UT_ASSERT(utScratchPath(srv, sizeof(srv), "server/TestBrain"));
    UT_ASSERT(bp_fill_brain(srv, "print('good')\n"));
    UT_ASSERT(utScratchPath(stage, sizeof(stage), "stage"));
    UT_ASSERT(bp_collect(srv, &m));

    /* The bytes match their size, but not the manifest's hash and sha. */
    memset(bits, 0, sizeof(bits));
    for (i = 0; i < m.brains[0].fileCount; i++) brainPackBitSet(bits, i);
    UT_ASSERT(bp_write(srv, "init.lua", "print('evil')\n"));
    packed = bp_pack(&m.brains[0], bits, &packedLen);
    UT_ASSERT(packed != NULL);
    memset(&sink, 0, sizeof(sink));
    sink.br = &m.brains[0];
    sink.stage = stage;
    r = brainPackReaderNew(&m.brains[0], 0, bits);
    UT_ASSERT(r != NULL);
    st = brainPackReaderFeed(r, packed, packedLen, bp_sink, &sink);
    UT_ASSERT(st == BRAIN_PACK_DONE);
    UT_ASSERT_MSG(sink.refused == 1 && sink.written == m.brains[0].fileCount - 1,
                  "refused %d written %d", sink.refused, sink.written);
    {
        char path[1100];
        SDL_snprintf(path, sizeof(path), "%s/init.lua", stage);
        UT_ASSERT(!bp_exists(path));
    }
    brainPackReaderFree(r);

    /* A damaged stream ends as bad. */
    packed[packedLen / 2] ^= 0x5a;
    memset(&sink, 0, sizeof(sink));
    sink.br = &m.brains[0];
    sink.stage = stage;
    r = brainPackReaderNew(&m.brains[0], 0, bits);
    st = brainPackReaderFeed(r, packed, packedLen, bp_sink, &sink);
    UT_ASSERT(st == BRAIN_PACK_BAD);
    brainPackReaderFree(r);
    free(packed);

    /* A stream with a file not asked for is bad. */
    {
        uint8_t one[BRAIN_PACK_BITMAP_BYTES];
        memset(one, 0, sizeof(one));
        brainPackBitSet(one, 1);
        packed = bp_pack(&m.brains[0], bits, &packedLen);
        UT_ASSERT(packed != NULL);
        r = brainPackReaderNew(&m.brains[0], 0, one);
        st = brainPackReaderFeed(r, packed, packedLen, NULL, NULL);
        UT_ASSERT(st == BRAIN_PACK_BAD);
        brainPackReaderFree(r);
        free(packed);
    }
    brainSyncFree(&m);
    return 0;
}

/* ================================================================
 * 5. Old peers keep the old way
 * ================================================================ */
int run_brain_pack_old_peer(void) {
    ControlEvent e, d;
    uint8_t buf[1024];
    size_t len = 0;
    ControlEncodeBodyFn enc;
    ControlDecodeBodyFn dec;

    /* No caps yet: the server waits a little, then sends v1. */
    UT_ASSERT(brainPackManifestChoice(false, 0, 0) == BRAIN_PACK_MANIFEST_WAIT);
    UT_ASSERT(brainPackManifestChoice(false, 0, BRAIN_PACK_CAPS_WAIT_TICKS - 1)
              == BRAIN_PACK_MANIFEST_WAIT);
    UT_ASSERT(brainPackManifestChoice(false, 0, BRAIN_PACK_CAPS_WAIT_TICKS)
              == BRAIN_PACK_MANIFEST_V1);
    UT_ASSERT(brainPackManifestChoice(true, 0, 0) == BRAIN_PACK_MANIFEST_V1);
    UT_ASSERT(brainPackManifestChoice(true, BRAIN_PACK_CAP_PACKED, 0)
              == BRAIN_PACK_MANIFEST_V2);

    /* The caps message. */
    memset(&e, 0, sizeof(e));
    e.type = CTRL_BOT_HOST_SYNC_CAPS;
    e.u.botHostSyncCaps.caps = BRAIN_PACK_CAP_PACKED;
    enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_SYNC_CAPS);
    dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_SYNC_CAPS);
    UT_ASSERT(enc != NULL && dec != NULL);
    UT_ASSERT(enc(&e, NULL, buf, sizeof(buf), &len) == ENCODE_OK && len == 1);
    UT_ASSERT(dec(buf, len, &d));
    UT_ASSERT(d.type == CTRL_BOT_HOST_SYNC_CAPS);
    UT_ASSERT(d.u.botHostSyncCaps.caps == BRAIN_PACK_CAP_PACKED);
    UT_ASSERT(!dec(buf, 0, &d));

    /* The pack request: 125 files (the GoalHunter count), 3 asked for. */
    memset(&e, 0, sizeof(e));
    e.type = CTRL_BOT_HOST_PACK_REQ;
    e.u.botHostPackReq.gen = 0x01020304u;
    e.u.botHostPackReq.brain = 2;
    e.u.botHostPackReq.fileCount = 125;
    e.u.botHostPackReq.fromInstall = 100;
    e.u.botHostPackReq.fromCache = 22;
    brainPackBitSet(e.u.botHostPackReq.bits, 0);
    brainPackBitSet(e.u.botHostPackReq.bits, 64);
    brainPackBitSet(e.u.botHostPackReq.bits, 124);
    enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_PACK_REQ);
    dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_PACK_REQ);
    UT_ASSERT(enc != NULL && dec != NULL);
    UT_ASSERT(enc(&e, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT_MSG(len == 11 + 16, "len %u", (unsigned)len);
    UT_ASSERT(dec(buf, len, &d));
    UT_ASSERT(d.type == CTRL_BOT_HOST_PACK_REQ);
    UT_ASSERT(d.u.botHostPackReq.gen == 0x01020304u);
    UT_ASSERT(d.u.botHostPackReq.brain == 2);
    UT_ASSERT(d.u.botHostPackReq.fileCount == 125);
    UT_ASSERT(d.u.botHostPackReq.fromInstall == 100);
    UT_ASSERT(d.u.botHostPackReq.fromCache == 22);
    UT_ASSERT(brainPackBitCount(d.u.botHostPackReq.bits, 125) == 3);
    UT_ASSERT(brainPackBitGet(d.u.botHostPackReq.bits, 124));
    UT_ASSERT(!dec(buf, len - 1, &d));
    /* A file count out of range is refused both ways. */
    e.u.botHostPackReq.fileCount = 0;
    UT_ASSERT(enc(&e, NULL, buf, sizeof(buf), &len) != ENCODE_OK);
    return 0;
}

/* ================================================================
 * 6. Session folders
 * ================================================================ */
int run_brain_pack_session(void) {
    BrainSession s, keep;
    char root[1024], plain[1100], rec[1100], held[600];
    int n;

    UT_ASSERT(utScratchPath(root, sizeof(root), "bothost_cache"));
    SDL_snprintf(rec, sizeof(rec), "%s/recordings/game.wbv", root);
    UT_ASSERT(brainSyncWriteFile(rec, "recording", 9));
    /* A folder under session/ with a session-like name but no marker. */
    SDL_snprintf(plain, sizeof(plain), "%s/session/0123456789abcdef/x.txt",
                 root);
    UT_ASSERT(brainSyncWriteFile(plain, "mine", 4));

    /* A session's folder is gone after its end (a leave). */
    UT_ASSERT(brainSessionCreate(root, &s));
    UT_ASSERT(bp_exists(s.dir));
    UT_ASSERT(bp_write(s.dir, "TestBrain/0000000000000001/init.lua", "x"));
    SDL_strlcpy(held, s.dir, sizeof(held));
    UT_ASSERT(brainSessionEnd(root, &s));
    UT_ASSERT(!bp_exists(held));

    /* A session another running client holds is kept by a sweep. */
    UT_ASSERT(brainSessionCreate(root, &keep));
    UT_ASSERT(bp_write(keep.dir, "TestBrain/0000000000000001/init.lua", "x"));
    n = brainSessionSweep(root);
    UT_ASSERT_MSG(n == 0, "removed %d", n);
    UT_ASSERT(bp_exists(keep.dir));

    /* Its program ends without clean-up (a crash): the lock is free, and
     * the next sweep removes it. */
    SDL_strlcpy(held, keep.dir, sizeof(held));
    brainSessionUnlock(&keep);
    n = brainSessionSweep(root);
    UT_ASSERT_MSG(n == 1, "removed %d", n);
    UT_ASSERT(!bp_exists(held));

    /* The folder without the marker and the recordings are kept. */
    UT_ASSERT(bp_exists(plain));
    UT_ASSERT(bp_exists(rec));
    /* An end on a path that is not a session folder removes nothing. */
    brainSessionInit(&s);
    SDL_snprintf(s.dir, sizeof(s.dir), "%s/recordings", root);
    UT_ASSERT(!brainSessionEnd(root, &s));
    UT_ASSERT(bp_exists(rec));
    return 0;
}
