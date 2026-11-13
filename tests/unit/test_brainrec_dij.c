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
 * The recorded Dijkstra box in brainrec.btr v8 (brain_record_dij.c).
 *
 * The recorder writes, per bot and tick, the active slates in a 15x15 box
 * round the tank, and only what changed. These cases drive the encoder with
 * hand-built slates (full node arrays, as the pathfinder holds them) and
 * read the blocks back with the decoder and the store BrainTest uses:
 *   - only changed nodes are written, and a tile that enters the box counts
 *     as changed;
 *   - unreached nodes (COST_INF) are never written;
 *   - a slate with allow_boat = 0 writes no boat node;
 *   - a tank on land keeps a boat node only when it is cheaper than the
 *     land node; a tank on a boat keeps every boat node;
 *   - a new started_tick resets the slate;
 *   - a frame rebuilt from its keyframe equals the same frame rebuilt by
 *     replaying every delta from the start, and both equal the slates;
 *   - a v7 file (no Dijkstra block) still walks, and a v8 one walks too.
 * The danger box (the bot's danger_grid in the same box) is checked too:
 *   - only changed danger cells are written; a 0 is written only to clear
 *     a cell that was non-zero, and a 0 cell entering the box is skipped;
 *   - a keyframe holds every non-zero cell, and a seek equals the replay.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "brain_record.h"
#include "brain_record_dij.h"
#include "brain_record_walk.h"
#include "input_packet.h"
#include "test_harness.h"

#define DIJ_NODES 131072

/* One synthetic slate: its full node arrays and meta. */
typedef struct {
    float   *g;
    uint8_t *d;
} TSlate;

static bool tSlateAlloc(TSlate *t) {
    t->g = (float *)malloc(sizeof(float) * DIJ_NODES);
    t->d = (uint8_t *)malloc(DIJ_NODES);
    if (!t->g || !t->d) return false;
    for (int i = 0; i < DIJ_NODES; i++) t->g[i] = BRAINREC_DIJ_COST_INF;
    memset(t->d, 0xFF, DIJ_NODES);
    return true;
}

static void tSlateFree(TSlate *t) {
    free(t->g);
    free(t->d);
    t->g = NULL;
    t->d = NULL;
}

static void tSlateReset(TSlate *t) {
    for (int i = 0; i < DIJ_NODES; i++) t->g[i] = BRAINREC_DIJ_COST_INF;
    memset(t->d, 0xFF, DIJ_NODES);
}

/* A plausible Dijkstra field from (sx, sy): cost grows with distance, and
 * dir_at points from the parent one step closer to the source. radius caps
 * how far the search has got; tiles past it stay unreached. */
static int tSign(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

static uint8_t tDirFor(int dx, int dy) {
    /* dir index whose (DX8, DY8) equals (dx, dy): N NE E SE S SW W NW */
    static const int DX8[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
    static const int DY8[8] = {-1, -1,  0,  1,  1,  1,  0, -1 };
    for (int i = 0; i < 8; i++) if (DX8[i] == dx && DY8[i] == dy) return (uint8_t)i;
    return 0xFF;
}

static void tSlateFill(TSlate *t, int sx, int sy, int radius, float base,
                       bool boat, float boatScale, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x > 255 || y < 0 || y > 255) continue;
            int dx = x - sx, dy = y - sy;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            int cheb = adx > ady ? adx : ady;
            if (cheb > radius) continue;
            int ni = y * 256 + x;
            float c = base * (float)(adx + ady) + (float)((x * 7 + y * 13) % 5);
            t->g[ni] = (cheb == 0) ? 0.0f : c;
            t->d[ni] = (cheb == 0) ? 0xFF : tDirFor(tSign(dx), tSign(dy));
            if (boat) {
                int bi = 65536 + ni;
                t->g[bi] = (cheb == 0) ? 0.0f : c * boatScale;
                /* boat parent on the boat layer: bit 3 set */
                t->d[bi] = (cheb == 0) ? 0xFF : (uint8_t)(tDirFor(tSign(dx), tSign(dy)) | 0x08);
            }
        }
    }
}

static void tMeta(BrainRecDijMeta *m, int kind, int sx, int sy, uint32_t started,
                  int allowBoat) {
    memset(m, 0, sizeof *m);
    m->kind = kind;
    m->src_x = (uint8_t)sx;
    m->src_y = (uint8_t)sy;
    m->allow_boat = (uint8_t)allowBoat;
    m->danger_scale = 1.0f;
    m->started_tick = started;
}

/* Parse a block the way the layout in brain_record_dij.h says and count
 * what it carries, independently of the decoder. */
typedef struct {
    int records, resets, metas, inactive;
    int landWrites, boatWrites, landClears, boatClears;
    int dangerWrites, dangerZeros;   /* danger cells written; of them, 0s */
    size_t dangerBytes;              /* danger section bytes, count byte included */
} TCount;

