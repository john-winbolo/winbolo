/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * lv_team_colour.h - which colour the log viewer draws a team in.
 *
 * The Team Colours dialog (Options > Team Colours) gives each of the sixteen
 * teams, and the things no team holds, one entry of a palette of seventeen
 * colours. The choice is kept in LogViewerState.tc: tc[0] to tc[15] for the
 * teams and tc[16] for neutral.
 *
 * Everything that draws a team in its colour asks for the team's entry here
 * (lv_screenTeamColour in screen.c is the one call that reads tc with it):
 * the tanks, boats, pillboxes and bases of the map, the marks of the
 * simplified view, and the HUD, Overview, Tank and Events panels. A team is
 * drawn in the entry the dialog has for it, with no exception: the row of
 * that entry in tanks.bmp, boats.bmp and items.bmp (lv_team_sprite.h) and
 * the flat colour of that entry (mapColourTeam).
 *
 * Header only, with no includes, so the viewer and the unit tests
 * (tests/unit/test_lv_hud.c) use the same sums.
 */

#ifndef LV_TEAM_COLOUR_H
#define LV_TEAM_COLOUR_H

/* The teams, the place of neutral in tc[], and the size of tc[]. */
#define LV_TEAM_COLOUR_TEAMS   16
#define LV_TEAM_COLOUR_NEUTRAL 16
#define LV_TEAM_COLOUR_SLOTS   17

/* The entries of the palette: 0 to 16. */
#define LV_TEAM_COLOUR_ENTRIES 17

/* The place in tc[] of a team. A number that is not a team (under 0, or 16
 * and over) is neutral. */
static inline int lvTeamColourSlot(int team) {
    if (team < 0 || team >= LV_TEAM_COLOUR_TEAMS) return LV_TEAM_COLOUR_NEUTRAL;
    return team;
}

/* The entry a team has before the user picks one: Team 1 green (2), Team 2
 * red (11), Team 3 blue (12), Team 4 yellow (4), and so on. Neutral is light
 * grey (10). These are the values the Team Colours dialog opens with. */
static inline unsigned char lvTeamColourDefault(int team) {
    static const unsigned char defaults[LV_TEAM_COLOUR_SLOTS] = {
        2, 11, 12, 4, 16, 13, 8, 3, 6, 1, 5, 7, 9, 14, 15, 0, 10
    };
    return defaults[lvTeamColourSlot(team)];
}

/* A value read from the preferences for a team: the value when it is an
 * entry of the palette, and the team's default when it is not. */
static inline unsigned char lvTeamColourPref(int team, int value) {
    if (value < 0 || value >= LV_TEAM_COLOUR_ENTRIES) return lvTeamColourDefault(team);
    return (unsigned char)value;
}

/* The entry a team is drawn in: the one tc[] has for it. tc is the
 * seventeen values of LogViewerState.tc. With no tc the answer is entry 0. */
static inline unsigned char lvTeamColourEntry(const unsigned char *tc, int team) {
    if (tc == 0) return 0;
    return lvTeamColourPref(team, tc[lvTeamColourSlot(team)]);
}

#endif /* LV_TEAM_COLOUR_H */
