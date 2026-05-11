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
#include "../../../bolo/global.h"
#include "../../../bolo/client_sim.h"
#include "../../../bolo/transport.h"
#include "../../../bolo/transport_udp.h"
#include "../../../server/server_lifecycle.h"
#include "../../../server/server_sim.h"
#include "../../../bolo/bolo_map.h"
#include "../../../bolo/pillbox.h"
#include "../../../bolo/bases.h"
#include "../../../bolo/starts.h"
#include "../../../bolo/bot_manager.h"
#include "../../../bolo/players.h"
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
 * mirrored back into the ClientSim via serverSimSyncLobbyToClient.
 *
 * Each helper takes both the ClientSim (to detect single-player) and
 * the Transport pointer (used by the multiplayer path).  When the
 * Transport is null and we're not single-player, the call is a no-op.
 */
static void lobbySendReadyToggle(ClientSim *cs, Transport *transport, bool ready) {
    if (cs && cs->isSinglePlayer) {
        /* Single-player: skip the multiplayer ready→countdown
         * choreography; clicking the Ready button starts the game
         * straight away.  The Unready direction is meaningless here
         * (there's nobody to wait on) so we ignore ready=false. */
        if (ready) {
            (void)gameFrontStartSinglePlayerGame(cs);
        }
        return;
    }
    if (transport) {
        transportUdpClientSendReady(transport, ready);
    }
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
static void lobbySendAddBot(ClientSim *cs, Transport *transport,
                            int namingPool, uint8_t teamNumber) {
    if (cs && cs->isSinglePlayer) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (sim->state != serverStateLobby) return;
        /* Find first free slot */
        BYTE slot;
        for (slot = 1; slot < MAX_TANKS; slot++) {
            if (!sim->playerConnected[slot]) break;
        }
        if (slot >= MAX_TANKS) return;
        if (sim->botBrainPath[0] == '\0') return;

        /* Pick a name. If a pool override is supplied we use it; else
         * fall back to "Bot N". Build the used-names list from current
         * lobby slots so the picker doesn't collide with existing bots. */
        char botName[32];
        if (namingPool >= 0 && namingPool < lobbyBotPoolCount()) {
            const char *usedNames[MAX_TANKS];
            int usedCount = 0;
            for (int i = 0; i < MAX_TANKS; i++) {
                if (sim->playerConnected[i]) {
                    usedNames[usedCount++] = cs->lobbySlots[i].playerName;
                }
            }
            lobbyBotPoolPick(namingPool, usedNames, usedCount, botName, sizeof(botName));
        } else {
            snprintf(botName, sizeof(botName), "Bot %d", slot);
        }

        botManagerAddBot(sim, slot, sim->botBrainPath, botName,
                         (aiType)sim->botAiType,
                         (gameType)cs->lobbyGameType,
                         cs->lobbyHiddenMines);
        if (teamNumber > 0 && teamNumber < MAX_TANKS) {
            sim->lobbyPlayers[slot].teamNumber = teamNumber;
        }
        /* Bot's name came from the pool — not an override. */
        s_botNameOverridden[slot] = false;
        serverSimSyncLobbyToClient(sim, cs);
        return;
    }
    if (transport) transportUdpClientSendAddBot(transport);
}

static void lobbySendRemoveBot(ClientSim *cs, Transport *transport, uint8_t slot) {
    if (cs && cs->isSinglePlayer) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (sim->state != serverStateLobby) return;
        botManagerRemoveBot(sim, slot);
        if (slot < MAX_TANKS) s_botNameOverridden[slot] = false;
        serverSimSyncLobbyToClient(sim, cs);
        return;
    }
    if (transport) transportUdpClientSendRemoveBot(transport, slot);
}

static void lobbySendTeamSet(ClientSim *cs, Transport *transport, uint8_t teamNumber) {
    if (cs && cs->isSinglePlayer) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (cs->myPlayerNum < MAX_TANKS) {
            sim->lobbyPlayers[cs->myPlayerNum].teamNumber = teamNumber;
            serverSimSyncLobbyToClient(sim, cs);
        }
        return;
    }
    if (transport) transportUdpClientSendTeamSet(transport, teamNumber);
}

/* Update a bot's per-slot config (difficulty / personality / name
 * override). Mirrors the server-side PACKET_LOBBY_BOT_CONFIG handler.
 * In single-player we write directly into spServerSim->botConfigs and
 * (if name supplied) update the players struct so the lobby slot's
 * playerName changes as well. */
