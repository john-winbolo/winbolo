/*
 * imgui_lv_panel.h - what every panel of the Log Viewer has: its options
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * A panel opens its window with lv_panel_begin and closes it with
 * lv_panel_end, where it would call ImGui::Begin and ImGui::End. For that it
 * gets:
 *
 *  - a small gear button. It is in the title bar, to the left of the close
 *    button, so it takes no room from what the panel shows. A panel that is
 *    a tab in a sidebar has no title bar of its own: its gear is in the same
 *    place in the bar of tabs, and belongs to the tab that shows. Where
 *    there is no bar at all (the bar of tabs can be hidden) the gear is in
 *    the bottom right corner of the panel. A click opens the panel's
 *    options.
 *
 *  - the option "Make unused panel portions transparent". With it on, the
 *    panel's background is not drawn: not the window's, not that of a list
 *    or a table inside it. What the panel shows stays, and behind each
 *    block of it is a backing in the colour of the background, a little
 *    see-through, so text can be read over the map. The title bar stays.
 *    The panel's border shows only while the mouse is over the panel or the
 *    panel is being moved or resized.
 *
 * The option is one a panel, kept in the LOGVIEWER preferences as
 * "Window.<Panel>.Transparent" (lv_panel_opts.h has the names).
 */

#ifndef LV_IMGUI_LV_PANEL_H
#define LV_IMGUI_LV_PANEL_H

#include "imgui.h"

/* ImGui::Begin for a panel. id is the part of the title that makes the
 * window's ID ("###tank"). Returns what Begin returns; lv_panel_end has to
 * follow whatever that is. Panels do not nest. */
bool lv_panel_begin(const char *id, const char *title, bool *p_open, ImGuiWindowFlags flags);

/* ImGui::End for a panel opened with lv_panel_begin. */
void lv_panel_end(void);

/* Whether the panel's unused parts are transparent, and set it. A window
 * that is not one of the panels has no option: false, and nothing is set. */
bool lv_panel_get_transparent(const char *id);
void lv_panel_set_transparent(const char *id, bool on);

#ifdef __cplusplus
extern "C" {
#endif

/* Write every panel's options to the preferences (not to disk: the caller
 * saves the preferences). */
void lv_panel_options_save(void);

/* A number that changes when an option of a panel changes, for the viewer's
 * "save the layout once it has been still" check. */
unsigned lv_panel_options_signature(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_IMGUI_LV_PANEL_H */
