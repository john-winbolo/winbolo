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
#include "imgui_controller_prompt.h"
#include "dialog_footer.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "../imgui_steam_nav.h"
#include "../glyphs.h"   /* glyphForActionAuto — controller A/B glyphs */

extern "C" {
#include "../../../steam/steam_input_actions.h"  /* SI_ACTION_MENU_* names */
#include "../sdl3draw.h"
#include "../bg_game.h"
#include "../flags.h"
#include "../sdl3imgui.h"
#include "../../ui_mode.h"
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
#include "imgui_keyboard.h"
#include "../map_preview_view.h"
#include "../map_preview_popup.h"
#include "../../../bolo/public/client_mappreview.h"
}

#include "../map_preview_fetch.h"

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
    int  maxPlayers;     /* server's join-slot cap; 0 when unknown (Internet) */
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

    /* Lobby status (derived from inLobby) */
    int lobbyStatus;     /* 1=in lobby, 0=in game (derived from inLobby) */

    /* From WinBolo.net game list (Internet path) */
    char serverKey[64];
    int  numHumans;
    int  numBots;
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
    bool hasRichInfo;    /* false for a legacy 76-byte server that can't report
                          * the flags/counts/md5 fields; gates the rich-only
                          * lines in the detail pane. */
    /* Server visibility rules. An advertisement that doesn't carry them
     * predates them, so it reads as the behaviour of the day: pill
     * always, base off, ally always, classic mode and allies in trees
     * off, the expanded overview window with nothing blocking sight.
     * That back-compatibility reading is not what an unconfigured
     * server runs today - see viewPolicyTag below. */
    ViewPolicy pillView;
    ViewPolicy baseView;
    ViewPolicy allyView;
    bool classicMode;
    bool alliesInTrees;
    uint8_t overviewWindow;
    uint8_t lineOfSight;
    /* Voice the server forwards. Unlike the fields above this one has a
     * true answer for a server that says nothing: both wires define an
     * absent value as serverVoiceOn. */
    ServerVoiceMode voiceMode;
    std::vector<std::string> players;   /* logged-in usernames, blanks already filtered */
};

/* Compact "Views:" tag for the detail pane. Lists only the categories
 * that differ from the defaults, so a stock server shows nothing at
 * all. Classic mode leads the list because it explains the policies
 * that follow it. Returns "" when the server is stock.
 *
 * The defaults compared against here are the rules a current
 * unconfigured server runs: pill Key, base Off, ally Off, the classic
 * overview window and sight off. That is deliberately not the same set
 * as the back-compatibility reading an advertisement missing the rules
 * gets (see ServerEntry above). A server old enough to leave them out
 * really does play differently from a stock one, so it gets tagged. */
static std::string viewPolicyTag(const ServerEntry &e) {
    /* Same four policy words the lobby, the hosting tab and the game info
     * panel use, so one server's rules read the same wherever they show. */
    const char *kModeStr[] = {
        langGetText(STR_DLGLOBBY_VIEW_ALWAYS),
        langGetText(STR_DLGLOBBY_VIEW_KEY),
        langGetText(STR_DLGLOBBY_VIEW_DECAY),
        langGetText(STR_DLGLOBBY_VIEW_OFF),
    };
    struct { const char *letter; ViewPolicy value; ViewPolicy def; } cats[] = {
        { "P", e.pillView, viewPolicyKey },
        { "B", e.baseView, viewPolicyOff },
        { "A", e.allyView, viewPolicyOff },
    };
    std::string out;
    if (e.classicMode) {
        out += langGetText(STR_DLGBROWSER_VIEWS_CLASSIC);
    }
    if (e.alliesInTrees) {
        if (!out.empty()) out += " ";
        out += langGetText(STR_DLGBROWSER_VIEWS_ALLYTREES);
    }
    /* The overview window and line of sight are server-wide rules like
     * the two above rather than per-category, so they sit with them.
     * Only the non-default value is worth a tag: an unconfigured server
     * runs the classic window with sight off. The window is prefixed to
     * match the P=/B=/A= form the categories below use, since a bare
     * "Expanded" in a list of tags names no setting in particular. */
    if (e.overviewWindow == (uint8_t)overviewWindowExpanded) {
        if (!out.empty()) out += " ";
        out += "W=";
        out += langGetText(STR_DLGLOBBY_WINDOW_EXPANDED);
    }
    if (e.lineOfSight != (uint8_t)lineOfSightOff) {
        if (!out.empty()) out += " ";
        out += langGetText(STR_DLGLOBBY_LINE_OF_SIGHT_CB);
    }
    for (const auto &c : cats) {
        if (c.value == c.def) continue;
        int idx = (int)c.value;
        if (idx < 0 || idx > 3) continue;
        if (!out.empty()) out += " ";
        out += c.letter;
        out += "=";
        out += kModeStr[idx];
    }
    if (out.empty()) return out;
    return std::string(langGetText(STR_DLGBROWSER_VIEWS_LBL)) + " " + out;
}

static const char *gameTypeStr(gameType g) {
    switch (g) {
    case gameOpen:           return langGetText(STR_DLGGAMEINFO_OPEN);
    case gameTournament:     return langGetText(STR_DLGGAMEINFO_TOURN);
    case gameStrictTournament:
    default:                 return langGetText(STR_DLGGAMESETUP_STRICT_SHORT);
    }
}

