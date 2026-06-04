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
#include "imgui_nav_outline.h"
#include "dialog_footer.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "../imgui_steam_nav.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../gamefront.h"
#include "global.h"
#include "client_enums.h"  /* labelLen */
#include "playername_validate.h"
#include "../bg_game.h"
#include "../../lang.h"
#include "imgui_settings.h"
#include "imgui_keysetup.h"
#include "imgui_winbolonet.h"
#include "imgui_news.h"
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
  extern int  soundVolume;
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
  void windowSetSoundVolume(int pct);
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

/* Chain a Noto Sans CJK font into the atlas covering exactly the CJK
 * codepoints that appear in the picker's language-name labels. Without
 * this, language entries written in their native script (日本語, 한국어,
 * 简体中文, 繁體中文) render as tofu in the dropdown until the user
 * actually selects them — a chicken-and-egg UX problem.
 *
 * The umbrella font is NotoSansCJKsc-Regular.otf: every regional Noto
 * Sans CJK .otf shares the same character set (CJK Unified Ideographs +
 * Hiragana + Katakana + Hangul) and differs only in glyph forms for
 * disputed-region ideographs. SC is therefore sufficient to render
 * names from all four CJK regions; the active-language chain (which
 * runs first via imguiLoadBoloFontSized) already wins for any
 * codepoints that overlap, so JP users still see JP-style kanji
 * elsewhere in the UI.
 *
 * Storage: ImGui keeps a pointer to the glyph range, so the ImWchar
 * vector must outlive the atlas. A function-local static suffices —
 * only one settings dialog atlas can exist at a time. */
