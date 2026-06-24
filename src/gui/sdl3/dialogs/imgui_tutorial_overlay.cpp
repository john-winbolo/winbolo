/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_tutorial_overlay.cpp
 * Purpose:       See imgui_tutorial_overlay.h.  An in-loop
 *                modal popup drawn through the main ImGui
 *                context (like imgui_deck_pause), replacing the
 *                old blocking imguiMessageBoxRich sequence so
 *                the game's input gate and solo-pause path apply
 *                while a tutorial message is up.
 *********************************************************/

#include <cfloat>

#include "imgui.h"
#include "imgui_tutorial_overlay.h"
#include "imgui_messagebox.h"
#include "../sdl3imgui.h"    /* sdl3ImguiGetUiScale */
#include "../tutorial_text.h"
#include "../../winbolo.h"   /* DIALOG_BOX_TITLE */

extern "C" {
#include "tutorial.h"        /* TUTORIAL_MAX_MSGS */
}

/* All state is touched only from the main thread: tutorialOverlayShow runs
   inside the game tick, render/advance inside the render pass — the two
   never overlap (both on the main loop). */
static uint16_t s_ids[TUTORIAL_MAX_MSGS];
static int      s_count       = 0;
static int      s_idx         = 0;
static bool     s_open        = false;
static bool     s_pendingOpen = false;
static bool     s_focusFirst  = false;
static void   (*s_onComplete)(void) = nullptr;

void tutorialOverlayShow(const uint16_t *ids, int count,
                         void (*onComplete)(void)) {
    if (s_open || s_pendingOpen) return;   /* already showing — ignore */
    if (!ids || count <= 0) return;
    if (count > TUTORIAL_MAX_MSGS) count = TUTORIAL_MAX_MSGS;
    for (int i = 0; i < count; ++i) s_ids[i] = ids[i];
    s_count       = count;
    s_idx         = 0;
    s_onComplete  = onComplete;
    s_pendingOpen = true;
}

bool tutorialOverlayIsOpen(void) {
    return s_open || s_pendingOpen;
}

void tutorialOverlayRender(struct ClientSim *cs) {
    (void)cs;
    if (s_pendingOpen) {
        ImGui::OpenPopup(DIALOG_BOX_TITLE "###TutorialMsg");
        ImGui::SetNavCursorVisible(true);
        s_pendingOpen = false;
        s_open        = true;
        s_focusFirst  = true;
    }
    if (!s_open) return;

    /* Resolve the current message's segments.  The producer parks TEXT runs
       in a static buffer reused on the next call, so resolve-then-render in
       one shot and never re-resolve before drawing. */
    TutorialSeg segs[TUTORIAL_SEG_MAX];
    int nSegs = (s_idx < s_count)
                    ? tutorialResolveSegments(s_ids[s_idx], segs, TUTORIAL_SEG_MAX)
                    : 0;

    /* The body reflows now (single '\n' is a space, blank lines are paragraph
       breaks), so width is a free readability choice rather than something we
       have to fit to a pre-wrapped line.  ~two-thirds of the viewport, clamped
       for readability on big screens and to 92% on small windows so it never
       overflows. */
    float uiScale = sdl3ImguiGetUiScale();
    if (uiScale <= 0.0f) uiScale = 1.0f;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float dialogW = vp->Size.x * 0.66f;
    float minW = 460.0f * uiScale;
    float maxW = 820.0f * uiScale;
    if (dialogW < minW) dialogW = minW;
    if (dialogW > maxW) dialogW = maxW;
    if (dialogW > vp->Size.x * 0.92f) dialogW = vp->Size.x * 0.92f;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(dialogW, 0),
                                        ImVec2(dialogW, FLT_MAX));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::BeginPopupModal(DIALOG_BOX_TITLE "###TutorialMsg",
                               nullptr, flags)) {
        bool dismissed = imguiRichSegmentsBody(segs, nSegs, &s_focusFirst);
        if (dismissed) {
            s_idx++;
            if (s_idx >= s_count) {
                /* Last message in the sequence dismissed — close, clear
                   state, then notify.  Run the callback after EndPopup so
                   it sees the overlay already closed. */
                ImGui::CloseCurrentPopup();
                s_open = false;
                void (*cb)(void) = s_onComplete;
                s_onComplete = nullptr;
                s_count = 0;
                s_idx   = 0;
                ImGui::EndPopup();
                if (cb) cb();
                return;
            }
            /* Advance to the next message; refocus OK for it. */
            s_focusFirst = true;
        }
        ImGui::EndPopup();
    } else {
        s_open = false;
    }
}
