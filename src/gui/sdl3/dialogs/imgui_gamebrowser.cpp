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
 * Name:          imgui_gamebrowser.cpp
 * Purpose:       ImGui Game Browser dialog.
 *                Table-based server browser with flags,
 *                async pings, and filters.
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

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
#include "../flags.h"
#include "../../gamefront.h"
#include "../../currentgames.h"
#include "../../../bolo/discovery.h"
#include "../../../bolo/global.h"
#include "../../../bolo/gametype.h"
#include "../../../bolo/netpacks.h"
#include "../../../bolo/bolo_packets.h"
#include "../../../server/geolookup.h"
#include "imgui_gamebrowser.h"
}

static const int DIALOG_W = 1024;
static const int DIALOG_H = 768;

#define STRVER_LEN 4

/* ---- Per-server enriched data ---- */
struct ServerEntry {
    /* From tracker/broadcast */
    char address[FILENAME_MAX];
    unsigned short port;
    char mapName[MAP_STR_SIZE];
    char version[FILENAME_MAX];
    BYTE numPlayers;
    BYTE numBases;
    BYTE numPills;
    bool mines;
    gameType game;
    aiType ai;
    bool password;

    /* From info ping response */
    int pingMs;          /* -1 = pending, -2 = timed out */
    WORD freePills;
    WORD freeBases;

    /* Country code for flag (from DNS lookup or mock) */
    char countryCode[3]; /* 2-char ISO + NUL */

    /* Lobby status (mocked) */
    int lobbyStatus;     /* 0=none, 1=in lobby, 2=starting */
};

static const char *gameTypeStr(gameType g) {
    switch (g) {
    case gameOpen:           return "Open";
    case gameTournament:     return "Tournament";
    case gameStrictTournament:
    default:                 return "Strict";
    }
}

static const char *aiTypeStr(aiType a) {
    switch (a) {
    case aiNone:         return "No";
    case aiYes:          return "Yes";
    case aiYesAdvantage: return "Adv";
    case aiFull:
    default:             return "Full";
    }
}

/* ---- Async ping worker ---- */
struct PingWork {
    char address[FILENAME_MAX];
    unsigned short port;
    int index;
};

struct PingResult {
    int index;
    int pingMs;       /* >=0 on success, -2 on timeout */
    WORD freePills;
    WORD freeBases;
    WORD numPlayers;
};

/* Resolve hostname to IP (if needed) and look up country via GeoIP database */
static void resolveCountryCode(ServerEntry &e) {
    e.countryCode[0] = '\0';

    /* Resolve hostname to an IP address string for the geo database */
    char ipStr[FILENAME_MAX];
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(e.address, nullptr, &hints, &res) == 0 && res) {
        if (res->ai_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)res->ai_addr;
            inet_ntop(AF_INET, &s->sin_addr, ipStr, sizeof(ipStr));
        } else if (res->ai_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)res->ai_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, ipStr, sizeof(ipStr));
        } else {
            SDL_strlcpy(ipStr, e.address, sizeof(ipStr));
        }
        freeaddrinfo(res);
    } else {
        /* Might already be an IP address string */
        SDL_strlcpy(ipStr, e.address, sizeof(ipStr));
    }

    geoLookupCountry(ipStr, e.countryCode);
}

/* Send an info request to a server and measure RTT.
 * Creates its own UDP socket so it's self-contained and thread-safe. */
