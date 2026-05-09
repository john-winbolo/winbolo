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
#include "../../ui_mode.h"
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

/* Try to load a BMP from next to the exe, then from data/ */
static SDL_Texture *loadBmp(SDL_Renderer *renderer, const char *filename) {
    SDL_Surface *surf = SDL_LoadBMP(filename);
    if (!surf) {
        char path[512];
        SDL_snprintf(path, sizeof(path), "data/%s", filename);
        surf = SDL_LoadBMP(path);
    }
    if (!surf) return nullptr;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_DestroySurface(surf);
    return tex;
}

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
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    /* Apply font and touch scaling */
    dialogApplyScaling(s);

    /* Load images */
    SDL_Texture *logoTex     = loadPng(renderer, "smalllogo-transparent.png");
    /* Icons in the same order as modes[] below. Tutorial is the first
     * row (no icon yet). */
    SDL_Texture *btnIcons[7] = {
        nullptr,  /* tutorial */
        loadBmp(renderer, "button_practice.bmp"),  /* single player */
        loadBmp(renderer, "button_internet.bmp"),  /* internet */
        loadBmp(renderer, "button_lan.bmp"),       /* local */
        nullptr,  /* map editor */
        nullptr,  /* log viewer */
        nullptr,  /* settings */
    };

    /* Query logo dimensions (scaled) */
    float logoW = 0.0f, logoH = 0.0f;
    if (logoTex) {
        SDL_GetTextureSize(logoTex, &logoW, &logoH);
        logoW *= 0.75f; logoH *= 0.75f;
        if (s > 1.05f) { logoW *= s; logoH *= s; }
    }

    /* Use shared background game */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);

    /* Dialog visibility toggle with animation.
     * Start minimized when there is a background game to watch;
     * otherwise start fully visible (e.g. Android where bg may not load). */
    bool dialogVisible = !hasBg;
    float dialogAlpha = hasBg ? 0.0f : 1.0f;
    Uint64 lastFrameTime = SDL_GetTicks();

    int result = RESULT_QUIT;
    bool running = true;
    Uint64 lastTickTime = SDL_GetTicks();

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
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

        /* Animate dialog alpha */
        Uint64 now = SDL_GetTicks();
        float dt = (float)(now - lastFrameTime) / 1000.0f;
        lastFrameTime = now;
        if (dt > 0.1f) dt = 0.1f;
        {
            float target = dialogVisible ? 1.0f : 0.0f;
            float speed = 4.0f;
            if (dialogAlpha < target) {
                dialogAlpha += speed * dt;
                if (dialogAlpha > target) dialogAlpha = target;
            } else if (dialogAlpha > target) {
                dialogAlpha -= speed * dt;
                if (dialogAlpha < target) dialogAlpha = target;
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

        /* When dialog is hidden, show restore button in top-right with transparent buttons */
        if (hasBg && dialogAlpha < 0.01f) {
            const float restoreMargin = 12.0f * s;
            float restoreX, restoreY;

            /* Show logo at actual size as the restore button */
            if (logoTex && logoW > 0.0f) {
                restoreX = (float)winW - logoW - restoreMargin;
                restoreY = restoreMargin;
                ImGui::SetCursorPos(ImVec2(restoreX, restoreY));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));

                if (ImGui::ImageButton("##restoreDialog", (ImTextureID)logoTex,
                                       ImVec2(logoW, logoH))) {
                    dialogVisible = true;
                }

                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(1);
            } else {
                const float restoreSize = 40.0f * s;
                restoreX = (float)winW - restoreSize - restoreMargin;
                restoreY = restoreMargin;
                ImGui::SetCursorPos(ImVec2(restoreX, restoreY));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * s);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.1f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.9f));

                if (ImGui::Button("W##restoreDialog", ImVec2(restoreSize, restoreSize))) {
                    dialogVisible = true;
                }

                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(2);
            }

            /* Transparent menu buttons below the logo */
            const float miniBtnW = 180.0f * s;
            const float miniBtnH = 30.0f * s;
            const float miniBtnGap = 4.0f * s;
