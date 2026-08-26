/*
 * Copyright (c) 1998-2026 John Morrison.
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
#include "imgui_controller_prompt.h"
#include "dialog_footer.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "../imgui_steam_nav.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../ui_mode.h"
#include "../../gamefront.h"
#include "global.h"
#include "client_enums.h"  /* labelLen */
#include "upload_policy.h"  /* UploadPolicy — map-upload combo */
#include "playername_validate.h"
#include "../bg_game.h"
#include "../../lang.h"
#include "imgui_settings.h"
#include "imgui_keyboard.h"
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
  extern bool letterboxBarsGray;
  extern bool showPillLabels;
  extern bool showBaseLabels;
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
  void windowLetterboxBarsGray_toggle(void);
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
  void windowLabelOwnTank_toggle(struct ClientSim *cs);
  void windowSetMessageLabelLen(struct ClientSim *cs, labelLen newLen);
  void windowSetTankLabelLen(struct ClientSim *cs, labelLen newLen);
  bool clientSimSetPlayerName(struct ClientSim *cs, char *value);

#if defined(__IPHONEOS__)
  bool iosCrashReportingGetEnabled(void);
  void iosCrashReportingSetEnabled(bool enabled);
#endif
}

/* Cached language-info icon. The SDL renderer is process-lifetime, so the
 * texture stays valid between settings opens. */
static SDL_Texture *s_langInfoIcon = nullptr;
static bool s_langInfoIconAttempted = false;

/* Shared player-name edit buffer for the General tab, used by both settings
 * shells. Seeded from the persisted name via imguiSettingsSeedPlayerName(). */
static char s_playerNameBuf[PLAYER_NAME_LEN];

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

/* Seed the shared player-name edit buffer from the persisted name. */
extern "C" void imguiSettingsSeedPlayerName(void) {
    s_playerNameBuf[0] = '\0';
    gameFrontGetPlayerName(s_playerNameBuf);
}

/* -------------------------------------------------------
 * General tab — player identity shared by the pre-game dialog
 * and the in-game overlay: the validated player-name control
 * (locked while signed in to WinBolo.net) and the WinBolo.net
 * account section.  ctx->cs is NULL pre-game; on a successful
 * name change the live sim is updated only when it is non-NULL.
 * ------------------------------------------------------- */