static void chainPickerNameGlyphs(LangFileEntry *entries, int count,
                                  float fontSize) {
    if (!entries || count <= 0) return;

    ImFontGlyphRangesBuilder builder;
    bool anyCjk = false;
    for (int i = 0; i < count; i++) {
        const char *name = entries[i].meta.name;
        if (!name || !*name) continue;
        for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
            if (*p >= 0x80) { anyCjk = true; break; }
        }
        builder.AddText(name);
    }
    if (!anyCjk) return;

    static ImVector<ImWchar> pickerRanges;
    pickerRanges.clear();
    builder.BuildRanges(&pickerRanges);
    if (pickerRanges.empty()) return;

    int            sz   = 0;
    unsigned char *data = imguiFontLoadData(
        "data/fonts/NotoSansCJKsc-Regular.otf", &sz);
    if (!data) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET, "imgui_settings: NotoSansCJKsc-Regular.otf missing — "
                "picker CJK names will render as tofu");
        return;
    }

    ImFontConfig cfg;
    cfg.MergeMode    = true;
    cfg.OversampleH  = 1;
    cfg.OversampleV  = 1;
    cfg.GlyphRanges  = pickerRanges.Data;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, sz, fontSize, &cfg);
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

    /* Scan available translations BEFORE loading fonts. The dropdown
     * displays each language's name in its native script (日本語, 한국어,
     * etc.); we need those codepoints in the atlas from the first
     * frame, otherwise the picker tofus the very entries the user
     * needs to click on. Re-scanning every frame would hit the disk
     * on every redraw; the picker is local to this dialog so a fresh
     * scan on next open is sufficient if a translator drops a new
     * file in data/lang/. */
    int            langCount = 0;
    LangFileEntry *langEntries = langPickerScan(&langCount);

    dialogApplyScaling(s);
    {
        float pickerFontSize = (s <= 1.05f) ? 18.0f : 20.0f * s;
        chainPickerNameGlyphs(langEntries, langCount, pickerFontSize);
    }

    /* Load current player name */
    char playerName[PLAYER_NAME_LEN];
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
        s_langInfoIcon = imguiLoadSvgIcon(renderer, "data/ui/dialog-info.svg", iconPx);
        if (!s_langInfoIcon) {
            char basePathBuf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(basePathBuf, sizeof(basePathBuf),
                             "%sdata/ui/dialog-info.svg", base);
                s_langInfoIcon = imguiLoadSvgIcon(renderer, basePathBuf, iconPx);
            }
        }
    }

    int curLangIdx = 0;
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

    /* Track the CJK font requirement currently baked into the ImGui
     * font atlas. The atlas was built for the language that was active
     * when dialogApplyScaling() ran above, so seed it from the same
     * gameFront state. When the user picks a language whose CJK
     * requirement differs, we rebuild the atlas in-place at end-of-
     * frame so non-Latin glyphs render in the same dialog session
     * (no app restart needed). */
    char        atlasLangCode[32] = {0};
    gameFrontGetLanguageCode(atlasLangCode, (int)sizeof(atlasLangCode));
    const char *atlasCjkPath = cjkNotoFontPath(atlasLangCode);
    bool        pendingFontRebuild = false;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
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
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

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

        /* Top-right close X. */
        if (WBUI::DrawPanelCloseX()) {
            running = false;
        }

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
            static int lastNameError = 0;
            bool wbnActive = gameFrontGetWinbolonetUse();
            if (wbnActive) {
                /* Refresh local buffer from gameFront in case WBN login just set it */
                gameFrontGetPlayerName(playerName);
            }
            ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_PLAYERNAME));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            if (wbnActive) ImGui::BeginDisabled();
            if (ImGui::InputText("##playerName", playerName, PLAYER_NAME_LEN,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                playerName[PLAYER_NAME_LAST] = '\0';
                char validated[PLAYER_NAME_LEN];
                PlayerNameValidationError nameErr = PLAYER_NAME_OK;
                if (playerNameValidate(playerName, validated, PLAYER_NAME_LEN, &nameErr)) {
                    SDL_strlcpy(playerName, validated, PLAYER_NAME_LEN);
                    gameFrontSetPlayerName(playerName);
                    lastNameError = 0;
                } else {
                    switch (nameErr) {
                        case PLAYER_NAME_ERR_EMPTY:
                            lastNameError = STR_DLGSETNAME_BLANK_ERR;
                            break;
                        case PLAYER_NAME_ERR_RESERVED_PREFIX:
                            lastNameError = STR_DLGSETNAME_STAR_ERR;
                            break;
                        case PLAYER_NAME_ERR_RESERVED_SUFFIX:
                            lastNameError = STR_NAME_INVALID_RESERVED_SUFFIX;
                            break;
                        case PLAYER_NAME_ERR_MIXED_SCRIPTS:
                            lastNameError = STR_NAME_INVALID_MIXED_SCRIPTS;
                            break;
                        case PLAYER_NAME_ERR_INVALID_UTF8:
                        case PLAYER_NAME_ERR_DISALLOWED_CHAR:
                        case PLAYER_NAME_ERR_TOO_LONG:
                        default:
                            lastNameError = STR_NAME_INVALID_CHARS;
                            break;
                    }
                }
            }
            ImGui::SameLine();
            {
                char applyBuf[64];
                snprintf(applyBuf, sizeof(applyBuf), "%s##name", langGetText(STR_DLGSETTINGS_APPLY));
                if (ImGui::Button(applyBuf)) {
                    playerName[PLAYER_NAME_LAST] = '\0';
                    char validated[PLAYER_NAME_LEN];
                    PlayerNameValidationError nameErr = PLAYER_NAME_OK;
                    if (playerNameValidate(playerName, validated, PLAYER_NAME_LEN, &nameErr)) {
                        SDL_strlcpy(playerName, validated, PLAYER_NAME_LEN);
                        gameFrontSetPlayerName(playerName);
                        lastNameError = 0;
                    } else {
                        switch (nameErr) {
                            case PLAYER_NAME_ERR_EMPTY:
                                lastNameError = STR_DLGSETNAME_BLANK_ERR;
                                break;
                            case PLAYER_NAME_ERR_RESERVED_PREFIX:
                                lastNameError = STR_DLGSETNAME_STAR_ERR;
                                break;
                            case PLAYER_NAME_ERR_RESERVED_SUFFIX:
                                lastNameError = STR_NAME_INVALID_RESERVED_SUFFIX;
                                break;
                            case PLAYER_NAME_ERR_MIXED_SCRIPTS:
                                lastNameError = STR_NAME_INVALID_MIXED_SCRIPTS;
                                break;
                            case PLAYER_NAME_ERR_INVALID_UTF8:
                            case PLAYER_NAME_ERR_DISALLOWED_CHAR:
                            case PLAYER_NAME_ERR_TOO_LONG:
                            default:
                                lastNameError = STR_NAME_INVALID_CHARS;
                                break;
                        }
                    }
                }
                imguiHandOnHover();
            }
            if (wbnActive) ImGui::EndDisabled();
            if (wbnActive) {
                ImGui::TextUnformatted(langGetText(STR_DLGSETNAME_WBN_LOCKED));
            }
            if (lastNameError != 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "%s", langGetText(lastNameError));
            }

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
                            /* English baseline — drop any loaded override.
                             * Persist as "en" rather than "" so that on
                             * relaunch gameFrontStart treats this as a
                             * deliberate choice and skips langAutoDetect
                             * (otherwise a German-locale machine would flip
                             * back to German on every restart). */
                            langUnloadFile();
                            gameFrontSetLanguageCode("en");
                        } else {
                            if (langLoadFile(langEntries[i].path)) {
                                gameFrontSetLanguageCode(langEntries[i].code);
                            }
                        }
                        curLangIdx = i;

                        /* If the selected language requires a different
                         * CJK font region than the one currently baked
                         * into the atlas, schedule a rebuild at end-of
                         * frame. Latin↔Latin and same-region CJK
                         * switches don't need a rebuild — langLoadFile
                         * already updated the override table and the
                         * next frame picks up new strings. */
                        char        newLangCode[32] = {0};
                        gameFrontGetLanguageCode(newLangCode,
                                                  (int)sizeof(newLangCode));
                        const char *newCjkPath = cjkNotoFontPath(newLangCode);
                        if (newCjkPath != atlasCjkPath) {
                            pendingFontRebuild = true;
                        }
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
                imguiHandOnHover();
            } else {
                if (ImGui::SmallButton("?##langInfoBtn")) {
                    openInfo = true;
                }
                imguiHandOnHover();
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

            {
                bool sf = gameFrontGetShowCountryFlagsInChat();
                if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_SHOW_COUNTRY_FLAGS), &sf)) {
                    gameFrontSetShowCountryFlagsInChat(sf);
                }
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
            imguiHandOnHover();
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
            imguiHandOnHover();
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
            {
                int vol = soundVolume;
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::SliderInt(langGetText(STR_MENU_VOLUME), &vol, 0, 100, "%d%%")) {
                    windowSetSoundVolume(vol);
                }
            }
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