static void lobbySendBotConfig(ClientSim *cs, Transport *transport,
                               uint8_t slot,
                               uint8_t difficulty, uint8_t personality,
                               const char *name) {
    if (cs && cs->isSinglePlayer) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || slot >= MAX_TANKS) return;
        if (sim->state != serverStateLobby) return;
        if (difficulty > 2 || personality > 3) return;
        if (!sim->lobbyPlayers[slot].isBot) return;
        sim->botConfigs[slot].difficulty  = difficulty;
        sim->botConfigs[slot].personality = personality;
        if (name && name[0] != '\0') {
            char nameBuf[32];
            strncpy(nameBuf, name, sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            char loc[3] = "??";
            playersSetPlayer(NULL, &sim->sim.plyrs, NEUTRAL, slot,
                             nameBuf, loc,
                             0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);
        }
        serverSimSyncLobbyToClient(sim, cs);
        return;
    }
    if (transport) {
        transportUdpClientSendLobbyBotConfig(transport, slot,
            difficulty, personality, name);
    }
}

/* Clear a team's metadata (color/name/pool back to defaults).
 * Mirrors PACKET_LOBBY_TEAM_CLEAR. Only called when the team is
 * empty — caller already gates on memberCount == 0. */
static void lobbySendTeamClear(ClientSim *cs, Transport *transport,
                               uint8_t teamId) {
    if (cs && cs->isSinglePlayer) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
        memset(&sim->teams[teamId], 0, sizeof(sim->teams[teamId]));
        serverSimSyncLobbyToClient(sim, cs);
        return;
    }
    if (transport) {
        transportUdpClientSendLobbyTeamClear(transport, teamId);
    }
}

/* Update a team's naming pool. Mirrors the per-team naming dropdown
 * which previously sent transportUdpClientSendLobbyTeamMeta directly.
 * Single-player path mutates spServerSim->teams[teamId] in-place AND
 * re-rolls every bot on the team whose name wasn't manually
 * overridden so the new pool's vibe applies immediately. */
static void lobbySendTeamPool(ClientSim *cs, Transport *transport,
                              uint8_t teamId, uint8_t namingPool,
                              const char *teamName) {
    if (cs && cs->isSinglePlayer) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
        TeamMetadata *t = &sim->teams[teamId];
        t->in_use = 1;
        t->namingPool = namingPool;
        if (teamName && teamName[0]) {
            strncpy(t->name, teamName, LOBBY_TEAM_NAME_LEN - 1);
            t->name[LOBBY_TEAM_NAME_LEN - 1] = '\0';
        }
        serverSimSyncLobbyToClient(sim, cs);

        /* Rename bots on this team whose names weren't overridden by
         * the host. We pick names sequentially from the new pool,
         * each call's used-list including everyone already on the
         * lobby plus the names we've assigned in this loop, so the
         * picker doesn't collide with itself. */
        const char *usedNames[MAX_TANKS];
        int usedCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            if (sim->playerConnected[i]) {
                usedNames[usedCount++] = cs->lobbySlots[i].playerName;
            }
        }
        for (int slot = 0; slot < MAX_TANKS; slot++) {
            if (!sim->playerConnected[slot]) continue;
            if (!sim->lobbyPlayers[slot].isBot) continue;
            if (sim->lobbyPlayers[slot].teamNumber != teamId) continue;
            if (s_botNameOverridden[slot]) continue;

            char pickBuf[32];
            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             pickBuf, sizeof(pickBuf));
            lobbySendBotConfig(cs, transport, (uint8_t)slot,
                               cs->lobbyBotDifficulty[slot],
                               cs->lobbyBotPersonality[slot],
                               pickBuf);
            /* Replace this slot's entry in usedNames so subsequent
             * picks see the updated name (avoids picking the same
             * name twice within the loop). */
            for (int u = 0; u < usedCount; u++) {
                if (usedNames[u] == cs->lobbySlots[slot].playerName) {
                    usedNames[u] = cs->lobbySlots[slot].playerName;
                    break;
                }
            }
        }
        return;
    }
    if (transport) {
        transportUdpClientSendLobbyTeamMeta(transport, teamId,
            cs->lobbyTeamColor[teamId], namingPool, teamName);
    }
}

