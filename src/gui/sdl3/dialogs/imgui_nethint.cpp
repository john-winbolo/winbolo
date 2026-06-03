/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          imgui_nethint.cpp
 * Purpose:       First-time net-play setup hint popup.
 *                See imgui_nethint.h. Rendered inside the
 *                host window (game browser); reuses the
 *                in-game key-setup popup for "Set Keys".
 *********************************************************/

#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "dialog_footer.h"
#include "imgui_keysetup.h"

extern "C" {
#include "global.h"
#include "../../gamefront.h"
#include "../../lang.h"
#include "imgui_nethint.h"
}

static bool s_openRequested = false;
static char s_name[PLAYER_NAME_LEN] = "";
static bool s_wbnLocked = false;

void imguiNetHintOpen(void) {
    s_openRequested = true;
}

void imguiNetHintRenderPopup(void) {
    char title[128];
    snprintf(title, sizeof(title), "%s###nethint", langGetText(STR_DLGNETHINT_TITLE));

    if (s_openRequested) {
        ImGui::OpenPopup(title);
        s_openRequested = false;
        s_name[0] = '\0';
        gameFrontGetPlayerName(s_name);
        /* WinBolo.net owns the name when signed in. */
        s_wbnLocked = gameFrontGetWinbolonetUse();
    }

    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);

    bool open = true;
    if (ImGui::BeginPopupModal(title, &open,
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove)) {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(langGetText(STR_DLGNETHINT_MSG));
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Player name. */
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_PLAYERNAME));
        ImGui::SameLine(130.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (s_wbnLocked) ImGui::BeginDisabled();
        ImGui::InputText("##nethintname", s_name, PLAYER_NAME_LEN);
        if (s_wbnLocked) ImGui::EndDisabled();
        if (s_wbnLocked) {
            ImGui::TextDisabled("%s", langGetText(STR_DLGSETNAME_WBN_LOCKED));
        }

        ImGui::Spacing();

        /* Set Keys — opens the in-game key-setup popup INSIDE this window
         * (rendered nested below so it stacks over this hint). */
        if (ImGui::Button(langGetText(STR_MENU_SETKEYS))) {
            imguiKeySetupOpenInGame();
        }

        /* Footer: lone primary to proceed. Esc / close also proceed. */
        int f = WBUI::DialogFooter(nullptr, langGetText(STR_OK));
        if (f != WBUI::FOOTER_NONE || !open) {
            if (!s_wbnLocked && s_name[0] != '\0') {
                gameFrontSetPlayerName(s_name);
                gameFrontPersistPlayerName();
            }
            ImGui::CloseCurrentPopup();
        }

        /* Nested in-game key-setup popup (no ClientSim pre-game → NULL). */
        imguiKeySetupRenderInGamePopup(NULL);

        ImGui::EndPopup();
    }
}