/* Compact form for the space-constrained list row. */
static const char *gameTypeAbbr(gameType g) {
    switch (g) {
    case gameOpen:           return langGetText(STR_DLGGAMEINFO_OPEN);
    case gameTournament:     return langGetText(STR_DLGBROWSER_TYPE_TOURN_ABBR);
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
    int  maxPlayers;
    char hostName[256];
    char mapMd5[33];
    bool allowNewPlayers;
    bool inLobby;
    bool allowSpectators;
    BYTE spectatorCount;
    bool ranked;
    bool randomMap;
    BYTE numHumans;
    BYTE numBots;
    int32_t timeLimit;
    bool hasRichInfo;
    ViewPolicy pillView;
    ViewPolicy baseView;
    ViewPolicy allyView;
    bool classicMode;
    bool alliesInTrees;
    uint8_t overviewWindow;
    uint8_t lineOfSight;
    ServerVoiceMode voiceMode;
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
    res.maxPlayers = 0;
    res.hostName[0] = '\0';
    res.mapMd5[0] = '\0';
    res.allowNewPlayers = false;
    res.inLobby = false;
    res.allowSpectators = false;
    res.spectatorCount = 0;
    res.ranked = false;
    res.randomMap = false;
    res.numHumans = 0;
    res.numBots = 0;
    res.timeLimit = 0;
    res.hasRichInfo = false;
    res.pillView = viewPolicyAlways;
    res.baseView = viewPolicyOff;
    res.allyView = viewPolicyAlways;
    res.classicMode = false;
    res.alliesInTrees = false;
    res.overviewWindow = (uint8_t)overviewWindowExpanded;
    res.lineOfSight = (uint8_t)lineOfSightOff;
    res.voiceMode = serverVoiceOn;

    /* Reverse-DNS the address regardless of whether the UDP info-ping
     * answers, so even unresponsive servers get a hostname. */
    resolveHostName(work.address, res.hostName, sizeof(res.hostName));

    DiscoveryPingResult dpr;
    if (discoveryPingServer(work.address, work.port, &dpr)) {
        res.pingMs    = dpr.rttMs;
        res.freePills = dpr.freePills;
        res.freeBases = dpr.freeBases;
        res.numPlayers = dpr.numPlayers;
        res.maxPlayers = dpr.maxPlayers;
        res.numHumans       = dpr.numHumans;
        res.numBots         = dpr.numBots;
        res.ranked          = dpr.ranked;
        res.inLobby         = dpr.inLobby;
        res.allowNewPlayers = dpr.allowNewPlayers;
        res.allowSpectators = dpr.allowSpectators;
        res.spectatorCount  = dpr.spectatorCount;
        res.randomMap       = dpr.randomMap;
        res.timeLimit       = dpr.timeLimit;
        res.hasRichInfo     = dpr.hasRichInfo;
        res.pillView        = dpr.pillView;
        res.baseView        = dpr.baseView;
        res.allyView        = dpr.allyView;
        res.classicMode     = dpr.classicMode;
        res.alliesInTrees   = dpr.alliesInTrees;
        res.overviewWindow  = dpr.overviewWindow;
        res.lineOfSight     = dpr.lineOfSight;
        res.voiceMode       = dpr.voiceMode;
        SDL_strlcpy(res.mapMd5, dpr.mapMd5, sizeof(res.mapMd5));
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
struct PingJob { PingWork work; uint64_t gen; };

/* All mutable ping-pool state lives in one heap instance reached via
 * pingPool(). The detached workers outlive normal static destruction, so the
 * cv/mutex/containers must never have their destructors run at process exit. */
struct PingPool {
    std::mutex                  resultsMtx;
    std::vector<PingResult>     results;
    std::mutex                  queueMtx;
    std::condition_variable     queueCv;
    std::deque<PingJob>         queue;
};

static PingPool &pingPool() {
    static PingPool *p = new PingPool();  /* intentionally leaked: detached
        ping workers outlive normal static destruction, so never run these
        destructors — avoids a hang at process exit. */
    return *p;
}

static std::atomic<uint64_t>    s_pingGeneration{0};
static std::once_flag           s_pingPoolOnce;

static void pingWorkerFn() {
    for (;;) {
        PingJob job;
        {
            std::unique_lock<std::mutex> lk(pingPool().queueMtx);
            pingPool().queueCv.wait(lk, [] { return !pingPool().queue.empty(); });
            job = pingPool().queue.front();
            pingPool().queue.pop_front();
        }
        /* Skip work already superseded by a newer search. */
        if (job.gen != s_pingGeneration.load()) continue;
        PingResult pr = pingServer(job.work);
        std::lock_guard<std::mutex> lk(pingPool().resultsMtx);
        if (job.gen == s_pingGeneration.load()) {
            pingPool().results.push_back(pr);
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
        std::lock_guard<std::mutex> lk(pingPool().queueMtx);
        pingPool().queue.push_back(PingJob{w, s_pingGeneration.load()});
    }
    pingPool().queueCv.notify_one();
}

/* Abandon pending/in-flight pings and clear stale results. Called on each
 * new search and on browser open so old results can't map onto a rebuilt
 * server list. */
static void resetPings() {
    s_pingGeneration.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(pingPool().queueMtx);
        pingPool().queue.clear();
    }
    std::lock_guard<std::mutex> lk(pingPool().resultsMtx);
    pingPool().results.clear();
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

    SDL_strlcpy(e.address, src->address, sizeof(e.address));
    e.port = src->port;
    SDL_strlcpy(e.mapName, src->mapName, sizeof(e.mapName));
    SDL_snprintf(e.version, sizeof(e.version), "%d.%d%d",
                 src->versionMajor, src->versionMinor, src->versionRevision);
    e.numPlayers = src->numPlayers;
    e.maxPlayers = src->maxPlayers;
    e.numBases   = src->numBases;
    e.numPills   = src->numPills;
    e.mines      = src->mines;
    e.game       = src->game;
    e.ai         = src->ai;
    e.password   = src->password;
    e.numHumans       = src->numHumans;
    e.numBots         = src->numBots;
    e.ranked          = src->ranked;
    e.inLobby         = src->inLobby;
    e.allowNewPlayers = src->allowNewPlayers;
    e.allowSpectators = src->allowSpectators;
    e.spectatorCount  = src->spectatorCount;
    e.randomMap       = src->randomMap;
    e.hasRichInfo     = src->hasRichInfo;
    e.pillView        = src->pillView;
    e.baseView        = src->baseView;
    e.allyView        = src->allyView;
    e.classicMode     = src->classicMode;
    e.alliesInTrees   = src->alliesInTrees;
    e.overviewWindow  = src->overviewWindow;
    e.lineOfSight     = src->lineOfSight;
    e.voiceMode       = src->voiceMode;
    SDL_strlcpy(e.mapMd5, src->mapMd5, sizeof(e.mapMd5));
    /* INFO/TXT time limit is game-length in 50ths-of-a-second ticks; convert
     * to minutes the same way the server does (ticks / (50 * 60)). */
    e.timeLimit   = (src->timeLimit != 0);
    e.timeMinutes = (int)(src->timeLimit / (50 * 60));
    e.lobbyStatus = src->inLobby ? 1 : 0;

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

/* ---- Lock icon (loaded from SVG, white so it can be tinted per state) ---- */
static SDL_Texture *s_lockIcon = nullptr;
static bool s_lockIconAttempted = false;

/* ---- Voice icons for the detail pane's voice row (white masks) ----
 * The plain speaker stands for both On and Proximity; there is no
 * distance or falloff art in data/ui/, so the word beside it carries
 * the difference. */
static SDL_Texture *s_voiceIcon = nullptr;
static SDL_Texture *s_voiceMutedIcon = nullptr;
static bool s_voiceIconsAttempted = false;

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
    char selKeyAddr[FILENAME_MAX] = "";   /* address of the selected server; "" = none */
    unsigned short selKeyPort = 0;        /* its port — together a stable identity across rebuilds */
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
    bool wantNeedNamePopup = false;

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

    /* Map preview for the detail pane. One view for the whole browser
     * session; the loaded map is swapped when the selected md5 changes.
     * NULL view => preview treated as unavailable (all uses guarded). */
    MapPreviewView *previewView = mapPreviewViewCreate();
    char loadedPreviewMd5[33] = "";
    bool loadedPreviewOk = false;
    /* Compressed bytes of the currently-loaded preview map, retained so a
     * click on the thumbnail can hand them to the zoomable popup without
     * re-fetching. Refreshed whenever loadedPreviewMd5 changes. */
    std::vector<uint8_t> loadedPreviewComp;
    /* True only on frames where the current selection's preview texture is
     * loaded and current, so the footer Enlarge button isn't offered (or
     * activated) for a stale or missing preview. */
    bool previewEnlargeReady = false;

    /* One-shot: seed controller focus onto the server list the first frame it
     * has rows. Cleared once the seed fires; the row loop doesn't run while the
     * (async-populated) list is empty, so the seed naturally defers until then. */
    bool seedListFocus = true;

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
                        e.hasRichInfo = true;   /* WinBolo.net JSON always carries the rich fields */
                        e.pillView = (ViewPolicy)w.pillView;
                        e.baseView = (ViewPolicy)w.baseView;
                        e.allyView = (ViewPolicy)w.allyView;
                        e.classicMode = w.classicMode;
                        e.alliesInTrees = w.alliesInTrees;
                        e.overviewWindow = (uint8_t)w.overviewWindow;
                        e.lineOfSight = (uint8_t)w.lineOfSight;
                        e.voiceMode = (ServerVoiceMode)w.voiceMode;

                        e.players.clear();
                        for (int p = 0; p < w.numPlayerNames; p++) {
                            e.players.emplace_back(w.players[p]);
                        }

                        e.pingMs = -1; /* pending — ping still fires below */
                        e.lobbyStatus = w.inLobby ? 1 : 0;

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
            std::lock_guard<std::mutex> lock(pingPool().resultsMtx);
            for (auto &pr : pingPool().results) {
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
                        servers[pr.index].maxPlayers      = pr.maxPlayers;
                        servers[pr.index].numHumans       = pr.numHumans;
                        servers[pr.index].numBots         = pr.numBots;
                        servers[pr.index].ranked          = pr.ranked;
                        servers[pr.index].inLobby         = pr.inLobby;
                        servers[pr.index].allowNewPlayers = pr.allowNewPlayers;
                        servers[pr.index].allowSpectators = pr.allowSpectators;
                        servers[pr.index].spectatorCount  = pr.spectatorCount;
                        servers[pr.index].randomMap       = pr.randomMap;
                        servers[pr.index].timeLimit       = (pr.timeLimit != 0);
                        servers[pr.index].timeMinutes     = (int)(pr.timeLimit / (50 * 60));
                        servers[pr.index].hasRichInfo     = pr.hasRichInfo;
                        servers[pr.index].pillView        = pr.pillView;
                        servers[pr.index].baseView        = pr.baseView;
                        servers[pr.index].allyView        = pr.allyView;
                        servers[pr.index].classicMode     = pr.classicMode;
                        servers[pr.index].alliesInTrees   = pr.alliesInTrees;
                        servers[pr.index].overviewWindow  = pr.overviewWindow;
                        servers[pr.index].lineOfSight     = pr.lineOfSight;
                        servers[pr.index].voiceMode       = pr.voiceMode;
                        servers[pr.index].lobbyStatus     = pr.inLobby ? 1 : 0;
                        SDL_strlcpy(servers[pr.index].mapMd5, pr.mapMd5, sizeof(servers[pr.index].mapMd5));
                    }
                }
            }
            pingPool().results.clear();
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

        /* Render the zoom popup's tiles to its offscreen texture before the
         * ImGui frame, matching the lobby/map-chooser wiring. */
        mapPreviewPopupRenderOffscreen(renderer, winW, winH);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();
        controllerDialogsRenderMenu();

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

            /* Load lock icon on first use (white mask so it can be tinted per state) */
            if (!s_lockIcon && !s_lockIconAttempted) {
                s_lockIconAttempted = true;
                int iconSize = (int)(24.0f * s);
                if (iconSize < 16) iconSize = 16;

                /* Try several path candidates */
                const char *candidates[] = {
                    "data/ui/lock.svg",
                    NULL /* filled in below with basePath variant */
                };
                char basePathBuf[FILENAME_MAX] = {};
                const char *base = SDL_GetBasePath();
                if (base) {
                    SDL_snprintf(basePathBuf, sizeof(basePathBuf), "%sdata/ui/lock.svg", base);
                    candidates[1] = basePathBuf;
                }
                for (int i = 0; i < 2 && !s_lockIcon; i++) {
                    if (candidates[i]) {
                        WB_LOG_DEBUG(WB_LOG_CAT_ASSET, "[GameBrowser] Trying lock icon: %s", candidates[i]);
                        s_lockIcon = imguiLoadSvgIconWhite(renderer, candidates[i], iconSize);
                    }
                }
                WB_LOG_DEBUG(WB_LOG_CAT_ASSET, "[GameBrowser] Lock icon loaded: %s", s_lockIcon ? "yes" : "no");
            }

            /* Load the two voice icons on first use, same candidate-path
             * shape as the lock icon. Either can come back null; the voice
             * row then draws the word on its own. */
            if (!s_voiceIconsAttempted) {
                s_voiceIconsAttempted = true;
                int iconSize = (int)(24.0f * s);
                if (iconSize < 16) iconSize = 16;

                const char *names[2] = { "speaker.svg", "speaker-muted.svg" };
                SDL_Texture **dests[2] = { &s_voiceIcon, &s_voiceMutedIcon };
                const char *base = SDL_GetBasePath();
                for (int n = 0; n < 2; n++) {
                    char relPath[FILENAME_MAX];
                    char basePathBuf[FILENAME_MAX] = {};
                    SDL_snprintf(relPath, sizeof(relPath), "data/ui/%s", names[n]);
                    const char *candidates[2] = { relPath, NULL };
                    if (base) {
                        SDL_snprintf(basePathBuf, sizeof(basePathBuf), "%sdata/ui/%s",
                                     base, names[n]);
                        candidates[1] = basePathBuf;
                    }
                    for (int i = 0; i < 2 && !*dests[n]; i++) {
                        if (candidates[i]) {
                            WB_LOG_DEBUG(WB_LOG_CAT_ASSET, "[GameBrowser] Trying voice icon: %s", candidates[i]);
                            *dests[n] = imguiLoadSvgIconWhite(renderer, candidates[i], iconSize);
                        }
                    }
                }
                WB_LOG_DEBUG(WB_LOG_CAT_ASSET, "[GameBrowser] Voice icons loaded: %s / %s",
                             s_voiceIcon ? "yes" : "no",
                             s_voiceMutedIcon ? "yes" : "no");
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

        /* ---- Server list + detail pane ---- */
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

        /* Status-dot colours, shared by the legend and the list rows. */
        const ImVec4 dotGreen (0.40f, 0.80f, 0.40f, 1.0f);  /* in lobby */
        const ImVec4 dotOrange(1.00f, 0.60f, 0.20f, 1.0f);  /* in game, accepting */
        const ImVec4 dotRed   (1.00f, 0.40f, 0.40f, 1.0f);  /* locked or full */
        const ImVec4 dotGrey  (0.60f, 0.60f, 0.60f, 1.0f);  /* no response */
        auto dotColorFor = [&](const ServerEntry &se, ImVec4 &colOut, bool &filledOut) {
            if (se.pingMs == -2) { colOut = dotGrey; filledOut = false; return; }
            filledOut = true;
            if (!se.allowNewPlayers ||
                (se.maxPlayers > 0 && (int)se.numPlayers >= se.maxPlayers)) {
                colOut = dotRed;
            } else if (se.inLobby) {
                colOut = dotGreen;
            } else {
                colOut = dotOrange;
            }
        };

        float listH = ImGui::GetContentRegionAvail().y - bottomH;
        if (listH < 120.0f) listH = 120.0f;

        float availW = ImGui::GetContentRegionAvail().x;
        float leftW = availW * 0.42f;
        if (leftW < 240.0f * s) leftW = 240.0f * s;

        /* ---- Left: compact server list ---- */
        ImGui::BeginChild("##ServerListPane", ImVec2(leftW, listH),
                          ImGuiChildFlags_Borders);
        {
            std::lock_guard<std::mutex> lock(serversMtx);

            /* Re-attach the selection to its server by identity, so a list
             * rebuild (refresh / auto-poll / async LAN repopulation) keeps the
             * same row highlighted. -1 when the server is no longer present. */
            if (selKeyAddr[0] != '\0') {
                selectedItem = -1;
                for (int si = 0; si < (int)servers.size(); si++) {
                    if (servers[si].port == selKeyPort &&
                        strcmp(servers[si].address, selKeyAddr) == 0) {
                        selectedItem = si;
                        break;
                    }
                }
            }

            /* Visible set: apply the three filters, then sort alphabetically
             * (case-insensitive) by the displayed name — the reverse-DNS host
             * name once resolved, else the tracker address — with port as the
             * tiebreak. A name key keeps the order steady across the ~20s
             * auto-refresh and as async pings land; only a server's one-time
             * DNS resolution can shift its row. The underlying servers vector
             * keeps its arrival order so async ping results still land on the
             * right index. */
            std::vector<int> visible;
            visible.reserve(servers.size());
            for (int i = 0; i < (int)servers.size(); i++) {
                const ServerEntry &fe = servers[i];
                if (filterGameType >= 0 && (int)fe.game != filterGameType) continue;
                if (filterLocked && fe.password) continue;
                if (filterLobby >= 0 && fe.lobbyStatus != filterLobby) continue;
                visible.push_back(i);
            }
            auto nameKey = [&](int idx) -> const char * {
                const ServerEntry &e = servers[idx];
                return e.hostName[0] != '\0' ? e.hostName : e.address;
            };
            std::stable_sort(visible.begin(), visible.end(),
                             [&](int a, int b) {
                                 int c = SDL_strcasecmp(nameKey(a), nameKey(b));
                                 if (c != 0) return c < 0;
                                 return servers[a].port < servers[b].port;
                             });

            /* Row to seed controller focus onto: the selected server if it is
             * visible, otherwise the first visible row. */
            int seedTarget = -1;
            if (!visible.empty()) {
                seedTarget = visible.front();
                if (selectedItem >= 0) {
                    for (int vi : visible) {
                        if (vi == selectedItem) { seedTarget = selectedItem; break; }
                    }
                }
            }

            for (int vi : visible) {
                int i = vi;
                const ServerEntry &e = servers[i];

                ImGui::PushID(i);

                float pad   = 6.0f * s;
                float lineH = ImGui::GetTextLineHeight();
                float rowH  = lineH * 2.0f + pad * 2.0f;
                float rowW  = ImGui::GetContentRegionAvail().x;

                ImVec2 p0 = ImGui::GetCursorScreenPos();
                bool isSelected = (selectedItem == i);
                ImGui::SetNextItemAllowOverlap();
                if (seedListFocus && i == seedTarget && uiShouldUseControllerMode()) {
                    ImGui::SetKeyboardFocusHere();
                    seedListFocus = false;
                }
                if (ImGui::Selectable("##srv", isSelected,
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(rowW, rowH))) {
                    /* Join on a mouse double-click, or — in controller mode,
                     * where the row activates via keyboard Space and never a
                     * mouse double-click — on a second A press on the row that
                     * was already selected entering this frame (isSelected is
                     * captured before the assignment below). */
                    bool joinActivate = ImGui::IsMouseDoubleClicked(0) ||
                                        (uiShouldUseControllerMode() && isSelected);
                    selectedItem = i;
                    SDL_strlcpy(selKeyAddr, e.address, sizeof(selKeyAddr));
                    selKeyPort = e.port;
                    if (joinActivate) {
                        char playerName[PLAYER_NAME_LEN];
                        gameFrontGetPlayerName(playerName);
                        if (strlen(playerName) == 0) {
                            errorMsg = langGetText(STR_DLGBROWSER_ERR_NEEDNAME);
                            wantNeedNamePopup = true;
                        } else {
                            gameFrontSetUdpOptions(playerName, (char *)e.address, e.port, 0);
                            gameFrontSetAIType(e.ai);
                            gameFrontSetDlgState(openUdpJoin);
                            result = (int)openUdpJoin;
                            running = false;
                        }
                    }
                }
                imguiHandOnHover();
                ImVec2 pEnd = ImGui::GetCursorScreenPos();

                ImDrawList *dl = ImGui::GetWindowDrawList();
                float contentX  = p0.x + pad;
                float dotW      = 16.0f * s;
                float flagW     = 26.0f * s;
                float textX     = contentX + dotW + flagW;
                float textRight = p0.x + rowW - pad;

                /* Status dot */
                {
                    ImVec4 dcol; bool dfilled;
                    dotColorFor(e, dcol, dfilled);
                    float r = 5.0f * s;
                    ImVec2 c(contentX + r, p0.y + pad + lineH * 0.5f);
                    if (dfilled) dl->AddCircleFilled(c, r, ImGui::GetColorU32(dcol));
                    else         dl->AddCircle(c, r, ImGui::GetColorU32(dcol), 0, 1.5f);
                }

                /* Country flag (overlay item) or a 2-letter fallback */
                bool flagDrawn = false;
                if (e.countryCode[0] != '\0' && e.countryCode[0] != 'X') {
                    ImGui::SetCursorScreenPos(ImVec2(contentX + dotW, p0.y + pad));
                    flagDrawn = drawCountryFlagWithTip(e.countryCode);
                }
                if (!flagDrawn && e.countryCode[0] != '\0') {
                    char cc[3] = { e.countryCode[0], e.countryCode[1], '\0' };
                    dl->AddText(ImVec2(contentX + dotW, p0.y + pad),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), cc);
                }

                /* Ping string + colour FIRST, so the name can be clamped to
                 * stop before it (a long reverse-DNS otherwise runs under the
                 * right-aligned ping). */
                char pingStr[24];
                ImVec4 pcol;
                if (e.pingMs >= 0) {
                    SDL_snprintf(pingStr, sizeof(pingStr), "%dms", e.pingMs);
                    if (e.pingMs < 50)       pcol = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
                    else if (e.pingMs < 150) pcol = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
                    else                     pcol = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                } else {
                    SDL_snprintf(pingStr, sizeof(pingStr), "%s",
                                 e.pingMs == -1 ? "..." : "--");
                    pcol = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                }
                float pingLeft = textRight - ImGui::CalcTextSize(pingStr).x;

                /* Name line — host name if known, else address:port. Clamp its
                 * width to (pingLeft − gap) so a long reverse-DNS can't overlap
                 * the ping; binary-search the longest prefix that fits + "...". */
                char name[288];
                if (e.hostName[0] != '\0') {
                    SDL_snprintf(name, sizeof(name), "%s", e.hostName);
                } else {
                    SDL_snprintf(name, sizeof(name), "%s:%u", e.address, e.port);
                }
                float nameMaxW = pingLeft - (8.0f * s) - textX;
                if (nameMaxW > 0.0f && ImGui::CalcTextSize(name).x > nameMaxW) {
                    const char *kEll = "...";
                    float budget = nameMaxW - ImGui::CalcTextSize(kEll).x;
                    if (budget <= 0.0f) {
                        name[0] = '\0';
                    } else {
                        int lo = 0, hi = (int)SDL_strlen(name), best = 0;
                        while (lo <= hi) {
                            int mid = (lo + hi) / 2;
                            char saved = name[mid]; name[mid] = '\0';
                            float w = ImGui::CalcTextSize(name).x;
                            name[mid] = saved;
                            if (w <= budget) { best = mid; lo = mid + 1; }
                            else             { hi = mid - 1; }
                        }
                        name[best] = '\0';
                        SDL_strlcat(name, kEll, sizeof(name));
                    }
                }
                dl->AddText(ImVec2(textX, p0.y + pad),
                            ImGui::GetColorU32(ImGuiCol_Text), name);

                /* Map line with ranked (*) / random (rnd) markers */
                char mapLine[MAP_STR_SIZE + 32];
                char rndMark[24] = "";
                if (e.randomMap)
                    SDL_snprintf(rndMark, sizeof(rndMark), " (%s)",
                                 langGetText(STR_DLGBROWSER_RND_ABBR));
                SDL_snprintf(mapLine, sizeof(mapLine), "%s%s%s", e.mapName,
                             e.ranked ? " *" : "", rndMark);
                dl->AddText(ImVec2(textX, p0.y + pad + lineH),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), mapLine);

                /* Ping, right-aligned on the first line (string/colour/pingLeft
                 * were computed above so the name could be clamped not to
                 * overlap it). */
                dl->AddText(ImVec2(pingLeft, p0.y + pad),
                            ImGui::GetColorU32(pcol), pingStr);

                /* Players X/cap in the left gutter, under the flag */
                {
                    int cap = e.maxPlayers > 0 ? e.maxPlayers : MAX_TANKS;
                    char pc[24];
                    SDL_snprintf(pc, sizeof(pc), "%d/%d", (int)e.numPlayers, cap);
                    dl->AddText(ImVec2(contentX, p0.y + pad + lineH),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), pc);
                }

                /* Game-type abbreviation, right-aligned under the ping */
                {
                    const char *gt = gameTypeAbbr(e.game);
                    ImVec2 gsz = ImGui::CalcTextSize(gt);
                    dl->AddText(ImVec2(textRight - gsz.x, p0.y + pad + lineH),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), gt);
                }

                ImGui::SetCursorScreenPos(pEnd);
                ImGui::PopID();
            }   /* for each visible server */
            /* The per-row flag image advances the layout cursor, which each row
             * rewinds to pEnd; anchor the final rewind with a zero-size item so
             * ImGui doesn't read it as a boundary-extending SetCursorPos. */
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }       /* serversMtx scope */
        ImGui::EndChild();

        ImGui::SameLine();

        /* Snapshot the selected entry under the lock so the detail pane can
         * render without holding serversMtx across the ImGui calls. */
        bool haveSel = false;
        ServerEntry sel;
        {
            std::lock_guard<std::mutex> lock(serversMtx);
            if (selectedItem >= 0 && selectedItem < (int)servers.size()) {
                sel = servers[selectedItem];
                haveSel = true;
            }
        }
        previewEnlargeReady = false;

        /* ---- Right: selection detail pane ---- */
        /* Flattened into the parent nav plane so a controller reaches the
         * detail fields and the enlarge-preview button in one step. */
        ImGui::BeginChild("##DetailPane", ImVec2(0, listH),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
        if (!haveSel) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", langGetText(STR_DLGBROWSER_SELECT_SERVER));
        } else {
            /* Preview is a modest fixed thumbnail on the left; the key fields
             * sit to its right (added after EndChild below). Capped so the
             * field table beside it fits at the default window size. */
            float boxSize = listH * 0.5f;
            float maxBox  = 160.0f * s;
            if (boxSize > maxBox) boxSize = maxBox;
            if (boxSize < 80.0f)  boxSize = 80.0f;
            ImGui::BeginChild("##MapPreview", ImVec2(boxSize, boxSize),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
            {
                auto centeredDimmed = [](const char *txt) {
                    ImVec2 ts = ImGui::CalcTextSize(txt);
                    ImVec2 av = ImGui::GetContentRegionAvail();
                    ImGui::SetCursorPos(ImVec2((av.x - ts.x) * 0.5f,
                                               (av.y - ts.y) * 0.5f));
                    ImGui::TextDisabled("%s", txt);
                };

                if (sel.mapMd5[0] == '\0') {
                    /* Random/unknown map — nothing to fetch. */
                    centeredDimmed(langGetText(STR_DLGBROWSER_PREVIEW_UNAVAIL));
                } else {
                    mapPreviewFetchRequest(sel.mapMd5);
                    const uint8_t *bytes = nullptr;
                    size_t len = 0;
                    MapPreviewFetchState st =
                        mapPreviewFetchTryGet(sel.mapMd5, &bytes, &len);

                    if (st == MapPreviewFetchState::Pending) {
                        centeredDimmed(langGetText(STR_DLGNEWS_LOADING));
                    } else if (st == MapPreviewFetchState::Unavailable) {
                        centeredDimmed(langGetText(STR_DLGBROWSER_PREVIEW_UNAVAIL));
                    } else {
                        /* Ready: (re)load only when the selected md5 changed,
                         * so the same map isn't reconverted every frame. */
                        if (previewView != nullptr &&
                            strcmp(loadedPreviewMd5, sel.mapMd5) != 0) {
                            int compLen = 0;
                            BYTE *comp = clientMapConvertFileToCompressed(
                                bytes, (int)len, &compLen);
                            loadedPreviewOk =
                                (comp != nullptr) &&
                                mapPreviewViewLoadCompressed(previewView, comp, compLen);
                            /* Keep the compressed bytes for the click-to-zoom
                             * popup, then free the raw malloc. */
                            if (comp != nullptr && loadedPreviewOk && compLen > 0)
                                loadedPreviewComp.assign(comp, comp + compLen);
                            else
                                loadedPreviewComp.clear();
                            if (comp) free(comp);
                            /* Record the md5 even on conversion failure so a
                             * bad map isn't reconverted every frame. */
                            SDL_strlcpy(loadedPreviewMd5, sel.mapMd5,
                                        sizeof(loadedPreviewMd5));
                        }

                        /* Drive the deferred build every frame a map is loaded:
                         * the widget only becomes ready as a result of a
                         * RenderOffscreen call, so gating the call on IsReady
                         * would never let it start. Display once the texture
                         * exists. */
                        if (previewView != nullptr && loadedPreviewOk &&
                            strcmp(loadedPreviewMd5, sel.mapMd5) == 0) {
                            /* Reserve a line for the caption so the static
                             * thumbnail fills the box without a scrollbar. */
                            ImVec2 av = ImGui::GetContentRegionAvail();
                            int boxW = (int)av.x;
                            int boxH = (int)(av.y - ImGui::GetTextLineHeightWithSpacing());
                            if (boxW < 1) boxW = 1;
                            if (boxH < 1) boxH = 1;
                            mapPreviewViewRenderOffscreen(previewView, renderer,
                                                          boxW, boxH);
                            SDL_Texture *tex = mapPreviewViewGetTexture(previewView);
                            if (tex != nullptr) {
                                previewEnlargeReady = !loadedPreviewComp.empty();
                                ImGui::Image((ImTextureID)tex,
                                             ImVec2((float)boxW, (float)boxH));
                                /* Click the thumbnail to open the zoomable
                                 * read-only popup. The popup self-frames from
                                 * the map data on its first offscreen frame,
                                 * so full-map bounds are fine here. */
                                if (ImGui::IsItemHovered())
                                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                                if (ImGui::IsItemClicked() && loadedPreviewOk &&
                                    !loadedPreviewComp.empty()) {
                                    mapPreviewPopupOpenCompressed(
                                        loadedPreviewComp.data(),
                                        (int)loadedPreviewComp.size(),
                                        0, 0, 255, 255);
                                }
                                if (!uiShouldUseControllerMode())
                                    ImGui::TextDisabled("%s", langGetText(STR_DLGBROWSER_PREVIEW_ENLARGE));
                            } else {
                                /* Build not finished this frame — try again next frame. */
                                centeredDimmed(langGetText(STR_DLGNEWS_LOADING));
                            }
                        } else {
                            centeredDimmed(langGetText(STR_DLGBROWSER_PREVIEW_UNAVAIL));
                        }
                    }
                }
            }
            ImGui::EndChild();

            /* Key fields sit in a group to the right of the preview box. */
            ImGui::SameLine();
            ImGui::BeginGroup();
            {
                /* Label cells carry the theme accent; values stay normal text.
                 * Columns separate label from value, so the label text drops the
                 * trailing colon. */
                const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                auto label = [&](const char *t) { ImGui::TextColored(accent, "%s", t); };

                if (ImGui::BeginTable("##gbdetail", 2, ImGuiTableFlags_SizingFixedFit)) {
                    /* Type */
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); label(langGetText(STR_DLGBROWSER_COL_TYPE));
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(gameTypeStr(sel.game));

                    /* Version */
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); label(langGetText(STR_DLGBROWSER_COL_VER));
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(sel.version);

                    /* Hidden mines */
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); label(langGetText(STR_DLGGAMESETUP_HIDDENMINES_SHORT));
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(langGetText(sel.mines ? STR_YES : STR_NO));

                    /* AI */
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); label(langGetText(STR_DLGGAMEINFO_AILABEL));
                    {
                        int aiStr = STR_NO;
                        if      (sel.ai == aiYes)          aiStr = STR_YES;
                        else if (sel.ai == aiYesAdvantage) aiStr = STR_DLGGAMEINFO_AIADV;
                        else if (sel.ai == aiFull)         aiStr = STR_DLGGAMEINFO_FULLADV;
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(langGetText(aiStr));
                    }

                    /* Players — count / cap, AI-player split when rich.
                     * Plain "Players" label: STR_DLGGAMEINFO_NUMPLAYERS is a
                     * {number} format template, not a usable label. */
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); label(langGetText(STR_DLGPLAYERS_TITLE));
                    {
                        char pbuf[96];
                        int cap = sel.maxPlayers > 0 ? sel.maxPlayers : MAX_TANKS;
                        int n = SDL_snprintf(pbuf, sizeof(pbuf), "%d/%d", (int)sel.numPlayers, cap);
                        if (sel.hasRichInfo && sel.numBots > 0 &&
                            n > 0 && (size_t)n < sizeof(pbuf)) {
                            MessageArgs aiArgs = {};
                            aiArgs.number = sel.numBots;
                            SDL_snprintf(pbuf + n, sizeof(pbuf) - (size_t)n, " %s",
                                         langGetTextFmt(STR_DLGBROWSER_AI_PLAYERS, &aiArgs));
                        }
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(pbuf);
                    }

                    /* Bases — free/total when the total is known. */
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); label(langGetText(STR_DLGBROWSER_COL_BASES));
                    {
                        char vbuf[32];
                        if (sel.numBases > 0)
                            SDL_snprintf(vbuf, sizeof(vbuf), "%u/%u", sel.freeBases, sel.numBases);
                        else
                            SDL_snprintf(vbuf, sizeof(vbuf), "%u", sel.freeBases);
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(vbuf);
                    }

                    /* Pillboxes — free/total when the total is known. */
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); label(langGetText(STR_TABLET_PILLBOXES));
                    {
                        char vbuf[32];
                        if (sel.numPills > 0)
                            SDL_snprintf(vbuf, sizeof(vbuf), "%u/%u", sel.freePills, sel.numPills);
                        else
                            SDL_snprintf(vbuf, sizeof(vbuf), "%u", sel.freePills);
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(vbuf);
                    }

                    /* Time limit */
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); label(langGetText(STR_DLGGAMESETUP_TIMELIMIT_SHORT));
                    {
                        char tbuf[32];
                        const char *timeVal;
                        if (sel.timeLimit) {
                            SDL_snprintf(tbuf, sizeof(tbuf), "%d", sel.timeMinutes);
                            timeVal = tbuf;
                        } else {
                            timeVal = langGetText(STR_DLGGAMEINFO_UNLIMITED);
                        }
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(timeVal);
                    }

                    /* New players — rich info only. */
                    if (sel.hasRichInfo) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); label(langGetText(STR_ALLOW_NEW_PLAYERS));
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(langGetText(sel.allowNewPlayers ? STR_YES : STR_NO));
                    }

                    /* Voice — drawn for every server, not only rich-info ones
                     * like the row above. Both the INFO flag bits and the
                     * WinBolo.net JSON define an absent value as on, so On is
                     * this server's real answer rather than a stand-in.
                     * Proximity shares the plain speaker with On; the word
                     * carries the difference. */
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        label(langGetText(STR_DLGSETTINGS_HOSTING_VOICE));
                        ImGui::TableSetColumnIndex(1);
                        SDL_Texture *voiceTex = (sel.voiceMode == serverVoiceOff)
                                                    ? s_voiceMutedIcon : s_voiceIcon;
                        if (voiceTex) {
                            const float glyphH = ImGui::GetTextLineHeight();
                            ImGui::Image((ImTextureID)voiceTex, ImVec2(glyphH, glyphH));
                            ImGui::SameLine(0.0f, 4.0f);
                        }
                        langid voiceStr = STR_DLGSETTINGS_HOSTING_VOICE_ON;
                        if (sel.voiceMode == serverVoiceOff) {
                            voiceStr = STR_DLGSETTINGS_HOSTING_VOICE_OFF;
                        } else if (sel.voiceMode == serverVoiceProximity) {
                            voiceStr = STR_DLGSETTINGS_HOSTING_VOICE_PROXIMITY;
                        }
                        ImGui::TextUnformatted(langGetText(voiceStr));
                    }

                    ImGui::EndTable();
                }

                /* Active-only badges: password + rich flags, on one line. */
                {
                    std::string badges;
                    auto addBadge = [&](const char *t) {
                        if (!badges.empty()) badges += "  ·  ";
                        badges += t;
                    };
                    if (sel.password) addBadge(langGetText(STR_DLGBROWSER_LOCK_PASSWORD));
                    if (sel.hasRichInfo) {
                        if (sel.ranked)    addBadge(langGetText(STR_DLGLOBBY_RANKED));
                        if (sel.randomMap) addBadge(langGetText(STR_MAPCHOOSER_RANDOMMAP));
                        if (sel.autoLock)  addBadge(langGetText(STR_DLGBROWSER_AUTOLOCK_HINT));
                        std::string views = viewPolicyTag(sel);
                        if (!views.empty()) addBadge(views.c_str());
                    }
                    if (!badges.empty()) {
                        ImGui::TextDisabled("%s", badges.c_str());
                    }
                }
            }
            ImGui::EndGroup();
            ImGui::Spacing();

            /* Roster — WBN-only; LAN rows carry no player names. */
            ImGui::Separator();
            ImGui::TextDisabled("%s", langGetText(STR_DLGBROWSER_WBN_PLAYERS));
            if (sel.players.empty()) {
                ImGui::TextDisabled("—");
            } else {
                for (const auto &nm : sel.players) {
                    ImGui::TextUnformatted(nm.c_str());
                }
            }
        }
        ImGui::EndChild();

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

            /* Status-dot legend, trailing the status text on the same line. */
            auto legendDot = [&](const ImVec4 &col, bool filled, const char *txt) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                float lh = ImGui::GetTextLineHeight();
                float r = lh * 0.30f;
                ImVec2 c(p.x + r + 1.0f, p.y + lh * 0.5f);
                ImDrawList *ld = ImGui::GetWindowDrawList();
                if (filled) ld->AddCircleFilled(c, r, ImGui::GetColorU32(col));
                else        ld->AddCircle(c, r, ImGui::GetColorU32(col), 0, 1.5f);
                ImGui::Dummy(ImVec2(r * 2.0f + 4.0f, lh));
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextDisabled("%s", txt);
            };
            ImGui::SameLine(0.0f, 16.0f * s);
            legendDot(dotGreen,  true,  langGetText(STR_DLGBROWSER_ST_LOBBY));
            ImGui::SameLine(0.0f, 16.0f * s);
            legendDot(dotOrange, true,  langGetText(STR_DLGBROWSER_ST_INGAME));
            ImGui::SameLine(0.0f, 16.0f * s);
            legendDot(dotRed,    true,  langGetText(STR_DLGBROWSER_ST_LOCKED));
            ImGui::SameLine(0.0f, 16.0f * s);
            legendDot(dotGrey,   false, langGetText(STR_DLGBROWSER_ST_NORESP));
        }

        ImGui::Spacing();

        /* Modal popup IDs (built once per frame, used by both Open and Begin) */
        char errPopupId[64];
        SDL_snprintf(errPopupId, sizeof(errPopupId), "%s##gb", langGetText(STR_ERR_TITLE));
        if (wantNeedNamePopup)
            ImGui::OpenPopup(errPopupId);
        char setNamePopupId[64];
        SDL_snprintf(setNamePopupId, sizeof(setNamePopupId), "%s##gb", langGetText(STR_DLGSETPLAYERNAME_TITLE));

        /* ---- Button row ---- */
        float btnW = 110.0f * s;
        float btnH = 28.0f * s;
        {
            bool hasSelection = (selectedItem >= 0 && selectedItem < (int)servers.size());

            /* Controller mode draws the bound A/B glyphs inline, left of the
             * primary buttons (A = Join, B = Cancel). Glyph height matches the
             * button frame height so the row height is unchanged. */
            const bool  padLegend = uiShouldUseControllerMode();
            const float glyphH    = ImGui::GetFrameHeight();
            auto glyphInline = [&](const char *action) {
                if (!padLegend) return;
                SDL_Texture *g = glyphForActionAuto(action);
                if (g) {
                    ImGui::Image((ImTextureID)g, ImVec2(glyphH, glyphH));
                    ImGui::SameLine(0.0f, 4.0f);
                }
            };

            /* Join — A glyph before the disabled-state guard so it isn't dimmed. */
            glyphInline(SI_ACTION_MENU_ACCEPT);
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

#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
            /* Spectate — beside Join. Enabled only when the selected server
             * advertises spectator support. Unlike Join it needs no player
             * name: a spectator is tankless and read-only, so the name guard
             * is deliberately skipped. The spectator host (logviewer-side) is
             * desktop-only, so this is gated to match the openSpectate case. */
            ImGui::SameLine();
            const bool canSpectate =
                hasSelection && servers[selectedItem].allowSpectators;
            if (!canSpectate) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_DLGBROWSER_SPECTATE), ImVec2(btnW, btnH))) {
                const ServerEntry &e = servers[selectedItem];
                char playerName[PLAYER_NAME_LEN];
                gameFrontGetPlayerName(playerName);
                /* Stash the target the same way Join does; the openSpectate
                 * case reads it back to drive clientSimConnectUdp. */
                gameFrontSetUdpOptions(playerName, (char *)e.address, e.port, 0);
                gameFrontSetDlgState(openSpectate);
                result = (int)openSpectate;
                running = false;
            }
            imguiHandOnHover();
            if (!canSpectate) ImGui::EndDisabled();
