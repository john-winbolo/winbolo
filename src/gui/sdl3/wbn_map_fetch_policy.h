/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          wbn_map_fetch_policy.h
 * Purpose:       The decisions the WinBolo.net map
 *                listing in wbn_map_source.cpp makes
 *                without touching the network or the
 *                chooser: when a listing that has not
 *                arrived is asked for again and when
 *                the listing stops asking, which
 *                folder to ask for when the path is one
 *                the catalogue cache has not met, and
 *                when to show the folder that did load
 *                in place of one that is gone. Plain C
 *                and header-only so the unit tests reach
 *                them without the HTTP and chooser stack.
 *********************************************************/

#ifndef WBN_MAP_FETCH_POLICY_H
#define WBN_MAP_FETCH_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How long the listing waits before it asks again for something it has
 * already asked for and still does not hold. The provider runs every
 * frame, so without a wait a request that keeps failing would be sent
 * back-to-back; with no retry at all, one failed request left the tab
 * empty until the program was restarted. This is the wait after the
 * first send; each further send of the same request doubles it. */
#define WBN_MAPS_RETRY_MS 1000u

/* The longest the doubled wait is allowed to grow to. */
#define WBN_MAPS_RETRY_MAX_MS 2000u

/* How many times in a row the same request is sent before the listing
 * stops asking. A folder the server no longer has would otherwise be
 * asked for, with a signed request each time, for as long as the tab is
 * open. */
#define WBN_MAPS_RETRY_MAX_SENDS 3u

/* The longest one listing or search request may take, connect and reply
 * together, before it counts as failed. The HTTP module's own limit for
 * a GET is 30 s; a server that does not answer at all would hold each
 * send for that long. This limit is for the map list only: the map file
 * download and the other WinBolo.net requests keep their own. */
#define WBN_MAPS_REQUEST_TIMEOUT_MS 3000u

/* The longest the player waits, from the first send of a request to the
 * error text, when nothing ever answers. wbnMapsWorstCaseMs() below must
 * stay at or under it; the unit test checks that. */
#define WBN_MAPS_GIVE_UP_BUDGET_MS 10000u

/* The sends of one request that has not been answered. Zeroed is the
 * starting state. The caller knows what the request is (a folder id, a
 * search string) and tells the functions below whether the one it wants
 * now is the one counted here. */
typedef struct WbnMapsRetry {
    unsigned sends;      /* times the current request has been sent */
    uint64_t lastKickMs; /* when it was last sent */
} WbnMapsRetry;

/* The wait before the same request is sent again, given how many times
 * it has been sent so far: 1 s after the first, then 2 s, never more
 * than WBN_MAPS_RETRY_MAX_MS. */
static inline uint64_t wbnMapsRetryWaitMs(unsigned sends) {
    uint64_t wait = WBN_MAPS_RETRY_MS;
    unsigned i;
    for (i = 1; i < sends && wait < WBN_MAPS_RETRY_MAX_MS; i++) wait *= 2;
    return wait < WBN_MAPS_RETRY_MAX_MS ? wait : WBN_MAPS_RETRY_MAX_MS;
}

/* Whether the listing has given up on a request: it is the one counted
 * in r and it has been sent WBN_MAPS_RETRY_MAX_SENDS times. */
static inline bool wbnMapsRetriesStopped(const WbnMapsRetry *r,
                                         bool sameAsLast) {
    return sameAsLast && r->sends >= WBN_MAPS_RETRY_MAX_SENDS;
}

/* Whether to send a request now. sameAsLast is true when the request
 * the listing wants is the one it sent most recently (and the answer is
 * not in the cache, or the caller would not be asking). A different
 * request goes at once. A repeat waits out wbnMapsRetryWaitMs from the
 * time the last one was sent, and is not sent at all once the retries
 * have stopped. */
static inline bool wbnMapsFetchDue(const WbnMapsRetry *r, bool sameAsLast,
                                   uint64_t nowMs) {
    if (!sameAsLast) return true;
    if (wbnMapsRetriesStopped(r, sameAsLast)) return false;
    return (nowMs - r->lastKickMs) >= wbnMapsRetryWaitMs(r->sends);
}

