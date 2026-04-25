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
 * Name:          imgui_settings.cpp
 * Purpose:       ImGui pre-game settings dialog.
 *                Shown as an overlay panel over the
 *                background game, same as the welcome
 *                dialog.
 *********************************************************/

#include <cstring>
#include <vector>
#include <string>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

extern "C" {
#include "../sdl3draw.h"
#include "../tileloader.h"
#include "../gfx_settings.h"
#include "../../tiles.h"
#include "../../gamefront.h"

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif
#include "../../../bolo/global.h"
#include "../../../bolo/screen.h"
#include "../bg_game.h"
#include "imgui_settings.h"
#include "imgui_keysetup.h"
#include "imgui_winbolonet.h"
}

/* Frame-rate / zoom constants (mirrors winbolo.h values) */
#ifndef FRAME_RATE_10
#define FRAME_RATE_10  19
#define FRAME_RATE_12  23
#define FRAME_RATE_15  28
#define FRAME_RATE_20  37
#define FRAME_RATE_30  55
#define FRAME_RATE_50  82
#define FRAME_RATE_60  111
#endif
#ifndef ZOOM_FACTOR_NORMAL
#define ZOOM_FACTOR_NORMAL 1
#define ZOOM_FACTOR_DOUBLE 2
#define ZOOM_FACTOR_QUAD   4
#endif

/* Globals from winbolo.c */
extern "C" {
  extern bool showGunsight;
  extern bool autoScrollingEnabled;
  extern bool showPillLabels;
  extern bool showBaseLabels;
  extern bool hideMainView;
  extern int  frameRate;
  extern labelLen labelMsg;
  extern labelLen labelTank;
  extern bool labelSelf;
  extern BYTE zoomFactor;
  extern bool soundEffects;
  extern bool backgroundSound;
  extern bool useSoundKeepalive;
  extern bool showNewswireMessages;
  extern bool showAssistantMessages;
  extern bool showAIMessages;
  extern bool showNetworkStatusMessages;
  extern bool showNetworkDebugMessages;

  void windowSetFrameRate(int newFrameRate, bool setTimer);
  void windowAutomaticScrolling_toggle(struct ClientSim *cs);
  void windowShowGunsight_toggle(struct ClientSim *cs);
  void windowShowPillLabels_toggle(struct ClientSim *cs);
  void windowShowBaseLabels_toggle(struct ClientSim *cs);
  void windowSoundEffects_toggle(void);
  void windowBackgroundSoundChange_toggle(void);
  void windowSoundKeepalive(void);
  void windowMenuNewswire_toggle(struct ClientSim *cs);
  void windowMenuAssistant_toggle(struct ClientSim *cs);
  void windowMenuAI_toggle(struct ClientSim *cs);
  void windowMenuNetwork_toggle(struct ClientSim *cs);
  void windowMenuNetworkDebug_toggle(struct ClientSim *cs);
  void windowHideMainView_toggle(void);
  void windowLabelOwnTank_toggle(struct ClientSim *cs);
  void windowSetMessageLabelLen(struct ClientSim *cs, labelLen newLen);
  void windowSetTankLabelLen(struct ClientSim *cs, labelLen newLen);

#if defined(__IPHONEOS__)
  bool iosCrashReportingGetEnabled(void);
  void iosCrashReportingSetEnabled(bool enabled);
#endif
}

/* Try to find a theme's tank sprite file.  Returns true on success and
 * fills outPath with the resolved path (.svg or .png).  themeName ==
 * NULL or "" means the default (data/svg/<base>).  Falls through to
 * data/svg/ if the theme is missing the asset. */
static bool findThemeAsset(const char *themeName, const char *base,
                           char *outPath, size_t outSize) {
    auto tryPath = [&](const char *fmt, const char *a, const char *b,
                       const char *ext) -> bool {
        snprintf(outPath, outSize, fmt, a, b, ext);
        SDL_PathInfo info;
        return SDL_GetPathInfo(outPath, &info);
    };
    if (themeName && themeName[0]) {
        if (tryPath("data/theme/%s/%s.%s", themeName, base, "svg")) return true;
        if (tryPath("data/theme/%s/%s.%s", themeName, base, "png")) return true;
    }
    /* Fallback to default svg/png. */
    snprintf(outPath, outSize, "data/svg/%s.svg", base);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(outPath, &info)) return true;
    snprintf(outPath, outSize, "data/svg/%s.png", base);
    return SDL_GetPathInfo(outPath, &info);
}

/* Rasterize an SVG to an SDL_Texture at the given size.  Returns NULL
 * if path doesn't end in .svg or load fails. */
