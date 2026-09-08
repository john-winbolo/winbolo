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
 * Name:          lobby_start_markers.h
 * Purpose:       Shared rules for drawing reserved-start
 *                ownership markers on the lobby map
 *                previews (the small inline ##MapPanel and
 *                the full-screen popup).
 *
 *                Pure helpers only — ownership colour,
 *                compass-direction label placement,
 *                minimal-unique-prefix disambiguation, and
 *                the team start-side rules (which team a
 *                start is off-side for, and the tooltip
 *                that says so) — so both call sites and the
 *                player list render and judge starts
 *                identically. Header-only (static inline)
 *                to avoid a separate translation unit.
 *********************************************************/

#ifndef LOBBY_START_MARKERS_H
#define LOBBY_START_MARKERS_H

#include <string.h>
#include <SDL3/SDL.h>     /* SDL_strlcpy — team label and tooltip copies */
#include "imgui.h"
#include "client_sim.h"   /* ClientSim, ClientLobbySlot, clientSimGetLobbySlot, team side / name */
#include "global.h"       /* MAX_TANKS */
#include "start_sides.h"  /* startSideMaskFor / Accepts / Bits — side mask and the side rules */
#include "../lang.h"      /* STR_DLGLOBBY_SIDE_* / STR_COMPASS_* / STR_STARTPICK_TIP_OFFSIDE* */

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

/* The same colour at half its alpha — how both previews dim the label of
 * a start the viewer's team side rejects, so ownership still reads. */
static inline ImU32 lobbyStartDimColor(ImU32 c) {
    ImU32 a = (c >> IM_COL32_A_SHIFT) & 0xFFu;
    return (c & ~(0xFFu << IM_COL32_A_SHIFT)) | ((a / 2u) << IM_COL32_A_SHIFT);
}

/* Compass direction of start (sx,sy) within bbox [minX..maxX,minY..maxY],
 * as the label-placement enum. Read off the same side mask that gives
 * lobbyStartCompassStr its lang id: one bit is a cardinal, two bits a
 * diagonal, no bits the centre band. */
