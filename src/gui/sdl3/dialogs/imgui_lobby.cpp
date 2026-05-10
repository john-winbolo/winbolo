/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          imgui_lobby.cpp
 * Purpose:       ImGui Lobby dialog.
 *                Blocking modal loop that shows the lobby
 *                while waiting for the game to start.
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../gamefront.h"
#include "../../../bolo/global.h"
#include "../../../bolo/client_sim.h"
#include "../../../bolo/transport.h"
#include "../../../bolo/transport_udp.h"
#include "../../../server/server_lifecycle.h"
#include "../../../bolo/bolo_map.h"
#include "../../../bolo/pillbox.h"
#include "../../../bolo/bases.h"
#include "../../../bolo/starts.h"
#include "../../../bolo/platform_net.h"
#include "../../../bolo/lobby_bot_pools.h"
#include "../flags.h"
#include "../sdl3imgui.h"
#include "../minimap_render.h"
#include "../map_preview_popup.h"
#include "../../lang.h"
#include "imgui_lobby.h"
#include "imgui_messagebox.h"
}
#include "../wb_theme.h"

#define MAX_TANKS 16
#define WBN_ICON_SIZE 14
#define CHAT_INPUT_SIZE 129  /* 128 chars + null terminator */
#define MAP_PREVIEW_SIZE 256

static const int DIALOG_W = 900;
static const int DIALOG_H = 700;

/* Bounding box of interesting (non-sea) terrain in the map preview */
struct MapBounds {
    int minX, minY, maxX, maxY;
};

/* Compressed map data — stashed when map download completes so the popup
 * can decompress on demand (transportUdpClientGetMapData() is only called
 * in the one-shot preview-build block; the pointer may not remain valid). */
static BYTE        *popupCompressedData = NULL;
static int          popupCompressedLen  = 0;

/* Build a 256x256 RGBA minimap from compressed map data.
 * Returns an SDL_Texture* or NULL on failure.
 * bounds is filled with the bounding box of non-sea terrain. */
static SDL_Texture *buildMapPreview(SDL_Renderer *renderer,
                                     const BYTE *compressedData, int dataLen,
                                     MapBounds *bounds) {
    MinimapBounds mb;
    SDL_Texture *tex = minimapFromCompressed(renderer, compressedData, dataLen,
                                             &mb, NULL, NULL, NULL);
    if (bounds) {
        bounds->minX = mb.minX;
        bounds->minY = mb.minY;
        bounds->maxX = mb.maxX;
        bounds->maxY = mb.maxY;
    }
    return tex;
}

static const char *gameTypeStr(gameType gt) {
    switch (gt) {
        case gameOpen:             return langGetText(STR_DLGGAMEINFO_OPEN);
        case gameTournament:       return langGetText(STR_DLGGAMEINFO_TOURN);
        case gameStrictTournament: return langGetText(STR_DLGGAMEINFO_STRICT);
        default:                   return langGetText(STR_UNKNOWN);
    }
}

static const char *aiTypeStr(uint8_t ai) {
    switch (ai) {
        case 0:  return langGetText(STR_NO);
        case 1:  return langGetText(STR_YES);
        case 2:  return langGetText(STR_DLGGAMEINFO_AIADV);
        case 3:  return langGetText(STR_DLGGAMEINFO_AIFULL);
        default: return langGetText(STR_UNKNOWN);
    }
}

static void formatTimeLimit(int32_t ticks, char *buf, int bufSize) {
    if (ticks <= 0) {
        SDL_snprintf(buf, bufSize, "%s", langGetText(STR_DLGGAMEINFO_UNLIMITED));
        return;
    }
    int totalSecs = ticks / 50;
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;
    if (hours > 0) {
        MessageArgs args = {};
        args.number = hours;
        SDL_snprintf(args.string1, sizeof(args.string1), "%02d", mins);
        SDL_snprintf(args.string2, sizeof(args.string2), "%02d", secs);
        SDL_snprintf(buf, bufSize, "%s", langGetTextFmt(STR_DLGLOBBY_TIME_HMS, &args));
    } else if (mins > 0) {
        MessageArgs args = {};
        args.number = mins;
        SDL_snprintf(args.string1, sizeof(args.string1), "%02d", secs);
        SDL_snprintf(buf, bufSize, "%s", langGetTextFmt(STR_DLGLOBBY_TIME_MS, &args));
    } else {
        MessageArgs args = {};
        args.number = secs;
        SDL_snprintf(buf, bufSize, "%s", langGetTextFmt(STR_DLGLOBBY_TIME_S, &args));
    }
}

static SDL_Texture *s_iconSuccess = nullptr;
static SDL_Texture *s_iconError   = nullptr;
static SDL_Texture *s_iconInfo    = nullptr;
static bool         s_iconsAttempted = false;

static void loadStatusIconsOnce(SDL_Renderer *renderer, float scale) {
    if (s_iconsAttempted) return;
    s_iconsAttempted = true;

    int iconPx = (int)(18.0f * scale);
    if (iconPx < 16) iconPx = 16;

    struct {
        SDL_Texture **target;
        const char   *relPath;
    } icons[] = {
        { &s_iconSuccess, "data/ui/dialog-success.svg" },
        { &s_iconError,   "data/ui/dialog-error.svg" },
        { &s_iconInfo,    "data/ui/dialog-info.svg" },
    };

    for (int i = 0; i < 3; i++) {
        *icons[i].target = imguiLoadSvgIcon(renderer, icons[i].relPath, iconPx);
        if (*icons[i].target == nullptr) {
            char basePathBuf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(basePathBuf, sizeof(basePathBuf),
                             "%s%s", base, icons[i].relPath);
                *icons[i].target = imguiLoadSvgIcon(renderer, basePathBuf, iconPx);
            }
        }
    }
}

/* Currently-expanded bot slot for the AiConfig sub-row, or -1. */
static int s_expandedBotSlot = -1;

/* Forward decl — defined below the team renderer. */
static void renderBotAiConfig(ClientSim *cs, Transport *transport,
                              int slot, int teamId, float s);

/* ── Layout A — team-grouped player list ──────────────────────────
 * Renders players grouped under team headers with color tints from
 * WbTheme. Replaces the flat 5-column table with the mockup's
 * "team containers" model. Sized to fit inside the calling child
 * window. Returns nothing — purely UI. */
