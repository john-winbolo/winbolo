/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_deck_pause.cpp
 * Purpose:       Steam Deck pause overlay. A controller-driven
 *                modal popup that mirrors the desktop menu bar
 *                (which is hidden on Deck) — Resume, Settings,
 *                Configure Keys, Players, Send Message, Leave
 *                Game, Quit.  Drawn through the main ImGui
 *                context so it picks up the global gamepad-nav
 *                config (D-pad / stick + A/B).
 *********************************************************/

#include "imgui.h"
#include "imgui_deck_pause.h"
#include "../../lang.h"
#include "../sdl3imgui.h"

extern "C" void windowSetQuitting(void);
extern "C" void windowNewGame(void);

static bool s_open        = false;
static bool s_pendingOpen = false;

void deckPauseOpen(void) {
    s_pendingOpen = true;
}

bool deckPauseIsOpen(void) {
    return s_open;
}

void deckPauseRender(struct ClientSim *cs) {
    (void)cs;

    if (s_pendingOpen) {
        ImGui::OpenPopup("Pause");
        s_pendingOpen = false;
        s_open        = true;
    }
    if (!s_open) return;

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::BeginPopupModal("Pause", nullptr, flags)) {
        const ImVec2 btnSize(320.0f, 0.0f);
        ImGuiStyle &style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(style.FramePadding.x, 8.0f));

        if (ImGui::Button("Resume", btnSize)) {
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        if (ImGui::Button(langGetText(STR_MENU_SETTINGS), btnSize)) {
            sdl3ImguiShowSettings();
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        if (ImGui::Button(langGetText(STR_MENU_SETKEYS), btnSize)) {
            sdl3ImguiShowKeySetup();
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        if (ImGui::Button(langGetText(STR_MENU_PLAYERS), btnSize)) {
            sdl3ImguiShowPlayersPanel(true);
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        if (ImGui::Button(langGetText(STR_MENU_SEND_MESSAGE), btnSize)) {
            sdl3ImguiShowSendMsg(true);
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        if (ImGui::Button(langGetText(STR_MENU_LEAVE_GAME), btnSize)) {
            windowNewGame();
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        if (ImGui::Button(langGetText(STR_MENU_EXIT), btnSize)) {
            windowSetQuitting();
            ImGui::CloseCurrentPopup();
            s_open = false;
        }

        ImGui::PopStyleVar();
        ImGui::EndPopup();
    } else {
        /* Popup closed externally (B button / Escape). */
        s_open = false;
    }
}