static SDL_Texture *loadSvgToTexture(SDL_Renderer *r, const char *path, int size) {
    if (!path) return nullptr;
    size_t n = strlen(path);
    if (n < 4 || strcmp(path + n - 4, ".svg") != 0) return nullptr;
    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) return nullptr;
    if (image->width < 1.0f || image->height < 1.0f) { nsvgDelete(image); return nullptr; }
    float scale = (float)size / image->width;
    if (image->height * scale > (float)size) scale = (float)size / image->height;
    int w = size, h = size;
    unsigned char *pixels = (unsigned char *)SDL_malloc((size_t)(w * h * 4));
    if (!pixels) { nsvgDelete(image); return nullptr; }
    memset(pixels, 0, (size_t)(w * h * 4));
    float offX = ((float)w - image->width  * scale) * 0.5f;
    float offY = ((float)h - image->height * scale) * 0.5f;
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, image, offX, offY, scale, pixels, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) { SDL_free(pixels); return nullptr; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surface);
    SDL_DestroySurface(surface);
    SDL_free(pixels);
    if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    return tex;
}

/* True when the theme dir has only the _00 sprite for tanks (i.e. an
 * "_ingamerotate" theme), so previews must rotate the single sprite
 * to show all directions. */
static bool themeIsIngamerotate(const char *themeName) {
    if (!themeName || !themeName[0]) return false;
    /* Same convention as tileLoaderThemeRotates. */
    const char *suffix = "_ingamerotate";
    size_t n = strlen(themeName), s = strlen(suffix);
    return n >= s && strcmp(themeName + n - s, suffix) == 0;
}

/* Set by imguiSettingsResetGraphicsSelection to make the next draw of
 * the Graphics section snap the preview index back to the active
 * theme (curThemeIdx). */
static bool s_resetPreviewOnNextDraw = false;

extern "C" void imguiSettingsResetGraphicsSelection(void) {
    s_resetPreviewOnNextDraw = true;
}

extern "C" void imguiSettingsDrawGraphicsSection(struct SDL_Renderer *rendererArg) {
    SDL_Renderer *renderer = (SDL_Renderer *)rendererArg;
    if (!ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    /* Theme picker — scan data/theme/* once per dialog open. */
    static std::vector<std::string> themeDirs;
    static bool themesScanned = false;
    if (!themesScanned) {
        themesScanned = true;
        themeDirs.push_back("(default)");
#ifdef _WIN32
        WIN32_FIND_DATAA findData;
        HANDLE h = FindFirstFileA("data\\theme\\*", &findData);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    && strcmp(findData.cFileName, ".") != 0
                    && strcmp(findData.cFileName, "..") != 0) {
                    themeDirs.push_back(findData.cFileName);
                }
            } while (FindNextFileA(h, &findData));
            FindClose(h);
        }
#else
        DIR *d = opendir("data/theme");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.') continue;
                char path[1024];
                snprintf(path, sizeof(path), "data/theme/%s", de->d_name);
                struct stat st;
                if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    themeDirs.push_back(de->d_name);
                }
            }
            closedir(d);
        }