extern "C" void imguiSettingsRenderGeneralTab(SettingsRenderCtx *ctx) {
    static int lastNameError = 0;
    bool wbnActive = gameFrontGetWinbolonetUse();
    if (wbnActive) {
        /* Refresh the buffer from gameFront in case WBN login just set it */
        gameFrontGetPlayerName(s_playerNameBuf);
    }
    ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_PLAYERNAME));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (wbnActive) ImGui::BeginDisabled();
    if (ImGui::InputText("##playerName", s_playerNameBuf, PLAYER_NAME_LEN,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        s_playerNameBuf[PLAYER_NAME_LAST] = '\0';
        char validated[PLAYER_NAME_LEN];
        PlayerNameValidationError nameErr = PLAYER_NAME_OK;
        if (playerNameValidate(s_playerNameBuf, validated, PLAYER_NAME_LEN, &nameErr)) {
            SDL_strlcpy(s_playerNameBuf, validated, PLAYER_NAME_LEN);
            gameFrontSetPlayerName(s_playerNameBuf);
            if (ctx->cs) clientSimSetPlayerName(ctx->cs, s_playerNameBuf);
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
            s_playerNameBuf[PLAYER_NAME_LAST] = '\0';
            char validated[PLAYER_NAME_LEN];
            PlayerNameValidationError nameErr = PLAYER_NAME_OK;
            if (playerNameValidate(s_playerNameBuf, validated, PLAYER_NAME_LEN, &nameErr)) {
                SDL_strlcpy(s_playerNameBuf, validated, PLAYER_NAME_LEN);
                gameFrontSetPlayerName(s_playerNameBuf);
                if (ctx->cs) clientSimSetPlayerName(ctx->cs, s_playerNameBuf);
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
    imguiWinbolonetDrawSection(ctx->inGame);
}

/* -------------------------------------------------------
 * Language picker — combo + info popup shared by both settings
 * shells.  Each shell passes its own scanned entries and owns their
 * lifecycle.  curLangIdx is recomputed each frame from the persisted
 * language code, so the picker holds no selection state.  On a pick
 * that changes the CJK font region it sets ctx->wantAtlasRebuild;
 * each shell consumes that into its own atlas-rebuild path.
 * ------------------------------------------------------- */
extern "C" void imguiSettingsRenderLanguagePicker(LangFileEntry *entries,
                                                  int count,
                                                  SettingsRenderCtx *ctx) {
    /* Current selection: match the persisted code against the entries;
       fall back to entry 0 (English baseline). */
    int curLangIdx = 0;
    {
        char curCode[32];
        curCode[0] = '\0';
        gameFrontGetLanguageCode(curCode, (int)sizeof(curCode));
        if (curCode[0] != '\0') {
            for (int i = 0; i < count; i++) {
                if (strcmp(curCode, entries[i].code) == 0) {
                    curLangIdx = i;
                    break;
                }
            }
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_LANGUAGE_LBL));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    const char *curLangLabel =
        (curLangIdx >= 0 && curLangIdx < count &&
         entries[curLangIdx].meta.name[0] != '\0')
            ? entries[curLangIdx].meta.name
            : langGetText(STR_DLGLANG_NAME);
    if (ImGui::BeginCombo("##language", curLangLabel)) {
        for (int i = 0; i < count; i++) {
            const char *itemLabel =
                (entries[i].meta.name[0] != '\0')
                    ? entries[i].meta.name
                    : entries[i].code;
            bool selected = (curLangIdx == i);
            if (ImGui::Selectable(itemLabel, selected)) {
                /* CJK region in effect before the switch, for the
                   rebuild decision below. */
                char oldCode[32] = {0};
                gameFrontGetLanguageCode(oldCode, (int)sizeof(oldCode));
                const char *oldCjkPath = cjkNotoFontPath(oldCode);

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
                    if (langLoadFile(entries[i].path)) {
                        gameFrontSetLanguageCode(entries[i].code);
                    }
                }

                /* Only a CJK-region change needs a font-atlas rebuild;
                 * Latin↔Latin and same-region CJK switches just pick up
                 * the new strings next frame.  cjkNotoFontPath returns
                 * stable per-region static pointers, so compare identity. */
                char newCode[32] = {0};
                gameFrontGetLanguageCode(newCode, (int)sizeof(newCode));
                if (cjkNotoFontPath(newCode) != oldCjkPath) {
                    ctx->wantAtlasRebuild = true;
                }
            }
        }
        ImGui::EndCombo();
    }

    /* Info icon next to the combo opens a popup carrying the author /
     * notes.  Lazy-load the icon once using the process-lifetime
     * renderer; size it font-relatively to suit the current metrics. */
    if (!s_langInfoIcon && !s_langInfoIconAttempted) {
        s_langInfoIconAttempted = true;
        SDL_Renderer *renderer = sdl3DrawGetRenderer();
        int iconPx = (int)ImGui::GetFontSize();
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

    /* langGetLoadedMeta() may be non-NULL for a loaded file; prefer
     * that for non-English entries so live re-translation of the meta
     * works.  Entry 0's meta is the English baseline. */
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
        if (curLangIdx == 0 || curLangIdx >= count) {
            displayMeta = (count > 0) ? &entries[0].meta : nullptr;
        } else {
            const LangFileMeta *loaded = langGetLoadedMeta();
            displayMeta = loaded ? loaded : &entries[curLangIdx].meta;
        }
        float wrapW = ImGui::GetFontSize() * 20.0f;
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
}

/* -------------------------------------------------------
 * Controls tab — the Set Keys button, shared by both shells.
 * The shared renderer only sets ctx->wantKeySetup; each shell
 * launches key setup after the frame (the pre-game modal tears
 * down and rebuilds its context; the in-game overlay opens the
 * key-setup overlay).  In-game-only controls (relative steering,
 * gamepad scroll) are drawn inline by that shell.
 * ------------------------------------------------------- */
extern "C" void imguiSettingsRenderControlsTab(SettingsRenderCtx *ctx) {
#if !BOLO_MOBILE
    if (!uiModeIsTablet()) {
        if (ImGui::Button(langGetText(STR_DLGSETTINGS_SETKEYS), ImVec2(120, 0))) {
            ctx->wantKeySetup = true;
        }
        imguiHandOnHover();
    }
#endif
}

/* -------------------------------------------------------
 * Display & Sound tab — the display and sound controls shared by
 * the pre-game dialog and the in-game overlay.  Frame rate,
 * letterbox, and Sound apply in both; window size and UI scale only
 * apply in-game (ctx->inGame), and their results are returned via
 * ctx->pendingZoom / ctx->wantAtlasRebuild for the in-game shell
 * to apply after the frame.  The Sound section renders last.
 * ------------------------------------------------------- */
extern "C" void imguiSettingsRenderDisplaySoundTab(SettingsRenderCtx *ctx) {
    /* ---- Frame rate ---- */
    if (!uiModeIsTablet()) {
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
                    windowSetFrameRate(frValues[i], ctx->inGame);
                }
            }
            ImGui::EndCombo();
        }
    }

#ifndef __ANDROID__
    /* ---- Window size — desktop, in-game only ---- */
    if (ctx->inGame && !uiModeIsTablet()) {
        const char *zoomLabels[] = {
            langGetText(STR_MENU_NORMAL),
            langGetText(STR_MENU_DOUBLE),
            langGetText(STR_MENU_TRIPLE),
            langGetText(STR_MENU_QUAD),
            langGetText(STR_MENU_CUSTOM_RESIZABLE),
        };
        BYTE zoomValues[] = { ZOOM_FACTOR_NORMAL, ZOOM_FACTOR_DOUBLE, ZOOM_FACTOR_TRIPLE, ZOOM_FACTOR_QUAD, ZOOM_FACTOR_CUSTOM };
        int curZoomIdx = 0;
        for (int i = 0; i < 5; i++) {
            if (zoomFactor == zoomValues[i]) { curZoomIdx = i; break; }
        }
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_WINDOWSIZE));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::BeginCombo("##windowsize", zoomLabels[curZoomIdx])) {
            for (int i = 0; i < 5; i++) {
                bool selected = (curZoomIdx == i);
                if (ImGui::Selectable(zoomLabels[i], selected)) {
                    ctx->pendingZoom = (unsigned char)zoomValues[i];
                }
            }
            ImGui::EndCombo();
        }
    }

    /* ---- UI scale — desktop, in-game only, not Steam Deck ---- */
    if (ctx->inGame && !uiModeIsTablet() && !uiModeIsSteamDeck()) {
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_UISCALE));
        const char *scaleLabels[] = {
            langGetText(STR_DLGSETTINGS_UISCALE_AUTO),
            langGetText(STR_DLGSETTINGS_UISCALE_SMALL),
            langGetText(STR_DLGSETTINGS_UISCALE_MEDIUM),
            langGetText(STR_DLGSETTINGS_UISCALE_LARGE),
        };
        int usIdx = (int)uiUiScaleGet();
        if (usIdx < 0 || usIdx > 3) usIdx = 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::BeginCombo("##uiscale", scaleLabels[usIdx])) {
            for (int i = 0; i < 4; i++) {
                bool sel = (usIdx == i);
                if (ImGui::Selectable(scaleLabels[i], sel) && i != usIdx) {
                    uiUiScaleSet((UiScalePref)i);
                    gameFrontSaveCurrentPrefs();
                    ctx->wantAtlasRebuild = true;
                }
            }
            ImGui::EndCombo();
        }
    }
