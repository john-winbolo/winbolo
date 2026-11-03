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

/* When the outline may show.  Most contexts set
   ConfigNavCursorVisibleAlways, so ImGui keeps NavCursorVisible on from
   the first frame (a window that opens, a default-focus item, a mouse
   click on a button or a combo all give it a NavId).  That flag also
   lets Space and Enter press the focused item, so it stays on; only the
   drawing waits.  A Tab press shows the outline; a mouse button click
   (any button, also on empty space) hides it again.  As in
   Discord, a mouse move or the mouse wheel does not hide it.  ImGui
   itself does not hide its nav cursor on a mouse move either: a move
   only clears NavHighlightItemUnderNav, and with
   ConfigNavCursorVisibleAlways
   NavUpdate keeps NavCursorVisible on.  Also as in Discord, the Tab
   that shows the outline does not move focus: the outline shows on the
   item that already has focus, and only the next Tab moves on (see
   navOutlineClaimTab).  The other keys that move or press the focused
   item (Enter, keypad Enter, Space, the arrow keys, the d-pad and the
   pad's activate button) do the same: while the outline is hidden, the
   first press only shows it, and only the next press acts (see
   navOutlineClaimNavKeys).  A controller keeps it on: any pad button press
   here, or the game's own record that the controller is the device in
   use (Steam Input reaches ImGui as arrow keys, not pad keys).

   An item that a dialog focuses from code (SetKeyboardFocusHere, such
   as a message box's OK button) also shows the outline at once, so the
   player sees what Enter will press, and Enter presses it at once.  A
   dialog can ask for a quiet focus instead (dialogNavOutlineQuietCodeFocus,
   used for the lobby's Ready button at open): then the outline stays
   hidden, and the first Enter only shows it.  The outline of a focus
   from code shows while that item keeps the focus, until a mouse
   button click.  A text field being edited does not count: its caret
   already marks it.  A mouse click on a button or a combo does not
   focus from code, so the outline stays hidden there. */
static bool s_navOutlineByKey     = false;
static bool s_navOutlineByPadUser = false;

/* The item focused from code, in the context s_codeFocusCtx.  Like
   s_tabStuckCtx below, the context pointers are only compared, never
   dereferenced.  navOutlineShutdown clears them when their context is
   destroyed, so a later context at the same address does not inherit
   them. */
static const ImGuiContext *s_codeFocusCtx        = nullptr;
static ImGuiID             s_codeFocusId         = 0;
static const ImGuiContext *s_codeFocusPendingCtx = nullptr;
/* The context whose next focus from code does not show the outline
   (dialogNavOutlineQuietCodeFocus). */
static const ImGuiContext *s_codeFocusQuietCtx   = nullptr;

static bool navOutlineShown(const ImGuiContext *g) {
    return s_navOutlineByKey || s_navOutlineByPadUser ||
           (g == s_codeFocusCtx && s_codeFocusId != 0 &&
            g->NavId == s_codeFocusId);
}

extern "C" void dialogNavOutlineSetControllerInUse(bool inUse) {
    s_navOutlineByPadUser = inUse;
}

extern "C" void dialogNavOutlineQuietCodeFocus(void) {
    s_codeFocusQuietCtx = ImGui::GetCurrentContext();
}

extern "C" bool dialogNavOutlineIsShown(void) {
    const ImGuiContext *g = ImGui::GetCurrentContext();
    return g && navOutlineShown(g);
}

/* Owner id for the Tab key while the outline is hidden. */
static const ImGuiID kNavOutlineTabOwner = 0x4E61764Fu; /* "NavO" */