/* Record that the request was sent at nowMs. A different request from
 * the last one starts the count again. */
static inline void wbnMapsRetryNoteSent(WbnMapsRetry *r, bool sameAsLast,
                                        uint64_t nowMs) {
    r->sends      = sameAsLast ? r->sends + 1 : 1;
    r->lastKickMs = nowMs;
}

/* Forget the count: the answer arrived, or the chooser was reopened. */
static inline void wbnMapsRetryReset(WbnMapsRetry *r) {
    r->sends      = 0;
    r->lastKickMs = 0;
}

/* The longest time from the first send of a request to the moment the
 * listing gives up on it. A send is made only when the one before has
 * ended and its wait (counted from when it was sent) has passed, so each
 * send but the last costs the larger of the request time limit and its
 * wait, and the last costs the request time limit. One provider frame
 * per send comes on top of this and is not counted. */
static inline uint64_t wbnMapsWorstCaseMs(void) {
    uint64_t total = 0;
    unsigned sends;
    for (sends = 1; sends < WBN_MAPS_RETRY_MAX_SENDS; sends++) {
        uint64_t wait = wbnMapsRetryWaitMs(sends);
        total += wait > WBN_MAPS_REQUEST_TIMEOUT_MS
                     ? wait : WBN_MAPS_REQUEST_TIMEOUT_MS;
    }
    return total + WBN_MAPS_REQUEST_TIMEOUT_MS;
}

/* Length of the parent of the folder path held in path[0..len): the
 * text before its last separator, or 0 (the catalogue root) when it has
 * none. Used to walk a path the cache cannot resolve up to the deepest
 * folder it can, and for the chooser's "[..]" row. */
static inline size_t wbnMapsParentLen(const char *path, size_t len) {
    while (len > 0) {
        len--;
        if (path[len] == '/' || path[len] == '\\') return len;
    }
    return 0;
}

/* Looks up the folder path held in path[0..len) in the catalogue's path
 * to folder id table. Returns true and writes the id when the table has
 * the path. */
typedef bool (*WbnMapsLookupFn)(void *ctx, const char *path, size_t len,
                                int *idOut);

/* The folder id to ask for when the chooser wants relPath. The table
 * only knows folders some listing has named, and a starred folder can
 * be opened before its parent was ever listed. So this walks up to the
 * deepest ancestor the table does know and returns its id (the root,
 * id 0, when it knows none): that listing names the next folder down,
 * so the walk gets one level closer per fetch instead of stopping at
 * the root. */
static inline int wbnMapsResolveTarget(const char *relPath,
                                       WbnMapsLookupFn lookup, void *ctx) {
    size_t len = 0;
    int id = 0;
    while (relPath[len] != '\0') len++;
    for (;;) {
        if (lookup(ctx, relPath, len, &id)) return id;
        if (len == 0) return 0;
        len = wbnMapsParentLen(relPath, len);
    }
}

/* Whether to move the chooser onto the folder the cache holds. Asked
 * when the cached listing is not the path the chooser wants and no
 * request is in flight. True when the cache holds the folder the wanted
 * path resolves to: the wanted folder is gone or was renamed, and
 * asking for the same id again would return the same listing.
 *
 * Never at the root, which can always be asked for by itself. And a
 * cached listing that has a path but came with no id (it reads as 0)
 * is not the root, so it does not match a path that resolves to the
 * root. Without these two, a listing with no id would be copied over
 * the root path and the chooser could not get back to the root. */
static inline bool wbnMapsRepointDue(bool wantedIsRoot, bool cachedIsRoot,
                                     int cachedId, int targetId) {
    if (wantedIsRoot) return false;
    if (cachedId == 0 && !cachedIsRoot) return false;
    return cachedId == targetId;
}

#endif /* WBN_MAP_FETCH_POLICY_H */