#endif

#if !BOLO_MOBILE
    /* ---- Letterbox bars ---- */
    {
        bool lb = (bool)letterboxBarsGray;
        if (ImGui::Checkbox(langGetText(STR_MENU_LETTERBOX_GRAY), &lb)) {
            windowLetterboxBarsGray_toggle();
        }
        imguiHelpTooltip("Fill the fullscreen border bars with gray "
                         "instead of black (when your monitor's aspect "
                         "ratio differs from the game).");
    }
#endif

    /* ---- Sound ---- */
    ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_SOUND));
    {
        bool se = (bool)soundEffects;
        if (ImGui::Checkbox(langGetText(STR_MENU_SOUND_EFFECTS), &se)) windowSoundEffects_toggle();
    }
    if (!uiModeIsTablet()) {
        bool bgs = (bool)backgroundSound;
        if (ImGui::Checkbox(langGetText(STR_MENU_BACKGROUND_SOUND), &bgs)) windowBackgroundSoundChange_toggle();
    }
#if !BOLO_MOBILE
    if (!uiModeIsTablet()) {
        bool sk = (bool)useSoundKeepalive;
        if (ImGui::Checkbox(langGetText(STR_MENU_SOUND_KEEPALIVE), &sk)) windowSoundKeepalive();
    }