static void renderTeamGroupedPlayers(ClientSim *cs, Transport *transport,
                                     int myPlayerNum, float s, bool isHost) {
    /* Helper to count members per team for header strings. */
    int memberCount[16] = {0};
    int botCount[16]    = {0};
    int unassignedCount = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!cs->lobbySlots[i].connected) continue;
        uint8_t t = cs->lobbySlots[i].teamNumber;
        if (t == 0) {
            unassignedCount++;
        } else if (t < 16) {
            memberCount[t]++;
            if (cs->lobbySlots[i].isBot) botCount[t]++;
        }
    }

    /* Walk teams 1..15, render those with members. Then unassigned. */
    bool effectiveHost = isHost || cs->lobbyOpenHost;
    for (int teamId = 1; teamId < 16; teamId++) {
        if (memberCount[teamId] == 0 && !cs->lobbyTeamInUse[teamId]) continue;

        /* Team color from theme; falls back to gray for un-themed teams. */
        ImU32 tc;
        uint8_t colorIdx = cs->lobbyTeamColor[teamId];
        if (cs->lobbyTeamInUse[teamId] && colorIdx < 8) {
            tc = g_theme->teamColors[colorIdx];
        } else {
            /* Default per-team color: cycle through palette by teamId. */
            tc = g_theme->teamColors[(teamId - 1) & 7];
        }

        /* Header row — color swatch + name + count + actions. */
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
            ImGui::ColorConvertU32ToFloat4((tc & 0x00FFFFFF) | (0x18 << 24)));
        char teamFrame[32];
        SDL_snprintf(teamFrame, sizeof(teamFrame), "##team%d", teamId);
        ImGui::BeginChild(teamFrame,
                          ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::PopStyleColor();

        /* Color swatch — small filled rect at the head of the row. */
        ImVec2 swatchPos = ImGui::GetCursorScreenPos();
        float  swatchSz  = 12.0f * s;
        ImGui::GetWindowDrawList()->AddRectFilled(
            swatchPos,
            ImVec2(swatchPos.x + swatchSz, swatchPos.y + swatchSz),
            tc, 2.0f);
        ImGui::Dummy(ImVec2(swatchSz + 6.0f * s, swatchSz));
        ImGui::SameLine();

        /* Team name — defaults to "Team N" when not customised. */
        char defaultName[16];
        SDL_snprintf(defaultName, sizeof(defaultName), "Team %d", teamId);
        const char *displayName = cs->lobbyTeamInUse[teamId] && cs->lobbyTeamName[teamId][0]
            ? cs->lobbyTeamName[teamId] : defaultName;

        /* Inline rename — host only. */
        if (effectiveHost && transport) {
            char editId[16];
            SDL_snprintf(editId, sizeof(editId), "##rn%d", teamId);
            char nameBuf[32];
            strncpy(nameBuf, displayName, sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tc));
            ImGui::SetNextItemWidth(160.0f * s);
            if (ImGui::InputText(editId, nameBuf, sizeof(nameBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
                transportUdpClientSendLobbyTeamMeta(transport, (uint8_t)teamId,
                    cs->lobbyTeamColor[teamId], cs->lobbyTeamPool[teamId], nameBuf);
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tc));
            ImGui::Text("%s", displayName);
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("· %d player%s%s%s",
            memberCount[teamId], memberCount[teamId] == 1 ? "" : "s",
            botCount[teamId] > 0 ? " · " : "",
            botCount[teamId] > 0 ? (botCount[teamId] == 1 ? "1 bot" : "bots") : "");

        /* Per-team Bot naming pool dropdown — only shown when team has bots. */
        if (botCount[teamId] > 0 && effectiveHost && transport) {
            ImGui::SameLine(0.0f, 16.0f * s);
            ImGui::TextDisabled("Bot naming:");
            ImGui::SameLine();
            int curPool = cs->lobbyTeamPool[teamId];
            if (curPool < 0 || curPool >= lobbyBotPoolCount()) curPool = 0;
            char poolId[16];
            SDL_snprintf(poolId, sizeof(poolId), "##pool%d", teamId);
            ImGui::SetNextItemWidth(160.0f * s);
            if (ImGui::BeginCombo(poolId, lobbyBotPoolLabel(curPool))) {
                for (int p = 0; p < lobbyBotPoolCount(); p++) {
                    bool sel = (p == curPool);
                    if (ImGui::Selectable(lobbyBotPoolLabel(p), sel)) {
                        transportUdpClientSendLobbyTeamMeta(transport, (uint8_t)teamId,
                            cs->lobbyTeamColor[teamId], (uint8_t)p,
                            cs->lobbyTeamInUse[teamId] ? cs->lobbyTeamName[teamId] : defaultName);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Separator();

        /* Member rows. */
        for (int i = 0; i < MAX_TANKS; i++) {
            if (!cs->lobbySlots[i].connected) continue;
            if (cs->lobbySlots[i].teamNumber != teamId) continue;

            bool isMe = (i == myPlayerNum);
            bool isBot = cs->lobbySlots[i].isBot;

            /* Country flag (humans only). */
            if (!isBot && cs->lobbySlots[i].countryCode[0] != '\0') {
                SDL_Texture *flagTex = flagsGetTexture(cs->lobbySlots[i].countryCode);
                if (flagTex) {
                    ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                    ImGui::SameLine();
                }
            }

            /* Name with appropriate tinting. Bots are clickable to
             * expand the AiConfig sub-row below; click again to collapse. */
            if (isBot) {
                char botLabel[80];
                SDL_snprintf(botLabel, sizeof(botLabel),
                             "%s [bot] %s##botclick%d",
                             s_expandedBotSlot == i ? "v" : ">",
                             cs->lobbySlots[i].playerName, i);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      wbThemeColor(g_theme->botBadge));
                if (ImGui::SmallButton(botLabel)) {
                    s_expandedBotSlot = (s_expandedBotSlot == i) ? -1 : i;
                }
                ImGui::PopStyleColor();
            } else if (isMe) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s (you)",
                    cs->lobbySlots[i].playerName);
            } else {
                ImGui::Text("%s", cs->lobbySlots[i].playerName);
            }

            /* Ping (humans). */
            if (!isBot && cs->lobbySlots[i].pingMs > 0) {
                ImGui::SameLine(0.0f, 12.0f * s);
                ImVec4 pingColor;
                if (cs->lobbySlots[i].pingMs < 50)        pingColor = wbThemeColor(g_theme->statusOnline);
                else if (cs->lobbySlots[i].pingMs < 150)  pingColor = wbThemeColor(g_theme->statusHighPing);
                else                                       pingColor = wbThemeColor(g_theme->statusDisconnected);
                ImGui::TextColored(pingColor, "%dms", (int)cs->lobbySlots[i].pingMs);
            }

            /* Ready status. */
            ImGui::SameLine(0.0f, 12.0f * s);
            if (cs->lobbySlots[i].ready) {
                ImGui::TextColored(wbThemeColor(g_theme->statusReady), "[ready]");
            } else {
                ImGui::TextDisabled("[not ready]");
            }

            /* Bot remove button — host only. */
            if (isBot && effectiveHost && transport) {
                ImGui::SameLine();
                char btnId[24];
                SDL_snprintf(btnId, sizeof(btnId), "x##rb%d", i);
                if (ImGui::SmallButton(btnId)) {
                    transportUdpClientSendRemoveBot(transport, (uint8_t)i);
                    if (s_expandedBotSlot == i) s_expandedBotSlot = -1;
                }
            }

            /* Expanded AiConfig sub-row for the clicked bot. */
            if (isBot && s_expandedBotSlot == i && effectiveHost) {
                renderBotAiConfig(cs, transport, i, teamId, s);
            }
        }

        /* "+ Bot" button at the bottom of each team — host only.
         * Drives the existing PACKET_LOBBY_ADD_BOT for now; future
         * work extends with explicit teamId + name + config. */
        if (effectiveHost && transport && cs->lobbyAiType != 0) {
            ImGui::Spacing();
            char addId[24];
            SDL_snprintf(addId, sizeof(addId), "+ Bot##ab%d", teamId);
            if (ImGui::SmallButton(addId)) {
                transportUdpClientSendAddBot(transport);
                /* Note: server picks slot + name. Subsequent commits
                 * will switch to a teamId-aware add-bot flow. */
            }
        }

        ImGui::EndChild();
        ImGui::Spacing();
    }

    /* Unassigned tray. */
    if (unassignedCount > 0) {
        ImGui::TextDisabled("Unassigned (%d):", unassignedCount);
        for (int i = 0; i < MAX_TANKS; i++) {
            if (!cs->lobbySlots[i].connected) continue;
            if (cs->lobbySlots[i].teamNumber != 0) continue;
            ImGui::Bullet();
            ImGui::Text("%s%s", cs->lobbySlots[i].playerName,
                        i == myPlayerNum ? " (you)" : "");
        }
    }

    /* "Switch my team" picker — always available so non-host players
     * can self-assign. Existing PACKET_LOBBY_TEAM_SET wire path. */
    if (transport && myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
        cs->lobbySlots[myPlayerNum].connected) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("My team:");
        ImGui::SameLine();
        int curTeam = cs->lobbySlots[myPlayerNum].teamNumber;
        const char *teamNameItems[17] = { "Unassigned" };
        char teamNameStorage[16][16];
        for (int t = 1; t < 16; t++) {
            const char *labelSrc = (cs->lobbyTeamInUse[t] && cs->lobbyTeamName[t][0])
                ? cs->lobbyTeamName[t] : nullptr;
            if (labelSrc) {
                strncpy(teamNameStorage[t], labelSrc, sizeof(teamNameStorage[t]) - 1);
                teamNameStorage[t][sizeof(teamNameStorage[t]) - 1] = '\0';
            } else {
                SDL_snprintf(teamNameStorage[t], sizeof(teamNameStorage[t]),
                             "Team %d", t);
            }
            teamNameItems[t] = teamNameStorage[t];
        }
        ImGui::SetNextItemWidth(140.0f * s);
        if (ImGui::Combo("##myteam", &curTeam, teamNameItems, 16)) {
            transportUdpClientSendTeamSet(transport, (uint8_t)curTeam);
        }
    }
}

/* ── Layout A — bot AiConfig sub-row ──────────────────────────────
 * Inline panel under an expanded bot row showing the name override
 * with dice-reroll, difficulty dropdown, personality dropdown, and
 * close button. Edits dispatch as PACKET_LOBBY_BOT_CONFIG. */
