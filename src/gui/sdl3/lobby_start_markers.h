/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          lobby_start_markers.h
 * Purpose:       Shared rules for drawing reserved-start
 *                ownership markers on the lobby map
 *                previews (the small inline ##MapPanel and
 *                the full-screen popup).
 *
 *                Pure helpers only — ownership colour,
 *                compass-direction label placement, and
 *                minimal-unique-prefix disambiguation — so
 *                both call sites render starts identically.
 *                Header-only (static inline) to avoid a
 *                separate translation unit.
 *********************************************************/

#ifndef LOBBY_START_MARKERS_H
#define LOBBY_START_MARKERS_H

#include <string.h>
#include <stdlib.h>   /* abs */
#include <math.h>     /* atan2, floor */
#include "imgui.h"
#include "client_sim.h"   /* ClientSim, ClientLobbySlot, clientSimGetLobbySlot */
#include "global.h"       /* MAX_TANKS */

/* Ownership of a start relative to the local player. */
enum LobbyStartOwner {
    LSO_UNCLAIMED = 0,
    LSO_SELF,
    LSO_ALLY,
    LSO_ENEMY
};

/* Eight compass octants plus centre, mirroring lobbyStartCompassStr. */
enum LobbyCompassDir {
    LCD_C = 0, LCD_N, LCD_NE, LCD_E, LCD_SE, LCD_S, LCD_SW, LCD_W, LCD_NW
};

/* Connected slot holding 1-based start index i, or -1 if free. */
static inline int lobbyStartHolderSlot(ClientSim *cs, int startIdx1) {
    for (int k = 0; k < MAX_TANKS; k++) {
        const ClientLobbySlot *sl = clientSimGetLobbySlot(cs, (BYTE)k);
        if (sl && sl->connected && sl->startIdx == (uint8_t)startIdx1) return k;
    }
    return -1;
}

/* Classify a holder slot (<0 == unclaimed) relative to the local player. */
static inline LobbyStartOwner lobbyStartClassify(ClientSim *cs, int holderSlot,
                                                 int myPlayerNum) {
    if (holderSlot < 0) return LSO_UNCLAIMED;
    if (holderSlot == myPlayerNum) return LSO_SELF;
    if (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS) {
        uint8_t myTeam = clientSimGetLobbySlot(cs, (BYTE)myPlayerNum)->teamNumber;
        uint8_t hTeam  = clientSimGetLobbySlot(cs, (BYTE)holderSlot)->teamNumber;
        if (myTeam != 0 && hTeam == myTeam) return LSO_ALLY;
    }
    return LSO_ENEMY;
}

/* Marker / label colour for an ownership class. Self is a darker forest
 * green, allies a lighter green, enemies red, unclaimed yellow. */
static inline ImU32 lobbyStartOwnerColor(LobbyStartOwner o) {
    switch (o) {
        case LSO_SELF:  return IM_COL32( 34, 139,  34, 255);  /* darker green */
        case LSO_ALLY:  return IM_COL32(124, 232, 124, 255);  /* lighter green */
        case LSO_ENEMY: return IM_COL32(226,  74,  74, 255);  /* red */
        default:        return IM_COL32(236, 226,  64, 255);  /* yellow */
    }
}

/* Compass octant of start (sx,sy) within bbox [minX..maxX,minY..maxY].
 * Map Y grows downward; north = smaller y. Mirrors lobbyStartCompassStr
 * but returns the placement enum instead of a lang id. */
static inline LobbyCompassDir lobbyStartCompassDir(int sx, int sy,
                                                   int minX, int minY,
                                                   int maxX, int maxY) {
    int cx = (minX + maxX) / 2;
    int cy = (minY + maxY) / 2;
    int dx = sx - cx;
    int dy = sy - cy;
    int tolX = (maxX - minX) / 8; if (tolX < 1) tolX = 1;
    int tolY = (maxY - minY) / 8; if (tolY < 1) tolY = 1;
    if (abs(dx) <= tolX && abs(dy) <= tolY) return LCD_C;
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
    double deg = atan2((double)(-dy), (double)dx) * 180.0 / M_PI;
    int sector = (int)floor((deg + 22.5) / 45.0);
    sector = ((sector % 8) + 8) % 8;
    static const LobbyCompassDir kSectorDir[8] = {
        LCD_E, LCD_NE, LCD_N, LCD_NW, LCD_W, LCD_SW, LCD_S, LCD_SE
    };
    return kSectorDir[sector];
}

/* Unit screen-space direction (y down) to push a start's label so it
 * falls toward the map edge — under a south start, west of a west start,
 * etc. — keeping labels off the playable centre. */
static inline void lobbyCompassOffset(LobbyCompassDir d, float *ox, float *oy) {
    const float k = 0.70710678f;  /* diagonal unit component */
    switch (d) {
        case LCD_N:  *ox =  0.0f; *oy = -1.0f; break;
        case LCD_NE: *ox =  k;    *oy = -k;    break;
        case LCD_E:  *ox =  1.0f; *oy =  0.0f; break;
        case LCD_SE: *ox =  k;    *oy =  k;    break;
        case LCD_S:  *ox =  0.0f; *oy =  1.0f; break;
        case LCD_SW: *ox = -k;    *oy =  k;    break;
        case LCD_W:  *ox = -1.0f; *oy =  0.0f; break;
        case LCD_NW: *ox = -k;    *oy = -k;    break;
        default:     *ox =  0.0f; *oy =  1.0f; break;  /* centre -> below */
    }
}

/* Minimal number of leading chars of names[idx] needed to tell it apart
 * from every other name in the set. 1 char if already unique, more until
 * disambiguated; capped at the name's length (when another name is a
 * prefix of this one, or they're identical, the full name is returned). */
static inline int lobbyStartUniquePrefixLen(const char *const *names,
                                            int count, int idx) {
    const char *a = names[idx];
    if (!a || !a[0]) return 0;
    int need = 1;
    for (int j = 0; j < count; j++) {
        if (j == idx || !names[j]) continue;
        const char *b = names[j];
        int c = 0;
        while (a[c] && b[c] && a[c] == b[c]) c++;
        /* Need one char past the shared prefix to differ from b. If b
         * matches a all the way, prefixes can't separate them. */
        int cand = c + 1;
        if (cand > need) need = cand;
    }
    int la = (int)strlen(a);
    if (need > la) need = la;
    if (need < 1) need = 1;
    return need;
}

#endif /* LOBBY_START_MARKERS_H */