static PingResult pingServer(const PingWork &work) {
    PingResult res;
    res.index = work.index;
    res.pingMs = -2;
    res.freePills = 0;
    res.freeBases = 0;
    res.numPlayers = 0;

    /* Resolve destination address */
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(work.port);
    dest.sin_addr.s_addr = inet_addr(work.address);
    if (dest.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *phe = gethostbyname(work.address);
        if (!phe) return res;
        dest.sin_addr.s_addr = *((uint32_t *)phe->h_addr_list[0]);
    }

    /* Create a temporary UDP socket */
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return res;

    /* Set receive timeout to 5 seconds */
#ifdef _WIN32
    DWORD tv = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    /* Send info request */
    BYTE buff[MAX_UDPPACKET_SIZE] = INFOREQUESTHEADER;
    Uint64 sendTime = SDL_GetTicks();
    int ret = sendto(sock, (const char *)buff, BOLOPACKET_REQUEST_SIZE, 0,
                     (struct sockaddr *)&dest, sizeof(dest));
    SDL_Log("ping: sent %d bytes to %s:%u (expected %d)",
            ret, work.address, work.port, BOLOPACKET_REQUEST_SIZE);
    if (ret != BOLOPACKET_REQUEST_SIZE) {
        SDL_Log("ping: sendto failed for %s:%u", work.address, work.port);
        closesocket(sock);
        return res;
    }

    /* Wait for response */
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int len = (int)recvfrom(sock, (char *)buff, MAX_UDPPACKET_SIZE, 0,
                            (struct sockaddr *)&from, &fromlen);
    closesocket(sock);

    if (len >= (int)sizeof(INFO_PACKET)) {
        Uint64 recvTime = SDL_GetTicks();
        res.pingMs = (int)(recvTime - sendTime);

        INFO_PACKET *info = (INFO_PACKET *)buff;
        res.freePills = info->free_pills;
        res.freeBases = info->free_bases;
        res.numPlayers = info->num_players;
        SDL_Log("ping: %s:%u responded in %dms, players=%d",
                work.address, work.port, res.pingMs, res.numPlayers);
    } else {
#ifdef _WIN32
        SDL_Log("ping: %s:%u no response (len=%d, err=%d)", work.address, work.port, len, WSAGetLastError());
#else
        SDL_Log("ping: %s:%u no response (len=%d, errno=%d)", work.address, work.port, len, errno);
#endif
    }

    return res;
}

/* ---- Callback data for async LAN broadcast search ---- */
struct BroadcastCbData {
    std::vector<ServerEntry> *servers;
    std::mutex *serversMtx;
    std::mutex *pingResultsMtx;
    std::vector<PingResult> *pingResults;
};

static ServerEntry serverEntryFromInfoPacket(INFO_PACKET *info, struct in_addr *addr) {
    ServerEntry e = {};
    e.pingMs = -1;
    e.freePills = 0;
    e.freeBases = 0;
    e.lobbyStatus = 0;

    utilPtoCString(info->mapname, e.mapName);
    e.password = (info->has_password != 0);
    e.mines = ((info->allow_mines & 0x80) != 0);

    if (info->gameid.serveraddress.s_addr == 0) {
        SDL_strlcpy(e.address, inet_ntoa(*addr), sizeof(e.address));
    } else {
        SDL_strlcpy(e.address, inet_ntoa(info->gameid.serveraddress), sizeof(e.address));
    }
    e.port = info->gameid.serverport;
    SDL_Log("serverEntryFromInfoPacket: raw serverport=%u e.port=%u", (unsigned)info->gameid.serverport, (unsigned)e.port);
    SDL_snprintf(e.version, sizeof(e.version), "%d.%d%d",
                 info->h.versionMajor, info->h.versionMinor, info->h.versionRevision);
    e.numPlayers = (BYTE)info->num_players;
    e.numBases = (BYTE)info->free_bases;
    e.numPills = (BYTE)info->free_pills;
    e.game = (gameType)info->gametype;
    e.ai = (aiType)info->allow_AI;

    resolveCountryCode(e);
    return e;
}

extern "C" void broadcastServerCallback(INFO_PACKET *info, struct in_addr *addr, void *userData) {
    BroadcastCbData *cbd = (BroadcastCbData *)userData;

    ServerEntry e = serverEntryFromInfoPacket(info, addr);

    int idx;
    {
        std::lock_guard<std::mutex> lock(*cbd->serversMtx);
        /* Deduplicate: skip if we already have this server (same address+port) */
        for (size_t i = 0; i < cbd->servers->size(); i++) {
            if ((*cbd->servers)[i].port == e.port &&
                strcmp((*cbd->servers)[i].address, e.address) == 0) {
                return;
            }
        }
        idx = (int)cbd->servers->size();
        cbd->servers->push_back(e);
    }

    /* Fire a ping for this server */
    PingWork pw = {};
    SDL_strlcpy(pw.address, e.address, FILENAME_MAX);
    pw.port = e.port;
    pw.index = idx;

    std::mutex *pMtx = cbd->pingResultsMtx;
    std::vector<PingResult> *pResults = cbd->pingResults;
    std::thread([pw, pMtx, pResults]() {
        PingResult pr = pingServer(pw);
        std::lock_guard<std::mutex> lock(*pMtx);
        pResults->push_back(pr);
    }).detach();
}

/* ---- Refresh icon (loaded from SVG) ---- */
static SDL_Texture *s_refreshIcon = nullptr;
static bool s_refreshIconAttempted = false;

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

