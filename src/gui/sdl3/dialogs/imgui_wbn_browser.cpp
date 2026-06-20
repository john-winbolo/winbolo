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
 * Name:          imgui_wbn_browser.cpp
 * Purpose:       WinBolo.net log browser dialog.
 *                Browse, search, download, and comment on
 *                archived game logs from WinBolo.net.
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

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
#include "../bg_game.h"
#include "../../gamefront.h"
#include "../../lang.h"
#include "../../../winbolonet/http.h"
#include "../../../winbolonet/wbn_comments.h"
#include "cJSON.h"
#include "imgui_wbn_browser.h"
#include "imgui_keyboard.h"
#include "imgui_winbolonet.h"
}

static const int DIALOG_W = 1024;
static const int DIALOG_H = 768;

/* ---- Data structures ---- */

struct LogComment {
    std::string username;
    int user_id;
    std::string comment;
    int rating;
    int timestamp;
    std::string time_formatted;
};

struct LogEntry {
    char key[33];
    char map[64];
    char game_type[32];
    char version[16];
    std::vector<std::string> players;
    int num_players;
    int start_time;
    int end_time;
    int game_length_seconds;
    bool log_available;
    int log_size;
    char log_size_formatted[16];
    float rating;
    int num_ratings;
    int num_downloads;

    /* Detail fields (populated on detail fetch) */
    bool detailLoaded;
    std::vector<LogComment> comments;
};

enum BrowserTab {
    TAB_RECENT = 0,
    TAB_TOP_RATED,
    TAB_MOST_DOWNLOADED,
    TAB_SEARCH,
    TAB_COUNT
};

static const langid s_tabNameIds[] = {
    STR_DLGWBN_TAB_RECENT,
    STR_DLGWBN_TAB_TOPRATED,
    STR_DLGWBN_TAB_MOSTDOWNLOADED,
    STR_DLGWBN_TAB_SEARCH,
};

/* ---- Per-tab state ---- */
struct TabState {
    std::vector<LogEntry> logs;
    int page;
    int totalPages;
    int total;
    bool fetching;
    bool fetched;
    const char *error;
};

/* ---- Star icon textures ---- */
static SDL_Texture *s_starFull  = nullptr;
static SDL_Texture *s_starHalf  = nullptr;
static SDL_Texture *s_starEmpty = nullptr;
static bool s_starsLoaded = false;

static void loadStarIcons(SDL_Renderer *renderer) {
    if (s_starsLoaded) return;
    s_starsLoaded = true;

    int iconSize = 16;
    const char *paths[][2] = {
        { "data/ui/star_full.svg",  nullptr },
        { "data/ui/star_half.svg",  nullptr },
        { "data/ui/star_empty.svg", nullptr },
    };

    char baseBuf[FILENAME_MAX];
    const char *base = SDL_GetBasePath();
    SDL_Texture **targets[] = { &s_starFull, &s_starHalf, &s_starEmpty };

    for (int i = 0; i < 3; i++) {
        *targets[i] = imguiLoadSvgIcon(renderer, paths[i][0], iconSize);
        if (!*targets[i] && base) {
            SDL_snprintf(baseBuf, sizeof(baseBuf), "%s%s", base, paths[i][0]);
            *targets[i] = imguiLoadSvgIcon(renderer, baseBuf, iconSize);
        }
    }
}

static void destroyStarIcons() {
    if (s_starFull)  { SDL_DestroyTexture(s_starFull);  s_starFull = nullptr; }
    if (s_starHalf)  { SDL_DestroyTexture(s_starHalf);  s_starHalf = nullptr; }
    if (s_starEmpty) { SDL_DestroyTexture(s_starEmpty); s_starEmpty = nullptr; }
    s_starsLoaded = false;
}

/* ---- Render star rating inline ---- */
static void renderStarRating(float rating10) {
    /* Convert 0-10 scale to 0-5 stars */
    float stars5 = rating10 / 2.0f;
    int full = (int)stars5;
    bool half = (stars5 - (float)full) >= 0.25f;
    int empty = 5 - full - (half ? 1 : 0);

    ImVec2 sz(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight());

    for (int i = 0; i < full; i++) {
        if (s_starFull) {
            ImGui::Image((ImTextureID)s_starFull, sz);
            if (i < full - 1 || half || empty > 0) ImGui::SameLine(0, 0);
        }
    }
    if (half && s_starHalf) {
        ImGui::Image((ImTextureID)s_starHalf, sz);
        if (empty > 0) ImGui::SameLine(0, 0);
    }
    for (int i = 0; i < empty; i++) {
        if (s_starEmpty) {
            ImGui::Image((ImTextureID)s_starEmpty, sz);
            if (i < empty - 1) ImGui::SameLine(0, 0);
        }
    }
}

/* ---- JSON parsing ---- */