static bool tCountBlock(const uint8_t *b, size_t n, TCount *c) {
    memset(c, 0, sizeof *c);
    size_t p = 0;
#define NEED(k) do { if (p + (k) > n) return false; } while (0)
    NEED(2);
    int nBots = b[1];
    p = 2;
    for (int i = 0; i < nBots; i++) {
        NEED(2);
        uint8_t bf = b[p + 1];
        p += 2;
        if (bf & BRAINREC_DIJ_BOT_BOX) { NEED(2); p += 2; }
        NEED(1);
        int nr = b[p++];
        for (int r = 0; r < nr; r++) {
            NEED(1);
            uint8_t h = b[p++];
            c->records++;
            if (h & BRAINREC_DIJ_REC_INACTIVE) { c->inactive++; continue; }
            if (h & BRAINREC_DIJ_REC_RESET) c->resets++;
            if (h & BRAINREC_DIJ_REC_META) { c->metas++; NEED(23); p += 23; }
            if (h & BRAINREC_DIJ_REC_NODES) {
                for (int l = 0; l < 2; l++) {
                    NEED(1);
                    int k = b[p++];
                    NEED((size_t)k * 6);
                    p += (size_t)k * 6;
                    if (l == 0) c->landWrites += k; else c->boatWrites += k;
                }
                for (int l = 0; l < 2; l++) {
                    NEED(1);
                    int k = b[p++];
                    NEED((size_t)k);
                    p += (size_t)k;
                    if (l == 0) c->landClears += k; else c->boatClears += k;
                }
            }
        }
        NEED(1);
        int nd = b[p++];
        NEED((size_t)nd * 3);
        for (int k = 0; k < nd; k++) {
            uint16_t v;
            memcpy(&v, b + p + (size_t)k * 3 + 1, 2);
            if (v == 0) c->dangerZeros++;
        }
        p += (size_t)nd * 3;
        c->dangerWrites += nd;
        c->dangerBytes  += 1 + (size_t)nd * 3;
    }
#undef NEED
    return p == n;
}

/* The box a reader should hold for one slate, computed straight from the
 * spec: reached land nodes; boat nodes only with allow_boat, and on land
 * only when cheaper than the land node. */
static bool tBoxMatches(const BrainRecDijBotBox *bb, int slate, const BrainRecDijBotIn *in) {
    const BrainRecDijSlateIn *si = &in->slates[slate];
    const BrainRecDijSlateBox *sb = &bb->slates[slate];
    if (!si->active) return !sb->active;
    if (!sb->active) return false;
    if (sb->meta.started_tick != si->meta.started_tick || sb->meta.kind != si->meta.kind)
        return false;
    for (int j = 0; j < BRAINREC_DIJ_BOX; j++) {
        for (int i = 0; i < BRAINREC_DIJ_BOX; i++) {
            int x = in->tx - BRAINREC_DIJ_HALF + i, y = in->ty - BRAINREC_DIJ_HALF + j;
            int cell = j * BRAINREC_DIJ_BOX + i;
            float el = BRAINREC_DIJ_COST_INF, eb = BRAINREC_DIJ_COST_INF;
            uint8_t dl = 0xFF, db = 0xFF;
            if (x >= 0 && x <= 255 && y >= 0 && y <= 255) {
                int ni = y * 256 + x;
                if (si->g_cost[ni] < 1e29f) { el = si->g_cost[ni]; dl = si->dir_at[ni]; }
                float bc = si->g_cost[65536 + ni];
                if (si->meta.allow_boat && bc < 1e29f && (in->onBoat || bc < si->g_cost[ni])) {
                    eb = bc;
                    db = si->dir_at[65536 + ni];
                }
            }
            if (memcmp(&sb->cost[0][cell], &el, 4) != 0 || sb->dir[0][cell] != dl) return false;
            if (memcmp(&sb->cost[1][cell], &eb, 4) != 0 || sb->dir[1][cell] != db) return false;
        }
    }
    return true;
}

/* The danger box a reader should hold: the grid's values in the box (0
 * off the map or with no grid). */
static bool tDangerMatches(const BrainRecDijBotBox *bb, const BrainRecDijBotIn *in) {
    if (!in->hasTank) return !bb->boxValid;
    for (int j = 0; j < BRAINREC_DIJ_BOX; j++) {
        for (int i = 0; i < BRAINREC_DIJ_BOX; i++) {
            int x = in->tx - BRAINREC_DIJ_HALF + i, y = in->ty - BRAINREC_DIJ_HALF + j;
            uint16_t want = 0, got = 0;
            if (in->danger && x >= 0 && x <= 255 && y >= 0 && y <= 255)
                want = in->danger[y * 256 + x];
            if (!brainRecDijDangerAt(bb, x, y, &got) || got != want) return false;
        }
    }
    return true;
}

