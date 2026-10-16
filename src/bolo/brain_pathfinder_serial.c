/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * brain_pathfinder_serial.c — Serialize / deserialize BrainPathfinder state
 *
 * Used for exact trace replay: captures grids, config, and Dijkstra slate
 * state so smart_cost returns identical values in the replay environment.
 *
 * Binary format (all little-endian):
 *   [Header]  "BPFS" (4) + version(4) + total_uncompressed_size(4)
 *   [Config]  scalars + terrain tables (fixed size, uncompressed)
 *   [Grids]   danger_grid + influence_grid + overlay_grid (zlib compressed)
 *   [Slates]  For each of DIJKSTRA_NUM_SLATES:
 *               slate metadata (fixed struct)
 *               if active: g_cost + closed + dir_at [+ shells_at if exact]
 *                          (zlib compressed per-slate)
 */

#include <string.h>
#include <stdlib.h>
#include <zlib.h>

#include "brain_pathfinder.h"
#include "byte_order.h"

#define SERIAL_MAGIC   0x53465042  /* "BPFS" */
#define SERIAL_VERSION 1

#define MAP_SIZE   256
#define GRID_SIZE  65536
#define NODE_COUNT 131072
/* Entries in each of the three terrain tables, from the array itself so the
 * writer and reader cannot disagree with it. The three are the same size. */
#define TERRAIN_TABLE_LEN \
    (sizeof(((BrainPathfinder *)0)->terrain_cost_table) / sizeof(float))

/* ── Helper: grow a dynamic buffer ──────────────────────────── */

typedef struct {
    unsigned char *data;
    size_t         size;
    size_t         cap;
} Buf;

static void bufInit(Buf *b) { b->data = NULL; b->size = 0; b->cap = 0; }
static void bufFree(Buf *b) { free(b->data); bufInit(b); }

static int bufGrow(Buf *b, size_t need) {
    if (b->size + need <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->size + need) nc *= 2;
    unsigned char *p = (unsigned char *)realloc(b->data, nc);
    if (!p) return 0;
    b->data = p;
    b->cap = nc;
    return 1;
}

static int bufAppend(Buf *b, const void *src, size_t n) {
    if (!bufGrow(b, n)) return 0;
    memcpy(b->data + b->size, src, n);
    b->size += n;
    return 1;
}

static int bufAppendU32(Buf *b, uint32_t v) {
    unsigned char le[4];
    byteOrderPutLe32(le, v);
    return bufAppend(b, le, 4);
}
static int bufAppendFloat(Buf *b, float v) {
    unsigned char le[4];
    byteOrderPutLeFloat(le, v);
    return bufAppend(b, le, 4);
}

/* Arrays as little-endian bytes and back. The decoders may work in place,
 * dst and src the same memory: each value is read before it is written. */
static void putLe16Array(unsigned char *dst, const uint16_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) byteOrderPutLe16(dst + 2 * i, src[i]);
}
static void getLe16Array(uint16_t *dst, const unsigned char *src, size_t n) {
    for (size_t i = 0; i < n; i++) { uint16_t v = byteOrderGetLe16(src + 2 * i); dst[i] = v; }
}
static void putLeFloatArray(unsigned char *dst, const float *src, size_t n) {
    for (size_t i = 0; i < n; i++) byteOrderPutLeFloat(dst + 4 * i, src[i]);
}
static void getLeFloatArray(float *dst, const unsigned char *src, size_t n) {
    for (size_t i = 0; i < n; i++) { float v = byteOrderGetLeFloat(src + 4 * i); dst[i] = v; }
}

/* Compress src into buf. Writes: compressed_size(4) + compressed_data. */
static int bufAppendCompressed(Buf *b, const void *src, size_t srcLen) {
    uLongf bound = compressBound((uLong)srcLen);
    if (!bufGrow(b, bound + 4)) return 0;
    uLongf destLen = bound;
    if (compress2(b->data + b->size + 4, &destLen,
                  (const Bytef *)src, (uLong)srcLen, 1) != Z_OK)
        return 0;
    uint32_t clen = (uint32_t)destLen;
    byteOrderPutLe32(b->data + b->size, clen);
    b->size += 4 + clen;
    return 1;
}

