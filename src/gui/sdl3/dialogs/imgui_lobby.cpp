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
#include <cstdlib>  /* rand() — used to randomise the default naming pool */
#include <cfloat>   /* FLT_MAX — unbounded max for window size constraints */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* ImGui::CloseButton — proper X widget with hit area */
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../gamefront.h"
#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "server_sim.h"          /* serverSim* T1 wrappers for SP-host paths */
#include "lobby_bot_pools.h"     /* lobbyBotPool* — public utility */
#include "../../../server/server_lifecycle.h"
#include "platform_net.h"
#include "../flags.h"
#include "../sdl3imgui.h"
#include "../minimap_render.h"
#include "../map_preview_popup.h"
#include "../../lang.h"
#include "imgui_lobby.h"
#include "imgui_messagebox.h"
#include "imgui_mapchooser.h"
}
#include "../wb_theme.h"

#define MAX_TANKS 16
#define WBN_ICON_SIZE 14
#define CHAT_INPUT_SIZE 129  /* 128 chars + null terminator */
#define MAP_PREVIEW_SIZE 256

static const int DIALOG_W = 1024;
static const int DIALOG_H = 768;

/* Per-slot tracking of whether the bot's name was manually overridden
 * by the host typing into the name input field.  Cleared when a bot
 * is added or its name is rerolled (those are pool-driven names);
 * set when the user types a name in the AiConfig sub-panel.  Used by
 * the team-naming-pool dropdown to decide which bots to auto-rename
 * when the pool changes — overridden names stay, pool-driven names
 * get a fresh pick from the new pool.
 *
 * UI-side state only (no server propagation). For multiplayer the
 * server picks names so this array would never gate anything; for
 * single-player it's the source of truth. */
static bool s_botNameOverridden[MAX_TANKS] = {0};

/* ── Single-player vs multiplayer command dispatch ─────────────────
 * The lobby UI was originally written against the UDP transport — it
 * sends PACKET_LOBBY_* commands and relies on the server broadcasting
 * resulting state changes back via PACKET_LOBBY_STATE.  The single-
 * player path runs the server in-process and has no packet flow, so
 * commands route directly to the local spServerSim and the result is
 * the matching transportUdpServerBroadcast*Chg function fires —
 * those publish ControlEvents on the in-process subscriber bus so
 * the local humanSim sees the new state via clientSimApplyControl.
 *
 * Each helper takes both the ClientSim (to detect single-player) and
 * the Transport pointer (used by the multiplayer path).  When the
 * Transport is null and we're not single-player, the call is a no-op.
 */
static void lobbySendReadyToggle(ClientSim *cs, bool ready) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        /* Single-player: skip the multiplayer ready→countdown
         * choreography; clicking the Ready button starts the game
         * straight away.  The Unready direction is meaningless here
         * (there's nobody to wait on) so we ignore ready=false. */
        if (ready) {
            (void)gameFrontStartSinglePlayerGame(cs);
        }
        return;
    }
    clientSimNetSendReady(cs, ready);
}

/* Add Bot. namingPool < 0 means "use the slot's team pool" (multiplayer
 * server already picks based on team membership). namingPool >= 0
 * forces a specific pool — used by per-team header "+ Bot" buttons
 * which have their own pool dropdown.  teamNumber > 0 assigns the
 * new bot to that team after creation; teamNumber == 0 leaves it on
 * whatever default team the server picked.  Single-player honors both
 * overrides locally; multiplayer currently ignores them (server-side
 * picks the name and default team — TODO: extend the protocol with a
 * teamId-aware add-bot packet). */
static void lobbySendAddBot(ClientSim *cs,
                            int namingPool, uint8_t teamNumber) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        /* Find first free slot */
        BYTE slot;
        for (slot = 1; slot < MAX_TANKS; slot++) {
            if (!serverSimIsPlayerConnected(sim, slot)) break;
        }
        if (slot >= MAX_TANKS) return;
        if (serverSimGetBotBrainPath(sim)[0] == '\0') return;

        /* Pick a name. If a pool override is supplied we use it; else
         * fall back to "Bot N". Build the used-names list from current
         * lobby slots so the picker doesn't collide with existing bots. */
        char botName[32];
        if (namingPool >= 0 && namingPool < lobbyBotPoolCount()) {
            const char *usedNames[MAX_TANKS];
            int usedCount = 0;
            for (int i = 0; i < MAX_TANKS; i++) {
                if (serverSimIsPlayerConnected(sim, i)) {
                    usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
                }
            }
            lobbyBotPoolPick(namingPool, usedNames, usedCount, botName, sizeof(botName));
        } else {
            snprintf(botName, sizeof(botName), "Bot %d", slot);
        }

        serverSimCreateBot(sim, slot, serverSimGetBotBrainPath(sim), botName,
                           (aiType)serverSimGetBotAiType(sim),
                           (gameType)clientSimGetLobbyGameType(cs),
                           clientSimIsLobbyHiddenMines(cs));
        /* Mirror the path into the per-bot table so the AiConfig combo
         * reflects "this bot's brain" rather than a global default. */
        serverSimSetBotBrainPathFor(sim, slot, serverSimGetBotBrainPath(sim));
        if (teamNumber > 0 && teamNumber < MAX_TANKS) {
            serverSimSetTeam(sim, slot, teamNumber);
        }
        /* Bot's name came from the pool — not an override. */
        s_botNameOverridden[slot] = false;
        /* Publish the slot's new state and the bot's brain path. */
        serverSimPublishLobbySlot(sim, slot);
        serverSimPublishLobbyBotBrain(sim, slot);
        return;
    }
    /* MP path */
    {
        /* Tell the server which brain to assign. Pick the first entry
         * in the catalogue — the host can change it later per-bot via
         * the AiConfig "Bot Code" combo (PACKET_LOBBY_SET_BOT_BRAIN).
         * Empty string lets the server use its own default. */
        const char *brainPath = "";
        if (cs && clientSimGetLobbyBrainList(cs)->count > 0) {
            brainPath = clientSimGetLobbyBrainList(cs)->entries[0].path;
        }
        /* Pick the bot's name from the chosen pool right here on the
         * client — server doesn't know pool contents (see
         * lobby_bot_pools.h). Empty string falls back to "Bot N". */
        char botName[32];
        botName[0] = '\0';
        if (cs && namingPool >= 0 && namingPool < lobbyBotPoolCount()) {
            const char *usedNames[MAX_TANKS];
            int usedCount = 0;
            for (int i = 0; i < MAX_TANKS; i++) {
                if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected &&
                    clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]) {
                    usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
                }
            }
            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             botName, sizeof(botName));
        }
        clientSimNetSendAddBotConfigured(cs, teamNumber, brainPath, botName);
    }
}

/* Add-bot debounce — disables the Add Bot button while a previously-sent
 * request is in flight. Counts current connected lobby slots at click
 * time, locks the button until either (a) the connected count grows to
 * the expected value (server acked) or (b) a 2 s timeout elapses (lost
 * packet / SP path that already returned). Prevents spam-clicks from
 * pushing past MAX_TANKS or otherwise racing the server.
 *
 * Belt-and-suspenders against the lobby-spam crash:
 *   - s_addBotFrame: ImGui frame number of the last successful send.
 *     Hard-blocks any second add-bot in the same frame, even if it
 *     comes from a different button (per-team header + per-row in the
 *     player table) before the debounce-disabled state can propagate.
 *   - All loops over MAX_TANKS guard the slot accessor against NULL
 *     and treat NULL as "not connected" (the SP path can briefly hold
 *     a partially-initialized slot while serverSimCreateBot is
 *     allocating the ClientSim). */
static Uint32 s_addBotSentMs = 0;
static int    s_addBotExpectedConn = 0;
static int    s_addBotFrame = -1;

static int lobbyCountConnectedSlots(ClientSim *cs) {
    int n = 0;
    if (!cs) return 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        const ClientLobbySlot *lp = clientSimGetLobbySlot(cs, (BYTE)i);
        if (lp && lp->connected) n++;
    }
    return n;
}

static bool lobbyAddBotPending(ClientSim *cs) {
    if (!cs) return false;
    /* In the same ImGui frame as the last send, refuse another no matter
     * what — multiple buttons render before any one's click can update
     * the disabled state of the others. */
    if (s_addBotFrame == ImGui::GetFrameCount()) return true;
    if (s_addBotSentMs == 0) return false;
    Uint32 now = SDL_GetTicks();
    if (now - s_addBotSentMs > 2000) {
        s_addBotSentMs = 0;
        return false;
    }
    if (lobbyCountConnectedSlots(cs) >= s_addBotExpectedConn) {
        s_addBotSentMs = 0;
        return false;
    }
    return true;
}

static void lobbySendAddBotDebounced(ClientSim *cs,
                                     int namingPool, uint8_t teamNumber) {
    if (!cs) return;
    /* Drop the call entirely if anything already added a bot this
     * frame — the visible button was probably stale-clicked. */
    if (s_addBotFrame == ImGui::GetFrameCount()) return;
    /* Refuse to push past the 16-slot ceiling regardless of how the
     * server would otherwise handle it. Keeps the bot pool / subscriber
     * registry from being torched if MAX_TANKS slots are already in use
     * and a queued click slips through. */
    if (lobbyCountConnectedSlots(cs) >= MAX_TANKS) return;
    s_addBotFrame = ImGui::GetFrameCount();
    s_addBotSentMs = SDL_GetTicks();
    s_addBotExpectedConn = lobbyCountConnectedSlots(cs) + 1;
    lobbySendAddBot(cs, namingPool, teamNumber);
}

/* ── Map chooser helpers (Phase 1) ─────────────────────────────────
 * The map chooser is a separate draggable ImGui window opened from the
 * lobby's Map tab. Phase 1 only wires the UI shell; selection is
 * local-only (no PACKET_LOBBY_SET_MAP yet — that's Phase 2). The state
 * persists across re-opens so the user's last tab + position are
 * remembered for the session.
 *
 * State has to be declared before the helpers that reference it.
 * s_chooseMapPrevName captures the map that was active when the window
 * opened so Cancel can restore it once Phase 2 wires the map-change
 * packet. s_chooseMapActiveTab survives across re-opens. */
static bool             s_chooseMapOpen          = false;
static MapChooserState  s_chooseMapState         = {};
static bool             s_chooseMapStateInited   = false;
static char             s_chooseMapPrevName[128] = "";
static int              s_chooseMapActiveTab     = 0; /* 0=server 1=upload 2=random */

static void lobbyChooseMapEnsureInit(SDL_Renderer *renderer) {
    if (!s_chooseMapStateInited) {
        mapChooserInit(&s_chooseMapState, renderer);
        /* "Load from device" + "Generate Random Map" exist as dedicated
         * tabs in this window, so suppress the in-widget buttons that
         * would duplicate them. */
        s_chooseMapState.hideExtras = true;
        /* Keep the map list narrow so the preview can claim most of
         * the row width. ~300 px fits a column of names comfortably
         * without crowding the preview. */
        s_chooseMapState.leftPanelMaxW = 300.0f;
        s_chooseMapStateInited = true;
    }
}

static void lobbyChooseMapOpen(ClientSim *cs, SDL_Renderer *renderer) {
    lobbyChooseMapEnsureInit(renderer);
    /* Snapshot the currently active map so Cancel can restore it once
     * Phase 2 wires the map-change packet. */
    const char *cur = cs ? clientSimGetMapName(cs) : "";
    SDL_strlcpy(s_chooseMapPrevName, cur ? cur : "",
                sizeof(s_chooseMapPrevName));

    /* Default the chooser's highlighted entry to whatever map is
     * currently active — so SP (which loads Everard by default) opens
     * with "Everard Island (Inbuilt)" pre-selected, and subsequent
     * opens reflect any choice made last time. Falls back to entry 0
     * (Everard) if the current map isn't in the list. */
    int matchedIdx = 0;
    if (cur && cur[0] != '\0') {
        for (int i = 0; i < s_chooseMapState.numMaps; i++) {
            if (SDL_strcasecmp(s_chooseMapState.maps[i].name, cur) == 0) {
                matchedIdx = i;
                break;
            }
        }
    }
    if (matchedIdx != s_chooseMapState.selectedIdx) {
        s_chooseMapState.selectedIdx = matchedIdx;
        SDL_strlcpy(s_chooseMapState.selectedPath,
                    s_chooseMapState.maps[matchedIdx].path,
                    sizeof(s_chooseMapState.selectedPath));
        SDL_strlcpy(s_chooseMapState.selectedName,
                    s_chooseMapState.maps[matchedIdx].name,
                    sizeof(s_chooseMapState.selectedName));
        s_chooseMapState.randomMapSelected = false;
    }

    s_chooseMapOpen = true;
}

static void lobbyChooseMapRenderWindow(ClientSim *cs, SDL_Renderer *renderer,
                                       float s, int screenW, int screenH) {
    if (!s_chooseMapOpen) return;
    lobbyChooseMapEnsureInit(renderer);

    /* Centered, compact default — sized so the action buttons are
     * visible without scrolling. First-use-only; user can drag/resize
     * freely after that and the size sticks for the session. */
    float defaultW = (float)screenW * 0.55f;
    float defaultH = (float)screenH * 0.55f;
    if (defaultW < 600.0f * s) defaultW = 600.0f * s;
    if (defaultH < 380.0f * s) defaultH = 380.0f * s;
    /* Cap default at a comfortably tight box so very tall windows
     * don't get a sprawling preview by default. */
    if (defaultH > 520.0f * s) defaultH = 520.0f * s;
    ImGui::SetNextWindowSize(ImVec2(defaultW, defaultH),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(((float)screenW - defaultW) * 0.5f,
               ((float)screenH - defaultH) * 0.5f),
        ImGuiCond_FirstUseEver);
    /* Min size keeps the action bar visible; no max — user can drag
     * the window as large as they like, even past the screen edges. */
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f * s, 320.0f * s),
                                        ImVec2(FLT_MAX, FLT_MAX));

    bool open = s_chooseMapOpen;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("Choose Map##LobbyMapChooser", &open, flags)) {
        ImGui::End();
        if (!open) s_chooseMapOpen = false;
        return;
    }

    /* Reserve space at the bottom for the action buttons so the tab
     * content gets the remaining region.  Compute the per-tab height
     * inside the tab item so it accounts for the tab-header strip
     * (otherwise the chooser overflows and forces a window scroll). */
    float btnBarH = ImGui::GetFrameHeight() + 14.0f * s;

    if (ImGui::BeginTabBar("##MapChooserTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Server Maps")) {
            s_chooseMapActiveTab = 0;
            float availW = ImGui::GetContentRegionAvail().x;
            float availH = ImGui::GetContentRegionAvail().y - btnBarH;
            if (availH < 120.0f) availH = 120.0f;
            /* Phase 1 uses the existing local data/maps/*.map scan as
             * the source list — fine in SP-host (host == server) and
             * provides a useful preview in MP too. Phase 3 will replace
             * with a server-list protocol so pure clients see what the
             * server actually has. */
            bool changed = mapChooserRender(&s_chooseMapState, renderer,
                                            availW, availH, s);
            if (changed) {
                /* TODO Phase 2: clientSimNetSendLobbySetMap(cs,
                 *                                           s_chooseMapState.selectedPath);
                 * For now the selection is recorded locally — server
                 * isn't told about it yet. */
                (void)cs;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Upload")) {
            s_chooseMapActiveTab = 1;
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Upload a local .map file to the server. "
                "Coming in a follow-up — the upload packet protocol "
                "isn't wired yet.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Random")) {
            s_chooseMapActiveTab = 2;
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Random map generation — coming soon.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    /* Action bar: Cancel restores the prior map (Phase 2 will wire),
     * Close keeps the current selection. */
    ImGui::Separator();
    if (ImGui::Button("Cancel")) {
        /* TODO Phase 2: re-send s_chooseMapPrevName as a SET_MAP packet
         * so the broadcast unwinds. For now we just close — phase 1
         * never sent anything in the first place, so there's nothing to
         * undo on the server. */
        s_chooseMapOpen = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        s_chooseMapOpen = false;
    }

    ImGui::End();
    /* If the user clicked the title-bar X, ImGui flips `open` to false. */
    if (!open) s_chooseMapOpen = false;
}