/* While the outline is hidden, claim the Tab key for the next frame, so
   the first Tab only shows the outline and does not move focus.  ImGui
   reads Tab for tabbing in NavUpdate (inside NewFrame), with
   IsKeyPressed(ImGuiKey_Tab, ..., ImGuiKeyOwner_NoOwner); that read fails
   while another id owns the key.  The claim is made here, at the end of a
   frame, so it is in place before the next NewFrame.  It has no lock
   flag, so a plain IsKeyPressed(ImGuiKey_Tab) (owner Any), as the tracker
   below and ImGuiColorTextEdit use, still sees the press.  The game reads
   its own keys from SDL events, so a Tab bound to a game key is not
   touched.

   A text field being edited gets the claim too, so the first Tab only
   shows the outline on it: the field stays active, with its caret, and
   typing goes on.  Without the claim ImGui's tabbing moves focus out of
   the field to the next item.  The claim is not made for a field that
   reads Tab itself (ImGuiInputTextFlags_AllowTabInput or
   _CallbackCompletion): such a field reads Tab through Shortcut() with
   its own id, which fails while another id owns the key.  Nor is it made
   while any other item is active.  No claim with no live focused item:
   then the first Tab focuses the first item, as before.  No claim while
   ImGui's own cursor is hidden: a context without
   ConfigNavCursorVisibleAlways (the onboarding and connecting dialogs)
   keeps ImGui's auto mode, where the first Tab shows the cursor and
   takes focus to the first item, as before.  With the claim there, that
   Tab was lost and the outline still did not draw (NavCursorVisible
   stayed off), so the player had to press Tab twice.  The claim stays
   while Tab is held, so a held first Tab does not repeat into a move; it
   is given back once Tab is up. */
static bool navOutlineActiveItemAllowsClaim(ImGuiContext *g) {
    if (g->ActiveId == 0) return true;
    const ImGuiInputTextState &st = g->InputTextState;
    return st.ID == g->ActiveId &&
           (st.Flags & (ImGuiInputTextFlags_AllowTabInput |
                        ImGuiInputTextFlags_CallbackCompletion)) == 0;
}

static void navOutlineClaimTab(ImGuiContext *g) {
    ImGuiID owner = ImGui::GetKeyOwner(ImGuiKey_Tab);
    bool    want  = !navOutlineShown(g) &&
                    g->NavCursorVisible && g->NavWindow && g->NavId != 0 &&
                    g->NavIdIsAlive && navOutlineActiveItemAllowsClaim(g);
    if (want) {
        if (owner == ImGuiKeyOwner_NoOwner || owner == kNavOutlineTabOwner)
            ImGui::SetKeyOwner(ImGuiKey_Tab, kNavOutlineTabOwner);
    } else if (owner == kNavOutlineTabOwner &&
               !ImGui::IsKeyDown(ImGuiKey_Tab)) {
        ImGui::SetKeyOwner(ImGuiKey_Tab, ImGuiKeyOwner_NoOwner);
    }
}

/* While the outline is hidden, claim the other keys that move or press
   the focused item for the next frame, so the first press only shows the
   outline, as the first Tab does: Enter, keypad Enter, Space, the arrow
   keys, Home, End, PageUp, PageDown, the d-pad and the pad's activate
   button.  ImGui reads each of
   them in NavUpdate with IsKeyPressed or IsKeyDown(key, ...,
   ImGuiKeyOwner_NoOwner), which fails while another id owns the key.
   navOutlineTrackInput reads the press with owner Any and shows the
   outline; the claim is given back once the key is up, so the next press
   acts.  The claim has no lock flag, so a plain
   IsKeyPressed(ImGuiKey_Enter) (owner Any) still sees the press: the
   dialog footer's Confirm on Enter (dialog_footer.cpp) still confirms on
   the first Enter, as on main.

   No claim while any item is active: a text field being edited reads
   Enter and the arrow keys itself, so Enter still submits it and the
   arrows still move the caret.  No claim with no live focused item, nor
   while ImGui's own cursor is hidden (auto mode): there ImGui presses no
   item, and an arrow key takes the focus to an item, so the press is left
   to ImGui.  The claim stays while the key is held, so a held first
   arrow does not repeat into a move.  The pad's activate button is
   FaceDown, or FaceRight with ConfigNavSwapGamepadButtons; the other
   face button is ImGui's cancel and is never claimed.

   No keyboard claim while Ctrl or Alt is held.  ImGui tests no modifier
   for these keys, so on main Ctrl+Enter pressed the focused item and
   Ctrl+Arrow moved the focus.  Such a press keeps that action, and does
   not show the outline.  A claim with no show would make the press do
   nothing at all, every time.  The claim is made at the end of the frame
   before the press, so the test reads the modifiers at that time; if Ctrl
   or Alt goes down in the same frame as the key, the key is still
   claimed, and navOutlineTrackInput then shows the outline for it.  The
   pad keys are claimed as before, as a pad has no Ctrl or Alt.  The
   other order is not covered either: if Ctrl or Alt is let go in the
   same frame as the key goes down, the claim was not made, so ImGui
   acts on that press with the outline hidden, and navOutlineTrackInput
   then shows the outline.  This is one frame wide and is left as it
   is. */