static bool tStatesEqual(const BrainRecDijState *a, const BrainRecDijState *b) {
    for (int i = 0; i < BRAINREC_DIJ_MAX_BOTS; i++) {
        const BrainRecDijBotBox *x = &a->bots[i], *y = &b->bots[i];
        if (x->present != y->present || x->boxValid != y->boxValid
            || x->onBoat != y->onBoat || x->cx != y->cx || x->cy != y->cy) return false;
        if (memcmp(x->danger, y->danger, sizeof x->danger) != 0) return false;
        for (int s = 0; s < BRAINREC_DIJ_SLATES; s++) {
            const BrainRecDijSlateBox *p = &x->slates[s], *q = &y->slates[s];
            if (p->active != q->active) return false;
            if (memcmp(p->cost, q->cost, sizeof p->cost) != 0) return false;
            if (memcmp(p->dir, q->dir, sizeof p->dir) != 0) return false;
            if (p->active && (p->meta.kind != q->meta.kind
                              || p->meta.started_tick != q->meta.started_tick
                              || p->meta.done != q->meta.done
                              || p->meta.completed_tick != q->meta.completed_tick))
                return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */

int run_brainrec_dij_changed_only(void) {
    TSlate t;
    UT_ASSERT(tSlateAlloc(&t));
    BrainRecDijState *prev = (BrainRecDijState *)malloc(sizeof *prev);
    BrainRecDijState *rd   = (BrainRecDijState *)malloc(sizeof *rd);
    UT_ASSERT(prev && rd);
    brainRecDijStateClear(prev);
    brainRecDijStateClear(rd);
    BrainRecDijBuf buf = {0};
    TCount c;

    /* Source at the tank, reached radius 3: a 7x7 square of the box. The
     * rest of the box is COST_INF and must not be written. */
    static BrainRecDijBotIn in;
    memset(&in, 0, sizeof in);
    in.hasTank = true; in.tx = 100; in.ty = 100;
    tSlateFill(&t, 100, 100, 3, 10.0f, false, 1.0f, 0, 0, 255, 255);
    in.slates[0].active = true;
    in.slates[0].g_cost = t.g;
    in.slates[0].dir_at = t.d;
    tMeta(&in.slates[0].meta, 1, 100, 100, 50, 0);
    const BrainRecDijBotIn *bots[BRAINREC_DIJ_MAX_BOTS] = {0};
    bots[3] = &in;

    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, true, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.landWrites == 49, "first frame wrote %d land nodes, want 49 (7x7 reached; "
                  "COST_INF nodes are never written)", c.landWrites);
    UT_ASSERT_MSG(c.resets == 1 && c.metas == 1, "first frame: %d resets %d metas", c.resets, c.metas);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[3], 0, &in));

    /* Same slate, same tank tile: nothing changed, so no record at all. */
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.records == 0, "an unchanged frame wrote %d records", c.records);
    UT_ASSERT_MSG(buf.len == 8, "an unchanged frame is %u bytes, want 8 "
                  "(flags, count, slot, bot flags, cx, cy, record count, danger count)",
                  (unsigned)buf.len);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[3], 0, &in));

    /* One node gets cheaper and the search reaches one new node: two writes. */
    t.g[100 * 256 + 102] -= 1.0f;
    t.g[100 * 256 + 104] = 55.0f;
    t.d[100 * 256 + 104] = 2;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.landWrites == 2 && c.metas == 0,
                  "two changed nodes wrote %d nodes, %d metas", c.landWrites, c.metas);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[3], 0, &in));

    /* The status changes (done): a meta record without a reset or nodes. */
    in.slates[0].meta.done = 1;
    in.slates[0].meta.completed_tick = 52;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.metas == 1 && c.resets == 0 && c.landWrites == 0,
                  "done flip: %d metas %d resets %d nodes", c.metas, c.resets, c.landWrites);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(rd->bots[3].slates[0].meta.done == 1);

    /* Fill the whole map, then move the tank one tile east: only the new
     * column of 15 tiles enters the box, so 15 writes. */
    tSlateFill(&t, 100, 100, 255, 10.0f, false, 1.0f, 0, 0, 255, 255);
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    in.tx = 101;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.landWrites == BRAINREC_DIJ_BOX,
                  "a one-tile move wrote %d nodes, want %d (the new column)",
                  c.landWrites, BRAINREC_DIJ_BOX);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[3], 0, &in));

    /* The tank dies: no box, no nodes. It comes back: the whole box is new. */
    in.hasTank = false;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT(c.landWrites == 0);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(!rd->bots[3].boxValid);
    in.hasTank = true;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.landWrites == BRAINREC_DIJ_CELLS, "respawn wrote %d nodes, want %d",
                  c.landWrites, BRAINREC_DIJ_CELLS);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[3], 0, &in));

    /* The slate goes away: one INACTIVE record, and the reader drops it. */
    in.slates[0].active = false;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT(c.inactive == 1);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(!rd->bots[3].slates[0].active);

    /* The bot leaves the game: the reader forgets it. */
    bots[3] = NULL;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(buf.len == 2);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(!rd->bots[3].present);

    brainRecDijBufFree(&buf);
    free(prev); free(rd);
    tSlateFree(&t);
    return 0;
}

/* The start heading (PF_START_HEADING) goes through the block: a slate
 * with a heading reads back with it, a heading change alone writes one meta
 * record and no node, and a slate with no heading reads back as 0 / 0 (the
 * value every file made before the recorder filled the field holds). */
int run_brainrec_dij_src_heading(void) {
    TSlate t;
    UT_ASSERT(tSlateAlloc(&t));
    BrainRecDijState *prev = (BrainRecDijState *)malloc(sizeof *prev);
    BrainRecDijState *rd   = (BrainRecDijState *)malloc(sizeof *rd);
    UT_ASSERT(prev && rd);
    brainRecDijStateClear(prev);
    brainRecDijStateClear(rd);
    BrainRecDijBuf buf = {0};
    TCount c;

    static BrainRecDijBotIn in;
    memset(&in, 0, sizeof in);
    in.hasTank = true; in.tx = 100; in.ty = 100;
    tSlateFill(&t, 100, 100, 3, 10.0f, false, 1.0f, 0, 0, 255, 255);
    in.slates[0].active = true;
    in.slates[0].g_cost = t.g;
    in.slates[0].dir_at = t.d;
    tMeta(&in.slates[0].meta, 1, 100, 100, 50, 0);
    in.slates[0].meta.has_src_heading = 1;
    in.slates[0].meta.src_heading     = 0xC0;
    const BrainRecDijBotIn *bots[BRAINREC_DIJ_MAX_BOTS] = {0};
    bots[2] = &in;

    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, true, &buf));
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT_MSG(rd->bots[2].slates[0].meta.has_src_heading == 1
                  && rd->bots[2].slates[0].meta.src_heading == 0xC0,
                  "heading read back as %u / %u, want 1 / 192",
                  rd->bots[2].slates[0].meta.has_src_heading,
                  rd->bots[2].slates[0].meta.src_heading);
    UT_ASSERT(tBoxMatches(&rd->bots[2], 0, &in));

    /* Only the heading changes: one meta record, no reset, no node. */
    in.slates[0].meta.src_heading = 0x40;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.metas == 1 && c.resets == 0 && c.landWrites == 0,
                  "heading change: %d metas %d resets %d nodes", c.metas, c.resets, c.landWrites);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(rd->bots[2].slates[0].meta.src_heading == 0x40);

    /* The heading is turned off: reads back as no heading. */
    in.slates[0].meta.has_src_heading = 0;
    in.slates[0].meta.src_heading     = 0;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(rd->bots[2].slates[0].meta.has_src_heading == 0
              && rd->bots[2].slates[0].meta.src_heading == 0);

    brainRecDijBufFree(&buf);
    free(prev); free(rd);
    tSlateFree(&t);
    return 0;
}

