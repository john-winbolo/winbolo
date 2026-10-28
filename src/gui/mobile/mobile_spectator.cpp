/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          Mobile Spectator
*Filename:      mobile_spectator.cpp
*Purpose:
*  spectatorRun for the mobile clients: watches a server's live
*  delayed feed, full screen, in the lobby's replay reel.
*
*  The desktop hosts the feed in the modal log viewer
*  (spectatorRun in src/logviewer/logviewer.c), with its
*  menus, panels, game view and controller input. iOS and Android build
*  only the viewer's embedded reel host, so this is the
*  smallest version of the same loop around it: pump the
*  spectator transport, wait for the seed with the same
*  countdown, connecting and connection-lost messages, load
*  it, then drain the records each frame and draw the
*  overview with touch pan and pinch zoom. The seed and
*  record handling is lv_spec_feed.c's, the same body
*  spectatorRun calls; the drawing and touch input are the
*  reel's (lobbyReelOpenSpectator / lobbyReelRenderSpectator).
*
*  Left out against the desktop: the followed-tank game
*  view, the DVR scrubber (the feed always follows the live
*  head), the event feed panel, the window title and the
*  controller pan and zoom.
*
*  Same contract as the desktop's: cs is the caller's
*  ClientSim, never disconnected here; returns true when the
*  server put the spectator back into its live lobby, false
*  when the player left or the feed was lost.
*********************************************************/

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "imgui.h"
#include "../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../sdl3/dialogs/imgui_dialog_utils.h"
#include "../sdl3/dialogs/lobby/lobby_reel.h"

extern "C" {
#include "../lang.h"
#include "spectator_drain.h"
}

#include "mobile_spectator.h"

/* Feed silence after which the screen says the connection is lost. A
 * spectator sends no input, so the transport's own timeout never fires; the
 * same 8 seconds the desktop host waits. */
#define MOBILE_SPEC_STALL_MS 8000u

/* The rect the picture gets: everything below the top row, inside the safe
 * area the host window was placed in. */
static ImVec2 mobileSpectatorViewRect(void) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f) avail.x = 1.0f;
    if (avail.y < 1.0f) avail.y = 1.0f;
    return avail;
}

/* The phase as a word for the top row, or NULL when the feed has not said. */
static const char *mobileSpectatorPhaseText(int phase) {
    switch (phase) {
    case SPEC_PHASE_LOBBY:
    case SPEC_PHASE_COUNTDOWN:
        return langGetText(STR_LV_SPEC_PHASE_LOBBY);
    case SPEC_PHASE_RUNNING:
        return langGetText(STR_LV_SPEC_PHASE_LIVE);
    case SPEC_PHASE_GAMEOVER:
        return langGetText(STR_LV_SPEC_PHASE_GAMEOVER);
    default:
        return NULL;
    }
}

extern "C" bool spectatorRun(struct SDL_Window *window,
                             struct SDL_Renderer *renderer, void *cs,
                             const char *serverHost, uint16_t serverPort) {
    (void)serverHost;
    (void)serverPort;
    if (window == NULL || renderer == NULL || cs == NULL) {
        return false;
    }

    bool liveResumed = false;

    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW = 0, screenH = 0;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

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

    /* Stall watchdog: progress is a changed countdown value while waiting, a
     * drained record once the feed runs. */
    Uint64 lastProgressMs = SDL_GetTicks();
    uint32_t lastCountdown = 0;
    bool haveCountdown = false;
    bool connectionLost = false;

    bool opened = false;   /* the reel came up */
    bool seeded = false;   /* the feed's seed is loaded */
    bool running = true;
    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();

        specDrainPump(cs);
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            lobbyReelPinchEvent(&ev);  /* two-finger zoom */
            if (dialogHandleQuitEvent(window, &ev)) {
                running = false;
            }
        }

        if (seeded) {
            /* The server put this spectator back on the live lobby control bus
             * (its delayed game drained back to the lobby): hand back to the
             * caller, which re-enters the live read-only lobby. */
            if (specDrainLiveResumed(cs)) {
                liveResumed = true;
                running = false;
            }
            SpecDrainRecord rec;
            bool drainedAny = false;
            while (running && specDrainPopRecord(cs, &rec)) {
                if (!lvEmbedSpecRecord(&rec)) {
                    /* A new segment failed to load: no decoder to draw from. */
                    running = false;
                }
                free(rec.payload);
                drainedAny = true;
            }
            specDrainPump(cs);
            if (drainedAny) {
                lastProgressMs = SDL_GetTicks();
                connectionLost = false;
            }
            lvEmbedSpecFrame();
        } else {
            uint32_t remaining = 0;
            if (specDrainCountdown(cs, &remaining) &&
                (!haveCountdown || remaining != lastCountdown)) {
                haveCountdown = true;
                lastCountdown = remaining;
                lastProgressMs = SDL_GetTicks();
            }
        }
        if (!connectionLost && SDL_GetTicks() - lastProgressMs > MOBILE_SPEC_STALL_MS) {
            connectionLost = true;
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
        ImGui::Begin("##MobileSpectator", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoSavedSettings);

        /* Top row: leave, the phase, and what the feed is doing. */
        if (ImGui::Button(langGetText(STR_CLOSE))) {
            running = false;
        }
        const char *status = NULL;
        char countdownText[64];
        if (connectionLost) {
            status = langGetText(STR_LV_SPEC_CONNECTION_LOST);
        } else if (!seeded) {
            if (haveCountdown) {
                /* ~50 ticks/sec; round up so the last second shows "1". */
                uint32_t secs = (lastCountdown + 49) / 50;
                if (secs == 0) {
                    status = langGetText(STR_LV_SPEC_BEGINS_NOW);
                } else {
                    MessageArgs args = {};
                    args.number = (int)secs;
                    snprintf(countdownText, sizeof(countdownText), "%s",
                             langGetTextFmt(STR_LV_SPEC_BEGINS_IN_FMT, &args));
                    status = countdownText;
                }
            } else {
                status = langGetText(STR_LV_SPEC_CONNECTING);
            }
        } else {
            status = mobileSpectatorPhaseText(lvEmbedSpecPhase());
        }
        if (status != NULL) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(status);
        }

        /* The reel comes up on the first frame, once the window says how much
         * room the picture has, and takes the seed when the feed has one. */
        if (running && !opened) {
            opened = true;
            if (!lobbyReelOpenSpectator(mobileSpectatorViewRect())) {
                running = false;
            }
        }
        if (running && !seeded && specDrainSeedReady(cs)) {
            uint8_t *seed = NULL;
            uint32_t seedLen = 0;
            if (specDrainTakeSeed(cs, &seed, &seedLen)) {
                seeded = lvEmbedSpecSeed(seed, seedLen);
                free(seed);
                if (!seeded) {
                    /* A seed that will not load leaves nothing to watch. */
                    running = false;
                } else {
                    /* A delivered seed means the feed is alive. */
                    connectionLost = false;
                    lastProgressMs = SDL_GetTicks();
                }
            }
        }
        if (running) {
            lobbyReelRenderSpectator(mobileSpectatorViewRect());
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    /* Ends the embed, which leaves live-DVR mode. The ClientSim is the
     * caller's and stays connected. */
    lobbyReelEnd();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prevCtx);

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);
    SDL_FlushEvent(SDL_EVENT_QUIT);

    return liveResumed;
}
