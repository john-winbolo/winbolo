/*
 * Golden-fixture generator for the flat-leaf-snapshot codecs. Run on demand,
 * not part of the fast suite: it stands up a running-mode loopback session,
 * collects the bytes the server's hand-rolled packers emit via the
 * WIRE_CORPUS_TAP hook, and writes a handful of distinct instances per message
 * to <WB_FIXTURE_DIR>/<label>.hex (one lowercase-hex line each).
 *
 * The committed output is what test_wire_corpus checks the generated codecs
 * against. Regenerate with, from the repo root:
 *
 *   WB_FIXTURE_DIR=tests/fixtures/wire \
 *       ./WinBoloUnitTests --test wire_corpus_capture
 *
 * then commit the changed tests/fixtures/wire/ *.hex.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loopback_harness.h"
#include "wire_corpus.h"
#include "test_harness.h"

#ifndef WB_WIRE_FIXTURE_DIR
#define WB_WIRE_FIXTURE_DIR "tests/fixtures/wire"
#endif

#define CAP_BYTES          64
#define CAP_MAX_LABELS     16
#define CAP_PER_LABEL      4    /* distinct instances kept per message */
#define CAP_PUMP_ITERS     400  /* enough for join + several running ticks */

typedef struct {
    char    label[32];
    uint8_t inst[CAP_PER_LABEL][CAP_BYTES];
    size_t  len[CAP_PER_LABEL];
    int     count;
} LabelBucket;

/* Append (bytes,len) to the bucket for `label`, ignoring exact duplicates and
 * stopping at CAP_PER_LABEL. Returns the bucket count or grows the table. */
static void bucketAdd(LabelBucket *buckets, int *nBuckets, const char *label,
                      const uint8_t *bytes, size_t len) {
    int b, j;
    LabelBucket *bk = NULL;
    for (b = 0; b < *nBuckets; b++) {
        if (strcmp(buckets[b].label, label) == 0) { bk = &buckets[b]; break; }
    }
    if (!bk) {
        if (*nBuckets >= CAP_MAX_LABELS) return;
        bk = &buckets[*nBuckets];
        memset(bk, 0, sizeof(*bk));
        snprintf(bk->label, sizeof(bk->label), "%s", label);
        (*nBuckets)++;
    }
    if (bk->count >= CAP_PER_LABEL || len > CAP_BYTES) return;
    for (j = 0; j < bk->count; j++) {
        if (bk->len[j] == len && memcmp(bk->inst[j], bytes, len) == 0) return;
    }
    memcpy(bk->inst[bk->count], bytes, len);
    bk->len[bk->count] = len;
    bk->count++;
}

static int writeBucket(const char *dir, const LabelBucket *bk) {
    char path[512];
    FILE *f;
    int i;
    size_t j;
    snprintf(path, sizeof(path), "%s/%s.hex", dir, bk->label);
    f = fopen(path, "w");
    if (!f) return -1;
    for (i = 0; i < bk->count; i++) {
        for (j = 0; j < bk->len[i]; j++) {
            fprintf(f, "%02x", bk->inst[i][j]);
        }
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

int run_wire_corpus_capture(void) {
    LoopbackHarness h;
    LabelBucket buckets[CAP_MAX_LABELS];
    int nBuckets = 0;
    const char *dir = getenv("WB_FIXTURE_DIR");
    size_t i, total;
    int b;

    if (dir == NULL || dir[0] == '\0') dir = WB_WIRE_FIXTURE_DIR;

    memset(&h, 0, sizeof(h));
    wireCorpusReset();
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "cap", false, NULL, 1234),
                  "loopback start failed");
    loopbackHarnessPumpUntil(&h, CAP_PUMP_ITERS, NULL, NULL);
    wireCorpusStop();

    total = wireCorpusCount();
    for (i = 0; i < total; i++) {
        size_t len = 0;
        const uint8_t *bytes = wireCorpusBytesAt(i, &len);
        bucketAdd(buckets, &nBuckets, wireCorpusLabelAt(i), bytes, len);
    }
    loopbackHarnessStop(&h);

    fprintf(stderr, "wire_corpus_capture: %zu tapped, %d labels -> %s\n",
            total, nBuckets, dir);
    for (b = 0; b < nBuckets; b++) {
        UT_ASSERT_MSG(writeBucket(dir, &buckets[b]) == 0,
                      "fixture write failed");
        fprintf(stderr, "  %-24s %d instance(s)\n",
                buckets[b].label, buckets[b].count);
    }
    UT_ASSERT_MSG(nBuckets > 0, "session emitted no flat leaf snapshots");
    return 0;
}