static LogEntry parseLogEntry(cJSON *obj) {
    LogEntry e = {};

    cJSON *v;
    v = cJSON_GetObjectItem(obj, "key");
    if (v && v->valuestring) SDL_strlcpy(e.key, v->valuestring, sizeof(e.key));

    v = cJSON_GetObjectItem(obj, "map");
    if (v && v->valuestring) SDL_strlcpy(e.map, v->valuestring, sizeof(e.map));

    v = cJSON_GetObjectItem(obj, "game_type");
    if (v && v->valuestring) SDL_strlcpy(e.game_type, v->valuestring, sizeof(e.game_type));

    v = cJSON_GetObjectItem(obj, "version");
    if (v && v->valuestring) SDL_strlcpy(e.version, v->valuestring, sizeof(e.version));

    v = cJSON_GetObjectItem(obj, "players");
    if (v && cJSON_IsArray(v)) {
        int n = cJSON_GetArraySize(v);
        for (int i = 0; i < n; i++) {
            cJSON *p = cJSON_GetArrayItem(v, i);
            if (p && p->valuestring) e.players.push_back(p->valuestring);
        }
    }

    v = cJSON_GetObjectItem(obj, "num_players");
    if (v) e.num_players = v->valueint;

    v = cJSON_GetObjectItem(obj, "start_time");
    if (v) e.start_time = v->valueint;

    v = cJSON_GetObjectItem(obj, "end_time");
    if (v) e.end_time = v->valueint;

    v = cJSON_GetObjectItem(obj, "game_length_seconds");
    if (v) e.game_length_seconds = v->valueint;

    v = cJSON_GetObjectItem(obj, "log_available");
    if (v) e.log_available = cJSON_IsTrue(v);

    v = cJSON_GetObjectItem(obj, "log_size");
    if (v) e.log_size = v->valueint;

    v = cJSON_GetObjectItem(obj, "log_size_formatted");
    if (v && v->valuestring) SDL_strlcpy(e.log_size_formatted, v->valuestring, sizeof(e.log_size_formatted));

    v = cJSON_GetObjectItem(obj, "rating");
    if (v) e.rating = (float)v->valuedouble;

    v = cJSON_GetObjectItem(obj, "num_ratings");
    if (v) e.num_ratings = v->valueint;

    v = cJSON_GetObjectItem(obj, "num_downloads");
    if (v) e.num_downloads = v->valueint;

    /* Parse comments if present */
    v = cJSON_GetObjectItem(obj, "comments");
    if (v && cJSON_IsArray(v)) {
        e.detailLoaded = true;
        int n = cJSON_GetArraySize(v);
        for (int i = 0; i < n; i++) {
            cJSON *co = cJSON_GetArrayItem(v, i);
            LogComment lc;
            cJSON *cv;
            cv = cJSON_GetObjectItem(co, "username");
            if (cv && cv->valuestring) lc.username = cv->valuestring;
            cv = cJSON_GetObjectItem(co, "user_id");
            lc.user_id = cv ? cv->valueint : 0;
            cv = cJSON_GetObjectItem(co, "comment");
            if (cv && cv->valuestring) lc.comment = cv->valuestring;
            cv = cJSON_GetObjectItem(co, "rating");
            lc.rating = cv ? cv->valueint : 0;
            cv = cJSON_GetObjectItem(co, "timestamp");
            lc.timestamp = cv ? cv->valueint : 0;
            cv = cJSON_GetObjectItem(co, "time_formatted");
            if (cv && cv->valuestring) lc.time_formatted = cv->valuestring;
            e.comments.push_back(lc);
        }
    }

    return e;
}

/* ---- Format duration ---- */
static void formatDuration(int seconds, char *buf, size_t bufSize) {
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    if (h > 0)
        SDL_snprintf(buf, bufSize, "%dh %02dm", h, m);
    else
        SDL_snprintf(buf, bufSize, "%dm", m);
}

/* ---- Format unix timestamp ---- */
static void formatTimestamp(int ts, char *buf, size_t bufSize) {
    time_t t = (time_t)ts;
    struct tm *tm = localtime(&t);
    if (tm)
        strftime(buf, bufSize, "%b %d, %Y %H:%M", tm);
    else
        SDL_strlcpy(buf, langGetText(STR_UNKNOWN), bufSize);
}

/* ---- Async fetch state ---- */
struct FetchRequest {
    BrowserTab tab;
    int page;
    /* Search filters */
    char player[64];
    char mapFilter[64];
};

struct FetchResult {
    BrowserTab tab;
    int page;
    int total;
    int totalPages;
    std::vector<LogEntry> logs;
    bool success;
    char error[256];
};

/* ---- Detail fetch state ---- */
struct DetailResult {
    char key[33];
    LogEntry entry;
    bool success;
    char error[256];
};

/* ---- Download state ---- */
struct DownloadResult {
    char key[33];
    char filePath[FILENAME_MAX];
    uint8_t *memoryData;
    size_t memorySize;
    bool success;
    bool toMemory;
    char error[256];
};

