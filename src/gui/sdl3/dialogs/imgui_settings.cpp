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
 * Name:          imgui_settings.cpp
 * Purpose:       ImGui pre-game settings dialog.
 *                Shown as an overlay panel over the
 *                background game, same as the welcome
 *                dialog.
 *********************************************************/

#include <cstring>

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
#include "../../../bolo/screen.h"
#include "../bg_game.h"
#include "imgui_settings.h"
#include "imgui_winbolonet.h"
}

/* Frame-rate / zoom constants (mirrors winbolo.h values) */
#ifndef FRAME_RATE_10
#define FRAME_RATE_10  19
#define FRAME_RATE_12  23
#define FRAME_RATE_15  28
#define FRAME_RATE_20  37
#define FRAME_RATE_30  55
#define FRAME_RATE_50  82
#define FRAME_RATE_60  111
#endif
#ifndef ZOOM_FACTOR_NORMAL
#define ZOOM_FACTOR_NORMAL 1
#define ZOOM_FACTOR_DOUBLE 2
#define ZOOM_FACTOR_QUAD   4
#endif

/* Globals from winbolo.c */
extern "C" {
  extern bool showGunsight;
  extern bool autoScrollingEnabled;
  extern bool showPillLabels;
  extern bool showBaseLabels;
  extern bool hideMainView;
  extern int  frameRate;
  extern labelLen labelMsg;
  extern labelLen labelTank;
  extern bool labelSelf;
  extern BYTE zoomFactor;
  extern bool soundEffects;
  extern bool backgroundSound;
  extern bool useSoundKeepalive;
  extern bool showNewswireMessages;
  extern bool showAssistantMessages;
  extern bool showAIMessages;
  extern bool showNetworkStatusMessages;
  extern bool showNetworkDebugMessages;

  void windowSetFrameRate(int newFrameRate, bool setTimer);
  void windowAutomaticScrolling_toggle(void);
  void windowShowGunsight_toggle(struct ClientSim *cs);
  void windowShowPillLabels_toggle(struct ClientSim *cs);
  void windowShowBaseLabels_toggle(struct ClientSim *cs);
  void windowSoundEffects_toggle(void);
  void windowBackgroundSoundChange_toggle(void);
  void windowSoundKeepalive(void);
  void windowMenuNewswire_toggle(void);
  void windowMenuAssistant_toggle(void);
  void windowMenuAI_toggle(void);
  void windowMenuNetwork_toggle(void);
  void windowMenuNetworkDebug_toggle(void);
  void windowHideMainView_toggle(void);
  void windowLabelOwnTank_toggle(void);
  void windowSetMessageLabelLen(labelLen newLen);
  void windowSetTankLabelLen(labelLen newLen);
}

extern "C" void imguiSettingsShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return;

    /* Save logical presentation */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

#if !BOLO_MOBILE
    /* Keep the window at the same size as the welcome dialog */
    dialogSetWindowSize(window, 1024, 768);
    dialogSetWindowTitle(window, "WinBolo - Settings");
    SDL_SetWindowResizable(window, true);