#endif
    {
        int vol = soundVolume;
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderInt(langGetText(STR_MENU_VOLUME), &vol, 0, 100, "%d%%")) windowSetSoundVolume(vol);
    }
}

/* -------------------------------------------------------
 * Game/HUD tab — gameplay/HUD controls shared by the pre-game
 * dialog and the in-game overlay: scrolling behaviour, the
 * gunsight, label controls, and message toggles.  ctx->cs is
 * NULL pre-game (the toggles tolerate it).
 * ------------------------------------------------------- */
extern "C" void imguiSettingsRenderGameHudTab(SettingsRenderCtx *ctx) {
    struct ClientSim *cs = ctx->cs;
    /* ---- Auto scrolling ---- */
    {
        bool as = (bool)autoScrollingEnabled;
        if (ImGui::Checkbox(langGetText(STR_MENU_AUTO_SCROLLING), &as)) {
            windowAutomaticScrolling_toggle(ctx->cs);
        }
    }

#if !BOLO_MOBILE
    /* ---- Smooth scrolling ---- */
    {
        bool ss = (bool)smoothScrollingEnabled;
        if (ImGui::Checkbox(langGetText(STR_MENU_SMOOTH_SCROLLING), &ss)) {
            windowSmoothScrolling_toggle();
        }
    }
#endif

    /* ---- Show gunsight ---- */
    {
        bool gs = (bool)showGunsight;
        if (ImGui::Checkbox(langGetText(STR_MENU_SHOW_GUNSIGHT), &gs)) {
            windowShowGunsight_toggle(ctx->cs);
        }
    }

    /* ---- Labels ---- */
    ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_LABELS));
    ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_MSGNAMES));
    ImGui::SameLine();
    {
        bool isShort = (labelMsg == lblShort);
        char shortBuf[64], longBuf[64];
        snprintf(shortBuf, sizeof(shortBuf), "%s##msg", langGetText(STR_SHORT));
        snprintf(longBuf,  sizeof(longBuf),  "%s##msg", langGetText(STR_LONG));
        if (ImGui::RadioButton(shortBuf, isShort))  windowSetMessageLabelLen(cs, lblShort);
        ImGui::SameLine();
        if (ImGui::RadioButton(longBuf,  !isShort)) windowSetMessageLabelLen(cs, lblLong);
    }
    ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_TANKLABELS));
    ImGui::SameLine();
    {
        char noneBuf[64], shortBuf[64], longBuf[64];
        snprintf(noneBuf,  sizeof(noneBuf),  "%s##tank", langGetText(STR_NONE));
        snprintf(shortBuf, sizeof(shortBuf), "%s##tank", langGetText(STR_SHORT));
        snprintf(longBuf,  sizeof(longBuf),  "%s##tank", langGetText(STR_LONG));
        if (ImGui::RadioButton(noneBuf,  labelTank == lblNone))  windowSetTankLabelLen(cs, lblNone);
        ImGui::SameLine();
        if (ImGui::RadioButton(shortBuf, labelTank == lblShort)) windowSetTankLabelLen(cs, lblShort);
        ImGui::SameLine();
        if (ImGui::RadioButton(longBuf,  labelTank == lblLong))  windowSetTankLabelLen(cs, lblLong);
    }
    {
        bool noSelf = !(bool)labelSelf;
        if (ImGui::Checkbox(langGetText(STR_MENU_NO_OWN_LABEL), &noSelf)) windowLabelOwnTank_toggle(cs);
    }
    {
        bool pl = (bool)showPillLabels;
        if (ImGui::Checkbox(langGetText(STR_MENU_PILLBOX_LABELS), &pl)) windowShowPillLabels_toggle(cs);
    }
    {
        bool bl = (bool)showBaseLabels;
        if (ImGui::Checkbox(langGetText(STR_MENU_BASE_LABELS), &bl)) windowShowBaseLabels_toggle(cs);
    }

    /* ---- Messages ---- */
    ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_MESSAGES));
    { bool nw = (bool)showNewswireMessages;      if (ImGui::Checkbox(langGetText(STR_MENU_NEWSWIRE_MSGS),  &nw)) windowMenuNewswire_toggle(cs); }
    { bool am = (bool)showAssistantMessages;     if (ImGui::Checkbox(langGetText(STR_MENU_ASSISTANT_MSGS), &am)) windowMenuAssistant_toggle(cs); }
    { bool ai = (bool)showAIMessages;            if (ImGui::Checkbox(langGetText(STR_MENU_AI_MSGS),        &ai)) windowMenuAI_toggle(cs); }
    { bool ns = (bool)showNetworkStatusMessages; if (ImGui::Checkbox(langGetText(STR_MENU_NETSTATUS_MSGS), &ns)) windowMenuNetwork_toggle(cs); }
    { bool nd = (bool)showNetworkDebugMessages;  if (ImGui::Checkbox(langGetText(STR_MENU_NETDEBUG_MSGS),  &nd)) windowMenuNetworkDebug_toggle(cs); }
}

