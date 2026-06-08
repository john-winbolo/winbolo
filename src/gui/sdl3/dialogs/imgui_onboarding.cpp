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
 * Name:          imgui_onboarding.cpp
 * Purpose:       Standalone blocking first-run online
 *                onboarding wizard. Runs its own ImGui
 *                context and SDL event loop, same pattern
 *                as imgui_keysetup.cpp. Three steps —
 *                Account (WinBolo.net sign-in), Player Name
 *                (validated), and Keys (inline rebind) — with
 *                Back / Next / Skip chrome and a progress
 *                indicator. The Name step is skipped when the
 *                player is already signed in, since the
 *                account display name takes precedence. All
 *                user-facing strings route through langGetText.
 *********************************************************/

#include <SDL3/SDL.h>

#include <cstring>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "dialog_footer.h"
#include "imgui_winbolonet.h"   /* imguiWinbolonetDrawSection — Account step */
#include "imgui_keysetup.h"     /* embedded key-rebind form — Keys step */

extern "C" {
#include "../sdl3draw.h"
#include "global.h"             /* PLAYER_NAME_LEN */
#include "../bg_game.h"
#include "../../lang.h"
#include "../../gamefront.h"   /* gameFrontSetOnboardingComplete, name + token */
#include "playername_validate.h" /* playerNameValidate — Name step gate */
#include "imgui_onboarding.h"
}

/* Wizard steps. Account → Name → Keys, but the Name step is shown
 * only when the player is not signed in: when signed in the account
 * display name wins, so navigation skips Name in both directions and
 * the progress indicator omits its dot. */
enum OnboardStep {
    ONBOARD_STEP_ACCOUNT = 0,
    ONBOARD_STEP_NAME,
    ONBOARD_STEP_KEYS,
    ONBOARD_STEP_COUNT
};

static const char *onboardStepTitle(int step) {
    switch (step) {
    case ONBOARD_STEP_ACCOUNT: return langGetText(STR_DLGONBOARD_ACCOUNT_TITLE);
    case ONBOARD_STEP_NAME:    return langGetText(STR_DLGONBOARD_NAME_TITLE);
    case ONBOARD_STEP_KEYS:    return langGetText(STR_DLGONBOARD_KEYS_TITLE);
    default:                   return "";
    }
}

static const char *onboardStepDesc(int step) {
    switch (step) {
    case ONBOARD_STEP_ACCOUNT: return langGetText(STR_DLGONBOARD_ACCOUNT_DESC);
    case ONBOARD_STEP_NAME:    return langGetText(STR_DLGONBOARD_NAME_DESC);
    case ONBOARD_STEP_KEYS:    return langGetText(STR_DLGONBOARD_KEYS_DESC);
    default:                   return "";
    }
}

/* The step reached by stepping forward / back from `step`, honouring
 * the signed-in skip of the Name step. Returns -1 when there is no
 * step in that direction (first / last of the active sequence). */
static int onboardNextStep(int step, bool loggedIn) {
    switch (step) {
    case ONBOARD_STEP_ACCOUNT:
        return loggedIn ? ONBOARD_STEP_KEYS : ONBOARD_STEP_NAME;
    case ONBOARD_STEP_NAME:
        return ONBOARD_STEP_KEYS;
    default:
        return -1;
    }
}

static int onboardPrevStep(int step, bool loggedIn) {
    switch (step) {
    case ONBOARD_STEP_KEYS:
        return loggedIn ? ONBOARD_STEP_ACCOUNT : ONBOARD_STEP_NAME;
    case ONBOARD_STEP_NAME:
        return ONBOARD_STEP_ACCOUNT;
    default:
        return -1;
    }
}

/* Map a player-name validation failure to a localized message, mirroring
 * the set-name dialog's error vocabulary. */
