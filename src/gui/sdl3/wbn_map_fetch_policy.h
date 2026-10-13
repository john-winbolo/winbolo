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
 * Purpose:       The two decisions the WinBolo.net map
 *                listing in wbn_map_source.cpp makes
 *                without touching the network or the
 *                chooser: when a listing that has not
 *                arrived is asked for again, and which
 *                folder to ask for when the path is one
 *                the catalogue cache has not met. Plain C
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
 * empty until the program was restarted. */
#define WBN_MAPS_RETRY_MS 3000u

/* Whether to send a request now. sameAsLast is true when the request
 * the listing wants is the one it sent most recently (and the answer is
 * not in the cache, or the caller would not be asking). A different
 * request goes at once; a repeat waits out WBN_MAPS_RETRY_MS from the
 * time the last one was sent. */
static inline bool wbnMapsFetchDue(bool sameAsLast, uint64_t nowMs,
                                   uint64_t lastKickMs) {
    if (!sameAsLast) return true;
    return (nowMs - lastKickMs) >= WBN_MAPS_RETRY_MS;
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

#endif /* WBN_MAP_FETCH_POLICY_H */
