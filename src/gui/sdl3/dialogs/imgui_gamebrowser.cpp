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
#include <condition_variable>
#include <deque>
#include <vector>
#include <string>

#include <SDL3/SDL.h>

#include "../../../common/wb_log.h"

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
#include "../flags.h"
#include "../sdl3imgui.h"
#include "../../gamefront.h"
#include "../../../winbolonet/wbn_serverlist.h"
#include "discovery.h"
#include "global.h"
#include "gametype.h"
#include "wire_limits.h"
#include "../../../server/geolookup.h"
#include "../../lang.h"
#include "../input_source.h"
#include "imgui_gamebrowser.h"
}

static const int DIALOG_W = 1024;
static const int DIALOG_H = 768;

#define STRVER_LEN 4

/* Servers older than this are hidden from the tracker list. Version strings
 * are "%d.%d%d" (e.g. "1.19"), so a lexicographic compare of the first
 * STRVER_LEN chars orders the 1.x series correctly. */
#define BROWSER_MIN_VERSION "1.19"

static bool browserVersionAllowed(const char *ver) {
    if (ver == nullptr || strlen(ver) < STRVER_LEN) return false;
    return strncmp(ver, BROWSER_MIN_VERSION, STRVER_LEN) >= 0;
}

/* ---- Per-server enriched data ---- */
struct ServerEntry {
    /* From tracker/broadcast */
    char address[FILENAME_MAX];
    char hostName[256];   /* reverse-DNS name of address; "" until resolved / on PTR miss */
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

    /* Lobby status (derived from hasLobby + inLobby) */
    int lobbyStatus;     /* 1=in lobby, 0=in game (derived from hasLobby+inLobby) */

    /* From WinBolo.net game list (Internet path) */
    char serverKey[64];
    int  numHumans;
    int  numBots;
    int  maxPlayers;
    bool ranked;
    bool inLobby;
    bool hasLobby;
    bool allowNewPlayers;
    bool autoLock;
    bool allowSpectators;
    int  spectatorCount;
    bool timeLimit;
    int  timeMinutes;
    bool randomMap;
    char mapMd5[33];
    std::vector<std::string> players;   /* logged-in usernames, blanks already filtered */
};

static const char *gameTypeStr(gameType g) {
    switch (g) {
    case gameOpen:           return langGetText(STR_DLGGAMEINFO_OPEN);
    case gameTournament:     return langGetText(STR_DLGGAMEINFO_TOURN);
    case gameStrictTournament:
    default:                 return langGetText(STR_DLGGAMESETUP_STRICT_SHORT);
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
    char hostName[256];
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

/* Reverse-resolve a server address (usually a bare IP) to a hostname.
 * Returns true and fills hostOut on success; leaves hostOut untouched on failure
 * (including no PTR record, via NI_NAMEREQD). */
static bool resolveHostName(const char *address, char *hostOut, size_t hostOutSize) {
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(address, nullptr, &hints, &res) != 0 || !res) {
        return false;
    }
    char host[NI_MAXHOST];
    int rc = getnameinfo(res->ai_addr, (socklen_t)res->ai_addrlen,
                         host, sizeof(host), nullptr, 0, NI_NAMEREQD);
    freeaddrinfo(res);
    if (rc != 0) {
        return false;
    }
    SDL_strlcpy(hostOut, host, hostOutSize);
    return true;
}

/* Send an info request to a server and measure RTT.
 * Thin wrapper around discoveryPingServer; the bolo helper owns the
 * socket and the wire-format parsing. */
static PingResult pingServer(const PingWork &work) {
    PingResult res;
    res.index = work.index;
    res.pingMs = -2;
    res.freePills = 0;
    res.freeBases = 0;
    res.numPlayers = 0;
    res.hostName[0] = '\0';

    /* Reverse-DNS the address regardless of whether the UDP info-ping
     * answers, so even unresponsive servers get a hostname. */
    resolveHostName(work.address, res.hostName, sizeof(res.hostName));

    DiscoveryPingResult dpr;
    if (discoveryPingServer(work.address, work.port, &dpr)) {
        res.pingMs    = dpr.rttMs;
        res.freePills = dpr.freePills;
        res.freeBases = dpr.freeBases;
        res.numPlayers = dpr.numPlayers;
    }
    return res;
}

/* ---- Bounded ping pool ----
 * The server list can be large; spawning one detached thread per server
 * (and never joining them) let blocking 5s pings pile up across repeated
 * browses. A fixed pool of worker threads drains a queue instead, so at
 * most kPingPoolSize pings run at once regardless of list size. A
 * generation counter — bumped on each new search and on browser open —
 * lets workers drop results that belong to a superseded server list
 * (whose indices no longer match). The pool threads live for the process
 * (like the bot pool) and idle on a condition variable when empty. */
static constexpr int            kPingPoolSize = 8;
static std::mutex               s_pingResultsMtx;
static std::vector<PingResult>  s_pingResults;
struct PingJob { PingWork work; uint64_t gen; };
static std::mutex               s_pingQueueMtx;
static std::condition_variable  s_pingQueueCv;
static std::deque<PingJob>      s_pingQueue;
static std::atomic<uint64_t>    s_pingGeneration{0};
static std::once_flag           s_pingPoolOnce;

static void pingWorkerFn() {
    for (;;) {
        PingJob job;
        {
            std::unique_lock<std::mutex> lk(s_pingQueueMtx);
            s_pingQueueCv.wait(lk, [] { return !s_pingQueue.empty(); });
            job = s_pingQueue.front();
            s_pingQueue.pop_front();
        }
        /* Skip work already superseded by a newer search. */
        if (job.gen != s_pingGeneration.load()) continue;
        PingResult pr = pingServer(job.work);
        std::lock_guard<std::mutex> lk(s_pingResultsMtx);
        if (job.gen == s_pingGeneration.load()) {
            s_pingResults.push_back(pr);
        }
    }
}

static void enqueuePing(const PingWork &w) {
    std::call_once(s_pingPoolOnce, [] {
        for (int i = 0; i < kPingPoolSize; i++) {
            std::thread(pingWorkerFn).detach();
        }
    });
    {
        std::lock_guard<std::mutex> lk(s_pingQueueMtx);
        s_pingQueue.push_back(PingJob{w, s_pingGeneration.load()});
    }
    s_pingQueueCv.notify_one();
}

/* Abandon pending/in-flight pings and clear stale results. Called on each
 * new search and on browser open so old results can't map onto a rebuilt
 * server list. */
static void resetPings() {
    s_pingGeneration.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(s_pingQueueMtx);
        s_pingQueue.clear();
    }
    std::lock_guard<std::mutex> lk(s_pingResultsMtx);
    s_pingResults.clear();
}