static void renderBotAiConfig(ClientSim *cs, Transport *transport,
                              int slot, int teamId, float s) {
    if (slot < 0 || slot >= MAX_TANKS) return;

    /* Build the "currently used names" array for the dice reroll —
     * collect every bot name in the lobby so we don't collide. */
    const char *usedNames[MAX_TANKS];
    int usedCount = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (cs->lobbySlots[i].connected && cs->lobbySlots[i].isBot &&
            cs->lobbySlots[i].playerName[0]) {
            usedNames[usedCount++] = cs->lobbySlots[i].playerName;
        }
    }

    ImGui::Indent(20.0f * s);
    ImGui::PushID(slot);

    /* ── Name + dice reroll ───────────────────────────────────── */
    ImGui::TextDisabled("Name (override)");
    char nameBuf[32];
    strncpy(nameBuf, cs->lobbySlots[slot].playerName, sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    ImGui::SetNextItemWidth(180.0f * s);
    bool nameChanged = ImGui::InputText("##botname", nameBuf, sizeof(nameBuf),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool diceClicked = ImGui::SmallButton("Reroll");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pick a fresh random name from the team's pool.");
    }

    if (diceClicked && transport) {
        /* Pick a name from this team's pool, excluding all currently-used
         * bot names (incl. this one — we want a NEW name, not the same). */
        int pool = (teamId > 0 && teamId < 16) ? cs->lobbyTeamPool[teamId] : 0;
        char pickBuf[32];
        lobbyBotPoolPick(pool, usedNames, usedCount, pickBuf, sizeof(pickBuf));
        transportUdpClientSendLobbyBotConfig(transport, (uint8_t)slot,
            cs->lobbyBotDifficulty[slot], cs->lobbyBotPersonality[slot], pickBuf);
    }
    if (nameChanged && transport) {
        transportUdpClientSendLobbyBotConfig(transport, (uint8_t)slot,
            cs->lobbyBotDifficulty[slot], cs->lobbyBotPersonality[slot], nameBuf);
    }

    /* ── Difficulty dropdown ──────────────────────────────────── */
    ImGui::SameLine(0.0f, 16.0f * s);
    ImGui::TextDisabled("Difficulty");
    ImGui::SameLine();
    const char *diffItems[] = { "Easy", "Normal", "Hard" };
    int diff = cs->lobbyBotDifficulty[slot];
    if (diff < 0 || diff > 2) diff = 1;
    ImGui::SetNextItemWidth(90.0f * s);
    if (ImGui::Combo("##diff", &diff, diffItems, 3) && transport) {
        transportUdpClientSendLobbyBotConfig(transport, (uint8_t)slot,
            (uint8_t)diff, cs->lobbyBotPersonality[slot],
            cs->lobbySlots[slot].playerName);
    }

    /* ── Personality dropdown ─────────────────────────────────── */
    ImGui::SameLine(0.0f, 16.0f * s);
    ImGui::TextDisabled("Personality");
    ImGui::SameLine();
    const char *persItems[] = { "Normal", "Aggressive", "Defensive", "Sniper" };
    int pers = cs->lobbyBotPersonality[slot];
    if (pers < 0 || pers > 3) pers = 0;
    ImGui::SetNextItemWidth(110.0f * s);
    if (ImGui::Combo("##pers", &pers, persItems, 4) && transport) {
        transportUdpClientSendLobbyBotConfig(transport, (uint8_t)slot,
            cs->lobbyBotDifficulty[slot], (uint8_t)pers,
            cs->lobbySlots[slot].playerName);
    }

    /* ── Close button ─────────────────────────────────────────── */
    ImGui::SameLine(0.0f, 16.0f * s);
    if (ImGui::SmallButton("Done")) {
        s_expandedBotSlot = -1;
    }

    ImGui::PopID();
    ImGui::Unindent(20.0f * s);
    ImGui::Spacing();
}

/* ── Layout A — small inline lock badge ───────────────────────────
 * Renders an inline orange "[locked]" pill next to a setting name
 * when the server has flagged it in serverLocks. Cosmetic + tooltip. */
static void renderLockBadge(void) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, wbThemeColor(g_theme->lockBadge));
    ImGui::Text("[locked]");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Locked by server (admin --lock-* CLI flag).\n"
                          "Cannot be changed from the lobby.");
    }
}

/* ── Layout A — editable game settings panel ──────────────────────
 * Renders the four mockup setting groups (Game Type / AI / Other /
 * Time Limit). Locked settings render disabled with a lock badge.
 * Edits dispatch as PACKET_LOBBY_SET_SETTING via the new wire
 * commands. Host-only or anyone if openHost. */