static void lobbySendRemoveBot(ClientSim *cs, uint8_t slot) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        serverSimRemoveBot(sim, slot);
        if (slot < MAX_TANKS) s_botNameOverridden[slot] = false;
        serverSimPublishLobbySlot(sim, slot);
        return;
    }
    clientSimNetSendRemoveBot(cs, slot);
}

/* Move `targetSlot` to `teamNumber`. The host (or any client with
 * lobbyClientMayEdit authority on the server) can move any player;
 * non-authorised clients can only move themselves. The server gates
 * this — we just send the request. */
static void lobbySendTeamSet(ClientSim *cs,
                             uint8_t targetSlot, uint8_t teamNumber) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || targetSlot >= MAX_TANKS) return;
        serverSimSetTeam(sim, targetSlot, teamNumber);
        serverSimPublishLobbySlot(sim, targetSlot);
        return;
    }
    /* MP teamSet: targetSlot == self only for now (server gates anyway). */
    clientSimNetSendTeamSet(cs, teamNumber);
}

/* Update a bot's per-slot config (difficulty / personality / name
 * override). Mirrors the server-side PACKET_LOBBY_BOT_CONFIG handler.
 * In single-player we write directly into spServerSim->botConfigs and
 * (if name supplied) update the players struct so the lobby slot's
 * playerName changes as well. */
static void lobbySendBotConfig(ClientSim *cs,
                               uint8_t slot,
                               uint8_t difficulty, uint8_t personality,
                               const char *name) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || slot >= MAX_TANKS) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        if (difficulty > 2 || personality > 3) return;
        if (!serverSimGetLobbyPlayer(sim, slot)->isBot) return;
        {
            LobbyBotConfig *bc = serverSimGetBotConfigMut(sim, slot);
            if (bc) {
                bc->difficulty  = difficulty;
                bc->personality = personality;
            }
        }
        if (name && name[0] != '\0') {
            serverSimRenameBotSlot(sim, slot, name);
        }
        /* Publish bot config change AND the slot's new state (playerName
         * may have changed). */
        serverSimPublishLobbyBotConfig(sim, slot);
        serverSimPublishLobbySlot(sim, slot);
        return;
    }
    clientSimNetSendLobbyBotConfig(cs, slot, difficulty, personality, name);
}

/* Change which Lua brain script a lobby bot uses. SP path mutates the
 * server sim directly; MP path goes through PACKET_LOBBY_SET_BOT_BRAIN. */
static void lobbySendSetBotBrain(ClientSim *cs,
                                 uint8_t slot, const char *brainPath) {
    if (!brainPath) brainPath = "";
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || slot >= MAX_TANKS) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        if (!serverSimGetLobbyPlayer(sim, slot)->isBot) return;
        serverSimSwitchBotBrain(sim, slot, brainPath);
        serverSimPublishLobbyBotBrain(sim, slot);
        return;
    }
    clientSimNetSendLobbySetBotBrain(cs, slot, brainPath);
}

/* Clear a team's metadata (color/name/pool back to defaults).
 * Mirrors PACKET_LOBBY_TEAM_CLEAR. Only called when the team is
 * empty — caller already gates on memberCount == 0. */
static void lobbySendTeamClear(ClientSim *cs, uint8_t teamId) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
        {
            TeamMetadata *t = serverSimGetTeamMetaMut(sim, teamId);
            if (t) memset(t, 0, sizeof(*t));
        }
        serverSimPublishLobbyTeamMeta(sim, teamId);
        return;
    }
    clientSimNetSendLobbyTeamClear(cs, teamId);
}

/* Update a team's naming pool. Mirrors the per-team naming dropdown
 * which previously sent transportUdpClientSendLobbyTeamMeta directly.
 * Single-player path mutates spServerSim->teams[teamId] in-place AND
 * re-rolls every bot on the team whose name wasn't manually
 * overridden so the new pool's vibe applies immediately. */
static void lobbySendTeamPool(ClientSim *cs,
                              uint8_t teamId, uint8_t namingPool,
                              const char *teamName) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
        TeamMetadata *t = serverSimGetTeamMetaMut(sim, teamId);
        if (!t) return;
        t->in_use = 1;
        t->namingPool = namingPool;
        if (teamName && teamName[0]) {
            strncpy(t->name, teamName, LOBBY_TEAM_NAME_LEN - 1);
            t->name[LOBBY_TEAM_NAME_LEN - 1] = '\0';
        }
        serverSimPublishLobbyTeamMeta(sim, teamId);

        /* Rename bots on this team whose names weren't overridden by
         * the host. We pick names sequentially from the new pool,
         * each call's used-list including everyone already on the
         * lobby plus the names we've assigned in this loop, so the
         * picker doesn't collide with itself. */
        const char *usedNames[MAX_TANKS];
        int usedCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            if (serverSimIsPlayerConnected(sim, i)) {
                usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
            }
        }
        for (int slot = 0; slot < MAX_TANKS; slot++) {
            if (!serverSimIsPlayerConnected(sim, slot)) continue;
            if (!serverSimGetLobbyPlayer(sim, slot)->isBot) continue;
            if (serverSimGetLobbyPlayer(sim, slot)->teamNumber != teamId) continue;
            if (s_botNameOverridden[slot]) continue;

            char pickBuf[32];
            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             pickBuf, sizeof(pickBuf));
            lobbySendBotConfig(cs, (uint8_t)slot,
                               clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)),
                               clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)),
                               pickBuf);
            /* Replace this slot's entry in usedNames so subsequent
             * picks see the updated name (avoids picking the same
             * name twice within the loop). */
            for (int u = 0; u < usedCount; u++) {
                if (usedNames[u] == clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName) {
                    usedNames[u] = clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName;
                    break;
                }
            }
        }
        return;
    }
    /* MP path. */
    {
        /* If the client hasn't received the team's actual metadata yet
         * (e.g. fresh join), fall back to a sensible per-teamId default
         * color rather than the zero-initialised value, which would
         * paint every team RED until the server echoed the real one
         * back. Server is authoritative — TEAM_META_CHG will correct
         * this on the next round-trip. */
        uint8_t color = clientSimGetLobbyTeamColor(cs, (BYTE)(teamId));
        if (!clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))) {
            color = (uint8_t)((teamId - 1) & 7);
        }
        clientSimNetSendLobbyTeamMeta(cs, teamId, color, namingPool, teamName);

        /* The server can't rename existing bots when the pool changes
         * because the per-pool name table lives only on the client
         * (lobby_bot_pools.h). Fan out one BOT_CONFIG per bot on this
         * team with a name drawn from the newly-selected pool — same
         * behaviour as the SP branch above, just over the wire. */
        const char *usedNames[MAX_TANKS];
        int usedCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected &&
                clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]) {
                usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
            }
        }
        /* Stash the assigned names locally so subsequent loop
         * iterations don't pick a name we just handed out. */
        char assigned[MAX_TANKS][32];
        for (int slot = 0; slot < MAX_TANKS; slot++) {
            if (!clientSimGetLobbySlot(cs, (BYTE)(slot))->connected) continue;
            if (!clientSimGetLobbySlot(cs, (BYTE)(slot))->isBot)       continue;
            if (clientSimGetLobbySlot(cs, (BYTE)(slot))->teamNumber != teamId) continue;
            if (slot < MAX_TANKS && s_botNameOverridden[slot]) continue;

            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             assigned[slot], sizeof(assigned[slot]));
            clientSimNetSendLobbyBotConfig(cs, (uint8_t)slot,
                clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)),
                clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)),
                assigned[slot]);
            usedNames[usedCount++] = assigned[slot];
        }
    }
}

/* Game-settings dispatcher. settingType is one of LST_* (netpacks.h);
 * payload is 1 or 2 bytes per server-side parser. Mirrors what
 * transport_udp_server.c::PACKET_LOBBY_SET_SETTING does to spServerSim
 * for the single-player path, then re-syncs so the lobby UI's read
 * of cs->lobby* sees the new value on the next frame. Without the
 * sync the checkbox/radio flashes for one frame and reverts. */
static void lobbySendSetting(ClientSim *cs,
                             uint8_t settingType,
                             const uint8_t *value, uint8_t valueLen) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        switch (settingType) {
            case 1 /* LST_GAME_TYPE */:
                /* gameType enum is 1..3 (Open / Tournament / Strict).
                 * The wire carries the raw enum value. */
                if (valueLen == 1 && value[0] >= 1 && value[0] <= 3) {
                    serverSimSetGameType(sim, (gameType)value[0]);
                }
                break;
            case 2 /* LST_HIDDEN_MINES */:
                if (valueLen == 1) serverSimSetHiddenMines(sim, value[0] != 0);
                break;
            case 3 /* LST_AI_POLICY */:
                if (valueLen == 1 && value[0] <= 3) {
                    serverSimSetAiPolicy(sim, value[0]);
                    serverSimSetBotAiType(sim, (aiType)value[0]);
                    /* Switching to "No computer tanks" should clear every
                     * existing bot — otherwise the lobby keeps showing
                     * bots that the host explicitly disabled. Iterate all
                     * slots (bots can occupy any) and remove unconditionally;
                     * publish each slot so the UI updates. */
                    if ((aiType)value[0] == aiNone) {
                        for (BYTE i = 0; i < MAX_TANKS; i++) {
                            if (serverSimIsBot(sim, i)) {
                                serverSimRemoveBot(sim, i);
                                serverSimPublishLobbySlot(sim, i);
                            }
                        }
                    }
                }
                break;
            case 4 /* LST_TIME_LIMIT */: {
                if (valueLen == 1) {
                    bool tl = value[0] != 0;
                    serverSimSetTimeLimit(sim, tl);
                    /* When the host turns the time limit off, gameLength
                     * goes to TIME_UNLIMITED; on, derive from current
                     * timeMinutes mirror. */
                    if (!tl) {
                        serverSimSetGameLength(sim, -1);
                    } else if (serverSimGetTimeMinutes(sim) > 0) {
                        serverSimSetGameLength(sim,
                            (int32_t)serverSimGetTimeMinutes(sim) * 60 * 50);
                    }
                }
                break;
            }
            case 5 /* LST_TIME_MINUTES */: {
                if (valueLen == 2) {
                    uint16_t mins = (uint16_t)((value[0] << 8) | value[1]);
                    serverSimSetTimeMinutes(sim, mins);
                    if (serverSimGetTimeLimit(sim)) {
                        serverSimSetGameLength(sim,
                            (int32_t)mins * 60 * 50);
                    }
                }
                break;
            }
            case 6 /* LST_AUTO_LOCK_ON_GAME */:
                if (valueLen == 1) serverSimSetAutoLockOnGameStart(sim, value[0] != 0);
                break;
        }
        serverSimPublishLobbySettings(sim);
        return;
    }
    clientSimNetSendLobbySetting(cs, settingType, value, valueLen);
}

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
static SDL_Texture *s_iconSettings = nullptr;
static SDL_Texture *s_iconBotCpuGreen  = nullptr;
static SDL_Texture *s_iconBotCpuRed    = nullptr;
static bool         s_iconsAttempted = false;

/* Tank sprite used as the team identity badge in the lobby header.
 * Loaded once on first lobby render; tinted with the team color via
 * a darkened semi-transparent overlay. */
static SDL_Texture *s_tankSelf04 = nullptr;
static SDL_Texture *s_tankEvil04 = nullptr;
static SDL_Texture *s_tankGood04 = nullptr;
static bool         s_tankSelf04Attempted = false;

/* stb_image entry points — defined in C, declared with C linkage
 * so the C++ linker finds them. Mirror of how imgui_welcome.cpp
 * pulls them in (its include of stb_image.h provides the same). */
extern "C" {
    unsigned char *stbi_load_from_memory(const unsigned char *, int,
                                         int *, int *, int *, int);
    void stbi_image_free(void *);
}

/* Minimal PNG loader (mirror of imgui_welcome.cpp's loadPng) — uses
 * SDL_IOFromFile + stb_image so it works on Android APK assets and
 * regular filesystem alike. */
static SDL_Texture *loadLobbyPng(SDL_Renderer *renderer, const char *filename) {
    SDL_IOStream *io = SDL_IOFromFile(filename, "rb");
    if (!io) {
        char path[512];
        SDL_snprintf(path, sizeof(path), "data/%s", filename);
        io = SDL_IOFromFile(path, "rb");
    }
    if (!io) return nullptr;
    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return nullptr; }
    unsigned char *buf = (unsigned char *)SDL_malloc((size_t)size);
    if (!buf) { SDL_CloseIO(io); return nullptr; }
    SDL_ReadIO(io, buf, (size_t)size);
    SDL_CloseIO(io);
    int w, h, ch;
    unsigned char *data = stbi_load_from_memory(buf, (int)size, &w, &h, &ch, 4);
    SDL_free(buf);
    if (!data) return nullptr;
    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, data, w * 4);
    if (!surf) { stbi_image_free(data); return nullptr; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_DestroySurface(surf);
    stbi_image_free(data);
    return tex;
}

static SDL_Texture *getTankSelf04Texture(SDL_Renderer *renderer) {
    if (s_tankSelf04Attempted) return s_tankSelf04;
    s_tankSelf04Attempted = true;
    s_tankSelf04 = loadLobbyPng(renderer, "svg/tank_self_04.png");
    if (s_tankSelf04) {
        SDL_SetTextureScaleMode(s_tankSelf04, SDL_SCALEMODE_LINEAR);
    }
    return s_tankSelf04;
}

/* Red enemy tank — used next to player rows on teams different from
 * the local player's. Loaded lazily on first use, same pattern as
 * getTankSelf04Texture. */
static SDL_Texture *getTankEvil04Texture(SDL_Renderer *renderer) {
    static bool attempted = false;
    if (attempted) return s_tankEvil04;
    attempted = true;
    s_tankEvil04 = loadLobbyPng(renderer, "svg/tank_evil_04.png");
    if (s_tankEvil04) {
        SDL_SetTextureScaleMode(s_tankEvil04, SDL_SCALEMODE_LINEAR);
    }
    return s_tankEvil04;
}

/* "Good" ally tank (yellow tone) — used to distinguish the local
 * player's own row from the rest of their team. Falls back to
 * tank_self_04 if the asset isn't there. */
static SDL_Texture *getTankGood04Texture(SDL_Renderer *renderer) {
    static bool attempted = false;
    if (attempted) return s_tankGood04;
    attempted = true;
    s_tankGood04 = loadLobbyPng(renderer, "svg/tank_good_04.png");
    if (s_tankGood04) {
        SDL_SetTextureScaleMode(s_tankGood04, SDL_SCALEMODE_LINEAR);
    }
    return s_tankGood04;
}

