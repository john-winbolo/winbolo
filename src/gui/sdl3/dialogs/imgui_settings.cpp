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
#include <vector>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "imgui_nav_outline.h"
#include "imgui_controller_prompt.h"
#include "dialog_footer.h"
#include "workshop_publish_modal.h"  /* declarations only; the calls are
                                        desktop-only, like the module */
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "../imgui_steam_nav.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../ui_mode.h"
#include "../../gamefront.h"
#include "global.h"
#include "client_enums.h"  /* labelLen */
#include "upload_policy.h"  /* UploadPolicy, ScriptUploadPolicy — upload combos */
#include "view_policy.h"  /* ViewPolicy — hosting visibility rows */
#include "server_voice_mode.h"  /* ServerVoiceMode — hosting voice combo */
#include "playername_validate.h"
#include "../bg_game.h"
#include "../skin_source.h"
#include "../skin_preview.h"
#include "../gfx_settings.h"
#include "../tileloader.h"
#include "../../lang.h"
#include "../../../steam/steam_wrapper.h"
#include "../workshop_sync.h"  /* workshopSyncGeneration — the skin picker */
#include "imgui_settings.h"
#include "imgui_keyboard.h"
#include "imgui_keysetup.h"
#include "imgui_winbolonet.h"
#include "imgui_news.h"
#if defined(WINBOLO_VOICE)
#include "../../voice.h"
#include "voice_core.h"  /* VOICE_DEVICE_NAME_MAX — the device name buffers */
#endif
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
  extern int  windowMasterVolume;
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
  void windowSetMasterVolume(int pct);
#if defined(WINBOLO_VOICE)
  /* Voice apply/persist helpers — winbolo.c on the desktop, main_wasm.c in
     the browser build, both beside windowSetSoundVolume. */
  void windowSetVoiceEnabled(bool on);
  void windowSetVoiceMode(int mode);
  void windowSetVoiceMicGain(float gain);
  void windowSetVoiceVolume(float gain);
  void windowSetShowTankMicIcons(bool on);
  bool windowGetShowTankMicIcons(void);
#if defined(WINBOLO_VOICE_AEC)
  void windowSetVoiceEchoCancel(bool on);
  bool windowGetVoiceEchoCancel(void);
  bool windowGetVoiceEchoCancelAvailable(void);
  bool windowGetVoiceEchoCancelPlatform(void);
#endif
#endif
  void windowMenuNewswire_toggle(struct ClientSim *cs);
  void windowMenuAssistant_toggle(struct ClientSim *cs);
  void windowMenuAI_toggle(struct ClientSim *cs);
  void windowMenuNetwork_toggle(struct ClientSim *cs);
  void windowMenuNetworkDebug_toggle(struct ClientSim *cs);
  void windowLabelOwnTank_toggle(struct ClientSim *cs);
  void windowSetMessageLabelLen(struct ClientSim *cs, labelLen newLen);
  void windowSetTankLabelLen(struct ClientSim *cs, labelLen newLen);
  void windowFullScreenChoose(bool on);
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

/* Source tag shown beside a skin's name in the picker. */
static const char *skinKindLabel(SkinKind kind) {
    switch (kind) {
        case SKIN_KIND_WORKSHOP: return langGetText(STR_DLGSKIN_SRC_WORKSHOP);
        case SKIN_KIND_USER:     return langGetText(STR_DLGSKIN_SRC_USER);
        case SKIN_KIND_BUILTIN:
        default:                 return langGetText(STR_DLGSKIN_SRC_BUILTIN);
    }
}

/* Which scan location an id came from.  The scan stamps the location into
   the id's prefix, so the closed combo can tag the active skin without
   listing the directories again. */
static SkinKind skinKindFromId(const char *id) {
    if (strncmp(id, "builtin:", 8) == 0)  return SKIN_KIND_BUILTIN;
    if (strncmp(id, "workshop:", 9) == 0) return SKIN_KIND_WORKSHOP;
    return SKIN_KIND_USER;
}

/* Set when a pick fails to load, cleared by the next one that succeeds.
   File scope because both settings shells share the tab renderer. */
static bool s_skinLoadFailed = false;

/* Rows for the open picker, plus whether it was open on the previous frame.
   A scan is a directory listing across three locations and a skin.ini read
   per candidate, so it runs on the frame the popup opens and not again
   until it is reopened — which also means a skin dropped into the folder
   while the dialog is up shows up the next time the combo is opened.
   File scope because both settings shells share the tab renderer. */
static std::vector<SkinEntry> s_skinRows;
static bool s_skinPopupWasOpen = false;

/* The whole publish path is desktop-only.  skin_preview.c is a client source;
   the Android, iOS and browser builds compile this file against their own
   source lists, which carry the PNG reader but not the writer, so a call to
   skinWritePreviewPng would not link there.  BOLO_MOBILE covers Android and
   iOS but not Emscripten, hence both halves. */
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)

/* The skin the Publish button last opened the window on.  A publish still
   uploading keeps the window on its own skin (workshopPublishPrepare refuses
   another), so this is also the skin a finished publish belongs to.  The
   context both callbacks below are handed. */
static char s_skinPublishPath[SKIN_PATH_MAX];

/* Builds the item's content — one .wsf, whichever shape the skin has on disk
   — and writes a preview where the window asks for it. */
static bool skinPublishBuild(void *ctx, const char *folder,
                             char *previewOut, size_t previewLen) {
    const char  *path = (const char *)ctx;
    char         archive[SKIN_PATH_MAX * 2];
    char         name[SKIN_ID_MAX];
    SDL_PathInfo info;

    (void)previewLen;

    /* The archive is named after the skin, which is the id's text after the
       ':' — the same name the scan would give it. */
    {
        const char *id = skinGetRequested();
        const char *colon = (id != nullptr) ? strchr(id, ':') : nullptr;
        if (colon != nullptr) {
            SDL_strlcpy(name, colon + 1, sizeof(name));
        } else {
            SDL_strlcpy(name, (id != nullptr && id[0] != '\0') ? id : "skin",
                        sizeof(name));
        }
    }
    SDL_snprintf(archive, sizeof(archive), "%s/%s.wsf", folder, name);

    if (!SDL_GetPathInfo(path, &info)) return false;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        /* A folder holding one archive is read as that archive, so the item
           gets the archive itself rather than a zip wrapping it. */
        char inner[SKIN_PATH_MAX];
        if (skinSourceResolveArchive(path, inner, sizeof(inner))) {
            if (!workshopPublishCopyFile(inner, archive)) return false;
        } else if (!skinSourceZipDirectory(path, archive, nullptr)) {
            return false;
        }
    } else if (!workshopPublishCopyFile(path, archive)) {
        return false;
    }

    /* A preview that will not render is not a reason to stop: the item
       publishes without one and Steam shows its own placeholder. */
    if (!skinWritePreviewPng(skinGetActiveSource(), previewOut)) {
        previewOut[0] = '\0';
    }
    return true;
}

/* The published id and author go into the skin's skin.ini. */
static bool skinPublishRecordId(void *ctx, uint64_t id, uint64_t author) {
    return skinSetWorkshopId((const char *)ctx, id, author);
}

static const WorkshopPublishSpec s_skinPublishSpec = {
    "##SkinPublish",
    STR_DLGSKIN_PUBLISH_HEADING,
    STR_DLGSKIN_PUBLISH_UPDATE,
    "Skin",
    skinPublishBuild,
    skinPublishRecordId,
    s_skinPublishPath,
};
#endif  /* !BOLO_MOBILE && !__EMSCRIPTEN__ */

#if defined(WINBOLO_VOICE)
/* The audio devices offered by the two combos in the voice section.
 *
 * Enumerating asks the driver what is plugged in, which is far too much to do
 * on every frame of a dialog that redraws continuously, so the lists are held
 * here and refreshed when the section comes back on screen.  "Came back" is
 * read off the frame the section last drew on: it draws every frame while it
 * is visible, so any gap in that run is it having been away.  Deriving it that
 * way rather than seeding the lists when the dialog opens is what makes both
 * ways into this tab — the pre-game dialog and the in-game overlay — refresh
 * without either of them having to ask.
 *
 * Names are copied rather than pointed at: the voice module's own list is only
 * good until the next count, and this one is read for the whole frame.  The
 * cap is the same order as the backend's; a machine with more devices than
 * this plugged in shows the first of them. */
#define VOICE_DEVICE_LIST_CAP 32
static int  s_voiceMicCount = 0;
static int  s_voiceOutCount = 0;
static char s_voiceMicNames[VOICE_DEVICE_LIST_CAP][VOICE_DEVICE_NAME_MAX];
static char s_voiceOutNames[VOICE_DEVICE_LIST_CAP][VOICE_DEVICE_NAME_MAX];
static int  s_voiceDeviceFrame = -1;