static void navOutlineClaimKey(ImGuiKey key, bool want) {
    ImGuiID owner = ImGui::GetKeyOwner(key);
    if (want) {
        if (owner == ImGuiKeyOwner_NoOwner || owner == kNavOutlineTabOwner)
            ImGui::SetKeyOwner(key, kNavOutlineTabOwner);
    } else if (owner == kNavOutlineTabOwner && !ImGui::IsKeyDown(key)) {
        ImGui::SetKeyOwner(key, ImGuiKeyOwner_NoOwner);
    }
}

static const ImGuiKey kNavOutlineKeyboardKeys[] = {
    ImGuiKey_Enter,     ImGuiKey_KeypadEnter, ImGuiKey_Space,
    ImGuiKey_LeftArrow, ImGuiKey_RightArrow,  ImGuiKey_UpArrow,
    ImGuiKey_DownArrow, ImGuiKey_Home,        ImGuiKey_End,
    ImGuiKey_PageUp,    ImGuiKey_PageDown,
};
static const ImGuiKey kNavOutlinePadMoveKeys[] = {
    ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
    ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown,
};

static void navOutlineClaimNavKeys(ImGuiContext *g) {
    const bool want = !navOutlineShown(g) && g->NavCursorVisible &&
                      g->NavWindow && g->NavId != 0 && g->NavIdIsAlive &&
                      g->ActiveId == 0;
    const bool wantKeyboard = want && !g->IO.KeyCtrl && !g->IO.KeyAlt;
    for (ImGuiKey key : kNavOutlineKeyboardKeys)
        navOutlineClaimKey(key, wantKeyboard);
    for (ImGuiKey key : kNavOutlinePadMoveKeys) navOutlineClaimKey(key, want);
    const bool swap = g->IO.ConfigNavSwapGamepadButtons;
    navOutlineClaimKey(ImGuiKey_GamepadFaceDown,  want && !swap);
    navOutlineClaimKey(ImGuiKey_GamepadFaceRight, want && swap);
}

/* A forward Tab that ImGui cannot move.  ImGui's Tab walk counts from the
   focused item, and it skips a disabled item, so with the focus on a
   disabled item (the game finder's Reload button while a search runs, or
   Join once a refresh clears the list) no item comes after it and Tab
   does nothing.  With no focused item (a click on an empty part of a
   window: the finder's border around its panel, or the empty part of a
   list) Tab also does nothing: these contexts set
   ConfigNavCursorVisibleAlways, so ImGui always tabs forward from the
   focused item and never takes its "focus the first item" path.  Arrow
   keys still work in the first case, as they move from the item's rect.

   Fix: just after NewFrame, when ImGui has made a forward Tab request
   from such a state, change it to ImGui's "focus the first item" request
   (NavTabbingDir 0): the first tab stop of the window takes the focus.
   When the focused window has no items (the finder's full-screen host
   behind its panel), the top window that has items takes the focus
   first.  A Shift+Tab already moves in these states (ImGui takes the last
   item), so it is left alone.  It runs from a NewFramePost hook because
   NavUpdate in NewFrame drops any nav request made before it, and the
   Tab request is made inside NewFrame.

   The state is read at the end of the previous frame (NavIdIsAlive is
   cleared in NavUpdate), for that context only.  s_tabStuckCtx is only
   compared with the hook's context, never dereferenced, so it is safe
   after that context is destroyed; the hook is destroyed with it, and
   navOutlineShutdown clears s_tabStuckCtx. */
static const ImGuiContext *s_tabStuckCtx = nullptr;
static bool                s_tabStuck    = false;

static void navOutlineNoteTabState(ImGuiContext *g) {
    s_tabStuckCtx = g;
    s_tabStuck    = g->NavWindow && g->ActiveId == 0 &&
                    (g->NavId == 0 || !g->NavIdIsAlive ||
                     (g->NavIdItemFlags & ImGuiItemFlags_Disabled));
}