#if BOLO_MOBILE
            const float ghostBtnAlpha = 0.6f;
            const float ghostTextAlpha = 0.9f;
#else
            const float ghostBtnAlpha = 0.15f;
            const float ghostTextAlpha = 0.75f;
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
            /* Map Editor is mouse-driven; gamepad-only players on Steam
               Deck have no usable workflow. Hide on Deck. */
            const bool showMapEditor = !uiModeIsSteamDeck();
            struct { langid labelId; int code; bool show; } miniModes[] = {
                { STR_DLGSETTINGS_TUTORIAL, RESULT_TUTORIAL,     showTutorial },
                { STR_DLGWELCOME_SINGLE,    RESULT_SINGLEPLAYER, true },
                { STR_DLGWELCOME_INTERNET,  RESULT_INTERNET,     true },
                { STR_DLGWELCOME_LOCAL,     RESULT_LAN,          true },
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
                { STR_DLGWELCOME_MAPEDITOR, RESULT_MAPEDITOR,    showMapEditor },
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
                /* Default keyboard / gamepad focus on Single Player so D-pad
                   navigation lands somewhere sensible (not Quit). */
                if (miniModes[i].code == RESULT_SINGLEPLAYER) {
                    ImGui::SetItemDefaultFocus();
                }
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

        /* Centered dialog panel — only when visible or animating */
        if (dialogAlpha > 0.01f) {
            float panelW = 500.0f * s, panelH = 570.0f * s;
            if (panelW > (float)winW * 0.9f) panelW = (float)winW * 0.9f;
            if (panelH > (float)winH * 0.9f) panelH = (float)winH * 0.9f;

            ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
            ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
            ImGui::SetNextWindowBgAlpha(0.7f * dialogAlpha);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, dialogAlpha);
            ImGui::Begin("##Welcome", nullptr,
                         ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar);

            /* Hide arrow button — top-right corner */
            if (hasBg) {
                const float arrowBtnSize = 24.0f * s;
                const float arrowMargin = 6.0f * s;
                ImGui::SetCursorPos(ImVec2(panelW - arrowBtnSize - arrowMargin, arrowMargin));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * s);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.4f, 0.6f));

                if (ImGui::Button("##hideDialog", ImVec2(arrowBtnSize, arrowBtnSize))) {
                    dialogVisible = false;
                }
                ImVec2 btnMin = ImGui::GetItemRectMin();
                ImVec2 btnMax = ImGui::GetItemRectMax();
                float cx = (btnMin.x + btnMax.x) * 0.5f;
                float cy = (btnMin.y + btnMax.y) * 0.5f;
                float sz = 6.0f * s;
                ImVec2 tri[3] = {
                    ImVec2(cx + sz, cy),
                    ImVec2(cx - sz, cy - sz),
                    ImVec2(cx - sz, cy + sz),
                };
                ImU32 arrowCol = IM_COL32(200, 200, 200, (int)(180 * dialogAlpha));
                ImGui::GetWindowDrawList()->AddTriangleFilled(tri[0], tri[1], tri[2], arrowCol);

                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(2);
            }

            /* Logo banner */
            ImGui::SetCursorPosY(8.0f * s);
            if (logoTex && logoW > 0.0f) {
                ImGui::SetCursorPosX((panelW - logoW) * 0.5f);
                ImGui::Image((ImTextureID)logoTex, ImVec2(logoW, logoH));
            } else {
                ImGui::SetWindowFontScale(2.0f);
                const char *title = langGetText(STR_MENU_WINBOLO);
                ImVec2 textSize = ImGui::CalcTextSize(title);
                ImGui::SetCursorPosX((panelW - textSize.x) * 0.5f);
                ImGui::Text("%s", title);
                ImGui::SetWindowFontScale(1.0f);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* Game mode buttons with icons */
            const float btnW = 220.0f * s;
            const float btnH = 36.0f * s;
            const float iconSize = 32.0f * s;
            const float iconGap = 6.0f * s;
            const float rowW = iconSize + iconGap + btnW;
            const float rowStartX = (panelW - rowW) * 0.5f;

            const bool showTutorialMain = gameFrontGetShowTutorialButton();
            const bool showMapEditorMain = !uiModeIsSteamDeck();
            struct { langid labelId; int code; bool show; } modes[] = {
                { STR_DLGSETTINGS_TUTORIAL, RESULT_TUTORIAL,     showTutorialMain },
                { STR_DLGWELCOME_SINGLE,    RESULT_SINGLEPLAYER, true },
                { STR_DLGWELCOME_INTERNET,  RESULT_INTERNET,     true },
                { STR_DLGWELCOME_LOCAL,     RESULT_LAN,          true },
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
                { STR_DLGWELCOME_MAPEDITOR, RESULT_MAPEDITOR,    showMapEditorMain },
                { STR_DLGWELCOME_LOGVIEWER, RESULT_LOGVIEWER,    true },
#endif
                { STR_DLGSETTINGS_TITLE,    RESULT_SETTINGS,     true },
            };
            int modeCount = sizeof(modes) / sizeof(modes[0]);

            for (int i = 0; i < modeCount; i++) {
                if (!modes[i].show) continue;
                /* Small gap before Map Editor and before Settings */
                if (modes[i].code == RESULT_MAPEDITOR || modes[i].code == RESULT_SETTINGS) {
                    ImGui::Spacing();
                    ImGui::Spacing();
                }

                float rowY = ImGui::GetCursorPosY();

                ImGui::SetCursorPosX(rowStartX);
                if (btnIcons[i]) {
                    ImGui::SetCursorPosY(rowY + (btnH - iconSize) * 0.5f);
                    ImGui::Image((ImTextureID)btnIcons[i], ImVec2(iconSize, iconSize));
                } else {
                    ImGui::Dummy(ImVec2(iconSize, iconSize));
                }

                ImGui::SameLine(0.0f, iconGap);
                ImGui::SetCursorPosY(rowY);
                if (ImGui::Button(langGetText(modes[i].labelId), ImVec2(btnW, btnH))) {
                    result = modes[i].code;
                    running = false;
                }
                /* Default keyboard / gamepad focus on Single Player so D-pad
                   navigation lands somewhere sensible (not Quit). */
                if (modes[i].code == RESULT_SINGLEPLAYER) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::Spacing();
            }


#if !BOLO_MOBILE
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* Quit button — right-aligned */
            {
                const float quitW = 120.0f * s;
                ImGui::SetCursorPosX(panelW - quitW - 16.0f * s);
                if (ImGui::Button(langGetText(STR_DLGOPENING_BUTTON2), ImVec2(quitW, btnH))) {
                    result = RESULT_QUIT;
                    running = false;
                }
            }
#endif

            ImGui::End(); /* ##Welcome panel */
            ImGui::PopStyleVar(); /* Alpha */
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

        /* Version label — bottom-right of screen */
        {
            const char *ver = "v" WINBOLO_VERSION;
            ImVec2 verSize = ImGui::CalcTextSize(ver);
            ImGui::SetCursorPos(ImVec2((float)winW - verSize.x - 12.0f * s,
                                       (float)winH - verSize.y - 12.0f * s));
            ImGui::TextDisabled("%s", ver);
        }

        ImGui::End(); /* ##WelcomeBg host */

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        if (hasBg) {
            bgGameRender(bg, renderer, winW, winH);
            int overlayAlpha = (int)(40.0f + dialogAlpha * 100.0f);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)overlayAlpha);
            SDL_FRect overlayRect = { 0, 0, (float)winW, (float)winH };
            SDL_RenderFillRect(renderer, &overlayRect);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    /* Clean up textures */
    for (int i = 0; i < 4; i++) {
        if (btnIcons[i]) SDL_DestroyTexture(btnIcons[i]);
    }
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