/* Refills one list from the voice module, capped at what there is room for. */
static int voiceFillDeviceNames(bool recording,
                                char names[][VOICE_DEVICE_NAME_MAX]) {
    int count = recording ? voiceRecordingDeviceCount()
                          : voicePlaybackDeviceCount();
    if (count > VOICE_DEVICE_LIST_CAP) count = VOICE_DEVICE_LIST_CAP;
    for (int i = 0; i < count; i++) {
        const char *name = recording ? voiceRecordingDeviceName(i)
                                     : voicePlaybackDeviceName(i);
        SDL_strlcpy(names[i], name ? name : "", VOICE_DEVICE_NAME_MAX);
    }
    return count;
}

/* Brings both lists up to date for this frame, called by each device combo.
 * The first check is for the second combo of a pair: both are drawn in the
 * same frame off one enumeration, so only the first of them does the work.
 * ImGui::GetFrameCount() counts frames in the current context, so a caller
 * drawing in a context of its own refills on its first draw, which is the
 * same answer the frame comparison gives. */
static void voiceRefreshDeviceLists(void) {
    int frame = ImGui::GetFrameCount();
    if (frame == s_voiceDeviceFrame) return;  /* second combo, same frame */
    if (frame != s_voiceDeviceFrame + 1) {
        s_voiceMicCount = voiceFillDeviceNames(true, s_voiceMicNames);
        s_voiceOutCount = voiceFillDeviceNames(false, s_voiceOutNames);
    }
    s_voiceDeviceFrame = frame;
}

/* The voice controls below are drawn by more than one caller, each passing
 * the width its own layout gives the control. */

extern "C" void imguiSettingsVoiceModeCombo(float comboWidth) {
    const char *voiceModeLabels[] = {
        langGetText(STR_DLGSETTINGS_VOICE_MODE_OFF),
        langGetText(STR_DLGSETTINGS_VOICE_MODE_PTT),
        langGetText(STR_DLGSETTINGS_VOICE_MODE_OPEN),
    };
    int curModeIdx = (int)voiceGetMode();
    if (curModeIdx < 0 || curModeIdx > 2) curModeIdx = 0;
    ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_VOICE_MODE));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##voicemode", voiceModeLabels[curModeIdx])) {
        for (int i = 0; i < 3; i++) {
            if (ImGui::Selectable(voiceModeLabels[i], curModeIdx == i)) {
                windowSetVoiceMode(i);
            }
        }
        ImGui::EndCombo();
    }
}

extern "C" bool imguiSettingsVoiceDeviceCombo(bool recording, float comboWidth) {
    voiceRefreshDeviceLists();
    int count = recording ? s_voiceMicCount : s_voiceOutCount;
    /* Left out entirely where there is nothing to choose between — the web
       build has the browser pick — rather than drawn as an empty control. */
    if (count <= 0) return false;
    char (*names)[VOICE_DEVICE_NAME_MAX] =
        recording ? s_voiceMicNames : s_voiceOutNames;
    /* Copied because picking an entry rewrites what the getter returns,
       and the rest of the list is compared against it after that. */
    char wanted[VOICE_DEVICE_NAME_MAX];
    SDL_strlcpy(wanted,
                recording ? voiceGetRecordingDevice() : voiceGetPlaybackDevice(),
                sizeof(wanted));
    /* The closed combo names the chosen device even when it is not
       plugged in, so an absent headset still reads as the choice rather
       than silently as the default it is running on. */
    ImGui::TextUnformatted(langGetText(recording
                                           ? STR_DLGSETTINGS_VOICE_MICDEVICE
                                           : STR_DLGSETTINGS_VOICE_OUTDEVICE));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo(recording ? "##voicemicdev" : "##voiceoutdev",
                          wanted[0] != '\0'
                              ? wanted
                              : langGetText(STR_DLGSETTINGS_VOICE_DEVICE_DEFAULT))) {
        if (ImGui::Selectable(langGetText(STR_DLGSETTINGS_VOICE_DEVICE_DEFAULT),
                              wanted[0] == '\0')) {
            if (recording) {
                voiceSetRecordingDevice("");
            } else {
                voiceSetPlaybackDevice("");
            }
        }
        for (int i = 0; i < count; i++) {
            /* Scoped by index: two devices can carry the same name. */
            ImGui::PushID(i);
            if (ImGui::Selectable(names[i], strcmp(names[i], wanted) == 0)) {
                if (recording) {
                    voiceSetRecordingDevice(names[i]);
                } else {
                    voiceSetPlaybackDevice(names[i]);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return true;
}

extern "C" void imguiSettingsVoiceMicGainSlider(float sliderWidth) {
    float gain = voiceGetMicGain();
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderFloat(langGetText(STR_DLGSETTINGS_VOICE_MICGAIN), &gain,
                           0.0f, 4.0f, "%.2fx")) {
        windowSetVoiceMicGain(gain);
    }
}

extern "C" void imguiSettingsVoiceLevelMeter(float barWidth) {
    ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_VOICE_LEVEL));
    ImGui::SameLine();
    ImGui::ProgressBar(voiceGetInputMeter(), ImVec2(barWidth, 0.0f));
    ImGui::SameLine();
    /* Spelt out both ways rather than a colour that only means something
       to players who can tell the two greens apart. */
    if (voiceIsTransmitting()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                           langGetText(STR_DLGSETTINGS_VOICE_TRANSMITTING));
    } else {
        ImGui::TextDisabled("%s",
                            langGetText(STR_DLGSETTINGS_VOICE_NOTTRANSMITTING));
    }
}

extern "C" void imguiSettingsVoiceMicTest(float barWidth) {
    /* Records first and plays back after, rather than monitoring live:
       on laptop speakers a live monitor is a feedback loop that howls.
       Scoped, because the cancel shares its label with the buttons the
       in-game overlay puts in this same window. */
    ImGui::PushID("voiceMicTest");
    VoiceMicTestState micTest = voiceMicTestGetState();
    if (micTest == VOICE_MICTEST_IDLE) {
        if (ImGui::Button(langGetText(STR_DLGSETTINGS_VOICE_LOOPBACK))) {
            voiceMicTestStart();
        }
    } else {
        if (ImGui::Button(langGetText(STR_CANCEL))) {
            voiceMicTestCancel();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(
            langGetText(micTest == VOICE_MICTEST_RECORDING
                            ? STR_DLGSETTINGS_VOICE_MICTEST_RECORDING
                            : STR_DLGSETTINGS_VOICE_MICTEST_PLAYING));
        ImGui::SameLine();
        ImGui::ProgressBar(voiceMicTestProgress(), ImVec2(barWidth, 0.0f));
    }
    ImGui::PopID();
}
#endif

/* -------------------------------------------------------
 * Display tab — the display controls shared by the pre-game dialog
 * and the in-game overlay: frame rate, window size, UI scale, full
 * screen, letterbox, the map HUD panels and Skin.  Frame rate,
 * letterbox and Skin apply in both; window size and UI scale only
 * apply in-game (ctx->inGame), and their results are returned via
 * ctx->pendingZoom / ctx->wantAtlasRebuild for the in-game shell to
 * apply after the frame, the full screen pick likewise via
 * ctx->pendingFullScreen.  A skin pick applies at once but needs the
 * tile sheet and sound set rebuilt, which the shell does after the
 * frame off ctx->wantSkinReload.
 * ------------------------------------------------------- */
extern "C" void imguiSettingsRenderDisplayTab(SettingsRenderCtx *ctx) {
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
    ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_FULLSCREEN));
#if !defined(__EMSCRIPTEN__)
    /* ---- Full screen ---- */
    if (!uiModeIsTablet() && !uiModeIsSteamDeck() && !uiShouldUseControllerMode()) {
        /* Read the window itself rather than the preference, so the tick still
           tells the truth after an OS-driven full screen change. */
        bool fs = (SDL_GetWindowFlags(sdl3DrawGetWindow()) & SDL_WINDOW_FULLSCREEN) != 0;
        if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_FULLSCREEN), &fs)) {
            /* Checkbox has already flipped fs to what the player asked for.
               Moving the window here would do it inside a live frame, so only
               record the request; each shell applies it once the frame ends. */
            ctx->pendingFullScreen = fs ? 1 : 0;
        }
        imguiHelpTooltip(langGetText(STR_DLGSETTINGS_FULLSCREEN_TIP));
    }
#endif

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

