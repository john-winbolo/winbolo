/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * players_panel.cpp - Floating players panel for mobile (Android/iOS)
 *
 * Renders an ImGui window showing all players with alliance checkboxes.
 * Toggled by a button in the touch overlay.
 *
 * Updated to use the new ClientSim API (*CS functions).
 */

#include <cstring>
#include <cstdio>

#include "imgui.h"
#include "../sdl3/dialogs/imgui_dialog_utils.h"

extern "C" {
#include "players_panel.h"
#include "client_sim.h"
#include "../sdl3/sdl3imgui.h"
#include "../lang.h"
}

#define MAX_PLAYERS 16
#define PLAYER_NAME_LEN 33

static bool sOpen = false;

static bool sPlayerEnabled[MAX_PLAYERS];
static char sPlayerName[MAX_PLAYERS][PLAYER_NAME_LEN];
static bool sPlayerChecked[MAX_PLAYERS];

/* ClientSim pointer — set by the frontend (gamefront_ios.c exports humanSim) */
extern "C" ClientSim *humanSim;

extern "C" void playersPanelToggle(void) {
    sOpen = !sOpen;
}

extern "C" bool playersPanelIsOpen(void) {
    return sOpen;
}

extern "C" void playersPanelSetPlayer(unsigned char playerNum, const char *name) {
    if (playerNum >= MAX_PLAYERS) return;
    sPlayerEnabled[playerNum] = true;
    if (name) {
        strncpy(sPlayerName[playerNum], name, PLAYER_NAME_LEN - 1);
        sPlayerName[playerNum][PLAYER_NAME_LEN - 1] = '\0';
    } else {
        sPlayerName[playerNum][0] = '\0';
    }
}

extern "C" void playersPanelClearPlayer(unsigned char playerNum) {
    if (playerNum >= MAX_PLAYERS) return;
    sPlayerEnabled[playerNum] = false;
    sPlayerName[playerNum][0] = '\0';
    sPlayerChecked[playerNum] = false;
}

extern "C" void playersPanelSetCheckState(unsigned char playerNum, bool isChecked) {
    if (playerNum >= MAX_PLAYERS) return;
    sPlayerChecked[playerNum] = isChecked;
}

extern "C" void playersPanelRender(void) {
    if (!sOpen) return;
    if (!humanSim) return;

    ImGuiIO &io = ImGui::GetIO();
    float panelW = 260.0f;
    float panelH = 380.0f;

    float posX = io.DisplaySize.x - panelW - 10.0f;
    float posY = 10.0f;

    ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);

    if (!ImGui::Begin(langGetText(STR_DLGPLAYERS_TITLE), &sOpen,
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    float btnW = 55.0f;
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_ALL), ImVec2(btnW, 0))) {
        clientSimCheckAllNonePlayers(humanSim, true);
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_NONE), ImVec2(btnW, 0))) {
        clientSimCheckAllNonePlayers(humanSim, false);
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_ALLIES), ImVec2(btnW, 0))) {
        clientSimCheckAlliedPlayers(humanSim);
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_NEARBY), ImVec2(btnW, 0))) {
        clientSimCheckNearbyPlayers(humanSim);
    }
    imguiHandOnHover();

    ImGui::Separator();

    ImGui::BeginChild("##PlayerList", ImVec2(0, 0), ImGuiChildFlags_None);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const char *label = sPlayerEnabled[i] ? sPlayerName[i] : nullptr;
        char defLabel[16];
        if (!label || label[0] == '\0') {
            snprintf(defLabel, sizeof(defLabel), "%d", i + 1);
            label = defLabel;
        }

        if (sPlayerEnabled[i]) {
            uint16_t ping  = clientSimGetPlayerPing(humanSim, (BYTE)i);
            uint8_t flags  = clientSimGetPlayerClientFlags(humanSim, (BYTE)i);
            uint8_t ctype  = clientSimGetPlayerClientType(humanSim, (BYTE)i);

            renderPlayerName(NULL, flags, ctype, "", false);

            char checkId[48];
            snprintf(checkId, sizeof(checkId), "%s##p%d", label, i);
            bool checked = sPlayerChecked[i];
            if (ImGui::Checkbox(checkId, &checked)) {
                clientSimTogglePlayerCheckState(humanSim, (BYTE)i);
            }

            if (ping > 0) {
                char pingStr[16];
                snprintf(pingStr, sizeof(pingStr), "%dms", (int)ping);
                float pingW = ImGui::CalcTextSize(pingStr).x;
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - pingW);

                ImVec4 pingColor;
                if (ping < 50)       pingColor = ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
                else if (ping < 150) pingColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
                else                 pingColor = ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, pingColor);
                ImGui::TextUnformatted(pingStr);
                ImGui::PopStyleColor();
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::TextUnformatted(label);
            ImGui::EndDisabled();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}
