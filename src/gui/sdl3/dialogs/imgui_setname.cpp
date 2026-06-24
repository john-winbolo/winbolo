/*
 * Copyright (c) 1998-2026 John Morrison.
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
#include "imgui_nav_outline.h"
#include "imgui_controller_prompt.h"
#include "../imgui_steam_nav.h"
#include "dialog_footer.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../gamefront.h"
#include "global.h"
#include "client_sim.h"
#include "util.h"
#include "playername_validate.h"
#include "../../lang.h"
#include "imgui_setname.h"
#include "imgui_keyboard.h"
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
    dialogSetWindowTitle(window, langGetText(STR_DLGSETNAME_WINTITLE));
    SDL_SetWindowResizable(window, false);
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    /* Set up ImGui context */
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

    /* Load current player name */
    char playerName[PLAYER_NAME_LEN];
    playerName[0] = '\0';
    if (inGame) {
        clientSimGetPlayerName(cs, playerName);
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
        controllerDialogsRenderMenu();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::Begin("##SetName", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        /* Top-right close X — same as Cancel. */
        if (WBUI::DrawPanelCloseX()) {
            running = false;
        }

        bool wbnLocked = gameFrontGetWinbolonetUse();

        if (wbnLocked) {
            ImGui::TextUnformatted(langGetText(STR_DLGSETNAME_WBN_LOCKED));
        } else {
            ImGui::TextUnformatted(langGetText(STR_DLGSETNAME_PLEASE_ENTER));
        }
        ImGui::Spacing();

        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_PLAYERNAME));
        ImGui::SameLine(120.0f * s);
        ImGui::SetNextItemWidth((float)winW - 120.0f * s - 16.0f);
        if (wbnLocked) ImGui::BeginDisabled();
        if (focusInput && !wbnLocked) {
            ImGui::SetKeyboardFocusHere();
            focusInput = false;
        }
        bool enterPressed = ImGui::InputText("##name", playerName, PLAYER_NAME_LEN,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (wbnLocked) ImGui::EndDisabled();

        char errPopupId[64];
        SDL_snprintf(errPopupId, sizeof(errPopupId), "%s##setname", langGetText(STR_ERR_TITLE));

        int footer = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                        langGetText(STR_OK),
                                        /*enterConfirms*/ true,
                                        /*showSeparator*/ true,
                                        /*confirmDisabled*/ wbnLocked);
        if (footer == WBUI::FOOTER_CONFIRM || enterPressed) {
            char newName[PLAYER_NAME_LEN];
            PlayerNameValidationError nameErr = PLAYER_NAME_OK;
            bool nameOk = playerNameValidate(playerName, newName,
                                             PLAYER_NAME_LEN, &nameErr);

            if (!nameOk) {
                switch (nameErr) {
                    case PLAYER_NAME_ERR_EMPTY:
                        errorMsg = langGetText(STR_DLGSETNAME_BLANK_ERR);
                        break;
                    case PLAYER_NAME_ERR_RESERVED_PREFIX:
                        errorMsg = langGetText(STR_DLGSETNAME_STAR_ERR);
                        break;
                    case PLAYER_NAME_ERR_RESERVED_SUFFIX:
                        errorMsg = langGetText(STR_NAME_INVALID_RESERVED_SUFFIX);
                        break;
                    case PLAYER_NAME_ERR_MIXED_SCRIPTS:
                        errorMsg = langGetText(STR_NAME_INVALID_MIXED_SCRIPTS);
                        break;
                    case PLAYER_NAME_ERR_INVALID_UTF8:
                    case PLAYER_NAME_ERR_DISALLOWED_CHAR:
                    case PLAYER_NAME_ERR_TOO_LONG:
                    default:
                        errorMsg = langGetText(STR_NAME_INVALID_CHARS);
                        break;
                }
                ImGui::OpenPopup(errPopupId);
            } else if (inGame) {
                char oldName[PLAYER_NAME_LEN];
                oldName[0] = '\0';
                clientSimGetPlayerName(cs, oldName);
                if (playerNameCompare(oldName, newName) == 0) {
                    running = false;
                } else {
                    bool changeOK = clientSimSetPlayerName(cs, newName);
                    if (!changeOK) {
                        errorMsg = langGetText(STR_DLGSETNAME_INUSE_ERR);
                        ImGui::OpenPopup(errPopupId);
                    } else {
                        running = false;
                    }
                }
            } else {
                gameFrontSetPlayerName(newName);
                running = false;
            }
        }
        if (footer == WBUI::FOOTER_CANCEL) {
            running = false;
        }

        /* Error popup */
        static float s_fadeSetNameErr = 0.0f;
        if (ImGui::BeginPopupModal(errPopupId, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                imguiPopupFadeAlpha(&s_fadeSetNameErr));
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
        keyboardUpdate();   /* controller text entry for this dialog's fields */
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        gameFrontPumpDirty(); /* sync cloud prefs from menus (login join + debounced upload) */
        dialogFrameCapEnd(frameCapStart);
    }

    /* Dismiss soft keyboard before tearing down ImGui */
    dialogDismissKeyboard(window);

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
