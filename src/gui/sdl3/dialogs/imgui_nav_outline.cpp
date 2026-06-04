/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_nav_outline.cpp
 * Purpose:       Replacement for ImGui's default nav cursor.
 *                See imgui_nav_outline.h for rationale.
 *********************************************************/

#include <math.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_nav_outline.h"

/* Promoted once per frame from dialogDrawNavOutline (called near end of
   frame, after the widgets' CancelKeyPressed checks).  So a read during
   the next frame's rendering returns the prior frame's end state, which
   is this frame's pre-NavUpdate (pre-pop) value. */
static bool s_navInsideSubRegionPrevFrame = false;
static bool s_navInsideSubRegionThisFrame = false;

extern "C" bool dialogNavWasInsideSubRegionAtFrameStart(void) {
    return s_navInsideSubRegionPrevFrame;
}

extern "C" void dialogDrawNavOutline(void) {
    ImGuiContext *g = ImGui::GetCurrentContext();
    if (!g) return;

    /* Promote the sub-region snapshot before any early-out below so the
       pre-pop value stays current every frame regardless of nav state. */
    s_navInsideSubRegionPrevFrame = s_navInsideSubRegionThisFrame;
    s_navInsideSubRegionThisFrame = dialogNavIsInsideSubRegion();

    if (g->NavId == 0) return;
    if (!g->NavCursorVisible) return;
    ImGuiWindow *win = g->NavWindow;
    if (!win) return;

    int layer = (int)g->NavLayer;
    if (layer < 0 || layer >= ImGuiNavLayer_COUNT) return;

    /* NavRectRel is in window-local coords with origin at
       DC.CursorStartPos (post-titlebar + padding), NOT at window->Pos.
       Use ImGui's WindowRectRelToAbs to convert correctly — using Pos
       directly produces a 5-10 px up-and-left offset that scales with
       title bar height + WindowPadding. */
    const ImRect &rel = win->NavRectRel[layer];
    if (rel.Min.x >= rel.Max.x || rel.Min.y >= rel.Max.y) return;

    ImRect abs = ImGui::WindowRectRelToAbs(win, rel);
    ImVec2 a = abs.Min;
    ImVec2 b = abs.Max;

    /* Inflate so the outline sits clearly outside the item frame. */
    a.x -= 2.0f; a.y -= 2.0f;
    b.x += 2.0f; b.y += 2.0f;

    /* Pixel-snap.  ImGui's default cursor renders without snapping,
       which is the root cause of the "thin-in-middle, thick-at-corners"
       artifact: stroke positions land between pixels and AA rasterises
       them inconsistently.  Snapping eliminates that. */
    a.x = floorf(a.x); a.y = floorf(a.y);
    b.x = floorf(b.x); b.y = floorf(b.y);

    ImDrawList *dl = win->DrawList;
    /* Theme's NavHighlight colour (light blue 0.40, 0.72, 0.88, 1.0)
       baked in — the theme zeroes ImGuiCol_NavCursor at startup so we
       can't read it back through the style. */
    const ImU32 col = IM_COL32(102, 184, 224, 255);
    /* Match the button frame rounding so the outline curves with the
       button corners.  3px stroke at integer positions reads cleanly. */
    const float rounding = ImGui::GetStyle().FrameRounding;
    dl->AddRect(a, b, col, rounding, ImDrawFlags_None, 3.0f);
}

extern "C" bool dialogNavIsInsideSubRegion(void) {
    ImGuiContext *g = ImGui::GetCurrentContext();
    if (!g || !g->NavWindow) return false;
    /* True for ImGui child windows (BeginChild) and table scroll
       regions (BeginTable with ScrollX/Y), both of which create a
       child window whose RootWindow points up to the user's outer
       Begin().  When nav is inside one of these, B should pop out
       via ImGui's NavCancel rather than close the dialog. */
    return g->NavWindow->RootWindow != g->NavWindow;
}
