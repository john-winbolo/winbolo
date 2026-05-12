/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_nav_outline.h
 * Purpose:       Replacement for ImGui's default nav cursor.
 *                Theme zeroes ImGuiCol_NavCursor so the
 *                stock 2px-thick stroked rect (which renders
 *                with irregular thickness + jagged corners
 *                at desktop DPI) doesn't draw.  Each ImGui
 *                context calls dialogDrawNavOutline() once
 *                per frame before ImGui::Render(); it walks
 *                the focused item bb via internals and draws
 *                a clean 3px outline on the nav window's
 *                draw list.
 *********************************************************/

#ifndef IMGUI_NAV_OUTLINE_H
#define IMGUI_NAV_OUTLINE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Draw a clean nav-cursor outline for the currently-focused item.
 * Safe to call in any context — does nothing if there is no NavId,
 * no NavWindow, or the cursor is not visible.  Intended to be called
 * once per frame, just before ImGui::Render(). */
void dialogDrawNavOutline(void);

/* True when ImGui's nav focus is inside a sub-region (child window
 * or BeginTable scroll child) of its top-level window.  Used by
 * the gamepad B-cancel handler so B pops out of the sub-region via
 * ImGui's NavCancel instead of closing the dialog. */
bool dialogNavIsInsideSubRegion(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_NAV_OUTLINE_H */