#if !defined(__EMSCRIPTEN__)
    /* ---- The full screen map's HUD panels ---- These follow the map view
       they belong to, which is desktop and Deck only.  Each slider is applied
       as it moves so the panel changes under it, and written to the
       preferences once it is let go. */
    if (!uiModeIsTablet()) {
        int trans = sdl3DrawGetNewswireTransparency();
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderInt(langGetText(STR_DLGSETTINGS_NEWS_TRANSPARENCY),
                             &trans, 0, OVERVIEW_HUD_TRANSPARENCY_MAX,
                             "%d%%")) {
            sdl3DrawSetNewswireTransparency(trans);
        }
        imguiHelpTooltip(langGetText(STR_DLGSETTINGS_NEWS_TRANSPARENCY_TIP));
        if (ImGui::IsItemDeactivatedAfterEdit()) gameFrontSaveCurrentPrefs();

        bool autoHide = sdl3DrawGetNewswireAutoHide();
        if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_NEWS_AUTOHIDE),
                            &autoHide)) {
            sdl3DrawSetNewswireAutoHide(autoHide);
            gameFrontSaveCurrentPrefs();
        }
        imguiHelpTooltip(langGetText(STR_DLGSETTINGS_NEWS_AUTOHIDE_TIP));

        int buildTrans = sdl3DrawGetBuildPanelTransparency();
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderInt(langGetText(STR_DLGSETTINGS_BUILD_TRANSPARENCY),
                             &buildTrans, 0, OVERVIEW_HUD_TRANSPARENCY_MAX,
                             "%d%%")) {
            sdl3DrawSetBuildPanelTransparency(buildTrans);
        }
        imguiHelpTooltip(langGetText(STR_DLGSETTINGS_BUILD_TRANSPARENCY_TIP));
        if (ImGui::IsItemDeactivatedAfterEdit()) gameFrontSaveCurrentPrefs();

        int statusTrans = sdl3DrawGetStatusPanelTransparency();
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderInt(langGetText(STR_DLGSETTINGS_STATUS_TRANSPARENCY),
                             &statusTrans, 0, OVERVIEW_HUD_TRANSPARENCY_MAX,
                             "%d%%")) {
            sdl3DrawSetStatusPanelTransparency(statusTrans);
        }
        imguiHelpTooltip(langGetText(STR_DLGSETTINGS_STATUS_TRANSPARENCY_TIP));
        if (ImGui::IsItemDeactivatedAfterEdit()) gameFrontSaveCurrentPrefs();
    }
#endif

    /* ---- The simplified view ---- Both surfaces it covers are desktop and
       Deck, the same as the map view settings above, so it follows their
       tablet test.  Each tick is applied and written as it is clicked: the
       views read the setting every frame, so the change shows at once. */
    if (!uiModeIsTablet()) {
        ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_MAPVIEW));

        bool simple = gfxGetSimplifiedZoomOut();
        if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_SIMPLEZOOM), &simple)) {
            gfxSetSimplifiedZoomOut(simple);
            gameFrontSaveCurrentPrefs();
        }
        imguiHelpTooltip(langGetText(STR_DLGSETTINGS_SIMPLEZOOM_TIP));

        /* Indented and disabled under its parent: with the simplified view
           off there is nothing for it to hold back, and the tick would read
           as a second, independent switch. */
        ImGui::Indent();
        ImGui::BeginDisabled(!simple);
        bool overviewOnly = gfxGetSimplifiedOverviewOnly();
        if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_SIMPLEZOOM_OVERVIEW),
                            &overviewOnly)) {
            gfxSetSimplifiedOverviewOnly(overviewOnly);
            gameFrontSaveCurrentPrefs();
        }
        imguiHelpTooltip(langGetText(STR_DLGSETTINGS_SIMPLEZOOM_OVERVIEW_TIP));
        ImGui::EndDisabled();
        ImGui::Unindent();
    }
