/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer player lag
 *Filename:      lv_lag.h
 *Purpose:
 *  Each player's network lag as the recording's log_PlayerLag
 *  records state it, and the short history of the world the
 *  "see what this player saw" view draws from.
 *
 *  A track holds one player's records in time order. The value
 *  at a time is the last record at or before it.
 *
 *  The history is a ring of frames, one for each 20 ms entry
 *  the viewer plays: where each tank, each builder and each
 *  shell was drawn at that entry. The lag view draws the frame
 *  that is the player's rewind older than the present.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer (C and C++) and the unit tests use the same code.
 *********************************************************/

#ifndef LV_LAG_H
#define LV_LAG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One slot for each player the game can hold (MAX_TANKS). */
#define LV_LAG_SLOTS 16

/* The time one rewind entry is: one log entry, one viewer tick. */
#define LV_LAG_ENTRY_MS 20

/* The most records one track keeps. A record a second for a whole day
   is far less than this; a file that holds more keeps the first ones. */
#define LV_LAG_TRACK_MAX (1 << 20)

/* The frames the history keeps. The server rewinds at most 14 entries
   (LAG_COMP_MAX_TICKS); the ring keeps more so a value just over that,
   from a later server, still finds its frame. */
#define LV_LAG_FRAMES 32

/* The most shells, explosions included, one frame keeps. */
#define LV_LAG_SHELLS_MAX 256

/* The samples the Tank panel's graph shows over the round. */
#define LV_LAG_GRAPH_SAMPLES 120

/* The preference that turns the viewer's lag display on: the Tank panel's
   lag line, graph and check box, the View menu item, the round-trip time in
   the HUD panel's tank tooltip, the banner and the delayed drawing. It is in
   the LOGVIEWER section of the preferences file, and it is off until set to
   "Yes". With it off the viewer still reads the lag records, but shows none
   of them and draws as it did before it read them.

   Every host reads it at setup (lvHostSetupCore): the standalone viewer, the
   game's Log Viewer and spectator view, which read the game's WinBolo.json,
   the browser build, and the in-game reel. The reel has no Tank panel, HUD
   panel or View menu, so the view cannot be turned on there; with the
   setting on, the reel's playback only keeps the history frames. */
#define LV_LAG_PREF_KEY     "Player Lag View"
#define LV_LAG_PREF_DEFAULT "No"

/* TRUE when the preference's value turns the display on: a value that
   starts with Y or y. Anything else, an empty value included, is off. */
static inline bool lv_lagPrefIsOn(const char *val) {
    return val != NULL && (val[0] == 'Y' || val[0] == 'y');
}

typedef struct {
    uint32_t ms;       /* playback time the record was read at */
    uint16_t rttMs;    /* round-trip time, ms */
    uint8_t  rewind;   /* rewind, in LV_LAG_ENTRY_MS entries */
} LvLagRecord;

typedef struct {
    LvLagRecord *recs;
    int          count;
    int          cap;
} LvLagTrack;

/* Empty the track and free what it holds. */
static inline void lv_lagTrackClear(LvLagTrack *t) {
    free(t->recs);
    t->recs  = NULL;
    t->count = 0;
    t->cap   = 0;
}