static bool navWindowHasItems(const ImGuiWindow *w) {
    return (w->DC.NavLayersActiveMask & (1 << ImGuiNavLayer_Main)) != 0;
}

static void navOutlineFixStuckTab(ImGuiContext *ctx, ImGuiContextHook *) {
    ImGuiContext &g = *ctx;
    if (ctx != s_tabStuckCtx || !s_tabStuck) return;
    if (!g.NavWindow || !g.NavMoveSubmitted) return;
    if ((g.NavMoveFlags & ImGuiNavMoveFlags_IsTabbing) == 0 ||
        (g.NavMoveFlags & ImGuiNavMoveFlags_FocusApi) != 0 ||
        g.NavTabbingDir != +1)
        return;
    if (!navWindowHasItems(g.NavWindow)) {
        ImGuiWindow *target = nullptr;
        for (int i = g.WindowsFocusOrder.Size - 1; i >= 0 && !target; i--) {
            ImGuiWindow *w = g.WindowsFocusOrder[i];
            if (w != g.NavWindow && w->WasActive && !w->Hidden &&
                !(w->Flags & (ImGuiWindowFlags_NoNavFocus |
                              ImGuiWindowFlags_NoNavInputs)) &&
                navWindowHasItems(w))
                target = w;
        }
        if (!target) return;
        /* This runs in a NewFramePost hook, before any Begin this frame,
           so each window's NavLayersActiveMask is last frame's value.  A
           window that first appears this frame can be chosen only on the
           next Tab. */
        ImGui::FocusWindow(target);
    }
    g.NavTabbingDir = 0;
}

/* Find the item a dialog focused from code.  SetKeyboardFocusHere makes
   a move request with ImGuiNavMoveFlags_FocusApi; the items after it
   score it in the same frame, and NavUpdate in the next NewFrame applies
   it, setting NavId to the result.  Before NewFrame (NewFramePre) the
   request and its result are still those of the last frame, so note
   that a request with a result is waiting; just after NewFrame
   (NewFramePost) NavId is the item it focused.  No Tab, arrow key or pad
   move uses FocusApi. */
static void navOutlineNoteFocusRequest(ImGuiContext *ctx, ImGuiContextHook *) {
    const ImGuiContext &g = *ctx;
    bool req = g.NavMoveSubmitted &&
               (g.NavMoveFlags & ImGuiNavMoveFlags_FocusApi) != 0 &&
               (g.NavMoveResultLocal.ID != 0 || g.NavMoveResultOther.ID != 0 ||
                g.NavTabbingResultFirst.ID != 0);
    /* A quiet focus (dialogNavOutlineQuietCodeFocus) is the request made
       in the frame that just ended.  It is used up here either way, so it
       cannot mute a later focus from code. */
    if (s_codeFocusQuietCtx == ctx) {
        s_codeFocusQuietCtx = nullptr;
        req                 = false;
    }
    if (req)
        s_codeFocusPendingCtx = ctx;
    else if (s_codeFocusPendingCtx == ctx)
        s_codeFocusPendingCtx = nullptr;
}

static void navOutlineTakeFocusRequest(ImGuiContext *ctx) {
    if (s_codeFocusPendingCtx != ctx) return;
    s_codeFocusPendingCtx = nullptr;
    if (ctx->NavId == 0) return;
    s_codeFocusCtx = ctx;
    s_codeFocusId  = ctx->NavId;
}

static void navOutlineNewFramePost(ImGuiContext *ctx, ImGuiContextHook *hook) {
    navOutlineTakeFocusRequest(ctx);
    navOutlineFixStuckTab(ctx, hook);
}

/* The context is being destroyed (DestroyContext calls Shutdown, which
   calls this hook first).  Forget every record kept for it.  Without
   this, a new context created later at the same address would inherit
   them: an item focused from code in the old context would show the
   outline in the new one if its first NavId had the same id. */