int run_brainrec_dij_boat_layer(void) {
    TSlate t;
    UT_ASSERT(tSlateAlloc(&t));
    BrainRecDijState *prev = (BrainRecDijState *)malloc(sizeof *prev);
    BrainRecDijState *rd   = (BrainRecDijState *)malloc(sizeof *rd);
    UT_ASSERT(prev && rd);
    BrainRecDijBuf buf = {0};
    TCount c;
    static BrainRecDijBotIn in;
    const BrainRecDijBotIn *bots[BRAINREC_DIJ_MAX_BOTS] = {0};
    bots[0] = &in;

    /* Boat layer cheaper (0.5x) in the west half of the box, dearer (2x)
     * in the east half. The source column is 0 on both layers. */
    tSlateFill(&t, 50, 50, 255, 10.0f, true, 0.5f, 0, 0, 49, 255);
    tSlateFill(&t, 50, 50, 255, 10.0f, true, 2.0f, 50, 0, 255, 255);
    int cheaper = 0;
    for (int y = 50 - BRAINREC_DIJ_HALF; y <= 50 + BRAINREC_DIJ_HALF; y++)
        for (int x = 50 - BRAINREC_DIJ_HALF; x <= 50 + BRAINREC_DIJ_HALF; x++)
            if (t.g[65536 + y * 256 + x] < t.g[y * 256 + x]) cheaper++;
    UT_ASSERT(cheaper > 0 && cheaper < BRAINREC_DIJ_CELLS);

    /* allow_boat = 0: the boat layer is skipped entirely. */
    memset(&in, 0, sizeof in);
    in.hasTank = true; in.tx = 50; in.ty = 50; in.onBoat = true;
    in.slates[1].active = true;
    in.slates[1].g_cost = t.g;
    in.slates[1].dir_at = t.d;
    tMeta(&in.slates[1].meta, 2, 50, 50, 10, 0);
    brainRecDijStateClear(prev);
    brainRecDijStateClear(rd);
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, true, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.boatWrites == 0, "allow_boat=0 wrote %d boat nodes", c.boatWrites);
    UT_ASSERT(c.landWrites == BRAINREC_DIJ_CELLS);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[0], 1, &in));

    /* allow_boat = 1, tank on land: a boat node only where it is cheaper. */
    in.onBoat = false;
    tMeta(&in.slates[1].meta, 2, 50, 50, 11, 1);
    brainRecDijStateClear(prev);
    brainRecDijStateClear(rd);
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, true, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.boatWrites == cheaper, "tank on land wrote %d boat nodes, want %d "
                  "(only those cheaper than land)", c.boatWrites, cheaper);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[0], 1, &in));

    /* The tank boards a boat: every boat node is kept now, so the dearer
     * ones are written as changes. */
    in.onBoat = true;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.boatWrites == BRAINREC_DIJ_CELLS - cheaper,
                  "boarding wrote %d boat nodes, want %d", c.boatWrites,
                  BRAINREC_DIJ_CELLS - cheaper);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[0], 1, &in));

    /* And back on land: those dearer boat nodes are cleared again. */
    in.onBoat = false;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.boatClears == BRAINREC_DIJ_CELLS - cheaper && c.boatWrites == 0,
                  "landing cleared %d boat nodes (want %d), wrote %d",
                  c.boatClears, BRAINREC_DIJ_CELLS - cheaper, c.boatWrites);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[0], 1, &in));

    /* The recorded path from a box corner follows dir_at to the source. */
    {
        int px[BRAINREC_DIJ_CELLS * 2], py[BRAINREC_DIJ_CELLS * 2];
        BrainRecDijTraceEnd end;
        int n = brainRecDijTrace(&rd->bots[0], 1, 50 + BRAINREC_DIJ_HALF, 50 + BRAINREC_DIJ_HALF,
                                 px, py, BRAINREC_DIJ_CELLS * 2, &end);
        UT_ASSERT_MSG(end == BRAINREC_DIJ_TRACE_SOURCE && n == BRAINREC_DIJ_HALF + 1,
                      "trace from the corner: %d tiles, end %d", n, (int)end);
        UT_ASSERT(px[n - 1] == 50 && py[n - 1] == 50);
    }

    /* A source outside the box: the path stops at the box edge. */
    tSlateReset(&t);
    tSlateFill(&t, 20, 50, 255, 10.0f, false, 1.0f, 0, 0, 255, 255);
    tMeta(&in.slates[1].meta, 2, 20, 50, 12, 0);
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    {
        int px[BRAINREC_DIJ_CELLS * 2], py[BRAINREC_DIJ_CELLS * 2];
        BrainRecDijTraceEnd end;
        int n = brainRecDijTrace(&rd->bots[0], 1, 55, 50, px, py, BRAINREC_DIJ_CELLS * 2, &end);
        UT_ASSERT_MSG(end == BRAINREC_DIJ_TRACE_LEFT_BOX && n == 55 - (50 - BRAINREC_DIJ_HALF) + 1,
                      "trace toward an outside source: %d tiles, end %d", n, (int)end);
        UT_ASSERT(brainRecDijCellIndex(&rd->bots[0], 42, 50) < 0);
        UT_ASSERT(brainRecDijCostAt(&rd->bots[0], 1, 42, 50, NULL) >= BRAINREC_DIJ_INF_LIMIT);
    }

    brainRecDijBufFree(&buf);
    free(prev); free(rd);
    tSlateFree(&t);
    return 0;
}

