/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_connecting.cpp
 * Purpose:       The iOS "Connecting to <host>..." screen.
 *                See imgui_connecting.h.
 *********************************************************/

#if defined(__IPHONEOS__) || defined(__ANDROID__)

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "imgui_nav_outline.h"
#include "../imgui_steam_nav.h"
#include "dialog_footer.h"
#include "imgui_connecting.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../lang.h"
}

extern "C" bool imguiConnectingRun(const char *host, ConnectingStepFn step,
                                   void *ctx) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer || !step) return false;

    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

    /* The context this screen owns, with the nav flags the standalone
     * dialog contract asks for. */
    IMGUI_CHECKVERSION();
    ImGuiContext *prevCtx = ImGui::GetCurrentContext();
    ImGuiContext *connCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(connCtx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    MessageArgs args;
    SDL_memset(&args, 0, sizeof(args));
    SDL_strlcpy(args.string1, host ? host : "", sizeof(args.string1));
    char title[256];
    SDL_strlcpy(title, langGetTextFmt(STR_DLGCONNECTING_TITLE, &args), sizeof(title));

    bool finished = false;
    bool running = true;
    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();

        /* Pumping events each frame is also what keeps iOS's lifecycle
         * callbacks serviced while the join waits. */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleQuitEvent(window, &ev)) {
                running = false;
            }
        }
        if (!running) break;

        const char *stage = "";
        float progress = -1.0f;
        if (step(ctx, &stage, &progress) == CONNECTING_STEP_DONE) {
            finished = true;
            break;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        float panelW = 420.0f * s;
        if (panelW > (float)winW * 0.9f) panelW = (float)winW * 0.9f;
        ImGui::SetNextWindowPos(ImVec2((float)winW * 0.5f, (float)winH * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 0.0f), ImVec2(panelW, (float)winH * 0.9f));
        ImGui::Begin("##Connecting", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::TextWrapped("%s", title);
        ImGui::Spacing();
        ImGui::TextDisabled("%s", stage ? stage : "");
        if (progress >= 0.0f) {
            ImGui::Spacing();
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const char *cancelLabel = langGetText(STR_CANCEL);
        float btnW = ImGui::CalcTextSize(cancelLabel).x +
                     ImGui::GetStyle().FramePadding.x * 2.0f;
        if (btnW < 100.0f * s) btnW = 100.0f * s;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - btnW);
        WBUI::PushCancelStyle();
        bool cancelClicked = ImGui::Button(cancelLabel, ImVec2(btnW, 0.0f));
        WBUI::PopCancelStyle();
        if (cancelClicked || WBUI::CancelKeyPressed()) {
            running = false;
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

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(connCtx);
    ImGui::SetCurrentContext(prevCtx);

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);
    SDL_FlushEvent(SDL_EVENT_QUIT);
    return finished;
}

#endif /* __IPHONEOS__ || __ANDROID__ */