#endif
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

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

    /* Load current player name */
    char playerName[FILENAME_MAX];
    playerName[0] = '\0';
    gameFrontGetPlayerName(playerName);

    /* Background game */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);
    Uint64 lastTickTime = SDL_GetTicks();

    bool pendingWbn = false;
    bool running = true;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 ev.window.windowID == SDL_GetWindowID(window))) {
                running = false;
            }
        }

        /* Tick the background game */
        if (hasBg && !bg->paused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bg->paused) {
            lastTickTime = SDL_GetTicks();
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        /* Transparent full-screen host window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##SettingsBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Centered overlay panel */
        float panelW = 500.0f * s, panelH = 580.0f * s;
        if (panelW > (float)winW * 0.95f) panelW = (float)winW * 0.95f;
        if (panelH > (float)winH * 0.95f) panelH = (float)winH * 0.95f;

        ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##SettingsPanel", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        /* Title */
        {
            ImGui::SetWindowFontScale(1.4f);
            const char *title = "Settings";
            ImVec2 textSize = ImGui::CalcTextSize(title);
            ImGui::SetCursorPosX((panelW - textSize.x) * 0.5f);
            ImGui::Text("%s", title);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* ---- Player ---- */
        if (ImGui::CollapsingHeader("Player", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Player Name:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("##playerName", playerName, 33,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                playerName[32] = '\0';
                if (playerName[0] != '\0') {
                    gameFrontSetPlayerName(playerName);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply##name")) {
                playerName[32] = '\0';
                if (playerName[0] != '\0') {
                    gameFrontSetPlayerName(playerName);
                }
            }

            ImGui::Spacing();
            if (ImGui::Button("Sign in to WBN...")) {
                pendingWbn = true;
            }
        }

        /* ---- Display ---- */
        if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char *frLabels[] = { "60", "50", "30", "20", "15", "12", "10" };
            int frValues[] = { FRAME_RATE_60, FRAME_RATE_50, FRAME_RATE_30,
                               FRAME_RATE_20, FRAME_RATE_15, FRAME_RATE_12, FRAME_RATE_10 };
            int curFrIdx = 2;
            for (int i = 0; i < 7; i++) {
                if (frameRate == frValues[i]) { curFrIdx = i; break; }
            }
            ImGui::Text("Frame Rate:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (ImGui::BeginCombo("##framerate", frLabels[curFrIdx])) {
                for (int i = 0; i < 7; i++) {
                    bool selected = (curFrIdx == i);
                    if (ImGui::Selectable(frLabels[i], selected)) {
                        windowSetFrameRate(frValues[i], false);
                    }
                }
                ImGui::EndCombo();
            }

#if !BOLO_MOBILE
            {
                bool as = (bool)autoScrollingEnabled;
                if (ImGui::Checkbox("Automatic Scrolling", &as)) {
                    windowAutomaticScrolling_toggle();
                }
            }
            {
                bool gs = (bool)showGunsight;
                if (ImGui::Checkbox("Show Gunsight", &gs)) {
                    /* Pre-game: just toggle the global directly */
                    showGunsight = !showGunsight;
                }
            }
#endif
        }

        /* ---- Labels ---- */
        if (ImGui::CollapsingHeader("Labels", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Message Names:");
            ImGui::SameLine();
            {
                bool isShort = (labelMsg == lblShort);
                if (ImGui::RadioButton("Short##msg", isShort)) windowSetMessageLabelLen(lblShort);
                ImGui::SameLine();
                if (ImGui::RadioButton("Long##msg", !isShort)) windowSetMessageLabelLen(lblLong);
            }

            ImGui::Text("Tank Labels:");
            ImGui::SameLine();
            {
                if (ImGui::RadioButton("None##tank", labelTank == lblNone))  windowSetTankLabelLen(lblNone);
                ImGui::SameLine();
                if (ImGui::RadioButton("Short##tank", labelTank == lblShort)) windowSetTankLabelLen(lblShort);
                ImGui::SameLine();
                if (ImGui::RadioButton("Long##tank", labelTank == lblLong))  windowSetTankLabelLen(lblLong);
            }
            {
                bool noSelf = !(bool)labelSelf;
                if (ImGui::Checkbox("Don't label own tank", &noSelf)) {
                    windowLabelOwnTank_toggle();
                }
            }
            {
                bool pl = (bool)showPillLabels;
                if (ImGui::Checkbox("Pillbox Labels", &pl)) {
                    showPillLabels = !showPillLabels;
                }
            }
            {
                bool bl = (bool)showBaseLabels;
                if (ImGui::Checkbox("Refuelling Base Labels", &bl)) {
                    showBaseLabels = !showBaseLabels;
                }
            }
        }

        /* ---- Sound ---- */
        if (ImGui::CollapsingHeader("Sound", ImGuiTreeNodeFlags_DefaultOpen)) {
            {
                bool se = (bool)soundEffects;
                if (ImGui::Checkbox("Sound Effects", &se)) {
                    windowSoundEffects_toggle();
                }
            }
            {
                bool bgs = (bool)backgroundSound;
                if (ImGui::Checkbox("Background Sound", &bgs)) {
                    windowBackgroundSoundChange_toggle();
                }
            }
#if !BOLO_MOBILE
            {
                bool sk = (bool)useSoundKeepalive;
                if (ImGui::Checkbox("Sound Keepalive", &sk)) {
                    windowSoundKeepalive();
                }
            }
#endif
        }

        /* ---- Messages ---- */
        if (ImGui::CollapsingHeader("Messages", ImGuiTreeNodeFlags_DefaultOpen)) {
            {
                bool nw = (bool)showNewswireMessages;
                if (ImGui::Checkbox("Newswire Messages", &nw)) {
                    windowMenuNewswire_toggle();
                }
            }
            {
                bool am = (bool)showAssistantMessages;
                if (ImGui::Checkbox("Assistant Messages", &am)) {
                    windowMenuAssistant_toggle();
                }
            }
            {
                bool ai = (bool)showAIMessages;
                if (ImGui::Checkbox("AI Brain Messages", &ai)) {
                    windowMenuAI_toggle();
                }
            }
            {
                bool ns = (bool)showNetworkStatusMessages;
                if (ImGui::Checkbox("Network Status Messages", &ns)) {
                    windowMenuNetwork_toggle();
                }
            }
            {
                bool nd = (bool)showNetworkDebugMessages;
                if (ImGui::Checkbox("Network Debug Messages", &nd)) {
                    windowMenuNetworkDebug_toggle();
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btnW = 80.0f;
        float btnX = (panelW - btnW) / 2.0f;
        ImGui::SetCursorPosX(btnX);
        if (ImGui::Button("Close", ImVec2(btnW, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            running = false;
        }

        ImGui::End(); /* ##SettingsPanel */
        ImGui::End(); /* ##SettingsBg */

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        /* Render background game with overlay */
        if (hasBg) {
            bgGameRender(bg, renderer, winW, winH);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
            SDL_FRect overlayRect = { 0, 0, (float)winW, (float)winH };
            SDL_RenderFillRect(renderer, &overlayRect);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);

        /* Deferred WBN dialog -- must run outside our frame */
        if (pendingWbn) {
            pendingWbn = false;
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();

            imguiWinbolonetShow();

            /* Re-create our context */
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO &rio = ImGui::GetIO();
            rio.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            rio.IniFilename = nullptr;
            ImGui::StyleColorsDark();
            imguiApplyBoloTheme();
            ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
            ImGui_ImplSDLRenderer3_Init(renderer);
            dialogApplyScaling(s);

#if !BOLO_MOBILE
            dialogSetWindowSize(window, 1024, 768);
            dialogSetWindowTitle(window, "WinBolo - Settings");
            SDL_SetWindowResizable(window, true);
#endif

            /* Refresh player name in case WBN changed it */
            gameFrontGetPlayerName(playerName);
        }
    }

    dialogDismissKeyboard(window);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    SDL_FlushEvent(SDL_EVENT_QUIT);
}