#endif

    /* ---- Skin ---- */
    ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_SKIN));
    {
        /* The player's choice, not what loaded, so a skin whose files are
           not there right now still shows as picked.  Copied, not aliased:
           skinSetActive() below rewrites the registry's own copy of the
           requested id. */
        char activeId[SKIN_ID_MAX];
        {
            const char *a = skinGetRequested();
            SDL_strlcpy(activeId, a != nullptr ? a : "", sizeof(activeId));
        }

        /* The shut combo needs only the active skin, so it takes the name
           from that skin's own ini — falling back to the id's text after the
           ':', which is what the scan would name it — and the source tag
           from the id's prefix.  No id is the built-in assets, which have no
           source tag. */
        char preview[SKIN_NAME_MAX + 32];
        if (activeId[0] == '\0') {
            SDL_strlcpy(preview, langGetText(STR_DLGSKIN_DEFAULT),
                        sizeof(preview));
        } else {
            SkinInfo info;
            skinSourceReadIni(skinGetActiveSource(), &info);
            const char *name = info.name;
            if (name[0] == '\0') {
                const char *colon = strchr(activeId, ':');
                name = (colon != nullptr) ? colon + 1 : activeId;
            }
            SDL_snprintf(preview, sizeof(preview), "%s (%s)", name,
                         skinKindLabel(skinKindFromId(activeId)));
        }

        /* A Workshop download finishing while the picker is open leaves the
           rows stale.  The sync consumes Steam's installed edge and moves its
           generation on; a generation this block has not seen yet is that
           edge. */
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
        static uint32_t s_workshopGenSeen = 0;
        const uint32_t  workshopGen       = workshopSyncGeneration();
        const bool      workshopChanged   = workshopGen != s_workshopGenSeen;
        s_workshopGenSeen = workshopGen;
#else
        /* No Workshop on these builds: the stub's edge never fired here. */
        const bool workshopChanged = false;
#endif

        /* The item that just finished downloading may be the one the player
           picked before it existed locally.  activeId is a local copy, so this
           does not hand skinSetActive its own buffer. */
        if (workshopChanged && activeId[0] != '\0' &&
            skinGetActiveSource() == nullptr && skinSetActive(activeId)) {
            s_skinLoadFailed = false;
            ctx->wantSkinReload = true;
        }

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::BeginCombo("##skin", preview)) {
            if (!s_skinPopupWasOpen || workshopChanged) {
                s_skinPopupWasOpen = true;
                s_skinRows.clear();
                int found = skinScanCount();
                if (found > 0) {
                    s_skinRows.resize((size_t)found);
                    found = skinScan(s_skinRows.data(), found);
                    s_skinRows.resize((size_t)(found > 0 ? found : 0));
                }
            }

            /* -1 is the Default row, which has no id and no source tag.  An
               active skin deleted since it was picked is not in the rows
               either, and lands here too. */
            int cur = -1;
            for (int i = 0; i < (int)s_skinRows.size(); i++) {
                if (strcmp(s_skinRows[(size_t)i].id, activeId) == 0) {
                    cur = i;
                    break;
                }
            }

            /* Default first: clears back to the built-in assets. */
            if (ImGui::Selectable(langGetText(STR_DLGSKIN_DEFAULT),
                                  activeId[0] == '\0') &&
                activeId[0] != '\0') {
                skinSetActive("");
                s_skinLoadFailed = false;
                gameFrontSaveCurrentPrefs();
                ctx->wantSkinReload = true;
            }
            for (int i = 0; i < (int)s_skinRows.size(); i++) {
                const SkinEntry &e = s_skinRows[(size_t)i];
                bool sel = (cur == i);
                char tag[64];
                /* A pending row says what it is waiting on instead of naming
                   its source — it has no folder to be a source yet. */
                SDL_snprintf(tag, sizeof(tag), "(%s)",
                             e.pending ? langGetText(STR_DLGSKIN_DOWNLOADING)
                                       : skinKindLabel(e.kind));
                /* Leave the dim source tag room at the right rather than let
                   a full-width row push it outside the popup and clip it.
                   The row still takes the click everywhere but under the tag. */
                float nameW = ImGui::CalcTextSize(e.displayName).x;
                float rowW  = ImGui::GetContentRegionAvail().x -
                              ImGui::CalcTextSize(tag).x -
                              ImGui::GetStyle().ItemSpacing.x;
                if (rowW < nameW) rowW = nameW;
                ImGui::PushID(i);
                ImGui::BeginDisabled(e.pending);
                if (ImGui::Selectable(e.displayName, sel,
                                      ImGuiSelectableFlags_None,
                                      ImVec2(rowW, 0.0f)) &&
                    strcmp(e.id, activeId) != 0) {
                    if (skinSetActive(e.id)) {
                        s_skinLoadFailed = false;
                        gameFrontSaveCurrentPrefs();
                        ctx->wantSkinReload = true;
                    } else {
                        /* A failed load clears to the built-in assets, so put
                           the previous skin back instead of letting a bad pick
                           reset it. */
                        skinSetActive(activeId);
                        s_skinLoadFailed = true;
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tag);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        } else {
            s_skinPopupWasOpen = false;
        }
        if (s_skinLoadFailed) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "%s", langGetText(STR_DLGSKIN_LOADERR));
        }

        /* Name / author / notes off the active skin's skin.ini.  Default has
           no source, and three "N/A" rows say nothing, so show none. */
        SkinSource *src = skinGetActiveSource();
        if (src != nullptr) {
            SkinInfo info;
            const char *na = langGetText(STR_DLGSKIN_NA);
            skinSourceReadIni(src, &info);
            ImGui::Text("%s %s", langGetText(STR_DLGSKIN_NAME_LBL),
                        info.name[0]   != '\0' ? info.name   : na);
            ImGui::Text("%s %s", langGetText(STR_DLGSKIN_AUTHOR_LBL),
                        info.author[0] != '\0' ? info.author : na);
            ImGui::TextWrapped("%s %s", langGetText(STR_DLGSKIN_NOTES_LBL),
                               info.notes[0] != '\0' ? info.notes : na);

            /* Unlike the three above, no row at all when the author did not
               name a filter — an "N/A" recommendation says nothing.  The
               name is the same word the Texture filter dropdown below uses.
               Shown only; the player's own setting is what applies. */
            const char *recName = nullptr;
            switch (info.recommendedFilter) {
                case SKIN_FILTER_NEAREST:
                    recName = langGetText(STR_DLGSKIN_TEXFILTER_NEAREST);
                    break;
                case SKIN_FILTER_LINEAR:
                    recName = langGetText(STR_DLGSKIN_TEXFILTER_LINEAR);
                    break;
                case SKIN_FILTER_PIXELART:
                    recName = langGetText(STR_DLGSKIN_TEXFILTER_PIXELART);
                    break;
                default:
                    break;
            }
            if (recName != nullptr) {
                ImGui::Text("%s %s", langGetText(STR_DLGSKIN_RECFILTER_LBL),
                            recName);
            }
        }

        /* Sample of the sheet that is live right now.  The tiles reload
           after the frame, so the strip trails a selection by a frame. */
        {
            static const struct { int x, y; } previewTiles[] = {
                { TANK_SELF_0_X,  TANK_SELF_0_Y  },
                { TANK_SELF_1_X,  TANK_SELF_1_Y  },
                { TANK_SELF_2_X,  TANK_SELF_2_Y  },
                { TANK_SELF_3_X,  TANK_SELF_3_Y  },
                { TANK_SELF_4_X,  TANK_SELF_4_Y  },
                { TANK_SELF_5_X,  TANK_SELF_5_Y  },
                { TANK_SELF_6_X,  TANK_SELF_6_Y  },
                { TANK_SELF_7_X,  TANK_SELF_7_Y  },
                { TANK_SELF_8_X,  TANK_SELF_8_Y  },
                { TANK_SELF_9_X,  TANK_SELF_9_Y  },
                { TANK_SELF_10_X, TANK_SELF_10_Y },
                { TANK_SELF_11_X, TANK_SELF_11_Y },
                { TANK_SELF_12_X, TANK_SELF_12_Y },
                { TANK_SELF_13_X, TANK_SELF_13_Y },
                { TANK_SELF_14_X, TANK_SELF_14_Y },
                { TANK_SELF_15_X, TANK_SELF_15_Y },
                { BASE_GOOD_X,    BASE_GOOD_Y    },
                { PILL_GOOD15_X,  PILL_GOOD15_Y  },
                { GRASS_X,        GRASS_Y        },
                { ROAD_CROSS_X,   ROAD_CROSS_Y   },
                { SWAMP_X,        SWAMP_Y        },
            };
            const int n = (int)(sizeof(previewTiles) / sizeof(previewTiles[0]));
            for (int i = 0; i < n; i++) {
                if (i > 0) ImGui::SameLine(0.0f, 1.0f);
                imguiDrawTileIcon(previewTiles[i].x, previewTiles[i].y);
            }
        }

        if (ImGui::Button(langGetText(STR_DLGSKIN_OPENFOLDER))) {
            /* SDL_GetPrefPath already ends in a separator. */
            const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
            if (prefDir != nullptr) {
                char dir[SKIN_PATH_MAX];
                char url[SKIN_PATH_MAX + 16];
                SDL_snprintf(dir, sizeof(dir), "%sskins", prefDir);
                SDL_free((void *)prefDir);
                SDL_CreateDirectory(dir);
                /* file:// wants forward slashes and an extra leading one for
                   a Windows "C:\..." path, giving "file:///C:/...". */
                SDL_snprintf(url, sizeof(url), "file://%s%s",
                             dir[0] == '/' ? "" : "/", dir);
                for (char *c = url; *c != '\0'; c++) {
                    if (*c == '\\') *c = '/';
                }
                imguiOpenUrl(url);
            }
        }
        imguiHandOnHover();

        /* Runtime check, not an #ifdef: the stub build answers false, so the
           button simply isn't there when Steam isn't running. */
        if (steam_workshop_available()) {
            ImGui::SameLine();
            if (ImGui::Button(langGetText(STR_DLGSKIN_BROWSE_WORKSHOP))) {
                steam_workshop_open_browse_page();
            }
            imguiHandOnHover();

#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
            /* Publishing is for a skin the player put in their own folder and
               that actually loaded: the built-in art has no files to send, a
               Workshop skin belongs to whoever published it, and a skin that
               failed to load has no source to draw a preview from. */
            const char *reqId = skinGetRequested();
            const bool  canPublish =
                reqId != nullptr && reqId[0] != '\0' &&
                skinKindFromId(reqId) == SKIN_KIND_USER &&
                skinGetActiveSource() != nullptr;

            ImGui::SameLine();
            ImGui::BeginDisabled(!canPublish);
            if (ImGui::Button(langGetText(STR_DLGSKIN_PUBLISH))) {
                /* The picker's rows only live while its combo is open, so the
                   skin's path on disk comes from a scan made here. */
                char path[SKIN_PATH_MAX];
                path[0] = '\0';
                int found = skinScanCount();
                if (found > 0) {
                    std::vector<SkinEntry> rows((size_t)found);
                    found = skinScan(rows.data(), found);
                    for (int i = 0; i < found; i++) {
                        if (strcmp(rows[(size_t)i].id, reqId) == 0) {
                            SDL_strlcpy(path, rows[(size_t)i].path,
                                        sizeof(path));
                            break;
                        }
                    }
                }
                /* No path is a skin that went away since it was picked; there
                   is nothing to publish, so the modal does not open. */
                if (path[0] != '\0') {
                    /* The window takes these only for a fresh start; on the
                       skin whose publish is still running it keeps the live
                       state instead. */
                    SkinInfo info;
                    skinSourceReadIni(skinGetActiveSource(), &info);
                    const char *name = info.name;
                    if (name[0] == '\0') {
                        const char *colon = strchr(reqId, ':');
                        name = (colon != nullptr) ? colon + 1 : reqId;
                    }
                    if (workshopPublishPrepare(path, name, info.notes,
                                               info.workshopId,
                                               info.workshopAuthor)) {
                        SDL_strlcpy(s_skinPublishPath, path,
                                    sizeof(s_skinPublishPath));
                        ImGui::OpenPopup(s_skinPublishSpec.popupId);
                    }
                }
            }
            ImGui::EndDisabled();
            imguiHandOnHover();
            /* A disabled item is not hovered as far as ImGui is concerned
               unless it is asked for, and the reason it is disabled is exactly
               what the player needs to read. */
            if (!canPublish &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                     ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s",
                                  langGetText(STR_DLGSKIN_PUBLISH_NEEDUSER));
            }

            workshopPublishDraw(&s_skinPublishSpec);