/* Folder-picker glue for the Hosting tab's Upload Directory field. The
 * SDL folder dialog is async — its callback (may fire on another thread)
 * stashes the chosen path and a flag, and the next render frame applies it
 * via the write-through setter.  The editable InputText remains the primary
 * input and the fallback on platforms without a native folder dialog. */
static char s_hostingPickedDir[FILENAME_MAX];
static bool s_hostingDirPicked = false;

static void SDLCALL hostingUploadDirDialogCallback(void *userdata,
                                                   const char *const *filelist,
                                                   int filter) {
    (void)userdata;
    (void)filter;
    if (filelist && filelist[0]) {
        SDL_strlcpy(s_hostingPickedDir, filelist[0], FILENAME_MAX);
        s_hostingDirPicked = true;
    }
}

/* Same async-picker glue for the Log Directory field. Kept on its own
 * statics so a pending pick can't cross-wire with the Upload Directory
 * picker above. */
static char s_hostingLogPickedDir[FILENAME_MAX];
static bool s_hostingLogDirPicked = false;

static void SDLCALL hostingLogDirDialogCallback(void *userdata,
                                                const char *const *filelist,
                                                int filter) {
    (void)userdata;
    (void)filter;
    if (filelist && filelist[0]) {
        SDL_strlcpy(s_hostingLogPickedDir, filelist[0], FILENAME_MAX);
        s_hostingLogDirPicked = true;
    }
}

/* -------------------------------------------------------
 * Hosting tab — settings for the server the client spins up
 * when hosting from the game finder.  Shared by the pre-game
 * dialog and the in-game overlay.
 * ------------------------------------------------------- */