static void loadStatusIconsOnce(SDL_Renderer *renderer, float scale) {
    if (s_iconsAttempted) return;
    s_iconsAttempted = true;

    int iconPx = (int)(18.0f * scale);
    if (iconPx < 16) iconPx = 16;

    struct {
        SDL_Texture **target;
        const char   *relPath;
    } icons[] = {
        { &s_iconSuccess,  "data/ui/dialog-success.svg" },
        { &s_iconError,    "data/ui/dialog-error.svg" },
        { &s_iconInfo,     "data/ui/dialog-info.svg" },
        { &s_iconSettings, "data/ui/settings.svg" },
        { &s_iconBotCpuGreen, "data/ui/bot-cpu-green.svg" },
        { &s_iconBotCpuRed,   "data/ui/bot-cpu-red.svg" },
    };

    for (int i = 0; i < (int)(sizeof(icons) / sizeof(icons[0])); i++) {
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
static void renderBotAiConfig(ClientSim *cs,
                              int slot, int teamId, float s);

/* ── Layout A — team-grouped player list ──────────────────────────
 * Renders players grouped under team headers with color tints from
 * WbTheme. Replaces the flat 5-column table with the mockup's
 * "team containers" model. Sized to fit inside the calling child
 * window. Returns nothing — purely UI. */
static void renderLockBadge(void);

/* Compact "Allow New Players:  [ ] Now   [ ] During game" row. Host
 * only and multiplayer only (single-player has no UDP listener). Used
 * to live inside renderTeamGroupedPlayers; hoisted to the parent so
 * the PlayerPanel and MapPanel top edges stay aligned. */
static void renderAllowNewPlayersRow(ClientSim *cs,
                                     int myPlayerNum, float s) {
    bool isHost = (myPlayerNum == 0);
    bool isLocalAdmin = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                        (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                         & PLAYER_FLAG_ADMIN));
    bool effectiveHost = isHost || clientSimGetLobbyOpenHost(cs) || isLocalAdmin;
    if (!effectiveHost) return;
    if (!clientSimHasTransport(cs) || clientSimIsSinglePlayer(cs)) return;

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Allow New Players:");
    /* TODO: server's allowNewPlayers state isn't carried by
     * CTRL_LOBBY_SETTINGS — show the host's last-clicked intent. */
    bool allowJoin = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Now##allowNow", &allowJoin)) {
        clientSimNetSendLockToggle(cs, allowJoin);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Accept new join requests right now while the lobby is open.");
    }
    bool autoLockLocked = (clientSimGetLobbyServerLocks(cs) & 0x10) != 0;
    bool duringGame = !clientSimGetLobbyAutoLockOnGameStart(cs);
    if (autoLockLocked) ImGui::BeginDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox("During game##allowDuring", &duringGame)) {
        uint8_t v = duringGame ? 0 : 1;  /* invert */
        lobbySendSetting(cs, 6 /*LST_AUTO_LOCK_ON_GAME*/, &v, 1);
    }
    if (autoLockLocked) ImGui::EndDisabled();
    if (autoLockLocked) {
        ImGui::SameLine(0.0f, 4.0f * s);
        renderLockBadge();
    }
    if (ImGui::IsItemHovered() && !autoLockLocked) {
        ImGui::SetTooltip(
            "Keep accepting new players after the game has started.");
    }
}

