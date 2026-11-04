/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record_dij.h
 *PURPOSE:
 *  The recorded Dijkstra box and danger box in brainrec.btr (format v8).
 *
 *  Per bot, per tick, the recorder writes the bot's active Dijkstra
 *  slates, but only in a BRAINREC_DIJ_BOX x BRAINREC_DIJ_BOX box that
 *  is centred on that bot's tank tile, and only what changed since the
 *  last tick:
 *    - a slate record with the slate's identity (kind, source, boat
 *      state, started tick) and status when any of it changed. A new
 *      identity is a reset: the reader clears the slate's cells;
 *    - the nodes in the box whose g_cost or dir_at differ from what was
 *      last written for that node. A tile that newly enters the box
 *      counts as changed. Unreached nodes (COST_INF) are never written;
 *    - the boat layer is skipped for a slate with allow_boat = 0. When
 *      the tank is on land, a boat node is kept only if it is cheaper
 *      than the land node of the same tile.
 *  In the same box the recorder also writes the bot's pathfinder
 *  danger_grid (uint16 per tile), changed cells only. A cell that
 *  enters the box holds 0 for the reader, so a 0 is written only when
 *  the cell was non-zero before (so the reader clears it).
 *  On a map keyframe frame (BRAINREC_MAP_KEYFRAME_INTERVAL) the block is
 *  a keyframe too: it holds the whole box, so a reader that seeks needs
 *  to replay only from the last keyframe.
 *
 *  This module is pure C with no SDL, sim or GUI dependency: the
 *  recorder (brain_record.c) fills BrainRecDijBotIn from each bot's
 *  pathfinder and BrainTest reads the blobs back through the store, so
 *  the unit test exercises the same code both sides use. The name says
 *  "Dij" for the whole block, the danger box included.
 *
 *  Block layout (all native byte order, as the rest of the .btr):
 *    u8 flags                  bit0 = keyframe
 *    u8 botCount
 *    per bot:
 *      u8 slot
 *      u8 bflags               bit0 = box valid (the tank is on the map)
 *                              bit1 = the tank is on a boat
 *      u8 cx, u8 cy            box centre (tank tile), when box valid
 *      u8 recCount
 *      per slate record:
 *        u8 hdr                bits0-1 slate index, bit2 META,
 *                              bit3 RESET, bit4 INACTIVE, bit5 NODES
 *        META  (23 bytes):     i32 kind, u8 src_x, u8 src_y, u8 in_boat,
 *                              u8 allow_boat, u8 has_src_heading,
 *                              u8 src_heading, f32 danger_scale,
 *                              u32 started_tick, u8 done,
 *                              u32 completed_tick
 *        NODES:                for the land layer, then the boat layer:
 *                                u8 n, n x (u8 cell, f32 g_cost, u8 dir_at)
 *                              then for land, then boat:
 *                                u8 n, n x u8 cell   (cleared cells)
 *      u8 nd                   danger cells written (0 when box not valid)
 *      nd x (u8 cell, u16 danger)
 *                              a cell whose danger differs from what the
 *                              reader holds; 0 only to clear a cell
 *  A cell index is (y - cy + HALF) * BOX + (x - cx + HALF).
 *  In the frame the block is written as u32 length + bytes, so readers
 *  that do not decode it skip it by its length.
 *********************************************************/
#ifndef BRAIN_RECORD_DIJ_H
#define BRAIN_RECORD_DIJ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Box side in tiles. Odd, so the tank tile is the centre. */
#define BRAINREC_DIJ_BOX      15
#define BRAINREC_DIJ_HALF     ((BRAINREC_DIJ_BOX - 1) / 2)
#define BRAINREC_DIJ_CELLS    (BRAINREC_DIJ_BOX * BRAINREC_DIJ_BOX)
/* Same as DIJKSTRA_NUM_SLATES and MAX_TANKS; brain_record.c checks both. */
#define BRAINREC_DIJ_SLATES   4
#define BRAINREC_DIJ_MAX_BOTS 16
/* Same as the pathfinder's COST_INF; any cost at or above
 * BRAINREC_DIJ_INF_LIMIT is "not reached". */