/* ── Helper: read from a cursor ─────────────────────────────── */

typedef struct {
    const unsigned char *data;
    size_t               size;
    size_t               pos;
} Cursor;

static int curRead(Cursor *c, void *dst, size_t n) {
    if (c->pos + n > c->size) return 0;
    memcpy(dst, c->data + c->pos, n);
    c->pos += n;
    return 1;
}

static int curReadU32(Cursor *c, uint32_t *v) {
    unsigned char le[4];
    if (!curRead(c, le, 4)) return 0;
    *v = byteOrderGetLe32(le);
    return 1;
}
static int curReadFloat(Cursor *c, float *v) {
    unsigned char le[4];
    if (!curRead(c, le, 4)) return 0;
    *v = byteOrderGetLeFloat(le);
    return 1;
}
static int curReadI32(Cursor *c, int32_t *v) {
    uint32_t u;
    if (!curReadU32(c, &u)) return 0;
    *v = (int32_t)u;
    return 1;
}

/* Decompress: reads compressed_size(4) + data, inflates into dst of dstLen. */
static int curReadDecompress(Cursor *c, void *dst, size_t dstLen) {
    uint32_t clen;
    if (!curReadU32(c, &clen)) return 0;
    if (c->pos + clen > c->size) return 0;
    uLongf ulen = (uLongf)dstLen;
    if (uncompress((Bytef *)dst, &ulen, c->data + c->pos, clen) != Z_OK)
        return 0;
    if (ulen != (uLongf)dstLen) return 0;
    c->pos += clen;
    return 1;
}

/* ── Slate metadata (fixed-size, uncompressed) ──────────────── */

typedef struct {
    int32_t  active, done, exact, kind;
    int32_t  src_x, src_y, in_boat, src_shells;
    int32_t  expanded, peak_open;
    float    max_cost, danger_scale;
    uint32_t version, started_tick, completed_tick;
    int32_t  open_count;
} SlateHeader;

/* ── Serialize ──────────────────────────────────────────────── */