static void renderTeamGroupedPlayers(ClientSim *cs,
                                     int myPlayerNum, float s, bool isHost) {
    /* Lazy-load the badge / bot-cpu icons. Used to be done inside
     * renderConnectivityBadge, but we now skip that in SP / LAN-only
     * mode where the badge has nothing to report — the bot-cpu PNGs
     * still need to come up though, so trigger it here too. The
     * helper is idempotent (s_iconsAttempted guard). */
    {
        SDL_Renderer *r = sdl3DrawGetRenderer();
        if (r) loadStatusIconsOnce(r, s);
    }

    /* Helper to count members per team for header strings. */
    int memberCount[16] = {0};
    int botCount[16]    = {0};
    int unassignedCount = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!clientSimGetLobbySlot(cs, (BYTE)(i))->connected) continue;
        uint8_t t = clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber;
        if (t == 0) {
            unassignedCount++;
        } else if (t < 16) {
            memberCount[t]++;
            if (clientSimGetLobbySlot(cs, (BYTE)(i))->isBot) botCount[t]++;
        }
    }

    /* Walk teams 1..15, render those with members. Then unassigned. */
    bool isLocalAdmin = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                         (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                          & PLAYER_FLAG_ADMIN));
    bool effectiveHost = isHost || clientSimGetLobbyOpenHost(cs) || isLocalAdmin;

    /* "Allow new players" row is rendered by the caller above the panels
     * so the PlayerPanel and MapPanel top edges stay aligned in Y.
     * "+ Add Team" lives in the footer below the teams list (see
     * the bottom of this function). */
    (void)effectiveHost;  /* still used by the footer "Add Team" below */
    for (int teamId = 1; teamId < 16; teamId++) {
        /* Teams 1 and 2 are always rendered (the lobby's two default
         * sides) — the host always has somewhere to drop the first
         * bot. Teams 3..15 render when they have members OR when the
         * host has explicitly added them via "+ Add Team"
         * (lobbyTeamInUse=1). Without the in_use check, freshly added
         * teams would vanish on the same frame because they have no
         * members yet. */
        bool persistTeam = (teamId == 1 || teamId == 2)
                        || clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId));
        if (!persistTeam && memberCount[teamId] == 0) continue;

        /* Team color from theme; falls back to gray for un-themed teams. */
        ImU32 tc;
        uint8_t colorIdx = clientSimGetLobbyTeamColor(cs, (BYTE)(teamId));
        if (clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId)) && colorIdx < 8) {
            tc = g_theme->teamColors[colorIdx];
        } else {
            /* Default per-team color: cycle through palette by teamId. */
            tc = g_theme->teamColors[(teamId - 1) & 7];
        }

        /* Team child window — uses the default ChildBg from the theme.
         * Team color is reserved for the header strip only (see below)
         * so the body stays neutral and the team identity reads as a
         * banner rather than a flood fill. */
        char teamFrame[32];
        SDL_snprintf(teamFrame, sizeof(teamFrame), "##team%d", teamId);
        ImGui::BeginChild(teamFrame,
                          ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

        /* Color header strip — fills the header row's full width with
         * the team color at low alpha, drawn under the header widgets
         * via the window draw list. Renders before the swatch so the
         * widgets sit on top of it. */
        ImVec2 stripStart = ImGui::GetCursorScreenPos();
        float  stripH     = ImGui::GetTextLineHeightWithSpacing() + 4.0f * s;
        float  contentW   = ImGui::GetContentRegionAvail().x;
        ImU32  stripCol   = (tc & 0x00FFFFFF) | (0x28 << 24);
        ImGui::GetWindowDrawList()->AddRectFilled(
            stripStart,
            ImVec2(stripStart.x + contentW, stripStart.y + stripH),
            stripCol, 2.0f);

        /* Team identity badge moved off the team header; each player
         * row now renders a green (self/ally) or red (enemy) tank
         * icon to the left of its name, so team affiliation reads
         * per-row instead of just in the header.  Pad a few pixels
         * before the "Team N" label so it doesn't hug the left edge. */
        ImGui::Dummy(ImVec2(6.0f * s, 0));
        ImGui::SameLine(0.0f, 0.0f);

        /* Team name — always "Team N", non-editable. Renaming was
         * dropped per UX feedback; the number is enough identity
         * alongside the colored tank badge.  AlignTextToFramePadding
         * vertically centers both text spans with the frame-padded
         * widgets on the same row (matches the "Bot Naming:" label). */
        char defaultName[16];
        SDL_snprintf(defaultName, sizeof(defaultName), "Team %d", teamId);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tc));
        ImGui::Text("%s", defaultName);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("· %d player%s%s%s",
            memberCount[teamId], memberCount[teamId] == 1 ? "" : "s",
            botCount[teamId] > 0 ? " · " : "",
            botCount[teamId] > 0 ? (botCount[teamId] == 1 ? "1 bot" : "bots") : "");

        /* "Join Team" — moves the local player to this team. Available
         * to every client regardless of permissions (you can always
         * move yourself) and only rendered when you're not already on
         * this team. */
        if (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
            clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->teamNumber != teamId) {
            ImGui::SameLine();
            char joinId[24];
            SDL_snprintf(joinId, sizeof(joinId), "Join##j%d", teamId);
            if (ImGui::SmallButton(joinId)) {
                lobbySendTeamSet(cs,
                                 (uint8_t)myPlayerNum, (uint8_t)teamId);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Move yourself to Team %d.", teamId);
            }
        }

        /* Per-team "+ Bot" button (always visible to the host) plus an
         * optional "Bot Naming:" pool dropdown (only when the team has
         * at least one bot — picking a pool before any bot exists has
         * nothing to apply to) and an optional X (clear) button on
         * the right edge.
         *
         * The X is shown only for teams 3..15 (teams 1 and 2 are
         * persistent and never removable) and only when the team has
         * no humans on it — clearing a team with humans would orphan
         * them. We still reserve its width even when hidden so the
         * "+ Bot" button stays at the same X coordinate across teams
         * that do/don't render an X. */
        if (effectiveHost) {
            const float comboW   = 200.0f * s;
            /* Gap between the Bot Naming combo and the Add Bot button —
             * just a normal widget-pair spacing so the dropdown sits
             * directly next to Add Bot rather than being pushed off to
             * the middle of the row. */
            const float namingShift = 6.0f * s;
            const float botBtnW  = 95.0f * s;
            const float xBtnW    = 22.0f * s;
            const float labelW   = ImGui::CalcTextSize("Bot Naming:").x;
            const float gap      = 6.0f * s;
            /* Bots are only addable when the server's AI policy allows
             * it (lobbyAiType != aiNone) AND the server has at least
             * one brain on disk to assign. Both fields are mirrored
             * from the server dynamically, so the button and the
             * Bot Naming controls disappear / reappear without a
             * reconnect when -ai policy or brains/ changes. */
            bool botsAllowed = (clientSimGetLobbyAiType(cs) != 0) &&
                               (clientSimGetLobbyBrainList(cs)->count > 0);
            bool showNaming = botsAllowed && botCount[teamId] > 0;
            int humanCount = memberCount[teamId] - botCount[teamId];
            bool showXBtn   = (teamId >= 3) && (humanCount == 0);
            /* Always reserve the X width so "+ Bot" sits at the same
             * X position across teams with/without an X. */
            /* When showing the Bot Naming controls, leave a wider
             * gap (namingShift) between the dropdown and the Add Bot
             * button so the label + dropdown sit further left and
             * the dropdown has room to be wider. */
            float effBotBtnW = botsAllowed ? botBtnW : 0.0f;
            float effBotGap  = botsAllowed ? gap    : 0.0f;
            float groupW = effBotBtnW + effBotGap + xBtnW
                         + (showNaming
                            ? labelW + gap + comboW + namingShift
                            : 0);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - groupW);
            int curPool = clientSimGetLobbyTeamPool(cs, (BYTE)(teamId));
            if (curPool < 0 || curPool >= lobbyBotPoolCount()) curPool = 0;
            if (showNaming) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Bot Naming:");
                ImGui::SameLine(0.0f, gap);
                char poolId[16];
                SDL_snprintf(poolId, sizeof(poolId), "##pool%d", teamId);
                ImGui::SetNextItemWidth(comboW);
                if (ImGui::BeginCombo(poolId, lobbyBotPoolLabel(curPool))) {
                    for (int p = 0; p < lobbyBotPoolCount(); p++) {
                        bool sel = (p == curPool);
                        /* Disable pools already claimed by some other
                         * in_use team — server enforces uniqueness; the
                         * UI mirrors it so users see what's available. */
                        bool takenElsewhere = false;
                        for (int ot = 1; ot < MAX_TANKS; ot++) {
                            if (ot == teamId) continue;
                            if (clientSimGetLobbyTeamInUse(cs, (BYTE)ot) &&
                                clientSimGetLobbyTeamPool(cs, (BYTE)ot) == p) {
                                takenElsewhere = true;
                                break;
                            }
                        }
                        ImGuiSelectableFlags flags = takenElsewhere
                            ? ImGuiSelectableFlags_Disabled : 0;
                        if (ImGui::Selectable(lobbyBotPoolLabel(p), sel, flags)) {
                            const char *nameForMeta = clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))
                                ? clientSimGetLobbyTeamName(cs, (BYTE)(teamId)) : defaultName;
                            lobbySendTeamPool(cs, (uint8_t)teamId,
                                              (uint8_t)p, nameForMeta);
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine(0.0f, namingShift);
            }
            /* Empty-label button used as a click target; the icon
             * and the "Add Bot" text are drawn ourselves on top via
             * WindowDrawList. This lets us shift the text right by
             * a precise pixel offset so the centered label clears
             * the overlaid bot-cpu glyph cleanly. */
            bool addBtnClicked = false;
            ImVec2 addBtnPos = ImGui::GetCursorScreenPos();
            bool addBtnPending = lobbyAddBotPending(cs);
            if (botsAllowed) {
                char addId[24];
                SDL_snprintf(addId, sizeof(addId), "##ab%d", teamId);
                if (addBtnPending) ImGui::BeginDisabled();
                addBtnClicked = ImGui::Button(addId, ImVec2(botBtnW, 0));
                if (addBtnPending) ImGui::EndDisabled();
            {
                float btnH = ImGui::GetFrameHeight();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                const char *addLbl = "Add Bot";
                ImVec2 textSz = ImGui::CalcTextSize(addLbl);
                ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
                /* Centered horizontally, then shifted right by 10px
                 * so it never collides with the icon on the left. */
                ImVec2 textPos(addBtnPos.x + (botBtnW - textSz.x) * 0.5f
                                            + 10.0f * s,
                               addBtnPos.y + (btnH - textSz.y) * 0.5f);
                dl->AddText(textPos, textCol, addLbl);
            }
            /* Green for "your team" buttons, red for the others —
             * matches the bot-cpu glyph rendered per row. */
            uint8_t myTeamHdr = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS)
                                ? clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->teamNumber : 0;
            SDL_Texture *addBtnIcon = (myTeamHdr != 0 && teamId == myTeamHdr)
                                      ? s_iconBotCpuGreen
                                      : s_iconBotCpuRed;
            if (addBtnIcon) {
                float btnH    = ImGui::GetFrameHeight();
                float imgSz   = ImGui::GetFontSize();
                /* Tucked a few pixels in from the left edge so the
                 * icon doesn't hug the button border. The button is
                 * wide enough that the centered "Add Bot" label
                 * clears the icon naturally. */
                ImVec2 iconPos(addBtnPos.x + 8.0f * s,
                               addBtnPos.y + (btnH - imgSz) * 0.5f);
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)addBtnIcon,
                    iconPos,
                    ImVec2(iconPos.x + imgSz, iconPos.y + imgSz));
            }
            }  /* end botsAllowed AddBot block */
            if (addBtnClicked) {
                /* When the first bot is added to a team, randomize
                 * the pool so the name comes from a varied source
                 * instead of always pool 0. Teams 1/2 are pre-marked
                 * in_use at server init, so gating on in_use never
                 * fired — gate on "no bots yet" instead. Subsequent
                 * bots in the same team reuse the chosen pool; the
                 * host can still override via the dropdown. */
                int effectivePool = curPool;
                if (botCount[teamId] == 0 && lobbyBotPoolCount() > 0) {
                    effectivePool = rand() % lobbyBotPoolCount();
                    const char *nameForMeta = clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))
                        ? clientSimGetLobbyTeamName(cs, (BYTE)(teamId)) : defaultName;
                    lobbySendTeamPool(cs, (uint8_t)teamId,
                                      (uint8_t)effectivePool, nameForMeta);
                }
                lobbySendAddBotDebounced(cs, effectivePool, (uint8_t)teamId);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Add a bot to Team %d, named from the selected pool.",
                                  teamId);
            }
            /* Pin the X to the right edge so it lines up across teams.
             * Use ImGui::CloseButton (imgui_internal.h) — the same
             * widget ImGui uses for window-close buttons. Renders a
             * proper crossed line "×" with hover + active styling,
             * sized to the current font. */
            if (botsAllowed) {
                ImGui::SameLine(0.0f, gap);
            }
            ImGui::AlignTextToFramePadding();
            if (showXBtn) {
                char rmStrId[24];
                SDL_snprintf(rmStrId, sizeof(rmStrId), "##rmt%d", teamId);
                ImGuiID rmId = ImGui::GetID(rmStrId);
                ImVec2 closePos = ImGui::GetCursorScreenPos();
                /* Vertically center the close button within the row
                 * height. CloseButton draws a FontSize × FontSize box;
                 * frame height is taller. */
                float frameH = ImGui::GetFrameHeight();
                float fs = ImGui::GetFontSize();
                closePos.y += (frameH - fs) * 0.5f;
                if (ImGui::CloseButton(rmId, closePos)) {
                    /* Clear the team's metadata and any bot members. */
                    for (int i = 0; i < MAX_TANKS; i++) {
                        if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected
                            && clientSimGetLobbySlot(cs, (BYTE)(i))->isBot
                            && clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber == teamId) {
                            lobbySendRemoveBot(cs, (uint8_t)i);
                        }
                    }
                    lobbySendTeamClear(cs, (uint8_t)teamId);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Remove Team %d (and its bots).", teamId);
                }
            } else {
                /* Hold the slot so + Bot's X position is stable. */
                ImGui::Dummy(ImVec2(xBtnW, 0));
            }
        }

        /* Member rows — proper ImGui Table.
         *
         * BordersInnerH gives us the inter-row dividers natively
         * (one between each row, none above the first / below the
         * last). RowBg + TableSetBgColor lets us paint the "you"
         * row a darker tint without manually computing rectangles.
         * SizingStretchProp makes the name column absorb leftover
         * space while the icon columns stay fixed.
         *
         * Column layout: [tank | name+icons | ping | ready | X]. */
        SDL_Renderer *r = sdl3DrawGetRenderer();
        uint8_t myTeam = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS)
                         ? clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->teamNumber : 0;
        char tableId[32];
        SDL_snprintf(tableId, sizeof(tableId), "##members%d", teamId);
        ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH
                                   | ImGuiTableFlags_RowBg
                                   | ImGuiTableFlags_SizingStretchProp
                                   | ImGuiTableFlags_PadOuterX
                                   /* NoClip so the expanded bot's
                                    * AiConfig form (rendered in col 0)
                                    * is allowed to extend visually
                                    * across the other columns. */
                                   | ImGuiTableFlags_NoClip;
        if (ImGui::BeginTable(tableId, 7, tableFlags)) {
            /* Wide column 0 so the tank icon sits well inside the
             * team panel (not crammed against the left edge). The
             * icon is positioned with a leading SetCursorPosX nudge
             * inside the cell.
             *
             * Column 1 hosts the per-row identity icons (country
             * flag + platform/WBN/Steam badges for humans, the
             * bot-cpu glyph for bots) — extracted from the name
             * column so the name text always starts at a fixed X.
             *
             * Two stretch columns (name + spacer) with weights 3:1
             * park the ping/gear column at roughly 3/4 across the
             * row instead of flush against the ready/X cluster. */
            ImGui::TableSetupColumn("##tank",   ImGuiTableColumnFlags_WidthFixed, 60.0f * s);
            ImGui::TableSetupColumn("##icons",  ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
            ImGui::TableSetupColumn("##name",   ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("##ping",   ImGuiTableColumnFlags_WidthFixed, 50.0f * s);
            ImGui::TableSetupColumn("##spacer", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("##ready",  ImGuiTableColumnFlags_WidthFixed, 80.0f * s);
            ImGui::TableSetupColumn("##x",      ImGuiTableColumnFlags_WidthFixed, 22.0f * s);

            /* Drive striping ourselves (per-player, not per-table-row)
             * so the bot's expanded AiConfig sub-row inherits the same
             * background as its parent bot row — keeps the odd/even
             * stripe stable when the gear is toggled. */
            const ImU32 stripeA = ImGui::GetColorU32(ImGuiCol_TableRowBg);
            const ImU32 stripeB = ImGui::GetColorU32(ImGuiCol_TableRowBgAlt);
            int playerIdx = 0;

            for (int i = 0; i < MAX_TANKS; i++) {
                if (!clientSimGetLobbySlot(cs, (BYTE)(i))->connected) continue;
                if (clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber != teamId) continue;

                bool isMe   = (i == myPlayerNum);
                bool isBot  = clientSimGetLobbySlot(cs, (BYTE)(i))->isBot;
                bool isSelf = isMe;
                bool isAlly = (myTeam != 0 && clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber == myTeam);

                /* Row height = the tallest single widget we render
                 * in the row plus a touch of breathing room. Widgets
                 * are positioned per-cell via absolute Y so each one
                 * is centered on the row's vertical midline regardless
                 * of its own height. */
                const float tankSz  = 18.0f * s;
                const float closeSz = ImGui::GetFontSize();
                const float rowH    = ImMax(ImGui::GetFrameHeight(),
                                            ImMax(tankSz, 22.0f * s));

                ImGui::TableNextRow(0, rowH);
                const ImU32 rowStripe = (playerIdx & 1) ? stripeB : stripeA;
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowStripe);
                if (isMe) {
                    /* Translucent dark tint that highlights the local
                     * player's row, painted on top of the stripe. */
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                           IM_COL32(0, 0, 0, 64));
                }
                playerIdx++;

                /* Per-cell vertical centering: capture the row's top
                 * Y on entering each cell, then set cursor.y to
                 * rowTopY + (rowH - widgetH)/2 immediately before
                 * every widget. SameLine() resets cursor.y to the
                 * line's min Y; we override it again per widget so
                 * widgets of different heights (flags 11px, icons
                 * 14px, text ~13px, tank 18px) each end up centered
                 * on the row midline. */
                float rowTopY;
                auto cyAbs = [&](float h) {
                    float y = rowTopY + (rowH - h) * 0.5f;
                    if (y < rowTopY) y = rowTopY;
                    ImGui::SetCursorPosY(y);
                };
                /* Text-specific centering: ImGui's text line box has a
                 * couple of pixels of "ascent whitespace" above the
                 * cap line and an almost-empty descender below, so a
                 * pure line-height-based centering makes the visible
                 * glyph mass sit too low (~8px top / ~5px bottom in a
                 * 22px row). Bias the cursor up by ~12% of font size
                 * (about 1.5–2 px) so the visible glyphs end up
                 * centered. */
                auto cyTextAbs = [&]() {
                    float h = ImGui::GetTextLineHeight();
                    float bias = ImGui::GetFontSize() * 0.12f;
                    float y = rowTopY + (rowH - h) * 0.5f - bias;
                    if (y < rowTopY) y = rowTopY;
                    ImGui::SetCursorPosY(y);
                };

                /* ── Column 0: drag handle + tank icon ─────────────── */
                ImGui::TableSetColumnIndex(0);
                rowTopY = ImGui::GetCursorPosY();
                /* Capture the row's screen-Y top while we're at it so
                 * later columns can do an absolute-rect hover test
                 * (gear visibility, etc.). */
                float rowTopScreenY = ImGui::GetCursorScreenPos().y;

                /* Drag handle — drives the "drag a player onto a team"
                 * flow. Authority: anyone may drag themselves; the host
                 * (or openHost / admin) may drag anyone (incl. bots).
                 * Server re-validates on PACKET_LOBBY_TEAM_SET.
                 * Rendered as a 4-arrow "move" cross centered in the
                 * row, before the tank icon. */
                bool canDragThis = (i == myPlayerNum) || effectiveHost;
                if (canDragThis) {
                    const float handleS = 14.0f * s;
                    cyAbs(handleS);
                    ImVec2 hPos = ImGui::GetCursorScreenPos();
                    char hId[24];
                    SDL_snprintf(hId, sizeof(hId), "##drag%d", i);
                    ImGui::InvisibleButton(hId, ImVec2(handleS, handleS));
                    ImU32 lineCol = ImGui::IsItemHovered()
                        ? IM_COL32(230, 230, 230, 230)
                        : IM_COL32(140, 140, 140, 200);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    float cx = hPos.x + handleS * 0.5f;
                    float cy = hPos.y + handleS * 0.5f;
                    float a  = handleS * 0.45f;
                    float h  = handleS * 0.20f;
                    float th = 1.2f;
                    /* Crossbars. */
                    dl->AddLine(ImVec2(cx - a, cy), ImVec2(cx + a, cy),
                                lineCol, th);
                    dl->AddLine(ImVec2(cx, cy - a), ImVec2(cx, cy + a),
                                lineCol, th);
                    /* Four arrowheads, one per crossbar tip. */
                    dl->AddLine(ImVec2(cx - a, cy),
                                ImVec2(cx - a + h, cy - h), lineCol, th);
                    dl->AddLine(ImVec2(cx - a, cy),
                                ImVec2(cx - a + h, cy + h), lineCol, th);
                    dl->AddLine(ImVec2(cx + a, cy),
                                ImVec2(cx + a - h, cy - h), lineCol, th);
                    dl->AddLine(ImVec2(cx + a, cy),
                                ImVec2(cx + a - h, cy + h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy - a),
                                ImVec2(cx - h, cy - a + h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy - a),
                                ImVec2(cx + h, cy - a + h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy + a),
                                ImVec2(cx - h, cy + a - h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy + a),
                                ImVec2(cx + h, cy + a - h), lineCol, th);
                    if (ImGui::BeginDragDropSource(
                            ImGuiDragDropFlags_SourceAllowNullID)) {
                        uint8_t slot = (uint8_t)i;
                        ImGui::SetDragDropPayload("WB_LOBBY_PLAYER",
                                                  &slot, sizeof(slot));
                        ImGui::TextUnformatted(
                            clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]
                            ? clientSimGetLobbySlot(cs, (BYTE)(i))->playerName
                            : "(slot)");
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("Drag onto a team to move");
                    }
                    ImGui::SameLine(0.0f, 4.0f * s);
                    /* Reset Y for the tank-icon block below. */
                    ImGui::SetCursorPosY(rowTopY);
                }
                {
                    SDL_Texture *tankTex = nullptr;
                    if (r) {
                        if (isSelf) {
                            tankTex = getTankSelf04Texture(r);  /* black self */
                        } else if (isAlly) {
                            tankTex = getTankGood04Texture(r);  /* green ally */
                            if (!tankTex) tankTex = getTankSelf04Texture(r);
                        } else {
                            tankTex = getTankEvil04Texture(r);  /* red enemy */
                        }
                    }
                    if (tankTex) {
                        /* Indent the tank inside its cell so it
                         * doesn't sit flush with the team panel's
                         * left edge. */
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 28.0f * s);
                        /* Apply the same optical bias used for text:
                         * the tank PNG has a sliver of empty pixels
                         * along its bottom (cannon points up), so a
                         * pure geometric center makes it sit visibly
                         * low in the row. Nudge up ~12% of font size
                         * to match the centered look of the labels. */
                        cyAbs(tankSz);
                        float tankBias = ImGui::GetFontSize() * 0.12f;
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - tankBias);
                        ImGui::Image((ImTextureID)tankTex, ImVec2(tankSz, tankSz));
                    }
                }

                /* ── Column 1: identity icons (flag/platform/bot) ── */
                ImGui::TableSetColumnIndex(1);
                rowTopY = ImGui::GetCursorPosY();
                if (isBot) {
                    /* Green for bots on the local player's team (incl.
                     * the local player's own bots), red for bots on
                     * any other team. Fall back to the neutral
                     * bot-cpu.svg if a tinted variant is missing.
                     * Drawn at the same size as the tank icon so the
                     * two badges line up visually across rows. */
                    SDL_Texture *botTex = isAlly ? s_iconBotCpuGreen
                                                 : s_iconBotCpuRed;
                    if (botTex) {
                        cyAbs(tankSz);
                        /* Visible pixel mass in the bot-cpu PNGs
                         * needs a small upward nudge to land
                         * vertically centered on the row. */
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
                        ImGui::Image((ImTextureID)botTex,
                                     ImVec2(tankSz, tankSz));
                    }
                } else {
                    /* Same 2px upward nudge applied to text / tank /
                     * chip / gear — keeps every glyph in the row
                     * landing on a consistent optical center. */
                    const float iconBiasY = 2.0f;
                    if (clientSimGetLobbySlot(cs, (BYTE)(i))->countryCode[0] != '\0') {
                        SDL_Texture *flagTex = flagsGetTexture(clientSimGetLobbySlot(cs, (BYTE)(i))->countryCode);
                        if (flagTex) {
                            cyAbs((float)FLAG_HEIGHT);
                            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconBiasY);
                            ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                            ImGui::SameLine();
                        }
                    }
                    /* All WBN/Steam/platform icons in renderPlayerName are
                     * WBN_ICON_SIZE tall — center them as one block. */
                    cyAbs((float)WBN_ICON_SIZE);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconBiasY);
                    {
                        /* Hide the WBN globe in local-only sessions —
                         * single-player has no WBN session and a LAN-only
                         * host would similarly skip WBN registration. Keep
                         * the Steam + platform badges since those signal
                         * what the player is running, not who they are on
                         * the leaderboard. */
                        uint8_t pflags = clientSimGetLobbySlot(cs, (BYTE)(i))->clientFlags;
                        if (clientSimIsSinglePlayer(cs) || clientSimIsLanOnly(cs)) {
                            pflags &= ~PLAYER_FLAG_WBN_VERIFIED;
                        }
                        renderPlayerName(NULL,
                                         pflags,
                                         clientSimGetLobbySlot(cs, (BYTE)(i))->clientType,
                                         "", false);
                    }
                }

                /* ── Column 2: name + inline tags ────────────────── */
                ImGui::TableSetColumnIndex(2);
                rowTopY = ImGui::GetCursorPosY();
                cyTextAbs();
                if (isBot) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          wbThemeColor(g_theme->botBadge));
                    ImGui::Text("%s", clientSimGetLobbySlot(cs, (BYTE)(i))->playerName);
                    ImGui::PopStyleColor();
                } else if (isMe) {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
                                       "%s", clientSimGetLobbySlot(cs, (BYTE)(i))->playerName);
                } else {
                    ImGui::Text("%s", clientSimGetLobbySlot(cs, (BYTE)(i))->playerName);
                }

                /* Inline tag pills after the name. Drawn via
                 * WindowDrawList so we can size them tightly and
                 * tint each one independently (HOST = yellow,
                 * BOT = muted blue-gray). Same pill recipe as the
                 * READY/NOT READY badge but at 70% font size. */
                auto drawNameTag = [&](const char *lbl, ImU32 bg, ImU32 fg,
                                       ImU32 border = 0) {
                    const float tagScale = 0.70f;
                    float tagFontSz = ImGui::GetFontSize() * tagScale;
                    ImVec2 baseSz = ImGui::CalcTextSize(lbl);
                    ImVec2 textSz(baseSz.x * tagScale, tagFontSz);
                    float padX = 6.0f * s;
                    float padY = 2.0f * s;
                    float pillW = textSz.x + padX * 2.0f;
                    float pillH = textSz.y + padY * 2.0f;
                    ImGui::SameLine(0.0f, 6.0f * s);
                    cyAbs(pillH);
                    /* Nudge HOST / BOT / ADMIN name-tags up 1px so
                     * they sit a touch above the row centerline,
                     * which lines them up better with the cap-height
                     * of the player name. */
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 1.0f);
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    /* Square corners on the name tags so they read as
                     * "labels", not status pills like READY. */
                    dl->AddRectFilled(pos,
                                      ImVec2(pos.x + pillW, pos.y + pillH),
                                      bg, 0.0f);
                    if (border != 0) {
                        dl->AddRect(pos,
                                    ImVec2(pos.x + pillW, pos.y + pillH),
                                    border, 0.0f, 0, 1.0f);
                    }
                    dl->AddText(ImGui::GetFont(), tagFontSz,
                                ImVec2(pos.x + padX, pos.y + padY),
                                fg, lbl);
                    ImGui::Dummy(ImVec2(pillW, pillH));
                };
                if (i == 0) {
                    /* Host is always player slot 0. Themable bg /
                     * border / text triple lives in wb_theme.cpp. */
                    drawNameTag("HOST",
                                g_theme->hostTagBg,
                                g_theme->hostTagText,
                                g_theme->hostTagBorder);
                }
                if (!isBot && i != 0 &&
                    (clientSimGetLobbySlot(cs, (BYTE)(i))->clientFlags & PLAYER_FLAG_ADMIN)) {
                    /* IP-matched admin (server -admins). Shown beside the
                     * name like HOST but in a distinct teal so it reads
                     * as a separate "host-level authority" badge. */
                    drawNameTag("ADMIN",
                                IM_COL32(70, 160, 175, 255),
                                IM_COL32(10, 30, 35, 255));
                }
                if (isBot) {
                    drawNameTag("BOT",
                                g_theme->botTagBg,
                                g_theme->botTagText,
                                g_theme->botTagBorder);
                }

                /* ── Column 3: gear (bots) or ping (humans) ──────── */
                ImGui::TableSetColumnIndex(3);
                rowTopY = ImGui::GetCursorPosY();
                /* Gear visibility: always shown for bots when this
                 * client has lobby-edit authority (host / openHost /
                 * admin). Hidden entirely for non-permitted clients
                 * so they don't see a non-functional control. */
                if (isBot && effectiveHost) {
                    if (s_iconSettings) {
                        float iconSize = ImGui::GetFontSize();
                        cyAbs(iconSize);
                        /* settings.svg renders 5px above / 2px below
                         * with pure geometric centering — the gear
                         * sits slightly low in the row. Nudge up
                         * ~12% of font size (about 1.5px) so it
                         * matches the optical center used by the
                         * text and tank widgets. */
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY()
                                             - ImGui::GetFontSize() * 0.12f);
                        ImVec2 iconStart = ImGui::GetCursorScreenPos();
                        char btnId[24];
                        SDL_snprintf(btnId, sizeof(btnId), "##cfg%d", i);
                        bool clicked = ImGui::InvisibleButton(btnId,
                                                              ImVec2(iconSize, iconSize));
                        /* Tint the gear toward grey so it reads as a
                         * secondary action — the primary visual focus
                         * is the player name and status badges. Full
                         * white on hover so it lights up under the
                         * mouse. */
                        ImU32 gearTint = ImGui::IsItemHovered()
                            ? IM_COL32_WHITE
                            : IM_COL32(180, 180, 180, 200);
                        ImGui::GetWindowDrawList()->AddImage(
                            (ImTextureID)s_iconSettings,
                            iconStart,
                            ImVec2(iconStart.x + iconSize, iconStart.y + iconSize),
                            ImVec2(0, 0), ImVec2(1, 1), gearTint);
                        if (clicked) {
                            s_expandedBotSlot = (s_expandedBotSlot == i) ? -1 : i;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Configure bot");
                        }
                    } else {
                        cyAbs(ImGui::GetFrameHeight());
                        char fallId[24];
                        SDL_snprintf(fallId, sizeof(fallId), "%s##cfg%d",
                                     s_expandedBotSlot == i ? "v" : ">", i);
                        if (ImGui::SmallButton(fallId)) {
                            s_expandedBotSlot = (s_expandedBotSlot == i) ? -1 : i;
                        }
                    }
                } else if (!isBot) {
                    if (clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs > 0) {
                        cyTextAbs();
                        ImVec4 pingColor;
                        if (clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs < 50)        pingColor = wbThemeColor(g_theme->statusOnline);
                        else if (clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs < 150)  pingColor = wbThemeColor(g_theme->statusHighPing);
                        else                                       pingColor = wbThemeColor(g_theme->statusDisconnected);
                        ImGui::TextColored(pingColor, "%dms", (int)clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs);
                    }
                }

                /* ── Column 5: ready / not ready badge (humans only — bots
                 *   are always ready and don't need a pill) ─────────────── */
                ImGui::TableSetColumnIndex(5);
                rowTopY = ImGui::GetCursorPosY();
                if (!isBot) {
                    bool isReady = clientSimGetLobbySlot(cs, (BYTE)(i))->ready;
                    const char *lbl = isReady ? "READY" : "NOT READY";
                    /* Render the badge text at 80% of the row font
                     * size — a touch smaller than the player name
                     * so the pill reads as a status tag rather than
                     * a primary label. */
                    const float pillFontScale = 0.80f;
                    float pillFontSz = ImGui::GetFontSize() * pillFontScale;
                    ImVec2 baseSz = ImGui::CalcTextSize(lbl);
                    ImVec2 textSz(baseSz.x * pillFontScale, pillFontSz);
                    float padX  = 7.0f * s;
                    float padY  = 3.0f * s;
                    float pillW = textSz.x + padX * 2.0f;
                    float pillH = textSz.y + padY * 2.0f;
                    cyAbs(pillH);
                    /* Nudge the READY / NOT READY pill up 2px so it
                     * lines up with the cap-height of adjacent text
                     * rather than the row's geometric center. */
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
                    ImVec2 pillPos = ImGui::GetCursorScreenPos();
                    ImU32 bgCol = isReady ? IM_COL32(42, 80, 44, 255)
                                          : IM_COL32(58, 58, 58, 255);
                    ImU32 fgCol = isReady ? IM_COL32(120, 210, 120, 255)
                                          : IM_COL32(180, 180, 180, 255);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(pillPos,
                                      ImVec2(pillPos.x + pillW,
                                             pillPos.y + pillH),
                                      bgCol, pillH * 0.5f);
                    dl->AddText(ImGui::GetFont(), pillFontSz,
                                ImVec2(pillPos.x + padX,
                                       pillPos.y + padY),
                                fgCol, lbl);
                    ImGui::Dummy(ImVec2(pillW, pillH));
                }

                /* ── Column 6: remove bot X (CloseButton) ────────── */
                ImGui::TableSetColumnIndex(6);
                rowTopY = ImGui::GetCursorPosY();
                if (isBot && effectiveHost) {
                    cyAbs(closeSz);
                    ImVec2 closePos = ImGui::GetCursorScreenPos();
                    char rbStr[24];
                    SDL_snprintf(rbStr, sizeof(rbStr), "##rb%d", i);
                    ImGuiID rbId = ImGui::GetID(rbStr);
                    if (ImGui::CloseButton(rbId, closePos)) {
                        lobbySendRemoveBot(cs, (uint8_t)i);
                        if (s_expandedBotSlot == i) s_expandedBotSlot = -1;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove bot");
                    }
                }

                /* AiConfig sub-row when the wrench is expanded.
                 * Rendered as its own table row, but we only put
                 * content in column 0 — the form's widgets render
                 * past the column boundary and visually span the
                 * full team panel. The horizontal border below this
                 * row groups the form with the bot above it via the
                 * normal inter-row line. */
                if (isBot && s_expandedBotSlot == i && effectiveHost) {
                    ImGui::TableNextRow();
                    /* Inherit the parent bot row's stripe color so the
                     * sub-row reads as a continuation of that row and
                     * doesn't disturb the odd/even pattern. */
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowStripe);
                    ImGui::TableSetColumnIndex(0);
                    renderBotAiConfig(cs, i, teamId, s);
                }
            }
            ImGui::EndTable();
        }

        /* "+ Bot" button moved to the team header bar (right-aligned
         * next to the "Bot Naming:" pool dropdown) so all team-bot
         * controls live in one place. */

        ImGui::EndChild();
        /* Drop target: the team child window is the last item. Any
         * BeginDragDropSource elsewhere can drop a slot index here
         * to move that player onto this team. While a drag is over
         * the team, paint a translucent highlight along its border. */
        if (ImGui::BeginDragDropTarget()) {
            ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery;
            const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(
                "WB_LOBBY_PLAYER", flags);
            if (payload) {
                /* Paint a thicker, brighter border while the payload
                 * hovers the team child. */
                ImVec2 itemMin = ImGui::GetItemRectMin();
                ImVec2 itemMax = ImGui::GetItemRectMax();
                ImU32 hl = (tc & 0x00FFFFFF) | (0xC0 << 24);
                ImGui::GetForegroundDrawList()->AddRect(
                    itemMin, itemMax, hl, 4.0f, 0, 3.0f);
                if (payload->IsDelivery() &&
                    payload->DataSize == (int)sizeof(uint8_t)) {
                    uint8_t fromSlot = *(const uint8_t *)payload->Data;
                    if (fromSlot < MAX_TANKS) {
                        lobbySendTeamSet(cs,
                                         fromSlot, (uint8_t)teamId);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::Spacing();
    }

    /* "Add Team" lives at the bottom of the team list so it reads as
     * "+ another team after these ones" rather than a header action.
     * Picks the lowest unused teamId and sends a default-name TEAM_META
     * (SP path writes the TeamMetadata directly since the wire packet
     * has no SP equivalent). */
    if (effectiveHost) {
        /* Full-width "Add Team" affordance, styled to read as a subtle
         * "+1 row" prompt rather than a primary action — translucent
         * background and dimmed text so it doesn't dominate the team
         * list it sits beneath. */
        ImVec4 baseBtn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 baseTxt = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.65f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(baseTxt.x, baseTxt.y, baseTxt.z, baseTxt.w * 0.65f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f * s));
        bool addTeamClicked = ImGui::Button("Add Team", ImVec2(-1, 0));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        if (addTeamClicked) {
            for (int t = 1; t < 16; t++) {
                if (memberCount[t] == 0 && !clientSimGetLobbyTeamInUse(cs, (BYTE)(t))) {
                    char defaultName[16];
                    SDL_snprintf(defaultName, sizeof(defaultName), "Team %d", t);
                    uint8_t color = (uint8_t)((t - 1) & 7);
                    if (clientSimIsSinglePlayer(cs)) {
                        ServerSim *spSim = gameFrontGetSinglePlayerServerSim();
                        TeamMetadata *tm = serverSimGetTeamMetaMut(spSim, t);
                        if (spSim && tm) {
                            tm->in_use = 1;
                            tm->color = color;
                            tm->namingPool = 0;
                            strncpy(tm->name, defaultName, LOBBY_TEAM_NAME_LEN - 1);
                            tm->name[LOBBY_TEAM_NAME_LEN - 1] = '\0';
                            serverSimPublishLobbyTeamMeta(spSim, (uint8_t)t);
                        }
                    } else {
                        clientSimNetSendLobbyTeamMeta(cs, (uint8_t)t,
                            color, 0 /*pool=classic*/, defaultName);
                    }
                    break;
                }
            }
        }
        ImGui::Spacing();
    }

    /* Unassigned tray. */
    if (unassignedCount > 0) {
        ImGui::TextDisabled("Unassigned (%d):", unassignedCount);
        for (int i = 0; i < MAX_TANKS; i++) {
            if (!clientSimGetLobbySlot(cs, (BYTE)(i))->connected) continue;
            if (clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber != 0) continue;
            ImGui::Bullet();
            ImGui::Text("%s%s", clientSimGetLobbySlot(cs, (BYTE)(i))->playerName,
                        i == myPlayerNum ? " (you)" : "");
        }
    }

    /* No bottom team picker — Layout A relies on drag-to-assign + the
     * default-team logic on the server. Players who want to switch
     * teams can be dragged by the host (planned) or via context menu. */
}

/* ── Layout A — bot AiConfig sub-row ──────────────────────────────
 * Inline panel under an expanded bot row showing the name override
 * with dice-reroll, difficulty dropdown, personality dropdown, and
 * close button. Edits dispatch as PACKET_LOBBY_BOT_CONFIG. */
static void renderBotAiConfig(ClientSim *cs,
                              int slot, int teamId, float s) {
    if (slot < 0 || slot >= MAX_TANKS) return;
    /* Render the whole AiConfig form at a smaller font so it reads
     * as a secondary control surface beneath the bot row. Saved +
     * restored so we don't bleed into the rest of the lobby. */
    float aicfgOldScale = ImGui::GetCurrentWindow()->FontWindowScale;
    ImGui::SetWindowFontScale(aicfgOldScale * 0.85f);

    /* Build the "currently used names" array for the dice reroll —
     * collect every bot name in the lobby so we don't collide. */
    const char *usedNames[MAX_TANKS];
    int usedCount = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected && clientSimGetLobbySlot(cs, (BYTE)(i))->isBot &&
            clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]) {
            usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
        }
    }

    /* Edit form sits flush with the left edge of the team panel —
     * no internal indent. (The caller already strips the surrounding
     * 50px row indent so the form spans the full team width.) */
    ImGui::PushID(slot);

    /* Two side-by-side groups: Name on the left, Bot Code on the right.
     * BeginGroup + SameLine works inside the outer table cell (which is
     * NoClip-enabled), unlike a nested BeginTable which gets clipped to
     * the parent's narrow column width and squashes the controls. */
    char nameBuf[32];
    strncpy(nameBuf, clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName, sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    bool nameChanged = false;
    bool diceClicked = false;
    const BrainList *bl = clientSimGetLobbyBrainList(cs);
    int pendingBrainPick = -1;

    /* Capture the form's top Y once and anchor every group there
     * explicitly via SetCursorScreenPos. Relying on SameLine alone
     * was producing a few-pixel drift (Name appearing higher than
     * Bot Code / Done) — probably because the Name group's
     * frame-bordered InputText shifts the group's "start Y" by
     * the FrameBorderSize when ItemAdd commits it. An explicit
     * anchor sidesteps that entirely. */
    ImVec2 formAnchor = ImGui::GetCursorScreenPos();
    /* Nudge all three sub-groups 2px down (purely cosmetic — the
     * AiConfig content felt visually crowded against the bot row
     * above). The end-of-function SetCursorScreenPos subtracts the
     * same nudge so the parent container's total height is
     * unchanged. */
    const float kFormNudgeY = 2.0f;
    formAnchor.y += kFormNudgeY;

    /* Name group. */
    ImGui::SetCursorScreenPos(formAnchor);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Name (override)");
    ImGui::SetNextItemWidth(180.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    nameChanged = ImGui::InputText("##botname", nameBuf, sizeof(nameBuf),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar();
    ImGui::SameLine();
    diceClicked = ImGui::Button("Reroll");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pick a fresh random name from the team's pool.");
    }
    ImGui::EndGroup();
    float nameGroupRightX = ImGui::GetItemRectMax().x;
    float nameGroupBottomY = ImGui::GetItemRectMax().y;

    /* Bot Code group, right of the Name group at the same anchor Y. */
    float botCodeStartX = 0.0f;
    float botCodeStartY = 0.0f;
    bool  botCodeShown  = (bl->count > 0);
    const float comboW  = 240.0f * s;
    if (botCodeShown) {
        float bcX = nameGroupRightX + 24.0f * s;
        ImGui::SetCursorScreenPos(ImVec2(bcX, formAnchor.y));
        botCodeStartX = bcX;
        botCodeStartY = formAnchor.y;
        ImGui::BeginGroup();
        ImGui::TextDisabled("Bot Code");
        const char *curPath = clientSimGetLobbyBotBrain(cs, (BYTE)(slot));
        const BrainListEntry *curEntry = brainListFindByPath(bl, curPath);
        char preview[BRAIN_LIST_NAME_LEN + BRAIN_LIST_VER_LEN + 8];
        if (curEntry) {
            if (curEntry->version[0])
                SDL_snprintf(preview, sizeof(preview), "%s (%s)",
                             curEntry->name, curEntry->version);
            else
                SDL_snprintf(preview, sizeof(preview), "%s", curEntry->name);
        } else if (curPath[0]) {
            SDL_snprintf(preview, sizeof(preview), "(custom)");
        } else {
            SDL_snprintf(preview, sizeof(preview), "(none)");
        }
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##botbrain", preview)) {
            for (int b = 0; b < bl->count; b++) {
                const BrainListEntry *e = &bl->entries[b];
                char label[BRAIN_LIST_NAME_LEN + BRAIN_LIST_VER_LEN + 8];
                if (e->version[0])
                    SDL_snprintf(label, sizeof(label), "%s (%s)",
                                 e->name, e->version);
                else
                    SDL_snprintf(label, sizeof(label), "%s", e->name);
                bool sel = (curEntry == e);
                if (ImGui::Selectable(label, sel)) {
                    pendingBrainPick = b;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
    }

    if (diceClicked) {
        /* Pick a name from this team's pool, excluding all currently-used
         * bot names (incl. this one — we want a NEW name, not the same).
         * Reroll = pool-driven, so clear the manual-override flag. */
        int pool = (teamId > 0 && teamId < 16) ? clientSimGetLobbyTeamPool(cs, (BYTE)(teamId)) : 0;
        char pickBuf[32];
        lobbyBotPoolPick(pool, usedNames, usedCount, pickBuf, sizeof(pickBuf));
        lobbySendBotConfig(cs, (uint8_t)slot,
            clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)), clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)), pickBuf);
        if (slot < MAX_TANKS) s_botNameOverridden[slot] = false;
    }
    if (nameChanged) {
        /* Manual edit — pin the name so a later pool change doesn't
         * overwrite it. */
        lobbySendBotConfig(cs, (uint8_t)slot,
            clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)), clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)), nameBuf);
        if (slot < MAX_TANKS) s_botNameOverridden[slot] = true;
    }
    if (pendingBrainPick >= 0 && pendingBrainPick < bl->count) {
        lobbySendSetBotBrain(cs, (uint8_t)slot,
                             bl->entries[pendingBrainPick].path);
    }

    /* Difficulty / Personality dropdowns are hidden for now — the
     * brain doesn't yet honor either field. Kept in the code (and
     * still wired through lobbySendBotConfig) so flipping this
     * flag to true is the only thing needed to re-enable the UI
     * once the brain consumes the values. */
    const bool kShowAiOptions = false;

    /* ── Difficulty dropdown ──────────────────────────────────── */
    if (kShowAiOptions) {
        ImGui::SameLine(0.0f, 16.0f * s);
        ImGui::TextDisabled("Difficulty");
        ImGui::SameLine();
        const char *diffItems[] = { "Easy", "Normal", "Hard" };
        int diff = clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot));
        if (diff < 0 || diff > 2) diff = 1;
        ImGui::SetNextItemWidth(90.0f * s);
        if (ImGui::Combo("##diff", &diff, diffItems, 3)) {
            lobbySendBotConfig(cs, (uint8_t)slot,
                (uint8_t)diff, clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)),
                clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName);
        }
    }

    /* ── Personality dropdown ─────────────────────────────────── */
    if (kShowAiOptions) {
        ImGui::SameLine(0.0f, 16.0f * s);
        ImGui::TextDisabled("Personality");
        ImGui::SameLine();
        const char *persItems[] = { "Normal", "Aggressive", "Defensive", "Sniper" };
        int pers = clientSimGetLobbyBotPersonality(cs, (BYTE)(slot));
        if (pers < 0 || pers > 3) pers = 0;
        ImGui::SetNextItemWidth(110.0f * s);
        if (ImGui::Combo("##pers", &pers, persItems, 4)) {
            lobbySendBotConfig(cs, (uint8_t)slot,
                clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)), (uint8_t)pers,
                clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName);
        }
    }

    /* ── Done group — anchored to formAnchor.y so all three sub-
     * groups (Name / Bot Code / Done) sit on the same top Y. The
     * "Hello" spacer above the button is a temporary debug label;
     * change back to a blank string once alignment is confirmed. */
    {
        const char *doneLbl = "Done";
        float doneW = ImGui::CalcTextSize(doneLbl).x
                    + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImVec2 winPos  = ImGui::GetWindowPos();
        float winRight = winPos.x + ImGui::GetWindowSize().x;
        float padR     = 20.0f * s;
        float targetScreenX = winRight - doneW - padR;
        ImGui::SetCursorScreenPos(ImVec2(targetScreenX, formAnchor.y));
        ImGui::BeginGroup();
        /* Invisible spacer that advances the cursor exactly the
         * same amount as the TextDisabled("Bot Code") above its
         * combo — using TextDisabled(" ") keeps the metrics
         * identical so the Done button below stays vertically
         * aligned with the combo. */
        ImGui::TextDisabled(" ");
        ImGui::SetCursorScreenPos(ImVec2(targetScreenX,
                                         ImGui::GetCursorScreenPos().y));
        if (ImGui::Button(doneLbl)) {
            s_expandedBotSlot = -1;
        }
        ImGui::EndGroup();
    }

    /* Restore the cursor below all three groups so any subsequent
     * widgets in the AiConfig sub-row land underneath. Subtract
     * kFormNudgeY from the final Y so the 2px we shifted the
     * contents down doesn't grow the parent container. */
    float bottomY = nameGroupBottomY;
    float curBotBottom = ImGui::GetItemRectMax().y;
    if (curBotBottom > bottomY) bottomY = curBotBottom;
    ImGui::SetCursorScreenPos(
        ImVec2(formAnchor.x,
               bottomY - kFormNudgeY + ImGui::GetStyle().ItemSpacing.y));

    ImGui::PopID();
    ImGui::Spacing();
    ImGui::SetWindowFontScale(aicfgOldScale);
}

