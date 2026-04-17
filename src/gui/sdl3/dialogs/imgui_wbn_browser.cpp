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
 * Name:          imgui_wbn_browser.cpp
 * Purpose:       WinBolo.net log browser dialog.
 *                Browse, search, download, and comment on
 *                archived game logs from WinBolo.net.
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>
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
#include "nanosvg.h"
#include "nanosvgrast.h"

extern "C" {
#include "../sdl3draw.h"
#include "../bg_game.h"
#include "../../gamefront.h"
#include "../../../winbolonet/http.h"
#include "cJSON.h"
#include "imgui_wbn_browser.h"
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

static const char *s_tabNames[] = { "Recent", "Top Rated", "Most Downloaded", "Search" };

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

static SDL_Texture *loadSvgIcon(SDL_Renderer *renderer, const char *path, int size) {
    NSVGimage *svg = nsvgParseFromFile(path, "px", 96.0f);
    if (!svg) return nullptr;

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(svg); return nullptr; }

    float scale = (float)size / (svg->width > svg->height ? svg->width : svg->height);
    int w = (int)(svg->width * scale);
    int h = (int)(svg->height * scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    unsigned char *pixels = (unsigned char *)SDL_calloc(1, (size_t)(w * h * 4));
    if (!pixels) { nsvgDeleteRasterizer(rast); nsvgDelete(svg); return nullptr; }

    nsvgRasterize(rast, svg, 0, 0, scale, pixels, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, pixels, w * 4);
    if (!surface) { SDL_free(pixels); return nullptr; }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    SDL_free(pixels);
    return tex;
}

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
        *targets[i] = loadSvgIcon(renderer, paths[i][0], iconSize);
        if (!*targets[i] && base) {
            SDL_snprintf(baseBuf, sizeof(baseBuf), "%s%s", base, paths[i][0]);
            *targets[i] = loadSvgIcon(renderer, baseBuf, iconSize);
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
        strftime(buf, bufSize, "%b %d, %Y", tm);
    else
        SDL_strlcpy(buf, "Unknown", bufSize);
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

/* ---- Comment post state ---- */
struct CommentResult {
    bool success;
    char message[256];
};

extern "C" WbnBrowserResult imguiWbnBrowserShow(void) {
    WbnBrowserResult finalResult = {};
    finalResult.action = WBN_BROWSER_CLOSE;

    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
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
    dialogSetWindowTitle(window, "WinBolo - Log Browser");
    SDL_SetWindowResizable(window, true);
#endif
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    /* Set up ImGui context */
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

    /* Async comment post */
    static std::mutex commentMtx;
    static std::atomic<bool> commenting(false);
    static CommentResult commentResultData = {};
    static std::atomic<bool> commentDone(false);

    /* Comment form */
    char commentText[512] = {};
    int commentRating = 0;
    const char *commentError = nullptr;
    const char *commentSuccess = nullptr;

    /* Status */
    const char *statusText = httpOk ? "Loading..." : "Could not connect to WinBolo.net";

    /* Trigger initial fetch of recent logs */
    auto triggerFetch = [&](BrowserTab tab, int page) {
        if (fetching || !httpOk) return;
        tabs[tab].fetching = true;
        fetching = true;
        fetchDone = false;

        FetchRequest req = {};
        req.tab = tab;
        req.page = page;
        SDL_strlcpy(req.player, searchPlayer, sizeof(req.player));
        SDL_strlcpy(req.mapFilter, searchMap, sizeof(req.mapFilter));

        std::thread([req]() {
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
            int status = wbn_api_get(path, &response);

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
                    SDL_strlcpy(res.error, "Failed to parse response", sizeof(res.error));
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
                SDL_strlcpy(res.error, "Network error", sizeof(res.error));
            }

            free(response);

            {
                std::lock_guard<std::mutex> lock(fetchMtx);
                fetchResult = std::move(res);
            }
            fetchDone = true;
            fetching = false;
        }).detach();
    };

    /* Trigger detail fetch */
    auto triggerDetailFetch = [&](const char *key) {
        if (detailFetching) return;
        detailFetching = true;
        detailDone = false;

        char keyCopy[33];
        SDL_strlcpy(keyCopy, key, sizeof(keyCopy));

        std::thread([keyCopy]() {
            DetailResult res = {};
            SDL_strlcpy(res.key, keyCopy, sizeof(res.key));
            res.success = false;

            char path[128];
            SDL_snprintf(path, sizeof(path), "logs/%s", keyCopy);

            char *response = nullptr;
            int status = wbn_api_get(path, &response);

            if (status == 200 && response) {
                cJSON *json = cJSON_Parse(response);
                if (json) {
                    res.entry = parseLogEntry(json);
                    res.entry.detailLoaded = true;
                    res.success = true;
                    cJSON_Delete(json);
                }
            } else {
                SDL_strlcpy(res.error, "Failed to load details", sizeof(res.error));
            }

            free(response);

            {
                std::lock_guard<std::mutex> lock(detailMtx);
                detailResult = std::move(res);
            }
            detailDone = true;
            detailFetching = false;
        }).detach();
    };

    /* Trigger download */
    auto triggerDownload = [&](const char *key) {
        if (downloading) return;
        downloading = true;
        downloadDone = false;

        char keyCopy[33];
        SDL_strlcpy(keyCopy, key, sizeof(keyCopy));

        std::thread([keyCopy]() {
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
            int status = wbn_api_download_to_memory(apiPath, &data, &dataSize);
            if (status == 200 && data && dataSize > 0) {
                res.memoryData = data;
                res.memorySize = dataSize;
                res.success = true;
            } else {
                free(data);
                SDL_strlcpy(res.error, "Download failed", sizeof(res.error));
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

                int status = wbn_api_download(apiPath, res.filePath);
                if (status == 200) {
                    res.success = true;
                } else {
                    SDL_strlcpy(res.error, "Download failed", sizeof(res.error));
                }
            } else {
                SDL_strlcpy(res.error, "Could not determine save path", sizeof(res.error));
            }
#endif

            {
                std::lock_guard<std::mutex> lock(downloadMtx);
                downloadResult = std::move(res);
            }
            downloadDone = true;
            downloading = false;
        }).detach();
    };

    /* Auto-fetch recent on open */
    if (httpOk) {
        triggerFetch(TAB_RECENT, 1);
    }

    bool running = true;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
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
                tabs[tab].error = "Fetch failed";
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
                statusText = "Download failed";
            }
        }

        /* Process comment completion */
        if (commentDone) {
            commentDone = false;
            std::lock_guard<std::mutex> lock(commentMtx);
            if (commentResultData.success) {
                commentSuccess = "Comment posted!";
                commentError = nullptr;
                commentText[0] = '\0';
                commentRating = 0;
                /* Refresh detail to show new comment */
                if (selectedItem >= 0 && selectedItem < (int)tabs[currentTab].logs.size()) {
                    tabs[currentTab].logs[selectedItem].detailLoaded = false;
                    triggerDetailFetch(tabs[currentTab].logs[selectedItem].key);
                }
            } else {
                commentError = "Failed to post comment";
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
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        /* ---- Title ---- */
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.75f, 0.3f, 1.0f));
        ImGui::SetWindowFontScale(1.3f);
        ImGui::Text("WinBolo.net Log Browser");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::Separator();

        /* ---- Tab bar ---- */
        if (ImGui::BeginTabBar("##WbnTabs")) {
            for (int i = 0; i < TAB_COUNT; i++) {
                if (ImGui::BeginTabItem(s_tabNames[i])) {
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

        /* ---- Search filters (Search tab only) ---- */
        if (currentTab == TAB_SEARCH) {
            ImGui::Text("Search Filters:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120 * s);
            ImGui::InputTextWithHint("##player", "Player", searchPlayer, sizeof(searchPlayer));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120 * s);
            ImGui::InputTextWithHint("##map", "Map", searchMap, sizeof(searchMap));
            ImGui::SameLine();
            bool isSearchFetching = tabs[TAB_SEARCH].fetching;
            if (isSearchFetching) ImGui::BeginDisabled();
            if (ImGui::Button("Search")) {
                tabs[TAB_SEARCH].fetched = false;
                triggerFetch(TAB_SEARCH, 1);
            }
            if (isSearchFetching) ImGui::EndDisabled();
            ImGui::Separator();
        }

        TabState &tab = tabs[currentTab];

        /* ---- Loading / Error ---- */
        if (tab.fetching) {
            ImGui::Text("Loading...");
        } else if (tab.error) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Error: %s", statusText ? statusText : "Unknown error");
        }

        /* ---- Results table ---- */
        float tableH = panelH * 0.40f;
        if (selectedItem < 0) tableH = panelH * 0.70f; /* more room when no detail */

        if (!tab.logs.empty() && ImGui::BeginChild("##LogTable", ImVec2(0, tableH), ImGuiChildFlags_Borders)) {
            ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##LogsTable", 6, tableFlags)) {
                ImGui::TableSetupColumn("Map",     0, 3.0f);
                ImGui::TableSetupColumn("Type",    0, 1.0f);
                ImGui::TableSetupColumn("Players", 0, 1.0f);
                ImGui::TableSetupColumn("Rating",  0, 1.5f);
                ImGui::TableSetupColumn("Size",    0, 1.0f);
                ImGui::TableSetupColumn("Date",    0, 2.0f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < (int)tab.logs.size(); i++) {
                    LogEntry &e = tab.logs[i];
                    ImGui::TableNextRow();

                    bool isSelected = (selectedItem == i);
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(e.map, isSelected,
                                          ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        selectedItem = i;
                        commentError = nullptr;
                        commentSuccess = nullptr;
                        /* Fetch detail if not loaded */
                        if (!e.detailLoaded) {
                            triggerDetailFetch(e.key);
                        }
                        /* Double-click to download */
                        if (ImGui::IsMouseDoubleClicked(0) && e.log_available) {
                            triggerDownload(e.key);
                        }
                    }

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
        if (!tab.logs.empty()) ImGui::EndChild();

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
                ImGui::Text("Players: ");
                ImGui::SameLine();
                for (size_t p = 0; p < e.players.size(); p++) {
                    if (p > 0) { ImGui::SameLine(0, 0); ImGui::TextUnformatted(", "); ImGui::SameLine(0, 0); }
                    ImGui::SameLine(0, 0);
                    ImGui::TextUnformatted(e.players[p].c_str());
                }
                if (e.players.empty()) {
                    ImGui::SameLine();
                    ImGui::Text("%d player(s)", e.num_players);
                }

                /* Stats row */
                char durBuf[32];
                formatDuration(e.game_length_seconds, durBuf, sizeof(durBuf));
                ImGui::Text("Duration: %s", durBuf);
                ImGui::SameLine(0, 20);
                ImGui::Text("Rating: %.1f/10 (%d)", e.rating, e.num_ratings);
                ImGui::SameLine(0, 20);
                ImGui::Text("Downloads: %d", e.num_downloads);
                ImGui::SameLine(0, 20);
                ImGui::Text("Size: %s", e.log_size_formatted);

                /* Comments */
                if (e.detailLoaded) {
                    ImGui::Separator();
                    if (!e.comments.empty()) {
                        ImGui::Text("Comments (%d):", (int)e.comments.size());
                        for (auto &c : e.comments) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
                            ImGui::TextUnformatted(c.username.c_str());
                            ImGui::PopStyleColor();
                            ImGui::SameLine();
                            if (c.rating > 0) {
                                ImGui::Text("(%d/10)", c.rating);
                                ImGui::SameLine();
                            }
                            ImGui::TextDisabled("- %s", c.time_formatted.c_str());
                            ImGui::TextWrapped("  %s", c.comment.c_str());
                            ImGui::Spacing();
                        }
                    } else {
                        ImGui::TextDisabled("No comments yet.");
                    }

                    /* Comment form */
                    ImGui::Separator();
                    ImGui::Text("Add Comment:");
                    ImGui::SetNextItemWidth(60 * s);
                    ImGui::Combo("Rating##cmtRating", &commentRating,
                                 "None\0 1\0 2\0 3\0 4\0 5\0 6\0 7\0 8\0 9\0 10\0");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(panelW * 0.5f);
                    ImGui::InputTextWithHint("##cmtText", "Write a comment...", commentText, sizeof(commentText));
                    ImGui::SameLine();

                    bool canPost = commentText[0] != '\0' && !commenting;
                    if (!canPost) ImGui::BeginDisabled();
                    if (ImGui::Button("Post")) {
                        /* TODO: check for auth token. For now, post directly. */
                        commenting = true;
                        commentDone = false;
                        commentError = nullptr;
                        commentSuccess = nullptr;

                        char keyCopy[33];
                        SDL_strlcpy(keyCopy, e.key, sizeof(keyCopy));
                        char textCopy[512];
                        SDL_strlcpy(textCopy, commentText, sizeof(textCopy));
                        int ratingCopy = commentRating;

                        std::thread([keyCopy, textCopy, ratingCopy]() {
                            CommentResult res = {};

                            cJSON *body = cJSON_CreateObject();
                            /* TODO: add "token" field from auth */
                            cJSON_AddStringToObject(body, "comment", textCopy);
                            if (ratingCopy > 0)
                                cJSON_AddNumberToObject(body, "rating", ratingCopy);

                            char endpoint[128];
                            SDL_snprintf(endpoint, sizeof(endpoint), "logs/%s/comment", keyCopy);

                            char *json_str = cJSON_PrintUnformatted(body);
                            char *response = nullptr;
                            int status = wbn_api_post(endpoint, json_str, &response);

                            if (status == 200) {
                                res.success = true;
                                SDL_strlcpy(res.message, "Comment posted!", sizeof(res.message));
                            } else {
                                res.success = false;
                                if (response) {
                                    cJSON *errJson = cJSON_Parse(response);
                                    if (errJson) {
                                        cJSON *errMsg = cJSON_GetObjectItem(errJson, "error");
                                        if (errMsg && errMsg->valuestring)
                                            SDL_strlcpy(res.message, errMsg->valuestring, sizeof(res.message));
                                        else
                                            SDL_snprintf(res.message, sizeof(res.message), "HTTP %d", status);
                                        cJSON_Delete(errJson);
                                    } else {
                                        SDL_snprintf(res.message, sizeof(res.message), "HTTP %d", status);
                                    }
                                } else {
                                    SDL_strlcpy(res.message, "Network error", sizeof(res.message));
                                }
                            }

                            free(json_str);
                            free(response);
                            cJSON_Delete(body);

                            {
                                std::lock_guard<std::mutex> lock(commentMtx);
                                commentResultData = res;
                            }
                            commentDone = true;
                            commenting = false;
                        }).detach();
                    }
                    if (!canPost) ImGui::EndDisabled();

                    if (commentError) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", commentError);
                    }
                    if (commentSuccess) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "%s", commentSuccess);
                    }
                } else if (detailFetching) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Loading details...");
                }

                /* Download button */
                ImGui::Separator();
                if (e.log_available) {
                    bool isDownloading = downloading;
                    if (isDownloading) ImGui::BeginDisabled();
                    if (ImGui::Button(isDownloading ? "Downloading..." : "Download & Play")) {
                        triggerDownload(e.key);
                    }
                    if (isDownloading) ImGui::EndDisabled();
                } else {
                    ImGui::TextDisabled("Log file not available");
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
            if (ImGui::Button("< Prev")) {
                triggerFetch(currentTab, tab.page - 1);
            }
            if (isFirst) ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::Text("Page %d of %d  (%d total)", tab.page, tab.totalPages, tab.total);
            ImGui::SameLine();

            if (isLast) ImGui::BeginDisabled();
            if (ImGui::Button("Next >")) {
                triggerFetch(currentTab, tab.page + 1);
            }
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
            if (ImGui::Button("Open File...")) {
                finalResult.action = WBN_BROWSER_OPEN_LOCAL;
                running = false;
            }
            ImGui::SameLine();
#endif

            float closeW = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(panelW - closeW - ImGui::GetStyle().WindowPadding.x);
            if (ImGui::Button("Close")) {
                running = false;
            }
        }

        ImGui::End(); /* ##WbnBrowser */
        ImGui::End(); /* ##WbnBrowserBg */

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        if (hasBg) {
            bgGameRenderWithOverlay(bg, renderer, winW, winH);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    /* Cleanup */
    destroyStarIcons();

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
