/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record_body.c
 *PURPOSE:       The per-bot entry body of brainrec.btr, shared by the
 *               server's recorder and the Bot Host's, and the Bot Host's
 *               .hbr files (see brain_record_host.h).
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_record.h"
#include "brain_record_host.h"

/* ---- Buffer ------------------------------------------------------------- */

void brainRecBufInit(BrainRecBuf *b) {
    memset(b, 0, sizeof(*b));
}

void brainRecBufFree(BrainRecBuf *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

void brainRecBufReset(BrainRecBuf *b) {
    b->len = 0;
    b->failed = false;
}

bool brainRecBufPut(BrainRecBuf *b, const void *p, size_t n) {
    if (b->failed) return false;
    if (n == 0) return true;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        uint8_t *d;
        while (cap < b->len + n) cap *= 2;
        d = (uint8_t *)realloc(b->data, cap);
        if (d == NULL) {
            b->failed = true;
            return false;
        }
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}

static void putU8(BrainRecBuf *b, uint8_t v)   { brainRecBufPut(b, &v, 1); }
static void putU16(BrainRecBuf *b, uint16_t v) { brainRecBufPut(b, &v, 2); }
static void putU32(BrainRecBuf *b, uint32_t v) { brainRecBufPut(b, &v, 4); }
static void putF32(BrainRecBuf *b, float v)    { brainRecBufPut(b, &v, 4); }

/* ---- Body --------------------------------------------------------------- */

/* Goal head: kind[32] + mx + my + target_id + substate[32]. */
#define BODY_GOAL_HEAD (32 + 12 + 32)

static void putOverlay(BrainRecBuf *b, const OverlayCmd *c) {
    uint8_t tl = 0;
    putU8(b, (uint8_t)c->type);
    putF32(b, c->x1); putF32(b, c->y1); putF32(b, c->x2); putF32(b, c->y2);
    putF32(b, c->radius);
    putU8(b, c->r); putU8(b, c->g); putU8(b, c->b); putU8(b, c->a);
    putU8(b, c->anchor);
    putU8(b, c->viz_idx);
    while (tl < OVERLAY_TEXT_MAX && c->text[tl] != '\0') tl++;
    putU8(b, tl);
    if (tl) brainRecBufPut(b, c->text, tl);
}

uint32_t brainRecBodyEncode(BrainRecBuf *b, float thinkMs,
                            const BrainGoalInfo *gi,
                            const OverlayCmdBuffer *ovl, const bool *skip,
                            const char *poolJson) {
    uint32_t kept = 0;
    putF32(b, thinkMs);

    if (gi == NULL) {
        char empty[32];
        memset(empty, 0, sizeof empty);
        brainRecBufPut(b, empty, 32);
        putU32(b, 0); putU32(b, 0); putU32(b, 0);
        brainRecBufPut(b, empty, 32);
        putU16(b, 0);
    } else {
        int nc = gi->num_candidates, i;
        brainRecBufPut(b, gi->kind, 32);
        putU32(b, (uint32_t)gi->mx);
        putU32(b, (uint32_t)gi->my);
        putU32(b, (uint32_t)gi->target_id);
        brainRecBufPut(b, gi->substate, 32);
        if (nc < 0) nc = 0;
        if (nc > BRAIN_GOAL_MAX_CANDIDATES) nc = BRAIN_GOAL_MAX_CANDIDATES;
        putU16(b, (uint16_t)nc);
        for (i = 0; i < nc; i++) {
            BrainRecCandidate c;
            memset(&c, 0, sizeof c);
            memcpy(c.desc, gi->candidates[i].desc, sizeof c.desc);
            c.desc[sizeof c.desc - 1] = '\0';
            c.cost = gi->candidates[i].cost;
            c.winner = gi->candidates[i].winner ? 1 : 0;
            c.phase_weight = gi->candidates[i].phase_weight;
            brainRecBufPut(b, &c, sizeof c);
        }
    }

    if (ovl == NULL || ovl->count <= 0) {
        putU32(b, 0);
    } else {
        uint32_t n = (uint32_t)ovl->count, k;
        for (k = 0; k < n; k++) {
            if (skip == NULL || !skip[ovl->cmds[k].viz_idx]) kept++;
        }
        putU32(b, kept);
        for (k = 0; k < n; k++) {
            if (skip != NULL && skip[ovl->cmds[k].viz_idx]) continue;
            putOverlay(b, &ovl->cmds[k]);
        }
    }

    {
        uint32_t pl = (poolJson != NULL) ? (uint32_t)strlen(poolJson) : 0u;
        putU32(b, pl);
        if (pl) brainRecBufPut(b, poolJson, pl);
    }
    return kept;
}

void brainRecBodyEncodeHosted(BrainRecBuf *b, uint16_t placeGen,
                              const char *hostName, uint32_t lastInput) {
    char kind[32], sub[32];
    memset(kind, 0, sizeof kind);
    memset(sub, 0, sizeof sub);
    memcpy(kind, BRAINREC_HOSTED_KIND, sizeof(BRAINREC_HOSTED_KIND));
    if (hostName != NULL) {
        strncpy(sub, hostName, sizeof sub - 1);
    }
    putF32(b, -1.0f);
    brainRecBufPut(b, kind, 32);
    putU32(b, (uint32_t)placeGen);
    putU32(b, BRAINREC_HOSTED_TAG);
    putU32(b, lastInput);
    brainRecBufPut(b, sub, 32);
    putU16(b, 0);   /* no candidates */
    putU32(b, 0);   /* no overlays */
    putU32(b, 0);   /* no pool */
}

bool brainRecBodyIsHosted(const uint8_t *body, size_t len,
                          uint16_t *placeGen, uint32_t *lastInput,
                          char *hostOut) {
    uint32_t mx, my, tid;
    if (body == NULL || len < 4 + BODY_GOAL_HEAD + 2 + 4 + 4) return false;
    if (memcmp(body + 4, BRAINREC_HOSTED_KIND, sizeof(BRAINREC_HOSTED_KIND)) != 0) {
        return false;
    }
    memcpy(&mx, body + 4 + 32, 4);
    memcpy(&my, body + 4 + 36, 4);
    memcpy(&tid, body + 4 + 40, 4);
    if (my != BRAINREC_HOSTED_TAG || mx > 0xFFFFu) return false;
    if (placeGen) *placeGen = (uint16_t)mx;
    if (lastInput) *lastInput = tid;
    if (hostOut) {
        memcpy(hostOut, body + 4 + 44, 32);
        hostOut[31] = '\0';
    }
    return true;
}

size_t brainRecBodyMeasure(const uint8_t *p, size_t avail) {
    size_t pos = 0;
    uint16_t nc;
    uint32_t oc, k, pl;

    if (avail < 4 + BODY_GOAL_HEAD + 2) return 0;
    pos = 4 + BODY_GOAL_HEAD;
    memcpy(&nc, p + pos, 2);
    pos += 2;
    if ((size_t)nc * sizeof(BrainRecCandidate) > avail - pos) return 0;
    pos += (size_t)nc * sizeof(BrainRecCandidate);
    if (avail - pos < 4) return 0;
    memcpy(&oc, p + pos, 4);
    pos += 4;
    for (k = 0; k < oc; k++) {
        uint8_t tl;
        if (avail - pos < 28) return 0;
        tl = p[pos + 27];
        pos += 28;
        if (avail - pos < tl) return 0;
        pos += tl;
    }
    if (avail - pos < 4) return 0;
    memcpy(&pl, p + pos, 4);
    pos += 4;
    if (avail - pos < pl) return 0;
    pos += pl;
    return pos;
}

size_t brainRecBodyDropPool(uint8_t *p, size_t len) {
    uint32_t zero = 0;
    size_t pos = 4 + BODY_GOAL_HEAD;
    uint16_t nc;
    uint32_t oc, k;
    /* Only a whole body is changed. */
    if (brainRecBodyMeasure(p, len) != len) return len;
    /* Walk to the pool length field, the last field before the JSON. */
    memcpy(&nc, p + pos, 2);
    pos += 2 + (size_t)nc * sizeof(BrainRecCandidate);
    memcpy(&oc, p + pos, 4);
    pos += 4;
    for (k = 0; k < oc; k++) pos += 28 + p[pos + 27];
    memcpy(p + pos, &zero, 4);
    return pos + 4;
}

/* ---- .hbr files --------------------------------------------------------- */

void hostRecFileName(char *out, size_t sz, uint8_t slot, uint16_t placeGen) {
    snprintf(out, sz, "bot%u_g%u%s", (unsigned)slot, (unsigned)placeGen,
             HOSTREC_EXT);
}

bool hostRecWriterOpen(HostRecWriter *w, const char *path,
                       const HostRecHeader *hdr, const char *legend) {
    BrainRecFile *f;
    HostRecHeader h = *hdr;
    uint32_t ll = legend ? (uint32_t)strlen(legend) : 0u;

    memset(w, 0, sizeof(*w));
    memcpy(h.magic, HOSTREC_MAGIC, sizeof(HOSTREC_MAGIC));
    h.version = HOSTREC_VERSION;
    h.brainrecVersion = BRAINREC_VERSION;
    f = brainRecFileOpenWrite(path);
    if (f == NULL) return false;
    if (!brainRecFileWrite(f, &h, sizeof h) ||
        !brainRecFileWrite(f, &ll, 4) ||
        (ll && !brainRecFileWrite(f, legend, ll))) {
        (void)brainRecFileClose(f);
        return false;
    }
    (void)brainRecFileFlush(f);
    w->file = f;
    return true;
}

bool hostRecWriterAppend(HostRecWriter *w, uint32_t seq, uint32_t estTick,
                         const uint8_t *body, size_t len) {
    uint32_t head[4];
    BrainRecFile *f = w->file;
    if (f == NULL) return false;
    head[0] = HOSTREC_RECORD_MAGIC;
    head[1] = seq;
    head[2] = estTick;
    head[3] = (uint32_t)len;
    if (!brainRecFileWrite(f, head, sizeof head)) return false;
    if (len && !brainRecFileWrite(f, body, len)) return false;
    w->records++;
    w->rawBytes += sizeof head + len;
    /* Same flush rhythm as the server's recorder: a killed host loses
     * about a second. */
    if (++w->sinceFlush >= 32) {
        (void)brainRecFileFlush(f);
        w->sinceFlush = 0;
    }
    return true;
}

void hostRecWriterClose(HostRecWriter *w) {
    (void)brainRecFileClose(w->file);
    w->file = NULL;
}

bool hostRecReaderOpen(HostRecReader *r, const char *path) {
    BrainRecFile *f;
    uint32_t ll = 0;
    memset(r, 0, sizeof(*r));
    f = brainRecFileOpenRead(path);
    if (f == NULL) return false;
    if (!brainRecFileReadExact(f, &r->hdr, sizeof r->hdr) ||
        memcmp(r->hdr.magic, HOSTREC_MAGIC, sizeof(HOSTREC_MAGIC)) != 0 ||
        r->hdr.version != HOSTREC_VERSION ||
        r->hdr.brainrecVersion != BRAINREC_VERSION ||
        !brainRecFileReadExact(f, &ll, 4) || ll > (16u << 20)) {
        (void)brainRecFileClose(f);
        return false;
    }
    r->hdr.hostName[HOSTREC_HOST_LEN - 1] = '\0';
    r->hdr.block[HOSTREC_BLOCK_LEN - 1] = '\0';
    r->legend = (char *)malloc((size_t)ll + 1);
    if (r->legend == NULL || (ll && !brainRecFileReadExact(f, r->legend, ll))) {
        free(r->legend);
        r->legend = NULL;
        (void)brainRecFileClose(f);
        return false;
    }
    r->legend[ll] = '\0';
    r->file = f;
    return true;
}

int hostRecReaderNext(HostRecReader *r, uint32_t *seq, uint32_t *estTick,
                      BrainRecBuf *body) {
    uint32_t head[4];
    BrainRecFile *f = r->file;
    if (f == NULL) return 0;
    /* A short read, or a read error at a cut-short tail, is the end. */
    if (!brainRecFileReadExact(f, head, sizeof head)) return 0;
    if (head[0] != HOSTREC_RECORD_MAGIC || head[3] > (64u << 20)) return -1;
    brainRecBufReset(body);
    if (head[3] > 0) {
        if (body->cap < head[3]) {
            uint8_t *d = (uint8_t *)realloc(body->data, head[3]);
            if (d == NULL) return -1;
            body->data = d;
            body->cap = head[3];
        }
        if (!brainRecFileReadExact(f, body->data, head[3])) return 0;
    }
    body->len = head[3];
    if (brainRecBodyMeasure(body->data, body->len) != body->len) return -1;
    if (seq) *seq = head[1];
    if (estTick) *estTick = head[2];
    return 1;
}

void hostRecReaderClose(HostRecReader *r) {
    (void)brainRecFileClose(r->file);
    free(r->legend);
    memset(r, 0, sizeof(*r));
}
