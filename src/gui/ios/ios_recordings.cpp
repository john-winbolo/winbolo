/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          iOS Recordings
*Filename:      ios_recordings.cpp
*Purpose:
*  logViewerRun for the iOS client: the recordings kept on
*  the phone, listed with name, date and size, newest first,
*  each played in the reel player (logViewerRunFromMemory,
*  ios_log_player.cpp) when tapped.
*
*  The desktop's logViewerRun opens the modal log viewer on
*  a path, or with no path on its own file dialog. iOS has
*  neither, so with a path this reads the file and plays it,
*  and with none it shows this screen. gamefront.c opens it
*  for the welcome screen's Log Viewer entry.
*
*  Where recordings are looked for:
*  - the preferences directory, where gamefront.c writes
*    singleplayer.wbv and the round recorder writes the
*    rounds of a hosted game. The live recording,
*    singleplayer-recording.wbv, is left out: it is rewritten
*    at every lobby entry and is not a finished round.
*  - Documents, which the Files app and Finder file sharing
*    show as the WinBolo folder (UIFileSharingEnabled and
*    LSSupportsOpeningDocumentsInPlace in Info.plist), and
*    Documents/Inbox, where iOS puts a file another app such
*    as Mail sends over.
*
*  The recorder keeps writing to the preferences directory.
*  Moving it to Documents would put the live recording and
*  the one-round singleplayer.wbv, which the next round
*  overwrites, in the Files app; instead a recording from
*  the preferences directory has a Copy to Files button that
*  keeps a dated copy in Documents, where the player can
*  take it off the phone.
*********************************************************/

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

#include "imgui.h"
#include "../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../sdl3/dialogs/imgui_dialog_utils.h"

extern "C" {
#include "../lang.h"
#include "../sdl3/dialogs/imgui_messagebox.h"
#include "../sdl3/dialogs/imgui_wbn_browser.h"
}

#include "ios_recordings.h"

/* Defined in ios_log_player.cpp; declared by gamefront.c without a header. */
extern "C" void logViewerRunFromMemory(struct SDL_Window *window,
                                       struct SDL_Renderer *renderer,
                                       uint8_t *zipData, size_t zipLen,
                                       bool fromMainMenu);
extern "C" void logViewerRun(struct SDL_Window *window,
                             struct SDL_Renderer *renderer,
                             const char *logPath, bool fromMainMenu);

/* The live single-player recording gamefront.c writes beside the finished
 * one; see the file header. */
#define IOS_REC_LIVE_NAME  "singleplayer-recording.wbv"
/* The last finished single-player round, shown under a name of its own. */
#define IOS_REC_LAST_NAME  "singleplayer.wbv"

/* A .wbv the system handed over, waiting for the recordings screen. */
static char s_pendingPath[FILENAME_MAX];

typedef struct {
    std::string path;      /* full path, for reading and copying */
    std::string name;      /* what the list shows */
    Uint64      size;
    SDL_Time    modified;
    bool        inPrefs;   /* in the preferences directory: offer the copy */
} IosRecording;

/* True when name ends in ".wbv", in any case. */
static bool iosRecordingsIsWbv(const char *name) {
    size_t len = strlen(name);
    return len > 4 && SDL_strcasecmp(name + len - 4, ".wbv") == 0;
}

extern "C" bool iosRecordingsTakeDropEvent(const SDL_Event *ev) {
    if (ev->type != SDL_EVENT_DROP_FILE || ev->drop.data == NULL) {
        return false;
    }
    if (SDL_strncmp(ev->drop.data, "winbolo://", 10) == 0 ||
        !iosRecordingsIsWbv(ev->drop.data)) {
        return false;
    }
    SDL_strlcpy(s_pendingPath, ev->drop.data, sizeof(s_pendingPath));
    return true;
}

/* Adds every .wbv in dir (which ends in a separator) to out. */
static void iosRecordingsScanDir(const char *dir, bool inPrefs,
                                 std::vector<IosRecording> &out) {
    int count = 0;
    char **names = SDL_GlobDirectory(dir, "*.wbv", SDL_GLOB_CASEINSENSITIVE, &count);
    if (names == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        /* The glob does not descend, but a name with a separator would be a
         * subfolder's; only this folder's own files are wanted. */
        if (strchr(names[i], '/') != NULL) continue;
        if (inPrefs && strcmp(names[i], IOS_REC_LIVE_NAME) == 0) continue;
        IosRecording r;
        r.path = std::string(dir) + names[i];
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(r.path.c_str(), &info) ||
            info.type != SDL_PATHTYPE_FILE || info.size == 0) {
            continue;
        }
        r.name = (inPrefs && strcmp(names[i], IOS_REC_LAST_NAME) == 0)
                     ? std::string(langGetText(STR_IOSREC_LAST_ROUND))
                     : std::string(names[i]);
        r.size = info.size;
        r.modified = info.modify_time;
        r.inPrefs = inPrefs;
        out.push_back(r);
    }
    SDL_free(names);
}