unsigned char *brainPathfinderSerialize(BrainPathfinder *pf, size_t *out_size) {
    if (!pf || !out_size) return NULL;
    *out_size = 0;

    Buf b;
    bufInit(&b);

    /* Header */
    bufAppendU32(&b, SERIAL_MAGIC);
    bufAppendU32(&b, SERIAL_VERSION);
    bufAppendU32(&b, 0);  /* placeholder for total size */

    /* Config scalars */
    bufAppendFloat(&b, pf->turn_cost);
    bufAppendFloat(&b, pf->wall_shoot_cost);
    bufAppendFloat(&b, pf->wall_shoot_shells);
    bufAppendFloat(&b, pf->shell_reserve);
    bufAppendFloat(&b, pf->road_build_cost);
    bufAppendFloat(&b, pf->tree_reserve);
    bufAppendFloat(&b, pf->mine_penalty);
    bufAppendFloat(&b, pf->estimate_samples);
    bufAppendFloat(&b, pf->danger_scale);
    bufAppendFloat(&b, pf->water_drain_rate);
    bufAppendFloat(&b, pf->shell_loss_cost);
    bufAppendFloat(&b, pf->mine_loss_cost);
    bufAppendFloat(&b, pf->armour_drain_rate);
    bufAppendFloat(&b, pf->road_build_danger_max);
    bufAppendFloat(&b, pf->min_shells);
    bufAppendFloat(&b, pf->min_mines);
    bufAppendFloat(&b, pf->min_armour);

    /* Terrain tables (3 × TERRAIN_TABLE_LEN floats) */
    for (size_t i = 0; i < TERRAIN_TABLE_LEN; i++) bufAppendFloat(&b, pf->terrain_cost_table[i]);
    for (size_t i = 0; i < TERRAIN_TABLE_LEN; i++) bufAppendFloat(&b, pf->terrain_cost_boat_table[i]);
    for (size_t i = 0; i < TERRAIN_TABLE_LEN; i++) bufAppendFloat(&b, pf->terrain_speed_table[i]);

    /* Grids (compressed together) */
    {
        size_t gridSize = sizeof(pf->danger_grid) + sizeof(pf->influence_grid)
                        + sizeof(pf->overlay_grid);
        unsigned char *tmp = (unsigned char *)malloc(gridSize);
        if (!tmp) { bufFree(&b); return NULL; }
        size_t off = 0;
        putLe16Array(tmp + off, pf->danger_grid, GRID_SIZE);                       off += sizeof(pf->danger_grid);
        putLe16Array(tmp + off, (const uint16_t *)pf->influence_grid, GRID_SIZE); off += sizeof(pf->influence_grid);
        putLe16Array(tmp + off, (const uint16_t *)pf->overlay_grid, GRID_SIZE);
        if (!bufAppendCompressed(&b, tmp, gridSize)) { free(tmp); bufFree(&b); return NULL; }
        free(tmp);
    }

    /* Dijkstra slates */
    bufAppendU32(&b, DIJKSTRA_NUM_SLATES);
    for (int s = 0; s < DIJKSTRA_NUM_SLATES; s++) {
        DijkstraSlate *sl = &pf->dij_slates[s];
        SlateHeader sh;
        sh.active = (sl->g_cost != NULL) ? 1 : 0;
        sh.done = sl->done;
        sh.exact = sl->exact;
        sh.kind = sl->kind;
        sh.src_x = sl->src_x;
        sh.src_y = sl->src_y;
        sh.in_boat = sl->in_boat;
        sh.src_shells = sl->src_shells;
        sh.expanded = sl->expanded;
        sh.peak_open = sl->peak_open;
        sh.max_cost = sl->max_cost;
        sh.danger_scale = sl->danger_scale;
        sh.version = sl->version;
        sh.started_tick = sl->started_tick;
        sh.completed_tick = sl->completed_tick;
        sh.open_count = sl->open_count;
        bufAppendU32(&b, (uint32_t)sh.active);
        bufAppendU32(&b, (uint32_t)sh.done);
        bufAppendU32(&b, (uint32_t)sh.exact);
        bufAppendU32(&b, (uint32_t)sh.kind);
        bufAppendU32(&b, (uint32_t)sh.src_x);
        bufAppendU32(&b, (uint32_t)sh.src_y);
        bufAppendU32(&b, (uint32_t)sh.in_boat);
        bufAppendU32(&b, (uint32_t)sh.src_shells);
        bufAppendU32(&b, (uint32_t)sh.expanded);
        bufAppendU32(&b, (uint32_t)sh.peak_open);
        bufAppendFloat(&b, sh.max_cost);
        bufAppendFloat(&b, sh.danger_scale);
        bufAppendU32(&b, sh.version);
        bufAppendU32(&b, sh.started_tick);
        bufAppendU32(&b, sh.completed_tick);
        bufAppendU32(&b, (uint32_t)sh.open_count);

        /* Only serialize arrays if the slate has been allocated and used */
        int hasData = sl->g_cost != NULL;
        bufAppendU32(&b, hasData ? 1 : 0);

        if (hasData) {
            /* g_cost: float[NODE_COUNT], encoded into a scratch buffer
             * that shells_at reuses below */
            unsigned char *le = (unsigned char *)malloc(NODE_COUNT * sizeof(float));
            if (!le) { bufFree(&b); return NULL; }
            putLeFloatArray(le, sl->g_cost, NODE_COUNT);
            if (!bufAppendCompressed(&b, le, NODE_COUNT * sizeof(float)))
                { free(le); bufFree(&b); return NULL; }
            /* closed: uint8_t[NODE_COUNT/8] */
            if (!bufAppendCompressed(&b, sl->closed, NODE_COUNT / 8))
                { free(le); bufFree(&b); return NULL; }
            /* dir_at: uint8_t[NODE_COUNT] */
            if (!bufAppendCompressed(&b, sl->dir_at, NODE_COUNT))
                { free(le); bufFree(&b); return NULL; }
            /* shells_at: int16_t[NODE_COUNT] (only if exact mode) */
            bufAppendU32(&b, sl->exact && sl->shells_at ? 1 : 0);
            if (sl->exact && sl->shells_at) {
                putLe16Array(le, (const uint16_t *)sl->shells_at, NODE_COUNT);
                if (!bufAppendCompressed(&b, le, NODE_COUNT * sizeof(int16_t)))
                    { free(le); bufFree(&b); return NULL; }
            }
            free(le);
        }
    }

    /* Patch total size in header */
    {
        byteOrderPutLe32(b.data + 8, (uint32_t)b.size);
    }

    *out_size = b.size;
    return b.data;
}