int run_brainrec_dij_reset_on_new_start(void) {
    TSlate t;
    UT_ASSERT(tSlateAlloc(&t));
    BrainRecDijState *prev = (BrainRecDijState *)malloc(sizeof *prev);
    BrainRecDijState *rd   = (BrainRecDijState *)malloc(sizeof *rd);
    UT_ASSERT(prev && rd);
    brainRecDijStateClear(prev);
    brainRecDijStateClear(rd);
    BrainRecDijBuf buf = {0};
    TCount c;
    static BrainRecDijBotIn in;
    memset(&in, 0, sizeof in);
    const BrainRecDijBotIn *bots[BRAINREC_DIJ_MAX_BOTS] = {0};
    bots[5] = &in;
    in.hasTank = true; in.tx = 200; in.ty = 30;

    /* A full search, then a new one from the same source with the same
     * costs: only started_tick differs, and that alone is a reset that
     * writes every reached node again. */
    tSlateFill(&t, 200, 30, 255, 7.0f, false, 1.0f, 0, 0, 255, 255);
    in.slates[2].active = true;
    in.slates[2].g_cost = t.g;
    in.slates[2].dir_at = t.d;
    tMeta(&in.slates[2].meta, 0, 200, 30, 100, 0);
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, true, &buf));
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));

    in.slates[2].meta.started_tick = 140;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.resets == 1 && c.landWrites == BRAINREC_DIJ_CELLS,
                  "new started_tick: %d resets, %d nodes (want 1, %d)",
                  c.resets, c.landWrites, BRAINREC_DIJ_CELLS);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(rd->bots[5].slates[2].meta.started_tick == 140);

    /* The new search has only reached radius 2 so far: the reset drops the
     * nodes of the old search, which a delta alone never clears. */
    tSlateReset(&t);
    tSlateFill(&t, 200, 30, 2, 7.0f, false, 1.0f, 0, 0, 255, 255);
    in.slates[2].meta.started_tick = 160;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT(c.resets == 1 && c.landWrites == 25 && c.landClears == 0);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tBoxMatches(&rd->bots[5], 2, &in));

    brainRecDijBufFree(&buf);
    free(prev); free(rd);
    tSlateFree(&t);
    return 0;
}

/* A long synthetic game: 4 bots, 4 slates each, searches that restart and
 * grow over several ticks, tanks that move and board boats. Every frame is
 * checked against the slates, a seek from the keyframe must equal a replay
 * of every delta, and the per-bot size is reported. */
