/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "braintest_pillcontrib_registry.h"
#include <string.h>

/* Per-bot storage. g_entries[bot] is the bot's array of up to
 * PILLCONTRIB_MAX_PILLS PillContribEntry. g_count[bot] is the
 * current entry count (0..PILLCONTRIB_MAX_PILLS). */
static PillContribEntry g_entries[PILLCONTRIB_MAX_BOTS][PILLCONTRIB_MAX_PILLS];
static int              g_count[PILLCONTRIB_MAX_BOTS];

/* Playback override. NULL counts/entries when inactive. */
static bool                       g_pb_active = false;
static PillContribSnapshot        g_pb_snap;

/* Slot encoding: high byte = bot index, low byte = within-bot
 * slot (0..PILLCONTRIB_MAX_PILLS-1). Encoded slots stay positive
 * for valid (bot, idx) pairs; -1 reserved for "registry full" or
 * "bot out of range". */
static int encode_slot(int bot, int within) {
    return (bot << 8) | (within & 0xFF);
}
static int slot_bot(int slot)    { return (slot >> 8) & 0xFF; }
static int slot_within(int slot) { return slot & 0xFF; }

void pillContribClearAll(void) {
    for (int b = 0; b < PILLCONTRIB_MAX_BOTS; b++) g_count[b] = 0;
}

int pillContribBeginPill(int bot, int pill_id, int mx, int my) {
    if (bot < 0 || bot >= PILLCONTRIB_MAX_BOTS) return -1;
    if (g_count[bot] >= PILLCONTRIB_MAX_PILLS)  return -1;
    int within = g_count[bot]++;
    PillContribEntry *e = &g_entries[bot][within];
    e->pill_id    = pill_id;
    e->pill_mx    = (short)mx;
    e->pill_my    = (short)my;
    e->tile_count = 0;
    return encode_slot(bot, within);
}

void pillContribAddTile(int slot, int tx, int ty, float value) {
    int bot    = slot_bot(slot);
    int within = slot_within(slot);
    if (slot < 0
        || bot < 0 || bot >= PILLCONTRIB_MAX_BOTS
        || within < 0 || within >= g_count[bot]) return;
    PillContribEntry *e = &g_entries[bot][within];
    if (e->tile_count >= PILLCONTRIB_MAX_TILES_PER_PILL) return;
    PillContribTile *t = &e->tiles[e->tile_count++];
    t->tile_x = (short)tx;
    t->tile_y = (short)ty;
    t->value  = value;
}

void pillContribSetPlaybackView(const PillContribSnapshot *snap) {
    g_pb_active = true;
    if (snap) {
        g_pb_snap = *snap;
    } else {
        g_pb_snap.entries = NULL;
        g_pb_snap.counts  = NULL;
    }
}

void pillContribClearPlaybackView(void) {
    g_pb_active = false;
    g_pb_snap.entries = NULL;
    g_pb_snap.counts  = NULL;
}

int pillContribCount(int bot) {
    if (bot < 0 || bot >= PILLCONTRIB_MAX_BOTS) return 0;
    if (g_pb_active) {
        return (g_pb_snap.counts) ? g_pb_snap.counts[bot] : 0;
    }
    return g_count[bot];
}

const PillContribEntry *pillContribGet(int bot, int idx) {
    if (bot < 0 || bot >= PILLCONTRIB_MAX_BOTS) return NULL;
    int n = pillContribCount(bot);
    if (idx < 0 || idx >= n) return NULL;
    if (g_pb_active) {
        if (!g_pb_snap.entries || !g_pb_snap.entries[bot]) return NULL;
        return g_pb_snap.entries[bot][idx];
    }
    return &g_entries[bot][idx];
}
