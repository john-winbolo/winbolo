/*
 * Tests for the focus outline's key rule (imgui_nav_outline.cpp). While the
 * outline is hidden, the first Enter, Space, arrow, Home, End, PageUp or
 * PageDown press only shows it on the focused item, and the next press acts.
 * A press with Ctrl or Alt held is not claimed and keeps its ImGui action. An item focused from code (a
 * message box's OK button) shows the outline at once, so Enter presses it at
 * once. A quiet focus from code (the lobby's Ready button at open) leaves the
 * outline hidden.
 *
 * Each case drives a real ImGui context with no renderer: a window with two
 * buttons, A and B, and dialogDrawNavOutline() at the end of each frame, as
 * the game calls it.
 */

#include <stdlib.h>

#include "imgui.h"
#include "imgui_internal.h"

#include "test_harness.h"
#include "imgui_nav_outline.h"

namespace {

struct NavRig {
    ImGuiContext *ctx       = nullptr;
    const char   *name      = "";
    bool          drawWin   = false;
    bool          focusB    = false; /* SetKeyboardFocusHere before B, once */
    bool          quiet     = false; /* ... as a quiet focus (the lobby) */
    bool          setFocusB = false; /* SetFocusID on B in this frame, once */
    bool          enterSeen = false; /* Enter read with owner Any, as the footer reads it */
    int           pressA    = 0;
    int           pressB    = 0;
    ImGuiID       idA       = 0;
    ImGuiID       idB       = 0;
};

void rigFrame(NavRig &r) {
    ImGui::SetCurrentContext(r.ctx);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    if (r.drawWin) {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
        ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
        ImGui::Begin(r.name, nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::Button("A")) r.pressA++;
        r.idA = ImGui::GetItemID();
        if (r.focusB) {
            ImGui::SetKeyboardFocusHere();
            if (r.quiet) dialogNavOutlineQuietCodeFocus();
            r.focusB = false;
        }
        if (ImGui::Button("B")) r.pressB++;
        r.idB = ImGui::GetItemID();
        if (r.setFocusB) {
            ImGui::SetFocusID(r.idB, ImGui::GetCurrentWindow());
            r.setFocusB = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) r.enterSeen = true;
        ImGui::End();
    }
    dialogDrawNavOutline();
    ImGui::Render();
}

void rigFrames(NavRig &r, int n) {
    for (int i = 0; i < n; i++) rigFrame(r);
}

/* One press and release of a key, one frame each, then one more frame. */
void rigPress(NavRig &r, ImGuiKey key) {
    ImGui::SetCurrentContext(r.ctx);
    ImGui::GetIO().AddKeyEvent(key, true);
    rigFrame(r);
    ImGui::GetIO().AddKeyEvent(key, false);
    rigFrame(r);
    rigFrame(r);
}

/* A new context, as a dialog has. A mouse click on empty space with no window
   drawn hides the outline (the outline's state is shared by all contexts),
   then the window opens and ImGui's nav init focuses A. */
void rigCreate(NavRig &r, const char *name) {
    r       = NavRig();
    r.name  = name;
    r.ctx   = ImGui::CreateContext();
    ImGui::SetCurrentContext(r.ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename  = nullptr;
    io.LogFilename  = nullptr;
    io.DisplaySize  = ImVec2(800.0f, 600.0f);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAlways = true;
    /* ImGui on a Mac swaps Ctrl and Cmd in AddKeyEvent, so the rig's
       ImGuiMod_Ctrl would arrive as Super there.  Off, so the keys mean
       the same on every platform. */
    io.ConfigMacOSXBehaviors = false;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
}

void rigOpen(NavRig &r, const char *name) {
    rigCreate(r, name);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(700.0f, 500.0f);
    io.AddMouseButtonEvent(0, true);
    rigFrame(r);
    io.AddMouseButtonEvent(0, false);
    rigFrame(r);
    r.drawWin = true;
    rigFrames(r, 3);
}

void rigClose(NavRig &r) {
    if (r.ctx) ImGui::DestroyContext(r.ctx);
    r.ctx = nullptr;
}

/* The first press of `key` with the outline hidden only shows it on A; the
   second press presses A. */
int checkFirstPressShows(ImGuiKey key, const char *name) {
    NavRig r;
    rigOpen(r, name);
    int rc = 0;
    UT_CHECK_MSG(GImGui->NavId == r.idA && r.idA != 0,
                 "nav init should focus A (NavId %08x, A %08x)",
                 (unsigned)GImGui->NavId, (unsigned)r.idA);
    UT_CHECK(!dialogNavOutlineIsShown());

    rigPress(r, key);
    UT_CHECK_MSG(r.pressA == 0, "first press pressed A %d time(s)", r.pressA);
    UT_CHECK(dialogNavOutlineIsShown());
    UT_CHECK(GImGui->NavId == r.idA);

    rigPress(r, key);
    UT_CHECK_MSG(r.pressA == 1, "second press pressed A %d time(s)", r.pressA);
    UT_CHECK(r.pressB == 0);
done:
    rigClose(r);
    return rc;
}

/* Hold or let go of a modifier, with one frame after it, so the claim made
   at the end of that frame sees it. */
void rigMod(NavRig &r, ImGuiKey mod, bool down) {
    ImGui::SetCurrentContext(r.ctx);
    ImGui::GetIO().AddKeyEvent(mod, down);
    rigFrame(r);
}

/* With the outline hidden and the focus on the start item (A, or B from a
   quiet focus), the first press of `key` only shows the outline and leaves
   the focus where it is; the second press moves the focus to the other
   item. No item is pressed. */
int checkFirstMoveShows(ImGuiKey key, bool startOnB, const char *name) {
    NavRig r;
    rigOpen(r, name);
    int rc = 0;
    if (startOnB) {
        r.focusB = true;
        r.quiet  = true;
        rigFrames(r, 2);
    }
    {
        const ImGuiID start = startOnB ? r.idB : r.idA;
        const ImGuiID other = startOnB ? r.idA : r.idB;
        UT_CHECK_MSG(GImGui->NavId == start && start != 0,
                     "%s: focus should start on %s (NavId %08x)", name,
                     startOnB ? "B" : "A", (unsigned)GImGui->NavId);
        UT_CHECK(!dialogNavOutlineIsShown());

        rigPress(r, key);
        UT_CHECK_MSG(GImGui->NavId == start,
                     "%s: first press moved the focus (NavId %08x)", name,
                     (unsigned)GImGui->NavId);
        UT_CHECK(dialogNavOutlineIsShown());

        rigPress(r, key);
        UT_CHECK_MSG(GImGui->NavId == other,
                     "%s: second press should move the focus (NavId %08x, "
                     "want %08x)",
                     name, (unsigned)GImGui->NavId, (unsigned)other);
        UT_CHECK(r.pressA == 0 && r.pressB == 0);
    }
done:
    rigClose(r);
    return rc;
}

/* An allocator that gives a freed ImGuiContext block to the next
   ImGuiContext, so a new context gets the old one's address. */
void *s_ctxBlock = nullptr; /* the live context's block */
void *s_ctxSpare = nullptr; /* a freed context block, kept for reuse */

void *reuseAlloc(size_t sz, void *) {
    if (sz != sizeof(ImGuiContext)) return malloc(sz);
    void *p = s_ctxSpare ? s_ctxSpare : malloc(sz);
    s_ctxSpare = nullptr;
    s_ctxBlock = p;
    return p;
}

void reuseFree(void *p, void *) {
    if (p && p == s_ctxBlock) {
        s_ctxBlock = nullptr;
        s_ctxSpare = p;
        return;
    }
    free(p);
}

} // namespace

extern "C" int run_nav_outline_first_enter_shows(void) {
    if (checkFirstPressShows(ImGuiKey_Enter, "EnterWin")) return 1;
    if (checkFirstPressShows(ImGuiKey_KeypadEnter, "KeypadEnterWin")) return 1;

    /* A dialog footer reads Enter with owner Any (dialog_footer.cpp), so its
       Confirm on Enter still sees the first Enter while the outline is
       hidden. */
    NavRig r;
    rigOpen(r, "FooterWin");
    int rc = 0;
    UT_CHECK(!dialogNavOutlineIsShown());
    rigPress(r, ImGuiKey_Enter);
    UT_CHECK(r.enterSeen);
    UT_CHECK(r.pressA == 0);
done:
    rigClose(r);
    return rc;
}

extern "C" int run_nav_outline_first_space_shows(void) {
    return checkFirstPressShows(ImGuiKey_Space, "SpaceWin");
}

extern "C" int run_nav_outline_first_arrow_shows(void) {
    NavRig r;
    rigOpen(r, "ArrowWin");
    int rc = 0;
    UT_CHECK(GImGui->NavId == r.idA && r.idA != 0);
    UT_CHECK(!dialogNavOutlineIsShown());

    rigPress(r, ImGuiKey_DownArrow);
    UT_CHECK_MSG(GImGui->NavId == r.idA,
                 "first Down moved the focus (NavId %08x, A %08x, B %08x)",
                 (unsigned)GImGui->NavId, (unsigned)r.idA, (unsigned)r.idB);
    UT_CHECK(dialogNavOutlineIsShown());

    rigPress(r, ImGuiKey_DownArrow);
    UT_CHECK_MSG(GImGui->NavId == r.idB,
                 "second Down should focus B (NavId %08x, B %08x)",
                 (unsigned)GImGui->NavId, (unsigned)r.idB);
    UT_CHECK(r.pressA == 0 && r.pressB == 0);
done:
    rigClose(r);
    return rc;
}

/* A message box focuses OK from code: the outline shows at once, and the first
   Enter presses OK. */
extern "C" int run_nav_outline_code_focus_enter_acts(void) {
    NavRig r;
    rigOpen(r, "MsgBoxWin");
    int rc = 0;
    r.focusB = true;
    rigFrames(r, 2);
    UT_CHECK_MSG(GImGui->NavId == r.idB && r.idB != 0,
                 "focus from code should focus B (NavId %08x, B %08x)",
                 (unsigned)GImGui->NavId, (unsigned)r.idB);
    UT_CHECK(dialogNavOutlineIsShown());

    rigPress(r, ImGuiKey_Enter);
    UT_CHECK_MSG(r.pressB == 1, "first Enter pressed B %d time(s)", r.pressB);
    UT_CHECK(r.pressA == 0);
done:
    rigClose(r);
    return rc;
}

/* The lobby seeds the focus on Ready with a quiet focus: it opens with the
   outline hidden, the first Enter only shows it, and the second presses Ready. */
extern "C" int run_nav_outline_quiet_focus_hidden(void) {
    NavRig r;
    rigOpen(r, "LobbyWin");
    int rc = 0;
    r.focusB = true;
    r.quiet  = true;
    rigFrames(r, 2);
    UT_CHECK_MSG(GImGui->NavId == r.idB && r.idB != 0,
                 "quiet focus should still focus B (NavId %08x, B %08x)",
                 (unsigned)GImGui->NavId, (unsigned)r.idB);
    UT_CHECK(!dialogNavOutlineIsShown());

    rigPress(r, ImGuiKey_Enter);
    UT_CHECK_MSG(r.pressB == 0, "first Enter pressed B %d time(s)", r.pressB);
    UT_CHECK(dialogNavOutlineIsShown());

    rigPress(r, ImGuiKey_Enter);
    UT_CHECK_MSG(r.pressB == 1, "second Enter pressed B %d time(s)", r.pressB);

    /* The quiet request is used up: a later focus from code shows the
       outline again. */
    ImGui::SetCurrentContext(r.ctx);
    ImGui::GetIO().AddMouseButtonEvent(0, true); /* hide it first */
    rigFrame(r);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    rigFrame(r);
    UT_CHECK(!dialogNavOutlineIsShown());
    r.focusB = true;
    r.quiet  = false;
    rigFrames(r, 2);
    UT_CHECK(GImGui->NavId == r.idB);
    UT_CHECK(dialogNavOutlineIsShown());
done:
    rigClose(r);
    return rc;
}

/* Ctrl+Enter with the outline hidden is not claimed, so it presses A at
   once, as on main, and it does not show the outline. The next plain
   Enter only shows the outline. */
extern "C" int run_nav_outline_ctrl_enter_not_claimed(void) {
    NavRig r;
    rigOpen(r, "CtrlEnterWin");
    int rc = 0;
    UT_CHECK(GImGui->NavId == r.idA && r.idA != 0);
    UT_CHECK(!dialogNavOutlineIsShown());

    rigMod(r, ImGuiMod_Ctrl, true);
    rigPress(r, ImGuiKey_Enter);
    UT_CHECK_MSG(r.pressA == 1, "Ctrl+Enter pressed A %d time(s)", r.pressA);
    UT_CHECK(!dialogNavOutlineIsShown());
    rigMod(r, ImGuiMod_Ctrl, false);

    rigPress(r, ImGuiKey_Enter);
    UT_CHECK_MSG(r.pressA == 1, "plain Enter pressed A, count %d", r.pressA);
    UT_CHECK(dialogNavOutlineIsShown());
done:
    rigClose(r);
    return rc;
}

/* Alt+Down with the outline hidden is not claimed, so it moves the focus to
   B at once, as on main. */
extern "C" int run_nav_outline_alt_arrow_not_claimed(void) {
    NavRig r;
    rigOpen(r, "AltArrowWin");
    int rc = 0;
    UT_CHECK(GImGui->NavId == r.idA && r.idA != 0);
    UT_CHECK(!dialogNavOutlineIsShown());

    rigMod(r, ImGuiMod_Alt, true);
    rigPress(r, ImGuiKey_DownArrow);
    UT_CHECK_MSG(GImGui->NavId == r.idB,
                 "Alt+Down should focus B (NavId %08x, A %08x, B %08x)",
                 (unsigned)GImGui->NavId, (unsigned)r.idA, (unsigned)r.idB);
    UT_CHECK(!dialogNavOutlineIsShown());
    rigMod(r, ImGuiMod_Alt, false);
    UT_CHECK(r.pressA == 0 && r.pressB == 0);
done:
    rigClose(r);
    return rc;
}

/* End from A and Home from B: the first press only shows the outline. */
extern "C" int run_nav_outline_first_home_end_shows(void) {
    if (checkFirstMoveShows(ImGuiKey_End, false, "EndWin")) return 1;
    return checkFirstMoveShows(ImGuiKey_Home, true, "HomeWin");
}

/* PageDown from A and PageUp from B: the first press only shows the
   outline. */
extern "C" int run_nav_outline_first_page_keys_shows(void) {
    if (checkFirstMoveShows(ImGuiKey_PageDown, false, "PageDownWin")) return 1;
    return checkFirstMoveShows(ImGuiKey_PageUp, true, "PageUpWin");
}

/* A destroyed context leaves no record of its focus from code. A message
   box focuses B from code, so the outline shows, and the box is then
   destroyed. A new context at the same address, with the same window, has
   B as its first NavId in its first frame. The outline must stay hidden
   there: the record of the old box's B must be gone. */
extern "C" int run_nav_outline_destroyed_context_forgets_focus(void) {
    ImGuiMemAllocFunc oldAlloc = nullptr;
    ImGuiMemFreeFunc  oldFree  = nullptr;
    void             *oldUser  = nullptr;
    ImGui::GetAllocatorFunctions(&oldAlloc, &oldFree, &oldUser);
    ImGui::SetAllocatorFunctions(reuseAlloc, reuseFree, nullptr);

    NavRig        r;
    int           rc      = 0;
    ImGuiContext *oldCtx  = nullptr;
    ImGuiID       oldIdB  = 0;
    rigOpen(r, "ReuseWin");
    r.focusB = true;
    rigFrames(r, 2);
    UT_CHECK_MSG(GImGui->NavId == r.idB && r.idB != 0,
                 "focus from code should focus B (NavId %08x, B %08x)",
                 (unsigned)GImGui->NavId, (unsigned)r.idB);
    UT_CHECK(dialogNavOutlineIsShown());
    oldCtx = r.ctx;
    oldIdB = r.idB;
    rigClose(r);

    /* No mouse click here: a click would clear the record in any case. */
    rigCreate(r, "ReuseWin");
    UT_CHECK_MSG(r.ctx == oldCtx,
                 "the new context should reuse the old address (%p, %p)",
                 (void *)r.ctx, (void *)oldCtx);
    r.drawWin   = true;
    r.setFocusB = true;
    rigFrame(r);
    UT_CHECK_MSG(r.idB == oldIdB && GImGui->NavId == r.idB,
                 "B should have the old id and the focus (NavId %08x, B %08x, "
                 "old B %08x)",
                 (unsigned)GImGui->NavId, (unsigned)r.idB, (unsigned)oldIdB);
    UT_CHECK_MSG(!dialogNavOutlineIsShown(),
                 "the old context's focus from code still shows the outline");
done:
    rigClose(r);
    ImGui::SetAllocatorFunctions(oldAlloc, oldFree, oldUser);
    free(s_ctxSpare);
    s_ctxSpare = nullptr;
    return rc;
}
