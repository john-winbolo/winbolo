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
 * Name:          mapeditor_wbn_open.cpp
 * Purpose:       The map editor's "Open from WinBolo.net"
 *                dialog. One map chooser instance wired to
 *                the WinBolo.net catalogue callbacks in
 *                wbn_map_source.cpp, drawn in a centered
 *                modal over the editor. A row click submits
 *                the map to the shared download worker; the
 *                per-frame drive polls the worker and, when
 *                the bytes land, hands them to the editor
 *                and closes. Closing for any reason stops
 *                the preview worker and drops the download
 *                so a late completion can't be applied on
 *                the next open. The chooser state itself
 *                persists across opens, so the user lands
 *                back in the folder they left.
 *********************************************************/

#include "mapeditor_wbn_open.h"

#if defined(MAPEDITOR_WBN_OPEN) && !defined(__EMSCRIPTEN__)

#include <cstdio>   /* FILENAME_MAX — the breadcrumb jump buffer */
#include <cstring>  /* memset / memcpy / strncmp — chooser state init, the byte copy and the "wbn:" prefix test */
#include <string>   /* std::string — the display name with its .map extension stripped */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../gui/sdl3/wbn_map_source.h"  /* wbnMapsListProvider / OnFolderJump / TooltipPrefix / GeneratePreview, WbnMapDownloadResult, wbnMapSourceSubmitDownload / PollDownload / ResetDownload */
#include "../gui/sdl3/dialogs/imgui_dialog_utils.h"  /* dialogComputeScale */
#include "../gui/sdl3/dialogs/dialog_footer.h"       /* WBUI::PushCancelStyle / PopCancelStyle / CancelKeyPressed */
extern "C" {
#include "../gui/sdl3/dialogs/imgui_mapchooser.h"  /* MapChooserState and the mapChooser* API */
#include "../gui/lang.h"  /* langGetText / STR_LV_MENU_OPEN_WBN / STR_DLGLOBBY_DOWNLOADING / STR_DLGLOBBY_WBN_ERR_* / STR_CANCEL */
}

/* The chooser instance. Wired on first open and kept for the life of
 * the process so folder position, view mode and split survive
 * re-opens, like the lobby's tabs. */
static MapChooserState s_chooser;
static bool            s_chooserWired = false;

/* WBN backend usable. Defaults true (the client has httpCreate'd long
 * before the editor opens); the standalone editor lowers it when its
 * own httpCreate fails, and the menu item follows. */
static bool s_available    = true;

static bool s_open         = false;  /* dialog is showing */
static bool s_popupPending = false;  /* OpenPopup still to be issued this open */
static bool s_downloading  = false;  /* a submitted map hasn't completed yet */
static char s_pendingName[128];      /* selectedName at submit time, for the status line */
static char s_errText[256];          /* last download failure, empty = none */

/* onSelect for the WBN provider. Synthetic "wbn:<id>" paths only —
 * anything else is a folder click the chooser handled internally.
 * Submitting the id kicks the download worker; the frame drive below
 * polls for the result. */
static void meWbnOnSelect(MapChooserState *state, void *ctx) {
    (void)ctx;
    const char *sel = state->selectedPath;
    static const char kPrefix[] = "wbn:";
    if (!sel || strncmp(sel, kPrefix, sizeof(kPrefix) - 1) != 0) return;
    uint32_t mapId = (uint32_t)SDL_atoi(sel + sizeof(kPrefix) - 1);
    if (mapId == 0) return;
    wbnMapSourceSubmitDownload(mapId);
    s_downloading = true;
    s_errText[0]  = '\0';
    SDL_strlcpy(s_pendingName, state->selectedName, sizeof(s_pendingName));
}

/* First-open wiring: init the chooser and plug in the catalogue
 * callbacks. No tick — polling is done by meWbnOpenFrame. */
