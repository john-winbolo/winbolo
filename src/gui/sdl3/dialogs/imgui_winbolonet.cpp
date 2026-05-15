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
 * Purpose:       ImGui WinBolo.net login popup rendered
 *                inline within the settings dialog.
 *********************************************************/

#include <cstring>
#include <cmath>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_dialog_utils.h"

extern "C" {
#include "../../gamefront.h"
#include "global.h"
#include "../../../winbolonet/winbolonet.h"
#include "../../lang.h"
#include "imgui_winbolonet.h"
}

/* ---- async login state ---- */

enum WbnLoginState {
    WBN_IDLE,
    WBN_LOGGING_IN,
    WBN_VALIDATING,
    WBN_SUCCESS,
    WBN_ERROR
};

struct WbnLoginWork {
    /* inputs */
    char username[256];
    char password[256];
    char token[256];
    /* outputs */
    char tokenOut[256];
    char expiryOut[256];
    char playerNameOut[PLAYER_NAME_LEN];
    char errorMsg[512];
    bool isValidate;
    bool success;
    SDL_AtomicInt done;
};

static WbnLoginState wbnState = WBN_IDLE;
static WbnLoginWork wbnWork;
static SDL_Thread *wbnThread = nullptr;
static char wbnErrorBuf[512];
static bool wbnPopupOpen = false;
static char wbnUsername[256];
static char wbnPassword[256];
static bool wbnFocusUser = false;

static int wbnLoginThreadFunc(void *data) {
    WbnLoginWork *w = (WbnLoginWork *)data;
    if (w->isValidate) {
        w->success = winbolonetAuthValidate(w->token, w->playerNameOut, w->errorMsg);
    } else {
        w->success = winbolonetAuthLogin(w->username, w->password, w->tokenOut, w->expiryOut, w->playerNameOut, w->errorMsg);
    }
    SDL_SetAtomicInt(&w->done, 1);
    return 0;
}

static void wbnStartLogin(void) {
    memset(&wbnWork, 0, sizeof(wbnWork));
    SDL_SetAtomicInt(&wbnWork.done, 0);
    SDL_strlcpy(wbnWork.username, wbnUsername, sizeof(wbnWork.username));
    SDL_strlcpy(wbnWork.password, wbnPassword, sizeof(wbnWork.password));
    wbnWork.isValidate = false;
    wbnState = WBN_LOGGING_IN;
    wbnThread = SDL_CreateThread(wbnLoginThreadFunc, "WBNLogin", &wbnWork);
}

static void wbnStartValidate(const char *token) {
    memset(&wbnWork, 0, sizeof(wbnWork));
    SDL_SetAtomicInt(&wbnWork.done, 0);
    SDL_strlcpy(wbnWork.token, token, sizeof(wbnWork.token));
    wbnWork.isValidate = true;
    wbnState = WBN_VALIDATING;
    wbnThread = SDL_CreateThread(wbnLoginThreadFunc, "WBNValidate", &wbnWork);
}

static void wbnCheckThread(void) {
    if (!wbnThread || !SDL_GetAtomicInt(&wbnWork.done)) return;

    SDL_WaitThread(wbnThread, nullptr);
    wbnThread = nullptr;

    if (wbnWork.success) {
        if (!wbnWork.isValidate) {
            gameFrontSetWinbolonetToken(wbnWork.tokenOut, wbnWork.expiryOut);
        }
        if (wbnWork.playerNameOut[0] != '\0') {
            gameFrontSetPlayerName(wbnWork.playerNameOut);
        }
        wbnState = WBN_SUCCESS;
    } else {
        SDL_strlcpy(wbnErrorBuf, wbnWork.errorMsg, sizeof(wbnErrorBuf));
        if (!wbnWork.isValidate) {
            wbnState = WBN_ERROR;
        } else {
            gameFrontClearWinbolonetToken();
            wbnState = WBN_IDLE;
        }
    }
}

/* Spinner helper */
static void wbnDrawSpinner(const char *label) {
    float radius = ImGui::GetFontSize() * 0.5f;
    float t = (float)SDL_GetTicks() / 1000.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 centre(pos.x + radius, pos.y + radius);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);

    int segments = 12;
    for (int i = 0; i < segments; i++) {
        float a = t * 6.0f + (float)i * (2.0f * 3.14159f / (float)segments);
        float alpha = (float)(segments - i) / (float)segments;
        float x = centre.x + cosf(a) * radius;
        float y = centre.y + sinf(a) * radius;
        dl->AddCircleFilled(ImVec2(x, y), 2.0f, (col & 0x00FFFFFF) | ((ImU32)(alpha * 255.0f) << 24));
    }

    ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f));
    ImGui::SameLine();
    ImGui::Text("%s", label);
}

/* ---- Public API ---- */

