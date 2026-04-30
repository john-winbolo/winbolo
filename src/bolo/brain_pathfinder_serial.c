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

#define SERIAL_MAGIC   0x53465042  /* "BPFS" */
#define SERIAL_VERSION 1

#define MAP_SIZE   256
#define GRID_SIZE  65536
#define NODE_COUNT 131072

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

static int bufAppendU32(Buf *b, uint32_t v) { return bufAppend(b, &v, 4); }
static int bufAppendFloat(Buf *b, float v)  { return bufAppend(b, &v, 4); }

/* Compress src into buf. Writes: compressed_size(4) + compressed_data. */
static int bufAppendCompressed(Buf *b, const void *src, size_t srcLen) {
    uLongf bound = compressBound((uLong)srcLen);
    if (!bufGrow(b, bound + 4)) return 0;
    uLongf destLen = bound;
    if (compress2(b->data + b->size + 4, &destLen,
                  (const Bytef *)src, (uLong)srcLen, 1) != Z_OK)
        return 0;
    uint32_t clen = (uint32_t)destLen;
    memcpy(b->data + b->size, &clen, 4);
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

static int curReadU32(Cursor *c, uint32_t *v) { return curRead(c, v, 4); }
static int curReadFloat(Cursor *c, float *v)  { return curRead(c, v, 4); }

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

    /* Terrain tables (3 × 16 floats) */
    bufAppend(&b, pf->terrain_cost_table,      sizeof(pf->terrain_cost_table));
    bufAppend(&b, pf->terrain_cost_boat_table,  sizeof(pf->terrain_cost_boat_table));
    bufAppend(&b, pf->terrain_speed_table,      sizeof(pf->terrain_speed_table));

    /* Grids (compressed together) */
    {
        size_t gridSize = sizeof(pf->danger_grid) + sizeof(pf->influence_grid)
                        + sizeof(pf->overlay_grid);
        unsigned char *tmp = (unsigned char *)malloc(gridSize);
        if (!tmp) { bufFree(&b); return NULL; }
        size_t off = 0;
        memcpy(tmp + off, pf->danger_grid, sizeof(pf->danger_grid));     off += sizeof(pf->danger_grid);
        memcpy(tmp + off, pf->influence_grid, sizeof(pf->influence_grid)); off += sizeof(pf->influence_grid);
        memcpy(tmp + off, pf->overlay_grid, sizeof(pf->overlay_grid));
        if (!bufAppendCompressed(&b, tmp, gridSize)) { free(tmp); bufFree(&b); return NULL; }
        free(tmp);
    }

    /* Dijkstra slates */
    bufAppendU32(&b, DIJKSTRA_NUM_SLATES);
    for (int s = 0; s < DIJKSTRA_NUM_SLATES; s++) {
        DijkstraSlate *sl = &pf->dij_slates[s];
        SlateHeader sh;
        sh.active = sl->active;
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
        bufAppend(&b, &sh, sizeof(sh));

        /* Only serialize arrays if the slate has been allocated and used */
        int hasData = sl->active && sl->g_cost != NULL;
        bufAppendU32(&b, hasData ? 1 : 0);

        if (hasData) {
            /* g_cost: float[NODE_COUNT] */
            if (!bufAppendCompressed(&b, sl->g_cost, NODE_COUNT * sizeof(float)))
                { bufFree(&b); return NULL; }
            /* closed: uint8_t[NODE_COUNT/8] */
            if (!bufAppendCompressed(&b, sl->closed, NODE_COUNT / 8))
                { bufFree(&b); return NULL; }
            /* dir_at: uint8_t[NODE_COUNT] */
            if (!bufAppendCompressed(&b, sl->dir_at, NODE_COUNT))
                { bufFree(&b); return NULL; }
            /* shells_at: int16_t[NODE_COUNT] (only if exact mode) */
            bufAppendU32(&b, sl->exact && sl->shells_at ? 1 : 0);
            if (sl->exact && sl->shells_at) {
                if (!bufAppendCompressed(&b, sl->shells_at, NODE_COUNT * sizeof(int16_t)))
                    { bufFree(&b); return NULL; }
            }
        }
    }

    /* Patch total size in header */
    {
        uint32_t totalSize = (uint32_t)b.size;
        memcpy(b.data + 8, &totalSize, 4);
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
    curRead(&c, pf->terrain_cost_table,      sizeof(pf->terrain_cost_table));
    curRead(&c, pf->terrain_cost_boat_table,  sizeof(pf->terrain_cost_boat_table));
    curRead(&c, pf->terrain_speed_table,      sizeof(pf->terrain_speed_table));

    /* Grids */
    {
        size_t gridSize = sizeof(pf->danger_grid) + sizeof(pf->influence_grid)
                        + sizeof(pf->overlay_grid);
        unsigned char *tmp = (unsigned char *)malloc(gridSize);
        if (!tmp) return 0;
        if (!curReadDecompress(&c, tmp, gridSize)) { free(tmp); return 0; }
        size_t off = 0;
        memcpy(pf->danger_grid, tmp + off, sizeof(pf->danger_grid));     off += sizeof(pf->danger_grid);
        memcpy(pf->influence_grid, tmp + off, sizeof(pf->influence_grid)); off += sizeof(pf->influence_grid);
        memcpy(pf->overlay_grid, tmp + off, sizeof(pf->overlay_grid));
        free(tmp);
    }

    /* Dijkstra slates */
    uint32_t numSlates;
    if (!curReadU32(&c, &numSlates)) return 0;
    if (numSlates > DIJKSTRA_NUM_SLATES) return 0;

    for (uint32_t s = 0; s < numSlates; s++) {
        DijkstraSlate *sl = &pf->dij_slates[s];
        SlateHeader sh;
        if (!curRead(&c, &sh, sizeof(sh))) return 0;

        sl->active = sh.active;
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
