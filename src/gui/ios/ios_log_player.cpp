/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          iOS Log Player
*Filename:      ios_log_player.cpp
*Purpose:
*  logViewerRunFromMemory for the iOS client: plays a game
*  the WinBolo.net log browser downloaded, full screen, in
*  the lobby's replay reel.
*
*  The desktop opens such a game in the modal log viewer
*  (src/logviewer/logviewer.c), with its menus, panels and
*  spectator host. iOS builds only the viewer's embedded
*  reel host (lv_embed.c), so the game plays through the
*  same reel the lobby's recap shows - the picture with pan
*  and pinch zoom, play and pause, and the seek bar - and this
*  file is only the screen around it: its own ImGui frame,
*  a title row with Close, and the loop. The reel's drawing
*  and input are lobby_reel.cpp's, called through
*  lobbyReelOpenStandalone / lobbyReelRenderStandalone.
*
*  The browser on mobile always downloads to memory
*  (imgui_wbn_browser.cpp), so logViewerRun, the file path
*  entry point, stays a stub in ios_stubs.c.
*********************************************************/

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdlib.h>

#include "imgui.h"
#include "../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../sdl3/dialogs/imgui_dialog_utils.h"
#include "../sdl3/dialogs/lobby/lobby_reel.h"

extern "C" {
#include "../lang.h"
#include "../sdl3/dialogs/imgui_messagebox.h"
}

/* Declared by gamefront.c, which calls it, without a header of its own. */
extern "C" void logViewerRunFromMemory(struct SDL_Window *window,
                                       struct SDL_Renderer *renderer,
                                       uint8_t *zipData, size_t zipLen,
                                       bool fromMainMenu);

/* The rect the reel's picture gets inside the screen: everything below the
 * title row and above the transport row the reel draws under it, inside the
 * safe area. */
static ImVec2 iosLogPlayerViewRect(void) {
    const ImGuiStyle &style = ImGui::GetStyle();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float rowH = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
    ImVec2 rect(avail.x, avail.y - rowH);
    if (rect.x < 1.0f) rect.x = 1.0f;
    if (rect.y < 1.0f) rect.y = 1.0f;
    return rect;
}

extern "C" void logViewerRunFromMemory(struct SDL_Window *window,
                                       struct SDL_Renderer *renderer,
                                       uint8_t *zipData, size_t zipLen,
                                       bool fromMainMenu) {
    (void)fromMainMenu;
    if (window == NULL || renderer == NULL) {
        free(zipData);
        return;
    }

    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW = 0, screenH = 0;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

    /* Own context, as every dialog has: the caller's (the welcome screen's)
     * may be current, and the backends must not initialise onto its IO. */
    ImGuiContext *prevCtx = ImGui::GetCurrentContext();
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    bool opened = false;
    bool running = true;
    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            lobbyReelPinchEvent(&ev);  /* two-finger zoom */
            if (dialogHandleQuitEvent(window, &ev)) {
                running = false;
            }
        }

        int winW = 0, winH = 0;
        SDL_GetWindowSize(window, &winW, &winH);
        DialogSafeInsets insets = dialogGetSafeInsets(window);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(insets.left, insets.top));
        ImGui::SetNextWindowSize(ImVec2((float)winW - insets.left - insets.right,
                                        (float)winH - insets.top - insets.bottom));
        ImGui::Begin("##IosLogPlayer", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoSavedSettings);

        if (ImGui::Button(langGetText(STR_CLOSE))) {
            running = false;
        }

        /* Opened on the first frame, once the window says how much room the
         * picture has; the reel owns zipData from the call on. */
        if (!opened) {
            opened = true;
            if (!lobbyReelOpenStandalone(zipData, zipLen, iosLogPlayerViewRect())) {
                running = false;
            }
            zipData = NULL;
        }
        if (running) {
            lobbyReelRenderStandalone(iosLogPlayerViewRect());
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    const bool loadFailed = opened && !lvEmbedIsActive();
    lobbyReelEnd();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prevCtx);

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);
    SDL_FlushEvent(SDL_EVENT_QUIT);

    if (loadFailed) {
        imguiMessageBoxEx(langGetText(STR_DLGWBN_WINTITLE),
                          langGetText(STR_DLGLOBBY_REEL_NONE),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    }
}
