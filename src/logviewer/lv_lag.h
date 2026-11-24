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
 *  shell was drawn at that entry, each pillbox as it was drawn,
 *  and the map squares that changed in that entry. The lag
 *  view draws the tanks, builders and shells from the frame
 *  that is the player's rewind older than the present, and the
 *  pillboxes and the map from the frame that is the player's
 *  round trip older than the present.
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

/* The most records one track keeps: 131072, which is about 36 hours at
   one record a second, and 1 MB a track (16 MB for all 16 tracks). A real
   round writes less than one record a second for each player. A file that
   holds more keeps the first ones, so a crafted file cannot make the
   viewer keep more than this. */
#define LV_LAG_TRACK_MAX (1 << 17)

/* The frames the history keeps. The server rewinds at most 14 entries
   (LAG_COMP_MAX_TICKS); the ring keeps more so a value just over that,
   from a later server, still finds its frame. */
#define LV_LAG_FRAMES 32

/* The most shells, explosions included, one frame keeps. */
#define LV_LAG_SHELLS_MAX 256

/* The pillboxes one frame keeps: every one a map can hold (MAX_PILLS). */
#define LV_LAG_PILLS_MAX 16

/* The map is 256 squares on each side (MAP_ARRAY_SIZE). A map is held as
   the viewer holds it, mapItem[x][y]: the square at x, y is byte
   x * LV_LAG_MAP_SIDE + y. */
#define LV_LAG_MAP_SIDE 256

/* The most changed map squares one frame keeps. One entry of a real game
   changes a few squares: a shell hits a wall or a tree, a builder builds, a
   mine goes off. A frame that has more changes than this keeps the first
   ones and is marked incomplete (squaresFull). The map before an incomplete
   frame cannot be made again, so for a time before it the lag view draws
   the present map. */
#define LV_LAG_SQUARES_MAX 64

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
   setting on, the reel's playback keeps the history frames, and when the
   recording has lag records each seek of the reel starts its decode 640 ms
   early. */
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
    uint8_t shells;  /* its shell stock, as the recording states it */
    bool    fired;   /* the stock went down at this entry */
    bool    alive;   /* the tank was alive (gameViewHud alive), for the hit box */
} LvLagTank;

/* Where one builder was drawn at one entry. out is false when he was in
   his tank. */
typedef struct {
    bool    out;
    uint8_t mx, my, px, py, frame;
} LvLagLgm;

typedef struct {
    uint8_t mx, my, px, py, frame;
    int8_t  owner;   /* the player who fired it, or LV_SHELL_OWNER_NONE */
    uint16_t id;     /* the same for one shell in every frame (lv_shellOwnerLinkIds) */
} LvLagShell;

/* One pillbox as the map drew it at one entry: what the pillbox drawing and
   the pillbox numbers read. on is false for a slot the round has taken off
   the map. */
typedef struct {
    uint8_t x, y;
    uint8_t owner;
    uint8_t armour;
    bool    inTank;
    bool    on;
} LvLagPill;

/* One map square that changed in one entry: its place, the terrain before
   the entry and the terrain after it. */
typedef struct {
    uint8_t x, y;
    uint8_t was;
    uint8_t now;
} LvLagSquare;