#if !BOLO_MOBILE
        /* ---- Network ---- */

        if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_NETWORK), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_NET_HELP));
            ImGui::Spacing();
            {
                bool b = gameFrontUseUpnp;
                if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_USE_UPNP), &b)) {
                    gameFrontUseUpnp = b;
                }
            }
            {
                bool b = gameFrontUseNatTraversal;
                if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_USE_NATTRAV), &b)) {
                    gameFrontUseNatTraversal = b;
                }
            }
            {
                const char *cur = newsPrefGetAutoShow();
                /* "unset" and "show" both default the checkbox to
                 * checked; only an explicit "dontShow" unchecks it.
                 * Toggling never writes "unset". */
                bool b = (strcmp(cur, "dontShow") != 0);
                if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_NEWS_AUTOSHOW), &b)) {
                    newsPrefSetAutoShow(b ? "show" : "dontShow");
                }
            }
        }
#endif

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

        /* Close is affirmative here ("I'm done, keep settings"), not a
         * cancel-equivalent — Settings has no destructive action to
         * back out of, changes apply live. Use the confirm slot so it
         * gets default primary styling, not the muted Cancel grey. */
        int f = WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                                   /*confirmLabel*/ langGetText(STR_CLOSE));
        if (f != WBUI::FOOTER_NONE) {
            running = false;
        }

        ImGui::End(); /* ##SettingsPanel */
        ImGui::End(); /* ##SettingsBg */

        dialogDrawNavOutline();
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

        /* Rebuild the font atlas in-place when the user picks a
         * language whose CJK requirement differs from the one that's
         * currently baked into the atlas. Doing this between Present
         * and the next NewFrame is safe — no draw commands are
         * pending against the old font texture. We re-add Inter (and
         * the chained CJK font, picked up automatically inside
         * imguiLoadBoloFontSized via gameFrontGetLanguageCode) at the
         * same pixel size dialogApplyScaling() used so the dialog's
         * style metrics still match. The SDL3 backend exposes the
         * ImGuiBackendFlags_RendererHasTextures contract, so atlas
         * texture lifecycle (WantDestroy on the old, WantCreate on
         * the new) is handled automatically inside the next
         * RenderDrawData call — we don't need to drive it manually. */
        if (pendingFontRebuild) {
            /* Mirror dialogApplyScaling() — `s` already includes the
               Deck multiplier from dialogComputeScale, so the rebuilt
               atlas matches what the dialog opened with. */
            float fontSize = (s <= 1.05f) ? 18.0f : 20.0f * s;
            ImGui::GetIO().Fonts->Clear();
            imguiLoadBoloFont(fontSize);
            chainPickerNameGlyphs(langEntries, langCount, fontSize);

            /* Refresh the recorded atlas state so subsequent picks
             * compare against what's actually in the atlas now. */
            char rebuiltLangCode[32] = {0};
            gameFrontGetLanguageCode(rebuiltLangCode,
                                     (int)sizeof(rebuiltLangCode));
            atlasCjkPath = cjkNotoFontPath(rebuiltLangCode);
            pendingFontRebuild = false;
        }

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
            imguiRegisterPlatformOpenUrl();
            ImGuiIO &ioNew = ImGui::GetIO();
            ioNew.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ioNew.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            ioNew.ConfigNavCursorVisibleAlways = true;
            ioNew.IniFilename = nullptr;

            ImGui::StyleColorsDark();
            imguiApplyBoloTheme();
            ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
            ImGui_ImplSDLRenderer3_Init(renderer);
            dialogApplyScaling(s);
            {
                /* `s` already factors in the Deck multiplier from
                   dialogComputeScale, so 20*s matches the dialog's
                   main font size. */
                float pickerFontSize = (s <= 1.05f) ? 18.0f : 20.0f * s;
                chainPickerNameGlyphs(langEntries, langCount, pickerFontSize);
            }
            /* Atlas was rebuilt with the now-active language; resync. */
            {
                char resumeLangCode[32] = {0};
                gameFrontGetLanguageCode(resumeLangCode,
                                         (int)sizeof(resumeLangCode));
                atlasCjkPath = cjkNotoFontPath(resumeLangCode);
            }

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
