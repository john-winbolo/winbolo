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
 * red (11), Team 3 blue (12), Team 4 yellow (4), and so on. Neutral is the
 * dark grey (0, 0x6f6f6f on items.bmp), the colour neutral pillboxes are
 * drawn in. These are the values the Team Colours dialog opens with. */
static inline unsigned char lvTeamColourDefault(int team) {
    static const unsigned char defaults[LV_TEAM_COLOUR_SLOTS] = {
        2, 11, 12, 4, 16, 13, 8, 3, 6, 1, 5, 7, 9, 14, 15, 0, 0
    };
    return defaults[lvTeamColourSlot(team)];
}

/* A value read from the preferences for a team: the value when it is an
 * entry of the palette, and the team's default when it is not. */
static inline unsigned char lvTeamColourPref(int team, int value) {
    if (value < 0 || value >= LV_TEAM_COLOUR_ENTRIES) return lvTeamColourDefault(team);
    return (unsigned char)value;
}

/* The Neutral entry before it was the dark grey: light grey (10). The
 * viewer writes every colour to the preferences each time it closes, so a
 * stored 10 is almost always this old default and not the user's pick.
 * The key LV_TEAM_COLOUR_NEUTRAL_MARK_KEY is written beside "Neutral
 * Colour" from now on. A stored Neutral value with no such mark comes from
 * the time of the old default: a 10 there reads as the new default. Once
 * the mark is written, every stored value is the user's, 10 too. */
#define LV_TEAM_COLOUR_NEUTRAL_OLD_DEFAULT 10
#define LV_TEAM_COLOUR_NEUTRAL_KEY         "Neutral Colour"
#define LV_TEAM_COLOUR_NEUTRAL_MARK_KEY    "Neutral Colour Version"
#define LV_TEAM_COLOUR_NEUTRAL_MARK        "2"

/* A value read from the preferences for Neutral. marked is nonzero when
 * LV_TEAM_COLOUR_NEUTRAL_MARK_KEY holds LV_TEAM_COLOUR_NEUTRAL_MARK. */
static inline unsigned char lvTeamColourNeutralPref(int value, int marked) {
    if (!marked && value == LV_TEAM_COLOUR_NEUTRAL_OLD_DEFAULT) {
        return lvTeamColourDefault(LV_TEAM_COLOUR_NEUTRAL);
    }
    return lvTeamColourPref(LV_TEAM_COLOUR_NEUTRAL, value);
}

/* The entry a team is drawn in: the one tc[] has for it. tc is the
 * seventeen values of LogViewerState.tc. With no tc the answer is entry 0. */
static inline unsigned char lvTeamColourEntry(const unsigned char *tc, int team) {
    if (tc == 0) return 0;
    return lvTeamColourPref(team, tc[lvTeamColourSlot(team)]);
}

#endif /* LV_TEAM_COLOUR_H */
