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
 *  drive every case. gamefront.c reads and writes the rows,
 *  under the section names and keys defined here;
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

/* The preferences sections a script's shown flag and pop-out rows live in.
 * The shown flag is a choice and syncs with the player's other machines.
 * The pop-out row holds this machine's desktop coordinates, so prefs.c
 * lists its section as device-local, the way it lists WINDOW. */
#define SCN_PANEL_PREFS_SHOWN_SECTION  "SCENARIO PANEL SHOWN"
#define SCN_PANEL_PREFS_POPOUT_SECTION "SCENARIO PANEL POPOUT"

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
 *NAME:          scnPanelPrefsKey
 *PURPOSE:
 *  The key a script's layout and shown rows are kept under:
 *  the script's name, with any byte below a space turned
 *  into an underscore so the row stays readable in the
 *  file. A longer name is cut to fit keySz.
 *********************************************************/
void scnPanelPrefsKey(const char *script, char *key, size_t keySz);

/*********************************************************
 *NAME:          scnPanelPrefsPopoutKey
 *PURPOSE:
 *  The key one panel's pop-out row is kept under: the
 *  script's key, "#" and the panel id, so a script with a
 *  second panel keeps one row per panel. Pass a buffer 8
 *  bytes larger than the one scnPanelPrefsKey is given: the
 *  name is cut 8 short of keySz, where that key cuts it.
 *********************************************************/
void scnPanelPrefsPopoutKey(const char *script, int panel, char *key,
                            size_t keySz);

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
 *  The caller passes each display's usable bounds (the
 *  display less its taskbar or dock), so a window whose
 *  title bar sits under the taskbar counts as lost.
 *
 *ARGUMENTS:
 *  win           - the window, in desktop coordinates
 *  displays      - each display's usable bounds
 *  n             - how many displays
 *  primaryUsable - the primary display's usable bounds
 *  out           - the moved rectangle, when lost
 *********************************************************/
bool scnPanelRescueRect(const ScnPanelRect *win, const ScnPanelRect *displays,
                        int n, const ScnPanelRect *primaryUsable,
                        ScnPanelRect *out);

/* The two opacities a scenario panel and the Scenario Text window each keep,
 * as percents. -1 in a saved value means the player never set it.
 *
 * Background opacity is the black backing's real alpha: 100 is fully
 * opaque black, 0 is no backing, and the percent maps straight onto the
 * alpha (scnBackingAlpha). The default is 43, which is 110 of 255, the
 * backing these windows had before there was a setting.
 *
 * Text opacity fades what the scenario or mod draws in the window, and not
 * the backing. 100 draws it at full strength, which is the default. */
#define SCN_BG_OPACITY_DEFAULT    43
#define SCN_TEXT_OPACITY_DEFAULT  100

/* The [WINDOW] keys the two opacities are kept under. The old single key of
 * each window is read only to fill the two new ones the first time a player
 * with it loads (scnOpacityFromPrefs), and is not written any more.
 *
 * "Scenario Panel Alpha" is the key main and the 2.1 build write.
 * "Scenario Text Alpha" never was on main: only an earlier build of this
 * change, the one that first drew the Scenario Text window, wrote it. Those
 * builds were played, so real preferences files hold it, and it is read the
 * same way. */
#define SCN_PANEL_PREFS_BG_KEY       "Scenario Panel Background Opacity"
#define SCN_PANEL_PREFS_TEXT_KEY     "Scenario Panel Text Opacity"
#define SCN_PANEL_PREFS_OLD_KEY      "Scenario Panel Alpha"
#define SCN_TEXT_PREFS_BG_KEY        "Scenario Text Background Opacity"
#define SCN_TEXT_PREFS_TEXT_KEY      "Scenario Text Text Opacity"
#define SCN_TEXT_PREFS_OLD_KEY       "Scenario Text Alpha"

/*********************************************************
 *NAME:          scnOpacityClamp
 *PURPOSE:
 *  A saved opacity held to 0..100. -1, never set, passes
 *  through, and any other value below 0 becomes 0.
 *********************************************************/
int scnOpacityClamp(int pct);