#endif  /* !BOLO_MOBILE && !__EMSCRIPTEN__ */
        }

        /* Tile detail.  A pick only shows once the sheet is rebuilt, so it
           asks for a skin reload the same way picking a skin does.  Nothing
           to choose between when the skin has no art above its base size,
           which is the common case: all three modes build the same sheet.
           tileLoaderGetDensityInfo caches on the source pointer, so asking
           every frame is a pointer compare, not a rescan. */
        {
            const char *detailLabels[] = {
                langGetText(STR_DLGSKIN_TILEDETAIL_CLASSIC),
                langGetText(STR_DLGSKIN_TILEDETAIL_MATCHZOOM),
                langGetText(STR_DLGSKIN_TILEDETAIL_HIGH),
            };
            int tdIdx = (int)gfxGetTileDetail();
            if (tdIdx < 0 || tdIdx > 2) tdIdx = 0;
            /* Each mode reads a different field of the scan: Match to zoom
               takes one density for the whole sheet (highestAll), High detail
               takes each sprite's own (so it is worth having as soon as any
               sprite is finer, which is what highestAny says).  A skin whose
               finer art covers only some sprites therefore has something to
               choose between, but not Match to zoom — that builds Classic's
               tiles. */
            const SkinDensityInfo *di =
                tileLoaderGetDensityInfo(skinGetActiveSource());
            bool allOneSize    = di->highestAny <= 1;
            bool zoomIsClassic = di->highestAll <= 1;

            ImGui::BeginDisabled(allOneSize);
            ImGui::TextUnformatted(langGetText(STR_DLGSKIN_TILEDETAIL));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            if (ImGui::BeginCombo("##tiledetail", detailLabels[tdIdx])) {
                for (int i = 0; i < 3; i++) {
                    bool sel = (tdIdx == i);
                    bool sameAsClassic = (!allOneSize && zoomIsClassic &&
                                          i == TILE_DETAIL_MATCH_ZOOM);
                    ImGui::BeginDisabled(sameAsClassic);
                    if (ImGui::Selectable(detailLabels[i], sel) && i != tdIdx) {
                        gfxSetTileDetail((GfxTileDetail)i);
                        gameFrontSaveCurrentPrefs();
                        ctx->wantSkinReload = true;
                    }
                    ImGui::EndDisabled();
                    /* A disabled item is not hovered as far as ImGui is
                       concerned unless it is asked for, and the reason it is
                       disabled is exactly what the player needs to read. */
                    if (sameAsClassic &&
                        ImGui::IsItemHovered(
                            ImGuiHoveredFlags_AllowWhenDisabled |
                            ImGuiHoveredFlags_ForTooltip)) {
                        ImGui::SetTooltip(
                            "%s",
                            langGetText(STR_DLGSKIN_TILEDETAIL_PARTIAL_TIP));
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (allOneSize) {
                imguiHelpTooltip(langGetText(STR_DLGSKIN_TILEDETAIL_ONESIZE_TIP));
            }
        }

        /* Animation smoothness, and the shell override.  Both are read where
           a sprite's screen position is worked out, so a pick shows on the
           next frame with no sheet rebuild.  Neither is greyed by what the
           skin holds: this is about where a sprite is put on screen, not how
           much detail it has, and it shows at any zoom above 1 even with
           16x16 art. */
        {
            const char *smoothLabels[] = {
                langGetText(STR_DLGSKIN_ANIMSMOOTH_CLASSIC),
                langGetText(STR_DLGSKIN_ANIMSMOOTH_PIXEL),
                langGetText(STR_DLGSKIN_ANIMSMOOTH_SMOOTH),
            };
            int asIdx = (int)gfxGetAnimSmoothness();
            if (asIdx < 0 || asIdx > 2) asIdx = 0;

            ImGui::TextUnformatted(langGetText(STR_DLGSKIN_ANIMSMOOTH));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            if (ImGui::BeginCombo("##animsmooth", smoothLabels[asIdx])) {
                for (int i = 0; i < 3; i++) {
                    bool sel = (asIdx == i);
                    if (ImGui::Selectable(smoothLabels[i], sel) && i != asIdx) {
                        gfxSetAnimSmoothness((GfxAnimSmoothness)i);
                        gameFrontSaveCurrentPrefs();
                    }
                }
                ImGui::EndCombo();
            }

            /* Redundant once everything already moves smoothly, so it only
               appears in the other two modes. */
            if (gfxGetAnimSmoothness() != GFX_ANIM_SMOOTH) {
                bool ss = gfxGetSmoothShells();
                if (ImGui::Checkbox(langGetText(STR_DLGSKIN_SMOOTHSHELLS), &ss)) {
                    gfxSetSmoothShells(ss);
                    gameFrontSaveCurrentPrefs();
                }
            }
        }

        /* Texture filter.  Set on the sheet that is already there, so a
           pick shows on the next frame with no rebuild.  Not greyed by
           what the skin holds: it applies to whatever art is loaded. */
        {
            const char *filterLabels[] = {
                langGetText(STR_DLGSKIN_TEXFILTER_NEAREST),
                langGetText(STR_DLGSKIN_TEXFILTER_LINEAR),
                langGetText(STR_DLGSKIN_TEXFILTER_PIXELART),
            };
            int tfIdx = (int)gfxGetTextureFilter();
            if (tfIdx < 0 || tfIdx > 2) tfIdx = 0;

            /* What the active skin's author recommends, if anything, so the
               entry they named can say so.  The ini is cached after the
               first read, and a NULL source answers SKIN_FILTER_NONE, so
               reading it every frame costs a struct copy. */
            SkinInfo recInfo;
            skinSourceReadIni(skinGetActiveSource(), &recInfo);
            const int recFilter = recInfo.recommendedFilter;

            ImGui::TextUnformatted(langGetText(STR_DLGSKIN_TEXFILTER));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            /* The closed combo shows the plain name: the tag belongs in the
               list, beside the entry it is about. */
            if (ImGui::BeginCombo("##texfilter", filterLabels[tfIdx])) {
                for (int i = 0; i < 3; i++) {
                    bool sel = (tfIdx == i);
                    char label[128];
                    if (i == recFilter) {
                        SDL_snprintf(label, sizeof(label), "%s %s",
                                     filterLabels[i],
                                     langGetText(STR_DLGSKIN_RECOMMENDED_TAG));
                    } else {
                        SDL_strlcpy(label, filterLabels[i], sizeof(label));
                    }
                    if (ImGui::Selectable(label, sel) && i != tfIdx) {
                        gfxSetTextureFilter((GfxTextureFilter)i);
                        gameFrontSaveCurrentPrefs();
                        sdl3DrawSetTilesScaleMode(
                            sdl3DrawScaleModeForFilter((GfxTextureFilter)i));
                    }
                }
                ImGui::EndCombo();
            }
            imguiHelpTooltip(langGetText(STR_DLGSKIN_TEXFILTER_TIP));
        }

        /* Fog of war style.  Read where each view washes its hidden squares,
           so a pick shows on the next frame with no rebuild of anything.  Not
           greyed by what the server allows: the wash is this client painting
           ground it is already being sent, and the pick changes the colour of
           that paint and nothing else. */
        {
            const char *fogLabels[FOG_STYLE_COUNT] = {
                langGetText(STR_DLGSKIN_FOGSTYLE_GREY),
                langGetText(STR_DLGSKIN_FOGSTYLE_DARK),
                langGetText(STR_DLGSKIN_FOGSTYLE_DARKROADS),
                langGetText(STR_DLGSKIN_FOGSTYLE_NONE),
            };
            const char *fogTips[FOG_STYLE_COUNT] = {
                langGetText(STR_DLGSKIN_FOGSTYLE_GREY_TIP),
                langGetText(STR_DLGSKIN_FOGSTYLE_DARK_TIP),
                langGetText(STR_DLGSKIN_FOGSTYLE_DARKROADS_TIP),
                langGetText(STR_DLGSKIN_FOGSTYLE_NONE_TIP),
            };
            int fsIdx = (int)gfxGetFogStyle();
            if (fsIdx < 0 || fsIdx >= FOG_STYLE_COUNT) fsIdx = 0;

            ImGui::TextUnformatted(langGetText(STR_DLGSKIN_FOGSTYLE));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            if (ImGui::BeginCombo("##fogstyle", fogLabels[fsIdx])) {
                for (int i = 0; i < FOG_STYLE_COUNT; i++) {
                    bool sel = (fsIdx == i);
                    if (ImGui::Selectable(fogLabels[i], sel) && i != fsIdx) {
                        gfxSetFogStyle((FogStyle)i);
                        gameFrontSaveCurrentPrefs();
                    }
                    /* One line each, on the entry it is about, so the list
                       says what it is offering without a tip per row in the
                       closed control. */
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                        ImGui::SetTooltip("%s", fogTips[i]);
                    }
                }
                ImGui::EndCombo();
            }
            imguiHelpTooltip(langGetText(STR_DLGSKIN_FOGSTYLE_TIP));

            /* None is the one entry a player could read as switching fog of
               war off, so it says plainly that it has not.  Read back from
               the store rather than from fsIdx above, which was taken before
               the combo and is a frame behind a pick made in it.  Wrapped and
               dimmed the way the other notes in this dialog are. */
            if (gfxGetFogStyle() == FOG_STYLE_NONE) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                       ImGui::GetFontSize() * 20.0f);
                ImGui::TextDisabled(
                    "%s", langGetText(STR_DLGSKIN_FOGSTYLE_NONE_NOTE));
                ImGui::PopTextWrapPos();
            }
        }
    }
}

/* -------------------------------------------------------
 * Sound tab — the audio controls shared by the pre-game dialog and
 * the in-game overlay: the sound effect, background sound and
 * keepalive toggles, the volume slider, and the voice section
 * (mode, the push to talk key, devices, microphone gain and test,
 * echo cancelling and playback volume).  Each control applies
 * through its own setter as it is changed, so nothing here is
 * returned to the shell on ctx.
 * ------------------------------------------------------- */
extern "C" void imguiSettingsRenderSoundTab(SettingsRenderCtx *ctx) {
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
    /* The three volume rows read label, slider, value, so the sliders have to
       start at a common x or the group looks ragged.  Widest of the labels
       this build draws, measured once. */
    float volLabelW = ImGui::CalcTextSize(langGetText(STR_DLGSETTINGS_MASTER_VOLUME)).x;
    {
        float w = ImGui::CalcTextSize(langGetText(STR_DLGSETTINGS_EFFECTS_VOLUME)).x;
        if (w > volLabelW) volLabelW = w;
    }
#if defined(WINBOLO_VOICE)
    {
        float w = ImGui::CalcTextSize(langGetText(STR_DLGSETTINGS_VOICE_VOLUME)).x;
        if (w > volLabelW) volLabelW = w;
    }
#endif
    const float volSliderX = volLabelW + ImGui::GetStyle().ItemSpacing.x;
    {
        int vol = windowMasterVolume;
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_MASTER_VOLUME));
        ImGui::SameLine(volSliderX);
        ImGui::SetNextItemWidth(200.0f);
        /* Empty format, and a ## id, so the slider draws neither the value
           inside itself nor a label after it; both go beside it instead. */
        if (ImGui::SliderInt("##mastervolume", &vol, 0, 100, "")) windowSetMasterVolume(vol);
        ImGui::SameLine();
        ImGui::Text("%d%%", vol);
    }
    {
        int vol = soundVolume;
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_EFFECTS_VOLUME));
        ImGui::SameLine(volSliderX);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderInt("##effectsvolume", &vol, 0, 100, "")) windowSetSoundVolume(vol);
        ImGui::SameLine();
        ImGui::Text("%d%%", vol);
    }
