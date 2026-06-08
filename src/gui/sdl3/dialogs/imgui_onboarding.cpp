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
 *                as imgui_keysetup.cpp. Scaffold only: a
 *                three-step Account / Name / Keys shell
 *                with Back / Next / Skip chrome and a
 *                progress indicator; real step content and
 *                account logic land in later phases. All
 *                strings are literal English here —
 *                localization is deferred to a later phase.
 *********************************************************/

#include <SDL3/SDL.h>

#include <cstring>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "dialog_footer.h"

extern "C" {
#include "../sdl3draw.h"
#include "../bg_game.h"
#include "../../gamefront.h"   /* gameFrontSetOnboardingComplete */
#include "imgui_onboarding.h"
}

/* Linear wizard steps. No conditional gating in this phase — the
 * array is walked front to back; a later input-method step can slot
 * into the sequence here. */
enum OnboardStep {
    ONBOARD_STEP_ACCOUNT = 0,
    ONBOARD_STEP_NAME,
    ONBOARD_STEP_KEYS,
    ONBOARD_STEP_COUNT
};

static const char *onboardStepTitle(int step) {
    switch (step) {
    case ONBOARD_STEP_ACCOUNT: return "Account";
    case ONBOARD_STEP_NAME:    return "Player Name";
    case ONBOARD_STEP_KEYS:    return "Keys";
    default:                   return "";
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
    dialogSetWindowTitle(window, "Welcome to WinBolo Online");
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

    bool running = true;
    int result = 0;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
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
            const char *title = "Welcome to WinBolo Online";
            ImVec2 textSize = ImGui::CalcTextSize(title);
            ImGui::SetCursorPosX((panelW - textSize.x) * 0.5f);
            ImGui::Text("%s", title);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::Spacing();

        /* Progress indicator — one dot per step, current step filled. */
        {
            char dots[ONBOARD_STEP_COUNT * 4 + 1];
            dots[0] = '\0';
            for (int i = 0; i < ONBOARD_STEP_COUNT; ++i) {
                if (i > 0) strncat(dots, " ", sizeof(dots) - strlen(dots) - 1);
                strncat(dots, (i == currentStep) ? "\xE2\x97\x8F" /* ● */
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

        /* Placeholder step body — real content lands in a later phase. */
        ImGui::SetWindowFontScale(1.2f);
        ImGui::Text("%s", onboardStepTitle(currentStep));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::TextDisabled("(set up in a later step)");

        /* Button row pinned near the bottom of the panel. */
        const float buttonH = 30.0f * s;
        ImGui::SetCursorPosY(panelH - buttonH - 16.0f * s);
        ImGui::Separator();
        ImGui::Spacing();

        const ImVec2 buttonSize(110.0f * s, buttonH);
        bool isFirst = (currentStep == 0);
        bool isLast = (currentStep == ONBOARD_STEP_COUNT - 1);

        /* Back — disabled on the first step. */
        ImGui::BeginDisabled(isFirst);
        if (ImGui::Button("Back", buttonSize)) {
            if (currentStep > 0) currentStep--;
        }
        ImGui::EndDisabled();

        /* Skip — always present; finish via the skip path. */
        ImGui::SameLine();
        if (ImGui::Button("Skip", buttonSize)) {
            result = 1;
            gameFrontSetOnboardingComplete();
            running = false;
        }

        /* Next / Finish — advance, or finish on the last step. */
        ImGui::SameLine();
        if (ImGui::Button(isLast ? "Finish" : "Next", buttonSize)) {
            if (isLast) {
                result = 1;
                gameFrontSetOnboardingComplete();
                running = false;
            } else {
                currentStep++;
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