/* ── Layout A — connectivity badge (icon + short text + Test btn) ─
 * Self-contained block extracted from the inline lobby chrome so it
 * can be rendered at top-right of the status bar (Layout A
 * placement) instead of below the settings line.
 *
 * Renders nothing when port-mapping status is DISABLED. Owns its own
 * popup modal for the detail view; clicking the icon/text opens it. */
static void renderConnectivityBadge(SDL_Renderer *renderer, float s) {
    ServerPortmapInfo pm;
    serverInstanceGetPortmapInfo(&pm);

    loadStatusIconsOnce(renderer, s);

    SDL_Texture *icon       = nullptr;
    const char  *shortText  = nullptr;
    ImVec4       color;
    const char  *detailFmt  = nullptr;

    switch (pm.status) {
        case SERVER_PORTMAP_DISABLED:
            return;  /* nothing to show */
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

    if (!shortText) return;

    ImGui::BeginGroup();
    if (icon) {
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

/* ── Layout A — server reject toast ───────────────────────────────
 * Surfaces the last PACKET_LOBBY_REJECT as a one-line orange status
 * pill. Auto-clears after the user dismisses it (clicks the X) so
 * subsequent rejects re-trigger naturally. */
static void renderLobbyRejectToast(ClientSim *cs, float s) {
    if (clientSimGetLobbyLastRejectPacket(cs) == 0) return;
    const char *reason = "rejected";
    switch (clientSimGetLobbyLastRejectReason(cs)) {
        case 1: reason = "host-only action";       break;  /* LOBBY_REJECT_NOT_HOST */
        case 2: reason = "setting locked by server"; break; /* LOBBY_REJECT_LOCKED */
        case 3: reason = "invalid request";         break; /* LOBBY_REJECT_INVALID */
        default: break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, wbThemeColor(g_theme->lockBadge));
    ImGui::Text("⚠ Lobby change rejected: %s", reason);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("X##rejdismiss")) {
        clientSimClearLobbyLastReject(cs);
    }
    (void)s;
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
static void renderGameSettingsPanel(ClientSim *cs,
                                    int myPlayerNum, float s) {
    bool effectiveHost = (myPlayerNum == 0) || clientSimGetLobbyOpenHost(cs) ||
                         (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                          (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                           & PLAYER_FLAG_ADMIN));

    /* Drive the CollapsingHeader's open state explicitly so a "Hide
     * Settings" button at the bottom of the panel can fold it away
     * once the host is happy with the configuration. */
    static bool s_settingsOpen = true;
    ImGui::SetNextItemOpen(s_settingsOpen, ImGuiCond_Always);
    /* Capture screen-Y of the header before drawing so the
     * right-aligned openHost control can be overlaid on the same
     * line via SetCursorScreenPos. */
    ImVec2 headerStart = ImGui::GetCursorScreenPos();
    if (!ImGui::CollapsingHeader("Game settings")) {
        s_settingsOpen = false;
        return;
    }
    s_settingsOpen = true;

    /* The openHost ("Allow all players to change settings") state is
     * intentionally invisible to regular players — they shouldn't even
     * know they got their edit access via that particular toggle, only
     * that the settings happen to be interactable for them. The actual
     * editable checkbox is rendered for host/admin only in the "Other"
     * column below. */

    /* Settings body uses a smaller font than the rest of the lobby so
     * the 3-column form doesn't dominate the visual hierarchy. */
    float settingsOldScale = ImGui::GetCurrentWindow()->FontWindowScale;
    ImGui::SetWindowFontScale(settingsOldScale * 0.85f);

    ImGui::Spacing();
    ImGui::Columns(3, "##settingsCols", false);

    /* ── Game Type ──────────────────────────────────────────── */
    bool gtLocked = (clientSimGetLobbyServerLocks(cs) & 0x01) != 0;  /* LOBBY_LOCK_GAME_TYPE */
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
            /* gameType enum is 1-based (gameOpen=1, gameTournament=2,
             * gameStrictTournament=3), so the array index → enum
             * mapping is i+1. The previous (gameType)i comparison
             * read the wrong row as "checked" — Open showed as
             * Unknown, Tournament showed as Open, etc. */
            int enumVal = i + 1;
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##gt%d", items[i], i);
            bool checked = (clientSimGetLobbyGameType(cs) == (gameType)enumVal);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)enumVal;
                lobbySendSetting(cs, 1 /*LST_GAME_TYPE*/, &v, 1);
            }
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── Computer Players ────────────────────────────────────── */
    bool aiLocked = (clientSimGetLobbyServerLocks(cs) & 0x02) != 0;  /* LOBBY_LOCK_AI_POLICY */
    {
        ImGui::Text("Computer Players");
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
            /* Same trick as the Game Type radios — embed the label so
             * the whole row is clickable. */
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##ai%d", items[i], i);
            bool checked = (clientSimGetLobbyAiType(cs) == (uint8_t)i);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)i;
                lobbySendSetting(cs, 3 /*LST_AI_POLICY*/, &v, 1);
            }
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── Other (mines / time limit / autoLockOnGameStart) ────── */
    {
        ImGui::Text("Other");

        bool minesLocked = (clientSimGetLobbyServerLocks(cs) & 0x04) != 0;
        bool minesV = clientSimIsLobbyHiddenMines(cs);
        bool minesDisabled = !effectiveHost || minesLocked;
        if (minesDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Allow Hidden Mines", &minesV)) {
            uint8_t v = minesV ? 1 : 0;
            lobbySendSetting(cs, 2 /*LST_HIDDEN_MINES*/, &v, 1);
        }
        if (minesDisabled) ImGui::EndDisabled();
        if (minesLocked) renderLockBadge();

        bool timeLocked = (clientSimGetLobbyServerLocks(cs) & 0x08) != 0;
        bool timeV = clientSimGetLobbyTimeLimit(cs) > 0;
        bool timeDisabled = !effectiveHost || timeLocked;
        if (timeDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Game time limit", &timeV)) {
            uint8_t v = timeV ? 1 : 0;
            lobbySendSetting(cs, 4 /*LST_TIME_LIMIT*/, &v, 1);
        }
        if (timeV) {
            int mins = clientSimGetLobbyTimeLimit(cs) > 0
                ? (int)(clientSimGetLobbyTimeLimit(cs) / (50 * 60))
                : 30;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f * s);
            if (ImGui::InputInt("##tmin", &mins, 1, 5,
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (mins < 1) mins = 1;
                if (mins > 999) mins = 999;
                uint8_t v[2] = { (uint8_t)((mins >> 8) & 0xFF),
                                 (uint8_t)(mins & 0xFF) };
                lobbySendSetting(cs, 5 /*LST_TIME_MINUTES*/, v, 2);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("min");
        }
        if (timeDisabled) ImGui::EndDisabled();
        if (timeLocked) renderLockBadge();

        /* "Allow new players" / "Disallow new players once game has
         * started" moved to the team-list header (right of "+ Add Team")
         * so all join-related controls live in one row. See
         * renderTeamGroupedPlayers. */

        /* "Allow all players to change settings" — toggles openHost
         * (the same flag that gates per-team manage-bots authority).
         * Visible to host / admin only.  Regular players who got their
         * edit access via this very toggle don't see the control or any
         * read-only mirror of it — surfacing it would just advertise the
         * mechanism and tempt them to flip it off (which they can't
         * anyway, but the absence avoids the noise). */
        if (!clientSimIsSinglePlayer(cs)) {
            bool isHostLocal = (myPlayerNum == 0);
            bool isAdminLocal = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                                 (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                                  & PLAYER_FLAG_ADMIN));
            if (isHostLocal || isAdminLocal) {
                bool oh = clientSimGetLobbyOpenHost(cs);
                if (ImGui::Checkbox("Allow all players to change settings",
                                    &oh)) {
                    clientSimNetSendLobbyOpenHost(cs, oh);
                }
            }
        }
    }

    ImGui::Columns(1);

    /* Right-aligned "Hide Settings" button. Positioned by directly
     * adjusting the cursor Y up by ~30px so the panel's overall
     * bottom edge sits 30 pixels higher than it would with default
     * ImGui::Spacing() padding — claws back vertical space for the
     * teams list below.
     *
     * Styled to match the "Add Team" affordance — translucent
     * background + dimmed text + reduced frame padding so it reads as
     * a subtle "collapse" action rather than a primary button. */
    {
        const char *label = "Hide Settings";
        ImVec4 baseBtn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 baseTxt = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.65f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(baseTxt.x, baseTxt.y, baseTxt.z, baseTxt.w * 0.65f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f * s));
        float btnW = ImGui::CalcTextSize(label).x + 16.0f * s;
        float y = ImGui::GetCursorPosY() - 30.0f * s;
        ImGui::SetCursorPosY(y);
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW);
        if (ImGui::Button(label)) {
            s_settingsOpen = false;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }
    ImGui::SetWindowFontScale(settingsOldScale);
}

