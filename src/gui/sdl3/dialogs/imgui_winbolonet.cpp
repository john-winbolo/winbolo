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
#include "dialog_footer.h"

extern "C" {
#include "../../gamefront.h"
#include "global.h"
#include "../../../winbolonet/winbolonet_client.h"
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
    int rankOut;
    int rankTotalOut;
    WbnStats statsOut;
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
static bool wbnShowStats = false;

static int wbnLoginThreadFunc(void *data) {
    WbnLoginWork *w = (WbnLoginWork *)data;
    if (w->isValidate) {
        w->success = winbolonetAuthValidate(w->token, w->playerNameOut, &w->rankOut, &w->rankTotalOut, &w->statsOut, w->errorMsg);
    } else {
        w->success = winbolonetAuthLogin(w->username, w->password, w->tokenOut, w->expiryOut, w->playerNameOut, &w->rankOut, &w->rankTotalOut, &w->statsOut, w->errorMsg);
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
        gameFrontSetWinbolonetRank(wbnWork.rankOut, wbnWork.rankTotalOut);
        gameFrontSetWinbolonetStats(&wbnWork.statsOut);
        if (wbnWork.playerNameOut[0] != '\0') {
            gameFrontSetPlayerName(wbnWork.playerNameOut);
        }
        /* Token is set (login) or confirmed (validate): pull the cloud prefs
         * once for this session. Runs after the player name is set so the
         * sync captures the account display_name to reassert over the synced
         * Player Name. Gated, so it fires at most once per sign-in. */
        gameFrontStartPrefsSync();
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
    float radius = ImGui::GetFontSize() * 0.6f;
    float phase = (float)SDL_GetTicks() * 0.002f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 centre(pos.x + radius, pos.y + radius);
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);

    /* A spinning WinBolo.net shield (the verified badge) stands in for a
     * generic spinner — narrower than tall, turning about its vertical axis. */
    imguiDrawSpinningShield(ImGui::GetWindowDrawList(), centre,
                            radius * 0.80f, radius, phase, col);

    ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f));
    ImGui::SameLine();
    ImGui::Text("%s", label);
}

/* Renders the 1v1 ladder line for a signed-in player: "#N of M" or
 * "Unranked" when the stored rank is < 0. Reads the rank gamefront
 * captured on the last auth/validate. */
static void wbnDrawRankLine(void) {
    int rank = -1, rankTotal = 0;
    gameFrontGetWinbolonetRank(&rank, &rankTotal);
    if (rank < 0) {
        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_UNRANKED));
    } else {
        MessageArgs args = {};
        args.number = rank;
        args.number2 = rankTotal;
        ImGui::TextDisabled("%s", langGetTextFmt(STR_DLGWBN_RANK, &args));
    }
}

/* Opens the shared sign-in popup, resetting its transient state. Both the
 * settings section and the welcome-screen status block call this. */
static void wbnOpenLoginPopup(void) {
    wbnPopupOpen = true;
    wbnFocusUser = true;
    wbnState = WBN_IDLE;
    wbnErrorBuf[0] = '\0';
    wbnUsername[0] = '\0';
    wbnPassword[0] = '\0';
    ImGui::OpenPopup(langGetText(STR_DLGWBN_SIGNIN_TITLE));
}

/* Clears the stored token and re-arms the once-per-session prefs-sync
 * gate so signing back in resyncs. Shared sign-out path. */
static void wbnSignOut(void) {
    gameFrontClearWinbolonetToken();
    gameFrontResetPrefsSyncSession();
}

/* Renders the shared sign-in popup modal. Must be called every frame by
 * whichever surface (settings section or welcome status block) owns the
 * popup this frame; OpenPopup scopes it to the current ImGui window. */
static void wbnRenderLoginPopup(void) {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);

    static float s_fadeWbnSignIn = 0.0f;
    if (ImGui::BeginPopupModal(langGetText(STR_DLGWBN_SIGNIN_TITLE), &wbnPopupOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            imguiPopupFadeAlpha(&s_fadeWbnSignIn));
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
            int footer = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                            langGetText(STR_DLGWBN_SIGNIN_OK),
                                            /*enterConfirms*/ true);
            if (footer == WBUI::FOOTER_CONFIRM || enterPressed) {
                if (strlen(wbnUsername) == 0 || strlen(wbnPassword) == 0) {
                    SDL_strlcpy(wbnErrorBuf, langGetText(STR_DLGWBN_NEEDCREDS), sizeof(wbnErrorBuf));
                    wbnState = WBN_ERROR;
                } else {
                    wbnStartLogin();
                }
            } else if (footer == WBUI::FOOTER_CANCEL) {
                ImGui::CloseCurrentPopup();
                wbnPopupOpen = false;
            }
        }

        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

/* Renders one "Label  value" row inside the stats dialog. A value < 0
 * means the field was absent from the response, so the row is skipped. */
static void wbnDrawStatRow(const char *label, int value) {
    if (value < 0) return;
    ImGui::TextUnformatted(label);
    ImGui::SameLine(170.0f);
    ImGui::Text("%d", value);
}

/* Renders the stat block for one game mode. The counter fields are shown
 * for every mode; the ranked fields (score/wins/losses/rank) are only
 * shown when `ranked` is true (tourn/strict), where a rank < 0 prints
 * "Unranked" rather than a position. */
