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
#include "../../gamefront.h"
#include "../../../bolo/global.h"
#include "../../../bolo/screen.h"
#include "../bg_game.h"
#include "../../lang.h"
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
  extern bool smoothScrollingEnabled;
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
  void windowSmoothScrolling_toggle(void);
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

/* Cached language-info icon. The SDL renderer is process-lifetime, so the
 * texture stays valid between settings opens. */
static SDL_Texture *s_langInfoIcon = nullptr;
static bool s_langInfoIconAttempted = false;

static SDL_Texture *loadSvgIcon(SDL_Renderer *rend, const char *path, int size) {
    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) return nullptr;
    if (image->width < 1.0f || image->height < 1.0f) { nsvgDelete(image); return nullptr; }
    float scale = (float)size / image->height;
    if (image->width * scale > (float)size) scale = (float)size / image->width;
    int w = size, h = size;
    unsigned char *pixels = (unsigned char *)SDL_malloc((size_t)(w * h * 4));
    if (!pixels) { nsvgDelete(image); return nullptr; }
    memset(pixels, 0, (size_t)(w * h * 4));
    float offX = ((float)w - image->width * scale) * 0.5f;
    float offY = ((float)h - image->height * scale) * 0.5f;
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, image, offX, offY, scale, pixels, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) { SDL_free(pixels); return nullptr; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surface);
    SDL_DestroySurface(surface);
    SDL_free(pixels);
    return tex;
}