static void meWbnWireChooser(SDL_Renderer *renderer) {
    if (s_chooserWired) return;
    memset(&s_chooser, 0, sizeof(s_chooser));
    mapChooserInit(&s_chooser, renderer);
    s_chooser.hideExtras    = true;
    s_chooser.leftPanelMaxW = 300.0f;
    s_chooser.provider.enumerate            = wbnMapsListProvider;
    s_chooser.provider.onSelect             = meWbnOnSelect;
    s_chooser.provider.onFolderJump         = wbnMapsOnFolderJump;
    s_chooser.provider.refreshTooltipPrefix = wbnMapsTooltipPrefix;
    s_chooser.provider.tick                 = NULL;
    s_chooser.provider.generatePreview      = wbnMapsGeneratePreview;
    s_chooser.provider.cacheScope           = "wbn";
    s_chooser.provider.refreshEveryFrame    = true;
    s_chooser.provider.ctx                  = NULL;
    SDL_strlcpy(s_chooser.crumbsRootLabel, "Maps",
                sizeof(s_chooser.crumbsRootLabel));
    s_chooserWired = true;
}

/* Close housekeeping, shared by the Cancel button, the window close
 * box and a successful pick. Dropping the download is what stops a
 * completion that lands after close from being applied on the next
 * open. The chooser state is kept. */
static void meWbnClose(void) {
    s_open         = false;
    s_popupPending = false;
    s_downloading  = false;
    mapChooserStopPreviewWorker();
    wbnMapSourceResetDownload();
}

/* Drive the chooser inside the modal: tooltip prefix, per-frame
 * enumerate, render, then drain a breadcrumb jump with the same
 * filter / selection resets the lobby applies. */
static void meWbnRenderChooser(SDL_Renderer *renderer, float availW,
                               float availH, float s) {
    MapChooserState *state = &s_chooser;
    if (state->provider.refreshTooltipPrefix) {
        state->provider.refreshTooltipPrefix(state, state->provider.ctx);
    }
    if (state->provider.refreshEveryFrame) {
        mapChooserRefresh(state);
    }
    mapChooserRender(state, renderer, availW, availH, s);

    char jumpPath[FILENAME_MAX];
    if (mapChooserConsumeFolderJump(state, jumpPath, sizeof(jumpPath))) {
        if (state->provider.onFolderJump) {
            state->provider.onFolderJump(state, jumpPath,
                                          state->provider.ctx);
        }
        state->searchFilter[0]     = '\0';
        state->searchRecursive     = false;
        state->selectedIdx         = -1;
        state->selectedPath[0]     = '\0';
        state->selectedName[0]     = '\0';
        state->activeCrumbsPath[0] = '\0';
        mapChooserRefresh(state);
    }
}

/* Drain the download worker. On success the bytes are copied into an
 * SDL_malloc buffer for the editor and the display name (minus any
 * .map extension) written to outName; returns true. On failure the
 * error text is shown on the status line and the dialog stays open. */
static bool meWbnPollDownload(unsigned char **outBytes, int *outLen,
                              char *outName, int outNameLen) {
    WbnMapDownloadResult res;
    if (!wbnMapSourcePollDownload(&res)) return false;
    s_downloading = false;
    if (!res.ok) {
        SDL_strlcpy(s_errText,
                    res.err.empty() ? langGetText(STR_DLGLOBBY_WBN_ERR_MAPFAILED)
                                    : res.err.c_str(),
                    sizeof(s_errText));
        return false;
    }

    unsigned char *buf = (unsigned char *)SDL_malloc(res.bytes.size());
    if (!buf) {
        SDL_strlcpy(s_errText, langGetText(STR_DLGLOBBY_WBN_ERR_OOM),
                    sizeof(s_errText));
        return false;
    }
    memcpy(buf, res.bytes.data(), res.bytes.size());

    std::string displayName = res.mapName;
    if (displayName.empty()) displayName = s_pendingName;
    if (displayName.empty()) displayName = "wbnmap";
    if (displayName.size() >= 4 &&
        SDL_strcasecmp(displayName.c_str() + displayName.size() - 4,
                       ".map") == 0) {
        displayName.resize(displayName.size() - 4);
    }

    *outBytes = buf;
    *outLen   = (int)res.bytes.size();
    if (outName && outNameLen > 0) {
        SDL_strlcpy(outName, displayName.c_str(), (size_t)outNameLen);
    }
    return true;
}

