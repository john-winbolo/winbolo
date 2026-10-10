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

/* The width drawProceduralKeycapWideAt gives a cap `h` high for `label`
 * at the current font: wide enough for the whole label, never narrower
 * than h. */
float proceduralKeycapWidth(float h, const char *label);

/* A key cap `h` high and proceduralKeycapWidth(h, label) wide, with the
 * whole label inside. For text that names the key only by its cap (the
 * tutorial's rich message box): "Num ." must not be drawn as "N". */
void drawProceduralKeycapWideAt(ImVec2 pos, float h, const char *label);

#endif /* IMGUI_KEYCAP_H */
