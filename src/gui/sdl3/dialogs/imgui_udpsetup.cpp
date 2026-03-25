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
 * Name:          imgui_udpsetup.cpp
 * Purpose:       ImGui UDP (Internet) Setup dialog.
 *                ImGui UDP setup dialog.
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
#include "../bg_game.h"
#include "../../gamefront.h"
#include "../../../bolo/global.h"
#include "../../../bolo/util.h"
#include "imgui_udpsetup.h"
}

static const int DIALOG_W = 1024;
static const int DIALOG_H = 768;

/* Validate inputs and save to gamefront. Returns true if valid. */
static bool saveOptions(char *playerName, char *address,
                        char *targetPortBuf, char *myPortBuf,
                        bool rememberName, bool isJoin) {
    char pn[PLAYER_NAME_LEN];
    SDL_strlcpy(pn, playerName, PLAYER_NAME_LEN);
    utilStripName(pn);

    if (strlen(pn) == 0) {
        return false;
    }
    if (pn[0] == '*') {
        return false;
    }

    if (strlen(address) == 0 && isJoin) {
        return false;
    }

    char *end;
    unsigned long val;

    val = strtoul(targetPortBuf, &end, 10);
    if (*end != '\0' || val > 65535) {
        return false;
    }
    unsigned short targetUdp = (unsigned short)val;

    val = strtoul(myPortBuf, &end, 10);
    if (*end != '\0' || val > 65535) {
        return false;
    }
    unsigned short myUdp = (unsigned short)val;

    gameFrontSetRemeber(rememberName);
    gameFrontSetUdpOptions(pn, address, targetUdp, myUdp);
    return true;
}

extern "C" int imguiUdpSetupShow(void) {
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
    /* Resize and show window for the dialog */
    dialogSetWindowSize(window, DIALOG_W, DIALOG_H);
    dialogSetWindowTitle(window, "UDP (Internet) Setup");
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

    /* Load current UDP options */
    char playerName[PLAYER_NAME_LEN];
    char address[FILENAME_MAX];
    unsigned short targetPort, myPort;
    playerName[0] = '\0';
    address[0] = '\0';
    gameFrontGetUdpOptions(playerName, address, &targetPort, &myPort);

    char targetPortBuf[16];
    char myPortBuf[16];
    SDL_snprintf(targetPortBuf, sizeof(targetPortBuf), "%u", targetPort);
    SDL_snprintf(myPortBuf, sizeof(myPortBuf), "%u", myPort);

    bool rememberName = gameFrontGetRemeber();

    /* Tracker options */
    char trackerAddr[FILENAME_MAX];
    unsigned short trackerPort;
    bool trackerEnabled;
    trackerAddr[0] = '\0';
    gameFrontGetTrackerOptions(trackerAddr, &trackerPort, &trackerEnabled);

    char trackerPortBuf[16];
    SDL_snprintf(trackerPortBuf, sizeof(trackerPortBuf), "%u", trackerPort);

    /* Error message popup state */
    const char *errorMsg = nullptr;

    /* Tracker popup state */
    bool showTracker = false;

    int result = 0;
    bool running = true;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            if (ev.type == SDL_EVENT_QUIT) {
                gameFrontSetDlgState(openWelcome);
                running = false;
            }
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(window)) {
                gameFrontSetDlgState(openWelcome);
                running = false;
            }
        }

        /* Tick the background game at fixed rate (unless paused) */
        if (hasBg && !bg->paused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bg->paused) {
            lastTickTime = SDL_GetTicks();
        }

        /* Query actual window size each frame */
        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        /* Transparent full-screen host window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##UdpSetupBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Centered dialog panel (wider on tablets for better input layout) */
#if BOLO_MOBILE || defined(__IPHONEOS__)
        float panelW = (float)winW * 0.80f, panelH = 440.0f * s;
#else
        float panelW = 500.0f * s, panelH = 440.0f * s;
