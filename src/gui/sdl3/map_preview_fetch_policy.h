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
 *                the same map is looked up again, and when.
 *                Plain C and header-only so the unit tests
 *                reach it without the worker threads and
 *                the HTTP stack.
 *********************************************************/

#ifndef MAP_PREVIEW_FETCH_POLICY_H
#define MAP_PREVIEW_FETCH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* How long a map whose lookup got no answer from WinBolo.net (no
 * connection, a timeout, 429, a 5xx) stays unavailable before it is looked
 * up again. The game finder asks every frame, so with no wait a machine
 * with no connection would post continuously; with no retry at all, one
 * failed request left the map without a preview until the game was
 * restarted. */
#define MAP_PREVIEW_RETRY_MS 15000u

typedef struct {
    bool     retry;      /* the last lookup failed without an answer */
    uint64_t retryAtMs;  /* earliest time of the next lookup */
} MapPreviewRetry;

/* Records a failed lookup. `answered` is WbnMapResult.answered: true when
 * WinBolo.net said it does not store the map, which is final. */
static inline void mapPreviewRetryNoteFailed(MapPreviewRetry *r, bool answered,
                                             uint64_t nowMs) {
    r->retry = !answered;
    r->retryAtMs = nowMs + MAP_PREVIEW_RETRY_MS;
}

/* True when a map recorded as unavailable is to be looked up again now. */
static inline bool mapPreviewRetryDue(const MapPreviewRetry *r,
                                      uint64_t nowMs) {
    return r->retry && nowMs >= r->retryAtMs;
}

#endif /* MAP_PREVIEW_FETCH_POLICY_H */
