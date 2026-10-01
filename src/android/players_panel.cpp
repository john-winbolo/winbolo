/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * players_panel.cpp - Floating players panel for mobile (Android/iOS)
 *
 * Renders an ImGui window showing all players with alliance checkboxes.
 * Toggled by a button in the touch overlay.
 */

#include <cstring>
#include <cstdio>

#include "imgui.h"
#include "../gui/sdl3/dialogs/imgui_dialog_utils.h"

extern "C" {
#include "players_panel.h"
#include "global.h"
#include "client_sim.h"
#include "../gui/sdl3/sdl3imgui.h"
#include "../gui/lang.h"
}

#define MAX_PLAYERS 16
/* See sdl3imgui.cpp — these rows index ClientSim state sized by MAX_TANKS. */
static_assert(MAX_PLAYERS <= MAX_TANKS, "player rows exceed ClientSim slots");

static bool sOpen = false;

static bool sPlayerEnabled[MAX_PLAYERS];
static char sPlayerName[MAX_PLAYERS][PLAYER_NAME_LEN];
static bool sPlayerChecked[MAX_PLAYERS];

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

extern "C" void playersPanelRender(ClientSim *cs) {
    if (!sOpen) return;

    ImGuiIO &io = ImGui::GetIO();
    float panelW = 260.0f;
    float panelH = 380.0f;

    /* Position in top-right area, below any status bars */
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

    /* Selection shortcuts */
    float btnW = 55.0f;
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_ALL), ImVec2(btnW, 0))) {
        clientSimCheckAllNonePlayers(cs, true);
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_NONE), ImVec2(btnW, 0))) {
        clientSimCheckAllNonePlayers(cs, false);
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_ALLIES), ImVec2(btnW, 0))) {
        clientSimCheckAlliedPlayers(cs);
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_NEARBY), ImVec2(btnW, 0))) {
        clientSimCheckNearbyPlayers(cs);
    }
    imguiHandOnHover();

    ImGui::Separator();

    /* Player list */
    ImGui::BeginChild("##PlayerList", ImVec2(0, 0), ImGuiChildFlags_None);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const char *label = sPlayerEnabled[i] ? sPlayerName[i] : nullptr;
        char defLabel[16];
        if (!label || label[0] == '\0') {
            snprintf(defLabel, sizeof(defLabel), "%d", i + 1);
            label = defLabel;
        }

        if (sPlayerEnabled[i]) {
            /* Refresh ping from engine */
            uint16_t ping  = clientSimGetPlayerPing(cs, (BYTE)i);
            uint8_t flags  = clientSimGetPlayerClientFlags(cs, (BYTE)i);
            uint8_t ctype  = clientSimGetPlayerClientType(cs, (BYTE)i);

            /* Which chip a bot gets. Without it the badge falls back to the
             * red one, so an allied bot on this list would be marked as an
             * enemy. Read from the mirror sdl3ImguiPumpAndRender keeps, the
             * same one the map's tank labels use — this list has a sim to
             * ask, but the mirror is already a frame fresh and costs no
             * call. */
            RenderPlayerNameOpts nameOpts = {
                sdl3ImguiPlayerIsAlly((unsigned char)i), false };
            renderPlayerNameEx(NULL, flags, ctype, "", false, &nameOpts);

            /* Checkbox + name */
            char checkId[48];
            snprintf(checkId, sizeof(checkId), "%s##p%d", label, i);
            bool checked = sPlayerChecked[i];
            if (ImGui::Checkbox(checkId, &checked)) {
                clientSimTogglePlayerCheckState(cs, (BYTE)i);
            }

            /* Ping on same line, right-aligned */
            if (ping > 0) {
                char pingStr[16];
                snprintf(pingStr, sizeof(pingStr), "%dms", (int)ping);
                float pingW = ImGui::CalcTextSize(pingStr).x;
                float avail = ImGui::GetContentRegionAvail().x;
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - pingW);

                ImVec4 pingColor = imguiPingBandColor(
                    clientSimGetPlayerPingBand(cs, (BYTE)i));
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