extern "C" void imguiSettingsRenderHostingTab(SettingsRenderCtx *ctx) {
    (void)ctx;

    /* ---- Port ---- */
    {
        int port = gameFrontHostingPort;
        if (ImGui::InputInt(langGetText(STR_DLGSETTINGS_HOSTING_PORT), &port)) {
            if (port < 1024)  port = 1024;
            if (port > 65535) port = 65535;
            gameFrontSetHostingPort((unsigned short)port);
        }
    }

    /* ---- Spectators ---- */
    {
        bool allow = gameFrontHostingAllowSpec;
        if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_HOSTING_ALLOWSPEC), &allow)) {
            gameFrontSetHostingAllowSpec(allow);
        }
        /* Max stays visible but greyed when spectators are off; its value is
         * preserved so toggling back on restores the previous number. */
        ImGui::BeginDisabled(!allow);
        int maxSpec = gameFrontHostingMaxSpec;
        if (ImGui::InputInt(langGetText(STR_DLGSETTINGS_HOSTING_MAXSPEC), &maxSpec)) {
            if (maxSpec < 1)  maxSpec = 1;
            if (maxSpec > 32) maxSpec = 32;
            gameFrontSetHostingMaxSpec(maxSpec);
        }
        ImGui::EndDisabled();
    }

    /* ---- Map uploads ---- */
    {
        /* Combo display order is Off / Allow / Persist, but the enum values
         * are not in that order (ALLOW=0, OFF=1, PERSIST=2) — map explicitly. */
        static const int kPolicyByIndex[3] = {
            UPLOAD_POLICY_OFF, UPLOAD_POLICY_ALLOW, UPLOAD_POLICY_PERSIST
        };
        const char *policyItems[3] = {
            langGetText(STR_DLGSETTINGS_HOSTING_UPLOAD_OFF),
            langGetText(STR_DLGSETTINGS_HOSTING_UPLOAD_ALLOW),
            langGetText(STR_DLGSETTINGS_HOSTING_UPLOAD_PERSIST)
        };
        int idx = 1;  /* default Allow */
        for (int i = 0; i < 3; ++i) {
            if (kPolicyByIndex[i] == gameFrontHostingUploadPolicy) { idx = i; break; }
        }
        if (ImGui::Combo(langGetText(STR_DLGSETTINGS_HOSTING_MAPUPLOADS),
                         &idx, policyItems, 3)) {
            gameFrontSetHostingUploadPolicy(kPolicyByIndex[idx]);
        }

        /* File/storage caps only bite on Persist (Off/Allow never write). */
        if (gameFrontHostingUploadPolicy == UPLOAD_POLICY_PERSIST) {
            /* Upload directory — editable text field is the primary input and
             * the fallback where no native folder dialog exists; Browse fills
             * it via SDL_ShowOpenFolderDialog. */
            static char dirBuf[FILENAME_MAX];
            static bool dirEditing = false;
            /* Apply a folder chosen on a previous frame. */
            if (s_hostingDirPicked) {
                gameFrontSetHostingUploadDir(s_hostingPickedDir);
                s_hostingDirPicked = false;
            }
            /* Re-seed from the global whenever the field isn't being edited,
             * so Browse results and the persisted value show without
             * clobbering in-progress typing. */
            if (!dirEditing) {
                SDL_strlcpy(dirBuf, gameFrontHostingUploadDir, sizeof(dirBuf));
            }
            bool commit = ImGui::InputText(
                langGetText(STR_DLGSETTINGS_HOSTING_UPLOADDIR),
                dirBuf, sizeof(dirBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            dirEditing = ImGui::IsItemActive();
            if (commit || ImGui::IsItemDeactivatedAfterEdit()) {
                gameFrontSetHostingUploadDir(dirBuf);
            }
            if (ImGui::Button(langGetText(STR_MAPEDIT_BROWSE))) {
                SDL_Window *win = sdl3DrawGetWindow();
                const char *loc = gameFrontHostingUploadDir[0]
                                      ? gameFrontHostingUploadDir : NULL;
                SDL_ShowOpenFolderDialog(hostingUploadDirDialogCallback, NULL,
                                         win, loc, false);
            }

            int maxFiles = gameFrontHostingUploadMaxFiles;
            if (ImGui::InputInt(langGetText(STR_DLGSETTINGS_HOSTING_UPLOAD_MAXFILES),
                                &maxFiles)) {
                if (maxFiles < 1)   maxFiles = 1;
                if (maxFiles > 255) maxFiles = 255;
                gameFrontSetHostingUploadMaxFiles(maxFiles);
            }
            int maxStorage = gameFrontHostingUploadMaxStorage;
            if (ImGui::InputInt(langGetText(STR_DLGSETTINGS_HOSTING_UPLOAD_MAXSTORAGE),
                                &maxStorage)) {
                if (maxStorage < 1)    maxStorage = 1;
                if (maxStorage > 4095) maxStorage = 4095;
                gameFrontSetHostingUploadMaxStorage(maxStorage);
            }
        }
    }

    /* ---- Round logging ---- */
    {
        bool logging = gameFrontHostingLogging;
        if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_HOSTING_ENABLELOG),
                            &logging)) {
            gameFrontSetHostingLogging(logging);
        }
        if (gameFrontHostingLogging) {
            /* Log directory — editable text field is the primary input and
             * the fallback where no native folder dialog exists; Browse fills
             * it via SDL_ShowOpenFolderDialog. */
            static char logDirBuf[FILENAME_MAX];
            static bool logDirEditing = false;
            /* Apply a folder chosen on a previous frame. */
            if (s_hostingLogDirPicked) {
                gameFrontSetHostingLogDir(s_hostingLogPickedDir);
                s_hostingLogDirPicked = false;
            }
            /* Re-seed from the global whenever the field isn't being edited,
             * so Browse results and the persisted value show without
             * clobbering in-progress typing. */
            if (!logDirEditing) {
                SDL_strlcpy(logDirBuf, gameFrontHostingLogDir, sizeof(logDirBuf));
            }
            bool commit = ImGui::InputText(
                langGetText(STR_DLGSETTINGS_HOSTING_LOGDIR),
                logDirBuf, sizeof(logDirBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            logDirEditing = ImGui::IsItemActive();
            if (commit || ImGui::IsItemDeactivatedAfterEdit()) {
                gameFrontSetHostingLogDir(logDirBuf);
            }
            /* Distinct ID from the Upload Directory Browse button, which
             * shares the same label and can be on screen at the same time. */
            ImGui::PushID("hostinglogdir");
            if (ImGui::Button(langGetText(STR_MAPEDIT_BROWSE))) {
                SDL_Window *win = sdl3DrawGetWindow();
                const char *loc = gameFrontHostingLogDir[0]
                                      ? gameFrontHostingLogDir : NULL;
                SDL_ShowOpenFolderDialog(hostingLogDirDialogCallback, NULL,
                                         win, loc, false);
            }
            ImGui::PopID();

            /* Hand the finished round's log to players who ask for it, so
             * their post-game recap plays. Nested under logging because
             * there is nothing to serve without a recording. */
            bool serveReplays = gameFrontHostingServeReplays;
            if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_HOSTING_SERVEREPLAY),
                                &serveReplays)) {
                gameFrontSetHostingServeReplays(serveReplays);
            }
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", langGetText(STR_DLGSETTINGS_HOSTING_APPLYNOTE));
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

    /* Load current player name into the shared edit buffer */
    imguiSettingsSeedPlayerName();

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

    /* When the language picker reports a CJK-region change it sets
     * ctx.wantAtlasRebuild, which we translate into this flag and act on
     * after Present — rebuilding the atlas in-place so non-Latin glyphs
     * render in the same dialog session (no app restart needed). */
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
        controllerDialogsRenderMenu();

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
        float panelW = 680.0f * s, panelH = 580.0f * s;
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

        /* Scrollable content region so the Close footer below stays pinned and
         * always visible no matter how tall the settings list grows. */
        float settingsFooterH = ImGui::GetFrameHeightWithSpacing() +
                                ImGui::GetStyle().ItemSpacing.y * 3.0f + 4.0f;
        ImGui::BeginChild("##settingsScroll", ImVec2(0.0f, -settingsFooterH), ImGuiChildFlags_NavFlattened);

        SettingsRenderCtx ctx = {};
        ctx.cs = nullptr;
        ctx.inGame = false;
        ctx.pendingZoom = 255;

        /* Controller tab cycling: shoulder buttons (or the Steam menu-tab
           actions where the pad is hidden from SDL) step through the visible
           tabs, skipping any that aren't present and wrapping at the ends. */
        enum { STAB_GENERAL, STAB_DISPLAY, STAB_CONTROLS, STAB_GAMEHUD, STAB_HOSTING, STAB_LAST, STAB_COUNT };
        static int s_pgActiveTab = STAB_GENERAL;
        static int s_pgForceTab  = -1;
        bool present[STAB_COUNT];
        present[STAB_GENERAL] = true;
        present[STAB_DISPLAY] = true;
        present[STAB_GAMEHUD] = true;
        present[STAB_HOSTING] = true;  /* mobile can host too */
