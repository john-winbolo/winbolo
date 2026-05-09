/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_controller_prompt.cpp
 * Purpose:       Controller-detected prompt (Phase 8.1).
 *                Modal popup that surfaces when a gamepad
 *                connects on a non-Deck desktop and the
 *                player has not opted out.  Three choices:
 *                  Yes              → controllerMode = ON,
 *                                     persisted to prefs
 *                  Not now          → close, leave pref
 *                                     unchanged (we'll
 *                                     ask again next time)
 *                  Don't ask again  → clear the prompt-on-
 *                                     connect flag, persist
 *********************************************************/

#include "imgui.h"
#include "imgui_controller_prompt.h"

extern "C" {
#include "../../ui_mode.h"
#include "../../gamefront.h"
}

static bool s_open        = false;
static bool s_pendingOpen = false;

void controllerPromptOpen(void) {
    s_pendingOpen = true;
}

bool controllerPromptIsOpen(void) {
    return s_open;
}

void controllerPromptRender(void) {
    if (s_pendingOpen) {
        ImGui::OpenPopup("Controller detected");
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

    if (ImGui::BeginPopupModal("Controller detected", &s_open, flags)) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
            s_open = false;
            ImGui::EndPopup();
            return;
        }

        ImGui::TextUnformatted("A gamepad has been connected.");
        ImGui::TextUnformatted("Switch to Controller Mode?");
        ImGui::Spacing();
        ImGui::TextDisabled("(Hides the menu bar; Start opens a controller-friendly pause menu.\n"
                            "Change later in Settings.)");
        ImGui::Spacing();

        const ImVec2 btnSize(140.0f, 0.0f);
        ImGuiStyle &style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(style.FramePadding.x, 8.0f));

        if (ImGui::Button("Yes", btnSize)) {
            uiControllerModeSet(CONTROLLER_MODE_ON);
            gameFrontSaveCurrentPrefs();
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Not now", btnSize)) {
            ImGui::CloseCurrentPopup();
            s_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't ask again", btnSize)) {
            uiControllerPromptAskOnConnectSet(false);
            gameFrontSaveCurrentPrefs();
            ImGui::CloseCurrentPopup();
            s_open = false;
        }

        ImGui::PopStyleVar();
        ImGui::EndPopup();
    } else {
        s_open = false;
    }
}