extern "C" void imguiSettingsShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return;

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
    dialogSetWindowTitle(window, langGetText(STR_DLGSETTINGS_WINTITLE));
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

    /* Lazily rasterise the info icon at a size that suits the dropdown row. */
    if (!s_langInfoIcon && !s_langInfoIconAttempted) {
        s_langInfoIconAttempted = true;
        int iconPx = (int)(20.0f * s);
        if (iconPx < 16) iconPx = 16;
        s_langInfoIcon = loadSvgIcon(renderer, "data/ui/dialog-info.svg", iconPx);
        if (!s_langInfoIcon) {
            char basePathBuf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(basePathBuf, sizeof(basePathBuf),
                             "%sdata/ui/dialog-info.svg", base);
                s_langInfoIcon = loadSvgIcon(renderer, basePathBuf, iconPx);
            }
        }
    }

    /* Scan available translations once for the language dropdown.
     * Re-scanning every frame would hit the disk on every redraw; the
     * picker is local to this dialog so a fresh scan on next open is
     * sufficient if a translator drops a new file in lang/. */
    int            langCount = 0;
    LangFileEntry *langEntries = langPickerScan(&langCount);
    int            curLangIdx = 0;
    {
        char curCode[32];
        curCode[0] = '\0';
        gameFrontGetLanguageCode(curCode, (int)sizeof(curCode));
        if (curCode[0] == '\0') {
            curLangIdx = 0;  /* English baseline */
        } else {
            for (int i = 0; i < langCount; i++) {
                if (strcmp(curCode, langEntries[i].code) == 0) {
                    curLangIdx = i;
                    break;
                }
            }
        }
    }

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
            const char *title = langGetText(STR_DLGSETTINGS_TITLE);
            ImVec2 textSize = ImGui::CalcTextSize(title);
            ImGui::SetCursorPosX((panelW - textSize.x) * 0.5f);
            ImGui::Text("%s", title);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* ---- Player ---- */
        if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_PLAYER), ImGuiTreeNodeFlags_DefaultOpen)) {
            bool wbnActive = gameFrontGetWinbolonetUse();
            if (wbnActive) {
                /* Refresh local buffer from gameFront in case WBN login just set it */
                gameFrontGetPlayerName(playerName);
            }
            ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_PLAYERNAME));
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
            {
                char applyBuf[64];
                snprintf(applyBuf, sizeof(applyBuf), "%s##name", langGetText(STR_DLGSETTINGS_APPLY));
                if (ImGui::Button(applyBuf)) {
                    playerName[32] = '\0';
                    if (playerName[0] != '\0') {
                        gameFrontSetPlayerName(playerName);
                    }
                }
            }
            if (wbnActive) ImGui::EndDisabled();

            ImGui::Spacing();
            imguiWinbolonetDrawSection(false);
        }

        /* ---- Display ---- */
        if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_DISPLAY), ImGuiTreeNodeFlags_DefaultOpen)) {
            const char *frLabels[] = { "60", "50", "30", "20", "15", "12", "10" };
            int frValues[] = { FRAME_RATE_60, FRAME_RATE_50, FRAME_RATE_30,
                               FRAME_RATE_20, FRAME_RATE_15, FRAME_RATE_12, FRAME_RATE_10 };
            int curFrIdx = 2;
            for (int i = 0; i < 7; i++) {
                if (frameRate == frValues[i]) { curFrIdx = i; break; }
            }
            ImGui::TextUnformatted(langGetText(STR_DLGSYSINFO_FRAMERATE));
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

            /* ---- Language picker ---- */
            ImGui::Spacing();
            ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_LANGUAGE_LBL));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220);
            const char *curLangLabel =
                (curLangIdx >= 0 && curLangIdx < langCount &&
                 langEntries[curLangIdx].meta.name[0] != '\0')
                    ? langEntries[curLangIdx].meta.name
                    : langGetText(STR_DLGLANG_NAME);
            if (ImGui::BeginCombo("##language", curLangLabel)) {
                for (int i = 0; i < langCount; i++) {
                    const char *itemLabel =
                        (langEntries[i].meta.name[0] != '\0')
                            ? langEntries[i].meta.name
                            : langEntries[i].code;
                    bool selected = (curLangIdx == i);
                    if (ImGui::Selectable(itemLabel, selected)) {
                        if (i == 0) {
                            /* English baseline — drop any loaded override. */
                            langUnloadFile();
                            gameFrontSetLanguageCode("");
                        } else {
                            if (langLoadFile(langEntries[i].path)) {
                                gameFrontSetLanguageCode(langEntries[i].code);
                            }
                        }
                        curLangIdx = i;
                    }
                }
                ImGui::EndCombo();
            }

            /* Info icon next to the combo opens a popup carrying the
             * author / notes / footnotes that used to be inlined below.
             * langGetLoadedMeta() may be non-NULL for a loaded file; prefer
             * that for non-English entries so live re-translation of the
             * meta works. Entry 0's meta is populated from
             * STR_DLGLANG_NAME/AUTHOR/NOTES for the English baseline. */
            ImGui::SameLine();
            float iconH = ImGui::GetFrameHeight();
            ImVec2 iconSz(iconH, iconH);
            bool openInfo = false;
            if (s_langInfoIcon) {
                if (ImGui::ImageButton("##langInfoBtn",
                                       (ImTextureID)s_langInfoIcon,
                                       iconSz)) {
                    openInfo = true;
                }
            } else {
                if (ImGui::SmallButton("?##langInfoBtn")) {
                    openInfo = true;
                }
            }
            if (openInfo) {
                ImGui::OpenPopup("##LangInfoPopup");
            }
            if (ImGui::BeginPopup("##LangInfoPopup")) {
                const LangFileMeta *displayMeta = nullptr;
                if (curLangIdx == 0 || curLangIdx >= langCount) {
                    displayMeta = (langCount > 0) ? &langEntries[0].meta : nullptr;
                } else {
                    const LangFileMeta *loaded = langGetLoadedMeta();
                    displayMeta = loaded ? loaded : &langEntries[curLangIdx].meta;
                }
                float wrapW = 360.0f * s;
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
                if (displayMeta) {
                    ImGui::TextWrapped("%s%s",
                                       langGetText(STR_DLGLANG_AUTHOR_CAPTION),
                                       displayMeta->author);
                    if (displayMeta->notes[0] != '\0') {
                        ImGui::TextWrapped("%s%s",
                                           langGetText(STR_DLGLANG_NOTES_CAPTION),
                                           displayMeta->notes);
                    }
                }
                ImGui::Separator();
                ImGui::TextDisabled("%s",
                                    langGetText(STR_DLGLANG_DEFAULTNOTE));
                ImGui::TextDisabled("%s",
                                    langGetText(STR_DLGLANG_MIDGAME_NOTE));
                ImGui::PopTextWrapPos();
                ImGui::EndPopup();
            }