extern "C" int imguiGameBrowserShow(const char *title, int useTracker) {
    /* Copy title — the caller passes langGetText() which returns a shared
     * static buffer that gets overwritten by any later langGetText() call
     * (e.g. from the background game's newswire/assistant messages). */
    char titleBuf[256];
    SDL_strlcpy(titleBuf, title ? title : "", sizeof(titleBuf));
    title = titleBuf;

    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return -1;

    /* Save logical presentation (Android sets one for the game view) */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    /* Get screen size and compute UI scale */
    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

#if !BOLO_MOBILE
    /* On desktop, resize window */
    dialogSetWindowSize(window, DIALOG_W, DIALOG_H);
    dialogSetWindowTitle(window, title);
    SDL_SetWindowResizable(window, true);
#endif
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    /* Set up ImGui context for this dialog */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    /* Apply font and touch scaling */
    dialogApplyScaling(s);

    /* Load flag atlas for this dialog's ImGui context */
    flagsCreate(renderer);

    /* Open GeoIP database for country flag lookups */
    if (!geoLookupIsLoaded()) {
#if BOLO_MOBILE
        /* Android assets can't be mmap'd.  Extract the mmdb file from
         * the APK asset to a real filesystem path in the cache dir. */
        {
            const char *assetPath = "data/dbip-country-lite.mmdb";
            const char *prefPath = SDL_GetPrefPath("winbolo", "winbolo");
            char cachePath[1024] = {};
            if (prefPath) {
                SDL_snprintf(cachePath, sizeof(cachePath), "%sdbip-country-lite.mmdb", prefPath);
                SDL_IOStream *src = SDL_IOFromFile(assetPath, "rb");
                if (src) {
                    Sint64 sz = SDL_GetIOSize(src);
                    if (sz > 0) {
                        void *buf = SDL_malloc((size_t)sz);
                        if (buf && SDL_ReadIO(src, buf, (size_t)sz) == (size_t)sz) {
                            SDL_IOStream *dst = SDL_IOFromFile(cachePath, "wb");
                            if (dst) {
                                SDL_WriteIO(dst, buf, (size_t)sz);
                                SDL_CloseIO(dst);
                            }
                        }
                        SDL_free(buf);
                    }
                    SDL_CloseIO(src);
                }
            }
            if (cachePath[0]) {
                geoLookupCreate(cachePath);
            }
        }
#else
        if (!geoLookupCreate("data/dbip-country-lite.mmdb")) {
            /* Try without data/ prefix */
            geoLookupCreate("dbip-country-lite.mmdb");
        }
#endif
        SDL_Log("[GameBrowser] GeoIP database loaded: %s", geoLookupIsLoaded() ? "yes" : "no");
    }

    /* Background game */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);
    Uint64 lastTickTime = SDL_GetTicks();

    /* Server list — static so background threads can add entries */
    static std::vector<ServerEntry> servers;
    static std::mutex serversMtx;
    int selectedItem = -1;
    {
        std::lock_guard<std::mutex> lock(serversMtx);
        servers.clear();
    }

    /* Search thread state — static so a detached thread can safely finish
     * writing after the dialog function returns (avoids blocking on exit). */
    static std::atomic<bool> searching(false);
    static std::atomic<bool> searchDone(false);
    static std::thread searchThread;
    static currentGames searchResultCg = nullptr;
    static char searchResultMotd[4096] = {};
    static bool searchResultOk = false;

    /* Clean up any leftover state from a previous detached search */
    if (searchDone) {
        searchDone = false;
        searching = false;
        if (searchThread.joinable()) searchThread.join();
        if (searchResultCg) { currentGamesDestroy(&searchResultCg); searchResultCg = nullptr; }
    }

    /* Ping result state — static so fire-and-forget threads can write safely */
    static std::mutex pingResultsMtx;
    static std::vector<PingResult> pingResults;
    {
        std::lock_guard<std::mutex> lock(pingResultsMtx);
        pingResults.clear();
    }

    /* Filter state */
    int filterGameType = -1; /* -1 = all */
    bool filterLocked = false;
    int filterLobby = -1;    /* -1 = all, 0 = none, 1 = in lobby, 2 = starting */

    /* Status */
    bool loadingGames = false;
    const char *statusText = useTracker ? "Click Refresh to load games..." : "Click Refresh to scan...";

    /* Error popup */
    const char *errorMsg = nullptr;

    /* Set Name popup */
    char nameEditBuf[PLAYER_NAME_LEN] = {};

    /* Tracker setup popup (TODO: tracker UI not yet implemented) */
    (void)0;

    int result = -1;
    bool running = true;

    /* Auto-refresh on open */
    bool autoRefresh = true;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            if (ev.type == SDL_EVENT_QUIT) {
                gameFrontSetDlgState(openWelcome);
                running = false;
            }
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(window)) {
                gameFrontSetDlgState(openWelcome);
                running = false;
            }
        }

        /* Check if background search completed */
        if (searchDone) {
            searchDone = false;
            if (searchThread.joinable()) {
                searchThread.join();
            }
            searching = false;

            if (useTracker) {
                /* Tracker mode: convert currentGames linked list to our vector */
                std::vector<ServerEntry> newServers;
                if (searchResultOk && searchResultCg) {
                    int total = currentGamesItemCount(&searchResultCg);
                    for (int i = 0; i < total; i++) {
                        ServerEntry e = {};
                        e.pingMs = -1; /* pending */
                        e.freePills = 0;
                        e.freeBases = 0;
                        e.lobbyStatus = (i % 5 == 0) ? 1 : (i % 7 == 0) ? 2 : 0; /* mock */

                        currentGamesGetItem(&searchResultCg, i + 1,
                            e.address, &e.port, e.mapName, e.version,
                            &e.numPlayers, &e.numBases, &e.numPills,
                            &e.mines, &e.game, &e.ai, &e.password);

                        resolveCountryCode(e);
                        newServers.push_back(e);
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(serversMtx);
                    servers = std::move(newServers);
                    selectedItem = -1;
                }

                if (searchResultCg) {
                    currentGamesDestroy(&searchResultCg);
                    searchResultCg = nullptr;
                }

                if (searchResultOk) {
                    int total = (int)servers.size();
                    if (total > 0) {
                        statusText = "Games loaded.";
                        loadingGames = false;

                        /* Fire-and-forget async pings to each server */
                        for (int i = 0; i < total; i++) {
                            PingWork pw = {};
                            SDL_strlcpy(pw.address, servers[i].address, FILENAME_MAX);
                            pw.port = servers[i].port;
                            pw.index = i;

                            std::thread([pw]() {
                                PingResult pr = pingServer(pw);
                                std::lock_guard<std::mutex> lock(pingResultsMtx);
                                pingResults.push_back(pr);
                            }).detach();
                        }
                    } else {
                        statusText = "No games found. Start one!";
                        loadingGames = false;
                    }
                } else {
                    statusText = "Search failed.";
                    loadingGames = false;
                }
            } else {
                /* LAN mode: servers were added incrementally by the callback,
                 * pings already fired per-server. Just update status. */
                if (searchResultCg) {
                    currentGamesDestroy(&searchResultCg);
                    searchResultCg = nullptr;
                }

                int total;
                {
                    std::lock_guard<std::mutex> lock(serversMtx);
                    total = (int)servers.size();
                    selectedItem = -1;
                }

                if (searchResultOk && total > 0) {
                    statusText = "Games loaded.";
                } else if (searchResultOk) {
                    statusText = "No games found. Start one!";
                } else {
                    statusText = "Search failed.";
                }
                loadingGames = false;
            }
        }

        /* Process incoming ping results */
        {
            std::lock_guard<std::mutex> lock(pingResultsMtx);
            for (auto &pr : pingResults) {
                std::lock_guard<std::mutex> slock(serversMtx);
                if (pr.index >= 0 && pr.index < (int)servers.size()) {
                    servers[pr.index].pingMs = pr.pingMs;
                    servers[pr.index].freePills = pr.freePills;
                    servers[pr.index].freeBases = pr.freeBases;
                    if (pr.numPlayers > 0) {
                        servers[pr.index].numPlayers = (BYTE)pr.numPlayers;
                    }
                }
            }
            pingResults.clear();
        }

        /* Tick the background game at fixed rate (unless paused) */
        if (hasBg && !bg->paused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bg->paused) {
            lastTickTime = SDL_GetTicks();
        }

        /* Query actual window size each frame */
        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();

        /* Transparent full-screen host window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##GameBrowserBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Main panel — fill most of the screen */
#if BOLO_MOBILE
        float panelW = (float)winW * 0.90f;
        float panelH = (float)winH * 0.90f;
#else
        const float margin = 40.0f;
        float panelW = (float)winW - margin * 2.0f;
        float panelH = (float)winH - margin * 2.0f;
        if (panelW < 700.0f) panelW = 700.0f;
        if (panelH < 500.0f) panelH = 500.0f;
#endif
        if (panelW > (float)winW * 0.95f) panelW = (float)winW * 0.95f;
        if (panelH > (float)winH * 0.95f) panelH = (float)winH * 0.95f;

        ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##GameBrowser", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar);

        /* ---- Title bar ---- */
        {
            /* Load refresh icon on first use */
            if (!s_refreshIcon && !s_refreshIconAttempted) {
                s_refreshIconAttempted = true;
                int iconSize = (int)(24.0f * s);
                if (iconSize < 16) iconSize = 16;

                /* Try several path candidates */
                const char *candidates[] = {
                    "data/ui/refresh.svg",
                    NULL /* filled in below with basePath variant */
                };
                char basePathBuf[FILENAME_MAX] = {};
                const char *base = SDL_GetBasePath();
                if (base) {
                    SDL_snprintf(basePathBuf, sizeof(basePathBuf), "%sdata/ui/refresh.svg", base);
                    candidates[1] = basePathBuf;
                }
                for (int i = 0; i < 2 && !s_refreshIcon; i++) {
                    if (candidates[i]) {
                        SDL_Log("[GameBrowser] Trying refresh icon: %s", candidates[i]);
                        s_refreshIcon = loadSvgIcon(renderer, candidates[i], iconSize);
                    }
                }
                SDL_Log("[GameBrowser] Refresh icon loaded: %s", s_refreshIcon ? "yes" : "no");
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.75f, 0.3f, 1.0f));
            ImGui::SetWindowFontScale(1.3f);
            ImGui::Text("%s", title);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();

            /* Refresh button on the right side of the title bar */
            bool doRefresh = autoRefresh;
            if (s_refreshIcon) {
                float iconH = ImGui::GetTextLineHeight() * 1.3f;
                ImVec2 iconSz(iconH, iconH);
                float pad = ImGui::GetStyle().FramePadding.x * 2.0f;
                ImGui::SameLine(panelW - iconH - pad - 16.0f * s);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconH * 0.15f);
                bool wasSearching = searching;
                if (wasSearching) ImGui::BeginDisabled();
                if (ImGui::ImageButton("##refreshBtn", (ImTextureID)s_refreshIcon, iconSz)) {
                    doRefresh = true;
                }
                if (wasSearching) ImGui::EndDisabled();
            } else {
                /* Text fallback when SVG icon is unavailable */
                float btnW = 70.0f * s;
                ImGui::SameLine(panelW - btnW - 16.0f * s);
                bool wasSearching = searching;
                if (wasSearching) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Refresh")) {
                    doRefresh = true;
                }
                if (wasSearching) ImGui::EndDisabled();
            }
            if (doRefresh) {
                autoRefresh = false;
                statusText = "Searching...";
                loadingGames = true;
                selectedItem = -1;

                {
                    std::lock_guard<std::mutex> lock(serversMtx);
                    servers.clear();
                }

                bool ut = (useTracker != 0);
                char tAddr[FILENAME_MAX] = {};
                unsigned short tPort = 0;
                if (ut) {
                    bool dummy;
                    gameFrontGetTrackerOptions(tAddr, &tPort, &dummy);
                }

                if (searchThread.joinable()) {
                    searchThread.join();
                }

                searchResultCg = currentGamesCreate();
                searchResultMotd[0] = '\0';
                searchResultOk = false;
                searching = true;

                struct SearchParams { char addr[FILENAME_MAX]; unsigned short port; bool tracker; };
                SearchParams sp = {};
                strncpy(sp.addr, tAddr, FILENAME_MAX - 1);
                sp.port = tPort;
                sp.tracker = ut;

                searchThread = std::thread([sp]() {
                    bool ret = false;
                    if (sp.tracker) {
                        char addr[FILENAME_MAX];
                        memcpy(addr, sp.addr, FILENAME_MAX);
                        ret = discoveryFindTrackedGames(&searchResultCg, addr, sp.port, searchResultMotd);
                    } else {
                        static BroadcastCbData cbd;
                        cbd.servers = &servers;
                        cbd.serversMtx = &serversMtx;
                        cbd.pingResultsMtx = &pingResultsMtx;
                        cbd.pingResults = &pingResults;
                        ret = discoveryFindBroadcastGamesAsync(broadcastServerCallback, &cbd);
                    }
                    searchResultOk = ret;
                    searchDone = true;
                });
            }

            ImGui::Separator();
            ImGui::Spacing();
        }

        /* ---- Loading banner ---- */
        if (loadingGames) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.5f, 1.0f));
            float time = (float)SDL_GetTicks() / 1000.0f;
            const char *dots[] = { "", ".", "..", "..." };
            int dotIdx = ((int)(time * 2.0f)) % 4;
            ImGui::Text("Loading games list%s", dots[dotIdx]);
            ImGui::PopStyleColor();
        }

        /* ---- Server table ---- */
        /* Reserve space for: filters row + separator + status + button row */
        float frameH = ImGui::GetFrameHeightWithSpacing();
        float spacingY = ImGui::GetStyle().ItemSpacing.y;
        float bottomH = frameH       /* filters row */
                       + spacingY     /* Spacing */
                       + 2.0f         /* Separator */
                       + spacingY     /* Spacing */
                       + frameH       /* status text */
                       + spacingY     /* Spacing */
                       + frameH       /* button row */
                       + spacingY;    /* bottom padding */
        float tableH = ImGui::GetContentRegionAvail().y - bottomH;
        if (tableH < 100.0f) tableH = 100.0f;

        ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable |
            ImGuiTableFlags_Hideable;

        if (ImGui::BeginTable("##ServerTable", 10, tableFlags, ImVec2(0, tableH))) {
            /* Column setup */
            ImGui::TableSetupScrollFreeze(0, 1); /* freeze header row */
            ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 24.0f);  /* flag */
            ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 20.0f);  /* lock */
            ImGui::TableSetupColumn("Server",    ImGuiTableColumnFlags_WidthStretch, 0.0f);
            ImGui::TableSetupColumn("Map",       ImGuiTableColumnFlags_WidthStretch, 0.0f);
            ImGui::TableSetupColumn("Players",   ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Type",      ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("AI",        ImGuiTableColumnFlags_WidthFixed, 45.0f);
            ImGui::TableSetupColumn("Bases",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Pills",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Ping",      ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 50.0f);
            ImGui::TableHeadersRow();

            /* Sort */
            if (ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs()) {
                if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                    int col = sortSpecs->Specs[0].ColumnIndex;
                    bool asc = (sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                    std::lock_guard<std::mutex> lock(serversMtx);
                    std::sort(servers.begin(), servers.end(), [col, asc](const ServerEntry &a, const ServerEntry &b) {
                        int cmp = 0;
                        switch (col) {
                        case 2: cmp = strcmp(a.address, b.address); break;
                        case 3: cmp = strcmp(a.mapName, b.mapName); break;
                        case 4: cmp = (int)a.numPlayers - (int)b.numPlayers; break;
                        case 5: cmp = (int)a.game - (int)b.game; break;
                        case 6: cmp = (int)a.ai - (int)b.ai; break;
                        case 7: cmp = (int)a.freeBases - (int)b.freeBases; break;
                        case 8: cmp = (int)a.freePills - (int)b.freePills; break;
                        case 9: {
                            int pa = (a.pingMs >= 0) ? a.pingMs : 99999;
                            int pb = (b.pingMs >= 0) ? b.pingMs : 99999;
                            cmp = pa - pb;
                            break;
                        }
                        default: break;
                        }
                        return asc ? (cmp < 0) : (cmp > 0);
                    });
                    sortSpecs->SpecsDirty = false;
                }
            }

            /* Rows */
            {
                std::lock_guard<std::mutex> lock(serversMtx);
                for (int i = 0; i < (int)servers.size(); i++) {
                    const ServerEntry &e = servers[i];

                    /* Apply filters */
                    if (filterGameType >= 0 && (int)e.game != filterGameType) continue;
                    if (filterLocked && e.password) continue;
                    if (filterLobby >= 0 && e.lobbyStatus != filterLobby) continue;

                    ImGui::TableNextRow();

                    /* Flag */
                    ImGui::TableNextColumn();
                    if (e.countryCode[0] != '\0' &&
                        e.countryCode[0] != 'X') {
                        SDL_Texture *flagTex = flagsGetTexture(e.countryCode);
                        if (flagTex) {
                            ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                        } else {
                            ImGui::TextDisabled("%c%c", e.countryCode[0], e.countryCode[1]);
                        }
                    } else if (e.countryCode[0] != '\0') {
                        ImGui::TextDisabled("%c%c", e.countryCode[0], e.countryCode[1]);
                    }

                    /* Lock icon */
                    ImGui::TableNextColumn();
                    if (e.password) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "L");
                    }

                    /* Server address - selectable across the row */
                    ImGui::TableNextColumn();
                    {
                        char label[256];
                        SDL_snprintf(label, sizeof(label), "%s:%u", e.address, e.port);
                        bool isSelected = (selectedItem == i);
                        if (ImGui::Selectable(label, isSelected,
                                              ImGuiSelectableFlags_SpanAllColumns |
                                              ImGuiSelectableFlags_AllowDoubleClick)) {
                            selectedItem = i;
                            if (ImGui::IsMouseDoubleClicked(0)) {
                                /* Double-click to join */
                                if (strlen(e.version) >= STRVER_LEN &&
                                    strncmp(e.version, STRVER, STRVER_LEN) == 0) {
                                    char playerName[PLAYER_NAME_LEN];
                                    gameFrontGetPlayerName(playerName);
                                    if (strlen(playerName) > 0) {
                                        gameFrontSetUdpOptions(playerName, (char *)e.address, e.port, 0);
                                        gameFrontSetAIType(e.ai);
                                        gameFrontSetDlgState(openUdpJoin);
                                        result = (int)openUdpJoin;
                                        running = false;
                                    }
                                }
                            }
                        }
                    }

                    /* Map name (C string from tracker/broadcast) */
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", e.mapName);

                    /* Players */
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", e.numPlayers);

                    /* Game type */
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", gameTypeStr(e.game));

                    /* AI */
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", aiTypeStr(e.ai));

                    /* Bases (free/total) */
                    ImGui::TableNextColumn();
                    if (e.pingMs >= 0) {
                        ImGui::Text("%u/%u", e.freeBases, e.numBases);
                    } else {
                        ImGui::Text("%u", e.numBases);
                    }

                    /* Pills (free/total) */
                    ImGui::TableNextColumn();
                    if (e.pingMs >= 0) {
                        ImGui::Text("%u/%u", e.freePills, e.numPills);
                    } else {
                        ImGui::Text("%u", e.numPills);
                    }

                    /* Ping */
                    ImGui::TableNextColumn();
                    if (e.pingMs >= 0) {
                        ImVec4 col;
                        if (e.pingMs < 50) col = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
                        else if (e.pingMs < 150) col = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
                        else col = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                        ImGui::TextColored(col, "%dms", e.pingMs);
                    } else if (e.pingMs == -1) {
                        ImGui::TextDisabled("...");
                    } else {
                        ImGui::TextDisabled("--");
                    }
                }
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();

        /* ---- Filters row ---- */
        {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Filter:");
            ImGui::SameLine();

            ImGui::SetNextItemWidth(130.0f * s);
            const char *gameTypes[] = { "All Types", "Open", "Tournament", "Strict" };
            int gtIdx = (filterGameType < 0) ? 0 : filterGameType;
            if (ImGui::Combo("##filterType", &gtIdx, gameTypes, 4)) {
                filterGameType = (gtIdx == 0) ? -1 : gtIdx;
            }

            ImGui::SameLine();
            ImGui::Checkbox("Unlocked Only", &filterLocked);

            ImGui::SameLine();
            ImGui::SetNextItemWidth(130.0f * s);
            const char *lobbyOpts[] = { "All Lobby", "None", "In Lobby", "Starting" };
            int lobbyIdx = (filterLobby < 0) ? 0 : filterLobby + 1;
            if (ImGui::Combo("##filterLobby", &lobbyIdx, lobbyOpts, 4)) {
                filterLobby = (lobbyIdx == 0) ? -1 : lobbyIdx - 1;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* ---- Status bar ---- */
        {
            int total = (int)servers.size();
            int pendingPings = 0;
            {
                std::lock_guard<std::mutex> lock(serversMtx);
                for (auto &s : servers) {
                    if (s.pingMs == -1) pendingPings++;
                }
            }
            if (pendingPings > 0) {
                ImGui::Text("Status: %s  |  %d servers  |  Pinging %d...", statusText, total, pendingPings);
            } else {
                ImGui::Text("Status: %s  |  %d servers", statusText, total);
            }
        }

        ImGui::Spacing();

        /* ---- Button row ---- */
        float btnW = 110.0f * s;
        float btnH = 28.0f * s;
        {
            bool hasSelection = (selectedItem >= 0 && selectedItem < (int)servers.size());

            /* Join */
            if (!hasSelection) ImGui::BeginDisabled();
            if (ImGui::Button("Join", ImVec2(btnW, btnH))) {
                const ServerEntry &e = servers[selectedItem];
                if (strlen(e.version) < STRVER_LEN ||
                    strncmp(e.version, STRVER, STRVER_LEN) != 0) {
                    errorMsg = "Server is running a different version of WinBolo.";
                    ImGui::OpenPopup("Error##gb");
                } else {
                    char playerName[PLAYER_NAME_LEN];
                    gameFrontGetPlayerName(playerName);
                    if (strlen(playerName) == 0) {
                        errorMsg = "You must set a player name first.";
                        ImGui::OpenPopup("Error##gb");
                    } else {
                        gameFrontSetUdpOptions(playerName, (char *)e.address, e.port, 0);
                        gameFrontSetAIType(e.ai);
                        gameFrontSetDlgState(openUdpJoin);
                        result = (int)openUdpJoin;
                        running = false;
                    }
                }
            }
            if (!hasSelection) ImGui::EndDisabled();

            /* Rejoin */
            ImGui::SameLine();
            if (!hasSelection) ImGui::BeginDisabled();
            if (ImGui::Button("Rejoin", ImVec2(btnW, btnH))) {
                const ServerEntry &e = servers[selectedItem];
                if (strlen(e.version) < STRVER_LEN ||
                    strncmp(e.version, STRVER, STRVER_LEN) != 0) {
                    errorMsg = "Server is running a different version of WinBolo.";
                    ImGui::OpenPopup("Error##gb");
                } else {
                    char playerName[PLAYER_NAME_LEN];
                    gameFrontGetPlayerName(playerName);
                    if (strlen(playerName) == 0) {
                        errorMsg = "You must set a player name first.";
                        ImGui::OpenPopup("Error##gb");
                    } else {
                        gameFrontSetUdpOptions(playerName, (char *)e.address, e.port, 0);
                        gameFrontSetAIType(e.ai);
                        gameFrontEnableRejoin();
                        gameFrontSetDlgState(openUdpJoin);
                        result = (int)openUdpJoin;
                        running = false;
                    }
                }
            }
            if (!hasSelection) ImGui::EndDisabled();

            /* New Game */
            ImGui::SameLine();
            if (ImGui::Button("New Game", ImVec2(btnW, btnH))) {
                char playerName[PLAYER_NAME_LEN];
                gameFrontGetPlayerName(playerName);
                gameFrontSetUdpOptions(playerName, (char *)"", 27500, 0);
                openingStates setupState = useTracker ? openInternetSetup : openLanSetup;
                gameFrontSetDlgState(setupState);
                result = (int)setupState;
                running = false;
            }

            /* Player Name */
            ImGui::SameLine(0.0f, 20.0f);
            if (ImGui::Button("Player Name", ImVec2(btnW + 30.0f * s, btnH))) {
                gameFrontGetPlayerName(nameEditBuf);
                ImGui::OpenPopup("Set Player Name##gb");
            }

            /* Manual Connect */
            ImGui::SameLine();
            if (ImGui::Button("Manual", ImVec2(btnW, btnH))) {
                /* Open the existing UDP setup dialog */
                gameFrontSetDlgState(useTracker ? openInternetManual : openLanManual);
                result = useTracker ? (int)openInternetManual : (int)openLanManual;
                running = false;
            }

            /* Cancel - right-aligned */
            ImGui::SameLine(panelW - btnW - 16.0f * s);
            bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                              !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
            if (ImGui::Button("Cancel", ImVec2(btnW, btnH)) || escPressed) {
                gameFrontSetDlgState(openWelcome);
                running = false;
            }
        }

        /* ---- Error popup ---- */
        if (ImGui::BeginPopupModal("Error##gb", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", errorMsg ? errorMsg : "");
            ImGui::Spacing();
            if (ImGui::Button("OK##err", ImVec2(80, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        /* ---- Set Player Name popup ---- */
        if (ImGui::BeginPopupModal("Set Player Name##gb", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Player Name:");
            ImGui::SameLine(120 * s);
            ImGui::SetNextItemWidth(200 * s);
            ImGui::InputText("##nameEdit", nameEditBuf, PLAYER_NAME_LEN);
            ImGui::Spacing();
            if (ImGui::Button("OK##name", ImVec2(80 * s, 0))) {
                gameFrontSetPlayerName(nameEditBuf);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0.0f, 8.0f);
            if (ImGui::Button("Cancel##name", ImVec2(80 * s, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End(); /* ##GameBrowser panel */
        ImGui::End(); /* ##GameBrowserBg host */

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        /* Draw map background and semi-transparent overlay */
        if (hasBg) {
            bgGameRenderWithOverlay(bg, renderer, winW, winH);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    /* Detach search thread if still running — static state keeps it safe */
    if (searchThread.joinable()) {
        searchThread.detach();
    }

    /* Ping threads are fire-and-forget (detached), nothing to clean up */

    /* Destroy refresh icon texture */
    if (s_refreshIcon) { SDL_DestroyTexture(s_refreshIcon); s_refreshIcon = nullptr; }
    s_refreshIconAttempted = false;

    /* Tear down ImGui */
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    /* Restore logical presentation */
    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    /* Flush any quit events so the main loop doesn't exit immediately */
    SDL_FlushEvent(SDL_EVENT_QUIT);

    return result;
}
