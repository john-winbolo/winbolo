/*
 * players_panel.cpp - Floating players panel for mobile (Android/iOS)
 *
 * Renders an ImGui window showing all players with alliance checkboxes.
 * Toggled by a button in the touch overlay.
 */

#include <cstring>
#include <cstdio>

#include "imgui.h"

extern "C" {
#include "players_panel.h"
#include "../bolo/screen.h"
#include "../bolo/players.h"
#include "../bolo/client_sim.h"
#include "../gui/sdl3/sdl3imgui.h"
}

#define MAX_PLAYERS 16
#define PLAYER_NAME_LEN 33
#define WBN_ICON_SIZE 14

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

    if (!ImGui::Begin("Players", &sOpen,
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    /* Selection shortcuts */
    float btnW = 55.0f;
    if (ImGui::Button("All", ImVec2(btnW, 0))) {
        screenCheckAllNonePlayersCS(cs, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("None", ImVec2(btnW, 0))) {
        screenCheckAllNonePlayersCS(cs, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Allies", ImVec2(btnW, 0))) {
        screenCheckAlliedPlayersCS(cs);
    }
    ImGui::SameLine();
    if (ImGui::Button("Near", ImVec2(btnW, 0))) {
        screenCheckNearbyPlayersCS(cs);
    }

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
            uint16_t ping = playersGetPing(&cs->sim.plyrs, (BYTE)i);
            bool wbn = playersGetWbnParticipant(&cs->sim.plyrs, (BYTE)i);
            bool steam = playersGetSteamParticipant(&cs->sim.plyrs, (BYTE)i);

            /* WBN/Steam icons */
            if (wbn) {
                SDL_Texture *globeTex = sdl3ImguiGetGlobeIcon();
                if (globeTex) {
                    ImGui::Image((ImTextureID)globeTex, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
                    ImGui::SameLine();
                }
            }
            if (steam) {
                SDL_Texture *steamTex = sdl3ImguiGetSteamIcon();
                if (steamTex) {
                    ImGui::Image((ImTextureID)steamTex, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
                    ImGui::SameLine();
                }
            }

            /* Checkbox + name */
            char checkId[48];
            snprintf(checkId, sizeof(checkId), "%s##p%d", label, i);
            bool checked = sPlayerChecked[i];
            if (ImGui::Checkbox(checkId, &checked)) {
                screenTogglePlayerCheckStateCS(cs, (BYTE)i);
            }

            /* Ping on same line, right-aligned */
            if (ping > 0) {
                char pingStr[16];
                snprintf(pingStr, sizeof(pingStr), "%dms", (int)ping);
                float pingW = ImGui::CalcTextSize(pingStr).x;
                float avail = ImGui::GetContentRegionAvail().x;
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