extern "C" int imguiLobbyShow(ClientSim *cs) {
    WB_LOG_INFO(WB_LOG_CAT_GUI, "[LOBBY] imguiLobbyShow called cs=%p inLobby=%d netStat=%d",
            (void*)cs, cs ? (int)clientSimIsInLobby(cs) : -1, cs ? (int)clientSimGetNetStatus(cs) : -1);
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
    bool prevMapDownloadComplete = clientSimIsMapDownloadComplete(cs);
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
        bool hasTransport = clientSimHasTransport(cs);
        if (hasTransport) {
            clientSimNetTick(cs);
        }

        /* Clear balance proposal when countdown starts */
        if (clientSimGetCountdownSeconds(cs) > 0 && clientSimIsBalanceProposalActive(cs)) {
            clientSimSetBalanceProposalActive(cs, false);
            clientSimClearBalanceProposal(cs);
        }

        /* Check for game start */
        if (clientSimGetNetStatus(cs) == netRunning) {
            result = 1;
            running = false;
            break;
        }

        /* Check for server disconnect/shutdown */
        if (hasTransport) {
            ClientConnectState js = clientSimGetConnectState(cs);
            if (js == CLIENT_CONNECT_SERVER_SHUTDOWN || js == CLIENT_CONNECT_ERROR) {
                imguiMessageBoxEx(DIALOG_BOX_TITLE,
                    langGetText(STR_DLGLOBBY_LOSTCONNECTION),
                    IMGUI_MSG_ERROR, IMGUI_MSG_OK);
                result = 0;
                running = false;
                break;
            }
        }

        /* Reset preview when a map change invalidates the download */
        if (!clientSimIsMapDownloadComplete(cs) && prevMapDownloadComplete) {
            mapPreviewBuilt = false;
            if (mapPreviewTex) {
                SDL_DestroyTexture(mapPreviewTex);
                mapPreviewTex = NULL;
            }
            if (popupCompressedData) { SDL_free(popupCompressedData); popupCompressedData = NULL; popupCompressedLen = 0; }
            mapPreviewPopupClose();
        }
        prevMapDownloadComplete = clientSimIsMapDownloadComplete(cs);

        /* Build map preview once download completes */
        if (clientSimIsMapDownloadComplete(cs) && !mapPreviewBuilt && hasTransport) {
            int mapLen = 0;
            const BYTE *mapData = clientSimGetServerMapData(cs, &mapLen);
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
            struct in_addr srvAddr = clientSimGetServerAddress(cs);
            const char *addrStr = inet_ntoa(srvAddr);
            /* LAN host self-joins via loopback (127.0.0.1) — display
             * the actual LAN-routable IPv4 instead so it's useful to
             * read off to a player on the same network. Local helper:
             * UDP socket + "connect" to a public address (no packets
             * sent, just routing-table lookup) + getsockname. */
            char lanIp[INET_ADDRSTRLEN];
            lanIp[0] = '\0';
            auto fillLanIp = [&]() -> bool {
                bolo_socket_t sk = socket(AF_INET, SOCK_DGRAM, 0);
                if (sk == BOLO_INVALID_SOCKET) return false;
                sockaddr_in tgt; memset(&tgt, 0, sizeof(tgt));
                tgt.sin_family = AF_INET;
                tgt.sin_port = htons(53);
                inet_pton(AF_INET, "8.8.8.8", &tgt.sin_addr);
                if (connect(sk, (sockaddr *)&tgt, sizeof(tgt)) != 0) {
                    closesocket(sk); return false;
                }
                sockaddr_in loc; memset(&loc, 0, sizeof(loc));
#ifdef _WIN32
                int slen = (int)sizeof(loc);
#else
                socklen_t slen = sizeof(loc);
#endif
                int rc = getsockname(sk, (sockaddr *)&loc, &slen);
                closesocket(sk);
                if (rc != 0) return false;
                return inet_ntop(AF_INET, &loc.sin_addr,
                                 lanIp, sizeof(lanIp)) != NULL;
            };
            if (clientSimIsLanOnly(cs) && addrStr &&
                strcmp(addrStr, "127.0.0.1") == 0 &&
                fillLanIp() &&
                lanIp[0] != '\0' &&
                strcmp(lanIp, "127.0.0.1") != 0) {
                addrStr = lanIp;
            }
            SDL_snprintf(serverStr, sizeof(serverStr), "%s:%u",
                         addrStr, clientSimGetServerPort(cs));

            char timeStr[32];
            formatTimeLimit(clientSimGetLobbyTimeLimit(cs), timeStr, sizeof(timeStr));

#if BOLO_MOBILE
            /* Stack labels vertically on mobile so the line wraps cleanly. */
            ImGui::Text("%s %s", langGetText(STR_DLGNETINFO_SERVER), serverStr);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_GAME_LBL), gameTypeStr(clientSimGetLobbyGameType(cs)));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MINES_LBL),
                        clientSimIsLobbyHiddenMines(cs) ? langGetText(STR_DLGLOBBY_HIDDEN) : langGetText(STR_DLGLOBBY_VISIBLE));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), aiTypeStr(clientSimGetLobbyAiType(cs)));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