/* ── Deserialize ────────────────────────────────────────────── */

int brainPathfinderDeserialize(BrainPathfinder *pf,
                                const unsigned char *data, size_t size) {
    if (!pf || !data || size < 12) return 0;

    Cursor c;
    c.data = data;
    c.size = size;
    c.pos = 0;

    uint32_t magic, version, totalSize;
    if (!curReadU32(&c, &magic) || magic != SERIAL_MAGIC) return 0;
    if (!curReadU32(&c, &version) || version != SERIAL_VERSION) return 0;
    if (!curReadU32(&c, &totalSize)) return 0;

    /* Config scalars */
    curReadFloat(&c, &pf->turn_cost);
    curReadFloat(&c, &pf->wall_shoot_cost);
    curReadFloat(&c, &pf->wall_shoot_shells);
    curReadFloat(&c, &pf->shell_reserve);
    curReadFloat(&c, &pf->road_build_cost);
    curReadFloat(&c, &pf->tree_reserve);
    curReadFloat(&c, &pf->mine_penalty);
    curReadFloat(&c, &pf->estimate_samples);
    curReadFloat(&c, &pf->danger_scale);
    curReadFloat(&c, &pf->water_drain_rate);
    curReadFloat(&c, &pf->shell_loss_cost);
    curReadFloat(&c, &pf->mine_loss_cost);
    curReadFloat(&c, &pf->armour_drain_rate);
    curReadFloat(&c, &pf->road_build_danger_max);
    curReadFloat(&c, &pf->min_shells);
    curReadFloat(&c, &pf->min_mines);
    curReadFloat(&c, &pf->min_armour);

    /* Terrain tables */
    for (size_t i = 0; i < TERRAIN_TABLE_LEN; i++) curReadFloat(&c, &pf->terrain_cost_table[i]);
    for (size_t i = 0; i < TERRAIN_TABLE_LEN; i++) curReadFloat(&c, &pf->terrain_cost_boat_table[i]);
    for (size_t i = 0; i < TERRAIN_TABLE_LEN; i++) curReadFloat(&c, &pf->terrain_speed_table[i]);

    /* Grids */
    {
        size_t gridSize = sizeof(pf->danger_grid) + sizeof(pf->influence_grid)
                        + sizeof(pf->overlay_grid);
        unsigned char *tmp = (unsigned char *)malloc(gridSize);
        if (!tmp) return 0;
        if (!curReadDecompress(&c, tmp, gridSize)) { free(tmp); return 0; }
        size_t off = 0;
        getLe16Array(pf->danger_grid, tmp + off, GRID_SIZE);                 off += sizeof(pf->danger_grid);
        getLe16Array((uint16_t *)pf->influence_grid, tmp + off, GRID_SIZE); off += sizeof(pf->influence_grid);
        getLe16Array((uint16_t *)pf->overlay_grid, tmp + off, GRID_SIZE);
        free(tmp);
    }

    /* Dijkstra slates */
    uint32_t numSlates;
    if (!curReadU32(&c, &numSlates)) return 0;
    if (numSlates > DIJKSTRA_NUM_SLATES) return 0;

    for (uint32_t s = 0; s < numSlates; s++) {
        DijkstraSlate *sl = &pf->dij_slates[s];
        SlateHeader sh;
        if (!curReadI32(&c, &sh.active) || !curReadI32(&c, &sh.done) ||
            !curReadI32(&c, &sh.exact) || !curReadI32(&c, &sh.kind) ||
            !curReadI32(&c, &sh.src_x) || !curReadI32(&c, &sh.src_y) ||
            !curReadI32(&c, &sh.in_boat) || !curReadI32(&c, &sh.src_shells) ||
            !curReadI32(&c, &sh.expanded) || !curReadI32(&c, &sh.peak_open) ||
            !curReadFloat(&c, &sh.max_cost) || !curReadFloat(&c, &sh.danger_scale) ||
            !curReadU32(&c, &sh.version) || !curReadU32(&c, &sh.started_tick) ||
            !curReadU32(&c, &sh.completed_tick) || !curReadI32(&c, &sh.open_count)) return 0;

        /* sh.active is kept in wire format for compatibility but ignored —
         * has_data is determined by whether g_cost gets allocated below. */
        sl->done = sh.done;
        sl->exact = sh.exact;
        sl->kind = sh.kind;
        sl->src_x = sh.src_x;
        sl->src_y = sh.src_y;
        sl->in_boat = sh.in_boat;
        sl->src_shells = sh.src_shells;
        sl->expanded = sh.expanded;
        sl->peak_open = sh.peak_open;
        sl->max_cost = sh.max_cost;
        sl->danger_scale = sh.danger_scale;
        sl->version = sh.version;
        sl->started_tick = sh.started_tick;
        sl->completed_tick = sh.completed_tick;
        sl->open_count = sh.open_count;

        uint32_t hasData;
        if (!curReadU32(&c, &hasData)) return 0;

        if (hasData) {
            /* Ensure arrays are allocated */
            if (!sl->g_cost) {
                sl->g_cost = (float *)malloc(NODE_COUNT * sizeof(float));
                sl->closed = (uint8_t *)malloc(NODE_COUNT / 8);
                sl->dir_at = (uint8_t *)malloc(NODE_COUNT);
                sl->heap = NULL;
                sl->heap_capacity = 0;
            }
            if (!sl->g_cost || !sl->closed || !sl->dir_at) return 0;

            if (!curReadDecompress(&c, sl->g_cost, NODE_COUNT * sizeof(float))) return 0;
            getLeFloatArray(sl->g_cost, (const unsigned char *)sl->g_cost, NODE_COUNT);
            if (!curReadDecompress(&c, sl->closed, NODE_COUNT / 8)) return 0;
            if (!curReadDecompress(&c, sl->dir_at, NODE_COUNT)) return 0;

            uint32_t hasShells;
            if (!curReadU32(&c, &hasShells)) return 0;
            if (hasShells) {
                if (!sl->shells_at) {
                    sl->shells_at = (int16_t *)malloc(NODE_COUNT * sizeof(int16_t));
                }
                if (!sl->shells_at) return 0;
                if (!curReadDecompress(&c, sl->shells_at, NODE_COUNT * sizeof(int16_t))) return 0;
                getLe16Array((uint16_t *)sl->shells_at, (const unsigned char *)sl->shells_at, NODE_COUNT);
            }

            /* Heap is not serialized — mark slate as done (no stepping needed).
             * The lookup functions only read g_cost/closed, not the heap. */
            sl->open_count = 0;
            sl->done = 1;
        }
    }

    /* Invalidate edge cost cache so it gets rebuilt from restored map */
    pf->edge_cost_valid = 0;

    return 1;
}
