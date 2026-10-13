/*
 * imgui_lv_icons.h - the Log Viewer's drawn icons
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * Every icon the viewer draws for itself is here: the buttons of the
 * Controls panel, the eye of the HUD panel, the gear of a panel's options
 * and the event markers of the seek slider. None of them ships with the game as a picture, and a font for a
 * few glyphs is a lot to load, so they are drawn with the draw list: filled
 * shapes in one colour, sized from the box they are given, so they follow
 * the size of the font and the density of the display.
 */

#ifndef LV_IMGUI_LV_ICONS_H
#define LV_IMGUI_LV_ICONS_H

#include "imgui.h"

enum LvIcon {
    LV_ICON_PLAY = 0,       /* a triangle to the right */
    LV_ICON_PAUSE,          /* two upright bars */
    LV_ICON_REWIND,         /* two triangles to the left */
    LV_ICON_FAST_FORWARD,   /* two triangles to the right */
    LV_ICON_TO_END,         /* a triangle to the right against a bar */
    LV_ICON_EYE,            /* the outline of an eye and its pupil */
    LV_ICON_GEAR,           /* a toothed wheel */
    LV_ICON_CLOSE,          /* an X */
    LV_ICON_SQUARE,         /* a filled square: a pillbox, as in the HUD panel */
    LV_ICON_DISC,           /* a filled circle: a base, as in the HUD panel */
    LV_ICON_PERSON,         /* a head and shoulders: a builder */
    LV_ICON_RING,           /* the outline of a circle: a player comes or goes */
    LV_ICON_BUBBLE,         /* a speech bubble: chat */
    LV_ICON_DIAMOND,        /* a square on its corner: the game itself */
    LV_ICON_STAR,           /* a star of five points: a highlight */
    LV_ICON_COUNT
};

/* The icon inside the box p0..p1, in one colour. The box is the icon's own:
 * a caller that wants a margin round it hands in a smaller box. */
void lv_icon_draw(ImDrawList *dl, int icon, ImVec2 p0, ImVec2 p1, ImU32 col);

/* A button with the icon in its middle, in the colour of the text. The
 * hovered, held and disabled looks are the ones every ImGui button has
 * (inside BeginDisabled it is dimmed and does not answer). id is the
 * button's ID for the keyboard and the gamepad; it is not shown. side is the
 * height, 0 for the height of a framed widget, so it lines up with a slider
 * or a combo on the same row. width is 0 for a square, or the width of a
 * wider button: the icon keeps the size side gives it and stays in the
 * middle, and the whole button takes the press. Returns true when pressed. */
bool lv_icon_button(const char *id, int icon, float side = 0.0f, float width = 0.0f);

/* The length lv_icon_button's square has for side = 0. */
float lv_icon_button_side(void);

/* The icon as an item in a line of text: as tall as the text, in its
 * colour. It takes its room in the window, so SameLine follows it. */
void lv_icon_text(int icon);

/* The width lv_icon_text takes for this icon. */
float lv_icon_text_width(int icon);

/* The map's own picture of a pillbox or a base over the box p0..p1: the one
 * helper every panel uses for it. team is the number of the team that holds
 * the thing, for the picture in that team's colour as the map has it; -1 is
 * a thing no team holds: the game's red pillbox, the game's neutral base.
 * alpha dims it. The picture is drawn sharp at a whole number of times its
 * 16 pixels and blended at any other size (lvItemPictureSharp). Returns
 * false, and draws nothing, when the game's sheets are not there; the
 * caller then draws its own plain shape. */
bool lv_item_picture(ImDrawList *dl, bool isBase, int team, ImVec2 p0, ImVec2 p1,
                     float alpha = 1.0f);

/* The line that names a player at the top of a panel (the Tank panel, the
 * HUD panel): a stripe in the colour of the player's team, an icon when
 * leadIcon is one (-1 for none), the name, and at the right end a small X
 * button. A name too long for the line is cut and ends in "..."; the button
 * is never cut and never under the name (lv_name_header.h has the sums).
 * The stripe keeps its room when the player is in no team, so the name
 * starts at the same place. nameTip is the tooltip of the name (NULL: the
 * whole name, when it was cut); closeTip that of the button. The line takes
 * the height of a line of text with its spacing. Returns true when the
 * button was pressed. */
bool lv_name_header(const char *id, bool hasStripe, ImU32 stripe, int leadIcon,
                    const char *name, const char *nameTip, const char *closeTip);

/* The width of the stripe of lv_name_header. */
#define LV_NAME_HEADER_STRIPE_W 4.0f

#endif /* LV_IMGUI_LV_ICONS_H */