/* The Documents directory with a trailing separator, or "" when SDL cannot
 * name it. */
static std::string iosRecordingsDocumentsDir(void) {
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (docs == NULL) return std::string();
    std::string dir(docs);
    if (!dir.empty() && dir[dir.size() - 1] != '/') dir += '/';
    return dir;
}

/* Every recording on the phone, newest first. */
static std::vector<IosRecording> iosRecordingsScan(void) {
    std::vector<IosRecording> list;
    char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir != NULL) {
        iosRecordingsScanDir(prefDir, true, list);
        SDL_free(prefDir);
    }
    std::string docs = iosRecordingsDocumentsDir();
    if (!docs.empty()) {
        iosRecordingsScanDir(docs.c_str(), false, list);
        iosRecordingsScanDir((docs + "Inbox/").c_str(), false, list);
    }
    std::sort(list.begin(), list.end(),
              [](const IosRecording &a, const IosRecording &b) {
                  return a.modified > b.modified;
              });
    return list;
}

/* Reads a whole file into a malloc'd buffer: malloc, not SDL_malloc, because
 * the reel releases it with free() and owns it from the call on. */
static bool iosRecordingsLoad(const char *path, uint8_t **outBuf, size_t *outLen) {
    *outBuf = NULL;
    *outLen = 0;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (io == NULL) return false;
    Sint64 len = SDL_GetIOSize(io);
    bool ok = false;
    if (len > 0 && (Uint64)len <= (Uint64)SIZE_MAX) {
        uint8_t *buf = (uint8_t *)malloc((size_t)len);
        if (buf != NULL) {
            if (SDL_ReadIO(io, buf, (size_t)len) == (size_t)len) {
                *outBuf = buf;
                *outLen = (size_t)len;
                ok = true;
            } else {
                free(buf);
            }
        }
    }
    SDL_CloseIO(io);
    return ok;
}

/* Plays one recording in the reel player, or says it could not be read. */
static void iosRecordingsPlay(SDL_Window *window, SDL_Renderer *renderer,
                              const char *path) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (iosRecordingsLoad(path, &buf, &len)) {
        logViewerRunFromMemory(window, renderer, buf, len, true);
    } else {
        imguiMessageBoxEx(langGetText(STR_IOSREC_TITLE),
                          langGetText(STR_IOSREC_OPEN_FAILED),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    }
}

/* Keeps a dated copy of a recording from the preferences directory in
 * Documents: "<name>-YYYYMMDD-HHMMSS.wbv", dated by the recording itself so a
 * second copy of the same round lands on the same name. */