/* Game-settings dispatcher. settingType is one of LST_* (netpacks.h);
 * payload is 1 or 2 bytes per server-side parser. Mirrors what
 * transport_udp_server.c::PACKET_LOBBY_SET_SETTING does to spServerSim
 * for the single-player path, then re-syncs so the lobby UI's read
 * of cs->lobby* sees the new value on the next frame. Without the
 * sync the checkbox/radio flashes for one frame and reverts. */
static void lobbySendSetting(ClientSim *cs, Transport *transport,
                             uint8_t settingType,
                             const uint8_t *value, uint8_t valueLen) {
    if (cs && cs->isSinglePlayer) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (sim->state != serverStateLobby) return;
        switch (settingType) {
            case 1 /* LST_GAME_TYPE */:
                /* gameType enum is 1..3 (Open / Tournament / Strict).
                 * The wire carries the raw enum value. */
                if (valueLen == 1 && value[0] >= 1 && value[0] <= 3) {
                    sim->sim.game = (gameType)value[0];
                }
                break;
            case 2 /* LST_HIDDEN_MINES */:
                if (valueLen == 1) sim->sim.hiddenMines = value[0] != 0;
                break;
            case 3 /* LST_AI_POLICY */:
                if (valueLen == 1 && value[0] <= 3) {
                    sim->aiPolicy = value[0];
                    sim->botAiType = (aiType)value[0];
                }
                break;
            case 4 /* LST_TIME_LIMIT */:
                if (valueLen == 1) {
                    sim->timeLimit = value[0] != 0;
                    /* When the host turns the time limit off, gameLength
                     * goes to TIME_UNLIMITED; on, derive from current
                     * timeMinutes mirror so the lobby still reflects
                     * the previously-set value. */
                    if (!sim->timeLimit) {
                        sim->gameLength = -1;
                    } else if (sim->timeMinutes > 0) {
                        sim->gameLength = (int32_t)sim->timeMinutes * 60 * 50;
                    }
                }
                break;
            case 5 /* LST_TIME_MINUTES */:
                if (valueLen == 2) {
                    sim->timeMinutes = (uint16_t)((value[0] << 8) | value[1]);
                    if (sim->timeLimit) {
                        sim->gameLength = (int32_t)sim->timeMinutes * 60 * 50;
                    }
                }
                break;
            case 6 /* LST_AUTO_LOCK_ON_GAME */:
                if (valueLen == 1) sim->autoLockOnGameStart = value[0] != 0;
                break;
        }
        serverSimSyncLobbyToClient(sim, cs);
        return;
    }
    if (transport) {
        transportUdpClientSendLobbySetting(transport, settingType, value, valueLen);
    }
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

    /* Header: "Add Team" button (host only) and the openHost toggle.
     * Add Team picks the lowest unused teamId, sends a default-name
     * TEAM_META. The openHost toggle lets non-host players manage
     * teams + bots; the wording flips between checked/unchecked
     * states so each phrasing reads truthfully. */
    if (effectiveHost) {
        if (ImGui::Button("+ Add Team")) {
            for (int t = 1; t < 16; t++) {
                if (memberCount[t] == 0 && !cs->lobbyTeamInUse[t]) {
                    char defaultName[16];
                    SDL_snprintf(defaultName, sizeof(defaultName), "Team %d", t);
                    uint8_t color = (uint8_t)((t - 1) & 7);
                    /* Single-player: write the team metadata directly
                     * (UDP transport reinterpret would corrupt memory,
                     * and the wire's TEAM_META has no SP equivalent).
                     * Multiplayer: existing PACKET_LOBBY_TEAM_META. */
                    if (cs->isSinglePlayer) {
                        ServerSim *spSim = gameFrontGetSinglePlayerServerSim();
                        if (spSim) {
                            TeamMetadata *tm = &spSim->teams[t];
                            tm->in_use = 1;
                            tm->color = color;
                            tm->namingPool = 0;
                            strncpy(tm->name, defaultName, LOBBY_TEAM_NAME_LEN - 1);
                            tm->name[LOBBY_TEAM_NAME_LEN - 1] = '\0';
                            serverSimSyncLobbyToClient(spSim, cs);
                        }
                    } else if (transport) {
                        transportUdpClientSendLobbyTeamMeta(transport, (uint8_t)t,
                            color, 0 /*pool=classic*/, defaultName);
                    }
                    break;
                }
            }
        }
        if (myPlayerNum == 0) {
            ImGui::SameLine(0.0f, 16.0f * s);
            bool oh = cs->lobbyOpenHost;
            if (ImGui::Checkbox("Everybody can manage teams", &oh)) {
                transportUdpClientSendLobbyOpenHost(transport, oh);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(oh
                    ? "Anyone in the lobby can add/rename/remove teams and bots."
                    : "Only the host can manage teams.");
            }
        }
        ImGui::Spacing();
    }
    for (int teamId = 1; teamId < 16; teamId++) {
        /* Teams 1 and 2 are always rendered (the lobby's two default
         * sides) — the host always has somewhere to drop the first
         * bot. Teams 3..15 render when they have members OR when the
         * host has explicitly added them via "+ Add Team"
         * (lobbyTeamInUse=1). Without the in_use check, freshly added
         * teams would vanish on the same frame because they have no
         * members yet. */
        bool persistTeam = (teamId == 1 || teamId == 2)
                        || cs->lobbyTeamInUse[teamId];
        if (!persistTeam && memberCount[teamId] == 0) continue;

        /* Team color from theme; falls back to gray for un-themed teams. */
        ImU32 tc;
        uint8_t colorIdx = cs->lobbyTeamColor[teamId];
        if (cs->lobbyTeamInUse[teamId] && colorIdx < 8) {
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
            const float comboW   = 140.0f * s;
            const float botBtnW  = 70.0f * s;
            const float xBtnW    = 22.0f * s;
            const float labelW   = ImGui::CalcTextSize("Bot Naming:").x;
            const float gap      = 6.0f * s;
            bool showNaming = botCount[teamId] > 0;
            int humanCount = memberCount[teamId] - botCount[teamId];
            bool showXBtn   = (teamId >= 3) && (humanCount == 0);
            /* Always reserve the X width so "+ Bot" sits at the same
             * X position across teams with/without an X. */
            float groupW = botBtnW + gap + xBtnW
                         + (showNaming ? labelW + gap + comboW + gap : 0);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - groupW);
            int curPool = cs->lobbyTeamPool[teamId];
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
                        if (ImGui::Selectable(lobbyBotPoolLabel(p), sel)) {
                            const char *nameForMeta = cs->lobbyTeamInUse[teamId]
                                ? cs->lobbyTeamName[teamId] : defaultName;
                            lobbySendTeamPool(cs, transport, (uint8_t)teamId,
                                              (uint8_t)p, nameForMeta);
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine(0.0f, gap);
            }
            char addId[24];
            SDL_snprintf(addId, sizeof(addId), "\xF0\x9F\xA7\xA0 + Bot##ab%d", teamId);
            if (ImGui::Button(addId, ImVec2(botBtnW, 0))) {
                /* If this team's pool hasn't been picked yet, pick a
                 * random one before adding the bot so its name comes
                 * from a varied source instead of always pool 0. */
                int effectivePool = curPool;
                if (!cs->lobbyTeamInUse[teamId] && lobbyBotPoolCount() > 0) {
                    effectivePool = rand() % lobbyBotPoolCount();
                    lobbySendTeamPool(cs, transport, (uint8_t)teamId,
                                      (uint8_t)effectivePool, defaultName);
                }
                lobbySendAddBot(cs, transport, effectivePool, (uint8_t)teamId);
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
            ImGui::SameLine(0.0f, gap);
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
                        if (cs->lobbySlots[i].connected
                            && cs->lobbySlots[i].isBot
                            && cs->lobbySlots[i].teamNumber == teamId) {
                            lobbySendRemoveBot(cs, transport, (uint8_t)i);
                        }
                    }
                    lobbySendTeamClear(cs, transport, (uint8_t)teamId);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Remove Team %d (and its bots).", teamId);
                }
            } else {
                /* Hold the slot so + Bot's X position is stable. */
                ImGui::Dummy(ImVec2(xBtnW, 0));
            }
        }

        ImGui::Separator();

        /* Member rows. */
        bool firstRow = true;
        for (int i = 0; i < MAX_TANKS; i++) {
            if (!cs->lobbySlots[i].connected) continue;
            if (cs->lobbySlots[i].teamNumber != teamId) continue;

            /* Thin separator between rows (not before the first row —
             * the team header already has a Separator() above this loop). */
            if (!firstRow) ImGui::Separator();
            firstRow = false;

            bool isMe = (i == myPlayerNum);
            bool isBot = cs->lobbySlots[i].isBot;

            /* "You" row gets a 25%-darker translucent overlay drawn
             * BEFORE the Indent, so the highlight spans the team
             * panel's full inner width (touches the left edge) while
             * the content still sits 50px in.  Effectively padding
             * rather than margin. */
            float rowH     = ImGui::GetFrameHeightWithSpacing();
            float rowFullW = ImGui::GetContentRegionAvail().x;
            ImVec2 rowFullStart = ImGui::GetCursorScreenPos();
            if (isMe) {
                ImU32 darkOverlay = IM_COL32(0, 0, 0, 64);  /* ~25% black */
                ImGui::GetWindowDrawList()->AddRectFilled(
                    rowFullStart,
                    ImVec2(rowFullStart.x + rowFullW, rowFullStart.y + rowH),
                    darkOverlay, 2.0f);
            }

            /* Indent each member row 50px past the team panel edge so
             * the rows visually sit "inside" the team header rather
             * than directly under it. */
            ImGui::Indent(50.0f * s);

            /* Per-row tank icon — green tank for slots on the local
             * player's team (allies, including bots), red tank for
             * slots on a different team (enemies). The check uses
             * teamNumber, so two players both at team 0 (unassigned)
             * read as on the same team. Vertically centered against
             * the row's frame-padded height. */
            {
                uint8_t myTeam   = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS)
                                   ? cs->lobbySlots[myPlayerNum].teamNumber : 0;
                uint8_t theirTeam = cs->lobbySlots[i].teamNumber;
                bool isSelf  = (i == myPlayerNum);
                /* Ally iff both sides share the same non-zero team
                 * number. Two "unassigned" (team 0) players don't
                 * yet share a team in the game-logic sense, so render
                 * those as enemies until one picks a team. */
                bool isAlly  = (myTeam != 0 && theirTeam == myTeam);
                SDL_Renderer *r  = sdl3DrawGetRenderer();
                SDL_Texture  *tankTex = nullptr;
                if (r) {
                    if (isSelf) {
                        /* Yourself — the black "self" tank
                         * (tank_self_04.png) so your row is
                         * instantly identifiable as you. */
                        tankTex = getTankSelf04Texture(r);
                    } else if (isAlly) {
                        /* Teammates (humans and bots) — green ally
                         * tank (tank_good_04.png). Falls back to the
                         * self tank if the good asset is missing. */
                        tankTex = getTankGood04Texture(r);
                        if (!tankTex) tankTex = getTankSelf04Texture(r);
                    } else {
                        tankTex = getTankEvil04Texture(r);
                    }
                }
                if (tankTex) {
                    float tankSz = 18.0f * s;
                    float frameH = ImGui::GetFrameHeight();
                    /* Center within the frame then nudge 2px lower so
                     * the tank reads visually aligned with the player
                     * name (which also got a 2px nudge). */
                    float yOff   = (frameH - tankSz) * 0.5f + 2.0f * s;
                    if (yOff < 0) yOff = 0;
                    ImVec2 cur = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + yOff));
                    ImGui::Image((ImTextureID)tankTex, ImVec2(tankSz, tankSz));
                    /* Restore Y so siblings on this row stay aligned
                     * to the frame baseline. */
                    ImGui::SameLine(0.0f, 6.0f * s);
                    ImVec2 after = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(ImVec2(after.x, cur.y));
                }
            }

            /* Country flag + platform / WBN / Steam icons (humans
             * only). Order matches the legacy lobby: country first,
             * then renderPlayerName emits the platform icon (with
             * supporter tint), the WBN verified globe, and the Steam
             * badge as needed.  We pass an empty name so it stops at
             * the badges — the actual name text is rendered below
             * with its own coloring. */
            if (!isBot) {
                ImGui::AlignTextToFramePadding();
                if (cs->lobbySlots[i].countryCode[0] != '\0') {
                    SDL_Texture *flagTex = flagsGetTexture(cs->lobbySlots[i].countryCode);
                    if (flagTex) {
                        ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                        ImGui::SameLine();
                    }
                }
                renderPlayerName(NULL,
                                 cs->lobbySlots[i].clientFlags,
                                 cs->lobbySlots[i].clientType,
                                 "", false);
            }

            /* Bot rows: small chrome-less settings.svg icon button on
             * the left that toggles the AiConfig sub-row, then the
             * tinted bot name as plain text.  Human rows: tinted name
             * text only.  All paths use AlignTextToFramePadding so
             * the label baseline matches the row height. */
            if (isBot) {
                /* Bot name first, then the settings (wrench) icon
                 * to its right.  The icon is a chrome-less
                 * InvisibleButton over an AddImage so it shows the
                 * SVG pixels with no border / hover-fill / padding. */
                ImGui::AlignTextToFramePadding();
                /* Nudge the text 2px lower to optically center inside
                 * the row's frame-padded height. */
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f * s);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      wbThemeColor(g_theme->botBadge));
                ImGui::Text("%s [bot]", cs->lobbySlots[i].playerName);
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 6.0f * s);
                if (s_iconSettings) {
                    float iconSize = ImGui::GetFontSize();
                    /* Vertically center the icon within the row's
                     * frame-padded height, then nudge 2px up so it
                     * sits a touch higher than dead-center. */
                    float frameH = ImGui::GetFrameHeight();
                    float yOff = (frameH - iconSize) * 0.5f - 2.0f * s;
                    ImVec2 iconStart = ImGui::GetCursorScreenPos();
                    iconStart.y += yOff;
                    ImGui::SetCursorScreenPos(iconStart);
                    char btnId[24];
                    SDL_snprintf(btnId, sizeof(btnId), "##cfg%d", i);
                    bool clicked = ImGui::InvisibleButton(btnId,
                                                          ImVec2(iconSize, iconSize));
                    ImGui::GetWindowDrawList()->AddImage(
                        (ImTextureID)s_iconSettings,
                        iconStart,
                        ImVec2(iconStart.x + iconSize, iconStart.y + iconSize));
                    if (clicked) {
                        s_expandedBotSlot = (s_expandedBotSlot == i) ? -1 : i;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Configure bot");
                    }
                } else {
                    /* Icon failed to load — fall back to a caret. */
                    char fallId[24];
                    SDL_snprintf(fallId, sizeof(fallId), "%s##cfg%d",
                                 s_expandedBotSlot == i ? "v" : ">", i);
                    if (ImGui::SmallButton(fallId)) {
                        s_expandedBotSlot = (s_expandedBotSlot == i) ? -1 : i;
                    }
                }
            } else {
                ImGui::AlignTextToFramePadding();
                /* Same 2px optical-center nudge as the bot rows. */
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f * s);
                if (isMe) {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s (you)",
                        cs->lobbySlots[i].playerName);
                } else {
                    ImGui::Text("%s", cs->lobbySlots[i].playerName);
                }
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

            /* Bot remove button — host only. Uses ImGui::CloseButton
             * (imgui_internal.h) for the same proper × glyph the
             * team-clear button renders, pinned to the row's right
             * edge so per-row X's line up with the team-clear X.
             *
             * Important: we have to SameLine + advance the cursor
             * with SetCursorPosX rather than passing an absolute
             * screen pos directly to CloseButton — the absolute-pos
             * path runs ItemAdd outside the row's flow and the first
             * such item per BeginChild gets clipped by the scrollable
             * area's initial cull, producing the "first bot has no X
             * but later bots do" symptom. */
            if (isBot && effectiveHost) {
                ImGui::SameLine();
                float closeSz = ImGui::GetFontSize();
                float rightX  = ImGui::GetContentRegionMax().x - closeSz - 4.0f * s;
                if (rightX < ImGui::GetCursorPosX()) rightX = ImGui::GetCursorPosX();
                ImGui::SetCursorPosX(rightX);
                ImVec2 closePos = ImGui::GetCursorScreenPos();
                closePos.y += (ImGui::GetFrameHeight() - closeSz) * 0.5f;
                char rbStr[24];
                SDL_snprintf(rbStr, sizeof(rbStr), "##rb%d", i);
                ImGuiID rbId = ImGui::GetID(rbStr);
                if (ImGui::CloseButton(rbId, closePos)) {
                    lobbySendRemoveBot(cs, transport, (uint8_t)i);
                    if (s_expandedBotSlot == i) s_expandedBotSlot = -1;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Remove bot");
                }
            }

            /* Expanded AiConfig sub-row for the clicked bot — drop
             * the row's 50px indent so the edit form spans the team
             * panel's full width.  Restored before the iteration
             * ends so subsequent sibling rows still align. */
            if (isBot && s_expandedBotSlot == i && effectiveHost) {
                ImGui::Unindent(50.0f * s);
                renderBotAiConfig(cs, transport, i, teamId, s);
                ImGui::Indent(50.0f * s);
            }

            /* Pair the per-row Indent above. */
            ImGui::Unindent(50.0f * s);
        }

        /* "+ Bot" button moved to the team header bar (right-aligned
         * next to the "Bot Naming:" pool dropdown) so all team-bot
         * controls live in one place. */

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

    /* No bottom team picker — Layout A relies on drag-to-assign + the
     * default-team logic on the server. Players who want to switch
     * teams can be dragged by the host (planned) or via context menu. */
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

    /* Edit form sits flush with the left edge of the team panel —
     * no internal indent. (The caller already strips the surrounding
     * 50px row indent so the form spans the full team width.) */
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

    if (diceClicked) {
        /* Pick a name from this team's pool, excluding all currently-used
         * bot names (incl. this one — we want a NEW name, not the same).
         * Reroll = pool-driven, so clear the manual-override flag. */
        int pool = (teamId > 0 && teamId < 16) ? cs->lobbyTeamPool[teamId] : 0;
        char pickBuf[32];
        lobbyBotPoolPick(pool, usedNames, usedCount, pickBuf, sizeof(pickBuf));
        lobbySendBotConfig(cs, transport, (uint8_t)slot,
            cs->lobbyBotDifficulty[slot], cs->lobbyBotPersonality[slot], pickBuf);
        if (slot < MAX_TANKS) s_botNameOverridden[slot] = false;
    }
    if (nameChanged) {
        /* Manual edit — pin the name so a later pool change doesn't
         * overwrite it. */
        lobbySendBotConfig(cs, transport, (uint8_t)slot,
            cs->lobbyBotDifficulty[slot], cs->lobbyBotPersonality[slot], nameBuf);
        if (slot < MAX_TANKS) s_botNameOverridden[slot] = true;
    }

    /* ── Difficulty dropdown ──────────────────────────────────── */
    ImGui::SameLine(0.0f, 16.0f * s);
    ImGui::TextDisabled("Difficulty");
    ImGui::SameLine();
    const char *diffItems[] = { "Easy", "Normal", "Hard" };
    int diff = cs->lobbyBotDifficulty[slot];
    if (diff < 0 || diff > 2) diff = 1;
    ImGui::SetNextItemWidth(90.0f * s);
    if (ImGui::Combo("##diff", &diff, diffItems, 3)) {
        lobbySendBotConfig(cs, transport, (uint8_t)slot,
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
    if (ImGui::Combo("##pers", &pers, persItems, 4)) {
        lobbySendBotConfig(cs, transport, (uint8_t)slot,
            cs->lobbyBotDifficulty[slot], (uint8_t)pers,
            cs->lobbySlots[slot].playerName);
    }

    /* ── Close button ─────────────────────────────────────────── */
    ImGui::SameLine(0.0f, 16.0f * s);
    if (ImGui::SmallButton("Done")) {
        s_expandedBotSlot = -1;
    }

    ImGui::PopID();
    ImGui::Spacing();
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
    if (cs->lobbyLastRejectPacket == 0) return;
    const char *reason = "rejected";
    switch (cs->lobbyLastRejectReason) {
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
        cs->lobbyLastRejectPacket = 0;
        cs->lobbyLastRejectReason = 0;
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
static void renderGameSettingsPanel(ClientSim *cs, Transport *transport,
                                    int myPlayerNum, float s) {
    bool effectiveHost = (myPlayerNum == 0) || cs->lobbyOpenHost;

    /* Drive the CollapsingHeader's open state explicitly so a "Hide
     * Settings" button at the bottom of the panel can fold it away
     * once the host is happy with the configuration. */
    static bool s_settingsOpen = true;
    ImGui::SetNextItemOpen(s_settingsOpen, ImGuiCond_Always);
    if (!ImGui::CollapsingHeader("Game settings")) {
        s_settingsOpen = false;
        return;
    }
    s_settingsOpen = true;

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
            /* gameType enum is 1-based (gameOpen=1, gameTournament=2,
             * gameStrictTournament=3), so the array index → enum
             * mapping is i+1. The previous (gameType)i comparison
             * read the wrong row as "checked" — Open showed as
             * Unknown, Tournament showed as Open, etc. */
            int enumVal = i + 1;
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##gt%d", items[i], i);
            bool checked = (cs->lobbyGameType == (gameType)enumVal);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)enumVal;
                lobbySendSetting(cs, transport, 1 /*LST_GAME_TYPE*/, &v, 1);
            }
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
            /* Same trick as the Game Type radios — embed the label so
             * the whole row is clickable. */
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##ai%d", items[i], i);
            bool checked = (cs->lobbyAiType == (uint8_t)i);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)i;
                lobbySendSetting(cs, transport, 3 /*LST_AI_POLICY*/, &v, 1);
            }
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
        if (ImGui::Checkbox("Allow Hidden Mines", &minesV)) {
            uint8_t v = minesV ? 1 : 0;
            lobbySendSetting(cs, transport, 2 /*LST_HIDDEN_MINES*/, &v, 1);
        }
        if (minesDisabled) ImGui::EndDisabled();
        if (minesLocked) renderLockBadge();

        bool timeLocked = (cs->lobbyServerLocks & 0x08) != 0;
        bool timeV = cs->lobbyTimeLimit > 0;
        bool timeDisabled = !effectiveHost || timeLocked;
        if (timeDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Game time limit", &timeV)) {
            uint8_t v = timeV ? 1 : 0;
            lobbySendSetting(cs, transport, 4 /*LST_TIME_LIMIT*/, &v, 1);
        }
        if (timeV) {
            int mins = cs->lobbyTimeLimit > 0
                ? (int)(cs->lobbyTimeLimit / (50 * 60))
                : 30;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f * s);
            if (ImGui::InputInt("##tmin", &mins, 1, 5,
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (mins < 1) mins = 1;
                if (mins > 999) mins = 999;
                uint8_t v[2] = { (uint8_t)((mins >> 8) & 0xFF),
                                 (uint8_t)(mins & 0xFF) };
                lobbySendSetting(cs, transport, 5 /*LST_TIME_MINUTES*/, v, 2);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("min");
        }
        if (timeDisabled) ImGui::EndDisabled();
        if (timeLocked) renderLockBadge();

        /* Multiplayer-only join controls. In single-player there's no
         * UDP listener, so "allow new players" / "disallow once started"
         * have no meaning — hide them rather than render disabled. */
        if (!cs->isSinglePlayer) {
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
            if (ImGui::Checkbox("Disallow new players once game has started", &autoLockV)) {
                uint8_t v = autoLockV ? 1 : 0;
                lobbySendSetting(cs, transport, 6 /*LST_AUTO_LOCK_ON_GAME*/, &v, 1);
            }
            if (autoLockDisabled) ImGui::EndDisabled();
            if (autoLockLocked) renderLockBadge();
        }

        /* openHost toggle moved to the team-list header (next to
         * "+ Add Team") so it lives where its scope is — managing
         * teams + bots. See renderTeamGroupedPlayers. */
    }

    ImGui::Columns(1);

    /* Right-aligned "Hide Settings" button. Positioned by directly
     * adjusting the cursor Y up by ~30px so the panel's overall
     * bottom edge sits 30 pixels higher than it would with default
     * ImGui::Spacing() padding — claws back vertical space for the
     * teams list below. */
    {
        const char *label = "Hide Settings";
        float btnW = ImGui::CalcTextSize(label).x + 16.0f * s;
        float y = ImGui::GetCursorPosY() - 30.0f * s;
        ImGui::SetCursorPosY(y);
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW);
        if (ImGui::Button(label)) {
            s_settingsOpen = false;
        }
    }
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

            /* Layout A — connectivity badge in the top-right corner.
             * Push the cursor to the right edge minus an estimated
             * badge width (icon + status text + Test button + status
             * text). 320px scaled is wide enough for the longest
             * combination ("Server unreachable [Test connectivity]
             * No reply yet"). The badge gracefully no-ops when port-
             * mapping is disabled (clients not hosting). */
            {
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
         * when cs->lobbyLastRejectPacket != 0. */
        renderLobbyRejectToast(cs, s);

        /* Layout A — collapsible game settings panel (radios, checkboxes,
         * lock badges). Edits dispatch via PACKET_LOBBY_SET_SETTING. */
        renderGameSettingsPanel(cs, transport, myPlayerNum, s);
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
                                        lobbySendTeamSet(cs, transport, (uint8_t)teamIdx);
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
                                        lobbySendRemoveBot(cs, transport, (uint8_t)i);
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
                                        lobbySendAddBot(cs, transport, -1, 0);
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
                    lobbySendReadyToggle(cs, transport, !myReady);
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
                                lobbySendTeamSet(cs, transport, (uint8_t)teamIdx);
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
                                lobbySendRemoveBot(cs, transport, (uint8_t)i);
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
                                lobbySendAddBot(cs, transport, -1, 0);
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

        /* --- Chat section --- */
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_CHAT));
        {
            float chatHeight = ImGui::GetTextLineHeightWithSpacing() * 3.4f;
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
                lobbySendReadyToggle(cs, transport, !myReady);
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
