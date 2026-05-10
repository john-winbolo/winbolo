#include "braintest_pillcontrib_registry.h"
#include <string.h>

static PillContribEntry g_entries[PILLCONTRIB_MAX_PILLS];
static int              g_count = 0;

/* Playback override (mirrors viz_detail registry). */
static bool                    g_pb_active  = false;
static const PillContribEntry *g_pb_entries = NULL;
static int                     g_pb_count   = 0;

void pillContribClear(void) {
    g_count = 0;
}

int pillContribBeginPill(int pill_id, int mx, int my) {
    if (g_count >= PILLCONTRIB_MAX_PILLS) return -1;
    int slot = g_count++;
    PillContribEntry *e = &g_entries[slot];
    e->pill_id    = pill_id;
    e->pill_mx    = (short)mx;
    e->pill_my    = (short)my;
    e->tile_count = 0;
    return slot;
}

void pillContribAddTile(int slot, int tx, int ty, float value) {
    if (slot < 0 || slot >= g_count) return;
    PillContribEntry *e = &g_entries[slot];
    if (e->tile_count >= PILLCONTRIB_MAX_TILES_PER_PILL) return;
    PillContribTile *t = &e->tiles[e->tile_count++];
    t->tile_x = (short)tx;
    t->tile_y = (short)ty;
    t->value  = value;
}

void pillContribSetPlaybackView(const PillContribEntry *entries, int count) {
    g_pb_active  = true;
    g_pb_entries = entries;
    g_pb_count   = (entries && count > 0) ? count : 0;
}

void pillContribClearPlaybackView(void) {
    g_pb_active  = false;
    g_pb_entries = NULL;
    g_pb_count   = 0;
}

int pillContribCount(void) {
    return g_pb_active ? g_pb_count : g_count;
}

const PillContribEntry *pillContribGet(int idx) {
    int n = pillContribCount();
    if (idx < 0 || idx >= n) return NULL;
    if (g_pb_active) return &g_pb_entries[idx];
    return &g_entries[idx];
}
