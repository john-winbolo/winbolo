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
 * Name:          imgui_winbolonet.cpp
 * Purpose:       ImGui WinBolo.net settings dialog.
 *                ImGui WinBolo.net dialog.
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
#include "imgui_winbolonet.h"
}

static const int DIALOG_W = 420;
static const int DIALOG_H = 300;

extern "C" void imguiWinbolonetShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return;

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
    dialogSetWindowTitle(window, "WinBolo.net Settings");
    SDL_SetWindowResizable(window, false);
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

    /* Load current settings */
    char userName[FILENAME_MAX];
    char password[FILENAME_MAX];
    bool useWbn = false;
    bool savePass = false;

    userName[0] = '\0';
    password[0] = '\0';
    gameFrontGetPlayerName(userName);
    gameFrontGetWinbolonetSettings(password, &useWbn, &savePass);

    const char *errorMsg = nullptr;
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

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::Begin("##WinboloNet", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        ImGui::TextWrapped(
            "Winbolo.net is a free real time game tracking and player "
            "statistics website. To signup or for more information "
            "please visit http://www.winbolo.net");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float labelW = 100.0f;
        float inputW = (float)winW - labelW - 30.0f;

        /* Username (read-only) */
        ImGui::Text("Username:");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##user", userName, FILENAME_MAX,
                         ImGuiInputTextFlags_ReadOnly);

        /* Password */
        ImGui::Text("Password:");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        if (!useWbn) ImGui::BeginDisabled();
        ImGui::InputText("##pass", password, FILENAME_MAX,
                         ImGuiInputTextFlags_Password);
        if (!useWbn) ImGui::EndDisabled();

        ImGui::Spacing();

        ImGui::Checkbox("Use Winbolo.net", &useWbn);
        ImGui::Checkbox("Save My Winbolo.net Password", &savePass);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btnW = 80.0f;
        float btnX = ((float)winW - btnW * 2 - 8.0f) / 2.0f;
        ImGui::SetCursorPosX(btnX);

        if (ImGui::Button("OK", ImVec2(btnW, 0))) {
            if (useWbn && strlen(password) == 0) {
                errorMsg = "Sorry, you must enter a username and password if "
                           "you wish to participate in winbolo.net";
                ImGui::OpenPopup("Error##wbn");
            } else {
                gameFrontSetWinbolonetSettings(password, useWbn, savePass);
                running = false;
            }
        }

        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button("Cancel", ImVec2(btnW, 0))) {
            running = false;
        }

        /* Error popup */
        if (ImGui::BeginPopupModal("Error##wbn", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", errorMsg ? errorMsg : "");
            ImGui::Spacing();
            if (ImGui::Button("OK##err", ImVec2(80, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    /* Restore logical presentation */
    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    SDL_FlushEvent(SDL_EVENT_QUIT);
}