extern "C" void imguiWinbolonetReset(void) {
    wbnState = WBN_IDLE;
    wbnPopupOpen = false;
    wbnUsername[0] = '\0';
    wbnPassword[0] = '\0';
    wbnErrorBuf[0] = '\0';
    wbnFocusUser = false;
    if (wbnThread) {
        SDL_WaitThread(wbnThread, nullptr);
        wbnThread = nullptr;
    }
}

extern "C" void imguiWinbolonetStartValidation(void) {
    char token[256];
    char expiry[256];
    gameFrontGetWinbolonetToken(token, expiry);
    if (token[0] != '\0') {
        wbnStartValidate(token);
    }
}

extern "C" void imguiWinbolonetDrawSection(bool inGame) {
    /* Check for async completion */
    wbnCheckThread();

    char token[256];
    char expiry[256];
    gameFrontGetWinbolonetToken(token, expiry);
    bool loggedIn = (token[0] != '\0');

    if (wbnState == WBN_VALIDATING) {
        wbnDrawSpinner(langGetText(STR_DLGWBN_CHECKING));
        return;
    }

    if (loggedIn) {
        ImGui::TextUnformatted(langGetText(STR_DLGWBN_LABEL));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", langGetText(STR_DLGWBN_SIGNED_IN));
        if (expiry[0] != '\0') {
            ImGui::SameLine();
            MessageArgs args = {};
            SDL_strlcpy(args.string1, expiry, sizeof(args.string1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_EXPIRES, &args));
            ImGui::PopStyleColor();
        }
        if (inGame) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_DLGWBN_SIGN_OUT))) {
            gameFrontClearWinbolonetToken();
        }
        imguiHandOnHover();
        if (inGame) ImGui::EndDisabled();
    } else {
        ImGui::TextUnformatted(langGetText(STR_DLGWBN_LABEL));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_NOT_SIGNED_IN));
        if (inGame) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_DLGWBN_SIGN_IN_BTN))) {
            wbnPopupOpen = true;
            wbnFocusUser = true;
            wbnState = WBN_IDLE;
            wbnErrorBuf[0] = '\0';
            wbnUsername[0] = '\0';
            wbnPassword[0] = '\0';
            ImGui::OpenPopup(langGetText(STR_DLGWBN_SIGNIN_TITLE));
        }
        imguiHandOnHover();
        if (inGame) ImGui::EndDisabled();
    }

    /* ---- Login popup ---- */
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal(langGetText(STR_DLGWBN_SIGNIN_TITLE), &wbnPopupOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        bool busy = (wbnState == WBN_LOGGING_IN);

        ImGui::TextWrapped("%s", langGetText(STR_DLGWBN_SIGNIN_BLURB));
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (busy) ImGui::BeginDisabled();

        float labelW = 90.0f;
        ImGui::TextUnformatted(langGetText(STR_DLGWBN_USERNAME));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-1);
        if (wbnFocusUser) {
            ImGui::SetKeyboardFocusHere();
            wbnFocusUser = false;
        }
        ImGui::InputText("##wbnuser", wbnUsername, sizeof(wbnUsername));

        ImGui::TextUnformatted(langGetText(STR_DLGWBN_PASSWORD));
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-1);
        bool enterPressed = ImGui::InputText("##wbnpass", wbnPassword, sizeof(wbnPassword),
                                              ImGuiInputTextFlags_Password |
                                              ImGuiInputTextFlags_EnterReturnsTrue);

        if (busy) ImGui::EndDisabled();

        ImGui::Spacing();

        /* Error message */
        if (wbnState == WBN_ERROR && wbnErrorBuf[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", wbnErrorBuf);
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        /* Success */
        if (wbnState == WBN_SUCCESS) {
            ImGui::CloseCurrentPopup();
            wbnPopupOpen = false;
            wbnState = WBN_IDLE;
        }

        ImGui::Separator();
        ImGui::Spacing();

        /* Buttons */
        if (busy) {
            wbnDrawSpinner(langGetText(STR_DLGWBN_SIGNINGIN));
        } else {
            float btnW = 80.0f;
            float totalW = btnW * 2 + 8.0f;
            float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - totalW) * 0.5f);

            if (ImGui::Button(langGetText(STR_DLGWBN_SIGNIN_OK), ImVec2(btnW, 0)) || enterPressed) {
                if (strlen(wbnUsername) == 0 || strlen(wbnPassword) == 0) {
                    SDL_strlcpy(wbnErrorBuf, langGetText(STR_DLGWBN_NEEDCREDS), sizeof(wbnErrorBuf));
                    wbnState = WBN_ERROR;
                } else {
                    wbnStartLogin();
                }
            }
            imguiHandOnHover();
            ImGui::SameLine(0.0f, 8.0f);
            if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, 0))) {
                ImGui::CloseCurrentPopup();
                wbnPopupOpen = false;
            }
            imguiHandOnHover();
        }

        ImGui::EndPopup();
    }
}
