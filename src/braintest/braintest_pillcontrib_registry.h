/*********************************************************
 * Per-pill, per-tile danger contribution registry.
 *
 * The brain (GoalHunter/threat.lua) precomputes a map of
 * which tiles each individual pill contributes danger to, and
 * by how much. The cost evaluator subtracts a target pill's
 * contribution from the approach cost so the pill we're about
 * to kill doesn't scare us off (`self_dr`).
 *
 * This registry lets the brain push the same per-pill data
 * into BrainTest each tick so we can render it as an overlay.
 * shift-2 in the main map cycles through pills one-at-a-time
 * for the *currently-followed* bot. Recording captures all
 * bots' data so scrubbing back + switching followed bot still
 * shows the right per-pill view.
 *
 * Storage is per-bot: each bot's Brain.think appends into its
 * own slot range. Display callers pass the followed bot index
 * to pillContribCount/Get. There is no concurrent-write risk
 * because each bot only writes to its own slot range.
 *
 * Mirrors the viz_detail registry's pattern: a live array
 * populated each brain tick, plus a playback override gated
 * by an explicit active-flag.
 *********************************************************/
#ifndef BRAINTEST_PILLCONTRIB_REGISTRY_H
#define BRAINTEST_PILLCONTRIB_REGISTRY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cap on the number of distinct pills tracked per tick per bot.
 * Bolo supports up to 16 pills on the map; we never need more. */
#define PILLCONTRIB_MAX_PILLS 16
/* Cap on tiles tracked per pill. A pill's danger footprint is
 * a ~7-tile-radius circle (~150 tiles max), padded for safety. */
#define PILLCONTRIB_MAX_TILES_PER_PILL 256
/* Cap on bot slots; matches Bolo MAX_TANKS. Kept locally so this
 * header doesn't pull in the engine's global.h. */
#define PILLCONTRIB_MAX_BOTS 16

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

/* Live-write API. The slot returned by pillContribBeginPill
 * encodes both bot index and within-bot slot, so subsequent
 * pillContribAddTile calls don't need to repeat the bot index. */
void pillContribClearAll(void);                                /* Reset all bots */
int  pillContribBeginPill(int bot, int pill_id, int mx, int my); /* slot or -1 */
void pillContribAddTile(int slot, int tx, int ty, float value);  /* slot from begin */

/* Read API — host renders from these. Honors the playback
 * override transparently when active. */
int  pillContribCount(int bot);
const PillContribEntry *pillContribGet(int bot, int idx);

/* Recording / playback. Playback snapshot is the full per-bot
 * layout — one entry-pointer array per bot plus the per-bot count.
 * Set with active=true on patch-in (with the recorded frame's
 * snapshot), clear on patch-out. NULL data with active=true is a
 * valid empty snapshot.
 *
 * Storage indirection: entries[bot] is NULL or points to an array of
 * `counts[bot]` (const PillContribEntry *) pointers. The recording
 * layer uses this to refcount/share individual pill entries across
 * frames (most frames have identical pcontrib state). */
typedef struct {
    const PillContribEntry *const *const *entries;
    const int                          *counts;
} PillContribSnapshot;

void pillContribSetPlaybackView(const PillContribSnapshot *snap);
void pillContribClearPlaybackView(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_PILLCONTRIB_REGISTRY_H */