static void renderGameSettingsPanel(ClientSim *cs, Transport *transport,
                                    int myPlayerNum, float s) {
    bool effectiveHost = (myPlayerNum == 0) || cs->lobbyOpenHost;

    if (!ImGui::CollapsingHeader("Game settings",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::Spacing();
    ImGui::Columns(3, "##settingsCols", false);

    /* ── Game Type ──────────────────────────────────────────── */
    bool gtLocked = (cs->lobbyServerLocks & 0x01) != 0;  /* LOBBY_LOCK_GAME_TYPE */
    {
        ImGui::Text("Game Type");
        if (gtLocked) renderLockBadge();
        bool disable = !effectiveHost || gtLocked;
        if (disable) ImGui::BeginDisabled();
        const char *items[] = {
            "Open Game (pre-armed)",
            "Tournament (free ammo early)",
            "Strict Tournament (no free ammo)",
        };
        for (int i = 0; i < 3; i++) {
            char rid[16];
            SDL_snprintf(rid, sizeof(rid), "##gt%d", i);
            bool checked = (cs->lobbyGameType == (gameType)i);
            if (ImGui::RadioButton(rid, checked) && !checked && transport) {
                uint8_t v = (uint8_t)i;
                transportUdpClientSendLobbySetting(transport, 1 /*LST_GAME_TYPE*/,
                                                   &v, 1);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(items[i]);
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── AI Computer Players ─────────────────────────────────── */
    bool aiLocked = (cs->lobbyServerLocks & 0x02) != 0;  /* LOBBY_LOCK_AI_POLICY */
    {
        ImGui::Text("AI Computer Players");
        if (aiLocked) renderLockBadge();
        bool disable = !effectiveHost || aiLocked;
        if (disable) ImGui::BeginDisabled();
        const char *items[] = {
            "No computer tanks",
            "Allow computer tanks",
            "Allow with advantage",
            "Allow with full advantage",
        };
        for (int i = 0; i < 4; i++) {
            char rid[16];
            SDL_snprintf(rid, sizeof(rid), "##ai%d", i);
            bool checked = (cs->lobbyAiType == (uint8_t)i);
            if (ImGui::RadioButton(rid, checked) && !checked && transport) {
                uint8_t v = (uint8_t)i;
                transportUdpClientSendLobbySetting(transport, 3 /*LST_AI_POLICY*/,
                                                   &v, 1);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(items[i]);
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── Other (mines / time limit / autoLockOnGameStart) ────── */
    {
        ImGui::Text("Other");

        bool minesLocked = (cs->lobbyServerLocks & 0x04) != 0;
        bool minesV = cs->lobbyHiddenMines;
        bool minesDisabled = !effectiveHost || minesLocked;
        if (minesDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Allow Hidden Mines", &minesV) && transport) {
            uint8_t v = minesV ? 1 : 0;
            transportUdpClientSendLobbySetting(transport, 2 /*LST_HIDDEN_MINES*/,
                                               &v, 1);
        }
        if (minesDisabled) ImGui::EndDisabled();
        if (minesLocked) renderLockBadge();

        bool timeLocked = (cs->lobbyServerLocks & 0x08) != 0;
        bool timeV = cs->lobbyTimeLimit > 0;
        bool timeDisabled = !effectiveHost || timeLocked;
        if (timeDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Game time limit", &timeV) && transport) {
            uint8_t v = timeV ? 1 : 0;
            transportUdpClientSendLobbySetting(transport, 4 /*LST_TIME_LIMIT*/,
                                               &v, 1);
        }
        if (timeV) {
            int mins = cs->lobbyTimeLimit > 0
                ? (int)(cs->lobbyTimeLimit / (50 * 60))
                : 30;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f * s);
            if (ImGui::InputInt("##tmin", &mins, 1, 5,
                                ImGuiInputTextFlags_EnterReturnsTrue) && transport) {
                if (mins < 1) mins = 1;
                if (mins > 999) mins = 999;
                uint8_t v[2] = { (uint8_t)((mins >> 8) & 0xFF),
                                 (uint8_t)(mins & 0xFF) };
                transportUdpClientSendLobbySetting(transport, 5 /*LST_TIME_MINUTES*/,
                                                   v, 2);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("min");
        }
        if (timeDisabled) ImGui::EndDisabled();
        if (timeLocked) renderLockBadge();

        /* "Allow new players" — uses existing PACKET_LOCK_TOGGLE */
        bool allowJoin = transportUdpServerGetLock();
        if (effectiveHost && transport &&
            ImGui::Checkbox("Allow new players", &allowJoin)) {
            transportUdpClientSendLockToggle(transport, allowJoin);
        }

        /* "Disallow new players once started" — autoLockOnGameStart */
        bool autoLockLocked = (cs->lobbyServerLocks & 0x10) != 0;
        bool autoLockV = cs->lobbyAutoLockOnGameStart;
        bool autoLockDisabled = !effectiveHost || autoLockLocked;
        if (autoLockDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Disallow new players once game has started", &autoLockV)
            && transport) {
            uint8_t v = autoLockV ? 1 : 0;
            transportUdpClientSendLobbySetting(transport, 6 /*LST_AUTO_LOCK_ON_GAME*/,
                                               &v, 1);
        }
        if (autoLockDisabled) ImGui::EndDisabled();
        if (autoLockLocked) renderLockBadge();

        /* openHost toggle — host-only, opens lobby controls to other players. */
        if (myPlayerNum == 0 && transport) {
            bool oh = cs->lobbyOpenHost;
            if (ImGui::Checkbox("Players can change teams and bots", &oh)) {
                transportUdpClientSendLobbyOpenHost(transport, oh);
            }
        }
    }

    ImGui::Columns(1);
    ImGui::Spacing();
}

extern "C" int imguiLobbyShow(ClientSim *cs) {
    WB_LOG_INFO(WB_LOG_CAT_GUI, "[LOBBY] imguiLobbyShow called cs=%p inLobby=%d netStat=%d",
            (void*)cs, cs ? cs->inLobby : -1, cs ? (int)cs->netStat : -1);
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return 0;

    /* Save logical presentation */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    /* Get screen size and compute UI scale */
    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

#if !BOLO_MOBILE
    dialogSetWindowSize(window, DIALOG_W, DIALOG_H);
    dialogSetWindowTitle(window, langGetText(STR_DLGLOBBY_WINTITLE));
    SDL_SetWindowResizable(window, true);
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    /* Set up ImGui context */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    /* Load a large font for the countdown overlay */
    float countdownFontSize = (s <= 1.05f) ? 54.0f : 60.0f * s;
    ImFont *countdownFont = imguiLoadBoloFontSized(countdownFontSize);

    /* Chat state */
    char chatInput[CHAT_INPUT_SIZE];
    chatInput[0] = '\0';

    /* Team combo items */
    const char *teamItems[] = {
        langGetText(STR_NONE), "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "10", "11", "12", "13", "14", "15", "16"
    };

    /* Map preview texture state */
    SDL_Texture *mapPreviewTex = NULL;
    bool mapPreviewBuilt = false;
    bool prevMapDownloadComplete = cs->mapDownloadComplete;
    MapBounds mapBounds = {0, 0, MAP_PREVIEW_SIZE - 1, MAP_PREVIEW_SIZE - 1};

#if BOLO_MOBILE
    /* Tab state for mobile tabbed layout */
    int activeTab = 0;      /* 0=Players, 1=Map, 2=Chat */
    bool chatUnread = false;
    int lastChatLen = 0;
#endif

    /* Query safe area insets for notch avoidance */
    DialogSafeInsets safeInsets = dialogGetSafeInsets(window);

    int result = 0;
    bool running = true;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            }
        }

        /* Tick transport to receive lobby packets */
        Transport *transport = gameFrontGetTransport();
        if (transport) {
            transport->tick(transport->ctx);
        }

        /* Clear balance proposal when countdown starts */
        if (cs->countdownSeconds > 0 && cs->balanceProposalActive) {
            cs->balanceProposalActive = false;
            memset(cs->balanceProposal, 0, sizeof(cs->balanceProposal));
        }

        /* Check for game start */
        if (cs->netStat == netRunning) {
            result = 1;
            running = false;
            break;
        }

        /* Check for server disconnect/shutdown */
        if (transport) {
            UdpClientJoinState js = transportUdpClientGetJoinState(transport);
            if (js == UDP_CLIENT_SERVER_SHUTDOWN || js == UDP_CLIENT_ERROR) {
                imguiMessageBoxEx(DIALOG_BOX_TITLE,
                    langGetText(STR_DLGLOBBY_LOSTCONNECTION),
                    IMGUI_MSG_ERROR, IMGUI_MSG_OK);
                result = 0;
                running = false;
                break;
            }
        }

        /* Reset preview when a map change invalidates the download */
        if (!cs->mapDownloadComplete && prevMapDownloadComplete) {
            mapPreviewBuilt = false;
            if (mapPreviewTex) {
                SDL_DestroyTexture(mapPreviewTex);
                mapPreviewTex = NULL;
            }
            if (popupCompressedData) { SDL_free(popupCompressedData); popupCompressedData = NULL; popupCompressedLen = 0; }
            mapPreviewPopupClose();
        }
        prevMapDownloadComplete = cs->mapDownloadComplete;

        /* Build map preview once download completes */
        if (cs->mapDownloadComplete && !mapPreviewBuilt && transport) {
            int mapLen = 0;
            const BYTE *mapData = transportUdpClientGetMapData(transport, &mapLen);
            if (mapData && mapLen > 0) {
                mapPreviewTex = buildMapPreview(renderer, mapData, mapLen, &mapBounds);
                /* Stash for popup decompression */
                if (popupCompressedData) { SDL_free(popupCompressedData); popupCompressedData = NULL; }
                popupCompressedData = (BYTE *)SDL_malloc(mapLen);
                if (popupCompressedData) {
                    SDL_memcpy(popupCompressedData, mapData, mapLen);
                    popupCompressedLen = mapLen;
                }
            }
            mapPreviewBuilt = true;
        }

        /* Query window size */
        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        /* Render popup tiles to offscreen texture before ImGui frame */
        mapPreviewPopupRenderOffscreen(renderer, winW, winH);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        /* Full-screen host window with safe area padding */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        float padL = ImGui::GetStyle().WindowPadding.x + safeInsets.left;
        float padR = safeInsets.right;
        float padT = ImGui::GetStyle().WindowPadding.y + safeInsets.top;
        float padB = safeInsets.bottom;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padL, padT));
        ImGui::Begin("##LobbyBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        BYTE myPlayerNum = gameFrontGetPlayerNum();

        /* --- Header: Server info line --- */
        {
            char serverStr[64];
            SDL_snprintf(serverStr, sizeof(serverStr), "%s:%u",
                         inet_ntoa(cs->serverAddress), cs->serverPort);

            char timeStr[32];
            formatTimeLimit(cs->lobbyTimeLimit, timeStr, sizeof(timeStr));

#if BOLO_MOBILE
            /* Stack labels vertically on mobile so the line wraps cleanly. */
            ImGui::Text("%s %s", langGetText(STR_DLGNETINFO_SERVER), serverStr);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_GAME_LBL), gameTypeStr(cs->lobbyGameType));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MINES_LBL),
                        cs->lobbyHiddenMines ? langGetText(STR_DLGLOBBY_HIDDEN) : langGetText(STR_DLGLOBBY_VISIBLE));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), aiTypeStr(cs->lobbyAiType));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
#else
            ImGui::Text("%s %s", langGetText(STR_DLGNETINFO_SERVER), serverStr);
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_GAME_LBL), gameTypeStr(cs->lobbyGameType));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MINES_LBL),
                        cs->lobbyHiddenMines ? langGetText(STR_DLGLOBBY_HIDDEN) : langGetText(STR_DLGLOBBY_VISIBLE));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), aiTypeStr(cs->lobbyAiType));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
