/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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
#include "imgui_nav_outline.h"
#include "imgui_controller_prompt.h"
#include "../imgui_steam_nav.h"
#if !BOLO_MOBILE
#include "imgui_news.h"
#endif

extern "C" {
#include "../sdl3draw.h"
#include "../bg_game.h"
#include "imgui_welcome.h"
#include "../../gamefront.h"
#include "../../ui_mode.h"
#include "../input_gamepad.h"
#include "../../lang.h"
#include "imgui_winbolonet.h"
}

#include "imgui_about.h"   /* aboutPopupOpen / aboutPopupRender */
#include "imgui_keyboard.h" /* keyboardUpdate */

/* From winbolo.c */
extern "C" {
  void windowFullScreenChoose(bool on);
}

/* The return value is cast straight back to openingStates by the caller, so
   each code is the openingStates enumerator itself rather than its position.
   That lets gamefront.h reorder or drop members without shifting these. */
enum {
    RESULT_WELCOME      = openWelcome,  /* comes straight back here */
    RESULT_SINGLEPLAYER = openSetup,
    RESULT_TUTORIAL     = openTutorial,
    RESULT_INTERNET     = openInternet,
    RESULT_LAN          = openLan,
    RESULT_SETTINGS     = openSettings,
    RESULT_MAPEDITOR    = openMapEditor,
    RESULT_LOGVIEWER    = openLogViewer,
    RESULT_QUIT         = -1   /* Quit sentinel, not a dialog state */
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

/* Reddit / Discord glyphs for the bottom-right social links. Rasterised white
 * (like the About-box icons) so they read over any terrain, and cached for the
 * program lifetime — the renderer outlives every show of this screen. */
static SDL_Texture *s_welcomeReddit  = nullptr;
static SDL_Texture *s_welcomeDiscord = nullptr;
/* Fraction of each icon texture below the artwork's lowest pixel. The SVGs are
 * square but their glyphs don't fill the canvas (and Reddit/Discord differ), so
 * the texture's bottom edge isn't the glyph's bottom edge. We use this to
 * bottom-align the *visible* mark with the version text rather than the square. */
static float s_welcomeRedditInsetB  = 0.0f;
static float s_welcomeDiscordInsetB = 0.0f;

/* Bottom transparent inset of an SVG, as a fraction of its height: the gap from
 * the lowest drawn point to the canvas bottom. Matches imguiLoadSvgIconWhite's
 * fit (aspect-preserving, centred), so it maps straight onto the texture. */
static float welcomeSvgBottomInset(const char *path) {
    NSVGimage *img = nsvgParseFromFile(path, "px", 96.0f);
    if (!img) return 0.0f;
    if (img->width < 1.0f || img->height < 1.0f) { nsvgDelete(img); return 0.0f; }
    float maxY = 0.0f;
    bool any = false;
    for (NSVGshape *sh = img->shapes; sh; sh = sh->next) {
        if (!(sh->flags & NSVG_FLAGS_VISIBLE)) continue;
        if (!any || sh->bounds[3] > maxY) maxY = sh->bounds[3];
        any = true;
    }
    /* Square fit (these SVGs) => scale = size/height, offY = 0, so the texture
     * inset fraction equals the document inset fraction. */
    float inset = any ? (img->height - maxY) / img->height : 0.0f;
    nsvgDelete(img);
    return inset < 0.0f ? 0.0f : inset;
}

static void ensureWelcomeSocialIcons(SDL_Renderer *r) {
    static bool tried = false;
    if (tried || !r) return;
    tried = true;
    s_welcomeReddit       = imguiLoadSvgIconWhite(r, "data/ui/reddit.svg", 48);
    s_welcomeDiscord      = imguiLoadSvgIconWhite(r, "data/ui/discord.svg", 48);
    s_welcomeRedditInsetB  = welcomeSvgBottomInset("data/ui/reddit.svg");
    s_welcomeDiscordInsetB = welcomeSvgBottomInset("data/ui/discord.svg");
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    /* Apply font and touch scaling */
    dialogApplyScaling(s);

    /* Load images */
    SDL_Texture *logoTex = loadPng(renderer, "smalllogo-transparent.png");

    float logoW = 0.0f, logoH = 0.0f;
    if (logoTex) {
        float nativeW = 0.0f, nativeH = 0.0f;
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

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();

        /* Host-OS menus (macOS Dock) post transitions through gamefront.
         * Treat a pending entry as if the equivalent ghost button was
         * clicked — the openingStates enum already matches the RESULT_*
         * codes byte-for-byte. */
        {
            openingStates pending;
            if (gameFrontConsumeRequestedTransition(&pending)) {
                result = (int)pending;
                running = false;
            }
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
            /* Alt+Enter, the same full screen key the game window takes. Ahead
               of the ImGui feed so the keystroke never reaches a widget, and
               out through the top of this dialog the way the FullScreen button
               does, so the fonts and the UI scale are rebuilt for the surface
               we now have. */
            if (dialogIsFullScreenToggleEvent(window, &ev)) {
                windowFullScreenChoose(
                    (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
                result = RESULT_WELCOME;
                running = false;
                continue;
            }
#endif
            ImGui_ImplSDL3_ProcessEvent(&ev);
            /* C cycles the background game's camera to the next tank.
             * Only this dialog takes it: the other pre-game dialogs that
             * draw the same background carry text fields and the key
             * capture in key setup, where a bare letter belongs to them.
             *
             * The test is WantTextInput, not WantCaptureKeyboard: with
             * NavEnableKeyboard on, this dialog's own buttons leave a
             * window nav-focused and latch WantCaptureKeyboard true for
             * good, so testing it swallows every press (the same latch
             * sdl3ImguiRenderMenuBar works around for in-game keys).
             * WantTextInput is true only while a field holds the caret,
             * which is exactly when a bare letter is not ours. */
            if (hasBg && ev.type == SDL_EVENT_KEY_DOWN &&
                !ImGui::GetIO().WantTextInput) {
                /* +/- zoom the background in and out, 0 back to the
                 * fit-to-screen default. Auto-repeat is allowed here (a
                 * held key should keep zooming) but not on C, where it
                 * would race through the roster. */
                switch (ev.key.key) {
                    case SDLK_C:
                        if (!ev.key.repeat) bgGameCycleCamera(bg);
                        break;
                    /* = and KP+ as well as +: on most layouts + is
                     * shifted =, and a viewer who does not hold shift
                     * still means "zoom in". */
                    case SDLK_PLUS:
                    case SDLK_EQUALS:
                    case SDLK_KP_PLUS:
                        bgGameAdjustZoom(bg, +1);
                        break;
                    case SDLK_MINUS:
                    case SDLK_KP_MINUS:
                        bgGameAdjustZoom(bg, -1);
                        break;
                    case SDLK_0:
                    case SDLK_KP_0:
                        bgGameResetZoom(bg);
                        break;
                    default:
                        break;
                }
            }
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (dialogHandleUrlDropEvent(&ev)) { result = (int)openInternetManual; running = false; continue; }
            if (dialogHandleQuitEvent(window, &ev)) {
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

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();
        controllerDialogsRenderMenu();

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

#if !BOLO_MOBILE
        /* Single kick per program run — newsPopupKickFetch is idempotent
         * but the static guard avoids the call entirely once we've kicked. */
        static bool sNewsKicked = false;
        if (!sNewsKicked) {
            newsPopupKickFetch();
            sNewsKicked = true;
        }
#endif

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
                ImGui::Image((ImTextureID)logoTex, ImVec2(logoW, logoH));
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
            /* Map Editor and Log Viewer are mouse-driven; a controller player
               can't use them.  Hide them whenever a real controller is
               connected (or on Steam Deck) — even while the player is using
               the mouse.  Unlike the rest of the controller-mode UI, these
               follow controller *presence*, not the last-used device. */
            const bool showDesktopTools =
                !inputGamepadRealControllerConnected() && !uiModeIsSteamDeck();
            /* Deck and tablet run full screen from window creation, and
               controller mode leaves the host window alone everywhere else,
               so on those surfaces the button would have nothing to say. */
            const bool showFullScreenToggle =
                !uiModeIsSteamDeck() && !uiModeIsTablet() && !uiShouldUseControllerMode();
            /* Read the window itself rather than the preference, so the label
               still tells the truth after an OS-driven full screen change. */
            const bool isFullScreen =
                (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
            /* The button is named for where it takes you, and the table is
               rebuilt every frame, so the name follows the window. */
            const langid fullScreenLabel = isFullScreen
                ? STR_DLGWELCOME_SWITCH_CLASSIC
                : STR_DLGWELCOME_SWITCH_FULLSCREEN;
            /* rawLabel, when non-null, signals a non-exit action: the click
             * handler dispatches by rawLabel string rather than setting
             * result/running. Display text still goes through
             * langGetText(labelId) whenever labelId is non-zero; rawLabel
             * is only used as the visible text when labelId == 0.
             *
             * Spelled out as nullptr on every exit row rather than left off
             * the end. The value is the same either way — an omitted member is
             * value-initialised — but the click handler reads rawLabel to
             * decide what a row does, so the rows that do nothing with it are
             * worth seeing rather than inferring, and it drops eight
             * -Wmissing-field-initializers warnings. */
            struct { langid labelId; int code; bool show; const char* rawLabel; } miniModes[] = {
                { STR_DLGSETTINGS_TUTORIAL, RESULT_TUTORIAL,     showTutorial,     nullptr },
                { STR_DLGWELCOME_SINGLE,    RESULT_SINGLEPLAYER, true,             nullptr },
                { STR_DLGWELCOME_INTERNET,  RESULT_INTERNET,     true,             nullptr },
                { STR_DLGWELCOME_LOCAL,     RESULT_LAN,          true,             nullptr },
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
                { STR_DLGWELCOME_MAPEDITOR, RESULT_MAPEDITOR,    showDesktopTools, nullptr },
                { STR_DLGWELCOME_LOGVIEWER, RESULT_LOGVIEWER,    showDesktopTools, nullptr },
#endif
                { STR_DLGSETTINGS_TITLE,    RESULT_SETTINGS,     true,             nullptr },
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
                { fullScreenLabel,          0,                   showFullScreenToggle,
                                                                 "FullScreen" },
#endif
#if !BOLO_MOBILE
                { STR_DLGWELCOME_NEWS,      0,                   true,             "News" },
                { STR_DLGOPENING_BUTTON2,   RESULT_QUIT,         true,             nullptr },
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
                const char* labelText = (miniModes[i].labelId != 0)
                    ? langGetText(miniModes[i].labelId)
                    : miniModes[i].rawLabel;
                SDL_snprintf(miniLabel, sizeof(miniLabel), "%s##mini", labelText);
                if (ImGui::Button(miniLabel, ImVec2(miniBtnW, miniBtnH))) {
#if !BOLO_MOBILE
#if !defined(__EMSCRIPTEN__)
                    if (miniModes[i].rawLabel &&
                        SDL_strcmp(miniModes[i].rawLabel, "FullScreen") == 0) {
                        /* Move the window, then come back in through the top
                           of this dialog so the fonts and the UI scale are
                           rebuilt for the surface we now have. */
                        windowFullScreenChoose(!isFullScreen);
                        result = RESULT_WELCOME;
                        running = false;
                    } else
#endif
                    if (miniModes[i].rawLabel && SDL_strcmp(miniModes[i].rawLabel, "News") == 0) {
                        newsPopupOpenManual();
                    } else
#endif
                    {
                        result = miniModes[i].code;
                        running = false;
                    }
                }
                imguiHandOnHover();
                /* Default keyboard / gamepad focus on Single Player so D-pad
                   navigation lands somewhere sensible (not Quit). */
                if (miniModes[i].code == RESULT_SINGLEPLAYER) {
                    ImGui::SetItemDefaultFocus();
                }
#if !BOLO_MOBILE
                /* Unread dot on the News button when fresh items exist. */
                if (miniModes[i].rawLabel &&
                    SDL_strcmp(miniModes[i].rawLabel, "News") == 0 &&
                    newsPopupHasUnread()) {
                    ImVec2 itemMin = ImGui::GetItemRectMin();
                    ImVec2 itemMax = ImGui::GetItemRectMax();
                    float dotR = 4.0f * s;
                    ImVec2 dotCenter(itemMax.x - dotR - 6.0f * s,
                                     (itemMin.y + itemMax.y) * 0.5f);
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        dotCenter, dotR, IM_COL32(220, 60, 60, 230));
                }
#endif
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

        /* Top-left WinBolo.net account status: player name + signed-in
         * state + Login/Logout, plus the 1v1 ladder rank when signed in.
         * Reuses the settings dialog's login popup and worker. */
        {
            const float statusMargin = 12.0f * s;
            ImGui::SetCursorPos(ImVec2(statusMargin, statusMargin));
            ImGui::BeginGroup();
            imguiWinbolonetDrawStatusBlock();
            ImGui::EndGroup();
        }

#if !BOLO_MOBILE
        /* Drive the news popup state machine. Renders the consent dialog
         * or the news modal when either is open, otherwise polls the
         * fetch handle and decides whether to auto-open. */
        newsPopupTick();
#endif

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
                bgGameTogglePause(bg);
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

        /* Reddit / Discord links — bottom-right, left of the version label and
         * centred on its baseline. Drawn icon-only in the version-label style
         * (invisible button + a tinted drawlist image with a 1px shadow) rather
         * than framed ImageButtons, so they sit cleanly over the terrain. */
        {
            ensureWelcomeSocialIcons(renderer);
            const float icon   = 27.5f * s;
            const float gap    = 10.0f * s;   /* between the two icons          */
            const float pad    = 12.0f * s;   /* between the icons and the text */
            const float margin = 12.0f * s;

            /* Mirror the version label's own placement so the icons tuck just
             * to its left and share its vertical centre. */
            char shortVer[32];
            SDL_snprintf(shortVer, sizeof(shortVer), "v%s", WINBOLO_DISPLAY_VERSION);
            ImVec2 verSize = ImGui::CalcTextSize(shortVer);
            float verX = (float)winW - verSize.x - margin;
            float verY = (float)winH - verSize.y - margin;

            float rowW  = icon * 2.0f + gap;
            float rowX  = verX - pad - rowW;
            float textBottom = verY + verSize.y;

            struct { SDL_Texture *tex; const char *id; const char *url; float insetB; } links[2] = {
                { s_welcomeReddit,  "##wreddit",  "https://www.reddit.com/r/winbolo", s_welcomeRedditInsetB },
                { s_welcomeDiscord, "##wdiscord", "https://discord.gg/znGR3VMaqd",    s_welcomeDiscordInsetB },
            };
            ImDrawList *sdl = ImGui::GetWindowDrawList();
            for (int i = 0; i < 2; ++i) {
                /* Drop each icon by its own transparent bottom band so the
                 * visible glyph — not the square — sits on the text baseline. */
                float rowY = textBottom - icon * (1.0f - links[i].insetB);
                ImGui::SetCursorPos(ImVec2(rowX + (float)i * (icon + gap), rowY));
                ImVec2 p = ImGui::GetCursorScreenPos();
                bool pressed = ImGui::InvisibleButton(links[i].id, ImVec2(icon, icon),
                                                      ImGuiButtonFlags_EnableNav);
                bool hov = ImGui::IsItemHovered();
                /* InvisibleButton already activates on release; a separate
                 * IsItemClicked() (fires on press-down) would open the URL a
                 * second time on the same click, spawning two browser tabs. */
                if (pressed) imguiOpenUrl(links[i].url);
                if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (links[i].tex) {
                    ImU32 tint = hov ? IM_COL32(255, 255, 255, 255)
                                     : IM_COL32(235, 235, 235, 210);
                    sdl->AddImage((ImTextureID)links[i].tex,
                                  ImVec2(p.x + 1.0f, p.y + 1.0f),
                                  ImVec2(p.x + icon + 1.0f, p.y + icon + 1.0f),
                                  ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0, 0, 0, 150));
                    sdl->AddImage((ImTextureID)links[i].tex, p,
                                  ImVec2(p.x + icon, p.y + icon),
                                  ImVec2(0, 0), ImVec2(1, 1), tint);
                }
            }
        }

        /* Version label — bottom-right of screen. Click to open About;
         * full version/hash/date is shown there instead of inline. A 1px
         * black drop shadow keeps the text legible over varied terrain. */
        {
            char shortVer[32];
            SDL_snprintf(shortVer, sizeof(shortVer), "v%s", WINBOLO_DISPLAY_VERSION);

            ImVec2 verSize = ImGui::CalcTextSize(shortVer);
            const float margin = 12.0f * s;

            ImGui::SetCursorPos(ImVec2((float)winW - verSize.x - margin,
                                       (float)winH - verSize.y - margin));
            ImVec2 verScreen = ImGui::GetCursorScreenPos();

            /* Invisible button captures the click + drives hover state.
             * Sized to the rendered text so the hit rect is exactly the
             * label, not the surrounding gutter. */
            /* EnableNav so controller / keyboard nav can reach the version
               label (InvisibleButton is ImGuiItemFlags_NoNav otherwise). Use
               the pressed return value so nav-activate (A / Space) opens About
               just like a mouse click. */
            bool verPressed = ImGui::InvisibleButton("##aboutver", verSize,
                                                     ImGuiButtonFlags_EnableNav);
            bool hovered = ImGui::IsItemHovered();
            if (verPressed || ImGui::IsItemClicked()) {
                aboutPopupOpen();
            }
            if (hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImU32 fg = hovered
                ? IM_COL32(255, 255, 255, 255)
                : IM_COL32(235, 235, 235, 230);
            dl->AddText(ImVec2(verScreen.x + 1.0f, verScreen.y + 1.0f),
                        IM_COL32(0, 0, 0, 180), shortVer);
            dl->AddText(verScreen, fg, shortVer);
        }

        ImGui::End(); /* ##WelcomeBg host */

        /* About box (opens on version-label click). Renders as a modal
         * popup over the welcome screen. */
        aboutPopupRender();

        dialogDrawNavOutline();
        keyboardUpdate();   /* controller text entry for the WinBolo.net login popup */
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
        gameFrontPumpDirty(); /* sync cloud prefs from menus (login join + debounced upload) */
        dialogFrameCapEnd(frameCapStart);
    }

    /* Clean up textures */
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
