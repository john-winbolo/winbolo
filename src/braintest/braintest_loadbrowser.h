/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * BrainTest "Load Session" browser — lists debug_sessions/
 * recordings (winbolods brainrec.btr) and lets the user pick
 * one to replay. Loading relaunches BrainTest with
 * -loadsession <dir> and exits the current game.
 *
 * The ImGui rendering lives in the .cpp (C++); the scan +
 * relaunch logic is C in braintest_main.c. This header is the
 * C/C++ bridge.
 *********************************************************/
#ifndef BRAINTEST_LOADBROWSER_H
#define BRAINTEST_LOADBROWSER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOADBROWSER_MAX_SESSIONS 512
#define LOADBROWSER_MAX_NAME     64   /* LoadSessionEntry::name capacity */

typedef struct {
    char   dir[300];        /* path, e.g. "debug_sessions/20260624_163153" */
    char   name[LOADBROWSER_MAX_NAME];  /* basename, for display */
    char   map[64];         /* recorded map name */
    int    bots;            /* recorded player/bot count */
    int    durationSec;     /* wall-clock game length, -1 if unknown */
    double sizeMB;          /* brainrec.btr size */
    bool   loadable;        /* valid, version-matching brainrec.btr present */
    char   note[64];        /* reason shown when not loadable */
} LoadSessionEntry;

/* Render the browser window. *open toggles visibility (Cancel clears it).
 * Returns the index in `list` the user clicked "Load" on this frame, else -1.
 * loadedDir: the -loadsession dir this instance is replaying ("" / NULL when
 * none) — its row is highlighted and scrolled into view when the window
 * opens, so stepping to the next part of a split session is one glance.
 * needsRescan (optional): set true when a rename changed dir names on disk, so
 * the caller re-runs its scan before using `list` again. The Load return
 * contract is unchanged; a frame that sets needsRescan always returns -1. */
int loadBrowserRender(bool *open, const LoadSessionEntry *list, int count,
                      const char *loadedDir, bool *needsRescan);

/* ---- Segment walking (braintest_loadbrowser_segment.c, plain C) --------
 * A recording reaches disk as 15-minute BLOCKS ("<ts>_<B>[_<label>]", the
 * recorder) that brainrec_split may further cut into PARTS
 * ("<block>_part<X>of<N>"). These let the browser offer "next / previous
 * segment" instead of making the user read dir names. */

/* Basename of a session path: the text after the last '/' or '\', with any
 * trailing separators ignored. Always NUL-terminates (empty on bad input). */
void loadBrowserBaseName(const char *path, char *out, size_t outSz);

/* True when two paths name the same session — the browser stores
 * "debug_sessions/<name>" while -loadsession may carry any prefix or
 * separator style, so only the basenames are compared (case-insensitively
 * on Windows). */
bool loadBrowserSameSession(const char *a, const char *b);

/* Index in `list` of the segment that follows (direction +1) or precedes
 * (direction -1) the session `loadedDir`, or -1 when there is none.
 *   - inside a split block, the neighbouring _part<X>of<N>;
 *   - off either end of the parts (or from an unsplit block dir), the
 *     neighbouring block <B±1> — preferring that block's _part1of<M> going
 *     forward / _part<M>of<M> going back, else the block dir itself. */
int loadBrowserFindSegment(const LoadSessionEntry *list, int count,
                           const char *loadedDir, int direction);

/* ---- Name parsing + Rename (braintest_loadbrowser_rename.c, plain C) ---
 * The session-dir grammar lives here, in one place, and everything else
 * (segment walking, the rename planner, the UI) is written on top of it. */

/* Case-folded name compare — the same fold loadBrowserSameSession applies
 * (case-insensitive on Windows, exact elsewhere), for whole names rather
 * than paths. NULL is never equal to anything. */
bool loadBrowserNameEq(const char *a, const char *b);

/* Decompose a session BASENAME into its four pieces:
 *
 *   "20260903_011013_2_oilrig_part1of3"
 *     ts    = "20260903_011013"   (always 15 chars, the game's start)
 *     block = 2                   (0 when the dir is a bare timestamp)
 *     label = "oilrig"            ("" when the dir carries no label)
 *     partX/partN = 1/3           (0/0 when the dir is not a split part)
 *
 * Returns false — leaving every output cleared — for anything that is not a
 * recorder dir name ("handmade_session", "<ts>_0_x", a name longer than
 * LOADBROWSER_MAX_NAME). Any output pointer may be NULL. */
bool loadBrowserSplitName(const char *name, char *ts, size_t tsSz, int *block,
                          char *label, size_t labelSz, int *partX, int *partN);

/* Longest label a rename will accept. Well under the room left in a name
 * ("<ts>_<B>_<label>_part<X>of<N>" must still fit LOADBROWSER_MAX_NAME). */
#define LOADBROWSER_LABEL_MAX 32

/* Most (oldDir,newDir) pairs one rename can touch: blocks 1..N of a session
 * plus each block's parts. 15-minute blocks split a few ways over even a very
 * long game stay far below this. */
#define LOADBROWSER_PLAN_MAX  128

/* Plan the rename of the whole FAMILY that `anyMemberDir` belongs to: every
 * listed session sharing its timestamp AND its label, whatever the block
 * number and whether or not it is a _part<X>of<N>. Each pair keeps the
 * timestamp, block and part suffix and swaps in `newLabel` (NULL / "" drops
 * the label entirely).
 *
 * Returns the number of pairs written to `plan`, or -1 with a human-readable
 * reason in `err` when the rename is refused:
 *   - `newLabel` holds anything outside [A-Za-z0-9_-], or is too long;
 *   - `newLabel` is already the family's label (nothing to do);
 *   - a target name is already taken by another listed session;
 *   - a family member is `loadedDir`, the recording BrainTest is replaying;
 *   - the member is not a "<ts>_<B>[_<label>]" dir (a legacy bare-timestamp
 *     recording has no block number to hang a label off).
 * Touches no filesystem — the caller applies the plan. */
int loadBrowserRenameFamily(const LoadSessionEntry *list, int count,
                            const char *anyMemberDir, const char *newLabel,
                            const char *loadedDir,
                            char (*plan)[2][300], int planMax,
                            char *err, size_t errSz);

/* Apply a plan with SDL_RenamePath, parts before blocks. Returns the number
 * renamed (== n) on success, or -1 with `err` naming the pair that failed,
 * the SDL reason, and how many renames already stand — there is no rollback,
 * so the UI must say so. Refuses up front if any target already exists on
 * disk (a dir the scan skipped is still a collision). */
int loadBrowserApplyRenames(const char (*plan)[2][300], int n,
                            char *err, size_t errSz);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_LOADBROWSER_H */
