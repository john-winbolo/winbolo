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
 * Name:          imgui_setname.cpp
 * Purpose:       ImGui Set Player Name dialog.
 *                ImGui set-name dialog.
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
#include "../../../bolo/client_sim.h"
#include "../../../bolo/util.h"
#include "imgui_setname.h"
}

static const int DIALOG_W = 380;
static const int DIALOG_H = 160;

extern "C" void imguiSetNameShow(ClientSim *cs, bool inGame) {
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
    /* Resize and show window for the dialog */
    dialogSetWindowSize(window, DIALOG_W, DIALOG_H);
    dialogSetWindowTitle(window, "Set Player Name");
    SDL_SetWindowResizable(window, false);
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

    /* Load current player name */
    char playerName[PLAYER_NAME_LEN];
    playerName[0] = '\0';
    if (inGame) {
        screenGetPlayerNameCS(cs, playerName);
    } else {
        gameFrontGetPlayerName(playerName);
    }

    /* Error popup state */
    const char *errorMsg = nullptr;
    bool focusInput = true;

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
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::Begin("##SetName", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        ImGui::Text("Please enter your player name.");
        ImGui::Spacing();

        ImGui::Text("Player Name:");
        ImGui::SameLine(120.0f * s);
        ImGui::SetNextItemWidth((float)winW - 120.0f * s - 16.0f);
        if (focusInput) {
            ImGui::SetKeyboardFocusHere();
            focusInput = false;
        }
        bool enterPressed = ImGui::InputText("##name", playerName, PLAYER_NAME_LEN,
                                              ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btnW = 80.0f * s;
        float btnX = ((float)winW - btnW * 2 - 8.0f * s) / 2.0f;
        ImGui::SetCursorPosX(btnX);

        if (ImGui::Button("OK", ImVec2(btnW, 0)) || enterPressed) {
            char newName[PLAYER_NAME_LEN];
            SDL_strlcpy(newName, playerName, PLAYER_NAME_LEN);
            utilStripName(newName);

            if (newName[0] == '\0') {
                errorMsg = "Sorry, you can not leave this blank";
                ImGui::OpenPopup("Error##setname");
            } else if (newName[0] == '*') {
                errorMsg = "Sorry, names can not begin with a '*'";
                ImGui::OpenPopup("Error##setname");
            } else if (inGame) {
                char oldName[PLAYER_NAME_LEN];
                oldName[0] = '\0';
                screenGetPlayerNameCS(cs, oldName);
                if (strcmp(oldName, newName) == 0) {
                    running = false;
                } else {
                    bool changeOK = screenSetPlayerNameCS(cs, newName);
                    if (!changeOK) {
                        errorMsg = "That name is already in use by another player.";
                        ImGui::OpenPopup("Error##setname");
                    } else {
                        running = false;
                    }
                }
            } else {
                gameFrontSetPlayerName(newName);
                running = false;
            }
        }

        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button("Cancel", ImVec2(btnW, 0)) ||
            (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
             !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
            running = false;
        }

        /* Error popup */
        if (ImGui::BeginPopupModal("Error##setname", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", errorMsg ? errorMsg : "");
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

    /* Tear down ImGui */
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