#define BRAINREC_DIJ_COST_INF  1e30f
#define BRAINREC_DIJ_INF_LIMIT 1e29f
#define BRAINREC_DIJ_DIR_NONE  0xFF
/* Pathfinder node layout: land layer at [0, 65536), boat layer above. */
#define BRAINREC_DIJ_BOAT_OFFSET 65536

#define BRAINREC_DIJ_FLAG_KEYFRAME 0x01u
#define BRAINREC_DIJ_BOT_BOX       0x01u
#define BRAINREC_DIJ_BOT_ONBOAT    0x02u
#define BRAINREC_DIJ_REC_META      0x04u
#define BRAINREC_DIJ_REC_RESET     0x08u
#define BRAINREC_DIJ_REC_INACTIVE  0x10u
#define BRAINREC_DIJ_REC_NODES     0x20u

/* A slate's identity and status, as recorded. */
typedef struct {
    int32_t  kind;
    uint8_t  src_x, src_y;
    uint8_t  in_boat;
    uint8_t  allow_boat;
    uint8_t  has_src_heading;
    uint8_t  src_heading;
    float    danger_scale;
    uint32_t started_tick;
    uint8_t  done;
    uint32_t completed_tick;
} BrainRecDijMeta;

/* Recorder input: one slate of one bot, read from its pathfinder. */
typedef struct {
    bool            active;   /* the slate has arrays and holds a search */
    BrainRecDijMeta meta;
    const float    *g_cost;   /* full node array (land + boat layers) */
    const uint8_t  *dir_at;   /* full node array (land + boat layers) */
} BrainRecDijSlateIn;

/* Recorder input: one bot. */
typedef struct {
    bool hasTank;  /* false: no box this tick (dead, not spawned) */
    bool onBoat;
    int  tx, ty;   /* tank tile */
    BrainRecDijSlateIn slates[BRAINREC_DIJ_SLATES];
    /* The pathfinder's danger_grid, 256x256 at y * 256 + x. NULL = no
     * grid: every cell reads 0. */
    const uint16_t *danger;
} BrainRecDijBotIn;

/* One slate's recorded box. layer 0 = land, 1 = boat. A cell holds
 * BRAINREC_DIJ_COST_INF / BRAINREC_DIJ_DIR_NONE when nothing is
 * recorded for it. */
typedef struct {
    bool            active;   /* meta is known */
    BrainRecDijMeta meta;
    float           cost[2][BRAINREC_DIJ_CELLS];
    uint8_t         dir[2][BRAINREC_DIJ_CELLS];
} BrainRecDijSlateBox;

/* One bot's recorded state at a frame. */
typedef struct {
    bool present;   /* the bot was in this frame's block */
    bool boxValid;  /* cx, cy name a box */
    bool onBoat;
    int  cx, cy;
    BrainRecDijSlateBox slates[BRAINREC_DIJ_SLATES];
    uint16_t danger[BRAINREC_DIJ_CELLS];   /* recorded danger in the box */
} BrainRecDijBotBox;

/* Everything a reader (or the writer's memory) holds after a frame. */
typedef struct {
    BrainRecDijBotBox bots[BRAINREC_DIJ_MAX_BOTS];
} BrainRecDijState;

/* Growable byte buffer for an encoded block. */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} BrainRecDijBuf;

/* Set every bot and slate of a state to "nothing recorded". */
void brainRecDijStateClear(BrainRecDijState *st);

void brainRecDijBufFree(BrainRecDijBuf *b);

/* Encode one frame's block into out (out->len is reset first). bots is
 * indexed by slot; NULL = no bot in that slot. prev is the writer's memory
 * of the last frame: it is read for the diff and then updated to the state
 * a reader holds after decoding this block. keyframe writes the whole box.
 * Returns false only when out of memory. */