static const char *onboardNameErrorText(PlayerNameValidationError err) {
    switch (err) {
    case PLAYER_NAME_ERR_EMPTY:           return langGetText(STR_DLGSETNAME_BLANK_ERR);
    case PLAYER_NAME_ERR_RESERVED_PREFIX: return langGetText(STR_DLGSETNAME_STAR_ERR);
    case PLAYER_NAME_ERR_RESERVED_SUFFIX: return langGetText(STR_NAME_INVALID_RESERVED_SUFFIX);
    case PLAYER_NAME_ERR_MIXED_SCRIPTS:   return langGetText(STR_NAME_INVALID_MIXED_SCRIPTS);
    case PLAYER_NAME_ERR_INVALID_UTF8:
    case PLAYER_NAME_ERR_DISALLOWED_CHAR:
    case PLAYER_NAME_ERR_TOO_LONG:
    default:                              return langGetText(STR_NAME_INVALID_CHARS);
    }
}

extern "C" int imguiOnboardingShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return 0;

    /* Save logical presentation */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

#if !BOLO_MOBILE
    dialogSetWindowSize(window, 1024, 768);
    dialogSetWindowTitle(window, langGetText(STR_DLGONBOARD_TITLE));
    SDL_SetWindowResizable(window, true);
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiRegisterPlatformOpenUrl();
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

    int currentStep = ONBOARD_STEP_ACCOUNT;

    /* Per-step working state. */
    char nameBuf[PLAYER_NAME_LEN] = {};
    bool nameSeeded = false;          /* seed nameBuf from prefs once */
    const char *nameError = nullptr;  /* inline validation message */
    bool keysBegun = false;           /* seed embedded key form once */

    bool running = true;
    int result = 0;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* While a key row is capturing, route the raw scancode into the
             * embedded key form instead of letting ImGui consume it. */
            if (imguiKeySetupIsCapturingInGameKey() &&
                ev.type == SDL_EVENT_KEY_DOWN &&
                ev.key.windowID == SDL_GetWindowID(window)) {
                imguiKeySetupHandleInGameScancode((int)ev.key.scancode);
                continue;
            }
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 ev.window.windowID == SDL_GetWindowID(window))) {
                /* Quit / close: bail to welcome, don't mark complete. */
                result = 0;
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
        ImGui::Begin("##OnboardingBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Centered overlay panel */
        float panelW = 460.0f * s, panelH = 560.0f * s;
        if (panelW > (float)winW * 0.95f) panelW = (float)winW * 0.95f;
        if (panelH > (float)winH * 0.95f) panelH = (float)winH * 0.95f;

        ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##OnboardingPanel", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        /* Top-right close X — same effect as a window-close: bail to
         * welcome without marking onboarding complete. */
        if (WBUI::DrawPanelCloseX()) {
            result = 0;
            running = false;
        }

        /* Title */
        {
            ImGui::SetWindowFontScale(1.4f);
            const char *title = langGetText(STR_DLGONBOARD_TITLE);
            ImVec2 textSize = ImGui::CalcTextSize(title);
            ImGui::SetCursorPosX((panelW - textSize.x) * 0.5f);
            ImGui::Text("%s", title);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::Spacing();

        /* Signed-in state drives which steps are active. Recomputed each
         * frame so signing in/out on the Account step immediately reshapes
         * both navigation and the progress indicator. */
        char token[256], expiry[256];
        gameFrontGetWinbolonetToken(token, expiry);
        bool loggedIn = (token[0] != '\0');

        /* Active step sequence — the Name step drops out when signed in. */
        int seq[ONBOARD_STEP_COUNT];
        int seqLen = 0;
        seq[seqLen++] = ONBOARD_STEP_ACCOUNT;
        if (!loggedIn) seq[seqLen++] = ONBOARD_STEP_NAME;
        seq[seqLen++] = ONBOARD_STEP_KEYS;

        /* Progress indicator — one dot per active step, current step filled. */
        {
            char dots[ONBOARD_STEP_COUNT * 4 + 1];
            dots[0] = '\0';
            for (int i = 0; i < seqLen; ++i) {
                if (i > 0) strncat(dots, " ", sizeof(dots) - strlen(dots) - 1);
                strncat(dots, (seq[i] == currentStep) ? "\xE2\x97\x8F" /* ● */
                                                      : "\xE2\x97\x8B" /* ○ */,
                        sizeof(dots) - strlen(dots) - 1);
            }
            ImVec2 dotsSize = ImGui::CalcTextSize(dots);
            ImGui::SetCursorPosX((panelW - dotsSize.x) * 0.5f);
            ImGui::Text("%s", dots);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Step heading + description. */
        ImGui::SetWindowFontScale(1.2f);
        ImGui::Text("%s", onboardStepTitle(currentStep));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", onboardStepDesc(currentStep));
        ImGui::Spacing();

        /* Step body. */
        switch (currentStep) {
        case ONBOARD_STEP_ACCOUNT:
            /* WBN sign-in widget — draws the Sign In button / signed-in
             * status and runs its own login popup. Sign In only; the
             * browser create-account path lands in a later phase. */
            imguiWinbolonetDrawSection(false);
            break;
        case ONBOARD_STEP_NAME:
            if (!nameSeeded) {
                gameFrontGetPlayerName(nameBuf);
                nameSeeded = true;
            }
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##onboardname", nameBuf, sizeof(nameBuf));
            if (nameError) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", nameError);
            }
            break;
        case ONBOARD_STEP_KEYS:
            if (!keysBegun) {
                imguiKeySetupBeginEmbedded();
                keysBegun = true;
            }
            imguiKeySetupRenderEmbedded();
            break;
        default:
            break;
        }

        /* Button row pinned near the bottom of the panel. */
        const float buttonH = 30.0f * s;
        ImGui::SetCursorPosY(panelH - buttonH - 16.0f * s);
        ImGui::Separator();
        ImGui::Spacing();

        const ImVec2 buttonSize(110.0f * s, buttonH);
        int prevStep = onboardPrevStep(currentStep, loggedIn);
        int nextStep = onboardNextStep(currentStep, loggedIn);
        bool isLast = (nextStep < 0);

        /* Back — disabled at the start of the active sequence. */
        ImGui::BeginDisabled(prevStep < 0);
        if (ImGui::Button(langGetText(STR_BACK), buttonSize)) {
            if (prevStep >= 0) {
                nameError = nullptr;
                currentStep = prevStep;
            }
        }
        ImGui::EndDisabled();

        /* Skip — abandon-and-proceed: mark complete and exit without
         * committing the current step's pending edits. */
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_DLGONBOARD_SKIP), buttonSize)) {
            result = 1;
            gameFrontSetOnboardingComplete();
            running = false;
        }

        /* Next / Finish — commit the step being left, then advance or finish.
         * A failed name validation blocks the advance and shows the error. */
        ImGui::SameLine();
        if (ImGui::Button(langGetText(isLast ? STR_DLGONBOARD_FINISH
                                             : STR_DLGONBOARD_NEXT), buttonSize)) {
            bool advance = true;
            if (currentStep == ONBOARD_STEP_NAME) {
                char validated[PLAYER_NAME_LEN];
                PlayerNameValidationError err = PLAYER_NAME_OK;
                if (playerNameValidate(nameBuf, validated, sizeof(validated), &err)) {
                    gameFrontSetPlayerName(validated);
                    nameError = nullptr;
                } else {
                    nameError = onboardNameErrorText(err);
                    advance = false;
                }
            } else if (currentStep == ONBOARD_STEP_KEYS) {
                imguiKeySetupCommitEmbedded();
            }
            if (advance) {
                if (isLast) {
                    result = 1;
                    gameFrontSetOnboardingComplete();
                    running = false;
                } else {
                    currentStep = nextStep;
                }
            }
        }

        ImGui::End(); /* ##OnboardingPanel */
        ImGui::End(); /* ##OnboardingBg */

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

    return result;
}
