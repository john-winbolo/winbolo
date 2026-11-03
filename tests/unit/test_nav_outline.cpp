/*
 * Tests for the focus outline's key rule (imgui_nav_outline.cpp). While the
 * outline is hidden, the first Enter, Space or arrow key press only shows it
 * on the focused item, and the next press acts. An item focused from code (a
 * message box's OK button) shows the outline at once, so Enter presses it at
 * once. A quiet focus from code (the lobby's Ready button at open) leaves the
 * outline hidden.
 *
 * Each case drives a real ImGui context with no renderer: a window with two
 * buttons, A and B, and dialogDrawNavOutline() at the end of each frame, as
 * the game calls it.
 */

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
void rigOpen(NavRig &r, const char *name) {
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
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
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