static bool iosRecordingsCopyToFiles(const IosRecording &r) {
    std::string docs = iosRecordingsDocumentsDir();
    if (docs.empty()) return false;
    const char *base = strrchr(r.path.c_str(), '/');
    base = (base != NULL) ? base + 1 : r.path.c_str();
    std::string stem(base);
    if (stem.size() > 4) stem.resize(stem.size() - 4);   /* drop ".wbv" */
    SDL_DateTime dt;
    char stamp[32] = "";
    if (SDL_TimeToDateTime(r.modified, &dt, true)) {
        snprintf(stamp, sizeof(stamp), "-%04d%02d%02d-%02d%02d%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    }
    std::string dest = docs + stem + stamp + ".wbv";
    return SDL_CopyFile(r.path.c_str(), dest.c_str());
}

/* "12 Oct 2026 14:05"-style local date, numeric so it needs no month names. */
static void iosRecordingsFormatDate(SDL_Time t, char *buf, size_t size) {
    SDL_DateTime dt;
    if (SDL_TimeToDateTime(t, &dt, true)) {
        snprintf(buf, size, "%04d-%02d-%02d %02d:%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute);
    } else {
        SDL_strlcpy(buf, "", size);
    }
}

static void iosRecordingsFormatSize(Uint64 bytes, char *buf, size_t size) {
    if (bytes >= 1024u * 1024u) {
        snprintf(buf, size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, size, "%u KB", (unsigned)((bytes + 1023u) / 1024u));
    }
}

extern "C" void logViewerRun(struct SDL_Window *window,
                             struct SDL_Renderer *renderer,
                             const char *logPath, bool fromMainMenu) {
    (void)fromMainMenu;
    if (window == NULL || renderer == NULL) return;

    if (logPath != NULL) {
        iosRecordingsPlay(window, renderer, logPath);
        return;
    }

    /* A file the system handed over while the welcome screen was up plays
     * first, then the list comes up behind it. */
    if (s_pendingPath[0] != '\0') {
        char path[FILENAME_MAX];
        SDL_strlcpy(path, s_pendingPath, sizeof(path));
        s_pendingPath[0] = '\0';
        iosRecordingsPlay(window, renderer, path);
    }

    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW = 0, screenH = 0;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

    ImGuiContext *prevCtx = ImGui::GetCurrentContext();
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    std::vector<IosRecording> list = iosRecordingsScan();
    const char *notice = NULL;   /* the last copy's outcome, under the title */

    bool running = true;
    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        /* What the frame asked for, done after the frame is presented: the
         * player and the browser run loops and ImGui contexts of their own. */
        int playIndex = -1;
        bool openMyGames = false;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (iosRecordingsTakeDropEvent(&ev)) {
                continue;   /* played below */
            }
            /* Files may have been added through the Files app meanwhile. */
            if (ev.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
                list = iosRecordingsScan();
            }
            if (dialogHandleQuitEvent(window, &ev)) {
                running = false;
            }
        }

        int winW = 0, winH = 0;
        SDL_GetWindowSize(window, &winW, &winH);
        DialogSafeInsets insets = dialogGetSafeInsets(window);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(insets.left, insets.top));
        ImGui::SetNextWindowSize(ImVec2((float)winW - insets.left - insets.right,
                                        (float)winH - insets.top - insets.bottom));
        ImGui::Begin("##IosRecordings", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);

        /* Top row: Close, the title, and My Games for the online ones. */
        if (ImGui::Button(langGetText(STR_CLOSE))) {
            running = false;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(langGetText(STR_IOSREC_TITLE));
        {
            const char *myGames = langGetText(STR_DLGWBN_TAB_MYGAMES);
            float w = ImGui::CalcTextSize(myGames).x +
                      ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
            if (ImGui::Button(myGames)) {
                openMyGames = true;
            }
        }
        if (notice != NULL) {
            ImGui::TextDisabled("%s", notice);
        }
        ImGui::Separator();

        if (list.empty()) {
            ImGui::TextWrapped("%s", langGetText(STR_IOSREC_EMPTY));
        } else if (ImGui::BeginTable("##IosRecordingsList", 4,
                                     ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##date");
            ImGui::TableSetupColumn("##size");
            ImGui::TableSetupColumn("##copy");
            const float rowH = ImGui::GetFrameHeight() * 1.4f;
            for (int i = 0; i < (int)list.size(); i++) {
                const IosRecording &r = list[(size_t)i];
                char date[32];
                char size[32];
                iosRecordingsFormatDate(r.modified, date, sizeof(date));
                iosRecordingsFormatSize(r.size, size, sizeof(size));
                ImGui::PushID(i);
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
                ImGui::TableSetColumnIndex(0);
                /* The whole row is the target, as large as a finger needs;
                   AllowOverlap so the copy button at its end can be pressed. */
                if (ImGui::Selectable(r.name.c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(0.0f, rowH))) {
                    playIndex = i;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(date);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(size);
                ImGui::TableSetColumnIndex(3);
                if (r.inPrefs) {
                    if (ImGui::SmallButton(langGetText(STR_IOSREC_COPY))) {
                        notice = iosRecordingsCopyToFiles(r)
                                     ? langGetText(STR_IOSREC_COPIED)
                                     : langGetText(STR_IOSREC_OPEN_FAILED);
                        list = iosRecordingsScan();
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);

        if (!running) break;

        /* The player and the browser each bring up and tear down their own
         * ImGui context and put this one back as current when they return. */
        if (s_pendingPath[0] != '\0') {
            char path[FILENAME_MAX];
            SDL_strlcpy(path, s_pendingPath, sizeof(path));
            s_pendingPath[0] = '\0';
            iosRecordingsPlay(window, renderer, path);
            list = iosRecordingsScan();
        } else if (playIndex >= 0 && playIndex < (int)list.size()) {
            std::string path = list[(size_t)playIndex].path;
            iosRecordingsPlay(window, renderer, path.c_str());
            list = iosRecordingsScan();
        } else if (openMyGames) {
            WbnBrowserResult r = imguiWbnBrowserShow(window, renderer);
            ImGui::SetCurrentContext(ctx);
            if (r.action == WBN_BROWSER_PLAY_MEMORY && r.memoryData != NULL) {
                logViewerRunFromMemory(window, renderer, r.memoryData, r.memorySize, true);
            }
        }
        ImGui::SetCurrentContext(ctx);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prevCtx);

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);
    SDL_FlushEvent(SDL_EVENT_QUIT);
}
