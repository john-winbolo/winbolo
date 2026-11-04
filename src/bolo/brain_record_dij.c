/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record_dij.c
 *PURPOSE:       Encode / decode the recorded Dijkstra box
 *               (see brain_record_dij.h for the layout).
 *
 *  The writer and the reader go through the same helpers: the
 *  encoder writes a record, then applies those very bytes to its own
 *  memory with the reader's apply function. So the writer's memory is
 *  always the state a reader rebuilds, and the next diff is taken
 *  against exactly that.
 *********************************************************/
#include "brain_record_dij.h"

#include <stdlib.h>
#include <string.h>

/* 8-direction offsets of the pathfinder's dir_at (N, NE, E, SE, S, SW,
 * W, NW); bit 3 of dir_at is the parent's boat layer. */
static const int DIJ_DX8[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
static const int DIJ_DY8[8] = {-1, -1,  0,  1,  1,  1,  0, -1 };

/* ---- small helpers ---------------------------------------------------- */

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }

static bool costReached(float c) { return c < BRAINREC_DIJ_INF_LIMIT; }

static void slateCellsClear(BrainRecDijSlateBox *s) {
    for (int l = 0; l < 2; l++) {
        for (int i = 0; i < BRAINREC_DIJ_CELLS; i++) s->cost[l][i] = BRAINREC_DIJ_COST_INF;
        memset(s->dir[l], BRAINREC_DIJ_DIR_NONE, BRAINREC_DIJ_CELLS);
    }
}

static void slateClear(BrainRecDijSlateBox *s) {
    s->active = false;
    memset(&s->meta, 0, sizeof s->meta);
    slateCellsClear(s);
}

static void botClear(BrainRecDijBotBox *b) {
    b->present  = false;
    b->boxValid = false;
    b->onBoat   = false;
    b->cx = b->cy = 0;
    for (int s = 0; s < BRAINREC_DIJ_SLATES; s++) slateClear(&b->slates[s]);
}

void brainRecDijStateClear(BrainRecDijState *st) {
    if (!st) return;
    for (int i = 0; i < BRAINREC_DIJ_MAX_BOTS; i++) botClear(&st->bots[i]);
}