static inline LobbyCompassDir lobbyStartCompassDir(int sx, int sy,
                                                   int minX, int minY,
                                                   int maxX, int maxY) {
    switch (startSideMaskFor(sx, sy, minX, minY, maxX, maxY)) {
        case START_SIDE_BIT_N:                    return LCD_N;
        case START_SIDE_BIT_N | START_SIDE_BIT_E: return LCD_NE;
        case START_SIDE_BIT_E:                    return LCD_E;
        case START_SIDE_BIT_S | START_SIDE_BIT_E: return LCD_SE;
        case START_SIDE_BIT_S:                    return LCD_S;
        case START_SIDE_BIT_S | START_SIDE_BIT_W: return LCD_SW;
        case START_SIDE_BIT_W:                    return LCD_W;
        case START_SIDE_BIT_N | START_SIDE_BIT_W: return LCD_NW;
        default:                                  return LCD_C;
    }
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

/* ── Team start sides ─────────────────────────────────────────────
 * The lobby mirror carries each team's START_SIDE_* choice; start_sides.h
 * says which starts a side accepts. These are the one client-side reading
 * of that, shared by the player list's start cell and dropdown and by both
 * map previews, so every surface judges the same start off-side. */

/* A team's START_SIDE_* choice; team 0 (no team) has no side. */
static inline BYTE lobbyTeamSide(ClientSim *cs, int teamId) {
    if (teamId <= 0 || teamId >= MAX_TANKS) return START_SIDE_ANY;
    return clientSimGetLobbyTeamStartSide(cs, (BYTE)teamId);
}

/* Team of a lobby slot; 0 (no team, so no side) when there is none — a
 * spectator passes -1. */
static inline int lobbySlotTeam(ClientSim *cs, int slot) {
    if (slot < 0 || slot >= MAX_TANKS) return 0;
    const ClientLobbySlot *sl = clientSimGetLobbySlot(cs, (BYTE)slot);
    return sl ? sl->teamNumber : 0;
}

/* True when teamId's side rejects a start with this mask — the claim the
 * server refuses when a member of that team picks it for themselves. A
 * team with no side rejects nothing, and a centre start is open to all. */
static inline bool lobbyStartOffSide(ClientSim *cs, int teamId, BYTE mask) {
    return !startSideAccepts(mask, lobbyTeamSide(cs, teamId));
}

/* Lang id of a side's full name — the selector's entries. */
static inline int lobbySideNameId(BYTE side) {
    switch (side) {
        case START_SIDE_N: return STR_DLGLOBBY_SIDE_N;
        case START_SIDE_E: return STR_DLGLOBBY_SIDE_E;
        case START_SIDE_S: return STR_DLGLOBBY_SIDE_S;
        case START_SIDE_W: return STR_DLGLOBBY_SIDE_W;
        default:           return STR_DLGLOBBY_SIDE_ANY;
    }
}

/* Lang id of a side's compass letter, for "Sea · N" and "Team side · N".
 * 0 for a team with no side. */
static inline int lobbySideCompassId(BYTE side) {
    switch (side) {
        case START_SIDE_N: return STR_COMPASS_N;
        case START_SIDE_E: return STR_COMPASS_E;
        case START_SIDE_S: return STR_COMPASS_S;
        case START_SIDE_W: return STR_COMPASS_W;
        default:           return 0;
    }
}

/* Union of the START_SIDE_BIT_* chosen by every team other than teamId
 * that has at least one connected member — the "teams present" rule the
 * server's lobby start pick applies, so a team with a side and no
 * players closes nothing. */
static inline BYTE lobbyClosedMaskForTeam(ClientSim *cs, int teamId) {
    BYTE closedMask = 0;
    for (int k = 0; k < MAX_TANKS; k++) {
        const ClientLobbySlot *sl = clientSimGetLobbySlot(cs, (BYTE)k);
        if (!sl->connected) continue;
        int t = sl->teamNumber;
        if (t == 0 || t >= MAX_TANKS || t == teamId) continue;
        closedMask |= startSideBits(clientSimGetLobbyTeamStartSide(cs, (BYTE)t));
    }
    return closedMask;
}

/* A team's name as the player list's header shows it: the team's own name
 * once the team is in use, the numbered default until then. */
static inline void lobbyTeamLabel(ClientSim *cs, int teamId,
                                  char *buf, size_t bufLen) {
    if (teamId > 0 && teamId < MAX_TANKS &&
        clientSimGetLobbyTeamInUse(cs, (BYTE)teamId)) {
        SDL_strlcpy(buf, clientSimGetLobbyTeamName(cs, (BYTE)teamId), bufLen);
        return;
    }
    MessageArgs args = {};
    args.number = teamId;
    SDL_strlcpy(buf, langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args), bufLen);
}

/* Tooltip for a start the viewer's own team side rejects. A player who is
 * not the host cannot take it, so the text says only that; the host can
 * still assign it and is told which team it is off-side for. */
static inline void lobbyStartOffSideTip(ClientSim *cs, int mySlot, bool host,
                                        int startIdx1, char *buf, size_t bufLen) {
    MessageArgs args = {};
    args.number = startIdx1;
    if (host) {
        lobbyTeamLabel(cs, lobbySlotTeam(cs, mySlot),
                       args.string1, sizeof(args.string1));
        SDL_strlcpy(buf, langGetTextFmt(STR_STARTPICK_TIP_OFFSIDE_HOST, &args), bufLen);
    } else {
        SDL_strlcpy(buf, langGetTextFmt(STR_STARTPICK_TIP_OFFSIDE, &args), bufLen);
    }
}

#endif /* LOBBY_START_MARKERS_H */
