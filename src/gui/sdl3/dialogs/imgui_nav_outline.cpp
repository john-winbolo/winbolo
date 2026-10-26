/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#if defined(__IPHONEOS__) || defined(__ANDROID__)
#include "imgui_touch.h"
#endif

/* Promoted once per frame from dialogDrawNavOutline (called near end of
   frame, after the widgets' CancelKeyPressed checks).  So a read during
   the next frame's rendering returns the prior frame's end state, which
   is this frame's pre-NavUpdate (pre-pop) value. */
static bool s_navInsideSubRegionPrevFrame = false;
static bool s_navInsideSubRegionThisFrame = false;

extern "C" bool dialogNavWasInsideSubRegionAtFrameStart(void) {
    return s_navInsideSubRegionPrevFrame;
}

/* When the outline may show.  Every context sets
   ConfigNavCursorVisibleAlways, so ImGui keeps NavCursorVisible on from
   the first frame (a window that opens, a default-focus item, a mouse
   click on a button or a combo all give it a NavId).  That flag also
   lets Enter / Space press the focused item, so it stays on; only the
   drawing waits.  A Tab press shows the outline; a mouse move or click
   hides it again.  A controller keeps it on: any pad button press here,
   or the game's own record that the controller is the device in use
   (Steam Input reaches ImGui as arrow keys, not pad keys). */
static bool s_navOutlineByKey     = false;
static bool s_navOutlineByPadUser = false;

extern "C" void dialogNavOutlineSetControllerInUse(bool inUse) {
    s_navOutlineByPadUser = inUse;
}

static void navOutlineTrackInput(ImGuiContext *g) {
    const ImGuiIO &io = g->IO;
    bool mouseUse = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    for (int b = 0; b < IM_ARRAYSIZE(io.MouseClicked) && !mouseUse; b++)
        if (io.MouseClicked[b]) mouseUse = true;
    if (mouseUse) s_navOutlineByKey = false;

    /* Tab counts only while a window takes nav input, so a Tab bound to
       a game key during play does not switch the outline on. */
    if (g->NavWindow && ImGui::IsKeyPressed(ImGuiKey_Tab, false))
        s_navOutlineByKey = true;
    for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadR3; k++)
        if (ImGui::IsKeyPressed((ImGuiKey)k, false)) s_navOutlineByKey = true;
}

