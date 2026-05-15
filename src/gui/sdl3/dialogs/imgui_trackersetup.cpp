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
 * Name:          imgui_trackersetup.cpp
 * Purpose:       ImGui Tracker Setup dialog.
 *                ImGui tracker setup dialog.
 *********************************************************/

#include <cstring>
#include <cstdlib>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "imgui_nav_outline.h"
#include "../imgui_steam_nav.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../gamefront.h"
#include "../../lang.h"
#include "imgui_trackersetup.h"
}

static const int DIALOG_W = 400;
static const int DIALOG_H = 200;

extern "C" int imguiTrackerSetupShow(void) {
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
    dialogSetWindowTitle(window, langGetText(STR_DLGTRACKER_WINTITLE));
    SDL_SetWindowResizable(window, false);
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiRegisterPlatformOpenUrl();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    /* Load current tracker options */
    char trackerAddr[FILENAME_MAX];
    unsigned short trackerPort;
    bool trackerEnabled;
    trackerAddr[0] = '\0';
    gameFrontGetTrackerOptions(trackerAddr, &trackerPort, &trackerEnabled);

    char portBuf[16];
    SDL_snprintf(portBuf, sizeof(portBuf), "%u", trackerPort);

    const char *errorMsg = nullptr;
    int result = 0;
    bool running = true;

    char errPopupId[64];
    SDL_snprintf(errPopupId, sizeof(errPopupId), "%s##tracker", langGetText(STR_ERR_TITLE));

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 ev.window.windowID == SDL_GetWindowID(window))) {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::Begin("##TrackerSetup", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        ImGui::Checkbox(langGetText(STR_DLGTRACKER_USETRACKER), &trackerEnabled);
        ImGui::Spacing();

        float labelW = 130.0f;
        float inputW = (float)winW - labelW - 30.0f;

        ImGui::TextUnformatted(langGetText(STR_DLGTRACKER_TRACKERADDRESS));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        if (!trackerEnabled) ImGui::BeginDisabled();
        ImGui::InputText("##addr", trackerAddr, FILENAME_MAX);
        if (!trackerEnabled) ImGui::EndDisabled();

        ImGui::TextUnformatted(langGetText(STR_DLGTRACKER_TRACKERPORT));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        if (!trackerEnabled) ImGui::BeginDisabled();
        ImGui::InputText("##port", portBuf, sizeof(portBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        if (!trackerEnabled) ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btnW = 80.0f;
        float btnX = ((float)winW - btnW * 2 - 8.0f) / 2.0f;
        ImGui::SetCursorPosX(btnX);

        if (ImGui::Button(langGetText(STR_OK), ImVec2(btnW, 0))) {
            char *end;
            unsigned long pval = strtoul(portBuf, &end, 10);
            if (*end != '\0' || pval > 65535) {
                errorMsg = langGetText(STR_DLGTRACKER_INVALIDPORT);
                ImGui::OpenPopup(errPopupId);
            } else {
                gameFrontSetTrackerOptions(trackerAddr, (unsigned short)pval,
                                           trackerEnabled);
                result = 1;
                running = false;
            }
        }
        imguiHandOnHover();

        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, 0))) {
            running = false;
        }
        imguiHandOnHover();

        /* Error popup */
        static float s_fadeTrackerErr = 0.0f;
        if (ImGui::BeginPopupModal(errPopupId, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                imguiPopupFadeAlpha(&s_fadeTrackerErr));
            ImGui::Text("%s", errorMsg ? errorMsg : "");
            ImGui::Spacing();
            {
                char okBuf[64];
                snprintf(okBuf, sizeof(okBuf), "%s##err", langGetText(STR_OK));
                if (ImGui::Button(okBuf, ImVec2(80, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                imguiHandOnHover();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        ImGui::End();

        dialogDrawNavOutline();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

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
