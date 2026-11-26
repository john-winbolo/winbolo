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
 * Name:          map_preview_fetch_policy.h
 * Purpose:       The one decision map_preview_fetch.cpp
 *                makes about a lookup that failed: whether
 *                the same map is looked up again, where, and
 *                when.
 *                Plain C and header-only so the unit tests
 *                reach it without the worker threads and
 *                the HTTP stack.
 *********************************************************/

#ifndef MAP_PREVIEW_FETCH_POLICY_H
#define MAP_PREVIEW_FETCH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* How long a map whose lookup failed stays as it is before it is looked up
 * again. The game finder asks every frame, so with no wait a machine with
 * no connection would post continuously; with no retry at all, one failed
 * request left the map without a preview until the game was restarted. */
#define MAP_PREVIEW_RETRY_MS 15000u

/* Where a map recorded as unavailable is looked for now. */
typedef enum {
    MAP_PREVIEW_LOOKUP_NONE = 0,  /* not yet: the wait is not over */
    MAP_PREVIEW_LOOKUP_LOCAL,     /* this machine's .map files only */
    MAP_PREVIEW_LOOKUP_FULL       /* this machine, then WinBolo.net */
} MapPreviewLookup;

typedef struct {
    bool     failed;     /* a lookup has failed */
    bool     answered;   /* WinBolo.net said it does not store the map */
    uint64_t retryAtMs;  /* earliest time of the next lookup */
} MapPreviewRetry;

/* Records a failed lookup. `answered` is WbnMapResult.answered: true when
 * WinBolo.net said it does not store the map. That answer is kept for the
 * session, so once it has been given it stays given: a later failure that
 * never reached WinBolo.net (a lookup of this machine's files only) does
 * not clear it. */
static inline void mapPreviewRetryNoteFailed(MapPreviewRetry *r, bool answered,
                                             uint64_t nowMs) {
    r->failed = true;
    r->answered = r->answered || answered;
    r->retryAtMs = nowMs + MAP_PREVIEW_RETRY_MS;
}

/* Where a map recorded as unavailable is to be looked for now. A map
 * WinBolo.net does not store is never asked for again, but the player can
 * still save it or subscribe to it during the session, so this machine's
 * files are searched again after each wait; that costs no request. A map
 * whose request failed on the way is asked for again as well. */
static inline MapPreviewLookup mapPreviewRetryDue(const MapPreviewRetry *r,
                                                  uint64_t nowMs) {
    if (!r->failed || nowMs < r->retryAtMs) {
        return MAP_PREVIEW_LOOKUP_NONE;
    }
    return r->answered ? MAP_PREVIEW_LOOKUP_LOCAL : MAP_PREVIEW_LOOKUP_FULL;
}

#endif /* MAP_PREVIEW_FETCH_POLICY_H */
