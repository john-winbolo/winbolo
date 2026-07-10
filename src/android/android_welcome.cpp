/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * android_welcome.cpp — Touch-friendly ImGui welcome dialog for Android.
 *
 * Based on src/gui/sdl3/dialogs/imgui_welcome.cpp but adapted for
 * fullscreen Android display with scaled fonts and touch-sized buttons.
 * Renders the Everard Island map as a dimmed background.
 * Runs its own blocking SDL event loop.
 */

#include <SDL3/SDL.h>

#include "../common/wb_log.h"

#include "imgui.h"
#include "../gui/imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../gui/sdl3/dialogs/imgui_dialog_utils.h"

extern "C" {
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/bg_game.h"
#include "../gui/sdl3/dialogs/imgui_welcome.h"
#include "../gui/gamefront.h"
#include "../gui/lang.h"
#include "global.h"
}

/* Match openingStates enum from gamefront.h */
enum {
    RESULT_TUTORIAL = 4,   /* openTutorial */
    RESULT_PRACTICE = 5,   /* openSetup */
    RESULT_TCP      = 6,   /* openUdp */
    RESULT_LAN      = 10,  /* openLan */
    RESULT_INTERNET = 14,  /* openInternet */
    RESULT_QUIT     = -1
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

/* Load font data from Android assets via SDL_IOFromFile into a buffer
 * that ImGui can own (allocated with IM_ALLOC so ImGui can free it). */
static unsigned char *loadFontFromAssets(const char *path, int *outSize) {
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return nullptr;
    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return nullptr; }
    unsigned char *buf = (unsigned char *)IM_ALLOC((size_t)size);
    if (!buf) { SDL_CloseIO(io); return nullptr; }
    size_t read = SDL_ReadIO(io, buf, (size_t)size);
    SDL_CloseIO(io);
    if ((Sint64)read != size) { IM_FREE(buf); return nullptr; }
    *outSize = (int)size;
    return buf;
}