#endif
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Layout A — collapsible game settings panel (radios, checkboxes,
         * lock badges). Edits dispatch via PACKET_LOBBY_SET_SETTING. */
        renderGameSettingsPanel(cs, transport, myPlayerNum, s);
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Hosted-MP port-mapping status --- */
        {
            ServerPortmapInfo pm;
            serverInstanceGetPortmapInfo(&pm);

            loadStatusIconsOnce(renderer, s);

            SDL_Texture *icon       = nullptr;
            const char  *shortText  = nullptr;
            ImVec4       color;
            const char  *detailFmt  = nullptr;

            switch (pm.status) {
                case SERVER_PORTMAP_DISABLED:
                    break;
                case SERVER_PORTMAP_PENDING:
                    icon      = s_iconInfo;
                    shortText = langGetText(STR_DLGLOBBY_PORTMAP_CHECKING);
                    color     = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                    detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_PENDING);
                    break;
                case SERVER_PORTMAP_SUCCEEDED:
                    icon      = s_iconSuccess;
                    shortText = langGetText(STR_DLGLOBBY_PORTMAP_ACCESSIBLE);
                    color     = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
                    detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_SUCCEEDED);
                    break;
                case SERVER_PORTMAP_HOLE_PUNCH_OK:
                    icon      = s_iconSuccess;
                    shortText = langGetText(STR_DLGLOBBY_PORTMAP_ACCESSIBLE);
                    color     = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
                    detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_HOLE_PUNCH);
                    break;
                case SERVER_PORTMAP_SYMMETRIC_NAT:
                    icon      = s_iconError;
                    shortText = langGetText(STR_DLGLOBBY_PORTMAP_UNREACHABLE);
                    color     = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
                    detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_SYMMETRIC);
                    break;
                case SERVER_PORTMAP_FAILED:
                    icon      = s_iconError;
                    shortText = langGetText(STR_DLGLOBBY_PORTMAP_UNREACHABLE);
                    color     = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
                    detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_FAILED);
                    break;
            }

            if (shortText != nullptr) {
                ImGui::BeginGroup();
                if (icon != nullptr) {
                    float iconSize = 18.0f * s;
                    ImGui::Image((ImTextureID)icon, ImVec2(iconSize, iconSize));
                    ImGui::SameLine();
                    float textOffset = (iconSize - ImGui::GetTextLineHeight()) * 0.5f;
                    if (textOffset > 0.0f) {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffset);
                    }
                }
                ImGui::TextColored(color, "%s", shortText);
                ImGui::EndGroup();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
                char popupTitle[128];
                SDL_snprintf(popupTitle, sizeof(popupTitle), "%s##portmap_details",
                             langGetText(STR_DLGLOBBY_PORTMAP_POPUP_TITLE));

                if (ImGui::IsItemClicked()) {
                    ImGui::OpenPopup(popupTitle);
                }

                ImGui::SameLine();
                bool canTest = (pm.status != SERVER_PORTMAP_DISABLED &&
                                pm.status != SERVER_PORTMAP_PENDING);
                if (!canTest) ImGui::BeginDisabled();
                if (ImGui::SmallButton(langGetText(STR_DLGLOBBY_TEST_CONNECTIVITY))) {
                    serverInstanceTriggerManualProbe();
                }
                if (!canTest) ImGui::EndDisabled();

                ManualProbeState mps = serverInstanceGetManualProbeState();
                if (mps != MANUAL_PROBE_IDLE) {
                    ImGui::SameLine();
                    switch (mps) {
                        case MANUAL_PROBE_IN_PROGRESS:
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                               "%s", langGetText(STR_DLGLOBBY_TEST_TESTING));
                            break;
                        case MANUAL_PROBE_SUCCESS:
                            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f),
                                               "%s", langGetText(STR_DLGLOBBY_TEST_REACHABLE));
                            break;
                        case MANUAL_PROBE_TIMEOUT:
                            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f),
                                               "%s", langGetText(STR_DLGLOBBY_TEST_NO_REPLY));
                            break;
                        case MANUAL_PROBE_IDLE:
                            break;
                    }
                }
                ImGui::Spacing();

                if (ImGui::BeginPopupModal(popupTitle, nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ServerPortmapInfo pm2;
                    serverInstanceGetPortmapInfo(&pm2);
                    ImGui::PushTextWrapPos(420.0f * s);
                    switch (pm2.status) {
                        case SERVER_PORTMAP_SUCCEEDED:
                            ImGui::Text(detailFmt, pm2.externalIp,
                                        (unsigned)pm2.externalPort);
                            break;
                        case SERVER_PORTMAP_SYMMETRIC_NAT:
                        case SERVER_PORTMAP_FAILED:
                            ImGui::Text(detailFmt,
                                        (unsigned)(pm2.internalPort != 0
                                                       ? pm2.internalPort
                                                       : 27500));
                            break;
                        default:
                            ImGui::TextUnformatted(detailFmt);
                            break;
                    }
                    ImGui::PopTextWrapPos();
                    ImGui::Spacing();
                    if (ImGui::Button(langGetText(STR_CLOSE)) ||
                        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }
        }

        /* --- Main content --- */
#if BOLO_MOBILE
        /* Tabbed layout for mobile: Players | Map | Chat */
        {
            float availW = ImGui::GetContentRegionAvail().x - padR;
            float btnAreaH = ImGui::GetTextLineHeightWithSpacing() * 2 + 16.0f * s;

            /* Detect new chat messages for unread indicator */
            int chatLen = (int)SDL_strlen(cs->lobbyChatHistory);
            if (chatLen > lastChatLen && activeTab != 2) {
                chatUnread = true;
            }
            lastChatLen = chatLen;

            if (ImGui::BeginTabBar("##LobbyTabs")) {
                /* --- Players tab --- */
                if (ImGui::BeginTabItem(langGetText(STR_MENU_PLAYERS))) {
                    activeTab = 0;
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;
                    ImGui::BeginChild("##PlayerPanel", ImVec2(availW, tabH), ImGuiChildFlags_None);

                    /* Layout A: team-grouped player rendering. The
                     * legacy 5-column table below the #if 0 is left
                     * intact for reference; toggle the 0/1 to A/B
                     * compare during the in-progress UI rewrite. */
#if 1
                    bool isHostHere = (myPlayerNum == 0);
                    renderTeamGroupedPlayers(cs, transport, myPlayerNum, s, isHostHere);
                    /* Avoid the legacy table entirely. */
                    if (false) {
#else
                    if (ImGui::BeginTable("##PlayerTable", 5,
                                          ImGuiTableFlags_Borders |
                                          ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_ScrollY)) {
#endif
                        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_PLAYER_COL), ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_PING_COL), ImGuiTableColumnFlags_WidthFixed, 45.0f * s);
                        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_TEAM_COL), ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
                        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_READY_COL), ImGuiTableColumnFlags_WidthFixed, 45.0f * s);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 65.0f * s);
                        ImGui::TableHeadersRow();

                        bool botsAllowed = (cs->lobbyAiType != 0);

                        WB_LOG_TRACE(WB_LOG_CAT_GUI, "[LOBBY DBG] rendering player table");
                        for (int i = 0; i < MAX_TANKS; i++) {
                            ImGui::TableNextRow();

                            if (cs->lobbySlots[i].connected) {
                                bool isMe = (i == myPlayerNum);

                                /* Player Name (with flag) */
                                ImGui::TableSetColumnIndex(0);
                                if (cs->lobbySlots[i].countryCode[0] != '\0') {
                                    SDL_Texture *flagTex = flagsGetTexture(cs->lobbySlots[i].countryCode);
                                    if (flagTex) {
                                        ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                                        ImGui::SameLine();
                                    }
                                }
                                if (!cs->lobbySlots[i].isBot) {
                                    renderPlayerName(NULL,
                                                     cs->lobbySlots[i].clientFlags,
                                                     cs->lobbySlots[i].clientType,
                                                     "", false);
                                }
                                if (cs->lobbySlots[i].isBot) {
                                    MessageArgs args = {};
                                    strncpy(args.playerName, cs->lobbySlots[i].playerName, sizeof(args.playerName) - 1);
                                    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                                                       langGetTextFmt(STR_DLGLOBBY_BOT_FMT, &args));
                                } else if (isMe) {
                                    MessageArgs args = {};
                                    strncpy(args.playerName, cs->lobbySlots[i].playerName, sizeof(args.playerName) - 1);
                                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s",
                                                       langGetTextFmt(STR_DLGLOBBY_YOU_FMT, &args));
                                } else {
                                    ImGui::Text("%s", cs->lobbySlots[i].playerName);
                                }

                                /* Ping */
                                ImGui::TableSetColumnIndex(1);
                                if (cs->lobbySlots[i].pingMs > 0) {
                                    ImVec4 pingColor;
                                    if (cs->lobbySlots[i].pingMs < 50)        pingColor = ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
                                    else if (cs->lobbySlots[i].pingMs < 150)   pingColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
                                    else                                        pingColor = ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
                                    ImGui::TextColored(pingColor, "%dms", (int)cs->lobbySlots[i].pingMs);
                                } else {
                                    ImGui::TextDisabled("-");
                                }

                                /* Team */
                                ImGui::TableSetColumnIndex(2);
                                if (isMe && transport) {
                                    int teamIdx = cs->lobbySlots[i].teamNumber;
                                    ImGui::SetNextItemWidth(-1);
                                    char comboId[16];
                                    SDL_snprintf(comboId, sizeof(comboId), "##team%d", i);
                                    if (ImGui::Combo(comboId, &teamIdx, teamItems, 17)) {
                                        transportUdpClientSendTeamSet(transport, (uint8_t)teamIdx);
                                    }
                                } else {
                                    if (cs->lobbySlots[i].teamNumber > 0) {
                                        ImGui::Text("%d", cs->lobbySlots[i].teamNumber);
                                    } else {
                                        ImGui::TextDisabled("%s", langGetText(STR_NONE));
                                    }
                                }
                                if (cs->balanceProposalActive && cs->balanceProposal[i] != 0 &&
                                    cs->balanceProposal[i] != cs->lobbySlots[i].teamNumber) {
                                    ImGui::SameLine();
                                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "-> %d", cs->balanceProposal[i]);
                                }

                                /* Ready */
                                ImGui::TableSetColumnIndex(3);
                                if (cs->lobbySlots[i].ready) {
                                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", langGetText(STR_YES));
                                } else {
                                    ImGui::TextDisabled("%s", langGetText(STR_NO));
                                }

                                /* Action */
                                ImGui::TableSetColumnIndex(4);
                                if (cs->lobbySlots[i].isBot && transport) {
                                    char btnId[64];
                                    SDL_snprintf(btnId, sizeof(btnId), "%s##%d", langGetText(STR_DLGLOBBY_REMOVE), i);
                                    if (ImGui::SmallButton(btnId)) {
                                        transportUdpClientSendRemoveBot(transport, (uint8_t)i);
                                    }
                                }
                            } else {
                                /* Empty slot */
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextDisabled("---");
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextDisabled("-");
                                ImGui::TableSetColumnIndex(2);
                                ImGui::TextDisabled("-");
                                ImGui::TableSetColumnIndex(3);
                                ImGui::TextDisabled("-");
                                ImGui::TableSetColumnIndex(4);
                                if (botsAllowed && transport) {
                                    char btnId[64];
                                    SDL_snprintf(btnId, sizeof(btnId), "%s##%d", langGetText(STR_DLGLOBBY_ADDBOT), i);
                                    if (ImGui::SmallButton(btnId)) {
                                        transportUdpClientSendAddBot(transport);
                                    }
                                }
                            }
                        }
                        ImGui::EndTable();
                    }

                    ImGui::EndChild(); /* ##PlayerPanel */
                    ImGui::EndTabItem();
                }

                /* --- Map tab --- */
                if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_MAP_TAB))) {
                    activeTab = 1;
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;

                    if (!cs->mapDownloadComplete) {
                        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_DOWNLOADING));
                        ImGui::Spacing();
                        float progress = (float)netGetDownloadPos() / 255.0f;
                        ImGui::ProgressBar(progress, ImVec2(-1, 20.0f * s));
                    } else if (mapPreviewTex) {
                        int pad = 4;
                        int bx0 = mapBounds.minX - pad; if (bx0 < 0) bx0 = 0;
                        int by0 = mapBounds.minY - pad; if (by0 < 0) by0 = 0;
                        int bx1 = mapBounds.maxX + pad; if (bx1 >= MAP_PREVIEW_SIZE) bx1 = MAP_PREVIEW_SIZE - 1;
                        int by1 = mapBounds.maxY + pad; if (by1 >= MAP_PREVIEW_SIZE) by1 = MAP_PREVIEW_SIZE - 1;
                        int bw = bx1 - bx0;
                        int bh = by1 - by0;
                        if (bw > bh) {
                            int diff = bw - bh;
                            by0 -= diff / 2; by1 += (diff + 1) / 2;
                            if (by0 < 0) { by1 -= by0; by0 = 0; }
                            if (by1 >= MAP_PREVIEW_SIZE) { by0 -= (by1 - MAP_PREVIEW_SIZE + 1); by1 = MAP_PREVIEW_SIZE - 1; }
                            if (by0 < 0) by0 = 0;
                        } else if (bh > bw) {
                            int diff = bh - bw;
                            bx0 -= diff / 2; bx1 += (diff + 1) / 2;
                            if (bx0 < 0) { bx1 -= bx0; bx0 = 0; }
                            if (bx1 >= MAP_PREVIEW_SIZE) { bx0 -= (bx1 - MAP_PREVIEW_SIZE + 1); bx1 = MAP_PREVIEW_SIZE - 1; }
                            if (bx0 < 0) bx0 = 0;
                        }
                        ImVec2 uv0((float)bx0 / MAP_PREVIEW_SIZE, (float)by0 / MAP_PREVIEW_SIZE);
                        ImVec2 uv1((float)(bx1 + 1) / MAP_PREVIEW_SIZE, (float)(by1 + 1) / MAP_PREVIEW_SIZE);

                        float infoH = ImGui::GetTextLineHeightWithSpacing() * 2;
                        float previewMaxH = tabH - infoH;
                        float previewMaxW = ImGui::GetContentRegionAvail().x;
                        float previewSize = previewMaxW < previewMaxH ? previewMaxW : previewMaxH;
                        if (previewSize < 10.0f) previewSize = 10.0f;
                        float offsetX = (previewMaxW - previewSize) * 0.5f;
                        if (offsetX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
                        ImGui::Image((ImTextureID)mapPreviewTex, ImVec2(previewSize, previewSize), uv0, uv1);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        }
                        if (popupCompressedData) {
                            mapPreviewPopupOnClick(popupCompressedData, popupCompressedLen,
                                                   mapBounds.minX, mapBounds.minY,
                                                   mapBounds.maxX, mapBounds.maxY);
                        }
                    } else {
                        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MAP_UNAVAILABLE));
                    }
                    ImGui::Spacing();
                    ImGui::Text("%s - %dP %dB %dS", cs->mapName, cs->lobbyPillCount, cs->lobbyBaseCount, cs->lobbyStartCount);

                    if (cs->mapSkipAvailable && cs->inLobby) {
                        ImGui::Spacing();
                        bool countdownActive = cs->countdownSeconds > 0;
                        if (countdownActive) ImGui::BeginDisabled();
                        bool voted = cs->mapSkipMyVote;
                        const char *skipLabel = voted ? langGetText(STR_DLGLOBBY_CANCELSKIP) : langGetText(STR_DLGLOBBY_SKIPMAP);
                        if (voted) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.3f, 0.05f, 1.0f));
                        }
                        if (ImGui::Button(skipLabel, ImVec2(100 * s, 0))) {
                            cs->mapSkipMyVote = !cs->mapSkipMyVote;
                            if (transport) {
                                transportUdpClientSendMapSkipVote(transport);
                            }
                        }
                        if (voted) {
                            ImGui::PopStyleColor(3);
                        }
                        ImGui::SameLine();
                        int skipCount = 0, humanCount = 0;
                        for (int j = 0; j < MAX_TANKS; j++) {
                            if (cs->lobbySlots[j].connected && !cs->lobbySlots[j].isBot) {
                                humanCount++;
                                if (cs->mapSkipVotes[j]) skipCount++;
                            }
                        }
                        {
                            MessageArgs args = {};
                            args.number = skipCount;
                            args.number2 = humanCount;
                            ImGui::TextUnformatted(langGetTextFmt(STR_DLGLOBBY_VOTES, &args));
                        }
                        if (countdownActive) ImGui::EndDisabled();
                    }

                    ImGui::EndTabItem();
                }

                /* --- Chat tab (with unread indicator) --- */
                {
                    bool chatTabColorPushed = false;
                    if (chatUnread) {
                        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.5f, 0.0f, 0.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                        chatTabColorPushed = true;
                    }
                    if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_CHAT))) {
                        activeTab = 2;
                        chatUnread = false;
                        if (chatTabColorPushed) {
                            ImGui::PopStyleColor(2);
                            chatTabColorPushed = false;
                        }
                        float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;
                        float inputH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
                        float chatHistH = tabH - inputH;
                        if (chatHistH < 20.0f) chatHistH = 20.0f;

                        ImGui::BeginChild("##ChatHistory", ImVec2(0, chatHistH), ImGuiChildFlags_Borders);
                        ImGui::TextUnformatted(cs->lobbyChatHistory);
                        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
                            ImGui::SetScrollHereY(1.0f);
                        }
                        ImGui::EndChild();

                        {
                            float btnW = 60.0f * s;
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - 8.0f);
                            bool enterPressed = ImGui::InputText("##ChatInput", chatInput, CHAT_INPUT_SIZE,
                                                                  ImGuiInputTextFlags_EnterReturnsTrue);
                            ImGui::SameLine();
                            bool chatEmpty = (chatInput[0] == '\0');
                            if (chatEmpty) ImGui::BeginDisabled();
                            bool sendClicked = ImGui::Button(langGetText(STR_DLGMSG_BUTTON), ImVec2(btnW, 0));
                            if (chatEmpty) ImGui::EndDisabled();
                            if ((sendClicked || enterPressed) &&
                                !chatEmpty && transport) {
                                transportUdpClientSendChat(transport, 0xFF, chatInput);
                                const char *myName = (myPlayerNum < MAX_TANKS && cs->lobbySlots[myPlayerNum].connected)
                                    ? cs->lobbySlots[myPlayerNum].playerName : langGetText(STR_DLGLOBBY_ME);
                                clientSimAppendLobbyChat(cs, myName, chatInput);
                                chatInput[0] = '\0';
                            }
                        }

                        ImGui::EndTabItem();
                    }
                    if (chatTabColorPushed) {
                        ImGui::PopStyleColor(2);
                    }
                }

                ImGui::EndTabBar();
            }

            /* --- Bottom buttons (always visible) --- */
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            {
                bool myReady = (myPlayerNum < MAX_TANKS && cs->lobbySlots[myPlayerNum].connected)
                               ? cs->lobbySlots[myPlayerNum].ready : false;
                bool canReady = cs->mapDownloadComplete;

                if (!canReady) ImGui::BeginDisabled();
                const char *readyLabel = myReady ? langGetText(STR_DLGLOBBY_UNREADY) : langGetText(STR_DLGLOBBY_READY);
                if (ImGui::Button(readyLabel, ImVec2(100 * s, 0))) {
                    if (transport) {
                        transportUdpClientSendReady(transport, !myReady);
                    }
                }
                if (!canReady) ImGui::EndDisabled();

                if (myPlayerNum == 0 && transport && !cs->balanceProposalActive) {
                    bool hasWbnPlayers = false;
                    uint8_t connectedCount = 0;
                    for (int j = 0; j < 16; j++) {
                        if (cs->lobbySlots[j].connected) {
                            connectedCount++;
                            if (cs->lobbySlots[j].clientFlags & PLAYER_FLAG_WBN_VERIFIED) {
                                hasWbnPlayers = true;
                            }
                        }
                    }
                    if (hasWbnPlayers) {
                        ImGui::SameLine(0, 20);
                        if (connectedCount < 2) ImGui::BeginDisabled();
                        if (ImGui::Button(langGetText(STR_DLGLOBBY_BALANCE_TEAMS), ImVec2(120 * s, 0))) {
                            uint8_t teamSize = (connectedCount > 1) ? (connectedCount / 2) : 1;
                            transportUdpClientSendBalanceRequest(transport, teamSize);
                        }
                        if (connectedCount < 2) ImGui::EndDisabled();
                    }
                } else if (myPlayerNum == 0 && transport && cs->balanceProposalActive) {
                    ImGui::SameLine(0, 20);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                    if (ImGui::Button(langGetText(STR_DLGLOBBY_APPLY_BALANCE), ImVec2(120 * s, 0))) {
                        transportUdpClientSendBalanceApply(transport);
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 8);
                    if (ImGui::Button(langGetText(STR_DLGLOBBY_DISMISS), ImVec2(80 * s, 0))) {
                        transportUdpClientSendBalanceDismiss(transport);
                    }
                }

                ImGui::SameLine(0, 20);
                if (ImGui::Button(langGetText(STR_DLGLOBBY_LEAVE), ImVec2(100 * s, 0)) ||
                    (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                     !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                    char leavePopupId[64];
                    SDL_snprintf(leavePopupId, sizeof(leavePopupId), "%s##lobby", langGetText(STR_DLGLOBBY_LEAVE_TITLE));
                    ImGui::OpenPopup(leavePopupId);
                }
            }
        }