#endif

            /* Controller-only: a pad can't reliably reach the preview-overlay
             * enlarge button across the detail pane, so expose enlarge as a
             * first-class footer action. Disabled until the selected server has
             * a loaded preview. */
            if (uiShouldUseControllerMode()) {
                ImGui::SameLine();
                if (!previewEnlargeReady) ImGui::BeginDisabled();
                if (ImGui::Button(langGetText(STR_DLGBROWSER_ENLARGE_BTN), ImVec2(btnW, btnH))
                    && !loadedPreviewComp.empty()) {
                    mapPreviewPopupOpenCompressed(loadedPreviewComp.data(),
                                                  (int)loadedPreviewComp.size(),
                                                  0, 0, 255, 255);
                }
                imguiHandOnHover();
                if (!previewEnlargeReady) ImGui::EndDisabled();
            }

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
                gameFrontSetUdpOptions(playerName, (char *)"",
                                       gameFrontHostingPort, gameFrontHostingPort);
                openingStates setupState = useTracker ? openInternetSetup : openLanSetup;
                gameFrontSetDlgState(setupState);
                result = (int)setupState;
                running = false;
            }
            imguiHandOnHover();
            if (useTracker) imguiHelpTooltip(langGetText(STR_DLGBROWSER_NEWGAME_PORTFWD_TIP));

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

            /* Cancel - right-aligned, muted-grey styling per dialog spec.
             * In controller mode a B glyph sits just left of it; shift the
             * right-align origin left by the glyph width so Cancel keeps its
             * exact position and the glyph fits beside it. */
            float cancelX = panelW - btnW - 16.0f * s;
            if (padLegend) cancelX -= glyphH + 4.0f;
            ImGui::SameLine(cancelX);
            glyphInline(SI_ACTION_MENU_CANCEL);
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

        /* Read-only zoom popup (no start-picker): drawn after the panel so it
         * sits on top, before ImGui::Render(). */
        mapPreviewPopupRenderModal(renderer);

        dialogDrawNavOutline();
        keyboardUpdate();   /* controller text entry for this dialog's fields */
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

    /* Close the shared zoom popup so it doesn't linger over the next
     * dialog (mirrors the map-chooser's exit cleanup). */
    mapPreviewPopupClose();
    mapPreviewViewDestroy(previewView);

    /* Destroy refresh icon texture */
    if (s_refreshIcon) { SDL_DestroyTexture(s_refreshIcon); s_refreshIcon = nullptr; }
    s_refreshIconAttempted = false;
    if (s_lockIcon) { SDL_DestroyTexture(s_lockIcon); s_lockIcon = nullptr; }
    s_lockIconAttempted = false;
    if (s_voiceIcon) { SDL_DestroyTexture(s_voiceIcon); s_voiceIcon = nullptr; }
    if (s_voiceMutedIcon) { SDL_DestroyTexture(s_voiceMutedIcon); s_voiceMutedIcon = nullptr; }
    s_voiceIconsAttempted = false;

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