#endif
        if (panelW > (float)winW * 0.95f) panelW = (float)winW * 0.95f;
        if (panelH > (float)winH * 0.95f) panelH = (float)winH * 0.95f;
        ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##UdpSetup", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        /* --- Blurb --- */
        ImGui::TextWrapped(
            "To join an internet Bolo game, you must give the name (or IP address) "
            "of a host machine running Bolo, and the UDP port number of the Bolo "
            "process on that machine.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Input fields --- */
#if BOLO_MOBILE || defined(__IPHONEOS__)
        /* On mobile, labels left, narrow inputs right-aligned on same line */
        float inputW = panelW * 0.225f;
        float labelW = panelW - inputW - 32.0f * s;

        ImGui::Text("Machine Name (or IP address):");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##address", address, FILENAME_MAX);

        ImGui::Text("UDP port of Bolo on that machine:");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##targetPort", targetPortBuf, sizeof(targetPortBuf),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::Text("UDP port for Bolo on this machine:");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##myPort", myPortBuf, sizeof(myPortBuf),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::Text("Your player name for the game:");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##playerName", playerName, PLAYER_NAME_LEN);
#else
        float labelW = 250.0f * s;
        float inputW = panelW - labelW - 60.0f * s;

        ImGui::Text("Machine Name (or IP address):");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##address", address, FILENAME_MAX);

        ImGui::Text("UDP port of Bolo on that machine:");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##targetPort", targetPortBuf, sizeof(targetPortBuf),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::Text("UDP port for Bolo on this machine:");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##myPort", myPortBuf, sizeof(myPortBuf),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::Text("Your player name for the game:");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##playerName", playerName, PLAYER_NAME_LEN);
#endif

        ImGui::Spacing();

        /* --- Remember + Tracker Setup --- */
        ImGui::Checkbox("Remember player name", &rememberName);
        ImGui::SameLine(panelW - 140.0f * s);
        if (ImGui::Button("Tracker Setup", ImVec2(120 * s, 0))) {
            /* Reload tracker options in case they changed */
            gameFrontGetTrackerOptions(trackerAddr, &trackerPort, &trackerEnabled);
            SDL_snprintf(trackerPortBuf, sizeof(trackerPortBuf), "%u", trackerPort);
            showTracker = true;
            ImGui::OpenPopup("Tracker Config");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Action buttons --- */
        float btnW = 80.0f * s;

        ImGui::Text("Click \"New\" to begin a game");
        ImGui::SameLine(panelW - btnW - 16.0f * s);
        if (ImGui::Button("New", ImVec2(btnW, 0))) {
            /* Check ports aren't equal */
            unsigned long v1 = strtoul(targetPortBuf, nullptr, 10);
            unsigned long v2 = strtoul(myPortBuf, nullptr, 10);
            if (v1 == v2) {
                errorMsg = "Server and own ports are the same!";
                ImGui::OpenPopup("Error##udp");
            } else if (saveOptions(playerName, address, targetPortBuf, myPortBuf,
                                   rememberName, false)) {
                gameFrontSetDlgState(openUdpSetup);
                result = 1;
                running = false;
            } else {
                errorMsg = "Something isn't correct here...";
                ImGui::OpenPopup("Error##udp");
            }
        }

        ImGui::Text("Click \"Join\" to join an existing game");
        ImGui::SameLine(panelW - btnW - 16.0f * s);
        if (ImGui::Button("Join", ImVec2(btnW, 0))) {
            if (saveOptions(playerName, address, targetPortBuf, myPortBuf,
                            rememberName, true)) {
                gameFrontSetDlgState(openUdpJoin);
                result = 1;
                running = false;
            } else {
                errorMsg = "Something isn't correct here...";
                ImGui::OpenPopup("Error##udp");
            }
        }

        ImGui::TextWrapped("Click \"Rejoin\" to rejoin a game and reclaim your old possessions");
        ImGui::SameLine(panelW - btnW - 16.0f * s);
        if (ImGui::Button("Rejoin", ImVec2(btnW, 0))) {
            if (saveOptions(playerName, address, targetPortBuf, myPortBuf,
                            rememberName, true)) {
                gameFrontEnableRejoin();
                gameFrontSetDlgState(openUdpJoin);
                result = 1;
                running = false;
            } else {
                errorMsg = "Something isn't correct here...";
                ImGui::OpenPopup("Error##udp");
            }
        }

        ImGui::Spacing();

        /* --- Cancel --- */
        {
            float cancelX = panelW - btnW - 16.0f * s;
            ImGui::SetCursorPosX(cancelX);
            if (ImGui::Button("Cancel", ImVec2(btnW, 0)) ||
                (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                 !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                gameFrontSetDlgState(openWelcome);
                running = false;
            }
        }

        /* --- Error popup --- */
        if (ImGui::BeginPopupModal("Error##udp", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", errorMsg ? errorMsg : "");
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(80, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        /* --- Tracker Config popup --- */
        if (ImGui::BeginPopupModal("Tracker Config", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Tracker Address:");
            ImGui::SameLine(120 * s);
            ImGui::SetNextItemWidth(160 * s);
            if (!trackerEnabled) ImGui::BeginDisabled();
            ImGui::InputText("##trackerAddr", trackerAddr, FILENAME_MAX);
            if (!trackerEnabled) ImGui::EndDisabled();

            ImGui::Text("Tracker Port:");
            ImGui::SameLine(120 * s);
            ImGui::SetNextItemWidth(160 * s);
            if (!trackerEnabled) ImGui::BeginDisabled();
            ImGui::InputText("##trackerPort", trackerPortBuf, sizeof(trackerPortBuf),
                             ImGuiInputTextFlags_CharsDecimal);
            if (!trackerEnabled) ImGui::EndDisabled();

            ImGui::Checkbox("Use Tracker", &trackerEnabled);

            ImGui::Spacing();
            if (ImGui::Button("OK##tracker", ImVec2(80, 0))) {
                char *end;
                unsigned long pval = strtoul(trackerPortBuf, &end, 10);
                if (pval > 65535) pval = 65535;
                gameFrontSetTrackerOptions(trackerAddr, (unsigned short)pval,
                                           trackerEnabled);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0.0f, 8.0f);
            if (ImGui::Button("Cancel##tracker", ImVec2(80, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End(); /* ##UdpSetup panel */
        ImGui::End(); /* ##UdpSetupBg host */

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

    /* Tear down ImGui */
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
