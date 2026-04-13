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

extern "C" {
#include "../sdl3draw.h"
#include "../../gamefront.h"
#include "../../../bolo/global.h"
#include "../../../bolo/client_sim.h"
#include "../../../bolo/transport.h"
#include "../../../bolo/transport_udp.h"
#include "../../../bolo/bolo_map.h"
#include "../../../bolo/pillbox.h"
#include "../../../bolo/bases.h"
#include "../../../bolo/starts.h"
#include "../../../bolo/platform_net.h"
#include "../../../bolo/screencalc.h"
#include "../../../bolo/tilenum.h"
#include "../../tiles.h"
#include "../flags.h"
#include "../sdl3imgui.h"
#include "../minimap_render.h"
#include "imgui_lobby.h"

/* From mapview.h — declared directly to avoid pulling in game_sim.h */
extern int mapViewPosX[256];
extern int mapViewPosY[256];

/* From tileloader.h */
extern SDL_Surface *tileLoaderBuildSheet(int tileSize);
}

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

/* Popup state */
static bool  mapPopupOpen    = false;
static int   popupZoomIndex  = 5;        // index into zoomSteps[] (1.0x default)
static float popupZoomLevel  = 1.0f;     // actual zoom multiplier
static WORLD popupCenterX    = 128 << 8; // camera center in WORLD coords (map centre)
static WORLD popupCenterY    = 128 << 8;
static bool  popupDragging   = false;
static float popupDragLastX  = 0;
static float popupDragLastY  = 0;

/* Compressed map data — stashed when map download completes so the popup
 * can decompress on demand (transportUdpClientGetMapData() is only called
 * in the one-shot preview-build block; the pointer may not remain valid). */
static BYTE        *popupCompressedData = NULL;
static int          popupCompressedLen  = 0;

/* Tile rendering resources (created once when popup opens) */
static SDL_Texture *popupTilesTex    = NULL;  // tile atlas
static SDL_Texture *popupOffscreen   = NULL;  // offscreen render target
static int          popupOffscreenW  = 0;
static int          popupOffscreenH  = 0;
static map          popupMap    = NULL;
static pillboxes    popupPills  = NULL;
static bases        popupBases  = NULL;
static starts       popupStarts = NULL;
static bool         popupDataLoaded  = false;

/* Zoom steps — same as map editor */
static const float zoomSteps[] = {
    0.5f, 0.6f, 0.7f, 0.8f, 0.9f,
    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
    9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f
};
#define POPUP_ZOOM_STEP_COUNT 21
#define POPUP_ZOOM_STEP_1X 5

/* Boat sprite atlas coordinates for start position overlays */
static const int popupBoatAtlasX[16] = {
    TANK_SELFBOAT_0_X,  TANK_SELFBOAT_1_X,  TANK_SELFBOAT_2_X,  TANK_SELFBOAT_3_X,
    TANK_SELFBOAT_4_X,  TANK_SELFBOAT_5_X,  TANK_SELFBOAT_6_X,  TANK_SELFBOAT_7_X,
    TANK_SELFBOAT_8_X,  TANK_SELFBOAT_9_X,  TANK_SELFBOAT_10_X, TANK_SELFBOAT_11_X,
    TANK_SELFBOAT_12_X, TANK_SELFBOAT_13_X, TANK_SELFBOAT_14_X, TANK_SELFBOAT_15_X
};
static const int popupBoatAtlasY[16] = {
    TANK_SELFBOAT_0_Y,  TANK_SELFBOAT_1_Y,  TANK_SELFBOAT_2_Y,  TANK_SELFBOAT_3_Y,
    TANK_SELFBOAT_4_Y,  TANK_SELFBOAT_5_Y,  TANK_SELFBOAT_6_Y,  TANK_SELFBOAT_7_Y,
    TANK_SELFBOAT_8_Y,  TANK_SELFBOAT_9_Y,  TANK_SELFBOAT_10_Y, TANK_SELFBOAT_11_Y,
    TANK_SELFBOAT_12_Y, TANK_SELFBOAT_13_Y, TANK_SELFBOAT_14_Y, TANK_SELFBOAT_15_Y
};