#if defined(WINBOLO_VOICE)
    /* The third of the three, drawn here rather than down in the voice
       section so all of them read as one group.  Its own disable, since it is
       outside the one that section puts around itself. */
    {
        bool voiceOnForVolume = voiceIsEnabled();
        float vol = voiceGetOutputVolume();
        /* The disable takes in the label and the value as well as the slider,
           so the row greys out as one. */
        if (!voiceOnForVolume) ImGui::BeginDisabled();
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_VOICE_VOLUME));
        ImGui::SameLine(volSliderX);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderFloat("##voicevolume", &vol, 0.0f, 2.0f, "")) {
            windowSetVoiceVolume(vol);
        }
        ImGui::SameLine();
        ImGui::Text("%.2fx", vol);
        if (!voiceOnForVolume) ImGui::EndDisabled();
    }

    /* ---- Voice ---- */
    ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_VOICE));
    /* Ahead of every control below, so the microphone is held open for the
       whole of this frame: the input meter further down needs a live level to
       show, and the controls in between can reach into the voice module,
       which decides there and then whether anything still wants the device.
       Not drawing this section is what gives the hold up again — the voice
       tick takes it back — so only the pre-game dialog, whose exit stops the
       ticking, says so explicitly when it closes. */
    voiceSettingsSectionDrawn();
    /* Read the master switch once: the checkbox below writes it, and
       BeginDisabled / EndDisabled have to be told the same answer. */
    bool voiceOn = voiceIsEnabled();
    if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_VOICE_ENABLE), &voiceOn)) {
        windowSetVoiceEnabled(voiceOn);
    }
    /* Said once, above the controls, on a server that drops what it is sent:
       the devices, the level meter and the microphone test below are all
       local and still worth having, but nothing said here reaches anyone. */
    if (voiceServerHasVoiceOff()) {
        ImGui::TextDisabled("%s", langGetText(STR_DLGSETTINGS_VOICE_SERVER_OFF));
    }
    if (!voiceOn) ImGui::BeginDisabled();
    imguiSettingsVoiceModeCombo(140.0f);
    /* The binding itself is set in Key Setup; showing it here is so the
       player can see which key push to talk is on without leaving. */
    if (voiceGetMode() == VOICE_MODE_PTT) {
        keyItems pttKeys;
        windowGetKeys(&pttKeys);
        const char *pttName =
            SDL_GetScancodeName((SDL_Scancode)pttKeys.kiPushToTalk);
        if (!pttName || pttName[0] == '\0') {
            pttName = langGetText(STR_DLGKEYSETUP_NONE_VAL);
        }
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_VOICE_PTTKEY));
        ImGui::SameLine();
        ImGui::TextUnformatted(pttName);
    }
    imguiSettingsVoiceDeviceCombo(true, 240.0f);
    imguiSettingsVoiceDeviceCombo(false, 240.0f);
    imguiSettingsVoiceMicGainSlider(200.0f);
    imguiSettingsVoiceLevelMeter(200.0f);
    imguiSettingsVoiceMicTest(200.0f);
#if defined(WINBOLO_VOICE_AEC)
    {
        /* Three states, and the checkbox shows the saved preference in all
           of them: the setting is saved either way, so the row reports who
           is cancelling rather than taking the player's choice away.  Greyed
           when the system is doing the work, because then there is nothing
           here to switch, and greyed when no canceller came up at all. */
        bool aecPlatform = windowGetVoiceEchoCancelPlatform();
        bool aecOurs = !aecPlatform && windowGetVoiceEchoCancelAvailable();
        bool echoCancel = windowGetVoiceEchoCancel();
        if (!aecOurs) ImGui::BeginDisabled();
        if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_VOICE_ECHOCANCEL),
                            &echoCancel)) {
            windowSetVoiceEchoCancel(echoCancel);
        }
        if (!aecOurs) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", langGetText(
                aecPlatform ? STR_DLGSETTINGS_VOICE_ECHOCANCEL_PLATFORM
                            : STR_DLGSETTINGS_VOICE_ECHOCANCEL_UNAVAILABLE));
        }
    }
#endif
    {
        bool micIcons = windowGetShowTankMicIcons();
        if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_VOICE_TANKICONS), &micIcons)) {
            windowSetShowTankMicIcons(micIcons);
        }
    }
    if (!voiceOn) ImGui::EndDisabled();
#endif
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

/* And again for the Scenario Directory field, on statics of its own for the
 * same reason. */
static char s_hostingScnPickedDir[FILENAME_MAX];
static bool s_hostingScnDirPicked = false;

static void SDLCALL hostingScenarioDirDialogCallback(void *userdata,
                                                     const char *const *filelist,
                                                     int filter) {
    (void)userdata;
    (void)filter;
    if (filelist && filelist[0]) {
        SDL_strlcpy(s_hostingScnPickedDir, filelist[0], FILENAME_MAX);
        s_hostingScnDirPicked = true;
    }
}

/* And for the Script Upload Directory field, on statics of its own for the
 * same reason. */
static char s_hostingScriptPickedDir[FILENAME_MAX];
static bool s_hostingScriptDirPicked = false;

static void SDLCALL hostingScriptUploadDirDialogCallback(void *userdata,
                                                         const char *const *filelist,
                                                         int filter) {
    (void)userdata;
    (void)filter;
    if (filelist && filelist[0]) {
        SDL_strlcpy(s_hostingScriptPickedDir, filelist[0], FILENAME_MAX);
        s_hostingScriptDirPicked = true;
    }
}

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

/* One visibility row: the category label, a 4-way policy combo, and — only
 * while that combo reads Decay — the decay-seconds box.  The combo index is
 * the ViewPolicy value, the enum being in display order.  An edit comes back
 * in *policy / *secs with the matching flag set, so the caller pushes just
 * the field the host touched through that category's setter. */
static void hostingViewRow(const char *id, langid label, int *policy,
                           int *secs, bool *policyEdited, bool *secsEdited) {
    const char *modes[4] = {
        langGetText(STR_DLGLOBBY_VIEW_ALWAYS),
        langGetText(STR_DLGLOBBY_VIEW_KEY),
        langGetText(STR_DLGLOBBY_VIEW_DECAY),
        langGetText(STR_DLGLOBBY_VIEW_OFF)
    };
    *policyEdited = false;
    *secsEdited   = false;

    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(langGetText(label));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::Combo("##policy", policy, modes, 4)) {
        *policyEdited = true;
    }
    if (*policy == (int)viewPolicyDecay) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::InputInt("##decay", secs, 1, 5)) {
            *secsEdited = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS));
    }
    ImGui::PopID();
}

/* The label to the left of a value field, as the Display tab draws its
 * rows: text first, then the widget on the same line under a hidden "##"
 * label. ImGui's own label goes to the right of a widget, which is where a
 * checkbox keeps it and where a number, a path or a choice should not. */