#if !BOLO_MOBILE
            {
                bool as = (bool)autoScrollingEnabled;
                if (ImGui::Checkbox(langGetText(STR_MENU_AUTO_SCROLLING), &as)) {
                    windowAutomaticScrolling_toggle(NULL);
                }
            }
            {
                bool ss = (bool)smoothScrollingEnabled;
                if (ImGui::Checkbox(langGetText(STR_MENU_SMOOTH_SCROLLING), &ss)) {
                    windowSmoothScrolling_toggle();
                }
            }
            {
                bool gs = (bool)showGunsight;
                if (ImGui::Checkbox(langGetText(STR_MENU_SHOW_GUNSIGHT), &gs)) {
                    /* Pre-game: just toggle the global directly */
                    showGunsight = !showGunsight;
                }
            }
            ImGui::Spacing();
            if (ImGui::Button(langGetText(STR_DLGSETTINGS_SETKEYS), ImVec2(120, 0))) {
                showKeySetup = true;
            }
#endif
        }

        /* ---- Labels ---- */
        if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_LABELS), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_MSGNAMES));
            ImGui::SameLine();
            {
                bool isShort = (labelMsg == lblShort);
                char shortBuf[64], longBuf[64];
                snprintf(shortBuf, sizeof(shortBuf), "%s##msg", langGetText(STR_SHORT));
                snprintf(longBuf,  sizeof(longBuf),  "%s##msg", langGetText(STR_LONG));
                if (ImGui::RadioButton(shortBuf, isShort)) windowSetMessageLabelLen(NULL, lblShort);
                ImGui::SameLine();
                if (ImGui::RadioButton(longBuf,  !isShort)) windowSetMessageLabelLen(NULL, lblLong);
            }

            ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_TANKLABELS));
            ImGui::SameLine();
            {
                char noneBuf[64], shortBuf[64], longBuf[64];
                snprintf(noneBuf,  sizeof(noneBuf),  "%s##tank", langGetText(STR_NONE));
                snprintf(shortBuf, sizeof(shortBuf), "%s##tank", langGetText(STR_SHORT));
                snprintf(longBuf,  sizeof(longBuf),  "%s##tank", langGetText(STR_LONG));
                if (ImGui::RadioButton(noneBuf,  labelTank == lblNone))  windowSetTankLabelLen(NULL, lblNone);
                ImGui::SameLine();
                if (ImGui::RadioButton(shortBuf, labelTank == lblShort)) windowSetTankLabelLen(NULL, lblShort);
                ImGui::SameLine();
                if (ImGui::RadioButton(longBuf,  labelTank == lblLong))  windowSetTankLabelLen(NULL, lblLong);
            }
            {
                bool noSelf = !(bool)labelSelf;
                if (ImGui::Checkbox(langGetText(STR_MENU_NO_OWN_LABEL), &noSelf)) {
                    windowLabelOwnTank_toggle(NULL);
                }
            }
            {
                bool pl = (bool)showPillLabels;
                if (ImGui::Checkbox(langGetText(STR_MENU_PILLBOX_LABELS), &pl)) {
                    showPillLabels = !showPillLabels;
                }
            }
            {
                bool bl = (bool)showBaseLabels;
                if (ImGui::Checkbox(langGetText(STR_MENU_BASE_LABELS), &bl)) {
                    showBaseLabels = !showBaseLabels;
                }
            }
        }

        /* ---- Tutorial ---- */
        if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_TUTORIAL), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button(langGetText(STR_DLGSETTINGS_PLAY_TUTORIAL), ImVec2(140, 0))) {
                gameFrontRequestPlayTutorial();
                running = false;  /* Close settings; openSettings handler routes to openTutorial. */
            }
            ImGui::SameLine();
            {
                bool showOnMain = gameFrontGetShowTutorialButton();
                if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_SHOW_ON_MAIN), &showOnMain)) {
                    gameFrontSetShowTutorialButton(showOnMain);
                }
            }
        }

        /* ---- Sound ---- */
        if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_SOUND), ImGuiTreeNodeFlags_DefaultOpen)) {
            {
                bool se = (bool)soundEffects;
                if (ImGui::Checkbox(langGetText(STR_MENU_SOUND_EFFECTS), &se)) {
                    windowSoundEffects_toggle();
                }
            }
            {
                bool bgs = (bool)backgroundSound;
                if (ImGui::Checkbox(langGetText(STR_MENU_BACKGROUND_SOUND), &bgs)) {
                    windowBackgroundSoundChange_toggle();
                }
            }
#if !BOLO_MOBILE
            {
                bool sk = (bool)useSoundKeepalive;
                if (ImGui::Checkbox(langGetText(STR_MENU_SOUND_KEEPALIVE), &sk)) {
                    windowSoundKeepalive();
                }
            }
#endif
        }

        /* ---- Messages ---- */
        if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_MESSAGES), ImGuiTreeNodeFlags_DefaultOpen)) {
            {
                bool nw = (bool)showNewswireMessages;
                if (ImGui::Checkbox(langGetText(STR_MENU_NEWSWIRE_MSGS), &nw)) {
                    windowMenuNewswire_toggle(NULL);
                }
            }
            {
                bool am = (bool)showAssistantMessages;
                if (ImGui::Checkbox(langGetText(STR_MENU_ASSISTANT_MSGS), &am)) {
                    windowMenuAssistant_toggle(NULL);
                }
            }
            {
                bool ai = (bool)showAIMessages;
                if (ImGui::Checkbox(langGetText(STR_MENU_AI_MSGS), &ai)) {
                    windowMenuAI_toggle(NULL);
                }
            }
            {
                bool ns = (bool)showNetworkStatusMessages;
                if (ImGui::Checkbox(langGetText(STR_MENU_NETSTATUS_MSGS), &ns)) {
                    windowMenuNetwork_toggle(NULL);
                }
            }
            {
                bool nd = (bool)showNetworkDebugMessages;
                if (ImGui::Checkbox(langGetText(STR_MENU_NETDEBUG_MSGS), &nd)) {
                    windowMenuNetworkDebug_toggle(NULL);
                }
            }
        }

#if defined(__IPHONEOS__)
        /* ---- Crash Reporting ---- */
        if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_CRASH_REPORTING), ImGuiTreeNodeFlags_DefaultOpen)) {
            bool cr = iosCrashReportingGetEnabled();
            if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_ENABLE_CRASH), &cr)) {
                iosCrashReportingSetEnabled(cr);
            }
            ImGui::TextWrapped("%s", langGetText(STR_DLGSETTINGS_CRASH_HELP));
            ImGui::Spacing();
            ImGui::TextDisabled("%s", langGetText(STR_DLGSETTINGS_CRASH_NEXTLAUNCH));
        }
#endif

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btnW = 80.0f;
        float btnX = (panelW - btnW) / 2.0f;
        ImGui::SetCursorPosX(btnX);
        if (ImGui::Button(langGetText(STR_CLOSE), ImVec2(btnW, 0)) ||
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
            dialogSetWindowTitle(window, langGetText(STR_DLGSETTINGS_WINTITLE));

            lastTickTime = SDL_GetTicks();
        }
#endif
    }

    dialogDismissKeyboard(window);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (langEntries) {
        langPickerFreeEntries(langEntries, langCount);
    }

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    SDL_FlushEvent(SDL_EVENT_QUIT);
}