#endif
    }

    /* Current selection — match active theme name to list. */
    const char *currentTheme = tileLoaderGetTheme();
    int curThemeIdx = 0;
    for (int i = 0; i < (int)themeDirs.size(); ++i) {
        if (i == 0 && (!currentTheme || !currentTheme[0])) {
            curThemeIdx = 0; break;
        }
        if (currentTheme && currentTheme[0]
            && strcmp(themeDirs[i].c_str(), currentTheme) == 0) {
            curThemeIdx = i; break;
        }
    }

    /* Pending preview selection (Apply commits it). */
    static int s_previewThemeIdx = -1;
    if (s_resetPreviewOnNextDraw) {
        s_previewThemeIdx = curThemeIdx;
        s_resetPreviewOnNextDraw = false;
    }
    if (s_previewThemeIdx < 0) s_previewThemeIdx = curThemeIdx;
    if (s_previewThemeIdx >= (int)themeDirs.size()) s_previewThemeIdx = curThemeIdx;

    ImGui::TextUnformatted("Theme");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##theme", themeDirs[s_previewThemeIdx].c_str())) {
        for (int i = 0; i < (int)themeDirs.size(); ++i) {
            bool selected = (i == s_previewThemeIdx);
            bool active = (i == curThemeIdx);
            char label[256];
            snprintf(label, sizeof(label), "%s%s",
                     active ? "* " : "  ", themeDirs[i].c_str());
            if (ImGui::Selectable(label, selected)) {
                s_previewThemeIdx = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (s_previewThemeIdx != curThemeIdx) {
        ImGui::SameLine();
        if (ImGui::Button("Apply##theme")) {
            const char *newTheme = (s_previewThemeIdx == 0)
                                     ? "" : themeDirs[s_previewThemeIdx].c_str();
            tileLoaderSetTheme(newTheme);
            sdl3DrawReloadTiles();
            /* Persist to INI so the choice survives restart. */
            extern void gameFrontSaveThemeChoice(const char *);
            gameFrontSaveThemeChoice(newTheme);
        }
    }

    /* Animation style. */
    const char *animLabels[] = { "Pixel Floor", "Pixel Nearest", "Smooth (sub-pixel)" };
    int curAnim = (int)gfxSettingsGetAnimStyle();
    ImGui::TextUnformatted("Animation");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##animstyle", animLabels[curAnim])) {
        for (int i = 0; i < 3; ++i) {
            bool sel = (i == curAnim);
            if (ImGui::Selectable(animLabels[i], sel)) {
                gfxSettingsSetAnimStyle((GfxAnimStyle)i);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled(
        "Floor = classic >>4 (default).  Nearest = round to game pixel.\n"
        "Smooth = full sub-pixel; only meaningful with SVG themes.");

    /* Theme preview strip + rotating preview. */
    ImGui::Spacing();
    ImGui::TextUnformatted("Preview");
    SDL_Texture *atlas = sdl3DrawGetTilesTex();
    if (!atlas) {
        ImGui::TextDisabled("(atlas not loaded yet)");
        return;
    }
    float texW = 0.0f, texH = 0.0f;
    SDL_GetTextureSize(atlas, &texW, &texH);
    if (texW <= 0.0f || texH <= 0.0f) return;

    /* Per-cell sprite previews — load each from the SELECTED theme's
     * file on disk so changing the dropdown updates them immediately,
     * even before Apply. */
    struct CellSpec { const char *label; const char *base;
                      int atlasX, atlasY, atlasW, atlasH; };
    static const CellSpec cells[] = {
        { "Self",  "tank_self_00",     TANK_SELF_0_X,     TANK_SELF_0_Y,     TILE_SIZE_X,    TILE_SIZE_Y    },
        { "Good",  "tank_good_00",     TANK_GOOD_0_X,     TANK_GOOD_0_Y,     TILE_SIZE_X,    TILE_SIZE_Y    },
        { "Evil",  "tank_evil_00",     TANK_EVIL_0_X,     TANK_EVIL_0_Y,     TILE_SIZE_X,    TILE_SIZE_Y    },
        { "Boat",  "tank_selfboat_00", TANK_SELFBOAT_0_X, TANK_SELFBOAT_0_Y, TILE_SIZE_X,    TILE_SIZE_Y    },
        { "Shell", "shell_00",         SHELL_0_X,         SHELL_0_Y,         SHELL_0_WIDTH,  SHELL_0_HEIGHT },
        { "LGM",   "lgm_frame0",       LGM0_X,            LGM0_Y,            LGM_WIDTH,      LGM_HEIGHT     },
    };
    const int kNumCells = (int)(sizeof(cells)/sizeof(cells[0]));
    static SDL_Texture *cellTex[6] = { nullptr };
    static int cellLoadedIdx = -1;
    if (cellLoadedIdx != s_previewThemeIdx) {
        cellLoadedIdx = s_previewThemeIdx;
        for (int i = 0; i < kNumCells; i++) {
            if (cellTex[i]) { SDL_DestroyTexture(cellTex[i]); cellTex[i] = nullptr; }
            const char *themeName = (s_previewThemeIdx == 0)
                                      ? "" : themeDirs[s_previewThemeIdx].c_str();
            char path[1024];
            if (findThemeAsset(themeName, cells[i].base, path, sizeof(path))) {
                /* Rasterize at a reasonable internal size — keep aspect
                 * for non-square sprites (shell/LGM). */
                int rasterW = 64, rasterH = 64;
                if (cells[i].atlasW < cells[i].atlasH) {
                    rasterW = (cells[i].atlasW * 64) / cells[i].atlasH;
                } else if (cells[i].atlasH < cells[i].atlasW) {
                    rasterH = (cells[i].atlasH * 64) / cells[i].atlasW;
                }
                int rasterMax = rasterW > rasterH ? rasterW : rasterH;
                cellTex[i] = loadSvgToTexture(renderer, path, rasterMax);
            }
        }
    }

    int previewPx = 32;
    for (int i = 0; i < kNumCells; i++) {
        if (i > 0) ImGui::SameLine();
        ImGui::BeginGroup();
        if (cellTex[i]) {
            ImGui::Image((ImTextureID)(intptr_t)cellTex[i],
                         ImVec2((float)previewPx, (float)previewPx));
        } else {
            /* Fallback: live atlas UV (whatever's currently active). */
            float scaleX = texW / 496.0f;
            float scaleY = texH / 176.0f;
            ImVec2 uv0((cells[i].atlasX * scaleX) / texW,
                       (cells[i].atlasY * scaleY) / texH);
            ImVec2 uv1(((cells[i].atlasX + cells[i].atlasW) * scaleX) / texW,
                       ((cells[i].atlasY + cells[i].atlasH) * scaleY) / texH);
            ImGui::Image((ImTextureID)(intptr_t)atlas,
                         ImVec2((float)previewPx, (float)previewPx),
                         uv0, uv1);
        }
        ImGui::TextUnformatted(cells[i].label);
        ImGui::EndGroup();
    }

    /* Rotating tank preview — loads the SELECTED theme's tank sprites
     * directly from disk.  If the theme has per-direction sprites
     * (tank_self_01..tank_self_15) we cycle through those as discrete
     * frames; if only tank_self_00 exists we rotate that single
     * sprite (matching what the engine does for _ingamerotate themes). */
    if (!renderer) return;
    static SDL_Texture *previewTarget = nullptr;
    static SDL_Texture *previewTankFrames[16] = { nullptr };
    static int  previewLoadedIdx = -1;
    static bool previewHasPerDir = false;  /* true if frames[1..] are loaded */
    static double rotPreview = 0.0;
    static Uint64 rotLastMs = 0;
    const int previewSize    = 96;
    const int tankRasterSize = 64;

    /* Reload tank textures when selection changes. */
    if (previewLoadedIdx != s_previewThemeIdx) {
        previewLoadedIdx = s_previewThemeIdx;
        for (int i = 0; i < 16; i++) {
            if (previewTankFrames[i]) { SDL_DestroyTexture(previewTankFrames[i]); previewTankFrames[i] = nullptr; }
        }
        previewHasPerDir = false;
        const char *themeName = (s_previewThemeIdx == 0)
                                  ? ""  /* default */
                                  : themeDirs[s_previewThemeIdx].c_str();
        char path[1024];
        for (int i = 0; i < 16; i++) {
            char base[32];
            snprintf(base, sizeof(base), "tank_self_%02d", i);
            if (findThemeAsset(themeName, base, path, sizeof(path))) {
                previewTankFrames[i] = loadSvgToTexture(renderer, path, tankRasterSize);
            }
            if (i > 0 && previewTankFrames[i]) previewHasPerDir = true;
        }
    }

    if (!previewTarget) {
        previewTarget = SDL_CreateTexture(renderer,
            SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            previewSize, previewSize);
        if (previewTarget) {
            SDL_SetTextureBlendMode(previewTarget, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(previewTarget, SDL_SCALEMODE_NEAREST);
        }
    }
    if (previewTarget) {
        Uint64 nowMs = SDL_GetTicks();
        if (rotLastMs > 0) {
            double dtSec = (double)(nowMs - rotLastMs) / 1000.0;
            rotPreview += dtSec * 45.0;
            if (rotPreview >= 360.0) rotPreview -= 360.0;
        }
        rotLastMs = nowMs;
        SDL_Texture *prevTarget = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, previewTarget);
        SDL_SetRenderDrawColor(renderer, 30, 30, 36, 255);
        SDL_RenderClear(renderer);
        float dstSize = (float)previewSize * 0.7f;
        SDL_FRect dstR = {
            ((float)previewSize - dstSize) * 0.5f,
            ((float)previewSize - dstSize) * 0.5f,
            dstSize, dstSize
        };
        SDL_FPoint pivot = { dstSize * 0.5f, dstSize * 0.5f };
        if (previewHasPerDir) {
            /* Cycle through the 16 per-direction sprites at the same
             * angular rate (22.5° per frame).  No rotation applied —
             * this is what the in-game render does. */
            int frameIdx = ((int)(rotPreview / 22.5)) & 15;
            SDL_Texture *frameTex = previewTankFrames[frameIdx];
            if (!frameTex) frameTex = previewTankFrames[0];
            if (frameTex) {
                SDL_RenderTexture(renderer, frameTex, NULL, &dstR);
            }
        } else if (previewTankFrames[0]) {
            /* Only _00 exists (ingamerotate-style): rotate it.  Step
             * in 22.5° increments to match the in-game render, which
             * bakes 16 discrete sprites at sheet build time and never
             * rotates at runtime. */
            int frameIdx = ((int)(rotPreview / 22.5)) & 15;
            double quantizedAngle = (double)frameIdx * 22.5;
            SDL_RenderTextureRotated(renderer, previewTankFrames[0], NULL, &dstR,
                                     quantizedAngle, &pivot, SDL_FLIP_NONE);
        } else {
            /* Theme has no tank_00 file — fall back to the live atlas. */
            float ascaleX = texW / 496.0f;
            float ascaleY = texH / 176.0f;
            SDL_FRect srcR = { (float)TANK_SELF_0_X * ascaleX,
                               (float)TANK_SELF_0_Y * ascaleY,
                               (float)TILE_SIZE_X   * ascaleX,
                               (float)TILE_SIZE_Y   * ascaleY };
            SDL_RenderTextureRotated(renderer, atlas, &srcR, &dstR,
                                     rotPreview, &pivot, SDL_FLIP_NONE);
        }
        SDL_SetRenderTarget(renderer, prevTarget);

        ImGui::Spacing();
        ImGui::TextUnformatted("Rotating preview:");
        ImGui::Image((ImTextureID)(intptr_t)previewTarget,
                     ImVec2((float)previewSize, (float)previewSize));
        ImGui::SameLine();
        if (previewHasPerDir) {
            ImGui::TextDisabled(
                "Cycling through tank_self_00..15 from the\n"
                "selected theme — exactly what the in-game\n"
                "render uses (one sprite per direction, no\n"
                "rotation).");
        } else if (previewTankFrames[0]) {
            ImGui::TextDisabled(
                "Only tank_self_00 in this theme — rotated in\n"
                "22.5° steps.  In-game the 16 directional sprites\n"
                "are baked from _00 at sheet build, so this is\n"
                "exactly what each facing direction looks like.");
        } else {
            ImGui::TextDisabled(
                "(tank_self_00 not found — falling back\n"
                "to live atlas)");
        }
    }
}

extern "C" void imguiSettingsShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return;
    /* Snap dropdown back to the active theme on every fresh open. */
    imguiSettingsResetGraphicsSelection();

    /* Save logical presentation */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

#if !BOLO_MOBILE
    /* Keep the window at the same size as the welcome dialog */
    dialogSetWindowSize(window, 1024, 768);
    dialogSetWindowTitle(window, "WinBolo - Settings");
    SDL_SetWindowResizable(window, true);
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    /* Load current player name */
    char playerName[FILENAME_MAX];
    playerName[0] = '\0';
    gameFrontGetPlayerName(playerName);

    /* Background game */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);
    Uint64 lastTickTime = SDL_GetTicks();

    /* Initialise WBN popup state and kick off token validation */
    imguiWinbolonetReset();
    imguiWinbolonetStartValidation();

    bool running = true;
#if !BOLO_MOBILE
    bool showKeySetup = false;
#endif

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
        ImGui::Begin("##SettingsBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Centered overlay panel */
        float panelW = 500.0f * s, panelH = 580.0f * s;
        if (panelW > (float)winW * 0.95f) panelW = (float)winW * 0.95f;
        if (panelH > (float)winH * 0.95f) panelH = (float)winH * 0.95f;

        ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##SettingsPanel", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        /* Title */
        {
            ImGui::SetWindowFontScale(1.4f);
            const char *title = "Settings";
            ImVec2 textSize = ImGui::CalcTextSize(title);
            ImGui::SetCursorPosX((panelW - textSize.x) * 0.5f);
            ImGui::Text("%s", title);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* ---- Player ---- */
        if (ImGui::CollapsingHeader("Player", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool wbnActive = gameFrontGetWinbolonetUse();
            if (wbnActive) {
                /* Refresh local buffer from gameFront in case WBN login just set it */
                gameFrontGetPlayerName(playerName);
            }
            ImGui::Text("Player Name:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            if (wbnActive) ImGui::BeginDisabled();
            if (ImGui::InputText("##playerName", playerName, 33,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                playerName[32] = '\0';
                if (playerName[0] != '\0') {
                    gameFrontSetPlayerName(playerName);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply##name")) {
                playerName[32] = '\0';
                if (playerName[0] != '\0') {
                    gameFrontSetPlayerName(playerName);
                }
            }
            if (wbnActive) ImGui::EndDisabled();

            ImGui::Spacing();
            imguiWinbolonetDrawSection(false);
        }

        /* ---- Display ---- */
        if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char *frLabels[] = { "60", "50", "30", "20", "15", "12", "10" };
            int frValues[] = { FRAME_RATE_60, FRAME_RATE_50, FRAME_RATE_30,
                               FRAME_RATE_20, FRAME_RATE_15, FRAME_RATE_12, FRAME_RATE_10 };
            int curFrIdx = 2;
            for (int i = 0; i < 7; i++) {
                if (frameRate == frValues[i]) { curFrIdx = i; break; }
            }
            ImGui::Text("Frame Rate:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (ImGui::BeginCombo("##framerate", frLabels[curFrIdx])) {
                for (int i = 0; i < 7; i++) {
                    bool selected = (curFrIdx == i);
                    if (ImGui::Selectable(frLabels[i], selected)) {
                        windowSetFrameRate(frValues[i], false);
                    }
                }
                ImGui::EndCombo();
            }

#if !BOLO_MOBILE
            {
                bool as = (bool)autoScrollingEnabled;
                if (ImGui::Checkbox("Automatic Scrolling", &as)) {
                    windowAutomaticScrolling_toggle(NULL);
                }
            }
            {
                bool gs = (bool)showGunsight;
                if (ImGui::Checkbox("Show Gunsight", &gs)) {
                    /* Pre-game: just toggle the global directly */
                    showGunsight = !showGunsight;
                }
            }
            ImGui::Spacing();
            if (ImGui::Button("Set Keys...", ImVec2(120, 0))) {
                showKeySetup = true;
            }
#endif
        }

        /* ---- Labels ---- */
        if (ImGui::CollapsingHeader("Labels", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Message Names:");
            ImGui::SameLine();
            {
                bool isShort = (labelMsg == lblShort);
                if (ImGui::RadioButton("Short##msg", isShort)) windowSetMessageLabelLen(NULL, lblShort);
                ImGui::SameLine();
                if (ImGui::RadioButton("Long##msg", !isShort)) windowSetMessageLabelLen(NULL, lblLong);
            }

            ImGui::Text("Tank Labels:");
            ImGui::SameLine();
            {
                if (ImGui::RadioButton("None##tank", labelTank == lblNone))  windowSetTankLabelLen(NULL, lblNone);
                ImGui::SameLine();
                if (ImGui::RadioButton("Short##tank", labelTank == lblShort)) windowSetTankLabelLen(NULL, lblShort);
                ImGui::SameLine();
                if (ImGui::RadioButton("Long##tank", labelTank == lblLong))  windowSetTankLabelLen(NULL, lblLong);
            }
            {
                bool noSelf = !(bool)labelSelf;
                if (ImGui::Checkbox("Don't label own tank", &noSelf)) {
                    windowLabelOwnTank_toggle(NULL);
                }
            }
            {
                bool pl = (bool)showPillLabels;
                if (ImGui::Checkbox("Pillbox Labels", &pl)) {
                    showPillLabels = !showPillLabels;
                }
            }
            {
                bool bl = (bool)showBaseLabels;
                if (ImGui::Checkbox("Refuelling Base Labels", &bl)) {
                    showBaseLabels = !showBaseLabels;
                }
            }
        }

        /* ---- Graphics ---- */
        imguiSettingsDrawGraphicsSection(renderer);
#if 0  /* moved into imguiSettingsDrawGraphicsSection() */
        if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
            /* Theme picker — scan data/theme/* once per dialog open. */
            static std::vector<std::string> themeDirs;
            static bool themesScanned = false;
            if (!themesScanned) {
                themesScanned = true;
                themeDirs.push_back("(default)");
#ifdef _WIN32
                WIN32_FIND_DATAA findData;
                HANDLE h = FindFirstFileA("data\\theme\\*", &findData);
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                            && strcmp(findData.cFileName, ".") != 0
                            && strcmp(findData.cFileName, "..") != 0) {
                            themeDirs.push_back(findData.cFileName);
                        }
                    } while (FindNextFileA(h, &findData));
                    FindClose(h);
                }
#else
                DIR *d = opendir("data/theme");
                if (d) {
                    struct dirent *de;
                    while ((de = readdir(d))) {
                        if (de->d_name[0] == '.') continue;
                        char path[1024];
                        snprintf(path, sizeof(path), "data/theme/%s", de->d_name);
                        struct stat st;
                        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                            themeDirs.push_back(de->d_name);
                        }
                    }
                    closedir(d);
                }
#endif
            }

            /* Current selection — match active theme name to list. */
            const char *currentTheme = tileLoaderGetTheme();
            int curThemeIdx = 0;
            for (int i = 0; i < (int)themeDirs.size(); ++i) {
                if (i == 0 && (!currentTheme || !currentTheme[0])) {
                    curThemeIdx = 0; break;
                }
                if (currentTheme && currentTheme[0]
                    && strcmp(themeDirs[i].c_str(), currentTheme) == 0) {
                    curThemeIdx = i; break;
                }
            }

            /* Pending preview selection (Apply commits it). */
            static int s_previewThemeIdx = -1;
            if (s_previewThemeIdx < 0) s_previewThemeIdx = curThemeIdx;
            if (s_previewThemeIdx >= (int)themeDirs.size()) s_previewThemeIdx = curThemeIdx;

            ImGui::TextUnformatted("Theme");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("##theme", themeDirs[s_previewThemeIdx].c_str())) {
                for (int i = 0; i < (int)themeDirs.size(); ++i) {
                    bool selected = (i == s_previewThemeIdx);
                    bool active = (i == curThemeIdx);
                    /* Mark active with a leading dot so the user knows
                     * which one is currently in effect, vs. the one
                     * being previewed. */
                    char label[256];
                    snprintf(label, sizeof(label), "%s%s",
                             active ? "* " : "  ", themeDirs[i].c_str());
                    if (ImGui::Selectable(label, selected)) {
                        s_previewThemeIdx = i;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            /* Apply button: visible when preview differs from active. */
            if (s_previewThemeIdx != curThemeIdx) {
                ImGui::SameLine();
                if (ImGui::Button("Apply##theme")) {
                    if (s_previewThemeIdx == 0) {
                        tileLoaderSetTheme("");
                    } else {
                        tileLoaderSetTheme(themeDirs[s_previewThemeIdx].c_str());
                    }
                    sdl3DrawReloadTiles();
                }
            }

            /* Animation style. */
            const char *animLabels[] = { "Pixel Floor", "Pixel Nearest", "Smooth (sub-pixel)" };
            int curAnim = (int)gfxSettingsGetAnimStyle();
            ImGui::TextUnformatted("Animation");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("##animstyle", animLabels[curAnim])) {
                for (int i = 0; i < 3; ++i) {
                    bool sel = (i == curAnim);
                    if (ImGui::Selectable(animLabels[i], sel)) {
                        gfxSettingsSetAnimStyle((GfxAnimStyle)i);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled(
                "Floor = classic >>4 (default).  Nearest = round to game pixel.\n"
                "Smooth = full sub-pixel; only meaningful with SVG themes.");

            /* Theme preview — render a strip of N-facing tank/shell
             * sprites from the live atlas so the user sees the
             * effect of the active theme.  Atlas constants come from
             * tile.bmp's classic layout (see src/gui/tiles.h). */
            ImGui::Spacing();
            ImGui::TextUnformatted("Preview");
            SDL_Texture *atlas = sdl3DrawGetTilesTex();
            if (atlas) {
                /* sheet is 496x176 (1x atlas) or scaled up; we use UV
                 * fractions so it works at any sheet scale. */
                float texW = 0.0f, texH = 0.0f;
                SDL_GetTextureSize(atlas, &texW, &texH);
                if (texW > 0.0f && texH > 0.0f) {
                    /* (atlasX, atlasY, w, h) for each preview sprite,
                     * in 1x atlas pixels.  These match the constants
                     * in src/gui/tiles.h. */
                    struct { const char *label; int x, y, w, h; } previews[] = {
                        { "Self",   400, 32, 16, 16 },   /* TANK_SELF_0  */
                        { "Good",   464, 32, 16, 16 },   /* TANK_GOOD_0  */
                        { "Evil",   336, 48, 16, 16 },   /* TANK_EVIL_0  */
                        { "Boat",    64,  0, 16, 16 },   /* boat0        */
                        { "Shell",  452, 72,  3,  4 },   /* SHELL_0      */
                        { "LGM",    431, 90,  3,  4 },   /* LGM0         */
                    };
                    /* The atlas may be at sheet-scale > 1 — scale UV
                     * accordingly via the texture's native size. */
                    int previewPx = 32;  /* on-screen size of each cell */
                    for (size_t i = 0; i < sizeof(previews)/sizeof(previews[0]); ++i) {
                        if (i > 0) ImGui::SameLine();
                        ImGui::BeginGroup();
                        /* Atlas is at sheet scale.  Source UV is in
                         * texture-coordinate space [0..1].  Multiply
                         * 1x atlas coords by (texSize / 496|176). */
                        float scaleX = texW / 496.0f;
                        float scaleY = texH / 176.0f;
                        ImVec2 uv0((previews[i].x * scaleX) / texW,
                                   (previews[i].y * scaleY) / texH);
                        ImVec2 uv1(((previews[i].x + previews[i].w) * scaleX) / texW,
                                   ((previews[i].y + previews[i].h) * scaleY) / texH);
                        ImGui::Image((ImTextureID)(intptr_t)atlas,
                                     ImVec2((float)previewPx, (float)previewPx),
                                     uv0, uv1);
                        ImGui::TextUnformatted(previews[i].label);
                        ImGui::EndGroup();
                    }

                    /* ---- Rotating tank preview (ingamerotate-style) ----
                     * Render the N-facing TANK_SELF_0 sprite onto a small
                     * SDL_Texture target each frame, with rotation, and
                     * display via ImGui::Image. */
                    static SDL_Texture *previewTarget = nullptr;
                    static double rotPreview = 0.0;
                    static Uint64 rotLastMs = 0;
                    const int previewSize = 96;
                    if (!previewTarget) {
                        previewTarget = SDL_CreateTexture(renderer,
                            SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                            previewSize, previewSize);
                        if (previewTarget) {
                            SDL_SetTextureBlendMode(previewTarget, SDL_BLENDMODE_BLEND);
                            SDL_SetTextureScaleMode(previewTarget, SDL_SCALEMODE_NEAREST);
                        }
                    }
                    if (previewTarget) {
                        Uint64 nowMs = SDL_GetTicks();
                        if (rotLastMs > 0) {
                            double dtSec = (double)(nowMs - rotLastMs) / 1000.0;
                            rotPreview += dtSec * 45.0;  /* 45 deg/sec */
                            if (rotPreview >= 360.0) rotPreview -= 360.0;
                        }
                        rotLastMs = nowMs;
                        SDL_Texture *prevTarget = SDL_GetRenderTarget(renderer);
                        SDL_SetRenderTarget(renderer, previewTarget);
                        SDL_SetRenderDrawColor(renderer, 30, 30, 36, 255);
                        SDL_RenderClear(renderer);
                        /* Source: TANK_SELF_0 atlas region. */
                        float ascaleX = texW / 496.0f;
                        float ascaleY = texH / 176.0f;
                        SDL_FRect srcR = { 400.0f * ascaleX, 32.0f * ascaleY,
                                           16.0f * ascaleX, 16.0f * ascaleY };
                        /* Destination: centred in target. */
                        float dstSize = (float)previewSize * 0.7f;
                        SDL_FRect dstR = {
                            ((float)previewSize - dstSize) * 0.5f,
                            ((float)previewSize - dstSize) * 0.5f,
                            dstSize, dstSize
                        };
                        SDL_FPoint pivot = { dstSize * 0.5f, dstSize * 0.5f };
                        SDL_RenderTextureRotated(renderer, atlas, &srcR, &dstR,
                                                 rotPreview, &pivot, SDL_FLIP_NONE);
                        SDL_SetRenderTarget(renderer, prevTarget);

                        ImGui::Spacing();
                        ImGui::TextUnformatted("Rotating preview:");
                        ImGui::Image((ImTextureID)(intptr_t)previewTarget,
                                     ImVec2((float)previewSize, (float)previewSize));
                        ImGui::SameLine();
                        ImGui::TextDisabled(
                            "Live preview of TANK_SELF_0 from the\n"
                            "active theme, rotated continuously.\n"
                            "With stock_svg_ingamerotate this is\n"
                            "what every facing direction will look\n"
                            "like in-game.");
                    }
                }
            } else {
                ImGui::TextDisabled("(atlas not loaded yet)");
            }
        }
#endif /* moved into imguiSettingsDrawGraphicsSection() */

        /* ---- Tutorial ---- */
        if (ImGui::CollapsingHeader("Tutorial", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Play Tutorial", ImVec2(140, 0))) {
                gameFrontRequestPlayTutorial();
                running = false;  /* Close settings; openSettings handler routes to openTutorial. */
            }
            ImGui::SameLine();
            {
                bool showOnMain = gameFrontGetShowTutorialButton();
                if (ImGui::Checkbox("Show on main menu", &showOnMain)) {
                    gameFrontSetShowTutorialButton(showOnMain);
                }
            }
        }

        /* ---- Sound ---- */
        if (ImGui::CollapsingHeader("Sound", ImGuiTreeNodeFlags_DefaultOpen)) {
            {
                bool se = (bool)soundEffects;
                if (ImGui::Checkbox("Sound Effects", &se)) {
                    windowSoundEffects_toggle();
                }
            }
            {
                bool bgs = (bool)backgroundSound;
                if (ImGui::Checkbox("Background Sound", &bgs)) {
                    windowBackgroundSoundChange_toggle();
                }
            }
#if !BOLO_MOBILE
            {
                bool sk = (bool)useSoundKeepalive;
                if (ImGui::Checkbox("Sound Keepalive", &sk)) {
                    windowSoundKeepalive();
                }
            }
#endif
        }

        /* ---- Messages ---- */
        if (ImGui::CollapsingHeader("Messages", ImGuiTreeNodeFlags_DefaultOpen)) {
            {
                bool nw = (bool)showNewswireMessages;
                if (ImGui::Checkbox("Newswire Messages", &nw)) {
                    windowMenuNewswire_toggle(NULL);
                }
            }
            {
                bool am = (bool)showAssistantMessages;
                if (ImGui::Checkbox("Assistant Messages", &am)) {
                    windowMenuAssistant_toggle(NULL);
                }
            }
            {
                bool ai = (bool)showAIMessages;
                if (ImGui::Checkbox("AI Brain Messages", &ai)) {
                    windowMenuAI_toggle(NULL);
                }
            }
            {
                bool ns = (bool)showNetworkStatusMessages;
                if (ImGui::Checkbox("Network Status Messages", &ns)) {
                    windowMenuNetwork_toggle(NULL);
                }
            }
            {
                bool nd = (bool)showNetworkDebugMessages;
                if (ImGui::Checkbox("Network Debug Messages", &nd)) {
                    windowMenuNetworkDebug_toggle(NULL);
                }
            }
        }

#if defined(__IPHONEOS__)
        /* ---- Crash Reporting ---- */
        if (ImGui::CollapsingHeader("Crash Reporting", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool cr = iosCrashReportingGetEnabled();
            if (ImGui::Checkbox("Enable Crash Reporting", &cr)) {
                iosCrashReportingSetEnabled(cr);
            }
            ImGui::TextWrapped("Help improve WinBolo by sending crash reports");
            ImGui::Spacing();
            ImGui::TextDisabled("Changes take effect on next launch.");
        }
#endif

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btnW = 80.0f;
        float btnX = (panelW - btnW) / 2.0f;
        ImGui::SetCursorPosX(btnX);
        if (ImGui::Button("Close", ImVec2(btnW, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            running = false;
        }

        ImGui::End(); /* ##SettingsPanel */
        ImGui::End(); /* ##SettingsBg */

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

#if !BOLO_MOBILE
        if (showKeySetup) {
            showKeySetup = false;

            /* Tear down current ImGui context */
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();

            /* Run the key setup dialog (blocking) */
            imguiKeySetupShow();

            /* Re-create ImGui context for the settings loop */
            SDL_GetWindowSize(window, &screenW, &screenH);
            if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
            s = dialogComputeScale(screenW, screenH);

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO &ioNew = ImGui::GetIO();
            ioNew.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ioNew.IniFilename = nullptr;

            ImGui::StyleColorsDark();
            imguiApplyBoloTheme();
            ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
            ImGui_ImplSDLRenderer3_Init(renderer);
            dialogApplyScaling(s);

            dialogSetWindowSize(window, 1024, 768);
            dialogSetWindowTitle(window, "WinBolo - Settings");

            lastTickTime = SDL_GetTicks();
        }
#endif
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
}
