/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_tutorial_overlay.cpp
 * Purpose:       See imgui_tutorial_overlay.h.  An in-loop
 *                modal popup drawn through the main ImGui
 *                context (like imgui_deck_pause), replacing the
 *                old blocking imguiMessageBoxRich sequence so
 *                the game's input gate and solo-pause path apply
 *                while a scenario's popup is up.
 *********************************************************/

#include <cfloat>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"  /* ClosePopupToLevel */
#include "imgui_tutorial_overlay.h"
#include "imgui_messagebox.h"
#include "../sdl3imgui.h"    /* sdl3ImguiGetUiScale */
#include "../tutorial_text.h"
#include "../../winbolo.h"   /* DIALOG_BOX_TITLE */

extern "C" {
#include "../input.h"        /* inputSwallowKeyUntilRelease */
}

/* All state is touched only from the main thread, by the render pass. */
static bool     s_open        = false;
static bool     s_pendingOpen = false;
static bool     s_focusFirst  = false;
/* The message up. Sized past SCN_POPUP_TEXT_MAX with room. */
static char     s_text[1024];
/* Set by tutorialOverlayClose while the ImGui popup is open: the next render
   takes it off ImGui's popup stack before anything else. */
static bool     s_closePopup  = false;

#define TUTORIAL_POPUP_ID DIALOG_BOX_TITLE "###TutorialMsg"

void tutorialOverlayShowText(const char *text) {
    if (s_open || s_pendingOpen) return;   /* already showing — ignore */
    if (!text || !*text) return;
    size_t len = strlen(text);
    if (len >= sizeof(s_text)) len = sizeof(s_text) - 1;
    memcpy(s_text, text, len);
    s_text[len]   = '\0';
    s_pendingOpen = true;
}

bool tutorialOverlayIsOpen(void) {
    return s_open || s_pendingOpen;
}

/* Clears the per-run state. */
void tutorialOverlayReset(void) {
    s_open        = false;
    s_pendingOpen = false;
    s_focusFirst  = false;
    s_text[0]     = '\0';
}

/* The round ended under the message (see the header). A popup only asked for
   (s_pendingOpen) never reached ImGui, so only an open one needs closing. */
void tutorialOverlayClose(void) {
    if (s_open) s_closePopup = true;
    tutorialOverlayReset();
}

void tutorialOverlayRender(struct ClientSim *cs) {
    (void)cs;
    if (s_closePopup) {
        /* Close it here, inside a frame and with the same ID stack the
           OpenPopup below used, rather than letting BeginPopupModal draw it
           once more. ClosePopupToLevel also drops any popup opened over it. */
        s_closePopup = false;
        ImGuiContext &g = *GImGui;
        ImGuiID id = ImGui::GetID(TUTORIAL_POPUP_ID);
        for (int i = 0; i < g.OpenPopupStack.Size; ++i) {
            if (g.OpenPopupStack[i].PopupId == id) {
                ImGui::ClosePopupToLevel(i, true);
                break;
            }
        }
    }
    if (s_pendingOpen) {
        ImGui::OpenPopup(TUTORIAL_POPUP_ID);
        ImGui::SetNavCursorVisible(true);
        s_pendingOpen = false;
        s_open        = true;
        s_focusFirst  = true;
    }
    if (!s_open) return;

    /* Resolve the message's segments.  The producer parks TEXT runs in a
       static buffer reused on the next call, so resolve-then-render in one
       shot and never re-resolve before drawing. */
    TutorialSeg segs[TUTORIAL_SEG_MAX];
    int nSegs = tutorialResolveText(s_text, segs, TUTORIAL_SEG_MAX);

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
    /* Centred when it opens, and then left where the player drags it by its
       title bar, so it can be moved off the part of the map it covers. Each
       message is a new popup (a message is shown only after the one before
       it is closed), so every message opens centred again. The width is
       still set every frame, so a window resize changes it as before; ImGui
       keeps a corner of a moved window inside the main window. */
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(dialogW, 0),
                                        ImVec2(dialogW, FLT_MAX));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::BeginPopupModal(TUTORIAL_POPUP_ID,
                               nullptr, flags)) {
        bool dismissed = imguiRichSegmentsBody(segs, nSegs, &s_focusFirst);
        if (dismissed) {
            /* The OK button is pressed by Enter or Space going down, and the
               game reads its keys by polling the keyboard, so the key that
               closed the message was still down on the next game tick: Space
               fired and Enter shortened the gunsight. The game reads these
               as up until the player lets go. */
            const bool *held = SDL_GetKeyboardState(NULL);
            if (held) {
                static const SDL_Scancode kCloseKeys[] = {
                    SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER,
                    SDL_SCANCODE_SPACE,
                };
                for (SDL_Scancode sc : kCloseKeys) {
                    if (held[sc]) inputSwallowKeyUntilRelease((int)sc);
                }
            }
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        ImGui::EndPopup();
    } else {
        s_open = false;
    }
}