int run_brainrec_dij_keyframe_equals_replay(void) {
    enum { NBOT = 4, NFRAMES = 3000, KEYINT = BRAINREC_MAP_KEYFRAME_INTERVAL };
    static TSlate ts[NBOT][BRAINREC_DIJ_SLATES];
    static BrainRecDijBotIn in[NBOT];
    static uint16_t dg[NBOT][65536];   /* each bot's danger grid */
    memset(dg, 0, sizeof dg);
    const BrainRecDijBotIn *bots[BRAINREC_DIJ_MAX_BOTS] = {0};
    int slotOf[NBOT] = {0, 2, 7, 15};
    for (int b = 0; b < NBOT; b++) {
        for (int s = 0; s < BRAINREC_DIJ_SLATES; s++) UT_ASSERT(tSlateAlloc(&ts[b][s]));
        bots[slotOf[b]] = &in[b];
    }
    BrainRecDijState *prev = (BrainRecDijState *)malloc(sizeof *prev);
    BrainRecDijState *seq  = (BrainRecDijState *)malloc(sizeof *seq);
    UT_ASSERT(prev && seq);
    brainRecDijStateClear(prev);
    brainRecDijStateClear(seq);
    BrainRecDijBuf buf = {0};
    BrainRecDijStore store;
    brainRecDijStoreInit(&store);
    BrainRecDijState **snap = (BrainRecDijState **)calloc(NFRAMES, sizeof *snap);
    UT_ASSERT(snap);

    size_t total = 0, keyTotal = 0, deltaTotal = 0;
    size_t dangerTotal = 0, dangerKeyTotal = 0;
    int keyFrames = 0;
    int sx[NBOT][BRAINREC_DIJ_SLATES], sy[NBOT][BRAINREC_DIJ_SLATES];
    uint32_t st[NBOT][BRAINREC_DIJ_SLATES];
    memset(st, 0, sizeof st);

    for (int f = 0; f < NFRAMES; f++) {
        uint32_t brainTick = (uint32_t)f / 2u;
        for (int b = 0; b < NBOT; b++) {
            BrainRecDijBotIn *bi = &in[b];
            /* Tank: one tile every 8 frames along a loop; boat for a while;
             * dead for a few frames now and then. */
            int step = f / 8;
            bi->tx = 60 + b * 30 + (step % 40) - 20;
            bi->ty = 80 + ((step / 40) % 2 ? 10 : 0) + b * 5;
            bi->hasTank = (f % 900) < 860;
            bi->onBoat  = ((f / 300) % 3) == 1;
            /* Danger: hostile pills on the tank's route, each a diamond of
             * radius 8. A pill's anger goes up while it shoots and decays;
             * a pill dies now and then (its danger drops to 0). */
            bi->danger = dg[b];
            memset(dg[b], 0, sizeof dg[b]);
            for (int k = 0; k < 6; k++) {
                int px = 50 + b * 30 + k * 7, py = 75 + b * 5 + (k % 3) * 6;
                if (((f / 400) + k) % 5 == 0) continue;            /* dead */
                int anger = ((f + k * 37) % 120) < 30 ? ((f + k * 37) % 120) / 3 : 0;
                for (int dy = -8; dy <= 8; dy++) {
                    for (int dx = -8; dx <= 8; dx++) {
                        int md = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                        if (md > 8) continue;
                        int x = px + dx, y = py + dy;
                        if (x < 0 || x > 255 || y < 0 || y > 255) continue;
                        int v = dg[b][y * 256 + x] + (15 + anger) * (9 - md) / 9;
                        dg[b][y * 256 + x] = (uint16_t)(v > 65535 ? 65535 : v);
                    }
                }
            }
            for (int s = 0; s < BRAINREC_DIJ_SLATES; s++) {
                BrainRecDijSlateIn *si = &bi->slates[s];
                TSlate *t = &ts[b][s];
                int period = 60 + 20 * s;          /* frames between restarts */
                int age = (f + b * 13 + s * 17) % period;
                if (age == 0 || st[b][s] == 0) {
                    st[b][s] = brainTick + 1;
                    sx[b][s] = bi->tx; sy[b][s] = bi->ty;
                    tSlateReset(t);
                }
                if (s == 3 && (f / 500) % 2 == 1) { si->active = false; continue; }
                /* The search grows 6 tiles a frame until done. */
                int radius = (age + 1) * 6;
                bool done = radius >= 40;
                if (radius > 40) radius = 40;
                tSlateFill(t, sx[b][s], sy[b][s], radius, 10.0f + (float)s, s < 2,
                           (s == 0) ? 0.6f : 1.4f,
                           sx[b][s] - radius, sy[b][s] - radius,
                           sx[b][s] + radius, sy[b][s] + radius);
                si->active = true;
                si->g_cost = t->g;
                si->dir_at = t->d;
                tMeta(&si->meta, s, sx[b][s], sy[b][s], st[b][s], s < 2);
                si->meta.done = done ? 1 : 0;
                si->meta.completed_tick = done ? brainTick : 0;
            }
        }
        bool key = (f % KEYINT) == 0;
        UT_ASSERT(brainRecDijEncodeFrame(prev, bots, key, &buf));
        total += buf.len;
        if (key) { keyTotal += buf.len; keyFrames++; } else deltaTotal += buf.len;
        {
            TCount cc;
            UT_ASSERT(tCountBlock(buf.data, buf.len, &cc));
            dangerTotal += cc.dangerBytes;
            if (key) dangerKeyTotal += cc.dangerBytes;
        }
        UT_ASSERT(brainRecDijStoreSet(&store, f, buf.data, buf.len));
        UT_ASSERT(brainRecDijDecodeFrame(seq, buf.data, buf.len));

        /* The reader holds exactly the filtered slates, every frame. */
        for (int b = 0; b < NBOT; b++) {
            const BrainRecDijBotBox *bb = &seq->bots[slotOf[b]];
            UT_ASSERT(bb->present);
            if (!in[b].hasTank) { UT_ASSERT(!bb->boxValid); continue; }
            UT_ASSERT_MSG(tDangerMatches(bb, &in[b]),
                          "frame %d bot %d: the replayed danger box does not match "
                          "the grid", f, b);
            for (int s = 0; s < BRAINREC_DIJ_SLATES; s++) {
                UT_ASSERT_MSG(tBoxMatches(bb, s, &in[b]),
                              "frame %d bot %d slate %d: the replayed box does not "
                              "match the slate", f, b, s);
            }
        }
        /* The writer's memory is the reader's state. */
        UT_ASSERT_MSG(tStatesEqual(prev, seq), "frame %d: writer memory != reader", f);
        if (f % 37 == 0 || f % KEYINT == KEYINT - 1) {
            snap[f] = (BrainRecDijState *)malloc(sizeof *seq);
            UT_ASSERT(snap[f]);
            memcpy(snap[f], seq, sizeof *seq);
        }
    }

    /* Seek in a scattered order: each seek starts from the last keyframe
     * (or the cache) and must equal the full replay. */
    int order[] = {2999, 0, 1517, 499, 500, 1036, 37, 2997 - 2997 % 37, 999, 1480, 74, 2499};
    for (size_t k = 0; k < sizeof order / sizeof order[0]; k++) {
        int f = order[k];
        if (!snap[f]) continue;
        const BrainRecDijState *at = brainRecDijStoreAt(&store, f);
        UT_ASSERT(at);
        UT_ASSERT_MSG(tStatesEqual(at, snap[f]), "seek to frame %d differs from the replay", f);
    }
    /* A keyframe rebuild alone (no cache) equals the delta replay. */
    for (int f = 0; f < NFRAMES; f++) {
        if (!snap[f]) continue;
        BrainRecDijState *fresh = (BrainRecDijState *)malloc(sizeof *fresh);
        UT_ASSERT(fresh);
        brainRecDijStateClear(fresh);
        int k = f - f % KEYINT;
        UT_ASSERT(brainRecDijBlockIsKeyframe(store.data + store.frames[k].off, store.frames[k].len));
        for (int i = k; i <= f; i++)
            UT_ASSERT(brainRecDijDecodeFrame(fresh, store.data + store.frames[i].off,
                                             store.frames[i].len));
        UT_ASSERT_MSG(tStatesEqual(fresh, snap[f]),
                      "frame %d rebuilt from keyframe %d differs from the replay", f, k);
        free(fresh);
    }

    printf("  brainrec_dij size: %d bots, %d frames: %.1f bytes/tick/bot on average "
           "(delta frames %.1f, keyframes %.1f); %u bytes total\n",
           NBOT, NFRAMES, (double)total / NFRAMES / NBOT,
           (double)deltaTotal / (NFRAMES - keyFrames) / NBOT,
           (double)keyTotal / keyFrames / NBOT, (unsigned)total);
    printf("  brainrec_dij danger part: %.2f bytes/tick/bot on average "
           "(delta frames %.2f, keyframes %.1f); %u bytes total\n",
           (double)dangerTotal / NFRAMES / NBOT,
           (double)(dangerTotal - dangerKeyTotal) / (NFRAMES - keyFrames) / NBOT,
           (double)dangerKeyTotal / keyFrames / NBOT, (unsigned)dangerTotal);
    UT_ASSERT_MSG((double)total / NFRAMES / NBOT < 2000.0,
                  "%.1f bytes per tick per bot", (double)total / NFRAMES / NBOT);

    for (int f = 0; f < NFRAMES; f++) free(snap[f]);
    free(snap);
    brainRecDijStoreFree(&store);
    brainRecDijBufFree(&buf);
    free(prev); free(seq);
    for (int b = 0; b < NBOT; b++)
        for (int s = 0; s < BRAINREC_DIJ_SLATES; s++) tSlateFree(&ts[b][s]);
    return 0;
}