static void hostingLabel(langid label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(langGetText(label));
    ImGui::SameLine();
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
        hostingLabel(STR_DLGSETTINGS_HOSTING_PORT);
        if (ImGui::InputInt("##hostingport", &port)) {
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
        hostingLabel(STR_DLGSETTINGS_HOSTING_MAXSPEC);
        if (ImGui::InputInt("##hostingmaxspec", &maxSpec)) {
            if (maxSpec < 1)  maxSpec = 1;
            if (maxSpec > 32) maxSpec = 32;
            gameFrontSetHostingMaxSpec(maxSpec);
        }
        ImGui::EndDisabled();
    }

    /* ---- Map scripts ---- */
    {
        bool runScripts = gameFrontHostingScripts;
        if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_HOSTING_SCRIPTS),
                            &runScripts)) {
            gameFrontSetHostingScripts(runScripts);
        }
        /* The narrower choice under it: what this host does with scripts
         * players send it. Off refuses them, and a map a player uploaded
         * here plays plainly even when it carries a scenario inside the .map
         * file; Allow keeps them for the session; Allow and keep saves them
         * to the directory below. Greyed while scripts are off altogether,
         * which already turns them down; the value is kept so switching
         * scripts back on restores what the host chose. */
        ImGui::BeginDisabled(!runScripts);
        {
            /* Combo display order is Off / Allow / Persist, but the enum
             * values are not in that order (ALLOW=0, OFF=1, PERSIST=2) — map
             * explicitly. */
            static const int kScriptPolicyByIndex[3] = {
                SCRIPT_UPLOAD_OFF, SCRIPT_UPLOAD_ALLOW, SCRIPT_UPLOAD_PERSIST
            };
            const char *scriptPolicyItems[3] = {
                langGetText(STR_DLGSETTINGS_HOSTING_UPLOAD_OFF),
                langGetText(STR_DLGSETTINGS_HOSTING_SCRIPTUPLOAD_SESSION),
                langGetText(STR_DLGSETTINGS_HOSTING_SCRIPTUPLOAD_KEEP)
            };
            int scriptIdx = 1;  /* default Allow */
            for (int i = 0; i < 3; ++i) {
                if (kScriptPolicyByIndex[i] == gameFrontHostingScriptUploadPolicy) {
                    scriptIdx = i;
                    break;
                }
            }
            hostingLabel(STR_DLGSETTINGS_HOSTING_SCRIPTUPLOADS);
            if (ImGui::Combo("##hostingscriptuploads", &scriptIdx,
                             scriptPolicyItems, 3)) {
                gameFrontSetHostingScriptUploadPolicy(
                    kScriptPolicyByIndex[scriptIdx]);
            }

            /* Directory and caps only bite on Persist (Off/Allow never keep
             * a script past the session). */
            if (gameFrontHostingScriptUploadPolicy == SCRIPT_UPLOAD_PERSIST) {
                static char scriptDirBuf[FILENAME_MAX];
                static bool scriptDirEditing = false;
                if (s_hostingScriptDirPicked) {
                    gameFrontSetHostingScriptUploadDir(s_hostingScriptPickedDir);
                    s_hostingScriptDirPicked = false;
                }
                if (!scriptDirEditing) {
                    SDL_strlcpy(scriptDirBuf, gameFrontHostingScriptUploadDir,
                                sizeof(scriptDirBuf));
                }
                hostingLabel(STR_DLGSETTINGS_HOSTING_SCRIPTUPLOADDIR);
                bool scriptCommit = ImGui::InputText(
                    "##hostingscriptuploaddirfield",
                    scriptDirBuf, sizeof(scriptDirBuf),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                scriptDirEditing = ImGui::IsItemActive();
                if (scriptCommit || ImGui::IsItemDeactivatedAfterEdit()) {
                    gameFrontSetHostingScriptUploadDir(scriptDirBuf);
                }
                /* Distinct ID from the other Browse buttons, which share the
                 * same label and can be on screen at the same time. */
                ImGui::PushID("hostingscriptuploaddir");
                if (ImGui::Button(langGetText(STR_MAPEDIT_BROWSE))) {
                    SDL_Window *win = sdl3DrawGetWindow();
                    const char *loc = gameFrontHostingScriptUploadDir[0]
                                          ? gameFrontHostingScriptUploadDir
                                          : NULL;
                    SDL_ShowOpenFolderDialog(hostingScriptUploadDirDialogCallback,
                                             NULL, win, loc, false);
                }
                ImGui::PopID();

                int scriptMaxFiles = gameFrontHostingScriptUploadMaxFiles;
                hostingLabel(STR_DLGSETTINGS_HOSTING_UPLOAD_MAXFILES);
                if (ImGui::InputInt("##hostingscriptuploadmaxfiles",
                                    &scriptMaxFiles)) {
                    if (scriptMaxFiles < 1)   scriptMaxFiles = 1;
                    if (scriptMaxFiles > 255) scriptMaxFiles = 255;
                    gameFrontSetHostingScriptUploadMaxFiles(scriptMaxFiles);
                }
                int scriptMaxStorage = gameFrontHostingScriptUploadMaxStorage;
                hostingLabel(STR_DLGSETTINGS_HOSTING_UPLOAD_MAXSTORAGE);
                if (ImGui::InputInt("##hostingscriptuploadmaxstorage",
                                    &scriptMaxStorage)) {
                    if (scriptMaxStorage < 1)    scriptMaxStorage = 1;
                    if (scriptMaxStorage > 4095) scriptMaxStorage = 4095;
                    gameFrontSetHostingScriptUploadMaxStorage(scriptMaxStorage);
                }
            }

            /* Whether players may save a copy of this host's mods and
             * scenarios. Greyed with the rest, since a host that runs no
             * scripts has none to share. */
            bool shareScripts = gameFrontHostingShareScripts;
            if (ImGui::Checkbox(
                    langGetText(STR_DLGSETTINGS_HOSTING_SHARESCRIPTS),
                    &shareScripts)) {
                gameFrontSetHostingShareScripts(shareScripts);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "%s", langGetText(STR_DLGSETTINGS_HOSTING_SHARESCRIPTS_TIP));
            }
        }
        ImGui::EndDisabled();

        /* The scenarios this host offers on their own, independently of any
         * map — what the lobby's mod chooser lists. Same shape as the Upload
         * Directory field below: an editable field that is the primary input
         * and the fallback where there is no native folder dialog, with
         * Browse filling it in. */
        static char scnDirBuf[FILENAME_MAX];
        static bool scnDirEditing = false;
        if (s_hostingScnDirPicked) {
            gameFrontSetHostingScenarioDir(s_hostingScnPickedDir);
            s_hostingScnDirPicked = false;
        }
        if (!scnDirEditing) {
            SDL_strlcpy(scnDirBuf, gameFrontHostingScenarioDir,
                        sizeof(scnDirBuf));
        }
        hostingLabel(STR_DLGSETTINGS_HOSTING_SCENARIODIR);
        bool scnCommit = ImGui::InputText(
            "##hostingscenariodirfield",
            scnDirBuf, sizeof(scnDirBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        scnDirEditing = ImGui::IsItemActive();
        if (scnCommit || ImGui::IsItemDeactivatedAfterEdit()) {
            gameFrontSetHostingScenarioDir(scnDirBuf);
        }
        /* Distinct ID from the Upload Directory and Log Directory Browse
         * buttons, which share the same label and can be on screen at the
         * same time. */
        ImGui::PushID("hostingscenariodir");
        if (ImGui::Button(langGetText(STR_MAPEDIT_BROWSE))) {
            SDL_Window *win = sdl3DrawGetWindow();
            const char *loc = gameFrontHostingScenarioDir[0]
                                  ? gameFrontHostingScenarioDir : NULL;
            SDL_ShowOpenFolderDialog(hostingScenarioDirDialogCallback, NULL,
                                     win, loc, false);
        }
        ImGui::PopID();
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
        hostingLabel(STR_DLGSETTINGS_HOSTING_MAPUPLOADS);
        if (ImGui::Combo("##hostingmapuploads", &idx, policyItems, 3)) {
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
            hostingLabel(STR_DLGSETTINGS_HOSTING_UPLOADDIR);
            bool commit = ImGui::InputText(
                "##hostinguploaddirfield",
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
            hostingLabel(STR_DLGSETTINGS_HOSTING_UPLOAD_MAXFILES);
            if (ImGui::InputInt("##hostinguploadmaxfiles", &maxFiles)) {
                if (maxFiles < 1)   maxFiles = 1;
                if (maxFiles > 255) maxFiles = 255;
                gameFrontSetHostingUploadMaxFiles(maxFiles);
            }
            int maxStorage = gameFrontHostingUploadMaxStorage;
            hostingLabel(STR_DLGSETTINGS_HOSTING_UPLOAD_MAXSTORAGE);
            if (ImGui::InputInt("##hostinguploadmaxstorage", &maxStorage)) {
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
            hostingLabel(STR_DLGSETTINGS_HOSTING_LOGDIR);
            bool commit = ImGui::InputText(
                "##hostinglogdirfield",
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

    /* ---- Voice chat ----
     * What the hosted server does with the voice its clients send it.
     * Fixed when the server starts, so this is read at host time and there
     * is no lobby control for it. */
    {
        /* Combo display order is On / Off / Proximity, and the enum values
         * happen to run ON=0, OFF=1, PROXIMITY=2 — map explicitly anyway so
         * a later reordering of either list cannot silently mismatch. */
        static const int kVoiceByIndex[3] = {
            serverVoiceOn, serverVoiceOff, serverVoiceProximity
        };
        const char *voiceItems[3] = {
            langGetText(STR_DLGSETTINGS_HOSTING_VOICE_ON),
            langGetText(STR_DLGSETTINGS_HOSTING_VOICE_OFF),
            langGetText(STR_DLGSETTINGS_HOSTING_VOICE_PROXIMITY)
        };
        int idx = 0;  /* default On */
        for (int i = 0; i < 3; ++i) {
            if (kVoiceByIndex[i] == gameFrontHostingVoiceMode) { idx = i; break; }
        }
        hostingLabel(STR_DLGSETTINGS_HOSTING_VOICE);
        if (ImGui::Combo("##hostingvoice", &idx, voiceItems, 3)) {
            gameFrontSetHostingVoiceMode(kVoiceByIndex[idx]);
        }
        /* Proximity is stored and sent but nothing acts on it yet, so say so
         * rather than let a host think picking it changed anything. */
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", langGetText(STR_DLGSETTINGS_HOSTING_VOICE_TIP));
        }
    }

    /* ---- Visibility ----
     * The seven view rules a game hosted from here starts with: the
     * pill / base / allied-tank policies, classic mode, allies in trees,
     * the overview window and line of sight.  The host can still change
     * them from the lobby once the game is up, and this dialog has no
     * path into a running game.  The setters persist to prefs and clamp
     * the seconds, so the values go through them untouched. */
    {
        int policy, secs;
        bool policyEdited, secsEdited;

        ImGui::SeparatorText(langGetText(STR_DLGLOBBY_VISIBILITY_LBL));

        policy = gameFrontViewPillPolicy;
        secs   = gameFrontViewPillDecaySecs;
        hostingViewRow("viewpill", STR_DLGLOBBY_VIEW_PILL, &policy, &secs,
                       &policyEdited, &secsEdited);
        if (policyEdited) gameFrontSetViewPillPolicy(policy);
        if (secsEdited)   gameFrontSetViewPillDecaySecs(secs);

        policy = gameFrontViewBasePolicy;
        secs   = gameFrontViewBaseDecaySecs;
        hostingViewRow("viewbase", STR_DLGLOBBY_VIEW_BASE, &policy, &secs,
                       &policyEdited, &secsEdited);
        if (policyEdited) gameFrontSetViewBasePolicy(policy);
        if (secsEdited)   gameFrontSetViewBaseDecaySecs(secs);

        policy = gameFrontViewAllyPolicy;
        secs   = gameFrontViewAllyDecaySecs;
        hostingViewRow("viewally", STR_DLGLOBBY_VIEW_ALLY, &policy, &secs,
                       &policyEdited, &secsEdited);
        if (policyEdited) gameFrontSetViewAllyPolicy(policy);
        if (secsEdited)   gameFrontSetViewAllyDecaySecs(secs);

        bool classic = gameFrontClassicMode;
        if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_CLASSIC_MODE_CB),
                            &classic)) {
            gameFrontSetClassicMode(classic);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_CLASSIC_MODE_TIP));
        }

        bool trees = gameFrontAlliesInTrees;
        if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_ALLIES_TREES_CB),
                            &trees)) {
            gameFrontSetAlliesInTrees(trees);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_ALLIES_TREES_TIP));
        }

        /* Which block of squares the map overview keeps live round the
         * player's own tank. A combo rather than a tick box because the
         * choice is a named mode, and the index is the OverviewWindow
         * value, so the entries are in enum order. */
        const char *windows[] = {
            langGetText(STR_DLGLOBBY_WINDOW_EXPANDED),
            langGetText(STR_DLGLOBBY_WINDOW_CLASSIC),
            langGetText(STR_DLGLOBBY_WINDOW_NONE),
        };
        int window = gameFrontOverviewWindow;
        ImGui::PushID("overviewwindow");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_OVERVIEW_WINDOW));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::Combo("##window", &window, windows,
                         (int)OVERVIEW_WINDOW_COUNT)) {
            gameFrontSetOverviewWindow(window);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              langGetText(STR_DLGLOBBY_OVERVIEW_WINDOW_TIP));
        }
        ImGui::PopID();

        /* What stops the player seeing inside that block. A tick box
         * because there is one rule to turn on, though the setting
         * carries a selector so another rule can join it. */
        bool sight = (gameFrontLineOfSight != (int)lineOfSightOff);
        if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_LINE_OF_SIGHT_CB),
                            &sight)) {
            gameFrontSetLineOfSight(sight ? (int)lineOfSightBuildingsAndTrees
                                          : (int)lineOfSightOff);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              langGetText(STR_DLGLOBBY_LINE_OF_SIGHT_TIP));
        }

        /* The seven controls above are the lobby's Custom row in another
         * shape, so the choice behind them is recorded the same way. Left
         * out, a set made here would be overwritten on the next start by
         * whatever preset was remembered before it.
         *
         * saveCustom is true: everything these controls hold was typed in
         * here by hand, so a set that matches no preset is a set the
         * player made and is worth keeping. */
        VisibilitySettings chosen;
        gameFrontGetVisibilitySettings(&chosen);
        gameFrontRememberVisibility(&chosen, true);
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
    /* Full screen pick from the Display tab, applied after Present alongside
       key setup: -1 = nothing pending, 0 = leave full screen, 1 = enter it. */
    signed char pendingFullScreen = -1;