typedef struct {
    uint32_t   ms;
    LvLagTank  tank[LV_LAG_SLOTS];
    LvLagLgm   lgm[LV_LAG_SLOTS];
    int        shellCount;
    LvLagShell shells[LV_LAG_SHELLS_MAX];
    LvLagPill  pills[LV_LAG_PILLS_MAX];
    uint8_t    pillCount;     /* the pillbox count at this entry */
    bool       squaresFull;   /* more squares changed than squares[] holds */
    uint16_t   squareCount;
    /* The squares that changed since the frame before. The first frame of
       the ring has none: it is where the ring starts. */
    LvLagSquare squares[LV_LAG_SQUARES_MAX];
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

/* How far back the lag view draws the pillboxes and the map for a player
   with a round trip of rttMs. A pillbox change or a map change is drawn on
   the player's screen when it gets there, with no delay, so when the
   player's input reaches the server the newest of them that player had was
   one round trip old. The round trip is taken in whole entries, rounded
   down, as the server rounds the view age down to whole entries for the
   rewind (serverSimComputeLagCompTicks). */
static inline uint32_t lv_lagRttBackMs(uint16_t rttMs) {
    return ((uint32_t) rttMs / LV_LAG_ENTRY_MS) * LV_LAG_ENTRY_MS;
}

/* The frame that is behindMs older than the present, nowMs. NULL when the
   newest frame is not the present's own (a frame noted before a seek is of
   another time), when behindMs is more than nowMs, or when the ring holds
   no frame that old. */
static inline const LvLagFrame *lv_lagRingBack(const LvLagRing *r, uint32_t nowMs,
                                              uint32_t behindMs) {
    if (r->count == 0 || r->frames[r->newest].ms != nowMs || behindMs > nowMs) {
        return NULL;
    }
    return lv_lagRingFind(r, nowMs - behindMs);
}

/* Note one changed square in f. Returns false, and marks f incomplete,
   when f holds LV_LAG_SQUARES_MAX squares already. */
static inline bool lv_lagFrameAddSquare(LvLagFrame *f, uint8_t x, uint8_t y, uint8_t was,
                                        uint8_t now) {
    LvLagSquare *sq;
    if (f->squareCount >= LV_LAG_SQUARES_MAX) {
        f->squaresFull = true;
        return false;
    }
    sq = &f->squares[f->squareCount++];
    sq->x = x;
    sq->y = y;
    sq->was = was;
    sq->now = now;
    return true;
}

/* Note in f each square where the map cur is not the same as prev, the map
   at the frame before, and make prev the same as cur. Both maps are
   LV_LAG_MAP_SIDE * LV_LAG_MAP_SIDE bytes. One line of squares is tested
   at a time, so a line with no change costs one compare. */
static inline void lv_lagMapDiff(uint8_t *prev, const uint8_t *cur, LvLagFrame *f) {
    int x, y;
    for (x = 0; x < LV_LAG_MAP_SIDE; x++) {
        uint8_t *p = prev + (size_t) x * LV_LAG_MAP_SIDE;
        const uint8_t *c = cur + (size_t) x * LV_LAG_MAP_SIDE;
        if (memcmp(p, c, LV_LAG_MAP_SIDE) == 0) {
            continue;
        }
        for (y = 0; y < LV_LAG_MAP_SIDE; y++) {
            if (p[y] != c[y]) {
                (void) lv_lagFrameAddSquare(f, (uint8_t) x, (uint8_t) y, p[y], c[y]);
            }
        }
        memcpy(p, c, LV_LAG_MAP_SIDE);
    }
}

/* The changed squares of the frames newer than target, which is a frame of
   r. Returns -1 when one of those frames is incomplete, or when target is
   not in r: then the map at target cannot be made again from the present
   one. */
static inline int lv_lagMapChangesAfter(const LvLagRing *r, const LvLagFrame *target) {
    int i, total = 0;
    for (i = 0; i < r->count; i++) {
        const LvLagFrame *g = &r->frames[(r->newest - i + LV_LAG_FRAMES) % LV_LAG_FRAMES];
        if (g == target) {
            return total;
        }
        if (g->squaresFull) {
            return -1;
        }
        total += g->squareCount;
    }
    return -1;
}

/* Make map, a copy of the present map, into the map as it was at target:
   undo the changes of every frame newer than target, the newest frame
   first and in each frame the last change first. Use it only when
   lv_lagMapChangesAfter gives 0 or more for the same target. */
static inline void lv_lagMapUndo(const LvLagRing *r, const LvLagFrame *target, uint8_t *map) {
    int i, j;
    for (i = 0; i < r->count; i++) {
        const LvLagFrame *g = &r->frames[(r->newest - i + LV_LAG_FRAMES) % LV_LAG_FRAMES];
        if (g == target) {
            return;
        }
        for (j = (int) g->squareCount - 1; j >= 0; j--) {
            const LvLagSquare *sq = &g->squares[j];
            map[(size_t) sq->x * LV_LAG_MAP_SIDE + sq->y] = sq->was;
        }
    }
}

/* The value lv_lagViewToggle gives when the view must stay as it is. */
#define LV_LAG_TOGGLE_NONE (-2)

/* The Options menu item "See What the Selected Player Saw" and its key, Ctrl+L
   (Cmd+L on macOS), use this to find what one click or press does. It gives
   the slot to give lv_screenLagViewSet: -1 turns the view off, a slot turns
   it on for that player. It gives LV_LAG_TOGGLE_NONE when the item is not
   there or is disabled, and then the view stays as it is.

   feature      the lag display preference is on
   loaded       a recording is open
   recorded     the recording has lag records
   sel          the selected tank's slot, or -1 for none
   viewSlot     the slot the view is on for, or -1 when it is off
   selHasRecord the selected player has a lag record at the playhead

   The view can be turned on only for a selected player with a record; a
   view that is on can always be turned off. When the view is on for the
   selected player, it turns off. When it is on for another player, it
   moves to the selected player when that player has a record, and turns
   off otherwise (no tank selected, or one with no record). */
static inline int lv_lagViewToggle(bool feature, bool loaded, bool recorded, int sel,
                                   int viewSlot, bool selHasRecord) {
    bool hasRecord = (sel >= 0) && selHasRecord;
    if (!feature || !loaded || !recorded || !(hasRecord || viewSlot >= 0)) {
        return LV_LAG_TOGGLE_NONE;
    }
    if (!hasRecord || viewSlot == sel) {
        return -1;
    }
    return sel;
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