extern "C" WbnBrowserResult imguiWbnBrowserShow(struct SDL_Window *window_in,
                                                struct SDL_Renderer *renderer_in) {
    WbnBrowserResult finalResult = {};
    finalResult.action = WBN_BROWSER_CLOSE;

    SDL_Window *window = window_in;
    SDL_Renderer *renderer = renderer_in;
    if (!window || !renderer) return finalResult;

    /* Save logical presentation */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

#if !BOLO_MOBILE
    dialogSetWindowSize(window, DIALOG_W, DIALOG_H);
    dialogSetWindowTitle(window, langGetText(STR_DLGWBN_WINTITLE));
    SDL_SetWindowResizable(window, true);
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    /* Set up ImGui context. CreateContext only makes itself current if no
     * context was already active — so when called from another ImGui app
     * (e.g. LogViewer) we have to switch explicitly or backends initialise
     * onto the caller's IO and trip an assertion. */
    IMGUI_CHECKVERSION();
    ImGuiContext *dlgCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(dlgCtx);
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
    dialogApplyScaling(s);

    /* Load star icons */
    loadStarIcons(renderer);

    /* Initialise HTTP for WBN API */
    bool httpOk = httpCreate();

    /* Background game */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);
    Uint64 lastTickTime = SDL_GetTicks();

    /* Tab state */
    TabState tabs[TAB_COUNT] = {};
    for (int i = 0; i < TAB_COUNT; i++) {
        tabs[i].page = 1;
        tabs[i].totalPages = 1;
    }

    BrowserTab currentTab = TAB_RECENT;
    int selectedItem = -1;

    /* Search filters */
    char searchPlayer[64] = {};
    char searchMap[64] = {};

    /* Display filters */
    int filterMinPlayers = 1;

    /* Async fetch */
    static std::mutex fetchMtx;
    static std::atomic<bool> fetching(false);
    static FetchResult fetchResult = {};
    static std::atomic<bool> fetchDone(false);

    /* Async detail fetch */
    static std::mutex detailMtx;
    static std::atomic<bool> detailFetching(false);
    static DetailResult detailResult = {};
    static std::atomic<bool> detailDone(false);

    /* Async download */
    static std::mutex downloadMtx;
    static std::atomic<bool> downloading(false);
    static DownloadResult downloadResult = {};
    static std::atomic<bool> downloadDone(false);
    /* Live byte counters published by the curl progress callback on the
     * download thread; read by the UI to draw the progress bar. */
    static std::atomic<long long> downloadBytesNow(0);
    static std::atomic<long long> downloadBytesTotal(0);
    /* Key of the log currently downloading, so the progress UI attaches to
     * that row and not to whatever the user selects next. */
    static char downloadingKey[33] = {};

    /* Per-instance worker handles. The result/flag state above is static
     * (persists for the program lifetime), so workers are joined on close:
     * a detached worker must never reach curl after httpDestroy(), nor write
     * these statics into a later dialog instance. The cancel flags let close
     * abort an in-flight transfer promptly rather than block on its timeout.
     *
     * These are plain `volatile int`, not std::atomic: they cross the C ABI
     * into http.c / libcurl's xferinfo callback (which takes `volatile int*`)
     * and are one-way "set once to abort" flags polled on the worker thread.
     * volatile is the matching idiom for that boundary; the std::atomic counters
     * above are C++-side display state and never leave this translation unit. */
    std::thread fetchThread, detailThread, downloadThread;
    volatile int fetchCancel = 0, detailCancel = 0, downloadCancel = 0;

    /* Clear any worker state left over from a previous dialog instance. */
    fetching = false;        fetchDone = false;
    detailFetching = false;  detailDone = false;
    downloading = false;     downloadDone = false;
    downloadBytesNow = 0;    downloadBytesTotal = 0;

    /* Async comment post — owned handle, freed when result is consumed */
    static WbnCommentPost *commentPost = nullptr;

    /* Comment form */
    char commentText[512] = {};
    int commentRating = 0;
    const char *commentError = nullptr;
    const char *commentSuccess = nullptr;

    /* Status */
    const char *statusText = httpOk ? langGetText(STR_DLGWBN_LOADING) : langGetText(STR_DLGWBN_NOCONNECT);

    /* Trigger initial fetch of recent logs */
    auto triggerFetch = [&](BrowserTab tab, int page) {
        if (fetching || !httpOk) return;
        if (fetchThread.joinable()) fetchThread.join(); /* reap previous (already finished) */
        fetchCancel = 0;
        tabs[tab].fetching = true;
        fetching = true;
        fetchDone = false;

        FetchRequest req = {};
        req.tab = tab;
        req.page = page;
        SDL_strlcpy(req.player, searchPlayer, sizeof(req.player));
        SDL_strlcpy(req.mapFilter, searchMap, sizeof(req.mapFilter));

        fetchThread = std::thread([req, &fetchCancel]() {
            FetchResult res = {};
            res.tab = req.tab;
            res.page = req.page;
            res.success = false;

            char path[512];
            switch (req.tab) {
            case TAB_RECENT:
                SDL_snprintf(path, sizeof(path), "logs/recent?limit=20");
                break;
            case TAB_TOP_RATED:
                SDL_snprintf(path, sizeof(path), "logs/top-rated?page=%d&limit=20", req.page);
                break;
            case TAB_MOST_DOWNLOADED:
                SDL_snprintf(path, sizeof(path), "logs/most-downloaded?page=%d&limit=20", req.page);
                break;
            case TAB_SEARCH: {
                char params[256] = {};
                int off = 0;
                off += SDL_snprintf(params + off, sizeof(params) - off, "page=%d&limit=20", req.page);
                if (req.player[0])
                    off += SDL_snprintf(params + off, sizeof(params) - off, "&player=%s", req.player);
                if (req.mapFilter[0])
                    off += SDL_snprintf(params + off, sizeof(params) - off, "&map=%s", req.mapFilter);
                SDL_snprintf(path, sizeof(path), "logs/search?%s", params);
                break;
            }
            default:
                break;
            }

            char *response = nullptr;
            int status = wbn_api_get_cancellable(path, &response, &fetchCancel);

            if (status == 200 && response) {
                cJSON *json = cJSON_Parse(response);
                if (json) {
                    cJSON *logsArr = cJSON_GetObjectItem(json, "logs");
                    if (logsArr && cJSON_IsArray(logsArr)) {
                        int n = cJSON_GetArraySize(logsArr);
                        for (int i = 0; i < n; i++) {
                            res.logs.push_back(parseLogEntry(cJSON_GetArrayItem(logsArr, i)));
                        }
                    }
                    cJSON *v;
                    v = cJSON_GetObjectItem(json, "total");
                    res.total = v ? v->valueint : (int)res.logs.size();
                    v = cJSON_GetObjectItem(json, "total_pages");
                    res.totalPages = v ? v->valueint : 1;
                    v = cJSON_GetObjectItem(json, "page");
                    if (v) res.page = v->valueint;

                    res.success = true;
                    cJSON_Delete(json);
                } else {
                    SDL_strlcpy(res.error, langGetText(STR_DLGWBN_PARSERR), sizeof(res.error));
                }
            } else if (response) {
                cJSON *json = cJSON_Parse(response);
                if (json) {
                    cJSON *err = cJSON_GetObjectItem(json, "error");
                    if (err && err->valuestring)
                        SDL_strlcpy(res.error, err->valuestring, sizeof(res.error));
                    else
                        SDL_snprintf(res.error, sizeof(res.error), "HTTP %d", status);
                    cJSON_Delete(json);
                } else {
                    SDL_snprintf(res.error, sizeof(res.error), "HTTP %d", status);
                }
            } else {
                SDL_strlcpy(res.error, langGetText(STR_DLGWBN_NETERR), sizeof(res.error));
            }

            free(response);

            {
                std::lock_guard<std::mutex> lock(fetchMtx);
                fetchResult = std::move(res);
            }
            fetchDone = true;
            fetching = false;
        });
    };

    /* Trigger detail fetch */
    auto triggerDetailFetch = [&](const char *key) {
        if (detailFetching) return;
        if (detailThread.joinable()) detailThread.join(); /* reap previous (already finished) */
        detailCancel = 0;
        detailFetching = true;
        detailDone = false;

        char keyCopy[33];
        SDL_strlcpy(keyCopy, key, sizeof(keyCopy));

        detailThread = std::thread([keyCopy, &detailCancel]() {
            DetailResult res = {};
            SDL_strlcpy(res.key, keyCopy, sizeof(res.key));
            res.success = false;

            char path[128];
            SDL_snprintf(path, sizeof(path), "logs/%s", keyCopy);

            char *response = nullptr;
            int status = wbn_api_get_cancellable(path, &response, &detailCancel);

            if (status == 200 && response) {
                cJSON *json = cJSON_Parse(response);
                if (json) {
                    res.entry = parseLogEntry(json);
                    res.entry.detailLoaded = true;
                    res.success = true;
                    cJSON_Delete(json);
                }
            } else {
                SDL_strlcpy(res.error, langGetText(STR_DLGWBN_LOADERR), sizeof(res.error));
            }

            free(response);

            {
                std::lock_guard<std::mutex> lock(detailMtx);
                detailResult = std::move(res);
            }
            detailDone = true;
            detailFetching = false;
        });
    };

    /* Curl progress sink — runs on the download thread, publishes byte
     * counts into the atomics the UI reads. Non-capturing so it converts
     * to the C WbnProgressFn function pointer. Keeps the seeded total when
     * curl hasn't reported a Content-Length yet (total == 0). */
    WbnProgressFn downloadProgressFn = [](void *user, int64_t now, int64_t total) {
        (void)user;
        downloadBytesNow = (long long)now;
        if (total > 0) downloadBytesTotal = (long long)total;
    };

    /* Trigger download */
    auto triggerDownload = [&](const char *key, long long knownSize) {
        if (downloading) return;
        if (downloadThread.joinable()) downloadThread.join(); /* reap previous (already finished) */
        downloadCancel = 0;
        SDL_strlcpy(downloadingKey, key, sizeof(downloadingKey));
        downloading = true;
        downloadDone = false;
        /* Seed from the JSON log_size so the bar is meaningful from the
         * first frame, before curl reports the real total. */
        downloadBytesNow = 0;
        downloadBytesTotal = knownSize;

        char keyCopy[33];
        SDL_strlcpy(keyCopy, key, sizeof(keyCopy));

        downloadThread = std::thread([keyCopy, downloadProgressFn, &downloadCancel]() {
            DownloadResult res = {};
            SDL_strlcpy(res.key, keyCopy, sizeof(res.key));
            res.success = false;
            res.memoryData = nullptr;
            res.memorySize = 0;

            char apiPath[128];
            SDL_snprintf(apiPath, sizeof(apiPath), "logs/%s/download", keyCopy);

#if BOLO_MOBILE
            /* Mobile: download to memory */
            res.toMemory = true;
            uint8_t *data = nullptr;
            size_t dataSize = 0;
            int status = wbn_api_download_to_memory_progress(apiPath, &data, &dataSize,
                                                             downloadProgressFn, nullptr,
                                                             &downloadCancel);
            if (status == 200 && data && dataSize > 0) {
                res.memoryData = data;
                res.memorySize = dataSize;
                res.success = true;
            } else {
                free(data);
                SDL_strlcpy(res.error, langGetText(STR_DLGWBN_DOWNLOAD_FAILED), sizeof(res.error));
            }
#else
            /* Desktop: download to file cache */
            res.toMemory = false;
            const char *prefPath = SDL_GetPrefPath("WinBolo", "WinBolo");
            if (prefPath) {
                char dirPath[FILENAME_MAX];
                SDL_snprintf(dirPath, sizeof(dirPath), "%swbn_logs", prefPath);
                SDL_CreateDirectory(dirPath);

                SDL_snprintf(res.filePath, sizeof(res.filePath), "%s/%s.wbv", dirPath, keyCopy);

                int status = wbn_api_download_progress(apiPath, res.filePath,
                                                       downloadProgressFn, nullptr,
                                                       &downloadCancel);
                if (status == 200) {
                    res.success = true;
                } else {
                    SDL_strlcpy(res.error, langGetText(STR_DLGWBN_DOWNLOAD_FAILED), sizeof(res.error));
                }
            } else {
                SDL_strlcpy(res.error, langGetText(STR_DLGWBN_NOSAVEPATH), sizeof(res.error));
            }
#endif

            {
                std::lock_guard<std::mutex> lock(downloadMtx);
                downloadResult = std::move(res);
            }
            downloadDone = true;
            downloading = false;
        });
    };

    /* Auto-fetch recent on open */
    if (httpOk) {
        triggerFetch(TAB_RECENT, 1);
    }

    /* File dialog state */
    struct { char path[FILENAME_MAX]; volatile int done; int ok; } fileDlgState = {};

    bool running = true;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            }
        }

        /* Process fetch completion */
        if (fetchDone) {
            fetchDone = false;
            std::lock_guard<std::mutex> lock(fetchMtx);
            BrowserTab tab = fetchResult.tab;
            tabs[tab].fetching = false;
            tabs[tab].fetched = true;
            if (fetchResult.success) {
                tabs[tab].logs = std::move(fetchResult.logs);
                tabs[tab].page = fetchResult.page;
                tabs[tab].total = fetchResult.total;
                tabs[tab].totalPages = fetchResult.totalPages;
                tabs[tab].error = nullptr;
                statusText = nullptr;
            } else {
                tabs[tab].error = langGetText(STR_DLGWBN_FETCHERR);
                statusText = fetchResult.error;
            }
            selectedItem = -1;
        }

        /* Process detail completion */
        if (detailDone) {
            detailDone = false;
            std::lock_guard<std::mutex> lock(detailMtx);
            if (detailResult.success) {
                /* Update the entry in the current tab's list */
                for (auto &e : tabs[currentTab].logs) {
                    if (strcmp(e.key, detailResult.key) == 0) {
                        e.comments = std::move(detailResult.entry.comments);
                        e.detailLoaded = true;
                        break;
                    }
                }
            }
        }

        /* Process download completion */
        if (downloadDone) {
            downloadDone = false;
            std::lock_guard<std::mutex> lock(downloadMtx);
            if (downloadResult.success) {
                if (downloadResult.toMemory) {
                    finalResult.action = WBN_BROWSER_PLAY_MEMORY;
                    finalResult.memoryData = downloadResult.memoryData;
                    finalResult.memorySize = downloadResult.memorySize;
                    downloadResult.memoryData = nullptr; /* transfer ownership */
                } else {
                    finalResult.action = WBN_BROWSER_PLAY_FILE;
                    SDL_strlcpy(finalResult.filePath, downloadResult.filePath, sizeof(finalResult.filePath));
                }
                running = false;
            } else {
                statusText = langGetText(STR_DLGWBN_DOWNLOAD_FAILED);
            }
        }

        /* Process file dialog completion */
        if (fileDlgState.done) {
            fileDlgState.done = 0;
            if (fileDlgState.ok) {
                finalResult.action = WBN_BROWSER_PLAY_FILE;
                SDL_strlcpy(finalResult.filePath, fileDlgState.path, sizeof(finalResult.filePath));
                running = false;
            }
        }

        /* Process comment completion */
        if (commentPost && wbn_comments_post_done(commentPost)) {
            char msg[256];
            int status = wbn_comments_post_result(commentPost, msg, sizeof(msg));
            wbn_comments_post_free(commentPost);
            commentPost = nullptr;

            if (status == 200 || status == 201) {
                commentSuccess = langGetText(STR_DLGWBN_POSTED);
                commentError = nullptr;
                commentText[0] = '\0';
                commentRating = 0;
                /* Refresh detail to show new comment */
                if (selectedItem >= 0 && selectedItem < (int)tabs[currentTab].logs.size()) {
                    tabs[currentTab].logs[selectedItem].detailLoaded = false;
                    triggerDetailFetch(tabs[currentTab].logs[selectedItem].key);
                }
            } else {
                commentError = msg[0] ? msg : "Failed to post comment";
                commentSuccess = nullptr;
            }
        }

        /* Tick background game */
        if (hasBg && !bg->paused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bg->paused) {
            lastTickTime = SDL_GetTicks();
        }

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

        /* Transparent full-screen host window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##WbnBrowserBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Main panel */
