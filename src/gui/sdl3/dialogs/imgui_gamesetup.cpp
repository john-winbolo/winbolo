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
 * Name:          imgui_gamesetup.cpp
 * Purpose:       ImGui Game Setup dialog.
 *                Two-phase flow:
 *                1. Map chooser (full screen)
 *                2. Merged setup: map preview + stats on
 *                   left, game options + bot table on right.
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <SDL3/SDL.h>
#include <SDL3/SDL_filesystem.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"

extern "C" {
#include "../sdl3draw.h"
#include "../bg_game.h"
#include "../../gamefront.h"
#include "../../../bolo/global.h"
#include "../../../bolo/screen.h"
#include "../../../bolo/client_sim.h"
#include "imgui_mapchooser.h"
#include "imgui_gamesetup.h"
}

#define NUM_SECONDS 60
#define MAX_BRAINS 32
#define MAX_SETUP_BOTS 15
#define PREVIEW_SIZE 256

static const int DIALOG_W = 800;
static const int DIALOG_H = 600;

/* Discovered brain scripts */
struct BrainEntry {
    char name[64];     /* Display name (directory name) */
    char path[FILENAME_MAX]; /* Full path to init.lua */
};

static int discoverBrains(BrainEntry *brains, int maxBrains) {
    int count = 0;
    const char *dirs[] = { "brains", "Brains", "data/Brains" };
    int numDirs = 3;

    /* Also search relative to the executable base path (needed on iOS where
       CWD may not match the app bundle location) */
    char baseDirs[3][FILENAME_MAX];
    const char *basePath = SDL_GetBasePath();
    if (basePath && basePath[0]) {
        for (int i = 0; i < numDirs; i++) {
            SDL_snprintf(baseDirs[i], sizeof(baseDirs[i]), "%s%s", basePath, dirs[i]);
        }
    }

    /* Search relative dirs first, then basePath-prefixed dirs */
    for (int d = 0; d < numDirs + (basePath ? numDirs : 0) && count < maxBrains; d++) {
        const char *dir = (d < numDirs) ? dirs[d] : baseDirs[d - numDirs];
        int numEntries = 0;
        char **entries = SDL_GlobDirectory(dir, "*", SDL_GLOB_CASEINSENSITIVE, &numEntries);
        if (!entries) continue;
        for (int i = 0; i < numEntries && count < maxBrains; i++) {
            char initPath[FILENAME_MAX];
            SDL_snprintf(initPath, sizeof(initPath), "%s/%s/init.lua", dir, entries[i]);
            SDL_IOStream *f = SDL_IOFromFile(initPath, "r");
            if (f) {
                SDL_CloseIO(f);
                bool duplicate = false;
                for (int j = 0; j < count; j++) {
                    if (strcmp(brains[j].name, entries[i]) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    SDL_strlcpy(brains[count].name, entries[i], sizeof(brains[count].name));
                    SDL_strlcpy(brains[count].path, initPath, sizeof(brains[count].path));
                    count++;
                }
            }
        }
        SDL_free(entries);
    }
    return count;
}

/* Per-bot setup slot */
struct BotSetupSlot {
    int brainIdx;    /* Index into brains[] array */
    int teamIdx;     /* 0 = None, 1-16 */
};

enum SetupPhase { PHASE_MAP_CHOOSER, PHASE_GAME_SETUP };

/* Compute UV coordinates to zoom into interesting area of a map preview */
static void computePreviewUVs(int bMinX, int bMinY, int bMaxX, int bMaxY,
                                ImVec2 *uv0, ImVec2 *uv1) {
    int pad = 4;
    int bx0 = bMinX - pad; if (bx0 < 0) bx0 = 0;
    int by0 = bMinY - pad; if (by0 < 0) by0 = 0;
    int bx1 = bMaxX + pad; if (bx1 >= PREVIEW_SIZE) bx1 = PREVIEW_SIZE - 1;
    int by1 = bMaxY + pad; if (by1 >= PREVIEW_SIZE) by1 = PREVIEW_SIZE - 1;
    /* Make region square */
    int bw = bx1 - bx0;
    int bh = by1 - by0;
    if (bw > bh) {
        int diff = bw - bh;
        by0 -= diff / 2;
        by1 += (diff + 1) / 2;
        if (by0 < 0) { by1 -= by0; by0 = 0; }
        if (by1 >= PREVIEW_SIZE) { by0 -= (by1 - PREVIEW_SIZE + 1); by1 = PREVIEW_SIZE - 1; }
        if (by0 < 0) by0 = 0;
    } else if (bh > bw) {
        int diff = bh - bw;
        bx0 -= diff / 2;
        bx1 += (diff + 1) / 2;
        if (bx0 < 0) { bx1 -= bx0; bx0 = 0; }
        if (bx1 >= PREVIEW_SIZE) { bx0 -= (bx1 - PREVIEW_SIZE + 1); bx1 = PREVIEW_SIZE - 1; }
        if (bx0 < 0) bx0 = 0;
    }
    *uv0 = ImVec2((float)bx0 / PREVIEW_SIZE, (float)by0 / PREVIEW_SIZE);
    *uv1 = ImVec2((float)(bx1 + 1) / PREVIEW_SIZE, (float)(by1 + 1) / PREVIEW_SIZE);
}

extern "C" int imguiGameSetupShow(ClientSim *cs) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return 0;

    /* Save logical presentation (Android sets one for the game view) */
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
    dialogSetWindowTitle(window, "Game Setup");
    SDL_SetWindowResizable(window, true);
#endif
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    /* Set up ImGui context for this dialog */
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

    /* Background game */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);
    Uint64 lastTickTime = SDL_GetTicks();

    /* Load current game options */
    char password[MAP_STR_SIZE];
    gameType gt;
    bool hm;
    aiType ai;
    int32_t sd;
    int32_t tlimit;

    password[0] = '\0';
    gameFrontGetGameOptions(password, &gt, &hm, &ai, &sd, &tlimit);

    int gameTypeIdx = (gt == gameOpen) ? 0 : (gt == gameTournament) ? 1 : 2;
    bool hiddenMines = hm;
    int aiIdx = (ai == aiNone) ? 0 : (ai == aiYes) ? 1 : (ai == aiYesAdvantage) ? 2 : 3;

    bool useTimeLimit = (tlimit != UNLIMITED_GAME_TIME);
    int timeLimitMin = 0;
    if (tlimit != UNLIMITED_GAME_TIME) {
        timeLimitMin = tlimit / (1000 / 20) / NUM_SECONDS;
    }

    /* Discover available brains */
    BrainEntry brains[MAX_BRAINS];
    int numBrains = discoverBrains(brains, MAX_BRAINS);

    /* Bot setup slots */
    BotSetupSlot botSlots[MAX_SETUP_BOTS];
    memset(botSlots, 0, sizeof(botSlots));
    int numBots = 0;
    int playerTeamIdx = 0; /* Human player team (0 = None, 1-16) */

    /* Load existing bot setup */
    {
        GameFrontBotSetup existingSetup;
        gameFrontGetBotSetup(&existingSetup);
        playerTeamIdx = existingSetup.playerTeamNumber;
        if (existingSetup.count > 0) {
            numBots = existingSetup.count;
            if (numBots > MAX_SETUP_BOTS) numBots = MAX_SETUP_BOTS;
            for (int i = 0; i < numBots; i++) {
                botSlots[i].teamIdx = existingSetup.bots[i].teamNumber;
                /* Find brain index */
                botSlots[i].brainIdx = 0;
                for (int j = 0; j < numBrains; j++) {
                    if (strcmp(brains[j].path, existingSetup.bots[i].brainPath) == 0) {
                        botSlots[i].brainIdx = j;
                        break;
                    }
                }
            }
        } else {
            /* Fall back to legacy bot options */
            int botCount = 0;
            char botBrainPath[FILENAME_MAX];
            botBrainPath[0] = '\0';
            gameFrontGetBotOptions(&botCount, botBrainPath, sizeof(botBrainPath));
            numBots = botCount;
            if (numBots > MAX_SETUP_BOTS) numBots = MAX_SETUP_BOTS;
            int defaultBrainIdx = 0;
            for (int i = 0; i < numBrains; i++) {
                if (strcmp(brains[i].path, botBrainPath) == 0) {
                    defaultBrainIdx = i;
                    break;
                }
            }
            for (int i = 0; i < numBots; i++) {
                botSlots[i].brainIdx = defaultBrainIdx;
                botSlots[i].teamIdx = 0;
            }
        }
    }

    /* Initialize map chooser */
    MapChooserState mapChooser;
    mapChooserInit(&mapChooser, renderer);

    /* Check command line argument for map */
    char cmdArgFile[FILENAME_MAX];
    cmdArgFile[0] = '\0';
    gameFrontGetCmdArg(cmdArgFile);
    if (cmdArgFile[0] != '\0') {
        mapChooser.selectedIdx = -1;
        SDL_strlcpy(mapChooser.selectedPath, cmdArgFile, FILENAME_MAX);
        const char *base = cmdArgFile;
        for (const char *p = cmdArgFile; *p; p++) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
        SDL_strlcpy(mapChooser.selectedName, base, sizeof(mapChooser.selectedName));
        gameFrontSetFileName(cmdArgFile);
    }

    /* Team combo items */
    static const char *teamItems[] = {
        "None", "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "10", "11", "12", "13", "14", "15", "16"
    };

    /* AI player count combo items */
    static const char *aiCountItems[] = {
        "0", "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "10", "11", "12", "13", "14", "15"
    };

    SetupPhase phase = PHASE_GAME_SETUP;
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

        /* Tick the background game at fixed rate */
        if (hasBg && !bg->paused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bg->paused) {
            lastTickTime = SDL_GetTicks();
        }

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        /* Transparent full-screen host window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##GameSetupBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Centered dialog panel */
#if BOLO_MOBILE
        float panelW = (float)winW * 0.90f;
        float panelH = (float)winH * 0.90f;
