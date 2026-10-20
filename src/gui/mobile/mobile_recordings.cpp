/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          Mobile Recordings
*Filename:      mobile_recordings.cpp
*Purpose:
*  logViewerRun for the mobile clients: the recordings kept
*  on the phone, listed with name, date and size, newest
*  first, each played in the reel player
*  (logViewerRunFromMemory, mobile_log_player.cpp) when
*  tapped.
*
*  The desktop's logViewerRun opens the modal log viewer on
*  a path, or with no path on its own file dialog. iOS and
*  Android have neither, so with a path this reads the file
*  and plays it, and with none it shows this screen.
*  gamefront.c opens it when the WinBolo.net log browser's
*  Open File is pressed, and shows the browser again when it
*  closes.
*
*  Where recordings are looked for and how one leaves the
*  phone are the platform's (mobile_recordings_platform.h):
*  the preferences directory on both, where gamefront.c
*  writes singleplayer.wbv and the round recorder writes the
*  rounds of a hosted game, plus Documents and
*  Documents/Inbox on iOS and the incoming subdirectory on
*  Android.
*  The live recording, singleplayer-recording.wbv, is left
*  out: it is rewritten at every lobby entry and is not a
*  finished round.
*
*  The recorder keeps writing to the preferences directory,
*  so a recording there has an export button that takes it
*  off the phone: a dated copy in the Files app on iOS, the
*  share sheet on Android.
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
}

#include "mobile_recordings.h"
#include "mobile_recordings_platform.h"
#include "mobile_log_player.h"

/* The live single-player recording gamefront.c writes beside the finished
 * one; see the file header. */
#define MOBILE_REC_LIVE_NAME  "singleplayer-recording.wbv"
/* The last finished single-player round, shown under a name of its own. */
#define MOBILE_REC_LAST_NAME  "singleplayer.wbv"

typedef struct {
    std::string path;      /* full path, for reading and exporting */
    std::string name;      /* what the list shows */
    Uint64      size;
    SDL_Time    modified;
    bool        inPrefs;   /* in the preferences directory */
} MobileRecording;

/* Adds every .wbv in dir (which ends in a separator) to out. */
static void mobileRecordingsScanDir(const char *dir, bool inPrefs,
                                    std::vector<MobileRecording> &out) {
    int count = 0;
    char **names = SDL_GlobDirectory(dir, "*.wbv", SDL_GLOB_CASEINSENSITIVE, &count);
    if (names == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        /* The glob does not descend, but a name with a separator would be a
         * subfolder's; only this folder's own files are wanted. */
        if (strchr(names[i], '/') != NULL) continue;
        if (inPrefs && strcmp(names[i], MOBILE_REC_LIVE_NAME) == 0) continue;
        MobileRecording r;
        r.path = std::string(dir) + names[i];
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(r.path.c_str(), &info) ||
            info.type != SDL_PATHTYPE_FILE || info.size == 0) {
            continue;
        }
        r.name = (inPrefs && strcmp(names[i], MOBILE_REC_LAST_NAME) == 0)
                     ? std::string(langGetText(STR_IOSREC_LAST_ROUND))
                     : std::string(names[i]);
        r.size = info.size;
        r.modified = info.modify_time;
        r.inPrefs = inPrefs;
        out.push_back(r);
    }
    SDL_free(names);
}

/* Every recording on the phone, newest first. */
static std::vector<MobileRecording> mobileRecordingsScan(void) {
    std::vector<MobileRecording> list;
    MobileRecordingsDir dirs[4];
    int dirCount = mobileRecordingsPlatformDirs(dirs, 4);
    for (int i = 0; i < dirCount; i++) {
        mobileRecordingsScanDir(dirs[i].path, dirs[i].inPrefs, list);
    }
    std::sort(list.begin(), list.end(),
              [](const MobileRecording &a, const MobileRecording &b) {
                  return a.modified > b.modified;
              });
    return list;
}

/* The largest recording read into memory. A recording is a compressed stream
 * of ticks and a long round is a few megabytes, so 64 MB is well above any
 * real one. The file can come from another app through Files, Mail or
 * AirDrop; without a limit a file of several gigabytes would be read whole
 * into memory and the system would end the app. */
#define MOBILE_REC_MAX_BYTES  (64 * 1024 * 1024)

/* Reads a whole file into a malloc'd buffer: malloc, not SDL_malloc, because
 * the reel releases it with free() and owns it from the call on. A file that
 * is empty, of unknown size or larger than MOBILE_REC_MAX_BYTES is refused. */