#if !BOLO_MOBILE
        present[STAB_CONTROLS] = !uiModeIsTablet();
        present[STAB_LAST]     = true;
#else
        present[STAB_CONTROLS] = false;
        present[STAB_LAST]     = false;
#endif
        {
            int shift = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : 0)
                      - (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? 1 : 0);
            if (shift == 0) shift = imguiSteamNavConsumeMenuTabShift();
            if (shift != 0) {
                int i = s_pgActiveTab;
                do { i = (i + shift + STAB_COUNT) % STAB_COUNT; } while (!present[i] && i != s_pgActiveTab);
                s_pgForceTab = i;
            }
        }

        if (ImGui::BeginTabBar("##settingsTabs")) {
            if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_CHAT_GENERAL), nullptr,
                    s_pgForceTab == STAB_GENERAL ? ImGuiTabItemFlags_SetSelected : 0)) {
                s_pgActiveTab = STAB_GENERAL;
                ImGui::BeginChild("##generalPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                imguiSettingsRenderGeneralTab(&ctx);
                imguiSettingsRenderLanguagePicker(langEntries, langCount, &ctx);

                /* ---- Tutorial ---- */
                ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_TUTORIAL));
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

#if defined(__IPHONEOS__)
                /* ---- Crash Reporting ---- */
                ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_CRASH_REPORTING));
                bool cr = iosCrashReportingGetEnabled();
                if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_ENABLE_CRASH), &cr)) {
                    iosCrashReportingSetEnabled(cr);
                }
                ImGui::TextWrapped("%s", langGetText(STR_DLGSETTINGS_CRASH_HELP));
                ImGui::Spacing();
                ImGui::TextDisabled("%s", langGetText(STR_DLGSETTINGS_CRASH_NEXTLAUNCH));