/* Read a neighbour tile for adjacency calculation, treating bases as ROAD
 * and stripping mine variants. Adapted from meNeighbour() in mapeditor.c. */
static BYTE popupNeighbour(BYTE nx, BYTE ny) {
    if (basesExistPos(&popupBases, nx, ny)) return ROAD;
    BYTE t = popupMap->mapItem[nx][ny];
    if (t >= MINE_START && t <= MINE_END) return (BYTE)(t - MINE_SUBTRACT);
    return t;
}

/* Adjacency-aware tile calculation. Adapted from meCalcTile() in mapeditor.c. */
static BYTE popupCalcTile(BYTE xValue, BYTE yValue) {
    if (pillsExistPos(&popupPills, xValue, yValue)) {
        static const BYTE pillTileForArmour[16] = {
            PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
            PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
            PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
            PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
        };
        BYTE armour = pillsGetArmourPos(&popupPills, xValue, yValue);
        if (armour <= 15) return pillTileForArmour[armour];
        return PILL_EVIL_15;
    }
    if (basesExistPos(&popupBases, xValue, yValue)) return BASE_NEUTRAL;
    if (startsExistPos(&popupStarts, xValue, yValue)) return DEEP_SEA_SOLID;

    BYTE currentPos = popupMap->mapItem[xValue][yValue];
    if (currentPos >= MINE_START && currentPos <= MINE_END)
        currentPos = (BYTE)(currentPos - MINE_SUBTRACT);

    BYTE aboveLeft  = popupNeighbour((BYTE)(xValue-1), (BYTE)(yValue-1));
    BYTE above      = popupNeighbour(xValue,            (BYTE)(yValue-1));
    BYTE aboveRight = popupNeighbour((BYTE)(xValue+1), (BYTE)(yValue-1));
    BYTE leftPos    = popupNeighbour((BYTE)(xValue-1), yValue);
    BYTE rightPos   = popupNeighbour((BYTE)(xValue+1), yValue);
    BYTE belowLeft  = popupNeighbour((BYTE)(xValue-1), (BYTE)(yValue+1));
    BYTE below      = popupNeighbour(xValue,            (BYTE)(yValue+1));
    BYTE belowRight = popupNeighbour((BYTE)(xValue+1), (BYTE)(yValue+1));

    switch (currentPos) {
    case ROAD:     return screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case BUILDING: return screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case FOREST:   return screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case RIVER:    return screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case DEEP_SEA: return screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case BOAT:     return screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    case CRATER:   return screenCalcCrater(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
    default:       return currentPos;
    }
}

/* Render start positions as boat sprites. Adapted from meRenderStarts() in mapeditor.c. */
static void popupRenderStarts(SDL_Renderer *renderer, int screenW, int screenH) {
    int zf = (int)popupZoomLevel;
    if (zf < 1) zf = 1;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)popupCenterX * tileSize) >> 8;
    int centerPY = ((int)popupCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    SDL_SetTextureAlphaMod(popupTilesTex, 200);

    for (BYTE i = 0; i < popupStarts->numStarts; i++) {
        start *s = &popupStarts->item[i];
        float dx = (float)((int)s->x * tileSize - camPX) * zf;
        float dy = (float)((int)s->y * tileSize - camPY) * zf;

        if (dx + scaledTile < 0 || dx > screenW ||
            dy + scaledTile < 0 || dy > screenH) continue;

        int dir = startsConvertDir((s->dir < 16) ? s->dir : 0);
        SDL_FRect src = {
            (float)popupBoatAtlasX[dir],
            (float)popupBoatAtlasY[dir],
            (float)tileSize,
            (float)tileSize
        };
        SDL_FRect dest = { dx, dy, (float)scaledTile, (float)scaledTile };
        SDL_RenderTexture(renderer, popupTilesTex, &src, &dest);
    }

    SDL_SetTextureAlphaMod(popupTilesTex, 255);
}