extern "C" void meWbnOpenSetAvailable(bool available) {
    s_available = available;
}

extern "C" bool meWbnOpenAvailable(void) {
    return s_available;
}

extern "C" void meWbnOpenShow(void) {
    /* The availability check also guards a stray action flag from a
     * menu path that didn't honour the disabled state. */
    if (!s_available || s_open) return;
    s_open         = true;
    s_popupPending = true;
    s_downloading  = false;
    s_errText[0]   = '\0';
}

extern "C" bool meWbnOpenIsOpen(void) {
    return s_open;
}

extern "C" bool meWbnOpenFrame(SDL_Renderer *renderer,
                               unsigned char **outBytes, int *outLen,
                               char *outName, int outNameLen) {
    if (!s_open) return false;
    if (outBytes) *outBytes = NULL;
    if (outLen)   *outLen   = 0;
    if (outName && outNameLen > 0) outName[0] = '\0';

    meWbnWireChooser(renderer);

    const char *title = langGetText(STR_LV_MENU_OPEN_WBN);
    if (s_popupPending) {
        ImGui::OpenPopup(title);
        s_popupPending = false;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImVec2 disp = io.DisplaySize;
    int screenW = (int)disp.x, screenH = (int)disp.y;
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(disp.x * 0.85f, disp.y * 0.85f),
                             ImGuiCond_Always);

    bool ready     = false;
    bool wantClose = false;
    bool keepOpen  = true;
    if (ImGui::BeginPopupModal(title, &keepOpen,
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGuiStyle &style = ImGui::GetStyle();
        /* Reserve a status line and the button row under the chooser. */
        float footerH = ImGui::GetTextLineHeightWithSpacing() +
                        ImGui::GetFrameHeightWithSpacing() +
                        style.ItemSpacing.y * 2.0f;
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y - footerH;
        if (availH < 120.0f) availH = 120.0f;

        meWbnRenderChooser(renderer, availW, availH, s);

        if (meWbnPollDownload(outBytes, outLen, outName, outNameLen)) {
            ready     = true;
            wantClose = true;
        }

        /* Status line: download progress, else the last failure, else
         * an empty line so the button row doesn't jump. */
        if (s_downloading) {
            ImGui::Text("%s  %s", langGetText(STR_DLGLOBBY_DOWNLOADING),
                        s_pendingName);
        } else if (s_errText[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", s_errText);
            ImGui::PopStyleColor();
        } else {
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
        }

        /* Cancel, right-aligned. Esc / Cmd+W dismiss too. */
        float btnW = 120.0f * s;
        float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - btnW;
        if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
        WBUI::PushCancelStyle();
        bool cancelClicked = ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, 0.0f));
        WBUI::PopCancelStyle();
        if (cancelClicked || WBUI::CancelKeyPressed()) {
            wantClose = true;
        }

        if (wantClose) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    /* keepOpen drops when the title-bar close box is used; the popup
     * can also be gone if something else tore the popup stack down. */
    if (!keepOpen || wantClose || !ImGui::IsPopupOpen(title)) {
        meWbnClose();
    }
    return ready;
}

extern "C" void meWbnOpenShutdown(void) {
    if (s_open) {
        meWbnClose();
        return;
    }
    mapChooserStopPreviewWorker();
    wbnMapSourceResetDownload();
}

#endif /* MAPEDITOR_WBN_OPEN && !__EMSCRIPTEN__ */