static void navOutlineShutdown(ImGuiContext *ctx, ImGuiContextHook *) {
    if (s_codeFocusCtx == ctx) {
        s_codeFocusCtx = nullptr;
        s_codeFocusId  = 0;
    }
    if (s_codeFocusPendingCtx == ctx) s_codeFocusPendingCtx = nullptr;
    if (s_codeFocusQuietCtx == ctx) s_codeFocusQuietCtx = nullptr;
    if (s_tabStuckCtx == ctx) {
        s_tabStuckCtx = nullptr;
        s_tabStuck    = false;
    }
}

/* Add the hooks once per context. */
static void navOutlineEnsureHook(ImGuiContext *g, ImGuiContextHookType type,
                                 ImGuiContextHookCallback callback) {
    for (const ImGuiContextHook &h : g->Hooks)
        if (h.Owner == kNavOutlineTabOwner && h.Type == type) return;
    ImGuiContextHook hook;
    hook.Type     = type;
    hook.Owner    = kNavOutlineTabOwner;
    hook.Callback = callback;
    ImGui::AddContextHook(g, &hook);
}

static void navOutlineEnsureHooks(ImGuiContext *g) {
    navOutlineEnsureHook(g, ImGuiContextHookType_NewFramePre,
                         navOutlineNoteFocusRequest);
    navOutlineEnsureHook(g, ImGuiContextHookType_NewFramePost,
                         navOutlineNewFramePost);
    navOutlineEnsureHook(g, ImGuiContextHookType_Shutdown,
                         navOutlineShutdown);
}

/* The item focused from code stops showing the outline once the focus
   leaves it, or while it is a text field being edited. */
static void navOutlineCheckCodeFocus(ImGuiContext *g) {
    if (g != s_codeFocusCtx || s_codeFocusId == 0) return;
    const bool editing = g->ActiveId == s_codeFocusId &&
                         g->InputTextState.ID == s_codeFocusId;
    if (g->NavId != s_codeFocusId || editing) s_codeFocusId = 0;
}

static void navOutlineTrackInput(ImGuiContext *g) {
    const ImGuiIO &io = g->IO;
    /* Only a mouse button click hides the outline.  A mouse move or the
       mouse wheel does not.  Once hidden, navOutlineClaimTab claims Tab
       again, so the next Tab again only shows the outline. */
    for (int b = 0; b < IM_ARRAYSIZE(io.MouseClicked); b++)
        if (io.MouseClicked[b]) {
            s_navOutlineByKey = false;
            s_codeFocusId     = 0;
        }
    navOutlineCheckCodeFocus(g);

    /* Tab counts only while a window takes nav input, so a Tab bound to
       a game key during play does not switch the outline on.  Ctrl+Tab
       and Alt+Tab do not count, as in ImGui's own tabbing: Alt+Tab to
       another program reaches the game as a Tab press. */
    if (g->NavWindow && !io.KeyCtrl && !io.KeyAlt &&
        ImGui::IsKeyPressed(ImGuiKey_Tab, false))
        s_navOutlineByKey = true;
    /* Enter, Space, the arrow keys, Home, End, PageUp and PageDown show it
       the same way, while a window takes keyboard nav input and no item is
       active (a text field reads these keys itself).  While the outline
       was hidden, navOutlineClaimNavKeys claimed the key, so ImGui did not
       act on this press; the next press acts.  A press with Ctrl or Alt
       held counts only if the key was claimed: an unclaimed press already
       had its ImGui action (navOutlineClaimNavKeys).  The claim is read
       here before navOutlineClaimNavKeys runs for the next frame. */
    if (g->NavWindow && g->ActiveId == 0 &&
        (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard))
        for (ImGuiKey key : kNavOutlineKeyboardKeys)
            if (ImGui::IsKeyPressed(key, false) &&
                ((!io.KeyCtrl && !io.KeyAlt) ||
                 ImGui::GetKeyOwner(key) == kNavOutlineTabOwner))
                s_navOutlineByKey = true;
    for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadR3; k++)
        if (ImGui::IsKeyPressed((ImGuiKey)k, false)) s_navOutlineByKey = true;
    navOutlineClaimTab(g);
    navOutlineClaimNavKeys(g);
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
    navOutlineEnsureHooks(g);
    navOutlineNoteTabState(g);
    if (!navOutlineShown(g)) return;

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