#else
        /* --- Desktop: Players (left) + Map Preview (right) --- */
        {
            float availW = ImGui::GetContentRegionAvail().x - padR;
            float availContentH = ImGui::GetContentRegionAvail().y
                           - ImGui::GetTextLineHeightWithSpacing() * 9  /* chat + buttons */
                           - 40.0f * s - padB;
            float mapPanelW = (MAP_PREVIEW_SIZE + 20) * s;
            float playerPanelW = availW - mapPanelW - 8.0f;
            float panelH = availContentH;

            /* Left: Player panel — Layout A team-grouped rendering. */
            ImGui::BeginChild("##PlayerPanel", ImVec2(playerPanelW, panelH), ImGuiChildFlags_None);

            /* Layout A: team-grouped player rendering. Legacy 6-column
             * table preserved below the #if 0 for reference; toggle to
             * A/B compare during the in-progress UI rewrite. */
#if 1
            {
                bool isHostHere = (myPlayerNum == 0);
                renderTeamGroupedPlayers(cs, transport, myPlayerNum, s, isHostHere);
            }
            if (false) {
#else
            if (ImGui::BeginTable("##PlayerTable", 6,
                                  ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY)) {
#endif
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_SLOT_COL),   ImGuiTableColumnFlags_WidthFixed, 30.0f * s);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_NAME_COL),   ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_PING_COL),   ImGuiTableColumnFlags_WidthFixed, 45.0f * s);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_TEAM_COL),   ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_READY_COL),  ImGuiTableColumnFlags_WidthFixed, 40.0f * s);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_ACTION_COL), ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
                ImGui::TableHeadersRow();

                bool botsAllowed = (cs->lobbyAiType != 0);

                for (int i = 0; i < MAX_TANKS; i++) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", i);

                    if (cs->lobbySlots[i].connected) {
                        bool isMe = (i == myPlayerNum);

                        /* Player Name (with flag icon) */
                        ImGui::TableSetColumnIndex(1);
                        if (cs->lobbySlots[i].countryCode[0] != '\0') {
                            SDL_Texture *flagTex = flagsGetTexture(cs->lobbySlots[i].countryCode);
                            if (flagTex) {
                                ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                                ImGui::SameLine();
                            }
                        }
                        if (!cs->lobbySlots[i].isBot) {
                            renderPlayerName(NULL,
                                             cs->lobbySlots[i].clientFlags,
                                             cs->lobbySlots[i].clientType,
                                             "", false);
                        }
                        if (cs->lobbySlots[i].isBot) {
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                                               "%s [Bot]", cs->lobbySlots[i].playerName);
                        } else if (isMe) {
                            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
                                               "%s (You)", cs->lobbySlots[i].playerName);
                        } else {
                            ImGui::Text("%s", cs->lobbySlots[i].playerName);
                        }

                        /* Ping */
                        ImGui::TableSetColumnIndex(2);
                        if (cs->lobbySlots[i].pingMs > 0) {
                            ImVec4 pingColor;
                            if (cs->lobbySlots[i].pingMs < 50)        pingColor = ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
                            else if (cs->lobbySlots[i].pingMs < 150)   pingColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
                            else                                        pingColor = ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
                            ImGui::TextColored(pingColor, "%dms", (int)cs->lobbySlots[i].pingMs);
                        } else {
                            ImGui::TextDisabled("-");
                        }

                        /* Team */
                        ImGui::TableSetColumnIndex(3);
                        if (isMe && transport) {
                            int teamIdx = cs->lobbySlots[i].teamNumber;
                            ImGui::SetNextItemWidth(-1);
                            char comboId[16];
                            SDL_snprintf(comboId, sizeof(comboId), "##team%d", i);
                            if (ImGui::Combo(comboId, &teamIdx, teamItems, 17)) {
                                transportUdpClientSendTeamSet(transport, (uint8_t)teamIdx);
                            }
                        } else {
                            if (cs->lobbySlots[i].teamNumber > 0) {
                                ImGui::Text("%d", cs->lobbySlots[i].teamNumber);
                            } else {
                                ImGui::TextDisabled("%s", langGetText(STR_NONE));
                            }
                        }
                        if (cs->balanceProposalActive && cs->balanceProposal[i] != 0 &&
                            cs->balanceProposal[i] != cs->lobbySlots[i].teamNumber) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "-> %d", cs->balanceProposal[i]);
                        }

                        /* Ready */
                        ImGui::TableSetColumnIndex(4);
                        if (cs->lobbySlots[i].ready) {
                            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", langGetText(STR_YES));
                        } else {
                            ImGui::TextDisabled("%s", langGetText(STR_NO));
                        }

                        /* Action */
                        ImGui::TableSetColumnIndex(5);
                        if (cs->lobbySlots[i].isBot && transport) {
                            char btnId[16];
                            SDL_snprintf(btnId, sizeof(btnId), "Remove##%d", i);
                            if (ImGui::SmallButton(btnId)) {
                                transportUdpClientSendRemoveBot(transport, (uint8_t)i);
                            }
                        }
                    } else {
                        /* Empty slot */
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("---");
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextDisabled("-");
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextDisabled("-");
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextDisabled("-");
                        ImGui::TableSetColumnIndex(5);
                        if (botsAllowed && transport) {
                            char btnId[16];
                            SDL_snprintf(btnId, sizeof(btnId), "Add Bot##%d", i);
                            if (ImGui::SmallButton(btnId)) {
                                transportUdpClientSendAddBot(transport);
                            }
                        }
                    }
                }
                ImGui::EndTable();
            }

            ImGui::EndChild(); /* ##PlayerPanel */

            ImGui::SameLine(0, 8.0f);

            /* Right: Map preview + info */
            ImGui::BeginChild("##MapPanel", ImVec2(mapPanelW, panelH), ImGuiChildFlags_Borders);

            if (!cs->mapDownloadComplete) {
                /* Map downloading - show progress */
                ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_DOWNLOADING));
                ImGui::Spacing();
                float progress = (float)netGetDownloadPos() / 255.0f;
                ImGui::ProgressBar(progress, ImVec2(-1, 20.0f * s));
                ImGui::Spacing();
            } else if (mapPreviewTex) {
                /* Compute UV coordinates to zoom into the interesting area with padding */
                int pad = 4;
                int bx0 = mapBounds.minX - pad; if (bx0 < 0) bx0 = 0;
                int by0 = mapBounds.minY - pad; if (by0 < 0) by0 = 0;
                int bx1 = mapBounds.maxX + pad; if (bx1 >= MAP_PREVIEW_SIZE) bx1 = MAP_PREVIEW_SIZE - 1;
                int by1 = mapBounds.maxY + pad; if (by1 >= MAP_PREVIEW_SIZE) by1 = MAP_PREVIEW_SIZE - 1;
                /* Make the region square so the preview isn't distorted */
                int bw = bx1 - bx0;
                int bh = by1 - by0;
                if (bw > bh) {
                    int diff = bw - bh;
                    by0 -= diff / 2;
                    by1 += (diff + 1) / 2;
                    if (by0 < 0) { by1 -= by0; by0 = 0; }
                    if (by1 >= MAP_PREVIEW_SIZE) { by0 -= (by1 - MAP_PREVIEW_SIZE + 1); by1 = MAP_PREVIEW_SIZE - 1; }
                    if (by0 < 0) by0 = 0;
                } else if (bh > bw) {
                    int diff = bh - bw;
                    bx0 -= diff / 2;
                    bx1 += (diff + 1) / 2;
                    if (bx0 < 0) { bx1 -= bx0; bx0 = 0; }
                    if (bx1 >= MAP_PREVIEW_SIZE) { bx0 -= (bx1 - MAP_PREVIEW_SIZE + 1); bx1 = MAP_PREVIEW_SIZE - 1; }
                    if (bx0 < 0) bx0 = 0;
                }
                ImVec2 uv0((float)bx0 / MAP_PREVIEW_SIZE, (float)by0 / MAP_PREVIEW_SIZE);
                ImVec2 uv1((float)(bx1 + 1) / MAP_PREVIEW_SIZE, (float)(by1 + 1) / MAP_PREVIEW_SIZE);

                /* Map preview image - fit to available panel width */
                float panelWidth = ImGui::GetContentRegionAvail().x;
                float previewSize = panelWidth;
                float availH = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 5;
                if (availH < previewSize) previewSize = availH;
                /* Center the preview */
                float offsetX = (panelWidth - previewSize) * 0.5f;
                if (offsetX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
                ImGui::Image((ImTextureID)mapPreviewTex, ImVec2(previewSize, previewSize), uv0, uv1);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
                if (popupCompressedData) {
                    mapPreviewPopupOnClick(popupCompressedData, popupCompressedLen,
                                           mapBounds.minX, mapBounds.minY,
                                           mapBounds.maxX, mapBounds.maxY);
                }
            } else {
                ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MAP_UNAVAILABLE));
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* Map info */
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MAP_LBL), cs->mapName);
            ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_PILLBOXES), cs->lobbyPillCount);
            ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_BASES), cs->lobbyBaseCount);
            ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_STARTS), cs->lobbyStartCount);

            if (cs->mapSkipAvailable && cs->inLobby) {
                ImGui::Spacing();
                bool countdownActive = cs->countdownSeconds > 0;
                if (countdownActive) ImGui::BeginDisabled();
                bool voted = cs->mapSkipMyVote;
                const char *skipLabel = langGetText(
                    voted ? STR_DLGLOBBY_CANCELSKIP : STR_DLGLOBBY_SKIPMAP);
                if (voted) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.3f, 0.05f, 1.0f));
                }
                if (ImGui::Button(skipLabel, ImVec2(100 * s, 0))) {
                    cs->mapSkipMyVote = !cs->mapSkipMyVote;
                    if (transport) {
                        transportUdpClientSendMapSkipVote(transport);
                    }
                }
                if (voted) {
                    ImGui::PopStyleColor(3);
                }
                ImGui::SameLine();
                int skipCount = 0, humanCount = 0;
                for (int j = 0; j < MAX_TANKS; j++) {
                    if (cs->lobbySlots[j].connected && !cs->lobbySlots[j].isBot) {
                        humanCount++;
                        if (cs->mapSkipVotes[j]) skipCount++;
                    }
                }
                {
                    MessageArgs vargs;
                    memset(&vargs, 0, sizeof(vargs));
                    vargs.number  = skipCount;
                    vargs.number2 = humanCount;
                    ImGui::TextUnformatted(
                        langGetTextFmt(STR_DLGLOBBY_VOTES, &vargs));
                }
                if (countdownActive) ImGui::EndDisabled();
            }

            ImGui::EndChild(); /* ##MapPanel */
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Chat section --- */
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_CHAT));
        {
            float chatHeight = ImGui::GetTextLineHeightWithSpacing() * 4;
            ImGui::BeginChild("##ChatHistory", ImVec2(0, chatHeight), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(cs->lobbyChatHistory);
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }

        {
            float btnW = 60.0f * s;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - 8.0f);
            bool enterPressed = ImGui::InputText("##ChatInput", chatInput, CHAT_INPUT_SIZE,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            bool chatEmpty = (chatInput[0] == '\0');
            if (chatEmpty) ImGui::BeginDisabled();
            bool sendClicked = ImGui::Button(langGetText(STR_DLGMSG_BUTTON), ImVec2(btnW, 0));
            if (chatEmpty) ImGui::EndDisabled();
            if ((sendClicked || enterPressed) &&
                !chatEmpty && transport) {
                transportUdpClientSendChat(transport, 0xFF, chatInput);
                const char *myName = (myPlayerNum < MAX_TANKS && cs->lobbySlots[myPlayerNum].connected)
                    ? cs->lobbySlots[myPlayerNum].playerName : langGetText(STR_DLGLOBBY_ME);
                clientSimAppendLobbyChat(cs, myName, chatInput);
                chatInput[0] = '\0';
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Bottom buttons --- */
        {
            bool myReady = (myPlayerNum < MAX_TANKS && cs->lobbySlots[myPlayerNum].connected)
                           ? cs->lobbySlots[myPlayerNum].ready : false;
            bool canReady = cs->mapDownloadComplete;

            if (!canReady) ImGui::BeginDisabled();
            const char *readyLabel = myReady ? langGetText(STR_DLGLOBBY_UNREADY) : langGetText(STR_DLGLOBBY_READY);
            if (ImGui::Button(readyLabel, ImVec2(100 * s, 0))) {
                if (transport) {
                    transportUdpClientSendReady(transport, !myReady);
                }
            }
            if (!canReady) ImGui::EndDisabled();

            if (myPlayerNum == 0 && transport && !cs->balanceProposalActive) {
                bool hasWbnPlayers = false;
                uint8_t connectedCount = 0;
                for (int j = 0; j < 16; j++) {
                    if (cs->lobbySlots[j].connected) {
                        connectedCount++;
                        if (cs->lobbySlots[j].clientFlags & PLAYER_FLAG_WBN_VERIFIED) {
                            hasWbnPlayers = true;
                        }
                    }
                }
                if (hasWbnPlayers) {
                    ImGui::SameLine(0, 20);
                    if (connectedCount < 2) ImGui::BeginDisabled();
                    if (ImGui::Button(langGetText(STR_DLGLOBBY_BALANCE_TEAMS), ImVec2(120 * s, 0))) {
                        uint8_t teamSize = (connectedCount > 1) ? (connectedCount / 2) : 1;
                        transportUdpClientSendBalanceRequest(transport, teamSize);
                    }
                    if (connectedCount < 2) ImGui::EndDisabled();
                }
            } else if (myPlayerNum == 0 && transport && cs->balanceProposalActive) {
                ImGui::SameLine(0, 20);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                if (ImGui::Button(langGetText(STR_DLGLOBBY_APPLY_BALANCE), ImVec2(120 * s, 0))) {
                    transportUdpClientSendBalanceApply(transport);
                }
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 8);
                if (ImGui::Button(langGetText(STR_DLGLOBBY_DISMISS), ImVec2(80 * s, 0))) {
                    transportUdpClientSendBalanceDismiss(transport);
                }
            }

            ImGui::SameLine(0, 20);
            if (ImGui::Button(langGetText(STR_DLGLOBBY_LEAVE), ImVec2(100 * s, 0)) ||
                (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                 !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                char leavePopupId[64];
                SDL_snprintf(leavePopupId, sizeof(leavePopupId), "%s##lobby", langGetText(STR_DLGLOBBY_LEAVE_TITLE));
                ImGui::OpenPopup(leavePopupId);
            }
        }
#endif

        /* --- Map preview popup --- */
        mapPreviewPopupRenderModal(renderer);

        /* --- Leave confirmation popup --- */
        char leavePopupModalId[64];
        SDL_snprintf(leavePopupModalId, sizeof(leavePopupModalId), "%s##lobby", langGetText(STR_DLGLOBBY_LEAVE_TITLE));
        if (ImGui::BeginPopupModal(leavePopupModalId, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_LEAVE_BLURB));
            ImGui::Spacing();
            if (ImGui::Button(langGetText(STR_YES), ImVec2(80 * s, 0))) {
                ImGui::CloseCurrentPopup();
                result = 0;
                running = false;
            }
            ImGui::SameLine(0.0f, 8.0f);
            if (ImGui::Button(langGetText(STR_NO), ImVec2(80 * s, 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        /* --- Countdown overlay --- */
        if (cs->netStat == netLobbyCountdown && cs->countdownSeconds > 0) {
            char countdownText[64];
            MessageArgs args = {};
            args.number = cs->countdownSeconds;
            SDL_snprintf(countdownText, sizeof(countdownText), "%s",
                         langGetTextFmt(STR_DLGLOBBY_STARTING_FMT, &args));
            ImGui::PushFont(countdownFont);
            ImVec2 textSize = ImGui::CalcTextSize(countdownText);
            ImGui::PopFont();
            ImVec2 winPos = ImGui::GetWindowPos();
            ImVec2 winSize = ImGui::GetWindowSize();
            ImVec2 textPos = ImVec2(
                winPos.x + (winSize.x - textSize.x) * 0.5f,
                winPos.y + (winSize.y - textSize.y) * 0.5f
            );
            ImDrawList *fg = ImGui::GetForegroundDrawList();
            float pad = 12.0f * s;
            fg->AddRectFilled(
                ImVec2(textPos.x - pad, textPos.y - pad * 0.5f),
                ImVec2(textPos.x + textSize.x + pad, textPos.y + textSize.y + pad * 0.5f),
                IM_COL32(0, 0, 0, 180), 6.0f * s);
            fg->AddText(countdownFont, countdownFontSize, textPos,
                        IM_COL32(255, 255, 0, 255), countdownText);
        }

        ImGui::End(); /* ##LobbyBg */
        ImGui::PopStyleVar(); /* WindowPadding */

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    /* Clean up map preview texture */
    if (mapPreviewTex) {
        SDL_DestroyTexture(mapPreviewTex);
    }
    if (popupCompressedData) {
        SDL_free(popupCompressedData);
        popupCompressedData = NULL;
        popupCompressedLen = 0;
    }
    mapPreviewPopupDestroy();

    /* Dismiss soft keyboard and tear down ImGui */
    dialogDismissKeyboard(window);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    /* Restore logical presentation */
    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    SDL_FlushEvent(SDL_EVENT_QUIT);

    return result;
}