static bool mobileRecordingsLoad(const char *path, uint8_t **outBuf, size_t *outLen) {
    *outBuf = NULL;
    *outLen = 0;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (io == NULL) {
        mobileRecordingsPlatformRelease(path);
        return false;
    }
    Sint64 len = SDL_GetIOSize(io);
    bool ok = false;
    /* len is negative when SDL cannot tell the size. The limit is far below
     * SIZE_MAX, so the size_t conversion below cannot wrap. */
    if (len > 0 && (Uint64)len <= (Uint64)MOBILE_REC_MAX_BYTES &&
        (Uint64)len <= (Uint64)SIZE_MAX) {
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
    /* A file opened in place from another provider was readable only for
     * this; the platform ends that access. */
    mobileRecordingsPlatformRelease(path);
    return ok;
}

/* Plays one recording in the reel player, or says it could not be read. */
static void mobileRecordingsPlay(SDL_Window *window, SDL_Renderer *renderer,
                                 const char *path) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (mobileRecordingsLoad(path, &buf, &len)) {
        logViewerRunFromMemory(window, renderer, buf, len, true);
    } else {
        imguiMessageBoxEx(langGetText(STR_IOSREC_TITLE),
                          langGetText(STR_IOSREC_OPEN_FAILED),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    }
}

/* "12 Oct 2026 14:05"-style local date, numeric so it needs no month names. */
static void mobileRecordingsFormatDate(SDL_Time t, char *buf, size_t size) {
    SDL_DateTime dt;
    if (SDL_TimeToDateTime(t, &dt, true)) {
        snprintf(buf, size, "%04d-%02d-%02d %02d:%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute);
    } else {
        SDL_strlcpy(buf, "", size);
    }
}

static void mobileRecordingsFormatSize(Uint64 bytes, char *buf, size_t size) {
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
        mobileRecordingsPlay(window, renderer, logPath);
        return;
    }

    /* A file the system handed over plays first, then the list comes up
     * behind it. */
    {
        char path[FILENAME_MAX];
        if (mobileRecordingsPlatformTakeDrop(path, sizeof(path))) {
            mobileRecordingsPlay(window, renderer, path);
        }
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

    std::vector<MobileRecording> list = mobileRecordingsScan();
    const char *notice = NULL;   /* the last export's outcome, under the title */

    bool running = true;
    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        /* What the frame asked for, done after the frame is presented: the
         * player runs a loop and an ImGui context of its own. */
        int playIndex = -1;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            /* A handed-over file is kept by the platform and played below. */
            /* Files may have been added from another app meanwhile. */
            if (ev.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
                list = mobileRecordingsScan();
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
        ImGui::Begin("##MobileRecordings", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);

        /* Top row: Close and the title. */
        if (ImGui::Button(langGetText(STR_CLOSE))) {
            running = false;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(langGetText(STR_IOSREC_TITLE));
        if (notice != NULL) {
            ImGui::TextDisabled("%s", notice);
        }
        ImGui::Separator();

        if (list.empty()) {
            ImGui::TextWrapped("%s", langGetText(STR_IOSREC_EMPTY));
        } else if (ImGui::BeginTable("##MobileRecordingsList", 4,
                                     ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##date");
            ImGui::TableSetupColumn("##size");
            ImGui::TableSetupColumn("##export");
            const float rowH = ImGui::GetFrameHeight() * 1.4f;
            for (int i = 0; i < (int)list.size(); i++) {
                const MobileRecording &r = list[(size_t)i];
                char date[32];
                char size[32];
                mobileRecordingsFormatDate(r.modified, date, sizeof(date));
                mobileRecordingsFormatSize(r.size, size, sizeof(size));
                ImGui::PushID(i);
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
                ImGui::TableSetColumnIndex(0);
                /* The whole row is the target, as large as a finger needs;
                   AllowOverlap so the export button at its end can be pressed. */
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
                if (mobileRecordingsPlatformExportShown(r.inPrefs)) {
                    if (ImGui::SmallButton(mobileRecordingsPlatformExportLabel())) {
                        bool ok = mobileRecordingsPlatformExport(r.path.c_str(),
                                                                 r.modified);
                        notice = mobileRecordingsPlatformExportNotice(ok);
                        list = mobileRecordingsScan();
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

        /* The player brings up and tears down its own ImGui context and
         * puts this one back as current when it returns. */
        char dropped[FILENAME_MAX];
        if (mobileRecordingsPlatformTakeDrop(dropped, sizeof(dropped))) {
            mobileRecordingsPlay(window, renderer, dropped);
            list = mobileRecordingsScan();
        } else if (playIndex >= 0 && playIndex < (int)list.size()) {
            std::string path = list[(size_t)playIndex].path;
            mobileRecordingsPlay(window, renderer, path.c_str());
            list = mobileRecordingsScan();
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