void brainRecDijBufFree(BrainRecDijBuf *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static bool bufReserve(BrainRecDijBuf *b, size_t more) {
    if (b->len + more <= b->cap) return true;
    size_t nc = b->cap ? b->cap * 2 : 1024;
    while (nc < b->len + more) nc *= 2;
    uint8_t *nd = (uint8_t *)realloc(b->data, nc);
    if (!nd) return false;
    b->data = nd;
    b->cap  = nc;
    return true;
}

static bool putBytes(BrainRecDijBuf *b, const void *p, size_t n) {
    if (!bufReserve(b, n)) return false;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}
static bool putU8 (BrainRecDijBuf *b, uint8_t v)  { return putBytes(b, &v, 1); }
static bool putU32(BrainRecDijBuf *b, uint32_t v) { return putBytes(b, &v, 4); }
static bool putI32(BrainRecDijBuf *b, int32_t v)  { return putBytes(b, &v, 4); }
static bool putF32(BrainRecDijBuf *b, float v)    { return putBytes(b, &v, 4); }

/* Read cursor with bounds checking: any short read sets ok = false. */
typedef struct {
    const uint8_t *p;
    size_t         n, pos;
    bool           ok;
} DijCur;

static void getBytes(DijCur *c, void *dst, size_t n) {
    if (!c->ok || c->pos + n > c->n) { c->ok = false; memset(dst, 0, n); return; }
    memcpy(dst, c->p + c->pos, n);
    c->pos += n;
}
static uint8_t  getU8 (DijCur *c) { uint8_t v;  getBytes(c, &v, 1); return v; }
static uint32_t getU32(DijCur *c) { uint32_t v; getBytes(c, &v, 4); return v; }
static int32_t  getI32(DijCur *c) { int32_t v;  getBytes(c, &v, 4); return v; }
static float    getF32(DijCur *c) { float v;    getBytes(c, &v, 4); return v; }

/* ---- shared apply steps (reader and writer) --------------------------- */

/* Start a bot's frame: move its box to the new centre. Cells that stay in
 * the box keep their values; cells that newly enter it are empty. A box
 * that is not valid now (or was not before) holds no cells. Slate metas
 * are kept either way. */
static void botBegin(BrainRecDijBotBox *b, bool boxValid, bool onBoat, int cx, int cy) {
    if (b->boxValid && boxValid && (b->cx != cx || b->cy != cy)) {
        int dx = cx - b->cx, dy = cy - b->cy;
        for (int s = 0; s < BRAINREC_DIJ_SLATES; s++) {
            BrainRecDijSlateBox *sl = &b->slates[s];
            float   oc[2][BRAINREC_DIJ_CELLS];
            uint8_t od[2][BRAINREC_DIJ_CELLS];
            memcpy(oc, sl->cost, sizeof oc);
            memcpy(od, sl->dir, sizeof od);
            slateCellsClear(sl);
            for (int j = 0; j < BRAINREC_DIJ_BOX; j++) {
                int oj = j + dy;
                if (oj < 0 || oj >= BRAINREC_DIJ_BOX) continue;
                for (int i = 0; i < BRAINREC_DIJ_BOX; i++) {
                    int oi = i + dx;
                    if (oi < 0 || oi >= BRAINREC_DIJ_BOX) continue;
                    int to = j * BRAINREC_DIJ_BOX + i, from = oj * BRAINREC_DIJ_BOX + oi;
                    for (int l = 0; l < 2; l++) {
                        sl->cost[l][to] = oc[l][from];
                        sl->dir[l][to]  = od[l][from];
                    }
                }
            }
        }
    } else if (!(b->boxValid && boxValid)) {
        for (int s = 0; s < BRAINREC_DIJ_SLATES; s++) slateCellsClear(&b->slates[s]);
    }
    b->present  = true;
    b->boxValid = boxValid;
    b->onBoat   = onBoat;
    b->cx = boxValid ? cx : 0;
    b->cy = boxValid ? cy : 0;
}

static void readMeta(DijCur *c, BrainRecDijMeta *m) {
    m->kind            = getI32(c);
    m->src_x           = getU8(c);
    m->src_y           = getU8(c);
    m->in_boat         = getU8(c);
    m->allow_boat      = getU8(c);
    m->has_src_heading = getU8(c);
    m->src_heading     = getU8(c);
    m->danger_scale    = getF32(c);
    m->started_tick    = getU32(c);
    m->done            = getU8(c);
    m->completed_tick  = getU32(c);
}

static bool writeMeta(BrainRecDijBuf *b, const BrainRecDijMeta *m) {
    return putI32(b, m->kind) && putU8(b, m->src_x) && putU8(b, m->src_y)
        && putU8(b, m->in_boat) && putU8(b, m->allow_boat)
        && putU8(b, m->has_src_heading) && putU8(b, m->src_heading)
        && putF32(b, m->danger_scale) && putU32(b, m->started_tick)
        && putU8(b, m->done) && putU32(b, m->completed_tick);
}

/* Read one slate record and apply it to the bot. */
static void applyRecord(DijCur *c, BrainRecDijBotBox *b) {
    uint8_t hdr = getU8(c);
    if (!c->ok) return;
    BrainRecDijSlateBox *sl = &b->slates[hdr & 0x03u];
    if (hdr & BRAINREC_DIJ_REC_INACTIVE) { slateClear(sl); return; }
    if (hdr & BRAINREC_DIJ_REC_RESET) slateCellsClear(sl);
    if (hdr & BRAINREC_DIJ_REC_META) {
        BrainRecDijMeta m;
        memset(&m, 0, sizeof m);
        readMeta(c, &m);
        if (!c->ok) return;
        sl->meta   = m;
        sl->active = true;
    }
    if (hdr & BRAINREC_DIJ_REC_NODES) {
        for (int l = 0; l < 2 && c->ok; l++) {
            int n = getU8(c);
            for (int k = 0; k < n && c->ok; k++) {
                int     cell = getU8(c);
                float   cost = getF32(c);
                uint8_t dir  = getU8(c);
                if (!c->ok) return;
                if (cell >= BRAINREC_DIJ_CELLS) { c->ok = false; return; }
                sl->cost[l][cell] = cost;
                sl->dir[l][cell]  = dir;
            }
        }
        for (int l = 0; l < 2 && c->ok; l++) {
            int n = getU8(c);
            for (int k = 0; k < n && c->ok; k++) {
                int cell = getU8(c);
                if (!c->ok) return;
                if (cell >= BRAINREC_DIJ_CELLS) { c->ok = false; return; }
                sl->cost[l][cell] = BRAINREC_DIJ_COST_INF;
                sl->dir[l][cell]  = BRAINREC_DIJ_DIR_NONE;
            }
        }
    }
}

/* ---- encoder ----------------------------------------------------------- */

static bool metaIdentityDiffers(const BrainRecDijMeta *a, const BrainRecDijMeta *b) {
    return a->kind != b->kind || a->src_x != b->src_x || a->src_y != b->src_y
        || a->in_boat != b->in_boat || a->started_tick != b->started_tick;
}

static bool metaDiffers(const BrainRecDijMeta *a, const BrainRecDijMeta *b) {
    return metaIdentityDiffers(a, b)
        || a->allow_boat != b->allow_boat
        || a->has_src_heading != b->has_src_heading
        || a->src_heading != b->src_heading
        || fbits(a->danger_scale) != fbits(b->danger_scale)
        || a->done != b->done
        || a->completed_tick != b->completed_tick;
}

/* Write the record for one slate (or nothing when nothing changed), then
 * apply the written bytes to the writer's memory. */
static bool encodeSlate(BrainRecDijBotBox *pb, int s, const BrainRecDijBotIn *in,
                        BrainRecDijBuf *out, int *recCount) {
    const BrainRecDijSlateIn *si = &in->slates[s];
    BrainRecDijSlateBox      *ps = &pb->slates[s];
    size_t recStart = out->len;

    if (!si->active || !si->g_cost || !si->dir_at) {
        if (!ps->active) return true;
        if (!putU8(out, (uint8_t)(s | BRAINREC_DIJ_REC_INACTIVE))) return false;
    } else {
        bool reset = !ps->active || metaIdentityDiffers(&ps->meta, &si->meta);
        bool meta  = reset || metaDiffers(&ps->meta, &si->meta);

        /* What a reader holds for each cell before this record. */
        float   baseCost[2][BRAINREC_DIJ_CELLS];
        uint8_t baseDir[2][BRAINREC_DIJ_CELLS];
        if (reset) {
            for (int l = 0; l < 2; l++) {
                for (int i = 0; i < BRAINREC_DIJ_CELLS; i++) baseCost[l][i] = BRAINREC_DIJ_COST_INF;
                memset(baseDir[l], BRAINREC_DIJ_DIR_NONE, BRAINREC_DIJ_CELLS);
            }
        } else {
            memcpy(baseCost, ps->cost, sizeof baseCost);
            memcpy(baseDir, ps->dir, sizeof baseDir);
        }

        uint8_t wCell[2][BRAINREC_DIJ_CELLS], cCell[2][BRAINREC_DIJ_CELLS];
        float   wCost[2][BRAINREC_DIJ_CELLS];
        uint8_t wDir[2][BRAINREC_DIJ_CELLS];
        int     nw[2] = {0, 0}, nc[2] = {0, 0};

        if (pb->boxValid) {
            for (int j = 0; j < BRAINREC_DIJ_BOX; j++) {
                int y = pb->cy - BRAINREC_DIJ_HALF + j;
                for (int i = 0; i < BRAINREC_DIJ_BOX; i++) {
                    int x = pb->cx - BRAINREC_DIJ_HALF + i;
                    int cell = j * BRAINREC_DIJ_BOX + i;
                    float   tc[2] = {BRAINREC_DIJ_COST_INF, BRAINREC_DIJ_COST_INF};
                    uint8_t td[2] = {BRAINREC_DIJ_DIR_NONE, BRAINREC_DIJ_DIR_NONE};
                    if (x >= 0 && x <= 255 && y >= 0 && y <= 255) {
                        int ni = y * 256 + x;
                        float land = si->g_cost[ni];
                        if (costReached(land)) { tc[0] = land; td[0] = si->dir_at[ni]; }
                        if (si->meta.allow_boat) {
                            float boat = si->g_cost[BRAINREC_DIJ_BOAT_OFFSET + ni];
                            /* A tank on land keeps a boat node only when it
                             * is cheaper than the land node. */
                            if (costReached(boat) && (in->onBoat || boat < land)) {
                                tc[1] = boat;
                                td[1] = si->dir_at[BRAINREC_DIJ_BOAT_OFFSET + ni];
                            }
                        }
                    }
                    for (int l = 0; l < 2; l++) {
                        bool baseHas = costReached(baseCost[l][cell]);
                        if (costReached(tc[l])) {
                            if (!baseHas || fbits(baseCost[l][cell]) != fbits(tc[l])
                                || baseDir[l][cell] != td[l]) {
                                wCell[l][nw[l]] = (uint8_t)cell;
                                wCost[l][nw[l]] = tc[l];
                                wDir[l][nw[l]]  = td[l];
                                nw[l]++;
                            }
                        } else if (baseHas) {
                            cCell[l][nc[l]++] = (uint8_t)cell;
                        }
                    }
                }
            }
        }
        bool nodes = (nw[0] + nw[1] + nc[0] + nc[1]) > 0;
        if (!meta && !nodes) return true;

        uint8_t hdr = (uint8_t)s;
        if (meta)  hdr |= BRAINREC_DIJ_REC_META;
        if (reset) hdr |= BRAINREC_DIJ_REC_RESET;
        if (nodes) hdr |= BRAINREC_DIJ_REC_NODES;
        if (!putU8(out, hdr)) return false;
        if (meta && !writeMeta(out, &si->meta)) return false;
        if (nodes) {
            for (int l = 0; l < 2; l++) {
                if (!putU8(out, (uint8_t)nw[l])) return false;
                for (int k = 0; k < nw[l]; k++) {
                    if (!putU8(out, wCell[l][k]) || !putF32(out, wCost[l][k])
                        || !putU8(out, wDir[l][k])) return false;
                }
            }
            for (int l = 0; l < 2; l++) {
                if (!putU8(out, (uint8_t)nc[l])) return false;
                if (nc[l] && !putBytes(out, cCell[l], (size_t)nc[l])) return false;
            }
        }
    }

    DijCur c = { out->data + recStart, out->len - recStart, 0, true };
    applyRecord(&c, pb);
    (*recCount)++;
    return true;
}

bool brainRecDijEncodeFrame(BrainRecDijState *prev,
                            const BrainRecDijBotIn *const bots[BRAINREC_DIJ_MAX_BOTS],
                            bool keyframe, BrainRecDijBuf *out) {
    if (!prev || !out) return false;
    out->len = 0;
    if (keyframe) brainRecDijStateClear(prev);

    int nBots = 0;
    for (int i = 0; i < BRAINREC_DIJ_MAX_BOTS; i++) {
        if (bots && bots[i]) nBots++;
        else botClear(&prev->bots[i]);
    }
    if (!putU8(out, keyframe ? BRAINREC_DIJ_FLAG_KEYFRAME : 0u)) return false;
    if (!putU8(out, (uint8_t)nBots)) return false;

    for (int i = 0; i < BRAINREC_DIJ_MAX_BOTS; i++) {
        const BrainRecDijBotIn *in = bots ? bots[i] : NULL;
        if (!in) continue;
        BrainRecDijBotBox *pb = &prev->bots[i];
        bool valid = in->hasTank && in->tx >= 0 && in->tx <= 255
                     && in->ty >= 0 && in->ty <= 255;
        uint8_t bflags = (uint8_t)((valid ? BRAINREC_DIJ_BOT_BOX : 0u)
                                   | (in->onBoat ? BRAINREC_DIJ_BOT_ONBOAT : 0u));
        if (!putU8(out, (uint8_t)i) || !putU8(out, bflags)) return false;
        if (valid && (!putU8(out, (uint8_t)in->tx) || !putU8(out, (uint8_t)in->ty))) return false;
        botBegin(pb, valid, in->onBoat, in->tx, in->ty);

        size_t countAt = out->len;
        if (!putU8(out, 0)) return false;
        int recCount = 0;
        for (int s = 0; s < BRAINREC_DIJ_SLATES; s++) {
            if (!encodeSlate(pb, s, in, out, &recCount)) return false;
        }
        out->data[countAt] = (uint8_t)recCount;
    }
    return true;
}

/* ---- decoder ----------------------------------------------------------- */

bool brainRecDijBlockIsKeyframe(const uint8_t *blob, size_t len) {
    return blob && len >= 1 && (blob[0] & BRAINREC_DIJ_FLAG_KEYFRAME);
}

bool brainRecDijDecodeFrame(BrainRecDijState *st, const uint8_t *blob, size_t len) {
    if (!st) return false;
    if (!blob || len == 0) return true;
    DijCur c = { blob, len, 0, true };
    uint8_t flags = getU8(&c);
    int     nBots = getU8(&c);
    if (!c.ok) return false;
    if (flags & BRAINREC_DIJ_FLAG_KEYFRAME) brainRecDijStateClear(st);

    bool seen[BRAINREC_DIJ_MAX_BOTS] = {false};
    for (int b = 0; b < nBots && c.ok; b++) {
        int     slot   = getU8(&c);
        uint8_t bflags = getU8(&c);
        bool    valid  = (bflags & BRAINREC_DIJ_BOT_BOX) != 0;
        int cx = 0, cy = 0;
        if (valid) { cx = getU8(&c); cy = getU8(&c); }
        int nRec = getU8(&c);
        if (!c.ok) break;
        if (slot >= BRAINREC_DIJ_MAX_BOTS) { c.ok = false; break; }
        seen[slot] = true;
        BrainRecDijBotBox *bb = &st->bots[slot];
        botBegin(bb, valid, (bflags & BRAINREC_DIJ_BOT_ONBOAT) != 0, cx, cy);
        for (int r = 0; r < nRec && c.ok; r++) applyRecord(&c, bb);
    }
    if (!c.ok) return false;
    for (int i = 0; i < BRAINREC_DIJ_MAX_BOTS; i++) {
        if (!seen[i]) botClear(&st->bots[i]);
    }
    return true;
}

/* ---- lookups ----------------------------------------------------------- */

int brainRecDijCellIndex(const BrainRecDijBotBox *b, int x, int y) {
    if (!b || !b->boxValid) return -1;
    int i = x - (b->cx - BRAINREC_DIJ_HALF);
    int j = y - (b->cy - BRAINREC_DIJ_HALF);
    if (i < 0 || i >= BRAINREC_DIJ_BOX || j < 0 || j >= BRAINREC_DIJ_BOX) return -1;
    return j * BRAINREC_DIJ_BOX + i;
}

float brainRecDijCostAt(const BrainRecDijBotBox *b, int slate, int x, int y,
                        int *layerOut) {
    if (layerOut) *layerOut = 0;
    if (!b || slate < 0 || slate >= BRAINREC_DIJ_SLATES) return BRAINREC_DIJ_COST_INF;
    const BrainRecDijSlateBox *sl = &b->slates[slate];
    if (!sl->active) return BRAINREC_DIJ_COST_INF;
    int cell = brainRecDijCellIndex(b, x, y);
    if (cell < 0) return BRAINREC_DIJ_COST_INF;
    float land = sl->cost[0][cell], boat = sl->cost[1][cell];
    if (boat < land) { if (layerOut) *layerOut = 1; return boat; }
    return land;
}

int brainRecDijTrace(const BrainRecDijBotBox *b, int slate, int x, int y,
                     int *path_x, int *path_y, int max_steps,
                     BrainRecDijTraceEnd *endOut) {
    BrainRecDijTraceEnd end = BRAINREC_DIJ_TRACE_NONE;
    int n = 0, layer = 0;
    if (endOut) *endOut = end;
    if (!b || !path_x || !path_y || max_steps <= 0) return 0;
    if (!costReached(brainRecDijCostAt(b, slate, x, y, &layer))) return 0;
    const BrainRecDijSlateBox *sl = &b->slates[slate];
    int cell = brainRecDijCellIndex(b, x, y);
    for (;;) {
        if (n >= max_steps) { end = BRAINREC_DIJ_TRACE_TOO_LONG; break; }
        path_x[n] = x;
        path_y[n] = y;
        n++;
        uint8_t d = sl->dir[layer][cell];
        if (d == BRAINREC_DIJ_DIR_NONE) { end = BRAINREC_DIJ_TRACE_SOURCE; break; }
        int px = x - DIJ_DX8[d & 0x07], py = y - DIJ_DY8[d & 0x07];
        int pl = (d & 0x08) ? 1 : 0;
        int pc = brainRecDijCellIndex(b, px, py);
        if (pc < 0) { end = BRAINREC_DIJ_TRACE_LEFT_BOX; break; }
        if (!costReached(sl->cost[pl][pc])) { end = BRAINREC_DIJ_TRACE_MISSING; break; }
        x = px; y = py; layer = pl; cell = pc;
    }
    if (endOut) *endOut = end;
    return n;
}

/* ---- store ------------------------------------------------------------- */

void brainRecDijStoreInit(BrainRecDijStore *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->cacheFrame = -1;
}

void brainRecDijStoreFree(BrainRecDijStore *s) {
    if (!s) return;
    free(s->data);
    free(s->frames);
    free(s->cache);
    brainRecDijStoreInit(s);
}

bool brainRecDijStoreSet(BrainRecDijStore *s, int frameIdx,
                         const uint8_t *blob, size_t len) {
    if (!s || frameIdx < 0) return false;
    if (frameIdx >= s->cap_frames) {
        int nc = s->cap_frames ? s->cap_frames : 1024;
        while (nc <= frameIdx) nc *= 2;
        BrainRecDijFrameRef *nf = (BrainRecDijFrameRef *)realloc(s->frames,
                                      (size_t)nc * sizeof *nf);
        if (!nf) return false;
        s->frames = nf;
        s->cap_frames = nc;
    }
    while (s->count <= frameIdx) {
        s->frames[s->count].off = 0;
        s->frames[s->count].len = 0;
        s->frames[s->count].key = false;
        s->count++;
    }
    BrainRecDijFrameRef *f = &s->frames[frameIdx];
    if (f->len) s->framesWithData--;
    f->len = 0;
    f->key = false;
    if (blob && len) {
        if (s->len + len > s->cap) {
            size_t nc = s->cap ? s->cap : (size_t)1 << 20;
            while (nc < s->len + len) nc *= 2;
            uint8_t *nd = (uint8_t *)realloc(s->data, nc);
            if (!nd) return false;
            s->data = nd;
            s->cap  = nc;
        }
        memcpy(s->data + s->len, blob, len);
        f->off = (uint32_t)s->len;
        f->len = (uint32_t)len;
        f->key = brainRecDijBlockIsKeyframe(blob, len);
        s->len += len;
        s->framesWithData++;
    }
    /* A frame at or before the cached one changed: the cache is stale. */
    if (s->cacheFrame >= frameIdx) s->cacheFrame = -1;
    return true;
}

bool brainRecDijStoreHasData(const BrainRecDijStore *s) {
    return s && s->framesWithData > 0;
}

const BrainRecDijState *brainRecDijStoreAt(BrainRecDijStore *s, int frameIdx) {
    if (!s || frameIdx < 0 || frameIdx >= s->count || !brainRecDijStoreHasData(s)) return NULL;
    if (!s->cache) {
        s->cache = (BrainRecDijState *)malloc(sizeof *s->cache);
        if (!s->cache) return NULL;
        s->cacheFrame = -1;
    }
    int key = 0;
    for (int i = frameIdx; i >= 0; i--) {
        if (s->frames[i].key) { key = i; break; }
    }
    int start;
    if (s->cacheFrame >= key && s->cacheFrame <= frameIdx) {
        start = s->cacheFrame + 1;
    } else {
        brainRecDijStateClear(s->cache);
        start = key;
    }
    for (int i = start; i <= frameIdx; i++) {
        const BrainRecDijFrameRef *f = &s->frames[i];
        if (f->len) brainRecDijDecodeFrame(s->cache, s->data + f->off, f->len);
    }
    s->cacheFrame = frameIdx;
    return s->cache;
}