/* ---- Callback data for async LAN broadcast search ---- */
struct BroadcastCbData {
    std::vector<ServerEntry> *servers;
    std::mutex *serversMtx;
};

static ServerEntry serverEntryFromDiscovery(const DiscoveryServer *src) {
    ServerEntry e = {};
    e.pingMs = -1;
    e.freePills = 0;
    e.freeBases = 0;
    e.lobbyStatus = 0;

    SDL_strlcpy(e.address, src->address, sizeof(e.address));
    e.port = src->port;
    SDL_strlcpy(e.mapName, src->mapName, sizeof(e.mapName));
    SDL_snprintf(e.version, sizeof(e.version), "%d.%d%d",
                 src->versionMajor, src->versionMinor, src->versionRevision);
    e.numPlayers = src->numPlayers;
    e.numBases   = src->numBases;
    e.numPills   = src->numPills;
    e.mines      = src->mines;
    e.game       = src->game;
    e.ai         = src->ai;
    e.password   = src->password;

    resolveCountryCode(e);
    return e;
}

extern "C" void broadcastServerCallback(const DiscoveryServer *server, void *userData) {
    BroadcastCbData *cbd = (BroadcastCbData *)userData;

    ServerEntry e = serverEntryFromDiscovery(server);

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

    /* Queue a ping for this server (bounded pool). */
    PingWork pw = {};
    SDL_strlcpy(pw.address, e.address, FILENAME_MAX);
    pw.port = e.port;
    pw.index = idx;
    enqueuePing(pw);
}

