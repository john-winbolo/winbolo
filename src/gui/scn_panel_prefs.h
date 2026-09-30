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
 *Name:          Scenario Panel Preferences
 *Filename:      scn_panel_prefs.h
 *Purpose:
 *  The pure parts of the scenario panel's saved state: the
 *  popped-out row a script's panel is remembered by, the
 *  yes/no words its other two settings are written as, and
 *  the rule that brings a pop-out window back onto a display
 *  when the display it was left on has gone.
 *
 *  No SDL and no preferences file here, so the unit tests can
 *  drive every case. gamefront.c reads and writes the rows;
 *  sdl3imgui.cpp asks SDL for the displays and hands them in.
 *********************************************************/
#ifndef SCN_PANEL_PREFS_H
#define SCN_PANEL_PREFS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A rectangle in desktop coordinates, the units SDL gives display bounds and
 * window positions in. */
typedef struct {
    int x, y, w, h;
} ScnPanelRect;

/* A panel's pop-out as it was left: whether it was popped out, and where the
 * OS window sat. x and y are -1 when no position was ever saved, and the
 * window then lands where the OS puts it. */
typedef struct {
    bool open;
    int  x, y, w, h;
} ScnPanelPopout;

/* The smallest pop-out side the row may ask for, and the largest. Below the
 * floor the window has no room for the gear and the close button; above the
 * ceiling a hand-edited row would open a window larger than any display. */
#define SCN_PANEL_POPOUT_MIN_PX  96
#define SCN_PANEL_POPOUT_MAX_PX  4096
/* The side a pop-out opens at when no size was ever saved. */
#define SCN_PANEL_POPOUT_DEFAULT_PX 256

/* How much of a window has to be on some display for it to count as found:
 * this many pixels of the strip this tall along the top of the window, where
 * the title bar is. Less than that and the player has nothing to grab. */
#define SCN_PANEL_RESCUE_MIN_PX  48

/*********************************************************
 *NAME:          scnPanelPopoutParse
 *PURPOSE:
 *  Reads a pop-out row, "open,x,y,w,h" with open 1 or 0.
 *  Returns false, leaving out alone, for a row that does
 *  not read as five whole numbers. A size is clamped into
 *  SCN_PANEL_POPOUT_MIN_PX..SCN_PANEL_POPOUT_MAX_PX. The
 *  position is passed through: only SDL knows the displays,
 *  and scnPanelRescueRect is what checks it.
 *********************************************************/
bool scnPanelPopoutParse(const char *row, ScnPanelPopout *out);

/*********************************************************
 *NAME:          scnPanelPopoutFormat
 *PURPOSE:
 *  Writes the row scnPanelPopoutParse reads.
 *********************************************************/
void scnPanelPopoutFormat(const ScnPanelPopout *in, char *buf, size_t bufSz);

/*********************************************************
 *NAME:          scnPanelYesNoParse / scnPanelYesNoWord
 *PURPOSE:
 *  The words the panel's yes/no settings are written as,
 *  "Yes" and "No", the words the rest of the preferences
 *  file uses. Parse takes any word starting with Y/y/1 as
 *  yes and N/n/0 as no, and anything else, empty included,
 *  as fallback.
 *********************************************************/
bool        scnPanelYesNoParse(const char *word, bool fallback);
const char *scnPanelYesNoWord(bool yes);

/*********************************************************
 *NAME:          scnPanelRescueRect
 *PURPOSE:
 *  The lost-window rule. A window is found when the strip
 *  along its top, SCN_PANEL_RESCUE_MIN_PX tall, overlaps one
 *  display by at least SCN_PANEL_RESCUE_MIN_PX across and at
 *  least half the strip down (less when the window itself is
 *  smaller). Half, so a window pushed a little above the top
 *  of a display does not jump back to the primary one.
 *  A found window is left where it is and false comes back.
 *
 *  A lost one is moved onto the primary display's usable
 *  area: its size is kept but cut down to fit that area,
 *  and it is centred there. out gets the new rectangle and
 *  true comes back.
 *
 *  With no displays at all (n == 0) nothing can be checked
 *  and the window is left alone.
 *
 *ARGUMENTS:
 *  win           - the window, in desktop coordinates
 *  displays      - each display's full bounds
 *  n             - how many displays
 *  primaryUsable - the primary display's usable bounds
 *  out           - the moved rectangle, when lost
 *********************************************************/
bool scnPanelRescueRect(const ScnPanelRect *win, const ScnPanelRect *displays,
                        int n, const ScnPanelRect *primaryUsable,
                        ScnPanelRect *out);

#ifdef __cplusplus
}
#endif

#endif /* SCN_PANEL_PREFS_H */