#else
            /* Leave button sits at the top-left, before the Server: line.
             * Escape key also opens the leave confirmation popup. Rendered as
             * a back arrow (left-pointing triangle) drawn into a normal-height
             * button so it matches Add Team / Ready visually without depending
             * on geometric-shape glyphs being present in the active font. */
            float leaveBtnH = ImGui::GetFrameHeight();
            float leaveBtnW = leaveBtnH * 1.4f;
            ImVec2 leaveBtnPos = ImGui::GetCursorScreenPos();
            bool leaveClicked = ImGui::Button("##leave", ImVec2(leaveBtnW, leaveBtnH));
            {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                float cx = leaveBtnPos.x + leaveBtnW * 0.5f;
                float cy = leaveBtnPos.y + leaveBtnH * 0.5f;
                float r  = leaveBtnH * 0.28f;
                ImVec2 p1(cx - r,         cy);
                ImVec2 p2(cx + r * 0.7f,  cy - r);
                ImVec2 p3(cx + r * 0.7f,  cy + r);
                ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
                dl->AddTriangleFilled(p1, p2, p3, col);
            }
            if (leaveClicked ||
                (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                 !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                char leavePopupId[64];
                SDL_snprintf(leavePopupId, sizeof(leavePopupId), "%s##lobby", langGetText(STR_DLGLOBBY_LEAVE_TITLE));
                ImGui::OpenPopup(leavePopupId);
            }
            ImGui::SameLine(0, 16);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s %s", langGetText(STR_DLGNETINFO_SERVER), serverStr);
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_GAME_LBL), gameTypeStr(clientSimGetLobbyGameType(cs)));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MINES_LBL),
                        clientSimIsLobbyHiddenMines(cs) ? langGetText(STR_DLGLOBBY_HIDDEN) : langGetText(STR_DLGLOBBY_VISIBLE));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), aiTypeStr(clientSimGetLobbyAiType(cs)));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
