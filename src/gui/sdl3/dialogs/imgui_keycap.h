/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_keycap.h
 * Purpose:       Procedural keycap drawer shared between
 *                the rich message box (tutorial dialogs)
 *                and the Configure Keys dialog.  Used as a
 *                fallback when no PNG glyph exists for a
 *                given keyboard scancode.
 *********************************************************/

#ifndef IMGUI_KEYCAP_H
#define IMGUI_KEYCAP_H

#include "imgui.h"

/* Draws a rounded-rect key cap of side `h` at screen position `pos`
 * with the label centred inside.  Border colour follows ImGuiCol_Text
 * so it adapts to light/dark themes.  Pure draw-list operation: does
 * not advance any layout cursor. */
void drawProceduralKeycapAt(ImVec2 pos, float h, const char *label);

#endif /* IMGUI_KEYCAP_H */