bool brainRecDijEncodeFrame(BrainRecDijState *prev,
                            const BrainRecDijBotIn *const bots[BRAINREC_DIJ_MAX_BOTS],
                            bool keyframe, BrainRecDijBuf *out);

/* Apply one frame's block to st. Returns false on a short or bad block
 * (st is then left as far as it got). A NULL / zero-length block is no
 * section at all and leaves st as it is. */
bool brainRecDijDecodeFrame(BrainRecDijState *st, const uint8_t *blob, size_t len);

/* Whether a block is a keyframe (false for an empty or NULL block). */
bool brainRecDijBlockIsKeyframe(const uint8_t *blob, size_t len);

/* ---- Lookups on a decoded bot box ---------------------------------- */

/* Cell index of map tile (x, y) in a bot's box, or -1 outside it (or when
 * the box is not valid). */
int brainRecDijCellIndex(const BrainRecDijBotBox *b, int x, int y);

/* Cheaper of the land and boat cost recorded at (x, y) for one slate.
 * Returns BRAINREC_DIJ_COST_INF when outside the box or not recorded.
 * *layerOut (may be NULL) gets 0 land / 1 boat. */
float brainRecDijCostAt(const BrainRecDijBotBox *b, int slate, int x, int y,
                        int *layerOut);

/* Recorded danger at map tile (x, y). Returns false when (x, y) is outside
 * the box or the box is not valid; *out (may be NULL) gets the value, or 0. */
bool brainRecDijDangerAt(const BrainRecDijBotBox *b, int x, int y, uint16_t *out);

/* Why a recorded path trace stopped. */
typedef enum {
    BRAINREC_DIJ_TRACE_NONE = 0,     /* (x, y) has no recorded cost */
    BRAINREC_DIJ_TRACE_SOURCE,       /* reached the slate source */
    BRAINREC_DIJ_TRACE_LEFT_BOX,     /* the next step is outside the box */
    BRAINREC_DIJ_TRACE_MISSING,      /* the next node is not recorded */
    BRAINREC_DIJ_TRACE_TOO_LONG      /* max steps reached */
} BrainRecDijTraceEnd;

/* Follow dir_at from (x, y) back toward the slate source, inside the box.
 * path_x/path_y get the tiles visited, (x, y) first. Returns the count;
 * *endOut (may be NULL) says why it stopped. */
int brainRecDijTrace(const BrainRecDijBotBox *b, int slate, int x, int y,
                     int *path_x, int *path_y, int max_steps,
                     BrainRecDijTraceEnd *endOut);

/* ---- Store: every frame's block, rebuilt on demand -------------------- */

typedef struct {
    uint32_t off;
    uint32_t len;   /* 0 = no section in this frame */
    bool     key;
} BrainRecDijFrameRef;

typedef struct {
    uint8_t             *data;
    size_t               len, cap;
    BrainRecDijFrameRef *frames;
    int                  count, cap_frames;
    int                  framesWithData;
    BrainRecDijState    *cache;      /* state after frame cacheFrame */
    int                  cacheFrame; /* -1 = cache empty */
} BrainRecDijStore;

void brainRecDijStoreInit(BrainRecDijStore *s);
void brainRecDijStoreFree(BrainRecDijStore *s);

/* Keep frame frameIdx's block (copied). Frames between the last one set
 * and frameIdx are filled as "no section". Returns false when out of
 * memory. */
bool brainRecDijStoreSet(BrainRecDijStore *s, int frameIdx,
                         const uint8_t *blob, size_t len);

/* True when at least one frame carried a section. */
bool brainRecDijStoreHasData(const BrainRecDijStore *s);

/* The state after frame frameIdx: replayed from the last keyframe at or
 * before it (or from the cached frame when that is nearer). Returns NULL
 * when frameIdx is out of range, the store has no data, or memory runs
 * out. The pointer stays valid until the next call. */
const BrainRecDijState *brainRecDijStoreAt(BrainRecDijStore *s, int frameIdx);

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_RECORD_DIJ_H */
