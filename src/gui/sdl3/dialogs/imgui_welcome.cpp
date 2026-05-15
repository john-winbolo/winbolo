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
 * Name:          imgui_welcome.cpp
 * Purpose:       ImGui Welcome/Opening dialog.
 *                Unified for desktop, Android and Steam Deck.
 *                Scales UI based on screen size at runtime.
 *********************************************************/

#include <SDL3/SDL.h>

#include "stb_image.h"
#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"

extern "C" {
#include "../sdl3draw.h"
#include "../bg_game.h"
#include "imgui_welcome.h"
#include "../../gamefront.h"
#include "../../lang.h"
}

/* Match openingStates enum from gamefront.h */
enum {
    RESULT_SINGLEPLAYER = 5,   /* openSetup */
    RESULT_TUTORIAL     = 4,   /* openTutorial */
    RESULT_INTERNET     = 14,  /* openInternet */
    RESULT_LAN          = 10,  /* openLan */
    RESULT_SETTINGS     = 19,  /* openSettings */
    RESULT_MAPEDITOR    = 20,  /* openMapEditor */
    RESULT_LOGVIEWER    = 21,  /* openLogViewer */
    RESULT_QUIT         = -1
};

/* Load a PNG with alpha via SDL_IOFromFile + stb_image.
   SDL_IOFromFile works with Android APK assets, unlike plain fopen. */
static SDL_Texture *loadPng(SDL_Renderer *renderer, const char *filename) {
    SDL_IOStream *io = SDL_IOFromFile(filename, "rb");
    if (!io) {
        char path[512];
        SDL_snprintf(path, sizeof(path), "data/%s", filename);
        io = SDL_IOFromFile(path, "rb");
    }
    if (!io) return nullptr;

    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return nullptr; }
    unsigned char *buf = (unsigned char *)SDL_malloc((size_t)size);
    if (!buf) { SDL_CloseIO(io); return nullptr; }
    SDL_ReadIO(io, buf, (size_t)size);
    SDL_CloseIO(io);

    int w, h, channels;
    unsigned char *data = stbi_load_from_memory(buf, (int)size, &w, &h, &channels, 4);
    SDL_free(buf);
    if (!data) return nullptr;

    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, data, w * 4);
    if (!surf) { stbi_image_free(data); return nullptr; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_DestroySurface(surf);
    stbi_image_free(data);
    return tex;
}

/* Composite the shield logo onto `target` with an additive "sun glint" band
 * traversing left-to-right at progress `t` in [0,1]. The band is built from
 * 16 vertical strips of `logo` re-drawn with SDL_BLENDMODE_ADD and Gaussian-
 * weighted alpha — re-drawing the logo (rather than a plain quad) makes the
 * additive contribution automatically masked by the logo's own alpha, so
 * the glint only brightens pixels inside the shield silhouette. */
static void composeShimmerFrame(SDL_Renderer *r, SDL_Texture *target,
                                 SDL_Texture *logo, int w, int h, float t) {
    SDL_Texture *prevTarget = SDL_GetRenderTarget(r);

    SDL_BlendMode prevTexBlend;
    SDL_GetTextureBlendMode(logo, &prevTexBlend);
    Uint8 prevR = 255, prevG = 255, prevB = 255;
    SDL_GetTextureColorMod(logo, &prevR, &prevG, &prevB);
    Uint8 prevA = 255;
    SDL_GetTextureAlphaMod(logo, &prevA);

    SDL_SetRenderTarget(r, target);
    SDL_SetRenderClipRect(r, NULL);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);

    /* Pass 1: opaque logo */
    SDL_SetTextureBlendMode(logo, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(logo, 255, 255, 255);
    SDL_SetTextureAlphaMod(logo, 255);
    SDL_RenderTexture(r, logo, NULL, NULL);

    /* Pass 2: additive glint band */
    const int NSTRIPS = 16;
    const float bandHalfW = (float)w * 0.25f;
    const float stripW    = (bandHalfW * 2.0f) / (float)NSTRIPS;
    const float sigma     = bandHalfW * 0.5f;
    const float centerX   = -bandHalfW + t * ((float)w + 2.0f * bandHalfW);

    SDL_SetTextureBlendMode(logo, SDL_BLENDMODE_ADD);
    for (int i = 0; i < NSTRIPS; i++) {
        float offset = ((float)i - (float)(NSTRIPS - 1) * 0.5f) * stripW;
        float ratio  = offset / sigma;
        float gauss  = SDL_expf(-ratio * ratio);
        Uint8 alpha  = (Uint8)(gauss * 200.0f);
        if (alpha == 0) continue;

        SDL_Rect clip;
        clip.x = (int)(centerX + offset - stripW * 0.5f);
        clip.y = 0;
        clip.w = (int)stripW + 1;
        clip.h = h;
        if (clip.x + clip.w <= 0 || clip.x >= w) continue;

        SDL_SetRenderClipRect(r, &clip);
        SDL_SetTextureAlphaMod(logo, alpha);
        SDL_RenderTexture(r, logo, NULL, NULL);
    }

    /* Restore */
    SDL_SetTextureAlphaMod(logo, prevA);
    SDL_SetTextureColorMod(logo, prevR, prevG, prevB);
    SDL_SetTextureBlendMode(logo, prevTexBlend);
    SDL_SetRenderTarget(r, prevTarget);
}

