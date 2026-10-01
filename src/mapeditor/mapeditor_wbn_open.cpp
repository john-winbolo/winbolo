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
 * Name:          mapeditor_wbn_open.cpp
 * Purpose:       The map editor's "Open from WinBolo.net"
 *                dialog. One map chooser instance wired to
 *                the WinBolo.net catalogue callbacks in
 *                wbn_map_source.cpp, drawn in a centered
 *                modal over the editor. A row click submits
 *                the map to the shared download worker; the
 *                per-frame drive polls the worker and, when
 *                the bytes land, feeds them to the chooser's
 *                preview pane and keeps a copy in a
 *                dialog-local stash. Only the Open button
 *                hands the stashed bytes to the editor and
 *                closes, and it is enabled only while the
 *                stash matches the current selection. Cancel,
 *                Esc and the close box discard the stash and
 *                any in-flight download, stop the preview
 *                worker, and drop the download so a late
 *                completion can't be applied on the next
 *                open. The chooser state itself persists
 *                across opens, so the user lands back in the
 *                folder they left.
 *********************************************************/

#include "mapeditor_wbn_open.h"

#if defined(MAPEDITOR_WBN_OPEN) && !defined(__EMSCRIPTEN__)

#include <cstdio>   /* FILENAME_MAX — the breadcrumb jump buffer */
#include <cstring>  /* memset / memcpy / strncmp — chooser state init, the byte copy and the "wbn:" prefix test */
#include <string>   /* std::string — display names and the stash's path stamps */
#include <utility>  /* std::move — the download result's bytes into the stash */
#include <vector>   /* std::vector — the stashed .map bytes */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../gui/sdl3/wbn_map_source.h"  /* wbnMapsListProvider / OnFolderJump / TooltipPrefix / GeneratePreview, WbnMapDownloadResult, wbnMapSourceSubmitDownload / PollDownload / ResetDownload */
#include "../gui/sdl3/dialogs/imgui_dialog_utils.h"  /* dialogComputeScale */
#include "../gui/sdl3/dialogs/dialog_footer.h"       /* WBUI::PushCancelStyle / PopCancelStyle / CancelKeyPressed */
extern "C" {
#include "../gui/sdl3/dialogs/imgui_mapchooser.h"  /* MapChooserState and the mapChooser* API, mapChooserSetSelectedMapBytes */
#include "../gui/lang.h"  /* langGetText / STR_LV_MENU_OPEN_WBN / STR_DLGLOBBY_DOWNLOADING / STR_DLGLOBBY_WBN_ERR_* / STR_OK / STR_CANCEL */
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
static std::string s_pendingPath;    /* selectedPath ("wbn:<id>") at submit time, stamps the stash */
static char s_errText[256];          /* last download failure, empty = none */

/* The last completed download, kept until Open hands it to the editor
 * or the dialog closes. s_stashPath is the "wbn:<id>" row it was
 * fetched for, so a re-click of that row doesn't re-download.
 * s_stashShownPath is whatever the chooser wrote into selectedPath
 * when the bytes were fed to its preview pane; while selectedPath
 * still equals it the stash is what the user is looking at, and Open
 * is enabled. Any other selection (another row, a folder jump) leaves
 * Open disabled until that row's download lands. */
static std::vector<uint8_t> s_stashBytes;
static std::string          s_stashName;       /* display name, .map stripped */
static std::string          s_stashPath;
static std::string          s_stashShownPath;

/* A same-row re-click found the stash already holding that map. The
 * chooser has just reset its preview to the "wbn:" spinner, so the
 * frame drive re-feeds the stash after the render pass rather than
 * touching the preview from inside the chooser's own click handler. */
static bool s_refeedPending = false;

static void meWbnClearStash(void) {
    s_stashBytes.clear();
    s_stashName.clear();
    s_stashPath.clear();
    s_stashShownPath.clear();
    s_refeedPending = false;
}

/* Push the stash into the chooser's preview pane and record the
 * selection marker the chooser wrote for it. */
static void meWbnFeedPreview(SDL_Renderer *renderer) {
    if (s_stashBytes.empty()) return;
    mapChooserSetSelectedMapBytes(&s_chooser, renderer,
                                  s_stashBytes.data(),
                                  (int)s_stashBytes.size(),
                                  s_stashName.c_str());
    s_stashShownPath = s_chooser.selectedPath;
}

/* onSelect for the WBN provider. Synthetic "wbn:<id>" paths only —
 * anything else is a folder click the chooser handled internally.
 * A row whose bytes are already stashed (or already on their way) is
 * left alone; anything else drops the stash and kicks the download
 * worker. The frame drive below polls for the result. */
static void meWbnOnSelect(MapChooserState *state, void *ctx) {
    (void)ctx;
    const char *sel = state->selectedPath;
    static const char kPrefix[] = "wbn:";
    if (!sel || strncmp(sel, kPrefix, sizeof(kPrefix) - 1) != 0) return;
    if (!s_stashBytes.empty() && s_stashPath == sel) {
        s_refeedPending = true;
        return;
    }
    if (s_downloading && s_pendingPath == sel) return;
    uint32_t mapId = (uint32_t)SDL_atoi(sel + sizeof(kPrefix) - 1);
    if (mapId == 0) return;
    meWbnClearStash();
    wbnMapSourceSubmitDownload(mapId);
    s_downloading = true;
    s_errText[0]  = '\0';
    s_pendingPath = sel;
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
 * box and Open. Dropping the download is what stops a completion that
 * lands after close from being applied on the next open; dropping the
 * stash is what stops a stale map (and a stale Open state) from
 * greeting the next open. The chooser state is kept. */
static void meWbnClose(void) {
    s_open         = false;
    s_popupPending = false;
    s_downloading  = false;
    s_pendingPath.clear();
    meWbnClearStash();
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

/* Drain the download worker. On success the bytes and the display
 * name (minus any .map extension) go into the stash, stamped with the
 * row they were fetched for, and the chooser's preview pane is fed so
 * it shows the real map instead of the spinner. On failure the error
 * text is shown on the status line and the stash stays empty. */
static void meWbnPollDownload(SDL_Renderer *renderer) {
    WbnMapDownloadResult res;
    if (!wbnMapSourcePollDownload(&res)) return;
    s_downloading = false;
    if (!res.ok) {
        SDL_strlcpy(s_errText,
                    res.err.empty() ? langGetText(STR_DLGLOBBY_WBN_ERR_MAPFAILED)
                                    : res.err.c_str(),
                    sizeof(s_errText));
        return;
    }

    std::string displayName = res.mapName;
    if (displayName.empty()) displayName = s_pendingName;
    if (displayName.empty()) displayName = "wbnmap";
    if (displayName.size() >= 4 &&
        SDL_strcasecmp(displayName.c_str() + displayName.size() - 4,
                       ".map") == 0) {
        displayName.resize(displayName.size() - 4);
    }

    s_stashBytes = std::move(res.bytes);
    s_stashName  = displayName;
    s_stashPath  = s_pendingPath;
    meWbnFeedPreview(renderer);
}

/* Open was clicked: copy the stash into an SDL_malloc buffer for the
 * editor. Returns false (with the error on the status line) only if
 * the copy can't be allocated. */
static bool meWbnTakeStash(unsigned char **outBytes, int *outLen,
                           char *outName, int outNameLen) {
    if (!outBytes || !outLen) return false;
    unsigned char *buf = (unsigned char *)SDL_malloc(s_stashBytes.size());
    if (!buf) {
        SDL_strlcpy(s_errText, langGetText(STR_DLGLOBBY_WBN_ERR_OOM),
                    sizeof(s_errText));
        return false;
    }
    memcpy(buf, s_stashBytes.data(), s_stashBytes.size());
    *outBytes = buf;
    *outLen   = (int)s_stashBytes.size();
    if (outName && outNameLen > 0) {
        SDL_strlcpy(outName, s_stashName.c_str(), (size_t)outNameLen);
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
    s_pendingPath.clear();
    s_errText[0]   = '\0';
    meWbnClearStash();
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

        if (s_refeedPending) {
            s_refeedPending = false;
            meWbnFeedPreview(renderer);
        }

        meWbnPollDownload(renderer);

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

        /* Open then Cancel, right-aligned. Open only while the stash
         * is what the preview pane is showing. Esc / Cmd+W dismiss
         * like Cancel. */
        bool canOpen = !s_downloading && !s_stashBytes.empty() &&
                       !s_stashShownPath.empty() &&
                       s_stashShownPath == s_chooser.selectedPath;
        float btnW  = 120.0f * s;
        float pairW = btnW * 2.0f + style.ItemSpacing.x;
        float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - pairW;
        if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);

        ImGui::BeginDisabled(!canOpen);
        bool openClicked = ImGui::Button(langGetText(STR_OK), ImVec2(btnW, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        WBUI::PushCancelStyle();
        bool cancelClicked = ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, 0.0f));
        WBUI::PopCancelStyle();

        if (openClicked && canOpen &&
            meWbnTakeStash(outBytes, outLen, outName, outNameLen)) {
            ready     = true;
            wantClose = true;
        }
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