/* ---- danger box ----------------------------------------------------- */

int run_brainrec_dij_danger_box(void) {
    static uint16_t grid[65536];
    memset(grid, 0, sizeof grid);
    BrainRecDijState *prev = (BrainRecDijState *)malloc(sizeof *prev);
    BrainRecDijState *rd   = (BrainRecDijState *)malloc(sizeof *rd);
    UT_ASSERT(prev && rd);
    brainRecDijStateClear(prev);
    brainRecDijStateClear(rd);
    BrainRecDijBuf buf = {0};
    TCount c;

    /* A pill's danger in a 5x5 square round (102, 100), inside the box
     * round the tank at (100, 100); one more cell outside the box. No
     * slate: the danger box is written on its own. */
    for (int y = 98; y <= 102; y++)
        for (int x = 100; x <= 104; x++) grid[y * 256 + x] = (uint16_t)(20 + x - y);
    grid[100 * 256 + 140] = 99;
    static BrainRecDijBotIn in;
    memset(&in, 0, sizeof in);
    in.hasTank = true; in.tx = 100; in.ty = 100;
    in.danger = grid;
    const BrainRecDijBotIn *bots[BRAINREC_DIJ_MAX_BOTS] = {0};
    bots[5] = &in;

    /* Keyframe: every non-zero cell in the box, no 0s. */
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, true, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.dangerWrites == 25 && c.dangerZeros == 0,
                  "keyframe wrote %d danger cells (%d zeros), want 25 and 0",
                  c.dangerWrites, c.dangerZeros);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tDangerMatches(&rd->bots[5], &in));
    uint16_t v = 1;
    UT_ASSERT(brainRecDijDangerAt(&rd->bots[5], 102, 100, &v) && v == 22);
    UT_ASSERT_MSG(!brainRecDijDangerAt(&rd->bots[5], 140, 100, &v) && v == 0,
                  "a tile outside the box must read as not recorded");

    /* Nothing changed: no danger cell. */
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.dangerWrites == 0, "an unchanged frame wrote %d danger cells",
                  c.dangerWrites);

    /* The pill gets angry at one cell and a new cell gets danger: 2 writes. */
    grid[100 * 256 + 102] = 300;
    grid[96 * 256 + 100]  = 7;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.dangerWrites == 2 && c.dangerZeros == 0,
                  "two changed cells wrote %d (%d zeros)", c.dangerWrites, c.dangerZeros);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tDangerMatches(&rd->bots[5], &in));

    /* The pill dies: its 25 cells go to 0. Each is written as a 0 so the
     * reader clears it; the other non-zero cell stays. */
    for (int y = 98; y <= 102; y++)
        for (int x = 100; x <= 104; x++) grid[y * 256 + x] = 0;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.dangerWrites == 25 && c.dangerZeros == 25,
                  "a dead pill wrote %d cells (%d zeros), want 25 zeros",
                  c.dangerWrites, c.dangerZeros);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tDangerMatches(&rd->bots[5], &in));
    UT_ASSERT(brainRecDijDangerAt(&rd->bots[5], 102, 100, &v) && v == 0);

    /* Zero cells were not written again: the next frame writes nothing. */
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT(c.dangerWrites == 0);

    /* The tank moves 1 tile east. The new column has danger 0 except one
     * cell: 1 write, and no 0 for the cells that enter the box. The column
     * that leaves the box (x = 93) holds no danger. */
    grid[100 * 256 + 108] = 12;
    in.tx = 101;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.dangerWrites == 1 && c.dangerZeros == 0,
                  "a one-tile move wrote %d danger cells (%d zeros), want 1 and 0",
                  c.dangerWrites, c.dangerZeros);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tDangerMatches(&rd->bots[5], &in));

    /* The tank dies: no box and no danger cells. It comes back: every
     * non-zero cell is new. */
    in.hasTank = false;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT(c.dangerWrites == 0);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(!brainRecDijDangerAt(&rd->bots[5], 101, 100, NULL));
    in.hasTank = true;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT_MSG(c.dangerWrites == 2 && c.dangerZeros == 0,
                  "respawn wrote %d danger cells (%d zeros), want 2 and 0",
                  c.dangerWrites, c.dangerZeros);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tDangerMatches(&rd->bots[5], &in));

    /* No grid (NULL): every cell reads 0, so the two non-zero cells are
     * cleared with 0s. */
    in.danger = NULL;
    UT_ASSERT(brainRecDijEncodeFrame(prev, bots, false, &buf));
    UT_ASSERT(tCountBlock(buf.data, buf.len, &c));
    UT_ASSERT(c.dangerWrites == 2 && c.dangerZeros == 2);
    UT_ASSERT(brainRecDijDecodeFrame(rd, buf.data, buf.len));
    UT_ASSERT(tDangerMatches(&rd->bots[5], &in));
    UT_ASSERT(tStatesEqual(prev, rd));

    brainRecDijBufFree(&buf);
    free(prev); free(rd);
    return 0;
}