extern "C" void dialogDrawNavOutline(void) {
    ImGuiContext *g = ImGui::GetCurrentContext();
    if (!g) return;

#if defined(__IPHONEOS__) || defined(__ANDROID__)
    /* Every ImGui frame on iOS passes through here just before Render, so
     * the touch handling rides along (see imgui_touch.h). */
    imguiTouchFrame();
#endif

    /* Promote the sub-region snapshot before any early-out below so the
       pre-pop value stays current every frame regardless of nav state. */
    s_navInsideSubRegionPrevFrame = s_navInsideSubRegionThisFrame;
    s_navInsideSubRegionThisFrame = dialogNavIsInsideSubRegion();

    navOutlineTrackInput(g);
    if (!s_navOutlineByKey && !s_navOutlineByPadUser) return;

    if (g->NavId == 0) return;
    if (!g->NavCursorVisible) return;
    /* Only draw if the focused item was actually submitted this frame.
       ImGui's built-in highlight is rendered from inside each widget, so it
       vanishes automatically when the focused item stops being submitted
       (e.g. a bot row whose remove-X was just clicked). This outline instead
       draws once per frame from the retained NavRectRel, which would linger
       at the removed item's old location. NavIdIsAlive is reset each frame in
       NavUpdate and set true by ItemAdd when the NavId item is submitted, so
       this gate mirrors the built-in behaviour. */
    if (!g->NavIdIsAlive) return;
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

    /* Keep the outline inside the item's own window.  This draws after
       the window has ended, when the draw list no longer clips to it, and
       an item can be wider than its window: a Selectable takes the width
       of its label when that is wider than the list, so a long row's
       outline ran out of a list child and over the panel beside it.
       ImGui's own cursor clips to the window the same way.  The main
       layer keeps to the window's inner clip area, plus the 2px the
       outline sits outside an item; the menu bar layer lies outside that
       area, so it keeps to the window's outer rect. */
    ImRect limit = win->OuterRectClipped;
    if (layer == ImGuiNavLayer_Main) {
        ImRect inner = win->InnerClipRect;
        inner.Expand(2.0f);
        limit.ClipWithFull(inner);
    }
    if (a.x < limit.Min.x) a.x = limit.Min.x;
    if (a.y < limit.Min.y) a.y = limit.Min.y;
    if (b.x > limit.Max.x) b.x = limit.Max.x;
    if (b.y > limit.Max.y) b.y = limit.Max.y;
    if (a.x >= b.x || a.y >= b.y) return;

    /* Pixel-snap.  ImGui's default cursor renders without snapping,
       which is the root cause of the "thin-in-middle, thick-at-corners"
       artifact: stroke positions land between pixels and AA rasterises
       them inconsistently.  Snapping eliminates that.

       The stroke is centred on the path and 3px (odd) wide, so its fill
       spans path-1.5 .. path+1.5.  Land the path on a pixel centre
       (integer + 0.5) so those edges fall on whole-pixel boundaries
       (int-1 .. int+2); snapping to a plain integer would straddle
       pixels on both edges and stay soft. */
    a.x = floorf(a.x) + 0.5f; a.y = floorf(a.y) + 0.5f;
    b.x = floorf(b.x) + 0.5f; b.y = floorf(b.y) + 0.5f;

    ImDrawList *dl = win->DrawList;
    /* Theme's NavHighlight colour (light blue 0.40, 0.72, 0.88, 1.0)
       baked in — the theme zeroes ImGuiCol_NavCursor at startup so we
       can't read it back through the style. */
    const ImU32 col = IM_COL32(102, 184, 224, 255);
    /* Match the button frame rounding so the outline curves with the
       button corners.  3px stroke at integer positions reads cleanly. */
    const float rounding = ImGui::GetStyle().FrameRounding;
    /* Force the geometry-based AA path for this stroke.  With the default
       textured AA, a 3px line is drawn by bilinear-sampling a baked line
       bitmap from the atlas; at 1:1 (non-Retina) that maps ~one texel per
       pixel and smears the stroke into uneven thickness.  Geometry AA
       builds the stroke from triangles at the already-snapped coords, so
       it stays crisp at 1:1 and unchanged on Retina. */
    ImDrawListFlags savedFlags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;
    dl->AddRect(a, b, col, rounding, ImDrawFlags_None, 3.0f);
    dl->Flags = savedFlags;
}

extern "C" bool dialogNavIsInsideSubRegion(void) {
    ImGuiContext *g = ImGui::GetCurrentContext();
    if (!g || !g->NavWindow) return false;
    /* True for ImGui child windows (BeginChild) and table scroll
       regions (BeginTable with ScrollX/Y), both of which create a
       child window whose RootWindow points up to the user's outer
       Begin().  When nav is inside one of these, B should pop out
       via ImGui's NavCancel rather than close the dialog.

       Nav-flattened children (ImGuiChildFlags_NavFlattened) are
       excluded: nav crosses their boundary as if they weren't there,
       and ImGui's NavCancel won't pop out of one (its exit-child path
       needs the focus root to have a parent, which a flattened child's
       top-level root does not).  So treating them as a sub-region would
       leave B dead inside the child.  RootWindowForNav walks up through
       any flattened ancestors, so it equals RootWindow for a flattened
       child (not a sub-region) while a real scroll/child region differs. */
    return g->NavWindow->RootWindowForNav != g->NavWindow->RootWindow;
}