#endif
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_TAB_DISPLAYSOUND), nullptr,
                    s_pgForceTab == STAB_DISPLAY ? ImGuiTabItemFlags_SetSelected : 0)) {
                s_pgActiveTab = STAB_DISPLAY;
                ImGui::BeginChild("##displayPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                imguiSettingsRenderDisplaySoundTab(&ctx);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
#if !BOLO_MOBILE
            if (!uiModeIsTablet()) {
                if (ImGui::BeginTabItem(langGetText(STR_LV_WIN_CONTROLS), nullptr,
                        s_pgForceTab == STAB_CONTROLS ? ImGuiTabItemFlags_SetSelected : 0)) {
                    s_pgActiveTab = STAB_CONTROLS;
                    ImGui::BeginChild("##controlsPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                    imguiSettingsRenderControlsTab(&ctx);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
#endif
            if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_TAB_GAMEHUD), nullptr,
                    s_pgForceTab == STAB_GAMEHUD ? ImGuiTabItemFlags_SetSelected : 0)) {
                s_pgActiveTab = STAB_GAMEHUD;
                ImGui::BeginChild("##gamehudPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                imguiSettingsRenderGameHudTab(&ctx);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_TAB_HOSTING), nullptr,
                    s_pgForceTab == STAB_HOSTING ? ImGuiTabItemFlags_SetSelected : 0)) {
                s_pgActiveTab = STAB_HOSTING;
                ImGui::BeginChild("##hostingPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                imguiSettingsRenderHostingTab(&ctx);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
#if !BOLO_MOBILE
            if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_NETWORK), nullptr,
                    s_pgForceTab == STAB_LAST ? ImGuiTabItemFlags_SetSelected : 0)) {
                s_pgActiveTab = STAB_LAST;
                ImGui::BeginChild("##networkPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
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
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
#endif
            ImGui::EndTabBar();
        }
        s_pgForceTab = -1;

        /* Translate the language picker's CJK-rebuild request into the
           pre-game atlas-rebuild flag consumed after Present.  Additive:
           never clobber an already-pending rebuild. */
        if (ctx.wantAtlasRebuild) pendingFontRebuild = true;
        /* The shared Controls tab requests key setup via the flag; honour it
           through the existing showKeySetup teardown below. */
        if (ctx.wantKeySetup) showKeySetup = true;

        ImGui::EndChild(); /* ##settingsScroll */

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
        keyboardUpdate();   /* controller text entry for this dialog's fields */
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
        gameFrontPumpDirty(); /* sync cloud prefs from menus (login join + debounced upload) */
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
            /* The shared name buffer is file-scope; re-seed it for the
               rebuilt context alongside the language/atlas resync. */
            imguiSettingsSeedPlayerName();

            dialogSetWindowSize(window, 1024, 768);
            dialogSetWindowTitle(window, langGetText(STR_DLGSETTINGS_WINTITLE));

            lastTickTime = SDL_GetTicks();
        }
#endif
    }

    /* Flush the in-memory settings (player name, address/ports, language,
     * keys, tank options, ...) into the prefs document now the dialog has
     * closed, so anything changed here marks the doc sync-dirty and rides the
     * debounced cloud upload — the same persistence Key Setup gets on OK.
     * prefsSetString skips unchanged values, so closing without edits writes
     * nothing and triggers no upload. */
    {
        keyItems liveKeys;
        windowGetKeys(&liveKeys);
        gameFrontPutPrefs(&liveKeys);
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