/* ---- old file without the section ---------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t n, pos;
} TMemSrc;

static size_t tMemRead(void *ctx, void *dst, size_t len) {
    TMemSrc *m = (TMemSrc *)ctx;
    size_t k = (m->n - m->pos) < len ? (m->n - m->pos) : len;
    memcpy(dst, m->p + m->pos, k);
    m->pos += k;
    return k;
}

static bool tMemSkip(void *ctx, size_t len) {
    TMemSrc *m = (TMemSrc *)ctx;
    if (m->n - m->pos < len) return false;
    m->pos += len;
    return true;
}

static void tPut(BrainRecDijBuf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n) nc *= 2;
        b->data = (uint8_t *)realloc(b->data, nc);
        b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void tPutU8(BrainRecDijBuf *b, uint8_t v)  { tPut(b, &v, 1); }
static void tPutU32(BrainRecDijBuf *b, uint32_t v) { tPut(b, &v, 4); }

/* A minimal file: header, empty legend, nFrames frames with no tanks, a
 * delta map with no changes and no bots, then the tails of `version`. */
static void tBuildFile(BrainRecDijBuf *f, uint32_t version, int nFrames,
                       const uint8_t *dij, uint32_t dijLen) {
    BrainRecHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, BRAINREC_MAGIC, BRAINREC_MAGIC_LEN);
    hdr.version = version;
    memcpy(hdr.mapName, "Test", 4);
    tPut(f, &hdr, sizeof hdr);
    tPutU32(f, 0);                         /* legend */
    for (int i = 0; i < nFrames; i++) {
        tPutU32(f, BRAINREC_FRAME_MAGIC);
        tPutU32(f, (uint32_t)(i * 2));
        tPutU8(f, 0); tPutU8(f, 0); tPutU8(f, 0); tPutU8(f, 0);  /* tanks shells bases pills */
        tPutU8(f, 0); tPutU32(f, 0);       /* map delta, 0 changes */
        tPutU8(f, 0);                      /* bots */
        for (int k = 0; k < MAX_TANKS; k++) tPutU32(f, 0);   /* alliances */
        if (version >= 7u) { tPutU32(f, 0x3u); tPutU32(f, 0x1u); }
        if (version >= 8u) { tPutU32(f, dijLen); if (dijLen) tPut(f, dij, dijLen); }
    }
}

static int tWalk(const BrainRecDijBuf *f, int *framesOut, uint32_t *lastTick) {
    TMemSrc m = { f->data, f->len, 0 };
    BrainRecReader r;
    memset(&r, 0, sizeof r);
    r.ctx = &m; r.read = tMemRead; r.skip = tMemSkip;
    if (brainRecWalkPreamble(&r, NULL, NULL) != BRAINREC_WALK_OK) return -1;
    BrainRecFrameInfo fi;
    BrainRecWalkStatus stt;
    int n = 0;
    while ((stt = brainRecWalkFrame(&r, &fi)) == BRAINREC_WALK_OK) { n++; *lastTick = fi.tick; }
    *framesOut = n;
    return stt == BRAINREC_WALK_EOF ? 0 : -2;
}

int run_brainrec_dij_old_file_loads(void) {
    BrainRecDijBuf f = {0};
    int n = 0;
    uint32_t last = 0;

    /* v7: no Dijkstra block. Still readable, and walks to a clean end. */
    tBuildFile(&f, 7u, 3, NULL, 0);
    {
        BrainRecHeader hdr;
        memcpy(&hdr, f.data, sizeof hdr);
        UT_ASSERT_MSG(brainRecVersionReadable(&hdr), "a v7 brainrec was refused");
    }
    UT_ASSERT_MSG(tWalk(&f, &n, &last) == 0 && n == 3 && last == 4,
                  "a v7 file walked %d frames (want 3)", n);

    /* v8 with a real block: the walker skips it by its length. */
    {
        BrainRecDijState *prev = (BrainRecDijState *)malloc(sizeof *prev);
        UT_ASSERT(prev);
        brainRecDijStateClear(prev);
        TSlate t;
        UT_ASSERT(tSlateAlloc(&t));
        tSlateFill(&t, 10, 10, 255, 5.0f, true, 0.5f, 0, 0, 255, 255);
        static BrainRecDijBotIn in;
        memset(&in, 0, sizeof in);
        in.hasTank = true; in.tx = 10; in.ty = 10;
        in.slates[0].active = true;
        in.slates[0].g_cost = t.g;
        in.slates[0].dir_at = t.d;
        tMeta(&in.slates[0].meta, 0, 10, 10, 1, 1);
        const BrainRecDijBotIn *bots[BRAINREC_DIJ_MAX_BOTS] = {0};
        bots[1] = &in;
        BrainRecDijBuf blk = {0};
        UT_ASSERT(brainRecDijEncodeFrame(prev, bots, true, &blk));
        f.len = 0;
        tBuildFile(&f, 8u, 4, blk.data, (uint32_t)blk.len);
        UT_ASSERT_MSG(tWalk(&f, &n, &last) == 0 && n == 4 && last == 6,
                      "a v8 file walked %d frames (want 4)", n);
        brainRecDijBufFree(&blk);
        tSlateFree(&t);
        free(prev);
    }

    /* A store fed only "no section" frames, as BrainTest feeds it for a
     * v7 file, has no data: playback keeps the old behaviour. */
    {
        BrainRecDijStore s;
        brainRecDijStoreInit(&s);
        for (int i = 0; i < 3; i++) UT_ASSERT(brainRecDijStoreSet(&s, i, NULL, 0));
        UT_ASSERT(!brainRecDijStoreHasData(&s));
        UT_ASSERT(brainRecDijStoreAt(&s, 1) == NULL);
        brainRecDijStoreFree(&s);
    }

    /* A truncated block is refused rather than read past its end. */
    {
        BrainRecDijState *st = (BrainRecDijState *)malloc(sizeof *st);
        UT_ASSERT(st);
        brainRecDijStateClear(st);
        const uint8_t bad[] = { 0x01, 0x01, 0x00, 0x01, 10, 10, 0x01, 0x24 };
        UT_ASSERT(!brainRecDijDecodeFrame(st, bad, sizeof bad));
        free(st);
    }

    brainRecDijBufFree(&f);
    return 0;
}