#if BOLO_MOBILE
        float panelW = (float)winW * 0.95f;
        float panelH = (float)winH * 0.95f;
#else
        const float margin = 40.0f;
        float panelW = (float)winW - margin * 2.0f;
        float panelH = (float)winH - margin * 2.0f;
        if (panelW < 700.0f) panelW = 700.0f;
        if (panelH < 500.0f) panelH = 500.0f;
#endif
        if (panelW > (float)winW * 0.98f) panelW = (float)winW * 0.98f;
        if (panelH > (float)winH * 0.98f) panelH = (float)winH * 0.98f;

        ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::SetNextWindowBgAlpha(0.90f);
        ImGui::Begin("##WbnBrowser", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar);

        /* Top-right close X — same as Close button. */
        if (WBUI::DrawPanelCloseX()) {
            running = false;
        }

        /* ---- Title ---- */
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.75f, 0.3f, 1.0f));
        ImGui::SetWindowFontScale(1.3f);
        ImGui::TextUnformatted(langGetText(STR_DLGWBN_TITLE));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::Separator();

        /* ---- Tab bar ---- */
        if (ImGui::BeginTabBar("##WbnTabs")) {
            for (int i = 0; i < TAB_COUNT; i++) {
                if (ImGui::BeginTabItem(langGetText(s_tabNameIds[i]))) {
                    if (currentTab != (BrowserTab)i) {
                        currentTab = (BrowserTab)i;
                        selectedItem = -1;
                        commentError = nullptr;
                        commentSuccess = nullptr;
                        /* Fetch if not already fetched */
                        if (!tabs[i].fetched && !tabs[i].fetching && httpOk) {
                            triggerFetch((BrowserTab)i, 1);
                        }
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        /* ---- Filter bar ---- */
        {
            ImGui::TextUnformatted(langGetText(STR_DLGBROWSER_FILTER));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80 * s);
            char minPlayersLbl[64];
            snprintf(minPlayersLbl, sizeof(minPlayersLbl), "%s##filterPlayers", langGetText(STR_DLGWBN_MINPLAYERS));
            ImGui::InputInt(minPlayersLbl, &filterMinPlayers, 1, 1);
            if (filterMinPlayers < 0) filterMinPlayers = 0;
        }

        /* ---- Search filters (Search tab only) ---- */
        if (currentTab == TAB_SEARCH) {
            ImGui::TextUnformatted(langGetText(STR_DLGWBN_SEARCHFILTERS));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120 * s);
            ImGui::InputTextWithHint("##player", langGetText(STR_DLGWBN_HINT_PLAYER), searchPlayer, sizeof(searchPlayer));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120 * s);
            ImGui::InputTextWithHint("##map", langGetText(STR_DLGWBN_HINT_MAP), searchMap, sizeof(searchMap));
            ImGui::SameLine();
            bool isSearchFetching = tabs[TAB_SEARCH].fetching;
            if (isSearchFetching) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_DLGWBN_SEARCH_BTN))) {
                tabs[TAB_SEARCH].fetched = false;
                triggerFetch(TAB_SEARCH, 1);
            }
            imguiHandOnHover();
            if (isSearchFetching) ImGui::EndDisabled();
            ImGui::Separator();
        }

        TabState &tab = tabs[currentTab];

        /* ---- Loading / Error ---- */
        if (tab.fetching) {
            ImGui::TextUnformatted(langGetText(STR_DLGWBN_LOADING));
        } else if (tab.error) {
            MessageArgs args = {};
            const char *errText = statusText ? statusText : langGetText(STR_DLGWBN_UNKNOWN_ERR);
            SDL_strlcpy(args.string1, errText, sizeof(args.string1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.3f, 0.3f, 1));
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_ERROR, &args));
            ImGui::PopStyleColor();
        }

        /* ---- Results table ---- */
        float tableH = panelH * 0.40f;
        if (selectedItem < 0) tableH = panelH * 0.70f; /* more room when no detail */

        if (!tab.logs.empty()) {
            ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                                         ImGuiTableFlags_BordersOuter;
            if (ImGui::BeginTable("##LogsTable", 6, tableFlags, ImVec2(0, tableH))) {
                ImGui::TableSetupColumn(langGetText(STR_DLGWBN_COL_MAP),     0, 3.0f);
                ImGui::TableSetupColumn(langGetText(STR_DLGWBN_COL_TYPE),    0, 1.0f);
                ImGui::TableSetupColumn(langGetText(STR_DLGWBN_COL_PLAYERS), 0, 1.0f);
                ImGui::TableSetupColumn(langGetText(STR_DLGWBN_COL_RATING),  0, 1.5f);
                ImGui::TableSetupColumn(langGetText(STR_DLGWBN_COL_SIZE),    0, 1.0f);
                ImGui::TableSetupColumn(langGetText(STR_DLGWBN_COL_DATE),    0, 2.0f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < (int)tab.logs.size(); i++) {
                    LogEntry &e = tab.logs[i];

                    /* Apply display filters */
                    if (e.num_players < filterMinPlayers) continue;

                    ImGui::TableNextRow();

                    bool isSelected = (selectedItem == i);
                    bool wasSelected = isSelected;
                    ImGui::TableNextColumn();
                    char selectId[128];
                    SDL_snprintf(selectId, sizeof(selectId), "%s##log%d", e.map, i);
                    if (ImGui::Selectable(selectId, isSelected,
                                          ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        selectedItem = i;
                        commentError = nullptr;
                        commentSuccess = nullptr;
                        /* Fetch detail if not loaded */
                        if (!e.detailLoaded) {
                            triggerDetailFetch(e.key);
                        }
                        /* Trigger View Log on:
                           - mouse double-click
                           - gamepad A / keyboard Enter on an already-
                             selected row (saves the user navigating
                             past stats + comments to find the button) */
                        bool mouseDbl     = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                        bool mouseSingle  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                        bool nonMouseActivate = !mouseSingle && !mouseDbl;
                        if (e.log_available &&
                            (mouseDbl || (nonMouseActivate && wasSelected))) {
                            triggerDownload(e.key, (long long)e.log_size);
                        }
                    }
                    imguiHandOnHover();

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e.game_type);

                    ImGui::TableNextColumn();
                    ImGui::Text("%d", e.num_players);

                    ImGui::TableNextColumn();
                    if (e.num_ratings > 0) {
                        renderStarRating(e.rating);
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%d)", e.num_ratings);
                    } else {
                        ImGui::TextDisabled("--");
                    }

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e.log_size_formatted);

                    ImGui::TableNextColumn();
                    char dateBuf[32];
                    formatTimestamp(e.start_time, dateBuf, sizeof(dateBuf));
                    ImGui::TextUnformatted(dateBuf);
                }

                ImGui::EndTable();
            }
        }

        /* ---- Detail panel ---- */
        if (selectedItem >= 0 && selectedItem < (int)tab.logs.size()) {
            LogEntry &e = tab.logs[selectedItem];

            ImGui::Separator();

            if (ImGui::BeginChild("##DetailPanel", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                /* Header info */
                ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.3f, 1.0f), "%s", e.map);
                ImGui::SameLine();
                ImGui::TextDisabled("(%s v%s)", e.game_type, e.version);

                /* Players */
                ImGui::TextUnformatted(langGetText(STR_DLGWBN_PLAYERS_LBL));
                ImGui::SameLine();
                for (size_t p = 0; p < e.players.size(); p++) {
                    if (p > 0) { ImGui::SameLine(0, 0); ImGui::TextUnformatted(", "); ImGui::SameLine(0, 0); }
                    ImGui::SameLine(0, 0);
                    ImGui::TextUnformatted(e.players[p].c_str());
                }
                if (e.players.empty()) {
                    ImGui::SameLine();
                    MessageArgs args = {};
                    args.number = e.num_players;
                    ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_NUMPLAYERS_FMT, &args));
                }

                /* Stats row */
                {
                    char durBuf[32];
                    formatDuration(e.game_length_seconds, durBuf, sizeof(durBuf));
                    MessageArgs args = {};
                    SDL_strlcpy(args.string1, durBuf, sizeof(args.string1));
                    ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_DURATION, &args));
                }
                ImGui::SameLine(0, 20);
                {
                    char ratingBuf[16];
                    SDL_snprintf(ratingBuf, sizeof(ratingBuf), "%.1f", e.rating);
                    MessageArgs args = {};
                    SDL_strlcpy(args.string1, ratingBuf, sizeof(args.string1));
                    args.number = e.num_ratings;
                    ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_RATING, &args));
                }
                ImGui::SameLine(0, 20);
                {
                    MessageArgs args = {};
                    args.number = e.num_downloads;
                    ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_DOWNLOADS_FMT, &args));
                }
                ImGui::SameLine(0, 20);
                {
                    MessageArgs args = {};
                    SDL_strlcpy(args.string1, e.log_size_formatted, sizeof(args.string1));
                    ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_SIZE, &args));
                }

                /* Comments */
                if (e.detailLoaded) {
                    ImGui::Separator();
                    if (!e.comments.empty()) {
                        MessageArgs args = {};
                        args.number = (int)e.comments.size();
                        ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_COMMENTS_FMT, &args));
                        for (auto &c : e.comments) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
                            ImGui::TextUnformatted(c.username.c_str());
                            ImGui::PopStyleColor();
                            ImGui::SameLine();
                            if (c.rating > 0) {
                                renderStarRating((float)c.rating);
                                ImGui::SameLine();
                            }

                            ImGui::TextDisabled("- %s", c.time_formatted.c_str());
                            ImGui::TextWrapped("  %s", c.comment.c_str());
                            ImGui::Spacing();
                        }
                    } else {
                        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_NOCOMMENTS));
                    }

                    /* Comment form */
                    ImGui::Separator();
                    ImGui::TextUnformatted(langGetText(STR_DLGWBN_ADDCOMMENT));

                    /* Check if signed in to WBN */
                    char wbnToken[256], wbnExpiry[256];
                    gameFrontGetWinbolonetToken(wbnToken, wbnExpiry);
                    bool wbnLoggedIn = (wbnToken[0] != '\0');

                    if (!wbnLoggedIn) {
                        imguiWinbolonetDrawSection(false);
                    } else {
                    ImGui::SetNextItemWidth(60 * s);
                    ImGui::Combo("Rating##cmtRating", &commentRating,
                                 "None\0 1\0 2\0 3\0 4\0 5\0 6\0 7\0 8\0 9\0 10\0");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(panelW * 0.5f);
                    ImGui::InputTextWithHint("##cmtText", langGetText(STR_DLGWBN_HINT_COMMENT), commentText, sizeof(commentText));
                    ImGui::SameLine();

                    bool canPost = commentText[0] != '\0' && commentPost == nullptr;
                    if (!canPost) ImGui::BeginDisabled();
                    if (ImGui::Button(langGetText(STR_DLGWBN_POST))) {
                        commentError = nullptr;
                        commentSuccess = nullptr;
                        commentPost = wbn_comments_post_start(e.key, wbnToken,
                                                              commentText, commentRating);
                    }
                    imguiHandOnHover();
                    if (!canPost) ImGui::EndDisabled();

                    if (commentError) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", commentError);
                    }
                    if (commentSuccess) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "%s", commentSuccess);
                    }
                    } /* end wbnLoggedIn else */
                } else if (detailFetching) {
                    ImGui::Separator();
                    ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_LOADINGDETAIL));
                }

                /* Download button */
                ImGui::Separator();
                if (e.log_available) {
                    /* A download is exclusive (triggerDownload no-ops while one
                       runs), so disable the button for any in-flight download,
                       but only show "Downloading"/progress on the row that is
                       actually transferring. */
                    bool anyDownloading  = downloading;
                    bool thisDownloading = anyDownloading && (strcmp(e.key, downloadingKey) == 0);
                    if (anyDownloading) ImGui::BeginDisabled();
                    if (ImGui::Button(thisDownloading ? langGetText(STR_DLGWBN_DOWNLOADING) : langGetText(STR_DLGWBN_VIEWLOG))) {
                        triggerDownload(e.key, (long long)e.log_size);
                    }
                    imguiHandOnHover();
                    if (anyDownloading) ImGui::EndDisabled();
                    if (thisDownloading) {
                        ImGui::SameLine();
                        long long now   = downloadBytesNow.load();
                        long long total = downloadBytesTotal.load();
                        if (total > 0) {
                            float progress = (float)((double)now / (double)total);
                            if (progress < 0.0f) progress = 0.0f;
                            if (progress > 1.0f) progress = 1.0f;
                            char overlay[32];
                            SDL_snprintf(overlay, sizeof(overlay), "%.0f%%", progress * 100.0f);
                            ImGui::ProgressBar(progress, ImVec2(150, 0), overlay);
                        } else {
                            /* No Content-Length yet — indeterminate animation
                               (a scrolling block, not a fill that loops). */
                            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(150, 0),
                                               langGetText(STR_DLGWBN_DOWNLOADING));
                        }
                    }
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", langGetText(STR_DLGWBN_NOLOG));
                }
            }
            ImGui::EndChild();
        }

        /* ---- Pagination ---- */
        if (tab.totalPages > 1) {
            ImGui::Separator();
            bool isFirst = (tab.page <= 1);
            bool isLast  = (tab.page >= tab.totalPages);

            if (isFirst) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_DLGWBN_PREV))) {
                triggerFetch(currentTab, tab.page - 1);
            }
            imguiHandOnHover();
            if (isFirst) ImGui::EndDisabled();

            ImGui::SameLine();
            {
                MessageArgs args = {};
                args.number = tab.page;
                args.number2 = tab.totalPages;
                args.number3 = tab.total;
                ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_PAGE, &args));
            }
            ImGui::SameLine();

            if (isLast) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_DLGWBN_NEXT))) {
                triggerFetch(currentTab, tab.page + 1);
            }
            imguiHandOnHover();
            if (isLast) ImGui::EndDisabled();
        }

        /* ---- Bottom buttons ---- */
        {
            float btnAreaY = panelH - ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().WindowPadding.y;
            float curY = ImGui::GetCursorPosY();
            if (curY < btnAreaY) {
                ImGui::SetCursorPosY(btnAreaY);
            }

#if !BOLO_MOBILE
            /* Hidden on Deck — no native file dialog reachable from a
               controller, and the WBN list covers the same need. */
            if (!uiModeIsSteamDeck()) {
                if (ImGui::Button(langGetText(STR_DLGWBN_OPENFILE))) {
                    fileDlgState.done = 0;
                    fileDlgState.ok = 0;
                    fileDlgState.path[0] = '\0';
                    SDL_DialogFileFilter filters[] = {
                        { langGetText(STR_DLGWBN_FILEFILTER), "wbv" },
                        { NULL, NULL }
                    };
                    struct FileDlgState { char *path; size_t size; volatile int *done; int *ok; };
                    auto *ctx = new FileDlgState{fileDlgState.path, sizeof(fileDlgState.path),
                                                 &fileDlgState.done, &fileDlgState.ok};
                    SDL_ShowOpenFileDialog([](void *userdata, const char * const *filelist, int) {
                        auto *s = (FileDlgState *)userdata;
                        if (filelist && filelist[0]) {
                            SDL_strlcpy(s->path, filelist[0], s->size);
                            *s->ok = 1;
                        }
                        *s->done = 1;
                        delete s;
                    }, ctx, window, filters, 1, NULL, false);
                }
                imguiHandOnHover();
                ImGui::SameLine();
            }
#endif

            /* Close is affirmative ("done viewing logs"), not a cancel —
             * leave it with default primary styling. */
            float closeW = ImGui::CalcTextSize(langGetText(STR_CLOSE)).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(panelW - closeW - ImGui::GetStyle().WindowPadding.x);
            bool closeClicked = ImGui::Button(langGetText(STR_CLOSE));
            if (closeClicked || WBUI::CancelKeyPressed()) {
                running = false;
            }
            imguiHandOnHover();
        }

        ImGui::End(); /* ##WbnBrowser */
        ImGui::End(); /* ##WbnBrowserBg */

        dialogDrawNavOutline();
        keyboardUpdate();   /* controller text entry for this dialog's fields */
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        if (hasBg) {
            bgGameRenderWithOverlay(bg, renderer, winW, winH);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        gameFrontPumpDirty(); /* sync cloud prefs from menus (login join + debounced upload) */
        dialogFrameCapEnd(frameCapStart);
    }

    /* Cleanup */
    destroyStarIcons();

    /* Stop and reap all workers before tearing down curl global state:
     * httpDestroy() -> curl_global_cleanup() is unsafe while any thread is
     * still inside a curl transfer. Signal every worker to abort first so an
     * in-flight transfer against a slow/unresponsive server doesn't block
     * close on its curl timeout (download 120s, fetch/detail 30s). */
    fetchCancel = detailCancel = downloadCancel = 1;
    if (downloadThread.joinable()) downloadThread.join();
    if (detailThread.joinable())   detailThread.join();
    if (fetchThread.joinable())    fetchThread.join();

    /* Free a memory download that finished but was never consumed by the
     * loop (closed the same frame it completed). When it is being returned
     * (PLAY_MEMORY) ownership has passed to the caller, so leave it. */
    if (downloadResult.memoryData && finalResult.action != WBN_BROWSER_PLAY_MEMORY) {
        free(downloadResult.memoryData);
        downloadResult.memoryData = nullptr;
    }

    if (httpOk) {
        httpDestroy();
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

    return finalResult;
}