/*********************************************************
 *NAME:          scnOpacityMigrate
 *PURPOSE:
 *  The two opacities that give the same look as one old
 *  single opacity. The old value faded the backing (110 of
 *  255 at 100) and the drawing together, so the text gets
 *  the old value and the background gets
 *  round(old * 43 / 100). An old -1, never set, gives -1
 *  for both, which is the defaults.
 *********************************************************/
void scnOpacityMigrate(int oldPct, int *bgPct, int *textPct);

/*********************************************************
 *NAME:          scnOpacityFromPrefs
 *PURPOSE:
 *  The two opacities from the strings the [WINDOW] keys
 *  hold, "" for a key that is not in the file. When both
 *  new keys are missing the old key is migrated with
 *  scnOpacityMigrate; when either new key is there the old
 *  one is not read. A missing or bad value is -1. Both
 *  results are clamped with scnOpacityClamp.
 *********************************************************/
void scnOpacityFromPrefs(const char *bgStr, const char *textStr,
                         const char *oldStr, int *bgPct, int *textPct);

/*********************************************************
 *NAME:          scnBackingAlpha
 *PURPOSE:
 *  The backing's alpha out of 255 for a background
 *  opacity: pct * 255 / 100, rounded. -1 gives the
 *  default's alpha.
 *********************************************************/
int scnBackingAlpha(int bgPct);

/* The section that holds each scenario's background opacity, one value per
 * key, with the same key as the scenario's [SCENARIO PANEL] layout row. It
 * syncs, as the layout row does.
 *
 * The background opacity is not a fifth number on the layout row because
 * [SCENARIO PANEL] syncs to the player's other machines, and an older
 * client there (main, or the 2.1 build) reads the row as "x,y,scale,alpha"
 * with "%d,%d,%d,%d": it takes the first four numbers and ignores the rest.
 * A five-number row "x,y,scale,bg,text" would give it the background
 * percent as its one opacity, so its panel would draw at 43% and not at
 * 100%. The four-number row "x,y,scale,text" gives it the text opacity,
 * which is the old look. */
#define SCN_PANEL_PREFS_BG_SECTION "SCENARIO PANEL BG"

/*********************************************************
 *NAME:          scnPanelLayoutParse
 *PURPOSE:
 *  A scenario's own panel layout, from its row under
 *  "SCENARIO PANEL" and its value under
 *  SCN_PANEL_PREFS_BG_SECTION ("" when that key is not in
 *  the file). Three kinds of row are read:
 *
 *  - "x,y,scale,text" with a background value: the row this
 *    code writes. The background is that value.
 *  - "x,y,scale,alpha" with no background value: the row
 *    main and 2.1 write. Its one opacity is migrated with
 *    scnOpacityMigrate.
 *  - "x,y,scale,bg,text": the row an earlier build of this
 *    change wrote. Both opacities are its own, and the
 *    background value is not read.
 *
 *  A background value that is not a whole number counts as
 *  missing. Any other row returns false and leaves the
 *  outputs alone. The opacities are clamped with
 *  scnOpacityClamp; the scale and the position pass through
 *  for the caller to check.
 *
 *  Note: an older client on another machine writes a
 *  four-number row with its one opacity and leaves the
 *  background value as it was. That row is then read as the
 *  text opacity with the older background value.
 *********************************************************/
bool scnPanelLayoutParse(const char *row, const char *bgRow, int *x, int *y,
                         int *scale, int *bgPct, int *textPct);

/*********************************************************
 *NAME:          scnPanelLayoutFormat / scnPanelLayoutBgFormat
 *PURPOSE:
 *  Write the two rows scnPanelLayoutParse reads: the
 *  layout row "x,y,scale,text", and the background value
 *  under SCN_PANEL_PREFS_BG_SECTION. Both are written each
 *  time, -1 included, so the parse can tell this row from
 *  an old one.
 *********************************************************/
void scnPanelLayoutFormat(int x, int y, int scale, int textPct, char *buf,
                          size_t bufSz);
void scnPanelLayoutBgFormat(int bgPct, char *buf, size_t bufSz);

#ifdef __cplusplus
}
#endif

#endif /* SCN_PANEL_PREFS_H */
