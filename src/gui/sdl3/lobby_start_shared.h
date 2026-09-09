/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          lobby_start_shared.h
 * Purpose:       The rules for a lobby start several
 *                players have reserved (LOBBY_SHARED_STARTS).
 *
 *                Two of them: what the mini map's label says
 *                when a start has more than one holder — the
 *                comma-joined list of minimal unique name
 *                prefixes, "C, M" — and what one ownership
 *                class a start with several holders reads as.
 *
 *                Pure C string and integer helpers only: no
 *                ImGui, no ClientSim — so the unit tests
 *                compile the exact code the lobby draws from.
 *                Header-only (static inline) to avoid a
 *                separate translation unit.
 *********************************************************/

#ifndef LOBBY_START_SHARED_H
#define LOBBY_START_SHARED_H

#include <stddef.h>
#include <string.h>

/* Ownership of a start relative to the local player. The GUI's
 * LobbyStartOwner enum takes its values from these, so a folded class can
 * be handed straight to lobbyStartOwnerColor. */
#define LOBBY_START_OWNER_UNCLAIMED 0
#define LOBBY_START_OWNER_SELF      1
#define LOBBY_START_OWNER_ALLY      2
#define LOBBY_START_OWNER_ENEMY     3

/* Minimal number of leading chars of names[idx] needed to tell it apart
 * from every other name in the set. 1 char if already unique, more until
 * disambiguated; capped at the name's length (when another name is a
 * prefix of this one, or they're identical, the full name is returned). */
static inline int lobbyStartUniquePrefixLen(const char *const *names,
                                            int count, int idx) {
    const char *a;
    int need = 1;
    int j;
    int la;
    if (!names || idx < 0 || idx >= count) return 0;
    a = names[idx];
    if (!a || !a[0]) return 0;
    for (j = 0; j < count; j++) {
        const char *b;
        int c = 0;
        int cand;
        if (j == idx || !names[j]) continue;
        b = names[j];
        while (a[c] && b[c] && a[c] == b[c]) c++;
        /* Need one char past the shared prefix to differ from b. If b
         * matches a all the way, prefixes can't separate them. */
        cand = c + 1;
        if (cand > need) need = cand;
    }
    la = (int)strlen(a);
    if (need > la) need = la;
    if (need < 1) need = 1;
    return need;
}

/* The mini map's label for the holders of one start: each holder's
 * minimal unique prefix, joined with ", " in the order given — "C, M".
 * names/nameCount is the whole set of holder names on the map, the set
 * lobbyStartUniquePrefixLen disambiguates against; holders[] lists this
 * start's indices into it. Truncates rather than overruns and always
 * NUL-terminates. Returns the number of holders that fitted. */
static inline int lobbyStartHolderPrefixLabel(const char *const *names,
                                              int nameCount,
                                              const int *holders, int nHolders,
                                              char *buf, size_t bufLen) {
    size_t used = 0;
    int written = 0;
    int h;
    if (!buf || bufLen == 0) return 0;
    buf[0] = '\0';
    if (!names || !holders) return 0;
    for (h = 0; h < nHolders; h++) {
        int idx = holders[h];
        int plen;
        const char *nm;
        size_t sep = (written > 0) ? 2u : 0u;   /* ", " */
        if (idx < 0 || idx >= nameCount) continue;
        nm = names[idx];
        if (!nm || !nm[0]) continue;
        plen = lobbyStartUniquePrefixLen(names, nameCount, idx);
        /* Stop at the first name that would not fit whole, so the label
         * never ends in half a prefix. */
        if (used + sep + (size_t)plen + 1u > bufLen) break;
        if (sep) {
            buf[used++] = ',';
            buf[used++] = ' ';
        }
        memcpy(buf + used, nm, (size_t)plen);
        used += (size_t)plen;
        buf[used] = '\0';
        written++;
    }
    return written;
}

/* The same list in full names rather than prefixes — what the popup's
 * tooltip names when several players hold one start. */
static inline int lobbyStartHolderNameLabel(const char *const *names,
                                            int nNames,
                                            char *buf, size_t bufLen) {
    size_t used = 0;
    int written = 0;
    int h;
    if (!buf || bufLen == 0) return 0;
    buf[0] = '\0';
    if (!names) return 0;
    for (h = 0; h < nNames; h++) {
        const char *nm = names[h];
        size_t len;
        size_t sep = (written > 0) ? 2u : 0u;   /* ", " */
        if (!nm || !nm[0]) continue;
        len = strlen(nm);
        if (used + sep + len + 1u > bufLen) break;
        if (sep) {
            buf[used++] = ',';
            buf[used++] = ' ';
        }
        memcpy(buf + used, nm, len);
        used += len;
        buf[used] = '\0';
        written++;
    }
    return written;
}

/* The one ownership class a start with several holders reads as: enemy if
 * any holder is an enemy (the start is contested, so it must warn), self
 * if the viewer is among the holders, ally otherwise; unclaimed when
 * nobody holds it. owners[] is one LOBBY_START_OWNER_* per holder. */
static inline int lobbyStartFoldOwners(const int *owners, int count) {
    int best = LOBBY_START_OWNER_UNCLAIMED;
    int i;
    if (!owners || count <= 0) return LOBBY_START_OWNER_UNCLAIMED;
    for (i = 0; i < count; i++) {
        if (owners[i] == LOBBY_START_OWNER_ENEMY) return LOBBY_START_OWNER_ENEMY;
        if (owners[i] == LOBBY_START_OWNER_SELF) {
            best = LOBBY_START_OWNER_SELF;
        } else if (owners[i] == LOBBY_START_OWNER_ALLY &&
                   best != LOBBY_START_OWNER_SELF) {
            best = LOBBY_START_OWNER_ALLY;
        }
    }
    return best;
}

#endif /* LOBBY_START_SHARED_H */
