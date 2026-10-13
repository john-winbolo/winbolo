/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Log Viewer Overview panel sums
 *Filename:      lv_overview.h
 *Purpose:
 *  The table the Log Viewer's Overview panel shows
 *  (imgui/imgui_overview.cpp): one row for each team in the
 *  game with the pillboxes and the bases it owns, a row for
 *  the ones nobody owns, and the totals.
 *
 *  The table is built from what the pillboxes and bases are
 *  at the playback time (lv_screenOverviewSnapshot), never
 *  from a count of capture events, so it is right after a
 *  seek in either direction.
 *
 *  Header only, with no includes but the C library's, so the
 *  viewer (C and C++) and the unit tests use the same sums.
 *********************************************************/

#ifndef LV_OVERVIEW_H
#define LV_OVERVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The most teams there can be: one for each player slot. */
#define LV_OVERVIEW_MAX_TEAMS 16

/* The longest list of a team's player names the table carries, with its
 * NUL. A longer list is cut. */
#define LV_OVERVIEW_NAMES_LEN 192

/* What separates two names in the list. */
#define LV_OVERVIEW_NAME_SEP ", "

/* One team. */
typedef struct LvOverviewRow {
    unsigned char team;     /* the team's number (an alliance group) */
    unsigned char colour;   /* the palette entry the team is drawn in */
    unsigned char players;  /* how many players are in it */
    int pills;              /* pillboxes it owns, carried ones too */
    int bases;              /* bases it owns */
    char firstName[LV_OVERVIEW_NAMES_LEN]; /* its first player, by slot */
    char names[LV_OVERVIEW_NAMES_LEN];     /* all its players, "A, B, C" */
    /* Where each player's name starts in names, and the chip the player has
     * (LvBotChip of lv_bot_chip.h; 0 for a person), so the panel can put the
     * chip in front of a bot's name. For the first LV_OVERVIEW_MAX_TEAMS
     * players of the team, which is every player a game can have. */
    unsigned char nameAt[LV_OVERVIEW_MAX_TEAMS];
    unsigned char chip[LV_OVERVIEW_MAX_TEAMS];
    unsigned char bots;     /* how many of them have a chip */
} LvOverviewRow;

/* The whole table. The rows are in team order. */
typedef struct LvOverview {
    int rowCount;
    LvOverviewRow rows[LV_OVERVIEW_MAX_TEAMS];
    int neutralPills;
    int neutralBases;
    int totalPills;
    int totalBases;
} LvOverview;

static inline void lvOverviewInit(LvOverview *o) {
    memset(o, 0, sizeof(*o));
}

/* The row of a team, NULL when the team has none. */
static inline LvOverviewRow *lvOverviewFind(LvOverview *o, int team) {
    for (int i = 0; i < o->rowCount; i++) {
        if (o->rows[i].team == team) return &o->rows[i];
    }
    return NULL;
}

/* Append src to a names list, cut to fit. */
static inline void lvOverviewAppend(char *dst, const char *src) {
    size_t used = strlen(dst);
    size_t room = LV_OVERVIEW_NAMES_LEN - 1 - used;
    size_t n = strlen(src);
    if (n > room) n = room;
    memcpy(dst + used, src, n);
    dst[used + n] = '\0';
}

/* A player is in the game: put the player in the team's row, and make the
 * row if this is the team's first player. A new row goes in where it keeps
 * the rows in team order, whatever order the players come in. chip is the
 * player's LvBotChip, 0 for a person. Returns false when the team number is
 * not one a player can have. */
static inline bool lvOverviewAddPlayerChip(LvOverview *o, int team, int colour,
                                           const char *name, int chip) {
    LvOverviewRow *row;
    if (team < 0 || team >= LV_OVERVIEW_MAX_TEAMS) return false;
    if (name == NULL) name = "";

    row = lvOverviewFind(o, team);
    if (row == NULL) {
        int at = 0;
        if (o->rowCount >= LV_OVERVIEW_MAX_TEAMS) return false;
        while (at < o->rowCount && o->rows[at].team < team) at++;
        memmove(&o->rows[at + 1], &o->rows[at],
                (size_t)(o->rowCount - at) * sizeof(o->rows[0]));
        o->rowCount++;
        row = &o->rows[at];
        memset(row, 0, sizeof(*row));
        row->team = (unsigned char)team;
        row->colour = (unsigned char)colour;
        lvOverviewAppend(row->firstName, name);
    } else {
        lvOverviewAppend(row->names, LV_OVERVIEW_NAME_SEP);
    }
    if (row->players < LV_OVERVIEW_MAX_TEAMS) {
        row->nameAt[row->players] = (unsigned char)strlen(row->names);
        row->chip[row->players] = (unsigned char)chip;
        if (chip != 0) row->bots++;
    }
    lvOverviewAppend(row->names, name);
    row->players++;
    return true;
}

/* The same for a person: no chip. */
static inline bool lvOverviewAddPlayer(LvOverview *o, int team, int colour,
                                       const char *name) {
    return lvOverviewAddPlayerChip(o, team, colour, name, 0);
}

/* The name of the row's player number index (0 is the first), as a part of
 * row->names: where it starts, and in *len how long it is. A name the list
 * had no room for has the length 0. NULL when the row has no such player. */
static inline const char *lvOverviewName(const LvOverviewRow *row, int index, size_t *len) {
    size_t start, end;
    if (index < 0 || index >= row->players || index >= LV_OVERVIEW_MAX_TEAMS) {
        *len = 0;
        return NULL;
    }
    start = row->nameAt[index];
    end = strlen(row->names);
    if (index + 1 < row->players && index + 1 < LV_OVERVIEW_MAX_TEAMS) {
        /* The next name starts after the separator. */
        size_t next = row->nameAt[index + 1];
        size_t sep = strlen(LV_OVERVIEW_NAME_SEP);
        if (next < end) end = (next >= sep) ? next - sep : 0;
    }
    if (end < start) end = start;
    *len = end - start;
    return row->names + start;
}

/* Count one pillbox (isBase false) or base that is on the map or in a tank.
 * It goes to the row of the owner's team; one nobody owns, or whose owner is
 * in no team that has a row (the owner left the game), is neutral. Call this
 * after every player is in. */
static inline void lvOverviewCount(LvOverview *o, bool isBase, bool hasOwner,
                                   int team) {
    LvOverviewRow *row = hasOwner ? lvOverviewFind(o, team) : NULL;
    if (isBase) {
        o->totalBases++;
        if (row != NULL) row->bases++; else o->neutralBases++;
    } else {
        o->totalPills++;
        if (row != NULL) row->pills++; else o->neutralPills++;
    }
}

/* The part of the whole a count is, 0..1. Nothing of nothing is 0. */
static inline float lvOverviewShare(int count, int total) {
    if (total <= 0 || count <= 0) return 0.0f;
    if (count >= total) return 1.0f;
    return (float)count / (float)total;
}

#endif /* LV_OVERVIEW_H */
