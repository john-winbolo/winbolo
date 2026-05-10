/*********************************************************
 * Per-pill, per-tile danger contribution registry.
 *
 * The brain (NewAutopilot/threat.lua) precomputes a map of
 * which tiles each individual pill contributes danger to, and
 * by how much. The cost evaluator subtracts a target pill's
 * contribution from the approach cost so the pill we're about
 * to kill doesn't scare us off (`self_dr`).
 *
 * This registry lets the brain push the same per-pill data
 * into BrainTest each tick so we can render it as an overlay.
 * shift-2 in the main map cycles through pills one-at-a-time.
 *
 * Mirrors the viz_detail registry's pattern: a live array
 * populated each brain tick, plus a playback override gated
 * by an explicit active-flag (so a recorded-frame with zero
 * pills doesn't fall through to the live data).
 *********************************************************/
#ifndef BRAINTEST_PILLCONTRIB_REGISTRY_H
#define BRAINTEST_PILLCONTRIB_REGISTRY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cap on the number of distinct pills tracked per tick. Bolo
 * supports up to 16 pills on the map; we never need more. */
#define PILLCONTRIB_MAX_PILLS 16
/* Cap on tiles tracked per pill. A pill's danger footprint is
 * a ~7-tile-radius circle (~150 tiles max), padded for safety. */
#define PILLCONTRIB_MAX_TILES_PER_PILL 256

typedef struct {
    short tile_x;
    short tile_y;
    float value;
} PillContribTile;

typedef struct {
    int  pill_id;       /* Stable across ticks (Lua's pill.id) */
    short pill_mx;
    short pill_my;
    int  tile_count;
    PillContribTile tiles[PILLCONTRIB_MAX_TILES_PER_PILL];
} PillContribEntry;

/* Live-write API — brain calls these each tick. */
void pillContribClear(void);                /* Reset live registry */
int  pillContribBeginPill(int pill_id, int mx, int my); /* Returns slot or -1 */
void pillContribAddTile(int slot, int tx, int ty, float value);

/* Read API — host renders from these. Honors the playback
 * override transparently when active. */
int  pillContribCount(void);
const PillContribEntry *pillContribGet(int idx);

/* Playback override. Set with active=true on patch-in (with the
 * recorded frame's snapshot), clear on patch-out. NULL data with
 * active=true is a valid empty snapshot. */
void pillContribSetPlaybackView(const PillContribEntry *entries, int count);
void pillContribClearPlaybackView(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_PILLCONTRIB_REGISTRY_H */