extern "C" int imguiWelcomeShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return RESULT_QUIT;

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
    /* On desktop, resize window to a reasonable dialog size */
    int dlgW = (int)(1024 * (s > 1.0f ? 1.0f : 1.0f)); /* keep 1024x768 on desktop */
    int dlgH = 768;
    dialogSetWindowSize(window, dlgW, dlgH);
    dialogSetWindowTitle(window, langGetText(STR_DLGWELCOME_WINTITLE));
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
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    /* Apply font and touch scaling */
    dialogApplyScaling(s);

    /* Load images */
    SDL_Texture *logoTex = loadPng(renderer, "smalllogo-transparent.png");

    /* Query logo dimensions. nativeW/H are the texture's pixel size and are
     * used for the shimmer offscreen target so the SDL render-target pass
     * is a 1:1 copy of the source — without that, the shimmer texture would
     * downscale 265→200 in SDL and then ImGui downscales again 200→198.75,
     * softening the text on the shield and making the logo look subtly
     * smaller mid-sweep than when displayed directly. */
    float nativeW = 0.0f, nativeH = 0.0f;
    float logoW = 0.0f, logoH = 0.0f;
    if (logoTex) {
        SDL_GetTextureSize(logoTex, &nativeW, &nativeH);
        logoW = nativeW * 0.75f;
        logoH = nativeH * 0.75f;
        if (s > 1.05f) { logoW *= s; logoH *= s; }
    }

    /* Use shared background game */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);

    int result = RESULT_QUIT;
    bool running = true;
    Uint64 lastTickTime = SDL_GetTicks();

    /* Show "In main menu" in the Steam friends list while the welcome
     * dialog is up. Cleared / overwritten when the user enters a lobby
     * or starts a game. */
    gameFrontSetSteamPresenceMenu();

    /* Logo shimmer: a 1.5s "sun glint" sweep, fired on hover entry and on
     * a 75–105s randomised idle timer. Offscreen target is cached for the
     * dialog lifetime and recreated on logo size change (e.g. DPI/resize). */
    const Uint64 SHIMMER_DURATION_MS = 1500;
    const Uint64 SHIMMER_AUTO_MIN_MS = 75000;
    const Uint64 SHIMMER_AUTO_MAX_MS = 105000;
    Uint64 shimmerStartMs    = 0;  /* 0 = idle */
    Uint64 shimmerNextAutoMs = 0;  /* 0 = needs init */
    SDL_Texture *shimmerTarget = nullptr;
    int shimmerTargetW = 0;
    int shimmerTargetH = 0;
    bool wasLogoHovered = false;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (dialogHandleUrlDropEvent(&ev)) { result = 16; running = false; continue; } /* openInternetManual */
            if (ev.type == SDL_EVENT_QUIT) {
                result = RESULT_QUIT;
                running = false;
            }
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(window)) {
                result = RESULT_QUIT;
                running = false;
            }
        }

        /* Tick the background game at fixed rate (unless paused) */
        if (hasBg && !bg->paused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bg->paused) {
            lastTickTime = SDL_GetTicks();
        }

        /* Query actual window size each frame (handles resize) */
        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        /* --- Shimmer state update + offscreen composition --- */
        Uint64 nowMs = SDL_GetTicks();
        bool sweeping = (shimmerStartMs != 0) &&
                        (nowMs - shimmerStartMs < SHIMMER_DURATION_MS);
        if (shimmerStartMs != 0 && !sweeping) shimmerStartMs = 0;

        if (shimmerNextAutoMs == 0) {
            Uint32 range = (Uint32)(SHIMMER_AUTO_MAX_MS - SHIMMER_AUTO_MIN_MS);
            shimmerNextAutoMs = nowMs + SHIMMER_AUTO_MIN_MS +
                                (Uint64)SDL_rand((Sint32)range);
        }
        if (!sweeping && nowMs >= shimmerNextAutoMs) {
            shimmerStartMs = nowMs;
            sweeping = true;
            Uint32 range = (Uint32)(SHIMMER_AUTO_MAX_MS - SHIMMER_AUTO_MIN_MS);
            shimmerNextAutoMs = nowMs + SHIMMER_AUTO_MIN_MS +
                                (Uint64)SDL_rand((Sint32)range);
        }

        ImTextureID logoImageTexID = (ImTextureID)logoTex;
        if (sweeping && logoTex && logoW > 0.0f && logoH > 0.0f) {
            int targetW = (int)nativeW;
            int targetH = (int)nativeH;
            if (!shimmerTarget ||
                shimmerTargetW != targetW || shimmerTargetH != targetH) {
                if (shimmerTarget) SDL_DestroyTexture(shimmerTarget);
                shimmerTarget = SDL_CreateTexture(renderer,
                                                  SDL_PIXELFORMAT_RGBA32,
                                                  SDL_TEXTUREACCESS_TARGET,
                                                  targetW, targetH);
                if (shimmerTarget) {
                    SDL_SetTextureBlendMode(shimmerTarget, SDL_BLENDMODE_BLEND);
                    shimmerTargetW = targetW;
                    shimmerTargetH = targetH;
                }
            }
            if (shimmerTarget) {
                float t = (float)(nowMs - shimmerStartMs) /
                          (float)SHIMMER_DURATION_MS;
                composeShimmerFrame(renderer, shimmerTarget, logoTex,
                                    shimmerTargetW, shimmerTargetH, t);
                logoImageTexID = (ImTextureID)shimmerTarget;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        /* Transparent full-screen host window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##WelcomeBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Top-right logo + ghost menu column. Logo is purely decorative. */
        {
            const float restoreMargin = 12.0f * s;
            const float miniBtnW      = 180.0f * s;

            if (logoTex && logoW > 0.0f) {
                /* Center the logo horizontally over the button column. The
                 * logo can be wider than miniBtnW, in which case the offset
                 * goes negative and the logo extends past the column on both
                 * sides equally — that's the intended look. Only clamp if
                 * the centred position would extend past the window's right
                 * edge entirely. */
                float restoreX = (float)winW - miniBtnW - restoreMargin
                                 + (miniBtnW - logoW) * 0.5f;
                if (restoreX + logoW > (float)winW)
                    restoreX = (float)winW - logoW;
                float restoreY = restoreMargin;
                ImGui::SetCursorPos(ImVec2(restoreX, restoreY));
                ImGui::Image(logoImageTexID, ImVec2(logoW, logoH));

                /* Hover entry triggers a sweep — edge-detected so a slow
                 * drag across the logo doesn't fire repeatedly, and a sweep
                 * already in progress is left alone. */
                bool isHovered = ImGui::IsItemHovered();
                if (!sweeping && isHovered && !wasLogoHovered) {
                    shimmerStartMs = nowMs;
                }
                wasLogoHovered = isHovered;
            }

            /* Transparent menu buttons below the logo */
            const float miniBtnH = 30.0f * s;
            const float miniBtnGap = 4.0f * s;
#if BOLO_MOBILE
            const float ghostBtnAlpha = 0.6f;
            const float ghostTextAlpha = 0.9f;
#else
            /* Without the background game, buttons need higher base alpha
             * to stay readable against the solid dark clear color. */
            const float ghostBtnAlpha = hasBg ? 0.15f : 0.6f;
            const float ghostTextAlpha = hasBg ? 0.75f : 0.9f;
#endif
            float logoBottom = (logoTex && logoW > 0.0f) ? restoreMargin + logoH : restoreMargin + 40.0f * s;
            float btnX = (float)winW - miniBtnW - restoreMargin;
            float btnY = logoBottom + 30.0f * s;

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * s);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.1f, ghostBtnAlpha));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, ghostTextAlpha));

            const bool showTutorial = gameFrontGetShowTutorialButton();
            struct { langid labelId; int code; bool show; } miniModes[] = {
                { STR_DLGSETTINGS_TUTORIAL, RESULT_TUTORIAL,     showTutorial },
                { STR_DLGWELCOME_SINGLE,    RESULT_SINGLEPLAYER, true },
                { STR_DLGWELCOME_INTERNET,  RESULT_INTERNET,     true },
                { STR_DLGWELCOME_LOCAL,     RESULT_LAN,          true },
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
                { STR_DLGWELCOME_MAPEDITOR, RESULT_MAPEDITOR,    true },
                { STR_DLGWELCOME_LOGVIEWER, RESULT_LOGVIEWER,    true },
#endif
                { STR_DLGSETTINGS_TITLE,    RESULT_SETTINGS,     true },
#if !BOLO_MOBILE
                { STR_DLGOPENING_BUTTON2,   RESULT_QUIT,         true },
#endif
            };
            int miniCount = sizeof(miniModes) / sizeof(miniModes[0]);

            for (int i = 0; i < miniCount; i++) {
                if (!miniModes[i].show) continue;
                ImGui::SetCursorPos(ImVec2(btnX, btnY));
                bool hovered = false;
                ImVec2 hoverMin(btnX, btnY);
                ImVec2 hoverMax(btnX + miniBtnW, btnY + miniBtnH);
                if (ImGui::IsMouseHoveringRect(hoverMin, hoverMax)) {
                    hovered = true;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                }
                char miniLabel[96];
                SDL_snprintf(miniLabel, sizeof(miniLabel), "%s##mini", langGetText(miniModes[i].labelId));
                if (ImGui::Button(miniLabel, ImVec2(miniBtnW, miniBtnH))) {
                    result = miniModes[i].code;
                    running = false;
                }
                imguiHandOnHover();
                if (hovered) {
                    ImGui::PopStyleColor();
                }
                btnY += miniBtnH + miniBtnGap;
                /* Small visual gap after Local and after Settings */
                if (miniModes[i].code == RESULT_LAN || miniModes[i].code == RESULT_SETTINGS) {
                    btnY += miniBtnH * 0.4f;
                }
            }

            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(1);
        }

        /* Play/Pause button — bottom-left */
        if (hasBg) {
            const float ppSize = 30.0f * s;
            const float ppMargin = 12.0f * s;
#if BOLO_MOBILE
            const float ppBaseAlpha = 0.7f; /* Touch screens have no hover — keep visible */
#else
            const float ppBaseAlpha = 0.15f;
#endif
            float ppX = ppMargin;
            float ppY = (float)winH - ppSize - ppMargin;

            ImGui::SetCursorPos(ImVec2(ppX, ppY));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * s);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ppBaseAlpha);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.1f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));

            ImVec2 ppHoverMin(ppX, ppY);
            ImVec2 ppHoverMax(ppX + ppSize, ppY + ppSize);
            bool ppHovered = ImGui::IsMouseHoveringRect(ppHoverMin, ppHoverMax);
            if (ppHovered) {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
            }

            if (ImGui::Button("##playPause", ImVec2(ppSize, ppSize))) {
                bg->paused = !bg->paused;
            }
            imguiHandOnHover();

            ImVec2 ppMin = ImGui::GetItemRectMin();
            ImVec2 ppMax = ImGui::GetItemRectMax();
            float cx = (ppMin.x + ppMax.x) * 0.5f;
            float cy = (ppMin.y + ppMax.y) * 0.5f;
            float effectiveAlpha = ppHovered ? 1.0f : ppBaseAlpha;
            ImU32 iconCol = IM_COL32(200, 200, 200, (int)(220 * effectiveAlpha));
            ImDrawList *dl = ImGui::GetWindowDrawList();

            if (bg->paused) {
                float sz = 7.0f * s;
                dl->AddTriangleFilled(
                    ImVec2(cx + sz, cy),
                    ImVec2(cx - sz * 0.6f, cy - sz),
                    ImVec2(cx - sz * 0.6f, cy + sz),
                    iconCol);
            } else {
                float bw = 3.0f * s, bh = 8.0f * s, gap = 2.0f * s;
                dl->AddRectFilled(ImVec2(cx - gap - bw, cy - bh), ImVec2(cx - gap, cy + bh), iconCol);
                dl->AddRectFilled(ImVec2(cx + gap, cy - bh), ImVec2(cx + gap + bw, cy + bh), iconCol);
            }

            if (ppHovered) {
                ImGui::PopStyleVar();
            }

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(3);
        }

        /* Version label — bottom-right of screen.
         * Idle shows "vX.Y"; hovering cross-fades to the full
         * "vX.Y \xC2\xB7 <sha> \xC2\xB7 <date>" over ~180ms, fading back
         * out on un-hover. A 1px black drop shadow keeps both forms
         * legible over varied terrain in the background. */
        {
            char shortVer[32], fullVer[96];
            SDL_snprintf(shortVer, sizeof(shortVer), "v%s", WINBOLO_VERSION);
            SDL_snprintf(fullVer, sizeof(fullVer),
                         "v%s \xC2\xB7 %s \xC2\xB7 %s",
                         WINBOLO_VERSION, WINBOLO_GIT_HASH, WINBOLO_BUILD_DATE);

            ImVec2 shortSize = ImGui::CalcTextSize(shortVer);
            ImVec2 fullSize  = ImGui::CalcTextSize(fullVer);
            const float margin = 12.0f * s;

            /* Anchor the long form's bottom-right corner; the short form
             * right-aligns to the same edge so the visible right edge of
             * the text stays still through the cross-fade. */
            ImGui::SetCursorPos(ImVec2((float)winW - fullSize.x - margin,
                                       (float)winH - fullSize.y - margin));
            ImVec2 fullScreen = ImGui::GetCursorScreenPos();
            const float rightX = fullScreen.x + fullSize.x;

            /* Stable hover hit-rect covers the long form so hover state
             * doesn't toggle as the text width grows/shrinks mid-fade. */
            bool hovering = ImGui::IsMouseHoveringRect(
                fullScreen,
                ImVec2(rightX, fullScreen.y + fullSize.y));

            static float s_phase = 0.0f;       /* 0 = short, 1 = full */
            const float FADE_SEC = 0.45f;
            const float target = hovering ? 1.0f : 0.0f;
            const float step = ImGui::GetIO().DeltaTime / FADE_SEC;
            if (s_phase < target) {
                s_phase = (s_phase + step >= target) ? target : s_phase + step;
            } else if (s_phase > target) {
                s_phase = (s_phase - step <= target) ? target : s_phase - step;
            }

            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float shortA = 1.0f - s_phase;
            const float fullA  = s_phase;

            if (shortA > 0.01f) {
                ImVec2 pos(rightX - shortSize.x, fullScreen.y);
                dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f),
                            IM_COL32(0, 0, 0, (int)(180 * shortA)), shortVer);
                dl->AddText(pos,
                            IM_COL32(235, 235, 235, (int)(230 * shortA)), shortVer);
            }
            if (fullA > 0.01f) {
                dl->AddText(ImVec2(fullScreen.x + 1.0f, fullScreen.y + 1.0f),
                            IM_COL32(0, 0, 0, (int)(180 * fullA)), fullVer);
                dl->AddText(fullScreen,
                            IM_COL32(235, 235, 235, (int)(230 * fullA)), fullVer);
            }
        }

        ImGui::End(); /* ##WelcomeBg host */

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        if (hasBg) {
            bgGameRender(bg, renderer, winW, winH);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 40);
            SDL_FRect overlayRect = { 0, 0, (float)winW, (float)winH };
            SDL_RenderFillRect(renderer, &overlayRect);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    /* Clean up textures */
    if (shimmerTarget) SDL_DestroyTexture(shimmerTarget);
    if (logoTex) SDL_DestroyTexture(logoTex);

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

    return result;
}