#else
        float panelW = 700.0f * s;
        /* Base height for options, expands with player rows */
        float baseH = 420.0f * s;
        float rowH = 28.0f * s;
        float extraRows = (numBots > 0) ? (numBots + 2) * rowH + 8.0f * s : 0; /* +2 for table header + "You" row */
        float panelH = baseH + extraRows;
        if (panelW > (float)winW * 0.95f) panelW = (float)winW * 0.95f;
        if (panelH > (float)winH * 0.95f) panelH = (float)winH * 0.95f;
#endif
        ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##GameSetup", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar);

        if (phase == PHASE_MAP_CHOOSER) {
            /* ============================================================
             * Phase 1: Map Chooser
             * ============================================================ */
            ImGui::Text("Select a map:");
            ImGui::Spacing();

            float headerH = ImGui::GetCursorPosY();
            float footerH = 50.0f * s;
            float contentH = panelH - headerH - footerH;
            if (contentH < 200.0f) contentH = 200.0f;

            mapChooserRender(&mapChooser, renderer, panelW - 16.0f, contentH, s);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* OK / Back */
            {
                float btnW = 80.0f * s;
                float totalW = btnW * 2 + 8.0f * s;
                ImGui::SetCursorPosX((panelW - totalW) * 0.5f);

                if (ImGui::Button("OK", ImVec2(btnW, 0))) {
                    phase = PHASE_GAME_SETUP;
                }
                ImGui::SameLine(0.0f, 8.0f);
                if (ImGui::Button("Back", ImVec2(btnW, 0)) ||
                    (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                     !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                    phase = PHASE_GAME_SETUP;
                }
            }
        } else {
            /* ============================================================
             * Phase 2: Game Setup + Bot Table
             * ============================================================ */
            float headerH = ImGui::GetCursorPosY();
            float footerH = 50.0f * s;
            float contentH = panelH - headerH - footerH;
            if (contentH < 200.0f) contentH = 200.0f;

            /* Determine layout: side-by-side or stacked */
            bool stacked = (panelW < 600.0f * s);

            if (stacked) {
                /* --- Stacked layout (portrait tablet / phone) --- */

                ImGui::BeginChild("##SetupContent", ImVec2(0, contentH), ImGuiChildFlags_None);

                /* Map info row: preview + stats side by side */
                {
                    float previewSz = 120.0f * s;
                    if (mapChooser.previewTex) {
                        ImVec2 uv0, uv1;
                        computePreviewUVs(mapChooser.previewBoundsMinX, mapChooser.previewBoundsMinY,
                                          mapChooser.previewBoundsMaxX, mapChooser.previewBoundsMaxY,
                                          &uv0, &uv1);
                        ImGui::Image((ImTextureID)mapChooser.previewTex,
                                     ImVec2(previewSz, previewSz), uv0, uv1);
                    } else {
                        ImGui::Dummy(ImVec2(previewSz, previewSz));
                    }
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text("%s", mapChooser.selectedName);
                    ImGui::Text("Pillboxes: %d", mapChooser.previewPills);
                    ImGui::Text("Bases: %d", mapChooser.previewBases);
                    ImGui::Text("Starts: %d", mapChooser.previewStarts);
                    ImGui::Spacing();
                    if (ImGui::SmallButton("Change Map")) {
                        phase = PHASE_MAP_CHOOSER;
                    }
                    ImGui::EndGroup();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                /* Game options */
                ImGui::Text("Game Type");
                ImGui::RadioButton("Open Game", &gameTypeIdx, 0);
                ImGui::SameLine();
                ImGui::RadioButton("Tournament", &gameTypeIdx, 1);
                ImGui::SameLine();
                ImGui::RadioButton("Strict", &gameTypeIdx, 2);

                ImGui::Checkbox("Hidden Mines", &hiddenMines);

                ImGui::Spacing();

                ImGui::Text("AI Computer Players");
                ImGui::RadioButton("None", &aiIdx, 0);
                ImGui::SameLine();
                ImGui::RadioButton("Allow", &aiIdx, 1);
                ImGui::SameLine();
                ImGui::RadioButton("Advantage", &aiIdx, 2);
                ImGui::SameLine();
                ImGui::RadioButton("Full Map", &aiIdx, 3);

                ImGui::Spacing();

                ImGui::Checkbox("Time Limit", &useTimeLimit);
                if (useTimeLimit) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60 * s);
                    ImGui::InputInt("min##timelimit", &timeLimitMin, 0, 0);
                    if (timeLimitMin < 0) timeLimitMin = 0;
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                /* Team Setup */
                {
                    ImGui::Text("Team Setup");
                    ImGui::SameLine();
                    ImGui::Text("  Number of AI players:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f * s);
                    int prevNumBotsS = numBots;
                    if (ImGui::Combo("##numAIS", &numBots, aiCountItems, 16)) {
                        if (prevNumBotsS == 0 && numBots > 0 && aiIdx == 0) {
                            aiIdx = 3;
                        }
                    }

                    if (numBrains == 0 && numBots > 0) {
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No brains found");
                    } else if (numBots > 0) {
                        if (ImGui::BeginTable("##TeamTable", 4,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_SizingStretchProp)) {
                            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 25.0f * s);
                            ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed, 60.0f * s);
                            ImGui::TableSetupColumn("Brain", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableHeadersRow();

                            /* Row 1: Human player */
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("1");
                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("You");
                            ImGui::TableSetColumnIndex(2);
                            ImGui::SetNextItemWidth(-1);
                            ImGui::Combo("##tmPS", &playerTeamIdx, teamItems, 17);
                            ImGui::TableSetColumnIndex(3);
                            ImGui::TextDisabled("--");

                            /* AI player rows */
                            for (int i = 0; i < numBots; i++) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::Text("%d", i + 2);

                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("Bot %d", i + 1);

                                ImGui::TableSetColumnIndex(2);
                                ImGui::SetNextItemWidth(-1);
                                char teamId[16];
                                SDL_snprintf(teamId, sizeof(teamId), "##tmS%d", i);
                                ImGui::Combo(teamId, &botSlots[i].teamIdx, teamItems, 17);

                                ImGui::TableSetColumnIndex(3);
                                ImGui::SetNextItemWidth(-1);
                                char brainId[16];
                                SDL_snprintf(brainId, sizeof(brainId), "##brS%d", i);
                                if (ImGui::BeginCombo(brainId, brains[botSlots[i].brainIdx].name)) {
                                    for (int j = 0; j < numBrains; j++) {
                                        bool sel = (j == botSlots[i].brainIdx);
                                        if (ImGui::Selectable(brains[j].name, sel))
                                            botSlots[i].brainIdx = j;
                                        if (sel) ImGui::SetItemDefaultFocus();
                                    }
                                    ImGui::EndCombo();
                                }
                            }
                            ImGui::EndTable();
                        }
                    }
                }

                ImGui::EndChild(); /* ##SetupContent */
            } else {
                /* --- Two-column layout (desktop / landscape tablet) --- */

                float mapPanelW = 240.0f * s;
                float optionsPanelW = panelW - mapPanelW - 24.0f;

                /* Left column: Map preview + stats */
                ImGui::BeginChild("##MapInfoPanel", ImVec2(mapPanelW, contentH), ImGuiChildFlags_None);

                if (mapChooser.previewTex) {
                    ImVec2 uv0, uv1;
                    computePreviewUVs(mapChooser.previewBoundsMinX, mapChooser.previewBoundsMinY,
                                      mapChooser.previewBoundsMaxX, mapChooser.previewBoundsMaxY,
                                      &uv0, &uv1);
                    float previewSz = mapPanelW - 8.0f;
                    float offsetX = (mapPanelW - previewSz) * 0.5f;
                    if (offsetX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
                    ImGui::Image((ImTextureID)mapChooser.previewTex,
                                 ImVec2(previewSz, previewSz), uv0, uv1);
                } else {
                    ImGui::TextDisabled("No preview");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("%s", mapChooser.selectedName);
                ImGui::Text("Pillboxes: %d", mapChooser.previewPills);
                ImGui::Text("Bases: %d", mapChooser.previewBases);
                ImGui::Text("Starts: %d", mapChooser.previewStarts);

                ImGui::Spacing();
                if (ImGui::Button("Change Map", ImVec2(-1, 0))) {
                    phase = PHASE_MAP_CHOOSER;
                }

                ImGui::EndChild(); /* ##MapInfoPanel */

                ImGui::SameLine(0, 8.0f);

                /* Right column: Game options + Bot table */
                ImGui::BeginChild("##OptionsPanel", ImVec2(optionsPanelW, contentH), ImGuiChildFlags_None);

                /* Game Type */
                ImGui::Text("Game Type");
                ImGui::RadioButton("Open Game (pre-armed)", &gameTypeIdx, 0);
                ImGui::RadioButton("Tournament (free ammo early)", &gameTypeIdx, 1);
                ImGui::RadioButton("Strict Tournament (no free ammo)", &gameTypeIdx, 2);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Checkbox("Allow Hidden Mines", &hiddenMines);

                ImGui::Spacing();

                /* AI Computer Players - 2 column layout */
                ImGui::Text("AI Computer Players");
                {
                    float indent = ImGui::GetStyle().IndentSpacing;
                    float availW = ImGui::GetContentRegionAvail().x;
                    float col2X = indent + (availW - indent) * 0.5f;
                    ImGui::Indent();
                    ImGui::RadioButton("No computer tanks", &aiIdx, 0);
                    ImGui::SameLine(col2X);
                    ImGui::RadioButton("Allow computer tanks", &aiIdx, 1);
                    ImGui::RadioButton("Allow with advantage", &aiIdx, 2);
                    ImGui::SameLine(col2X);
                    ImGui::RadioButton("Allow with full map", &aiIdx, 3);
                    ImGui::Unindent();
                }

                ImGui::Spacing();

                /* Time Limit */
                ImGui::Checkbox("Game time limit", &useTimeLimit);
                if (useTimeLimit) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60 * s);
                    ImGui::InputInt("minutes##timelimit", &timeLimitMin, 0, 0);
                    if (timeLimitMin < 0) timeLimitMin = 0;
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                /* Team Setup */
                {
                    ImGui::Text("Team Setup");
                    ImGui::SameLine();
                    ImGui::Text("  Number of AI players:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f * s);
                    int prevNumBots = numBots;
                    if (ImGui::Combo("##numAI", &numBots, aiCountItems, 16)) {
                        /* Auto-enable AI if going from 0 to >0 */
                        if (prevNumBots == 0 && numBots > 0 && aiIdx == 0) {
                            aiIdx = 3; /* Allow with full map */
                        }
                    }

                    if (numBrains == 0 && numBots > 0) {
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No brains found");
                    } else if (numBots > 0) {
                        if (ImGui::BeginTable("##TeamTable", 4,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_SizingStretchProp)) {
                            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f * s);
                            ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch, 0.3f);
                            ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
                            ImGui::TableSetupColumn("Brain", ImGuiTableColumnFlags_WidthStretch, 0.7f);
                            ImGui::TableHeadersRow();

                            /* Row 1: Human player */
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("1");
                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("You");
                            ImGui::TableSetColumnIndex(2);
                            ImGui::SetNextItemWidth(-1);
                            ImGui::Combo("##tmP", &playerTeamIdx, teamItems, 17);
                            ImGui::TableSetColumnIndex(3);
                            ImGui::TextDisabled("--");

                            /* AI player rows */
                            for (int i = 0; i < numBots; i++) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::Text("%d", i + 2);

                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("Bot %d", i + 1);

                                ImGui::TableSetColumnIndex(2);
                                ImGui::SetNextItemWidth(-1);
                                char teamId[16];
                                SDL_snprintf(teamId, sizeof(teamId), "##tm%d", i);
                                ImGui::Combo(teamId, &botSlots[i].teamIdx, teamItems, 17);

                                ImGui::TableSetColumnIndex(3);
                                ImGui::SetNextItemWidth(-1);
                                char brainId[16];
                                SDL_snprintf(brainId, sizeof(brainId), "##br%d", i);
                                if (ImGui::BeginCombo(brainId, brains[botSlots[i].brainIdx].name)) {
                                    for (int j = 0; j < numBrains; j++) {
                                        bool sel = (j == botSlots[i].brainIdx);
                                        if (ImGui::Selectable(brains[j].name, sel))
                                            botSlots[i].brainIdx = j;
                                        if (sel) ImGui::SetItemDefaultFocus();
                                    }
                                    ImGui::EndCombo();
                                }
                            }
                            ImGui::EndTable();
                        }
                    }
                }

                ImGui::EndChild(); /* ##OptionsPanel */
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* --- Start Game / Cancel --- */
            {
                float btnW = 100.0f * s;
                float totalW = btnW * 2 + 8.0f * s;
                ImGui::SetCursorPosX((panelW - totalW) * 0.5f);

                if (ImGui::Button("Start Game", ImVec2(btnW, 0))) {
                    /* Save game options */
                    gameType newGt = (gameTypeIdx == 0) ? gameOpen :
                                     (gameTypeIdx == 1) ? gameTournament : gameStrictTournament;
                    aiType newAi = (aiIdx == 0) ? aiNone :
                                   (aiIdx == 1) ? aiYes :
                                   (aiIdx == 2) ? aiYesAdvantage : aiFull;

                    int32_t newTl = UNLIMITED_GAME_TIME;
                    if (useTimeLimit && timeLimitMin > 0) {
                        newTl = timeLimitMin * NUM_SECONDS * (1000 / 20);
                    }

                    gameFrontSetFileName((char *)mapChooser.selectedPath);
                    gameFrontSetGameOptions((char *)"", newGt, hiddenMines, newAi, 0, newTl, FALSE);

                    /* Build per-bot setup */
                    GameFrontBotSetup botSetup;
                    memset(&botSetup, 0, sizeof(botSetup));
                    botSetup.playerTeamNumber = (uint8_t)playerTeamIdx;
                    if (aiIdx != 0) {
                        botSetup.count = numBots;
                        for (int i = 0; i < numBots; i++) {
                            if (numBrains > 0) {
                                SDL_strlcpy(botSetup.bots[i].brainPath,
                                            brains[botSlots[i].brainIdx].path,
                                            sizeof(botSetup.bots[i].brainPath));
                            }
                            botSetup.bots[i].teamNumber = (uint8_t)botSlots[i].teamIdx;
                        }
                    }
                    gameFrontSetBotSetup(&botSetup);

                    result = 1;
                    running = false;
                }

                ImGui::SameLine(0.0f, 8.0f);

                if (ImGui::Button("Cancel", ImVec2(btnW, 0)) ||
                    (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                     !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                    running = false;
                }
            }
        }

        ImGui::End(); /* ##GameSetup panel */
        ImGui::End(); /* ##GameSetupBg host */

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        /* Draw map background and semi-transparent overlay */
        if (hasBg) {
            bgGameRenderWithOverlay(bg, renderer, winW, winH);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    /* Clean up map chooser */
    mapChooserDestroy(&mapChooser);

    /* Tear down ImGui */
    dialogDismissKeyboard(window);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    /* Restore logical presentation */
    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    /* Flush any quit events so the main loop doesn't exit immediately */
    SDL_FlushEvent(SDL_EVENT_QUIT);

    return result;
}
