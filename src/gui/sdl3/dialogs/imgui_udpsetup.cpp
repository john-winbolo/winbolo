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
#include "imgui_nav_outline.h"
#include "../imgui_steam_nav.h"
#include "dialog_footer.h"

extern "C" {
#include "../sdl3draw.h"
#include "../bg_game.h"
#include "../../gamefront.h"
#include "global.h"
#include "util.h"
#include "../../lang.h"
#include "../input_source.h"
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
    dialogSetWindowTitle(window, langGetText(STR_DLGTCP_WINTITLE));
    SDL_SetWindowResizable(window, true);
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    /* Set up ImGui context for this dialog */
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

    /* Error message popup state */
    const char *errorMsg = nullptr;

    int result = 0;
    bool running = true;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
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
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

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
        ImGui::TextWrapped("%s", langGetText(STR_DLGTCP_BLURB));
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Input fields --- */
#if BOLO_MOBILE || defined(__IPHONEOS__)
        /* On mobile, labels left, narrow inputs right-aligned on same line */
        float inputW = panelW * 0.225f;
        float labelW = panelW - inputW - 32.0f * s;

        ImGui::TextUnformatted(langGetText(STR_DLGTCP_MACHINENAME));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##address", address, FILENAME_MAX);

        ImGui::TextUnformatted(langGetText(STR_DLGTCP_THEREUDP));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##targetPort", targetPortBuf, sizeof(targetPortBuf),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::TextUnformatted(langGetText(STR_DLGTCP_USUDP));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##myPort", myPortBuf, sizeof(myPortBuf),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::TextUnformatted(langGetText(STR_DLGTCP_NAME));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##playerName", playerName, PLAYER_NAME_LEN);
#else
        float labelW = 250.0f * s;
        float inputW = panelW - labelW - 60.0f * s;

        ImGui::TextUnformatted(langGetText(STR_DLGTCP_MACHINENAME));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##address", address, FILENAME_MAX);

        ImGui::TextUnformatted(langGetText(STR_DLGTCP_THEREUDP));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##targetPort", targetPortBuf, sizeof(targetPortBuf),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::TextUnformatted(langGetText(STR_DLGTCP_USUDP));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##myPort", myPortBuf, sizeof(myPortBuf),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::TextUnformatted(langGetText(STR_DLGTCP_NAME));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##playerName", playerName, PLAYER_NAME_LEN);
#endif

        ImGui::Spacing();

        char errPopupId[64];
        SDL_snprintf(errPopupId, sizeof(errPopupId), "%s##udp", langGetText(STR_ERR_TITLE));

        /* --- Remember --- */
        ImGui::Checkbox(langGetText(STR_DLGTCP_REMEMBER), &rememberName);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Action buttons --- */
        float btnW = 80.0f * s;

        if (inputSourceCurrent() == INPUT_SOURCE_KEYBOARD) {
            ImGui::TextUnformatted(langGetText(STR_DLGTCP_NEWBLURB));
        }
        ImGui::SameLine(panelW - btnW - 16.0f * s);
        if (ImGui::Button(langGetText(STR_DLGTCP_NEW), ImVec2(btnW, 0))) {
            /* Check ports aren't equal */
            unsigned long v1 = strtoul(targetPortBuf, nullptr, 10);
            unsigned long v2 = strtoul(myPortBuf, nullptr, 10);
            if (v1 == v2) {
                errorMsg = langGetText(STR_ERR_DLGTCP_PORTS);
                ImGui::OpenPopup(errPopupId);
            } else if (saveOptions(playerName, address, targetPortBuf, myPortBuf,
                                   rememberName, false)) {
                gameFrontSetDlgState(openUdpSetup);
                result = 1;
                running = false;
            } else {
                errorMsg = langGetText(STR_ERR_DLGTCP_NOTRIGHT);
                ImGui::OpenPopup(errPopupId);
            }
        }
        imguiHandOnHover();

        if (inputSourceCurrent() == INPUT_SOURCE_KEYBOARD) {
            ImGui::TextUnformatted(langGetText(STR_DLGTCP_JOINBLURB));
        }
        ImGui::SameLine(panelW - btnW - 16.0f * s);
        if (ImGui::Button(langGetText(STR_DLGTCP_JOIN), ImVec2(btnW, 0))) {
            if (saveOptions(playerName, address, targetPortBuf, myPortBuf,
                            rememberName, true)) {
                gameFrontSetDlgState(openUdpJoin);
                result = 1;
                running = false;
            } else {
                errorMsg = langGetText(STR_ERR_DLGTCP_NOTRIGHT);
                ImGui::OpenPopup(errPopupId);
            }
        }
        imguiHandOnHover();

        if (inputSourceCurrent() == INPUT_SOURCE_KEYBOARD) {
            ImGui::TextWrapped("%s", langGetText(STR_DLGTCP_REJOINBLURB));
        }
        ImGui::SameLine(panelW - btnW - 16.0f * s);
        if (ImGui::Button(langGetText(STR_DLGTCP_REJOIN), ImVec2(btnW, 0))) {
            if (saveOptions(playerName, address, targetPortBuf, myPortBuf,
                            rememberName, true)) {
                gameFrontEnableRejoin();
                gameFrontSetDlgState(openUdpJoin);
                result = 1;
                running = false;
            } else {
                errorMsg = langGetText(STR_ERR_DLGTCP_NOTRIGHT);
                ImGui::OpenPopup(errPopupId);
            }
        }
        imguiHandOnHover();

        ImGui::Spacing();

        /* --- Cancel --- muted grey, right-aligned (panel-screen convention). */
        {
            float cancelX = panelW - btnW - 16.0f * s;
            ImGui::SetCursorPosX(cancelX);
            WBUI::PushCancelStyle();
            bool cancelClicked = ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, 0));
            WBUI::PopCancelStyle();
            if (cancelClicked || WBUI::CancelKeyPressed()) {
                gameFrontSetDlgState(openWelcome);
                running = false;
            }
            imguiHandOnHover();
        }

        /* --- Error popup --- */
        static float s_fadeUdpErr = 0.0f;
        if (ImGui::BeginPopupModal(errPopupId, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                imguiPopupFadeAlpha(&s_fadeUdpErr));
            ImGui::Text("%s", errorMsg ? errorMsg : "");
            ImGui::Spacing();
            if (ImGui::Button(langGetText(STR_OK), ImVec2(80, 0))) {
                ImGui::CloseCurrentPopup();
            }
            imguiHandOnHover();
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        ImGui::End(); /* ##UdpSetup panel */
        ImGui::End(); /* ##UdpSetupBg host */

        dialogDrawNavOutline();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        /* Draw map background and semi-transparent overlay */
        if (hasBg) {
            bgGameRenderWithOverlay(bg, renderer, winW, winH);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        gameFrontPumpDirty(); /* sync cloud prefs from menus (login join + debounced upload) */
        dialogFrameCapEnd(frameCapStart);
    }

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