/* The index of the last record at or before ms, or -1. */
static inline int lv_lagTrackIndexAt(const LvLagTrack *t, uint32_t ms) {
    int lo = 0;
    int hi = t->count - 1;
    int best = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (t->recs[mid].ms <= ms) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

/* Add a record at ms, in time order. unlessPresent is what playback
   passes: when the track already has a record at ms, that record takes
   the new values and no second one is added. The load walk read the
   record first on a loaded file, and a seek back reads it again.
   Returns false when the track is full or out of memory. */
static inline bool lv_lagTrackAdd(LvLagTrack *t, uint32_t ms, uint16_t rttMs,
                                  uint8_t rewind, bool unlessPresent) {
    int at = lv_lagTrackIndexAt(t, ms);
    if (unlessPresent && at >= 0 && t->recs[at].ms == ms) {
        t->recs[at].rttMs  = rttMs;
        t->recs[at].rewind = rewind;
        return true;
    }
    if (t->count >= LV_LAG_TRACK_MAX) {
        return false;
    }
    if (t->count == t->cap) {
        int cap = t->cap == 0 ? 64 : t->cap * 2;
        LvLagRecord *grown;
        if (cap > LV_LAG_TRACK_MAX) {
            cap = LV_LAG_TRACK_MAX;
        }
        grown = (LvLagRecord *)realloc(t->recs, (size_t)cap * sizeof(LvLagRecord));
        if (grown == NULL) {
            return false;
        }
        t->recs = grown;
        t->cap  = cap;
    }
    /* After every record at or before ms, so two at one time stay in the
       order they were read. */
    at++;
    if (at < t->count) {
        memmove(&t->recs[at + 1], &t->recs[at],
                (size_t)(t->count - at) * sizeof(LvLagRecord));
    }
    t->recs[at].ms     = ms;
    t->recs[at].rttMs  = rttMs;
    t->recs[at].rewind = rewind;
    t->count++;
    return true;
}

/* The record in effect at ms: the last one at or before it. Returns NULL
   when the track has none that early. */
static inline const LvLagRecord *lv_lagTrackAt(const LvLagTrack *t, uint32_t ms) {
    int at = lv_lagTrackIndexAt(t, ms);
    return at < 0 ? NULL : &t->recs[at];
}

/* Where one tank was drawn at one entry. on is false when the slot had
   no tank on the map (no player, or a tank with no place yet). */
typedef struct {
    bool    on;
    uint8_t mx, my, px, py, frame;
    bool    onBoat;
} LvLagTank;

/* Where one builder was drawn at one entry. out is false when he was in
   his tank. */
typedef struct {
    bool    out;
    uint8_t mx, my, px, py, frame;
} LvLagLgm;

typedef struct {
    uint8_t mx, my, px, py, frame;
} LvLagShell;

typedef struct {
    uint32_t   ms;
    LvLagTank  tank[LV_LAG_SLOTS];
    LvLagLgm   lgm[LV_LAG_SLOTS];
    int        shellCount;
    LvLagShell shells[LV_LAG_SHELLS_MAX];
} LvLagFrame;

typedef struct {
    LvLagFrame frames[LV_LAG_FRAMES];
    int        newest;   /* index of the newest frame */
    int        count;    /* frames held, at most LV_LAG_FRAMES */
} LvLagRing;

static inline void lv_lagRingClear(LvLagRing *r) {
    r->newest = 0;
    r->count  = 0;
}

/* The frame to fill for the entry at ms, emptied. A time at or before the
   newest frame's starts the ring again: playback went back (a seek), and
   the frames held are of a later time. */
static inline LvLagFrame *lv_lagRingPush(LvLagRing *r, uint32_t ms) {
    LvLagFrame *f;
    if (r->count > 0 && ms <= r->frames[r->newest].ms) {
        r->count = 0;
    }
    r->newest = (r->count == 0) ? 0 : (r->newest + 1) % LV_LAG_FRAMES;
    if (r->count < LV_LAG_FRAMES) {
        r->count++;
    }
    f = &r->frames[r->newest];
    memset(f, 0, sizeof(*f));
    f->ms = ms;
    return f;
}

/* The newest frame at or before ms. Returns NULL when ms is before the
   oldest frame held, since the ring has no frame for that time. */
static inline const LvLagFrame *lv_lagRingFind(const LvLagRing *r, uint32_t ms) {
    int i;
    for (i = 0; i < r->count; i++) {
        int at = (r->newest - i + LV_LAG_FRAMES) % LV_LAG_FRAMES;
        if (r->frames[at].ms <= ms) {
            return &r->frames[at];
        }
    }
    return NULL;
}

/* What the Tank panel shows of one player's lag. */
typedef struct {
    bool  recorded;  /* the recording has lag records for some player */
    bool  has;       /* this player has a value at the playhead */
    bool  bot;       /* a bot drives the tank: it has no round trip, so the
                        panel shows the rewind only */
    int   rttMs;     /* round-trip time at the playhead */
    int   behindMs;  /* how far behind the present the player's screen was */
    /* The RTT over the recording, LV_LAG_GRAPH_SAMPLES samples spread
       from its start to its end; a sample before the player's first record
       is 0. */
    float graph[LV_LAG_GRAPH_SAMPLES];
    float graphMax;  /* the highest sample, at least 1 */
    float playhead;  /* where the playhead is across the graph, 0 to 1 */
} LvPlayerLag;

#endif /* LV_LAG_H */