/* Render tiles to the offscreen texture. Adapted from meRenderTiles() in mapeditor.c. */
static void popupRenderTilesToOffscreen(SDL_Renderer *renderer, int screenW, int screenH) {
    int zf = (int)popupZoomLevel;
    if (zf < 1) zf = 1;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)popupCenterX * tileSize) >> 8;
    int centerPY = ((int)popupCenterY * tileSize) >> 8;

    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    int camMX = camPX / tileSize;
    int camMY = camPY / tileSize;
    if (camPX < 0) camMX--;
    if (camPY < 0) camMY--;
    int edgeX = (camPX - camMX * tileSize) * zf;
    int edgeY = (camPY - camMY * tileSize) * zf;

    int tilesW = screenW / scaledTile + 3;
    int tilesH = screenH / scaledTile + 3;
    if (tilesW > 256) tilesW = 256;
    if (tilesH > 256) tilesH = 256;

    for (int x = 0; x < tilesW; x++) {
        for (int y = 0; y < tilesH; y++) {
            int mapX = camMX + x;
            int mapY = camMY + y;

            BYTE tileNum;
            if (mapX < 0 || mapX > 255 || mapY < 0 || mapY > 255) {
                tileNum = DEEP_SEA_SOLID;
            } else {
                tileNum = popupCalcTile((BYTE)mapX, (BYTE)mapY);
            }

            SDL_FRect src = {
                (float)(mapViewPosX[tileNum]),
                (float)(mapViewPosY[tileNum]),
                (float)tileSize,
                (float)tileSize
            };
            SDL_FRect dest = {
                (float)(x * scaledTile - edgeX),
                (float)(y * scaledTile - edgeY),
                (float)scaledTile,
                (float)scaledTile
            };
            SDL_RenderTexture(renderer, popupTilesTex, &src, &dest);
        }
    }

    popupRenderStarts(renderer, screenW, screenH);
}

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
        case gameOpen:             return "Open";
        case gameTournament:       return "Tournament";
        case gameStrictTournament: return "Strict Tournament";
        default:                   return "Unknown";
    }
}

static const char *aiTypeStr(uint8_t ai) {
    switch (ai) {
        case 0:  return "No";
        case 1:  return "Yes";
        case 2:  return "Yes (Advantage)";
        case 3:  return "Yes (Full)";
        default: return "Unknown";
    }
}

static void formatTimeLimit(int32_t ticks, char *buf, int bufSize) {
    if (ticks <= 0) {
        SDL_snprintf(buf, bufSize, "Unlimited");
        return;
    }
    int totalSecs = ticks / 50;
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;
    if (hours > 0) {
        SDL_snprintf(buf, bufSize, "%dh %02dm %02ds", hours, mins, secs);
    } else if (mins > 0) {
        SDL_snprintf(buf, bufSize, "%dm %02ds", mins, secs);
    } else {
        SDL_snprintf(buf, bufSize, "%ds", secs);
    }
}