extern "C" int imguiWelcomeShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return RESULT_QUIT;

    /* Save and disable the logical presentation so that ImGui's input
     * coordinates (window pixels) match the rendering coordinates. */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) {
        screenW = 1920;
        screenH = 1080;
    }

    SDL_ShowWindow(window);
    WB_LOG_INFO(WB_LOG_CAT_GUI, "[Welcome] Screen: %dx%d", screenW, screenH);

    /* Use shared background game (live bots playing) */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);
    Uint64 lastTickTime = SDL_GetTicks();

    /* Set up ImGui context */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    /* Compute a UI scale factor based on screen height. */
    float uiScale = (float)screenH / 540.0f;
    if (uiScale < 1.0f) uiScale = 1.0f;

    /* Load touch-friendly font via SDL assets */
    float fontSize = 20.0f * uiScale;
    {
        int fontDataSize = 0;
        unsigned char *fontData = loadFontFromAssets("data/fonts/InterVariable.ttf", &fontDataSize);
        if (!fontData) {
            fontData = loadFontFromAssets("fonts/InterVariable.ttf", &fontDataSize);
        }
        if (fontData) {
            WB_LOG_DEBUG(WB_LOG_CAT_ASSET, "[Welcome] Loaded font, %d bytes, size=%.0f", fontDataSize, fontSize);
            io.Fonts->AddFontFromMemoryTTF(fontData, fontDataSize, fontSize);
        } else {
            WB_LOG_WARN(WB_LOG_CAT_ASSET, "[Welcome] Font load failed, using default font scaled to %.1f", uiScale);
            ImFontConfig config;
            config.SizePixels = fontSize;
            config.OversampleH = 2;
            config.OversampleV = 2;
            io.Fonts->AddFontDefault(&config);
        }
    }

    /* Touch-friendly style — make the dialog panel semi-transparent */
    {
        ImGuiStyle &style = ImGui::GetStyle();
        style.TouchExtraPadding = ImVec2(8.0f, 8.0f);
        style.ScaleAllSizes(uiScale);
        /* Semi-transparent window background so the map shows through */
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.85f);
    }

    /* Load images */
    SDL_Texture *logoTex = loadBmp(renderer, "smalllogo.bmp");
    SDL_Texture *btnIcons[5] = {
        loadBmp(renderer, "button_tutorial.bmp"),
        loadBmp(renderer, "button_practice.bmp"),
        loadBmp(renderer, "button_tcp.bmp"),
        loadBmp(renderer, "button_lan.bmp"),
        loadBmp(renderer, "button_internet.bmp"),
    };

    /* Query logo dimensions */
    float logoW = 0.0f, logoH = 0.0f;
    if (logoTex) {
        SDL_GetTextureSize(logoTex, &logoW, &logoH);
        logoW *= uiScale;
        logoH *= uiScale;
    }

    float dlgW = (float)screenW;
    float dlgH = (float)screenH;

    /* Dialog panel dimensions — centered, not fullscreen */
    float panelW = 500.0f * uiScale;
    float panelH = dlgH * 0.9f;
    if (panelW > dlgW * 0.8f) panelW = dlgW * 0.8f;
    float panelX = (dlgW - panelW) * 0.5f;
    float panelY = (dlgH - panelH) * 0.5f;

    /* Button sizing scaled to screen */
    float btnW = 280.0f * uiScale;
    float btnH = 56.0f * uiScale;
    float iconSize = 48.0f * uiScale;
    float iconGap = 10.0f * uiScale;
    if (btnW > panelW - 80.0f * uiScale) btnW = panelW - 80.0f * uiScale;

    /* Dialog visibility toggle with animation — start minimized */
    bool dialogVisible = false;
    float dialogAlpha = 0.0f;     /* 1.0 = dialog shown, 0.0 = dialog hidden */
    bool bgPaused = false;
    Uint64 lastFrameTime = SDL_GetTicks();

    int result = RESULT_QUIT;
    bool running = true;

    /* Mode labels and codes shared between full dialog and mini buttons.
     * IDs reused from the SDL3 welcome dialog where the action matches;
     * STR_DLGOPENING_OPTION2 reused for TCP/IP (no SDL3 welcome equivalent). */
    struct { langid labelId; int code; } modes[] = {
        { STR_DLGSETTINGS_TUTORIAL, RESULT_TUTORIAL },
        { STR_DLGWELCOME_SINGLE,    RESULT_PRACTICE },
        { STR_DLGOPENING_OPTION2,   RESULT_TCP },
        { STR_DLGWELCOME_LOCAL,     RESULT_LAN },
        { STR_DLGWELCOME_INTERNET,  RESULT_INTERNET },
    };

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) {
                result = RESULT_QUIT;
                running = false;
            }
        }

        /* Tick the background game at fixed rate (unless paused) */
        if (hasBg && !bgPaused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bgPaused) {
            lastTickTime = SDL_GetTicks();
        }

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
        ImGui::NewFrame();

        /* Transparent full-screen host window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(dlgW, dlgH));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##WelcomeBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* When dialog is hidden, show restore button (logo) in top-right with transparent buttons below */
        if (hasBg && dialogAlpha < 0.01f) {
            const float restoreSize = 48.0f * uiScale;
            const float restoreMargin = 16.0f * uiScale;
            float restoreX = dlgW - restoreSize - restoreMargin;
            float restoreY = restoreMargin;
            ImGui::SetCursorPos(ImVec2(restoreX, restoreY));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * uiScale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.1f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.9f));

            if (logoTex && logoW > 0.0f) {
                if (ImGui::ImageButton("##restoreDialog", (ImTextureID)logoTex,
                                       ImVec2(restoreSize, restoreSize))) {
                    dialogVisible = true;
                }
                imguiHandOnHover();
            } else {
                if (ImGui::Button("W##restoreDialog", ImVec2(restoreSize, restoreSize))) {
                    dialogVisible = true;
                }
                imguiHandOnHover();
            }

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);

            /* Transparent menu buttons below the logo */
            const float miniBtnW = 160.0f * uiScale;
            const float miniBtnH = 40.0f * uiScale;
            const float miniBtnGap = 6.0f * uiScale;
            const float ghostAlpha = 0.15f;
            float mbX = dlgW - miniBtnW - restoreMargin;
            float mbY = restoreY + restoreSize + 12.0f * uiScale;

            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ghostAlpha);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * uiScale);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.1f, 0.1f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

            for (int i = 0; i < 5; i++) {
                ImGui::SetCursorPos(ImVec2(mbX, mbY));
                bool hovered = false;
                ImVec2 hoverMin(mbX, mbY);
                ImVec2 hoverMax(mbX + miniBtnW, mbY + miniBtnH);
                if (ImGui::IsMouseHoveringRect(hoverMin, hoverMax)) {
                    hovered = true;
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
                }
                char miniLabel[96];
                SDL_snprintf(miniLabel, sizeof(miniLabel), "%s##mini",
                             langGetText(modes[i].labelId));
                if (ImGui::Button(miniLabel, ImVec2(miniBtnW, miniBtnH))) {
                    if (modes[i].code == RESULT_TUTORIAL) {
                        gameFrontSetUdpOptions((char *)"android",
                                               (char *)"192.168.42.81",
                                               28500, 28500);
                        gameFrontSetDlgState(openUdp);
                        gameFrontSetDlgState(openUdpJoin);
                        result = (int)openFinished;
                    } else {
                        result = modes[i].code;
                    }
                    running = false;
                }
                imguiHandOnHover();
                if (hovered) {
                    ImGui::PopStyleVar();
                }
                mbY += miniBtnH + miniBtnGap;
            }

            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }

        /* Centered dialog panel — only when visible or animating */
        if (dialogAlpha > 0.01f) {
            ImGui::SetNextWindowPos(ImVec2(panelX, panelY));
            ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
            ImGui::SetNextWindowBgAlpha(0.85f * dialogAlpha);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, dialogAlpha);
            ImGui::Begin("##Welcome", nullptr,
                         ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar);

            /* Hide arrow button — top-right corner of the dialog panel */
            if (hasBg) {
                const float arrowBtnSize = 32.0f * uiScale;
                const float arrowMargin = 8.0f * uiScale;
                ImGui::SetCursorPos(ImVec2(panelW - arrowBtnSize - arrowMargin, arrowMargin));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * uiScale);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.4f, 0.6f));

                if (ImGui::Button("##hideDialog", ImVec2(arrowBtnSize, arrowBtnSize))) {
                    dialogVisible = false;
                }
                imguiHandOnHover();
                /* Draw right-pointing arrow (triangle) on the button */
                ImVec2 btnMin = ImGui::GetItemRectMin();
                ImVec2 btnMax = ImGui::GetItemRectMax();
                float cx = (btnMin.x + btnMax.x) * 0.5f;
                float cy = (btnMin.y + btnMax.y) * 0.5f;
                float sz = 8.0f * uiScale;
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

            /* Center content vertically within the panel */
            float rowH = btnH + ImGui::GetStyle().ItemSpacing.y;
            float contentH = logoH + 20.0f * uiScale
                            + rowH * 5
                            + 30.0f * uiScale
                            + btnH;
            float startY = panelY + (panelH - contentH) * 0.4f;
            if (startY < panelY + 10.0f * uiScale) startY = panelY + 10.0f * uiScale;
            ImGui::SetCursorPosY(startY - panelY);

            /* Logo banner */
            if (logoTex && logoW > 0.0f) {
                ImGui::SetCursorPosX((panelW - logoW) * 0.5f);
                ImGui::Image((ImTextureID)logoTex, ImVec2(logoW, logoH));
            } else {
                ImGui::SetWindowFontScale(2.5f);
                const char *title = "WinBolo";
                ImVec2 textSize = ImGui::CalcTextSize(title);
                ImGui::SetCursorPosX((panelW - textSize.x) * 0.5f);
                ImGui::Text("%s", title);
                ImGui::SetWindowFontScale(1.0f);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* Game mode buttons with icons */
            float rowW = iconSize + iconGap + btnW;
            float rowStartX = (panelW - rowW) * 0.5f;

            for (int i = 0; i < 5; i++) {
                float rowY = ImGui::GetCursorPosY();

                /* Icon */
                ImGui::SetCursorPosX(rowStartX);
                if (btnIcons[i]) {
                    ImGui::SetCursorPosY(rowY + (btnH - iconSize) * 0.5f);
                    ImGui::Image((ImTextureID)btnIcons[i], ImVec2(iconSize, iconSize));
                } else {
                    ImGui::Dummy(ImVec2(iconSize, iconSize));
                }

                /* Button on same line */
                ImGui::SameLine(0.0f, iconGap);
                ImGui::SetCursorPosY(rowY);
                if (ImGui::Button(langGetText(modes[i].labelId), ImVec2(btnW, btnH))) {
                    if (modes[i].code == RESULT_TUTORIAL) {
                        /* TEMP: Join server at 192.168.42.81:28500 as "android" */
                        gameFrontSetUdpOptions((char *)"android",
                                               (char *)"192.168.42.81",
                                               28500, 28500);
                        gameFrontSetDlgState(openUdp);
                        gameFrontSetDlgState(openUdpJoin);
                        result = (int)openFinished;
                    } else {
                        result = modes[i].code;
                    }
                    running = false;
                }
                imguiHandOnHover();
                ImGui::Spacing();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            /* Quit button */
            {
                float quitW = 180.0f * uiScale;
                ImGui::SetCursorPosX((panelW - quitW) * 0.5f);
                if (ImGui::Button(langGetText(STR_DLGOPENING_BUTTON2), ImVec2(quitW, btnH))) {
                    result = RESULT_QUIT;
                    running = false;
                }
                imguiHandOnHover();
            }

            /* Version label */
            {
                const char *ver = "v" WINBOLO_DISPLAY_VERSION;
                ImVec2 verSize = ImGui::CalcTextSize(ver);
                ImGui::SetCursorPosX(panelW - verSize.x - 16.0f);
                ImGui::SetCursorPosY(panelH - verSize.y - 16.0f);
                ImGui::TextDisabled("%s", ver);
            }

            ImGui::End(); /* ##Welcome panel */
            ImGui::PopStyleVar(); /* Alpha */
        }

        /* Play/Pause button — bottom-left, transparent like mini buttons */
        if (hasBg) {
            const float ppSize = 40.0f * uiScale;
            const float ppMargin = 16.0f * uiScale;
            const float ppGhostAlpha = 0.15f;
            float ppX = ppMargin;
            float ppY = dlgH - ppSize - ppMargin;

            ImGui::SetCursorPos(ImVec2(ppX, ppY));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * uiScale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ppGhostAlpha);
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
                bgPaused = !bgPaused;
            }
            imguiHandOnHover();

            ImVec2 ppMin = ImGui::GetItemRectMin();
            ImVec2 ppMax = ImGui::GetItemRectMax();
            float cx = (ppMin.x + ppMax.x) * 0.5f;
            float cy = (ppMin.y + ppMax.y) * 0.5f;
            float effectiveAlpha = ppHovered ? 1.0f : ppGhostAlpha;
            ImU32 iconCol = IM_COL32(200, 200, 200, (int)(220 * effectiveAlpha));
            ImDrawList *dl = ImGui::GetWindowDrawList();

            if (bgPaused) {
                /* Play triangle (right-pointing) */
                float sz = 9.0f * uiScale;
                dl->AddTriangleFilled(
                    ImVec2(cx + sz, cy),
                    ImVec2(cx - sz * 0.6f, cy - sz),
                    ImVec2(cx - sz * 0.6f, cy + sz),
                    iconCol);
            } else {
                /* Pause bars */
                float bw = 3.0f * uiScale, bh = 9.0f * uiScale, gap = 2.5f * uiScale;
                dl->AddRectFilled(ImVec2(cx - gap - bw, cy - bh), ImVec2(cx - gap, cy + bh), iconCol);
                dl->AddRectFilled(ImVec2(cx + gap, cy - bh), ImVec2(cx + gap + bw, cy + bh), iconCol);
            }

            if (ppHovered) {
                ImGui::PopStyleVar();
            }

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(3);
        }

        ImGui::End(); /* ##WelcomeBg host */

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        /* Draw live background game with overlay that lightens as dialog hides */
        if (hasBg) {
            bgGameRender(bg, renderer, screenW, screenH);
            int overlayAlpha = (int)(40.0f + dialogAlpha * 100.0f);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)overlayAlpha);
            SDL_FRect overlayRect = { 0, 0, (float)screenW, (float)screenH };
            SDL_RenderFillRect(renderer, &overlayRect);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    /* Clean up textures */
    for (int i = 0; i < 5; i++) {
        if (btnIcons[i]) SDL_DestroyTexture(btnIcons[i]);
    }
    if (logoTex) SDL_DestroyTexture(logoTex);

    /* Tear down ImGui — the main game loop will reinitialise it */
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    /* Restore the logical presentation for the game renderer */
    SDL_SetRenderLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

    /* Flush any quit events so the main loop doesn't exit immediately */
    SDL_FlushEvent(SDL_EVENT_QUIT);

    return result;
}