/* ---- Refresh icon (loaded from SVG) ---- */
static SDL_Texture *s_refreshIcon = nullptr;
static bool s_refreshIconAttempted = false;

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
    {
      MessageArgs args = {};
      SDL_strlcpy(args.string1, title, sizeof(args.string1));
      dialogSetWindowTitle(window, langGetTextFmt(STR_DLGBROWSER_WINTITLE, &args));
    }
    SDL_SetWindowResizable(window, true);
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    /* Set up ImGui context for this dialog */
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
        WB_LOG_INFO(WB_LOG_CAT_ASSET, "[GameBrowser] GeoIP database loaded: %s", geoLookupIsLoaded() ? "yes" : "no");
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
    /* LAN mDNS browse runs on its own thread alongside the broadcast worker
     * (non-tracker searches only); both feed broadcastServerCallback, which
     * dedupes by address+port under serversMtx. */
    static std::thread mdnsThread;
    static bool searchResultOk = false;
    static WbnServerList searchResultList = {};

    /* Clean up any leftover state from a previous detached search */
    if (searchDone) {
        searchDone = false;
        searching = false;
        if (searchThread.joinable()) searchThread.join();
        if (mdnsThread.joinable()) mdnsThread.join();
    }

    /* Drop any pings still in flight from a previous browser session. */
    resetPings();

    /* Filter state */
    int filterGameType = -1; /* -1 = all */
    bool filterLocked = false;
    int filterLobby = -1;    /* -1 = all, 0 = in game, 1 = in lobby */

    /* Status */
    bool loadingGames = false;
    /* "Click Refresh..." prompt is keyboard/mouse-specific; suppress for gamepad. */
    const char *statusText = (inputSourceCurrent() == INPUT_SOURCE_KEYBOARD)
                                 ? (useTracker ? langGetText(STR_DLGBROWSER_TRACKER_INSTRUCTION) : langGetText(STR_DLGBROWSER_LAN_INSTRUCTION))
                                 : "";

    /* Error popup */
    const char *errorMsg = nullptr;

    /* Set Name popup */
    char nameEditBuf[PLAYER_NAME_LEN] = {};

    int result = -1;
    bool running = true;

    /* Auto-refresh on open */
    bool autoRefresh = true;
    bool autoRefreshEnabled = true;   /* user toggle (Internet tab); gates the periodic re-poll */
    /* Last good WinBolo.net MOTD lines; kept across a failed refresh. */
    std::vector<std::string> motdLines;
    bool autoPollEnabled = true;   /* Internet path: cleared on any fetch failure, re-armed on manual refresh */
    Uint64 lastFetchTime = 0;      /* SDL_GetTicks() when the last Internet fetch finished; 0 = none yet */
    constexpr Uint64 kInternetAutoRefreshMs = 20000;  /* ~20s; list freshness window is 5min, so faster is pointless */

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
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
            /* The mDNS worker (LAN searches) shares the 5s window and is
             * effectively done too; join it so it doesn't linger. */
            if (mdnsThread.joinable()) {
                mdnsThread.join();
            }
            searching = false;

            if (useTracker) {
                /* Internet mode: map the WinBolo.net game list into our vector */
                if (searchResultOk) {
                    std::vector<ServerEntry> newServers;
                    for (int i = 0; i < searchResultList.count; i++) {
                        const WbnServerListEntry &w = searchResultList.servers[i];
                        ServerEntry e = {};

                        SDL_strlcpy(e.address, w.address, sizeof(e.address));
                        e.port = (unsigned short)w.port;
                        SDL_strlcpy(e.mapName, w.map, sizeof(e.mapName));
                        SDL_strlcpy(e.version, w.version, sizeof(e.version));
                        e.game = (gameType)w.gameType;
                        e.ai = (aiType)w.ai;
                        e.mines = w.mines;
                        e.password = w.password;
                        e.numPlayers = (BYTE)w.numPlayers;
                        e.freeBases = (WORD)w.freeBases;
                        e.freePills = (WORD)w.freePills;
                        e.numBases = (BYTE)w.numBases;
                        e.numPills = (BYTE)w.numPills;
                        /* Country comes straight from the JSON on this path. */
                        SDL_strlcpy(e.countryCode, w.country, sizeof(e.countryCode));

                        SDL_strlcpy(e.serverKey, w.serverKey, sizeof(e.serverKey));
                        SDL_strlcpy(e.mapMd5, w.mapMd5, sizeof(e.mapMd5));
                        e.numHumans = w.numHumans;
                        e.numBots = w.numBots;
                        e.maxPlayers = w.maxPlayers;
                        e.ranked = w.ranked;
                        e.inLobby = w.inLobby;
                        e.hasLobby = w.hasLobby;
                        e.allowNewPlayers = w.allowNewPlayers;
                        e.autoLock = w.autoLock;
                        e.allowSpectators = w.allowSpectators;
                        e.spectatorCount = w.spectatorCount;
                        e.timeLimit = w.timeLimit;
                        e.timeMinutes = w.timeMinutes;
                        e.randomMap = w.randomMap;

                        e.players.clear();
                        for (int p = 0; p < w.numPlayerNames; p++) {
                            e.players.emplace_back(w.players[p]);
                        }

                        e.pingMs = -1; /* pending — ping still fires below */
                        e.lobbyStatus = (w.hasLobby && w.inLobby) ? 1 : 0;

                        /* Hide servers older than BROWSER_MIN_VERSION. */
                        if (!browserVersionAllowed(e.version)) {
                            continue;
                        }

                        newServers.push_back(std::move(e));
                    }
                    {
                        std::lock_guard<std::mutex> lock(serversMtx);
                        servers = std::move(newServers);
                        selectedItem = -1;
                    }
                    /* Copy the MOTD out before wbnServerListFree below; a later
                     * failed fetch zeroes the whole struct, so this keeps the
                     * last good MOTD alongside the kept server list. */
                    motdLines.clear();
                    for (int m = 0; m < searchResultList.numMotd; m++) {
                        motdLines.emplace_back(searchResultList.motd[m]);
                    }
                    int total = (int)servers.size();
                    if (total > 0) {
                        statusText = langGetText(STR_DLGBROWSER_GAMES_LOADED);
                        /* Queue async pings to each server (bounded pool) */
                        for (int i = 0; i < total; i++) {
                            PingWork pw = {};
                            SDL_strlcpy(pw.address, servers[i].address, FILENAME_MAX);
                            pw.port = servers[i].port;
                            pw.index = i;
                            enqueuePing(pw);
                        }
                    } else {
                        statusText = langGetText(STR_DLGBROWSER_NO_GAMES);
                    }
                } else {
                    /* Fetch failed (transport error or 429): keep the last good
                     * list shown and stop auto-polling until the user refreshes. */
                    statusText = langGetText(STR_DLGBROWSER_SEARCH_FAILED);
                    autoPollEnabled = false;
                }
                wbnServerListFree(&searchResultList);
                loadingGames = false;
                lastFetchTime = SDL_GetTicks();
            } else {
                /* LAN mode: servers were added incrementally by the callback,
                 * pings already fired per-server. Just update status. */
                int total;
                {
                    std::lock_guard<std::mutex> lock(serversMtx);
                    total = (int)servers.size();
                    selectedItem = -1;
                }

                if (searchResultOk && total > 0) {
                    statusText = langGetText(STR_DLGBROWSER_GAMES_LOADED);
                } else if (searchResultOk) {
                    statusText = langGetText(STR_DLGBROWSER_NO_GAMES);
                } else {
                    statusText = langGetText(STR_DLGBROWSER_SEARCH_FAILED);
                }
                loadingGames = false;
            }
        }

        /* Process incoming ping results */
        {
            std::lock_guard<std::mutex> lock(s_pingResultsMtx);
            for (auto &pr : s_pingResults) {
                std::lock_guard<std::mutex> slock(serversMtx);
                if (pr.index >= 0 && pr.index < (int)servers.size()) {
                    servers[pr.index].pingMs = pr.pingMs;
                    /* Copy the reverse-DNS name only when it resolved, so a PTR
                     * miss never clobbers a previously shown name/IP. Applies to
                     * both the LAN and Internet paths. */
                    if (pr.hostName[0] != '\0') {
                        SDL_strlcpy(servers[pr.index].hostName, pr.hostName,
                                    sizeof(servers[pr.index].hostName));
                    }
                    /* Internet path: counts come from the WinBolo.net JSON and
                     * must not be overwritten by the latency ping (a server that
                     * doesn't answer the info-ping would zero them). LAN has no
                     * such source, so the ping still fills the counts there. */
                    if (!useTracker) {
                        servers[pr.index].freePills = pr.freePills;
                        servers[pr.index].freeBases = pr.freeBases;
                        if (pr.numPlayers > 0) {
                            servers[pr.index].numPlayers = (BYTE)pr.numPlayers;
                        }
                    }
                }
            }
            s_pingResults.clear();
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
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

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
                        WB_LOG_DEBUG(WB_LOG_CAT_ASSET, "[GameBrowser] Trying refresh icon: %s", candidates[i]);
                        s_refreshIcon = imguiLoadSvgIcon(renderer, candidates[i], iconSize);
                    }
                }
                WB_LOG_DEBUG(WB_LOG_CAT_ASSET, "[GameBrowser] Refresh icon loaded: %s", s_refreshIcon ? "yes" : "no");
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.75f, 0.3f, 1.0f));
            ImGui::SetWindowFontScale(1.3f);
            ImGui::Text("%s", title);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();

            /* Refresh button on the right side of the title bar */
            bool doRefresh = autoRefresh;
            /* Internet tab: re-poll periodically while open, until a fetch fails. */
            if (useTracker && autoRefreshEnabled && autoPollEnabled && !searching && lastFetchTime != 0 &&
                SDL_GetTicks() - lastFetchTime >= kInternetAutoRefreshMs) {
                doRefresh = true;
            }
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
                imguiHandOnHover();
                if (wasSearching) ImGui::EndDisabled();
            } else {
                /* Text fallback when SVG icon is unavailable */
                float btnW = 70.0f * s;
                ImGui::SameLine(panelW - btnW - 16.0f * s);
                bool wasSearching = searching;
                if (wasSearching) ImGui::BeginDisabled();
                if (ImGui::SmallButton(langGetText(STR_DLGBROWSER_REFRESH))) {
                    doRefresh = true;
                }
                imguiHandOnHover();
                if (wasSearching) ImGui::EndDisabled();
            }
            if (doRefresh) {
                autoRefresh = false;
                autoPollEnabled = true;
                statusText = langGetText(STR_DLGBROWSER_SEARCHING);
                loadingGames = true;

                if (!useTracker) {
                    selectedItem = -1;
                    std::lock_guard<std::mutex> lock(serversMtx);
                    servers.clear();
                }

                bool ut = (useTracker != 0);

                if (searchThread.joinable()) {
                    searchThread.join();
                }
                if (mdnsThread.joinable()) {
                    mdnsThread.join();
                }
                /* Old search fully stopped: supersede any pings from the
                 * previous list so their results can't land on the rebuilt
                 * indices, and drop pending/in-flight work. */
                resetPings();

                wbnServerListFree(&searchResultList);
                searchResultOk = false;
                searching = true;

                struct SearchParams { bool tracker; };
                SearchParams sp = {};
                sp.tracker = ut;

                searchThread = std::thread([sp]() {
                    bool ret = false;
                    if (sp.tracker) {
                        ret = wbnFetchServerList(&searchResultList);
                    } else {
                        static BroadcastCbData cbd;
                        cbd.servers = &servers;
                        cbd.serversMtx = &serversMtx;
                        ret = discoveryFindBroadcastGamesAsync(broadcastServerCallback, &cbd);
                    }
                    searchResultOk = ret;
                    searchDone = true;
                });

                /* LAN searches also browse via mDNS on a parallel thread,
                 * feeding the same dedup callback. The broadcast worker above
                 * owns the searchDone/searchResultOk signal; this thread just
                 * contributes additional servers. */
                if (!sp.tracker) {
                    mdnsThread = std::thread([]() {
                        static BroadcastCbData mcbd;
                        mcbd.servers = &servers;
                        mcbd.serversMtx = &serversMtx;
                        discoveryFindMdnsGamesAsync(broadcastServerCallback, &mcbd);
                    });
                }
            }

            ImGui::Separator();
            ImGui::Spacing();
        }

        /* ---- WinBolo.net MOTD (Internet path only) ---- */
        /* Server-supplied content, not a localized UI string. Rendered wrapped
         * in the default text colour with trailing spacing; kept across a
         * failed refresh so it doesn't flicker. */
        if (useTracker && !motdLines.empty()) {
            for (const auto &line : motdLines) {
                ImGui::TextWrapped("%s", line.c_str());
            }
            ImGui::Spacing();
        }

        /* ---- Loading banner ---- */
        if (loadingGames) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.5f, 1.0f));
            float time = (float)SDL_GetTicks() / 1000.0f;
            const char *dots[] = { "", ".", "..", "..." };
            int dotIdx = ((int)(time * 2.0f)) % 4;
            MessageArgs args = {};
            SDL_strlcpy(args.string1, dots[dotIdx], sizeof(args.string1));
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGBROWSER_LOADING, &args));
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
            ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 28.0f);  /* St */
            ImGui::TableSetupColumn(langGetText(STR_DLGBROWSER_COL_SERVER),  ImGuiTableColumnFlags_WidthStretch, 0.0f);  /* Server / Map */
            ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 24.0f);  /* flag */
            ImGui::TableSetupColumn(langGetText(STR_DLGBROWSER_COL_PLAYERS), ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn(langGetText(STR_DLGBROWSER_COL_BASES),   ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn(langGetText(STR_DLGBROWSER_COL_PILLS),   ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn(langGetText(STR_DLGBROWSER_COL_TYPE),    ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn(langGetText(STR_DLGBROWSER_COL_VER),     ImGuiTableColumnFlags_WidthFixed, 45.0f);
            ImGui::TableSetupColumn(langGetText(STR_DLGBROWSER_COL_PING),    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 50.0f);
            ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 24.0f);  /* lock */
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
                        case 1: cmp = strcmp(a.hostName[0] ? a.hostName : a.address,
                                             b.hostName[0] ? b.hostName : b.address); break;
                        case 3: cmp = (int)a.numPlayers - (int)b.numPlayers; break;
                        case 4: cmp = (int)a.freeBases - (int)b.freeBases; break;
                        case 5: cmp = (int)a.freePills - (int)b.freePills; break;
                        case 6: cmp = (int)a.game - (int)b.game; break;
                        case 7: cmp = strcmp(a.version, b.version); break;
                        case 8: {
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

                    /* St — lobby vs in-game marker (placeholder letter until icons land) */
                    ImGui::TableNextColumn();
                    if (e.hasLobby && e.inLobby) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "L");
                        ImGui::SetItemTooltip("%s", langGetText(STR_DLGBROWSER_FILTER_INLOBBY));
                    } else {
                        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "G");
                        ImGui::SetItemTooltip("%s", langGetText(STR_DLGBROWSER_FILTER_INGAME));
                    }

                    /* Server / Map — host line (selectable) + map line in one cell */
                    ImGui::TableNextColumn();
                    {
                        char label[256];
                        SDL_snprintf(label, sizeof(label), "%s:%u",
                                     e.hostName[0] != '\0' ? e.hostName : e.address, e.port);
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
                        imguiHandOnHover();
                        /* Row detail popup — the SpanAllColumns selectable owns the
                         * whole row's hover, so attach the tooltip here. Carries
                         * only detail that isn't already a column: AI level, hidden
                         * mines, time limit, lobby flags and the player roster. */
                        if (ImGui::IsItemHovered()) {
                            char buf[512];
                            ImGui::BeginTooltip();

                            /* Header: server identity */
                            ImGui::TextUnformatted(label);
                            ImGui::Separator();

                            /* AI level (the Type column only flags its presence) */
                            if (e.ai != aiNone) {
                                int aiStr = STR_YES;
                                if (e.ai == aiYesAdvantage) aiStr = STR_DLGGAMEINFO_AIADV;
                                else if (e.ai == aiFull)    aiStr = STR_DLGGAMEINFO_FULLADV;
                                SDL_snprintf(buf, sizeof(buf), "%s %s",
                                             langGetText(STR_DLGGAMEINFO_AILABEL),
                                             langGetText(aiStr));
                                ImGui::TextUnformatted(buf);
                            }

                            /* Hidden mines */
                            if (e.mines) {
                                ImGui::TextUnformatted(langGetText(STR_DLGGAMESETUP_HIDDENMINES_SHORT));
                            }

                            /* Time limit */
                            if (e.timeLimit) {
                                SDL_snprintf(buf, sizeof(buf), "%s: %d",
                                             langGetText(STR_DLGGAMESETUP_TIMELIMIT_SHORT),
                                             e.timeMinutes);
                            } else {
                                SDL_snprintf(buf, sizeof(buf), "%s: %s",
                                             langGetText(STR_DLGGAMESETUP_TIMELIMIT_SHORT),
                                             langGetText(STR_DLGGAMEINFO_UNLIMITED));
                            }
                            ImGui::TextUnformatted(buf);

                            /* Flags */
                            if (e.ranked) {
                                ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_RANKED));
                            }
                            if (e.randomMap) {
                                ImGui::TextUnformatted(langGetText(STR_MAPCHOOSER_RANDOMMAP));
                            }
                            if (e.autoLock) {
                                ImGui::TextUnformatted(langGetText(STR_DLGBROWSER_AUTOLOCK_HINT));
                            }
                            if (e.password) {
                                ImGui::TextUnformatted(langGetText(STR_DLGBROWSER_LOCK_PASSWORD));
                            }
                            if (!e.allowNewPlayers) {
                                ImGui::TextUnformatted(langGetText(STR_DLGBROWSER_LOCK_NONEWPLAYERS));
                            }

                            /* Player roster — names aren't shown in the row */
                            if (!e.players.empty() || e.numBots > 0) {
                                ImGui::Separator();
                                for (const auto &name : e.players) {
                                    ImGui::TextUnformatted(name.c_str());
                                }
                                for (int b = 0; b < e.numBots; b++) {
                                    ImGui::TextUnformatted("[bot]");
                                }
                            }

                            ImGui::EndTooltip();
                        }
                        /* Second line: map name with ranked (*) / random (rnd)
                         * markers. Placeholder marker text, not localized. */
                        char mapLine[MAP_STR_SIZE + 32];
                        SDL_snprintf(mapLine, sizeof(mapLine), "  %s%s%s", e.mapName,
                                     e.ranked ? " *" : "",
                                     e.randomMap ? " (rnd)" : "");
                        ImGui::TextDisabled("%s", mapLine);
                    }

                    /* Flag */
                    ImGui::TableNextColumn();
                    if (e.countryCode[0] != '\0' &&
                        e.countryCode[0] != 'X') {
                        if (!drawCountryFlagWithTip(e.countryCode)) {
                            ImGui::TextDisabled("%c%c", e.countryCode[0], e.countryCode[1]);
                        }
                    } else if (e.countryCode[0] != '\0') {
                        ImGui::TextDisabled("%c%c", e.countryCode[0], e.countryCode[1]);
                    }

                    /* Players — n/m (Nh Nb); roster lives in the row detail tooltip */
                    ImGui::TableNextColumn();
                    ImGui::Text("%d/%d (%dh %db)", e.numPlayers, e.maxPlayers,
                                e.numHumans, e.numBots);

                    /* Bases (free/total, from JSON) */
                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", e.freeBases, e.numBases);

                    /* Pills (free/total, from JSON) */
                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", e.freePills, e.numPills);

                    /* Type — folds in AI and mines markers */
                    ImGui::TableNextColumn();
                    {
                        char typeBuf[96];
                        int n = SDL_snprintf(typeBuf, sizeof(typeBuf), "%s", gameTypeStr(e.game));
                        if (e.ai != aiNone && n > 0 && (size_t)n < sizeof(typeBuf)) {
                            n += SDL_snprintf(typeBuf + n, sizeof(typeBuf) - (size_t)n,
                                              " %s", langGetText(STR_DLGBROWSER_TYPE_AI));
                        }
                        if (e.mines && n > 0 && (size_t)n < sizeof(typeBuf)) {
                            SDL_snprintf(typeBuf + n, sizeof(typeBuf) - (size_t)n,
                                         " %s", langGetText(STR_DLGBROWSER_TYPE_MINES));
                        }
                        ImGui::TextUnformatted(typeBuf);
                    }

                    /* Ver */
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", e.version);

                    /* RTT */
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

                    /* lock — most-restrictive marker first (placeholder letters) */
                    ImGui::TableNextColumn();
                    if (!e.allowNewPlayers) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "N");
                        ImGui::SetItemTooltip("%s", langGetText(STR_DLGBROWSER_LOCK_NONEWPLAYERS));
                    } else if (e.password) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "P");
                        ImGui::SetItemTooltip("%s", langGetText(STR_DLGBROWSER_LOCK_PASSWORD));
                    }
                }
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();

        /* ---- Filters row ---- */
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(langGetText(STR_DLGBROWSER_FILTER));
            ImGui::SameLine();

            ImGui::SetNextItemWidth(130.0f * s);
            const char *gameTypes[] = {
                langGetText(STR_DLGBROWSER_FILTER_ALLTYPES),
                langGetText(STR_DLGGAMEINFO_OPEN),
                langGetText(STR_DLGGAMEINFO_TOURN),
                langGetText(STR_DLGGAMESETUP_STRICT_SHORT),
            };
            int gtIdx = (filterGameType < 0) ? 0 : filterGameType;
            if (ImGui::Combo("##filterType", &gtIdx, gameTypes, 4)) {
                filterGameType = (gtIdx == 0) ? -1 : gtIdx;
            }

            ImGui::SameLine();
            ImGui::Checkbox(langGetText(STR_DLGBROWSER_FILTER_UNLOCKED), &filterLocked);

            ImGui::SameLine();
            ImGui::SetNextItemWidth(130.0f * s);
            const char *lobbyOpts[] = {
                langGetText(STR_DLGBROWSER_FILTER_ALLLOBBY),
                langGetText(STR_DLGBROWSER_FILTER_INLOBBY),
                langGetText(STR_DLGBROWSER_FILTER_INGAME),
            };
            /* index 0=All(-1), 1=In lobby(lobbyStatus 1), 2=In game(lobbyStatus 0) */
            int lobbyIdx = (filterLobby < 0) ? 0 : (filterLobby == 1 ? 1 : 2);
            if (ImGui::Combo("##filterLobby", &lobbyIdx, lobbyOpts, 3)) {
                filterLobby = (lobbyIdx == 0) ? -1 : (lobbyIdx == 1 ? 1 : 0);
            }

            if (useTracker) {
                const char *arLabel = langGetText(STR_DLGBROWSER_AUTO_REFRESH);
                float cbW = ImGui::CalcTextSize(arLabel).x + ImGui::GetFrameHeight()
                          + ImGui::GetStyle().ItemInnerSpacing.x;
                float rightX = ImGui::GetContentRegionMax().x - cbW;
                ImGui::SameLine();
                if (ImGui::GetCursorPosX() < rightX) ImGui::SetCursorPosX(rightX);
                ImGui::Checkbox(arLabel, &autoRefreshEnabled);
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
            MessageArgs args = {};
            SDL_strlcpy(args.string1, statusText, sizeof(args.string1));
            args.number = total;
            if (pendingPings > 0) {
                args.number2 = pendingPings;
                ImGui::TextUnformatted(langGetTextFmt(STR_DLGBROWSER_STATUS_PINGING, &args));
            } else {
                ImGui::TextUnformatted(langGetTextFmt(STR_DLGBROWSER_STATUS, &args));
            }
        }

        ImGui::Spacing();

        /* Modal popup IDs (built once per frame, used by both Open and Begin) */
        char errPopupId[64];
        SDL_snprintf(errPopupId, sizeof(errPopupId), "%s##gb", langGetText(STR_ERR_TITLE));
        char setNamePopupId[64];
        SDL_snprintf(setNamePopupId, sizeof(setNamePopupId), "%s##gb", langGetText(STR_DLGSETPLAYERNAME_TITLE));

        /* ---- Button row ---- */
        float btnW = 110.0f * s;
        float btnH = 28.0f * s;
        {
            bool hasSelection = (selectedItem >= 0 && selectedItem < (int)servers.size());

            /* Join */
            if (!hasSelection) ImGui::BeginDisabled();

            if (ImGui::Button(langGetText(STR_DLGTCP_JOIN), ImVec2(btnW, btnH))) {
                const ServerEntry &e = servers[selectedItem];
                /* No version-equality gate — the pre-flight info-request
                 * inside gameFrontSetDlgState(openUdpJoin) surfaces a
                 * localized "Server is version X, you have Y" error if
                 * the build mismatches. */
                char playerName[PLAYER_NAME_LEN];
                gameFrontGetPlayerName(playerName);
                if (strlen(playerName) == 0) {
                    errorMsg = langGetText(STR_DLGBROWSER_ERR_NEEDNAME);
                    ImGui::OpenPopup(errPopupId);
                } else {
                    gameFrontSetUdpOptions(playerName, (char *)e.address, e.port, 0);
                    gameFrontSetAIType(e.ai);
                    gameFrontSetDlgState(openUdpJoin);
                    result = (int)openUdpJoin;
                    running = false;
                }
            }
            imguiHandOnHover();
            if (!hasSelection) ImGui::EndDisabled();

            /* Spectate — placeholder, disabled until spectator fields land */
            ImGui::SameLine();
            ImGui::BeginDisabled();
            ImGui::Button(langGetText(STR_DLGBROWSER_SPECTATE), ImVec2(btnW, btnH));
            ImGui::EndDisabled();

            /* Rejoin */
            ImGui::SameLine();
            if (!hasSelection) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_DLGTCP_REJOIN), ImVec2(btnW, btnH))) {
                const ServerEntry &e = servers[selectedItem];
                char playerName[PLAYER_NAME_LEN];
                gameFrontGetPlayerName(playerName);
                if (strlen(playerName) == 0) {
                    errorMsg = langGetText(STR_DLGBROWSER_ERR_NEEDNAME);
                    ImGui::OpenPopup(errPopupId);
                } else {
                    gameFrontSetUdpOptions(playerName, (char *)e.address, e.port, 0);
                    gameFrontSetAIType(e.ai);
                    gameFrontEnableRejoin();
                    gameFrontSetDlgState(openUdpJoin);
                    result = (int)openUdpJoin;
                    running = false;
                }
            }
            imguiHandOnHover();
            if (!hasSelection) ImGui::EndDisabled();

            /* New Game */
            ImGui::SameLine();
            if (ImGui::Button(langGetText(STR_DLGBROWSER_NEWGAME), ImVec2(btnW, btnH))) {
                char playerName[PLAYER_NAME_LEN];
                gameFrontGetPlayerName(playerName);
                gameFrontSetUdpOptions(playerName, (char *)"", 27500, 27500);
                openingStates setupState = useTracker ? openInternetSetup : openLanSetup;
                gameFrontSetDlgState(setupState);
                result = (int)setupState;
                running = false;
            }
            imguiHandOnHover();
            if (useTracker && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGBROWSER_NEWGAME_PORTFWD_TIP));
            }

            /* Player Name */
            ImGui::SameLine(0.0f, 20.0f);
            if (ImGui::Button(langGetText(STR_DLGBROWSER_PLAYER_NAME_BTN), ImVec2(btnW + 30.0f * s, btnH))) {
                gameFrontGetPlayerName(nameEditBuf);
                ImGui::OpenPopup(setNamePopupId);
            }
            imguiHandOnHover();

            /* Manual Connect */
            ImGui::SameLine();
            if (ImGui::Button(langGetText(STR_DLGBROWSER_MANUAL), ImVec2(btnW, btnH))) {
                /* Open the existing UDP setup dialog */
                gameFrontSetDlgState(useTracker ? openInternetManual : openLanManual);
                result = useTracker ? (int)openInternetManual : (int)openLanManual;
                running = false;
            }
            imguiHandOnHover();

            /* Cancel - right-aligned, muted-grey styling per dialog spec. */
            ImGui::SameLine(panelW - btnW - 16.0f * s);
            WBUI::PushCancelStyle();
            bool cancelClicked = ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, btnH));
            WBUI::PopCancelStyle();
            if (cancelClicked || WBUI::CancelKeyPressed()) {
                gameFrontSetDlgState(openWelcome);
                running = false;
            }
            imguiHandOnHover();
        }

        /* ---- Error popup ---- */
        static float s_fadeGbErr = 0.0f;
        static bool s_gbErrOpen = true; s_gbErrOpen = true;
        if (ImGui::BeginPopupModal(errPopupId, &s_gbErrOpen,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                imguiPopupFadeAlpha(&s_fadeGbErr));
            ImGui::Text("%s", errorMsg ? errorMsg : "");
            ImGui::Spacing();
            {
                char okBuf[64];
                snprintf(okBuf, sizeof(okBuf), "%s##err", langGetText(STR_OK));
                if (ImGui::Button(okBuf, ImVec2(80, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                imguiHandOnHover();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        /* ---- Set Player Name popup ---- */
        static float s_fadeGbSetName = 0.0f;
        static bool s_gbSetNameOpen = true; s_gbSetNameOpen = true;
        if (ImGui::BeginPopupModal(setNamePopupId, &s_gbSetNameOpen,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                imguiPopupFadeAlpha(&s_fadeGbSetName));
            bool wbnActive = gameFrontGetWinbolonetUse();
            ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_PLAYERNAME));
            ImGui::SameLine(120 * s);
            ImGui::SetNextItemWidth(200 * s);
            if (wbnActive) ImGui::BeginDisabled();
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            bool nameEnter = ImGui::InputText("##nameEdit", nameEditBuf, PLAYER_NAME_LEN,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
            if (wbnActive) ImGui::EndDisabled();
            if (wbnActive) {
                ImGui::TextUnformatted(langGetText(STR_DLGSETNAME_WBN_LOCKED));
            }
            int nameFooter = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                                langGetText(STR_OK),
                                                /*enterConfirms*/ true,
                                                /*showSeparator*/ true,
                                                /*confirmDisabled*/ wbnActive);
            if (nameFooter == WBUI::FOOTER_CONFIRM || (nameEnter && !wbnActive)) {
                gameFrontSetPlayerName(nameEditBuf);
                ImGui::CloseCurrentPopup();
            } else if (nameFooter == WBUI::FOOTER_CANCEL) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        ImGui::End(); /* ##GameBrowser panel */
        ImGui::End(); /* ##GameBrowserBg host */

        dialogDrawNavOutline();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        /* Draw map background and semi-transparent overlay */
        if (hasBg) {
            bgGameRenderWithOverlay(bg, renderer, winW, winH);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        gameFrontPumpDirty(); /* sync cloud prefs from menus (login join + debounced upload) */
        dialogFrameCapEnd(frameCapStart);
    }

    /* Halt the LAN broadcast search and drop the ping pool's pending
     * work before the dialog returns. Without this, the New Game path
     * that follows opens a fresh UDP server on port 27500, and the
     * still-running broadcast worker sends INFO_REQUESTs that the
     * new server queues ahead of the host's own version-pre-flight
     * discoveryPingServer, racing it past the 5-second recvfrom
     * timeout. resetPings() supersedes anything already popped by a
     * pool worker so its eventual result is dropped; the workers
     * themselves block on their own ephemeral-port sockets and don't
     * touch port 27500, so they don't need to be joined. */
    discoveryAbortBroadcastSearch();
    discoveryAbortMdnsSearch();
    resetPings();
    if (searchThread.joinable()) {
        searchThread.join();
    }
    if (mdnsThread.joinable()) {
        mdnsThread.join();
    }

    /* Destroy refresh icon texture */
    if (s_refreshIcon) { SDL_DestroyTexture(s_refreshIcon); s_refreshIcon = nullptr; }
    s_refreshIconAttempted = false;

    /* Tear down ImGui */
    dialogDismissKeyboard(window);
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