extern "C" int imguiLobbyShow(ClientSim *cs) {
    SDL_Log("[LOBBY] imguiLobbyShow called cs=%p inLobby=%d netStat=%d",
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
    dialogSetWindowTitle(window, "WinBolo - Game Lobby");
    SDL_SetWindowResizable(window, true);
#endif
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

    /* Chat state */
    char chatInput[CHAT_INPUT_SIZE];
    chatInput[0] = '\0';

    /* Team combo items */
    static const char *teamItems[] = {
        "None", "1", "2", "3", "4", "5", "6", "7", "8",
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
            if (popupDataLoaded) {
                if (popupOffscreen) { SDL_DestroyTexture(popupOffscreen); popupOffscreen = NULL; }
                popupOffscreenW = 0; popupOffscreenH = 0;
                mapDestroy(&popupMap); pillsDestroy(&popupPills);
                basesDestroy(&popupBases); startsDestroy(&popupStarts);
                popupDataLoaded = false;
            }
            mapPopupOpen = false;
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
        if (mapPopupOpen && popupDataLoaded && popupTilesTex) {
            int viewW = (int)(winW * 0.8f);
            int viewH = (int)(winH * 0.8f);
            if (viewW < 1) viewW = 1;
            if (viewH < 1) viewH = 1;
            /* Sub-1x zoom: render at 1x into larger texture, ImGui scales down */
            int ofsW = viewW, ofsH = viewH;
            if (popupZoomLevel < 1.0f) {
                ofsW = (int)(viewW / popupZoomLevel);
                ofsH = (int)(viewH / popupZoomLevel);
                if (ofsW > 4096) ofsW = 4096;
                if (ofsH > 4096) ofsH = 4096;
            }
            if (!popupOffscreen || popupOffscreenW != ofsW || popupOffscreenH != ofsH) {
                if (popupOffscreen) SDL_DestroyTexture(popupOffscreen);
                popupOffscreen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                                   SDL_TEXTUREACCESS_TARGET, ofsW, ofsH);
                popupOffscreenW = ofsW;
                popupOffscreenH = ofsH;
            }
            if (popupOffscreen) {
                SDL_SetRenderTarget(renderer, popupOffscreen);
                SDL_SetRenderDrawColor(renderer, 0, 0, 64, 255);
                SDL_RenderClear(renderer);
                popupRenderTilesToOffscreen(renderer, ofsW, ofsH);
                SDL_SetRenderTarget(renderer, NULL);
            }
        }

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
            ImGui::TextWrapped("%s:%u | %s | %s Mines | AI: %s | %s",
                         inet_ntoa(cs->serverAddress), cs->serverPort,
                         gameTypeStr(cs->lobbyGameType),
                         cs->lobbyHiddenMines ? "Hidden" : "Visible",
                         aiTypeStr(cs->lobbyAiType), timeStr);
#else
            ImGui::Text("Server: %s", serverStr);
            ImGui::SameLine(0, 16);
            ImGui::Text("Game: %s", gameTypeStr(cs->lobbyGameType));
            ImGui::SameLine(0, 16);
            ImGui::Text("Mines: %s", cs->lobbyHiddenMines ? "Hidden" : "Visible");
            ImGui::SameLine(0, 16);
            ImGui::Text("AI: %s", aiTypeStr(cs->lobbyAiType));
            ImGui::SameLine(0, 16);
            ImGui::Text("Time: %s", timeStr);
#endif
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

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
                if (ImGui::BeginTabItem("Players")) {
                    activeTab = 0;
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;
                    ImGui::BeginChild("##PlayerPanel", ImVec2(availW, tabH), ImGuiChildFlags_None);

                    if (ImGui::BeginTable("##PlayerTable", 5,
                                          ImGuiTableFlags_Borders |
                                          ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_ScrollY)) {
                        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 45.0f * s);
                        ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
                        ImGui::TableSetupColumn("Ready", ImGuiTableColumnFlags_WidthFixed, 45.0f * s);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 65.0f * s);
                        ImGui::TableHeadersRow();

                        bool botsAllowed = (cs->lobbyAiType != 0);

                        SDL_Log("[LOBBY DBG] rendering player table");
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
                                if (cs->lobbySlots[i].wbnParticipant) {
                                    SDL_Texture *globeTex = sdl3ImguiGetGlobeIcon();
                                    if (globeTex) {
                                        ImGui::Image((ImTextureID)globeTex, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
                                        ImGui::SameLine();
                                    }
                                }
                                if (cs->lobbySlots[i].steamParticipant) {
                                    SDL_Texture *steamTex = sdl3ImguiGetSteamIcon();
                                    if (steamTex) {
                                        ImGui::Image((ImTextureID)steamTex, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
                                        ImGui::SameLine();
                                    }
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
                                        ImGui::TextDisabled("None");
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
                                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Yes");
                                } else {
                                    ImGui::TextDisabled("No");
                                }

                                /* Action */
                                ImGui::TableSetColumnIndex(4);
                                if (cs->lobbySlots[i].isBot && transport) {
                                    char btnId[16];
                                    SDL_snprintf(btnId, sizeof(btnId), "Remove##%d", i);
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
                    ImGui::EndTabItem();
                }

                /* --- Map tab --- */
                if (ImGui::BeginTabItem("Map")) {
                    activeTab = 1;
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;

                    if (!cs->mapDownloadComplete) {
                        ImGui::Text("Downloading map...");
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
                        if (ImGui::IsItemClicked() && popupCompressedData) {
                            mapPopupOpen = true;
                            popupZoomIndex = POPUP_ZOOM_STEP_1X + 1;
                            popupZoomLevel = 2.0f;
                            popupCenterX = ((mapBounds.minX + mapBounds.maxX) / 2) << 8;
                            popupCenterY = ((mapBounds.minY + mapBounds.maxY) / 2) << 8;
                            popupDragging = false;
                        }
                    } else {
                        ImGui::Text("Map preview unavailable");
                    }
                    ImGui::Spacing();
                    ImGui::Text("%s - %dP %dB %dS", cs->mapName, cs->lobbyPillCount, cs->lobbyBaseCount, cs->lobbyStartCount);

                    if (cs->mapSkipAvailable && cs->inLobby) {
                        ImGui::Spacing();
                        bool countdownActive = cs->countdownSeconds > 0;
                        if (countdownActive) ImGui::BeginDisabled();
                        bool voted = cs->mapSkipMyVote;
                        const char *skipLabel = voted ? "Cancel Skip" : "Skip Map";
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
                        ImGui::Text("%d/%d votes to skip", skipCount, humanCount);
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
                    if (ImGui::BeginTabItem("Chat")) {
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
                            bool sendClicked = ImGui::Button("Send", ImVec2(btnW, 0));
                            if (chatEmpty) ImGui::EndDisabled();
                            if ((sendClicked || enterPressed) &&
                                !chatEmpty && transport) {
                                transportUdpClientSendChat(transport, 0xFF, chatInput);
                                const char *myName = (myPlayerNum < MAX_TANKS && cs->lobbySlots[myPlayerNum].connected)
                                    ? cs->lobbySlots[myPlayerNum].playerName : "Me";
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
                const char *readyLabel = myReady ? "Unready" : "Ready";
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
                            if (cs->lobbySlots[j].wbnParticipant) {
                                hasWbnPlayers = true;
                            }
                        }
                    }
                    if (hasWbnPlayers) {
                        ImGui::SameLine(0, 20);
                        if (connectedCount < 2) ImGui::BeginDisabled();
                        if (ImGui::Button("Balance Teams", ImVec2(120 * s, 0))) {
                            uint8_t teamSize = (connectedCount > 1) ? (connectedCount / 2) : 1;
                            transportUdpClientSendBalanceRequest(transport, teamSize);
                        }
                        if (connectedCount < 2) ImGui::EndDisabled();
                    }
                } else if (myPlayerNum == 0 && transport && cs->balanceProposalActive) {
                    ImGui::SameLine(0, 20);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                    if (ImGui::Button("Apply Balance", ImVec2(120 * s, 0))) {
                        transportUdpClientSendBalanceApply(transport);
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 8);
                    if (ImGui::Button("Dismiss", ImVec2(80 * s, 0))) {
                        transportUdpClientSendBalanceDismiss(transport);
                    }
                }

                ImGui::SameLine(0, 20);
                if (ImGui::Button("Leave", ImVec2(100 * s, 0)) ||
                    (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                     !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                    ImGui::OpenPopup("Leave Game?##lobby");
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

            /* Left: Player table */
            ImGui::BeginChild("##PlayerPanel", ImVec2(playerPanelW, panelH), ImGuiChildFlags_None);

            if (ImGui::BeginTable("##PlayerTable", 6,
                                  ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 30.0f * s);
                ImGui::TableSetupColumn("Player Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 45.0f * s);
                ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
                ImGui::TableSetupColumn("Ready", ImGuiTableColumnFlags_WidthFixed, 40.0f * s);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
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
                        if (cs->lobbySlots[i].wbnParticipant) {
                            SDL_Texture *globeTex = sdl3ImguiGetGlobeIcon();
                            if (globeTex) {
                                ImGui::Image((ImTextureID)globeTex, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
                                ImGui::SameLine();
                            }
                        }
                        if (cs->lobbySlots[i].steamParticipant) {
                            SDL_Texture *steamTex = sdl3ImguiGetSteamIcon();
                            if (steamTex) {
                                ImGui::Image((ImTextureID)steamTex, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
                                ImGui::SameLine();
                            }
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
                                ImGui::TextDisabled("None");
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
                            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Yes");
                        } else {
                            ImGui::TextDisabled("No");
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
                ImGui::Text("Downloading map...");
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
                if (ImGui::IsItemClicked() && popupCompressedData) {
                    mapPopupOpen = true;
                    popupZoomIndex = POPUP_ZOOM_STEP_1X + 1;
                    popupZoomLevel = 2.0f;
                    popupCenterX = ((mapBounds.minX + mapBounds.maxX) / 2) << 8;
                    popupCenterY = ((mapBounds.minY + mapBounds.maxY) / 2) << 8;
                    popupDragging = false;
                }
            } else {
                ImGui::Text("Map preview unavailable");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* Map info */
            ImGui::Text("Map: %s", cs->mapName);
            ImGui::Text("Pillboxes: %d", cs->lobbyPillCount);
            ImGui::Text("Bases: %d", cs->lobbyBaseCount);
            ImGui::Text("Starts: %d", cs->lobbyStartCount);

            if (cs->mapSkipAvailable && cs->inLobby) {
                ImGui::Spacing();
                bool countdownActive = cs->countdownSeconds > 0;
                if (countdownActive) ImGui::BeginDisabled();
                bool voted = cs->mapSkipMyVote;
                const char *skipLabel = voted ? "Cancel Skip" : "Skip Map";
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
                ImGui::Text("%d/%d votes to skip", skipCount, humanCount);
                if (countdownActive) ImGui::EndDisabled();
            }

            ImGui::EndChild(); /* ##MapPanel */
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Chat section --- */
        ImGui::Text("Chat");
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
            bool sendClicked = ImGui::Button("Send", ImVec2(btnW, 0));
            if (chatEmpty) ImGui::EndDisabled();
            if ((sendClicked || enterPressed) &&
                !chatEmpty && transport) {
                transportUdpClientSendChat(transport, 0xFF, chatInput);
                const char *myName = (myPlayerNum < MAX_TANKS && cs->lobbySlots[myPlayerNum].connected)
                    ? cs->lobbySlots[myPlayerNum].playerName : "Me";
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
            const char *readyLabel = myReady ? "Unready" : "Ready";
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
                        if (cs->lobbySlots[j].wbnParticipant) {
                            hasWbnPlayers = true;
                        }
                    }
                }
                if (hasWbnPlayers) {
                    ImGui::SameLine(0, 20);
                    if (connectedCount < 2) ImGui::BeginDisabled();
                    if (ImGui::Button("Balance Teams", ImVec2(120 * s, 0))) {
                        uint8_t teamSize = (connectedCount > 1) ? (connectedCount / 2) : 1;
                        transportUdpClientSendBalanceRequest(transport, teamSize);
                    }
                    if (connectedCount < 2) ImGui::EndDisabled();
                }
            } else if (myPlayerNum == 0 && transport && cs->balanceProposalActive) {
                ImGui::SameLine(0, 20);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                if (ImGui::Button("Apply Balance", ImVec2(120 * s, 0))) {
                    transportUdpClientSendBalanceApply(transport);
                }
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 8);
                if (ImGui::Button("Dismiss", ImVec2(80 * s, 0))) {
                    transportUdpClientSendBalanceDismiss(transport);
                }
            }

            ImGui::SameLine(0, 20);
            if (ImGui::Button("Leave", ImVec2(100 * s, 0)) ||
                (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                 !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                ImGui::OpenPopup("Leave Game?##lobby");
            }
        }
#endif

        /* --- Map preview popup --- */
        if (mapPopupOpen && !ImGui::IsPopupOpen("Map Preview##full")) {
            ImGui::OpenPopup("Map Preview##full");
        }
        {
            ImVec2 displaySize = ImGui::GetIO().DisplaySize;
            ImVec2 popupSize(displaySize.x * 0.8f, displaySize.y * 0.8f);
            ImGui::SetNextWindowSize(popupSize, ImGuiCond_Appearing);
            ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                                    ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        }
        if (ImGui::BeginPopupModal("Map Preview##full", &mapPopupOpen,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove)) {
            /* Load tile resources on first open */
            if (!popupDataLoaded && popupCompressedData) {
                if (!popupTilesTex) {
                    SDL_Surface *sheet = tileLoaderBuildSheet(16);
                    if (sheet) {
                        popupTilesTex = SDL_CreateTextureFromSurface(renderer, sheet);
                        SDL_SetTextureScaleMode(popupTilesTex, SDL_SCALEMODE_NEAREST);
                        SDL_DestroySurface(sheet);
                    }
                }
                mapCreate(&popupMap);
                pillsCreate(&popupPills);
                basesCreate(&popupBases);
                startsCreate(&popupStarts);
                mapLoadCompressedMap(&popupMap, &popupPills, &popupBases, &popupStarts,
                                     popupCompressedData, popupCompressedLen);
                popupDataLoaded = true;
            }

            if (popupDataLoaded && popupOffscreen) {
                ImVec2 contentSize = ImGui::GetContentRegionAvail();
                ImGui::Image((ImTextureID)popupOffscreen, contentSize);

                /* Cursor and drag-to-pan */
                ImGuiIO &io = ImGui::GetIO();
                if (ImGui::IsItemHovered()) {
                    if (ImGui::IsMouseDragging(0)) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                        float zf = popupZoomLevel;
                        float tileSize = 16.0f;
                        float dx = io.MouseDelta.x / (zf * tileSize) * 256.0f;
                        float dy = io.MouseDelta.y / (zf * tileSize) * 256.0f;
                        int newCX = (int)popupCenterX - (int)dx;
                        int newCY = (int)popupCenterY - (int)dy;
                        if (newCX < 0) newCX = 0;
                        if (newCX > 65280) newCX = 65280;
                        if (newCY < 0) newCY = 0;
                        if (newCY > 65280) newCY = 65280;
                        popupCenterX = (WORLD)newCX;
                        popupCenterY = (WORLD)newCY;
                    } else {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                }

                /* Zoom via scroll wheel / pinch */
                if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
                    if (io.MouseWheel > 0 && popupZoomIndex < POPUP_ZOOM_STEP_COUNT - 1)
                        popupZoomIndex++;
                    else if (io.MouseWheel < 0 && popupZoomIndex > 0)
                        popupZoomIndex--;
                    popupZoomLevel = zoomSteps[popupZoomIndex];
                }

                /* Arrow key panning */
                {
                    float panSpeed = 512.0f / popupZoomLevel; /* WORLD units per frame */
                    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) {
                        int newCX = (int)popupCenterX - (int)panSpeed;
                        popupCenterX = (WORLD)(newCX < 0 ? 0 : newCX);
                    }
                    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
                        int newCX = (int)popupCenterX + (int)panSpeed;
                        popupCenterX = (WORLD)(newCX > 65280 ? 65280 : newCX);
                    }
                    if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) {
                        int newCY = (int)popupCenterY - (int)panSpeed;
                        popupCenterY = (WORLD)(newCY < 0 ? 0 : newCY);
                    }
                    if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) {
                        int newCY = (int)popupCenterY + (int)panSpeed;
                        popupCenterY = (WORLD)(newCY > 65280 ? 65280 : newCY);
                    }
                }

                /* Zoom indicator overlay */
                {
                    char zoomText[16];
                    SDL_snprintf(zoomText, sizeof(zoomText), "%.1fx", popupZoomLevel);
                    ImVec2 textSize = ImGui::CalcTextSize(zoomText);
                    ImVec2 windowPos = ImGui::GetWindowPos();
                    ImVec2 windowSize = ImGui::GetWindowSize();
                    float pad = 8.0f;
                    ImVec2 textPos(windowPos.x + windowSize.x - textSize.x - pad - ImGui::GetStyle().WindowPadding.x,
                                  windowPos.y + windowSize.y - textSize.y - pad - ImGui::GetStyle().WindowPadding.y);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    ImVec2 bgMin(textPos.x - 4, textPos.y - 2);
                    ImVec2 bgMax(textPos.x + textSize.x + 4, textPos.y + textSize.y + 2);
                    dl->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 160), 4.0f);
                    dl->AddText(textPos, IM_COL32(255, 255, 255, 220), zoomText);
                }
            } else {
                ImGui::Text("Map preview loading...");
            }
            ImGui::EndPopup();
        }
        if (!mapPopupOpen && popupDataLoaded) {
            /* Cleanup on popup close */
            if (popupOffscreen) { SDL_DestroyTexture(popupOffscreen); popupOffscreen = NULL; }
            popupOffscreenW = 0;
            popupOffscreenH = 0;
            mapDestroy(&popupMap);
            pillsDestroy(&popupPills);
            basesDestroy(&popupBases);
            startsDestroy(&popupStarts);
            popupDataLoaded = false;
        }

        /* --- Leave confirmation popup --- */
        if (ImGui::BeginPopupModal("Leave Game?##lobby", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Are you sure you want to leave this game?");
            ImGui::Spacing();
            if (ImGui::Button("Yes", ImVec2(80 * s, 0))) {
                ImGui::CloseCurrentPopup();
                result = 0;
                running = false;
            }
            ImGui::SameLine(0.0f, 8.0f);
            if (ImGui::Button("No", ImVec2(80 * s, 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        /* --- Countdown overlay --- */
        if (cs->netStat == netLobbyCountdown && cs->countdownSeconds > 0) {
            char countdownText[32];
            SDL_snprintf(countdownText, sizeof(countdownText),
                         "Starting in %d...", cs->countdownSeconds);
            ImVec2 textSize = ImGui::CalcTextSize(countdownText);
            ImVec2 winSize = ImGui::GetWindowSize();
            ImGui::SetCursorPos(ImVec2(
                (winSize.x - textSize.x) * 0.5f,
                (winSize.y - textSize.y) * 0.5f
            ));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
            float origScale = ImGui::GetFont()->Scale;
            ImGui::GetFont()->Scale = 3.0f;
            ImGui::PushFont(ImGui::GetFont());
            textSize = ImGui::CalcTextSize(countdownText);
            ImGui::SetCursorPos(ImVec2(
                (winSize.x - textSize.x) * 0.5f,
                (winSize.y - textSize.y) * 0.5f
            ));
            ImGui::Text("%s", countdownText);
            ImGui::GetFont()->Scale = origScale;
            ImGui::PopFont();
            ImGui::PopStyleColor();
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
    if (popupDataLoaded) {
        mapDestroy(&popupMap); pillsDestroy(&popupPills);
        basesDestroy(&popupBases); startsDestroy(&popupStarts);
        popupDataLoaded = false;
    }
    if (popupOffscreen) { SDL_DestroyTexture(popupOffscreen); popupOffscreen = NULL; }
    popupOffscreenW = 0; popupOffscreenH = 0;
    if (popupTilesTex) { SDL_DestroyTexture(popupTilesTex); popupTilesTex = NULL; }
    mapPopupOpen = false;

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