#endif

            /* Layout A — connectivity badge in the top-right corner.
             * Only meaningful when this client is also hosting AND
             * the host instance is actively NAT-punching (i.e. not
             * passed -no-natpunch / LAN-only). Skipping it covers SP,
             * LAN-only hosts, and non-hosting clients in one check. */
            if (serverInstanceIsNatPunchActive()) {
                float availW   = ImGui::GetContentRegionAvail().x;
                float badgeW   = 320.0f * s;
                float startX   = ImGui::GetCursorPosX() + availW - badgeW;
                if (startX > ImGui::GetCursorPosX()) {
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(startX);
                    renderConnectivityBadge(renderer, s);
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Layout A — surface the most recent server reject (locked
         * setting, non-host action, invalid request). Renders only
         * when clientSimGetLobbyLastRejectPacket(cs) != 0. */
        renderLobbyRejectToast(cs, s);

        /* Layout A — collapsible game settings panel (radios, checkboxes,
         * lock badges). Edits dispatch via PACKET_LOBBY_SET_SETTING. */
        renderGameSettingsPanel(cs, myPlayerNum, s);
        ImGui::Separator();
        ImGui::Spacing();

        /* Hosted-MP port-mapping status now renders at top-right via
         * renderConnectivityBadge — see the call site in the read-only
         * status block above. */

        /* --- Main content --- */
#if BOLO_MOBILE
        /* Tabbed layout for mobile: Players | Map | Chat */
        {
            float availW = ImGui::GetContentRegionAvail().x - padR;
            float btnAreaH = ImGui::GetTextLineHeightWithSpacing() * 2 + 16.0f * s;

            /* Detect new chat messages for unread indicator */
            int chatLen = (int)SDL_strlen(clientSimGetLobbyChatHistory(cs));
            if (chatLen > lastChatLen && activeTab != 2) {
                chatUnread = true;
            }
            lastChatLen = chatLen;

            if (ImGui::BeginTabBar("##LobbyTabs")) {
                /* --- Players tab --- */
                if (ImGui::BeginTabItem(langGetText(STR_MENU_PLAYERS))) {
                    activeTab = 0;
                    /* Allow New Players row above the player list (mobile). */
                    renderAllowNewPlayersRow(cs, myPlayerNum, s);
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;
                    ImGui::BeginChild("##PlayerPanel", ImVec2(availW, tabH), ImGuiChildFlags_None);

                    /* Layout A: team-grouped player rendering. The
                     * legacy 5-column table below the #if 0 is left
                     * intact for reference; toggle the 0/1 to A/B
                     * compare during the in-progress UI rewrite. */
#if 1
                    bool isHostHere = (myPlayerNum == 0);
                    renderTeamGroupedPlayers(cs, myPlayerNum, s, isHostHere);
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

                        bool botsAllowed = (clientSimGetLobbyAiType(cs) != 0);

                        WB_LOG_TRACE(WB_LOG_CAT_GUI, "[LOBBY DBG] rendering player table");
                        for (int i = 0; i < MAX_TANKS; i++) {
                            ImGui::TableNextRow();

                            const ClientLobbySlot *slot = clientSimGetLobbySlot(cs, (BYTE)i);
                            if (slot && slot->connected) {
                                bool isMe = (i == myPlayerNum);

                                /* Player Name (with flag) */
                                ImGui::TableSetColumnIndex(0);
                                if (slot->countryCode[0] != '\0') {
                                    SDL_Texture *flagTex = flagsGetTexture(slot->countryCode);
                                    if (flagTex) {
                                        ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                                        ImGui::SameLine();
                                    }
                                }
                                if (!slot->isBot) {
                                    uint8_t pflags = slot->clientFlags;
                                    if (clientSimIsSinglePlayer(cs) || clientSimIsLanOnly(cs)) {
                                        pflags &= ~PLAYER_FLAG_WBN_VERIFIED;
                                    }
                                    renderPlayerName(NULL,
                                                     pflags,
                                                     slot->clientType,
                                                     "", false);
                                }
                                if (slot->isBot) {
                                    MessageArgs args = {};
                                    strncpy(args.playerName, slot->playerName, sizeof(args.playerName) - 1);
                                    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                                                       langGetTextFmt(STR_DLGLOBBY_BOT_FMT, &args));
                                } else if (isMe) {
                                    MessageArgs args = {};
                                    strncpy(args.playerName, slot->playerName, sizeof(args.playerName) - 1);
                                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s",
                                                       langGetTextFmt(STR_DLGLOBBY_YOU_FMT, &args));
                                } else {
                                    ImGui::Text("%s", slot->playerName);
                                }

                                /* Ping */
                                ImGui::TableSetColumnIndex(1);
                                if (slot->pingMs > 0) {
                                    ImVec4 pingColor;
                                    if (slot->pingMs < 50)        pingColor = ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
                                    else if (slot->pingMs < 150)   pingColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
                                    else                                        pingColor = ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
                                    ImGui::TextColored(pingColor, "%dms", (int)slot->pingMs);
                                } else {
                                    ImGui::TextDisabled("-");
                                }

                                /* Team */
                                ImGui::TableSetColumnIndex(2);
                                if (isMe && hasTransport) {
                                    int teamIdx = slot->teamNumber;
                                    ImGui::SetNextItemWidth(-1);
                                    char comboId[16];
                                    SDL_snprintf(comboId, sizeof(comboId), "##team%d", i);
                                    if (ImGui::Combo(comboId, &teamIdx, teamItems, 17)) {
                                        lobbySendTeamSet(cs, (uint8_t)myPlayerNum, (uint8_t)teamIdx);
                                    }
                                } else {
                                    if (slot->teamNumber > 0) {
                                        ImGui::Text("%d", slot->teamNumber);
                                    } else {
                                        ImGui::TextDisabled("%s", langGetText(STR_NONE));
                                    }
                                }
                                if (clientSimIsBalanceProposalActive(cs) && clientSimGetBalanceProposal(cs, (BYTE)i) != 0 &&
                                    clientSimGetBalanceProposal(cs, (BYTE)i) != slot->teamNumber) {
                                    ImGui::SameLine();
                                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "-> %d", clientSimGetBalanceProposal(cs, (BYTE)i));
                                }

                                /* Ready */
                                ImGui::TableSetColumnIndex(3);
                                if (slot->ready) {
                                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", langGetText(STR_YES));
                                } else {
                                    ImGui::TextDisabled("%s", langGetText(STR_NO));
                                }

                                /* Action */
                                ImGui::TableSetColumnIndex(4);
                                if (slot->isBot && hasTransport) {
                                    char btnId[64];
                                    SDL_snprintf(btnId, sizeof(btnId), "%s##%d", langGetText(STR_DLGLOBBY_REMOVE), i);
                                    if (ImGui::SmallButton(btnId)) {
                                        lobbySendRemoveBot(cs, (uint8_t)i);
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
                                if (botsAllowed && hasTransport) {
                                    char btnId[64];
                                    SDL_snprintf(btnId, sizeof(btnId), "%s##%d", langGetText(STR_DLGLOBBY_ADDBOT), i);
                                    bool pending = lobbyAddBotPending(cs);
                                    if (pending) ImGui::BeginDisabled();
                                    if (ImGui::SmallButton(btnId)) {
                                        lobbySendAddBotDebounced(cs, -1, 0);
                                    }
                                    if (pending) ImGui::EndDisabled();
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

                    /* "Choose Map" — opens the separate chooser window.
                     * Host / admin / openHost-allowed only; non-privileged
                     * clients never see the button (server enforces the
                     * authority gate when Phase 2 wires the packet). */
                    {
                        bool isHostLocal  = (myPlayerNum == 0);
                        bool isAdminLocal = (myPlayerNum < MAX_TANKS &&
                            (clientSimGetLobbySlot(cs, (BYTE)myPlayerNum)->clientFlags
                             & PLAYER_FLAG_ADMIN));
                        bool effHostMap = isHostLocal || isAdminLocal ||
                                          clientSimGetLobbyOpenHost(cs);
                        if (effHostMap) {
                            if (ImGui::Button("Choose Map")) {
                                lobbyChooseMapOpen(cs, renderer);
                            }
                            ImGui::Spacing();
                            /* Adjust remaining tab height for the button
                             * row we just consumed so the preview below
                             * keeps its aspect ratio. */
                            tabH -= ImGui::GetFrameHeightWithSpacing()
                                  + ImGui::GetStyle().ItemSpacing.y;
                        }
                    }

                    if (!clientSimIsMapDownloadComplete(cs)) {
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
                    ImGui::Text("%s - %dP %dB %dS", clientSimGetMapName(cs), clientSimGetLobbyPillCount(cs), clientSimGetLobbyBaseCount(cs), clientSimGetLobbyStartCount(cs));

                    if (clientSimIsMapSkipAvailable(cs) && clientSimIsInLobby(cs)) {
                        ImGui::Spacing();
                        bool countdownActive = clientSimGetCountdownSeconds(cs) > 0;
                        if (countdownActive) ImGui::BeginDisabled();
                        bool voted = clientSimIsMapSkipMyVote(cs);
                        const char *skipLabel = voted ? langGetText(STR_DLGLOBBY_CANCELSKIP) : langGetText(STR_DLGLOBBY_SKIPMAP);
                        if (voted) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.3f, 0.05f, 1.0f));
                        }
                        if (ImGui::Button(skipLabel, ImVec2(100 * s, 0))) {
                            clientSimSetMapSkipMyVote(cs, !clientSimIsMapSkipMyVote(cs));
                            if (hasTransport) {
                                clientSimNetSendMapSkipVote(cs);
                            }
                        }
                        if (voted) {
                            ImGui::PopStyleColor(3);
                        }
                        ImGui::SameLine();
                        int skipCount = 0, humanCount = 0;
                        for (int j = 0; j < MAX_TANKS; j++) {
                            const ClientLobbySlot *jSlot = clientSimGetLobbySlot(cs, (BYTE)j);
                            if (jSlot && jSlot->connected && !jSlot->isBot) {
                                humanCount++;
                                if (clientSimIsMapSkipVote(cs, (BYTE)j)) skipCount++;
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
                        ImGui::TextUnformatted(clientSimGetLobbyChatHistory(cs));
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
                                !chatEmpty && hasTransport) {
                                clientSimNetSendChat(cs, 0xFF, chatInput);
                                const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
                                const char *myName = (mySlot && mySlot->connected)
                                    ? mySlot->playerName : langGetText(STR_DLGLOBBY_ME);
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
                const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
                bool myReady = (mySlot && mySlot->connected) ? mySlot->ready : false;
                bool canReady = clientSimIsMapDownloadComplete(cs);

                if (!canReady) ImGui::BeginDisabled();
                const char *readyLabel = myReady ? langGetText(STR_DLGLOBBY_UNREADY) : langGetText(STR_DLGLOBBY_READY);
                if (ImGui::Button(readyLabel, ImVec2(100 * s, 0))) {
                    if (hasTransport) {
                        lobbySendReadyToggle(cs, !myReady);
                    }
                }
                if (!canReady) ImGui::EndDisabled();

                if (myPlayerNum == 0 && hasTransport && !clientSimIsBalanceProposalActive(cs)) {
                    bool hasWbnPlayers = false;
                    uint8_t connectedCount = 0;
                    for (int j = 0; j < 16; j++) {
                        const ClientLobbySlot *jSlot = clientSimGetLobbySlot(cs, (BYTE)j);
                        if (jSlot && jSlot->connected) {
                            connectedCount++;
                            if (jSlot->clientFlags & PLAYER_FLAG_WBN_VERIFIED) {
                                hasWbnPlayers = true;
                            }
                        }
                    }
                    if (hasWbnPlayers) {
                        ImGui::SameLine(0, 20);
                        if (connectedCount < 2) ImGui::BeginDisabled();
                        if (ImGui::Button(langGetText(STR_DLGLOBBY_BALANCE_TEAMS), ImVec2(120 * s, 0))) {
                            uint8_t teamSize = (connectedCount > 1) ? (connectedCount / 2) : 1;
                            clientSimNetSendBalanceRequest(cs, teamSize);
                        }
                        if (connectedCount < 2) ImGui::EndDisabled();
                    }
                } else if (myPlayerNum == 0 && hasTransport && clientSimIsBalanceProposalActive(cs)) {
                    ImGui::SameLine(0, 20);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                    if (ImGui::Button(langGetText(STR_DLGLOBBY_APPLY_BALANCE), ImVec2(120 * s, 0))) {
                        clientSimNetSendBalanceApply(cs);
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 8);
                    if (ImGui::Button(langGetText(STR_DLGLOBBY_DISMISS), ImVec2(80 * s, 0))) {
                        clientSimNetSendBalanceDismiss(cs);
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
            /* Panel reservation: leave room for the chat label + chat
             * history + chat input + ready/leave button row.
             * Chat history is 3.4 lines tall (the chat-history child
             * below) — 15% shorter than the original 4 lines. Reducing
             * the reservation by the same amount lets the teams + map
             * panels grow into the freed space and the chat block
             * naturally slides lower. */
            float availContentH = ImGui::GetContentRegionAvail().y
                           - ImGui::GetTextLineHeightWithSpacing() * 8.4f  /* chat + buttons */
                           - 20.0f * s - padB;
            /* Map panel: scales to ~30% of available width on larger
             * screens, but never shrinks below the natural preview
             * size (so on small windows the map stays readable and
             * the teams take whatever extra room there is). */
            float mapPanelW = ImMax((MAP_PREVIEW_SIZE + 20) * s,
                                    availW * 0.30f);
            float playerPanelW = availW - mapPanelW - 8.0f;
            float panelH = availContentH;

            /* "Allow New Players" row spans the full width above both
             * panels so PlayerPanel and MapPanel top edges align in Y.
             * Subtract its actual height (if anything renders) from
             * panelH so the bottom alignment with the chat row holds. */
            float beforeAllowY = ImGui::GetCursorPosY();
            renderAllowNewPlayersRow(cs, myPlayerNum, s);
            float allowRowH = ImGui::GetCursorPosY() - beforeAllowY;
            if (allowRowH > 0.0f) {
                panelH -= allowRowH;
            }

            /* Bottom row reservation must match what the (now grouped)
             * left column consumes: ChatBlock height + spacing. Compute
             * it up front so the right column's MapPanel can grow into
             * everything except the Ready/Balance footer. */
            float bottomH = ImGui::GetTextLineHeightWithSpacing() * 8.4f - 20.0f * s;
            if (bottomH < ImGui::GetTextLineHeightWithSpacing() * 5.0f) {
                bottomH = ImGui::GetTextLineHeightWithSpacing() * 5.0f;
            }
            float spacingH = ImGui::GetStyle().ItemSpacing.y;
            /* Reserve room for the bottom buttons below the map.
             *   - Always one row for Ready.
             *   - One extra row for Balance / Apply / Dismiss only when
             *     it'll actually render: host + transport + at least
             *     one other WBN-verified player, OR an active balance
             *     proposal. In SP / LAN that whole branch is skipped,
             *     so reserving for it pushes Ready below the chat's
             *     Send button and shrinks the map for no reason. */
            bool balanceRowReserve = false;
            if (myPlayerNum == 0 && hasTransport) {
                if (clientSimIsBalanceProposalActive(cs)) {
                    balanceRowReserve = true;
                } else {
                    int wbnCount = 0;
                    for (int j = 0; j < 16; j++) {
                        const ClientLobbySlot *jSlot =
                            clientSimGetLobbySlot(cs, (BYTE)j);
                        if (jSlot && jSlot->connected &&
                            (jSlot->clientFlags & PLAYER_FLAG_WBN_VERIFIED)) {
                            wbnCount++;
                        }
                    }
                    if (wbnCount >= 1) balanceRowReserve = true;
                }
            }
            float readyAreaH = ImGui::GetFrameHeightWithSpacing()
                             * (balanceRowReserve ? 2.0f : 1.0f)
                             + spacingH;
            float mapH = panelH + bottomH + spacingH - readyAreaH - spacingH;
            if (mapH < panelH) mapH = panelH;

            /* Left column — PlayerPanel above ChatBlock, grouped so the
             * right column can SameLine alongside the whole stack. */
            ImGui::BeginGroup();

            /* Left: Player panel — Layout A team-grouped rendering. */
            ImGui::BeginChild("##PlayerPanel", ImVec2(playerPanelW, panelH), ImGuiChildFlags_None);

            /* Layout A: team-grouped player rendering. Legacy 6-column
             * table preserved below the #if 0 for reference; toggle to
             * A/B compare during the in-progress UI rewrite. */
#if 1
            {
                bool isHostHere = (myPlayerNum == 0);
                renderTeamGroupedPlayers(cs, myPlayerNum, s, isHostHere);
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

                bool botsAllowed = (clientSimGetLobbyAiType(cs) != 0);

                for (int i = 0; i < MAX_TANKS; i++) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", i);

                    const ClientLobbySlot *slot = clientSimGetLobbySlot(cs, (BYTE)i);
                    if (slot && slot->connected) {
                        bool isMe = (i == myPlayerNum);

                        /* Player Name (with flag icon) */
                        ImGui::TableSetColumnIndex(1);
                        if (slot->countryCode[0] != '\0') {
                            SDL_Texture *flagTex = flagsGetTexture(slot->countryCode);
                            if (flagTex) {
                                ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                                ImGui::SameLine();
                            }
                        }
                        if (!slot->isBot) {
                            uint8_t pflags = slot->clientFlags;
                            if (clientSimIsSinglePlayer(cs) || clientSimIsLanOnly(cs)) {
                                pflags &= ~PLAYER_FLAG_WBN_VERIFIED;
                            }
                            renderPlayerName(NULL,
                                             pflags,
                                             slot->clientType,
                                             "", false);
                        }
                        if (slot->isBot) {
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                                               "%s [Bot]", slot->playerName);
                        } else if (isMe) {
                            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
                                               "%s (You)", slot->playerName);
                        } else {
                            ImGui::Text("%s", slot->playerName);
                        }

                        /* Ping */
                        ImGui::TableSetColumnIndex(2);
                        if (slot->pingMs > 0) {
                            ImVec4 pingColor;
                            if (slot->pingMs < 50)        pingColor = ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
                            else if (slot->pingMs < 150)   pingColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
                            else                                        pingColor = ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
                            ImGui::TextColored(pingColor, "%dms", (int)slot->pingMs);
                        } else {
                            ImGui::TextDisabled("-");
                        }

                        /* Team */
                        ImGui::TableSetColumnIndex(3);
                        if (isMe && hasTransport) {
                            int teamIdx = slot->teamNumber;
                            ImGui::SetNextItemWidth(-1);
                            char comboId[16];
                            SDL_snprintf(comboId, sizeof(comboId), "##team%d", i);
                            if (ImGui::Combo(comboId, &teamIdx, teamItems, 17)) {
                                lobbySendTeamSet(cs, (uint8_t)myPlayerNum, (uint8_t)teamIdx);
                            }
                        } else {
                            if (slot->teamNumber > 0) {
                                ImGui::Text("%d", slot->teamNumber);
                            } else {
                                ImGui::TextDisabled("%s", langGetText(STR_NONE));
                            }
                        }
                        if (clientSimIsBalanceProposalActive(cs) && clientSimGetBalanceProposal(cs, (BYTE)i) != 0 &&
                            clientSimGetBalanceProposal(cs, (BYTE)i) != slot->teamNumber) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "-> %d", clientSimGetBalanceProposal(cs, (BYTE)i));
                        }

                        /* Ready */
                        ImGui::TableSetColumnIndex(4);
                        if (slot->ready) {
                            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", langGetText(STR_YES));
                        } else {
                            ImGui::TextDisabled("%s", langGetText(STR_NO));
                        }

                        /* Action */
                        ImGui::TableSetColumnIndex(5);
                        if (slot->isBot && hasTransport) {
                            char btnId[16];
                            SDL_snprintf(btnId, sizeof(btnId), "Remove##%d", i);
                            if (ImGui::SmallButton(btnId)) {
                                lobbySendRemoveBot(cs, (uint8_t)i);
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
                        if (botsAllowed && hasTransport) {
                            char btnId[16];
                            SDL_snprintf(btnId, sizeof(btnId), "Add Bot##%d", i);
                            bool pending = lobbyAddBotPending(cs);
                            if (pending) ImGui::BeginDisabled();
                            if (ImGui::SmallButton(btnId)) {
                                lobbySendAddBotDebounced(cs, -1, 0);
                            }
                            if (pending) ImGui::EndDisabled();
                        }
                    }
                }
                ImGui::EndTable();
            }

            ImGui::EndChild(); /* ##PlayerPanel */

            ImGui::Spacing();

            /* Left bottom: chat block (label + history + input row),
             * still in the left column group so it stacks under
             * PlayerPanel. */
            ImGui::BeginChild("##ChatBlock", ImVec2(playerPanelW, bottomH), ImGuiChildFlags_None);
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_CHAT));
            {
                float inputRowH = ImGui::GetFrameHeightWithSpacing();
                float chatHeight = ImGui::GetContentRegionAvail().y - inputRowH;
                if (chatHeight < ImGui::GetTextLineHeightWithSpacing() * 3.4f)
                    chatHeight = ImGui::GetTextLineHeightWithSpacing() * 3.4f;
                ImGui::BeginChild("##ChatHistory", ImVec2(0, chatHeight), ImGuiChildFlags_Borders);
                ImGui::TextUnformatted(clientSimGetLobbyChatHistory(cs));
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
                    !chatEmpty && hasTransport) {
                    clientSimNetSendChat(cs, 0xFF, chatInput);
                    const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
                    const char *myName = (mySlot && mySlot->connected)
                        ? mySlot->playerName : langGetText(STR_DLGLOBBY_ME);
                    clientSimAppendLobbyChat(cs, myName, chatInput);
                    chatInput[0] = '\0';
                }
            }
            ImGui::EndChild();

            ImGui::EndGroup(); /* /left column */

            ImGui::SameLine(0, 8.0f);

            /* Right column — MapPanel extends down to just above the
             * Ready/Balance footer, so the map preview's bottom border
             * sits on the same Y as the Ready button's top. */
            ImGui::BeginGroup();

            /* Right: Map preview + info */
            ImGui::BeginChild("##MapPanel", ImVec2(mapPanelW, mapH), ImGuiChildFlags_Borders);

            if (!clientSimIsMapDownloadComplete(cs)) {
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
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MAP_LBL), clientSimGetMapName(cs));
            ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_PILLBOXES), clientSimGetLobbyPillCount(cs));
            ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_BASES), clientSimGetLobbyBaseCount(cs));
            ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_STARTS), clientSimGetLobbyStartCount(cs));

            if (clientSimIsMapSkipAvailable(cs) && clientSimIsInLobby(cs)) {
                ImGui::Spacing();
                bool countdownActive = clientSimGetCountdownSeconds(cs) > 0;
                if (countdownActive) ImGui::BeginDisabled();
                bool voted = clientSimIsMapSkipMyVote(cs);
                const char *skipLabel = langGetText(
                    voted ? STR_DLGLOBBY_CANCELSKIP : STR_DLGLOBBY_SKIPMAP);
                if (voted) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.3f, 0.05f, 1.0f));
                }
                if (ImGui::Button(skipLabel, ImVec2(100 * s, 0))) {
                    clientSimSetMapSkipMyVote(cs, !clientSimIsMapSkipMyVote(cs));
                    if (hasTransport) {
                        clientSimNetSendMapSkipVote(cs);
                    }
                }
                if (voted) {
                    ImGui::PopStyleColor(3);
                }
                ImGui::SameLine();
                int skipCount = 0, humanCount = 0;
                for (int j = 0; j < MAX_TANKS; j++) {
                    const ClientLobbySlot *jSlot = clientSimGetLobbySlot(cs, (BYTE)j);
                    if (jSlot && jSlot->connected && !jSlot->isBot) {
                        humanCount++;
                        if (clientSimIsMapSkipVote(cs, (BYTE)j)) skipCount++;
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

            /* "Choose Map" — host / admin / openHost-allowed only. Sits
             * at the bottom of the MapPanel, under the preview and map
             * info, so it reads as "change the current map" rather than
             * a header action. */
            {
                bool isHostLocal  = (myPlayerNum == 0);
                bool isAdminLocal = (myPlayerNum < MAX_TANKS &&
                    (clientSimGetLobbySlot(cs, (BYTE)myPlayerNum)->clientFlags
                     & PLAYER_FLAG_ADMIN));
                bool effHostMap = isHostLocal || isAdminLocal ||
                                  clientSimGetLobbyOpenHost(cs);
                if (effHostMap) {
                    ImGui::Spacing();
                    if (ImGui::Button("Choose Map", ImVec2(-1, 0))) {
                        lobbyChooseMapOpen(cs, renderer);
                    }
                }
            }

            ImGui::EndChild(); /* ##MapPanel */

            /* Ready / Balance buttons sit directly below MapPanel inside
             * the right column group. The MapPanel height was sized so
             * its bottom border lands just above this footer, aligning
             * with the top of the Ready button. */
            {
                const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
                bool myReady = (mySlot && mySlot->connected) ? mySlot->ready : false;
                bool canReady = clientSimIsMapDownloadComplete(cs);

                if (myPlayerNum == 0 && hasTransport && !clientSimIsBalanceProposalActive(cs)) {
                    bool hasWbnPlayers = false;
                    uint8_t connectedCount = 0;
                    for (int j = 0; j < 16; j++) {
                        const ClientLobbySlot *jSlot = clientSimGetLobbySlot(cs, (BYTE)j);
                        if (jSlot && jSlot->connected) {
                            connectedCount++;
                            if (jSlot->clientFlags & PLAYER_FLAG_WBN_VERIFIED) {
                                hasWbnPlayers = true;
                            }
                        }
                    }
                    if (hasWbnPlayers) {
                        if (connectedCount < 2) ImGui::BeginDisabled();
                        if (ImGui::Button(langGetText(STR_DLGLOBBY_BALANCE_TEAMS), ImVec2(-1, 0))) {
                            uint8_t teamSize = (connectedCount > 1) ? (connectedCount / 2) : 1;
                            clientSimNetSendBalanceRequest(cs, teamSize);
                        }
                        if (connectedCount < 2) ImGui::EndDisabled();
                    }
                } else if (myPlayerNum == 0 && hasTransport && clientSimIsBalanceProposalActive(cs)) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                    if (ImGui::Button(langGetText(STR_DLGLOBBY_APPLY_BALANCE), ImVec2(-1, 0))) {
                        clientSimNetSendBalanceApply(cs);
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::Button(langGetText(STR_DLGLOBBY_DISMISS), ImVec2(-1, 0))) {
                        clientSimNetSendBalanceDismiss(cs);
                    }
                }

                if (!canReady) ImGui::BeginDisabled();
                const char *readyLabel = myReady ? langGetText(STR_DLGLOBBY_UNREADY) : langGetText(STR_DLGLOBBY_READY);
                if (ImGui::Button(readyLabel, ImVec2(-1, 0))) {
                    if (hasTransport) {
                        lobbySendReadyToggle(cs, !myReady);
                    }
                }
                if (!canReady) ImGui::EndDisabled();
            }
            ImGui::EndGroup(); /* /right column */
        } /* /desktop layout scope (playerPanelW/mapPanelW) */
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
        if (clientSimGetNetStatus(cs) == netLobbyCountdown && clientSimGetCountdownSeconds(cs) > 0) {
            char countdownText[64];
            MessageArgs args = {};
            args.number = clientSimGetCountdownSeconds(cs);
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

        /* Map chooser sub-window (Phase 1). Rendered after the main
         * lobby End() so it's a top-level ImGui window that the user
         * can drag around freely. Only renders when s_chooseMapOpen is
         * true, set by the "Choose Map" button on the Map tab. */
        lobbyChooseMapRenderWindow(cs, renderer, s, screenW, screenH);

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