static void wbnDrawModeStats(const char *title, const WbnModeStats *m, bool ranked) {
    ImGui::SeparatorText(title);
    wbnDrawStatRow(langGetText(STR_DLGWBN_STATS_GAMES), m->numGames);
    wbnDrawStatRow(langGetText(STR_DLGWBN_STATS_BASES), m->numBases);
    wbnDrawStatRow(langGetText(STR_DLGWBN_STATS_PILLS), m->numPills);
    wbnDrawStatRow(langGetText(STR_DLGWBN_STATS_TANKS), m->numTanks);
    if (ranked) {
        wbnDrawStatRow(langGetText(STR_DLGWBN_STATS_SCORE), m->score);
        wbnDrawStatRow(langGetText(STR_DLGWBN_STATS_WINS), m->wins);
        wbnDrawStatRow(langGetText(STR_DLGWBN_STATS_LOSES), m->loses);
        ImGui::TextUnformatted(langGetText(STR_DLGWBN_STATS_RANK));
        ImGui::SameLine(170.0f);
        if (m->rank < 0) {
            ImGui::TextUnformatted(langGetText(STR_DLGWBN_UNRANKED));
        } else {
            MessageArgs args = {};
            args.number = m->rank;
            args.number2 = m->rankTotal;
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_RANK, &args));
        }
    }
}

/* ---- Public API ---- */

/* Renders the shared "My Stats" modal from gamefront's stored per-mode
 * stats. Driven by wbnShowStats, set by the My Stats buttons in both the
 * welcome status block and the settings section. Both surfaces call this
 * each frame; one OpenPopup scopes it to the active ImGui window. */
extern "C" void imguiWinbolonetDrawStatsDialog(void) {
    char title[128];
    SDL_snprintf(title, sizeof(title), "%s###wbnstats",
                 langGetText(STR_DLGWBN_STATS_TITLE));
    if (wbnShowStats) {
        ImGui::OpenPopup(title);
        wbnShowStats = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Appearing);

    static float s_fadeWbnStats = 0.0f;
    bool open = true;
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, imguiPopupFadeAlpha(&s_fadeWbnStats));

    WbnStats st;
    gameFrontGetWinbolonetStats(&st);
    if (!st.valid) {
        ImGui::TextWrapped("%s", langGetText(STR_DLGWBN_STATS_NONE));
    } else {
        wbnDrawModeStats(langGetText(STR_DLGWBN_STATS_OPEN), &st.open, false);
        wbnDrawModeStats(langGetText(STR_DLGWBN_STATS_TOURN), &st.tourn, true);
        wbnDrawModeStats(langGetText(STR_DLGWBN_STATS_STRICT), &st.strict, true);
    }

    int f = WBUI::DialogFooter(/*cancelLabel*/ nullptr, langGetText(STR_OK));
    if (f != WBUI::FOOTER_NONE) ImGui::CloseCurrentPopup();

    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

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
        wbnDrawRankLine();
        if (ImGui::Button(langGetText(STR_DLGWBN_STATS_BTN))) {
            wbnShowStats = true;
        }
        imguiHandOnHover();
        ImGui::SameLine();
        if (inGame) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_DLGWBN_SIGN_OUT))) {
            wbnSignOut();
        }
        imguiHandOnHover();
        if (inGame) ImGui::EndDisabled();
    } else {
        ImGui::TextUnformatted(langGetText(STR_DLGWBN_LABEL));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_NOT_SIGNED_IN));
        if (inGame) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_DLGWBN_SIGN_IN_BTN))) {
            wbnOpenLoginPopup();
        }
        imguiHandOnHover();
        if (inGame) ImGui::EndDisabled();
    }

    wbnRenderLoginPopup();
    imguiWinbolonetDrawStatsDialog();
}

/* Compact account status block for the welcome screen: player name +
 * Signed in / Not signed in, a Login or Logout button, and the 1v1
 * ladder rank when signed in. Reuses the same popup/worker as the
 * settings section. Render inside an existing ImGui window. */
extern "C" void imguiWinbolonetDrawStatusBlock(void) {
    /* Check for async completion */
    wbnCheckThread();

    /* Cold-start: validate the stored token once per session the first
     * time the welcome screen draws, so rank/stats populate shortly after
     * launch instead of only on the first server join. Gated by a static
     * flag so it does not re-fire every frame; the validate is also a
     * no-op when no token is stored. */
    static bool s_welcomeValidated = false;
    if (!s_welcomeValidated) {
        s_welcomeValidated = true;
        if (wbnState == WBN_IDLE && !wbnThread) {
            imguiWinbolonetStartValidation();
        }
    }

    char token[256];
    char expiry[256];
    gameFrontGetWinbolonetToken(token, expiry);
    bool loggedIn = (token[0] != '\0');

    if (wbnState == WBN_VALIDATING) {
        wbnDrawSpinner(langGetText(STR_DLGWBN_CHECKING));
        wbnRenderLoginPopup();
        return;
    }

    if (loggedIn) {
        char playerName[PLAYER_NAME_LEN];
        playerName[0] = '\0';
        gameFrontGetPlayerName(playerName);
        if (playerName[0] != '\0') {
            ImGui::TextUnformatted(playerName);
            ImGui::SameLine();
        }
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", langGetText(STR_DLGWBN_SIGNED_IN));
        wbnDrawRankLine();
        if (ImGui::Button(langGetText(STR_DLGWBN_STATS_BTN))) {
            wbnShowStats = true;
        }
        imguiHandOnHover();
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_DLGWBN_SIGN_OUT))) {
            wbnSignOut();
        }
        imguiHandOnHover();
    } else {
        ImGui::TextUnformatted(langGetText(STR_DLGWBN_LABEL));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_NOT_SIGNED_IN));
        if (ImGui::Button(langGetText(STR_DLGWBN_SIGN_IN_BTN))) {
            wbnOpenLoginPopup();
        }
        imguiHandOnHover();
    }

    wbnRenderLoginPopup();
    imguiWinbolonetDrawStatsDialog();
}