#endif

    /* When the language picker reports a CJK-region change it sets
     * ctx.wantAtlasRebuild, which we translate into this flag and act on
     * after Present — rebuilding the atlas in-place so non-Latin glyphs
     * render in the same dialog session (no app restart needed). */
    bool        pendingFontRebuild = false;

    /* Set when the Skin combo changes.  gameFrontReloadSkins() destroys and
     * rebuilds the tile texture, so it runs after Present, not mid-frame. */
    bool        pendingSkinReload = false;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (dialogHandleQuitEvent(window, &ev)) {
                running = false;
            }
        }

        /* Tick the background game */
        if (hasBg && !bg->paused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bg->paused) {
            lastTickTime = SDL_GetTicks();
        }

#if defined(WINBOLO_VOICE)
        /* This dialog owns the event loop while it is up, so the voice pump
         * the in-game loop normally runs has to happen here too — otherwise
         * the loopback test is silent whenever settings are opened before a
         * game starts. NULL because this is the pre-game dialog: it has no
         * client (its own SettingsRenderCtx sets cs to null), so only the
         * local loopback path runs. */
        voiceTick(NULL);
#endif

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
        ctx.pendingFullScreen = -1;

        /* Controller tab cycling: shoulder buttons (or the Steam menu-tab
           actions where the pad is hidden from SDL) step through the visible
           tabs, skipping any that aren't present and wrapping at the ends. */
        enum { STAB_GENERAL, STAB_DISPLAY, STAB_SOUND, STAB_CONTROLS, STAB_GAMEHUD, STAB_HOSTING, STAB_LAST, STAB_COUNT };
        static int s_pgActiveTab = STAB_GENERAL;
        static int s_pgForceTab  = -1;
        bool present[STAB_COUNT];
        present[STAB_GENERAL] = true;
        present[STAB_DISPLAY] = true;
        present[STAB_SOUND]   = true;
        present[STAB_GAMEHUD] = true;
        /* Mobile can host too; a browser tab can't listen for connections. */
#if defined(__EMSCRIPTEN__)
        present[STAB_HOSTING] = false;
#else
        present[STAB_HOSTING] = true;
#endif
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
            if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_DISPLAY), nullptr,
                    s_pgForceTab == STAB_DISPLAY ? ImGuiTabItemFlags_SetSelected : 0)) {
                s_pgActiveTab = STAB_DISPLAY;
                ImGui::BeginChild("##displayPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                imguiSettingsRenderDisplayTab(&ctx);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_SOUND), nullptr,
                    s_pgForceTab == STAB_SOUND ? ImGuiTabItemFlags_SetSelected : 0)) {
                s_pgActiveTab = STAB_SOUND;
                ImGui::BeginChild("##soundPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                imguiSettingsRenderSoundTab(&ctx);
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
#if !defined(__EMSCRIPTEN__)
            if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_TAB_HOSTING), nullptr,
                    s_pgForceTab == STAB_HOSTING ? ImGuiTabItemFlags_SetSelected : 0)) {
                s_pgActiveTab = STAB_HOSTING;
                ImGui::BeginChild("##hostingPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                imguiSettingsRenderHostingTab(&ctx);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
#endif
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
        /* Same deal for a skin change: the reload happens after Present. */
        if (ctx.wantSkinReload) pendingSkinReload = true;
#if !BOLO_MOBILE
        /* The shared Controls tab requests key setup via the flag; honour it
           through the existing showKeySetup teardown below. */
        if (ctx.wantKeySetup) showKeySetup = true;
        /* The Display tab's full screen tick rides the same teardown: the
           window changes size, so the context has to be rebuilt for it. */
        if (ctx.pendingFullScreen >= 0) pendingFullScreen = ctx.pendingFullScreen;
#endif

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

        /* Rebuild the tile sheet and sound set for a new skin here, between
         * Present and the next NewFrame — the old tile texture may still be
         * referenced by draw data before Present. */
        if (pendingSkinReload) {
            pendingSkinReload = false;
            gameFrontReloadSkins();
        }

#if !BOLO_MOBILE
        /* Two things need the dialog's ImGui context torn down and rebuilt
           between Present and the next NewFrame: running key setup, which owns
           the context while it is up, and a full screen change, which lands the
           dialog on a differently sized surface.  Both rebuild from the live
           window size below, so they share one block. */
        if (showKeySetup || pendingFullScreen >= 0) {
            bool        doKeySetup   = showKeySetup;
            signed char doFullScreen = pendingFullScreen;
            showKeySetup = false;
            pendingFullScreen = -1;

            /* Tear down current ImGui context */
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();

            /* Run the key setup dialog (blocking) */
            if (doKeySetup) imguiKeySetupShow();

            /* Move the window before the size is read back; the SyncWindow
               inside makes the new size readable straight away. */
            if (doFullScreen >= 0) windowFullScreenChoose(doFullScreen != 0);

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

            /* Which tab is open lived in the context we just destroyed, so the
               tab bar would come back on the first one.  s_pgActiveTab is file
               scope and survived; force the bar back to it so the player lands
               where they left off — on Display for a full screen tick, on
               Controls coming back out of key setup. */
            s_pgForceTab = s_pgActiveTab;

            dialogSetWindowSize(window, 1024, 768);
            dialogSetWindowTitle(window, langGetText(STR_DLGSETTINGS_WINTITLE));

            lastTickTime = SDL_GetTicks();
        }
#endif
    }

#if defined(WINBOLO_VOICE)
    /* The Sound tab holds the microphone open for its level meter, and that
     * hold is otherwise given up by the voice tick this loop was running.
     * Nothing ticks voice at the menu we are handing back to, so the release
     * has to happen here or the recording device runs on with no-one draining
     * it. The in-game overlay needs no such call: the game loop keeps
     * ticking. */
    voiceSettingsSectionClosed();
#endif

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
